# ReSaverHelper

SKSE plugin that lets ReSaver++ (ReSaver Plus) act in the running game: complete a quest or set a
quest stage (the game's own stage scripts run, so rewards and follow-up quests happen normally), and
refresh the quest journal (brings back quests and objectives the journal stopped showing).

- Talks to ReSaver++ over a local named pipe (`\\.\pipe\ReSaverHelper`). Only the Windows user running
  the game can open it; remote connections are refused. It understands a short fixed list of commands
  (see `src/Protocol.h`).
- Mark a quest completed without its stage script, when the game refuses the completion stage (version 3).
- Transplant (version 9): takes a snapshot of the loaded character (level, skills, perks, spells, shouts,
  items, look via RaceMenu, worn gear, map locations, factions, quests and the journal itself) and puts what
  ReSaver++ sends into another save: quests are started and set to their stage, their journal entries are
  written as the other save had them, and quests the game will not start are left alone. Every step is logged.
- Followers (version 10): the snapshot lists the characters following you, the quest aliases that hold them
  (their follower mod's own system) and what they carry; Transplant brings them to you, back in those aliases,
  as your teammate, with their items (revived and enabled if needed).
- Stop a quest, or reset it as if never done (version 11): Quest.Stop / Quest.Reset, the game's own way (no rewards). Followers
  in Transplant are put back in their aliases and given their items again (fixed in version 11).
- Rescue (version 12; 12.2: ReSaver Plus lists your followers first so you pick who comes, and remembers the ones you leave out in `sGatherSkip`): bring every follower next to you (lost, stuck, left behind; follower mods and DLC followers
  too), switch your controls back on, save the game as a new save - from ReSaver Plus's "Work on the running game"
  window with no save open, or by holding Ctrl in the game for 8 seconds (`bRescueHotkey`, `iRescueHoldSeconds`), which
  does all three even without ReSaver Plus.
- Followers and beast form (version 13): take a follower back the vanilla way (DialogueFollowerScript.SetFollower), switch a
  character's teammate flag, and turn you back from werewolf / Vampire Lord form with the transformation script's own ShiftBack
  (its "already busy" guards are reset first, so a revert that stopped half way runs to the end). Version 13 also sets
  PlayerFollowerCount (0BCC98) when Transplant brings a vanilla follower back; 10-12 looked up 0BCCAE, a perk, so the count stayed 0.
- Legendary Edition has its own helper (`helper-le`), with the same commands.
- Confirms every action in game: top-left notification or a message box (`sNotify` in ReSaverHelper.ini).
- Optional: refresh the journal automatically after every save load (`bRefreshJournalOnLoad=1`).
- SE 1.5.97, AE 1.6.x / 1.7.x and VR 1.4.15 from one DLL (CommonLibSSE-NG).
- Log: Documents\My Games\Skyrim Special Edition\SKSE\ReSaverHelper.log
- What it does not do: it patches no game code (no hooks, no trampoline writes; the DLL does not even import
  VirtualProtect), listens to no game events (no death, combat, hit or animation event sinks) and touches no
  physics, ragdoll, Havok or animation graph. On its own it only waits on the pipe and, from version 12, checks
  every 0.1 s whether Ctrl is held (the rescue hotkey). Everything else happens only when you ask in ReSaver Plus,
  when Ctrl is held for 8 seconds, or after a load with `bRefreshJournalOnLoad=1`, and is written to the log.

## Install

Copy `ReSaverHelper.dll` and `ReSaverHelper.ini` to `Data\SKSE\Plugins` (or a mod folder in MO2 / Vortex).

## Build

From the Developer Command Prompt for VS 2022, in this folder:

    cmake --preset build-release-msvc
    cmake --build build/release-msvc

The DLL is `build\release-msvc\ReSaverHelper.dll`.

Apache License 2.0, like ReSaver++.
