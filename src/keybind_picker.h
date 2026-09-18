#pragma once

#include <plugin_interface.h>
#include <cstddef>

namespace BetterDrone::Keybind
{
	// Draws the current combo plus a rebind button. Returns true on the frame the
	// user finishes picking a new combo, with newCombo filled in.
	bool RenderPicker(IModLoaderImGui* imgui,
	                  const char* id,
	                  const char* currentCombo,
	                  char* newCombo,
	                  size_t newComboSize);

	// True while a picker is waiting for a key press.
	bool IsCapturing();

	// Drops any in-progress capture. Call when the menu closes.
	void CancelCapture();
}
