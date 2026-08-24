Village Mod Loader v1.0.1
=========================
A lightweight mod loader for Village in the Shade (Steam).

What it does
------------
- Launches village.exe in a suspended state.
- Injects every .dll found under the game's Mods folder (including
  subfolders) before the game fully initializes.
- Resumes the game and prints an injection report to the console
  window (per-module OK / FAILED plus totals).
- Injects a built-in crash reporter first; if the game crashes, it writes
  `crash.log` (exception code, faulting module+offset, registers, stack) in
  the game root.
- Does not modify any game files.

Requirements
------------
- Windows 10/11
- Steam version of Village in the Shade
  (tested on game build 24771274)

Installation
------------
1. Put VillageModLoader_v1.0.1.exe in the game's Mods folder.
2. Place compatible mod .dll files in Mods\<modname>\,
   one subfolder per mod.
3. In Steam, right-click the game -> Properties -> Launch Options,
   and enter:
     "D:\Steam\steamapps\common\Village in the Shade\Mods\VillageModLoader_v1.0.1.exe" %command%
   (adjust the path to your own game install location)
4. Launch the game normally from Steam.

Uninstallation
--------------
- Remove the loader from the Steam launch options.
- Delete VillageModLoader_v1.0.0.exe and any mod folders you no longer want.

Files in this package
---------------------
- VillageModLoader_v1.0.1.exe - the loader itself (with built-in crash log)

Notes
-----
- Because the loader uses standard DLL injection, some antivirus
  software may flag it as suspicious. This is a false positive.
- Each mod must be compatible with the loader and with your game
  version. Mods are third-party code; use at your own risk.
- This is an unofficial fan-made project. Village in the Shade and
  all related assets belong to their respective owners.

Author: gloaming
