# dmrecomp

Static recompilation of Dave Mirra Freestyle BMX (PS1, NTSC-U, SLUS-01026) and Dave Mirra Freestyle BMX: Maximum Remix
(SLUS-01347) for Windows.

The game's MIPS code is translated to C ahead of time and linked against a small
runtime that stands in for the PS1 hardware. No game code or assets are stored
here: `gen/` is produced from your own disc image, and the game reads everything
else from that image when it runs. The repository does carry a few game-specific
numbers: the addresses of functions in the game's executable that static
analysis cannot find (`games/*/extra_*.txt`) and some constants in the tools and
runtime. You need your own copy of the game; this project is not affiliated
with its publisher or developer, and Dave Mirra Freestyle BMX is their trademark.
No BIOS file is needed.

Released under the MIT license (see `LICENSE`).

## Games

Each supported game has a profile in `games/<id>.json` (its executable, overlays, overlay load address, the one
function the CPU assist must not speed up) and its entry-point lists in `games/<id>/extra_*.txt`. `tools/regen.py`
translates the game whose executable is on the disc and writes `gen/game.h`, which tells the runtime which game
it was built for. `dmbmx` is Dave Mirra Freestyle BMX, `maxremix` is Maximum Remix. The launcher builds each into its
own folder (`game/<id>/`, with its own settings and saves) and picks the game from the disc image.

## Build

Needs Python 3, CMake, MinGW-w64 gcc, and Git Bash (or MSYS2) for the `sh` scripts.

```
python tools/extract_iso.py "<your image>.bin" ../disc
sh tools/regen.sh ../disc        # add a game id (dmbmx, maxremix) to force one
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```

**Linux / Steam Deck (early work in progress, expect rough edges):** the game has an SDL2 front end (`runtime/plat_sdl.c`). It needs gcc, CMake,
SDL2 and OpenGL development files (Debian/Ubuntu: `build-essential cmake libsdl2-dev libgl-dev`), then the same two
`cmake` lines without `-G`. Tested on Ubuntu 22.04 under WSL: scripted runs give the same picture and sound as the
Windows build, at 1x and 2x, with and without OpenGL. Not yet tried: real Linux hardware or a Steam Deck, and sound
output and gamepads there. `cmake -DDM_SDL=ON` builds this front end on Windows too (with SDL2's MinGW package on
`CMAKE_PREFIX_PATH`).

**Steam Deck (untested, written from documentation):** SteamOS has no compiler and a read-only system, so the launcher
downloads a portable one (Zig, about 55 MB, version and checksum pinned) and SDL2's header files into `tools/`, and
links against the SDL2 and OpenGL libraries the Deck already has. Run `python3 dmlauncher.py` in Desktop Mode
(Konsole), build, then use "Add to the applications menu (for Steam)" (or `python3 dmlauncher.py shortcut`) and in
Steam choose Games > Add a Non-Steam Game to My Library. In Game Mode the game starts fullscreen and the Deck's
controls arrive as a normal gamepad through Steam Input. The download-and-build step has been run under WSL with no
compiler on the path, and the game it builds matches the Windows build's pictures and sound. Gamepad handling is checked
with `tools/padtest.c` (virtual controllers). Not verified on a Deck: anything else, including Game Mode, real controllers
and rumble, the launcher window on SteamOS, and sound out of the Deck.

## Run

```
build/dmrecomp.exe "<your image>.cue"
```

Keys: arrows = d-pad, Z = Cross, X = Circle, A = Square, S = Triangle,
Q/W = L1/R1, 1/3 = L2/R2, Enter = Start, Backspace = Select, Tab = fast-forward.
An XInput gamepad also works.

## Settings

The first run writes `dmrecomp.ini` in the working directory. Its defaults give
the original console picture; delete the file to get them back.

