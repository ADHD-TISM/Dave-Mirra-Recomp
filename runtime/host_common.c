/* Host: settings, input mapping, presentation, screenshots and frame pacing.
   The window, OpenGL context, keys, gamepads, sound device and clock come from
   the platform file (plat_win32.c or plat_sdl.c) through host.h. */
#ifdef DM_SDL
#include <SDL_opengl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#endif
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "hw.h"
#include "host.h"
#include "game.h"   /* GAME_NAME: written into gen/ by tools/regen.py */
#include "gpu_gl.h"
#ifndef APIENTRY
#define APIENTRY
#endif

static u32 *fb; static int fb_w = 640, fb_h = 480;
static double last_frame;   /* ms */
static int turbo;
int g_turbo, g_auto_start;   /* test aids: --turbo, --auto-start N */
static unsigned input_frame;
#include "script_parse.h"   /* host_set_script: the --script / --script-file button script */

/* ------------------------------------------------------------ input --- */
static u16 pad_state[2] = { 0xffff, 0xffff };
static u8 pad_axes[2][4] = { { 128, 128, 128, 128 }, { 128, 128, 128, 128 } };
static int pad_found[2];      /* a gamepad is plugged in for this player */
int g_rumble = 1, g_player2 = 1;
extern int g_pad_analog[2], g_dualshock;

static int key(int k) { return k && plat_key(k); }

/* ----------------------------------------------------------- config --- */
/* Settings come from dmrecomp.ini in the working directory (written with the
   defaults on first run), then from the command line. The defaults reproduce
   the console picture; F6/F7/F8/F11 change the display while playing. */
int g_fullscreen, g_integer_scale, g_filter, g_aspect;   /* filter: 0 nearest, 1 smooth; aspect: 0 = 4:3, 1 = stretch */
int g_scale_auto = 1;   /* step the internal resolution down when the machine cannot keep up */
int g_upscale, g_fsr_sharp = 80;                          /* upscale: 0 = plain, 1 = FSR 1; sharpness 0..100 */
static int win_w = 960, win_h = 720;                      /* window_size */
extern int g_ws_kq; extern unsigned g_gte_rtp;
static double ws_ratio;   /* widescreen: 0 = off, else the width/height of the picture */
static int ws_on;        /* the current frames are 3D, so the wide shape applies */
static int fs_w, fs_h;                                    /* fullscreen_size; 0 = the desktop's own resolution */
extern int g_cpu_assist;
/* two keys per pad button, indexed by the pad's button bit */
static int keymap[16][2] = {
    [4] = { HK_UP }, [5] = { HK_RIGHT }, [6] = { HK_DOWN }, [7] = { HK_LEFT },
    [3] = { HK_ENTER }, [0] = { HK_BACKSPACE, HK_RSHIFT },
    [14] = { 'Z', 'K' }, [13] = { 'X', 'L' }, [15] = { 'A', 'J' }, [12] = { 'S', 'I' },
    [10] = { 'Q' }, [11] = { 'W' }, [8] = { '1' }, [9] = { '3' } };
