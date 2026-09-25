#include "drone_audio.h"
#include "drone_config.h"
#include <Chimera_classes.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace DroneAudio
{
    namespace
    {
        constexpr float kActiveEpsilon = 0.0001f;
        constexpr float kReapplySeconds = 0.5f;

        std::atomic<float> g_vol[kVolCount] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float g_timer = 0.0f;
        std::atomic<bool> g_pendingApply{ false };

        bool IsActive()
        {
            for (int v = 0; v < kVolCount; ++v)
            {
                if (std::fabs(g_vol[v].load(std::memory_order_relaxed) - 1.0f) > kActiveEpsilon)
                    return true;
            }
            return false;
        }

        // Compare before writing.
        // SetVolumeMultiplier is not free of side effects -- re-asserting a value the
        // component already had, twice a second, was audible as a faint cycling
        // on/off artifact even with the volume at zero. Reading VolumeMultiplier back
        // and writing only on a real difference makes the steady state completely
        // silent.
        void SetComponentVolume(SDK::UAudioComponent* audio, float volume)
        {
            if (!audio) return;
            if (std::fabs(audio->VolumeMultiplier - volume) <= kActiveEpsilon) return;

            audio->SetVolumeMultiplier(volume);
        }

        bool NameHasDrone(SDK::UObject* obj)
        {
            if (!obj) return false;

            std::string name = obj->GetName();
            for (char& c : name)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');

            return name.find("drone") != std::string::npos;
        }

        void ApplyToWorld()
        {
            SDK::UWorld* world = nullptr;
            try { world = SDK::UWorld::GetWorld(); }
            catch (...) { return; }
            if (!world) return;

            // Piloted drone audio
            {
                SDK::TArray<SDK::AActor*> actors{};
                SDK::UGameplayStatics::GetAllActorsOfClass(
                    world, SDK::ACrCharacterDroneBase::StaticClass(), &actors);

                for (int32_t i = 0; i < actors.Num(); ++i)
                {
                    auto* drone = static_cast<SDK::ACrCharacterDroneBase*>(actors[i]);
                    if (!drone) continue;

                    SetComponentVolume(drone->IdleSound,     g_vol[kVolIdle].load(std::memory_order_relaxed));
                    SetComponentVolume(drone->MovementSound, g_vol[kVolMovement].load(std::memory_order_relaxed));
                    SetComponentVolume(drone->RotationSound, g_vol[kVolRotation].load(std::memory_order_relaxed));
                }
            }

            // Building drone station audio
            {
                SDK::TArray<SDK::AActor*> actors{};
                SDK::UGameplayStatics::GetAllActorsOfClass(
                    world, SDK::ACrBuildingActorBase::StaticClass(), &actors);

                const float stationVol = g_vol[kVolStation].load(std::memory_order_relaxed);
                for (int32_t i = 0; i < actors.Num(); ++i)
                {
                    auto* building = static_cast<SDK::ACrBuildingActorBase*>(actors[i]);
                    if (!building || !NameHasDrone(building)) continue;

                    SDK::TArray<SDK::UAudioComponent*>& sounds = building->StateAudioComponents;
                    for (int32_t s = 0; s < sounds.Num(); ++s)
                        SetComponentVolume(sounds[s], stationVol);
                }
            }
        }

        void ReadAudioConfig()
        {
            for (int v = 0; v < kVolCount; ++v)
                g_vol[v].store(DroneConfig::Config::ReadAudioVolume(kVolumeKeys[v]), std::memory_order_relaxed);
        }
    }

    void Initialize()
    {
        g_timer = 0.0f;
        g_pendingApply.store(false, std::memory_order_relaxed);

        ReadAudioConfig();
    }

    void Shutdown()
    {
    }

    void ApplySavedConfig()
    {
        ReadAudioConfig();
        g_pendingApply.store(true, std::memory_order_relaxed);
    }

    void SetVolume(const char* key, float value)
    {
        if (!key) return;

        for (int v = 0; v < kVolCount; ++v)
        {
            if (strcmp(kVolumeKeys[v], key) != 0)
                continue;

            const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
            g_vol[v].store(clamped, std::memory_order_relaxed);
            g_pendingApply.store(true, std::memory_order_relaxed);
            return;
        }
    }

    void Tick(float deltaSeconds)
    {
        if (g_pendingApply.exchange(false, std::memory_order_relaxed))
        {
            g_timer = 0.0f;
            if (IsActive())
                ApplyToWorld();
            return;
        }

        if (!IsActive())
            return;

        g_timer += deltaSeconds;
        if (g_timer < kReapplySeconds)
            return;

        g_timer = 0.0f;
        ApplyToWorld();
    }
}
