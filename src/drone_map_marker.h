#pragma once

// A marker for the building drone on the map, drawn over the game's own map
// widget while the player is in drone mode with the map open. The game draws
// no marker for the drone: its own player marker stays at the body, which is
// parked where the drone was deployed.
//
// The tick (game thread) places the drone on the map from the terrain
// widget's geometry, checks that placement against a building marker the game
// has put on the same map, and publishes plain numbers. The render callback
// only draws from those numbers.

struct IPluginSelf;

void InitDroneMapMarker(IPluginSelf* self);
void ShutdownDroneMapMarker(IPluginSelf* self);

// Game thread only, every engine tick.
void TickDroneMapMarker(float deltaSeconds);

// The colour of the local player's arrow on the map, as an ImGui colour, or
// the arrow's usual cyan while the game's colour asset is not loaded. Game
// thread only.
unsigned int PlayerMarkerColour();
