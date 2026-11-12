#ifndef TASIM_DEFS_H
#define TASIM_DEFS_H
/* tasim_defs.h -- the shared simulation core's definitions: what a unit type is, read out of the entry the sim plays
   (WIKISIM.entry, the bound entry with a casting) and, for the pages a type shows, out of the sidebar's layout.
   research/notes/impai/shared-core.md names the core's modules; this is tasim_defs.

   ENGINE-FREE AND WITHOUT GLOBALS, as tagpu_wikisim.h states for the whole core: every accessor reads the WIKISIM its
   caller passes and writes nothing. They are static inline because the core's rules read them in their inner loops.
   The definitions themselves are loaded by tagpu_wikidata.c. */
#include "tagpu_wikisim.h"

enum { KIND_NONE, KIND_MEX, KIND_GEO };

static inline const WIKIDEF* tasim_def(const WIKISIM* s, int t) { return &s->entry->def[t]; }
static inline int tasim_fpx(const WIKISIM* s, int t) { return tasim_def(s, t)->fx; }
static inline int tasim_fpz(const WIKISIM* s, int t) { return tasim_def(s, t)->fz; }
static inline int tasim_mobile(const WIKISIM* s, int t) { return tasim_def(s, t)->max_velocity > 0; }
/* a resource building: a mex extracts metal, a geothermal plant needs a vent under a G cell of its yard */
static inline int tasim_kind(const WIKISIM* s, int t) { return tasim_def(s, t)->extracts > 0 ? KIND_MEX : tasim_def(s, t)->vent ? KIND_GEO : KIND_NONE; }
/* footprint cell (dx, dz) of type t is a G cell: WIKIDEF.g holds the `g` a type
   lists, or every cell of a `vent` type with none. THE BOUND: the rows are WIKI_FP_MAX cells a
   side, and a type with G cells is held within them (the loader and the casting's reader refuse
   one past it), so a cell outside them is no G cell. */
static inline int tasim_g_cell(const WIKISIM* s, int t, int dx, int dz)
{
    return dx >= 0 && dz >= 0 && dx < WIKI_FP_MAX && dz < WIKI_FP_MAX && ((tasim_def(s, t)->g[dz] >> dx) & 1);
}
static inline int tasim_can_build(const WIKISIM* s, const WIKIUNIT* u, int t) { return (int)((tasim_def(s, u->type)->builds >> t) & 1); }
/* the layout's pages of a type, NULL for none or with the sidebar off. A unit's page state is the
   engine's unit record's (tagpu_wikisim.h, WIKIUNIT.bp), so the core sets it as a unit enters. */
static inline const WIKIPAGES* tasim_type_pages(const WIKISIM* s, int t)
{
    return s->lay && s->ltype[t] >= 0 ? &s->lay->type[s->ltype[t]] : NULL;
}
static inline int tasim_npages(const WIKISIM* s, int t) { const WIKIPAGES* p = tasim_type_pages(s, t); return p ? p->npage : 0; }

#endif
