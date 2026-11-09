#include "drone_settings.h"
#include "drone_fields.h"
#include "drone_config.h"
#include "drone_interact.h"
#include "drone_wave_patch.h"
#include "drone_audio.h"
#include "drone_body_anchor.h"
#include "drone_floor_guard.h"
#include "drone_hotbar.h"
#include "drone_laser.h"
#include "drone_map_marker.h"
#include "drone_speedometer.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "drone_ui.h"

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

static std::atomic<DWORD> g_gameThreadId{ 0 };

void NoteGameThread() { g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed); }
bool OnGameThread()   { return GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed); }

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
    PLUGIN_TARGET_CLIENT,
    PLUGIN_GAME_BUILT_CL,
    PLUGIN_MAP_MIGRATED
};

static void OnEngineInit()
{
    if (!DroneFields::Features::Limits.On())
        return;

    LOG_DEBUG("OnEngineInit: acquiring drone settings CDO");

    if (!InitDroneSettings())
    {
        LOG_WARN("OnEngineInit: CDO not found — drone settings unavailable");
        return;
    }

    LOG_DEBUG("OnEngineInit: CDO defaults — speed=%.0f maxRadius=%.0f maxHeight=%.0f",
        *g_drone.speedPerSec, *g_drone.maxRadius, *g_drone.maxHeight);

    DroneConfig::Config::AdoptGameDefaults(g_drone.origSpeedPerSec, g_drone.origMaxRadius, g_drone.origMaxHeight);

    *g_drone.speedPerSec   = DroneConfig::Config::ReadSpeedPerSec();
    *g_drone.maxRadius     = DroneConfig::Config::ReadMaxRadius();
    *g_drone.maxHeight     = DroneConfig::Config::ReadMaxHeight();
    RequestSphereRadius(DroneConfig::Config::ReadSphereRadius());
    *g_drone.warningRadius = *g_drone.maxRadius * 0.95f;
    *g_drone.warningHeight = *g_drone.maxHeight * 0.95f;

    LOG_DEBUG("OnEngineInit: applied — speed=%.0f maxRadius=%.0f warningRadius=%.0f maxHeight=%.0f warningHeight=%.0f",
        *g_drone.speedPerSec, *g_drone.maxRadius, *g_drone.warningRadius,
        *g_drone.maxHeight, *g_drone.warningHeight);

    UpdateActiveDrones();
}

static void OnEngineTick(float deltaSeconds)
{
    NoteGameThread();
    if (DroneFields::Features::Limits.On())
        OnDroneTick(deltaSeconds);
    TickDroneFloorGuard(deltaSeconds);
    TickDroneBodyAnchor();
    if (DroneFields::Features::Audio.On())
        DroneAudio::Tick(deltaSeconds);
    if (DroneFields::Features::Announcer.On())
        DroneAudio::TickAnnouncer();
    if (DroneFields::Features::MiningLaser.On())
        TickDroneLaser(deltaSeconds);
    TickDroneMenuClose();
    TickDroneMenuOpen();
    if (DroneFields::Features::MapMarker.On())
        TickDroneMapMarker(deltaSeconds);
    if (DroneFields::Features::MapFollow.On())
        TickDroneMapFollow();
    if (DroneFields::Features::Speedometer.On())
        TickDroneSpeedometer();
    TickDronePresetKeys();
}

