#include "drone_floor_guard.h"
#include "drone_interact.h"
#include "drone_settings.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <cmath>

namespace
{
    // How far below its parked spot the character may be on return before it
    // counts as having dropped. Above the gap the movement component keeps
    // between capsule and floor, well below a fall through a floor panel.
    constexpr double k_returnDropCm = 20.0;

    // While the drone is out only a clear fall is acted on, so nothing the
    // game itself does to the parked character is fought over.
    constexpr double k_flightDropCm = 100.0;

    // How much farther below the capsule than at park time a floor may be and
    // still count as the parked floor. Anything lower (terrain under a missing
    // panel) is not the floor the character was left on.
    constexpr float k_floorSlackCm = 5.0f;

    // How long to hold for the floor before giving up and leaving the
    // character to the game. Counted on return, and during the flight only
    // while the drone is within the stock range of the character, where the
    // floor would be loaded if it still existed.
    constexpr float k_holdTimeoutSeconds = 10.0f;

    // How long after return the character is still watched while it stands on
    // its floor. The world streams back in around the view over a few
    // seconds, and the floor's collision can drop out during that time.
    constexpr float k_returnSettleSeconds = 3.0f;

    // Input this soon after return is left over from flying the drone, not
    // the player taking the character back.
    constexpr float k_returnInputGraceSeconds = 0.5f;

    // The stock drone range, used when the game's own values were not read.
    constexpr float k_stockRadiusCm = 5000.0f;
    constexpr float k_stockHeightCm = 2000.0f;

    enum class Phase
    {
        Idle,       // drone not out, or the guard did not arm for this flight
        Flying,     // drone out, character parked
        Returning,  // drone back, waiting for the floor under the character
    };

    Phase  g_phase = Phase::Idle;
    bool   g_wasInDrone = false;

    // The character's state on the last tick before the drone went out. The
    // game switches the parked character's movement off (MOVE_None) as the
    // drone launches, which also clears its floor, so both are read the tick
    // before.
    bool   g_wasStanding = false;
    SDK::EMovementMode g_lastMode = SDK::EMovementMode::MOVE_None;
    float  g_lastFloorDistCm = 0.0f;

    ObjectRef<SDK::ACrCharacterPlayerBase> g_parked;
    SDK::FVector g_parkedLocation;
    float  g_parkedFloorDistCm = 0.0f;

    // True once the character has been caught falling, until it is set down.
    bool   g_holding = false;
    float  g_holdSeconds = 0.0f;
    float  g_returnSeconds = 0.0f;
    bool   g_leftStockRange = false;
    double g_maxDroneDistanceCm = 0.0;
    double g_largestDropCm = 0.0;

    float StockRadiusCm() { return g_drone.valid ? g_drone.origMaxRadius : k_stockRadiusCm; }
    float StockHeightCm() { return g_drone.valid ? g_drone.origMaxHeight : k_stockHeightCm; }

    // Distance from the capsule down to walkable floor at `at`, or a negative
    // value when there is none within the movement component's reach.
    float WalkableFloorDistCm(SDK::UCharacterMovementComponent* move, const SDK::FVector& at)
    {
        SDK::FFindFloorResult floor{};
        move->K2_FindFloor(at, &floor);
        return floor.bBlockingHit && floor.bWalkableFloor ? floor.FloorDist : -1.0f;
    }

    bool HasParkedFloor(SDK::UCharacterMovementComponent* move, const SDK::FVector& at)
    {
        const float dist = WalkableFloorDistCm(move, at);
        return dist >= 0.0f && dist <= g_parkedFloorDistCm + k_floorSlackCm;
    }

