/* Software implementation of the PS1 GPU (GP0/GP1) rendering into VRAM. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   /* worker threads; before hw.h, whose R macro clashes with it */
#include <stdlib.h>
#include "hw.h"

u16 vram[512 * 1024];
#define VR(x, y) vram[(((y) & 511) << 10) | ((x) & 1023)]

/* Internal resolution. vram always holds the console-exact picture and is what
   textures, palettes and read-backs use. With a scale above 1, hv is a second
   copy at that multiple: polygons are rasterised into it again at the higher
   resolution, everything else is written as blocks, and the display shows it. */
int g_gpu_scale = 1;          /* 1, 2 or 4 */
static u16 *hv; static int hv_scale;
#define HS hv_scale
#define HV(x, y) hv[(size_t)((y) & (512 * HS - 1)) * (size_t)(1024 * HS) + (size_t)((x) & (1024 * HS - 1))]
static void hi_flush(void);
const u16 *gpu_hires(int *scale) { hi_flush(); *scale = HS; return HS > 1 ? hv : NULL; }
/* apply g_gpu_scale; the high-resolution copy starts as the console picture enlarged */
void gpu_set_scale(void) {
    int s = (g_gpu_scale == 2 || g_gpu_scale == 4) ? g_gpu_scale : 1;
    if (s == hv_scale) return;
    hi_flush();
    free(hv); hv = NULL; hv_scale = 1;
    if (s == 1) return;
    hv = malloc((size_t)1024 * s * 512 * s * sizeof *hv);
    if (!hv) return;
    hv_scale = s;
    for (int y = 0; y < 512 * s; y++) for (int x = 0; x < 1024 * s; x++) HV(x, y) = VR(x / s, y / s);
}

static struct {
    u32 texpage;              /* E1 bits */
    u32 tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    s32 dx1, dy1, dx2, dy2;   /* drawing area, inclusive */
    s32 ox, oy;               /* drawing offset */
    int set_mask, check_mask;
    u32 disp_x, disp_y, hr1, hr2, vr1, vr2, dmode;
    int disp_off, dma_dir;
    u32 gpuread;
} g;

static u32 frame_field;   /* toggles every vblank (interlace field) */
/* command FIFO */
static u32 fifo[64]; static int fifo_n, fifo_need;
static int poly_line;      /* polyline in progress */
/* vram transfer state */
static struct { int active, x, y, w, h, cx, cy; } xfer_in, xfer_out;

void gpu_init(void) {
    memset(&g, 0, sizeof g);
    g.disp_off = 1; g.dx2 = 1023; g.dy2 = 511;
    if (!hv_scale) hv_scale = 1;
    gpu_set_scale();
    g.hr1 = 0x200; g.hr2 = 0x200 + 2560; g.vr1 = 0x10; g.vr2 = 0x10 + 240;
}

