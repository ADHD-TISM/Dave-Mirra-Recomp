/* CD-ROM controller emulation backed by a 2352-byte/sector BIN image. */
#include "hw.h"
#include <stdlib.h>
#include <stdarg.h>

static FILE *bin;
static u32 bin_sectors;
static u32 track2_lba = 123425;   /* from the cue sheet: 27:25:50 */

static u8 idx, ie = 0x1f, ifl;   /* BIOS leaves all CD IRQs enabled */
static u8 params[16]; static int nparams;
static u8 resp[16]; static int nresp, resp_pos;
static u8 mode, stat = 0x02, filt_file, filt_chan, muted;
static u32 setloc_lba, cur_lba; static int setloc_pending;
static int reading, read_xa;
static u8 sector[2352];
static u8 data[2352]; static int data_len, data_pos;
static u8 last_hdr[8];

typedef struct { u8 type; u8 n; u8 b[12]; u32 delay; } Resp;
static Resp q[8]; static int qn;
static u64 t_resp = ~0ull, t_read = ~0ull;

static u8 bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static int unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

int cd_read_sector_raw(u32 lba, u8 *out) {
    if (!bin || lba >= bin_sectors) { memset(out, 0, 2352); return 0; }
    fseek(bin, (long)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, bin) == 2352;
}

static void resched(void) {
    u64 t = t_resp < t_read ? t_resp : t_read;
    if (t == ~0ull) event_cancel(EV_CD); else event_at(EV_CD, t);
}

static void push(u8 type, u32 delay, int n, ...) {
    if (qn >= 8) return;
    Resp *r = &q[qn++];
    r->type = type; r->n = (u8)n; r->delay = delay;
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) r->b[i] = (u8)va_arg(ap, int);
    va_end(ap);
    if (qn == 1 && ifl == 0) { t_resp = cpu.cycles + delay; resched(); }
}

static void deliver(void) {
    if (g_log_bios) fprintf(stderr, "[cd] deliver INT%d (ie=%x)\n", q[0].type, ie);
    Resp r = q[0];
    memmove(q, q + 1, sizeof(Resp) * (size_t)(--qn));
    ifl = r.type;
    memcpy(resp, r.b, r.n); nresp = r.n; resp_pos = 0;
    if (ifl & ie & 0x1f) irq_raise(IRQ_CDROM);
}

static u32 sector_period(void) { return (mode & 0x80) ? CLOCK_HZ / 150 : CLOCK_HZ / 75; }

/* ---- XA ADPCM ---------------------------------------------------------- */
static s32 xa_old[2], xa_older[2];
static const s32 K0[4] = { 0, 60, 115, 98 }, K1[4] = { 0, 0, -52, -55 };
static double xa_phase;
static s16 xa_last[2];

static void xa_decode(const u8 *raw) {
    u8 coding = raw[19];
    int stereo = (coding & 3) == 1, rate = ((coding >> 2) & 3) ? 18900 : 37800, bits8 = ((coding >> 4) & 3) == 1;
    static s16 pcm[2][4032]; int n[2] = { 0, 0 };
    const u8 *p = raw + 24;
    for (int g = 0; g < 18; g++, p += 128) {
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            u8 prm = p[4 + u];
            int shift = prm & 15, filt = (prm >> 4) & 3, ch = stereo ? (u & 1) : 0;
            for (int i = 0; i < 28; i++) {
                s32 t;
                if (bits8) t = (s32)(s8)p[16 + i * 4 + u] << 8;
                else { u8 b = p[16 + i * 4 + (u >> 1)]; int nib = (u & 1) ? b >> 4 : b & 15; t = (s32)(s16)(nib << 12); }
                t >>= shift > 12 ? 12 : shift;
                t += (xa_old[ch] * K0[filt] + xa_older[ch] * K1[filt] + 32) / 64;
                if (t > 32767) t = 32767; if (t < -32768) t = -32768;
                xa_older[ch] = xa_old[ch]; xa_old[ch] = t;
                pcm[ch][n[ch]++] = (s16)t;
            }
        }
    }
    if (!stereo) { memcpy(pcm[1], pcm[0], sizeof(s16) * (size_t)n[0]); n[1] = n[0]; }
    /* linear resample to 44100 */
    static s16 out[2 * 6000]; int o = 0;
    double step = (double)rate / 44100.0;
    int count = n[0];
    while (xa_phase < count && o < 6000) {
        int i = (int)xa_phase; double f = xa_phase - i;
        for (int ch = 0; ch < 2; ch++) {
            s16 a = i == 0 ? xa_last[ch] : pcm[ch][i - 1], b = pcm[ch][i];
            out[o * 2 + ch] = (s16)(a + (b - a) * f);
        }
        o++; xa_phase += step;
    }
    xa_phase -= count;
    xa_last[0] = pcm[0][count - 1]; xa_last[1] = pcm[1][count - 1];
    if (!muted) spu_cd_audio(out, o);
}

