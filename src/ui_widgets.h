#pragma once

#include "plugin_interface.h"

#include <cmath>

// Small custom widgets drawn through the ImDrawList API rather than the font.
// The loader's ImGui font has no guaranteed glyph range beyond ASCII -- nothing
// in the plugin ships a non-ASCII UI string -- so an icon like U+21BA cannot be
// relied on to render. These draw the shape directly instead.
namespace BetterDrone::UI
{
	// A circular "revert to default" arrow: a near-full arc with an arrowhead on
	// the leading end. Returns true on click. `size` is the square side in pixels;
	// pass ImGui's frame height for a control that lines up with a checkbox.
	inline bool ResetButton(IModLoaderImGui* imgui, const char* id, float size = 0.0f)
	{
		if (size <= 0.0f)
			size = imgui->GetFrameHeight ? imgui->GetFrameHeight() : 18.0f;

		float x = 0.0f, y = 0.0f;
		imgui->GetCursorScreenPos(&x, &y);

		const bool pressed = imgui->InvisibleButton(id, size, size);
		const bool hovered = imgui->IsItemHovered();
		const bool active  = imgui->IsItemActive();

		// ImGui packs colours as 0xAABBGGRR.
		const unsigned int col = active  ? 0xFFFFFFFFu     // white while held
		                       : hovered ? 0xFFFFFFFFu
		                                 : 0xFFB0B0B0u;    // muted grey at rest

		PluginDrawList dl = imgui->GetWindowDrawList();
		if (!dl)
			return pressed;

		const float cx = x + size * 0.5f;
		const float cy = y + size * 0.5f;
		const float r  = size * 0.30f;

		// Arc swept anticlockwise, stopping short of a full turn so the gap reads
		// as motion rather than a plain circle.
		const float aMin = 0.70f;
		const float aMax = 5.75f;
		imgui->DL_PathArcTo(dl, cx, cy, r, aMin, aMax, 20);
		imgui->DL_PathStroke(dl, col, 0, size * 0.10f);

		// Arrowhead on the arc's leading end, pointing along the tangent.
		const float ex = cx + r * std::cos(aMax);
		const float ey = cy + r * std::sin(aMax);
		const float tx = -std::sin(aMax);          // unit tangent
		const float ty =  std::cos(aMax);
		const float nx = std::cos(aMax);           // unit normal (outward)
		const float ny = std::sin(aMax);
		const float h  = size * 0.22f;             // head length
		const float w  = size * 0.13f;             // half-width

		imgui->DL_AddTriangleFilled(dl,
			ex + tx * h,          ey + ty * h,
			ex + nx * w,          ey + ny * w,
			ex - nx * w,          ey - ny * w,
			col);

		return pressed;
	}
}
