#include "drone_config.h"

namespace DroneConfig
{
    IPluginSelf* Config::s_self          = nullptr;
    char         Config::s_defSpeed[32]      = {};
    char         Config::s_defRadius[32]     = {};
    char         Config::s_defHeight[32]     = {};
    ConfigEntry  Config::s_entries[15]       = {};
    ConfigSchema Config::s_schema            = {};
}
