#include "drone_ui.h"
#include "drone_settings.h"
#include "drone_config.h"
#include "drone_audio.h"
#include "preset_store.h"
#include "ui_widgets.h"
#include "plugin_helpers.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>

// preset_store.h declares BetterDrone::PresetStore; alias it down to the
// bare PresetStore:: used throughout the saved-presets code below.
namespace PresetStore = BetterDrone::PresetStore;

static IPluginSelf* s_self = nullptr;
static PanelHandle s_panelHandle = nullptr;
static std::atomic<bool> s_menuOpen{ false };
static void* g_inputCaptureToken = nullptr;
static char g_registeredToggleKey[64] = {};

namespace
{
    constexpr float kActiveEpsilon = 0.0001f;
    constexpr int   kTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

    // Mirrors drone_config.cpp's GetModuleDirectory: resolved via this
    // function's own address rather than a stored DllMain HMODULE, so it
    // works regardless of load order. Sits next to BetterDrone.ini and
    // BetterDrone-Panel.ini under <this dir>\config\.
    void GetPresetsFilePath(char* outPath, size_t outSize)
    {
        HMODULE module = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&GetPresetsFilePath), &module);

        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);

        char* lastSlash = strrchr(path, '\\');
        if (lastSlash)
            *lastSlash = '\0';

        snprintf(outPath, outSize, "%s\\config\\BetterDrone-Presets.ini", path);
    }

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

    // Volume is already a plain 0..1 fraction, so RenderScaledRow's engine
    // unit and slider range are used as-is (factor 1.0, no conversion).
    constexpr FieldUnitScale kMasterVolumeScale = { "%.2f", 1.0f, 0.05f, 0.25f, 0.0f, 1.0f };

    constexpr const char* kVolKeys[4] = { "IdleVolume", "MovementVolume", "RotationVolume", "StationVolume" };

    // Sliders stop growing past this width instead of filling the whole
    // column -- 240px at the default font size, same idea as the loader's
    // own sliderMaxW (modloader_window.cpp). A plugin has no access to
    // ImGuiStyle::FontScaleMain, so the scale is derived from the ratio
    // between the live font size and the loader's base font pixel size
    // (imgui_backend.cpp's kBasePx) instead.
    constexpr float kBaseFontPx        = 15.0f;
    constexpr float kDefaultSliderMaxW = 240.0f;

    float DefaultSliderMaxWidth(IModLoaderImGui* imgui)
    {
        return kDefaultSliderMaxW * (imgui->GetFontSize() / kBaseFontPx);
    }

    // Renders one label | slider+box (joined, zero spacing) | reset row for a
    // value tracked in engine units (cm or cm/s). `scale` converts to the
    // unit currently on display; the slider's own range is chosen to feel
    // right in that unit and is independent of the hard clamp the typed
    // Write* layer applies to whatever engine value is committed. `maxWidth`
    // caps the slider's pixel width; 0 (the default) uses DefaultSliderMaxWidth.
    //
    // Returns true and fills *outEngineValue when the row changes the value
    // this frame (drag step, typed edit, or reset) -- the caller should
    // apply this live (cache + drone), every time. *outCommit is set only
    // when the edit is actually finished (slider/box released after a real
    // change, or the reset button, which is a single click) -- the caller
    // should persist to disk only then, not on every drag step.
    bool RenderScaledRow(IModLoaderImGui* imgui, const char* rowId, const char* label,
                          const char* tooltip, float engineValue, float engineDefault,
                          const FieldUnitScale& scale, float* outEngineValue, bool* outCommit,
                          float maxWidth = 0.0f)
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
        float sliderW = (availX > numBoxW + frameH * 2.0f) ? (availX - numBoxW) : (availX * 0.55f);

        const float cap = (maxWidth > 0.0f) ? maxWidth : DefaultSliderMaxWidth(imgui);
        if (sliderW > cap)
            sliderW = cap;

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

    float DeriveMasterVolume()
    {
        float vols[4];
        for (int i = 0; i < 4; ++i)
            vols[i] = DroneConfig::Config::ReadAudioVolume(kVolKeys[i]);

        float maxV = vols[0];
        bool allEqual = true;
        for (int i = 0; i < 4; ++i)
        {
            if (std::fabs(vols[i] - vols[0]) > kActiveEpsilon) allEqual = false;
            if (vols[i] > maxV) maxV = vols[i];
        }

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

        float newValue = 0.0f;
        bool  commit   = false;
        if (RenderScaledRow(ui, "##master_vol", "Master Volume",
                             "Sets all four drone audio volumes together. Individual volumes are on the ModLoader settings page.",
                             DeriveMasterVolume(), 1.0f, kMasterVolumeScale, &newValue, &commit))
        {
            if (newValue < 0.0f) newValue = 0.0f;
            if (newValue > 1.0f) newValue = 1.0f;
            ApplyMasterVolumeLive(newValue);
            if (commit)
                PersistMasterVolume(newValue);
        }

        ui->EndTable();
    }
}

