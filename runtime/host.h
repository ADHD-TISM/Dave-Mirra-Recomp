/* What the host (host_common.c) needs from the platform: a window with an OpenGL
   context, keys, gamepads, a sound device and a clock. plat_win32.c provides it
   with the Win32 API, plat_sdl.c with SDL2 (Linux, Steam Deck; also builds on Windows). */
#ifndef HOST_H
#define HOST_H
#include <stdint.h>

/* Key codes: letters and digits are their upper-case ASCII value, the rest are these. */
enum { HK_UP = 256, HK_DOWN, HK_LEFT, HK_RIGHT, HK_ENTER, HK_BACKSPACE, HK_TAB, HK_SPACE, HK_LSHIFT, HK_RSHIFT,
       HK_LCTRL, HK_RCTRL, HK_LALT, HK_RALT, HK_COMMA, HK_PERIOD, HK_SLASH, HK_SEMICOLON,
       HK_NUM0, HK_NUM1, HK_NUM2, HK_NUM3, HK_NUM4, HK_NUM5, HK_NUM6, HK_NUM7, HK_NUM8, HK_NUM9, HK_COUNT };

/* Keys that change the display while playing: F11 / Alt+Enter, F6, F7, F8, F9, F10. */
enum { HOT_FULLSCREEN, HOT_SCALE, HOT_FILTER, HOT_ASPECT, HOT_FSR, HOT_WIDE };

/* A gamepad in the XInput layout. Sticks: +x is right, +y is up. */
enum { HP_UP = 0x0001, HP_DOWN = 0x0002, HP_LEFT = 0x0004, HP_RIGHT = 0x0008, HP_START = 0x0010, HP_BACK = 0x0020,
       HP_LS = 0x0040, HP_RS = 0x0080, HP_LB = 0x0100, HP_RB = 0x0200, HP_A = 0x1000, HP_B = 0x2000, HP_X = 0x4000, HP_Y = 0x8000 };
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } HostPad;

/* host_common.c, called by the platform while it pumps events */
void host_hotkey(int which);
void host_quit(void);               /* the window was closed */

/* the platform */
void plat_init(void);               /* before anything else */
int  plat_video_init(int w, int h, const char *title);   /* window; returns 1 with an OpenGL context current, 0 without one */
void plat_set_title(const char *title);
void plat_set_fullscreen(int on, int w, int h);          /* w = 0: the desktop's own resolution */
void plat_client_size(int *w, int *h);                   /* drawable size in pixels */
void plat_swap(void);                                    /* show the OpenGL back buffer */
void *plat_gl_proc(const char *name);                    /* OpenGL entry point by name, NULL if missing */
/* without OpenGL: draw the picture (0x00RRGGBB) as a w x h rectangle centred in the cw x ch window */
void plat_soft_present(const uint32_t *fb, int fb_w, int fb_h, int cw, int ch, int w, int h, int smooth);
void plat_pump(void);               /* handle window events; may call host_hotkey / host_quit */
int  plat_key(int code);            /* key held, and the window has the keyboard */
int  plat_pad(int n, HostPad *p);   /* gamepad n (0 or 1): fills p and returns 1 when it is connected */
void plat_pad_rumble(int n, uint16_t large, uint16_t small);
void plat_audio_init(void);
void plat_audio_push(const int16_t *lr, int frames);     /* stereo, 44100 Hz; dropped when the device is behind */
double plat_now_ms(void);
void plat_sleep_ms(int ms);
void plat_message(const char *text);                     /* error box */
void plat_exit(int code);
#endif
