#include "drone_settings.h"
#include "drone_config.h"
#include "drone_interact.h"
#include "drone_wave_patch.h"
#include "drone_audio.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <windows.h>
#include <cstring>
#include <cstdlib>

#include "drone_ui.h"

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_info =
{
    "BetterDrone",
    MODLOADER_BUILD_TAG,
    "AlienX",
    "Adjust building drone limits, speed boost, and audio via plugin config.",
    PLUGIN_INTERFACE_VERSION,
    PLUGIN_TARGET_CLIENT
};

static void OnEngineInit()
{
    LOG_DEBUG("OnEngineInit: acquiring drone settings CDO");

    if (!InitDroneSettings())
    {
        LOG_WARN("OnEngineInit: CDO not found — drone settings unavailable");
        return;
    }

    LOG_DEBUG("OnEngineInit: CDO defaults — speed=%.0f maxRadius=%.0f maxHeight=%.0f",
        *g_drone.speedPerSec, *g_drone.maxRadius, *g_drone.maxHeight);

    *g_drone.speedPerSec   = DroneConfig::Config::ReadSpeedPerSec();
    *g_drone.maxRadius     = DroneConfig::Config::ReadMaxRadius();
    *g_drone.maxHeight     = DroneConfig::Config::ReadMaxHeight();
    *g_drone.warningRadius = *g_drone.maxRadius * 0.95f;
    *g_drone.warningHeight = *g_drone.maxHeight * 0.95f;

    LOG_DEBUG("OnEngineInit: applied — speed=%.0f maxRadius=%.0f warningRadius=%.0f maxHeight=%.0f warningHeight=%.0f",
        *g_drone.speedPerSec, *g_drone.maxRadius, *g_drone.warningRadius,
        *g_drone.maxHeight, *g_drone.warningHeight);

    UpdateActiveDrones();
}

static void OnEngineTick(float deltaSeconds)
{
    OnDroneTick(deltaSeconds);
    DroneAudio::Tick(deltaSeconds);
    TickDroneMenuClose();
}

// Fires only from the loader's own settings window, for the entries
// registered in DroneConfig::Config::Initialize. Keybind rebinds and the
// boolean entries are picked up live wherever they're consulted. The Audio
// volumes are the one case with real work: the loader's slider fires this on
// every drag frame, before the value is committed to disk, so this has to
// parse newValue itself rather than re-read the file (GSS-9).
static void OnConfigChanged(const char* section, const char* key, const char* newValue)
{
    if (!section || !key)
        return;

    if (strcmp(section, "Audio") == 0 && newValue)
    {
        DroneAudio::SetVolume(key, strtof(newValue, nullptr));
        return;
    }

    if (strcmp(section, "Controls") == 0 && strcmp(key, "BoostKey") == 0)
    {
        // Re-register from scratch rather than rely on the loader's own
        // live-rebind patch: it matches by exact combo string, and Released
        // is registered under the bare base key, not the full combo.
        RebindBoostKey(g_self, newValue);
        LOG_DEBUG("OnConfigChanged: BoostKey rebound to '%s'", newValue ? newValue : "");
        return;
    }

    LOG_DEBUG("OnConfigChanged: [%s] %s updated", section, key);
}

static void OnEngineShutdown()
{
    LOG_DEBUG("OnEngineShutdown: resetting drone settings");
    g_drone = DroneSettings{};
}

extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo()
{
    return &s_info;
}

extern "C" __declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
{
    g_self = self;

    ResolveWavePatch(self, scanner);
    ResolveDroneInteract(self, scanner);
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    g_self = self;

    LOG_DEBUG("PluginInit: registering hooks");

    DroneConfig::Config::Initialize(self);
    DroneAudio::Initialize();
    InitGameSessionTracking(self);
    InitDroneUI(self);

    self->hooks->Engine->RegisterOnInit(OnEngineInit);
    self->hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
    self->hooks->Engine->RegisterOnTick(OnEngineTick);

    InitWavePatch();
    InitDroneInteract();
    RegisterBoostKey(self);

    if (self->hooks->UI)
    {
        self->hooks->UI->RegisterOnConfigChanged(self, OnConfigChanged);
    }

    LOG_INFO("BetterDrone initialised");
    return true;
}

extern "C" __declspec(dllexport) void PluginShutdown()
{
    LOG_DEBUG("PluginShutdown: restoring CDO defaults and unregistering hooks");

    UnregisterBoostKey(g_self);
    ShutdownDroneInteract();
    ShutdownWavePatch();
    DroneAudio::Shutdown();
    ShutdownDroneUI(g_self);
    ShutdownGameSessionTracking(g_self);
    RestoreCDODefaults();

    if (g_self)
    {
        g_self->hooks->Engine->UnregisterOnInit(OnEngineInit);
        g_self->hooks->Engine->UnregisterOnShutdown(OnEngineShutdown);
        g_self->hooks->Engine->UnregisterOnTick(OnEngineTick);

        if (g_self->hooks->UI)
        {
            g_self->hooks->UI->UnregisterOnConfigChanged(g_self, OnConfigChanged);
        }

        g_self = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
