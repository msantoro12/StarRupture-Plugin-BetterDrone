#include "drone_interact.h"
#include "drone_config.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <atomic>
#include <windows.h>

namespace
{
    // USceneComponent::ComponentToWorld.Translation. ComponentToWorld is not a
    // UProperty so the generated SDK has no name for it; 0x210 is the offset
    // OnInteractableTargetsChanged itself reads when it measures the range
    // between the character and a candidate interactable.
    constexpr size_t k_componentToWorldTranslation = 0x210;

    // ACrPlayerControllerBase::PlayerControlState. Not a UProperty, so the
    // generated SDK leaves it inside Pad_E88 and it has to be reached by
    // offset; OnInteractableTargetsChanged reads it at 0x1475FB0AA and gates on
    // it twice, at 0x1475FB0B9 (== Interacting) and 0x1475FB155 (== Deconstruct).
    constexpr size_t k_pcPlayerControlState = 0xE88;

    // ECrPlayerControlState values, from the game's own UEnum. Only the two the
    // drone path has to reconcile are named here.
    constexpr uint8_t k_controlStateNormal          = 0;
    constexpr uint8_t k_controlStateDeconstructMode = 2;

    // Both the game's own interact mapping (if this build still binds it while
    // the drone is out) and our synthetic press land on the same native
    // functions. Whichever arrives first wins; the other is dropped inside this
    // window. Only applied during drone mode, so a fast double-tap on foot is
    // never eaten.
    constexpr uint64_t k_dedupeWindowMs = 100;

    typedef void (__fastcall* OnInteractableTargetsChanged_t)(void* pc, const void* newTargets);
    typedef void (__fastcall* InteractVoid_t)(void* pc);
    typedef bool (__fastcall* InteractBool_t)(void* pc);

    OnInteractableTargetsChanged_t g_origTargetsChanged    = nullptr;
    InteractVoid_t                 g_origInteractStarted   = nullptr;
    InteractBool_t                 g_origInteractCompleted = nullptr;

    // Resolved during OnPluginLoadHooks; 0 means that pattern missed on this build.
    uintptr_t g_addrTargetsChanged    = 0;
    uintptr_t g_addrInteractStarted   = 0;
    uintptr_t g_addrInteractCompleted = 0;
    uintptr_t g_addrInteract          = 0;

    constexpr const char* kTargetsChangedPattern =
        "40 55 56 41 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC F0 02 00 00";
    constexpr const char* kInteractStartedPattern =
        "40 53 56 57 48 83 EC 30 48 8B F1 C6 81";
    constexpr const char* kInteractCompletedPattern =
        "40 53 48 83 EC 40 80 B9 ?? ?? ?? ?? ?? 48 8B D9 75 ?? B0 01";
    constexpr const char* kInteractPattern =
        "40 55 56 41 56 41 57 48 8B EC 48 83 EC 78";

    HookHandle g_hookTargetsChanged    = nullptr;
    HookHandle g_hookInteractStarted   = nullptr;
    HookHandle g_hookInteractCompleted = nullptr;

    char g_keyName[64] = {};

    // Key events arrive on the modloader's WndProc hook; the native interact
    // calls have to happen on the game thread, so the callback only raises a
    // flag and OnTick does the work.
    std::atomic<bool> g_pendingPress{ false };
    std::atomic<bool> g_pendingRelease{ false };

    std::atomic<uint64_t> g_lastPressMs{ 0 };
    std::atomic<uint64_t> g_lastReleaseMs{ 0 };

    bool g_loggedDuplicate = false;

    // Why the last keypress did not turn into an interact. Compared by pointer
    // -- every caller passes a distinct literal -- so a key held down against a
    // condition that will not change logs once rather than once per frame.
    const char* g_lastGate = nullptr;

    void LogGateOnce(const char* reason)
    {
        if (g_lastGate == reason)
            return;

        g_lastGate = reason;
        LOG_DEBUG("DroneInteract: interact key seen but nothing sent -- %s", reason);
    }

