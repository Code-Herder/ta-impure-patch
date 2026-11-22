#ifndef TAGPU_SCATTERGEO_H
#define TAGPU_SCATTERGEO_H
/* tagpu_scattergeo -- the geometry of clearing a build site, engine-free (research/notes/build-scatter-plan.md):
   where a unit standing on a blocked site goes. Pure functions of their arguments, in 16-px cells, with every
   buffer the caller's; tagpu_scatter.c fills the inputs from the engine and tests/scattertest.c from cases.

   THE BOX and a PLACE are tagpu_guardgeo.h's: every raster is the caller's box, w x h cells from first cell
   (x0, z0), row-major, and a place is the first (top-left) cell of a footprint. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_guardgeo.h"

/* seen: w h bytes; qx, qz: w h entries each */
typedef struct SgWork { uint8_t* seen; int16_t* qx; int16_t* qz; } SgWork;

/* THE WAY OFF A SITE for the unit `self` of footprint fx x fz standing at place (x, z): a breadth-first search,
   four-connected, from its own place over the places it can walk --
   - its footprint lies in the box;
   - `pass`, its class's content map (nonzero passable for a footprint whose first cell it is), holds the place;
   - its footprint covers no `wall` cell (a structure's held cell: `wsums`, gg_sums of the wall raster)
   -- to the nearest place where it can stand:
   - its footprint covers no `taken` cell (`tsums`: the site, a structure's whole footprint, another site, a place
     already given to another unit);
   - and no cell of `occ` holds a unit other than `self` (occ: the unit number standing on each cell, 0 none).
   The unit's own place is where the search starts, whatever the rasters hold there: it stands on it. Nearest is
   in steps; of two places as near, the one the queue reaches first, its neighbours taken in the fixed order
   north, west, east, south. 1 with (*ox, *oz), box-absolute; 0 when no such place is reached inside the box. */
int sg_way_off(const GgBox* box, int x, int z, int fx, int fz, unsigned self, const uint8_t* pass,
               const int32_t* wsums, const int32_t* tsums, const uint16_t* occ, const SgWork* wk, int* ox, int* oz);

/* the footprints fx x fz at (x, z) and gx x gz at (u, v) share a cell */
int sg_overlap(int x, int z, int fx, int fz, int u, int v, int gx, int gz);

#endif
