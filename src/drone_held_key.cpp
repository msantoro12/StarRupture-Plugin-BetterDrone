#include "drone_held_key.h"
#include "drone_key_vk.h"
#include "plugin_helpers.h"
#include <cstdio>
#include <windows.h>

void HeldKey::Register(IPluginSelf* self, const char* keyName, PluginKeybindCallback callback)
{
    if (!self || !self->hooks->Input || !keyName || !keyName[0])
        return;

    char baseKey[64] = {};
    ExtractBaseKey(keyName, baseKey, sizeof(baseKey));

    self->hooks->Input->RegisterKeybindByName(keyName, EModKeyEvent::Pressed, callback);
    self->hooks->Input->RegisterKeybindByName(baseKey, EModKeyEvent::Released, callback);
    m_callback = callback;
    snprintf(m_pressedName, sizeof(m_pressedName), "%s", keyName);
    snprintf(m_releasedName, sizeof(m_releasedName), "%s", baseKey);
    SetVk(KeyNameToVk(keyName));
    LOG_INFO("%s key: custom (%s)", m_label, keyName);
}

void HeldKey::Unregister(IPluginSelf* self)
{
    if (!self || !self->hooks->Input)
        return;

    if (m_pressedName[0] != '\0')
        self->hooks->Input->UnregisterKeybindByName(m_pressedName, EModKeyEvent::Pressed, m_callback);
    if (m_releasedName[0] != '\0')
        self->hooks->Input->UnregisterKeybindByName(m_releasedName, EModKeyEvent::Released, m_callback);

    m_pressedName[0]  = '\0';
    m_releasedName[0] = '\0';
}

void HeldKey::OnEvent(EModKeyEvent event)
{
    const bool held = (event == EModKeyEvent::Pressed);
    m_held.store(held, std::memory_order_relaxed);

    if (m_inDrone.load(std::memory_order_relaxed))
    {
        LOG_INFO("OnKeyEvent: %s key %s (in drone: yes)", m_label, held ? "pressed" : "released");
    }
    else
    {
        LOG_DEBUG("OnKeyEvent: %s key %s (in drone: no)", m_label, held ? "pressed" : "released");
    }
}

bool HeldKey::Held(bool inDrone) const
{
    bool held = m_held.load(std::memory_order_relaxed);
    if (!held && inDrone)
    {
        const int vk = m_vk.load(std::memory_order_relaxed);
        held = vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0 && GameHasFocus();
    }
    return held;
}
