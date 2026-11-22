# Renderer roadmap: adding Vulkan next to OpenGL

Status: plan. Phase 1 (finishing the OpenGL path) is in progress: its tests are written, the NVIDIA and Intel runs are not
done. Phase 2 (the backend seam) is done: the window, its surface, the window picture and the GPU renderer all sit behind
`runtime/gfx.h`. Phase 3 (core-profile OpenGL) is done on the Radeon. Phase 4 is done on the Radeon: Vulkan shows the CPU
picture, FSR included. Phase 5 is done on the Radeon: `gpu_gl = 1` under `renderer = vulkan` draws the 2x and 4x picture on
the card (`docs/vulkan-gpu-renderer-plan.md`). Phase 6 is started (the Radeon under the validation layer, and Linux under WSL
2 with Mesa's OpenGL and software Vulkan drivers). Phase 7 is started: the setting `renderer = auto` exists, the default is
still `gl`. Estimates are rough and are not promises.

## Goal

Draw the picture with Vulkan on Windows and Linux, and keep OpenGL as the fallback for machines where Vulkan is missing or
misbehaves. The CPU renderer stays the console-exact reference and the default. Linux here includes the Steam Deck build,
which the README still marks untested.

OpenGL is formally deprecated only on macOS; Windows and Linux drivers will carry it for years. This is not an emergency.
The reasons to do it anyway are driver futures and having one modern path to test and optimise. No Vulkan-only feature is
needed today.

## Decisions

1. **API: Vulkan directly**, behind a small backend interface of our own. No engine, no SDL3 GPU layer: the project has almost
   no dependencies and should keep it that way.
2. **Platforms: Windows and Linux only.** macOS, and with it MoltenVK, is out of scope for now.
3. **Shaders: compiled SPIR-V is checked into the repository next to the GLSL source**, so users need no shader compiler. A
   contributor who changes a shader regenerates the SPIR-V with `glslang` (or `glslc`) and commits both. Different `glslc` versions produce
   different bytes from the same source, so nothing compares bytes: a hash of the GLSL source (with the files it includes and
   the compiler options used) is recorded next to the SPIR-V. A small check in `tools/` (`tools/shaders.py check`; the
   repo has no test runner) recomputes the hash and fails when the source changed without the SPIR-V being regenerated. It
   needs no compiler, and it cannot tell whether the committed `.spv` is right, only that it is not stale.
4. **OpenGL stays as the fallback**, not only for a transition period. The full chain is Vulkan, then OpenGL, then the
   platform's software stretch (`plat_soft_present`, which exists today for machines without OpenGL). OpenGL is removed only
   if it becomes a burden and Vulkan has proved itself on real machines.
5. **Vulkan is loaded at run time**, not linked: the program opens `vulkan-1.dll` or `libvulkan.so.1` and gets the rest through
   `vkGetInstanceProcAddr`, much as `plat_gl_proc` looks up OpenGL entry points (OpenGL itself stays linked, as it is today).
   A machine without a Vulkan loader must still start
   and use the fallback chain. The Vulkan headers are committed (or fetched like SDL2's headers on the Deck route), so no
   system package is needed to build.

## Where OpenGL is today

- `runtime/plat_win32.c` and `runtime/plat_sdl.c` own the window and its surface:
  `plat_surface_open(kind, ...)` creates both for one kind of surface (`PLAT_SURF_GL`: a window with an OpenGL 3.3 core context,
  `SDL_WINDOW_OPENGL` or a pixel format on Win32; `PLAT_SURF_SOFT`: a plain window) and `plat_surface_close` removes them.
  `plat_swap` shows the frame, and `plat_gl_proc` looks up entry points (`runtime/host.h`). `plat_soft_present` is the
  existing no-OpenGL path.
- `runtime/gfx_gl.c` (the OpenGL backend of `runtime/gfx.h`) draws the window picture with GL: upload of the CPU picture, the
  sharp and smooth filter, FSR (two GLSL 3.30 shaders), and the `--shot-window` grab. `runtime/gfx.c` is the software
  backend. `runtime/host_common.c` builds each frame and hands it to `gfx->present`; `runtime/gpu.c` reaches the GPU
  renderer through `gfx->r_*`.
- `runtime/gpu_gl.c` (the experimental GPU hi-res renderer, `gpu_gl=1`) draws the 2x and 4x picture. Its whole interface is
  the seven functions in `runtime/gpu_gl.h`; `runtime/gpu.c` never touches GL. It also holds the pixel-exact logic: the vram
  mirror texture, the PSX texture-decode shader, the mask bit kept as a stencil bit only while check-mask is on, and two-pass
  translucency. See `docs/gpu-renderer-testing.md`.
- The renderer and the window picture are coupled: `glg_present` copies the picture into the host's bound GL texture.

A replacement has to cover the GPU renderer, the window presentation and the window itself.

## Phases

Each phase ends with its exit test. Nothing moves on until the test passes on the Radeon baseline and the OpenGL output has
no regressions against it (phases 1 and 3 change the OpenGL code on purpose; they must not change what it draws).

1. **Finish the OpenGL path** (in progress; about a week). Test it on NVIDIA and Intel graphics on Windows and fix what that
   finds (most likely GLSL 1.20 strictness). Done: `tools/gltest.c` now checks that copies the console does not smear keep the
   picture's hi-res detail (it draws a triangle finer than vram's pixels into the source first) and covers the mask bit
   (set-mask, check-mask, copies, and the stencil rebuilt from vram); every case was shown to fail when the matching part of
   `gpu_gl.c` is broken. `tools/gltest.c` is wgl-only, so a Linux/Mesa check of the GL path is either added here (an SDL
   variant of the harness) or deferred, and the Linux GL path stays untested until then.
   *Exit:* `tools/gltest.c` and the scripted CPU-versus-GPU comparison pass on AMD, NVIDIA and Intel on Windows; both games
   play.
2. **Make the seam explicit** (3 to 5 days). One backend interface in front of everything GL: window and surface creation per
   backend (including recreating the window when falling back, as `plat_sdl.c` already does for the no-GL case: a Win32 window
   can have its pixel format set only once, and SDL needs `SDL_WINDOW_VULKAN`), size, swap, present, read-back and the seven
   renderer calls. Today's GL code moves behind it with no behaviour change.
   *Done:* `runtime/gfx.h` defines the backend (`init`, `present`, `grab` and the seven renderer calls); the OpenGL code that
   was in `host_common.c` is now `runtime/gfx_gl.c`, the software stretch is `runtime/gfx.c`, and the backend is chosen once in
   `host_init`. Old and new builds were compared on the same scripted runs: identical screenshots with the CPU picture and with
   the GPU picture, identical window grabs (`--shot-window`, FSR and the smooth filter on) with both, and with the OpenGL
   backend forced off the software path gives the same picture as before.
   Window and surface creation moved too: each backend's `init` calls `plat_surface_open` for its own kind of surface, and a
   failed attempt leaves no window, so the next backend gets a fresh one (a Win32 window takes a pixel format only once; it is
   shown only once its surface works). A Vulkan backend adds a kind (a window with `SDL_WINDOW_VULKAN` on SDL, a plain `HWND`
   on Win32) and, in phase 4, the calls that hand it the platform's surface and instance extensions (`SDL_Vulkan_*`, or the
   `HWND` and `HINSTANCE` for `vkCreateWin32SurfaceKHR`), which `host.h` does not have yet. On Windows the same scripted runs gave identical screenshots and window grabs before and after, and a GL attempt forced to
   fail falls back to the software window with the same pictures; the SDL front end was compile-checked only.
   *Exit:* `gltest` and the scripted run give identical results before and after.
