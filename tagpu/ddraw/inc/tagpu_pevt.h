#ifndef TAGPU_PEVT_H
#define TAGPU_PEVT_H
/* tagpu_pevt -- a particle EMITTER EVENT, and the closed form a renderer draws it by
   (gpu-particles-plan.md, *The emitter event*; effects.md §7.8).

   On a Vulkan launch every particle class is drawn from events: the nano spray (emitters
   0x4720D0, 0x472200), fire (0x472330, 0x472AB0), the teleport flare (0x471FD0), the wake
   with the submarines' bubbles (0x472430, 0x472530), Smoke2 (0x472630, 0x472810, 0x4728F0,
   0x4729D0) and Smoke1 (0x472C50). The game
   thread records one event per emission (tagpu_patches.c, *The particles as events*), the frame
   packet carries the live ones, and the GPU computes every particle from its event and the
   packet's GameTime (tagpu_fx.c NVS). The engine's object is a pure function of the event, so
   this header is that function, written once: the oracle (tagpu_particlecheck.on) holds the
   engine's objects to it every tick, and NVS is its GLSL edition, statement for statement --
   EDIT BOTH OR NEITHER.

   EVERY EVENT: seed, the emission's seed tagpu_prand_seed(level, T, n) (tagpu_prand.h); t0, T,
   the GameTime of the call; flags, bits 0..3 the layer, TAGPU_PEVT_LATE, TAGPU_PEVT_BURST2 and
   bits 8..11 the class.

   A NANO EVENT is what the emitter's init 0x473B50 and spawn 0x473D50 take from the call:
     box1/2  the two boxes as the init cuts them: per axis {base x, alt, y, extent x, alt, y},
             16.16, base = min + 4d/11 and extent = 7d/11 - 4d/11, d = max - min, each division
             toward zero on the 32-bit products (0x473B80..0x473D34);
     steps   each particle's step count, the int16 high word of (len << 16) / 0x40000, len the
             x87 length of its path (0x473E75..0x473ED9). The emitter computes it on the game
             thread with the spawn's own instruction sequence, because that is a float square root
             under the thread's x87 control word, which no other processor reproduces bit for
             bit; everything else here is integer arithmetic.
   The object it stands for: five particles at T (burst 1, ordinals 0..4) and, when the object is
   updated at T+1, five more then (burst 2, ordinals 5..9), particle k drawing its start from box1
   and its end from box2 with draws 0..2 and 3..5 of tagpu_prand(seed, k, d) -- all six whatever
   its steps, and a particle of 0 steps is not made. A particle moves by step = (end - start) /
   steps per axis (idiv, toward zero) at each update of its object, its colour's low nibble
   cycling 1..7 from 1 + its place in the burst, and is removed by the update of a GameTime past
   born + steps (0x473B30: `GameTime > end`, signed).

   A SPRITE EVENT (fire, flare) is what the class's init leaves in the object (tagpu_patches.c
   computes it, the flare's step count by the init's own x87 sequence):
     from    the init's first point, 16.16, where every particle of the object starts;
     step    the per-update step: fire's (d * floor(2^32 / (life << 16))) >> 16 per axis
             (0x47432C..0x47438B), the flare's d / steps (0x473788..0x4737A8), d = to - from;
     period  fire's frame period (0x4742D8), 0 for the flare;
     nsub    the particles the object makes before it is destroyed, 0..10;
     nseq1   the flare sequence's frame count - 1 (0x4B7F60 - 1), the frame's modulus;
     life    fire: the init's life, every particle's end being T + life (0x4742CB, 0x47454A);
             the flare: its step count, the int16 high word of (len << 16) / 0x50000
             (0x473744..0x473788), each particle's end being its birth + that.
   Particle k of fire is spawned by the update of T + k (0x4743A0 through again 0x4730C0), the
   flare's by the update of T + 10k (0x4737C0 through 0x472E00), particle 0 by the emitter; each
   moves at every update of its object from the next one on, the emitter's particle from the
   update of T (T + 1 when LATE), and is removed by the first update past its end (0x4742A0,
   0x4736C0: `GameTime > end`, signed). Fire's frame advances every `period` moves, cycling
   0..nseq1-1 from 0 (0x474130); the flare's every move, from draw 0 of tagpu_prand(seed, k, 0)
   times nseq1 / 0x8000 (0x47395F, 0x473560).

   A WAKE EVENT (the wake and the bubbles, one class: init 0x474760(a, b, period, 1, dir)) takes
   the sprite layout's from, step, period and nsub, and three words of its own:
     from    a, the init's first point, 16.16 (0x474778..0x47478D);
     step    the low dword of (d * q) >> 16 per axis, d = b - a and q the low dword, sign-extended,
             of 2^32 / (int32)(len * 2), len the x87 length of d (0x4747CC..0x474868): half a world
             unit a tick toward b. The emitter computes it with the init's own instruction sequence;
     period  the colour's period: it steps every `period` moves (0x474580);
     nsub    the dots the object makes, 0..2: none when len * 2 is 0 in its low dword (the divide's
             fix ends the init before the spawn), the second only when the object is updated at
             T + 1;
     dir     1: the colour starts at 0x61 and rises, 0: it starts at 0x67 and falls (0x474A85);
     gone    each dot's DEATH WALK, the GameTime of the particle walk whose update removes it
             (0x474720): the first walk past its end tick, or the first whose move puts it over
             ground at or above the sea level (0x485070(&pos) >= the byte main+0x1427F). The game
             thread finds it at emission by marching the dot's straight path against the terrain,
             which does not change in play (tagpu_patches.c, THE WAKE'S DEATH), so no renderer
             needs the terrain.
   Dot k is spawned at T + k (k = 0 by the init's spawn 0x474880, k = 1 by the update of T + 1
   through again 0x473220) at a + its jitter: draws 0..2 of tagpu_prand(seed, k, d), each adding
   rand * 7 / 0x8000 - 3 to the x, altitude and y high words (0x4749D9..0x474A4B). It moves at every
   update of its object from the next one on (dot 0 from the update of T, T + 1 when LATE): pos +=
   step, counter = (counter + 1) % period, and at counter 0 the colour one step on, 0x61..0x67
   wrapping (0x474580); its end tick is its birth + 6 * period (0x4749AE..0x4749C3).

   A SMOKE EVENT (Smoke2 0x4FD618, init 0x474D50(pos, fcap, interval, countdown, life, seqsel);
   Smoke1 0x4FD638, init 0x475150(pos, interval, countdown, life)) is an object that spawns puffs
   over time, one at its emission and one every `ival` walks after (tagpu_patches.c computes the
   timing words when the walk after the emission runs, THE SMOKE'S TIMING):
     at      the init's point, 16.16 (0x474D5D..0x474D7C, 0x47515D..0x475178);
     ival    the spawn interval +0x1C, 1 or more (the spawn divides by it, 0x474E10);
     first   the walk of puff 0's first update: the first particle walk after the emission;
     again   the walk that spawns puff 1: the first walk from `first` on at or past +8, which the
             emission's spawn set to T + ival (0x474F71), as the again tests read it (Smoke2's
             0x475440 `+8 <= T` unsigned, Smoke1's 0x4750F0 likewise);
     npuff   the puffs the object makes: Smoke2's again also wants +8 <= +4 = T + life signed
             (0x475446), so puff 1 needs T + ival <= T + life and puff j >= 2 needs again + (j - 1)
             * ival <= T + life; Smoke1's has no end test (TAGPU_PEVT_ENDLESS);
     reset   the countdown's reset +0x20: the init's countdown, or 7 when it is 0 (0x474DC7);
     ncap    +0x24, the frame cap: the sequence's frame count - 1, capped by Smoke2's fcap when that
             is nonzero and smaller (0x474DA1..0x474DB8);
     seqsel  1: "smoke 2" main+0x147D3, else "smoke 1" main+0x147CF (0x474D7F..0x474DA0; Smoke1
             always "smoke 1");
     lmax    the most updates a puff can live, from reset and ncap: what the game thread's wind
             table must reach back over;
     pbase   the first puff one copy of the event draws: the frame packet's copy carries an event
             once for every ten puffs that can be alive (tagpu_pevt_copy);
     endT    Smoke2's +4, T + life.
   Puff j (its spawn ordinal, the object's count of the puffs it began, which is the keyed rule's
   particle: tagpu_prand.h) is spawned at the emission (j = 0) or by the update of again + (j-1) *
   ival, AFTER that update's moves (0x475422), so it is first moved at the walk after its spawn.
   It starts at `at` with j's low bits in its position's low bits (x bits 0..2, y bits 0..2, alt
   bits 0..1 of Smoke2 and 0..3 of Smoke1: tagpu_patches.c THE PARTICLES' OWN RANDOM RULE), its
   frame count nframes = 2 + draw 0 * (ncap - 2) / 0x8000, frame 0, countdown = reset. Each update
   adds wind_x << 3 to x, the rise << 2 (Smoke2) or << 4 (Smoke1) to alt and wind_y << 3 to y, the
   values main+0x37ECC, main+0x14263 and main+0x37ED4 hold at that walk (0x475340, 0x475600), and
   counts the countdown down; at 0 the frame advances to f and the countdown is h + draw f * h /
   0x8000, h = reset / 2 toward zero (0x4753A6..0x4753D0); the puff is removed by the update that
   leaves its frame at nframes or more (0x4753D3). So after u updates a puff is at its start plus
   the wind's sums over the walks of those updates, which the closed form takes as the difference
   of two prefix sums (TAGPU_PEVT_WIND below). Its draws are those of its own ordinal j while the
   object has spawned fewer than 256 (Smoke2) or 1024 (Smoke1) puffs since it, which the game
   thread holds every smoke event to (an emission it cannot hold keeps its engine object). */
