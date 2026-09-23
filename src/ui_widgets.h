#pragma once

#include "plugin_interface.h"

// Small custom widgets. The loader embeds Material Icons and, under ImGui
// 1.92, loads glyphs on demand -- any codepoint in that font renders from a
// plugin with no atlas setup (see MaterialIcons.md in the ModLoader repo).
namespace BetterDrone::UI
{
	// A "revert to default" icon button drawing the replay glyph (U+E042).
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

		// replay.
		const char* glyph = "\xEE\x81\x82";
		float textW = 0.0f, textH = 0.0f;
		imgui->CalcTextSize(glyph, &textW, &textH, false, -1.0f);
		imgui->DL_AddText(dl, x + (size - textW) * 0.5f, y + (size - textH) * 0.5f, col, glyph);

		return pressed;
	}
}
