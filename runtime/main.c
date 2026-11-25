/* Boot: open the disc, load the game's main executable into RAM, run the recompiled entry. */
#include "hw.h"
#include "game.h"   /* GAME_NAME, GAME_SERIAL, GAME_EXE: written into gen/ by tools/regen.py */
#include <stdlib.h>
#include <ctype.h>
#if !defined(_WIN32) || defined(DM_PTHREAD)
#include <pthread.h>
#endif

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

/* File names are case-sensitive outside Windows, and a .cue often spells its .bin differently from
   the file on disk: when the path does not open, look in its folder for the same name in another case. */
#ifdef _WIN32
static void fix_case(char *path, size_t cap) { (void)path; (void)cap; }
#else
#include <dirent.h>
static void fix_case(char *path, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    char dir[1024]; const char *slash = strrchr(path, '/'), *name = slash ? slash + 1 : path;
    if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - path + 1), path); else snprintf(dir, sizeof dir, "./");
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) != NULL;)
        if (!_stricmp(e->d_name, name)) {
            char found[1024]; snprintf(found, sizeof found, "%s%s", slash ? dir : "", e->d_name);
            snprintf(path, cap, "%s", found);
            break;
        }
    closedir(d);
}
#endif

static const char *default_images[] = {
    "game.cue", "game.bin",
    GAME_FILE " [" GAME_SERIAL "].cue",
    NULL };

/* No default name is here: look through this folder. A .cue whose name has the serial in it wins (a region tag such as
   "[U]" does not matter), then the only .cue, then the only .bin. Several of either with no serial is not a guess. */
