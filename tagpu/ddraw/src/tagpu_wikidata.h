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
#include "tagpu_recrules.h"

#define WIKI_ID_MAX   24    /* a unit id or a unit type, with its terminator */
#define WIKI_SCENE_MAX 17   /* a scene's name, a-z and 0-9 with a letter first, with its terminator */
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
enum { WIKI_KEY_SHIFT, WIKI_KEY_ALT, WIKI_KEY_CTRL, WIKI_KEY_Z, WIKI_KEY_X, WIKI_KEY_E, WIKI_KEY_LMB, WIKI_KEY_RMB, WIKI_NKEYS,
       WIKI_ACT_SELECT = WIKI_NKEYS, WIKI_ACT_ORDER, WIKI_ACT_CANCEL, WIKI_NKEYNAMES };

/* sim.js's CASTS, in its order: a unit's place in the demos, which a side's casting answers with
   one of the mod's types (research/notes/ingame-wiki.md, the owner's decisions 13-15). The
   commander is last so that every earlier cast keeps its index and every casting's `builds` bit
   mask its meaning. */
enum { WIKI_CAST_BUILDER, WIKI_CAST_ADVBUILDER, WIKI_CAST_MEX, WIKI_CAST_MOHO, WIKI_CAST_GEO,
       WIKI_CAST_WALKER, WIKI_CAST_TOWER, WIKI_CAST_WALL, WIKI_CAST_SOLAR, WIKI_CAST_COMMANDER, WIKI_NCAST };
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
    int         cast;               /* WIKI_CAST_*, or -1: no `cast`, or a name not one of the ten */
    int         fx, fz;             /* footprint, cells */
    double      build_time, worker_time, build_distance, max_velocity, extracts, energy_make;
    double      build_cost_metal, build_cost_energy, metal_make, metal_storage, energy_storage;
                                    /* the economy's, as a cast's: finite, >= 0; absent 0 */
    double      energy_use;         /* |x| < WIKI_ECON_MAX, negative for a solar collector; absent 0 */
    int         rival;              /* a game entry's: the opponent's unit, cast by the rival casting */
    int         corpse;             /* a game entry's: its wreck, a row of WIKIENTRY.feat; -1 none */
    int         vent;               /* has a G cell: a geothermal plant, needing a vent under one */
    uint16_t    g[WIKI_FP_MAX];     /* row dz: bit dx set = footprint cell (dx, dz) is a G cell;
                                       the unit's `g`, or every cell when `vent` comes with none */
    int         canstop;            /* FBI canstop, def+0x245 bit 3: STOP is live for a selection
                                       holding it */
    int         activate_when_built;/* FBI activatewhenbuilt, def+0x241 bit 18: a structure
                                       finished, or made whole, is switched on (0x41B9A6) */
    int         canreclamate;       /* FBI canreclamate, def+0x245 bit 0x400: RECLAIM is live for
                                       a selection holding it, and the sweep orders it */
    int         sight_distance, radar_distance, sonar_distance, model_height;
                                    /* THE SIGHT FIGURES (WIKICASTDEF's): absent 0 */
    uint64_t    builds;             /* bit i: its build list names type i of the entry */
} WIKIDEF;

/* A FEATURE of the entry's `features` table, sim.js's checkFeature: its TDF section's name and
   figures as tools/wiki/gen-reclaim.mjs reads them. footprint 1..WIKI_FEAT_FOOT cells (the reach
   tagpu_sweep.c's FOOT_MAX gives a feature's centre from its anchor); metal (FeatureDef +0xF0)
   and energy (+0xEC) at least 0 and below WIKI_FEAT_AMOUNT; height (+0xFA) 0..255; blocking, and
   reclaimable (+0xFE bit 7); object, a 3D feature's model (the section's `object=`, a wreck or a
   heap), which the whitebox draws as a wreck, "" for a 2D feature. An entry has at most
   WIKI_FEAT_DEFS, a scene at most WIKI_FEATS_MAX placed, the entry's and the demo's together. */
#define WIKI_FEAT_NAME   32
#define WIKI_FEAT_FOOT   16
#define WIKI_FEAT_AMOUNT 1e7
/* the bound of a signed economy figure, a cast's energy_use: sim.js's ECON_MAX */
#define WIKI_ECON_MAX    1e7
#define WIKI_FEAT_DEFS   64
#define WIKI_FEATS_MAX   256
typedef struct {
    char   name[WIKI_FEAT_NAME];    /* 1..31 bytes */
    int    fx, fz;
    double metal, energy;
    int    height, blocking, reclaimable;
    char   object[WIKI_FEAT_NAME];  /* 1..31 bytes, or "" */
} WIKIFEATDEF;

