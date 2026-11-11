#include "drone_hotbar.h"
#include "drone_fields.h"
#include "drone_interact.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace
{
    // The game's hotbar-page system (UCrHotbarLocalPlayerSubsystem) is found by
    // name, not through the Game-SDK: a game build without hotbar pages has no
    // such class, and the page switch is then off. The game's own Next and
    // Previous page inputs read the subsystem's active page and page count and
    // call its reflected SetActiveHotbar(int32) with the new page; this plugin
    // does the same, so a page changes exactly as it does on foot. It never
    // calls ActivateItemOnHotbarByIndex: activating a weapon slot would equip
    // the weapon and end the drone.
    constexpr const char* kSubsystemClass  = "CrHotbarLocalPlayerSubsystem";
    constexpr const char* kSetActiveHotbar = "SetActiveHotbar";

    // The game wraps Previous from the first page to the last one, and never
    // past page index 9.
    constexpr int32_t kMaxPageIndex = 9;

    // One wheel notch, as the message reports it (WHEEL_DELTA).
    constexpr int32_t kRawPerNotch = 120;

    // Wheel forward (away from the user) is the next page and wheel back the
    // previous one, the direction of the game's own plain wheel (forward is the
    // next weapon). The game's binding for its page inputs is in its data and
    // is not read.
    constexpr bool kForwardIsNext = true;

    // A lookup that misses is repeated this often at the earliest, since it
    // scans the game's objects.
    constexpr uint64_t kLookupRetryMs = 5000;

    // The subsystem's class and its SetActiveHotbar, set together. Tick only.
    ObjectRef<SDK::UObject> g_subsystemClass;
    SDK::UFunction*         g_setActiveHotbar = nullptr;
    uint64_t                g_nextLookupMs    = 0;
    bool                    g_missLogged      = false;

    // The wheel callback runs inside the window's message pump, so it only
    // queues the delta and OnTick makes the game calls. g_canSwitch is the
    // tick's last answer to "the drone is out and the page switch is found", so
    // the callback passes a notch on when the switch cannot run, without
    // touching the game's objects.
    std::atomic<bool>    g_canSwitch{ false };
    std::atomic<int32_t> g_pendingRawDelta{ 0 };

    // Most page switches made in one tick; a flick beyond a full lap of the
    // pages only wraps around.
    constexpr int32_t kMaxSwitchesPerTick = kMaxPageIndex + 1;

    // A part-notch is dropped after this long without wheel input.
    constexpr float kRemainderTimeout = 0.5f;

    // Less than a notch left over, from a wheel that reports small steps.
    // Tick only.
    int32_t g_rawRemainder = 0;
    float   g_remainderAge = 0.0f;

    // Finds the subsystem's class and its SetActiveHotbar. False while either is
    // not there, which stays so on a game build without hotbar pages.
    bool ResolvePageSwitch()
    {
        if (g_subsystemClass.Get() && g_setActiveHotbar)
            return true;

        g_subsystemClass.Reset();
        g_setActiveHotbar = nullptr;

        const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now < g_nextLookupMs)
            return false;
        g_nextLookupMs = now + kLookupRetryMs;

        IPluginSelf* self = GetSelf();
        IPluginObjectWalker* walker = self && self->hooks ? self->hooks->ObjectWalker : nullptr;
        if (!walker || !walker->IsReady())
            return false;

        auto* object = static_cast<SDK::UObject*>(walker->FindFirstObjectByName(kSubsystemClass));
        SDK::UFunction* function = nullptr;
        if (object && object->IsA(SDK::UClass::StaticClass())
            && static_cast<SDK::UClass*>(object)->IsSubclassOf(SDK::ULocalPlayerSubsystem::StaticClass()))
            function = static_cast<SDK::UClass*>(object)->GetFunction(kSubsystemClass, kSetActiveHotbar);

        // One int32 in, nothing returned.
        if (!function || function->Size != static_cast<int32_t>(sizeof(int32_t)))
        {
            if (!g_missLogged)
            {
                LOG_WARN("DroneHotbar: no %s::%s(int32) in this game -- Ctrl + wheel will not switch the hotbar page",
                    kSubsystemClass, kSetActiveHotbar);
                g_missLogged = true;
            }
            return false;
        }

        g_subsystemClass.Set(object);
        g_setActiveHotbar = function;
        return true;
    }

    // Moves the local player to the next or previous hotbar page the way the
    // game's own page inputs do. False when nothing was called.
    bool SwitchPage(SDK::ACrPlayerControllerBase* pc, bool next)
    {
        if (!ResolvePageSwitch())
            return false;

        auto* subsystem = static_cast<SDK::UObject*>(SDK::USubsystemBlueprintLibrary::GetLocalPlayerSubsystem(
            pc, static_cast<SDK::UClass*>(g_subsystemClass.Get())));
        const uint8_t*         active  = DroneFields::CurrentHotbarIndex.In(subsystem);
        const HotbarArrayData* hotbars = DroneFields::Hotbars.In(subsystem);
        if (!active || !hotbars || hotbars->Num <= 0)
            return false;

        const int32_t current = *active;
        const int32_t last    = hotbars->Num - 1;

        int32_t page = 0;
        if (next)
            page = current >= last ? 0 : current + 1;
        else
            page = current != 0 ? current - 1 : (std::min)(kMaxPageIndex, last);

        page = (std::min)(page, last);
        subsystem->ProcessEvent(g_setActiveHotbar, &page);
        return true;
    }

    bool OnMouseWheel(const PluginMouseWheelEvent* event, void*)
    {
        if (!event || event->size < sizeof(PluginMouseWheelEvent))
            return false;

        // While a loader window has capture the game is not getting the wheel
        // anyway. A horizontal wheel and a wheel without Ctrl are not ours, and
        // a menu open on the game's layer owns the wheel.
        if (event->uiCapturing || event->rawDelta == 0 || event->rawDeltaH != 0
            || (event->modifiers & PluginWheelMod_Ctrl) == 0
            || !g_canSwitch.load(std::memory_order_relaxed) || IsGameMenuOpen())
            return false;

        // Consumed: neither the game nor a wheel handler registered after this
        // one (the minimap zoom) also sees the notch.
        g_pendingRawDelta.fetch_add(event->rawDelta, std::memory_order_relaxed);
        return true;
    }

    void OnTick(float deltaTime)
    {
        const bool inDrone = IsLocalPlayerInDrone();
        g_canSwitch.store(inDrone && ResolvePageSwitch(), std::memory_order_relaxed);

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

        for (int32_t switches = 0; switches < kMaxSwitchesPerTick && std::abs(g_rawRemainder) >= kRawPerNotch; ++switches)
        {
            const bool forward = g_rawRemainder > 0;
            g_rawRemainder += forward ? -kRawPerNotch : kRawPerNotch;

            // A page that is not there (no subsystem yet, no pages) drops the
            // rest of the flick instead of retrying it on every notch.
            if (!SwitchPage(pc, forward == kForwardIsNext))
            {
                g_rawRemainder = 0;
                break;
            }
        }
    }
}

