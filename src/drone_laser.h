#pragma once

// Mining laser for the building drone. While the laser key is held in drone
// mode, a line trace from the drone camera picks the ore under the crosshair
// and hands it to the mining component's own native requests, with the
// mining tool's damage, hit interval and range. The yield goes to the
// player's inventory the way stock mining's does.
//
// What it mines: actor ore (meteor ores and their chunks) and infinite ore
// veins. Mass ore and crops are left alone. It never places, deconstructs or highlights
// anything: the key is a loader keybind, not a game input action, and the
// trace is a plain line trace that does not go through the building tool.
//
// Host and single player only. On a client the mining requests do nothing,
// so the key says so once and stays off.
//
// Mining stops, through the same call the stock game makes when the mining
// beam is let go, on every way out: the key released, the target changed,
// lost or out of range, the drone left or recalled, the character dead, the
// world gone, the drill or the laser overheated, the bag unable to take the
// next grant, and plugin shutdown. A full bag or an overheated drill keeps
// the laser off until the key is pressed again; an overheated laser keeps it
// off until it has cooled.
//
// It looks, sounds and heats like the mining tool: the tool's beam and sound
// cues (drone_laser_fx.h), and a heat count of the laser's own with the
// tool's numbers, shown as a ring around the crosshair (drone_heat_ring.h).
// The drill's own heat is read, not changed.
//
// Game thread only: the key callback only stores an atomic, and every
// UObject is reached from the tick. Nothing is kept across ticks but
// ObjectRefs.

struct IPluginSelf;
struct IPluginHookScanner;

// Resolve the AOB addresses. Callable only from OnPluginLoadHooks.
void ResolveDroneLaser(IPluginSelf* self, IPluginHookScanner* scanner);

void InitDroneLaser(IPluginSelf* self);

// Stops any mining, beam and sound in progress (game thread only; elsewhere
// it logs and leaves them to the game) and unregisters the key and the ring.
void ShutdownDroneLaser(IPluginSelf* self);

// Re-registers the key after the loader's settings page changed it.
void RebindDroneLaserKey(IPluginSelf* self, const char* newKeyName);

// Call every engine tick.
void TickDroneLaser(float deltaSeconds);
