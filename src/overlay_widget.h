#pragma once
#include <plugin_interface.h>
#include <atomic>

// A transparent window of the loader's for draw-list drawing over the game:
// no title, no input, no background, and shown only while there is
// something to draw. Used by the drone's map marker and the laser's heat
// ring.
class OverlayWidget
{
public:
    // Registers the widget, hidden. False if the loader would not.
    bool Register(IPluginSelf* self, const char* name, PluginImGuiRenderCallback render);

    // Hides and unregisters it. Touches no game object, so any thread.
    void Unregister(IPluginSelf* self);

    // Game thread. Shows or hides it; a call that changes nothing is free.
    void SetShown(bool shown);

    bool IsRegistered() const { return m_widget.load() != nullptr; }

private:
    PluginWidgetDesc          m_desc{};
    std::atomic<WidgetHandle> m_widget{ nullptr };
    bool                      m_shown = false;
};
