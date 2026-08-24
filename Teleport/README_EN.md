# Village in the Shade - Teleport Mod (v2.1.0)

> [中文版](README.md)

## Changelog

### v2.1.0 (2026-08-20)

- Fixed a bug where teleporting to points with small coordinates could corrupt
  recorded destination coordinates.
- Destination limit raised to 24; beyond that, recorded points cannot be
  unlocked.
- Key changes are now synced in place in the config file (no more appended
  history).
- Added a keyboard-only build `teleport_kb_only.dll` (choose one of the two
  builds; they share the same config).
- Player coordinates HUD `player pos now: (x, y)` in the top-left corner, off
  by default, toggle with F7.
- New destinations: hot spring, mountain lake fishing spot, mountain top.
- Custom destinations supported: add a name and coordinates following the
  template in `teleport.txt`.

### v2.0.0 (2026-08-19)

- Gamepad support: LB menu, RB teleport, ←/→ select, ↓ page, A settings page;
  keybinds can be changed in the settings page.
- Settings page auto-detects the current input method (keyboard / gamepad) and
  edits the matching config.
- UI redesign: translucent menu window in the center, key hints in the top-left
  (yellow for gamepad, green for keyboard).
- Camera mod is no longer bundled.
- Switched to VillageModLoader injection: no lz4 proxy, no modification of
  original game files.

## Installation

This mod is based on the VillageModLoader injection system and coexists with
other mods (camera, QoL, etc.) without conflicts or file modification.

1. Close the game.
2. Put `VillageModLoader.exe` into the `Mods` folder.
3. Put the `Teleport` folder into the `Mods` folder.
4. Launch the game as described below.

> Default build is `teleport.dll` (keyboard + gamepad). Keyboard-only players
> can back up `teleport.dll` outside `Mods` or delete it (choose one build;
> never keep both in `Mods` — the loader will inject both and cause conflicts).

> Only compatible with the same game version (village.exe 18,108,416 bytes).
> Mods may break after game updates.

### Upgrading / keeping unlock records

The new template differs from old versions (destination list, default keys,
etc.). When upgrading, keep only your own unlock records and use the new
template for everything else:

1. Use the new `teleport.txt` template (shipped with the release).
2. Open the old `teleport.txt`, copy all `Unlock=` unlock record lines (at the
   end of the file) into the new `teleport.txt` below the
   `#以下内容自动写入` marker.
3. If you remapped keys in the old version, edit them in the new file as
   needed.

## Uninstallation

Delete the `Mods\Teleport` folder.

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

> If the loader isn't running, the mod won't work (pressing F8 in a save has
> no effect).

## Controls

| Action | Keyboard | Gamepad |
|--------|----------|---------|
| Open / close menu | F8 | LB |
| Settings page | `、` | A |
| Select | PgUp / PgDn | ← / → |
| Page | [ / ] | ↓ |
| Teleport | Enter | RB |
| Close menu | Esc | LB (press again) |
| Show / hide player coords | F7 | — |

- A destination unlocks only after you have visited nearby (within 300 units).
- Note: teleporting directly changes coordinates; same-map teleports are
  recommended. Cross-map teleports may place you in an odd position.

## Files

| File | Description |
|------|-------------|
| `teleport.dll` | The mod (keyboard + gamepad) |
| `teleport_kb_only.dll` | Keyboard-only build (replace `teleport.dll`; choose one) |
| `teleport.txt` | Config and destination list (not overwritten on updates) |
| `teleport.log` | Runtime log (cleared on each launch) |

## Warning

- Do not use this mod during story events.
- Never force-unlock unvisited destinations (you must visit them in person).
- We are not responsible for any bugs or save corruption caused by this.

> Please credit the source when re-sharing.

---

Author: gloaming
