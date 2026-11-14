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
    // The game's hotbar system (UCrHotbarLocalPlayerSubsystem) is found by name,
    // not through the Game-SDK: a game build without hotbar pages has no such
    // class, and both features are then off. The game's own Next and Previous
    // page inputs read the subsystem's active page and page count and call its
    // reflected SetActiveHotbar(int32) with the new page; this plugin does the
    // same, so a page changes exactly as it does on foot. Its number keys call
    // the reflected ActivateItemOnHotbarByIndex(int32), as the game's own digit
    // inputs do, but only for a building slot: a weapon slot would equip the
    // weapon and end the drone.
    constexpr const char* kSubsystemClass  = "CrHotbarLocalPlayerSubsystem";
    constexpr const char* kSetActiveHotbar = "SetActiveHotbar";
    constexpr const char* kActivateItem    = "ActivateItemOnHotbarByIndex";

    // The slot type of a building: EHotbarInternalSlotType::Building, read by
    // name from the game's own UEnum, since the numbers of a game enum can move in
    // an update. Looked up once, on the tick.
    uint8_t g_slotBuilding = 0;

    // The game wraps Previous from the first page to the last one, and never
    // past page index 9.
    constexpr int32_t kMaxPageIndex = 9;

    // The modifier that makes the wheel pick a slot on foot in build or deconstruct
    // mode. None is the plain wheel, which replaces the game's building height and
    // spline tangent wheel actions there; PluginWheelMod_Alt moves the pick to
    // Alt + wheel and leaves them alone. The drone always uses the plain wheel.
    constexpr uint32_t kOnFootWheelModifier = PluginWheelMod_None;

    // A page has ten slots: the digits 1 to 9, then 0.
    constexpr int32_t kSlotsPerPage = 10;

    // One wheel notch, as the message reports it (WHEEL_DELTA).
    constexpr int32_t kRawPerNotch = 120;

    // Wheel forward (away from the user) is the next page or slot and wheel back
    // the previous one, the direction of the game's own plain wheel (forward is
    // the next weapon). The game's binding for its page inputs is in its data and
    // is not read.
    constexpr bool kForwardIsNext = true;

    // A lookup that misses is repeated this often at the earliest, since it
    // scans the game's objects.
    constexpr uint64_t kLookupRetryMs = 5000;

    // Most steps made in one tick; a flick beyond a full lap only wraps around.
    constexpr int32_t kMaxStepsPerTick = kMaxPageIndex + 1;

    // A part-notch is dropped after this long without wheel input.
    constexpr float kRemainderTimeout = 0.5f;

    // The notches of one wheel use. The callback adds the raw delta, the tick
    // takes whole notches out.
    struct WheelNotches
    {
        std::atomic<int32_t> pending{ 0 };
        int32_t              remainder = 0; // less than a notch left over; tick only
        float                age       = 0.0f;

        // Whole notches since the last call, forward positive. Tick only.
        int32_t Take(float deltaTime)
        {
            const int32_t raw = pending.exchange(0, std::memory_order_relaxed);
            if (raw != 0)
                age = 0.0f;
            else if ((age += deltaTime) > kRemainderTimeout)
                remainder = 0;

            remainder += raw;
            const int32_t notches = remainder / kRawPerNotch;
            remainder -= notches * kRawPerNotch;
            return notches;
        }

        void Reset()
        {
            pending.store(0, std::memory_order_relaxed);
            remainder = 0;
            age       = 0.0f;
        }
    };

    uint64_t NowMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    bool g_pageOn   = false;
    bool g_selectOn = false;

    // ECrPlayerControlState values, read by name from the game's own UEnum.
    uint8_t g_controlBuilding    = 0;
    uint8_t g_controlDeconstruct = 0;

    // The subsystem's class and its functions. Tick only.
    ObjectRef<SDK::UObject> g_subsystemClass;
    SDK::UFunction*         g_setActiveHotbar = nullptr;
    SDK::UFunction*         g_activateItem    = nullptr;
    uint64_t                g_nextLookupMs    = 0;
    bool                    g_missLogged      = false;

    // The wheel callback runs inside the window's message pump, so it only
    // queues deltas and slots, and OnTick makes the game calls. g_canPage and
    // g_canSelect are the tick's last answers to "the drone is out and this can
    // run", so the callback passes a notch on when it cannot, without touching
    // the game's objects.
    std::atomic<bool> g_canPage{ false };
    std::atomic<bool> g_canSelect{ false };     // in the drone, plain wheel
    std::atomic<bool> g_canSelectFoot{ false }; // on foot in build or deconstruct mode
    WheelNotches      g_pageWheel;
    WheelNotches      g_slotWheel;

    // The slot a digit asked for (0 to 9), or -1. Written by the key callback.
    std::atomic<int32_t> g_pendingSlot{ -1 };

    // The same digit on foot, where the game picks the slot itself: the last
    // digit pressed and when, so the wheel steps on from the slot the game
    // picked. The bind does not consume the key. Stale after a second.
    std::atomic<int32_t>  g_footSlot{ -1 };
    std::atomic<uint64_t> g_footSlotMs{ 0 };
    constexpr uint64_t    kFootSlotFreshMs = 1000;

    // The game has no "current slot" member, so this is the last slot this plugin
    // picked, on the page it picked it on, or -1. Tick only.
    int32_t g_lastSlot = -1;
    int32_t g_lastPage = -1;

    // The function `name` of the subsystem's class, when it takes one int32 and
    // returns nothing.
    SDK::UFunction* FindIntFunction(SDK::UClass* cls, const char* name)
    {
        SDK::UFunction* function = cls->GetFunction(kSubsystemClass, name);
        return function && function->Size == static_cast<int32_t>(sizeof(int32_t)) ? function : nullptr;
    }

    // Finds the subsystem's class and the functions of the features that are on.
    // False while any is not there, which stays so on a game build without
    // hotbar pages.
    bool ResolveSubsystem()
    {
        if (g_subsystemClass.Get() && (!g_pageOn || g_setActiveHotbar) && (!g_selectOn || g_activateItem))
            return true;

        g_subsystemClass.Reset();
        g_setActiveHotbar = nullptr;
        g_activateItem    = nullptr;

        const uint64_t now = NowMs();
        if (now < g_nextLookupMs)
            return false;
        g_nextLookupMs = now + kLookupRetryMs;

        IPluginSelf* self = GetSelf();
        IPluginObjectWalker* walker = self && self->hooks ? self->hooks->ObjectWalker : nullptr;
        if (!walker || !walker->IsReady())
            return false;

        auto* object = static_cast<SDK::UObject*>(walker->FindFirstObjectByName(kSubsystemClass));
        SDK::UFunction* setActive = nullptr;
        SDK::UFunction* activate  = nullptr;
        if (object && object->IsA(SDK::UClass::StaticClass())
            && static_cast<SDK::UClass*>(object)->IsSubclassOf(SDK::ULocalPlayerSubsystem::StaticClass()))
        {
            auto* cls = static_cast<SDK::UClass*>(object);
            setActive = FindIntFunction(cls, kSetActiveHotbar);
            activate  = FindIntFunction(cls, kActivateItem);
        }

        if ((g_pageOn && !setActive) || (g_selectOn && !activate))
        {
            if (!g_missLogged)
            {
                LOG_WARN("DroneHotbar: no %s::%s(int32) or %s::%s(int32) in this game -- the hotbar in the drone will not work",
                    kSubsystemClass, kSetActiveHotbar, kSubsystemClass, kActivateItem);
                g_missLogged = true;
            }
            return false;
        }

        g_subsystemClass.Set(object);
        g_setActiveHotbar = setActive;
        g_activateItem    = activate;
        return true;
    }

    // The local player's hotbar subsystem, or null. Valid for this tick only.
    SDK::UObject* LocalSubsystem(SDK::ACrPlayerControllerBase* pc)
    {
        return static_cast<SDK::UObject*>(SDK::USubsystemBlueprintLibrary::GetLocalPlayerSubsystem(
            pc, static_cast<SDK::UClass*>(g_subsystemClass.Get())));
    }

    // Moves the local player to the next or previous hotbar page the way the
    // game's own page inputs do. False when nothing was called.
    bool SwitchPage(SDK::UObject* subsystem, bool next)
    {
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

    // The slots of the current page, as the game holds them. Valid for this tick
    // only.
    struct PageSlots
    {
        int32_t        page  = -1;
        const uint8_t* slots = nullptr;
        int32_t        count = 0;
    };

    bool ReadPage(const SDK::UObject* subsystem, PageSlots& out)
    {
        const uint8_t*         active  = DroneFields::CurrentHotbarIndex.In(subsystem);
        const HotbarArrayData* hotbars = DroneFields::Hotbars.In(subsystem);
        if (!active || !hotbars)
            return false;

        const uint8_t* entry = PluginMap::ElementAt(hotbars->Data, hotbars->Num, *active, DroneFields::HotbarPageSize);
        const HotbarArrayData* slots = DroneFields::HotbarSlots.In(static_cast<const void*>(entry));
        if (!slots)
            return false;

        out.page  = *active;
        out.slots = static_cast<const uint8_t*>(slots->Data);
        out.count = slots->Num;
        return true;
    }

    // Looks up the building slot type the first time it is needed. False when the
    // enum or its entry is not on this build: the slot picking is then left off,
    // since only this number keeps weapon and consumable slots out.
    bool BuildingSlotTypeResolved()
    {
        static bool tried    = false;
        static bool resolved = false;
        if (tried)
            return resolved;

        tried = true;
        int64_t value = 0;
        if (PluginMap::EnumValue("EHotbarInternalSlotType", "Building", value) != PluginMap::EnumLookup::Found
            || value < 0 || value > UINT8_MAX)
        {
            LOG_WARN("DroneHotbar: EHotbarInternalSlotType Building not found on this build "
                     "-- the number keys and the wheel will not pick a slot in the drone");
            return false;
        }

        g_slotBuilding = static_cast<uint8_t>(value);
        resolved       = true;
        return true;
    }

    bool IsBuildingSlot(const PageSlots& page, int32_t index)
    {
        const uint8_t* slot = PluginMap::ElementAt(page.slots, page.count, index, DroneFields::HotbarSlotSize);
        const uint8_t* type = DroneFields::HotbarSlotType.In(static_cast<const void*>(slot));
        return type && *type == g_slotBuilding;
    }

    // The building slot after (or before) `from`, wrapping around the page; -1
    // when the page has none. `from` -1 is no slot yet: next starts at the first
    // slot and previous at the last. The slot `from` itself comes last.
    int32_t FindBuildingSlot(const PageSlots& page, int32_t from, bool next)
    {
        if (page.count <= 0)
            return -1;
        if (from < 0)
            from = next ? -1 : 0;

        for (int32_t step = 1; step <= page.count; ++step)
        {
            const int32_t index = ((from + (next ? step : -step)) % page.count + page.count) % page.count;
            if (IsBuildingSlot(page, index))
                return index;
        }
        return -1;
    }

    // Picks slot `index` of the page the way the digit does on foot, when it is
    // a building slot. False when nothing was called.
    bool PickBuildingSlot(SDK::UObject* subsystem, const PageSlots& page, int32_t index)
    {
        if (!IsBuildingSlot(page, index))
            return false;

        subsystem->ProcessEvent(g_activateItem, &index);
        g_lastSlot = index;
        return true;
    }

    // Looks up the control states of build and deconstruct mode the first time
    // the on-foot pick needs them. False when the enum or an entry is not on this
    // build: the on-foot pick stays off, quietly, and the drone part is unaffected.
    bool ControlStatesResolved()
    {
        static bool tried    = false;
        static bool resolved = false;
        if (tried)
            return resolved;

        tried = true;
        if (!DroneFields::ControlStateValue("BuildingMode", g_controlBuilding)
            || !DroneFields::ControlStateValue("DeconstructMode", g_controlDeconstruct))
        {
            LOG_INFO("DroneHotbar: ECrPlayerControlState BuildingMode/DeconstructMode not found on this build "
                     "-- the wheel does not pick a hotbar slot on foot");
            return false;
        }

        resolved = true;
        return true;
    }

    // True while the local player on foot has the building tool out: build mode
    // or deconstruct mode. Tick only.
    bool InPickMode(SDK::ACrPlayerControllerBase* pc)
    {
        if (!pc || !ControlStatesResolved())
            return false;

        const uint8_t* state = DroneFields::PlayerControlState.In(pc);
        return state && (*state == g_controlBuilding || *state == g_controlDeconstruct);
    }

    // True when the wheel's modifiers are exactly `required`: none for the plain
    // wheel, otherwise only that modifier (either side) held.
    bool ModifiersAre(uint32_t held, uint32_t required)
    {
        return required == PluginWheelMod_None ? held == PluginWheelMod_None
                                               : (held & required) != 0 && (held & ~required) == 0;
    }

    // The slot a number key is: 1 to 9 are slots 0 to 8 and 0 is slot 9.
    int32_t SlotOfKey(EModKey key)
    {
        return key == EModKey::Zero ? 9 : static_cast<int32_t>(key) - static_cast<int32_t>(EModKey::One);
    }

    void OnDigitKey(EModKey key, EModKeyEvent event)
    {
        if (event != EModKeyEvent::Pressed)
            return;
        g_pendingSlot.store(SlotOfKey(key), std::memory_order_relaxed);
        g_footSlot.store(SlotOfKey(key), std::memory_order_relaxed);
        g_footSlotMs.store(NowMs(), std::memory_order_relaxed);
    }

    bool OnMouseWheel(const PluginMouseWheelEvent* event, void*)
    {
        if (!event || event->size < sizeof(PluginMouseWheelEvent))
            return false;

        // While a loader window has capture the game is not getting the wheel
        // anyway. A horizontal wheel is not ours, and a menu open on the game's
        // layer owns the wheel.
        if (event->uiCapturing || event->rawDelta == 0 || event->rawDeltaH != 0 || IsGameMenuOpen())
            return false;

        // Ctrl is the page, no modifier at all is the slot in the drone, and
        // kOnFootWheelModifier is the slot on foot in build or deconstruct mode.
        // A notch is consumed only when the tick can use it, so neither the game
        // nor a wheel handler registered after this one (the minimap zoom) also
        // sees it.
        if ((event->modifiers & PluginWheelMod_Ctrl) != 0)
        {
            if (!g_canPage.load(std::memory_order_relaxed))
                return false;
            g_pageWheel.pending.fetch_add(event->rawDelta, std::memory_order_relaxed);
            return true;
        }

        const bool pick = (ModifiersAre(event->modifiers, PluginWheelMod_None) && g_canSelect.load(std::memory_order_relaxed))
            || (ModifiersAre(event->modifiers, kOnFootWheelModifier) && g_canSelectFoot.load(std::memory_order_relaxed));
        if (!pick)
            return false;
        g_slotWheel.pending.fetch_add(event->rawDelta, std::memory_order_relaxed);
        return true;
    }

    // Drops everything queued and the slot this plugin last picked.
    void ResetState()
    {
        g_pageWheel.Reset();
        g_slotWheel.Reset();
        g_pendingSlot.store(-1, std::memory_order_relaxed);
        g_lastSlot = -1;
        g_lastPage = -1;
    }

    void OnTick(float deltaTime)
    {
        // The drone, or on foot with the building tool out (the wheel pick only).
        const bool inDrone = IsLocalPlayerInDrone();
        SDK::ACrPlayerControllerBase* pc = inDrone || g_selectOn ? LocalPlayerController() : nullptr;
        const bool onFoot  = !inDrone && InPickMode(pc);
        const bool found   = (inDrone || onFoot) && ResolveSubsystem();

        // The same gates for both: the local character is alive and is the pawn
        // the player controls. The key callbacks are already held back by the
        // loader while a text field has focus.
        if (!found)
            pc = nullptr;
        SDK::ACrCharacterPlayerBase* character = DroneFields::ValueOr(DroneFields::CrChar, pc);
        const bool ready = character && !DroneFields::Dead(character)
            && pc->Pawn == static_cast<SDK::APawn*>(character);

        g_canPage.store(g_pageOn && ready && inDrone, std::memory_order_relaxed);
        if (!ready)
        {
            g_canSelect.store(false, std::memory_order_relaxed);
            g_canSelectFoot.store(false, std::memory_order_relaxed);
            ResetState();
            return;
        }

        SDK::UObject* subsystem = LocalSubsystem(pc);

        if (g_pageOn && inDrone)
        {
            const int32_t notches = g_pageWheel.Take(deltaTime);
            for (int32_t steps = 0; steps < (std::min)(std::abs(notches), kMaxStepsPerTick); ++steps)
            {
                // A page that is not there (no subsystem yet, no pages) drops the
                // rest of the flick instead of retrying it on every notch.
                if (!SwitchPage(subsystem, (notches > 0) == kForwardIsNext))
                {
                    g_pageWheel.Reset();
                    break;
                }
            }
        }

        // Read after the page switch above, so a slot is picked on the page the
        // player now sees.
        PageSlots page;
        const bool havePage = g_selectOn && !IsGameMenuOpen() && BuildingSlotTypeResolved()
            && ReadPage(subsystem, page);
        if (!havePage)
        {
            g_canSelect.store(false, std::memory_order_relaxed);
            g_canSelectFoot.store(false, std::memory_order_relaxed);
            g_slotWheel.Reset();
            g_pendingSlot.store(-1, std::memory_order_relaxed);
            return;
        }

        // A page of its own starts with no slot picked.
        if (page.page != g_lastPage)
        {
            g_lastPage = page.page;
            g_lastSlot = -1;
        }

        // The wheel is taken only on a page that has a building slot to go to.
        const bool anyBuilding = FindBuildingSlot(page, -1, true) >= 0;
        g_canSelect.store(inDrone && anyBuilding, std::memory_order_relaxed);
        g_canSelectFoot.store(onFoot && anyBuilding, std::memory_order_relaxed);

        // The number keys are the game's own on foot, so the game has picked the
        // slot already: only note it, so the wheel steps on from it. (A click on
        // the hotbar with the mouse cannot be seen.)
        const int32_t digit = g_pendingSlot.exchange(-1, std::memory_order_relaxed);
        const int32_t footDigit = g_footSlot.exchange(-1, std::memory_order_relaxed);
        if (onFoot && footDigit >= 0 && footDigit < kSlotsPerPage
            && NowMs() - g_footSlotMs.load(std::memory_order_relaxed) < kFootSlotFreshMs
            && IsBuildingSlot(page, footDigit))
            g_lastSlot = footDigit;
        if (inDrone && digit >= 0 && digit < kSlotsPerPage)
        {
            // The page's slots are not read again after a game call.
            PickBuildingSlot(subsystem, page, digit);
            g_slotWheel.Reset();
            return;
        }

        // The notches of a flick walk to the slot they end on; only that one is
        // picked, so one flick is one call.
        const int32_t notches = g_slotWheel.Take(deltaTime);
        int32_t target = g_lastSlot;
        for (int32_t steps = 0; steps < (std::min)(std::abs(notches), kMaxStepsPerTick); ++steps)
        {
            const int32_t next = FindBuildingSlot(page, target, (notches > 0) == kForwardIsNext);
            if (next < 0)
                break;
            target = next;
        }
        if (target != g_lastSlot)
            PickBuildingSlot(subsystem, page, target);
    }
}

