#include "drone_settings.h"
#include "drone_config.h"
#include "drone_audio.h"
#include "drone_body_anchor.h"
#include "drone_floor_guard.h"
#include "drone_held_key.h"
#include "drone_hit_probe.h"
#include "drone_interact.h"
#include "drone_sprint_key.h"
#include "plugin_helpers.h"
#include "object_ref.h"
#include "drone_fields.h"
#include <AuActorPlacement_classes.hpp>
#include <Chimera_classes.hpp>
#include <BP_FloatingDrone_classes.hpp>
#include <Basic.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <windows.h>

DroneSettings g_drone;

namespace
{
    std::atomic<bool> g_lastKnownInDrone{ false };

    // Written by the keybind callbacks, acted on in OnDroneTick. A HeldKey's
    // VK is the custom key's (KeyNameToVk) or, whenever Boost Key is
    // DroneConfig::kBoostKeyFollowsSprint, the resolved Sprint key's. The
    // tick polls it as a fallback alongside the loader's own dispatch --
    // needed outright for a bare modifier, which the loader never dispatches
    // to keybinds, and for Sprint mode, which registers no keybind with the
    // loader at all.
    HeldKey g_boost{ "boost", g_lastKnownInDrone };
    HeldKey g_slow{ "slow", g_lastKnownInDrone };

    // True when Boost Key is DroneConfig::kBoostKeyFollowsSprint and boost is
    // following the resolved Sprint key instead of a registered custom
    // keybind.
    std::atomic<bool> g_boostFollowsSprint{ false };

    float g_currentEffectiveSpeed = 0.0f;

    // Last base speed OnDroneTick observed, so a slider/preset/config change
    // can be told apart from the boost multiplier moving the target. -1
    // guarantees the first tick after load counts as a change.
    float g_lastBaseSpeed = -1.0f;

    IPluginSelf* s_sessionSelf = nullptr;
    bool g_inGameSession = false;

    bool g_wasInDrone = false;

    // Drone Size wanted on the live drone, in cm. Set from the pending
    // request on the tick; starts at the stock radius.
    float g_droneSizeCm = 0.0f;
    bool g_boostWasActive = false;
    bool g_slowWasActive = false;

    // The Slow key has been compared with the game's drone Up and Down keys
    // since it was last bound. Cleared by a (re)bind, set on the game thread.
    std::atomic<bool> g_slowClashChecked{ false };
    bool g_loggedInstanceReport = false;

    void OnBoostKeyEvent(EModKey, EModKeyEvent event) { g_boost.OnEvent(event); }
    void OnSlowKeyEvent(EModKey, EModKeyEvent event)  { g_slow.OnEvent(event); }

    // The Sprint lookup's state: written on the game thread only (a lookup
    // reads the world, so it is never made from the init or render thread).
    // pending: Boost Key follows Sprint and its key is not settled yet.
    FollowedKey g_sprintKey;

    // Resolves the game's current Sprint key and caches its VK for
    // OnDroneTick to poll -- following Sprint registers no keybind with the
    // loader, so nothing else would dispatch it. LeftShift stands in, logged
    // once, only when Sprint has no keyboard binding, or the lookup still
    // misses with the player in the drone.
    void ResolveAndCacheSprintKey(bool inDrone)
    {
        char keyName[64] = {};
        const FollowedKey::Result result = g_sprintKey.Resolve(
            SDK::UCrInputSprint::StaticClass(), inDrone, VK_LSHIFT, keyName, sizeof(keyName));
        if (!result.followed && !result.fellBack)
            return;

        g_boost.SetVk(g_sprintKey.vk);
        if (result.followed)
            LOG_INFO("boost key: following Sprint (%s)", keyName);
        else if (result.firstFallback)
            LOG_WARN("boost key: could not resolve the game's Sprint key (%s), using LeftShift", result.why);
    }

    // keyName == kBoostKeyFollowsSprint is not a real key: nothing is
    // registered with the loader for it, and OnDroneTick's poll of
    // g_boost's VK (kept fed by ResolveAndCacheSprintKey) stands in for
    // dispatch instead.
    void RegisterBoostKeyName(IPluginSelf* self, const char* keyName)
    {
        if (!self || !self->hooks->Input || !keyName || !keyName[0])
            return;

        if (strcmp(keyName, DroneConfig::kBoostKeyFollowsSprint) == 0)
        {
            g_boostFollowsSprint.store(true, std::memory_order_relaxed);
            g_boost.SetVk(0);
            g_sprintKey.Restart();
            return;
        }

        g_boostFollowsSprint.store(false, std::memory_order_relaxed);
        g_boost.Register(self, keyName, OnBoostKeyEvent);
    }

