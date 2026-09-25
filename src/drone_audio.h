#pragma once

namespace DroneAudio
{
    // The drone's four independently-controlled sound channels, addressed by
    // key everywhere a volume is read, written, or migrated: DroneConfig's
    // panel storage, this file's own live-apply loop, and the panel rows
    // (the Master Volume convenience plus one row per channel). Keep this
    // the one place the four spellings are typed.
    enum VolIndex : int { kVolIdle = 0, kVolMovement, kVolRotation, kVolStation, kVolCount };
    constexpr const char* kVolumeKeys[kVolCount] = { "IdleVolume", "MovementVolume", "RotationVolume", "StationVolume" };

    void Initialize();
    void Shutdown();
    void Tick(float deltaSeconds);
    void ApplySavedConfig();

    // Stores one already-known volume value (a live drag on a panel row, or
    // the panel's own master control) straight into the atomic, without
    // re-reading the config file. Requests the next tick apply it.
    void SetVolume(const char* key, float value);
}
