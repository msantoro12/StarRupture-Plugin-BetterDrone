#pragma once
#include <plugin_interface.h>

// Ctrl and the mouse wheel change the character's hotbar while the drone is
// out, the way the wheel does on foot. The drone's own input has no wheel, so
// the loader's wheel callback takes the notch and the game tick makes the
// call. Holding Ctrl still slows the drone: only the wheel is taken here.

// Resolves the game's weapon-cycle call. Call in OnPluginLoadHooks.
void ResolveDroneHotbar(IPluginSelf* self, IPluginHookScanner* scanner);

// Start and stop listening for the wheel. Call from PluginInit and
// PluginShutdown when DroneFields::Features::HotbarCycle is on.
bool InitDroneHotbar(IPluginSelf* self);
void ShutdownDroneHotbar(IPluginSelf* self);
