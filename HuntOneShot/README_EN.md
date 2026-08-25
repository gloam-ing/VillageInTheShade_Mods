# One-Shot Hunt (HuntOneShot v0.1.5)

> [中文版](README.md)

## Features

- Huntable animals (squirrel, raccoon dog, weasel, rabbit, fox, green
  pheasant, wild boar, deer, bear) can be hunted successfully with
  **a single shot**.
- Does not affect drops or hunting settlement; animals still flee/disappear
  normally.

## Matching Method & Limitations (v0.1.5)

- Matching is done with an **initial hit-count whitelist** (based on the
  initial value at instance +0x10 high 32 bits, not the current +8):
  `{30, 40, 50, 150, 200, 500}` (squirrel 30 / raccoon dog·weasel·rabbit 40 /
  fox·green pheasant 50 / wild boar 150 / deer 200 / bear 500).
- **Why not match by animal name**: after an exhaustive search of the game
  memory (instance body, sub-objects, container, pool nodes, spawn data,
  templates), **no readable species name/ID exists on animal instances**;
  the species is presumably resolved from templates at spawn time, and only
  numeric copies remain on the instance. Therefore name-level matching is
  not possible and the value whitelist is used instead.
- **Known limitation**: the Wild Boar Lord / Deer Lord share the same values
  (150 / 200) as their normal counterparts and cannot be distinguished by
  value, so they will also be one-shot. The Bear Lord (600) is not in the
  whitelist and keeps its challenge.

## Changelog

### v0.1.5

- **Fixed a crash** (in the game's cleanup function): v0.1.4, after matching
  by initial value, also clamped dead / reused objects whose current hit
  count (+8) was 0 to 10, writing into freed memory. Now a `v > 0` check is
  added and all zero-value objects are skipped.

### v0.1.4

- **Fixed Bear Lord being one-shot by mistake**: matching now uses the
  **initial hit count** (instance +0x10 high 32 bits, unchanged during
  combat) instead of the current +8. Previously the Bear Lord (initial 600)
  was one-shot when its +8 dropped to 500 and hit the whitelist; now its
  initial value 600 is not whitelisted and never triggers.

### v0.1.3

- Matching changed from a range to an **initial hit-count whitelist**
  `{30, 40, 50, 150, 200, 500}`, explicitly covering the bear (500).
- Notes that no species name/ID can be found in animal instance memory, so
  name-level matching is not possible.
- Wild Boar Lord / Deer Lord share 150 / 200 with normal animals and will be
  one-shot as well; Bear Lord (600) is not whitelisted and is preserved.

### v0.1.2

- Precise prey matching: +8 is a multiple of 10 in 11–200 and +0x20 is the
  animal sub-vtable.
- Fixed stamina not recovering after sleep: no longer touches non-prey /
  status objects sharing the vtable (giant rabbit, whelk, treasure box, etc.
  have +8 = 0/1).
- Note: that version matched by range; in practice only the bear (500) and
  above were excluded, while Wild Boar Lord / Deer Lord (150 / 200, same as
  normal) were still one-shot.

## Installation

1. Close the game.
2. Put the `HuntOneShot_v0.1.5` folder into the `Mods` folder in the game root.
3. Launch the game via `VillageModLoader.exe` (set it as a Steam launch option).

> Only compatible with the same game version (village.exe 18,108,416 bytes).
> Mods may break after game updates.

## Uninstallation

Delete the `Mods\HuntOneShot_v0.1.5` folder.

Author: gloaming. Please credit the source when re-sharing.