3. **Modernise GL as a stepping stone** (about a week, optional). Replace `glBegin` and the fixed pipeline with vertex
   buffers and core-profile shaders. This is most of the porting work for Vulkan anyway, it drops the compatibility-profile
   requirement, and it can be checked against the baseline before any new API is involved.
   *Done (Radeon only):* the game asks for an OpenGL 3.3 core context (`wglCreateContextAttribsARB` on Win32, the core
   profile attributes on SDL). All vertices go through one streamed buffer (`runtime/gl_stream.c`); the shaders are GLSL 3.30;
   the vram texture is `GL_RG8` instead of luminance-alpha; nothing from the fixed pipeline is left. `gltest` passes all 31
   checks on a real 3.3 core context. Window grabs with the CPU picture and the GPU picture, with FSR, with the smooth filter,
   were identical to the previous build. With the sharp filter at a window size that is not a whole multiple of the picture
   (512 wide into 1280), about 2 % of the pixels differ: the sample points fall exactly on texel edges there and the
   interpolators of the fixed pipeline and of a shader round them differently; neither choice is wrong (every differing pixel
   sits in a window column x with x mod 5 = 2, where the texel coordinate is a whole number). *Cost:* a machine
   whose driver offers less than OpenGL 3.3 (some Windows drivers for early Intel graphics stop at 3.1) no longer gets the
   OpenGL path at all, window picture included, and uses the software stretch. The SDL front end was compile-checked only.
   *Exit:* same as phase 2, on a core-profile context.
