#ifndef TASIM_MAP_H
#define TASIM_MAP_H
/* tasim_map.h -- the engine's feature grid as the shared simulation core holds it: the 13-byte cell, LoadMap's
   order of writes into it, the features a spawn and a destroy place and lift, and the ground height.
   research/notes/engine/map.md (*The cell*, *LoadMap's feature placement*) holds the engine facts each function
   cites; research/notes/impai/shared-core.md names the core's modules.

   ENGINE-FREE AND FILE-FREE. The core reads no file: the install loader reads the map's TNT and OTA and the feature
   TDFs (tagpu_vfs, tagpu_tdf) and hands them over as values -- the TNT's attribute records, the schema's numbers,
   the feature definitions filled through tasim_keys.c's one rule per key, the schema's features with their
   definitions already resolved (TS_MAPLOAD).

   ONE CALLER-OWNED BLOCK, NO ALLOCATION. ts_map_bytes sizes a map, ts_map_init lays it out in the caller's memory;
   nothing grows afterwards. The arrays are one per cell field (the cell's bytes split by field), indexed z * w + x
   as the engine's row-major cells are; ts_map_cell packs a cell back into the engine's 13 bytes. The T1 plan's
   arena (b1) takes these arrays over as its GRID part.

   EVERY INDEX TAKEN FROM DATA IS BOUNDED. A feature definition index against the definitions the map was given, a
   footprint's back-reference against the grid, a wreck record against the pool: where the engine has no bound, the
   function names the engine behaviour the bound replaces. */
#include <stddef.h>
#include <stdint.h>
#include "tasim_keys.h"

struct TS_CLASSMAPS;            /* tasim_class.h: the pathing maps a feature change re-marks */

/* the feature word +0x08's markers (map.md, The cell); every feature function treats a word from TS_F_MARK up as no
   feature (0x4240ED, 0x4244D3, 0x4246EC) */
#define TS_F_NONE    0xFFFFu    /* empty                                                                      */
#define TS_F_FOOT    0xFFFEu    /* a footprint cell; +0x0A/+0x0B lead back to its anchor                      */
#define TS_F_BORDER  0xFFFDu    /* masked by the border mask 0x4833B0                                         */
#define TS_F_VOID    0xFFFCu    /* a v2 TNT's void marker                                                     */
#define TS_F_MARK    0xFFFBu    /* the first word that is no feature                                          */

/* THE BOUNDS a map is refused outside. 2^22 cells is the T1 plan's arena bound (a 64 x 64 map's 2048 x 2048 cells).
   The two floors are where the engine's own loops stay on its grid: the border mask's pass 1 masks columns W-2 and
   W-1 (a map narrower than 2 reads before its first cell) and its pass 2 walks down a column while row * 16 -
   height / 2 is negative, which stops by row 8 at the latest (16 * 8 - 255 / 2 > 0), so a map of fewer than 9 rows
   runs it off the grid; pass 3 stops above row 1 on the same arithmetic. DISASSEMBLED 0x4833F8..0x483589. */
#define TS_MAP_CELLS_MAX (1u << 22)
#define TS_MAP_W_MIN     2
#define TS_MAP_H_MIN     9

/* the cell's owner bits, 10 for none, as LoadMap's height loops (0x483A24, 0x483ABE: and 0xD7, or 0x50) and every
   map spawn (owner 0xA, 0x423EF4..0x423F07) write them */
#define TS_OWNER_NONE    10

typedef struct TS_MAP {
    int32_t   w, h;             /* main+0x14233, +0x14237: the TNT header's +4, +8 (0x48387D..0x483892)          */
    int32_t   cells;            /* w * h                                                                          */
    uint8_t   sea;              /* main+0x1427F, the TNT header +0x24's low byte (0x483873)                      */
    int32_t   ext_w, ext_h;     /* the scroll extent main+0x1422B, +0x1422F: (w << 4) - 0x20, (h << 4) - 0x80,
                                   written by the border mask (0x4833B8..0x4833E0)                               */
    /* the cell, one array a field: +0x00 slot A, +0x02 slot B (unit slots), +0x04 the height, +0x05/+0x06 the 2x2
       maximum and minimum, +0x07 the metal, +0x08 the feature word, +0x0A/+0x0B the back-reference (dz in the low
       byte, dx in the high: the engine's little-endian word +0x0A; an anchor with a record holds its record here),
       +0x0C the flags (bit 0 the anchor holds a record, bit 1 the yard mark, bit 2 the draw's, bits 3-6 the owner) */
    uint16_t* slot_a;
    uint16_t* slot_b;
    uint8_t*  height;
    uint8_t*  hmax;
    uint8_t*  hmin;
    uint8_t*  metal;
    uint16_t* fdef;
    uint16_t* back;
    uint8_t*  flags;
    /* the feature definitions the words index, the engine's FeatureDef table main+0x1426F (count main+0x14253):
       the TNT's names in their order (0x421F20), then each schema feature not among them (0x423160) */
    const TASIM_FEATUREDEF* fd;
    int32_t   nfd;
    /* the wreck pool's free list, LIFO: 0x421F20 threads records 0..cap-1 with the head at 0, a 3DO spawn takes the
       head (0x423DA8..0x423DC5 through 0x4232F0), a destroy pushes the record back on it (0x424761..0x42476F) */
    int32_t   pool_cap;
    int32_t   pool_top;         /* records free: pool_free[pool_top - 1] is the head                             */
    uint16_t* pool_free;
    /* what a load refused that the engine would have read past its tables, counted (tasim_keys.h, the report) */
    int32_t   past_defs;        /* a feature word at or past nfd handed to a spawn                               */
} TS_MAP;

