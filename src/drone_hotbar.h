#pragma once
#include <plugin_interface.h>

// Ctrl and the mouse wheel switch the hotbar page while the drone is out, the
// way Ctrl and the wheel do on foot. The drone's own input has no wheel, so
// the loader's wheel callback takes the notch and the game tick makes the
// call. Only the wheel is taken here; Ctrl itself still reaches the game.

// Start and stop listening for the wheel. Call from PluginInit and
// PluginShutdown when DroneFields::Features::HotbarPage is on.
bool InitDroneHotbar(IPluginSelf* self);
void ShutdownDroneHotbar(IPluginSelf* self);
