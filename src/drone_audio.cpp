#include "drone_audio.h"
#include "drone_config.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace DroneAudio
{
    namespace
    {
        constexpr float kActiveEpsilon = 0.0001f;
        constexpr float kReapplySeconds = 0.5f;

        std::atomic<float> g_vol[kVolCount] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float g_timer = 0.0f;
        std::atomic<bool> g_pendingApply{ false };

        // ------------------------------------------------------------------
        // TEMPORARY DIAGNOSTIC -- added 2026-09-27, remove once the drone's
        // "cycling" audio source is confirmed (see
        // drafts/drone-audio-progress.log, not part of this repo).
        //
        // The owner reports the cycling survives our Master Volume at 0.
        // Master Volume only ever writes IdleVolume/MovementVolume/
        // RotationVolume/StationVolume (kVolumeKeys above), which map to
        // IdleSound/MovementSound/RotationSound and station
        // StateAudioComponents below. But the SDK dump shows
        // ACrCharacterDroneBase also owns a second, separate sound table --
        // TMap<EBuildingDroneAudioSoundType, USoundBase*> Sounds, with
        // Idle/IdleTPP/Movement/MovementTPP/CameraMovement/
        // CameraMovementTPP/Start/StartTPP/Stop/StopTPP/Option entries --
        // that this plugin has never read or written. Dumper-7 only
        // reflects that map as a UPROPERTY; nothing in the SDK shows which
        // native or Blueprint call plays each entry, or whether it spawns a
        // persistent attached UAudioComponent or a fire-and-forget one, so
        // rather than guess, this logs every UAudioComponent actually
        // attached to a live drone. Remove this whole Diag block (and its
        // one call from Tick()) once the owner's log capture identifies the
        // component or confirms it never appears as one.
        namespace Diag
        {
            constexpr float kSummarySeconds = 5.0f;
            constexpr size_t kMaxTracked = 128;

            std::vector<void*> g_seen;
            float g_summaryTimer = 0.0f;

            bool AlreadySeen(void* ptr)
            {
                for (void* p : g_seen)
                    if (p == ptr) return true;
                return false;
            }

            void Remember(void* ptr)
            {
                if (g_seen.size() >= kMaxTracked)
                    g_seen.erase(g_seen.begin());
                g_seen.push_back(ptr);
            }

            void LogComponent(SDK::AActor* owner, SDK::UAudioComponent* audio)
            {
                if (!audio || AlreadySeen(audio)) return;
                Remember(audio);

                std::string ownerClass = (owner && owner->Class) ? owner->Class->GetName() : "<unknown>";
                std::string compName   = audio->GetName();
                std::string soundName  = audio->Sound ? audio->Sound->GetName() : "<none>";
                std::string attenName  = audio->AttenuationSettings ? audio->AttenuationSettings->GetName() : "<none>";

                LOG_INFO("DroneAudioDiag: new UAudioComponent owner='%s' comp='%s' sound='%s' vol=%.3f UISound=%d attenuation='%s'",
                    ownerClass.c_str(), compName.c_str(), soundName.c_str(),
                    audio->VolumeMultiplier, audio->bIsUISound ? 1 : 0, attenName.c_str());
            }

            // Enumerates every UAudioComponent actually attached to the drone,
            // not just the three named fields this plugin already knows
            // about, so a component reachable only via the Sounds map (or
            // spawned one-shot) still gets logged the moment it exists.
            void ScanDrone(SDK::AActor* drone, int32_t& liveAudioCount)
            {
                if (!drone) return;

                SDK::TArray<SDK::UActorComponent*> comps =
                    drone->K2_GetComponentsByClass(SDK::UAudioComponent::StaticClass());

                liveAudioCount += comps.Num();
                for (int32_t c = 0; c < comps.Num(); ++c)
                    LogComponent(drone, static_cast<SDK::UAudioComponent*>(comps[c]));
            }

            void Tick(float deltaSeconds)
            {
                SDK::UWorld* world = nullptr;
                try { world = SDK::UWorld::GetWorld(); }
                catch (...) { return; }
                if (!world) return;

                SDK::TArray<SDK::AActor*> drones{};
                try
                {
                    SDK::UGameplayStatics::GetAllActorsOfClass(
                        world, SDK::ACrCharacterDroneBase::StaticClass(), &drones);
                }
                catch (...) { return; }
                if (drones.Num() == 0) return;

                int32_t liveAudioCount = 0;
                for (int32_t i = 0; i < drones.Num(); ++i)
                {
                    try { ScanDrone(drones[i], liveAudioCount); }
                    catch (...) {}
                }

                g_summaryTimer += deltaSeconds;
                if (g_summaryTimer >= kSummarySeconds)
                {
                    g_summaryTimer = 0.0f;
                    LOG_INFO("DroneAudioDiag: summary -- %d drone(s), %d live UAudioComponent(s), %zu tracked total",
                        drones.Num(), liveAudioCount, g_seen.size());
                }
            }
        }
        // ------------------------------------------------------------------

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
        // TEMPORARY DIAGNOSTIC -- see the Diag namespace above. Runs
        // independently of the volume-reapply logic below (it should log
        // even while every channel is at its default 1.0).
        Diag::Tick(deltaSeconds);

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