#include <stdint.h>
#include "tagpu_prand.h"

#define TAGPU_PEVT_LAYER(f)  ((f) & 0xFu)
/* Emitted after its GameTime's particle walk (a replay's frame visit re-fires the emitter
   between two steps): burst 1's first update is then at T+1, not T. */
#define TAGPU_PEVT_LATE      0x10u
/* The object is updated at T+1, so its second burst exists (tagpu_pevt_nano_finish). */
#define TAGPU_PEVT_BURST2    0x20u
#define TAGPU_PEVT_CLASS(f)  (((f) >> 8) & 0xFu)
/* The event's object is the engine's, which the layer's cap trims itself: a proxy (or, under the
   oracle, the real object) of a class sharing its layer with the engine's objects. An event
   without it stands for an object nobody made, and the emitter's cap is applied to its events. */
#define TAGPU_PEVT_PROXY     0x40u
/* A smoke event whose timing words wait for the next particle walk (tagpu_patches.c) */
#define TAGPU_PEVT_PEND      0x80u
enum { TAGPU_PEVT_C_NANO = 0, TAGPU_PEVT_C_FIRE = 1, TAGPU_PEVT_C_FLARE = 2,
       TAGPU_PEVT_C_WAKE = 3, TAGPU_PEVT_C_SMOKE2 = 4, TAGPU_PEVT_C_SMOKE1 = 5 };