static int key_turbo = HK_TAB;
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
    if (!_stricmp(name, "rumble")) { g_rumble = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "dualshock")) { g_dualshock = atoi(val) != 0; return 1; }
    if (!_stricmp(name, "player2")) { g_player2 = atoi(val); return 1; }   /* 2 = always present (tests) */
    if (!_stricmp(name, "reverb")) { extern int g_reverb; g_reverb = atoi(val) != 0; return 1; }
    for (int i = 0; pad_opts[i].name; i++) if (!_stricmp(pad_opts[i].name, name)) {
        for (int k = 0; k < (int)(sizeof xnames / sizeof *xnames); k++)
            if (!_stricmp(xnames[k], val)) { padmap[pad_opts[i].bit] = k; return 1; }
        return 0;
    }
    if (!_stricmp(name, "key_fast_forward")) { int vk = key_parse(val); if (vk < 0) return 0; key_turbo = vk; return 1; }
    for (int i = 0; key_opts[i].name; i++) if (!_stricmp(key_opts[i].name, name)) {
        char buf[64]; snprintf(buf, sizeof buf, "%s", val);
        char *second = strchr(buf, ','); if (second) *second++ = 0;
        int a = key_parse(trim(buf)), b = second ? key_parse(trim(second)) : 0;
        if (a < 0 || b < 0) return 0;
        keymap[key_opts[i].bit][0] = a; keymap[key_opts[i].bit][1] = b;
        return 1;
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
               "# Keyboard. One or two keys per button, separated by a comma. Letters, digits, or:\n"
               "# up down left right enter backspace tab space lshift rshift lctrl rctrl lalt ralt\n"
               "# comma period slash semicolon num0..num9 none\n");
    for (int i = 0; key_opts[i].name; i++) {
        const int *k = keymap[key_opts[i].bit];
        fprintf(f, "%s = %s", key_opts[i].name, key_label(k[0]));
        if (k[1]) fprintf(f, ", %s", key_label(k[1]));
        fprintf(f, "\n");
    }
    fprintf(f, "key_fast_forward = %s\n", key_label(key_turbo));
    fclose(f);
}
void host_load_config(const char *path) {
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
static u8 axis(int v, int flip) { v = (flip ? -v : v) / 256 + 128; return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void poll_input(void) {
    static unsigned probe;
    u16 b[2] = { 0, 0 };   /* active-high while building */
    for (int i = 0; i < 16; i++) if (key(keymap[i][0]) || key(keymap[i][1])) b[0] |= (u16)(1 << i);
    turbo = key(key_turbo);
    /* looking for a controller that is not there is slow, so only retry once a second */
    int look = (probe++ % 60) == 0;
    for (int n = 0; n < 2; n++) {
        HostPad s;
        memset(pad_axes[n], 128, 4);
        if (n == 1 && !g_player2) { pad_found[n] = 0; continue; }
        if (!pad_found[n] && !look) continue;
        pad_found[n] = plat_pad(n, &s);
        if (!pad_found[n]) continue;
        for (int i = 0; i < 16; i++) if (xsrc(&s, padmap[i])) b[n] |= (u16)(1 << i);
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
    input_frame++;
    if (g_auto_start && (!script_n || input_frame < script[0].frame) &&
        input_frame % (unsigned)g_auto_start < 4) b[0] |= 1 << 3;
    for (int i = 0; i < script_n; i++)
        if (input_frame >= script[i].frame && input_frame < script[i].frame + 6) {
            b[0] |= script[i].mask;
            if (g_player2 == 2) b[1] |= script[i].mask;   /* test mode: player 2 presses the same buttons */
        }
    if (g_turbo) turbo = 1;
    pad_state[0] = (u16)~b[0]; pad_state[1] = (u16)~b[1];
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
void host_quit(void) { g_rumble = 0; host_pad_rumble(0, 0, 0); host_pad_rumble(1, 0, 0); plat_exit(0); }

static void gl_setup(void);
static int gl_ok, gl_tw, gl_th; static GLuint gl_tex;
void host_init(void) {
    plat_init();
    last_frame = plat_now_ms();
    fb = calloc((size_t)1024 * 4 * 512 * 4, 4);   /* room for the largest internal resolution */
    gl_ok = plat_video_init(win_w, win_h, GAME_NAME " (static recomp)");
    if (gl_ok) gl_setup();
    show_settings();
    if (g_fullscreen || plat_wants_fullscreen()) set_fullscreen(1);
    plat_audio_init();
}

/* The picture is drawn as one textured quad so the graphics card does the
   enlarging (and the smooth filter). Without OpenGL the platform stretches it. */

/* The two shaders below are ported from AMD FidelityFX Super Resolution 1.0 (ffx_fsr1.h):

   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
   associated documentation files (the "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
   following conditions: The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
   ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
   FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
/* FSR 1 (AMD FidelityFX Super Resolution, MIT licence): EASU upscales the picture to
   the size it will have on screen, then RCAS sharpens it. Both are fragment shaders;
   the GL 2.0 / FBO entry points are looked up at run time, and if anything is missing
   the plain path is used. */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
typedef GLuint (APIENTRY *fn_createshader)(GLenum);
typedef void (APIENTRY *fn_shadersource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void (APIENTRY *fn_u_i)(GLuint);
typedef void (APIENTRY *fn_getiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *fn_getlog)(GLuint, GLsizei, GLsizei *, char *);
typedef GLuint (APIENTRY *fn_createprogram)(void);
typedef void (APIENTRY *fn_attach)(GLuint, GLuint);
typedef GLint (APIENTRY *fn_uloc)(GLuint, const char *);
typedef void (APIENTRY *fn_uni1i)(GLint, GLint);
typedef void (APIENTRY *fn_uni1f)(GLint, GLfloat);
typedef void (APIENTRY *fn_uni2f)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *fn_genfb)(GLsizei, GLuint *);
typedef void (APIENTRY *fn_bindfb)(GLenum, GLuint);
typedef void (APIENTRY *fn_fbtex)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *fn_checkfb)(GLenum);
static fn_createshader p_CreateShader; static fn_shadersource p_ShaderSource; static fn_u_i p_CompileShader, p_LinkProgram, p_UseProgram;
static fn_getiv p_GetShaderiv, p_GetProgramiv; static fn_getlog p_GetShaderInfoLog; static fn_createprogram p_CreateProgram;
static fn_attach p_AttachShader; static fn_uloc p_GetUniformLocation; static fn_uni1i p_Uniform1i; static fn_uni1f p_Uniform1f;
static fn_uni2f p_Uniform2f; static fn_genfb p_GenFramebuffers; static fn_bindfb p_BindFramebuffer; static fn_fbtex p_FramebufferTexture2D;
static fn_checkfb p_CheckFramebufferStatus;
static GLuint fsr_easu, fsr_rcas, fsr_fbo, fsr_tex; static int fsr_state, fsr_tw, fsr_th;   /* state: 0 untried, 1 ready, -1 unavailable */

static const char *const easu_src =
"#version 120\n"
"uniform sampler2D tex; uniform vec2 inSize; uniform vec2 outSize;\n"
"vec3 fetch(vec2 t) { return texture2D(tex, (t + 0.5) / inSize).rgb; }\n"
"float lum(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
"void setf(inout vec2 dir, inout float len, vec2 pp, float w, float lA, float lB, float lC, float lD, float lE) {\n"
"  float dc = lD - lC, cb = lC - lB;\n"
"  float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1e-6);\n"
"  float dirX = lD - lB;\n"
"  dir.x += dirX * w;\n"
"  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX; len += lenX * w;\n"
"  float ec = lE - lC, ca = lC - lA;\n"
"  float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1e-6);\n"
"  float dirY = lE - lA;\n"
"  dir.y += dirY * w;\n"
"  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY; len += lenY * w;\n"
"}\n"
"void tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {\n"
"  vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x);\n"
"  v *= len;\n"
"  float d2 = min(v.x * v.x + v.y * v.y, clp);\n"
"  float wB = 2.0 / 5.0 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
"  wB *= wB; wA *= wA;\n"
"  wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);\n"
"  float w = wB * wA;\n"
"  aC += c * w; aW += w;\n"
"}\n"
"void main() {\n"
"  vec2 ip = vec2(floor(gl_FragCoord.x), outSize.y - 1.0 - floor(gl_FragCoord.y));\n"
"  vec2 sc = inSize / outSize;\n"
"  vec2 pp = ip * sc + (0.5 * sc - 0.5);\n"
"  vec2 fp = floor(pp); pp -= fp;\n"
"  vec3 b = fetch(fp + vec2(0.,-1.)), c = fetch(fp + vec2(1.,-1.));\n"
"  vec3 e = fetch(fp + vec2(-1.,0.)), f = fetch(fp), g = fetch(fp + vec2(1.,0.)), h = fetch(fp + vec2(2.,0.));\n"
"  vec3 i = fetch(fp + vec2(-1.,1.)), j = fetch(fp + vec2(0.,1.)), k = fetch(fp + vec2(1.,1.)), l = fetch(fp + vec2(2.,1.));\n"
"  vec3 n = fetch(fp + vec2(0.,2.)), o = fetch(fp + vec2(1.,2.));\n"
"  float bL = lum(b), cL = lum(c), eL = lum(e), fL = lum(f), gL = lum(g), hL = lum(h), iL = lum(i), jL = lum(j), kL = lum(k), lL = lum(l), nL = lum(n), oL = lum(o);\n"
"  vec2 dir = vec2(0.0); float len = 0.0;\n"
"  setf(dir, len, pp, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
"  setf(dir, len, pp, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
"  setf(dir, len, pp, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
"  setf(dir, len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
"  vec2 dir2 = dir * dir;\n"
"  float dirR = dir2.x + dir2.y;\n"
"  bool zro = dirR < 1.0 / 32768.0;\n"
"  dirR = inversesqrt(max(dirR, 1e-9));\n"
"  dirR = zro ? 1.0 : dirR;\n"
"  dir.x = zro ? 1.0 : dir.x;\n"
"  dir *= dirR;\n"
"  len = len * 0.5; len *= len;\n"
"  float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));\n"
"  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
"  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
"  float clp = 1.0 / lob;\n"
"  vec3 min4 = min(min(f, g), min(j, k)), max4 = max(max(f, g), max(j, k));\n"
"  vec3 aC = vec3(0.0); float aW = 0.0;\n"
"  tap(aC, aW, vec2( 0.,-1.) - pp, dir, len2, lob, clp, b);\n"
"  tap(aC, aW, vec2( 1.,-1.) - pp, dir, len2, lob, clp, c);\n"
"  tap(aC, aW, vec2(-1., 1.) - pp, dir, len2, lob, clp, i);\n"
"  tap(aC, aW, vec2( 0., 1.) - pp, dir, len2, lob, clp, j);\n"
"  tap(aC, aW, vec2( 0., 0.) - pp, dir, len2, lob, clp, f);\n"
"  tap(aC, aW, vec2(-1., 0.) - pp, dir, len2, lob, clp, e);\n"
"  tap(aC, aW, vec2( 1., 1.) - pp, dir, len2, lob, clp, k);\n"
"  tap(aC, aW, vec2( 2., 1.) - pp, dir, len2, lob, clp, l);\n"
"  tap(aC, aW, vec2( 2., 0.) - pp, dir, len2, lob, clp, h);\n"
"  tap(aC, aW, vec2( 1., 0.) - pp, dir, len2, lob, clp, g);\n"
"  tap(aC, aW, vec2( 1., 2.) - pp, dir, len2, lob, clp, o);\n"
"  tap(aC, aW, vec2( 0., 2.) - pp, dir, len2, lob, clp, n);\n"
"  gl_FragColor = vec4(min(max4, max(min4, aC / aW)), 1.0);\n"
"}\n";

static const char *const rcas_src =
"#version 120\n"
"uniform sampler2D tex; uniform vec2 texSize; uniform vec2 origin; uniform float con;\n"
"vec3 ld(vec2 p) { return texture2D(tex, (p + 0.5) / texSize).rgb; }\n"
"void main() {\n"
"  vec2 sp = floor(gl_FragCoord.xy - origin);\n"
"  vec3 b = ld(sp + vec2(0., -1.)), d = ld(sp + vec2(-1., 0.)), e = ld(sp), f = ld(sp + vec2(1., 0.)), h = ld(sp + vec2(0., 1.));\n"
"  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
"  vec3 hitMin = min(mn4, e) / (4.0 * max(mx4, 1e-5));\n"
"  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);\n"
"  vec3 lobe3 = max(-hitMin, hitMax);\n"
"  float lobe = max(-0.1875, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * con;\n"
"  float rcpL = 1.0 / (4.0 * lobe + 1.0);\n"
"  gl_FragColor = vec4(clamp((lobe * (b + d + h + f) + e) * rcpL, 0.0, 1.0), 1.0);\n"
"}\n";

static GLuint make_program(const char *src, const char *what) {
    GLuint s = p_CreateShader(GL_FRAGMENT_SHADER);
    p_ShaderSource(s, 1, &src, NULL); p_CompileShader(s);
    GLint ok = 0; p_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = ""; p_GetShaderInfoLog(s, sizeof log - 1, NULL, log);
        fprintf(stderr, "[video] FSR %s shader failed: %s\n", what, log);
        return 0;
    }
    GLuint p = p_CreateProgram(); p_AttachShader(p, s); p_LinkProgram(p);
    p_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "[video] FSR %s shader did not link\n", what); return 0; }
    return p;
}
static int fsr_init(void) {
#define LOADGL(var, nm) (var = plat_gl_proc(nm))
    int ok = LOADGL(p_CreateShader, "glCreateShader") && LOADGL(p_ShaderSource, "glShaderSource") && LOADGL(p_CompileShader, "glCompileShader")
          && LOADGL(p_GetShaderiv, "glGetShaderiv") && LOADGL(p_GetShaderInfoLog, "glGetShaderInfoLog") && LOADGL(p_CreateProgram, "glCreateProgram")
          && LOADGL(p_AttachShader, "glAttachShader") && LOADGL(p_LinkProgram, "glLinkProgram") && LOADGL(p_GetProgramiv, "glGetProgramiv")
          && LOADGL(p_UseProgram, "glUseProgram") && LOADGL(p_GetUniformLocation, "glGetUniformLocation") && LOADGL(p_Uniform1i, "glUniform1i")
          && LOADGL(p_Uniform1f, "glUniform1f") && LOADGL(p_Uniform2f, "glUniform2f");
    if (ok) ok = (LOADGL(p_GenFramebuffers, "glGenFramebuffers") || LOADGL(p_GenFramebuffers, "glGenFramebuffersEXT"))
              && (LOADGL(p_BindFramebuffer, "glBindFramebuffer") || LOADGL(p_BindFramebuffer, "glBindFramebufferEXT"))
              && (LOADGL(p_FramebufferTexture2D, "glFramebufferTexture2D") || LOADGL(p_FramebufferTexture2D, "glFramebufferTexture2DEXT"))
              && (LOADGL(p_CheckFramebufferStatus, "glCheckFramebufferStatus") || LOADGL(p_CheckFramebufferStatus, "glCheckFramebufferStatusEXT"));
    if (ok) {
        fsr_easu = make_program(easu_src, "EASU"); fsr_rcas = make_program(rcas_src, "RCAS");
        ok = fsr_easu && fsr_rcas;
    }
    if (!ok) { fprintf(stderr, "[video] FSR is not available on this graphics driver; using the plain picture\n"); return -1; }
    p_GenFramebuffers(1, &fsr_fbo);
    glGenTextures(1, &fsr_tex);
    return 1;
}
/* Both passes; returns 0 if the picture should be drawn the plain way. */
static int fsr_present(int cw, int ch, int w, int h) {
    if (fsr_state == 0) fsr_state = fsr_init();
    if (fsr_state < 0 || w <= fb_w || h <= fb_h) return 0;
    if (w != fsr_tw || h != fsr_th) {
        glBindTexture(GL_TEXTURE_2D, fsr_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_BindFramebuffer(GL_FRAMEBUFFER, fsr_fbo);
        p_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fsr_tex, 0);
        GLenum st = p_CheckFramebufferStatus(GL_FRAMEBUFFER);
        p_BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[video] FSR render target is not usable; using the plain picture\n"); fsr_state = -1; glBindTexture(GL_TEXTURE_2D, gl_tex); return 0; }
        fsr_tw = w; fsr_th = h;
    }
    /* pass 1: EASU, input picture -> w x h target */
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_BindFramebuffer(GL_FRAMEBUFFER, fsr_fbo);
    glViewport(0, 0, w, h);
    p_UseProgram(fsr_easu);
    p_Uniform1i(p_GetUniformLocation(fsr_easu, "tex"), 0);
    p_Uniform2f(p_GetUniformLocation(fsr_easu, "inSize"), (float)fb_w, (float)fb_h);
    p_Uniform2f(p_GetUniformLocation(fsr_easu, "outSize"), (float)w, (float)h);
    glBegin(GL_QUADS); glVertex2f(-1, 1); glVertex2f(1, 1); glVertex2f(1, -1); glVertex2f(-1, -1); glEnd();
    p_BindFramebuffer(GL_FRAMEBUFFER, 0);
    /* pass 2: RCAS, target -> window */
    int ox = (cw - w) / 2, oy = (ch - h) / 2;
    glBindTexture(GL_TEXTURE_2D, fsr_tex);
    glViewport(ox, oy, w, h);
    p_UseProgram(fsr_rcas);
    p_Uniform1i(p_GetUniformLocation(fsr_rcas, "tex"), 0);
    p_Uniform2f(p_GetUniformLocation(fsr_rcas, "texSize"), (float)w, (float)h);
    p_Uniform2f(p_GetUniformLocation(fsr_rcas, "origin"), (float)ox, (float)oy);
    p_Uniform1f(p_GetUniformLocation(fsr_rcas, "con"), (float)exp2(-(1.0 - g_fsr_sharp / 100.0) * 2.0));
    glBegin(GL_QUADS); glVertex2f(-1, 1); glVertex2f(1, 1); glVertex2f(1, -1); glVertex2f(-1, -1); glEnd();
    p_UseProgram(0);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    return 1;
}
static void gl_setup(void) {
    glGenTextures(1, &gl_tex); glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);   /* clamp to edge */
    glEnable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}
