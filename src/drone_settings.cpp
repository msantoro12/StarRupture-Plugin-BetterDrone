#include "drone_settings.h"
#include "drone_config.h"
#include "drone_audio.h"
#include "drone_floor_guard.h"
#include "drone_interact.h"
#include "drone_sprint_key.h"
#include "drone_key_vk.h"
#include "plugin_helpers.h"
#include "object_ref.h"
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

    // The custom key's VK (KeyNameToVk), or, whenever Boost Key is
    // DroneConfig::kBoostKeyFollowsSprint, the resolved Sprint key's VK.
    // OnDroneTick polls this as a fallback alongside the loader's own
    // dispatch -- needed outright for a bare modifier, which the loader
    // never dispatches to keybinds, and for Sprint mode, which registers
    // no keybind with the loader at all.
    std::atomic<int> g_boostKeyVk{ 0 };

    // True when Boost Key is DroneConfig::kBoostKeyFollowsSprint and boost is
    // following the resolved Sprint key instead of a registered custom
    // keybind.
    std::atomic<bool> g_boostFollowsSprint{ false };

    std::atomic<bool> g_lastKnownInDrone{ false };

    // The thread OnDroneTick runs on, so shutdown can tell whether it may
    // touch the live drone. 0 until the first tick.
    std::atomic<DWORD> g_gameThreadId{ 0 };

    float g_currentEffectiveSpeed = 0.0f;

    // Last base speed OnDroneTick observed, so a slider/preset/config change
    // can be told apart from the boost multiplier moving the target. -1
    // guarantees the first tick after load counts as a change.
    float g_lastBaseSpeed = -1.0f;

    // The two names actually registered: Pressed on the full combo, Released
    // on its bare base key (see RegisterBoostKeyName). Kept separately so
    // Unregister always matches what was registered.
    char g_registeredBoostKeyPressed[64]  = {};
    char g_registeredBoostKeyReleased[64] = {};

    IPluginSelf* s_sessionSelf = nullptr;
    bool g_inGameSession = false;

    bool g_wasInDrone = false;

    // Drone Size wanted on the live drone, in cm. Set from the pending
    // request on the tick; starts at the stock radius.
    float g_droneSizeCm = 0.0f;
    bool g_boostWasActive = false;
    bool g_loggedInstanceReport = false;

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
        DroneAudio::OnWorldEnd();
        ResetDroneFloorGuard();

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
    g_drone.origSphereRadius  = cdo->BuildingDroneSphereRadius;
    g_drone.valid             = true;
    g_currentEffectiveSpeed   = *g_drone.speedPerSec;
    g_droneSizeCm             = g_drone.origSphereRadius;

    LOG_DEBUG("InitDroneSettings: CDO found at %p", static_cast<void*>(cdo));
    return true;
}

namespace
{
    // Game thread only. The local player's drone sphere, or nullptr when no
    // drone is out. Re-resolved on every call, never kept.
    SDK::USphereComponent* LiveDroneSphere()
    {
        SDK::ACrCharacterPlayerBase* character = LocalPlayerCharacter();
        SDK::ACrCharacterDroneBase* drone = character ? character->BuildingDrone : nullptr;
        return drone ? drone->SphereComponent : nullptr;
    }

    // Drone Size, applied to the live drone. Game thread only.
    //
    // The game moves the drone by pushing its physics body with a force, so
    // a body that got lighter as it shrank would fly faster. Each drone's
    // body keeps the mass it has at the stock radius: read once, before the
    // first resize, and set as a mass override after every resize. The body
    // is held as an ObjectRef, so a drone that is gone is never touched.
    ObjectRef<SDK::USphereComponent> g_sizedSphere;

    // g_sizedSphere's body as the game made it, at the stock radius.
    struct StockBody
    {
        float massKg = 0.0f;
        float overrideKg = 0.0f;   // BodyInstance.MassInKgOverride
        bool  overridden = false;  // the drone already had a mass override
    };
    StockBody g_stockBody;

