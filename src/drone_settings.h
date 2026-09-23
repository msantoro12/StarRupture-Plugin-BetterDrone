#pragma once
#include <plugin_interface.h>
#include <cstdint>

struct DroneSettings
{
    float* speedPerSec    = nullptr;  // BuildingDroneSpeedPerSec
    float* maxRadius      = nullptr;  // BuildingDroneMaxRadius
    float* warningRadius  = nullptr;  // BuildingDroneWarningRadius
    float* maxHeight      = nullptr;  // BuildingDroneMaxHeight
    float* warningHeight  = nullptr;  // BuildingDroneWarningHeight
    bool   valid          = false;

    // Snapshot of CDO values captured at init, restored on shutdown.
    float origSpeedPerSec  = 0.f;
    float origMaxRadius    = 0.f;
    float origWarningRadius= 0.f;
    float origMaxHeight    = 0.f;
    float origWarningHeight= 0.f;
};

extern DroneSettings g_drone;

bool InitDroneSettings();
void RestoreCDODefaults();
void UpdateActiveDrones();
void RequestUpdateActiveDrones();
void RequestMaxRadius(float radiusCm);
void RequestMaxHeight(float heightCm);

bool IsInGameSession();
void InitGameSessionTracking(IPluginSelf* self);
void ShutdownGameSessionTracking(IPluginSelf* self);

void OnDroneTick(float deltaSeconds);

// Reads BoostKey from config and applies it: a custom combo, or following
// the game's Sprint key if it's DroneConfig::kBoostKeyFollowsSprint.
void RegisterBoostKey(IPluginSelf* self);
void UnregisterBoostKey(IPluginSelf* self);

// Unregisters the current boost key and applies newKeyName in its place: a
// custom combo, or following the game's Sprint key if newKeyName is
// DroneConfig::kBoostKeyFollowsSprint. Call from a live rebind
// (OnConfigChanged for Controls/BoostKey) instead of relying on the
// loader's own UpdateKeybindByName, which only patches a registration
// whose stored combo string matches the old value exactly -- the Released
// half is registered under the bare base key, not the full combo, so it
// wouldn't be found -- and which never fires at all for the sentinel,
// since nothing is ever registered under that name.
void RebindBoostKey(IPluginSelf* self, const char* newKeyName);

// Call whenever the boost key name changes.
void UpdateBoostKeyCache(const char* keyName);
