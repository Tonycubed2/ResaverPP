# ReSaver Plus (ReSaverPP): handoff for a new Claude thread

Read this first. It is written by the previous thread (October 2026) so a fresh thread can continue
without asking Tony to explain everything again. Tony (GitHub: Tonycubed2, Nexus: tonycubed2) owns the
project; you do the coding, building, delivering and publishing.

## 1. What the project is

- C++ port of ReSaver (FallrimTools save editor by Mark Fairchild, Apache 2.0) with a full Dear ImGui GUI,
  for Skyrim SE/AE saves, plus Skyrim LE saves. Name on Nexus: **ReSaver Plus**.
- Nexus pages (every release goes to BOTH):
  - Skyrim SE mod **196293**. Main file id 8208150, unique mod id 7318624468677 (defaults in the workflow).
  - Skyrim LE mod **122557** ("ReSaverPP LE"). File id 8246049, unique mod id 472446525117.
- Main file is manual download only. **ReSaverHelper** (SKSE DLL, `helper/`, project `C:\dllmaker\ReSaverHelper`)
  is an optional file on the SE page so the app can act in the running game.
- **ReSaverHelper LE** (`helper-le/`, project `C:\dllmaker\ReSaverHelperLE`): the Legendary Edition helper, 32-bit,
  SKSE 1.7.3, TESV.exe 1.9.32.0, built with VS2022 for Win32 (`cmake --preset vs2022-win32`,
  `cmake --build build --config Release`). Same pipe and protocol as `helper/` (HELLO reports "LE"). SKSE source
  vendored in `helper-le/extern/skse` (himika's libSKSE); missing addresses from himika's libSkyrim
  (github.com/kassent/SkyrimSouls) documented in `src/LE.h` and `src/Papyrus.h`. Papyrus calls go through the VM
  vtable (0x25 static, 0x27 method by handle; SE's slots minus one). No console on LE. Transplant differences
  (no map markers / jobj in snapshots, no made-in-game potions/enchantments) are listed in its README.
  Released as the optional file "ReSaverHelper LE" (File ID 8276523) on the LE page with the workflow
  `nexus-helper-le.yml` (uploads the zip in `helper-le/release/`, Tony's own build; file_id defaults to 8276523).
  SE helper: optional file 8222850 on the SE page, updated with `nexus-update-file.yml`. Helper 11 published with v41; helper 12 (both, rescue: GATHER/CONTROLS/SAVEGAME/RESCUE, hold Ctrl 8 s) with v49 (LE untested in game: Tony has no SKSE on LE); fixed as 12.1 (Gather: half circle behind the player, holding-cell helpers skipped, sGatherSkip, save before moving); 12.2 with v50: FOLLOWERS list + GATHER <ids> (pick who to bring in the app), unticked remembered in sGatherSkip as Plugin.esp:localid. 12.2.1 (both, 2026-11-02): packaging only, the DLL was stored read-only (0444, from Tony's file) and Linux users could not overwrite it; when making helper zips, chmod 644 the files first.
- Fallout 4 saves (.fos, "FO4_SAVEGAME", ported from FallrimTools' isFO4 branches): header has no compression field, RGBA
  screenshot; body starts form version + wstring game version; light plugins at form version 68+; table 3 count not
  under-reported; Papyrus: 64-bit ids, 32-bit strings above form version 61, struct defs (count after the script count)
  and struct instances (after references; data after reference data), script instance extra byte when
  (unknown2bits & 3) == 3, thread "attached" u64 after the fragment (not for kind 2 / TerminalRunResults), no byte
  after the frames when thread flag == 0, fragments +4 bytes (quest/scene kinds; kind 2 = running thread id), no
  saveFileVersion u16. Change form types renumbered from 28 (no NOTE); change forms are NOT decoded for FO4
  (ChangeFormData::Decode returns an error), quests/journal off. GUI: App::fo4() + SkyrimOnly(Sc) grey out the
  tools that need decoded change forms (Leftovers, MCM, Havok, Purify, Edit player, Find NPCs, Transplant, Quests,
  Open .pex; the merged patch works for Fallout 4 since 2026-11-05, see below).
  tools that need decoded change forms (Leftovers, MCM, Edit player, Find NPCs, Transplant, Quests,
  merged patch, Open .pex).
- v54 (published 2026-11-05 on SE, LE and Fallout 4): Fallout 4 is a third game setup (`App::_game` 0 SE / 1 VR / 2 FO4, `_setups[3]`,
  ini keys `active_game=`, `*_f4=`; `setGame(int)`; platform::DefaultSavesFolder/SkyrimInstallFolders/MyGamesName and
  loadorder::GamePluginsTxt/Read take the game number). Opening a .fos switches to it and scans its mods folder (Data
  or MO2 "Fallout 4" / Vortex "fallout4"). Working for FO4: missing mods (Fallout4Masters, Fallout4.ccc, FO4 Creation
  Club "cc????fo4###"), Find form / plugin names (PluginDB Input::fo4: names and editor IDs only, Fallout4_en.strings),
  Papyrus log, saves manager (.fos/.f4se), crash log (Buffout 4 / NG, Documents\My Games\Fallout4\F4SE), file
  conflicts and the archive window (BA2 GNRL/DX10 v1/7/8 in BsaArchive; FO4 archive order: Fallout4_Default.ini
  [Archive] lists, then "<plugin> - Main/Textures/Voices_xx.ba2"). Namespaced scripts: Scripts\DLC01\Foo.pex = "DLC01:Foo".
  Tested on Tony's Steam install via a static Linux harness in the device shell (C:\dllmaker\ReSaverPP\ba2test\f4h):
  1638/1638 save scripts found (448 namespaced), 355516 forms named in 3 s, 55 archives / 506479 files in 0.3 s.
  Test hook RESAVERPP_TEST_SHORTCUT=<App::Sc number> opens any tool.
  Tested on Tony's 3 vanilla+DLC saves (2016, v13, form 65) and 2 next-gen saves (1.10.984, v15, form 68, 7 full + 9
  light plugins, 9 threads with attached ids): all byte-identical round trips; terminating and deleting threads
  writes and reads back cleanly. Also 5 saves of a 100%-completed game (1.10.138, 25-27 MB, ~100k instances, threads
  with TerminalRunResults fragments and function messages): all byte-identical.
- v56 (2026-11-05, not published): the game's own scripts (ScriptSources::gameScripts: scripts in the official archives of the
  game's Data folder - Skyrim - *.bsa, Skyrim_VR - *, Update/DLC/cc*.bsa; Fallout4 - *.ba2, DLC* - *.ba2, cc*.ba2) are marked "* " in
  the Scripts, Instances, References, Threads, Messages and Suspended lists and in a mod's script table; "hide game scripts (*)"
  (ini hide_game_scripts=) hides them there and hides the base game / DLC / Creation Club plugins in the Mods view. Checked on
  Tony's SE (15162 of 15201), LE (13499) and FO4 (10342) Data folders.
- Fallout 4 Transplant (2026-11-05, not published; needs helper-f4 version 13, which Tony must build in VS2022:
  `helper-f4/src/Transplant.h/.cpp` new, `Server.cpp`, `Protocol.h`, `CMakeLists.txt` changed): `core/TransplantFO4.*`
  (`ess::transplant::fo4`) reads BOTH saves offline (FromSave: level, XP, SPECIAL, caps, perks + rank, player inventory,
  quests with Pip-Boy flag; forms as plugin + local id, light plugins FE), Diff() marks each change kFile (written with the
  fo4cf setters: SetPlayerLevel/XP/Special/Caps, SetPerkRank, AddPerk when the save has a reference) or kGame (helper APPLY
  records: `additem id count`, `addperk id ranks`, `setstage quest stage`, `completequest quest`; nothing else accepted).
  GUI `gui/AppTransplantF4.cpp` (drawTransplant redirects for fo4()): A writes the file part, B asks the game to SAVEGAME
  first, waits for that .fos in the saves folders, uses its plugin list as the game's load order (RuntimeID), then sends the
  records. Console commands checked in Fallout4.exe 1.11.240's script function table (0x1002 AddItem, 0x1176 AddPerk: rank 0
  = next rank, 0x1039 SetStage, 0x1071 CompleteQuest). CLI `--fo4-transplant <old.fos>` [`--fo4-transplant-write -o`].
  Tests: rs_unit with `RESAVERPP_FO4_SAVES="dir;dir"` reads real saves (Tony's Save7 -> Save8 must list the 9 Pipe Pistols
  00024F55). Test hook `RESAVERPP_TEST_TPOLD=<old.fos>` + `RESAVERPP_TEST_SHORTCUT=12`. Not tested in the real game yet.
- Fallout 4 Transplant 2 (2026-11-06, branch tp2, not published; needs helper-f4 version 14 for objectives, which Tony must
  build in VS2022: `helper-f4/src/Transplant.h/.cpp`, `Protocol.h`, `Server.cpp` changed): items now go INTO THE SAVE FILE with
  their extra data (`tf4::AddItemStacks`, Kind::kItem Via::kFile): per stack object mods (extra 0x35 BGSObjectInstanceExtra,
  LoadGame 0x24a240: u16 template index, u32 n, n x {ref, u8 attach index, u8 rank, u8 flags from form version 0x15}),
  text display data (0x99, 0x2acdf0: ref message, ref quest, i32, u16, wstring name when both null and -2, vsval n x
  {wstring, ref}) and ExtraHealth (0x25 f32); `StackData::key()` = what makes two stacks the same thing. Other stack extras
  (0x2e ExtraLeveledItem, 0x9f ExtraUniqueID, 0x88 ExtraAliasInstanceArray, 0x95 ExtraFromAlias, 0x1c ExtraReferenceHandles,
  0x21 ownership, 0xc9 ExtraModRank, 0x48, 0x8e, 0xa0; names from RTTI) are left behind. Stacks are added to the player's
  last entry for that item (u32 stack count raised) or as a new entry (u32 item count raised); stack flags 0 (u16 at +0x24 of
  BGSInventoryItem::Stack, 0x34a310). Form ids are moved plugin+local -> the good save's load order; a form id the save's
  array lacks is APPENDED to `formIDs` (the loader 0xbeadb0 reads the array as u32 count + ids, index i+1 -> id, nothing
  else). Needs `Actor::complete` for the player; the actor part is kept byte for byte and everything is decoded again.
  In-game fallback (plain additem) only when the player cannot be edited or a stack names a form the good save cannot
  (created forms, missing plugins). No console command adds an item with mods/name/condition: AttachMod (amod, 0x5f7f20)
  edits a REFERENCE's own extra data (the calling or console-selected ref), not an inventory item.
  Quest objectives: Kind::kObjective (QUST change flag 29: vsval n x {u32 index, u32 state}; TESQuest::LoadGame 0x66ca40 only
  stores the state byte (+0x1f) of objectives the quest has; the Pip-Boy list lives in PlayerCharacter and is kept by the
  objective's SetState 0x629bc0, which also runs only for running quests, marks change flag 29, fires the HUD/event code
  and calls the player 0xd60610) -> not edited offline, sent as helper record `objective <quest> <index> <state>` ->
  setobjectivedisplayed/completed/failed (0x11a2/0x11a1/0x1184: Displayed 1 -> SetState(1) unless shown; Completed: 3 from
  shown, 2 from dormant, nothing from 2..5; Failed 5/4). Only objectives the good save has dormant/shown, of quests running
  and not completed in the old save and listed in the Pip-Boy; sent after all stages.
  Versions: the command tables (names, opcodes, parameters; tools/fo4emu/cmdtab.py <exe> <out>) of 1.1.29, 1.10.163,
  1.10.984 and 1.11.240 are the same for every command the helper uses. Tony's 1.10.163 / 1.10.984 / 1.1.29 exes are
  SteamStub 3.1 wrapped: the 163/984 code was decrypted locally to read it (scratchpad tool, not in the repo; 1.1.29 left
  encrypted). tools/fo4emu/norm.py compares functions instruction by instruction across exes: 1.10.984 = 1.11.240 for every
  command used and for CompileAndRun / Script::SetText; 1.10.163 read by hand (same logic). Console path per version:
  CompileAndRun 163 +0x4E29B0 / 984 +0x55E3E0 / 1.11.240 +0x5B2690 (the console's own callers pass script, compiler, 1,
  ref), form factory array +0x58D3BF0 / +0x2E69490 / +0x30EB9D0 (cleared as 0x4f8 bytes at start; = F4SE's values), slot
  0x16 = ConcreteFormFactory<Script,22>, vfunc 1 allocates 0x80 + Script ctor (type byte 0x16), Script text +0x38: all
  verified. Not verified: 1.10.980 exe, other 1.11 builds, F4SE plugin loading on old versions, anything in the real game.
  Checks: rs_unit (506 with RESAVERPP_FO4_SAVES): Save7 -> Save8 = one file change, stacks 8 x "Pipe Pistol" 6 mods +
  1 x "Pipe Pistol" 13 mods, written, read back: 16 pistols, Diff empty; Save14 -> Save8 (fo4full) 113 file changes incl.
  103 items, 26 form ids appended, read back only the game part left. tools/fo4emu invcheck.py + emucheck.py on the edited
  players (dump of ACHR 0x14 in dumpcf format): the game's inventory loader and Actor/PlayerCharacter::LoadGame read to the
  exact end.
- Fallout 4 merged patch (2026-11-05, not published): `core/MergedPatch.*` with `Options::game` 2 (or Fallout4.esm in the load
  order). Merged types: LVLI, LVLN, LVSP, CONT, FLST, OTFT, MISC, KEYM (+ the types listed below), layouts from xEdit's wbDefinitionsFO4 (dev-4.1.6) in
  `LayoutFO4`/`Lists`/`Order`/`Groups`/`Fields` (a_fo4): LVLO 12 bytes with chance none, LLCT u8, LVLM, LVLG, LLKC/CVPA/DAMC/PRPS
  as {form, 4 bytes} pairs (kFormPairs), LVSG, LVLI ONAM, MODS/DMDS material swaps, VMAD v6 structs (types 7/17; 6/16 refused).
  Every unknown part leaves the record to the winner (reported). Leveled entries are
  written sorted by level for FO4. Header: HEDR/record version copied from Fallout4.esm (1.0/131), INTV + INCC, FO4 group order,
  ESL 0x200 unless `Options::light` is off (GUI: an open FO4 save with form version < 68; CLI `--no-esl`). CLI `--fo4` (or
  Fallout4.exe in `--game`).
  Since 2026-11-06 (branch mp2) also NPC_, ARMO, WEAP, AMMO, BOOK, ALCH, INGR, SPEL, CELL (WRLD copied only as an exterior cell's
  parent, RNAM/OFST dropped as for Skyrim), every part from wbDefinitionsFO4 (dev-4.1.6), same per-field design as Skyrim:
  - NPC_: lists SNAM {faction, rank s8} (5 bytes), SPLO/SPCT, PRKR {perk, rank u8}/PRKZ, CNTO+COED/COCT, KWDA/KSIZ. ACBS 20 bytes
    (flags @0, XP offset @4, level @6, min @8, max @10, disposition @12, template flags @14, bleedout @16); linkNpcParts uses
    those offsets, template = TPLT + TPTA (13 forms). Face unit: RNAM PNAM HCLF BCLF FTST QNAM MSDK MSDV TETI TEND MRSV FMRI FMRS
    FMIN MWGT. Attack unit ATKD (44, spell @8) ATKE ATKW ATKS ATKT; actor sounds CS2H CS2K CS2D CS2E CS2F CSCR. AIDT 24 (or 20),
    DNAM 8 field by field.
  - Object templates (OBTE, then OBTF/FULL/OBTS per combination, STOP) are packed into ONE pseudo-subrecord "OBTE" in canon (its
    FULL would mix with the record's name) and unpacked in write(). OBTS: the keyword count is a u8 (xEdit's array count -4 =
    byte; checked on real data), includes 7 bytes, properties 24 bytes (value 1 a form for value types 4 and 6).
  - WEAP: DNAM 132 (forms @0, 40, 44, 73..101), FNAM 41 (@29), CRDT 12 (@8) via Kind::kFixed (other sizes: left to the winner);
    EITM+EAMT unit, 1st person MOD4 unit. ARMO: male+female models + icons one unit (MODC appears in both), INDX+MODL armatures
    one unit (not a list), DATA 12, FNAM 8. AMMO: DNAM 16 (projectile @0), NAM1+NAM2 casing unit. BOOK: DNAM 13, teaches @1 is a
    form when flags & 0x15; flags+teaches merged as one 5-byte value. ALCH ENIT 20 (@8, @16), INGR ENIT 8, SPEL SPIT 36 (@32).
    DAMA {damage type, u32} (12 bytes from record version 152: refused).
  - Conditions (CTDA 32): Fallout 4's own table `core/ConditionFunctionsFO4.h` (generated from wbDefinitionsFO4 by
    scratchpad mp2tools/gencond.py: 479 functions); reference/actor/package parameters are numbers when the type flag has 0x02
    (alias, except GetIsCurrentPackage run on a quest alias) or 0x08 (package data).
  - CELL: DATA u16 flags, XCLC, lighting {XCLL, LTMP}, water {XCLW XCWT XWEM XWCN XWCU}, owner {XOWN (12 bytes), XRNK}, XCLR
    regions list, the rest single units. Previs/precombined {VISI RVIS PCMB CNAM ZNAM XPRI XCRI} is never taken from a non-winner
    (applyUnits skips it): it belongs to the winner's previs/precombined files. XCRI: references count = 2x entries.
  Tests: TestMergedPatchFO4 (synthetic lists), TestMergedPatchFO4Types (synthetic, every new type, renumbering, alias numbers,
  previs kept), TestMergedPatchFO4Real with `RESAVERPP_FO4_PLUGINS="file;file"`: every own record forced to change in ModA (a list
  entry, OBND bit, or CELL DATA 0x10) and renamed in ModB; all merged, read back with the same subrecords. DLCRobot 1256,
  DLCCoast 18860 (17614 cells), DLCNukaWorld 6597, DLCworkshop01 77, ccBGSFO4115-X02 171 merges, none left out. INGR has no
  real records in those plugins (synthetic only). Not tested in the real game yet; Fallout4.esm itself not run through yet.
- Fallout 4 settlements (2026-11-06, branch ws, not published): `core/WorkshopFO4.*` (`ess::fo4ws`), GUI `gui/AppF4Settlements.cpp`
  (Tools > Settlements, Fallout 4 only; Save check items via `App::settlementCheckItems`, read on a thread from a copy of the save;
  test hook `RESAVERPP_TEST_SETTLEMENTS=1`), CLI `--fo4-settlements`, `--fo4-reset-budget <id|all|over>`, `--fo4-set-budget <id>:<d>:<t>`.
  A workshop = the reference carrying WorkshopScript (or a script extending it); myLocation / WorkshopID from its instance. Its
  values are actor values of the REFERENCE in extra 0xC2 (ExtraActorValueStorage, ctor 0x29c750; loader case 0x2980e3 ->
  ActorValueStorage::LoadGame 0xcedc80: vsval n x {ref, f32 base}, vsval m x {ref, 3 x f32}); GetActorValue (0x507ed0) = base (or
  the AVIF default) + base object PRPS (0x508010) + the 3 modifiers. Budget AVs are hard-coded: WorkshopCurrentTriangles 0x348,
  MaxTriangles 0x349, CurrentDraws 0x34A, MaxDraws 0x34B (indices 0x8c..0x8f at 0x6fc62b.., form id 0x2BC + index, default 0; same
  in 1.10.163 / 1.10.984, tools/fo4emu/wsvers.py). Workshop mode (0x390330) reads them as ints, a max of 0 -> uWorkshopBudgetDrawCount
  10000 / uWorkshopBudgetTriangleCount 10000000; placing adds the object's counts and writes the bases back (0x390426 -> vfunc 9 ->
  SetBaseActorValue 0x5080d0, change flag 31), scrapping subtracts, floored at 0 (0x3905e6): the game never recounts. Meter 0x37d6c0
  = max(draws/max, triangles/max) clamped to 1.0; compared with 1.0 (0xb2e994). Population 0x12723E / happiness 0x129157 sit in the
  same storage. Links: extra 0x50 ExtraLinkedRef (0x2a5910: vsval n x {keyword, target}) with WorkshopItemKeyword 0x54BA6; settlers'
  WorkshopNPCScript.workshopID indexes WorkshopParentScript.Workshops (scripts set both together). Checks (all 0 on the test saves
  except Save14's The Castle 66EB6, 1005094/1000000 triangles): over budget, links to a created workbench not in the save or a
  plugin the save lacks (null links are UnassignActor's and not reported), settlers whose workshopID names no workshop or another
  workbench than their link. Tests: rs_unit TestFo4Workshops (synthetic + every real save: reset all, written, read back, only the
  workbenches changed); tools/fo4emu wsdump.cpp + wsemu.py: the game's TESObjectREFR::LoadGame on every reset workbench of all 12
  test saves reads to the exact end with the expected values (the fo4full Sanctuary body stops in its inventory in the emulator,
  edited or not). Not tested in the real game.
- Skyrim followers / beast form / quests between stages (2026-11-06, branch skyf, not published; needs ReSaverHelper 13 for
  three commands, which Tony must build in VS2022: `helper/src/SkyFix.h/.cpp` new, `Server.cpp`, `Protocol.h`, cmake lists):
  `core/FollowersSky.*` (`ess::skyf`), GUI `gui/AppSkyFollowers.cpp` (Tools > Followers and beast form, Save check items via
  `App::skyfCheckItems`), CLI `--followers --beast-form --stuck-stages --fix-follower-count --fix-animal-count --clear-teammate`.
  Verified facts (SkyrimSE.exe 1.6.1170 decrypted locally with tp2tools/unstub.py, scratchpad skyf/): teammate = bit 26 of the
  ACHR "BOOL BITS" (Actor+0xE8, IsPlayerTeammate native 0x9F5500); SetPlayerTeammate 0x68D690 also sets bit 7 of "BOOL FLAGS"
  (+0x204) and counts PlayerCharacter+0xA10, which is not saved and is recounted in Actor::FinishLoadGame (0x687881); RemoveFromFaction
  0x68B470 writes a unique NPC's change to its NPC_ (ACTOR_BASE_FACTIONS), others to ExtraFactionChanges; NPC_RACE (flag 25) = RACE
  (npc+0x158, the actor copies it in SwitchRace 0x69ADF0) + OLDRACE (+0x1E8); ActorState byte 4 = life state. QUST QUEST_RUN_DATA
  ITEMS1 = alias fills {alias id, flag 0, ref} (matches each ref's AliasInstanceArray in Save88); QUEST_INSTANCES are text-replacement
  names (BGSQuestInstanceText), not fills. Global variables = table 1 type 3 (vsval n x {RefID, f32}). DialogueFollower 000750BA:
  stage 400 = DismissFollower(); PlayerWerewolfQuest 0002BA16 / DLC1PlayerVampireQuest (Dawnguard 0071D0): stage 100 = ShiftBack().
  Offline edits: only PlayerFollowerCount / PlayerAnimalCount and the teammate bits. Race/beast state is fixed only in game
  (SETSTAGE <quest> 100, or helper 13 BEASTREVERT: guards __tryingToShiftBack/__shiftingBack/__shuttingDown set false, then
  ShiftBack()). Helper 13 also fixes Transplant's follower count global (0BCC98; 10-12 used 0BCCAE, a perk; LE helper too).
  Tests: rs_unit TestSkyFollowers with `RESAVERPP_SKY_SAVES="dir;dir"`. SkyrimIstari's Save89 has the "you already have a follower"
  state (count 1, alias empty). No save in beast form was available: those verdicts are tested on human saves only.
- Skyrim follow-ups (2026-11-06, branch sky2, not published):
  - ReSaverHelper LE 13 (`helper-le/src/SkyFix.h/.cpp` new, `Papyrus.h/.cpp` SetBoolVariable, `Server.cpp`, `Protocol.h` = helper's
    copy, CMakeLists 13.0.0 + SkyFix, README): FOLLOWERSET, TEAMMATE, BEASTREVERT as on SE. Tony must build it in VS2022 (Win32); it was
    only syntax-checked here (clang i686-w64-mingw32 against a patched copy of the SKSE headers: scratchpad le13/chk.sh). The app needed no
    change: helperAtLeast(13) counts the LE helper's HELLO version. Addresses read in TESV.exe 1.9.32.0 from Tony's Steam folder
    (C:\Steam\steamapps\common\Skyrim, SteamStub 2.1 x86: decrypted with scratchpad le13tools/unstub21.py, a port of Steamless'
    Variant21 x86 steps; tools ledis.py, xref.py, vtbl.py, callers.py there): VM vtable 0x0114C1B0 (ctor 0xC54DE0), slot 0x1B FindBoundObject
    0xC4D5F0 (handle, const char* class -> BSFixedString inside, out ref'd), 0x21 GetVariableValue 0xC47E90 (variables at object +0x20, 8 bytes,
    valid bit 2 of +0), VMClassInfo::GetVariable 0xC33E30 (index over the parent chain, 0xC335F0 = chain count), release 0xC30720 = SKSE's
    DecrementLock, 0x25/0x27 dispatch slots (ret 0x10 / 0x18), TESQuest aliases +0x30/+0x38 and alias id +0x0C (0x56B6F0 behind
    Quest.GetAlias 0x9152C0). Form ids and alias 0 "Follower" checked in LE Skyrim.esm / Dawnguard.esm; script names, ShiftBack and the three
    Bool guards in the LE .pex (Skyrim - Misc.bsa, Dawnguard.bsa; Dawnguard's PlayerWerewolfChangeScript replaces Skyrim's). Not run in game.
  - Load order check: Skyrim SE limits checked in SkyrimSE.exe 1.6.1170 (comments in LoadOrderCheck.cpp): light = header flag 0x200 or ".esl"
    (0x1c5c4f..0x1c5cf2; "esm"/"esl" also set master), AddCompiledFile 0x1b8580 (compile index byte / FE + small index u16), GetFileForFormID
    0x1bdfe0 (FE -> (id >> 12) & 0xFFF): 254 full + 4096 light, light ids FE | index << 12 | id & 0xFFF (0x1c8d70); HEDR must be <= 1.7101
    (0x1c6220, else not loaded). The code already agreed; the 0x800 light floor stays an xEdit/CK rule (the loader takes any 12-bit id).
  - Quests between stages: shut-down stage check uses PluginDB QUST INDX flags via `skyf::ShutdownOnlyStages` (flag 4 without 2). Exe:
    INDX read into stage +2 (0x3db0e0); stop runs flag-4 stages (0x3c9af0 from 0x5330c9/0x53352f, quest flag 0x80 set first), start resets
    done bits (0x3c9bf0 -> 0x532d20 -> Reset 0x3ca170) and runs flag-2 stages (0x3c9a60); SetStage 0x3c9d00/0x3da8f0 never stops a quest.
    Quests saved with flag 0x80 are left out. Tony's Save132 had 6 mod injector quests with stage 0 flagged start-up + shut-down (false
    findings before the flag-2 rule); 12 of his saves with his MO2 mods: 0 shut-down findings now. GUI: "Stop the quest in the game" (helper 11).
  - Co-save JContainers: `cosave::jc::Analyze/AnalyzeBytes` read JSTR (boost 1.67 binary archive, library 16; layout in Cosave.h from
    ryobg/JContainers' serialize code). All 166 of Tony's JSTR chunks read to the exact end (C++ and the Python prototype scratchpad
    jctools/jstr.py). His saves only hold kJMap objects with text/number values (FSMP MCM, DBVO voice packs); arrays, form maps, integer maps,
    forms and domains were checked only on a synthetic archive written with boost 1.83 (jctools/gen/gen.cpp; 1.83 numbers classes one
    higher at the first polymorphic pointer, so the Python reader has a test-only "tolerant" mode for it). Read only. jc::Header was never
    found before (the JSON comes after its length as decimal text): fixed.
  - Skyrim VR co-saves: SKSE VR 2.0.12's Serialization.cpp (ryobg/JContainers dep/sksevr/sksevr.7z) has the same file layout as SKSE64
    (only the saves folder and the load-order index resolution differ).
  - tools/i18n/tr_keys.py: an entry's "# File.cpp:N" comments are no longer kept in the header (each --update added a copy of the first
    entry's comment); lang/ru.txt cleaned (72 lines). A full --update now would also refresh ~1100 line-number comments and move 8 unused
    keys (no translation changes; checked): not run in this branch, the 11 new entries were added by hand above the unused section.
- Versions are whole numbers, +1 per release. **Current published version: v57** (SE, LE and Fallout 4 pages, 2026-11-05; v57 adds the Russian interface, the Leftover items plugin-read fix and Fallout 4 Reset Havok / Purify / Merged patch / AI / NPC+player leftovers; ReSaverHelper F4 13; the Fallout 4 main file is packed as Data/Tools/ReSaverPP/ with mod manager download on: nexus-upload layout=data-tools, mod_manager=true) (v53 was never published on its own). v55: Collect all logs gathers Fallout 4 (F4SE folder, Logs, Fallout4*.ini, AppData Fallout4 plugins.txt, Fallout4.exe version; zip "Fallout 4 logs <date>.zip").
- Fallout 4 Reset Havok + Purify FormLists (2026-11-05, not published): REFR change flag 2 = initial data + havok blob, read
  by the initial data loader 0xbf77b0 (not LoadGame) through 0xc10b00 blob kind 2: u8 (form version the blob was written
  with), vsval size, bytes; size 0 stores nothing (the game writes size 0 itself, e.g. 112 of 3889 in MainQuestRailroad).
  The bytes are parsed by 0x4f7440 when the 3D loads (else kept in extra data 0x28c060), so Reset Havok for FO4 sets the
  size to 0 and keeps the version byte (`fo4cf::ClearHavok`), not Skyrim's zeroing. FLST = type 42 in FO4;
  BGSListForm::LoadGame 0x5898f0: bit 0 u32+u16, bit 31 u32 count + refs (null refs are kept as entries by the game);
  `fo4cf::DecodeFormList` / `PurifyFormList`. Checked with tools/fo4emu/havemu.py and flstemu.py; rs_unit
  TestFo4HavokFormLists (real saves with RESAVERPP_FO4_SAVES).
- Fallout 4 NPC change forms (2026-11-05, branch npc, not published): `core/ChangeFormFO4Pkg.cpp` decodes the AI process's
  package data (0xcea430: type byte -> 2 form id, 13, 14, 16 nothing, 18 procedure tree; the tree's states and data objects
  are saved by class name and loaded through the procedure factory 0x7106f0 (state LoadGame vtable +0x38, data object +0x40
  of the IPackageData base, which is the base at +8 for ObjectList/Ref/RefOLD/Location/LocationWrapper) or the location
  factory 0x21c2b0 (+0xc8: LocRef, LocPt, Prim, LocCell)); a created package's own form (0x757db0, TESPackage::LoadGame
  0x75e5a0); the movement controller's Havok pathing requests (0xfe74a0 -> +0x88, common part 0xfe7680). `fo4cf::Actor::complete`
  = an NPC whose Actor::LoadGame ends at the body's end; Leftover items (RemoveInventoryItemsFO4) edits those (actor part kept
  byte for byte, decoded again). Checked read for read against the emulator (tools/fo4emu: actcmp, stack1.py, fails.cpp).
  Branch npc2 (2026-11-05): every ACHR of every test save reads to the end of its body (`core/ChangeFormFO4Combat.cpp`):
  CombatController::LoadGame 0x87c540 (Actor::LoadGame's byte at 0xc8b31c; `Actor::hasCombat`/`combatRange`) and the
  combat objects behind the object pointer reader 0xed5cf0/0xed5bd0/0xed5e10 (u64 id: 0 none, even = read before, odd =
  u32 class id + that class's LoadGame; 19 Combat* classes by RTTI, e.g. CombatAreaHoldPosition, CombatCoverLocation;
  an odd id seen again loads with its first class). Registry 0x3271390 (0x89cb10 values: u32 hash, u16, u8, fixed size by
  the FIRST entry with that hash, entry 0 when none) is filled by the game's own 0x89ce90, not a CRT initializer: the
  emulator now runs it. Named values (0xe75e30) and the tree state (0xeff4d0 -> 0xf04bf0) are sized blocks (writers
  0xe76300 / 0xf04b90 patch in the bytes written). Extra data 0x32 (form id + active effects), 0x34, 0x44, 0x87 are
  lists (were fixed sizes, right only when empty: the Save8-14 "reference part misread" and Save14's "extra 0x3d"), 0x9d
  a form id, 0x79 two u32, 0xa9 u32 type + two form ids + u8 (Mount/PassengerInteraction LoadGame 0xdbfe20 reads nothing). Package data class names are the game's registrations (factory 0x32174c0, 0x705b60 calls): SingleRef
  (0x709870), Ref (RefOLD class, nothing), LocWrapper, Location/LocationSelector, Topic/TargetSelector (nothing); the old
  names Ref/RefOLD/LocationWrapper were never written by the game. A created package's mask bit 4 = its data object
  (Follow u32+u8+location, UseItem u8+location, Patrol u8 u8, Custom nothing). Checked: tools/fo4emu trcmp.py (cmpall.sh)
  read for read on all actors of all saves, refrcheck.sh (reference part vs the game's TESObjectREFR::LoadGame),
  npcremove.cpp + emucheck.py round trips (rtcheck.sh); rs_unit TestFo4Combat (491 passed with RESAVERPP_FO4_SAVES).
  Emulator limits (not decoder issues): dec.py cannot read ~25 reference parts per big save (trcmp/emucheck then take the
  decoder's / the game's own end), and ~0.3% of actors stop inside the emulator (a pointer-id hash map 0x1e0d0f0 that
  never finishes growing, a few unmapped reads); for those the reads up to the stop are compared.
- Fallout 4 player (2026-11-05, branch pc, not published): `core/ChangeFormFO4Player.cpp` (`fo4cf::detail::PlayerPart`) reads
  PlayerCharacter::LoadGame (0xd65bd0) after Actor::LoadGame to its end (0xd68175): fields, the Pip-Boy quest log (0xd6682a) and
  objectives (0xd66961, 0x61e170), PlayerCamera::LoadGame (0x102aac0, 13 camera states from the constructor 0x1024f80), knowledge
  (0xd2faa0), the +0xd80 reference's inventory (0x4f6bb0 -> 0x34ee00, empty in every save seen), lists and maps, every form
  version branch. No read depends on a form lookup (checked at each). `Actor::complete` is true for the player on all 18 test
  saves (fo4full 8/9/10/12/14, fo4in x3, fo4ng x2, fo4t x6, Tony's Save7/Save8; read for read equal to the emulator); `Actor`
  exposes `playerParts`, `questLog`/`objectives` (+ ranges), `cameraRange`, `extraInventory`. Quests takes the Pip-Boy lists
  from it (shape search as fallback). Leftover items (RemoveInventoryItemsFO4) now edits the player: removal + caps/perk/SPECIAL/
  XP/level edits written, read back complete, and run through the game's loaders in the emulator (tools/fo4emu playerremove.cpp,
  emucheck.py, invcheck.py). The emulator's player object was 0x400 bytes (PlayerCharacter is 0xe08): fixed in harness.py. Extra 0x6c (package data), 0x1b, 0xd3, 0xd5 and 0x5c's per-entry lists are decoded. Coverage: every NPC
  but 1-2% of the big saves (Save10: 11040 of 11073 non-player ACHRs read exactly; 539 -> 1891 of 1892 in nate_cleansave).
- Fallout 4 active effect removal (2026-11-06, branch fx, not published): `fo4cf::RemoveActiveEffects(save, actorCF, indexes, sources)`,
  `EffectsFrom` (one magic item) / `EffectsFromPlugin`, `PlanEffectRemoval`, `EffectSources(save, actor, PluginDB*)`; NPCs and the
  player (Actor::complete required). The list: AIProcess middle-high data, 0xb896c0: vsval count, entries read by 0xb753d0 (ref item,
  u8 effect index, vsval archetype = MGEF DATA +0x40, checked on 1861 effects, vsval size, data skipped by size whatever happens);
  `Actor::effectList`. Nothing else in the body refers to an entry (casters 0xb85b10/0xb79840 save forms; the unique id +0x8c is
  picked again by 0xb76730 at Start). Data starts f32 elapsed, magnitude, duration, u32 flags (0xb74e90). Flags (0xb75b10 Update):
  0x10000 started, 0x20000 finished (also when it turns inactive 0x8000), 0x200 magnitude as saved. Undo = the game's Finish:
  archetype 0/34 (Value/PeakValueModifierEffect, the same vfuncs) with an MGEF Recover flag (0x2) take the saved magnitude off the
  modifier slot Start used (0xba0400: target is the player (singleton 0x31ede50) and the item is an ability (SPIT type 4), a constant
  effect enchantment (ENIT cast 0, type 6) or a non-poison potion on Health / S.P.E.C.I.A.L. (AV type 1, set only by 0x6fd1e0) ->
  slot 0 through ActorValueOwner +0x30; else slot 1); |result| <= 0.0001 -> 0 (0xcc9b70), one effect at a time. Health,
  ActionPoints, UnusedStamina, Rads keep their modifiers in Actor +0x444.. (0xccbe80), saved as the four "u32 n + min(n,3) f32" lists
  before the actor values (0xced2e0, `Actor::vitals`). Archetypes whose Finish does nothing (1 script, 2, 3, 9, 10, 15, 16, 19, 27-29,
  33, 35, and 17/26/30/36 which have no class: a plain ActiveEffect) are removed as they are; other started ones are refused, as are
  started value modifiers whose plugin record is unknown (GUI: load plugin names) or whose strength tapers (no 0x200). Archetype ->
  class from the registrations (0xb89a80 calls: archetype, creator). Checked: real plugins (tools/fo4emu fxdb.cpp: removing every
  running effect of Save10's player puts all 21 of its modifiers back to exactly 0), every actor of 12 saves written and read back
  (fxremove.cpp: effects that stayed byte for byte, the rest of the body unchanged, other change forms unchanged), the game's
  Actor/PlayerCharacter::LoadGame in the emulator on every edited actor (fxcheck.py; where the emulator stops inside an actor, it stops
  at the same place in the original). PluginDB reads MGEF/SPEL/ENCH/ALCH for Fallout 4 too (`Form::castType`, `poison`).
  GUI: Edit player > Active effects: state, "When removed", Remove, "All N from this source" (test hook RESAVERPP_TEST_FX). AI:
  fo4_player numbers the effects (#n) and says what removing each does; fo4_edit_player `remove_effects` / `remove_effects_from`.
  rs_unit TestFo4ActiveEffects. Left alone: Papyrus ActiveMagicEffect script instances of a removed effect (what the game does with
  them on load was not checked), the OnEffectFinish event (not sent), what a script or perk re-applies later, and Finish's clamp that
  keeps a stat from going below 0 (needs the current value). No CLI option yet.
- v51 (published 2026-11-02): Edit player > Active magic effects (list, tick, Remove; CLI `--effects`, `--remove-effect 3,7`).
  Save layout: high process ACTIVE EFFECTS = vsval count, each {REF item, u8 effect index, vsval type (= MGEF archetype),
  vsval size, DATA: f32 elapsed, f32 magnitude, f32 duration, u32 flags, ...}. Checked on Tony's Save112: the player's
  modifiers are the sums of the magnitudes of type 0/34 effects without flag 0x8000 (switched off), with 0x200 (recovers);
  flag 0x80 (conditions) -> temporary modifier, else permanent. Health/Magicka/Stamina/Voice keep their modifiers in
  their own lists (Decoded::vitalModsAt), not in the general AV modifier list. Removal takes the magnitude off that
  modifier; the AV comes from the MGEF (PluginDB::magicEffect, DATA 0x44), so stat-changing effects need plugin names.
  `project(ReSaverPP VERSION 50` in CMakeLists.txt.
- Interface translation (2026-11-05, not published): `src/core/Lang.*` (`tr()`, `trId()` for window/popup/tab/header
  ids, `trc("ctx", ...)` variants, `N_()` for static tables), catalogues `lang/<code>.txt` built into the exe by
  `cmake/EmbedLang.cmake`; a `lang/<code>.txt` next to the exe overrides (Options > Settings > Reload language files).
  `language=` in resaverpp.ini, applied at once. Russian `lang/ru.txt` (stalkerVlad1 proofreads). New GUI texts: wrap
  them in `tr()`, then `python3 tools/i18n/tr_keys.py --update lang/ru.txt` (CTest i18n_keys fails otherwise; rs_lang_test
  checks % conversions). Not translated: core texts (save check findings from ops, change form labels, error texts
  from core), AI prompt/tool texts, logs, CLI, reports/system info, in-game notifications. See TRANSLATING.md.
- Save bloat by mod + load order check (2026-11-06, branch bloat, not published): `core/SaveBloat.*` (`ess::bloat::Build`: per plugin
  the Papyrus instances/threads/arrays/structs with their bytes as written, change forms by type, created forms by their base's plugin
  (FO4 DecodeRefr initial data, Skyrim baseObject / leveled template), inventory entries by item plugin, taken out of the holder's
  change form; rows + shared Papyrus data = Papyrus block + change forms exactly; `Compare` = growth per row), `core/LoadOrderCheck.*`
  (`ess::locheck::Check`). GUI `gui/AppF4Bloat.cpp` (Sc::kBloat 34 / kLoadOrderCheck 35, home buttons, Tools menu, Save check:
  load order errors, a mod gone from the load order that still takes >256 KB). The open save's report is read again from its file on
  a thread while unedited; the older save comes from the save history (same character, same folder), test hook
  `RESAVERPP_TEST_BLOAT_OLD`. CLI `--bloat [--bloat-compare <old>]`, `--load-order-check`. Test `rs_bloat_test` (ctest bloat;
  RESAVERPP_FO4_SAVES / RESAVERPP_SKYRIM_SAVES / RESAVERPP_FO4_DATA). Fallout 4 limits checked in Fallout4.exe 1.11.240 (same code in
  1.10.163/984): light = header flag 0x200 or .esl (file open 0x2f6217..0x2f62ba; .esm/.esl also set master), IsLight 0x2f91a0 = flag
  only; data handler 0x2e6dc0: light list -> load index 0xFE + light index, full list -> load index = position; form id 0x2f91c0 =
  FE000000 | light << 12 | (id & 0xFFF): 254 full (00..FD) + 4096 light; HEDR > 1.0 not loaded (0x2f67e0, 0x2f7360 clear active bits).
  Light object id floor (0x800, or 0x001 with HEDR >= 1.0 and a master) from xEdit dev-4.1.6 wbDefinitionsFO4 (HardcodedRangeMinVersion);
  Skyrim SE limits checked in SkyrimSE.exe 1.6.1170 since branch sky2 (the light id floor stays xEdit's). F4SE whatsnew: light plugin support came with 0.5.0
  (after 1.9.4, i.e. 1.10.20), not 1.10.162.
- Fallout 4 companions + save checks (2026-11-06, branch comp, not published): `core/CompanionsFO4.*` (`ess::fo4comp`): companions
  = actors with a CompanionActorScript (or derived) / DogmeatActorScript instance; FollowersScript's own variables give the follower
  quest, CurrentCompanionFaction 00023C01, HasBeen 000A1B85, Disallowed 00075D56, CA_Affinity 000A1B80, CA_CurrentThreshold, CA_IsRomantic
  00148DF6 ...; CommonPropertiesScript.FollowerState = AV 00000344 (constant globals 1 follow / 2 wait / 4 go home). An alias object's
  instance `unknown` field = its ALST id (Companion 0, DogmeatCompanion 5, ActiveCompanions 6: checked against Fallout4.esm). Alias fills:
  QUST change flag 28 run data ITEMS1 {u32 alias id, u8 created, ref} (TESQuest::LoadGame 0x66cd80..0x66d1c5, the ref's handle stored as
  that alias's fill); the actor's side: extra 0x88 ExtraAliasInstanceArray (0x2ab7c0: vsval n x {TESQuest ref, u32 alias id}); extra 0x5b
  ExtraFactionChanges (0x295ce9: vsval n x {TESFaction ref, i8 rank}, ref, u8). CurrentCompanionFaction comes from the aliases' ALFC, not
  saved. Findings: slot holder without companion script, slot vs actor alias list disagree, two claimants, left for good
  (HasLeftPlayerPermanently), DisallowedCompanionFaction, waiting/sent home, missing from ActiveCompanions, affinity outside
  Min/MaxAffinity. Offline edit only SetAffinity (CA_Affinity base in place + Highest/LowestReached like ModAffinity; edited Codsworth
  of Save8 read to the exact end by the game's Actor::LoadGame in tools/fo4emu). Being a companion again is the scripts' work
  (SetPlayerTeammate, keywords, registrations, alias), so it is console text: `cqf Followers "FollowersScript.AllowCompanion" <ref> 1 1`,
  `<ref>.cf "CompanionActorScript.UnsetHasLeftPlayerPermanently"`, `<ref>.cf "<script>.SetAffinity" <v>`, `cqf Followers
  "FollowersScript.FollowerFollow" <ref>` (cf 0x028b / cqf 0x028c in the 1.10.163 / 984 / 1.11.240 tables; argument conversion 0x602590 /
  0x60aa80: a form is hex id, "player", "selected" or "none"; a bool is false for "0", "false", ""). The helper has no generic console
  command, so the app copies the commands. `core/QuestChecksFO4.*`: Fallout 4 .pex reader (`ess::pexfo4`) and the stopped-stage check
  (done stage whose fragment's straight-line start sets a stage / shows / completes an objective that the save lacks, only when no other
  function of the script touches it, the plugin defines it, and the script is not running or queued); 0 findings on 10 test saves. GUI
  `gui/AppF4Companions.cpp` (Tools > Companions, Save check findings via `appendCheckItemsF4`), test hook `RESAVERPP_TEST_COMPANIONS=<ref>`.
  CLI `--fo4-companions [--sources]`, `--fo4-set-affinity <ref>:<v> -o`, `--fo4-quest-check --sources`. Tests: `rs_fo4comp_test` (ctest
  fo4comp; RESAVERPP_FO4_SAVES, RESAVERPP_FO4_DATA). Research scripts (not in the repo): scratchpad comptools/pexdis.py (FO4 .pex
  disassembler), fragchk.py (the check's Python prototype), esmrec.py (one record's subrecords). Not tested in the real game.
- Fallout 4 world items + reset an area (2026-11-06, branch world, not published): `core/WorldItemsFO4.*` (`ess::fo4world`) lists the
  created references (REFR change forms with FF ids; initial type 5 = cell/worldspace, position, rotation, u8, base) lying in the
  world (extra 0x1C ExtraReferenceHandles = held in an inventory: not listed), by place (interior cell, or worldspace + pos/4096)
  and by the base object's mod, kind from the base's record type, count from ExtraCount 0x24 (i16). Removal = deleting the change
  form: Save7 -> Save8 (9 dropped Pipe Pistols) adds only the 3 new refs' change forms (FF001199 x3, FF0011BB x4, FF0013CF) and
  nothing anywhere else (globals, other change forms, form id array, Papyrus); FF000D87 was in the player's inventory in Save7
  and is a quest alias (QUST 0006DFAF alias 17, extras 0x88/0x8C) -> kept. Kept as well (the game's own cell reset 0x4d2588
  keeps the same: 0x4fcc70/0x4fcc90 extra 0x1C, 0x4feb70/0x4fcd60 extra 0x88): scripted (Papyrus instance on the RefID), named
  in any other change form / global block / form id array (byte search of the 3-byte RefID and the u32 id), not read completely.
  Save8 minus the 3 = Save7's change form set and form id array. `core/CellResetFO4.*` (`ess::fo4cell`): reset = what console
  ResetInterior (0x1127, 0x5d11d0 -> 0x4c9330(cell, 0xFFFFFFFF, 0)) writes: CELL change flag 30 with detach time 0xFFFFFFFF
  (initial data per 0xbf6c40/0xbf7090: bit 30+29 {u16 worldspace index (1-based, visited worldspaces), i8 x, i8 y, u32}, 30+28
  {u16, i16, i16, u32}, 30 alone {u32}); then TESObjectCELL::LoadGame 0x4d7b30: bit 0 u32+u16, bit 1 u16, bit 31 seen data
  (interior vsval n x 34, exterior 32), bit 2 u16 string, bit 3 RefID. The game resets on the next attach (not during a load:
  save manager flag 2 at +0x360). New interior CELL change form = flags 0x40000000 + 4 bytes (the game writes such forms itself,
  e.g. Save14 0014FC9B). Refused: the player's cell, exterior cells within 2 of the player's, exterior cells without coordinates,
  cells the save does not say are interiors. CLI `--fo4-world-items`, `--fo4-remove-world-items <ids|all>`, `--fo4-cells`,
  `--fo4-reset-cell <id>` (src/cli/WorldF4.cpp); GUI Tools > World items and areas (`gui/AppF4World.cpp`, test hook
  RESAVERPP_TEST_CAT=0 RESAVERPP_TEST_OPEN=f4world|f4cells); Save check finding at 500+ removable items. Checks: rs_unit
  TestFo4WorldItems; tools/fo4emu/cellemu.py (every CELL of Save8/Save14/MainQuest...FarHarbor/nate read exactly by the game's code;
  the edited cells give detach 0xFFFFFFFF). Not tested in the real game.
- Companions through the helper (2026-11-06, branch comp2, not published; needs helper-f4 version 15, which Tony must build in
  VS2022: `helper-f4/src/Transplant.h/.cpp`, `Protocol.h` (kHelperVersion 15), `Rescue.cpp`, README changed): APPLY records
  `companion <ref> <0|1> [script]`, `companionfollow <ref>`, `affinity <ref> <n> [script]` -> `<ref>.cf "<script>.UnsetHasLeftPlayerPermanently"`
  + `cqf 000289E4 "FollowersScript.AllowCompanion" <ref> 1 1`, `cqf 000289E4 "FollowersScript.FollowerFollow" <ref>`,
  `<ref>.cf "<script>.SetAffinity" <n>`. The follower quest goes by form id everywhere now (Gather too): the console's form
  argument reader (1.10.163 0x4e7620, 1.10.984 0x561ae0 = 1.11.240 0x5b5d90) takes "player", an editor id the game kept
  (0x311d60), else `_strtoui64(text, 16)`; whether quests keep editor ids was not settled. cf/cqf -> 0x60b2c0 -> 0x60c5c0
  (strchr '.': Script.Function) and 0x602b70 (arguments by type: atof / atoi / bool / forms via 0x60aa80); 984 the same
  instruction for instruction, 163 the same order. App: Companions window "Run in the game" buttons and a Save check option
  (`App::f4compSend`, `f4compCannot`; `fo4comp::CompanionRecord / FollowRecord / AffinityRecord`), copy buttons kept.
  `App::helperAtLeast` for the Fallout 4 helper now compares versions from 12 up (it answered true only for 12, so Transplant's
  13/14 checks were never met).
- Linux/Wine is not officially supported.

- Co-save viewer/cleaner (2026-11-06, branch cosave, not published): `core/Cosave.*` reads/writes .skse/.f4se byte-exact (format
  identical in skse/Serialization.cpp LE 1.7.x, skse64/Serialization.cpp, f4se/Serialization.cpp: header sig/format 1/xse ver/runtime/
  numPlugins; plugin {uid, numChunks, length}; chunk {type, version, length}; the loader ignores numPlugins and reads while 12 bytes
  remain). `KnownTable()`: uid -> plugin with the source file of each SetUniqueID call (or "DLL:" for ids read from a shipped DLL).
  Installed or not: `FindIds` searches the mods' SKSE/F4SE\Plugins DLLs for the id as a SetUniqueID immediate (x64 BA / B9 / 41 B8,
  x86 68 push); nothing found at all = wrong folder (`View::plausible`). PapyrusUtil (uid 884715692+227106806, text records
  INTV..FORL, PLGN load order) broken down by plugin/key/object; objects on forms of gone plugins removed by byte ranges (count
  adjusted). JContainers ('JSTR', boost archive) broken down since branch sky2 (see the Skyrim follow-ups above). GUI `gui/AppCosave.cpp` (Tools > SKSE/F4SE co-save, home button,
  Sc::kCosave, Save check line), edits `_save->cosave` + markEdited, written with Save As. CLI `--cosave` (CosaveCli.cpp). Tests
  `rs_cosave_test` (RESAVERPP_COSAVES="dir;dir" for real files). Tony's 167 .skse co-saves: all byte-identical. No .f4se tested
  (none on Tony's PC). VR: same layout as SKSE64 (SKSE VR 2.0.12 source, see the Skyrim follow-ups above).

## 2. Where things are

- GitHub: `Tonycubed2/ResaverPP` (public), branch `main`. Other threads also push here, so ALWAYS
  `git fetch` and check `git log HEAD..origin/main` before committing; rebase if it has commits.
- Tony's PC (through the linked-computer tools, `device_bash`):
  - `$HOME/mnt/dllmaker` = `C:\dllmaker`. Project copy: `C:\dllmaker\ReSaverPP`.
    Builds Tony runs go to `C:\dllmaker\ReSaverPP\prebuilt\ReSaverPP.exe` and `resaverpp-cli.exe`.
  - Static Linux CLI for testing on the device: `C:\dllmaker\ReSaverPP\mergetest\resaverpp-cli-linux`
    (on the device: `cp` it to `$HOME/rcli2`, `chmod +x`, run it there).
  - MO2 mods: `$HOME/mnt/SkyrimSEMOModfolder` (F:\SkyrimSEMOModfolder). Profile: `$HOME/mnt/MO2/profiles/vanilla`.
  - Game: `$HOME/mnt/common--Skyrim Special Edition` (C:\Steam\steamapps\common\Skyrim Special Edition), 1.6.1170.
  - CommonLibSSE-NG: `C:\dllmaker\CommonLibSSE-NG`.
- The cloud workspace of a new thread starts EMPTY: clone the repo again (section 3).

## 3. Setting up a fresh workspace

```
git clone https://github.com/Tonycubed2/ResaverPP.git gitrepo
```
Work in the clone directly (the old thread used a separate work copy plus a clone; one is enough).
Third-party sources (glfw, imgui, ImGuiColorTextEdit, lz4, miniz) are in `third_party/`.

Linux build (GUI + tests; needs X11/GL dev packages for glfw: `apt-get install -y libx11-dev libxrandr-dev
libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev` if cmake complains):
```
cmake -S gitrepo -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/rs_unit          # unit tests, 95 passed at v39
```
Windows build (cross, MinGW-w64 posix threads; `apt-get install -y mingw-w64` if missing):
```
cmake -S gitrepo -B build-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=gitrepo/cmake/mingw-w64.cmake -DRESAVERPP_TESTS=OFF
cmake --build build-win -j8
```
Sources are globbed: run `cmake .` in the build dir again after adding a file.
Static Linux CLI:
```
cmake -S gitrepo -B build-static -DCMAKE_BUILD_TYPE=Release -DRESAVERPP_GUI=OFF -DRESAVERPP_TESTS=OFF -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build build-static -j8 --target resaverpp-cli
```
GUI screenshots in the container: run under Xvfb; `src/gui/main.cpp` has test hooks via environment variables
(`RESAVERPP_TEST_CAT`, `_FILTER`, `_OPEN`, `_COMPILE`, `_ROWVIEW`, `_ARCHIVE`, `_EDITPROP`, `_SCRIPTPROP`,
`_SELECTPROP`, `_HOMETAB`, ...). xdotool drags need slow stepped moves.
Test saves: ask Tony to attach some, or stage them from his PC.

## 4. Commits

```
git -c user.name=Tonycubed2 -c user.email=16966940+Tonycubed2@users.noreply.github.com commit -m "..."
```
Message ends with the Co-Authored-By / Claude-Session lines the system gives you. Push to `main`.

## 5. Delivering a build to Tony (every time)

1. Copy the exes to a NEW folder `/mnt/user-data/outputs/<new name>/` (never reuse a folder).
2. `device_commit_files` with `force: true` to `C:\dllmaker\ReSaverPP\prebuilt\ReSaverPP.exe` and
   `C:\dllmaker\ReSaverPP\prebuilt\resaverpp-cli.exe`.
3. Verify with `md5sum` through `device_bash` in `$HOME/mnt/dllmaker/ReSaverPP/prebuilt`; if it differs, commit again.
4. If the exe is open/locked: tell Tony to close ReSaverPP, then save it properly. NEVER save it under another
   name (he was angry when ReSaverPP-new.exe was made; that leftover file can be deleted).
- Never copy builds into a game or MO2 `SKSE\Plugins` folder.

## 6. Publishing (ONLY when Tony says publish)

1. Bump `project(ReSaverPP VERSION N` in CMakeLists.txt, commit "vN", push.
2. Write `dispN.json` (SE) and `dispNle.json` (LE):
```
{"ref":"main","inputs":{"version":"N","changelog":"line one | line two","description":"vN: one line, max 250 chars"}}
```
   LE adds `"file_id":"8246049","mod_id":"472446525117","display_name":"ReSaverPP LE"` inside `inputs`.
3. Dispatch each (Content-Type header is required, else 415):
```
curl -sS -X POST https://api.github.com/repos/Tonycubed2/ResaverPP/actions/workflows/nexus-upload.yml/dispatches -H "Content-Type: application/json" -d @dispN.json
```
   Dispatch LE after SE's run finishes. The workflow builds on GitHub with MinGW and uploads (this repo's
   workflow already existed; do not create GitHub build workflows for Tony's other mods).
4. Check SE: the script below (`pubwait.sh`) waits for the upload run, then runs `nexus-check.yml` and prints
   the "mod version" annotation.
```
#!/bin/bash
A=https://api.github.com/repos/Tonycubed2/ResaverPP/actions
until [ "$(curl -sS "$A/workflows/nexus-upload.yml/runs?per_page=1" | python3 -c "import json,sys;r=json.load(sys.stdin)['workflow_runs'][0];print(r['status'])")" = completed ]; do sleep 15; done
curl -sS "$A/workflows/nexus-upload.yml/runs?per_page=1" | python3 -c "import json,sys;r=json.load(sys.stdin)['workflow_runs'][0];print('upload',r['conclusion'])"
curl -sS -o /dev/null -X POST $A/workflows/nexus-check.yml/dispatches -H "Content-Type: application/json" -d '{"ref":"main"}'
until [ "$(curl -sS "$A/workflows/nexus-check.yml/runs?per_page=1" | python3 -c "import json,sys,datetime;r=json.load(sys.stdin)['workflow_runs'][0];print(r['status'] if r['event']=='workflow_dispatch' and r['created_at']>'$(date -u -d '-1 min' +%Y-%m-%dT%H:%M:%SZ)' else 'old')")" = completed ]; do sleep 10; done
id=$(curl -sS "$A/workflows/nexus-check.yml/runs?per_page=1" | python3 -c "import json,sys;print(json.load(sys.stdin)['workflow_runs'][0]['id'])")
for c in $(curl -sS $A/runs/$id/jobs | python3 -c "import json,sys;[print(j['id']) for j in json.load(sys.stdin)['jobs']]"); do curl -sS https://api.github.com/repos/Tonycubed2/ResaverPP/check-runs/$c/annotations | python3 -c "import json,sys;[print(a['message'][:160]) for a in json.load(sys.stdin)]" | grep -E "latest uploads|mod version"; done
```
5. Check LE: dispatch `nexus-ids.yml` with `{"ref":"main","inputs":{"domain":"skyrim","mod":"122557"}}` and read
   its annotations; the "MOD uid ... version N" line is enough (the file list gets cut off).
6. Changelogs (v43, 2026-10-31): the API entry IS stored (the upload log shows HTTP 201), but the SE page's Logs tab can
   take a while to show it; LE showed 42/43 at once, SE only after two more page saves. Check before adding anything:
   the file's own changelog is in Edit mod > Files > the file's "..." menu > "Edit changelog" (it says "Add changelog"
   when none is attached yet); adding one there while the API entry is pending shows the text twice. Older note:
   changelogs sent through the API only show on Nexus after an edit on the website (agreed: a throwaway edit
   afterwards; `nexus-touch.yml` / `nexus-changelog.yml` exist for this). Website steps go through Tony's
   desktop Chrome, not the app's built-in browser. Never handle Tony's Nexus API key; it is a GitHub secret
   in the `nexus` environment.

## 7. How Tony wants to work

- Complete replacement files and full command sequences; each command in its own copyable block, never chained.
- Replies he will post (Nexus comments) go in a code box, written plainly, not AI-sounding.
- Announcements/changelogs in Nexus BBCode when asked, under 5000 characters for announcements.
- He types fast with many typos; read for intent. When he says it does not work like the Creation Kit, he means
  copy the CK's behaviour closely, not invent a new system.
- Do not ask permission for routine build/deliver steps; do ask before publishing (he says "publish").

## 8. Code map (src/)

- `core/Save.*`, `core/Papyrus.*`, `core/ChangeFormData.*`: save reading/writing. Round trip must be byte-identical.
  Note: an active script thread whose first frame is native (fnFlags & 2) has no trailing unknown5 byte
  (fixed in v35; the reader looks ahead and seeks back).
- `core/PluginDB.*` (plugin records, names, `ofType`), `core/LoadOrder.*` (plugins.txt: SE uses `*` marks;
  LE and MO2-for-LE have no marks and every listed line is enabled; LE file is `%LOCALAPPDATA%\Skyrim\plugins.txt`;
  `GamePluginsTxt(vr, le)`).
- `core/MergedPatch.*` (Smash-style merged patch, categories chosen in a menu; LE support since v37).
- `core/SaveArchive.*` (v36): keeps the last 10 saves opened/edited in `<settings dir>/ReSaverPP Archive`;
  File > Restore an archived save (`gui/AppArchive.cpp`) with thumbnails, browse, restore in place or as copy.
- `core/PropertyEdit.*` (`ess::props`): fill a script's object property in the SAVE (`PointAt`, `RefFor`,
  `RecordTypeFor`, `ParseFormID`). New instances need id = max(all ids)+1, `instanceDataOrder` push, `reindex()`;
  set the variable BEFORE pushing into `p.instances` (pointer invalidation).
- `core/PluginEdit.*` (`ess::plugedit`): set the same property in the PLUGIN's VMAD, optional, asked first;
  refuses official files/cc*; backup `<plugin>.<YYYYmmdd-HHMMSS>[-n].resaverpp.bak`, temp file + rename + read back.
- `core/PapyrusCompiler.*`, `core/Pex*`, `core/ScriptSources.*`: compiling/decompiling scripts.
- `core/CrashLog.*`: crash log reading (another thread added finding spawned NPCs named in a crash).
- `gui/AppHome.cpp`: home screen tabs "Your save" and "Your mods" (Merged patch, File conflicts, Papyrus log,
  Find a form, Open script). Major buttons have a still coloured border (no animation; Tony asked).
- `gui/AppScripts.cpp`: script window; compile results open in their own resizable window (`###compileresults`),
  all sections open, boxes resizable; full log `PapyrusCompileLog.txt` next to the exe. Clicking a property in
  the outline opens the picker.
- `gui/AppProps.cpp`: Creation-Kit-like property picker: only forms of the property's type, names (editor id
  dimmed), type to filter, double-click applies on release, OK / Clear. Works on the object the script is bound
  to (`_scriptBinding`); from the Scripts page it applies to all objects with that script. Then optionally
  "Also set it in the plugin?".
- `gui/main.cpp`: window, dark title bar matching the theme (DwmSetWindowAttribute 20/19, 35/36/34), test hooks.
- `cli/main.cpp`: `resaverpp-cli` (incl. `--set-property`, `--plugin-property`).
- `tests/unit_test.cpp` (`rs_unit`): add a test for each fix.

7. **Fallout 4 page** (set up 2026-11-05): domain `fallout4`, mod 110975, Unique Mod ID 4943507468671, game_id 1151.
   Files: main "ReSaverPP" File ID 8317824 (Short ID seiQXq0kr), optional "ReSaverHelper" File ID 8317827 (both first
   uploaded by hand by Tony as version 1). The page went public on 2026-11-05 (v54 the first version through the API); before it was public v1 `files.json` listed nothing
   then, so `nexus-ids.yml` also prints the v3 list (`/v3/mods/<uid>/files`), which does.
   - Main file: the same `nexus-upload.yml`, `dispNf4.json`:
```
{"ref":"main","inputs":{"version":"N","changelog":"line one | line two","description":"vN: one line","file_id":"8317824","mod_id":"4943507468671","display_name":"ReSaverPP"}}
```
   - Helper: `nexus-helper-f4.yml` (zip `helper-f4/release/ReSaverHelper.zip`, Tony's build; file_id defaults to
     8317827; changelog entry `ReSaverHelper-F4-<version>`):
```
{"ref":"main","inputs":{"version":"12","changelog":"line one | line two"}}
```
   - Both take `"dry_run":"true"`: everything is checked (the file is on that page, the zip, the build) and nothing
     is posted or uploaded. Check afterwards with `nexus-ids.yml` `{"domain":"fallout4","mod":"110975"}`.
   - `nexus-update-file.yml` now has a `mod_id` input (default SE 7318624468677) for its changelog.

## 9. Release history (recent)

- v34 merged patch announcement; v35 native-frame thread fix (etgmotley's save); v36 save archive + title bar;
  v37 LE merged patch (other thread); v38 property editing in save/plugin + compile results window + full
  compile log (requested by tyg3rtyg3r); v39 LE plugins.txt without `*` marks (tiodrakul's report).

## 10. Open items

- Released in v48 (2026-10-31): `--bsa-list/--bsa-extract/--bsa-find` (core `ExtractBsa`), BSA window (`gui/AppBsa.cpp`,
  File menu, drop a .bsa; test hook RESAVERPP_TEST_BSA=<file>), Collect all logs (`gui/AppCollect.cpp`, `core/Zip.*` zip
  writer on miniz tdefl, `gui/SysInfo.cpp` PC report via registry/DXGI/version.dll; RESAVERPP_TEST_COLLECT), chat text
  selectable (`SelectableText` in AppAi.cpp: read-only boxes with same-text wrapping, copy unwraps).

- Released in v47 (2026-10-31): save gallery cards show "Save N, <how long ago>" (Richard). Richard confirmed v46 works under Proton.

- Released in v46 (2026-10-31): File > Open is a save gallery (`gui/AppGallery.cpp`: pictures newest first or a sortable
  list, `gallery_list=` in the ini; headers read with `ess::ReadSaveHeader` on a thread); status bar in three parts that never
  overlap; freeze detector (`gui/Watchdog.h`, sections marked in `App::draw`, written to ReSaverPP-startup.log); the path
  idle job waits for the mods scan and names (`_pathJobPending`), below-normal priority. SkyrimIstari (Richard) runs it
  under Proton/Wine; his save opens fine here under Wine without mods, his freeze happens with his 46-mod scan: waiting on
  his ReSaverPP-startup.log. Test hooks: RESAVERPP_TEST_GALLERY=pics|list, RESAVERPP_TEST_HANG=<s>.

- Released in v45 (2026-10-31): the path idle check runs on its own detached thread (`App::PathJob` in AppCharges.cpp) over
  a copy of the ACHR change forms + form id table (Decode only needs `formIDOf`); v44's per-frame slices still froze
  SkyrimIstari's save (time checked only every 16 actors). Waiting on his ReSaverPP log (Help > Show log > Copy) and save.

- Released in v44 (2026-10-31): the path idle check reads the actors a few per frame (`App::pollPathIdles`, 25 ms a frame;
  it used to decode every ACHR at once on opening, about a minute on SkyrimIstari's save), progress in the Save check, a
  "very heavy save" finding when it takes over 10 s (`RESAVERPP_TEST_HEAVY` shows it on any save). SkyrimIstari was asked
  for his save.

- Released in v43 (2026-10-31): **Ask the AI** and **Analyze Skyrim logs** (Tony: "Do it all no restrictions").
  - `core/SkyrimLogs.*` (`ess::logs`): finds logs (My Games\<game>\SKSE two levels deep, Logs\Script, NetScriptFramework\Crash),
    `Analyze` -> findings (SKSE load failures, crash logs via `crashlog::Parse` + call stack DLLs + known places table
    `kKnown`, Papyrus dumps / missing scripts / counts, plugin logs' error lines with info/debug and optional-mod lines
    left out). `resaverpp-cli --analyze-logs [My Games] [--days n]`. Tested on Tony's real logs (307 logs, 2 s).
  - `gui/Ai.*` (`ai::Chat`): OpenAI-compatible chat completions and Anthropic messages (prompt caching on system+tools),
    background thread, tool loop (40 steps), approval (`approve`) and UI-thread jobs (`runOnUi`), retries on 429/5xx.
    `gui/Net.*`: WinHTTP requests (curl elsewhere), CreateProcessW with captured output, DPAPI for the key (`ai_key=dpapi:`).
  - `gui/AppAi.cpp`: tools (get_app_state, run_cli, suggest_save_name, list_saves, open_save, game_command, analyze_logs,
    list_logs, read_log, search_logs, list_folder, write_text_file), the lesson book `kLesson` + `CliUsage()`
    (`core/CliHelp.h`, the CLI's help text moved there) + current state + `ReSaverPP AI notes.txt`; chat window, engine
    settings (presets in `ai::Presets()`), Skyrim logs window. Test hooks `RESAVERPP_TEST_AI=<text>`, `RESAVERPP_TEST_LOGS`,
    `RESAVERPP_TEST_MYGAMES`. Tested in the container against a mock engine (both API kinds); WinHTTP/CreateProcess paths
    only run on Windows (Tony tests with a real key).

- Released in v42: Save check finding + `--path-idles` / `--fix-path-idles` for actors saved with a path follower
  in state 14 (MovementAgentPathFollowerStandard waiting for a start/stop/turn idle; the save keeps the state, not the
  idle, so the load crashes at SkyrimSE+0CEC680 on 1.6.1170). Fix: state 14 -> 1 in place, as the PathIdleCrashFix
  plugin does in game (`cf::FindStuckPathIdles`, `cf::FixStuckPathIdles`).

- Released in v41 (2026-10-30). Helper version 11 (both helpers): STOP and RESET commands (Quest.Stop). App: "Stop quest in game" (needs helper 11) and
  "Copy stop command" (no helper) on the quest page. Added October 30 2026 (after v40), for an LE user's question.
- Ideas not started: an options page for choosing what the merged patch merges (partly exists as categories).
- "Clean Reload" (separate SKSE mod idea: restart the game and auto-load after death to avoid stale reload
  state) is only a proposal (Docs artifact "Clean Reload: mod proposal"); Tony has not said to build it.
