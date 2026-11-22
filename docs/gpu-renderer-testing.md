# Testing the GPU renderer (`gpu_gl`) on other graphics cards

`gpu_gl = 1` draws the 2x / 4x picture on the graphics card instead of the CPU. It is an early prototype, off by default.
So far it has been tried on one machine only: Windows, an AMD Radeon RX 7900 XTX (OpenGL 4.6; the game now asks for a 3.3 core context),
both games, plus a scripted CPU-versus-GPU comparison. Not tried on NVIDIA or Intel graphics, or on Linux. This page is
for whoever tries it next.

## How it works (the short version)

- `runtime/gpu.c` still draws the console-exact picture into `vram` on the CPU. Textures, palettes and read-backs use it.
  It also describes every primitive to `runtime/gpu_gl.c` (`GPrim`, see `gpu_gl.h`).
- `gpu_gl.c` mirrors `vram` into a 1024x512 two-channel texture (rows uploaded when they change), decodes 4-bit, 8-bit and
  15-bit textures, palettes, modulation, dithering and the blend modes in a GLSL 3.30 fragment shader, and draws into a
  1024S x 512S colour texture on a framebuffer object (S = 2 or 4).
- Translucent textured primitives are drawn singly in two passes (opaque texels, then translucent ones).
- The console's mask bit (bit 15 of a vram pixel) is a stencil bit, kept only while the game checks it (`check_mask`; neither
  game does in the runs so far). It is rebuilt from `vram` when checking starts.
- Needs an OpenGL 3.3 context (the game asks for the core profile; nothing uses the fixed pipeline): GLSL 3.30, vertex
  buffers (`runtime/gl_stream.c`), framebuffer objects, and a packed depth-stencil attachment. Without a 3.3 context the
  whole OpenGL path is skipped, the window picture included, and the platform's software stretch is used (stderr says
  `[video] ...`). Other missing pieces make `gpu_gl` fall back to the CPU picture (`[gpu_gl] ...`).

## What to do

1. Get the game building (see the README: the launcher builds it from your own disc image), then build with CMake, or use
   the launcher. Run the game from a terminal so the `[gpu_gl]` messages are visible.
2. Run the driver check (Windows only: it uses wgl; on Linux skip to step 3). It opens a small window, draws random vram
   through the GL path, reads the picture back and compares it with the CPU's vram, including display boxes and copies that
   wrap or overlap, whether copies the console does not smear keep the picture's hi-res detail, and the mask bit (set-mask and
   check-mask on rectangles and copies). Every line should say `ok`. It does not exercise the picture copied into the window
   (`glg_present`), textured drawing or gameplay:
   ```
   mkdir -p build && gcc -I runtime tools/gltest.c runtime/gpu_gl.c runtime/gl_stream.c -o build/gltest.exe -lopengl32 -lgdi32 && build/gltest.exe
   ```
   It prints the OpenGL renderer string and version first (a 3.3 core context, as in the game), so you can see which card the context landed on (on a laptop with two
   graphics chips, check that it is the NVIDIA one). The game and this test both ask the driver for the dedicated chip
   (the exported `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` variables, `runtime/host_common.c`); if the
   integrated chip still shows, the driver or a per-program graphics preference overrode the request.
3. Play with it:
   ```
   build/dmrecomp.exe --set renderer=gl --set gpu_gl=1 --set scale=4 --set scale_auto=0 "<your image>.cue"
   ```
   (F6 switches the scale, F7 the filter). `--set renderer=gl` is needed: this guide is about the OpenGL renderer, and the
   default, `renderer = auto`, would use Vulkan and its own renderer where it can. The game says which one it uses; look
   for `[video] renderer = gl: using OpenGL` and a `[gpu_gl]` line on stderr. Look for dark or missing frames, flickering,
   wrong textures, shadows and transparent effects, and the Run Finished screen.
4. A scripted comparison, if you want numbers: run the same script once with `--set gpu_gl=0` and once with `gpu_gl=1`,
   both with `--set renderer=gl`, for example
   ```
   --turbo --auto-start 90 --shot-every 300 --set renderer=gl --set scale=2 --set scale_auto=0 --script-file lvl.txt "<your image>.cue"
   ```
   Set the environment variable `DMRECOMP_GFX_TEST=nopad` for both runs, or unplug your gamepad: the game reads a gamepad
   whether or not its window has the focus, and a touched pad sends the two runs down different paths.
   (`lvl.txt` is one line of comma-separated `frame:button` events such as `90:st,300:x`; the button names are in
   `host_set_script`, `runtime/host_common.c`), and compare the `shot_*.png` files
   pixel by pixel. On the Radeon the worst frame had 0.6 to 0.9 % of its pixels differing by more than 16 of 255 (measured locally on
   Freestyle BMX at scale 2 with a 261-frame button script: worst frame 0.86 %, average 0.23 %; the script is not in the repo); most
   frames match exactly. A card that is far above that, or whose differences form patterns, is worth a look.

## Where problems are most likely

- **Shader compile errors.** NVIDIA's GLSL compiler is stricter than AMD's about implicit int to float
  conversions, constructors, `mod` and `floor` on mixed types. The fragment shader is `fs_src` at the top of
  `runtime/gpu_gl.c`; the log is printed as `[video] picture shader failed: ...` (or `... shaders did not link: ...`).
- **The vram texture.** It is `GL_RG8` (low byte, high byte), read in the shader as `.rg`. Wrong textures everywhere would
  point here.
- **The picture target.** A colour texture plus a `GL_DEPTH24_STENCIL8` texture attached as depth-stencil. If the
  framebuffer is reported incomplete the message says so and the CPU picture is used.
- **Rasterisation rules.** Where exactly a triangle edge lands on a pixel centre can differ between vendors, which could
  move an edge by one pixel at 2x. Triangle corners are moved half a pixel in (`+ 0.5` in `emit`) to match the CPU picture;
  rectangles, fills and blits are not shifted. Check thin lines and sprite edges.
- **Read-back and the window picture.** `glg_read` (screenshots) uses `GL_BGRA`; the window picture is copied into the
  host texture by `glg_present` (one `glCopyTexImage2D`, or, when the display box wraps around the edge of vram, a
  `glTexImage2D` plus one `glCopyTexSubImage2D` per piece), then drawn as a textured quad or with FSR.

## Reporting

For each card: the `GL_RENDERER` string, the `gltest` output, the `[gpu_gl]` lines from stderr, and for any difference a
pair of screenshots (`gpu_gl=0` and `gpu_gl=1` at the same frame) with the scale and the point in the game.
