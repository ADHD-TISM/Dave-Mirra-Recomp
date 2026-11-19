/* Cutting a vram-to-vram copy where the console wraps it. Kept apart from gpu_gl.c so tools/cutstest.c can check it without OpenGL. */
#ifndef VRAM_CUTS_H
#define VRAM_CUTS_H

/* One axis of a copy: n pixels from s to d inside a wrapped space of `size` (a power of two, n <= size). Fills out[] with the
   offsets where either the source or the destination reaches the edge (always starting with 0) and returns how many; the pieces
   between consecutive cuts (the last one runs to n) wrap at neither end. */
static int cuts(int s, int d, int n, int size, int out[3]) {
    int k = 0, c0 = size - (s & (size - 1)), c1 = size - (d & (size - 1));
    out[k++] = 0;
    if (c0 < n) out[k++] = c0;
    if (c1 < n && c1 != c0) out[k++] = c1;
    if (k == 3 && out[1] > out[2]) { int t = out[1]; out[1] = out[2]; out[2] = t; }
    return k;
}

#endif
