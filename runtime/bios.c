/* High-level emulation of the PS1 kernel (A0/B0/C0 tables, syscalls,
 * exception entry). No BIOS ROM is needed. */
#include "hw.h"
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int g_log_bios = 0;
int bios_in_exception;

#define V0 R[2]
#define A0 R[4]
#define A1 R[5]
#define A2 R[6]
#define A3 R[7]
#define SP R[29]
#define RA R[31]

#define KSTACK 0x8000dff0u    /* exception-handler stack in kernel RAM */
#define TBL_A  0x00000200u
#define TBL_B  0x00000874u
#define TBL_C  0x00000674u

static u32 arg(int n) { return n < 4 ? R[4 + n] : mr32(SP + 4 * n); }

/* guest strings */
static void gstr(u32 a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { char c = (char)mr8(a + i); if (!c) break; out[i] = c; }
    out[i] = 0;
}
static u32 glen(u32 a) { u32 n = 0; while (mr8(a + n)) n++; return n; }

/* ---------------------------------------------------------- events ----- */
typedef struct { u32 cls, spec, mode, func, status; } Event;
#define NEV 32
static Event ev[NEV];
#define EV_FREE 0
#define EV_DISABLED 0x1000
#define EV_ENABLED 0x2000
#define EV_READY 0x4000

static void call_guest(u32 fn, u32 a0) {
    CPU save = cpu;
    A0 = a0;
    psx_call(fn);
    u32 v0 = V0;
    u64 cyc = cpu.cycles, ne = cpu.next_event;
    cpu = save; cpu.cycles = cyc; cpu.next_event = ne;
    V0 = v0;
}

static void deliver_event(u32 cls, u32 spec) {
    for (int i = 0; i < NEV; i++) {
        Event *e = &ev[i];
        if (e->status != EV_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x2000) e->status = EV_READY;
        else if (e->mode == 0x1000 && e->func) call_guest(e->func, 0);
    }
}
static void undeliver_event(u32 cls, u32 spec) {
    for (int i = 0; i < NEV; i++)
        if (ev[i].status == EV_READY && ev[i].cls == cls && ev[i].spec == spec && ev[i].mode == 0x2000)
            ev[i].status = EV_ENABLED;
}
static Event *ev_get(u32 h) { u32 i = h & 0xffff; return ((h >> 24) == 0xf1 && i < NEV) ? &ev[i] : NULL; }

/* ------------------------------------------------------------- pads ---- */
static u32 pad_buf[2], pad_siz[2], pad_started, pad_clear = 1, pad_old_dest;
static u32 rcnt_clear[4] = { 1, 1, 1, 1 };

static void mc_flush_events(void);
void bios_vblank(void) {
    mc_flush_events();
    if (pad_started) {
        for (int p = 0; p < 2; p++) {
            if (!pad_buf[p]) continue;
            if (p == 0) {
                u16 b = host_pad_buttons(0);
                mw8(pad_buf[p] + 0, 0x00);
                mw8(pad_buf[p] + 1, 0x41);
                mw8(pad_buf[p] + 2, b & 0xff);
                mw8(pad_buf[p] + 3, b >> 8);
            } else {
                mw8(pad_buf[p] + 0, 0xff);
            }
        }
    }
    if (pad_old_dest) {
        u32 b1 = (u16)~host_pad_buttons(0);
        mw32(pad_old_dest, b1);
    }
}

/* --------------------------------------------------------- interrupts -- */
static u32 int_chain[4][8]; static int int_chain_n[4];
static u32 hook_buf;   /* HookEntryInt / SetCustomExitFromException */
static jmp_buf exc_jmp;

int bios_take_interrupt(void) {
    CPU saved = cpu;
    bios_in_exception = 1;
    cpu.epc = 0; cpu.cause = 0;
    cpu.sr = (cpu.sr & ~0x3fu) | ((cpu.sr << 2) & 0x3cu);
    if (setjmp(exc_jmp) == 0) {
        u32 pend = i_stat & i_mask;
        /* kernel's own root-counter handlers */
        if (pend & 1) { deliver_event(0xf2000003u, 2); if (rcnt_clear[3]) i_stat &= ~1u; }
        for (int t = 0; t < 3; t++)
            if (pend & (0x10u << t)) { deliver_event(0xf2000000u + t, 2); if (rcnt_clear[t]) i_stat &= ~(0x10u << t); }
        if ((pend & 1) && pad_started && pad_clear) i_stat &= ~1u;
        /* game-registered SysEnqIntRP handlers */
        for (int p = 0; p < 4; p++) {
            for (int k = 0; k < int_chain_n[p]; k++) {
                u32 node = int_chain[p][k];
                u32 f1 = mr32(node + 8), f2 = mr32(node + 4);
                if (!f1) continue;
                SP = KSTACK;
                psx_call(f1);
                if (V0 && f2) { A0 = V0; SP = KSTACK; psx_call(f2); }
            }
        }
        if (hook_buf) {
            RA = mr32(hook_buf + 0); SP = mr32(hook_buf + 4); R[30] = mr32(hook_buf + 8);
            for (int i = 0; i < 8; i++) R[16 + i] = mr32(hook_buf + 12 + 4 * i);
            R[28] = mr32(hook_buf + 44);
            V0 = 1;
            psx_call(RA);
        }
    }
    u64 cyc = cpu.cycles;
    cpu = saved;
    cpu.cycles = cyc;
    bios_in_exception = 0;
    sched_kick();
    return 1;
}

static void return_from_exception(void) {
    if (bios_in_exception) longjmp(exc_jmp, 1);
}

void bios_syscall(void) {
    switch (A0) {
    case 1: V0 = (cpu.sr & 0x401) == 0x401; cpu.sr &= ~0x401u; break;     /* EnterCriticalSection */
    case 2: cpu.sr |= 0x401; sched_kick(); break;                        /* ExitCriticalSection */
    default: break;
    }
}

