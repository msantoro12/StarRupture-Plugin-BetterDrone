#include "drone_laser.h"
#include "drone_config.h"
#include "drone_heat_display.h"
#include "drone_interact.h"
#include "drone_key_vk.h"
#include "drone_laser_fx.h"
#include "drone_map_marker.h"
#include "mining_natives.h"
#include "named_object.h"
#include "object_ref.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <GameplayAbilities_classes.hpp>
#include <UMG_classes.hpp>
#include <BP_MiningToolActor_classes.hpp>
#include <GA_MiningToolPassiveCooling_classes.hpp>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

namespace
{
    // The trace channel that reaches both infinite ore veins and meteor ore
    // from the drone camera, and stops on buildings, so the laser cannot mine
    // through a wall. It is the second of the project's 32 trace slots.
    constexpr SDK::ETraceTypeQuery kOreTraceChannel = SDK::ETraceTypeQuery::TraceTypeQuery2;

    constexpr const char* kMiningToolActorCdoName = "Default__BP_MiningToolActor_C";
    constexpr const char* kCoolingAbilityCdoName  = "Default__GA_MiningToolPassiveCooling_C";

    // The stock mining tool's heat: GE_WeaponHeatOverTimeBase adds one stack
    // of GE_WeaponHeatStackBase every 0.2 s of firing, up to its limit of
    // 100, and the passive cooling ability takes one off every
    // SingleStackDuration once its start delay has run out (a longer delay
    // after an overheat). The laser keeps a count of its own with the same
    // numbers; the drill's own heat is not touched.
    constexpr float kHeatStackSeconds = 0.2f;
    constexpr int   kHeatStackLimit   = 100;

    // GA_MiningToolPassiveCooling's stock values, for while its defaults are
    // not loaded.
    constexpr float kStockStackCoolSeconds  = 0.82f;
    constexpr float kStockCoolDelay         = 8.0f;
    constexpr float kStockOverheatCoolDelay = 15.0f;

    // UCrInventoryComponent::GetAmountCanAdd(item defaults, amount): how
    // many of an item fit in the bag, up to the amount asked. It is the same
    // space check the mining grant passes before adding anything, and the
    // grant adds all or nothing, so a grant fits exactly when this returns
    // the whole amount. A pure read.
    constexpr const char* kPatGetAmountCanAdd =
        "40 55 41 55 41 56 48 83 EC 30 45 8B F0 48 8B EA 4C 8B E9 48 85 D2 75 ?? 33 C0 48 83 C4 30 41 5E 41 5D 5D C3 48 63 81 ?? ?? ?? ??";
    using GetAmountCanAddFn = int32_t(__fastcall*)(void* inventory, const void* itemData, int32_t amount);

    uintptr_t g_addrMineActor      = 0;
    uintptr_t g_addrMineIsm        = 0;
    uintptr_t g_addrGetAmountCanAdd = 0;

    // Written by the key callback, read by the tick.
    std::atomic<bool>  g_keyHeld{ false };
    std::atomic<int>   g_keyVk{ 0 };
    std::atomic<DWORD> g_gameThreadId{ 0 };

    // The names the key is registered under, kept so exactly those are
    // unregistered again.
    char g_keyPressedName[64]  = {};
    char g_keyReleasedName[64] = {};

    enum class OreKind { None, Actor, Vein };

    // What the trace picked this tick, and where the beam ends: the hit, or
    // the end of the range.
    struct Aim
    {
        OreKind             kind = OreKind::None;
        SDK::AActor*        ore  = nullptr;   // OreKind::Actor
        SDK::UPhysicalMaterial* vein = nullptr;   // OreKind::Vein
        SDK::FVector        point{};
    };

    // Game thread only.
    struct LaserState
    {
        // The target the laser last asked to mine, or last refused. mining
        // says whether the request took, so a stop is owed.
        OreKind                             kind = OreKind::None;
        ObjectRef<SDK::AActor>              ore;
        ObjectRef<SDK::UPhysicalMaterial>   vein;
        ObjectRef<SDK::UCrMiningComponent>  comp;
        bool                                mining = false;

        // Set by a full bag, an overheated drill or laser, or a refusal that
        // holding the key cannot fix; cleared when the key is let go.
        bool                                latched = false;

