/* Sound Processing Unit: 24 ADPCM voices, ADSR, CD audio input. */
#include "hw.h"

static u8 spuram[512 * 1024];
static u16 regs[0x200];          /* raw register file, 0x1F801C00.. */
#define REG(off) regs[((off) - 0x1f801c00u) >> 1]

static u32 xfer_addr;
static u32 endx;
static u16 spucnt, spustat;

typedef struct {
    u32 cur, start, repeat;   /* byte addresses */
    u32 counter;              /* 12.12 fixed sample position within block */
    s16 buf[28]; s32 old, older;
    int block_valid;
    int phase;                /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    s32 env; int env_cnt;
    s16 last;                 /* previous output sample for pitch modulation */
    s16 prev;                 /* for linear interpolation */
} Voice;
static Voice v[24];

static const s32 F0[5] = { 0, 60, 115, 98, 122 }, F1[5] = { 0, 0, -52, -55, -60 };

static void check_irq(u32 addr) {
    if ((spucnt & 0x40) && (addr & ~7u) == ((u32)REG(0x1f801da4u) * 8 & ~7u)) {
        if (!(spustat & 0x40)) { spustat |= 0x40; irq_raise(IRQ_SPU); }
    }
}

static void decode_block(Voice *vc, int n) {
    const u8 *b = spuram + (vc->cur & 0x7fff0);
    check_irq(vc->cur);
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
    if (filt > 4) filt = 4;
    if (shift > 12) shift = 9;
    u8 flags = b[1];
    if (flags & 4) { vc->repeat = vc->cur; REG(0x1f801c0eu + n * 16) = (u16)(vc->cur >> 3); }
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        s32 s = (s32)(s16)(nib << 12) >> shift;
        s += (vc->old * F0[filt] + vc->older * F1[filt] + 32) >> 6;
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        vc->older = vc->old; vc->old = s;
        vc->buf[i] = (s16)s;
    }
    vc->block_valid = 1;
}

static void next_block(Voice *vc, int n) {
    const u8 *b = spuram + (vc->cur & 0x7fff0);
    u8 flags = b[1];
    if (flags & 1) {
        endx |= 1u << n;
        vc->cur = vc->repeat;
        if (!(flags & 2)) { vc->phase = 4; vc->env = 0; }
    } else vc->cur = (vc->cur + 16) & 0x7ffff;
    vc->block_valid = 0;
}

/* ADSR envelope step per psx-spx */
static void env_step(Voice *vc, int rate, int exp, int dec) {
    if (rate >= 0x7f && !dec) return;
    int shift = rate >> 2;
    s32 step = dec ? (-8 + (rate & 3)) : (7 - (rate & 3));
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    step <<= (shift < 11 ? 11 - shift : 0);
    if (exp && !dec && vc->env > 0x6000) cycles *= 4;
    if (exp && dec) step = step * vc->env >> 15;
    if (++vc->env_cnt < cycles) return;
    vc->env_cnt = 0;
    vc->env += step;
    if (vc->env > 0x7fff) vc->env = 0x7fff;
    if (vc->env < 0) vc->env = 0;
}

static void env_tick(Voice *vc, int n) {
    u16 lo = REG(0x1f801c08u + n * 16), hi = REG(0x1f801c0au + n * 16);
    switch (vc->phase) {
    case 1:
        env_step(vc, (lo >> 8) & 0x7f, lo >> 15, 0);
        if (vc->env >= 0x7fff) { vc->env = 0x7fff; vc->phase = 2; }
        break;
    case 2: {
        s32 sl = ((lo & 15) + 1) * 0x800;
        env_step(vc, ((lo >> 4) & 15) << 2, 1, 1);
        if (vc->env <= sl) vc->phase = 3;
        break;
    }
    case 3: env_step(vc, (hi >> 6) & 0x7f, hi >> 15, (hi >> 14) & 1); break;
    case 4:
        env_step(vc, (hi & 31) << 2, (hi >> 5) & 1, 1);
        if (vc->env <= 0) { vc->env = 0; vc->phase = 0; }
        break;
    }
    REG(0x1f801c0cu + n * 16) = (u16)vc->env;
}

