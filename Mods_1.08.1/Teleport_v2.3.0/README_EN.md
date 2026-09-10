# Teleport Mod (v2.3.0)

> [中文版](README.md)

## Features

- Press **F6** to open/close the teleport menu; **PageUp / PageDown** select a
  destination, **[ / ]** page, **Enter** teleports, **Esc** closes.
- **F7** toggles the top-left HUD coordinates (hidden by default, for testing).
- **Alt+F6**: manually start locating at the farmhouse door (required again
  after loading a save or a day change).
- Gamepad (full version): LB toggles the menu, Left/Right selects, Down pages,
  RB confirms, A opens settings.
- Key bindings can be changed in-game and are saved back to `teleport.txt`.
- Destinations are stored in `teleport.txt` (`Dest1Name/X/Y ...`) and can be
  edited freely.
- Player coordinates are located with a CE-style anchor convergence (fixed
  coordinate near the farmhouse door); convergence is triggered manually with
  Alt+F6 after loading a save or a day change - no automatic scanning. It does
  not depend on the player object vtable, and addresses are re-located by byte
  signatures after game updates.
- The F7 HUD only reads the converged copies that actually follow player
  movement, so static (non-following) copies cannot stall the display.

## Four DLLs in This Version (pick one)

| DLL | Target | Notes |
|------|--------|-------|
| `teleport.dll` | Desktop PC | Keyboard + gamepad, fixed-size UI |
| `teleport_kbd.dll` | Desktop PC | Keyboard only, fixed-size UI |
| `teleport_sd.dll` | Steam Deck | Keyboard + gamepad, auto-scaling UI |
| `teleport_kbd_sd.dll` | Steam Deck | Keyboard only, auto-scaling UI |

All four DLLs share the same logic (v2.3.0); they differ only in gamepad
support and UI scaling.

## Steam Deck Screen Adaptation

- The two SD DLLs auto-scale the HUD and menu to the screen resolution
  (1920x1080 baseline, capped at 1.0, floored at 0.5). On Steam Deck's native
  1280x800 this is about 0.67: windows and fonts shrink proportionally.
- For a fixed scale, set `HudScale` in `teleport.txt`: 0 = automatic,
  0.4~2.0 = manual (ignored by the two desktop DLLs).
- On Steam Deck, keyboard-only builds work with a docked/Bluetooth physical
  keyboard or the Steam+X virtual keyboard.

## Installation

1. Close the game.
2. Put this folder into the `Mods` folder in the game root.
3. **Pick one of the four DLLs and delete or rename the other three to
   `.dll_bak`** (they cannot coexist: the loader loads every `*.dll` and they
   would conflict).
4. Launch the game normally from Steam (Mods are auto-loaded by the
   `steam_api64.dll` bridge).

## Usage & Notes

- A destination must have been visited (within 300 units by default) before it
  can be teleported to; unlock records are written to the `Unlock=` lines in
  `teleport.txt` automatically - do not edit them manually.
- After loading a save or crossing a day, stand still at the farmhouse door and
  press **Alt+F6** to locate (about 1~2 seconds to converge). Teleport stays
  unavailable until locating succeeds. A failed attempt (wrong spot or moving
  during the scan) is not retried automatically - press Alt+F6 again.
- If the camera does not follow after teleporting, or you have to move to reach
  the target, the convergence was inaccurate - **do not teleport again that
  day, there is a crash risk**.
- **Back up your save before use.**

## Configuration

`teleport.txt`: `Enabled`, `TeleportKey`, `HudKey`, `UpKey`, `PadToggleKey`
etc. (editable in-game); `HudScale` UI scaling (SD DLLs only);
`UnlockRadius` visit-unlock radius; `DestNName / DestNX / DestNY` destination
list.

## Uninstallation

Delete this folder.

---

Author: gloaming. Please credit the source when re-sharing.