        // The key's state last tick, to tell a fresh press.
        bool                                keyWasHeld = false;

        // The mining tool's item data: its damage, range and hit interval.
        // BetterCheats' Mining Damage, Range and Hit Rate rows write into this
        // same object, so the laser follows them.
        NamedObject                         toolData{ MiningNatives::kMiningToolCdoName };

        // The mining tool actor's defaults, for the tag it is overheated by.
        NamedObject                         toolActor{ kMiningToolActorCdoName };

        // The passive cooling ability's defaults, for its cooling numbers.
        // BetterCheats' Overheat Cooldown row writes into the same object.
        NamedObject                         coolingAbility{ kCoolingAbilityCdoName };

        const char*                         lastGate = nullptr;
    };

    LaserState g_s;

    // The laser's heat, in the stock tool's stacks. Game thread only.
    struct Heat
    {
        int   stacks     = 0;
        float riseClock  = 0.0f;    // firing time toward the next stack
        float delay      = 0.0f;    // left before cooling starts
        float drainClock = 0.0f;    // cooling time toward the next stack off
        bool  firing     = false;   // last tick
        bool  overheated = false;   // locked out until the heat is gone

        float stackCoolSeconds  = kStockStackCoolSeconds;
        float coolDelay         = kStockCoolDelay;
        float overheatCoolDelay = kStockOverheatCoolDelay;
    };

    Heat g_heat;

    // What the tick decided: whether the laser fires, and at what.
    struct Frame
    {
        SDK::ACrCharacterPlayerBase* character = nullptr;
        bool                         firing    = false;
        DroneLaserFx::Shot           shot{};
    };

    void LogGateOnce(const char* reason)
    {
        if (g_s.lastGate == reason)
            return;

        g_s.lastGate = reason;
        LOG_DEBUG("DroneLaser: key held, not mining -- %s", reason);
    }

    // ---- the key ----------------------------------------------------------------

    void OnLaserKey(EModKey, EModKeyEvent event)
    {
        g_keyHeld.store(event == EModKeyEvent::Pressed, std::memory_order_relaxed);
    }

    // The keybind's state, confirmed against the key itself where it maps to
    // a VK: a Released lost to a focus change or to a modifier let go first
    // must not leave the laser on.
    bool KeyHeld()
    {
        if (!g_keyHeld.load(std::memory_order_relaxed))
            return false;

        const int vk = g_keyVk.load(std::memory_order_relaxed);
        if (vk == 0)
            return true;

        return (GetAsyncKeyState(vk) & 0x8000) != 0 && GameHasFocus();
    }

    // A named combo's Released only fires on an exact modifier match, so the
    // release is registered under the bare base key, as the boost key does.
    void RegisterLaserKey(IPluginSelf* self, const char* keyName)
    {
        if (!self || !self->hooks->Input || !keyName || !keyName[0])
            return;

        char baseKey[64] = {};
        ExtractBaseKey(keyName, baseKey, sizeof(baseKey));

        self->hooks->Input->RegisterKeybindByName(keyName, EModKeyEvent::Pressed,  &OnLaserKey);
        self->hooks->Input->RegisterKeybindByName(baseKey, EModKeyEvent::Released, &OnLaserKey);
        snprintf(g_keyPressedName,  sizeof(g_keyPressedName),  "%s", keyName);
        snprintf(g_keyReleasedName, sizeof(g_keyReleasedName), "%s", baseKey);
        g_keyVk.store(KeyNameToVk(baseKey), std::memory_order_relaxed);
    }

    void UnregisterLaserKey(IPluginSelf* self)
    {
        if (self && self->hooks->Input)
        {
            if (g_keyPressedName[0])
                self->hooks->Input->UnregisterKeybindByName(g_keyPressedName, EModKeyEvent::Pressed, &OnLaserKey);
            if (g_keyReleasedName[0])
                self->hooks->Input->UnregisterKeybindByName(g_keyReleasedName, EModKeyEvent::Released, &OnLaserKey);
        }

        g_keyPressedName[0]  = '\0';
        g_keyReleasedName[0] = '\0';
        g_keyHeld.store(false, std::memory_order_relaxed);
        g_keyVk.store(0, std::memory_order_relaxed);
    }

