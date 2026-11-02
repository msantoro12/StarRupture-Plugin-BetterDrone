#pragma once

// Keeps a small area around the player's parked body loaded while the drone
// is out.
//
// The world streams around the view, and in drone mode the view is the drone
// camera. Fly far enough and the cells around the parked character unload, so
// the floor and the base are gone when the player returns and stream back in
// only after the view snaps back.
//
// While the drone is out, the character carries a streaming source of the
// game's own kind (the one the megamachine teleporters use at their
// destination) with one small round shape, so the cells within that radius of
// the body stay loaded and active. It is added when the drone goes out and
// removed when the drone comes back, when the setting is turned off, when the
// character or the world goes away, and on plugin shutdown.
//
// Cost: memory and actor ticks for the cells inside the radius, about what
// standing there costs. No GPU time, since those cells are beyond the drone's
// view.

// Game thread only. Call once per engine tick.
void TickDroneBodyAnchor();

// "Keep Area Around Body Loaded", as the loader reports it. Any thread.
void SetDroneBodyAnchorEnabled(bool enabled);

// "Body Area Radius", in metres. Clamped. Any thread.
void SetDroneBodyAnchorRadiusMeters(float meters);

// Removes the anchor if there is one. Game thread; call when the world ends.
void ResetDroneBodyAnchor();

// Removes the anchor if called on the game thread. Anywhere else (process
// exit, older loaders' render thread) the engine is left to drop it with the
// character, since a game object is only touched from the game thread.
void ShutdownDroneBodyAnchor();
