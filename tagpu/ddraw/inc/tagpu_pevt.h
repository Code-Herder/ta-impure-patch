#ifndef TAGPU_PEVT_H
#define TAGPU_PEVT_H
/* tagpu_pevt -- a particle EMITTER EVENT, and the closed form a renderer draws it by
   (gpu-particles-plan.md, *The emitter event*; effects.md §7.8).

   On a Vulkan launch the nano emitters 0x4720D0 and 0x472200 make no engine object: the game
   thread records one event per call instead (tagpu_patches.c, *The nano spray as events*), the
   frame packet carries the live ones, and the GPU computes every particle from its event and the
   packet's GameTime (tagpu_fx.c NVS). The engine's object is a pure function of the event, so
   this header is that function, written once: the oracle (tagpu_particlecheck.on) holds the
   engine's objects to it every tick, and NVS is its GLSL edition, statement for statement --
   EDIT BOTH OR NEITHER.

   THE EVENT is what the emitter's init 0x473B50 and spawn 0x473D50 take from the call:
     seed    the emission's seed, tagpu_prand_seed(level, T, n) (tagpu_prand.h);
     t0      T, the GameTime of the call;
     flags   bits 0..3 the layer, TAGPU_PEVT_LATE, TAGPU_PEVT_BURST2;
     box1/2  the two boxes as the init cuts them: per axis {base x, alt, y, extent x, alt, y},
             16.16, base = min + 4d/11 and extent = 7d/11 - 4d/11, d = max - min, each division
             toward zero on the 32-bit products (0x473B80..0x473D34);
     steps   each particle's step count, the int16 high word of (len << 16) / 0x40000, len the
             x87 length of its path (0x473E75..0x473ED9). The emitter computes it on the game
             thread with the spawn's own instruction sequence, because that is a float square root
             under the thread's x87 control word, which no other processor reproduces bit for
             bit; everything else here is integer arithmetic.

   THE OBJECT IT STANDS FOR. Five particles at T (burst 1, ordinals 0..4) and, when the object is
   updated at T+1, five more then (burst 2, ordinals 5..9), particle k drawing its start from box1
   and its end from box2 with draws 0..2 and 3..5 of tagpu_prand(seed, k, d) -- all six whatever
   its steps, and a particle of 0 steps is not made. A particle moves by step = (end - start) /
   steps per axis (idiv, toward zero) at each update of its object, its colour's low nibble
   cycling 1..7 from 1 + its place in the burst, and is removed by the update of a GameTime past
   born + steps (0x473B30: `GameTime > end`, signed). */
#include <stdint.h>
#include "tagpu_prand.h"

#define TAGPU_PEVT_LAYER(f)  ((f) & 0xFu)
/* Emitted after its GameTime's particle walk (a replay's frame visit re-fires the emitter
   between two steps): burst 1's first update is then at T+1, not T. */
#define TAGPU_PEVT_LATE      0x10u
/* The object is updated at T+1, so its second burst exists (tagpu_pevt_nano_finish). */
#define TAGPU_PEVT_BURST2    0x20u

#define TAGPU_PEVT_NPART     10u
#define TAGPU_PEVT_WORDS     20u     /* the GLSL reads the record as 20 uints */

/* 80 B, the frame packet's record and the game thread's alike */
typedef struct TAGPU_PEVT {
    uint32_t seed;
    uint32_t t0;
    uint32_t flags;
    int32_t  box1[6];
    int32_t  box2[6];
    int16_t  steps[TAGPU_PEVT_NPART];
} TAGPU_PEVT;

typedef char tagpu_pevt_size[(sizeof(TAGPU_PEVT) == TAGPU_PEVT_WORDS * 4u) ? 1 : -1];

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

#endif
