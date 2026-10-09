# BetterDrone — StarRupture Client Plugin

Extends the building drone limits in [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/) beyond their default values, configurable via an INI file without touching game files.

**Target:** Game client only

---

## What It Does

Patches the game's drone developer settings CDO at runtime to apply your configured values each session:

| Setting | Description |
|---|---|
| Speed | Movement speed of the building drone (cm/s) |
| Max Radius | Maximum horizontal operating range (cm) |
| Max Height | Maximum vertical operating range (cm) |
| Max Rail Length | Maximum rail length (cm) |

The warning thresholds (the visual indicators before hitting the limit) are automatically adjusted to 95% of the configured max values.

### Interacting from the drone

Normally you have to walk up to a building on foot to press **E** and open its UI — the game clears the interaction target for as long as the drone is out, and measures interaction range from your body rather than the drone. With this plugin the drone can do it too: fly up to a building, look at it, and press the interact key.

### Opening the map from the drone

The game's map key does nothing while the drone is out. With this plugin, pressing the map key while flying opens the map.

### Mining from the drone

Hold the mining laser key (default **G**, `Mining Laser Key` under `[Controls]`) while flying to mine the ore under the crosshair: meteor ores, their chunks and infinite ore veins, within the mining tool's range and with its damage and rate. The yield goes to your inventory. Mining stops when you let go, when the target changes or leaves range, and when the drone is recalled; it also stops, until you press the key again, when your inventory can't take the next grant or the drill is overheated. It never builds or deconstructs anything. Host and single player only.

---

## Configuration

Config is stored in `Plugins\config\BetterDrone.ini` and is generated on first launch with the game's own default values pre-filled. You can also edit settings in-game via the ModLoader config editor (default key: **F2**).

| Key | Description |
|---|---|
| `SpeedPerSec` | Movement speed in cm/s (max `4000`) |
| `MaxRadius` | Horizontal range in cm (max `1000000`) |
| `MaxHeight` | Vertical range in cm (max `100000`) |

| Key (`[Interaction]`) | Description |
|---|---|
| `Interact In Drone Mode` | Let the drone open building UIs (default `true`) |
| `Interact Key` | Key used to interact while the drone is out (default `E`) — set this to match your in-game interact key |
| `Map In Drone Mode` | Let the drone open the map (default `true`) |
| `Map Key` | Key used to open the map while the drone is out (default `M`) — set this to match your in-game map key |

Changes are applied live — no restart required, except `Interact Key` and `Map Key`, which are picked up on the next launch.

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - `BetterDrone-Client-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder — it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\BetterDrone.ini` and set your desired values.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Values not changing | Check `modloader.log` in `Binaries\Win64\` for errors. |
| Plugin not loading | Confirm ModLoader is installed and `BetterDrone.dll` is in `Plugins\`. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

Clone the repo, open `BetterDrone.sln`, and build the `Client Release|x64` configuration. The output DLL will be placed in `build\Client Release\Plugins\`.

---

## Disclaimer

Use at your own risk. The authors are not responsible for any damage caused by using this software.
