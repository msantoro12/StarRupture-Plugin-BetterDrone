#include "drone_map_marker.h"
#include "drone_interact.h"
#include "drone_marker_icon.h"
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
#include <mutex>
#include <windows.h>

namespace
{
    // The map's own terrain widget lays out 6000 widget units for 600000 world
    // units, so one widget unit is 100 world units. With the widget's size on
    // screen, that gives pixels per world unit at any zoom and DPI scale.
    constexpr double kWorldPerLocalUnit = 100.0;

    // The placement is checked against the game's own building markers. One that
    // sits further than this from where the transform puts it means the
    // transform is wrong, and nothing is drawn.
    constexpr double kTolerancePx = 2.0;

    // A tick checks up to this many markers, looking at no more than kMaxScan
    // map widgets to find them, and shows the drone if at least one agrees. A
    // merged cluster marker sits at its cluster's centre and would disagree on
    // its own.
    constexpr int kMaxReferences = 3;
    constexpr int kMaxScan       = 40;

    // Guard against reading a torn array header as a huge count.
    constexpr int32_t kMaxSaneCount = 100000;

    constexpr uint64_t kLogIntervalMs = 5000;

    // The player marker colour when the game's colour asset is not loaded: the
    // cyan of the player arrow on the map, as an ImGui colour (R 119, G 225, B 233).
    constexpr unsigned int kFallbackColour = 0xFFE9E177u;

    // What the render callback draws, as plain numbers.
    struct Snapshot
    {
        bool         valid;
        float        x, y;       // viewport pixels
        float        yawDeg;     // the drone's world yaw
        float        dpi;        // pixels per widget unit
        float        clip[4];    // the map canvas, viewport pixels
        unsigned int colour;
    };

    std::mutex g_lock;
    Snapshot   g_snapshot = {};

    std::atomic<WidgetHandle> g_widget{ nullptr };
    bool g_widgetShown = false;   // tick only

    // Tick only. A marker that agreed last tick is tried first. Handle numbers,
    // not pointers: the widget is looked up again each tick.
    int32_t  g_refIndex  = -1;
    int32_t  g_refSerial = 0;
    uint64_t g_lastLogMs = 0;
    bool     g_failed    = false;
    bool     g_loggedColourFallback = false;