    SDK::ACrCharacterPlayerBase* DroneCharacter(SDK::ACrPlayerControllerBase* pc)
    {
        if (!pc)
            return nullptr;

        SDK::ACrCharacterPlayerBase* character = pc->CrChar;
        if (!character || character->Status != SDK::EPlayerCharacterStatus::BuildingDrone)
            return nullptr;

        // Checked last: this runs from a targeting tick, and the pointer reads
        // above rule out the common case without touching the config at all.
        return DroneConfig::Config::ReadInteractInDroneMode() ? character : nullptr;
    }

    SDK::ACrPlayerControllerBase* LocalController()
    {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
            return nullptr;

        SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
        if (!pc || !pc->IsA(SDK::ACrPlayerControllerBase::StaticClass()))
            return nullptr;

        return static_cast<SDK::ACrPlayerControllerBase*>(pc);
    }

    bool SwallowDuplicate(std::atomic<uint64_t>& last, void* pc)
    {
        if (!DroneCharacter(static_cast<SDK::ACrPlayerControllerBase*>(pc)))
            return false;

        const uint64_t now = GetTickCount64();
        if (now - last.load(std::memory_order_relaxed) < k_dedupeWindowMs)
        {
            if (!g_loggedDuplicate)
            {
                g_loggedDuplicate = true;
                LOG_DEBUG("DroneInteract: dropped a duplicate interact — this build still binds "
                          "the interact key during drone mode");
            }
            return true;
        }

        last.store(now, std::memory_order_relaxed);
        return false;
    }

    void __fastcall Detour_OnInteractableTargetsChanged(void* pc, const void* newTargets)
    {
        SDK::ACrCharacterPlayerBase* character = DroneCharacter(static_cast<SDK::ACrPlayerControllerBase*>(pc));
        SDK::USceneComponent*        root      = character ? character->RootComponent : nullptr;
        SDK::UCameraComponent*       droneCam  = character ? character->DroneCamera   : nullptr;

        if (!root || !droneCam)
        {
            g_origTargetsChanged(pc, newTargets);
            return;
        }

        // The original bails out on IsBuildingDroneActive, and ranges every
        // candidate against the character's root component. Hand it a character
        // that is not in drone mode and is standing at the drone's camera, then
        // put both back before anything else can observe them.
        auto*       rootTranslation = reinterpret_cast<double*>(
            reinterpret_cast<uint8_t*>(root) + k_componentToWorldTranslation);
        const auto* camTranslation  = reinterpret_cast<const double*>(
            reinterpret_cast<const uint8_t*>(droneCam) + k_componentToWorldTranslation);

        // A second gate sits ahead of the drone one: DeconstructMode jumps
        // straight to ResetInteractableActor. The drone is summoned from the
        // building tool -- CanActivateDrone returns true outright when the
        // controller is already in DeconstructMode -- so that state is simply
        // what the drone is flown in, and the gate is closed the whole time the
        // drone is out with delete mode selected. Only that one value is
        // rewritten: BuildingMode already passes, and Interacting means an
        // interaction is genuinely in flight.
        //
        // This one is a deliberate override rather than a fix. The gate is on
        // the controller, not the drone, so the player on foot in delete mode is
        // blocked in exactly the same way; suppressing it lets the drone open a
        // building's UI without first switching back to construction mode.
        // Nothing collides: NativeOnInputInteractStarted has no deconstruct
        // branch, and opening a building UI is an instant interaction, so it
        // never sets PlayerControlState to Interacting and never drops the
        // player out of delete mode.
        auto*         controlState      = reinterpret_cast<uint8_t*>(pc) + k_pcPlayerControlState;
        const uint8_t savedControlState = *controlState;

        const SDK::EPlayerCharacterStatus savedStatus = character->Status;
        const double savedTranslation[3] = { rootTranslation[0], rootTranslation[1], rootTranslation[2] };

        character->Status  = SDK::EPlayerCharacterStatus::None;
        rootTranslation[0] = camTranslation[0];
        rootTranslation[1] = camTranslation[1];
        rootTranslation[2] = camTranslation[2];

        if (savedControlState == k_controlStateDeconstructMode)
            *controlState = k_controlStateNormal;

        g_origTargetsChanged(pc, newTargets);

        *controlState      = savedControlState;
        rootTranslation[0] = savedTranslation[0];
        rootTranslation[1] = savedTranslation[1];
        rootTranslation[2] = savedTranslation[2];
        character->Status  = savedStatus;
    }