/* a scene's feature: the table's row and its anchor cell, the footprint's top-left */
typedef struct {
    char   id[WIKI_ID_MAX];         /* "" when the scene gives none: the sim names it */
    int    def;                     /* an index of WIKIENTRY.feat */
    int    ox, oz;
} WIKISCENEFEAT;

/* THE RECLAIM SWEEP'S KINDS, sim.js's KINDS in its order (tagpu_sweep.h's SWEEP_METAL,
   SWEEP_ENERGY and SWEEP_ALL; the popup's rows top down) */
enum { WIKI_KIND_METAL, WIKI_KIND_ENERGY, WIKI_KIND_ALL, WIKI_NKINDS };

typedef struct { int x, z, w, h; double v; } WIKIMETAL;      /* cells, and the metal value     */
typedef struct { int x, z, w, h; int black; } WIKIFOG;       /* cells; black unexplored, else grey */
typedef struct { double x, z; } WIKIPOINT;                   /* world units                    */

typedef struct {
    char             id[WIKI_ID_MAX];   /* "" when the scene gives none: the sim names it */
    int              def;
    int              owner;             /* 0..7; in a game entry one of its scene's seats */
    double           x, z;
    double           built;             /* 1 finished, below it under construction */
    double           hx, hz;            /* "facing" as a unit vector, (0, 1) when absent; the
                                           simulation turns only a mobile unit by it */
    const char*      label;             /* NULL: the type's name */
    int              nmove;
    WIKIPOINT*       move;              /* its move orders, in order */
} WIKISCENEUNIT;

/* A GAME ENTRY'S SEATS (research/notes/ingame-wiki-replay-plan.md): each one a player of the game,
   the seat being its index; its name (HUDSEAT's) 1..WIKI_SEAT_NAME - 1 bytes with no "{", and its
   metal and energy, the skirmish's settings, integers WIKI_SEAT_MIN..WIKI_SEAT_MAX. sim.js's
   SEATS, SEAT_NAME, SEAT_MIN, SEAT_MAX. */
#define WIKI_SEATS     4
#define WIKI_SEAT_NAME 32
#define WIKI_SEAT_MIN  200
#define WIKI_SEAT_MAX  10000
/* A PLAYERS ENTRY'S SEAT also carries `camera`, its view's centre at the game's first tick (world
   units, integers, the view inside the map at the entry's screen: tagpu_wikidata_seat_cam), and
   `team`, 0..WIKI_SEATS - 1, absent its own index; seats of one team are allied to each other. A
   seat with no camera is one whose camera was not recorded. */
typedef struct { char name[WIKI_SEAT_NAME]; int metal, energy; int has_camera, cam_x, cam_z, team; } WIKISEAT;

/* One of the entry's scenes, or a demo's additions to the one it plays on: the demo's units,
   metal, vents and fog go after the scene's, and its selection and cursor replace the scene's when
   it gives them. Only the entry's scenes have a name and a map. */
typedef struct {
    const char*      name;              /* the entry's scene: 1..WIKI_SCENE_MAX-1 bytes; NULL for
                                           a demo's additions                                     */
    int              w, h;              /* cells */
    double           surface_metal;
    int              nmetal, nvent, nfog, nunit, nselected, nfeat;
    WIKIMETAL*       metal;
    WIKIPOINT*       vent;
    WIKIFOG*         fog;
    WIKISCENEUNIT*   unit;
    WIKISCENEFEAT*   feat;
    int              has_selected, has_cursor, has_kind;
    const char**     selected;          /* unit ids */
    WIKIPOINT        cursor;
    int              kind;              /* has_kind: the sweep's kind it starts with, WIKI_KIND_* */
    int              nseat;             /* a game entry's scene: 1..WIKI_SEATS; else 0 */
    WIKISEAT         seat[WIKI_SEATS];
} WIKISCENE;

/* RECLAIM: arm reclaim through RECLAIM (`reclaim: true`); PICK: click the popup's row of `pick`;
   KILL: a game script's kill of the unit `kill` names; HUD: a viewer's press of the replay HUD's
   control `hud`; CAMERA: a players entry's seat's camera moved, recorded data the simulation never
   reads (tagpu_wikidata_seat_cam) */
enum { WIKI_IN_MOVE = 1, WIKI_IN_BUILD = 2, WIKI_IN_CAPTION = 4, WIKI_IN_UNTIL = 8, WIKI_IN_RECLAIM = 16, WIKI_IN_PICK = 32,
       WIKI_IN_KILL = 64, WIKI_IN_HUD = 128, WIKI_IN_CAMERA = 256 };

/* A DEMO'S WAIT (sim.js's UNTILS, in its order): the state a wait holds the demo's clock for.
   IDLE: no unit of the player's holds a build or a reclaim order. ARRIVED: no unit of the
   player's holds a move order. BUILT, a demo's k-th such wait: k buildings of the player's have
   been finished in the demo. SEEKED, a game entry's viewer's only wait (sim.js checkViewer), not
   one of sim.js's UNTILS: no seek is pending (tagpu_wikireplay.h). WIKI_UNTIL_MAX bounds its max,
   in ticks. */
