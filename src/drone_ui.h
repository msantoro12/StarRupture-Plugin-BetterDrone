#pragma once
#include <plugin_interface.h>

void InitDroneUI(IPluginSelf* self);
void ShutdownDroneUI(IPluginSelf* self);
void ToggleDroneMenu();
void RebindToggleKey();
void RenderDronePanel(IModLoaderImGui* imgui);

// Applies a pending Escape/Q close request. Must be called from the game
// tick (OnEngineTick), never from the render callback -- see its definition
// for why.
void TickDroneMenuClose();
