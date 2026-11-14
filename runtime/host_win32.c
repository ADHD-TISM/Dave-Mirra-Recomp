/* Win32 host: window, presentation, input, audio, frame pacing. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include "hw.h"

static HWND wnd;
static u32 *fb; static int fb_w = 640, fb_h = 480;
static LARGE_INTEGER qpf, last_frame;
static int turbo;
int g_turbo, g_auto_start;   /* test aids: --turbo, --auto-start N */
static unsigned input_frame;
/* --script "frame:btn,frame:btn,..." : press btn for 6 frames starting at frame.
   btn: u d l r x o s t st se l1 r1 l2 r2 */
static struct { unsigned frame; u16 mask; } script[2048]; static int script_n;
void host_set_script(const char *t) {
    static const struct { const char *n; int bit; } names[] = {
        {"u",4},{"r",5},{"d",6},{"l",7},{"st",3},{"se",0},{"x",14},{"o",13},{"s",15},{"t",12},
        {"l1",10},{"r1",11},{"l2",8},{"r2",9},{0,0} };
    while (*t && script_n < 2048) {
        unsigned f = (unsigned)strtoul(t, (char **)&t, 10);
        if (*t == ':') t++;
        char nm[8]; int k = 0;
        while (*t && *t != ',' && k < 7) nm[k++] = *t++;
        nm[k] = 0;
        if (*t == ',') t++;
        for (int i = 0; names[i].n; i++) if (!strcmp(names[i].n, nm)) {
            script[script_n].frame = f; script[script_n].mask = (u16)(1 << names[i].bit); script_n++;
        }
    }
}

/* ------------------------------------------------------------ input --- */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef struct { WORD left, right; } XVib;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState *);
typedef DWORD (WINAPI *XInputSetState_t)(DWORD, XVib *);
static XInputGetState_t xinput_get;
static XInputSetState_t xinput_set;

static u16 pad_state[2] = { 0xffff, 0xffff };
static u8 pad_axes[2][4] = { { 128, 128, 128, 128 }, { 128, 128, 128, 128 } };
static int pad_found[2];      /* an XInput controller is plugged in for this player */
int g_rumble = 1, g_player2 = 1;
extern int g_pad_analog[2], g_dualshock;

