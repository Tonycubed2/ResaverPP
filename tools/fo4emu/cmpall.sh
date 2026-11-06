#!/bin/bash
S=/tmp/claude-0/-home-claude/2c0005dd-b415-560f-be25-db472ea63176/scratchpad
cd $S/npc2emu
F=$S/fo4full/Fallout4/Saves
for x in "$S/fo4in/MainQuestRailroad.fos og1" "$S/fo4in/MainQuestRailroadAutomatron.fos og2" "$S/fo4in/MainQuestRailroadAutomatronFarHarbor.fos og3" "$(ls $F/Save10_*) full1" "$(ls $F/Save12_*) full2" "$(ls $F/Save14_*) full3" "$(ls $F/Save8_*) full4" "$(ls $F/Save9_*) full5" "$S/fo4ng/nate_cleansave.fos ng1" "$S/fo4ng/nora_cleansave.fos ng2" "$(ls /mnt/user-data/uploads/Fallout4/Saves/Save7_*) sv7"; do
  set -- $x
  ./cpptrace "$1" ct_$2.bin ${MODE:-combat} > /dev/null
  timeout 5000 python3 trcmp.py $2 ct_$2.bin 2>&1 | grep -v "^inits" | tail -40
done
