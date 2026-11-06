#!/bin/bash
# every resettable cell of each save reset (CLI with the plugins), dumped, and the game's CELL loader run on every CELL body
S=${S:-$(pwd)}  # the folder holding fo4full/, fo4in/, fo4ng/, f4bout/after/<save>.txt (--fo4-cells listings), bld-f4b/, f4btools/
D="/mnt/user-data/uploads/common--Fallout 4/Data"
for f in $S/fo4full/Fallout4/Saves/Save*.fos $S/fo4in/*.fos $S/fo4ng/*.fos /mnt/user-data/uploads/Fallout4/Saves/*.fos; do
  n=$(basename "$f" .fos | cut -c1-40)
  ids=$(grep "can reset$" "$S/f4bout/after/$(basename "$f" .fos).txt" | awk '{print $1}' | paste -sd,)
  cnt=$(echo "$ids" | tr ',' '\n' | grep -c .)
  $S/bld-f4b/resaverpp-cli "$f" --fo4-reset-cell "$ids" --sources "$D" -o "$S/f4bout/emu/$n.fos" > "$S/f4bout/emu/$n.log" 2>&1
  ok=$(grep -c "reset set" "$S/f4bout/emu/$n.log")
  $S/f4btools/dumpcf "$S/f4bout/emu/$n.fos" "$S/f4bout/emu/$n.bin" > /dev/null
  (cd $S/f4btools/emu && python3 cellemu.py "$S/f4bout/emu/$n.bin" "$ids" > "$S/f4bout/emu/$n.emu" 2>&1)
  ff=$(grep -c "detach FFFFFFFF.*exact" "$S/f4bout/emu/$n.emu")
  all=$(cd $S/f4btools/emu && python3 cellemu.py "$S/f4bout/emu/$n.bin" | head -1)
  echo "$n: resettable $cnt, reset $ok, emulator detach FFFFFFFF + exact $ff; all CELLs: $all"
  rm -f "$S/f4bout/emu/$n.fos"
done