// Fires only from the loader's own settings window, for the entries
// registered in DroneConfig::Config::Initialize. Keybind rebinds and the
// boolean entries are picked up live wherever they're consulted, except
// Drone Reveals Map and the two body-area entries, which the tick reads from
// a cache set here. This fires
// before the loader writes the INI, so the cache takes newValue.
static void OnConfigChanged(const char* section, const char* key, const char* newValue)
{
    if (!section || !key)
        return;

    if (strcmp(section, "Controls") == 0 && strcmp(key, "Boost Key") == 0)
    {
        if (!DroneFields::Features::Limits.On())
            return;

        // Re-register from scratch rather than rely on the loader's own
        // live-rebind patch: it matches by exact combo string, and Released
        // is registered under the bare base key, not the full combo.
        RebindBoostKey(g_self, newValue);
        LOG_DEBUG("OnConfigChanged: Boost Key rebound to '%s'", newValue ? newValue : "");
        return;
    }

    if (strcmp(section, "Controls") == 0 && strcmp(key, "Slow Key") == 0)
    {
        if (!DroneFields::Features::Limits.On())
            return;

        // Re-registered for the same reason as Boost Key: its Released is
        // registered under the bare base key.
        RebindSlowKey(g_self, newValue);
        LOG_DEBUG("OnConfigChanged: Slow Key rebound to '%s'", newValue ? newValue : "");
        return;
    }

    if (strcmp(section, "Controls") == 0 && strcmp(key, "Mining Laser Key") == 0)
    {
        if (!DroneFields::Features::MiningLaser.On())
            return;

        // Re-registered for the same reason as Boost Key: its Released is
        // registered under the bare base key.
        RebindDroneLaserKey(g_self, newValue);
        LOG_DEBUG("OnConfigChanged: Mining Laser Key rebound to '%s'", newValue ? newValue : "");
        return;
    }

    if (strcmp(section, "Interaction") == 0 && strcmp(key, "Drone Reveals Map") == 0
        && DroneFields::Features::MapReveal.On())
        SetDroneFogEnabled(newValue && (strcmp(newValue, "1") == 0 || _stricmp(newValue, "true") == 0));

    if (strcmp(section, "Drone") == 0 && strcmp(key, "Keep Area Around Body Loaded") == 0)
        SetDroneBodyAnchorEnabled(newValue && (strcmp(newValue, "1") == 0 || _stricmp(newValue, "true") == 0));

    if (strcmp(section, "Drone") == 0 && strcmp(key, "Body Area Radius") == 0 && newValue)
        SetDroneBodyAnchorRadiusMeters(static_cast<float>(atof(newValue)));

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

    DroneFields::Features::Declare(self, scanner);

    ResolveWavePatch(self, scanner);
    ResolveDroneInteract(self, scanner);
    ResolveDroneMap(self, scanner);
    ResolveDroneMapFollow(self, scanner);
    ResolveDroneFog(self, scanner);
    ResolveDroneBuildMenu(self, scanner);
    ResolveDroneLaser(self, scanner);
    ResolveDroneHotbar(self, scanner);
    DroneFields::Resolve(self, scanner);
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    // First, before any early return: off its game build the loader refuses a
    // plugin whose PluginInit did not take the globals.
    PluginSdkRuntime::Init(self->hooks);

    g_self = self;

    LOG_DEBUG("PluginInit: registering hooks");

    DroneConfig::Config::Initialize(self);
    InitDroneMenuState(self);
    SetDroneBodyAnchorEnabled(DroneConfig::Config::ReadKeepBodyLoaded());
    SetDroneBodyAnchorRadiusMeters(DroneConfig::Config::ReadBodyAreaRadius());
    if (DroneFields::Features::Audio.On())
        DroneAudio::Initialize();
    InitGameSessionTracking(self);
    if (DroneFields::Features::MiningLaser.On())
        InitDroneLaser(self);
    InitDroneUI(self);
    if (DroneFields::Features::MapMarker.On())
        InitDroneMapMarker(self);
    if (DroneFields::Features::Speedometer.On())
        InitDroneSpeedometer(self);
    InitDronePresetKeys(self);

    self->hooks->Engine->RegisterOnInit(OnEngineInit);
    self->hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
    self->hooks->Engine->RegisterOnTick(OnEngineTick);

    InitWavePatch(DroneFields::Features::AlwaysAllow.On(), DroneFields::Features::WaveShelter.On());
    if (DroneFields::Features::Interaction.On())
        InitDroneInteract();
    if (DroneFields::Features::HotbarCycle.On())
        InitDroneHotbar(self);
    if (DroneFields::Features::MapOpen.On())
        InitDroneMap();
    if (DroneFields::Features::MapFollow.On())
        InitDroneMapFollow(self);
    if (DroneFields::Features::MapReveal.On())
        InitDroneFog();
    if (DroneFields::Features::BuildMenu.On())
        InitDroneBuildMenu();
    if (DroneFields::Features::Limits.On())
    {
        RegisterBoostKey(self);
        RegisterSlowKey(self);
    }

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

    if (DroneFields::Features::Limits.On())
    {
        UnregisterBoostKey(g_self);
        UnregisterSlowKey(g_self);
    }
    ShutdownDroneBodyAnchor();
    if (DroneFields::Features::MiningLaser.On())
        ShutdownDroneLaser(g_self);
    if (DroneFields::Features::Interaction.On())
        ShutdownDroneInteract();
    if (DroneFields::Features::HotbarCycle.On())
        ShutdownDroneHotbar(g_self);
    if (DroneFields::Features::MapOpen.On())
        ShutdownDroneMap();
    if (DroneFields::Features::MapReveal.On())
        ShutdownDroneFog();
    if (DroneFields::Features::BuildMenu.On())
        ShutdownDroneBuildMenu();
    ShutdownWavePatch();
    if (DroneFields::Features::Audio.On())
        DroneAudio::Shutdown();
    if (DroneFields::Features::Announcer.On())
        DroneAudio::ShutdownAnnouncer();
    if (DroneFields::Features::MapFollow.On())
        ShutdownDroneMapFollow(g_self);
    if (DroneFields::Features::MapMarker.On())
        ShutdownDroneMapMarker(g_self);
    if (DroneFields::Features::Speedometer.On())
        ShutdownDroneSpeedometer(g_self);
    ShutdownDronePresetKeys(g_self);
    ShutdownDroneUI(g_self);
    ShutdownGameSessionTracking(g_self);
    if (g_self)
        ShutdownDroneMenuState(g_self);
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
