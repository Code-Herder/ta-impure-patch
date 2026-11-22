#ifndef TAGPU_SFX_H
#define TAGPU_SFX_H
/* The particle pass (smoke, fire, the teleport flare, wakes and bubbles, the nanolathe spray):
   the frame packet's particle events handed to the effects pass, which draws every particle on
   the GPU from its event (tagpu_fx.c NVS). Armed by tagpu_sfx.on. See research/notes/effects.md
   §7. */
#include "tagpu_fx.h"

int  tagpu_sfx_armed(unsigned frame_counter);   /* re-reads tagpu_sfx.on (30f) */
int  tagpu_sfx_on(void);                        /* armed state, no re-read     */
/* the packet's events to the effects pass, once a frame (none when passive) */
void tagpu_sfx_gather(const TAGPU_FXVIEW* v);
void tagpu_sfx_frame_done(const TAGPU_FXVIEW* v);   /* per-frame log + counters */
#endif
