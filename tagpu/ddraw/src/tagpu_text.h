#ifndef TAGPU_TEXT_H
#define TAGPU_TEXT_H
/* Engine text, rasterised into a texture of ours (G13p) — the L2 half of the
   marker port: the group digit over a squad-tagged unit and the `ShowRanges`
   labels beside each range circle.

   WHY THIS EXISTS AT ALL. Those two are the last engine-drawn world-anchored
   pixels in the frame, and they are bounded exactly as every marker before them
   was: the engine's drawers clip to the OFFSCREEN's own width and height
   (`ctx+0x00`/`ctx+0x04`, read by `0x4CC650` before the clip rect is consulted),
   and the offscreen is the size of the SCREEN while `vpwide` lets the projection
   reach far outside it. So at zoom < 1 a digit or a label out in the ring is
   thrown away by the engine's own rasteriser and no capture can reach it
   (research/notes/ui-markers.md 6.1). The answer is the same one G13n and G13o
   gave: own the draw.

   AND WHY IT IS NOT A FONT PORT. TA's in-game bitmap font is a private format
   and decoding it would be a week of work for nothing. Its blitter is not:

     0x4CCF60(u8* base, int pitch, void* font, const char* str, int x, int y,
              int fg, int bg, int transparent)                cdecl, 9 args

   takes its DESTINATION DIRECTLY — no OFFSCREEN, no clip rect, none of the
   `0x4CC650` surface bound — so it will rasterise TA's own glyphs into any
   buffer of ours at any position. [BINARY-VERIFIED: `DrawTextCustomFont
   0x4C14F0` pushes `[edi+0xC]` and `[edi+0x8]`, its OFFSCREEN's pixel base and
   pitch, as arguments 1 and 2 at `0x4C173E`/`0x4C1738`; the ctx==NULL arm at
   `0x4C16A0` pushes the same two fields of a locally locked surface.] We call it
   with `fg=255, bg=0, transparent=0`, which makes the store `if (colour !=
   transparent)` keep only the set bits: the result is a 1-bit COVERAGE MASK, not
   a coloured sprite, so the atlas is colour-free and one raster serves the same
   string in any colour, at no cost when a colour changes. (An earlier revision
   justified this by saying the weapon-range labels flash their colour every game
   tick. They do not: `0x438EA0` hands the flashing colour to the LINE drawer
   only, and the string goes to `DrawTextCustomFont`, whose foreground is
   `[globals+0x208]` — set once at `0x4696E7` and untouched inside the block. The
   design is right; that reason for it was wrong.)

   Each distinct string is rasterised ONCE into a shelf-packed 8bpp atlas, and
   tagpu_mark.c draws it as a quad in the palette index the engine would have
   used. Constant SCREEN size at any zoom, like the waypoint crosshair and unlike
   the health bars: a label is information, and a bitmap glyph magnified 4x is
   the "1997 art blown up" this port exists to stop — at 0.25x it would be two
   pixels tall and unreadable.

   THE FONT AND THE COLOUR ARE SNAPSHOTTED ON THE GAME THREAD, at hook 8, and
   not read live: `[globals+0x204]` is whatever the engine's last `SetFont
   0x4C1420` left there and the engine changes it many times a frame, so a
   present-thread read would get the side panel's font as often as the game's.
   Between hook 8 `0x469BD7` and the digit at `0x469CF9` nothing calls
   `0x4C1420` or `0x4C13A0` — the order driver, the walker, the five drawers and
   `DrawHealthBars` are all clear of both, checked against a full-image scan of
   their call sites — so one latch at hook 8 is exactly what both the digit and
   the range labels would have drawn with. `[globals+0x208]` is the foreground
   colour those same two use, a palette index; `DrawGameScreen` sets it at
   `0x4696E7` to `gui[0xF]` with the background left equal to the transparent
   index. [BINARY-VERIFIED]

   The rasterise itself runs on the PRESENT thread, on a cache miss only (at most
   once per distinct string per font, ~20 in a session). That is safe in a way
   the order-list walk was not: `0x4CCF60` reads the font object and writes our
   buffer, and touches no engine state whatsoever — no globals, no allocation, no
   clip. The one hazard left is the font object's lifetime, which is the session,
   and which tagpu_ui.c already takes the same way when it measures a listbox
   row. */

/* ---- game thread ---- */
/* Latch `[globals+0x204]` and `[globals+0x208]`. Called from markown's hook-8
   stub, i.e. at the instant the engine's own marker block begins. */
void tagpu_text_snapshot(void);

/* ---- present thread ---- */
/* Latch the font this frame's rasters will use. Called once per gather, BEFORE
   any tagpu_text_place: the game thread republishes the snapshot ~83 times per
   present, and a font change re-packs the atlas, which must not happen between
   two quads of the same frame. */
void tagpu_text_frame(void);
/* The palette index the engine would draw this block's text in; -1 if nothing
   has been snapshotted yet (no in-game frame has run). */
int  tagpu_text_colour(void);

/* Find or rasterise `s` in the atlas. On success fills the atlas rect in TEXELS
   (ax, ay, w, h) and the row offset the blitter applies (`yoff`, the signed
   `font+0x02`, which the engine SUBTRACTS from the y it is given — so the
   string's first pixel row lands at `y - yoff`). 0 when there is no font, the
   string does not fit, or the atlas is full. */
int  tagpu_text_place(const char* s, int* ax, int* ay, int* w, int* h, int* yoff);

/* ---- the GLYPH cache (G17d), present thread ---- */
/* Find or rasterise ONE character of `font` in the glyph atlas. The engine's
   own blitter advances x by the glyph's width byte and nothing else, so a run
   of these at those offsets reproduces its string blit exactly rather than
   approximating it — which is what the GL UI renderer's `PK_STRING` needs and
   what tagpu_text_place cannot give it: the UI's text is metal readouts and a
   clock, a new STRING every tick, against a 64-entry string cache.
   Its atlas is separate from the string one: different lifetimes (a font change
   repacks that one) and different keys. 0 when the font will not read, the code
   is outside [0x20, 0x7E], the font's table skips it, or the atlas is full.
   `font` is validated here — it arrives from a published op, not from the
   snapshot. */
int  tagpu_text_glyph(const void* font, int ch, int* ax, int* ay, int* w, int* h, int* yoff);
void tagpu_text_glyph_dims(int* w, int* h);
unsigned int tagpu_text_glyph_tex(void);
int  tagpu_text_glyph_stats(unsigned* glyphs, unsigned* drops, int* fonts);

/* ---- GL (present thread, context current) ---- */
/* The atlas texture, uploading it first if a raster has landed since the last
   call. 0 when there is nothing to draw. */
unsigned int tagpu_text_tex(void);
void tagpu_text_dims(int* w, int* h);
void tagpu_text_glreset(void);
int  tagpu_text_stats(int* strings, int* dropped);
#endif
