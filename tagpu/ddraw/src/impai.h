#ifndef IMPAI_H
#define IMPAI_H
/* impai.h -- ImpAI's engine-free half: what each seat knows of every unit slot, kept by one tick a seat over a
   WORLDQ (worldq.h), the engine's (tagpu_impai.c) or the shared core's (tests/wikitest.c `impai`).
   research/notes/modules/impai.md, What a seat knows.

   THE INFORMATION RULE. A seat's row of a slot changes only through what the seat observes: the WORLDQ's
   observe (its own units, its line of sight, an ally's shared radar), its own radar pass, and its own sight of
   the block where it last knew something. A death or a slot reuse the seat cannot see changes nothing it knows,
   so no engine fact reaches a decision through a row.

   No file of this half includes the engine header (thread-split.allow's default-deny), so it cannot read
   anything but its arguments: what it keeps is a function of the WORLDQ's answers and its own state. Integers
   only; no clock, no random generator. */
#include <stdint.h>
#include "worldq.h"

/* IMPAI_SEEN.how: the classes the seat holds the slot by now */
#define IMPAI_OWN     0x01u
#define IMPAI_LOS     0x02u
#define IMPAI_SHARED  0x04u
#define IMPAI_RADAR   0x08u
#define IMPAI_SONAR   0x10u
#define IMPAI_GHOST   0x20u     /* none any longer: the row is the last thing known, at its block        */
#define IMPAI_NOW     (IMPAI_OWN | IMPAI_LOS | IMPAI_SHARED)
#define IMPAI_PASS    (IMPAI_RADAR | IMPAI_SONAR)

/* One seat's knowledge of one slot, 8 bytes. An all-zero row is empty: the seat knows nothing there. */
typedef struct IMPAI_SEEN {
    uint16_t type;              /* the type last identified; WQ_NONE when never identified                */
    uint8_t  how;               /* IMPAI_*                                                                */
    uint8_t  owner;             /* 0..9, 0xFF when never identified                                       */
    uint16_t bx, bz;            /* the 32-px block last known                                             */
} IMPAI_SEEN;
typedef struct IMPAI_ROW { IMPAI_SEEN seat[WQ_SEATS]; } IMPAI_ROW;     /* keyed by unit slot            */

/* One seat's progress through its observation, plain values. */
typedef struct IMPAI_SEAT {
    uint16_t obs_cursor;        /* the next slot the line-of-sight slice looks at                         */
    uint16_t pad;
    WQ_RADAR radar;             /* the radar pass in progress                                             */
    uint32_t known_gen;         /* bumped on every write of this seat's rows                              */
    uint32_t radar_done;        /* the gametime the last pass was published                               */
} IMPAI_SEAT;

/* The budgets of one seat's tick, each a count the loops are bounded by. */
typedef struct IMPAI_BUDGET {
    uint32_t obs_slice;         /* slots the line-of-sight slice looks at                                 */
    uint32_t radar_visits;      /* the radar pass's target visits, rounded up to one walk                 */
} IMPAI_BUDGET;

/* What one tick did, against its bounds: `slots` the observe calls (the slice's, and a completed pass's
   publish), `visits` the radar pass's. */
typedef struct IMPAI_WORK {
    uint32_t slots, visits;
} IMPAI_WORK;

/* The tick's phase of a radar pass: a seat starts one when idle and gametime % 30 == impai_radar_phase(seat),
   staggered so two seats never start on one tick. */
int impai_radar_phase(int seat);

/* One tick of seat q->seat: the line-of-sight slice, then the radar pass's step and, when it completes, its
   publish. `rows` holds nrows rows, at least q's nslots; `bits` the seat's pass, nbytes at least
   WQ_RADAR_BYTES(nslots). Returns 0, or -1 when the arrays are short of the world (nothing is written). */
int impai_know_tick(const WORLDQ* q, IMPAI_SEAT* st, IMPAI_ROW* rows, int nrows, uint8_t* bits, int nbytes,
                    const IMPAI_BUDGET* b, IMPAI_WORK* w);

#endif
