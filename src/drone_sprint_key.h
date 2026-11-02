#pragma once
#include <cstddef>

namespace SDK { class UClass; }

// How a key lookup ended.
enum class InputLookup
{
    Found,
    // Not found, but it may turn up: no world, player or input config yet.
    NotReady,
    // The action is bound, but only to keys this plugin cannot poll (a
    // gamepad, most likely). Looking again will not change that.
    NoKeyboardBinding,
};

// Resolves the boost key's default binding: whatever the game's own Sprint
// action is currently mapped to, so boost never needs its own default key.
//
// Finds the first keyboard or mouse key applied to Sprint in the local
// player's UEnhancedPlayerInput::EnhancedActionMappings, or, when the game
// has not applied it (yet, or no longer), the one its input mapping context
// defaults to, and returns the Windows VK code to poll on the drone tick --
// the same polling path OnDroneTick already uses for a bare-modifier custom
// key, since the loader cannot dispatch keybinds for a key it does not own.
// Returns 0 if resolution fails; outKeyName is left empty in that case.
// outStatus and outWhy, when given, say how it ended and, short of a key,
// which step stopped it.
//
// Game-thread only, and safe to call before a world exists.
int ResolveSprintVk(char* outKeyName, size_t outKeyNameSize,
                    InputLookup* outStatus = nullptr, const char** outWhy = nullptr);

// The same resolution for any native input handler class (a subclass of
// UCrNativeInputHandler, e.g. UCrInputNativeDroneMoveUp): the first keyboard
// or mouse key for that handler's UInputAction, as a VK code. Returns 0 on
// failure. Game-thread only.
int ResolveInputHandlerVk(const SDK::UClass* handlerClass, char* outKeyName, size_t outKeyNameSize,
                          InputLookup* outStatus = nullptr, const char** outWhy = nullptr);