#define TAGPU_PEVT_NCLASS    6u
#define TAGPU_PEVT_SMOKE(c)  ((c) == TAGPU_PEVT_C_SMOKE2 || (c) == TAGPU_PEVT_C_SMOKE1)
/* a Smoke1 object's puff count: it re-puffs for the rest of the level */
#define TAGPU_PEVT_ENDLESS   0x7FFFFFFF

#define TAGPU_PEVT_NPART     10u
#define TAGPU_PEVT_WORDS     20u     /* the GLSL reads the record as 20 uints */

/* 80 B, the frame packet's record and the game thread's alike; the GLSL reads a nano event's
   boxes at words 3..14 and its steps at 15..19, a sprite event's fields at words 3..12, a wake
   event's at 3..10 and 13..15 */
typedef struct TAGPU_PEVT {
    uint32_t seed;
    uint32_t t0;
    uint32_t flags;
    union {
        struct {                        /* TAGPU_PEVT_C_NANO                  */
            int32_t  box1[6];
            int32_t  box2[6];
            int16_t  steps[TAGPU_PEVT_NPART];
        };
        struct {                        /* TAGPU_PEVT_C_FIRE, _C_FLARE, _C_WAKE */
            int32_t  from[3];
            int32_t  step[3];
            int32_t  period;
            int32_t  nsub;
            int32_t  nseq1;             /* fire, flare                        */
            int32_t  life;              /* fire, flare                        */
            int32_t  dir;               /* wake                               */
            uint32_t gone[2];           /* wake                               */
        };
        struct {                        /* TAGPU_PEVT_C_SMOKE2, _C_SMOKE1      */
            int32_t  at[3];
            int32_t  ival;
            uint32_t first, again;
            int32_t  npuff, reset, ncap, seqsel, lmax;
            uint32_t pbase, endT;
        };
    };
} TAGPU_PEVT;

