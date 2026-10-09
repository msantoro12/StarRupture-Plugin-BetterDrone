#pragma once
#include <plugin_interface.h>
#include <cmath>

// The mining laser's heat, as a thin ring around the crosshair, drawn with
// draw-list primitives in the same way as the drone's map marker.
//
//   * a faint full ring is the track;
//   * an arc from 12 o'clock, clockwise, fills with the heat, in the player's
//     map marker colour up to half, then through orange to red near the top;
//   * from 85% an outer ring pulses as a warning;
//   * while cooling holds before it drains, the arc dims; while it drains,
//     the arc shrinks back;
//   * overheated, the arc and the outer ring are red and flash until the heat
//     is gone.
//
// Render thread only: everything it needs is in View.

namespace DroneHeatRing
{
    enum class Phase
    {
        Heating,    // the laser is firing
        Holding,    // released, waiting out the cooling delay
        Draining,   // cooling
    };

    struct View
    {
        float        heat;         // 0..1
        Phase        phase;
        bool         overheated;   // locked out until cold
        float        scale;        // the game's UI scale (DPI scale)
        unsigned int baseColour;   // ImGui colour, opaque
    };

    // Sized in the game's UI units, so it keeps its place outside the
    // crosshair at any resolution.
    constexpr float kRadius    = 26.0f;
    constexpr float kThickness = 2.0f;

    constexpr unsigned int kOrange = 0xFF2090FFu;   // #FF9020
    constexpr unsigned int kRed    = 0xFF2A3AFFu;   // #FF3A2A

    inline unsigned int Lerp(unsigned int a, unsigned int b, float t)
    {
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        unsigned int out = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            const float ca = static_cast<float>((a >> shift) & 0xFFu);
            const float cb = static_cast<float>((b >> shift) & 0xFFu);
            out |= static_cast<unsigned int>(ca + (cb - ca) * t + 0.5f) << shift;
        }
        return out;
    }

    inline unsigned int WithAlpha(unsigned int col, float alpha)
    {
        alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
        return (col & 0x00FFFFFFu) | (static_cast<unsigned int>(alpha * 255.0f + 0.5f) << 24);
    }

    // The fill colour for a heat level.
    inline unsigned int HeatColour(unsigned int base, float heat)
    {
        if (heat <= 0.5f)
            return base;
        if (heat <= 0.75f)
            return Lerp(base, kOrange, (heat - 0.5f) / 0.25f);
        return Lerp(kOrange, kRed, (heat - 0.75f) / 0.15f);
    }

    // timeSeconds drives the pulse and the flash.
    inline void Draw(IModLoaderImGui* ui, PluginDrawList dl, float cx, float cy, const View& v, double timeSeconds)
    {
        constexpr float kPi = 3.14159265f;

        const float radius = kRadius * v.scale;
        const float thick  = kThickness * v.scale;
        const float heat   = v.heat < 0.0f ? 0.0f : (v.heat > 1.0f ? 1.0f : v.heat);
        const float t      = static_cast<float>(std::fmod(timeSeconds, 1000.0));

        // Three flashes a second while overheated; a slower pulse as a warning.
        const float flash = 0.5f + 0.5f * std::cos(2.0f * kPi * 3.0f * t);
        const float pulse = 0.5f + 0.5f * std::sin(2.0f * kPi * 1.5f * t);

        unsigned int fill  = v.overheated ? kRed : HeatColour(v.baseColour, heat);
        float        alpha = 1.0f;
        if (v.overheated)
            alpha = 0.45f + 0.55f * flash;
        else if (v.phase == Phase::Holding)
            alpha = 0.6f;
        else if (v.phase == Phase::Draining)
            alpha = 0.85f;

        ui->DL_AddCircle(dl, cx, cy, radius, WithAlpha(v.overheated ? kRed : v.baseColour, 0.22f), 64, thick);

        if (heat > 0.0f)
        {
            const float start = -0.5f * kPi;
            const int segments = 4 + static_cast<int>(60.0f * heat);
            ui->DL_PathClear(dl);
            ui->DL_PathArcTo(dl, cx, cy, radius, start, start + 2.0f * kPi * heat, segments);
            ui->DL_PathStroke(dl, WithAlpha(fill, alpha), 0, thick);
        }

        if (v.overheated || heat >= 0.85f)
        {
            const float ringAlpha = v.overheated ? 0.15f + 0.65f * flash : 0.25f + 0.45f * pulse;
            ui->DL_AddCircle(dl, cx, cy, radius + 2.5f * thick, WithAlpha(kRed, ringAlpha), 64, 0.75f * thick);
        }
    }
}