| Setting | Values | Key while playing |
| --- | --- | --- |
| `scale` | `1` original, `2`, `4` internal 3D resolution. The CPU draws it, and 4 is 16 times the pixels: use 2 on a laptop | F6 cycles |
| `scale_auto` | `1` drops `scale` by itself when the game cannot hold full speed, `0` never | |
| `gpu_gl` | `1` draws the 2x / 4x picture on the graphics card instead of the CPU (needs OpenGL 2.1; falls back to the CPU picture if the driver cannot). **Early prototype**, off by default | `0` |
| `filter` | `nearest` sharp pixels, `smooth` blended | F7 |
| `aspect` | `4:3`, `stretch` | F8 |
| `integer_scale` | `0`, `1` whole multiples of 240 lines only | |
| `fullscreen` | `0`, `1` | F11 or Alt+Enter |
| `window_size` | `auto` (960x720) or `WxH` | |
| `fullscreen_size` | `native` or `WxH`: switches the display to that mode | |
| `widescreen` | `off`, `16:9`, `16:10`, `21:9` (experimental) | F10 |
| `upscale` | `off`, `fsr` AMD FidelityFX Super Resolution 1 | F9 |
| `fsr_sharpness` | `0`-`100`, how much sharpening FSR adds | |
| `cpu_assist` | `1` hold 30 fps, `0` console timing | |
| `reverb` | `1` console echo effect, `0` dry | |
| `dualshock` | `1` DualShock (sticks, rumble), `0` plain digital pad | |
| `rumble` | `1`, `0` | |
| `player2` | `1` a second gamepad is player 2, `0` off | |
| `key_up` ... `key_r2`, `key_fast_forward` | one or two key names | |
| `pad_up` ... `pad_r3` | a gamepad control: `a b x y lb rb lt rt start back ls rs up down left right none` | |

Command line: `--scale N`, `--fullscreen`, `--console-cpu`, and
`--set name=value` for any setting. Hotkey changes are not written back to the
file.

`widescreen` (experimental) squeezes the game's 3D projection horizontally and stretches the picture to the wider shape, so 3D keeps its
proportions and the sides show more of the scene. It applies only while 3D is being drawn; menus stay 4:3. Text and other 2D elements drawn over a 3D
scene (the HUD, pause and finish menus) are stretched wide, and anything the game itself leaves out beyond its normal view can pop in at the edges.

`upscale = fsr` runs the picture through FSR 1 (edge-directed upscaling plus sharpening, as two OpenGL shaders) when the window is 
larger than the game's internal picture; if the graphics driver lacks the needed OpenGL features it falls back to the plain picture. 
It only changes how the finished picture is enlarged, so it works at any internal `scale`. DirectX 12 and DLSS are not used: the game draws on the CPU, 
and DLSS needs motion and depth data that a software renderer does not have.

At `scale` 1 the picture is pixel-identical to the console-resolution renderer.
At 2 and 4, polygons are drawn a second time into a larger copy of video memory
by worker threads; textures are still sampled at their original resolution, and
sprites, lines and movies are enlarged as blocks. The window is drawn with
OpenGL (GDI if that is unavailable).

Controllers are XInput gamepads: the first is player 1 (together with the
keyboard), the second is player 2. The game sees DualShock pads, detects them
and drives the motors itself. It never switches the pads to analog mode, so the
left stick acts as the d-pad.

Memory card slots 1 and 2 are `memcard1.mcd` and `memcard2.mcd` (standard
128 KB card images) in the working directory. A file there that is not a card
image is never overwritten: it is renamed to `memcard1.mcd.bad` (or `.bad1`,
`.bad2`, ...) and a blank card is started.

Not available: save states (recompiled code runs on the host call stack, which
cannot be snapshotted).

## Launcher

`launcher/dmlauncher.py` is a small window with a game list at the top (it chooses the game from your disc image) (Python with Tk, or the one-file exe) that does the
setup for you on Windows: **Update** downloads the project and a C compiler (about 200 MB) into
folders next to the launcher, **Build** checks your disc image, translates the code and builds
`game/<id>/dmrecomp.exe` (steps that are up to date are skipped), **Play** shows what is ready and starts
the game, and **Settings** edits `game/<id>/dmrecomp.ini`. The launcher makes a **Dave Mirra BMX** folder next to itself: put your .cue and .bin there and they are found automatically. **Close the launcher when the game starts** is a checkbox on the Play tab. A **Dark mode** checkbox (it starts from your Windows setting) switches the window's colours. Updates never touch `game/`, so settings and
saves are kept. `python launcher/dmlauncher.py status|download|update|build|play|update-launcher`
does the same without a window. `sh launcher/build_exe.sh` (needs PyInstaller) packs it into one file,
`launcher/dist/DaveMirraLauncher.exe`, which needs no Python installed and holds no game code,
so it can be shared; put it in its own folder and run it. On Linux run the script, `python3 launcher/dmlauncher.py`
(the window needs Tkinter, `python3-tk` on Debian/Ubuntu; `status`, `download`, `build` and `play` work without it). There it
uses the system's compiler and SDL2 instead of downloading a compiler, and says what to install when they are missing.
Tested under WSL from the command line only; the Steam Deck, which has no compiler of its own, is not handled yet.

## Packaging

```
sh tools/package.sh "<your image>.cue" [out_dir]
```

extracts the disc, regenerates `gen/`, builds, and writes `dist/` (or
`out_dir`) with `dmrecomp.exe` and a short README. The exe needs only DLLs that
ship with Windows. It contains code translated from your disc, so keep it for
your own use.