/* --shot-window: the next frame drawn is also copied out, from the back buffer before it is shown
   (reading the front buffer afterwards is not reliable on every system) */
static u32 *grab; static int grab_w, grab_h, grab_want;
static void grab_and_swap(int cw, int ch) {
    if (grab_want) {
        grab_want = 0;
        free(grab); grab = malloc((size_t)cw * ch * 4); grab_w = cw; grab_h = ch;
        if (grab) {
            glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, cw, ch, 0x80E1, GL_UNSIGNED_BYTE, grab);
            for (int y = 0; y < ch / 2; y++) for (int x = 0; x < cw; x++) { u32 t = grab[y * cw + x]; grab[y * cw + x] = grab[(ch - 1 - y) * cw + x]; grab[(ch - 1 - y) * cw + x] = t; }
        }
    }
    plat_swap();
}
static int fb_on_gpu, gpu_box[4];   /* the picture is a texture on the graphics card (gpu_gl) */
extern int g_shot_every;
static void gl_present(int cw, int ch, int w, int h) {
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (fb_on_gpu) {   /* the picture is already on the graphics card: copy the visible part into the texture */
        glg_present(gpu_box[0], gpu_box[1], gpu_box[2], gpu_box[3]);
        gl_tw = fb_w; gl_th = fb_h;
    } else if (fb_w != gl_tw || fb_h != gl_th) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fb_w, fb_h, 0, 0x80E1 /* BGRA */, GL_UNSIGNED_BYTE, fb);
        gl_tw = fb_w; gl_th = fb_h;
    } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fb_w, fb_h, 0x80E1, GL_UNSIGNED_BYTE, fb);
    if (g_upscale && fsr_present(cw, ch, w, h)) { grab_and_swap(cw, ch); return; }
    int f = g_filter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    float qx = (float)w / (float)cw, qy = (float)h / (float)ch;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-qx, qy);
    glTexCoord2f(1, 0); glVertex2f(qx, qy);
    glTexCoord2f(1, 1); glVertex2f(qx, -qy);
    glTexCoord2f(0, 1); glVertex2f(-qx, -qy);
    glEnd();
    grab_and_swap(cw, ch);
}

