#include "drone_speedometer.h"
#include "drone_config.h"
#include "drone_interact.h"
#include "drone_ui.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <ChimeraUI_classes.hpp>
#include <Engine_classes.hpp>
#include <UMG_classes.hpp>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <unordered_map>

namespace
{
    // The text and its place are refreshed this often, never every frame.
    constexpr uint64_t kUpdateMs = 100;

    // A drone HUD that could not be found is looked for again this often,
    // for as long as the drone is out.
    constexpr uint64_t kFindRetryMs = 2000;

    // How long the drone is out before a HUD still not found is logged: the
    // game may build it a moment after the drone launches.
    constexpr uint64_t kMissingGraceMs = 3000;

    // Each 10 Hz sample's weight in the shown speed. Enough to hide the
    // frame-to-frame jitter, little enough to follow a boost.
    constexpr double kSmoothing = 0.5;

    // Moves smaller than this, in canvas units, are not applied.
    constexpr double kMoveEpsilon = 0.25;

    // Guards the walk up the widget tree against a loop.
    constexpr int kMaxTreeDepth = 64;

    ObjectRef<SDK::UCrUW_BuildingDroneHud> g_hud;
    ObjectRef<SDK::UTextBlock>             g_text;

    std::atomic<DWORD> g_gameThreadId{ 0 };

    // Game thread only.
    uint64_t       g_nextUpdateMs  = 0;
    uint64_t       g_nextFindMs    = 0;
    uint64_t       g_droneSinceMs  = 0;
    bool           g_wasInDrone    = false;
    bool           g_loggedMissing = false; // one warning until the next attach
    bool           g_shown         = false;
    double         g_smoothed      = -1.0; // cm/s; below 0 until a flight's first sample
    double         g_alignX        = 0.0;  // 0 left, 0.5 centre, 1 right, from the readout's justification
    SDK::FVector2D g_position      = {};
    std::wstring   g_shownText;

    // One FText per string ever shown, built once and kept, as the loader's
    // menu rows do: dropping an FText means releasing its refcounted text
    // data by hand. Speeds are rounded to three significant figures, which
    // keeps the set of strings to a few thousand at most.
    std::unordered_map<std::wstring, SDK::FText> g_texts;

    SDK::FVector2D Vec(double x, double y)
    {
        SDK::FVector2D v;
        v.X = x;
        v.Y = y;
        return v;
    }

    // The drone HUD that belongs to `character`. A scan of the object table,
    // run only while no HUD is attached, at most every kFindRetryMs.
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
            if (!ObjectRef<SDK::UCrUW_BuildingDroneHud>(hud).Get())
                continue; // being destroyed

