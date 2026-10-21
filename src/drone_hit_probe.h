#pragma once
#include <Engine_classes.hpp>

// Temporary diagnostic: what stops the drone flying up through a ladder hole.
//
// Once per drone launch, logs the drone's collision body: the root's and
// the drone sphere's profile and object type, and every primitive component
// on the drone actor with its collision state and shape size. While the
// drone's Up or Down key is held, sweeps the drone sphere a short way in
// that direction as a query (nothing is moved) and logs what it would hit.
// Each blocking component is logged at most once every 2 s. All lines start
// with "DroneHitProbe:".

// Game thread only. Call once per engine tick. Holds no UObject pointer
// across ticks; its state starts over on each drone launch.
void TickDroneHitProbe();


// The engine's name for a collision-enabled state, for log lines.
const char* CollisionEnabledName(SDK::ECollisionEnabled enabled);
