# Renderer roadmap: adding Vulkan next to OpenGL

Status: plan, nothing of phases 2 to 7 is started. Phase 1 (finishing the OpenGL path) is in progress. Estimates are rough
and are not promises.

## Goal

Draw the picture with Vulkan on Windows and Linux (including the Steam Deck), and keep OpenGL as the fallback for machines
where Vulkan is missing or misbehaves. The CPU renderer stays the console-exact reference and the default.

OpenGL is formally deprecated only on macOS; Windows and Linux drivers will carry it for years. This is not an emergency.
The reasons to do it anyway are driver futures, Vulkan-only features, and having one modern path to test and optimise.

## Decisions

1. **API: Vulkan directly**, behind a small backend interface of our own. No engine, no SDL3 GPU layer: the project has almost
   no dependencies and should keep it that way.
2. **Platforms: Windows and Linux only** (the Linux build is also the Steam Deck build). macOS, and with it MoltenVK, is out of
   scope for now.
3. **Shaders: compiled SPIR-V is checked into the repository next to the GLSL source**, so users need no shader compiler and
   the launcher's downloads stay as they are. A contributor who changes a shader regenerates the SPIR-V with `glslc` and commits
   both; a check in the tests compares them.
4. **OpenGL stays as the fallback**, not only for a transition period. It is used when Vulkan is unavailable, fails to start,
   or is switched off in the settings. It is removed only if it becomes a burden and Vulkan has proved itself on real machines.

## Where OpenGL is today

- `plat_win32.c` and `plat_sdl.c` create the GL context through three functions: `plat_video_init`, `plat_swap` and
  `plat_gl_proc` (`host.h`).
- `host_common.c` draws the window picture with GL: upload of the CPU picture, the sharp and smooth filter, FSR (two GLSL 1.20
  shaders), screenshots and the window grab. About 40 GL calls.
- `gpu_gl.c` (the experimental GPU hi-res renderer, `gpu_gl=1`) draws the 2x and 4x picture. Its whole interface is the seven
  functions in `gpu_gl.h`; `gpu.c` never touches GL. It also holds the pixel-exact logic: the vram mirror texture, the PSX
  texture-decode shader, the mask bit kept as a stencil bit only while check-mask is on, and two-pass translucency. See
  `docs/gpu-renderer-testing.md`.

A replacement has to cover both the GPU renderer and the window presentation.

## Phases

Each phase ends with its exit test. Nothing moves on until the test passes on the Radeon baseline and the OpenGL path is
unchanged.

1. **Finish the OpenGL path** (in progress; about a week). Test it on NVIDIA and Intel graphics, fix what that finds (most
   likely GLSL 1.20 strictness), and close the small review items that are still open (a test that up/left scroll copies keep
   their full detail, a few comments). The Vulkan port reuses the shader logic and the pixel-match tests, so they have to be right first.
   *Exit:* `tools/gltest.c` and the scripted CPU-versus-GPU comparison pass on AMD, NVIDIA and Intel; both games play.
2. **Make the seam explicit** (3 to 5 days). One backend interface in front of everything GL: init, present, read-back and the
   seven renderer calls; today's GL code moves behind it with no behaviour change.
   *Exit:* `gltest` and the scripted run give identical results before and after.
3. **Modernise GL as a stepping stone** (about a week, optional). Replace `glBegin` and the fixed pipeline with vertex buffers
   and core-profile shaders. This is most of the porting work for Vulkan anyway, it drops the compatibility-profile
   requirement, and it can be checked against the baseline before any new API is involved.
   *Exit:* same as phase 2, on a core-profile context.
4. **Vulkan device and presentation** (about a week). Instance, device and swapchain on both front ends (Win32 and SDL);
   upload of the CPU picture, filter, FSR, screenshots and the window grab.
   *Exit:* every CPU-renderer run, including FSR, looks the same as through OpenGL.
5. **Vulkan GPU renderer** (2 to 3 weeks). The vram texture with dirty-row uploads, the offscreen target with stencil, the
   decode shader as SPIR-V, one pipeline per blend and stencil state, two-pass translucency, copies and the check-mask rebuild.
   *Exit:* a Vulkan version of `gltest` passes, and the scripted comparison matches the Radeon OpenGL baseline on both games.
6. **Hardware validation** (a week or more). AMD, NVIDIA, Intel and a Steam Deck, with the validation layers on. The least
   predictable phase: it depends on having the hardware.
   *Exit:* no validation errors, no regressions against OpenGL on any of them.
7. **Make Vulkan the default where it works.** The picture setting chooses Vulkan when the device supports what is needed,
   falls back to OpenGL otherwise, and can be forced either way.

Rough total: 5 to 8 weeks of focused work, phase 3 being the optional one.

## Risks

- **Driver differences** show up late and on hardware that is not always at hand (phase 6).
- **The mask bit** (stencil as bit 15 of a vram pixel, rebuilt when check-mask turns on) and the two-pass translucency are the
  parts most likely to behave differently on another API. Neither game uses check-mask, so it needs synthetic tests such as
  those in `tools/gltest.c`.
- **Two renderers to keep correct.** The CPU renderer is the reference for both, and the same pixel-comparison runs apply to
  each.
- **Portability of the build.** The Linux build is compiled with a downloaded portable compiler on the Steam Deck; the Vulkan
  headers and loader have to fit that route without a system package.

## How a contributor can help

- Run the GPU renderer on a graphics card that is not an AMD Radeon RX 7900 XTX and report what happens
  (`docs/gpu-renderer-testing.md`). That is phase 1 and needs no Vulkan knowledge.
- Review or extend the pixel-exact tests; they are what makes a new backend safe to write.
- For the Vulkan phases, open an issue first so the work can follow the backend interface of phase 2.
