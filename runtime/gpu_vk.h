/* The GPU hi-res renderer on Vulkan (docs/vulkan-gpu-renderer-plan.md): the same interface as gpu_gl.h, drawn into a Vulkan image on
   the device gfx_vk.c made. The presenter in gfx_vk.c copies the visible box out of it with gvk_present_copy. It does everything
   gpu_gl.c does (steps 2 to 5 of the plan): uploads, reads, blits, rectangles, fills, pixels and triangles with the console's
   blend modes and drawing area, textured drawing, copies and the mask bit (the stencil). The game uses it when
   renderer = vulkan and gpu_gl = 1 (step 6 of the plan: both games were playtested on it). */
#ifndef GPU_VK_H
#define GPU_VK_H
#include "vk_ctx.h"
#include "gpu_gl.h"

int  gvk_init(int scale);              /* 1 when the renderer is ready at this scale (the Vulkan backend must be running) */
void gvk_submit(const GPrim *p);
void gvk_mask(int set, int check);
void gvk_dirty(int y0, int y1);
void gvk_flush(void);
void gvk_read(int x, int y, int w, int h, unsigned *dst);    /* the box of the picture as 0xAARRGGBB, top row first (alpha is the shader's) */
/* record into c the copy of a console-pixel box of the picture into dst, an image of w*S x h*S in TRANSFER_DST_OPTIMAL; the caller has
   called gvk_flush first. The picture stays in its attachment layout afterwards. */
void gvk_present_copy(VkCommandBuffer c, VkImage dst, int x, int y, int w, int h);
void gvk_shutdown(void);               /* before the device is destroyed */
#endif