static s32 vol(u16 r) {
    if (r & 0x8000) return 0x7fff;        /* sweep mode not modelled: full */
    return (s32)(s16)(r << 1);
}

static void key_on(u32 mask) {
    for (int n = 0; n < 24; n++) if (mask & (1u << n)) {
        Voice *vc = &v[n];
        vc->start = (u32)REG(0x1f801c06u + n * 16) * 8;
        vc->cur = vc->start; vc->repeat = (u32)REG(0x1f801c0eu + n * 16) * 8;
        vc->counter = 0; vc->old = vc->older = 0; vc->block_valid = 0;
        vc->phase = 1; vc->env = 0; vc->env_cnt = 0; vc->prev = 0;
        endx &= ~(1u << n);
    }
}
static void key_off(u32 mask) {
    for (int n = 0; n < 24; n++) if ((mask & (1u << n)) && v[n].phase) { v[n].phase = 4; v[n].env_cnt = 0; }
}

/* ---- CD audio ring ---- */
#define CDR 32768
static s16 cdring[CDR * 2]; static u32 cd_w, cd_r;
void spu_cd_audio(const s16 *lr, int frames) {
    for (int i = 0; i < frames; i++) {
        if (cd_w - cd_r >= CDR) cd_r++;   /* overflow: drop oldest */
        cdring[(cd_w % CDR) * 2] = lr[i * 2];
        cdring[(cd_w % CDR) * 2 + 1] = lr[i * 2 + 1];
        cd_w++;
    }
}

/* ---- reverb ----
   The reverb unit runs at half the sample rate over a ring buffer at the top
   of sound RAM (from mBASE to the end). Register names follow psx-spx. */
int g_reverb = 1;
enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
       mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
       mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };
