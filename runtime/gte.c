/* Geometry Transformation Engine (COP2). */
#include "psx.h"

static s32 d[32];   /* data registers */
static s32 c[32];   /* control registers */
static u32 flag;

/* data register aliases */
#define VX(i) ((s16)d[(i) * 2])
#define VY(i) ((s16)(d[(i) * 2] >> 16))
#define VZ(i) ((s16)d[(i) * 2 + 1])
#define RGBC d[6]
#define OTZ d[7]
#define IR0 d[8]
#define IR1 d[9]
#define IR2 d[10]
#define IR3 d[11]
#define SXY(i) d[12 + (i)]
#define SZ(i) d[16 + (i)]
#define RGB(i) d[20 + (i)]
#define MAC0 d[24]
#define MAC1 d[25]
#define MAC2 d[26]
#define MAC3 d[27]

/* control register aliases */
#define M(m, r, col) ((s16)(((r) * 3 + (col)) & 1 ? c[(m) * 8 + (((r) * 3 + (col)) >> 1)] >> 16 : c[(m) * 8 + (((r) * 3 + (col)) >> 1)]))
#define TR(i) c[5 + (i)]
#define BK(i) c[13 + (i)]
#define FC(i) c[21 + (i)]
#define OFX c[24]
#define OFY c[25]
#define H ((u16)c[26])
#define DQA ((s16)c[27])
#define DQB c[28]
#define ZSF3 ((s16)c[29])
#define ZSF4 ((s16)c[30])

static u8 unr[0x101];
static int unr_ready;

static void init_unr(void) {
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr[i] = (u8)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}

/* ---- saturation helpers ---- */
static s64 A(int i, s64 v) {
    if (v > 0x7ffffffffffLL) flag |= 1u << (31 - i);
    else if (v < -0x80000000000LL) flag |= 1u << (28 - i);
    return (v << 20) >> 20;
}
static s32 LmB(int i, s32 v, int lm) {
    s32 lo = lm ? 0 : -0x8000;
    if (v < lo) { flag |= 1u << (25 - i); return lo; }
    if (v > 0x7fff) { flag |= 1u << (25 - i); return 0x7fff; }
    return v;
}
static s32 LmC(int i, s32 v) {
    if (v < 0) { flag |= 1u << (22 - i); return 0; }
    if (v > 0xff) { flag |= 1u << (22 - i); return 0xff; }
    return v;
}
static s32 LmD(s64 v) {
    if (v < 0) { flag |= 1u << 18; return 0; }
    if (v > 0xffff) { flag |= 1u << 18; return 0xffff; }
    return (s32)v;
}
static s64 F(s64 v) {
    if (v > 0x7fffffffLL) flag |= 1u << 16;
    else if (v < -0x80000000LL) flag |= 1u << 15;
    return v;
}
static s32 LmG(int i, s64 v) {
    if (v < -0x400) { flag |= 1u << (15 - i); return -0x400; }
    if (v > 0x3ff) { flag |= 1u << (15 - i); return 0x3ff; }
    return (s32)v;
}
static s32 LmH(s64 v) {
    if (v < 0) { flag |= 1u << 12; return 0; }
    if (v > 0x1000) { flag |= 1u << 12; return 0x1000; }
    return (s32)v;
}

static void mac123(s64 a, s64 b, s64 cc, int sh) {
    MAC1 = (s32)(A(1, a) >> sh); MAC2 = (s32)(A(2, b) >> sh); MAC3 = (s32)(A(3, cc) >> sh);
}
static void ir123(int lm) { IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm); IR3 = LmB(3, MAC3, lm); }

static void push_color(void) {
    RGB(0) = RGB(1); RGB(1) = RGB(2);
    RGB(2) = (s32)(((u32)RGBC & 0xff000000u) | ((u32)LmC(3, MAC3 >> 4) << 16) |
                   ((u32)LmC(2, MAC2 >> 4) << 8) | (u32)LmC(1, MAC1 >> 4));
}

