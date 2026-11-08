#include "drone_map_marker.h"
#include "drone_interact.h"
#include "drone_marker_icon.h"
#include "object_ref.h"
#include "overlay_widget.h"
#include "plugin_helpers.h"
#include "drone_fields.h"
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

    // Following the drone. The map is moved by a share of the drone's distance
    // from the canvas centre each tick, not all of it: the cached geometry that
    // distance is measured from can trail a write by a frame or two, and a full
    // correction on a trailing measurement overshoots.
    constexpr double kFollowGain = 0.5;

    // A move of the terrain's slot under this (widget units) is not worth a write.
    constexpr double kMinMove = 0.1;

    // The slot reads back as it was left within this (widget units, floats).
    constexpr double kSlotTolerance = 0.5;

    // A correction of at least this many pixels has to show in the terrain's
    // geometry within kMaxMisses ticks, by a quarter to two and a half times
    // what was asked (the geometry can trail a write, so one move may land
    // together with the next).
    constexpr double kCheckPx   = 8.0;
    constexpr int    kMaxMisses = 3;

    // For this many ticks after follow starts, a slot the game moved itself
    // (focusing the player, restoring a saved pan, a resize) is followed from
    // where it is instead of ending follow.
    constexpr int kSettleTicks = 15;

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

    OverlayWidget g_overlay;

    // Tick only. A marker that agreed last tick is tried first. Handle numbers,
    // not pointers: the widget is looked up again each tick.
    int32_t  g_refIndex  = -1;
    int32_t  g_refSerial = 0;
    uint64_t g_lastLogMs = 0;
    bool     g_failed    = false;
    bool     g_loggedColourFallback = false;

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
    // world point. -1 when it is hidden, not laid out or where the transform puts
    // it outside the canvas (Slate does not paint it there, so its geometry is
    // whatever it last was), else 1 when it is where the transform says and 0
    // when it is not.
    int CompareReference(SDK::UCrUW_MapMenuMarker* marker, double worldX, double worldY, SDK::UObject* ctx,
                         const Transform& t, const Geo& canvas)
    {
        const double expectedX = t.originX + worldX * t.scale;
        const double expectedY = t.originY + worldY * t.scale;
        if (expectedX < canvas.topLeft[0] || expectedX > canvas.topLeft[0] + canvas.absSize[0] ||
            expectedY < canvas.topLeft[1] || expectedY > canvas.topLeft[1] + canvas.absSize[1])
            return -1;

        Geo g;
        if (!marker || !SampleGeo(marker, ctx, &g) || !g.visible || !LaidOut(g))
            return -1;

        const double dx = expectedX - g.topLeft[0];
        const double dy = expectedY - g.topLeft[1];
        return std::fabs(dx) <= kTolerancePx && std::fabs(dy) <= kTolerancePx ? 1 : 0;
    }

    // Looks at up to kMaxReferences building markers the game shows and counts
    // how many sit where the transform puts them.
    void CheckReferences(SDK::UCrUW_MapMenuMapArea* area, SDK::ACrMapMenuDataReplicationHelper* helper,
                         SDK::UObject* ctx, const Transform& t, const Geo& canvas, int* checked, int* agreed)
    {
        *checked = 0;
        *agreed  = 0;
        __try
        {
            auto* widgetsAt   = DroneFields::MapMassMarkers.In(area);
            auto* containerAt = DroneFields::BuildingsMarkers.In(helper);
            auto* dataAt      = containerAt ? DroneFields::BuildingsMarkerData.In(containerAt) : nullptr;
            if (!widgetsAt || !dataAt)
                return;

            auto& widgets = *widgetsAt;
            const int32_t allocated = widgets.NumAllocated();
            auto& data = *dataAt;
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
                        const uint8_t* b = PluginMap::ElementAt(data, d, DroneFields::MarkerItemSize);
                        const auto* bHandle  = DroneFields::MarkerBuildingHandle.In(b);
                        const auto* location = DroneFields::MarkerLocation.In(b);
                        auto*       marker   = DroneFields::MarkerWidget.In(&widgets[i].Value());
                        if (!bHandle || !location || !marker)
                            continue;
                        if (bHandle->Index != handle.Index || bHandle->SerialNumber != handle.SerialNumber)
                            continue;

                        const int r = CompareReference(*marker, location->X, location->Y, ctx, t, canvas);
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
            SDK::ACrCharacterPlayerBase* character = DroneFields::ValueOr(DroneFields::CrChar, pc);
            SDK::ACrCharacterDroneBase*  drone     = DroneFields::ValueOr(DroneFields::BuildingDrone, character);
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
        SDK::UWorld* world = PluginSdkRuntime::GetWorld();
        if (!world || !world->GameState || !world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
            return nullptr;

        return DroneFields::ValueOr(DroneFields::MapDataHelper, static_cast<SDK::ACrGameStateBase*>(world->GameState));
    }

    // The colour of the local player's arrow: MarkerPlayer in the map's marker
    // colour asset, which the map settings point to (assumed from the asset's
    // field names, not read from the arrow widget). The asset is read through
    // the settings' soft pointer, which holds it only once the game has loaded
    // it, so it is checked every tick and nothing is kept.
    bool ReadPlayerMarkerColour(SDK::UCrMapMenuDevSettings* settings, float* linearRgb)
    {
        __try
        {
            auto* soft = DroneFields::MarkerColorData.In(settings);
            SDK::UCrMapMenuMarkerDefaultColorData* colours = soft ? soft->Get() : nullptr;
            if (!colours || !colours->IsA(SDK::UCrMapMenuMarkerDefaultColorData::StaticClass()))
                return false;

            const auto* player = DroneFields::MarkerPlayerColour.In(colours);
            if (!player)
                return false;

            linearRgb[0] = player->R;
            linearRgb[1] = player->G;
            linearRgb[2] = player->B;
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

        g_overlay.SetShown(snapshot.valid);
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

    // ---- following the drone ---------------------------------------------------
    //
    // While the map is open in drone mode, the terrain widget's canvas slot is
    // moved so the drone sits at the canvas centre. The drone's pixel is measured
    // again every tick, so the loop corrects itself and no map maths is needed.
    // It stops, for the rest of that open, when the slot is somewhere this code
    // did not leave it (a hand pan, the game's own "Center on player"), when a
    // write does not move the map the way it was asked to, or when the game's
    // markers no longer sit where the terrain says. When it ends because the
    // drone is gone or the map closed, the slot is put back where the game had it.

    struct Follow
    {
        bool   stopped;              // off for this open
        bool   haveSlot;             // the fields below hold last tick's state
        bool   wrote;                // last tick moved the slot
        double expectX, expectY;     // where the slot should be now: what this code left in it
        double beforeX, beforeY;     // where it was before the last write
        double scale;                // the terrain's render scale, which is the zoom
        double topLeftX, topLeftY;   // the terrain's geometry last tick, pixels
        double pendingX, pendingY;   // the pixel move the last write asked for
        double appliedX, appliedY;   // everything written this open, widget units
        int    misses;
        int    ticks;                // ticks since follow started, up to kSettleTicks
    };

    Follow                  g_follow = {};
    ObjectRef<SDK::UWidget> g_followTerrain;

    struct SlotRead
    {
        double x, y;   // the slot's position in the canvas, widget units
        double scale;  // the widget's render scale
        bool   point;  // anchored to a point, so SetPosition translates the widget
    };

    // The terrain's canvas slot, or null. Inside the callers' __try.
    SDK::UCanvasPanelSlot* CanvasSlot(SDK::UWidget* widget)
    {
        SDK::UPanelSlot* slot = widget->Slot;
        if (!slot || !slot->IsA(SDK::UCanvasPanelSlot::StaticClass()))
            return nullptr;

        return static_cast<SDK::UCanvasPanelSlot*>(slot);
    }

    bool ReadSlot(SDK::UWidget* widget, SlotRead* out)
    {
        __try
        {
            SDK::UCanvasPanelSlot* slot = CanvasSlot(widget);
            if (!slot)
                return false;

            const SDK::FVector2D position = slot->GetPosition();
            out->x     = position.X;
            out->y     = position.Y;
            out->scale = widget->RenderTransform.Scale.X;
            const SDK::FAnchors& anchors = slot->LayoutData.Anchors;
            out->point = anchors.Minimum.X == anchors.Maximum.X && anchors.Minimum.Y == anchors.Maximum.Y;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Sets the slot's position through the UFunction, so the layout updates, and
    // reads it back.
    bool WriteSlot(SDK::UWidget* widget, double x, double y, SlotRead* readBack)
    {
        __try
        {
            SDK::UCanvasPanelSlot* slot = CanvasSlot(widget);
            if (!slot)
                return false;

            SDK::FVector2D position;
            position.X = x;
            position.Y = y;
            slot->SetPosition(position);

            const SDK::FVector2D now = slot->GetPosition();
            readBack->x     = now.X;
            readBack->y     = now.Y;
            readBack->scale = widget->RenderTransform.Scale.X;
            readBack->point = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void StopFollow(const char* why)
    {
        if (g_follow.stopped)
            return;

        g_follow.stopped = true;
        LOG_INFO("DroneMapMarker: map follow off until the map is reopened: %s", why);
    }

    // Puts back what this code moved.
    void PutBack(SDK::UWidget* terrainWidget)
    {
        SlotRead now, back;
        if (ReadSlot(terrainWidget, &now))
            WriteSlot(terrainWidget, now.x - g_follow.appliedX, now.y - g_follow.appliedY, &back);
    }

    // Puts back what this code moved, then stops.
    void UndoFollow(SDK::UWidget* terrainWidget, const char* why)
    {
        if (g_follow.stopped || !g_follow.haveSlot || g_followTerrain.Get() != terrainWidget)
            return;

        PutBack(terrainWidget);
        StopFollow(why);
    }

    // Drops the state, touching no game object.
    void ForgetFollow()
    {
        g_follow = {};
        g_followTerrain.Reset();
    }

    // Ends follow wherever it stands (the drone was recalled, the map closed or
    // replaced, the world ended): the terrain goes back to where the game had
    // it, if the map is still alive. A stopped follow leaves the map where the
    // player or the game put it.
    void EndFollow()
    {
        SDK::UWidget* terrainWidget = g_followTerrain.Get();
        if (terrainWidget && g_follow.haveSlot && !g_follow.stopped)
            PutBack(terrainWidget);
        ForgetFollow();
    }

    void FollowDrone(SDK::UWidget* terrainWidget, const Geo& terrain, const Geo& canvas, double dronePxX,
                     double dronePxY)
    {
        // A new map widget is a new open.
        if (g_followTerrain.Get() != terrainWidget)
        {
            EndFollow();
            g_followTerrain.Set(terrainWidget);
        }

        Follow& f = g_follow;
        if (f.stopped)
            return;

        SlotRead slot;
        if (!ReadSlot(terrainWidget, &slot))
        {
            StopFollow("the map's terrain slot could not be read as a canvas slot");
            return;
        }

        if (!slot.point)
        {
            StopFollow("the map's terrain slot is stretched, not anchored to a point");
            return;
        }

        if (f.haveSlot)
        {
            // A zoom moves the slot too, to keep its own centre in place.
            const bool zoomed = std::fabs(slot.scale - f.scale) > 1e-4;
            if (!zoomed && (std::fabs(slot.x - f.expectX) > kSlotTolerance ||
                            std::fabs(slot.y - f.expectY) > kSlotTolerance))
            {
                // Early on, the game may still be setting the pan itself. Follow
                // it from here; what this code wrote is no longer in the slot, so
                // it is not put back later.
                if (f.ticks < kSettleTicks)
                {
                    f.appliedX = 0.0;
                    f.appliedY = 0.0;
                    f.wrote    = false;
                }
                else
                {
                    const bool movedBack = f.wrote && std::fabs(slot.x - f.beforeX) <= kSlotTolerance &&
                                           std::fabs(slot.y - f.beforeY) <= kSlotTolerance &&
                                           (std::fabs(f.expectX - f.beforeX) > 2.0 * kSlotTolerance ||
                                            std::fabs(f.expectY - f.beforeY) > 2.0 * kSlotTolerance);
                    StopFollow(movedBack ? "the game put the map back where it was" : "the map was moved by hand");
                    return;
                }
            }

            // A correction worth seeing has to show in the terrain's geometry.
            const double asked = std::sqrt(f.pendingX * f.pendingX + f.pendingY * f.pendingY);
            if (!zoomed && f.wrote && asked >= kCheckPx)
            {
                const double moved = ((terrain.topLeft[0] - f.topLeftX) * f.pendingX +
                                      (terrain.topLeft[1] - f.topLeftY) * f.pendingY) / asked;
                const double share = moved / asked;
                f.misses = share < 0.25 || share > 2.5 ? f.misses + 1 : 0;
                if (f.misses >= kMaxMisses)
                {
                    UndoFollow(terrainWidget, "a write does not move the map as asked");
                    return;
                }
            }
            else
            {
                f.misses = 0;
            }
        }

        // Widget units are canvas pixels divided by the DPI scale.
        const double dpi = canvas.absSize[0] / canvas.localSize[0];
        const double errX = (canvas.topLeft[0] + canvas.absSize[0] * 0.5 - dronePxX) * kFollowGain;
        const double errY = (canvas.topLeft[1] + canvas.absSize[1] * 0.5 - dronePxY) * kFollowGain;
        const double moveX = errX / dpi;
        const double moveY = errY / dpi;

        f.haveSlot = true;
        if (f.ticks < kSettleTicks)
            ++f.ticks;
        f.wrote    = false;
        f.scale    = slot.scale;
        f.topLeftX = terrain.topLeft[0];
        f.topLeftY = terrain.topLeft[1];
        f.expectX  = slot.x;
        f.expectY  = slot.y;

        if (std::fabs(moveX) < kMinMove && std::fabs(moveY) < kMinMove)
            return;

        SlotRead written;
        if (!WriteSlot(terrainWidget, slot.x + moveX, slot.y + moveY, &written) ||
            std::fabs(written.x - (slot.x + moveX)) > 0.05 || std::fabs(written.y - (slot.y + moveY)) > 0.05)
        {
            UndoFollow(terrainWidget, "the terrain's slot would not take the new position");
            return;
        }

        f.wrote    = true;
        f.beforeX  = slot.x;
        f.beforeY  = slot.y;
        f.expectX  = written.x;
        f.expectY  = written.y;
        f.pendingX = errX;
        f.pendingY = errY;
        f.appliedX += written.x - slot.x;
        f.appliedY += written.y - slot.y;
    }

    void TickImpl()
    {
        if (!IsLocalPlayerInDrone())
        {
            EndFollow();
            Hide();
            return;
        }

        SDK::UCrUW_MapMenu* map = ActiveLocalMap();
        if (!map)
            EndFollow();
        SDK::UCrUW_MapMenuMapArea* area = DroneFields::ValueOr(DroneFields::MapMenuMapArea, map);
        SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj();
        auto* terrainWidget = DroneFields::ValueOr(DroneFields::MapTerrain, area);
        auto* canvasWidget  = DroneFields::ValueOr(DroneFields::MapCanvasPanel, area);
        const auto* pivot   = DroneFields::MapPivotPoint.In(settings);
        if (!area || !terrainWidget || !canvasWidget || !pivot)
        {
            Hide();
            return;
        }

        SDK::ACrPlayerControllerBase* pc = LocalPlayerController();
        DroneState drone;
        Geo terrain, canvas;
        if (!ReadDrone(pc, &drone) || !SampleGeo(terrainWidget, pc, &terrain) ||
            !SampleGeo(canvasWidget, pc, &canvas) || !LaidOut(terrain) || !LaidOut(canvas))
        {
            Hide();
            return;
        }

        const Transform t = MakeTransform(terrain, pivot->X, pivot->Y);

        // Hidden when markers the game placed are not where the transform says;
        // with none to compare against, the transform stands.
        SDK::ACrMapMenuDataReplicationHelper* helper = ReplicationHelper();
        int checked = 0;
        int agreed  = 0;
        if (helper)
            CheckReferences(area, helper, pc, t, canvas, &checked, &agreed);
        if (checked > 0 && agreed == 0)
        {
            UndoFollow(terrainWidget, "the map's markers did not move with the terrain");
            LogDisagreement(checked);
            Hide();
            return;
        }

        FollowDrone(terrainWidget, terrain, canvas, t.originX + drone.x * t.scale, t.originY + drone.y * t.scale);

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

        s.colour    = PlayerMarkerColour();
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

unsigned int PlayerMarkerColour()
{
    float linear[3];
    SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj();
    if (settings && ReadPlayerMarkerColour(settings, linear))
        return ToImGuiColour(linear);

    if (!g_loggedColourFallback)
    {
        g_loggedColourFallback = true;
        LOG_INFO("DroneMapMarker: the game's player marker colour is not available, using the default cyan");
    }
    return kFallbackColour;
}

void InitDroneMapMarker(IPluginSelf* self)
{
    if (!self || !self->hooks || !self->hooks->UI)
        return;

    if (!g_overlay.Register(self, "BetterDrone Map Marker", &RenderMarker))
        LOG_WARN("DroneMapMarker: the loader would not register the overlay widget, no marker will show.");
}

void ShutdownDroneMapMarker(IPluginSelf* self)
{
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_snapshot = Snapshot{};
    }

    // Game objects are touched only on the game thread, which is where shutdown
    // runs on loader 1.22 and later; anywhere else the map is left as it is.
    if (OnGameThread())
        EndFollow();
    else
        ForgetFollow();

    g_overlay.Unregister(self);
}

void TickDroneMapMarker(float)
{
    if (g_failed || !g_overlay.IsRegistered())
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
