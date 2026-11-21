/* Checks runtime/gpu_gl.c against vram on the machine's own OpenGL driver (Windows; opens a small window for the context).
   Random vram is drawn through the GL path at 2x, then the picture is read back and compared with what the CPU's vram says:
   display boxes that wrap at the vram edges (glg_read), vram-to-vram copies that wrap or overlap, whether the copies the console
   does not smear keep the picture's hi-res detail, and the mask bit (set-mask and check-mask on rectangles and copies, with the
   stencil rebuilt from vram when checking starts). Every line should say ok.
   From the repository root:
     mkdir -p build && gcc -I runtime tools/gltest.c runtime/gpu_gl.c runtime/gl_stream.c -o build/gltest.exe -lopengl32 -lgdi32 && build/gltest.exe
   It prints the renderer, so it also shows which graphics card the context landed on. Not a test of gameplay, textured
   drawing or the window picture. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "host.h"
#include "gpu_gl.h"

/* the same request for the dedicated graphics chip that the game makes (runtime/host_common.c), so this shows what the game gets */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

u16 vram[512 * 1024];
void *plat_gl_proc(const char *name) { return (void *)wglGetProcAddress(name); }

#define S 2
static int fails;
static unsigned want(int x, int y) { u16 p = vram[(y & 511) * 1024 + (x & 1023)]; return ((p & 31) << 3) << 16 | (((p >> 5) & 31) << 3) << 8 | (((p >> 10) & 31) << 3); }

/* not inlined: clang 22.1.8 at -O2 vectorises this loop wrongly when it is inlined with constant arguments that wrap in x
   (pixels in the wrapped part come out wrong); gcc 16.2, which builds the game, and gpu.c's own loop are not affected */
__attribute__((noinline)) static void cpu_copy(int sx, int sy, int dx, int dy, int w, int h) {   /* what gpu.c does, a pixel at a time */
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++)
        vram[((dy + yy) & 511) * 1024 + ((dx + xx) & 1023)] = vram[((sy + yy) & 511) * 1024 + ((sx + xx) & 1023)];
}
static void gl_copy(int sx, int sy, int dx, int dy, int w, int h) {
    GPrim p; memset(&p, 0, sizeof p); p.kind = GK_COPY; p.x = dx; p.y = dy; p.w = w; p.h = h; p.sx = sx; p.sy = sy;
    glg_dirty(dy, dy + h - 1); glg_submit(&p);
}
static void upload_all(void) {   /* the picture becomes the vram contents, the way gpu_set_scale starts it */
    GPrim b; memset(&b, 0, sizeof b); b.kind = GK_BLIT; b.w = 1024; b.h = 512;
    glg_dirty(0, 511); glg_submit(&b); glg_flush();
}
static void check_box(const char *what, int x, int y, int w, int h) {
    static unsigned buf[1024 * S * 512 * S];
    memset(buf, 0xAA, sizeof(unsigned) * w * S * h * S);
    glg_flush();
    glg_read(x, y, w, h, buf);
    long bad = 0, first = -1;
    for (int j = 0; j < h * S; j++) for (int i = 0; i < w * S; i++) {
        unsigned got = buf[(size_t)j * w * S + i] & 0xffffff, exp = want(x + i / S, y + j / S);
        if (got != exp) { if (!bad) first = (long)j * w * S + i; bad++; }
    }
    printf("%-44s %s (%ld of %d pixels differ)\n", what, bad ? "FAIL" : "ok", bad, w * S * h * S);
    if (bad) { printf("   first bad pixel: x %ld y %ld of the picture\n", first % (w * S), first / (w * S)); fails++; }
}

