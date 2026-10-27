#pragma once
#include <plugin_interface.h>

namespace DroneConfig
{
    // Boost Key's value when boost follows the game's own Sprint binding
    // instead of a custom keybind. The one spelling shared by the schema
    // default, the migration, and drone_settings.cpp's registration check.
    constexpr const char* kBoostKeyFollowsSprint = "Sprint";

    // The panel-file floats. Each one's section, key, bounds and default are
    // typed once, in the table in drone_config.cpp. The four volumes keep
    // DroneAudio::VolIndex order, so AudioVolumeId(i) is the sound named by
    // DroneAudio::kVolumeKeys[i].
    enum class PanelFloat : int
    {
        SpeedPerSec, MaxRadius, MaxHeight, SphereRadius,
        BoostMultiplier, Acceleration, Deceleration,
        IdleVolume, MovementVolume, RotationVolume, StationVolume,
        Count
    };

    constexpr PanelFloat AudioVolumeId(int index)
    {
        return static_cast<PanelFloat>(static_cast<int>(PanelFloat::IdleVolume) + index);
    }

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
        static bool ReadMapInDroneMode();
        static void ReadMapKey(char* outBuffer, int bufferSize);
        static bool ReadDroneRevealsMap();
        static void ReadToggleKey(char* outBuffer, int bufferSize);
        static void ReadPrevPresetKey(char* outBuffer, int bufferSize);
        static void ReadNextPresetKey(char* outBuffer, int bufferSize);
        static void ReadMiningLaserKey(char* outBuffer, int bufferSize);

        // kBoostKeyFollowsSprint means "follow the game's Sprint key"; any
        // other value is a custom combo. Never empty.
        static void ReadBoostKey(char* outBuffer, int bufferSize);

        // Panel-only settings: stored in BetterDrone-Panel.ini, which the
        // loader never rewrites, and cached in memory (loaded once in
        // Initialize) so the tick path never touches the file. ReadPanel
        // returns the cache. SetPanelLive clamps and updates the cache only
        // -- for a slider mid-drag, where the drone should react but a disk
        // write every frame would not. PersistPanel writes the current cached
        // value to disk once the edit is done. WritePanel does both, for a
        // single action (reset button, preset). These change the stored
        // value only: they do not apply it to the drone (range requests,
        // live audio). Call PanelSettings for that; it is what the panel
        // rows use.
        static float ReadPanel(PanelFloat id);
        static float SetPanelLive(PanelFloat id, float value);
        static void  PersistPanel(PanelFloat id);
        static float WritePanel(PanelFloat id, float value);

        // Called once the drone settings CDO is found, with the game's own
        // values read before BetterDrone changed anything. Speed, MaxRadius
        // and MaxHeight that the panel file does not hold take these as
        // their value. Cache only: nothing is written to the file, so they
        // keep following the game until the player sets one.
        static void AdoptGameDefaults(float speedPerSec, float maxRadius, float maxHeight);

        static float ReadSpeedPerSec();
        static float ReadMaxRadius();
        static float ReadMaxHeight();
        static float ReadSphereRadius();
        static float ReadBoostMultiplier();
        static float ReadAcceleration();
        static float ReadDeceleration();

        static void  ReadSpeedUnit(char* outBuffer, int bufferSize);
        static void  WriteSpeedUnit(const char* unit);

        // The speedometer under the drone HUD's distance readout. Cached like the unit, so the
        // tick reads memory, not the file.
        static bool  ReadSpeedometer();
        static void  WriteSpeedometer(bool enabled);

        // Applies to boosted speed too, not just the base setting.
        static float MaxSpeedPerSec();

        // The in-panel defaults for fields with no CDO equivalent (Speed,
        // MaxRadius, MaxHeight and SphereRadius instead reset to DroneSettings::orig*, the
        // stock CDO values captured in InitDroneSettings).
        static float DefaultBoostMultiplier();

        // Backed by kDefaultAccelDecel in drone_config.cpp, the one place
        // that value is chosen -- see the comment there for why. Also the
        // Init clamp floor, so this is never reachable as "instant" again.
        static float DefaultAcceleration();
        static float DefaultDeceleration();

    private:
        static IPluginSelf* s_self;
    };
}
