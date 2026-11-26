/* The in-game menu: part of host_common.c (included once, after present and save_png), kept apart for its size.
   The menu key (key_menu, Escape unless changed) or the gamepad controls of pad_menu (Back + Start unless changed) open it. While it is open the
   game stands still: host_frame does not return, so no guest code runs, no sound is made and no game time passes. The
   menu is drawn by the CPU into its own 640x480 picture, over a darkened copy of the game's last frame, and shown through
   the backend's ordinary CPU-picture path, so it looks the same under Vulkan, OpenGL and the software stretch, with the
   GPU renderer or without. A setting changed here takes effect at once and is written to dmrecomp.ini at once too (the
   window can be closed with the menu open): only its own line is replaced, the rest of the file is kept. */
#include "font8x8.h"

#define MENU_W 640
#define MENU_H 480
static u32 *menu_bg, *menu_px;      /* the darkened game picture, and the picture shown */

/* Replace the line "name = ..." in the settings file, or add it; every other line is kept as it is. A second line with the
   same name is dropped: the game reads the file top to bottom, so that one would win at the next start. The new text is
   written beside the file and then takes its place in one step (plat_replace_file), so there is no moment without a
   settings file; when the old file cannot be read through, it is left alone. */
static void config_store(const char *name, const char *val) {
    if (!config_path[0]) return;
    char tmp[530]; snprintf(tmp, sizeof tmp, "%s.new", config_path);
    errno = 0;
    FILE *in = fopen(config_path, "r");
    if (!in && errno != ENOENT) { fprintf(stderr, "[menu] could not read %s; %s is not saved\n", config_path, name); return; }
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    char line[512]; int done = 0, line_start = 1, ends_nl = 1;
    while (in && fgets(line, sizeof line, in)) {
        size_t len = strlen(line);
        if (line_start) {
            char key[64]; const char *q = line; size_t k = 0;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != '=' && *q != ' ' && *q != '\t' && *q != '\n' && k < sizeof key - 1) key[k++] = *q++;
            key[k] = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=' && !_stricmp(key, name)) {
                if (!done) fprintf(out, "%s = %s\n", name, val);
                done = 1;
                while (len && line[len - 1] != '\n' && fgets(line, sizeof line, in)) len = strlen(line);   /* the rest of a very long line */
                line_start = ends_nl = 1;
                continue;
            }
        }
        fputs(line, out);
        line_start = ends_nl = len && line[len - 1] == '\n';
    }
    int bad = in && ferror(in);
    if (!done) fprintf(out, "%s%s = %s\n", ends_nl ? "" : "\n", name, val);
    if (in) fclose(in);
    if (fclose(out) != 0 || bad || !plat_replace_file(tmp, config_path)) {   /* a full disk, a read error: the old file stays */
        remove(tmp);
        fprintf(stderr, "[menu] could not write %s; %s is not saved\n", config_path, name);
    }
}

/* ---- drawing ---- */
static void menu_fill(int x, int y, int w, int h, u32 col) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < MENU_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < MENU_W; xx++) menu_px[yy * MENU_W + xx] = col;
}
static void menu_text(int x, int y, int sc, const char *s, u32 col) {   /* sc: pixels per font dot */
    for (; *s; s++, x += 8 * sc) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const unsigned char *g = font8x8[c - 32];
        for (int r = 0; r < 8; r++) for (int b = 0; b < 8; b++) if (g[r] >> b & 1) menu_fill(x + b * sc, y + r * sc, sc, sc, col);
    }
}

/* ---- the entries ---- */
enum { MI_RESUME, MI_SCALE, MI_FILTER, MI_ASPECT, MI_FSR, MI_WIDE, MI_FULL, MI_VOLUME, MI_RUMBLE, MI_EXIT, MI_COUNT };
static const char *const menu_names[MI_COUNT] = { "Resume", "Internal resolution", "Picture filter", "Aspect ratio", "FSR upscaler",
                                                  "Widescreen", "Fullscreen", "Volume", "Rumble", "Exit game" };
