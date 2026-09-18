#pragma once
#include <plugin_interface.h>
#include <cstdio>

namespace DroneConfig
{
    class Config
    {
    public:
        static void SetSelf(IPluginSelf* self) { s_self = self; }

        // Called once the CDO is available so defaultValue strings reflect real game values.
        static void InitializeWithCDODefaults(float speedPerSec, float maxRadius, float maxHeight)
        {
            snprintf(s_defSpeed,      sizeof(s_defSpeed),      "%.2f", speedPerSec);
            snprintf(s_defRadius,     sizeof(s_defRadius),     "%.2f", maxRadius);
            snprintf(s_defHeight,     sizeof(s_defHeight),     "%.2f", maxHeight);

            s_entries[0]  = { "Drone", "SpeedPerSec",        ConfigValueType::Float,   s_defSpeed,  "Movement speed (cm/s)",                         0.0f, 4000.0f    };
            s_entries[1]  = { "Drone", "MaxRadius",          ConfigValueType::Float,   s_defRadius, "Maximum horizontal range (cm)",                 0.0f, 1000000.0f };
            s_entries[2]  = { "Drone", "MaxHeight",          ConfigValueType::Float,   s_defHeight, "Maximum vertical range (cm)",                   0.0f, 100000.0f  };
            s_entries[3]  = { "Drone", "Always Allow Drone", ConfigValueType::Boolean, "false",     "Allow building drone during wave events.",      0.0f, 1.0f       };

            s_entries[4]  = { "Controls", "ToggleKey",       ConfigValueType::Keybind, "F9",       "Key to toggle BetterDrone menu window",          0.0f, 0.0f       };
            s_entries[5]  = { "Controls", "BoostMultiplier", ConfigValueType::Float,   "2.0",      "Speed multiplier when holding boost key",        1.0f, 10.0f      };
            s_entries[6]  = { "Controls", "BoostKey",        ConfigValueType::Keybind, "LeftShift","Key held to boost drone speed",                 0.0f, 0.0f       };
            s_entries[7]  = { "Controls", "Acceleration",    ConfigValueType::Float,   "0.0",      "Acceleration rate in cm/s² (0 = instant)",      0.0f, 50000.0f   };
            s_entries[8]  = { "Controls", "Deceleration",    ConfigValueType::Float,   "0.0",      "Deceleration rate in cm/s² (0 = instant)",      0.0f, 50000.0f   };

            s_entries[9]  = { "Interaction", "Interact In Drone Mode", ConfigValueType::Boolean, "true", "Let the drone open building UIs.", 0.0f, 1.0f };
            s_entries[10] = { "Interaction", "Interact Key",           ConfigValueType::Keybind, "E",    "Key that interacts while drone is out.", 0.0f, 0.0f };

            s_entries[11] = { "Audio", "IdleVolume",     ConfigValueType::Float, "1.0", "Drone constant idle hum volume (0.0 to 1.0)", 0.0f, 1.0f };
            s_entries[12] = { "Audio", "MovementVolume", ConfigValueType::Float, "1.0", "Drone movement sound volume (0.0 to 1.0)",     0.0f, 1.0f };
            s_entries[13] = { "Audio", "RotationVolume", ConfigValueType::Float, "1.0", "Drone rotation sound volume (0.0 to 1.0)",     0.0f, 1.0f };
            s_entries[14] = { "Audio", "StationVolume",  ConfigValueType::Float, "1.0", "Drone station sound volume (0.0 to 1.0)",      0.0f, 1.0f };

            // Note: We do NOT initialize modloader UI schema here so that duplicate
            // controls do not clutter the modloader config window; all tuning is handled
            // in the dedicated BetterDrone panel UI window.
        }

        static float ReadSpeedPerSec()        { return s_self ? s_self->config->ReadFloat(s_self, "Drone", "SpeedPerSec",      0.0f)  : 0.0f;  }
        static float ReadMaxRadius()          { return s_self ? s_self->config->ReadFloat(s_self, "Drone", "MaxRadius",        0.0f)  : 0.0f;  }
        static float ReadMaxHeight()          { return s_self ? s_self->config->ReadFloat(s_self, "Drone", "MaxHeight",        0.0f)  : 0.0f;  }
        static bool  ReadAlwaysAllowDrone()   { return s_self ? s_self->config->ReadBool (s_self, "Drone", "Always Allow Drone", false) : false; }

        static float ReadBoostMultiplier()   { return s_self ? s_self->config->ReadFloat(s_self, "Controls", "BoostMultiplier", 2.0f)  : 2.0f; }
        static float ReadAcceleration()      { return s_self ? s_self->config->ReadFloat(s_self, "Controls", "Acceleration",    0.0f)  : 0.0f; }
        static float ReadDeceleration()      { return s_self ? s_self->config->ReadFloat(s_self, "Controls", "Deceleration",    0.0f)  : 0.0f; }

        static void ReadToggleKey(char* outBuffer, int bufferSize)
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

        static void ReadBoostKey(char* outBuffer, int bufferSize)
        {
            if (!outBuffer || bufferSize <= 0) return;
            outBuffer[0] = '\0';
            if (!s_self ||
                !s_self->config->ReadString(s_self, "Controls", "BoostKey", outBuffer, bufferSize, "LeftShift") ||
                outBuffer[0] == '\0')
            {
                snprintf(outBuffer, static_cast<size_t>(bufferSize), "LeftShift");
            }
        }

        static void ReadSpeedUnit(char* outBuffer, int bufferSize)
        {
            if (!outBuffer || bufferSize <= 0) return;
            outBuffer[0] = '\0';
            if (!s_self ||
                !s_self->config->ReadString(s_self, "UI", "SpeedUnit", outBuffer, bufferSize, "km/h") ||
                outBuffer[0] == '\0')
            {
                snprintf(outBuffer, static_cast<size_t>(bufferSize), "km/h");
            }
        }

        static void WriteSpeedUnit(const char* unit)
        {
            if (s_self && s_self->config && unit)
            {
                s_self->config->WriteString(s_self, "UI", "SpeedUnit", unit);
            }
        }

        static bool ReadInteractInDroneMode()
        {
            return s_self ? s_self->config->ReadBool(s_self, "Interaction", "Interact In Drone Mode", true) : false;
        }

        static void ReadInteractKey(char* outBuffer, int bufferSize)
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

    private:
        static IPluginSelf* s_self;
        static char s_defSpeed[32];
        static char s_defRadius[32];
        static char s_defHeight[32];
        static ConfigEntry s_entries[15];
        static ConfigSchema s_schema;
    };
}

