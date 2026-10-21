#include "drone_hit_probe.h"
#include "drone_interact.h"
#include "drone_key_vk.h"
#include "drone_sprint_key.h"
#include "plugin_helpers.h"
#include <BP_FloatingDrone_classes.hpp>
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

const char* CollisionEnabledName(SDK::ECollisionEnabled enabled)
{
    switch (static_cast<int>(enabled))
    {
    case 0:  return "NoCollision";
    case 1:  return "QueryOnly";
    case 2:  return "PhysicsOnly";
    case 3:  return "QueryAndPhysics";
    case 4:  return "ProbeOnly";
    case 5:  return "QueryAndProbe";
    default: return "?";
    }
}

namespace
{
    // How far ahead of the drone sphere the query sweep looks.
    constexpr double   k_sweepCm = 60.0;

    // Each blocking component is logged at most this often.
    constexpr uint64_t k_throttleMs = 2000;

    // Held this long with the drone not moving vertically, and the sweep hits
    // nothing, means whatever stops it is invisible to queries.
    constexpr uint64_t k_stuckMs = 300;
    constexpr double   k_stuckSpeedCmPerSec = 20.0;

    // A drone key the game had not mapped yet is looked up again this often.
    constexpr uint64_t k_keyRetryMs = 1000;

    bool     g_wasInDrone = false;
    bool     g_bodyLogged = false;
    int      g_upVk = 0;
    int      g_downVk = 0;
    uint64_t g_keysResolvedMs = 0;
    uint64_t g_heldSinceMs = 0;
    uint64_t g_noHitLoggedMs = 0;

    // GObjects index of a logged component -> when it was last logged.
    std::unordered_map<int32_t, uint64_t> g_lastLoggedMs;

    std::string ChannelName(SDK::ECollisionChannel channel)
    {
        static const char* const kNames[] = {
            "WorldStatic", "WorldDynamic", "Pawn", "Visibility",
            "Camera", "PhysicsBody", "Vehicle", "Destructible" };
        const int c = static_cast<int>(channel);
        if (c >= 0 && c < 8)
            return kNames[c];
        if (c >= 8 && c <= 13)
            return "EngineTraceChannel" + std::to_string(c - 7);
        if (c >= 14 && c <= 31)
            return "GameTraceChannel" + std::to_string(c - 13);
        return "Channel" + std::to_string(c);
    }

    const char* ResponseName(SDK::ECollisionResponse response)
    {
        switch (response)
        {
        case SDK::ECollisionResponse::ECR_Ignore:  return "Ignore";
        case SDK::ECollisionResponse::ECR_Overlap: return "Overlap";
        case SDK::ECollisionResponse::ECR_Block:   return "Block";
        default:                                   return "?";
        }
    }

    // The component's response to each object channel (and the first game
    // channels, where a project's own object types live), one letter each:
    // I(gnore), O(verlap), B(lock).
    std::string ResponseRow(SDK::UPrimitiveComponent* comp)
    {
        std::string row;
        for (int c = 0; c <= 21; ++c)
        {
            if (c >= 8 && c <= 13)
                continue;
            const auto response = comp->GetCollisionResponseToChannel(static_cast<SDK::ECollisionChannel>(c));
            row += ChannelName(static_cast<SDK::ECollisionChannel>(c));
            row += '=';
            row += ResponseName(response)[0];
            row += ' ';
        }
        if (!row.empty())
            row.pop_back();
        return row;
    }

