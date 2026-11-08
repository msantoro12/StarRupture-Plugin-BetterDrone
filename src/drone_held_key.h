#pragma once
#include <plugin_interface.h>
#include <atomic>

// A key held to change how the drone flies (Boost, Slow). The loader's
// keybind callback sets a flag; the tick also polls the key's VK, which is
// the only way to see a bare modifier (the loader never dispatches those to a
// keybind) and, for Boost, a key it does not own at all.
class HeldKey
{
public:
    // `label` names the key in the log ("boost"); `inDrone` is the flag the
    // tick keeps current, so a press is logged at the level that fits.
    HeldKey(const char* label, const std::atomic<bool>& inDrone) : m_label(label), m_inDrone(inDrone) {}

    // Registers keyName with the loader and caches its VK. A named combo's
    // Released only fires on an exact modifier match, so "Shift+K" never sees
    // a Released if the player lets go of Shift before K and the key would
    // read stuck held. The bare base key has no modifier requirement and
    // always fires on release, so that is where Released is registered.
    // `callback` must set the flag through OnEvent.
    void Register(IPluginSelf* self, const char* keyName, PluginKeybindCallback callback);

    // Unregisters what Register registered.
    void Unregister(IPluginSelf* self);

    // The key's VK for the poll; 0 polls nothing.
    void SetVk(int vk) { m_vk.store(vk, std::memory_order_relaxed); }
    int  Vk() const    { return m_vk.load(std::memory_order_relaxed); }

    // The keybind callback's body.
    void OnEvent(EModKeyEvent event);

    // Held now: the loader's flag (cleared when the polled key is up), or,
    // in the drone and with the game in front, the polled key.
    bool Held(bool inDrone) const;

private:
    const char*                  m_label;
    const std::atomic<bool>&     m_inDrone;
    mutable std::atomic<bool>    m_held{ false };
    std::atomic<int>             m_vk{ 0 };
    PluginKeybindCallback        m_callback = nullptr;

    // The two names actually registered, kept so Unregister always matches.
    char m_pressedName[64]  = {};
    char m_releasedName[64] = {};
};
