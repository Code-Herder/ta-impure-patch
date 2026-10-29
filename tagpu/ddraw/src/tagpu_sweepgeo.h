#ifndef TAGPU_SWEEPGEO_H
#define TAGPU_SWEEPGEO_H
/* tagpu_sweepgeo.c -- the reclaim sweep's geometry and its split, engine-free: which targets a
   ring holds, the order they are taken in, and how they are shared between the selected
   builders. C99 and libc only, so tests/sweeptest.py builds it natively and checks it against a
   port of the owner's mockup (orderSweep). Any thread; nothing here keeps state.

   Units: world pixels for positions, ticks for times. A builder's speed is its def's
   maxvelocity (+0x192, 16.16) as world px a tick and its reach its builddistance (+0x212), both
   [INFERRED] units; the model only balances the split, and no correctness rests on it. */
#include <stdint.h>

#define SWEEP_MAX_TARGETS 1024u

typedef struct { int32_t x, z; uint32_t ticks; } SweepTarget;
typedef struct { int32_t x, z; double speed, reach, load; uint32_t room; } SweepBuilder;

/* (x, z) lies inside the ring of radius r round (cx, cz), its edge included: 64-bit exact for
   every int32 input. 0 for r < 0. */
int      sweep_inside(int32_t cx, int32_t cz, int32_t r, int32_t x, int32_t z);

/* idx[0..n) = the targets' indices nearest (cx, cz) first; ties by z, then x, then index. n is
   at most SWEEP_MAX_TARGETS; more are not ordered. */
void     sweep_order(const SweepTarget* t, unsigned n, int32_t cx, int32_t cz, uint16_t* idx);

/* The ticks a Reclaim order works on a feature of `energy` and `metal`: 2 ticks a working call,
   ceil(C0 / 2) calls for the countdown C0 = trunc(15 + (energy + metal) / 2). A negative or NaN
   amount counts as 0. */
uint32_t sweep_reclaim_ticks(float energy, float metal);

/* The split, deterministic: until no target is left or no builder can take one, the builder
   with the least `load` that has `room` and can reach a remaining target (ties: lowest index)
   takes the remaining target nearest its current point (64-bit squared distance; ties: lowest
   index). Its load grows by travel + ticks, travel = max(0, d - reach) / speed; a builder with
   speed <= 0 reaches only targets within `reach` of its point and travels 0. Its point moves to
   the target and its room drops by one. who[k] / seq[k] are the builder and the target of the
   k-th assignment; `b` is updated in place. nt is at most SWEEP_MAX_TARGETS and nb at most
   65535; the rest are not considered. Returns the assignments made. */
unsigned sweep_split(SweepBuilder* b, unsigned nb, const SweepTarget* t, unsigned nt,
                     uint16_t* who, uint16_t* seq);

#endif
