#ifndef TAGPU_RECLOSSY_H
#define TAGPU_RECLOSSY_H
/* tagpu_reclossy.h -- the demo recorder's lossy filter: which records a skirmish recording's
   stream carries, chosen on the writer thread between the writer's exact copy of the world and
   the stream it writes (tagpu_reclossy.c says how and why). Portable C99 like the codec: no
   Windows, no engine memory, no writable static -- so it is not on thread-split.allow, and the
   build refuses an engine address in it -- and each RecLossy is one writer's, owned by one
   thread. The codec then codes whatever records come, losslessly; the two share no state. */
#include <stddef.h>
#include "tagpu_recfmt.h"
#include "tagpu_reccodec.h"

/* an order list's node images, in list order: the writer's copy and the filter's hold them so */
typedef struct RecLossyList { unsigned n, cap; unsigned char* img; } RecLossyList;

typedef struct RecLossyParams {
    unsigned nslots;                    /* the header's RT_SLOTS, 2..65535                        */
    unsigned flags;                     /* RT_HOLD's: RECL_SPRAYS writes the sprays as sessions   */
    const unsigned char* hold_unit;     /* REC_UNIT_IMAGE bytes, a set bit held (RT_HOLD)          */
    const unsigned char* hold_node;     /* REC_ORDER_IMAGE bytes: a main list node's              */
} RecLossyParams;

/* The writer's exact world at the end of the iteration handed over: nslots unit images and
   liveness, nslots * 2 lists (the main, then the sub list of each slot). Read only. */
typedef struct RecLossyExact {
    const unsigned char* units;
    const unsigned char* live;
    const RecLossyList*  lists;
} RecLossyExact;

typedef struct RecLossy RecLossy;

/* NULL: no memory, or nslots out of its bounds. */
RecLossy* reclossy_begin(const RecLossyParams* p);
void      reclossy_free(RecLossy* L);

/* One completed iteration: `recs`, n bytes, the records as the capture made them, RK_TICK
   first, corrections included. Appends the iteration's stream records to `out`. With `refresh`,
   the lossy world ends equal to `x`, the writer's world at the iteration's end; `snapshot`, a
   refresh a snapshot follows, also writes every spray and closes every spray session. 1, or 0
   for an iteration that breaks a bound the filter keeps -- a writer would have refused it, so the
   writer abandons the file. */
int reclossy_iteration(RecLossy* L, const unsigned char* recs, size_t n, int refresh,
                       int snapshot, const RecLossyExact* x, RecBytes* out);

/* 1 when the lossy world equals `x` byte for byte: liveness, every live unit's image and every
   slot's two lists. The writer asks it after every refresh. */
int reclossy_equal(const RecLossy* L, const RecLossyExact* x);

/* THE REFRESH GRID: 1 when a segment's iteration at GameTime `tick` is a refresh, `n` its ordinal
   in the segment from 1 and `prev` the GameTime of the segment's last refresh before it. The
   segment's first two iterations are, and after them the first iteration in each interval of
   `interval` GameTimes (1 to interval, interval + 1 to 2 * interval, ...) other than the last
   refresh's: on consecutive GameTimes those 1 modulo the interval, and past a gap that skips
   such a GameTime the first iteration after it. The second is one because a takeover starts at
   a refresh and its fork is appended after the iteration before it (tagpu_replay.c,
   takeover_point): at the first, that iteration is the parent's or none, so a request inside the
   first interval rounds to the second, whose iteration before is the segment's own. The writer
   decides with this and every check of a file mirrors it (codectest lossy, tarec.py lossy-check).
   `interval` is REC_REFRESH_MIN..REC_REFRESH_TICKS; 0 makes only the first two refreshes. */
int reclossy_is_refresh(unsigned n, unsigned tick, unsigned prev, unsigned interval);

/* THE SPRAY SESSIONS' TABLE, which every reader of a stream with RECL_SPRAYS keeps and the filter
   writes by (tagpu_reclossy.c, THE SPRAY SESSIONS): each unit slot's session and its sprays of
   the last four GameTimes. One table per reader, owned by its thread. */
typedef struct RecSprays RecSprays;

/* NULL: no memory, or nslots out of 2..65535. */
RecSprays* recsprays_begin(unsigned nslots);
void       recsprays_free(RecSprays* t);
/* no session and no spray: where a reader holds nothing from before (a reset, a snapshot) */
void       recsprays_empty(RecSprays* t);
/* An RK_SPRAY_SESSION's body: 1, the unit's session from this iteration on; 0 malformed (its
   length, its zero bytes, a mode outside RK_SPRAY_SESSION's, an ID that names no unit slot). */
int        recsprays_session(RecSprays* t, const unsigned char* b, unsigned len);
/* An RK_EV_SPRAY of iteration T, 48 bytes, taken into the table: `units` the slots' images
   (REC_UNIT_IMAGE each) and `live` their liveness at T. A spray whose ID names no unit slot
   enters none. */
void       recsprays_spray(RecSprays* t, unsigned T, const unsigned char* b, const void* units,
                           const unsigned char* live);
/* At the end of iteration T, once its records are taken: the sprays the sessions imply, 48 bytes
   each (an RK_EV_SPRAY's body), slots ascending, into a buffer of the table's that stays as it
   is until the next call; each taken into the table. 1, or 0 when a session would predict from a
   GameTime with no single spray of its unit, or follows a unit not live at T or then: malformed,
   since the filter never implies such a spray. */
int        recsprays_imply(RecSprays* t, unsigned T, const void* units, const unsigned char* live,
                           const unsigned char** out, unsigned* n);

/* RT_HOLD's bytes (tagpu_recfmt.h) */
typedef struct RecLossyHold {
    unsigned      refresh, flags;
    unsigned char sha[32];
    unsigned char unit[REC_UNIT_IMAGE], node[REC_ORDER_IMAGE];
} RecLossyHold;
/* An RT_HOLD entry's `n` bytes: 1, or 0 with `why` -- not REC_HOLD_BYTES long, a flag outside
   RECL_CELLS | RECL_SPRAYS, a refresh interval outside REC_REFRESH_MIN..REC_REFRESH_TICKS, or a
   held bit of the unit position +0x6A..+0x75, which a spray session that follows its unit reads.
   Every reader parses it so, and the generator refuses such a row too (replay-hold-gen.py). */
int reclossy_hold_parse(RecLossyHold* h, const void* p, unsigned n, char* why, unsigned cap);

/* One stream block of a segment, as a reader's index holds it: its GameTimes and whether it
   carries RBF_REFRESH. */
typedef struct RecLossyBlock { unsigned first, last, refresh; } RecLossyBlock;

/* The last refresh iteration at or before T of a segment whose first iteration is `first`, from
   its stream blocks b[0..n) in file order: the largest `last` <= T of a block with RBF_REFRESH,
   or `first`, which is always one. 0 when T is before `first`. */
unsigned reclossy_refresh_at(const RecLossyBlock* b, unsigned n, unsigned first, unsigned T);

#endif