    std::string ShapeText(SDK::UPrimitiveComponent* comp)
    {
        char buf[160] = {};
        if (comp->IsA(SDK::USphereComponent::StaticClass()))
        {
            snprintf(buf, sizeof(buf), "sphere r=%.1f",
                static_cast<SDK::USphereComponent*>(comp)->GetScaledSphereRadius());
        }
        else if (comp->IsA(SDK::UCapsuleComponent::StaticClass()))
        {
            float radius = 0.0f, halfHeight = 0.0f;
            static_cast<SDK::UCapsuleComponent*>(comp)->GetScaledCapsuleSize(&radius, &halfHeight);
            snprintf(buf, sizeof(buf), "capsule r=%.1f halfHeight=%.1f", radius, halfHeight);
        }
        else if (comp->IsA(SDK::UBoxComponent::StaticClass()))
        {
            const SDK::FVector e = static_cast<SDK::UBoxComponent*>(comp)->GetScaledBoxExtent();
            snprintf(buf, sizeof(buf), "box extent=(%.1f %.1f %.1f)", e.X, e.Y, e.Z);
        }

        SDK::FVector origin{}, extent{};
        float boundsRadius = 0.0f;
        SDK::UKismetSystemLibrary::GetComponentBounds(comp, &origin, &extent, &boundsRadius);

        char bounds[128] = {};
        snprintf(bounds, sizeof(bounds), "%sbounds extent=(%.1f %.1f %.1f) r=%.1f",
            buf[0] ? ", " : "", extent.X, extent.Y, extent.Z, boundsRadius);
        return std::string(buf) + bounds;
    }

    // Once per drone: the drone's collision body. Each primitive component
    // the drone actor owns, so a welded child wider than the sphere shows.
    void LogDroneBody(SDK::ACrCharacterDroneBase* drone, SDK::USphereComponent* sphere)
    {
        SDK::USceneComponent* root = drone->RootComponent;
        LOG_INFO("DroneHitProbe: drone %s (%s), root %s (%s), drone sphere %s profile %s object type %s%s",
            drone->GetName().c_str(), drone->Class->GetName().c_str(),
            root ? root->GetName().c_str() : "none", root ? root->Class->GetName().c_str() : "-",
            sphere->GetName().c_str(), sphere->GetCollisionProfileName().ToString().c_str(),
            ChannelName(sphere->GetCollisionObjectType()).c_str(),
            static_cast<SDK::USceneComponent*>(sphere) == root ? " (is the root)" : "");

        auto* objects = SDK::UObject::GObjects.GetTypedPtr();
        if (!objects)
            return;

        const SDK::UClass* primitiveClass = SDK::UPrimitiveComponent::StaticClass();
        const int32_t count = objects->Num();
        for (int32_t i = 0; i < count; ++i)
        {
            SDK::UObject* obj = objects->GetByIndex(i);
            if (!obj || obj->Outer != drone || obj->IsDefaultObject() || !obj->IsA(primitiveClass))
                continue;

            auto* comp = static_cast<SDK::UPrimitiveComponent*>(obj);
            SDK::USceneComponent* parent = comp->GetAttachParent();
            LOG_INFO("DroneHitProbe: drone part %s (%s) collision %s, profile %s, object type %s, simulating %d, "
                     "attached to %s, %s, responses %s",
                comp->GetName().c_str(), comp->Class->GetName().c_str(),
                CollisionEnabledName(comp->GetCollisionEnabled()),
                comp->GetCollisionProfileName().ToString().c_str(),
                ChannelName(comp->GetCollisionObjectType()).c_str(),
                comp->IsSimulatingPhysics(SDK::FName()) ? 1 : 0,
                parent ? parent->GetName().c_str() : "none",
                ShapeText(comp).c_str(), ResponseRow(comp).c_str());
        }
    }

    // An object-type sweep, for a shape whose profile is Custom and so has no
    // name to sweep by, covers the first six object types.
    constexpr int32_t k_objectTypes = 6;

    // Fills storage with those types and returns a view over it. The engine
    // copies the array before the sweep returns.
    SDK::TArray<SDK::EObjectTypeQuery> AllObjectTypes(SDK::EObjectTypeQuery (&storage)[k_objectTypes])
    {
        for (int32_t i = 0; i < k_objectTypes; ++i)
            storage[i] = static_cast<SDK::EObjectTypeQuery>(i);
        return SDK::TArray<SDK::EObjectTypeQuery>(storage, k_objectTypes, k_objectTypes);
    }

    // The plugin, when its engine allocator can free an array a sweep filled.
    IPluginSelf* PluginAllocator()
    {
        IPluginSelf* self = GetSelf();
        if (!self || !self->hooks || !self->hooks->Memory || !self->hooks->Memory->IsAllocatorAvailable())
            return nullptr;
        return self;
    }

