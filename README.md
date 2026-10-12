# BetterDrone — StarRupture Client Plugin

Extends the building drone limits in [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/) beyond their default values, and adds drone controls and tools: speed presets, a mining laser, a map marker and more <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>. Everything is configurable without touching game files.

**Target:** Game client only

---

## What It Does

Patches the game's drone developer settings CDO at runtime to apply your configured values each session:

| Setting | Description |
|---|---|
| Speed | Movement speed of the building drone (cm/s) |
| Max Radius | Maximum horizontal operating range (cm) |
| Max Height | Maximum vertical operating range (cm) |

The warning thresholds (the visual indicators before hitting the limit) are automatically adjusted to 95% of the configured max values.

### The BetterDrone panel <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

The panel holds the drone's speed, boost, acceleration, deceleration, range and audio volumes, with built-in and saved presets for speed and for range (saved presets can be saved, renamed and deleted), and a choice of speed unit (km/h, mph or cm/s). Open it with the toggle key (default **F10**, `Toggle Key` under `[Controls]`) or with the **BETTER DRONE** row in the game's pause menu, just below OPTIONS. **Esc** or **Q** closes it. The row is added only when the ModLoader's game menu is available; the toggle key always works. The panel can only be used in a game session.

Boost speeds the drone up while the boost key is held (default: your game's Sprint key, `Boost Key` under `[Controls]`; multiplier 1x to 10x, default 2x; boosted speed is capped at the same 20000 cm/s maximum as the base speed). Acceleration and deceleration are never instant.

### Speed presets from the keyboard <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

While the drone is out, **-** steps to the next slower speed preset and **=** to the next faster one (`Previous Preset Key` and `Next Preset Key` under `[Controls]`, default `Hyphen` and `Equals`). The presets are the panel's built-in speed presets (Stock, Better Construction, Agile Builder, Long Haul, Ludicrous Speed) and your saved speed presets. They are stepped in order of speed, slowest to fastest, and a saved preset that repeats another's values is skipped. Stepping stops at either end. If the speed sits on no preset because you used the sliders, a step goes to the nearest preset in that direction.

A small toast in the top-right corner of the screen shows the preset's name, its speed and its boost speed, in the panel's speed unit, for about a second and a half. The keys do nothing on foot, while a game menu is open, or while the panel is open.

### Interacting from the drone

Normally you have to walk up to a building on foot to press **E** and open its UI — the game clears the interaction target for as long as the drone is out, and measures interaction range from your body rather than the drone. With this plugin the drone can do it too: fly up to a building, look at it, and press the interact key.

The same key gathers wild plants and other gatherables (fruit, eggs) from the drone. The game ranges a gatherable from your body, so for these the plugin ranges it from the drone instead. Host and single player only; on a client the server still ranges the plant from your body and refuses it. <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

### Opening the map from the drone <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

The game's map key does nothing while the drone is out. With this plugin, pressing the map key while flying opens the map.

### The drone on the map <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

With the map open in drone mode, the drone gets its own marker, drawn over the map in the colour of your player arrow. It is a small copy of the drone's targeting reticle and turns with the drone's heading. The game draws no marker for the drone: its own player arrow stays at your body, parked where you launched the drone. There is no setting for it.

### Revealing the map from the drone <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

Flying the drone does not uncover the map the way walking does, because the game uncovers it around your body. Turn on `Drone Reveals Map` (under `[Interaction]`, default off) and the drone uncovers the map as it flies, at the same radius and spacing as walking, including your upgrades. What it uncovers is saved with your game. Nothing is revealed inside the Forgotten Engine.

### Mining from the drone <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

Hold the mining laser key (default **G**, `Mining Laser Key` under `[Controls]`) while flying to mine the ore under the crosshair: meteor ores, their chunks and infinite ore veins, within the mining tool's range and with its damage and rate. The yield goes to your inventory. Mining stops when you let go, when the target changes or leaves range, and when the drone is recalled; it also stops, until you press the key again, when your inventory can't take the next grant or the drill is overheated. It never builds or deconstructs anything. Host and single player only.

The laser looks and sounds like the mining tool: its beam, its start, loop and finish sounds, and its click when you press the key while overheated. It also heats like the tool, with the tool's own numbers: the same heat build-up, cooling delay and overheat lockout. A ring around the crosshair shows the heat, filling clockwise from 12 o'clock, shifting from your marker colour through orange to red, pulsing from 85% and flashing while the laser is locked out. Your drill's own heat is read, never changed. The laser takes what the drill has: if you carry the unlimited heat tag (the game's own cheat, or BetterCheats' No Handheld Drill Overheat) it never heats, its fire rate follows the drill's fire rate bonuses as last seen with the drill in hand, and BetterCheats' Mining Damage, Range, Hit Rate and Overheat Cooldown rows apply to it too.

A small ore chunk, such as Ignitium, can slip out from under the crosshair as the drone drifts. The laser aims with a little slack, catching a small ore just beside the crosshair, and holds the ore it is mining through a miss shorter than about a third of a second.

### Keeping your character on the floor <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

Buildings and the ground around your parked character are unloaded when you fly a long way out (well past the stock drone range), and on return the character could drop through the missing floor. The plugin remembers where the character stood when the drone went out. With the range raised above stock, if the character starts falling straight down out of that spot, during the flight or after the drone returns, it is held there until the floor is back, then set down walking. Nothing is touched when the floor is fine; walking, jumping or being knocked back ends the watch; and a floor that is really gone (deconstructed, destroyed) is not held for: the hold gives up after ten seconds. There is no setting for it.

### Waves and shelter

`Always Allow Drone` (under `[Drone]`, default off) lets you use the drone in places it is normally blocked, including during wave events.

While the drone is out, the shelter that protects you from a wave is judged at your parked body, not at the drone. The game counts the drone's own collision as yours, so flying out of a shelter could expose a sheltered body to the wave's heat, and parking the drone in one could shield an exposed body. With this plugin the drone's surroundings are ignored, and a shelter built or deconstructed around your body during a flight counts at once. With `Always Allow Drone` off, the drone still ends when it flies into an interior, as in the stock game. On foot nothing changes. There is no setting for it. <a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a>

---

## Configuration

Config is stored in `Plugins\config\BetterDrone.ini` and is generated on first launch. You can also edit the `[Interaction]`, `[Controls]` and `Always Allow Drone` settings in-game via the ModLoader config editor (default key: **F2**). The values you set in the panel (speed, boost, acceleration, deceleration, range, audio volumes and the speed unit) are saved to `Plugins\config\BetterDrone-Panel.ini`, and saved presets to `Plugins\config\BetterDrone-Presets.ini`.

| Key (`[Drone]`, `BetterDrone-Panel.ini`) | Description |
|---|---|
| `SpeedPerSec` | Movement speed in cm/s (default `1000`, max `20000`) |
| `MaxRadius` | Horizontal range in cm (default `5000`, max `1000000`) |
| `MaxHeight` | Vertical range in cm (default `2000`, max `500000`) |

| Key (`[Drone]`) | Description |
|---|---|
| `Always Allow Drone` | Allow the drone in places it's normally blocked, including wave events (default `false`) |

| Key (`[Interaction]`) | Description |
|---|---|
| `Interact In Drone Mode` | Let the drone open building UIs and gather plants (default `true`) |
| `Interact Key` | Key used to interact while the drone is out (default `E`) — set this to match your in-game interact key |
| `Map In Drone Mode` | Let the drone open the map (default `true`) |
| `Map Key` | Key used to open the map while the drone is out (default `M`) — set this to match your in-game map key |
| `Drone Reveals Map` | Flying the drone uncovers the map the way walking does (default `false`) |

| Key (`[Controls]`) | Description |
|---|---|
| `Toggle Key` | Opens and closes the BetterDrone panel (default `F10`) |
| `Boost Key` | Key held to boost drone speed (default `Sprint`, which follows your game's Sprint key) |
| `Previous Preset Key` | Steps to the next slower speed preset while the drone is out (default `Hyphen`) |
| `Next Preset Key` | Steps to the next faster speed preset while the drone is out (default `Equals`) |
| `Mining Laser Key` | Key held to mine the ore under the crosshair from the drone (default `G`) — host and single player only |

Changes made in the panel or the config editor are applied live — no restart required, except `Interact Key` and `Map Key`, which are picked up on the next launch.

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - `BetterDrone-Client-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder — it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, set your desired values in the BetterDrone panel, or edit `Plugins\config\BetterDrone.ini` and `Plugins\config\BetterDrone-Panel.ini` directly (restart the game or press RELOAD to pick up hand edits).

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Values not changing | Check `modloader.log` in `Binaries\Win64\` for errors. |
| Plugin not loading | Confirm ModLoader is installed and `BetterDrone.dll` is in `Plugins\`. |
| Mining laser does nothing | It works in drone mode only, on the host or in single player, and stops while your inventory is full, or the drill or the laser is overheated. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

Clone the repo, open `BetterDrone.sln`, and build the `Client Release|x64` configuration. The output DLL will be placed in `build\Client Release\Plugins\`.

---

## Disclaimer

Use at your own risk. The authors are not responsible for any damage caused by using this software.

---

## Credits

<a href="https://github.com/msantoro12"><img src="https://github.com/msantoro12.png?size=40" width="18" height="18" alt="@msantoro12"></a> marks features added by @msantoro12, built with Claude Code.