If the game stops making progress (no display flips, movie frames, disc reads or drawing
for 10 seconds), the console window prints a `[watchdog]` report: the guest's
return addresses and the last BIOS calls. Leave the window open and send it along
with a bug report.

Test options: `--shot-window` (screenshots show the window contents instead of
the internal picture), `--stats`, `--log-bios`, `--log-sio`, `--turbo`, `--auto-start N`
(press Start every N frames until the script begins), `--shot-every N` (PNG every
N frames), `--wav file` (raw 44.1 kHz stereo s16 dump of the mixed audio) and
`--script "frame:btn,frame:btn,..."` (press a button for 6 frames; buttons are
`u d l r x o s t st se l1 r1 l2 r2`) or `--script-file FILE` (the same list read
from a file). `--set player2=2` mirrors scripted input to player 2 for testing. `--ramdump f1,f2,...` writes the 2 MB
guest RAM at those frames and `--poke frame:addr:val[:size]` writes guest memory.

`--console-cpu` turns off the CPU assist. The game draws a frame every two
vblanks (30 fps) and slows down when a frame's work runs past the second one.
By default, instructions executed between the first and second vblank after a
display flip are charged half the console's time, which removes those dips
without ever letting the game run faster than 30 fps. The libetc VSync wait
loop is excluded (`fullcost` in `games/dmbmx.json`) because its
iteration-count timeout would fire early. `--stats` prints `flips=` (frames
the game showed in the last second).

## Layout

- `tools/recomp.py` — MIPS R3000A to C recompiler (PS-X EXE and raw overlays).
- `tools/regen.py` (`regen.sh`) — regenerates `gen/` for the main EXE, both overlays and the movie player, from a game profile.
- `tools/iterate.sh` — runs the game (in `testrun/`, apart from your saves and settings) and adds any missing function entry to `extra_<module>.txt`.
- `tools/sweep.py` — coverage sweep: scripted runs through every mode, level and menu, each under `iterate.sh`. Works for either game (the one `gen/` was built for). `--card FILE --unlocked` plays each level unlocked on a save, `--probe MODE` writes a contact sheet of the level list; see its header.
- `tools/package.sh` — disc image to ready-to-run folder.
- `games/` — one profile and one set of `extra_*.txt` entry-point lists (function pointers, setjmp targets the static analysis can't find) per game.
- `runtime/` — CPU glue and scheduler, HLE BIOS, GPU (software), GTE, CD-ROM, MDEC, SPU, and the host: `host_common.c` (settings, input mapping, presentation, pacing) on top of one platform file, `plat_win32.c` or `plat_sdl.c` (`host.h` is the interface between them).

## Modules

| Module | File on disc | Load address |
| --- | --- | --- |
| main | `SLUS_010.26` | `0x80010000` |
| fr (front end) | `FRTEXT.BIN` | `0x800bbb6c` |
| ai (in-game) | `AITEXT.BIN` | `0x800bbb6c` |
| movie | `PLAYMOV.EXE` | `0x80160000` |

`fr` and `ai` share an address range; the runtime checks which one is in RAM before dispatching.

## Status

**In progress: lower-end hardware.** The 2x and 4x internal resolutions are drawn by the CPU and lag on laptops. `scale_auto` (on by default) now steps the resolution down when the game cannot hold full speed, and the 2x/4x drawing is somewhat faster. More work on slow machines is planned; use `scale = 1` or `2` on a laptop for now.

Boots through the intro movies and the front end into gameplay. Scripted runs
(random pedalling, steering, tricks, pause menu) complete without a missing
function in ProQuest, Session and Freeride, in the Options and Cool Stuff
screens, and in level indexes 0-12 (levels past the first are locked on a fresh
game, so they were loaded with `--poke 1903:80089f54:N`, which overwrites the
selected-level byte just after the level is accepted). Emulation takes about
2 ms per frame in-game. Sound has been checked by ear in hand play (music,
effects, movies). The SPU reverb (the console's echo effect) is emulated and
can be turned off with `reverb = 0`; a scripted run with it on stays unclipped
but its effect on the sound has not been judged by ear. Memory card slot 1 is backed by
`memcard1.mcd` (a standard 128 KB card image, created in the working directory
on first use); slot 2 is `memcard2.mcd`. "Save game" on the Run Finished menu writes a
2-block save, and the next boot reads it back into game memory (checked with a
marked high-score record). In hand play with an Xbox pad, a level unlocked by
completing challenges stayed unlocked after a restart. Anything neither the
scripts nor that play session
did not reach may still stop on a missing function, which `tools/iterate.sh`
adds.
