/* tagpu_chain_ref.c -- the C side of the mip chains (inc/tagpu_chain.h): the level rule, the
   reference reduction and the fragment stage's lookup. Engine-free, Win32-free and stateless, so
   the DLL, both scenes' passes and tools/replay/chain_test.c all compile this one file. */
#include "tagpu_chain.h"
#include <math.h>
#include <stddef.h>

/* THE LEVEL RULE, AND WHY 1x AND ABOVE DRAW EXACTLY WHAT THEY DREW WITHOUT IT.

   1. A tile's quad spans 32 texels and 32 * uZoom game px (`aPos * uZoom` in both vertex
      stages), and the viewport maps uGame's `gw` game px onto the `w` target px the pass records
      into. So one texel spans zoom * w / gw target px across and zoom * h / gh down; f is the
      smaller of the two.
   2. The loop's first test is f * 2^0 < 1, so L = 0 exactly when f >= 1: level 0 -- the atlases,
      sampled as ever -- exactly when a texel covers at least one target pixel, and so is reached
      by at least one sample of the supersampled target and seen by the resolve. Otherwise L is the
      finest level whose texel, 2^L of level 0's, covers at least one target px, capped at 5.
   3. With the world target standing, w = gw * ss and h = gh * ss (tagpu_vk_world.c `prepare`),
      so f = zoom * ss: a float times an integer <= 8, exact in double. At zoom >= 1, f >= ss >= 1
      and L = 0; at zoom 0.5 with ss = 2, f is exactly 1.0 and L = 0.
   4. The EXTENT, not the hand-over's `ss`: `ss` says what the gather was told, not whether the
      target stood. Without one the world is drawn at client resolution, w / gw is the real
      footprint, and a client smaller than the game frame is minified even at 1x -- where L >= 1
      is right. That is the one configuration in which a 1x frame changes, and it was aliasing.
   5. `zoom` is the float the pass writes into uZoom and (w, h) are its `record`'s arguments, so the
      level and the geometry come from the same numbers.

   ROUNDING UP (the finest level that skips no texel) rather than to the nearest: nearest would keep
   level 0 down to 0.354x at ss = 2, where a sample grid skips up to 29 % of the texels. */
int tagpu_chain_level(float zoom, float gw, float gh, unsigned w, unsigned h)
{
    double sx, sy, f;
    int L = 0;
    if (!(gw > 0.0f) || !(gh > 0.0f)) return 0;
    sx = (double)w / gw;
    sy = (double)h / gh;
    f = (double)zoom * (sx < sy ? sx : sy);
    if (!(f > 0.0)) return 0;                       /* garbage in: level 0, the picture without chains */
    while (L < TAGPU_CHAIN_LEVELS && f * (double)(1 << L) < 1.0) L++;
    return L;
}

/* The C mirror of REDUCE_CS (src/tagpu_chain_comp.h): EDIT BOTH OR NEITHER. Every level is
   reduced from level 0 directly, which is the same number the shader's sums of sums give: a sum of
   disjoint blocks' sums is the sum over their union. */
void tagpu_chain_ref(const unsigned char* rgba, int rowBytes, int sx, int sy, int tw, int th,
                     unsigned char* out)
{
    int L, bx, by, x, y;
    unsigned char* o = out;
    for (L = 1; L <= TAGPU_CHAIN_LEVELS; L++) {
        int n = 32 >> L, s = 1 << L;
        for (by = 0; by < n; by++)
            for (bx = 0; bx < n; bx++, o += 4) {
                unsigned r = 0, g = 0, b = 0, cO = 0, cB = 0;
                for (y = by * s; y < by * s + s; y++)
                    for (x = bx * s; x < bx * s + s; x++) {
                        const unsigned char* p;
                        if (x >= tw || y >= th) continue;
                        p = rgba + (size_t)(sy + y) * rowBytes + (size_t)(sx + x) * 4;
                        cB++;
                        if (p[3] < 128) continue;
                        cO++; r += p[0]; g += p[1]; b += p[2];
                    }
                o[0] = (unsigned char)(cO ? (r + cO / 2) / cO : 0);
                o[1] = (unsigned char)(cO ? (g + cO / 2) / cO : 0);
                o[2] = (unsigned char)(cO ? (b + cO / 2) / cO : 0);
                o[3] = (unsigned char)(cB ? (cO * 255u + cB / 2) / cB : 0);
            }
    }
}

/* The C mirror of tagpu_terr.c's `taChain`: EDIT BOTH OR NEITHER. The division by 2^L is exact in
   float, as the shader's is. */
void tagpu_chain_texel(int L, int cx, int cy, float px, float py, int* x, int* y)
{
    int n = 32 >> L;
    int qx = (int)floorf(px / (float)(1 << L)), qy = (int)floorf(py / (float)(1 << L));
    if (qx < 0) qx = 0;
    if (qx > n - 1) qx = n - 1;
    if (qy < 0) qy = 0;
    if (qy > n - 1) qy = n - 1;
    *x = cx * n + qx;
    *y = cy * n + qy;
}
