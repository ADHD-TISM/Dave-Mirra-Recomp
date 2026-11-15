#!/bin/sh
# Build the one-file launcher exe: launcher/dist/DaveMirraLauncher.exe
# Needs Python 3 with Tk and PyInstaller (python -m pip install pyinstaller).
set -e
cd "$(dirname "$0")"
${PYTHON:-python} -m PyInstaller --onefile --windowed --noconfirm --clean --name DaveMirraLauncher \
  --distpath dist --workpath ../build/launcher_pyi --specpath ../build/launcher_pyi dmlauncher.py
echo "built $(pwd)/dist/DaveMirraLauncher.exe"
