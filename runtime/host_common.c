/* Host: settings, input mapping, presentation, screenshots and frame pacing.
   The window, OpenGL context, keys, gamepads, sound device and clock come from
   the platform file (plat_win32.c or plat_sdl.c) through host.h. */
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include "hw.h"
#include "host.h"
#include "game.h"   /* GAME_NAME: written into gen/ by tools/regen.py */
#include "gfx.h"

#ifdef _WIN32
/* A Windows laptop with two graphics chips runs a program on the integrated one unless the program asks for the other. These two
   exported variables are the drivers' documented request for the dedicated chip (NVIDIA Optimus, AMD Switchable Graphics); they
   change no system setting and do nothing on a machine with one chip. */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

static u32 *fb; static int fb_w = 640, fb_h = 480;
static double last_frame;   /* ms */
static int turbo;
int g_turbo, g_auto_start;   /* test aids: --turbo, --auto-start N */
int g_skip_intro = 1;        /* skip_intro = 1 (default): Start is pressed for the player while the movies shown at start-up play */
unsigned g_mdec_frames;      /* movie pictures decoded since start (mdec.c); never reset, unlike g_stat_mdec */
static unsigned input_frame;
#include "script_parse.h"   /* host_set_script: the --script / --script-file button script */

/* ------------------------------------------------------------ input --- */
static u16 pad_state[2] = { 0xffff, 0xffff };
static u8 pad_axes[2][4] = { { 128, 128, 128, 128 }, { 128, 128, 128, 128 } };
static int pad_found[2];      /* a gamepad is plugged in for this player */
static int log_input;         /* DM_LOG_INPUT is set (a test aid): every change of player 1's buttons is printed */
static int test_nopad;        /* DMRECOMP_GFX_TEST has "nopad": gamepads are not read, so a scripted run cannot be disturbed by one */
int g_rumble = 1, g_player2 = 1;
static int g_volume = 100;    /* volume: percent, 0 to 100 */
extern int g_pad_analog[2], g_dualshock;

static int key(int k) { return k && plat_key(k); }

/* ----------------------------------------------------------- config --- */
/* Settings come from dmrecomp.ini in the working directory (written with the
   defaults on first run), then from the command line. The defaults reproduce
   the console picture; F6/F7/F8/F11 change the display while playing. */
int g_fullscreen, g_integer_scale, g_filter, g_aspect;   /* filter: 0 nearest, 1 smooth; aspect: 0 = 4:3, 1 = stretch */
int g_scale_auto = 1;   /* step the internal resolution down when the machine cannot keep up */
int g_renderer = 2;                                       /* 0 = gl, 1 = vulkan, 2 = auto (default): see host_init; a backend that cannot start falls back */
int g_upscale, g_fsr_sharp = 80;                          /* upscale: 0 = plain, 1 = FSR 1; sharpness 0..100 */
static int win_w = 960, win_h = 720;                      /* window_size */
extern int g_ws_kq; extern unsigned g_gte_rtp;
static double ws_ratio;   /* widescreen: 0 = off, else the width/height of the picture */
static int ws_on;        /* the current frames are 3D, so the wide shape applies */
static int fs_w, fs_h;                                    /* fullscreen_size; 0 = the desktop's own resolution */
extern int g_cpu_assist;
/* up to MAXKEYS keys per pad button, indexed by the pad's button bit; a 0 ends the list */
#define MAXKEYS 4
static int keymap[16][MAXKEYS] = {
    [4] = { HK_UP }, [5] = { HK_RIGHT }, [6] = { HK_DOWN }, [7] = { HK_LEFT },
    [3] = { HK_ENTER }, [0] = { HK_BACKSPACE, HK_RSHIFT },
    [14] = { 'Z', 'K' }, [13] = { 'X', 'L' }, [15] = { 'A', 'J' }, [12] = { 'S', 'I' },
    [10] = { 'Q' }, [11] = { 'W' }, [8] = { '1' }, [9] = { '3' } };
static int key_turbo = HK_TAB;
/* keys for the display actions, indexed by the HOT_ numbers in host.h */
static int hotkeys[6] = { HK_F11, HK_F6, HK_F7, HK_F8, HK_F9, HK_F10 };
static const char *const hotkey_opts[6] = { "key_fullscreen", "key_scale", "key_filter", "key_aspect", "key_fsr", "key_widescreen" };
static int key_menu = HK_ESCAPE;   /* opens the in-game menu (host_menu.h) */
static int pad_menu[2];             /* and so do these gamepad controls of player 1, held together (X_ numbers; the second may be none):
                                       pad_menu, Back + Start unless changed. Set in host_set_option and before the settings are read. */
static int menu_want;               /* the menu was asked for: host_frame opens it */
static int menu_capture;            /* present() must leave the game's picture in fb, also when the graphics card holds it */
static char config_path[512];       /* the settings file host_load_config read: where the menu writes a changed setting */
/* The mouse. Each button and wheel direction presses a pad button (its bit, or -1 for none). Pointer movement does nothing. */
enum { MS_LEFT, MS_RIGHT, MS_MIDDLE, MS_X1, MS_X2, MS_WHEEL_UP, MS_WHEEL_DOWN, MS_COUNT };
static int mousemap[MS_COUNT] = { 14, 12, 3, -1, -1, 4, 6 };   /* left = cross, right = triangle (back), middle = start, wheel = up/down */
static const char *const mouse_opts[MS_COUNT] = { "mouse_left", "mouse_right", "mouse_middle", "mouse_x1", "mouse_x2", "wheel_up", "wheel_down" };
static const struct { const char *name; int bit; } ps_names[] = {
    { "up", 4 }, { "right", 5 }, { "down", 6 }, { "left", 7 }, { "start", 3 }, { "select", 0 },
    { "cross", 14 }, { "circle", 13 }, { "square", 15 }, { "triangle", 12 },
    { "l1", 10 }, { "r1", 11 }, { "l2", 8 }, { "r2", 9 }, { "l3", 1 }, { "r3", 2 }, { "none", -1 }, { NULL, 0 } };
static int mouse_held;   /* the buttons down at the last poll (for the input log) */
/* which gamepad control presses each pad button, indexed by the pad's button bit */
enum { X_NONE, X_A, X_B, X_X, X_Y, X_LB, X_RB, X_LT, X_RT, X_START, X_BACK, X_LS, X_RS, X_UP, X_DOWN, X_LEFT, X_RIGHT };
static const char *const xnames[] = { "none", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "back", "ls", "rs", "up", "down", "left", "right" };
static int padmap[16] = {
    [4] = X_UP, [5] = X_RIGHT, [6] = X_DOWN, [7] = X_LEFT, [3] = X_START, [0] = X_BACK, [1] = X_LS, [2] = X_RS,
    [14] = X_A, [13] = X_B, [15] = X_X, [12] = X_Y, [10] = X_LB, [11] = X_RB, [8] = X_LT, [9] = X_RT };
