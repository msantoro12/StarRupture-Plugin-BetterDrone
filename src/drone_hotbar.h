#pragma once
#include <plugin_interface.h>

// Ctrl and the mouse wheel change the character's hotbar while the drone is
// out, the way the wheel does on foot.

// Resolves the game's weapon-cycle call. Call in OnPluginLoadHooks.
void ResolveDroneHotbar(IPluginSelf* self, IPluginHookScanner* scanner);
