/* tagpu_lerp.c -- smooth-motion.md option A: history-based interpolation of a
   unit's COB piece pose. See tagpu_lerp.h for the invariants; this file is the
   history table and the blend.

   THE SHAPE. Every drawn unit gets a record keyed by (Object3do, nparts, level
   generation) holding TWO snapshots of its P_POS/P_TURN triples -- the pose at
   sim tick T-1 and at T. posed_pose then draws at T-1+u, where u is the
   fraction of tick T that has elapsed on the wall clock. So the model sweeps
   continuously through the poses the script authored instead of snapping
   between them, at the cost of rendering the POSE (not the unit's position)
   one tick behind the simulation.

   WHY THE KEY HAS THREE PARTS. Unit array slots are recycled and Object3do
   allocations are reused, so a pointer alone would let a newly spawned unit
   inherit a dead one's poses and blend across the two -- a limb sweeping in
   from wherever the previous occupant held it. `nparts` catches a different
   model landing on the same address; the level generation catches a whole map
   teardown, which is the case tagpu_reclaim already bumps a counter for.

   WHY BLOCKS OF 48 PIECES. The arena is fixed and preallocated (3.4 MB, less
   than the 4.7 MB pose arena beside it), and a fixed block size means no
   fragmentation and no allocator: slot i owns pieces [i*48, i*48+48). 48 is
   above every stock model (36 on ARMSCORP/CORSCORP, the largest) and below
   TAGPU_PBMAXPIECE (256), so a hypothetical 200-piece model gets NO history
   and draws stepped -- counted as `big=`, never dropped. That is invariant 2:
   a refusal here is a blend weight of 1.0, which is a `return 0` and the
   caller's untouched code.

   THREADING. Render thread only, reached from posed_pose and from
   tagpu_native_frame. Nothing here is written from the game thread and nothing
   here writes to the engine. */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "tagpu_lerp.h"
#include "tagpu_model3do.h"      /* TAGPU_PBMAXPIECE, P_POS, P_TURN */
#include "tagpu_reclaim.h"       /* tagpu_reclaim_level_gen */
#include "tagpu_opt.h"

#define TA_MAINPP   0x00511DE8u
#define OFF_TICK    0x38A47      /* int GameTime, 30 a second */

#define LERP_HASH   4096         /* power of two, comfortably over MAXU 2048 */
#define LERP_PROBE  8
#define LERP_BLOCK  48           /* pieces of history one unit may hold      */
#define LERP_SLOTS  2048         /* = MAXU: one block per drawable unit      */
#define LERP_STALE  180          /* frames unseen before a record is dropped */
#define LERP_SWEEP  256          /* records aged per frame (16 frames/table) */
#define LERP_MAXGAP 8            /* sim ticks a usable sample pair may span */

/* THE ARENA. Two banks, so a new sample is written into the one the record is
   not currently pointing at and the "shift" is a flipped index rather than a
   memcpy of the whole unit. 2 * 98304 pieces * (12 B pos + 6 B turn). */
static int            s_pos[2][LERP_SLOTS * LERP_BLOCK * 3];   /* 2.25 MB */
static unsigned short s_turn[2][LERP_SLOTS * LERP_BLOCK * 3];  /* 1.13 MB */

typedef struct {
    const char*   o3;        /* key, NULL = empty                            */
    unsigned      gen;       /* level generation this record was claimed in  */
    unsigned      frame;     /* last frame it was asked for (ageing)         */
    unsigned      tick;      /* sim tick of the CUR sample                   */
    int           nparts;
    short         slot;      /* arena block, -1 = none (draws stepped)       */
    DWORD         ms;        /* wall clock when the CUR sample was taken     */
    unsigned char cur;       /* which bank holds the CUR sample              */
    unsigned char gap;       /* sim ticks between PREV and CUR, 0 = unusable */
    unsigned char have;      /* 0 nothing, 1 cur only, 2 cur AND a usable
                                prev -- only 2 can be interpolated          */
} LREC;

