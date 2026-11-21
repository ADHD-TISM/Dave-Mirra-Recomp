/* Checks runtime/gpu_vk.c (the GPU hi-res renderer on Vulkan) against vram and against a small CPU model, on the machine's own Vulkan
   driver (Windows; it opens a window, makes the Vulkan backend of runtime/gfx_vk.c and asks it for the renderer). Steps 2 and 3 of
   docs/vulkan-gpu-renderer-plan.md: random vram is drawn into the picture by a blit and read back, for display boxes that wrap at the
   vram edges, for rows the CPU changed afterwards (the dirty-row upload), at scales 2 and 4, and back; flat rectangles, triangles,
   fills and pixels at each of the five blend modes and with a drawing area; many primitives in one flush; and the presenter's copy
   of a box into the window, plain and with FSR. The later steps add textured drawing, copies and the mask bit. Every line should say ok.
   From the repository root:
     mkdir -p build && gcc -I runtime tools/vktest.c runtime/gfx_vk.c runtime/gpu_vk.c runtime/gfx_vk_spv.c runtime/plat_win32.c \
       -o build/vktest.exe -lgdi32 -lwinmm -lopengl32 && build/vktest.exe
   Two more runs: `vktest small` makes the vertex buffer 64 KB (DMRECOMP_VK_VBUF_KB, a test hook) so a flush has to split into several
   submits, and `vktest gate` checks that the renderer says no without DMRECOMP_VK_RENDERER. DMRECOMP_VK_VALIDATE=1 runs any of them
   under the validation layer if that is installed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "host.h"
#include "gfx.h"

u16 vram[512 * 1024];
void host_hotkey(int which) { (void)which; }
void host_quit(void) { exit(0); }

static const GfxBackend *be = &gfx_vk_backend;
static int S = 2, fails;
static unsigned want(int x, int y) { u16 p = vram[(y & 511) * 1024 + (x & 1023)]; return ((p & 31) << 3) << 16 | (((p >> 5) & 31) << 3) << 8 | (((p >> 10) & 31) << 3); }
static unsigned col15(int r, int g, int b) { return (unsigned)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }
static void report(const char *what, long bad, long total) {
    printf("%-50s %s (%ld of %ld pixels differ)\n", what, bad ? "FAIL" : "ok", bad, total);
    if (bad) fails++;
}

static void blit(int x, int y, int w, int h) {   /* what gpu.c does for an upload: the rows are marked changed, then the box is copied from vram */
    GPrim b; memset(&b, 0, sizeof b); b.kind = GK_BLIT; b.x = x; b.y = y; b.w = w; b.h = h;
    be->r_dirty(y, y + h - 1); be->r_submit(&b); be->r_flush();
}
static void upload_all(void) { be->r_dirty(0, 511); blit(0, 0, 1024, 512); }
static unsigned buf[1024 * 4 * 512 * 4];
static void check_box(const char *what, int x, int y, int w, int h) {
    memset(buf, 0xAA, sizeof(unsigned) * (size_t)w * S * h * S);
    be->r_flush();
    be->r_read(x, y, w, h, buf);
    long bad = 0, first = -1;
    for (int j = 0; j < h * S; j++) for (int i = 0; i < w * S; i++) {
        unsigned got = buf[(size_t)j * w * S + i] & 0xffffff, exp = want(x + i / S, y + j / S);
        if (got != exp) { if (!bad) first = (long)j * w * S + i; bad++; }
    }
    report(what, bad, (long)w * S * h * S);
    if (bad) printf("   first bad pixel: x %ld y %ld of the picture\n", first % (w * S), first / (w * S));
}
static void randomise(int y0, int rows, unsigned seed) {   /* the CPU rewrites some rows of vram */
    srand(seed);
    for (int i = y0 * 1024; i < (y0 + rows) * 1024; i++) vram[i] = (u16)(rand() & 0x7fff);
}

