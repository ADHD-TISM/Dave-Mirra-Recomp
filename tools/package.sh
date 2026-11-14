#!/bin/sh
# Build a ready-to-run folder from your own disc image, in one step.
# usage: tools/package.sh <image.cue or image.bin> [out_dir]
#
# Needs Python 3, CMake and MinGW-w64 gcc on PATH. The output folder holds
# dmrecomp.exe (which contains code translated from your disc, so it is for
# your own use only) and a short README. It does not copy the disc image.
set -e
[ -n "$1" ] || { echo "usage: tools/package.sh <image.cue or image.bin> [out_dir]"; exit 1; }
# pwd -W gives a Windows-style path under Git Bash, which native Python and CMake need
IMG=$(cd "$(dirname "$1")" && (pwd -W 2>/dev/null || pwd))/$(basename "$1")
cd "$(dirname "$0")/.."
ROOT=$(pwd -W 2>/dev/null || pwd)
case "$2" in "") OUT=$ROOT/dist ;; /*|?:*) OUT=$2 ;; *) OUT=$ROOT/$2 ;; esac
[ -f "$IMG" ] || { echo "not found: $IMG"; exit 1; }

# a .cue names its .bin on the FILE line
BIN=$IMG
case "$IMG" in
  *.cue|*.CUE)
    NAME=$(sed -n 's/^ *FILE *"\(.*\)".*/\1/p' "$IMG" | head -1)
    [ -n "$NAME" ] || { echo "no FILE line in $IMG"; exit 1; }
    BIN=$(dirname "$IMG")/$NAME ;;
esac
[ -f "$BIN" ] || { echo "not found: $BIN"; exit 1; }

DISC=$ROOT/build/package_disc
rm -rf "$DISC"; mkdir -p "$DISC"
echo "1/4 extracting the disc"
python -I tools/extract_iso.py "$BIN" "$DISC" > /dev/null
[ -f "$DISC/SLUS_010.26" ] || { echo "SLUS_010.26 is not on this disc: wrong game or region"; exit 1; }
echo "2/4 translating the game code"
sh tools/regen.sh "$DISC" > /dev/null
echo "3/4 building (this takes a few minutes the first time)"
cmake -S . -B build -G "MinGW Makefiles" > /dev/null
mkdir -p build
cmake --build build -j 8 > build/build.log 2>&1 || { tail -20 build/build.log; echo "build failed"; exit 1; }
echo "4/4 writing $OUT"
mkdir -p "$OUT"
cp build/dmrecomp.exe "$OUT/"
cat > "$OUT/README.txt" <<'EOF'
Dave Mirra Freestyle BMX (PS1, SLUS-01026) - static recompilation for Windows

To play: put your disc image next to dmrecomp.exe as game.cue + its .bin (or
game.bin), then run dmrecomp.exe. You can also drag the .cue onto the exe.

Keyboard: arrows = d-pad, Z = Cross, X = Circle, A = Square, S = Triangle,
Q/W = L1/R1, 1/3 = L2/R2, Enter = Start, Backspace = Select, Tab = fast-forward.
Xbox-style gamepads work, with rumble; a second one is player 2.

While playing: F6 = internal resolution (original / 2x / 4x), F7 = sharp or
smooth, F8 = 4:3 or stretched, F11 or Alt+Enter = fullscreen.

The first run writes dmrecomp.ini (settings, key and button layout) and
memcard1.mcd / memcard2.mcd (your saves) in this folder.

This exe contains code translated from your own disc. Keep it for your own use.
EOF
rm -rf "$DISC"
# desktop shortcut that starts in the output folder; skip with DM_SHORTCUT=0, replace an existing one with DM_SHORTCUT=force
if [ "${DM_SHORTCUT:-1}" != 0 ] && command -v powershell > /dev/null 2>&1; then
  FORCE=; [ "$DM_SHORTCUT" = force ] && FORCE=-Force
  powershell -NoProfile -ExecutionPolicy Bypass -File "$ROOT/tools/make_shortcut.ps1" "$OUT/dmrecomp.exe" $FORCE || echo "(could not create the desktop shortcut)"
fi
echo "done: $OUT"
