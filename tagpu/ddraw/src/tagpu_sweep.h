#ifndef TAGPU_SWEEP_H
#define TAGPU_SWEEP_H
/* tagpu_sweep.c -- the reclaim sweep: with RECLAIM armed (cursor mode 12), a left drag on the
   ground draws a ring; its release orders the selected reclaimers to take the features of the
   chosen kind inside it, shared nearest-first (tagpu_sweepgeo.h); a click below the drag's
   threshold is stock's click. The cursor shows a feature of the other kind as ground, and a click
   on it orders nothing. A popup off the RECLAIM gadget's row picks the kind, and its quickkey
   cycles it, while armed; a right press or Esc ends a drag. What a commit queues is drawn by the engine's
   own queued-order markers. Off on GDI and on a puppet seat; the ring, the
   filter and the popup outside mode 12 too. The kind is kept in the settings store.

   Every function here runs on the GAME thread; the render thread sees all of it only as the
   packet's copied table (PK_SWEEP) and hands back the popup it drew (TAGPU_CMD_SWEEP). */
#include "tagpu_packet.h"

/* the kind of feature a ring takes; the first is ALL */
enum { SWEEP_METAL, SWEEP_ENERGY, SWEEP_ALL };

/* DLL attach, after tagpu_route_init: arms when the router holds all of its sites */
void tagpu_sweep_init(void);
/* the level's teardown (tagpu_packet_pub.c, beside tagpu_place_level_end): the gesture and the
   taken presses cleared, the level's counters logged */
void tagpu_sweep_level_end(void);

/* The click route's verdict for a mode-12 record `rec` (the handler's stack copy) with message
   `msg`: 0 stock, 1 consumed, 2 run stock's click on this record (tagpu_route.c). */
int tagpu_sweep_button(const int* rec, unsigned msg);
/* The in-play handler's entry, before the record is handled (tagpu_route.c). */
void tagpu_sweep_entry(void);
/* The cursor index the handler stores in main+0x2CBE: `idx`, the chooser's answer for `mode`,
   or 0x13 where a click would take a feature of the other kind. `handler` is 1 at the call
   inside the in-play handler, whose record the gesture follows (tagpu_route.c). */
int tagpu_sweep_cursor(int mode, int idx, int handler);

/* A character the window procedure delivers (tagpu_shield.c): 1 when the sweep took it -- Esc
   during a drag, or the RECLAIM gadget's quickkey while armed -- and the engine must not see it. */
int tagpu_sweep_char(unsigned ch);
/* A mouse button the window procedure delivers, at the engine point (gx, gy): 1 when the sweep
   took the press, or the release of a press it took, and the engine must not see it. */
int tagpu_sweep_mouse(int gx, int gy, int down, int right);
/* the window lost the mouse: the drag ends and no release is waited for */
void tagpu_sweep_cancel(void);

/* The packet's sweep header and items, from the publisher; it may run more than once a frame,
   and its one side effect is offering a pending kind to the settings store. Returns 1 with the
   header filled when the sweep has something to show; `*n` items written, at most `max`, and
   `*cut` set when more would have been. */
int tagpu_sweep_fill(TAGPU_PK_SWEEP* h, TAGPU_PK_SWEEPITEM* items, unsigned max, unsigned* n,
                     int* cut);

/* The command record's popup as drawn, where the record is applied (tagpu_packet_pub.c); NULL
   when no record stands. */
void tagpu_sweep_take(const TAGPU_CMD_SWEEP* c);

#endif
