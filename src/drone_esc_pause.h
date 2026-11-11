#pragma once
#include <plugin_interface.h>

// Esc opens the game's pause menu while the drone is out, instead of ending
// the drone. Off by default (Interaction > Esc Opens Pause Menu In Drone Mode).
//
// ACrPlayerControllerBase::NativeOnInputCloseInGameMenu ends the drone before
// it closes the open widgets, and it is the handler the close-menu input
// reaches. The drone input mapping could not be read, so which handlers the
// drone's Esc reaches is not proven (the cancel-tool handler is covered too);
// the in-game checks and the LOG_DEBUG lines settle it. Esc while a building
// is placed, the drone toggle and an equip change are other ways the game ends
// the drone and are not touched.
//
// A hook on the close-menu handler takes the call when the option is on, the
// drone is out and the Esc key itself was pressed in this game tick or the
// last (the loader's keybind dispatch counts the press), and opens the pause
// menu through the controller's reflected SetOpenGameMenu(true), the call the
// game's own Esc makes on foot. Any other call, Q included, goes to the game's
// handler as before; a Q press also clears the Esc press. The pause menu
// closing is the game's own: the widget calls SetOpenGameMenu(false), and the
// drone was never touched.
//
// The hook is installed whenever the version map has the row, option on or
// off; with it off every call passes straight to the game.

// Resolves the game's close-menu handler. Call in OnPluginLoadHooks.
void ResolveDroneEscPause(IPluginSelf* self, IPluginHookScanner* scanner);

// Install the hook and start listening for Esc. Call from PluginInit and
// PluginShutdown when DroneFields::Features::EscPause is on.
bool InitDroneEscPause(IPluginSelf* self);
void ShutdownDroneEscPause(IPluginSelf* self);

// Game thread only. For the cancel-tool detour: true when this cancel is the
// Esc key in the drone with the option on, and the pause menu was opened in
// its place. The caller then skips the game's cancel. Only reached when the
// cancel-tool hook is installed, which is the BuildMenu feature's.
bool DroneEscPauseTakeCancel(void* character);