/* the presenter: a box of the picture, copied into the window's picture and read back from the window */
static void frame(GfxFrame *f, int bx, int by, int bw, int bh) {
    int cw, ch; plat_client_size(&cw, &ch);
    memset(f, 0, sizeof *f);
    f->fb_w = bw * S; f->fb_h = bh * S; f->on_gpu = 1; f->box[0] = bx; f->box[1] = by; f->box[2] = bw; f->box[3] = bh;
    f->cw = cw; f->ch = ch; f->w = f->fb_w; f->h = f->fb_h; f->grab = 1;
}
static void check_present(const char *what, int bx, int by, int bw, int bh) {
    GfxFrame f; frame(&f, bx, by, bw, bh);
    if (f.w > f.cw || f.h > f.ch) { printf("%-50s skipped (the window is %dx%d, the picture %dx%d)\n", what, f.cw, f.ch, f.w, f.h); return; }
    be->r_flush();
    be->present(&f);
    int gw = 0, gh = 0; const uint32_t *g = be->grab(&gw, &gh);
    if (!g || gw != f.cw || gh != f.ch) { printf("%-50s FAIL (no window grab, or its size is wrong)\n", what); fails++; return; }
    int x0 = (f.cw - f.w) / 2, y0 = f.ch - f.h - (f.ch - f.h) / 2;   /* placed like the OpenGL presenter: the offset counted from the bottom */
    long bad = 0;
    for (int j = 0; j < f.h; j++) for (int i = 0; i < f.w; i++)
        if ((g[(size_t)(y0 + j) * gw + x0 + i] & 0xffffff) != want(bx + i / S, by + j / S)) bad++;
    for (int j = 0; j < f.ch; j++) for (int i = 0; i < f.cw; i++) {   /* and the bars around it are black */
        if (i >= x0 && i < x0 + f.w && j >= y0 && j < y0 + f.h) continue;
        if (g[(size_t)j * gw + i] & 0xffffff) bad++;
    }
    report(what, bad, (long)f.cw * f.ch);
}
/* FSR: the picture straight from the renderer's target must come out as the same picture sent from the CPU */
static void check_present_fsr(const char *what, int bx, int by, int bw, int bh) {
    GfxFrame f; frame(&f, bx, by, bw, bh);
    f.upscale = 1; f.fsr_sharp = 80; f.w = f.fb_w * 5 / 4; f.h = f.fb_h * 5 / 4;
    if (f.w > f.cw || f.h > f.ch) { printf("%-50s skipped (the window is too small)\n", what); return; }
    be->r_flush();
    be->present(&f);
    int gw = 0, gh = 0; const uint32_t *g = be->grab(&gw, &gh);
    if (!g) { printf("%-50s FAIL (no window grab)\n", what); fails++; return; }
    static uint32_t a[2000 * 1500];
    if ((long)gw * gh > 2000 * 1500) { printf("%-50s skipped (the window is too big for this check)\n", what); return; }
    memcpy(a, g, (size_t)gw * gh * 4);
    be->r_read(bx, by, bw, bh, buf);   /* the same picture, as the CPU would send it */
    GfxFrame c = f; c.on_gpu = 0; c.fb = buf;
    be->present(&c);
    const uint32_t *h2 = be->grab(&gw, &gh);
    long bad = 0;
    if (!h2) bad = 1; else for (long i = 0; i < (long)gw * gh; i++) if ((a[i] & 0xffffff) != (h2[i] & 0xffffff)) bad++;
    report(what, bad, (long)gw * gh);
}

