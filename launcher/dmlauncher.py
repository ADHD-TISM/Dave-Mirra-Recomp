#!/usr/bin/env python3
"""Launcher for the Dave Mirra Freestyle BMX recompilation.

Downloads the project and its build tools, builds the game from your own disc
image, keeps it updated, edits its settings and starts it. No other installs
are needed: the build tools (a C compiler) are downloaded into a folder next to
this file, and nothing is written outside that folder.

    python dmlauncher.py                      open the window
    python dmlauncher.py status               what is ready (no window)
    python dmlauncher.py games                the games it can build
    python dmlauncher.py download|update|build|play|update-launcher
    python dmlauncher.py run [game options]   start the game and wait until it ends (its exit code is this one's)
    python dmlauncher.py prepare --dmbmx=FILE.cue --maxremix=FILE.cue [--accept-downloads]
                                              build those games without a window, printing "progress" lines;
                                              downloads only with --accept-downloads (RecompLauncher's setup step)
    python dmlauncher.py shortcut             Linux: add the game to the applications menu (Steam can add it from there)
    python dmlauncher.py --game ID ...        pick the game (dmbmx, maxremix) instead of the remembered one
    python dmlauncher.py --root DIR ...       keep everything in DIR instead
    python dmlauncher.py --disc FILE.cue ...  use this disc image

Folders under the root: tools/ (compiler, downloaded), project/ (the game's
source, downloaded), work/<game>/ (generated code, objects, caches) and
game/<game>/ (the built game, dmrecomp.ini and your memory cards for
that game). Updates never touch game/, so settings and saves are kept. The one thing written elsewhere is
the applications-menu entry on Linux, and only when you ask for it ("shortcut").

Windows and Linux. On Windows the compiler is downloaded. On Linux the system's own compiler and SDL2
development files are used when there are any; otherwise (the Steam Deck's SteamOS has no compiler) a
portable compiler (Zig) and SDL2's header files are downloaded, and the system's SDL2 and OpenGL libraries
are used. The Update tab says what to install if even those are missing.
"""
import array
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import threading
import time
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

REPO = "ADHD-TISM/Dave-Mirra-Recomp"
LAUNCHER_VERSION = "12"
APP_NAME = "Dave Mirra Freestyle BMX"   # the launcher's name: window, header and dialogs (packaged file "Dave Mirra Freestyle BMX.exe")

# Build tools: llvm-mingw (clang + a MinGW-w64 UCRT sysroot), pinned and checked.
LLVM_NAME = "llvm-mingw-20261104-ucrt-x86_64"
LLVM_URL = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/%s.zip" % LLVM_NAME
LLVM_SHA256 = "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f"
# Other targets' sysroots are not needed; leaving them out saves disk space.
LLVM_SKIP = ("aarch64-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/")
# Linux without a system compiler (the Steam Deck's SteamOS has none and a read-only system): Zig's bundled
# clang, which brings its own C library headers, plus SDL2's public headers (the SDL2 and OpenGL libraries
# themselves are the system's). Both pinned and checked.
ZIG_VERSION = "0.17.1"
ZIG_URL = "https://ziglang.org/download/%s/zig-x86_64-linux-%s.tar.xz" % (ZIG_VERSION, ZIG_VERSION)
ZIG_SHA256 = "3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a"
ZIG_TARGET = "x86_64-linux-gnu.2.31"      # runs on any glibc from 2.31 on
SDL_SRC_NAME = "SDL2-2.30.9"
SDL_SRC_URL = "https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/%s.tar.gz" % SDL_SRC_NAME
SDL_SRC_SHA256 = "24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4"
# Python for the code translator, used only when the launcher itself is a packaged exe.
PY_URL = "https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-amd64.zip"
PY_SHA256 = "8d3f33be9eb810f23c102f08475af2854e50484b8e4e06275e937be61ce3d2fb"

FROZEN = bool(getattr(sys, "frozen", False))
WINDOWS = sys.platform == "win32"
RUN_LOG, RUN_LOG_BEFORE = "last-run.log", "previous-run.log"   # in the game folder: see play
RUN_NOT_STARTED = 4   # "run" exits with this when it could not start the game (recomplauncher.json exit_codes)
NO_WINDOW = 0x08000000 if WINDOWS else 0
NEW_CONSOLE = 0x00000010 if WINDOWS else 0
EXE_FILE = "dmrecomp.exe" if WINDOWS else "dmrecomp"
UA = {"User-Agent": "dmrecomp-launcher"}


class Fail(Exception):
    """A step failed; the message says what to do about it."""


# ---------------------------------------------------------------- paths/config

def icon_file():
    """The launcher's icon (launcher/icon.ico, drawn by tools/make_icon.py): packed into the exe, or next to this
    script. None when it is not there, as for a script updated with update-launcher."""
    base = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(base, "icon.ico")
    return path if os.path.isfile(path) else None


def icon_png(path):
    """The largest picture of an .ico whose pictures are PNGs (as tools/make_icon.py writes them), as PNG bytes, or
    None. Tk on Linux cannot read an .ico, but it reads PNG."""
    try:
        with open(path, "rb") as f:
            d = f.read()
        best = None
        for i in range(struct.unpack_from("<H", d, 4)[0]):
            w, _h, _c, _r, _p, _b, size, off = struct.unpack_from("<BBBBHHII", d, 6 + 16 * i)
            w = w or 256                    # 0 stands for 256
            p = d[off:off + size]
            if p[:8] == b"\x89PNG\r\n\x1a\n" and len(p) == size and (best is None or w > best[0]):
                best = (w, p)
        return best[1] if best else None
    except (OSError, struct.error):
        return None


def set_window_icon(win, tk):
    """Give the window (and its dialogs) the launcher's icon, if there is one; a failure leaves Tk's own."""
    icon = icon_file()
    if not icon:
        return
    try:
        if WINDOWS:
            win.iconbitmap(default=icon)    # the window, its dialogs and the taskbar
        else:
            png = icon_png(icon)
            if png:
                import base64
                win.icon_image = tk.PhotoImage(master=win, data=base64.b64encode(png).decode("ascii"))   # kept: Tk needs it alive
                win.iconphoto(True, win.icon_image)
    except tk.TclError:
        pass


def default_root():
    if FROZEN and not WINDOWS and os.environ.get("APPIMAGE") and os.environ.get("APPDIR"):
        # run from our own AppImage file: its files are read-only, so everything goes next to the AppImage file
        # (RecompLauncher unpacks an AppImage and runs it from that folder, without APPIMAGE). Only when this program
        # is inside APPDIR: an APPIMAGE inherited from some other AppImage program is not ours.
        appdir = os.path.realpath(os.environ["APPDIR"])
        me = os.path.realpath(sys.executable)
        if os.path.commonpath([appdir, me]) == appdir:
            return os.path.dirname(os.path.abspath(os.environ["APPIMAGE"]))
    if FROZEN:
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


class Paths:
    def __init__(self, root, gid="dmbmx"):
        self.root = os.path.abspath(root)
        self.tools = os.path.join(self.root, "tools")
        self.project = os.path.join(self.root, "project")
        self.work = os.path.join(self.root, "work")
        self.dl = os.path.join(self.work, "dl")
        self.llvm = os.path.join(self.tools, "llvm-mingw")
        self.pydir = os.path.join(self.tools, "python")
        self.zigdir = os.path.join(self.tools, "zig")
        self.sdl_inc = os.path.join(self.tools, "sdl2-include")
        self.discdir = os.path.join(self.root, "Dave Mirra BMX")   # where people put their disc image
        self.cfg = os.path.join(self.root, "launcher.json")
        CHD_CACHE[0] = os.path.join(self.work, "chd")   # where a .chd disc image is written out for the game
        self.data = None    # --data DIR: the game keeps its settings, memory cards and logs in DIR/<game> instead
        self.use(gid)

    def use(self, gid):
        """Switch to a game: each game has its own generated code, objects and game folder (exe, settings, saves)."""
        self.gid = gid
        self.game = os.path.join(self.root, "game", gid)
        self.gen = os.path.join(self.work, gid, "gen")
        self.obj = os.path.join(self.work, gid, "obj")
        self.disc = os.path.join(self.work, gid, "disc")
        self.exe = os.path.join(self.game, EXE_FILE)
        self.home = os.path.join(self.data, gid) if self.data else self.game   # where the game runs and writes
        self.ini = os.path.join(self.home, "dmrecomp.ini")

    @property
    def cc(self):
        """The C compiler as a command: the downloaded llvm-mingw on Windows; on Linux the system's own
        compiler, or Zig's when there is none."""
        if not WINDOWS:
            sysc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
            if sysc and shutil.which("sdl2-config"):
                return [sysc]
            return [os.path.join(self.zigdir, "zig"), "cc", "-target", ZIG_TARGET]
        return [self.clang]

    @property
    def portable(self):
        """Linux build with the downloaded compiler instead of the system's."""
        return not WINDOWS and self.cc[0].startswith(self.zigdir)

    @property
    def clang(self):
        return os.path.join(self.llvm, "bin", "x86_64-w64-mingw32-clang.exe")

    @property
    def tool_python(self):
        """The Python the code translator runs with: this one; when packaged, the downloaded one on Windows and the
        system's python3 elsewhere (Linux desktops and the Steam Deck have one)."""
        if not FROZEN:
            return sys.executable
        if WINDOWS:
            return os.path.join(self.pydir, "python.exe")
        return shutil.which("python3") or "python3"


PER_GAME = ("disc", "regen_key", "build_key")   # kept separately for each game


class Config:
    def __init__(self, path):
        self.path = path
        self.lock = threading.RLock()   # the worker thread and the window both save
        self.d = {}
        try:
            with open(path, encoding="utf-8") as f:
                self.d = json.load(f)
        except (OSError, ValueError):
            self.d = {}
        for k in PER_GAME:              # settings from before there was more than one game belong to the first
            if k in self.d:
                self.d.setdefault("games", {}).setdefault("dmbmx", {}).setdefault(k, self.d.pop(k))
        self.game = self.d.get("game") or "dmbmx"

    def use(self, gid):
        with self.lock:
            self.game = gid
            self.d["game"] = gid
            self.save()

    def gget(self, gid, k, default=None):
        return self.d.get("games", {}).get(gid, {}).get(k, default)

    def gset(self, gid, k, v):
        with self.lock:
            self.d.setdefault("games", {}).setdefault(gid, {})[k] = v
            self.save()

    def get(self, k, default=None):
        if k in PER_GAME:
            return self.gget(self.game, k, default)
        return self.d.get(k, default)

    def set(self, k, v):
        if k in PER_GAME:
            return self.gset(self.game, k, v)
        with self.lock:
            self.d[k] = v
            self.save()

    def save(self):
        with self.lock:
            tmp = self.path + ".tmp"
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.d, f, indent=1, sort_keys=True)
            os.replace(tmp, self.path)


