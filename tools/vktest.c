/* Checks runtime/gpu_vk.c (the GPU hi-res renderer on Vulkan) against vram on the machine's own Vulkan driver (Windows; it opens a
   window, makes the Vulkan backend of runtime/gfx_vk.c and asks it for the renderer). Step 2 of docs/vulkan-gpu-renderer-plan.md:
   random vram is drawn into the picture by a blit, then read back and compared with what the CPU's vram says, for display boxes that
   wrap at the vram edges, for rows the CPU changed afterwards (the dirty-row upload), at scales 2 and 4; and the presenter's copy of
   a box out of the picture into the window is compared with the window's own pixels. The later steps add flat primitives, textured
   drawing, copies and the mask bit. Every line should say ok.
   From the repository root:
     mkdir -p build && gcc -I runtime tools/vktest.c runtime/gfx_vk.c runtime/gpu_vk.c runtime/gfx_vk_spv.c runtime/plat_win32.c \
       -o build/vktest.exe -lgdi32 -lwinmm -lopengl32 && build/vktest.exe
   DMRECOMP_VK_VALIDATE=1 runs it under the validation layer if that is installed. */
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

static void blit(int x, int y, int w, int h) {   /* what gpu.c does for an upload: the rows are marked changed, then the box is copied from vram */
    GPrim b; memset(&b, 0, sizeof b); b.kind = GK_BLIT; b.x = x; b.y = y; b.w = w; b.h = h;
    be->r_dirty(y, y + h - 1); be->r_submit(&b); be->r_flush();
}
static void upload_all(void) { be->r_dirty(0, 511); blit(0, 0, 1024, 512); }
static void check_box(const char *what, int x, int y, int w, int h) {
    static unsigned buf[1024 * 4 * 512 * 4];
    memset(buf, 0xAA, sizeof(unsigned) * (size_t)w * S * h * S);
    be->r_flush();
    be->r_read(x, y, w, h, buf);
    long bad = 0, first = -1;
    for (int j = 0; j < h * S; j++) for (int i = 0; i < w * S; i++) {
        unsigned got = buf[(size_t)j * w * S + i] & 0xffffff, exp = want(x + i / S, y + j / S);
        if (got != exp) { if (!bad) first = (long)j * w * S + i; bad++; }
    }
    printf("%-46s %s (%ld of %d pixels differ)\n", what, bad ? "FAIL" : "ok", bad, w * S * h * S);
    if (bad) { printf("   first bad pixel: x %ld y %ld of the picture\n", first % (w * S), first / (w * S)); fails++; }
}
static void randomise(int y0, int rows, unsigned seed) {   /* the CPU rewrites some rows of vram */
    srand(seed);
    for (int i = y0 * 1024; i < (y0 + rows) * 1024; i++) vram[i] = (u16)(rand() & 0x7fff);
}

/* the presenter: a box of the picture, copied into the window's picture and read back from the window */
static void check_present(const char *what, int bx, int by, int bw, int bh) {
    int cw, ch; plat_client_size(&cw, &ch);
    GfxFrame f; memset(&f, 0, sizeof f);
    f.fb_w = bw * S; f.fb_h = bh * S; f.on_gpu = 1; f.box[0] = bx; f.box[1] = by; f.box[2] = bw; f.box[3] = bh;
    f.cw = cw; f.ch = ch; f.w = f.fb_w; f.h = f.fb_h; f.grab = 1;
    if (f.w > cw || f.h > ch) { printf("%-46s skipped (the window is %dx%d, the picture %dx%d)\n", what, cw, ch, f.w, f.h); return; }
    be->r_flush();
    be->present(&f);
    int gw = 0, gh = 0; const uint32_t *g = be->grab(&gw, &gh);
    if (!g || gw != cw || gh != ch) { printf("%-46s FAIL (no window grab, or its size is wrong)\n", what); fails++; return; }
    int x0 = (cw - f.w) / 2, y0 = ch - f.h - (ch - f.h) / 2;   /* placed like the OpenGL presenter: the offset counted from the bottom */
    long bad = 0;
    for (int j = 0; j < f.h; j++) for (int i = 0; i < f.w; i++)
        if ((g[(size_t)(y0 + j) * gw + x0 + i] & 0xffffff) != want(bx + i / S, by + j / S)) bad++;
    for (int j = 0; j < ch; j++) for (int i = 0; i < cw; i++) {   /* and the bars around it are black */
        if (i >= x0 && i < x0 + f.w && j >= y0 && j < y0 + f.h) continue;
        if (g[(size_t)j * gw + i] & 0xffffff) bad++;
    }
    printf("%-46s %s (%ld of %d window pixels differ)\n", what, bad ? "FAIL" : "ok", bad, cw * ch);
    if (bad) fails++;
}

int main(void) {
    _putenv("DMRECOMP_VK_RENDERER=1");   /* the renderer is off for the game until the plan's last step */
    if (!be->init(960, 720, "vktest")) { printf("the Vulkan backend cannot start\n"); return 2; }
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
    /* the presenter */
    upload_all();   /* the rows rewritten above are in the picture only where they were blitted */
    check_present("window picture, box (100,50) 320x240", 100, 50, 320, 240);
    check_present("window picture, box wraps (900,400)", 900, 400, 320, 240);
    /* scale 4: the picture is made again at four times the size */
    S = 4;
    if (!be->r_init(S)) { printf("the renderer cannot make the 4x picture\n"); return 2; }
    upload_all();
    check_box("4x: plain box (100,50) 320x240", 100, 50, 320, 240);
    check_box("4x: box wraps both (900,400) 160x120", 900, 400, 160, 120);
    printf(fails ? "FAILURES: %d\n" : "all ok\n", fails);
    return fails != 0;
}