    void __fastcall Detour_NativeOnInputInteractStarted(void* pc)
    {
        if (SwallowDuplicate(g_lastPressMs, pc))
            return;

        g_origInteractStarted(pc);
    }

    bool __fastcall Detour_NativeOnInputInteractCompleted(void* pc)
    {
        // Reporting the release as handled is what stops the caller falling
        // through to NativeOnInputInteract, so a swallowed release drops the
        // whole pair rather than half of it.
        if (SwallowDuplicate(g_lastReleaseMs, pc))
            return true;

        return g_origInteractCompleted(pc);
    }

    // Mirrors UCrInputNativeInteract: press starts a held interaction, release
    // finishes it, and an interaction that never started falls through to the
    // instant one.
    void DispatchInteract(SDK::ACrPlayerControllerBase* pc, bool press, bool release)
    {
        if (press)
            reinterpret_cast<InteractVoid_t>(g_addrInteractStarted)(pc);

        if (release && !reinterpret_cast<InteractBool_t>(g_addrInteractCompleted)(pc))
            reinterpret_cast<InteractVoid_t>(g_addrInteract)(pc);
    }

    // Spelled out rather than reusing DroneCharacter() so a keypress that goes
    // nowhere says which condition stopped it. Every one of these is a state the
    // player can be in legitimately, so silence here is indistinguishable from
    // the key never arriving at all -- which is the report this has to answer.
    void OnTick(float)
    {
        const bool press   = g_pendingPress.exchange(false);
        const bool release = g_pendingRelease.exchange(false);
        if (!press && !release)
            return;

        SDK::ACrPlayerControllerBase* pc = LocalController();
        if (!pc)
        {
            LogGateOnce("no local ACrPlayerControllerBase");
            return;
        }

        SDK::ACrCharacterPlayerBase* character = pc->CrChar;
        if (!character)
        {
            LogGateOnce("player controller has no CrChar");
            return;
        }

        if (character->Status != SDK::EPlayerCharacterStatus::BuildingDrone)
        {
            LogGateOnce("character is not in drone mode");
            return;
        }

        if (!DroneConfig::Config::ReadInteractInDroneMode())
        {
            LogGateOnce("'Interact In Drone Mode' is disabled in the config");
            return;
        }

        if (character->bDead)
        {
            LogGateOnce("character is dead");
            return;
        }

        // NativeOnInputInteractCompleted dereferences the pawn as a player
        // character without checking it, so refuse to call in if it is not one.
        if (pc->Pawn != static_cast<SDK::APawn*>(character))
        {
            LogGateOnce("the possessed pawn is not the player character");
            return;
        }

        g_lastGate = nullptr;
        DispatchInteract(pc, press, release);
    }

    void OnInteractKey(EModKey, EModKeyEvent event)
    {
        // The only place that can tell "the modloader never dispatched the key"
        // apart from "it arrived and a gate in OnTick rejected it".
        LOG_DEBUG("DroneInteract: interact key %s", event == EModKeyEvent::Pressed ? "pressed" : "released");

        if (event == EModKeyEvent::Pressed)
            g_pendingPress.store(true, std::memory_order_relaxed);
        else
            g_pendingRelease.store(true, std::memory_order_relaxed);
    }

    bool InstallHooks()
    {
        auto* hooks = GetSelf()->hooks->Hooks;

        const uintptr_t targetsChanged = g_addrTargetsChanged;

        if (!targetsChanged || !g_addrInteractStarted || !g_addrInteractCompleted || !g_addrInteract)
        {
            LOG_WARN("DroneInteract: unresolved addresses — targetsChanged=0x%llX started=0x%llX "
                     "completed=0x%llX interact=0x%llX",
                     targetsChanged, g_addrInteractStarted, g_addrInteractCompleted, g_addrInteract);
            return false;
        }

        LOG_INFO("DroneInteract: OnInteractableTargetsChanged at 0x%llX, NativeOnInputInteract at 0x%llX",
                 targetsChanged, g_addrInteract);

        g_hookTargetsChanged = hooks->Install(
            targetsChanged,
            reinterpret_cast<void*>(&Detour_OnInteractableTargetsChanged),
            reinterpret_cast<void**>(&g_origTargetsChanged));

        g_hookInteractStarted = hooks->Install(
            g_addrInteractStarted,
            reinterpret_cast<void*>(&Detour_NativeOnInputInteractStarted),
            reinterpret_cast<void**>(&g_origInteractStarted));

        g_hookInteractCompleted = hooks->Install(
            g_addrInteractCompleted,
            reinterpret_cast<void*>(&Detour_NativeOnInputInteractCompleted),
            reinterpret_cast<void**>(&g_origInteractCompleted));

        if (!g_hookTargetsChanged || !g_hookInteractStarted || !g_hookInteractCompleted)
        {
            LOG_WARN("DroneInteract: hook installation failed");
            return false;
        }

        return true;
    }