typedef char tagpu_pevt_size[(sizeof(TAGPU_PEVT) == TAGPU_PEVT_WORDS * 4u) ? 1 : -1];

/* THE SPRITES' FRAMES: sequence s's frame i, s = 0 the flare sequence *(main+0x147F3) fire and
   the flare draw from, 1 "smoke 1" *(main+0x147CF), 2 "smoke 2" *(main+0x147D3). The render thread
   resolves them into the atlas once a frame and hands the GPU this table ahead of the events, 8
   words a frame, row s * TAGPU_PEVT_NFRAME + i. w == 0: the frame is not drawn this frame (beyond
   the sequence, not painted yet, a compound frame, the alpha blit not built). */
#define TAGPU_PEVT_NFRAME    64u
#define TAGPU_PEVT_NSEQ      3u
/* THE WIND'S PREFIX SUMS, per particle walk: row t holds the sums of wind_x, the rise and wind_y
   over the walks up to and including t, wrapping int32, from a base the game thread picks; a puff
   first moved at walk F has moved, after the walk of P, by row P - row (F - 1). The game thread
   keeps the last TAGPU_PEVT_WIND walks; a smoke event whose puffs could outlive that is not made.
   The frame packet carries the rows its smoke events reach back to, the GPU table after the
   frames: a header {base tick, rows} and then 4 words a row (x, rise, y, 0). */
#define TAGPU_PEVT_WIND      1024u
#define TAGPU_PEVT_WIND_AT   (TAGPU_PEVT_NSEQ * TAGPU_PEVT_NFRAME * 8u)
#define TAGPU_PEVT_EVT_AT    (TAGPU_PEVT_WIND_AT + 4u + 4u * TAGPU_PEVT_WIND)
typedef struct TAGPU_PEVT_FRAME {
    int32_t ox, oy;                     /* -hotx, -hoty: the quad's corner from the point   */
    int32_t w, h;
    float   u0, v0, u1, v1;
} TAGPU_PEVT_FRAME;
typedef char tagpu_pevt_frame_size[(sizeof(TAGPU_PEVT_FRAME) == 32u) ? 1 : -1];

/* base + r * extent / 0x8000: the 64-bit imul and _alldiv 0x4E4440, toward zero, then a 32-bit
   add (0x473DB2..0x473DC3) */
static inline int32_t tagpu_pevt_coord(int32_t base, int32_t ext, int r)
{
    return (int32_t)((uint32_t)base + (uint32_t)(int32_t)(((int64_t)r * ext) / 0x8000));
}

/* particle k's start s[3] and end t[3], x / altitude / y */
static inline void tagpu_pevt_ends(const TAGPU_PEVT* e, unsigned k, int32_t s[3], int32_t t[3])
{
    unsigned a;
    for (a = 0; a < 3; a++) {
        s[a] = tagpu_pevt_coord(e->box1[a], e->box1[3 + a], tagpu_prand(e->seed, k, a));
        t[a] = tagpu_pevt_coord(e->box2[a], e->box2[3 + a], tagpu_prand(e->seed, k, 3 + a));
    }
}

/* one sub-particle record of the engine's nano object (stride 0x30): pos @0, end @0xC, step
   @0x18, 0x100 @0x24, colour @0x28, end tick @0x2C */
typedef struct TAGPU_PEVT_NANO {
    int32_t  pos[3], end[3], step[3];
    uint32_t colour, endTick;
} TAGPU_PEVT_NANO;

/* Particle k as the engine's object holds it after the particle walk of GameTime P (P >= T):
   1 and *o when it is in the object's sub-vector then, 0 when it is not (not made, not yet
   born, or removed). */
