#include "drone_settings.h"
#include "drone_config.h"
#include "drone_audio.h"
#include "drone_interact.h"
#include "drone_sprint_key.h"
#include "drone_key_vk.h"
#include "plugin_helpers.h"
#include <AuActorPlacement_classes.hpp>
#include <Chimera_classes.hpp>
#include <BP_FloatingDrone_classes.hpp>
#include <Basic.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <windows.h>

DroneSettings g_drone;

namespace
{
    // Written by the keybind callback, acted on in OnDroneTick.
    std::atomic<bool> g_boostKeyHeld{ false };

    // The custom key's VK (KeyNameToVk), or, whenever BoostKey is
    // DroneConfig::kBoostKeyFollowsSprint, the resolved Sprint key's VK.
    // OnDroneTick polls this as a fallback alongside the loader's own
    // dispatch -- needed outright for a bare modifier, which the loader
    // never dispatches to keybinds, and for Sprint mode, which registers
    // no keybind with the loader at all.
    std::atomic<int> g_boostKeyVk{ 0 };

    // True when BoostKey is DroneConfig::kBoostKeyFollowsSprint and boost is
    // following the resolved Sprint key instead of a registered custom
    // keybind.
    std::atomic<bool> g_boostFollowsSprint{ false };

    std::atomic<bool> g_lastKnownInDrone{ false };

    float g_currentEffectiveSpeed = 0.0f;

    // The two names actually registered: Pressed on the full combo, Released
    // on its bare base key (see RegisterBoostKeyName). Kept separately so
    // Unregister always matches what was registered.
    char g_registeredBoostKeyPressed[64]  = {};
    char g_registeredBoostKeyReleased[64] = {};

    IPluginSelf* s_sessionSelf = nullptr;
    bool g_inGameSession = false;

    bool g_wasInDrone = false;
    bool g_boostWasActive = false;
    bool g_loggedInstanceReport = false;

    // The text after the last '+' in a combo string ("Shift+K" -> "K"),
    // or the whole string when there's no modifier prefix.
    void ExtractBaseKey(const char* combo, char* outBuf, size_t outSize)
    {
        const char* base = combo ? combo : "";
        const char* lastPlus = std::strrchr(base, '+');
        if (lastPlus)
            base = lastPlus + 1;

        snprintf(outBuf, outSize, "%s", base);
    }

    // GetAsyncKeyState is global -- don't boost on a Shift held in another window.
    bool GameHasFocus()
    {
        HWND fg = GetForegroundWindow();
        if (!fg)
            return false;

        DWORD fgProcessId = 0;
        GetWindowThreadProcessId(fg, &fgProcessId);
        return fgProcessId == GetCurrentProcessId();
    }

    void OnBoostKeyPressed(EModKey, EModKeyEvent event)
    {
        const bool held = (event == EModKeyEvent::Pressed);
        g_boostKeyHeld.store(held, std::memory_order_relaxed);

        if (g_lastKnownInDrone.load(std::memory_order_relaxed))
        {
            LOG_INFO("OnBoostKeyPressed: boost key %s (in drone: yes)", held ? "pressed" : "released");
        }
        else
        {
            LOG_DEBUG("OnBoostKeyPressed: boost key %s (in drone: no)", held ? "pressed" : "released");
        }
    }

    // Resolves the game's current Sprint key and caches its VK for
    // OnDroneTick to poll -- following Sprint registers no keybind with the
    // loader, so nothing else would dispatch it. Falls back to LeftShift,
    // logged once, if the Sprint action or its binding cannot be found (no
    // world yet, gamepad-only, etc.); a later re-resolve picks up the real
    // key once one is available.
    void ResolveAndCacheSprintKey()
    {
        char keyName[64] = {};
        int vk = ResolveSprintVk(keyName, sizeof(keyName));
        if (vk == 0)
        {
            vk = VK_LSHIFT;
            snprintf(keyName, sizeof(keyName), "LeftShift");
            LOG_WARN("ResolveAndCacheSprintKey: could not resolve the game's Sprint key, falling back to LeftShift");
        }

        g_boostKeyVk.store(vk, std::memory_order_relaxed);
        LOG_INFO("boost key: following Sprint (%s)", keyName);
    }

