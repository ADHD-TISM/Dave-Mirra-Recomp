#!/bin/sh
# Build the one-file launcher: launcher/dist/Dave Mirra Freestyle BMX.exe (no .exe on Linux), with the icon
# launcher/icon.ico (drawn by tools/make_icon.py) as the file's icon and packed in for the window.
# Needs Python 3 with Tk and PyInstaller 6 or later (python -m pip install pyinstaller).
# DIST=<folder> builds into another folder; a relative one is taken from the folder this is run from.
# RELEASE_TAG=<version> (launcher/release.sh sets it) packs that project version in: RecompLauncher's setup step
# ("prepare") then installs exactly it, not the newest one.
set -e
OUT=${DIST:-$(dirname "$0")/dist}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)   # CDPATH off: the folder itself, and cd prints nothing
CDPATH= cd -- "$(dirname -- "$0")"
WORK=../build/launcher_pyi
mkdir -p "$WORK"
WORK=$(CDPATH= cd -- "$WORK" && pwd)
printf '%s\n' "$RELEASE_TAG" > "$WORK/release_tag.txt"   # empty without RELEASE_TAG: then the launcher takes the newest
${PYTHON:-python} -m PyInstaller --onefile --windowed --noconfirm --clean --name "Dave Mirra Freestyle BMX" \
  --icon "$(pwd)/icon.ico" --add-data "$(pwd)/icon.ico:." --add-data "$WORK/release_tag.txt:." \
  --distpath "$OUT" --workpath "$WORK" --specpath "$WORK" dmlauncher.py
echo "built in $OUT"
