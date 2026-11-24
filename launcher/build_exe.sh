#!/bin/sh
# Build the one-file launcher exe: launcher/dist/Dave Mirra Freestyle BMX.exe, with the icon launcher/icon.ico
# (drawn by tools/make_icon.py) as the file's icon and packed in for the window.
# Needs Python 3 with Tk and PyInstaller 6 or later (python -m pip install pyinstaller).
# DIST=<folder> builds into another folder than launcher/dist.
set -e
cd "$(dirname "$0")"
${PYTHON:-python} -m PyInstaller --onefile --windowed --noconfirm --clean --name "Dave Mirra Freestyle BMX" \
  --icon "$(pwd)/icon.ico" --add-data "$(pwd)/icon.ico:." \
  --distpath "${DIST:-dist}" --workpath ../build/launcher_pyi --specpath ../build/launcher_pyi dmlauncher.py
echo "built ${DIST:-$(pwd)/dist}/Dave Mirra Freestyle BMX.exe"
