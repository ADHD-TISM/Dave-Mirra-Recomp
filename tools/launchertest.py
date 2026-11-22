"""Checks that the launcher saves settings as they are chosen, and then that it keeps the run log. It opens the launcher's real window on a scratch folder (two
made-up games, no disc, no files downloaded), makes choices in it and reads the settings files back. The window shows for
about 20 seconds; do not click in it meanwhile. The launcher asks GitHub for the latest version, as it always does.

    python tools/launchertest.py launcher/dmlauncher.py <an empty scratch folder> [window position such as +100+100]

The scratch folder must be new, empty, or one this test made before (it leaves a file named .launchertest in it);
then it is deleted and made again. Any other folder is refused. Prints one line per check and "N of N checks passed"; exit
code 0 when all pass."""
import sys, os, json, shutil, time, importlib.util
import tkinter as tk
from tkinter import ttk

SRC, T = sys.argv[1], os.path.abspath(sys.argv[2])
MARK = os.path.join(T, ".launchertest")
if os.path.exists(T) and (not os.path.isdir(T) or (os.listdir(T) and not os.path.isfile(MARK))):
    sys.exit("%s is a file, or a folder this test did not make: not touched. Name an empty or new folder." % T)
shutil.rmtree(T, ignore_errors=True)
os.makedirs(T)
open(MARK, "w").close()
A_INI = "# my note\nscale = 1\nrenderer = gl\nwindow_size = auto\nkey_cross = Z, K\nfoo = bar\n"
for g in ("dmbmx", "maxremix"):
    os.makedirs(os.path.join(T, "game", g))
    open(os.path.join(T, "game", g, "dmrecomp.ini"), "w", newline="\n").write(A_INI if g == "dmbmx" else "scale = 4\n")
json.dump({"game": "dmbmx"}, open(os.path.join(T, "launcher.json"), "w"))

spec = importlib.util.spec_from_file_location("dml", SRC); dml = importlib.util.module_from_spec(spec); spec.loader.exec_module(dml)
res = []

def ini(g="dmbmx"):
    return open(os.path.join(T, "game", g, "dmrecomp.ini")).read()

def val(g, k):
    for line in ini(g).splitlines():
        if line.startswith(k + " ="): return line.split("=", 1)[1].strip()
    return None

def check(name, ok, extra=""):
    res.append(ok); print(("ok   " if ok else "FAIL ") + name + (" : " + extra if extra else ""))

