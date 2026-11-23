/* Memory map slow path, interrupt controller, DMA, timers, SIO0 (pads). */
#include "hw.h"

u8 psx_ram[0x200000];
u8 psx_scratch[0x400];
static u32 memctrl[16], ram_size;

static inline u32 ram32(u32 a) { u32 v; memcpy(&v, psx_ram + (a & 0x1ffffcu), 4); return v; }
static inline void wram32(u32 a, u32 v) { memcpy(psx_ram + (a & 0x1ffffcu), &v, 4); }

/* ---------------------------------------------------------------- DMA --- */
static u32 madr[7], bcr[7], chcr[7], dpcr = 0x07654321, dicr;

static void dicr_update(void) {
    u32 flags = (dicr >> 24) & 0x7f, en = (dicr >> 16) & 0x7f;
    int irq = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & en));
    int was = dicr >> 31;
    dicr = (dicr & 0x7fffffffu) | ((u32)irq << 31);
    if (irq && !was) irq_raise(IRQ_DMA);
}

void dma_complete(int ch) {
    chcr[ch] &= ~0x11000000u;
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dicr_update();
}

static u32 dma_words(int ch) {
    u32 mode = (chcr[ch] >> 9) & 3;
    if (mode == 0) { u32 n = bcr[ch] & 0xffff; return n ? n : 0x10000; }
    return (bcr[ch] & 0xffff) * (bcr[ch] >> 16);
}

int mdec_dma_ready(int ch);  /* mdec.c: returns 0 to keep the channel busy */

#include "game.h"   /* GAME_HUD_SLOT */
extern int g_ws_kq, gpu_2d;

static void dma_run(int ch) {
    u32 a = madr[ch] & 0x1ffffcu, n, mode = (chcr[ch] >> 9) & 3, dir = chcr[ch] & 1;
    switch (ch) {
    case 0: mdec_dma_in(a, dma_words(ch)); break;
    case 1:
        if (!mdec_dma_ready(1)) return;
        mdec_dma_out(a, dma_words(ch));
        break;
    case 2:
        if (mode == 2) {
            /* Widescreen: the game draws a 3D scene from one ordering table, back to front; an empty node is one of its
               slots. Both games put the 3D polygons in the slots up to 1524 and everything flat (the HUD, the pause and
               finish texts) in the slots from 1534 on, so counting the slots passed tells the two apart: gpu_2d is set
               for the flat part, and gpu.c squeezes it like the 3D picture (GAME_HUD_SLOT, from games/<id>.json). */
            int slots = 0;
            for (int guard = 0; guard < 0x100000; guard++) {
                u32 h = ram32(a);
#ifdef GAME_HUD_SLOT
                if (!(h >> 24)) slots++;
                gpu_2d = g_ws_kq != 65536 && slots >= GAME_HUD_SLOT;
#endif
                for (u32 i = 0, c = h >> 24; i < c; i++) gpu_write_gp0(ram32(a + 4 + 4 * i));
                if (h & 0x800000) break;
                a = h & 0x1ffffcu;
            }
            gpu_2d = 0;
#ifdef GAME_HUD_SLOT
            if (slots >= GAME_HUD_SLOT) gte_sxy_clear();   /* that was the 3D list: the next frame's projections start afresh */
#endif
            (void)slots;
        } else {
            n = dma_words(ch);
            if (dir) for (u32 i = 0; i < n; i++) gpu_write_gp0(ram32(a + 4 * i));
            else for (u32 i = 0; i < n; i++) wram32(a + 4 * i, gpu_read());
        }
        break;
    case 3:
        n = dma_words(ch);
        cd_dma_read(psx_ram + a, n * 4 > 0x200000 - a ? 0x200000 - a : n * 4);
        break;
    case 4:
        n = dma_words(ch);
        if (n * 4 > 0x200000 - a) n = (0x200000 - a) / 4;
        if (dir) spu_dma_write(psx_ram + a, n * 4); else spu_dma_read(psx_ram + a, n * 4);
        break;
    case 6:
        n = bcr[ch] & 0xffff; if (!n) n = 0x10000;
        for (u32 i = 0; i < n; i++, a -= 4) wram32(a, i == n - 1 ? 0x00ffffffu : ((a - 4) & 0x1fffffu));
        break;
    default: break;
    }
    dma_complete(ch);
}