static const struct { const char *name; int bit; } pad_opts[] = {
    { "pad_up", 4 }, { "pad_right", 5 }, { "pad_down", 6 }, { "pad_left", 7 }, { "pad_start", 3 }, { "pad_select", 0 },
    { "pad_cross", 14 }, { "pad_circle", 13 }, { "pad_square", 15 }, { "pad_triangle", 12 },
    { "pad_l1", 10 }, { "pad_r1", 11 }, { "pad_l2", 8 }, { "pad_r2", 9 }, { "pad_l3", 1 }, { "pad_r3", 2 }, { NULL, 0 } };
static const struct { const char *name; int bit; } key_opts[] = {
    { "key_up", 4 }, { "key_right", 5 }, { "key_down", 6 }, { "key_left", 7 }, { "key_start", 3 }, { "key_select", 0 },
    { "key_cross", 14 }, { "key_circle", 13 }, { "key_square", 15 }, { "key_triangle", 12 },
    { "key_l1", 10 }, { "key_r1", 11 }, { "key_l2", 8 }, { "key_r2", 9 }, { NULL, 0 } };
static const struct { const char *name; int vk; } key_names[] = {
    { "up", HK_UP }, { "down", HK_DOWN }, { "left", HK_LEFT }, { "right", HK_RIGHT }, { "enter", HK_ENTER },
    { "backspace", HK_BACKSPACE }, { "tab", HK_TAB }, { "space", HK_SPACE }, { "lshift", HK_LSHIFT }, { "rshift", HK_RSHIFT },
    { "lctrl", HK_LCTRL }, { "rctrl", HK_RCTRL }, { "lalt", HK_LALT }, { "ralt", HK_RALT },
    { "comma", HK_COMMA }, { "period", HK_PERIOD }, { "slash", HK_SLASH }, { "semicolon", HK_SEMICOLON },
    { "num0", HK_NUM0 }, { "num1", HK_NUM1 }, { "num2", HK_NUM2 }, { "num3", HK_NUM3 }, { "num4", HK_NUM4 },
    { "num5", HK_NUM5 }, { "num6", HK_NUM6 }, { "num7", HK_NUM7 }, { "num8", HK_NUM8 }, { "num9", HK_NUM9 },
    { "escape", HK_ESCAPE }, { "insert", HK_INSERT }, { "delete", HK_DELETE }, { "home", HK_HOME }, { "end", HK_END },
    { "pageup", HK_PAGEUP }, { "pagedown", HK_PAGEDOWN }, { "capslock", HK_CAPSLOCK },
    { "minus", HK_MINUS }, { "equals", HK_EQUALS }, { "lbracket", HK_LBRACKET }, { "rbracket", HK_RBRACKET },
    { "backslash", HK_BACKSLASH }, { "quote", HK_QUOTE }, { "grave", HK_GRAVE },
    { "numadd", HK_NUMADD }, { "numsub", HK_NUMSUB }, { "nummul", HK_NUMMUL }, { "numdiv", HK_NUMDIV },
    { "numdot", HK_NUMDOT }, { "numenter", HK_NUMENTER },
    { "f1", HK_F1 }, { "f2", HK_F2 }, { "f3", HK_F3 }, { "f4", HK_F4 }, { "f5", HK_F5 }, { "f6", HK_F6 },
    { "f7", HK_F7 }, { "f8", HK_F8 }, { "f9", HK_F9 }, { "f10", HK_F10 }, { "f11", HK_F11 }, { "f12", HK_F12 },
    { "none", 0 }, { NULL, 0 } };