# ---------------------------------------------------------------- helpers

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, dest, log, sha256=None, label=None):
    """Download url to dest (through a .part file), checking the SHA-256 if given."""
    label = label or os.path.basename(dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    part = dest + ".part"
    last = None
    for attempt in range(3):
        try:
            h = hashlib.sha256()
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=30) as r, open(part, "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got, shown = 0, -1
                while True:
                    b = r.read(1 << 18)
                    if not b:
                        break
                    f.write(b)
                    h.update(b)
                    got += len(b)
                    if total and got * 10 // total != shown:
                        shown = got * 10 // total
                        log("  %s: %d%% of %.0f MB" % (label, shown * 10, total / 1e6))
            if sha256 and h.hexdigest() != sha256:
                raise Fail("%s failed its checksum (corrupt or tampered download)" % label)
            os.replace(part, dest)
            return
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            last = e
            log("  %s: %s%s" % (label, e, ", retrying" if attempt < 2 else ""))
            time.sleep(2)
    raise Fail("could not download %s: %s. Check your internet connection." % (label, last))


def safe_extract(zpath, dest, strip=0, skip=()):
    """Unpack a zip into dest, dropping `strip` leading path parts and refusing
    any member that would land outside dest."""
    base = os.path.realpath(dest)
    os.makedirs(base, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            parts = [p for p in info.filename.split("/") if p]
            parts = parts[strip:]
            if not parts:
                continue
            rel = "/".join(parts)
            if any((rel + "/").startswith(s) for s in skip):
                continue
            target = os.path.realpath(os.path.join(base, *parts))
            if target != base and not target.startswith(base + os.sep):
                raise Fail("unsafe path in archive: %s" % info.filename)
            if info.is_dir() or info.filename.endswith("/"):
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)


def safe_extract_tar(tpath, dest, strip=0, only=""):
    """Unpack a .tar.xz/.tar.gz into dest like safe_extract: leading path parts dropped, only members under
    `only` kept, nothing outside dest, no device files, links only to places inside dest."""
    base = os.path.realpath(dest)
    os.makedirs(base, exist_ok=True)
    with tarfile.open(tpath) as t:
        for m in t:
            if only and not m.name.startswith(only):
                continue
            parts = [p for p in m.name.split("/") if p][strip:]
            if not parts:
                continue
            target = os.path.realpath(os.path.join(base, *parts))
            if target != base and not target.startswith(base + os.sep):
                raise Fail("unsafe path in archive: %s" % m.name)
            if m.isdir():
                os.makedirs(target, exist_ok=True)
            elif m.isreg():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with t.extractfile(m) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                os.chmod(target, 0o755 if m.mode & 0o111 else 0o644)
            elif m.issym() or m.islnk():
                link = os.path.realpath(os.path.join(os.path.dirname(target), m.linkname)) if m.issym() \
                    else os.path.realpath(os.path.join(base, *[p for p in m.linkname.split("/") if p][strip:]))
                if link != base and not link.startswith(base + os.sep):
                    raise Fail("unsafe link in archive: %s" % m.name)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                if os.path.lexists(target):
                    os.remove(target)
                os.symlink(m.linkname, target) if m.issym() else shutil.copyfile(link, target)
            else:
                raise Fail("unexpected file type in archive: %s" % m.name)


def replace_dir(tmp, final):
    if os.path.isdir(final):
        shutil.rmtree(final)
    os.replace(tmp, final)


def child_env(env=None):
    """The environment for a program the launcher starts. A packaged Linux launcher (PyInstaller) puts its own bundled
    libraries first in LD_LIBRARY_PATH; the compiler, the code translator and the game must get the system's, so the
    variable goes back to what it was before (LD_LIBRARY_PATH_ORIG), or away. Elsewhere it is left as it is."""
    if not FROZEN or WINDOWS:
        return env
    e = dict(os.environ if env is None else env)
    orig = e.pop("LD_LIBRARY_PATH_ORIG", None)
    if orig is not None:
        e["LD_LIBRARY_PATH"] = orig
    else:
        e.pop("LD_LIBRARY_PATH", None)
    return e


def run(cmd, log, cwd=None, env=None, tag=""):
    """Run a command, streaming its output to log. Returns the output text."""
    p = subprocess.Popen(cmd, cwd=cwd, env=child_env(env), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         creationflags=NO_WINDOW, text=True, errors="replace")
    out = []
    for line in p.stdout:
        out.append(line)
        if tag:
            log(tag + line.rstrip())
    p.wait()
    text = "".join(out)
    if p.returncode != 0:
        raise Fail("%s failed (exit %d)\n%s" % (os.path.basename(cmd[0]), p.returncode, text[-2000:]))
    return text


def key_of(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode("utf-8"))
        h.update(b"\0")
    return h.hexdigest()


def file_bytes(path):
    with open(path, "rb") as f:
        return f.read()


# ---------------------------------------------------------------- download/update

def version_tuple(tag):
    m = re.fullmatch(r"v?(\d+(?:\.\d+)*)", tag)
    return tuple(int(x) for x in m.group(1).split(".")) if m else None


def latest_tag(log, strict=False):
    """Newest vX.Y.Z tag of the project. None if the project has no tags; if GitHub cannot be
    reached that is None too, or a Fail when strict (so a failed lookup never falls back to main)."""
    try:
        req = urllib.request.Request("https://api.github.com/repos/%s/tags?per_page=100" % REPO, headers=UA)
        with urllib.request.urlopen(req, timeout=20) as r:
            tags = [t["name"] for t in json.load(r)]
    except (urllib.error.URLError, OSError, ValueError, TimeoutError) as e:
        if strict:
            raise Fail("could not look up the newest version (%s). Try again in a few minutes." % e)
        log("could not look up the newest version: %s" % e)
        return None
    tags = [t for t in tags if version_tuple(t)]
    return max(tags, key=version_tuple) if tags else None


# Linux: the build uses the system's compiler and SDL2 instead of downloading them.
LINUX_HINT = ("Install them with your package manager, then try again:\n"
              "  Debian/Ubuntu:  sudo apt install build-essential libsdl2-dev libgl-dev\n"
              "  Fedora:         sudo dnf install gcc SDL2-devel mesa-libGL-devel\n"
              "  Arch:           sudo pacman -S base-devel sdl2")
_sdl = {}


LIB_DIRS = ("/usr/lib", "/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/local/lib")


def find_lib(name):
    """Full path of a shared library the system has, or None."""
    for d in LIB_DIRS:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def linux_missing(P):
    """What the build needs from the system and cannot get for itself."""
    miss = []
    if FROZEN and not shutil.which("python3"):   # the packaged launcher runs the code translator with it
        miss.append("Python 3 (python3)")
    if not P.portable:
        if not (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")):
            miss.append("a C compiler (gcc)")
        if not shutil.which("sdl2-config"):
            miss.append("the SDL2 development files")
    else:    # the compiler and headers are downloaded; the libraries must be on the system already
        if not find_lib("libSDL2-2.0.so.0"):
            miss.append("the SDL2 library (libSDL2-2.0.so.0)")
        if not find_lib("libGL.so.1"):
            miss.append("the OpenGL library (libGL.so.1)")
    return miss


def sdl_flags(which):
    """sdl2-config --cflags or --libs, as a list."""
    if which not in _sdl:
        r = subprocess.run(["sdl2-config", which], capture_output=True, text=True, env=child_env())
        if r.returncode != 0:
            raise Fail("sdl2-config %s failed: %s" % (which, r.stderr.strip()))
        _sdl[which] = r.stdout.split()
    return _sdl[which]


def portable_ok(P):
    return os.path.isfile(os.path.join(P.zigdir, "zig")) and os.path.isfile(os.path.join(P.sdl_inc, "SDL.h"))


def toolchain_ok(P):
    if not WINDOWS:
        return not linux_missing(P) and (not P.portable or portable_ok(P))
    ok = os.path.isfile(P.clang)
    if FROZEN:
        ok = ok and os.path.isfile(P.tool_python)
    return ok


def install_toolchain(P, log):
    if not WINDOWS:
        miss = linux_missing(P)
        if miss:
            raise Fail("Not found on this system: %s.\n%s" % (", ".join(miss), LINUX_HINT))
        if not P.portable:
            log("build tools found: %s, SDL2 %s" % (P.cc[0], " ".join(sdl_flags("--version"))))
            return
        if not os.path.isfile(os.path.join(P.zigdir, "zig")):
            z = os.path.join(P.dl, "zig-%s.tar.xz" % ZIG_VERSION)
            if not (os.path.isfile(z) and sha256_file(z) == ZIG_SHA256):
                log("no C compiler on this system: downloading a portable one (Zig, about 55 MB)")
                fetch(ZIG_URL, z, log, ZIG_SHA256, "compiler")
            log("unpacking the compiler")
            tmp = P.zigdir + ".tmp"
            shutil.rmtree(tmp, ignore_errors=True)
            safe_extract_tar(z, tmp, strip=1)
            replace_dir(tmp, P.zigdir)
            os.remove(z)
        if not os.path.isfile(os.path.join(P.sdl_inc, "SDL.h")):
            z = os.path.join(P.dl, SDL_SRC_NAME + ".tar.gz")
            if not (os.path.isfile(z) and sha256_file(z) == SDL_SRC_SHA256):
                log("downloading SDL2's header files (about 8 MB)")
                fetch(SDL_SRC_URL, z, log, SDL_SRC_SHA256, "SDL2 headers")
            tmp = P.sdl_inc + ".tmp"
            shutil.rmtree(tmp, ignore_errors=True)
            safe_extract_tar(z, tmp, strip=2, only=SDL_SRC_NAME + "/include/")
            replace_dir(tmp, P.sdl_inc)
            os.remove(z)
        if not portable_ok(P):
            raise Fail("the compiler did not unpack correctly; delete the tools folder and try again")
        return
    if not os.path.isfile(P.clang):
        z = os.path.join(P.dl, LLVM_NAME + ".zip")
        if not (os.path.isfile(z) and sha256_file(z) == LLVM_SHA256):
            log("downloading the C compiler (about 190 MB)")
            fetch(LLVM_URL, z, log, LLVM_SHA256, "compiler")
        log("unpacking the compiler")
        tmp = P.llvm + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        safe_extract(z, tmp, strip=1, skip=LLVM_SKIP)
        replace_dir(tmp, P.llvm)
        os.remove(z)
    if FROZEN and not os.path.isfile(P.tool_python):
        z = os.path.join(P.dl, "python-embed.zip")
        log("downloading Python for the code translator (about 11 MB)")
        fetch(PY_URL, z, log, PY_SHA256, "python")
        tmp = P.pydir + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        safe_extract(z, tmp)
        replace_dir(tmp, P.pydir)
        os.remove(z)
    if not toolchain_ok(P):
        raise Fail("the compiler did not unpack correctly; delete the tools folder and try again")


# Games the launcher knows by name before the project is downloaded; the project's games/*.json have the details.
BUILTIN_GAMES = [("dmbmx", "Dave Mirra Freestyle BMX", "SLUS_010.26"),
                 ("maxremix", "Dave Mirra Freestyle BMX: Maximum Remix", "SLUS_013.47")]


def load_profiles(P):
    out = {}
    gd = os.path.join(P.project, "games")
    try:
        names = sorted(os.listdir(gd))
    except OSError:
        return out
    for n in names:
        if n.endswith(".json"):
            try:
                with open(os.path.join(gd, n), encoding="utf-8") as f:
                    g = json.load(f)
                out[g["id"]] = g
            except (OSError, ValueError, KeyError):
                continue
    return out


def game_list(P):
    """[(id, name, exe)] from the project if it is downloaded, else the built-in list."""
    pr = load_profiles(P)
    if pr:
        return [(g["id"], g["name"], g["exe"]) for g in pr.values()]
    return list(BUILTIN_GAMES)


def get_profile(P):
    g = load_profiles(P).get(P.gid)
    if not g:
        raise Fail("this version of the project does not know the game \"%s\" (Update tab)" % P.gid)
    return g


def project_ok(P):
    return (all(os.path.isfile(os.path.join(P.project, "tools", n)) for n in ("recomp.py", "regen.py", "extract_iso.py"))
            and bool(load_profiles(P)))


def fetch_project_zip(P, tag, log):
    ref = "refs/tags/" + tag if tag else "refs/heads/main"
    url = "https://github.com/%s/archive/%s.zip" % (REPO, ref)
    z = os.path.join(P.dl, "project-%s.zip" % (tag or "main"))
    log("downloading the project (%s)" % (tag or "main"))
    fetch(url, z, log, None, "project")
    return z


def release_tag():
    """The project version a packaged launcher was released with (launcher/release.sh puts it in), or None."""
    base = getattr(sys, "_MEIPASS", None)
    try:
        with open(os.path.join(base, "release_tag.txt"), encoding="ascii") as f:
            tag = f.read().strip()
        return tag if re.fullmatch(r"v[0-9][0-9A-Za-z.\-]*", tag) else None
    except (OSError, TypeError, ValueError):
        return None


def install_project(P, C, log, force=False, want=None):
    """Download the newest project version (or the version want) unless that one is already installed."""
    tag = want or latest_tag(log, strict=not (project_ok(P) and C.get("tag")))
    have = C.get("tag")
    if project_ok(P) and not force and have and (tag is None or tag == have):
        log("project is up to date (%s)" % have)
        return have
    z = fetch_project_zip(P, tag, log)
    tmp = P.project + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    safe_extract(z, tmp, strip=1)
    if not os.path.isfile(os.path.join(tmp, "tools", "recomp.py")):
        shutil.rmtree(tmp, ignore_errors=True)
        raise Fail("the downloaded project looks wrong (no tools/recomp.py)")
    replace_dir(tmp, P.project)
    os.remove(z)
    C.set("tag", tag or "main")
    log("project installed: %s" % (tag or "main"))
    return tag or "main"


def update_launcher(P, C, log):
    """Replace this file with the launcher from the project's newest version."""
    if FROZEN:
        raise Fail("This is the packaged launcher: download the new launcher from the project's releases page.")
    here = os.path.dirname(os.path.abspath(__file__))
    if os.path.exists(os.path.join(here, "..", ".git")):
        raise Fail("this launcher is inside a git checkout; update it with git pull instead")
    tag = latest_tag(log, strict=True)
    z = fetch_project_zip(P, tag, log)
    try:
        with zipfile.ZipFile(z) as zf:
            names = [n for n in zf.namelist() if n.endswith("/launcher/dmlauncher.py")]
            if not names:
                raise Fail("this version of the project has no launcher")
            new = zf.read(names[0])
    finally:
        os.remove(z)
    me = os.path.abspath(__file__)
    if new == file_bytes(me):
        log("the launcher is already the newest")
        return False
    m = re.search(rb'^LAUNCHER_VERSION = "(\d+)"', new, re.M)
    if not m:
        raise Fail("the downloaded launcher looks wrong; not replacing this one")
    if int(m.group(1)) <= int(LAUNCHER_VERSION):
        log("the launcher is already the newest (version %s)" % LAUNCHER_VERSION)
        return False
    tmp = me + ".new"
    with open(tmp, "wb") as f:
        f.write(new)
    os.replace(tmp, me)
    log("launcher updated; restart it to use the new version")
    return True


# ---------------------------------------------------------------- CHD disc images
# A .chd (MAME's compressed hunks of data, version 5) is read here with the standard library only: its map, its CD
# tracks, and hunks compressed with LZMA or Deflate (the "cdlz" and "cdzl" codecs chdman uses for CDs). Hunks in FLAC or
# Zstandard, and CHDs that need a parent, are refused with a message saying how to make one this reads. The launcher reads
# a few sectors of it to tell which game it is; to translate and play, it writes the disc out once as the .bin and .cue the
# game reads (work/chd/), the same bytes the image was made from.

CHD_CACHE = [None]                     # the folder the images are written out to: set by Paths (work/chd)
CHD_MAX_LOGICAL = 2 << 30              # a 99-minute CD in 2448-byte frames is about 1.09 GB; anything above is no CD
CD_FRAME, CD_SECTOR = 2448, 2352       # a CHD CD frame: the sector and its 96 bytes of subcode
CHD_CD_CODECS = {b"cdlz": "lzma", b"cdzl": "deflate", b"cdfl": "flac", b"cdzs": "zstd"}


class ChdBits:
    """Bits read most significant first, as libchdr's bitstream does; reading past the end gives zeros."""
    def __init__(self, data):
        self.data, self.pos = data, 0

    def read(self, n):
        if n == 0:
            return 0
        first, last = self.pos >> 3, (self.pos + n - 1) >> 3
        chunk = self.data[first:last + 1].ljust(last - first + 1, b"\0")
        v = (int.from_bytes(chunk, "big") >> ((last + 1) * 8 - self.pos - n)) & ((1 << n) - 1)
        self.pos += n
        return v


class ChdHuffman:
    """The canonical Huffman code a v5 map is written with (libchdr's huffman_import_tree_rle and decode_one)."""
    def __init__(self, bits, numcodes=16, maxbits=8):
        nb = 5 if maxbits >= 16 else 4 if maxbits >= 8 else 3
        lens = []
        while len(lens) < numcodes:
            n = bits.read(nb)
            if n != 1:
                lens.append(n)
            else:
                n = bits.read(nb)
                if n == 1:
                    lens.append(n)
                else:
                    lens.extend([n] * (bits.read(nb) + 3))
        if len(lens) != numcodes or max(lens) > maxbits:
            raise Fail("the .chd map is damaged (its code table)")
        histo = [0] * 33
        for n in lens:
            histo[n] += 1
        start = 0
        for n in range(32, 0, -1):
            nxt = (start + histo[n]) >> 1
            histo[n], start = start, nxt
        self.maxbits, self.table = maxbits, [None] * (1 << maxbits)
        for sym, n in enumerate(lens):
            if n:
                code = histo[n]
                histo[n] += 1
                shift = maxbits - n
                for k in range(code << shift, (code + 1) << shift):
                    self.table[k] = (sym, n)

    def decode(self, bits):
        save = bits.pos
        hit = self.table[bits.read(self.maxbits)]
        if hit is None:
            raise Fail("the .chd map is damaged (an unknown code)")
        bits.pos = save + hit[1]
        return hit[0]


def _ecc_tables():
    f, b = [0] * 256, [0] * 256
    for i in range(256):
        j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
        f[i], b[i ^ j] = j, i
    return f, b


def cd_ecc_generate(sector):
    """Put back the P and Q error correction of a sector (bytes 2076 to 2351), as the CD format defines it. A Mode 2
    sector (a PlayStation disc) has it worked out with its 4-byte address read as zeros."""
    f, b = _ecc_tables()
    address = bytes(sector[12:16])
    if sector[15] == 2:
        sector[12:16] = b"\0\0\0\0"

    def block(major_count, minor_count, major_mult, minor_inc, dest):
        size = major_count * minor_count    # bytes from offset 12 on; Q covers P's bytes, so P is made first
        for major in range(major_count):
            index, ea, eb = (major >> 1) * major_mult + (major & 1), 0, 0
            for _minor in range(minor_count):
                t = sector[12 + index]
                index += minor_inc
                if index >= size:
                    index -= size
                ea ^= t
                eb ^= t
                ea = f[ea]
            ea = b[f[ea] ^ eb]
            sector[dest + major] = ea
            sector[dest + major + major_count] = ea ^ eb
    block(86, 24, 2, 86, 0x81C)
    block(52, 43, 86, 88, 0x8C8)
    sector[12:16] = address


def flac_frames(data, nsamples):
    """Decode FLAC frames (no stream header, as a CHD's "cdfl" hunk holds them) until nsamples stereo samples are
    out. Returns (the samples interleaved left, right; the byte offset after the last frame). The subset libFLAC
    writes: constant, verbatim, fixed and LPC subframes, Rice residuals, the three stereo decorrelations."""
    bits = bin(int.from_bytes(b"\x01" + data, "big"))[3:]   # one '0' or '1' per bit: str.find reads unary codes fast
    pos = 0

    def rd(n):
        nonlocal pos
        if not n:
            return 0
        v = int(bits[pos:pos + n], 2)
        pos += n
        return v

    def rds(n):
        v = rd(n)
        return v - (1 << n) if n and v >> (n - 1) else v

    def residual(bs, order):
        nonlocal pos
        method, porder = rd(2), None
        pbits, esc = (4, 15) if method == 0 else (5, 31)
        porder = rd(4)
        out = []
        for p in range(1 << porder):
            n = (bs >> porder) - (order if p == 0 else 0)
            k = rd(pbits)
            if k == esc:
                w = rd(5)
                out.extend(rds(w) for _ in range(n))
                continue
            find = bits.find
            for _ in range(n):
                one = find("1", pos)
                if one < 0:
                    raise Fail("a FLAC hunk of the .chd is damaged")
                q, pos = one - pos, one + 1
                v = (q << k) | (int(bits[pos:pos + k], 2) if k else 0)
                pos += k
                out.append((v >> 1) ^ -(v & 1))
        return out

    left, right, done = [], [], 0
    while done < nsamples:
        if rd(14) != 0x3FFE:
            raise Fail("a FLAC hunk of the .chd is damaged (no frame start)")
        rd(2)
        bscode, srcode, chan, sscode = rd(4), rd(4), rd(4), rd(3)
        rd(1)
        first = rd(8)
        extra = 0
        while first & (0x80 >> extra):
            extra += 1
        for _ in range(max(extra - 1, 0)):
            rd(8)
        if bscode == 1:
            bs = 192
        elif 2 <= bscode <= 5:
            bs = 576 << (bscode - 2)
        elif bscode == 6:
            bs = rd(8) + 1
        elif bscode == 7:
            bs = rd(16) + 1
        elif bscode >= 8:
            bs = 256 << (bscode - 8)
        else:
            raise Fail("a FLAC hunk of the .chd is damaged (block size)")
        if srcode == 12:
            rd(8)
        elif srcode in (13, 14):
            rd(16)
        ss = {0: 16, 1: 8, 2: 12, 4: 16, 5: 20, 6: 24}.get(sscode, 16)
        rd(8)                                             # header CRC-8
        if chan not in (1, 8, 9, 10):
            raise Fail("a FLAC hunk of the .chd is not stereo")
        chans = []
        for ch in range(2):
            bps = ss + (1 if (chan == 8 and ch == 1) or (chan == 9 and ch == 0) or (chan == 10 and ch == 1) else 0)
            rd(1)
            t = rd(6)
            wasted = 0
            if rd(1):
                one = bits.find("1", pos)
                wasted, pos = one - pos + 1, one + 1
                bps -= wasted
            if t == 0:
                s = [rds(bps)] * bs
            elif t == 1:
                s = [rds(bps) for _ in range(bs)]
            elif 8 <= t <= 12:
                order = t - 8
                s = [rds(bps) for _ in range(order)]
                res = residual(bs, order)
                if order == 0:
                    s = res
                else:
                    for r in res:
                        if order == 1:
                            s.append(r + s[-1])
                        elif order == 2:
                            s.append(r + 2 * s[-1] - s[-2])
                        elif order == 3:
                            s.append(r + 3 * s[-1] - 3 * s[-2] + s[-3])
                        else:
                            s.append(r + 4 * s[-1] - 6 * s[-2] + 4 * s[-3] - s[-4])
            elif t >= 32:
                order = (t & 31) + 1
                s = [rds(bps) for _ in range(order)]
                prec = rd(4) + 1
                shift = rds(5)
                coefs = [rds(prec) for _ in range(order)]
                res = residual(bs, order)
                rc = coefs[::-1]
                for r in res:
                    tail = s[-order:]
                    s.append(r + (sum(c * x for c, x in zip(rc, tail)) >> shift))
            else:
                raise Fail("a FLAC hunk of the .chd uses a subframe type this does not read")
            if wasted:
                s = [x << wasted for x in s]
            chans.append(s)
        pos = (pos + 7) & ~7
        rd(16)                                            # frame CRC-16
        a, b = chans
        if chan == 8:                                     # left, side
            b = [x - y for x, y in zip(a, b)]
        elif chan == 9:                                   # side, right
            a = [x + y for x, y in zip(a, b)]
        elif chan == 10:                                  # mid, side
            m = [(x << 1) | (y & 1) for x, y in zip(a, b)]
            a = [(x + y) >> 1 for x, y in zip(m, b)]
            b = [(x - y) >> 1 for x, y in zip(m, b)]
        left += a
        right += b
        done += bs
    inter = [0] * (2 * nsamples)
    inter[0::2], inter[1::2] = left[:nsamples], right[:nsamples]
    return inter, pos >> 3


def chd_errors():
    """What reading a damaged or cut-short .chd can raise besides Fail and OSError; each is turned into a Fail."""
    import lzma
    import zlib
    return (struct.error, KeyError, ValueError, IndexError, EOFError, OverflowError, ZeroDivisionError,
            lzma.LZMAError, zlib.error)


def chd_damaged(path, e):
    return Fail("%s is damaged or not complete (%s); if it is still being copied, wait until it is done"
                % (os.path.basename(path), e.__class__.__name__ + (": %s" % e if str(e) else "")))


class Chd:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        try:
            self._open()
        except Fail:
            self.f.close()
            raise
        except chd_errors() as e:
            self.f.close()
            raise chd_damaged(path, e)

    def _open(self):
        path = self.path
        h = self.f.read(124)
        if h[:8] == b"MComprHD" and len(h) < 124:
            raise chd_damaged(path, EOFError("the header is cut short"))
        if len(h) < 124 or h[:8] != b"MComprHD":
            raise Fail("%s is not a CHD image" % os.path.basename(path))
        version = struct.unpack_from(">I", h, 12)[0]
        if version != 5:
            raise Fail("%s is a version %d CHD; this reads version 5 (make it again with a recent chdman: "
                       "chdman createcd -i <your .cue> -o <new .chd>)" % (os.path.basename(path), version))
        self.codecs = [h[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logical, mapoffset, metaoffset = struct.unpack_from(">QQQ", h, 32)
        self.hunkbytes, self.unitbytes = struct.unpack_from(">II", h, 56)
        if h[104:124] != b"\0" * 20:
            raise Fail("%s needs a parent CHD; make a whole one with chdman" % os.path.basename(path))
        if self.unitbytes != CD_FRAME or not self.hunkbytes or self.hunkbytes % CD_FRAME:
            raise Fail("%s is not a CD image (its units or hunks are not whole CD frames)" % os.path.basename(path))
        if self.logical > CHD_MAX_LOGICAL or self.hunkbytes > 1 << 20:   # a CD holds at most about 1.1 GB this way
            raise Fail("%s is not a CD image (it says it holds %d bytes in hunks of %d)"
                       % (os.path.basename(path), self.logical, self.hunkbytes))
        self.size = os.fstat(self.f.fileno()).st_size
        self.hunks = (self.logical + self.hunkbytes - 1) // self.hunkbytes
        self.map = self._read_map(mapoffset)
        self.tracks = self._read_tracks(metaoffset)
        self._cache = {}

    def close(self):
        self.f.close()

    def _read_map(self, mapoffset):
        if self.codecs[0] == b"\0\0\0\0":    # an uncompressed CHD: a plain table of hunk offsets
            if mapoffset + 4 * self.hunks > self.size:
                raise chd_damaged(self.path, EOFError("its hunk table runs past the end of the file"))
            self.f.seek(mapoffset)
            raw = self.f.read(4 * self.hunks)
            return [(4, struct.unpack_from(">I", raw, 4 * i)[0] * self.hunkbytes, self.hunkbytes, None) if
                    struct.unpack_from(">I", raw, 4 * i)[0] else (None, 0, 0, None) for i in range(self.hunks)]
        self.f.seek(mapoffset)
        mh = self.f.read(16)
        mapbytes = struct.unpack_from(">I", mh, 0)[0]
        firstoffs = int.from_bytes(mh[4:10], "big")
        mapcrc = struct.unpack_from(">H", mh, 10)[0]
        lengthbits, selfbits, parentbits = mh[12], mh[13], mh[14]
        if lengthbits > 24 or selfbits > 48 or parentbits > 48:   # more than a map entry's 3 and 6 bytes can hold
            raise chd_damaged(self.path, ValueError("its map header is damaged"))
        if mapoffset + 16 + mapbytes > self.size:
            raise chd_damaged(self.path, EOFError("its map runs past the end of the file"))
        bits = ChdBits(self.f.read(mapbytes))
        huff = ChdHuffman(bits)
        comps, rep, last = [], 0, 0
        for _ in range(self.hunks):
            if rep:
                comps.append(last)
                rep -= 1
                continue
            v = huff.decode(bits)
            if v == 7:                       # COMPRESSION_RLE_SMALL
                comps.append(last)
                rep = 2 + huff.decode(bits)
            elif v == 8:                     # COMPRESSION_RLE_LARGE
                comps.append(last)
                rep = 2 + 16 + (huff.decode(bits) << 4)
                rep += huff.decode(bits)
            else:
                comps.append(v)
                last = v
        out, cur, last_self, last_parent = [], firstoffs, 0, 0
        raw = bytearray()
        for n, c in enumerate(comps):
            length, offset, crc = 0, 0, 0
            if c <= 3:                       # COMPRESSION_TYPE_0..3: compressed with codecs[c]
                length = bits.read(lengthbits)
                offset, cur = cur, cur + length
                crc = bits.read(16)
            elif c == 4:                     # COMPRESSION_NONE
                length = self.hunkbytes
                offset, cur = cur, cur + length
                crc = bits.read(16)
            elif c == 5:                     # COMPRESSION_SELF: a copy of an earlier hunk
                offset = last_self = bits.read(selfbits)
            elif c == 6:                     # COMPRESSION_PARENT
                offset = last_parent = bits.read(parentbits)
            elif c == 9:                     # COMPRESSION_SELF_0
                c, offset = 5, last_self
            elif c == 10:                    # COMPRESSION_SELF_1
                last_self += 1
                c, offset = 5, last_self
            elif c == 11:                    # COMPRESSION_PARENT_SELF
                c, offset = 6, n * self.hunkbytes // self.unitbytes
                last_parent = offset
            elif c == 12:                    # COMPRESSION_PARENT_0
                c, offset = 6, last_parent
            elif c == 13:                    # COMPRESSION_PARENT_1
                last_parent += self.hunkbytes // self.unitbytes
                c, offset = 6, last_parent
            else:
                raise Fail("the .chd map is damaged (hunk type %d)" % c)
            if c == 5 and offset >= n:       # a copy must be of an earlier hunk (else reading it would never end)
                raise chd_damaged(self.path, ValueError("hunk %d is a copy of hunk %d" % (n, offset)))
            # the check sum is over the map as libchdr stores it: 24-bit lengths and 48-bit offsets (put_u24, put_u48)
            raw += (bytes([c]) + (length & 0xFFFFFF).to_bytes(3, "big") + (offset & 0xFFFFFFFFFFFF).to_bytes(6, "big")
                    + (crc & 0xFFFF).to_bytes(2, "big"))
            if c == 5 and out[offset][0] == 5:   # a copy of a copy: straight to the stored hunk, so reading never chains
                offset = out[offset][1]
            out.append((c, offset, length, crc))
        import binascii
        if binascii.crc_hqx(bytes(raw), 0xFFFF) != mapcrc:
            raise Fail("the .chd map is damaged (its check sum is wrong)")
        return out

    def _read_tracks(self, metaoffset):
        tracks, off, seen = [], metaoffset, set()
        while off:
            if off in seen:                      # a list that points back at an entry already read is damaged
                raise chd_damaged(self.path, ValueError("its track list loops"))
            if len(seen) >= 256:                 # a CD has at most 99 tracks
                raise chd_damaged(self.path, ValueError("its track list is too long"))
            seen.add(off)
            self.f.seek(off)
            mh = self.f.read(16)
            if len(mh) < 16:
                break
            tag, fl, nxt = mh[:4], struct.unpack_from(">I", mh, 4)[0], struct.unpack_from(">Q", mh, 8)[0]
            data = self.f.read(fl & 0xFFFFFF)
            if tag in (b"CHT2", b"CHTR"):
                d = dict(re.findall(r"([A-Z]+):(\S+)", data.split(b"\0")[0].decode("ascii", "replace")))
                tracks.append({"num": int(d["TRACK"]), "type": d["TYPE"], "frames": int(d["FRAMES"]),
                               "pregap": int(d.get("PREGAP", 0)), "pgtype": d.get("PGTYPE", "MODE1"),
                               "postgap": int(d.get("POSTGAP", 0))})
            off = nxt
        if not tracks:
            raise Fail("%s is not a CD image (it has no track list)" % os.path.basename(self.path))
        tracks.sort(key=lambda t: t["num"])
        start = 0
        for t in tracks:
            if t["type"] not in ("MODE1_RAW", "MODE2_RAW", "AUDIO"):
                raise Fail("%s has a %s track; this reads raw 2352-byte tracks (make it with chdman createcd from "
                           "your .cue and .bin)" % (os.path.basename(self.path), t["type"]))
            t["start"] = start
            start += (t["frames"] + 3) // 4 * 4    # each track's frames are padded to a multiple of 4 in the CHD
        return tracks

    def hunk(self, n):
        try:
            return self._hunk(n)
        except chd_errors() as e:
            raise chd_damaged(self.path, e)

    def _hunk(self, n):
        got = self._cache.get(n)
        if got is not None:
            return got
        c, offset, length, crc = self.map[n]
        if c is None:                            # never written: zeros
            data = bytes(self.hunkbytes)
        elif c == 5:
            data = self.hunk(offset)
        elif c == 6:
            raise Fail("%s needs a parent CHD; make a whole one with chdman" % os.path.basename(self.path))
        else:
            self.f.seek(offset)
            src = self.f.read(length)
            data = src if c == 4 else self._decompress(self.codecs[c], src)
            if crc is not None and c != 5:
                import binascii
                if binascii.crc_hqx(data, 0xFFFF) != crc:
                    raise Fail("%s is damaged: hunk %d does not match its check sum" % (os.path.basename(self.path), n))
        if len(self._cache) > 64:
            self._cache.clear()
        self._cache[n] = data
        return data

    def _decompress(self, codec, src):
        kind = CHD_CD_CODECS.get(codec)
        if kind not in ("lzma", "deflate", "flac"):
            raise Fail("%s uses %s compression, which this does not read. Make one with LZMA, Deflate and FLAC: "
                       "chdman createcd -c cdlz,cdzl,cdfl -i <your .cue> -o <new .chd>"
                       % (os.path.basename(self.path), kind or codec.decode("ascii", "replace")))
        frames = self.hunkbytes // CD_FRAME
        if kind == "flac":                 # the sectors as 16-bit big-endian stereo samples, then the subcode (Deflate)
            import zlib
            samples, end = flac_frames(src, frames * CD_SECTOR // 4)
            a = array.array("h", samples)
            if sys.byteorder == "little":
                a.byteswap()
            base = a.tobytes()
            sub = zlib.decompressobj(-15).decompress(src[end:], frames * 96).ljust(frames * 96, b"\0")
            return b"".join(base[i * CD_SECTOR:(i + 1) * CD_SECTOR] + sub[i * 96:(i + 1) * 96] for i in range(frames))
        clen = 2 if self.hunkbytes < 65536 else 3
        eccb = (frames + 7) // 8
        hdr = eccb + clen
        base_len = int.from_bytes(src[eccb:eccb + clen], "big")
        base_src, sub_src = src[hdr:hdr + base_len], src[hdr + base_len:]
        want = frames * CD_SECTOR
        if kind == "lzma":
            import lzma
            dsz = max(1 << 12, 1 << (want - 1).bit_length())
            base = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[
                {"id": lzma.FILTER_LZMA1, "dict_size": dsz, "lc": 3, "lp": 0, "pb": 2}]).decompress(base_src, want)
        else:
            import zlib
            base = zlib.decompressobj(-15).decompress(base_src, want)
        import zlib
        sub = zlib.decompressobj(-15).decompress(sub_src, frames * 96) if sub_src else bytes(frames * 96)
        if len(base) != want:
            raise Fail("%s is damaged: a hunk is too short" % os.path.basename(self.path))
        sub = sub.ljust(frames * 96, b"\0")
        out = bytearray()
        for i in range(frames):
            sector = bytearray(base[i * CD_SECTOR:(i + 1) * CD_SECTOR])
            if src[i >> 3] & (1 << (i & 7)):     # sync and error correction left out: they are put back
                sector[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
                cd_ecc_generate(sector)
            out += sector + sub[i * 96:(i + 1) * 96]
        return bytes(out)

    def frame(self, n):
        """Sector n (2352 bytes, no subcode) counted from the start of the CHD's frames."""
        per = self.hunkbytes // CD_FRAME
        h = self.hunk(n // per)
        i = n % per
        return h[i * CD_FRAME:i * CD_FRAME + CD_SECTOR]

    def data_sector(self, lba):
        """The 2048 data bytes of the first track's sector at lba, or b"" past its end."""
        t = self.tracks[0]
        if lba >= t["frames"]:
            return b""
        raw = self.frame(t["start"] + lba)
        return raw[24:24 + 2048] if raw[15] == 2 else raw[16:16 + 2048]


class ChdRaw:
    """A .chd's first track read like the raw .bin it was made from (seek and read), for a look at a few sectors."""
    def __init__(self, path):
        self.chd, self.pos = Chd(path), 0

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.chd.close()

    def seek(self, pos):
        self.pos = pos

    def read(self, n):
        t, out = self.chd.tracks[0], bytearray()
        while n > 0:
            lba, off = divmod(self.pos, CD_SECTOR)
            if lba >= t["frames"]:
                break
            part = self.chd.frame(t["start"] + lba)[off:off + n]
            out += part
            self.pos += len(part)
            n -= len(part)
        return bytes(out)


def is_chd(path):
    return bool(path) and path.lower().endswith(".chd")


def game_image(path):
    """The image the game itself is given: a .chd is written out to a .bin and .cue first, which the game reads."""
    return chd_written_out(path) if is_chd(path) else path


def chd_fingerprint(path):
    st = os.stat(path)
    return "%s|%d|%d" % (os.path.abspath(path), st.st_size, st.st_mtime_ns)


def chd_out_paths(path):
    """(folder, .cue, .bin) a .chd is written out to. The folder is named after the SHA-1 the CHD's header holds for
    its contents, so the same disc moved, renamed or copied again finds the copy it already has."""
    if not CHD_CACHE[0]:
        raise Fail("no folder to write the .chd out to")
    with open(path, "rb") as f:
        h = f.read(124)
    key = h[84:104].hex()[:16] if len(h) == 124 and h[:8] == b"MComprHD" and h[84:104].strip(b"\0") else \
        hashlib.sha1(chd_fingerprint(path).encode()).hexdigest()[:16]
    d = os.path.join(CHD_CACHE[0], key)
    return d, os.path.join(d, "disc.cue"), os.path.join(d, "disc.bin")


def chd_ready(path):
    """True when a .chd is written out already (playing it then starts at once)."""
    try:
        d, cue, binp = chd_out_paths(path)
    except (Fail, OSError):
        return False
    return os.path.isfile(os.path.join(d, "done")) and os.path.isfile(cue) and os.path.isfile(binp)


def chd_written_out(path):
    """The .cue that a .chd is written out to, writing it the first time: one .bin holding every track one after the
    other, audio little-endian as in a .bin (a CHD keeps it big-endian), and a .cue naming the tracks."""
    d, cue, binp = chd_out_paths(path)
    if os.path.isfile(os.path.join(d, "done")) and os.path.isfile(cue) and os.path.isfile(binp):
        return cue
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    c = Chd(path)
    try:
        lines, pos = ['FILE "%s" BINARY' % os.path.basename(binp)], 0
        with open(binp + ".tmp", "wb") as out:
            for t in c.tracks:
                typ = {"MODE1_RAW": "MODE1/2352", "MODE2_RAW": "MODE2/2352", "AUDIO": "AUDIO"}[t["type"]]
                lines.append("  TRACK %02d %s" % (t["num"], typ))
                stored_pregap = t["pgtype"].startswith("V") and t["pregap"]
                if t["pregap"] and not stored_pregap:
                    lines.append("    PREGAP %s" % cue_msf(t["pregap"]))
                if stored_pregap:
                    lines.append("    INDEX 00 %s" % cue_msf(pos))
                lines.append("    INDEX 01 %s" % cue_msf(pos + (t["pregap"] if stored_pregap else 0)))
                if t["postgap"]:
                    lines.append("    POSTGAP %s" % cue_msf(t["postgap"]))
                for i in range(t["frames"]):
                    s = c.frame(t["start"] + i)
                    if t["type"] == "AUDIO":     # big-endian samples back to little-endian
                        a = array.array("H", s)
                        a.byteswap()
                        s = a.tobytes()
                    out.write(s)
                pos += t["frames"]
        os.replace(binp + ".tmp", binp)
        with open(cue, "w", newline="\r\n") as f:
            f.write("\n".join(lines) + "\n")
        open(os.path.join(d, "done"), "w").close()
    finally:
        c.close()
    return cue


def cue_msf(frames):
    return "%02d:%02d:%02d" % (frames // 4500, frames // 75 % 60, frames % 75)


# ---------------------------------------------------------------- disc

def resolve_bin(path):
    """The .bin behind a .cue (or the .bin itself; for a .chd, the .bin it is written out to)."""
    if not path or not os.path.isfile(path):
        raise Fail("disc image not found: %s" % (path or "(none chosen)"))
    if is_chd(path):
        path = chd_written_out(path)
    if path.lower().endswith(".cue"):
        with open(path, encoding="utf-8", errors="replace") as f:
            m = re.search(r'^\s*FILE\s+"(.+?)"', f.read(), re.M | re.I)
        if not m:
            raise Fail("%s has no FILE line" % os.path.basename(path))
        d = os.path.dirname(os.path.abspath(path))
        b = os.path.join(d, m.group(1))
        if not os.path.isfile(b) and not WINDOWS:
            # file names are case-sensitive here, and a .cue often spells its .bin differently from the file
            # on disk: take the same name in another case (the game does the same; only ASCII letters fold)
            fold = lambda t: re.sub(r"[A-Z]", lambda c: c.group().lower(), t)
            same = [n for n in os.listdir(d) if fold(n) == fold(m.group(1)) and os.path.isfile(os.path.join(d, n))]
            if len(same) > 1:
                raise Fail("%s names %s, and %s all match it in different letter case: leave only one"
                           % (os.path.basename(path), m.group(1), ", ".join(same)))
            if same:
                b = os.path.join(d, same[0])
        if not os.path.isfile(b):
            raise Fail("%s names %s, which is not next to it" % (os.path.basename(path), m.group(1)))
        return b
    return path


def disc_fingerprint(path):
    if is_chd(path):                   # the .chd itself: writing it out is left for the build
        if not os.path.isfile(path):
            raise Fail("disc image not found: %s" % path)
        with open(path, "rb") as f:
            if f.read(8) != b"MComprHD":
                raise Fail("%s is not a CHD image" % os.path.basename(path))
        return chd_fingerprint(path)
    b = resolve_bin(path)
    st = os.stat(b)
    if st.st_size % 2352:
        raise Fail("%s is not a raw 2352-byte-sector image; the game needs a BIN/CUE or CHD dump (an .iso holds "
                   "no movie or XA sound sectors)" % os.path.basename(b))
    return "%s|%d|%d" % (os.path.abspath(b), st.st_size, st.st_mtime_ns)


# ---------------------------------------------------------------- generate

def regen_key(P, disc_path):
    pr = P.project
    g = get_profile(P)
    parts = [disc_fingerprint(disc_path), "regen2", P.gid]
    for rel in (os.path.join("tools", "recomp.py"), os.path.join("tools", "regen.py"), os.path.join("games", P.gid + ".json")):
        parts.append(file_bytes(os.path.join(pr, rel)))
    for n in ("main", "ai", "fr", "movie"):
        p = os.path.join(pr, g["extras"], "extra_%s.txt" % n)
        parts.append(file_bytes(p) if os.path.isfile(p) else b"")
    return key_of(*parts)


def generated_ok(P, C, disc_path):
    try:
        return (bool(disc_path) and project_ok(P) and os.path.isdir(P.gen)
                and C.get("regen_key") == regen_key(P, disc_path)
                and any(n.endswith(".c") for n in os.listdir(P.gen)))
    except (Fail, OSError):
        return False


def _lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return [l.rstrip("\r\n") for l in f]
    except OSError:
        return []


def generate(P, C, disc_path, log):
    if generated_ok(P, C, disc_path):
        log("generated code is up to date")
        return
    if not project_ok(P):
        raise Fail("the project is not downloaded, or is too old for this launcher (Update tab)")
    g = get_profile(P)
    have = identify_disc(P, C, disc_path)
    if have.lower() != g["exe"].lower():
        other = game_for_exe(P, have)
        raise Fail("this disc image is %s, not %s [%s]%s" % (
            dict((i, n) for i, n, _e in game_list(P)).get(other, have or "an unknown disc"), g["name"], g["serial"],
            "; choose it in the Game list" if other else ""))
    bin_path = resolve_bin(disc_path)
    key = regen_key(P, disc_path)           # also checks the image size
    py = P.tool_python
    C.set("regen_key", None)
    shutil.rmtree(P.disc, ignore_errors=True)
    os.makedirs(P.disc)
    log("reading the disc image")
    run([py, "-I", os.path.join(P.project, "tools", "extract_iso.py"), bin_path, P.disc], log, cwd=P.project)
    for need in [g["exe"], g["movie"]] + [o["file"] for o in g["overlays"].values()]:
        if not os.path.isfile(os.path.join(P.disc, need)):
            raise Fail("%s is not on this disc: wrong game or region (need %s, %s)" % (need, g["name"], g["serial"]))
    log("translating the game code (a minute or two)")
    run([py, "-I", os.path.join(P.project, "tools", "regen.py"), "--game", P.gid, P.disc, P.gen], log, cwd=P.project)
    shutil.rmtree(P.disc, ignore_errors=True)
    C.set("regen_key", key)
    log("code translated")


# ---------------------------------------------------------------- compile

def _sources(P):
    rt = os.path.join(P.project, "runtime")
    # one platform file goes with the shared runtime: Win32 on Windows, SDL2 elsewhere
    plat = "plat_win32.c" if WINDOWS else "plat_sdl.c"
    names = [n for n in sorted(os.listdir(rt)) if n.endswith(".c") and (not n.startswith("plat_") or n == plat)]
    if not WINDOWS and plat not in names:
        raise Fail("this version of the project has no Linux front end: use Update to get a newer one")
    src = [("rt", os.path.join(rt, n)) for n in names]
    src += [("gen", os.path.join(P.gen, n)) for n in sorted(os.listdir(P.gen)) if n.endswith(".c")]
    hdr = [os.path.join(rt, n) for n in sorted(os.listdir(rt)) if n.endswith(".h")]
    hdr += [os.path.join(P.gen, n) for n in sorted(os.listdir(P.gen)) if n.endswith(".h")]
    rla = os.path.join(P.project, "third_party", "rl_achievements")   # Recomp Achievements, in newer projects
    if os.path.isdir(rla):
        for sub in ("src", os.path.join("third_party", "cjson"), os.path.join("third_party", "rcheevos", "src", "runtime"),
                    os.path.join("third_party", "rcheevos", "src", "util")):
            d = os.path.join(rla, sub)
            src += [("ach", os.path.join(d, n)) for n in sorted(os.listdir(d)) if n.endswith(".c")]
        for sub in ("include", os.path.join("third_party", "cjson"), os.path.join("third_party", "rcheevos", "include"),
                    os.path.join("third_party", "rcheevos", "src", "runtime"), os.path.join("third_party", "rcheevos", "src"),
                    os.path.join("third_party", "rcheevos", "src", "util")):
            d = os.path.join(rla, sub)
            hdr += [os.path.join(d, n) for n in sorted(os.listdir(d)) if n.endswith(".h")]
    return src, hdr


def _flags(P, kind):
    # the same options CMakeLists.txt uses; the generated code is huge, so -O1 there
    f = ["-O3", "-DNDEBUG", "-std=gnu11", "-I", os.path.join(P.project, "runtime"), "-I", P.gen,
         "-Wall", "-Wno-unused-function", "-fno-strict-aliasing"]
    if kind == "gen":
        f += ["-O1", "-w"]
    rla = os.path.join(P.project, "third_party", "rl_achievements")
    if os.path.isdir(rla):   # runtime/achievements.c and the library itself (CMakeLists.txt does the same)
        f += ["-I", os.path.join(rla, "include")]
        if kind == "ach":
            f += ["-I", os.path.join(rla, "third_party", "rcheevos", "include"), "-I", os.path.join(rla, "third_party", "rcheevos", "src"),
                  "-I", os.path.join(rla, "third_party", "cjson"), "-DRC_DISABLE_LUA", "-w"]
    if not WINDOWS:
        f += ["-DDM_SDL"] + (["-I", P.sdl_inc] if P.portable else sdl_flags("--cflags"))
    return f


def build_key(P):
    src, hdr = _sources(P)
    h = hashlib.sha256()
    h.update(b"build1")
    for p in hdr:
        h.update(os.path.basename(p).encode())
        h.update(file_bytes(p))
    for _, p in src:
        h.update(os.path.basename(p).encode())
        h.update(file_bytes(p))
    return h.hexdigest()


def built_ok(P, C):
    try:
        return os.path.isfile(P.exe) and generated_dir_ok(P) and C.get("build_key") == build_key(P)
    except OSError:
        return False


def generated_dir_ok(P):
    return project_ok(P) and os.path.isdir(P.gen) and any(n.endswith(".c") for n in os.listdir(P.gen))


def compile_game(P, C, log, progress=None):
    if not toolchain_ok(P):
        raise Fail("the build tools are not downloaded yet (Update tab)")
    if built_ok(P, C):
        log("game is up to date")
        return
    src, hdr = _sources(P)
    hh = hashlib.sha256(b"".join(file_bytes(p) for p in hdr)).hexdigest()
    os.makedirs(P.obj, exist_ok=True)
    keyfile = os.path.join(P.obj, "keys.json")
    try:
        with open(keyfile) as f:
            keys = json.load(f)
    except (OSError, ValueError):
        keys = {}
    env = dict(os.environ)
    if WINDOWS:
        env["PATH"] = os.path.join(P.llvm, "bin") + os.pathsep + env.get("PATH", "")
    elif P.portable:   # keep Zig's caches with the other build files, not in the home folder
        env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(P.work, "zigcache")
        env["ZIG_LOCAL_CACHE_DIR"] = os.path.join(P.work, "zigcache")
    todo, objs, newkeys = [], [], {}
    for kind, p in src:
        name = os.path.basename(p)
        o = os.path.join(P.obj, name[:-2] + ".o")
        objs.append(o)
        k = key_of(hh, " ".join(_flags(P, kind)), " ".join(P.cc), file_bytes(p))
        newkeys[name] = k
        if not (os.path.isfile(o) and keys.get(name) == k):
            todo.append((kind, p, o))
    log("compiling %d of %d files%s" % (len(todo), len(src), " (the first build takes a few minutes)" if len(todo) > 20 else ""))
    done = [0]
    errors = []
    lock = threading.Lock()
    if todo and P.portable:    # the first compile builds Zig's C library files: do it once, before the parallel ones
        log("preparing the compiler (first time only)")
        warm = os.path.join(P.obj, "warmup.c")
        with open(warm, "w") as f:
            f.write("#include <stdio.h>\n#include <pthread.h>\nint main(void) { return puts(\"\") < 0; }\n")
        run(P.cc + ["-c", warm, "-o", warm + ".o"], lambda s: None, cwd=P.root, env=env)

    def one(t):
        kind, p, o = t
        if errors:
            return
        tmp = o + ".tmp"
        try:
            run(P.cc + ["-c", p, "-o", tmp] + _flags(P, kind), lambda s: None, cwd=P.root, env=env)
            os.replace(tmp, o)
        except Fail as e:
            errors.append(str(e))
        with lock:
            done[0] += 1
            if progress:
                progress(done[0], len(todo))
            if done[0] % 25 == 0 or done[0] == len(todo):
                log("  %d / %d" % (done[0], len(todo)))

    if todo:
        for _, _, o in todo:
            keys.pop(os.path.basename(o)[:-2] + ".c", None)
        with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 2)) as ex:
            list(ex.map(one, todo))
        if errors:
            raise Fail("compile error:\n" + errors[0])
    with open(keyfile, "w") as f:
        json.dump(newkeys, f)
    log("linking")
    os.makedirs(P.game, exist_ok=True)
    tmp = P.exe + ".new"
    if WINDOWS:
        libs = ["-static", "-Wl,--stack,16777216", "-lwinmm", "-lopengl32", "-lgdi32"]
    else:
        if P.portable:   # link against the system's libraries by exact name: it has no development symlinks
            libs = ["-l:libSDL2-2.0.so.0", "-l:libGL.so.1", "-lm", "-lpthread", "-Wl,--allow-shlib-undefined"]
            for d in LIB_DIRS:
                if os.path.isdir(d):
                    libs += ["-L" + d]
        else:
            libs = sdl_flags("--libs") + ["-lGL", "-lm", "-lpthread"]
    try:
        run(P.cc + objs + ["-o", tmp] + libs, log, cwd=P.root, env=env)
    except Fail as e:
        if WINDOWS:
            raise
        raise Fail("%s\nIf a library was not found, its development files are missing.\n%s" % (e, LINUX_HINT))
    try:
        os.replace(tmp, P.exe)
    except OSError:
        os.remove(tmp)
        raise Fail("%s is in use: close the game and build again" % EXE_FILE)
    C.set("build_key", build_key(P))
    log("game built: %s" % P.exe)


def write_out_chd(disc_path, log, starting=None):
    """A .chd not written out yet (never, or its copy in work/chd was deleted) is written out now, before the build.
    starting() is called first when there is something to do (prepare's progress line)."""
    if is_chd(disc_path) and not chd_ready(disc_path):
        if starting:
            starting()
        log("writing %s out to a .bin and .cue for the game (once; about a minute)" % os.path.basename(disc_path))
        resolve_bin(disc_path)


def build_all(P, C, disc_path, log, progress=None):
    if not disc_path:
        raise Fail("choose your disc image first")
    if not toolchain_ok(P) or not project_ok(P):
        raise Fail("download the build tools and project first (Update tab)")
    write_out_chd(disc_path, log)
    generate(P, C, disc_path, log)
    compile_game(P, C, log, progress)


# ---------------------------------------------------------------- settings (dmrecomp.ini)

KEYNAMES = ("up down left right enter backspace tab space lshift rshift lctrl rctrl lalt ralt "
            "comma period slash semicolon minus equals lbracket rbracket backslash quote grave "
            "escape insert delete home end pageup pagedown capslock "
            "num0 num1 num2 num3 num4 num5 num6 num7 num8 num9 numadd numsub nummul numdiv numdot numenter "
            "f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 none").split()
MAXKEYS = 4                                   # keys per pad button, as in the game
HOTKEYS = [("fullscreen", "f11", "Key: fullscreen"), ("scale", "f6", "Key: internal resolution"),
           ("filter", "f7", "Key: picture filter"), ("aspect", "f8", "Key: aspect ratio"),
           ("fsr", "f9", "Key: FSR upscaler"), ("widescreen", "f10", "Key: widescreen"),
           ("menu", "escape", "Key: in-game menu")]   # the menu while playing: display settings and Exit
PSNAMES = "up right down left start select cross circle square triangle l1 r1 l2 r2 l3 r3 none".split()
MOUSE_BUTTONS = [("mouse_left", "Mouse: left button", "cross"), ("mouse_right", "Mouse: right button", "triangle"),
                 ("mouse_middle", "Mouse: middle button", "start"), ("mouse_x1", "Mouse: side button 1", "none"),
                 ("mouse_x2", "Mouse: side button 2", "none"), ("wheel_up", "Mouse: wheel up", "up"),
                 ("wheel_down", "Mouse: wheel down", "down")]
# Tk key names (event.keysym) -> the game's key names, for the "press a key" button
TK_KEYS = {"Up": "up", "Down": "down", "Left": "left", "Right": "right", "Return": "enter", "BackSpace": "backspace",
           "Tab": "tab", "space": "space", "Shift_L": "lshift", "Shift_R": "rshift", "Control_L": "lctrl",
           "Control_R": "rctrl", "Alt_L": "lalt", "Alt_R": "ralt", "comma": "comma", "period": "period",
           "slash": "slash", "semicolon": "semicolon", "minus": "minus", "equal": "equals", "bracketleft": "lbracket",
           "bracketright": "rbracket", "backslash": "backslash", "apostrophe": "quote", "grave": "grave",
           "Escape": "escape", "Insert": "insert", "Delete": "delete", "Home": "home", "End": "end", "Prior": "pageup",
           "Next": "pagedown", "Caps_Lock": "capslock", "KP_Add": "numadd", "KP_Subtract": "numsub",
           "KP_Multiply": "nummul", "KP_Divide": "numdiv", "KP_Decimal": "numdot", "KP_Enter": "numenter"}
for _i in range(10):
    TK_KEYS["KP_%d" % _i] = "num%d" % _i
for _i in range(1, 13):
    TK_KEYS["F%d" % _i] = "f%d" % _i


def key_name_from_tk(keysym):
    """The game's name for a Tk key press, or None if the game has no such key."""
    if len(keysym) == 1 and keysym.isascii() and keysym.isalnum():
        return keysym.upper()
    return TK_KEYS.get(keysym)
PADNAMES = "a b x y lb rb lt rt start back ls rs up down left right none".split()
B01 = ["0", "1"]

# (key, tab, label, kind, choices, default, comment)
SETTINGS = [
    ("scale", "Video", "Internal 3D resolution", "choice", ["1", "2", "4"], "1",
     "Internal 3D resolution: 1 = original, 2 or 4 = sharper (F6 cycles while playing). The CPU draws it: 4 is 16 times the work, so use 2 on a laptop"),
    ("scale_auto", "Video", "Lower it when too slow", "bool", B01, "1",
     "1 = drop to a lower internal resolution by itself when the game cannot hold full speed at 2 or 4"),
    ("renderer", "Video", "Graphics API", "choice", ["auto", "vulkan", "gl"], "auto",
     "Graphics API that shows the picture: auto = Vulkan when it can do everything asked, else OpenGL; "
     "vulkan = Vulkan first (falls back to OpenGL); gl = OpenGL"),
    ("gpu_gl", "Video", "Draw 2x / 4x on the graphics card (early prototype)", "bool", B01, "0",
     "1 = at scale 2 or 4 the graphics card draws the picture instead of the processor (early prototype)"),
    ("filter", "Video", "Picture filter", "choice", ["nearest", "smooth"], "nearest",
     "How the picture is enlarged to the window: nearest = sharp pixels, smooth = blended (F7)"),
    ("aspect", "Video", "Aspect ratio", "choice", ["4:3", "stretch"], "4:3",
     "4:3 = console shape with black bars, stretch = fill the window (F8)"),
    ("integer_scale", "Video", "Whole-number scaling only", "bool", B01, "0",
     "1 = only enlarge by whole multiples of 240 lines"),
    ("fullscreen", "Video", "Start in fullscreen", "bool", B01, "0",
     "1 = start in fullscreen (F11 or Alt+Enter toggles)"),
    ("window_size", "Video", "Window size", "combo", ["auto", "640x480", "800x600", "960x720", "1280x960", "1600x1200",
                                                      "1280x720", "1600x900", "1920x1080"], "auto",
     "Window size in pixels (WxH), e.g. 1280x960. auto = 960x720"),
    ("fullscreen_size", "Video", "Fullscreen resolution", "combo", ["native", "1024x768", "1280x960", "1280x720",
                                                                     "1600x1200", "1920x1080", "2560x1440", "3840x2160"], "native",
     "Fullscreen resolution (WxH) or native = the desktop's own resolution"),
    ("widescreen", "Video", "Widescreen (experimental)", "choice", ["off", "16:9", "16:10", "21:9"], "off",
     "Widescreen (experimental): off, or the shape such as 16:9, 16:10, 21:9. Shows more scenery at the sides in 3D scenes (F10 cycles)"),
    ("upscale", "Video", "Upscaler", "choice", ["off", "fsr"], "off",
     "off = plain, fsr = AMD FidelityFX Super Resolution 1 (F9 toggles); fsr_sharpness 0..100"),
    ("fsr_sharpness", "Video", "FSR sharpness (0-100)", "combo", ["0", "25", "50", "80", "100"], "80", None),
    ("skip_intro", "Gameplay", "Skip the intro movies", "bool", B01, "1",
     "1 = skip the movies shown when the game starts (the publisher, the developer and the intro), 0 = show them"),
    ("cpu_assist", "Gameplay", "Hold 30 fps in busy scenes", "bool", B01, "1",
     "1 = hold 30 fps in busy scenes, 0 = console CPU timing with its slowdowns"),
    ("volume", "Audio", "Volume (percent)", "choice", ["0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"], "100",
     "Sound volume in percent, 0 to 100 (also in the menu while playing)"),
    ("reverb", "Audio", "Console echo effect (reverb)", "bool", B01, "1",
     "1 = echo effect the console adds in levels, 0 = dry sound"),
    ("dualshock", "Gameplay", "DualShock pad (sticks, rumble)", "bool", B01, "1",
     "Controllers. dualshock = 0 makes the game see a plain digital pad (no sticks, no rumble)."),
    ("rumble", "Gameplay", "Rumble", "bool", B01, "1", None),
    ("tap_hold", "Gameplay", "Keep quick key taps", "bool", B01, "1",
     "1 = a quick key tap always reaches the game (each press lasts 4 frames at least), 0 = keys exactly as held"),
    ("player2", "Gameplay", "Second gamepad is player 2", "bool", B01, "1",
     "1 = a second gamepad, when plugged in, is player 2"),
]
PADS = [("up", "up"), ("right", "right"), ("down", "down"), ("left", "left"), ("start", "start"), ("select", "back"),
        ("cross", "a"), ("circle", "b"), ("square", "x"), ("triangle", "y"), ("l1", "lb"), ("r1", "rb"),
        ("l2", "lt"), ("r2", "rt"), ("l3", "ls"), ("r3", "rs")]
KEYS = [("up", "up"), ("right", "right"), ("down", "down"), ("left", "left"), ("start", "enter"),
        ("select", "backspace, rshift"), ("cross", "Z, K"), ("circle", "X, L"), ("square", "A, J"),
        ("triangle", "S, I"), ("l1", "Q"), ("r1", "W"), ("l2", "1"), ("r2", "3")]
for _n, _d in PADS:
    SETTINGS.append(("pad_" + _n, "Input", "Pad: " + _n.upper(), "choice", PADNAMES, _d, None))
SETTINGS.append(("pad_menu", "Input", "Pad: in-game menu", "choice",
                 ["back+start", "ls+rs", "lb+rb", "lt+rt", "back", "start", "ls", "rs", "none"], "back+start",
                 "The gamepad controls that open the menu while playing: one, or two held together (ls+rs = both sticks pressed). "
                 "Other pairs can be typed into dmrecomp.ini"))
for _n, _d in KEYS:
    SETTINGS.append(("key_" + _n, "Input", "Key: " + _n.upper(), "key", None, _d, None))
SETTINGS.append(("key_fast_forward", "Input", "Key: fast-forward", "key", None, "tab", None))
for _n, _d, _lab in HOTKEYS:
    SETTINGS.append(("key_" + _n, "Input", _lab, "key", None, _d, None))
for _k, _lab, _d in MOUSE_BUTTONS:
    SETTINGS.append((_k, "Input", _lab, "choice", PSNAMES, _d, None))
SCHEMA = {s[0]: s for s in SETTINGS}
SETTING_TABS = ["Gameplay", "Video", "Input", "Audio", "Debug", "All settings"]


def default_ini_text():
    out = ["# dmrecomp settings. Delete this file to get the defaults back.",
           "# The defaults give the original console picture.", ""]
    for key, _tab, _lab, _kind, _ch, dflt, comment in SETTINGS:
        if comment:
            out.append("# " + comment)
        out.append("%s = %s" % (key, dflt))
    return "\n".join(out) + "\n"


_INI_LINE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$")


def read_ini(path, strict=False):
    """Settings in the file (name -> value); the defaults for anything missing. A file that is not there gives the
    defaults, as the game sees it; one that cannot be read does too, unless strict: then OSError is raised."""
    vals = {s[0]: s[5] for s in SETTINGS}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                if not s or s[0] in "#;":
                    continue
                m = _INI_LINE.match(line)
                if m:
                    vals[m.group(1).lower()] = m.group(2)
    except FileNotFoundError:
        pass
    except OSError:
        if strict:
            raise
    return vals


def valid_setting(key, val):
    s = SCHEMA.get(key)
    if not s:
        return True
    if key == "volume":                 # as the game takes them: more than the lists here offer
        return val.isdecimal() and val.isascii() and 0 <= int(val) <= 100
    if key == "pad_menu":               # one control, or two different ones joined by +
        parts = [p.strip().lower() for p in val.split("+")]
        if len(parts) == 1:
            return parts[0] in PADNAMES
        return len(parts) == 2 and parts[0] != parts[1] and all(p in PADNAMES and p != "none" for p in parts)
    if s[3] in ("choice", "bool"):
        return val.lower() in [c.lower() for c in s[4]]
    if s[3] == "combo":
        if key == "fsr_sharpness":
            return val.isdecimal() and val.isascii() and 0 <= int(val) <= 100
        m = re.fullmatch(r"(\d{3,5})x(\d{3,5})", val)   # the game wants a lower-case x and at most 16384
        return val.lower() == s[5] or bool(m and 320 <= int(m.group(1)) <= 16384 and 240 <= int(m.group(2)) <= 16384)
    parts = [p.strip() for p in val.split(",")]
    if not 1 <= len(parts) <= (1 if key == "key_fast_forward" or key[4:] in [h[0] for h in HOTKEYS] else MAXKEYS):
        return False
    return all((len(p) == 1 and p.isalnum() and p.isascii()) or p.lower() in KEYNAMES for p in parts)


def write_ini(path, values):
    """Write values into the file, keeping its comments, order and unknown lines."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        lines = default_ini_text().splitlines()
    todo = dict(values)
    out = []
    for line in lines:
        m = _INI_LINE.match(line)
        if m and not line.lstrip().startswith(("#", ";")) and m.group(1).lower() in todo:
            k = m.group(1).lower()
            out.append("%s = %s" % (m.group(1), todo.pop(k)))
        else:
            out.append(line)
    for k, v in todo.items():
        out.append("%s = %s" % (k, v))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    os.replace(tmp, path)


# ---------------------------------------------------------------- play

DEBUG_FLAGS = [("--stats", "Print speed statistics (flips = frames per second)"),
               ("--log-bios", "Log BIOS and disc commands"),
               ("--log-sio", "Log controller and memory-card traffic"),
               ("--turbo", "Run without frame limiting (testing)")]


def _log_lock(f):
    """Take the lock that says "a game is writing this log". Windows needs none: it refuses to move or replace a file
    that a running game has open. Elsewhere an open file can be moved and deleted, so the launcher locks the log it
    hands to the game; the game inherits the lock and holds it until it ends. Raises OSError when it is held. On a file
    system that has no locks the launcher goes on without one: a log that two games could share is better than none."""
    if not WINDOWS:
        import errno
        import fcntl
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                raise


SAVE_FILES = ("memcard1.mcd", "memcard2.mcd", "dmrecomp.ini")


def bring_saves(P, log):
    """With --data, the game's own files live in DIR/<game>. The first time, the memory cards and settings the game
    already has in game/<game> are copied there (copied, never moved: the originals stay). A file already in DIR/<game>
    is never replaced."""
    os.makedirs(P.home, exist_ok=True)
    for n in SAVE_FILES:
        src, dst = os.path.join(P.game, n), os.path.join(P.home, n)
        if os.path.isfile(src) and not os.path.exists(dst):
            tmp = dst + ".copying"
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
            log("copied %s to %s (the one in %s stays)" % (n, P.home, P.game))


def play(P, C, disc_path, log, extra=(), wait=False):
    """Start the built game. extra: more arguments for the game (after the disc). wait: stay until the game ends and
    return its exit code (the run command, for a launcher that watches the process); otherwise return at once."""
    if not os.path.isfile(P.exe):
        raise Fail("the game is not built yet (Build tab)")
    if not disc_path:
        raise Fail("choose your disc image first (Build tab)")
    resolve_bin(disc_path)
    args = [P.exe, os.path.abspath(game_image(disc_path))] + [f for f, _ in DEBUG_FLAGS if f in C.get("debug_flags", [])] + list(extra)
    flags = NEW_CONSOLE if C.get("debug_console", False) else NO_WINDOW
    if P.home != P.game:
        bring_saves(P, log)
    # Without the console window, what the game says goes to a file in its folder: if the game ever stops, the stall
    # watchdog's report is in there. The run before is kept too, because the first thing done after a freeze is to start again.
    out = None
    if not (WINDOWS and C.get("debug_console", False)):   # the console window exists on Windows only
        cur, old = os.path.join(P.home, RUN_LOG), os.path.join(P.home, RUN_LOG_BEFORE)
        try:
            if os.path.isfile(cur):
                if WINDOWS:
                    os.replace(cur, old)       # fails when a game still writes it
                else:
                    with open(cur, "rb") as held:
                        _log_lock(held)        # fails when a game still writes it
                        # another launcher may have moved this file away between the open and the lock: then the name
                        # now belongs to its new log, which must not be moved
                        if os.fstat(held.fileno()).st_ino != os.stat(cur).st_ino:
                            raise OSError("another launcher started a game at the same moment")
                        os.replace(cur, old)   # moved while the lock is held, so no other launcher moves it too
            # The name is free now: never there, or just moved away. So the log is made new, and only if the name is still
            # free (O_EXCL). When another launcher made its log there in this moment, that log is not opened or emptied.
            # Known gap, not Windows: between this open and the lock below the new log has no lock, and a launcher that
            # starts in those microseconds moves it to the name of the log before, whose own content is then lost.
            out = os.fdopen(os.open(cur, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o644), "wb")
            _log_lock(out)
            out.write(("[launcher] version %s started the game at %s\n" % (LAUNCHER_VERSION, time.strftime("%Y-%m-%d %H:%M:%S"))).encode())
            out.flush()
        except OSError as e:           # a game still running keeps its log: this run goes without one
            log("no run log this time: %s" % e)
            if out:
                out.close()
            out = None
    try:
        if out:
            proc = subprocess.Popen(args, cwd=P.home, creationflags=flags, stdin=subprocess.DEVNULL, stdout=out, stderr=out,
                                    env=child_env())
        else:
            proc = subprocess.Popen(args, cwd=P.home, creationflags=flags, env=child_env())
    finally:
        if out:
            out.close()                # the game has its own handle now
    log("started the game" + (" (its messages go to %s)" % os.path.join(P.home, RUN_LOG) if out else ""))
    if not wait:
        return None
    code = proc.wait()
    if out and P.data:   # RecompLauncher sends the newest .log at the top of its data folder with a crash report
        try:
            shutil.copyfile(os.path.join(P.home, RUN_LOG), os.path.join(P.data, RUN_LOG))
        except OSError as e:
            log("could not copy %s to %s: %s" % (RUN_LOG, P.data, e))
    return code


def desktop_entry_text(P, disc_path, name):
    """A freedesktop .desktop file that starts the built game from its own folder."""
    def q(t):   # Exec quoting: double quotes, with the few special characters escaped
        quoted = '"' + "".join("\\" + c if c in '"`$\\' else c for c in t) + '"'
        return quoted.replace("\\", "\\\\").replace("%", "%%")   # the file's own string escaping comes first, so every backslash doubles; % starts a field code in Exec
    return ("[Desktop Entry]\nType=Application\nName=%s (recomp)\nComment=Static recompilation of the PlayStation game\n"
            "Exec=%s %s\nPath=%s\nTerminal=false\nCategories=Game;\nStartupNotify=false\n"
            % (name, q(P.exe), q(os.path.abspath(disc_path)), P.game))   # Path= takes no field codes: no % escaping there


def make_shortcut(P, C, log):
    """Linux: put the built game in the applications menu. On the Steam Deck that is what makes it available
    to 'Add a Non-Steam Game' (Steam > Games > Add a Non-Steam Game to My Library), and so to Game Mode."""
    if WINDOWS:
        raise Fail("this is for Linux; on Windows the game starts from the launcher")
    if not os.path.isfile(P.exe):
        raise Fail("the game is not built yet (Build tab)")
    disc = C.get("disc")
    if not disc:
        raise Fail("choose your disc image first (Build tab)")
    resolve_bin(disc)
    name = dict((i, n) for i, n, _e in game_list(P)).get(P.gid, P.gid)
    d = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share"), "applications")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "dmrecomp-%s.desktop" % P.gid)
    with open(path, "w", encoding="utf-8") as f:
        f.write(desktop_entry_text(P, game_image(disc), name))
    os.chmod(path, 0o755)
    log("added \"%s (recomp)\" to the applications menu: %s" % (name, path))
    log("On the Steam Deck: in Steam choose Games > Add a Non-Steam Game to My Library, tick it, then add it. "
        "Game Mode uses the controller as a normal gamepad, and the game starts fullscreen there.")


# ---------------------------------------------------------------- disc folder

EXE_NAME = re.compile(r"^S[A-Z]{3}_\d{3}\.\d{2}$")


def iso_root_files(bin_path):
    """File names in the root directory of a raw 2352-byte-sector PS1 image (or of a .chd's first track)."""
    with (ChdRaw(bin_path) if is_chd(bin_path) else open(bin_path, "rb")) as f:
        def sector(lba):
            f.seek(lba * 2352)
            raw = f.read(2352)
            if len(raw) < 2352:
                return b""
            return raw[24:24 + 2048] if raw[15] == 2 else raw[16:16 + 2048]
        pvd = sector(16)
        if pvd[1:6] != b"CD001" or len(pvd) < 190:
            return []
        lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        if not 0 < size <= 1 << 20:
            return []
        data = b"".join(sector(lba + i) for i in range((size + 2047) // 2048))[:size]
    names, p = [], 0
    while p + 34 < len(data):
        n = data[p]
        if n == 0:
            p = (p // 2048 + 1) * 2048
            continue
        k = data[p + 32]
        names.append(data[p + 33:p + 33 + k].decode("ascii", "replace").split(";")[0])
        p += n
    return names


def identify_disc(P, C, path):
    """The game executable's name on this disc (e.g. SLUS_010.26), or "" if it has none. Cached per image."""
    fp = disc_fingerprint(path)
    with C.lock:
        cached = C.d.get("ident", {}).get(fp)
    if cached is None:
        exes = [n for n in iso_root_files(path if is_chd(path) else resolve_bin(path)) if EXE_NAME.match(n)]
        cached = exes[0] if exes else ""
        with C.lock:
            C.d.setdefault("ident", {})[fp] = cached
            C.save()
    return cached


def game_for_exe(P, exe):
    for gid, _n, e in game_list(P):
        if e.lower() == exe.lower():
            return gid
    return None


def find_discs(P, C):
    """{game id: image path} for the images in the "Dave Mirra BMX" folder (or one folder inside it)."""
    found = {}
    try:
        os.makedirs(P.discdir, exist_ok=True)
        cands = {".cue": [], ".bin": [], ".chd": []}
        for dirpath, dirs, files in os.walk(P.discdir):
            if os.path.relpath(dirpath, P.discdir).count(os.sep) >= 1:
                dirs[:] = []
            for n in sorted(files):
                ext = os.path.splitext(n)[1].lower()
                if ext in cands:
                    cands[ext].append(os.path.join(dirpath, n))
        for path in cands[".cue"] + cands[".bin"] + cands[".chd"]:
            try:
                gid = game_for_exe(P, identify_disc(P, C, path))
            except (Fail, OSError):
                continue
            if gid and gid not in found:
                found[gid] = path
    except OSError:
        pass
    return found


def select_game(P, C, gid):
    P.use(gid)
    C.use(gid)


def adopt_disc(P, C, switch=False):
    """Use the images in the disc folder for any game that has no working one chosen. True if the selected game's changed.
    With switch, a selected game that has no image gives way to a game that has one. That is only for a first run,
    before any game has been chosen; a game chosen with --game, the game list or Browse is never given up."""
    changed = False
    found = find_discs(P, C)
    for gid, path in found.items():
        cur = C.gget(gid, "disc")
        try:
            if cur:
                disc_fingerprint(cur)
                continue
        except Fail:
            pass
        C.gset(gid, "disc", os.path.abspath(path))
        changed = changed or gid == P.gid
    if switch and found and not C.get("disc"):          # the selected game has no image but another game does
        select_game(P, C, next(iter(found)))
        changed = True
    return changed


def migrate_layout(P):
    """Move what an older launcher left directly under work/ and game/ into the first game's folders.
    Memory cards go first and the move stops at the first failure, so saves are never stranded behind a new blank card.
    Returns a message if something could not be moved, else None."""
    first = Paths(P.root, "dmbmx")
    moves = [(os.path.join(P.root, "game", n), os.path.join(first.game, n)) for n in ("memcard1.mcd", "memcard2.mcd", "dmrecomp.ini")]
    moves += [(os.path.join(P.root, "game", "dmrecomp.exe"), first.exe),
              (os.path.join(P.work, "gen"), first.gen), (os.path.join(P.work, "obj"), first.obj)]
    for src, dst in moves:
        if src.endswith(".mcd") and os.path.exists(src) and os.path.exists(dst):
            return "there is a memory card at both %s and %s; compare them and keep the one with your saves in %s" % (src, dst, dst)
        if os.path.exists(src) and not os.path.exists(dst):
            try:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.move(src, dst)
            except OSError as e:
                return "could not move %s to %s (%s); close anything using it and start the launcher again" % (src, dst, e)
    return None


# ---------------------------------------------------------------- status

STATUS_KEYS = ("tools", "project", "disc", "chd", "gen", "built")   # every row status() can give, in order


def status(P, C):
    """(key, label, ok, tab_to_fix_it) for each requirement; the "chd" row only for a .chd disc."""
    disc = C.get("disc")
    disc_ok = False
    try:
        if disc:
            disc_fingerprint(disc)
            want = dict((i, e) for i, _n, e in game_list(P)).get(P.gid, "")
            disc_ok = not want or identify_disc(P, C, disc).lower() == want.lower()
    except (Fail, OSError):
        pass
    rows = [
        ("tools", "Build tools", toolchain_ok(P), "Update"),
        ("project", "Project files", project_ok(P), "Update"),
        ("disc", "Disc image", disc_ok, "Build"),
        ("gen", "Game code generated", generated_ok(P, C, disc) if disc_ok else False, "Build"),
        ("built", "Game built", built_ok(P, C) if disc_ok else os.path.isfile(P.exe), "Build"),
    ]
    if is_chd(disc):   # the game reads a .bin and .cue: the .chd must be written out (Build does it)
        rows.insert(3, ("chd", "Disc written out for the game", disc_ok and chd_ready(disc), "Build"))
    return rows


def ready_to_play(P, C):
    st = {k: ok for k, _l, ok, _t in status(P, C)}
    # a .chd not written out yet would be written out on Play, in the window's own thread: Build does it instead
    return os.path.isfile(P.exe) and st["disc"] and st.get("chd", True)


# ---------------------------------------------------------------- command line

def open_session(root, disc=None, game=None):
    """Paths and Config for a run. The game is --game if given, else the game the --disc image is, else the
    remembered one; the disc is remembered for it. Returns (P, C, note); raises Fail for a bad game or image."""
    P = Paths(root or default_root())
    C = Config(P.cfg)
    first_run = "game" not in C.d       # no game has ever been chosen
    note = migrate_layout(P)
    P.use(C.game)
    ids = [i for i, _n, _e in game_list(P)]
    if game:
        if game not in ids:
            raise Fail("unknown game \"%s\"; try: %s" % (game, ", ".join(ids)))
        select_game(P, C, game)
    if disc:
        try:
            gid = game_for_exe(P, identify_disc(P, C, disc))
        except OSError as e:
            raise Fail(str(e))
        if gid and not game:
            select_game(P, C, gid)
        C.set("disc", os.path.abspath(disc))
    else:
        adopt_disc(P, C, switch=first_run and not game)
    return P, C, note


PREPARE_GAMES = ("dmbmx", "maxremix")


def prepare(root, argv):
    """The setup step for RecompLauncher (recomplauncher.json): no window, nothing asked. --dmbmx=FILE and
    --maxremix=FILE are the disc images (.cue) to build those games from; --accept-downloads allows downloading the
    build tools and the project's source, which the player agreed to on the setup screen. Prints lines
    "progress <0 to 1> <text>". Returns 0 when every game given is built (or already was), 3 when the downloads are
    needed but not allowed; a failure raises Fail."""
    picks, accept = [], False
    for a in argv:
        name, eq, value = a[2:].partition("=") if a.startswith("--") else ("", "", "")
        if a == "--accept-downloads":
            accept = True
        elif eq and name in PREPARE_GAMES and value:
            picks.append((name, value))
        else:
            raise Fail("prepare: not understood: %s (use --dmbmx=FILE, --maxremix=FILE, --accept-downloads)" % a)
    if not picks:
        raise Fail("prepare: no disc image given (--dmbmx=FILE or --maxremix=FILE)")
    lock = threading.Lock()

    def say(f, text):
        with lock:
            print("progress %.3f %s" % (min(max(f, 0.0), 1.0), text), flush=True)
    log = lambda s: print(s, flush=True)
    P, C, _note = open_session(root, picks[0][1], picks[0][0])
    if not (toolchain_ok(P) and project_ok(P)):
        if not accept:
            print("the build tools and the project are not downloaded yet, and --accept-downloads was not given", flush=True)
            return 3
        say(0.01, "Downloading the C compiler")
        install_toolchain(P, log)
        say(0.25, "Downloading the project")
        install_project(P, C, log, want=release_tag())
    elif accept:   # a packaged release builds its own project version, also when the player goes back to an older one
        want = release_tag()
        say(0.25, ("Getting project version %s" % want) if want else "Checking for a newer project version")
        install_project(P, C, log, want=want)
    names = dict((i, n) for i, n, _e in game_list(P))
    span = 0.7 / len(picks)
    for i, (gid, disc) in enumerate(picks):
        P, C, _note = open_session(root, disc, gid)
        lo, name = 0.3 + i * span, names.get(gid, gid)
        write_out_chd(disc, log, lambda: say(lo, "Writing out your disc image for %s" % name))
        say(lo, "Translating %s from your disc" % name)
        generate(P, C, disc, log)
        say(lo + span * 0.2, "Building %s" % name)
        compile_game(P, C, log, lambda d, n, lo=lo, name=name:
                     say(lo + span * (0.2 + 0.8 * d / max(n, 1)), "Building %s (%d of %d files)" % (name, d, n)))
    say(1, "Ready to play")
    return 0


def cli(argv):
    if argv[:1] == ["prepare"] or (argv[:2] and argv[0] == "--root" and argv[2:3] == ["prepare"]):
        root = argv[1] if argv[0] == "--root" else None
        try:
            return prepare(root, argv[3:] if argv[0] == "--root" else argv[1:])
        except (Fail, OSError) as e:
            print("error: %s" % e, flush=True)
            return 1
    root, disc, game, data, cmd = None, None, None, None, []
    it = iter(argv)
    for a in it:
        if a == "--root":
            root = next(it, None)
        elif a == "--disc":
            disc = next(it, None)
        elif a == "--game":
            game = next(it, None)
        elif a == "--data":
            data = next(it, None)
        else:
            cmd.append(a)
    try:
        P, C, note = open_session(root, disc, game)
    except Fail as e:
        print("error: %s" % e)
        return RUN_NOT_STARTED if cmd[:1] == ["run"] else 1   # run: the game did not start (a disc that has moved)
    if data:                           # the game's settings, memory cards and logs in DATA/<game> (RecompLauncher's {data})
        P.data = os.path.abspath(data)
        P.use(P.gid)
    if note:
        print("warning: %s" % note)
    log = lambda s: print(s, flush=True)
    what = cmd[0] if cmd else "status"
    try:
        if what == "games":
            for gid, name, exe in game_list(P):
                print("%s %-9s %s [%s]" % ("*" if gid == P.gid else " ", gid, name, exe))
        elif what == "status":
            print("game: %s" % dict((i, n) for i, n, _e in game_list(P)).get(P.gid, P.gid))
            for _k, label, ok, tab in status(P, C):
                print("%s %s%s" % ("OK " if ok else "-- ", label, "" if ok else "   (%s tab)" % tab))
            print("project version: %s" % C.get("tag", "none"))
        elif what == "download":
            install_toolchain(P, log)
            install_project(P, C, log)
        elif what == "update":
            install_project(P, C, log)
        elif what == "update-launcher":
            update_launcher(P, C, log)
        elif what == "build":
            build_all(P, C, C.get("disc"), log)
        elif what == "play":
            play(P, C, C.get("disc"), log)
        elif what == "run":            # like play, but stays until the game ends; the words after it go to the game
            try:
                return play(P, C, C.get("disc"), log, extra=cmd[1:], wait=True) or 0
            except (Fail, OSError) as e:   # the game did not start: 4, so it is not taken for the game's own exit 1
                print("error: %s" % e)
                return RUN_NOT_STARTED
        elif what == "shortcut":
            make_shortcut(P, C, log)
        else:
            print(__doc__)
            return 2
    except (Fail, OSError) as e:
        print("error: %s" % e)
        return 1
    return 0


# ---------------------------------------------------------------- window

LIGHT = dict(bg="#f0f0f0", fg="#000000", field="#ffffff", btn="#e1e1e1", btn_hot="#d0d0d0", line="#adadad",
             muted="#6b6b6b", warn="#b36b00", ok="#1a7f37", bad="#c62828", accent="#0078d7")
DARK = dict(bg="#1e1f22", fg="#e6e6e6", field="#2b2d31", btn="#3a3c42", btn_hot="#4a4d55", line="#4a4d55",
            muted="#9a9da5", warn="#e0a030", ok="#4cc366", bad="#ff6b6b", accent="#3b82f6")


def system_prefers_dark():
    """Windows' own app theme (Settings > Personalization > Colors); light elsewhere."""
    if not WINDOWS:
        return False
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as k:
            return winreg.QueryValueEx(k, "AppsUseLightTheme")[0] == 0
    except OSError:
        return False

def gui(root_dir, pinned=False, notice=None):
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except ImportError:    # some Linux systems package Tk apart from Python
        print("The launcher window needs Tkinter, which this Python does not have.\n"
              "  Debian/Ubuntu: sudo apt install python3-tk     Fedora: sudo dnf install python3-tkinter     Arch: sudo pacman -S tk\n"
              "Or use it without a window:  python3 dmlauncher.py [--disc <image.cue>] status | download | build | play")
        return 1
    import queue

    P = Paths(root_dir)
    C = Config(P.cfg)
    first_run = "game" not in C.d       # no game has ever been chosen
    migrate_note = migrate_layout(P)
    P.use(C.game)
    win = tk.Tk()
    win.title(APP_NAME)
    set_window_icon(win, tk)
    win.geometry("760x640")
    win.minsize(640, 520)
    q = queue.Queue()
    busy = threading.Event()

    def log(s):
        q.put(("log", s))

    style = ttk.Style(win)
    native_theme = style.theme_use()
    plain = []          # classic tk widgets that ttk's theme does not colour
    pal = dict(LIGHT)

    def apply_theme(dark):
        pal.update(DARK if dark else LIGHT)
        if dark:
            style.theme_use("clam")
            style.configure(".", background=pal["bg"], foreground=pal["fg"], fieldbackground=pal["field"],
                            bordercolor=pal["line"], lightcolor=pal["bg"], darkcolor=pal["bg"],
                            troughcolor=pal["field"], insertcolor=pal["fg"], focuscolor=pal["bg"])
            style.map(".", foreground=[("disabled", pal["muted"])])
            style.configure("TButton", background=pal["btn"], padding=(8, 3))
            style.map("TButton", background=[("active", pal["btn_hot"]), ("disabled", pal["bg"])])
            style.configure("TNotebook", background=pal["bg"])
            style.configure("TNotebook.Tab", background=pal["btn"], foreground=pal["fg"], padding=(10, 3))
            style.map("TNotebook.Tab", background=[("selected", pal["bg"])])
            style.configure("TCombobox", arrowcolor=pal["fg"], selectbackground=pal["field"], selectforeground=pal["fg"])
            style.map("TCombobox", fieldbackground=[("readonly", pal["field"])], foreground=[("readonly", pal["fg"])])
            style.configure("TCheckbutton", indicatorbackground=pal["field"], indicatorforeground=pal["fg"])
            style.configure("TProgressbar", background=pal["accent"])
            win.option_add("*TCombobox*Listbox.background", pal["field"])
            win.option_add("*TCombobox*Listbox.foreground", pal["fg"])
            win.option_add("*TCombobox*Listbox.selectBackground", pal["accent"])
        else:
            style.theme_use(native_theme)
            win.option_add("*TCombobox*Listbox.background", "white")
            win.option_add("*TCombobox*Listbox.foreground", "black")
            win.option_add("*TCombobox*Listbox.selectBackground", "#0078d7")
        style.configure("Muted.TLabel", foreground=pal["muted"])
        style.configure("Warn.TLabel", foreground=pal["warn"])
        win.configure(background=pal["bg"] if dark else style.lookup("TFrame", "background") or pal["bg"])
        for w in plain:
            w.configure(background=pal["field"] if isinstance(w, tk.Text) else pal["bg"])
            if isinstance(w, tk.Text):
                w.configure(foreground=pal["fg"], insertbackground=pal["fg"])
        try:                       # dark title bar on Windows 10/11
            import ctypes
            hwnd = ctypes.windll.user32.GetParent(win.winfo_id())
            v = ctypes.c_int(1 if dark else 0)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(v), 4)
        except Exception:
            pass

    dark_var = tk.BooleanVar(value=C.get("dark", system_prefers_dark()))

    top = ttk.Frame(win, padding=(10, 8))
    top.pack(fill="x")
    ttk.Label(top, text=APP_NAME, font=("Segoe UI", 14, "bold")).pack(side="left")
    game_var = tk.StringVar()
    game_box = ttk.Combobox(top, textvariable=game_var, state="readonly", width=42)
    game_box.pack(side="left", padx=(16, 0))
    ver = ttk.Label(top, text="")
    ver.pack(side="right")
    shown_gid = [None]

    def sync_game_box():
        names = dict((i, n) for i, n, _e in game_list(P))
        game_box.configure(values=list(names.values()))
        game_var.set(names.get(P.gid, P.gid))

    def on_game(_e=None):
        gid = next((i for i, n, _e2 in game_list(P) if n == game_var.get()), None)
        if gid and gid != P.gid and not busy.is_set():
            select_game(P, C, gid)
            disc_var.set(C.get("disc", ""))
            refresh()
        else:
            sync_game_box()

    game_box.bind("<<ComboboxSelected>>", on_game)

    def toggle_dark():
        C.set("dark", bool(dark_var.get()))
        apply_theme(dark_var.get())
        refresh()

    ttk.Checkbutton(top, text="Dark mode", variable=dark_var, command=toggle_dark).pack(side="right", padx=16)

    nb = ttk.Notebook(win)
    nb.pack(fill="both", expand=True, padx=10)
    tabs = {}
    for name in ("Play", "Build", "Update", "Settings"):
        fr = ttk.Frame(nb, padding=12)
        nb.add(fr, text=name)
        tabs[name] = fr

    bar = ttk.Progressbar(win, mode="determinate")
    bar.pack(fill="x", padx=10, pady=(6, 0))
    logbox = tk.Text(win, height=9, wrap="word", state="disabled", font=("Consolas", 9))
    logbox.pack(fill="both", padx=10, pady=8)
    plain.append(logbox)

    buttons = []

    def lock(b):
        buttons.append(b)
        return b

    # ---- Play tab
    p = tabs["Play"]
    ttk.Label(p, text="What is ready", font=("Segoe UI", 10, "bold")).pack(anchor="w")
    # One label for every row status() can give, made once; refresh() shows the ones the current disc has, in order
    # (the "chd" row only for a .chd disc), so picking another kind of disc never needs a label that does not exist.
    rows = {}
    for key in STATUS_KEYS:
        rows[key] = ttk.Label(p, text="", cursor="hand2", font=("Segoe UI", 11))
    rows_end = ttk.Label(p, text="A ✗ can be clicked to go where it is fixed.", style="Muted.TLabel")
    rows_end.pack(anchor="w", pady=(0, 10))
    play_btn = lock(ttk.Button(p, text="Play"))
    play_btn.pack(anchor="w", ipadx=24, ipady=6)
    close_var = tk.BooleanVar(value=C.get("close_on_play", False))

    def set_close():
        C.set("close_on_play", bool(close_var.get()))

    ttk.Checkbutton(p, text="Close the launcher when the game starts", variable=close_var,
                    command=set_close).pack(anchor="w", pady=(8, 0))
    upd_note = ttk.Label(p, text="", style="Warn.TLabel")
    upd_note.pack(anchor="w", pady=8)

    # ---- Build tab
    b = tabs["Build"]
    adopt_disc(P, C, switch=first_run and not pinned)
    if migrate_note:
        log("warning: " + migrate_note)
    disc_title = ttk.Label(b, text="")
    disc_title.pack(anchor="w")
    ttk.Label(b, style="Muted.TLabel", wraplength=660,
              text="Put the .cue and .bin in the \"Dave Mirra BMX\" folder next to the launcher and it is found "
                   "automatically, or browse to them anywhere.").pack(anchor="w")
    disc_var = tk.StringVar(value=C.get("disc", ""))
    row = ttk.Frame(b)
    row.pack(fill="x", pady=4)
    ttk.Entry(row, textvariable=disc_var).pack(side="left", fill="x", expand=True)

    def browse():
        if busy.is_set():
            return                      # the game must not change under a running build
        f = filedialog.askopenfilename(title="Choose the disc image",
                                       filetypes=[("Disc image", "*.cue *.bin *.chd"), ("All files", "*.*")])
        if f:
            try:                      # an image of the other game switches to that game
                gid = game_for_exe(P, identify_disc(P, C, f))
            except (Fail, OSError):
                gid = None
            if gid and gid != P.gid:
                select_game(P, C, gid)
            disc_var.set(os.path.normpath(f))
            C.set("disc", disc_var.get())
            refresh()

    def open_discdir():
        os.makedirs(P.discdir, exist_ok=True)
        if WINDOWS:
            os.startfile(P.discdir)
        else:
            try:
                subprocess.Popen(["xdg-open", P.discdir], env=child_env())
            except OSError:
                log("your disc folder is %s" % P.discdir)

    ttk.Button(row, text="Browse...", command=browse).pack(side="left", padx=(6, 0))
    ttk.Button(row, text="Open folder", command=open_discdir).pack(side="left", padx=(6, 0))
    disc_msg = ttk.Label(b, text="", wraplength=640)
    disc_msg.pack(anchor="w", pady=4)
    build_btn = lock(ttk.Button(b, text="Build the game"))
    build_btn.pack(anchor="w", pady=6, ipadx=16, ipady=4)
    if not WINDOWS:
        def do_shortcut():
            try:
                make_shortcut(P, C, log)
                messagebox.showinfo("Applications menu", "Added to the applications menu.\n\nOn the Steam Deck: in Steam choose "
                                    "Games > Add a Non-Steam Game to My Library and tick it to get it into Game Mode.")
            except (Fail, OSError) as e:
                messagebox.showerror("Applications menu", str(e))
        ttk.Button(b, text="Add to the applications menu (for Steam)", command=do_shortcut).pack(anchor="w", pady=(0, 6))
    ttk.Label(b, wraplength=660, style="Muted.TLabel",
              text="Checks the image, translates the game code and builds the game into the game folder. "
                   "Steps that are already up to date are skipped. The first build takes a few minutes.").pack(anchor="w")

    # ---- Update tab
    u = tabs["Update"]
    info = ttk.Label(u, text="", justify="left")
    info.pack(anchor="w", pady=(0, 8))
    dl_btn = lock(ttk.Button(u, text="Download (build tools and project, about 200 MB)" if WINDOWS
                             else "Download (the project; checks this system's build tools)"))
    dl_btn.pack(anchor="w", pady=4, ipadx=8, ipady=3)
    up_btn = lock(ttk.Button(u, text="Update the project"))
    up_btn.pack(anchor="w", pady=4, ipadx=8, ipady=3)
    ul_btn = lock(ttk.Button(u, text="Update launcher"))
    ul_btn.pack(anchor="w", pady=4, ipadx=8, ipady=3)
    ttk.Label(u, wraplength=660, style="Muted.TLabel",
              text="Updates never touch your settings or saves. After updating the project, build again.").pack(anchor="w", pady=8)

    # ---- Settings tab
    s = tabs["Settings"]
    svars = {}
    typed = set()          # settings edited by typing (keys, sizes, anything unknown): saved when the typing is finished
    committed = {}         # their last finished values; a save writes these, not what is in the field at that moment
    cur = read_ini(P.ini)
    for k, v in cur.items():
        svars[k] = tk.StringVar(value=v)
    snb = ttk.Notebook(s)
    snb.pack(fill="both", expand=True)
    stabs = {}
    for name in SETTING_TABS:
        fr = ttk.Frame(snb, padding=8)
        snb.add(fr, text=name)
        stabs[name] = fr

    def scrolled(parent):
        cv = tk.Canvas(parent, highlightthickness=0, background=pal["bg"])
        plain.append(cv)
        sb = ttk.Scrollbar(parent, orient="vertical", command=cv.yview)
        inner = ttk.Frame(cv)
        inner.bind("<Configure>", lambda e: cv.configure(scrollregion=cv.bbox("all")))
        cv.create_window((0, 0), window=inner, anchor="nw")
        cv.configure(yscrollcommand=sb.set)
        cv.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        return inner

    def capture_key(var, single, label, done):
        """A small window that waits for one key press and adds it to the setting; done() then saves it."""
        top = tk.Toplevel(win)
        top.title("Press a key")
        top.transient(win)
        ttk.Label(top, text="%s\n\nPress the key to use. Escape cancels (to use Escape itself, type escape in the box)." % label,
                  wraplength=320, padding=16).pack()
        msg = ttk.Label(top, text="", padding=(16, 0, 16, 12), style="Muted.TLabel")
        msg.pack()

        def on_key(e):
            if e.keysym == "Escape":
                top.destroy()
                return "break"
            name = key_name_from_tk(e.keysym)
            if not name:
                msg.configure(text="The game has no key for \"%s\"." % e.keysym)
                return "break"
            cur = [k.strip() for k in var.get().split(",") if k.strip() and k.strip().lower() != "none"]
            if single or not cur:
                cur = [name]
            elif name.lower() not in [c.lower() for c in cur]:
                if len(cur) >= MAXKEYS:
                    msg.configure(text="At most %d keys per button: clear one first." % MAXKEYS)
                    return "break"
                cur.append(name)
            var.set(", ".join(cur))
            top.destroy()
            done()
            return "break"
        top.bind("<KeyPress>", on_key)
        top.grab_set()
        top.focus_force()

    def widget_for(parent, key, label, r):
        sc = SCHEMA.get(key)
        ttk.Label(parent, text=label).grid(row=r, column=0, sticky="w", padx=(0, 12), pady=2)
        var = svars.setdefault(key, tk.StringVar(value=""))
        field = None                    # the part that is typed in, when the widget is more than that
        if sc and sc[3] == "bool":
            w = ttk.Checkbutton(parent, variable=var, onvalue="1", offvalue="0")
        elif sc and sc[3] == "choice":
            w = ttk.Combobox(parent, textvariable=var, values=sc[4], state="readonly", width=14)
        elif sc and sc[3] == "combo":
            w = ttk.Combobox(parent, textvariable=var, values=sc[4], width=14)   # editable: pick one or type WxH
        elif sc and sc[3] == "key":
            w = ttk.Frame(parent)
            single = key == "key_fast_forward" or key[4:] in [h[0] for h in HOTKEYS]
            field = ttk.Entry(w, textvariable=var, width=24)
            field.pack(side="left")
            ttk.Button(w, text="Press a key...",
                       command=lambda v=var, sg=single, lb=label, k=key: capture_key(v, sg, lb, lambda: typed_done(k))).pack(side="left", padx=(6, 0))
            ttk.Button(w, text="Clear", width=6, command=lambda v=var, k=key: (v.set("none"), typed_done(k))).pack(side="left", padx=(4, 0))
        else:
            w = ttk.Entry(parent, textvariable=var, width=24)
        w.grid(row=r, column=1, sticky="w", pady=2)
        if not (sc and sc[3] in ("bool", "choice")):
            typed.add(key)
            for ev in ("<FocusOut>", "<Return>", "<<ComboboxSelected>>"):
                (field or w).bind(ev, lambda _e, k=key: typed_done(k))

    for name in ("Gameplay", "Video", "Input", "Audio"):
        inner = scrolled(stabs[name]) if name == "Input" else stabs[name]
        r = 0
        for key, tab, label, *_ in SETTINGS:
            if tab == name:
                widget_for(inner, key, label, r)
                r += 1
        if name == "Input":
            ttk.Label(inner, style="Muted.TLabel", wraplength=520,
                      text="Keys: use \"Press a key...\" to add one (up to four per button), or type names separated by "
                           "commas: a letter or digit, or " + " ".join(k for k in KEYNAMES if k != "none") + ". Clear sets none.").grid(
                row=r, column=0, columnspan=2, sticky="w", pady=6)

    dbg = stabs["Debug"]
    dc = tk.BooleanVar(value=C.get("debug_console", False))
    if WINDOWS:                         # elsewhere there is no console window to show: the messages always go to the log
        ttk.Checkbutton(dbg, text="Show the console window while playing", variable=dc).pack(anchor="w", pady=2)
    dflags = {}
    for flag, text in DEBUG_FLAGS:
        v = tk.BooleanVar(value=flag in C.get("debug_flags", []))
        dflags[flag] = v
        ttk.Checkbutton(dbg, text="%s   %s" % (flag, text), variable=v).pack(anchor="w", pady=2)
    ttk.Label(dbg, style="Muted.TLabel", wraplength=560,
              text="These are launcher options, not part of dmrecomp.ini. On Windows, the console window shows the "
                   "game's messages, including the [watchdog] and [exit] reports. Otherwise, and always on Linux, "
                   "they are kept in last-run.log in the game folder, and the run before in "
                   "previous-run.log.").pack(anchor="w", pady=8)

    allinner = scrolled(stabs["All settings"])
    allrow = [0]

    def fill_all():
        for w in allinner.winfo_children():
            w.destroy()
        for r, key in enumerate(sorted(svars)):
            widget_for(allinner, key, key, r)

    fill_all()

    # Settings are saved by themselves: any choice starts a short timer, and when it runs out the values go to the game's
    # file. settings_ini is the file the values on screen came from (the shown game's), so a save that was still waiting
    # when another game is chosen goes to the right file.
    settings_ini = [P.ini]
    autosave = {"job": None, "quiet": False}
    watched = set()

    # The game writes this file too (its in-game menu), so the launcher is not the only writer. disk is what the file
    # held when the launcher last read or wrote it: a save writes only the settings that differ from it, and what the
    # game changed meanwhile is taken over first (take_file_changes), so neither undoes the other.
    disk = dict(cur)
    disk_time = [None]

    def file_time():
        try:
            return os.stat(settings_ini[0]).st_mtime_ns
        except OSError:
            return None

    def take_file_changes():
        """Show what the file says now, for every setting that was not changed here since the launcher last looked.
        One changed in both places keeps the launcher's value, and the next save writes it."""
        if file_time() == disk_time[0]:
            return
        try:                            # the game may be replacing the file this instant: look again at the next save or focus
            new = read_ini(settings_ini[0], strict=True)
        except OSError:
            return
        was = autosave["quiet"]
        autosave["quiet"] = True
        try:
            for k, v in new.items():
                if disk.get(k) == v:
                    continue
                if k in svars:
                    here = committed.get(k, "") if k in typed else svars[k].get().strip()
                    if here != disk.get(k, ""):
                        continue
                    svars[k].set(v)
                    if k in typed:
                        committed[k] = v
                disk[k] = v
        finally:
            autosave["quiet"] = was
        disk_time[0] = file_time()

    def save_settings():
        """Write the settings that were changed here. A value that is not valid (a size or a key half typed) is left as
        the file has it."""
        autosave["job"] = None
        take_file_changes()
        vals = {}
        for k, var in svars.items():
            v = committed.get(k, "") if k in typed else var.get().strip()
            if v != "":
                vals[k] = v
        bad = sorted(k for k, v in vals.items() if not valid_setting(k, v))
        for k in bad:
            del vals[k]
        vals = {k: v for k, v in vals.items() if disk.get(k) != v}
        try:
            if vals or not os.path.exists(settings_ini[0]):
                write_ini(settings_ini[0], vals)
            disk.update(vals)
            disk_time[0] = file_time()
            with C.lock:
                C.d["debug_console"] = bool(dc.get())
                C.d["debug_flags"] = [f for f, v in dflags.items() if v.get()]
                C.save()
        except OSError as e:
            saved_lbl.configure(text="Could not save: %s" % e, foreground=pal["bad"])
            return
        if bad:
            saved_lbl.configure(text="Saved, except %s: not a valid value" % ", ".join(bad), foreground=pal["bad"])
        else:
            saved_lbl.configure(text="Saved", foreground=pal["ok"])

    def settings_changed(*_a):
        if autosave["quiet"]:
            return
        if autosave["job"] is not None:
            win.after_cancel(autosave["job"])
        autosave["job"] = win.after(400, save_settings)

    def typed_done(key):
        """A typed field was left, or Return was pressed in it: its value counts from now on."""
        v = svars[key].get().strip() if key in svars else ""
        if committed.get(key) != v:
            committed[key] = v
            settings_changed()

    def sync_typed():
        for k in typed:
            if k in svars:
                committed[k] = svars[k].get().strip()

    def flush_settings():
        """Save now if a save is waiting: before the game starts, another game is shown or the window closes. A typed
        field that still has the cursor counts too, if what it holds is a valid value."""
        for k in sorted(typed):
            v = svars[k].get().strip() if k in svars else ""
            if committed.get(k) != v and v != "" and valid_setting(k, v):
                committed[k] = v
                settings_changed()
        if autosave["job"] is not None:
            win.after_cancel(autosave["job"])
            save_settings()

    def watch_settings():
        for k, v in svars.items():
            if k not in watched and k not in typed:
                watched.add(k)
                v.trace_add("write", settings_changed)

    def reset_settings():
        if not messagebox.askyesno("Settings", "Put every setting back to its default?"):
            return
        for k, sc in SCHEMA.items():
            svars.setdefault(k, tk.StringVar()).set(sc[5])
        watch_settings()
        sync_typed()
        settings_changed()

    def reload_settings():
        """Show what the shown game's file says; a save that was still waiting is dropped."""
        if autosave["job"] is not None:
            win.after_cancel(autosave["job"])
            autosave["job"] = None
        autosave["quiet"] = True
        try:
            settings_ini[0] = P.ini
            new = read_ini(P.ini)
            disk.clear()
            disk.update(new)
            disk_time[0] = file_time()
            for k in [k for k in svars if k not in SCHEMA and k not in new]:   # a line only the other game's file has
                del svars[k]
                watched.discard(k)
                typed.discard(k)
                committed.pop(k, None)
            for k, v in new.items():
                svars.setdefault(k, tk.StringVar()).set(v)
            fill_all()
            watch_settings()
            sync_typed()
        finally:
            autosave["quiet"] = False
        saved_lbl.configure(text="")

    sbtn = ttk.Frame(s)
    sbtn.pack(fill="x", pady=(8, 0))
    ttk.Button(sbtn, text="Reset to defaults", command=reset_settings).pack(side="left")
    ttk.Button(sbtn, text="Reload", command=reload_settings).pack(side="left", padx=6)
    ttk.Label(sbtn, text="Changes are saved as you make them.", style="Muted.TLabel").pack(side="left", padx=(2, 0))
    saved_lbl = ttk.Label(sbtn, text="")
    saved_lbl.pack(side="left", padx=8)
    edits_lbl = ttk.Label(sbtn, text="", style="Muted.TLabel")
    edits_lbl.pack(side="left", padx=8)
    watch_settings()
    sync_typed()
    disk_time[0] = file_time()
    # back from the game: show what its menu changed (a save that is waiting does this itself)
    win.bind("<FocusIn>", lambda e: take_file_changes() if e.widget is win and autosave["job"] is None else None, add="+")
    dc.trace_add("write", settings_changed)
    for _v in dflags.values():
        _v.trace_add("write", settings_changed)

    # ---- plumbing
    def refresh():
        if not busy.is_set() and adopt_disc(P, C):
            disc_var.set(C.get("disc", ""))
            log("found your disc image: %s" % C.get("disc"))
        sync_game_box()
        if shown_gid[0] != P.gid:      # a different game: its settings file, its disc image
            flush_settings()           # a waiting save belongs to the game shown until now
            shown_gid[0] = P.gid
            disc_var.set(C.get("disc", ""))
            reload_settings()
            edits_lbl.configure(text="(edits %s)" % P.ini)
        gname = dict((i, n) for i, n, _e in game_list(P)).get(P.gid, P.gid)
        disc_title.configure(text="Your disc image for %s (.cue, .bin or .chd)" % gname)
        st = status(P, C)
        ok_all = True
        first_bad = None
        for l in rows.values():
            l.pack_forget()
        for key, label, ok, tab in st:
            rows[key].configure(text=("✓  " if ok else "✗  ") + label,
                                foreground=pal["ok"] if ok else pal["bad"])
            rows[key].tab = tab
            rows[key].bind("<Button-1>", lambda e, t=tab: nb.select(tabs[t]))
            rows[key].pack(anchor="w", pady=2, before=rows_end)
            if not ok:
                ok_all = False
                first_bad = first_bad or tab
        play_btn.configure(state="normal" if (ready_to_play(P, C) and not busy.is_set()) else "disabled")
        ver.configure(text="project %s" % C.get("tag", "not downloaded"))
        info.configure(text="Project: %s\nBuild tools: %s\nGame folder: %s" % (
            C.get("tag", "not downloaded"), "installed" if toolchain_ok(P) else "not downloaded", P.game))
        d = disc_var.get().strip()
        if d:
            try:
                disc_fingerprint(d)
                other = game_for_exe(P, identify_disc(P, C, d))
                if other and other != P.gid:
                    disc_msg.configure(text="This image is %s: choose it in the game list at the top." % dict(
                        (i, n) for i, n, _e in game_list(P)).get(other, other), foreground=pal["bad"])
                else:
                    disc_msg.configure(text="Image found: %s" % os.path.basename(d if is_chd(d) else resolve_bin(d)), foreground=pal["ok"])
            except (Fail, OSError) as e:
                disc_msg.configure(text=str(e), foreground=pal["bad"])
        else:
            disc_msg.configure(text="No image chosen yet.", foreground=pal["muted"])
        return first_bad

    def work(fn, done_msg):
        if busy.is_set():
            return
        busy.set()
        for bt in buttons:
            bt.configure(state="disabled")
        bar.configure(mode="indeterminate")
        bar.start(12)

        def target():
            try:
                fn()
                q.put(("done", done_msg))
            except Fail as e:
                q.put(("error", str(e)))
            except Exception as e:      # keep the window alive on a bug
                q.put(("error", "unexpected error: %r" % e))
        threading.Thread(target=target, daemon=True).start()

    def progress(done, total):
        q.put(("progress", (done, total)))

    def do_download():
        def f():
            install_toolchain(P, log)
            install_project(P, C, log)
        work(f, "Download finished.")

    def do_update():
        work(lambda: install_project(P, C, log), "Project updated. Build again to use it.")

    def do_update_launcher():
        work(lambda: update_launcher(P, C, log), "Launcher check finished.")

    def do_build():
        C.set("disc", disc_var.get().strip())
        work(lambda: build_all(P, C, C.get("disc"), log, progress), "Build finished.")

    def do_play():
        flush_settings()
        try:
            play(P, C, C.get("disc"), log)
        except Fail as e:
            messagebox.showerror("Play", str(e))
            return
        if close_var.get():
            win.after(500, win.destroy)

    play_btn.configure(command=do_play)
    build_btn.configure(command=do_build)
    dl_btn.configure(command=do_download)
    up_btn.configure(command=do_update)
    ul_btn.configure(command=do_update_launcher)

    def check_updates():
        t = latest_tag(lambda s: None)
        q.put(("latest", t))
    threading.Thread(target=check_updates, daemon=True).start()

    def pump():
        try:
            while True:
                kind, val = q.get_nowait()
                if kind == "log":
                    logbox.configure(state="normal")
                    logbox.insert("end", val + "\n")
                    logbox.see("end")
                    logbox.configure(state="disabled")
                elif kind == "progress":
                    bar.stop()
                    bar.configure(mode="determinate", maximum=max(1, val[1]), value=val[0])
                elif kind == "latest":
                    have = C.get("tag")
                    if val and have and version_tuple(val) and (version_tuple(have) or (0,)) < version_tuple(val):
                        upd_note.configure(text="A newer project version is available: %s (Update tab)." % val)
                    elif val and not have:
                        upd_note.configure(text="Start with the Update tab: Download.")
                elif kind in ("done", "error"):
                    busy.clear()
                    bar.stop()
                    bar.configure(mode="determinate", value=0)
                    for bt in buttons:
                        bt.configure(state="normal")
                    if kind == "error":
                        log("ERROR: " + val)
                        messagebox.showerror(APP_NAME, val)
                    else:
                        log(val)
                    refresh()
        except queue.Empty:
            pass
        win.after(100, pump)

    apply_theme(dark_var.get())
    win.bind("<FocusIn>", lambda e: refresh() if e.widget is win else None)   # picks up a disc image just dropped in the folder
    first = refresh()
    nb.select(tabs["Play"] if ready_to_play(P, C) else tabs[first or "Update"])
    pump()
    if notice:
        log("ERROR: " + notice)
        win.after(300, lambda: messagebox.showerror(APP_NAME, notice))
    def on_close():
        try:
            flush_settings()
        finally:
            win.destroy()          # whatever the save did, the window closes

    win.protocol("WM_DELETE_WINDOW", on_close)
    win.mainloop()
    return 0


def attach_std_streams():
    """The packaged launcher has no console, so Python gives it no stdout and stderr. When whoever started it handed
    it a pipe or a file for them (RecompLauncher reads the setup step's progress lines that way), write there;
    otherwise, as when it is started by a double click, what it prints goes nowhere."""
    if sys.stdout is not None:
        return
    streams = []
    if WINDOWS:
        try:
            import ctypes
            import msvcrt
            k32 = ctypes.windll.kernel32
            k32.GetStdHandle.restype = ctypes.c_void_p
            for which in (-11, -12):    # STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
                h = k32.GetStdHandle(which)
                if h in (None, 0, ctypes.c_void_p(-1).value) or k32.GetFileType(ctypes.c_void_p(h)) == 0:
                    streams.append(None)
                    continue
                streams.append(os.fdopen(msvcrt.open_osfhandle(h, os.O_WRONLY), "w", encoding="utf-8", errors="replace"))
        except (OSError, AttributeError, ValueError):
            streams = []
    out = streams[0] if streams else None
    err = streams[1] if len(streams) > 1 else None
    sink = None if (out and err) else open(os.devnull, "w")
    sys.stdout = out or err or sink
    sys.stderr = err or out or sink


def main(argv):
    cmds = {"status", "games", "download", "update", "build", "play", "run", "prepare", "shortcut", "update-launcher",
            "-h", "--help", "help"}
    if any(a in cmds for a in argv):
        attach_std_streams()        # packaged without a console window: print where the caller put stdout and stderr
        return cli(argv)
    if sys.stdout is None:          # the window: what it prints goes nowhere, as before
        sys.stdout = sys.stderr = open(os.devnull, "w")
    def opt(name):
        return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else None
    root, disc, game = opt("--root"), opt("--disc"), opt("--game")
    notice = None
    if disc or game:
        try:
            open_session(root, disc, game)      # remembers the choice; the window then starts from it
        except Fail as e:
            notice = str(e)                     # a packaged launcher has no console: say it in the window
    return gui(root or default_root(), pinned=bool(game), notice=notice)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
