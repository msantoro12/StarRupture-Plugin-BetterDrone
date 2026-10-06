#pragma once

// Keeps the player's character on its floor across a long drone flight.
//
// The character stays the possessed pawn while the drone is out, parked where
// the player left it, but the view goes with the drone. Buildings are Mass
// entities whose collision comes from an actor that is spawned only while the
// viewer is near them, and the world streams around the view as well. Fly far
// enough (well past the stock drone radius) and the floor under the parked
// character is unloaded. On return the character's movement ticks before the
// floor is back, and it drops through.
//
// The guard records where the character stood when the drone went out. The
// game keeps the parked character's movement off during the flight, so the
// drop comes on return. If, after return, the character starts falling
// straight down out of its spot before the floor has stayed under it for a
// few seconds, it is held at that spot until the floor is there again, then
// set down walking. Nothing is touched when the floor is fine, and anything
// that moves the character off its spot (walking, jumping, crouching,
// knockback) ends the watch.
//
// A floor that is really gone (deconstructed, destroyed, moved) must not be
// held for: the guard does not arm at the stock drone range, does not hold
// once the drone has stayed within stock range long enough for the floor to
// load, and lets go when the player moves, or after a bounded wait.
//
// The hold re-places the character every tick rather than switching its
// movement off, so if the plugin is unloaded mid-hold nothing is left frozen.

// Game thread only. Call once per engine tick.
void TickDroneFloorGuard(float deltaSeconds);

// Forgets the parked character. Call when the world ends.
void ResetDroneFloorGuard();