/* --------------------------------------------------------------- heap -- */
static u32 heap_start, heap_end;
static void heap_init(u32 a, u32 sz) {
    heap_start = (a + 3) & ~3u; heap_end = (a + sz) & ~3u;
    mw32(heap_start, (heap_end - heap_start) | 1);   /* size | free */
}
static u32 hmalloc(u32 n) {
    n = (n + 3) & ~3u;
    for (u32 p = heap_start; p && p < heap_end;) {
        u32 h = mr32(p), sz = h & ~3u;
        if (!sz) break;
        if (h & 1) {
            /* coalesce following free blocks */
            u32 q = p + sz;
            while (q < heap_end && (mr32(q) & 1)) { u32 s2 = mr32(q) & ~3u; if (!s2) break; sz += s2; q += s2; }
            mw32(p, sz | 1);
            if (sz >= n + 4) {
                if (sz >= n + 4 + 16) { mw32(p + 4 + n, (sz - 4 - n) | 1); mw32(p, n + 4); }
                else mw32(p, sz);
                return p + 4;
            }
        }
        p += sz;
    }
    return 0;
}
static void hfree(u32 a) { if (a >= heap_start + 4 && a < heap_end) mw32(a - 4, mr32(a - 4) | 1); }

/* ------------------------------------------------------------- printf -- */
static void gprintf(u32 fmt_a, int first) {
    char fmt[512], out[2048]; int o = 0, an = first;
    gstr(fmt_a, fmt, sizeof fmt);
    for (char *p = fmt; *p && o < (int)sizeof out - 64; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char spec[32]; int s = 0; spec[s++] = *p++;
        while (*p && strchr("-+ #0123456789.lh", *p) && s < 28) spec[s++] = *p++;
        if (!*p) break;
        char c = *p; spec[s] = 0;
        char f2[40]; snprintf(f2, sizeof f2, "%s%c", spec, c);
        if (c == '%') out[o++] = '%';
        else if (c == 's') { char str[256]; gstr(arg(an++), str, sizeof str); o += snprintf(out + o, sizeof out - o, f2, str); }
        else if (c == 'c') o += snprintf(out + o, sizeof out - o, f2, (int)arg(an++));
        else if (strchr("diuxXop", c)) {
            for (char *q = f2; *q; q++) if (*q == 'l' || *q == 'h') *q = ' ';
            char f3[40]; int k = 0; for (char *q = f2; *q; q++) if (*q != ' ') f3[k++] = *q; f3[k] = 0;
            if (c == 'p') { f3[k - 1] = 'x'; }
            o += snprintf(out + o, sizeof out - o, f3, arg(an++));
        } else out[o++] = c;
        if (o >= (int)sizeof out - 64) break;
    }
    if (o > (int)sizeof out - 1) o = (int)sizeof out - 1;   /* snprintf returns the untruncated length */
    out[o] = 0;
    fputs(out, stdout); fflush(stdout);
}

/* --------------------------------------------------------- memcard ----- */
/* Each slot holds a card backed by a standard 128 KB image file (memcard1.mcd
 * and memcard2.mcd in the working directory, created formatted on first use).
 * The kernel's file API (bu00: / bu10:) and raw frame calls are served from
 * them. `mc` points at the card of the slot being worked on. */
#define MC_SIZE 131072
#define MC_BLOCK 8192
static u8 mc_data[2][MC_SIZE], *mc = mc_data[0];
static int mc_slot, mc_loaded[2], mc_new[2] = { 1, 1 }, mc_chan;
const char *g_memcard_path[2] = { "memcard1.mcd", "memcard2.mcd" };
static void mc_select(int slot) { mc_slot = slot & 1; mc = mc_data[mc_slot]; }
static struct { u32 cls, spec; } mc_ev[8]; static int mc_ev_n;
static struct { int used, slot, pos, size, mode, chain[15], nchain; } mc_fd[4];
static struct { char pat[24]; int idx, slot; } mc_dir;

/* What the card did last, for the [watchdog] and [exit] reports: the file calls with their results, each completion event as it is queued and
   as it is delivered (and to how many of the game's events), and each event that was dropped. Kept in memory and printed
   only in a report: a menu asks the card for its state all the time, so a line per record would fill the log. */
