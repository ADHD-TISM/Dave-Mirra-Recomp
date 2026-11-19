/* Checks runtime/vram_cuts.h (how the GL renderer cuts a vram copy that wraps): for every source, destination and length on a
   small power-of-two axis, the pieces must be non-empty, wrap at neither end and tile the length exactly once; the real vram sizes
   (1024 and 512) are checked on every edge offset. From the repository root:
     gcc -I runtime tools/cutstest.c -o build/cutstest && build/cutstest */
#include <stdio.h>
#include "vram_cuts.h"

static long fails, runs;

static void check(int s, int d, int n, int size) {
    int c[3], k = cuts(s, d, n, size, c), covered = 0;
    runs++;
    if (k < 1 || k > 3 || c[0] != 0) { fails++; return; }
    for (int i = 0; i < k; i++) {
        int a = c[i], b = i + 1 < k ? c[i + 1] : n;
        if (b <= a || (i && a <= c[i - 1])) { fails++; return; }   /* empty, or cuts not increasing */
        if (((s + a) & (size - 1)) + (b - a) > size || ((d + a) & (size - 1)) + (b - a) > size) { fails++; return; }   /* wraps inside a piece */
        covered += b - a;
    }
    if (covered != n) fails++;
}

int main(void) {
    for (int s = 0; s < 64; s++) for (int d = 0; d < 64; d++) for (int n = 1; n <= 64; n++) check(s, d, n, 64);
    for (int s = 1000; s < 1024; s++) for (int d = 0; d < 1024; d += 7) for (int n = 1; n <= 1024; n += 13) check(s, d, n, 1024);
    for (int s = 500; s < 512; s++) for (int d = 0; d < 512; d += 5) for (int n = 1; n <= 512; n += 11) check(s, d, n, 512);
    for (int s = 0; s < 1024; s += 9) for (int d = 1000; d < 1024; d++) for (int n = 1; n <= 1024; n += 17) check(s, d, n, 1024);
    printf("%ld cases, %ld failures\n", runs, fails);
    return fails != 0;
}