/* ---- reading ----------------------------------------------------------- */
static void do_read(void) {
    cd_read_sector_raw(cur_lba, sector);
    memcpy(last_hdr, sector + 12, 8);
    cur_lba++;
    t_read = cpu.cycles + sector_period();
    u8 submode = sector[18];
    if ((mode & 0x40) && (submode & 0x04)) {        /* XA audio sector */
        if (!(mode & 0x08) || (sector[16] == filt_file && sector[17] == filt_chan)) xa_decode(sector);
        return;
    }
    if ((mode & 0x08) && read_xa && (submode & 0x04)) return;
    { extern unsigned g_stat_sectors; g_stat_sectors++; }
    stat = 0x22;
    push(1, 0, 1, stat);
}

void cd_event(void) {
    if (cpu.cycles >= t_resp) {
        t_resp = ~0ull;
        if (qn && ifl == 0) deliver();
    }
    if (cpu.cycles >= t_read) {
        t_read = ~0ull;
        if (reading) {
            if (ifl == 0 && qn == 0) do_read();
            else t_read = cpu.cycles + 2000;   /* host hasn't acked yet */
        }
    }
    resched();
}

static void start_read(int xa) {
    if (setloc_pending) { cur_lba = setloc_lba; setloc_pending = 0; }
    reading = 1; read_xa = xa; stat = 0x22;
    t_read = cpu.cycles + sector_period() + 20000;
}