static int key_parse(const char *s) {
    if (s[0] && !s[1]) { int c = toupper((unsigned char)s[0]); if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; }
    for (int i = 0; key_names[i].name; i++) if (!_stricmp(key_names[i].name, s)) return key_names[i].vk;
    return -1;
}
static const char *key_label(int vk) {
    static char one[2];
    for (int i = 0; key_names[i].name; i++) if (key_names[i].vk == vk) return key_names[i].name;
    one[0] = (char)vk; return one;
}
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
/* one "name = value" setting; returns 0 when the name or value is not understood */
int host_set_option(const char *name, const char *val) {
    if (!_stricmp(name, "scale")) { int v = atoi(val); if (v != 1 && v != 2 && v != 4) return 0; g_gpu_scale = v; return 1; }
    if (!_stricmp(name, "renderer")) { if (!_stricmp(val, "gl")) g_renderer = 0; else if (!_stricmp(val, "vulkan")) g_renderer = 1; else if (!_stricmp(val, "auto")) g_renderer = 2; else return 0; return 1; }
    if (!_stricmp(name, "gpu_gl")) { g_gpu_gl = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "scale_auto")) { g_scale_auto = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "filter")) { if (!_stricmp(val, "nearest")) g_filter = 0; else if (!_stricmp(val, "smooth")) g_filter = 1; else return 0; return 1; }
    if (!_stricmp(name, "aspect")) { if (!_stricmp(val, "4:3")) g_aspect = 0; else if (!_stricmp(val, "stretch")) g_aspect = 1; else return 0; return 1; }
    if (!_stricmp(name, "window_size") || !_stricmp(name, "fullscreen_size")) {
        int fs = !_stricmp(name, "fullscreen_size"), a = 0, b = 0;
        if (!_stricmp(val, fs ? "native" : "auto")) { a = fs ? 0 : 960; b = fs ? 0 : 720; }
        else if (sscanf(val, "%dx%d", &a, &b) != 2 || a < 320 || b < 240 || a > 16384 || b > 16384) return 0;
        if (fs) { fs_w = a; fs_h = b; } else { win_w = a; win_h = b; }
        return 1;
    }
    if (!_stricmp(name, "widescreen")) {
        int a, b;
        if (!_stricmp(val, "off")) ws_ratio = 0;
        else if (sscanf(val, "%d:%d", &a, &b) == 2 && a > 0 && b > 0 && a * 3 > b * 4 && a < b * 6) ws_ratio = (double)a / b;
        else return 0;
        return 1;
    }
    if (!_stricmp(name, "upscale")) { if (!_stricmp(val, "off")) g_upscale = 0; else if (!_stricmp(val, "fsr")) g_upscale = 1; else return 0; return 1; }
    if (!_stricmp(name, "fsr_sharpness")) { int v = atoi(val); if (v < 0 || v > 100) return 0; g_fsr_sharp = v; return 1; }
    if (!_stricmp(name, "integer_scale")) { g_integer_scale = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "fullscreen")) { g_fullscreen = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "cpu_assist")) { g_cpu_assist = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "skip_intro")) { g_skip_intro = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "rumble")) { g_rumble = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "volume")) { char *e; long v = strtol(val, &e, 10); if (e == val || *e || v < 0 || v > 100) return 0; g_volume = (int)v; return 1; }
    if (!_stricmp(name, "dualshock")) { g_dualshock = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "player2")) { g_player2 = atoi(val); return 1; }   /* 2 = always present (tests) */
    if (!_stricmp(name, "reverb")) { extern int g_reverb; g_reverb = atoi(val) != 0; return 1; }
    for (int i = 0; pad_opts[i].name; i++) if (!_stricmp(pad_opts[i].name, name)) {
        for (int k = 0; k < (int)(sizeof xnames / sizeof *xnames); k++)
            if (!_stricmp(xnames[k], val)) { padmap[pad_opts[i].bit] = k; return 1; }
        return 0;
    }
    if (!_stricmp(name, "key_fast_forward")) { int vk = key_parse(val); if (vk < 0) return 0; key_turbo = vk; return 1; }
    if (!_stricmp(name, "key_menu")) { int vk = key_parse(val); if (vk < 0) return 0; key_menu = vk; return 1; }
    if (!_stricmp(name, "pad_menu")) {   /* one control, or two joined by +: both must be held; none = the gamepad does not open the menu */
        char buf[32]; snprintf(buf, sizeof buf, "%s", val);
        char *plus = strchr(buf, '+'); if (plus) *plus++ = 0;
        int got[2] = { -1, plus ? -1 : X_NONE };
        for (int k = 0; k < (int)(sizeof xnames / sizeof *xnames); k++) {
            if (!_stricmp(xnames[k], trim(buf))) got[0] = k;
            if (plus && !_stricmp(xnames[k], trim(plus))) got[1] = k;
        }
        if (got[0] < 0 || got[1] < 0 || (plus && (got[0] == X_NONE || got[1] == X_NONE || got[0] == got[1]))) return 0;
        pad_menu[0] = got[0]; pad_menu[1] = got[1];
        return 1;
    }
    for (int i = 0; i < 6; i++) if (!_stricmp(hotkey_opts[i], name)) { int vk = key_parse(val); if (vk < 0) return 0; hotkeys[i] = vk; return 1; }
    for (int i = 0; key_opts[i].name; i++) if (!_stricmp(key_opts[i].name, name)) {   /* up to MAXKEYS keys, separated by commas */
        char buf[128]; snprintf(buf, sizeof buf, "%s", val);
        int list[MAXKEYS] = { 0 }, n = 0;
        for (char *tok = buf, *next; tok; tok = next) {
            next = strchr(tok, ','); if (next) *next++ = 0;
            int k = key_parse(trim(tok));
            if (k < 0 || n >= MAXKEYS) return 0;
            if (k) list[n++] = k;   /* "none" adds nothing */
        }
        memcpy(keymap[key_opts[i].bit], list, sizeof list);
        return 1;
    }
    for (int i = 0; i < MS_COUNT; i++) if (!_stricmp(mouse_opts[i], name)) {
        for (int k = 0; ps_names[k].name; k++) if (!_stricmp(ps_names[k].name, val)) { mousemap[i] = ps_names[k].bit; return 1; }
        return 0;
    }
    return 0;
}
static void config_write_default(const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "# dmrecomp settings. Delete this file to get the defaults back.\n"
               "# The defaults give the original console picture.\n\n"
               "# Internal 3D resolution: 1 = original, 2 or 4 = sharper (F6 cycles while playing)\n"
               "scale = 1\n"
               "# 1 = drop to a lower scale by itself when the game cannot hold full speed at 2 or 4\n"
               "scale_auto = 1\n"
               "# Graphics API that shows the picture: gl = OpenGL, vulkan = Vulkan first (falls back to OpenGL),\n"
               "# auto = Vulkan when it can do everything asked (with gpu_gl = 1 that includes drawing on the graphics card), else OpenGL\n"
               "renderer = auto\n"
               "# 1 = at scale 2 or 4, draw the picture on the graphics card instead of the processor (early prototype)\n"
               "gpu_gl = 0\n"
               "# How the picture is enlarged to the window: nearest = sharp pixels, smooth = blended (F7)\n"
               "filter = nearest\n"
               "# 4:3 = console shape with black bars, stretch = fill the window (F8)\n"
               "aspect = 4:3\n"
               "# 1 = only enlarge by whole multiples of 240 lines\n"
               "integer_scale = 0\n"
               "# 1 = start in fullscreen (F11 or Alt+Enter toggles)\n"
               "fullscreen = 0\n"
               "# Window size in pixels (WxH), e.g. 1280x960. auto = 960x720\n"
               "window_size = auto\n"
               "# Fullscreen resolution (WxH) or native = the desktop's own resolution\n"
               "fullscreen_size = native\n"
               "# Widescreen (experimental): off, or the shape such as 16:9, 16:10, 21:9. Shows more scenery at the sides in 3D scenes (F10 cycles)\n"
               "widescreen = off\n"
               "# off = plain, fsr = AMD FidelityFX Super Resolution 1 (F9 toggles); fsr_sharpness 0..100\n"
               "upscale = off\n"
               "fsr_sharpness = 80\n"
               "# 1 = hold 30 fps in busy scenes, 0 = console CPU timing with its slowdowns\n"
               "cpu_assist = 1\n"
               "# 1 = skip the movies shown when the game starts (the publisher, the developer and the intro), 0 = show them\n"
               "skip_intro = 1\n"
               "# Sound volume in percent, 0 to 100 (also in the menu while playing)\n"
               "volume = 100\n"
               "# 1 = echo effect the console adds in levels, 0 = dry sound\n"
               "reverb = 1\n\n"
               "# Controllers. dualshock = 0 makes the game see a plain digital pad (no sticks, no rumble).\n"
               "dualshock = 1\n"
               "rumble = 1\n"
               "# 1 = a second gamepad, when plugged in, is player 2\n"
               "player2 = 1\n"
               "# Gamepad buttons (both players): a b x y lb rb lt rt start back ls rs up down left right none\n");
    for (int i = 0; pad_opts[i].name; i++) fprintf(f, "%s = %s\n", pad_opts[i].name, xnames[padmap[pad_opts[i].bit]]);
    fprintf(f, "\n"
               "# Keyboard. Up to %d keys per button, separated by commas: letters, digits, or\n"
               "# up down left right enter backspace tab space lshift rshift lctrl rctrl lalt ralt\n"
               "# comma period slash semicolon minus equals lbracket rbracket backslash quote grave\n"
               "# escape insert delete home end pageup pagedown capslock f1..f12 num0..num9\n"
               "# numadd numsub nummul numdiv numdot numenter none\n", MAXKEYS);
    for (int i = 0; key_opts[i].name; i++) {
        const int *k = keymap[key_opts[i].bit];
        fprintf(f, "%s = ", key_opts[i].name);
        if (!k[0]) fprintf(f, "none");
        for (int j = 0; j < MAXKEYS && k[j]; j++) fprintf(f, "%s%s", j ? ", " : "", key_label(k[j]));
        fprintf(f, "\n");
    }
    fprintf(f, "key_fast_forward = %s\n", key_label(key_turbo));
    fprintf(f, "# Opens the menu while playing: display settings, volume and Exit\n"
               "key_menu = %s\n", key_label(key_menu));
    fprintf(f, "# The same on player 1's gamepad: one control, or two held together joined by + (back+start,\n"
               "# ls+rs = both sticks pressed, lb+rb ...), or none. Of two, the game can get the one pressed first.\n"
               "pad_menu = %s%s%s\n", xnames[pad_menu[0]], pad_menu[1] ? "+" : "", pad_menu[1] ? xnames[pad_menu[1]] : "");
    fprintf(f, "\n# Keys for the display actions (one key each, or none)\n");
    for (int i = 0; i < 6; i++) fprintf(f, "%s = %s\n", hotkey_opts[i], key_label(hotkeys[i]));
    fprintf(f, "\n# Mouse. Each button and wheel direction presses a pad button: up right down left start select\n"
               "# cross circle square triangle l1 r1 l2 r2 l3 r3 none\n");
    for (int i = 0; i < MS_COUNT; i++) {
        const char *nm = "none";
        for (int k = 0; ps_names[k].name; k++) if (ps_names[k].bit == mousemap[i]) { nm = ps_names[k].name; break; }
        fprintf(f, "%s = %s\n", mouse_opts[i], nm);
    }
    fclose(f);
}
void host_load_config(const char *path) {
    snprintf(config_path, sizeof config_path, "%s", path);
    pad_menu[0] = X_BACK; pad_menu[1] = X_START;   /* the default, before the file and the command line */
    FILE *f = fopen(path, "r");
    if (!f) { config_write_default(path); return; }
    char line[256]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == '#' || *s == ';') continue;
        if (eq) { *eq = 0; if (host_set_option(trim(s), trim(eq + 1))) continue; }
        fprintf(stderr, "%s line %d: not understood\n", path, n);
    }
    fclose(f);
}