static const struct { double ratio; const char *name; } menu_wide[] = { { 0, "off" }, { 16.0 / 10, "16:10" }, { 16.0 / 9, "16:9" }, { 21.0 / 9, "21:9" } };   /* narrow to wide */
static int menu_wide_at(void) {   /* which of the shapes is set; -1 for one typed into the settings file */
    for (int i = 0; i < 4; i++) if (fabs(ws_ratio - menu_wide[i].ratio) < 0.005) return i;
    return -1;
}
/* the entry's value as shown, and (ini) as the settings file spells it; "" for an entry without a value */
static const char *menu_value(int item, int ini) {
    switch (item) {
    case MI_SCALE: return g_gpu_scale == 1 ? (ini ? "1" : "1x") : g_gpu_scale == 2 ? (ini ? "2" : "2x") : (ini ? "4" : "4x");   /* short: the label is long */
    case MI_FILTER: return g_filter ? "smooth" : ini ? "nearest" : "sharp";
    case MI_ASPECT: return g_aspect ? "stretch" : "4:3";
    case MI_FSR: return g_upscale ? (ini ? "fsr" : "on") : "off";
    case MI_WIDE: { int i = menu_wide_at(); return i < 0 ? "custom" : menu_wide[i].name; }
    case MI_FULL: return g_fullscreen ? (ini ? "1" : "on") : (ini ? "0" : "off");
    case MI_VOLUME: { static char t[8]; snprintf(t, sizeof t, ini ? "%d" : "%d%%", g_volume); return t; }
    case MI_RUMBLE: return g_rumble ? (ini ? "1" : "on") : (ini ? "0" : "off");
    }
    return "";
}
static const char *const menu_ini[MI_COUNT] = { NULL, "scale", "filter", "aspect", "upscale", "widescreen", "fullscreen", "volume", "rumble", NULL };
static void menu_change(int item, int dir) {   /* dir: +1 the next value, -1 the one before */
    switch (item) {
    case MI_SCALE: {
        int at = g_gpu_scale == 1 ? 0 : g_gpu_scale == 2 ? 1 : 2;
        at = (at + dir + 3) % 3; g_gpu_scale = 1 << at; gpu_set_scale(); break;
    }
    case MI_FILTER: g_filter = !g_filter; break;
    case MI_ASPECT: g_aspect = !g_aspect; break;
    case MI_FSR: g_upscale = !g_upscale; break;
    case MI_WIDE: {
        int i = menu_wide_at();
        if (i >= 0) i = (i + dir + 4) % 4;
        else {   /* a shape typed into the settings file: the nearest of ours on the side asked for, else off */
            i = 0;
            for (int k = 1; k < 4; k++) {
                double d = (menu_wide[k].ratio - ws_ratio) * dir;
                if (d > 0 && (!i || d < (menu_wide[i].ratio - ws_ratio) * dir)) i = k;
            }
        }
        ws_ratio = menu_wide[i].ratio; break;
    }
    case MI_FULL: set_fullscreen(!g_fullscreen); break;
    case MI_VOLUME: {   /* steps of 10, stopping at the ends; a value between steps goes to the next step first */
        int v = dir > 0 ? (g_volume / 10 + 1) * 10 : ((g_volume + 9) / 10 - 1) * 10;
        g_volume = v < 0 ? 0 : v > 100 ? 100 : v; break;
    }
    case MI_RUMBLE: g_rumble = !g_rumble; break;
    }
    show_settings();
}

/* the menu's box and its entry rows, shared by the drawing and the pointer: row i's text is at MENU_ROW_Y(i), and its
   highlight bar is MENU_BAR_H high from MENU_BAR_UP pixels above that, MENU_BAR_X in from each side of the box */
enum { MENU_PX = 72, MENU_PY = 44, MENU_PW = MENU_W - 144, MENU_PH = MENU_H - 88,
       MENU_ROW_STEP = 27, MENU_BAR_H = 26, MENU_BAR_X = 12, MENU_BAR_UP = 5 };