static LREC     s_rec[LERP_HASH];
static short    s_free[LERP_SLOTS];
static int      s_nfree = -1;                 /* -1 = the pool is unbuilt    */

static int      s_armed = -1;                 /* -1 = never polled           */
static unsigned s_gen = 0;
static unsigned s_frame = 0;
static unsigned s_sweep = 0;
static unsigned s_tick = 0;
static int      s_haveTick = 0;
static DWORD    s_tickMs = 0;
static DWORD    s_now = 0;                    /* this frame's wall clock     */
/* MEASURED, NEVER ASSUMED -- and the first thing the instrument found. The
   counter at main+0x38A47 advances 60 a second in a skirmish, not the 30 the
   engine's own +clock arithmetic implies, so a hard-coded 33.3 ms would have
   run every blend at half speed and the legs would have lagged the unit by a
   whole stride. It is learned from the observed interval DIVIDED BY THE GAP,
   because below the sim rate a render frame sees several ticks arrive at once
   and the raw interval is that many periods. */
static float    s_period = 1000.0f / 30.0f;   /* observed ms per sim tick    */

/* window counters, printed on the native: line and reset with it */
static unsigned s_nblend = 0, s_nsnap = 0, s_nbig = 0, s_nfull = 0;
static float    s_lastU = 0.0f;               /* for the native: line */

/* A cheap filter on a VALUE, exactly as tagpu_native.c's ptr_ok is -- it is
   NOT the safety argument. What makes the read below safe is that `main` is
   the engine's session-lifetime global (five other modules already read the
   tick out of it) and that everything derived from it here is a clamped
   float weight that reaches no pointer and no index. */
static int lptr_ok(const void* p)
{
    return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u;
}

static void llog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static void pool_build(void)
{
    int i;
    memset(s_rec, 0, sizeof s_rec);
    for (i = 0; i < LERP_SLOTS; i++) s_free[i] = (short)i;
    s_nfree = LERP_SLOTS;
}

static void rec_release(LREC* r)
{
    if (r->slot >= 0 && s_nfree < LERP_SLOTS) s_free[s_nfree++] = r->slot;
    memset(r, 0, sizeof *r);
}

void tagpu_lerp_frame(unsigned frameCounter)
{
    unsigned gen;
    int i, was = s_armed;

    s_frame = frameCounter;
    if (s_nfree < 0) pool_build();

    /* the lever, on the pass's own 30-frame cadence -- tagpu_opt is stateless
       and answers from the file system, so this is not a per-frame stat */
    if (s_armed < 0 || (frameCounter % 30) == 0)
        s_armed = tagpu_opt_on("tagpu_lerp.on") ? 1 : 0;
    if (was != s_armed && was != -1)
        llog(s_armed ? "lerp: ON -- piece poses interpolated between sim ticks"
                     : "lerp: off -- piece poses step at the sim tick");
    if (!s_armed) {
        if (was == 1) pool_build();      /* drop the history rather than age it */
        return;
    }

    /* A LEVEL CHANGE INVALIDATES EVERY KEY AT ONCE. Object3do allocations are
       reused across a teardown, so a surviving record would blend a new unit
       toward a dead one's pose. */
    gen = tagpu_reclaim_level_gen();
    if (gen != s_gen) { s_gen = gen; pool_build(); }

    /* the sim tick and the phase inside it */
    {
        const char* ta = *(const char* const*)TA_MAINPP;
        DWORD now = timeGetTime();
        s_now = now;
        if (!lptr_ok(ta)) { s_haveTick = 0; return; }
        {
            unsigned tick = (unsigned)*(const int*)(ta + OFF_TICK);
            if (!s_haveTick || tick != s_tick) {
                unsigned gap = s_haveTick ? tick - s_tick : 0;
                DWORD dt = now - s_tickMs;
                if (gap >= 1 && gap <= LERP_MAXGAP && dt > 0 && dt < 500) {
                    float per = (float)dt / (float)gap;
                    s_period = s_period * 0.75f + per * 0.25f;
                    if (s_period < 4.0f) s_period = 4.0f;
                    if (s_period > 100.0f) s_period = 100.0f;
                }
                s_tick = tick;
                s_tickMs = now;
                s_haveTick = 1;
            }
        }
    }

    /* age a slice of the table: a unit that died, went off screen or lost its
       bake stops asking, and its block has to come back to the pool */
    for (i = 0; i < LERP_SWEEP; i++) {
        LREC* r = &s_rec[(s_sweep + (unsigned)i) & (LERP_HASH - 1)];
        if (r->o3 && (s_frame - r->frame) > LERP_STALE) rec_release(r);
    }
    s_sweep = (s_sweep + LERP_SWEEP) & (LERP_HASH - 1);
}