/* ---- hi-res detail and the mask bit ---- */
static void cpu_copy_m(int sx, int sy, int dx, int dy, int w, int h, int set, int check) {   /* gpu.c's copy with the mask bit */
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        u16 s = vram[((sy + yy) & 511) * 1024 + ((sx + xx) & 1023)], *d = &vram[((dy + yy) & 511) * 1024 + ((dx + xx) & 1023)];
        if (check && (*d & 0x8000)) continue;
        *d = s | (set ? 0x8000 : 0);
    }
}
static unsigned col15(int r, int g, int b) { return (unsigned)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }
static void cpu_rect(int x, int y, int w, int h, int r, int g, int b, int set, int check) {
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        u16 *d = &vram[((y + yy) & 511) * 1024 + ((x + xx) & 1023)];
        if (check && (*d & 0x8000)) continue;
        *d = (u16)(col15(r, g, b) | (set ? 0x8000 : 0));
    }
}
static void gl_rect(int x, int y, int w, int h, int r, int g, int b) {
    GPrim p; memset(&p, 0, sizeof p); p.kind = GK_RECT; p.x = x; p.y = y; p.w = w; p.h = h; p.r = r; p.g = g; p.b = b;
    p.clip[2] = 1023; p.clip[3] = 511;
    glg_dirty(y, y + h - 1); glg_submit(&p);
}
static void gl_tri(int x0, int y0, int x1, int y1, int x2, int y2, int r, int g, int b) {
    GPrim p; memset(&p, 0, sizeof p); p.kind = GK_TRI; p.clip[2] = 1023; p.clip[3] = 511;
    int xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    for (int i = 0; i < 3; i++) { p.vx[i] = xs[i]; p.vy[i] = ys[i]; p.vr[i] = r; p.vg[i] = g; p.vb[i] = b; }
    glg_submit(&p);
}
static void clear_marks(void) { for (int i = 0; i < 512 * 1024; i++) vram[i] &= 0x7fff; glg_mask(0, 0); upload_all(); }
static void mark_region(int x, int y, int w, int h) {
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) vram[(y + yy) * 1024 + x + xx] |= 0x8000;
    glg_dirty(y, y + h - 1);
}

/* A copy the console does not smear is a copy of the picture, so a triangle drawn into the source (finer than vram's pixels at
   2x) must arrive at the destination. A copy redrawn from vram would lose it. */
static void detail_copy(const char *what, int sx, int sy, int dx, int dy, int w, int h) {
    static unsigned a[200 * S * 200 * S], b[200 * S * 200 * S];
    upload_all();
    gl_tri(sx + 10, sy + 5, sx + w - 10, sy + 5, sx + 10, sy + h - 5, 255, 0, 255);
    glg_flush();
    glg_read(sx, sy, w, h, a);
    long fine = 0, bad = 0;
    for (int j = 0; j + 1 < h * S; j += 2) for (int i = 0; i + 1 < w * S; i += 2) {
        unsigned p = a[(size_t)j * w * S + i];
        if (a[(size_t)j * w * S + i + 1] != p || a[(size_t)(j + 1) * w * S + i] != p || a[(size_t)(j + 1) * w * S + i + 1] != p) fine++;
    }
    gl_copy(sx, sy, dx, dy, w, h);
    glg_flush();
    glg_read(dx, dy, w, h, b);
    for (size_t i = 0; i < (size_t)w * S * h * S; i++) if (a[i] != b[i]) bad++;
    printf("%-44s %s (%ld of %d pixels differ; %ld finer-than-vram blocks drawn)\n", what, bad || !fine ? "FAIL" : "ok", bad, w * S * h * S, fine);
    if (bad || !fine) fails++;
}

/* a textured rectangle under check-mask: texels with bit 15 stamp the stencil, texels that are 0 are not drawn, and a flat rectangle drawn
   after it must leave the stamped pixels alone (the stamp pass for textured triangles and rectangles had no case) */
static void check_mask_textured(const char *what) {
    clear_marks();
    mark_region(620, 120, 20, 20);     /* texels with bit 15, inside the 64x64 texture at (600,100) */
    vram[130 * 1024 + 610] = 0; vram[131 * 1024 + 611] = 0;   /* and two texels that are 0: transparent */
    glg_mask(0, 1);                        /* checking starts: the stencil is rebuilt from vram (the marked texels are masked too, but not where we draw) */
    GPrim p; memset(&p, 0, sizeof p); p.kind = GK_RECT; p.textured = 1; p.raw = 1; p.x = 300; p.y = 200; p.w = 64; p.h = 64;
    p.tx = 600; p.ty = 100; p.depth = 2; p.clip[2] = 1023; p.clip[3] = 511;
    glg_dirty(200, 263); glg_submit(&p);
    gl_rect(280, 180, 120, 120, 250, 40, 40);
    /* the model: the picture before, the texture on top, then the flat rectangle where the stencil is not set */
    static unsigned canvas[120 * 120]; static char stencil[120 * 120];
    for (int j = 0; j < 120; j++) for (int i = 0; i < 120; i++) canvas[j * 120 + i] = want(280 + i, 180 + j);
    for (int j = 0; j < 64; j++) for (int i = 0; i < 64; i++) {
        unsigned w = vram[(100 + j) * 1024 + 600 + i];
        if (!w) continue;
        int cx = 300 + i - 280, cy = 200 + j - 180;
        canvas[cy * 120 + cx] = ((w & 31) << 3) << 16 | (((w >> 5) & 31) << 3) << 8 | (((w >> 10) & 31) << 3);
        if (w & 0x8000) stencil[cy * 120 + cx] = 1;
    }
    for (int k = 0; k < 120 * 120; k++) if (!stencil[k]) canvas[k] = ((250 >> 3) << 3) << 16 | ((40 >> 3) << 3) << 8 | ((40 >> 3) << 3);
    static unsigned buf2[120 * S * 120 * S];
    memset(buf2, 0xAA, sizeof buf2);
    glg_flush(); glg_read(280, 180, 120, 120, buf2);
    long bad = 0;
    for (int j = 0; j < 120 * S; j++) for (int i = 0; i < 120 * S; i++) if ((buf2[j * 120 * S + i] & 0xffffff) != canvas[(j / S) * 120 + i / S]) bad++;
    printf("%-44s %s (%ld of %d pixels differ)\n", what, bad ? "FAIL" : "ok", bad, 120 * S * 120 * S);
    if (bad) fails++;
    glg_mask(0, 0);
}

