#include "drone_interact.h"
#include "drone_config.h"
#include "drone_fields.h"
#include "drone_key_vk.h"
#include "drone_sprint_key.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <Chimera_classes.hpp>
#include <ChimeraUI_classes.hpp>
#include <CommonGame_classes.hpp>
#include <EnhancedInput_classes.hpp>
#include <EnhancedInput_structs.hpp>
#include <Engine_classes.hpp>
#include <GameplayAbilities_classes.hpp>
#include <atomic>
#include <optional>
#include <windows.h>

namespace
{
    // ACrPlayerControllerBase::PlayerControlState (DroneFields). Not a UProperty,
    // so the generated SDK leaves it in padding; OnInteractableTargetsChanged
    // reads it at 0x1475FB0AA and gates on it twice, at 0x1475FB0B9
    // (== Interacting) and 0x1475FB155 (== Deconstruct).

    // ECrPlayerControlState values, read by name from the game's own UEnum: the
    // numbers can move in an update, the names are what the game saves. Only the
    // two the drone path has to reconcile are looked up.
    uint8_t g_controlStateNormal          = 0;
    uint8_t g_controlStateDeconstructMode = 0;

    // Looked up once, on the game thread, the first time the drone path needs
    // them. False when the enum or an entry is not on this build: the drone
    // interaction is then left off.
    bool ControlStatesResolved()
    {
        static bool tried    = false;
        static bool resolved = false;
        if (tried)
            return resolved;

        tried = true;
        int64_t normal = 0, deconstruct = 0;
        const PluginMap::EnumLookup n = PluginMap::EnumValue("ECrPlayerControlState", "Normal", normal);
        const PluginMap::EnumLookup d = PluginMap::EnumValue("ECrPlayerControlState", "DeconstructMode", deconstruct);
        if (n != PluginMap::EnumLookup::Found || d != PluginMap::EnumLookup::Found
            || normal < 0 || normal > UINT8_MAX || deconstruct < 0 || deconstruct > UINT8_MAX)
        {
            LOG_WARN("DroneInteract: ECrPlayerControlState Normal/DeconstructMode not found on this build "
                     "-- building interaction in drone mode is off");
            return false;
        }

        g_controlStateNormal          = static_cast<uint8_t>(normal);
        g_controlStateDeconstructMode = static_cast<uint8_t>(deconstruct);
        resolved                      = true;
        LOG_INFO("DroneInteract: ECrPlayerControlState Normal=%d DeconstructMode=%d",
                 static_cast<int>(normal), static_cast<int>(deconstruct));
        return true;
    }

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

    // The context interaction (the game's "hold to take all"): Started picks
    // what the target offers, Context runs it, Completed drops it. Each takes
    // only the controller. Called by this plugin, never hooked. Resolved during
    // OnPluginLoadHooks; null means the key is missing on this build, which
    // turns off only the hold (DroneFields::Features::HoldTakeAll).
    InteractVoid_t g_contextStarted   = nullptr;
    InteractVoid_t g_context          = nullptr;
    InteractVoid_t g_contextCompleted = nullptr;

    constexpr const char* kContextStartedPattern =
        "40 55 56 41 56 41 57 48 8B EC 48 83 EC 68 45 33 FF";
    constexpr const char* kContextPattern =
        "40 57 48 81 EC 90 00 00 00 80 3D ?? ?? ?? ?? 00 48 8B F9 48 8B 81 ?? ?? ?? ??";
    // The controller member it clears (0xD90) is wildcarded to its low byte:
    // with the whole offset open the pattern also matches seven other functions.
    constexpr const char* kContextCompletedPattern =
        "48 83 EC 38 48 81 C1 ?? 0D 00 00 48 8D 44 24 20 0F 57 C0 F3 0F 7F 44 24 20 48 3B C8 74 ?? 48 C7 01";

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

    // The controller's character while it is flying the drone, else null.
    SDK::ACrCharacterPlayerBase* PilotingCharacter(SDK::ACrPlayerControllerBase* pc)
    {
        SDK::ACrCharacterPlayerBase* character = DroneFields::ValueOr(DroneFields::CrChar, pc);
        if (!character || !DroneFields::InDrone(character))
            return nullptr;

        return character;
    }

    SDK::ACrCharacterPlayerBase* DroneCharacter(SDK::ACrPlayerControllerBase* pc)
    {
        SDK::ACrCharacterPlayerBase* character = PilotingCharacter(pc);
        if (!character)
            return nullptr;

        // Checked last: this runs from a targeting tick, and the pointer reads
        // above rule out the common case without touching the config at all.
        return DroneConfig::Config::ReadInteractInDroneMode() ? character : nullptr;
    }

