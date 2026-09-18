#include "drone_ui.h"
#include "drone_settings.h"
#include "drone_config.h"
#include "keybind_picker.h"
#include "plugin_helpers.h"
#include <cstdio>
#include <cmath>
#include <cstring>

static IPluginSelf* s_self = nullptr;
static PanelHandle s_panelHandle = nullptr;
static bool s_menuOpen = false;
static void* g_inputCaptureToken = nullptr;
static char g_registeredToggleKey[64] = {};

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
      "Modelled on 'Better Construction Drone' by SwiftstepsKR -- 2.5x speed, fast acceleration & double range.",
      "Modelled on NexusMod #27 by SwiftstepsKR",
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
        s_menuOpen = false;
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

    RebindToggleKey();
}

void ShutdownDroneUI(IPluginSelf* self)
{
    if (s_self && s_self->hooks)
    {
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
    s_menuOpen = false;
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

    s_menuOpen = !s_menuOpen;
    if (s_menuOpen)
    {
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
    if (!g_drone.valid || !s_self || !s_self->config) return;
    auto* cfg = s_self->config;

    *g_drone.speedPerSec   = preset.speedPerSec;
    *g_drone.maxRadius     = preset.maxRadius;
    *g_drone.warningRadius = preset.maxRadius * 0.95f;
    *g_drone.maxHeight     = preset.maxHeight;
    *g_drone.warningHeight = preset.maxHeight * 0.95f;

    cfg->WriteFloat(s_self, "Drone", "SpeedPerSec",        preset.speedPerSec);
    cfg->WriteFloat(s_self, "Drone", "MaxRadius",          preset.maxRadius);
    cfg->WriteFloat(s_self, "Drone", "MaxHeight",          preset.maxHeight);

    cfg->WriteFloat(s_self, "Controls", "BoostMultiplier", preset.boostMultiplier);
    cfg->WriteFloat(s_self, "Controls", "Acceleration",    preset.acceleration);
    cfg->WriteFloat(s_self, "Controls", "Deceleration",    preset.deceleration);

    RequestUpdateActiveDrones();
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
            char tooltipBuf[256];
            snprintf(tooltipBuf, sizeof(tooltipBuf),
                "%s\n\nSpeed: %.0f cm/s | Boost: %.1fx | Accel: %.0f | Range: %.0fm H / %.0fm V%s%s",
                preset.tooltip, preset.speedPerSec, preset.boostMultiplier, preset.acceleration,
                preset.maxRadius / 100.0f, preset.maxHeight / 100.0f,
                preset.credit ? "\n\n" : "", preset.credit ? preset.credit : "");
            ui->SetTooltip(tooltipBuf);
        }
    }

    ui->Spacing();
    ui->SeparatorText("Movement & Tuning");

    float speed = *g_drone.speedPerSec;
    ui->SetNextItemWidth(240.f);
    if (ui->InputFloat("Speed (cm/s)##speed", &speed, 100.f, 1000.f, "%.0f"))
    {
        *g_drone.speedPerSec = speed;
        if (s_self && s_self->config)
            s_self->config->WriteFloat(s_self, "Drone", "SpeedPerSec", speed);
        RequestUpdateActiveDrones();
    }

    float boostMult = DroneConfig::Config::ReadBoostMultiplier();
    ui->SetNextItemWidth(240.f);
    if (ui->InputFloat("Boost Multiplier##boostm", &boostMult, 0.5f, 1.0f, "%.1fx"))
    {
        if (boostMult < 1.0f) boostMult = 1.0f;
        if (boostMult > 10.0f) boostMult = 10.0f;
        if (s_self && s_self->config)
            s_self->config->WriteFloat(s_self, "Controls", "BoostMultiplier", boostMult);
    }
    if (ui->IsItemHovered())
        ui->SetTooltip("Speed multiplier applied when holding the Boost key (default LeftShift).");

    float accel = DroneConfig::Config::ReadAcceleration();
    ui->SetNextItemWidth(240.f);
    if (ui->InputFloat("Acceleration (cm/s2)##accel", &accel, 500.f, 2000.f, "%.0f"))
    {
        if (accel < 0.0f) accel = 0.0f;
        if (s_self && s_self->config)
            s_self->config->WriteFloat(s_self, "Controls", "Acceleration", accel);
    }
    if (ui->IsItemHovered())
        ui->SetTooltip("Acceleration rate in cm/s2. 0 = instant max speed.");

    float decel = DroneConfig::Config::ReadDeceleration();
    ui->SetNextItemWidth(240.f);
    if (ui->InputFloat("Deceleration (cm/s2)##decel", &decel, 500.f, 2000.f, "%.0f"))
    {
        if (decel < 0.0f) decel = 0.0f;
        if (s_self && s_self->config)
            s_self->config->WriteFloat(s_self, "Controls", "Deceleration", decel);
    }
    if (ui->IsItemHovered())
        ui->SetTooltip("Deceleration rate in cm/s2. 0 = instant stop.");

    ui->Spacing();
    ui->SeparatorText("Flight Envelope");

    float maxR = *g_drone.maxRadius;
    ui->SetNextItemWidth(240.f);
    if (ui->InputFloat("Max Radius (cm)##maxr", &maxR, 500.f, 2500.f, "%.0f"))
    {
        *g_drone.maxRadius = maxR;
        *g_drone.warningRadius = maxR * 0.95f;
        if (s_self && s_self->config)
            s_self->config->WriteFloat(s_self, "Drone", "MaxRadius", maxR);
        RequestUpdateActiveDrones();
    }

    float maxH = *g_drone.maxHeight;
    ui->SetNextItemWidth(240.f);
    if (ui->InputFloat("Max Height (cm)##maxh", &maxH, 500.f, 2500.f, "%.0f"))
    {
        *g_drone.maxHeight = maxH;
        *g_drone.warningHeight = maxH * 0.95f;
        if (s_self && s_self->config)
            s_self->config->WriteFloat(s_self, "Drone", "MaxHeight", maxH);
        RequestUpdateActiveDrones();
    }

    ui->Spacing();
    ui->SeparatorText("Wave Event Rules");
    bool waveAllowed = DroneConfig::Config::ReadAlwaysAllowDrone();
    if (ui->Checkbox("Allow Drone During Waves", &waveAllowed))
    {
        if (s_self && s_self->config)
            s_self->config->WriteBool(s_self, "Drone", "Always Allow Drone", waveAllowed);
    }
    if (ui->IsItemHovered())
        ui->SetTooltip("Allows summoning and operating the building drone while a wave defense is active.");

    ui->Spacing();
    ui->SeparatorText("Hotkeys & Keybindings");

    char toggleKeyBuf[64] = {};
    DroneConfig::Config::ReadToggleKey(toggleKeyBuf, sizeof(toggleKeyBuf));
    ui->Text("Toggle Menu Hotkey:");
    ui->SameLine(180.0f, -1.0f);
    char newToggleKey[64] = {};
    if (BetterDrone::Keybind::RenderPicker(ui, "##toggle_key_picker", toggleKeyBuf, newToggleKey, sizeof(newToggleKey)))
    {
        if (s_self && s_self->config)
        {
            s_self->config->WriteString(s_self, "Controls", "ToggleKey", newToggleKey);
            RebindToggleKey();
        }
    }

    char boostKeyBuf[64] = {};
    DroneConfig::Config::ReadBoostKey(boostKeyBuf, sizeof(boostKeyBuf));
    ui->Text("Boost Speed Key:");
    ui->SameLine(180.0f, -1.0f);
    char newBoostKey[64] = {};
    if (BetterDrone::Keybind::RenderPicker(ui, "##boost_key_picker", boostKeyBuf, newBoostKey, sizeof(newBoostKey)))
    {
        if (s_self && s_self->config)
        {
            s_self->config->WriteString(s_self, "Controls", "BoostKey", newBoostKey);
            RebindBoostKey();
        }
    }
}

