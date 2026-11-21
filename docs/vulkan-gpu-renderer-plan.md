# Plan: the GPU renderer on Vulkan (roadmap phase 5)

Status: plan, nothing written yet. It follows `docs/renderer-roadmap.md` (phase 5) and starts from what phases 2 to 4 left:
`runtime/gfx_vk.c` shows the CPU picture through Vulkan (blit, FSR) and its renderer calls (`r_*`) say no. This plan makes them
say yes, so `renderer = vulkan` with `gpu_gl = 1` draws the 2x and 4x picture on the card. The OpenGL renderer
(`runtime/gpu_gl.c`) is the reference for behaviour: the same `GPrim` queue, the same pixel results.

## What has to exist (taken from `gpu_gl.c`)

| OpenGL today | Vulkan |
|---|---|
| vram mirror, `GL_RG8` 1024x512, dirty rows uploaded with `glTexSubImage2D` | `R8G8_UNORM` image, dirty rows memcpy'd into a staging buffer and copied with `vkCmdCopyBufferToImage` (one region per dirty span) |
| picture target: RGBA8 colour texture + packed depth-stencil, 1024S x 512S | `B8G8R8A8_UNORM` image (the format of the presenter's `pic`, so the copy into `pic` needs no conversion and `r_read` is a copy of rows (the alpha byte is masked off, as `gltest` does); usage colour attachment, transfer source and destination) + a depth-stencil image; the format is chosen at start from what the device offers as a depth-stencil attachment: `D24_UNORM_S8_UINT` if it does, else `D32_SFLOAT_S8_UINT` |
| one shader pair, `pass` and `alpha` uniforms | the same fragment shader ported to GLSL 450 (SPIR-V, `runtime/shaders/gpu_decode.frag` and `gpu_draw.vert`), `pass` and `alpha` (fragment stage) and `target` (vertex stage) as push constants |
| `gls_draw(GL_TRIANGLES, ...)` of `GlsVert` | a host-visible vertex buffer filled per flush, same `GlsVert` layout, `vkCmdDraw` |
| blend state (5 modes), stencil state, colour mask | pipelines, made when first needed and cached: blend {off, average, add, subtract, quarter-add} x stencil {off, fill/blit write, check + replace, check + keep} x colour mask {all, none}, at most 40, plus 3 stencil variants of the plain textured program (see the next row); scissor, stencil reference and compare mask are dynamic |
| `copy_box`: `glCopyTexSubImage2D` into a temp texture, then a textured quad | end the render pass, `vkCmdCopyImage` target to a temp image, a second pass draws it back with the plain textured program, which `copy_box` runs under the stencil state of the copy (`set_stencil`), so it needs those 3 stencil variants (off, check + replace, check + keep) and a descriptor set for the temp image |
| `glg_present` copies the visible box into the host's texture | the presenter already owns `pic`; for a GPU picture it copies the box (one to four `vkCmdCopyImage` pieces when it wraps) from the target into `pic`, then the blit or FSR path runs as today |
| `glg_read` (screenshots) `glReadPixels` | `vkCmdCopyImageToBuffer`, wait, convert to `0x00RRGGBB` |

## Synchronisation

One graphics queue. Two frame slots, each with a command buffer, a fence, a vertex buffer (32 MB) and a staging buffer (1 MB: all of vram as 16-bit). `glg_flush` records
the queued primitives into the slot's command buffer and submits at once; it waits only for the fence of the slot it is about to
reuse. Unlike OpenGL, which frees the vertices as it draws, Vulkan keeps every vertex of a command buffer until it has run, so the
buffer size is a bound, not a guess. Read off `gpu_gl.c`: one flush queues up to 8192 primitives; a blit or fill is up to 4
pieces of 6 vertices and a blit under check-mask is drawn twice (the pixels, then again stencil-only), a translucent textured primitive is
drawn two or three times, and a copy is cut into up to 9 boxes, each a quad. At 68 bytes a vertex that is about 27 MB, about 30 MB
with the copies. That is arithmetic, not a measurement, so there is also a rule: when a slot's buffer is full, `glg_flush`
submits the command buffer so far and goes on in the other slot (waiting for its fence) instead of relying on the size.
Reads (`r_read`) and the end of a frame wait for the slot's fence. Layout changes are explicit barriers: the target stays
in `COLOR_ATTACHMENT_OPTIMAL` (and the depth-stencil in its attachment layout) inside the renderer, and moves to
`TRANSFER_SRC_OPTIMAL` only for a copy or read.

## Steps, each ending with a test

1. **Share the device.** Move the device, queue and helper functions of `gfx_vk.c` into `runtime/vk_ctx.h` so a second file
   (`runtime/gpu_vk.c`) can hold the renderer. No behaviour change. *Test:* the Vulkan window grabs of phase 4 stay identical.
   *Done:* `runtime/vk_ctx.h` holds the function table (with the calls the renderer will need added), the `VkCtx vkc` struct
   (instance, physical device, device, queue, family, memory properties, command pool) and the shared helpers
   (`vk_find_mem`, `vk_make_buffer`, `vk_free_buffer`, `vk_make_module`, `vk_image_barrier`); `gfx_vk.c` fills and uses them.
   `DMRECOMP_VK_VALIDATE=1` asks for `VK_LAYER_KHRONOS_validation` and a debug messenger that prints warnings and errors to
   stderr; with the layer missing it says so and runs on without it. Radeon, same runs as phase 4: smooth 63 of 63 identical
   grabs, whole-multiple sharp 64 of 64, FSR 57 of 63 and 46 of 64 (the rest within 1 of 255), tie columns unchanged. The
   validation layer itself has not run (it is not installed here).
2. **Target, vram image, uploads, reads.** `r_init`, `r_dirty`, `r_read`, and the presenter's copy from the target. No drawing
   yet: a fill of the whole target from vram (`GK_BLIT`) is the only draw. *Test:* a Vulkan version of `tools/gltest.c`
   (`tools/vktest.c`, its own small window and the same checks): the display-box and the plain upload cases.
   *Done:* `runtime/gpu_vk.c` (interface in `gpu_vk.h`): the `B8G8R8A8` target with a depth-stencil image beside it (the format is
   picked from what the device offers; on the Radeon it is `D32_SFLOAT_S8_UINT`, as the message at start says), the
   `R8G8` vram image with dirty rows copied in through a staging buffer, two frame slots, a render pass and one pipeline, the
   draw shaders ported to GLSL 450 (`runtime/shaders/gpu_draw.vert`, `gpu_decode.frag`), blits, `r_read`, and the presenter's copy of
   the visible box (with wraps) from the target into `pic`. The vertex code both renderers use moved to `runtime/gpu_emit.h`
   (`gpu_gl.c` uses it too: `gltest` still passes 31 of 31 and the OpenGL grabs are identical to before). The SPIR-V moved to
   `runtime/gfx_vk_spv.c` and `.h`, so it is compiled once. The renderer says no unless `DMRECOMP_VK_RENDERER` is set, because only
   blits are drawn so far. `tools/vktest.c` makes 16 checks (plain and wrapping display boxes, whole vram, dirty rows, two spans,
   first and last row, a blit that wraps both edges, the window picture with and without a wrap, and the 4x picture); all pass on
   the Radeon, and three deliberate breaks (the upload row, the presenter's piece offset, the read-back offset) each make it fail.
   Not tried: the validation layer (not installed here), other hardware.
3. **Flat primitives and blending.** Pipelines for rectangles, fills, pixels and flat triangles, all five blend modes, scissor.
   *Test:* new `vktest` cases (every rectangle case in `gltest` uses the mask bit, so they belong to step 5): a flat rectangle,
   fill, pixel and triangle at each of the five blend modes against a CPU model of the blend (the arithmetic of `gpu.c`), a
   scissored draw, and a triangle finer than vram's pixels (the hi-res detail).
   *Done:* a flush groups the queued primitives into runs of the same state (`same_state`, as in `gpu_gl.c`), and each run is a draw
   with the blend pipeline it needs (average, add, subtract, quarter-add, none; made when first used and cached) and its drawing
   area as the scissor; translucent textured primitives get the two draws (opaque texels, then translucent ones). When the
   vertices of a flush would not fit in a slot's buffer it submits what it has and goes on in the other slot (the flush-on-full
   rule), which `vktest small` forces with a 64 KB buffer (`DMRECOMP_VK_VBUF_KB`, a test hook). `vktest` now makes 42 checks, all
   of which pass on the Radeon in the normal, the `small` and the `gate` runs: flat rectangle, triangles (two that make a
   rectangle, so a pixel drawn twice along the diagonal would show) and pixel at all five blend modes against a CPU model of the
   blend, a fill (the blend is ignored), drawing areas (clipped, and empty), 200 and 1500 primitives in one flush against a
   painter's-algorithm model, a triangle finer than vram's pixels, the presenter's copy with FSR (it must equal the same picture
   sent from the CPU), the 4x picture and going back to 2x. Four deliberate breaks (the subtract operation, the scissor, a run
   dropped at a batch boundary, the average's blend factor) each make it fail. The step also fixes what the step-2 review found: a failed
   target no longer disables the renderer for the session, a failed set-up frees what it had made, a failed submit keeps the
   dirty rows and replaces the fence, `runs()` is shared (`gv_runs`).
   Review fixes after step 3 (all in `gpu_vk.c` unless noted): an out-of-memory failure drops only the run it happened in; nothing
   draws, presents or reads while there is no target; the fence made again after a failed submit is checked (a device that does not
   recover turns the renderer off); every blend pipeline is made at set-up, so a draw never meets a missing one; `GV_MAX_VERTS`
   names the bound of 24 (`gpu_emit.h`); `vktest` clears the two environment variables it depends on and has a new check, rows to
   upload plus 400 rectangles plus a blit in one flush (43 checks; with the upload skipped in a batch that has draws it fails 37).
   *Step 4 needed no new code:* the decode shader was ported whole in step 2 and the run grouping and two-pass translucency came
   with step 3, so textured drawing was tested by the measured comparison below.
4. **Textured decode.** Port `fs_src` (4, 8 and 15-bit textures, palettes, modulation, dithering, the texture window) and the
   two-pass translucency. *Test:* the scripted run against the CPU renderer on both games, and the grab comparison against the
   OpenGL GPU renderer (same scale, same script): the worst frame is held to the OpenGL baseline: against the CPU renderer 0.6 to 0.9 % of the pixels differ by more than 16 of
   255 in the worst frame (Freestyle BMX at 2x, the 261-frame `test-kit/lvl.txt` run on the Radeon: worst 0.86 %, average 0.23 %,
   see `docs/gpu-renderer-testing.md`), and the Vulkan renderer must stay within that, and close to the OpenGL one.
   *Done (Windows, Radeon, scale 2, the 261-frame script, a shot every 300 frames, the CPU renderer, the OpenGL GPU renderer and the
   Vulkan GPU renderer run side by side):* Freestyle BMX, 71 shots in common with the CPU run: the OpenGL GPU renderer is worst
   0.86 % and on average 0.24 % of pixels differing by more than 16 of 255, and the Vulkan one gives exactly the same numbers; the
   Vulkan and OpenGL GPU shots are identical in all 83 that both runs made. Maximum Remix (built from a copy of the repo with its
   own generated code, the same script): 84 shots in common with the CPU run, worst 0.62 %, average 0.25 % for both GPU renderers;
   Vulkan against OpenGL 89 of 89 identical. So the step's exit test holds at the shots taken. What it does not show: only one
   frame in 300 is compared, copies are not drawn yet by the Vulkan renderer (the runs print that they were asked for), and the
   mask bit is not drawn (neither game checks it in these runs).
5. **Copies and the mask bit.** `copy_box` (with the smear rule that turns overlapping copies into blits), the stencil as bit 15
   and its rebuild when check-mask turns on. *Test:* all 31 `gltest` checks in `vktest`.
6. **FSR and the rest of the path.** FSR on a GPU picture, 2x and 4x, `gpu_gl = 1` under `renderer = vulkan`; playtest of both
   games; frame time against the OpenGL renderer. *Exit of the phase:* `vktest` passes and the scripted comparison matches the
   Radeon OpenGL baseline on both games (the roadmap's exit test).

Hardware validation (NVIDIA, Intel, Mesa, the Steam Deck, validation layers) is phase 6; the validation layers are not on this
machine, so step 1 also added an opt-in switch (`DMRECOMP_VK_VALIDATE=1`, read once at start) that asks for
`VK_LAYER_KHRONOS_validation` when the layer exists, so whoever has it can run every step under it.

## Decisions and risks

- **Depth-stencil format.** The stencil (bit 15) is only ever drawn into and tested, never copied or read, so the format needs
  just the `DEPTH_STENCIL_ATTACHMENT` feature. It is cleared by a render pass whose stencil load operation is CLEAR, not by
  `vkCmdClearDepthStencilImage`, which would need transfer support too. Which of the two formats a driver offers is from memory
  (AMD is said to offer only `D32_SFLOAT_S8_UINT`, NVIDIA and Intel both) and is not assumed: step 2 checks it on the machine at
  hand with `vkGetPhysicalDeviceFormatProperties` and fails the renderer (CPU picture, with a message) if neither is usable.
- **Rounding and orientation.** The OpenGL renderer's results are the reference. Both APIs number the target's rows from the
  image's row 0 (clip y = -1 is row 0 in each), so vram is stored the same way and `gl_FragCoord.y`, which the dither pattern
  uses, counts the same; but Vulkan's clip y points down where OpenGL's points up, so the winding of every triangle reverses
  (culling is off, so that is harmless) and a flip by a negative viewport height would need `VK_KHR_maintenance1` on a Vulkan
  1.0 device, or adjusted offsets. The `+ 0.5` shift of triangle corners in `emit` was found for OpenGL's rasterisation and is
  carried over, not assumed: Vulkan states its top-left rule in framebuffer coordinates, and interpolation of the attributes
  the shader decodes (`t0` to `t2`) must come out the same. Expect to chase single-pixel edge differences in step 4.
- **Performance.** `glg_flush` can be called hundreds of times a frame (every translucent textured primitive is drawn singly in two
  passes). One submit per flush is cheap on a desktop driver but unproven on a Steam Deck. If it shows up, batch flushes into
  one command buffer per frame and submit at present.
- **Size.** About 1,000 lines of C and 250 of GLSL. The OpenGL renderer took the same shape in `gpu_gl.c` (536 lines) plus the
  shared pieces; Vulkan's explicitness roughly doubles it. The new shaders go into `runtime/shaders`, and `gfx_vk_spv.c` is
  regenerated by `tools/shaders.py build`.
- **Other hardware.** The step-by-step tests run on the Radeon only until phase 6.
