/* The 2x / 4x picture drawn by the graphics card (gpu_gl = 1; early prototype).
   gpu.c still draws the console-exact picture into vram on the CPU, because textures,
   palettes and read-backs use it. It also describes every primitive here; gpu_gl.c
   draws them at the higher resolution into an OpenGL texture, reading textures and
   palettes from a copy of vram. */
#ifndef GPU_GL_H
#define GPU_GL_H

enum { GK_TRI, GK_RECT, GK_PIX, GK_FILL, GK_BLIT, GK_COPY };

typedef struct {
    int kind;
    int vx[3], vy[3], vr[3], vg[3], vb[3], vu[3], vv[3];   /* GK_TRI: corners, colours, texture coordinates */
    int vfx[3];                                             /* GK_TRI: quarters of a pixel to add to vx (widescreen, flat polygons); else 0 */
    int x, y, w, h;                    /* the box; GK_PIX: one pixel; GK_COPY: the destination */
    int sx, sy;                        /* GK_COPY: the source corner */
    int u0, v0, flipx, flipy, r, g, b; /* GK_RECT: first texel, flips, colour */
    int textured, raw, semi, semimode, dither;
    int tx, ty, depth, cx, cy;         /* texture page corner, 0 = 4-bit, 1 = 8-bit, 2 = 15-bit, palette corner */
    unsigned tw[4];                    /* texture window: mask x, mask y, offset x, offset y */
    int clip[4];                       /* drawing area, inclusive: x1, y1, x2, y2 */
    unsigned col;                      /* GK_PIX, GK_FILL: 15-bit colour */
    int setm, chkm;                    /* mask bit: set it on drawn pixels, skip pixels that have it (filled in by glg_submit) */
} GPrim;

int  glg_init(int scale);              /* 1 when the graphics card can do it; sets up (or resizes) the picture */
void glg_submit(const GPrim *p);       /* queue a primitive */
void glg_mask(int set, int check);     /* GP0 e6: the mask-bit mode for what is queued from now on */
void glg_dirty(int y0, int y1);        /* vram rows y0..y1 changed on the CPU */
void glg_flush(void);                  /* draw everything queued */
void glg_present(int x, int y, int w, int h);                /* copy a console-pixel box of the picture into the bound 2D texture */
void glg_read(int x, int y, int w, int h, unsigned *dst);    /* the same box as 0x00RRGGBB pixels, top row first */
#endif
