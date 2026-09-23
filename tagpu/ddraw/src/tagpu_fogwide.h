#ifndef TAGPU_FOGWIDE_H
#define TAGPU_FOGWIDE_H

/* tagpu_fogwide — a fog-of-war grid that spans the ZOOMED-OUT view.

   THE PROBLEM. Every native pass samples the engine's own screen fog grid
   (`*(main+0x1421F)`, terrain-depth.md §5.2): a view-anchored lattice of 32-px
   cells whose two bytes are 4-bit corner masks. That grid is allocated ONCE per
   map load, from the engine's viewport size — `0x483BB8`:

       cols = viewW/32 + (viewW % 32 ? 3 : 2)      rows likewise from viewH

   — and `0x4843C0` rebuilds it anchored at the eye, so it covers the 1x
   viewport and about two cells more. At zoom < 1 the native passes draw a world
   rect `vw/z` across; everything outside the engine's grid is off the lattice,
   the shaders' `taFog` clamps to the border cell, and the outer ring gets a
   SMEAR of the last row and column instead of fog. That is the bug: at zoom-out
   the grey LOS band stops partway across the screen.

   Neither half of the engine's grid can be moved. Its dimensions come from a
   map-load allocation, and its origin is recomputed from `main+0x1431F/0x14323`
   inside the builder itself — lying to it about the eye would have to be undone
   before the render thread reads the same field, which is a race with the whole
   world's position on the wrong side of it.

   SO WE BUILD OUR OWN, over whatever window we choose, by replicating
   `0x4843C0` exactly (see fogw_build): the same per-map-cell test of the LOCAL
   player's LOS counter and the MAPPED bitmap, the same four-corner OR into the
   surrounding grid entries, the same four map-border completions. It is the
   same lattice — origin `32·col0 + 16` — so nothing downstream changes but the
   numbers in `uFogOrg`/`uFogDim`.

   WHERE IT RUNS. On the GAME thread, from `tagpu_terrown.c`'s `terr_fogtick`,
   which is the engine's own fog-overlay call site and already calls the engine's
   builder there. That is the lifetime argument for reading the LOS and MAPPED
   maps at all: at this call site the engine's builder reads exactly the same two
   allocations, so a pointer we could not read is one the engine could not read
   either. Every index into them is bounded by the maps' own dimensions, as the
   engine's own loop bounds it.

   WHAT IT COVERS. The window is sized for the WIDEST view the zoom levers can
   produce — `tagpu_zoom_min()`, not the current level — plus a margin. The
   level the game thread can see is the one the render thread published on the
   previous frame, so sizing for the current level would leave the ring bare for
   a frame whenever the zoom eased outward; sizing for the whole range means no
   step of any lever can outrun the grid.

   HANDING IT OVER — IT DOES NOT (frame packet exchange). The render thread
   holds no pointer into this module: the packet's publisher copies the grid
   out of it on THIS thread, in the same DrawGameScreen the build ran in, and
   the render thread reads its own packet. One buffer, no lock, nothing
   retired, and no liveness test to answer — a frame either has a wide grid in
   its packet or it has not.

   HOW BIG. Sized from the window the screen asks for, not from a constant — see
   THE STATE in tagpu_fogwide.c for why the size is taken at the worst eye
   residue. It is grown, never shrunk, and the old block is freed on the spot:
   with nothing outside this thread reading it, the size is an allocation
   question again rather than a lifetime one. */

/* The sanity bound the PUBLISHER applies to the engine's own grid descriptor —
   three numbers read out of engine memory, where the question is "has this
   struct been corrupted", not "how big may a grid be". It stays fixed and
   generous: the engine builds one cell per 32 px of ITS viewport plus two, so
   this is a viewport 32,000 px wide and cannot be reached by a screen. What
   bounds the CONSUMER is not this at all — it is the packet's
   own `len == cols * rows * 2`, checked once at acquire, which is exact. */
#define FOGW_ENGINE_DIMCAP 1024

/* DllMain only. It creates nothing, but the module stays inert until this
   has run. */
void tagpu_fogwide_init(void);

/* Game thread, from the fog-overlay call site, once per engine frame. `ta` is
   the TAdynmem base; `rebuilt` is 1 when the engine's own grid was rebuilt on
   this tick (its is-current flag had been cleared), which is also our cue that
   the LOS state moved under us. It rebuilds only when `rebuilt` is set or the
   window itself moved — but `rebuilt` is cleared by every LOS stamp as well as
   every scroll, so in a live game that is the SIM TICK rate whenever anything
   is moving, ~30/s at gamespeed 10 (see the tick's own comment for the number).

   TWO DIFFERENT RATES, and the pair reads as a contradiction if they are not
   kept apart. This function is called on the DRAW path, not the sim tick:
   `0x4848E0`'s sole call site is `0x469D8E` inside the per-frame world draw
   (VERIFIED by objdump against the pristine exe, 2026-09-11 — exactly one
   `call 0x4848e0` in the image). That path turns over at the `DrawGameScreen`
   rate, MEASURED at 443/s at 1920x1080 and ~900/s at 1024x768 against a
   presenter holding 60, and it keeps running with the sim PAUSED. What runs at
   the sim rate is the REBUILD, because `rebuilt` is cleared by LOS stamps and
   nothing stamps while the sim is stopped.

   AT EVERY ZOOM. It does not ask what the level is: the level is
   the render thread's to publish, and that thread is the one that decides,
   mid-frame, to draw the first zoomed-out frame of a gesture — so a producer
   gated on it is a producer that is always one tick late exactly when it
   matters. Which grid a FRAME uses is the consumer's decision, below. */
void tagpu_fogwide_tick(char* ta, int rebuilt);

/* GAME THREAD, from the packet's publisher, in the same DrawGameScreen the tick
   above ran in. Hands back the grid to copy into this frame's packet, or 0 when
   there is none: the module is off (`tagpu_fogwide.off`), attach did not run,
   nothing has been built yet, or a bail-out withdrew it. The consumer then uses
   the engine's own grid.

   WHICH GRID A FRAME USES IS THE CONSUMER'S DECISION:
   the packet carries both, and the native pass takes the wide one only while it
   is drawing at zoom < 1 or from an eye the game thread has not acknowledged.
   At zoom >= 1 the engine's own grid spans the frame by construction. */
int tagpu_fogwide_current(const unsigned short** buf,
                          int* cols, int* rows, int* orgX, int* orgY);

#endif