    // ---- the mining tool --------------------------------------------------------

    // ---- trace --------------------------------------------------------------

    // What is under the crosshair within range: an ore actor (meteor ore or
    // a chunk, reached through any of its meshes, weak spots included), or an
    // instanced mesh's physical material, which may be an infinite ore vein.
    // The native request decides that; anything else is no target.
    Aim TraceAim(SDK::ACrCharacterPlayerBase* character, double range)
    {
        // The character's eyes are the drone camera while the drone is out:
        // ActivateBuildingDrone makes it the active camera.
        SDK::FVector eye{};
        SDK::FRotator rot{};
        character->GetActorEyesViewPoint(&eye, &rot);

        const SDK::FVector dir = SDK::UKismetMathLibrary::GetForwardVector(rot);
        SDK::FVector end{};
        end.X = eye.X + dir.X * range;
        end.Y = eye.Y + dir.Y * range;
        end.Z = eye.Z + dir.Z * range;

        // The drone has collision of its own and the camera sits inside it;
        // bIgnoreSelf covers only the character. The array is a view over this
        // stack slot, which the engine copies before the call returns.
        SDK::AActor* ignoreData[1] = { character->BuildingDrone };
        const SDK::TArray<SDK::AActor*> ignore(ignoreData, ignoreData[0] ? 1 : 0, 1);
        const SDK::FLinearColor colour{};

        SDK::FHitResult hit{};
        const bool blocked = SDK::UKismetSystemLibrary::LineTraceSingle(character, eye, end, kOreTraceChannel,
            false, ignore, SDK::EDrawDebugTrace::None, &hit, true, colour, colour, 0.0f);

        Aim aim;
        aim.point = (blocked && hit.bBlockingHit) ? hit.ImpactPoint : end;

        SDK::UPrimitiveComponent* comp = (blocked && hit.bBlockingHit) ? hit.Component.Get() : nullptr;
        if (!comp)
            return aim;

        // Meteor ores and their chunks only: crops are ore actors too, and
        // are left to the player.
        SDK::AActor* owner = comp->GetOwner();
        if (owner && (owner->IsA(SDK::ACrMeteOreActor::StaticClass())
                   || owner->IsA(SDK::ACrStandaloneMeteOreChunk::StaticClass())))
        {
            aim.kind = OreKind::Actor;
            aim.ore  = owner;
            return aim;
        }

        SDK::UPhysicalMaterial* material = hit.PhysMaterial.Get();
        if (material && comp->IsA(SDK::UInstancedStaticMeshComponent::StaticClass()))
        {
            aim.kind = OreKind::Vein;
            aim.vein = material;
        }
        return aim;
    }

    bool SameTarget(const Aim& aim)
    {
        if (aim.kind != g_s.kind)
            return false;

        switch (aim.kind)
        {
        case OreKind::Actor: return g_s.ore.Get() == aim.ore;
        case OreKind::Vein:  return g_s.vein.Get() == aim.vein;
        default:             return true;
        }
    }

    // Whether the component's mining state still holds the target the laser
    // started.
    bool StillMining(SDK::UCrMiningComponent* comp)
    {
        const SDK::FCrOreMiningState& state = comp->CurrentlyMinedOre;
        if (g_s.kind == OreKind::Actor)
            return state.OreActor && state.OreActor == g_s.ore.Get();
        return state.InfiniteOrePhysicalMaterial && state.InfiniteOrePhysicalMaterial == g_s.vein.Get();
    }

    // The mining damage multiplier the stock tool's damage carries:
    // GetMiningDamage scales the item's MiningTypeDamage by the gem attribute
    // HarvesterMiningDamageMultiplier. Its weak-spot and boost terms are left
    // out, as the laser drives neither.
    float DamageMultiplier(SDK::ACrCharacterPlayerBase* character)
    {
        SDK::UCrGemAttributeSet* gems = character->GemAttributes;
        return gems ? gems->HarvesterMiningDamageMultiplier.CurrentValue : 1.0f;
    }

