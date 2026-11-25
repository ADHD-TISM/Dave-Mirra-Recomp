/* What the host (host_common.c) needs from the platform: a window with an OpenGL
   context, keys, gamepads, a sound device and a clock. plat_win32.c provides it
   with the Win32 API, plat_sdl.c with SDL2 (Linux, Steam Deck; also builds on Windows). */
#ifndef HOST_H
#define HOST_H
#include <stdint.h>

/* Key codes: letters and digits are their upper-case ASCII value, the rest are these. */
enum { HK_UP = 256, HK_DOWN, HK_LEFT, HK_RIGHT, HK_ENTER, HK_BACKSPACE, HK_TAB, HK_SPACE, HK_LSHIFT, HK_RSHIFT,
       HK_LCTRL, HK_RCTRL, HK_LALT, HK_RALT, HK_COMMA, HK_PERIOD, HK_SLASH, HK_SEMICOLON,
       HK_NUM0, HK_NUM1, HK_NUM2, HK_NUM3, HK_NUM4, HK_NUM5, HK_NUM6, HK_NUM7, HK_NUM8, HK_NUM9,
       HK_ESCAPE, HK_INSERT, HK_DELETE, HK_HOME, HK_END, HK_PAGEUP, HK_PAGEDOWN, HK_CAPSLOCK,
       HK_MINUS, HK_EQUALS, HK_LBRACKET, HK_RBRACKET, HK_BACKSLASH, HK_QUOTE, HK_GRAVE,
       HK_NUMADD, HK_NUMSUB, HK_NUMMUL, HK_NUMDIV, HK_NUMDOT, HK_NUMENTER,
       HK_F1, HK_F2, HK_F3, HK_F4, HK_F5, HK_F6, HK_F7, HK_F8, HK_F9, HK_F10, HK_F11, HK_F12, HK_COUNT };

/* Display actions on keys the player can choose (defaults F11, F6, F7, F8, F9, F10); Alt+Enter also does HOT_FULLSCREEN. */
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
/* The window and the surface a graphics backend draws on (gfx.h). plat_surface_open makes both for one kind of surface and
   returns 1; when this platform cannot give that kind it leaves nothing open and returns 0, so the caller can try another kind.
   PLAT_SURF_GL: an OpenGL 3.3 core context, current on the calling thread. PLAT_SURF_SOFT: a plain window the CPU picture
   is stretched into with plat_soft_present. Any other kind returns 0. */
enum { PLAT_SURF_GL = 1, PLAT_SURF_SOFT = 2, PLAT_SURF_VK = 3 };   /* PLAT_SURF_VK: a window a Vulkan surface can be made for (see below) */
int  plat_surface_open(int kind, int w, int h, const char *title);
void plat_surface_close(void);                           /* the window and its surface; no-op when none is open */
void plat_surface_show(void);                            /* the window stays hidden until the backend has what it needs, then this shows it */
/* Vulkan, after plat_surface_open(PLAT_SURF_VK): the platform opens the system loader (vulkan-1.dll, libvulkan.so.1) at run time */
void *plat_vk_loader(void);                              /* the loader's vkGetInstanceProcAddr, NULL when there is no loader */
int  plat_vk_extensions(const char **names, int max);    /* instance extensions the window's surface needs; the count, 0 on failure */
int  plat_vk_surface(void *instance, uint64_t *surface); /* make the VkSurfaceKHR for the window; 1 on success */
void plat_set_title(const char *title);
int  plat_wants_fullscreen(void);   /* the system runs this as a fullscreen app (Steam Deck Game Mode) */
void plat_set_fullscreen(int on, int w, int h);          /* w = 0: the desktop's own resolution */
void plat_client_size(int *w, int *h);                   /* drawable size in pixels */
void plat_swap(void);                                    /* show the OpenGL back buffer */
void *plat_gl_proc(const char *name);                    /* OpenGL entry point by name, NULL if missing */
/* without OpenGL: draw the picture (0x00RRGGBB) as a w x h rectangle centred in the cw x ch window */
void plat_soft_present(const uint32_t *fb, int fb_w, int fb_h, int cw, int ch, int w, int h, int smooth);
void plat_pump(void);               /* handle window events; may call host_hotkey / host_quit */
/* The mouse over the window: buttons held (bit 0 left, 1 right, 2 middle, 3 and 4 the side buttons) and wheel notches
   since the last call (+ is away from the player). Both zero without focus. */
void plat_mouse(int *buttons, int *wheel);
/* the buttons that went down since the last plat_pump, even if already up again (a click shorter than a menu turn) */
int  plat_mouse_pressed(void);
/* where the pointer is, in the drawable's pixels (as plat_client_size counts them); 0 without focus or outside the window */
int  plat_mouse_pos(int *x, int *y);
void plat_menu_cursor(int on);      /* the in-game menu is open: show the pointer, also in fullscreen */
int  plat_focused(void);            /* the window has the keyboard (it is the one in front) */
/* put the file "from" in the place of "to" in one step: there is no moment without "to". 1 when done. */
int  plat_replace_file(const char *from, const char *to);
int  plat_key(int code);            /* key held, or tapped since the last plat_pump; and the window has the keyboard */
int  plat_pad(int n, HostPad *p);   /* gamepad n (0 or 1): fills p and returns 1 when it is connected */
void plat_pad_rumble(int n, uint16_t large, uint16_t small);
void plat_audio_init(void);
void plat_audio_push(const int16_t *lr, int frames);     /* stereo, 44100 Hz; dropped when the device is behind */
double plat_now_ms(void);
void plat_sleep_ms(int ms);
void plat_message(const char *text);                     /* error box */
void plat_exit(int code);
#endif
