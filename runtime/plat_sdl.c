/* SDL2 platform: window, OpenGL context, keys, game controllers, sound, clock.
   Used on Linux and the Steam Deck; it also builds on Windows (cmake -DDM_SDL=ON). */
#ifdef DM_SDL   /* the whole file: a build that compiles every runtime file without choosing gets the Win32 one */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "host.h"

static SDL_Window *win;
static SDL_GLContext glc;
static SDL_Renderer *ren;            /* only without OpenGL */
static SDL_Texture *tex; static int tex_w, tex_h, tex_smooth;
static SDL_GameController *pads[2];
static SDL_AudioDeviceID adev;

/* Every SDL call (start-up, events, OpenGL, shutdown) is made from the thread the game runs on,
   which outside Windows is not the process's first thread (see main.c). */
void plat_init(void) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "FATAL: SDL cannot start video: %s\n", SDL_GetError()); exit(1); }
    /* no sound device or no controller support is not a reason to stop */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "[audio] no sound: %s\n", SDL_GetError());
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) fprintf(stderr, "[input] no gamepad support: %s\n", SDL_GetError());
}

/* ------------------------------------------------------------ video --- */
int plat_video_init(int w, int h, const char *title) {
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* the picture is drawn with OpenGL 1.x calls plus optional 2.0 shaders: ask for a compatibility context */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win) {
        glc = SDL_GL_CreateContext(win);
        if (glc) { SDL_GL_SetSwapInterval(0); return 1; }   /* pacing is done by host_frame, so do not also wait for the monitor */
        SDL_DestroyWindow(win);
    }
    fprintf(stderr, "[video] no OpenGL (%s); drawing without it\n", SDL_GetError());
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "FATAL: cannot open a window: %s\n", SDL_GetError()); exit(1); }
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "FATAL: cannot draw in the window: %s\n", SDL_GetError()); exit(1); }
    return 0;
}
void plat_set_title(const char *title) { SDL_SetWindowTitle(win, title); }
void plat_set_fullscreen(int on, int w, int h) {
    static int is_on;
    if (on == is_on) return;
    if (!on) SDL_SetWindowFullscreen(win, 0);
    else {
        SDL_DisplayMode want = { 0 }, got;
        int d = SDL_GetWindowDisplayIndex(win);
        want.w = w; want.h = h;
        /* a fullscreen resolution other than the desktop's: switch the display mode, if it has one that fits */
        if (w > 0 && d >= 0 && SDL_GetClosestDisplayMode(d, &want, &got) && SDL_SetWindowDisplayMode(win, &got) == 0
            && SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN) == 0) { }
        else {
            if (w > 0) fprintf(stderr, "[video] the display cannot switch to %dx%d; using the desktop resolution\n", w, h);
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }
    is_on = on;
    SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
}
void plat_client_size(int *w, int *h) {
    if (glc) SDL_GL_GetDrawableSize(win, w, h);
    else if (SDL_GetRendererOutputSize(ren, w, h) != 0) SDL_GetWindowSize(win, w, h);
}
void plat_swap(void) { SDL_GL_SwapWindow(win); }
void *plat_gl_proc(const char *name) { return SDL_GL_GetProcAddress(name); }
void plat_soft_present(const uint32_t *fb, int fb_w, int fb_h, int cw, int ch, int w, int h, int smooth) {
    if (!ren) return;
    if (!tex || tex_w != fb_w || tex_h != fb_h || tex_smooth != smooth) {
        if (tex) SDL_DestroyTexture(tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, smooth ? "1" : "0");
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, fb_w, fb_h);
        tex_w = fb_w; tex_h = fb_h; tex_smooth = smooth;
        if (!tex) return;
    }
    SDL_UpdateTexture(tex, NULL, fb, fb_w * 4);
    SDL_Rect dst = { (cw - w) / 2, (ch - h) / 2, w, h };
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------ input --- */
static void pad_added(int device) {
    if (!SDL_IsGameController(device)) return;
    SDL_GameController *c = SDL_GameControllerOpen(device);
    if (!c) return;
    for (int i = 0; i < 2; i++) if (pads[i] == c) { SDL_GameControllerClose(c); return; }   /* already ours: opening again took a second reference */
    for (int i = 0; i < 2; i++) if (!pads[i]) { pads[i] = c; return; }
    SDL_GameControllerClose(c);   /* a third pad: not used */
}
static void pad_removed(SDL_JoystickID id) {
    for (int i = 0; i < 2; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) { SDL_GameControllerClose(pads[i]); pads[i] = NULL; }
}
void plat_pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: host_quit(); break;
        case SDL_CONTROLLERDEVICEADDED: pad_added(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: pad_removed(e.cdevice.which); break;
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            switch (e.key.keysym.sym) {
            case SDLK_RETURN: if (e.key.keysym.mod & KMOD_ALT) host_hotkey(HOT_FULLSCREEN); break;
            case SDLK_F11: host_hotkey(HOT_FULLSCREEN); break;
            case SDLK_F6: host_hotkey(HOT_SCALE); break;
            case SDLK_F7: host_hotkey(HOT_FILTER); break;
            case SDLK_F8: host_hotkey(HOT_ASPECT); break;
            case SDLK_F9: host_hotkey(HOT_FSR); break;
            case SDLK_F10: host_hotkey(HOT_WIDE); break;
            }
            break;
        }
    }
}
static const SDL_Scancode sc_of[HK_COUNT - 256] = {
    [HK_UP - 256] = SDL_SCANCODE_UP, [HK_DOWN - 256] = SDL_SCANCODE_DOWN, [HK_LEFT - 256] = SDL_SCANCODE_LEFT, [HK_RIGHT - 256] = SDL_SCANCODE_RIGHT,
    [HK_ENTER - 256] = SDL_SCANCODE_RETURN, [HK_BACKSPACE - 256] = SDL_SCANCODE_BACKSPACE, [HK_TAB - 256] = SDL_SCANCODE_TAB,
    [HK_SPACE - 256] = SDL_SCANCODE_SPACE, [HK_LSHIFT - 256] = SDL_SCANCODE_LSHIFT, [HK_RSHIFT - 256] = SDL_SCANCODE_RSHIFT,
    [HK_LCTRL - 256] = SDL_SCANCODE_LCTRL, [HK_RCTRL - 256] = SDL_SCANCODE_RCTRL, [HK_LALT - 256] = SDL_SCANCODE_LALT, [HK_RALT - 256] = SDL_SCANCODE_RALT,
    [HK_COMMA - 256] = SDL_SCANCODE_COMMA, [HK_PERIOD - 256] = SDL_SCANCODE_PERIOD, [HK_SLASH - 256] = SDL_SCANCODE_SLASH,
    [HK_SEMICOLON - 256] = SDL_SCANCODE_SEMICOLON,
    [HK_NUM0 - 256] = SDL_SCANCODE_KP_0, [HK_NUM1 - 256] = SDL_SCANCODE_KP_1, [HK_NUM2 - 256] = SDL_SCANCODE_KP_2, [HK_NUM3 - 256] = SDL_SCANCODE_KP_3,
    [HK_NUM4 - 256] = SDL_SCANCODE_KP_4, [HK_NUM5 - 256] = SDL_SCANCODE_KP_5, [HK_NUM6 - 256] = SDL_SCANCODE_KP_6, [HK_NUM7 - 256] = SDL_SCANCODE_KP_7,
    [HK_NUM8 - 256] = SDL_SCANCODE_KP_8, [HK_NUM9 - 256] = SDL_SCANCODE_KP_9 };
