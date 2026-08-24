# Village in the Shade - Mine Helper (v1.0.0)

> [中文版](README.md)

## Features

- Guarantees the mine exit appears within at most 15 stones, even on floors
  with very large numbers of rocks.
- `FirstHole=1` (config): makes the first stone open the exit. Off by default;
  enable with caution.

> Two exits may occasionally appear; both work fine and don't block progress.

## Installation

1. Close the game.
2. Put the `MineHelper` folder into the `Mods` folder in the game root.
3. Launch the game as described below.

> Only compatible with the same game version (village.exe 18,108,416 bytes).
> Mods may break after game updates.

## Uninstallation

Delete the `Mods\MineHelper` folder.

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
game from Steam.

## Configuration (minehelper.txt)

| Option | Description |
|--------|-------------|
| `FirstHole` | 1 = first stone opens the exit; 0 = default |

Saving the file takes effect immediately (from the next floor).

## Files

| File | Description |
|------|-------------|
| `minehelper.dll` | The mod itself |
| `minehelper.txt` | Configuration |
| `minehelper.log` | Runtime log |

## Warning

- Two exits may occasionally appear; this does not affect gameplay.
- Please credit the source when re-sharing.

---

Author: gloaming
