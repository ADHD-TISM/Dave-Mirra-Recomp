/* The vertices of one primitive, and the display-box helper, for the GPU hi-res renderers (gpu_gl.c and gpu_vk.c): the same GlsVert layout, the same positions
   (vram pixels times the scale, triangle corners half a pixel in), the same flags for the fragment shader. Included by both files.
   gv_emit appends to a GpuVerts; when it cannot grow the array it sets fail and appends nothing more, so the caller drops the whole
   run instead of drawing a half-built triangle. */
#ifndef GPU_EMIT_H
#define GPU_EMIT_H
#include <stdlib.h>
#include "gpu_gl.h"
#include "gl_stream.h"

#define GV_MAX_VERTS 24   /* the most vertices gv_emit adds for one primitive: four pieces of a wrapped box, two triangles each */
typedef struct { GlsVert *v; int n, cap, fail; } GpuVerts;

static inline void gv_vert(GpuVerts *g, const GPrim *p, int flags, float px, float py, float r, float gr, float b, float u, float v) {
    if (g->fail) return;
    if (g->n == g->cap) {
        GlsVert *nb = realloc(g->v, sizeof *g->v * (size_t)(g->cap ? g->cap * 2 : 4096));
        if (!nb) { g->fail = 1; return; }
        g->v = nb; g->cap = g->cap ? g->cap * 2 : 4096;
    }
    GlsVert *o = &g->v[g->n++];
    o->x = px; o->y = py;
    o->c[0] = r / 255.0f; o->c[1] = gr / 255.0f; o->c[2] = b / 255.0f;
    o->t0[0] = u; o->t0[1] = v; o->t0[2] = (float)p->tx; o->t0[3] = (float)p->ty;
    o->t1[0] = (float)p->cx; o->t1[1] = (float)p->cy; o->t1[2] = (float)p->depth; o->t1[3] = (float)flags;
    for (int i = 0; i < 4; i++) o->t2[i] = (float)p->tw[i];
}
static inline void gv_quad(GpuVerts *g, const GPrim *p, int flags, float x0, float y0, float x1, float y1, float r, float gr, float b,
                    float ul, float vt, float ur, float vb) {
    gv_vert(g, p, flags, x0, y0, r, gr, b, ul, vt); gv_vert(g, p, flags, x1, y0, r, gr, b, ur, vt); gv_vert(g, p, flags, x1, y1, r, gr, b, ur, vb);
    gv_vert(g, p, flags, x0, y0, r, gr, b, ul, vt); gv_vert(g, p, flags, x1, y1, r, gr, b, ur, vb); gv_vert(g, p, flags, x0, y1, r, gr, b, ul, vb);
}
static inline int gv_flags(const GPrim *p) {
    return (p->textured || p->kind == GK_BLIT ? 1 : 0) | (p->raw ? 2 : 0) | (p->dither ? 4 : 0) | (p->kind == GK_BLIT ? 8 : 0);
}
/* a box in vram wraps around the edges: up to four pieces */
static inline void gv_pieces(int x, int y, int w, int h, int out[4][4], int *n) {
    int xs[2][2], ys[2][2], nx = 0, ny = 0;
    x &= 1023; y &= 511;
    xs[nx][0] = x; xs[nx][1] = w > 1024 - x ? 1024 - x : w; nx++;
    if (w > 1024 - x) { xs[nx][0] = 0; xs[nx][1] = w - (1024 - x); nx++; }
    ys[ny][0] = y; ys[ny][1] = h > 512 - y ? 512 - y : h; ny++;
    if (h > 512 - y) { ys[ny][0] = 0; ys[ny][1] = h - (512 - y); ny++; }
    *n = 0;
    for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {
        out[*n][0] = xs[i][0]; out[*n][1] = ys[j][0]; out[*n][2] = xs[i][1]; out[*n][3] = ys[j][1]; (*n)++;
    }
}
/* The display box wraps around the edges of vram like the CPU picture does (a 640 wide box starting at x 512, say): cut one axis
   of it into the one or two runs that do not wrap. out[i] = { first vram coordinate, offset inside the box, length }. */
static inline int gv_runs(int s, int n, int size, int out[2][3]) {
    s &= size - 1;
    if (n > size) n = size;
    out[0][0] = s; out[0][1] = 0; out[0][2] = n < size - s ? n : size - s;
    if (n <= size - s) return 1;
    out[1][0] = 0; out[1][1] = size - s; out[1][2] = n - (size - s);
    return 2;
}
static inline void gv_emit(GpuVerts *g, const GPrim *p, int scale) {
    float s = (float)scale;
    int fl = gv_flags(p);
    switch (p->kind) {
    case GK_TRI:
        for (int i = 0; i < 3; i++)   /* the picture is sampled at pixel corners, so corners sit half a pixel in */
            gv_vert(g, p, fl, p->vx[i] * s + 0.5f, p->vy[i] * s + 0.5f, (float)p->vr[i], (float)p->vg[i], (float)p->vb[i], (float)p->vu[i], (float)p->vv[i]);
        break;
    case GK_RECT: {
        float ul = (float)p->u0, vt = (float)p->v0, ur, vb;
        if (p->flipx) ul += 1.0f - 0.004f;
        if (p->flipy) vt += 1.0f - 0.004f;
        ur = p->flipx ? ul - (float)p->w : ul + (float)p->w;
        vb = p->flipy ? vt - (float)p->h : vt + (float)p->h;
        gv_quad(g, p, fl, p->x * s, p->y * s, (p->x + p->w) * s, (p->y + p->h) * s, (float)p->r, (float)p->g, (float)p->b, ul, vt, ur, vb);
        break;
    }
    case GK_PIX: case GK_FILL: {
        int pc[4][4], n;
        gv_pieces(p->x, p->y, p->w, p->h, pc, &n);
        float r = (float)((p->col & 31) * 8), gr = (float)(((p->col >> 5) & 31) * 8), b = (float)(((p->col >> 10) & 31) * 8);
        for (int i = 0; i < n; i++) gv_quad(g, p, fl, pc[i][0] * s, pc[i][1] * s, (pc[i][0] + pc[i][2]) * s, (pc[i][1] + pc[i][3]) * s, r, gr, b, 0, 0, 0, 0);
        break;
    }
    case GK_BLIT: {
        int pc[4][4], n;
        gv_pieces(p->x, p->y, p->w, p->h, pc, &n);
        for (int i = 0; i < n; i++)
            gv_quad(g, p, fl, pc[i][0] * s, pc[i][1] * s, (pc[i][0] + pc[i][2]) * s, (pc[i][1] + pc[i][3]) * s, 255, 255, 255,
                    (float)pc[i][0], (float)pc[i][1], (float)(pc[i][0] + pc[i][2]), (float)(pc[i][1] + pc[i][3]));
        break;
    }
    }
}
#endif
