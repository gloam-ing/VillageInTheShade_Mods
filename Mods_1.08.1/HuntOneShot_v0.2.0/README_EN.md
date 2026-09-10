# Hunt One-Shot Kill (HuntOneShot v0.2.0)

> [中文版](README.md)

## Features

- Huntable animals (squirrel, tanuki, weasel, rabbit, fox, pheasant, boar,
  deer, bear) are killed with one shot.
- Drops and hunting settlement are unaffected; animals still flee/vanish
  normally.

## Matching & Limits (v0.1.4)

- Uses an **initial-hit whitelist** (based on the initial value in the upper
  32 bits of instance +0x10, not the current value):
  `{30, 40, 50, 150, 200, 500}` (squirrel 30 / tanuki·weasel·rabbit 40 /
  fox·pheasant 50 / boar 150 / deer 200 / bear 500).
- Animal instances contain no readable species name/ID; matching is numeric
  only.
- Known limits: boar lord/deer lord share values 150/200 with normal
  variants and are one-shot too; bear lord (600) is not in the whitelist.

## Installation

1. Close the game.
2. Put the `HuntOneShot_v0.2.0` folder into the `Mods` folder in the game root.
3. Launch the game normally from Steam (Mods are auto-loaded by the
   `steam_api64.dll` bridge).

> Only compatible with the same game version (village.exe 18,105,864 bytes).
> Mods may break after game updates.

## Uninstallation

Delete the `Mods\HuntOneShot_v0.2.0` folder.

Author: gloaming. Please credit the source when re-sharing.
