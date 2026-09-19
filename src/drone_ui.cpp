#include "drone_ui.h"
#include "drone_settings.h"
#include "drone_config.h"
#include "drone_audio.h"
#include "ui_widgets.h"
#include "plugin_helpers.h"
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>

static IPluginSelf* s_self = nullptr;
static PanelHandle s_panelHandle = nullptr;
static std::atomic<bool> s_menuOpen{ false };
static void* g_inputCaptureToken = nullptr;
static char g_registeredToggleKey[64] = {};

namespace
{
    constexpr float kActiveEpsilon = 0.0001f;
    constexpr int   kTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

    std::atomic<bool> g_closeRequested{ false };

    // Escape and Q (the game's own close/cancel key) are universal dismiss
    // keys, not a per-plugin setting, so both are registered by enum rather
    // than by name and neither appears on the loader config page. Simple
    // enum registrations like these are never in the loader's blocking map
    // (only a config-schema Keybind entry's own Blocking checkbox can put a
    // combo there), so Q still reaches the game normally whether or not
    // this panel is open.
    //
    // The callback's thread isn't guaranteed, so it only raises a flag;
    // RenderDronePanel does the actual close from the render thread. The
    // owner runs BetterCheats and BetterDrone panels open together on F10,
    // and only one can hold ImGui focus at a time -- gating the close on
    // focus (as this once did) silently dropped the request for whichever
    // panel didn't have it. Closing whenever the panel is open, regardless
    // of focus, is what actually dismisses both.
    void OnCloseKeyPressed(EModKey, EModKeyEvent event)
    {
        if (event != EModKeyEvent::Pressed)
            return;

        const bool open = s_menuOpen.load(std::memory_order_relaxed);
        LOG_DEBUG("OnCloseKeyPressed: close key received (open: %s)", open ? "yes" : "no");
        if (open)
            g_closeRequested.store(true, std::memory_order_relaxed);
    }

    // Conversion + slider feel for one field, per display unit. Values are
    // always stored and clamped in engine units (cm, cm/s); this only
    // controls what the row draws and how far its slider travels.
    struct FieldUnitScale
    {
        const char* format;
        float factor;
        float step;
        float stepFast;
        float sliderMin;
        float sliderMax;
    };

    enum UnitIndex { kUnitKmh = 0, kUnitMph = 1, kUnitCms = 2, kUnitCount = 3 };

    int SelectUnitIndex(const char* unit)
    {
        if (strcmp(unit, "mph") == 0)  return kUnitMph;
        if (strcmp(unit, "cm/s") == 0) return kUnitCms;
        return kUnitKmh;
    }

    constexpr FieldUnitScale kSpeedScale[kUnitCount] = {
        { "%.1f km/h",  0.036f,        5.0f,  25.0f,  0.0f, 400.0f    },
        { "%.1f mph",   0.0223693629f, 5.0f,  20.0f,  0.0f, 250.0f    },
        { "%.0f cm/s",  1.0f,        100.0f, 1000.0f, 0.0f, 12000.0f  },
    };

    constexpr FieldUnitScale kRateScale[kUnitCount] = {
        { "%.0f km/h/s", 0.036f,        10.0f, 50.0f,   0.0f, 200.0f   },
        { "%.0f mph/s",  0.0223693629f,  5.0f, 25.0f,   0.0f, 125.0f   },
        { "%.0f cm/s2",  1.0f,         500.0f, 2000.0f, 0.0f, 30000.0f },
    };

    constexpr FieldUnitScale kRadiusScale[kUnitCount] = {
        { "%.1f m",  0.01f,       50.0f,  250.0f, 0.0f, 10000.0f   },
        { "%.0f ft", 0.0328084f, 150.0f,  750.0f, 0.0f, 32808.4f   },
        { "%.0f cm", 1.0f,       500.0f, 2500.0f, 0.0f, 1000000.0f },
    };