    void RemoveHooks()
    {
        auto* hooks = GetSelf()->hooks->Hooks;

        if (g_hookInteractCompleted)
        {
            hooks->Remove(g_hookInteractCompleted);
            g_hookInteractCompleted = nullptr;
            g_origInteractCompleted = nullptr;
        }

        if (g_hookInteractStarted)
        {
            hooks->Remove(g_hookInteractStarted);
            g_hookInteractStarted = nullptr;
            g_origInteractStarted = nullptr;
        }

        if (g_hookTargetsChanged)
        {
            hooks->Remove(g_hookTargetsChanged);
            g_hookTargetsChanged = nullptr;
            g_origTargetsChanged = nullptr;
        }
    }
}

void ResolveDroneInteract(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    // Optional throughout: a miss leaves the rest of BetterDrone working, which
    // is what the old scan-at-init path did. The loader still lists each miss.
    g_addrTargetsChanged = scanner->ResolveOptional(
        self, "ACrPlayerControllerBase::OnInteractableTargetsChanged", kTargetsChangedPattern);
    g_addrInteractStarted = scanner->ResolveOptional(
        self, "ACrPlayerControllerBase::NativeOnInputInteractStarted", kInteractStartedPattern);
    g_addrInteractCompleted = scanner->ResolveOptional(
        self, "ACrPlayerControllerBase::NativeOnInputInteractCompleted", kInteractCompletedPattern);
    g_addrInteract = scanner->ResolveOptional(
        self, "ACrPlayerControllerBase::NativeOnInputInteract", kInteractPattern);
}

bool InitDroneInteract()
{
    if (!InstallHooks())
    {
        RemoveHooks();
        return false;
    }

    GetSelf()->hooks->Engine->RegisterOnTick(OnTick);

    auto* input = GetSelf()->hooks->Input;
    if (!input)
    {
        LOG_WARN("DroneInteract: no input dispatch — the interact key will not reach drone mode");
        return true;
    }

    DroneConfig::Config::ReadInteractKey(g_keyName, sizeof(g_keyName));

    input->RegisterKeybindByName(g_keyName, EModKeyEvent::Pressed,  &OnInteractKey);
    input->RegisterKeybindByName(g_keyName, EModKeyEvent::Released, &OnInteractKey);

    LOG_INFO("DroneInteract: building interaction enabled in drone mode on '%s'", g_keyName);
    return true;
}

bool IsLocalPlayerInDrone()
{
    try
    {
        SDK::ACrPlayerControllerBase* pc = LocalController();
        SDK::ACrCharacterPlayerBase* character = pc ? pc->CrChar : nullptr;
        return character && character->Status == SDK::EPlayerCharacterStatus::BuildingDrone;
    }
    catch (...)
    {
        return false;
    }
}

void ShutdownDroneInteract()
{
    auto* input = GetSelf()->hooks->Input;
    if (input && g_keyName[0])
    {
        input->UnregisterKeybindByName(g_keyName, EModKeyEvent::Pressed,  &OnInteractKey);
        input->UnregisterKeybindByName(g_keyName, EModKeyEvent::Released, &OnInteractKey);
    }

    GetSelf()->hooks->Engine->UnregisterOnTick(OnTick);

    RemoveHooks();

    g_pendingPress.store(false, std::memory_order_relaxed);
    g_pendingRelease.store(false, std::memory_order_relaxed);

    LOG_DEBUG("DroneInteract: hooks removed");
}
