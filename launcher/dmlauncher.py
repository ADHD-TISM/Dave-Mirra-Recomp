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
    python dmlauncher.py --game ID ...        pick the game (dmbmx, maxremix) instead of the remembered one
    python dmlauncher.py --root DIR ...       keep everything in DIR instead
    python dmlauncher.py --disc FILE.cue ...  use this disc image

Folders under the root: tools/ (compiler, downloaded), project/ (the game's
source, downloaded), work/<game>/ (generated code, objects, caches) and
game/<game>/ (the built dmrecomp.exe, dmrecomp.ini and your memory cards for
that game). Updates never touch game/, so settings and saves are kept.

Windows only for now: the game itself has only a Windows front end so far.
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

REPO = "ADHD-TISM/Dave-Mirra-Recomp"
LAUNCHER_VERSION = "3"

# Build tools: llvm-mingw (clang + a MinGW-w64 UCRT sysroot), pinned and checked.
LLVM_NAME = "llvm-mingw-20261104-ucrt-x86_64"
LLVM_URL = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/%s.zip" % LLVM_NAME
LLVM_SHA256 = "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f"
# Other targets' sysroots are not needed; leaving them out saves disk space.
LLVM_SKIP = ("aarch64-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/")
# Python for the code translator, used only when the launcher itself is a packaged exe.
PY_URL = "https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-amd64.zip"
PY_SHA256 = "8d3f33be9eb810f23c102f08475af2854e50484b8e4e06275e937be61ce3d2fb"

FROZEN = bool(getattr(sys, "frozen", False))
WINDOWS = sys.platform == "win32"
NO_WINDOW = 0x08000000 if WINDOWS else 0
NEW_CONSOLE = 0x00000010 if WINDOWS else 0
UA = {"User-Agent": "dmrecomp-launcher"}


class Fail(Exception):
    """A step failed; the message says what to do about it."""


# ---------------------------------------------------------------- paths/config

def default_root():
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
        self.discdir = os.path.join(self.root, "Dave Mirra BMX")   # where people put their disc image
        self.cfg = os.path.join(self.root, "launcher.json")
        self.use(gid)

    def use(self, gid):
        """Switch to a game: each game has its own generated code, objects and game folder (exe, settings, saves)."""
        self.gid = gid
        self.game = os.path.join(self.root, "game", gid)
        self.gen = os.path.join(self.work, gid, "gen")
        self.obj = os.path.join(self.work, gid, "obj")
        self.disc = os.path.join(self.work, gid, "disc")
        self.exe = os.path.join(self.game, "dmrecomp.exe")
        self.ini = os.path.join(self.game, "dmrecomp.ini")

    @property
    def clang(self):
        return os.path.join(self.llvm, "bin", "x86_64-w64-mingw32-clang.exe")

    @property
    def tool_python(self):
        return os.path.join(self.pydir, "python.exe") if FROZEN else sys.executable


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


def replace_dir(tmp, final):
    if os.path.isdir(final):
        shutil.rmtree(final)
    os.replace(tmp, final)


def run(cmd, log, cwd=None, env=None, tag=""):
    """Run a command, streaming its output to log. Returns the output text."""
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
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


def toolchain_ok(P):
    ok = os.path.isfile(P.clang)
    if FROZEN:
        ok = ok and os.path.isfile(P.tool_python)
    return ok


def install_toolchain(P, log):
    if not WINDOWS:
        raise Fail("The game only has a Windows front end so far, so there is nothing to build here yet.")
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


def install_project(P, C, log, force=False):
    """Download the newest project version unless that one is already installed."""
    tag = latest_tag(log, strict=not (project_ok(P) and C.get("tag")))
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


# ---------------------------------------------------------------- disc

def resolve_bin(path):
    """The .bin behind a .cue (or the .bin itself)."""
    if not path or not os.path.isfile(path):
        raise Fail("disc image not found: %s" % (path or "(none chosen)"))
    if path.lower().endswith(".cue"):
        with open(path, encoding="utf-8", errors="replace") as f:
            m = re.search(r'^\s*FILE\s+"(.+?)"', f.read(), re.M | re.I)
        if not m:
            raise Fail("%s has no FILE line" % os.path.basename(path))
        b = os.path.join(os.path.dirname(os.path.abspath(path)), m.group(1))
        if not os.path.isfile(b):
            raise Fail("%s names %s, which is not next to it" % (os.path.basename(path), m.group(1)))
        return b
    return path


