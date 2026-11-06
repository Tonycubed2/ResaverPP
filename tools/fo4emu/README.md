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