static struct { char kind; unsigned vbl; u32 a, b, c, ra; } mc_ring[256]; static unsigned mc_ring_n;   /* a write is 4 records: seek, write, event queued, event delivered */
static unsigned mc_ring_shown;   /* records up to here are in a report already */
static unsigned mc_vbl, mc_n_queued, mc_n_delivered, mc_n_dropped, mc_n_unheard; static int mc_most;
static unsigned mc_note(char kind, u32 a, u32 b, u32 c) {   /* returns the record's place, for a result that comes later */
    unsigned i = mc_ring_n++ & 255;
    mc_ring[i].kind = kind; mc_ring[i].vbl = mc_vbl; mc_ring[i].a = a; mc_ring[i].b = b; mc_ring[i].c = c; mc_ring[i].ra = RA;
    return i;
}
static void mc_queue(u32 cls, u32 spec) {
    if (mc_ev_n < 8) {
        mc_ev[mc_ev_n].cls = cls; mc_ev[mc_ev_n].spec = spec; mc_ev_n++; mc_n_queued++;
        if (mc_ev_n > mc_most) mc_most = mc_ev_n;
        mc_note('q', cls, spec, 0);
    } else {   /* the game would wait for this one for ever: say so at once, the first few times */
        mc_note('x', cls, spec, 0);
        if (mc_n_dropped++ < 5) fprintf(stderr, "[card] a completion event was dropped, 8 were waiting already: class %08x spec %04x, asked from %08x\n", cls, spec, RA);
    }
}
static void mc_flush_events(void) {
    mc_vbl++;
    for (int i = 0; i < mc_ev_n; i++) {
        /* who hears it: events the game opened for this class and spec. ready = now set for TestEvent, calls = callbacks
           run, was = set already and so not counted twice, off = opened but not enabled */
        u32 ready = 0, calls = 0, was = 0, off = 0;
        for (int k = 0; k < NEV; k++) {
            Event *e = &ev[k];
            if (e->status == EV_FREE || e->cls != mc_ev[i].cls || e->spec != mc_ev[i].spec) continue;
            if (e->status == EV_READY) was++;
            else if (e->status != EV_ENABLED) off++;
            else if (e->mode == 0x2000) ready++;
            else if (e->mode == 0x1000 && e->func) calls++;
        }
        mc_note('d', mc_ev[i].cls, mc_ev[i].spec, ready | calls << 8 | was << 16 | off << 24);
        mc_n_delivered++; if (!ready && !calls) mc_n_unheard++;
        deliver_event(mc_ev[i].cls, mc_ev[i].spec);
    }
    mc_ev_n = 0;
}
static void mc_sum(u8 *f) { u8 x = 0; for (int i = 0; i < 127; i++) x ^= f[i]; f[127] = x; }
/* move a file; replace = 0 fails when the target exists instead of replacing it */
#ifdef _WIN32
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned long flags);
static int file_move(const char *from, const char *to, int replace) { return MoveFileExA(from, to, replace ? 1 /* MOVEFILE_REPLACE_EXISTING */ : 0); }
#else
#include <unistd.h>
static int file_move(const char *from, const char *to, int replace) {
    if (!replace && access(to, F_OK) == 0) return 0;
    return rename(from, to) == 0;   /* replaces the target in one step */
}
#endif
static int mc_nosave[2];   /* set when an unrecognised card file could not be moved aside */
static void mc_save(void) {
    if (mc_nosave[mc_slot]) return;
    /* write a temporary file, then replace the card image, so a failed or interrupted write cannot destroy the old one */
    static int warned;
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", g_memcard_path[mc_slot]);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(mc, 1, MC_SIZE, f) == MC_SIZE;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = file_move(tmp, g_memcard_path[mc_slot], 1);
    if (!ok && !warned) { warned = 1; fprintf(stderr, "[card] cannot write %s: saves will not persist\n", g_memcard_path[mc_slot]); }
}
static void mc_format(void) {
    memset(mc, 0, MC_SIZE);
    mc[0] = 'M'; mc[1] = 'C'; mc_sum(mc);
    for (int i = 1; i < 16; i++) { u8 *f = mc + i * 128; f[0] = 0xa0; f[8] = f[9] = 0xff; mc_sum(f); }
    for (int i = 16; i < 36; i++) { u8 *f = mc + i * 128; memset(f, 0xff, 4); f[8] = f[9] = 0xff; mc_sum(f); }
    mc[63 * 128] = 'M'; mc[63 * 128 + 1] = 'C'; mc_sum(mc + 63 * 128);
    mc_save();
}
static void mc_open_image(void) {
    if (mc_loaded[mc_slot]) return;
    mc_loaded[mc_slot] = 1;
    FILE *f = fopen(g_memcard_path[mc_slot], "rb");
    size_t n = f ? fread(mc, 1, MC_SIZE, f) : 0;
    if (f) fclose(f);
    if (n != MC_SIZE || mc[0] != 'M' || mc[1] != 'C') {
        if (f) {   /* a file we do not recognise: keep it, never overwrite it with a blank card */
            char bad[300]; int moved = 0;
            for (int k = 0; k < 100 && !moved; k++) {   /* .bad, .bad1, .bad2 ... so an earlier .bad is never replaced */
                if (k) snprintf(bad, sizeof bad, "%s.bad%d", g_memcard_path[mc_slot], k);
                else snprintf(bad, sizeof bad, "%s.bad", g_memcard_path[mc_slot]);
                moved = file_move(g_memcard_path[mc_slot], bad, 0);   /* fails rather than replacing */
            }
            if (moved) {
                fprintf(stderr, "[card] %s is not a 128 KB memory card image; kept as %s, started a blank card\n", g_memcard_path[mc_slot], bad);
            } else {
                fprintf(stderr, "[card] %s is not a 128 KB memory card image and could not be moved aside; card will not be saved\n", g_memcard_path[mc_slot]);
                mc_nosave[mc_slot] = 1;
            }
        } else if (errno != ENOENT) {   /* exists but cannot be read: do not replace what we could not look at */
            fprintf(stderr, "[card] cannot read %s (error %d); card will not be saved\n", g_memcard_path[mc_slot], errno);
            mc_nosave[mc_slot] = 1;
        }
        mc_format();
    }
}
static u8 *mc_ent(int i) { return mc + (i + 1) * 128; }               /* directory entry 0..14 */
static u32 mc_ent_state(int i) { u8 *e = mc_ent(i); return e[0] | (u32)e[1] << 8 | (u32)e[2] << 16 | (u32)e[3] << 24; }
static u32 mc_ent_size(int i) { u8 *e = mc_ent(i); return e[4] | (u32)e[5] << 8 | (u32)e[6] << 16 | (u32)e[7] << 24; }
/* "bu00:NAME" / "bu10:NAME" -> NAME with that slot's card selected, or NULL for any other path */
static const char *mc_name(const char *path) {
    if (path[0] != 'b' || path[1] != 'u') return NULL;
    const char *c = strchr(path, ':');
    if (!c || (path[2] != '0' && path[2] != '1')) return NULL;
    mc_select(path[2] - '0');
    return c + 1;
}
static int mc_match(const char *pat, const char *name) {
    for (;; pat++, name++) {
        if (*pat == '*') return 1;
        if (*pat == 0) return *name == 0;
        if (*pat == '?') { if (!*name) return 0; continue; }
        if (*pat != *name) return 0;
    }
}
static int mc_find(const char *pat, int from) {
    for (int i = from; i < 15; i++)
        if (mc_ent_state(i) == 0x51 && mc_match(pat, (const char *)mc_ent(i) + 10)) return i;
    return -1;
}
static int mc_chain(int first, int *chain) {
    int n = 0, i = first;
    while (i >= 0 && i < 15 && n < 15) {
        chain[n++] = i;
        u8 *e = mc_ent(i); u32 nx = e[8] | (u32)e[9] << 8;
        i = nx == 0xffff ? -1 : (int)nx;
    }
    return n;
}
static u32 mc_fopen(const char *path, u32 mode) {
    const char *name = mc_name(path);
    if (!name) return 0xffffffffu;
    mc_open_image();
    int first = mc_find(name, 0);
    if (mode & 0x200) {                       /* create: block count in the high half */
        int blocks = (int)(mode >> 16); if (blocks < 1) blocks = 1;
        int freeb[15], nf = 0;
        if (first >= 0) return 0xffffffffu;
        for (int i = 0; i < 15 && nf < blocks; i++) if ((mc_ent_state(i) & 0xf0) == 0xa0) freeb[nf++] = i;
        if (nf < blocks) return 0xffffffffu;
        for (int k = 0; k < blocks; k++) {
            u8 *e = mc_ent(freeb[k]); memset(e, 0, 128);
            e[0] = k == 0 ? 0x51 : (k == blocks - 1 ? 0x53 : 0x52);
            u32 nx = k == blocks - 1 ? 0xffff : (u32)freeb[k + 1];
            e[8] = (u8)nx; e[9] = (u8)(nx >> 8);
            if (k == 0) {
                u32 sz = (u32)blocks * MC_BLOCK;
                e[4] = (u8)sz; e[5] = (u8)(sz >> 8); e[6] = (u8)(sz >> 16); e[7] = (u8)(sz >> 24);
                strncpy((char *)e + 10, name, 20);
            }
            mc_sum(e);
        }
        mc_save();
        first = freeb[0];
    }
    if (first < 0) return 0xffffffffu;
    for (int i = 0; i < 4; i++) if (!mc_fd[i].used) {
        mc_fd[i].used = 1; mc_fd[i].slot = mc_slot; mc_fd[i].pos = 0; mc_fd[i].mode = (int)mode;
        mc_fd[i].nchain = mc_chain(first, mc_fd[i].chain);
        mc_fd[i].size = mc_fd[i].nchain * MC_BLOCK;
        return (u32)(i + 4);
    }
    return 0xffffffffu;
}
static u32 mc_frw(u32 fd, u32 buf, u32 n, int wr) {
    if (fd < 4 || fd > 7 || !mc_fd[fd - 4].used) return 0xffffffffu;
    int k = (int)fd - 4;
    mc_select(mc_fd[k].slot);
    if (mc_fd[k].pos < 0 || mc_fd[k].pos > mc_fd[k].size) return 0xffffffffu;
    if (n > (u32)(mc_fd[k].size - mc_fd[k].pos)) n = (u32)(mc_fd[k].size - mc_fd[k].pos);
    for (u32 i = 0; i < n; i++) {
        int pos = mc_fd[k].pos + (int)i;
        u8 *b = mc + (mc_fd[k].chain[pos / MC_BLOCK] + 1) * MC_BLOCK + pos % MC_BLOCK;
        if (wr) *b = mr8(buf + i); else mw8(buf + i, *b);
    }
    mc_fd[k].pos += (int)n;
    if (wr) mc_save();
    if (mc_fd[k].mode & 0x8000) { mc_queue(0xf4000001u, 0x0004); return 0; }   /* O_NOWAIT */
    return n;
}
static u32 mc_fseek(u32 fd, u32 off, u32 whence) {
    if (fd < 4 || fd > 7 || !mc_fd[fd - 4].used) return 0xffffffffu;
    int k = (int)fd - 4;
    long long np = whence == 1 ? (long long)mc_fd[k].pos + (int)off : (int)off;
    mc_fd[k].pos = np < 0 ? 0 : np > mc_fd[k].size ? mc_fd[k].size : (int)np;
    return (u32)mc_fd[k].pos;
}
static u32 mc_fclose(u32 fd) {
    if (fd < 4 || fd > 7) return 0xffffffffu;
    mc_fd[fd - 4].used = 0;
    return fd;
}
static u32 mc_erase(const char *path) {
    const char *name = mc_name(path);
    if (!name) return 0;
    mc_open_image();
    int first = mc_find(name, 0), chain[15];
    if (first < 0) return 0;
    int n = mc_chain(first, chain);
    for (int k = 0; k < n; k++) { u8 *e = mc_ent(chain[k]); memset(e, 0, 128); e[0] = 0xa0; e[8] = e[9] = 0xff; mc_sum(e); }
    mc_save();
    return 1;
}
static u32 mc_dirent(u32 de, int i) {
    if (i < 0) return 0;
    u8 *e = mc_ent(i);
    for (int k = 0; k < 20; k++) mw8(de + (u32)k, e[10 + k]);
    mw32(de + 0x14, 0x50); mw32(de + 0x18, mc_ent_size(i)); mw32(de + 0x1c, 0); mw32(de + 0x20, (u32)i + 1); mw32(de + 0x24, 0);
    mc_dir.idx = i + 1;
    return de;
}
static u32 mc_firstfile(const char *path, u32 de) {
    const char *name = mc_name(path);
    if (!name) return 0;
    mc_open_image();
    strncpy(mc_dir.pat, name, sizeof mc_dir.pat - 1); mc_dir.pat[sizeof mc_dir.pat - 1] = 0;
    mc_dir.slot = mc_slot;
    return mc_dirent(de, mc_find(mc_dir.pat, 0));
}
static void card_info(u32 chan) {            /* _card_info: answered on the next vblank */
    mc_chan = (int)chan;
    mc_select((int)(chan >> 4));
    mc_open_image();
    if (mc_new[mc_slot]) { mc_new[mc_slot] = 0; mc_queue(0xf4000001u, 0x2000); }
    else mc_queue(0xf4000001u, 0x0004);
}
static void card_frame(u32 chan, u32 frame, u32 buf, int wr) {
    mc_chan = (int)chan;
    if (frame >= 1024) { mc_queue(0xf0000011u, 0x0100); return; }
    mc_select((int)(chan >> 4));
    mc_open_image();
    for (u32 i = 0; i < 128; i++) { if (wr) { if (buf) mc[frame * 128 + i] = mr8(buf + i); } else mw8(buf + i, mc[frame * 128 + i]); }
    if (wr) mc_save();
    mc_queue(0xf0000011u, 0x0004);
}

