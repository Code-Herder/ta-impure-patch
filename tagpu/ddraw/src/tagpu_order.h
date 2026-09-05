#ifndef TAGPU_ORDER_H
#define TAGPU_ORDER_H
/* Order markers (G13o) — the shift-held overlay: queued build sites, the
   marching route dots, waypoint sprites, target circles and range circles.

   WHY THIS IS A PORT AND NOT A CAPTURE. Everything in this block used to be
   captured: the engine drew it into a scratch 8bpp buffer of ours and we
   replayed the buffer as one quad (tagpu_markown.h, window A). A capture can
   only ever reach the OFFSCREEN's own bound — `ctx+0x00`/`ctx+0x04`, read by
   `0x4CC650` BEFORE the clip rect is consulted at all — and the offscreen is
   the size of the SCREEN, while `vpwide` lets the engine project these markers
   at coordinates far outside it. So at zoom < 1 every marker whose engine
   position leaves that surface is clipped away by the engine's own rasteriser
   and there is nothing left to capture: measured at 0.467x, a mex queued
   inside the central band shows its site rect and one queued out in the ring
   shows nothing. That is the same wall G13n hit with the build cursor, and the
   answer is the same one — own the draw (research/notes/ui-markers.md 6.1).

   TWO THREADS, AND THE SPLIT IS THE SAFETY ARGUMENT.

   The order list is a LINKED LIST WHOSE NODES THE SIM FREES (head `unit+0x5C`,
   next `node+0x4A`) — unlike the unit array, which is stable storage a render
   thread may read at any time. Walking it from the present thread can follow a
   pointer into a recycled block (still mapped, still readable, now somebody
   else's data — IsBadReadPtr does not catch that), into a cycle (the render
   thread spins and the game freezes with the process still up), or into a page
   the allocator has since returned. There is already one unexplained
   render-thread crash of exactly this class in the tree; see the fog-grid
   guard in tagpu_fx.c.

   So the walk happens ON THE GAME THREAD, at the very call site the engine's
   own driver was reached from (`0x469BFC` -> `0x48CC30`), and copies what it
   needs into an arena. The present thread only ever walks the arena, which is
   flat storage we own. Two arenas, the published index stored last, and the
   publication only ever REPLACED, never emptied first — the discipline
   measured in ui-markers.md 6.2, where clearing on the way in put a hole in
   11% of presents because the engine runs this block ~83 times per frame.

   NATIVE RESOLUTION, DELIBERATELY. The markers are re-drawn as geometry, not
   as magnified 1997 art: real arcs instead of the engine's 16 chords, one
   SCREEN pixel of line width at any zoom, a round dot where `pathicon` put one
   and a crosshair on the engine's own animation phase where `cursor_ary` put
   one. Same spacing (one dot per 48 world px), same phase (+48 px per 30
   ticks), same colours — the dot and crosshair inks are read out of the GAF
   frames the engine would have blitted. It is TA's iconography drawn properly,
   not a restyle; the rest of the HUD is still TA's art.

   THE SHOWRANGES LABELS CAME WITH G13p. Every circle both limbs draw carries
   the engine's own label string at the engine's own point on it, rasterised
   through TA's blitter into tagpu_text.c's atlas — so `ShowRanges` is now whole
   rather than a set of unnamed rings. The same landing found that G13o drew
   every RANGE circle 11% flat: `DrawRangeCircle 0x438EA0` hands one radius to
   both lookups and only the TARGET circle `0x4399F0` squashes its y by the 0.89
   at `0x4FD2C0`.

   Armed by tagpu_order.on. Tokens: `log`, `passive` (gather and count, let the
   engine draw), `trace` (both sides run and both log their node lists, for the
   selection-rule diff), and `nobuild` / `nodots` / `nocircle` / `nosprite` /
   `noranges` / `nolabels`. Refuses to take the draw unless tagpu_markown.c
   actually installed the `0x469BFC` redirect. */
#include "tagpu_fx.h"

/* ---- arming (present thread) ---- */
int  tagpu_order_armed(unsigned frame_counter);  /* re-reads tagpu_order.on (30f) */
int  tagpu_order_on(void);                       /* armed state, no re-read       */

/* ---- game thread: the snapshot at 0x469BFC ---- */
/* Apply the driver's selection rules, walk each selected unit's order list,
   copy what the drawers need into the arena and publish it. 1 when the
   publication is complete and the caller may SKIP the engine's driver; 0 when
   it must let the engine draw its own (not armed, passive, trace, or a
   snapshot that could not be trusted). */
int  tagpu_order_snapshot(void* ctx, void* view);
/* Bracket one DrawGameScreen marker block, so a block in which no snapshot ran
   — SHIFT released, the gate not taken — gives the last publication up instead
   of leaving it standing on screen forever. */
void tagpu_order_block_begin(void);
void tagpu_order_block_end(void);
/* One engine drawer call, logged under `trace` so the engine's own node list
   can be diffed against ours. `bit` is the capability bit (0..4; 5 = the
   sprite reached by delegation from the route-dot drawer). Inert otherwise. */
void tagpu_order_trace_drawer(int bit, const void* node, const int* pos, int flag);

/* ---- present thread ---- */
/* Walk the published arena and emit this frame's marker geometry through
   tagpu_mark.c's line and triangle buckets. Returns the number of arena
   records drawn. Called from tagpu_mark_gather, because the geometry lands in
   that pass's buckets and its draw slot is the engine's own. */
int  tagpu_order_gather(const TAGPU_FXVIEW* v);
void tagpu_order_frame_done(const TAGPU_FXVIEW* v);   /* per-frame log */
#endif
