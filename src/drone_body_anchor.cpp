#include "drone_body_anchor.h"
#include "drone_interact.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <atomic>
#include <windows.h>

namespace
{
    // The stock drone range (BuildingDroneMaxRadius), in metres. Inside it
    // the game never unloads the parked character's cells, so this keeps the
    // same ground the stock drone keeps, no more. A shape spans whole cells,
    // so the loaded patch is a little larger than the circle.
    constexpr float kDefaultRadiusM = 40.0f;
    constexpr float kMinRadiusM     = 20.0f;
    constexpr float kMaxRadiusM     = 200.0f;

    std::atomic<bool>  g_enabled{ true };
    std::atomic<float> g_radiusCm{ kDefaultRadiusM * 100.0f };
    std::atomic<DWORD> g_gameThreadId{ 0 };

    using Source = SDK::UWorldPartitionStreamingSourceComponent;

    ObjectRef<Source>                      g_source;
    ObjectRef<SDK::ACrCharacterPlayerBase> g_owner;
    bool                                   g_warnedNoAllocator = false;

    // The anchor's one shape lives in engine memory: the component frees its
    // Shapes array through the engine's allocator when it is destroyed.
    Source* Create(SDK::ACrCharacterPlayerBase* character, float radiusCm)
    {
        IPluginMemoryUtils* memory = GetSelf()->hooks->Memory;
        if (!memory || !memory->IsAllocatorAvailable())
        {
            if (!g_warnedNoAllocator)
                LOG_WARN("DroneBodyAnchor: the engine allocator is unavailable; the area around the body is not kept loaded");
            g_warnedNoAllocator = true;
            return nullptr;
        }

        auto* shape = static_cast<SDK::FStreamingSourceShape*>(
            memory->Alloc(sizeof(SDK::FStreamingSourceShape), alignof(SDK::FStreamingSourceShape)));
        if (!shape)
            return nullptr;

        *shape = SDK::FStreamingSourceShape{};
        shape->bUseGridLoadingRange = false;
        shape->LoadingRangeScale    = 1.0f;
        shape->Radius               = radiusCm;

        SDK::FTransform identity{};
        identity.Rotation.W = 1.0;
        identity.Scale3D    = SDK::FVector{ 1.0, 1.0, 1.0 };

        auto* source = static_cast<Source*>(
            character->AddComponentByClass(Source::StaticClass(), false, identity, false));
        if (!source)
        {
            memory->Free(shape);
            return nullptr;
        }

        source->Shapes = SDK::TArray<SDK::FStreamingSourceShape>(shape, 1, 1);
        // Activated, not just loaded: the floor's collision and the base's
        // actors have to be live when the player is back.
        source->TargetState = SDK::EStreamingSourceTargetState::Activated;
        source->EnableStreamingSource();
        return source;
    }

    void Remove()
    {
        if (Source* source = g_source.Get())
        {
            source->DisableStreamingSource();
            source->K2_DestroyComponent(source);
            LOG_INFO("DroneBodyAnchor: removed the area anchor");
        }
        g_source.Reset();
        g_owner.Reset();
    }
}

void SetDroneBodyAnchorEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
}

void SetDroneBodyAnchorRadiusMeters(float meters)
{
    if (meters < kMinRadiusM) meters = kMinRadiusM;
    if (meters > kMaxRadiusM) meters = kMaxRadiusM;
    g_radiusCm.store(meters * 100.0f, std::memory_order_relaxed);
}

void TickDroneBodyAnchor()
{
    g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

    SDK::ACrCharacterPlayerBase* character = LocalPlayerCharacter();
    const bool wanted = g_enabled.load(std::memory_order_relaxed) && character && !character->bDead &&
                        character->Status == SDK::EPlayerCharacterStatus::BuildingDrone;

    // A respawned or reloaded character is a different object; the old
    // anchor went with it.
    if (g_source.Get() && (!wanted || g_owner.Get() != character))
        Remove();

    if (!wanted)
        return;

    const float radiusCm = g_radiusCm.load(std::memory_order_relaxed);
    if (Source* source = g_source.Get())
    {
        if (source->Shapes.Num() == 1 && source->Shapes[0].Radius != radiusCm)
            source->Shapes[0].Radius = radiusCm;
        return;
    }

    Source* source = Create(character, radiusCm);
    if (!source)
        return;

    g_source.Set(source);
    g_owner.Set(character);
    LOG_INFO("DroneBodyAnchor: keeping %.0f m around the body loaded", radiusCm / 100.0f);
}

void ResetDroneBodyAnchor()
{
    Remove();
}

void ShutdownDroneBodyAnchor()
{
    if (GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed))
        Remove();
    else if (g_source.Get())
        LOG_WARN("DroneBodyAnchor: shut down off the game thread with the anchor out; the engine drops it with the character");

    g_source.Reset();
    g_owner.Reset();
}
