/* Win32 platform: window, OpenGL context, keys, XInput gamepads, waveOut sound, clock. */
#if defined(_WIN32) && !defined(DM_SDL)   /* the whole file: see plat_sdl.c */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#define VK_NO_PROTOTYPES          /* the loader is opened at run time: nothing links against Vulkan */
#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan/vulkan.h"
#include "host.h"

static HWND wnd;
static HDC dc;
static LARGE_INTEGER qpf;

/* ------------------------------------------------------------ input --- */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef struct { WORD left, right; } XVib;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState *);
typedef DWORD (WINAPI *XInputSetState_t)(DWORD, XVib *);
static XInputGetState_t xinput_get;
static XInputSetState_t xinput_set;

static const int vk_of[HK_COUNT - 256] = {
    [HK_UP - 256] = VK_UP, [HK_DOWN - 256] = VK_DOWN, [HK_LEFT - 256] = VK_LEFT, [HK_RIGHT - 256] = VK_RIGHT,
    [HK_ENTER - 256] = VK_RETURN, [HK_BACKSPACE - 256] = VK_BACK, [HK_TAB - 256] = VK_TAB, [HK_SPACE - 256] = VK_SPACE,
    [HK_LSHIFT - 256] = VK_LSHIFT, [HK_RSHIFT - 256] = VK_RSHIFT, [HK_LCTRL - 256] = VK_LCONTROL, [HK_RCTRL - 256] = VK_RCONTROL,
    [HK_LALT - 256] = VK_LMENU, [HK_RALT - 256] = VK_RMENU, [HK_COMMA - 256] = VK_OEM_COMMA, [HK_PERIOD - 256] = VK_OEM_PERIOD,
    [HK_SLASH - 256] = VK_OEM_2, [HK_SEMICOLON - 256] = VK_OEM_1,
    [HK_NUM0 - 256] = VK_NUMPAD0, [HK_NUM1 - 256] = VK_NUMPAD1, [HK_NUM2 - 256] = VK_NUMPAD2, [HK_NUM3 - 256] = VK_NUMPAD3,
    [HK_NUM4 - 256] = VK_NUMPAD4, [HK_NUM5 - 256] = VK_NUMPAD5, [HK_NUM6 - 256] = VK_NUMPAD6, [HK_NUM7 - 256] = VK_NUMPAD7,
    [HK_NUM8 - 256] = VK_NUMPAD8, [HK_NUM9 - 256] = VK_NUMPAD9,
    [HK_ESCAPE - 256] = VK_ESCAPE, [HK_INSERT - 256] = VK_INSERT, [HK_DELETE - 256] = VK_DELETE, [HK_HOME - 256] = VK_HOME,
    [HK_END - 256] = VK_END, [HK_PAGEUP - 256] = VK_PRIOR, [HK_PAGEDOWN - 256] = VK_NEXT, [HK_CAPSLOCK - 256] = VK_CAPITAL,
    [HK_MINUS - 256] = VK_OEM_MINUS, [HK_EQUALS - 256] = VK_OEM_PLUS, [HK_LBRACKET - 256] = VK_OEM_4, [HK_RBRACKET - 256] = VK_OEM_6,
    [HK_BACKSLASH - 256] = VK_OEM_5, [HK_QUOTE - 256] = VK_OEM_7, [HK_GRAVE - 256] = VK_OEM_3,
    [HK_NUMADD - 256] = VK_ADD, [HK_NUMSUB - 256] = VK_SUBTRACT, [HK_NUMMUL - 256] = VK_MULTIPLY, [HK_NUMDIV - 256] = VK_DIVIDE,
    [HK_NUMDOT - 256] = VK_DECIMAL, [HK_NUMENTER - 256] = VK_RETURN,   /* Win32 reports the keypad's Enter as the main one: "numenter" is either */
    [HK_F1 - 256] = VK_F1, [HK_F2 - 256] = VK_F2, [HK_F3 - 256] = VK_F3, [HK_F4 - 256] = VK_F4, [HK_F5 - 256] = VK_F5,
    [HK_F6 - 256] = VK_F6, [HK_F7 - 256] = VK_F7, [HK_F8 - 256] = VK_F8, [HK_F9 - 256] = VK_F9, [HK_F10 - 256] = VK_F10,
    [HK_F11 - 256] = VK_F11, [HK_F12 - 256] = VK_F12 };