// Speed and range used to come as one bundled preset; split so either axis
// can be picked independently (e.g. Better Construction speed with a
// Map-wide range).
struct SpeedPreset
{
    const char* label;
    const char* tooltip;
    const char* credit;
    float speedPerSec;
    float boostMultiplier;
    float acceleration;
    float deceleration;
};

struct RangePreset
{
    const char* label;
    const char* tooltip;
    const char* credit;
    float maxRadius;
    float maxHeight;
};

static const SpeedPreset k_speedPresets[] = {
    { "Stock",
      "Default un-modded StarRupture building drone speed.",
      "Game Default",
      1000.0f, 2.0f, 0.0f, 0.0f },

    { "Better Construction",
      "Modelled on 'Better Construction Drone' by CrazyCovin -- 2.5x speed & fast acceleration.",
      "Modelled on NexusMod #27 by CrazyCovin",
      2500.0f, 2.5f, 5000.0f, 5000.0f },

    { "Agile Builder",
      "High speed and rapid response for mega-base building.",
      "GSS Preset",
      4000.0f, 3.0f, 10000.0f, 10000.0f },

    { "Ludicrous Speed",
      "Supercharged drone: ultra-fast travel and heavy boost multiplier.",
      "GSS Preset",
      8000.0f, 4.0f, 20000.0f, 20000.0f },

    { "Long Haul",
      "Moderate speed and boost for long-range trips -- pair with the Map-wide range preset below for full planet coverage.",
      "GSS Preset",
      5000.0f, 3.0f, 12000.0f, 12000.0f }
};
constexpr int k_speedPresetCount = static_cast<int>(sizeof(k_speedPresets) / sizeof(k_speedPresets[0]));

// Every entry stays within kMaxRadiusBound/kMaxHeightBound (drone_config.cpp);
// Map-wide sits exactly at that ceiling.
static const RangePreset k_rangePresets[] = {
    { "Stock",
      "Default un-modded StarRupture building drone range.",
      "Game Default",
      5000.0f, 2000.0f },

    { "Better Construction",
      "Modelled on 'Better Construction Drone' by CrazyCovin -- double range.",
      "Modelled on NexusMod #27 by CrazyCovin",
      10000.0f, 5000.0f },

    { "Agile Builder",
      "Expanded flight envelope for mega-base building.",
      "GSS Preset",
      20000.0f, 10000.0f },

    { "Map-wide",
      "Build anywhere across the planet -- the same ceiling as NexusMod #27's 'Unlimited'.",
      "Modelled on NexusMod #27 by CrazyCovin",
      1000000.0f, 500000.0f }
};
constexpr int k_rangePresetCount = static_cast<int>(sizeof(k_rangePresets) / sizeof(k_rangePresets[0]));