/* ---- step 3: flat primitives. The model is the arithmetic of the shader and the blend unit on 8-bit values. ---- */
typedef struct { int x, y, w, h; } Box;
static int blend8(int mode, int s, int d) {   /* mode -1: opaque; 0: B/2 + F/2; 1: B + F; 2: B - F; 3: B + F/4 (colours are multiples of 8, so this is exact) */
    int v;
    switch (mode) {
    case -1: return s;
    case 0: return (s + d) / 2;
    case 1: v = s + d; return v > 255 ? 255 : v;
    case 2: v = d - s; return v < 0 ? 0 : v;
    default: v = d + s / 4; return v > 255 ? 255 : v;
    }
}
static unsigned model(int mode, unsigned s, unsigned d) {
    unsigned out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) out |= (unsigned)blend8(mode, (int)((s >> sh) & 255), (int)((d >> sh) & 255)) << sh;
    return out;
}
static unsigned reduce(int r, int g, int b) { return (unsigned)((r & ~7) << 16 | (g & ~7) << 8 | (b & ~7)); }   /* 8-bit colour as the shader keeps it: 5 bits */
static void draw(int kind, int mode, int x, int y, int w, int h, int r, int g, int b, const int clip[4]) {
    GPrim p; memset(&p, 0, sizeof p);
    p.semi = mode >= 0; p.semimode = mode >= 0 ? mode : 0;
    p.clip[0] = clip ? clip[0] : 0; p.clip[1] = clip ? clip[1] : 0; p.clip[2] = clip ? clip[2] : 1023; p.clip[3] = clip ? clip[3] : 511;
    if (kind == GK_RECT) { p.kind = GK_RECT; p.x = x; p.y = y; p.w = w; p.h = h; p.r = r; p.g = g; p.b = b; be->r_submit(&p); }
    else if (kind == GK_TRI) {   /* two triangles that make the rectangle: nothing may be drawn twice along the diagonal */
        p.kind = GK_TRI;
        int cx[2][3] = { { x, x + w, x }, { x + w, x + w, x } }, cy[2][3] = { { y, y, y + h }, { y, y + h, y + h } };
        for (int t = 0; t < 2; t++) {
            for (int i = 0; i < 3; i++) { p.vx[i] = cx[t][i]; p.vy[i] = cy[t][i]; p.vr[i] = r; p.vg[i] = g; p.vb[i] = b; }
            be->r_submit(&p);
        }
    }
    else {
        p.kind = kind; p.x = x; p.y = y; p.w = w; p.h = h; p.col = col15(r, g, b);
        if (kind == GK_FILL) p.semi = 0;   /* gpu.c never marks a fill translucent */
        be->r_submit(&p);
    }
}
static int inside(const Box *b, int x, int y) { return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h; }
/* draw one primitive on a fresh picture and compare the area around it with the model */
static void check_flat(const char *what, int kind, int mode, int x, int y, int w, int h, int r, int g, int b, const int clip[4]) {
    upload_all();
    draw(kind, mode, x, y, w, h, r, g, b, clip);
    Box rect = { x, y, w, h }, area = { clip ? clip[0] : 0, clip ? clip[1] : 0, clip ? clip[2] - clip[0] + 1 : 1024, clip ? clip[3] - clip[1] + 1 : 512 };
    int m = kind == GK_FILL ? -1 : mode;   /* a fill ignores the blend */
    unsigned s = reduce(r, g, b);
    Box bx = { x - 8, y - 8, w + 16, h + 16 };
    be->r_flush();
    memset(buf, 0xAA, sizeof(unsigned) * (size_t)bx.w * S * bx.h * S);
    be->r_read(bx.x, bx.y, bx.w, bx.h, buf);
    long bad = 0, first = -1;
    for (int j = 0; j < bx.h * S; j++) for (int i = 0; i < bx.w * S; i++) {
        int vx = bx.x + i / S, vy = bx.y + j / S;
        unsigned d = want(vx, vy), exp = inside(&rect, vx, vy) && inside(&area, vx, vy) ? model(m, s, d) : d;
        if ((buf[(size_t)j * bx.w * S + i] & 0xffffff) != exp) { if (!bad) first = (long)j * bx.w * S + i; bad++; }
    }
    report(what, bad, (long)bx.w * S * bx.h * S);
    if (bad) printf("   first bad pixel: x %ld y %ld of the picture\n", first % (bx.w * S), first / (bx.w * S));
}
static const char *mname[4] = { "average", "add", "subtract", "quarter" };