    // Whether the player's ability system carries the mining tool's
    // OverheatTag, the tag the stock tool refuses to fire under. The tag
    // count covers loose and effect-granted tags alike. A pure read; the
    // laser's own heat is kept apart. While the tool actor's defaults are not
    // loaded there is no tag to check, and the laser is not held back.
    bool Overheated(SDK::ACrCharacterPlayerBase* character)
    {
        auto* toolActor = static_cast<SDK::ABP_MiningToolActor_C*>(
            g_s.toolActor.Resolve(SDK::ACrWeaponActor::StaticClass()));
        SDK::UCrAbilitySystemComponent* abilities = character->AbilitySystem;
        if (!toolActor || !abilities || toolActor->OverheatTag.TagName.IsNone())
            return false;

        return abilities->GetGameplayTagCount(toolActor->OverheatTag) > 0;
    }

    // ---- the bag ---------------------------------------------------------------

    // Items the game is about to put in the bag, summed per item.
    struct Grant
    {
        SDK::UClass* resource;
        int64_t      count;
    };

    void AddGrant(std::vector<Grant>& grants, SDK::UClass* resource, int64_t count)
    {
        if (!resource || count <= 0)
            return;

        for (Grant& grant : grants)
        {
            if (grant.resource == resource)
            {
                grant.count += count;
                return;
            }
        }
        grants.push_back({ resource, count });
    }

    // Whether the player's bag takes every grant whole. The game adds each
    // grant all or nothing, behind the same space walk as GetAmountCanAdd,
    // and drops what does not fit at the player's body. GetAmountCanAdd
    // counts an item's room in its own part-filled stacks plus in every empty
    // slot; the empty slots are shared, so what each item cannot put in its
    // own stacks must fit the empty slots together. Anything unknown (the
    // native not found, no inventory, an item's defaults not created yet)
    // counts as room, and the game's own handling applies.
    bool BagHasRoom(SDK::ACrCharacterPlayerBase* character, const std::vector<Grant>& grants)
    {
        if (!g_addrGetAmountCanAdd || grants.empty())
            return true;

        SDK::UCrInventoryComponent* inventory = character->BP_GetInventory();
        if (!inventory)
            return true;

        int64_t emptySlots = 0;
        for (int32_t i = 0; i < inventory->Slots.Num(); ++i)
        {
            const SDK::FGuid& id = inventory->Slots[i].ItemId.Handle;
            if ((id.A | id.B | id.C | id.D) == 0)
                ++emptySlots;
        }

        const auto amountCanAdd = reinterpret_cast<GetAmountCanAddFn>(g_addrGetAmountCanAdd);
        int64_t slotsNeeded = 0;
        for (const Grant& grant : grants)
        {
            auto* item = static_cast<SDK::UAuItemDataBase*>(grant.resource->ClassDefaultObject);
            if (!item)
                continue;

            const int64_t stack     = item->MaxStack > 1 ? item->MaxStack : 1;
            const int64_t room      = amountCanAdd(inventory, item, INT32_MAX);
            const int64_t inStacks  = room - emptySlots * stack;
            const int64_t remaining = grant.count - (inStacks > 0 ? inStacks : 0);
            if (remaining > 0)
                slotsNeeded += (remaining + stack - 1) / stack;
        }
        return slotsNeeded <= emptySlots;
    }

    // What spending an ore actor grants. The ore gives everything it holds
    // in one go when its damage threshold is reached. A meteor ore then also
    // gives, for each of its weak spots, what a weak spot not yet mined
    // holds, plus between InitMinCount and InitMaxCount of the weak spot's
    // resource whether it was mined or not; the most is reserved. A spent
    // ore grants nothing.
    std::vector<Grant> OreGrants(SDK::AActor* ore)
    {
        std::vector<Grant> grants;
        auto* oreActor = static_cast<SDK::ACrOreActor*>(ore);
        if (oreActor->OreData.bIsDepleted || oreActor->OreData.CurrentResourceCount <= 0)
            return grants;

        AddGrant(grants, oreActor->Resource, oreActor->OreData.MaxResourceCount);

        const auto& spots = oreActor->WeakSpotsDataContainer.Items;
        for (int32_t i = 0; i < spots.Num(); ++i)
        {
            const SDK::FCrWeakSpotRuntimeData& spot = spots[i];
            const int64_t held = spot.CurrentMiningHealth > 0.0f ? spot.ResourceCount : 0;
            AddGrant(grants, spot.Resource, held + oreActor->InitMaxCount);
        }
        return grants;
    }

