#include "drone_heat_display.h"
#include "overlay_widget.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <mutex>

namespace
{
    struct Snapshot
    {
        bool                valid;
        DroneHeatRing::View view;
    };

    std::mutex g_lock;
    Snapshot   g_snapshot = {};

    OverlayWidget g_overlay;

    // Render thread. The crosshair is the middle of the screen: the laser
    // traces along the drone camera's centre line.
    void RenderRing(IModLoaderImGui* ui)
    {
        Snapshot s;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            s = g_snapshot;
        }
        if (!s.valid)
            return;

        float width = 0.0f, height = 0.0f;
        ui->GetDisplaySize(&width, &height);
        if (width <= 0.0f || height <= 0.0f)
            return;

        DroneHeatRing::Draw(ui, ui->GetForegroundDrawList(), 0.5f * width, 0.5f * height, s.view, ui->GetTime());
    }
}

void InitDroneHeatDisplay(IPluginSelf* self)
{
    if (!self || !self->hooks || !self->hooks->UI)
        return;

    if (!g_overlay.Register(self, "BetterDrone Laser Heat", &RenderRing))
        LOG_WARN("DroneLaser: the loader would not register the overlay widget, the heat ring will not show.");
}

void ShutdownDroneHeatDisplay(IPluginSelf* self)
{
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_snapshot = Snapshot{};
    }

    g_overlay.Unregister(self);
}

void PublishDroneHeat(const DroneHeatRing::View* view)
{
    const bool show = view != nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_snapshot.valid = show;
        if (view)
            g_snapshot.view = *view;
    }

    g_overlay.SetShown(show);
}
