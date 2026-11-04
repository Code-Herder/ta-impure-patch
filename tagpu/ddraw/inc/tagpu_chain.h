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

/* THE FEATURE CHAINS (tagpu_vk_feat.c). A GAF frame of w x h has no implied place for a chain --
   the feature atlas is a shelf atlas of frames of any size at any origin -- so it is cut, from its
   own first texel, into ceil(w/32) x ceil(h/32) tiles, each clipped to the frame, and each tile
   takes one cell of a chain image FCOLS x FROWS cells, given out in the atlas's entry order. A
   level-L block measured from the frame's origin lies inside one such tile, because 2^L divides
   32, so the tiles' chains are the frame's own and never touch another frame's texels. The
   fragment stage's taFChain hard-codes the FCOLS of 64 as `& 63` and `>> 6`. */
#define TAGPU_CHAIN_FCOLS  64
#define TAGPU_CHAIN_FROWS  128
#define TAGPU_CHAIN_FCELLS (TAGPU_CHAIN_FCOLS * TAGPU_CHAIN_FROWS)

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

/* THE RESTORED TERRAIN CHAIN'S LEDGER (tagpu_vk_terr.c `chain_maintain`): what one frame does to
   the chain so that it MIRRORS THE RESTORED ATLAS'S LEVEL 0 whenever that atlas is a picture.
   In: the chain's state -- `have` (anything recorded into it), `gen` (the job it follows), `done`
   (the painted count it covers), `whole` (its next paint must be reduced whole) -- which this
   updates as though the action were recorded, and the frame's facts: `job` (the restore job's
   generation), `picture` (the atlas is a picture), `painted` (the job's count), `fifo` (painted
   = P names places [0, P) of a queue, so a list may follow it).
   THE RULE. The restorer clears its destination at a job's FIRST OUT, not when the job is made.
     * A new generation with something painted: the clear has happened, so the chain is reduced
       WHOLE.
     * A new generation with nothing painted on a standing picture -- a job made again on an
       unchanged request -- leaves level 0 as the last job left it: the chain is KEPT (or reduced
       whole if it holds nothing yet), and that generation's first paint is reduced whole,
       because that OUT clears level 0 first.
     * A new generation with no picture: CLEAR (nothing samples it, and the first paint will be a
       whole reduction).
     * The same generation, painted moved: a LIST of the places painted since, under a FIFO queue
       once the generation's first paint has been reduced whole; otherwise WHOLE.
   Returns TAGPU_RC_*. */
enum { TAGPU_RC_NONE = 0, TAGPU_RC_CLEAR, TAGPU_RC_KEEP, TAGPU_RC_WHOLE, TAGPU_RC_LIST };
int tagpu_chain_rest_step(int* have, unsigned* gen, int* done, int* whole, unsigned job,
                          int picture, int painted, int fifo);

/* ...and its FRAME FORM, the texel the feature fragment stage fetches (tagpu_feat.c `taFChain`):
   level L (1..5), the frame's table record rec = { x | y << 16, w | h << 16, its first cell, its
   tiles across } (w, h >= 1), a sample at atlas UV (u, v) of a `dim`-square atlas; (*x, *y) at
   image level L-1 of a chain TAGPU_CHAIN_FCOLS cells wide. */
void tagpu_chain_ftexel(int L, const unsigned rec[4], float u, float v, int dim, int* x, int* y);

#endif