int plat_key(int code) {
    if (SDL_GetKeyboardFocus() != win) return 0;
    SDL_Scancode sc = code < 256 ? SDL_GetScancodeFromKey((SDL_Keycode)tolower(code))   /* the key that types this letter or digit */
                    : code < HK_COUNT ? sc_of[code - 256] : SDL_SCANCODE_UNKNOWN;
    int n; const Uint8 *st = SDL_GetKeyboardState(&n);
    return sc != SDL_SCANCODE_UNKNOWN && (int)sc < n && st[sc];
}
static int16_t flip(int v) { v = -v; return (int16_t)(v > 32767 ? 32767 : v); }   /* SDL sticks: +y is down */
int plat_pad(int n, HostPad *p) {
    static const struct { SDL_GameControllerButton b; uint16_t bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP, HP_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, HP_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, HP_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, HP_RIGHT }, { SDL_CONTROLLER_BUTTON_START, HP_START }, { SDL_CONTROLLER_BUTTON_BACK, HP_BACK },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, HP_LS }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, HP_RS }, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, HP_LB },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, HP_RB }, { SDL_CONTROLLER_BUTTON_A, HP_A }, { SDL_CONTROLLER_BUTTON_B, HP_B },
        { SDL_CONTROLLER_BUTTON_X, HP_X }, { SDL_CONTROLLER_BUTTON_Y, HP_Y } };
    SDL_GameController *c = n >= 0 && n < 2 ? pads[n] : NULL;
    if (!c || !SDL_GameControllerGetAttached(c)) return 0;
    p->buttons = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (SDL_GameControllerGetButton(c, map[i].b)) p->buttons |= map[i].bit;
    p->lt = (uint8_t)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    p->rt = (uint8_t)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    p->lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX); p->ly = flip(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    p->rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX); p->ry = flip(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    return 1;
}
void plat_pad_rumble(int n, uint16_t large, uint16_t small) {
    /* the host only calls on a change, so ask for a long time; zero strengths stop it */
    if (n >= 0 && n < 2 && pads[n]) SDL_GameControllerRumble(pads[n], large, small, 60000);
}

/* ------------------------------------------------------------ audio --- */
#define QUEUE_MAX_BYTES (8 * 1024 * 4)   /* as much as the Win32 host buffers: 8 blocks of 1024 frames */
void plat_audio_init(void) {
    SDL_AudioSpec want = { 0 }, have;
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);   /* 0: SDL converts if the device wants another format */
    if (!adev) { fprintf(stderr, "[audio] no sound: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(adev, 0);
}
void plat_audio_push(const int16_t *lr, int frames) {
    if (!adev || SDL_GetQueuedAudioSize(adev) > QUEUE_MAX_BYTES) return;   /* device is behind: drop */
    SDL_QueueAudio(adev, lr, (Uint32)frames * 4);
}

/* ------------------------------------------------------------- misc --- */
double plat_now_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
void plat_sleep_ms(int ms) { SDL_Delay((Uint32)ms); }
void plat_message(const char *text) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "dmrecomp", text, win); }
void plat_exit(int code) { SDL_Quit(); exit(code); }
#endif
