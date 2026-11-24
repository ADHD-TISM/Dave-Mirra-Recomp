/* Shared runtime interface for the recompiled code. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

typedef struct {
    u32 r[32];
    u32 hi, lo;
    u32 sr, cause, epc;
    u64 cycles;      /* elapsed guest cycles */
    u64 next_event;  /* psx_sched() runs when cycles reaches this */
} CPU;

extern CPU cpu;
#define R cpu.r

extern u8 psx_ram[0x200000];
extern u8 psx_scratch[0x400];

typedef void (*psx_fn)(void);
typedef struct { u32 addr; psx_fn fn; } psx_func_entry;

/* cycles charged per recompiled instruction */
extern unsigned g_cpi;   /* guest cycles charged per instruction: 2 = console speed, 1 = overclocked CPU */
#define PSX_CPI g_cpi
void psx_sched(void);
#define CYC(n) do { cpu.cycles += (u64)(n) * PSX_CPI; \
                    if (cpu.cycles >= cpu.next_event) psx_sched(); } while (0)
/* same, at console cost whatever the CPU assist is doing (see tools/regen.sh --fullcost) */
#define CYCF(n) do { cpu.cycles += (u64)(n) * 2; \
                     if (cpu.cycles >= cpu.next_event) psx_sched(); } while (0)

/* control flow / misc helpers used by generated code */
void psx_call(u32 addr);
void psx_syscall(u32 pc);
void psx_break(u32 pc, u32 code);
void psx_bad(u32 pc, u32 insn);
void psx_fallthrough(u32 pc);
/* widescreen sky: called from the game's sky routine through the "patches" of games/<id>.json (host_common.c) */
s32 ws_sky_left(u32 w);
u32 ws_sky_first(u32 i, u32 n);
s32 ws_sky_right(void);
static inline void psx_div(u32 a, u32 b) {
    s32 n = (s32)a, d = (s32)b;
    if (d == 0) { cpu.hi = a; cpu.lo = n >= 0 ? 0xffffffffu : 1; }
    else if (a == 0x80000000u && d == -1) { cpu.hi = 0; cpu.lo = 0x80000000u; }
    else { cpu.lo = (u32)(n / d); cpu.hi = (u32)(n % d); }
}
static inline void psx_divu(u32 a, u32 b) {
    if (b == 0) { cpu.hi = a; cpu.lo = 0xffffffffu; }
    else { cpu.lo = a / b; cpu.hi = a % b; }
}

u32 cop0_read(int r);
void cop0_write(int r, u32 v);
void cop0_rfe(void);

u32 gte_read(int r);
u32 gte_readc(int r);
void gte_write(int r, u32 v);
void gte_writec(int r, u32 v);
void gte_cmd(u32 op);

/* memory */
u32 mem_read_slow(u32 a, int size);
void mem_write_slow(u32 a, u32 v, int size);

#define PHYS(a) ((a) & 0x1fffffffu)
#define IS_RAM(p) ((p) < 0x00800000u)
#define IS_SCR(p) ((u32)((p) - 0x1f800000u) < 0x400u)

static inline u32 mr32(u32 a) {
    u32 p = PHYS(a), v;
    if (IS_RAM(p)) { memcpy(&v, psx_ram + (p & 0x1ffffcu), 4); return v; }
    if (IS_SCR(p)) { memcpy(&v, psx_scratch + (p & 0x3fcu), 4); return v; }
    return mem_read_slow(a, 4);
}
static inline u32 mr16(u32 a) {
    u32 p = PHYS(a); u16 v;
    if (IS_RAM(p)) { memcpy(&v, psx_ram + (p & 0x1ffffeu), 2); return v; }
    if (IS_SCR(p)) { memcpy(&v, psx_scratch + (p & 0x3feu), 2); return v; }
    return mem_read_slow(a, 2);
}
static inline u32 mr8(u32 a) {
    u32 p = PHYS(a);
    if (IS_RAM(p)) return psx_ram[p & 0x1fffffu];
    if (IS_SCR(p)) return psx_scratch[p & 0x3ffu];
    return mem_read_slow(a, 1);
}
static inline void mw32(u32 a, u32 v) {
    u32 p = PHYS(a);
    if (cpu.sr & 0x10000) return;  /* cache isolated */
    if (IS_RAM(p)) { memcpy(psx_ram + (p & 0x1ffffcu), &v, 4); return; }
    if (IS_SCR(p)) { memcpy(psx_scratch + (p & 0x3fcu), &v, 4); return; }
    mem_write_slow(a, v, 4);
}
static inline void mw16(u32 a, u16 v) {
    u32 p = PHYS(a);
    if (cpu.sr & 0x10000) return;
    if (IS_RAM(p)) { memcpy(psx_ram + (p & 0x1ffffeu), &v, 2); return; }
    if (IS_SCR(p)) { memcpy(psx_scratch + (p & 0x3feu), &v, 2); return; }
    mem_write_slow(a, v, 2);
}
static inline void mw8(u32 a, u8 v) {
    u32 p = PHYS(a);
    if (cpu.sr & 0x10000) return;
    if (IS_RAM(p)) { psx_ram[p & 0x1fffffu] = v; return; }
    if (IS_SCR(p)) { psx_scratch[p & 0x3ffu] = v; return; }
    mem_write_slow(a, v, 1);
}
static inline u32 mlwl(u32 a, u32 rt) {
    u32 w = mr32(a & ~3u);
    switch (a & 3) {
    case 0: return (rt & 0x00ffffffu) | (w << 24);
    case 1: return (rt & 0x0000ffffu) | (w << 16);
    case 2: return (rt & 0x000000ffu) | (w << 8);
    default: return w;
    }
}
static inline u32 mlwr(u32 a, u32 rt) {
    u32 w = mr32(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (rt & 0xff000000u) | (w >> 8);
    case 2: return (rt & 0xffff0000u) | (w >> 16);
    default: return (rt & 0xffffff00u) | (w >> 24);
    }
}
static inline void mswl(u32 a, u32 rt) {
    u32 m = mr32(a & ~3u);
    switch (a & 3) {
    case 0: m = (m & 0xffffff00u) | (rt >> 24); break;
    case 1: m = (m & 0xffff0000u) | (rt >> 16); break;
    case 2: m = (m & 0xff000000u) | (rt >> 8); break;
    default: m = rt;
    }
    mw32(a & ~3u, m);
}
static inline void mswr(u32 a, u32 rt) {
    u32 m = mr32(a & ~3u);
    switch (a & 3) {
    case 0: m = rt; break;
    case 1: m = (m & 0x000000ffu) | (rt << 8); break;
    case 2: m = (m & 0x0000ffffu) | (rt << 16); break;
    default: m = (m & 0x00ffffffu) | (rt << 24);
    }
    mw32(a & ~3u, m);
}

/* host-side pointers into guest RAM */
static inline u8 *ram_ptr(u32 a) { return psx_ram + (PHYS(a) & 0x1fffffu); }