4. **Vulkan device and presentation** (about a week). The loader, instance, device and swapchain on both front ends; upload of
   the CPU picture, filter, FSR and the window grab. The backend is chosen once per run for the presenter and the renderer
   together: until phase 5, `gpu_gl=1` keeps the OpenGL presenter, because `glg_present` hands its picture to a GL texture.
   *Done so far (Windows, Radeon):* `runtime/gfx_vk.c` opens the loader at run time (`vulkan-1.dll`), makes an instance, picks
   a device (a discrete one first), makes a swapchain (no vsync wait if the driver offers one) and shows the CPU picture with
   `vkCmdBlitImage`, nearest or linear, black bars included, so it needs no shader. With FSR on it draws EASU into an image of
   the picture's final size and RCAS into the swapchain image, from SPIR-V in `runtime/gfx_vk_spv.c`. It also does the
   `--shot-window` read-back.
   `PLAT_SURF_VK`, `plat_vk_loader`, `plat_vk_extensions` and `plat_vk_surface` are the platform side (Win32, and SDL with
   `SDL_WINDOW_VULKAN`). The setting `renderer = gl | vulkan` (default `gl`) puts Vulkan first in the fallback chain; `auto` is
   phase 7. Window grabs against OpenGL, same runs: smooth filter 64 of 64 identical, sharp filter at a whole-multiple window
   size 64 of 64 identical; at 512 into 1280 the same x mod 5 = 2 texel-edge ties as in phase 3 (blit and shader round the
   tie differently). FSR: 57 of 63 grabs identical and the other 6 differ by at most 1 of 255 in under 0.01 % of the pixels;
   at an odd window size (1001x751) 46 of 64 identical, the rest the same way (the picture is placed like the OpenGL one, its
   offset counted from the bottom). Resizing, minimising and restoring the window work. With the loader made unavailable the run says so
   and uses OpenGL, with identical grabs (64 of 64). Under Vulkan `gpu_gl` was ignored in this phase (the renderer calls said no, so the CPU
   picture was used): the GPU renderer is phase 5. The SDL front end is compile-checked only.
   *Shaders:* the GLSL for Vulkan is in `runtime/shaders` (`fsr_easu.frag` and `fsr_rcas.frag` are ports of the OpenGL ones with
   push constants and a top-left origin, `fullscreen.vert` draws one triangle). `tools/shaders.py build <glslang>` compiles them
   to SPIR-V 1.0 (`-V --target-env vulkan1.0`, glslang 16.6.0, the Khronos release; `glslc` was not available) and writes
   `runtime/gfx_vk_spv.c` and `runtime/shaders/hashes.txt`; `tools/shaders.py check` needs no compiler and fails when a source
   changed without the SPIR-V being regenerated (decision 3, as a Python tool, not C).
   *Not done:* validation layers, NVIDIA, Intel, Mesa and Linux (untested), the SDL front end (compile-checked only).
   *Exit:* every CPU-renderer run, including FSR, looks the same as through OpenGL, and a machine with no Vulkan loader still
   starts through the fallback chain.
5. **Vulkan GPU renderer** (2 to 3 weeks). The vram texture with dirty-row uploads, the offscreen target with stencil, the
   decode shader as SPIR-V, one pipeline per blend and stencil state, two-pass translucency, copies and the check-mask rebuild.
   *Exit:* a Vulkan version of `gltest` (including the check-mask cases from phase 1) passes, and the scripted comparison
   matches the Radeon OpenGL baseline on both games.
   *Done (Windows, Radeon):* `runtime/gpu_vk.c`; `tools/vktest.c` passes, the scripted comparison matches on both games and both
   were played by hand. The steps and the measurements are in `docs/vulkan-gpu-renderer-plan.md`.