/* ----------------------------------------------------------- pixels --- */
static const s8 dither[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

static inline u16 blend(u16 b, u16 f, int mode) {
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int fr = f & 31, fg = (f >> 5) & 31, fb = (f >> 10) & 31, r, gg, bl;
    switch (mode) {
    case 0: r = (br + fr) >> 1; gg = (bg + fg) >> 1; bl = (bb + fb) >> 1; break;
    case 1: r = br + fr; gg = bg + fg; bl = bb + fb; break;
    case 2: r = br - fr; gg = bg - fg; bl = bb - fb; break;
    default: r = br + (fr >> 2); gg = bg + (fg >> 2); bl = bb + (fb >> 2); break;
    }
    if (r > 31) r = 31; if (r < 0) r = 0;
    if (gg > 31) gg = 31; if (gg < 0) gg = 0;
    if (bl > 31) bl = 31; if (bl < 0) bl = 0;
    return (u16)(r | (gg << 5) | (bl << 10));
}

/* the part of the GPU state a primitive depends on while it is being drawn */
typedef struct { s32 dx1, dy1, dx2, dy2; u32 tw_mask_x, tw_mask_y, tw_off_x, tw_off_y; int dither, set_mask, check_mask; } DS;
static inline DS ds_now(void) {
    DS s = { g.dx1, g.dy1, g.dx2, g.dy2, g.tw_mask_x, g.tw_mask_y, g.tw_off_x, g.tw_off_y,
             (int)((g.texpage >> 9) & 1), g.set_mask, g.check_mask };
    return s;
}
typedef struct { int tx, ty, depth, cx, cy; } TexInfo;
typedef struct { int x, y, r, g, b, u, v; } Vtx;

static inline void put(const DS *s, u16 *d, u16 c, int semi, int mode) {
    if (s->check_mask && (*d & 0x8000)) return;
    if (semi) c = (u16)((c & 0x8000) | blend(*d, c, mode));
    *d = c | (s->set_mask ? 0x8000 : 0);
}

/* Work for the high-resolution copy is queued and drawn by worker threads, each
   owning interleaved bands of rows, so commands keep their order within a row.
   The queue is drained before anything reads the copy or writes it directly. */
enum { C_TRI, C_PIX, C_FILL };
typedef struct {
    u8 type, textured, raw, semi, gouraud, semimode; u16 col;
    int x, y, w, h;
    Vtx v[3]; TexInfo ti; DS ds;
} Cmd;
#define QCAP 60000
#define BAND_SHIFT 4
#define MAX_WORKERS 8
static Cmd *q; static int qn;
static int n_workers;
static HANDLE w_start[MAX_WORKERS], w_done[MAX_WORKERS];
static void hi_run(int id, int n);
static DWORD WINAPI worker_main(LPVOID arg) {
    int id = (int)(INT_PTR)arg;
    for (;;) { WaitForSingleObject(w_start[id], INFINITE); hi_run(id, n_workers); SetEvent(w_done[id]); }
    return 0;
}
static void hi_flush(void) {
    if (!qn) return;
    if (!n_workers) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int n = (int)si.dwNumberOfProcessors - 1;
        if (n < 1) n = 1; if (n > MAX_WORKERS) n = MAX_WORKERS;
        n_workers = n;
        for (int i = 0; i < n; i++) {
            w_start[i] = CreateEventA(NULL, FALSE, FALSE, NULL); w_done[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
            CloseHandle(CreateThread(NULL, 0, worker_main, (LPVOID)(INT_PTR)i, 0, NULL));
        }
    }
    for (int i = 0; i < n_workers; i++) SetEvent(w_start[i]);
    WaitForMultipleObjects((DWORD)n_workers, w_done, TRUE, INFINITE);
    qn = 0;
}
static Cmd *hi_cmd(int type) {
    if (!q) q = malloc(sizeof *q * QCAP);
    if (qn == QCAP) hi_flush();
    Cmd *c = &q[qn++]; c->type = (u8)type; c->ds = ds_now();
    return c;
}

/* one console pixel, and its block in the high-resolution copy */
static inline void plot(int x, int y, u16 c, int semi, int mode) {
    DS s = ds_now();
    put(&s, &VR(x, y), c, semi, mode);
    if (HS > 1) { Cmd *k = hi_cmd(C_PIX); k->x = x; k->y = y; k->col = c; k->semi = (u8)semi; k->semimode = (u8)mode; }
}

static inline u16 texel(const DS *s, const TexInfo *t, int u, int v) {
    u &= 0xff; v &= 0xff;
    u = (u & ~(s->tw_mask_x * 8)) | ((s->tw_off_x & s->tw_mask_x) * 8);
    v = (v & ~(s->tw_mask_y * 8)) | ((s->tw_off_y & s->tw_mask_y) * 8);
    switch (t->depth) {
    case 0: { u16 w = VR(t->tx + (u >> 2), t->ty + v); return VR(t->cx + ((w >> ((u & 3) * 4)) & 0xf), t->cy); }
    case 1: { u16 w = VR(t->tx + (u >> 1), t->ty + v); return VR(t->cx + ((w >> ((u & 1) * 8)) & 0xff), t->cy); }
    default: return VR(t->tx + u, t->ty + v);
    }
}

static inline u16 shade(int r, int gg, int b, int x, int y, int dith) {
    if (dith) { int d = dither[y & 3][x & 3]; r += d; gg += d; b += d; }
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (gg < 0) gg = 0; if (gg > 255) gg = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (u16)((r >> 3) | ((gg >> 3) << 5) | ((b >> 3) << 10));
}

static inline u16 modulate(u16 t, int r, int gg, int b, int x, int y, int dith) {
    int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
    return shade((tr * r) >> 7, (tg * gg) >> 7, (tb * b) >> 7, x, y, dith) | (t & 0x8000);
}

/* -------------------------------------------------------- triangles --- */
/* S = 1 draws into vram. S = HS draws into the high-resolution copy, and only
   the rows of band `id` out of `nb`. */
static void tri(const DS *ds, const Vtx *a0, const Vtx *a1, const Vtx *a2, int textured, int raw, int semi,
                int gouraud, const TexInfo *ti, int semimode, int S, int id, int nb) {
    Vtx s0 = *a0, s1 = *a1, s2 = *a2;
    const Vtx *v0 = &s0, *v1 = &s1, *v2 = &s2;
    /* reject oversized primitives like the hardware does */
    int minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    const Vtx *vv[3] = { v0, v1, v2 };
    for (int i = 1; i < 3; i++) {
        if (vv[i]->x < minx) minx = vv[i]->x; if (vv[i]->x > maxx) maxx = vv[i]->x;
        if (vv[i]->y < miny) miny = vv[i]->y; if (vv[i]->y > maxy) maxy = vv[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    s0.x *= S; s0.y *= S; s1.x *= S; s1.y *= S; s2.x *= S; s2.y *= S;
    minx *= S; maxx *= S; miny *= S; maxy *= S;
    s64 area = (s64)(v1->x - v0->x) * (v2->y - v0->y) - (s64)(v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return;
    if (area < 0) { const Vtx *t = v1; v1 = v2; v2 = t; area = -area; }
    if (minx < ds->dx1 * S) minx = ds->dx1 * S; if (maxx > (ds->dx2 + 1) * S - 1) maxx = (ds->dx2 + 1) * S - 1;
    if (miny < ds->dy1 * S) miny = ds->dy1 * S; if (maxy > (ds->dy2 + 1) * S - 1) maxy = (ds->dy2 + 1) * S - 1;
    if (minx > maxx || miny > maxy) return;
    int dith = ds->dither;
    if (!gouraud && (!textured || raw)) dith = 0;

    /* edge setup: e_i(x,y) = A*x + B*y + C, top-left rule bias */
    const Vtx *P[3] = { v0, v1, v2 };
    s64 A[3], B[3], C[3]; int bias[3];
    for (int i = 0; i < 3; i++) {
        const Vtx *a = P[(i + 1) % 3], *b = P[(i + 2) % 3];
        A[i] = a->y - b->y; B[i] = b->x - a->x;
        C[i] = (s64)a->x * b->y - (s64)a->y * b->x;
        int top_left = (A[i] > 0) || (A[i] == 0 && B[i] < 0);
        bias[i] = top_left ? 0 : -1;
    }
    double inv = 1.0 / (double)area;
    for (int y = miny; y <= maxy; y++) {
        if (nb > 1 && ((y >> BAND_SHIFT) % nb) != id) continue;
        /* the pixels of this row inside all three edges form one span [xa, xb] */
        s64 e[3]; int xa = minx, xb = maxx, empty = 0;
        for (int i = 0; i < 3; i++) {
            e[i] = A[i] * minx + B[i] * y + C[i];
            s64 t = e[i] + bias[i];                       /* inside when t + A*k >= 0 */
            if (A[i] > 0) {
                if (t < 0) { s64 k = (-t + A[i] - 1) / A[i]; if (k > maxx - minx) empty = 1; else if (minx + (int)k > xa) xa = minx + (int)k; }
            } else if (A[i] < 0) {
                if (t < 0) empty = 1;
                else { s64 k = t / -A[i]; if (k < maxx - minx && minx + (int)k < xb) xb = minx + (int)k; }
            } else if (t < 0) empty = 1;
        }
        if (empty || xa > xb) continue;
        s64 e0 = e[0] + A[0] * (xa - minx), e1 = e[1] + A[1] * (xa - minx), e2 = e[2] + A[2] * (xa - minx);
        u16 *row = S == 1 ? &vram[(y & 511) << 10] : hv + (size_t)(y & (512 * S - 1)) * (size_t)(1024 * S);
        int xmask = 1024 * S - 1;
        if (S > 1) {
            /* high-resolution copy: every colour and texture coordinate is linear in x, so step them
               across the span instead of rebuilding them from the three edge values at each pixel
               (this copy is drawn 4 or 16 times as many pixels as the console picture) */
            double cr = (e0 * v0->r + e1 * v1->r + e2 * v2->r) * inv, sr = (A[0] * v0->r + A[1] * v1->r + A[2] * v2->r) * inv;
            double cg = (e0 * v0->g + e1 * v1->g + e2 * v2->g) * inv, sg = (A[0] * v0->g + A[1] * v1->g + A[2] * v2->g) * inv;
            double cb = (e0 * v0->b + e1 * v1->b + e2 * v2->b) * inv, sb = (A[0] * v0->b + A[1] * v1->b + A[2] * v2->b) * inv;
            double cu = (e0 * v0->u + e1 * v1->u + e2 * v2->u) * inv, su = (A[0] * v0->u + A[1] * v1->u + A[2] * v2->u) * inv;
            double cv = (e0 * v0->v + e1 * v1->v + e2 * v2->v) * inv, sv = (A[0] * v0->v + A[1] * v1->v + A[2] * v2->v) * inv;
            if (!gouraud) { cr = v0->r; cg = v0->g; cb = v0->b; sr = sg = sb = 0; }
            for (int x = xa; x <= xb; x++, cr += sr, cg += sg, cb += sb, cu += su, cv += sv) {
                int r = (int)(cr + 0.5), gg = (int)(cg + 0.5), b = (int)(cb + 0.5);
                if (textured) {
                    u16 t = texel(ds, ti, (int)cu, (int)cv);
                    if (!t) continue;
                    u16 c = raw ? t : modulate(t, r, gg, b, x, y, dith);
                    put(ds, &row[x & xmask], c, semi && (t & 0x8000), semimode);
                } else {
                    put(ds, &row[x & xmask], shade(r, gg, b, x, y, dith), semi, semimode);
                }
            }
            continue;
        }
        for (int x = xa; x <= xb; x++, e0 += A[0], e1 += A[1], e2 += A[2]) {
            double w0 = e0 * inv, w1 = e1 * inv, w2 = e2 * inv;
            int r = v0->r, gg = v0->g, b = v0->b;
            if (gouraud) {
                r = (int)(w0 * v0->r + w1 * v1->r + w2 * v2->r + 0.5);
                gg = (int)(w0 * v0->g + w1 * v1->g + w2 * v2->g + 0.5);
                b = (int)(w0 * v0->b + w1 * v1->b + w2 * v2->b + 0.5);
            }
            if (textured) {
                int u = (int)(w0 * v0->u + w1 * v1->u + w2 * v2->u);
                int v = (int)(w0 * v0->v + w1 * v1->v + w2 * v2->v);
                u16 t = texel(ds, ti, u, v);
                if (!t) continue;
                u16 c = raw ? t : modulate(t, r, gg, b, x, y, dith);
                put(ds, &row[x & xmask], c, semi && (t & 0x8000), semimode);
            } else {
                put(ds, &row[x & xmask], shade(r, gg, b, x, y, dith), semi, semimode);
            }
        }
    }
}

/* draw the queued commands into the rows of band `id` out of `n` */
static void hi_run(int id, int n) {
    int S = HS;
    for (int k = 0; k < qn; k++) {
        const Cmd *c = &q[k];
        if (c->type == C_TRI) {
            tri(&c->ds, &c->v[0], &c->v[1], &c->v[2], c->textured, c->raw, c->semi, c->gouraud, &c->ti, c->semimode, S, id, n);
        } else if (c->type == C_PIX) {
            for (int j = 0; j < S; j++) {
                int y = c->y * S + j;
                if (((y >> BAND_SHIFT) % n) != id) continue;
                for (int i = 0; i < S; i++) put(&c->ds, &HV(c->x * S + i, y), c->col, c->semi, c->semimode);
            }
        } else {
            for (int yy = 0; yy < c->h * S; yy++) {
                int y = c->y * S + yy;
                if (((y >> BAND_SHIFT) % n) != id) continue;
                for (int xx = 0; xx < c->w * S; xx++) HV(c->x * S + xx, y) = c->col;
            }
        }
    }
}

static inline s32 sx11(u32 v) { return ((s32)(v << 21)) >> 21; }

static void cmd_polygon(void) {
    u32 c = fifo[0], op = c >> 24;
    int quad = op & 8, textured = op & 4, semi = op & 2, raw = op & 1, gouraud = op & 0x10;
    int n = quad ? 4 : 3, k = 1;
    Vtx v[4]; TexInfo ti = { 0 };
    u32 clut = 0, tpage = g.texpage;
    for (int i = 0; i < n; i++) {
        u32 col = (i > 0 && gouraud) ? fifo[k++] : fifo[0];
        u32 xy = fifo[k++];
        v[i].r = col & 0xff; v[i].g = (col >> 8) & 0xff; v[i].b = (col >> 16) & 0xff;
        v[i].x = sx11(xy) + g.ox; v[i].y = sx11(xy >> 16) + g.oy;
        if (textured) {
            u32 uv = fifo[k++];
            v[i].u = uv & 0xff; v[i].v = (uv >> 8) & 0xff;
            if (i == 0) clut = uv >> 16;
            if (i == 1) tpage = uv >> 16;
        } else v[i].u = v[i].v = 0;
    }
    int semimode = (g.texpage >> 5) & 3;
    if (textured) {
        g.texpage = (g.texpage & ~0x9ffu) | (tpage & 0x9ff);
        semimode = (tpage >> 5) & 3;
        ti.tx = (tpage & 0xf) * 64; ti.ty = ((tpage >> 4) & 1) * 256;
        ti.depth = (tpage >> 7) & 3; if (ti.depth == 3) ti.depth = 2;
        ti.cx = (clut & 0x3f) * 16; ti.cy = (clut >> 6) & 0x1ff;
    }
    if (raw && !textured) raw = 0;
    DS ds = ds_now();
    for (int t = 0; t < (quad ? 2 : 1); t++) {
        tri(&ds, &v[t], &v[t + 1], &v[t + 2], textured, raw, semi, gouraud, &ti, semimode, 1, 0, 1);
        if (HS > 1) {
            Cmd *c = hi_cmd(C_TRI);
            c->v[0] = v[t]; c->v[1] = v[t + 1]; c->v[2] = v[t + 2]; c->ti = ti;
            c->textured = textured != 0; c->raw = raw != 0; c->semi = semi != 0; c->gouraud = gouraud != 0; c->semimode = (u8)semimode;
        }
    }
}

static void cmd_rect(void) {
    u32 c = fifo[0], op = c >> 24;
    int textured = op & 4, semi = op & 2, raw = op & 1, k = 1;
    u32 xy = fifo[k++], uv = textured ? fifo[k++] : 0;
    int w, h;
    switch ((op >> 3) & 3) {
    case 0: w = fifo[k] & 0x3ff; h = (fifo[k] >> 16) & 0x1ff; break;
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    int x0 = sx11(xy) + g.ox, y0 = sx11(xy >> 16) + g.oy;
    int r = c & 0xff, gg = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
    TexInfo ti;
    ti.tx = (g.texpage & 0xf) * 64; ti.ty = ((g.texpage >> 4) & 1) * 256;
    ti.depth = (g.texpage >> 7) & 3; if (ti.depth == 3) ti.depth = 2;
    ti.cx = ((uv >> 16) & 0x3f) * 16; ti.cy = (uv >> 22) & 0x1ff;
    int semimode = (g.texpage >> 5) & 3;
    int flipx = (g.texpage >> 12) & 1, flipy = (g.texpage >> 13) & 1;
    u16 flat = shade(r, gg, b, 0, 0, 0);
    DS ds = ds_now();
    for (int yy = 0; yy < h; yy++) {
        int y = y0 + yy; if (y < g.dy1 || y > g.dy2) continue;
        for (int xx = 0; xx < w; xx++) {
            int x = x0 + xx; if (x < g.dx1 || x > g.dx2) continue;
            if (textured) {
                int u = (int)(uv & 0xff) + (flipx ? -xx : xx), v = (int)((uv >> 8) & 0xff) + (flipy ? -yy : yy);
                u16 t = texel(&ds, &ti, u, v);
                if (!t) continue;
                u16 col = raw ? t : modulate(t, r, gg, b, x, y, 0);
                plot(x, y, col, semi && (t & 0x8000), semimode);
            } else plot(x, y, flat, semi, semimode);
        }
    }
}

static void line(int x0, int y0, int x1, int y1, u32 c0, u32 c1, int gouraud, int semi) {
    int dx = x1 - x0, dy = y1 - y0;
    if ((dx < 0 ? -dx : dx) >= 1024 || (dy < 0 ? -dy : dy) >= 512) return;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int semimode = (g.texpage >> 5) & 3, dith = (g.texpage >> 9) & 1 && gouraud;
    for (int i = 0; i <= steps; i++) {
        int x = steps ? x0 + (dx * i + (dx >= 0 ? steps / 2 : -steps / 2)) / steps : x0;
        int y = steps ? y0 + (dy * i + (dy >= 0 ? steps / 2 : -steps / 2)) / steps : y0;
        if (x < g.dx1 || x > g.dx2 || y < g.dy1 || y > g.dy2) continue;
        int r = c0 & 0xff, gg = (c0 >> 8) & 0xff, b = (c0 >> 16) & 0xff;
        if (gouraud && steps) {
            r += ((int)(c1 & 0xff) - r) * i / steps;
            gg += ((int)((c1 >> 8) & 0xff) - gg) * i / steps;
            b += ((int)((c1 >> 16) & 0xff) - b) * i / steps;
        }
        plot(x, y, shade(r, gg, b, x, y, dith), semi, semimode);
    }
}

/* ---------------------------------------------------------- GP0 ------- */
static int cmd_len(u32 w) {
    u32 op = w >> 24;
    if (op >= 0x20 && op < 0x40) {
        int n = (op & 8) ? 4 : 3;
        return 1 + n * (1 + ((op & 4) ? 1 : 0)) + ((op & 0x10) ? n - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) return (op & 0x10) ? 4 : 3;
    if (op >= 0x60 && op < 0x80) return 2 + ((op & 4) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xa0) return 4;
    if (op >= 0xa0 && op < 0xc0) return 3;
    if (op >= 0xc0 && op < 0xe0) return 3;
    return 1;
}

static void exec_cmd(void) {
    { extern unsigned g_stat_prims; g_stat_prims++; }
    u32 w = fifo[0], op = w >> 24;
    if (op >= 0x20 && op < 0x40) { cmd_polygon(); return; }
    if (op >= 0x40 && op < 0x60) {
        int gouraud = op & 0x10, semi = op & 2;
        u32 c0 = fifo[0], c1 = gouraud ? fifo[2] : fifo[0];
        u32 p0 = fifo[1], p1 = gouraud ? fifo[3] : fifo[2];
        line(sx11(p0) + g.ox, sx11(p0 >> 16) + g.oy, sx11(p1) + g.ox, sx11(p1 >> 16) + g.oy, c0, c1, gouraud, semi);
        if (op & 8) poly_line = 1;  /* polyline: keep consuming vertices */
        return;
    }
    if (op >= 0x60 && op < 0x80) { cmd_rect(); return; }
    switch (op) {
    case 0x02: {
        u32 c = fifo[0];
        u16 col = (u16)(((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10));
        int x = fifo[1] & 0x3f0, y = (fifo[1] >> 16) & 0x1ff;
        int ww = ((fifo[2] & 0x3ff) + 15) & ~15, hh = (fifo[2] >> 16) & 0x1ff;
        hi_flush();   /* queued polygons sample textures from vram: draw them before it changes */
        for (int yy = 0; yy < hh; yy++) for (int xx = 0; xx < ww; xx++) VR(x + xx, y + yy) = col;
        if (HS > 1) { Cmd *k = hi_cmd(C_FILL); k->x = x; k->y = y; k->w = ww; k->h = hh; k->col = col; }
        return;
    }
    case 0x01: case 0x00: case 0x1f: return;
    case 0xe1: g.texpage = w & 0x3fff; return;
    case 0xe2: g.tw_mask_x = w & 31; g.tw_mask_y = (w >> 5) & 31; g.tw_off_x = (w >> 10) & 31; g.tw_off_y = (w >> 15) & 31; return;
    case 0xe3: g.dx1 = w & 0x3ff; g.dy1 = (w >> 10) & 0x1ff; return;
    case 0xe4: g.dx2 = w & 0x3ff; g.dy2 = (w >> 10) & 0x1ff; return;
    case 0xe5: g.ox = sx11(w); g.oy = sx11(w >> 11); return;
    case 0xe6: g.set_mask = w & 1; g.check_mask = (w >> 1) & 1; return;
    }
    if (op >= 0x80 && op < 0xa0) {
        int sx = fifo[1] & 0x3ff, sy = (fifo[1] >> 16) & 0x1ff, dx = fifo[2] & 0x3ff, dy = (fifo[2] >> 16) & 0x1ff;
        int ww = ((fifo[3] - 1) & 0x3ff) + 1, hh = (((fifo[3] >> 16) - 1) & 0x1ff) + 1;
        hi_flush();   /* before vram changes: queued polygons still need the old texture data */
        for (int yy = 0; yy < hh; yy++) for (int xx = 0; xx < ww; xx++) {
            u16 s = VR(sx + xx, sy + yy); u16 *d = &VR(dx + xx, dy + yy);
            if (g.check_mask && (*d & 0x8000)) continue;
            *d = s | (g.set_mask ? 0x8000 : 0);
        }
        if (HS > 1) for (int yy = 0; yy < hh * HS; yy++) for (int xx = 0; xx < ww * HS; xx++) {
            u16 s = HV(sx * HS + xx, sy * HS + yy); u16 *d = &HV(dx * HS + xx, dy * HS + yy);
            if (g.check_mask && (*d & 0x8000)) continue;
            *d = s | (g.set_mask ? 0x8000 : 0);
        }
        return;
    }
    if (op >= 0xa0 && op < 0xc0) {
        xfer_in.x = fifo[1] & 0x3ff; xfer_in.y = (fifo[1] >> 16) & 0x1ff;
        xfer_in.w = ((fifo[2] - 1) & 0x3ff) + 1; xfer_in.h = (((fifo[2] >> 16) - 1) & 0x1ff) + 1;
        xfer_in.cx = xfer_in.cy = 0; xfer_in.active = 1;
        hi_flush();   /* the upload rewrites vram pixel by pixel: draw queued polygons that sample it first */
        return;
    }
    if (op >= 0xc0 && op < 0xe0) {
        xfer_out.x = fifo[1] & 0x3ff; xfer_out.y = (fifo[1] >> 16) & 0x1ff;
        xfer_out.w = ((fifo[2] - 1) & 0x3ff) + 1; xfer_out.h = (((fifo[2] >> 16) - 1) & 0x1ff) + 1;
        xfer_out.cx = xfer_out.cy = 0; xfer_out.active = 1;
        return;
    }
}

static void xfer_pixel(u16 p) {
    plot(xfer_in.x + xfer_in.cx, xfer_in.y + xfer_in.cy, p, 0, 0);
    if (++xfer_in.cx >= xfer_in.w) { xfer_in.cx = 0; if (++xfer_in.cy >= xfer_in.h) xfer_in.active = 0; }
}

static u32 pl_prev_xy, pl_prev_col, pl_col; static int pl_state;

void gpu_write_gp0(u32 v) {
    if (xfer_in.active) {
        xfer_pixel((u16)v);
        if (xfer_in.active) xfer_pixel((u16)(v >> 16));
        return;
    }
    if (poly_line) {
        u32 op = fifo[0] >> 24; int gouraud = op & 0x10;
        if ((v & 0xf000f000u) == 0x50005000u) { poly_line = 0; pl_state = 0; return; }
        if (pl_state == 0) {
            pl_prev_xy = gouraud ? fifo[3] : fifo[2];
            pl_prev_col = gouraud ? fifo[2] : fifo[0];
            pl_state = 1;
        }
        if (gouraud && pl_state == 1) { pl_col = v; pl_state = 2; return; }
        u32 col = gouraud ? pl_col : fifo[0];
        line(sx11(pl_prev_xy) + g.ox, sx11(pl_prev_xy >> 16) + g.oy, sx11(v) + g.ox, sx11(v >> 16) + g.oy,
             pl_prev_col, col, gouraud, op & 2);
        pl_prev_xy = v; pl_prev_col = col; pl_state = 1;
        return;
    }
    if (fifo_n == 0) fifo_need = cmd_len(v);
    fifo[fifo_n++] = v;
    if (fifo_n >= fifo_need) { exec_cmd(); fifo_n = 0; }
}

u32 gpu_read(void) {
    if (xfer_out.active) {
        u32 r = 0;
        for (int k = 0; k < 2; k++) {
            u16 p = VR(xfer_out.x + xfer_out.cx, xfer_out.y + xfer_out.cy);
            r |= (u32)p << (16 * k);
            if (++xfer_out.cx >= xfer_out.w) { xfer_out.cx = 0; if (++xfer_out.cy >= xfer_out.h) { xfer_out.active = 0; break; } }
        }
        g.gpuread = r;
    }
    return g.gpuread;
}

void gpu_write_gp1(u32 v) {
    u32 op = (v >> 24) & 0x3f;
    switch (op) {
    case 0x00: gpu_init(); fifo_n = 0; xfer_in.active = xfer_out.active = 0; poly_line = 0; break;
    case 0x01: fifo_n = 0; xfer_in.active = 0; poly_line = 0; break;
    case 0x02: break;
    case 0x03: g.disp_off = v & 1; break;
    case 0x04: g.dma_dir = v & 3; break;
    case 0x05: if (g.disp_x != (v & 0x3fe) || g.disp_y != ((v >> 10) & 0x1ff)) { extern void cpu_flip(void); cpu_flip(); }
        g.disp_x = v & 0x3fe; g.disp_y = (v >> 10) & 0x1ff; break;
    case 0x06: g.hr1 = v & 0xfff; g.hr2 = (v >> 12) & 0xfff; break;
    case 0x07: g.vr1 = v & 0x3ff; g.vr2 = (v >> 10) & 0x3ff; break;
    case 0x08: g.dmode = v & 0xff; break;
    case 0x10:
        switch (v & 7) {
        case 2: g.gpuread = g.tw_mask_x | (g.tw_mask_y << 5) | (g.tw_off_x << 10) | (g.tw_off_y << 15); break;
        case 3: g.gpuread = g.dx1 | (g.dy1 << 10); break;
        case 4: g.gpuread = g.dx2 | (g.dy2 << 10); break;
        case 5: g.gpuread = (g.ox & 0x7ff) | ((g.oy & 0x7ff) << 11); break;
        case 7: g.gpuread = 2; break;
        }
        break;
    }
}

u32 gpu_stat(void) {
    u32 s = g.texpage & 0x7ff;
    s |= (u32)g.set_mask << 11 | (u32)g.check_mask << 12;
    s |= (g.texpage & 0x800) << 4;
    u32 hres = g.dmode & 3, hr2 = (g.dmode >> 6) & 1;
    s |= hr2 << 16 | hres << 17 | ((g.dmode >> 2) & 1) << 19 | ((g.dmode >> 3) & 1) << 20;
    s |= ((g.dmode >> 4) & 1) << 21 | ((g.dmode >> 5) & 1) << 22 | (u32)g.disp_off << 23;
    s |= 0x1c000000u;                      /* ready for cmd / vram send / dma */
    s |= (u32)g.dma_dir << 29;
    if (g.dma_dir == 1 || g.dma_dir == 2) s |= 1u << 25;
    if (g.dma_dir == 3) s |= 1u << 25;
    u64 line = (cpu.cycles % CYCLES_PER_FRAME) / CYCLES_PER_LINE;
    int interlace = (g.dmode >> 5) & 1;
    if (interlace) { if (frame_field) s |= 1u << 31; s |= frame_field << 13; }
    else if (line & 1) s |= 1u << 31;
    return s;
}

void gpu_vblank(void) { frame_field ^= 1; }

void gpu_display_area(DisplayArea *d) {
    static const int widths[4] = { 256, 320, 512, 640 };
    int w = (g.dmode & 0x40) ? 368 : widths[g.dmode & 3];
    int h = ((g.dmode & 0x24) == 0x24) ? 480 : 240;
    int vis = (int)(g.vr2 - g.vr1);
    if (h == 480) vis *= 2;
    if (vis > 0 && vis < h) h = vis;
    d->x = g.disp_x; d->y = g.disp_y; d->w = w; d->h = h;
    d->depth24 = (g.dmode >> 4) & 1;
    d->enabled = !g.disp_off;
}
