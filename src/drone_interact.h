#pragma once

// Lets the player open a building's UI while flying the building drone, the
// same way walking up to it and pressing the interact key does on foot, and
// open the map.
//
// Three separate things stop this in the stock game:
//
//   * ACrPlayerControllerBase::OnInteractableTargetsChanged returns early --
//     after clearing CurrentInteractableActor -- whenever
//     ACrCharacterPlayerBase::IsBuildingDroneActive is true, so nothing is ever
//     targeted while the drone is out. The same function also measures the
//     interaction range from the character's root component, which stays parked
//     wherever the player left their body.
//
//   * The same function clears CurrentInteractableActor again, earlier, when
//     PlayerControlState is DeconstructMode. The drone is summoned from the
//     building tool and UCrBuildingComponent::CanActivateDrone returns true
//     outright for a controller already in DeconstructMode, so that is simply
//     the state the drone is flown in and the gate stays closed the whole time
//     delete mode is selected. Suppressing this one is a deliberate override,
//     not a fix: the gate is on the controller, so a player on foot in delete
//     mode is blocked identically.
//
//   * The interact key lives in the BasePlayerAlive input config, which
//     ActivateBuildingDrone unbinds in favour of the Drone config, so the key
//     may never reach ACrPlayerControllerBase::NativeOnInputInteract at all.
//
// InitDroneInteract hooks around the first two and supplies the third from
// the modloader's own keybind dispatch.
//
// The map key is lost the same way as the third point: its handler,
// UCrInputNativeMapMenu, belongs to the unbound on-foot config, and the
// controller function behind it has no drone check of its own.
// InitDroneMap supplies that key from the keybind dispatch as well.
//
// Flying the drone does not uncover the map either. UCrMapManuSubsystem
// uncovers it around the player's body, sampled from the character's
// movement component, and the body stays where it was left while the drone
// is out. InitDroneFog feeds the drone's position to the same function, at
// the same spacing, when "Drone Reveals Map" is on.

struct IPluginSelf;
struct IPluginHookScanner;

namespace SDK { class ACrCharacterPlayerBase; class ACrPlayerControllerBase; class UCrUW_MapMenu; }

// Resolve every AOB the interact path needs. Callable only from
// OnPluginLoadHooks — the loader refuses scans made anywhere else.
void ResolveDroneInteract(IPluginSelf* self, IPluginHookScanner* scanner);

bool InitDroneInteract();

void ShutdownDroneInteract();

// Same contract as ResolveDroneInteract, for the map path.
void ResolveDroneMap(IPluginSelf* self, IPluginHookScanner* scanner);

bool InitDroneMap();

void ShutdownDroneMap();

// Same contract as ResolveDroneInteract, for the map-reveal path.
void ResolveDroneFog(IPluginSelf* self, IPluginHookScanner* scanner);

bool InitDroneFog();

void ShutdownDroneFog();

// "Drone Reveals Map" as the loader just reported it. The tick reads a
// cached copy, never the INI.
void SetDroneFogEnabled(bool enabled);

// Game thread only. The local player's character, or nullptr outside a game.
// It stays the possessed pawn while the drone is out.
SDK::ACrCharacterPlayerBase* LocalPlayerCharacter();

// Game thread only.
bool IsLocalPlayerInDrone();

// Game thread only. The local player's controller, or nullptr outside a game.
// Valid for the current tick only.
SDK::ACrPlayerControllerBase* LocalPlayerController();

// Game thread only. The map, if it is the active widget on the local player's
// menu layer, whether it was opened from drone mode or on foot. Valid for the
// current tick only.
SDK::UCrUW_MapMenu* ActiveLocalMap();
