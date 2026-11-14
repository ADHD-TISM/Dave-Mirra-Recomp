/* Boot: open the disc, load SLUS_010.26 into RAM, run the recompiled entry. */
#include "hw.h"
#include <stdlib.h>
#include <ctype.h>

extern const u32 main_entry, main_base, main_end, main_sig[8];
void sched_init(void);
unsigned g_cpi = 2;
int g_cpu_assist = 1;      /* halve CPU cost for the part of a game frame that runs past one vblank */
extern int g_shot_every, g_stats, g_turbo, g_auto_start;
void host_set_script(const char *t);

static u8 sec[2352];
static const u8 *sector_data(u32 lba) { cd_read_sector_raw(lba, sec); return sec + 24; }
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

/* find a file in the ISO9660 root directory */
static int iso_find_root(const char *name, u32 *lba, u32 *size) {
    const u8 *pvd = sector_data(16);
    if (memcmp(pvd + 1, "CD001", 5)) return 0;
    u32 rl = le32(pvd + 156 + 2), rs = le32(pvd + 156 + 10);
    for (u32 s = 0; s < (rs + 2047) / 2048; s++) {
        u8 d[2048]; memcpy(d, sector_data(rl + s), 2048);
        for (u32 p = 0; p < 2048 && d[p];) {
            u8 len = d[p], nl;
            if (len < 34 || p + len > 2048) break;   /* malformed record */
            nl = d[p + 32];
            if (nl > 63 || 33u + nl > len) break;
            char n[64]; memcpy(n, d + p + 33, nl); n[nl] = 0;
            char *semi = strchr(n, ';'); if (semi) *semi = 0;
            if (!_stricmp(n, name)) { *lba = le32(d + p + 2); *size = le32(d + p + 10); return 1; }
            p += len;
        }
    }
    return 0;
}

/* resolve a .cue to its .bin, or accept a .bin directly */
static void resolve_image(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if (n > 4 && !_stricmp(in + n - 4, ".cue")) {
        FILE *f = fopen(in, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                if (!strncmp(line, "FILE", 4) && q1 && q2) {
                    *q2 = 0;
                    const char *slash = strrchr(in, '\\'), *s2 = strrchr(in, '/');
                    if (s2 > slash) slash = s2;
                    if (slash) snprintf(out, cap, "%.*s%s", (int)(slash - in + 1), in, q1 + 1);
                    else snprintf(out, cap, "%s", q1 + 1);
                    fclose(f); return;
                }
            }
            fclose(f);
        }
    }
    snprintf(out, cap, "%s", in);
}

static const char *default_images[] = {
    "game.cue", "game.bin",
    "Dave Mirra Freestyle BMX [SLUS-01026].cue",
    NULL };

int main(int argc, char **argv) {
    const char *img = NULL;
    void host_load_config(const char *path); int host_set_option(const char *name, const char *val);
    host_load_config("dmrecomp.ini");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-bios")) g_log_bios = 1;
        else if (!strcmp(argv[i], "--console-cpu")) g_cpu_assist = 0;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) host_set_option("scale", argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) host_set_option("fullscreen", "1");
        else if (!strcmp(argv[i], "--shot-window")) { extern int g_shot_window; g_shot_window = 1; }
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {   /* --set name=value: any dmrecomp.ini setting */
            char buf[128]; snprintf(buf, sizeof buf, "%s", argv[++i]);
            char *eq = strchr(buf, '='); if (eq) *eq++ = 0;
            if (!eq || !host_set_option(buf, eq)) fprintf(stderr, "--set %s: not understood\n", argv[i]);
        }
        else if (!strcmp(argv[i], "--stats")) g_stats++;
        else if (!strcmp(argv[i], "--turbo")) g_turbo = 1;
        else if (!strcmp(argv[i], "--auto-start") && i + 1 < argc) g_auto_start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log-sio")) { extern int g_log_sio; g_log_sio = 1; }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { extern FILE *g_wav; g_wav = fopen(argv[++i], "wb"); }
        else if (!strcmp(argv[i], "--ramdump") && i + 1 < argc) { void host_set_ramdump(const char *); host_set_ramdump(argv[++i]); }
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) { void host_add_poke(const char *); host_add_poke(argv[++i]); }
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) host_set_script(argv[++i]);
        else if (!strcmp(argv[i], "--script-file") && i + 1 < argc) {   /* same, read from a file */
            FILE *sf = fopen(argv[++i], "rb"); static char sbuf[65536];
            if (sf) { size_t n = fread(sbuf, 1, sizeof sbuf - 1, sf); sbuf[n] = 0; fclose(sf); host_set_script(sbuf); }
        }
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = atoi(argv[++i]);
        else img = argv[i];
    }
    if (!img) for (int i = 0; default_images[i]; i++) {
        FILE *f = fopen(default_images[i], "rb");
        if (f) { fclose(f); img = default_images[i]; break; }
    }
    host_init();
    if (!img) host_fatal("Pass the path to your Dave Mirra Freestyle BMX (SLUS-01026) .cue or .bin,\n"
                         "or place it next to the executable as game.cue / game.bin.");
    char bin[1024]; resolve_image(img, bin, sizeof bin);
    cd_init(bin);

    u32 lba, size;
    if (!iso_find_root("SLUS_010.26", &lba, &size)) host_fatal("SLUS_010.26 not found on disc: wrong game or region?");
    if (size < 0x800 || size > 0x200000) host_fatal("SLUS_010.26 has an implausible size: wrong game or region?");
    u8 *exe = malloc(size + 2048);
    if (!exe) host_fatal("Out of memory.");
    for (u32 s = 0; s < (size + 2047) / 2048; s++) memcpy(exe + s * 2048, sector_data(lba + s), 2048);
    if (memcmp(exe, "PS-X EXE", 8)) host_fatal("SLUS_010.26 is not a PS-X EXE");
    u32 pc0 = le32(exe + 0x10), gp = le32(exe + 0x14), t_addr = le32(exe + 0x18), t_size = le32(exe + 0x1c);
    u32 s_addr = le32(exe + 0x30), s_size = le32(exe + 0x34);
    if (pc0 != main_entry || t_addr != main_base) host_fatal("Disc executable does not match the recompiled build.");
    if (t_size > size - 0x800 || (t_addr & 0x1fffff) + t_size > 0x200000) host_fatal("Disc executable header is invalid.");
    memcpy(ram_ptr(t_addr), exe + 0x800, t_size);
    free(exe);
    for (int i = 0; i < 8; i++)
        if (mr32(main_base + 4 * i) != main_sig[i]) host_fatal("Disc executable contents do not match the recompiled build.");

    sched_init(); timers_init(); gpu_init(); spu_init(); bios_init();
    cpu.sr = 0x40000401u;
    R[28] = gp;
    R[29] = R[30] = s_addr ? s_addr + s_size : 0x801fff00u;
    R[31] = 0;
    fprintf(stderr, "[boot] entry %08x sp %08x\n", pc0, R[29]);
    psx_call(pc0);
    host_fatal("The game's main() returned.");
    return 0;
}