#define MENU_ROW_Y(i) (MENU_PY + 56 + (i) * MENU_ROW_STEP)   /* 10 rows above the 3 lines of help */
static void menu_draw(int sel) {
    memcpy(menu_px, menu_bg, (size_t)MENU_W * MENU_H * 4);
    const int px = MENU_PX, py = MENU_PY, pw = MENU_PW, ph = MENU_PH;
    menu_fill(px - 2, py - 2, pw + 4, ph + 4, 0x5a7fb8);
    menu_fill(px, py, pw, ph, 0x10161f);
    const char *title = "GAME MENU";
    menu_text((MENU_W - (int)strlen(title) * 16) / 2, py + 14, 2, title, 0xffffff);
    menu_fill(px + 16, py + 40, pw - 32, 2, 0x5a7fb8);
    for (int i = 0; i < MI_COUNT; i++) {
        int y = MENU_ROW_Y(i);
        if (i == sel) menu_fill(px + MENU_BAR_X, y - MENU_BAR_UP, pw - 2 * MENU_BAR_X, MENU_BAR_H, 0x2a5db0);
        u32 col = i == sel ? 0xffffff : 0xc0c8d0;
        menu_text(px + 24, y, 2, menu_names[i], col);
        const char *v = menu_value(i, 0);
        if (*v) {
            char buf[40]; snprintf(buf, sizeof buf, i == sel ? "< %s >" : "%s", v);
            menu_text(px + pw - 24 - (int)strlen(buf) * 16, y, 2, buf, i == sel ? 0xffffff : 0x8fd0ff);
        }
    }
    /* at most 56 characters fit in a line of this size */
    menu_text(px + 24, py + ph - 58, 1, "Up / Down or the mouse: choose    Left / Right: change", 0x9aa4b0);
    menu_text(px + 24, py + ph - 44, 1, "Cross or Enter: select   Triangle or the menu key: back", 0x9aa4b0);
    menu_text(px + 24, py + ph - 24, 1, "Changes are saved. More settings are in the launcher.", 0x6f7a88);
}
/* where the menu picture is in the window: w x h, centred (every backend places it so) */
static int menu_place(int *cw, int *ch, int *w, int *h) {
    plat_client_size(cw, ch);
    if (*cw <= 0 || *ch <= 0) return 0;
    *w = *cw; *h = *ch;
    if (!g_aspect) { *h = *cw * 3 / 4; if (*h > *ch) { *h = *ch; *w = *ch * 4 / 3; } }
    return *w > 0 && *h > 0;   /* a window a pixel or two wide has no room for a 4:3 picture */
}
/* the entry under the pointer, -1 for none; a row is its highlight bar in menu_draw */
static int menu_row_at_pointer(void) {
    int mx, my, cw, ch, w, h;
    if (!plat_mouse_pos(&mx, &my) || !menu_place(&cw, &ch, &w, &h)) return -1;
    int x = (int)((long long)(mx - (cw - w) / 2) * MENU_W / w), y = (int)((long long)(my - (ch - h) / 2) * MENU_H / h);
    const int top = MENU_ROW_Y(0) - MENU_BAR_UP;   /* the first highlight bar's top */
    if (x < MENU_PX + MENU_BAR_X || x >= MENU_PX + MENU_PW - MENU_BAR_X || y < top) return -1;
    int i = (y - top) / MENU_ROW_STEP;
    return i < MI_COUNT && (y - top) % MENU_ROW_STEP < MENU_BAR_H ? i : -1;
}
static void menu_show(void) {
    int cw, ch, w, h;
    if (!menu_place(&cw, &ch, &w, &h)) return;
    GfxFrame f; memset(&f, 0, sizeof f);
    f.fb = menu_px; f.fb_w = MENU_W; f.fb_h = MENU_H;
    f.cw = cw; f.ch = ch; f.w = w; f.h = h;
    f.filter = 1; f.fsr_sharp = g_fsr_sharp;   /* blended, never FSR: the text is small */
    gfx->present(&f);
}

/* ---- input: the pad buttons as the game would get them, plus keys that always work here ---- */
enum { MB_UP = 1 << 4, MB_RIGHT = 1 << 5, MB_DOWN = 1 << 6, MB_LEFT = 1 << 7, MB_START = 1 << 3, MB_SELECT = 1 << 0,
       MB_CROSS = 1 << 14, MB_TRIANGLE = 1 << 12, MB_MENU = 1 << 16,
       MB_HELD = 1 << 17 };   /* one of pad_menu's controls is down: no action, but the wait after the menu must see it */