void dma_write(u32 r, u32 v) {
    if (r == 0x70) { dpcr = v; return; }
    if (r == 0x74) {
        dicr = (dicr & 0x7f000000u & ~(v & 0x7f000000u)) | (v & 0x00ff803fu);
        dicr_update();
        return;
    }
    int ch = r >> 4; if (ch > 6) return;
    switch (r & 0xf) {
    case 0: madr[ch] = v & 0xffffff; break;
    case 4: bcr[ch] = v; break;
    case 8:
        chcr[ch] = v;
        if (v & 0x01000000u) {
            u32 mode = (v >> 9) & 3;
            if (mode == 0 && ch != 6 && ch != 3 && !(v & 0x10000000u) && ch != 2 && ch != 4) break;
            dma_run(ch);
        }
        break;
    }
}
u32 dma_read(u32 r) {
    if (r == 0x70) return dpcr;
    if (r == 0x74) return dicr;
    int ch = r >> 4; if (ch > 6) return 0;
    switch (r & 0xf) { case 0: return madr[ch]; case 4: return bcr[ch]; case 8: return chcr[ch]; }
    return 0;
}
/* lets mdec.c finish a deferred output transfer */
void dma_mdec_out_kick(void) {
    if ((chcr[1] & 0x01000000u) && mdec_dma_ready(1)) {
        mdec_dma_out(madr[1] & 0x1ffffcu, dma_words(1));
        dma_complete(1);
    }
}

/* ------------------------------------------------------------- timers --- */
typedef struct { u32 mode, target; u64 base; u32 start; } Timer;
static Timer tm[3];
extern int g_log_sio;

static u32 tdiv(int n) {
    u32 src = (tm[n].mode >> 8) & 3;
    if (n == 0) return (src & 1) ? 5 : 1;              /* dotclock approx */
    if (n == 1) return (src & 1) ? CYCLES_PER_LINE : 1; /* hblank */
    return (src & 2) ? 8 : 1;
}
static u64 tticks(int n) { return (cpu.cycles - tm[n].base) / tdiv(n); }
static u32 tperiod(int n) {
    if (tm[n].mode & 8) return (tm[n].target & 0xffff) + 1;
    return 0x10000;
}
static u32 tcount(int n) { return (u32)((tm[n].start + tticks(n)) % tperiod(n)); }

static void tschedule(int n) {
    int ev = EV_TMR0 + n;
    u32 m = tm[n].mode;
    if (!(m & 0x30)) { event_cancel(ev); return; }
    u32 c = tcount(n), tgt = tm[n].target & 0xffff, d;
    if (m & 0x10) d = tgt > c ? tgt - c : tperiod(n) - c + tgt;
    else d = 0x10000 - c;
    if (!d) d = tperiod(n);
    event_at(ev, cpu.cycles + (u64)d * tdiv(n));
}
void timer_event(int n) {
    Timer *t = &tm[n];
    t->mode |= (t->mode & 0x10) ? 0x800 : 0x1000;
    if (t->mode & 0x80) {            /* toggle mode: IRQ on falling edge of bit10 */
        t->mode ^= 0x400;
        if (!(t->mode & 0x400)) irq_raise(IRQ_TMR0 + n);
    } else irq_raise(IRQ_TMR0 + n);  /* pulse mode: bit10 stays 1 afterwards */
    if (t->mode & 0x40) tschedule(n);
    else event_cancel(EV_TMR0 + n);
}
void timers_init(void) { for (int i = 0; i < 3; i++) tm[i].mode = 0x400; }
u32 timer_read(u32 r) {
    int n = (r >> 4) & 3; if (n > 2) return 0;
    switch (r & 0xf) {
    case 0: return tcount(n);
    case 4: { u32 m = tm[n].mode; tm[n].mode &= ~0x1800u; return m; }
    case 8: return tm[n].target;
    }
    return 0;
}
void timer_write(u32 r, u32 v) {
    int n = (r >> 4) & 3; if (n > 2) return;
    switch (r & 0xf) {
    case 0: tm[n].start = v & 0xffff; tm[n].base = cpu.cycles; break;
    case 4: tm[n].mode = (v & 0x3ff) | 0x400; tm[n].start = 0; tm[n].base = cpu.cycles; break;
    case 8: tm[n].target = v & 0xffff; break;
    }
    tschedule(n);
}

