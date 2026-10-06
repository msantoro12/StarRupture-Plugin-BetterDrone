#include "panel_settings.h"
#include "drone_audio.h"
#include "drone_settings.h"

#include <cmath>

namespace PanelSettings
{
    using DroneConfig::Config;
    using DroneConfig::PanelFloat;

    float SetLive(PanelFloat id, float value)
    {
        const float applied = Config::SetPanelLive(id, value);

        switch (id)
        {
        case PanelFloat::MaxRadius:
            RequestMaxRadius(applied);
            break;
        case PanelFloat::MaxHeight:
            RequestMaxHeight(applied);
            break;
        case PanelFloat::IdleVolume:
        case PanelFloat::MovementVolume:
        case PanelFloat::RotationVolume:
        case PanelFloat::StationVolume:
            DroneAudio::SetVolume(
                DroneAudio::kVolumeKeys[static_cast<int>(id) - static_cast<int>(PanelFloat::IdleVolume)], applied);
            break;
        default:
            break;
        }

        return applied;
    }

    void Commit(PanelFloat id)
    {
        Config::PersistPanel(id);
    }

    float MasterVolume()
    {
        float vols[DroneAudio::kVolCount];
        for (int i = 0; i < DroneAudio::kVolCount; ++i)
            vols[i] = Config::ReadPanel(DroneConfig::AudioVolumeId(i));

        float maxV = vols[0];
        bool allEqual = true;
        for (int i = 0; i < DroneAudio::kVolCount; ++i)
        {
            if (std::fabs(vols[i] - vols[0]) > kActiveEpsilon) allEqual = false;
            if (vols[i] > maxV) maxV = vols[i];
        }

        return allEqual ? vols[0] : maxV;
    }

    // Updates the drone's live audio immediately (an atomic store, same as
    // a single row's own drag path) and the cache the rows below read from,
    // so they visibly track a master drag; does not touch BetterDrone-Panel.ini.
    void SetMasterVolumeLive(float value)
    {
        for (int i = 0; i < DroneAudio::kVolCount; ++i)
            SetLive(DroneConfig::AudioVolumeId(i), value);
    }

    // Persists all four volumes to BetterDrone-Panel.ini. Called once the
    // edit is done, not on every drag step; SetMasterVolumeLive already
    // updated the cache each of those already wrote to.
    void CommitMasterVolume()
    {
        for (int i = 0; i < DroneAudio::kVolCount; ++i)
            Commit(DroneConfig::AudioVolumeId(i));
    }

    int MatchingSpeedPreset()
    {
        const float speed = Config::ReadSpeedPerSec();
        const float boost = Config::ReadBoostMultiplier();
        const float accel = Config::ReadAcceleration();
        const float decel = Config::ReadDeceleration();

        for (int i = 0; i < kSpeedPresetCount; ++i)
        {
            const SpeedPreset& p = kSpeedPresets[i];
            if (std::fabs(speed - p.speedPerSec) <= kActiveEpsilon &&
                std::fabs(boost - p.boostMultiplier) <= kActiveEpsilon &&
                std::fabs(accel - p.acceleration) <= kActiveEpsilon &&
                std::fabs(decel - p.deceleration) <= kActiveEpsilon)
                return i;
        }
        return -1;
    }

    int MatchingRangePreset()
    {
        const float radius = Config::ReadMaxRadius();
        const float height = Config::ReadMaxHeight();

        for (int i = 0; i < kRangePresetCount; ++i)
        {
            const RangePreset& p = kRangePresets[i];
            if (std::fabs(radius - p.maxRadius) <= kActiveEpsilon &&
                std::fabs(height - p.maxHeight) <= kActiveEpsilon)
                return i;
        }
        return -1;
    }

    void ApplySpeedPreset(const SpeedPreset& preset)
    {
        if (!g_drone.valid) return;

        ApplySpeed(preset.speedPerSec, preset.boostMultiplier, preset.acceleration, preset.deceleration);
    }

    void ApplyRangePreset(const RangePreset& preset)
    {
        if (!g_drone.valid) return;

        ApplyRange(preset.maxRadius, preset.maxHeight);
    }

    void ApplySpeed(float speedPerSec, float boostMultiplier, float acceleration, float deceleration)
    {
        Config::WritePanel(PanelFloat::SpeedPerSec, speedPerSec);
        Config::WritePanel(PanelFloat::BoostMultiplier, boostMultiplier);
        Config::WritePanel(PanelFloat::Acceleration, acceleration);
        Config::WritePanel(PanelFloat::Deceleration, deceleration);
    }

    void ApplyRange(float radius, float height)
    {
        radius = Config::WritePanel(PanelFloat::MaxRadius, radius);
        height = Config::WritePanel(PanelFloat::MaxHeight, height);
        RequestMaxRadius(radius);
        RequestMaxHeight(height);
    }
}
