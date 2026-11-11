#include "drone_esc_pause.h"
#include "drone_config.h"
#include "drone_fields.h"
#include "drone_interact.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <atomic>
#include <cstdint>

namespace
{
    // ACrPlayerControllerBase::NativeOnInputCloseInGameMenu(this, value): the
    // controller function behind the game's close-menu input action. Not
    // reflected. It puts a building move back if one can be restored, ends the
    // drone if it is out, then closes the actor widgets and gives the mouse
    // back to the game. The value is the input action's, passed through.
    // Hooked. Null means the key is missing on this build, which turns off
    // only this option (DroneFields::Features::EscPause).
    using CloseInGameMenu_t = void (__fastcall*)(void* controller, const void* value);

    // The first bytes of the function, up to the build-component test and the
    // widget-subsystem flag test. The member offsets and the calls are
    // wildcarded, so the same bytes are expected to match on every game build.
    constexpr const char* kCloseInGameMenuPattern =
        "48 89 74 24 18 57 48 83 EC 30 48 8B F9 48 8B F2 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 ?? "
        "E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 48 8B 87 ?? ?? ?? ?? "
        "48 89 44 24 40 48 89 5C 24 48 74 ??";

    CloseInGameMenu_t g_closeInGameMenu = nullptr;
    CloseInGameMenu_t g_origCloseInGameMenu = nullptr;
    HookHandle        g_hookCloseInGameMenu = nullptr;

    // Game ticks seen so far. Bumped on the game thread by OnTick.
    std::atomic<uint64_t> g_tick{ 0 };

    // The Esc key press this plugin last saw: g_escTick is the tick it arrived
    // in plus one (0: none), g_escSeq counts presses. Written by the keybind
    // callback, whose thread is not guaranteed, and read on the game thread.
    // The loader dispatches the key from the window procedure as the game reads
    // its messages, before the game's input runs, so a handler that belongs to
    // this press runs in the same tick or the next. Counting ticks rather than
    // milliseconds keeps a slow frame from losing the press and a later key from
    // borrowing it.
    std::atomic<uint64_t> g_escTick{ 0 };
    std::atomic<uint64_t> g_escSeq{ 0 };

    // The press that already opened the pause menu. A second handler reached by
    // the same press is swallowed without opening it again.
    std::atomic<uint64_t> g_openedSeq{ 0 };

    // The hook is in. The cancel-tool detour asks before it does anything.
    std::atomic<bool> g_active{ false };

    void OnTick(float)
    {
        g_tick.fetch_add(1, std::memory_order_relaxed);
    }

