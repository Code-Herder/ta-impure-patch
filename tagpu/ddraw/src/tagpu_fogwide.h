#ifndef TAGPU_FOGWIDE_H
#define TAGPU_FOGWIDE_H

#include "tagpu_fogsrc.h"

/* tagpu_fogwide — the fog of war's SOURCE for the whole map, published for the Vulkan lane.

   THE PROBLEM. The engine's overlay samples its own screen fog grid
   (`*(main+0x1421F)`, terrain-depth.md §5.2): a view-anchored lattice of 32-px
   cells whose two bytes are 4-bit corner masks. That grid is allocated ONCE per
   map load, from the engine's viewport size — `0x483BB8`:

       cols = viewW/32 + (viewW % 32 ? 3 : 2)      rows likewise from viewH

   — and `0x4843C0` rebuilds it anchored at the eye, so it covers the 1x
   viewport and about two cells more. A zoomed-out view reaches far past it.

   Neither half of the engine's grid can be moved. Its dimensions come from a
   map-load allocation, and its origin is recomputed from `main+0x1431F/0x14323`
   inside the builder itself — lying to it about the eye would have to be undone
   before the render thread reads the same field, which is a race with the whole
   world's position on the wrong side of it.

   SO WE PUBLISH WHAT THE GRID IS BUILT FROM. The builder's per-cell tests — the
   LOCAL player's LOS counter (true LOS only) and the MAPPED bitmap — are packed
   for every cell of the map, two bits a cell (inc/tagpu_fogsrc.h), and every reader
   expands them as the builder would: the shaders through TAGPU_GLSL_FOG_FN, the CPU
   gates through tagpu_fog_entry. A source that covers the whole map has no window,
   so no zoom and no step of the eye can outrun it. Seven Islands, 640 x 640 cells,
   is 100 KB.

   WHERE IT RUNS. On the GAME thread, once in every tracked in-play draw: from
   `tagpu_terrown.c`'s `terr_fogtick`, the engine's own fog-overlay call site,
   when that site is ours, and otherwise, while the session renders on
   Vulkan, from the packet publisher's `after` of the same draw (`wide_tick`
   in tagpu_packet_pub.c) -- so a level that runs with our terrain pass off still
   has the source. On the GDI lane nothing reads it and neither runs. Both read
   the LOS and MAPPED maps inside an in-play draw, after the level load that
   allocates them and before the teardown, on the thread that runs that teardown:
   the teardown frees both, the LOS counters first (`0x464A00` at `0x491BA9`, then
   MAPPED through `0x483DD0` at `0x491BB3`; the lifetimes are spelled out at
   `wide_tick`). Every index into them is bounded by the maps' own dimensions, as
   the engine's own loop bounds it.

   HANDING IT OVER — IT DOES NOT (frame packet exchange). The render thread
   holds no pointer into this module: the packet's publisher copies the words
   out of it on THIS thread, in the same DrawGameScreen the pack ran in, and
   the render thread reads its own packet. One buffer, no lock, nothing
   retired, and no liveness test to answer — a frame either has the source in
   its packet or it has not.

   THE ORACLE. `tagpu_fogwide_check.on` compares, every 120 ticks, the builder's
   replication over the engine's own window from our bits with the grid the engine
   built (`differ=`), and the whole-map expansion with that replication
   (`mirror-differ=`) — see fogw_check. */

/* DllMain only. It creates nothing, but the module stays inert until this
   has run. */
void tagpu_fogwide_init(void);

/* Game thread, once per tracked in-play draw: from the fog-overlay call site
   when it is ours, else, on the Vulkan renderer, from the publisher's `after`
   (see WHERE IT RUNS). `ta` is the TAdynmem base; `rebuilt` is 1 when the
   engine's own grid was not current on this draw (its is-current flag had been
   cleared), which is also our cue that the LOS state moved under us. It packs
   only when `rebuilt` is set or the maps themselves changed — but `rebuilt` is
   cleared by every LOS stamp as well as every scroll, so in a live game that is
   the SIM TICK rate whenever anything is moving, ~30/s at gamespeed 10.

   TWO DIFFERENT RATES, and the pair reads as a contradiction if they are not
   kept apart. This function is called on the DRAW path, not the sim tick:
   `0x4848E0`'s sole call site is `0x469D8E` inside the per-frame world draw
   (VERIFIED by objdump against the pristine exe, 2026-09-11 — exactly one
   `call 0x4848e0` in the image). That path turns over at the `DrawGameScreen`
   rate, MEASURED at 443/s at 1920x1080 and ~900/s at 1024x768 against a
   presenter holding 60, and it keeps running with the sim PAUSED. What runs at
   the sim rate is the PACK, because `rebuilt` is cleared by LOS stamps and
   nothing stamps while the sim is stopped.

   AT EVERY ZOOM. The source does not depend on the level or the eye, so a
   frame drawn at any level from any eye finds its fog in it. */
void tagpu_fogwide_tick(char* ta, int rebuilt);

/* GAME THREAD, from the packet's publisher, in the same DrawGameScreen the tick
   above ran in. Fills `out` with the source to copy into this frame's packet and
   returns 1, or returns 0 when there is none: attach did not run, nothing has
   been packed yet, or a bail-out withdrew it. `out->words` is this module's and is read on this thread
   only. */
int tagpu_fogwide_source(TAGPU_FOGSRC* out);

#endif
