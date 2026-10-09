#include "overlay_widget.h"
#include "plugin_helpers.h"

namespace
{
    constexpr int kWindowFlagAlwaysAutoResize = 1 << 6;
    const PluginWindowHints kHints = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0,
        PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoResize | PluginWindowFlags_NoMove |
        PluginWindowFlags_NoScrollbar | PluginWindowFlags_NoBackground | PluginWindowFlags_NoSavedSettings |
        PluginWindowFlags_NoMouseInputs | kWindowFlagAlwaysAutoResize
    };
}

bool OverlayWidget::Register(IPluginSelf* self, const char* name, PluginImGuiRenderCallback render)
{
    if (!self || !self->hooks || !self->hooks->UI)
        return false;

    m_desc = { name, render, &kHints };
    WidgetHandle widget = self->hooks->UI->RegisterWidget(&m_desc);
    if (!widget)
        return false;

    self->hooks->UI->SetWidgetVisible(widget, false);
    m_shown = false;
    m_widget.store(widget);
    return true;
}

void OverlayWidget::Unregister(IPluginSelf* self)
{
    WidgetHandle widget = m_widget.exchange(nullptr);
    if (widget && self && self->hooks && self->hooks->UI)
    {
        self->hooks->UI->SetWidgetVisible(widget, false);
        self->hooks->UI->UnregisterWidget(widget);
    }
    m_shown = false;
}

void OverlayWidget::SetShown(bool shown)
{
    if (m_shown == shown)
        return;

    WidgetHandle widget = m_widget.load();
    IPluginSelf* self   = GetSelf();
    if (widget && self && self->hooks && self->hooks->UI)
    {
        self->hooks->UI->SetWidgetVisible(widget, shown);
        m_shown = shown;
    }
}