static void present(void) {
    DisplayArea d; gpu_display_area(&d);
    int gls = (d.enabled && !d.depth24 && gl_ok) ? gpu_gl_scale() : 0;
    int hs = 1; const u16 *hv = gls ? NULL : gpu_hires(&hs);
    fb_w = d.w; fb_h = d.h; fb_on_gpu = 0;
    if (!d.enabled) memset(fb, 0, (size_t)fb_w * fb_h * 4);
    else if (gls) {
        fb_w = d.w * gls; fb_h = d.h * gls; fb_on_gpu = 1;
        gpu_box[0] = d.x; gpu_box[1] = d.y; gpu_box[2] = d.w; gpu_box[3] = d.h;
        if (g_shot_every) glg_read(d.x, d.y, d.w, d.h, fb);   /* screenshots are saved from fb */
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
    if (gl_ok) { gl_present(cw, ch, w, h); return; }
    plat_soft_present(fb, fb_w, fb_h, cw, ch, w, h, g_filter);
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
    if (g_shot_window && gl_ok) {   /* what is actually put in the window, not the internal picture */
        grab_want = 1; present();
        if (!grab_want && grab) { src = grab; w = grab_w; h = grab_h; }
        grab_want = 0;
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
void host_frame(void) {
    static unsigned frame_no;
    static double t_host, t_last_exit, t_guest;
    double t_enter = plat_now_ms();
    if (t_last_exit != 0) t_guest += t_enter - t_last_exit;
    frame_no++;
    debug_frame(frame_no);
    {   /* stall watchdog: no flips, movie frames, disc reads or drawing for 10 s means the game is stuck (a long load or movie still flips or finishes sooner) */
        extern unsigned g_flip_total;
        static unsigned last_flips, still;
        unsigned prog = g_flip_total + g_stat_mdec + g_stat_sectors + g_stat_prims;   /* flips, movie frames, disc reads and drawing all count as progress */
        if (prog != last_flips) { last_flips = prog; still = 0; }
        else if (++still % 600 == 0) {
            fprintf(stderr, "[watchdog] no flips, movie frames, disc reads or drawing for %u s: ra=%08x sp=%08x sr=%08x istat=%03x imask=%03x\n", still / 60, cpu.r[31], cpu.r[29], cpu.sr, i_stat, i_mask);
            fprintf(stderr, "[watchdog] guest return addresses on the stack:");
            int k = 0; for (u32 sp = cpu.r[29]; sp < 0x80200000u && k < 24; sp += 4) { u32 w = mr32(sp); if (w >= 0x80010000u && w < 0x80200000u && !(w & 3)) { fprintf(stderr, " %08x", w); k++; } }
            fprintf(stderr, "\n");
            bios_report_stall();
        }
    }
    if (g_stats && frame_no % 60 == 0) { fprintf(stderr, "[time] guest=%.0fms host=%.0fms per 60 frames\n", t_guest, t_host); t_guest = t_host = 0; }
    if (g_stats && frame_no % 60 == 0) {
        fprintf(stderr, "[stats] t=%us mdec=%u sectors=%u prims=%u flips=%u ra=%08x sp=%08x exc=%d sr=%08x istat=%03x imask=%03x\n", frame_no / 60, g_stat_mdec, g_stat_sectors, g_stat_prims, g_stat_flips, cpu.r[31], cpu.r[29], bios_in_exception, cpu.sr, i_stat, i_mask);
        g_stat_mdec = g_stat_sectors = g_stat_prims = g_stat_flips = 0;
        if (g_stats > 1) { fprintf(stderr, "[stack]"); int k = 0; for (u32 sp = cpu.r[29]; sp < 0x80200000u && k < 40; sp += 4) { u32 w = mr32(sp); if (w >= 0x80010000u && w < 0x800ee000u && !(w & 3)) { fprintf(stderr, " %08x", w); k++; } } fprintf(stderr, "\n"); }
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
    }
    static int skip;
    if (!turbo || (++skip & 7) == 0) present();
    if (g_shot_every && frame_no % (unsigned)g_shot_every == 0) {
        char name[64]; snprintf(name, sizeof name, "shot_%05u.png", frame_no);
        present(); save_png(name);   /* present first: fb holds this frame even in fast-forward */
    }
    plat_pump();
    poll_input();
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
