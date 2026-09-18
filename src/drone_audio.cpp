#include "drone_audio.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <cmath>
#include <cstdio>
#include <string>

namespace DroneAudio
{
    namespace
    {
        constexpr float kActiveEpsilon = 0.0001f;
        constexpr float kReapplySeconds = 0.5f;

        enum VolIndex : int { kVolIdle = 0, kVolMovement, kVolRotation, kVolStation, kVolCount };

        struct VolDef { const char* key; const char* description; };

        const VolDef kVols[kVolCount] = {
            { "IdleVolume",     "Volume multiplier for drone idle hum (0.0 to 1.0)" },
            { "MovementVolume", "Volume multiplier for drone movement sound (0.0 to 1.0)" },
            { "RotationVolume", "Volume multiplier for drone rotation sound (0.0 to 1.0)" },
            { "StationVolume",  "Volume multiplier for drone station audio (0.0 to 1.0)" },
        };

        IPluginSelf* s_self = nullptr;
        float g_vol[kVolCount] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float g_timer = 0.0f;
        bool g_pendingApply = false;

        bool IsActive()
        {
            for (int v = 0; v < kVolCount; ++v)
            {
                if (std::fabs(g_vol[v] - 1.0f) > kActiveEpsilon)
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

                    SetComponentVolume(drone->IdleSound,     g_vol[kVolIdle]);
                    SetComponentVolume(drone->MovementSound, g_vol[kVolMovement]);
                    SetComponentVolume(drone->RotationSound, g_vol[kVolRotation]);
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
                        SetComponentVolume(sounds[s], g_vol[kVolStation]);
                }
            }
        }

        void ReadAudioConfig()
        {
            if (!s_self) return;

            for (int v = 0; v < kVolCount; ++v)
            {
                float val = s_self->config->ReadFloat(s_self, "Audio", kVols[v].key, 1.0f);
                if (val < 0.0f) val = 0.0f;
                if (val > 1.0f) val = 1.0f;
                g_vol[v] = val;
            }
        }
    }

    void Initialize(IPluginSelf* self)
    {
        s_self = self;
        g_timer = 0.0f;
        g_pendingApply = false;

        ReadAudioConfig();
    }

    void Shutdown()
    {
        s_self = nullptr;
    }

    void ApplySavedConfig()
    {
        ReadAudioConfig();
        g_pendingApply = true;
    }

    void OnConfigChanged(const char* section, const char* /*key*/, const char* /*newValue*/)
    {
        if (!section || strcmp(section, "Audio") != 0)
            return;

        ReadAudioConfig();
        g_pendingApply = true;
    }

    void Tick(float deltaSeconds)
    {
        if (g_pendingApply)
        {
            g_pendingApply = false;
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
