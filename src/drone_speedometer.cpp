#include "drone_speedometer.h"
#include "drone_config.h"
#include "drone_heat_ring.h"
#include "drone_interact.h"
#include "drone_map_marker.h"
#include "drone_ui.h"
#include "object_ref.h"
#include "overlay_widget.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <ChimeraUI_classes.hpp>
#include <Engine_classes.hpp>
#include <UMG_classes.hpp>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace
{
    // The speed is sampled, and the text placed, this often, never every frame.
    constexpr uint64_t kSampleMs = 100;

    // Each sample's weight in the shown speed. Enough to hide the
    // sample-to-sample jitter, little enough to follow a boost.
    constexpr float kSmoothing = 0.5f;

    // A drone HUD that could not be found is looked for again this often.
    constexpr uint64_t kFindRetryMs = 2000;

    // How long the drone is out before a readout still not laid out counts as
    // a failed attach: the game shows its HUD a moment after the launch.
    constexpr uint64_t kAttachGraceMs = 3000;

    // Moves smaller than this, in the panel's units, are not applied.
    constexpr double kMoveEpsilon = 0.25;

    // The room the text needs under the readout, in ems of its font: wide
    // enough for "9999 km/h", one line high.
    constexpr double kTextWidthEm  = 6.0;
    constexpr double kTextHeightEm = 1.5;

    // Guards the walk up the widget tree against a loop.
    constexpr int kMaxTreeDepth = 64;

    // The fallback beside the crosshair, in the game's UI units like the
    // heat ring: the gap between the ring's outer edge and the text, and the
    // text's height.
    constexpr float kGap      = 6.0f;
    constexpr float kTextSize = 18.0f;

    constexpr unsigned int kOutline = 0xC0000000u;   // black, 75%

    // Handed from the tick to the fallback's render callback. A speed below
    // zero shows nothing.
    std::atomic<float>        g_speed{ -1.0f };   // cm/s, smoothed
    std::atomic<float>        g_scale{ 1.0f };    // the game's UI scale
    std::atomic<unsigned int> g_colour{ 0xFFFFFFFFu };

    OverlayWidget g_overlay;

    // Game thread only from here on.
    uint64_t g_nextSampleMs = 0;
    uint64_t g_nextFindMs   = 0;
    uint64_t g_droneSinceMs = 0;
    bool     g_wasInDrone   = false;
    float    g_smoothed     = -1.0f;   // below 0 until a flight's first sample

    // The text block under the drone HUD's distance readout, the panel it was
    // added to, and the HUD. Looked up again on every use.
    ObjectRef<SDK::UCrUW_BuildingDroneHud> g_hud;
    ObjectRef<SDK::UPanelWidget>           g_panel;
    ObjectRef<SDK::UTextBlock>             g_text;

    bool           g_attachFailed   = false;   // until the drone is next launched
    bool           g_loggedAttach   = false;   // one line per path, per load
    bool           g_loggedFallback = false;
    float          g_textOpacity    = 1.0f;
    SDK::FVector2D g_position       = {};
    SDK::FVector2D g_room           = {};      // the text size the panel was checked for
    std::string    g_shownText;

    // One FText per string ever shown, built once and kept, as the loader's
    // menu rows do: dropping an FText means releasing its refcounted text
    // data by hand. Speeds are rounded to three significant figures, which
    // keeps the set to a few thousand strings at most.
    std::unordered_map<std::string, SDK::FText> g_texts;

    enum class Attached { Yes, NotYet, Never };

    // `cmPerSec` in the panel's unit, to three significant figures.
    void FormatSpeed(float cmPerSec, char* out, size_t size)
    {
        char unit[16] = {};
        const float inUnit = (std::min)(SpeedInPanelUnit(cmPerSec, unit, sizeof(unit)), 999999.0f);
        const float step   = inUnit >= 10000.0f ? 100.0f : inUnit >= 1000.0f ? 10.0f : 1.0f;
        const long  value  = static_cast<long>(std::round(inUnit / step) * step);
        snprintf(out, size, "%ld %s", value, unit);
    }

    // Render thread. The fallback: just right of the crosshair, outside the
    // heat ring.
    void RenderSpeed(IModLoaderImGui* ui)
    {
        const float speed = g_speed.load();
        if (speed < 0.0f)
            return;

        float cx = 0.0f, cy = 0.0f;
        if (!DroneHeatRing::Crosshair(ui, &cx, &cy))
            return;

        char text[48];
        FormatSpeed(speed, text, sizeof(text));

        const float scale = g_scale.load();
        const float size  = kTextSize * scale;
        const float x     = cx + (DroneHeatRing::kOuterRadius + kGap) * scale;
        const float y     = cy - 0.5f * size;
        const float o     = (std::max)(1.0f, scale);

        // Behind the loader's windows, unlike the ring: it shows for the whole
        // flight, and must not cover a panel opened over the crosshair.
        PluginDrawList dl = ui->GetBackgroundDrawList();
        ui->DL_AddTextSized(dl, size, x - o, y, kOutline, text);
        ui->DL_AddTextSized(dl, size, x + o, y, kOutline, text);
        ui->DL_AddTextSized(dl, size, x, y - o, kOutline, text);
        ui->DL_AddTextSized(dl, size, x, y + o, kOutline, text);
        ui->DL_AddTextSized(dl, size, x, y, g_colour.load(), text);
    }

    void HideFallback()
    {
        g_speed.store(-1.0f);
        g_overlay.SetShown(false);
    }

    SDK::FVector2D Vec(double x, double y)
    {
        SDK::FVector2D v;
        v.X = x;
        v.Y = y;
        return v;
    }

    // The drone HUD that belongs to `character`. A scan of the object table,
    // run only while no HUD is known, at most every kFindRetryMs.
    SDK::UCrUW_BuildingDroneHud* FindHud(SDK::ACrCharacterPlayerBase* character)
    {
        SDK::UClass* hudClass = SDK::UCrUW_BuildingDroneHud::StaticClass();
        if (!hudClass)
            return nullptr;

        constexpr uint32_t kTemplateFlags =
            static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject) |
            static_cast<uint32_t>(SDK::EObjectFlags::ArchetypeObject);

        SDK::UCrUW_BuildingDroneHud* unowned = nullptr;
        const int32_t count = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < count; ++i)
        {
            SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
            if (!obj || (static_cast<uint32_t>(obj->Flags) & kTemplateFlags) || !obj->IsA(hudClass))
                continue;

            auto* hud = static_cast<SDK::UCrUW_BuildingDroneHud*>(obj);
            if (!ObjectRef<SDK::UCrUW_BuildingDroneHud>(hud).Get() || !hud->DistanceValueU)
                continue;

            if (hud->PlayerCharacter == character)
                return hud;
            if (!hud->PlayerCharacter && !unowned)
                unowned = hud;
        }
        return unowned;
    }

    // The widget that holds `widget`. A user widget's tree ends at its root;
    // the walk then carries on from the user widget itself, in the tree that
    // holds it.
    SDK::UWidget* ParentOf(SDK::UWidget* widget)
    {
        if (widget->Slot)
            return widget->Slot->Parent;

        SDK::UObject* tree  = widget->Outer;
        SDK::UObject* owner = tree && tree->IsA(SDK::UWidgetTree::StaticClass()) ? tree->Outer : nullptr;
        return owner && owner->IsA(SDK::UUserWidget::StaticClass()) ? static_cast<SDK::UWidget*>(owner) : nullptr;
    }

    bool IsShown(const SDK::UWidget* widget)
    {
        return widget->Visibility != SDK::ESlateVisibility::Collapsed &&
               widget->Visibility != SDK::ESlateVisibility::Hidden;
    }

    // Whether the readout is on screen as far up as `panel` (to the top of
    // the tree when null), and the opacity the widgets in between give it.
    bool ReadoutShown(SDK::UWidget* readout, SDK::UWidget* panel, float* opacity)
    {
        float product = 1.0f;
        SDK::UWidget* widget = readout;
        for (int depth = 0; widget && widget != panel && depth < kMaxTreeDepth; ++depth)
        {
            if (!IsShown(widget))
                return false;
            product *= widget->RenderOpacity;
            widget = ParentOf(widget);
        }
        *opacity = product;
        return product > 0.01f;
    }

    // The visible distance digits, in `panel`'s own units: their left edge
    // and bottom edge, and how much bigger they draw there than in their own
    // units. False until the HUD has been laid out.
    bool DigitsIn(SDK::UCrUW_BuildingDroneHud* hud, SDK::UWidget* panel, double* left, double* bottom, double* scale)
    {
        const SDK::FGeometry panelGeo = panel->GetCachedGeometry();
        if (SDK::USlateBlueprintLibrary::GetLocalSize(panelGeo).X <= 0.0)
            return false;

        SDK::UTextBlock* const elems[] = {
            hud->DistanceValueT, hud->DistanceValueH, hud->DistanceValueD, hud->DistanceValueU, hud->DistanceValueM };

        bool any = false;
        *scale = 1.0;
        for (SDK::UTextBlock* elem : elems)
        {
            // A leading zero may be kept in the row but faded out.
            if (!elem || !IsShown(elem) || elem->RenderOpacity < 0.05f || elem->ColorAndOpacity.SpecifiedColor.A < 0.05f)
                continue;

            const SDK::FGeometry geo  = elem->GetCachedGeometry();
            const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetLocalSize(geo);
            if (size.X <= 0.0 || size.Y <= 0.0)
                continue;

            const SDK::FVector2D topLeft = SDK::USlateBlueprintLibrary::AbsoluteToLocal(
                panelGeo, SDK::USlateBlueprintLibrary::LocalToAbsolute(geo, Vec(0.0, 0.0)));
            const SDK::FVector2D bottomRight = SDK::USlateBlueprintLibrary::AbsoluteToLocal(
                panelGeo, SDK::USlateBlueprintLibrary::LocalToAbsolute(geo, size));

            if (!any || topLeft.X < *left)        *left   = topLeft.X;
            if (!any || bottomRight.Y > *bottom)  *bottom = bottomRight.Y;
            if (elem == hud->DistanceValueU)      *scale  = (bottomRight.Y - topLeft.Y) / size.Y;
            any = true;
        }
        return any;
    }

    // Why the speed is beside the crosshair, then the readout's ancestry,
    // one widget per line from the units digit up (a log line has a length
    // limit), so the next attempt starts from facts.
    void LogFallback(const char* why, SDK::UWidget* readout)
    {
        LOG_INFO("Speedometer: beside the crosshair: %s", why);
        SDK::UWidget* widget = readout;
        for (int depth = 0; widget && depth < kMaxTreeDepth; ++depth, widget = ParentOf(widget))
        {
            const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetLocalSize(widget->GetCachedGeometry());
            LOG_INFO("Speedometer:   %s %s [%s, visibility %d, opacity %.2f, %.0fx%.0f]",
                widget->Class ? widget->Class->GetName().c_str() : "?", widget->GetName().c_str(),
                widget->Slot && widget->Slot->Class ? widget->Slot->Class->GetName().c_str() : "no slot",
                static_cast<int>(widget->Visibility), widget->RenderOpacity, size.X, size.Y);
        }
    }

    const SDK::FText* TextFor(const std::string& s)
    {
        auto it = g_texts.find(s);
        if (it != g_texts.end())
            return &it->second;

        const SDK::FText text = SDK::UKismetTextLibrary::Conv_StringToText(SDK::FString(std::wstring(s.begin(), s.end()).c_str()));
        return &g_texts.emplace(s, text).first->second;
    }

    void Forget()
    {
        g_text.Reset();
        g_panel.Reset();
        g_textOpacity = 1.0f;
        g_position    = {};
        g_room        = {};
        g_shownText.clear();
    }

    // The text block, while it is still the one added to the panel.
    SDK::UTextBlock* OurText()
    {
        SDK::UTextBlock*   text  = g_text.Get();
        SDK::UPanelWidget* panel = g_panel.Get();
        return text && panel && text->Slot && text->Slot->Parent == panel ? text : nullptr;
    }

    // Takes the text block out of the HUD, if it is still ours.
    void Detach()
    {
        if (SDK::UTextBlock* text = OurText())
            text->RemoveFromParent();
        Forget();
    }

    void ShowText(SDK::UTextBlock* text, bool shown)
    {
        if (IsShown(text) != shown)
            text->SetVisibility(shown ? SDK::ESlateVisibility::HitTestInvisible : SDK::ESlateVisibility::Collapsed);
    }

    // Exactly these classes: a subclass such as CommonUI's visibility
    // switcher lays its children out differently.
    bool IsOverlay(const SDK::UWidget* widget) { return widget->Class == SDK::UOverlay::StaticClass(); }
    bool IsCanvas(const SDK::UWidget* widget)  { return widget->Class == SDK::UCanvasPanel::StaticClass(); }

    bool SameColour(const SDK::FSlateColor& a, const SDK::FSlateColor& b)
    {
        return a.ColorUseRule == b.ColorUseRule &&
               a.SpecifiedColor.R == b.SpecifiedColor.R && a.SpecifiedColor.G == b.SpecifiedColor.G &&
               a.SpecifiedColor.B == b.SpecifiedColor.B && a.SpecifiedColor.A == b.SpecifiedColor.A;
    }

    // Puts the text at `position` in its panel: an overlay slot's padding,
    // or a canvas slot's position.
    void Place(SDK::UTextBlock* text, SDK::UPanelWidget* panel, const SDK::FVector2D& position)
    {
        if (IsOverlay(panel))
        {
            SDK::FMargin padding{};
            padding.Left = static_cast<float>(position.X);
            padding.Top  = static_cast<float>(position.Y);
            static_cast<SDK::UOverlaySlot*>(text->Slot)->SetPadding(padding);
        }
        else
        {
            static_cast<SDK::UCanvasPanelSlot*>(text->Slot)->SetPosition(position);
        }
        g_position = position;
    }

    // Adds a text block, styled like the readout's units digit, to the
    // nearest overlay or canvas panel above the readout that has room for it
    // under the digits. Only those two place a child without moving the ones
    // already there: a horizontal or vertical box would shift the readout or
    // the rows under it, and a size box or border holds a single child. Room
    // means the text stays inside the panel's current bounds, so the panel's
    // own size, and everything laid out around it, is unchanged.
    Attached Attach(SDK::UCrUW_BuildingDroneHud* hud, const char** why)
    {
        *why = "the distance readout is hidden or not laid out yet";
        SDK::UTextBlock* source = hud->DistanceValueU;
        float opacity = 1.0f;
        if (!source || !ReadoutShown(source, nullptr, &opacity))
            return Attached::NotYet;

        const SDK::FVector2D sourceSize = SDK::USlateBlueprintLibrary::GetLocalSize(source->GetCachedGeometry());
        if (sourceSize.X <= 0.0 || sourceSize.Y <= 0.0)
            return Attached::NotYet;

        SDK::UPanelWidget* panel = nullptr;
        SDK::FVector2D room = {};
        double left = 0.0, bottom = 0.0, scale = 1.0;
        SDK::UWidget* widget = ParentOf(source);
        for (int depth = 0; widget && depth < kMaxTreeDepth; ++depth, widget = ParentOf(widget))
        {
            if (!IsOverlay(widget) && !IsCanvas(widget))
                continue;
            if (!DigitsIn(hud, widget, &left, &bottom, &scale))
                return Attached::NotYet;

            const double em = source->Font.Size * scale * 96.0 / 72.0;
            const SDK::FVector2D bounds = SDK::USlateBlueprintLibrary::GetLocalSize(widget->GetCachedGeometry());
            room = Vec(kTextWidthEm * em, kTextHeightEm * em);
            if (left >= 0.0 && bottom >= 0.0 && left + room.X <= bounds.X && bottom + room.Y <= bounds.Y)
            {
                panel = static_cast<SDK::UPanelWidget*>(widget);
                break;
            }
        }
        *why = "no overlay or canvas panel above the distance readout has room for the text";
        if (!panel)
            return Attached::Never;

        *why = "the text block could not be created";
        SDK::UObject* created = SDK::UGameplayStatics::SpawnObject(SDK::UTextBlock::StaticClass(), panel->Outer);
        if (!created || !created->IsA(SDK::UTextBlock::StaticClass()))
            return Attached::Never;
        auto* text = static_cast<SDK::UTextBlock*>(created);

        // Drawn at the readout's size even if a panel in between scales it.
        SDK::FSlateFontInfo font = source->Font;
        if (std::fabs(scale - 1.0) > 0.01)
            font.Size = static_cast<float>(font.Size * scale);

        text->SetVisibility(SDK::ESlateVisibility::Collapsed);
        text->SetFont(font);
        text->SetColorAndOpacity(source->ColorAndOpacity);
        text->SetShadowOffset(source->ShadowOffset);
        text->SetShadowColorAndOpacity(source->ShadowColorAndOpacity);
        text->SetTextTransformPolicy(source->TextTransformPolicy);
        text->SetJustification(SDK::ETextJustify::Left);

        *why = "the panel would not take the text block";
        if (IsOverlay(panel))
        {
            SDK::UOverlaySlot* slot = static_cast<SDK::UOverlay*>(panel)->AddChildToOverlay(text);
            if (!slot)
                return Attached::Never;
            slot->SetHorizontalAlignment(SDK::EHorizontalAlignment::HAlign_Left);
            slot->SetVerticalAlignment(SDK::EVerticalAlignment::VAlign_Top);
        }
        else
        {
            SDK::UCanvasPanelSlot* slot = static_cast<SDK::UCanvasPanel*>(panel)->AddChildToCanvas(text);
            if (!slot)
                return Attached::Never;
            SDK::FAnchors anchors;
            anchors.Minimum = Vec(0.0, 0.0);
            anchors.Maximum = Vec(0.0, 0.0);
            slot->SetAnchors(anchors);
            slot->SetAlignment(Vec(0.0, 0.0));
            slot->SetAutoSize(true);
        }

        g_hud.Set(hud);
        g_panel.Set(panel);
        g_text.Set(text);
        g_textOpacity = 1.0f;
        g_room        = room;
        g_shownText.clear();
        Place(text, panel, Vec(left, bottom));

        if (!g_loggedAttach)
        {
            LOG_INFO("Speedometer: under the drone HUD's distance readout, in %s %s at %.0f,%.0f",
                panel->Class ? panel->Class->GetName().c_str() : "?", panel->GetName().c_str(), left, bottom);
            g_loggedAttach = true;
        }
        return Attached::Yes;
    }

    // Shows `shown` under the readout. False when the speed has to be drawn
    // beside the crosshair instead.
    bool ShowUnderReadout(SDK::ACrCharacterPlayerBase* character, const char* shown, uint64_t now)
    {
        // All three are looked up again on every use. A text block left in a
        // panel that outlives the HUD is taken out; one whose panel or slot
        // has changed is no longer known to be ours, and is only forgotten.
        SDK::UCrUW_BuildingDroneHud* hud = g_hud.Get();
        SDK::UPanelWidget* panel = g_panel.Get();
        SDK::UTextBlock* text = g_text.Get();
        if (text && (!hud || !panel || !text->Slot || text->Slot->Parent != panel))
        {
            Detach();
            text = nullptr;
        }
        else if (!text)
        {
            Forget();
        }

        if (!text)
        {
            if (g_attachFailed)
                return false;

            if (!hud || (hud->PlayerCharacter && hud->PlayerCharacter != character))
            {
                g_hud.Reset();
                hud = nullptr;
                if (now >= g_nextFindMs)
                {
                    g_nextFindMs = now + kFindRetryMs;
                    hud = FindHud(character);
                    g_hud.Set(hud);
                }
            }

            // Only a HUD with no room, or none found by the end of the grace,
            // gives up until the next launch; a readout not laid out yet is
            // tried again on the next sample.
            const char* why = "the drone HUD was not found";
            const bool  late = now - g_droneSinceMs >= kAttachGraceMs;
            const Attached result = hud ? Attach(hud, &why) : late ? Attached::Never : Attached::NotYet;
            if (result == Attached::Yes)
            {
                text  = g_text.Get();
                panel = g_panel.Get();
            }
            else if (result == Attached::Never || late)
            {
                g_attachFailed = result == Attached::Never;
                if (!g_loggedFallback)
                {
                    LogFallback(why, hud ? hud->DistanceValueU : nullptr);
                    g_loggedFallback = true;
                }
            }
            if (!text || !panel)
                return false;
        }

        // The game hides the readout (a cutscene, a collapsed HUD): the speed
        // goes with it, and is not drawn anywhere else.
        float opacity = 1.0f;
        double left = 0.0, bottom = 0.0, scale = 1.0;
        if (!ReadoutShown(hud->DistanceValueU, panel, &opacity) || !DigitsIn(hud, panel, &left, &bottom, &scale))
        {
            ShowText(text, false);
            return true;
        }

        if (std::fabs(left - g_position.X) > kMoveEpsilon || std::fabs(bottom - g_position.Y) > kMoveEpsilon)
            Place(text, panel, Vec(left, bottom));

        // Text larger than the room checked at attach could grow a panel
        // that sizes to its content: give the place up.
        const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetLocalSize(text->GetCachedGeometry());
        if (size.X > g_room.X || size.Y > g_room.Y)
        {
            Detach();
            g_attachFailed = true;
            if (!g_loggedFallback)
            {
                LogFallback("the speed text is larger than the room checked under the readout", hud->DistanceValueU);
                g_loggedFallback = true;
            }
            return false;
        }

        if (!SameColour(text->ColorAndOpacity, hud->DistanceValueU->ColorAndOpacity))
            text->SetColorAndOpacity(hud->DistanceValueU->ColorAndOpacity);

        if (std::fabs(opacity - g_textOpacity) > 0.02f)
        {
            text->SetRenderOpacity(opacity);
            g_textOpacity = opacity;
        }

        if (g_shownText != shown)
        {
            g_shownText = shown;
            text->SetText(*TextFor(g_shownText));
        }

        ShowText(text, true);
        return true;
    }

    // Game thread. Nothing shows, and the next flight starts afresh.
    void HideAll()
    {
        Detach();
        HideFallback();
        g_smoothed     = -1.0f;
        g_attachFailed = false;
        g_wasInDrone   = false;
    }
}

