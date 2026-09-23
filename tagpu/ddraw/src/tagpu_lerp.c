/* tagpu_lerp.c -- smooth-motion.md option A: history-based interpolation of a
   unit's COB piece pose. See tagpu_lerp.h for the invariants; this file is the
   pairing and the blend.

   THE SHAPE. The consumer holds two frame packets; `prev` carries the pose at an earlier sim tick and `read` the pose
   at a later one, and the exchange's own rotation guarantees the two ticks
   differ, so there is no history to keep here at all. `posed_pose` draws at
   prev + (read - prev) * u, where u is how far through the interval between
   the two ticks the wall clock now is. The model therefore sweeps continuously
   through the poses the script authored instead of snapping between them, at
   the cost of rendering the POSE (not the unit's position) one tick behind.

   THE CLOCK IS THE PRODUCER'S, NOT OURS. Each packet stamps
   QueryPerformanceCounter the instant the GAME thread first saw its tick
   change -- within one engine draw of the true tick boundary, and the engine
   draws hundreds of times a second. So the interval between two packets'
   stamps IS the measured duration of the ticks between them, per pair, and
   nothing here learns or smooths a period. u is (now - read.tick_start)
   divided by that interval: 0 on the frame the later tick's first packet arrives,
   which is the frame that must show PREV.

   UNITS ARE MATCHED BY THE STABLE ID (unit+0xA8), never by table position --
   the tables are in engine slot order and a death between the two ticks would
   otherwise shift every unit after it by one and blend each toward its
   neighbour. The pairing is verified with the model identity as well
   (Object3do key, piece count, UnitDef row), which narrows the one residual
   this module has: the engine RECYCLES an in-game index when a unit
   dies, so a unit that died and one that took its id within a single tick
   could be paired. It would have to be of the same type, on the same
   Object3do allocation, within one tick; the cost is one frame of a limb
   sweeping in from the dead unit's stance, and the collision oracle in the
   publisher (`dup=` on the heartbeat) is what says the id is unique among the
   LIVE units of one packet.

   THREADING. Render thread only, reached from posed_pose and from
   tagpu_native_frame. Nothing here reads engine memory, and nothing here
   writes to it. */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "tagpu_lerp.h"
#include "tagpu_packet.h"
#include "tagpu_model3do.h"      /* TAGPU_PBMAXPIECE */
#include "tagpu_opt.h"
#include "tagpu_log.h"

/* id -> index in the PREV packet's units table, rebuilt once per frame and
   cleared only over the ids it used. 64 K entries of 2 bytes: the id is a u16
   and the engine's own slot count tops out well inside it. */
#define LERP_IDS  65536
static unsigned short s_prevOf[LERP_IDS];
static unsigned short s_idUsed[TAGPU_PK_MAX_UNITS];
static unsigned       s_nIdUsed;
static int            s_idInit;

static int      s_armed = -1;                 /* -1 = never polled           */
static unsigned s_frame;
static const TAGPU_PACKET* s_pk;              /* this frame's later tick     */
static const TAGPU_PACKET* s_prev;            /* ...and its earlier one      */
static int      s_w16;                        /* the weight, 16.16, [0,65535]*/
static int      s_have;                       /* a usable pair this frame    */
static float    s_lastU;
static float    s_lastSpanMs;

/* window counters, printed on the native: line and reset with it */
static unsigned s_nblend, s_nsnap, s_nmiss;

static void llog(const char* s)
{
    tagpu_log(s);
}

static LONGLONG qpc_of(const TAGPU_PACKET* p)
{
    return ((LONGLONG)(unsigned)p->tick_start_hi << 32) | (LONGLONG)(unsigned)p->tick_start_lo;
}

