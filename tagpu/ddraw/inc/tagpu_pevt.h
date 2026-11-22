#ifndef TAGPU_PEVT_H
#define TAGPU_PEVT_H
/* tagpu_pevt -- a particle EMITTER EVENT, and the closed form a renderer draws it by
   (gpu-particles-plan.md, *The emitter event*; effects.md §7.8).

   On a Vulkan launch three particle classes are drawn from events: the nano spray (emitters
   0x4720D0, 0x472200), fire (0x472330, 0x472AB0) and the teleport flare (0x471FD0). The game
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
   times nseq1 / 0x8000 (0x47395F, 0x473560). */
#include <stdint.h>
#include "tagpu_prand.h"

#define TAGPU_PEVT_LAYER(f)  ((f) & 0xFu)
/* Emitted after its GameTime's particle walk (a replay's frame visit re-fires the emitter
   between two steps): burst 1's first update is then at T+1, not T. */
#define TAGPU_PEVT_LATE      0x10u
/* The object is updated at T+1, so its second burst exists (tagpu_pevt_nano_finish). */
#define TAGPU_PEVT_BURST2    0x20u
#define TAGPU_PEVT_CLASS(f)  (((f) >> 8) & 0xFu)
enum { TAGPU_PEVT_C_NANO = 0, TAGPU_PEVT_C_FIRE = 1, TAGPU_PEVT_C_FLARE = 2 };

#define TAGPU_PEVT_NPART     10u
#define TAGPU_PEVT_WORDS     20u     /* the GLSL reads the record as 20 uints */

/* 80 B, the frame packet's record and the game thread's alike; the GLSL reads a nano event's
   boxes at words 3..14 and its steps at 15..19, a sprite event's fields at words 3..12 */
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
        struct {                        /* TAGPU_PEVT_C_FIRE, _C_FLARE        */
            int32_t  from[3];
            int32_t  step[3];
            int32_t  period;
            int32_t  nsub;
            int32_t  nseq1;
            int32_t  life;
        };
    };
} TAGPU_PEVT;

typedef char tagpu_pevt_size[(sizeof(TAGPU_PEVT) == TAGPU_PEVT_WORDS * 4u) ? 1 : -1];

/* THE SPRITE'S FRAMES, the flare sequence *(main+0x147F3) both sprite classes draw from: the
   render thread resolves frame i of it into the atlas once a frame and hands the GPU this table
   ahead of the events, 8 words a frame. w == 0: the frame is not drawn this frame (beyond the
   sequence, not painted yet, a compound frame, the alpha blit not built). */
#define TAGPU_PEVT_NFRAME    64u
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

#endif
