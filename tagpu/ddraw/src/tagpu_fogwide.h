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

   HANDING IT OVER. Three buffers and three pointers — build, published, held —
   swapped under a critical section, never copied and never reallocated (they are
   allocated once, at the cap). Each pointer belongs to exactly one party at a
   time and a swap only ever exchanges two of them, so the game thread cannot
   write the buffer the render thread is reading. The render thread keeps its
   held buffer for the whole frame, which is what the CPU-side gates
   (`tagpu_fog_at`) need. */

/* Game thread, from the fog-overlay call site, once per engine frame. `ta` is
   the TAdynmem base; `rebuilt` is 1 when the engine's own grid was rebuilt on
   this tick (its is-current flag had been cleared), which is also our cue that
   the LOS state moved under us. Cheap when nothing changed: it rebuilds only
   when `rebuilt` is set or the window itself moved. */
void tagpu_fogwide_tick(char* ta, int rebuilt);

/* Render thread, once per frame, before the fog texture upload. Hands back the
   grid to sample — the buffer stays valid until the next call on this thread —
   or 0 when there is none to use: the module is off, no zoomed-out view is
   live, nothing has been published, or the game thread has stopped ticking
   (terrain ownership disarmed, the menus). The caller then uses the engine's own
   grid exactly as before. */
int tagpu_fogwide_get(const unsigned short** buf,
                      int* cols, int* rows, int* orgX, int* orgY);

#endif
