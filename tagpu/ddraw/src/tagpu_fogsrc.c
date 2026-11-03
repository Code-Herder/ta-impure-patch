/* tagpu_fogsrc.c -- the window build of the fog source (tagpu_fogsrc.h). No engine memory, no
   Win32: tools/replay/fog_expand_test.py compiles this file as the DLL does and holds
   `tagpu_fog_entry` to it, and tagpu_fogwide.c's oracle holds it to the engine's grid. */
#include "tagpu_fogsrc.h"
#include <string.h>

/* One cell sets a DIFFERENT corner bit in each of the four entries around it (terrain-depth.md
   §5.2). `hi` picks the byte: 0 the unexplored mask, 1 the out-of-LOS one. Every test is unsigned
   as the engine's `jae` pairs are, so a negative index is rejected rather than wrapped -- that is
   the bound on every write here. */
static void corner_or(unsigned char* g, int cols, int rows, int gx, int gy, int hi)
{
    if ((unsigned)gx < (unsigned)cols && (unsigned)gy < (unsigned)rows)
        g[((size_t)gy * cols + gx) * 2 + hi] |= 1;
    if ((unsigned)(gx - 1) < (unsigned)cols && (unsigned)gy < (unsigned)rows)
        g[((size_t)gy * cols + gx - 1) * 2 + hi] |= 2;
    if ((unsigned)gx < (unsigned)cols && (unsigned)(gy - 1) < (unsigned)rows)
        g[((size_t)(gy - 1) * cols + gx) * 2 + hi] |= 4;
    if ((unsigned)(gx - 1) < (unsigned)cols && (unsigned)(gy - 1) < (unsigned)rows)
        g[((size_t)(gy - 1) * cols + gx - 1) * 2 + hi] |= 8;
}

/* `a |= b` on both bytes of entry k: the completion's corner `a` takes corner `b`'s bit */
static void complete(unsigned char* g, size_t k, unsigned a, unsigned b)
{
    if (g[k] & b) g[k] |= (unsigned char)a;
    if (g[k + 1] & b) g[k + 1] |= (unsigned char)a;
}

void tagpu_fog_build_window(const TAGPU_FOGSRC* f, uint16_t* out,
                            int cols, int rows, int col0, int row0)
{
    unsigned char* g = (unsigned char*)out;
    int cx, cy, i;

    memset(out, 0, (size_t)cols * rows * 2);

    for (cy = row0; cy < row0 + rows; cy++) {
        if ((unsigned)cy >= (unsigned)f->h) continue;          /* engine: `jae` skip */
        for (cx = col0; cx < col0 + cols; cx++) {
            unsigned c = tagpu_fog_cell(f, cx, cy);
            if (c & TAGPU_FOG_OUTOFLOS)   corner_or(g, cols, rows, cx - col0, cy - row0, 1);
            if (c & TAGPU_FOG_UNEXPLORED) corner_or(g, cols, rows, cx - col0, cy - row0, 0);
        }
    }

    /* The four map-border completions (`0x4846A1`..`0x4848CD`), in the engine's own order.
       THE LINE EACH ONE FIXES IS DERIVED, not the engine's literal 0 / rows-2: the entry that
       straddles an edge, `-row0 - 1` for the top. The engine writes row 0, which is that row only
       while its window overshoots the map by at most one cell (`row0` 0 or -1, which an eye in the
       engine's own range `[0, extent - W]` guarantees); the oracle leaves both lines out wherever
       the two differ (tagpu_fogwide.c `fogw_check`). The bottom gives `h - 1 - row0`, which is the
       engine's `rows - 2` whenever its window overshoots by exactly the one cell. A completion on
       a byte that is all zero -- the out-of-LOS byte outside true LOS -- changes nothing, so both
       bytes are completed unconditionally. */
    {
        int gTop = -row0 - 1, gBot = f->h - 1 - row0;
        int gLef = -col0 - 1, gRig = f->w - 1 - col0;

        if (row0 < 0 && gTop >= 0 && gTop < rows)
            for (i = 0; i < cols; i++) {
                size_t k = ((size_t)gTop * cols + i) * 2;
                complete(g, k, 1, 4); complete(g, k, 2, 8);
            }
        if (row0 + rows > f->h && gBot >= 0 && gBot < rows)
            for (i = 0; i < cols; i++) {
                size_t k = ((size_t)gBot * cols + i) * 2;
                complete(g, k, 4, 1); complete(g, k, 8, 2);
            }
        if (col0 < 0 && gLef >= 0 && gLef < cols)
            for (i = 0; i < rows; i++) {
                size_t k = ((size_t)i * cols + gLef) * 2;
                complete(g, k, 4, 8); complete(g, k, 1, 2);
            }
        if (col0 + cols > f->w && gRig >= 0 && gRig < cols)
            for (i = 0; i < rows; i++) {
                size_t k = ((size_t)i * cols + gRig) * 2;
                complete(g, k, 8, 4); complete(g, k, 2, 1);
            }
    }
}
