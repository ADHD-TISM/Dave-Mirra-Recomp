/* Checks runtime/gpu_gl.c against vram on the machine's own OpenGL driver (Windows; opens a small window for the context).
   Random vram is drawn through the GL path at 2x, then the picture is read back and compared with what the CPU's vram says:
   display boxes that wrap at the vram edges (glg_read) and vram-to-vram copies that wrap or overlap. Every line should say ok.
   From the repository root:
     mkdir -p build && gcc -I runtime tools/gltest.c runtime/gpu_gl.c -o build/gltest.exe -lopengl32 -lgdi32 && build/gltest.exe
   It prints the renderer, so it also shows which graphics card the context landed on. Not a test of gameplay, check-mask or
   the window picture. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "host.h"
#include "gpu_gl.h"

u16 vram[512 * 1024];
void *plat_gl_proc(const char *name) { return (void *)wglGetProcAddress(name); }

#define S 2
static int fails;
static unsigned want(int x, int y) { u16 p = vram[(y & 511) * 1024 + (x & 1023)]; return ((p & 31) << 3) << 16 | (((p >> 5) & 31) << 3) << 8 | (((p >> 10) & 31) << 3); }

static void cpu_copy(int sx, int sy, int dx, int dy, int w, int h) {   /* what gpu.c does, a pixel at a time */
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

int main(void) {
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "t"; RegisterClassA(&wc);
    HWND w = CreateWindowA("t", "t", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(w); PIXELFORMATDESCRIPTOR pf = {0}; pf.nSize = sizeof pf; pf.nVersion = 1; pf.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pf.iPixelType = PFD_TYPE_RGBA; pf.cColorBits = 32; pf.cDepthBits = 24; pf.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pf), &pf);
    wglMakeCurrent(dc, wglCreateContext(dc));
    printf("renderer: %s\n", glGetString(GL_RENDERER));
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
    cpu_copy(10, 10, 300, 200, 100, 50); gl_copy(10, 10, 300, 200, 100, 50); check_box("copy, apart", 280, 190, 140, 70);
    cpu_copy(980, 10, 20, 100, 100, 50); gl_copy(980, 10, 20, 100, 100, 50); check_box("copy, source wraps x", 0, 90, 140, 70);
    cpu_copy(100, 480, 500, 20, 64, 64); gl_copy(100, 480, 500, 20, 64, 64); check_box("copy, source wraps y", 480, 10, 120, 100);
    cpu_copy(200, 100, 1000, 450, 100, 100); gl_copy(200, 100, 1000, 450, 100, 100); check_box("copy, dest wraps both", 990, 440, 200, 150);
    cpu_copy(980, 500, 30, 20, 100, 100); gl_copy(980, 500, 30, 20, 100, 100); check_box("copy, source wraps both", 20, 10, 140, 130);
    cpu_copy(400, 300, 420, 310, 100, 100); gl_copy(400, 300, 420, 310, 100, 100); check_box("copy, overlapping (smear)", 390, 290, 140, 130);
    cpu_copy(420, 310, 400, 300, 100, 100); gl_copy(420, 310, 400, 300, 100, 100); check_box("copy, overlapping the other way", 390, 290, 140, 130);
    cpu_copy(1000, 100, 1010, 110, 100, 60); gl_copy(1000, 100, 1010, 110, 100, 60); check_box("copy, overlapping and wrapping", 990, 90, 140, 100);
    glg_flush();
    check_box("whole vram", 0, 0, 1024, 512);
    printf(fails ? "FAILURES: %d\n" : "all ok\n", fails);
    return fails != 0;
}