void tagpu_lerp_frame(unsigned frameCounter, const TAGPU_PACKET* pk, const TAGPU_PACKET* prev)
{
    int was = s_armed;
    unsigned i;

    s_frame = frameCounter;
    s_pk = NULL; s_prev = NULL; s_have = 0; s_w16 = 0;

    /* the lever, on the pass's own 30-frame cadence -- tagpu_opt is stateless
       and answers from the file system, so this is not a per-frame stat */
    if (s_armed < 0 || (frameCounter % 30) == 0)
        s_armed = tagpu_opt_on("tagpu_lerp.on") ? 1 : 0;
    if (was != s_armed && was != -1)
        llog(s_armed ? "lerp: ON -- piece poses interpolated between sim ticks"
                     : "lerp: off -- piece poses step at the sim tick");
    if (!s_armed || !pk || !prev) return;
    /* the acquire hands out `prev` only when it is the same level, in game and
       a DIFFERENT tick, so these are assertions about the exchange rather than
       filters -- but a blend divides by the span, and a zero span would be an
       infinity that reached a clamp instead of a refusal */
    if (!pk->in_game || !prev->in_game) return;
    if (pk->level_gen != prev->level_gen) return;
    if (pk->tick == prev->tick) return;
    if (!pk->n_units || !prev->n_units) return;

    {
        LARGE_INTEGER now, freq;
        LONGLONG t1 = qpc_of(pk), t0 = qpc_of(prev), span, el;
        double u;
        if (!t1 || !t0 || t1 <= t0) return;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        span = t1 - t0;
        el   = now.QuadPart - t1;
        /* A SPAN THAT IS NOT A PLAUSIBLE RUN OF SIM TICKS IS NOT A CLOCK. The
           sim runs at 3 x GameSpeed a second, so a tick is 8..33 ms and the
           two packets are a handful of ticks apart at most; anything past half
           a second is a stall, a load or a paused sim, and blending over it
           would smear one pose across seconds of wall clock. */
        if (freq.QuadPart > 0) {
            double ms = (double)span * 1000.0 / (double)freq.QuadPart;
            s_lastSpanMs = (float)ms;
            if (!(ms > 0.5 && ms < 500.0)) return;
        }
        if (el < 0) return;                 /* the stamp is in our future     */
        u = (double)el / (double)span;
        if (!(u >= 0.0)) return;            /* NaN fails this, as it must     */
        if (u >= 1.0) { return; }           /* weight 1.0 is a refusal, never
                                               an extrapolation past the tick */
        s_lastU = (float)u;
        s_w16 = (int)(u * 65536.0);
        if (s_w16 < 0) s_w16 = 0;
        if (s_w16 > 65535) s_w16 = 65535;   /* the turn multiply below has only
                                               32767 of headroom: t reaches
                                               +32768 and 32768 * 65536 would
                                               overflow a signed int          */
    }

    /* THE ID INDEX OVER PREV, cleared only where the last frame set it -- a
       memset of 128 KB per frame would cost more than the pairing it serves.
       `s_prevOf` is all-zero as a static, which is a valid index, so the first
       call fills it with the empty marker explicitly. */
    if (!s_idInit) { memset(s_prevOf, 0xFF, sizeof s_prevOf); s_idInit = 1; s_nIdUsed = 0; }
    for (i = 0; i < s_nIdUsed; i++) s_prevOf[s_idUsed[i]] = 0xFFFFu;
    s_nIdUsed = 0;
    {
        const TAGPU_PK_UNIT* pu = tagpu_pk_units(prev);
        unsigned n = prev->n_units;
        if (n > TAGPU_PK_MAX_UNITS) n = TAGPU_PK_MAX_UNITS;
        for (i = 0; i < n; i++) {
            unsigned id = pu[i].id;
            if (s_prevOf[id] != 0xFFFFu) continue;   /* a duplicate id in prev:
                                                        the first entry wins and
                                                        the publisher counted it */
            s_prevOf[id] = (unsigned short)i;
            s_idUsed[s_nIdUsed++] = (unsigned short)id;
        }
    }
    s_pk = pk; s_prev = prev; s_have = 1;
}