    // An object-type sweep counts every shape of those types as a hit,
    // triggers included. Keeps the nearest hit that blocks shape both ways,
    // and frees the array the engine allocated.
    bool TakeNearestMutualBlock(IPluginSelf* self, SDK::TArray<SDK::FHitResult>& hits,
                                SDK::UPrimitiveComponent* shape, SDK::FHitResult* hit)
    {
        const SDK::ECollisionChannel shapeType = shape->GetCollisionObjectType();
        bool found = false;
        for (int32_t i = 0; i < hits.Num() && !found; ++i)
        {
            SDK::UPrimitiveComponent* comp = hits[i].Component.Get();
            if (comp &&
                comp->GetCollisionResponseToChannel(shapeType) == SDK::ECollisionResponse::ECR_Block &&
                shape->GetCollisionResponseToChannel(comp->GetCollisionObjectType()) == SDK::ECollisionResponse::ECR_Block)
            {
                *hit = hits[i];
                found = true;
            }
        }

        if (hits.GetDataPtr())
            self->hooks->Memory->Free(const_cast<SDK::FHitResult*>(hits.GetDataPtr()));
        return found;
    }

    // A query sweep of the drone sphere from where it is, in the direction it
    // is being flown. Moves nothing. Returns true and fills hit on a blocking
    // hit.
    bool SweepDroneSphere(SDK::ACrCharacterPlayerBase* character, SDK::ACrCharacterDroneBase* drone,
                          SDK::USphereComponent* sphere, double dirZ, SDK::FHitResult* hit)
    {
        const SDK::FVector start = sphere->K2_GetComponentLocation();
        SDK::FVector end = start;
        end.Z += dirZ * k_sweepCm;
        const float radius = sphere->GetScaledSphereRadius();

        // The engine copies these arrays before the call returns; they are
        // views over stack slots.
        SDK::AActor* ignoreData[1] = { drone };
        const SDK::TArray<SDK::AActor*> ignore(ignoreData, 1, 1);
        const SDK::FLinearColor colour{};

        // A named profile sweeps exactly as the drone body collides.
        const SDK::FName profile = sphere->GetCollisionProfileName();
        if (profile.ToString() != "Custom")
        {
            return SDK::UKismetSystemLibrary::SphereTraceSingleByProfile(character, start, end, radius, profile,
                false, ignore, SDK::EDrawDebugTrace::None, hit, true, colour, colour, 0.0f) && hit->bBlockingHit;
        }

        // A custom profile has no name to look up: sweep for objects instead.
        IPluginSelf* self = PluginAllocator();
        if (!self)
            return false;

        SDK::EObjectTypeQuery typeData[k_objectTypes];
        SDK::TArray<SDK::FHitResult> hits;
        SDK::UKismetSystemLibrary::SphereTraceMultiForObjects(character, start, end, radius, AllObjectTypes(typeData),
            false, ignore, SDK::EDrawDebugTrace::None, &hits, true, colour, colour, 0.0f);
        return TakeNearestMutualBlock(self, hits, sphere, hit);
    }

    // A query sweep of the drone mesh's bounds (start, halfSize), as a box,
    // upward from where the mesh is. Moves nothing. Returns true and fills hit on a blocking
    // hit. Shows whether the mesh, wider than a shrunken sphere, is what
    // catches.
    bool SweepDroneMesh(SDK::ACrCharacterPlayerBase* character, SDK::ACrCharacterDroneBase* drone,
                        SDK::UPrimitiveComponent* mesh, const SDK::FVector& start, const SDK::FVector& halfSize,
                        SDK::FHitResult* hit)
    {
        SDK::FVector end = start;
        end.Z += k_sweepCm;

        // Bounds are world-aligned, so the box is not rotated.
        SDK::AActor* ignoreData[1] = { drone };
        const SDK::TArray<SDK::AActor*> ignore(ignoreData, 1, 1);
        const SDK::FLinearColor colour{};
        const SDK::FRotator orientation{};

        const SDK::FName profile = mesh->GetCollisionProfileName();
        if (profile.ToString() != "Custom")
        {
            return SDK::UKismetSystemLibrary::BoxTraceSingleByProfile(character, start, end, halfSize, orientation,
                profile, false, ignore, SDK::EDrawDebugTrace::None, hit, true, colour, colour, 0.0f) && hit->bBlockingHit;
        }

        IPluginSelf* self = PluginAllocator();
        if (!self)
            return false;

        SDK::EObjectTypeQuery typeData[k_objectTypes];
        SDK::TArray<SDK::FHitResult> hits;
        SDK::UKismetSystemLibrary::BoxTraceMultiForObjects(character, start, end, halfSize, orientation,
            AllObjectTypes(typeData), false, ignore, SDK::EDrawDebugTrace::None, &hits, true, colour, colour, 0.0f);
        return TakeNearestMutualBlock(self, hits, mesh, hit);
    }