            if (hud->PlayerCharacter == character)
                return hud;
            if (!hud->PlayerCharacter && !unowned)
                unowned = hud;
        }
        return unowned;
    }

    // The nearest canvas panel that holds `widget`. A user widget's tree ends
    // at its root; the walk then carries on from the user widget itself, in
    // the tree that holds it.
    SDK::UCanvasPanel* FindCanvas(SDK::UWidget* widget)
    {
        for (int depth = 0; widget && depth < kMaxTreeDepth; ++depth)
        {
            SDK::UPanelWidget* parent = widget->GetParent();
            if (parent)
            {
                if (parent->IsA(SDK::UCanvasPanel::StaticClass()))
                    return static_cast<SDK::UCanvasPanel*>(parent);
                widget = parent;
                continue;
            }

            SDK::UObject* tree  = widget->Outer;
            SDK::UObject* owner = tree && tree->IsA(SDK::UWidgetTree::StaticClass()) ? tree->Outer : nullptr;
            widget = owner && owner->IsA(SDK::UUserWidget::StaticClass()) ? static_cast<SDK::UWidget*>(owner) : nullptr;
        }
        return nullptr;
    }

    // The readout the speed sits under: the distance digits.
    void DistanceElems(SDK::UCrUW_BuildingDroneHud* hud, SDK::UTextBlock* (&out)[5])
    {
        out[0] = hud->DistanceValueT;
        out[1] = hud->DistanceValueH;
        out[2] = hud->DistanceValueD;
        out[3] = hud->DistanceValueU;
        out[4] = hud->DistanceValueM;
    }

    // Where the text goes, in the canvas's own units: under the bottom edge of
    // the visible distance digits, at their left edge, centre or right edge
    // as the readout is justified. False until the HUD has been laid out.
    bool PlaceUnderReadout(SDK::UCrUW_BuildingDroneHud* hud, SDK::UCanvasPanel* canvas, SDK::FVector2D* out)
    {
        const SDK::FGeometry canvasGeo = canvas->GetCachedGeometry();
        if (SDK::USlateBlueprintLibrary::GetLocalSize(canvasGeo).X <= 0.0)
            return false;

        SDK::UTextBlock* elems[5];
        DistanceElems(hud, elems);

        bool   any = false;
        double left = 0.0, right = 0.0, bottom = 0.0;
        for (SDK::UTextBlock* elem : elems)
        {
            if (!elem || !elem->IsVisible())
                continue;

            const SDK::FGeometry geo  = elem->GetCachedGeometry();
            const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetLocalSize(geo);
            if (size.X <= 0.0 || size.Y <= 0.0)
                continue;

            const SDK::FVector2D topLeft = SDK::USlateBlueprintLibrary::AbsoluteToLocal(
                canvasGeo, SDK::USlateBlueprintLibrary::LocalToAbsolute(geo, Vec(0.0, 0.0)));
            const SDK::FVector2D bottomRight = SDK::USlateBlueprintLibrary::AbsoluteToLocal(
                canvasGeo, SDK::USlateBlueprintLibrary::LocalToAbsolute(geo, size));

            if (!any || topLeft.X < left)        left   = topLeft.X;
            if (!any || bottomRight.X > right)   right  = bottomRight.X;
            if (!any || bottomRight.Y > bottom)  bottom = bottomRight.Y;
            any = true;
        }
        if (!any)
            return false;

        *out = Vec(left + (right - left) * g_alignX, bottom);
        return true;
    }

    const SDK::FText* TextFor(const std::wstring& s)
    {
        auto it = g_texts.find(s);
        if (it != g_texts.end())
            return &it->second;

        const SDK::FText text = SDK::UKismetTextLibrary::Conv_StringToText(SDK::FString(s.c_str()));
        return &g_texts.emplace(s, text).first->second;
    }

    // `cmPerSec` as the readout shows it, in the panel's unit.
    std::wstring FormatSpeed(double cmPerSec)
    {
        char unit[16] = {};
        long value = std::lround(SpeedInPanelUnit(static_cast<float>(cmPerSec), unit, sizeof(unit)));
        if (value < 0)
            value = 0;
        for (long step = 10; value >= 1000 * step / 10; step *= 10)
            value = (value + step / 2) / step * step;

        wchar_t buf[48];
        swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%ld %hs", value, unit);
        return buf;
    }

    void SetShown(SDK::UTextBlock* text, bool shown)
    {
        if (g_shown == shown)
            return;
        text->SetVisibility(shown ? SDK::ESlateVisibility::HitTestInvisible : SDK::ESlateVisibility::Collapsed);
        g_shown = shown;
    }

    void Forget()
    {
        g_text.Reset();
        g_hud.Reset();
        g_shown = false;
        g_shownText.clear();
        g_position = {};
    }

    // Game thread only.
    void Detach()
    {
        if (SDK::UTextBlock* text = g_text.Get())
            text->RemoveFromParent();
        Forget();
    }

    // Game thread only. Adds the text block, styled like the readout's units
    // digit and hidden until it has a place, to the canvas that holds the
    // readout.
    bool Attach(SDK::UCrUW_BuildingDroneHud* hud)
    {
        SDK::UTextBlock* source = hud->DistanceValueU;
        SDK::UCanvasPanel* canvas = source ? FindCanvas(source) : nullptr;
        if (!canvas)
        {
            if (!g_loggedMissing)
                LOG_WARN("Speedometer: no canvas panel holds the drone HUD's distance readout; no speedometer");
            g_loggedMissing = true;
            return false;
        }

        SDK::UObject* created = SDK::UGameplayStatics::SpawnObject(SDK::UTextBlock::StaticClass(), canvas->Outer);
        if (!created || !created->IsA(SDK::UTextBlock::StaticClass()))
        {
            if (!g_loggedMissing)
                LOG_WARN("Speedometer: could not create the text block; no speedometer");
            g_loggedMissing = true;
            return false;
        }
        auto* text = static_cast<SDK::UTextBlock*>(created);

        text->SetFont(source->Font);
        text->SetColorAndOpacity(source->ColorAndOpacity);
        text->SetShadowOffset(source->ShadowOffset);
        text->SetShadowColorAndOpacity(source->ShadowColorAndOpacity);
        text->SetJustification(source->Justification);
        text->SetVisibility(SDK::ESlateVisibility::Collapsed);

        switch (source->Justification)
        {
        case SDK::ETextJustify::Center: g_alignX = 0.5; break;
        case SDK::ETextJustify::Right:  g_alignX = 1.0; break;
        default:                        g_alignX = 0.0; break;
        }

        SDK::UCanvasPanelSlot* slot = canvas->AddChildToCanvas(text);
        if (!slot)
        {
            if (!g_loggedMissing)
                LOG_WARN("Speedometer: the HUD's canvas would not take the text block; no speedometer");
            g_loggedMissing = true;
            return false;
        }
        SDK::FAnchors anchors;
        anchors.Minimum = Vec(0.0, 0.0);
        anchors.Maximum = Vec(0.0, 0.0);
        slot->SetAnchors(anchors);
        slot->SetAlignment(Vec(g_alignX, 0.0));
        slot->SetAutoSize(true);

        g_hud.Set(hud);
        g_text.Set(text);
        g_shown = false;
        g_shownText.clear();
        g_position = {};
        g_loggedMissing = false;

        LOG_INFO("Speedometer: attached to the drone HUD (%s in %s)",
            canvas->GetName().c_str(), canvas->Outer && canvas->Outer->Outer ? canvas->Outer->Outer->GetName().c_str() : "?");
        return true;
    }
}

