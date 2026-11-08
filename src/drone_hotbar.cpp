#include "drone_hotbar.h"
#include "drone_fields.h"
#include "drone_interact.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <atomic>
#include <cstdint>
#include <cstdlib>

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

    // The directions the game's inputs pass to the cycle.
    constexpr uint8_t kCycleNext     = 0;
    constexpr uint8_t kCyclePrevious = 1;

    // One wheel notch, as the message reports it (WHEEL_DELTA).
    constexpr int32_t kRawPerNotch = 120;

    // Wheel forward (away from the user) is the previous slot and wheel back
    // the next, the order most PC games use. The game's own binding is not read.
    constexpr uint8_t kForwardCycle = kCyclePrevious;
    constexpr uint8_t kBackCycle    = kCycleNext;

    // The wheel callback runs inside the window's message pump, so it only
    // queues the delta and OnTick makes the game calls. g_inDrone is the tick's last answer, so the
    // callback can pass a notch on when the drone is not out without touching
    // the game's objects.
    std::atomic<bool>    g_inDrone{ false };
    std::atomic<int32_t> g_pendingRawDelta{ 0 };

    // Most cycle calls made in one tick; a flick beyond a full turn of the
    // hotbar only wraps around.
    constexpr int32_t kMaxCyclesPerTick = 9;

    // A part-notch is dropped after this long without wheel input.
    constexpr float kRemainderTimeout = 0.5f;

    // Less than a notch left over, from a wheel that reports small steps.
    // Tick only.
    int32_t g_rawRemainder = 0;
    float   g_remainderAge = 0.0f;

    bool OnMouseWheel(const PluginMouseWheelEvent* event, void*)
    {
        if (!event || event->size < sizeof(PluginMouseWheelEvent))
            return false;

        // While a loader window has capture the game is not getting the wheel
        // anyway. A horizontal wheel and a wheel without Ctrl are not ours, and
        // a menu open on the game's layer owns the wheel.
        if (event->uiCapturing || event->rawDelta == 0 || event->rawDeltaH != 0
            || (event->modifiers & PluginWheelMod_Ctrl) == 0
            || !g_inDrone.load(std::memory_order_relaxed) || IsGameMenuOpen())
            return false;

        // Consumed: neither the game nor a wheel handler registered after this
        // one (the minimap zoom) also sees the notch.
        g_pendingRawDelta.fetch_add(event->rawDelta, std::memory_order_relaxed);
        return true;
    }

    void OnTick(float deltaTime)
    {
        const bool inDrone = IsLocalPlayerInDrone();
        g_inDrone.store(inDrone, std::memory_order_relaxed);

        const int32_t raw = g_pendingRawDelta.exchange(0, std::memory_order_relaxed);
        if (!inDrone)
        {
            g_rawRemainder = 0;
            return;
        }

        if (raw != 0)
            g_remainderAge = 0.0f;
        else if ((g_remainderAge += deltaTime) > kRemainderTimeout)
            g_rawRemainder = 0;

        g_rawRemainder += raw;
        if (std::abs(g_rawRemainder) < kRawPerNotch)
            return;

        SDK::ACrPlayerControllerBase* pc = LocalPlayerController();
        SDK::ACrCharacterPlayerBase* character = DroneFields::ValueOr(DroneFields::CrChar, pc);
        if (!character || DroneFields::Dead(character) || pc->Pawn != static_cast<SDK::APawn*>(character))
        {
            g_rawRemainder = 0;
            return;
        }

        for (int32_t cycles = 0; cycles < kMaxCyclesPerTick && std::abs(g_rawRemainder) >= kRawPerNotch; ++cycles)
        {
            const bool forward = g_rawRemainder > 0;
            g_rawRemainder += forward ? -kRawPerNotch : kRawPerNotch;

            const uint8_t direction = forward ? kForwardCycle : kBackCycle;
            g_cycleWeapon(character, direction);
        }
    }
}

void ResolveDroneHotbar(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    g_cycleWeapon = PluginMap::FunctionAs<CycleWeapon_t>(scanner, self, DroneFields::Keys::CycleWeapon,
        PLUGIN_MAP_PATTERN(kCycleWeaponPattern), PLUGIN_SCAN_FUNCTION_START,
        PLUGIN_SCAN_FLAG_OPTIONAL | PLUGIN_SCAN_FLAG_FOLLOW_REL32, 0, kCycleWeaponCallAt);
}

bool InitDroneHotbar(IPluginSelf* self)
{
    auto* input = self ? self->hooks->Input : nullptr;
    if (!g_cycleWeapon || !input || !input->RegisterMouseWheel)
    {
        LOG_WARN("DroneHotbar: no weapon-cycle call or mouse wheel input -- Ctrl + wheel will not change the hotbar");
        return false;
    }

    input->RegisterMouseWheel(&OnMouseWheel, nullptr);
    self->hooks->Engine->RegisterOnTick(OnTick);

    LOG_INFO("DroneHotbar: Ctrl + mouse wheel changes the hotbar in drone mode");
    return true;
}

void ShutdownDroneHotbar(IPluginSelf* self)
{
    if (!self)
        return;

    if (self->hooks->Input && self->hooks->Input->UnregisterMouseWheel)
        self->hooks->Input->UnregisterMouseWheel(&OnMouseWheel, nullptr);
    self->hooks->Engine->UnregisterOnTick(OnTick);

    g_inDrone.store(false, std::memory_order_relaxed);
    g_pendingRawDelta.store(0, std::memory_order_relaxed);
    g_rawRemainder = 0;
    g_remainderAge = 0.0f;
}
