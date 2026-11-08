#pragma once
#include <cstddef>

namespace SDK { class UClass; }

// How a key lookup ended.
enum class InputLookup
{
    // A key was found among the keys the player's input has applied.
    Found,
    // A key was found only as the default of an input mapping context: the
    // game has not applied the action's context (yet, or at the moment), so
    // a key the player rebound would not show.
    FoundDefault,
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
// Game thread only, and safe to call before a world exists.
int ResolveSprintVk(char* outKeyName, size_t outKeyNameSize,
                    InputLookup* outStatus = nullptr, const char** outWhy = nullptr);

// The same resolution for any native input handler class (a subclass of
// UCrNativeInputHandler, e.g. UCrInputNativeDroneMoveUp): the first keyboard
// or mouse key for that handler's UInputAction, as a VK code. Returns 0 on
// failure. Game thread only.
int ResolveInputHandlerVk(const SDK::UClass* handlerClass, char* outKeyName, size_t outKeyNameSize,
                          InputLookup* outStatus = nullptr, const char** outWhy = nullptr);

// How long the game's input mapping makes the player hold the key of a native
// input handler class (a Hold trigger's HoldTimeThreshold, on the key mapping
// or on the action), in seconds. False when the class has no action loaded or
// no Hold trigger with a positive time; *outSeconds is then untouched. Game
// thread only.
bool ResolveInputHandlerHoldSeconds(const SDK::UClass* handlerClass, float* outSeconds);

// One key that follows a game input action (Sprint for the boost key, Reload
// for the laser's vent): looked up through ResolveInputHandlerVk, kept as a
// VK for the tick to poll, and looked up again when the game may have changed
// it. Game thread only: a lookup reads the world.
struct FollowedKey
{
    // What a lookup changed, for the caller to log.
    struct Result
    {
        bool        followed      = false;   // vk is now the game's key
        bool        fellBack      = false;   // vk is now the stand-in
        bool        firstFallback = false;   // ... for the first time
        const char* why           = nullptr; // the step that stopped the lookup
    };

    int                vk             = 0;
    // The key is not settled yet: the lookup is repeated.
    bool               pending        = true;
    // vk came from the player's applied mappings, so a context default found
    // later must not replace it.
    bool               fromApplied    = false;
    bool               fallbackLogged = false;
    int                tries          = 0;
    unsigned long long triedMs        = 0;

    // Starts over: no key, pending, the next lookup due at once.
    void Restart();

    // Pending, and the retry time has passed: quick at first, for the load,
    // then rare, since a miss that lasts is not going to clear by being asked
    // every two seconds.
    bool Due() const;

    // Looks the action's key up now. A miss before the world and its input
    // exist is not an answer; fallbackVk stands in, once, only when the action
    // has no keyboard binding or the lookup still misses with the player in
    // the drone (everything it needs exists by then). A later success
    // replaces it. keyName receives the key's name when one is found.
    Result Resolve(const SDK::UClass* handlerClass, bool inDrone, int fallbackVk,
                   char* keyName, size_t keyNameSize);
};