/* PREV -> CUR at weight u, field by field. Invariants 3 and 4 live here.

   THE WEIGHT IS A 16.16 INTEGER AND THE LOOP TOUCHES NO FLOAT AT ALL. This
   target has no SSE, so there is no `cvttss2si`: C requires a float->int
   conversion to truncate toward zero, the x87 rounds to nearest, and GCC
   therefore brackets EVERY `(int)` of a float with a control-word save and
   restore. Two such conversions per iteration put FOUR `fldcw` in the loop --
   a serialising reload of the whole x87 state -- for about ten cycles of
   actual arithmetic.

   MEASURED FROM THE COMPILER, not from a stopwatch: both forms built with this
   makefile's own flags give 13 x87 instructions including 4 `fldcw` for the
   float loop and ZERO for this one (smooth-motion.md section 7i).

   The blend does not need floating point. `u` is in [0,1), so 16.16 gives it
   1/65536 of a tick of resolution, which is finer than a piece moves in a tick
   by orders of magnitude; the position delta stays a WIDE multiply so no pair
   of endpoints can overflow it whatever a mod puts in those fields, and the
   turn delta is 17 bits at most and fits a plain int multiply. */
static void blend(const TAGPU_PK_PIECE* a, const TAGPU_PK_PIECE* b, int nparts,
                  int w16, int* obuf, unsigned short* otbuf)
{
    int i, k;
    for (i = 0; i < nparts; i++) {
        for (k = 0; k < 3; k++) {
            long long d = (long long)b[i].pos[k] - (long long)a[i].pos[k];
            obuf[i * 3 + k] = a[i].pos[k] + (int)((d * w16) >> 16);
            {
                /* TAang WRAPS: 350 deg -> 10 deg is +20, not -340. The short
                   way round, which is the way the engine's own TURN takes it
                   (file-formats.md section 2.6). */
                int t = (int)(unsigned short)(b[i].turn[k] - a[i].turn[k]);
                if (t > 32768) t -= 65536;
                otbuf[i * 3 + k] = (unsigned short)((int)a[i].turn[k] + ((t * w16) >> 16));
            }
        }
    }
}

int tagpu_lerp_unit(const TAGPU_PK_UNIT* u, const TAGPU_PK_PIECE* cur, int nparts,
                    const int** pos, const unsigned short** turn)
{
    static int            obuf[TAGPU_PBMAXPIECE * 3];   /* render thread only */
    static unsigned short otbuf[TAGPU_PBMAXPIECE * 3];
    const TAGPU_PK_UNIT*  pu;
    const TAGPU_PK_PIECE* old;
    unsigned idx;

    if (!s_have || !u || !cur || nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    idx = s_prevOf[u->id];
    if (idx == 0xFFFFu || idx >= s_prev->n_units) { s_nmiss++; return 0; }
    pu = tagpu_pk_units(s_prev) + idx;
    /* the same unit, the same model, the same run length: anything else is a
       recycled id or a model swapped under it, and the answer is the packet's
       own pose rather than a sweep between two different things */
    if (pu->id != u->id || pu->o3_key != u->o3_key ||
        pu->type_row != u->type_row || (int)pu->piece_n != nparts) { s_nmiss++; return 0; }
    old = tagpu_pk_pieces(s_prev, pu->piece_off, pu->piece_n);
    if (!old) { s_nsnap++; return 0; }

    /* THE DEGRADATION IS A `return 0`, NOT A LERP AT WEIGHT 1. `a+(b-a)*1.0f`
       is not `b` in floating point, so "blend with weight 1" would be off by
       an ulp on every piece of every unit and invariant 2's parity claim
       would be false. Refusing hands the caller back the packet's
       own untouched triples, which is bit-identical by construction. */
    blend(old, cur, nparts, s_w16, obuf, otbuf);
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
    n = _snprintf(buf, cap, " lerp=%u/%u span=%.1fms u=%.2f",
                  s_nblend, s_nsnap, s_lastSpanMs, s_lastU);
    if (n < 0 || (unsigned)n >= cap) { buf[cap - 1] = 0; return; }
    if (s_nmiss)
        _snprintf(buf + n, cap - (unsigned)n, " miss=%u", s_nmiss);
    buf[cap - 1] = 0;
    s_nblend = s_nsnap = s_nmiss = 0;
}