    SDK::ACrPlayerControllerBase* LocalController()
    {
        SDK::UWorld* world = PluginSdkRuntime::GetWorld();
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

    // A scene component's cached world translation (X, Y, Z), or null when the
    // map has no row for this build.
    double* WorldTranslation(SDK::USceneComponent* component)
    {
        SDK::FTransform* toWorld = DroneFields::ComponentToWorld.In(component);
        return reinterpret_cast<double*>(DroneFields::TransformTranslation.In(toWorld));
    }

    const double* WorldTranslation(const SDK::USceneComponent* component)
    {
        const SDK::FTransform* toWorld = DroneFields::ComponentToWorld.In(component);
        return reinterpret_cast<const double*>(DroneFields::TransformTranslation.In(toWorld));
    }

    // Stands the character's root component at the drone's camera for as long
    // as it is in scope, then puts it back. Only the cached world translation
    // is rewritten, and it is restored before the scope ends, so only the
    // native calls made inside the scope see the move.
    //
    // USceneComponent::ComponentToWorld.Translation is not a UProperty, so the
    // generated SDK has no name for it: it comes from the loader's map
    // (DroneFields::ComponentToWorld and TransformTranslation), the same place
    // OnInteractableTargetsChanged reads it from when it measures the range
    // between the character and a candidate interactable. A build the map does
    // not cover leaves the drone interaction off, so nothing is moved.
    class BodyAtDroneCamera
    {
    public:
        BodyAtDroneCamera(SDK::USceneComponent* root, const SDK::UCameraComponent* droneCam)
            : m_translation(WorldTranslation(root))
        {
            const double* camTranslation = WorldTranslation(droneCam);
            if (!m_translation || !camTranslation)
            {
                m_translation = nullptr;
                return;
            }

            for (int i = 0; i < 3; ++i)
            {
                m_saved[i]       = m_translation[i];
                m_translation[i] = camTranslation[i];
            }
        }

        ~BodyAtDroneCamera()
        {
            if (!m_translation)
                return;

            for (int i = 0; i < 3; ++i)
                m_translation[i] = m_saved[i];
        }

        BodyAtDroneCamera(const BodyAtDroneCamera&)            = delete;
        BodyAtDroneCamera& operator=(const BodyAtDroneCamera&) = delete;

    private:
        double* m_translation;
        double  m_saved[3] = {};
    };

    void __fastcall Detour_OnInteractableTargetsChanged(void* pc, const void* newTargets)
    {
        SDK::ACrCharacterPlayerBase* character = DroneCharacter(static_cast<SDK::ACrPlayerControllerBase*>(pc));
        SDK::USceneComponent*        root      = character ? character->RootComponent : nullptr;
        SDK::UCameraComponent*       droneCam  = DroneFields::ValueOr(DroneFields::DroneCamera, character);

        if (!root || !droneCam)
        {
            g_origTargetsChanged(pc, newTargets);
            return;
        }

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
        uint8_t* controlState = DroneFields::PlayerControlState.In(pc);
        auto*    statusAt     = DroneFields::Status.In(character);
        if (!controlState || !statusAt || !ControlStatesResolved())
        {
            g_origTargetsChanged(pc, newTargets);
            return;
        }
        const uint8_t savedControlState = *controlState;

        // The original bails out on IsBuildingDroneActive, and ranges every
        // candidate against the character's root component. Hand it a character
        // that is not in drone mode and is standing at the drone's camera, then
        // put both back before anything else can observe them.
        const SDK::EPlayerCharacterStatus savedStatus = *statusAt;
        *statusAt = SDK::EPlayerCharacterStatus::None;

        if (savedControlState == g_controlStateDeconstructMode)
            *controlState = g_controlStateNormal;

        {
            const BodyAtDroneCamera atDrone(root, droneCam);
            g_origTargetsChanged(pc, newTargets);
        }

        *controlState     = savedControlState;
        *statusAt         = savedStatus;
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

    // A gatherable (wild plants, fruit, eggs) ranges itself against the
    // possessed pawn, which in drone mode is the parked body:
    // ACrGatherableBaseActor::CanInteract refuses one more than
    // GatherableCropSettings' GatherableInteractionDistanceThresholdInCm away.
    // Targeting picks it with the body at the drone's camera, but the press and
    // release handlers ask CanInteract again, so they need the body there too.
    // Crops and every other target keep the path they had.
    bool IsGatherableTarget(SDK::ACrPlayerControllerBase* pc)
    {
        SDK::AActor* target = DroneFields::ValueOr(DroneFields::CurrentInteractableActor, pc);
        return target && target->IsA(SDK::ACrGatherableBaseActor::StaticClass());
    }

    // Game thread only. Keeps the per-press target line below to one a second.
    uint64_t g_lastTargetLogMs = 0;

    // What a press in the drone reached, so a target that does not respond can
    // be told apart from one that was never selected.
    void LogPressTarget(SDK::ACrPlayerControllerBase* pc, bool movedBody)
    {
        const uint64_t now = GetTickCount64();
        if (now - g_lastTargetLogMs < 1000)
            return;

        g_lastTargetLogMs = now;

        SDK::AActor* target = DroneFields::ValueOr(DroneFields::CurrentInteractableActor, pc);
        LOG_INFO("DroneInteract: interact on %s%s",
                 target ? target->Class->GetName().c_str() : "no target",
                 movedBody ? ", ranged from the drone" : "");
    }

    // The drone camera to stand the body at while a press or hold reaches the
    // current target, or null when the target is ranged as it is. Only where
    // the gather is granted: a client also asks the server, which checks the
    // range again against the body it holds, refuses, and leaves the plant
    // marked gathered on the client alone.
    SDK::UCameraComponent* RangingCamera(SDK::ACrPlayerControllerBase* pc, SDK::ACrCharacterPlayerBase* character)
    {
        SDK::UCameraComponent* droneCamera = DroneFields::ValueOr(DroneFields::DroneCamera, character);
        if (character->RootComponent && droneCamera && IsGatherableTarget(pc) &&
            SDK::UKismetSystemLibrary::IsServer(character))
            return droneCamera;

        return nullptr;
    }

    // Set when the hold ran the context interaction on a logistics drone on the
    // current press, so the release that ends the same press does not also take
    // the one item the plain interact would. A hold on anything else leaves the
    // release alone. Cleared by the next plain press. Game thread only.
    bool g_holdRan = false;

    // Mirrors UCrInputNativeInteract: press starts a held interaction, release
    // finishes it, and an interaction that never started falls through to the
    // instant one.
    void DispatchInteract(SDK::ACrPlayerControllerBase* pc, SDK::ACrCharacterPlayerBase* character,
                          bool press, bool release)
    {
        std::optional<BodyAtDroneCamera> atDrone;
        if (SDK::UCameraComponent* camera = RangingCamera(pc, character))
            atDrone.emplace(character->RootComponent, camera);

        if (press)
        {
            g_holdRan = false;
            LogPressTarget(pc, atDrone.has_value());
            reinterpret_cast<InteractVoid_t>(g_addrInteractStarted)(pc);
        }

        if (release)
        {
            // Completed is still called after a hold: it winds the press back.
            const bool handled = reinterpret_cast<InteractBool_t>(g_addrInteractCompleted)(pc);
            if (!handled && !g_holdRan)
                reinterpret_cast<InteractVoid_t>(g_addrInteract)(pc);
            g_holdRan = false;
        }
    }

    // Why an interact cannot go to this controller's drone right now, or null
    // when it can, with the character it would act for. Spelled out rather
    // than reusing DroneCharacter() so a keypress that goes nowhere says which
    // condition stopped it. Every one of these is a state the player can be in
    // legitimately, so silence is indistinguishable from the key never
    // arriving at all -- which is the report this has to answer.
    const char* InteractGate(SDK::ACrPlayerControllerBase* pc, SDK::ACrCharacterPlayerBase*& character)
    {
        if (!pc)
            return "no local ACrPlayerControllerBase";

        character = DroneFields::ValueOr(DroneFields::CrChar, pc);
        if (!character)
            return "player controller has no CrChar";

        if (!DroneFields::InDrone(character))
            return "character is not in drone mode";

        if (!DroneConfig::Config::ReadInteractInDroneMode())
            return "'Interact In Drone Mode' is disabled in the config";

        if (DroneFields::Dead(character))
            return "character is dead";

        // NativeOnInputInteractCompleted dereferences the pawn as a player
        // character without checking it, so refuse to call in if it is not one.
        if (pc->Pawn != static_cast<SDK::APawn*>(character))
            return "the possessed pawn is not the player character";

        return nullptr;
    }

    // ---- hold to take all ---------------------------------------------------

    // The game's context interaction (hold E on a building: take everything)
    // is a Hold trigger on UCrInputNativeInteractContext's action, bound by the
    // game's input component to the controller. The drone is not what that
    // component is bound for, and no Enhanced Input event reaches it, so the
    // key is polled here, as the boost and Reload keys are, and the three
    // controller functions the game binds are called directly.
    FollowedKey g_holdKey;
    bool        g_holdInDrone = false;   // last tick
    bool        g_holdDown    = false;   // last tick
    bool        g_holdArmed   = false;   // pressed in the drone, not yet run
    uint64_t    g_holdDownMs  = 0;
    float       g_holdSeconds = 0.0f;
    bool        g_holdTimeRead = false;  // g_holdSeconds came from the game's mapping
    bool        g_holdOn      = false;

    // What the crosshair was on when the key went down. The hold runs on that
    // and on nothing else.
    ObjectRef<SDK::AActor> g_holdTarget;

    // True while a loader or plugin window has the keyboard, so typing in it
    // does not arm the hold.
    bool OverlayOpen()
    {
        auto* ui = GetSelf()->hooks->UI;
        return ui && ui->IsOverlayOpen && ui->IsOverlayOpen();
    }

    // The hold time to wait for: the game's own Hold trigger on that action,
    // else Unreal's default for the trigger. Logged with where it came from.
    // Once it has come from the game's mapping it does not change, so it is
    // not looked up again.
    void ResolveHoldSeconds()
    {
        float seconds = 0.0f;
        const char* source = "the game's input mapping";
        const bool game = ResolveInputHandlerHoldSeconds(SDK::UCrInputNativeInteractContext::StaticClass(), &seconds);
        if (!game)
        {
            const SDK::UInputTriggerHold* defaults = SDK::UInputTriggerHold::GetDefaultObj();
            seconds = defaults ? defaults->HoldTimeThreshold : 0.0f;
            source  = "the Hold trigger's engine default, the game's mapping was not reachable";
            if (!(seconds > 0.0f))
            {
                seconds = 1.0f;
                source  = "1 second, nothing readable";
            }
        }

        if (seconds != g_holdSeconds)
            LOG_INFO("DroneInteract: hold to take all after %.2f s, from %s", seconds, source);
        g_holdSeconds  = seconds;
        g_holdTimeRead = game;
    }

    // Runs the context interaction on the target the key went down on, if the
    // crosshair is still on it: Started picks what the target offers and stores
    // it on the controller, Context runs it, and Completed drops it again, all
    // in this call so nothing stays stored past the tick. Game thread only (the
    // tick).
    void RunHold(SDK::ACrPlayerControllerBase* pc, SDK::AActor* pressTarget)
    {
        SDK::ACrCharacterPlayerBase* character = nullptr;
        if (const char* gate = InteractGate(pc, character))
        {
            LOG_DEBUG("DroneInteract: hold finished but nothing sent -- %s", gate);
            return;
        }

        SDK::AActor* target = DroneFields::ValueOr(DroneFields::CurrentInteractableActor, pc);
        if (!target || target != pressTarget)
        {
            LOG_DEBUG("DroneInteract: hold finished but nothing sent -- the target changed");
            return;
        }

        std::optional<BodyAtDroneCamera> atDrone;
        if (SDK::UCameraComponent* camera = RangingCamera(pc, character))
            atDrone.emplace(character->RootComponent, camera);

        LOG_INFO("DroneInteract: hold to take all on %s%s", target->Class->GetName().c_str(),
                 atDrone.has_value() ? ", ranged from the drone" : "");
        const bool onDrone = target->IsA(SDK::ACrDroneBase::StaticClass());
        g_contextStarted(pc);
        g_context(pc);
        g_contextCompleted(pc);
        g_holdRan = onDrone;
    }

    // The context key goes down in the drone with no menu open, and stays down
    // for the hold time: run the context interaction once. The key is looked up
    // again on each entry to the drone, which picks up a rebind, and on the
    // retry timer while it is not settled; E stands in, logged once. The lookup
    // starts over when the drone ends: the context action is bound on foot, so
    // that is where a rebind shows, and the drone keeps the key found there. A
    // short press never gets here, so it stays the plain interact.
    void HoldTick(SDK::ACrPlayerControllerBase* pc)
    {
        const bool inDrone = PilotingCharacter(pc) != nullptr;
        if (!inDrone && g_holdInDrone)
            g_holdKey.Restart();

        if ((inDrone && !g_holdInDrone) || g_holdKey.Due())
        {
            char keyName[64] = {};
            const FollowedKey::Result result = g_holdKey.Resolve(
                SDK::UCrInputNativeInteractContext::StaticClass(), inDrone, 'E', keyName, sizeof(keyName));
            if (result.followed)
                LOG_INFO("DroneInteract: hold to take all on the game's context interact key (%s)", keyName);
            else if (result.firstFallback)
                LOG_WARN("DroneInteract: could not resolve the game's context interact key (%s), using E", result.why);
            if (inDrone && !g_holdInDrone && !g_holdTimeRead)
                ResolveHoldSeconds();
        }
        g_holdInDrone = inDrone;

        const bool down    = g_holdKey.vk != 0 && (GetAsyncKeyState(g_holdKey.vk) & 0x8000) != 0 && GameHasFocus() &&
                             !OverlayOpen();
        const bool pressed = down && !g_holdDown;
        g_holdDown         = down;

        if (!down)
        {
            g_holdArmed = false;
            g_holdTarget.Reset();
            return;
        }

        const uint64_t now = GetTickCount64();
        if (pressed)
        {
            SDK::AActor* target = inDrone ? DroneFields::ValueOr(DroneFields::CurrentInteractableActor, pc) : nullptr;
            g_holdArmed  = target && !IsGameMenuOpen();
            g_holdDownMs = now;
            g_holdTarget.Set(g_holdArmed ? target : nullptr);
        }

        if (!g_holdArmed || !inDrone || g_holdSeconds <= 0.0f ||
            now - g_holdDownMs < static_cast<uint64_t>(g_holdSeconds * 1000.0f))
            return;

        g_holdArmed = false;
        SDK::AActor* pressTarget = g_holdTarget.Get();
        g_holdTarget.Reset();
        if (pressTarget && !IsGameMenuOpen())
            RunHold(pc, pressTarget);
    }

    void OnTick(float)
    {
        if (g_holdOn)
            HoldTick(LocalController());

        const bool press   = g_pendingPress.exchange(false);
        const bool release = g_pendingRelease.exchange(false);
        if (!press && !release)
            return;

        SDK::ACrPlayerControllerBase* pc        = LocalController();
        SDK::ACrCharacterPlayerBase*  character = nullptr;
        if (const char* gate = InteractGate(pc, character))
        {
            LogGateOnce(gate);
            return;
        }

        g_lastGate = nullptr;
        DispatchInteract(pc, character, press, release);
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

    // ACrPlayerControllerBase's handler for the Map input action; the name
    // follows NativeOnInputInteract's. UCrInputNativeMapMenu's triggered
    // override calls it with the pawn's controller and the action value.
    // It never looks at the drone: it re-shows the HUD, stops only if the
    // map is not unlocked yet, broadcasts the event ACrHUD opens the map
    // screen from, and sends ServerOnInputMapMenu.
    typedef void (__fastcall* NativeOnInputMapMenu_t)(void* pc, const SDK::FInputActionValue* value);

    // Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
    uintptr_t g_addrMapMenu = 0;

    constexpr const char* kMapMenuPattern =
        "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? "
        "48 85 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? F6 80 ?? ?? ?? ?? 02";

    // FInputActionValue is opaque in the SDK. Its layout is an FVector and
    // then EInputActionValueType, and the handler only reads the vector's
    // length. This is the value a key press delivers.
    struct PressedActionValue
    {
        double                     value[3] = { 1.0, 0.0, 0.0 };
        SDK::EInputActionValueType type     = SDK::EInputActionValueType::Boolean;
    };
    static_assert(sizeof(PressedActionValue) == sizeof(SDK::FInputActionValue),
                  "FInputActionValue layout changed");

    char g_mapKeyName[64] = {};

    // Raised by the keybind callback, consumed on the game thread in OnMapTick.
    std::atomic<bool> g_pendingMapPress{ false };

    // Whether a menu was open as of the previous tick. Menu input is
    // handled before the world ticks, so when the map key also closes the
    // map, OnMapTick sees that press only after the map is already gone.
    // Judged on the current state alone, it would open the map straight
    // back up.
    std::atomic<bool> g_menuWasOpen{ false };

    // The game's UI state as the loader last reported it. The loader sets the
    // menu bits when a widget lands on the menu layer and clears them when the
    // widget is fully gone, close animation included, which is the whole time
    // a menu counts as open here. The callback runs on the game thread, the
    // loader's tick runs before this plugin's, and any thread may read it.
    constexpr uint64_t kMenuStateMask = PLUGIN_UI_ANY_MENU | PLUGIN_UI_MAP;
    std::atomic<uint64_t> g_uiState{ 0 };

    void OnUIState(uint64_t, uint64_t newState, void*)
    {
        g_uiState.store(newState, std::memory_order_relaxed);
    }

    // The layer UCrUIManagerSubsystem opens the map, the inventory and
    // every building window on, and the one it opens the death screen on.
    // The HUD layout pushes its escape menu there too. The HUD itself sits
    // on another layer, so an empty stack here means no menu is up.
    // Walked only to get at a widget that is about to be acted on.
    constexpr const char* kMenuLayerTag = "UI.Layer.Menu";

    // The local player's menu layer, or null. Valid for the current tick only.
    SDK::UCommonActivatableWidgetContainerBase* FindMenuLayer(SDK::ACrPlayerControllerBase* pc)
    {
        auto* ui = static_cast<SDK::UGameUIManagerSubsystem*>(
            SDK::USubsystemBlueprintLibrary::GetGameInstanceSubsystem(pc, SDK::UCrUIManagerSubsystem::StaticClass()));
        SDK::UGameUIPolicy* policy = ui ? ui->CurrentPolicy : nullptr;
        if (!policy)
            return nullptr;

        for (int32_t i = 0; i < policy->RootViewportLayouts.Num(); ++i)
        {
            const SDK::FRootViewportLayoutInfo& info = policy->RootViewportLayouts[i];
            if (!info.RootLayout || info.LocalPlayer != pc->Player)
                continue;

            // Not const: the SDK's const TMap indexer does not compile.
            auto& layers = info.RootLayout->Layers;
            for (int32_t l = 0; l < layers.NumAllocated(); ++l)
            {
                if (!layers.IsValidIndex(l))
                    continue;

                SDK::UCommonActivatableWidgetContainerBase* stack = layers[l].Value();
                if (stack && layers[l].Key().TagName.ToString() == kMenuLayerTag)
                    return stack;
            }
        }

        return nullptr;
    }

    // The menu of type T, if it is the active widget on the menu layer and
    // not already closing. DisplayedWidget is what the stack's
    // GetActiveWidget returns.
    template <class T>
    T* ActiveMenu(SDK::UCommonActivatableWidgetContainerBase* layer)
    {
        SDK::UCommonActivatableWidget* top = layer->DisplayedWidget;
        if (!top || !top->bIsActive || !top->IsA(T::StaticClass()))
            return nullptr;

        return static_cast<T*>(top);
    }

    SDK::UCrUW_MapMenu* ActiveMap(SDK::UCommonActivatableWidgetContainerBase* layer)
    {
        return ActiveMenu<SDK::UCrUW_MapMenu>(layer);
    }

    // Idle until the map key is pressed, then tracks the menu layer only
    // while something is on it. Everything is looked up fresh each tick:
    // the controller and the widgets can go away with the drone, travel or
    // GC.
    void OnMapTick(float)
    {
        const bool press = g_pendingMapPress.exchange(false);
        if (!press && !g_menuWasOpen.load(std::memory_order_relaxed))
            return;

        SDK::ACrPlayerControllerBase* pc = LocalController();

        // On foot the game opens the map itself, so this stays out of it.
        const bool inDrone = PilotingCharacter(pc) != nullptr;

        // The map handler never checks what is already on screen, so it
        // would push the map on top of any other menu. A menu counts as open
        // until it has finished closing.
        const bool open    = inDrone && IsGameMenuOpen();
        const bool wasOpen = g_menuWasOpen.exchange(open);

        // Read only on a press, never per tick: it is an INI read.
        if (!press || !inDrone || !DroneConfig::Config::ReadMapInDroneMode())
            return;

        if (open)
        {
            // In drone mode the game's map toggle never reaches the map, so
            // the key would only close it on foot. Close it the way its own
            // close button and back action do: both end in
            // UCrUIManagerSubsystem::CloseMainWidgetByClass, which also puts
            // the input config back to the game. Any other menu is left alone.
            SDK::UCommonActivatableWidgetContainerBase* layer = FindMenuLayer(pc);
            if (SDK::UCrUW_MapMenu* map = layer ? ActiveMap(layer) : nullptr)
                map->HandleOnExitClicked();
            return;
        }

        if (wasOpen)
            return;

        const PressedActionValue value;
        reinterpret_cast<NativeOnInputMapMenu_t>(g_addrMapMenu)(
            pc, reinterpret_cast<const SDK::FInputActionValue*>(&value));

        // Settled against the real state from the next tick on.
        g_menuWasOpen.store(true, std::memory_order_relaxed);
    }

    void OnMapKey(EModKey, EModKeyEvent)
    {
        g_pendingMapPress.store(true, std::memory_order_relaxed);
    }

    // UCrMapManuSubsystem::UncoverFogOfWar(PlayerController, Location, Radius).
    // Public but not a UFunction. It reveals the map around Location for the
    // player character the controller is possessing: it gives up unless that
    // pawn is an ACrCharacterPlayerBase, clears the character's
    // UCrPlayerMapMenuDataComponent fog cells within Radius, and redraws the
    // map texture only when that character is locally controlled. The
    // subsystem's own tick calls it from RecordPlayerLocation with the body's
    // position. The location arrives by pointer, as the x64 ABI passes a
    // 12-byte struct, and only its X and Y are read.
    typedef void (__fastcall* UncoverFogOfWar_t)(void* subsystem, const void* pc,
                                                 const SDK::FVector3f* location, float radius);

    // Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
    uintptr_t g_addrUncoverFog = 0;

    // Runs on through the first single-precision subtractions on the location
    // (subss), so a build that switched the parameter to a double FVector
    // misses here instead of being called with the wrong layout.
    constexpr const char* kUncoverFogPattern =
        "48 89 5C 24 ?? 48 89 6C 24 ?? 56 48 83 EC ?? F2 41 0F 10 00 48 8B F2 "
        "0F 29 74 24 ?? 48 8B E9 F2 0F 11 44 24 ?? 0F 28 F3 E8 ?? ?? ?? ?? "
        "48 8B D8 48 83 B8 ?? ?? ?? ?? 00 75 ?? 48 8B C8 E8 ?? ?? ?? ?? "
        "48 8B 83 ?? ?? ?? ?? 48 89 7C 24 ?? 48 85 C0 75 ?? 48 8B 3D ?? ?? ?? ?? "
        "EB ?? 66 0F 6E 50 ?? F3 0F 10 44 24 ?? F3 0F 10 4C 24 ?? "
        "F3 0F 5C 40 ?? F3 0F 5C 48";

    // Status.InForgottenEngine, the tag the game's own footstep reveal checks
    // (IsPlayerInForgottenEngine) before uncovering anything. Built on the
    // game thread the first time it is needed; an FName stays valid.
    SDK::FGameplayTag g_forgottenEngineTag = {};

    // True only when the character's ability system can be read and does not
    // carry the tag. Anything unreadable counts as inside, so nothing is
    // revealed that the game would not reveal on foot.
    bool OutsideForgottenEngine(SDK::ACrCharacterPlayerBase* character)
    {
        if (g_forgottenEngineTag.TagName.IsNone())
            g_forgottenEngineTag.TagName = SDK::BasicFilesImplUtils::StringToName(L"Status.InForgottenEngine");
        if (g_forgottenEngineTag.TagName.IsNone())
            return false;

        SDK::UAbilitySystemComponent* asc = SDK::UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(character);
        return asc && asc->GetGameplayTagCount(g_forgottenEngineTag) == 0;
    }

    // Cached so the tick never reads the INI. Written from the loader's
    // config-changed callback, read on the game thread.
    std::atomic<bool> g_fogEnabled{ false };

    // Where the drone last uncovered the map. Plain values, so nothing here
    // outlives a world; cleared whenever the drone is not out.
    bool         g_hasLastReveal = false;
    SDK::FVector g_lastReveal    = {};

    // Mirrors RecordPlayerLocation for the drone: uncover once on the first
    // tick in the drone, then again each time the drone has moved more than
    // MinFootstepDistance from the last spot, with the walking radius
    // (FogOfWarPlayerUncoverRadius scaled by the FogOfWarRadiusMultiplier gem
    // attribute), and never inside the Forgotten Engine, which the footstep
    // reveal skips too. Everything is looked up fresh each tick.
    void OnFogTick(float)
    {
        if (!g_fogEnabled.load(std::memory_order_relaxed))
        {
            g_hasLastReveal = false;
            return;
        }

        SDK::ACrPlayerControllerBase* pc        = LocalController();
        SDK::ACrCharacterPlayerBase*  character = PilotingCharacter(pc);
        if (!character || DroneFields::Dead(character))
        {
            g_hasLastReveal = false;
            return;
        }

        // UncoverFogOfWar reveals for the possessed pawn, which stays the
        // character while the drone is out.
        SDK::ACrCharacterDroneBase* drone = DroneFields::ValueOr(DroneFields::BuildingDrone, character);
        if (pc->Pawn != static_cast<SDK::APawn*>(character) || !drone)
            return;

        SDK::UCrMapMenuDevSettings* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj();
        if (!settings)
            return;

        const SDK::FVector here = drone->K2_GetActorLocation();
        if (g_hasLastReveal && here.GetDistanceTo(g_lastReveal) <= DroneFields::ValueOr(DroneFields::MinFootstepDistance, settings))
            return;

        // Checked only once the drone has moved far enough to reveal, so it
        // costs nothing on the other ticks. The spot still counts as visited,
        // so inside the zone the check runs once per step, not every tick.
        if (!OutsideForgottenEngine(character))
        {
            g_lastReveal    = here;
            g_hasLastReveal = true;
            return;
        }

        auto* subsystem = SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
            pc, SDK::UCrMapManuSubsystem::StaticClass());
        if (!subsystem)
            return;

        float radius = DroneFields::ValueOr(DroneFields::FogUncoverRadius, settings);
        if (SDK::UCrGemAttributeSet* gems = DroneFields::ValueOr(DroneFields::GemAttributes, character))
        {
            if (const auto* multiplier = DroneFields::FogRadiusMultiplier.In(gems))
                radius *= multiplier->CurrentValue;
        }

        const SDK::FVector3f location = { static_cast<float>(here.X),
                                          static_cast<float>(here.Y),
                                          static_cast<float>(here.Z) };
        reinterpret_cast<UncoverFogOfWar_t>(g_addrUncoverFog)(subsystem, pc, &location, radius);

        g_lastReveal    = here;
        g_hasLastReveal = true;
    }

    // ACrCharacterPlayerBase::NativeOnWeaponCancelTool(this). Puts the
    // building tool away (MulticastCancelTool, or the server RPC on a client),
    // unless the building component can restore a move in progress, in which
    // case it does that instead. Putting the tool away while the drone is out
    // also ends the drone: the unequip reaches DeactivateBuildingDrone from
    // UCrWeaponComponent::PostEquip.
    //
    // Two things call it: the cancel-tool input action, and
    // UCrUW_BuildingMenu::HandleCancelTool. The building menu is the one menu
    // that does not close the way the others do. UCrUW_ActivatableWidget
    // binds its Back and Close actions (Escape and Q) to HandleDeactivate;
    // UCrUW_BuildingMenu::SetupInputs binds both, and its CloseMenuInputAction,
    // to HandleCancelTool, which tail-calls this with the menu's Player. On
    // foot that closes the menu and puts the tool away in one press. In drone
    // mode the same press ends the drone.
    typedef void (__fastcall* NativeOnWeaponCancelTool_t)(void* character);

    // UCrUW_ActivatableWidget::HandleDeactivate(this): how every other menu
    // closes on Back and Close. With a Chimera controller it hands the menu
    // layer to UCrUIManagerSubsystem::CloseMainWidgetByClass, which also puts
    // the input config back to the game.
    typedef void (__fastcall* HandleDeactivate_t)(void* widget);

    // Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
    uintptr_t g_addrCancelTool       = 0;
    uintptr_t g_addrHandleDeactivate = 0;

    // Runs through the cancel's first gate, the weapon component flag at
    // 0xDA0, and the controller's building component lookup.
    constexpr const char* kCancelToolPattern =
        "40 53 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 48 8B D9 F6 80 ?? ?? ?? ?? 01 75 ?? "
        "48 8B 89 ?? ?? ?? ?? 48 89 7C 24 30 48 85 C9 74 ?? E8";

    // Runs to the end of the function: the two tails, the jump to
    // CloseMainWidgetByClass and the fallback to DeactivateWidget for a
    // controller that is not a Chimera one, are what tell it apart from the
    // map's and the encyclopedia's own close handlers, which begin the same way.
    constexpr const char* kHandleDeactivatePattern =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B D9 FF 90 ?? ?? ?? ?? 48 8B F8 "
        "48 85 C0 74 ?? E8 ?? ?? ?? ?? 48 8B 4F 10 48 83 C0 30 48 63 50 08 3B 51 38 "
        "7F ?? 48 8B 49 30 48 39 04 D1 75 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B C8 E8 "
        "?? ?? ?? ?? 48 8B D7 48 8B C8 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? ?? ?? ?? "
        "48 8B CB 48 8B 5C 24 30 48 83 C4 20 5F E9";

    NativeOnWeaponCancelTool_t g_origCancelTool = nullptr;
    HookHandle                 g_hookCancelTool = nullptr;

    // The building menu this cancel came from, if the local player is flying
    // the drone and that menu is the active widget on their menu layer.
    // HandleCancelTool runs inside the menu's own action handler, so the
    // menu is still up, and still active, when this is asked. Valid for this
    // call only.
    SDK::UCrUW_BuildingMenu* DroneBuildMenu(void* character)
    {
        SDK::ACrPlayerControllerBase* pc = LocalController();
        if (!character || PilotingCharacter(pc) != character)
            return nullptr;

        SDK::UCommonActivatableWidgetContainerBase* layer = FindMenuLayer(pc);
        SDK::UCrUW_BuildingMenu* menu = layer ? ActiveMenu<SDK::UCrUW_BuildingMenu>(layer) : nullptr;
        if (!menu || DroneFields::ValueOr(DroneFields::BuildingMenuPlayer, menu) != character)
            return nullptr;

        return menu;
    }

    // Game thread: both callers are input handlers.
    void __fastcall Detour_NativeOnWeaponCancelTool(void* character)
    {
        if (SDK::UCrUW_BuildingMenu* menu = DroneBuildMenu(character))
        {
            // Close it the way every other menu closes, and leave the tool,
            // and so the drone, where they are. The next Escape or Q, with
            // no menu up, goes the usual way.
            LOG_DEBUG("DroneBuildMenu: building menu closed, drone kept");
            reinterpret_cast<HandleDeactivate_t>(g_addrHandleDeactivate)(menu);
            return;
        }

        g_origCancelTool(character);
    }
}

void ResolveDroneInteract(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    // Optional throughout: a miss leaves the rest of BetterDrone working, which
    // is what the old scan-at-init path did. The loader still lists each miss.
    g_addrTargetsChanged = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::TargetsChanged, kTargetsChangedPattern);
    g_addrInteractStarted = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::InteractStarted, kInteractStartedPattern);
    g_addrInteractCompleted = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::InteractCompleted, kInteractCompletedPattern);
    g_addrInteract = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::Interact, kInteractPattern);

    if (DroneFields::Features::HoldTakeAll.On())
    {
        g_contextStarted = PluginMap::FunctionAs<InteractVoid_t>(scanner, self, DroneFields::Keys::ContextStarted,
            PLUGIN_MAP_PATTERN(kContextStartedPattern), PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
        g_context = PluginMap::FunctionAs<InteractVoid_t>(scanner, self, DroneFields::Keys::Context,
            PLUGIN_MAP_PATTERN(kContextPattern), PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
        g_contextCompleted = PluginMap::FunctionAs<InteractVoid_t>(scanner, self, DroneFields::Keys::ContextCompleted,
            PLUGIN_MAP_PATTERN(kContextCompletedPattern), PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
    }
}

bool InitDroneInteract()
{
    if (!InstallHooks())
    {
        RemoveHooks();
        return false;
    }

    // Without the three context functions only the hold is off; the interact
    // key above keeps working.
    g_holdOn = DroneFields::Features::HoldTakeAll.On() && g_contextStarted && g_context && g_contextCompleted;
    if (!g_holdOn)
        LOG_INFO("DroneInteract: hold to take all is off on this build");

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

SDK::ACrCharacterPlayerBase* LocalPlayerCharacter()
{
    SDK::ACrPlayerControllerBase* pc = LocalController();
    return DroneFields::ValueOr(DroneFields::CrChar, pc);
}

SDK::ACrPlayerControllerBase* LocalPlayerController()
{
    return LocalController();
}

SDK::UCrUW_MapMenu* ActiveLocalMap()
{
    // No map on the menu layer, no walk of it.
    if (!(g_uiState.load(std::memory_order_relaxed) & PLUGIN_UI_MAP))
        return nullptr;

    SDK::ACrPlayerControllerBase* pc = LocalController();
    SDK::UCommonActivatableWidgetContainerBase* layer = pc ? FindMenuLayer(pc) : nullptr;
    return layer ? ActiveMap(layer) : nullptr;
}

bool IsLocalPlayerInDrone()
{
    try
    {
        return PilotingCharacter(LocalController()) != nullptr;
    }
    catch (...)
    {
        return false;
    }
}

bool IsGameMenuOpen()
{
    return (g_uiState.load(std::memory_order_relaxed) & PLUGIN_UI_ANY_MENU) != 0;
}

void InitDroneMenuState(IPluginSelf* self)
{
    if (!self->hooks->GameMenu)
    {
        LOG_WARN("DroneMenuState: no game menu table — menus will read as closed");
        return;
    }

    self->hooks->GameMenu->RegisterOnUIStateChanged(kMenuStateMask, &OnUIState, nullptr);
}

void ShutdownDroneMenuState(IPluginSelf* self)
{
    if (self->hooks->GameMenu)
        self->hooks->GameMenu->UnregisterOnUIStateChanged(&OnUIState, nullptr);

    g_uiState.store(0, std::memory_order_relaxed);
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

    g_holdOn      = false;
    g_holdKey     = FollowedKey{};
    g_holdInDrone = false;
    g_holdDown    = false;
    g_holdArmed   = false;
    g_holdRan     = false;
    g_holdSeconds = 0.0f;
    g_holdTimeRead = false;
    g_holdTarget.Reset();

    LOG_DEBUG("DroneInteract: hooks removed");
}

void ResolveDroneMap(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    g_addrMapMenu = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::MapMenu, kMapMenuPattern);
}

bool InitDroneMap()
{
    if (!g_addrMapMenu)
    {
        LOG_WARN("DroneMap: NativeOnInputMapMenu unresolved — the map will not open from the drone");
        return false;
    }

    auto* input = GetSelf()->hooks->Input;
    if (!input)
    {
        LOG_WARN("DroneMap: no input dispatch — the map key will not reach drone mode");
        return false;
    }

    GetSelf()->hooks->Engine->RegisterOnTick(OnMapTick);

    DroneConfig::Config::ReadMapKey(g_mapKeyName, sizeof(g_mapKeyName));
    input->RegisterKeybindByName(g_mapKeyName, EModKeyEvent::Pressed, &OnMapKey);

    LOG_INFO("DroneMap: NativeOnInputMapMenu at 0x%llX, map enabled in drone mode on '%s'",
             g_addrMapMenu, g_mapKeyName);
    return true;
}

void ShutdownDroneMap()
{
    auto* input = GetSelf()->hooks->Input;
    if (input && g_mapKeyName[0])
        input->UnregisterKeybindByName(g_mapKeyName, EModKeyEvent::Pressed, &OnMapKey);

    GetSelf()->hooks->Engine->UnregisterOnTick(OnMapTick);

    g_pendingMapPress.store(false, std::memory_order_relaxed);
    g_menuWasOpen.store(false, std::memory_order_relaxed);
}

void ResolveDroneFog(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    g_addrUncoverFog = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::UncoverFog, kUncoverFogPattern);
}

bool InitDroneFog()
{
    if (!g_addrUncoverFog)
    {
        LOG_WARN("DroneFog: UncoverFogOfWar unresolved — the drone will not uncover the map");
        return false;
    }

    g_hasLastReveal = false;
    SetDroneFogEnabled(DroneConfig::Config::ReadDroneRevealsMap());
    GetSelf()->hooks->Engine->RegisterOnTick(OnFogTick);

    LOG_INFO("DroneFog: UncoverFogOfWar at 0x%llX", g_addrUncoverFog);
    return true;
}

void SetDroneFogEnabled(bool enabled)
{
    g_fogEnabled.store(enabled, std::memory_order_relaxed);
}

void ShutdownDroneFog()
{
    GetSelf()->hooks->Engine->UnregisterOnTick(OnFogTick);

    g_fogEnabled.store(false, std::memory_order_relaxed);
}

void ResolveDroneBuildMenu(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    g_addrCancelTool = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::CancelTool, kCancelToolPattern);
    g_addrHandleDeactivate = DroneFields::ResolveFunction(self, scanner,
        DroneFields::Keys::HandleDeactivate, kHandleDeactivatePattern);
}

bool InitDroneBuildMenu()
{
    if (!g_addrCancelTool || !g_addrHandleDeactivate)
    {
        LOG_WARN("DroneBuildMenu: unresolved addresses — cancelTool=0x%llX handleDeactivate=0x%llX; "
                 "closing the building menu will still end drone mode",
                 g_addrCancelTool, g_addrHandleDeactivate);
        return false;
    }

    g_hookCancelTool = GetSelf()->hooks->Hooks->Install(
        g_addrCancelTool,
        reinterpret_cast<void*>(&Detour_NativeOnWeaponCancelTool),
        reinterpret_cast<void**>(&g_origCancelTool));

    if (!g_hookCancelTool)
    {
        LOG_WARN("DroneBuildMenu: hook installation failed");
        return false;
    }

    LOG_INFO("DroneBuildMenu: NativeOnWeaponCancelTool at 0x%llX, HandleDeactivate at 0x%llX",
             g_addrCancelTool, g_addrHandleDeactivate);
    return true;
}

void ShutdownDroneBuildMenu()
{
    if (g_hookCancelTool)
    {
        GetSelf()->hooks->Hooks->Remove(g_hookCancelTool);
        g_hookCancelTool = nullptr;
        g_origCancelTool = nullptr;
    }
}