    // Holds the character at its parked spot until its floor is there, then
    // sets it down walking. Returns true once it is set down.
    bool HoldUntilFloor(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move)
    {
        g_holding = true;
        character->K2_SetActorLocation(g_parkedLocation, false, nullptr, true);
        move->Velocity = SDK::FVector();

        if (!HasParkedFloor(move, g_parkedLocation))
            return false;

        // Only undo a fall. While the drone is out the game keeps movement
        // off, and that is left alone.
        if (move->MovementMode == SDK::EMovementMode::MOVE_Falling)
            move->SetMovementMode(SDK::EMovementMode::MOVE_Walking, 0);

        g_holding = false;
        return true;
    }

    void Release()
    {
        g_holding = false;
        g_phase = Phase::Idle;
    }

    // Remembers whether the character is standing on walkable floor. Reads
    // what the movement component already worked out this tick; no trace.
    void NoteStanding(SDK::UCharacterMovementComponent* move)
    {
        const SDK::FFindFloorResult& floor = move->CurrentFloor;
        g_lastMode = move->MovementMode;
        g_wasStanding = (g_lastMode == SDK::EMovementMode::MOVE_Walking ||
                         g_lastMode == SDK::EMovementMode::MOVE_NavWalking) &&
                        floor.bBlockingHit && floor.bWalkableFloor;
        g_lastFloorDistCm = floor.FloorDist;
    }

    void Park(SDK::ACrCharacterPlayerBase* character)
    {
        g_phase    = Phase::Idle;
        g_holding  = false;
        g_holdSeconds        = 0.0f;
        g_returnSeconds      = 0.0f;
        g_leftStockRange     = false;
        g_maxDroneDistanceCm = 0.0;
        g_largestDropCm      = 0.0;

        // Within the stock range the floor never unloads, so any fall is real
        // and stays the game's.
        if (!g_drone.valid ||
            (*g_drone.maxRadius <= g_drone.origMaxRadius && *g_drone.maxHeight <= g_drone.origMaxHeight))
        {
            LOG_INFO("DroneFloorGuard: not guarding this flight, the drone range is stock");
            return;
        }

        // Only a character that was standing on a floor is guarded. Swimming,
        // ladders, ziplines and the like keep the stock behaviour.
        if (!g_wasStanding)
        {
            LOG_INFO("DroneFloorGuard: not guarding this flight, the character was not standing on a floor (movement mode %d)",
                static_cast<int>(g_lastMode));
            return;
        }

        g_parked.Set(character);
        g_parkedLocation    = character->K2_GetActorLocation();
        g_parkedFloorDistCm = g_lastFloorDistCm;
        g_phase = Phase::Flying;
        LOG_INFO("DroneFloorGuard: guarding this flight, the character is parked %.1f cm above its floor",
            g_parkedFloorDistCm);
    }

    double DropBelowParked(SDK::ACrCharacterPlayerBase* character)
    {
        const double drop = g_parkedLocation.Z - character->K2_GetActorLocation().Z;
        if (drop > g_largestDropCm)
            g_largestDropCm = drop;
        return drop;
    }

    // Tracks how far the drone has gone. Returns true while it is within the
    // stock range of the parked spot.
    bool TrackDrone(SDK::ACrCharacterPlayerBase* character)
    {
        SDK::UCameraComponent* camera = character->DroneCamera;
        if (!camera)
            return !g_leftStockRange;

        const SDK::FVector at = camera->K2_GetComponentLocation();
        const double distance = at.GetDistanceTo(g_parkedLocation);
        if (distance > g_maxDroneDistanceCm)
            g_maxDroneDistanceCm = distance;

        const SDK::FVector level{ at.X, at.Y, g_parkedLocation.Z };
        const bool inStockRange = level.GetDistanceTo(g_parkedLocation) <= StockRadiusCm() &&
                                  std::abs(at.Z - g_parkedLocation.Z) <= StockHeightCm();
        if (!inStockRange)
            g_leftStockRange = true;
        return inStockRange;
    }

