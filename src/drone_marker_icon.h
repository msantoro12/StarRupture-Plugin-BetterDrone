#pragma once
#include <plugin_interface.h>
#include <cmath>

// The drone's marker on the map: the drone's HUD reticle in miniature, drawn
// with draw-list primitives so no texture is needed.
//
//   * two side arcs of one circle, each about 95 degrees, centred on the left
//     and right sides, with a small hollow diamond at each of the four ends;
//   * four small wedges at the corners of an inner square, pointing inward;
//   * an arrowhead outside the arcs, showing the heading;
//   * no centre dot.
//
// Each arc stops at the tip of its diamond, never inside it: its end is pulled
// back along the circle by the diamond's half-diagonal, measured as an angle.
// The whole marker turns with the heading. Render thread only.

namespace DroneMarkerIcon
{
    // headingYawDeg is the drone's world yaw. On the map, world X runs to the
    // right and world Y down the screen, so yaw 0 points right and a larger yaw
    // turns clockwise. dpi is the map's pixel scale (canvas size in pixels over
    // its size in widget units). The game's own markers (the player arrow, the
    // building icons) sit in a 32 widget unit square, 48 px at 1.5. They merge
    // by screen distance as the map zooms, which points to a fixed size, so the
    // marker is sized in widget units and does not scale with zoom.
    inline void Draw(IModLoaderImGui* ui, PluginDrawList dl, float cx, float cy, float headingYawDeg,
                     float dpi, unsigned int col)
    {
        constexpr float kPi = 3.14159265f;

        // In widget units: the arcs and diamonds span 29.7 of the game's 32 unit
        // icon square, and the line weight keeps its proportion to the rest.
        const float w      = 5.2f * dpi;        // half-width of the wedges' square
        const float radius = 2.2f * w;          // arcs
        const float half   = 47.5f * kPi / 180; // each arc spans 95 degrees
        const float diamond = 0.3f * radius;    // diamond half-diagonal
        const float thick  = 0.275f * w;
        const float cap    = 0.5f * thick;

        // Local axes: x right, y down, up (-y) is the heading. Turned clockwise
        // by yaw + 90 degrees, so yaw 0 puts "up" on the +X side.
        const float theta = (headingYawDeg + 90.0f) * kPi / 180;
        const float cs = std::cos(theta);
        const float sn = std::sin(theta);

        auto px = [&](float x, float y) { return cx + x * cs - y * sn; };
        auto py = [&](float x, float y) { return cy + x * sn + y * cs; };

        // Arcs. Local angle 0 is the right side, pi the left; the stroke stops
        // short of each end by the diamond's half-diagonal along the circle.
        const float trim = diamond / radius;
        for (int side = 0; side < 2; ++side)
        {
            const float centre = side == 0 ? 0.0f : kPi;

            ui->DL_PathClear(dl);
            ui->DL_PathArcTo(dl, cx, cy, radius, centre - half + trim + theta, centre + half - trim + theta, 16);
            ui->DL_PathStroke(dl, col, 0, thick);

            // The diamonds sit on the true ends; the first one gets a round cap
            // where the stroke meets its tip.
            for (int end = -1; end <= 1; end += 2)
            {
                const float a  = centre + end * half;
                const float ca = std::cos(a);
                const float sa = std::sin(a);
                const float ox = radius * ca;
                const float oy = radius * sa;
                const float tx = -sa; // tangent
                const float ty = ca;

                ui->DL_AddQuad(dl,
                    px(ox + tx * diamond, oy + ty * diamond), py(ox + tx * diamond, oy + ty * diamond),
                    px(ox + ca * diamond, oy + sa * diamond), py(ox + ca * diamond, oy + sa * diamond),
                    px(ox - tx * diamond, oy - ty * diamond), py(ox - tx * diamond, oy - ty * diamond),
                    px(ox - ca * diamond, oy - sa * diamond), py(ox - ca * diamond, oy - sa * diamond),
                    col, thick);

                // Stroke end: on the tip that faces the arc's middle.
                const float tipx = ox - end * tx * diamond;
                const float tipy = oy - end * ty * diamond;
                ui->DL_AddCircleFilled(dl, px(tipx, tipy), py(tipx, tipy), cap, col, 8);
            }
        }

        // Wedges: a small triangle at each corner of the square, base on the
        // corner, apex toward the centre.
        for (int sx = -1; sx <= 1; sx += 2)
        {
            for (int sy = -1; sy <= 1; sy += 2)
            {
                const float ax = sx * 0.3f * w, ay = sy * 0.3f * w;
                const float b1x = sx * w,        b1y = sy * 0.55f * w;
                const float b2x = sx * 0.55f * w, b2y = sy * w;
                ui->DL_AddTriangleFilled(dl, px(ax, ay), py(ax, ay), px(b1x, b1y), py(b1x, b1y),
                    px(b2x, b2y), py(b2x, b2y), col);
            }
        }

        // Heading arrowhead, outside the arcs and their diamonds.
        const float baseY = -(radius + diamond + 0.3f * w);
        const float tipY  = baseY - 1.7f * w;
        const float hw    = 0.9f * w;
        ui->DL_AddTriangleFilled(dl, px(0, tipY), py(0, tipY), px(-hw, baseY), py(-hw, baseY),
            px(hw, baseY), py(hw, baseY), col);
    }
}
