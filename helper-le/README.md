# ReSaverHelper for Skyrim Legendary Edition

SKSE plugin that lets ReSaver++ (ReSaver Plus) act in the running game on **Skyrim Legendary Edition**
(TESV.exe 1.9.32.0 with SKSE 1.7.3). It is the LE version of `helper\` (SE/AE/VR): the same named pipe
(`\\.\pipe\ReSaverHelper`), the same commands (`src\Protocol.h`), so ReSaver++ uses it the same way.

- Quests: look at a quest, set a stage (its stage script runs), complete it, mark it completed, stop it
  (version 11, Quest.Stop: the game cleans up after the quest), refresh the journal.
- Transplant: snapshot of the loaded character and applying another save's progress, like on SE.
  Differences on LE (logged when they matter):
  - no map markers in a snapshot (LE's game headers have no list of all references; a save file still gives them)
  - the journal is set through Papyrus (on SE the helper writes the journal list directly): you may see
    an objective notification for each one
  - potions, poisons and enchantments made in the game cannot be made again; an item that carried one
    comes over without it; a poison on a weapon is not put back
- Quests without any script cannot be reached through Papyrus: the helper says so and gives the
  console command to type (the SE helper runs it itself).
- Log: `Documents\My Games\Skyrim\SKSE\ReSaverHelper.log`

## Install

Copy `ReSaverHelper.dll` and `ReSaverHelper.ini` to `Data\SKSE\Plugins` (or a mod folder in MO2 / Vortex).
Needs SKSE 1.7.3. The DLL refuses to load on any game version other than 1.9.32.0.

## Build

Visual Studio 2022 with the C++ desktop workload (its 32-bit compiler is part of it). From a Developer
Command Prompt, in this folder:

    cmake --preset vs2022-win32
    cmake --build build --config Release

The DLL is `build\Release\ReSaverHelper.dll` (with its .pdb). Nothing is copied anywhere else.

## Where the game parts come from

- `extern\skse`: SKSE 1.7.3 source as himika's libSKSE keeps it (github.com/himika/libSKSE), with its
  additions listed in `extern\skse\WHATS_DIFF.txt`. SKSE's license is in `extern\skse\skse_license.txt`.
- The few functions and offsets SKSE does not have (Actor::HasPerk, HasSpell, GetLevel, the menu pause
  count, the script engine's call functions) are from himika's libSkyrim (github.com/kassent/SkyrimSouls,
  `Skyrim\include`), documented next to each one in `src\LE.h` and `src\Papyrus.h`.

Apache License 2.0, like ReSaver++ (SKSE's files keep their own license).