int main(void) {
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "t"; RegisterClassA(&wc);
    HWND w = CreateWindowA("t", "t", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(w); PIXELFORMATDESCRIPTOR pf = {0}; pf.nSize = sizeof pf; pf.nVersion = 1; pf.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pf.iPixelType = PFD_TYPE_RGBA; pf.cColorBits = 32; pf.cDepthBits = 24; pf.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pf), &pf);
    HGLRC legacy = wglCreateContext(dc);   /* the game's context: OpenGL 3.3 core, made through a plain one (see plat_win32.c) */
    if (!legacy || !wglMakeCurrent(dc, legacy)) { printf("no OpenGL context\n"); return 2; }
    typedef HGLRC (WINAPI *ctx_fn)(HDC, HGLRC, const int *);
    ctx_fn make_ctx = (ctx_fn)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    static const int attribs[] = { 0x2091, 3, 0x2092, 3, 0x9126, 1, 0 };
    HGLRC core = make_ctx ? make_ctx(dc, NULL, attribs) : NULL;
    if (!core) { printf("no OpenGL 3.3 core context\n"); return 2; }
    wglMakeCurrent(NULL, NULL); wglDeleteContext(legacy);
    if (!wglMakeCurrent(dc, core)) { printf("cannot use the core context\n"); return 2; }
    printf("renderer: %s, OpenGL %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    srand(7);
    for (int i = 0; i < 512 * 1024; i++) vram[i] = (u16)(rand() & 0x7fff);   /* no mask bits: texels are plain colours */
    if (!glg_init(S)) { printf("glg_init failed\n"); return 2; }
    upload_all();
    check_box("plain box (100,50) 320x240", 100, 50, 320, 240);
    check_box("box wraps x: (900,50) 320x240", 900, 50, 320, 240);
    check_box("box wraps y: (100,400) 320x240", 100, 400, 320, 240);
    check_box("box wraps both: (900,400) 320x240", 900, 400, 320, 240);
    check_box("640 wide at x 512 (wraps)", 512, 0, 640, 240);
    /* copies */
#define COPY(name, sx, sy, dx, dy, w, h, bx, by, bw, bh) do { upload_all(); cpu_copy(sx, sy, dx, dy, w, h); gl_copy(sx, sy, dx, dy, w, h); check_box(name, bx, by, bw, bh); } while (0)
    /* each case starts from a freshly uploaded picture, so one failure does not spoil the next */
    COPY("copy, apart", 10, 10, 300, 200, 100, 50, 280, 190, 140, 70);
    COPY("copy, source wraps x", 980, 10, 20, 100, 100, 50, 0, 90, 140, 70);
    COPY("copy, source wraps y", 100, 480, 500, 20, 64, 64, 480, 10, 120, 100);
    COPY("copy, dest wraps both", 200, 100, 1000, 450, 100, 100, 990, 440, 200, 150);
    COPY("copy, source wraps both", 980, 500, 30, 20, 100, 100, 20, 10, 140, 130);
    /* overlapping copies: the first four smear on the console, the last four do not (and are drawn at full detail) */
    COPY("overlap, destination down-right (smear)", 400, 300, 420, 310, 100, 100, 390, 290, 140, 130);
    COPY("overlap, destination down-left (smear)", 620, 300, 600, 310, 100, 100, 590, 290, 140, 130);
    COPY("overlap, same row, destination right (smear)", 400, 300, 420, 300, 100, 100, 390, 290, 140, 120);
    COPY("overlap, directly below (smear)", 400, 300, 400, 310, 100, 100, 390, 290, 120, 130);
    COPY("overlap, destination up-left", 420, 310, 400, 300, 100, 100, 390, 290, 140, 130);
    COPY("overlap, destination up-right", 400, 310, 420, 300, 100, 100, 390, 290, 140, 130);
    COPY("overlap, same row, destination left", 420, 300, 400, 300, 100, 100, 390, 290, 140, 120);
    COPY("overlap, directly above", 400, 310, 400, 300, 100, 100, 390, 290, 120, 130);
    COPY("overlap and wrap, destination down-right", 1000, 100, 1010, 110, 100, 60, 990, 90, 140, 100);
    COPY("overlap and wrap, destination up-left", 1010, 110, 1000, 100, 100, 60, 990, 90, 140, 100);
    /* the copies the console does not smear keep the picture's hi-res detail */
    detail_copy("detail, destination up-left", 420, 310, 400, 300, 100, 100);
    detail_copy("detail, destination up-right", 400, 310, 420, 300, 100, 100);
    detail_copy("detail, same row, destination left", 420, 300, 400, 300, 100, 100);
    detail_copy("detail, directly above", 400, 310, 400, 300, 100, 100);
    detail_copy("detail, apart", 100, 100, 300, 200, 100, 100);
    /* the mask bit: each case starts with no mask bits in vram and the picture freshly uploaded */
    clear_marks();
    mark_region(300, 200, 100, 100);   /* pixels the console has already masked */
    glg_mask(0, 1);                    /* checking starts: the stencil is rebuilt from vram */
    cpu_rect(250, 150, 200, 200, 200, 100, 50, 0, 1); gl_rect(250, 150, 200, 200, 200, 100, 50);
    check_box("check-mask: masked pixels are kept", 240, 140, 220, 220);
    clear_marks();
    glg_mask(1, 1);                    /* set and check: the first rectangle masks what it draws, the second must skip it */
    cpu_rect(260, 160, 120, 120, 255, 0, 0, 1, 1); gl_rect(260, 160, 120, 120, 255, 0, 0);
    cpu_rect(320, 220, 120, 120, 0, 255, 0, 1, 1); gl_rect(320, 220, 120, 120, 0, 255, 0);
    check_box("set-mask + check-mask: second skips first", 250, 150, 200, 200);
    clear_marks();
    mark_region(300, 200, 100, 100);
    glg_mask(0, 1);
    cpu_copy_m(100, 100, 280, 180, 140, 140, 0, 1); gl_copy(100, 100, 280, 180, 140, 140);
    check_box("check-mask: a copy skips masked pixels", 270, 170, 160, 160);
    clear_marks();
    glg_mask(1, 0);                    /* a copy that sets the mask bit with checking off ... */
    cpu_copy_m(100, 100, 400, 300, 60, 60, 1, 0); gl_copy(100, 100, 400, 300, 60, 60);
    glg_mask(0, 1);                    /* ... is seen when checking starts (the stencil is rebuilt from vram) */
    cpu_rect(380, 280, 100, 100, 10, 200, 240, 0, 1); gl_rect(380, 280, 100, 100, 10, 200, 240);
    check_box("mask set by a copy, then checked", 370, 270, 120, 120);
    clear_marks();
    mark_region(100, 100, 60, 30);     /* the top of the source is masked */
    glg_mask(0, 1);                    /* a copy takes the mask bit along with the pixel ... */
    cpu_copy_m(100, 100, 400, 300, 60, 60, 0, 1); gl_copy(100, 100, 400, 300, 60, 60);
    cpu_rect(380, 280, 100, 100, 10, 200, 240, 0, 1); gl_rect(380, 280, 100, 100, 10, 200, 240);   /* ... so this skips the top of it */
    check_box("a copy carries the mask bit along", 370, 270, 120, 120);
    glg_mask(0, 0);
    glg_flush();
    check_box("whole vram", 0, 0, 1024, 512);
    check_mask_textured("check-mask: textured rect stamps the stencil");
    printf(fails ? "FAILURES: %d\n" : "all ok\n", fails);
    return fails != 0;
}
