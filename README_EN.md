# Village in the Shade Mod Collection

> [中文版](README.md)

A collection of personal mods for *Village in the Shade* (静谧田园 / ほのぐらしの庭),
built on the `VillageModLoader` DLL-injection framework.

> This repository ships **compiled binaries only** (dll / exe). Source code is
> not public.
>
> Compatible with game version `1.06` (Steam build `24771274`,
> village.exe 18,108,416 bytes). Mods may stop working after game updates.

## Mod List

| Mod | Version | Description |
| --- | --- | --- |
| VillageModLoader | v1.0.2 | Injection framework: auto-loads every `Mods\*\*.dll` on game launch; built-in crash log (with module names) |
| Teleport | v2.1.2 | Teleport menu (keyboard and gamepad builds), unlock tracking |
| CameraZoom | v1.1.0 | Adjustable camera zoom (3 levels) |
| SelfServiceStore | v0.1.6 | 24-hour self-service shopping at the general store / lumber & construction / hunter outpost / library (doors forcibly closed at deep night) |
| MineHelper | v1.0.0 | Mine helper (floor / stone counter / fast-hole mode) |
| AutoPet | v0.2.0 | Daily auto-petting for livestock & pets, affinity multiplier |
| BirthdayReminder | v0.1.0 | Reminds you of today's birthday villager and their loved gifts |
| HuntOneShot | v0.1.5 | One-shot hunting kills (initial hit-count whitelist matching) |
| TimeFreeze | v0.1.0 | Freeze time during the late night |
| Simplified Chinese Patch | v1.0.0 | Converts Traditional Chinese texts to Simplified (~54k entries); requires replacing game `data.dat` |

> The patch file (data.dat, ~350 MB) exceeds the repository single-file limit;
> download it from the **Releases** page.

## Installation

1. Close the game.
2. Copy the mod folders you want (e.g. `Teleport`) into the `Mods` folder in
   the game root. Also copy `VillageModLoader_v1.0.1.exe` (from the
   `VillageModLoader` folder) into `Mods`.
3. Launch the game using one of these methods:
   - **Steam launch options (recommended)**: game properties → Launch Options →
     `"<game root>\Mods\VillageModLoader_v1.0.1.exe" & %command%`
     (replace `<game root>` with your own path), then launch from Steam.
   - **Manual**: double-click `VillageModLoader_v1.0.1.exe` first (keep the
     window running), then launch the game from Steam.

## Configuration

The `*.txt` files in each mod folder are config / record files. Most of them
support hot-reload (Teleport keybinds, CameraZoom zoom levels, MineHelper
toggles, etc.).

## Usage Notes

- Do **not** use the Teleport mod during story events / cutscenes, and never
  force-unlock locked teleport points. We are not responsible for bugs or
  save corruption caused by this.
- Mods such as MineHelper and CameraZoom modify game memory and restore
  everything when the game exits.
- Back up your saves regularly.
- If the game crashes, VillageModLoader writes `mod_crash.log` in the game root
  with the exception code, faulting module+offset, registers and stack, which
  helps identify which mod crashed.

## License

See [LICENSE](LICENSE). Please credit the author (gloaming) when re-sharing.
