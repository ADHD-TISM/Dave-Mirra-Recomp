# Testing the GPU renderer (`gpu_gl`) on other graphics cards

`gpu_gl = 1` draws the 2x / 4x picture on the graphics card instead of the CPU. It is an early prototype, off by default.
So far it has been tried on one machine only: Windows, an AMD Radeon RX 7900 XTX (OpenGL 4.6 compatibility profile),
both games, plus a scripted CPU-versus-GPU comparison. Not tried on NVIDIA or Intel graphics, or on Linux. This page is
for whoever tries it next.

## How it works (the short version)

- `runtime/gpu.c` still draws the console-exact picture into `vram` on the CPU. Textures, palettes and read-backs use it.
  It also describes every primitive to `runtime/gpu_gl.c` (`GPrim`, see `gpu_gl.h`).
- `gpu_gl.c` mirrors `vram` into a 1024x512 two-channel texture (rows uploaded when they change), decodes 4-bit, 8-bit and
  15-bit textures, palettes, modulation, dithering and the blend modes in a GLSL 1.20 fragment shader, and draws into a
  1024S x 512S colour texture on a framebuffer object (S = 2 or 4).
- Translucent textured primitives are drawn singly in two passes (opaque texels, then translucent ones).
- The console's mask bit (bit 15 of a vram pixel) is a stencil bit, kept only while the game checks it (`check_mask`; neither
  game does in the runs so far). It is rebuilt from `vram` when checking starts.
- Needs a compatibility-profile OpenGL 3.0 context: GLSL 1.20 with the fixed-function attributes (`gl_Vertex`, `glBegin`),
  framebuffer objects, and a packed depth-stencil attachment. Anything missing makes it fall back to the CPU picture and say
  so on stderr (`[gpu_gl] ...`).

## What to do

1. Get the game building (see the README: the launcher builds it from your own disc image), then build with CMake, or use
   the launcher. Run the game from a terminal so the `[gpu_gl]` messages are visible.
2. Run the driver check (Windows only: it uses wgl; on Linux skip to step 3). It opens a small window, draws random vram
   through the GL path, reads the picture back and compares it with the CPU's vram, including display boxes and copies that
   wrap or overlap, whether copies the console does not smear keep the picture's hi-res detail, and the mask bit (set-mask and
   check-mask on rectangles and copies). Every line should say `ok`. It does not exercise the picture copied into the window
   (`glg_present`), textured drawing or gameplay:
   ```
   mkdir -p build && gcc -I runtime tools/gltest.c runtime/gpu_gl.c -o build/gltest.exe -lopengl32 -lgdi32 && build/gltest.exe
   ```
   It prints the OpenGL renderer string first, so you can see which card the context landed on (on a laptop with two
   graphics chips, check that it is the NVIDIA one). The game and this test both ask the driver for the dedicated chip
   (the exported `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` variables, `runtime/host_common.c`); if the
   integrated chip still shows, the driver or a per-program graphics preference overrode the request.
3. Play with it: `build/dmrecomp.exe --set gpu_gl=1 --set scale=4 --set scale_auto=0 "<your image>.cue"` (F6 switches the
   scale, F7 the filter). Look for dark or missing frames, flickering, wrong textures, shadows and transparent effects,
   and the Run Finished screen.
4. A scripted comparison, if you want numbers: run the same script once with `--set gpu_gl=0` and once with `gpu_gl=1`,
   for example
   ```
   --turbo --auto-start 90 --shot-every 300 --set scale=2 --set scale_auto=0 --script-file lvl.txt "<your image>.cue"
   ```
   (`lvl.txt` is one line of comma-separated `frame:button` events such as `90:st,300:x`; the button names are in
   `host_set_script`, `runtime/host_common.c`), and compare the `shot_*.png` files
   pixel by pixel. On the Radeon the worst frame had 0.6 to 0.9 % of its pixels differing by more than 16 (of 255); most
   frames match exactly. A card that is far above that, or whose differences form patterns, is worth a look.

## Where problems are most likely

- **Shader compile errors.** NVIDIA's GLSL compiler is stricter than AMD's about GLSL 1.20: implicit int to float
  conversions, constructors, `mod` and `floor` on mixed types. The fragment shader is `fs_src` at the top of
  `runtime/gpu_gl.c`; the log is printed as `[gpu_gl] fragment shader failed: ...`.
- **The vram texture.** It is `GL_LUMINANCE8_ALPHA8`, read in the shader as `.ra`. A driver that treats luminance-alpha
  differently would show wrong textures everywhere.
- **The picture target.** A colour texture plus a `GL_DEPTH24_STENCIL8` texture attached as depth-stencil. If the
  framebuffer is reported incomplete the message says so and the CPU picture is used.
- **Rasterisation rules.** Where exactly a triangle edge lands on a pixel centre can differ between vendors, which could
  move an edge by one pixel at 2x. Triangle corners are moved half a pixel in (`+ 0.5` in `emit`) to match the CPU picture;
  rectangles, fills and blits are not shifted. Check thin lines and sprite edges.
- **Read-back and the window picture.** `glg_read` (screenshots) uses `GL_BGRA`; the window picture is copied into the
  host texture by `glg_present` (one `glCopyTexImage2D`, or, when the display box wraps around the edge of vram, a
  `glTexImage2D` plus one `glCopyTexSubImage2D` per piece), then drawn with the fixed pipeline or FSR.

## Reporting

For each card: the `GL_RENDERER` string, the `gltest` output, the `[gpu_gl]` lines from stderr, and for any difference a
pair of screenshots (`gpu_gl=0` and `gpu_gl=1` at the same frame) with the scale and the point in the game.
