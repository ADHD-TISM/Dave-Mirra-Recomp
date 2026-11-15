#!/bin/sh
# Regenerate all recompiled C from the extracted disc (see tools/regen.py).
# usage: tools/regen.sh <extracted disc dir> [game id]
# Without a game id, the game is the one whose executable is on the disc.
set -e
D=${1:-../disc}
cd "$(dirname "$0")/.."
[ -d "$D" ] || { echo "not found: $D"; exit 1; }
if [ -n "$2" ]; then exec python -I tools/regen.py --game "$2" "$D" gen; fi
exec python -I tools/regen.py "$D" gen