    bool BagHasRoomForOre(SDK::ACrCharacterPlayerBase* character, SDK::AActor* ore)
    {
        return BagHasRoom(character, OreGrants(ore));
    }

    // A vein grants GrantingMomentResourceCount each time its damage
    // threshold is reached.
    bool BagHasRoomForVein(SDK::ACrCharacterPlayerBase* character, const SDK::FCrInfiniteOreData& data)
    {
        std::vector<Grant> grants;
        AddGrant(grants, data.Resource, data.GrantingMomentResourceCount);
        return BagHasRoom(character, grants);
    }

    // The ore data of a vein's physical material, as the ore subsystem maps
    // it, for the check before the first request: that request can already
    // grant. Null when the material is not a vein.
    const SDK::FCrInfiniteOreData* VeinOreData(SDK::ACrCharacterPlayerBase* character, SDK::UPhysicalMaterial* material)
    {
        auto* ores = static_cast<SDK::UCrOreSubsystem*>(
            SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(character, SDK::UCrOreSubsystem::StaticClass()));
        SDK::UCrInfiniteOrePhysMatData* mapping = ores ? ores->InfiniteOrePhysMatMappingData : nullptr;
        if (!mapping)
            return nullptr;

        // Not const: the SDK's const TMap indexer does not compile.
        auto& veins = mapping->PhysMatOreDataMap;
        for (int32_t i = 0; i < veins.NumAllocated(); ++i)
        {
            if (veins.IsValidIndex(i) && veins[i].Key() == material)
                return &veins[i].Value();
        }
        return nullptr;
    }

    // Whether the target being mined can still be granted into the bag. A
    // vein's ore data is the copy the request made into the component.
    bool BagHasRoomForNextGrant(SDK::ACrCharacterPlayerBase* character, SDK::UCrMiningComponent* comp)
    {
        if (g_s.kind == OreKind::Actor)
        {
            SDK::AActor* ore = g_s.ore.Get();
            return !ore || BagHasRoomForOre(character, ore);
        }

        return BagHasRoomForVein(character, comp->CurrentlyMinedOre.InfiniteOreData);
    }

    // Whether a new target can be started: both kinds can grant on the
    // request itself.
    bool BagHasRoomToStart(SDK::ACrCharacterPlayerBase* character, const Aim& aim)
    {
        if (aim.kind == OreKind::Actor)
            return BagHasRoomForOre(character, aim.ore);

        const SDK::FCrInfiniteOreData* data = VeinOreData(character, aim.vein);
        return !data || BagHasRoomForVein(character, *data);
    }

    // ---- stop and start -------------------------------------------------------

    // The stop the stock game makes when the mining beam is let go:
    // BP_OnMiningStopped releases the ore's claim on the player, clears the
    // mining state, removes the mining tags and switches the component's tick
    // off. It takes the ore actor for actor ore and null for a vein. Forgets
    // the target either way.
    void StopMining(const char* reason)
    {
        if (g_s.mining)
        {
            if (SDK::UCrMiningComponent* comp = g_s.comp.Get())
            {
                SDK::AActor* ore = g_s.kind == OreKind::Actor ? g_s.ore.Get() : nullptr;
                comp->BP_OnMiningStopped(ore);
            }
            LOG_INFO("DroneLaser: stopped mining (%s)", reason);
        }

        g_s.mining = false;
        g_s.kind   = OreKind::None;
        g_s.ore.Reset();
        g_s.vein.Reset();
        g_s.comp.Reset();
    }

    void StopAndLatch(const char* reason)
    {
        StopMining(reason);
        if (!g_s.latched)
            LOG_INFO("DroneLaser: off until the key is pressed again (%s)", reason);
        g_s.latched = true;
    }