enum { WIKI_UNTIL_IDLE = 1, WIKI_UNTIL_ARRIVED = 2, WIKI_UNTIL_BUILT = 3, WIKI_UNTIL_SEEKED = 4 };
#define WIKI_UNTIL_MAX 3600

/* THE HUD'S CONTROLS a game entry's viewer's script presses (sim.js's HUD_CONTROLS, in its order;
   the game's keys, hud-in-game.md), WIKI_NHUD of them: the replay HUD's eight, then the live HUD's
   own three, then the replay HUD's Take over set (tagpu_hudinput.c pick_act and lane_act): Take
   over, a picker row, the Forks bay, a lane, Back to main recording, a lane's Save as a replay and
   its Delete; then the Players set (tagpu_hudinput.c act): a row's Jump, Sight and Follow, the
   four overlay switches, the stats window's button, its close and a chip. A replay entry's script
   presses the replay HUD's, a live entry's the live HUD's, which share rew, fwd and track
   (tagpu_wikidata_hud_presses), a takeover entry's all three sets (tagpu_wikidata_hud_takeover), a
   players entry's the replay HUD's and the Players set (tagpu_wikidata_hud_players). leave:yes and a
   team row's fold are the player's alone, never a script's: Try it yourself may press them
   (replay.js PRESSES, tagpu_wikireplay_press), so they are numbered after the script's controls and
   have no name the loader reads. */
enum { WIKI_HUD_PLAY, WIKI_HUD_SPEED, WIKI_HUD_REW, WIKI_HUD_FWD, WIKI_HUD_TRACK, WIKI_HUD_BOOKMARK, WIKI_HUD_LEAVE,
       WIKI_HUD_LEAVE_NO, WIKI_HUD_REWIND, WIKI_HUD_LIVE_PLAY, WIKI_HUD_LIVE_NOW, WIKI_HUD_TAKEOVER, WIKI_HUD_TAKEAS,
       WIKI_HUD_LANES, WIKI_HUD_LANE, WIKI_HUD_MAIN, WIKI_HUD_EXPORT, WIKI_HUD_DELETE, WIKI_HUD_JUMP, WIKI_HUD_SIGHT,
       WIKI_HUD_FOLLOW, WIKI_HUD_CURSORS, WIKI_HUD_CLICKS, WIKI_HUD_SELECTIONS, WIKI_HUD_CHAT, WIKI_HUD_STATS,
       WIKI_HUD_CLOSE, WIKI_HUD_CHIP, WIKI_NHUD,
       WIKI_HUD_LEAVE_YES = WIKI_NHUD, WIKI_HUD_FOLD, WIKI_NPRESS };

/* THE STATS WINDOW'S CHIPS a `chip` press names (sim.js's CHIPS, in its order): the two splits, then
   art/hud/layout.json's window.measures keys in that file's order (tools/wiki/castcheck.mjs holds
   the list to the file) */
enum { WIKI_CHIP_PLAYER, WIKI_CHIP_TEAM, WIKI_CHIP_METAL, WIKI_CHIP_ENERGY, WIKI_CHIP_UNITS, WIKI_CHIP_ARMY,
       WIKI_CHIP_KILLS, WIKI_CHIP_LOSSES, WIKI_CHIP_EXCESS, WIKI_NCHIP };

/* One input of a demo's timeline, or one the player gives Try it yourself. */
typedef struct {
    double           at;                /* the tick it is fed on */
    unsigned         has;               /* WIKI_IN_* */
    WIKIPOINT        move;              /* WIKI_IN_MOVE: the cursor, world units */
    int              down, up;          /* WIKI_KEY_*, WIKI_ACT_* (an action, unresolved), or -1 */
    int              build;             /* WIKI_IN_BUILD: the type armed, or -1 to leave the mode */
    const char*      caption;           /* WIKI_IN_CAPTION: the caption, NULL to clear it */
    int              until, until_max;  /* WIKI_IN_UNTIL: the WIKI_UNTIL_* and its max, ticks */
    int              pick;              /* WIKI_IN_PICK: the WIKI_KIND_* */
    const char*      kill;              /* WIKI_IN_KILL: the unit's id, in the entry's document */
    int              hud;               /* WIKI_IN_HUD: the WIKI_HUD_* pressed */
    int              to, drag, over;    /* WIKI_IN_HUD, the track: the game tick pressed, and the
                                           drag's (-1 none) over `over` ticks */
    int              extra;             /* a demo's input names a key besides at, caption, build,
                                           reclaim and pick: it is no caption alone, which a
                                           trip's ticks admit (tagpu_wikidata_trips) */
    int              seat;              /* a game input's seat, whose input state it acts on, and a
                                           takeas, jump, sight or follow press's row, the seat it
                                           names (0 when absent, and on every other input):
                                           0..WIKI_SEATS - 1 */
    int              fork;              /* a lane, export or delete press's fork id, -1 for none */
    int              cam_x, cam_z;      /* WIKI_IN_CAMERA: the view's centre it glides to over `over`
                                           ticks, world units */
    int              by;                /* WIKI_IN_KILL: the killer's seat, -1 for none */
    int              chip;              /* a chip press's WIKI_CHIP_*, -1 for none */
} WIKIINPUT;

