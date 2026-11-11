#pragma once

#include <plugin_interface.h>
#include <Chimera_classes.hpp>
#include <Chimera_structs.hpp>
#include <ChimeraUI_classes.hpp>
#include <AuActorPlacement_classes.hpp>
#include <AuItems_structs.hpp>
#include <AuWeapon_classes.hpp>
#include <AuWeapon_structs.hpp>
#include <Engine_classes.hpp>
#include <plugin_map.h>
#include "plugin_helpers.h"

#include <cstdint>
#include <vector>

// Every member of a game class BetterDrone reads or writes, taken from the
// loader's map for the running game build (see plugin_map.h). Resolved once in
// OnPluginLoadHooks by DroneFields::Resolve; read on the game thread with
// In(object), which is null when the object is null. Structs that are only
// stepped through (an array's elements, a map's pairs) are covered by a size
// check instead of a field per member. Engine members (AActor, ACharacter,
// UWidget, FHitResult, ...) stay compiled in.

#define DRONE_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

// The map area's pan and zoom: a native UE::Math::TMatrix<float>, row major,
// scale on the diagonal and the translation in the last row.
struct MapAreaMatrixData
{
	float m[4][4];
};

// The hotbar subsystem's pages: a native TArray (data pointer, Num, Max) of
// 16-byte entries. Only Num is read, as the page count.
struct HotbarArrayData
{
	void*   Data;
	int32_t Num;
	int32_t Max;
};

namespace DroneFields
{
	// The player character
	inline auto BuildingDrone     = DRONE_FIELD(ACrCharacterPlayerBase, BuildingDrone);
	inline auto Status            = DRONE_FIELD(ACrCharacterPlayerBase, Status);
	inline auto DroneCamera       = DRONE_FIELD(ACrCharacterPlayerBase, DroneCamera);
	inline auto DroneCameraColl   = DRONE_FIELD(ACrCharacterPlayerBase, DroneCameraColl);
	inline auto IsDead            = DRONE_FIELD(ACrCharacterPlayerBase, bDead);
	inline auto GemAttributes     = DRONE_FIELD(ACrCharacterPlayerBase, GemAttributes);
	inline auto MiningComponent   = DRONE_FIELD(ACrCharacterPlayerBase, MiningComponent);
	inline auto WeaponSystem      = DRONE_FIELD(ACrCharacterPlayerBase, WeaponSystem);
	inline auto WeaponAttributes  = DRONE_FIELD(ACrCharacterPlayerBase, WeaponAttributes);
	inline auto AbilitySystem     = DRONE_FIELD(ACrCharacterPlayerBase, AbilitySystem);

	// The drone
	// The drone's own camera. The character has a DroneCamera member too, at
	// another offset: read each one off its own class.
	inline auto DroneOwnCamera              = DRONE_FIELD(ACrCharacterDroneBase, DroneCamera);
	inline auto SphereComponent             = DRONE_FIELD(ACrCharacterDroneBase, SphereComponent);
	inline auto PlacementDeveloperSettings  = DRONE_FIELD(ACrCharacterDroneBase, PlacementDeveloperSettings);
	inline auto IdleSound                   = DRONE_FIELD(ACrCharacterDroneBase, IdleSound);
	inline auto MovementSound               = DRONE_FIELD(ACrCharacterDroneBase, MovementSound);
	inline auto RotationSound               = DRONE_FIELD(ACrCharacterDroneBase, RotationSound);
	inline auto StateAudioComponents        = DRONE_FIELD(ACrBuildingActorBase, StateAudioComponents);

	// The attenuation the player character plays its system voice with.
	inline auto AttenuationFPP = DRONE_FIELD(ACrCharacterPlayerBase, AttenuationFPP);
	inline auto AttenuationTPP = DRONE_FIELD(ACrCharacterPlayerBase, AttenuationTPP);

