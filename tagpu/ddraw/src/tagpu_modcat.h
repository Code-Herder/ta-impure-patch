#ifndef TAGPU_MODCAT_H
#define TAGPU_MODCAT_H
/* tagpu_modcat -- what a mod's entry in the catalog says (mods/catalog.json, written by
   tools/modpack/modpack.py and embedded as MODPAK; research/notes/mod-manager-plan.md).

   Pure C99 and the C library, so tools/modpack/test_modpack.py compiles it natively. */
#include <stddef.h>

#define MODCAT_REASON 120

/* A mod's play options: whether it allows each and what its default is. A reason says why one is
   off, for the player. */
typedef struct {
    int  manual, radius, maximum;       /* resource_snapping: allowed, default and most radius */
    int  quick, quick_default;          /* quick_build: allowed, default                        */
    char manual_reason[MODCAT_REASON];
    char quick_reason[MODCAT_REASON];
} MODRULES;

#define MODRULES_RADIUS_MAX 8

/* Mod `id`'s options from the catalog text. 1 with `out` filled, 0 when no entry names it, -1 with
   the reason in `err` when its options are missing or out of range: a radius beyond
   MODRULES_RADIUS_MAX, a default above its maximum, or an option off with no reason given. */
int tagpu_modcat_rules(const char* catalog, size_t n, const char* id, MODRULES* out, char* err, size_t errn);

#endif