static int xsrc(const HostPad *p, int s) {
    switch (s) {
    case X_A: return p->buttons & HP_A; case X_B: return p->buttons & HP_B;
    case X_X: return p->buttons & HP_X; case X_Y: return p->buttons & HP_Y;
    case X_LB: return p->buttons & HP_LB; case X_RB: return p->buttons & HP_RB;
    case X_LT: return p->lt > 100; case X_RT: return p->rt > 100;
    case X_START: return p->buttons & HP_START; case X_BACK: return p->buttons & HP_BACK;
    case X_LS: return p->buttons & HP_LS; case X_RS: return p->buttons & HP_RS;
    case X_UP: return p->buttons & HP_UP; case X_DOWN: return p->buttons & HP_DOWN;
    case X_LEFT: return p->buttons & HP_LEFT; case X_RIGHT: return p->buttons & HP_RIGHT;
    }
    return 0;
}
/* the gamepad controls that open the in-game menu are held */
static int pad_menu_held(const HostPad *p) { return pad_menu[0] && xsrc(p, pad_menu[0]) && (!pad_menu[1] || xsrc(p, pad_menu[1])); }
static u8 axis(int v, int flip) { v = (flip ? -v : v) / 256 + 128; return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static int wheel_waiting[2];   /* wheel notches (up, down) not yet turned into a press */
static int mouse_clicks_own;   /* the in-game menu reads the buttons itself: they are not turned into pad buttons */
/* mouse -> pad buttons for player 1 (added to b, active-high) */
static void mouse_input(u16 *b) {
    int mb, wheel;
    plat_mouse(&mb, &wheel);
    if (test_nopad) mb = wheel = 0;   /* a scripted run: a pointer over the window must not disturb it */
    mouse_held = mb;
    for (int i = 0; i < 5; i++) if (!mouse_clicks_own && (mb & (1 << i)) && mousemap[i] >= 0) *b |= (u16)(1 << mousemap[i]);
    /* Each notch is a short press of its own: 6 frames down, then 4 up, so that the game sees every one. Notches that
       come faster than that wait their turn (8 at most). side: the press was started with left Shift held, and goes
       left or right instead. */
    static int hold[2], gap[2], side[2];
    if (test_nopad || !plat_focused()) wheel_waiting[0] = wheel_waiting[1] = 0;   /* notches not yet pressed do not outlive the focus */
    if (wheel > 0) wheel_waiting[0] += wheel; else if (wheel < 0) wheel_waiting[1] -= wheel;
    for (int i = 0; i < 2; i++) {
        if (wheel_waiting[i] > 8) wheel_waiting[i] = 8;
        if (!hold[i] && !gap[i] && wheel_waiting[i]) { wheel_waiting[i]--; hold[i] = 6; gap[i] = 4; side[i] = key(HK_LSHIFT); }
        int bit = side[i] ? (i ? 5 : 7) : mousemap[MS_WHEEL_UP + i];   /* Shift + wheel: up = left, down = right */
        if (hold[i]) { if (bit >= 0) *b |= (u16)(1 << bit); hold[i]--; }
        else if (gap[i]) gap[i]--;
    }
}

static void poll_input(void) {
    static unsigned probe;
    u16 b[2] = { 0, 0 };   /* active-high while building */
    for (int i = 0; i < 16; i++) for (int j = 0; j < MAXKEYS && keymap[i][j]; j++) if (key(keymap[i][j])) { b[0] |= (u16)(1 << i); break; }
    turbo = key(key_turbo);
    static int hot_down[6];   /* a display key acts once when it goes down */
    for (int i = 0; i < 6; i++) { int d = key(hotkeys[i]); if (d && !hot_down[i]) host_hotkey(i); hot_down[i] = d; }
    int menu_held = key(key_menu);   /* the menu key, or pad_menu on player 1's gamepad (below) */
    mouse_input(&b[0]);
    /* looking for a controller that is not there is slow, so only retry once a second */
    int look = (probe++ % 60) == 0;
    for (int n = 0; n < 2; n++) {
        HostPad s;
        memset(pad_axes[n], 128, 4);
        if (test_nopad || (n == 1 && !g_player2)) { pad_found[n] = 0; continue; }
        if (!pad_found[n] && !look) continue;
        pad_found[n] = plat_pad(n, &s);
        if (!pad_found[n]) continue;
        for (int i = 0; i < 16; i++) if (xsrc(&s, padmap[i])) b[n] |= (u16)(1 << i);
        if (n == 0 && pad_menu_held(&s) && plat_focused()) menu_held = 1;   /* a gamepad is read without the focus too */
        if (g_pad_analog[n]) {
            pad_axes[n][0] = axis(s.rx, 0); pad_axes[n][1] = axis(s.ry, 1);
            pad_axes[n][2] = axis(s.lx, 0); pad_axes[n][3] = axis(s.ly, 1);
        } else {   /* digital pad: the left stick works the d-pad */
            if (s.ly > 16000) b[n] |= 1 << 4;
            if (s.ly < -16000) b[n] |= 1 << 6;
            if (s.lx < -16000) b[n] |= 1 << 7;
            if (s.lx > 16000) b[n] |= 1 << 5;
        }
    }
    { static int menu_down; if (menu_held && !menu_down) menu_want = 1; menu_down = menu_held; }   /* once per press */
    input_frame++;
    /* skip_intro: the game's movie player ends a movie when Start is pressed, and the movies shown at start-up follow each
       other without a pause. So Start is pressed in short pulses while movie pictures keep coming, and the aid switches itself
       off for the rest of the session once no picture has come for 5 seconds (the menu is up), or when no movie has started in
       the first minute. A movie watched later, from the menus, is left alone. It stays out of a run that uses --auto-start
       or a button script: those press Start themselves, and their frame numbers were chosen without it. */
    if (g_skip_intro && !g_auto_start && !script_n) {
        static unsigned seen, last_at; static int state;   /* 0 = waiting for the first movie, 1 = in the movies, 2 = done */
        if (state < 2 && g_mdec_frames != seen) { seen = g_mdec_frames; last_at = input_frame; state = 1; }
        if (state == 1) {
            if (input_frame - last_at > 300) state = 2;
            else if (input_frame - last_at < 12 && input_frame % 16 < 4) b[0] |= 1 << 3;
        } else if (state == 0 && input_frame > 3600) state = 2;
    }
    if (g_auto_start && (!script_n || input_frame < script[0].frame) &&
        input_frame % (unsigned)g_auto_start < 4) b[0] |= 1 << 3;
    for (int i = 0; i < script_n; i++)
        if (input_frame >= script[i].frame && input_frame < script[i].frame + 6) {
            b[0] |= script[i].mask;
            if (g_player2 == 2) b[1] |= script[i].mask;   /* test mode: player 2 presses the same buttons */
        }
    if (g_turbo) turbo = 1;
    pad_state[0] = (u16)~b[0]; pad_state[1] = (u16)~b[1];
    static u16 logged;
    if (log_input && b[0] != logged) { logged = b[0]; fprintf(stderr, "[input] frame %u buttons %04x mouse %x\n", input_frame, b[0], mouse_held); }
}
u16 host_pad_buttons(int port) { return port >= 0 && port < 2 ? pad_state[port] : 0xffff; }
int host_pad_connected(int port) { return port == 0 || (port == 1 && (pad_found[1] || g_player2 == 2)); }   /* player 1 always has the keyboard */
void host_pad_read(int port, u16 *buttons, u8 axes[4]) { *buttons = pad_state[port & 1]; memcpy(axes, pad_axes[port & 1], 4); }
void host_pad_rumble(int port, int small_on, int large) {
    static int last[2] = { -1, -1 };
    int v = g_rumble && !turbo ? ((small_on ? 1 : 0) << 8) | (large & 0xff) : 0;
    if (port < 0 || port > 1 || v == last[port] || !pad_found[port]) return;
    last[port] = v;
    plat_pad_rumble(port, (u16)((v & 0xff) * 257), (u16)((v >> 8) ? 0xffff : 0));   /* large motor, small motor */
}

/* ------------------------------------------------------------ audio --- */
FILE *g_wav;   /* --wav file: raw 44.1 kHz stereo s16 dump of everything mixed */
void host_audio_push(const s16 *lr, int frames) {
    if (g_wav) fwrite(lr, 4, (size_t)frames, g_wav);
    if (turbo) return;
    if (g_volume != 100) {   /* the volume setting: what goes to the sound device, not the --wav dump above */
        static s16 quiet[4096 * 2];
        if (frames > 4096) frames = 4096;
        for (int i = 0; i < frames * 2; i++) quiet[i] = (s16)(lr[i] * g_volume / 100);
        lr = quiet;
    }
    plat_audio_push(lr, frames);
}

/* ------------------------------------------------------------ video --- */
static void set_fullscreen(int on) { plat_set_fullscreen(on, fs_w, fs_h); g_fullscreen = on; }
static void show_settings(void) {
    char t[160];
    snprintf(t, sizeof t, GAME_NAME " (static recomp) - %dx resolution, %s, %s%s",
             g_gpu_scale, g_filter ? "smooth" : "sharp", g_aspect ? "stretched" : "4:3", g_upscale ? ", FSR" : "");
    if (ws_ratio > 0) { char *e = t + strlen(t); snprintf(e, sizeof t - (size_t)(e - t), ", widescreen %.2f:1", ws_ratio); }
    plat_set_title(t);
}
void host_hotkey(int which) {
    switch (which) {
    case HOT_FULLSCREEN: set_fullscreen(!g_fullscreen); return;
    case HOT_SCALE: g_gpu_scale = g_gpu_scale == 1 ? 2 : g_gpu_scale == 2 ? 4 : 1; gpu_set_scale(); break;
    case HOT_FILTER: g_filter = !g_filter; break;
    case HOT_ASPECT: g_aspect = !g_aspect; break;
    case HOT_WIDE: ws_ratio = ws_ratio == 0 ? 16.0 / 9 : ws_ratio < 2 ? 21.0 / 9 : 0; break;
    case HOT_FSR: g_upscale = !g_upscale; break;
    }
    show_settings();
}
/* The guest's return addresses on its stack, for the reports below: up to max words that point into the code below hi.
   The words are read from memory directly, and only up to the end of the area sp is in. mr32 must not be used here: some
   functions keep their stack at the end of the scratchpad (sp = 1f8003e4), and a walk with mr32 runs on from there into
   the hardware registers, where a read takes a CD response byte or a pad byte, clears a timer flag or takes MDEC data.
   Where such a function saved the caller's sp in one of the scratchpad's last 2 words (main_800273ec does), the walk goes
   on from there, after a '|'; where it did not, the walk ends with the scratchpad. */
static void print_stack(u32 hi, int max) {
    u32 p = PHYS(cpu.r[29]), off, next = 0; int k = 0;
    if (IS_SCR(p)) {
        for (off = p & 0x3fcu; off < 0x400u && k < max; off += 4) {
            u32 w; memcpy(&w, psx_scratch + off, 4);
            if (off >= 0x3f8u && w >= 0x801f0000u && w < 0x80200000u && !(w & 3)) next = w;   /* the saved sp, not a return address */
            else if (w >= 0x80010000u && w < hi && !(w & 3)) { fprintf(stderr, " %08x", w); k++; }
        }
        if (!next) return;
        fprintf(stderr, " |"); p = PHYS(next);
    }
    if (!IS_RAM(p)) { fprintf(stderr, " (sp is not in memory)"); return; }
    for (off = p & 0x1ffffcu; off < 0x200000u && k < max; off += 4) {
        u32 w; memcpy(&w, psx_ram + off, 4);
        if (w >= 0x80010000u && w < hi && !(w & 3)) { fprintf(stderr, " %08x", w); k++; }
    }
}
static unsigned run_frames;   /* frames since start, for host_quit's report */
void host_quit(void) {
    /* where the game stood when it was closed: after a freeze this is the trace that is left (the game's messages go to a
       log file when it is started from the launcher) */
    fprintf(stderr, "[exit] closed after %u s of game time: ra=%08x sp=%08x sr=%08x istat=%03x imask=%03x\n", run_frames / 60, cpu.r[31], cpu.r[29], cpu.sr, i_stat, i_mask);
    fprintf(stderr, "[exit] guest return addresses on the stack:");
    print_stack(0x80200000u, 24);
    fprintf(stderr, "\n");
    bios_report_stall("exit", 1);
    { void ach_shutdown(void); ach_shutdown(); }
    g_rumble = 0; host_pad_rumble(0, 0, 0); host_pad_rumble(1, 0, 0); plat_exit(0);
}

void host_init(void) {
    plat_init();
    setvbuf(stdout, NULL, _IONBF, 0);   /* the game's own text (BIOS putchar, puts) must be in a log file even when the game is killed */
    { const char *tst = getenv("DMRECOMP_GFX_TEST"); test_nopad = tst && strstr(tst, "nopad"); }   /* read once, here */
    log_input = getenv("DM_LOG_INPUT") != NULL;
    last_frame = plat_now_ms();
    fb = calloc((size_t)1024 * 4 * 512 * 4, 4);   /* room for the largest internal resolution */
    /* the graphics backend is chosen once, here. renderer = gl: OpenGL, else the software stretch. renderer = vulkan: Vulkan
       first, whatever it can do there (without its GPU renderer the CPU draws the picture). renderer = auto: Vulkan when it can
       do all that is asked, which with gpu_gl = 1 includes the GPU renderer, and when its device is not one that draws on the CPU;
       otherwise OpenGL gets its turn. Each step down is said. */
    static const char *const rname[3] = { "gl", "vulkan", "auto" };
    const GfxBackend *order[4] = { &gfx_gl_backend, &gfx_soft_backend, NULL, NULL };
    if (g_renderer) { order[0] = &gfx_vk_backend; order[1] = &gfx_gl_backend; order[2] = &gfx_soft_backend; }
    gfx_vk_need_renderer = g_renderer == 2 && g_gpu_gl;
    gfx_vk_no_software = g_renderer == 2;   /* auto: OpenGL on the graphics card is better than Vulkan on the CPU */
    for (int i = 0; order[i]; i++) {
        if (order[i]->init(win_w, win_h, GAME_NAME " (static recomp)")) { gfx = order[i]; break; }
        if (!order[i + 1]) { plat_message("Cannot open a window."); plat_exit(1); }
        fprintf(stderr, "[video] renderer = %s: %s cannot be used here; trying %s\n", rname[g_renderer], order[i]->name, order[i + 1]->name);
    }
    fprintf(stderr, "[video] renderer = %s: using %s\n", rname[g_renderer], gfx->name);
    show_settings();
    if (g_fullscreen || plat_wants_fullscreen()) set_fullscreen(1);
    plat_audio_init();
    { void ach_init(void); ach_init(); }   /* Recomp Achievements (achievements.c): does nothing without RecompLauncher */
}

extern int g_shot_every;
static int want_grab;               /* --shot-window: the next frame shown is also copied out of the window */

static void present(void) {
    DisplayArea d; gpu_display_area(&d);
    int fb_on_gpu = 0, gpu_box[4] = { 0, 0, 0, 0 };   /* fb_on_gpu: the picture is on the graphics card (gpu_gl), in this console-pixel box */
    int gls = (d.enabled && !d.depth24 && gfx->hw) ? gpu_gl_scale() : 0;
    int hs = 1; const u16 *hv = gls ? NULL : gpu_hires(&hs);
    fb_w = d.w; fb_h = d.h;
    if (!d.enabled) memset(fb, 0, (size_t)fb_w * fb_h * 4);
    else if (gls) {
        fb_w = d.w * gls; fb_h = d.h * gls; fb_on_gpu = 1;
        gpu_box[0] = d.x; gpu_box[1] = d.y; gpu_box[2] = d.w; gpu_box[3] = d.h;
        if (g_shot_every || menu_capture) gfx->r_read(d.x, d.y, d.w, d.h, fb);   /* screenshots are saved from fb; the menu darkens it */
    }
    else if (!d.depth24 && hv) {
        fb_w = d.w * hs; fb_h = d.h * hs;
        for (int y = 0; y < fb_h; y++) {
            const u16 *row = hv + (size_t)((d.y * hs + y) & (512 * hs - 1)) * (size_t)(1024 * hs);
            for (int x = 0; x < fb_w; x++) {
                u16 p = row[(d.x * hs + x) & (1024 * hs - 1)];
                u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                fb[y * fb_w + x] = (r << 16) | (g << 8) | b;
            }
        }
    } else if (!d.depth24) {
        for (int y = 0; y < fb_h; y++) for (int x = 0; x < fb_w; x++) {
            u16 p = vram[((d.y + y) & 511) * 1024 + ((d.x + x) & 1023)];
            u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
            fb[y * fb_w + x] = (r << 16) | (g << 8) | b;
        }
    } else {
        for (int y = 0; y < fb_h; y++) {
            const u16 *row = &vram[((d.y + y) & 511) * 1024];
            for (int x = 0; x < fb_w; x++) {
                u32 bo = (u32)d.x * 2 + (u32)x * 3, v = 0;
                for (int k = 0; k < 3; k++) {
                    u32 bb = bo + k; u16 w = row[(bb >> 1) & 1023];
                    v |= (u32)((bb & 1) ? w >> 8 : w & 0xff) << (8 * k);
                }
                fb[y * fb_w + x] = ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
            }
        }
    }
    int cw, ch; plat_client_size(&cw, &ch);
    int w = cw, h = ch;
    if (!g_aspect) {
        double ar = ws_on ? ws_ratio : 4.0 / 3.0;
        h = (int)(cw / ar);
        if (h > ch) { h = ch; w = (int)(ch * ar); }
        if (g_integer_scale && h >= 240) { h -= h % 240; w = (int)(h * ar); }
    }
    if (cw <= 0 || ch <= 0) return;
    GfxFrame f;
    f.fb = fb; f.fb_w = fb_w; f.fb_h = fb_h;
    f.on_gpu = fb_on_gpu; for (int i = 0; i < 4; i++) f.box[i] = gpu_box[i];
    f.cw = cw; f.ch = ch; f.w = w; f.h = h;
    f.filter = g_filter; f.upscale = g_upscale; f.fsr_sharp = g_fsr_sharp;
    f.grab = want_grab;
    gfx->present(&f);
}

/* ---- debug: uncompressed PNG screenshots -------------------------------- */
int g_shot_every, g_stats, g_shot_window;   /* --shot-window: screenshots show the window contents */
unsigned g_stat_mdec, g_stat_sectors, g_stat_prims, g_stat_flips;
static u32 crc_tab[256];
static u32 crc32b(u32 c, const u8 *p, size_t n) {
    if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 k = i; for (int j = 0; j < 8; j++) k = (k & 1) ? 0xedb88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8); return ~c;
}
static void png_chunk(FILE *f, const char *t, const u8 *d, u32 n) {
    u8 h[8] = { (u8)(n >> 24), (u8)(n >> 16), (u8)(n >> 8), (u8)n, (u8)t[0], (u8)t[1], (u8)t[2], (u8)t[3] };
    fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    u32 c = crc32b(crc32b(0, h + 4, 4), d, n);
    u8 cb[4] = { (u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c }; fwrite(cb, 1, 4, f);
}
static void save_png(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    int w = fb_w, h = fb_h;
    const u32 *src = fb;
    if (g_shot_window && gfx->hw) {   /* what is actually put in the window, not the internal picture */
        int gw, gh; const u32 *gp;
        want_grab = 1; present(); want_grab = 0;
        if ((gp = gfx->grab(&gw, &gh)) != NULL) { src = gp; w = gw; h = gh; }
    }
    if (src == fb) { w = fb_w; h = fb_h; }
    u32 raw = (u32)(w * 3 + 1) * h;
    u8 *z = malloc(raw + (raw / 65535 + 1) * 5 + 16), *o = z;
    u8 *img = malloc(raw), *q = img;
    for (int y = 0; y < h; y++) { *q++ = 0; for (int x = 0; x < w; x++) { u32 p = src[y * w + x]; *q++ = (u8)(p >> 16); *q++ = (u8)(p >> 8); *q++ = (u8)p; } }
    *o++ = 0x78; *o++ = 0x01;
    u32 a = 1, b = 0;
    for (u32 pos = 0; pos < raw;) {
        u32 n = raw - pos > 65535 ? 65535 : raw - pos;
        *o++ = pos + n >= raw; *o++ = (u8)n; *o++ = (u8)(n >> 8); *o++ = (u8)~n; *o++ = (u8)(~n >> 8);
        memcpy(o, img + pos, n); o += n;
        for (u32 i = 0; i < n; i++) { a = (a + img[pos + i]) % 65521; b = (b + a) % 65521; }
        pos += n;
    }
    *o++ = (u8)(b >> 8); *o++ = (u8)b; *o++ = (u8)(a >> 8); *o++ = (u8)a;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ih[13] = { (u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h, 8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ih, 13); png_chunk(f, "IDAT", z, (u32)(o - z)); png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(z); free(img);
}

#include "host_menu.h"   /* the in-game menu: menu_run */

/* --ramdump f1,f2,..  : write ram_<frame>.bin at those frames
   --poke frame:addr:val[:size] : write guest memory at a frame (size 1/2/4, default 1) */
static unsigned dump_at[32]; static int dump_n;
static struct { unsigned frame; u32 addr, val; int size; } pokes[32]; static int poke_n;
void host_set_ramdump(const char *t) { while (*t && dump_n < 32) { dump_at[dump_n++] = (unsigned)strtoul(t, (char **)&t, 10); if (*t == ',') t++; } }
void host_add_poke(const char *t) {
    if (poke_n >= 32) return;
    pokes[poke_n].frame = (unsigned)strtoul(t, (char **)&t, 10); if (*t == ':') t++;
    pokes[poke_n].addr = (u32)strtoul(t, (char **)&t, 16); if (*t == ':') t++;
    pokes[poke_n].val = (u32)strtoul(t, (char **)&t, 16);
    pokes[poke_n].size = *t == ':' ? atoi(t + 1) : 1;
    poke_n++;
}
static void debug_frame(unsigned frame) {
    for (int i = 0; i < dump_n; i++) if (dump_at[i] == frame) {
        char name[64]; snprintf(name, sizeof name, "ram_%05u.bin", frame);
        FILE *f = fopen(name, "wb"); if (f) { fwrite(psx_ram, 1, sizeof psx_ram, f); fclose(f); }
    }
    for (int i = 0; i < poke_n; i++) if (pokes[i].frame == frame) {
        if (pokes[i].size == 4) mw32(pokes[i].addr, pokes[i].val);
        else if (pokes[i].size == 2) mw16(pokes[i].addr, (u16)pokes[i].val);
        else mw8(pokes[i].addr, (u8)pokes[i].val);
    }
}
#ifdef GAME_VIEW_CONE
/* Widescreen, the other half. The squeeze in the GTE (g_ws_kq) widens what is drawn, but the game leaves out, before it
   draws, every object whose bounding sphere is outside its own 4:3 view cone, so things at the sides of a wide picture
   appeared and vanished. The game keeps the cone in three words of its camera record, set when the field of view
   changes (Freestyle BMX: main_8003edf4, read by the sphere test main_8003f864 and by main_80042660): the slope x/z of
   the side planes at GAME_VIEW_CONE, its inverse 8 bytes on, and sqrt(slope^2 + 1) 16 bytes on, all in 20.12 fixed
   point. With widescreen on, those three are widened by the same factor as the picture, once a frame, here. The
   game's own values are remembered and put back when widescreen is switched off; words that do not look like a cone
   (before the game has set one) are left alone, and with widescreen off nothing is ever written. */
static void ws_view_cone(void) {
    static s32 game[3], ours[3]; static int have;
    u8 *p = psx_ram + (GAME_VIEW_CONE & 0x1fffffu);
    s32 cur[3], want[3];
    for (int i = 0; i < 3; i++) memcpy(&cur[i], p + 8 * i, 4);
    if (!have || memcmp(cur, ours, sizeof cur)) {   /* not what was left here: the game has set a cone */
        double xs = cur[0], one = 4096.0 * 4096.0;
        have = cur[0] > 0 && fabs(xs * cur[1] - one) < one / 50 && fabs(cur[2] - sqrt(xs * xs + one)) < 64;
        if (!have) return;
        memcpy(game, cur, sizeof game);
    }
    memcpy(want, game, sizeof want);
    if (ws_ratio > 0) {
        double wide = ws_ratio / (4.0 / 3.0), xs = game[0] * wide;
        want[0] = (s32)(xs + 0.5); want[1] = (s32)(game[1] / wide + 0.5); want[2] = (s32)(sqrt(xs * xs + 4096.0 * 4096.0) + 0.5);
    }
    if (memcmp(want, cur, sizeof want)) {
        static int said;
        if (!said && ws_ratio > 0) { said = 1; fprintf(stderr, "[video] widescreen: the game's view cone is widened too (slope %d to %d)\n", game[0], want[0]); }
        for (int i = 0; i < 3; i++) memcpy(p + 8 * i, &want[i], 4);
    }
    memcpy(ours, want, sizeof ours);
}
#endif

/* Widescreen, the sky. Both games draw the sky backdrop as rows of flat tiles in one slot of the drawing list
   (sky_slot), not through the GTE: each row starts left of the screen, at a place set by the camera's heading, and
   steps one tile width at a time while x < 512, the tile number going round the sky's ring of tiles. mem.c and gpu.c
   squeeze those tiles towards the middle like the 3D picture, so they keep their shape. To fill the wider view, the
   "patches" in games/<id>.json make the game's own routine start whole tiles further left, with the tile number as
   many tiles back, and stop further right. With widescreen off the three give the game's own values. */
static int sky_back;   /* tiles added on the left of the row being drawn */
static s32 sky_reach(void) {   /* how far the wide view reaches past each side of the 512-pixel screen */
    return g_ws_kq == 65536 ? 0 : (s32)((256LL * 65536 + g_ws_kq - 1) / g_ws_kq) - 256;
}
s32 ws_sky_left(u32 w) {
    s32 r = sky_reach();
    sky_back = (r > 0 && w >= 16 && w <= 512) ? (int)((r + (s32)w - 1) / (s32)w) : 0;
    return sky_back * (s32)w;
}
u32 ws_sky_first(u32 i, u32 n) {
    if (!sky_back || !n || i >= n) return i;
    return (i + n - (u32)sky_back % n) % n;
}
s32 ws_sky_right(void) { return 512 + sky_reach(); }

void host_frame(void) {
    static unsigned frame_no;
    static double t_host, t_last_exit, t_guest;
    double t_enter = plat_now_ms();
    if (t_last_exit != 0) t_guest += t_enter - t_last_exit;
    frame_no++;
    run_frames = frame_no;
    debug_frame(frame_no);
    { void ach_frame(void); ach_frame(); }   /* achievement rules on this frame's memory (nothing without RecompLauncher) */
    {   /* stall watchdog: no flips, movie frames, disc reads or drawing for 10 s means the game is stuck (a long load or movie still flips or finishes sooner) */
        extern unsigned g_flip_total;
        static unsigned last_flips, still;
        unsigned prog = g_flip_total + g_stat_mdec + g_stat_sectors + g_stat_prims;   /* flips, movie frames, disc reads and drawing all count as progress */
        if (prog != last_flips) { last_flips = prog; still = 0; }
        else if (++still % 600 == 0) {
            fprintf(stderr, "[watchdog] no flips, movie frames, disc reads or drawing for %u s: ra=%08x sp=%08x sr=%08x istat=%03x imask=%03x\n", still / 60, cpu.r[31], cpu.r[29], cpu.sr, i_stat, i_mask);
            fprintf(stderr, "[watchdog] guest return addresses on the stack:");
            print_stack(0x80200000u, 24);
            fprintf(stderr, "\n");
            bios_report_stall("watchdog", 0);
        }
        /* Once a minute, one line that says where the game is. A second alarm for "still drawing, but stuck" is not possible
           from the outside: the menus are drawn without a display flip, and a save draws its bar for 20 s without one, so a
           missing flip proves nothing. With these lines a log shows what the last minutes were like, also when the game
           was killed; host_quit adds the full report when the game is closed. */
        if (frame_no % 3600 == 0) {
            static unsigned f0, m0;
            fprintf(stderr, "[state] minute %u: %u flips, %u movie pictures; ra=%08x sp=%08x; stack:", frame_no / 3600, g_flip_total - f0, g_mdec_frames - m0, cpu.r[31], cpu.r[29]);
            print_stack(0x80200000u, 8);
            fprintf(stderr, "\n");
            f0 = g_flip_total; m0 = g_mdec_frames;
        }
    }
    if (g_stats && frame_no % 60 == 0) { fprintf(stderr, "[time] guest=%.0fms host=%.0fms per 60 frames\n", t_guest, t_host); t_guest = t_host = 0; }
    if (g_stats && frame_no % 60 == 0) {
        fprintf(stderr, "[stats] t=%us mdec=%u sectors=%u prims=%u flips=%u ra=%08x sp=%08x exc=%d sr=%08x istat=%03x imask=%03x\n", frame_no / 60, g_stat_mdec, g_stat_sectors, g_stat_prims, g_stat_flips, cpu.r[31], cpu.r[29], bios_in_exception, cpu.sr, i_stat, i_mask);
        g_stat_mdec = g_stat_sectors = g_stat_prims = g_stat_flips = 0;
        if (g_stats > 1) { fprintf(stderr, "[stack]"); print_stack(0x800ee000u, 40); fprintf(stderr, "\n"); }
    }
    static u64 last_audio_cycles;
    static s16 mix[4096 * 2];
    u64 n = (cpu.cycles - last_audio_cycles) / 768;
    if (n > 4096) n = 4096;
    last_audio_cycles += n * 768;
    spu_render(mix, (int)n);
    host_audio_push(mix, (int)n);

    {   /* widescreen applies while 3D is being drawn, not in menus */
        static unsigned last_rtp, hot_until;
        unsigned n = g_gte_rtp - last_rtp; last_rtp = g_gte_rtp;
        if (ws_ratio > 0 && n > 15) hot_until = frame_no + 15;
        ws_on = ws_ratio > 0 && frame_no < hot_until;
        g_ws_kq = ws_on ? (int)(65536.0 * (4.0 / 3.0) / ws_ratio + 0.5) : 65536;
#ifdef GAME_VIEW_CONE
        ws_view_cone();
#endif
    }
    static int skip;
    if (!turbo || (++skip & 7) == 0) present();
    if (g_shot_every && frame_no % (unsigned)g_shot_every == 0) {
        char name[64]; snprintf(name, sizeof name, "shot_%05u.png", frame_no);
        present(); save_png(name);   /* present first: fb holds this frame even in fast-forward */
    }
    plat_pump();
    poll_input();
    if (menu_want || (menu_test_keys && frame_no == menu_test_frame)) {   /* the game stands still until the menu is closed */
        int asked = menu_want; menu_want = 0;
        menu_run(frame_no, asked ? NULL : menu_test_keys);
        menu_want = 0; wheel_waiting[0] = wheel_waiting[1] = 0; last_frame = t_enter = plat_now_ms();   /* the pause is neither game time nor host time */
        pad_state[0] = pad_state[1] = 0xffff;   /* what was held when the menu opened was let go long ago: the game must not get it now */
    }
    t_last_exit = plat_now_ms(); t_host += t_last_exit - t_enter;
    if (turbo) { last_frame = plat_now_ms(); return; }
    /* pace to 59.94 Hz */
    double target = 1001.0 / 60.0, now;
    for (int first = 1;; first = 0) {
        now = plat_now_ms();
        double el = now - last_frame;
        if (first) {   /* no time left over: this frame ran late. Half of 120 frames late at scale 2 or 4 means the CPU is too slow for it. */
            static int late, seen, last_scale;
            if (g_gpu_scale != last_scale) { last_scale = g_gpu_scale; late = seen = 0; }   /* F6 or an earlier drop: count afresh */
            if (g_gpu_scale > 1 && el > target + target / 8) late++;
            if (g_gpu_scale > 1 && ++seen == 120) {
                if (g_scale_auto && g_gpu_scale > 1 && late >= 60) {
                    g_gpu_scale = g_gpu_scale == 4 ? 2 : 1; gpu_set_scale(); show_settings();
                    fprintf(stderr, "[scale] could not hold full speed; internal resolution lowered to %dx (scale_auto = 0 turns this off)\n", g_gpu_scale);
                }
                late = seen = 0;
            }
        }
        if (el >= target) break;
        if (target - el > 2.0) plat_sleep_ms(1);
    }
    if (now - last_frame > target * 4) last_frame = now;
    else last_frame += target;
}

void host_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "FATAL: %s\n", buf);
    plat_message(buf);
    plat_exit(1);
}
