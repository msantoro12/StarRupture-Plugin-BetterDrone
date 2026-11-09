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
//
// Following is a part of its own: while the map is open in drone mode it is
// kept centred on the drone until the player pans it by hand. The Map Centre
// Key (the game's Center on player key) switches it off, so the map stays on
// the player's body, and on again.

struct IPluginSelf;
struct IPluginHookScanner;

void InitDroneMapMarker(IPluginSelf* self);
void ShutdownDroneMapMarker(IPluginSelf* self);

// Game thread only, every engine tick.
void TickDroneMapMarker(float deltaSeconds);

// Resolves the game's CenterOnLocation for the follow. Call in OnPluginLoadHooks.
void ResolveDroneMapFollow(IPluginSelf* self, IPluginHookScanner* scanner);

// Registers the Map Centre Key. Call from PluginInit.
void InitDroneMapFollow(IPluginSelf* self);

// Game thread only, every engine tick.
void TickDroneMapFollow();

// Puts the map back where the game had it, if it can be touched from this
// thread, and unregisters the Map Centre Key. Call from PluginShutdown.
void ShutdownDroneMapFollow(IPluginSelf* self);

// The colour of the local player's arrow on the map, as an ImGui colour, or
// the arrow's usual cyan while the game's colour asset is not loaded. Game
// thread only.
unsigned int PlayerMarkerColour();
