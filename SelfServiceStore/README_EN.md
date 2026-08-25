# 24-Hour Self-Service Store (SelfServiceStore v0.1.4)

> [中文版](README.md)

## Features

- The following buildings are open **all day**, ignoring closing hours; you
  can enter and buy even when the owner is absent:
  - General Store (9:00-17:00, closed Fridays)
  - Lumber & Construction (10:00-17:00, closed Sat/Sun)
  - Hunter Outpost (10:00-17:00, closed Mon/Tue)
  - Library (9:00-17:00, closed Sat/Sun)
- **Tea Bowl Pavilion**: the door is open all day; ordering still follows the
  game's original logic and is subject to business hours (11:00-22:00,
  closed Thu/Fri) - not handled for now.
- The **street vendor** is not included: Kianana disappears from the map
  after closing (NPC mechanism, not a door lock), which is outside this
  mod's scope.
- **Deep night (AM 00:00-05:59): the mod auto-disables** - store doors
  return to the game's original closed state; during the day it works as
  before.

## Installation

1. Close the game.
2. Put the `SelfServiceStore_v0.1.4` folder into the `Mods` folder in the
   game root.
3. Launch the game via `VillageModLoader.exe` (set it as a Steam launch
   option).

> Only compatible with the same game version (village.exe 18,108,416 bytes).
> Mods may break after game updates.

## Uninstallation

Delete the `Mods\SelfServiceStore_v0.1.4` folder.

Author: gloaming. Please credit the source when re-sharing.
