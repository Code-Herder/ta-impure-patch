#ifndef TAGPU_RESTORE_REF_H
#define TAGPU_RESTORE_REF_H
/* tagpu_restore_ref.h -- the restorer's launch self-test, the half with no
   rendering API in it (research/notes/compute-restorer.md D2, D13): a
   synthetic probe, the network run on the CPU from the weight file the game
   loaded, and the comparison of the device's answer against it.

   WHY ON THE CPU, AND WHY AT LAUNCH. A reference computed from the weights
   actually loaded needs nothing kept in step with them -- no stored answer to
   regenerate when the model changes, no torch in the build. The CPU half runs
   on a worker thread (`tagpu_rref_test_start`) while the device runs the same
   probe, so the launch waits for the slower of the two rather than both.

   THE PROBE IS SYNTHETIC. Its atlases are generated here, from a fixed seed;
   nothing in it comes from the original game. It covers what the device path
   has that a plain convolution does not: both source kinds (an RGBA base with
   alpha-0 keys, and palette indices with a key index), a keyed frame and a
   wrap-padded one in one batch of two slots, so the grid is two conv bands
   tall, the replicated border and its slack, and a two-level mip chain.

   AND THE NEIGHBOURHOODS, against the rule they exist for. A 3 x 3 map of
   16-texel tiles, each cut from a scattered place of the base atlas, is
   restored cell by cell as neighbourhood frames -- all nine, so every edge
   and corner of the mirror rule is in it -- by the model the terrain runs.
   Their reference is not the same addressing done twice: it is ONE restore
   of the whole map, reflect-padded, and each cell with its ring is a crop of
   it. A device that passes paints what a whole-map restore paints. */

#include "tagpu_restore_core.h"

#define TAGPU_RPROBE_DIM   64      /* every probe atlas and destination is square */
#define TAGPU_RPROBE_MIPS  2       /* levels 1..2 of the base job's destination   */
#define TAGPU_RPROBE_NF    2       /* frames per probe job                        */
#define TAGPU_RPROBE_NBT   16      /* the neighbourhood map's tile side           */
#define TAGPU_RPROBE_NBM   3       /* ... and its side, in tiles                  */
#define TAGPU_RPROBE_NBN   (TAGPU_RPROBE_NBM * TAGPU_RPROBE_NBM)

typedef struct {
    unsigned char base[TAGPU_RPROBE_DIM * TAGPU_RPROBE_DIM * 4]; /* RGBA, alpha 0 keyed */
    unsigned char r8[TAGPU_RPROBE_DIM * TAGPU_RPROBE_DIM];        /* palette indices    */
    unsigned char pal[256 * 4];                                    /* R, G, B, pad       */
    TAGPU_RGLSL_FRAME fb[TAGPU_RPROBE_NF];                         /* frames over `base` */
    TAGPU_RGLSL_FRAME fr[TAGPU_RPROBE_NF];                         /* frames over `r8`   */
    unsigned          map[TAGPU_RPROBE_NBN];  /* cell -> its tile in `base`, x | y << 16 */
    TAGPU_RNBFRAME    fn[TAGPU_RPROBE_NBN];   /* every cell of `map`, over `base`        */
} TAGPU_RPROBE;

/* Fill `p`: the same bytes on every call and every machine. */
void tagpu_rref_probe(TAGPU_RPROBE* p);

/* A CPU run of the probe on a worker thread of its own: the two frame jobs
   against `m`, the neighbourhoods against `mn`. The weights it needs are
   copied out of both before this returns, so the thread shares nothing that
   can change under it; NULL when a copy or the thread could not be had. */
typedef struct TAGPU_RTEST TAGPU_RTEST;
TAGPU_RTEST* tagpu_rref_test_start(const TAGPU_RMODEL* m, const TAGPU_RMODEL* mn,
                                   const TAGPU_RPROBE* p);
/* 1 once the worker has finished. Never blocks. */
int tagpu_rref_test_done(TAGPU_RTEST* t);
/* The verdict, once `done`: the device's three destinations as read back --
   `gotB` the base job's whole chain, level after level (level 0, then 1, then
   2), `gotR` the palette job's level 0, `gotN` the neighbourhood job's --
   against the reference. 1 pass, 0 wrong bytes, -1 the reference itself
   could not be computed. `why` names the first failure and says how much was
   compared. */
int tagpu_rref_test_check(TAGPU_RTEST* t, const unsigned char* gotB, const unsigned char* gotR,
                          const unsigned char* gotN, char* why, int whyLen);
/* JOINS THE WORKER FIRST: the thread writes into `t` until it finishes, so
   freeing it earlier would be a use after free. The work is bounded (about a
   gigaflop), so the join always returns. */
void tagpu_rref_test_free(TAGPU_RTEST* t);

/* The size of the base job's whole chain as the readback lays it out, level
   0 then 1 then 2, end to end. */
int tagpu_rref_chain_bytes(void);

#endif
