#include "drone_laser_fx.h"
#include "named_object.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <Niagara_classes.hpp>
#include <chrono>
#include <cstdint>

namespace
{
    // Where the beam starts, in the drone camera's frame (cm): ahead of the
    // lens and below the bottom edge of the screen, a little to the right, so
    // the beam rises into the crosshair and the effect's muzzle stays out of
    // view.
    constexpr double kMuzzleForward = 40.0;
    constexpr double kMuzzleRight   = 10.0;
    constexpr double kMuzzleUp      = -30.0;

    // How long a loop fades when it stops.
    constexpr float kLoopFadeSeconds = 0.1f;

    // A released beam winds down and destroys itself; one still there after
    // this long is destroyed outright.
    constexpr uint64_t kBeamWindDownMs = 3000;

    // The mining tool's own assets (BP_MiningToolActor's beam component, its
    // audio components' sounds and its overheat refusal sound). They are hard
    // references of the tool, so they are loaded whenever the tool is.
    struct Assets
    {
        NamedObject beam        { "NS_HarvesterResized" };
        NamedObject start       { "S_Harvester_Rotate_Default_Start_Cue" };
        NamedObject loop        { "S_Harvester_Rotate_Default_Loop_Cue" };
        NamedObject finished    { "S_Harvester_Rotate_Default_Finished_Cue" };
        NamedObject ore         { "S_Wpn_Hv_Stones_Loop_01_Cue" };
        NamedObject reject      { "S_Harvester_Trigger" };
        NamedObject attenuation { "ATT_MiningLaser" };
    };

    // The beam's parameters, under the names the tool's blueprint sets them
    // by. HitLocation is in the effect component's own space: the blueprint
    // passes the hit through InverseTransformLocation of the component's
    // transform. TPP is true for any view but the tool's first-person one.
    struct ParamNames
    {
        SDK::FName hitLocation, hitDistance, active, aiming, heat, cooling, tpp;
        bool       built = false;
    };

    // Game thread only.
    struct FxState
    {
        bool                              firing = false;
        ObjectRef<SDK::USceneComponent>   anchor;
        ObjectRef<SDK::UNiagaraComponent> beam;
        ObjectRef<SDK::UNiagaraComponent> fadingBeam;   // deactivated, finishing its particles
        uint64_t                          fadingSinceMs = 0;
        ObjectRef<SDK::UAudioComponent>   laserLoop;
        ObjectRef<SDK::UAudioComponent>   oreLoop;
        bool                              loggedFirstShot = false;
    };

    Assets     g_assets;
    ParamNames g_names;
    FxState    g_fx;

    uint64_t NowMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void BuildNames()
    {
        if (g_names.built)
            return;

        g_names.hitLocation = SDK::BasicFilesImplUtils::StringToName(L"HitLocation");
        g_names.hitDistance = SDK::BasicFilesImplUtils::StringToName(L"HitTargetDistance");
        g_names.active      = SDK::BasicFilesImplUtils::StringToName(L"isLaserActive");
        g_names.aiming      = SDK::BasicFilesImplUtils::StringToName(L"isAimingAtHarvestable");
        g_names.heat        = SDK::BasicFilesImplUtils::StringToName(L"Heat");
        g_names.cooling     = SDK::BasicFilesImplUtils::StringToName(L"IsCooling");
        g_names.tpp         = SDK::BasicFilesImplUtils::StringToName(L"TPP");
        g_names.built       = true;
    }

    SDK::USoundBase* Sound(NamedObject& slot)
    {
        return static_cast<SDK::USoundBase*>(slot.Resolve(SDK::USoundBase::StaticClass()));
    }

    // The drone camera: the view the laser aims along.
    SDK::USceneComponent* Anchor(SDK::ACrCharacterPlayerBase* character)
    {
        SDK::ACrCharacterDroneBase* drone = character ? character->BuildingDrone : nullptr;
        return drone ? drone->DroneCamera : nullptr;
    }

    // A sound on the drone camera, at an offset (KeepRelativeOffset) or at a
    // world point (KeepWorldPosition). It destroys itself once it stops, and
    // stops if the camera goes.
    SDK::UAudioComponent* PlayOn(SDK::USoundBase* sound, SDK::USceneComponent* anchor,
                                 const SDK::FVector& location, SDK::EAttachLocation where)
    {
        if (!sound || !anchor)
            return nullptr;

        auto* attenuation = static_cast<SDK::USoundAttenuation*>(
            g_assets.attenuation.Resolve(SDK::USoundAttenuation::StaticClass()));
        return SDK::UGameplayStatics::SpawnSoundAttached(sound, anchor, SDK::FName(), location, SDK::FRotator{},
            where, true, 1.0f, 1.0f, 0.0f, attenuation, nullptr, true);
    }

    SDK::UAudioComponent* PlayAtAnchor(SDK::USoundBase* sound, SDK::USceneComponent* anchor)
    {
        return PlayOn(sound, anchor, SDK::FVector{}, SDK::EAttachLocation::KeepRelativeOffset);
    }

    void FadeOut(ObjectRef<SDK::UAudioComponent>& ref)
    {
        if (SDK::UAudioComponent* audio = ref.Get())
            audio->FadeOut(kLoopFadeSeconds, 0.0f, SDK::EAudioFaderCurve::Linear);
        ref.Reset();
    }

    void Destroy(ObjectRef<SDK::UNiagaraComponent>& ref)
    {
        if (SDK::UNiagaraComponent* beam = ref.Get())
            beam->K2_DestroyComponent(beam->GetOwner());
        ref.Reset();
    }

