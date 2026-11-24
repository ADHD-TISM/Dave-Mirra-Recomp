#!/bin/sh
# Pack a release of the launcher with recomplauncher.json beside it, for this system (RecompLauncher installs from these;
# their names say the system and processor). Attach what it makes to the GitHub release.
#   Windows: DaveMirraFreestyleBMX-<version>-Windows-x64.zip            (and the exe on its own, for people who download it by hand)
#   Linux:   DaveMirraFreestyleBMX-<version>-Linux-x86_64.tar.xz, and with APPIMAGE_RUNTIME=<the AppImage type 2 runtime
#            (github.com/AppImage/type2-runtime)> also DaveMirraFreestyleBMX-<version>-Linux-x86_64.AppImage of the same
#            files (needs mksquashfs). Build it on an old system (Ubuntu 22.04: glibc 2.35) so it runs on newer ones too.
# usage: sh launcher/release.sh v1.2.3 [out folder]      (out folder: launcher/dist; a relative one is taken from here)
# Needs what build_exe.sh needs.
set -e
V=${1:?usage: sh launcher/release.sh <version, like v1.2.3> [out folder]}
case "$V" in   # the packed launcher only takes a version of this form (release_tag in dmlauncher.py)
    v[0-9]*) ;;
    *) echo "the version must look like v1.2.3, not $V" >&2; exit 2 ;;
esac
OUT=${2:-$(dirname -- "$0")/dist}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RELEASE_TAG="$V" DIST="$OUT" sh "$HERE/build_exe.sh"   # the launcher's setup step then installs exactly this project version
NAME="Dave Mirra Freestyle BMX"
pack() {   # pack <archive> <files...>: each file at the top of a .zip or .tar.xz
    ${PYTHON:-python} -c '
import os, sys, tarfile, zipfile
out, files = sys.argv[1], sys.argv[2:]
if out.endswith(".zip"):
    with zipfile.ZipFile(out + ".tmp", "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(f, os.path.basename(f))
else:
    with tarfile.open(out + ".tmp", "w:xz") as t:
        for f in files:
            t.add(f, os.path.basename(f))
os.replace(out + ".tmp", out)
' "$@"
    echo "packed $1"
}
case "$(uname -s)" in
    Linux*)
        pack "$OUT/DaveMirraFreestyleBMX-$V-Linux-x86_64.tar.xz" "$OUT/$NAME" "$HERE/../recomplauncher.json"
        if [ -n "$APPIMAGE_RUNTIME" ]; then
            A=$(mktemp -d)/AppDir
            mkdir -p "$A"
            cp "$OUT/$NAME" "$HERE/../recomplauncher.json" "$A/"
            ${PYTHON:-python} "$HERE/../tools/make_icon.py" "$A/.icon.ico" "$A/dmrecomp.png" > /dev/null
            rm -f "$A/.icon.ico"
            ln -s dmrecomp.png "$A/.DirIcon"
            printf '[Desktop Entry]\nType=Application\nName=%s\nExec=AppRun\nIcon=dmrecomp\nCategories=Game;\nTerminal=false\n' "$NAME" > "$A/dmrecomp.desktop"
            printf '#!/bin/sh\n# the launcher keeps its tools, project, builds and games next to itself, in this folder\nexec "$(dirname "$(readlink -f "$0")")/%s" "$@"\n' "$NAME" > "$A/AppRun"
            chmod +x "$A/AppRun" "$A/$NAME"
            IMG="$OUT/DaveMirraFreestyleBMX-$V-Linux-x86_64.AppImage"
            mksquashfs "$A" "$IMG.squashfs" -root-owned -noappend -comp gzip -quiet
            cat "$APPIMAGE_RUNTIME" "$IMG.squashfs" > "$IMG.tmp"
            rm -f "$IMG.squashfs"
            chmod +x "$IMG.tmp"
            mv "$IMG.tmp" "$IMG"
            rm -rf "$(dirname "$A")"
            echo "packed $IMG"
        fi ;;
    *)
        pack "$OUT/DaveMirraFreestyleBMX-$V-Windows-x64.zip" "$OUT/$NAME.exe" "$HERE/../recomplauncher.json" ;;
esac