    std::string SphereText(SDK::USphereComponent* sphere)
    {
        char buf[48] = {};
        snprintf(buf, sizeof(buf), "drone sphere r=%.1f", sphere->GetScaledSphereRadius());
        return buf;
    }

    void LogHit(const SDK::FHitResult& hit, SDK::UPrimitiveComponent* comp, SDK::ACrCharacterDroneBase* drone,
                SDK::UPrimitiveComponent* shape, const std::string& sweptWith, const char* direction, uint64_t now)
    {
        uint64_t& last = g_lastLoggedMs[comp->Index];
        if (last != 0 && now - last < k_throttleMs)
            return;
        last = now;

        SDK::AActor* owner = comp->GetOwner();
        const SDK::ECollisionChannel droneType = shape->GetCollisionObjectType();
        const SDK::ECollisionChannel hitType = comp->GetCollisionObjectType();
        const SDK::FVector at = shape->K2_GetComponentLocation();
        const SDK::FVector velocity = drone->GetVelocity();
        LOG_INFO("DroneHitProbe: flying %s (held %.1f s, vertical speed %.1f cm/s), in the way: actor %s (%s), component %s (%s) item %d; "
                 "collision %s, object type %s, profile %s, its response to the drone (%s) %s, drone's response to it %s; "
                 "normal (%.2f %.2f %.2f) at (%.1f %.1f %.1f), distance %.1f%s; swept %s from (%.1f %.1f %.1f)",
            direction, (now - g_heldSinceMs) / 1000.0, velocity.Z,
            owner ? owner->GetName().c_str() : "none", owner ? owner->Class->GetName().c_str() : "-",
            comp->GetName().c_str(), comp->Class->GetName().c_str(), hit.Item,
            CollisionEnabledName(comp->GetCollisionEnabled()), ChannelName(hitType).c_str(),
            comp->GetCollisionProfileName().ToString().c_str(),
            ChannelName(droneType).c_str(), ResponseName(comp->GetCollisionResponseToChannel(droneType)),
            ResponseName(shape->GetCollisionResponseToChannel(hitType)),
            hit.ImpactNormal.X, hit.ImpactNormal.Y, hit.ImpactNormal.Z,
            hit.ImpactPoint.X, hit.ImpactPoint.Y, hit.ImpactPoint.Z,
            hit.Distance, hit.bStartPenetrating ? " (started inside it)" : "",
            sweptWith.c_str(), at.X, at.Y, at.Z);
    }

    // Looks the drone's Up and Down keys up from the game's own mappings.
    // Logged on the first try of a launch and when a missing key turns up.
    void ResolveKeys(bool firstTry)
    {
        char upName[64] = {}, downName[64] = {};
        const int upVk   = ResolveInputHandlerVk(SDK::UCrInputNativeDroneMoveUp::StaticClass(), upName, sizeof(upName));
        const int downVk = ResolveInputHandlerVk(SDK::UCrInputNativeDroneMoveDown::StaticClass(), downName, sizeof(downName));
        const bool changed = upVk != g_upVk || downVk != g_downVk;
        g_upVk   = upVk;
        g_downVk = downVk;
        g_keysResolvedMs = GetTickCount64();
        if (firstTry || changed)
        {
            LOG_INFO("DroneHitProbe: drone up key %s, down key %s",
                upName[0] ? upName : "(not found)", downName[0] ? downName : "(not found)");
        }
    }

    void StartLaunch()
    {
        g_lastLoggedMs.clear();
        g_bodyLogged = false;
        g_heldSinceMs = 0;
        g_noHitLoggedMs = 0;
        g_upVk = 0;
        g_downVk = 0;
        ResolveKeys(true);
    }