    // Temporary diagnostic: one DroneSizeProbe line per resize, written once
    // the radius has held still for kSizeProbeSettleMs, since a slider drag
    // resizes on every frame.
    struct SizeProbe
    {
        bool      pending = false;
        float     massBefore = 0.0f;       // before the first resize of the burst
        float     massAfterResize = 0.0f;  // after the last resize, before the override
        ULONGLONG lastResizeMs = 0;
    };
    SizeProbe g_sizeProbe;
    constexpr ULONGLONG kSizeProbeSettleMs = 500;

    bool IsStockSize(float radiusCm)
    {
        return std::fabs(radiusCm - g_drone.origSphereRadius) <= 0.01f;
    }

    // Sets the body's mass the way the game had it: an override only if
    // the drone came with one.
    void SetStockMassSetting(SDK::USphereComponent* sphere)
    {
        sphere->SetMassOverrideInKg(SDK::FName(), g_stockBody.overrideKg, g_stockBody.overridden);
    }

    void ResizeDroneSphere(SDK::USphereComponent* sphere, float radiusCm)
    {
        if (g_sizedSphere.Get() != sphere)
        {
            // A drone not resized yet spawned at the stock radius, since
            // the CDO is left alone. Anything else is put back first, so the
            // mass read is the stock one.
            if (!IsStockSize(sphere->SphereRadius))
                sphere->SetSphereRadius(g_drone.origSphereRadius, true);

            // Setting the mass the way it already is makes the body work
            // its mass out again, so the read below is the stock shape's.
            g_sizedSphere.Set(sphere);
            g_stockBody.overrideKg = sphere->BodyInstance.MassInKgOverride;
            g_stockBody.overridden = sphere->BodyInstance.bOverrideMass;
            SetStockMassSetting(sphere);
            g_stockBody.massKg = sphere->GetMass();
        }

        if (!g_sizeProbe.pending)
        {
            g_sizeProbe.pending    = true;
            g_sizeProbe.massBefore = sphere->GetMass();
        }

        // The game's own mass setting, applied after the resize so the body
        // works its mass out from the new shape: the probe reads the mass
        // that shape gets by itself.
        sphere->SetSphereRadius(radiusCm, true);
        SetStockMassSetting(sphere);
        g_sizeProbe.massAfterResize = sphere->GetMass();
        g_sizeProbe.lastResizeMs    = GetTickCount64();

        if (!IsStockSize(radiusCm) && g_stockBody.massKg > 0.0f)
            sphere->SetMassOverrideInKg(SDK::FName(), g_stockBody.massKg, true);
    }

    void TickDroneSizeProbe(SDK::USphereComponent* sphere)
    {
        if (!g_sizeProbe.pending || GetTickCount64() - g_sizeProbe.lastResizeMs < kSizeProbeSettleMs)
            return;

        g_sizeProbe.pending = false;
        LOG_INFO("DroneSizeProbe: radius %.1f cm, mass %.2f kg before, %.2f kg after the resize alone, "
                 "%.2f kg applied (stock %.2f kg, game override %s), linear damping %.3f",
            sphere->SphereRadius, g_sizeProbe.massBefore, g_sizeProbe.massAfterResize, sphere->GetMass(),
            g_stockBody.massKg, g_stockBody.overridden ? "yes" : "no", sphere->GetLinearDamping());
    }

    // Puts the drone we resized back as the game made it: the stock radius
    // and its own mass setting. A drone that is gone needs nothing. Game
    // thread only.
    void RestoreDroneBody()
    {
        SDK::USphereComponent* sphere = g_sizedSphere.Get();
        g_sizedSphere.Reset();
        g_sizeProbe.pending = false;
        if (!sphere)
            return;

        if (!IsStockSize(sphere->SphereRadius))
            sphere->SetSphereRadius(g_drone.origSphereRadius, true);
        SetStockMassSetting(sphere);
    }
}

