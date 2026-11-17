#ifndef TASIM_DEFS_H
#define TASIM_DEFS_H
/* tasim_defs.h -- the shared simulation core's definitions: what a unit type is, read out of the entry the sim plays
   (TASIM_DEFS.entry, the bound entry with a casting). research/notes/impai/shared-core.md names the core's modules;
   this is tasim_defs.

   ENGINE-FREE AND WITHOUT GLOBALS, as tasim_state.h states for the whole core: every accessor reads the TASIM_DEFS
   its caller passes and writes nothing. They are static inline because the core's rules read them in their inner
   loops. The definitions themselves are loaded by tagpu_wikidata.c. */
#include "tasim_state.h"

enum { KIND_NONE, KIND_MEX, KIND_GEO };

static inline const WIKIDEF* tasim_def(const TASIM_DEFS* defs, int t) { return &defs->entry->def[t]; }
static inline int tasim_fpx(const TASIM_DEFS* defs, int t) { return tasim_def(defs, t)->fx; }
static inline int tasim_fpz(const TASIM_DEFS* defs, int t) { return tasim_def(defs, t)->fz; }
static inline int tasim_mobile(const TASIM_DEFS* defs, int t) { return tasim_def(defs, t)->max_velocity > 0; }
/* a resource building: a mex extracts metal, a geothermal plant needs a vent under a G cell of its yard */
static inline int tasim_kind(const TASIM_DEFS* defs, int t) { return tasim_def(defs, t)->extracts > 0 ? KIND_MEX : tasim_def(defs, t)->vent ? KIND_GEO : KIND_NONE; }
/* footprint cell (dx, dz) of type t is a G cell: WIKIDEF.g holds the `g` a type
   lists, or every cell of a `vent` type with none. THE BOUND: the rows are WIKI_FP_MAX cells a
   side, and a type with G cells is held within them (the loader and the casting's reader refuse
   one past it), so a cell outside them is no G cell. */
static inline int tasim_g_cell(const TASIM_DEFS* defs, int t, int dx, int dz)
{
    return dx >= 0 && dz >= 0 && dx < WIKI_FP_MAX && dz < WIKI_FP_MAX && ((tasim_def(defs, t)->g[dz] >> dx) & 1);
}
static inline int tasim_can_build(const TASIM_DEFS* defs, const WIKIUNIT* u, int t) { return (int)((tasim_def(defs, u->type)->builds >> t) & 1); }

#endif
