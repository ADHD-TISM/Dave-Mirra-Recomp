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
                for sib in [x for top in w.master.grid_slaves(row=int(gi["row"]), column=1) for x in walk(top)]:   # a key field is inside a frame
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
    check("the window has the launcher's name", win.title() == dml.APP_NAME == "Dave Mirra Freestyle BMX", repr(win.title()))
    icon = dml.icon_file()
    check("the icon is found next to the script", icon == os.path.join(os.path.dirname(os.path.abspath(SRC)), "icon.ico"), repr(icon))
    png = dml.icon_png(icon) if icon else None
    check("the icon's largest picture is a 256-pixel PNG", bool(png) and png[16:24] == (256).to_bytes(4, "big") * 2)
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
    # a key field's two buttons save by themselves
    def key_button(label, text):
        return [b for b in field(label)[0].master.winfo_children() if isinstance(b, ttk.Button) and str(b.cget("text")) == text][0]
    def press_in_dialog(label, keysym):   # "Press a key...", then the key in the small window it opens
        key_button(label, "Press a key...").invoke()
        pump(0.3)
        tops = [w for w in win.winfo_children() if isinstance(w, tk.Toplevel)]
        if tops: tops[-1].event_generate("<KeyPress>", keysym=keysym)
        pump(1.0)
        return len(tops)
    key_button("Key: SQUARE", "Clear").invoke()
    pump(1.0)
    check("Clear writes none", val("dmbmx", "key_square") == "none", repr(val("dmbmx", "key_square")))
    n_tops = press_in_dialog("Key: SQUARE", "F5")
    check("Press a key: the key is written", n_tops == 1 and val("dmbmx", "key_square") == "f5", "%d windows, %r" % (n_tops, val("dmbmx", "key_square")))
    press_in_dialog("Key: SQUARE", "g")
    check("Press a key: a second key is added", val("dmbmx", "key_square") == "f5, G", repr(val("dmbmx", "key_square")))
    press_in_dialog("Key: fullscreen", "F12"); press_in_dialog("Key: fullscreen", "F4")
    check("Press a key: a display key holds one key", val("dmbmx", "key_fullscreen") == "f4", repr(val("dmbmx", "key_fullscreen")))
    type_in("Key: in-game menu", "f1", True)
    pump(1.0)
    check("the menu key is saved", val("dmbmx", "key_menu") == "f1", repr(val("dmbmx", "key_menu")))
    type_in("Key: in-game menu", "f1, f2", True)
    pump(1.0)
    check("the menu key holds one key", val("dmbmx", "key_menu") == "f1", repr(val("dmbmx", "key_menu")))
    type_in("Key: in-game menu", "f1", True)                # a valid value again, so that the checks below see only their own field
    pump(1.0)
    check("Press a key: its window is gone", not [w for w in win.winfo_children() if isinstance(w, tk.Toplevel)])
    pick("Pad: in-game menu", "ls+rs")
    pump(1.0)
    check("the gamepad menu controls are saved", val("dmbmx", "pad_menu") == "ls+rs", repr(val("dmbmx", "pad_menu")))
    for k, v, want in (("volume", "55", True), ("volume", "101", False), ("volume", "x", False), ("pad_menu", "a+b", True),
                       ("pad_menu", "Back + Start", True), ("pad_menu", "none", True), ("pad_menu", "a+a", False),
                       ("pad_menu", "none+a", False), ("pad_menu", "ls+", False), ("pad_menu", "a+b+x", False)):
        check("valid_setting %s = %s is %s" % (k, v, want), dml.valid_setting(k, v) == want)
    pick("Volume (percent)", "50")
    pump(1.0)
    check("the volume is saved", val("dmbmx", "volume") == "50", repr(val("dmbmx", "volume")))
    pick("Mouse: left button", "circle")
    pump(1.0)
    check("a mouse button is saved", val("dmbmx", "mouse_left") == "circle", repr(val("dmbmx", "mouse_left")))
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
    # a save writes only what was changed here, so these two lines are not in the second file at all
    check("nor the first game's typed values", val("maxremix", "key_cross") in (None, "Z, K") and val("maxremix", "window_size") in (None, "auto"),
          "key_cross %r window_size %r" % (val("maxremix", "key_cross"), val("maxremix", "window_size")))
    check("a save writes only the changed setting", ini("maxremix") == "scale = 4\nrumble = 0\n", repr(ini("maxremix")))
    # the game writes this file too (its in-game menu): what it changed must survive the launcher's next save
    time.sleep(0.05)
    open(os.path.join(T, "game", "maxremix", "dmrecomp.ini"), "w", newline="\n").write("scale = 2\nrumble = 0\nfilter = smooth\n")
    pick("Rumble", "1")
    pump(1.0)
    check("a change made by the game survives a save here", val("maxremix", "scale") == "2" and val("maxremix", "filter") == "smooth" and val("maxremix", "rumble") == "1",
          repr(ini("maxremix")))
    check("and is shown", win.getvar(field("Internal 3D resolution")[1]) == "2" and win.getvar(field("Picture filter")[1]) == "smooth",
          "%r %r" % (win.getvar(field("Internal 3D resolution")[1]), win.getvar(field("Picture filter")[1])))
    time.sleep(0.05)
    open(os.path.join(T, "game", "maxremix", "dmrecomp.ini"), "w", newline="\n").write("scale = 1\nrumble = 1\nfilter = nearest\n")
    pick("Internal 3D resolution", "4")                      # changed in both places: the choice made here counts
    pump(1.0)
    check("a setting changed in both places keeps the launcher's value", val("maxremix", "scale") == "4" and val("maxremix", "filter") == "nearest",
          repr(ini("maxremix")))
    pick("Internal 3D resolution", "2")
    pump(1.0)
    # a .chd picked with Browse after the window is open: its "Disc written out" row shows, nothing raises; a .cue
    # picked after it hides the row again
    import struct
    from tkinter import filedialog
    errors = []
    real_report, real_ask = win.report_callback_exception, filedialog.askopenfilename
    win.report_callback_exception = lambda *a: errors.append("%s: %s" % (a[0].__name__, a[1]))
    fake_chd = os.path.join(T, "picked.chd")
    open(fake_chd, "wb").write(b"MComprHD" + struct.pack(">II", 124, 5) + b"cdlz" + b"cdzl" + bytes(8)
                               + struct.pack(">QQQ", 19584, 124, 0) + struct.pack(">II", 19584, 2448) + bytes(60))
    fake_cue = os.path.join(T, "picked.cue")
    open(fake_cue, "w").write('FILE "picked.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    open(os.path.join(T, "picked.bin"), "wb").write(bytes(2352 * 20))
    browse_btn = [w for w in walk(win) if isinstance(w, ttk.Button) and str(w.cget("text")) == "Browse..."]
    def shown_row():
        # packed on the Play tab (on screen once that tab is the one shown)
        return any(isinstance(w, ttk.Label) and "Disc written out" in str(w.cget("text")) and w.winfo_manager() == "pack" for w in walk(win))
    try:
        filedialog.askopenfilename = lambda **kw: fake_chd
        browse_btn[0].invoke(); pump(0.5)
        check("a .chd picked after the window opens raises nothing", not errors, repr(errors))
        check("and shows its 'Disc written out' row", shown_row())
        filedialog.askopenfilename = lambda **kw: fake_cue
        browse_btn[0].invoke(); pump(0.5)
        check("a .cue picked after it hides that row again", not shown_row() and not errors, repr(errors))
    finally:
        filedialog.askopenfilename, win.report_callback_exception = real_ask, real_report
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
    last = 4 if os.name == "nt" else 5
    check("run log: the last stand-in games ended", wait(lambda: done() == last), "%d of %d" % (done(), last))

def lone_script():
    """A launcher script without icon.ico next to it (as update-launcher leaves it) finds no icon and still opens."""
    d = os.path.join(T, "alone"); os.makedirs(d, exist_ok=True)
    shutil.copy(SRC, os.path.join(d, "dmlauncher.py"))
    sp = importlib.util.spec_from_file_location("dml_alone", os.path.join(d, "dmlauncher.py"))
    lone = importlib.util.module_from_spec(sp); sp.loader.exec_module(lone)
    check("a script without icon.ico finds no icon", lone.icon_file() is None, repr(lone.icon_file()))
    w = tk.Tk()
    try:
        lone.set_window_icon(w, tk)
        err = None
    except Exception as e:
        err = e
    finally:
        w.destroy()
    check("and setting up its window raises nothing", err is None, "" if err is None else repr(err))
    first = tk.Tk()   # a root made before: the picture must still belong to the window it is for
    w = tk.Tk(); real = dml.WINDOWS; dml.WINDOWS = False   # the Linux way, which Tk on Windows can do as well
    try:
        dml.set_window_icon(w, tk)
        img = getattr(w, "icon_image", None)
        mine = img is not None and str(img) in w.tk.call("image", "names")   # in this window's Tk, not the first root's
        check("off Windows the window gets the icon as a 256-pixel picture of its own", mine and img.width() == 256,
              repr((mine, img and img.width())))
    finally:
        dml.WINDOWS = real; w.destroy(); first.destroy()

def new_paths():
    """The paths RecompLauncher uses: damaged CHDs, --data, prepare's arguments and exit codes, run's words."""
    import struct
    R = os.path.join(T, "rl"); os.makedirs(R, exist_ok=True)
    # a CHD cut short (still being copied) or with a map past its end is refused with a Fail, never a crash
    disc = os.path.join(R, "Dave Mirra BMX"); os.makedirs(disc, exist_ok=True)
    short = os.path.join(disc, "short.chd")
    open(short, "wb").write(b"MComprHD" + bytes(32))
    head = b"MComprHD" + struct.pack(">II", 124, 5) + b"cdlz" + b"cdzl" + bytes(8) + struct.pack(">QQQ", 19584 * 4, 1 << 30, 0)
    head += struct.pack(">II", 19584, 2448) + bytes(60)
    nomap = os.path.join(disc, "nomap.chd")
    open(nomap, "wb").write(head)
    for name, p in (("cut short", short), ("map past its end", nomap)):
        try:
            dml.iso_root_files(p); ok, why = False, "no error"
        except dml.Fail as e:
            ok, why = "damaged or not complete" in str(e), str(e)
        except Exception as e:
            ok, why = False, "%s: %s" % (type(e).__name__, e)
        check("a .chd %s is refused with a message" % name, ok, why[:90])
    # a header with no unit size, and a track list that points back at itself, are messages (no crash, no hang)
    odd = os.path.join(R, "odd"); os.makedirs(odd, exist_ok=True)
    nounit = os.path.join(odd, "nounit.chd")
    open(nounit, "wb").write(head[:60] + struct.pack(">I", 0) + head[64:])
    track1 = b"TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:8 PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0\0"
    loop = os.path.join(odd, "loop.chd")   # uncompressed, 1 hunk, map at 124, its one track entry at 128 points at 128
    open(loop, "wb").write(b"MComprHD" + struct.pack(">II", 124, 5) + bytes(16) + struct.pack(">QQQ", 19584, 124, 128)
                           + struct.pack(">II", 19584, 2448) + bytes(60) + bytes(4)
                           + b"CHT2" + struct.pack(">IQ", len(track1), 128) + track1)
    for name, p, said in (("with no unit size", nounit, "not a CD image"), ("whose track list loops", loop, "loops")):
        try:
            dml.iso_root_files(p); ok, why = False, "no error"
        except dml.Fail as e:
            ok, why = said in str(e), str(e)
        except Exception as e:
            ok, why = False, "%s: %s" % (type(e).__name__, e)
        check("a .chd %s is refused with a message" % name, ok, why[:90])
    P = dml.Paths(R); C = dml.Config(P.cfg)
    try:
        found = dml.find_discs(P, C); check("the disc folder with damaged .chd files is looked through", found == {}, repr(found))
    except Exception as e:
        check("the disc folder with damaged .chd files is looked through", False, "%s: %s" % (type(e).__name__, e))
    # --data: the cards are copied once, never moved, never replaced
    P.data = os.path.join(R, "recompdata"); P.use("dmbmx")
    os.makedirs(P.game, exist_ok=True)
    card = os.path.join(P.game, "memcard1.mcd"); open(card, "wb").write(b"A" * 128)
    said = []
    dml.bring_saves(P, said.append)
    copy = os.path.join(P.home, "memcard1.mcd")
    check("--data: the card is copied", os.path.isfile(copy) and open(copy, "rb").read() == b"A" * 128 and os.path.isfile(card))
    open(copy, "wb").write(b"B" * 128)
    dml.bring_saves(P, said.append)
    check("--data: a card already there is never replaced", open(copy, "rb").read() == b"B" * 128 and open(card, "rb").read() == b"A" * 128)
    # prepare: wrong words, no disc, and downloads needed but not allowed
    E = os.path.join(T, "rl-empty"); os.makedirs(E, exist_ok=True)
    fake = os.path.join(E, "fake.bin"); open(fake, "wb").write(bytes(2352 * 20))
    check("prepare: a word it does not know gives exit 1", dml.cli(["--root", E, "prepare", "--bogus"]) == 1)
    check("prepare: no disc gives exit 1", dml.cli(["--root", E, "prepare"]) == 1)
    check("prepare: downloads needed but not allowed give exit 3", dml.cli(["--root", E, "prepare", "--dmbmx=" + fake]) == 3)
    # run: the words after it go to the game, the game's exit code comes back, --data is where it runs
    open(P.exe, "wb").write(b"")
    seen = {}

    class FakeGame:
        def __init__(self, args, **kw):
            seen["args"], seen["cwd"] = args, kw.get("cwd")

        def wait(self):
            return 7
    real = dml.subprocess.Popen
    dml.subprocess.Popen = FakeGame
    try:
        rc = dml.cli(["--root", R, "run", "--game", "dmbmx", "--disc", fake, "--data", P.data, "--set", "volume=0"])
    finally:
        dml.subprocess.Popen = real
    check("run: the game's exit code comes back", rc == 7, repr(rc))
    check("run: the words after it go to the game", seen.get("args", [])[-2:] == ["--set", "volume=0"], repr(seen.get("args")))
    check("run: with --data the game runs in the data folder", os.path.normcase(seen.get("cwd") or "") == os.path.normcase(P.home),
          repr(seen.get("cwd")))
    check("run: the log is copied to the top of the data folder", os.path.isfile(os.path.join(P.data, dml.RUN_LOG)))
    # a game that cannot be started gives exit 4 (recomplauncher.json says what it means), never the game's own 1
    N = os.path.join(T, "rl-notbuilt"); os.makedirs(N, exist_ok=True)
    rc = dml.cli(["--root", N, "run", "--game", "dmbmx", "--disc", fake])
    check("run: a game not built yet gives exit 4", rc == dml.RUN_NOT_STARTED == 4, repr(rc))
    rc = dml.cli(["--root", N, "run", "--game", "dmbmx", "--disc", os.path.join(N, "moved", "gone.cue")])
    check("run: a disc image that has moved gives exit 4", rc == 4, repr(rc))
    rc = dml.cli(["--root", N, "status", "--game", "dmbmx", "--disc", os.path.join(N, "moved", "gone.cue")])
    check("other words with a disc that has moved still give exit 1", rc == 1, repr(rc))

def helpers():
    """The small helpers the release and the CHD write-out rest on."""
    import struct
    H = os.path.join(T, "helpers"); os.makedirs(H, exist_ok=True)
    # release_tag: the version a packaged launcher carries; nothing, empty or odd text gives None
    had = hasattr(sys, "_MEIPASS")
    try:
        check("release_tag: none outside a packaged launcher", (dml.release_tag() is None) if not had else True)
        sys._MEIPASS = H
        for text, want in (("v1.2.3\n", "v1.2.3"), ("\n", None), ("../x\n", None), ("1.2.3\n", None)):
            open(os.path.join(H, "release_tag.txt"), "w").write(text)
            check("release_tag: %r gives %r" % (text.strip(), want), dml.release_tag() == want, repr(dml.release_tag()))
    finally:
        if not had:
            del sys._MEIPASS
    # child_env: a packaged Linux launcher gives its programs the library path from before it started
    frozen, windows = dml.FROZEN, dml.WINDOWS
    try:
        dml.FROZEN, dml.WINDOWS = True, False
        e = dml.child_env({"LD_LIBRARY_PATH": "/bundle", "LD_LIBRARY_PATH_ORIG": "/usr/lib/x", "A": "1"})
        check("child_env: LD_LIBRARY_PATH back from LD_LIBRARY_PATH_ORIG", e.get("LD_LIBRARY_PATH") == "/usr/lib/x"
              and "LD_LIBRARY_PATH_ORIG" not in e and e.get("A") == "1", repr(e))
        e = dml.child_env({"LD_LIBRARY_PATH": "/bundle"})
        check("child_env: no LD_LIBRARY_PATH when there was none before", "LD_LIBRARY_PATH" not in e, repr(e))
        dml.FROZEN, dml.WINDOWS = True, True
        check("child_env: unchanged on Windows", dml.child_env({"X": "1"}) == {"X": "1"})
        dml.FROZEN = False
        check("child_env: unchanged when not packaged", dml.child_env(None) is None)
        # default_root: APPIMAGE counts only when this program is inside APPDIR
        dml.FROZEN, dml.WINDOWS = True, False
        exe_real, env_keep = sys.executable, {k: os.environ.get(k) for k in ("APPIMAGE", "APPDIR")}
        try:
            mine = os.path.join(H, "AppDir"); os.makedirs(mine, exist_ok=True)
            sys.executable = os.path.join(mine, "Dave Mirra Freestyle BMX")
            os.environ["APPIMAGE"] = os.path.join(H, "far", "Game.AppImage")
            os.environ["APPDIR"] = mine
            check("default_root: our own AppImage keeps its folders next to the AppImage file",
                  dml.default_root() == os.path.join(H, "far"), dml.default_root())
            os.environ["APPDIR"] = os.path.join(H, "OtherAppDir")
            check("default_root: an APPIMAGE inherited from another program is not ours",
                  dml.default_root() == mine, dml.default_root())
        finally:
            sys.executable = exe_real
            for k, v in env_keep.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
    finally:
        dml.FROZEN, dml.WINDOWS = frozen, windows
    # CHD: a header that claims more than a CD holds is refused; the write-out folder follows the header's SHA-1
    head = lambda logical, sha: (b"MComprHD" + struct.pack(">II", 124, 5) + b"cdlz" + b"cdzl" + bytes(8)
                                 + struct.pack(">QQQ", logical, 124, 0) + struct.pack(">II", 19584, 2448) + bytes(20) + sha + bytes(20))
    huge = os.path.join(H, "huge.chd"); open(huge, "wb").write(head(1 << 62, b"\1" * 20))
    try:
        dml.Chd(huge); ok, why = False, "no error"
    except dml.Fail as e:
        ok, why = "not a CD image" in str(e), str(e)
    except Exception as e:
        ok, why = False, "%s: %s" % (type(e).__name__, e)
    check("a .chd header that claims more than a CD holds is refused", ok, why[:80])
    dml.CHD_CACHE[0] = os.path.join(H, "chd")
    a, b = os.path.join(H, "a.chd"), os.path.join(H, "moved", "b.chd")
    os.makedirs(os.path.dirname(b), exist_ok=True)
    open(a, "wb").write(head(19584, b"\2" * 20)); open(b, "wb").write(head(19584, b"\2" * 20))
    check("a moved or renamed .chd finds the same written-out copy", dml.chd_out_paths(a) == dml.chd_out_paths(b))
    check("not written out yet: chd_ready is False", not dml.chd_ready(a))
    d, cue, binp = dml.chd_out_paths(a)
    os.makedirs(d, exist_ok=True)
    for p in (cue, binp, os.path.join(d, "done")):
        open(p, "w").close()
    check("written out: chd_ready is True", dml.chd_ready(a) and dml.chd_ready(b))
    # the map reader on scripted bits (the Huffman codes and the bit fields), without a real CHD
    import io, binascii
    script = {}

    class FakeBits:
        def __init__(self, data):
            self.vals = list(script["bits"])

        def read(self, n):
            return self.vals.pop(0)

    class FakeHuff:
        def __init__(self, bits):
            self.codes = list(script["codes"])

        def decode(self, bits):
            return self.codes.pop(0)

    def read_map(codes, bits, firstoffs, stored, widths=(20, 16, 16)):
        script.update(codes=codes, bits=bits)
        raw = b"".join(bytes([c]) + l.to_bytes(3, "big") + (o & (2 ** 48 - 1)).to_bytes(6, "big") + k.to_bytes(2, "big")
                       for c, l, o, k in stored)
        mh = struct.pack(">I", 0) + firstoffs.to_bytes(6, "big") + struct.pack(">H", binascii.crc_hqx(raw, 0xFFFF)) + bytes(widths) + b"\0"
        c = dml.Chd.__new__(dml.Chd)
        c.path, c.f, c.size, c.hunks, c.hunkbytes, c.unitbytes = "x.chd", io.BytesIO(mh), 10 ** 9, len(codes), 19584, 2448
        c.codecs = [b"cdlz", b"cdzl", b"\0\0\0\0", b"\0\0\0\0"]
        return c._read_map(0)
    real_bits, real_huff = dml.ChdBits, dml.ChdHuffman
    dml.ChdBits, dml.ChdHuffman = FakeBits, FakeHuff
    try:
        m = read_map([4, 5, 5, 5], [7, 0, 1, 2], 1000, [(4, 19584, 1000, 7), (5, 0, 0, 0), (5, 0, 1, 0), (5, 0, 2, 0)])
        check("CHD map: a copy of a copy points at the stored hunk", [e[1] for e in m[1:]] == [0, 0, 0], repr(m))
        for name, args in (("a bit width too wide", ([0], [10, 1], 1000, [(0, 10, 1000, 1)], (25, 16, 16))),
                           ("an offset past 48 bits", ([0, 0], [10, 1, 10, 2], 2 ** 48 - 5, [(0, 10, 2 ** 48 - 5, 1), (0, 10, 2 ** 48 + 5, 2)]))):
            try:
                read_map(*args[:4], **({"widths": args[4]} if len(args) > 4 else {}))
                ok, why = name == "an offset past 48 bits", "read (its hunk fails when read)"
            except dml.Fail as e:
                ok, why = True, str(e)
            except Exception as e:
                ok, why = False, "%s: %s" % (type(e).__name__, e)
            check("CHD map: %s gives a message, never a crash" % name, ok, why[:80])
    finally:
        dml.ChdBits, dml.ChdHuffman = real_bits, real_huff
    P = dml.Paths(os.path.join(H, "root")); C = dml.Config(P.cfg)
    C.set("disc", a)
    keys = [k for k, _l, _ok, _t in dml.status(P, C)]
    check("a .chd disc adds the 'written out' row to the status", "chd" in keys, repr(keys))

tk.Tk.mainloop = driver
dml.gui(T)
run_log()
lone_script()
new_paths()
helpers()
print("%d of %d checks passed" % (sum(res), len(res)))
sys.exit(0 if all(res) else 1)
