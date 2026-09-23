#pragma once

namespace DroneAudio
{
    void Initialize();
    void Shutdown();
    void Tick(float deltaSeconds);
    void ApplySavedConfig();

    // Stores one already-known volume value (a live drag on the loader page,
    // or the in-panel master control) straight into the atomic, without
    // re-reading the config file. Requests the next tick apply it.
    void SetVolume(const char* key, float value);
}
