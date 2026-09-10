# Auto Fish (AutoFish v0.1.5)

> [中文版](README.md)

## Features

- Completes the whole fishing loop automatically: auto reel on bite, auto
  finish the note minigame, auto re-cast for continuous fishing.
- Optional junk filter: when enabled, fishing never yields the four junk
  items (can, plastic bag, seaweed, branch); fish only.
- Zero key simulation: implemented with native byte patches and state machine
  hooks; no key injection, no impact on keyboard/gamepad input.
- Auto address resolution: locates game functions by byte signatures at
  startup; if signatures fail after a game update, the mod disables itself
  safely and logs the reason.

## Installation

1. Close the game.
2. Put the `AutoFish_v0.1.5` folder into the `Mods` folder in the game root.
3. Launch the game normally from Steam (Mods are auto-loaded by the
   `steam_api64.dll` bridge).

> Only compatible with the same game version (village.exe 18,105,864 bytes).
> Mods may break after game updates.

## Usage

- Equip a fishing rod and cast at any fishing spot; the minigame still shows
  and is completed automatically.
- No config file needed; auto-fishing is enabled by default.
- The junk filter is disabled by default; press **F10** to toggle it
  (keyboard only, no gamepad binding). The log records one line on each
  toggle (`no-trash ON` / `no-trash OFF (exec/skip)`), and a toast in the
  bottom-right corner shows the switch state for 1.8 seconds.

## Uninstallation

Delete the `Mods\AutoFish_v0.1.5` folder.

Author: gloaming. Please credit the source when re-sharing.
