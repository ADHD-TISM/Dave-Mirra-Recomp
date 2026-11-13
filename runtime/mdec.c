/* MDEC: macroblock decoder used for FMV playback. */
#include "hw.h"
#include <math.h>

static u8 qt_y[64], qt_c[64];
static u32 ctrl_reg;

/* incoming command */
static u32 cmd, cmd_words, cmd_got;
static u32 inbuf[0x40000]; static u32 in_n;
static int expecting;           /* 0 none, 1 decode, 2 quant, 3 scale */

/* outgoing data */
static u32 outbuf[0x60000]; static u32 out_w, out_r;

static const u8 zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43,
    9, 11, 18, 24, 31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63 };
static u8 zagzig[64];
static float cosv[8][8];
static int tables_ready;

static void init_tables(void) {
    for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (u8)i;
    for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++)
        cosv[x][u] = (float)((u == 0 ? sqrt(0.5) : 1.0) * cos((2 * x + 1) * u * 3.14159265358979 / 16.0) * 0.5);
    tables_ready = 1;
}

static inline s32 s10(u32 v) { return ((s32)(v << 22)) >> 22; }

/* returns 0 when input runs out */
static const u16 *src16; static u32 src_n, src_pos;
static int rl_block(s32 blk[64], const u8 *qt) {
    s32 tmp[64];
    memset(tmp, 0, sizeof tmp);
    u32 n;
    do { if (src_pos >= src_n) return 0; n = src16[src_pos++]; } while (n == 0xfe00);
    u32 q = (n >> 10) & 63; int k = 0;
    s32 val = s10(n) * qt[0];
    for (;;) {
        if (q == 0) val = s10(n) * 2;
        if (val < -0x400) val = -0x400; if (val > 0x3ff) val = 0x3ff;
        if (q > 0) tmp[zagzig[k]] = val; else tmp[k] = val;
        if (src_pos >= src_n) break;
        n = src16[src_pos++];
        k += ((n >> 10) & 63) + 1;
        if (k > 63) break;
        val = (s10(n) * qt[k] * (s32)q + 4) / 8;
    }
    /* separable float IDCT */
    float t[64];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        float s = 0; for (int u = 0; u < 8; u++) s += cosv[x][u] * tmp[y * 8 + u]; t[y * 8 + x] = s;
    }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        float s = 0; for (int v = 0; v < 8; v++) s += cosv[y][v] * t[v * 8 + x];
        s32 r = (s32)lrintf(s);
        blk[y * 8 + x] = r < -128 ? -128 : r > 127 ? 127 : r;
    }
    return 1;
}

