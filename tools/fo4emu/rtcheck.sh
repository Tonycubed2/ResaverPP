#!/bin/bash
# Leftover items round trip on NPCs, checked by the game's own loaders: rtcheck.sh <save> <plugin hex> <name>
# npcremove.cpp removes the plugin's items from every NPC, writes <name>.fos, reads it back and dumps the edited ACHRs
# (<name>.bin); refrcheck.sh compares their reference parts with TESObjectREFR::LoadGame; emucheck.py runs
# Actor::LoadGame on each (the game's reference-part end where dec.py cannot read it).
cd "$(dirname "$0")"
./npcremove "$1" "$2" "$3.fos" "$3.bin" || exit 1
./refrcheck.sh "$3.fos" "$3.bin"
REFR_ENDS="$3.bin.game.txt" python3 emucheck.py "$3.bin" 2>&1 | grep -v "^inits"