static int menu_pad_combo;   /* pad_menu is held on player 1's gamepad */
static u32 menu_buttons(void) {
    static unsigned probe;
    u16 b = 0; u32 r;
    for (int i = 0; i < 16; i++) for (int j = 0; j < MAXKEYS && keymap[i][j]; j++) if (key(keymap[i][j])) { b |= (u16)(1 << i); break; }
    mouse_input(&b);
    HostPad s;
    menu_pad_combo = 0; int menu_pad_one = 0;
    if (!test_nopad && !pads_away() && (pad_found[0] || probe++ % 60 == 0) && (pad_found[0] = plat_pad(0, &s)) != 0) {
        /* The controls that open the menu (pad_menu) mean only that in here, alone or together: with back+start, Start
           would otherwise select the highlighted entry on the way to closing the menu, and that entry may be Exit. */
        for (int i = 0; i < 16; i++)
            if (padmap[i] != X_NONE && padmap[i] != pad_menu[0] && padmap[i] != pad_menu[1] && xsrc(&s, padmap[i])) b |= (u16)(1 << i);
        if (s.ly > 16000) b |= MB_UP;
        if (s.ly < -16000) b |= MB_DOWN;
        if (s.lx < -16000) b |= MB_LEFT;
        if (s.lx > 16000) b |= MB_RIGHT;
        menu_pad_combo = pad_menu_held(&s) && plat_focused();
        /* those controls are not in b (above), so one of a pair held alone would be invisible: after a close with
           back+start, Back let go first, the wait for "nothing held" would end with Start down, and the game would get it */
        menu_pad_one = (pad_menu[0] && xsrc(&s, pad_menu[0])) || (pad_menu[1] && xsrc(&s, pad_menu[1]));
    }
    r = b;
    if (mouse_held) r |= MB_HELD;   /* a click that closed the menu must be let go before the game runs again */
    if (key(HK_UP)) r |= MB_UP;
    if (key(HK_DOWN)) r |= MB_DOWN;
    if (key(HK_LEFT)) r |= MB_LEFT;
    if (key(HK_RIGHT)) r |= MB_RIGHT;
    if (key(HK_ENTER)) r |= MB_CROSS;
    if (key(key_menu) || key(HK_ESCAPE) || menu_pad_combo) r |= MB_MENU;
    if (menu_pad_one) r |= MB_HELD;
    return r;
}
/* The four directions from the keyboard alone (the arrows and the keys mapped to them), not the menu key */
static u32 menu_arrow_keys(void) {
    static const int hk[4] = { HK_UP, HK_RIGHT, HK_DOWN, HK_LEFT };   /* pad bits 4 to 7: MB_UP MB_RIGHT MB_DOWN MB_LEFT */
    u32 r = 0;
    for (int i = 4; i < 8; i++) {
        int on = hk[i - 4] != key_menu && key(hk[i - 4]);   /* key_menu may be an arrow (key_parse) */
        for (int j = 0; !on && j < MAXKEYS && keymap[i][j]; j++)
            on = keymap[i][j] != key_menu && keymap[i][j] != HK_ESCAPE && key(keymap[i][j]);
        if (on) r |= 1u << i;
    }
    return r;
}

/* --menu-at FRAME:KEYS (a test aid): the menu opens at that frame and gets these presses, one a turn: u d l r, x (select),
   t (back) and m (the menu controls). After the last one menu_<frame>.png is written and the menu closes, unless a press already closed it. */
static unsigned menu_test_frame; static const char *menu_test_keys;
void host_set_menu_test(const char *t) {
    menu_test_frame = (unsigned)strtoul(t, (char **)&t, 10);
    menu_test_keys = *t == ':' ? t + 1 : "";
}

