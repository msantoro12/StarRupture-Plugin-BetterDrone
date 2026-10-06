#pragma once

// Log-only probe for the drone map marker: with the map open it prints what
// the overlay needs to place a marker over the game's own map widget. It
// draws nothing and writes nothing to the game. Off by default; the
// "bd_mapprobe" console command turns it on for the session.
//
// Game thread only. It reads the map widget, its markers and the replicated
// marker data fresh each tick and keeps no pointers between ticks.

struct IPluginSelf;

void InitMapProbe(IPluginSelf* self);
void ShutdownMapProbe(IPluginSelf* self);

// Call every engine tick. Returns at once while the probe is off.
void TickMapProbe(float deltaSeconds);