static u32 gte_divide(u32 h, u32 sz3) {
    if (h < sz3 * 2) {
        int z = 0; while (z < 16 && !((sz3 << z) & 0x8000)) z++;
        u64 n = (u64)h << z; u32 dd = sz3 << z;
        u32 u = unr[(dd - 0x7fc0) >> 7] + 0x101;
        dd = (0x2000080 - dd * u) >> 8;
        dd = (0x0000080 + dd * u) >> 8;
        u64 r = (n * dd + 0x8000) >> 16;
        return r > 0x1ffff ? 0x1ffff : (u32)r;
    }
    flag |= 1u << 17;
    return 0x1ffff;
}

static void rtp(int v, int sh, int lm, int last) {
    s64 x = VX(v), y = VY(v), z = VZ(v);
    s64 m1 = ((s64)TR(0) << 12) + M(0, 0, 0) * x + M(0, 0, 1) * y + M(0, 0, 2) * z;
    s64 m2 = ((s64)TR(1) << 12) + M(0, 1, 0) * x + M(0, 1, 1) * y + M(0, 1, 2) * z;
    s64 m3 = ((s64)TR(2) << 12) + M(0, 2, 0) * x + M(0, 2, 1) * y + M(0, 2, 2) * z;
    mac123(m1, m2, m3, sh);
    IR1 = LmB(1, MAC1, lm); IR2 = LmB(2, MAC2, lm);
    /* IR3 flag uses the unshifted value >> 12 */
    s32 m3s = (s32)(A(3, m3) >> 12);
    if (m3s < -0x8000 || m3s > 0x7fff) flag |= 1u << 22;
    { s32 lo = lm ? 0 : -0x8000; IR3 = MAC3 < lo ? lo : MAC3 > 0x7fff ? 0x7fff : MAC3; }
    SZ(0) = SZ(1); SZ(1) = SZ(2); SZ(2) = SZ(3);
    SZ(3) = LmD(A(3, m3) >> 12);
    u32 n = gte_divide(H, (u32)SZ(3));
    s64 sx = F((s64)n * IR1 + OFX), sy;
    s32 X = LmG(1, sx >> 16);
    sy = F((s64)n * IR2 + OFY);
    s32 Y = LmG(2, sy >> 16);
    MAC0 = (s32)sy;
    SXY(0) = SXY(1); SXY(1) = SXY(2);
    SXY(2) = (s32)(((u32)X & 0xffff) | ((u32)Y << 16));
    if (last) {
        s64 dq = F((s64)n * DQA + DQB);
        MAC0 = (s32)dq;
        IR0 = LmH(dq >> 12);
    }
}

static void mvmva(u32 op, int sh, int lm) {
    int mx = (op >> 17) & 3, vv = (op >> 15) & 3, cv = (op >> 13) & 3;
    s64 x, y, z;
    if (vv == 3) { x = (s16)IR1; y = (s16)IR2; z = (s16)IR3; }
    else { x = VX(vv); y = VY(vv); z = VZ(vv); }
    s64 mm[3][3];
    if (mx == 3) {
        s64 r = (s64)(((u32)RGBC & 0xff) << 4);
        mm[0][0] = -r; mm[0][1] = r; mm[0][2] = (s16)IR0;
        mm[1][0] = mm[1][1] = mm[1][2] = M(0, 0, 2);
        mm[2][0] = mm[2][1] = mm[2][2] = M(0, 1, 1);
    } else for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) mm[i][j] = M(mx, i, j);
    s64 t[3] = { 0, 0, 0 };
    if (cv == 0) { t[0] = TR(0); t[1] = TR(1); t[2] = TR(2); }
    else if (cv == 1) { t[0] = BK(0); t[1] = BK(1); t[2] = BK(2); }
    else if (cv == 2) { t[0] = FC(0); t[1] = FC(1); t[2] = FC(2); }
    s64 res[3];
    for (int i = 0; i < 3; i++) {
        if (cv == 2) {
            /* hardware bug: far-colour vector only affects flags */
            LmB(i + 1, (s32)(A(i + 1, (t[i] << 12) + mm[i][0] * x) >> sh), 0);
            res[i] = mm[i][1] * y + mm[i][2] * z;
        } else res[i] = (t[i] << 12) + mm[i][0] * x + mm[i][1] * y + mm[i][2] * z;
    }
    mac123(res[0], res[1], res[2], sh);
    ir123(lm);
}

