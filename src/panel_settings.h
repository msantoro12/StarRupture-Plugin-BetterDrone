#pragma once
#include "drone_config.h"

#include <iterator>

// What changing a panel setting does: the cached value, the drone, and the
// panel file, for each kind of edit (a slider drag, a commit, a reset, a
// built-in preset, the Master Volume row). The F10 panel's rows call these
// instead of carrying the handler bodies themselves, so anything else that
// needs to change a panel setting runs the very same code. Nothing here
// draws.
//
// SetLive touches only the cache and atomics. Commit and the preset
// appliers also write BetterDrone-Panel.ini, and DroneConfig serializes
// those file writes behind one lock. That is all the lock covers: the
// panel calls everything here from the render thread, and a range
// setting's cache update and its drone request are two separate steps, so
// keep to one writer per setting. The preset appliers also read
// g_drone.valid, which the game thread writes without synchronization.
namespace PanelSettings
{
    // Values closer than this count as equal: a row is "at its default", a
    // set of values "matches" a built-in preset.
    constexpr float kActiveEpsilon = 0.0001f;

    // Clamps and caches `value`, and applies it to the drone: a range
    // setting queues its CDO update, a volume goes straight to the audio
    // atomic. Does not touch the file -- for a slider mid-drag. Returns the
    // clamped value.
    float SetLive(DroneConfig::PanelFloat id, float value);

    // Persists the cached value. Called once an edit is done, not on every
    // drag step.
    void Commit(DroneConfig::PanelFloat id);

    // Master Volume is a "set all" convenience, not a fifth stored value: it
    // derives its display from the four volumes, so it can never drift out
    // of sync with them.
    float MasterVolume();
    void  SetMasterVolumeLive(float value);
    void  CommitMasterVolume();

    // Speed and range used to come as one bundled preset; split so either
    // axis can be picked independently (e.g. Better Construction speed with
    // a Map-wide range).
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

    inline constexpr SpeedPreset kSpeedPresets[] = {
        { "Stock",
          "Default un-modded StarRupture building drone speed.",
          "Game Default",
          // 4000.0f matches DroneConfig's own accel/decel floor (drone_config.cpp,
          // kMinAccelDecel) rather than 0, which no longer means instant.
          1000.0f, 2.0f, 4000.0f, 4000.0f },

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
    inline constexpr int kSpeedPresetCount = static_cast<int>(std::size(kSpeedPresets));

    // Every entry stays within DroneConfig's radius and height bounds
    // (kMaxRadiusBound/kMaxHeightBound in drone_config.cpp); Map-wide sits
    // exactly at that ceiling.
    inline constexpr RangePreset kRangePresets[] = {
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
    inline constexpr int kRangePresetCount = static_cast<int>(std::size(kRangePresets));

    // The index of the built-in preset whose values equal the current ones,
    // or -1 when they match none ("Custom").
    int MatchingSpeedPreset();
    int MatchingRangePreset();

    // Applies and persists a built-in preset. No-ops until the drone
    // settings CDO has been found.
    void ApplySpeedPreset(const SpeedPreset& preset);
    void ApplyRangePreset(const RangePreset& preset);

    // Sets and persists all four speed values. The one body behind a speed
    // preset and a saved speed preset.
    void ApplySpeed(float speedPerSec, float boostMultiplier, float acceleration, float deceleration);

    // Sets and persists both range values, then queues the drone update.
    // The one body behind a range preset and a saved range preset. Unlike
    // ApplyRangePreset it does not check that the drone settings CDO has
    // been found, and neither does the saved-preset path that calls it.
    void ApplyRange(float radius, float height);
}
