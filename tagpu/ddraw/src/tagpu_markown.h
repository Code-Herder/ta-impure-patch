#ifndef TAGPU_MARKOWN_H
#define TAGPU_MARKOWN_H
/* Own the engine's world-space UI markers (G13d) — health bars, group digits,
   order/waypoint/build-queue markers, range circles, the build-cursor
   footprint and the drag band box (research/notes/ui-markers.md).

   These are the last engine pixels left inside the viewport that are anchored
   to WORLD positions, and therefore the last thing standing between us and a
   free view zoom: everything else in the frame is either ours (terrain,
   features, units, effects, fog) or genuinely screen-space (side panel,
   minimap, top bar, chat, dialogs), which must stay at 1:1 at any zoom.

   TWO MECHANISMS, because the markers split cleanly in two:

   1. HEALTH BARS are re-drawn natively (tagpu_mark.c) and the engine's leaf
      `DrawHealthBars 0x46A430` (single caller `0x469CB9`) is skipped. They
      have to be re-drawn rather than captured because the engine's own loop
      walks HotUnits — a list culled to the UNZOOMED viewport — so a captured
      bar layer would stop at the 1x rect and leave the outer ring of a
      zoomed-out view bare. Our walk uses the same effective rect as every
      other gather.

   2. EVERYTHING ELSE IS CAPTURED AND REPLAYED. The order-marker pass alone is
      five drawers over the order list, with a build rect that grows over ten
      ticks, a marching dot phase, a last-seen LOS cache and ShowRanges' text
      labels. Re-deriving that is a lot of arithmetic to get subtly wrong;
      instead the engine draws it, into a scratch 8bpp buffer of ours rather
      than into its frame, and we upload that buffer and draw it as one quad
      through the same zoom transform the world uses. Parity is exact by
      construction, including the text.

   The capture is a pointer swap, nothing more: the OFFSCREEN context passed
   down the draw is a stack local in DrawGameScreen, so pointing its pixel base
   (+0x0C) at our own buffer for the length of a block sends every clipped blit
   inside it to us and leaves the engine's own frame untouched. What the
   blend-reading primitives (`DrawTranspRectangle`'s transparent edges, the
   order sprite's alpha composite) see underneath is the fill key — which is
   exactly what they already read out of the engine's frame today, because
   tagpu_terrown.c fills the viewport with that same key.

   Two capture windows, because fog divides them:

     A  hook 8 `0x469BD7` .. hook 9 `0x469D2C`   order markers + group digits,
                                                 drawn BEFORE the fog overlay
                                                 and therefore fog-darkened
     B  the two `DrawTranspRectangle 0x4BF8C0` calls at `0x469EC5`/`0x469F1E`
                                                 build cursor + band box, drawn
                                                 AFTER fog and never darkened

   And one marker sits outside both windows: the SELECTION RECTANGLE, which the
   engine draws per unit inside the sweeps, immediately before that unit's own
   sprite. tagpu_native.c has always re-drawn it (the engine's is buried under
   our pixels); what was left over is that the engine still drew its own into the
   frame underneath, invisible at 1x because it lands on the same pixels and a
   ghost at any other zoom. Its two call sites are redirected through a per-unit
   test, so units the native pass does not own keep the engine's.

   Six call-site redirects and one prologue detour, so nothing here collides with
   fxown's detour on `0x471F90` or terrown's on `0x4848E0` — our stubs call
   straight through to those addresses and whatever is installed there runs.

   Installed ONCE at DllMain and only if tagpu_markown.on exists then (the
   arming rule, own-the-draw.md); the capture itself follows tagpu_mark.on
   live, so an engine-vs-ours A/B is a file flip with no relaunch. Read-only
   over sim: every site is pure draw. */
#include <windows.h>

void tagpu_markown_init(void);
void tagpu_markown_flush(unsigned int frame_counter);
/* Both follow tagpu_mark.on, and they are SEPARATE levers: skipping the
   engine's health bars without drawing ours would simply lose them, and
   `nobars` has to be able to leave that half alone. */
void tagpu_markown_set_capture(int on);
void tagpu_markown_set_bars(int ours);
/* the selection rectangle, which the native pass has always re-drawn: this stops
   the engine drawing its own underneath, per unit and only for units the native
   pass actually owns */
void tagpu_markown_set_selbox(int ours);
void tagpu_markown_beat(unsigned int frame_counter);   /* "we drew this frame" */
int  tagpu_markown_installed(void);

/* one captured 8bpp layer, handed to the GL side */
typedef struct TAGPU_MARKLAYER {
    const unsigned char* pix;   /* the rect's top-left texel (NULL = nothing) */
    int pitch;                  /* bytes per row of the buffer it sits in     */
    int x, y, w, h;             /* the game-frame rect it covers              */
} TAGPU_MARKLAYER;

enum { TAGPU_MARK_PREFOG = 0, TAGPU_MARK_POSTFOG = 1, TAGPU_MARK_NLAYER = 2 };

/* 1 and *out filled when layer i carries something this frame, else 0 */
int tagpu_markown_layer(int i, TAGPU_MARKLAYER* out);
/* the palette index an untouched texel of a captured layer holds */
int tagpu_markown_key(void);
#endif
