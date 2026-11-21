# Renderer roadmap: adding Vulkan next to OpenGL

Status: plan. Phase 1 (finishing the OpenGL path) is in progress: its tests are written, the NVIDIA and Intel runs are not
done. Phase 2 (the backend seam) is done: the window, its surface, the window picture and the GPU renderer all sit behind
`runtime/gfx.h`. Phases 3 to 7 are not started. Estimates are rough and are not promises.

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
   contributor who changes a shader regenerates the SPIR-V with `glslc` and commits both. Different `glslc` versions produce
   different bytes from the same source, so nothing compares bytes: a hash of the GLSL source (with the files it includes and
   the `glslc` options used) is recorded next to each `.spv`. A small check in `tools/`, built like `tools/gltest.c` (the
   repo has no test runner), recomputes the hash and fails when the source changed without the SPIR-V being regenerated. It
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

- `runtime/plat_win32.c` and `runtime/plat_sdl.c` own the window and its surface: `plat_surface_open(kind, ...)` creates both for one kind of surface
  (`PLAT_SURF_GL`: a window with a compatibility context, `SDL_WINDOW_OPENGL` or a pixel format on Win32; `PLAT_SURF_SOFT`: a
  plain window) and `plat_surface_close` removes them. `plat_swap` shows the frame, and `plat_gl_proc` looks up entry points
  (`runtime/host.h`). `plat_soft_present` is the existing no-OpenGL path.
- `runtime/gfx_gl.c` (the OpenGL backend of `runtime/gfx.h`) draws the window picture with GL: upload of the CPU picture, the
  sharp and smooth filter, FSR (two GLSL 1.20 shaders), and the `--shot-window` grab. `runtime/gfx.c` is the software
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
   failed attempt leaves no window, so the next backend gets a fresh one (a Win32 window takes a pixel format only once). A
   Vulkan backend adds a kind (a window with `SDL_WINDOW_VULKAN` on SDL, a plain `HWND` on Win32) and nothing else changes. On
   Windows the same scripted runs gave identical screenshots and window grabs before and after, and a GL attempt forced to
   fail falls back to the software window with the same pictures; the SDL front end was compile-checked only.
   *Exit:* `gltest` and the scripted run give identical results before and after.
3. **Modernise GL as a stepping stone** (about a week, optional). Replace `glBegin` and the fixed pipeline with vertex buffers
   and core-profile shaders. This is most of the porting work for Vulkan anyway, it drops the compatibility-profile
   requirement, and it can be checked against the baseline before any new API is involved.
   *Exit:* same as phase 2, on a core-profile context.
4. **Vulkan device and presentation** (about a week). The loader, instance, device and swapchain on both front ends; upload of
   the CPU picture, filter, FSR and the window grab. The backend is chosen once per run for the presenter and the renderer
   together: until phase 5, `gpu_gl=1` keeps the OpenGL presenter, because `glg_present` hands its picture to a GL texture.
   *Exit:* every CPU-renderer run, including FSR, looks the same as through OpenGL, and a machine with no Vulkan loader still
   starts through the fallback chain.
5. **Vulkan GPU renderer** (2 to 3 weeks). The vram texture with dirty-row uploads, the offscreen target with stencil, the
   decode shader as SPIR-V, one pipeline per blend and stencil state, two-pass translucency, copies and the check-mask rebuild.
   *Exit:* a Vulkan version of `gltest` (including the check-mask cases from phase 1) passes, and the scripted comparison
   matches the Radeon OpenGL baseline on both games.
6. **Hardware validation** (a week or more). AMD, NVIDIA and Intel on Windows, and Linux with Mesa and NVIDIA drivers, a Steam
   Deck if one is available, with the validation layers on. The least predictable phase: it depends on having the hardware.
   *Exit:* no validation errors, no regressions against OpenGL on any of them.
7. **Make Vulkan the default presentation where it works** (not estimated). A new setting, `renderer = auto | vulkan | gl`,
   picks the presentation and GPU-renderer backend: `auto` uses Vulkan when the device supports what is needed and falls back
   otherwise; the other two force one. It does not change what `gpu_gl` means: the CPU renderer stays the default picture, and
   whether the GPU renderer becomes the default is a separate decision.
   *Exit:* `auto` picks Vulkan on the machines of phase 6 and falls back to OpenGL, then to the software stretch, on a machine
   without a working Vulkan loader; `vulkan` and `gl` force their backend and say so when it is unavailable.

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
