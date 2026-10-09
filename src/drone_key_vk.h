#pragma once
#include <cstddef>

// Maps a UE key name -- as the loader spells a keybind combo's base key, or
// as FKey::KeyName reports an applied Enhanced Input mapping -- to a Windows
// VK code. Covers letters, digits, function keys, the six modifiers, a few
// named keyboard keys, and the five mouse buttons: the realistic set both
// call sites need. Returns 0 for anything else (a gamepad key, most likely)
// or a null/empty name.
int KeyNameToVk(const char* name);

// The text after the last '+' in a combo string ("Shift+K" -> "K"),
// or the whole string when there's no modifier prefix.
void ExtractBaseKey(const char* combo, char* outBuf, size_t outSize);

// GetAsyncKeyState is global, so a key polled with it only counts while the
// game's own window has focus, not when it is held in another window.
bool GameHasFocus();