static int has_ext(const char *n, const char *ext) { size_t l = strlen(n); return l > 4 && !_stricmp(n + l - 4, ext); }
static int has_text(const char *s, const char *sub) {
    for (size_t n = strlen(sub); *s; s++) if (!_strnicmp(s, sub, n)) return 1;
    return 0;
}
static char by_serial[1024], lone_cue[1024], lone_bin[1024];
static int n_cue, n_bin;
static void consider(const char *n) {
    if (has_ext(n, ".cue")) {
        if (!n_cue++) snprintf(lone_cue, sizeof lone_cue, "%s", n);
        if (!by_serial[0] && has_text(n, GAME_SERIAL)) snprintf(by_serial, sizeof by_serial, "%s", n);
    } else if (has_ext(n, ".bin") && !n_bin++) snprintf(lone_bin, sizeof lone_bin, "%s", n);
}
#ifdef _WIN32
#include <io.h>
static const char *find_image(void) {
    struct _finddata_t fd; intptr_t h = _findfirst("*", &fd);
    if (h != -1) { do if (!(fd.attrib & _A_SUBDIR)) consider(fd.name); while (_findnext(h, &fd) == 0); _findclose(h); }
#else
static const char *find_image(void) {
    DIR *d = opendir(".");
    if (d) { for (struct dirent *e; (e = readdir(d)) != NULL;) consider(e->d_name); closedir(d); }
#endif
    return by_serial[0] ? by_serial : n_cue == 1 ? lone_cue : !n_cue && n_bin == 1 ? lone_bin : NULL;
}

static int run(int argc, char **argv) {
    const char *img = NULL;
    int fs_given = 0;   /* a fullscreen choice on the command line */
    void host_load_config(const char *path); int host_set_option(const char *name, const char *val);
    host_load_config("dmrecomp.ini");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-bios")) g_log_bios = 1;
        else if (!strcmp(argv[i], "--console-cpu")) g_cpu_assist = 0;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) host_set_option("scale", argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) { host_set_option("fullscreen", "1"); fs_given = 1; }
        else if (!strcmp(argv[i], "--shot-window")) { extern int g_shot_window; g_shot_window = 1; }
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {   /* --set name=value: any dmrecomp.ini setting */
            char buf[128]; snprintf(buf, sizeof buf, "%s", argv[++i]);
            char *eq = strchr(buf, '='); if (eq) *eq++ = 0;
            if (!eq || !host_set_option(buf, eq)) fprintf(stderr, "--set %s: not understood\n", argv[i]);
            else if (!_stricmp(buf, "fullscreen")) fs_given = 1;   /* host_set_option takes any letter case too */
        }
        else if (!strcmp(argv[i], "--stats")) g_stats++;
        else if (!strcmp(argv[i], "--turbo")) g_turbo = 1;
        else if (!strcmp(argv[i], "--auto-start") && i + 1 < argc) g_auto_start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log-sio")) { extern int g_log_sio; g_log_sio = 1; }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { extern FILE *g_wav; g_wav = fopen(argv[++i], "wb"); }
        else if (!strcmp(argv[i], "--ramdump") && i + 1 < argc) { void host_set_ramdump(const char *); host_set_ramdump(argv[++i]); }
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) { void host_add_poke(const char *); host_add_poke(argv[++i]); }
        else if (!strcmp(argv[i], "--menu-at") && i + 1 < argc) { void host_set_menu_test(const char *); host_set_menu_test(argv[++i]); }   /* test aid: FRAME:KEYS, see host_menu.h */
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) host_set_script(argv[++i]);
        else if (!strcmp(argv[i], "--script-file") && i + 1 < argc) {   /* same, read from a file */
            FILE *sf = fopen(argv[++i], "rb"); static char sbuf[65536];
            if (sf) { size_t n = fread(sbuf, 1, sizeof sbuf - 1, sf); sbuf[n] = 0; fclose(sf); host_set_script(sbuf); }
        }
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = atoi(argv[++i]);
        else img = argv[i];
    }
    {   /* RecompLauncher's Big Picture mode (played with a controller, from the couch): full screen, unless the
           command line chose. Read once, here; without the variable nothing changes. */
        const char *m = getenv("RECOMPLAUNCHER_MODE");
        if (m && !strcmp(m, "bigpicture") && !fs_given) host_set_option("fullscreen", "1");
    }
    if (!img) for (int i = 0; default_images[i]; i++) {
        FILE *f = fopen(default_images[i], "rb");
        if (f) { fclose(f); img = default_images[i]; break; }
    }
    if (!img) img = find_image();
    host_init();
    if (!img) host_fatal("Pass the path to your " GAME_NAME " (" GAME_SERIAL ") .cue or .bin,\n"
                         "or place it next to the executable: game.cue, or the only .cue there.");
    char bin[1024]; resolve_image(img, bin, sizeof bin); fix_case(bin, sizeof bin);
    cd_init(bin);

    u32 lba, size;
    if (!iso_find_root(GAME_EXE, &lba, &size)) host_fatal(GAME_EXE " not found on disc: wrong game or region? This build is for " GAME_NAME " (" GAME_SERIAL ").");
    if (size < 0x800 || size > 0x200000) host_fatal(GAME_EXE " has an implausible size: wrong game or region?");
    u8 *exe = malloc(size + 2048);
    if (!exe) host_fatal("Out of memory.");
    for (u32 s = 0; s < (size + 2047) / 2048; s++) memcpy(exe + s * 2048, sector_data(lba + s), 2048);
    if (memcmp(exe, "PS-X EXE", 8)) host_fatal(GAME_EXE " is not a PS-X EXE");
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

/* The recompiled code keeps the game's call depth on the native stack, more than a default
   stack holds. The Windows build asks the linker for 16 MB; elsewhere the game runs on a
   thread created with that much. */
#if defined(_WIN32) && !defined(DM_PTHREAD)
int main(int argc, char **argv) { return run(argc, argv); }
#else
static struct { int argc; char **argv; } run_args;
static void *run_thread(void *unused) { (void)unused; exit(run(run_args.argc, run_args.argv)); return NULL; }
int main(int argc, char **argv) {
    pthread_attr_t a; pthread_t t;
    run_args.argc = argc; run_args.argv = argv;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (size_t)64 << 20);
    if (pthread_create(&t, &a, run_thread, NULL) != 0) { fprintf(stderr, "FATAL: cannot start the game thread\n"); return 1; }
    pthread_join(t, NULL);
    return 0;
}
#endif