    constexpr FieldUnitScale kHeightScale[kUnitCount] = {
        { "%.1f m",  0.01f,       50.0f,  250.0f, 0.0f, 5000.0f   },
        { "%.0f ft", 0.0328084f, 150.0f,  750.0f, 0.0f, 16404.2f  },
        { "%.0f cm", 1.0f,       500.0f, 2500.0f, 0.0f, 500000.0f },
    };

    constexpr FieldUnitScale kBoostScale = { "%.1fx", 1.0f, 0.5f, 1.0f, 1.0f, 10.0f };

    constexpr const char* kVolKeys[4] = { "IdleVolume", "MovementVolume", "RotationVolume", "StationVolume" };

    // Renders one label | slider+box (joined, zero spacing) | reset row for a
    // value tracked in engine units (cm or cm/s). `scale` converts to the
    // unit currently on display; the slider's own range is chosen to feel
    // right in that unit and is independent of the hard clamp the typed
    // Write* layer applies to whatever engine value is committed.
    //
    // Returns true and fills *outEngineValue when the row changes the value
    // this frame (drag step, typed edit, or reset) -- the caller should
    // apply this live (cache + drone), every time. *outCommit is set only
    // when the edit is actually finished (slider/box released after a real
    // change, or the reset button, which is a single click) -- the caller
    // should persist to disk only then, not on every drag step.
    bool RenderScaledRow(IModLoaderImGui* imgui, const char* rowId, const char* label,
                          const char* tooltip, float engineValue, float engineDefault,
                          const FieldUnitScale& scale, float* outEngineValue, bool* outCommit)
    {
        const bool active = std::fabs(engineValue - engineDefault) > kActiveEpsilon;

        imgui->PushIDStr(rowId);
        imgui->TableNextRow(0, 0.0f);

        imgui->TableSetColumnIndex(0);
        if (active) imgui->Text(label);
        else        imgui->TextDisabled(label);
        if (tooltip && imgui->IsItemHovered())
            imgui->SetTooltip(tooltip);

        imgui->TableSetColumnIndex(1);

        float availX = 0.0f, availY = 0.0f;
        imgui->GetContentRegionAvail(&availX, &availY);

        char widest[40];
        snprintf(widest, sizeof(widest), scale.format, -scale.sliderMax);
        float textW = 0.0f, textH = 0.0f;
        imgui->CalcTextSize(widest, &textW, &textH, false, -1.0f);

        const float frameH  = imgui->GetFrameHeight();
        const float numBoxW = textW + (frameH * 2.0f) + (frameH * 0.9f);
        const float sliderW = (availX > numBoxW + frameH * 2.0f) ? (availX - numBoxW) : (availX * 0.55f);

        float value = engineValue * scale.factor;
        bool  changed = false;
        bool  commit  = false;

        imgui->SetNextItemWidth(sliderW);
        if (imgui->SliderFloat("##slider", &value, scale.sliderMin, scale.sliderMax, scale.format))
            changed = true;
        if (imgui->IsItemDeactivatedAfterEdit())
            commit = true;

        imgui->SameLine(0.0f, 0.0f);
        imgui->SetNextItemWidth(-1.0f);
        if (imgui->InputFloat("##num", &value, scale.step, scale.stepFast, scale.format))
            changed = true;
        if (imgui->IsItemDeactivatedAfterEdit())
            commit = true;

        imgui->TableSetColumnIndex(2);
        if (BetterDrone::UI::ResetButton(imgui, "##reset"))
        {
            value = engineDefault * scale.factor;
            changed = true;
            commit  = true;
        }
        if (imgui->IsItemHovered())
            imgui->SetTooltip("Reset to default.");

        imgui->PopID();

        if (changed && outEngineValue)
            *outEngineValue = value / scale.factor;
        if (outCommit)
            *outCommit = commit;

        return changed;
    }

