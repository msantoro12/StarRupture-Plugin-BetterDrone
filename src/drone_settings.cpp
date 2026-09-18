#include "drone_settings.h"
#include "drone_config.h"
#include "drone_audio.h"
#include "plugin_helpers.h"
#include <AuActorPlacement_classes.hpp>
#include <Chimera_classes.hpp>
#include <BP_FloatingDrone_classes.hpp>
#include <Basic.hpp>
#include <atomic>
#include <cmath>
#include <cstring>

DroneSettings g_drone;

namespace
{
    std::atomic<bool> g_isBoosting{ false };
    float g_currentEffectiveSpeed = 0.0f;
    char g_registeredBoostKey[64] = {};
    IPluginSelf* s_sessionSelf = nullptr;
    bool g_inGameSession = false;

    void OnBoostKeyPressed(EModKey, EModKeyEvent event)
    {
        if (event == EModKeyEvent::Pressed)
        {
            SetBoostActive(true);
        }
        else if (event == EModKeyEvent::Released)
        {
            SetBoostActive(false);
        }
    }

    void OnWorldBeginPlay(SDK::UWorld*)
    {
        g_inGameSession = true;
        UpdateActiveDrones();
        DroneAudio::ApplySavedConfig();
    }

    void OnWorldEndPlay(SDK::UWorld*, const char* worldName)
    {
        if (worldName && std::strcmp(worldName, "ChimeraMain") == 0)
            g_inGameSession = false;
    }
}

bool IsInGameSession()
{
    return g_inGameSession;
}

void InitGameSessionTracking(IPluginSelf* self)
{
    s_sessionSelf = self;
    if (!self || !self->hooks || !self->hooks->World)
        return;

    self->hooks->World->RegisterOnWorldBeginPlay(OnWorldBeginPlay);
    self->hooks->World->RegisterOnAfterWorldEndPlay(OnWorldEndPlay);

    try
    {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (world && world->GetName() == "ChimeraMain")
            g_inGameSession = true;
    }
    catch (...) {}
}

void ShutdownGameSessionTracking(IPluginSelf* self)
{
    if (s_sessionSelf && s_sessionSelf->hooks && s_sessionSelf->hooks->World)
    {
        s_sessionSelf->hooks->World->UnregisterOnWorldBeginPlay(OnWorldBeginPlay);
        s_sessionSelf->hooks->World->UnregisterOnAfterWorldEndPlay(OnWorldEndPlay);
    }
    s_sessionSelf = nullptr;
    g_inGameSession = false;
}

bool InitDroneSettings()
{
    LOG_DEBUG("InitDroneSettings: looking up UAuActorPlacementDeveloperSettings CDO");

    auto* cdo = SDK::UAuActorPlacementDeveloperSettings::GetDefaultObj();
    if (!cdo)
    {
        LOG_WARN("InitDroneSettings: CDO is null");
        g_drone.valid = false;
        return false;
    }

    g_drone.speedPerSec   = &cdo->BuildingDroneSpeedPerSec;
    g_drone.maxRadius     = &cdo->BuildingDroneMaxRadius;
    g_drone.warningRadius = &cdo->BuildingDroneWarningRadius;
    g_drone.maxHeight     = &cdo->BuildingDroneMaxHeight;
    g_drone.warningHeight = &cdo->BuildingDroneWarningHeight;
    g_drone.origSpeedPerSec   = *g_drone.speedPerSec;
    g_drone.origMaxRadius     = *g_drone.maxRadius;
    g_drone.origWarningRadius = *g_drone.warningRadius;
    g_drone.origMaxHeight     = *g_drone.maxHeight;
    g_drone.origWarningHeight = *g_drone.warningHeight;
    g_drone.valid             = true;
    g_currentEffectiveSpeed   = *g_drone.speedPerSec;

    LOG_DEBUG("InitDroneSettings: CDO found at %p", static_cast<void*>(cdo));
    return true;
}

void RestoreCDODefaults()
{
    if (!g_drone.valid)
        return;

    *g_drone.speedPerSec   = g_drone.origSpeedPerSec;
    *g_drone.maxRadius     = g_drone.origMaxRadius;
    *g_drone.warningRadius = g_drone.origWarningRadius;
    *g_drone.maxHeight     = g_drone.origMaxHeight;
    *g_drone.warningHeight = g_drone.origWarningHeight;

    LOG_DEBUG("RestoreCDODefaults: speed=%.0f maxRadius=%.0f maxHeight=%.0f",
        g_drone.origSpeedPerSec, g_drone.origMaxRadius, g_drone.origMaxHeight);
}

void UpdateActiveDrones()
{
    if (!g_drone.valid)
        return;

    auto* objects = SDK::UObject::GObjects.GetTypedPtr();
    if (!objects)
    {
        LOG_WARN("UpdateActiveDrones: GObjects unavailable");
        return;
    }

    const SDK::UClass* droneClass = SDK::ABP_FloatingDrone_C::StaticClass();
    if (!droneClass)
    {
        LOG_WARN("UpdateActiveDrones: ABP_FloatingDrone_C class not found");
        return;
    }

    int32_t updated = 0;
    const int32_t count = objects->Num();
    for (int32_t i = 0; i < count; ++i)
    {
        SDK::UObject* obj = objects->GetByIndex(i);
        if (!obj || obj->IsDefaultObject() || !obj->IsA(droneClass))
            continue;

        auto* drone = static_cast<SDK::ACrCharacterDroneBase*>(obj);
        SDK::UAuActorPlacementDeveloperSettings* settings = drone->PlacementDeveloperSettings;
        if (!settings)
        {
            LOG_DEBUG("UpdateActiveDrones: drone instance has null PlacementDeveloperSettings, skipping");
            continue;
        }

        settings->BuildingDroneSpeedPerSec      = *g_drone.speedPerSec;
        settings->BuildingDroneMaxRadius        = *g_drone.maxRadius;
        settings->BuildingDroneWarningRadius    = *g_drone.warningRadius;
        settings->BuildingDroneMaxHeight        = *g_drone.maxHeight;
        settings->BuildingDroneWarningHeight    = *g_drone.warningHeight;
        ++updated;
    }

    LOG_DEBUG("UpdateActiveDrones: updated %d active drone(s)", updated);
}

