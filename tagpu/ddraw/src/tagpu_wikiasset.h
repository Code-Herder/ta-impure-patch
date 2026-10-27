#ifndef TAGPU_WIKIASSET_H
#define TAGPU_WIKIASSET_H
/* tagpu_wikiasset -- the in-game wiki's 3D assets, read from the player's own archives by the
   engine's rules on a loader thread of their own, into one arena (research/notes/
   ingame-wiki-3d-stage-plan.md, sections 4, 5 and 8, landing 6e).

   WHAT IT LOADS. For every entry of the wiki's data, its stage description (art/wiki/stage/
   <entry>.json in WIKIPAK): each source map's TNT opened by the engine's path rule and its header
   checked, its tile count compared, and only the tiles the description names read and their
   FNV-1a 32 compared; each feature section from the first features\**\*.tdf that holds it (in the
   engine's enumeration order), with its sequences (a 2D feature) or its model (a 3D one). And for
   every unit type any entry defines: units\<name>.FBI by path, as a level opens it; the model
   objects3d\<Objectname, else UnitName>.3DO; the script scripts\<UnitName>.COB, and from it the
   pieces Create() hides (tagpu_cobscan_mask, the build ghost's rule). The folders' names are the
   running executable's (Escalation's `unitsE`, TA Zero's `ZUnits` and `logoz`). And the game's
   own cursors, <anims>\cursors.GAF's cursornormal and cursormove, which the stage draws its
   pointer with.

   THE MODELS ARE BUILT AS THE ENGINE HOLDS THEM (the scout's batch A4, DISASSEMBLED): the file's
   object and primitive structs with every offset turned into a pointer into the arena, the
   selection primitive swapped to face 0 with the node's +0x0C set to 0 (0x4CB370), faces 1.. sorted
   by their mean vertex Y as the engine's bubble sort leaves them, X and Z negated (0x4CB590), and
   every textured face bound as 0x42A140 binds it: the first texture GAF in the bank's order
   holding the name, else logos.GAF (FF_TEAM when that sequence has exactly 10 frames), else the
   flat colour 0xD1 with FF_FLAT; a sequence of more than one frame an anim state with FF_ANIM, one
   frame its GAFFrame. The sequences and frames a face or feature names are copied, not whole GAFs,
   each in the engine's in-memory form (0x4B8C60: the file's structs, rebased, nothing decoded). So
   tagpu_posebake.c, tagpu_render3do.c and tagpu_gaf.c read a model of the arena exactly as they read
   an engine template, with no line of theirs changed.

   THE INVARIANTS.
     - EVERY OFFSET IN A FILE IS DATA UNTIL IT IS BOUNDED AGAINST THAT FILE: the readers below it
       (tagpu_hpi.h, tagpu_tdf.h) and every structure built here check each offset, count and index
       against the blob it came from before reading through it; what fails is refused per element,
       named in the report, and logged.
     - THE ARENA IS ONE BLOCK, IMMUTABLE ONCE PUBLISHED AND NEVER FREED. It is reserved whole and
       refused unless it lies inside (0x600000, tagpu_user_top()], the readers' range filter
       (tagpu_m3_ptr_ok), so every pointer in it passes that filter by construction. Every pointer
       field in it is one the loader wrote, to a place inside it: a word of a file that a reader
       follows as a pointer (a face's texture state) is never copied. Everything the
       render thread may read -- the WIKI3D below, the stages, the models, the frames, the report --
       lives in it, written by the loader alone before the one interlocked store that publishes it.
     - THE LOADER READS NO ENGINE MEMORY AND PAINTS NOTHING. Its inputs arrive by value (the mount
       list, the current directory, the language and the folder names, copied on the game thread at
       the wiki's first open; the wiki's data, immutable and never freed); it opens its own
       read-only handles; it touches no atlas and no device (tagpu_gaf.c decodes through static
       buffers, so painting is the render thread's).

   A missing map, tile or feature makes that stage's ground fall back whole (the plan's section 3);
   a unit type whose model does not resolve is drawn as its whitebox box. Nothing here crashes on a
   file: the fuzz pass of tests/wikiassettest.c holds that. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_vfs.h"
#include "tagpu_wikidata.h"

#define WIKI3D_NAMEMAX  32              /* a feature section, a unit type, a folder: the engine's 0x20 */

/* THE ENGINE'S NAMES FOR THE ASSET FOLDERS, as the running executable spells them: a mod renames
   them in place in its .data (the scout's batch D2), and the language is the registry's, "english"
   when it gives none. Copied on the game thread (tagpu_replays_asset_names). */
typedef struct WIKI3DNAMES {
    char language[64];
    char units[WIKI3D_NAMEMAX], textures[WIKI3D_NAMEMAX], logos[WIKI3D_NAMEMAX],
         logos_gaf[WIKI3D_NAMEMAX], objects3d[WIKI3D_NAMEMAX], scripts[WIKI3D_NAMEMAX],
         anims[WIKI3D_NAMEMAX], features[WIKI3D_NAMEMAX];
} WIKI3DNAMES;

/* What the load reads, all by value except `data`, which is immutable and never freed. */
typedef struct {
    char             cwd[TAGPU_VFS_PATHMAX];
    int              narc;
    char             arc[TAGPU_VFS_ARCMAX][TAGPU_VFS_PATHMAX];  /* the mount list, in mount order */
    WIKI3DNAMES      names;
    const WIKIDATA*  data;
    const unsigned char* pak;           /* WIKIPAK: the stage descriptions                      */
    size_t           npak;
} WIKI3DIN;

