#pragma once
#include "drone_heat_ring.h"

// The mining laser's heat ring around the crosshair (see drone_heat_ring.h),
// on a transparent overlay widget of the loader's. The laser's tick publishes
// what to show; the render callback only draws it.

struct IPluginSelf;

void InitDroneHeatDisplay(IPluginSelf* self);

// Touches no game object, so it may run on any thread.
void ShutdownDroneHeatDisplay(IPluginSelf* self);

// Game thread. Shows the ring as given, or hides it when view is null.
void PublishDroneHeat(const DroneHeatRing::View* view);
