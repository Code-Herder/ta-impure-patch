#ifndef TAGPU_WIKIDATA_H
#define TAGPU_WIKIDATA_H
/* tagpu_wikidata -- the in-game wiki's content: art/wiki/tree.json and art/wiki/entries/<id>.json
   (research/notes/ingame-wiki.md, "The data format"), read with tagpu_json into C structs. The
   DLL embeds them as the resource WIKIPAK (tools/hudpak.py --set wiki); the mockup reads the
   same files.

   ENGINE-FREE AND WIN32-FREE, with no state of its own: a WIKIDATA or WIKIENTRY owns everything
   it points to, the JSON trees its strings live in included, and nothing writes it once it is
   returned, so any thread may read one its owner keeps alive. tools/wiki/simparity.mjs compiles
   this file natively.

   THE LOADER REFUSES WHAT THE MOCKUP WOULD THROW ON OR READ AS UNDEFINED: a unit type the entry
   does not define, a key it does not name, a rectangle or a map size that is not an integer, a
   missing footprint, a BuildTime that is not positive, a unit's facing that is not two numbers
   with a positive, finite length, a demo's `sidebar` that is not a boolean, a unit's `g` that
   sim.js's checkG refuses, and a text token text.js's checkEntry refuses (tagpu_wikidata_entry).
   Every other absent figure reads as 0, which every rule of sim.js treats as it treats
   `undefined` (each one tests `> 0`). It also refuses what the mockup would play: a map wider or
   taller than WIKI_MAP_MAX, a type name that is empty or of WIKI_ID_MAX bytes or more, a type
   with G cells whose footprint passes WIKI_FP_MAX (its G rows hold 16 cells), a `canstop` that
   is neither a number nor a boolean, and a non-boolean `sidebar` on a demo only Try it yourself
   would read. A unit's `cast` is read, not required: sim.js refuses a unit with no cast, or two
   units of one cast, only where it binds the entry to a casting, so the loader leaves both to
   the binding. The demo's build trips (tagpu_wikidata_trips) and the data rule of its resolved script
   (tagpu_wikidata_buttons) are refused where sim.js refuses them, when a simulation starts.

   IT NORMALISES A FACING AS sim.js's facing() DOES, bit for bit: the same + * / and sqrt, each
   rounded once, which is why the DLL compiles this file for SSE2 as it compiles the simulation
   (Makefile, THE WIKI'S SIMULATION). */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_json.h"

#define WIKI_ID_MAX   24    /* a unit id or a unit type, with its terminator */
#define WIKI_MAX_DEFS 64    /* the unit types of one entry: a build list is a mask over them */
/* Cells a side of an entry's map; the loader refuses a larger one. The DLL's packet carries the
   map by value, at most 128 x 128 cells, and a 700-pixel stage still gives 5 px a cell; the
   simulation's take-off rule rests on it too (tagpu_wikisim.c, fixed16). The data's maps are
   40 x 30. */
#define WIKI_MAP_MAX  128

/* sim.js's KEYS, in its order, which is the order the view lists the held ones in; then its
   ACTIONS, in its order: what a demo's press does, which the interface type turns into a mouse
   button (tagpu_wikidata_button). A simulation holds keys only: an index into its held[] is
   below WIKI_NKEYS, and an action reaches it resolved. */
enum { WIKI_KEY_SHIFT, WIKI_KEY_ALT, WIKI_KEY_CTRL, WIKI_KEY_Z, WIKI_KEY_X, WIKI_KEY_LMB, WIKI_KEY_RMB, WIKI_NKEYS,
       WIKI_ACT_SELECT = WIKI_NKEYS, WIKI_ACT_ORDER, WIKI_ACT_CANCEL, WIKI_NKEYNAMES };

/* sim.js's CASTS, in its order: a unit's place in the demos, which a side's casting answers with
   one of the mod's types (research/notes/ingame-wiki.md, the owner's decisions 13-15) */
enum { WIKI_CAST_BUILDER, WIKI_CAST_ADVBUILDER, WIKI_CAST_MEX, WIKI_CAST_MOHO, WIKI_CAST_GEO,
       WIKI_CAST_WALKER, WIKI_CAST_TOWER, WIKI_CAST_WALL, WIKI_CAST_SOLAR, WIKI_NCAST };
/* A footprint's side, each axis, in a casting, and the width of a G row's bit mask: sim.js's
   FP_MAX. An entry's type with G cells is held to it too. */
