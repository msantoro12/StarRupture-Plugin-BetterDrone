#include "drone_speedometer.h"
#include "drone_config.h"
#include "drone_heat_ring.h"
#include "drone_interact.h"
#include "drone_map_marker.h"
#include "drone_ui.h"
#include "object_ref.h"
#include "drone_fields.h"
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

    // The room each text needs, in ems of the readout's font: the number
    // wide enough for "99999", the unit for "km/h", both one line high.
    constexpr double kNumberWidthEm = 4.0;
    constexpr double kUnitWidthEm   = 3.0;
    constexpr double kTextHeightEm  = 1.5;

    // The width pinned on each of the HUD's digits, in ems of its font: wider
    // than the widest digit of the proportional font.
    constexpr double kDigitWidthEm = 0.7;

    // Feet in a metre, for the distance and height readouts when the panel's
    // unit is mph.
    constexpr double kFeetPerMetre = 3.280839895;

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

    // Where a HUD readout (distance or height) is, in a panel's own units: the
    // units digit's right edge, where the "m" label starts and how far it sits
    // below the digit's top (the unit's own left edge when there is no
    // label), the top and bottom edges of the row, and how much bigger it
    // draws there than in its own units.
    struct Digits
    {
        double right    = 0.0;
        double unitLeft = 0.0;
        double unitDy   = 0.0;
        double top      = 0.0;
        double bottom   = 0.0;
        double scale    = 1.0;
    };

    // One readout this plugin adds to the drone HUD: the number, right aligned
    // to the right edge of the HUD readout's digits, and its unit, left
    // aligned to where the HUD's own "m" label starts, as the HUD's rows lay
    // them out.
    struct Line
    {
        ObjectRef<SDK::UTextBlock> number;
        ObjectRef<SDK::UTextBlock> unit;
        float          opacity = 1.0f;
        SDK::FVector2D numberAt = {};
        SDK::FVector2D unitAt   = {};
        SDK::FVector2D numberRoom = {};   // the text sizes the panel was checked for
        SDK::FVector2D unitRoom   = {};
        std::string    shownNumber;
        std::string    shownUnit;
    };

    // The HUD's distance and height readouts: each is five elements (T, H, D,
    // U, M) in a row.
    enum class Row { Distance, Height };
    constexpr int kRows      = 2;
    constexpr int kGameElems = 5;

    // The speed, under the drone HUD's distance readout, and the distance and
    // height in feet, over their readouts while the game's digits are hidden.
    // The panel they were added to, and the HUD. Looked up again on every use.
    ObjectRef<SDK::UCrUW_BuildingDroneHud> g_hud;
    ObjectRef<SDK::UPanelWidget>           g_panel;
    Line g_speedLine;

    // What the feet line of one readout needs between samples.
    struct FeetRow
    {
        Line           line;
        bool           failed = false;   // until the drone is next launched
        bool           logged = false;   // one log line per load
        bool           hidden = false;   // the game's elements are hidden, with the opacity each had
        float          opacity[kGameElems] = {};
        bool           pinned = false;   // the digits have a fixed width, with what each had
        float          minWidth[kGameElems] = {};
        SDK::ETextJustify justify[kGameElems] = {};
        float          cell = 0.0f;      // the pinned width of one digit
        float          unitsWidth = 0.0f;   // the width last given the units digit
        bool           measured = false;
        Digits         digits;           // where the readout was when last measured
        SDK::FVector2D measuredIn = {};  // the panel's size then
        float          measuredOpacity = 0.0f;   // and the opacity it was shown at
    };
    FeetRow g_rows[kRows];

    bool g_attachFailed   = false;   // until the drone is next launched
    bool g_loggedAttach   = false;   // one line per path, per load
    bool g_loggedFallback = false;

    // One FText per string ever shown, built once and kept, as the loader's
    // menu rows do: dropping an FText means releasing its refcounted text
    // data by hand. Speeds and distances are rounded to three significant
    // figures, which keeps the set to a few thousand strings at most.
    std::unordered_map<std::string, SDK::FText> g_texts;

    enum class Attached { Yes, NotYet, Never };

    // `value` rounded to three significant figures.
    long ThreeFigures(float value)
    {
        const float step = value >= 10000.0f ? 100.0f : value >= 1000.0f ? 10.0f : 1.0f;
        return static_cast<long>(std::round(value / step) * step);
    }

    // `cmPerSec` in the panel's unit: the number and the unit's name.
    void SpeedParts(float cmPerSec, char* number, size_t numberSize, char* unit, size_t unitSize)
    {
        const float inUnit = (std::min)(SpeedInPanelUnit(cmPerSec, unit, static_cast<int>(unitSize)), 999999.0f);
        snprintf(number, numberSize, "%ld", ThreeFigures(inUnit));
    }

    void FormatSpeed(float cmPerSec, char* out, size_t size)
    {
        char number[32] = {};
        char unit[16]   = {};
        SpeedParts(cmPerSec, number, sizeof(number), unit, sizeof(unit));
        snprintf(out, size, "%s %s", number, unit);
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
            if (!ObjectRef<SDK::UCrUW_BuildingDroneHud>(hud).Get() || !DroneFields::DistanceU(hud))
                continue;

            if (DroneFields::HudPilot(hud) == character)
                return hud;
            if (!DroneFields::HudPilot(hud) && !unowned)
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
    // the tree when null), and the opacity the widgets above it give it. The
    // readout's own opacity is left out: this plugin sets it to zero while
    // it shows the distance in feet.
    bool ReadoutShown(SDK::UWidget* readout, SDK::UWidget* panel, float* opacity)
    {
        float product = 1.0f;
        SDK::UWidget* widget = readout;
        for (int depth = 0; widget && widget != panel && depth < kMaxTreeDepth; ++depth)
        {
            if (!IsShown(widget))
                return false;
            if (widget != readout)
                product *= widget->RenderOpacity;
            widget = ParentOf(widget);
        }
        *opacity = product;
        return product > 0.01f;
    }

    // The element's edges in `panel`'s units. False until it has been laid out.
    bool EdgesIn(SDK::UWidget* elem, SDK::UWidget* panel, SDK::FVector2D* topLeft, SDK::FVector2D* bottomRight)
    {
        if (!elem || !IsShown(elem))
            return false;

        const SDK::FGeometry panelGeo = panel->GetCachedGeometry();
        const SDK::FGeometry geo      = elem->GetCachedGeometry();
        const SDK::FVector2D size     = SDK::USlateBlueprintLibrary::GetLocalSize(geo);
        if (SDK::USlateBlueprintLibrary::GetLocalSize(panelGeo).X <= 0.0 || size.X <= 0.0 || size.Y <= 0.0)
            return false;

        *topLeft = SDK::USlateBlueprintLibrary::AbsoluteToLocal(
            panelGeo, SDK::USlateBlueprintLibrary::LocalToAbsolute(geo, Vec(0.0, 0.0)));
        *bottomRight = SDK::USlateBlueprintLibrary::AbsoluteToLocal(
            panelGeo, SDK::USlateBlueprintLibrary::LocalToAbsolute(geo, size));
        return true;
    }

    // The HUD's elements of one readout, T H D U M.
    void RowElems(SDK::UCrUW_BuildingDroneHud* hud, Row row, SDK::UTextBlock** out)
    {
        if (row == Row::Distance)
        {
            out[0] = DroneFields::ValueOr(DroneFields::HudDistanceT, hud);
            out[1] = DroneFields::ValueOr(DroneFields::HudDistanceH, hud);
            out[2] = DroneFields::ValueOr(DroneFields::HudDistanceD, hud);
            out[3] = DroneFields::DistanceU(hud);
            out[4] = DroneFields::ValueOr(DroneFields::HudDistanceM, hud);
        }
        else
        {
            out[0] = DroneFields::ValueOr(DroneFields::HudHeightT, hud);
            out[1] = DroneFields::ValueOr(DroneFields::HudHeightH, hud);
            out[2] = DroneFields::ValueOr(DroneFields::HudHeightD, hud);
            out[3] = DroneFields::ValueOr(DroneFields::HudHeightU, hud);
            out[4] = DroneFields::ValueOr(DroneFields::HudHeightM, hud);
        }
    }

    // False until the readout has been laid out. The units digit is the
    // rightmost of the digits, so its right edge keeps its place as the
    // value gains or loses a digit.
    bool DigitsIn(SDK::UTextBlock* const* elems, SDK::UWidget* panel, Digits* out)
    {
        SDK::UTextBlock* const digit = elems[3];
        SDK::FVector2D topLeft, bottomRight;
        if (!EdgesIn(digit, panel, &topLeft, &bottomRight))
            return false;

        const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetLocalSize(digit->GetCachedGeometry());
        out->scale    = (bottomRight.Y - topLeft.Y) / size.Y;
        out->right    = bottomRight.X;
        out->top      = topLeft.Y;
        out->bottom   = bottomRight.Y;
        out->unitLeft = bottomRight.X;
        out->unitDy   = 0.0;

        SDK::FVector2D labelTopLeft, labelBottomRight;
        if (EdgesIn(elems[4], panel, &labelTopLeft, &labelBottomRight))
        {
            out->unitLeft = labelTopLeft.X;
            out->unitDy   = labelTopLeft.Y - topLeft.Y;
            out->top      = (std::min)(out->top, labelTopLeft.Y);
            out->bottom   = (std::max)(out->bottom, labelBottomRight.Y);
        }
        return true;
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

    // The text block, while it is still the one added to the panel.
    SDK::UTextBlock* Ours(const ObjectRef<SDK::UTextBlock>& ref)
    {
        SDK::UTextBlock*   text  = ref.Get();
        SDK::UPanelWidget* panel = g_panel.Get();
        return text && panel && text->Slot && text->Slot->Parent == panel ? text : nullptr;
    }

    bool Intact(const Line& line) { return Ours(line.number) && Ours(line.unit); }

    void ForgetLine(Line& line)
    {
        line.number.Reset();
        line.unit.Reset();
        line.opacity = 1.0f;
        line.numberAt = {};
        line.unitAt   = {};
        line.numberRoom = {};
        line.unitRoom   = {};
        line.shownNumber.clear();
        line.shownUnit.clear();
    }

    void Forget()
    {
        ForgetLine(g_speedLine);
        for (FeetRow& row : g_rows)
        {
            ForgetLine(row.line);
            row.measured = false;
        }
        g_panel.Reset();
    }

    // Hides a readout's game elements by their opacity, which keeps them in
    // the layout and keeps the game updating their text, or gives them back
    // the opacity they had.
    void HideGameDigits(SDK::UCrUW_BuildingDroneHud* hud, Row which, bool hide)
    {
        FeetRow& row = g_rows[static_cast<int>(which)];
        if (!hide && !row.hidden)
            return;

        SDK::UTextBlock* elems[kGameElems] = {};
        if (hud)
            RowElems(hud, which, elems);

        for (int i = 0; i < kGameElems; ++i)
        {
            if (!elems[i])
                continue;
            if (hide)
            {
                if (!row.hidden)
                    row.opacity[i] = elems[i]->RenderOpacity;
                if (elems[i]->RenderOpacity != 0.0f)
                    elems[i]->SetRenderOpacity(0.0f);
            }
            else
            {
                elems[i]->SetRenderOpacity(row.opacity[i]);
            }
        }
        row.hidden = hide;
    }

    // The padding a digit's slot puts around it in a horizontal box, which a
    // collapsed digit gives up with its width.
    float SlotGap(const SDK::UWidget* widget)
    {
        if (!widget->Slot || !widget->Slot->IsA(SDK::UHorizontalBoxSlot::StaticClass()))
            return 0.0f;
        const SDK::FMargin& padding = static_cast<const SDK::UHorizontalBoxSlot*>(widget->Slot)->Padding;
        return padding.Left + padding.Right;
    }

    // Gives the units digit the room of the leading digits the game has
    // collapsed (it collapses the thousands, hundreds and tens below 1000,
    // 100 and 10 on every tick), so the row keeps four digits' width.
    void HoldRowWidth(SDK::UTextBlock* const* elems, FeetRow& row)
    {
        if (!elems[3] || !(row.cell > 0.0f))
            return;
        float width = row.cell;
        for (int i = 0; i < 3; ++i)
            if (elems[i] && elems[i]->Visibility == SDK::ESlateVisibility::Collapsed)
                width += row.cell + SlotGap(elems[i]);
        if (width != row.unitsWidth)
        {
            elems[3]->SetMinDesiredWidth(width);
            row.unitsWidth = width;
        }
    }

    // Gives the readout's digits a fixed width, the units digit right aligned
    // in the room of the digits the game collapses. The game lays the digits
    // out in a box with the reticle, so a digit changing width (a "1" for an
    // "8"), or one more digit shown, re-centred the box and moved the
    // reticle. False when nothing changed.
    bool PinDigits(SDK::UCrUW_BuildingDroneHud* hud, Row which)
    {
        FeetRow& row = g_rows[static_cast<int>(which)];
        if (row.pinned)
            return false;

        SDK::UTextBlock* elems[kGameElems] = {};
        RowElems(hud, which, elems);
        if (!elems[3] || !(elems[3]->Font.Size > 0.0f))
            return false;
        row.cell = static_cast<float>(kDigitWidthEm * elems[3]->Font.Size * 96.0 / 72.0);
        for (int i = 0; i < 4; ++i)
        {
            if (!elems[i])
                continue;
            row.minWidth[i] = elems[i]->MinDesiredWidth;
            row.justify[i]  = elems[i]->Justification;
            elems[i]->SetMinDesiredWidth(row.cell);
            elems[i]->SetJustification(i == 3 ? SDK::ETextJustify::Right : SDK::ETextJustify::Center);
        }
        row.unitsWidth = row.cell;
        HoldRowWidth(elems, row);
        row.pinned   = true;
        row.measured = false;
        return true;
    }

    // Every tick: the room of the digits the game has collapsed since the
    // last one.
    void HoldPinnedRows()
    {
        SDK::UCrUW_BuildingDroneHud* hud = g_hud.Get();
        if (!hud)
            return;
        for (int i = 0; i < kRows; ++i)
        {
            FeetRow& row = g_rows[i];
            if (!row.pinned)
                continue;
            SDK::UTextBlock* elems[kGameElems] = {};
            RowElems(hud, static_cast<Row>(i), elems);
            HoldRowWidth(elems, row);
        }
    }

    // Gives the digits the width and alignment they had.
    void UnpinDigits(SDK::UCrUW_BuildingDroneHud* hud, Row which)
    {
        FeetRow& row = g_rows[static_cast<int>(which)];
        if (!row.pinned)
            return;

        SDK::UTextBlock* elems[kGameElems] = {};
        if (hud)
            RowElems(hud, which, elems);
        for (int i = 0; i < 4; ++i)
        {
            if (!elems[i])
                continue;
            elems[i]->SetMinDesiredWidth(row.minWidth[i]);
            elems[i]->SetJustification(row.justify[i]);
        }
        row.pinned   = false;
        row.measured = false;
    }

    // Takes our text blocks out of the HUD, if they are still ours, and gives
    // the game its digits back.
    void Detach()
    {
        for (int i = 0; i < kRows; ++i)
        {
            UnpinDigits(g_hud.Get(), static_cast<Row>(i));
            HideGameDigits(g_hud.Get(), static_cast<Row>(i), false);
            g_rows[i].hidden = false;
        }
        for (Line* line : { &g_speedLine, &g_rows[0].line, &g_rows[1].line })
        {
            if (SDK::UTextBlock* text = Ours(line->number))
                text->RemoveFromParent();
            if (SDK::UTextBlock* text = Ours(line->unit))
                text->RemoveFromParent();
        }
        Forget();
    }

    void ShowText(SDK::UTextBlock* text, bool shown)
    {
        if (IsShown(text) != shown)
            text->SetVisibility(shown ? SDK::ESlateVisibility::HitTestInvisible : SDK::ESlateVisibility::Collapsed);
    }

    void ShowLine(Line& line, bool shown)
    {
        if (SDK::UTextBlock* text = Ours(line.number))
            ShowText(text, shown);
        if (SDK::UTextBlock* text = Ours(line.unit))
            ShowText(text, shown);
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

    // Where a row of the readout goes, in the form `panel` takes it: for an
    // overlay slot, the number's gap from the panel's right edge, the unit's
    // gap from its left edge, and both tops as top padding; for a canvas
    // slot, the positions themselves.
    void Anchors(SDK::UPanelWidget* panel, const Digits& d, double rowTop, SDK::FVector2D* number, SDK::FVector2D* unit)
    {
        *number = Vec(d.right, rowTop);
        *unit   = Vec(d.unitLeft, rowTop + d.unitDy);
        if (IsOverlay(panel))
            number->X = SDK::USlateBlueprintLibrary::GetLocalSize(panel->GetCachedGeometry()).X - d.right;
    }

    void Place(Line& line, SDK::UPanelWidget* panel, const SDK::FVector2D& number, const SDK::FVector2D& unit)
    {
        SDK::UTextBlock* const texts[] = { Ours(line.number), Ours(line.unit) };
        const SDK::FVector2D   at[]    = { number, unit };
        for (int i = 0; i < 2; ++i)
        {
            if (IsOverlay(panel))
            {
                SDK::FMargin padding{};
                (i == 0 ? padding.Right : padding.Left) = static_cast<float>(at[i].X);
                padding.Top = static_cast<float>(at[i].Y);
                static_cast<SDK::UOverlaySlot*>(texts[i]->Slot)->SetPadding(padding);
            }
            else
            {
                static_cast<SDK::UCanvasPanelSlot*>(texts[i]->Slot)->SetPosition(at[i]);
            }
        }
        line.numberAt = number;
        line.unitAt   = unit;
    }

    // A text block styled like `source`, added to `panel` and aligned to the
    // right (the number) or the left (the unit) of its slot; null if the
    // panel would not take it.
    SDK::UTextBlock* AddText(SDK::UPanelWidget* panel, SDK::UTextBlock* source, double scale, bool rightAligned)
    {
        SDK::UObject* created = SDK::UGameplayStatics::SpawnObject(SDK::UTextBlock::StaticClass(), panel->Outer);
        if (!created || !created->IsA(SDK::UTextBlock::StaticClass()))
            return nullptr;
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
        text->SetJustification(rightAligned ? SDK::ETextJustify::Right : SDK::ETextJustify::Left);

        if (IsOverlay(panel))
        {
            SDK::UOverlaySlot* slot = static_cast<SDK::UOverlay*>(panel)->AddChildToOverlay(text);
            if (!slot)
                return nullptr;
            slot->SetHorizontalAlignment(rightAligned ? SDK::EHorizontalAlignment::HAlign_Right
                                                      : SDK::EHorizontalAlignment::HAlign_Left);
            slot->SetVerticalAlignment(SDK::EVerticalAlignment::VAlign_Top);
        }
        else
        {
            SDK::UCanvasPanelSlot* slot = static_cast<SDK::UCanvasPanel*>(panel)->AddChildToCanvas(text);
            if (!slot)
                return nullptr;
            SDK::FAnchors anchors;
            anchors.Minimum = Vec(0.0, 0.0);
            anchors.Maximum = Vec(0.0, 0.0);
            slot->SetAnchors(anchors);
            slot->SetAlignment(Vec(rightAligned ? 1.0 : 0.0, 0.0));
            slot->SetAutoSize(true);
        }
        return text;
    }

    // The unit takes its look from the readout's "m" label, or the digit when
    // there is none.
    SDK::UTextBlock* UnitSource(SDK::UTextBlock* const* elems)
    {
        return elems[4] ? elems[4] : elems[3];
    }

    // The room each text of a readout needs, in the panel's units.
    void RoomsFor(SDK::UTextBlock* digit, const Digits& d, SDK::FVector2D* number, SDK::FVector2D* unit)
    {
        const double em = digit->Font.Size * d.scale * 96.0 / 72.0;
        *number = Vec(kNumberWidthEm * em, kTextHeightEm * em);
        *unit   = Vec(kUnitWidthEm * em, kTextHeightEm * em);
    }

    // Whether the texts of a row at `rowTop` stay inside the panel's current
    // bounds, so the panel's own size, and everything laid out around it, is
    // unchanged.
    bool HasRoom(const Digits& d, double rowTop, const SDK::FVector2D& number, const SDK::FVector2D& unit,
                 const SDK::FVector2D& bounds)
    {
        return d.right - number.X >= 0.0 && d.top >= 0.0 && d.unitLeft + unit.X <= bounds.X &&
               rowTop + number.Y <= bounds.Y;
    }

    // Adds a line's two text blocks to `panel`, placed for a row at `rowTop`.
    bool AttachLine(Line& line, SDK::UTextBlock* const* elems, SDK::UPanelWidget* panel, const Digits& d, double rowTop)
    {
        SDK::UTextBlock* number = AddText(panel, elems[3], d.scale, true);
        SDK::UTextBlock* unit   = number ? AddText(panel, UnitSource(elems), d.scale, false) : nullptr;
        if (!unit)
        {
            if (number)
                number->RemoveFromParent();
            return false;
        }

        line.number.Set(number);
        line.unit.Set(unit);
        line.opacity = 1.0f;
        line.shownNumber.clear();
        line.shownUnit.clear();
        RoomsFor(elems[3], d, &line.numberRoom, &line.unitRoom);

        SDK::FVector2D numberAt, unitAt;
        Anchors(panel, d, rowTop, &numberAt, &unitAt);
        Place(line, panel, numberAt, unitAt);
        return true;
    }

    // A readout's place in `panel`. It is measured when the readout is first
    // seen, again when the panel's size changes, and while its opacity is
    // still changing (fading in); otherwise the place is kept. The digits the
    // game shows change the row's width with their value, and following that
    // appears to have moved the text with every change. False until the
    // readout has been laid out.
    bool PlaceOf(FeetRow& row, SDK::UTextBlock* const* elems, SDK::UWidget* panel, float opacity, Digits* out)
    {
        const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetLocalSize(panel->GetCachedGeometry());
        if (size.X <= 0.0)
            return false;

        if (!row.measured || std::fabs(opacity - row.measuredOpacity) > 0.005f || size.X != row.measuredIn.X ||
            size.Y != row.measuredIn.Y)
        {
            if (!DigitsIn(elems, panel, &row.digits))
                return false;
            row.measured        = true;
            row.measuredIn      = size;
            row.measuredOpacity = opacity;
        }
        *out = row.digits;
        return true;
    }

    // Adds the speed, in two text blocks styled like the readout's units
    // digit and "m" label, to the nearest overlay or canvas panel above the
    // readout that has room for them under the digits: the number right
    // aligned to the digits' right edge, the unit left aligned to where the
    // label starts. Only those two panels place a child without moving the
    // ones already there: a horizontal or vertical box would shift the
    // readout or the rows under it, and a size box or border holds a single
    // child. Room means the texts stay inside the panel's current bounds, so
    // the panel's own size, and everything laid out around it, is unchanged.
    Attached Attach(SDK::UCrUW_BuildingDroneHud* hud, const char** why)
    {
        *why = "the distance readout is hidden or not laid out yet";
        SDK::UTextBlock* elems[kGameElems] = {};
        RowElems(hud, Row::Distance, elems);
        SDK::UTextBlock* source = elems[3];
        float opacity = 1.0f;
        if (!source || !ReadoutShown(source, nullptr, &opacity))
            return Attached::NotYet;

        const SDK::FVector2D sourceSize = SDK::USlateBlueprintLibrary::GetLocalSize(source->GetCachedGeometry());
        if (sourceSize.X <= 0.0 || sourceSize.Y <= 0.0)
            return Attached::NotYet;

        SDK::UPanelWidget* panel = nullptr;
        Digits d;
        SDK::UWidget* widget = ParentOf(source);
        for (int depth = 0; widget && depth < kMaxTreeDepth; ++depth, widget = ParentOf(widget))
        {
            if (!IsOverlay(widget) && !IsCanvas(widget))
                continue;
            if (!DigitsIn(elems, widget, &d))
                return Attached::NotYet;

            SDK::FVector2D numberRoom, unitRoom;
            RoomsFor(source, d, &numberRoom, &unitRoom);
            if (HasRoom(d, d.bottom, numberRoom, unitRoom, SDK::USlateBlueprintLibrary::GetLocalSize(widget->GetCachedGeometry())))
            {
                panel = static_cast<SDK::UPanelWidget*>(widget);
                break;
            }
        }
        *why = "no overlay or canvas panel above the distance readout has room for the text";
        if (!panel)
            return Attached::Never;

        g_panel.Set(panel);
        *why = "the text blocks could not be created";
        if (!AttachLine(g_speedLine, elems, panel, d, d.bottom))
        {
            g_panel.Reset();
            return Attached::Never;
        }

        g_hud.Set(hud);

        if (!g_loggedAttach)
        {
            LOG_INFO("Speedometer: under the drone HUD's distance readout, in %s %s, digits' right edge at %.0f, unit at %.0f, bottom at %.0f",
                panel->Class ? panel->Class->GetName().c_str() : "?", panel->GetName().c_str(), d.right, d.unitLeft, d.bottom);
            g_loggedAttach = true;
        }
        return Attached::Yes;
    }

    // The text in one of the HUD's elements; empty if it has none.
    std::string TextOf(const SDK::UTextBlock* elem)
    {
        return elem && elem->Text.TextData ? elem->Text.ToString() : std::string();
    }

    // The value a readout shows, in whole metres, from its digits and "m"
    // label. False when they read as anything else.
    bool GameMetres(SDK::UTextBlock* const* elems, Row which, long* metres)
    {
        std::string digits;
        for (int i = 0; i < 4; ++i)
        {
            if (!elems[i])
            {
                if (i == 3)
                    return false;
                continue;
            }
            for (char c : TextOf(elems[i]))
            {
                if (c >= '0' && c <= '9')
                    digits += c;
                else if (c != ' ')
                    return false;
            }
        }
        const std::string label = TextOf(elems[4]);
        if (digits.empty() || (!label.empty() && label[0] >= '0' && label[0] <= '9'))
            return false;

        FeetRow& row = g_rows[static_cast<int>(which)];
        if (!row.logged)
        {
            LOG_INFO("Speedometer: %s digits '%s' '%s' '%s' '%s', label '%s'", which == Row::Distance ? "distance" : "height",
                TextOf(elems[0]).c_str(), TextOf(elems[1]).c_str(), TextOf(elems[2]).c_str(), TextOf(elems[3]).c_str(),
                label.c_str());
            row.logged = true;
        }
        *metres = std::strtol(digits.c_str(), nullptr, 10);
        return true;
    }

    // Brings a line up to date: its place, look and text.
    void UpdateLine(Line& line, SDK::UTextBlock* const* elems, SDK::UPanelWidget* panel, const Digits& d, double rowTop,
                    float opacity, const char* number, const char* unit)
    {
        SDK::UTextBlock* numberText = Ours(line.number);
        SDK::UTextBlock* unitText   = Ours(line.unit);

        SDK::FVector2D numberAt, unitAt;
        Anchors(panel, d, rowTop, &numberAt, &unitAt);
        if (std::fabs(numberAt.X - line.numberAt.X) > kMoveEpsilon || std::fabs(numberAt.Y - line.numberAt.Y) > kMoveEpsilon ||
            std::fabs(unitAt.X - line.unitAt.X) > kMoveEpsilon || std::fabs(unitAt.Y - line.unitAt.Y) > kMoveEpsilon)
            Place(line, panel, numberAt, unitAt);

        if (!SameColour(numberText->ColorAndOpacity, elems[3]->ColorAndOpacity))
            numberText->SetColorAndOpacity(elems[3]->ColorAndOpacity);
        if (!SameColour(unitText->ColorAndOpacity, UnitSource(elems)->ColorAndOpacity))
            unitText->SetColorAndOpacity(UnitSource(elems)->ColorAndOpacity);

        if (std::fabs(opacity - line.opacity) > 0.02f)
        {
            numberText->SetRenderOpacity(opacity);
            unitText->SetRenderOpacity(opacity);
            line.opacity = opacity;
        }

        if (line.shownNumber != number)
        {
            line.shownNumber = number;
            numberText->SetText(*TextFor(line.shownNumber));
        }
        if (line.shownUnit != unit)
        {
            line.shownUnit = unit;
            unitText->SetText(*TextFor(line.shownUnit));
        }
        ShowLine(line, true);
    }

    // Whether either text of a line is larger than the room checked for it.
    bool Oversize(const Line& line)
    {
        const SDK::FVector2D n = SDK::USlateBlueprintLibrary::GetLocalSize(Ours(line.number)->GetCachedGeometry());
        const SDK::FVector2D u = SDK::USlateBlueprintLibrary::GetLocalSize(Ours(line.unit)->GetCachedGeometry());
        return n.X > line.numberRoom.X || n.Y > line.numberRoom.Y || u.X > line.unitRoom.X || u.Y > line.unitRoom.Y;
    }

    // While the panel's unit is mph a readout (the distance, the height)
    // shows in feet, in text of ours over the HUD's digits, which are hidden;
    // otherwise, or when the digits cannot be read, the HUD's own metres show.
    void ShowFeet(SDK::UCrUW_BuildingDroneHud* hud, Row which, SDK::UPanelWidget* panel)
    {
        FeetRow& row = g_rows[static_cast<int>(which)];
        SDK::UTextBlock* elems[kGameElems] = {};
        RowElems(hud, which, elems);

        // This readout's own visibility and opacity, as the distance's are checked.
        float rowOpacity = 1.0f;
        long metres = 0;
        Digits d;
        if (DistanceInFeet() && !row.failed && elems[3] && ReadoutShown(elems[3], panel, &rowOpacity) &&
            PlaceOf(row, elems, panel, rowOpacity, &d) && GameMetres(elems, which, &metres))
        {
            if (!Intact(row.line))
            {
                SDK::FVector2D numberRoom, unitRoom;
                RoomsFor(elems[3], d, &numberRoom, &unitRoom);
                if (!HasRoom(d, d.top, numberRoom, unitRoom, SDK::USlateBlueprintLibrary::GetLocalSize(panel->GetCachedGeometry())) ||
                    !AttachLine(row.line, elems, panel, d, d.top))
                    row.failed = true;
            }

            if (Intact(row.line))
            {
                HideGameDigits(hud, which, true);
                char number[32];
                snprintf(number, sizeof(number), "%ld", ThreeFigures(static_cast<float>(metres * kFeetPerMetre)));
                UpdateLine(row.line, elems, panel, d, d.top, rowOpacity, number, "ft");
                return;
            }
        }
        ShowLine(row.line, false);
        HideGameDigits(hud, which, false);
    }

    // Shows the speed under the readout. False when it has to be drawn
    // beside the crosshair instead.
    bool ShowUnderReadout(SDK::ACrCharacterPlayerBase* character, const char* number, const char* unit, uint64_t now)
    {
        // All three are looked up again on every use. Text blocks left in a
        // panel that outlives the HUD are taken out; ones whose panel or slot
        // has changed are no longer known to be ours, and are only forgotten.
        SDK::UCrUW_BuildingDroneHud* hud = g_hud.Get();
        SDK::UPanelWidget* panel = g_panel.Get();
        const bool known = g_speedLine.number.Get() || g_speedLine.unit.Get();
        if (known && (!hud || !panel || !Intact(g_speedLine)))
        {
            Detach();
            hud = nullptr;
            panel = nullptr;
        }
        else if (!known)
        {
            Forget();
        }

        if (!Intact(g_speedLine))
        {
            if (g_attachFailed)
                return false;

            if (!hud || (DroneFields::HudPilot(hud) && DroneFields::HudPilot(hud) != character))
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
                panel = g_panel.Get();
            }
            else if (result == Attached::Never || late)
            {
                g_attachFailed = result == Attached::Never;
                if (!g_loggedFallback)
                {
                    LogFallback(why, hud ? DroneFields::DistanceU(hud) : nullptr);
                    g_loggedFallback = true;
                }
            }
            if (!Intact(g_speedLine) || !panel)
                return false;
        }

        // The digits take their fixed width first; their place is measured on
        // the next sample, once the game has laid them out again.
        if (PinDigits(hud, Row::Distance) | PinDigits(hud, Row::Height))
            return true;

        // The game hides the readout (a cutscene, a collapsed HUD): the speed
        // goes with it, and is not drawn anywhere else.
        float opacity = 1.0f;
        Digits d;
        SDK::UTextBlock* elems[kGameElems] = {};
        RowElems(hud, Row::Distance, elems);
        if (!ReadoutShown(elems[3], panel, &opacity) || !PlaceOf(g_rows[0], elems, panel, opacity, &d))
        {
            ShowLine(g_speedLine, false);
            for (FeetRow& row : g_rows)
                ShowLine(row.line, false);
            return true;
        }

        // Text larger than the room checked at attach could grow a panel
        // that sizes to its content: give the place up.
        if (Oversize(g_speedLine) || (Intact(g_rows[0].line) && Oversize(g_rows[0].line)) ||
            (Intact(g_rows[1].line) && Oversize(g_rows[1].line)))
        {
            Detach();
            g_attachFailed = true;
            if (!g_loggedFallback)
            {
                LogFallback("the speed text is larger than the room checked under the readout", DroneFields::DistanceU(hud));
                g_loggedFallback = true;
            }
            return false;
        }

        UpdateLine(g_speedLine, elems, panel, d, d.bottom, opacity, number, unit);
        ShowFeet(hud, Row::Distance, panel);
        ShowFeet(hud, Row::Height, panel);
        return true;
    }

    // Game thread. Nothing shows, and the next flight starts afresh.
    void HideAll()
    {
        Detach();
        HideFallback();
        g_smoothed     = -1.0f;
        g_attachFailed = false;
        for (FeetRow& row : g_rows)
            row.failed = false;
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

    HoldPinnedRows();

    const uint64_t now = GetTickCount64();
    if (now < g_nextSampleMs)
        return;
    g_nextSampleMs = now + kSampleMs;

    SDK::ACrCharacterPlayerBase* character = DroneConfig::Config::ReadSpeedometer() ? LocalPlayerCharacter() : nullptr;
    SDK::ACrCharacterDroneBase* drone =
        character && DroneFields::InDrone(character) ? DroneFields::ValueOr(DroneFields::BuildingDrone, character) : nullptr;
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
        ShowLine(g_speedLine, false);
        for (FeetRow& row : g_rows)
            ShowLine(row.line, false);
        HideFallback();
        return;
    }

    const SDK::FVector velocity = drone->GetVelocity();
    const float speed = static_cast<float>(
        std::sqrt(velocity.X * velocity.X + velocity.Y * velocity.Y + velocity.Z * velocity.Z));
    if (!std::isfinite(speed))
        return;
    g_smoothed = g_smoothed < 0.0f ? speed : g_smoothed + (speed - g_smoothed) * kSmoothing;

    char number[32] = {};
    char unit[16]   = {};
    SpeedParts(g_smoothed, number, sizeof(number), unit, sizeof(unit));
    if (ShowUnderReadout(character, number, unit, now))
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
