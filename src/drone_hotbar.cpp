#include "drone_hotbar.h"
#include "drone_fields.h"
#include "drone_interact.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

    // DIAG: capture of what the game calls when the hotbar switches on foot.
    // A window opens for kDiagWindowMs after any vertical wheel notch or a
    // press of the number keys 1-9; while it is open a UObject::ProcessEvent
    // observer counts the hotbar-related functions the game runs, one entry per
    // distinct "Owner::Func on ObjClass". When the window closes (OnTick) it
    // logs one summary line per entry, in first-seen order, tagged with the
    // trigger that opened it. Functions with hotbar, slot, equip or
    // activeweapon in the name also log a line the first time they are seen in
    // a window. Outside the window the observer costs one clock read.
    constexpr int64_t kDiagWindowMs = 1500;

    using ProcessEvent_t = void(__fastcall*)(void* obj, void* fn, void* params);
    ProcessEvent_t g_origProcessEvent = nullptr;
    HookHandle     g_hookProcessEvent = nullptr;
    uintptr_t      g_addrProcessEvent = 0;

    using CycleWeaponHook_t = void(__fastcall*)(SDK::ACrCharacterPlayerBase*, uint8_t);
    CycleWeaponHook_t g_origCycleWeapon = nullptr;
    HookHandle        g_hookCycleWeapon = nullptr;
    bool              g_ownCycleCall    = false;  // game thread only

    std::atomic<int64_t>  g_diagWindowEndMs{ 0 };
    std::atomic<uint32_t> g_diagSeq{ 0 };

    struct DiagFn { bool match; bool key; std::string text; };
    struct DiagEntry { std::string text; uint32_t count; };
    std::mutex g_diagMutex;
    std::unordered_map<const void*, DiagFn> g_diagFns;    // UFunction* -> verdict, guarded by g_diagMutex
    std::vector<DiagEntry> g_diagEntries;                 // first-seen order, guarded by g_diagMutex
    std::unordered_map<std::string, size_t> g_diagIndex;  // entry text -> index in g_diagEntries, guarded by g_diagMutex
    std::string g_diagTrigger;                            // what opened the window, guarded by g_diagMutex
    bool g_diagOpen = false;                              // guarded by g_diagMutex

    int64_t DiagNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void DiagOpenWindow(const char* why)
    {
        const int64_t now = DiagNowMs();
        if (g_diagWindowEndMs.load(std::memory_order_relaxed) <= now)
        {
            std::lock_guard<std::mutex> lock(g_diagMutex);
            g_diagEntries.clear();
            g_diagIndex.clear();
            g_diagTrigger = why;
            g_diagOpen = true;
            LOG_INFO("DIAG hotbar capture window open (%s) drone=%d", why, (int)g_inDrone.load(std::memory_order_relaxed));
        }
        g_diagWindowEndMs.store(now + kDiagWindowMs, std::memory_order_relaxed);
    }

    // Logs the counted entries of a window that has closed, or of one cut
    // short by shutdown (force). Game thread (OnTick) or the shutdown path.
    void DiagFlushWindow(bool force)
    {
        if (!force && g_diagWindowEndMs.load(std::memory_order_relaxed) > DiagNowMs())
            return;
        std::lock_guard<std::mutex> lock(g_diagMutex);
        if (!g_diagOpen)
            return;
        g_diagOpen = false;
        LOG_INFO("DIAG hotbar capture window closed (%s): %u distinct", g_diagTrigger.c_str(), (unsigned)g_diagEntries.size());
        for (const DiagEntry& entry : g_diagEntries)
            LOG_INFO("DIAG [%s] %ux %s", g_diagTrigger.c_str(), (unsigned)entry.count, entry.text.c_str());
        g_diagEntries.clear();
        g_diagIndex.clear();
    }

    // Function names whose first call in a window is logged on its own, so the
    // order of the key calls is visible.
    bool DiagIsKeyName(const std::string& name)
    {
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const char* const kWords[] = { "hotbar", "slot", "equip", "activeweapon" };
        for (const char* word : kWords)
            if (lower.find(word) != std::string::npos)
                return true;
        return false;
    }

    bool DiagNameMatches(const std::string& name)
    {
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const char* const kWords[] = { "hotbar", "weapon", "equip", "slot", "cycle", "inventory", "quickuse", "item" };
        for (const char* word : kWords)
            if (lower.find(word) != std::string::npos)
                return true;
        return false;
    }

    void __fastcall Detour_ProcessEvent(void* obj, void* fn, void* params)
    {
        if (obj && fn && g_diagWindowEndMs.load(std::memory_order_relaxed) > DiagNowMs())
        {
            std::lock_guard<std::mutex> lock(g_diagMutex);
            auto it = g_diagFns.find(fn);
            if (it == g_diagFns.end())
            {
                const auto* func = static_cast<const SDK::UFunction*>(fn);
                const std::string name = func->GetName();
                const std::string owner = func->Outer ? func->Outer->GetName() : std::string("?");
                it = g_diagFns.emplace(fn, DiagFn{ DiagNameMatches(name) || DiagNameMatches(owner), DiagIsKeyName(name), owner + "::" + name }).first;
            }
            if (it->second.match && g_diagOpen)
            {
                const auto* object = static_cast<const SDK::UObject*>(obj);
                const std::string text = it->second.text + " on " + (object->Class ? object->Class->GetName() : std::string("?"))
                    + " drone=" + std::to_string((int)g_inDrone.load(std::memory_order_relaxed));
                const auto found = g_diagIndex.find(text);
                if (found != g_diagIndex.end())
                    ++g_diagEntries[found->second].count;
                else
                {
                    g_diagIndex.emplace(text, g_diagEntries.size());
                    g_diagEntries.push_back(DiagEntry{ text, 1 });
                    if (it->second.key)
                        LOG_INFO("DIAG first call #%u [%s]: %s", g_diagSeq.fetch_add(1, std::memory_order_relaxed) + 1,
                            g_diagTrigger.c_str(), text.c_str());
                }
            }
        }

        g_origProcessEvent(obj, fn, params);
    }

    // The game's next-weapon and previous-weapon inputs call this; so does
    // this plugin's Ctrl + wheel, flagged as "plugin".
    void __fastcall Detour_NativeOnCycleWeapon(SDK::ACrCharacterPlayerBase* character, uint8_t direction)
    {
        LOG_INFO("DIAG NativeOnCycleWeapon #%u: dir=%d source=%s drone=%d",
            g_diagSeq.fetch_add(1, std::memory_order_relaxed) + 1, (int)direction,
            g_ownCycleCall ? "plugin" : "game", (int)g_inDrone.load(std::memory_order_relaxed));
        g_origCycleWeapon(character, direction);
    }

    void OnDiagNumberKey(EModKey key, EModKeyEvent)
    {
        char why[16];
        snprintf(why, sizeof(why), "key %d", (int)key - (int)EModKey::One + 1);
        DiagOpenWindow(why);
    }

    bool OnMouseWheel(const PluginMouseWheelEvent* event, void*)
    {
        if (!event || event->size < sizeof(PluginMouseWheelEvent))
            return false;

        if (event->rawDelta != 0 && event->rawDeltaH == 0)
        {
            const char* mods = (event->modifiers & PluginWheelMod_Ctrl) ? "ctrl"
                : (event->modifiers & (PluginWheelMod_LeftAlt | PluginWheelMod_RightAlt)) ? "alt" : "none";
            char why[48];
            snprintf(why, sizeof(why), "wheel mods=%s dir=%s", mods, event->rawDelta > 0 ? "forward" : "back");
            DiagOpenWindow(why);  // DIAG
        }

        // DIAG: say which gate rejected each vertical notch while in the drone.
        if (event->rawDelta != 0 && event->rawDeltaH == 0 && g_inDrone.load(std::memory_order_relaxed))
        {
            const char* verdict = "queued";
            if (event->uiCapturing)
                verdict = "rejected: uiCapturing";
            else if ((event->modifiers & PluginWheelMod_Ctrl) == 0)
                verdict = "rejected: Ctrl not held";
            else if (IsGameMenuOpen())
                verdict = "rejected: game menu or map open";
            LOG_INFO("DIAG DroneHotbar wheel: raw=%d mods=0x%x -> %s", (int)event->rawDelta, (unsigned)event->modifiers, verdict);
        }
        else if (event->rawDelta != 0 && event->rawDeltaH == 0)
            LOG_INFO("DIAG DroneHotbar wheel: raw=%d -> rejected: not in drone", (int)event->rawDelta);
        else if (event->rawDeltaH != 0)
            LOG_INFO("DIAG DroneHotbar wheel: rawH=%d -> rejected: horizontal wheel", (int)event->rawDeltaH);

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

    // DIAG: the weapon component's equipped item and unarmed flag, read through
    // fields BetterDrone already maps, before and after a cycle call.
    void DiagWeaponState(const char* when, uint8_t direction)
    {
        SDK::ACrPlayerControllerBase* pc = LocalPlayerController();
        SDK::ACrCharacterPlayerBase* character = DroneFields::ValueOr(DroneFields::CrChar, pc);
        SDK::UCrWeaponComponent* weapons = DroneFields::ValueOr(DroneFields::WeaponSystem, character);
        const auto* equipped = DroneFields::EquippedWeapon.In(weapons);
        SDK::UAuWeaponItemDataBase* item = DroneFields::ValueOr(DroneFields::EquippedItemData, equipped);
        LOG_INFO("DIAG DroneHotbar cycle %s dir=%d: equipped item=%s class=%s unarmed=%d",
            when, (int)direction,
            item ? item->GetName().c_str() : "none",
            item && item->Class ? item->Class->GetName().c_str() : "none",
            (int)DroneFields::ValueOr(DroneFields::LastEquippedUnarmed, weapons, false));
    }

    void OnTick(float deltaTime)
    {
        DiagFlushWindow(false);  // DIAG

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

        LOG_INFO("DIAG DroneHotbar tick: %d notch(es), %s", (int)(std::abs(g_rawRemainder) / kRawPerNotch),  // DIAG
            g_rawRemainder > 0 ? "forward" : "back");
        for (int32_t cycles = 0; cycles < kMaxCyclesPerTick && std::abs(g_rawRemainder) >= kRawPerNotch; ++cycles)
        {
            const bool forward = g_rawRemainder > 0;
            g_rawRemainder += forward ? -kRawPerNotch : kRawPerNotch;

            const uint8_t direction = forward ? kForwardCycle : kBackCycle;
            DiagWeaponState("before", direction);  // DIAG
            g_ownCycleCall = true;  // DIAG
            g_cycleWeapon(character, direction);
            g_ownCycleCall = false;  // DIAG
            DiagWeaponState("after", direction);  // DIAG
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

    // DIAG: the loader's map row for UObject::ProcessEvent, for the observer.
    g_addrProcessEvent = DroneFields::ResolveFunction(self, scanner, DroneFields::Keys::ProcessEvent, nullptr);
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

    // DIAG: the number-key trigger, the ProcessEvent observer and the cycle detour.
    for (EModKey key = EModKey::One; key <= EModKey::Nine; key = static_cast<EModKey>(static_cast<uint32_t>(key) + 1))
        input->RegisterKeybind(key, EModKeyEvent::Pressed, &OnDiagNumberKey);
    if (g_addrProcessEvent)
    {
        g_hookProcessEvent = self->hooks->Hooks->Install(g_addrProcessEvent,
            reinterpret_cast<void*>(&Detour_ProcessEvent), reinterpret_cast<void**>(&g_origProcessEvent));
        LOG_INFO("DIAG ProcessEvent observer at 0x%llX: %s", (unsigned long long)g_addrProcessEvent,
            g_hookProcessEvent ? "installed" : "install failed");
    }
    else
        LOG_INFO("DIAG ProcessEvent observer: the loader's map has no UObject::ProcessEvent row");
    g_hookCycleWeapon = self->hooks->Hooks->Install(reinterpret_cast<uintptr_t>(g_cycleWeapon),
        reinterpret_cast<void*>(&Detour_NativeOnCycleWeapon), reinterpret_cast<void**>(&g_origCycleWeapon));
    LOG_INFO("DIAG NativeOnCycleWeapon detour: %s", g_hookCycleWeapon ? "installed" : "install failed");

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

    // DIAG
    if (g_hookProcessEvent)
        self->hooks->Hooks->Remove(g_hookProcessEvent);
    if (g_hookCycleWeapon)
        self->hooks->Hooks->Remove(g_hookCycleWeapon);
    g_hookProcessEvent = nullptr;
    g_hookCycleWeapon = nullptr;
    DiagFlushWindow(true);
    g_diagWindowEndMs.store(0, std::memory_order_relaxed);
    if (self->hooks->Input && self->hooks->Input->UnregisterKeybind)
        for (EModKey key = EModKey::One; key <= EModKey::Nine; key = static_cast<EModKey>(static_cast<uint32_t>(key) + 1))
            self->hooks->Input->UnregisterKeybind(key, EModKeyEvent::Pressed, &OnDiagNumberKey);

    g_inDrone.store(false, std::memory_order_relaxed);
    g_pendingRawDelta.store(0, std::memory_order_relaxed);
    g_rawRemainder = 0;
    g_remainderAge = 0.0f;
}
