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
- **Transplant (version 13, quest objectives from 14):** brings your character's progress from an old save (one
  that crashes or will not load) into a good one. See below.
- **Companions (version 15):** from ReSaver Plus's Tools > Companions window: make an actor your companion again
  (also one that left for good), tell your companion to follow, set a companion's affinity, empty a companion slot
  held by an actor without a companion script. See below.

## Transplant

ReSaver Plus reads both saves from their files, so the old save never has to load in the game. Open the good save
in ReSaver Plus, then Tools > Transplant progress, and choose the old save. The page lists what the old save has on
top, with ticks:

- **Into the save file (A):** level, experience, S.P.E.C.I.A.L., caps, perk ranks, perks the good save already has
  a place for, and **items with their weapon/armor mods, custom names and condition** (added to your inventory in
  the save, exactly as the old save had them). ReSaver Plus writes them into the open save; save it (File > Save).
  No helper is needed for this part.
- **Into the running game (B):** the other perks, quest stages and quest objectives (and items only when the good
  save's player cannot be edited by ReSaver Plus). Load the good save in Fallout 4 (after A), then press B. The game
  makes a new save first (to go back to; its plugin list tells ReSaver Plus the game's load order), then this plugin
  runs one console command per thing:
  - items (only as a fallback): `player.additem <id> <count>` (the item as the game makes it: the console has no
    command that adds an item with mods, a name or condition; `AttachMod` works on a reference in the world, not
    on an inventory item)
  - perks: `player.addperk <id>`, once per rank (each call gives the next rank)
  - quests: `setstage <quest> <stage>` (its stage script runs: rewards and follow-up quests happen normally), and
    for quests completed in the old save `completequest <quest>` (can be switched off)
  - quest objectives (version 14): `setobjectivedisplayed <quest> <objective> 1`, then
    `setobjectivecompleted <quest> <objective> 1` or `setobjectivefailed <quest> <objective> 1`, so each
    objective ends shown / completed / failed as in the old save. The game only changes objectives of running,
    not completed quests, and never takes back a completed or failed objective.

The plugin only accepts these five kinds of records, with checked arguments: it cannot be used to run any other
console command. The console does not say whether a command did something, so afterwards save in the game, open
that save in ReSaver Plus and compare again: whatever is still listed did not take.

## Companions (version 15)

ReSaver Plus's Companions window (Fallout 4 saves) shows every companion with affinity, romance, whether it left for
good and where it is. Its "Run in the game" buttons send one record each; the plugin runs the follower system's own
Papyrus functions through the console, the same as typing them:

- make it my companion again: `<ref>.cf "CompanionActorScript.UnsetHasLeftPlayerPermanently"` (only when it left for
  good), then `cqf 000289E4 "FollowersScript.AllowCompanion" <ref> 1 1` (out of DisallowedCompanionFaction, then the
  game's SetCompanion / SetDogmeatCompanion: the companion slot, follow, teammate; your current companion is dismissed)
- follow me: `cqf 000289E4 "FollowersScript.FollowerFollow" <ref>`
- affinity: `<ref>.cf "CompanionActorScript.SetAffinity" <value>` (the companion's own ModAffinity: threshold scenes,
  the companion perk, and it stays below a threshold whose scene has not played yet)
- clear a companion slot held by an actor without a companion script (a removed companion mod, or a mod that forced
  its NPC into the slot): `cqf 000289E4 "FollowersScript.ClearCompanion"` then `<ref>.setplayerteammate 0`, or for the
  Dogmeat slot `cqf 000289E4 "FollowersScript.ClearDogmeatCompanion"`. Both functions take no arguments (checked in
  FollowersScript.pex from Fallout4 - Misc.ba2). ClearCompanion's first step treats the slot holder as a
  CompanionActorScript; for this holder that step fails with a Papyrus error and the function goes on (clears the
  alias, sends the companion change event, PlayerHasActiveCompanion 0), so the teammate flag it would have turned off
  is turned off with the console's SetPlayerTeammate.

`000289E4` is the Followers quest of Fallout4.esm (always load index 00). A mod companion whose script extends
CompanionActorScript gets its own script's name in the `cf` lines. Only these four record kinds, with checked
arguments (8-digit form ids, 0/1, a number -100000..100000, a script name of letters, digits, `_` and `:`).

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

## What was checked, per game version

Checked in the game's own files: Fallout4.exe 1.11.240 (Steam), 1.10.984.0 and 1.10.163.0 (Steam depots, their
SteamStub layer removed to read the code), and the 2015 1.1.29 (its code is encrypted: only its tables were read).

- **Console commands** (AddItem 0x1002, AddPerk 0x1176, SetStage 0x1039, CompleteQuest 0x1071,
  SetObjectiveDisplayed 0x11a2, SetObjectiveCompleted 0x11a1, SetObjectiveFailed 0x1184, CallGlobalFunction /
  CallQuestFunction, SaveGame, ForceEnablePlayerControls, ResetForceEnabledPlayerControls): same name, opcode and
  parameter list in the script function tables of 1.1.29, 1.10.163, 1.10.984 and 1.11.240.
- **Their code:** in 1.10.984 every one of these commands is instruction for instruction the same as in 1.11.240
  (only addresses differ). 1.10.163 is compiled differently; read by hand where it matters: AddItem acts on the
  reference before the dot (`player.`; the console's selected reference when there is none); AddPerk needs an
  actor and passes rank 0, which the perk code turns into "the next rank" (current rank + 1, nothing past the
  perk's number of ranks); the three objective commands do nothing for a completed quest, never take an objective
  back from completed / failed, and Displayed sets it shown, Completed gives 3 from shown and 2 from dormant,
  Failed 5 / 4; the objective state change itself (which updates the Pip-Boy list) works only on running quests.
  In all three: the same.
- **The console itself** (the only per-version part of the helper): Address Library ids 526625 / 228366
  (1.10.163) and 2204287 / 2689177 (1.10.984), 2204287 / 4796464 (1.11.x):
  - Script::CompileAndRun: 1.10.163 +0x4E29B0, 1.10.984 +0x55E3E0, 1.11.240 +0x5B2690. Each is the function the
    game's own console calls with (script, compiler, 1 = system window, reference), as the helper does.
  - the form factory array: 1.10.163 +0x58D3BF0, 1.10.984 +0x2E69490, 1.11.240 +0x30EB9D0 (the array the game clears
    as 159 pointers at start; the same values F4SE 0.6.23 / 0.7.x hard-code). Slot 0x16 is
    `ConcreteFormFactory<Script, 22>` in all three; its virtual function 1 makes a new Script.
  - Script::text at +0x38 (the game's own SetText writes it) and form type 0x16, in all three.
- **The companion commands (version 15):** CallFunction "cf" 0x028b and CallQuestFunction "cqf" 0x028c have the same
  name, opcode and parameters (String, optional strings; Quest, String, optional strings) in all four tables; their
  code is the same in 1.10.984 and 1.11.240. The console reads a form argument (also cqf's quest) as "player", else
  a form whose editor id the game kept, else a hex form id (`_strtoui64(text, 16)`): 1.10.163 +0x4E7620, 1.10.984
  +0x561AE0, 1.11.240 +0x5B5D90 (984 = 1.11.240 instruction for instruction; 163 the same order). Whether the game
  keeps the Followers quest's editor id was not settled from the code, so the plugin uses the form id; the Gather
  command (version 12) used `cqf Followers` and uses `cqf 000289E4` now. cf / cqf split "Script.Function" at the
  first '.' (1.11.240 +0x60C5C0, 1.10.984 +0x5B8360, 1.10.163 +0x533770) and convert each argument to the
  function's parameter type (a form: "none", "player", "selected" or a hex form id, +0x60AA80 / +0x5B6820 /
  +0x533420; a float with atof; an int with atoi; a bool is false for "0", "false" or "").
  SetPlayerTeammate (0x11c4, reference, one Integer) is +0x5D5A40 in 1.11.240: an actor (form type 0x41) gets
  Actor::SetPlayerTeammate(arg != 0, 1, 0) (+0xC93070, which reads the favor / XP arguments only when turning it on);
  1.10.984 +0x581790 is the same instruction for instruction, 1.10.163 +0x507340 the same code.
- **Not checked:** 1.10.980 (only its Address Library file was read: the same ids), 1.11.x builds other than 1.11.240
  (the 1.11 Address Library files give the ids; no exe), and anything in the game (no test run in Fallout 4 yet).

## Build

Visual Studio 2022 with the C++ workload. Nothing else is needed (no F4SE sources, no CommonLib, no vcpkg).

```
cmake --preset vs2022-x64
cmake --build build --config Release
```

The DLL is `build\Release\ReSaverHelper.dll`.