    float DeriveMasterVolume(bool* outActive)
    {
        float vols[4];
        for (int i = 0; i < 4; ++i)
            vols[i] = DroneConfig::Config::ReadAudioVolume(kVolKeys[i]);

        float maxV = vols[0];
        bool allEqual = true;
        bool anyNonDefault = false;
        for (int i = 0; i < 4; ++i)
        {
            if (std::fabs(vols[i] - vols[0]) > kActiveEpsilon) allEqual = false;
            if (vols[i] > maxV) maxV = vols[i];
            if (std::fabs(vols[i] - 1.0f) > kActiveEpsilon) anyNonDefault = true;
        }

        if (outActive) *outActive = anyNonDefault;
        return allEqual ? vols[0] : maxV;
    }

    // Updates the drone's live audio immediately (an atomic store, same as
    // the loader page's own drag path); does not touch BetterDrone.ini.
    void ApplyMasterVolumeLive(float value)
    {
        for (const char* key : kVolKeys)
            DroneAudio::SetVolume(key, value);
    }

    // Writes all four volumes to BetterDrone.ini. Called once the edit is
    // done, not on every drag step.
    void PersistMasterVolume(float value)
    {
        for (const char* key : kVolKeys)
            DroneConfig::Config::WriteAudioVolume(key, value);
    }

    // The four individual volumes live on the ModLoader settings page (instant
    // there too, see dllmain.cpp's OnConfigChanged). This is a "set all"
    // convenience, not a fifth value -- it derives its display from the four
    // rather than persisting one of its own, so it can never drift out of
    // sync with the loader page.
    void RenderAudioSection(IModLoaderImGui* ui)
    {
        if (!ui->CollapsingHeader("Audio"))
            return;

        if (!ui->BeginTable("##drone_audio_table", 3, kTableFlags))
            return;

        ui->TableSetupColumn("", 0, 0.36f);
        ui->TableSetupColumn("", 0, 0.54f);
        ui->TableSetupColumn("", 0, 0.10f);

        bool active = false;
        const float current = DeriveMasterVolume(&active);

        ui->PushIDStr("##master_vol");
        ui->TableNextRow(0, 0.0f);

        ui->TableSetColumnIndex(0);
        if (active) ui->Text("Master Volume");
        else        ui->TextDisabled("Master Volume");
        if (ui->IsItemHovered())
            ui->SetTooltip("Sets all four drone audio volumes together. Individual volumes are on the ModLoader settings page.");

        ui->TableSetColumnIndex(1);

        float availX = 0.0f, availY = 0.0f;
        ui->GetContentRegionAvail(&availX, &availY);

        float textW = 0.0f, textH = 0.0f;
        ui->CalcTextSize("1.00", &textW, &textH, false, -1.0f);
        const float frameH  = ui->GetFrameHeight();
        const float numBoxW = textW + (frameH * 2.0f) + (frameH * 0.9f);
        const float sliderW = (availX > numBoxW + frameH * 2.0f) ? (availX - numBoxW) : (availX * 0.55f);

        float value = current;
        bool  changed = false;
        bool  commit  = false;

        ui->SetNextItemWidth(sliderW);
        if (ui->SliderFloat("##slider", &value, 0.0f, 1.0f, "%.2f"))
            changed = true;
        if (ui->IsItemDeactivatedAfterEdit())
            commit = true;

        ui->SameLine(0.0f, 0.0f);
        ui->SetNextItemWidth(-1.0f);
        if (ui->InputFloat("##num", &value, 0.05f, 0.25f, "%.2f"))
            changed = true;
        if (ui->IsItemDeactivatedAfterEdit())
            commit = true;

        ui->TableSetColumnIndex(2);
        if (BetterDrone::UI::ResetButton(ui, "##reset"))
        {
            value = 1.0f;
            changed = true;
            commit  = true;
        }
        if (ui->IsItemHovered())
            ui->SetTooltip("Reset all four volumes to 1.0.");

        ui->PopID();

        if (changed)
        {
            if (value < 0.0f) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            ApplyMasterVolumeLive(value);
            if (commit)
                PersistMasterVolume(value);
        }

        ui->EndTable();
    }
}