#define WIKI_FP_MAX    16
/* A display name with its terminator: the engine's name is 31 code-page bytes (def+0x00 is 0x20
   bytes, DISASSEMBLED), and each Windows-1252 byte is at most 3 bytes of UTF-8 once converted
   (U+20AC and U+2122 are the widest), so 31 x 3 + 1. sim.js's TITLE_MAX. */
#define WIKI_TITLE_MAX 94
/* A caption filled, with its terminator: the stage record's and the page's caption fields. The
   loader refuses an entry one of whose captions could fill past it (text.js's CAPTION_MAX), so no
   casting and no interface type can cut a loaded caption. */
#define WIKI_CAPTION_MAX 512

/* A unit type: its figures from the stock unit file, as the entry carries them. */
typedef struct {
    char        name[WIKI_ID_MAX];  /* ARMMEX */
    const char* title;              /* "Metal Extractor"; NULL when the entry gives none */
    int         cast;               /* WIKI_CAST_*, or -1: no `cast`, or a name not one of the nine */
    int         fx, fz;             /* footprint, cells */
    double      build_time, worker_time, build_distance, max_velocity, extracts, energy_make;
    int         vent;               /* has a G cell: a geothermal plant, needing a vent under one */
    uint16_t    g[WIKI_FP_MAX];     /* row dz: bit dx set = footprint cell (dx, dz) is a G cell;
                                       the unit's `g`, or every cell when `vent` comes with none */
    int         canstop;            /* FBI canstop, def+0x245 bit 3: STOP is live for a selection
                                       holding it */
    int         activate_when_built;/* FBI activatewhenbuilt, def+0x241 bit 18: a structure
                                       finished, or made whole, is switched on (0x41B9A6) */
    uint64_t    builds;             /* bit i: its build list names type i of the entry */
} WIKIDEF;

typedef struct { int x, z, w, h; double v; } WIKIMETAL;      /* cells, and the metal value     */
typedef struct { int x, z, w, h; int black; } WIKIFOG;       /* cells; black unexplored, else grey */
typedef struct { double x, z; } WIKIPOINT;                   /* world units                    */

typedef struct {
    char             id[WIKI_ID_MAX];   /* "" when the scene gives none: the sim names it */
    int              def;
    int              owner;
    double           x, z;
    double           built;             /* 1 finished, below it under construction */
    double           hx, hz;            /* "facing" as a unit vector, (0, 1) when absent; the
                                           simulation turns only a mobile unit by it */
    const char*      label;             /* NULL: the type's name */
    int              nmove;
    WIKIPOINT*       move;              /* its move orders, in order */
} WIKISCENEUNIT;

/* The entry's scene, or a demo's additions to it: the demo's units, metal, vents and fog go after
   the entry's, and its selection and cursor replace the entry's when it gives them. Only the
   entry's map counts. */
typedef struct {
    int              w, h;              /* cells */
    double           surface_metal;
    int              nmetal, nvent, nfog, nunit, nselected;
    WIKIMETAL*       metal;
    WIKIPOINT*       vent;
    WIKIFOG*         fog;
    WIKISCENEUNIT*   unit;
    int              has_selected, has_cursor;
    const char**     selected;          /* unit ids */
    WIKIPOINT        cursor;
} WIKISCENE;

enum { WIKI_IN_MOVE = 1, WIKI_IN_BUILD = 2, WIKI_IN_CAPTION = 4, WIKI_IN_UNTIL = 8 };

/* A DEMO'S WAIT (sim.js's UNTILS): the state a wait holds the demo's clock for. IDLE: no unit of
   the player's holds a build or a reclaim order. WIKI_UNTIL_MAX bounds its max, in ticks. */
enum { WIKI_UNTIL_IDLE = 1 };
#define WIKI_UNTIL_MAX 3600

/* One input of a demo's timeline, or one the player gives Try it yourself. */
typedef struct {
    double           at;                /* the tick it is fed on */
    unsigned         has;               /* WIKI_IN_* */
    WIKIPOINT        move;              /* WIKI_IN_MOVE: the cursor, world units */
    int              down, up;          /* WIKI_KEY_*, WIKI_ACT_* (an action, unresolved), or -1 */
    int              build;             /* WIKI_IN_BUILD: the type armed, or -1 to leave the mode */
    const char*      caption;           /* WIKI_IN_CAPTION: the caption, NULL to clear it */
    int              until, until_max;  /* WIKI_IN_UNTIL: the WIKI_UNTIL_* and its max, ticks */
    int              extra;             /* a demo's input names a key besides at, caption and
                                           build: it is no caption alone, which a build trip's
                                           ticks admit (tagpu_wikidata_trips) */
} WIKIINPUT;