static void command(u8 c) {
    const u32 D = 25000;
    if (g_log_bios) fprintf(stderr, "[cd] cmd %02x (%d params) lba=%u\n", c, nparams, cur_lba);
    switch (c) {
    case 0x01: push(3, D, 1, stat); break;
    case 0x02:
        setloc_lba = (u32)((unbcd(params[0]) * 60 + unbcd(params[1])) * 75 + unbcd(params[2])) - 150;
        if (g_log_bios) fprintf(stderr, "[cd] setloc %u\n", setloc_lba);
        setloc_pending = 1; push(3, D, 1, stat); break;
    case 0x03: reading = 0; t_read = ~0ull; stat = 0x82; push(3, D, 1, stat); break;
    case 0x06: start_read(0); push(3, D, 1, stat); break;
    case 0x1b: start_read(1); push(3, D, 1, stat); break;
    case 0x07: reading = 0; t_read = ~0ull; stat = 0x02; push(3, D, 1, stat); push(2, 100000, 1, stat); break;
    case 0x08: reading = 0; t_read = ~0ull; stat = 0x02; push(3, D, 1, stat); stat = 0x00; push(2, 400000, 1, stat); break;
    case 0x09: {
        u8 s = stat; int was = reading; reading = 0; t_read = ~0ull; stat = 0x02;
        push(3, D, 1, s); push(2, was ? 1000000 : 70000, 1, stat); break;
    }
    case 0x0a: mode = 0; reading = 0; t_read = ~0ull; stat = 0x02; push(3, D, 1, stat); push(2, 100000, 1, stat); break;
    case 0x0b: muted = 1; push(3, D, 1, stat); break;
    case 0x0c: muted = 0; push(3, D, 1, stat); break;
    case 0x0d: filt_file = params[0]; filt_chan = params[1]; push(3, D, 1, stat); break;
    case 0x0e: mode = params[0]; push(3, D, 1, stat); break;
    case 0x0f: push(3, D, 5, stat, mode, 0, filt_file, filt_chan); break;
    case 0x10: push(3, D, 8, last_hdr[0], last_hdr[1], last_hdr[2], last_hdr[3],
                    last_hdr[4], last_hdr[5], last_hdr[6], last_hdr[7]); break;
    case 0x11: {
        u32 a = cur_lba + 150, r = cur_lba >= track2_lba ? cur_lba - track2_lba : cur_lba;
        int tr = cur_lba >= track2_lba ? 2 : 1;
        push(3, D, 8, bcd(tr), 1, bcd(r / 4500), bcd((r / 75) % 60), bcd(r % 75),
             bcd(a / 4500), bcd((a / 75) % 60), bcd(a % 75));
        break;
    }
    case 0x12: push(3, D, 1, stat); push(2, 100000, 1, stat); break;
    case 0x13: push(3, D, 3, stat, 1, 2); break;
    case 0x14: {
        int t = unbcd(params[0]); u32 l;
        if (t == 0) l = bin_sectors + 150; else if (t == 1) l = 150; else l = track2_lba + 150;
        push(3, D, 3, stat, bcd(l / 4500), bcd((l / 75) % 60));
        break;
    }
    case 0x15: case 0x16:
        if (setloc_pending) { cur_lba = setloc_lba; setloc_pending = 0; }
        reading = 0; t_read = ~0ull; stat = 0x42; push(3, D, 1, stat); stat = 0x02; push(2, 150000, 1, stat); break;
    case 0x19:
        if (params[0] == 0x20) push(3, D, 4, 0x94, 0x09, 0x19, 0xc0);
        else push(3, D, 1, stat);
        break;
    case 0x1a: push(3, D, 1, stat); push(2, 50000, 8, 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'); break;
    case 0x1e: push(3, D, 1, stat); push(2, 500000, 1, stat); break;
    default:
        fprintf(stderr, "[cd] unknown command %02x\n", c);
        push(5, D, 2, stat | 1, 0x40);
    }
    nparams = 0;
    resched();
}

u8 cd_read_reg(u32 r) {
    switch (r) {
    case 0: return (u8)(idx | (nparams == 0 ? 0x08 : 0) | (nparams < 16 ? 0x10 : 0) |
                        (resp_pos < nresp ? 0x20 : 0) | (data_pos < data_len ? 0x40 : 0));
    case 1: return resp_pos < nresp ? resp[resp_pos++] : 0;
    case 2: return data_pos < data_len ? data[data_pos++] : 0;
    case 3: return (idx & 1) ? (u8)(ifl | 0xe0) : (u8)(ie | 0xe0);
    }
    return 0;
}

void cd_write_reg(u32 r, u8 v) {
    if (r == 0) { idx = v & 3; return; }
    switch (r * 4 + idx) {
    case 4 + 0: command(v); break;
    case 8 + 0: if (nparams < 16) params[nparams++] = v; break;
    case 8 + 1: ie = v & 0x1f; break;
    case 12 + 0:
        if (v & 0x80) {
            if (data_pos < data_len) break;   /* already loaded: keep position */
            if (mode & 0x20) { memcpy(data, sector + 12, 2340); data_len = 2340; }
            else { memcpy(data, sector + 24, 2048); data_len = 2048; }
            data_pos = 0;
        } else { data_len = data_pos = 0; }
        break;
    case 12 + 1:
        ifl &= ~(v & 0x1f);
        if (v & 0x40) nparams = 0;
        if (ifl == 0) { nresp = resp_pos = 0; if (qn) { t_resp = cpu.cycles + (q[0].delay ? q[0].delay : 1000); resched(); } }
        break;
    default: break;   /* volume registers */
    }
}

u32 cd_dma_read(u8 *dst, u32 bytes) {
    u32 n = 0;
    while (n < bytes) { dst[n++] = data_pos < data_len ? data[data_pos++] : 0; }
    return n;
}

void cd_init(const char *path) {
    bin = fopen(path, "rb");
    if (!bin) host_fatal("Cannot open disc image:\n%s", path);
    fseek(bin, 0, SEEK_END);
    bin_sectors = (u32)(ftell(bin) / 2352);
}