void RestoreCDODefaults()
{
    if (!g_drone.valid)
        return;

    // Shutdown runs on the game thread on loader 1.22 and later; anywhere
    // else, leave the live drone alone.
    if (GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed))
    {
        try
        {
            RestoreDroneBody();
        }
        catch (...) {}
    }

    *g_drone.speedPerSec   = g_drone.origSpeedPerSec;
    *g_drone.maxRadius     = g_drone.origMaxRadius;
    *g_drone.warningRadius = g_drone.origWarningRadius;
    *g_drone.maxHeight     = g_drone.origMaxHeight;
    *g_drone.warningHeight = g_drone.origWarningHeight;

    LOG_DEBUG("RestoreCDODefaults: speed=%.0f maxRadius=%.0f maxHeight=%.0f sphereRadius=%.0f",
        g_drone.origSpeedPerSec, g_drone.origMaxRadius, g_drone.origMaxHeight, g_drone.origSphereRadius);
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
    PendingFloat      g_pendingSphere;
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

void RequestSphereRadius(float radiusCm)
{
    g_pendingSphere.Request(radiusCm);
}

void UpdateBoostKeyCache(const char* keyName)
{
    g_boostKeyVk.store(KeyNameToVk(keyName), std::memory_order_relaxed);
}

void OnDroneTick(float deltaSeconds)
{
    if (!g_drone.valid || !g_drone.speedPerSec)
        return;

    g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

    bool needsInstanceUpdate = false;

    g_pendingSphere.TakeIfPending(&g_droneSizeCm);

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

    const bool justEnteredDrone = inDrone && !g_wasInDrone;
    if (justEnteredDrone)
    {
        needsInstanceUpdate = true;

        // Picks up an in-game rebind of Sprint made mid-session.
        if (g_boostFollowsSprint.load(std::memory_order_relaxed))
            ResolveAndCacheSprintKey();
    }
    // A drone that stays around after the player leaves it goes back to
    // the game's size and mass until the next launch.
    if (!inDrone && g_wasInDrone)
        RestoreDroneBody();
    g_wasInDrone = inDrone;

    // Each drone spawns at the stock radius and is resized here, on its
    // first tick out and whenever Drone Size changes. Reading the sphere
    // each tick also covers a spawn that lands after the launch edge.
    if (inDrone)
    {
        SDK::USphereComponent* sphere = LiveDroneSphere();
        if (sphere)
        {
            if (std::fabs(sphere->SphereRadius - g_droneSizeCm) > 0.01f)
                ResizeDroneSphere(sphere, g_droneSizeCm);
            TickDroneSizeProbe(sphere);
        }
    }

    bool held = g_boostKeyHeld.load(std::memory_order_relaxed);
    if (!held && inDrone)
    {
        const int vk = g_boostKeyVk.load(std::memory_order_relaxed);
        held = vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0 && GameHasFocus();
    }

    const bool boostActive = held && inDrone;

    const float baseSpeed = DroneConfig::Config::ReadSpeedPerSec();
    const bool baseSpeedChanged = std::fabs(baseSpeed - g_lastBaseSpeed) > 0.01f;
    g_lastBaseSpeed = baseSpeed;

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
    // kMinAccelDecel) and still govern the boost engage/disengage edge. A
    // base-speed change (slider, preset, config load, entering the drone)
    // snaps straight to target instead -- the ramp is only meant to smooth
    // boost, not gate every speed change.
    const float accel = DroneConfig::Config::ReadAcceleration();
    const float decel = DroneConfig::Config::ReadDeceleration();

    const bool wasAtTarget = std::fabs(g_currentEffectiveSpeed - targetSpeed) <= 0.01f;

    if (baseSpeedChanged || justEnteredDrone)
    {
        g_currentEffectiveSpeed = targetSpeed;
    }
    else if (g_currentEffectiveSpeed < targetSpeed)
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
