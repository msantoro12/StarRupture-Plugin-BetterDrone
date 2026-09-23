#include "drone_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
        // the default 2x boost jump (1000 -> 2000 cm/s) in a quarter second.
        // It doubles as the reset-to-default value (DefaultAcceleration/
        // DefaultDeceleration in drone_config.h -- keep both in sync with
        // this), and as the Init clamp floor, so any saved 0 from before this
        // change (including a fresh migration with nothing to migrate) gets
        // clamped up to it on every load, not just once.
        constexpr float kMinAccelDecel       = 4000.0f;
        constexpr float kDefaultAccelDecel   = kMinAccelDecel;
        constexpr float kMaxAccelDecel       = 50000.0f;

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

        void PanelWriteFloat(const char* section, const char* key, float value)
        {
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
            char path[MAX_PATH] = {};
            GetPanelConfigPath(path, sizeof(path));
            WritePrivateProfileStringA(section, key, value, path);
        }

        // Copies one setting out of the loader-managed BetterDrone.ini (read
        // through IPluginConfig, before InitializeFromSchema rewrites that
        // file down to the schema keys) into the panel file.
        void MigrateFloat(IPluginSelf* self, const char* section, const char* key, float fallback)
        {
            const float value = self->config->ReadFloat(self, section, key, fallback);
            PanelWriteFloat(section, key, value);
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

            MigrateFloat(self, "Drone", "SpeedPerSec", 1000.0f);
            MigrateFloat(self, "Drone", "MaxRadius",   5000.0f);
            MigrateFloat(self, "Drone", "MaxHeight",   2000.0f);

            MigrateFloat(self, "Controls", "BoostMultiplier", 2.0f);
            MigrateFloat(self, "Controls", "Acceleration",    kDefaultAccelDecel);
            MigrateFloat(self, "Controls", "Deceleration",    kDefaultAccelDecel);

            MigrateString(self, "UI", "SpeedUnit", "km/h");
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
            self->config->ReadString(self, "Controls", "BoostKey", current, sizeof(current), kBoostKeyFollowsSprint);
            if (current[0] == '\0' || strcmp(current, "LeftShift") == 0)
                self->config->WriteString(self, "Controls", "BoostKey", kBoostKeyFollowsSprint);
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
            void Init(const char* section, const char* key, float minV, float maxV, float loaded)
            {
                m_section = section;
                m_key     = key;
                m_min     = minV;
                m_max     = maxV;
                m_value.store(Clamp(loaded, m_min, m_max), std::memory_order_relaxed);
            }

            float Read() const { return m_value.load(std::memory_order_relaxed); }

            float SetLive(float value)
            {
                value = Clamp(value, m_min, m_max);
                m_value.store(value, std::memory_order_relaxed);
                return value;
            }

            void Persist() const
            {
                PanelWriteFloat(m_section, m_key, m_value.load(std::memory_order_relaxed));
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
        };

        CachedFloat g_speedPerSec;
        CachedFloat g_maxRadius;
        CachedFloat g_maxHeight;
        CachedFloat g_boostMultiplier;
        CachedFloat g_acceleration;
        CachedFloat g_deceleration;

        // The panel needs the unit every frame it draws, so it is cached like the
        // floats rather than read from the file on each call.
        const char* const kSpeedUnits[] = { "km/h", "mph", "cm/s" };
        constexpr int kSpeedUnitCount = static_cast<int>(sizeof(kSpeedUnits) / sizeof(kSpeedUnits[0]));
        std::atomic<int> g_speedUnit{ 0 };

        int SpeedUnitIndex(const char* unit)
        {
            for (int i = 0; i < kSpeedUnitCount; ++i)
                if (unit && strcmp(unit, kSpeedUnits[i]) == 0)
                    return i;
            return 0;
        }
    }

    IPluginSelf* Config::s_self = nullptr;

    void Config::Initialize(IPluginSelf* self)
    {
        s_self = self;
        if (!s_self)
            return;

        MigratePanelSettingsIfNeeded(s_self);
        MigrateBoostKeyIfNeeded(s_self);

        g_speedPerSec.Init("Drone", "SpeedPerSec", kMinSpeedPerSec, kMaxSpeedPerSec,
            PanelReadFloat("Drone", "SpeedPerSec", 1000.0f));
        g_maxRadius.Init("Drone", "MaxRadius", kMinRadius, kMaxRadiusBound,
            PanelReadFloat("Drone", "MaxRadius", 5000.0f));
        g_maxHeight.Init("Drone", "MaxHeight", kMinHeight, kMaxHeightBound,
            PanelReadFloat("Drone", "MaxHeight", 2000.0f));
        g_boostMultiplier.Init("Controls", "BoostMultiplier", kMinBoostMultiplier, kMaxBoostMultiplier,
            PanelReadFloat("Controls", "BoostMultiplier", 2.0f));
        // kMinAccelDecel is the floor, not just a fallback: Init clamps
        // whatever loads (see CachedFloat::Init below), so a value saved as
        // 0 by a build predating this change comes back up to the floor on
        // this and every later load, the same way any other stored value
        // outside its bounds already gets clamped back in range.
        g_acceleration.Init("Controls", "Acceleration", kMinAccelDecel, kMaxAccelDecel,
            PanelReadFloat("Controls", "Acceleration", kDefaultAccelDecel));
        g_deceleration.Init("Controls", "Deceleration", kMinAccelDecel, kMaxAccelDecel,
            PanelReadFloat("Controls", "Deceleration", kDefaultAccelDecel));

        char unit[16] = {};
        PanelReadString("UI", "SpeedUnit", kSpeedUnits[0], unit, sizeof(unit));
        g_speedUnit.store(SpeedUnitIndex(unit));

        static const ConfigEntry entries[] = {
            { "Drone",       "Always Allow Drone",     ConfigValueType::Boolean, "false",     "Allow the building drone in places it's normally blocked, including wave events.", 0.0f, 1.0f },
            { "Interaction", "Interact In Drone Mode", ConfigValueType::Boolean, "true",      "Opens nearby containers and doors from the drone without dismounting. The camera does not recenter when the prompt appears.", 0.0f, 1.0f },
            { "Interaction", "Interact Key",           ConfigValueType::Keybind, "E",         "Key that triggers interaction while the drone is out, matching the game's own interact key.", 0.0f, 0.0f },
            // Matches BetterCheats' own ToggleKey default on purpose, so one
            // F10 press opens both panels. The loader dispatches a keypress
            // to every plugin registered on it, not just one, so this is safe.
            { "Controls",    "ToggleKey",              ConfigValueType::Keybind, "F10",        "Key to toggle the BetterDrone menu window", 0.0f, 0.0f },
            { "Controls",    "BoostKey",               ConfigValueType::Keybind, kBoostKeyFollowsSprint, "Key held to boost drone speed, following your Sprint key unless you set one here.", 0.0f, 0.0f },
            { "Audio",       "IdleVolume",             ConfigValueType::Float,   "1.0",       "Drone constant idle hum volume (0.0 to 1.0)", 0.0f, 1.0f },
            { "Audio",       "MovementVolume",         ConfigValueType::Float,   "1.0",       "Drone movement sound volume (0.0 to 1.0)",     0.0f, 1.0f },
            { "Audio",       "RotationVolume",         ConfigValueType::Float,   "1.0",       "Drone rotation sound volume (0.0 to 1.0)",     0.0f, 1.0f },
            { "Audio",       "StationVolume",          ConfigValueType::Float,   "1.0",       "Drone station sound volume (0.0 to 1.0)",      0.0f, 1.0f },
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
        if (!outBuffer || bufferSize <= 0) return;
        outBuffer[0] = '\0';
        if (!s_self ||
            !s_self->config->ReadString(s_self, "Interaction", "Interact Key", outBuffer, bufferSize, "E") ||
            outBuffer[0] == '\0')
        {
            snprintf(outBuffer, static_cast<size_t>(bufferSize), "E");
        }
    }

    void Config::ReadToggleKey(char* outBuffer, int bufferSize)
    {
        if (!outBuffer || bufferSize <= 0) return;
        outBuffer[0] = '\0';
        if (!s_self ||
            !s_self->config->ReadString(s_self, "Controls", "ToggleKey", outBuffer, bufferSize, "F10") ||
            outBuffer[0] == '\0')
        {
            snprintf(outBuffer, static_cast<size_t>(bufferSize), "F10");
        }
    }

    void Config::ReadBoostKey(char* outBuffer, int bufferSize)
    {
        if (!outBuffer || bufferSize <= 0) return;
        outBuffer[0] = '\0';
        if (!s_self ||
            !s_self->config->ReadString(s_self, "Controls", "BoostKey", outBuffer, bufferSize, kBoostKeyFollowsSprint) ||
            outBuffer[0] == '\0')
        {
            snprintf(outBuffer, static_cast<size_t>(bufferSize), "%s", kBoostKeyFollowsSprint);
        }
    }

    float Config::ReadSpeedPerSec()               { return g_speedPerSec.Read(); }
    float Config::SetSpeedPerSecLive(float value)  { return g_speedPerSec.SetLive(value); }
    void  Config::PersistSpeedPerSec()             { g_speedPerSec.Persist(); }
    float Config::WriteSpeedPerSec(float value)    { return g_speedPerSec.Write(value); }

    float Config::ReadMaxRadius()               { return g_maxRadius.Read(); }
    float Config::SetMaxRadiusLive(float value)  { return g_maxRadius.SetLive(value); }
    void  Config::PersistMaxRadius()             { g_maxRadius.Persist(); }
    float Config::WriteMaxRadius(float value)    { return g_maxRadius.Write(value); }

    float Config::ReadMaxHeight()               { return g_maxHeight.Read(); }
    float Config::SetMaxHeightLive(float value)  { return g_maxHeight.SetLive(value); }
    void  Config::PersistMaxHeight()             { g_maxHeight.Persist(); }
    float Config::WriteMaxHeight(float value)    { return g_maxHeight.Write(value); }

    float Config::ReadBoostMultiplier()               { return g_boostMultiplier.Read(); }
    float Config::SetBoostMultiplierLive(float value)  { return g_boostMultiplier.SetLive(value); }
    void  Config::PersistBoostMultiplier()             { g_boostMultiplier.Persist(); }
    float Config::WriteBoostMultiplier(float value)    { return g_boostMultiplier.Write(value); }

    float Config::ReadAcceleration()               { return g_acceleration.Read(); }
    float Config::SetAccelerationLive(float value)  { return g_acceleration.SetLive(value); }
    void  Config::PersistAcceleration()             { g_acceleration.Persist(); }
    float Config::WriteAcceleration(float value)    { return g_acceleration.Write(value); }

    float Config::ReadDeceleration()               { return g_deceleration.Read(); }
    float Config::SetDecelerationLive(float value)  { return g_deceleration.SetLive(value); }
    void  Config::PersistDeceleration()             { g_deceleration.Persist(); }
    float Config::WriteDeceleration(float value)    { return g_deceleration.Write(value); }

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
        g_speedUnit.store(index);
        PanelWriteString("UI", "SpeedUnit", kSpeedUnits[index]);
    }

    float Config::MaxSpeedPerSec() { return kMaxSpeedPerSec; }

    float Config::ReadAudioVolume(const char* key)
    {
        if (!s_self || !key) return 1.0f;
        return Clamp(s_self->config->ReadFloat(s_self, "Audio", key, 1.0f), 0.0f, 1.0f);
    }

    void Config::WriteAudioVolume(const char* key, float value)
    {
        if (!s_self || !key) return;
        s_self->config->WriteFloat(s_self, "Audio", key, Clamp(value, 0.0f, 1.0f));
    }
}