/* ------------------------------------------------------------- Exec ---- */
static void do_exec(u32 hdr, u32 p1, u32 p2) {
    u32 pc0 = mr32(hdr + 0x00), gp = mr32(hdr + 0x04);
    u32 b_addr = mr32(hdr + 0x18), b_size = mr32(hdr + 0x1c);
    u32 s_addr = mr32(hdr + 0x20), s_size = mr32(hdr + 0x24);
    for (u32 i = 0; i < b_size; i += 4) mw32(b_addr + i, 0);
    CPU save = cpu;
    if (s_addr) SP = R[30] = s_addr + s_size;
    R[28] = gp; A0 = p1; A1 = p2;
    fprintf(stderr, "[bios] Exec pc0=%08x sp=%08x\n", pc0, SP);
    psx_call(pc0);
    u64 cyc = cpu.cycles, ne = cpu.next_event; u32 sr = cpu.sr;
    cpu = save; cpu.cycles = cyc; cpu.next_event = ne; cpu.sr = sr;
    V0 = 1;
}

/* -------------------------------------------------------------- A0 ----- */
static u32 rand_seed = 0x24040001;

/* the last few BIOS calls, for the stall watchdog's report */
static struct { char tbl; u32 fn, a0, a1, a2, ra; } bios_ring[16]; static unsigned bios_ring_n;
static void bios_note(char tbl, u32 fn) {
    unsigned i = bios_ring_n++ & 15;
    bios_ring[i].tbl = tbl; bios_ring[i].fn = fn; bios_ring[i].a0 = A0; bios_ring[i].a1 = A1; bios_ring[i].a2 = A2; bios_ring[i].ra = RA;
}
/* tag: who asks, "watchdog" or "exit". all = 0: only the card records that no report has shown yet (the watchdog repeats
   its report every 10 s of a stall, and the same 256 lines each time would bury the rest of the log) */