	// The player controller. PlayerControlState has no member in the Game-SDK
	// (it sits in padding), so it is a native field: the loader's row for the
	// running build, or none and the plugin is refused.
	inline auto CrChar                   = DRONE_FIELD(ACrPlayerControllerBase, CrChar);
	inline auto CurrentInteractableActor = DRONE_FIELD(ACrPlayerControllerBase, CurrentInteractableActor);
	inline auto PlayerControlState = PluginField<uint8_t>{ { "ACrPlayerControllerBase::PlayerControlState",
		"ACrPlayerControllerBase", "PlayerControlState", PLUGIN_FIELD_NATIVE, sizeof(uint8_t), 0xE88 } };
	inline auto BuildingMenuPlayer = DRONE_FIELD(UCrUW_BuildingMenu, Player);

	// A scene component's world transform. ComponentToWorld is an engine member
	// the generated SDK does not name, and an FTransform keeps its translation
	// in a SIMD register, so the translation is a native row of its own at the
	// FVector size this plugin reads. Both are rows of the loader's map for the
	// running build; a build with neither turns off the feature using them.
	// ComponentToWorld also carries this plugin's own rows below, because not
	// every loader this plugin runs on holds one for the newest game build.
	inline auto ComponentToWorld = PluginField<SDK::FTransform>{ { "USceneComponent::ComponentToWorld",
		"USceneComponent", "ComponentToWorld", PLUGIN_FIELD_NATIVE, sizeof(SDK::FTransform), 0x1F0 } };
	inline auto TransformTranslation = PluginField<SDK::FVector>{ { "FTransform::Translation",
		"FTransform", "Translation", PLUGIN_FIELD_NATIVE, sizeof(SDK::FVector), 0x20 } };

	// ComponentToWorld on each game build BetterDrone supports (0x60 bytes at
	// +0x1F0 on both, measured from the PDBs). The loader's own row for the
	// running build wins when it has one.
	inline const PluginFieldCandidate kComponentToWorldRows[] = {
		{ 127004, 0x1F0, static_cast<uint32_t>(sizeof(SDK::FTransform)), 0, nullptr },
		{ 128786, 0x1F0, static_cast<uint32_t>(sizeof(SDK::FTransform)), 0, nullptr },
	};

	// The map area's pan and zoom matrix. It is native (the Game-SDK has no
	// member for it), so it is the loader's row for the running build or the
	// map-follow feature is off.
	inline auto MapAreaMatrix = PluginField<MapAreaMatrixData>{ { "UCrUW_MapMenuMapArea::MapAreaMatrix",
		"UCrUW_MapMenuMapArea", "MapAreaMatrix", PLUGIN_FIELD_NATIVE, sizeof(MapAreaMatrixData), 0x500 } };

	// The hotbar pages of the local player's UCrHotbarLocalPlayerSubsystem, which
	// only game builds with hotbar pages have. Neither member is in the
	// Game-SDK, so both are native: the loader's row for the running build, or
	// the page switch is off. The current page is a byte at +0x48, the pages an
	// array at +0x38 whose Num is at +0x40.
	inline auto CurrentHotbarIndex = PluginField<uint8_t>{ { "UCrHotbarLocalPlayerSubsystem::CurrentHotbarIndex",
		"UCrHotbarLocalPlayerSubsystem", "CurrentHotbarIndex", PLUGIN_FIELD_NATIVE, sizeof(uint8_t), 0x48 } };
	inline auto Hotbars = PluginField<HotbarArrayData>{ { "UCrHotbarLocalPlayerSubsystem::Hotbars",
		"UCrHotbarLocalPlayerSubsystem", "Hotbars", PLUGIN_FIELD_NATIVE, sizeof(HotbarArrayData), 0x38 } };