/* ------------------------------------------------------- SIO0 (pads) --- */
static u32 sio_ctrl, sio_mode, sio_baud;
static u8 sio_rx; static int sio_rx_full, sio_irq;

/* One controller per port. With g_dualshock the pad answers the DualShock
   configuration commands (which is how games find out they may use the sticks
   and the motors); otherwise it is a plain digital pad. */
int g_dualshock = 1;
int g_pad_analog[2];              /* analog mode is on: the host stops turning the stick into d-pad presses */
typedef struct {
    int pos, cmd, len, config, analog;
    u8 map[6], small, large, resp[8];
} Pad;
static Pad pads[2] = { { .map = { 255, 255, 255, 255, 255, 255 } }, { .map = { 255, 255, 255, 255, 255, 255 } } };

static void pad_fill(u8 *d, int k) { static const u8 t[5][6] = {
    { 0x01, 0x02, 0x00, 0x02, 0x01, 0x00 },   /* 45 status (byte 2 = analog LED) */
    { 0x00, 0x00, 0x01, 0x02, 0x00, 0x0a },   /* 46 index 0 */
    { 0x00, 0x00, 0x01, 0x01, 0x01, 0x14 },   /* 46 index 1 */
    { 0x00, 0x00, 0x02, 0x00, 0x01, 0x00 },   /* 47 */
    { 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 } }; /* 4c index 0 (index 1: byte 3 = 7) */
    memcpy(d, t[k], 6); }

