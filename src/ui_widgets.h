#pragma once

#include "plugin_interface.h"

// Small custom widgets.
namespace BetterDrone::UI
{
	// A "revert to default" icon button drawing the Material Icons replay arrow.
	// Returns true on click. `size` is the square side in pixels;
	// pass ImGui's frame height for a control that lines up with a
	// checkbox. `id` must be unique per call -- ImGui derives a widget's
	// identity from it, same as any other id string.
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
		const unsigned int col = (active || hovered) ? 0xFFFFFFFFu   // white while held or hovered
		                                              : 0xFFB0B0B0u; // muted grey at rest

		PluginDrawList dl = imgui->GetWindowDrawList();
		if (!dl)
			return pressed;

		// The arrow is drawn from primitives on the replay glyph's own 24-unit
		// design grid (Material Icons U+E042, MaterialIcons.md) instead of placing
		// the glyph as text. Text placement centers the font's line box, but the
		// merged icon font's vertical metrics sit on the base font's ascent, so
		// the glyph's ink lands off the button's center by an amount that changes
		// with the base font and that a plugin can't query. Drawing the shape
		// puts its ink box exactly on the center, whatever the font.
		//
		// The shape is a 270 degree ring (center 12,13, radius 7, 2 wide, open at
		// the top left) plus a triangular head. Its ink box is x 4..20, y 1..21,
		// so grid point (12,11) goes on the button's center. The glyph's advance
		// is one 24-unit em, which gives the pixel size of a unit and keeps the
		// arrow the size the glyph was.
		float emW = 0.0f, emH = 0.0f;
		imgui->CalcTextSize("\xEE\x81\x82", &emW, &emH, false, -1.0f);
		const float u  = emW / 24.0f;
		const float ox = x + size * 0.5f - 12.0f * u;
		const float oy = y + size * 0.5f - 11.0f * u;

		imgui->DL_AddTriangleFilled(dl, ox + 12.0f * u, oy + 1.0f * u,
		                                ox +  7.0f * u, oy + 6.0f * u,
		                                ox + 12.0f * u, oy + 11.0f * u, col);
		imgui->DL_PathClear(dl);
		imgui->DL_PathArcTo(dl, ox + 12.0f * u, oy + 13.0f * u, 7.0f * u,
		                    -1.5707964f, 3.1415927f, 0);
		imgui->DL_PathStroke(dl, col, 0, 2.0f * u);

		return pressed;
	}
}
