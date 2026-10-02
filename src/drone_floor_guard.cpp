#include "drone_floor_guard.h"
#include "drone_interact.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <cmath>
#include <cstdint>
#include <windows.h>

namespace
{
    // How far below its parked spot the character may be on return before it
    // counts as having dropped. Above the gap the movement component keeps
    // between capsule and floor, well below a fall through a floor panel.
    constexpr double k_returnDropCm = 20.0;

    // While the drone is out only a clear fall is acted on, so nothing the
    // game itself does to the parked character is fought over.
    constexpr double k_flightDropCm = 100.0;

    // On return, how long to wait for the floor before giving up and leaving
    // the character to the game.
    constexpr uint64_t k_returnTimeoutMs = 10000;

    enum class Phase
    {
        Idle,       // drone not out, or the character was not standing when it went out
        Flying,     // drone out, character parked
        Returning,  // drone back, waiting for the floor under the character
    };

    Phase  g_phase = Phase::Idle;
    bool   g_wasInDrone = false;
    ObjectRef<SDK::ACrCharacterPlayerBase> g_parked;
    SDK::FVector g_parkedLocation;

    // True once the character has been caught falling, until it is set down.
    bool     g_holding = false;
    uint64_t g_returnStartMs = 0;
    double   g_maxDroneDistanceCm = 0.0;
    double   g_largestDropCm = 0.0;

    bool HasWalkableFloor(SDK::UCharacterMovementComponent* move, const SDK::FVector& at)
    {
        SDK::FFindFloorResult floor{};
        move->K2_FindFloor(at, &floor);
        return floor.bBlockingHit && floor.bWalkableFloor;
    }

    void PlaceAtParkedSpot(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move)
    {
        character->K2_SetActorLocation(g_parkedLocation, false, nullptr, true);
        move->Velocity = SDK::FVector();
    }

    // Holds the character at its parked spot until there is floor there, then
    // sets it down walking. Returns true once it is set down.
    bool HoldUntilFloor(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move)
    {
        g_holding = true;
        PlaceAtParkedSpot(character, move);

        if (!HasWalkableFloor(move, g_parkedLocation))
            return false;

        if (move->MovementMode != SDK::EMovementMode::MOVE_Walking)
            move->SetMovementMode(SDK::EMovementMode::MOVE_Walking, 0);

        g_holding = false;
        return true;
    }

    void Park(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move)
    {
        g_phase    = Phase::Idle;
        g_holding  = false;
        g_maxDroneDistanceCm = 0.0;
        g_largestDropCm      = 0.0;

        // Only a character standing on a floor is guarded. Swimming, ladders,
        // ziplines and the like keep the stock behaviour.
        if (move->MovementMode != SDK::EMovementMode::MOVE_Walking &&
            move->MovementMode != SDK::EMovementMode::MOVE_NavWalking)
            return;

        const SDK::FVector location = character->K2_GetActorLocation();
        if (!HasWalkableFloor(move, location))
            return;

        g_parked.Set(character);
        g_parkedLocation = location;
        g_phase = Phase::Flying;
    }

    double DropBelowParked(SDK::ACrCharacterPlayerBase* character)
    {
        const double drop = g_parkedLocation.Z - character->K2_GetActorLocation().Z;
        if (drop > g_largestDropCm)
            g_largestDropCm = drop;
        return drop;
    }

    void TrackDroneDistance(SDK::ACrCharacterPlayerBase* character)
    {
        SDK::UCameraComponent* camera = character->DroneCamera;
        if (!camera)
            return;

        const SDK::FVector at = camera->K2_GetComponentLocation();
        const double dx = at.X - g_parkedLocation.X;
        const double dy = at.Y - g_parkedLocation.Y;
        const double dz = at.Z - g_parkedLocation.Z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > g_maxDroneDistanceCm)
            g_maxDroneDistanceCm = distance;
    }

    void TickFlying(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move)
    {
        TrackDroneDistance(character);

        if (!g_holding && DropBelowParked(character) <= k_flightDropCm)
            return;

        if (!g_holding)
        {
            LOG_INFO("DroneFloorGuard: the character fell %.0f cm with the drone out (up to %.0f m away); holding it at its parked spot",
                g_largestDropCm, g_maxDroneDistanceCm / 100.0);
        }

        HoldUntilFloor(character, move);
    }

    void TickReturning(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move)
    {
        const uint64_t now = GetTickCount64();

        // The usual case: still standing where it was left. Nothing to do.
        if (!g_holding &&
            DropBelowParked(character) < k_returnDropCm &&
            HasWalkableFloor(move, character->K2_GetActorLocation()))
        {
            g_phase = Phase::Idle;
            return;
        }

        if (HoldUntilFloor(character, move))
        {
            LOG_INFO("DroneFloorGuard: drone back from %.0f m with the character off its floor (dropped %.0f cm); "
                     "held it %.1f s, then set it back down",
                g_maxDroneDistanceCm / 100.0, g_largestDropCm,
                static_cast<double>(now - g_returnStartMs) / 1000.0);
            g_phase = Phase::Idle;
            return;
        }

        if (now - g_returnStartMs > k_returnTimeoutMs)
        {
            LOG_WARN("DroneFloorGuard: drone back from %.0f m and still no floor under the character after %.0f s; released it",
                g_maxDroneDistanceCm / 100.0, static_cast<double>(k_returnTimeoutMs) / 1000.0);
            g_holding = false;
            g_phase = Phase::Idle;
        }
    }
}

void TickDroneFloorGuard()
{
    SDK::ACrCharacterPlayerBase* character = LocalPlayerCharacter();
    SDK::UCharacterMovementComponent* move = character ? character->CharacterMovement : nullptr;
    if (!move || character->bDead)
    {
        ResetDroneFloorGuard();
        return;
    }

    const bool inDrone = character->Status == SDK::EPlayerCharacterStatus::BuildingDrone;

    // A respawned or reloaded character is a different object; whatever was
    // parked no longer applies.
    if (g_phase != Phase::Idle && g_parked.Get() != character)
        ResetDroneFloorGuard();

    switch (g_phase)
    {
    case Phase::Idle:
        if (inDrone && !g_wasInDrone)
            Park(character, move);
        break;

    case Phase::Flying:
        if (inDrone)
        {
            TickFlying(character, move);
            break;
        }
        g_phase = Phase::Returning;
        g_returnStartMs = GetTickCount64();
        TickReturning(character, move);
        break;

    case Phase::Returning:
        // Back into the drone before the floor came back: keep the same
        // parked spot and keep holding while it is out.
        if (inDrone)
        {
            g_phase = Phase::Flying;
            TickFlying(character, move);
            break;
        }
        TickReturning(character, move);
        break;
    }

    g_wasInDrone = inDrone;
}

void ResetDroneFloorGuard()
{
    g_phase      = Phase::Idle;
    g_wasInDrone = false;
    g_holding    = false;
    g_parked.Reset();
}