/* A GAME ENTRY'S DEMO'S GAME: the game script, played by a game simulation (WIKI_PLAY_GAME) on game
   ticks. `end` an integer 1..WIKI_GAME_END_MAX; each input of a seat of the scene (decision 16:
   every seat acts through its own cursor), and of seat 0 alone unless the sidebar is false (THE
   SEAT RULE, tagpu_wikidata.c), neither caption nor wait, and at most WIKI_GAME_KILLS kills, each
   of a unit of the merged scene by its explicit id, once, at or before the end. sim.js's
   GAME_END_MAX, GAME_KILLS. */
#define WIKI_GAME_KILLS   64
#define WIKI_GAME_END_MAX 5400
/* A PLAYERS ENTRY'S DEATHS, its game script's kills: at most WIKI_PLAYERS_DEATHS units of one seat
   at ticks within any WIKI_PLAYERS_DEATH_SPAN consecutive ticks. The engine's temporary sight
   record add 0x482910 refuses a 21st live record (0x48292C, the scout's P1.5), and a record made at
   d is still live at an add at d' while d >= d' - 61 (the expiry 0x482130 after the step ending at
   d' - 1 keeps d + 60 >= d' - 1). The simulation applies that refusal itself (tagpu_wikisim.c
   kill), so a fork part's kills are held to it too; this rule keeps an authored script from ever
   reaching it, so its death vision is every death of the viewed seat within 60 ticks exactly. */
#define WIKI_PLAYERS_DEATHS     20
#define WIKI_PLAYERS_DEATH_SPAN 62
/* THE SIGHT FLOOR of a players entry's type (its own and every one a casting binds): the stamp's
   radius index is trunc(SightDistance / 32), read from the def's signed word (movsx at 0x48230D),
   and index 0 makes 0x433500 return the 16 bytes before the table's buffer (`begin + (idx - 1) *
   16`, no guard: the scout's P1.2), which the engine leaves undefined; so the wiki never makes one */
#define WIKI_SIGHT_MIN    32
typedef struct {
    int              ninput;
    WIKIINPUT*       input;             /* in file order */
    double           end;
    int              has_sidebar, sidebar;
} WIKIGAME;

/* A LIVE OR TAKEOVER DEMO'S FORK PART (sim.js checkGame): the game inputs the viewer's fork of
   `id` goes on with, made at `from` with `parent` its parent timeline and played as `seat`. id
   1..RECR_TIMELINES - 1, the recorder's own bound on the timelines a recording holds
   (tagpu_recrules.h, ingame-wiki-replay-plan.md decision 9), no two parts of one id; from 2..the
   game's end; parent 0..id - 1; seat one of the scene's (THE SEAT RULE), 0 when absent; each
   input a game input of any seat at a tick from..end, a unit killed at most once in the part and
   at most WIKI_GAME_KILLS kills. A demo holds at most WIKI_DEMO_FORKS. sim.js's TIMELINES and
   DEMO_FORKS.
   A TAKEOVER DEMO'S KEPT FORK, the same record with `end`: a fork the recording's file holds when
   the demo starts, as it was played. At most REC_FORK_LIMIT, id k + 1 at index k; parent
   0..id - 1; from a refresh of the parent's segment after its first (the original's 1, a kept
   parent's from) and at most its end (tagpu_wikidata_refresh_at); seat one of the scene's; end
   from..the game's end; each input at a tick from..end. A part's end is -1. */
#define WIKI_DEMO_FORKS 8
typedef struct {
    int              id, from, parent, seat, end;
    int              ninput;
    WIKIINPUT*       input;             /* in file order */
} WIKIFORK;

/* An entry's frame: a page (the wiki's tutorial page) or a game (`"frame": "game"`, a small game) */
enum { WIKI_FRAME_PAGE, WIKI_FRAME_GAME };

