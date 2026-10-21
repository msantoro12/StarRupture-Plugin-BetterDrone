#pragma once

// The drone's speed on the game's own drone HUD, just under its distance
// readout, in the readout's font, colour and shadow and in the panel's Speed
// Display Unit. Shown while the drone is out and the panel's Speedometer
// setting is on.
//
// The readout is a UMG text block BetterDrone adds to the canvas panel that
// holds the HUD's distance digits. It is placed from the digits' laid-out
// geometry, so it follows the HUD however the game sizes it.

// Game thread only. Call once per engine tick. Holds no UObject pointer
// across ticks.
void TickDroneSpeedometer();

// Takes the readout out of the HUD. Plugin shutdown can run off the game
// thread (older loaders, process exit); there the HUD is left alone.
void ShutdownDroneSpeedometer();