void bios_report_stall(const char *tag, int all) {
    fprintf(stderr, "[%s] card events waiting now=%d, last BIOS calls (oldest first):\n", tag, mc_ev_n);
    for (unsigned k = 0; k < 16; k++) {
        unsigned i = (bios_ring_n + k) & 15;
        if (!bios_ring[i].tbl) continue;
        fprintf(stderr, "[%s]   %c(%02x) a0=%08x a1=%08x a2=%08x ra=%08x\n", tag, bios_ring[i].tbl, bios_ring[i].fn, bios_ring[i].a0, bios_ring[i].a1, bios_ring[i].a2, bios_ring[i].ra);
    }
    /* how many records the list below has, and how many more there were: the ring keeps the last 256 only */
    unsigned made = all ? mc_ring_n : mc_ring_n - mc_ring_shown, kept = made > 256 ? 256 : made;
    char part[40] = "";   /* said only when the list is shorter than the count */
    if (made > kept) snprintf(part, sizeof part, ", the last %u listed", kept);
    fprintf(stderr, "[%s] card events: %u queued, %u delivered (%u of them to no waiting event), %u dropped, at most %d waiting; now vblank %u. Card records %s: %u%s%s\n",
            tag, mc_n_queued, mc_n_delivered, mc_n_unheard, mc_n_dropped, mc_most, mc_vbl,
            all || !mc_ring_shown ? "since the start" : "since the report before", made, part, made ? " (oldest first):" : "");
    unsigned lines = 0;
    for (unsigned k = 0; k < 256; k++) {
        unsigned seq = mc_ring_n - 256 + k, i = seq & 255; u32 a = mc_ring[i].a, b = mc_ring[i].b, c = mc_ring[i].c;
        if (!mc_ring[i].kind) continue;   /* a slot not used yet */
        if (!all && (int)(seq - mc_ring_shown) < 0) continue;   /* in an earlier report */
        lines++;
        fprintf(stderr, "[%s]   vblank %u", tag, mc_ring[i].vbl);
        /* a delivery happens in the vblank, not in a call of the game's: it has no caller to name */
        if (mc_ring[i].kind == 'd') fprintf(stderr, ": "); else fprintf(stderr, " ra=%08x: ", mc_ring[i].ra);
        switch (mc_ring[i].kind) {
        case 'o': fprintf(stderr, "open mode %04x gave %d\n", a, (int)b); break;
        case 'c': fprintf(stderr, "close file %u gave %d\n", a, (int)b); break;
        case 's': fprintf(stderr, "seek file %u to %d (whence %u) gave %d\n", a & 0xffffffu, (int)b, a >> 24, (int)c); break;
        case 'r': case 'w': fprintf(stderr, "%s file %u, %u bytes, gave %d\n", mc_ring[i].kind == 'w' ? "write" : "read", a, b, (int)c); break;
        case 'q': fprintf(stderr, "event %08x:%04x queued\n", a, b); break;
        case 'x': fprintf(stderr, "event %08x:%04x DROPPED\n", a, b); break;
        case 'd': fprintf(stderr, "event %08x:%04x delivered: %u set ready, %u callbacks, %u were ready already, %u not enabled\n", a, b, c & 255, c >> 8 & 255, c >> 16 & 255, c >> 24); break;
        }
    }
    if (!lines) fprintf(stderr, "[%s]   (none)\n", tag);
    mc_ring_shown = mc_ring_n;
}

