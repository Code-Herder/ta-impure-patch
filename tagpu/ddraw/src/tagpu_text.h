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

   THE FONT ARRIVES AS BYTES, IN THE FRAME PACKET (landing 1 of the frame packet
   exchange, 2026-09-12). The engine's font and text colour are latched on the
   GAME THREAD at hook 8 — `[globals+0x204]` is whatever the engine's last
   `SetFont 0x4C1420` left there and the engine changes it many times a frame,
   so a present-thread read would get the side panel's font as often as the
   game's, and between hook 8 `0x469BD7` and the digit at `0x469CF9` nothing
   calls `0x4C1420` or `0x4C13A0` [BINARY-VERIFIED] — and the publisher
   (tagpu_packet_pub.c) COPIES the font's header and its 95 printable glyphs,
   each as a one-glyph font object the blitter accepts, into every packet.
   Until this landing the present thread held the engine's font POINTER and
   dereferenced it behind IsBadReadPtr; no note establishes a UI font's
   lifetime and a probe is not a lifetime argument (CLAUDE.md). Now no
   render-thread code touches an engine font: the raster runs the engine's
   blitter, on the present thread, over our copy, one glyph at a time at the
   same x the blitter's own string loop would have used (it advances by the
   width byte and nothing else), so the pixels are the ones the engine draws.
   `0x4CCF60..0x4CD00E` reads its arguments and writes its destination and
   touches nothing else — verified instruction by instruction: no global, no
   allocation, no clip — which is what makes engine code on the present thread
   legitimate here, and the reason tagpu_text.c is the allow-list's one
   "pure engine code" entry (tools/thread-split-check.sh). */
#include "tagpu_packet.h"

/* ---- present thread ---- */
/* Latch the font and colour THIS frame's rasters will use, from the packet the
   driver acquired this frame (NULL = no packet: keep what we have). Called once
   per gather, BEFORE any tagpu_text_place: a font change re-packs the atlas,
   which must not happen between two quads of the same frame. The glyph bytes
   are copied out of the packet, keyed on its font generation, so nothing here
   holds a packet pointer past the frame. */
void tagpu_text_frame(const TAGPU_PACKET* pk);
/* The palette index the engine would draw this block's text in; -1 if no
   packet has carried one yet. */
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
   repacks that one) and different keys. */
/* THE UI'S GLYPHS, BY ID AND BY BITS (landing 4c). `feed` installs every glyph
   record a string op carried — `{u8 code, u8 w, u16 nbytes, u8 bits[]}` each,
   4-byte aligned, the producer's copy of the font's own packed rows — and
   `glyph_id` is the lookup afterwards. Nothing here dereferences a font: the
   string op used to carry the engine's font OBJECT and this file read its
   header, its offset table and every glyph behind IsBadReadPtr, on the present
   thread, up to a queue backlog after the observer saw it. The blitter still
   stamps the glyph, from a one-glyph font object of OURS. */
void tagpu_text_glyph_feed(unsigned font_id, int rows, int yoff,
                           const unsigned char* block, unsigned n, unsigned len);
int  tagpu_text_glyph_id(unsigned font_id, int ch, int* ax, int* ay,
                         int* w, int* h, int* yoff);
/* where the string starts inside the block, past the glyph records */
unsigned tagpu_text_glyph_block_bytes(const unsigned char* block, unsigned n, unsigned len);
void tagpu_text_glyph_dims(int* w, int* h);
/* Bumped whenever the glyph atlas repacks, which invalidates every cell handed
   out before it. A caller that gathers a run of cells and draws them afterwards
   must read this before the gather and again after it, and discard the run if
   it moved -- the cells would otherwise name texels that have just been
   cleared and re-used. */
unsigned tagpu_text_glyph_gen(void);
/* has the cache rasterised any glyph? */
int tagpu_text_glyph_have(void);
/* THE GLYPH ATLAS AS BYTES (G19f landing 2). A second backend cannot read a GL
   texture, and this one needs no mechanism to expose: the module already keeps
   the atlas as a CPU array and uploads the texture FROM it, so this is the
   array and `tagpu_text_glyph_gen`'s counter says when it last moved. Render
   thread only. */
const unsigned char* tagpu_text_glyph_atlas(int* w, int* h);
/* THE ATLAS'S CONTENT SERIAL -- bumped whenever its BYTES change, including an
   ordinary new glyph, which `tagpu_text_glyph_gen` does NOT count. Key a second
   backend's upload on this one; key the validity of CELLS ALREADY RESOLVED on
   the generation above. Render thread only. */
unsigned tagpu_text_glyph_serial(void);
int  tagpu_text_glyph_stats(unsigned* glyphs, unsigned* drops, int* fonts);

/* THIS MODULE HOLDS NO GPU OBJECT (11-5e-1). It rasterises with TA's own
   blitter into two CPU arrays and hands them out as bytes; a backend uploads
   them itself. `tagpu_text_tex` and `tagpu_text_glyph_tex` used to own a GL
   texture each, creating it lazily and re-uploading the whole array when a
   raster landed, and `tagpu_text_glreset` dropped both ids when the fork's GL
   context changed. All three were callerless [masked scan, 11-5e-1] -- their
   callers went with the GL draws in 11-4a and 11-4b -- and the reset cascade
   that reached the last of them could not fire at all (tagpu_overlay.c).

   WHAT REPLACED THEM IS ALREADY HERE AND IS BETTER: the two serials below.
   A texture-owning accessor can serve only one backend, because the upload
   consumes the dirty flag that told it to run; a counter can serve any number,
   because each consumer keeps its own last-seen value. That was found the hard
   way (the G19f landing-2 review, both reviewers) and it is the reason nothing
   is ported back. */
void tagpu_text_dims(int* w, int* h);

/* ---- the bytes a backend uploads (Phase G / G19d) ----
   The CPU-side string atlas -- ATLAS_W x ATLAS_H, one coverage byte per texel,
   128 KB -- and a counter that ticks whenever a raster lands in it. A backend
   holds its own texture and refills it when the counter moves. Valid for the
   process's life; render thread. */
const unsigned char* tagpu_text_atlas(unsigned* gen);
int  tagpu_text_stats(int* strings, int* dropped);
#endif