/* exchange one byte with the pad on the selected port; *more = it expects another byte */
static u8 sio_exchange(u8 tx, int *more) {
    int port = (sio_ctrl >> 13) & 1;
    Pad *p = &pads[port];
    *more = 0;
    if (p->pos == 0) {
        if (tx != 0x01 || !host_pad_connected(port)) return 0xff;   /* memory cards go through the BIOS instead */
        p->pos = 1; *more = 1; return 0xff;
    }
    if (p->pos == 1) {
        u8 id = p->config ? 0xf3 : p->analog ? 0x73 : 0x41;
        int n = (p->config || p->analog) ? 6 : 2;
        p->cmd = tx;
        if (tx != 0x42 && !(g_dualshock && (tx == 0x43 || p->config))) { p->pos = 0; return 0xff; }
        memset(p->resp, 0, sizeof p->resp);
        p->resp[0] = 0x5a;
        if (tx == 0x42 || (tx == 0x43 && !p->config)) {
            u16 b; u8 ax[4]; host_pad_read(port, &b, ax);
            p->resp[1] = (u8)b; p->resp[2] = (u8)(b >> 8);
            memcpy(p->resp + 3, ax, 4);
        } else {
            n = 6;
            if (tx == 0x45) { pad_fill(p->resp + 1, 0); p->resp[3] = (u8)p->analog; }
            else if (tx == 0x47) pad_fill(p->resp + 1, 3);
            else if (tx == 0x4d) memcpy(p->resp + 1, p->map, 6);
        }
        p->len = 1 + n; p->pos = 2; *more = 1;
        return id;
    }
    int idx = p->pos - 2, d = idx - 1;          /* d: index of this byte among the data bytes */
    if (d >= 0 && d < 6) switch (p->cmd) {
    case 0x42:
        if (p->map[d] == 0) p->small = tx & 1; else if (p->map[d] == 1) p->large = tx;
        break;
    case 0x43: if (d == 0) p->config = tx == 1; break;
    case 0x44: if (d == 0 && p->config) { p->analog = tx == 1; g_pad_analog[port] = p->analog; } break;
    case 0x46: if (d == 0 && p->config) pad_fill(p->resp + 1, tx ? 2 : 1); break;
    case 0x4c: if (d == 0 && p->config) { pad_fill(p->resp + 1, 4); if (tx) p->resp[4] = 7; } break;
    case 0x4d: if (p->config) p->map[d] = tx; break;
    }
    u8 r = p->resp[idx];
    if (idx + 1 >= p->len) {
        p->pos = 0;
        if (p->cmd == 0x42) host_pad_rumble(port, p->small, p->large);
    } else { p->pos++; *more = 1; }
    return r;
}
extern int g_log_sio;
void sio_event(void) {
    if (g_log_sio) fprintf(stderr, "[sio] ack event ctrl=%x cyc=%llu\n", sio_ctrl, (unsigned long long)cpu.cycles);
    if (sio_ctrl & 0x1000) { sio_irq = 1; irq_raise(IRQ_SIO0); }
}
int g_log_sio;
void sio_write(u32 r, u32 v, int size) {
    if (g_log_sio) fprintf(stderr, "[sio] w%02x.%d=%x ra=%08x istat=%x\n", r, size, v, cpu.r[31], i_stat);
    switch (r) {
    case 0x40: {
        int ack = 0;
        if (sio_ctrl & 0x2) sio_rx = sio_exchange((u8)v, &ack);   /* /JOY asserted */
        else sio_rx = 0xff;
        sio_rx_full = 1;
        if (ack) event_at(EV_SIO, cpu.cycles + 1088);
        break;
    }
    case 0x48: sio_mode = v; break;
    case 0x4a:
        sio_ctrl = v;
        if (v & 0x10) { sio_irq = 0; }
        if (v & 0x40) { sio_ctrl = 0; sio_rx_full = 0; sio_irq = 0; }
        if (!(sio_ctrl & 0x2)) pads[0].pos = pads[1].pos = 0;
        break;
    case 0x4e: sio_baud = v; break;
    }
}
u32 sio_read(u32 r, int size) {
    if (g_log_sio && r != 0x44) fprintf(stderr, "[sio] r%02x.%d ra=%08x\n", r, size, cpu.r[31]);
    switch (r) {
    case 0x40: { u8 v = sio_rx_full ? sio_rx : 0xff; sio_rx_full = 0; return v | (v << 8) | (v << 16) | (v << 24); }
    case 0x44: return 0x5 | (sio_rx_full ? 0x2 : 0) | (sio_irq ? 0x200 : 0);
    case 0x48: return sio_mode;
    case 0x4a: return sio_ctrl;
    case 0x4e: return sio_baud;
    }
    return 0;
}