    // A transparent window the draw list rides on: no title, no input, no
    // background. The widget is shown only while there is something to draw.
    constexpr int kWindowFlagAlwaysAutoResize = 1 << 6;
    PluginWindowHints g_hints = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0,
        PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoResize | PluginWindowFlags_NoMove |
        PluginWindowFlags_NoScrollbar | PluginWindowFlags_NoBackground | PluginWindowFlags_NoSavedSettings |
        PluginWindowFlags_NoMouseInputs | kWindowFlagAlwaysAutoResize
    };

    // ---- render thread --------------------------------------------------------

    void RenderMarker(IModLoaderImGui* ui)
    {
        Snapshot s;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            s = g_snapshot;
        }
        if (!s.valid)
            return;

        PluginDrawList dl = ui->GetForegroundDrawList();
        ui->DL_PushClipRect(dl, s.clip[0], s.clip[1], s.clip[2], s.clip[3], true);
        DroneMarkerIcon::Draw(ui, dl, s.x, s.y, s.yawDeg, s.dpi, s.colour);
        ui->DL_PopClipRect(dl);
    }

    // ---- game thread: reads, each under SEH, into plain data ----------------------

    struct Geo
    {
        bool   visible;
        double topLeft[2];   // viewport pixels
        double absSize[2];
        double localSize[2];
    };

    bool SampleGeo(SDK::UWidget* w, SDK::UObject* worldContext, Geo* out)
    {
        __try
        {
            memset(out, 0, sizeof(*out));
            out->visible = w->IsVisible();

            const SDK::FGeometry geo       = w->GetCachedGeometry();
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
            out->topLeft[0] = pixel.X;
            out->topLeft[1] = pixel.Y;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool LaidOut(const Geo& g)
    {
        return g.absSize[0] > 0.0 && g.absSize[1] > 0.0 && g.localSize[0] > 0.0 && g.localSize[1] > 0.0;
    }

    // World to viewport pixels.
    struct Transform
    {
        double originX, originY; // pixel of the world origin
        double scale;            // pixels per world unit
    };

    // The terrain widget's top-left corner is the dev settings' pivot point, and
    // its on-screen size per widget unit is the scale.
    Transform MakeTransform(const Geo& terrain, double pivotX, double pivotY)
    {
        Transform t;
        t.scale   = terrain.absSize[0] / terrain.localSize[0] / kWorldPerLocalUnit;
        t.originX = terrain.topLeft[0] - pivotX * t.scale;
        t.originY = terrain.topLeft[1] - pivotY * t.scale;
        return t;
    }

    // A building marker's widget sits with its top-left corner on the building's
    // world point. -1 when it is hidden or not laid out, else 1 when it is where
    // the transform says and 0 when it is not.
    int CompareReference(SDK::UCrUW_MapMenuMarker* marker, double worldX, double worldY, SDK::UObject* ctx,
                         const Transform& t)
    {
        Geo g;
        if (!marker || !SampleGeo(marker, ctx, &g) || !g.visible || !LaidOut(g))
            return -1;

        const double dx = t.originX + worldX * t.scale - g.topLeft[0];
        const double dy = t.originY + worldY * t.scale - g.topLeft[1];
        return std::fabs(dx) <= kTolerancePx && std::fabs(dy) <= kTolerancePx ? 1 : 0;
    }

    // Looks at up to kMaxReferences building markers the game shows and counts
    // how many sit where the transform puts them.
    void CheckReferences(SDK::UCrUW_MapMenuMapArea* area, SDK::ACrMapMenuDataReplicationHelper* helper,
                         SDK::UObject* ctx, const Transform& t, int* checked, int* agreed)
    {
        *checked = 0;
        *agreed  = 0;
        __try
        {
            auto& widgets = area->MapMenuMassMarkers;
            const int32_t allocated = widgets.NumAllocated();
            auto& data = helper->BuildingsMarkerDataContainer.BuildingsMarkerData;
            const int32_t dataNum = data.Num();
            if (allocated < 0 || allocated > kMaxSaneCount || dataNum < 0 || dataNum > kMaxSaneCount)
                return;

            int scanned = 0;
            for (int pass = 0; pass < 2; ++pass)
            {
                for (int32_t i = 0; i < allocated; ++i)
                {
                    if (!widgets.IsValidIndex(i))
                        continue;

                    const SDK::FMassEntityHandle& handle = widgets[i].Key();
                    const bool isLast = handle.Index == g_refIndex && handle.SerialNumber == g_refSerial;
                    if (isLast != (pass == 0))
                        continue;
                    if (pass == 1 && ++scanned > kMaxScan)
                        break;

                    for (int32_t d = 0; d < dataNum; ++d)
                    {
                        const SDK::FBuildingMarkerDataFastArrayItem& b = data[d];
                        if (b.BuildingHandle.Index != handle.Index || b.BuildingHandle.SerialNumber != handle.SerialNumber)
                            continue;

                        const int r = CompareReference(widgets[i].Value().Marker, b.Location.X, b.Location.Y, ctx, t);
                        if (r >= 0)
                        {
                            ++*checked;
                            if (r == 1)
                            {
                                ++*agreed;
                                g_refIndex  = handle.Index;
                                g_refSerial = handle.SerialNumber;
                            }
                            else if (isLast)
                                g_refIndex = -1;
                        }
                        break;
                    }

                    if (*checked >= kMaxReferences)
                        return;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *checked = 0;
            *agreed  = 0;
        }
    }

    // Everything the placement needs from the drone.
    struct DroneState
    {
        bool   ok;
        double x, y, yawDeg;
    };

    bool ReadDrone(SDK::ACrPlayerControllerBase* pc, DroneState* out)
    {
        __try
        {
            memset(out, 0, sizeof(*out));
            SDK::ACrCharacterPlayerBase* character = pc ? pc->CrChar : nullptr;
            SDK::ACrCharacterDroneBase*  drone     = character ? character->BuildingDrone : nullptr;
            if (!drone)
                return false;

            const SDK::FVector location = drone->K2_GetActorLocation();
            out->x      = location.X;
            out->y      = location.Y;
            out->yawDeg = drone->K2_GetActorRotation().Yaw;
            out->ok = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    SDK::ACrMapMenuDataReplicationHelper* ReplicationHelper()
    {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world || !world->GameState || !world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
            return nullptr;

        return static_cast<SDK::ACrGameStateBase*>(world->GameState)->MapMenuDataReplicationHelper;
    }

    // The colour the game paints the local player's arrow with: MarkerPlayer in
    // the map's marker colour asset, which the map settings point to. The asset
    // is read through the settings' soft pointer, which holds it only once the
    // game has loaded it, so it is checked every tick and nothing is kept.
    bool ReadPlayerMarkerColour(SDK::UCrMapMenuDevSettings* settings, float* linearRgb)
    {
        __try
        {
            SDK::UCrMapMenuMarkerDefaultColorData* colours = settings->MarkerStatusesColorData.Get();
            if (!colours || !colours->IsA(SDK::UCrMapMenuMarkerDefaultColorData::StaticClass()))
                return false;

            linearRgb[0] = colours->MarkerPlayer.R;
            linearRgb[1] = colours->MarkerPlayer.G;
            linearRgb[2] = colours->MarkerPlayer.B;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    unsigned int ToImGuiColour(const float* linear)
    {
        unsigned int c[3];
        for (int i = 0; i < 3; ++i)
        {
            const float v = linear[i] <= 0.0f ? 0.0f : (linear[i] >= 1.0f ? 1.0f : linear[i]);
            const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            c[i] = static_cast<unsigned int>(s * 255.0f + 0.5f);
        }
        return 0xFF000000u | (c[2] << 16) | (c[1] << 8) | c[0];
    }

    // Publishes the snapshot, then shows or hides the widget to match.
    void Publish(const Snapshot& snapshot)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_snapshot = snapshot;
        }

        if (g_widgetShown == snapshot.valid)
            return;

        WidgetHandle widget = g_widget.load();
        IPluginSelf* self   = GetSelf();
        if (widget && self && self->hooks->UI)
        {
            self->hooks->UI->SetWidgetVisible(widget, snapshot.valid);
            g_widgetShown = snapshot.valid;
        }
    }

    void Hide()
    {
        Snapshot none = {};
        Publish(none);
    }

    void LogDisagreement(int checked)
    {
        const uint64_t now = GetTickCount64();
        if (now - g_lastLogMs < kLogIntervalMs)
            return;

        g_lastLogMs = now;
        LOG_INFO("DroneMapMarker: hidden, none of %d map markers sits within %.0f px of where the "
                 "terrain transform puts it", checked, kTolerancePx);
    }

    void TickImpl()
    {
        if (!IsLocalPlayerInDrone())
        {
            Hide();
            return;
        }

        SDK::UCrUW_MapMenu* map = ActiveLocalMap();
        SDK::UCrUW_MapMenuMapArea* area = map ? map->MapMenuMapArea : nullptr;
        SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj();
        if (!area || !area->MapMenuTerrain || !area->CanvasPanelMapArea || !settings)
        {
            Hide();
            return;
        }

        SDK::ACrPlayerControllerBase* pc = LocalPlayerController();
        DroneState drone;
        Geo terrain, canvas;
        if (!ReadDrone(pc, &drone) || !SampleGeo(area->MapMenuTerrain, pc, &terrain) ||
            !SampleGeo(area->CanvasPanelMapArea, pc, &canvas) || !LaidOut(terrain) || !LaidOut(canvas))
        {
            Hide();
            return;
        }

        const Transform t = MakeTransform(terrain, settings->MapAreaPivotPoint.X, settings->MapAreaPivotPoint.Y);

        // Hidden when markers the game placed are not where the transform says;
        // with none to compare against, the transform stands.
        SDK::ACrMapMenuDataReplicationHelper* helper = ReplicationHelper();
        int checked = 0;
        int agreed  = 0;
        if (helper)
            CheckReferences(area, helper, pc, t, &checked, &agreed);
        if (checked > 0 && agreed == 0)
        {
            LogDisagreement(checked);
            Hide();
            return;
        }

        Snapshot s = {};
        s.valid     = true;
        s.x         = static_cast<float>(t.originX + drone.x * t.scale);
        s.y         = static_cast<float>(t.originY + drone.y * t.scale);
        s.yawDeg    = static_cast<float>(drone.yawDeg);
        s.dpi       = static_cast<float>(canvas.absSize[0] / canvas.localSize[0]);
        s.clip[0]   = static_cast<float>(canvas.topLeft[0]);
        s.clip[1]   = static_cast<float>(canvas.topLeft[1]);
        s.clip[2]   = static_cast<float>(canvas.topLeft[0] + canvas.absSize[0]);
        s.clip[3]   = static_cast<float>(canvas.topLeft[1] + canvas.absSize[1]);

        float linear[3];
        if (ReadPlayerMarkerColour(settings, linear))
        {
            s.colour = ToImGuiColour(linear);
        }
        else
        {
            s.colour = kFallbackColour;
            if (!g_loggedColourFallback)
            {
                g_loggedColourFallback = true;
                LOG_INFO("DroneMapMarker: the game's player marker colour is not loaded yet, using the default cyan");
            }
        }
        Publish(s);
    }

    // Apart from TickImpl: a function with __try may not hold objects that need
    // unwinding, and the code it calls may.
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
}

void InitDroneMapMarker(IPluginSelf* self)
{
    if (!self || !self->hooks || !self->hooks->UI)
        return;

    static const PluginWidgetDesc desc = { "BetterDrone Map Marker", &RenderMarker, &g_hints };
    WidgetHandle widget = self->hooks->UI->RegisterWidget(&desc);
    if (!widget)
    {
        LOG_WARN("DroneMapMarker: the loader would not register the overlay widget, no marker will show.");
        return;
    }

    self->hooks->UI->SetWidgetVisible(widget, false);
    g_widget.store(widget);
}

void ShutdownDroneMapMarker(IPluginSelf* self)
{
    // May run on the render thread: no game object is touched here.
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_snapshot = Snapshot{};
    }

    WidgetHandle widget = g_widget.exchange(nullptr);
    if (widget && self && self->hooks->UI)
    {
        self->hooks->UI->SetWidgetVisible(widget, false);
        self->hooks->UI->UnregisterWidget(widget);
    }

}

void TickDroneMapMarker(float)
{
    if (g_failed || !g_widget.load())
        return;

    // Fail closed: a fault while reading the map stops the marker for the rest
    // of the session instead of repeating every tick.
    if (!RunGuarded())
    {
        g_failed = true;
        Hide();
        LOG_ERROR("DroneMapMarker: access violation while reading the map; marker switched off until RELOAD");
    }
}