namespace
{
    std::atomic<bool>  g_pendingUpdateDrones{ false };
    std::atomic<bool>  g_pendingRadius{ false };
    std::atomic<bool>  g_pendingHeight{ false };
    std::atomic<float> g_pendingRadiusValue{ 0.0f };
    std::atomic<float> g_pendingHeightValue{ 0.0f };
}

void RequestUpdateActiveDrones()
{
    g_pendingUpdateDrones.store(true, std::memory_order_relaxed);
}

void RequestMaxRadius(float radiusCm)
{
    g_pendingRadiusValue.store(radiusCm, std::memory_order_relaxed);
    g_pendingRadius.store(true, std::memory_order_relaxed);
    RequestUpdateActiveDrones();
}

void RequestMaxHeight(float heightCm)
{
    g_pendingHeightValue.store(heightCm, std::memory_order_relaxed);
    g_pendingHeight.store(true, std::memory_order_relaxed);
    RequestUpdateActiveDrones();
}

void SetBoostActive(bool active)
{
    g_isBoosting.store(active, std::memory_order_relaxed);
}

void OnDroneTick(float deltaSeconds)
{
    if (!g_drone.valid || !g_drone.speedPerSec)
        return;

    if (g_pendingUpdateDrones.exchange(false, std::memory_order_relaxed))
    {
        if (g_pendingRadius.exchange(false, std::memory_order_relaxed))
        {
            const float radius = g_pendingRadiusValue.load(std::memory_order_relaxed);
            *g_drone.maxRadius     = radius;
            *g_drone.warningRadius = radius * 0.95f;
        }
        if (g_pendingHeight.exchange(false, std::memory_order_relaxed))
        {
            const float height = g_pendingHeightValue.load(std::memory_order_relaxed);
            *g_drone.maxHeight     = height;
            *g_drone.warningHeight = height * 0.95f;
        }
        UpdateActiveDrones();
    }

    const float baseSpeed = DroneConfig::Config::ReadSpeedPerSec();
    const float mult = g_isBoosting.load(std::memory_order_relaxed) ? DroneConfig::Config::ReadBoostMultiplier() : 1.0f;
    const float targetSpeed = baseSpeed * mult;

    const float accel = DroneConfig::Config::ReadAcceleration();
    const float decel = DroneConfig::Config::ReadDeceleration();

    if (g_currentEffectiveSpeed <= 0.0f)
        g_currentEffectiveSpeed = baseSpeed;

    if (g_currentEffectiveSpeed < targetSpeed)
    {
        if (accel > 0.0f)
        {
            g_currentEffectiveSpeed += accel * deltaSeconds;
            if (g_currentEffectiveSpeed > targetSpeed)
                g_currentEffectiveSpeed = targetSpeed;
        }
        else
        {
            g_currentEffectiveSpeed = targetSpeed;
        }
    }
    else if (g_currentEffectiveSpeed > targetSpeed)
    {
        if (decel > 0.0f)
        {
            g_currentEffectiveSpeed -= decel * deltaSeconds;
            if (g_currentEffectiveSpeed < targetSpeed)
                g_currentEffectiveSpeed = targetSpeed;
        }
        else
        {
            g_currentEffectiveSpeed = targetSpeed;
        }
    }

    if (std::fabs(*g_drone.speedPerSec - g_currentEffectiveSpeed) > 0.01f)
    {
        *g_drone.speedPerSec = g_currentEffectiveSpeed;
        UpdateActiveDrones();
    }
}

void RegisterBoostKey(IPluginSelf* self)
{
    if (!self || !self->hooks->Input)
        return;

    char keyName[64] = {};
    DroneConfig::Config::ReadBoostKey(keyName, sizeof(keyName));
    if (keyName[0] == '\0')
        return;

    self->hooks->Input->RegisterKeybindByName(keyName, EModKeyEvent::Pressed, OnBoostKeyPressed);
    self->hooks->Input->RegisterKeybindByName(keyName, EModKeyEvent::Released, OnBoostKeyPressed);
    snprintf(g_registeredBoostKey, sizeof(g_registeredBoostKey), "%s", keyName);
}

void UnregisterBoostKey(IPluginSelf* self)
{
    if (!self || !self->hooks->Input || g_registeredBoostKey[0] == '\0')
        return;

    self->hooks->Input->UnregisterKeybindByName(g_registeredBoostKey, EModKeyEvent::Pressed, OnBoostKeyPressed);
    self->hooks->Input->UnregisterKeybindByName(g_registeredBoostKey, EModKeyEvent::Released, OnBoostKeyPressed);
    g_registeredBoostKey[0] = '\0';
}