    // Remembers the target whether or not the request takes, so a refused
    // target is not asked for again every tick while it stays in the
    // crosshair.
    void StartMining(SDK::UCrMiningComponent* comp, const Aim& aim, float damage, float interval)
    {
        g_s.kind = aim.kind;
        g_s.ore.Set(aim.ore);
        g_s.vein.Set(aim.vein);
        g_s.comp.Set(comp);
        g_s.mining = false;

        const SDK::FCrOreMiningState& state = comp->CurrentlyMinedOre;
        bool took = false;

        if (aim.kind == OreKind::Actor)
        {
            // No weak spot: the socket is NAME_None and the flag false.
            reinterpret_cast<MiningNatives::MineActorFn>(g_addrMineActor)(comp, aim.ore, 0, damage, interval, false);
            took = state.OreActor == aim.ore;
        }
        else
        {
            // The request checks the material itself and clears the state
            // for anything that is not a vein the player may harvest.
            reinterpret_cast<MiningNatives::MineIsmFn>(g_addrMineIsm)(comp, aim.vein, damage, interval);
            took = state.InfiniteOrePhysicalMaterial == aim.vein;
        }

        if (!took)
            return;

        // The native request leaves the component's tick, which is what
        // applies the damage, as it was; the stock blueprint wrapper turns it
        // on after the request, and so does this.
        comp->SetComponentTickEnabled(true);
        g_s.mining = true;

        LOG_INFO("DroneLaser: mining %s (%s), damage %.2f, %.3f s between hits",
            aim.kind == OreKind::Actor ? aim.ore->GetName().c_str() : aim.vein->GetName().c_str(),
            aim.kind == OreKind::Actor ? aim.ore->Class->GetName().c_str() : "infinite ore vein",
            damage, interval);
    }

    // ---- heat -------------------------------------------------------------------

    // The cooling numbers from the passive cooling ability's defaults, read
    // as each firing starts so a change (BetterCheats' row) is picked up.
    // Anything out of reason keeps the stock value.
    void ReadCoolingNumbers()
    {
        auto* cooling = static_cast<SDK::UGA_MiningToolPassiveCooling_C*>(
            g_s.coolingAbility.Resolve(SDK::UGA_MiningToolPassiveCooling_C::StaticClass()));
        if (!cooling)
            return;

        auto pick = [](double value, float fallback)
        {
            return (value >= 0.0 && value <= 600.0) ? static_cast<float>(value) : fallback;
        };

        const float stack = pick(cooling->SingleStackDuration, kStockStackCoolSeconds);
        g_heat.stackCoolSeconds  = stack > 0.01f ? stack : kStockStackCoolSeconds;
        g_heat.coolDelay         = pick(cooling->DefaultStartDelayDuration, kStockCoolDelay);
        g_heat.overheatCoolDelay = pick(cooling->OverheatStartDelayDuration, kStockOverheatCoolDelay);
    }

    // One tick of heat. Firing adds a stack every kHeatStackSeconds and holds
    // off the cooling; let go, the heat waits out the delay and then drains a
    // stack at a time. Returns true on the tick the laser overheats.
    bool UpdateHeat(Heat& h, bool firing, float deltaSeconds)
    {
        if (firing)
        {
            if (!h.firing)
                h.riseClock = 0.0f;
            h.firing     = true;
            h.delay      = h.coolDelay;
            h.drainClock = 0.0f;

            h.riseClock += deltaSeconds;
            while (h.riseClock >= kHeatStackSeconds && h.stacks < kHeatStackLimit)
            {
                h.riseClock -= kHeatStackSeconds;
                ++h.stacks;
            }

            if (h.stacks < kHeatStackLimit)
                return false;

            h.firing     = false;
            h.riseClock  = 0.0f;
            h.overheated = true;
            h.delay      = h.overheatCoolDelay;
            return true;
        }

        h.firing    = false;
        h.riseClock = 0.0f;

        if (h.delay > 0.0f)
        {
            h.delay -= deltaSeconds;
            if (h.delay > 0.0f)
                return false;
            deltaSeconds = -h.delay;
            h.delay      = 0.0f;
        }

        if (h.stacks > 0)
        {
            h.drainClock += deltaSeconds;
            while (h.drainClock >= h.stackCoolSeconds && h.stacks > 0)
            {
                h.drainClock -= h.stackCoolSeconds;
                --h.stacks;
            }
        }

        if (h.stacks == 0)
        {
            h.overheated = false;
            h.drainClock = 0.0f;
        }
        return false;
    }