	// Drone settings and the map's fog of war
	inline auto SpeedPerSec   = DRONE_FIELD(UAuActorPlacementDeveloperSettings, BuildingDroneSpeedPerSec);
	inline auto MaxRadius     = DRONE_FIELD(UAuActorPlacementDeveloperSettings, BuildingDroneMaxRadius);
	inline auto WarningRadius = DRONE_FIELD(UAuActorPlacementDeveloperSettings, BuildingDroneWarningRadius);
	inline auto MaxHeight     = DRONE_FIELD(UAuActorPlacementDeveloperSettings, BuildingDroneMaxHeight);
	inline auto WarningHeight = DRONE_FIELD(UAuActorPlacementDeveloperSettings, BuildingDroneWarningHeight);
	inline auto SphereRadius  = DRONE_FIELD(UAuActorPlacementDeveloperSettings, BuildingDroneSphereRadius);
	inline auto FogUncoverRadius   = DRONE_FIELD(UCrMapMenuDevSettings, FogOfWarPlayerUncoverRadius);
	inline auto MinFootstepDistance = DRONE_FIELD(UCrMapMenuDevSettings, MinFootstepDistance);
	inline auto FogRadiusMultiplier = DRONE_FIELD(UCrGemAttributeSet, FogOfWarRadiusMultiplier);

	// Safe interiors and input
	inline auto IsSafeInterior = DRONE_FIELD(UInteriorOverlapComponent, bIsSafeInterior);
	inline auto NativeInputActions = DRONE_FIELD(UCrInputConfig, NativeInputActionsWithHandlers);
	inline auto NativeInputAction  = DRONE_FIELD(FCrNativeInputAction, InputAction);
	inline auto NativeInputHandler = DRONE_FIELD(FCrNativeInputAction, NativeInputHandler);

	// The mining laser
	inline auto HarvesterMiningDamage = DRONE_FIELD(UCrGemAttributeSet, HarvesterMiningDamageMultiplier);
	inline auto FireRateMod           = DRONE_FIELD(UCrWeaponAttributeSet, FireRateModMultiplier);
	inline auto LastEquippedUnarmed   = DRONE_FIELD(UCrWeaponComponent, LastEquippedUnarmed);
	inline auto EquippedWeapon        = DRONE_FIELD(UAuWeaponComponent, EquippedWeapon);
	inline auto EquippedItemData      = DRONE_FIELD(FAuEquippedWeapon, ItemDataLoaded);
	inline auto MiningTypeDamage      = DRONE_FIELD(UCrWeaponItemDataBase, MiningTypeDamage);
	inline auto RoundsPerMinute       = DRONE_FIELD(UAuWeaponItemDataBase, RoundsPerMinute);
	inline auto BaseRange             = DRONE_FIELD(UAuWeaponItemDataBase, BaseRange);
	inline auto MaxStack              = DRONE_FIELD(UAuItemDataBase, MaxStack);
	inline auto CurrentlyMinedOre     = DRONE_FIELD(UCrMiningComponent, CurrentlyMinedOre);
	inline auto MinedOreActor         = DRONE_FIELD(FCrOreMiningState, OreActor);
	inline auto MinedVeinMaterial     = DRONE_FIELD(FCrOreMiningState, InfiniteOrePhysicalMaterial);
	inline auto MinedVeinData         = DRONE_FIELD(FCrOreMiningState, InfiniteOreData);
	inline auto VeinResource          = DRONE_FIELD(FCrInfiniteOreData, Resource);
	inline auto VeinGrantCount        = DRONE_FIELD(FCrInfiniteOreData, GrantingMomentResourceCount);
	inline auto OreData               = DRONE_FIELD(ACrOreActor, OreData);
	inline auto OreResource           = DRONE_FIELD(ACrOreActor, Resource);
	inline auto OreInitMaxCount       = DRONE_FIELD(ACrOreActor, InitMaxCount);
	inline auto OreWeakSpots          = DRONE_FIELD(ACrOreActor, WeakSpotsDataContainer);
	inline auto OreIsDepleted         = DRONE_FIELD(FCrOreRuntimeData, bIsDepleted);
	inline auto OreCurrentCount       = DRONE_FIELD(FCrOreRuntimeData, CurrentResourceCount);
	inline auto OreMaxCount           = DRONE_FIELD(FCrOreRuntimeData, MaxResourceCount);
	inline auto WeakSpotItems         = DRONE_FIELD(FCrWeakSpotsContainer, Items);
	inline auto WeakSpotHealth        = DRONE_FIELD(FCrWeakSpotRuntimeData, CurrentMiningHealth);
	inline auto WeakSpotCount         = DRONE_FIELD(FCrWeakSpotRuntimeData, ResourceCount);
	inline auto WeakSpotResource      = DRONE_FIELD(FCrWeakSpotRuntimeData, Resource);
	inline auto InventorySlots        = DRONE_FIELD(UCrInventoryComponent, Slots);
	inline auto SlotItemId            = DRONE_FIELD(FCrInventorySlot, ItemId);
	inline auto ItemIdHandle          = DRONE_FIELD(FAuItemId, Handle);
	inline auto OreMappingData        = DRONE_FIELD(UCrOreSubsystem, InfiniteOrePhysMatMappingData);
	inline auto PhysMatOreDataMap     = DRONE_FIELD(UCrInfiniteOrePhysMatData, PhysMatOreDataMap);

