/* Recomp Achievements (third_party/rl_achievements): RecompLauncher's achievements, as rules on the PlayStation's
   memory that its list holds, checked once a frame. The game itself is not changed: the library only reads memory.
   Started without RecompLauncher (no RL_SESSION in the environment, read once at start), every call returns at once.
   Addresses are the ones rcheevos uses for the PlayStation: 0x000000 to 0x1FFFFF the 2 MB of main RAM, 0x200000 to
   0x2003FF the scratchpad. */
#include <string.h>
#include "psx.h"
#include "rl_achievements.h"

static uint32_t read_mem(uint32_t addr, uint8_t *buf, uint32_t n, void *ud) {
    (void)ud;
    if (addr < 0x200000u && n <= 0x200000u - addr) { memcpy(buf, psx_ram + addr, n); return n; }
    if (addr >= 0x200000u && addr < 0x200400u && n <= 0x200400u - addr) { memcpy(buf, psx_scratch + (addr - 0x200000u), n); return n; }
    return 0;   /* an address the console doesn't have */
}

void ach_init(void) { rl_ach_init("psx", read_mem, NULL); }
void ach_frame(void) { rl_ach_frame(); }
void ach_save_loaded(void) { rl_ach_save_loaded(); }
void ach_shutdown(void) { rl_ach_shutdown(); }