static void OnToggleKeyPressed(EModKey, EModKeyEvent event)
{
    if (event == EModKeyEvent::Pressed)
    {
        ToggleDroneMenu();
    }
}

// Applies "closed" to this plugin's own state -- s_menuOpen false, capture
// token released -- idempotently. Every close path funnels through this
// (never a raw flip), so a second close signal for an already-closed panel,
// from any source, is a guaranteed no-op instead of a reopen.
static void ApplyMenuClosed(const char* reason)
{
    if (!s_menuOpen.exchange(false, std::memory_order_relaxed))
    {
        LOG_DEBUG("ApplyMenuClosed(%s): already closed, ignoring", reason);
        return;
    }

    LOG_DEBUG("ApplyMenuClosed(%s): closing", reason);
    if (g_inputCaptureToken)
    {
        LOG_DEBUG("ApplyMenuClosed(%s): releasing capture token %p", reason, g_inputCaptureToken);
        if (s_self && s_self->hooks && s_self->hooks->UI)
            s_self->hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
        g_inputCaptureToken = nullptr;
    }
}

// The loader's own notification that our panel closed -- its titlebar X, or
// (redundantly, harmlessly) our own SetPanelClose call cascading back here.
// Never touches the registry itself; only reconciles our side.
static void OnPanelClosed(PanelHandle handle)
{
    if (handle != s_panelHandle)
        return;

    LOG_DEBUG("OnPanelClosed: loader reports the panel closed");
    ApplyMenuClosed("OnPanelClosed");
}