enum {
    TS_MAP_OK = 0,
    TS_MAP_SMALL,               /* below TS_MAP_W_MIN x TS_MAP_H_MIN                                              */
    TS_MAP_LARGE,               /* above TS_MAP_CELLS_MAX cells                                                  */
    TS_MAP_POOL,                /* a pool of more records than its 16-bit links hold (0x4232F0's movsx)          */
    TS_MAP_ROOM                 /* the block is smaller than ts_map_bytes asks                                   */
};

/* The bytes a w x h map with a wreck pool of pool_cap records needs; 0 when ts_map_init would refuse it. */
size_t ts_map_bytes(int32_t w, int32_t h, int32_t pool_cap);

/* Lays the map out in `mem` and writes what LoadMap's grid init 0x4839D5..0x4839ED writes into every cell -- slots 0,
   the feature word TS_F_NONE, flags bits 0-1 clear, the metal the load's background (set by ts_map_load) -- and the
   fields the engine leaves to its allocator as 0: the back-reference, flags bits 2 and 7, and the 2x2 extremes of
   the last column and the last row, which no engine code writes (ts_map_extremes). `fd`/`nfd` are the feature
   definitions; the pool starts as 0x421F20 leaves it. Returns TS_MAP_OK or the refusal. */
int ts_map_init(TS_MAP* m, void* mem, size_t bytes, int32_t w, int32_t h, uint8_t sea, int32_t pool_cap,
                const TASIM_FEATUREDEF* fd, int32_t nfd);

/* a schema feature as 0x436C30 stores it (DISASSEMBLED 0x437204..0x43725E): `Featurename`, `XPos`, `ZPos` of each
   subsection of the chosen schema's [features], in file order, XPos and ZPos read as ints (default -1) in 16-px
   cells. The loader resolves the name to `def` as 0x423160 does: the first definition whose name equals it without
   case (0x4231A3..0x4231CF, 0x4F8A70), else the one LoadFeature 0x4224B0 appends for it. A record with a negative
   XPos or ZPos has its name emptied (0x43725E) and places nothing: the loader passes it with def -1. */
typedef struct TS_MAPFEAT {
    int32_t def;
    int32_t x, z;
} TS_MAPFEAT;

/* What LoadMap reads, handed over by the loader. */
typedef struct TS_MAPLOAD {
    int32_t           version;      /* the TNT header +0: 0x2000 (v2) or 0x1020 (v1)                             */
    const uint8_t*    attr;         /* the attribute records, w * h of them: v2 4 bytes (height, the feature word
                                       at +1), v1 8 bytes (height, the feature byte at +2, the metal at +6)       */
    int32_t           surface_metal;/* the schema's SurfaceMetal, game state +0xD30 (0x4366B3, default 0)        */
    int32_t           lava;         /* GlobalHeader `lavaworld`, game state +0xD44 (0x436586, default 0)        */
    const TS_MAPFEAT* schema;       /* the schema's features, v2 only (0x423160 runs on that path alone)         */
    int32_t           nschema;
} TS_MAPLOAD;

/* LoadMap 0x483610's grid on a new game, in its order (DISASSEMBLED 0x4839A8..0x483D7D): the metal background
   (0x4839B3..0x4839ED); per cell the height and the owner bits, a v2 cell's void marker spawned where the word is
   TS_F_VOID, a v1 cell's metal byte (0x483A14..0x483A31, 0x483AB9..0x483AF2); then every TNT feature below the
   threshold (0xFFFB for v2, 0xFC for v1) spawned in row order (0x483A7E, 0x483B38), and on v2 the schema's
   (0x423160 at 0x483B4E); the 2x2 extremes of the whole map (0x483210 at 0x483CE7); the border mask (0x4833B0 at
   0x483CF1); the metal features' footprints (0x422040 at 0x483D7D). A saved game's branch is not modelled: the core
   loads no saved game. The pathing maps do not exist yet (0x440940 fills them after LoadMap), so no spawn re-marks.
   Returns 0, or -1 for a version LoadMap does not know (it stops with an error there, 0x483652..0x48366A) or no
   records. */
int ts_map_load(TS_MAP* m, const TS_MAPLOAD* in);