	// The map marker
	inline auto MapDataHelper      = DRONE_FIELD(ACrGameStateBase, MapMenuDataReplicationHelper);
	inline auto BuildingsMarkers   = DRONE_FIELD(ACrMapMenuDataReplicationHelper, BuildingsMarkerDataContainer);
	inline auto BuildingsMarkerData = DRONE_FIELD(FBuildingsMarkerDataContainer, BuildingsMarkerData);
	inline auto MarkerBuildingHandle = DRONE_FIELD(FBuildingMarkerDataFastArrayItem, BuildingHandle);
	inline auto MarkerLocation     = DRONE_FIELD(FBuildingMarkerDataFastArrayItem, Location);
	inline auto MapMenuMapArea     = DRONE_FIELD(UCrUW_MapMenu, MapMenuMapArea);
	inline auto MapMassMarkers     = DRONE_FIELD(UCrUW_MapMenuMapArea, MapMenuMassMarkers);
	inline auto MapTerrain         = DRONE_FIELD(UCrUW_MapMenuMapArea, MapMenuTerrain);
	inline auto MapCanvasPanel     = DRONE_FIELD(UCrUW_MapMenuMapArea, CanvasPanelMapArea);
	inline auto MarkerWidget       = DRONE_FIELD(FEntityMarkerUIRepresentation, Marker);
	inline auto MapPivotPoint      = DRONE_FIELD(UCrMapMenuDevSettings, MapAreaPivotPoint);
	inline auto MarkerColorData    = DRONE_FIELD(UCrMapMenuDevSettings, MarkerStatusesColorData);
	inline auto MarkerPlayerColour = DRONE_FIELD(UCrMapMenuMarkerDefaultColorData, MarkerPlayer);

	// The drone HUD
	inline auto HudPlayerCharacter = DRONE_FIELD(UCrUW_BuildingDroneHud, PlayerCharacter);
	inline auto HudDistanceT = DRONE_FIELD(UCrUW_BuildingDroneHud, DistanceValueT);
	inline auto HudDistanceH = DRONE_FIELD(UCrUW_BuildingDroneHud, DistanceValueH);
	inline auto HudDistanceD = DRONE_FIELD(UCrUW_BuildingDroneHud, DistanceValueD);
	inline auto HudDistanceU = DRONE_FIELD(UCrUW_BuildingDroneHud, DistanceValueU);
	inline auto HudDistanceM = DRONE_FIELD(UCrUW_BuildingDroneHud, DistanceValueM);
	inline auto HudHeightT   = DRONE_FIELD(UCrUW_BuildingDroneHud, HeightValueT);
	inline auto HudHeightH   = DRONE_FIELD(UCrUW_BuildingDroneHud, HeightValueH);
	inline auto HudHeightD   = DRONE_FIELD(UCrUW_BuildingDroneHud, HeightValueD);
	inline auto HudHeightU   = DRONE_FIELD(UCrUW_BuildingDroneHud, HeightValueU);
	inline auto HudHeightM   = DRONE_FIELD(UCrUW_BuildingDroneHud, HeightValueM);

