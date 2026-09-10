/* tagpu_pal.h — the palette the screen is actually SHOWN with.

   `main+0x143A7` is the engine's own table, and it is NOT what the player
   sees: every palette the engine sets goes through 0x4BA200, which keeps the
   entries in the graphics globals and hands DirectDraw
   `min(255, entry x *(float*)(globals+0x614))` — the Gamma factor
   (`SetGamma 0x4BA590`; an option screen applies `0.5 + Gamma/24`, so registry
   Gamma 15 is 1.125, while `+gamma N` in chat applies N/10 outright). The
   scale is applied on the way to SetEntries and never to +0x143A7, so at any
   factor but 1.0 a pass that reads +0x143A7 draws the world at the wrong
   brightness while the engine's own pixels beside it are right.
   Engine map: "The palette the screen is presented with".

   This module is the one resolution of that question for the whole DLL: the
   primary's palette object — what cnc-ddraw's ddp_SetEntries stored, the same
   table `tacli shot` writes into its PNG — with the engine's table as the
   fallback until a primary exists.

   RENDER THREAD ONLY, every entry point: they resolve into a shared snapshot,
   so a call from the game thread would race it. tagpu_order.c's seq_ink is the
   one world reader left on the engine's table directly, and its walk being on
   the game thread is why. */
#ifndef TAGPU_PAL_H
#define TAGPU_PAL_H

/* Once per present, before any pass reads it: the next tagpu_pal_live()
   re-resolves. Cheap — one critical section per frame, not per reader. */
void tagpu_pal_frame(void);

/* 256 x {R,G,B,255}. The buffer is ours and lives as long as the process, so
   a caller may keep the pointer (the atlases and the restorer jobs do); its
   CONTENTS follow the screen. NULL only before any palette is readable. */
const unsigned char* tagpu_pal_live(void);

/* Bumped whenever the bytes change, so anything baked FROM the palette — a
   restored Classic++ twin, a LUT — can tell that what it holds is stale.
   0 until the first resolve, and never 0 after it. */
unsigned tagpu_pal_serial(void);

/* The heartbeat's numbers: 1 = the presented palette, 0 = the engine's table;
   how many of the 256 entries differ from main+0x143A7 (and in *first the
   lowest such index, -1 when none); how many changes have been seen. */
int      tagpu_pal_presented(void);
int      tagpu_pal_diff(int* first);
unsigned tagpu_pal_changes(void);

/* The engine's OWN table (main+0x143A7), R,G,B,255, snapshotted beside the
   presented one. Not what the screen shows -- the one caller that wants it is
   the restorer's tileability test, which asks a question about the ART and
   would otherwise reclassify tiles whenever the player moves the Gamma slider,
   because a scaled palette stretches every colour distance by the same factor.
   NULL when the engine's table is unreadable. */
const unsigned char* tagpu_pal_engine(void);

/* The engine's gamma factor itself, for the passes whose colour never came
   from a palette at all (a replacement mesh's glTF texture) and so cannot be
   scaled by resolving through a different one. 1.0 when unreadable, and
   bounded to a band a slider can actually produce. */
float tagpu_pal_gamma(void);

#endif