/* The record for this unit, or NULL. Claims one on first sight. */
static LREC* lookup(const char* o3, int nparts)
{
    unsigned h = (unsigned)(((size_t)o3 >> 4) * 2654435761u) & (LERP_HASH - 1);
    LREC* cand = NULL;
    int k;
    for (k = 0; k < LERP_PROBE; k++) {
        LREC* r = &s_rec[(h + (unsigned)k) & (LERP_HASH - 1)];
        if (!r->o3) { if (!cand) cand = r; continue; }
        if (r->o3 != o3) continue;
        if (r->nparts == nparts && r->gen == s_gen) { r->frame = s_frame; return r; }
        /* SAME ADDRESS, DIFFERENT UNIT -- the recycled-slot case the key
           exists for. Recycle the record in place; do not blend across it. */
        rec_release(r);
        cand = r;
        break;
    }
    if (!cand) { s_nfull++; return NULL; }      /* probe run full: draw stepped */
    memset(cand, 0, sizeof *cand);
    cand->o3 = o3;
    cand->gen = s_gen;
    cand->nparts = nparts;
    cand->frame = s_frame;
    cand->slot = -1;
    if (nparts > LERP_BLOCK) { s_nbig++; return cand; }
    if (s_nfree > 0) cand->slot = s_free[--s_nfree];
    else s_nfull++;
    return cand;
}

/* PREV -> CUR at weight u, field by field. Invariants 3 and 4 live here. */
static void blend(const LREC* r, size_t base, int n3, float u,
                  int* obuf, unsigned short* otbuf)
{
    const int*            ap = s_pos[1 - r->cur] + base;
    const int*            bp = s_pos[r->cur] + base;
    const unsigned short* at = s_turn[1 - r->cur] + base;
    const unsigned short* bt = s_turn[r->cur] + base;
    int i;
    for (i = 0; i < n3; i++) {
        /* 16.16 position. The delta over one tick is a piece's own travel, but
           the subtraction is done wide so that no pair of endpoints can
           overflow it whatever a mod puts in those fields. */
        long long d = (long long)bp[i] - (long long)ap[i];
        obuf[i] = ap[i] + (int)((double)d * u);
        {
            /* TAang WRAPS: 350 deg -> 10 deg is +20, not -340. The short way
               round, which is the way the engine's own TURN takes it
               (file-formats.md section 2.6). */
            int w = (int)(unsigned short)(bt[i] - at[i]);
            if (w > 32768) w -= 65536;
            otbuf[i] = (unsigned short)((int)at[i] + (int)((float)w * u));
        }
    }
}

