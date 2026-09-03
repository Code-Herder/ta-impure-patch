#ifndef TAGPU_MARK_H
#define TAGPU_MARK_H
/* UI marker pass (G13d) — the world-space markers the engine used to draw over
   the viewport: health bars, group digits, order/waypoint/build-queue markers,
   range circles, the build-cursor footprint and the drag band box.

   They are the last engine pixels inside the viewport that are anchored to a
   WORLD position, so they are what stands between us and a free view zoom —
   everything else in the frame is either ours or genuinely screen-space.

   Two sources, one draw (see tagpu_markown.h for why):

     health bars   re-gathered here from the engine's own unit state and drawn
                   as flat quads, so they follow the ZOOM's wider rect instead
                   of the engine's HotUnits list, which is culled at 1x
     everything    captured 8bpp, exactly as the engine drew it, and uploaded
     else          as two layers — one from before the fog overlay (darkened
                   like the engine's) and one from after it (never darkened)

   Both are drawn through the same zoom transform as the world, so a marker
   stays over its unit at any zoom and scales with it — the tile art, the unit
   sprites and the markers magnify together rather than sliding apart.

   Armed by tagpu_mark.on (tokens: log, passive, nobars, nocapture). Refuses to
   emit unless tagpu_markown.c actually installed its patches — without them the
   engine is still drawing these markers itself and ours would be a double
   draw at the wrong place. */
#include "tagpu_fx.h"

int  tagpu_mark_armed(unsigned frame_counter);   /* re-reads tagpu_mark.on (30f) */
/* build this frame's health-bar quads; returns the number of bars */
int  tagpu_mark_gather(const TAGPU_FXVIEW* v);
/* draw into the currently bound FBO. Expects depth test and blending OFF (the
   markers are the frame's top layer and every fragment is opaque); own
   program/VAO, leaves program, VAO and texture bindings dirty. */
void tagpu_mark_render(const TAGPU_FXVIEW* v, unsigned int palTex);
void tagpu_mark_glreset(void);
#endif
