#include "drone_speedometer.h"
#include "drone_config.h"
#include "drone_heat_ring.h"
#include "drone_interact.h"
#include "drone_map_marker.h"
#include "drone_ui.h"
#include "object_ref.h"
#include "overlay_widget.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <UMG_classes.hpp>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{
    // The speed is sampled this often, never every frame.
    constexpr uint64_t kSampleMs = 100;

    // Each sample's weight in the shown speed. Enough to hide the
    // sample-to-sample jitter, little enough to follow a boost.
    constexpr float kSmoothing = 0.5f;

    // In the game's UI units, like the ring: the gap between the ring's outer
    // edge and the text, and the text's height.
    constexpr float kGap      = 6.0f;
    constexpr float kTextSize = 18.0f;

    constexpr unsigned int kOutline = 0xC0000000u;   // black, 75%

    // Handed from the tick to the render callback. A speed below zero shows
    // nothing.
    std::atomic<float>        g_speed{ -1.0f };   // cm/s, smoothed
    std::atomic<float>        g_scale{ 1.0f };    // the game's UI scale
    std::atomic<unsigned int> g_colour{ 0xFFFFFFFFu };

    // Game thread only.
    uint64_t g_nextSampleMs = 0;
    float    g_smoothed     = -1.0f;   // below 0 until a flight's first sample

    OverlayWidget g_overlay;

    // Render thread.
    void RenderSpeed(IModLoaderImGui* ui)
    {
        const float speed = g_speed.load();
        if (speed < 0.0f)
            return;

        float cx = 0.0f, cy = 0.0f;
        if (!DroneHeatRing::Crosshair(ui, &cx, &cy))
            return;

        char unit[16] = {};
        const float inUnit  = (std::min)(SpeedInPanelUnit(speed, unit, sizeof(unit)), 999999.0f);
        const float step    = inUnit >= 10000.0f ? 100.0f : inUnit >= 1000.0f ? 10.0f : 1.0f;
        const long  value   = static_cast<long>(std::round(inUnit / step) * step);

        char text[48];
        snprintf(text, sizeof(text), "%ld %s", value, unit);

        const float scale = g_scale.load();
        const float size  = kTextSize * scale;
        const float x     = cx + (DroneHeatRing::kOuterRadius + kGap) * scale;
        const float y     = cy - 0.5f * size;
        const float o     = (std::max)(1.0f, scale);

        PluginDrawList dl = ui->GetForegroundDrawList();
        ui->DL_AddTextSized(dl, size, x - o, y, kOutline, text);
        ui->DL_AddTextSized(dl, size, x + o, y, kOutline, text);
        ui->DL_AddTextSized(dl, size, x, y - o, kOutline, text);
        ui->DL_AddTextSized(dl, size, x, y + o, kOutline, text);
        ui->DL_AddTextSized(dl, size, x, y, g_colour.load(), text);
    }

    // Game thread.
    void Hide()
    {
        g_smoothed = -1.0f;
        g_speed.store(-1.0f);
        g_overlay.SetShown(false);
    }
}

void InitDroneSpeedometer(IPluginSelf* self)
{
    g_nextSampleMs = 0;
    g_smoothed     = -1.0f;
    g_speed.store(-1.0f);

    if (!g_overlay.Register(self, "BetterDrone Speedometer", &RenderSpeed))
        LOG_WARN("Speedometer: the loader would not register the overlay widget, the speedometer will not show.");
}

void ShutdownDroneSpeedometer(IPluginSelf* self)
{
    g_speed.store(-1.0f);
    g_overlay.Unregister(self);
}

void TickDroneSpeedometer()
{
    if (!g_overlay.IsRegistered())
        return;

    const uint64_t now = GetTickCount64();
    if (now < g_nextSampleMs)
        return;
    g_nextSampleMs = now + kSampleMs;

    SDK::ACrCharacterPlayerBase* character =
        DroneConfig::Config::ReadSpeedometer() && !IsGameMenuOpen() ? LocalPlayerCharacter() : nullptr;
    SDK::ACrCharacterDroneBase* drone =
        character && character->Status == SDK::EPlayerCharacterStatus::BuildingDrone ? character->BuildingDrone : nullptr;
    if (!drone || !ObjectRef<SDK::ACrCharacterDroneBase>(drone).Get())
    {
        Hide();
        return;
    }

    const SDK::FVector velocity = drone->GetVelocity();
    const float speed = static_cast<float>(
        std::sqrt(velocity.X * velocity.X + velocity.Y * velocity.Y + velocity.Z * velocity.Z));
    g_smoothed = g_smoothed < 0.0f ? speed : g_smoothed + (speed - g_smoothed) * kSmoothing;

    float scale = SDK::UWidgetLayoutLibrary::GetViewportScale(character);
    if (!(scale > 0.0f))
        scale = 1.0f;

    g_scale.store(scale);
    g_colour.store(PlayerMarkerColour());
    g_speed.store(g_smoothed);
    g_overlay.SetShown(true);
}