/* light matrix * vector, then BK + colour matrix * IR */
static void normal_light(int v, int sh, int lm) {
    s64 x = VX(v), y = VY(v), z = VZ(v);
    mac123(M(1, 0, 0) * x + M(1, 0, 1) * y + M(1, 0, 2) * z,
           M(1, 1, 0) * x + M(1, 1, 1) * y + M(1, 1, 2) * z,
           M(1, 2, 0) * x + M(1, 2, 1) * y + M(1, 2, 2) * z, sh);
    ir123(lm);
}
static void color_matrix(int sh, int lm) {
    s64 a = IR1, b = IR2, cc = IR3;
    mac123(((s64)BK(0) << 12) + M(2, 0, 0) * a + M(2, 0, 1) * b + M(2, 0, 2) * cc,
           ((s64)BK(1) << 12) + M(2, 1, 0) * a + M(2, 1, 1) * b + M(2, 1, 2) * cc,
           ((s64)BK(2) << 12) + M(2, 2, 0) * a + M(2, 2, 1) * b + M(2, 2, 2) * cc, sh);
    ir123(lm);
}
/* interpolate MAC (given as pre-shift 'base' values) towards far colour by IR0 */
static void depth_cue(s64 b1, s64 b2, s64 b3, int sh, int lm) {
    s32 t1 = LmB(1, (s32)(A(1, ((s64)FC(0) << 12) - b1) >> sh), 0);
    s32 t2 = LmB(2, (s32)(A(2, ((s64)FC(1) << 12) - b2) >> sh), 0);
    s32 t3 = LmB(3, (s32)(A(3, ((s64)FC(2) << 12) - b3) >> sh), 0);
    mac123(b1 + (s64)IR0 * t1, b2 + (s64)IR0 * t2, b3 + (s64)IR0 * t3, sh);
    ir123(lm);
    push_color();
}
static void color_mul(int sh, int lm, int cue) {
    s64 r = (u32)RGBC & 0xff, g = ((u32)RGBC >> 8) & 0xff, b = ((u32)RGBC >> 16) & 0xff;
    s64 b1 = (r * IR1) << 4, b2 = (g * IR2) << 4, b3 = (b * IR3) << 4;
    if (cue) { depth_cue(b1, b2, b3, sh, lm); return; }
    mac123(b1, b2, b3, sh); ir123(lm); push_color();
}

