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
  Report checked 2026-11-08 (brianadragon, Nexus bugs, 1 Nov: dead animals' legs twist with helper 11-12.2 installed):
  the helper has no hooks, no game event sinks, no Havok / ragdoll / animation graph calls in any version (source history
  and the released DLLs 11, 12.2, 13: no VirtualProtect import, no BSTEventSink RTTI). Unprompted it only runs the pipe
  thread and (12+) the Ctrl poll. Not the helper's code; asked the user for logs and an A/B test (branch ragdoll).
- **ReSaverHelper LE** (`helper-le/`, project `C:\dllmaker\ReSaverHelperLE`): the Legendary Edition helper, 32-bit,
  SKSE 1.7.3, TESV.exe 1.9.32.0, built with VS2022 for Win32 (`cmake --preset vs2022-win32`,
  `cmake --build build --config Release`). Same pipe and protocol as `helper/` (HELLO reports "LE"). SKSE source
  vendored in `helper-le/extern/skse` (himika's libSKSE); missing addresses from himika's libSkyrim
  (github.com/kassent/SkyrimSouls) documented in `src/LE.h` and `src/Papyrus.h`. Papyrus calls go through the VM
  vtable (0x25 static, 0x27 method by handle; SE's slots minus one). No console on LE. Transplant differences
  (no map markers / jobj in snapshots, no made-in-game potions/enchantments) are listed in its README.
  Released as the optional file "ReSaverHelper LE" (File ID 8276523) on the LE page with the workflow
  `nexus-helper-le.yml` (uploads the zip in `helper-le/release/`, Tony's own build; file_id defaults to 8276523).
  SE helper: optional file 8222850 on the SE page, updated with `nexus-update-file.yml`. Helper 11 published with v41; helper 12 (both, rescue: GATHER/CONTROLS/SAVEGAME/RESCUE, hold Ctrl 8 s) with v49 (LE untested in game: Tony has no SKSE on LE); fixed as 12.1 (Gather: half circle behind the player, holding-cell helpers skipped, sGatherSkip, save before moving); 12.2 with v50: FOLLOWERS list + GATHER <ids> (pick who to bring in the app), unticked remembered in sGatherSkip as Plugin.esp:localid. 12.2.1 (both, 2026-11-02): packaging only, the DLL was stored read-only (0444, from Tony's file) and Linux users could not overwrite it; when making helper zips, chmod 644 the files first. Helper 14 (diagnostic stages, iDiagnosticStage). Helper 15 (branch f1menu, not built or published yet): the optional in-game page in SKSE Menu Framework (F1), see the entry at the end of section 1.
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
- Freeze after opening a save (SkyrimIstari, 8 Nov 2026, v64 + helper 14, Proton/Wine, i7 X990, no mod manager: mods folder =
  the game's Data folder on Wine's Z: drive, 865 plugins; branch freeze, not published). His v51 log: open 14 s, pollWatch 19 s,
  pollScan stuck 150 s+ at "mods scan 0 of 46" (= his Data folder's 44 sub-folders + 2: the scan was done, pollScan's
  post-processing hung). His v64 log: 44 s "between frames" with no save open (that part covered window messages, the frame
  cap wait, a dropped file and the minimized loop, which never counted a frame: minimized looked like a freeze).
  - Measured on a Data folder built like his (scratchpad freezetools/gendata.py from his plugins.txt and Save89's lists: 945
    plugins, 440 BSAs, 12.5k loose .pex, 44 folders), his Save89 (21.5 MB, level 1340), zord, tg/a.ess (4000 plugins), s140,
    natively, under Wine 9 (MinGW build, Z: drive, freezetools/winrun.sh) and with an LD_PRELOAD slow-disk shim
    (freezetools/slowfs.so: 1 ms per stat/open, 0.1 ms per directory entry, one lock like wineserver; slowrun.sh).
    Window-thread blocks before (Wine, Save89): the mods list copied for the plugin names 7.5 s and again for the load order
    check 7-9.6 s (ScriptSources copied by value), Save check items 10.5 s once and ~10 ms on EVERY call (3 calls a frame: read
    plugins.txt, 6 stats), followers view 1.5 s (twice), enchanted items 1.2 s, Mods table 0.3-0.8 s, renamed-plugin check (a
    directory listing of Data per enabled plugin: pluginPath with lowercase names). tg/a.ess: renamed check 18.5 s
    (RenameCandidates per missing plugin re-read the save's 4000 names). Two saves in a row while the scan ran: 26-28 s in
    "waiting for the previous mods scan to stop" (the second open restarted the scan and joined it; a Data-folder scan is one
    folder and never looked at Cancel) = "selecting any save from a while back freezes". UI-thread file calls in 30 s: 12,773.
  - After: worst window step 223 ms under Wine (two saves in a row; 76 ms native, 11 ms for tg/a.ess), no NOT RESPONDING in any
    run; UI-thread file calls 644 in 30 s. Results compared with RESAVERPP_TEST_DUMP_STATE (Mods table sums, missing mods with
    every field, enchanted items, quests, followers, Character effects, every Save check title) between the new path and
    RESAVERPP_SYNC_OPEN=1 RESAVERPP_NO_BACKGROUND_ANALYSIS=1 (the window working it out): identical on Save89 and zord.
  - Code: gui/AppBackground.cpp. open() runs on a worker once the window runs (setAsyncOpen; start-up/test hooks and the AI's
    open_save use openNow): read + Analyze + history numbers + picture + description + the forms for the names + MO2 profile
    scoring (ScoreProfiles, split from matchProfileToSave); finishOpen takes it over. Then an AnalysisJob re-reads the same
    unedited file (size + time checked) and works out kAnMods / kAnMissing (ComputeMissing, split from checkMissingPlugins) /
    kAnFirst (enchanted items, quests) / kAnSkyf / kAnFx (Character effects); again after the scan (mods, missing) and the names
    (skyf, fx). analysisWillProvide() makes ensureMods / checkMissingPlugins / chargedItems / questStates / skyfRefresh / charFx
    wait instead of computing (empty, never the previous save's); an edited save or a changed file falls back to the window.
    _sources is a shared_ptr (workers share it; replaced, never changed); PluginDB::Input::shared / wantedShared;
    ScriptSources::pluginFileName (names seen while scanning: pluginPath makes no file call); ops::RenameFinder; checkItems()
    cached (0.5 s, invalidateCheck); currentLoadOrder cached by file time (looked at every 3 s); the load order check reads
    plugins.txt in its thread; the saves folder watch is a worker (WatchJob; folders refreshed every 15 s; the old code never
    kept the job: fixed before release); a running scan of the same folder is kept when another save opens; Cancel works inside
    one folder (BSA and loose-script loops), and a Data-folder scan counts its archives (was "0 of 46" for the whole scan); the
    previous save is freed on a worker; the path idle check also waits for the open/analysis workers.
  - Background jobs in the status bar (Tony, same day): runningJobs() lists opening, the scan, the analysis, the names, the
    crash-on-load check, bloat / older save / load order check, merged patch, file conflicts, save history, gallery pictures,
    Collect logs, each with name, done / total (spinner when unknown) and Cancel where it can stop; "+N more" opens all of them.
    Windows that wait show the same bars (drawBackgroundProgress, drawJobsInline). 28 Russian entries added at the end of ru.txt.
  - Freeze log: watchdog::Step (Watchdog.h) names nested steps ("pollScan > the mods scan is done > checkMissingPlugins >
    renamed plugins"), logs any step over 500 ms; "between frames" is split (window messages, title bar, dropped file, minimized
    = "window minimized (not drawing)" and not a freeze, new frame, waiting for the next frame, swap); workers log over 1 s as
    "slow (background)". RESAVERPP_TIMING=1 logs every step from 1 ms (and every status line), =mark adds file lookups of
    "/resaverpp-step/begin|end/<step>" so strace shows which step made which file call (freezetools/stcount.py).
    RESAVERPP_STARTUP_LOG=<file> writes the log on Linux. Test hooks: RESAVERPP_TEST_OPEN_LATER="a.ess;b.ess" (opened as gallery
    clicks 5 s apart), RESAVERPP_TEST_DUMP_STATE, RESAVERPP_SYNC_OPEN, RESAVERPP_NO_BACKGROUND_ANALYSIS. rs_unit TestFreezeFix.
  - Not known without his save and PC: how slow his Z: drive really is (the shim is a guess), whether his 44 s with no save open
    was the window minimized (the next log says), and how long the background analysis takes there (it re-reads the save up to
    three times: after opening, after the scan, after the names; on his CPU maybe 10-30 s each, with the bars showing it).
    The scan thread makes ~240k readlink calls on the Data folder (std::filesystem, not looked into).
- Versions are whole numbers, +1 per release. **Current published version: v64** (SE, LE and Fallout 4 pages, 2026-11-08; v64: suspended stack / thread / message deletes take the thread with them, stranded thread check and repair, invalid Papyrus block read, cleanup keeps running effects' scripts, --compare shows in-place edits. v63, 2026-11-08: v63: Fix the load order (late masters), the game's real load order in the check, MO2 relative gamePath (StockGame). v62, 2026-11-08: v62: MO2 profile matched to the open save. v61, 2026-11-07: v61: effect states explained in Character effects, compile logs kept per compile. v60, 2026-11-07: v60: Character effects window (paper doll, effects by source, stuck effects). v59, 2026-11-07: v59: load order check reads the plugin copy Vortex deployed, a missing plugin takes no slot. v58, 2026-11-06: v58 adds Fallout 4 active effect removal, settlements, save bloat by mod, load order check, world items / reset an area, companions, quest checks; Skyrim followers / beast form / quest checks; SKSE/F4SE co-save viewer and cleaner; ReSaverHelper SE 13, LE 13, F4 15; the Fallout 4 helperAtLeast fix. Before that v57, 2026-11-05; v57 adds the Russian interface, the Leftover items plugin-read fix and Fallout 4 Reset Havok / Purify / Merged patch / AI / NPC+player leftovers; ReSaverHelper F4 13; the Fallout 4 main file is packed as Data/Tools/ReSaverPP/ with mod manager download on: nexus-upload layout=data-tools, mod_manager=true) (v53 was never published on its own). v55: Collect all logs gathers Fallout 4 (F4SE folder, Logs, Fallout4*.ini, AppData Fallout4 plugins.txt, Fallout4.exe version; zip "Fallout 4 logs <date>.zip").
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
  rs_unit TestFo4ActiveEffects. Left alone: the OnEffectFinish event (not sent), what a script or perk re-applies later, and Finish's
  clamp that keeps a stat from going below 0 (needs the current value). Script instances and CLI: see "Fallout 4 open items" below.
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
- Load order check: Fix the order (2026-11-08, branch lofix, not published). Tony's request: one button that moves each late
  master to just before the first plugin that needs it (and its own late masters before it), shows the moves first
  ("MysticismMagic.esp: #99 -> before 360 Ward.esp #87"; #n = place among plugins.txt's '*' lines from 0, Tony's numbering),
  asks, backs up and writes where the mod manager / game reads it.
  - Engine (checked in the exes, comments in LoadOrderCheck.h): the game does NOT load plugins in plugins.txt order. SkyrimSE.exe
    1.6.1170 0x1ba4d0 rebuilds the file list: base masters, Skyrim.ccc, then every MASTER (flags & 1; .esm/.esl set it) in
    plugins.txt order, other masters, then plain plugins in plugins.txt order; then 0x1b8f50 "hoists": for each master, a master of
    it further down (any file, also a plain .esp) is moved just before it, repeated on the moved file. Reader 0x1bb410, writer
    0x1bafa0. Fallout4.exe 1.11.240 the same: 0x2e9020 / 0x2e78a0, reader 0x24d760, IsMaster 0x2f9160. TESV.exe 1.9.32.0: 0x43e330
    lists Data (masters first, each group by file time, CompareFileTime 0x43e549), then 0x6971a0 reorders by plugins.txt (masters
    listed, other masters, plugins listed, the rest) and hoists (0x43c1d0 at 0x697576); LE's IsMaster 0x4446d0 is the header flag
    only (no .esm extension rule) -> ReadHeader(kSkyrimLE) now ignores the extension. So file times only order LE's unlisted plugins.
    Consequence: `locheck::GameOrder` mimics this; Check() gives slots in that order and reports "loads after it" only when the
    master really loads later (in practice a plain plugin needing a plain plugin listed after it); an ESM-flagged master listed late
    is an INFO now. Light/full slot numbers follow the real order (the synthetic test's Both.esm light index went 3 -> 1).
  - MO2 (modorganizer 2.5.2 = Tony's version, src/pluginlist.cpp, organizercore.cpp, processrunner.cpp; plugin files in
    modorganizer-game_bethesda src/gamebryo/gamebryogameplugins.cpp, src/creation/creationgameplugins.cpp): readPluginLists (l.39)
    takes the order from loadorder.txt when it is newer than MO2's last read or plugins.txt is not, else from plugins.txt; it reads on
    every refresh: PluginList::refresh l.322 (Refresh/F5 -> onDirectoryRefreshed -> refreshLists), and afterRun (organizercore.cpp
    l.2044: refreshESPList + savePluginList after a program MO2 started ends, also on Unlock). It WRITES plugins.txt + loadorder.txt +
    lockedorder.txt from memory on any plugin change (DelayedFileWriter), profile switch and on exit (~OrganizerCore l.156 ->
    saveCurrentProfile -> saveCurrentLists). No file watcher. So: we write both files; under MO2 (RunningUnderMO2) MO2 picks it up
    when ReSaver Plus exits; MO2 open but not our parent -> the user must press Refresh at once (else MO2 writes its old order back);
    Mo2ProcessRunning() (Toolhelp) picks the message. lockedorder.txt "name|loadindex" (readLockedOrderFrom l.697, refreshLoadOrder
    l.920): a locked plugin whose index would change refuses the fix. MO2 also enforces masters-first and child-after-master itself
    on every refresh (fixPluginRelationships l.387, since 2022) by moving the DEPENDENT down; that cannot fix Tony's case because
    setPluginPriority will not move a master below its own children (360WardTweaksSOT.esp needs 360 Ward.esp).
  - Vortex (Nexus-Mods/extension-plugin-management, util/PluginPersistor.ts): plugins.txt/loadorder.txt in %LOCALAPPDATA%\<game> ARE
    its 'loadOrder' state (registerPersistor index.ts l.559); it watches the folder (startWatch l.478) and re-reads on an outside
    change (deserialize l.369; LE 'original' format takes the order from loadorder.txt, plugins.txt only for enabled). So writing is
    kept (no "steps in Vortex" fallback needed); autosort (default on, reducers/settings.ts l.20) re-sorts with LOOT on deploy, and
    LOOT keeps masters first anyway. Vortex writes "# Automatically generated by Vortex", CRLF, latin1; we only move lines.
  - Code: `locheck::GameOrder / LateMasters / PlanFix(entries) / PlanFix(Result, enabledLines) / MoveText`, `Finding::lateMaster`,
    `PluginInfo::loadPos`; `core/LoadOrderFix.*` (`ess::lofix`): ApplyMoves (line moves only: comments, '*', disabled lines, CRLF/LF,
    BOM, missing last newline kept), EnabledLines, LockedMoved, TargetsFor (plugins.txt + loadorder.txt if present, MO2 lockedorder),
    Write (all files read first; backup <file>.<YYYYmmdd-HHMMSS>[-n].resaverpp.bak; temp + rename + read back; undo on failure), Diff.
    GUI `drawLoadOrderFix` (AppF4Bloat.cpp): "Fix the order" under the findings and as a Save check option; popup with the moves,
    files, backups, the manager note; after writing st.lo is reset so the check runs again. Test hook RESAVERPP_TEST_LOFIX=ask|write
    (with RESAVERPP_TEST_SHORTCUT=35). The Load order check window now starts the mods scan itself when no save was opened
    (ensureSourcesScanned; before, it checked with the Data folder only) and checks again once the scan is done. CLI `--fix-load-order [-n] [--vortex]` (same options as --load-order-check; prints moves and
    diffs, checks again). AI notes in AiGame.cpp (Skyrim and FO4). rs_bloat_test TestFixLoadOrder.
  - Tony's profile (copies only, scratchpad lofix-tony; plugin headers taken with a script on the device into
    C:\dllmaker\ReSaverPP\lofixtest\plugin-headers.tar): exactly "MysticismMagic.esp: #99 -> before 360 Ward.esp #87" and
    "CS_Visions.esp: #410 -> before CS_Mornhilde.esp #407"; plugins.txt (game-written header, LF) and loadorder.txt (CRLF) each got
    two lines moved, nothing else; check again: 0 late masters. His real files were not touched; he has not run it yet.
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
- Reset an area with the plugins (2026-11-06, branch f4b, not published, not tested in the real game): `PluginDB::CellDef`
  (`cell(id)`, `cellAt(ws, x, y)`): every CELL's DATA flags (bit 0 interior; the game keeps DATA in TESObjectCELL +0x40,
  0x4c28d1), XCLC x / y, the worldspace (label of the World Children group, type 1), persistent (record flag 0x400 in a
  worldspace), last override wins, a deleted override removes it. `fo4world::CellRecord` + `Lookup::cell / cellAt` and
  `fo4world::LookupFrom(db)` (GUI and CLI use it). `fo4cell::ListCells` takes indoors / outdoors and an outdoor cell's place
  from the save's initial data, else the plugins; outdoor cells named only by saved positions (and the 5 x 5 around an
  outdoor player) are listed through `cellAt`; `ResetCell` works on any cell the save or a plugin knows; `ResetCells`
  (batch, the cells read once; the CLI uses it). Exe findings (1.11.240; 1.10.163 / 984 the same code, checked on the
  decrypted copies scratchpad/f4v163, f4v984; the uploaded depot exes are encrypted): ResetInterior and Papyrus Cell.Reset()
  (0x114b5f0) and the encounter zone reset (0x49d830) all call 0x4c9330(cell, -1, 0), indoor or outdoor (outdoor: bit 29 if
  x and y fit a byte, else 28); 0x4c9330 does NOTHING when the cell has no detach time (0x4c934d, so the console command is
  a no-op on a cell with no CELL change form or one without bit 30), and the attach code neither resets nor respawns such a
  cell (0x4d2601); the player leaving a cell (0x4d6e30 -> 0x4d7605) gives it detach time = now + bit 30 (+29/28), skipped
  for FF cells. So for those cells the reset writes what the game writes after the player left once and ResetInterior ran:
  bit 30 (+29/28) and 0xFFFFFFFF, initial data put in front ({u32}, {u16 ws index, i8, i8, u32} or {u16, i16, i16, u32},
  writer 0xbf72f0); a worldspace not in the visited array is appended (writer 0xbeab40 / loader 0xbeadb0 number them
  1, 2, ...). The save's own bit 1 cell flags do not carry the interior bit (only 0x2264 bits go back, 0x4d7a73). New refusals:
  worldspace's persistent cell, FF cells, forms that are not CELLs (the old code listed the WRLDs DiamondCity 0F94,
  Goodneighbor 54BD5, DLC03FarHarbor, NukaWorld... as "interior cells, can reset": fixed). Per save (with the plugins, can
  reset before -> after): Tony's Save8 368 -> 897 (the 13 Diamond City flags-only houses now resettable), Save7 368 -> 896,
  nate 269 -> 813, nora 262 -> 797, MainQuestRailroad 977 -> 1308 (+Automatron 1014 -> 1345, +FarHarbor 1508 -> 1789),
  fo4full Save8/9/10/12/14 about +4 to +9 each. Every resettable cell of every save reset, dumped and run through the game's
  CELL loader (tools/fo4emu/cellall.sh + cellemu.py): all read exactly, detach 0xFFFFFFFF; synthetic cases (outdoor change
  form without detach, i16 coordinates, unvisited worldspace: tools/fo4emu/cellsyn.cpp) too. GUI: Reset an area tab has
  "Reset any cell" (search the mods' CELLs by name / editor ID / form ID; test hook RESAVERPP_TEST_FINDCELL), "indoors or
  outdoors?" / "outdoors" places, a tip on cells with no detach time. Tests: rs_unit TestFo4CellReset (needs
  RESAVERPP_FO4_SAVES with Tony's Save8 for the synthetic part, RESAVERPP_FO4_DATA for the plugins part).
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
- Fallout 4 open items finished (2026-11-06, branch f4a, not published):
  - Clear a companion slot through the helper: APPLY record `clearcompanion <ref> <0 companion slot|1 Dogmeat slot>` in helper-f4
    version 15 (no bump: 15 is not built yet; `Transplant.h/.cpp`, `Protocol.h` comment, README changed) -> 0: `cqf 000289E4
    "FollowersScript.ClearCompanion"` then `<ref>.setplayerteammate 0`; 1: `cqf 000289E4 "FollowersScript.ClearDogmeatCompanion"`.
    Checked in FollowersScript.pex (Fallout4 - Misc.ba2, Tony's Data): both take no arguments. ClearCompanion: GetActorReference on
    the Companion alias (nothing at all when it is not an actor), CompanionDataToggle(False, False, True, True), StopCombatAlarm,
    Clear, SendCompanionChangeEvent, PlayerHasActiveCompanion 0. CompanionDataToggle casts the holder to CompanionActorScript and
    calls IgnoreFriendlyHits / SetPlayerTeammate on that, so for the holder this finding is about (no companion script) those calls
    fail and the function goes on (Papyrus aborts only the call on None: seen in Tony's Skyrim Papyrus.0.log, the same frame
    erroring at line 437 then 439; Fallout 4's VM has the same message 0x28fc748 - not traced further) and the teammate flag stays:
    hence `setplayerteammate 0` (console 0x11c4, 1.11.240 0x5d5a40 -> Actor::SetPlayerTeammate 0xc93070(actor, arg != 0, 1, 0),
    actors only (type 0x41), the favor/XP arguments read only when turning it on; 1.10.984 0x581790 the same instruction for
    instruction, 1.10.163 0x507340 the same code). ClearDogmeatCompanion uses the holder as an Actor and turns the flag off itself.
    `fo4comp::ClearSlotCommands / ClearSlotRecord`; the finding says when the holder is saved as a non-actor reference (then the
    commands do nothing). App: the Companions window now lists the slot findings (they belong to no companion row) with "Run in the
    game: empty the slot" and copy; the Save check gets "Run it in the game" from the record. Helper parsing tested with a stub PCH
    (scratchpad f4a/htest). Not tested in the game.
  - Quest check and the DLCs: nothing was missing in the code (ScriptSources reads every archive of the Data folder, Scripts\DLC01..06
    inside "<DLC> - Main.ba2"; findPex turns "DLC03:Fragments:Quests:QF_x" into that path); earlier runs had only Fallout4 - Misc.ba2.
    `fo4quest::Stats` now has per plugin readBy / missingBy / checksBy and the missing names; the CLI prints them; rs_fo4comp_test
    requires every running DLC quest's script to be read when that DLC's Main.ba2 is in RESAVERPP_FO4_DATA. Run on Tony's real Data
    folder with the static CLI in the device shell (saves copied to C:\dllmaker\ReSaverPP\ba2test\f4a\saves; files over ~25 MB
    only went through device_commit_files split in parts): all 12 readable test saves 0 findings; every DLC stage script found; the
    only script not found anywhere is the base game's Fragments:Quests:QF_DiamondCitySuperMutantInt_001A8A0E (named by Fallout4.esm,
    not in Fallout4 - Misc.ba2: the game lacks it too). Lines checked (base / DLC): Save14 33 / 16, Save10 38 / 14, Save12 36 / 14,
    Save8 37 / 14, Save9 37 / 14, MainQuestRailroad 17 / 5, ...Automatron 17 / 4, ...FarHarbor 18 / 9, Save7/Save8 (Tony) 8 / 2,
    nate/nora 7 / 2.
  - CLI `--fo4-effects <player|ref> [--sources]` (numbered #0.., what removal does, how many script instances go with each) and
    `--fo4-remove-effects <actor>:<#n,#m|all|from:<item id or plugin>> [--sources] -o` (src/cli/EffectsF4Cli.cpp, any actor; written
    save read back). CTest `cli_fo4_effects` (tests/cli_fo4_effects.cmake: argument checks; with RESAVERPP_FO4_SAVES removes one
    no-stat effect of the player of each save and counts the effects of the new save).
  - Script instances of removed effects are removed with them now (`fo4cf::RemoveActiveEffects` step 5, `EffectScriptInstances`,
    `ops::PurgeInstances` = PurgeScript's thread/stack/queued-call logic for an instance set, plus their queued unbinds;
    `ops::ReferrersOutside`). Verified in Fallout4.exe 1.11.240: the Papyrus handle of an active effect is
    GameScript::HandlePolicy::GetHandleForObject (vtable 0x25b8620, vfunc 7 0x109a4f0, type 0xA3) = 2 << 48 | uniqueID (+0x8c) << 32 |
    target form id; in the save that is the instance's unknown2bits (2), unknown (the unique id) and RefID (the actor) - matched on
    every scripted effect of the test saves (unique id = effect data +33, after 16 bytes, 4 refs, the hit effect list's vsval count
    and a u32; ActiveEffect::LoadGame 0xb74e90 reads it at 0xb75214; the hit list was empty on all ~3,000 effects). For a type 2
    handle IsHandleLoaded (0x109a2d0) / IsHandleObjectAvailable (0x109a3d0) only look up the actor's form, ShouldAttemptToCleanHandle
    (0x109a490) is false, GetObjectForHandle (0x109aae0) searches the actor's effects for the unique id: so a left instance stays
    loaded with no effect behind it, and Start's unique id pick (0xb76730: the lowest id the actor's current effects do not use) can
    give a later effect the same handle. The game's own saves: on actors whose effect list is saved, 1 such leftover instance in 12
    saves (Save10 SynthCrippleMeleeScript on 00190540); hundreds on actors whose effect list is not saved (low process). Not traced:
    the VM's own load loop that calls these policy functions. An instance another object still names (variables of instances,
    references, arrays, structs, threads kept) is kept and the result says so. rs_unit: 318 instances removed with 1872 effects of
    12 saves (no threads on them, none kept), Papyrus section reads back clean; one edited save round-trips byte-identical.
- Linux/Wine is not officially supported.

- Character effects (2026-11-07, branch doll, not published): `core/CharEffects.*` (`ess::chfx`), GUI `gui/AppCharEffects.cpp` (Tools >
  Character effects, home button "Character effects", a button in Edit player > Active effects of both games, Save check line via
  `App::charFxCheck`; Sc::kCharEffects = 37), CLI `--char-effects [--sources] [--plugins-txt <file> [--game <dir>]] [--all]` and
  `--remove-stuck -o` (src/cli/CharEffectsCli.cpp). For trashgod69 (effects stuck from a ring he took off; 1927 effects). Window: a drawn
  figure with slot boxes (Skyrim: head, amulet, body, hands, ring, feet, right/left hand, shield, ammo; Fallout 4: head, neck, body,
  chest armor, arms, legs, hands, ring, weapons, Pip-Boy; power armor not shown), "slot not known" / "other slots" rows, the save's own
  screenshot (`_shotTex`), every worn item, then cards per source (magic item + the item it came from) in groups (worn items, items
  not worn, spells/powers/shouts, potions/food, abilities, diseases, put on by others, unknown) with Remove per effect and "Remove all
  from this source"; stuck findings in red at the top with "Remove them" (refusals listed with reasons). Removal = `chfx::Remove` ->
  cf::RemovePlayerActiveEffects / fo4cf::RemoveActiveEffects, then the player is read again and must hold exactly that many fewer
  effects (Skyrim: else the change form is put back). Verified readings:
  - Each active effect's data: f32 elapsed (+0x70), magnitude (+0x78), duration (+0x74), u32 flags, then 3-byte RefIDs caster (handle
    +0x34), target, +0x68, +0x58 = the item the effect came from ("source"): SkyrimSE.exe 1.6.1170 ActiveEffect vtable 0x187b2f8,
    SaveGame vfunc 8 0x5acea0 / LoadGame 9 0x5ad0d0 (ref writers 0x626f80 / 0x627000 write 3 bytes); LE TESV.exe 1.9.32.0 0x6568d0 the
    same order (+0x48/+0x50/+0x4c/+0x54, caster +0x28, target, +0x44, +0x3c; no LE save was available to check it on data). Fallout 4:
    the 4th ref of the layout in ChangeFormFO4.h (+0x60). Data check: flag 0x100 <=> a source is set, on every effect of every save;
    Tony's Save88/89/132: every source is a worn item; Fallout 4 (12 saves): all 12 enchantment effects' sources are equipped stacks.
  - Worn: Skyrim ExtraWorn 0x16 / ExtraWornLeft 0x17 (their vtables' GetType 0x17bfb0 / 0x17c010 in SkyrimSE.exe; ExtraEnchantment 0x9b,
    ExtraCount 0x24); the decoder now keeps every stack (`Decoded::stacks`, `InvCharge::worn/wornLeft`). Fallout 4 has no worn extra:
    Actor.IsEquipped (native 0x10f9b30 -> 0x502380 in 1.11.240) = a stack's u16 flags at +0x24 & 7, which is the u16 after the stack's
    count in the save (rs_unit checks it on every save).
  - Slots: PluginDB `Form::bipedSlots/hasBiped` (ARMO BOD2 / BODT first u32; xEdit tes5 / wbDefinitionsFO4 bit names), `Form::enchantment`
    (ARMO/WEAP EITM); Skyrim ENCH ENIT cast/enchant type and SPIT cast type now read for Skyrim too (Skyrim.esm: EnchArmorFortifyHealth01
    cast 0 / type 6; ArmorBladesHelmet 0x1002).
  - Made-in-game enchantments/potions (`chfx::CreatedObjects`): global data type 4, four tables {RefID, u32, vsval k x {RefID MGEF, f32,
    u32, u32, f32}}: reads to the exact end on trashgod69 (14843 bytes) and Tony's 3 saves; FF005BE4's 18 effects = its 18 active
    effects. Gives the MGEF (and stat) of effects of player-made enchantments, so they can be removed.
  - Timed effects past their duration are NOT reported: ActiveEffect::Update (0x5ae701..0x5ae7fd) ends an effect itself once elapsed >=
    duration unless flags & 0x4100 (those never run out by time; Tony's ordinary saves have such "past duration" effects).
  Stuck rules: (1) group "items not worn" = enchantment effect (flag 0x100 or ENCH) whose source is not a worn item and whose enchantment
  no worn stack carries (stack's own or base EITM); stuck when caster = player, duration 0, not dispelled (0x40000), not a known
  non-constant ENCH (weapon) nor a made-in-game weapon enchantment; (2) item / source plugin not in the current load order (or "?"),
  skipped when the load order holds fewer than half of the save's plugins (another setup). trashgod69: 5 findings, 9 effects: 183DCEC9
  (LegacyoftheDragonborn.esm, 3), 880063C8 (ZIA_Complete Pack.esp, 2 + 1), 9F000918 (Artifact Patch.esp, 2, one switched off),
  00049509 EnchArmorFortifyHealth01 (1); their sources are created ARMO forms FF009928 / FF007039 / FF003A1F / FF006F82 / FF008650 that
  are in no inventory and nowhere else in the save (only in the player's effect list; their own ARMO change forms are 4 bytes; the
  co-save's 'MYPR' plugin names three of them). Tony's PC lacks his plugins, so the 6 stat effects of the first two need his plugins
  (on his PC they load); without them 4 are removable here (re-read: 1923 effects, 3 findings left). Tony's saves and all FO4 saves: 0
  findings. Test: rs_unit TestCharEffects (synthetic view; RESAVERPP_SKY_SAVES, RESAVERPP_TG_SAVE=<a.ess>, RESAVERPP_FO4_SAVES +
  RESAVERPP_FO4_DATA: an equipped item's stack flags cleared -> its effects found stuck, removed, read back). GUI hooks:
  RESAVERPP_TEST_SHORTCUT=37, RESAVERPP_TEST_CFXSLOT=<chfx::Slot>, RESAVERPP_TEST_CFXREMOVE=<finding n>. Not done: Fallout 4 made-in-game
  items (none), the LE layout checked on a real LE save, any test in the real game.

- Save140 load crash (2026-11-08, branch crash, not published). Tony's Save140 edited with v62 crashed on load twice
  (crash-2026-11-08-07-55-34.log, -07-58-56.log); the original loads. Copies: scratchpad s140/old.ess, new.ess.
  - What v62 changed (ReSaverPP-startup.log on his PC, C:\dllmaker\ReSaverPP\prebuilt): TWO edits. "Removed/terminated 6
    undefined elements" AND "Removed 1 effect(s): Enter Witch's House" (Character effects window). The old `--compare` only
    counted change forms by type, so it showed Papyrus only; the player's ACHR (#42501 of 72482) had lost one active effect
    entry (49 bytes, vsval count 165 -> 164, nothing else). Compare now lists change forms edited in place.
  - The 6 undefined elements: 2 scripts with no parent (ReanimateDelivery, OnCombatExit) and 4 instances. ReanimateDelivery
    `extends ReferenceAlias` (ReAnimate.bsa, ReAnimate.esp not in the save's plugin list): handle u2b 0 / unknown 0 / RefID 0 =
    alias 0 of a quest whose form is gone. OnCombatExit: 3 plain form handles (u2b 0, unknown FFFF) on E2001CBF, E2002343,
    E2001D67 (SOTFull.esp ids; no record with those ids in the current SOTFull.esp, and the script is in no BSA/loose file of
    the mods folder or ReAnimate). None is an ActiveMagicEffect.
  - Handles (Skyrim SE, from the save data): u2b 0 + unknown FFFF = form; u2b 0 + other = alias (quest RefID, alias id); u2b 1 =
    inventory item; u2b 2 = ActiveMagicEffect (RefID actor, unknown = effect unique id = ActiveEffect +0x84, effect data offset
    33). Matched on the player (0x14 ccBGSSSE013..., 0x1D DSerEnchantQuiverFX); no AME handle uses unique id 0 (0 of 21,341 in
    5 saves). ActiveEffect::LoadGame 0x5ad0d0: f32 +0x70, +0x78, +0x74, u32 flags +0x7C, refs caster(+0x34 handle), target
    (+0x50), +0x68, +0x58, vsval hit list (types 9/10), u32 +0x80 (-1 when no conditions), u16 +0x84, vfunc 0x110 on +8, u32
    +0x88. List loader 0x5ad8b0 (from the middle-high process loader at 0x6e7436, list at middle-high +0x1A0): ref item, u8
    index, vsval archetype (checked against MGEF +0xC0), vsval size, creates (0x5c20d0) and loads, then skips by size.
    ScriptEffect vtable 0x18803e0 (LoadGame 0x5ceaf0 = base + u8 "has object").
  - The removed effect: ksws03_quest.esp (Quest - Baba Yaga and the Labyrinth) SPEL 0x94C ksws03_house_util_spell "Enter
    Witch's House" (ability, in the player's added spells), effect #0 MGEF 0x94B ksws03_house_perk_mgef, archetype 1 Script,
    no VMAD, Perk to Apply 0x94A ksws03_house_perk (activate entries for the walking house actor 0x94E). Unique id 0 (no
    script object). Effects #1/#2 of the same spell stayed.
  - Load order (SkyrimSE.exe 1.6.1170, Address Library versionlib-1-6-1170-0.bin decoded with scratchpad crashtools/addrlib.py;
    the crash log's ids are Address Library ids): 0x605f90 (35600) seeks the form id array, loads global table 1, pass 1 over
    the change forms (create), table 2, pass 2 (0x6066f0..: 0x60e5e0 initial data, then form->LoadGame vfunc 0x78), and only
    then table 3 (0x60692d, Papyrus is type 1001 there). Both crashes are in pass 2 at change form #56850 (FF003307) and #57114
    (FF003D2A); the loop index is on the stack in both logs (0xDE12, 0xDF1A). So the game had not read the Papyrus section
    when it crashed: the removed instances cannot be the cause. The only data read differently before the crash is the player's
    change form without that effect.
  - Crash sites: 07:58 Actor::LoadGame 0x686de0 -> 0x6760d0 (process level from the save, 2) -> vfunc 0x798 = 0x682f20 ->
    0x6c3720 (adds base / race abilities; [actor+0x1F8] = race copied from base+0x158) with the actor's base (actor+0x40)
    garbage (FormID 0, type 69). 07:55: initial data 0x60e630 -> 0x1b6c10 -> 0x1d74a0 with TESNPC FF001CEE's +0x1A0 garbage
    (0xF0F). Both victims are created leveled actors whose created base NPC is not in the save; a different one each run.
    NOT established: how one fewer ScriptEffect on the player leads to freed / overwritten created NPC bases (a real cause, or a
    changed heap layout exposing another bug). The game itself saves abilities with missing effect indexes routinely (43 to 74
    such spells per save; 4 in Save89), so the edited list is not an invalid shape. There is no Skyrim emulator; nothing was run.
  - Test saves for Tony (in game, both from old.ess): s140/fixed.ess = old + Remove undefined elements with the fixed code
    (Papyrus bytes identical to new.ess, change forms identical to old.ess); s140/effect-only.ess = old + only the effect
    removal (`--remove-effect 56`; change forms identical to new.ess, Papyrus = old). Expected if the analysis is right:
    fixed.ess loads, effect-only.ess crashes. Not tried yet.
  - Code (this branch): `ops::LiveEffectInstances / KeepLiveEffectInstances / KeptText`, `ops::RemoveUnattachedInstances(SaveFile&)`,
    `RemoveUndefinedElements(SaveFile&)`, `DeleteScripts(SaveFile&, names)` (CleanResult {removed, kept}); PurgeScript,
    PurgeInstances, ApplyPluginPurge, modremove::Apply (kept arrays/unbinds stay), batch::Apply carry `kept`. Rule: an AME
    instance whose effect is still in its actor's saved list is kept with its script definition (an actor the same edit deletes
    takes them with it; an actor whose effect list cannot be read keeps them). GUI `App::keptNote` (one tr() string, details to
    the log), CLI prints KeptText. On the 5 test saves no cleanup ever hit such an instance (no undefined or unattached AME
    instance exists in them); rs_unit TestLiveEffectInstances makes one script undefined in each real save and checks the kept
    set, the write and read back. `compare::Result::edited/editedCount` (Skyrim ACHR notes "active effects a -> b").
  - Side finding, not fixed: `--char-effects --plugins-txt <MO2 plugins.txt>` reports Creation Club effects (ccbgssse038 ...)
    as "no longer in your load order": MO2's plugins.txt does not list CC plugins (they are in loadorder.txt / Skyrim.ccc).
    The GUI path was not checked.

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
- Suspended stacks report (2026-11-08, Nexus, Skyrim, v62/v63; branch stacks, not published; user's saves asked for, not received).
  User deleted several Suspended Stacks entries, saved, the game loaded it, then the NEXT game save failed in ReSaver Plus with
  "Papyrus section: unexpected end of data (needed 4 bytes) while reading the header". Verified in SkyrimSE.exe 1.6.1170 (scratchpad
  skyf/exe, tools skyf/xdis.py, xref.py, fn.py (pdata function starts), vt.py (all vtables of a class), fld.py (field accesses)):
  - Every saved thread is held by exactly one of: a suspended stack table entry, a function message with its id, the latent-wait list
    (thread type byte != 0, inserted into VM+0x9350 by the stack table loader 0x1454140), or the extra slot (Papyrus `unk1` = VM+0x9398,
    `unk2` = id of the stack at VM+0x9390; VM SaveGame 0x144a840 / LoadGame 0x144b150 are on the IVMSaveLoadInterface subobject at
    VM+0x18, so their offsets read 0x18 lower). Checked on 31 Skyrim SE + 168 Fallout 4 saves: no exception (tool scratchpad stk/part.cpp).
  - The VM counts paused when suspend queues (0x9308, 0x92f8, +0x810) + overflow arrays (0x9310, 0x9300, +0x10) + queued messages
    (0x8278, ++ per loaded message in 0x1455860) + latent waits (0x935c-0x9360) + 0x9398 >= running stacks (0x932c-0x9330): 0x14517c0,
    called from VM Update 0x1444210 which then sets freeze state VM+0x939c = 2. A thread whose suspended entry was deleted is in none
    of them and nothing resumes it, so this never holds again in that session.
  - SkyrimVM freeze-and-wait 0x9c9db0 gives up after 30000 ms (QPF/1000 ticks, 0xcca200) with "error: VM timed out while waiting to
    freeze! Save will be bad."; SkyrimVM::SaveGame 0x9c9ef0 then logs "Someone tried to save without freezing the Papyrus VM first",
    calls MarkSaveInvalid 0x144bc70 (u16 0xFFFF) and writes its own u16 0xFFFF (0x9a3a30): the Papyrus block is FF FF FF FF. ReSaver
    Plus read header 0xFFFF then a u32 string count with 2 bytes left: exactly the user's error. Loading such a save the game ignores
    Papyrus (VM 0x144b150 "VM tried to load save flagged as invalid", SkyrimVM 0x9cdbd0 "Papyrus attempted to load a save flagged as invalid").
  - Java ReSaver's delete (Papyrus.removeElements, SuspendedStack branch) also removes only the table entry, so it writes the same thing;
    the user's "Java did it without problems" cannot be reconciled with the exe (maybe they never reopened the next save in Java).
  - Load side for leftovers: a suspended entry or message naming a missing thread is dropped with "Failed to read suspended stack from
    save game" (0x1455fd0) and the load reports errors (logging only).
  - Changes: Papyrus::invalid (header 0xFFFF: tail kept, written back as is, script changes into it refused); ops::StrandedThreads /
    RemoveStrandedThreads (Save check item "N script thread(s) are waiting for nothing", report warning, CLI --remove-stranded-threads;
    Skyrim SE/AE only: FO4 / LE exes not checked); ops::RemoveSuspendedStacks / RemoveFunctionMessages / RemoveThreads used by the GUI
    list deletes (a stack, a queued call with a thread id, or a thread goes with the thread and everything naming it; the extra slot is
    emptied when its thread goes). Purge / mod removal / MCM fix already removed threads with their stacks. Save check item for an invalid
    Papyrus block; --describe says FLAGGED INVALID. Tests: rs_unit TestSuspendedStacks (RESAVERPP_SKY_SAVES, RESAVERPP_TG_SAVE,
    RESAVERPP_STACK_SAVES="a.ess;b.ess", e.g. scratchpad trash/a.ess with 261 stacks).
  - User's way out: do not play on the new save (no script data). Open the save edited with v62/v63 in the fixed build: the Save
    check offers "Remove the stranded threads"; save, load in game, save again. Worth asking for: both saves, Papyrus.0.log (should show the
    two log lines above), whether saving hung ~30 s.
- Script-call floods (2026-11-08, branch flood, not published; Zordrac's Save1086, scratchpad zord/): 15151 suspended stacks, 15162
  threads; 13303 AX_ValQuestAliasScript.OnMagicEffectApply + 402 .OnHit on alias 0 of quest 0D093567 (AX ValSerano.esp) and 1281
  SM_AstridQuestAliasScript.OnMagicEffectApply on alias 0 of 20014C19 (SM_Astrid.esp), every one a thread with 0 frames, state 4.
  His "edited" save is the v62/v63 delete (entries gone, threads stranded) and Save1087 is the result: Papyrus flagged invalid.
  - New `core/ScriptFlood.*` (`ess::flood`): Find groups suspended entries and function messages by script.event (receiver objects,
    alias / effect / form, plugin); a flood = more than 100 calls of one script.event (kOver). Clear removes only suspended entries
    whose thread has 0 frames, type 0, no fragment, no next-stack flag, state 2/4/8, named by nothing else (message, 2nd entry,
    extra slot, another thread's next stack, an object's lock holder), through ops::RemoveSuspendedStacks (the v64 delete).
    Function messages are counted, never cleared (their processing was not checked). ReadBack writes + reads the Papyrus block in
    memory and runs StrandedThreads. Report warning per flood (ops::Report::floods), Save check item per flood with "Clear N queued
    calls" and "Clear all floods" (gui/AppCheck.cpp, App::clearFloods: SAFE on Skyrim SE/AE, CAUTION on LE / Fallout 4, status line
    gives the read-back counts), CLI `--clear-floods [Script.Event]` (prints the re-read stranded count), AI lines in AiGame.cpp,
    quest alias names from QUST ALST/ALLS/ALCS + ALID (QuestDef::aliases, PluginDB.cpp) for the finding text.
  - Threshold 100: the game's own line. SkyrimVM's StatsEvent sink 0x9c9070 (SkyrimVM+0x198) compares suspendedStacksCount
    (StatsEvent+4) with 100; above it calls VM vfunc 6 SetOverstressed(true) (VM+0x827C: this answers "who sets overstressed" in the
    helper 15 note) and, first after 5 s then at most every 60 s, logs "Suspended stack count is over our warning threshold, dumping
    stacks:". Normal saves (skyf/saves 3, Save89 gallery, tg/a.ess, trash/a.ess, s140 x4, fo4full 5, fo4in 3, fo4ng 2, bloatrun 2):
    the biggest single script.event is 50 (trash/a.ess, zxShp_DistantShiptBase.OnCellAttach on 50 objects): no finding.
  - Exe evidence (SkyrimSE.exe 1.6.1170): Stack::Load 0x14337b0 = state (+0x68, <9) = ActiveScriptData::major, freeze state (+0x6c)
    = minor, return value, flag (bit 0: next stack id -> +0x98), stack type (+0x84). VM update 0x144f8a0 swaps the suspend queues,
    pops {stack, IFuncCallQuery} (0x14509c0), finds the stack by id, jump table 0x144fec0: states 2/4/8 -> 0x144e370, which calls
    GetFunctionCallInfo, checks the object (Object+0 bits constructed/initialized/valid) and its lock, pushes the frame and runs;
    not ready / locked -> back into the suspend queue (0x14507b0) with state 4. Object::Load 0x1422890: saved flag 1 constructed,
    2 valid, 4 lock holder id follows (-> Object+0x18), 8 initialized, 0x10 lock bit; unknown1 = properties left to init (<<3).
    Nothing in an object lists waiting stacks. In Zordrac's save SM_Astrid's alias instance (flag 15) is locked by thread 82453272,
    a running OnMagicEffectApply (1 frame): the 1281 wait on that lock. Alias handle in the save: unknown2bits 0, unknown = alias id
    (checked against Skyrim.esm VMAD in Save132: DA10GhostTrap instance alias 5, VMAD has it on 1,3,5..; da05huntersscript instance alias 55, VMAD 46..57), refID = the quest.
  - Fallout4.exe (scratchpad re/): only the load side checked (suspended loader 0x21302b0: u32 id, lookup, u8, call 0x215ac80,
    queue push, same failure log). Resume path and state values not checked; no Fallout 4 save we have has a suspended stack.
  - Result on Save1086: 14986 calls + 14986 threads removed; 176 threads, 165 suspended stacks (98 ski_favoritesmanager.
    OnFoundInvalidItem stays: under the line), 0 stranded, instances / change forms unchanged, re-reads clean. Tests: rs_unit
    TestScriptFloods (RESAVERPP_FLOOD_SAVE=<Save1086>, plus the normal-save env vars for "no flood"). Not run in the game.
- ReSaverHelper 15, in-game "ReSaver Plus" page in SKSE Menu Framework (F1, Nexus 120352, Tony has 3.18) (2026-11-08, branch f1menu,
  not published, not built: Tony must build it in VS2022; SE/AE). New: `helper/src/MenuPage.h/.cpp` (UI), `LiveInfo.h/.cpp` (main-thread
  reads and actions), `SKSEMenuFramework.h` (the framework's API header, unchanged, from QTR-Modding/SKSE-Menu-Framework-3-Example aa8effa,
  MIT: the framework repo itself is GPL-3.0, so do not copy from it); changed: main.cpp (kPostLoad -> MenuPage::Register only when
  !Diag(5); Settings::SetStartupDiagnosticStage), Settings (iDiagnosticStage read/write: DiagnosticStageInIni, WriteDiagnosticStage),
  Server (Connected()), Game (CompleteObjective = Quest.SetObjectiveCompleted), Protocol.h 15, CMakeLists 15.0.0 + shell32, cmake lists,
  README, NOTICE. No protocol change; the app needed no change.
  - Optional: Register() returns after one log line when GetModuleHandleW(L"SKSEMenuFramework") is null. Registered at kPostLoad because
    the header caches GetProcAddress results in function statics (a call before the DLL is loaded would cache null for good), and
    ReSaverHelper.dll loads before SKSEMenuFramework.dll. Every ImGuiMCP / framework function the page calls was checked against the
    export table of Tony's installed 3.18 DLL (objdump; all present: igBeginTable, igTextWrappedV, igPlotLines_FloatPtr, ...).
  - Threads (SkyrimSE.exe 1.6.1170): the framework draws from its call hook at 36555+0x371 (0x643c00 is called at 0x646937 from Main
    update 36564 = 0x645ea0); SKSE 2.2.6 runs its task queue from the same function (0x645ea0+0xB9C); the VM runs its code tasklets
    on that thread before (0x6463e2 -> SkyrimVM 53927 -> VirtualMachine::UpdateTasklets vfunc 5 0x1444880, one tasklet after another).
    So render callbacks and SKSE tasks are on the main thread and no Papyrus code runs while a task reads the VM. The page only reads
    copies; every read and action is an SKSE task (no RunOnMain from the render callback: it would wait on itself).
  - Effects: MagicTarget vfunc 7 (Character's MagicTarget vtable 0x18a5f80, slot 7 = 0x6c5ac0 = [this+0x58]) via
    GetActiveEffectList (not VisitActiveEffects: in a VR-enabled build that uses REL::ID(33756), a VR id). ActiveEffect layout as
    CommonLib and the handoff's save work (+0x34 caster, +0x40 spell, +0x58 source, +0x70/74/78, flags +0x7C: 0x100 enchanting,
    0x8000 switched off, 0x40000 ended, +0x84 usUniqueID used to find an effect again). Remove one = ActiveEffect::Dispel(false)
    (34061 = 0x5ac880; ActiveMagicEffect.Dispel's native 0x9e5ca0 is `xor edx,edx; jmp 0x5ac880`; Dispel sets 0x40000 then finishes via
    34092 0x5ae2b0, except disease/ability/addiction types {1,4,10} with extra conditions, which it only marks). Abilities / diseases:
    Actor::RemoveSpell (38717 = 0x6c14b0; Papyrus RemoveSpell 0x9e75f0 queues task 0x5d {handle +8, spell +0x10}; the task processor
    0x656f30 reads those and calls 0x6c14b0). Worn item: ActorEquipManager::UnequipObject(player, item) with CommonLib's defaults = the
    arguments Actor.UnequipItem's native passes (0x9eb409 -> 0x6ca010 = 38901). Stuck = ReSaver Plus's rule (group "items not worn",
    constant-effect enchantment, cast by the player, duration 0, not ended); worn = ExtraWorn/ExtraWornLeft on a stack; enchantments a
    worn stack carries = its ExtraEnchantment or the base EITM.
  - Quests: TESQuest+0xE8 is ONE inline BSSimpleList<TESQuestStage*> head node (item +0xE8, next +0xF0: SetStage 0x3c9d00 walks it and
    compares each item's u16 index), not CommonLib's two pointers executedStages / waitingStages; LiveInfo::ReadQuest reads it so.
    Set stage = Game::SetStage, Complete objective = new Game::CompleteObjective, Stop = Game::Stop, refresh = Game::StartRefresh.
  - Script health: VM members as CommonLib (= the handoff's offsets): allRunningStacks 0x9320 / waitingLatentReturns 0x9350 under
    runningStacksLock 0x9318, suspendQueue1/2 numEntries (+0x810) + overflow arrays under suspendQueueLock 0x92F0,
    uiWaitingFunctionMessages 0x8278 under funcQueueLock 0x200, frozenStacksCount 0x9398 / freezeState 0x939C under frozenStacksLock
    0x9384 (IsCompletelyFrozen vfunc 7 0x1444a20 takes that lock and tests 0x939C == 2). Stack+0x80 = stackID (0x14546c9 writes
    VM+0x9380 there); Stack+0x60 top / StackFrame+0x10 owningFunction from CommonLib only, read under __try (unreadable counted).
    Sampled every 3 s from the first opening of the page (a detached thread queues SampleVM). The overstressed flag (VM+0x827C, written
    by SetOverstressed vfunc 6 0x1444a10) is NOT shown: who sets it was not found.
  - Save health: SkyrimVM isFrozen (+0x680, frozenLock +0x678; GetVMRuntimeData) is the freeze REQUEST, set by 0x9c3590, which the save
    (freeze-and-wait 54015 0x9c9dc9), loading and the console's ToggleScripts (22865 0x36ab80, "Script processing %s") call. Requested
    and freezeState != 2 for two samples = the bad-save condition (the save's wait never ends). Requested and 2 = scripts paused (ts).
    Not requested but state != 0 twice = odd. The page tells users to test with `ts` (the same request as a save). A stranded thread is
    NOT detectable without a freeze request (a live stack outside every queue cannot be told from a running one).
  - Spawns: EncWolf 00023ABE (WolfRace 1320A), EncDeer 000CF89D (DeerRace CF89B), EncSabreCat 00023AB5 (SabreCatRace 13200), read from
    Tony's Skyrim.esm (Update.esm does not override them; the Lvl* ones are template NPCs over leveled lists). PlaceAtMe on the player,
    then MoveTo(player, 300 units ahead), then Kill(None) on demand; "the one I look at" = CrosshairPickData::GetActiveTarget, else the
    console's selected ref. Beast races for the Followers page: WerewolfBeastRace 000CDD84, Dawnguard DLC1VampireBeastRace local 00283A.
  - Not in Skyrim: "Give the controls back to the game" (Fallout 4 ResetForceEnabledPlayerControls); Skyrim's ControlMap has only enabled /
    stored bits, nothing forced to undo, so the Rescue page explains it in the "Get my controls back" tooltip instead of a button.
  - Syntax check here: scratchpad f1check/chk.sh (clang 18, --target=x86_64-pc-windows-msvc, CommonLibSSE-NG v6.7.0 = Tony's copy (only
    RTTI.h differs, by line endings), mingw-w64 headers + libstdc++ 13 with shims in f1check/prelude.h, spdlog 1.16.0 / fmt 12.1.0 /
    xbyak 7.28 / DirectXMath / DirectXTK / openvr headers in f1deps). xwin cannot run here (aka.ms is blocked by the proxy). All helper
    sources pass; a broken copy shows the checker reports errors. Only verified on 1.6.1170; SE 1.5.97 and VR use CommonLib's ids, not
    checked in those exes. Nothing was run in the game.

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

- SkyrimIstari's "menus crash while ReSaver Plus is connected" report (24-26 Oct 2026, ReSaver Plus v19-v24, helper 4-10,
  game 1.6.640 under Proton on Linux Mint), checked 2026-11-08 on branch menucrash; no helper change, waiting on his crash logs:
  - SkyrimSE.exe+0F322A1 on 1.6.640 = Address Library ID 82665 + 0x11 (versionlib-1-6-640-0.bin from Tony's MO2 "Address Library for
    SKSE Plugins": 82665 starts at 0xF32290, 0x230 bytes; 1.6.1170 0xFBFC90, also 0x230). CommonLibSSE-NG: GFxMovieView::InvokeNoReturn
    (RELOCATION_ID(80547, 82665)). +0x11 in 1.6.1170 is `mov rcx,[rcx+0x58]`, its first read of `this`: something called Invoke on a null
    movie (a menu with no uiMovie). Its only game caller (ID 51052) tests the movie for null first; the other callers are inside GFx.
    The helper never calls GFx / Invoke / any menu code (grep: no GFx, Invoke, uiMovie, IMenu, ControlMap, event sink or hook in helper/src).
  - Idle with ReSaver Plus connected (helper 10 and 13 alike): ReSaver Plus sends HELLO every 3 s and GETOPTION bRefreshJournalOnLoad once
    (v24 and v63 Live.cpp/AppQuests.cpp; PROGRESS only during a refresh). Both run on the pipe thread and read only the helper's own data
    (REL::Module version, an atomic in-game flag, the INI values); nothing is queued to the main thread. At load: SKSE listener (kDataLoaded
    starts the pipe thread and, from helper 12, the Ctrl hotkey thread: GetForegroundWindow/GetAsyncKeyState only; kPreLoadGame/kNewGame
    set the atomic; kPostLoadGame does nothing with bRefreshJournalOnLoad=0). No REL::ID/Offset of its own; every CommonLib AE id it can
    reach exists in versionlib-1-6-640 (only 3 CommonLib ids are missing there: BSScaleformExternalTexture, BSWin32SystemUtility,
    InventoryChanges::RemoveAllItems, none used). Struct layouts are only used by commands (on the main thread), never while idle.
  - Asked him for: the crash logs (call stack names the DLL that called Invoke), a test with "Connect to the running game" unticked while
    ReSaver Plus runs in the same Proton prefix, and one with bListen=0 in ReSaverHelper.ini.

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

- Decision (Tony, 2026-11-08): the helpers' rescue hotkey stays Ctrl held 8 s (bRescueHotkey/iRescueHoldSeconds). Do not propose changing it.