void bios_call_a(void) {
    u32 fn = R[9] & 0xff;
    char s1[256];
    bios_note('A', fn);
    if (g_log_bios) fprintf(stderr, "[bios] A(%02x) a0=%08x a1=%08x a2=%08x ra=%08x\n", fn, A0, A1, A2, RA);
    switch (fn) {
    case 0x0e: V0 = (s32)A0 < 0 ? -(s32)A0 : A0; break;
    case 0x0f: V0 = (s32)A0 < 0 ? -(s32)A0 : A0; break;
    case 0x10: case 0x11: gstr(A0, s1, sizeof s1); V0 = (u32)atoi(s1); break;
    case 0x13: /* setjmp */
        mw32(A0, RA); mw32(A0 + 4, SP); mw32(A0 + 8, R[30]);
        for (int i = 0; i < 8; i++) mw32(A0 + 12 + 4 * i, R[16 + i]);
        mw32(A0 + 44, R[28]); V0 = 0; break;
    case 0x14: host_fatal("longjmp() via BIOS is not supported (buf=%08X)", A0); break;
    case 0x15: { u32 d = A0 + glen(A0), s = A1; u8 c; do { c = mr8(s++); mw8(d++, c); } while (c); V0 = A0; break; }
    case 0x16: { u32 d = A0 + glen(A0), s = A1, n = A2; while (n--) { u8 c = mr8(s++); if (!c) break; mw8(d++, c); } mw8(d, 0); V0 = A0; break; }
    case 0x17: case 0x18: {
        u32 a = A0, b = A1, n = fn == 0x18 ? A2 : 0xffffffffu; s32 r = 0;
        while (n--) { u8 x = mr8(a++), y = mr8(b++); if (x != y) { r = (s32)x - (s32)y; break; } if (!x) break; }
        V0 = (u32)r; break;
    }
    case 0x19: { u32 d = A0, s = A1; u8 c; do { c = mr8(s++); mw8(d++, c); } while (c); V0 = A0; break; }
    case 0x1a: { u32 d = A0, s = A1, n = A2; u8 c = 1; while (n--) { if (c) c = mr8(s++); mw8(d++, c); } V0 = A0; break; }
    case 0x1b: V0 = glen(A0); break;
    case 0x1c: case 0x1e: { u32 a = A0; V0 = 0; for (;; a++) { u8 c = mr8(a); if (c == (u8)A1) { V0 = a; break; } if (!c) break; } break; }
    case 0x1d: case 0x1f: { u32 a = A0; V0 = 0; for (;; a++) { u8 c = mr8(a); if (c == (u8)A1) V0 = a; if (!c) break; } break; }
    case 0x24: {
        u32 h = A0, nl = glen(A1); V0 = 0;
        for (; mr8(h); h++) { u32 k = 0; while (k < nl && mr8(h + k) == mr8(A1 + k)) k++; if (k == nl) { V0 = h; break; } }
        break;
    }
    case 0x25: V0 = (u32)toupper((int)(A0 & 0xff)); break;
    case 0x26: V0 = (u32)tolower((int)(A0 & 0xff)); break;
    case 0x27: for (u32 i = 0; i < A2; i++) mw8(A1 + i, mr8(A0 + i)); break;          /* bcopy(src,dst,n) */
    case 0x28: for (u32 i = 0; i < A1; i++) mw8(A0 + i, 0); break;                    /* bzero */
    case 0x29: case 0x2d: { s32 r = 0; for (u32 i = 0; i < A2; i++) { s32 d = (s32)mr8(A0 + i) - (s32)mr8(A1 + i); if (d) { r = d; break; } } V0 = (u32)r; break; }
    case 0x2a: for (u32 i = 0; i < A2; i++) mw8(A0 + i, mr8(A1 + i)); V0 = A0; break;
    case 0x2b: for (u32 i = 0; i < A2; i++) mw8(A0 + i, (u8)A1); V0 = A0; break;
    case 0x2c:
        if (A0 < A1) for (u32 i = 0; i < A2; i++) mw8(A0 + i, mr8(A1 + i));
        else for (u32 i = A2; i-- > 0;) mw8(A0 + i, mr8(A1 + i));
        V0 = A0; break;
    case 0x2e: { V0 = 0; for (u32 i = 0; i < A2; i++) if (mr8(A0 + i) == (u8)A1) { V0 = A0 + i; break; } break; }
    case 0x2f: rand_seed = rand_seed * 0x41c64e6du + 0x3039; V0 = (rand_seed >> 16) & 0x7fff; break;
    case 0x30: rand_seed = A0; break;
    case 0x33: V0 = hmalloc(A0); break;
    case 0x34: hfree(A0); break;
    case 0x37: { u32 n = A0 * A1; V0 = hmalloc(n); for (u32 i = 0; V0 && i < n; i++) mw8(V0 + i, 0); break; }
    case 0x38: {
        u32 old = A0, n = A1, nw = hmalloc(n);
        if (old && nw) { u32 osz = (mr32(old - 4) & ~3u) - 4; for (u32 i = 0; i < osz && i < n; i++) mw8(nw + i, mr8(old + i)); hfree(old); }
        V0 = nw; break;
    }
    case 0x39: heap_init(A0, A1); break;
    case 0x3c: fputc((int)(A0 & 0xff), stdout); break;
    case 0x3e: gstr(A0, s1, sizeof s1); puts(s1); break;
    case 0x3f: gprintf(A0, 1); break;
    case 0x43: do_exec(A0, A1, A2); break;
    case 0x44: break; /* FlushCache */
    case 0x47: for (u32 i = 0; i < A2 * A3 / 2; i++) gpu_write_gp0(mr32(arg(4) + 4 * i)); break;
    case 0x48: gpu_write_gp1(A0); break;
    case 0x49: gpu_write_gp0(A0); V0 = 0; break;
    case 0x4a: for (u32 i = 0; i < A1; i++) gpu_write_gp0(mr32(A0 + 4 * i)); break;
    case 0x4d: V0 = gpu_stat(); break;
    case 0x4e: V0 = 0; break;
    case 0x55: case 0x70: case 0x71: case 0x72: break;
    case 0x9c: break;
    case 0x9d: mw32(A0, 4); mw32(A1, 16); mw32(A2, 0x801fff00u); break;
    case 0x9f: break;
    case 0xa0: host_fatal("WarmBoot called"); break;
    case 0xa1: host_fatal("SystemErrorBootOrDiskFailure(%c, %x)", (char)A0, A1); break;
    case 0xab: card_info(A0); V0 = 1; break;
    case 0xac: mc_chan = (int)A0; mc_queue(0xf4000001u, 0x0004); V0 = 1; break;
    case 0xad: break;
    case 0xb4: V0 = 0; break;
    default:
        fprintf(stderr, "[bios] unimplemented A(%02x) ra=%08x\n", fn, RA);
        V0 = 0;
    }
}

