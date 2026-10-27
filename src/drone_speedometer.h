#pragma once

// The drone's speed, in the panel's Speed Display Unit, just right of the
// crosshair and centred on it, outside the mining laser's heat ring. Drawn
// on a transparent overlay widget of the loader's, like the ring. Shown while
// the drone is out and the panel's Speedometer setting is on.

struct IPluginSelf;

void InitDroneSpeedometer(IPluginSelf* self);

// Touches no game object, so it may run on any thread.
void ShutdownDroneSpeedometer(IPluginSelf* self);

// Game thread only. Call once per engine tick. Holds no UObject pointer
// across ticks.
void TickDroneSpeedometer();
