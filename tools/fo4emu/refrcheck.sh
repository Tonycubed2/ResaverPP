#!/bin/bash
# reference part of every ACHR in a dumpcf file (e.g. npcremove.cpp's edited bodies): fo4cf::DecodeRefr on the save vs the
# game's own TESObjectREFR::LoadGame in the emulator (extra type positions and end). usage: refrcheck.sh <save> <bin>
cd "$(dirname "$0")"
refs=$(python3 -c "
import sys; sys.path.insert(0,'.')
from cf import load
fv,c=load('$2'); print(' '.join('%x'%x[0] for x in c if x[1]==1))")
./refrpos "$1" $refs | sed 's/ ok end/ end/' | sort > /tmp/claude-0/-home-claude/2c0005dd-b415-560f-be25-db472ea63176/scratchpad/npc2rt/$(basename $2).cpp.txt
python3 refrnative.py "$2" 2>/dev/null | grep -v "^inits" | sort > /tmp/claude-0/-home-claude/2c0005dd-b415-560f-be25-db472ea63176/scratchpad/npc2rt/$(basename $2).game.txt
echo "$(basename $2): $(wc -l < /tmp/claude-0/-home-claude/2c0005dd-b415-560f-be25-db472ea63176/scratchpad/npc2rt/$(basename $2).game.txt) actors, $(comm -3 /tmp/claude-0/-home-claude/2c0005dd-b415-560f-be25-db472ea63176/scratchpad/npc2rt/$(basename $2).cpp.txt /tmp/claude-0/-home-claude/2c0005dd-b415-560f-be25-db472ea63176/scratchpad/npc2rt/$(basename $2).game.txt | wc -l) lines differ"
