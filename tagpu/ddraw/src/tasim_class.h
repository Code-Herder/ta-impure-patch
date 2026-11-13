#ifndef TASIM_CLASS_H
#define TASIM_CLASS_H
/* tasim_class.h -- the engine's pathing maps as the shared simulation core holds them: one 2-bit code per cell for
   each movement class, filled at load, re-marked where a feature or a unit changes them. research/notes/engine/map.md
   (*The pathing maps*) holds the engine facts each function cites.

   THE MAP of a class is the engine's [0x512358 + 0x20 * i] + 0x18: ((H + 15) >> 4) * W dwords, a dword packing 16
   rows of one column, dword (z >> 4) * W + x, code (dword >> 2 * (z & 15)) & 3. Code 0 is blocked, 1 passable but
   bad, 3 good; 2 is never stored (the pathfinder's reader 0x40D7B0 answers it for an unexplored cell). Entry (x, z)
   is the class footprint with its top-left cell at (x, z) and the one-cell ring around it.

   THE UNITS. The rule reads the unit in a cell's slot A: whether it has a mover, and its mover's claim stamp against
   the class's watermark. Until the core's units sit in the engine's slots (the T1 plan's b1), the caller keeps that
   view of them in a TS_CUNITS, indexed by the engine's slot: the claim stamp is written here by the claim and the
   watermark, and read by the rule.

   ONE CALLER-OWNED BLOCK, NO ALLOCATION, as tasim_map.h; ENGINE-FREE AND FILE-FREE. */
#include <stddef.h>
#include <stdint.h>
#include "tasim_keys.h"
#include "tasim_map.h"

/* what the class maps read of one unit slot (the unit record +0x00 is its mover pointer) */
typedef struct TS_CUNIT {
    int16_t  cx, cz;            /* +0x76, +0x78: the footprint's top-left cell                                   */
    int16_t  sx, sz;            /* +0x7E, +0x80: the footprint's size                                            */
    uint32_t claim;             /* the mover's +0x26, the claim stamp: a GameTime                                */
    uint8_t  mover;             /* the record's +0x00 is not NULL                                                */
    uint8_t  alive;             /* +0x110 bit 28                                                                 */
} TS_CUNIT;

typedef struct TS_CUNITS {
    TS_CUNIT* u;                /* slots 0..n-1; slot 0 is never a unit (the engine's player blocks start at 1)  */
    int32_t   n;                /* the engine's walk to its last slot [main+0x1435B] covers 1..n-1               */
} TS_CUNITS;

/* one movement class, the engine's 0x20-byte record 0x512358 + 0x20 * i */
typedef struct TS_CLASSMAP {
    uint8_t   present;          /* +0x00, the name pointer, set for every CLASS<i> moveinfo.tdf has               */
    int16_t   fx, fz;           /* +0x04, +0x06 FootPrintX, FootPrintZ                                           */
    int16_t   maxwaterdepth;    /* +0x08                                                                         */
    int16_t   minwaterdepth;    /* +0x0A                                                                         */
    uint8_t   maxslope;         /* +0x0C                                                                         */
    uint8_t   badslope;         /* +0x0D                                                                         */
    uint8_t   maxwaterslope;    /* +0x0E                                                                         */
    uint8_t   badwaterslope;    /* +0x0F                                                                         */
    uint32_t  wm;               /* +0x1C, the watermark: only a search writes it (0x440B18)                       */
    uint32_t* map;              /* +0x18, NULL for an absent class                                                */
} TS_CLASSMAP;

typedef struct TS_CLASSMAPS {
    const TS_MAP* map;          /* the grid the rule reads                                                       */
    TS_CUNITS*    units;        /* the units it reads, or NULL while none exists                                  */
    int32_t       w, h;         /* +0x10, +0x14: the grid's W and H when the maps were made (0x44098D, 0x440990) */
    int32_t       rows16;       /* (h + 15) >> 4                                                                 */
    TS_CLASSMAP   c[TS_CLASSES];
    uint8_t*      line;         /* the fill's line buffer, as long as its largest class needs                     */
} TS_CLASSMAPS;

enum {
    TS_CLASS_OK = 0,
    TS_CLASS_FOOT,              /* a present class with a negative footprint: the fill would index before its
                                   buffer (0x440509..0x440531 sizes it by W + fx), refused and named              */
    TS_CLASS_ROOM               /* the block is smaller than ts_class_bytes asks                                 */
};