void InitDroneUI(IPluginSelf* self)
{
    s_self = self;
    if (!self || !self->hooks || !self->hooks->UI) return;

    char presetsPath[MAX_PATH] = {};
    GetPresetsFilePath(presetsPath, sizeof(presetsPath));
    PresetStore::Init(presetsPath);

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
            ApplyMenuClosed("ShutdownDroneUI");
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

// Idempotent: no-op if already open. Never call this from a close path.
static void OpenDroneMenu()
{
    if (!s_panelHandle || !s_self || !s_self->hooks || !s_self->hooks->UI) return;

    if (s_menuOpen.exchange(true, std::memory_order_relaxed))
    {
        LOG_DEBUG("OpenDroneMenu: already open, ignoring");
        return;
    }

    g_closeRequested.store(false, std::memory_order_relaxed);
    s_self->hooks->UI->SetPanelOpen(s_panelHandle);
    g_inputCaptureToken = s_self->hooks->UI->AcquireInputCapture();
    LOG_DEBUG("OpenDroneMenu: opened, capture token %p", g_inputCaptureToken);
}

// Idempotent: no-op if already closed (ApplyMenuClosed's own guard). Tells
// the loader first -- SetPanelClose synchronously cascades into
// OnPanelClosed/ApplyMenuClosed when the registry agrees the panel was
// open, but calling ApplyMenuClosed here too covers the panel already
// having gone stale in the registry for any reason.
static void CloseDroneMenu()
{
    if (!s_panelHandle || !s_self || !s_self->hooks || !s_self->hooks->UI) return;
    if (!s_menuOpen.load(std::memory_order_relaxed)) return;

    LOG_DEBUG("CloseDroneMenu: requesting SetPanelClose");
    s_self->hooks->UI->SetPanelClose(s_panelHandle);
    ApplyMenuClosed("CloseDroneMenu");
}

// The deliberate user action (the configured toggle key): flip which one of
// Open/Close applies. This is the only path allowed to choose between them
// -- every other path (Escape/Q, the loader's own close notification,
// shutdown) always calls CloseDroneMenu, never this.
void ToggleDroneMenu()
{
    if (s_menuOpen.load(std::memory_order_relaxed))
        CloseDroneMenu();
    else
        OpenDroneMenu();
}

// Applies a pending Escape/Q close request. Called from the game tick (via
// OnEngineTick) rather than from RenderDronePanel: the loader's
// RenderPanelWindows snapshots each panel's isOpen into a local bool BEFORE
// calling our render function and writes that same stale local back to the
// registry AFTER it returns (plugin_panel_registry.cpp, RenderPanelWindows)
// -- so a SetPanelClose called from inside our own render callback closes
// the panel for an instant and then the loader's own snapshot silently
// reopens it one statement later. That's the flicker: the window closing
// and immediately springing back open, left with a released capture token
// but a panel the registry still considers open, which is what stopped
// Escape/Q from working again afterwards. Applying the close from the tick
// instead means it lands on a call stack RenderPanelWindows is never nested
// inside, so there is nothing left for it to stomp.
void TickDroneMenuClose()
{
    if (g_closeRequested.exchange(false, std::memory_order_relaxed))
    {
        LOG_DEBUG("TickDroneMenuClose: applying a pending close request");
        CloseDroneMenu();
    }
}

static void ApplySpeedPreset(const SpeedPreset& preset)
{
    if (!g_drone.valid) return;

    DroneConfig::Config::WriteSpeedPerSec(preset.speedPerSec);
    DroneConfig::Config::WriteBoostMultiplier(preset.boostMultiplier);
    DroneConfig::Config::WriteAcceleration(preset.acceleration);
    DroneConfig::Config::WriteDeceleration(preset.deceleration);
}

static void ApplyRangePreset(const RangePreset& preset)
{
    if (!g_drone.valid) return;

    const float radius = DroneConfig::Config::WriteMaxRadius(preset.maxRadius);
    const float height = DroneConfig::Config::WriteMaxHeight(preset.maxHeight);

    RequestMaxRadius(radius);
    RequestMaxHeight(height);
}

// ---------------------------------------------------------------------------
// Saved presets (PresetStore-backed) -- the owner's own tweaks, named and
// kept apart from the built-in arrays above. Per group: a live-fields
// getter (for Save), an apply function (for picking one from the dropdown),
// a built-in-name check (so a saved preset can never collide with, rename
// onto, or shadow a built-in), and a suggested-base-name computer (the
// "<matched built-in> Custom" / "Custom" rule -- PresetStore::SuggestName
// only handles making that name unique, not choosing it).
// ---------------------------------------------------------------------------

constexpr const char* kSpeedGroup = "Speed";
constexpr int         kSpeedFieldCount = 4;

static void GetLiveSpeedFields(PresetStore::Field* out)
{
    out[0] = { "speedPerSec",     DroneConfig::Config::ReadSpeedPerSec() };
    out[1] = { "boostMultiplier", DroneConfig::Config::ReadBoostMultiplier() };
    out[2] = { "acceleration",    DroneConfig::Config::ReadAcceleration() };
    out[3] = { "deceleration",    DroneConfig::Config::ReadDeceleration() };
}

static void ApplySpeedFields(const PresetStore::Field* fields, int count)
{
    if (count > 0) DroneConfig::Config::WriteSpeedPerSec(fields[0].value);
    if (count > 1) DroneConfig::Config::WriteBoostMultiplier(fields[1].value);
    if (count > 2) DroneConfig::Config::WriteAcceleration(fields[2].value);
    if (count > 3) DroneConfig::Config::WriteDeceleration(fields[3].value);
}

static bool IsBuiltinSpeedName(const char* name)
{
    for (int i = 0; i < k_speedPresetCount; ++i)
        if (strcmp(k_speedPresets[i].label, name) == 0)
            return true;
    return false;
}

static void ComputeSpeedSuggestedBase(char* out, int cap)
{
    const float speed = DroneConfig::Config::ReadSpeedPerSec();
    const float boost = DroneConfig::Config::ReadBoostMultiplier();
    const float accel = DroneConfig::Config::ReadAcceleration();
    const float decel = DroneConfig::Config::ReadDeceleration();

    for (int i = 0; i < k_speedPresetCount; ++i)
    {
        const auto& p = k_speedPresets[i];
        if (std::fabs(speed - p.speedPerSec) <= kActiveEpsilon &&
            std::fabs(boost - p.boostMultiplier) <= kActiveEpsilon &&
            std::fabs(accel - p.acceleration) <= kActiveEpsilon &&
            std::fabs(decel - p.deceleration) <= kActiveEpsilon)
        {
            snprintf(out, cap, "%s Custom", p.label);
            return;
        }
    }
    snprintf(out, cap, "Custom");
}

constexpr const char* kRangeGroup = "Range";
constexpr int         kRangeFieldCount = 2;

static void GetLiveRangeFields(PresetStore::Field* out)
{
    out[0] = { "maxRadius", DroneConfig::Config::ReadMaxRadius() };
    out[1] = { "maxHeight", DroneConfig::Config::ReadMaxHeight() };
}

static void ApplyRangeFields(const PresetStore::Field* fields, int count)
{
    float radius = DroneConfig::Config::ReadMaxRadius();
    float height = DroneConfig::Config::ReadMaxHeight();
    if (count > 0) radius = fields[0].value;
    if (count > 1) height = fields[1].value;

    radius = DroneConfig::Config::WriteMaxRadius(radius);
    height = DroneConfig::Config::WriteMaxHeight(height);
    RequestMaxRadius(radius);
    RequestMaxHeight(height);
}

static bool IsBuiltinRangeName(const char* name)
{
    for (int i = 0; i < k_rangePresetCount; ++i)
        if (strcmp(k_rangePresets[i].label, name) == 0)
            return true;
    return false;
}

static void ComputeRangeSuggestedBase(char* out, int cap)
{
    const float radius = DroneConfig::Config::ReadMaxRadius();
    const float height = DroneConfig::Config::ReadMaxHeight();

    for (int i = 0; i < k_rangePresetCount; ++i)
    {
        const auto& p = k_rangePresets[i];
        if (std::fabs(radius - p.maxRadius) <= kActiveEpsilon &&
            std::fabs(height - p.maxHeight) <= kActiveEpsilon)
        {
            snprintf(out, cap, "%s Custom", p.label);
            return;
        }
    }
    snprintf(out, cap, "Custom");
}

// Persists across frames per group: which saved preset is selected, and
// whatever the rename field/delete confirm/last error are doing right now.
struct PresetRowState
{
    char selected[PresetStore::kMaxNameLen] = {};
    bool renaming = false;
    char renameBuf[PresetStore::kMaxNameLen] = {};
    char errorMsg[96] = {};
};

static PresetRowState s_speedPresetRow;
static PresetRowState s_rangePresetRow;

using GetLiveFieldsFn  = void (*)(PresetStore::Field* out);
using ApplyFieldsFn    = void (*)(const PresetStore::Field* fields, int count);
using IsBuiltinNameFn  = bool (*)(const char* name);
using ComputeSuggestFn = void (*)(char* out, int cap);

// A dropdown of the group's saved presets plus Save/Rename/Delete, in
// addition to (not replacing) the built-in preset buttons above it. Save
// never prompts -- it computes the suggested name itself and selects the
// result, so it stays one click, per the owner's ask.
static void RenderSavedPresetsRow(IModLoaderImGui* ui, const char* idScope, const char* group,
                            PresetStore::Field* fields, int fieldCount,
                            GetLiveFieldsFn getLive, ApplyFieldsFn apply,
                            IsBuiltinNameFn isBuiltin, ComputeSuggestFn computeSuggest,
                            PresetRowState& state)
{
    ui->PushIDStr(idScope);

    char names[16][PresetStore::kMaxNameLen];
    const int count = PresetStore::ListNames(group, names, 16);

    bool selectedStillValid = false;
    for (int i = 0; i < count; ++i)
        if (strcmp(names[i], state.selected) == 0)
            selectedStillValid = true;
    if (!selectedStillValid)
        state.selected[0] = '\0';

    ui->AlignTextToFramePadding();
    ui->Text("Saved Presets");
    ui->SameLine(0.0f, -1.0f);

    ui->SetNextItemWidth(220.0f);
    const char* preview = state.selected[0] ? state.selected : "(none saved)";
    if (ui->BeginCombo("##saved", preview))
    {
        for (int i = 0; i < count; ++i)
        {
            const bool isSelected = (strcmp(names[i], state.selected) == 0);
            if (ui->Selectable(names[i], isSelected))
            {
                snprintf(state.selected, sizeof(state.selected), "%s", names[i]);
                getLive(fields); // seed so a key missing from this preset stays unchanged
                if (PresetStore::Load(group, state.selected, fields, fieldCount))
                    apply(fields, fieldCount);
            }
        }
        ui->EndCombo();
    }

    ui->SameLine(0.0f, -1.0f);
    if (ui->SmallButton("Save"))
    {
        char base[PresetStore::kMaxNameLen];
        computeSuggest(base, sizeof(base));
        char suggested[PresetStore::kMaxNameLen];
        PresetStore::SuggestName(group, base, suggested, sizeof(suggested));

        getLive(fields);
        if (PresetStore::Save(group, suggested, fields, fieldCount))
        {
            snprintf(state.selected, sizeof(state.selected), "%s", suggested);
            state.errorMsg[0] = '\0';
        }
        else
        {
            snprintf(state.errorMsg, sizeof(state.errorMsg), "Could not save -- presets file unavailable.");
        }
    }

    const bool hasSelection = state.selected[0] != '\0';

    ui->SameLine(0.0f, -1.0f);
    ui->BeginDisabled(!hasSelection);
    if (ui->SmallButton("Rename"))
    {
        state.renaming = true;
        snprintf(state.renameBuf, sizeof(state.renameBuf), "%s", state.selected);
        state.errorMsg[0] = '\0';
    }
    ui->EndDisabled();

    char deletePopupId[80];
    snprintf(deletePopupId, sizeof(deletePopupId), "Delete preset?##%s", group);

    ui->SameLine(0.0f, -1.0f);
    ui->BeginDisabled(!hasSelection);
    if (ui->SmallButton("Delete"))
        ui->OpenPopup(deletePopupId, 0);
    ui->EndDisabled();

    if (ui->BeginPopupModal(deletePopupId, nullptr, 0))
    {
        ui->Text("Delete this saved preset?");
        ui->TextDisabled(state.selected);
        ui->Spacing();
        if (ui->SmallButton("Delete##confirm"))
        {
            PresetStore::Delete(group, state.selected);
            state.selected[0] = '\0';
            ui->CloseCurrentPopup();
        }
        ui->SameLine(0.0f, -1.0f);
        if (ui->SmallButton("Cancel##delete"))
            ui->CloseCurrentPopup();
        ui->EndPopup();
    }

    if (state.renaming)
    {
        ui->SetNextItemWidth(200.0f);
        ui->InputText("##rename", state.renameBuf, sizeof(state.renameBuf));

        ui->SameLine(0.0f, -1.0f);
        if (ui->SmallButton("OK##rename"))
        {
            if (isBuiltin(state.renameBuf))
            {
                snprintf(state.errorMsg, sizeof(state.errorMsg), "\"%s\" is a built-in preset name.", state.renameBuf);
            }
            else if (PresetStore::Rename(group, state.selected, state.renameBuf))
            {
                snprintf(state.selected, sizeof(state.selected), "%s", state.renameBuf);
                state.renaming = false;
                state.errorMsg[0] = '\0';
            }
            else
            {
                snprintf(state.errorMsg, sizeof(state.errorMsg), "\"%s\" is already used.", state.renameBuf);
            }
        }

        ui->SameLine(0.0f, -1.0f);
        if (ui->SmallButton("Cancel##rename"))
        {
            state.renaming = false;
            state.errorMsg[0] = '\0';
        }
    }

    if (state.errorMsg[0])
        ui->TextColored(1.0f, 0.4f, 0.4f, 1.0f, state.errorMsg);

    ui->PopID();
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
    // A pending Escape/Q close is applied from TickDroneMenuClose (the game
    // tick), not here -- see its comment for why closing from inside this
    // render callback caused the close to silently undo itself one frame
    // later. By the time this runs, the loader will simply not have called
    // it at all for a frame where the close already landed.
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

    ui->SeparatorText("Speed Units & Movement Tuning");
    ui->TextDisabled("Speed presets modelled on classic construction drone mods:");
    ui->Spacing();

    // Speed and range presets share button labels (Stock, Better
    // Construction, Agile Builder) in the same window -- ImGui derives a
    // widget's ID from its label, so the two loops would otherwise collide.
    ui->PushIDStr("speed_presets");
    for (int i = 0; i < k_speedPresetCount; ++i)
    {
        const auto& preset = k_speedPresets[i];
        if (i > 0) ui->SameLine(0.0f, -1.0f);

        if (ui->SmallButton(preset.label))
        {
            ApplySpeedPreset(preset);
        }
        if (ui->IsItemHovered())
        {
            char tooltipBuf[320];
            float kmh = preset.speedPerSec * 0.036f;
            float mph = preset.speedPerSec * 0.0223693629f;
            snprintf(tooltipBuf, sizeof(tooltipBuf),
                "%s\n\nSpeed: %.1f km/h (%.1f mph | %.0f cm/s) | Boost: %.1fx | Accel: %.0f%s%s",
                preset.tooltip, kmh, mph, preset.speedPerSec, preset.boostMultiplier, preset.acceleration,
                preset.credit ? "\n\n" : "", preset.credit ? preset.credit : "");
            ui->SetTooltip(tooltipBuf);
        }
    }
    ui->PopID();

    ui->Spacing();

    {
        PresetStore::Field speedFields[kSpeedFieldCount];
        RenderSavedPresetsRow(ui, "speed_saved", kSpeedGroup, speedFields, kSpeedFieldCount,
                               &GetLiveSpeedFields, &ApplySpeedFields,
                               &IsBuiltinSpeedName, &ComputeSpeedSuggestedBase, s_speedPresetRow);
    }

    ui->Spacing();

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
    ui->TextDisabled("Range presets modelled on classic construction drone mods:");
    ui->Spacing();

    ui->PushIDStr("range_presets");
    for (int i = 0; i < k_rangePresetCount; ++i)
    {
        const auto& preset = k_rangePresets[i];
        if (i > 0) ui->SameLine(0.0f, -1.0f);

        if (ui->SmallButton(preset.label))
        {
            ApplyRangePreset(preset);
        }
        if (ui->IsItemHovered())
        {
            char tooltipBuf[320];
            snprintf(tooltipBuf, sizeof(tooltipBuf),
                "%s\n\nRange: %.0f m H / %.0f m V (%.0f ft H / %.0f ft V)%s%s",
                preset.tooltip, preset.maxRadius / 100.0f, preset.maxHeight / 100.0f,
                preset.maxRadius * 0.0328084f, preset.maxHeight * 0.0328084f,
                preset.credit ? "\n\n" : "", preset.credit ? preset.credit : "");
            ui->SetTooltip(tooltipBuf);
        }
    }
    ui->PopID();

    ui->Spacing();

    {
        PresetStore::Field rangeFields[kRangeFieldCount];
        RenderSavedPresetsRow(ui, "range_saved", kRangeGroup, rangeFields, kRangeFieldCount,
                               &GetLiveRangeFields, &ApplyRangeFields,
                               &IsBuiltinRangeName, &ComputeRangeSuggestedBase, s_rangePresetRow);
    }

    ui->Spacing();

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