	// Structs stepped through as array elements: the size the running game
	// reflects, read with PluginMap::ElementAt, so a build that resizes one does
	// not turn the plugin off.
	inline auto NativeInputActionSize = SR_SIZE(FCrNativeInputAction);
	inline auto InventorySlotSize     = SR_SIZE(FCrInventorySlot);
	inline auto WeakSpotSize          = SR_SIZE(FCrWeakSpotRuntimeData);
	inline auto MarkerItemSize        = SR_SIZE(FBuildingMarkerDataFastArrayItem);

	// Structs held as map values: the pair layout is compiled into the code, so
	// the size is checked against it.
	inline auto MarkerRepresentationSize = SR_SIZE_CHECK(FEntityMarkerUIRepresentation);
	inline auto VeinDataSize          = SR_SIZE_CHECK(FCrInfiniteOreData);

	// Blueprint (_C) members have no offset in the Game-SDK: found by name on
	// first use. A member that is not there leaves its feature off.
	inline PluginLazyField<SDK::UStaticMeshComponent*> DroneMesh{ "SM_BuidingDrone" };
	inline PluginLazyField<SDK::FGameplayTag>          OverheatTag{ "OverheatTag" };
	inline PluginLazyField<double>                     SingleStackDuration{ "SingleStackDuration" };
	inline PluginLazyField<float>                      DefaultStartDelay{ "DefaultStartDelayDuration" };
	inline PluginLazyField<float>                      OverheatStartDelay{ "OverheatStartDelayDuration" };