struct DronePreset
{
    const char* label;
    const char* tooltip;
    const char* credit;
    float speedPerSec;
    float boostMultiplier;
    float acceleration;
    float deceleration;
    float maxRadius;
    float maxHeight;
};

static const DronePreset k_presets[] = {
    { "Stock",
      "Default un-modded StarRupture building drone limits.",
      "Game Default",
      1000.0f, 2.0f, 0.0f, 0.0f, 5000.0f, 2000.0f },

    { "Better Construction",
      "Modelled on 'Better Construction Drone' by CrazyCovin -- 2.5x speed, fast acceleration & double range.",
      "Modelled on NexusMod #27 by CrazyCovin",
      2500.0f, 2.5f, 5000.0f, 5000.0f, 10000.0f, 5000.0f },

    { "Agile Builder",
      "High speed, rapid response, and expanded flight envelope for mega-base building.",
      "GSS Preset",
      4000.0f, 3.0f, 10000.0f, 10000.0f, 20000.0f, 10000.0f },

    { "Ludicrous Speed",
      "Supercharged drone: ultra-fast travel and heavy boost multiplier.",
      "GSS Preset",
      8000.0f, 4.0f, 20000.0f, 20000.0f, 50000.0f, 25000.0f },

    { "Infinite Range",
      "Map-wide construction envelope: build anywhere across the planet without height or distance restrictions.",
      "GSS Preset",
      5000.0f, 3.0f, 12000.0f, 12000.0f, 1000000.0f, 500000.0f }
};
constexpr int k_presetCount = static_cast<int>(sizeof(k_presets) / sizeof(k_presets[0]));

static void OnToggleKeyPressed(EModKey, EModKeyEvent event)
{
    if (event == EModKeyEvent::Pressed)
    {
        ToggleDroneMenu();
    }
}

static void OnPanelClosed(PanelHandle handle)
{
    if (handle == s_panelHandle)
    {
        s_menuOpen.store(false, std::memory_order_relaxed);
        if (s_self && g_inputCaptureToken)
        {
            s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
            g_inputCaptureToken = nullptr;
        }
    }
}

void InitDroneUI(IPluginSelf* self)
{
    s_self = self;
    if (!self || !self->hooks || !self->hooks->UI) return;

    static PluginPanelDesc desc{};
    desc.buttonLabel = "BetterDrone";
    desc.windowTitle = "BetterDrone";
    desc.renderFn    = &RenderDronePanel;

    s_panelHandle = self->hooks->UI->RegisterPanel(&desc);
    self->hooks->UI->RegisterOnPanelWindowClosed(OnPanelClosed);

    if (self->hooks->Input)
    {
        self->hooks->Input->RegisterKeybind(EModKey::Escape, EModKeyEvent::Pressed, OnCloseKeyPressed);
        self->hooks->Input->RegisterKeybind(EModKey::Q, EModKeyEvent::Pressed, OnCloseKeyPressed);
    }

    RebindToggleKey();
}

void ShutdownDroneUI(IPluginSelf*)
{
    if (s_self && s_self->hooks)
    {
        if (s_self->hooks->Input)
        {
            s_self->hooks->Input->UnregisterKeybind(EModKey::Escape, EModKeyEvent::Pressed, OnCloseKeyPressed);
            s_self->hooks->Input->UnregisterKeybind(EModKey::Q, EModKeyEvent::Pressed, OnCloseKeyPressed);
        }

        if (g_registeredToggleKey[0] != '\0' && s_self->hooks->Input)
        {
            s_self->hooks->Input->UnregisterKeybindByName(g_registeredToggleKey, EModKeyEvent::Pressed, OnToggleKeyPressed);
            g_registeredToggleKey[0] = '\0';
        }

        if (s_panelHandle && s_self->hooks->UI)
        {
            s_self->hooks->UI->SetPanelClose(s_panelHandle);
            if (g_inputCaptureToken)
            {
                s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
                g_inputCaptureToken = nullptr;
            }
            s_self->hooks->UI->UnregisterOnPanelWindowClosed(OnPanelClosed);
            s_self->hooks->UI->UnregisterPanel(s_panelHandle);
            s_panelHandle = nullptr;
        }
    }
    s_self = nullptr;
    s_menuOpen.store(false, std::memory_order_relaxed);
}

