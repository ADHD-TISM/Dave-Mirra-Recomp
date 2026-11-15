#!/bin/sh
# Boot-test loop: run the game; when it stops on a missing recompiled entry,
# add the address to that module's extra_<mod>.txt, regenerate, rebuild, retry.
# usage: [DM_GAME=<id>] tools/iterate.sh <image.cue> [seconds] [max_rounds]
# The game is the one gen/ was built for (gen/game.h) and its disc must be extracted in ../disc;
# DM_GAME is an optional check that it is the one you meant.
# The game runs in testrun/ so its memory cards, settings and logs stay apart
# from the ones used for real play. <image.cue> must be an absolute path.
cd "$(dirname "$0")/.."
IMG=$1; SECS=${2:-20}; MAX=${3:-20}
# the game under test is the one gen/ was last built for; DM_GAME may only confirm it
G=$(grep '^#define GAME_ID' gen/game.h 2>/dev/null | cut -d'"' -f2); G=${G:-dmbmx}
if [ -n "$DM_GAME" ] && [ "$DM_GAME" != "$G" ]; then echo "DM_GAME=$DM_GAME but gen/ is for $G: run tools/regen.sh <disc> $DM_GAME and build first"; exit 1; fi
T=testrun; mkdir -p $T
for i in $(seq 1 "$MAX"); do
  rm -f $T/missing_entries.txt
  (cd $T && timeout "$SECS" ../build/dmrecomp.exe $DM_ARGS "$IMG" > run.log 2>&1)
  if grep -q "^FATAL" $T/run.log && [ ! -s $T/missing_entries.txt ]; then echo "round $i: stopped: $(grep -m1 '^FATAL' $T/run.log)"; exit 1; fi
  if [ ! -s $T/missing_entries.txt ]; then echo "round $i: no missing entries (see $T/run.log)"; exit 0; fi
  read -r mod addr rest < $T/missing_entries.txt
  if [ "$mod" = unknown ]; then echo "round $i: miss at $addr outside any module"; cat $T/missing_entries.txt; exit 1; fi
  if grep -qi "^$addr" "games/$G/extra_$mod.txt" 2>/dev/null; then echo "round $i: $addr already listed for $mod"; exit 1; fi
  echo "round $i: adding $mod $addr $rest"
  echo "$addr $rest" >> "games/$G/extra_$mod.txt"
  sh tools/regen.sh ../disc $G > /dev/null 2>&1 || { echo regen failed; exit 1; }
  # a lingering process blocks the link; this stops EVERY running dmrecomp.exe, so do not play while iterating
  taskkill //F //IM dmrecomp.exe > /dev/null 2>&1; sleep 1
  cmake --build build -j 16 > $T/build.log 2>&1 || { tail -20 $T/build.log; echo "build failed"; exit 1; }
done
