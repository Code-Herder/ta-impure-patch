#ifndef TAGPU_CHAIN_H
#define TAGPU_CHAIN_H
/* tagpu_chain.h -- the mip chain of a 32x32 tile: its format, the level a frame draws at, and the
   C reference of the GPU's reduction. Engine-free and Win32-free, so tools/replay/chain_test.c
   compiles it natively.

   WHY A CHAIN PER TILE AND NOT PER ATLAS. The terrain atlases pack their tiles on a 34-texel pitch
   with the tile at offset 1 (tagpu_terr.c CELL_PITCH, CELL_BORDER), so a 2x2 block of a whole-
   atlas chain straddles the guard texel and the neighbour from level 1 on. A chain cell holds one
   tile's levels and nothing else: every 32x32 tile (or neighbourhood key) of a source atlas gets a
   cell of a separate 5-level RGBA8 image, in the source atlas's own row-major grid, 16 texels a
   side at image level 0. Image level k therefore holds tile level k+1, `16 >> k` texels a cell
   side, and cell (cx, cy) is at (cx, cy) * (16 >> k) there.

   THE ARITHMETIC, one definition for the reducer (src/tagpu_chain_comp.h REDUCE_CS) and for
   `tagpu_chain_ref`. B is the set of the tile's valid texels inside a level-L block (all 4^L of
   them for a terrain tile), O the subset whose alpha byte is >= 128:

       rgb = |O| > 0 ? (sum over O of rgb + |O|/2) / |O| : 0
       a   = (|O| * 255 + |B|/2) / |B|                         (0 when |B| is 0)

   in integers, from the LEVEL-0 BYTES at every level -- one rounding per level, none compounded.
   An opaque tile (alpha 255 everywhere: the base terrain atlas) gets its rounded box mean; a cell
   the restorer has not painted (alpha 0) stays alpha 0 at every level, so a chain cell is painted
   or not exactly as its level 0 is; a keyed sprite averages its colour over opaque texels only and
   carries its coverage as alpha. */

#define TAGPU_CHAIN_LEVELS 5        /* tile levels 1..5 of a 32x32 tile: 16, 8, 4, 2, 1 texels a side */
#define TAGPU_CHAIN_CELL   16       /* a cell's side at image level 0 (tile level 1)                  */
#define TAGPU_CHAIN_TEXELS 341      /* 256 + 64 + 16 + 4 + 1: one cell's chain, every level           */

/* The level a frame draws at, 0..TAGPU_CHAIN_LEVELS: the finest whose texel covers at least one
   target pixel. `zoom` is the pass's uZoom, (gw, gh) the game frame its vertex stage divides by
   (uGame), (w, h) the extent the pass records into. See the proof at the definition. */
int  tagpu_chain_level(float zoom, float gw, float gh, unsigned w, unsigned h);

/* One tile's chain from its level-0 texels: RGBA8 at (sx, sy) of `rgba` (`rowBytes` a row), the
   tile's valid size tw x th <= 32 x 32 measured from (sx, sy), into out[TAGPU_CHAIN_TEXELS * 4] --
   levels 1..5, each row-major, level after level. A texel outside the valid size counts in
   neither B nor O. */
void tagpu_chain_ref(const unsigned char* rgba, int rowBytes, int sx, int sy, int tw, int th,
                     unsigned char* out);

/* The texel the terrain's fragment stage fetches (tagpu_terr.c `taChain`): level L (1..5), a
   sample at in-tile coordinate (px, py), cell (cx, cy); (*x, *y) at image level L-1. */
void tagpu_chain_texel(int L, int cx, int cy, float px, float py, int* x, int* y);

#endif
