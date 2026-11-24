#!/bin/sh
# Build the one-file launcher: launcher/dist/Dave Mirra Freestyle BMX.exe (no .exe on Linux), with the icon
# launcher/icon.ico (drawn by tools/make_icon.py) as the file's icon and packed in for the window.
# Needs Python 3 with Tk and PyInstaller 6 or later (python -m pip install pyinstaller).
# DIST=<folder> builds into another folder; a relative one is taken from the folder this is run from.
set -e
OUT=${DIST:-$(dirname "$0")/dist}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)   # CDPATH off: the folder itself, and cd prints nothing
cd "$(dirname "$0")"
${PYTHON:-python} -m PyInstaller --onefile --windowed --noconfirm --clean --name "Dave Mirra Freestyle BMX" \
  --icon "$(pwd)/icon.ico" --add-data "$(pwd)/icon.ico:." \
  --distpath "$OUT" --workpath ../build/launcher_pyi --specpath ../build/launcher_pyi dmlauncher.py
echo "built in $OUT"