/* A feature of a stage, resolved. The sequences are the engine's in-memory form (TAGPU_SQ_*,
   TAGPU_GF_*); `model` a Model3DONode tree (tagpu_model3do.h). */
typedef struct {
    char        name[WIKI3D_NAMEMAX];
    int         cx, cz;                 /* its anchor cell, as the description places it        */
    int         fx, fz, height;         /* footprintx, footprintz, height (LoadFeature 0x4224B0) */
    uint32_t    flags;                  /* the def's +0xFE bits as LoadFeature sets them: bit 0 a
                                           2D feature, 1 animating, 2 animtrans, 3 shadtrans,
                                           4 flamable, 5 geothermal, 6 blocking, 7 reclaimable,
                                           8 autoreclaimable, 9 indestructible, 10 nodisplayinfo,
                                           11 nodrawundergray                                   */
    const void* seq;                    /* seqname's sequence, or NULL                           */
    const void* seqshad;                /* seqnameshad's, or NULL                                */
    const char* model;                  /* a 3D feature's root node, else NULL                   */
    int         ok;
} WIKI3DFEAT;

/* A stage's ground. When `ok` is 0 nothing of it is drawn in 3D: the whitebox's ground is. */
typedef struct {
    char              entry[WIKI_ID_MAX];
    int               ok;
    int               w, h;             /* cells; the tiles are w/2 x h/2                       */
    int               ntile;
    const unsigned char* tiles;         /* ntile x 1024 bytes, 32 x 32 palette indices each, in
                                           the description's tileset order                        */
    const uint16_t*   tilemap;          /* (w/2) x (h/2), row-major, indices of `tiles`          */
    int               nfeat, nfeat3d;
    const WIKI3DFEAT* feat;             /* in the description's order                            */
} WIKI3DSTAGE;

#define WIKI3D_MAXPIECE 256             /* TAGPU_PBMAXPIECE: the readers' piece bound            */

/* A unit type, resolved. `model` is NULL when it did not: its whitebox box is drawn.
   `hidden` is what its Create() hides (tagpu_cobscan_mask, the build ghost's rule): bit g set when
   node g of tagpu_model_walk's walk of `model` is hidden. `hides` is 1 with bits set, 0 when
   nothing is hidden (no script, no Create(), no hide), -1 when the script or the model is refused
   (then nothing is hidden either: every piece shows). */
typedef struct {
    char        name[WIKI_ID_MAX];
    const char* model;
    int         ok;
    int         hides;
    uint32_t    hidden[WIKI3D_MAXPIECE / 32];
    /* what Create() marks dont-cache (tagpu_cobscan_create_mask, TAGPU_COB_DONTCACHE), as
       `hidden` and with `uncaches` read as `hides`: a piece set here casts no structure shadow */
    int         uncaches;
    uint32_t    uncached[WIKI3D_MAXPIECE / 32];
    /* THE FBI'S FACTS the stage draws a unit by, read with the engine's integer rule:
       `mobile` unless BMcode is 0; `slant` -- the structure shadow's branch -- for a structure that
       is not a Digger [INFERRED: the unit's structure bit +0x110 0x20000000 follows BMcode 0, as
       tools/tascene takes it; tagpu_native.c `slant` tests that bit]; `shadow` unless NoShadow,
       and for a mobile unit unless CanHover or Floater (tagpu_native.c, the engine's FBI gates). */
    uint8_t     mobile, slant, shadow;
} WIKI3DUNIT;

typedef struct {
    int                nstage, nunit;
    const WIKI3DSTAGE* stage;
    const WIKI3DUNIT*  unit;
    const char*        report;          /* the JSON tagpu_wiki3d.dump writes, NUL-terminated     */
    /* the game's cursor each shape shows, by WIKI_CURSOR_* (tagpu_wikisim.h): a sequence of the
       player's <anims>\cursors.GAF in the engine's form (cursornormal, cursormove, cursornormal;
       arrow and build share one copy), or NULL when it did not resolve */
    const void*        cursor[3];
    uint32_t           base;            /* the arena's address as its pointer fields hold it     */
    uint32_t           bytes;           /* the arena's bytes in use, from `base`                 */
} WIKI3D;

#ifdef _WIN32
/* The load, started once on a thread of its own with `in`, which it takes and frees; later calls
   free `in` and return. The game thread, at the wiki's first open. */
void          tagpu_wikiasset_start(WIKI3DIN* in);

/* The published load, or NULL while it runs and for good when it failed before publishing (the
   log says why). Any thread; immutable once returned and never freed. */
const WIKI3D* tagpu_wiki3d_get(void);

/* TRIGGER: `tagpu_wiki3d.dump` in the game's folder writes the report to `tagpu_wiki3d.json` once
   the load is published (until then the trigger waits). The game thread, from the wiki's fill. */
void          tagpu_wikiasset_poll(void);
#endif

/* The load itself, on the calling thread, into an arena at `base` of `cap` bytes whose pointer
   fields hold `base32` + offset: the DLL passes its reserved block and its own address; the native
   test passes host memory and a stand-in address, since its pointers are not 32-bit. `commit`
   makes the block usable up to `upto` bytes and answers how far it now is, 0 when it cannot; NULL
   when the whole block already is. Returns the WIKI3D, which lies inside the arena past its first
   bytes, or NULL when not even the report fit. */
const WIKI3D* tagpu_wiki3d_load(const WIKI3DIN* in, unsigned char* base, uint32_t base32, uint32_t cap,
                                uint32_t (*commit)(unsigned char* base, uint32_t upto));

#endif