void gte_cmd(u32 op) {
    if (!unr_ready) init_unr();
    int sh = (op & (1 << 19)) ? 12 : 0, lm = (op >> 10) & 1;
    flag = 0;
    switch (op & 0x3f) {
    case 0x01: rtp(0, sh, lm, 1); break;
    case 0x30: rtp(0, sh, lm, 0); rtp(1, sh, lm, 0); rtp(2, sh, lm, 1); break;
    case 0x06: {
        s64 x0 = (s16)SXY(0), y0 = SXY(0) >> 16, x1 = (s16)SXY(1), y1 = SXY(1) >> 16, x2 = (s16)SXY(2), y2 = SXY(2) >> 16;
        MAC0 = (s32)F(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        break;
    }
    case 0x0c: {
        s64 d1 = M(0, 0, 0), d2 = M(0, 1, 1), d3 = M(0, 2, 2);
        mac123(d2 * IR3 - d3 * IR2, d3 * IR1 - d1 * IR3, d1 * IR2 - d2 * IR1, sh);
        ir123(lm);
        break;
    }
    case 0x10: case 0x2a: {
        int n = (op & 0x3f) == 0x2a ? 3 : 1;
        for (int i = 0; i < n; i++) {
            u32 col = n == 3 ? (u32)RGB(0) : (u32)RGBC;
            s64 r = col & 0xff, g = (col >> 8) & 0xff, b = (col >> 16) & 0xff;
            depth_cue(r << 16, g << 16, b << 16, sh, lm);
        }
        break;
    }
    case 0x11: depth_cue((s64)IR1 << 12, (s64)IR2 << 12, (s64)IR3 << 12, sh, lm); break;
    case 0x12: mvmva(op, sh, lm); break;
    case 0x13: normal_light(0, sh, lm); color_matrix(sh, lm); color_mul(sh, lm, 1); break;
    case 0x16: for (int v = 0; v < 3; v++) { normal_light(v, sh, lm); color_matrix(sh, lm); color_mul(sh, lm, 1); } break;
    case 0x14: color_matrix(sh, lm); color_mul(sh, lm, 1); break;
    case 0x1b: normal_light(0, sh, lm); color_matrix(sh, lm); color_mul(sh, lm, 0); break;
    case 0x3f: for (int v = 0; v < 3; v++) { normal_light(v, sh, lm); color_matrix(sh, lm); color_mul(sh, lm, 0); } break;
    case 0x1c: color_matrix(sh, lm); color_mul(sh, lm, 0); break;
    case 0x1e: normal_light(0, sh, lm); color_matrix(sh, lm); push_color(); break;
    case 0x20: for (int v = 0; v < 3; v++) { normal_light(v, sh, lm); color_matrix(sh, lm); push_color(); } break;
    case 0x28: mac123((s64)IR1 * IR1, (s64)IR2 * IR2, (s64)IR3 * IR3, sh); ir123(lm); break;
    case 0x29: color_mul(sh, lm, 1); break;
    case 0x2d: { s64 v = F((s64)ZSF3 * (SZ(1) + SZ(2) + SZ(3))); MAC0 = (s32)v; OTZ = LmD(v >> 12); break; }
    case 0x2e: { s64 v = F((s64)ZSF4 * (SZ(0) + SZ(1) + SZ(2) + SZ(3))); MAC0 = (s32)v; OTZ = LmD(v >> 12); break; }
    case 0x3d: mac123((s64)IR0 * IR1, (s64)IR0 * IR2, (s64)IR0 * IR3, sh); ir123(lm); push_color(); break;
    case 0x3e:
        mac123(((s64)MAC1 << sh) + (s64)IR0 * IR1, ((s64)MAC2 << sh) + (s64)IR0 * IR2,
               ((s64)MAC3 << sh) + (s64)IR0 * IR3, sh);
        ir123(lm); push_color(); break;
    default: break;
    }
    if (flag & 0x7f87e000u) flag |= 0x80000000u;
}

static s32 sat5(s32 v) { v >>= 7; return v < 0 ? 0 : v > 0x1f ? 0x1f : v; }

u32 gte_read(int r) {
    switch (r) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (u32)(s32)(s16)d[r];
    case 7: case 16: case 17: case 18: case 19: return (u16)d[r];
    case 15: return (u32)SXY(2);
    case 28: case 29: return (u32)(sat5(IR1) | (sat5(IR2) << 5) | (sat5(IR3) << 10));
    case 31: {
        s32 v = d[30]; u32 n = 0; u32 x = v < 0 ? ~(u32)v : (u32)v;
        while (n < 32 && !(x & 0x80000000u)) { n++; x <<= 1; }
        return n;
    }
    default: return (u32)d[r];
    }
}
void gte_write(int r, u32 v) {
    switch (r) {
    case 15: SXY(0) = SXY(1); SXY(1) = SXY(2); SXY(2) = (s32)v; break;
    case 28: d[28] = v & 0x7fff; IR1 = (v & 0x1f) << 7; IR2 = ((v >> 5) & 0x1f) << 7; IR3 = ((v >> 10) & 0x1f) << 7; break;
    case 29: case 31: break;
    default: d[r] = (s32)v;
    }
}
u32 gte_readc(int r) {
    if (r == 31) return flag;
    if (r == 4 || r == 12 || r == 20 || r == 26 || r == 27 || r == 29 || r == 30) return (u32)(s32)(s16)c[r];
    return (u32)c[r];
}
void gte_writec(int r, u32 v) {
    if (r == 31) { flag = v & 0x7ffff000u; if (flag & 0x7f87e000u) flag |= 0x80000000u; return; }
    c[r] = (s32)v;
}