static inline int tagpu_pevt_nano_at(const TAGPU_PEVT* e, unsigned k, uint32_t P,
                                     TAGPU_PEVT_NANO* o)
{
    int32_t s = e->steps[k], st[3], en[3], u;
    uint32_t born, first;
    unsigned a;
    if (!s) return 0;
    if (k < 5u) {
        born = e->t0;
        first = (e->flags & TAGPU_PEVT_LATE) ? e->t0 + 1u : e->t0;
    } else {
        if (!(e->flags & TAGPU_PEVT_BURST2)) return 0;
        born = e->t0 + 1u;              /* spawned by the update of T+1, after its moves */
        first = e->t0 + 2u;
    }
    if ((int32_t)(P - born) < 0) return 0;
    if ((int32_t)(P - first) >= 0 && (int32_t)(P - (born + (uint32_t)s)) > 0) return 0;
    u = (int32_t)(P - first) + 1;       /* the updates it has had */
    if (u < 0) u = 0;
    tagpu_pevt_ends(e, k, st, en);
    for (a = 0; a < 3; a++) {
        int32_t d = (int32_t)((uint32_t)en[a] - (uint32_t)st[a]);
        o->step[a] = (int32_t)((int64_t)d / s);
        o->pos[a] = (int32_t)((uint32_t)st[a] + (uint32_t)u * (uint32_t)o->step[a]);
        o->end[a] = en[a];
    }
    o->colour = 0xA0u | ((k % 5u + (uint32_t)u) % 7u + 1u);
    o->endTick = born + (uint32_t)s;
    return 1;
}

/* Sets TAGPU_PEVT_BURST2 and answers D, the GameTime whose particle walk destroys the object:
   its slot 3 0x472FD0 finds the sub-vector empty at the start of that walk. Until then the
   object is in its layer, which is what the layer's cap counts (0x47219F). A walk at Q sees the
   state the walk of Q-1 left, so an object whose last particle is in the state after the walk of
   L is destroyed at L + 2; one that made no particle at the first walk after its call. */
static inline uint32_t tagpu_pevt_nano_finish(TAGPU_PEVT* e)
{
    const int late = (e->flags & TAGPU_PEVT_LATE) != 0;
    const uint32_t T = e->t0, first = late ? T + 1u : T;
    int32_t last = -1;                  /* the last post-walk state holding a particle, - T */
    int made = 0, k;
    e->flags &= ~TAGPU_PEVT_BURST2;
    for (k = 0; k < 5; k++) {
        int32_t s = e->steps[k];
        if (!s) continue;
        made = 1;
        if (late && s < 0) s = 0;       /* in the state at T, which no walk has tested */
        if (s > last) last = s;
    }
    if (!made) return first;
    /* updated at T+1: always when late; otherwise when the walk of T left a particle */
    if (late || last >= 0) {
        e->flags |= TAGPU_PEVT_BURST2;
        for (k = 5; k < 10; k++) {
            int32_t s = e->steps[k];
            if (!s) continue;
            if (1 + (s < 0 ? 0 : s) > last) last = 1 + (s < 0 ? 0 : s);
        }
    }
    if (last < 0) return first + 1u;
    return T + (uint32_t)last + 2u > first + 1u ? T + (uint32_t)last + 2u : first + 1u;
}

/* one sub-particle record of the engine's fire object (stride 0x3C: pos @4, step @0x1C, nseq1
   @0x28, frame @0x2C, counter @0x30, period @0x34, end tick @0x38) or flare object (stride 0x34:
   pos @4, step @0x1C, nseq1 @0x28, frame @0x2C, end tick @0x30) */
typedef struct TAGPU_PEVT_SPRITE {
    int32_t  pos[3], step[3];
    int32_t  nseq1, frame, counter, period;
    uint32_t endTick;
} TAGPU_PEVT_SPRITE;

/* the flare's particle k's first frame: rand * nseq1 / 0x8000, the 64-bit imul and _alldiv
   0x4E4440 (0x473964..0x473971), toward zero */
static inline int32_t tagpu_pevt_flare_frame0(uint32_t seed, unsigned k, int32_t nseq1)
{
    return (int32_t)(((int64_t)tagpu_prand(seed, k, 0) * nseq1) / 0x8000);
}

/* Particle k of a sprite event as the engine's object holds it after the particle walk of
   GameTime P (P >= T): 1 and *o when it is in the object's sub-vector then, 0 when it is not
   (not made, not yet born, or removed). The update moves a particle, then removes it when
   GameTime > its end (0x473030..0x473043, 0x472D70..0x472D83). */
