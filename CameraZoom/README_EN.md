# Village in the Shade - Camera Zoom Mod (v1.1.0)

> [中文版](README.md)

## Changelog

### v1.1.0

- Switched to the VillageModLoader injection system: no more lz4 proxy DLL,
  no modification of original game files.
- The three camera zoom levels can be customized in `camera_zoom.txt`.

## Features

- Adjusts the three camera zoom levels (near / mid / far).
- Original values: 1.2 / 1.35 / 1.5. Default values in this mod: 0.6 / 0.9 / 1.2.
- Note: this is a "zoom multiplier" — **smaller values mean the camera is
  farther away and you see a wider area**. A value of 0.4 can cause far-view
  rendering gaps; increase the value if that happens.

## Installation

1. Close the game.
2. Put the `CameraZoom` folder into the `Mods` folder in the game root.
3. Launch the game as described below.

> Only compatible with the same game version (village.exe 18,108,416 bytes).
> Mods may break after game updates.

## Uninstallation

Delete the `Mods\CameraZoom` folder.

## Launching

This mod is injected via `VillageModLoader.exe`. Setting it as a Steam launch
option is recommended:

1. Steam Library → right-click Village in the Shade → Properties → Launch
   Options.
2. Enter the following (replace the path with your own game root):

   ```
   "<your game root>\Mods\VillageModLoader.exe" & %command%
   ```

3. Launch the game from Steam.

Alternatively, run `Mods\VillageModLoader.exe` manually before launching the
game from Steam (keep its window running).

> The mod will not take effect if the loader is not running.

## Files

| File | Description |
|------|-------------|
| `camerazoom.dll` | The mod itself |
| `camera_zoom.txt` | Zoom levels and patch configuration |

## Warning

- Very small zoom values may cause far-view rendering gaps; keep reasonable
  values.
- Please credit the source when re-sharing.

---

Author: gloaming
