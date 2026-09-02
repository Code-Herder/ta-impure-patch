#ifndef TAGPU_SFX_H
#define TAGPU_SFX_H
/* Particle sfx pass (smoke, fire, wakes, nanolathe spray) — the ten
   "layer" vectors at *(main+0x38D77) the engine draws through
   0x471F90(ctx, n) at ten DrawGameScreen sites. Gathered here, emitted
   through tagpu_fx.c's sprite/dot buckets at the layer's depth key. Armed by
   tagpu_sfx.on. See research/notes/effects.md §7. */
#include "tagpu_fx.h"

int  tagpu_sfx_armed(unsigned frame_counter);   /* re-reads tagpu_sfx.on (30f) */
int  tagpu_sfx_on(void);                        /* armed state, no re-read     */
/* walk layers [from, to] and emit (nothing when passive: count + log only) */
void tagpu_sfx_gather(const TAGPU_FXVIEW* v, int from, int to);
void tagpu_sfx_frame_done(const TAGPU_FXVIEW* v);   /* per-frame log + counters */
#endif