	// The forced cooldown ability's defaults (the vent on the Reload key). Its
	// SingleStackDuration is a float, unlike the passive cooling's above, and
	// each field caches the class it was last read from, so these are rows of
	// their own.
	inline PluginLazyField<float>                      VentSingleStackDuration{ "SingleStackDuration" };
	inline PluginLazyField<double>                     VentPenaltyDuration{ "PenaltyDuration" };

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		const bool toWorld = PluginMap::ResolveField(scanner, self, ComponentToWorld, kComponentToWorldRows,
			static_cast<uint32_t>(sizeof(kComponentToWorldRows) / sizeof(kComponentToWorldRows[0])));
		return PluginMap::ResolveFields(scanner, self,
			BuildingDrone, Status, DroneCamera, DroneCameraColl, IsDead, GemAttributes, MiningComponent,
			WeaponSystem, WeaponAttributes, AbilitySystem,
			DroneOwnCamera, SphereComponent, PlacementDeveloperSettings, IdleSound, MovementSound, RotationSound, StateAudioComponents,
			AttenuationFPP, AttenuationTPP,
			CrChar, CurrentInteractableActor, PlayerControlState, BuildingMenuPlayer, TransformTranslation,
			SpeedPerSec, MaxRadius, WarningRadius, MaxHeight, WarningHeight, SphereRadius,
			FogUncoverRadius, MinFootstepDistance, FogRadiusMultiplier,
			IsSafeInterior, NativeInputActions, NativeInputAction, NativeInputHandler,
			HarvesterMiningDamage, FireRateMod, LastEquippedUnarmed, EquippedWeapon, EquippedItemData,
			MiningTypeDamage, RoundsPerMinute, BaseRange, MaxStack, CurrentlyMinedOre, MinedOreActor,
			MinedVeinMaterial, MinedVeinData, VeinResource, VeinGrantCount, OreData, OreResource, OreInitMaxCount,
			OreWeakSpots, OreIsDepleted, OreCurrentCount, OreMaxCount, WeakSpotItems, WeakSpotHealth, WeakSpotCount,
			WeakSpotResource, InventorySlots, SlotItemId, ItemIdHandle, OreMappingData, PhysMatOreDataMap,
			MapDataHelper, BuildingsMarkers, BuildingsMarkerData, MarkerBuildingHandle, MarkerLocation,
			MapMenuMapArea, MapMassMarkers, MapTerrain, MapCanvasPanel, MarkerWidget, MapPivotPoint, MapAreaMatrix,
			CurrentHotbarIndex, Hotbars,
			MarkerColorData, MarkerPlayerColour,
			HudPlayerCharacter, HudDistanceT, HudDistanceH, HudDistanceD, HudDistanceU, HudDistanceM,
			HudHeightT, HudHeightH, HudHeightD, HudHeightU, HudHeightM,
			NativeInputActionSize, InventorySlotSize, WeakSpotSize, MarkerItemSize, MarkerRepresentationSize,
			VeinDataSize) && toWorld;
	}

	// The map keys of the game functions BetterDrone resolves, so the resolves
	// and the feature declarations below spell each one once.
	namespace Keys
	{
		inline constexpr const char* MineActor          = "UCrMiningComponent::MineResourceRequest(actor)";
		inline constexpr const char* MineIsm            = "UCrMiningComponent::MineResourceRequest(ISM)";
		inline constexpr const char* TargetsChanged     = "ACrPlayerControllerBase::OnInteractableTargetsChanged";
		inline constexpr const char* InteractStarted    = "ACrPlayerControllerBase::NativeOnInputInteractStarted";
		inline constexpr const char* InteractCompleted  = "ACrPlayerControllerBase::NativeOnInputInteractCompleted";
		inline constexpr const char* Interact           = "ACrPlayerControllerBase::NativeOnInputInteract";
		inline constexpr const char* ContextStarted     = "ACrPlayerControllerBase::NativeOnInputInteractContextStarted";
		inline constexpr const char* Context            = "ACrPlayerControllerBase::NativeOnInputInteractContext";
		inline constexpr const char* ContextCompleted   = "ACrPlayerControllerBase::NativeOnInputInteractContextCompleted";
		inline constexpr const char* MapMenu            = "ACrPlayerControllerBase::NativeOnInputMapMenu";
		inline constexpr const char* UncoverFog         = "UCrMapManuSubsystem::UncoverFogOfWar";
		inline constexpr const char* CancelTool         = "ACrCharacterPlayerBase::NativeOnWeaponCancelTool";
		inline constexpr const char* CloseInGameMenu    = "ACrPlayerControllerBase::NativeOnInputCloseInGameMenu";
		inline constexpr const char* HandleDeactivate   = "UCrUW_ActivatableWidget::HandleDeactivate";
		inline constexpr const char* CanDroneBeActive   = "ACrCharacterPlayerBase::CanBuildingDroneBeActive";
		inline constexpr const char* CheckIfInInterior  = "ACrCharacterPlayerBase::CheckIfInInterior";
		inline constexpr const char* MapCenterOnLocation = "UCrUW_MapMenuMapArea::CenterOnLocation";
	}

	// The parts of BetterDrone that can be off while the rest runs. Each lists
	// every key it uses, a key two parts use in both. A key stays core only
	// when the plugin can do nothing without it: the drone, the pilot's status
	// and dead flag, and the player controller's character. A miss on those
	// still refuses the plugin. Declared in OnPluginLoadHooks before any key
	// is resolved. Every entry point of a part checks On(), so
	// an off part runs nothing and reads none of its keys or config entries.
	namespace Features
	{
		inline PluginMap::Feature MiningLaser{ "mining-laser", "Mining laser",
			PluginMap::KeysOf(Keys::MineActor, Keys::MineIsm,
				DroneOwnCamera, GemAttributes, MiningComponent, WeaponSystem, WeaponAttributes, AbilitySystem,
				HarvesterMiningDamage, FireRateMod, LastEquippedUnarmed, EquippedWeapon, EquippedItemData,
				MiningTypeDamage, RoundsPerMinute, BaseRange, MaxStack, CurrentlyMinedOre, MinedOreActor,
				MinedVeinMaterial, MinedVeinData, VeinResource, VeinGrantCount, OreData, OreResource,
				OreInitMaxCount, OreWeakSpots, OreIsDepleted, OreCurrentCount, OreMaxCount, WeakSpotItems,
				WeakSpotHealth, WeakSpotCount, WeakSpotResource, InventorySlots, SlotItemId, ItemIdHandle,
				OreMappingData, PhysMatOreDataMap, InventorySlotSize, WeakSpotSize, VeinDataSize),
			{ "Controls.Mining Laser Key" } };

		inline PluginMap::Feature MapReveal{ "map-reveal", "Drone uncovers the map",
			PluginMap::KeysOf(Keys::UncoverFog, GemAttributes, FogUncoverRadius, MinFootstepDistance, FogRadiusMultiplier),
			{ "Interaction.Drone Reveals Map" } };

		inline PluginMap::Feature MapMarker{ "map-marker", "Drone marker on the map",
			PluginMap::KeysOf(MapDataHelper, BuildingsMarkers, BuildingsMarkerData, MarkerBuildingHandle, MarkerLocation,
				MapMenuMapArea, MapMassMarkers, MapTerrain, MapCanvasPanel, MarkerWidget, MapPivotPoint,
				MarkerColorData, MarkerPlayerColour, MarkerItemSize, MarkerRepresentationSize),
			{} };

		inline PluginMap::Feature MapFollow{ "map-follow", "Map follows the drone",
			PluginMap::KeysOf(Keys::MapCenterOnLocation, MapAreaMatrix, MapMenuMapArea, MapCanvasPanel),
			{ "Interaction.Map Centre Key" } };

		inline PluginMap::Feature Limits{ "drone-limits", "Drone speed, boost, size and limits",
			PluginMap::KeysOf(PlacementDeveloperSettings, SpeedPerSec, MaxRadius, WarningRadius, MaxHeight,
				WarningHeight, SphereRadius, SphereComponent),
			{ "Controls.Boost Key", "Controls.Slow Key" } };

		inline PluginMap::Feature SprintKey{ "sprint-key", "Following the game's Sprint key",
			PluginMap::KeysOf(NativeInputActions, NativeInputAction, NativeInputHandler, NativeInputActionSize),
			{} };

		inline PluginMap::Feature AlwaysAllow{ "always-allow", "Drone allowed during wave events",
			PluginMap::KeysOf(Keys::CanDroneBeActive),
			{ "Drone.Always Allow Drone" } };

		inline PluginMap::Feature WaveShelter{ "wave-shelter", "Drone does not count toward the wave shelter",
			PluginMap::KeysOf(Keys::CheckIfInInterior, DroneCameraColl, IsSafeInterior),
			{} };

		inline PluginMap::Feature Interaction{ "drone-interact", "Open containers and doors from the drone",
			PluginMap::KeysOf(Keys::TargetsChanged, Keys::InteractStarted, Keys::InteractCompleted, Keys::Interact,
				CurrentInteractableActor, PlayerControlState, DroneCamera, ComponentToWorld, TransformTranslation),
			{ "Interaction.Interact In Drone Mode", "Interaction.Interact Key" } };

		// Holding the game's context key takes everything from the target, as
		// on foot. A part of its own: a game build without these three keys
		// loses only this, and the interact key keeps working.
		inline PluginMap::Feature HoldTakeAll{ "hold-take-all", "Hold the interact key to take all",
			PluginMap::KeysOf(Keys::ContextStarted, Keys::Context, Keys::ContextCompleted,
				NativeInputActions, NativeInputAction, NativeInputHandler, NativeInputActionSize),
			{} };

		// Ctrl and the mouse wheel switch the hotbar page in the drone, through
		// the game's own SetActiveHotbar. A part of its own: a game build without
		// hotbar pages, or without the loader's rows for them, loses only this.
		inline PluginMap::Feature HotbarPage{ "hotbar-page", "Ctrl + mouse wheel switches the hotbar page in the drone",
			PluginMap::KeysOf(CurrentHotbarIndex, Hotbars),
			{} };

		inline PluginMap::Feature MapOpen{ "drone-map", "Open the map from the drone",
			PluginMap::KeysOf(Keys::MapMenu),
			{ "Interaction.Map In Drone Mode", "Interaction.Map Key" } };

		inline PluginMap::Feature BuildMenu{ "build-menu", "Closing the building menu keeps the drone",
			PluginMap::KeysOf(Keys::CancelTool, Keys::HandleDeactivate, BuildingMenuPlayer),
			{} };

		// Esc opens the pause menu in the drone. A part of its own: a game build
		// without the key loses only this option, and Esc keeps ending the drone. The
		// cancel-tool path of it also needs BuildMenu.
		inline PluginMap::Feature EscPause{ "esc-pause", "Esc opens the pause menu in the drone",
			PluginMap::KeysOf(Keys::CloseInGameMenu),
			{ "Interaction.Esc Opens Pause Menu In Drone Mode" } };

		inline PluginMap::Feature Speedometer{ "speedometer", "Speedometer on the drone HUD",
			PluginMap::KeysOf(HudPlayerCharacter, HudDistanceT, HudDistanceH, HudDistanceD, HudDistanceU, HudDistanceM),
			{} };

		// Apart from the speedometer: without these keys the height stays in the game's metres.
		inline PluginMap::Feature SpeedometerHeight{ "speedometer-height", "Height in feet on the drone HUD",
			PluginMap::KeysOf(HudHeightT, HudHeightH, HudHeightD, HudHeightU, HudHeightM),
			{} };

		inline PluginMap::Feature Audio{ "drone-audio", "Drone sound volumes",
			PluginMap::KeysOf(IdleSound, MovementSound, RotationSound, StateAudioComponents),
			{} };

		inline PluginMap::Feature Announcer{ "announcer-audio", "System voice heard while the drone is out",
			PluginMap::KeysOf(AttenuationFPP, AttenuationTPP),
			{} };

		// Declares them all. Call first in OnPluginLoadHooks.
		inline bool Declare(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			return PluginMap::DeclareFeatures(scanner, self, MiningLaser, MapReveal, MapMarker, MapFollow, Limits, SprintKey,
				Announcer,
				AlwaysAllow, WaveShelter, Interaction, HoldTakeAll, HotbarPage, MapOpen, BuildMenu, EscPause, Speedometer, SpeedometerHeight, Audio);
		}
	}

	// A game function by its map key. The pattern is mapping input only; the
	// loader resolves from its row for the running build. Every address BetterDrone resolves is a function entry, so each
	// request declares PLUGIN_SCAN_FUNCTION_START: some get a detour written
	// over them and the rest are called directly. The loader then checks the
	// match against the executable's exception directory instead of trusting
	// that the bytes lined up -- a pattern that drifted into the middle of some
	// other function is refused rather than used. Optional: a miss leaves the
	// rest of BetterDrone working, and the loader still lists it for the user.
	inline uintptr_t ResolveFunction(IPluginSelf* self, IPluginHookScanner* scanner,
	                                 const char* key, const char* pattern)
	{
		return PluginMap::Function(scanner, self, key, pattern, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
	}

	// The member's value, or a fallback when the object is null.
	using PluginMap::ValueOr;

	// The loader's property reader for Blueprint members, or null.
	inline const IPluginObjectProperties* ObjectProperties()
	{
		IPluginSelf* self = GetSelf();
		return self && self->hooks ? self->hooks->ObjectProperties : nullptr;
	}

	// True when the character is flying the building drone.
	inline bool InDrone(const SDK::ACrCharacterPlayerBase* character)
	{
		return ValueOr(Status, character, SDK::EPlayerCharacterStatus::None) == SDK::EPlayerCharacterStatus::BuildingDrone;
	}

	inline bool Dead(const SDK::ACrCharacterPlayerBase* character)
	{
		return ValueOr(IsDead, character, false);
	}

	// The drone HUD's speed text (the "U" digit), or null.
	inline SDK::UTextBlock* DistanceU(const SDK::UCrUW_BuildingDroneHud* hud)
	{
		return ValueOr(HudDistanceU, hud);
	}

	// The character the HUD belongs to, or null.
	inline SDK::ACrCharacterPlayerBase* HudPilot(const SDK::UCrUW_BuildingDroneHud* hud)
	{
		return ValueOr(HudPlayerCharacter, hud);
	}
}