6. **Hardware validation** (a week or more). AMD, NVIDIA and Intel on Windows, and Linux with Mesa and NVIDIA drivers, a Steam
   Deck if one is available, with the validation layers on. The least predictable phase: it depends on having the hardware.
   *Exit:* no validation errors, no regressions against OpenGL on any of them.
   *Done so far:* the Radeon on Windows under the validation layer, and Linux under WSL 2 with two Mesa drivers. With the
   Khronos layer (the unpinned nightly build the plan's step 5 used, through `VK_LAYER_PATH`, nothing installed) and
   `DMRECOMP_VK_VALIDATE=1`, the code as of phase 7 gives no layer message in `vktest` (normal, `small`, `decline`, `keep`)
   nor in four scripted game runs: Freestyle BMX with `renderer = vulkan` at 2x and with `auto` at 4x through FSR, Maximum
   Remix with `auto` at 2x and with `vulkan` at 4x through FSR, all with `gpu_gl = 1`; the two plain 2x runs also match the
   OpenGL baseline (64 of 64 and 71 of 71 shots). On Linux, the SDL front end built with gcc and CMake under WSL 2 (Ubuntu
   22.04, SDL 2.0.20, X11 through WSLg) and ran the Freestyle BMX script with `gpu_gl = 1` at 2x on Mesa 23.2.1's D3D12
   driver (`GL_RENDERER` "D3D12 (AMD Radeon(TM) Graphics)", OpenGL 4.2 core): 75 of 75 shots are identical to the Windows
   Radeon OpenGL baseline. That system has no Vulkan loader, so it is also the roadmap's machine without one: `renderer =
   vulkan` and `renderer = auto` both said so (`[video] no Vulkan loader (Failed loading libvulkan.so.1 ...)`), went to
   OpenGL, and gave the same shots (74 of 74 each). Then, with Ubuntu's `mesa-vulkan-drivers` and `vulkan-validationlayers`
   installed, Vulkan on Linux through Mesa's software driver (lavapipe: `llvmpipe (LLVM 15.0.7, 256 bits)`, depth-stencil
   D24S8, the first run on that format): `renderer = vulkan` and `renderer = auto` both use it, give the same shots as each
   other (68 of 68), and the layer (1.3.204) gives no message with the GPU renderer or with the CPU picture. Against the
   Windows OpenGL baseline the lavapipe shots are the same scenes but not the same pixels: 6 of 69 identical, at most 0.15 %
   of a shot's pixels differ (0.03 % on average), nine in ten by 4 or 8 of 255 and most of the rest by 1 or 2 (largest 200).
   The 11132 differing pixels of the 69 shots were located: 8740 of them (79 %) have no differing pixel among their eight
   neighbours and 1238 more are in pairs; of the 710 in groups of six or more, 26 are in a group shaped like a line (six or
   more long, at most two wide) and 684 in patches, so almost none form lines; whether they lie on polygon edges was not
   checked. Two thirds (66.5 %) are not on a colour edge of the picture; the other third is. By size: 43 % differ by 8, which
   is one step of the 5-bit colour the shader rounds to (`floor(col * 255 + 0.5)`, then `floor(m / 8)`), 47 % by 4, 8 % by 1
   or 2, and 1 % by other amounts (7, and 12 to 200). That is what a rasteriser that interpolates vertex colours and texture
   coordinates a little differently would give: a value next to a rounding threshold lands on the other side. The reading of
   these groups (a colour step, a colour step halved by a half-and-half blend, the blend's own rounding, and for the rest a
   neighbouring texel) fits the numbers but was not traced pixel by pixel to the primitives, and `vktest`, which would say
   exactly, is a Win32 program and has not run on lavapipe. Maximum Remix on the same system, built the same way, with the
   same script and `gpu_gl = 1` at 2x: on the D3D12 OpenGL driver 83 of 83 shots are identical to the Windows Radeon OpenGL
   baseline. On lavapipe 6 of 86 are identical, at most 0.14 % of a shot's pixels differ (0.05 % on average), and of the
   20910 differing pixels 48 % differ by 8, 45 % by 4, 6 % by 1 or 2 and 2 % by other amounts (largest 88): the same kind of
   difference as in Freestyle BMX; these pixels were not located. Three runs under the validation layer gave no message. A
   second plain lavapipe run and two of the validated runs give the first plain run's shots (74 of 74, 69 of 69, 69 of 69).
   The other validated run, the first one, left the script's path at frame 1500 and showed other screens from there on, so
   its shots compare with nothing; the cause was not found, and it did not happen again in the two runs after it. A fault
   found on the way, not a renderer one: after some minutes of `--turbo` the game did not end when told to (SIGTERM), under
   OpenGL and Vulkan alike; it stopped inside SDL's closing of the PulseAudio device, and with `SDL_AUDIODRIVER=dummy` it
   ended. It is a fault of `--turbo` runs as far as seen: with the old code a run of 200 seconds at normal speed ended as it
   should, and the turbo run beside it did not (one run each). `plat_exit` in `runtime/plat_sdl.c` no longer closes the sound
   device (it stops rumble, closes the pads, gives back the window and the display mode and leaves); three turbo runs of 170
   seconds then ended with code 0. The new exit path was not tried with a gamepad, in fullscreen, or on a machine other than
   this one. Not done: Vulkan on Linux on a graphics card (lavapipe is a CPU driver), NVIDIA and Intel on Windows, Linux on
   real hardware with Mesa's own drivers or NVIDIA's, and a Steam Deck.
7. **Make Vulkan the default presentation where it works** (not estimated). A new setting, `renderer = auto | vulkan | gl`,
   picks the presentation and GPU-renderer backend: `auto` uses Vulkan when the device supports what is needed and falls back
   otherwise; the other two force one. It does not change what `gpu_gl` means: the CPU renderer stays the default picture, and
   whether the GPU renderer becomes the default is a separate decision.
   *Exit:* `auto` picks Vulkan on the machines of phase 6 and falls back to OpenGL, then to the software stretch, on a machine
   without a working Vulkan loader; `vulkan` and `gl` force their backend and say so when it is unavailable.
   *Done so far (Windows, Radeon), ahead of phase 6:* `renderer = auto` is a third value (`host_init` in
   `runtime/host_common.c`). Like `vulkan` it tries Vulkan, then OpenGL, then the software stretch. The difference: with
   `gpu_gl = 1`, `auto` asks the Vulkan backend to say no when its GPU renderer cannot be set up (`gfx_vk_need_renderer`,
   `gvk_ready`), so OpenGL and its GPU renderer get their turn; `vulkan` keeps Vulkan and the CPU draws the picture. `gl`
   never tries Vulkan. Each choice and each step down is printed (`[video] renderer = auto: Vulkan cannot be used here;
   trying OpenGL`, `[video] renderer = auto: using OpenGL`). Checked with a test hook, `DMRECOMP_GFX_TEST` (read once at
   start, a list): `noloader` behaves like a machine without Vulkan, `norenderer` like a device without a depth-stencil
   format, `nogl` like a machine without OpenGL 3.3. Eleven runs (the three values, with and without each fault, `gpu_gl` 0
   and 1) each picked the backend described here, and an unknown value is refused. Scripted runs under `auto` at plain 2x
   give the OpenGL renderer's shots exactly (BMX 94 of 94, MR 108 of 108, the runs made with the code as committed). With
   `nogl` added, six more runs reached the software stretch from each of the three values and kept running. The launcher's
   Video settings have `renderer` and `gpu_gl` (launcher version 7). The SDL front end is compile-checked only (gcc under WSL
   with `-fsyntax-only -Wall`: `host_common.c`, `gfx_vk.c`, `gpu_vk.c`, `gfx_gl.c`, `gfx.c`, `plat_sdl.c` and `gpu.c`; no
   warnings). `vktest decline` checks that the Vulkan backend says no and leaves no device behind when its renderer cannot be
   set up and that it starts again without the fault; `vktest keep` that under `renderer = vulkan` it starts with the fault.
   Not done: the default is still `gl` (changing it waits for phase 6); the picture the software stretch shows after a step
   down was not compared; nothing was run on other hardware or on Linux.

Rough total: 5 to 8 weeks of focused work for phases 1 to 6 if the hardware is at hand, phase 3 being the optional one.

## Risks

- **Driver differences** show up late and on hardware that is not always at hand (phase 6).
- **The mask bit** (stencil as bit 15 of a vram pixel, rebuilt when check-mask turns on) and the two-pass translucency are the
  parts most likely to behave differently on another API. Neither game has used check-mask in the runs so far, so it is covered
  by the synthetic mask cases in `tools/gltest.c` (phase 1), which the Vulkan version must pass too (phase 5).
- **Two renderers to keep correct.** The CPU renderer is the reference for both, and the same pixel-comparison runs apply to
  each.
- **Portability of the build.** The Linux build is compiled with a downloaded portable compiler on the Steam Deck route; the
  Vulkan headers have to be committed or fetched the same way, and the loader opened at run time, so no system package is
  needed.

## How a contributor can help

- Run the GPU renderer on a graphics card that is not an AMD Radeon RX 7900 XTX and report what happens
  (`docs/gpu-renderer-testing.md`). That is phase 1 and needs no Vulkan knowledge.
- Review or extend the pixel-exact tests; they are what makes a new backend safe to write.
- For the Vulkan phases, open an issue first so the work can follow the backend interface of phase 2.