static int key(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* ----------------------------------------------------------- config --- */
/* Settings come from dmrecomp.ini in the working directory (written with the
   defaults on first run), then from the command line. The defaults reproduce
   the console picture; F6/F7/F8/F11 change the display while playing. */
int g_fullscreen, g_integer_scale, g_filter, g_aspect;   /* filter: 0 nearest, 1 smooth; aspect: 0 = 4:3, 1 = stretch */
extern int g_cpu_assist;
/* two keys per pad button, indexed by the pad's button bit */
static int keymap[16][2] = {
    [4] = { VK_UP }, [5] = { VK_RIGHT }, [6] = { VK_DOWN }, [7] = { VK_LEFT },
    [3] = { VK_RETURN }, [0] = { VK_BACK, VK_RSHIFT },
    [14] = { 'Z', 'K' }, [13] = { 'X', 'L' }, [15] = { 'A', 'J' }, [12] = { 'S', 'I' },
    [10] = { 'Q' }, [11] = { 'W' }, [8] = { '1' }, [9] = { '3' } };
static int key_turbo = VK_TAB;
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
    { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT }, { "enter", VK_RETURN },
    { "backspace", VK_BACK }, { "tab", VK_TAB }, { "space", VK_SPACE }, { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
    { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU },
    { "comma", VK_OEM_COMMA }, { "period", VK_OEM_PERIOD }, { "slash", VK_OEM_2 }, { "semicolon", VK_OEM_1 },
    { "num0", VK_NUMPAD0 }, { "num1", VK_NUMPAD1 }, { "num2", VK_NUMPAD2 }, { "num3", VK_NUMPAD3 }, { "num4", VK_NUMPAD4 },
    { "num5", VK_NUMPAD5 }, { "num6", VK_NUMPAD6 }, { "num7", VK_NUMPAD7 }, { "num8", VK_NUMPAD8 }, { "num9", VK_NUMPAD9 },
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
    if (!_stricmp(name, "filter")) { if (!_stricmp(val, "nearest")) g_filter = 0; else if (!_stricmp(val, "smooth")) g_filter = 1; else return 0; return 1; }
    if (!_stricmp(name, "aspect")) { if (!_stricmp(val, "4:3")) g_aspect = 0; else if (!_stricmp(val, "stretch")) g_aspect = 1; else return 0; return 1; }
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
               "# How the picture is enlarged to the window: nearest = sharp pixels, smooth = blended (F7)\n"
               "filter = nearest\n"
               "# 4:3 = console shape with black bars, stretch = fill the window (F8)\n"
               "aspect = 4:3\n"
               "# 1 = only enlarge by whole multiples of 240 lines\n"
               "integer_scale = 0\n"
               "# 1 = start in fullscreen (F11 or Alt+Enter toggles)\n"
               "fullscreen = 0\n"
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

static int xsrc(const XPad *p, int s) {
    switch (s) {
    case X_A: return p->buttons & 0x1000; case X_B: return p->buttons & 0x2000;
    case X_X: return p->buttons & 0x4000; case X_Y: return p->buttons & 0x8000;
    case X_LB: return p->buttons & 0x0100; case X_RB: return p->buttons & 0x0200;
    case X_LT: return p->lt > 100; case X_RT: return p->rt > 100;
    case X_START: return p->buttons & 0x0010; case X_BACK: return p->buttons & 0x0020;
    case X_LS: return p->buttons & 0x0040; case X_RS: return p->buttons & 0x0080;
    case X_UP: return p->buttons & 0x0001; case X_DOWN: return p->buttons & 0x0002;
    case X_LEFT: return p->buttons & 0x0004; case X_RIGHT: return p->buttons & 0x0008;
    }
    return 0;
}
static u8 axis(int v, int flip) { v = (flip ? -v : v) / 256 + 128; return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void poll_input(void) {
    static unsigned probe;
    u16 b[2] = { 0, 0 };   /* active-high while building */
    if (GetForegroundWindow() == wnd) {
        for (int i = 0; i < 16; i++) if (key(keymap[i][0]) || key(keymap[i][1])) b[0] |= (u16)(1 << i);
        turbo = key(key_turbo);
    }
    /* looking for a controller that is not there is slow, so only retry once a second */
    int look = (probe++ % 60) == 0;
    for (int n = 0; n < 2 && xinput_get; n++) {
        XState s;
        memset(pad_axes[n], 128, 4);
        if (n == 1 && !g_player2) { pad_found[n] = 0; continue; }
        if (!pad_found[n] && !look) continue;
        pad_found[n] = xinput_get((DWORD)n, &s) == 0;
        if (!pad_found[n]) continue;
        for (int i = 0; i < 16; i++) if (xsrc(&s.pad, padmap[i])) b[n] |= (u16)(1 << i);
        if (g_pad_analog[n]) {
            pad_axes[n][0] = axis(s.pad.rx, 0); pad_axes[n][1] = axis(s.pad.ry, 1);
            pad_axes[n][2] = axis(s.pad.lx, 0); pad_axes[n][3] = axis(s.pad.ly, 1);
        } else {   /* digital pad: the left stick works the d-pad */
            if (s.pad.ly > 16000) b[n] |= 1 << 4;
            if (s.pad.ly < -16000) b[n] |= 1 << 6;
            if (s.pad.lx < -16000) b[n] |= 1 << 7;
            if (s.pad.lx > 16000) b[n] |= 1 << 5;
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
    if (port < 0 || port > 1 || v == last[port] || !xinput_set || !pad_found[port]) return;
    last[port] = v;
    XVib vib = { (WORD)((v & 0xff) * 257), (WORD)((v >> 8) ? 0xffff : 0) };   /* large motor left, small motor right */
    xinput_set((DWORD)port, &vib);
}

/* ------------------------------------------------------------ audio --- */
#define NBUF 8
#define BUF_FRAMES 1024
static HWAVEOUT wo;
static WAVEHDR hdr[NBUF];
static s16 abuf[NBUF][BUF_FRAMES * 2];
static int afill, acur;

static void audio_init(void) {
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = NULL; return; }
    for (int i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)abuf[i]; hdr[i].dwBufferLength = BUF_FRAMES * 4;
        hdr[i].dwFlags = WHDR_DONE;
    }
}
FILE *g_wav;   /* --wav file: raw 44.1 kHz stereo s16 dump of everything mixed */
void host_audio_push(const s16 *lr, int frames) {
    if (g_wav) fwrite(lr, 4, (size_t)frames, g_wav);
    if (!wo || turbo) return;
    for (int i = 0; i < frames; i++) {
        WAVEHDR *h = &hdr[acur];
        if (afill == 0) {
            if (!(h->dwFlags & WHDR_DONE)) return;   /* queue full: drop */
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
        }
        abuf[acur][afill * 2] = lr[i * 2]; abuf[acur][afill * 2 + 1] = lr[i * 2 + 1];
        if (++afill == BUF_FRAMES) {
            h->dwBufferLength = BUF_FRAMES * 4; h->dwFlags = 0;
            waveOutPrepareHeader(wo, h, sizeof *h);
            waveOutWrite(wo, h, sizeof *h);
            afill = 0; acur = (acur + 1) % NBUF;
        }
    }
}

/* ------------------------------------------------------------ video --- */
static void set_fullscreen(int on) {
    static WINDOWPLACEMENT saved = { sizeof saved };
    static int is_on;
    if (on == is_on) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(wnd, &saved);
        GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(wnd, &saved);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    is_on = g_fullscreen = on;
    ShowCursor(!on);
}
static void show_settings(void) {
    char t[160];
    snprintf(t, sizeof t, "Dave Mirra Freestyle BMX (static recomp) - %dx resolution, %s, %s",
             g_gpu_scale, g_filter ? "smooth" : "sharp", g_aspect ? "stretched" : "4:3");
    SetWindowTextA(wnd, t);
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && !(l & (1 << 30))) { set_fullscreen(!g_fullscreen); return 0; }
        break;
    case WM_KEYDOWN:
        if (l & (1 << 30)) break;   /* auto-repeat */
        if (w == VK_F11) { set_fullscreen(!g_fullscreen); return 0; }
        if (w == VK_F6) { g_gpu_scale = g_gpu_scale == 1 ? 2 : g_gpu_scale == 2 ? 4 : 1; gpu_set_scale(); show_settings(); return 0; }
        if (w == VK_F7) { g_filter = !g_filter; show_settings(); return 0; }
        if (w == VK_F8) { g_aspect = !g_aspect; show_settings(); return 0; }
        break;
    case WM_CLOSE: g_rumble = 0; host_pad_rumble(0, 0, 0); host_pad_rumble(1, 0, 0); ExitProcess(0);
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(h, m, w, l);
}

static void gl_init(void);
void host_init(void) {
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&last_frame);
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    if (xi) xinput_get = (XInputGetState_t)(void *)GetProcAddress(xi, "XInputGetState");
    if (xi) xinput_set = (XInputSetState_t)(void *)GetProcAddress(xi, "XInputSetState");
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dmrecomp"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    RECT r = { 0, 0, 960, 720 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("dmrecomp", "Dave Mirra Freestyle BMX (static recomp)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    fb = calloc((size_t)1024 * 4 * 512 * 4, 4);   /* room for the largest internal resolution */
    gl_init();
    show_settings();
    if (g_fullscreen) set_fullscreen(1);
    audio_init();
}

/* The picture is drawn as one textured quad so the graphics card does the
   enlarging (and the smooth filter). Without OpenGL, GDI stretches it. */
static HDC gl_dc; static int gl_ok, gl_tw, gl_th;
static void gl_init(void) {
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    gl_dc = GetDC(wnd);
    int pf = ChoosePixelFormat(gl_dc, &pfd);
    if (!pf || !SetPixelFormat(gl_dc, pf, &pfd)) return;
    HGLRC rc = wglCreateContext(gl_dc);
    if (!rc || !wglMakeCurrent(gl_dc, rc)) return;
    /* pacing is done by host_frame, so do not also wait for the monitor */
    typedef BOOL (WINAPI *swap_fn)(int);
    swap_fn swap = (swap_fn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap) swap(0);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);   /* clamp to edge */
    glEnable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl_ok = 1;
}
static void gl_present(int cw, int ch, int w, int h) {
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (fb_w != gl_tw || fb_h != gl_th) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fb_w, fb_h, 0, 0x80E1 /* BGRA */, GL_UNSIGNED_BYTE, fb);
        gl_tw = fb_w; gl_th = fb_h;
    } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fb_w, fb_h, 0x80E1, GL_UNSIGNED_BYTE, fb);
    int f = g_filter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    float qx = (float)w / (float)cw, qy = (float)h / (float)ch;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-qx, qy);
    glTexCoord2f(1, 0); glVertex2f(qx, qy);
    glTexCoord2f(1, 1); glVertex2f(qx, -qy);
    glTexCoord2f(0, 1); glVertex2f(-qx, -qy);
    glEnd();
    SwapBuffers(gl_dc);
}

