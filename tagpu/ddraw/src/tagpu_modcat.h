#ifndef TAGPU_MODCAT_H
#define TAGPU_MODCAT_H
/* tagpu_modcat -- what a mod's entry in the catalog says (mods/catalog.json, written by
   tools/modpack/modpack.py and embedded as MODPAK; research/notes/mod-manager-plan.md).

   Pure C99 and the C library, so tools/modpack/test_modpack.py compiles it natively. */
#include <stddef.h>
#include "tagpu_modtar.h"

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

/* 1 when mod `id`'s `hide` list names the archive `name`, without case. */
int tagpu_modcat_hidden(const char* catalog, size_t n, const char* id, const char* name);

/* 64 lower-case hex digits and the end of the text into `out`; 0 for anything else. */
int tagpu_modcat_hash(const char* text, uint8_t out[32]);

/* A package of a mod, as the catalog declares it: where to fetch it and what the download and each file in it must be. */
typedef struct {
    char*        url;
    uint32_t     size;
    uint8_t      sha256[32];
    MODTAR_FILE* files;
    int          nfiles;
} MODPKG;

typedef struct {
    char*   id;
    char*   version;
    char*   base_url;           /* the catalog's own, "" until there is a place to name */
    MODPKG* packages;
    int     npackages;
} MODINSTALL;

/* What it takes to install mod `id`. 1 with `out` filled, for tagpu_modcat_install_free; 0 with the reason in `err`
   when no entry names it or the entry is not one to act on: an id that is not a plain name, no version, no package, a hash that
   is not 64 hex digits, a size past 2 GB, or a file whose path tagpu_modtar would refuse. */
int  tagpu_modcat_install(const char* catalog, size_t n, const char* id, MODINSTALL* out, char* err, size_t errn);
void tagpu_modcat_install_free(MODINSTALL* m);

/* `fn` with each entry's id that is a plain name; 0 when the catalog does not parse. */
int  tagpu_modcat_each_id(const char* catalog, size_t n, void (*fn)(void* ctx, const char* id), void* ctx);

#endif
