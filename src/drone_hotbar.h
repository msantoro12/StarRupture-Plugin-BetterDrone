#pragma once
#include <plugin_interface.h>

// The hotbar while the drone is out. Ctrl and the mouse wheel switch the hotbar
// page, the way Ctrl and the wheel do on foot. The number keys 0 to 9 pick
// slots 0 to 9 of the current page (key N is slot N), and the plain mouse
// wheel steps to the next or previous slot, but only building slots: a weapon slot would end
// the drone and a consumable would be used on the parked body. The drone's own
// input has none of these, so the loader's key and wheel callbacks take them
// and the game tick makes the call. Only the wheel and the digits are taken
// here; Ctrl itself still reaches the game. On foot, the same wheel pick works
// with the building tool out (build mode or deconstruct mode); everywhere else
// on foot the wheel and the number keys are the game's.

// Start and stop listening. Call from PluginInit and PluginShutdown when
// DroneFields::Features::HotbarPage or HotbarSelect is on; `page` and `select`
// say which of the two is.
bool InitDroneHotbar(IPluginSelf* self, bool page, bool select);
void ShutdownDroneHotbar(IPluginSelf* self);