def driver(win, n=0):
    if len(sys.argv) > 3: win.geometry(sys.argv[3])
    def pump(secs):
        end = time.time() + secs
        while time.time() < end:
            win.update(); time.sleep(0.02)
    def walk(w):
        yield w
        for c in w.winfo_children(): yield from walk(c)
    def field(label):
        for w in walk(win):
            if isinstance(w, ttk.Label) and str(w.cget("text")) == label:
                gi = w.grid_info()
                if not gi: continue
                for sib in w.master.grid_slaves(row=int(gi["row"]), column=1):
                    for opt in ("variable", "textvariable"):
                        try:
                            name = str(sib.cget(opt))
                            if name: return sib, name
                        except tk.TclError: pass
        raise SystemExit("no widget for " + label)
    def pick(label, value):            # a box or a list: the choice itself
        win.setvar(field(label)[1], value)
    def type_in(label, value, done):   # a typed field: the letters, then (done) the field is left
        w, name = field(label)
        win.setvar(name, value)
        if done: w.event_generate("<FocusOut>")
    def texts():
        return [str(w.cget("text")) for w in walk(win) if isinstance(w, ttk.Label)]
    pump(1.5)
    check("nothing is written before a choice", ini() == A_INI)
    check("no Save button", not any(isinstance(w, ttk.Button) and str(w.cget("text")) == "Save" for w in walk(win)))
    pick("Skip the intro movies", "0")
    pump(0.15)
    check("not written at once (the timer)", val("dmbmx", "skip_intro") is None)
    pump(1.0)
    check("a ticked box is saved by itself", val("dmbmx", "skip_intro") == "0", repr(val("dmbmx", "skip_intro")))
    check("the file keeps its comment and its unknown line", "# my note" in ini() and val("dmbmx", "foo") == "bar")
    check("the label says Saved", "Saved" in texts())
    # typed fields: nothing is written letter by letter
    type_in("Key: CROSS", "l", False)                       # the first letter of "lshift", then a pause
    pump(1.0)
    check("a half-typed key is not written", val("dmbmx", "key_cross") == "Z, K", repr(val("dmbmx", "key_cross")))
    pick("Rumble", "0")                                     # another choice is saved meanwhile
    pump(1.0)
    check("nor when another choice is saved meanwhile", val("dmbmx", "rumble") == "0" and val("dmbmx", "key_cross") == "Z, K",
          "rumble %r key_cross %r" % (val("dmbmx", "rumble"), val("dmbmx", "key_cross")))
    type_in("Key: CROSS", "lshift", True)
    pump(1.0)
    check("the finished key is written", val("dmbmx", "key_cross") == "lshift", repr(val("dmbmx", "key_cross")))
    type_in("Window size", "12", True)
    pump(1.0)
    check("a value that is not valid is not written", val("dmbmx", "window_size") == "auto", repr(val("dmbmx", "window_size")))
    check("and is named", any(t.startswith("Saved, except window_size") for t in texts()))
    type_in("Window size", "1280x960", True)
    pump(1.0)
    check("the corrected value is written", val("dmbmx", "window_size") == "1280x960")
    # another game is chosen while a save is waiting: it must go to the first game's file
    pick("Internal 3D resolution", "2")
    box = [w for w in walk(win) if isinstance(w, ttk.Combobox) and "Maximum Remix" in " ".join(map(str, w.cget("values")))]
    if not box:
        check("found the game list", False); win.destroy(); return
    names = list(box[0].cget("values")); want = [x for x in names if "Maximum Remix" in x][0]
    box[0].set(want); box[0].event_generate("<<ComboboxSelected>>")
    pump(1.5)
    check("a waiting save goes to the game it was made for", val("dmbmx", "scale") == "2" and val("maxremix", "scale") == "4",
          "dmbmx %r maxremix %r" % (val("dmbmx", "scale"), val("maxremix", "scale")))
    check("the other game's file is not written by showing it", ini("maxremix") == "scale = 4\n")
    pick("Rumble", "0")
    pump(1.0)
    check("a line only the first game's file has does not reach the second", val("maxremix", "rumble") == "0" and val("maxremix", "foo") is None,
          "rumble %r foo %r" % (val("maxremix", "rumble"), val("maxremix", "foo")))
    check("nor the first game's typed values", val("maxremix", "key_cross") == "Z, K" and val("maxremix", "window_size") == "auto",
          "key_cross %r window_size %r" % (val("maxremix", "key_cross"), val("maxremix", "window_size")))
    pick("Graphics API", "vulkan")
    type_in("Window size", "800x600", False)                 # typed in full, the cursor still in the field
    win.tk.call(win.protocol("WM_DELETE_WINDOW"))            # the window is closed before the timer runs out
    check("closing the window saves a waiting choice", val("maxremix", "renderer") == "vulkan", repr(val("maxremix", "renderer")))
    check("and a typed value still in its field", val("maxremix", "window_size") == "800x600", repr(val("maxremix", "window_size")))
    check("and leaves the first game alone", val("dmbmx", "renderer") == "gl" and val("dmbmx", "window_size") == "1280x960")
    try:
        win.winfo_exists(); closed = False
    except tk.TclError:
        closed = True
    check("the window is closed", closed)

