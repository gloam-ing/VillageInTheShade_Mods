# Auto Pet (v0.2.5)

> [中文版](README.md)

## Features

- Pets every farm animal/companion automatically each day: +5 affection
  × multiplier (default +5/day), capped at 2000.
- Resets the daily "already petted" flag automatically on a new day.
- Pure data operations plus the game's native event map; no instruction
  patching, does not affect the original petting logic.
- Addresses are resolved by byte signatures at runtime; if signatures fail
  after a game update, the mod disables itself safely and logs the reason.

## Installation

1. Close the game.
2. Put the `AutoPet_v0.2.5` folder into the `Mods` folder in the game root.
3. Launch the game normally from Steam (Mods are auto-loaded by the
   `steam_api64.dll` bridge).

## Configuration

Config file `autopet.txt` (included by default): `GainMultiplier=1` (1~100,
affection multiplier, applied in real time after saving; default 1× = +5/day).

## Uninstallation

Delete the `Mods\AutoPet_v0.2.5` folder.

---

Author: gloaming. Please credit the source when re-sharing.