    SDK::UNiagaraComponent* SpawnBeam(SDK::USceneComponent* anchor)
    {
        auto* system = static_cast<SDK::UNiagaraSystem*>(g_assets.beam.Resolve(SDK::UNiagaraSystem::StaticClass()));
        if (!system)
            return nullptr;

        SDK::FVector muzzle{};
        muzzle.X = kMuzzleForward;
        muzzle.Y = kMuzzleRight;
        muzzle.Z = kMuzzleUp;

        SDK::UNiagaraComponent* beam = SDK::UNiagaraFunctionLibrary::SpawnSystemAttached(system, anchor, SDK::FName(),
            muzzle, SDK::FRotator{}, SDK::EAttachLocation::KeepRelativeOffset, false, true, SDK::ENCPoolMethod::None, false);
        if (beam)
            beam->SetVariableBool(g_names.tpp, true);
        return beam;
    }

    void DriveBeam(SDK::UNiagaraComponent* beam, const DroneLaserFx::Shot& shot)
    {
        const SDK::FVector local = SDK::UKismetMathLibrary::InverseTransformLocation(beam->K2_GetComponentToWorld(), shot.hit);
        beam->SetVariableVec3(g_names.hitLocation, local);
        beam->SetVariableFloat(g_names.hitDistance, static_cast<float>(beam->K2_GetComponentLocation().GetDistanceTo(shot.hit)));
        beam->SetVariableBool(g_names.active, true);
        beam->SetVariableBool(g_names.aiming, shot.mining);
        beam->SetVariableFloat(g_names.heat, shot.heat);
        beam->SetVariableBool(g_names.cooling, false);
    }

    // Once per session: what was found, and where the listener is. The audio
    // listener sits on the player's camera unless the game overrides it, so a
    // camera far from the drone would leave the drone's sounds out of earshot.
    void LogFirstShot(SDK::ACrCharacterPlayerBase* character, SDK::USceneComponent* anchor)
    {
        if (g_fx.loggedFirstShot)
            return;
        g_fx.loggedFirstShot = true;

        SDK::APlayerCameraManager* camera = SDK::UGameplayStatics::GetPlayerCameraManager(character, 0);
        const double listener = camera ? camera->GetCameraLocation().GetDistanceTo(anchor->K2_GetComponentLocation()) : -1.0;
        LOG_INFO("DroneLaser: effects -- beam %s, laser loop %s, ore loop %s; camera %.0f cm from the drone camera",
            g_fx.beam.Get() ? "on" : "not loaded",
            g_fx.laserLoop.Get() ? "on" : "not loaded",
            Sound(g_assets.ore) ? "found" : "not loaded",
            listener);
    }
}

namespace DroneLaserFx
{
    void Fire(SDK::ACrCharacterPlayerBase* character, const Shot& shot)
    {
        SDK::USceneComponent* anchor = Anchor(character);
        if (!anchor)
        {
            Stop();
            return;
        }

        // A different drone camera (a new drone): start over on it.
        if (g_fx.firing && g_fx.anchor.Get() != anchor)
            Stop();

        BuildNames();

        if (!g_fx.firing)
        {
            g_fx.firing = true;
            g_fx.anchor.Set(anchor);
            PlayAtAnchor(Sound(g_assets.start), anchor);
            g_fx.laserLoop.Set(PlayAtAnchor(Sound(g_assets.loop), anchor));
            g_fx.beam.Set(SpawnBeam(anchor));
            LogFirstShot(character, anchor);
        }

        if (SDK::UNiagaraComponent* beam = g_fx.beam.Get())
            DriveBeam(beam, shot);

        // The grinding loop sits at the hit point while ore is being mined.
        SDK::UAudioComponent* ore = g_fx.oreLoop.Get();
        if (!shot.mining)
            FadeOut(g_fx.oreLoop);
        else if (ore)
            ore->K2_SetWorldLocation(shot.hit, false, nullptr, true);
        else
            g_fx.oreLoop.Set(PlayOn(Sound(g_assets.ore), anchor, shot.hit, SDK::EAttachLocation::KeepWorldPosition));
    }

    void Stop()
    {
        if (g_fx.fadingSinceMs && NowMs() - g_fx.fadingSinceMs > kBeamWindDownMs)
        {
            Destroy(g_fx.fadingBeam);
            g_fx.fadingSinceMs = 0;
        }

        FadeOut(g_fx.laserLoop);
        FadeOut(g_fx.oreLoop);

        // The beam winds down the way the tool's does, then destroys itself.
        // One still winding down from the last release is cut short.
        if (SDK::UNiagaraComponent* beam = g_fx.beam.Get())
        {
            Destroy(g_fx.fadingBeam);
            beam->SetVariableBool(g_names.active, false);
            beam->SetVariableBool(g_names.cooling, true);
            beam->SetAutoDestroy(true);
            beam->Deactivate();
            g_fx.fadingBeam.Set(beam);
            g_fx.fadingSinceMs = NowMs();
        }
        g_fx.beam.Reset();

        if (g_fx.firing)
        {
            if (SDK::USceneComponent* anchor = g_fx.anchor.Get())
                PlayAtAnchor(Sound(g_assets.finished), anchor);
        }
        g_fx.firing = false;
        g_fx.anchor.Reset();
    }

    void PlayReject(SDK::ACrCharacterPlayerBase* character)
    {
        PlayAtAnchor(Sound(g_assets.reject), Anchor(character));
    }

    void Shutdown(bool onGameThread)
    {
        if (onGameThread)
        {
            if (SDK::UAudioComponent* audio = g_fx.laserLoop.Get())
                audio->Stop();
            if (SDK::UAudioComponent* audio = g_fx.oreLoop.Get())
                audio->Stop();
            Destroy(g_fx.beam);
            Destroy(g_fx.fadingBeam);
        }

        g_fx     = FxState{};
        g_assets = Assets{};
        g_names  = ParamNames{};
    }
}
