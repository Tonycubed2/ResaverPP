# ReSaverHelper for Fallout 4 (beta)

An optional F4SE plugin for ReSaver Plus: it lets ReSaver Plus act in the running game, and gives you a rescue
key for a stuck game.

- **Call my companion and Dogmeat:** your current companion and Dogmeat are teleported to you by the game's own
  follower system (the teleport the game itself uses).
- **Get my controls back:** your controls are forced back on, whatever scene or script disabled them
  (the game's `ForceEnablePlayerControls`). **Give the controls back to the game** undoes it once the scene or
  mod that stuck them is over.
- **Save the game now:** a new save, `ReSaverPP_Rescue_<date>_<time>`. Your other saves are not touched.
- **Hold Ctrl for 8 seconds in the game:** all three at once, even when ReSaver Plus is not running.
- **Transplant (version 13):** brings your character's progress from an old save (one that crashes or will not
  load) into a good one. See below.

## Transplant

ReSaver Plus reads both saves from their files, so the old save never has to load in the game. Open the good save
in ReSaver Plus, then Tools > Transplant progress, and choose the old save. The page lists what the old save has on
top, with ticks:

- **Into the save file (A):** level, experience, S.P.E.C.I.A.L., caps, perk ranks, and perks the good save already
  has a place for. ReSaver Plus writes them into the open save; save it (File > Save).
- **Into the running game (B):** items, the other perks and quest stages. Load the good save in Fallout 4 (after A),
  then press B. The game makes a new save first (to go back to; its plugin list tells ReSaver Plus the game's load
  order), then this plugin runs one console command per thing:
  - items: `player.additem <id> <count>` (the item as the game makes it: weapon/armor mods, names and condition
    are not copied)
  - perks: `player.addperk <id>`, once per rank (each call gives the next rank)
  - quests: `setstage <quest> <stage>` (its stage script runs: rewards and follow-up quests happen normally), and
    for quests completed in the old save `completequest <quest>` (can be switched off)

The plugin only accepts these four kinds of records, with checked arguments: it cannot be used to run any other
console command. The console does not say whether a command did something, so afterwards save in the game, open
that save in ReSaver Plus and compare again: whatever is still listed did not take.

## Requirements

- Fallout 4 **1.10.163**, **1.10.984** (next-gen) or **1.11.x** (Anniversary). One DLL works on all three.
- F4SE for your game version: 0.6.23 for 1.10.163, 0.7.2 for 1.10.984, the newest 0.7.x for 1.11.x.
- **Address Library for F4SE Plugins** for your game version.

## Install

Install the zip with your mod manager, or copy `F4SE\Plugins\ReSaverHelper.dll` and `ReSaverHelper.ini` into
`Fallout 4\Data\F4SE\Plugins`. Settings are in `ReSaverHelper.ini`; ReSaver Plus can change them too.
The log is `Documents\My Games\Fallout4\F4SE\ReSaverHelper.log`.

## How it works

Everything it does in the game goes through the game's own console commands, run the way the console runs them.
That needs only two addresses from the Address Library and one structure field, the same in every version,
which is why one DLL covers all three game versions. The Skyrim helper's quest tools and journal refresh are not
part of the Fallout 4 helper; Transplant is, in the form above (the save files are read by ReSaver Plus itself).

## Build

Visual Studio 2022 with the C++ workload. Nothing else is needed (no F4SE sources, no CommonLib, no vcpkg).

```
cmake --preset vs2022-x64
cmake --build build --config Release
```

The DLL is `build\Release\ReSaverHelper.dll`.
