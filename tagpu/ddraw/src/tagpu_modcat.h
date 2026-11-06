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

/* What the Mods screen says of a mod: UTF-8, each cut to its array. */
typedef struct {
    char name[48], author[48], version[32], summary[400];
    int  is_mutator;
    char base[32];              /* a mutator's base mod, "none" for the folder's own game */
} MODABOUT;

/* Mod `id`'s entry for the screen. 1 with `out` filled, 0 when no entry names it. */
int tagpu_modcat_about(const char* catalog, size_t n, const char* id, MODABOUT* out);

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
    int     is_mutator;         /* its package is one archive: no recipe runs */
} MODINSTALL;

/* What it takes to install mod `id`. 1 with `out` filled, for tagpu_modcat_install_free; 0 with the reason in `err`
   when no entry names it or the entry is not one to act on: an id that is not a plain name, no version, no package, a hash that
   is not 64 hex digits, a size past 2 GB, or a file whose path tagpu_modtar would refuse. */
int  tagpu_modcat_install(const char* catalog, size_t n, const char* id, MODINSTALL* out, char* err, size_t errn);
void tagpu_modcat_install_free(MODINSTALL* m);

/* `fn` with each entry's id that is a plain name; 0 when the catalog does not parse. */
int  tagpu_modcat_each_id(const char* catalog, size_t n, void (*fn)(void* ctx, const char* id), void* ctx);

/* ---- a mutator ----------------------------------------------------------------------------------

   Game files layered over one base mod, ahead of everything it mounts. Every path here has '\\' for its
   separators, as the engine's own lookup splits at (0x4BB4E0); the catalog spells them with '/'. */
#define MODCAT_PATH 96

typedef struct {
    char    path[MODCAT_PATH];
    int     replaces;           /* the base holds this path, and `sha` is that file's when packed */
    uint8_t sha[32];
} MODMUTFILE;

typedef struct {
    char        id[32], base[32];
    MODMUTFILE* files;
    int         nfiles;
    char      (*hides)[MODCAT_PATH];    /* the base's files it takes away */
    int         nhides;
} MODMUTATOR;

/* A file a mod's package holds different bytes of from its drop-in folder, and the game finds the same in: settle_places renames a section so the engine's
   order reaches the drop-in's winners. The fingerprint counts `here` as `dropin`, so one mod has one print however it is installed. */
typedef struct {
    char    path[MODCAT_PATH];
    uint8_t here[32], dropin[32];
} MODSAMEAS;

/* Mod `id`'s, malloc'd, with the count in `count`. NULL with none, or if any of them is one nothing here could act on. */
MODSAMEAS* tagpu_modcat_same_as(const char* catalog, size_t n, const char* id, int* count);

/* Mutator `id`, for tagpu_modcat_mutator_free. 1 with `out` filled, 0 when no entry names it, it is no mutator, or a
   path or hash in it is one nothing here could act on: too long, unsafe, or with a character a profile's key cannot hold. */
int  tagpu_modcat_mutator(const char* catalog, size_t n, const char* id, MODMUTATOR* out);
void tagpu_modcat_mutator_free(MODMUTATOR* m);

/* The same path as the engine's lookup takes it: without case, '/' and '\\' alike. */
int  tagpu_modcat_same_path(const char* a, const char* b);

/* What the player has switched off: 1 for a path the switch `file.<path>` has off. NULL: nothing is off. */
typedef int (*MODCAT_OFF)(void* ctx, const char* path);
/* The file the base answers for a path: 1 with its SHA-256, 0 when it holds none. */
typedef int (*MODCAT_HASH)(void* ctx, const char* path, uint8_t out[32]);

/* Every file `m` replaces that the base no longer holds as it was packed against, whatever the switches say, so a verdict
   survives a switch: into `out`, up to `max`; returns how many there are. New files and hides are not checked. */
int  tagpu_modcat_stale(const MODMUTATOR* m, MODCAT_HASH hash, void* ctx, char (*out)[MODCAT_PATH], int max);
/* The first of that list a switch has not taken out of play, or NULL: with `total` past `max` a file past the list may be the
   one in play, so the answer is never NULL there. */
const char* tagpu_modcat_stale_live(char (*list)[MODCAT_PATH], int total, int max, MODCAT_OFF off, void* ctx);
/* The first path two mutators of one base cannot both have, or NULL: a file both carry, or one's file the other hides. A path
   a switch has off is in neither. */
const char* tagpu_modcat_clash(const MODMUTATOR* a, const MODMUTATOR* b, MODCAT_OFF off, void* ctx);

#endif