/* many primitives in one flush: the CPU model paints them in order on a canvas (with a small vertex buffer they cannot go in one submit) */
static void check_many(const char *what, int n) {
    upload_all();
    Box area = { 200, 100, 320, 300 };
    static unsigned canvas[300 * 320];
    for (int j = 0; j < area.h; j++) for (int i = 0; i < area.w; i++) canvas[j * area.w + i] = want(area.x + i, area.y + j);
    srand(99);
    for (int k = 0; k < n; k++) {
        int w = 4 + rand() % 40, h = 4 + rand() % 30, x = area.x + rand() % (area.w - w), y = area.y + rand() % (area.h - h);
        int r = rand() & 255, g = rand() & 255, b = rand() & 255, mode = rand() % 3 == 0 ? 1 + rand() % 3 : -1;   /* not the average: repeated, it halves into values the unorm rounding settles differently from the model */
        draw(GK_RECT, mode, x, y, w, h, r, g, b, NULL);
        for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) {
            unsigned *c = &canvas[(j - area.y) * area.w + (i - area.x)];
            *c = model(mode, reduce(r, g, b), *c);
        }
    }
    be->r_flush();
    memset(buf, 0xAA, sizeof(unsigned) * (size_t)area.w * S * area.h * S);
    be->r_read(area.x, area.y, area.w, area.h, buf);
    long bad = 0;
    for (int j = 0; j < area.h * S; j++) for (int i = 0; i < area.w * S; i++)
        if ((buf[(size_t)j * area.w * S + i] & 0xffffff) != canvas[(j / S) * area.w + i / S]) bad++;
    report(what, bad, (long)area.w * S * area.h * S);
}
/* rows rewritten on the CPU, rectangles, then a blit of those rows, all in one flush: with a small vertex buffer the flush splits, the
   upload goes with the first batch and the blit (last, in a later batch) has to find the rows in vram */