bool InitDroneHotbar(IPluginSelf* self, bool page, bool select)
{
    auto* input = self ? self->hooks->Input : nullptr;
    if (!input || !input->RegisterMouseWheel)
    {
        LOG_WARN("DroneHotbar: no mouse wheel input -- the hotbar will not work in the drone");
        return false;
    }

    g_pageOn   = page;
    g_selectOn = select;

    input->RegisterMouseWheel(&OnMouseWheel, nullptr);
    if (select)
    {
        for (int32_t key = static_cast<int32_t>(EModKey::Zero); key <= static_cast<int32_t>(EModKey::Nine); ++key)
            input->RegisterKeybind(static_cast<EModKey>(key), EModKeyEvent::Pressed, OnDigitKey);
    }
    self->hooks->Engine->RegisterOnTick(OnTick);

    if (page)
        LOG_INFO("DroneHotbar: Ctrl + mouse wheel switches the hotbar page in drone mode");
    if (select)
        LOG_INFO("DroneHotbar: the number keys and the mouse wheel pick a building from the hotbar in drone mode, "
                 "and the wheel does on foot in build and deconstruct mode");
    return true;
}

void ShutdownDroneHotbar(IPluginSelf* self)
{
    if (!self)
        return;

    if (self->hooks->Input && self->hooks->Input->UnregisterMouseWheel)
        self->hooks->Input->UnregisterMouseWheel(&OnMouseWheel, nullptr);
    if (self->hooks->Input && g_selectOn)
    {
        for (int32_t key = static_cast<int32_t>(EModKey::Zero); key <= static_cast<int32_t>(EModKey::Nine); ++key)
            self->hooks->Input->UnregisterKeybind(static_cast<EModKey>(key), EModKeyEvent::Pressed, OnDigitKey);
    }
    self->hooks->Engine->UnregisterOnTick(OnTick);

    g_canPage.store(false, std::memory_order_relaxed);
    g_canSelect.store(false, std::memory_order_relaxed);
    g_canSelectFoot.store(false, std::memory_order_relaxed);
    ResetState();
    g_footSlot.store(-1, std::memory_order_relaxed);
    g_subsystemClass.Reset();
    g_setActiveHotbar = nullptr;
    g_activateItem    = nullptr;
    g_nextLookupMs    = 0;
    g_missLogged      = false;
    g_pageOn          = false;
    g_selectOn        = false;
}
