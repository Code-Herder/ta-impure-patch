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
   with a positive, finite length, or a demo's `sidebar` that is not a boolean. Every other absent
   figure reads as 0, which every rule of sim.js treats as it treats `undefined` (each one tests
   `> 0`). It also refuses what the mockup would play: a map wider or taller than WIKI_MAP_MAX, a
   type name that is empty or of WIKI_ID_MAX bytes or more, a `canstop` that is neither a number
   nor a boolean, and a non-boolean `sidebar` on a demo only Try it yourself would read. The demo's
   build trips (tagpu_wikidata_trips) are refused where sim.js refuses them, when a simulation
   starts with the sidebar on.

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

/* sim.js's KEYS, in its order, which is the order the view lists the held ones in */
enum { WIKI_KEY_SHIFT, WIKI_KEY_ALT, WIKI_KEY_CTRL, WIKI_KEY_Z, WIKI_KEY_X, WIKI_KEY_LMB, WIKI_KEY_RMB, WIKI_NKEYS };

/* A unit type: its figures from the stock unit file, as the entry carries them. */
typedef struct {
    char        name[WIKI_ID_MAX];  /* ARMMEX */
    const char* title;              /* "Metal Extractor"; NULL when the entry gives none */
    int         fx, fz;             /* footprint, cells */
    double      build_time, worker_time, build_distance, max_velocity, extracts, energy_make;
    int         vent;               /* a geothermal plant: needs a vent under its footprint */
    int         canstop;            /* FBI canstop, def+0x245 bit 3: STOP is live for a selection
                                       holding it */
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

enum { WIKI_IN_MOVE = 1, WIKI_IN_BUILD = 2, WIKI_IN_CAPTION = 4 };

/* One input of a demo's timeline, or one the player gives Try it yourself. */
typedef struct {
    double           at;                /* the tick it is fed on */
    unsigned         has;               /* WIKI_IN_* */
    WIKIPOINT        move;              /* WIKI_IN_MOVE: the cursor, world units */
    int              down, up;          /* WIKI_KEY_*, or -1 */
    int              build;             /* WIKI_IN_BUILD: the type armed, or -1 to leave the mode */
    const char*      caption;           /* WIKI_IN_CAPTION: the caption, NULL to clear it */
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

/* sim.js's name of a key: "shift", "alt", "ctrl", "z", "x", "lmb", "rmb"; NULL out of range. */
const char* tagpu_wikidata_key_name(int key);

/* ---- the left sidebar's layout: the running mod's (tagpu_wikiasset.c reads it from the mod's
   .GUI files) or the fixture's (art/wiki/side/fixture.json, our own numbers, in WIKIPAK). The
   same schema as sim.js's checkLayout, which is its specification; research/notes/
   ingame-wiki-sidebar-plan.md says what the sidebar is.

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
#define WIKI_SIDE_TYPES    WIKI_MAX_DEFS
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

typedef struct {
    char name[WIKI_SIDE_NAME];
    int  npage;
    int  page[WIKI_SIDE_PAGES];     /* screen indices: page[k] is page k + 1 */
    int  orders;                    /* the <NAME>0 screen's index, or -1 */
} WIKIPAGES;

typedef struct WIKILAYOUT {
    int         format, width, height;
    int         nside, player_side;
    WIKISIDEDEF side[WIKI_SIDE_SIDES];
    int         ntype;
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
   checkLayout's bounds it refuses two screens of one name, which sim.js cannot be given (a JSON
   object keeps one of them). */
WIKILAYOUT* tagpu_wikidata_layout(const char* text, size_t len, char* err, size_t errn);
WIKILAYOUT* tagpu_wikidata_layout_json(const TJV* v, char* err, size_t errn);
WIKILAYOUT* tagpu_wikidata_layout_pak(const unsigned char* pak, size_t npak, const char* path, char* err, size_t errn);

/* THE BUILD TRIP'S DATA RULE, sim.js's checkTrips, for a demo that plays with the sidebar: every
   `build` names a type, carries no other input and finds the left button up; no input but a
   caption stands in its trip, the ticks (t, t + WIKI_TRIP), nor after it within tick t; and the
   demo lasts the whole trip. The inputs are read in the simulation's order (by tick, then file
   order). 1 when it holds, else 0 with a message in `err`. tagpu_wikisim_init asks it exactly
   where sim.js throws: a scripted demo whose sidebar is on. */
int tagpu_wikidata_trips(const WIKIDEMO* d, char* err, size_t errn);

#endif