    // The heat as a smooth 0..1, for the beam and the ring: the stacks, plus
    // the part of the next one on its way in or out.
    float HeatLevel(const Heat& h)
    {
        float stacks = static_cast<float>(h.stacks);
        if (h.firing)
            stacks += h.riseClock / kHeatStackSeconds;
        else if (h.delay <= 0.0f && h.stacks > 0)
            stacks -= h.drainClock / h.stackCoolSeconds;

        const float level = stacks / static_cast<float>(kHeatStackLimit);
        return level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
    }

    // The ring shows in the drone while there is heat, menus aside.
    void PublishHeat()
    {
        const float level = HeatLevel(g_heat);
        SDK::ACrCharacterPlayerBase* character = level > 0.0f ? LocalPlayerCharacter() : nullptr;
        if (!character || !IsLocalPlayerInDrone() || IsGameMenuOpen())
        {
            PublishDroneHeat(nullptr);
            return;
        }

        DroneHeatRing::View view{};
        view.heat       = level;
        view.phase      = g_heat.firing ? DroneHeatRing::Phase::Heating
                        : g_heat.delay > 0.0f ? DroneHeatRing::Phase::Holding
                        : DroneHeatRing::Phase::Draining;
        view.overheated = g_heat.overheated;
        view.scale      = SDK::UWidgetLayoutLibrary::GetViewportScale(character);
        if (!(view.scale > 0.0f))
            view.scale = 1.0f;
        view.baseColour = PlayerMarkerColour();
        PublishDroneHeat(&view);
    }

    // ---- the tick -------------------------------------------------------------

    void TickImpl(Frame& frame)
    {
        const bool held    = KeyHeld();
        const bool pressed = held && !g_s.keyWasHeld;
        g_s.keyWasHeld = held;

        if (!held)
        {
            StopMining("key released");
            g_s.latched  = false;
            g_s.lastGate = nullptr;
            return;
        }

        if (g_s.latched)
            return;

        if (!g_addrMineActor || !g_addrMineIsm)
        {
            StopAndLatch("the game's mining requests were not found in this build");
            return;
        }

        SDK::ACrCharacterPlayerBase* character = LocalPlayerCharacter();
        if (!character || !IsLocalPlayerInDrone())
        {
            StopMining("not in the drone");
            LogGateOnce("not in the drone");
            return;
        }

        if (character->bDead)
        {
            StopMining("character dead");
            return;
        }

        // On a client the mining requests change nothing: the server owns
        // the ore.
        if (!SDK::UKismetSystemLibrary::IsServer(character))
        {
            StopAndLatch("host and single player only");
            return;
        }

        if (IsGameMenuOpen())
        {
            StopMining("a menu is open");
            LogGateOnce("a menu is open");
            return;
        }

        // Locked out until the heat is gone, with the tool's refusal click
        // for a press, as the tool does.
        if (g_heat.overheated)
        {
            if (pressed)
                DroneLaserFx::PlayReject(character);
            StopAndLatch("laser overheated, cooling down");
            return;
        }

        SDK::UCrMiningComponent* comp = character->MiningComponent;
        if (!comp)
        {
            StopMining("no mining component");
            LogGateOnce("no mining component");
            return;
        }

        // A different component (a new character after travel or respawn):
        // the old one, if it still exists, is stopped first.
        if (g_s.mining && g_s.comp.Get() != comp)
            StopMining("character changed");

        auto* tool = static_cast<SDK::UCrWeaponItemDataBase*>(
            g_s.toolData.Resolve(SDK::UCrWeaponItemDataBase::StaticClass()));
        if (!tool)
        {
            StopMining("mining tool data not loaded");
            LogGateOnce("mining tool data not loaded");
            return;
        }

        const float damage   = tool->MiningTypeDamage * DamageMultiplier(character);
        const float interval = tool->RoundsPerMinute.Value;
        const float range    = tool->BaseRange.Value;
        if (!(damage > 0.0f) || !(interval > 0.0f) || !(range > 0.0f))
        {
            StopMining("mining tool has no damage, rate or range");
            LogGateOnce("mining tool has no damage, rate or range");
            return;
        }

        if (Overheated(character))
        {
            if (pressed)
                DroneLaserFx::PlayReject(character);
            StopAndLatch("drill overheated");
            return;
        }

        // From here the laser fires: at ore, or into the air like the tool.
        const Aim aim = TraceAim(character, range);
        if (!SameTarget(aim))
        {
            StopMining(aim.kind == OreKind::None ? "target lost or out of range" : "target changed");
            if (aim.kind == OreKind::None)
            {
                LogGateOnce("no ore in the crosshair within range");
            }
            else if (!BagHasRoomToStart(character, aim))
            {
                StopAndLatch("inventory full");
                return;
            }
            else
            {
                StartMining(comp, aim, damage, interval);
            }
        }
        else if (g_s.mining && !StillMining(comp))
        {
            // The game dropped it: the ore ran out, or something else
            // cleared the state. Stopped once, and not asked for again while
            // it stays in the crosshair.
            StopMining("ore depleted or released");
            g_s.kind = aim.kind;
            g_s.ore.Set(aim.ore);
            g_s.vein.Set(aim.vein);
        }

        if (g_s.mining && !BagHasRoomForNextGrant(character, comp))
        {
            StopAndLatch("inventory full");
            return;
        }

        frame.character    = character;
        frame.firing       = true;
        frame.shot.hit     = aim.point;
        frame.shot.onOre   = aim.kind != OreKind::None;
        frame.shot.mining  = g_s.mining;
    }
}