#define RV(i) REG(0x1f801dc0u + (i) * 2)
static u32 rev_cur;                      /* current buffer address, in bytes */
static s32 rev_out_l, rev_out_r;         /* last output, held for the in-between sample */
static inline s32 sat16(s32 x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
static u32 rev_addr(u32 reg_units, s32 bytes) {
    u32 base = (u32)REG(0x1f801da2u) * 8, size = 0x80000 - base;
    if (!size) return 0x7fffe;
    long long off = ((long long)rev_cur - base + (long long)reg_units * 8 + bytes) % (long long)size;
    if (off < 0) off += size;            /* negative taps (-2, all-pass delays) must wrap inside the buffer */
    return (base + (u32)off) & 0x7fffe;
}
static inline s32 rev_rd(int reg, s32 bytes) { u32 a = rev_addr(RV(reg), bytes); return (s16)(spuram[a] | (spuram[a + 1] << 8)); }
static inline void rev_wr(int reg, s32 val) {
    if (!(spucnt & 0x80)) return;        /* reverb master enable: off = buffer is read but not written */
    u32 a = rev_addr(RV(reg), 0); val = sat16(val);
    spuram[a] = (u8)val; spuram[a + 1] = (u8)(val >> 8);
}
#define VOLR(i) ((s32)(s16)RV(i))
static void rev_step(s32 in_l, s32 in_r) {
    u32 base = (u32)REG(0x1f801da2u) * 8;
    if (rev_cur < base || rev_cur >= 0x80000) rev_cur = base;
    s32 lin = in_l * VOLR(vLIN) >> 15, rin = in_r * VOLR(vRIN) >> 15;
    s32 wall = VOLR(vWALL), iir = VOLR(vIIR), t;
    /* same-side and cross reflections */
    t = rev_rd(mLSAME, -2); rev_wr(mLSAME, ((lin + (rev_rd(dLSAME, 0) * wall >> 15) - t) * iir >> 15) + t);
    t = rev_rd(mRSAME, -2); rev_wr(mRSAME, ((rin + (rev_rd(dRSAME, 0) * wall >> 15) - t) * iir >> 15) + t);
    t = rev_rd(mLDIFF, -2); rev_wr(mLDIFF, ((lin + (rev_rd(dRDIFF, 0) * wall >> 15) - t) * iir >> 15) + t);
    t = rev_rd(mRDIFF, -2); rev_wr(mRDIFF, ((rin + (rev_rd(dLDIFF, 0) * wall >> 15) - t) * iir >> 15) + t);
    /* early echo */
    s32 l = (VOLR(vCOMB1) * rev_rd(mLCOMB1, 0) + VOLR(vCOMB2) * rev_rd(mLCOMB2, 0) + VOLR(vCOMB3) * rev_rd(mLCOMB3, 0) + VOLR(vCOMB4) * rev_rd(mLCOMB4, 0)) >> 15;
    s32 r = (VOLR(vCOMB1) * rev_rd(mRCOMB1, 0) + VOLR(vCOMB2) * rev_rd(mRCOMB2, 0) + VOLR(vCOMB3) * rev_rd(mRCOMB3, 0) + VOLR(vCOMB4) * rev_rd(mRCOMB4, 0)) >> 15;
    /* two all-pass filters */
    s32 d1 = -(s32)RV(dAPF1) * 8, d2 = -(s32)RV(dAPF2) * 8;
    l = sat16(l - (VOLR(vAPF1) * rev_rd(mLAPF1, d1) >> 15)); rev_wr(mLAPF1, l); l = sat16((l * VOLR(vAPF1) >> 15) + rev_rd(mLAPF1, d1));
    r = sat16(r - (VOLR(vAPF1) * rev_rd(mRAPF1, d1) >> 15)); rev_wr(mRAPF1, r); r = sat16((r * VOLR(vAPF1) >> 15) + rev_rd(mRAPF1, d1));
    l = sat16(l - (VOLR(vAPF2) * rev_rd(mLAPF2, d2) >> 15)); rev_wr(mLAPF2, l); l = sat16((l * VOLR(vAPF2) >> 15) + rev_rd(mLAPF2, d2));
    r = sat16(r - (VOLR(vAPF2) * rev_rd(mRAPF2, d2) >> 15)); rev_wr(mRAPF2, r); r = sat16((r * VOLR(vAPF2) >> 15) + rev_rd(mRAPF2, d2));
    rev_out_l = l * (s32)(s16)REG(0x1f801d84u) >> 15;
    rev_out_r = r * (s32)(s16)REG(0x1f801d86u) >> 15;
    rev_cur += 2; if (rev_cur >= 0x80000) rev_cur = base;
}

void spu_render(s16 *out, int frames) {
    u32 eon = REG(0x1f801d98u) | ((u32)REG(0x1f801d9au) << 16);
    static s32 rin_l, rin_r; static int rev_phase;
    u32 pmon = REG(0x1f801d90u) | ((u32)REG(0x1f801d92u) << 16);
    s32 mvl = vol(REG(0x1f801d80u)), mvr = vol(REG(0x1f801d82u));
    s32 cvl = (s16)REG(0x1f801db0u), cvr = (s16)REG(0x1f801db2u);
    int enabled = (spucnt & 0x8000) != 0, unmuted = (spucnt & 0x4000) != 0;
    for (int f = 0; f < frames; f++) {
        s32 l = 0, r = 0, el = 0, er = 0;   /* el/er: what is sent into the reverb unit */
        for (int n = 0; n < 24; n++) {
            Voice *vc = &v[n];
            if (!vc->phase) { vc->last = 0; continue; }
            if (!vc->block_valid) decode_block(vc, n);
            int idx = vc->counter >> 12;
            s16 s0 = idx ? vc->buf[idx - 1] : vc->prev, s1 = vc->buf[idx];
            s32 frac = vc->counter & 0xfff;
            s32 s = s0 + (((s1 - s0) * frac) >> 12);
            s = s * vc->env >> 15;
            vc->last = (s16)s;
            s32 sl = s * vol(REG(0x1f801c00u + n * 16)) >> 15, sr = s * vol(REG(0x1f801c02u + n * 16)) >> 15;
            l += sl; r += sr;
            if (eon & (1u << n)) { el += sl; er += sr; }
            u32 step = REG(0x1f801c04u + n * 16);
            if (n > 0 && (pmon & (1u << n))) {
                s32 factor = v[n - 1].last + 0x8000;
                step = (u32)(((s32)(s16)step * factor) >> 15) & 0xffff;
            }
            if (step > 0x3fff) step = 0x3fff;
            vc->counter += step;
            while ((vc->counter >> 12) >= 28) {
                vc->counter -= 28 << 12;
                vc->prev = vc->buf[27];
                next_block(vc, n);
                if (!vc->phase) break;
                decode_block(vc, n);
            }
            env_tick(vc, n);
        }
        l = l * mvl >> 15; r = r * mvr >> 15;
        if (cd_r != cd_w) {
            s32 cl = cdring[(cd_r % CDR) * 2], cr = cdring[(cd_r % CDR) * 2 + 1]; cd_r++;
            if (spucnt & 1) { l += cl * cvl >> 15; r += cr * cvr >> 15; }
            if ((spucnt & 5) == 5) { el += cl * cvl >> 15; er += cr * cvr >> 15; }
        }
        if (g_reverb) {
            rin_l += el; rin_r += er;
            if ((rev_phase ^= 1) == 0) { rev_step(sat16(rin_l >> 1), sat16(rin_r >> 1)); rin_l = rin_r = 0; }
            l += rev_out_l * mvl >> 15; r += rev_out_r * mvr >> 15;
        }
        if (!enabled || !unmuted) l = r = 0;
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        out[f * 2] = (s16)l; out[f * 2 + 1] = (s16)r;
    }
}

u16 spu_read(u32 a) {
    u32 off = a - 0x1f801c00u;
    if (off >= 0x400) return 0;
    switch (a) {
    case 0x1f801d9cu: return (u16)endx;
    case 0x1f801d9eu: return (u16)(endx >> 16);
    case 0x1f801daau: return spucnt;
    case 0x1f801daeu: return (spustat & 0xffc0) | (spucnt & 0x3f);
    case 0x1f801da6u: return (u16)(xfer_addr >> 3);
    }
    if (off >= 0x200 && off < 0x260) return 0x4000;   /* current voice volume */
    return regs[off >> 1];
}

void spu_write(u32 a, u16 val) {
    u32 off = a - 0x1f801c00u;
    if (off >= 0x400) return;
    regs[off >> 1] = val;
    switch (a) {
    case 0x1f801d88u: key_on(val); break;
    case 0x1f801d8au: key_on((u32)val << 16); break;
    case 0x1f801d8cu: key_off(val); break;
    case 0x1f801d8eu: key_off((u32)val << 16); break;
    case 0x1f801d9cu: case 0x1f801d9eu: break;
    case 0x1f801da6u: xfer_addr = (u32)val * 8; break;
    case 0x1f801da8u:
        check_irq(xfer_addr);
        spuram[xfer_addr & 0x7ffff] = (u8)val; spuram[(xfer_addr + 1) & 0x7ffff] = (u8)(val >> 8);
        xfer_addr = (xfer_addr + 2) & 0x7ffff; break;
    case 0x1f801daau:
        spucnt = val;
        if (!(val & 0x40)) spustat &= ~0x40;
        spustat = (spustat & ~0x380u) | ((val & 0x30) == 0x20 ? 0x100 : 0) | ((val & 0x30) == 0x30 ? 0x200 : 0);
        break;
    }
}

void spu_dma_write(const u8 *src, u32 bytes) {
    for (u32 i = 0; i < bytes; i++) { if (!(i & 7)) check_irq(xfer_addr); spuram[xfer_addr] = src[i]; xfer_addr = (xfer_addr + 1) & 0x7ffff; }
}
void spu_dma_read(u8 *dst, u32 bytes) {
    for (u32 i = 0; i < bytes; i++) { dst[i] = spuram[xfer_addr]; xfer_addr = (xfer_addr + 1) & 0x7ffff; }
}
void spu_init(void) { memset(v, 0, sizeof v); }