static void present(void) {
    DisplayArea d; gpu_display_area(&d);
    int hs; const u16 *hv = gpu_hires(&hs);
    fb_w = d.w; fb_h = d.h;
    if (!d.enabled) memset(fb, 0, (size_t)fb_w * fb_h * 4);
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
    RECT cr; GetClientRect(wnd, &cr);
    int cw = cr.right, ch = cr.bottom, w = cw, h = ch;
    if (!g_aspect) {
        h = cw * 3 / 4;
        if (h > ch) { h = ch; w = ch * 4 / 3; }
        if (g_integer_scale && h >= 240) { h -= h % 240; w = h * 4 / 3; }
    }
    if (cw <= 0 || ch <= 0) return;
    if (gl_ok) { gl_present(cw, ch, w, h); return; }
    HDC dc = gl_dc;
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = fb_w; bi.bmiHeader.biHeight = -fb_h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    PatBlt(dc, 0, 0, cw, (ch - h) / 2, BLACKNESS);
    PatBlt(dc, 0, (ch + h) / 2, cw, ch, BLACKNESS);
    PatBlt(dc, 0, 0, (cw - w) / 2, ch, BLACKNESS);
    PatBlt(dc, (cw + w) / 2, 0, cw, ch, BLACKNESS);
    SetStretchBltMode(dc, g_filter ? HALFTONE : COLORONCOLOR);
    if (g_filter) SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, (cw - w) / 2, (ch - h) / 2, w, h, 0, 0, fb_w, fb_h, fb, &bi, DIB_RGB_COLORS, SRCCOPY);
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
    const u32 *src = fb; u32 *grab = NULL;
    if (g_shot_window && gl_ok) {   /* what was actually put in the window, not the internal picture */
        RECT cr; GetClientRect(wnd, &cr); w = cr.right; h = cr.bottom;
        grab = malloc((size_t)w * h * 4);
        glReadBuffer(GL_FRONT); glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, 0x80E1, GL_UNSIGNED_BYTE, grab);
        for (int y = 0; y < h / 2; y++) for (int x = 0; x < w; x++) { u32 t = grab[y * w + x]; grab[y * w + x] = grab[(h - 1 - y) * w + x]; grab[(h - 1 - y) * w + x] = t; }
        src = grab;
    }
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
    fclose(f); free(z); free(img); free(grab);
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
static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart; }
void host_frame(void) {
    static unsigned frame_no;
    static double t_host, t_last_exit, t_guest;
    double t_enter = now_ms();
    if (t_last_exit != 0) t_guest += t_enter - t_last_exit;
    frame_no++;
    debug_frame(frame_no);
    {   /* stall watchdog: no flips, movie frames or disc reads for 10 s means the game is stuck (a long load or movie still flips or finishes sooner) */
        extern unsigned g_flip_total;
        static unsigned last_flips, still;
        unsigned prog = g_flip_total + g_stat_mdec + g_stat_sectors;   /* flips, movie frames and disc reads all count as progress */
        if (prog != last_flips) { last_flips = prog; still = 0; }
        else if (++still % 600 == 0) {
            fprintf(stderr, "[watchdog] no flips, movie frames or disc reads for %u s: ra=%08x sp=%08x sr=%08x istat=%03x imask=%03x\n", still / 60, cpu.r[31], cpu.r[29], cpu.sr, i_stat, i_mask);
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

    static int skip;
    if (!turbo || (++skip & 7) == 0) present();
    if (g_shot_every && frame_no % (unsigned)g_shot_every == 0) {
        char name[64]; snprintf(name, sizeof name, "shot_%05u.png", frame_no);
        present(); save_png(name);
    }
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    poll_input();
    t_last_exit = now_ms(); t_host += t_last_exit - t_enter;
    if (turbo) { QueryPerformanceCounter(&last_frame); return; }
    /* pace to 59.94 Hz */
    LONGLONG target = qpf.QuadPart * 1001 / 60000;
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG el = now.QuadPart - last_frame.QuadPart;
        if (el >= target) break;
        if (target - el > qpf.QuadPart / 500) Sleep(1);
    }
    if (now.QuadPart - last_frame.QuadPart > target * 4) last_frame = now;
    else last_frame.QuadPart += target;
}

void host_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "FATAL: %s\n", buf);
    MessageBoxA(wnd, buf, "dmrecomp", MB_ICONERROR);
    ExitProcess(1);
}