int tagpu_lerp_unit(const char* o3, int nparts, const char* const* pr,
                    const int** pos, const unsigned short** turn)
{
    static int            obuf[LERP_BLOCK * 3];   /* render thread only */
    static unsigned short otbuf[LERP_BLOCK * 3];
    LREC* r;
    size_t base;
    int i, n3;

    if (!s_armed || !s_haveTick || nparts <= 0 || nparts > LERP_BLOCK) {
        if (s_armed && nparts > LERP_BLOCK) s_nbig++;
        return 0;
    }
    r = lookup(o3, nparts);
    if (!r || r->slot < 0) { s_nsnap++; return 0; }
    base = (size_t)r->slot * LERP_BLOCK * 3;
    n3 = nparts * 3;

    /* SAMPLE, once per unit per sim tick. Into the bank the record is not
       pointing at, then flip -- so the previous sample survives untouched. */
    if (!r->have || r->tick != s_tick) {
        unsigned gap = r->have ? s_tick - r->tick : 0;
        unsigned char nx = (unsigned char)(1 - r->cur);
        int* dp = s_pos[nx] + base;
        unsigned short* dt = s_turn[nx] + base;
        for (i = 0; i < nparts; i++) {
            const int* mv = (const int*)(pr[i] + P_POS);
            const unsigned short* tn = (const unsigned short*)(pr[i] + P_TURN);
            dp[i * 3 + 0] = mv[0]; dp[i * 3 + 1] = mv[1]; dp[i * 3 + 2] = mv[2];
            dt[i * 3 + 0] = tn[0]; dt[i * 3 + 1] = tn[1]; dt[i * 3 + 2] = tn[2];
        }
        r->cur = nx;
        r->tick = s_tick;
        r->ms = s_now;
        /* THE GAP IS THE UNIT'S OWN, NOT THE FRAME'S. A render slower than the
           sim sees ticks arrive several at a time, and a unit that was off
           screen or past the pose arena has not been sampled for many more
           than that -- so the pair to interpolate is not "last tick and this
           tick", it is "the last two samples", and the wall-clock window to
           spread them over is however many ticks apart they were. Past
           LERP_MAXGAP the pair is too stale to be a motion at all: that unit
           draws stepped for one sample and is interpolable from the next. */
        r->gap = (unsigned char)((gap >= 1 && gap <= LERP_MAXGAP) ? gap : 0);
        r->have = (unsigned char)(r->gap ? 2 : 1);
    }

    /* THE DEGRADATION IS A `return 0`, NOT A LERP AT WEIGHT 1. `a+(b-a)*1.0f`
       is not `b` in floating point, so "blend with weight 1" would have been
       off by an ulp on every piece of every unit and invariant 2's parity
       claim would have been false. Refusing hands the caller back its own
       untouched read of the live fields, which is bit-identical by
       construction. */
    if (r->have < 2) { s_nsnap++; return 0; }
    {
        float u = (float)(s_now - r->ms) / ((float)r->gap * s_period);
        /* `>= 0`, NOT `> 0` -- and the difference is a visible stutter, not a
           nicety. On the frame a sample lands, s_now IS r->ms, so u is exactly
           0 and the honest answer is "show PREV". Refusing there instead shows
           the live fields, which are CUR: the unit would jump forward one tick
           and be pulled back on the very next frame, once per tick, forever.
           u == 0 blends exactly (`ap[i] + (int)(d * 0.0f)` is `ap[i]`), so
           there is nothing to protect against. NaN fails `>= 0` just as it
           failed `> 0`. */
        if (!(u >= 0.0f)) { s_nsnap++; return 0; }
        if (u >= 1.0f) { s_nsnap++; return 0; }     /* weight 1.0 is a refusal */
        s_lastU = u;
        blend(r, base, n3, u, obuf, otbuf);
    }
    *pos = obuf;
    *turn = otbuf;
    s_nblend++;
    return 1;
}

void tagpu_lerp_stats(char* buf, unsigned cap)
{
    int n;
    if (cap == 0) return;
    buf[0] = 0;
    if (s_armed != 1) return;
    n = _snprintf(buf, cap, " lerp=%u/%u p=%.1fms u=%.2f",
                  s_nblend, s_nsnap, s_period, s_lastU);
    if (n < 0 || (unsigned)n >= cap) { buf[cap - 1] = 0; return; }
    if (s_nbig || s_nfull)
        _snprintf(buf + n, cap - (unsigned)n, " big=%u full=%u", s_nbig, s_nfull);
    buf[cap - 1] = 0;
    s_nblend = s_nsnap = s_nbig = s_nfull = 0;
}