typedef struct {
    const char*      id;
    const char*      title;
    WIKISCENE        scene;
    int              ninput;
    WIKIINPUT*       input;             /* in file order; the sim plays them by `at` */
    int              has_end;
    double           end;               /* the tick it ends on */
    int              has_sidebar;       /* `sidebar` given: it decides whether the demo plays with */
    int              sidebar;           /* the sidebar, else a mobile builder of the player's does */
} WIKIDEMO;

typedef struct {
    const char*      id;
    const char*      title;
    const char*      subtitle;
    int              ntext, nlocked, ndef, ndemo;
    const char**     text;              /* paragraphs */
    const char**     locked_in;         /* the mods that turn the feature off */
    double           pace;              /* construction and reclaim run this many times faster */
    WIKIDEF*         def;               /* in the file's order, which breaks the sim's ties */
    WIKISCENE        scene;
    WIKIDEMO*        demo;
    TJDOC*           doc;               /* the strings above live in it */
} WIKIENTRY;

/* The tree in preorder: a category has a title and no entry, a leaf an entry and no title. */
typedef struct {
    int              depth;
    const char*      title;
    int              entry;             /* an index of WIKIDATA.entry, or -1 */
} WIKINODE;

struct WIKILAYOUT;

typedef struct {
    int              nnode, nentry;
    WIKINODE*        node;
    WIKIENTRY*       entry;
    TJDOC*           doc;
    /* art/wiki/side/fixture.json, checked: the sidebar the wiki falls back to when the running
       mod's is absent or refused. NULL when the archive has none (hudpak.py's wiki set requires
       it, so the DLL's always does). It lives as long as the WIKIDATA, which the DLL never frees,
       and nothing writes it, so a simulation and the render thread may hold it. */
    const struct WIKILAYOUT* fixture;
} WIKIDATA;

/* An entry from its JSON text into `e`; 0 with a message in `err` when it is refused, and then
   `e` holds nothing to free. */
int  tagpu_wikidata_entry(WIKIENTRY* e, const char* text, size_t len, char* err, size_t errn);
void tagpu_wikidata_entry_free(WIKIENTRY* e);

/* One input as a demo's timeline gives it, into `in`, its build type read against `e`; the
   caption points into `v`'s tree, and `at` reads 0 when absent (a demo's inputs must carry it,
   one fed straight to the sim need not). 0 with a message in `err` when it is refused. */
int  tagpu_wikidata_input(const WIKIENTRY* e, const TJV* v, WIKIINPUT* in, char* err, size_t errn);

/* The tree and every entry it names, from an archive of art/wiki/ (tagpu_pak.h), or NULL with a
   message in `err`. */
WIKIDATA* tagpu_wikidata_load(const unsigned char* pak, size_t npak, char* err, size_t errn);
void      tagpu_wikidata_free(WIKIDATA* d);

#ifdef _WIN32
/* tagpu_wikidata_load over this DLL's WIKIPAK resource. */
WIKIDATA* tagpu_wikidata_load_resource(char* err, size_t errn);
#endif

/* A type of the entry by name, or -1. */
int tagpu_wikidata_def(const WIKIENTRY* e, const char* name);

/* sim.js's name of a key or an action: "shift", "alt", "ctrl", "z", "x", "lmb", "rmb", "select",
   "order", "cancel"; NULL out of range. */
const char* tagpu_wikidata_key_name(int key);

/* sim.js's and text.js's name of a cast: "builder", "advbuilder", ... "solar"; NULL out of range. */
const char* tagpu_wikidata_cast_name(int cast);

/* text.js's BUTTONS: the word of the button token named by the `len` bytes at `s` at interface
   type `iface` (1 exactly when 1, else 0), or NULL when no button has that name. */
const char* tagpu_wikidata_button_word(const char* s, size_t len, int iface);

/* A key or an action as interface type `iface` presses it, sim.js's button(): select is the left
   button at both types, cancel the right one at both, order the right one at type 1 and the left
   one at any other; a key comes back as given. */