    // A named combo's Released only fires on an exact modifier match
    // (DispatchCombo), so "Shift+K" never sees a Released if the player
    // lets go of Shift before K -- the key would read stuck held. The bare
    // base key has no modifier requirement and always fires on release.
    //
    // keyName == kBoostKeyFollowsSprint is not a real key: nothing is
    // registered with the loader for it, and OnDroneTick's poll of
    // g_boostKeyVk (kept fed by ResolveAndCacheSprintKey) stands in for
    // dispatch instead.
    void RegisterBoostKeyName(IPluginSelf* self, const char* keyName)
    {
        if (!self || !self->hooks->Input || !keyName || !keyName[0])
            return;

        if (strcmp(keyName, DroneConfig::kBoostKeyFollowsSprint) == 0)
        {
            g_boostFollowsSprint.store(true, std::memory_order_relaxed);
            ResolveAndCacheSprintKey();
            return;
        }

        g_boostFollowsSprint.store(false, std::memory_order_relaxed);

        char baseKey[64] = {};
        ExtractBaseKey(keyName, baseKey, sizeof(baseKey));

        self->hooks->Input->RegisterKeybindByName(keyName, EModKeyEvent::Pressed, OnBoostKeyPressed);
        self->hooks->Input->RegisterKeybindByName(baseKey, EModKeyEvent::Released, OnBoostKeyPressed);
        snprintf(g_registeredBoostKeyPressed, sizeof(g_registeredBoostKeyPressed), "%s", keyName);
        snprintf(g_registeredBoostKeyReleased, sizeof(g_registeredBoostKeyReleased), "%s", baseKey);
        UpdateBoostKeyCache(keyName);
        LOG_INFO("boost key: custom (%s)", keyName);
    }

    void OnWorldBeginPlay(SDK::UWorld*)
    {
        g_inGameSession = true;
        g_loggedInstanceReport = false;
        UpdateActiveDrones();
        DroneAudio::ApplySavedConfig();

        // Picks up an in-game rebind of Sprint made while no drone session
        // was active to observe it via the enter-drone edge below.
        if (g_boostFollowsSprint.load(std::memory_order_relaxed))
            ResolveAndCacheSprintKey();
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

void ShutdownGameSessionTracking(IPluginSelf*)
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

    int32_t found = 0;
    int32_t updated = 0;
    int32_t sharedCdo = 0;
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

        ++found;

        // Already written through the CDO.
        if (&settings->BuildingDroneSpeedPerSec == g_drone.speedPerSec)
        {
            ++sharedCdo;
            continue;
        }

        settings->BuildingDroneSpeedPerSec      = *g_drone.speedPerSec;
        settings->BuildingDroneMaxRadius        = *g_drone.maxRadius;
        settings->BuildingDroneWarningRadius    = *g_drone.warningRadius;
        settings->BuildingDroneMaxHeight        = *g_drone.maxHeight;
        settings->BuildingDroneWarningHeight    = *g_drone.warningHeight;
        ++updated;
    }

    LOG_DEBUG("UpdateActiveDrones: updated %d of %d drone instance(s), %d share the CDO settings object",
        updated, found, sharedCdo);

    // OnWorldBeginPlay calls this before any drone exists.
    if (!g_loggedInstanceReport && found > 0)
    {
        g_loggedInstanceReport = true;
        LOG_INFO("UpdateActiveDrones: session check -- %d drone instance(s) found, %d use the CDO settings object directly",
            found, sharedCdo);
    }
}

namespace
{
    // One requested value plus a dirty flag: set from any thread (the UI
    // render thread, here), consumed once and cleared by TakeIfPending,
    // called from the game thread only. Replaces a bespoke atomic<bool> +
    // atomic<float> pair per field.
    class PendingFloat
    {
    public:
        void Request(float value)
        {
            m_value.store(value, std::memory_order_relaxed);
            m_pending.store(true, std::memory_order_relaxed);
        }

        // Returns true and fills *outValue if a request was pending,
        // clearing it either way.
        bool TakeIfPending(float* outValue)
        {
            if (!m_pending.exchange(false, std::memory_order_relaxed))
                return false;
            if (outValue)
                *outValue = m_value.load(std::memory_order_relaxed);
            return true;
        }

    private:
        std::atomic<bool>  m_pending{ false };
        std::atomic<float> m_value{ 0.0f };
    };

    std::atomic<bool> g_pendingUpdateDrones{ false };
    PendingFloat      g_pendingRadius;
    PendingFloat      g_pendingHeight;
}

void RequestUpdateActiveDrones()
{
    g_pendingUpdateDrones.store(true, std::memory_order_relaxed);
}

void RequestMaxRadius(float radiusCm)
{
    g_pendingRadius.Request(radiusCm);
    RequestUpdateActiveDrones();
}

void RequestMaxHeight(float heightCm)
{
    g_pendingHeight.Request(heightCm);
    RequestUpdateActiveDrones();
}

void UpdateBoostKeyCache(const char* keyName)
{
    g_boostKeyVk.store(KeyNameToVk(keyName), std::memory_order_relaxed);
}

