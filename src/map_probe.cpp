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

    // The first snapshot is logged the moment the map opens, then one every
    // kMinIntervalMs while it stays open, and none past kMaxSnapshots until it
    // is closed and opened again.
    constexpr uint64_t kMinIntervalMs = 2000;
    constexpr int      kMaxSnapshots  = 8;

    // Widgets sampled per snapshot. The area's own markers number in the
    // dozens; a few well-spread ones are enough to fit a transform.
    constexpr int kMaxCoop     = 6;
    constexpr int kMaxPairs    = 5;
    constexpr int kMaxSegments = 3;
    constexpr int kMaxPlayers  = 8;

    // Guard against reading a torn or stale TArray header as a huge count.
    constexpr int32_t kMaxSaneCount = 100000;

    bool g_commandRegistered = false;

    // Set by the console command, which runs on the game thread, and read by
    // the tick. Atomic all the same: nothing here depends on that ordering.
    std::atomic<bool> g_enabled{ false };

    // Per-open state. Only the tick touches these.
    bool     g_wasOpen   = false;
    int      g_snapshots = 0;
    uint64_t g_lastMs    = 0;

    // ---- plain-data samples ------------------------------------------------
    // Everything read through a game pointer is copied into one of these inside
    // an SEH-guarded function, then logged from a plain one. The layouts of the
    // map widgets are only known from the SDK dump, so a bad read must cost one
    // log line, not the game.

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

    struct MarkerColours
    {
        float      userWidget[4];    // UUserWidget::ColorAndOpacity
        float      iconColor[4];     // ImageIcon->ColorAndOpacity
        float      iconBrushTint[4]; // ImageIcon->Brush.TintColor
        SDK::FName brushResource;
        bool       hasIcon;
    };

    struct PlayerEntry
    {
        char    name[48];
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

    std::string WidgetNames(SDK::UObject* obj)
    {
        try
        {
            return obj->GetName() + " : " + obj->Class->GetName();
        }
        catch (...) { return "?"; }
    }

    // ---- SEH readers (POD locals only) --------------------------------------

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
                const SDK::FLinearColor& tint  = icon->ColorAndOpacity;
                const SDK::FLinearColor& brush = icon->Brush.TintColor.SpecifiedColor;
                out->iconColor[0]     = tint.R;
                out->iconColor[1]     = tint.G;
                out->iconColor[2]     = tint.B;
                out->iconColor[3]     = tint.A;
                out->iconBrushTint[0] = brush.R;
                out->iconBrushTint[1] = brush.G;
                out->iconBrushTint[2] = brush.B;
                out->iconBrushTint[3] = brush.A;
                out->brushResource    = icon->Brush.ResourceName;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
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

                const wchar_t* chars = e.Player.CStr();
                const int32_t len = e.Player.Num();
                if (chars && len > 0 && len < 1024)
                {
                    for (int n = 0; n < static_cast<int>(sizeof(p.name)) - 1 && chars[n]; ++n)
                        p.name[n] = chars[n] < 0x80 ? static_cast<char>(chars[n]) : '?';
                }

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

    // The coop marker widgets, in array order.
    int CollectCoopWidgets(SDK::UCrUW_MapMenuMapArea* area, SDK::UCrUW_MapMenuMarker** out, int max, int* total)
    {
        __try
        {
            const int32_t num = area->MapMenuCoopMarkers.Num();
            if (num < 0 || num > kMaxSaneCount)
            {
                *total = -1;
                return 0;
            }
            *total = num;

            int filled = 0;
            for (int32_t i = 0; i < num && filled < max; ++i)
                out[filled++] = area->MapMenuCoopMarkers[i].Marker;
            return filled;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *total = -1;
            return 0;
        }
    }

    // Building marker widgets paired with their replicated data through the
    // Mass entity handle the map keys them by. These are the calibration
    // pairs: a world location the data carries, and a widget the game placed
    // for it. Taken at an even stride so the sample spans the map.
    int CollectPairs(SDK::UCrUW_MapMenuMapArea* area, SDK::ACrMapMenuDataReplicationHelper* helper,
                     PairEntry* out, int max, int* widgetTotal, int* dataTotal)
    {
        __try
        {
            auto& widgets = area->MapMenuMassMarkers;
            const int32_t allocated = widgets.NumAllocated();
            if (allocated < 0 || allocated > kMaxSaneCount)
            {
                *widgetTotal = -1;
                return 0;
            }

            int count = 0;
            for (int32_t i = 0; i < allocated; ++i)
                if (widgets.IsValidIndex(i))
                    ++count;
            *widgetTotal = count;

            auto& data = helper->BuildingsMarkerDataContainer.BuildingsMarkerData;
            const int32_t dataNum = data.Num();
            *dataTotal = (dataNum >= 0 && dataNum <= kMaxSaneCount) ? dataNum : -1;

            const int stride = count > max ? count / max : 1;
            int seen   = 0;
            int filled = 0;
            for (int32_t i = 0; i < allocated && filled < max; ++i)
            {
                if (!widgets.IsValidIndex(i))
                    continue;
                if (seen++ % stride != 0)
                    continue;

                PairEntry& p = out[filled++];
                memset(&p, 0, sizeof(p));
                p.handleIndex  = widgets[i].Key().Index;
                p.handleSerial = widgets[i].Key().SerialNumber;
                p.marker       = widgets[i].Value().Marker;

                for (int32_t d = 0; d < dataNum && *dataTotal >= 0; ++d)
                {
                    const SDK::FBuildingMarkerDataFastArrayItem& b = data[d];
                    if (b.BuildingHandle.Index != p.handleIndex || b.BuildingHandle.SerialNumber != p.handleSerial)
                        continue;

                    p.haveData    = true;
                    p.location[0] = b.Location.X;
                    p.location[1] = b.Location.Y;
                    p.type        = static_cast<uint8_t>(b.BuildingType);
                    p.uniqueName  = b.BuildingUniqueName;
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

        const std::string names = WidgetNames(widget);
        LOG_INFO("[MapProbe] %s[%d] %s vis=%d slot=%s off(l,t,r,b)=(%.1f %.1f %.1f %.1f) "
                 "anchors=(%.2f %.2f %.2f %.2f) align=(%.2f %.2f) rt(T=(%.1f %.1f) S=(%.3f %.3f) ang=%.1f)",
            label, index, names.c_str(), ws.visible, ws.hasSlot ? (ws.slotIsCanvas ? "canvas" : "other") : "none",
            ws.offsets[0], ws.offsets[1], ws.offsets[2], ws.offsets[3],
            ws.anchors[0], ws.anchors[1], ws.anchors[2], ws.anchors[3], ws.alignment[0], ws.alignment[1],
            ws.rtTranslation[0], ws.rtTranslation[1], ws.rtScale[0], ws.rtScale[1], ws.rtAngle);
        LOG_INFO("[MapProbe] %s[%d] geo topLeftPx=(%.1f %.1f) topLeftVp=(%.1f %.1f) localSize=(%.1f %.1f) absSize=(%.1f %.1f)",
            label, index, ws.topLeftPixel[0], ws.topLeftPixel[1], ws.topLeftViewport[0], ws.topLeftViewport[1],
            ws.localSize[0], ws.localSize[1], ws.absSize[0], ws.absSize[1]);
    }

    void LogMarkerColours(const char* label, int index, SDK::UCrUW_MapMenuMarker* marker)
    {
        MarkerColours c;
        if (!SampleColours(marker, &c))
        {
            LOG_INFO("[MapProbe] %s[%d] colours unreadable", label, index);
            return;
        }

        char user[96], tint[96], brush[96];
        FormatColour(c.userWidget, user, sizeof(user));
        FormatColour(c.iconColor, tint, sizeof(tint));
        FormatColour(c.iconBrushTint, brush, sizeof(brush));
        LOG_INFO("[MapProbe] %s[%d] colours widget=%s | icon=%s%s | brushTint=%s | brushResource=%s",
            label, index, user, c.hasIcon ? "" : "(no icon) ", tint, brush,
            c.hasIcon ? NameOf(c.brushResource).c_str() : "-");
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
            LogWidget("icon", index, icon, worldContext);
        LogMarkerColours(label, index, marker);
    }

    // The zoom slider next to the map's own zoom limits, to relate the slider to
    // the scale the geometry reports.
    void LogZoom(SDK::UCrUW_MapMenu* map)
    {
        if (SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj())
        {
            LOG_INFO("[MapProbe] settings zoom min=%.3f default=%.3f max=%.3f pivot=(%.1f %.1f %.1f)",
                settings->MinZoom, settings->DefaultZoom, settings->MaxZoom,
                settings->MapAreaPivotPoint.X, settings->MapAreaPivotPoint.Y, settings->MapAreaPivotPoint.Z);
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

    void LogPlayerEntries(const PlayerEntry* players, int count, const std::string& localName, const SDK::FVector& body)
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
                i, p.name, !localName.empty() && localName == p.name, p.location[0], p.location[1], p.rotationZ,
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

        std::string localName;
        try
        {
            if (pc && pc->PlayerState)
                localName = pc->PlayerState->PlayerNamePrivate.ToString();
        }
        catch (...) {}
        LOG_INFO("[MapProbe] local player name='%s'", localName.c_str());

        LogZoom(map);

        if (!area)
        {
            LOG_INFO("[MapProbe] map has no MapMenuMapArea");
            return;
        }

        // The map's own panels: the canvas the markers live on and the terrain
        // that pans and zooms under them.
        LogWidget("canvas", 0, area->CanvasPanelMapArea, pc);
        LogWidget("terrain", 0, area->MapMenuTerrain, pc);

        SDK::ACrMapMenuDataReplicationHelper* helper = nullptr;
        if (world && world->GameState && world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
            helper = static_cast<SDK::ACrGameStateBase*>(world->GameState)->MapMenuDataReplicationHelper;

        PlayerEntry players[kMaxPlayers];
        int playerTotal = -1;
        const int playerCount = helper ? CollectPlayers(helper, players, kMaxPlayers, &playerTotal) : 0;

        SDK::UCrUW_MapMenuMarker* coop[kMaxCoop] = {};
        int coopTotal = -1;
        const int coopCount = CollectCoopWidgets(area, coop, kMaxCoop, &coopTotal);

        PairEntry pairs[kMaxPairs];
        int pairWidgets = -1;
        int pairData    = -1;
        const int pairCount = helper ? CollectPairs(area, helper, pairs, kMaxPairs, &pairWidgets, &pairData) : 0;

        // The local-player question: does the replicated array carry the local
        // player, and is there a coop widget for each entry.
        LOG_INFO("[MapProbe] helper=%d playerData=%d coopWidgets=%d buildingWidgets=%d buildingData=%d",
            helper != nullptr, playerTotal, coopTotal, pairWidgets, pairData);

        LogPlayerEntries(players, playerCount, localName, body);

        for (int i = 0; i < coopCount; ++i)
            LogMarker("coop", i, coop[i], pc);

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
            g_wasOpen   = true;
            g_snapshots = 0;
            LOG_INFO("[MapProbe] map opened");
        }
        else if (g_snapshots >= kMaxSnapshots || now - g_lastMs < kMinIntervalMs)
            return;

        g_lastMs = now;
        LogSnapshot(map, ++g_snapshots);

        if (g_snapshots == kMaxSnapshots)
            LOG_INFO("[MapProbe] snapshot cap reached; close and reopen the map for more");
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

    try
    {
        TickImpl();
    }
    catch (...)
    {
        // Fail closed: one bad read ends the probe instead of repeating every tick.
        g_enabled.store(false);
        g_wasOpen = false;
        LOG_ERROR("[MapProbe] read threw; probe switched off");
    }
}