int tagpu_wikidata_button(int key, int iface);

/* THE DATA RULE OF A RESOLVED SCRIPT, sim.js's resolve(): the demo's inputs in the simulation's
   order (by tick, then file order), each action turned into its button at `iface`; a press of a
   mouse button already held, or a release of one not held, refuses the demo with the input's
   tick. Keys are not held to it: a key's second down is an auto-repeat. 1 when it holds, else 0
   with a message in `err`. sim.js asks it of every scripted demo. */
int tagpu_wikidata_buttons(const WIKIDEMO* d, int iface, char* err, size_t errn);

/* ---- a casting: one side's answer to each cast, the side's record of a pack's `casting` list
   (gfx/side/<mod>/side.json, index-aligned with the layout's sides) or of
   tools/wiki/casting-fixture.json. sim.js's checkCasting is its specification. Pointer-free and
   bounded, like WIKILAYOUT: a producer fills it, and from then on nothing writes it. */
typedef struct {
    int      bound;                 /* 0: the cast's value is null, the rule found no type */
    char     type[WIKI_ID_MAX];     /* the UnitName UPPER-CASE, as the layout's build buttons spell
                                       it (the engine compares names without case, 0x488B10
                                       _stricmp); 1..23 bytes, not all digits */
    char     title[WIKI_TITLE_MAX]; /* the display name, UTF-8; may be "" */
    int      fx, fz;                /* 1..WIKI_FP_MAX: a producer leaves a type past it unbound
                                       (gfx.py's casts), and the JSON reader refuses it */
    double   build_time, worker_time, build_distance, max_velocity, extracts, energy_make;
                                    /* finite, >= 0, build_time > 0; an absent one reads 0 */
    int      canstop;               /* (canstop & 1), as WIKIDEF's */
    int      activate_when_built;   /* (activatewhenbuilt & 1), as WIKIDEF's */
    int      vent;                  /* has a G cell */
    uint16_t g[WIKI_FP_MAX];        /* as WIKIDEF's; inside fx x fz */
    uint16_t builds;                /* bit c: its build menu holds cast c's type */
} WIKICASTDEF;

typedef struct { WIKICASTDEF cast[WIKI_NCAST]; } WIKICASTING;

/* A side's casting from its JSON record ({"casts": {...}}; `side`, `commander`, `report` and any
   other key are read past) into `c`, held to every bound of sim.js's checkCasting: all nine casts
   given, each null or a record whose type, title, footprint, figures, canstop, activatewhenbuilt,
   vent, g and builds
   hold. Beyond it, a cast named twice is refused (a JSON object keeps one of them for sim.js).
   1, or 0 with a message in `err` and `c` zeroed: a refused casting is not used at all. */
int tagpu_wikidata_casting(const TJV* v, WIKICASTING* c, char* err, size_t errn);

/* ---- the left sidebar's layout: the running mod's (tagpu_wikiasset.c reads it from the mod's
   .GUI files) or the fixture's (art/wiki/side/fixture.json, our own numbers, in WIKIPAK). The
   same schema as sim.js's checkLayout, which is its specification; research/notes/
   ingame-wiki.md, *The left sidebar on the stage*, says what the sidebar is.

   Screens are referred to by their index in `screen`, never by a pointer, so a layout is one
   block with no pointers in it: a producer fills it, tagpu_wikilayout_check bounds it, and from
   then on nothing writes it, so the simulation and the render thread may both read it. Every
   index stored in it is checked there against the count it indexes. It is about 285 KB: it lives
   on the heap or in an arena, never on a stack. */
#define WIKI_SIDE_W        128   /* the engine's panel width, whatever the HUD scale */
#define WIKI_SIDE_MIN_H    480   /* the PANELSIDE picture's rows: the column is never shorter */
#define WIKI_SIDE_MINIMAP  128   /* the minimap's square at the column's top, never clickable */
#define WIKI_SIDE_ROWS     1024  /* the composed column's texture is 128 x 1024 */
#define WIKI_SIDE_SCREENS  48
#define WIKI_SIDE_GADGETS  64    /* a screen's gadgets, and the length of the view's states */
#define WIKI_SIDE_PAGES    7     /* the page is unit+0x110 bits 23..25: at most 7 numbered pages */
#define WIKI_SIDE_TYPES    WIKI_MAX_DEFS   /* over every side */
#define WIKI_SIDE_SIDES    8
#define WIKI_SIDE_NAME     32    /* a gadget, screen, side or unit name, with its terminator */

