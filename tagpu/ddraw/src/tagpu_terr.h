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
/* build this frame's quads; returns the CELL count (0 = nothing to draw) —
   one instanced quad each, see the vertex shader in tagpu_terr.c */
int  tagpu_terr_gather(const TAGPU_FXVIEW* v);
/* draw into the currently bound FBO (depth test on, depth writes on). Own
   program/VAO; leaves program, VAO and texture bindings dirty. */
void tagpu_terr_render(const TAGPU_FXVIEW* v, unsigned int palTex);
void tagpu_terr_glreset(void);

/* Classic++ shadows (tagpu_shadow.c, renderers.md 2.8): the heightfield as a
   caster. One world-space vertex per 16-px grid point of the height grid,
   built with it, row-major indices; draws the cell rows r0..r1 (inclusive,
   clamped) with the CALLER's program in use, attribute 0 = the world point.
   Returns 1 if anything was drawn. */
int  tagpu_terr_hills_draw(int r0, int r1);

/* RESERVE FOR THIS VIEWPORT, then trim a would-be gather rect (game px) to
   what this pass can actually draw in one frame. Called once a frame by
   tagpu_native.c before any pass sizes itself, so every pass gathers over one
   rect they all agree on.

   THE BUDGET IS THE SCREEN, not a constant. `vw`/`vh` are the true 1x world
   viewport, and the widest rect any zoom can ask for is that viewport at
   TAGPU_ZOOM_MIN — so the staging is reserved for exactly that and grows when
   the player changes resolution. Nothing here names a resolution, and no
   screen is a special case: MEASURED, 1024x768 reserves 83 KB, 2560x1440
   423 KB, 3840x2160 972 KB and 5120x2880 1746 KB, and each draws its whole
   view at the zoom floor.

   The trim is what is left of the old fixed budget, and it now fires only if
   the reservation could not be met — an allocation that failed, or a viewport
   so large the module's own memory guard refuses it. A gather that exceeds the
   staging BAILS, which hands the draw back for a frame and flashes, and a
   flash is the one failure that reads as a bug; a black margin is the honest
   degradation instead.

   THE TRIM HAS A FLOOR THE CALLER PUTS BACK, and it is worth knowing which
   failure that leaves. tagpu_native.c raises the rect to the viewport again
   right after this call (`if (evw < vw) evw = vw`) because a rect narrower
   than the screen would cull terrain that is plainly on it -- choosing a
   visible hole over a flash. So the degradation above is real only while the
   trim stays above the viewport, which is every case the memory ceiling can
   produce: at the 16384-px viewport gate the reserve asks for ~65600 px and
   the ceiling cuts it to ~56000, still far wider than the screen. Below the
   viewport is reachable only if `realloc` itself fails, and there the gather
   does bail and flash. Sizing the ceiling so it can never cross the viewport
   is what keeps that path unreachable; do not lower it without re-checking
   this.

   What this replaced was a fixed 32768 cells sized for 1024x768 and 1920x1080.
   An ordinary 3840x2160 desktop exceeded it at any zoom below about 0.5x, and
   the rect was then cut to roughly a third of the width the view showed —
   terrain, units, features and markers all stopping at a black margin that a
   player reads as the map failing to draw at the edges. */
void tagpu_terr_clamp_span(int vw, int vh, int* w, int* h);

/* the palette index tagpu_terrown.c fills the viewport with in place of the
   engine's terrain blit; the composite treats every OTHER index in the engine's
   frame as "an overlay the engine still draws, and we must not cover it" */
int  tagpu_terr_key(void);
#endif
