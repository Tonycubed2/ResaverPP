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
which is why one DLL covers all three game versions. The Skyrim helper's quest tools, journal refresh and
Transplant are not part of the Fallout 4 helper.

## Build

Visual Studio 2022 with the C++ workload. Nothing else is needed (no F4SE sources, no CommonLib, no vcpkg).

```
cmake --preset vs2022-x64
cmake --build build --config Release
```

The DLL is `build\Release\ReSaverHelper.dll`.
