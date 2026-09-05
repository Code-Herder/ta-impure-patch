#ifndef TAGPU_MARK_H
#define TAGPU_MARK_H
/* UI marker pass (G13d..G13p) — the world-space markers the engine used to draw
   over the viewport: health bars, group digits, order/waypoint/build-queue
   markers, range circles and their labels, the build-cursor footprint and the
   drag band box.

   They are the last engine pixels inside the viewport that are anchored to a
   WORLD position, so they are what stands between us and a free view zoom —
   everything else in the frame is either ours or genuinely screen-space.

   Two sources, one draw (see tagpu_markown.h for why):

     health bars   re-gathered here from the engine's own unit state and drawn
                   as flat quads, so they follow the ZOOM's wider rect instead
                   of the engine's HotUnits list, which is culled at 1x
     build cursor  likewise re-drawn, from the six world globals the engine
     + band box    projects at 0x469E13 — a capture cannot reach them at all
                   at zoom < 1, because it is bounded by the screen-sized
                   offscreen and their engine position is not (see the header
                   comment in tagpu_mark.c)
     group digit   TA's own glyphs, rasterised through the engine's own blitter
     + ShowRanges  into an atlas of ours (tagpu_text.h) and drawn at a constant
     labels        SCREEN size, because a bitmap glyph magnified with the zoom
                   is the 1997 art this pass exists to stop
     order markers ported whole in tagpu_order.c, emitted through the two
                   buckets below

   NOTHING IS CAPTURED ANY MORE except the post-fog build-cursor window, which
   only `nocursor` opens (see tagpu_markown.h for why the capture died)

   Both are drawn through the same zoom transform as the world, so a marker
   stays over its unit at any zoom and scales with it — the tile art, the unit
   sprites and the markers magnify together rather than sliding apart.

   Armed by tagpu_mark.on (tokens: log, passive, nobars, nocapture, noselbox,
   nocursor, nodigits). Refuses to
   emit unless tagpu_markown.c actually installed its patches — without them the
   engine is still drawing these markers itself and ours would be a double
   draw at the wrong place. */
#include "tagpu_fx.h"

int  tagpu_mark_armed(unsigned frame_counter);   /* re-reads tagpu_mark.on (30f) */
/* build this frame's health-bar quads; returns the number of bars. Also runs
   the order-marker gather (tagpu_order.c), which emits through the two
   functions below — that pass's geometry belongs in THIS pass's buckets,
   because its draw slot is the engine's own: markers first, health bars over
   them (`0x469BFC` before `0x469CB9`). */
int  tagpu_mark_gather(const TAGPU_FXVIEW* v);

/* Emission API for tagpu_order.c: flat-coloured geometry in game-frame
   coordinates, fogged at the world point (wx,wz) the caller names. `colidx` is
   a PALETTE index, as gui[] holds — not a GUI slot number. 0 = the bucket is
   full and nothing was emitted, so the caller can count the drop.

   Lines are drawn GL_LINES at glLineWidth(ss), i.e. one SCREEN pixel at any
   zoom; triangles carry no such trick and must be sized by the caller. */
int  tagpu_mark_emit_line(float x0, float y0, float x1, float y1,
                          int colidx, float wx, float wz);
int  tagpu_mark_emit_tri(float x0, float y0, float x1, float y1,
                         float x2, float y2, int colidx, float wx, float wz);
/* One string at the (x, y) the engine would have handed DrawTextCustomFont,
   drawn out of tagpu_text.c's atlas in palette index `colidx` at a constant
   SCREEN size. 0 = the atlas refused the string or the bucket is full. */
int  tagpu_mark_emit_text(float x, float y, const char* s, int colidx,
                          float wx, float wz);
/* draw into the currently bound FBO. Expects depth test and blending OFF (the
   markers are the frame's top layer and every fragment is opaque); own
   program/VAO, leaves program, VAO and texture bindings dirty. */
void tagpu_mark_render(const TAGPU_FXVIEW* v, unsigned int palTex);
void tagpu_mark_glreset(void);
#endif
