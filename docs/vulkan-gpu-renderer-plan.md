# Plan: the GPU renderer on Vulkan (roadmap phase 5)

Status: plan, nothing written yet. It follows `docs/renderer-roadmap.md` (phase 5) and starts from what phases 2 to 4 left:
`runtime/gfx_vk.c` shows the CPU picture through Vulkan (blit, FSR) and its renderer calls (`r_*`) say no. This plan makes them
say yes, so `renderer = vulkan` with `gpu_gl = 1` draws the 2x and 4x picture on the card. The OpenGL renderer
(`runtime/gpu_gl.c`) is the reference for behaviour: the same `GPrim` queue, the same pixel results.

## What has to exist (taken from `gpu_gl.c`)

| OpenGL today | Vulkan |
|---|---|
| vram mirror, `GL_RG8` 1024x512, dirty rows uploaded with `glTexSubImage2D` | `R8G8_UNORM` image, dirty rows memcpy'd into a staging buffer and copied with `vkCmdCopyBufferToImage` (one region per dirty span) |
| picture target: RGBA8 colour texture + packed depth-stencil, 1024S x 512S | `R8G8B8A8_UNORM` image + a depth-stencil image; the format is chosen at start: `D24_UNORM_S8_UINT` if offered, else `D32_SFLOAT_S8_UINT` (AMD offers only the second) |
| one shader pair, `pass` and `alpha` uniforms | the same fragment shader ported to GLSL 450 (SPIR-V, `runtime/shaders/gpu_decode.frag` and `gpu_draw.vert`), `pass` and `alpha` as push constants |
| `glBegin` triangles of `GlsVert` | a host-visible vertex buffer filled per flush, same `GlsVert` layout, `vkCmdDraw` |
| blend state (5 modes), stencil state, colour mask | pipelines, made when first needed and cached: blend {off, average, add, subtract, quarter-add} x stencil {off, fill/blit write, check + replace, check + keep} x colour mask {all, none}, at most 40; scissor, stencil reference and compare mask are dynamic |
| `copy_box`: `glCopyTexSubImage2D` into a temp texture, then a textured quad | end the render pass, `vkCmdCopyImage` target to a temp image, a second pass draws it back with the plain pipeline |
| `glg_present` copies the visible box into the host's texture | the presenter already owns `pic`; for a GPU picture it copies the box (one to four `vkCmdCopyImage` pieces when it wraps) from the target into `pic`, then the blit or FSR path runs as today |
| `glg_read` (screenshots) `glReadPixels` | `vkCmdCopyImageToBuffer`, wait, convert to `0x00RRGGBB` |

## Synchronisation

One graphics queue. Two frame slots, each with a command buffer, a fence, a vertex buffer (16 MB: 8192 queued primitives of at
most 24 vertices of 68 bytes is the worst case, 13 MB) and a staging buffer (1 MB: all of vram as 16-bit). `glg_flush` records
the queued primitives into the slot's command buffer and submits at once; it waits only for the fence of the slot it is about to
reuse. Reads (`r_read`) and the end of a frame wait for the slot's fence. Layout changes are explicit barriers: the target stays
in `COLOR_ATTACHMENT_OPTIMAL` (and the depth-stencil in its attachment layout) inside the renderer, and moves to
`TRANSFER_SRC_OPTIMAL` only for a copy or read.

## Steps, each ending with a test

1. **Share the device.** Move the device, queue and helper functions of `gfx_vk.c` into `runtime/vk_ctx.h` so a second file
   (`runtime/gpu_vk.c`) can hold the renderer. No behaviour change. *Test:* the Vulkan window grabs of phase 4 stay identical.
2. **Target, vram image, uploads, reads.** `r_init`, `r_dirty`, `r_read`, and the presenter's copy from the target. No drawing
   yet: a fill of the whole target from vram (`GK_BLIT`) is the only draw. *Test:* a Vulkan version of `tools/gltest.c`
   (`tools/vktest.c`, its own small window and the same checks): the display-box and the plain upload cases.
3. **Flat primitives and blending.** Pipelines for rectangles, fills, pixels and flat triangles, all five blend modes, scissor.
   *Test:* the rest of `gltest`'s rectangle cases.
4. **Textured decode.** Port `fs_src` (4, 8 and 15-bit textures, palettes, modulation, dithering, the texture window) and the
   two-pass translucency. *Test:* the scripted run against the CPU renderer on both games, and the grab comparison against the
   OpenGL GPU renderer (same scale, same script): the worst frame is held to the OpenGL baseline (0.86 % of pixels differing).
5. **Copies and the mask bit.** `copy_box` (with the smear rule that turns overlapping copies into blits), the stencil as bit 15
   and its rebuild when check-mask turns on. *Test:* all 31 `gltest` checks in `vktest`.
6. **FSR and the rest of the path.** FSR on a GPU picture, 2x and 4x, `gpu_gl = 1` under `renderer = vulkan`; playtest of both
   games; frame time against the OpenGL renderer. *Exit of the phase:* `vktest` passes and the scripted comparison matches the
   Radeon OpenGL baseline on both games (the roadmap's exit test).

Hardware validation (NVIDIA, Intel, Mesa, the Steam Deck, validation layers) is phase 6; the validation layers are not on this
machine, so step 1 should also add an opt-in switch (an environment variable read once at start) that asks for
`VK_LAYER_KHRONOS_validation` when the layer exists, so whoever has it can run every step under it.

## Decisions and risks

- **Depth-stencil format.** Stencil bit 15 needs a format that is both attachment and transfer-source capable; the order above
  is from memory, not measured: AMD is said to offer only `D32_SFLOAT_S8_UINT`, NVIDIA and Intel both formats. Step 2 checks
  this on the machine at hand with `vkGetPhysicalDeviceFormatProperties` and fails the renderer (CPU picture, with a message) if neither is usable.
- **Rounding.** The OpenGL renderer's results are the reference. Vulkan's rasterisation rules are the same top-left rule, but
  interpolation of the attributes the shader decodes (`t0` to `t2`) must come out the same: flat shading and `floor` on texel
  coordinates are the sensitive spots, and the `+ 0.5` shift of triangle corners in `emit` is carried over unchanged. Expect to
  chase single-pixel edge differences in step 4.
- **Performance.** `glg_flush` can be called hundreds of times a frame (every translucent textured primitive is drawn singly in two
  passes). One submit per flush is cheap on a desktop driver but unproven on a Steam Deck. If it shows up, batch flushes into
  one command buffer per frame and submit at present.
- **Size.** About 1,000 lines of C and 250 of GLSL. The OpenGL renderer took the same shape in `gpu_gl.c` (560 lines) plus the
  shared pieces; Vulkan's explicitness roughly doubles it.
- **Other hardware.** The step-by-step tests run on the Radeon only until phase 6.