void RebindToggleKey()
{
    if (!s_self || !s_self->hooks || !s_self->hooks->Input) return;

    if (g_registeredToggleKey[0] != '\0')
    {
        s_self->hooks->Input->UnregisterKeybindByName(g_registeredToggleKey, EModKeyEvent::Pressed, OnToggleKeyPressed);
        g_registeredToggleKey[0] = '\0';
    }

    char keyName[64] = {};
    DroneConfig::Config::ReadToggleKey(keyName, sizeof(keyName));
    if (keyName[0] != '\0')
    {
        s_self->hooks->Input->RegisterKeybindByName(keyName, EModKeyEvent::Pressed, OnToggleKeyPressed);
        snprintf(g_registeredToggleKey, sizeof(g_registeredToggleKey), "%s", keyName);
    }
}

void ToggleDroneMenu()
{
    if (!s_panelHandle || !s_self || !s_self->hooks || !s_self->hooks->UI) return;

    const bool opening = !s_menuOpen.load(std::memory_order_relaxed);
    s_menuOpen.store(opening, std::memory_order_relaxed);

    if (opening)
    {
        g_closeRequested.store(false, std::memory_order_relaxed);
        s_self->hooks->UI->SetPanelOpen(s_panelHandle);
        g_inputCaptureToken = s_self->hooks->UI->AcquireInputCapture();
    }
    else
    {
        s_self->hooks->UI->SetPanelClose(s_panelHandle);
        if (g_inputCaptureToken)
        {
            s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
            g_inputCaptureToken = nullptr;
        }
    }
}

static void ApplyPreset(const DronePreset& preset)
{
    if (!g_drone.valid) return;

    DroneConfig::Config::WriteSpeedPerSec(preset.speedPerSec);
    const float radius = DroneConfig::Config::WriteMaxRadius(preset.maxRadius);
    const float height = DroneConfig::Config::WriteMaxHeight(preset.maxHeight);
    DroneConfig::Config::WriteBoostMultiplier(preset.boostMultiplier);
    DroneConfig::Config::WriteAcceleration(preset.acceleration);
    DroneConfig::Config::WriteDeceleration(preset.deceleration);

    RequestMaxRadius(radius);
    RequestMaxHeight(height);
}

static void RenderUnavailableMessage(IModLoaderImGui* imgui, float avail_x, float avail_y, const char* message)
{
    float text_x = 0.0f, text_y = 0.0f;
    imgui->CalcTextSize(message, &text_x, &text_y, false, 0.0f);

    float cursor_x = imgui->GetCursorPosX();
    float cursor_y = imgui->GetCursorPosY();
    imgui->SetCursorPos(cursor_x + (avail_x - text_x) * 0.5f, cursor_y + (avail_y - text_y) * 0.5f);
    imgui->Text(message);
}