def run_log():
    """The run log, through the launcher's own play(): a small script stands in for the game (python runs it where the
    game's exe would run the disc). It says one line, waits for a file named stop, says another and ends. It lets go of
    the log (and with it the lock) before it makes its done file, so a done file means the log is free."""
    g = os.path.join(T, "game", "dmbmx")
    game = os.path.join(T, "standin.py")
    open(game, "w").write("import os, sys, time\nprint('[game] started %d' % os.getpid(), flush=True)\n"
                          "for i in range(300):\n    if os.path.exists('stop'): break\n    time.sleep(0.1)\n"
                          "print('[game] ended', flush=True)\nos.close(1); os.close(2)\nopen('done.%d' % os.getpid(), 'w').close()\n")
    P = dml.Paths(T); P.use("dmbmx"); P.exe = sys.executable
    C = {"debug_console": False, "debug_flags": []}
    cur, old, stop = (os.path.join(g, n) for n in (dml.RUN_LOG, dml.RUN_LOG_BEFORE, "stop"))
    said = []
    def rd(p):
        return open(p, "rb").read().decode() if os.path.isfile(p) else None
    def wait(cond, secs=15):
        end = time.time() + secs
        while time.time() < end and not cond(): time.sleep(0.05)
        return cond()
    def done():
        return len([n for n in os.listdir(g) if n.startswith("done.")])
    dml.play(P, C, game, said.append)
    wait(lambda: "[game] started" in (rd(cur) or ""))
    a = rd(cur) or ""
    check("run log: it has the launcher's line, then the game's", a.startswith("[launcher] version") and "[game] started" in a, repr(a[:70]))
    check("run log: the launcher says where it is", any(dml.RUN_LOG in s for s in said), repr(said[-1:]))
    check("run log: no log of a run before yet", rd(old) is None)
    n = len(said)
    dml.play(P, C, game, said.append)                        # a second game while the first still writes its log
    check("run log: a start while a game runs gets no log, and says so", any("no run log this time" in s for s in said[n:]), repr(said[n:]))
    time.sleep(0.5)
    check("run log: the running game's log is left alone", rd(cur) == a and rd(old) is None, repr(rd(cur)))
    open(stop, "w").close()
    check("run log: both stand-in games ended", wait(lambda: done() == 2), "%d of 2" % done())
    os.remove(stop)
    a = rd(cur) or ""
    check("run log: the game's last line is in it", a.rstrip().endswith("[game] ended"), repr(a[-40:]))
    dml.play(P, C, game, said.append)
    wait(lambda: "[game] started" in (rd(cur) or ""))
    b = rd(cur) or ""
    check("run log: the next start moves it to the log of the run before", rd(old) == a)
    check("run log: and starts a new one", b.startswith("[launcher] version") and "[game] started" in b and "[game] ended" not in b, repr(b[:70]))
    open(stop, "w").close()                                 # stays: the games started below end at once
    check("run log: the third stand-in game ended", wait(lambda: done() == 3), "%d of 3" % done())
    # Two launchers at the same moment, without timing: the other launcher's step is put inside this one's.
    def put(text):
        open(cur, "w", newline="\n").write(text)
    if os.name != "nt":
        # the other launcher moves the log and makes its own between this launcher's open and its lock
        real_lock, calls = dml._log_lock, []
        def lock(f):
            if not calls:
                os.rename(cur, old); put("the other launcher's log 1\n")
            calls.append(1); real_lock(f)
        dml._log_lock = lock; n = len(said)
        try: dml.play(P, C, game, said.append)
        finally: dml._log_lock = real_lock
        check("run log: a log moved away under this launcher: the new one at its name is not moved",
              rd(cur) == "the other launcher's log 1\n" and any("no run log this time" in s and "at the same moment" in s for s in said[n:]), repr((rd(cur), said[n:])))
    # the other launcher makes its log between this launcher's move and its own new log
    real_replace = os.replace
    def replace(a, b):
        real_replace(a, b); put("the other launcher's log 2\n")
    os.replace = replace; n = len(said)
    try: dml.play(P, C, game, said.append)
    finally: os.replace = real_replace
    check("run log: a log made by another launcher in the same moment is not opened or emptied",
          rd(cur) == "the other launcher's log 2\n" and any("no run log this time" in s and "exists" in s for s in said[n:]), repr((rd(cur), said[n:])))
    wait(lambda: done() == (4 if os.name == "nt" else 5))

tk.Tk.mainloop = driver
dml.gui(T)
run_log()
print("%d of %d checks passed" % (sum(res), len(res)))
sys.exit(0 if all(res) else 1)
