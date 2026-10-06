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
    // While the drone is out only a clear fall is acted on, so nothing the
    // game itself does to the parked character is fought over.
    constexpr double k_flightDropCm = 100.0;

    // How much farther below (or above) the capsule than at park time a floor
    // may be and still count as the parked floor. Anything lower (terrain
    // under a missing panel) is not the floor the character was left on.
    constexpr float k_floorSlackCm = 5.0f;

    // How far the character may drift sideways, and how fast it may move
    // sideways, while still counting as on its parked spot. A fall through a
    // missing floor goes straight down; anything else is the player or the
    // game moving it.
    constexpr double k_spotSlackCm = 10.0;
    constexpr double k_spotSpeedCmPerSec = 50.0;

    // How far the character may be from where it last stood when the drone
    // launches for that snapshot to still describe it.
    constexpr double k_snapshotSlackCm = 50.0;
    constexpr double k_snapshotHeightSlackCm = 10.0;

    // How long to hold for the floor before giving up and leaving the
    // character to the game. Counted on return, and during the flight only
    // while the drone is within the stock range of the character, where the
    // floor would be loaded if it still existed.
    constexpr float k_holdTimeoutSeconds = 10.0f;

    // How long the floor has to stay under the character after return before
    // the guard stops watching. The world streams back in around the view
    // over a few seconds, and the floor's collision can drop out during that
    // time. Also how long the drone has to stay near the character before
    // its floor counts as loaded again.
    constexpr float k_settleSeconds = 3.0f;

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
        Returning,  // drone back, watching the floor under the character
    };

    Phase  g_phase = Phase::Idle;
    bool   g_wasInDrone = false;

    // Where the character last stood on walkable floor before the drone went
    // out. The game switches the parked character's movement off (MOVE_None)
    // as the drone launches, which also clears its floor, so this is taken
    // on earlier ticks and only refreshed while the character stands.
    bool   g_snapshotValid = false;
    SDK::FVector g_snapshotLocation;
    float  g_snapshotFloorDistCm = 0.0f;

    ObjectRef<SDK::ACrCharacterPlayerBase> g_parked;
    SDK::FVector g_parkedLocation;
    float  g_parkedFloorDistCm = 0.0f;

    // True once the character has been caught falling, until it is set down.
    bool   g_holding = false;
    float  g_holdSeconds = 0.0f;
    float  g_returnSeconds = 0.0f;
    float  g_watchSeconds = 0.0f;    // since return or the last set-down
    float  g_settledSeconds = 0.0f;  // floor continuously under the character
    float  g_droneNearSeconds = 0.0f;
    bool   g_leftStockRange = false;
    double g_maxDroneDistanceCm = 0.0;
    double g_largestDropCm = 0.0;

    float StockRadiusCm() { return g_drone.valid ? g_drone.origMaxRadius : k_stockRadiusCm; }
    float StockHeightCm() { return g_drone.valid ? g_drone.origMaxHeight : k_stockHeightCm; }

    double DistanceTo2D(const SDK::FVector& a, const SDK::FVector& b)
    {
        return SDK::FVector{ a.X, a.Y, b.Z }.GetDistanceTo(b);
    }

    // Whether there is walkable floor under `at` no farther below the capsule
    // than the parked floor was. A small negative distance is the movement
    // component pulling the capsule out of a floor it slightly overlaps.
    bool HasParkedFloor(SDK::UCharacterMovementComponent* move, const SDK::FVector& at)
    {
        SDK::FFindFloorResult floor{};
        move->K2_FindFloor(at, &floor);
        return floor.bBlockingHit && floor.bWalkableFloor &&
               floor.FloorDist >= -k_floorSlackCm &&
               floor.FloorDist <= g_parkedFloorDistCm + k_floorSlackCm;
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

    // Remembers where the character stands on walkable floor. Reads what the
    // movement component already worked out this tick; no trace. Ticks where
    // it is not standing leave the last snapshot in place, so a tick of
    // movement switched off before the drone status arrives does not lose it.
    void NoteStanding(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move)
    {
        const SDK::FFindFloorResult& floor = move->CurrentFloor;
        if (move->MovementMode != SDK::EMovementMode::MOVE_Walking ||
            !floor.bBlockingHit || !floor.bWalkableFloor)
        {
            // Off its floor (jumping, falling, swimming, a ladder): an older
            // snapshot no longer describes it.
            if (move->MovementMode != SDK::EMovementMode::MOVE_None)
                g_snapshotValid = false;
            return;
        }

        g_snapshotValid       = true;
        g_snapshotLocation    = character->K2_GetActorLocation();
        g_snapshotFloorDistCm = floor.FloorDist;
    }

    void Park(SDK::ACrCharacterPlayerBase* character)
    {
        g_phase    = Phase::Idle;
        g_holding  = false;
        g_holdSeconds        = 0.0f;
        g_returnSeconds      = 0.0f;
        g_settledSeconds     = 0.0f;
        g_droneNearSeconds   = 0.0f;
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

        // Only a character that was standing on a floor, right here, is
        // guarded. Swimming, ladders, ziplines and the like keep the stock
        // behaviour.
        const SDK::FVector location = character->K2_GetActorLocation();
        const double heightChange = location.Z - g_snapshotLocation.Z;
        if (!g_snapshotValid ||
            DistanceTo2D(location, g_snapshotLocation) > k_snapshotSlackCm ||
            std::abs(heightChange) > k_snapshotHeightSlackCm)
        {
            LOG_INFO("DroneFloorGuard: not guarding this flight, the character was not standing on a floor (movement mode %d)",
                static_cast<int>(character->CharacterMovement->MovementMode));
            return;
        }

        g_parked.Set(character);
        g_parkedLocation    = location;
        g_parkedFloorDistCm = g_snapshotFloorDistCm + static_cast<float>(heightChange);
        g_phase = Phase::Flying;
        LOG_INFO("DroneFloorGuard: guarding this flight, the character is parked %.1f cm above its floor",
            g_parkedFloorDistCm);
    }

    double DropBelowParked(const SDK::FVector& at)
    {
        const double drop = g_parkedLocation.Z - at.Z;
        if (drop > g_largestDropCm)
            g_largestDropCm = drop;
        return drop;
    }

    // Tracks how far the drone has gone. Returns true while it is within the
    // stock range of the parked spot. Once the drone has stayed that close
    // long enough for the floor to load again, the flight counts as never
    // having left, so a floor missing after that is really gone.
    bool TrackDrone(SDK::ACrCharacterPlayerBase* character, float deltaSeconds)
    {
        SDK::UCameraComponent* camera = character->DroneCamera;
        if (!camera)
            return !g_leftStockRange;

        const SDK::FVector at = camera->K2_GetComponentLocation();
        const double distance = at.GetDistanceTo(g_parkedLocation);
        if (distance > g_maxDroneDistanceCm)
            g_maxDroneDistanceCm = distance;

        const bool inStockRange = DistanceTo2D(at, g_parkedLocation) <= StockRadiusCm() &&
                                  std::abs(at.Z - g_parkedLocation.Z) <= StockHeightCm();
        if (!inStockRange)
        {
            g_leftStockRange = true;
            g_droneNearSeconds = 0.0f;
        }
        else
        {
            g_droneNearSeconds += deltaSeconds;
            if (g_droneNearSeconds > k_settleSeconds)
                g_leftStockRange = false;
        }
        return inStockRange;
    }

    // The game keeps the parked character's movement off while the drone is
    // out, so this only acts if something moves the character during the
    // flight anyway.
    void TickFlying(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move, float deltaSeconds)
    {
        const bool droneNear = TrackDrone(character, deltaSeconds);

        if (!g_holding)
        {
            if (DropBelowParked(character->K2_GetActorLocation()) <= k_flightDropCm)
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

    // Whether the player or the game has moved the character off its parked
    // spot since return: walking, jumping, crouching, sliding, knockback.
    // Its floor is then no longer the guard's business.
    bool MovedOffSpot(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move,
                      const SDK::FVector& at)
    {
        const SDK::FVector& v = move->Velocity;
        const SDK::EMovementMode mode = move->MovementMode;
        return DistanceTo2D(at, g_parkedLocation) > k_spotSlackCm ||
               std::sqrt(v.X * v.X + v.Y * v.Y) > k_spotSpeedCmPerSec ||
               v.Z > 0.0 ||
               character->bIsCrouched ||
               (mode != SDK::EMovementMode::MOVE_None &&
                mode != SDK::EMovementMode::MOVE_Walking &&
                mode != SDK::EMovementMode::MOVE_Falling);
    }

    void TickReturning(SDK::ACrCharacterPlayerBase* character, SDK::UCharacterMovementComponent* move, float deltaSeconds)
    {
        // The drone never left the stock range, or came back and stayed near
        // long enough for the floor to load, so a missing floor is real.
        if (!g_leftStockRange)
        {
            g_phase = Phase::Idle;
            return;
        }

        g_returnSeconds += deltaSeconds;

        if (!g_holding)
        {
            const SDK::FVector at = character->K2_GetActorLocation();
            if (MovedOffSpot(character, move, at))
            {
                g_phase = Phase::Idle;
                return;
            }

            // Only a character dropping straight down out of its spot is
            // falling through a missing floor.
            DropBelowParked(at);
            if (move->MovementMode != SDK::EMovementMode::MOVE_Falling)
            {
                // Still standing where it was left. Watch until the floor has
                // stayed under it long enough for the world to finish
                // streaming back in.
                if (move->MovementMode == SDK::EMovementMode::MOVE_Walking && HasParkedFloor(move, at))
                    g_settledSeconds += deltaSeconds;
                else
                    g_settledSeconds = 0.0f;

                g_watchSeconds += deltaSeconds;
                if (g_settledSeconds > k_settleSeconds || g_watchSeconds > k_holdTimeoutSeconds)
                    g_phase = Phase::Idle;
                return;
            }

            g_holdSeconds = 0.0f;
        }
        // The player is back in control: moving ends the hold.
        else if (g_returnSeconds > k_returnInputGraceSeconds &&
                 !character->GetLastMovementInputVector().IsZero())
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
            g_settledSeconds = 0.0f;
            g_watchSeconds = 0.0f;
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
            NoteStanding(character, move);
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
        g_settledSeconds = 0.0f;
        g_watchSeconds = 0.0f;
        TickReturning(character, move, deltaSeconds);
        break;

    case Phase::Returning:
        // Back into the drone before the floor came back: keep the same
        // parked spot and keep holding while it is out.
        if (inDrone)
        {
            g_phase = Phase::Flying;
            g_holdSeconds = 0.0f;
            g_droneNearSeconds = 0.0f;
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
    // The standing snapshot is kept: Park only uses it while the character
    // is still where it was taken.
    g_phase      = Phase::Idle;
    g_wasInDrone = false;
    g_holding    = false;
    g_parked.Reset();
}