void RenderDronePanel(IModLoaderImGui* ui)
{
    // Deferred by one frame: closing here, inside the same keypress that
    // requested it, could release input capture in time for that same
    // press to also reach the game's own pause menu. No focus check --
    // several panels (BetterCheats, BetterDrone) are typically open at
    // once and only one can hold ImGui focus, so gating the close on focus
    // silently dropped it for whichever panel didn't have it. Closing
    // whenever the panel is open dismisses it regardless of which one the
    // player was actually looking at.
    if (g_closeRequested.exchange(false, std::memory_order_relaxed) &&
        s_menuOpen.load(std::memory_order_relaxed))
    {
        ToggleDroneMenu();
        return;
    }

    float avail_x = 0.0f, avail_y = 0.0f;
    ui->GetContentRegionAvail(&avail_x, &avail_y);

    if (!IsInGameSession())
    {
        RenderUnavailableMessage(ui, avail_x, avail_y, "BetterDrone menu can only be used in game");
        return;
    }

    if (!g_drone.valid)
    {
        RenderUnavailableMessage(ui, avail_x, avail_y, "Drone settings CDO not found -- engine may still be initialising.");
        return;
    }

    ui->SeparatorText("Presets");
    ui->TextDisabled("Select a preset modelled on classic construction drone mods:");
    ui->Spacing();

    for (int i = 0; i < k_presetCount; ++i)
    {
        const auto& preset = k_presets[i];
        if (i > 0) ui->SameLine(0.0f, -1.0f);

        if (ui->SmallButton(preset.label))
        {
            ApplyPreset(preset);
        }
        if (ui->IsItemHovered())
        {
            char tooltipBuf[320];
            float kmh = preset.speedPerSec * 0.036f;
            float mph = preset.speedPerSec * 0.0223693629f;
            snprintf(tooltipBuf, sizeof(tooltipBuf),
                "%s\n\nSpeed: %.1f km/h (%.1f mph | %.0f cm/s) | Boost: %.1fx | Accel: %.0f | Range: %.0fm H / %.0fm V%s%s",
                preset.tooltip, kmh, mph, preset.speedPerSec, preset.boostMultiplier, preset.acceleration,
                preset.maxRadius / 100.0f, preset.maxHeight / 100.0f,
                preset.credit ? "\n\n" : "", preset.credit ? preset.credit : "");
            ui->SetTooltip(tooltipBuf);
        }
    }

    ui->Spacing();
    ui->SeparatorText("Speed Units & Movement Tuning");

    char currentUnit[16] = {};
    DroneConfig::Config::ReadSpeedUnit(currentUnit, sizeof(currentUnit));
    const int unitIdx = SelectUnitIndex(currentUnit);

    const char* unitLabel = "Speed Display Unit:";
    float unitLabelW = 0.0f, unitLabelH = 0.0f;
    ui->CalcTextSize(unitLabel, &unitLabelW, &unitLabelH, false, -1.0f);

    ui->Text(unitLabel);
    ui->SameLine(unitLabelW + ui->GetFrameHeight() * 0.5f, -1.0f);
    if (ui->RadioButton("km/h (Metric)##unit_kmh", unitIdx == kUnitKmh))
        DroneConfig::Config::WriteSpeedUnit("km/h");
    ui->SameLine(0.0f, 10.0f);
    if (ui->RadioButton("mph (Imperial)##unit_mph", unitIdx == kUnitMph))
        DroneConfig::Config::WriteSpeedUnit("mph");
    ui->SameLine(0.0f, 10.0f);
    if (ui->RadioButton("cm/s (Engine)##unit_cms", unitIdx == kUnitCms))
        DroneConfig::Config::WriteSpeedUnit("cm/s");

    ui->Spacing();

    if (ui->BeginTable("##drone_tuning_table", 3, kTableFlags))
    {
        ui->TableSetupColumn("", 0, 0.36f);
        ui->TableSetupColumn("", 0, 0.54f);
        ui->TableSetupColumn("", 0, 0.10f);

        float newSpeed = 0.0f;
        bool  speedCommit = false;
        if (RenderScaledRow(ui, "##speed", "Drone Speed", nullptr,
                             DroneConfig::Config::ReadSpeedPerSec(), g_drone.origSpeedPerSec,
                             kSpeedScale[unitIdx], &newSpeed, &speedCommit))
        {
            DroneConfig::Config::SetSpeedPerSecLive(newSpeed);
            if (speedCommit)
                DroneConfig::Config::PersistSpeedPerSec();
        }

        float newAccel = 0.0f;
        bool  accelCommit = false;
        if (RenderScaledRow(ui, "##accel", "Acceleration", "0 = instant max speed.",
                             DroneConfig::Config::ReadAcceleration(), DroneConfig::Config::DefaultAcceleration(),
                             kRateScale[unitIdx], &newAccel, &accelCommit))
        {
            DroneConfig::Config::SetAccelerationLive(newAccel);
            if (accelCommit)
                DroneConfig::Config::PersistAcceleration();
        }

        float newDecel = 0.0f;
        bool  decelCommit = false;
        if (RenderScaledRow(ui, "##decel", "Deceleration", "0 = instant stop.",
                             DroneConfig::Config::ReadDeceleration(), DroneConfig::Config::DefaultDeceleration(),
                             kRateScale[unitIdx], &newDecel, &decelCommit))
        {
            DroneConfig::Config::SetDecelerationLive(newDecel);
            if (decelCommit)
                DroneConfig::Config::PersistDeceleration();
        }

        float newBoost = 0.0f;
        bool  boostCommit = false;
        if (RenderScaledRow(ui, "##boost", "Boost Multiplier", "Speed multiplier while the Boost key is held. Set the Boost key on the ModLoader settings page.",
                             DroneConfig::Config::ReadBoostMultiplier(), DroneConfig::Config::DefaultBoostMultiplier(),
                             kBoostScale, &newBoost, &boostCommit))
        {
            DroneConfig::Config::SetBoostMultiplierLive(newBoost);
            if (boostCommit)
                DroneConfig::Config::PersistBoostMultiplier();
        }

        ui->EndTable();
    }

    ui->Spacing();
    ui->SeparatorText("Flight Envelope");

    if (ui->BeginTable("##drone_radius_table", 3, kTableFlags))
    {
        ui->TableSetupColumn("", 0, 0.36f);
        ui->TableSetupColumn("", 0, 0.54f);
        ui->TableSetupColumn("", 0, 0.10f);

        float engineRadius = DroneConfig::Config::ReadMaxRadius();
        float newRadius = 0.0f;
        bool  radiusCommit = false;
        if (RenderScaledRow(ui, "##maxr", "Max Radius", nullptr,
                             engineRadius, g_drone.origMaxRadius,
                             kRadiusScale[unitIdx], &newRadius, &radiusCommit))
        {
            const float radius = DroneConfig::Config::SetMaxRadiusLive(newRadius);
            RequestMaxRadius(radius);
            if (radiusCommit)
                DroneConfig::Config::PersistMaxRadius();
            engineRadius = radius;
        }

        ui->EndTable();

        char radiusEquiv[128];
        snprintf(radiusEquiv, sizeof(radiusEquiv), "Horizontal Range: %.1f m  |  %.0f ft",
            engineRadius * 0.01f, engineRadius * 0.0328084f);
        ui->TextDisabled(radiusEquiv);
    }

    ui->Spacing();

    if (ui->BeginTable("##drone_height_table", 3, kTableFlags))
    {
        ui->TableSetupColumn("", 0, 0.36f);
        ui->TableSetupColumn("", 0, 0.54f);
        ui->TableSetupColumn("", 0, 0.10f);

        float engineHeight = DroneConfig::Config::ReadMaxHeight();
        float newHeight = 0.0f;
        bool  heightCommit = false;
        if (RenderScaledRow(ui, "##maxh", "Max Height", nullptr,
                             engineHeight, g_drone.origMaxHeight,
                             kHeightScale[unitIdx], &newHeight, &heightCommit))
        {
            const float height = DroneConfig::Config::SetMaxHeightLive(newHeight);
            RequestMaxHeight(height);
            if (heightCommit)
                DroneConfig::Config::PersistMaxHeight();
            engineHeight = height;
        }

        ui->EndTable();

        char heightEquiv[128];
        snprintf(heightEquiv, sizeof(heightEquiv), "Vertical Ceiling: %.1f m  |  %.0f ft",
            engineHeight * 0.01f, engineHeight * 0.0328084f);
        ui->TextDisabled(heightEquiv);
    }

    ui->Spacing();
    RenderAudioSection(ui);
}