static inline int tagpu_pevt_sprite_at(const TAGPU_PEVT* e, unsigned k, uint32_t P,
                                       TAGPU_PEVT_SPRITE* o)
{
    const int fire = TAGPU_PEVT_CLASS(e->flags) == TAGPU_PEVT_C_FIRE;
    uint32_t born, endT, first;
    int32_t u;
    unsigned a;
    if ((int32_t)k >= e->nsub) return 0;
    born = e->t0 + k * (fire ? 1u : 10u);
    endT = fire ? e->t0 + (uint32_t)e->life : born + (uint32_t)e->life;
    if ((int32_t)(P - born) < 0 || (int32_t)(P - endT) > 0) return 0;
    first = k ? born + 1u : (e->flags & TAGPU_PEVT_LATE) ? e->t0 + 1u : e->t0;
    u = (int32_t)(P - first) + 1;       /* the moves it has had */
    if (u < 0) u = 0;
    for (a = 0; a < 3; a++) {
        o->step[a] = e->step[a];
        o->pos[a] = (int32_t)((uint32_t)e->from[a] + (uint32_t)u * (uint32_t)e->step[a]);
    }
    o->nseq1 = e->nseq1;
    o->endTick = endT;
    if (fire) {
        o->period = e->period;
        o->counter = u % e->period;
        o->frame = (u / e->period) % e->nseq1;
    } else {
        o->period = 0;
        o->counter = 0;
        o->frame = (tagpu_pevt_flare_frame0(e->seed, k, e->nseq1) + u) % e->nseq1;
    }
    return 1;
}

/* D, the GameTime whose particle walk destroys a sprite event's object: the walk after the one
   that removed its last particle (slot 3, 0x473130 / 0x472E70, finds the sub-vector empty), or
   the first walk after its call when it made none. Fire's particles all end at T + life, so the
   walk of T + life + 1 removes the last; the flare's last, particle nsub - 1, at its birth plus
   its step count plus 1. */
static inline uint32_t tagpu_pevt_sprite_finish(const TAGPU_PEVT* e)
{
    const uint32_t first = (e->flags & TAGPU_PEVT_LATE) ? e->t0 + 1u : e->t0;
    if (e->nsub < 1) return first;
    if (TAGPU_PEVT_CLASS(e->flags) == TAGPU_PEVT_C_FIRE) return e->t0 + (uint32_t)e->life + 2u;
    return e->t0 + 10u * (uint32_t)(e->nsub - 1) + (uint32_t)e->life + 2u;
}

/* the wake's colour range (0x474A8C, 0x474A91) and the most moves its period lets a dot have
   before its end tick: the game thread marches at most that many terrain lookups a dot, and an
   emission of a longer period keeps its engine object or is refused (tagpu_patches.c) */
#define TAGPU_PEVT_WAKE_LO     0x61
#define TAGPU_PEVT_WAKE_HI     0x67
#define TAGPU_PEVT_WAKE_PERIOD 1024

/* one sub-particle record of the engine's wake object (stride 0x44): pos @4, b @0x10, step @0x1C,
   colour min @0x28 and max @0x2C, colour @0x30, its step @0x34, counter @0x38, period @0x3C,
   end tick @0x40 */
typedef struct TAGPU_PEVT_WAKE {
    int32_t  pos[3], step[3];
    int32_t  colour, delta, counter, period;
    uint32_t endTick;
} TAGPU_PEVT_WAKE;

/* dot k's start: a with draws 0..2 of tagpu_prand(seed, k, d) added to the high words, each
   rand * 7 / 0x8000 - 3 (the 64-bit imul and _alldiv 0x4E4440, toward zero; then a 16-bit add to
   the high word, which is the 32-bit add of the value shifted 16) */
static inline void tagpu_pevt_wake_start(const TAGPU_PEVT* e, unsigned k, int32_t s[3])
{
    unsigned a;
    for (a = 0; a < 3; a++) {
        int32_t j = (int32_t)(((int64_t)tagpu_prand(e->seed, k, a) * 7) / 0x8000) - 3;
        s[a] = (int32_t)((uint32_t)e->from[a] + ((uint32_t)j << 16));
    }
}

/* dot k's birth and the walk of its first move */
static inline uint32_t tagpu_pevt_wake_born(const TAGPU_PEVT* e, unsigned k)
{
    return e->t0 + k;
}
static inline uint32_t tagpu_pevt_wake_first(const TAGPU_PEVT* e, unsigned k)
{
    return k ? e->t0 + 2u : (e->flags & TAGPU_PEVT_LATE) ? e->t0 + 1u : e->t0;
}