    void TickFlying(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move, float deltaSeconds)
    {
        const bool droneNear = TrackDrone(character);

        if (!g_holding)
        {
            if (DropBelowParked(character) <= k_flightDropCm)
                return;

            // With the drone this close the floor is loaded, so the floor is
            // really gone (deconstructed, destroyed, moved). Leave the fall to
            // the game for the rest of this flight.
            if (droneNear)
            {
                LOG_INFO("DroneFloorGuard: the character fell %.0f cm with the drone within stock range; leaving it to the game",
                    g_largestDropCm);
                Release();
                return;
            }

            LOG_INFO("DroneFloorGuard: the character fell %.0f cm with the drone out (up to %.0f m away); holding it at its parked spot",
                g_largestDropCm, g_maxDroneDistanceCm / 100.0);
            g_holdSeconds = 0.0f;
        }

        if (HoldUntilFloor(character, move))
            return;

        if (droneNear)
            g_holdSeconds += deltaSeconds;
        if (g_holdSeconds > k_holdTimeoutSeconds)
        {
            LOG_WARN("DroneFloorGuard: no floor under the character after %.0f s with the drone nearby; released it",
                k_holdTimeoutSeconds);
            Release();
        }
    }

    void TickReturning(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move, float deltaSeconds)
    {
        // The drone never left the stock range, so the floor never unloaded.
        if (!g_leftStockRange)
        {
            g_phase = Phase::Idle;
            return;
        }

        g_returnSeconds += deltaSeconds;
        const bool playerMoved = g_returnSeconds > k_returnInputGraceSeconds &&
            (!character->GetLastMovementInputVector().IsZero() || character->bPressedJump);

        if (!g_holding)
        {
            // The usual case: still standing where it was left. Keep watching
            // until the world around it has streamed back in, or the player
            // walks off.
            if (DropBelowParked(character) < k_returnDropCm &&
                HasParkedFloor(move, character->K2_GetActorLocation()))
            {
                if (playerMoved || g_returnSeconds > k_returnSettleSeconds)
                    g_phase = Phase::Idle;
                return;
            }
        }
        // The player is back in control: moving or jumping ends the hold.
        else if (playerMoved)
        {
            LOG_INFO("DroneFloorGuard: player moved %.1f s into the return hold; released the character",
                g_holdSeconds);
            Release();
            return;
        }

        if (HoldUntilFloor(character, move))
        {
            LOG_INFO("DroneFloorGuard: drone back from %.0f m with the character off its floor (dropped %.0f cm); "
                     "held it %.1f s, then set it back down",
                g_maxDroneDistanceCm / 100.0, g_largestDropCm, g_holdSeconds);
            // Stays in Returning, so the floor is still watched while the
            // world finishes streaming back in.
            return;
        }

        g_holdSeconds += deltaSeconds;
        if (g_holdSeconds > k_holdTimeoutSeconds)
        {
            LOG_WARN("DroneFloorGuard: drone back from %.0f m and still no floor under the character after %.0f s; released it",
                g_maxDroneDistanceCm / 100.0, k_holdTimeoutSeconds);
            Release();
        }
    }
}

void TickDroneFloorGuard(float deltaSeconds)
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
            Park(character);
        else if (!inDrone)
            NoteStanding(move);
        break;

    case Phase::Flying:
        if (inDrone)
        {
            TickFlying(character, move, deltaSeconds);
            break;
        }
        g_phase = Phase::Returning;
        g_holdSeconds = 0.0f;
        g_returnSeconds = 0.0f;
        TickReturning(character, move, deltaSeconds);
        break;

    case Phase::Returning:
        // Back into the drone before the floor came back: keep the same
        // parked spot and keep holding while it is out.
        if (inDrone)
        {
            g_phase = Phase::Flying;
            g_holdSeconds = 0.0f;
            TickFlying(character, move, deltaSeconds);
            break;
        }
        TickReturning(character, move, deltaSeconds);
        break;
    }

    g_wasInDrone = inDrone;
}

void ResetDroneFloorGuard()
{
    g_phase      = Phase::Idle;
    g_wasInDrone  = false;
    g_wasStanding = false;
    g_holding     = false;
    g_parked.Reset();
}
