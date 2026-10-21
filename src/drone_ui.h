#pragma once
#include <plugin_interface.h>

void InitDroneUI(IPluginSelf* self);
void ShutdownDroneUI(IPluginSelf* self);
void ToggleDroneMenu();
void RebindToggleKey();
void RenderDronePanel(IModLoaderImGui* imgui);

// `cmPerSec` in the panel's Speed Display Unit. Writes the unit's name
// ("km/h", "mph" or "cm/s") to `unit`. Any thread.
float SpeedInPanelUnit(float cmPerSec, char* unit, int unitSize);

// The "Previous Preset Key" / "Next Preset Key" hotkeys: while the drone is
// out they step the speed presets slowest to fastest, with a brief toast.
void InitDronePresetKeys(IPluginSelf* self);
void ShutdownDronePresetKeys(IPluginSelf* self);

// Applies a pending key press. Must be called from the game tick
// (OnEngineTick).
void TickDronePresetKeys();

// Applies a pending Escape/Q close request. Must be called from the game
// tick (OnEngineTick), never from the render callback -- see its definition
// for why.
void TickDroneMenuClose();

// Applies a pending open request from the pause menu row. Must be called from
// the game tick (OnEngineTick).
void TickDroneMenuOpen();