/* dot k after u moves (u >= 0): its position, colour, counter and end tick */
static inline void tagpu_pevt_wake_after(const TAGPU_PEVT* e, unsigned k, int32_t u,
                                         TAGPU_PEVT_WAKE* o)
{
    int32_t s[3], c;
    unsigned a;
    tagpu_pevt_wake_start(e, k, s);
    for (a = 0; a < 3; a++) {
        o->step[a] = e->step[a];
        o->pos[a] = (int32_t)((uint32_t)s[a] + (uint32_t)u * (uint32_t)e->step[a]);
    }
    c = (u / e->period) % 7;
    o->colour = e->dir ? TAGPU_PEVT_WAKE_LO + c : TAGPU_PEVT_WAKE_HI - c;
    o->delta = e->dir ? 1 : -1;
    o->counter = u % e->period;
    o->period = e->period;
    o->endTick = tagpu_pevt_wake_born(e, k) + 6u * (uint32_t)e->period;
}

/* Dot k as the engine's object holds it after the particle walk of GameTime P (P >= T): 1 and *o
   when it is in the object's sub-vector then -- born, and not yet removed by its death walk --
   0 when it is not. */
static inline int tagpu_pevt_wake_at(const TAGPU_PEVT* e, unsigned k, uint32_t P,
                                     TAGPU_PEVT_WAKE* o)
{
    int32_t u;
    if ((int32_t)k >= e->nsub || k > 1u) return 0;
    if ((int32_t)(P - tagpu_pevt_wake_born(e, k)) < 0 || (int32_t)(P - e->gone[k]) >= 0) return 0;
    u = (int32_t)(P - tagpu_pevt_wake_first(e, k)) + 1;
    tagpu_pevt_wake_after(e, k, u < 0 ? 0 : u, o);
    return 1;
}

/* D, the walk that destroys a wake event's object: the walk after its last dot's death walk
   (slot 3 0x473290 finds the sub-vector empty), or the first walk after its call when it made
   none. */
static inline uint32_t tagpu_pevt_wake_finish(const TAGPU_PEVT* e)
{
    uint32_t D = e->gone[0];
    if (e->nsub < 1) return (e->flags & TAGPU_PEVT_LATE) ? e->t0 + 1u : e->t0;
    if (e->nsub > 1 && (int32_t)(e->gone[1] - D) > 0) D = e->gone[1];
    return D + 1u;
}

/* the frame table holds TAGPU_PEVT_NFRAME frames of a sequence: a puff's frame stays under ncap
   - 1 (nframes <= ncap - 1 for ncap >= 3), so an event's ncap is at most this */
#define TAGPU_PEVT_SMOKE_NCAP  ((int32_t)TAGPU_PEVT_NFRAME)

/* one puff record of the engine's smoke object (stride 0x20): seq* @0, pos @4, nframes @0x10,
   frame @0x14, reset @0x18, countdown @0x1C */
typedef struct TAGPU_PEVT_PUFF {
    int32_t pos[3];
    int32_t nframes, frame, reset, countdown;
} TAGPU_PEVT_PUFF;

/* puff j's frame count: 2 + draw 0 * (ncap - 2) / 0x8000, the 64-bit imul and _alldiv toward
   zero (0x474F1C..0x474F3A, 0x4752D4..0x4752EF) */
static inline int32_t tagpu_pevt_puff_nframes(const TAGPU_PEVT* e, uint32_t j)
{
    return tagpu_pevt_coord(2, e->ncap - 2, tagpu_prand(e->seed, j, 0));
}

/* Puff j after u updates (u >= 0): 1 and its frame and countdown while it is in its object, 0
   once an update has removed it. Frame f lasts from the update `start` for `len` updates: frame 0
   the reset, frame f h + draw f * h / 0x8000 (the countdown set at the advance to f); the update
   that advances it to nframes removes it, and with nframes 0 or less the first update does. At
   most nframes frames, each a draw: bounded by ncap. */
static inline int tagpu_pevt_puff_frame(const TAGPU_PEVT* e, uint32_t j, int32_t u,
                                        int32_t* frame, int32_t* count)
{
    const int32_t nf = tagpu_pevt_puff_nframes(e, j), h = e->reset / 2;
    int32_t f = 0, start = 0, len = e->reset;
    if (u > 0 && nf <= 0) return 0;
    while (u >= start + len) {
        if (++f >= nf) return 0;
        start += len;
        len = tagpu_pevt_coord(h, h, tagpu_prand(e->seed, j, (unsigned)f));
    }
    *frame = f;
    *count = len - (u - start);
    return 1;
}