/* SpawnFeatureOnMap 0x423C50(cell, def, 0, 0, owner)'s grid writes, DISASSEMBLED 0x423C50..0x424041: def TS_F_NONE
   places nothing; TS_F_VOID writes the word alone; a footprint past the far edge (column + footx above w, row +
   footz above h; equal is accepted) is refused; every footprint cell that is not empty goes to ts_map_destroy(cell,
   0) row by row, and a refusal abandons the spawn with the cells before it already cleared; a 3DO definition (+0xFE
   bit 0 clear) takes the pool's head, none left abandons it; the anchor takes the word, the record (3DO) or 0 (GAF)
   in +0x0A and flags bit 0 to match, the owner in bits 3-6; every other footprint cell TS_F_FOOT with its dz, dx and
   flags bit 0 clear. Then 0x440A40 re-marks the footprint in every present class (`cm`, NULL while none exists).
   Returns 1 placed, 0 not. A word at or past nfd is counted in past_defs and placed nowhere: THE BOUND. The engine
   indexes its FeatureDef table with it unchecked (0x423CCF..0x423CD6). */
int ts_map_spawn(TS_MAP* m, struct TS_CLASSMAPS* cm, int32_t idx, uint32_t def, int32_t owner);

/* FEATURES_Destroy 0x4246B0(cell, force)'s grid writes, DISASSEMBLED 0x4246B0..0x424830: a TS_F_FOOT cell resolves
   to its anchor through +0x0A rows and +0x0B columns back; an anchor word at TS_F_MARK or above refuses; without
   force an indestructible definition (+0xFF bit 1, flags bit 9) refuses; a record (flags bit 0) goes back on the
   pool; the anchor and every TS_F_FOOT cell of the definition's footprint from it become TS_F_NONE with flags bit 0
   clear (whoever's footprint the cell was); +0x0A and the owner bits are left. Then 0x440A40 over the footprint.
   Returns 1 destroyed, 0 refused. THE BOUNDS: an anchor the back-reference puts before the grid, or a definition at
   or past nfd, refuses (the engine reads before its grid and past its table there); a footprint cell past the grid
   is skipped; a record at or past the pool is not pushed. */
int ts_map_destroy(TS_MAP* m, struct TS_CLASSMAPS* cm, int32_t idx, int force);

/* 0x483210(x | z << 16, w | h << 16), DISASSEMBLED 0x483210..0x483368, each argument a word as the engine packs
   them: for z in [max(z, 0), min(z + h, H - 1)) and x in [max(x, 0), min(x + w, W - 1)), the far ends from the
   unclamped x and z, +0x05 and +0x06 are the maximum and minimum of the cell's height and its
   right, lower and lower-right neighbours' (each taken only where it is on the grid). THE ENGINE'S, KEPT: both ends
   clamp to W - 1 and H - 1, so no call ever writes the last column or the last row. LoadMap's whole-map call
   (0x483CE7) leaves them as the grid's allocation 0x4D83B0 handed them over [INFERRED: the CRT's malloc, not
   cleared unless the -memset option is given]; the core holds 0 there (ts_map_init). The fill of the pathing maps
   reads them (0x47DE60 on every cell) and the lava mask reads +0x06; nothing else does. */
void ts_map_extremes(TS_MAP* m, int32_t x, int32_t z, int32_t w, int32_t h);

/* The border mask 0x4833B0, DISASSEMBLED 0x4833B0..0x483604: the scroll extent first, then four passes, each store
   turning an empty or TS_F_FOOT cell into TS_F_BORDER and leaving every other word (an anchor survives):
   1. columns W-2 and W-1 of every row;
   2. down each column from row 0 while row * 16 - height / 2 is negative;
   3. up each column from row H-1 while row * 16 - height / 2 is above ext_h, masking THE ROW ABOVE the one tested
      (0x483559..0x483565, `sub eax, W*13`): the engine's, kept -- row H-1 itself is never masked by this pass;
   4. with `lava` set, every cell whose 2x2 minimum is at or below the sea level. */
void ts_map_mask(TS_MAP* m, int lava);

/* 0x422040, DISASSEMBLED 0x422040..0x422169: for every anchor in cell order whose definition has a non-zero metal
   float (+0xF0) and is indestructible (+0xFF bit 1), the metal byte of every footprint cell on the grid is the low
   byte of _ftol of that float. A later anchor overwrites an earlier one where footprints meet. */
void ts_map_metal(TS_MAP* m);

/* The ground height 0x485070(pos*), DISASSEMBLED 0x485070..0x48513D: x and z are the 16.16 position's integer words
   (movsx of the high words), the cell (x >> 4, z >> 4) arithmetic, the fraction their low four bits; -1 when the
   cell or its right or lower neighbour is off the grid (cx < 0, cx + 1 >= W, cz < 0, cz + 1 >= H); else the bilinear
   of the four heights, each division by 16 truncating toward zero (cdq; and edx, 15; add; sar 4): top = h00 +
   (h10 - h00) * fx / 16, bot = h01 + (h11 - h01) * fx / 16, top + (bot - top) * fz / 16. */
int32_t ts_map_ground(const TS_MAP* m, int32_t x16, int32_t z16);

/* The cell `idx` as the engine's 13 bytes, little-endian, for a comparison with the engine's grid. */
void ts_map_cell(const TS_MAP* m, int32_t idx, uint8_t out[13]);

#endif
