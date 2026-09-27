#include "drone_audio.h"
#include "drone_config.h"
#include "object_ref.h"
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

        // Discovery (GetAllActorsOfClass for drones and stations) stays on this
        // cadence -- it's the expensive part, walking every actor in the world.
        constexpr float kRescanSeconds = 0.5f;

        // The piloted drone's own components are reapplied every tick (see
        // ApplyDroneVolumes) since the cached list is always tiny -- typically
        // one drone, three components. Station components can't make the same
        // guarantee (a built-out base can have many drone stations), so they
        // stay on a timer instead of every tick, just a faster one than
        // discovery so the audible gap between writes shrinks.
        constexpr float kStationApplySeconds = 0.25f;

        std::atomic<float> g_vol[kVolCount] = { 1.0f, 1.0f, 1.0f, 1.0f };
        std::atomic<bool> g_pendingApply{ false };
        float g_rescanTimer = 0.0f;
        float g_stationApplyTimer = 0.0f;

        // Components found by the last rescan, re-read every kRescanSeconds and
        // written far more often than that (see ApplyDroneVolumes /
        // ApplyStationVolumes). A drone or station can be destroyed between
        // rescans (recall, despawn, world travel), so each entry is an
        // ObjectRef, checked on every use, never a raw pointer.
        struct CachedDrone
        {
            ObjectRef<SDK::UAudioComponent> idle;
            ObjectRef<SDK::UAudioComponent> movement;
            ObjectRef<SDK::UAudioComponent> rotation;
        };
        std::vector<CachedDrone> g_cachedDrones;
        std::vector<ObjectRef<SDK::UAudioComponent>> g_cachedStationComponents;

        // The world the caches above were filled from. A different world
        // means a travel happened, so everything cached belongs to the old one.
        ObjectRef<SDK::UWorld> g_cachedWorld;

        void DropCaches()
        {
            g_cachedDrones.clear();
            g_cachedStationComponents.clear();
            g_cachedWorld.Reset();
        }

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
        void SetComponentVolume(const ObjectRef<SDK::UAudioComponent>& ref, float volume)
        {
            SDK::UAudioComponent* audio = ref.Get();
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

        // Re-discovers drones and drone stations and refills the caches above.
        // Never called more than once per kRescanSeconds -- see Tick().
        void RescanWorld()
        {
            DropCaches();

            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world) return;
            g_cachedWorld.Set(world);

            // Piloted drone audio
            {
                SDK::TArray<SDK::AActor*> actors{};
                SDK::UGameplayStatics::GetAllActorsOfClass(
                    world, SDK::ACrCharacterDroneBase::StaticClass(), &actors);

                for (int32_t i = 0; i < actors.Num(); ++i)
                {
                    auto* drone = static_cast<SDK::ACrCharacterDroneBase*>(actors[i]);
                    if (!drone) continue;

                    CachedDrone cached;
                    cached.idle.Set(drone->IdleSound);
                    cached.movement.Set(drone->MovementSound);
                    cached.rotation.Set(drone->RotationSound);
                    g_cachedDrones.push_back(cached);
                }
            }

            // Building drone station audio
            {
                SDK::TArray<SDK::AActor*> actors{};
                SDK::UGameplayStatics::GetAllActorsOfClass(
                    world, SDK::ACrBuildingActorBase::StaticClass(), &actors);

                for (int32_t i = 0; i < actors.Num(); ++i)
                {
                    auto* building = static_cast<SDK::ACrBuildingActorBase*>(actors[i]);
                    if (!building || !NameHasDrone(building)) continue;

                    SDK::TArray<SDK::UAudioComponent*>& sounds = building->StateAudioComponents;
                    for (int32_t s = 0; s < sounds.Num(); ++s)
                    {
                        if (sounds[s])
                            g_cachedStationComponents.emplace_back(sounds[s]);
                    }
                }
            }
        }

        // Cheap: the cached list is a handful of components (typically one
        // piloted drone, three components) -- safe to walk and
        // compare-before-write every tick, which is what closes the audible
        // gap between the game re-asserting its own volume and us catching it.
        void ApplyDroneVolumes()
        {
            const float idleVol     = g_vol[kVolIdle].load(std::memory_order_relaxed);
            const float movementVol = g_vol[kVolMovement].load(std::memory_order_relaxed);
            const float rotationVol = g_vol[kVolRotation].load(std::memory_order_relaxed);

            for (const CachedDrone& cached : g_cachedDrones)
            {
                SetComponentVolume(cached.idle, idleVol);
                SetComponentVolume(cached.movement, movementVol);
                SetComponentVolume(cached.rotation, rotationVol);
            }
        }

        // Not walked every tick -- a built-out base can have many drone
        // stations, unlike the always-tiny piloted-drone list above. See
        // kStationApplySeconds.
        void ApplyStationVolumes()
        {
            const float stationVol = g_vol[kVolStation].load(std::memory_order_relaxed);

            for (const ObjectRef<SDK::UAudioComponent>& audio : g_cachedStationComponents)
                SetComponentVolume(audio, stationVol);
        }

        void ReadAudioConfig()
        {
            for (int v = 0; v < kVolCount; ++v)
                g_vol[v].store(DroneConfig::Config::ReadAudioVolume(kVolumeKeys[v]), std::memory_order_relaxed);
        }
    }

    void Initialize()
    {
        g_rescanTimer = 0.0f;
        g_stationApplyTimer = 0.0f;
        g_pendingApply.store(false, std::memory_order_relaxed);

        ReadAudioConfig();
    }

    void Shutdown()
    {
        // Plain memory only -- none of the objects behind these are touched.
        DropCaches();
    }

    void OnWorldEnd()
    {
        DropCaches();
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
            // A slider changed -- rescan and apply immediately rather than
            // waiting out the timers, same as before this change. Bounded to
            // however long the user is actively dragging a row.
            g_rescanTimer = 0.0f;
            g_stationApplyTimer = 0.0f;
            if (IsActive())
            {
                RescanWorld();
                ApplyDroneVolumes();
                ApplyStationVolumes();
            }
            return;
        }

        if (!IsActive())
            return;

        // A travel since the last rescan: nothing cached belongs to this
        // world, so rescan now rather than write to the old world's leftovers.
        if (g_cachedWorld.Get() != SDK::UWorld::GetWorld())
            g_rescanTimer = kRescanSeconds;

        g_rescanTimer += deltaSeconds;
        if (g_rescanTimer >= kRescanSeconds)
        {
            g_rescanTimer = 0.0f;
            RescanWorld();
            g_stationApplyTimer = 0.0f;
            ApplyStationVolumes();
        }

        // Every tick: the drone's own few components (see ApplyDroneVolumes).
        ApplyDroneVolumes();

        g_stationApplyTimer += deltaSeconds;
        if (g_stationApplyTimer >= kStationApplySeconds)
        {
            g_stationApplyTimer = 0.0f;
            ApplyStationVolumes();
        }
    }
}
