#ifndef TAGPU_RECRULES_H
#define TAGPU_RECRULES_H
/* The recorder's rules the wiki is to play: the refresh grid a writer flags, the live-fork bound
   and its eviction choice. Pure and engine-free so the in-game wiki's Rewind entry can include the
   recorder's own code (ingame-wiki-replay-plan.md, decision 9). Today the recorder runs it, and
   tests/wikitest.c runs it against the mockup's twins.

   No state, no engine memory, no Win32: stdint.h only. tools/wiki/mockup/replay.js holds the
   wiki's JavaScript twins (isRefresh, refreshBin, forkEvict, REFRESH_TICKS, FORK_LIMIT,
   TIMELINES), which tools/wiki/simparity.mjs compares with these case by case; the file checks'
   copies are tools/replay/tarec.py's is_refresh and constants. */
#include <stdint.h>

/* THE REFRESH INTERVAL. A refresh iteration is a segment's first or second, one whose GameTime is
   1 modulo the interval (the first after it where an iteration skips that GameTime), and every
   snapshot's (reclossy_is_refresh): on the grid, every refresh is also 1 modulo the radar pass's
   step of 30 ticks, and the grid is the same in every recording. A skirmish header's RT_HOLD
   carries the interval its file was written with (tagpu_recfmt.h); a file without RT_HOLD, a
   multiplayer recording, is written on REC_REFRESH_TICKS's grid. */
#define REC_REFRESH_TICKS 300   /* the interval a recording is written with                     */
#define REC_REFRESH_MIN   30    /* the least a test launch may ask for (tagpu_rec.refresh)       */

#define REC_FORK_LIMIT 3        /* the live forks a recording keeps: one more evicts one
                                   (rec_fork_evict)                                              */
#define RECR_TIMELINES 64       /* the segments a file may hold: a file with more is refused
                                   by name                                                       */

/* the interval of the refresh grid that holds GameTime t: 1 to interval is the first */
static inline unsigned long long refresh_bin(unsigned t, unsigned interval)
{
    return ((unsigned long long)t + interval - 1) / interval;
}

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
static inline int reclossy_is_refresh(unsigned n, unsigned tick, unsigned prev, unsigned interval)
{
    return n <= 2 || (interval && refresh_bin(tick, interval) != refresh_bin(prev, interval));
}

/* a timeline as the eviction sees it, in file order: its id (0 the original), whether it is
   live (not deleted), and its last-used stamp */
typedef struct RecForkRow {
    uint32_t id;
    uint8_t  live;
    uint64_t used;
} RecForkRow;

/* THE EVICTION, the recorder's choice (recr_append_point): never the original, never the
   selected; the least recently used, the earliest on a tie; refused when the live set cannot be
   bounded. Among the live rows other than the original, fewer than `limit` evict none; exactly
   `limit` evict the candidate; more than `limit`, or `limit` with no candidate (every live fork
   the selected one), cannot be bounded. 1 with *evict the id to delete or UINT32_MAX for none;
   0 when the live fork set cannot be bounded, *evict UINT32_MAX. */
static inline int rec_fork_evict(const RecForkRow* rows, unsigned n, uint32_t selected,
                                 unsigned limit, uint32_t* evict)
{
    unsigned i, live = 0;
    uint64_t oldest = UINT64_MAX;
    *evict = UINT32_MAX;
    for (i = 0; i < n; i++)
        if (rows[i].id && rows[i].live) {
            live++;
            if (rows[i].id != selected && (*evict == UINT32_MAX || rows[i].used < oldest)) {
                oldest = rows[i].used;
                *evict = rows[i].id;
            }
        }
    if (live < limit) *evict = UINT32_MAX;
    else if (*evict == UINT32_MAX || live > limit) {
        *evict = UINT32_MAX;
        return 0;
    }
    return 1;
}

#endif