static inline s32 clamp8(s32 v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

static void emit_bytes(const u8 *b, u32 n) {
    for (u32 i = 0; i + 3 < n; i += 4)
        if (out_w < sizeof outbuf / 4) outbuf[out_w++] = b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | ((u32)b[i + 3] << 24);
}

static void decode(void) {
    if (!tables_ready) init_tables();
    int depth = (cmd >> 27) & 3, sign = (cmd >> 26) & 1, b15 = (cmd >> 25) & 1;
    src16 = (const u16 *)inbuf; src_n = in_n * 2; src_pos = 0;
    s32 cr[64], cb[64], y[4][64];
    while (src_pos < src_n) {
        if (depth >= 2) {
            if (!rl_block(cr, qt_c) || !rl_block(cb, qt_c)) break;
            int ok = 1;
            for (int i = 0; i < 4 && ok; i++) ok = rl_block(y[i], qt_y);
            if (!ok) break;
            static u8 px[16 * 16 * 3];
            int o = 0;
            for (int yy = 0; yy < 16; yy++) for (int xx = 0; xx < 16; xx++) {
                int bi = (yy >= 8) * 2 + (xx >= 8);
                s32 Y = y[bi][(yy & 7) * 8 + (xx & 7)];
                s32 Cr = cr[(yy >> 1) * 8 + (xx >> 1)], Cb = cb[(yy >> 1) * 8 + (xx >> 1)];
                s32 r = clamp8(Y + (s32)(1.402f * Cr));
                s32 g = clamp8(Y + (s32)(-0.3437f * Cb - 0.7143f * Cr));
                s32 b = clamp8(Y + (s32)(1.772f * Cb));
                if (!sign) { r ^= 0x80; g ^= 0x80; b ^= 0x80; }
                if (depth == 2) { px[o++] = (u8)r; px[o++] = (u8)g; px[o++] = (u8)b; }
                else {
                    u16 c = (u16)(((r & 0xff) >> 3) | (((g & 0xff) >> 3) << 5) | (((b & 0xff) >> 3) << 10) | (b15 << 15));
                    px[o++] = (u8)c; px[o++] = (u8)(c >> 8);
                }
            }
            emit_bytes(px, (u32)o);
        } else {
            if (!rl_block(y[0], qt_y)) break;
            u8 px[64]; int o = 0;
            if (depth == 1) for (int i = 0; i < 64; i++) px[o++] = (u8)(sign ? y[0][i] : (y[0][i] ^ 0x80));
            else for (int i = 0; i < 64; i += 2) {
                u8 a = (u8)(sign ? y[0][i] : (y[0][i] ^ 0x80)), b = (u8)(sign ? y[0][i + 1] : (y[0][i + 1] ^ 0x80));
                px[o++] = (u8)((a >> 4) | (b & 0xf0));
            }
            emit_bytes(px, (u32)o);
        }
    }
}

void dma_mdec_out_kick(void);

static void finish_command(void) {
    if (expecting == 1) { extern unsigned g_stat_mdec; g_stat_mdec++; out_w = out_r = 0; decode(); dma_mdec_out_kick(); }
    else if (expecting == 2) {
        memcpy(qt_y, inbuf, 64);
        if (cmd & 1) memcpy(qt_c, (u8 *)inbuf + 64, 64);
    }
    expecting = 0;
}

static void feed(u32 w) {
    if (!expecting) {
        cmd = w; cmd_got = 0; in_n = 0;
        switch (w >> 29) {
        case 1: expecting = 1; cmd_words = w & 0xffff; break;
        case 2: expecting = 2; cmd_words = (w & 1) ? 32 : 16; break;
        case 3: expecting = 3; cmd_words = 32; break;
        default: return;
        }
        if (!cmd_words) finish_command();
        return;
    }
    if (in_n < sizeof inbuf / 4) inbuf[in_n++] = w;
    if (++cmd_got >= cmd_words) finish_command();
}

void mdec_write(u32 r, u32 v) {
    if (r == 0x820) { feed(v); return; }
    if (v & 0x80000000u) { expecting = 0; out_w = out_r = 0; }
    ctrl_reg = v;
}
u32 mdec_read(u32 r) {
    if (r == 0x820) return out_r < out_w ? outbuf[out_r++] : 0;
    u32 s = 0;
    if (out_r >= out_w) s |= 1u << 31;
    if (expecting) s |= 1u << 29;
    if (ctrl_reg & 0x40000000u) s |= 1u << 28;
    if ((ctrl_reg & 0x20000000u) && out_r < out_w) s |= 1u << 27;
    s |= ((cmd >> 25) & 0xf) << 23;
    s |= expecting ? ((cmd_words - cmd_got - 1) & 0xffff) : 0xffff;
    return s;
}
void mdec_dma_in(u32 a, u32 words) {
    for (u32 i = 0; i < words; i++) { u32 w; memcpy(&w, psx_ram + ((a + 4 * i) & 0x1ffffc), 4); feed(w); }
}
int mdec_dma_ready(int ch) { (void)ch; return out_r < out_w; }
void mdec_dma_out(u32 a, u32 words) {
    for (u32 i = 0; i < words; i++) {
        u32 w = out_r < out_w ? outbuf[out_r++] : 0;
        memcpy(psx_ram + ((a + 4 * i) & 0x1ffffc), &w, 4);
    }
}
