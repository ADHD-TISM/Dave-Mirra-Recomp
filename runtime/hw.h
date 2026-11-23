/* Internal interfaces between runtime subsystems. */
#pragma once
#include "psx.h"
#include <stdio.h>
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp   /* the runtime uses the Windows name */
#endif

#define CLOCK_HZ 33868800u
#define CYCLES_PER_FRAME (CLOCK_HZ / 60u)     /* NTSC */
#define SCANLINES 263u
#define CYCLES_PER_LINE (CYCLES_PER_FRAME / SCANLINES)

enum { IRQ_VBLANK = 0, IRQ_GPU, IRQ_CDROM, IRQ_DMA, IRQ_TMR0, IRQ_TMR1, IRQ_TMR2,
       IRQ_SIO0, IRQ_SIO, IRQ_SPU, IRQ_PIO };

extern u32 i_stat, i_mask;
void irq_raise(int n);
void sched_kick(void);              /* force psx_sched on next CYC */

/* scheduled events */
enum { EV_VBLANK, EV_CD, EV_TMR0, EV_TMR1, EV_TMR2, EV_SIO, EV_COUNT };
void event_at(int ev, u64 when);
void event_cancel(int ev);

/* modules */
void gpu_init(void);
void gpu_write_gp0(u32 v);
void gpu_write_gp1(u32 v);
u32 gpu_read(void);
u32 gpu_stat(void);
void gpu_vblank(void);
extern u16 vram[512 * 1024];
typedef struct { int x, y, w, h, depth24, enabled; } DisplayArea;
void gpu_display_area(DisplayArea *d);
extern int g_gpu_scale;            /* requested internal resolution: 1, 2 or 4 */
extern int g_gpu_gl;               /* 1 = let the graphics card draw the 2x / 4x picture (gpu_gl setting; early prototype) */
int gpu_gl_scale(void);            /* the scale when the graphics card holds the picture (all queued drawing is done), else 0 */
void gpu_set_scale(void);           /* apply g_gpu_scale (safe at any time) */
const u16 *gpu_hires(int *scale);   /* high-resolution copy of VRAM, NULL at scale 1 */

void cd_init(const char *bin_path);
u8 cd_read_reg(u32 r);
void cd_write_reg(u32 r, u8 v);
void cd_event(void);
u32 cd_dma_read(u8 *dst, u32 bytes);
int cd_read_sector_raw(u32 lba, u8 *out2352);

void spu_init(void);
u16 spu_read(u32 a);
void spu_write(u32 a, u16 v);
void spu_dma_write(const u8 *src, u32 bytes);
void spu_dma_read(u8 *dst, u32 bytes);
void spu_render(s16 *out, int frames);   /* stereo, 44100 Hz */
void spu_cd_audio(const s16 *lr, int frames);

void mdec_write(u32 a, u32 v);
u32 mdec_read(u32 a);
void mdec_dma_in(u32 madr, u32 words);
void mdec_dma_out(u32 madr, u32 words);

void timers_init(void);
u32 timer_read(u32 a);
void timer_write(u32 a, u32 v);
void timer_event(int n);

void dma_write(u32 a, u32 v);
u32 dma_read(u32 a);

void sio_write(u32 a, u32 v, int size);
u32 sio_read(u32 a, int size);
void sio_event(void);

/* HLE BIOS */
void bios_init(void);
void bios_call_a(void);
void bios_call_b(void);
void bios_call_c(void);
void bios_syscall(void);
void bios_vblank(void);
void bios_report_stall(const char *tag, int all);   /* watchdog and host_quit: recent BIOS calls and pending card events */
int  bios_take_interrupt(void);     /* returns 1 if handled */
extern int bios_in_exception;

/* host */
void host_init(void);
void host_frame(void);              /* present + pump + pace */
u16  host_pad_buttons(int port);    /* active-low PS1 button word */
int host_pad_connected(int port);
void host_pad_read(int port, u16 *buttons, u8 axes[4]);   /* axes: right X, right Y, left X, left Y; 0x80 = centre */
void host_pad_rumble(int port, int small_on, int large);
void host_fatal(const char *fmt, ...);
void host_audio_push(const s16 *lr, int frames);
int  gte_sxy_seen(u32 xy);          /* widescreen: this screen position came out of the GTE since the last 3D drawing list */
void gte_sxy_clear(void);

/* module dispatch */
psx_fn psx_lookup(u32 addr);
void exe_load_main(void);
extern const char *g_disc_dir;
extern int g_log_bios;
