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