typedef struct {
    const char*      id;
    const char*      title;
    int              on;                /* the index in WIKIENTRY.scenes of the scene it plays on */
    WIKISCENE        scene;             /* its additions */
    int              ninput;
    WIKIINPUT*       input;             /* in file order; the sim plays them by `at`. A game entry's:
                                           the viewer's script, which tagpu_wikireplay plays */
    int              has_end;
    double           end;               /* the tick it ends on */
    int              has_sidebar;       /* `sidebar` given: it decides whether the demo plays with */
    int              sidebar;           /* the sidebar, else a mobile builder of the player's does */
    int              has_game;          /* a game entry's demo: its game script */
    WIKIGAME         game;
    int              nfork;             /* a live or a takeover entry's demo: its fork parts, in file
                                           order */
    WIKIFORK*        fork;
    int              nkept;             /* a takeover entry's demo: the forks its file holds, by id */
    WIKIFORK*        kept;
} WIKIDEMO;

typedef struct {
    const char*      id;
    const char*      title;
    const char*      subtitle;
    int              ntext, nlocked, ndef, ndemo, nfeat;
    const char**     text;              /* paragraphs */
    const char**     locked_in;         /* the mods that turn the feature off */
    double           pace;              /* construction and reclaim run this many times faster */
    int              frame;             /* WIKI_FRAME_*; a game entry has no pace (1) */
    int              live;              /* a game entry's `live`: its demos play a live skirmish with
                                           the rewind, never a replay */
    int              takeover;          /* a game entry's `takeover`: its demos play a replay with the
                                           replay HUD's Take over (never with `live`) */
    int              players;           /* a game entry's `players`: its demos play a replay with the
                                           Players set and a camera (never with `live` or `takeover`) */
    int              screen_w, screen_h;/* a players entry's `screen`, cells: the game screen its
                                           recordings were made on; 0 on any other entry */
    WIKIDEF*         def;               /* in the file's order, which breaks the sim's ties */
    WIKIFEATDEF*     feat;              /* the `features` table, in the file's order */
    int              nscene;            /* 1 or more, every map the first one's size */
    WIKISCENE*       scenes;            /* in the file's order: a demo with no `on` plays on the first */
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
/* The same for an input of a game script (sim.js checkGame's rules for one input): no caption and
   no wait, a `seat` an integer 0..WIKI_SEATS - 1 (the scene's seats and the sidebar bound it
   further where the demo is read), a kill {at, kill} alone, its id pointing into `v`'s tree. */
int  tagpu_wikidata_game_input(const WIKIENTRY* e, const TJV* v, WIKIINPUT* in, char* err, size_t errn);

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

/* sim.js's name of a key or an action: "shift", "alt", "ctrl", "z", "x", "e", "lmb", "rmb",
   "select", "order", "cancel"; NULL out of range. */
const char* tagpu_wikidata_key_name(int key);

/* sim.js's and text.js's name of a cast: "builder", "advbuilder", ... "commander"; NULL out of range. */
const char* tagpu_wikidata_cast_name(int cast);

/* sim.js's name of a sweep kind: "metal", "energy", "all"; NULL out of range. */
const char* tagpu_wikidata_kind_name(int kind);

/* sim.js's name of a replay HUD control: "play", "speed", ... "leave:no"; NULL out of range. And
   the control of a name, or -1. */
const char* tagpu_wikidata_hud_name(int hud);
int         tagpu_wikidata_hud(const char* name);
/* 1 when a demo of a live entry (`live` 1: sim.js's LIVE_CONTROLS) or of a replay entry (0: its
   REPLAY_CONTROLS) presses control `hud`, else 0 */
int         tagpu_wikidata_hud_presses(int hud, int live);
/* 1 when `hud` is one of the replay HUD's Take over set (sim.js's TAKEOVER_CONTROLS), else 0; a
   takeover entry's demo presses these and both sets above */
int         tagpu_wikidata_hud_takeover(int hud);
/* 1 when `hud` is one of the Players set (sim.js's PLAYERS_CONTROLS), else 0; a players entry's demo
   presses these and the replay HUD's, and no other entry's presses one */
int         tagpu_wikidata_hud_players(int hud);
/* sim.js's name of a chip: "player", "team", "metal", ... "excess"; NULL out of range */
const char* tagpu_wikidata_chip_name(int chip);

/* A SEAT'S CAMERA, sim.js's seatCam: the centre of seat `seat`'s view at game tick `t`, into *x, *z;
   1, or 0 when the seat has no camera (its scene gives none: "Camera was not recorded"). The
   scene's `camera`, then each of the seat's camera inputs in file order: at tick t the last one
   with at <= t gives its point once t >= at + over, else each axis a + trunc((b - a) (t - at) /
   over) from the point a it stood at at `at` -- persp_sample's arithmetic on the authored samples
   (research/notes/tadr-port/demo-recorder-perspective-streams.md, *The perspective clock*).
   Integers only; the loader holds a seat's camera inputs in tick order with no glide over another.
   `sc` is the demo's entry scene, `g` its game. */
int         tagpu_wikidata_seat_cam(const WIKISCENE* sc, const WIKIGAME* g, int seat, int t, int* x, int* z);

/* THE REFRESH WALK (sim.js refreshAt): the last GameTime up to T that the recorder's writer flags a
   refresh (reclossy_is_refresh) in a segment whose first iteration is `first`, walked from it with
   the ordinal from 1; `first` itself when T is below first + 1. At most T - first + 1 steps. */
int         tagpu_wikidata_refresh_at(int first, int T);

/* A feature of the entry's table by name, or -1. */
int tagpu_wikidata_feat(const WIKIENTRY* e, const char* name);

/* text.js's BUTTONS: the word of the button token named by the `len` bytes at `s` at interface
   type `iface` (1 exactly when 1, else 0), or NULL when no button has that name. */
const char* tagpu_wikidata_button_word(const char* s, size_t len, int iface);

/* A key or an action as interface type `iface` presses it, sim.js's button(): select is the left
   button at both types, cancel the right one at both, order the right one at type 1 and the left
   one at any other; a key comes back as given. */
int tagpu_wikidata_button(int key, int iface);

/* THE DATA RULE OF A RESOLVED SCRIPT, sim.js's resolve(): the demo's inputs in the simulation's
   order (by tick, then file order), each action turned into its button at `iface`; a press of a
   mouse button the input's seat already holds, or a release of one it does not hold, refuses the
   demo with the input's tick and seat. Each seat has its own hand, from nothing, or with `held`
   from held[2 * seat] (the left button) and held[2 * seat + 1] (the right), WIKI_SEATS seats: the
   buttons a fork's carried seats hold when it takes over. Keys are not held to it: a key's second
   down is an auto-repeat. An input already resolved is its own button. 1 when it holds, else 0
   with a message in `err`. sim.js asks it of every scripted demo. */
int tagpu_wikidata_buttons(const WIKIDEMO* d, int iface, const int* held, char* err, size_t errn);

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
    double   build_cost_metal, build_cost_energy, metal_make, metal_storage, energy_storage;
    double   energy_use;            /* THE ECONOMY'S: finite, an absent one 0. Only a game reads
                                       them, so only a game holds them to the economy's bounds
                                       (tagpu_wikidata_cast_economy): a page entry plays a cast
                                       whatever its economy */
    int      canstop;               /* (canstop & 1), as WIKIDEF's */
    int      activate_when_built;   /* (activatewhenbuilt & 1), as WIKIDEF's */
    int      canreclamate;          /* (canreclamate & 1), as WIKIDEF's */
    /* THE SIGHT FIGURES, an absent one 0. sight_distance, radar_distance and sonar_distance are the
       FBI's SightDistance, RadarDistance and SonarDistance as the def holds them, the words +0x202,
       +0x204 and +0x206 (the FBI reader stores the number's low 16 bits, 0x42C3B3), read signed as
       every reader does (movsx: the stamp at 0x48230D, the radar pass at 0x467565..0x467576):
       -32768..32767. model_height is def+0x170, the byte 0..255: the type loader stores
       0x4CB5F0(model) at +0x16E (0x42D7BF..0x42D7CA), so +0x170 is the integer part's low byte of
       the model's height: over a sibling chain from the root, the most of 0 and, for each piece,
       its vertices' y and its child chain's height, each plus the piece's y offset, in 16.16
       (0x4CB5F0..0x4CB641, DISASSEMBLED; the side load's tagpu_wikiasset.c height_chain and gfx.py
       model_height compute it from the 3DO, the 3D load's model_top from its arena). MEASURED
       2026-11-01 (M0-P1, the def bytes read in game): ARMCOM 39, ARMCK 27, ARMSOLAR 38, CORAK 26.
       A cast is activated when finished exactly when activate_when_built is set, the radar pass's
       test (+0x10E bit 0, set at 0x486135..0x486148 and 0x41B9AC..0x41B9BF). */
    int      sight_distance, radar_distance, sonar_distance, model_height;
    int      vent;                  /* has a G cell */
    uint16_t g[WIKI_FP_MAX];        /* as WIKIDEF's; inside fx x fz */
    uint16_t builds;                /* bit c: its build menu holds cast c's type */
    int      has_corpse;            /* 0: `corpse` null or absent, the type leaves no wreck */
    WIKIFEATDEF corpse;             /* its wreck's feature, held to the entry table's rules */
} WIKICASTDEF;

