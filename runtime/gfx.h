/* The graphics backend: how a finished frame gets into the window, and who draws the 2x / 4x picture (docs/renderer-roadmap.md,
   phase 2). There is one hardware backend today, OpenGL (gfx_gl.c); gfx.c has the software one for machines without it. A
   Vulkan backend would be a second GfxBackend with the same shape. The backend is chosen once, in host_init, for the window
   picture and the GPU renderer together. */
#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include "gpu_gl.h"

typedef struct {                      /* one frame to show */
    const uint32_t *fb;               /* the CPU picture, 0x00RRGGBB, fb_w x fb_h (its contents are not used when on_gpu) */
    int fb_w, fb_h;                   /* the picture's size: on the GPU path it is the enlarged size, like fb's */
    int on_gpu;                       /* the picture lives on the graphics card (the GPU renderer made it) ... */
    int box[4];                       /* ... in this console-pixel box: x, y, w, h */
    int cw, ch, w, h;                 /* the window's client size, and the size of the picture inside it */
    int filter, upscale, fsr_sharp;   /* smooth filter, FSR on or off, FSR sharpness 0..100 */
    int grab;                         /* also copy the window contents out (--shot-window): see GfxBackend.grab */
} GfxFrame;

typedef struct GfxBackend {
    const char *name;
    int hw;                           /* 1 for a backend that draws on the graphics card, 0 for the software stretch */
    /* the window and what draws into it; 0 when this backend cannot run. A failed hardware init leaves the plain window that
       plat_video_init made, and the software backend's init relies on that window: it creates nothing itself. */
    int  (*init)(int w, int h, const char *title);
    void (*present)(const GfxFrame *f);              /* draw and show one frame */
    const uint32_t *(*grab)(int *w, int *h);         /* the window picture of the last present that had grab set, or NULL; one-shot: the next call gives NULL */
    /* the GPU hi-res renderer (gpu_gl = 1); see gpu_gl.h */
    int  (*r_init)(int scale);
    void (*r_submit)(const GPrim *p);
    void (*r_mask)(int set, int check);
    void (*r_dirty)(int y0, int y1);
    void (*r_flush)(void);
    void (*r_read)(int x, int y, int w, int h, unsigned *dst);
} GfxBackend;

extern const GfxBackend gfx_gl_backend, gfx_soft_backend;
extern const GfxBackend *gfx;         /* the backend in use; the software one until host_init has chosen */
#endif
