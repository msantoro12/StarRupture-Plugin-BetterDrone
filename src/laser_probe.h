#pragma once

// Log-only probe for the drone mining laser. It settles what the game does
// when the player mines on foot, and what a camera trace from the drone sees,
// so the laser itself can be written against facts rather than inference.
// It changes nothing in the game: the detours it installs log their arguments
// and hand the call straight on, and everything else is a read. Off by
// default; the "bd_laserprobe" console command turns it on for the session,
// and it switches itself off again after a fixed time and after fixed line
// counts, so a forgotten "on" cannot grow the log for a whole session.
//
// What it logs, all under the [LaserProbe] tag:
//   * the native mining entry points as the stock mining ability calls them
//     (actor, instanced-mesh and Mass ore requests, the Mass grantee, and the
//     two stop calls), with their arguments and the time between calls;
//   * the character's mining state, equipped weapon data and heat stacks;
//   * what a camera line trace hits, on every trace channel, on foot while
//     mining and always from the drone.
//
// Game thread only. It keeps no UObject pointers between ticks, only the
// object indices it compares to notice a change.

struct IPluginSelf;
struct IPluginHookScanner;

// Resolve the AOB addresses. Callable only from OnPluginLoadHooks.
void ResolveLaserProbe(IPluginSelf* self, IPluginHookScanner* scanner);

void InitLaserProbe(IPluginSelf* self);
void ShutdownLaserProbe(IPluginSelf* self);

// Call every engine tick. Returns at once while the probe is off.
void TickLaserProbe(float deltaSeconds);