bool InitDroneHotbar(IPluginSelf* self)
{
    auto* input = self ? self->hooks->Input : nullptr;
    if (!input || !input->RegisterMouseWheel)
    {
        LOG_WARN("DroneHotbar: no mouse wheel input -- Ctrl + wheel will not switch the hotbar page");
        return false;
    }

    input->RegisterMouseWheel(&OnMouseWheel, nullptr);
    self->hooks->Engine->RegisterOnTick(OnTick);

    LOG_INFO("DroneHotbar: Ctrl + mouse wheel switches the hotbar page in drone mode");
    return true;
}

void ShutdownDroneHotbar(IPluginSelf* self)
{
    if (!self)
        return;

    if (self->hooks->Input && self->hooks->Input->UnregisterMouseWheel)
        self->hooks->Input->UnregisterMouseWheel(&OnMouseWheel, nullptr);
    self->hooks->Engine->UnregisterOnTick(OnTick);

    g_canSwitch.store(false, std::memory_order_relaxed);
    g_pendingRawDelta.store(0, std::memory_order_relaxed);
    g_rawRemainder = 0;
    g_remainderAge = 0.0f;
    g_subsystemClass.Reset();
    g_setActiveHotbar = nullptr;
    g_nextLookupMs = 0;
    g_missLogged = false;
}