void InitDroneSpeedometer(IPluginSelf* self)
{
    g_nextSampleMs = 0;
    g_nextFindMs   = 0;
    g_smoothed     = -1.0f;
    g_speed.store(-1.0f);

    if (!g_overlay.Register(self, "BetterDrone Speedometer", &RenderSpeed))
        LOG_WARN("Speedometer: the loader would not register the overlay widget, the speedometer will not show.");
}

void ShutdownDroneSpeedometer(IPluginSelf* self)
{
    g_speed.store(-1.0f);
    g_overlay.Unregister(self);

    // Off the game thread (process exit, older loaders) the text block is
    // left where it is, to go with the HUD.
    if (OnGameThread())
        Detach();
    else
        Forget();
    g_hud.Reset();
}

void TickDroneSpeedometer()
{
    if (!g_overlay.IsRegistered())
        return;

    const uint64_t now = GetTickCount64();
    if (now < g_nextSampleMs)
        return;
    g_nextSampleMs = now + kSampleMs;

    SDK::ACrCharacterPlayerBase* character = DroneConfig::Config::ReadSpeedometer() ? LocalPlayerCharacter() : nullptr;
    SDK::ACrCharacterDroneBase* drone =
        character && character->Status == SDK::EPlayerCharacterStatus::BuildingDrone ? character->BuildingDrone : nullptr;
    if (!drone || !ObjectRef<SDK::ACrCharacterDroneBase>(drone).Get())
    {
        HideAll();
        return;
    }
    if (!g_wasInDrone)
    {
        g_droneSinceMs = now;
        g_wasInDrone   = true;
    }

    if (IsGameMenuOpen())
    {
        if (SDK::UTextBlock* text = OurText())
            ShowText(text, false);
        HideFallback();
        return;
    }

    const SDK::FVector velocity = drone->GetVelocity();
    const float speed = static_cast<float>(
        std::sqrt(velocity.X * velocity.X + velocity.Y * velocity.Y + velocity.Z * velocity.Z));
    if (!std::isfinite(speed))
        return;
    g_smoothed = g_smoothed < 0.0f ? speed : g_smoothed + (speed - g_smoothed) * kSmoothing;

    char shown[48];
    FormatSpeed(g_smoothed, shown, sizeof(shown));
    if (ShowUnderReadout(character, shown, now))
    {
        HideFallback();
        return;
    }

    float scale = SDK::UWidgetLayoutLibrary::GetViewportScale(character);
    if (!(scale > 0.0f))
        scale = 1.0f;

    g_scale.store(scale);
    g_colour.store(PlayerMarkerColour());
    g_speed.store(g_smoothed);
    g_overlay.SetShown(true);
}