/* -------------------------------------------------------------- B0 ----- */
static void init_timer(u32 t, u32 reload, u32 flags) {
    if (t > 2) { V0 = 0; return; }
    u32 base = 0x1f801100u + t * 16;
    mw16(base + 4, 0);
    mw16(base + 8, (u16)reload);
    u32 mode = (flags & 0x10) ? 0x49 : 0x48;
    if (!(flags & 1)) mode |= 0x100;
    if (flags & 0x1000) mode |= 0x10;
    mw16(base + 4, (u16)mode);
    V0 = 1;
}

void bios_call_b(void) {
    u32 fn = R[9] & 0xff;
    if (fn != 0x0b && fn != 0x17) bios_note('B', fn);
    if (g_log_bios && fn != 0x0b) fprintf(stderr, "[bios] B(%02x) a0=%08x a1=%08x a2=%08x ra=%08x\n", fn, A0, A1, A2, RA);
    switch (fn) {
    case 0x00: V0 = hmalloc(A0); break;
    case 0x01: hfree(A0); break;
    case 0x02: init_timer(A0, A1, A2); break;
    case 0x03: V0 = A0 < 3 ? mr16(0x1f801100u + A0 * 16) : 0; break;
    case 0x04: i_mask |= A0 == 3 ? 1u : (0x10u << A0); V0 = 1; sched_kick(); break;
    case 0x05: i_mask &= ~(A0 == 3 ? 1u : (0x10u << A0)); V0 = 1; break;
    case 0x06: if (A0 < 3) mw16(0x1f801100u + A0 * 16, 0); V0 = 1; break;
    case 0x07: deliver_event(A0, A1); V0 = 1; break;
    case 0x08: {
        V0 = 0xffffffffu;
        for (int i = 0; i < NEV; i++) if (ev[i].status == EV_FREE) {
            ev[i] = (Event){ A0, A1, A2, A3, EV_DISABLED };
            V0 = 0xf1000000u | i; break;
        }
        if (g_log_bios) fprintf(stderr, "[bios]   OpenEvent(%08x,%x,%x,%08x) -> %08x\n", A0, A1, A2, A3, V0);
        break;
    }
    case 0x09: { Event *e = ev_get(A0); if (e) e->status = EV_FREE; V0 = 1; break; }
    case 0x0a: {
        Event *e = ev_get(A0); V0 = 0;
        if (!e) break;
        for (int guard = 0; e->status == EV_ENABLED && guard < 50000000; guard++) CYC(32);
        if (e->status == EV_READY) { e->status = EV_ENABLED; V0 = 1; }
        break;
    }
    case 0x0b: { Event *e = ev_get(A0); V0 = 0; if (e && e->status == EV_READY) { e->status = EV_ENABLED; V0 = 1; } break; }
    case 0x0c: { Event *e = ev_get(A0); if (e && e->status != EV_FREE) e->status = EV_ENABLED; V0 = 1; break; }
    case 0x0d: { Event *e = ev_get(A0); if (e && e->status != EV_FREE) e->status = EV_DISABLED; V0 = 1; break; }
    case 0x0e: V0 = 0xff000001u; break;   /* OpenThread: single-threaded stub */
    case 0x0f: V0 = 1; break;
    case 0x10: V0 = 1; break;
    case 0x12: pad_buf[0] = A0; pad_siz[0] = A1; pad_buf[1] = A2; pad_siz[1] = A3; V0 = 2; break;
    case 0x13: pad_started = 1; pad_clear = 1; i_mask |= 1; cpu.sr |= 0x401; sched_kick(); V0 = 1; break;
    case 0x14: pad_started = 0; break;
    case 0x15: pad_old_dest = A1; pad_started = 0; i_mask |= 1; V0 = 2; break;
    case 0x16: { u32 b = (u16)~host_pad_buttons(0); V0 = b; break; }
    case 0x17: return_from_exception(); break;
    case 0x18: hook_buf = 0; break;
    case 0x19: hook_buf = A0; break;
    case 0x20: undeliver_event(A0, A1); break;
    case 0x35:  /* write(fd, buf, n): fd 0/1 are the TTY */
        if (A0 <= 1) {   /* stdout is unbuffered (host_init): collect the bytes, so that a line is one write, not one per byte */
            char b[256]; u32 n = 0;
            for (u32 i = 0; i < A2; i++) { b[n++] = (char)mr8(A1 + i); if (n == sizeof b) { fwrite(b, 1, n, stdout); n = 0; } }
            if (n) fwrite(b, 1, n, stdout);
            fflush(stdout); V0 = A2;
        }
        else {   /* noted before the call, so that the record stands before the event the call queues; the result is filled
                    in after it, so a refused call shows too */
            u32 fd = A0, n = A2; unsigned at = mc_note('w', fd, n, 0); V0 = mc_frw(fd, A1, n, 1); mc_ring[at].c = V0;
        }
        break;
    case 0x32: { char n[64]; gstr(A0, n, sizeof n); u32 mode = A1; V0 = mc_fopen(n, mode); mc_note('o', mode, V0, 0); break; }
    case 0x33: { u32 fd = A0, off = A1, wh = A2; V0 = mc_fseek(fd, off, wh); mc_note('s', (fd & 0xffffffu) | wh << 24, off, V0); break; }
    case 0x34: { u32 fd = A0, n = A2; unsigned at = mc_note('r', fd, n, 0); V0 = mc_frw(fd, A1, n, 0); mc_ring[at].c = V0; break; }
    case 0x36: { u32 fd = A0; V0 = mc_fclose(fd); mc_note('c', fd, V0, 0); break; }
    case 0x41: { char n[64]; gstr(A0, n, sizeof n); if (mc_name(n)) { mc_open_image(); mc_format(); V0 = 1; } else V0 = 0; break; }
    case 0x42: { char n[64]; gstr(A0, n, sizeof n); V0 = mc_firstfile(n, A1); if (mc_name(n)) mc_queue(0xf0000011u, 0x0004); break; }
    case 0x43: mc_select(mc_dir.slot); V0 = mc_dirent(A0, mc_find(mc_dir.pat, mc_dir.idx)); mc_queue(0xf0000011u, 0x0004); break;
    case 0x45: { char n[64]; gstr(A0, n, sizeof n); V0 = mc_erase(n); break; }
    case 0x3d: fputc((int)(A0 & 0xff), stdout); break;
    case 0x3f: { char s[256]; gstr(A0, s, sizeof s); puts(s); break; }
    case 0x4a: case 0x4b: case 0x4c: V0 = 1; break;
    case 0x4e: card_frame(A0, A1, A2, 1); V0 = 1; break;
    case 0x4f: card_frame(A0, A1, A2, 0); V0 = 1; break;
    case 0x50: mc_new[(A0 >> 4) & 1] = 0; V0 = 1; break;
    case 0x56: V0 = TBL_C; break;
    case 0x57: V0 = TBL_B; break;
    case 0x58: V0 = (u32)mc_chan; break;
    case 0x5b: pad_clear = A0; break;
    case 0x5c: V0 = 0x01; break;
    case 0x5d: V0 = 0x01; break;
    default:
        fprintf(stderr, "[bios] unimplemented B(%02x) ra=%08x\n", fn, RA);
        V0 = 0;
    }
}

