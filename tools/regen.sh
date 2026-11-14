#!/bin/sh
# Regenerate all recompiled C from the extracted disc. Iterates so that main-EXE
# functions only reachable from overlays become entries too.
# usage: tools/regen.sh <extracted disc dir>
set -e
D=${1:-../disc}
cd "$(dirname "$0")/.."
[ -f "$D/SLUS_010.26" ] || { echo "not found: $D/SLUS_010.26"; exit 1; }
rm -rf gen; mkdir -p gen
: > gen/xref_main.txt
prev=-1
while :; do
  python -I tools/recomp.py "$D/SLUS_010.26" main gen --scan "$D/AITEXT.BIN" --scan "$D/FRTEXT.BIN" --scan "$D/AISDATA.BIN" --scan "$D/FRSDATA.BIN" --extra extra_main.txt --extra gen/xref_main.txt --fullcost 80059a9c
  python -I tools/recomp.py "$D/AITEXT.BIN" ai gen --raw 800bbb6c --scan "$D/SLUS_010.26" --scan "$D/AISDATA.BIN" --roots gen/main_unresolved.txt --extra extra_ai.txt --extern gen/main_entries.txt
  python -I tools/recomp.py "$D/FRTEXT.BIN" fr gen --raw 800bbb6c --scan "$D/SLUS_010.26" --scan "$D/FRSDATA.BIN" --roots gen/main_unresolved.txt --extra extra_fr.txt --extern gen/main_entries.txt
  cat gen/ai_unresolved.txt gen/fr_unresolved.txt gen/xref_main.txt | awk '$1 < "800bbb6c"' | sort -u > gen/xref_new.txt
  mv gen/xref_new.txt gen/xref_main.txt
  n=$(wc -l < gen/main_entries.txt)
  [ "$n" = "$prev" ] && break
  prev=$n
done
python -I tools/recomp.py "$D/PLAYMOV.EXE" movie gen --extra extra_movie.txt