    bool KeyHeld(int vk)
    {
        return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
}

void TickDroneHitProbe()
{
    SDK::ACrCharacterPlayerBase* character = LocalPlayerCharacter();
    const bool inDrone = character && character->Status == SDK::EPlayerCharacterStatus::BuildingDrone;
    if (inDrone && !g_wasInDrone)
        StartLaunch();
    g_wasInDrone = inDrone;
    if (!inDrone)
        return;

    SDK::ACrCharacterDroneBase* drone = character->BuildingDrone;
    SDK::USphereComponent* sphere = drone ? drone->SphereComponent : nullptr;
    if (!sphere)
        return;

    if (!g_bodyLogged)
    {
        g_bodyLogged = true;
        LogDroneBody(drone, sphere);
    }

    if ((g_upVk == 0 || g_downVk == 0) && GetTickCount64() - g_keysResolvedMs >= k_keyRetryMs)
        ResolveKeys(false);

    const bool focused = GameHasFocus();
    const bool up   = focused && KeyHeld(g_upVk);
    const bool down = focused && KeyHeld(g_downVk);
    if (up == down)
    {
        g_heldSinceMs = 0;
        return;
    }

    const uint64_t now = GetTickCount64();
    if (g_heldSinceMs == 0)
        g_heldSinceMs = now;

    SDK::FHitResult hit{};
    if (SweepDroneSphere(character, drone, sphere, up ? 1.0 : -1.0, &hit))
    {
        if (SDK::UPrimitiveComponent* comp = hit.Component.Get())
        {
            LogHit(hit, comp, drone, sphere, SphereText(sphere), up ? "up" : "down", now);
            return;
        }
    }

    // Nothing in the way to a query, yet the drone is not moving (or, flown
    // up, sinking): the blocker is a physics-only shape, or part of the
    // drone wider than the sphere.
    const SDK::FVector velocity = drone->GetVelocity();
    const bool stuck = up ? velocity.Z < k_stuckSpeedCmPerSec : std::abs(velocity.Z) < k_stuckSpeedCmPerSec;
    if (now - g_heldSinceMs < k_stuckMs || !stuck ||
        (g_noHitLoggedMs != 0 && now - g_noHitLoggedMs < k_throttleMs))
        return;

    g_noHitLoggedMs = now;
    const SDK::FVector at = sphere->K2_GetComponentLocation();
    LOG_INFO("DroneHitProbe: flying %s, held %.1f s, vertical speed %.1f cm/s, no query-visible blocker "
             "within %.0f cm; drone at (%.1f %.1f %.1f) radius %.1f",
        up ? "up" : "down", (now - g_heldSinceMs) / 1000.0, velocity.Z, k_sweepCm,
        at.X, at.Y, at.Z, sphere->GetScaledSphereRadius());

    // The mesh is wider than a shrunken sphere: sweep its bounds as well.
    if (!up || !drone->IsA(SDK::ABP_FloatingDrone_C::StaticClass()))
        return;
    SDK::UStaticMeshComponent* mesh = static_cast<SDK::ABP_FloatingDrone_C*>(drone)->SM_BuidingDrone;
    if (!mesh)
        return;

    SDK::FVector origin{}, extent{};
    float boundsRadius = 0.0f;
    SDK::UKismetSystemLibrary::GetComponentBounds(mesh, &origin, &extent, &boundsRadius);
    char meshText[96] = {};
    snprintf(meshText, sizeof(meshText), "drone mesh %s as a box (%.1f %.1f %.1f)",
        mesh->GetName().c_str(), extent.X, extent.Y, extent.Z);

    SDK::FHitResult meshHit{};
    SDK::UPrimitiveComponent* meshBlocker = SweepDroneMesh(character, drone, mesh, origin, extent, &meshHit)
        ? meshHit.Component.Get() : nullptr;
    if (meshBlocker)
        LogHit(meshHit, meshBlocker, drone, mesh, meshText, "up", now);
    else
        LOG_INFO("DroneHitProbe: flying up, %s finds no query-visible blocker within %.0f cm either", meshText, k_sweepCm);
}