def disc_fingerprint(path):
    b = resolve_bin(path)
    st = os.stat(b)
    if st.st_size % 2352:
        raise Fail("%s is not a raw 2352-byte-sector image; the game needs a BIN/CUE dump" % os.path.basename(b))
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
    src = [("rt", os.path.join(rt, n)) for n in sorted(os.listdir(rt)) if n.endswith(".c")]
    src += [("gen", os.path.join(P.gen, n)) for n in sorted(os.listdir(P.gen)) if n.endswith(".c")]
    hdr = [os.path.join(rt, n) for n in sorted(os.listdir(rt)) if n.endswith(".h")]
    hdr += [os.path.join(P.gen, n) for n in sorted(os.listdir(P.gen)) if n.endswith(".h")]
    return src, hdr


def _flags(P, kind):
    # the same options CMakeLists.txt uses; the generated code is huge, so -O1 there
    f = ["-O3", "-DNDEBUG", "-std=gnu11", "-I", os.path.join(P.project, "runtime"), "-I", P.gen,
         "-Wall", "-Wno-unused-function", "-fno-strict-aliasing"]
    if kind == "gen":
        f += ["-O1", "-w"]
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
    env["PATH"] = os.path.join(P.llvm, "bin") + os.pathsep + env.get("PATH", "")
    todo, objs, newkeys = [], [], {}
    for kind, p in src:
        name = os.path.basename(p)
        o = os.path.join(P.obj, name[:-2] + ".o")
        objs.append(o)
        k = key_of(hh, " ".join(_flags(P, kind)), P.clang, file_bytes(p))
        newkeys[name] = k
        if not (os.path.isfile(o) and keys.get(name) == k):
            todo.append((kind, p, o))
    log("compiling %d of %d files%s" % (len(todo), len(src), " (the first build takes a few minutes)" if len(todo) > 20 else ""))
    done = [0]
    errors = []
    lock = threading.Lock()

    def one(t):
        kind, p, o = t
        if errors:
            return
        tmp = o + ".tmp"
        try:
            run([P.clang, "-c", p, "-o", tmp] + _flags(P, kind), lambda s: None, cwd=P.root, env=env)
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
    run([P.clang] + objs + ["-o", tmp, "-static", "-Wl,--stack,16777216", "-lwinmm", "-lopengl32", "-lgdi32"],
        log, cwd=P.root, env=env)
    try:
        os.replace(tmp, P.exe)
    except OSError:
        os.remove(tmp)
        raise Fail("dmrecomp.exe is in use: close the game and build again")
    C.set("build_key", build_key(P))
    log("game built: %s" % P.exe)


def build_all(P, C, disc_path, log, progress=None):
    if not disc_path:
        raise Fail("choose your disc image first")
    if not toolchain_ok(P) or not project_ok(P):
        raise Fail("download the build tools and project first (Update tab)")
    generate(P, C, disc_path, log)
    compile_game(P, C, log, progress)


# ---------------------------------------------------------------- settings (dmrecomp.ini)

KEYNAMES = ("up down left right enter backspace tab space lshift rshift lctrl rctrl lalt ralt "
            "comma period slash semicolon num0 num1 num2 num3 num4 num5 num6 num7 num8 num9 none").split()
PADNAMES = "a b x y lb rb lt rt start back ls rs up down left right none".split()
B01 = ["0", "1"]

