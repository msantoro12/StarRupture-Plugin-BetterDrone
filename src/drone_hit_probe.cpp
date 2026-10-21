#include "drone_hit_probe.h"
#include "drone_interact.h"
#include "drone_key_vk.h"
#include "drone_sprint_key.h"
#include "plugin_helpers.h"
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

        // A custom profile has no name to look up. An object-type sweep counts
        // every shape of those types as a hit, triggers included, so take all
        // of them and keep the nearest that blocks the drone both ways. The
        // engine allocates the result array; it is freed here.
        IPluginSelf* self = GetSelf();
        if (!self || !self->hooks || !self->hooks->Memory || !self->hooks->Memory->IsAllocatorAvailable())
            return false;

        SDK::EObjectTypeQuery typeData[6] = {
            SDK::EObjectTypeQuery::ObjectTypeQuery1, SDK::EObjectTypeQuery::ObjectTypeQuery2,
            SDK::EObjectTypeQuery::ObjectTypeQuery3, SDK::EObjectTypeQuery::ObjectTypeQuery4,
            SDK::EObjectTypeQuery::ObjectTypeQuery5, SDK::EObjectTypeQuery::ObjectTypeQuery6 };
        const SDK::TArray<SDK::EObjectTypeQuery> types(typeData, 6, 6);
        SDK::TArray<SDK::FHitResult> hits;
        SDK::UKismetSystemLibrary::SphereTraceMultiForObjects(character, start, end, radius, types,
            false, ignore, SDK::EDrawDebugTrace::None, &hits, true, colour, colour, 0.0f);

        const SDK::ECollisionChannel droneType = sphere->GetCollisionObjectType();
        bool found = false;
        for (int32_t i = 0; i < hits.Num() && !found; ++i)
        {
            SDK::UPrimitiveComponent* comp = hits[i].Component.Get();
            if (comp &&
                comp->GetCollisionResponseToChannel(droneType) == SDK::ECollisionResponse::ECR_Block &&
                sphere->GetCollisionResponseToChannel(comp->GetCollisionObjectType()) == SDK::ECollisionResponse::ECR_Block)
            {
                *hit = hits[i];
                found = true;
            }
        }

        if (hits.GetDataPtr())
            self->hooks->Memory->Free(const_cast<SDK::FHitResult*>(hits.GetDataPtr()));
        return found;
    }

    void LogHit(const SDK::FHitResult& hit, SDK::UPrimitiveComponent* comp, SDK::ACrCharacterDroneBase* drone,
                SDK::USphereComponent* sphere, const char* direction, uint64_t now)
    {
        uint64_t& last = g_lastLoggedMs[comp->Index];
        if (last != 0 && now - last < k_throttleMs)
            return;
        last = now;

        SDK::AActor* owner = comp->GetOwner();
        const SDK::ECollisionChannel droneType = sphere->GetCollisionObjectType();
        const SDK::ECollisionChannel hitType = comp->GetCollisionObjectType();
        const SDK::FVector at = sphere->K2_GetComponentLocation();
        const SDK::FVector velocity = drone->GetVelocity();
        LOG_INFO("DroneHitProbe: flying %s (held %.1f s, vertical speed %.1f cm/s), in the way: actor %s (%s), component %s (%s) item %d; "
                 "collision %s, object type %s, profile %s, its response to the drone (%s) %s, drone's response to it %s; "
                 "normal (%.2f %.2f %.2f) at (%.1f %.1f %.1f), distance %.1f%s; drone at (%.1f %.1f %.1f) radius %.1f",
            direction, (now - g_heldSinceMs) / 1000.0, velocity.Z,
            owner ? owner->GetName().c_str() : "none", owner ? owner->Class->GetName().c_str() : "-",
            comp->GetName().c_str(), comp->Class->GetName().c_str(), hit.Item,
            CollisionEnabledName(comp->GetCollisionEnabled()), ChannelName(hitType).c_str(),
            comp->GetCollisionProfileName().ToString().c_str(),
            ChannelName(droneType).c_str(), ResponseName(comp->GetCollisionResponseToChannel(droneType)),
            ResponseName(sphere->GetCollisionResponseToChannel(hitType)),
            hit.ImpactNormal.X, hit.ImpactNormal.Y, hit.ImpactNormal.Z,
            hit.ImpactPoint.X, hit.ImpactPoint.Y, hit.ImpactPoint.Z,
            hit.Distance, hit.bStartPenetrating ? " (started inside it)" : "",
            at.X, at.Y, at.Z, sphere->GetScaledSphereRadius());
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
            LogHit(hit, comp, drone, sphere, up ? "up" : "down", now);
            return;
        }
    }

    // Nothing in the way to a query, yet the drone is not moving: the
    // blocker is a physics-only shape, or part of the drone wider than the
    // sphere.
    const SDK::FVector velocity = drone->GetVelocity();
    if (now - g_heldSinceMs >= k_stuckMs && std::abs(velocity.Z) < k_stuckSpeedCmPerSec &&
        (g_noHitLoggedMs == 0 || now - g_noHitLoggedMs >= k_throttleMs))
    {
        g_noHitLoggedMs = now;
        const SDK::FVector at = sphere->K2_GetComponentLocation();
        LOG_INFO("DroneHitProbe: flying %s, held %.1f s, vertical speed %.1f cm/s, no query-visible blocker "
                 "within %.0f cm; drone at (%.1f %.1f %.1f) radius %.1f",
            up ? "up" : "down", (now - g_heldSinceMs) / 1000.0, velocity.Z, k_sweepCm,
            at.X, at.Y, at.Z, sphere->GetScaledSphereRadius());
    }
}