/* sim.js's ROLES, in its order: how 0x41AA00 tells the buttons apart by name */
enum { WIKI_ROLE_ORDERS, WIKI_ROLE_BUILDTAB, WIKI_ROLE_BUILD, WIKI_ROLE_PREV, WIKI_ROLE_NEXT,
       WIKI_ROLE_STOP, WIKI_ROLE_ORDER, WIKI_NROLES };

typedef struct {
    char name[WIKI_SIDE_NAME];
    int  role;                      /* WIKI_ROLE_* */
    char unit[WIKI_SIDE_NAME];      /* a build button's unit type; "" for none (null) */
    int  grey, hidden;
    int  x, y, w, h;                /* in its screen's pixels, inside the screen */
} WIKIGADGET;

typedef struct {
    char       name[WIKI_SIDE_NAME];
    int        y, w, h;             /* y: its first row in the column */
    int        ngadget;
    WIKIGADGET gadget[WIKI_SIDE_GADGETS];
} WIKISCREEN;

typedef struct {
    char name[WIKI_SIDE_NAME], prefix[WIKI_SIDE_NAME];
    int  main2, gen;                /* screen indices */
} WIKISIDEDEF;

/* One side's pages of one type: a side's types are the types with a page (def+0x22E != 0) that
   side's casting names, their pages read with that side's prefix, so one name may have other
   pages in another side. A type is found by (name, side). */
typedef struct {
    char name[WIKI_SIDE_NAME];      /* the type UPPER-CASE, as the casting's `type` */
    int  side;                      /* the side index it belongs to */
    int  npage;
    int  page[WIKI_SIDE_PAGES];     /* screen indices: page[k] is page k + 1 */
    int  orders;                    /* the <NAME>0 screen's index, or -1 */
} WIKIPAGES;

typedef struct WIKILAYOUT {
    int         format, width, height;
    int         nside;
    WIKISIDEDEF side[WIKI_SIDE_SIDES];
    int         ntype;              /* over every side */
    WIKIPAGES   type[WIKI_SIDE_TYPES];
    int         nscreen;
    WIKISCREEN  screen[WIKI_SIDE_SCREENS];
} WIKILAYOUT;

/* Every bound sim.js's checkLayout holds, over a filled layout: 1 when it holds them all, else 0
   with a message in `err`; a refused layout is not used at all (the fixture is). Every producer
   calls it before the layout is published, so a count or an index read out of a checked layout
   is in range by construction. */
int tagpu_wikilayout_check(const WIKILAYOUT* L, char* err, size_t errn);

/* A layout from its JSON (sim.js's schema; a `note` key, or any other the schema does not name,
   is read past), into a heap block the caller frees with free(), checked by
   tagpu_wikilayout_check; NULL with a message in `err` when it is refused. The text form, the
   tree form (a member of a larger document), and a file of a WIKIPAK archive (`path`, e.g.
   "side/fixture-tall.json"). Every string is copied, so the layout outlives the text. Beyond
   checkLayout's bounds it refuses two screens of one name, or two types of one name in one side,
   which sim.js cannot be given (a JSON object keeps one of them). */
WIKILAYOUT* tagpu_wikidata_layout(const char* text, size_t len, char* err, size_t errn);
WIKILAYOUT* tagpu_wikidata_layout_json(const TJV* v, char* err, size_t errn);
WIKILAYOUT* tagpu_wikidata_layout_pak(const unsigned char* pak, size_t npak, const char* path, char* err, size_t errn);

/* THE BUILD TRIP'S DATA RULE, sim.js's checkTrips, for a demo that plays with the sidebar: every
   `build` names a type, carries no other input and finds the left button up; no input but a
   caption stands in its trip, the ticks (t, t + WIKI_TRIP), nor after it within tick t; and the
   demo lasts the whole trip. The inputs are read in the simulation's order (by tick, then file
   order), each action as its button at `iface`, as sim.js reads its resolved script. 1 when it
   holds, else 0 with a message in `err`. tagpu_wikisim_init asks it exactly where sim.js throws:
   a scripted demo whose sidebar is on. */
int tagpu_wikidata_trips(const WIKIDEMO* d, int iface, char* err, size_t errn);

#endif
