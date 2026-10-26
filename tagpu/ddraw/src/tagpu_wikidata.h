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
   missing footprint or a BuildTime that is not positive. Every other absent figure reads as 0,
   which every rule of sim.js treats as it treats `undefined` (each one tests `> 0`). It also
   refuses a map wider or taller than WIKI_MAP_MAX, which the mockup would play. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_json.h"

#define WIKI_ID_MAX   24    /* a unit id or a unit type, with its terminator */
#define WIKI_MAX_DEFS 64    /* the unit types of one entry: a build list is a mask over them */
/* Cells a side of an entry's map; the loader refuses a larger one. The DLL's packet carries the
   map by value, at most 128 x 128 cells, and a 700-pixel stage still gives 5 px a cell; the
   simulation's take-off rule rests on it too (tagpu_wikisim.c, fixed16). The data's map is
   40 x 24. */
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
} WIKIINPUT;

typedef struct {
    const char*      id;
    const char*      title;
    WIKISCENE        scene;
    int              ninput;
    WIKIINPUT*       input;             /* in file order; the sim plays them by `at` */
    int              has_end;
    double           end;               /* the tick it ends on */
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

typedef struct {
    int              nnode, nentry;
    WIKINODE*        node;
    WIKIENTRY*       entry;
    TJDOC*           doc;
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

#endif
