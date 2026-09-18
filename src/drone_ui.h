#pragma once
#include <plugin_interface.h>

void InitDroneUI(IPluginSelf* self);
void ShutdownDroneUI(IPluginSelf* self);
void ToggleDroneMenu();
void RebindToggleKey();
void RenderDronePanel(IModLoaderImGui* imgui);
