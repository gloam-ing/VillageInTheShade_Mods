# Auto Pet (AutoPet v0.2.0)

> [中文版](README.md)

## Features

- Automatically pets **all** livestock and pets every day (chickens, cows,
  sheep, ducks, dogs, horses, etc.).
- Uses the game's native affinity logic, identical to manual petting.
- Each animal is petted once per day; resets automatically on a new day.
- Configurable affinity multiplier: edit `GainMultiplier` in `autopet.txt`
  (default 1). Changes take effect immediately without restarting.
- Does not modify any game files or code bytes; it only reads/writes data and
  calls native functions.

## Installation

1. Close the game.
2. Put the `AutoPet` folder into the `Mods` folder in the game root.
3. Launch the game via `VillageModLoader.exe` (set it as a Steam launch option).

> Only compatible with the same game version (village.exe 18,108,416 bytes).
> Mods may break after game updates.

## Uninstallation

Delete the `Mods\AutoPet` folder.

## Log

`Mods\AutoPet\autopet.log` records the unique ID and affinity change for each
auto-pet.

## Configuration

`autopet.txt` (same directory as the DLL):

```ini
; AutoPet configuration (reloads automatically when changed)
; GainMultiplier: affinity gain multiplier for auto-petting, default 1
GainMultiplier=1
```

Author: gloaming. Please credit the source when re-sharing.