/* A key that went down since the last plat_pump, even if it is already up again: a tap shorter than a frame is not lost */
static unsigned char tapped[256];
static void key_went_down(WPARAM w, LPARAM l) {
    UINT vk = (UINT)w;
    int ext = (int)((l >> 24) & 1);
    if (l & (1 << 30)) return;   /* auto-repeat: the key is held, which GetAsyncKeyState sees */
    if (vk == VK_SHIFT) vk = MapVirtualKeyA((UINT)((l >> 16) & 0xff), MAPVK_VSC_TO_VK_EX);   /* the host asks for left or right */
    else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
    if (vk < 256) tapped[vk] = 1;
}
int plat_key(int code) {
    int vk = code < 256 ? code : code < HK_COUNT ? vk_of[code - 256] : 0;   /* letters and digits are their own virtual-key codes */
    return vk && GetForegroundWindow() == wnd && (tapped[vk] || (GetAsyncKeyState(vk) & 0x8000) != 0);
}
int plat_pad(int n, HostPad *p) {
    XState s;
    if (!xinput_get || xinput_get((DWORD)n, &s) != 0) return 0;
    p->buttons = s.pad.buttons; p->lt = s.pad.lt; p->rt = s.pad.rt;
    p->lx = s.pad.lx; p->ly = s.pad.ly; p->rx = s.pad.rx; p->ry = s.pad.ry;
    return 1;
}
void plat_pad_rumble(int n, uint16_t large, uint16_t small) {
    XVib vib = { large, small };   /* large motor left, small motor right */
    if (xinput_set) xinput_set((DWORD)n, &vib);
}

/* ------------------------------------------------------------ audio --- */
#define NBUF 8
#define BUF_FRAMES 1024
static HWAVEOUT wo;
static WAVEHDR hdr[NBUF];
static int16_t abuf[NBUF][BUF_FRAMES * 2];
static int afill, acur;

void plat_audio_init(void) {
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = NULL; return; }
    for (int i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)abuf[i]; hdr[i].dwBufferLength = BUF_FRAMES * 4;
        hdr[i].dwFlags = WHDR_DONE;
    }
}
void plat_audio_push(const int16_t *lr, int frames) {
    if (!wo) return;
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
/* the pointer is hidden in fullscreen, except while the in-game menu is open. ShowCursor keeps a count, so it is
   stepped until the count gives the state wanted. */
static int fs_on, menu_cursor;
static void cursor_update(void) {
    if (fs_on && !menu_cursor) { while (ShowCursor(FALSE) >= 0) {} }
    else while (ShowCursor(TRUE) < 0) {}
}
void plat_menu_cursor(int on) { menu_cursor = on; cursor_update(); }
void plat_set_fullscreen(int on, int fs_w, int fs_h) {
    static WINDOWPLACEMENT saved = { sizeof saved };
    static int is_on;
    static char mode_dev[CCHDEVICENAME]; static int mode_changed;
    if (on == is_on) return;
    if (on) {
        MONITORINFOEXA mi = { { sizeof mi } };
        GetWindowPlacement(wnd, &saved);
        GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi);
        if (fs_w > 0) {   /* a fullscreen resolution other than the desktop's: switch the display mode */
            DEVMODEA dm = { 0 };
            dm.dmSize = sizeof dm; dm.dmPelsWidth = fs_w; dm.dmPelsHeight = fs_h; dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
            if (ChangeDisplaySettingsExA(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL) == DISP_CHANGE_SUCCESSFUL) {
                snprintf(mode_dev, sizeof mode_dev, "%s", mi.szDevice); mode_changed = 1;
                GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi);
            } else fprintf(stderr, "[video] the display cannot switch to %dx%d; using the desktop resolution\n", fs_w, fs_h);
        }
        SetWindowLongA(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        if (mode_changed) { ChangeDisplaySettingsExA(mode_dev, NULL, NULL, 0, NULL); mode_changed = 0; }
        SetWindowLongA(wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(wnd, &saved);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    is_on = on;
    fs_on = on; cursor_update();
}
void plat_set_title(const char *title) { SetWindowTextA(wnd, title); }
int plat_wants_fullscreen(void) { return 0; }
static int mouse_buttons, mouse_wheel_acc;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: key_went_down(w, l); break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && !(l & (1 << 30))) { host_hotkey(HOT_FULLSCREEN); return 0; }
        key_went_down(w, l);
        break;
    /* the mouse: the host reads these through plat_mouse */
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
        int bit = m == WM_LBUTTONDOWN ? 1 : m == WM_RBUTTONDOWN ? 2 : m == WM_MBUTTONDOWN ? 4 : (HIWORD(w) == XBUTTON1 ? 8 : 16);
        if (!mouse_buttons) SetCapture(h);   /* the release arrives even when the pointer has left the window */
        mouse_buttons |= bit;
        return m == WM_XBUTTONDOWN ? TRUE : 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
        int bit = m == WM_LBUTTONUP ? 1 : m == WM_RBUTTONUP ? 2 : m == WM_MBUTTONUP ? 4 : (HIWORD(w) == XBUTTON1 ? 8 : 16);
        mouse_buttons &= ~bit;
        if (!mouse_buttons) ReleaseCapture();
        return m == WM_XBUTTONUP ? TRUE : 0;
    }
    case WM_MOUSEWHEEL:
        mouse_wheel_acc += GET_WHEEL_DELTA_WPARAM(w);
        return 0;
    case WM_KILLFOCUS: mouse_buttons = 0; ReleaseCapture(); memset(tapped, 0, sizeof tapped); break;
    case WM_CAPTURECHANGED: mouse_buttons = 0; break;   /* the capture was taken away: the releases will not arrive */
    case WM_MOUSEACTIVATE:   /* the click that brings the window to the front is not a press in the game */
        if (GetForegroundWindow() != h && LOWORD(l) == HTCLIENT) return MA_ACTIVATEANDEAT;
        break;
    case WM_SYSCOMMAND:   /* F10 or Alt alone would put the window in menu mode, and the next arrow key would open the system menu and stop the game */
        if ((w & 0xfff0) == SC_KEYMENU && l == 0) return 0;
        break;
    case WM_CLOSE: host_quit(); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(h, m, w, l);
}