static void menu_run(unsigned frame, const char *test) {
    if (!menu_bg) { menu_bg = malloc((size_t)MENU_W * MENU_H * 4); menu_px = malloc((size_t)MENU_W * MENU_H * 4); }
    if (!menu_bg || !menu_px) return;
    host_pad_rumble(0, 0, 0); host_pad_rumble(1, 0, 0);
    menu_capture = 1; present(); menu_capture = 0;   /* fb now holds the game's picture, also when the graphics card drew it */
    for (int y = 0; y < MENU_H; y++) for (int x = 0; x < MENU_W; x++)
        menu_bg[y * MENU_W + x] = (fb[(y * fb_h / MENU_H) * fb_w + x * fb_w / MENU_W] >> 2) & 0x3f3f3fu;
    int sel = 0, quit = 0;
    mouse_clicks_own = 1;
    if (!test) plat_menu_cursor(1);
    u32 prev = test ? 0 : menu_buttons();   /* what is held now does not count as a press */
    /* ...but a direction tapped in the same frame as the menu key, down and up already, is a press: the menu's first
       plat_pump forgets it (plat_key), so it counts in the first turn. Escape and Down at once: Down was lost. */
    u32 early = 0;
    if (!test && g_tap_hold) { early = menu_arrow_keys(); g_tap_hold = 0; early &= ~menu_arrow_keys(); g_tap_hold = 1; }
    int prev_mouse = mouse_held, last_row = test ? -1 : menu_row_at_pointer();   /* a pointer resting on a row does not take the choice */
    int last_cw = 0, last_ch = 0, last_aspect = g_aspect;
    if (!test) plat_client_size(&last_cw, &last_ch);
    for (;;) {
        u32 hit;
        if (test) {
            char c = *test ? *test++ : 0;
            hit = c == 'u' ? MB_UP : c == 'd' ? MB_DOWN : c == 'l' ? MB_LEFT : c == 'r' ? MB_RIGHT : c == 'x' ? MB_CROSS : c == 't' ? MB_TRIANGLE
                : c == 'm' ? MB_MENU | MB_START | MB_SELECT : 0;   /* m: the menu controls, with the meanings they must not have here */
            if (!c) {
                menu_draw(sel); menu_show();
                char name[64]; snprintf(name, sizeof name, "menu_%05u.png", frame);
                u32 *keep = fb; int kw = fb_w, kh = fb_h, ks = g_shot_window;
                fb = menu_px; fb_w = MENU_W; fb_h = MENU_H; g_shot_window = 0;
                save_png(name);
                fb = keep; fb_w = kw; fb_h = kh; g_shot_window = ks;
                break;
            }
        } else {
            plat_pump();
            u32 b = menu_buttons();
            hit = (b & ~prev) | early; prev = b; early = 0;
            /* the mouse: moving onto an entry chooses it; a left click works it like Cross, a right click on an entry
               with a value steps back one value, and a right click anywhere else is Back */
            /* a click counts once: as a button that is down now and was not last turn, or as one that went down and
               came up again inside this turn (a fast click, a touchpad tap), which mouse_held never sees */
            int row = menu_row_at_pointer(), clicks = (mouse_held & ~prev_mouse) | (test_nopad ? 0 : plat_mouse_pressed());   /* nopad: as mouse_input */
            prev_mouse = mouse_held;
            /* The picture moving under a pointer that stays put is not a choice: a change of Fullscreen or Aspect
               resizes or re-centres it, and the window may take a few turns to get its new size. */
            int cw, ch; plat_client_size(&cw, &ch);
            if (cw == last_cw && ch == last_ch && g_aspect == last_aspect && row >= 0 && row != last_row) sel = row;
            last_row = row; last_cw = cw; last_ch = ch; last_aspect = g_aspect;
            if ((clicks & 1) && row >= 0) { sel = row; hit |= MB_CROSS; }
            if (clicks & 2) {
                if (row >= 0 && menu_ini[row]) { sel = row; hit |= MB_LEFT; }
                else hit |= MB_TRIANGLE;
            }
        }
        if (hit & MB_MENU) break;   /* before anything else: the press that closes the menu selects nothing */
        if (hit & MB_UP) sel = (sel + MI_COUNT - 1) % MI_COUNT;
        if (hit & MB_DOWN) sel = (sel + 1) % MI_COUNT;
        int dir = (hit & MB_RIGHT) ? 1 : (hit & MB_LEFT) ? -1 : 0;
        if (hit & (MB_CROSS | MB_START)) {
            if (sel == MI_RESUME) break;
            if (sel == MI_EXIT) { quit = 1; break; }
            dir = 1;
        }
        if (dir && menu_ini[sel]) {
            menu_change(sel, dir); config_store(menu_ini[sel], menu_value(sel, 1));   /* saved at once */
            if (!test) last_row = menu_row_at_pointer();   /* the change may have moved the picture (above) */
        }
        if (hit & (MB_TRIANGLE | MB_SELECT)) break;
        if (!test) { menu_draw(sel); menu_show(); plat_sleep_ms(16); }
    }
    if (quit) host_quit();
    /* the button that closed the menu must not reach the game: wait until nothing is held (2 seconds at most) */
    for (int n = 0; !test && n < 120 && menu_buttons(); n++) { plat_pump(); plat_sleep_ms(16); }
    mouse_clicks_own = 0;
    if (!test) plat_menu_cursor(0);
}