static void check_split_upload(const char *what, int n) {
    upload_all();
    Box area = { 200, 100, 320, 300 };
    static unsigned canvas[300 * 320];
    for (int j = 0; j < area.h; j++) for (int i = 0; i < area.w; i++) canvas[j * area.w + i] = want(area.x + i, area.y + j);
    randomise(250, 11, 77);   /* the CPU changes these rows: vram has them, the picture does not */
    be->r_dirty(250, 260);
    srand(5);
    for (int k = 0; k < n; k++) {
        int w = 4 + rand() % 40, h = 4 + rand() % 30, x = area.x + rand() % (area.w - w), y = area.y + rand() % (area.h - h);
        int r = rand() & 255, g = rand() & 255, b = rand() & 255;
        draw(GK_RECT, -1, x, y, w, h, r, g, b, NULL);
        for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) canvas[(j - area.y) * area.w + (i - area.x)] = reduce(r, g, b);
    }
    GPrim b; memset(&b, 0, sizeof b); b.kind = GK_BLIT; b.x = 0; b.y = 250; b.w = 1024; b.h = 11;
    be->r_submit(&b);
    for (int j = 250; j < 261; j++) for (int i = area.x; i < area.x + area.w; i++) canvas[(j - area.y) * area.w + (i - area.x)] = want(i, j);
    be->r_flush();
    memset(buf, 0xAA, sizeof(unsigned) * (size_t)area.w * S * area.h * S);
    be->r_read(area.x, area.y, area.w, area.h, buf);
    long bad = 0;
    for (int j = 0; j < area.h * S; j++) for (int i = 0; i < area.w * S; i++)
        if ((buf[(size_t)j * area.w * S + i] & 0xffffff) != canvas[(j / S) * area.w + i / S]) bad++;
    report(what, bad, (long)area.w * S * area.h * S);
}
static void check_detail(const char *what) {   /* a triangle finer than vram's pixels: its diagonal edge makes blocks that are not one colour */
    upload_all();
    GPrim p; memset(&p, 0, sizeof p); p.kind = GK_TRI; p.clip[2] = 1023; p.clip[3] = 511;
    int vx[3] = { 400, 700, 400 }, vy[3] = { 100, 100, 400 };
    for (int i = 0; i < 3; i++) { p.vx[i] = vx[i]; p.vy[i] = vy[i]; p.vr[i] = 200; p.vg[i] = 40; p.vb[i] = 120; }
    be->r_submit(&p); be->r_flush();
    memset(buf, 0, sizeof(unsigned) * 320 * S * 320 * S);
    be->r_read(390, 90, 320, 320, buf);
    long fine = 0, bad = 0;
    unsigned inner = reduce(200, 40, 120);
    for (int j = 0; j + 1 < 320 * S; j += S) for (int i = 0; i + 1 < 320 * S; i += S) {
        unsigned a0 = buf[(size_t)j * 320 * S + i] & 0xffffff, a1 = buf[(size_t)j * 320 * S + i + 1] & 0xffffff;
        unsigned a2 = buf[(size_t)(j + 1) * 320 * S + i] & 0xffffff, a3 = buf[(size_t)(j + 1) * 320 * S + i + 1] & 0xffffff;
        if (a1 != a0 || a2 != a0 || a3 != a0) fine++;
    }
    if ((buf[(size_t)(100 * S) * 320 * S + 100 * S] & 0xffffff) != inner) bad++;   /* a point well inside is the triangle's colour */
    printf("%-50s %s (%ld blocks finer than vram's pixels)\n", what, bad || !fine ? "FAIL" : "ok", fine);
    if (bad || !fine) fails++;
}

