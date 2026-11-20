#ifndef TAGPU_GUARDGEO_H
#define TAGPU_GUARDGEO_H
/* tagpu_guardgeo -- the geometry of guarding a factory, engine-free (research/notes/factory-guard-plan.md):
   the yard's exits, a lane out for each ground class the plant builds, and seats for the builders that guard it.
   Pure functions of their arguments, in 16-px cells, with every buffer the caller's; tagpu_guard.c fills the
   inputs from the engine and tests/guardtest.c from cases.

   THE BOX. Every raster is the caller's box, w x h cells from first cell (x0, z0), row-major. A place is the
   first (top-left) cell of a footprint; a footprint of fx x fz at place (x, z) covers [x, x + fx) x [z, z + fz). */
#include <stddef.h>
#include <stdint.h>

typedef struct GgRect { int16_t x0, z0, x1, z1; } GgRect;   /* inclusive cells */
typedef struct GgBox  { int x0, z0, w, h; } GgBox;

/* THE EXITS of a yard map, fx x fz bytes row-major: each footprint edge holding a cell a product is built on and
   walks off -- a yard byte with 0x04 and without 0x02, which the stamp 0x47CC30 files only while the yard is
   closed (impai_build.c, THE WAY OUT). Edge 0 the top row (z = 0), 1 the bottom, 2 the left column, 3 the right;
   [a, e] the run along the edge that holds such cells. */
typedef struct GgExits { int n; uint8_t edge[4]; int16_t a[4], e[4]; } GgExits;
void gg_exits(const uint8_t* yard, int fx, int fz, GgExits* out);

/* The blocked raster as sums (THE BLOCKER RASTER of impai_build.c): sums[(z + 1) (w + 1) + x + 1] counts the
   blocked cells of [0, x] x [0, z], box-relative, so a footprint's test is four reads. `blocked` is w x h, nonzero
   blocked; `sums` holds (w + 1) (h + 1). */
void    gg_sums(const uint8_t* blocked, int w, int h, int32_t* sums);
int32_t gg_count(const int32_t* sums, int w, int x, int z, int fx, int fz);

/* THE SQUARE'S FIT, in place over a class's content map of the box (nonzero passable for a footprint whose first
   cell it is): a place becomes 1 when every entry of [x, x + ex] x [z, z + ez] is passable, 0 otherwise, an entry
   past the box counting blocked. impai_build.c's square_fit. */
void gg_square_fit(uint8_t* m, int bw, int bh, int ex, int ez);

/* THE LANE of one ground class: a breadth-first search, four-connected, over the places where the class's
   largest product, an s x s square, fits -- `fit`, the class's map after gg_square_fit(s - cfx, s - cfz) --
   covers no blocked cell (`sums`) and does not overlap the plant, from every place just beyond an exit's run, to
   the first place whose square stands `open` cells clear of the plant. The path's squares, each grown by
   `margin` cells and clipped to the box, are added to `out` as rectangles, a square joining the rectangle before
   it while their union has at most twice the cells its squares cover (impai_build.c, way_rects). `start` is the
   exit to try first (its index in L), -1 none: starts are queued in that exit's order first, so of two equally
   short paths the preferred exit's is found. Work: `seen` w h bits / 8 + 1 bytes, `qx`, `qz`, `qf` w h entries
   each, `sums` (w + 1) (h + 1). 1 with the lane added; 0 no path, or `out` full (cap rectangles). */
typedef struct GgWork { uint8_t* seen; int16_t* qx; int16_t* qz; int32_t* qf; int32_t* sums; } GgWork;
int gg_lane(const GgBox* box, const GgRect* plant, const GgExits* L, int start, int s, int open, int margin,
            const uint8_t* fit, const int32_t* sums, const GgWork* wk, GgRect* out, int* nout, int cap);

/* the rectangle r overlaps the footprint fx x fz at place (x, z) */
int gg_overlaps(const GgRect* r, int x, int z, int fx, int fz);

/* THE BAND a helper builds from. HelpBuild's goal (0x44D3B0, vtable 0x4FD358) is an annulus around the frame's
   centre: with G the place the helper would have were it centred on the frame, the helper at place P is in
   range iff (rin/16)^2 <= |P - G|^2 <= (rout/16)^2, the divisions truncating, where rin = trunc((int)(16
   sqrt(fx^2 + 2 fz)) / 2) of the FRAME's footprint and rout = builddistance + rin (0x403FF9..0x404045,
   0x44D3BF..0x44D434, 0x44D800). The guard's seats lie in it for every product of the plant (tagpu_guard.c). */
int  gg_rin(int fx, int fz);
/* G for a helper of footprint (bfx, bfz) and a frame centre (cx, cz), 16.16 world px: ((c - (bf << 19) +
   0x80000) >> 20), the shift arithmetic as the engine's `sar` */
int  gg_goal_place(int32_t c, int bf);

/* ONE BUILDER'S SEAT SEARCH. A place (x, z) is a seat for the builder when
   - its footprint lies in the box and `pass` (the builder's class map, nonzero passable) holds it;
   - its footprint covers no cell of `taken` (nonzero: the plant, a structure, a unit standing still, a lane,
     another builder's seat);
   - |(x, z) - (gx, gz)|^2 lies in [r2min, r2max];
   - and a way leads out of the box for it and for every builder already seated: from its place, over places
     its own footprint fits on `pass` and covers no `taken` cell but its own seat's, four-connected, to a place
     whose footprint touches the box's edge (THE WAY OUT FOR A SEAT, tagpu_guardgeo.c).
   The places are ranked by r2max - |(x, z) - (gx, gz)|^2, the band's outer edge first, then z, then x, and the
   first that is a seat is taken. `seated` lists the builders already seated: their places, footprints and
   class maps (`spass[k]`). At most `budget` candidates are tested for the way out; 1 with (*sx, *sz), 0 none
   found within it. Work: `wk` as gg_lane's, and `taken2` w h bytes. */
typedef struct GgSeated { int x, z, fx, fz; const uint8_t* pass; } GgSeated;
int gg_seat(const GgBox* box, int fx, int fz, const uint8_t* pass, const uint8_t* taken, int gx, int gz,
            int r2min, int r2max, const GgSeated* seated, int nseated, int budget, const GgWork* wk,
            uint8_t* taken2, int* sx, int* sz);

#ifdef GUARDGEO_TEST
/* A way out of the box for a builder of footprint fx x fz at place (x, z), over `pass` and around `taken`, its own
   footprint's cells excepted: 1 found. Work: `wk` as gg_lane's. Only the self-test asks it from outside
   (tests/guardtest.py defines GUARDGEO_TEST): gg_seat makes the same test itself, so no DLL build compiles it. */
int gg_way_out(const GgBox* box, int x, int z, int fx, int fz, const uint8_t* pass, const uint8_t* taken,
               const GgWork* wk);
#endif

/* THE WAIT SPOT: the place nearest (x, z) -- rings of growing Chebyshev radius up to `rmax`, each walked from its
   top-left corner clockwise -- whose footprint lies in the box, fits `pass` and covers no `taken` cell. 1 with
   (*wx, *wz); 0 none. */
int gg_wait_spot(const GgBox* box, int x, int z, int fx, int fz, const uint8_t* pass, const uint8_t* taken,
                 int rmax, int* wx, int* wz);
#endif
