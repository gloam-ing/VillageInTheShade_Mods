# Camera Zoom Mod (v1.2.0)

> [中文版](README.md)

## Features

- Adjusts the three camera zoom levels (near / mid / far).
- Original values: 1.2 / 1.35 / 1.5. Default values in this mod: 0.6 / 0.9 / 1.2.
- Note: this is a "zoom multiplier" — **smaller values mean the camera is
  farther away and you see a wider area**. A value of 0.4 can cause far-view
  rendering gaps; increase the value if that happens.

## Installation

1. Close the game.
2. Put the `CameraZoom_v1.2.0` folder into the `Mods` folder in the game root.
3. Launch the game normally from Steam (Mods are auto-loaded by the
   `steam_api64.dll` bridge).

> The zoom table is located dynamically at runtime, so it usually survives
> game updates.

## Uninstallation

Delete the `Mods\CameraZoom_v1.2.0` folder.

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
