#ifndef TAGPU_FOGSRC_H
#define TAGPU_FOGSRC_H
/* tagpu_fogsrc -- the fog of war as its SOURCE: two bits for each 32-px LOS cell of the whole map,
   and the one expansion every reader turns them into the engine's corner masks with.

   THE FORMAT. Cell (cx, cy) of a `w` x `h` map (the local player's LOS map, `losW` x `losH`,
   which tagpu_fogwide.c's `fogw_source` requires to be `plotC/2` x `plotR/2`) holds
     bit 0  never explored: `(MAPPED[(plotC*cy)/2 + cx] & (1 << player)) == 0`, the index bounded
            by MAPPED's allocation as the engine's builder bounds it;
     bit 1  out of LOS: `LosType & 2` (true LOS) and the LOS counter is 0.
   Word `(cy, cx >> 4)` holds cell cx at bits `2 * (cx & 15)`, `stride = (w + 15) / 16` words a
   row. The Vulkan passes read it as an R32_UINT image `stride` x `h` (tagpu_vk_fog.c); the GLSL
   edition of the expansion is TAGPU_GLSL_FOG_FN (tagpu_glsl.h). True LOS is folded into bit 1
   when the bits are packed, so no reader needs the mode.

   THE EXPANSION, the engine's builder `0x4843C0` (terrain-depth.md §5.2) over the whole map at
   once. Entry (cx, cy) covers cells (cx, cy) bit 1, (cx + 1, cy) bit 2, (cx, cy + 1) bit 4 and
   (cx + 1, cy + 1) bit 8 of each nibble -- the low nibble unexplored, the next out of LOS -- and
   its origin is world (32 cx + 16, 32 cy + 16):
     1. the entry is clamped to cx in [-1, w - 1], cy in [-1, h - 1]: past the entries that
        straddle the map's edge every entry repeats the straddling one;
     2. the four-corner OR, a cell off the map contributing nothing (the engine's `jae` skip);
     3. the four border completions `0x4846A1..0x4848CD`, in the engine's order, on both nibbles:
        top (cy == -1) 1 |= 4, 2 |= 8; bottom (cy == h - 1) 4 |= 1, 8 |= 2; left (cx == -1)
        4 |= 8, 1 |= 2; right (cx == w - 1) 8 |= 4, 2 |= 1. Two of them meet only at a corner
        entry, which holds one map cell, so there they commute (tools/replay/fog_expand_test.c
        checks both orders against the window build).
   A reader then interpolates the entry's corners as `taFog` does (tagpu_glsl.h, step 4).

   THE C MIRROR OF TAGPU_GLSL_FOG_FN IS `tagpu_fog_entry` BELOW: edit both or neither. The host
   test tools/replay/fog_expand_test.py holds it to `tagpu_fog_build_window`, the replication of
   the builder over a window that the oracle (tagpu_fogwide.c `fogw_check`) holds to the engine's
   own grid.

   Engine-free and Win32-free, so the host test compiles it as the DLL does. */

#include <stddef.h>
#include <stdint.h>

/* cells a side: the terrain's own bound (it refuses a map wider than 4096 16-px cells), so every
   map the Vulkan lane draws has its fog. The packet reserves this square in every slot
   (tagpu_packet.c, PK_DESIGN_FRONT). */
#define TAGPU_FOG_MAXDIM 2048

#define TAGPU_FOG_UNEXPLORED 1u          /* a cell's bit 0                                      */
#define TAGPU_FOG_OUTOFLOS   2u          /* a cell's bit 1                                      */

typedef struct {
    const uint32_t* words;               /* stride * h words                                    */
    int             w, h, stride;        /* cells, cells, words a row                           */
    unsigned        serial;              /* moves whenever the bits do                          */
} TAGPU_FOGSRC;

/* words a row for a map `w` cells wide */
static __inline int tagpu_fog_stride(int w) { return (w + 15) / 16; }

/* THE BOUND every reader checks before it forms an index: a non-NULL source whose dimensions are
   inside TAGPU_FOG_MAXDIM and whose stride is the one its width implies. With it, every word
   `tagpu_fog_cell` reads is one of `stride * h`, the length the source's owner sized. */
static __inline int tagpu_fog_src_ok(const TAGPU_FOGSRC* f)
{
    return f && f->words && f->w >= 1 && f->h >= 1 &&
           f->w <= TAGPU_FOG_MAXDIM && f->h <= TAGPU_FOG_MAXDIM &&
           f->stride == tagpu_fog_stride(f->w);
}

/* The cell's two bits; 0 off the map. The unsigned compares are the bound: no word outside
   `[0, stride) x [0, h)` is read. */
static __inline unsigned tagpu_fog_cell(const TAGPU_FOGSRC* f, int cx, int cy)
{
    if ((unsigned)cx >= (unsigned)f->w || (unsigned)cy >= (unsigned)f->h) return 0;
    return (f->words[(size_t)cy * (size_t)f->stride + (size_t)(cx >> 4)] >> (2 * (cx & 15))) & 3u;
}

/* Entry (cx, cy) of the whole-map expansion: low nibble unexplored, next nibble out of LOS. */
static __inline unsigned tagpu_fog_entry(const TAGPU_FOGSRC* f, int cx, int cy)
{
    unsigned a, b, c, d, e;
    if (cx < -1) cx = -1;
    if (cx > f->w - 1) cx = f->w - 1;
    if (cy < -1) cy = -1;
    if (cy > f->h - 1) cy = f->h - 1;
    a = tagpu_fog_cell(f, cx, cy);
    b = tagpu_fog_cell(f, cx + 1, cy);
    c = tagpu_fog_cell(f, cx, cy + 1);
    d = tagpu_fog_cell(f, cx + 1, cy + 1);
    e = (a & 1u) | (b & 1u) << 1 | (c & 1u) << 2 | (d & 1u) << 3 |
        (a >> 1) << 4 | (b >> 1) << 5 | (c >> 1) << 6 | (d >> 1) << 7;
    if (cy == -1)         e |= (e >> 2) & 0x33u;     /* top:    1 |= 4, 2 |= 8 */
    if (cy == f->h - 1)   e |= (e << 2) & 0xCCu;     /* bottom: 4 |= 1, 8 |= 2 */
    if (cx == -1)         e |= (e >> 1) & 0x55u;     /* left:   1 |= 2, 4 |= 8 */
    if (cx == f->w - 1)   e |= (e << 1) & 0xAAu;     /* right:  2 |= 1, 8 |= 4 */
    return e;
}

/* `0x4843C0` over an arbitrary window, from the source bits: grid entry (0, 0) covers map cell
   (col0, row0) and the three right of and below it, as the engine's does for its own eye-derived
   col0/row0, and `out` (cols * rows u16, low byte unexplored, high byte out of LOS -- the engine
   grid's layout) is built by the engine's loops: every cell of the window ORs its corners into
   the entries it touches, so the window's last column and row are short their right and bottom
   corners, and each border completion is applied to the entry that straddles that edge, on the
   engine's gates: the top and left ones when the window holds entry -1, the bottom and right ones
   when it holds entry h (or w), one past the straddling entry. The oracle (tagpu_fogwide.c)
   compares this with the engine's grid; the host test compares `tagpu_fog_entry` with this. */
void tagpu_fog_build_window(const TAGPU_FOGSRC* f, uint16_t* out,
                            int cols, int rows, int col0, int row0);

#endif