# (key, tab, label, kind, choices, default, comment)
SETTINGS = [
    ("scale", "Video", "Internal 3D resolution", "choice", ["1", "2", "4"], "1",
     "Internal 3D resolution: 1 = original, 2 or 4 = sharper (F6 cycles while playing). The CPU draws it: 4 is 16 times the work, so use 2 on a laptop"),
    ("scale_auto", "Video", "Lower it when too slow", "bool", B01, "1",
     "1 = drop to a lower internal resolution by itself when the game cannot hold full speed at 2 or 4"),
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
    ("cpu_assist", "Gameplay", "Hold 30 fps in busy scenes", "bool", B01, "1",
     "1 = hold 30 fps in busy scenes, 0 = console CPU timing with its slowdowns"),
    ("reverb", "Audio", "Console echo effect (reverb)", "bool", B01, "1",
     "1 = echo effect the console adds in levels, 0 = dry sound"),
    ("dualshock", "Gameplay", "DualShock pad (sticks, rumble)", "bool", B01, "1",
     "Controllers. dualshock = 0 makes the game see a plain digital pad (no sticks, no rumble)."),
    ("rumble", "Gameplay", "Rumble", "bool", B01, "1", None),
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
for _n, _d in KEYS:
    SETTINGS.append(("key_" + _n, "Input", "Key: " + _n.upper(), "key", None, _d, None))
SETTINGS.append(("key_fast_forward", "Input", "Key: fast-forward", "key", None, "tab", None))
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


def read_ini(path):
    """Settings in the file (name -> value); the defaults for anything missing."""
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
    except OSError:
        pass
    return vals


def valid_setting(key, val):
    s = SCHEMA.get(key)
    if not s:
        return True
    if s[3] in ("choice", "bool"):
        return val.lower() in [c.lower() for c in s[4]]
    if s[3] == "combo":
        if key == "fsr_sharpness":
            return val.isdecimal() and val.isascii() and 0 <= int(val) <= 100
        m = re.fullmatch(r"(\d{3,5})x(\d{3,5})", val)   # the game wants a lower-case x and at most 16384
        return val.lower() == s[5] or bool(m and 320 <= int(m.group(1)) <= 16384 and 240 <= int(m.group(2)) <= 16384)
    parts = [p.strip() for p in val.split(",")]
    if not 1 <= len(parts) <= 2:
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


def play(P, C, disc_path, log):
    if not os.path.isfile(P.exe):
        raise Fail("the game is not built yet (Build tab)")
    if not disc_path:
        raise Fail("choose your disc image first (Build tab)")
    resolve_bin(disc_path)
    args = [P.exe, os.path.abspath(disc_path)] + [f for f, _ in DEBUG_FLAGS if f in C.get("debug_flags", [])]
    flags = NEW_CONSOLE if C.get("debug_console", False) else NO_WINDOW
    subprocess.Popen(args, cwd=P.game, creationflags=flags)
    log("started the game")


# ---------------------------------------------------------------- disc folder

EXE_NAME = re.compile(r"^S[A-Z]{3}_\d{3}\.\d{2}$")


def iso_root_files(bin_path):
    """File names in the root directory of a raw 2352-byte-sector PS1 image."""
    with open(bin_path, "rb") as f:
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
        exes = [n for n in iso_root_files(resolve_bin(path)) if EXE_NAME.match(n)]
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
        cands = {".cue": [], ".bin": []}
        for dirpath, dirs, files in os.walk(P.discdir):
            if os.path.relpath(dirpath, P.discdir).count(os.sep) >= 1:
                dirs[:] = []
            for n in sorted(files):
                ext = os.path.splitext(n)[1].lower()
                if ext in cands:
                    cands[ext].append(os.path.join(dirpath, n))
        for path in cands[".cue"] + cands[".bin"]:
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

def status(P, C):
    """(key, label, ok, tab_to_fix_it) for each requirement."""
    disc = C.get("disc")
    disc_ok = False
    try:
        if disc:
            disc_fingerprint(disc)
            want = dict((i, e) for i, _n, e in game_list(P)).get(P.gid, "")
            disc_ok = not want or identify_disc(P, C, disc).lower() == want.lower()
    except (Fail, OSError):
        pass
    return [
        ("tools", "Build tools", toolchain_ok(P), "Update"),
        ("project", "Project files", project_ok(P), "Update"),
        ("disc", "Disc image", disc_ok, "Build"),
        ("gen", "Game code generated", generated_ok(P, C, disc) if disc_ok else False, "Build"),
        ("built", "Game built", built_ok(P, C) if disc_ok else os.path.isfile(P.exe), "Build"),
    ]


def ready_to_play(P, C):
    st = {k: ok for k, _l, ok, _t in status(P, C)}
    return os.path.isfile(P.exe) and st["disc"]


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


def cli(argv):
    root, disc, game, cmd = None, None, None, []
    it = iter(argv)
    for a in it:
        if a == "--root":
            root = next(it, None)
        elif a == "--disc":
            disc = next(it, None)
        elif a == "--game":
            game = next(it, None)
        else:
            cmd.append(a)
    try:
        P, C, note = open_session(root, disc, game)
    except Fail as e:
        print("error: %s" % e)
        return 1
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
        else:
            print(__doc__)
            return 2
    except Fail as e:
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
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    import queue

    P = Paths(root_dir)
    C = Config(P.cfg)
    first_run = "game" not in C.d       # no game has ever been chosen
    migrate_note = migrate_layout(P)
    P.use(C.game)
    win = tk.Tk()
    win.title("Dave Mirra BMX Recomp launcher")
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
    ttk.Label(top, text="Dave Mirra BMX", font=("Segoe UI", 14, "bold")).pack(side="left")
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
    rows = {}
    for key, label, _ok, _tab in status(P, C):
        l = ttk.Label(p, text=label, cursor="hand2", font=("Segoe UI", 11))
        l.pack(anchor="w", pady=2)
        rows[key] = l
    ttk.Label(p, text="A ✗ can be clicked to go where it is fixed.", style="Muted.TLabel").pack(anchor="w", pady=(0, 10))
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
                                       filetypes=[("Disc image", "*.cue *.bin"), ("All files", "*.*")])
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

    ttk.Button(row, text="Browse...", command=browse).pack(side="left", padx=(6, 0))
    ttk.Button(row, text="Open folder", command=open_discdir).pack(side="left", padx=(6, 0))
    disc_msg = ttk.Label(b, text="", wraplength=640)
    disc_msg.pack(anchor="w", pady=4)
    build_btn = lock(ttk.Button(b, text="Build the game"))
    build_btn.pack(anchor="w", pady=6, ipadx=16, ipady=4)
    ttk.Label(b, wraplength=660, style="Muted.TLabel",
              text="Checks the image, translates the game code and builds dmrecomp.exe into the game folder. "
                   "Steps that are already up to date are skipped. The first build takes a few minutes.").pack(anchor="w")

    # ---- Update tab
    u = tabs["Update"]
    info = ttk.Label(u, text="", justify="left")
    info.pack(anchor="w", pady=(0, 8))
    dl_btn = lock(ttk.Button(u, text="Download (build tools and project, about 200 MB)"))
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

    def widget_for(parent, key, label, r):
        sc = SCHEMA.get(key)
        ttk.Label(parent, text=label).grid(row=r, column=0, sticky="w", padx=(0, 12), pady=2)
        var = svars.setdefault(key, tk.StringVar(value=""))
        if sc and sc[3] == "bool":
            w = ttk.Checkbutton(parent, variable=var, onvalue="1", offvalue="0")
        elif sc and sc[3] == "choice":
            w = ttk.Combobox(parent, textvariable=var, values=sc[4], state="readonly", width=14)
        elif sc and sc[3] == "combo":
            w = ttk.Combobox(parent, textvariable=var, values=sc[4], width=14)   # editable: pick one or type WxH
        else:
            w = ttk.Entry(parent, textvariable=var, width=24)
        w.grid(row=r, column=1, sticky="w", pady=2)

    for name in ("Gameplay", "Video", "Input", "Audio"):
        inner = scrolled(stabs[name]) if name == "Input" else stabs[name]
        r = 0
        for key, tab, label, *_ in SETTINGS:
            if tab == name:
                widget_for(inner, key, label, r)
                r += 1
        if name == "Input":
            ttk.Label(inner, style="Muted.TLabel", wraplength=520,
                      text="Keys: a letter or digit, or up down left right enter backspace tab space lshift rshift "
                           "lctrl rctrl lalt ralt comma period slash semicolon num0..num9 none. Two keys separated by a comma.").grid(
                row=r, column=0, columnspan=2, sticky="w", pady=6)

    dbg = stabs["Debug"]
    dc = tk.BooleanVar(value=C.get("debug_console", False))
    ttk.Checkbutton(dbg, text="Show the console window while playing", variable=dc).pack(anchor="w", pady=2)
    dflags = {}
    for flag, text in DEBUG_FLAGS:
        v = tk.BooleanVar(value=flag in C.get("debug_flags", []))
        dflags[flag] = v
        ttk.Checkbutton(dbg, text="%s   %s" % (flag, text), variable=v).pack(anchor="w", pady=2)
    ttk.Label(dbg, style="Muted.TLabel", wraplength=560,
              text="These are launcher options, not part of dmrecomp.ini. The console window shows the stall "
                   "watchdog report if the game ever stops responding.").pack(anchor="w", pady=8)

    allinner = scrolled(stabs["All settings"])
    allrow = [0]

    def fill_all():
        for w in allinner.winfo_children():
            w.destroy()
        for r, key in enumerate(sorted(svars)):
            widget_for(allinner, key, key, r)

    fill_all()

    def save_settings():
        vals = {k: v.get().strip() for k, v in svars.items() if v.get().strip() != ""}
        for k, v in vals.items():
            if not valid_setting(k, v):
                messagebox.showerror("Settings", "%s: \"%s\" is not a valid value." % (k, v))
                return
        write_ini(P.ini, vals)
        with C.lock:
            C.d["debug_console"] = bool(dc.get())
            C.d["debug_flags"] = [f for f, v in dflags.items() if v.get()]
            C.save()
        log("settings saved to %s" % P.ini)

    def reset_settings():
        if not messagebox.askyesno("Settings", "Put every setting back to its default?"):
            return
        for k, sc in SCHEMA.items():
            svars.setdefault(k, tk.StringVar()).set(sc[5])

    def reload_settings():
        for k, v in read_ini(P.ini).items():
            svars.setdefault(k, tk.StringVar()).set(v)
        fill_all()

    sbtn = ttk.Frame(s)
    sbtn.pack(fill="x", pady=(8, 0))
    ttk.Button(sbtn, text="Save", command=save_settings).pack(side="left")
    ttk.Button(sbtn, text="Reset to defaults", command=reset_settings).pack(side="left", padx=6)
    ttk.Button(sbtn, text="Reload", command=reload_settings).pack(side="left")
    edits_lbl = ttk.Label(sbtn, text="", style="Muted.TLabel")
    edits_lbl.pack(side="left", padx=8)

    # ---- plumbing
    def refresh():
        if not busy.is_set() and adopt_disc(P, C):
            disc_var.set(C.get("disc", ""))
            log("found your disc image: %s" % C.get("disc"))
        sync_game_box()
        if shown_gid[0] != P.gid:      # a different game: its settings file, its disc image
            shown_gid[0] = P.gid
            disc_var.set(C.get("disc", ""))
            reload_settings()
            edits_lbl.configure(text="(edits %s)" % P.ini)
        gname = dict((i, n) for i, n, _e in game_list(P)).get(P.gid, P.gid)
        disc_title.configure(text="Your disc image for %s (.cue or .bin)" % gname)
        st = status(P, C)
        ok_all = True
        first_bad = None
        for key, label, ok, tab in st:
            rows[key].configure(text=("✓  " if ok else "✗  ") + label,
                                foreground=pal["ok"] if ok else pal["bad"])
            rows[key].tab = tab
            rows[key].bind("<Button-1>", lambda e, t=tab: nb.select(tabs[t]))
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
                    disc_msg.configure(text="Image found: %s" % os.path.basename(resolve_bin(d)), foreground=pal["ok"])
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
    if WINDOWS:
        threading.Thread(target=check_updates, daemon=True).start()
    else:
        log("The game only has a Windows front end so far; building and playing are not available here yet.")

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
                        messagebox.showerror("Dave Mirra launcher", val)
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
        win.after(300, lambda: messagebox.showerror("Dave Mirra launcher", notice))
    win.mainloop()
    return 0


def main(argv):
    if sys.stdout is None:          # packaged without a console window
        sys.stdout = sys.stderr = open(os.devnull, "w")
    cmds = {"status", "games", "download", "update", "build", "play", "update-launcher", "-h", "--help", "help"}
    if any(a in cmds for a in argv):
        return cli(argv)
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