/* The bytes the present classes of `cls` need over a w x h grid. */
size_t ts_class_bytes(const TASIM_MOVECLASS cls[TS_CLASSES], int32_t w, int32_t h);

/* Lays the maps out in `mem` over the grid `m` and the units `units` (NULL for none), copies each present class's
   record from `cls` and sets every watermark to 0, as a process's first load finds them. Returns TS_CLASS_OK or the
   refusal, with the refused class in *bad. The maps hold nothing until ts_class_fill_all. */
int ts_class_init(TS_CLASSMAPS* cm, void* mem, size_t bytes, const TASIM_MOVECLASS cls[TS_CLASSES], const TS_MAP* m,
                  TS_CUNITS* units, int* bad);

/* Each function below takes a class index i and answers 0 or does nothing when i is outside 0..TS_CLASSES - 1, or
   the cell or slot it names is off the grid or the units' view.

   The per-cell rule 0x47DE60(class, cell): 0, 1 or 3 for the one cell idx. */
int ts_class_cell(const TS_CLASSMAPS* cm, int i, int32_t idx);

/* The rule's ground half, the depths and the slope (0x47DE60 past its feature and unit rules), for class record c
   over a cell of 2x2 minimum mn and maximum mx under sea level `sea`: 0, 1 or 3. */
int ts_class_ground(const TS_CLASSMAP* c, int32_t sea, int32_t mn, int32_t mx);

/* THE CONTENT RULE: 0x47DE60 for the one cell idx with its unit rule left out -- the cell's feature word and its
   ground alone, what the map's content makes of it. */
int ts_class_content(const TS_CLASSMAPS* cm, int i, int32_t idx);

/* THE FILL'S ENTRIES OVER A WINDOW: grid holds TS_FOLD_CAP(w, h, fx, fz) codes, row-major, of the cells
   [x0 - 1, x0 + w + fx] x [z0 - 1, z0 + h + fz] (0 for a cell off the map), for a class footprint fx x fz, each at
   least 1. Leaves in grid[0 .. w h), row-major, entry (x0 + x, z0 + z) at x + z w as the fill 0x440500 makes it: 0
   when a cell of [x, x + fx) x [z, z + fz) is 0, 3 when every cell of [x - 1, x + fx] x [z - 1, z + fz] is 3, else
   1 (ts_class_fill). O(window): a few byte reads a cell, whatever the footprint. */
#define TS_FOLD_CAP(w, h, fx, fz) ((size_t)((w) + (fx) + 2) * (size_t)((h) + (fz) + 2))
void ts_class_fold(uint8_t* grid, int32_t w, int32_t h, int32_t fx, int32_t fz);

/* 0x47DFC0(class, x, z, w, h): the rectangle's worst code, 0 off the grid. */
int ts_class_rect(const TS_CLASSMAPS* cm, int i, int32_t x, int32_t z, int32_t w, int32_t h);

/* 0x47E1F0(class, x, z): entry (x, z) recomputed, the footprint and then its ring. */
int ts_class_refresh(const TS_CLASSMAPS* cm, int i, int32_t x, int32_t z);

/* The fill 0x440500 of class i, and 0x440940's walk over the 32 classes, filling each present one. */
void ts_class_fill(TS_CLASSMAPS* cm, int i);
void ts_class_fill_all(TS_CLASSMAPS* cm);

/* The stored code of entry (x, z), 0 off the map: the read 0x40D7B0 makes after its exploration test, which needs a
   player's explored map and is the pathfinder's (T1b). */
int ts_class_code(const TS_CLASSMAPS* cm, int i, int32_t x, int32_t z);

/* 0x440830 remark(class, x | z << 16, w | h << 16) and 0x440A40, the same in every present class. */
void ts_class_remark(TS_CLASSMAPS* cm, int i, int32_t x, int32_t z, int32_t w, int32_t h);
void ts_class_remark_all(TS_CLASSMAPS* cm, int32_t x, int32_t z, int32_t w, int32_t h);

/* The claim 0x440A70(unit), the search's watermark 0x440AF0(class, unit) and its cleanup 0x440BE0(class, unit), on
   the unit in `slot` of cm->units at GameTime `now`. */
void ts_class_claim(TS_CLASSMAPS* cm, int32_t slot, uint32_t now);
void ts_class_watermark(TS_CLASSMAPS* cm, int i, int32_t slot, uint32_t now);
void ts_class_cleanup(TS_CLASSMAPS* cm, int i, int32_t slot);

#endif