/* the update that removes puff j: its count of updates in the object, 1 or more */
static inline int32_t tagpu_pevt_puff_life(const TAGPU_PEVT* e, uint32_t j)
{
    const int32_t nf = tagpu_pevt_puff_nframes(e, j), h = e->reset / 2;
    int32_t f = 0, start = 0, len = e->reset;
    if (nf <= 0) return 1;
    while (++f < nf) {
        start += len;
        len = tagpu_pevt_coord(h, h, tagpu_prand(e->seed, j, (unsigned)f));
    }
    return start + len;
}

/* the puffs spawned by the walk of P: puff 0 and those of the walks again, again + ival, ... up
   to P, at most npuff */
static inline uint32_t tagpu_pevt_puffs(const TAGPU_PEVT* e, uint32_t P)
{
    const int32_t d = (int32_t)(P - e->again);
    uint32_t n = 1u;
    if (e->npuff > 1 && d >= 0) n += (uint32_t)(d / e->ival) + 1u;
    return n < (uint32_t)e->npuff ? n : (uint32_t)e->npuff;
}

/* the walk of puff j's first update */
static inline uint32_t tagpu_pevt_puff_first(const TAGPU_PEVT* e, uint32_t j)
{
    return j ? e->again + (j - 1u) * (uint32_t)e->ival + 1u : e->first;
}

/* the updates puff j has had after the walk of P, or -1 when it is not spawned by then */
static inline int32_t tagpu_pevt_puff_u(const TAGPU_PEVT* e, uint32_t j, uint32_t P)
{
    int32_t u;
    if (j >= tagpu_pevt_puffs(e, P)) return -1;
    u = (int32_t)(P - tagpu_pevt_puff_first(e, j)) + 1;
    return u < 0 ? 0 : u;
}

/* Puff j after u updates, `d` the wind's sums over the walks of those updates (x, rise, y: the
   prefix sums' difference, row F + u - 1 less row F - 1): 1 and *o while it is in its object. Its
   start carries j's low bits (the alt bits are the rise's shift: 2 Smoke2, 4 Smoke1). */
static inline int tagpu_pevt_puff_after(const TAGPU_PEVT* e, uint32_t j, int32_t u,
                                        const int32_t* d, TAGPU_PEVT_PUFF* o)
{
    const unsigned rise = TAGPU_PEVT_CLASS(e->flags) == TAGPU_PEVT_C_SMOKE1 ? 4u : 2u;
    const uint32_t am = (1u << rise) - 1u;
    if (!tagpu_pevt_puff_frame(e, j, u, &o->frame, &o->countdown)) return 0;
    o->pos[0] = (int32_t)((((uint32_t)e->at[0] & ~7u) | (j & 7u)) + ((uint32_t)d[0] << 3));
    o->pos[1] = (int32_t)((((uint32_t)e->at[1] & ~am) | (j >> 6 & am)) + ((uint32_t)d[1] << rise));
    o->pos[2] = (int32_t)((((uint32_t)e->at[2] & ~7u) | (j >> 3 & 7u)) + ((uint32_t)d[2] << 3));
    o->nframes = tagpu_pevt_puff_nframes(e, j);
    o->reset = e->reset;
    return 1;
}

/* D, the walk that destroys a Smoke2 event's object: slot 3 0x474F80 finds its puffs all gone
   (puff j's last update is the walk first(j) + life(j) - 1) AND +4 = endT before the walk
   (`cmp +4, T; jae`), so the later of the walk after the last puff's removal and endT + 1 */
static inline uint32_t tagpu_pevt_smoke_finish(const TAGPU_PEVT* e)
{
    uint32_t D = e->endT + 1u, j;
    for (j = 0; j < (uint32_t)e->npuff; j++) {
        const uint32_t X = tagpu_pevt_puff_first(e, j) + (uint32_t)tagpu_pevt_puff_life(e, j);
        if ((int32_t)(X - D) > 0) D = X;
    }
    return D;
}

/* the most updates a puff of these words can live: nframes - 1 countdowns of the most a draw
   gives, after the reset */
static inline int32_t tagpu_pevt_smoke_lmax(const TAGPU_PEVT* e)
{
    const int32_t h = e->reset / 2, c = tagpu_pevt_coord(h, h, 0x7FFF);
    int32_t nf = tagpu_pevt_coord(2, e->ncap - 2, 0x7FFF), n0 = tagpu_pevt_coord(2, e->ncap - 2, 0);
    if (n0 > nf) nf = n0;
    return nf <= 0 ? 1 : e->reset + (nf - 1) * c;
}

#endif
