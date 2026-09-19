#pragma once
#include <cstddef>

// Resolves the boost key's default binding: whatever the game's own Sprint
// action is currently mapped to, so boost never needs its own default key.
//
// Finds the first keyboard or mouse key QueryKeysMappedToAction reports for
// Sprint and returns the Windows VK code to poll on the drone tick -- the
// same polling path OnDroneTick already uses for a bare-modifier custom
// key, since the loader cannot dispatch keybinds for a key it does not own.
// Returns 0 if resolution fails (no world/subsystem/action, or only a
// gamepad key is mapped); outKeyName is left empty in that case.
//
// Game-thread only, and safe to call before a world exists.
int ResolveSprintVk(char* outKeyName, size_t outKeyNameSize);