    // Slow is a held key like the game's own drone Up and Down, so a Slow key
    // that is also one of them would drop or climb the drone while slowing
    // it. Warns once per binding. The lookup needs the game's input, which
    // exists by the time the player is in the drone; until a key is found
    // the check is tried again on the next launch.
    void CheckSlowKeyClash()
    {
        if (g_slowClashChecked.load(std::memory_order_relaxed) || !DroneFields::Features::SprintKey.On())
            return;

        const int slowVk = g_slow.Vk();
        if (slowVk == 0)
            return;

        char upName[64] = {}, downName[64] = {};
        const int upVk   = ResolveInputHandlerVk(SDK::UCrInputNativeDroneMoveUp::StaticClass(), upName, sizeof(upName));
        const int downVk = ResolveInputHandlerVk(SDK::UCrInputNativeDroneMoveDown::StaticClass(), downName, sizeof(downName));
        if (upVk == 0 && downVk == 0)
            return;

        g_slowClashChecked.store(true, std::memory_order_relaxed);
        if (slowVk == upVk || slowVk == downVk)
            LOG_WARN("slow key: %s is also the game's drone %s key, so slowing down will also move the drone; "
                     "pick another Slow Key on the loader's Config page",
                     slowVk == upVk ? upName : downName, slowVk == upVk ? "Up" : "Down");
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
            ResolveAndCacheSprintKey(false);
        g_sprintKey.tries = 0;
    }

