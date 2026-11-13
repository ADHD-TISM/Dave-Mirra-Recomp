/* Dispatcher, scheduler, interrupt plumbing, COP0. */
#include "hw.h"
#include <stdlib.h>

CPU cpu;

/* ---- module registry -------------------------------------------------- */
#define MODULE(P) \
    extern const psx_func_entry P##_table[]; extern const unsigned P##_table_count; \
    extern const u32 P##_base, P##_end, P##_entry, P##_sig[8];
MODULE(main) MODULE(ai) MODULE(fr) MODULE(movie)

typedef struct {
    const char *name;
    const psx_func_entry *tab; const unsigned *count;
    const u32 *base, *end, *sig;
    int resident;   /* always present, no signature check */
} Module;

static const Module modules[] = {
    { "main",  main_table,  &main_table_count,  &main_base,  &main_end,  main_sig,  1 },
    { "ai",    ai_table,    &ai_table_count,    &ai_base,    &ai_end,    ai_sig,    0 },
    { "fr",    fr_table,    &fr_table_count,    &fr_base,    &fr_end,    fr_sig,    0 },
    { "movie", movie_table, &movie_table_count, &movie_base, &movie_end, movie_sig, 0 },
};

static int module_present(const Module *m) {
    if (m->resident) return 1;
    for (int i = 0; i < 8; i++)
        if (mr32(*m->base + 4 * i) != m->sig[i]) return 0;
    return 1;
}

psx_fn psx_lookup(u32 a) {
    for (unsigned k = 0; k < sizeof modules / sizeof *modules; k++) {
        const Module *m = &modules[k];
        if (a < *m->base || a >= *m->end) continue;
        if (!module_present(m)) continue;
        unsigned lo = 0, hi = *m->count;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if (m->tab[mid].addr < a) lo = mid + 1; else hi = mid;
        }
        if (lo < *m->count && m->tab[lo].addr == a) return m->tab[lo].fn;
    }
    return NULL;
}

static void log_miss(u32 a) {
    const char *mod = "unknown";
    for (unsigned k = 0; k < sizeof modules / sizeof *modules; k++)
        if (a >= *modules[k].base && a < *modules[k].end && module_present(&modules[k])) { mod = modules[k].name; break; }
    FILE *f = fopen("missing_entries.txt", "a");
    if (f) { fprintf(f, "%s %08x # ra=%08x\n", mod, a, R[31]); fclose(f); }
}

void psx_call(u32 a) {
    u32 p = a & 0x1fffffffu;
    if (p < 0x00800000u) a = 0x80000000u | (p & 0x1fffffu);
    if ((a & 0x1fffffu) == 0xa0 && (a >> 21) == (0x80000000u >> 21)) { bios_call_a(); return; }
    if ((a & 0x1fffffu) == 0xb0 && (a >> 21) == (0x80000000u >> 21)) { bios_call_b(); return; }
    if ((a & 0x1fffffu) == 0xc0 && (a >> 21) == (0x80000000u >> 21)) { bios_call_c(); return; }
    if ((a >> 16) == 0xbfc1) {
        /* Psy-Q calls kernel-internal routines located relative to B0-table
         * entries (pad/card patches). The HLE kernel has nothing to run there. */
        static int warned;
        if (warned < 8) { warned++; fprintf(stderr, "[bios] ignored call into kernel internals %08x (ra=%08x)\n", a, R[31]); }
        return;
    }
    psx_fn f = psx_lookup(a);
    if (f) { f(); return; }
    log_miss(a);
    host_fatal("No recompiled function at %08X (ra=%08X).\n"
               "Added to missing_entries.txt; append it to the module's extra_*.txt "
               "and re-run tools/regen.sh.", a, R[31]);
}

void psx_syscall(u32 pc) { (void)pc; bios_syscall(); }
void psx_break(u32 pc, u32 code) {
    fprintf(stderr, "[cpu] break %u at %08x\n", code, pc);
}
void psx_bad(u32 pc, u32 insn) { host_fatal("Invalid instruction %08X at %08X", insn, pc); }
void psx_fallthrough(u32 pc) { host_fatal("Execution fell off recompiled code at %08X", pc); }

/* ---- COP0 --------------------------------------------------------------- */
u32 cop0_read(int r) {
    switch (r) {
    case 12: return cpu.sr;
    case 13: return cpu.cause | ((i_stat & i_mask) ? 0x400 : 0);
    case 14: return cpu.epc;
    case 15: return 0x00000002;  /* PRId */
    default: return 0;
    }
}
void cop0_write(int r, u32 v) {
    if (r == 12) { cpu.sr = v; sched_kick(); }
    else if (r == 13) cpu.cause = (cpu.cause & ~0x300u) | (v & 0x300u);
}
void cop0_rfe(void) { cpu.sr = (cpu.sr & ~0xfu) | ((cpu.sr >> 2) & 0xfu); sched_kick(); }

/* ---- interrupts & events ------------------------------------------------ */
u32 i_stat, i_mask;
#define NEVER (~(u64)0)
static u64 ev_time[EV_COUNT];

static void recompute(void) {
    u64 m = NEVER;
    for (int i = 0; i < EV_COUNT; i++) if (ev_time[i] < m) m = ev_time[i];
    cpu.next_event = m;
}
void sched_kick(void) { cpu.next_event = cpu.cycles; }
void event_at(int ev, u64 when) { ev_time[ev] = when; if (when < cpu.next_event) cpu.next_event = when; }
void event_cancel(int ev) { ev_time[ev] = NEVER; }

void irq_raise(int n) { i_stat |= 1u << n; sched_kick(); }

extern int g_cpu_assist;
extern unsigned g_stat_flips;
static unsigned vb_since_flip;
void cpu_flip(void) {
    g_stat_flips++;
    vb_since_flip = 0;
    g_cpi = 2;
}
static void on_vblank(void) {
    /* CPU assist: the game draws a frame every two vblanks and slows down when a
       frame's work runs past the second one. Between the first and second vblank
       after a display flip, instructions cost half, so up to three vblanks' worth
       of work still fits in two. Only that window: at half cost the libetc VSync
       wait loop times out if it has to spin for a whole vblank. */
    g_cpi = (g_cpu_assist && ++vb_since_flip == 1) ? 1 : 2;
    event_at(EV_VBLANK, ev_time[EV_VBLANK] + CYCLES_PER_FRAME);
    gpu_vblank();
    irq_raise(IRQ_VBLANK);
    bios_vblank();
    host_frame();
}

void psx_sched(void) {
    for (;;) {
        int best = -1; u64 bt = NEVER;
        for (int i = 0; i < EV_COUNT; i++) if (ev_time[i] < bt) { bt = ev_time[i]; best = i; }
        if (best < 0 || bt > cpu.cycles) break;
        if (best == EV_VBLANK) { on_vblank(); continue; }
        ev_time[best] = NEVER;
        switch (best) {
        case EV_CD:   cd_event(); break;
        case EV_TMR0: timer_event(0); break;
        case EV_TMR1: timer_event(1); break;
        case EV_TMR2: timer_event(2); break;
        case EV_SIO:  sio_event(); break;
        }
    }
    recompute();
    if (!bios_in_exception && (i_stat & i_mask) && (cpu.sr & 0x401) == 0x401)
        bios_take_interrupt();
}

void sched_init(void) {
    for (int i = 0; i < EV_COUNT; i++) ev_time[i] = NEVER;
    ev_time[EV_VBLANK] = CYCLES_PER_FRAME;
    recompute();
}
