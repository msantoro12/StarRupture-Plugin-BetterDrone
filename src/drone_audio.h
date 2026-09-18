#pragma once

#include <plugin_interface.h>

namespace DroneAudio
{
    void Initialize(IPluginSelf* self);
    void Shutdown();
    void Tick(float deltaSeconds);
    void OnConfigChanged(const char* section, const char* key, const char* newValue);
    void ApplySavedConfig();
}
