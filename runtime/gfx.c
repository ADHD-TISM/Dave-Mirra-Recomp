/* The software backend: no graphics card involved, the platform stretches the CPU picture into the window (plat_soft_present).
   It is also what runs before host_init has chosen a backend, so gfx is never NULL. See gfx.h. */
#include <stddef.h>
#include "hw.h"
#include "host.h"
#include "gfx.h"

static int soft_init(int w, int h, const char *title) { (void)w; (void)h; (void)title; return 1; }
static void soft_present(const GfxFrame *f) { plat_soft_present(f->fb, f->fb_w, f->fb_h, f->cw, f->ch, f->w, f->h, f->filter); }
static const uint32_t *soft_grab(int *w, int *h) { (void)w; (void)h; return NULL; }
static int soft_r_init(int scale) { (void)scale; return 0; }
static void soft_r_submit(const GPrim *p) { (void)p; }
static void soft_r_mask(int set, int check) { (void)set; (void)check; }
static void soft_r_dirty(int y0, int y1) { (void)y0; (void)y1; }
static void soft_r_flush(void) { }
static void soft_r_read(int x, int y, int w, int h, unsigned *dst) { (void)x; (void)y; (void)w; (void)h; (void)dst; }

const GfxBackend gfx_soft_backend = {
    .name = "software", .hw = 0,
    .init = soft_init, .present = soft_present, .grab = soft_grab,
    .r_init = soft_r_init, .r_submit = soft_r_submit, .r_mask = soft_r_mask,
    .r_dirty = soft_r_dirty, .r_flush = soft_r_flush, .r_read = soft_r_read
};
const GfxBackend *gfx = &gfx_soft_backend;