/* -------------------------------------------------------------- C0 ----- */
void bios_call_c(void) {
    u32 fn = R[9] & 0xff;
    bios_note('C', fn);
    if (g_log_bios) fprintf(stderr, "[bios] C(%02x) a0=%08x a1=%08x ra=%08x\n", fn, A0, A1, RA);
    switch (fn) {
    case 0x02: {
        u32 p = A0 & 3;
        if (int_chain_n[p] < 8) int_chain[p][int_chain_n[p]++] = A1;
        V0 = 0; break;
    }
    case 0x03: {
        u32 p = A0 & 3;
        for (int k = 0; k < int_chain_n[p]; k++) if (int_chain[p][k] == A1) {
            memmove(&int_chain[p][k], &int_chain[p][k + 1], (int_chain_n[p] - k - 1) * 4);
            int_chain_n[p]--; break;
        }
        V0 = 0; break;
    }
    case 0x0a: if (A0 < 4) { V0 = rcnt_clear[A0]; rcnt_clear[A0] = A1; } break;
    case 0x00: case 0x01: case 0x07: case 0x08: case 0x0c: case 0x12: case 0x1c: break;
    default:
        fprintf(stderr, "[bios] unimplemented C(%02x) ra=%08x\n", fn, RA);
        V0 = 0;
    }
}

void bios_init(void) {
    memset(ev, 0, sizeof ev);
    /* fake dispatch tables so GetB0Table/GetC0Table callers read sane data */
    for (u32 i = 0; i < 0xc0; i++) mw32(TBL_A + 4 * i, 0xbfc10000u + i * 4);
    for (u32 i = 0; i < 0x60; i++) mw32(TBL_B + 4 * i, 0xbfc11000u + i * 4);
    for (u32 i = 0; i < 0x20; i++) mw32(TBL_C + 4 * i, 0xbfc12000u + i * 4);
    /* kernel jump stubs at A0/B0/C0, in case something inspects them */
    mw32(0xa0, 0x3c080000u); mw32(0xb0, 0x3c080000u); mw32(0xc0, 0x3c080000u);
    /* device table (0x150/0x154) with a "bu" entry: the Psy-Q firstfile patch
       looks the memory card device up there before calling firstfile */
    mw32(0x150, 0x8000b900u); mw32(0x154, 0x50);
    mw8(0xb9f0, 'b'); mw8(0xb9f1, 'u'); mw8(0xb9f2, 0);
    mw32(0xb900, 0x8000b9f0u);
    for (u32 i = 0x10; i < 0x50; i += 4) mw32(0xb900 + i, 0xbfc13000u + i);
    heap_init(0x800c0000u, 0x10000u);
}
