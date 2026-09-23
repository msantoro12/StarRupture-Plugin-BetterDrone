#pragma once

// Lets the player open a building's UI while flying the building drone, the
// same way walking up to it and pressing the interact key does on foot.
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
// InitDroneInteract hooks around the first and supplies the second from the
// modloader's own keybind dispatch.

struct IPluginSelf;
struct IPluginHookScanner;

// Resolve every AOB the interact path needs. Callable only from
// OnPluginLoadHooks — the loader refuses scans made anywhere else.
void ResolveDroneInteract(IPluginSelf* self, IPluginHookScanner* scanner);

bool InitDroneInteract();

void ShutdownDroneInteract();

// True if the local player's character is currently flying the building
// drone. Game-thread only -- touches UWorld/UObject state. Shared with
// drone_settings.cpp so boost gating and the interact hooks agree on what
// "in the drone" means.
bool IsLocalPlayerInDrone();
