#include "drone_heat_display.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <atomic>
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

    std::atomic<WidgetHandle> g_widget{ nullptr };
    bool g_widgetShown = false;   // game thread only

    // A transparent window the draw list rides on, as the map marker's: no
    // title, no input, no background, shown only while there is a ring.
    constexpr int kWindowFlagAlwaysAutoResize = 1 << 6;
    PluginWindowHints g_hints = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0,
        PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoResize | PluginWindowFlags_NoMove |
        PluginWindowFlags_NoScrollbar | PluginWindowFlags_NoBackground | PluginWindowFlags_NoSavedSettings |
        PluginWindowFlags_NoMouseInputs | kWindowFlagAlwaysAutoResize
    };

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

    static const PluginWidgetDesc desc = { "BetterDrone Laser Heat", &RenderRing, &g_hints };
    WidgetHandle widget = self->hooks->UI->RegisterWidget(&desc);
    if (!widget)
    {
        LOG_WARN("DroneLaser: the loader would not register the overlay widget, the heat ring will not show.");
        return;
    }

    self->hooks->UI->SetWidgetVisible(widget, false);
    g_widget.store(widget);
}

void ShutdownDroneHeatDisplay(IPluginSelf* self)
{
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

void PublishDroneHeat(const DroneHeatRing::View* view)
{
    const bool show = view != nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_snapshot.valid = show;
        if (view)
            g_snapshot.view = *view;
    }

    if (g_widgetShown == show)
        return;

    WidgetHandle widget = g_widget.load();
    IPluginSelf* self   = GetSelf();
    if (widget && self && self->hooks->UI)
    {
        self->hooks->UI->SetWidgetVisible(widget, show);
        g_widgetShown = show;
    }
}