typedef struct { WIKICASTDEF cast[WIKI_NCAST]; } WIKICASTING;

/* 1 when a cast's economy is within the bounds a game's economy rests on: the build costs, the
   metal made and the storages >= 0 and the energy use of a magnitude below WIKI_ECON_MAX (a solar
   collector's is negative); 0 leaves the cast unbound in a game (tagpu_wikisim.c bind, sim.js
   binder) and in a game only */
int tagpu_wikidata_cast_economy(const WIKICASTDEF* k);

/* A side's casting from its JSON record ({"casts": {...}}; `side`, `commander`, `report` and any
   other key are read past) into `c`, held to every bound of sim.js's checkCasting: all ten casts
   given, each null or a record whose type, title, footprint, figures, canstop, canreclamate,
   activatewhenbuilt, vent, g, builds and corpse hold. Beyond it, a cast named twice is refused (a
   JSON object keeps one of them for sim.js). 1, or 0 with a message in `err` and `c` zeroed: a refused casting is not used at all. */
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
/* the deepest column, every compatibility setup's: a stage's map grows to its column
   (tagpu_wiki_map_rows), and every stage description's ground is WIKI_SIDE_ROWS / 16 rows deep
   (tools/wiki/stagemap.py GROUND_ROWS); the composed column's texture is 128 x WIKI_SIDE_ROWS */
