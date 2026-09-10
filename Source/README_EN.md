# Village in the Shade — Mods (1.08.1 archive)

> **Status: unmaintained (archived).**
> The last game version this project was aligned to is **1.08.1**
> (`village.exe` 18,105,864 bytes, buildid 24969282).
> Game versions **1.09 and later are not adapted and not verified**.
> Anyone is welcome to take over, port or rewrite these mods.
> This repository is kept as a source archive and reference only.

---

## 1. Contents

Complete C source of 10 mods plus one shared utility layer.
Every mod is a **single-file implementation** and only depends on `shared/`.

|            mod            |                    source                    |
| :-----------------------: | :------------------------------------------: |
|        Auto fishing       |         `AutoFish_v0.1.5/autofish.c`         |
|    Machine automation     |         `Automate_v0.5.4/automate.c`         |
|        Auto petting       |          `AutoPet_v0.2.5/autopet.c`          |
|     Birthday reminder     | `BirthdayReminder_v0.1.2/birthdayreminder.c` |
|        Camera zoom        |       `CameraZoom_v1.2.0/camerazoom.c`       |
| One-shot hunting (clamp)  |      `HuntOneShot_v0.2.0/huntoneshot.c`      |
|        Mine helper        |       `MineHelper_v1.1.1/minehelper.c`       |
| 7x11 self-service store   |   `SelfServiceStore_v0.1.8/selfservice.c`    |
|       Map teleport        |         `Teleport_v2.3.0/teleport.c`         |
|        Time freeze        |       `TimeFreeze_v0.1.2/timefreeze.c`       |

|                        shared layer                         |         `shared/`       |
| :---------------------------------------------------------: | :---------------------: |
|          Runtime signature-based address resolution          | `addrsig.c / addrsig.h` |
| Logging `LogKit` / memory `mk_*` / hook transaction `HookTxn` / version profile `GameProfile` |  `modkit.c / modkit.h`  |

## 2. Building

Zig is required (tested with 0.14.1). Common template:

```bat
zig cc -shared -O2 -fms-extensions -I..\shared -o <Mod>.dll <mod>.c ^
    ..\shared\addrsig.c ..\shared\modkit.c [-lgdi32] -luser32
```

- Mods with a window / HUD / toast need `-lgdi32`: `AutoFish`, `TimeFreeze`,
  `BirthdayReminder`, `MineHelper`, `AutoPet`.

## 3. Disclaimer

- For single-player use only. No game files or game assets are distributed; back up your saves.
- A game update may silently disable a mod (a signature `MISS` degrades the feature).
  This is intended behaviour, not a crash.



Author: gloaming. Please credit the source when reposting or sharing.
