#ifndef TAGPU_MARK_H
#define TAGPU_MARK_H
/* UI marker pass — the world-space markers the engine would otherwise draw
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

   NOTHING IS CAPTURED except the post-fog build-cursor window, which only
   `nocursor` opens (see tagpu_markown.h for why)

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

/* Is the CURSOR LAYER ours this frame — the build placement square, the drag
   band box and the pointer? Exactly gather_cursor's own gate (the lever, its
   `nocursor` token, passive mode, and whether the engine's own draw is
   actually redirected). A client that draws a twin of anything in that layer
   must ask THIS rather than re-derive it: the build ghost is the twin of the
   placement square, and when the two disagreed the ghost tracked the pointer
   over the engine's unzoomed square. */
int  tagpu_mark_cursor_ours(void);
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

   Lines are drawn as a line list `ss` device pixels wide (tagpu_vk_mark.c),
   i.e. one SCREEN pixel at any zoom; triangles carry no such trick and must
   be sized by the caller. */
int  tagpu_mark_emit_line(float x0, float y0, float x1, float y1,
                          int colidx, float wx, float wz);
int  tagpu_mark_emit_tri(float x0, float y0, float x1, float y1,
                         float x2, float y2, int colidx, float wx, float wz);
/* One unit's SELECTION RECT (`DrawUnitSelectBoxRect 0x46A530`, ui-markers.md
   §1): the closed loop p0->p1->p2->p3->p0 as four lines at `ss` width, in
   palette index `colidx`, fogged at (wx, wz).

   IT IS THE ONLY MARKER HERE THAT IS DEPTH-TESTED, because it is the only one
   the engine draws INSIDE the unit sweep -- under its own unit's sprite and
   under every later row, where every other marker is drawn over the finished
   world. `depth` is the clip-space z the unit pass would give a vertex of key
   `enc` (`1 - enc/depthScale`), so the caller, which owns the keys, decides
   what the rect sits under. The caller also owns the corners' arithmetic
   (the engine's truncating projection, tagpu_native.c). 0 = not ours this
   frame (the pass is disarmed or passive, or `noselbox`) or the bucket is
   full. */
int  tagpu_mark_emit_selbox(const float px[4], const float py[4], int colidx,
                            float wx, float wz, float depth);
/* One string at the (x, y) the engine would have handed DrawTextCustomFont,
   drawn out of tagpu_text.c's atlas in palette index `colidx` at a constant
   SCREEN size. 0 = the atlas refused the string or the bucket is full. */
int  tagpu_mark_emit_text(float x, float y, const char* s, int colidx,
                          float wx, float wz);
/* Gather the marker layer and publish it through `tagpu_mark_handover`. This
   issues NO draw. The markers are the frame's top layer with every fragment
   opaque; tagpu_vk_mark.c honours that. */
void tagpu_mark_render(const TAGPU_FXVIEW* v);
/* ---- the hand-over to the Vulkan lane ----

   THIS PASS IS NOT ONE DRAW AND THAT IS THE WHOLE POINT OF THE RECORD. It is
   order triangles, order lines at `ss` line width, order labels, health bars,
   group digits, the post-fog layer and the build cursors -- in the engine's own
   order (`0x469BFC` -> `0x469CB9` -> `0x469CF9`, then the cursor after the fog
   overlay), each with its own `uText` and `uFog`, and the last two with fog
   forced OFF because the engine draws them after its fog overlay and never
   darkens them. A consumer that re-derived which buckets are non-empty from
   the counts would be free to disagree with the pass that gathered them, so
   what crosses is the DRAW LIST `tagpu_mark_render` builds.

   NOTHING HERE NEEDS A NEW MIRROR: the vertices are this file's own arrays and
   `tagpu_text_atlas` exists for exactly this. */
/* 1 is retired rather than reused, so a stale `tex` field cannot silently
   become a text draw. */
enum { TAGPU_MK_TEX_NONE = 0,   /* no sampler feeds uLayer this draw */
       TAGPU_MK_TEX_TEXT = 2 }; /* tagpu_text.c's coverage atlas */

typedef struct TAGPU_MKDRAW {
    int first, count;           /* vertices into `verts` below              */
    int lines;                  /* 1 = LINE_LIST at `ss` width, 0 = TRIANGLES */
    int text;                   /* uText */
    int fog;                    /* uFog, AS THE DRAW SET IT -- not derived  */
    int tex;                    /* TAGPU_MK_TEX_*                           */
    int depth;                  /* 1 = depth-TESTED against the world (never
                                   written): the selection rects alone. Every
                                   other draw is the frame's top layer.      */
} TAGPU_MKDRAW;

#define TAGPU_MK_MAXDRAW 16

typedef struct TAGPU_MKHAND {
    unsigned frame;
    /* 8 floats a vertex: x,y  u,v  wx,wz  colour  depth -- this file's MVST,
       and the layout `tagpu_mark.spv.h`'s vertex stage expects. `depth` is
       clip z and is 0 on every vertex but a selection rect's. */
    const float* verts; int nvert;
    const TAGPU_MKDRAW* draws; int ndraw;
    /* tagpu_text.c's atlas: one coverage byte a texel. `textGen` moves when a
       raster lands, which is how a backend holding its own copy is told. */
    const unsigned char* text; unsigned textGen; int textW, textH;
    /* THE TWO SHARED TEXTURES, AS BYTES: the consumer builds its own
       images from them. They come over in the shapes `tagpu_fx.h` already uses
       for the same two, so the two consumers agree about what they are:
       the palette is 256 RGBA8 texels of `tagpu_pal_engine()` and the fog grid
       is cols x rows of RG8. A draw that wants fog and whose grid did not
       cross is refused rather than drawn unfogged. */
    const unsigned char*  pal;        /* 256 x RGBA8 */
    unsigned              palSerial;
    const unsigned short* fogGrid;    /* cols x rows RG8; NULL when fog is off */
    int                   fogGridCols, fogGridRows;
    int   key;                  /* uKey: the index an untouched layer texel holds */
    float gw, gh, zoom, zoomCx, zoomCy;
    float fogOrgX, fogOrgY, fogCols, fogRows;
    float ss;                   /* the line width a one-screen-pixel line takes */
    /* 1 on the ONE frame `tagpu_mark.ab` latched its claim and
       `tagpu_vk_ab_arm` got the `_vk.ppm` target unlinked, so the Vulkan lane
       captures THAT frame rather than whichever one its own lever poll landed
       on. It does NOT mean a capture file was written. */
    int   ab;
    /* THE VIEWPORT THE MARKERS ARE CLIPPED TO, in game-frame pixels measured
       from the TOP. The consumer needs it for its scissor, and `scissorOn` is
       what the native pass ACTUALLY did rather than what it would have liked
       to -- tagpu_vk_fx.c's rule, and its `fx_scissor` is the shape to follow. */
    int   vpL, vpT, vw, vh;
    int   scissorOn;
} TAGPU_MKHAND;

/* Exactly once per frame, and only for the frame it was published for -- `now`
   is COMPARED, not stamped: a stamped record could be read again stale and make
   a census over-count. 0 = nothing to draw. */
int  tagpu_mark_handover(TAGPU_MKHAND* out, unsigned now);
/* This frame's counter, and the previous frame's record dropped with it.
   Unconditional, beside `tagpu_posedraw_frame`: `tagpu_mark_render` returns
   early on an ordinary empty frame, so without this a record outlives its
   frame. */
void tagpu_mark_frame(unsigned frame_counter);
#endif