#define WIKI_SIDE_ROWS     768
#define WIKI_GROUND_ROWS   (WIKI_SIDE_ROWS / 16)  /* cells: every stage description's ground */

/* THE MAP'S ROWS, sim.js THE MAP: a scene of `scene_rows` beside a column of `column_rows` (0 for
   none) is grown downward by whole 32-unit tiles until it is at least as tall as the column, so no
   band lies under it. A column outside 1..WIKI_SIDE_ROWS grows nothing, so the rows are at most
   the larger of the scene's and WIKI_GROUND_ROWS. The game thread's simulation, the page's fit of
   a press onto the drawn stage (tagpu_wiki.c stage_world) and the loader's minimap all take the
   map from here, so they cannot disagree on it. */
static inline int tagpu_wiki_map_rows(int scene_rows, int column_rows)
{
    const int grown = column_rows > 0 && column_rows <= WIKI_SIDE_ROWS ? 2 * ((column_rows + 31) / 32) : 0;
    return grown > scene_rows ? grown : scene_rows;
}

/* THE GAME FRAME (tagpu_wikiframe.h): the column's width and each bar's rows, game pixels */
#define WIKI_FRAME_SIDE_W  128
#define WIKI_FRAME_BAR     32
/* THE GAME FRAME'S 4:3 BOUND: 1 when a game entry's map of `w` x `h` cells makes a frame the
   replay HUD fits, Wg 720 / Hg >= 960 units, i.e. 3 Wg >= 4 Hg (the HUD fits at 4:3, MEASURED at
   1024 x 768: hud-in-game.md). Wg is 128 + 16 w; Hg is taken at its largest, the map grown beside
   the tallest column there can be (tagpu_wiki_map_rows with WIKI_SIDE_ROWS) and both bars, so the
   bound holds beside every layout's column whether the map grows or not. A map of at most
   WIKI_GROUND_ROWS rows needs w >= 62. tagpu_wikidata_entry refuses a game entry that fails it,
   as sim.js checkGame does, so every frame the stage draws is at least 4:3 by construction. */
int tagpu_wikidata_frame_fits(int w, int h);
/* THE SAME BOUND FOR A PLAYERS ENTRY, whose frame is its screen's (sw x sh cells): Wg = 128 + 16 sw,
   Hg = max(16 sh + 64, the deepest column's WIKI_SIDE_ROWS), 3 Wg >= 4 Hg. tagpu_wikidata_entry
   takes it instead of the map's on a players entry, as sim.js checkGame does. */
int tagpu_wikidata_screen_fits(int sw, int sh);
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
    int  key;                       /* the record's quickkey +0x13A, 1..255; 0 for none */
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