void TickDroneSpeedometer()
{
    g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

    // Both are looked up again every tick. The text dies with the HUD's
    // widget tree; a HUD gone while the text lives on (it sits in a canvas
    // further up) takes the text out with it.
    SDK::UCrUW_BuildingDroneHud* hud = g_hud.Get();
    SDK::UTextBlock* text = g_text.Get();
    if (text && (!hud || !text->Slot))
    {
        Detach();
        hud  = nullptr;
        text = nullptr;
    }
    else if (!text && hud)
    {
        Forget();
        hud = nullptr;
    }

    if (!DroneConfig::Config::ReadSpeedometer())
    {
        if (text)
            Detach();
        return;
    }

    SDK::ACrCharacterPlayerBase* character = LocalPlayerCharacter();
    const bool inDrone = character && character->Status == SDK::EPlayerCharacterStatus::BuildingDrone;
    const uint64_t now = GetTickCount64();
    if (inDrone && !g_wasInDrone)
    {
        g_droneSinceMs = now;
        g_nextFindMs   = 0;
        g_smoothed     = -1.0;
    }
    g_wasInDrone = inDrone;

    if (!inDrone)
    {
        if (text)
            SetShown(text, false);
        return;
    }

    if (now < g_nextUpdateMs)
        return;
    g_nextUpdateMs = now + kUpdateMs;

    if (!text)
    {
        if (now < g_nextFindMs)
            return;
        g_nextFindMs = now + kFindRetryMs;

        hud = FindHud(character);
        if (!hud)
        {
            if (!g_loggedMissing && now - g_droneSinceMs >= kMissingGraceMs)
            {
                LOG_WARN("Speedometer: drone HUD widget not found; no speedometer");
                g_loggedMissing = true;
            }
            return;
        }
        if (!Attach(hud))
        {
            g_nextFindMs = UINT64_MAX; // the HUD will not change shape; try again next launch
            return;
        }
        text = g_text.Get();
        if (!text)
            return;
    }

    SDK::ACrCharacterDroneBase* drone = character->BuildingDrone;
    SDK::UCanvasPanelSlot* slot = text->Slot && text->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())
        ? static_cast<SDK::UCanvasPanelSlot*>(text->Slot) : nullptr;
    SDK::UCanvasPanel* canvas = slot && slot->Parent && slot->Parent->IsA(SDK::UCanvasPanel::StaticClass())
        ? static_cast<SDK::UCanvasPanel*>(slot->Parent) : nullptr;

    SDK::FVector2D position;
    if (!drone || !canvas || IsGameMenuOpen() || !PlaceUnderReadout(hud, canvas, &position))
    {
        SetShown(text, false);
        return;
    }

    const SDK::FVector velocity = drone->GetVelocity();
    const double speed = std::sqrt(velocity.X * velocity.X + velocity.Y * velocity.Y + velocity.Z * velocity.Z);
    g_smoothed = g_smoothed < 0.0 ? speed : g_smoothed + (speed - g_smoothed) * kSmoothing;

    if (std::fabs(position.X - g_position.X) > kMoveEpsilon || std::fabs(position.Y - g_position.Y) > kMoveEpsilon)
    {
        slot->SetPosition(position);
        g_position = position;
    }

    std::wstring shown = FormatSpeed(g_smoothed);
    if (shown != g_shownText)
    {
        text->SetText(*TextFor(shown));
        g_shownText = std::move(shown);
    }

    SetShown(text, true);
}

void ShutdownDroneSpeedometer()
{
    // On the game thread the text block is taken out of the HUD. Anywhere
    // else (older loaders, process exit) it is left where it is, to go with
    // the HUD.
    const bool onGameThread = GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed);
    if (onGameThread)
        Detach();
    else
        Forget();

    g_nextUpdateMs  = 0;
    g_nextFindMs    = 0;
    g_wasInDrone    = false;
    g_loggedMissing = false;
    g_smoothed      = -1.0;
}
