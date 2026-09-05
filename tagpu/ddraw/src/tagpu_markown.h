#ifndef TAGPU_MARKOWN_H
#define TAGPU_MARKOWN_H
/* Own the engine's world-space UI markers (G13d..G13p) — health bars, group
   digits, order/waypoint/build-queue markers, range circles and their labels,
   the build-cursor footprint and the drag band box
   (research/notes/ui-markers.md).

   These are the last engine pixels inside the viewport that are anchored to
   WORLD positions, and therefore the last thing standing between us and a free
   view zoom: everything else in the frame is either ours (terrain, features,
   units, effects, fog) or genuinely screen-space (side panel, minimap, top bar,
   chat, dialogs), which must stay at 1:1 at any zoom.

   EVERY ONE OF THEM IS NOW RE-DRAWN, AND THE CAPTURE IS GONE. It was a good
   mechanism and it died of one bound: the engine's rasterisers clip to the
   OFFSCREEN's own width and height (`ctx+0x00`/`ctx+0x04`, read by `0x4CC650`
   BEFORE the clip rect at `+0x1C` is consulted), the offscreen is the size of
   the SCREEN, and `vpwide` lets the projection reach far outside it. So at
   zoom < 1 anything the engine draws at a coordinate off that surface is thrown
   away by its own clipper and there is nothing left to capture — no buffer of
   ours, however wide, can get it back, because widening ours would not change
   the geometry the engine rasterises against. The pieces came over one at a
   time:

     G13d  health bars       re-gathered from unit state; the leaf
                             `DrawHealthBars 0x46A430` prologue-detoured
     G12b  selection rect    already native; its two call sites redirected
                             through a per-unit test
     G13n  build cursor +    re-derived from the six world globals; the engine's
           drag band box     two `DrawTranspRectangle 0x4BF8C0` calls SKIPPED
     G13o  order markers     ported whole (tagpu_order.c): the driver call at
                             `0x469BFC` runs OUR snapshot on the game thread and
                             the engine's `0x48CC30` is skipped
     G13p  group digit +     TA's own glyphs rasterised into a texture of ours
           ShowRanges labels (tagpu_text.c); the digit's `0x469CF9` is skipped

   WHAT IS LEFT OF THE CAPTURE is one window, and it is opened by nothing in
   normal play: the post-fog `DrawTranspRectangle` pair, kept so that
   `tagpu_mark.on=nocursor` can still show the engine's own build cursor through
   our frame for an A/B. The pre-fog window (hook 8 `0x469BD7` .. hook 9
   `0x469D2C`) is gone with G13p, and with it the 64 KB identity blend LUT that
   existed only to stop the order pass's target sprite alpha-compositing against
   our fill key — nothing the engine draws lands in a buffer of ours any more,
   so there is no destination of ours for it to read.

   The capture, where it still runs, is a pointer swap and nothing more: the
   OFFSCREEN context passed down the draw is a stack local in DrawGameScreen, so
   pointing its pixel base (+0x0C) at our own buffer for the length of a block
   sends every clipped blit inside it to us and leaves the engine's own frame
   untouched.

   HOOK 8 AND HOOK 9 ARE STILL REDIRECTED, and not for the capture. They bracket
   one DrawGameScreen marker block, which is what tells tagpu_order.c that a
   block ran in which no snapshot happened (SHIFT released) and the markers must
   come off the screen; and hook 8 is where tagpu_text.c latches the font and
   text colour the engine's own digit and labels would have used.

   Twelve call-site redirects and one prologue detour, so nothing here collides
   with fxown's detour on `0x471F90` or terrown's on `0x4848E0` — our stubs call
   straight through to those addresses and whatever is installed there runs.

   Installed ONCE at DllMain and only if tagpu_markown.on exists then (the
   arming rule, own-the-draw.md); each skip follows its own arm file live, so an
   engine-vs-ours A/B is a file flip with no relaunch. Read-only over sim with
   one deliberate exception on the game thread (tagpu_order.h, the last-seen
   cache). */
#include <windows.h>

void tagpu_markown_init(void);
void tagpu_markown_flush(unsigned int frame_counter);
/* the post-fog capture window, the only one left: `tagpu_mark.on=nocursor`
   turns the re-draw off and this on, so the engine's build cursor can be put
   through our frame beside ours */
void tagpu_markown_set_capture(int on);
/* the health bars. A SEPARATE lever from everything else: skipping the engine's
   without drawing ours would simply lose them, and `nobars` has to be able to
   leave that half alone. */
void tagpu_markown_set_bars(int ours);
/* the selection rectangle, which the native pass has always re-drawn: this stops
   the engine drawing its own underneath, per unit and only for units the native
   pass actually owns */
void tagpu_markown_set_selbox(int ours);
/* the build-cursor footprint and the drag band box: with this set the engine's
   own pair of `DrawTranspRectangle` calls is skipped outright — not captured —
   because `tagpu_mark.c` re-draws both from the same globals */
void tagpu_markown_set_cursor(int ours);
/* the shift-held order markers: with this set the engine's own driver call at
   `0x469BFC` is skipped, after our stub has snapshotted the order lists on the
   game thread for tagpu_order.c to draw from (tagpu_order.h) */
void tagpu_markown_set_orders(int ours);
/* the group digit over a squad-tagged unit: with this set the engine's
   `DrawTextCustomFont` call at `0x469CF9` is skipped and tagpu_mark.c draws the
   digit itself out of tagpu_text.c's atlas */
void tagpu_markown_set_digits(int ours);
void tagpu_markown_beat(unsigned int frame_counter);   /* "we drew this frame" */
int  tagpu_markown_installed(void);

/* one captured 8bpp layer, handed to the GL side */
typedef struct TAGPU_MARKLAYER {
    const unsigned char* pix;   /* the rect's top-left texel (NULL = nothing) */
    int pitch;                  /* bytes per row of the buffer it sits in     */
    int x, y, w, h;             /* the game-frame rect it covers              */
} TAGPU_MARKLAYER;

/* One layer, and it is the post-fog one. The pre-fog layer went with G13p; the
   enum keeps the shape so the GL side can grow another without a rewrite. */
enum { TAGPU_MARK_POSTFOG = 0, TAGPU_MARK_NLAYER = 1 };

/* 1 and *out filled when layer i carries something this frame, else 0 */
int tagpu_markown_layer(int i, TAGPU_MARKLAYER* out);
/* the palette index an untouched texel of a captured layer holds */
int tagpu_markown_key(void);
#endif
