#include "drone_hotbar.h"
#include "drone_fields.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <cstdint>

namespace
{
    // ACrCharacterPlayerBase::NativeOnCycleWeapon(character, direction): the
    // call the game's next-weapon and previous-weapon inputs make, 0 for next
    // and 1 for previous. Not reflected. Called by this plugin, never hooked.
    // Null means the key is missing on this build, which turns off only the
    // hotbar (DroneFields::Features::HotbarCycle).
    using CycleWeapon_t = void (*)(SDK::ACrCharacterPlayerBase*, uint8_t);

    CycleWeapon_t g_cycleWeapon = nullptr;

    // The start of the next-weapon input handler's InputPressed: it checks the
    // pawn is a player character, then calls the cycle with direction 0
    // (xor edx, edx). The byte at kCycleWeaponCallAt is that call's E8; the
    // loader follows it to the cycle function. The call's target and the
    // handler's first call are wildcarded, so the same bytes match on every
    // game build, where the cycle function itself starts differently.
    constexpr const char* kCycleWeaponPattern =
        "4D 85 C0 74 ?? 53 48 83 EC 20 49 8B D8 E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 30 "
        "48 63 40 38 3B 42 38 7F ?? 48 8B C8 48 8B 42 30 4C 39 04 C8 75 ?? 33 D2 48 8B CB "
        "E8 ?? ?? ?? ??";
    constexpr int32_t kCycleWeaponCallAt = 53;
}

void ResolveDroneHotbar(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    g_cycleWeapon = PluginMap::FunctionAs<CycleWeapon_t>(scanner, self, DroneFields::Keys::CycleWeapon,
        kCycleWeaponPattern, PLUGIN_SCAN_FUNCTION_START,
        PLUGIN_SCAN_FLAG_OPTIONAL | PLUGIN_SCAN_FLAG_FOLLOW_REL32, 0, kCycleWeaponCallAt);
}
