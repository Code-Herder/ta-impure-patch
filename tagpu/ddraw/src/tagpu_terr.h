#ifndef TAGPU_TERR_H
#define TAGPU_TERR_H
/* Terrain pass (G13b) — the 32x32 pre-rendered map tiles, the last layer the
   engine still paints inside the viewport.

   The engine's pass is 0x483FA0(OFFSCREEN* ctx): a flat grid blit of 8bpp
   tiles indexed by a u16 tile map, with no height, no LOS and no depth
   (research/notes/terrain-depth.md 2). This module reproduces it from the same
   two stores, into the native pass's FBO, at a depth key BELOW every other
   band — terrain is the frame's implicit far plane.

   Because it covers the whole viewport it also inherits the fog overlay's
   solid black: where the engine would paint an unexplored cell black, terrain
   PAINTS black instead of discarding, since nothing is behind it any more.

   Armed by tagpu_terr.on (tokens: log, passive, over, key=N). `passive` emits
   nothing; `over` draws ours on top of the engine's own terrain without owning
   the draw (the pixel-parity A/B); the default owns it via tagpu_terrown.c. */
#include "tagpu_fx.h"

int  tagpu_terr_armed(unsigned frame_counter);   /* re-reads tagpu_terr.on (30f) */
int  tagpu_terr_on(void);
/* build this frame's quads; returns the vertex count (0 = nothing to draw) */
int  tagpu_terr_gather(const TAGPU_FXVIEW* v);
/* draw into the currently bound FBO (depth test on, depth writes on). Own
   program/VAO; leaves program, VAO and texture bindings dirty. */
void tagpu_terr_render(const TAGPU_FXVIEW* v, unsigned int palTex);
void tagpu_terr_glreset(void);

/* the palette index tagpu_terrown.c fills the viewport with in place of the
   engine's terrain blit; the composite treats every OTHER index in the engine's
   frame as "an overlay the engine still draws, and we must not cover it" */
int  tagpu_terr_key(void);
#endif
