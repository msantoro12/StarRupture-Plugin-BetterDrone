#include "drone_config.h"
#include "drone_audio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace DroneConfig
{
    namespace
    {
        constexpr float kMinSpeedPerSec      = 1.0f;
        constexpr float kMaxSpeedPerSec      = 20000.0f;
        constexpr float kMinRadius           = 100.0f;
        constexpr float kMaxRadiusBound      = 1000000.0f;
        constexpr float kMinHeight           = 100.0f;
        constexpr float kMaxHeightBound      = 500000.0f;
        constexpr float kMinSphereRadius     = 20.0f;
        constexpr float kMaxSphereRadius     = 50.0f;   // the stock radius
        constexpr float kMinBoostMultiplier  = 1.0f;
        constexpr float kMaxBoostMultiplier  = 10.0f;

        // The drone has no movement component (no UFloatingPawnMovement or
        // similar on ACrCharacterDroneBase/ABP_FloatingDrone_C -- checked the
        // SDK dump), and UAuActorPlacementDeveloperSettings exposes only a
        // flat BuildingDroneSpeedPerSec target, no acceleration/deceleration
        // of its own to inherit. There is nothing native to read here.
        //
        // So this floor is a deliberate choice, not a game value: fast enough
        // to feel responsive, slow enough to never read as a snap. It covers
        // a 1000 cm/s boost jump in a quarter second.
        // It doubles as the reset-to-default value (DefaultAcceleration/
        // DefaultDeceleration in drone_config.h -- keep both in sync with
        // this), and as the Init clamp floor, so any saved 0 from before this
        // change (including a fresh migration with nothing to migrate) gets
        // clamped up to it on every load, not just once.
        constexpr float kMinAccelDecel       = 4000.0f;
        constexpr float kDefaultAccelDecel   = kMinAccelDecel;
        constexpr float kMaxAccelDecel       = 50000.0f;

        constexpr float kMinVolume           = 0.0f;
        constexpr float kMaxVolume           = 1.0f;

        float Clamp(float value, float lo, float hi)
        {
            if (value < lo) return lo;
            if (value > hi) return hi;
            return value;
        }

        // The plugin DLL's own directory, resolved via the address of this
        // function rather than a stored DllMain HMODULE. The panel config
        // file sits next to BetterDrone.ini, under <this dir>\config\.
        void GetModuleDirectory(char* outDir, size_t outSize)
        {
            HMODULE module = nullptr;
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&GetModuleDirectory), &module);

            char path[MAX_PATH] = {};
            GetModuleFileNameA(module, path, MAX_PATH);

            char* lastSlash = strrchr(path, '\\');
            if (lastSlash)
                *lastSlash = '\0';

            snprintf(outDir, outSize, "%s", path);
        }

        void GetPanelConfigPath(char* outPath, size_t outSize)
        {
            char dir[MAX_PATH] = {};
            GetModuleDirectory(dir, sizeof(dir));
            snprintf(outPath, outSize, "%s\\config\\BetterDrone-Panel.ini", dir);
        }

        bool PanelConfigExists()
        {
            char path[MAX_PATH] = {};
            GetPanelConfigPath(path, sizeof(path));
            return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        }

        float PanelReadFloat(const char* section, const char* key, float defaultValue)
        {
            char path[MAX_PATH] = {};
            GetPanelConfigPath(path, sizeof(path));

            char defStr[64] = {};
            snprintf(defStr, sizeof(defStr), "%.6f", defaultValue);

            char buf[64] = {};
            GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), path);
            return static_cast<float>(atof(buf));
        }

        // Every write to the panel file goes through this one lock, so the
        // file never depends on kernel32's own undocumented locking around
        // WritePrivateProfileString. Recursive, so a caller can hold it
        // across reading the value it is about to write: two writers of one
        // key then land in the order they read, and the file never ends up
        // behind the cache. It covers the file only; the cached values are
        // atomics, and nothing here orders them against any other state.
        std::recursive_mutex g_panelFileMutex;

        void PanelWriteFloat(const char* section, const char* key, float value)
        {
            std::lock_guard<std::recursive_mutex> lock(g_panelFileMutex);

            char path[MAX_PATH] = {};
            GetPanelConfigPath(path, sizeof(path));

            char valStr[64] = {};
            snprintf(valStr, sizeof(valStr), "%.6f", value);
            WritePrivateProfileStringA(section, key, valStr, path);
        }

        void PanelReadString(const char* section, const char* key, const char* defaultValue,
                              char* outValue, int outSize)
        {
            char path[MAX_PATH] = {};
            GetPanelConfigPath(path, sizeof(path));
            GetPrivateProfileStringA(section, key, defaultValue, outValue, outSize, path);
        }

        void PanelWriteString(const char* section, const char* key, const char* value)
        {
            std::lock_guard<std::recursive_mutex> lock(g_panelFileMutex);

            char path[MAX_PATH] = {};
            GetPanelConfigPath(path, sizeof(path));
            WritePrivateProfileStringA(section, key, value, path);
        }

        // The panel needs the unit every frame it draws, so it is cached like the
        // floats rather than read from the file on each call.
        const char* const kSpeedUnits[] = { "km/h", "mph", "cm/s" };
        constexpr int kSpeedUnitCount = static_cast<int>(sizeof(kSpeedUnits) / sizeof(kSpeedUnits[0]));
        std::atomic<int> g_speedUnit{ 0 };

        // The one speed-unit key in the panel file; the floats' keys are in
        // kPanelFloats below.
        constexpr const char* kUnitSection = "UI";
        constexpr const char* kUnitKey     = "SpeedUnit";

        // Panel-file booleans are written as "true"/"false", the loader's own
        // spelling for its Boolean entries.
        constexpr const char* kSpeedometerKey = "Speedometer";
        std::atomic<bool> g_speedometer{ true };

        int SpeedUnitIndex(const char* unit)
        {
            for (int i = 0; i < kSpeedUnitCount; ++i)
                if (unit && strcmp(unit, kSpeedUnits[i]) == 0)
                    return i;
            return 0;
        }

        // Every float in BetterDrone-Panel.ini, in PanelFloat order: its
        // section and key as written to the file, its clamp bounds, and the
        // value used when the file has none. This one table drives the
        // migration, the load, and every later lookup of a panel setting, so
        // a key cannot be typed (or forgotten) anywhere else.
        struct PanelFloatDef
        {
            const char* section;
            const char* key;
            float minValue;
            float maxValue;
            float defaultValue;
        };

        // kMinAccelDecel is the floor, not just a fallback: Init clamps
        // whatever loads (see CachedFloat::Init below), so a value saved as
        // 0 by a build predating this change comes back up to the floor on
        // this and every later load, the same way any other stored value
        // outside its bounds already gets clamped back in range.
        //
        // Speed, MaxRadius and MaxHeight default to the game's own values,
        // which are only known once the drone settings CDO is found
        // (Config::AdoptGameDefaults). The numbers here are the game's
        // shipped config values, used only until then.
        constexpr PanelFloatDef kPanelFloats[] = {
            { "Drone",    "SpeedPerSec",     kMinSpeedPerSec,     kMaxSpeedPerSec,     400.0f  },
            { "Drone",    "MaxRadius",       kMinRadius,          kMaxRadiusBound,     4000.0f },
            { "Drone",    "MaxHeight",       kMinHeight,          kMaxHeightBound,     3500.0f },
            { "Drone",    "SphereRadius",    kMinSphereRadius,    kMaxSphereRadius,    kMaxSphereRadius },
            { "Controls", "BoostMultiplier", kMinBoostMultiplier, kMaxBoostMultiplier, 2.0f    },
            { "Controls", "Acceleration",    kMinAccelDecel,      kMaxAccelDecel,      kDefaultAccelDecel },
            { "Controls", "Deceleration",    kMinAccelDecel,      kMaxAccelDecel,      kDefaultAccelDecel },
            { "Audio", DroneAudio::kVolumeKeys[DroneAudio::kVolIdle],     kMinVolume, kMaxVolume, 1.0f },
            { "Audio", DroneAudio::kVolumeKeys[DroneAudio::kVolMovement], kMinVolume, kMaxVolume, 1.0f },
            { "Audio", DroneAudio::kVolumeKeys[DroneAudio::kVolRotation], kMinVolume, kMaxVolume, 1.0f },
            { "Audio", DroneAudio::kVolumeKeys[DroneAudio::kVolStation],  kMinVolume, kMaxVolume, 1.0f },
        };
        constexpr int kPanelFloatCount = static_cast<int>(PanelFloat::Count);
        static_assert(sizeof(kPanelFloats) / sizeof(kPanelFloats[0]) == kPanelFloatCount,
                      "kPanelFloats needs exactly one row per PanelFloat");
        static_assert(kPanelFloatCount - static_cast<int>(PanelFloat::IdleVolume) == DroneAudio::kVolCount,
                      "the volumes must be the last PanelFloat entries, one per DroneAudio::kVolumeKeys");

        const PanelFloatDef& DefOf(PanelFloat id) { return kPanelFloats[static_cast<int>(id)]; }

        // Copies one setting out of the loader-managed BetterDrone.ini (read
        // through IPluginConfig, before InitializeFromSchema rewrites that
        // file down to the schema keys) into the panel file.
        // A key the old file never had is left out, so it keeps following its
        // default (for speed and range, the game's own value) instead of
        // having today's default written down as if the player chose it.
        void MigrateFloat(IPluginSelf* self, const char* section, const char* key)
        {
            char stored[64] = {};
            if (!self->config->ReadString(self, section, key, stored, sizeof(stored), "") || stored[0] == '\0')
                return;

            PanelWriteFloat(section, key, static_cast<float>(atof(stored)));
        }

        void MigrateString(IPluginSelf* self, const char* section, const char* key, const char* fallback)
        {
            char buf[64] = {};
            self->config->ReadString(self, section, key, buf, sizeof(buf), fallback);
            PanelWriteString(section, key, buf);
        }

        // Existing installs have their panel-only values sitting in
        // BetterDrone.ini. Registering the schema rewrites that file down to
        // just the schema keys, so anything panel-only has to be copied out
        // first. Runs once: skipped once the panel file exists.
        void MigratePanelSettingsIfNeeded(IPluginSelf* self)
        {
            if (PanelConfigExists())
                return;

            for (const PanelFloatDef& def : kPanelFloats)
                MigrateFloat(self, def.section, def.key);

            MigrateString(self, kUnitSection, kUnitKey, kSpeedUnits[0]);
        }

        // Two stale spellings of "no custom key chosen" predate the current
        // kBoostKeyFollowsSprint sentinel: the literal "LeftShift" (the
        // original shipped default, which the loader's rebind picker rejects
        // outright and so could never have been a deliberate choice) and an
        // empty value (what briefly wrote it before the sentinel existed).
        // Both are safe to rewrite unconditionally, every load, with no
        // one-time flag needed -- a real custom combo is never either of
        // these.
        void MigrateBoostKeyIfNeeded(IPluginSelf* self)
        {
            char current[64] = {};
            self->config->ReadString(self, "Controls", "Boost Key", current, sizeof(current), kBoostKeyFollowsSprint);
            if (current[0] == '\0' || strcmp(current, "LeftShift") == 0)
                self->config->WriteString(self, "Controls", "Boost Key", kBoostKeyFollowsSprint);
        }

        // Earlier builds spelled the Controls keys "ToggleKey" and "BoostKey".
        // Registering the schema drops keys it doesn't list, so a value saved
        // under an old name is copied to the new one first. Runs every load;
        // once the new key holds a value it is left alone.
        void MigrateRenamedKey(IPluginSelf* self, const char* section, const char* oldKey, const char* newKey)
        {
            char value[64] = {};
            self->config->ReadString(self, section, oldKey, value, sizeof(value), "");
            if (value[0] == '\0')
                return;

            char current[64] = {};
            self->config->ReadString(self, section, newKey, current, sizeof(current), "");
            if (current[0] == '\0')
                self->config->WriteString(self, section, newKey, value);
        }

        // A keybind entry, or its default when the entry is missing or empty.
        void ReadKeybind(IPluginSelf* self, const char* section, const char* key, const char* fallback,
                         char* outBuffer, int bufferSize)
        {
            if (!outBuffer || bufferSize <= 0) return;
            outBuffer[0] = '\0';
            if (!self ||
                !self->config->ReadString(self, section, key, outBuffer, bufferSize, fallback) ||
                outBuffer[0] == '\0')
            {
                snprintf(outBuffer, static_cast<size_t>(bufferSize), "%s", fallback);
            }
        }

        // In-memory mirror of one panel-file float. Loaded once in
        // Config::Initialize; every Read after that is a plain atomic load
        // -- OnDroneTick calls several of these every tick, and the file
        // must never be touched from there. SetLive clamps and updates the
        // cache only, for a slider mid-drag; Persist writes the current
        // cached value to disk, called once the edit is done rather than on
        // every drag step.
        class CachedFloat
        {
        public:
            void Init(const char* section, const char* key, float minV, float maxV, float loaded, bool stored)
            {
                m_section = section;
                m_key     = key;
                m_min     = minV;
                m_max     = maxV;
                m_stored.store(stored, std::memory_order_relaxed);
                m_value.store(Clamp(loaded, m_min, m_max), std::memory_order_relaxed);
            }

            float Read() const { return m_value.load(std::memory_order_relaxed); }

            // Whether the panel file held this key when it was loaded.
            bool Stored() const { return m_stored.load(std::memory_order_relaxed); }

            float SetLive(float value)
            {
                value = Clamp(value, m_min, m_max);
                m_value.store(value, std::memory_order_relaxed);
                return value;
            }

            void Persist()
            {
                // Read the value under the file lock: a concurrent Persist
                // of this key then writes whatever the cache holds when its
                // turn comes, never an older value after a newer one.
                std::lock_guard<std::recursive_mutex> lock(g_panelFileMutex);
                PanelWriteFloat(m_section, m_key, m_value.load(std::memory_order_relaxed));
                m_stored.store(true, std::memory_order_relaxed);
            }

            float Write(float value)
            {
                value = SetLive(value);
                Persist();
                return value;
            }

        private:
            std::atomic<float> m_value{ 0.0f };
            const char* m_section = nullptr;
            const char* m_key     = nullptr;
            float m_min = 0.0f;
            float m_max = 0.0f;
            std::atomic<bool> m_stored{ false };
        };

        // One CachedFloat per kPanelFloats row, same order.
        CachedFloat g_panel[kPanelFloatCount];

        CachedFloat& Panel(PanelFloat id) { return g_panel[static_cast<int>(id)]; }
    }

    IPluginSelf* Config::s_self = nullptr;

    void Config::Initialize(IPluginSelf* self)
    {
        s_self = self;
        if (!s_self)
            return;

        MigratePanelSettingsIfNeeded(s_self);
        MigrateRenamedKey(s_self, "Controls", "ToggleKey", "Toggle Key");
        MigrateRenamedKey(s_self, "Controls", "BoostKey",  "Boost Key");
        MigrateBoostKeyIfNeeded(s_self);

        for (int i = 0; i < kPanelFloatCount; ++i)
        {
            const PanelFloatDef& def = kPanelFloats[i];
            char stored[64] = {};
            PanelReadString(def.section, def.key, "", stored, sizeof(stored));
            g_panel[i].Init(def.section, def.key, def.minValue, def.maxValue,
                PanelReadFloat(def.section, def.key, def.defaultValue), stored[0] != '\0');
        }

        char unit[16] = {};
        PanelReadString(kUnitSection, kUnitKey, kSpeedUnits[0], unit, sizeof(unit));
        g_speedUnit.store(SpeedUnitIndex(unit));

        char speedometer[16] = {};
        PanelReadString(kUnitSection, kSpeedometerKey, "true", speedometer, sizeof(speedometer));
        g_speedometer.store(_stricmp(speedometer, "false") != 0 && strcmp(speedometer, "0") != 0);

        static const ConfigEntry entries[] = {
            { "Drone",       "Always Allow Drone",     ConfigValueType::Boolean, "false",     "Allow the building drone in places it's normally blocked, including wave events.", 0.0f, 1.0f },
            { "Interaction", "Interact In Drone Mode", ConfigValueType::Boolean, "true",      "Opens nearby containers and doors from the drone. The camera does not recenter when the prompt appears.", 0.0f, 1.0f },
            { "Interaction", "Interact Key",           ConfigValueType::Keybind, "E",         "Key that triggers interaction while the drone is out, matching the game's own interact key.", 0.0f, 0.0f },
            { "Interaction", "Map In Drone Mode",      ConfigValueType::Boolean, "true",      "Opens the map from the drone.", 0.0f, 1.0f },
            { "Interaction", "Map Key",                ConfigValueType::Keybind, "M",         "Key that opens the map while the drone is out. Set it to the game's own map key.", 0.0f, 0.0f },
            { "Interaction", "Drone Reveals Map",      ConfigValueType::Boolean, "false",     "Flying the drone uncovers the map the way walking does. What it uncovers is saved with your game.", 0.0f, 1.0f },
            // Matches BetterCheats' own ToggleKey default on purpose, so one
            // F10 press opens both panels. The loader dispatches a keypress
            // to every plugin registered on it, not just one, so this is safe.
            { "Controls",    "Toggle Key",             ConfigValueType::Keybind, "F10",        "Key to toggle the BetterDrone menu window", 0.0f, 0.0f },
            { "Controls",    "Boost Key",              ConfigValueType::Keybind, kBoostKeyFollowsSprint, "Key held to boost drone speed, following your Sprint key unless you set one here.", 0.0f, 0.0f },
            { "Controls",    "Previous Preset Key",    ConfigValueType::Keybind, "Hyphen",    "Key that steps to the next slower speed preset while the drone is out.", 0.0f, 0.0f },
            { "Controls",    "Next Preset Key",        ConfigValueType::Keybind, "Equals",    "Key that steps to the next faster speed preset while the drone is out.", 0.0f, 0.0f },
            { "Controls",    "Mining Laser Key",       ConfigValueType::Keybind, "G",         "Key held to mine the ore under the crosshair from the drone, with the mining tool's damage and range. Host and single player only.", 0.0f, 0.0f },
        };
        static const ConfigSchema schema{ entries, static_cast<int>(sizeof(entries) / sizeof(entries[0])) };

        s_self->config->InitializeFromSchema(s_self, &schema);
    }

    bool Config::ReadAlwaysAllowDrone()
    {
        return s_self ? s_self->config->ReadBool(s_self, "Drone", "Always Allow Drone", false) : false;
    }

    bool Config::ReadInteractInDroneMode()
    {
        return s_self ? s_self->config->ReadBool(s_self, "Interaction", "Interact In Drone Mode", true) : false;
    }

    void Config::ReadInteractKey(char* outBuffer, int bufferSize)
    {
        ReadKeybind(s_self, "Interaction", "Interact Key", "E", outBuffer, bufferSize);
    }

    bool Config::ReadMapInDroneMode()
    {
        return s_self ? s_self->config->ReadBool(s_self, "Interaction", "Map In Drone Mode", true) : false;
    }

    bool Config::ReadDroneRevealsMap()
    {
        return s_self ? s_self->config->ReadBool(s_self, "Interaction", "Drone Reveals Map", false) : false;
    }

    void Config::ReadMapKey(char* outBuffer, int bufferSize)
    {
        ReadKeybind(s_self, "Interaction", "Map Key", "M", outBuffer, bufferSize);
    }

    void Config::ReadToggleKey(char* outBuffer, int bufferSize)
    {
        ReadKeybind(s_self, "Controls", "Toggle Key", "F10", outBuffer, bufferSize);
    }

    void Config::ReadPrevPresetKey(char* outBuffer, int bufferSize)
    {
        ReadKeybind(s_self, "Controls", "Previous Preset Key", "Hyphen", outBuffer, bufferSize);
    }

    void Config::ReadNextPresetKey(char* outBuffer, int bufferSize)
    {
        ReadKeybind(s_self, "Controls", "Next Preset Key", "Equals", outBuffer, bufferSize);
    }

    void Config::ReadMiningLaserKey(char* outBuffer, int bufferSize)
    {
        ReadKeybind(s_self, "Controls", "Mining Laser Key", "G", outBuffer, bufferSize);
    }

    void Config::ReadBoostKey(char* outBuffer, int bufferSize)
    {
        ReadKeybind(s_self, "Controls", "Boost Key", kBoostKeyFollowsSprint, outBuffer, bufferSize);
    }

    float Config::ReadPanel(PanelFloat id)                  { return Panel(id).Read(); }
    float Config::SetPanelLive(PanelFloat id, float value)  { return Panel(id).SetLive(value); }
    void  Config::PersistPanel(PanelFloat id)               { Panel(id).Persist(); }
    float Config::WritePanel(PanelFloat id, float value)    { return Panel(id).Write(value); }

    void Config::AdoptGameDefaults(float speedPerSec, float maxRadius, float maxHeight)
    {
        const struct { PanelFloat id; float value; } game[] = {
            { PanelFloat::SpeedPerSec, speedPerSec },
            { PanelFloat::MaxRadius,   maxRadius },
            { PanelFloat::MaxHeight,   maxHeight },
        };
        for (const auto& g : game)
            if (!Panel(g.id).Stored())
                Panel(g.id).SetLive(g.value);
    }

    float Config::ReadSpeedPerSec()      { return ReadPanel(PanelFloat::SpeedPerSec); }
    float Config::ReadMaxRadius()        { return ReadPanel(PanelFloat::MaxRadius); }
    float Config::ReadMaxHeight()        { return ReadPanel(PanelFloat::MaxHeight); }
    float Config::ReadSphereRadius()     { return ReadPanel(PanelFloat::SphereRadius); }
    float Config::ReadBoostMultiplier()  { return ReadPanel(PanelFloat::BoostMultiplier); }
    float Config::ReadAcceleration()     { return ReadPanel(PanelFloat::Acceleration); }
    float Config::ReadDeceleration()     { return ReadPanel(PanelFloat::Deceleration); }

    float Config::DefaultBoostMultiplier() { return DefOf(PanelFloat::BoostMultiplier).defaultValue; }
    float Config::DefaultAcceleration() { return kDefaultAccelDecel; }
    float Config::DefaultDeceleration() { return kDefaultAccelDecel; }

    void Config::ReadSpeedUnit(char* outBuffer, int bufferSize)
    {
        if (!outBuffer || bufferSize <= 0) return;
        snprintf(outBuffer, static_cast<size_t>(bufferSize), "%s", kSpeedUnits[g_speedUnit.load()]);
    }

    void Config::WriteSpeedUnit(const char* unit)
    {
        if (!unit) return;
        const int index = SpeedUnitIndex(unit);

        // Cache and file change together, so two threads picking a unit
        // leave both on the same one.
        std::lock_guard<std::recursive_mutex> lock(g_panelFileMutex);
        g_speedUnit.store(index);
        PanelWriteString(kUnitSection, kUnitKey, kSpeedUnits[index]);
    }

    bool Config::ReadSpeedometer() { return g_speedometer.load(); }

    void Config::WriteSpeedometer(bool enabled)
    {
        std::lock_guard<std::recursive_mutex> lock(g_panelFileMutex);
        g_speedometer.store(enabled);
        PanelWriteString(kUnitSection, kSpeedometerKey, enabled ? "true" : "false");
    }

    float Config::MaxSpeedPerSec() { return kMaxSpeedPerSec; }
}