/* ------------------------------------------------------ slow path ------ */
static u32 io_read(u32 p, int size) {
    u32 r = p - 0x1f801000u;
    if (r < 0x24) return memctrl[r >> 2];
    if (r >= 0x40 && r < 0x60) return sio_read(r, size);
    if (r == 0x60) return ram_size;
    if (r == 0x70) return i_stat;
    if (r == 0x74) return i_mask;
    if (r >= 0x80 && r < 0x100) return dma_read(r - 0x80);
    if (r >= 0x100 && r < 0x130) return timer_read(r - 0x100);
    if (r >= 0x800 && r < 0x804) {
        u32 v = cd_read_reg(r - 0x800);
        if (size > 1) v |= cd_read_reg(r - 0x800 + 1) << 8;
        return v;
    }
    if (r == 0x810) return gpu_read();
    if (r == 0x814) return gpu_stat();
    if (r == 0x820 || r == 0x824) return mdec_read(r);
    if (r >= 0xc00 && r < 0x1000) {
        u32 v = spu_read(p & ~1u);
        if (size == 4) v |= (u32)spu_read((p & ~1u) + 2) << 16;
        else if (p & 1) v >>= 8;
        return v;
    }
    return 0;
}
static void io_write(u32 p, u32 v, int size) {
    u32 r = p - 0x1f801000u;
    if (r < 0x24) { memctrl[r >> 2] = v; return; }
    if (r >= 0x40 && r < 0x60) { sio_write(r, v, size); return; }
    if (r == 0x60) { ram_size = v; return; }
    if (r == 0x70) { i_stat &= v; return; }
    if (r == 0x74) { i_mask = v & 0x7ff; sched_kick(); return; }
    if (r >= 0x80 && r < 0x100) { dma_write(r - 0x80, v); return; }
    if (r >= 0x100 && r < 0x130) { timer_write(r - 0x100, v); return; }
    if (r >= 0x800 && r < 0x804) {
        cd_write_reg(r - 0x800, (u8)v);
        if (size > 1) cd_write_reg(r - 0x800 + 1, (u8)(v >> 8));
        return;
    }
    if (r == 0x810) { gpu_write_gp0(v); return; }
    if (r == 0x814) { gpu_write_gp1(v); return; }
    if (r == 0x820 || r == 0x824) { mdec_write(r, v); return; }
    if (r >= 0xc00 && r < 0x1000) {
        spu_write(p & ~1u, (u16)v);
        if (size == 4) spu_write((p & ~1u) + 2, (u16)(v >> 16));
        return;
    }
}

u32 mem_read_slow(u32 a, int size) {
    u32 p = PHYS(a);
    if (a >= 0xfffe0000u) return 0;
    if (p >= 0x1f801000u && p < 0x1f803000u) {
        u32 v = io_read(p, size);
        if (size == 1) {
            /* byte reads of word-sized registers */
            u32 r = p - 0x1f801000u;
            if (!(r >= 0x800 && r < 0x804) && !(r >= 0xc00 && r < 0x1000) && !(r >= 0x40 && r < 0x60))
                v = io_read(p & ~3u, 4) >> ((p & 3) * 8);
            return v & 0xff;
        }
        if (size == 2) {
            u32 r = p - 0x1f801000u;
            if ((p & 2) && !(r >= 0xc00 && r < 0x1000) && !(r >= 0x800 && r < 0x804) && !(r >= 0x40 && r < 0x60))
                v = io_read(p & ~3u, 4) >> 16;
            return v & 0xffff;
        }
        return v;
    }
    if (p >= 0x1f000000u && p < 0x1f800000u) return size == 4 ? 0xffffffffu : (size == 2 ? 0xffff : 0xff);
    return 0;  /* BIOS ROM / unmapped */
}
void mem_write_slow(u32 a, u32 v, int size) {
    u32 p = PHYS(a);
    if (a >= 0xfffe0000u) return;
    if (p >= 0x1f801000u && p < 0x1f803000u) {
        u32 r = p - 0x1f801000u;
        /* byte/halfword writes into 32-bit registers: merge with the current value */
        int word_reg = r < 0x24 || r == 0x60 || (r >= 0x70 && r < 0x78) || (r >= 0x80 && r < 0x100) ||
                       (r >= 0x810 && r < 0x828);
        if (size < 4 && word_reg) {
            u32 base = p & ~3u, sh = (p & 3) * 8, m = (size == 1 ? 0xffu : 0xffffu) << sh;
            u32 rb = base - 0x1f801000u, cur;
            if (rb == 0x70) cur = 0xffffffffu;                     /* I_STAT: untouched bits stay */
            else if (rb == 0xf4) cur = io_read(base, 4) & 0x00ffffffu;  /* DICR: don't ack flags */
            else if (rb >= 0x810) { if (p & 3) return; cur = 0; }
            else cur = io_read(base, 4);
            io_write(base, (cur & ~m) | ((v << sh) & m), 4);
            return;
        }
        io_write(p, v, size);
    }
}
