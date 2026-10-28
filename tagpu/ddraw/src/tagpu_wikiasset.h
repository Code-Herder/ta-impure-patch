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

/* THE ENGINE'S NAMES FOR THE ASSET FOLDERS, as the running process spells them: a mod renames
   them in place in its .data, in the executable file (Escalation, TA Zero: the scout's batch D2)
   or at run time from its loader DLL's DllMain, before the engine reads them (Mayhem's and ProTA's
   gui, unitpics, gamedata and download: the sidebar scout's Q6), and the copy is taken from the
   running process, so it carries both. The language is the registry's, "english" when it gives
   none. Copied on the game thread (tagpu_replays_asset_names). The sidebar's are `gui` to
   `guipal`; `palette` is the palette files' base name and `guipal` the GUI palette's. */
typedef struct WIKI3DNAMES {
    char language[64];
    char units[WIKI3D_NAMEMAX], textures[WIKI3D_NAMEMAX], logos[WIKI3D_NAMEMAX],
         logos_gaf[WIKI3D_NAMEMAX], objects3d[WIKI3D_NAMEMAX], scripts[WIKI3D_NAMEMAX],
         anims[WIKI3D_NAMEMAX], features[WIKI3D_NAMEMAX];
    char gui[WIKI3D_NAMEMAX], gamedata[WIKI3D_NAMEMAX], unitpics[WIKI3D_NAMEMAX],
         download[WIKI3D_NAMEMAX], commongui[WIKI3D_NAMEMAX], palettes[WIKI3D_NAMEMAX],
         palette[WIKI3D_NAMEMAX], guipal[WIKI3D_NAMEMAX];
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
    /* THE SIDEBAR'S MINIMAP PICTURE of this stage (tagpu_wikiside_minimap), mm_w x mm_h palette
       indices at (mm_offx, mm_offy) of the 128 x 128 square, shrunk with the sidebar's `alp`; NULL
       when the ground did not resolve or the sidebar was refused (the minimap is the whitebox's) */
    const unsigned char* minimap;
    int               mm_w, mm_h, mm_offx, mm_offy;
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

/* ---- THE MOD'S LEFT SIDEBAR ----------------------------------------------------------------------
   The running mod's sidebar as the wiki's stage draws it: its layout (WIKILAYOUT, tagpu_wikidata.h)
   and its art, read from the mod's files by the engine's rules. tools/wiki/gfx.py's side pack
   (`--side-only`) is the same resolution in Python, and tools/wiki/assetcheck.py holds the two
   equal per compat setup; the spec is .data/wiki-sidebar/spec.md sections 1, 2 and 7.

   WHAT IT READS, every file by the engine's path rule (the language's folder first, a 0-byte file
   missing, as 0x4BBC40 answers):
     - <units>\*.FBI, each by position (tagpu_guifile.h): [UNITINFO]'s UnitName, Side and canstop.
       A "unit type" is a UnitName some FBI declares, compared without case (0x488B10's _stricmp
       over def+0x20); the first file listed wins a name.
     - <gamedata>\sidedata.TDF: [SIDE0], [SIDE1], ... up to the first missing one (0x431A60), each
       its `name`, `nameprefix` and `intgaf`; at most WIKI_SIDE_SIDES are kept.
     - <download>\*.TDF in the enumeration's order (0x42DCF0), each section with a UNITMENU an
       entry (UNITMENU, MENU, BUTTON, UNITNAME), MENU and BUTTON 0 when missing.
     - per player type: the page probe of the def load (0x42D800..0x42D8CB) over <gui>\<NAME>0.GUI,
       <NAME>1.GUI, ..., the count raised by the type's download entries' MENU (0x42DF60..0x42DFB4),
       and every page: its own file, else the player's side's <prefix>DL.GUI (0x41AD61..0x41AD94);
       each page's download fill (0x41ACE0); each side's <prefix>MAIN2.GUI and <prefix>GEN.GUI.
     - the pictures, by the spec's picture rule (decision 5): <anims>\<SCREEN>.GAF, then
       <anims>\<commongui>.GAF, then commongui's generic picture, for an order, tab or pager; for a
       build button, <anims>\<UNIT>_gadget.GAF, then <unitpics>\<UNIT>.PCX, else a labelled button.
       PANELSIDE from <anims>\<intgaf>.GAF; the minimap dots from <anims>\fx.GAF. A GAF is read by
       the archive's 64 KiB page, only the pages its lookups touch: Escalation's fx.GAF is 82 MB.
     - <palettes>\<palette>.PAL, .ALP and .SHD and <palettes>\<guipal>.PAL.
   Format strings the engine builds these names with (`%s%d.GUI` 0x502838, `%sGEN.GUI` 0x50282C,
   `%sMAIN2.GUI` 0x509570, `%sDL` 0x502818, `_gadget` 0x509970, `PANELSIDE` 0x5033D4, `sidedata`
   0x503410) are this file's constants, DISASSEMBLED from the retail executable: a mod that
   patches one of them is not followed (none of the six compat setups does, the Q6 scout answer).

   WHERE THIS DEPARTS FROM THE ENGINE, each as gfx.py does (the two are compared, not the engine):
     - the player's side is not a PlayerInfo's (the stage has none): the first unit of the wiki's
       first entry (by id) whose FBI's Side names a side, else whose type begins with a side's
       nameprefix (the longest), else side 0 -- TA Zero's human units say `NotForAI_Arm`;
     - a build button that only commongui's generic picture would picture skips it and keeps the
       .GUI's rect (the engine would shrink a 64 x 64 slot to a 96 x 20 BUTTONS0 frame);
     - a download entry whose BUTTON + 4 is no gadget of its page is skipped and reported: THE
       ENGINE HAS NO BOUND THERE (0x41AE5E..0x41AE73 indexes ControlsAry with it unchecked);
     - a type with more numbered pages than WIKI_SIDE_PAGES keeps none (the page is unit+0x110 bits
       23..25), and the router then shows GEN for it.

   ONE BLOCK, IMMUTABLE ONCE PUBLISHED AND NEVER FREED: the WIKISIDE, its sheet and its report are
   one allocation the loader fills alone before the store that publishes it (tagpu_wikiside_get).
   Every pointer in it points into it. Its layout has passed tagpu_wikilayout_check, and every frame
   rect in its art lies inside the sheet (checked before the store), so a composer that reads a
   frame through its rect never reads outside the block. */
#define WIKISIDE_SHEET_W    1024        /* the sheet's width; frames are packed in shelves        */
#define WIKISIDE_SHEET_MAXH 8192        /* a sheet taller than this refuses the sidebar           */
#define WIKISIDE_DOTS       32          /* radlogo frames kept, one per owner                     */
#define WIKISIDE_LABELS     16          /* a screen's labels (id 5) kept                          */
#define WIKISIDE_TEXT       128         /* a gadget's text, the engine's +0xB6 (0x80 bytes)       */

/* Where a button's picture came from, the spec's picture rule (section 2.3) step by step. */
enum {
    WIKISIDE_FROM_LABEL,                /* no picture: the stage draws a labelled button (ops)    */
    WIKISIDE_FROM_PAGE,                 /* its name in <anims>\<SCREEN>.GAF                       */
    WIKISIDE_FROM_COMMONGUI,            /* its name in <anims>\<commongui>.GAF                    */
    WIKISIDE_FROM_FALLBACK,             /* commongui's generic CHECKBOX/stagebuttnN/BUTTONS0      */
    WIKISIDE_FROM_DOWNLOAD,             /* <anims>\<UNIT>_gadget.GAF, the sequence <UNIT>         */
    WIKISIDE_FROM_UNITPICS              /* <unitpics>\<UNIT>.PCX, one frame, no key               */
};

/* A frame of the sheet: the w x h pixels at (sx, sy), palette indices. (hx, hy) is the hotspot
   the blit subtracts from its point; 0 for a button's frames, because GUI_ButtonDraw 0x4A5F40
   adds the hotspot back (0x4A6188..0x4A61BE) so a button's frame lands with its top left on the
   gadget's corner. A frame with sub-frames is composed into its own w x h box as CopyGafToContext
   0x4B7F90 draws it (each layer at the point minus its own hotspot). */
typedef struct { int sx, sy, w, h, hx, hy; } WIKISIDEFRAME;

/* A button's picture; pic[i] of a screen belongs to the layout screen's gadget[i]. */
typedef struct {
    int           from;                 /* WIKISIDE_FROM_*                                       */
    int           nframe;               /* 0 a label; 1 one frame (every state shows frame 0 but
                                           pressed, for which the engine draws NO picture:
                                           0x4B7F30 returns none past the count, 0x4A6176 skips);
                                           3 the frames below                                     */
    WIKISIDEFRAME frame[3];             /* at rest; pressed or the current tab; greyed -- as
                                           GUI_ButtonDraw picks them over the sequence: base,
                                           base + 1, and the last frame for attribs 0x100, else
                                           min(base + 2, n - 1), each clipped to the sequence   */
    int           key;                  /* the transparent index of frame 0, -1 none (a PCX)     */
    int           attribs;              /* the gadget's: 0x100 the last frame is the greyed one;
                                           0x80 or 0x100 the greyed tint is not drawn            */
    char          text[WIKISIDE_TEXT];  /* a label's text (the unit, or the gadget's name); the
                                           gadget's own text for a generic commongui picture    */
} WIKISIDEPIC;

typedef struct { int x, y, w, h; char text[WIKISIDE_TEXT]; } WIKISIDELABEL;

/* A screen's art, parallel to the layout's screen of the same index. */
typedef struct {
    int           has_panel;
    WIKISIDEFRAME panel;                /* drawn once with its top left at (-hx, screen.y - hy),
                                           clipped to the screen (0x4B0230 with one frame); a
                                           many-framed BackTile is composed here into one frame,
                                           the 3 x 3 nine-slice the engine tiles it as. Without
                                           one the engine fills the panel with a GUI colour this
                                           block does not carry                                  */
    int           panel_key;
    WIKISIDEPIC   pic[WIKI_SIDE_GADGETS];
    int           nlabel;               /* its labels (id 5), which nothing fills (spec 1, MAIN2) */
    WIKISIDELABEL label[WIKISIDE_LABELS];
} WIKISIDESCREEN;

typedef struct {
    char name[WIKI_SIDE_NAME];          /* the entry's type name, as the entry spells it          */
    int  canstop;                       /* the FBI's canstop & 1 (def+0x245 bit 3, 0 when absent) */
} WIKISIDETYPE;

typedef struct WIKISIDE {
    WIKILAYOUT     layout;              /* checked by tagpu_wikilayout_check                      */
    const unsigned char* sheet;         /* sheet_w x sheet_h palette indices                      */
    int            sheet_w, sheet_h;
    unsigned char  pal[256 * 4];        /* <palette>.PAL as RGBA, alpha 255                       */
    int            has_panelside;       /* the player's side's PANELSIDE frame 0, drawn with its  */
    WIKISIDEFRAME  panelside;           /*   top left at (0, 0) whatever its hotspot: 0x467D70   */
    int            panelside_key;       /*   passes the hotspot as the point (0x467E30..0x467E3C) */
    int            nradlogo;            /* fx.GAF's radlogo, frame k for owner k (0x466DC0)       */
    WIKISIDEFRAME  radlogo[WIKISIDE_DOTS];
    int            has_high;            /* radlogohigh frame 0, over the unit under the pointer   */
    WIKISIDEFRAME  high;
    int            dot_key;
    int            unexplored, box;     /* the GUI colour table's entries 0 (unexplored fog,
                                           0x466C20) and 0x0E (the camera box, 0x466B70): guipal
                                           matched into the palette (0x4AC7D0); -1 without
                                           guipal.PAL, where the mockup's read() takes 0          */
    unsigned char  seen[256];           /* the explored-but-unseen remap (0x4BAD30)              */
    int            has_greyed;          /* the tint over a greyed button: the grey remap then the */
    unsigned char  greyed[256];         /*   box shader at level -20 (0x4A699B..0x4A69BA); none
                                           without a 32-row <palette>.SHD: drawn untinted         */
    unsigned char  alp[65536];          /* the 2:1 shrink's table, <palette>.ALP or generated
                                           (0x42E140, 0x4BA750); T[row * 256 + col]              */
    WIKISIDESCREEN screen[WIKI_SIDE_SCREENS];
    int            ndef;                /* the player's types that resolved to an FBI             */
    WIKISIDETYPE   def[WIKI_SIDE_TYPES];
    const char*    report;              /* the JSON tagpu_wikiside.json holds, NUL-terminated     */
} WIKISIDE;

/* The sidebar's load, on the calling thread, over `vfs` opened on `in`'s sources: the block, or
   NULL when the sidebar is refused (no sidedata, no MAIN2 and GEN for the player's side, no
   palette, a layout tagpu_wikilayout_check refuses, a sheet past WIKISIDE_SHEET_MAXH, memory).
   `*report` gets the report either way, malloc'd, or NULL when memory ran out; the block keeps its
   own copy. The block is one allocation; the DLL never frees it, the native test frees it. */
const WIKISIDE* tagpu_wikiside_load(const WIKI3DIN* in, TAGPU_VFS* vfs, char** report);

/* THE MINIMAP PICTURE the engine generates for a map with no TNT picture (0x466780), over a
   resolved stage: the stage map's extent W x H (cells x 16), not the engine's scroll extent, which
   would cut the stage's bottom rows off; the long side 126 pixels, the short one trunc(126 * short
   / long), centred on the short axis. A 2w x 2h surface sampled from the tiles, shrunk 2:1 by
   0x4B95A0 through `alp`: out = T[T[a][b]][T[c][d]], a, b the top pair and c, d the bottom pair.
   Writes w * h indices to `out` (at most 126 * 126) and the geometry; 0 when the stage has no
   resolved ground or odd cells. Pure. */
int tagpu_wikiside_minimap(const WIKI3DSTAGE* st, const unsigned char* alp, unsigned char* out,
                           int* w, int* h, int* offx, int* offy);

#ifdef _WIN32
/* The load, started once on a thread of its own with `in`, which it takes and frees; later calls
   free `in` and return. The game thread, at the wiki's first open. The thread loads the sidebar
   first and publishes it (tagpu_wikiside_get), then the 3D assets (tagpu_wiki3d_get). */
void          tagpu_wikiasset_start(WIKI3DIN* in);

/* THE SIDEBAR'S PUBLICATION. WIKISIDE_PENDING until the loader has decided, then for good either
   WIKISIDE_READY with `*out` the block, or WIKISIDE_REFUSED (the sidebar is refused, the loader
   could not start, or the sources did not open): the wiki then uses the fixture layout.
   ORDERING: the block is written whole before the one interlocked store that publishes it, and
   this read is an interlocked one, a full barrier, so every byte read through `*out` after a
   WIKISIDE_READY was written before that store. The sidebar is published BEFORE the 3D assets
   are loaded, so a reader that waits for this answer before it starts a demo never starts one on
   a layout that arrives later. Any thread; the block is never written or freed after. */
enum { WIKISIDE_PENDING, WIKISIDE_READY, WIKISIDE_REFUSED };
int           tagpu_wikiside_get(const WIKISIDE** out);

/* The published load, or NULL while it runs and for good when it failed before publishing (the
   log says why). Any thread; immutable once returned and never freed. */
const WIKI3D* tagpu_wiki3d_get(void);

/* TRIGGER: `tagpu_wiki3d.dump` in the game's folder writes the report to `tagpu_wiki3d.json` once
   the load is published (until then the trigger waits), and the sidebar's to `tagpu_wikiside.json`
   when it was published. The game thread, from the wiki's fill. */
void          tagpu_wikiasset_poll(void);
#endif

/* The load itself, on the calling thread, into an arena at `base` of `cap` bytes whose pointer
   fields hold `base32` + offset: the DLL passes its reserved block and its own address; the native
   test passes host memory and a stand-in address, since its pointers are not 32-bit. `commit`
   makes the block usable up to `upto` bytes and answers how far it now is, 0 when it cannot; NULL
   when the whole block already is. `vfs` is the sources opened on `in`, or NULL to open them here.
   `side` is the published sidebar, whose `alp` makes each resolved stage's minimap picture, or
   NULL for none. Returns the WIKI3D, which lies inside the arena past its first bytes, or NULL
   when not even the report fit. */
const WIKI3D* tagpu_wiki3d_load(const WIKI3DIN* in, TAGPU_VFS* vfs, const WIKISIDE* side, unsigned char* base,
                                uint32_t base32, uint32_t cap, uint32_t (*commit)(unsigned char* base, uint32_t upto));

#endif
