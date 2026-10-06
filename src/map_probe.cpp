#include "map_probe.h"
#include "drone_interact.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <Chimera_classes.hpp>
#include <ChimeraUI_classes.hpp>
#include <Engine_classes.hpp>
#include <UMG_classes.hpp>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace
{
    constexpr const char* kCommandName = "bd_mapprobe";

    // The first snapshot is logged once the map has laid out, then one every
    // kMinIntervalMs while it stays open, and none past kMaxSnapshots until it
    // is closed and opened again.
    constexpr uint64_t kMinIntervalMs = 2000;
    constexpr int      kMaxSnapshots  = 8;

    // The probe stops itself after this many map opens, so a forgotten
    // "bd_mapprobe on" cannot grow the log for a whole session.
    constexpr int kMaxOpens = 4;

    // Slate has not painted on the tick the map opens, so the first snapshot
    // waits for the map canvas to report a size, or this long.
    constexpr uint64_t kReadyTimeoutMs = 3000;

    // Widgets sampled per snapshot. The area's own markers number in the
    // dozens; a few well-spread ones are enough to fit a transform.
    constexpr int kMaxCoop     = 6;
    constexpr int kMaxPairs    = 8;
    constexpr int kMaxSegments = 3;
    constexpr int kMaxPlayers  = 8;
    constexpr int kMaxExtra    = 2; // personal and POI widgets sampled

    // Building markers considered as calibration pairs, and how many
    // replacements are tried for a pick that turns out hidden or collapsed.
    constexpr int kMaxCandidates = 4096;
    constexpr int kMaxPickTries  = 12;

    // Guard against reading a torn or stale TArray header as a huge count.
    constexpr int32_t kMaxSaneCount = 100000;

    bool g_commandRegistered = false;

    // Set by the console command, which runs on the game thread, and read by
    // the tick. Atomic all the same: nothing here depends on that ordering.
    std::atomic<bool> g_enabled{ false };

    // Per-open state. Only the tick touches these.
    bool     g_wasOpen   = false;
    int      g_snapshots = 0;
    int      g_opens     = 0;
    uint64_t g_lastMs    = 0;
    uint64_t g_openMs    = 0;

    // ---- plain-data samples ------------------------------------------------
    // Everything read through a game pointer is copied into one of these inside
    // an SEH-guarded function, then logged from a plain one. The layouts of the
    // map widgets are only known from the SDK dump, so a bad read must cost one
    // log line, not the game.

    // FNames of an object and its class, copied out under SEH so they can be
    // turned into text later without touching the object again.
    struct ObjNames
    {
        SDK::FName name;
        SDK::FName cls;
        bool       ok;
    };

    struct WidgetSample
    {
        bool   visible;
        bool   hasSlot;
        bool   slotIsCanvas;
        float  offsets[4];       // canvas slot LayoutData.Offsets: left, top, right, bottom
        double anchors[4];       // min x, min y, max x, max y
        double alignment[2];
        double rtTranslation[2]; // UWidget::RenderTransform
        double rtScale[2];
        float  rtAngle;
        double localSize[2];     // cached geometry
        double absSize[2];
        double topLeftPixel[2];  // widget local (0,0) in viewport pixels
        double topLeftViewport[2];
    };

    // Parameter names a player-colour material might expose. A guess: a name
    // the material lacks reads back as zero, which the log leaves out.
    constexpr const wchar_t* kParamNames[] = { L"Color", L"Colour", L"Tint", L"TintColor", L"BaseColor",
                                               L"MarkerColor", L"PlayerColor", L"IconColor" };
    constexpr int kParamCount = sizeof(kParamNames) / sizeof(kParamNames[0]);

    struct BrushSample
    {
        float      tint[4];     // Brush.TintColor.SpecifiedColor
        uint8_t    drawAs;      // ESlateBrushDrawType
        uint8_t    imageType;   // ESlateBrushImageType
        SDK::FName resourceName;
        bool       hasObject;   // Brush.ResourceObject: a texture, or a material the colour may come from
        ObjNames   object;
        bool       hasParent;   // material instance parent
        ObjNames   parent;
        bool       hasParams;   // object is a material instance; params were read
        float      params[kParamCount][4];
    };

    struct MarkerColours
    {
        float       userWidget[4]; // UUserWidget::ColorAndOpacity
        float       iconColor[4];  // ImageIcon->ColorAndOpacity
        bool        hasIcon;
        BrushSample brush;         // ImageIcon->Brush
        bool        mergedVisible; // MergedNumber: a cluster marker sits at the cluster centre
        char        mergedText[24];
    };

    struct PlayerEntry
    {
        char    name[128];
        float   location[2];
        float   color[4];
        float   rotationZ;
        uint8_t flags;
    };

    struct PairEntry
    {
        int32_t                   handleIndex;
        int32_t                   handleSerial;
        bool                      haveData;
        float                     location[2];
        uint8_t                   type;
        SDK::FName                uniqueName;
        SDK::UCrUW_MapMenuMarker* marker;
    };

    // Linear to the 8-bit sRGB the overlay's ImGui colour wants.
    int SrgbByte(float linear)
    {
        const float v = linear <= 0.0f ? 0.0f : (linear >= 1.0f ? 1.0f : linear);
        const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return static_cast<int>(s * 255.0f + 0.5f);
    }

    void FormatColour(const float* c, char* out, size_t size)
    {
        snprintf(out, size, "lin(%.3f %.3f %.3f %.3f) srgb #%02X%02X%02X", c[0], c[1], c[2], c[3],
            SrgbByte(c[0]), SrgbByte(c[1]), SrgbByte(c[2]));
    }

    std::string NameOf(const SDK::FName& name)
    {
        try { return name.ToString(); }
        catch (...) { return "?"; }
    }

    std::string NamesText(const ObjNames& n)
    {
        return n.ok ? NameOf(n.name) + " : " + NameOf(n.cls) : "?";
    }

    // Narrows a UTF-16 string to ASCII. Everything outside it becomes '?', so the
    // same input always gives the same text and two copies compare equal.
    void NarrowAscii(const wchar_t* chars, int32_t len, char* out, size_t size)
    {
        out[0] = '\0';
        if (!chars || len <= 0 || len >= 1024)
            return;

        size_t n = 0;
        for (; n + 1 < size && chars[n]; ++n)
            out[n] = chars[n] < 0x80 ? static_cast<char>(chars[n]) : '?';
        out[n] = '\0';
    }

    // ---- SEH readers (POD locals only) --------------------------------------

    bool ReadNames(SDK::UObject* obj, ObjNames* out)
    {
        __try
        {
            memset(out, 0, sizeof(*out));
            out->name = obj->Name;
            out->cls  = obj->Class->Name;
            out->ok   = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out->ok = false;
            return false;
        }
    }

    bool SampleWidget(SDK::UWidget* w, SDK::UObject* worldContext, WidgetSample* out)
    {
        __try
        {
            memset(out, 0, sizeof(*out));
            out->visible = w->IsVisible();

            out->rtTranslation[0] = w->RenderTransform.Translation.X;
            out->rtTranslation[1] = w->RenderTransform.Translation.Y;
            out->rtScale[0]       = w->RenderTransform.Scale.X;
            out->rtScale[1]       = w->RenderTransform.Scale.Y;
            out->rtAngle          = w->RenderTransform.Angle;

            SDK::UPanelSlot* slot = w->Slot;
            out->hasSlot = slot != nullptr;
            if (slot && slot->IsA(SDK::UCanvasPanelSlot::StaticClass()))
            {
                const SDK::FAnchorData& layout = static_cast<SDK::UCanvasPanelSlot*>(slot)->LayoutData;
                out->slotIsCanvas = true;
                out->offsets[0]   = layout.Offsets.Left;
                out->offsets[1]   = layout.Offsets.Top;
                out->offsets[2]   = layout.Offsets.Right;
                out->offsets[3]   = layout.Offsets.Bottom;
                out->anchors[0]   = layout.Anchors.Minimum.X;
                out->anchors[1]   = layout.Anchors.Minimum.Y;
                out->anchors[2]   = layout.Anchors.Maximum.X;
                out->anchors[3]   = layout.Anchors.Maximum.Y;
                out->alignment[0] = layout.Alignment.X;
                out->alignment[1] = layout.Alignment.Y;
            }

            const SDK::FGeometry geo = w->GetCachedGeometry();
            const SDK::FVector2D localSize = SDK::USlateBlueprintLibrary::GetLocalSize(geo);
            const SDK::FVector2D absSize   = SDK::USlateBlueprintLibrary::GetAbsoluteSize(geo);
            out->localSize[0] = localSize.X;
            out->localSize[1] = localSize.Y;
            out->absSize[0]   = absSize.X;
            out->absSize[1]   = absSize.Y;

            SDK::FVector2D origin;
            origin.X = 0.0;
            origin.Y = 0.0;
            SDK::FVector2D pixel;
            SDK::FVector2D viewport;
            SDK::USlateBlueprintLibrary::LocalToViewport(worldContext, geo, origin, &pixel, &viewport);
            out->topLeftPixel[0]    = pixel.X;
            out->topLeftPixel[1]    = pixel.Y;
            out->topLeftViewport[0] = viewport.X;
            out->topLeftViewport[1] = viewport.Y;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Reads the vector parameters a material instance carries under the names
    // in kParamNames and its parent. A template so each material class gets its
    // own call; the caller holds the SEH guard.
    template<class Material>
    void ReadMaterialParams(Material* m, BrushSample* out)
    {
        out->hasParams = true;
        for (int i = 0; i < kParamCount; ++i)
        {
            const SDK::FLinearColor c =
                m->K2_GetVectorParameterValue(SDK::BasicFilesImplUtils::StringToName(kParamNames[i]));
            out->params[i][0] = c.R;
            out->params[i][1] = c.G;
            out->params[i][2] = c.B;
            out->params[i][3] = c.A;
        }
        if (SDK::UMaterialInterface* parent = m->Parent)
            out->hasParent = ReadNames(parent, &out->parent);
    }

    // Everything a brush can colour an icon with: its tint, and the resource it
    // draws, which may be a material that takes the colour as a parameter.
    bool SampleBrush(const SDK::FSlateBrush& brush, BrushSample* out)
    {
        __try
        {
            memset(out, 0, sizeof(*out));
            const SDK::FLinearColor& tint = brush.TintColor.SpecifiedColor;
            out->tint[0]      = tint.R;
            out->tint[1]      = tint.G;
            out->tint[2]      = tint.B;
            out->tint[3]      = tint.A;
            out->drawAs       = static_cast<uint8_t>(brush.DrawAs);
            out->imageType    = static_cast<uint8_t>(brush.ImageType);
            out->resourceName = brush.ResourceName;

            SDK::UObject* res = brush.ResourceObject;
            if (!res)
                return true;

            out->hasObject = ReadNames(res, &out->object);
            if (res->IsA(SDK::UMaterialInstanceDynamic::StaticClass()))
                ReadMaterialParams(static_cast<SDK::UMaterialInstanceDynamic*>(res), out);
            else if (res->IsA(SDK::UMaterialInstanceConstant::StaticClass()))
                ReadMaterialParams(static_cast<SDK::UMaterialInstanceConstant*>(res), out);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SampleColours(SDK::UCrUW_MapMenuMarker* marker, MarkerColours* out)
    {
        __try
        {
            memset(out, 0, sizeof(*out));
            const SDK::FLinearColor& user = marker->ColorAndOpacity;
            out->userWidget[0] = user.R;
            out->userWidget[1] = user.G;
            out->userWidget[2] = user.B;
            out->userWidget[3] = user.A;

            SDK::UImage* icon = marker->ImageIcon;
            if (icon)
            {
                out->hasIcon = true;
                const SDK::FLinearColor& tint = icon->ColorAndOpacity;
                out->iconColor[0] = tint.R;
                out->iconColor[1] = tint.G;
                out->iconColor[2] = tint.B;
                out->iconColor[3] = tint.A;
                SampleBrush(icon->Brush, &out->brush);
            }

            if (SDK::UTextBlock* merged = marker->MergedNumber)
            {
                out->mergedVisible = merged->IsVisible();
                const SDK::FText& text = merged->Text;
                if (text.TextData)
                    NarrowAscii(text.TextData->TextSource.CStr(), text.TextData->TextSource.Num(),
                        out->mergedText, sizeof(out->mergedText));
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The category data the game keeps for a marker kind. Reports not loaded while
    // the soft asset is not; nothing here loads it.
    bool SampleCategory(const SDK::TSoftObjectPtr<SDK::UCrMapMenuCategoryData>& soft, bool* loaded, BrushSample* out)
    {
        __try
        {
            SDK::UCrMapMenuCategoryData* category = soft.Get();
            *loaded = category != nullptr;
            return category ? SampleBrush(category->Icon, out) : true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *loaded = false;
            return false;
        }
    }

    // Whether the map canvas has been laid out: Slate paints a widget a frame
    // after it opens, and its geometry reads as zero until then.
    bool MapReady(SDK::UCrUW_MapMenu* map)
    {
        __try
        {
            SDK::UCrUW_MapMenuMapArea* area = map->MapMenuMapArea;
            if (!area || !area->CanvasPanelMapArea)
                return false;

            const SDK::FGeometry geo  = area->CanvasPanelMapArea->GetCachedGeometry();
            const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetAbsoluteSize(geo);
            return size.X > 0.0 && size.Y > 0.0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The local player's name, narrowed the same way as the replicated entries'.
    void ReadLocalName(SDK::ACrPlayerControllerBase* pc, char* out, size_t size)
    {
        out[0] = '\0';
        __try
        {
            if (pc && pc->PlayerState)
            {
                const SDK::FString& name = pc->PlayerState->PlayerNamePrivate;
                NarrowAscii(name.CStr(), name.Num(), out, size);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
        }
    }

    // The widget's icon image, or null when it cannot be read.
    SDK::UImage* IconOf(SDK::UCrUW_MapMenuMarker* marker)
    {
        __try
        {
            return marker->ImageIcon;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // The replicated player entries, in array order. Returns how many were
    // copied; total is the array's real count, or -1 when it could not be read.
    int CollectPlayers(SDK::ACrMapMenuDataReplicationHelper* helper, PlayerEntry* out, int max, int* total)
    {
        __try
        {
            auto& entries = helper->PlayersMarkerDataContainer.PlayersMarkerData;
            const int32_t num = entries.Num();
            if (num < 0 || num > kMaxSaneCount)
            {
                *total = -1;
                return 0;
            }
            *total = num;

            int filled = 0;
            for (int32_t i = 0; i < num && filled < max; ++i)
            {
                const SDK::FPlayerMarkerDataFastArrayItem& e = entries[i];
                PlayerEntry& p = out[filled++];
                memset(&p, 0, sizeof(p));

                NarrowAscii(e.Player.CStr(), e.Player.Num(), p.name, sizeof(p.name));

                p.location[0] = e.Location.X;
                p.location[1] = e.Location.Y;
                p.color[0]    = e.PlayerColor.R;
                p.color[1]    = e.PlayerColor.G;
                p.color[2]    = e.PlayerColor.B;
                p.color[3]    = e.PlayerColor.A;
                p.rotationZ   = e.RotationZ;
                p.flags       = static_cast<uint8_t>(e.PlayerMarkerFlags);
            }
            return filled;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *total = -1;
            return 0;
        }
    }

    // The marker widgets of one of the map area's UI arrays, in array order. Every
    // element type holds its widget in a member named Marker. With max = 0 it
    // only counts.
    template<class Array>
    int CollectMarkers(Array& array, SDK::UCrUW_MapMenuMarker** out, int max, int* total)
    {
        __try
        {
            const int32_t num = array.Num();
            if (num < 0 || num > kMaxSaneCount)
            {
                *total = -1;
                return 0;
            }
            *total = num;

            int filled = 0;
            for (int32_t i = 0; i < num && filled < max; ++i)
                out[filled++] = array[i].Marker;
            return filled;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *total = -1;
            return 0;
        }
    }

    // A marker the game is showing at its own spot: visible, with a laid-out icon.
    // Hidden, filtered and not-yet-painted widgets read as stale geometry.
    bool UsableMarker(SDK::UCrUW_MapMenuMarker* marker, SDK::UObject* worldContext)
    {
        if (!marker)
            return false;

        SDK::UImage*  icon = IconOf(marker);
        SDK::UWidget* w    = icon ? static_cast<SDK::UWidget*>(icon) : marker;

        WidgetSample ws;
        return SampleWidget(w, worldContext, &ws) && ws.visible && ws.absSize[0] > 0.0 && ws.absSize[1] > 0.0;
    }

    // One building marker that has both a widget and replicated data.
    struct Candidate
    {
        int32_t sparseIndex; // into MapMenuMassMarkers
        int32_t dataIndex;   // into BuildingsMarkerData
        float   x, y;
        bool    spent;       // picked, or tried and found unusable
    };

    // Scratch for CollectPairs: plain data, no pointers, rewritten on every call.
    Candidate g_candidates[kMaxCandidates];

    // Lower is a better pick for target t: the four extremes of X and Y, the
    // marker nearest the middle, then three diagonals.
    float PickKey(int t, const Candidate& c, float mx, float my)
    {
        switch (t)
        {
        case 0:  return c.x;
        case 1:  return -c.x;
        case 2:  return c.y;
        case 3:  return -c.y;
        case 4:  return (c.x - mx) * (c.x - mx) + (c.y - my) * (c.y - my);
        case 5:  return c.x + c.y;
        case 6:  return -(c.x + c.y);
        default: return c.x - c.y;
        }
    }

    // Building marker widgets paired with their replicated data through the Mass
    // entity handle the map keys them by. These are the calibration pairs: a world
    // location the data carries, and a widget the game placed for it. Picked by
    // spread of world position, skipping widgets that are hidden or not laid out.
    int CollectPairs(SDK::UCrUW_MapMenuMapArea* area, SDK::ACrMapMenuDataReplicationHelper* helper,
                     SDK::UObject* worldContext, PairEntry* out, int max, int* widgetTotal, int* dataTotal,
                     int* candidateTotal)
    {
        __try
        {
            *candidateTotal = 0;
            auto& widgets = area->MapMenuMassMarkers;
            const int32_t allocated = widgets.NumAllocated();
            if (allocated < 0 || allocated > kMaxSaneCount)
            {
                *widgetTotal = -1;
                return 0;
            }

            auto& data = helper->BuildingsMarkerDataContainer.BuildingsMarkerData;
            const int32_t dataNum = data.Num();
            if (dataNum < 0 || dataNum > kMaxSaneCount)
            {
                *dataTotal = -1;
                return 0;
            }
            *dataTotal = dataNum;

            int    widgetCount = 0;
            int    nCand       = 0;
            double sumX        = 0.0;
            double sumY        = 0.0;
            for (int32_t i = 0; i < allocated; ++i)
            {
                if (!widgets.IsValidIndex(i))
                    continue;
                ++widgetCount;

                const SDK::FMassEntityHandle& handle = widgets[i].Key();
                for (int32_t d = 0; d < dataNum && nCand < kMaxCandidates; ++d)
                {
                    const SDK::FBuildingMarkerDataFastArrayItem& b = data[d];
                    if (b.BuildingHandle.Index != handle.Index || b.BuildingHandle.SerialNumber != handle.SerialNumber)
                        continue;

                    Candidate& c  = g_candidates[nCand++];
                    c.sparseIndex = i;
                    c.dataIndex   = d;
                    c.x           = b.Location.X;
                    c.y           = b.Location.Y;
                    c.spent       = false;
                    sumX += c.x;
                    sumY += c.y;
                    break;
                }
            }
            *widgetTotal    = widgetCount;
            *candidateTotal = nCand;
            if (nCand == 0)
                return 0;

            const float mx = static_cast<float>(sumX / nCand);
            const float my = static_cast<float>(sumY / nCand);

            int filled = 0;
            for (int t = 0; t < max; ++t)
            {
                for (int tries = 0; tries < kMaxPickTries; ++tries)
                {
                    int best = -1;
                    for (int c = 0; c < nCand; ++c)
                        if (!g_candidates[c].spent &&
                            (best < 0 || PickKey(t, g_candidates[c], mx, my) < PickKey(t, g_candidates[best], mx, my)))
                            best = c;
                    if (best < 0)
                        break;

                    Candidate& pick = g_candidates[best];
                    pick.spent = true;

                    SDK::UCrUW_MapMenuMarker* marker = widgets[pick.sparseIndex].Value().Marker;
                    if (!UsableMarker(marker, worldContext))
                        continue;

                    const SDK::FBuildingMarkerDataFastArrayItem& b = data[pick.dataIndex];
                    PairEntry& p = out[filled++];
                    memset(&p, 0, sizeof(p));
                    p.handleIndex  = b.BuildingHandle.Index;
                    p.handleSerial = b.BuildingHandle.SerialNumber;
                    p.haveData     = true;
                    p.location[0]  = pick.x;
                    p.location[1]  = pick.y;
                    p.type         = static_cast<uint8_t>(b.BuildingType);
                    p.uniqueName   = b.BuildingUniqueName;
                    p.marker       = marker;
                    break;
                }
            }
            return filled;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *widgetTotal = -1;
            return 0;
        }
    }

    // ---- logging --------------------------------------------------------------

    void LogWidget(const char* label, int index, SDK::UWidget* widget, SDK::UObject* worldContext)
    {
        WidgetSample ws;
        if (!widget || !SampleWidget(widget, worldContext, &ws))
        {
            LOG_INFO("[MapProbe] %s[%d] unreadable", label, index);
            return;
        }

        ObjNames names;
        ReadNames(widget, &names);
        LOG_INFO("[MapProbe] %s[%d] %s vis=%d slot=%s off(l,t,r,b)=(%.1f %.1f %.1f %.1f) "
                 "anchors=(%.2f %.2f %.2f %.2f) align=(%.2f %.2f) rt(T=(%.1f %.1f) S=(%.3f %.3f) ang=%.1f) "
                 "geo topLeftPx=(%.1f %.1f) topLeftVp=(%.1f %.1f) localSize=(%.1f %.1f) absSize=(%.1f %.1f)",
            label, index, NamesText(names).c_str(), ws.visible,
            ws.hasSlot ? (ws.slotIsCanvas ? "canvas" : "other") : "none",
            ws.offsets[0], ws.offsets[1], ws.offsets[2], ws.offsets[3],
            ws.anchors[0], ws.anchors[1], ws.anchors[2], ws.anchors[3], ws.alignment[0], ws.alignment[1],
            ws.rtTranslation[0], ws.rtTranslation[1], ws.rtScale[0], ws.rtScale[1], ws.rtAngle,
            ws.topLeftPixel[0], ws.topLeftPixel[1], ws.topLeftViewport[0], ws.topLeftViewport[1],
            ws.localSize[0], ws.localSize[1], ws.absSize[0], ws.absSize[1]);
    }

    void LogBrush(const char* label, int index, const char* what, const BrushSample& b)
    {
        char tint[96];
        FormatColour(b.tint, tint, sizeof(tint));
        LOG_INFO("[MapProbe] %s[%d] %s brushTint=%s drawAs=%u imageType=%u resourceName=%s object=%s",
            label, index, what, tint, b.drawAs, b.imageType, NameOf(b.resourceName).c_str(),
            b.hasObject ? NamesText(b.object).c_str() : "-");

        if (!b.hasParams)
            return;

        std::string params;
        for (int i = 0; i < kParamCount; ++i)
        {
            const float* v = b.params[i];
            if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f)
                continue;

            char one[96];
            snprintf(one, sizeof(one), " %ls=(%.3f %.3f %.3f %.3f)", kParamNames[i], v[0], v[1], v[2], v[3]);
            params += one;
        }
        LOG_INFO("[MapProbe] %s[%d] %s material parent=%s vectorParams:%s", label, index, what,
            b.hasParent ? NamesText(b.parent).c_str() : "-", params.empty() ? " none non-zero" : params.c_str());
    }

    void LogMarkerColours(const char* label, int index, SDK::UCrUW_MapMenuMarker* marker)
    {
        MarkerColours c;
        if (!SampleColours(marker, &c))
        {
            LOG_INFO("[MapProbe] %s[%d] colours unreadable", label, index);
            return;
        }

        char user[96], tint[96];
        FormatColour(c.userWidget, user, sizeof(user));
        FormatColour(c.iconColor, tint, sizeof(tint));
        LOG_INFO("[MapProbe] %s[%d] colours widget=%s | icon=%s%s | merged visible=%d text='%s'",
            label, index, user, c.hasIcon ? "" : "(no icon) ", tint, c.mergedVisible, c.mergedText);
        if (c.hasIcon)
            LogBrush(label, index, "icon", c.brush);
    }

    // The widget and its icon image: the image is what the player actually sees,
    // so its geometry is the marker's true on-screen centre.
    void LogMarker(const char* label, int index, SDK::UCrUW_MapMenuMarker* marker, SDK::UObject* worldContext)
    {
        if (!marker)
        {
            LOG_INFO("[MapProbe] %s[%d] null widget", label, index);
            return;
        }

        LogWidget(label, index, marker, worldContext);
        if (SDK::UImage* icon = IconOf(marker))
        {
            char iconLabel[24];
            snprintf(iconLabel, sizeof(iconLabel), "%s-icon", label);
            LogWidget(iconLabel, index, icon, worldContext);
        }
        LogMarkerColours(label, index, marker);
    }

    // The zoom slider next to the map's own zoom and marker-merging settings, to
    // relate the slider to the scale the geometry reports.
    void LogZoom(SDK::UCrUW_MapMenu* map)
    {
        if (SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj())
        {
            LOG_INFO("[MapProbe] settings zoom min=%.3f default=%.3f max=%.3f pivot=(%.1f %.1f %.1f) "
                     "merge minDist=%.1f zoomFactor=%.3f zoomStep=%.3f",
                settings->MinZoom, settings->DefaultZoom, settings->MaxZoom,
                settings->MapAreaPivotPoint.X, settings->MapAreaPivotPoint.Y, settings->MapAreaPivotPoint.Z,
                settings->MarkerMergingMinimalDistance, settings->MarkerMergingDistanceZoomFactor,
                settings->MarkerMergingDistanceZoomStep);
        }

        __try
        {
            SDK::UCrUW_MapMenuZoomSlider* zoom   = map->MapMenuZoomSlider;
            SDK::USlider*                 slider = zoom ? zoom->Slider : nullptr;
            if (slider)
                LOG_INFO("[MapProbe] zoom slider value=%.4f min=%.3f max=%.3f", slider->Value, slider->MinValue,
                    slider->MaxValue);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_INFO("[MapProbe] zoom slider unreadable");
        }
    }

    // The category icons the game keeps for the player, coop and personal markers.
    // These are soft assets: the log says so when one is not loaded.
    void LogCategories()
    {
        SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj();
        if (!settings)
            return;

        const struct
        {
            const char*                                                 label;
            const SDK::TSoftObjectPtr<SDK::UCrMapMenuCategoryData>*     soft;
        } kinds[] = {
            { "category-player",   &settings->PlayerMarkerCateroryData },
            { "category-coop",     &settings->CoopMarkerCateroryData },
            { "category-personal", &settings->PersonalMarkerCateroryData },
        };

        for (const auto& kind : kinds)
        {
            bool loaded = false;
            BrushSample brush;
            if (!SampleCategory(*kind.soft, &loaded, &brush))
                LOG_INFO("[MapProbe] %s unreadable", kind.label);
            else if (!loaded)
                LOG_INFO("[MapProbe] %s not loaded", kind.label);
            else
                LogBrush(kind.label, 0, "icon", brush);
        }
    }

    // A few terrain segments, for the world extent one segment covers.
    void LogTerrainSegments(SDK::UCrUW_MapMenuMapArea* area, SDK::UObject* worldContext)
    {
        SDK::UCrUW_MapMenuTerrain* terrain = area->MapMenuTerrain;
        if (!terrain)
            return;

        SDK::UWidget* segments[kMaxSegments] = {};
        int32_t       indices[kMaxSegments]  = {};
        int           shown = 0;
        int32_t       total = -1;

        __try
        {
            total = terrain->TerrainSegments.Num();
            const int32_t stride = total > kMaxSegments ? total / kMaxSegments : 1;
            for (int32_t i = 0; i < total && total <= kMaxSaneCount && shown < kMaxSegments; i += stride)
            {
                segments[shown] = terrain->TerrainSegments[i];
                indices[shown]  = i;
                ++shown;
            }

            LOG_INFO("[MapProbe] terrain segments=%d debugBorder pos=(%.1f %.1f) size=(%.1f %.1f)", total,
                terrain->DebugBorderTexturePosition.X, terrain->DebugBorderTexturePosition.Y,
                terrain->DebugBorderTextureSize.X, terrain->DebugBorderTextureSize.Y);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_INFO("[MapProbe] terrain segments unreadable");
            return;
        }

        for (int i = 0; i < shown; ++i)
            LogWidget("segment", indices[i], segments[i], worldContext);
    }

    void LogPlayerEntries(const PlayerEntry* players, int count, const char* localName, const SDK::FVector& body)
    {
        for (int i = 0; i < count; ++i)
        {
            const PlayerEntry& p = players[i];
            const double dx = p.location[0] - body.X;
            const double dy = p.location[1] - body.Y;
            char colour[96];
            FormatColour(p.color, colour, sizeof(colour));
            LOG_INFO("[MapProbe] playerData[%d] name='%s' local=%d loc=(%.1f %.1f) rotZ=%.1f flags=%u "
                     "distToBodyXY=%.1f colour %s",
                i, p.name, localName[0] && strcmp(localName, p.name) == 0, p.location[0], p.location[1], p.rotationZ,
                p.flags, std::sqrt(dx * dx + dy * dy), colour);
        }
    }

    void LogSnapshot(SDK::UCrUW_MapMenu* map, int number)
    {
        SDK::ACrPlayerControllerBase* pc        = LocalPlayerController();
        SDK::ACrCharacterPlayerBase*  character = pc ? pc->CrChar : nullptr;
        SDK::UCrUW_MapMenuMapArea*    area      = map->MapMenuMapArea;
        SDK::UWorld*                  world     = SDK::UWorld::GetWorld();

        LOG_INFO("[MapProbe] ---- snapshot %d/%d ----", number, kMaxSnapshots);

        // Where the player and the drone are, in the same world space the
        // replicated marker data should be in.
        SDK::FVector body = {};
        if (character)
        {
            body = character->K2_GetActorLocation();
            const SDK::FRotator bodyRot = character->K2_GetActorRotation();
            LOG_INFO("[MapProbe] open inDrone=%d body=(%.1f %.1f %.1f) bodyYaw=%.1f controlYaw=%.1f",
                IsLocalPlayerInDrone(), body.X, body.Y, body.Z, bodyRot.Yaw, pc->ControlRotation.Yaw);

            if (SDK::ACrCharacterDroneBase* drone = character->BuildingDrone)
            {
                const SDK::FVector d = drone->K2_GetActorLocation();
                LOG_INFO("[MapProbe] drone=(%.1f %.1f %.1f) droneYaw=%.1f", d.X, d.Y, d.Z,
                    drone->K2_GetActorRotation().Yaw);
            }
            else
                LOG_INFO("[MapProbe] drone not deployed");

            // One of the places the player's own marker colour lives; the others
            // are the replicated entry and the widget, logged below.
            const float own[4] = { character->MultiplayerColor.R, character->MultiplayerColor.G,
                                   character->MultiplayerColor.B, character->MultiplayerColor.A };
            char text[96];
            FormatColour(own, text, sizeof(text));
            LOG_INFO("[MapProbe] colour source: character MultiplayerColor %s", text);
        }
        else
            LOG_INFO("[MapProbe] no local character");

        char localName[128];
        ReadLocalName(pc, localName, sizeof(localName));
        LOG_INFO("[MapProbe] local player name='%s'", localName);

        LogZoom(map);

        if (!area)
        {
            LOG_INFO("[MapProbe] map has no MapMenuMapArea");
            return;
        }

        // The map's own panels: the canvas the markers live on and the terrain
        // that pans and zooms under them, plus the crosshair the map draws.
        LogWidget("canvas", 0, area->CanvasPanelMapArea, pc);
        LogWidget("terrain", 0, area->MapMenuTerrain, pc);
        LogWidget("crosshair", 0, area->MapMenuCrosshair, pc);
        LogCategories();

        SDK::ACrMapMenuDataReplicationHelper* helper = nullptr;
        if (world && world->GameState && world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
            helper = static_cast<SDK::ACrGameStateBase*>(world->GameState)->MapMenuDataReplicationHelper;

        PlayerEntry players[kMaxPlayers];
        int playerTotal = -1;
        const int playerCount = helper ? CollectPlayers(helper, players, kMaxPlayers, &playerTotal) : 0;

        SDK::UCrUW_MapMenuMarker* coop[kMaxCoop] = {};
        int coopTotal = -1;
        const int coopCount = CollectMarkers(area->MapMenuCoopMarkers, coop, kMaxCoop, &coopTotal);

        // The other marker kinds: counts for all, a couple of widgets for the two
        // that could be the game's own arrow if the coop list has none for the
        // local player.
        SDK::UCrUW_MapMenuMarker* personal[kMaxExtra] = {};
        SDK::UCrUW_MapMenuMarker* poi[kMaxExtra]      = {};
        int personalTotal = -1, poiTotal = -1, infectionTotal = -1, attackTotal = -1;
        const int personalCount =
            CollectMarkers(area->MapMenuPlayerPersonalMarkers, personal, kMaxExtra, &personalTotal);
        const int poiCount = CollectMarkers(area->MapMenuPOIMarkers, poi, kMaxExtra, &poiTotal);
        CollectMarkers(area->MapMenuInfectionNotificationMarkers, nullptr, 0, &infectionTotal);
        CollectMarkers(area->MapMenuAttackWaveMarkers, nullptr, 0, &attackTotal);

        PairEntry pairs[kMaxPairs];
        int pairWidgets    = -1;
        int pairData       = -1;
        int pairCandidates = 0;
        const int pairCount = helper
            ? CollectPairs(area, helper, pc, pairs, kMaxPairs, &pairWidgets, &pairData, &pairCandidates)
            : 0;

        // The local-player question: does the replicated array carry the local
        // player, and is there a coop widget for each entry.
        LOG_INFO("[MapProbe] helper=%d playerData=%d coopWidgets=%d buildingWidgets=%d buildingData=%d "
                 "pairCandidates=%d pairsPicked=%d personal=%d poi=%d infection=%d attackWave=%d",
            helper != nullptr, playerTotal, coopTotal, pairWidgets, pairData, pairCandidates, pairCount,
            personalTotal, poiTotal, infectionTotal, attackTotal);

        LogPlayerEntries(players, playerCount, localName, body);

        for (int i = 0; i < coopCount; ++i)
            LogMarker("coop", i, coop[i], pc);
        for (int i = 0; i < personalCount; ++i)
            LogMarker("personal", i, personal[i], pc);
        for (int i = 0; i < poiCount; ++i)
            LogMarker("poi", i, poi[i], pc);

        // Calibration pairs: world location from the data, pixel position from
        // the widget the game placed for it.
        for (int i = 0; i < pairCount; ++i)
        {
            const PairEntry& p = pairs[i];
            LOG_INFO("[MapProbe] pair[%d] handle=(%d %d) data=%d world=(%.1f %.1f) type=%u name=%s", i,
                p.handleIndex, p.handleSerial, p.haveData, p.location[0], p.location[1], p.type,
                p.haveData ? NameOf(p.uniqueName).c_str() : "-");
            LogMarker("pair", i, p.marker, pc);
        }

        LogTerrainSegments(area, pc);
    }

    void TickImpl()
    {
        SDK::UCrUW_MapMenu* map = ActiveLocalMap();
        if (!map)
        {
            g_wasOpen = false;
            return;
        }

        const uint64_t now = GetTickCount64();
        if (!g_wasOpen)
        {
            if (++g_opens > kMaxOpens)
            {
                g_enabled.store(false);
                LOG_INFO("[MapProbe] %d opens logged; probe switched off. Type bd_mapprobe on to run it again.",
                    kMaxOpens);
                return;
            }

            g_wasOpen   = true;
            g_snapshots = 0;
            g_openMs    = now;
            LOG_INFO("[MapProbe] map opened (open %d/%d)", g_opens, kMaxOpens);
        }

        if (g_snapshots == 0)
        {
            // The first snapshot waits for the canvas to be laid out, or gives up
            // waiting and logs whatever is there.
            if (!MapReady(map) && now - g_openMs < kReadyTimeoutMs)
                return;
        }
        else if (g_snapshots >= kMaxSnapshots || now - g_lastMs < kMinIntervalMs)
            return;

        g_lastMs = now;
        LogSnapshot(map, ++g_snapshots);

        if (g_snapshots == kMaxSnapshots)
            LOG_INFO("[MapProbe] snapshot cap reached; close and reopen the map for more");
    }

    // Kept apart from TickImpl: a function with __try may not hold objects that
    // need unwinding, and the snapshot code does.
    bool RunGuarded()
    {
        __try
        {
            TickImpl();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // "bd_mapprobe [on|off]" -- no argument prints the state. gameThread = true,
    // so this runs on the tick.
    void HandleMapProbe(const char* const* argv, int argc, PluginConsoleSink sink, void* userData)
    {
        IPluginSelf* self = static_cast<IPluginSelf*>(userData);
        if (!self || !self->hooks || !self->hooks->Console)
            return;

        IPluginConsole* console = self->hooks->Console;

        if (argc >= 2)
        {
            if (_stricmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0)
                g_enabled.store(true);
            else if (_stricmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0)
                g_enabled.store(false);
            else
            {
                console->Printf(sink, PluginConsoleLineKind::Error, "Usage: bd_mapprobe [on|off]");
                return;
            }

            g_wasOpen = false;
            g_opens   = 0;
        }

        console->Printf(sink, PluginConsoleLineKind::Output,
            "Map probe is %s. Open the map to log; grep the log for [MapProbe].",
            g_enabled.load() ? "on" : "off");
    }
}

void InitMapProbe(IPluginSelf* self)
{
    if (!self || !self->hooks || !self->hooks->Console)
    {
        LOG_WARN("MapProbe: console unavailable, '%s' not registered.", kCommandName);
        return;
    }

    PluginConsoleCommandDesc desc{};
    desc.name       = kCommandName;
    desc.usage      = "bd_mapprobe [on|off]";
    desc.help       = "Log what the map shows (marker positions, colours, geometry) while it is open. Off by default.";
    desc.handler    = &HandleMapProbe;
    desc.userData   = self;
    desc.gameThread = true;

    g_commandRegistered = self->hooks->Console->RegisterCommand(self, &desc);
    if (!g_commandRegistered)
        LOG_WARN("MapProbe: console command '%s' is already taken.", kCommandName);
}

void ShutdownMapProbe(IPluginSelf* self)
{
    g_enabled.store(false);
    g_wasOpen = false;

    if (g_commandRegistered && self && self->hooks && self->hooks->Console)
        self->hooks->Console->UnregisterCommand(self, kCommandName);
    g_commandRegistered = false;
}

void TickMapProbe(float)
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;

    // Fail closed: a bad read ends the probe instead of repeating every tick.
    // The SEH guard catches a fault anywhere in a snapshot, including the plain
    // reads outside the Sample and Collect helpers; the catch is for C++ errors.
    try
    {
        if (RunGuarded())
            return;

        LOG_ERROR("[MapProbe] access violation while reading the map; probe switched off");
    }
    catch (...)
    {
        LOG_ERROR("[MapProbe] read threw; probe switched off");
    }

    g_enabled.store(false);
    g_wasOpen = false;
}