    void OnWorldEndPlay(SDK::UWorld*, const char* worldName)
    {
        DroneAudio::OnWorldEnd();
        ResetDroneFloorGuard();
        ResetDroneBodyAnchor();

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
        SDK::UWorld* world = PluginSdkRuntime::GetWorld();
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

    g_drone.speedPerSec   = DroneFields::SpeedPerSec.In(cdo);
    g_drone.maxRadius     = DroneFields::MaxRadius.In(cdo);
    g_drone.warningRadius = DroneFields::WarningRadius.In(cdo);
    g_drone.maxHeight     = DroneFields::MaxHeight.In(cdo);
    g_drone.warningHeight = DroneFields::WarningHeight.In(cdo);
    const float* sphereRadius = DroneFields::SphereRadius.In(cdo);
    if (!g_drone.speedPerSec || !g_drone.maxRadius || !g_drone.warningRadius || !g_drone.maxHeight
        || !g_drone.warningHeight || !sphereRadius)
    {
        LOG_WARN("InitDroneSettings: a drone setting did not resolve on this build");
        g_drone = DroneSettings{};
        return false;
    }
    g_drone.origSpeedPerSec   = *g_drone.speedPerSec;
    g_drone.origMaxRadius     = *g_drone.maxRadius;
    g_drone.origWarningRadius = *g_drone.warningRadius;
    g_drone.origMaxHeight     = *g_drone.maxHeight;
    g_drone.origWarningHeight = *g_drone.warningHeight;
    g_drone.origSphereRadius  = *sphereRadius;
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
        SDK::ACrCharacterDroneBase* drone = DroneFields::ValueOr(DroneFields::BuildingDrone, character);
        return DroneFields::ValueOr(DroneFields::SphereComponent, drone);
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

    // The drone's mesh, SM_BuidingDrone, is welded to the sphere's physics
    // body and blocks the world with a plate about 43 x 48 cm that does not
    // shrink with Drone Size. Below the stock radius it is the widest part
    // of the body and catches on edges the sphere would pass, so it leaves
    // the physics body (keeping its query collision) until the drone is back
    // at the stock radius. mesh is set only while its collision is changed.
    struct ChangedPlate
    {
        ObjectRef<SDK::UStaticMeshComponent> mesh;
        SDK::ECollisionEnabled original = SDK::ECollisionEnabled::NoCollision;
        SDK::FName profile;
        bool       namedProfile = false;  // profile is a named preset, not Custom
    };
    ChangedPlate g_plate;

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

    // Game thread only. Re-resolved on every call, never kept.
    SDK::ABP_FloatingDrone_C* FloatingDrone(SDK::USphereComponent* sphere)
    {
        SDK::AActor* owner = sphere->GetOwner();
        if (!owner || !owner->IsA(SDK::ABP_FloatingDrone_C::StaticClass()))
            return nullptr;
        return static_cast<SDK::ABP_FloatingDrone_C*>(owner);
    }

    // The drone's body mesh, or null when the drone is null or the Blueprint
    // has no such member on this build.
    SDK::UStaticMeshComponent* DroneMeshOf(SDK::ABP_FloatingDrone_C* drone)
    {
        SDK::UStaticMeshComponent** at = DroneFields::DroneMesh.In(DroneFields::ObjectProperties(), drone);
        return at ? *at : nullptr;
    }

    // Gives the mesh back the collision the game gave it. Setting the
    // collision state renames the profile to Custom, so a named profile is
    // put back by name. A mesh that is gone needs nothing.
    void RestorePlate()
    {
        SDK::UStaticMeshComponent* mesh = g_plate.mesh.Get();
        g_plate.mesh.Reset();
        if (!mesh)
            return;

        // The state first: setting it is what welds the mesh back into the
        // drone's physics body. Loading a profile writes the state without
        // rebuilding the body, so it only puts the name back.
        mesh->SetCollisionEnabled(g_plate.original);
        if (g_plate.namedProfile)
            mesh->SetCollisionProfileName(g_plate.profile, false);
    }

    // Below the stock radius, takes the mesh out of the physics body and
    // leaves its query collision as it was; at or above it, restores it.
    void ApplyPlateCollision(SDK::USphereComponent* sphere, float radiusCm)
    {
        SDK::ABP_FloatingDrone_C* drone = FloatingDrone(sphere);
        SDK::UStaticMeshComponent* mesh = DroneMeshOf(drone);
        const bool shrink = mesh && radiusCm < g_drone.origSphereRadius - 0.01f;

        if (!shrink || g_plate.mesh.Get() != mesh)
            RestorePlate();
        if (!shrink || g_plate.mesh.Get() == mesh)
            return;

        const SDK::ECollisionEnabled original = mesh->GetCollisionEnabled();
        SDK::ECollisionEnabled target;
        if (original == SDK::ECollisionEnabled::QueryAndPhysics)
            target = SDK::ECollisionEnabled::QueryOnly;
        else if (original == SDK::ECollisionEnabled::PhysicsOnly)
            target = SDK::ECollisionEnabled::NoCollision;
        else
            return;  // not part of the physics body to begin with

        g_plate.mesh.Set(mesh);
        g_plate.original     = original;
        g_plate.profile      = mesh->GetCollisionProfileName();
        g_plate.namedProfile = g_plate.profile.ToString() != "Custom";
        mesh->SetCollisionEnabled(target);
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
        ApplyPlateCollision(sphere, radiusCm);
        SetStockMassSetting(sphere);
        g_sizeProbe.massAfterResize = sphere->GetMass();
        g_sizeProbe.lastResizeMs    = GetTickCount64();

        if (!IsStockSize(radiusCm) && g_stockBody.massKg > 0.0f)
        {
            // The override sets the sphere's own mass, but the body's total
            // also counts the parts welded to it. Take their share off once,
            // so the whole body weighs what it did at the stock radius.
            sphere->SetMassOverrideInKg(SDK::FName(), g_stockBody.massKg, true);
            const float overrideKg = 2.0f * g_stockBody.massKg - sphere->GetMass();
            if (overrideKg > 0.0f && std::fabs(overrideKg - g_stockBody.massKg) > 0.01f)
                sphere->SetMassOverrideInKg(SDK::FName(), overrideKg, true);
        }
    }

    void TickDroneSizeProbe(SDK::USphereComponent* sphere)
    {
        if (!g_sizeProbe.pending || GetTickCount64() - g_sizeProbe.lastResizeMs < kSizeProbeSettleMs)
            return;

        g_sizeProbe.pending = false;

        SDK::ABP_FloatingDrone_C* drone = FloatingDrone(sphere);
        SDK::UStaticMeshComponent* mesh = DroneMeshOf(drone);
        SDK::UCapsuleComponent** capsuleAt = DroneFields::DroneCapsule.In(DroneFields::ObjectProperties(), drone);
        SDK::UCapsuleComponent* capsule = capsuleAt ? *capsuleAt : nullptr;
        float capsuleRadius = 0.0f, capsuleHalfHeight = 0.0f;
        if (capsule)
            capsule->GetScaledCapsuleSize(&capsuleRadius, &capsuleHalfHeight);

        const std::string meshState = !mesh ? std::string("(no mesh)")
            : g_plate.mesh.Get() == mesh
                ? std::string(CollisionEnabledName(mesh->GetCollisionEnabled())) + " (changed from " +
                      CollisionEnabledName(g_plate.original) + ")"
                : std::string(CollisionEnabledName(mesh->GetCollisionEnabled())) + " (the game's)";

        LOG_INFO("DroneSizeProbe: radius %.1f cm, mass %.2f kg before, %.2f kg after the resize alone, "
                 "%.2f kg applied (stock %.2f kg, game override %s), linear damping %.3f; "
                 "mesh collision %s, capsule r=%.1f halfHeight=%.1f",
            sphere->SphereRadius, g_sizeProbe.massBefore, g_sizeProbe.massAfterResize, sphere->GetMass(),
            g_stockBody.massKg, g_stockBody.overridden ? "yes" : "no", sphere->GetLinearDamping(),
            meshState.c_str(), capsuleRadius, capsuleHalfHeight);
    }

    // Puts the drone we resized back as the game made it: the stock radius
    // and its own mass setting. A drone that is gone needs nothing. Game
    // thread only.
    void RestoreDroneBody()
    {
        RestorePlate();

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
    if (OnGameThread())
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
        SDK::UAuActorPlacementDeveloperSettings* settings = DroneFields::ValueOr(DroneFields::PlacementDeveloperSettings, drone);
        if (!settings)
        {
            LOG_DEBUG("UpdateActiveDrones: drone instance has null PlacementDeveloperSettings, skipping");
            continue;
        }

        ++found;

        float* speedAt         = DroneFields::SpeedPerSec.In(settings);
        float* maxRadiusAt     = DroneFields::MaxRadius.In(settings);
        float* warningRadiusAt = DroneFields::WarningRadius.In(settings);
        float* maxHeightAt     = DroneFields::MaxHeight.In(settings);
        float* warningHeightAt = DroneFields::WarningHeight.In(settings);

        // Already written through the CDO.
        if (speedAt == g_drone.speedPerSec)
        {
            ++sharedCdo;
            continue;
        }

        if (!speedAt || !maxRadiusAt || !warningRadiusAt || !maxHeightAt || !warningHeightAt)
            continue;

        *speedAt         = *g_drone.speedPerSec;
        *maxRadiusAt     = *g_drone.maxRadius;
        *warningRadiusAt = *g_drone.warningRadius;
        *maxHeightAt     = *g_drone.maxHeight;
        *warningHeightAt = *g_drone.warningHeight;
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

void OnDroneTick(float deltaSeconds)
{
    if (!g_drone.valid || !g_drone.speedPerSec)
        return;

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
        CheckSlowKeyClash();

        // Picks up an in-game rebind of Sprint made mid-session.
        if (g_boostFollowsSprint.load(std::memory_order_relaxed))
            ResolveAndCacheSprintKey(true);
    }
    else if (g_boostFollowsSprint.load(std::memory_order_relaxed) && g_sprintKey.Due())
    {
        ResolveAndCacheSprintKey(inDrone);
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

    const bool boostActive = g_boost.Held(inDrone) && inDrone;
    const bool slowActive  = g_slow.Held(inDrone) && inDrone;

    const float baseSpeed = DroneConfig::Config::ReadSpeedPerSec();
    const bool baseSpeedChanged = std::fabs(baseSpeed - g_lastBaseSpeed) > 0.01f;
    g_lastBaseSpeed = baseSpeed;

    // Slow wins over Boost: with both held the drone flies at base x slow.
    float targetSpeed = baseSpeed;
    if (slowActive)
        targetSpeed = baseSpeed * DroneConfig::Config::ReadSlowMultiplier();
    else if (boostActive)
        targetSpeed = baseSpeed * DroneConfig::Config::ReadBoostMultiplier();
    if (targetSpeed > DroneConfig::Config::MaxSpeedPerSec())
        targetSpeed = DroneConfig::Config::MaxSpeedPerSec();

    if (boostActive != g_boostWasActive)
    {
        g_boostWasActive = boostActive;
        const float boostSpeed = std::fmin(baseSpeed * DroneConfig::Config::ReadBoostMultiplier(),
                                           DroneConfig::Config::MaxSpeedPerSec());
        LOG_INFO("OnDroneTick: boost %s, target=%.0f cm/s",
            boostActive ? "engaged" : "disengaged", boostSpeed);
    }

    if (slowActive != g_slowWasActive)
    {
        g_slowWasActive = slowActive;
        LOG_INFO("OnDroneTick: slow %s, target=%.0f cm/s",
            slowActive ? "engaged" : "disengaged", targetSpeed);
    }

    // Acceleration/Deceleration are clamped to a floor above 0 (DroneConfig,
    // kMinAccelDecel) and still govern the boost and slow engage/disengage
    // edges. A base-speed change (slider, preset, config load, entering the
    // drone) snaps straight to target instead -- the ramp is only meant to
    // smooth boost and slow, not gate every speed change.
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
    g_boost.Unregister(self);
}

void RebindBoostKey(IPluginSelf* self, const char* newKeyName)
{
    UnregisterBoostKey(self);
    RegisterBoostKeyName(self, newKeyName);
}

void RegisterSlowKey(IPluginSelf* self)
{
    if (!self || !self->hooks->Input)
        return;

    char keyName[64] = {};
    DroneConfig::Config::ReadSlowKey(keyName, sizeof(keyName));
    g_slow.Register(self, keyName, OnSlowKeyEvent);
    g_slowClashChecked.store(false, std::memory_order_relaxed);
}

void UnregisterSlowKey(IPluginSelf* self)
{
    g_slow.Unregister(self);
}

void RebindSlowKey(IPluginSelf* self, const char* newKeyName)
{
    UnregisterSlowKey(self);
    g_slow.Register(self, newKeyName, OnSlowKeyEvent);
    g_slowClashChecked.store(false, std::memory_order_relaxed);
}