void OnDroneTick(float deltaSeconds)
{
    if (!g_drone.valid || !g_drone.speedPerSec)
        return;

    bool needsInstanceUpdate = false;

    if (g_pendingUpdateDrones.exchange(false, std::memory_order_relaxed))
    {
        float radius = 0.0f;
        if (g_pendingRadius.TakeIfPending(&radius))
        {
            *g_drone.maxRadius     = radius;
            *g_drone.warningRadius = radius * 0.95f;
        }
        float height = 0.0f;
        if (g_pendingHeight.TakeIfPending(&height))
        {
            *g_drone.maxHeight     = height;
            *g_drone.warningHeight = height * 0.95f;
        }
        needsInstanceUpdate = true;
    }

    const bool inDrone = IsLocalPlayerInDrone();
    g_lastKnownInDrone.store(inDrone, std::memory_order_relaxed);

    if (inDrone && !g_wasInDrone)
    {
        needsInstanceUpdate = true;

        // Picks up an in-game rebind of Sprint made mid-session.
        if (g_boostFollowsSprint.load(std::memory_order_relaxed))
            ResolveAndCacheSprintKey();
    }
    g_wasInDrone = inDrone;

    bool held = g_boostKeyHeld.load(std::memory_order_relaxed);
    if (!held && inDrone)
    {
        const int vk = g_boostKeyVk.load(std::memory_order_relaxed);
        held = vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0 && GameHasFocus();
    }

    const bool boostActive = held && inDrone;

    const float baseSpeed = DroneConfig::Config::ReadSpeedPerSec();
    float targetSpeed = boostActive ? baseSpeed * DroneConfig::Config::ReadBoostMultiplier() : baseSpeed;
    if (targetSpeed > DroneConfig::Config::MaxSpeedPerSec())
        targetSpeed = DroneConfig::Config::MaxSpeedPerSec();

    if (boostActive != g_boostWasActive)
    {
        g_boostWasActive = boostActive;
        LOG_INFO("OnDroneTick: boost %s, target=%.0f cm/s",
            boostActive ? "engaged" : "disengaged", targetSpeed);
    }

    // Acceleration/Deceleration are clamped to a floor above 0 (DroneConfig,
    // kMinAccelDecel), so there is no longer a snap path here to fall back
    // to -- every speed change, boost or otherwise, ramps.
    const float accel = DroneConfig::Config::ReadAcceleration();
    const float decel = DroneConfig::Config::ReadDeceleration();

    if (g_currentEffectiveSpeed <= 0.0f)
        g_currentEffectiveSpeed = baseSpeed;

    const bool wasAtTarget = std::fabs(g_currentEffectiveSpeed - targetSpeed) <= 0.01f;

    if (g_currentEffectiveSpeed < targetSpeed)
    {
        g_currentEffectiveSpeed += accel * deltaSeconds;
        if (g_currentEffectiveSpeed > targetSpeed)
            g_currentEffectiveSpeed = targetSpeed;
    }
    else if (g_currentEffectiveSpeed > targetSpeed)
    {
        g_currentEffectiveSpeed -= decel * deltaSeconds;
        if (g_currentEffectiveSpeed < targetSpeed)
            g_currentEffectiveSpeed = targetSpeed;
    }

    if (std::fabs(*g_drone.speedPerSec - g_currentEffectiveSpeed) > 0.01f)
        *g_drone.speedPerSec = g_currentEffectiveSpeed;

    // Walking GObjects every ramp step is too slow; sync instances once it lands.
    if (!wasAtTarget && std::fabs(g_currentEffectiveSpeed - targetSpeed) <= 0.01f)
        needsInstanceUpdate = true;

    if (needsInstanceUpdate)
        UpdateActiveDrones();
}

void RegisterBoostKey(IPluginSelf* self)
{
    if (!self || !self->hooks->Input)
        return;

    char keyName[64] = {};
    DroneConfig::Config::ReadBoostKey(keyName, sizeof(keyName));
    RegisterBoostKeyName(self, keyName);
}

void UnregisterBoostKey(IPluginSelf* self)
{
    if (!self || !self->hooks->Input)
        return;

    if (g_registeredBoostKeyPressed[0] != '\0')
        self->hooks->Input->UnregisterKeybindByName(g_registeredBoostKeyPressed, EModKeyEvent::Pressed, OnBoostKeyPressed);
    if (g_registeredBoostKeyReleased[0] != '\0')
        self->hooks->Input->UnregisterKeybindByName(g_registeredBoostKeyReleased, EModKeyEvent::Released, OnBoostKeyPressed);

    g_registeredBoostKeyPressed[0]  = '\0';
    g_registeredBoostKeyReleased[0] = '\0';
}

void RebindBoostKey(IPluginSelf* self, const char* newKeyName)
{
    UnregisterBoostKey(self);
    RegisterBoostKeyName(self, newKeyName);
}
