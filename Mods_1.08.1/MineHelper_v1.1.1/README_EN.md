# Mine Helper (v1.1.1)

> [中文版](README.md)

## Features

- Guarantees the mine exit appears within at most 15 stones, even on floors
  with very large numbers of rocks.
- `FirstHole=1` (config): makes the first stone open the exit. Off by default;
  enable with caution.

## Installation

1. Close the game.
2. Put the `MineHelper_v1.1.1` folder into the `Mods` folder in the game root.
3. Launch the game normally from Steam (Mods are auto-loaded by the
   `steam_api64.dll` bridge).

## Uninstallation

Delete the `Mods\MineHelper_v1.1.1` folder.

## Configuration (minehelper.txt)

| Option | Description |
|--------|-------------|
| `FirstHole` | 1 = first stone opens the exit; 0 = default |

Saving the file takes effect immediately (from the next floor).

## Files

| File | Description |
|------|-------------|
| `minehelper.dll` | The mod itself |
| `minehelper.txt` | Configuration (FirstHole / F9 toggle) |

---

Author: gloaming. Please credit the source when re-sharing.