/* THE SIGHT TABLE of a layout's mod: <gamedata>\los.tdf as the engine's loader stores it
   (0x433130, 0x433380, 0x4336F0, 0x4339E0, DISASSEMBLED; the scout's P1.1), reduced to count
   grids. The loader makes `numtables` elements, element i from [TABLE<i+1>]; each of its `numlines`
   lines (`line<k>=count, x1, y1, ...`, strtok on ", ", each number atoi) is stored four times, as
   rays k, N+k, 2N+k and 3N+k, each pair (x, y) as the (column, row) offset (x, -y), (y, x), (-x, y)
   and (-y, -x): two reflections and two axis swaps, no point merged. The stamp 0x482270 adds one to
   the unit's own block (0x48239E) and one to the block under every point of every ray of element
   idx - 1, idx = min(trunc(sight / 32), numtables - 1) (0x48230D..0x482366, 0x433500), where the
   height test passes -- on the stage's flat ground everywhere (MEASURED M0-P1: no block of the
   set unlit). So a table loses nothing as a COUNT GRID, each offset with the number of ray points
   on it, in any order. MEASURED M0-P1 with the stock file (numtables 9): a unit's lit set is its
   table's offsets and its own block, 213 / 177 / 137 blocks at idx 8 / 7 / 6.
   ONLY THE TABLES THE SETUP'S TYPES REACH ARE KEPT, sparse by index: the idx of each sight of a
   cast the layout's castings bind and of each type a players entry of the wiki's tree defines,
   at least WIKI_SIGHT_MIN (tagpu_wikiasset.c sight_read, gfx.py `sight_table`). Stored table k is
   radius index idx[k] (strictly increasing, each 1..numtables - 1) and cell[first[k] ..
   first[k + 1]), its offsets strictly increasing by (dr, dc). A type whose index is not stored
   plays in no players entry's game over this table (tagpu_wikidata_sight_ok): a cast is left
   unbound and an entry's own type refuses the game, so the stamp never reads a table the layout
   does not hold. numtables is not bounded above beyond the engine's signed word (the clamp
   makes every index below it); MEASURED 2026-11-02 over the compat setups' files: numtables 9
   (stock), 64 (Mayhem), 90 (ProTA, Escalation), 94 (Twilight), 63 (TA Zero), and the tables their
   castings reach 4 / 6 / 6 / 11 / 6 / 7 indices of 560 / 1200 / 1200 / 15212 / 1800 / 3900 cells,
   Twilight's reaching 38 blocks with a count of 39. THE BOUNDS a producer holds or keeps no table
   (numtables 0: Sight lights every block, with a report line): numtables at least 2 (1 or less
   gives idx 0 for every unit, the read before the buffer); at most WIKI_SIGHT_IDX indices, about
   three times Twilight's 11; in a stored table every point within WIKI_SIGHT_REACH blocks on both
   axes, a count 1..255 and every line's numbers present (a missing one is atoi(NULL), undefined in
   the engine); and the stored tables' cells together at most WIKI_SIGHT_CELLS, about twice
   Twilight's 15212 (3 bytes a cell: 96 KB a layout). */
#define WIKI_SIGHT_NUMTABLES 32767
#define WIKI_SIGHT_REACH  63
#define WIKI_SIGHT_IDX    32
#define WIKI_SIGHT_CELLS  32768
typedef struct { int8_t dc, dr; uint8_t n; } WIKISIGHTCELL;
typedef struct {
    int           numtables;        /* 0: the layout has no table */
    int           nidx;             /* the tables stored */
    int           idx[WIKI_SIGHT_IDX];
    int           first[WIKI_SIGHT_IDX + 1];
    WIKISIGHTCELL cell[WIKI_SIGHT_CELLS];
} WIKISIGHT;

/* THE RADIUS INDEX of a sight over a layout's table, the engine's min(trunc(sight / 32),
   numtables - 1); 0 below WIKI_SIGHT_MIN or with no table (numtables 0), where no table is read */
int tagpu_wikidata_sight_index(const WIKISIGHT* t, int sight);
/* The stored table a sight stamps with: k with t->idx[k] its radius index, or -1 when it has none
   (below WIKI_SIGHT_MIN, no table, or an index the layout does not store) */
int tagpu_wikidata_sight_table(const WIKISIGHT* t, int sight);
/* 1 when a type of `sight` plays in a players entry's game over table `t` (NULL: no layout): its
   sight at least WIKI_SIGHT_MIN (THE SIGHT FLOOR) and, when `t` holds a table, its radius index
   stored. A cast that fails it is left unbound (tagpu_wikisim.c bind), an entry's own type refuses
   the game (tagpu_wikisim_init), as sim.js's sightFits */
int tagpu_wikidata_sight_ok(const WIKISIGHT* t, int sight);

typedef struct WIKILAYOUT {
    int         format, width, height;
    int         nside;
    WIKISIDEDEF side[WIKI_SIDE_SIDES];
    int         ntype;              /* over every side */
    WIKIPAGES   type[WIKI_SIDE_TYPES];
    int         nscreen;
    WIKISCREEN  screen[WIKI_SIDE_SCREENS];
    WIKISIGHT   sight;
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

/* THE TRIPS' DATA RULE, sim.js's checkTrips, for a demo that plays with the sidebar: every
   `build` names a type, a `reclaim` and a `pick` are trips too; each carries no other input and
   finds the left button up; no input but a caption stands in its trip, the ticks (t, t + WIKI_TRIP)
   for a build or a reclaim and (t, t + WIKI_PICK) for a pick, nor after it within tick t; and the
   demo lasts the whole trip. The inputs are read in the simulation's order (by tick, then file
   order), each action as its button at `iface`, as sim.js reads its resolved script. 1 when it
   holds, else 0 with a message in `err`. tagpu_wikisim_init asks it exactly where sim.js throws:
   a scripted demo whose sidebar is on. */
int tagpu_wikidata_trips(const WIKIDEMO* d, int iface, char* err, size_t errn);

#endif