    void OnEscKey(EModKey, EModKeyEvent event)
    {
        if (event != EModKeyEvent::Pressed)
            return;
        g_escSeq.fetch_add(1, std::memory_order_relaxed);
        g_escTick.store(g_tick.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // Q ends the Esc press: whatever the game's handlers run after a Q press
    // belongs to Q, never to a stale Esc.
    void OnQKey(EModKey, EModKeyEvent event)
    {
        if (event == EModKeyEvent::Pressed)
            g_escTick.store(0, std::memory_order_relaxed);
    }

    // True when this handler call belongs to an Esc key press in the drone with
    // the option on. Game thread. The config is read last, so a Q press or a
    // press on foot never touches the INI.
    bool EscInDrone(const SDK::ACrPlayerControllerBase* controller)
    {
        const uint64_t at = g_escTick.load(std::memory_order_relaxed);
        if (!at || g_tick.load(std::memory_order_relaxed) - (at - 1) > 1)
            return false;

        if (!controller || controller != LocalPlayerController() || !IsLocalPlayerInDrone())
            return false;

        return DroneConfig::Config::ReadEscPauseInDrone();
    }

    // Takes the Esc press that EscInDrone accepted: opens the pause menu the
    // way Esc does on foot, once per press. The caller skips the game's
    // handler either way.
    void TakeEsc(SDK::ACrPlayerControllerBase* controller, const char* path)
    {
        const uint64_t seq = g_escSeq.load(std::memory_order_relaxed);

        // A menu already up closes itself on this press, and a menu this
        // press opened is not opened twice. Either way the same press reaching
        // the drone's handler must not recall the drone.
        if (g_openedSeq.load(std::memory_order_relaxed) == seq || IsGameMenuOpen())
        {
            LOG_DEBUG("DroneEscPause: Esc with a menu up or already handled (%s), drone kept", path);
            return;
        }

        g_openedSeq.store(seq, std::memory_order_relaxed);
        LOG_DEBUG("DroneEscPause: Esc in the drone, pause menu opened (%s)", path);
        controller->SetOpenGameMenu(true);
    }

    void __fastcall Detour_NativeOnInputCloseInGameMenu(void* controller, const void* value)
    {
        auto* pc = static_cast<SDK::ACrPlayerControllerBase*>(controller);
        if (EscInDrone(pc))
        {
            TakeEsc(pc, "close-menu input");
            return;
        }

        g_origCloseInGameMenu(controller, value);
    }
}

void ResolveDroneEscPause(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    g_closeInGameMenu = PluginMap::FunctionAs<CloseInGameMenu_t>(scanner, self, DroneFields::Keys::CloseInGameMenu,
        PLUGIN_MAP_PATTERN(kCloseInGameMenuPattern), PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
}

bool InitDroneEscPause(IPluginSelf* self)
{
    if (!self || !g_closeInGameMenu || !self->hooks->Input || !self->hooks->Hooks || !self->hooks->Engine)
    {
        LOG_WARN("DroneEscPause: no close-menu handler or input dispatch -- Esc will keep ending the drone");
        return false;
    }

    g_hookCloseInGameMenu = self->hooks->Hooks->Install(
        reinterpret_cast<uintptr_t>(g_closeInGameMenu),
        reinterpret_cast<void*>(&Detour_NativeOnInputCloseInGameMenu),
        reinterpret_cast<void**>(&g_origCloseInGameMenu));
    if (!g_hookCloseInGameMenu)
    {
        LOG_WARN("DroneEscPause: hook installation failed -- Esc will keep ending the drone");
        return false;
    }

    self->hooks->Engine->RegisterOnTick(OnTick);
    self->hooks->Input->RegisterKeybind(EModKey::Escape, EModKeyEvent::Pressed, OnEscKey);
    self->hooks->Input->RegisterKeybind(EModKey::Q, EModKeyEvent::Pressed, OnQKey);
    g_active.store(true, std::memory_order_relaxed);

    LOG_INFO("DroneEscPause: close-menu handler at 0x%llX", reinterpret_cast<uintptr_t>(g_closeInGameMenu));
    return true;
}

void ShutdownDroneEscPause(IPluginSelf* self)
{
    g_active.store(false, std::memory_order_relaxed);

    if (self && self->hooks->Input)
    {
        self->hooks->Input->UnregisterKeybind(EModKey::Escape, EModKeyEvent::Pressed, OnEscKey);
        self->hooks->Input->UnregisterKeybind(EModKey::Q, EModKeyEvent::Pressed, OnQKey);
    }

    if (self && self->hooks->Engine)
        self->hooks->Engine->UnregisterOnTick(OnTick);

    if (g_hookCloseInGameMenu && self)
        self->hooks->Hooks->Remove(g_hookCloseInGameMenu);

    g_hookCloseInGameMenu = nullptr;
    g_origCloseInGameMenu = nullptr;
    g_escTick.store(0, std::memory_order_relaxed);
}

bool DroneEscPauseTakeCancel(void* character)
{
    if (!g_active.load(std::memory_order_relaxed) || !character || character != LocalPlayerCharacter())
        return false;

    SDK::ACrPlayerControllerBase* pc = LocalPlayerController();
    if (!EscInDrone(pc))
        return false;

    TakeEsc(pc, "cancel-tool input");
    return true;
}