void ResolveDroneLaser(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    auto resolve = [&](const char* name, const char* pattern)
    {
        PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
        req.hookName = name;
        req.pattern  = pattern;
        req.kind     = PLUGIN_SCAN_FUNCTION_START;
        req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;
        return scanner->Resolve(self, &req);
    };

    g_addrMineActor = resolve("UCrMiningComponent::MineResourceRequest(actor)", MiningNatives::kPatMineActor);
    g_addrMineIsm   = resolve("UCrMiningComponent::MineResourceRequest(ISM)",   MiningNatives::kPatMineIsm);
    g_addrGetAmountCanAdd = resolve("UCrInventoryComponent::GetAmountCanAdd", kPatGetAmountCanAdd);
}

void InitDroneLaser(IPluginSelf* self)
{
    if (!self || !self->hooks)
        return;

    char keyName[64] = {};
    DroneConfig::Config::ReadMiningLaserKey(keyName, sizeof(keyName));
    RegisterLaserKey(self, keyName);

    if (!g_addrMineActor || !g_addrMineIsm)
        LOG_WARN("DroneLaser: the game's mining requests were not found -- the mining laser is off");
    else
        LOG_INFO("DroneLaser: mining laser on '%s' while the drone is out", keyName);

    if (!g_addrGetAmountCanAdd)
        LOG_WARN("DroneLaser: the game's inventory space check was not found -- a full bag will not stop the laser");

    InitDroneHeatDisplay(self);
}

void RebindDroneLaserKey(IPluginSelf* self, const char* newKeyName)
{
    UnregisterLaserKey(self);
    RegisterLaserKey(self, newKeyName);
}

void ShutdownDroneLaser(IPluginSelf* self)
{
    UnregisterLaserKey(self);

    // Shutdown also runs at process exit and, on older loaders, from the
    // render thread. Game objects are only touched from the game thread;
    // anywhere else the stop is left to the game.
    const bool onGameThread = GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed);
    DroneLaserFx::Shutdown(onGameThread);
    ShutdownDroneHeatDisplay(self);

    if (g_s.mining && !onGameThread)
        LOG_WARN("DroneLaser: shut down off the game thread while mining; the stop is left to the game");
    else
        StopMining("plugin shutdown");

    g_s    = LaserState{};
    g_heat = Heat{};
}

void TickDroneLaser(float deltaSeconds)
{
    g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

    Frame frame;
    TickImpl(frame);

    if (frame.firing && !g_heat.firing)
        ReadCoolingNumbers();

    // Heat runs every tick, in the drone or not, so it cools on foot too.
    if (UpdateHeat(g_heat, frame.firing, deltaSeconds))
    {
        frame.firing = false;
        StopAndLatch("laser overheated");
    }

    if (frame.firing)
    {
        frame.shot.heat = HeatLevel(g_heat);
        DroneLaserFx::Fire(frame.character, frame.shot);
    }
    else
    {
        DroneLaserFx::Stop();
    }

    PublishHeat();
}
