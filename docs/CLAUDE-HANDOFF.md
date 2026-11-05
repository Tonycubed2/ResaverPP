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
- Versions are whole numbers, +1 per release. **Current published version: v56 on the Fallout 4 page (2026-11-05, with ReSaverHelper F4 13); SE and LE pages still v55** (v53 was never published on its own). v55: Collect all logs gathers Fallout 4 (F4SE folder, Logs, Fallout4*.ini, AppData Fallout4 plugins.txt, Fallout4.exe version; zip "Fallout 4 logs <date>.zip").
- v51 (published 2026-11-02): Edit player > Active magic effects (list, tick, Remove; CLI `--effects`, `--remove-effect 3,7`).
  Save layout: high process ACTIVE EFFECTS = vsval count, each {REF item, u8 effect index, vsval type (= MGEF archetype),
  vsval size, DATA: f32 elapsed, f32 magnitude, f32 duration, u32 flags, ...}. Checked on Tony's Save112: the player's
  modifiers are the sums of the magnitudes of type 0/34 effects without flag 0x8000 (switched off), with 0x200 (recovers);
  flag 0x80 (conditions) -> temporary modifier, else permanent. Health/Magicka/Stamina/Voice keep their modifiers in
  their own lists (Decoded::vitalModsAt), not in the general AV modifier list. Removal takes the magnitude off that
  modifier; the AV comes from the MGEF (PluginDB::magicEffect, DATA 0x44), so stat-changing effects need plugin names.
  `project(ReSaverPP VERSION 50` in CMakeLists.txt.
- Linux/Wine is not officially supported.

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