int main(int argc, char **argv) {
    int gate = argc > 1 && !strcmp(argv[1], "gate"), small = argc > 1 && !strcmp(argv[1], "small");
    _putenv(gate ? "DMRECOMP_VK_RENDERER=" : "DMRECOMP_VK_RENDERER=1");   /* the renderer is off for the game until the plan's last step */
    _putenv(small ? "DMRECOMP_VK_VBUF_KB=64" : "DMRECOMP_VK_VBUF_KB=");
    if (!be->init(960, 720, "vktest")) { printf("the Vulkan backend cannot start\n"); return 2; }
    if (gate) {
        int on = be->r_init(2);
        printf("%-50s %s\n", "renderer is off without DMRECOMP_VK_RENDERER", on ? "FAIL" : "ok");
        return on != 0;
    }
    srand(7);
    for (int i = 0; i < 512 * 1024; i++) vram[i] = (u16)(rand() & 0x7fff);   /* no mask bits: texels are plain colours */
    if (!be->r_init(S)) { printf("the Vulkan renderer cannot start\n"); return 2; }
    upload_all();
    check_box("plain box (100,50) 320x240", 100, 50, 320, 240);
    check_box("box wraps x: (900,50) 320x240", 900, 50, 320, 240);
    check_box("box wraps y: (100,400) 320x240", 100, 400, 320, 240);
    check_box("box wraps both: (900,400) 320x240", 900, 400, 320, 240);
    check_box("640 wide at x 512 (wraps)", 512, 0, 640, 240);
    check_box("whole vram", 0, 0, 1024, 512);
    /* the CPU changes rows, the renderer is told, and a blit of them brings them in */
    randomise(100, 21, 11); blit(0, 100, 1024, 21);
    check_box("rows 100..120 changed on the CPU", 0, 90, 1024, 40);
    randomise(0, 1, 12); randomise(511, 1, 13); be->r_dirty(0, 0); be->r_dirty(511, 511); blit(0, 0, 1024, 1); blit(0, 511, 1024, 1);
    check_box("first and last row changed", 0, 0, 1024, 512);
    randomise(200, 4, 14); be->r_dirty(200, 203); be->r_dirty(300, 300); randomise(300, 1, 15); blit(0, 200, 1024, 101);
    check_box("two separate spans marked, one blit", 0, 190, 1024, 120);
    check_box("whole vram again", 0, 0, 1024, 512);
    /* a blit of a box that wraps around the edges */
    randomise(480, 32, 16); be->r_dirty(480, 511); randomise(0, 16, 17); be->r_dirty(0, 15); blit(900, 480, 300, 48);
    check_box("blit of a box that wraps both edges", 900, 480, 300, 48);   /* only what was blitted is new */
    /* flat primitives */
    char name[96];
    static const int kinds[3] = { GK_RECT, GK_TRI, GK_PIX };
    static const char *kname[3] = { "rectangle", "triangles", "pixel" };
    for (int k = 0; k < 3; k++) for (int m = -1; m < 4; m++) {
        snprintf(name, sizeof name, "flat %s, %s%s", kname[k], m < 0 ? "opaque" : mname[m], m < 0 ? "" : " blend");
        if (kinds[k] == GK_PIX) check_flat(name, GK_PIX, m, 333, 222, 1, 1, 250, 120, 33, NULL);
        else check_flat(name, kinds[k], m, 300, 200, 111, 77, 250, 120, 33, NULL);
    }
    check_flat("fill", GK_FILL, -1, 300, 200, 111, 77, 250, 120, 33, NULL);
    check_flat("rectangle, black on a bright area (subtract)", GK_RECT, 2, 300, 200, 64, 64, 255, 255, 255, NULL);
    { int clip[4] = { 340, 220, 380, 250 }; check_flat("rectangle clipped to a drawing area", GK_RECT, -1, 300, 200, 111, 77, 10, 220, 90, clip); }
    { int clip[4] = { 340, 220, 380, 250 }; check_flat("blended triangles clipped to a drawing area", GK_TRI, 1, 300, 200, 111, 77, 60, 60, 200, clip); }
    { int clip[4] = { 500, 500, 100, 100 }; check_flat("an empty drawing area draws nothing", GK_RECT, -1, 300, 200, 111, 77, 10, 220, 90, clip); }
    check_many("many primitives in one flush (200)", 200);
    check_many("many primitives in one flush (1500)", 1500);
    check_split_upload("rows to upload, 400 rectangles, then a blit (one flush)", 400);
    check_detail("a triangle finer than vram's pixels");
    /* the presenter */
    upload_all();   /* the rows rewritten above are in the picture only where they were blitted */
    check_present("window picture, box (100,50) 320x240", 100, 50, 320, 240);
    check_present("window picture, box wraps (900,400)", 900, 400, 320, 240);
    check_present_fsr("window picture with FSR, same as from the CPU", 100, 50, 320, 240);
    /* scale 4: the picture is made again at four times the size, and back */
    S = 4;
    if (!be->r_init(S)) { printf("the renderer cannot make the 4x picture\n"); return 2; }
    upload_all();
    check_box("4x: plain box (100,50) 320x240", 100, 50, 320, 240);
    check_box("4x: box wraps both (900,400) 160x120", 900, 400, 160, 120);
    check_present("4x: window picture, box (100,50) 160x120", 100, 50, 160, 120);
    check_flat("4x: flat rectangle, average blend", GK_RECT, 0, 300, 200, 111, 77, 250, 120, 33, NULL);
    S = 2;
    if (!be->r_init(S)) { printf("the renderer cannot go back to the 2x picture\n"); return 2; }
    upload_all();
    check_box("back at 2x: plain box (100,50) 320x240", 100, 50, 320, 240);
    printf(fails ? "FAILURES: %d\n" : "all ok\n", fails);
    return fails != 0;
}
