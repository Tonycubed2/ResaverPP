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
- Ghost actors (version 15, SE/AE): `GHOSTREMOVE` disables and marks for delete the spawned characters ReSaver Plus's
  Save check found with a base the save does not contain, the same as the console's `prid <id>`, `disable`,
  `markfordelete` (run through the game's console route, see `src/GhostRemove.h`), and reports each one: done, not
  found, not a created actor. Also on the in-game page (Troubleshooting > Ghost actors).
- Legendary Edition has its own helper (`helper-le`), with the same commands.
- Confirms every action in game: top-left notification or a message box (`sNotify` in ReSaverHelper.ini).
- Optional: refresh the journal automatically after every save load (`bRefreshJournalOnLoad=1`).
- Journal refresh (version 15, SE/AE): only objectives missing from the player's journal list are hidden and shown
  again; objectives the journal already lists are left alone. Each re-show puts an objective message on the HUD, so
  versions 4-14, which redid every shown objective (286 for one tester), kept the HUD busy for many minutes.
- SE 1.5.97, AE 1.6.x / 1.7.x and VR 1.4.15 from one DLL (CommonLibSSE-NG).
- Log: Documents\My Games\Skyrim Special Edition\SKSE\ReSaverHelper.log
- In-game page (version 15): a "ReSaver Plus" page in SKSE Menu Framework's Mod Control Panel (F1), when that mod
  is installed. Optional: without it the helper works exactly as before. See "The in-game page (F1)" below.
- Menu load guard (version 16, SE/AE; version 17 adds the Skills menu, a notice you see at once, and Scaleform's own
  messages in the log): when a menu's interface file does not load, the menu does not open (with a notice) instead of
  crashing the game, and ReSaverHelper.log says why the load failed. See "Menu load guard" below.
- What it does not do: apart from the menu load guard (version 16: one hook on the game's movie loading, three on the
  calls inside it, and the menu creators wrapped; version 17: two slots of the game's background movie load task,
  Scaleform's log and file opener, and, only after a failed load, the "show" message of that menu's class;
  `bMenuLoadGuard=0` leaves all of that out), it patches no game
  code, listens to no game events (no death, combat, hit or animation event sinks) and touches no
  physics, ragdoll, Havok or animation graph. On its own it only waits on the pipe and, from version 12, checks
  every 0.1 s whether Ctrl is held (the rescue hotkey). Everything else happens only when you ask in ReSaver Plus,
  on the in-game page, when Ctrl is held for 8 seconds, or after a load with `bRefreshJournalOnLoad=1`, and is
  written to the log. (The in-game page is drawn by SKSE Menu Framework, which has its own hooks; the helper adds
  none. Once you open its Script health page, the helper reads the script engine every 3 seconds until you stop it.)

## Install

Copy `ReSaverHelper.dll` and `ReSaverHelper.ini` to `Data\SKSE\Plugins` (or a mod folder in MO2 / Vortex).

## Build

From the Developer Command Prompt for VS 2022, in this folder:

    cmake --preset build-release-msvc
    cmake --build build/release-msvc

The DLL is `build\release-msvc\ReSaverHelper.dll`.

Apache License 2.0, like ReSaver++.

## Diagnostic stages (version 14)

To find out whether something in your game changes only because the helper is installed, add one line to
ReSaverHelper.ini (next to the DLL):

    iDiagnosticStage=0

and test; then 1, 2, 3, 4. Each stage stops the helper after one more step of what it does when the game starts:

- 0: nothing at all: the DLL is loaded and returns at once (it does not even write its log)
- 1: + SKSE's own setup, the log file and the settings
- 2: + the SKSE message listener (it ignores every message)
- 3: + the listener follows game loads and new games (no threads)
- 4: + the ReSaver Plus connection thread (no Ctrl hotkey thread)
- 5 (or no line): everything, including the menu load guard's hooks (version 16)

Remove the line (or set 5) to run normally. ReSaver Plus cannot talk to the helper below stage 4.

## Menu load guard (versions 16 and 17)

Every menu (Map, Journal, Skills, Inventory...) loads its interface file, `Interface\<name>.swf` (or
`Interface\Exported\<name>.gfx`), when it opens. If that load fails, the game uses the menu anyway and crashes: the
Map at once (SkyrimSE.exe+9250A0 on 1.6.640), the Journal when it shows (GFxMovieView::InvokeNoReturn), the Skills
menu a moment after it opens (SkyrimSE.exe+901699 on 1.6.640, StatsMenu::ProcessMessage). This has been seen on
1.6.640 under Proton with SkyUI, InfinityUI and Map Marker Framework, a few times per session, for files that loaded
fine earlier in the same session. The cause is not known yet; the guard is there to stop the crash and to find the
cause.

What it does (Skyrim SE and AE; not VR):

- Every failed load is written to ReSaverHelper.log: which step failed (the game's file check found no file; GFx
  could not read the file; the movie could not be created from it), how long it took, the files on disk (size,
  whether they open) and what the game's own file system finds, the memory of the game process and of the PC, open
  handles, video memory, Scaleform's own memory, and the interface files loaded since the last such list. At start
  it also logs which other mods hook the movie loading (for example Map Marker Framework) and which mod made each
  menu.
- Version 17: what Scaleform itself says. The game throws Scaleform's messages away; the helper now catches them
  (and still passes them on) and writes the ones of a failed load to the log ("GFxLoader failed to open ...", "Error
  loading SWF file ...", and so on), and which file Scaleform asked to open during the load and what it got (or that
  it asked for none). Messages from the interface scripts are only counted, not written.
- The load is tried again (`iMenuLoadRetries`, 2 by default: at once, then after 50 ms). The log says whether a
  retry worked.
- If every try fails, the menu does not open and a notice says "ReSaverHelper: MapMenu could not load its interface
  file (Map.swf) and did not open. See ReSaverHelper.log." The game goes on; open the menu again to try again.
- Version 17: the Skills menu (StatsMenu) loads its file a moment after it opens, in the background. If that load
  fails, the helper keeps the menu from using the missing file and closes it (as if you closed it right away), with
  the same notice.
- Version 17: the notice comes at once, also while a menu has the game paused: with `sNotify=hud` it is the
  top-left notification, or a message box while the game is paused in a menu (the notification would only show
  later); `sNotify=box` always uses a message box; `sNotify=none` shows nothing (the log and F1 page still have it).
- The F1 page (Troubleshooting tests > Helper and connection) and the report count the failures per file and say
  what the guard did.

Settings in ReSaverHelper.ini (read when the game starts):

- `bMenuLoadGuard=1`: 0 = off: no hooks at all, the game behaves as without the helper.
- `iMenuLoadRetries=2`: tries after a failed load, 0 to 5 (the second and later wait 50 ms).
- `bLogAllMovieLoads=0`: 1 = one log line for every interface file loaded (normally only failures, with the
  loads since the last list, to keep the log small).
- `bMenuLoadDiagnostics=1` (version 17): 0 = do not watch Scaleform's log and file opener.
- Test only, leave empty in normal play: `sTestFailMovies=` lists interface file names (without .swf, separated by
  commas, any case, e.g. `Map,Quest_Journal,StatsMenu`) whose load is made to fail on purpose, to see the guard work.
  `StatsMenu` is the Skills menu: it tests the background load (version 17). `sTestFailMode=first` fails only the
  first try (the retry works, the menu opens); `all` fails every try (the menu does not open, the notice shows).
  Forced failures are marked "TEST: forced failure" in the log, "(TEST)" on the F1 page and "- test" in the notice.

When it happens, please send ReSaverHelper.log (and the save, if a menu would not open), as soon as you can: the
log is rewritten each time the game starts.

## The in-game page (F1) (version 15)

Needs [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) (version 3, tested against
3.18). It is optional: when SKSEMenuFramework.dll is not loaded, the helper writes one line to its log and works
exactly as before. Skyrim SE and AE. The page is English only (the helper has no translations).

Press F1 (SKSE Menu Framework's key) and open "ReSaver Plus". The page is not added during a diagnostic stage test
(iDiagnosticStage 0 to 4). Buttons that change the game ask first; most of what they do happens when you close the
menu. Everything is written to ReSaverHelper.log. Hover any button or line for details.

Every list on the page is a table sorted A to Z by name when it opens. Click a column's header to sort by that
column, click it again to reverse the order; the page remembers the choice. The long lists (effects, quests, scripts)
have a search box above them: only the rows containing that text are shown.

- **Rescue**
  - Save now: saves as a new save, ReSaverPP_Rescue_<date>_<time>.
  - Get my controls back: EnablePlayerControls. (Skyrim has nothing to give back afterwards: "Give the controls back
    to the game" in ReSaver Plus is for Fallout 4 only.)
  - Bring my followers: every follower (vanilla, DLC, follower mods, teammates) next to you; sGatherSkip is respected.
  - All three: the same as holding Ctrl for 8 seconds (the save first).
- **My character**: every source of magic effects on you (a spell, an ability, an item's enchantment...) in one table:
  Name, Type (worn item, item not worn, spell, power, shout, scroll, potion / food, poison, ability, disease,
  addiction, from someone else), Plugin (the plugin that adds it), Effects (how many) and Running (how many act on
  you right now). Sort by any column (for example Effects, most first) and search by name, item, type, plugin or
  effect name. "Group by kind" (on by default) keeps the groups (worn items, items not worn, spells, potions,
  abilities, diseases, put on you by others) with your sort inside each; switch it off for one flat list. Click a
  row to see its effects below the table (a sortable table too: effect, strength, time left, state = running,
  switched off = its condition is not met, or ended) and the buttons. Stuck effects (an item's never-ending
  enchantment while nothing you wear carries it, or the item is gone) are listed first, in red, A to Z.
  - Remove (one effect): dispels it, as a script's Dispel() does.
  - Take it off (a worn item): unequips the item, as Actor.UnequipItem does.
  - Remove it (an ability or disease): Actor.RemoveSpell.
  - Remove all / Remove them (stuck): dispels each effect of that source.
- **Followers**: your followers and teammates, where they are and why they count as followers (sortable by name,
  where, why).
  - Bring: just that one, next to you.
  - Teammate on / off: Actor.SetPlayerTeammate.
  - Dismiss (the vanilla follower): the follower quest's own dismissal (DialogueFollower stage 400).
  - Take back: DialogueFollowerScript.SetFollower (makes them your vanilla follower again).
  - Get out of werewolf / Vampire Lord form: the transformation script's own ShiftBack().
- **Quests**
  - Refresh the journal: brings back quests and objectives the journal stopped showing.
  - Fix a stuck quest: the running quests in a table (Name, Editor ID, Plugin, Stage; quests with no name at the end),
    with a search box (name, editor ID, plugin or form ID). Click one to see its stage, its stages and its
    objectives. Set (a stage the quest has: Quest.SetStage, its script runs), Complete (an objective:
    Quest.SetObjectiveCompleted), Stop quest (Quest.Stop).
- **Troubleshooting**
  - Script health: script threads (Papyrus stacks), how many wait on a timer, wait their turn (suspended), or are
    calls not started yet, read every 3 seconds, with a trend ("growing" for minutes = script lag), a graph and the
    scripts with the most threads (the 100 busiest script functions, sorted by threads, most first, or by name;
    with a search box). Save health: warns when the script engine was asked to pause (as a save does)
    and cannot, which makes the next save bad; how to test it with the console's `ts`.
  - Write a report: game and helper versions, cell, characters around you with problems, followers, script health,
    plugin counts, settings. "Write the report..." asks where to save it with the Windows Save As dialog, starting
    where it always went: ReSaverHelper-report.txt in the SKSE log folder. Open it opens it in your editor.
  - Ghost actors: paste or type the ids ReSaver Plus's Save check lists (spawned characters whose base NPC is a
    temporary copy the save does not contain; Ultimate Deadly Encounters' clone spawns make them), tick the ones to
    remove (a table sortable by form ID or result), tick "I understand the risk" and remove them: for each, the console's `disable` then `markfordelete`,
    one at a time, each step written to ReSaverHelper.log first. Touching a broken character can crash the game: make
    a save first; afterwards save as a new save and open it in ReSaver Plus to check. The live game cannot tell which
    characters are ghosts, so the list always comes from ReSaver Plus.
- **Troubleshooting tests**
  - Diagnostic stages: what each stage does, buttons that write iDiagnosticStage to ReSaverHelper.ini (takes effect
    when the game starts again), "Back to normal", and how to run the test.
  - Test spawns: a wolf (EncWolf 00023ABE), deer (EncDeer 000CF89D) or sabre cat (EncSabreCat 00023AB5) from
    Skyrim.esm in front of you; Kill the creature; Kill the one I look at (the crosshair target, or the console's
    selected reference).
  - Helper and connection: helper, game and menu framework versions, whether ReSaver Plus is connected, bListen,
    bRescueHotkey, "Menu load problems" (version 16: whether the guard is on, and per interface file how often it
    failed, worked on a retry, or the menu could not open; hover a line for the step that failed), and where the logs
    are. The report (Write a report, What to send > Save...) has the same lines.
  - What to send: the list of files to attach, with their full paths, a Copy button, and Save...: one text file
    with the list and the report, saved where you choose (the Windows Save As dialog, starting on your Desktop as
    ReSaverHelper-report-<date>.txt).
  - The Save As dialog opens over the game; the game keeps running while it is open, and comes back to the front when
    it closes. If you do not see it, press Alt+Tab. In exclusive fullscreen Windows cannot show a dialog over the
    game: the file is then saved to the proposed place (the SKSE log folder for the report, the Desktop for What to
    send) and the page says where.
- **About**: the helper version and the ReSaver Plus Nexus page.

Build note: `src/SKSEMenuFramework.h` is the framework's official API header (from its MIT-licensed example
project); it looks up SKSEMenuFramework.dll at run time, so nothing is linked against it.
