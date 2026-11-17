/* Drives runtime/plat_sdl.c's gamepad code with SDL virtual controllers (no hardware): button, stick and trigger
   mapping, hotplug into two slots, rumble not crashing. From the repository root:
     gcc -DDM_SDL -I runtime tools/padtest.c -o padtest $(sdl2-config --cflags --libs)
     SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./padtest
   Needs SDL 2.0.14 or newer. Not a test of real controllers or of rumble motors. */
#include "plat_sdl.c"
#include <assert.h>

void host_hotkey(int w) { (void)w; }
void host_quit(void) { }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int attach(const char *name) {
    (void)name;   /* SDL 2.0.20 has only the simple form: no name, no rumble callback */
    int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    CHECK(idx >= 0, "attach %s: %s", name, SDL_GetError());
    return idx;
}
static int index_of(SDL_JoystickID id) {   /* device indexes shift as pads come and go: look the pad up by instance */
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_JoystickGetDeviceInstanceID(i) == id) return i;
    return -1;
}
static void pump(void) { SDL_PumpEvents(); plat_pump(); }

int main(void) {
    plat_init();
    int i0 = attach("vpad0");
    pump();
    HostPad p;
    CHECK(plat_pad(0, &p) == 1, "pad 0 not found after hotplug event");
    CHECK(plat_pad(1, &p) == 0, "pad 1 should be absent");
    SDL_Joystick *j0 = SDL_JoystickOpen(i0);
    CHECK(j0 != NULL, "open virtual joystick: %s", SDL_GetError());

    /* every button, one at a time: SDL controller button k is joystick button k on a virtual pad */
    static const struct { int sdl; int bit; const char *n; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, HP_A, "A" }, { SDL_CONTROLLER_BUTTON_B, HP_B, "B" }, { SDL_CONTROLLER_BUTTON_X, HP_X, "X" },
        { SDL_CONTROLLER_BUTTON_Y, HP_Y, "Y" }, { SDL_CONTROLLER_BUTTON_BACK, HP_BACK, "Back" }, { SDL_CONTROLLER_BUTTON_START, HP_START, "Start" },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, HP_LS, "LS" }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, HP_RS, "RS" },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, HP_LB, "LB" }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, HP_RB, "RB" },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, HP_UP, "Up" }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, HP_DOWN, "Down" },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, HP_LEFT, "Left" }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, HP_RIGHT, "Right" } };
    for (size_t k = 0; k < sizeof map / sizeof *map; k++) {
        SDL_JoystickSetVirtualButton(j0, map[k].sdl, 1); pump();
        plat_pad(0, &p);
        CHECK(p.buttons == map[k].bit, "button %s: got %04x want %04x", map[k].n, p.buttons, map[k].bit);
        SDL_JoystickSetVirtualButton(j0, map[k].sdl, 0); pump();
        plat_pad(0, &p);
        CHECK(p.buttons == 0, "button %s did not release: %04x", map[k].n, p.buttons);
    }
    printf("buttons checked: %zu\n", sizeof map / sizeof *map);

    /* triggers: 0..255, the host presses at > 100 */
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0); pump(); plat_pad(0, &p);   /* a virtual axis is -32768..32767; a trigger is 0..32767 */
    CHECK(p.lt == 255 && p.rt >= 100 && p.rt <= 128, "triggers: lt %u rt %u", p.lt, p.rt);
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768); pump(); plat_pad(0, &p);
    CHECK(p.lt == 0 && p.rt == 0, "triggers at rest: lt %u rt %u", p.lt, p.rt);

    /* sticks: the host expects +x right and +y UP; SDL reports +y down */
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_LEFTX, 20000);
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_LEFTY, -30000);   /* pushed up */
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_RIGHTX, -10000);
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_RIGHTY, 25000); pump(); plat_pad(0, &p);   /* pushed down */
    CHECK(p.lx == 20000 && p.ly == 30000 && p.rx == -10000 && p.ry == -25000, "sticks: lx %d ly %d rx %d ry %d", p.lx, p.ly, p.rx, p.ry);
    SDL_JoystickSetVirtualAxis(j0, SDL_CONTROLLER_AXIS_LEFTY, -32768); pump(); plat_pad(0, &p);
    CHECK(p.ly == 32767, "full up must clamp to 32767, got %d", p.ly);
    for (int a = 0; a < 4; a++) SDL_JoystickSetVirtualAxis(j0, a, 0);   /* sticks to centre */
    pump();

    /* rumble: a virtual pad has no motors, so SDL refuses; the call must simply not crash, with or without a pad */
    plat_pad_rumble(0, 0x8000, 0x4000); plat_pad_rumble(0, 0, 0); plat_pad_rumble(1, 100, 100); plat_pad_rumble(-1, 1, 1);

    /* hotplug: a second pad takes slot 1, unplugging pad 0 frees slot 0, the next pad takes it */
    SDL_JoystickClose(j0);   /* the test's own handle, so only the game's reference keeps the pad open */
    SDL_JoystickID id0 = SDL_JoystickGetDeviceInstanceID(i0);
    int i1 = attach("vpad1"); pump();
    SDL_JoystickID id1 = SDL_JoystickGetDeviceInstanceID(i1);
    printf("instances: pad0 %d pad1 %d; slots hold %d and %d\n", (int)id0, (int)id1,
           pads[0] ? (int)SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[0])) : -1,
           pads[1] ? (int)SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[1])) : -1);
    CHECK(plat_pad(1, &p) == 1, "second pad not in slot 1");
    SDL_JoystickDetachVirtual(index_of(id0)); pump();
    CHECK(plat_pad(0, &p) == 0 && plat_pad(1, &p) == 1, "after unplug of pad 0: slot0 %d slot1 %d", plat_pad(0, &p), plat_pad(1, &p));
    int i2 = attach("vpad2"); pump(); SDL_JoystickID id2 = SDL_JoystickGetDeviceInstanceID(i2);
    CHECK(plat_pad(0, &p) == 1, "new pad did not take the free slot 0");
    int i3 = attach("vpad3"); pump(); SDL_JoystickID id3 = SDL_JoystickGetDeviceInstanceID(i3);   /* a third pad: ignored, no crash */
    CHECK(plat_pad(2, &p) == 0, "slot 2 must not exist");
    SDL_JoystickID ids[3] = { id1, id2, id3 };
    for (int k = 0; k < 3; k++) { SDL_JoystickDetachVirtual(index_of(ids[k])); pump(); }
    CHECK(plat_pad(0, &p) == 0 && plat_pad(1, &p) == 0, "all unplugged but pads remain");

    printf(fails ? "%d FAILED\n" : "all pad checks passed\n", fails);
    SDL_Quit();
    return fails != 0;
}
