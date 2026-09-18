#pragma once
#include <plugin_interface.h>

namespace DroneConfig
{
    class Config
    {
    public:
        // Migrates any panel-only settings out of the loader's BetterDrone.ini
        // (first run only) and registers the slim loader-page schema. Must run
        // once, from PluginInit.
        static void Initialize(IPluginSelf* self);

        // Loader-page settings: stored in BetterDrone.ini via IPluginConfig,
        // editable from the ModLoader settings window.
        static bool ReadAlwaysAllowDrone();
        static bool ReadInteractInDroneMode();
        static void ReadInteractKey(char* outBuffer, int bufferSize);
        static void ReadToggleKey(char* outBuffer, int bufferSize);
        static void ReadBoostKey(char* outBuffer, int bufferSize);

        // Panel-only settings: stored in BetterDrone-Panel.ini, which the
        // loader never rewrites, and cached in memory (loaded once in
        // Initialize) so the tick path never touches the file. Read* returns
        // the cache. SetXxxLive clamps and updates the cache only -- for a
        // slider mid-drag, where the drone should react but a disk write
        // every frame would not. PersistXxx writes the current cached value
        // to disk once the edit is done. WriteXxx does both, for a single
        // action (reset button, preset).
        static float ReadSpeedPerSec();
        static float SetSpeedPerSecLive(float value);
        static void  PersistSpeedPerSec();
        static float WriteSpeedPerSec(float value);

        static float ReadMaxRadius();
        static float SetMaxRadiusLive(float value);
        static void  PersistMaxRadius();
        static float WriteMaxRadius(float value);

        static float ReadMaxHeight();
        static float SetMaxHeightLive(float value);
        static void  PersistMaxHeight();
        static float WriteMaxHeight(float value);

        static float ReadBoostMultiplier();
        static float SetBoostMultiplierLive(float value);
        static void  PersistBoostMultiplier();
        static float WriteBoostMultiplier(float value);

        static float ReadAcceleration();
        static float SetAccelerationLive(float value);
        static void  PersistAcceleration();
        static float WriteAcceleration(float value);

        static float ReadDeceleration();
        static float SetDecelerationLive(float value);
        static void  PersistDeceleration();
        static float WriteDeceleration(float value);

        static void  ReadSpeedUnit(char* outBuffer, int bufferSize);
        static void  WriteSpeedUnit(const char* unit);

        // Hard ceiling on cm/s reaching the CDO, base speed or boosted. The
        // same bound Init/Write clamp the base speed to.
        static float MaxSpeedPerSec();

        // Audio volumes: loader-page settings (schema-registered, instant via
        // OnConfigChanged), stored in BetterDrone.ini like the rest of this list.
        static float ReadAudioVolume(const char* key);
        static void  WriteAudioVolume(const char* key, float value);

        // The in-panel defaults for fields with no CDO equivalent (Speed,
        // MaxRadius and MaxHeight instead reset to DroneSettings::orig*, the
        // stock CDO values captured in InitDroneSettings).
        static float DefaultBoostMultiplier() { return 2.0f; }
        static float DefaultAcceleration()    { return 0.0f; }
        static float DefaultDeceleration()    { return 0.0f; }

    private:
        static IPluginSelf* s_self;
    };
}
