# One-Shot Hunt (HuntOneShot v0.1.2)

> [中文版](README.md)

## Features

- All huntable animals (squirrel, wild boar, weasel, rabbit, deer, fox,
  raccoon dog, bear, green pheasant) can be hunted successfully with
  **a single shot**.
- Does not affect drops or hunting settlement; animals still flee/disappear
  normally.

## v0.1.2 Fixes

- **Precise prey matching**: only real prey are handled (+8 is a multiple of
  10 in 11–200, and +0x20 is the animal sub-vtable 0xE49D18).
- **Fixed stamina not recovering after sleep**: no longer touches non-prey /
  status objects sharing the vtable (giant rabbit, whelk, treasure box, etc.
  have +8 = 0/1 and were previously clamped to 10, breaking stamina
  settlement).
- The three "Lords" — Bear Lord, Deer Lord, Wild Boar Lord (HP > 200) —
  keep their challenge and are not one-shot.

## Installation

1. Close the game.
2. Put the `HuntOneShot_v0.1.2` folder into the `Mods` folder in the game root.
3. Launch the game via `VillageModLoader.exe` (set it as a Steam launch option).

> Only compatible with the same game version (village.exe 18,108,416 bytes).
> Mods may break after game updates.

## Uninstallation

Delete the `Mods\HuntOneShot_v0.1.2` folder.

Author: gloaming. Please credit the source when re-sharing.