void plat_init(void) {
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&qpf);
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    if (xi) xinput_get = (XInputGetState_t)(void *)GetProcAddress(xi, "XInputGetState");
    if (xi) xinput_set = (XInputSetState_t)(void *)GetProcAddress(xi, "XInputSetState");
}
static HGLRC glrc;
void plat_surface_close(void) {
    if (glrc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(glrc); glrc = NULL; }
    if (wnd) { if (dc) ReleaseDC(wnd, dc); DestroyWindow(wnd); }
    wnd = NULL; dc = NULL;
}
int plat_surface_open(int kind, int w, int h, const char *title) {
    plat_surface_close();   /* a pixel format can be set on a window only once, so each attempt gets a new window */
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dmrecomp"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.style = CS_OWNDC;
    RegisterClassA(&wc);   /* fails harmlessly when the class exists from an earlier attempt */
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    /* hidden until plat_surface_show: a failed attempt, or a backend that cannot use the surface, never flashes a window or takes the keyboard focus */
    wnd = CreateWindowA("dmrecomp", title, WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) { fprintf(stderr, "[video] cannot open a window\n"); return 0; }
    dc = GetDC(wnd);
    if (kind == PLAT_SURF_GL) {
        PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
        int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) { plat_surface_close(); return 0; }
        /* an OpenGL 3.3 core context: wglCreateContextAttribsARB is found through a plain context made current first, which is then dropped */
        glrc = wglCreateContext(dc);
        if (!glrc || !wglMakeCurrent(dc, glrc)) { plat_surface_close(); return 0; }
        typedef HGLRC (WINAPI *ctx_fn)(HDC, HGLRC, const int *);
        ctx_fn make_ctx = (ctx_fn)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
        static const int attribs[] = { 0x2091 /* major */, 3, 0x2092 /* minor */, 3, 0x9126 /* profile */, 1 /* core */, 0 };
        HGLRC core = make_ctx ? make_ctx(dc, NULL, attribs) : NULL;
        if (!core) { fprintf(stderr, "[video] no OpenGL 3.3 core context\n"); plat_surface_close(); return 0; }
        wglMakeCurrent(NULL, NULL); wglDeleteContext(glrc); glrc = core;
        if (!wglMakeCurrent(dc, glrc)) { plat_surface_close(); return 0; }
        /* pacing is done by host_frame, so do not also wait for the monitor */
        typedef BOOL (WINAPI *swap_fn)(int);
        swap_fn swap = (swap_fn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap) swap(0);
    } else if (kind != PLAT_SURF_SOFT && kind != PLAT_SURF_VK) { plat_surface_close(); return 0; }   /* a kind this platform does not know */
    return 1;
}
void plat_surface_show(void) { if (wnd) ShowWindow(wnd, SW_SHOW); }
static PFN_vkGetInstanceProcAddr vk_gipa;
void *plat_vk_loader(void) {
    if (!vk_gipa) {
        HMODULE m = LoadLibraryA("vulkan-1.dll");
        if (m) vk_gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vkGetInstanceProcAddr");
    }
    return (void *)vk_gipa;
}
int plat_vk_extensions(const char **names, int max) {
    if (max < 2) return 0;
    names[0] = "VK_KHR_surface"; names[1] = "VK_KHR_win32_surface";
    return 2;
}
int plat_vk_surface(void *instance, uint64_t *surface) {
    PFN_vkCreateWin32SurfaceKHR create = vk_gipa && wnd ? (PFN_vkCreateWin32SurfaceKHR)(void *)vk_gipa((VkInstance)instance, "vkCreateWin32SurfaceKHR") : NULL;
    if (!create) return 0;
    VkWin32SurfaceCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance = GetModuleHandleA(NULL), .hwnd = wnd };
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (create((VkInstance)instance, &ci, NULL, &s) != VK_SUCCESS) return 0;
    *surface = (uint64_t)s;
    return 1;
}
void plat_client_size(int *w, int *h) { RECT cr; GetClientRect(wnd, &cr); *w = cr.right; *h = cr.bottom; }
void plat_swap(void) { SwapBuffers(dc); }
void *plat_gl_proc(const char *name) { return (void *)wglGetProcAddress(name); }
/* without OpenGL, GDI stretches the picture */
void plat_soft_present(const uint32_t *fb, int fb_w, int fb_h, int cw, int ch, int w, int h, int smooth) {
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = fb_w; bi.bmiHeader.biHeight = -fb_h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    PatBlt(dc, 0, 0, cw, (ch - h) / 2, BLACKNESS);
    PatBlt(dc, 0, (ch + h) / 2, cw, ch, BLACKNESS);
    PatBlt(dc, 0, 0, (cw - w) / 2, ch, BLACKNESS);
    PatBlt(dc, (cw + w) / 2, 0, cw, ch, BLACKNESS);
    SetStretchBltMode(dc, smooth ? HALFTONE : COLORONCOLOR);
    if (smooth) SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, (cw - w) / 2, (ch - h) / 2, w, h, 0, 0, fb_w, fb_h, fb, &bi, DIB_RGB_COLORS, SRCCOPY);
}
int plat_focused(void) { return GetForegroundWindow() == wnd; }
int plat_replace_file(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0; }
void plat_mouse(int *buttons, int *wheel) {
    if (GetForegroundWindow() != wnd) { *buttons = *wheel = 0; mouse_wheel_acc = 0; return; }
    *buttons = mouse_buttons;
    *wheel = mouse_wheel_acc / WHEEL_DELTA; mouse_wheel_acc -= *wheel * WHEEL_DELTA;
}
int plat_mouse_pos(int *x, int *y) {
    POINT pt; RECT cr;
    if (GetForegroundWindow() != wnd || !GetCursorPos(&pt) || !ScreenToClient(wnd, &pt) || !GetClientRect(wnd, &cr)) return 0;
    if (pt.x < 0 || pt.y < 0 || pt.x >= cr.right || pt.y >= cr.bottom) return 0;
    *x = pt.x; *y = pt.y;
    return 1;
}
void plat_pump(void) {
    MSG m;
    memset(tapped, 0, sizeof tapped);   /* a tap counts until the next pump: one frame */
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

/* ------------------------------------------------------------- misc --- */
double plat_now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart; }
void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }
void plat_message(const char *text) { MessageBoxA(wnd, text, "dmrecomp", MB_ICONERROR); }
void plat_exit(int code) { ExitProcess((UINT)code); }
#endif
