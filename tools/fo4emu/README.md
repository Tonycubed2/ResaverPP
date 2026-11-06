# fo4emu - Fallout 4 change-form research tools (not part of the build)

Runs the game's own loaders from Fallout4.exe 1.11.240 (not included: put it here as `Fallout4.exe`) in Unicorn on
change-form bodies dumped from saves, to find formats and check the C++ decoders read by read.

- `dumpcf.cpp`: dumps a save's change forms to `dump/<name>.bin` (u32 form version; per form: u32 refid, u8 type,
  u8 version, u32 flags, u32 formid, u32 length, body). Build against src/core.
- `achr.py`: `A(version).parse(ref, flags, body)` runs Actor::LoadGame / PlayerCharacter::LoadGame (all CRT static
  initializers are run first, the player camera is built, factories are faked) and records every read.
- `runall.py <dump>`: how many ACHRs the emulator reads exactly. `tracedump.py <dump> [player]`: writes the traces.
- `actcmp.cpp <save> <trace>`: compares fo4cf::DecodeActor's reads with an emulator trace.
- `dec.py`: Python reference decoder for REFR / NPC_. `pe.py`, `fw.py`, `cond.py`, `psum.py`, `rtti.py`: disassembly
  helpers (capstone, pefile, unicorn, numpy).
- `havemu.py <dump>`: Reset Havok check. On every REFR with change flag 2 it runs the game's havok blob reader (0xc10b00, blob
  kind 2, called by the initial data loader 0xbf77b0 after the initial data) and then TESObjectREFR::LoadGame (0x4f5c50);
  counts bodies read to the exact end and whether a blob was stored (size 0: nothing stored).
- `flstemu.py <dump>`: Purify FormLists check. Runs BGSListForm::LoadGame (0x5898f0; vtable 0x24abf80) on every FLST (type 42)
  body, on it with two null entries put in (the game keeps them as entries) and on that purified.
- `stack1.py <dump> <refid hex> [from] [to]`: one ACHR with exact call stacks (function, return site) for every read.
- `fails.cpp <save> [1]`: which ACHRs fo4cf::DecodeActor does not read to the end of the body, and why.
- The fake procedure factory (`achr.py`) uses the game's own creators for the package data classes with two bases
  (ObjectList, Ref, RefOLD, Location, LocationWrapper: the creators hand out the IPackageData base at +8). Created form
  ids (refid type 2) become 0xFFxxxxxx (`emutest.py`) so created packages take the game's created-form path; 0xbedd20
  (new form id) and 0x2383e0 (a form map insert) are stubbed.
- The fixed class names some procedure states look up (Activate, Dialogue, Travel, UseIdleMarker, SitSleep) are BSFixedStrings
  set by functions the game runs on demand, not by CRT initializers; `achr.py` sets them after the initializers.
- `extra6c.cpp <save>` lists the extra 0x6c (package data) entries the C++ decoder finds; `extra6c.py <dump> <list>` runs the
  game's own handling of each (0xceaed0(type), LoadGame +0x30, with fresh pointer id tables as Actor::LoadGame makes them)
  and compares the end.
- `npcremove.cpp <save> [plugin hex] [out.fos] [out.bin]`: without a plugin, lists the items NPCs hold per plugin index; with
  one, removes that plugin's items from every NPC that reads completely (cf::RemoveInventoryItems), writes the save, reads it
  back and dumps the edited ACHRs; `emucheck.py <file.bin>` then runs the game's Actor::LoadGame on each of them.
- The player: `achr.py` runs PlayerCharacter::LoadGame (0xd65bd0) for refid 0x14; the fake player object has PlayerCharacter's
  size (0x1000) with zeroed objects at +0xd00 (0x3ff0f0's list owner) and +0xd80 (the reference whose inventory 0x4f6bb0
  loads); with the old 0x400-byte object those pointers came from unrelated heap memory. PlayerCharacter::LoadGame reads
  the same bytes whether its form lookups succeed or not (each lookup checked in the code), so the faked lookups do not
  change its path. `fo4cf::DecodeActor` reads the player read for read like it on 18 saves.
- `playerremove.cpp <save> [plugin out.fos out.bin [1]]`: lists the player's items per plugin; with a plugin, removes its
  items from the player (cf::RemoveInventoryItems, Leftover items), optionally also edits caps / a perk rank / Strength /
  XP / level, writes and reads back the save and dumps the edited player for `emucheck.py`.
- `invcheck.py <dump>`: runs the game's ExtraDataList::LoadGame (0x294760) and inventory loader (0x34ee00) natively on each
  ACHR's reference part (after the actor prefix) and compares the end with dec.py's (the end achr.py's REFR stub uses), so
  an edited inventory is checked by the game's own code too.
- Combat (branch npc2): `achr.py` runs the game's 0x89ce90 (the pathing/combat type registry 0x3271390), stubs the cover
  manager registration 0xe78b40 and the quest entry lookup 0x66e040 (no buffer reads), and builds the package data factory
  with the game's own creators under the names the game registers (`DATA_CREATORS`; `realfactory.py`, `regnames.py` list
  the registrations). `classes.py <dump> <fails list>` lists the combat object classes (0xed5cf0 first appearances).
- `cpptrace.cpp <save> <out> [all|combat]` + `trcmp.py <dump> <out>`: fo4cf::DecodeActor's reads compared with the game's
  Actor::LoadGame read by read (sized blocks and active effect data compared as blocks; a fresh emulator when one has
  collected state; where dec.py cannot read the reference part the decoder's end is used; where the emulator stops early the
  reads so far are compared). `cmpall.sh` (MODE=all) runs every test save.
- `refrpos.cpp` + `refrnative.py` (+ `refrcheck.sh <save> <dump>`): the reference part's extra type positions and end from
  the decoder and from the game's own TESObjectREFR::LoadGame. `stack1.py` with NATIVE=1 runs that loader instead of dec.py.
- `xscan.py <type hex>...`: which ExtraDataList::LoadGame handlers reach a vsval read (lists among the "fixed" types).
