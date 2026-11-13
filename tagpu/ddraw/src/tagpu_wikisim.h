#ifndef TAGPU_WIKISIM_H
#define TAGPU_WIKISIM_H
/* tagpu_wikisim -- the in-game wiki's simplified simulation: a scene and its inputs in, a view of
   the game out. This header, its files and research/notes/ingame-wiki.md, "The simulation's
   contract", are its specification; the browser mockup runs this same C as a host process
   (tests/wikihost.c, tools/wiki/mockup/simhost.js). A demo is its scene and its inputs played through it, and Try it yourself is the same
   simulation taking the player's inputs instead.

   EVERY BUILD AGREES BIT FOR BIT. It visits units in creation order, uses no clock and no
   randomness, and computes on doubles with + - * / and sqrt only, each rounded once, plus trunc,
   floor and comparisons, which are exact, so the same inputs give the same doubles.
   tools/wiki/simparity.mjs plays every demo of every entry, with the fixture's sidebar, the tall
   fixture's and none, with each casting of tools/wiki/casting-fixture.json and none, at both
   Interface Types, and simcheck.mjs's rule cases through the host's three builds (native, under
   the sanitizers, and the DLL's i686 build under Wine) at once, and compares every answer --
   every field of the view, every unit's orders and the state hash -- tick by tick. The DLL compiles tagpu_wikisim.c and the core's tasim_*.c for SSE2
   (Makefile, THE WIKI'S SIMULATION) so that no intermediate keeps x87's extra precision.

   ONE SIMULATION, TWO PARTS: the shared simulation core, the tasim_*.c files over tasim.h
   (research/notes/impai/shared-core.md), holds the world, the orders, movement, the economy, the
   work, sight, the units' scripts and the spray and sound, in its TASIM (tasim_state.h), which
   WIKISIM embeds as `sim` beside the definitions it plays, `defs`; tagpu_wikisim.c is the wiki's
   front end over it, the UI emulation, scripted input, selection, the scene and the casting, and
   these entry points. Both keep this header's contract; the core reads nothing of the front end's
   and tells it of a unit's entry, leaving and completion through its unit hook.

   ENGINE-FREE, WIN32-FREE AND WITHOUT GLOBALS. It reads no engine memory and names no engine
   address: where it mirrors the rule code of tagpu_resource.c or tagpu_place.c, which read the
   engine, it says which function each piece mirrors -- except what the game
   keeps engine-free in tagpu_placegeo.c, the shapes and the take-off rule, which it calls
   directly as the game does. Every piece of its state lives in the WIKISIM its caller owns
   (and on the heap that WIKISIM owns), so it runs on whichever thread owns the wiki window;
   one WIKISIM is used by one thread at a time. The WIKIENTRY (and the WIKIDEMO) it was made
   from must outlive it: it points into them. So must its sidebar layout and its context's
   casting, which it reads and never writes.

   A SIM IS MADE FOR A CONTEXT, WIKISIMCTX: the layout's side the player plays,
   that side's casting (the mod's own type for each of the entry's casts, or none for the entry's
   own types) and the Interface Type. With a casting it plays the BOUND ENTRY, its own copy of
   the entry whose types are the casting's (tagpu_wikisim_init says how), and every type name it
   shows -- the view's, the hash's, WIKISIM.defs.entry's -- is the bound one.

   BOUNDED BY CONSTRUCTION: a unit's orders stop at PLACE_LIST_CAP (every append goes through
   one function that refuses past it), and the map at WIKI_MAP_MAX cells a side (the loader
   refuses a larger one), so a player's clicks cannot grow the heap without bound.

   Time is ticks, 30 a second; positions are world units, a footprint cell 16. The player is
   owner 0. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_placegeo.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikicob.h"
#include "tasim_state.h"

/* THE BUILD TRIP (the host's `hello` gives these to the mockup). A scripted demo with the sidebar plays a `build` input at tick t as the cursor's
   trip to the button: it takes WIKI_TRIP ticks whatever the page, the pagers or the mod, the
   button goes down at t + WIKI_TRIP_DOWN and is released at t + WIKI_TRIP_UP, which is where it
   arms. Because the arm tick is fixed, the mode and everything else in the world are the same in
   every mod at every tick; only the cursor and the sidebar differ. Row k is the trip through k
   pager presses (k <= 3: the nearer way round WIKI_SIDE_PAGES = 7 pages is at most 3): the glide
   to the pager (to the button itself for k = 0), each press held `hold` ticks then `gap` ticks
   before the next, the glide from the pager to the button, and the hover on it. Every row ends on
   the button's down (tagpu_wikisim.c checks it as it compiles). */
#define WIKI_TRIP       90
#define WIKI_TRIP_DOWN  45
#define WIKI_TRIP_UP    60
/* glide, hold, gap, back, hover */
#define WIKI_TRIP_ROW0  30, 0, 0, 0, 15
#define WIKI_TRIP_ROW1  12, 15, 3, 12, 3
#define WIKI_TRIP_ROW2  9, 10, 3, 10, 0
#define WIKI_TRIP_ROW3  6, 8, 3, 6, 0
#define WIKI_TRIP_ROWS  { { WIKI_TRIP_ROW0 }, { WIKI_TRIP_ROW1 }, { WIKI_TRIP_ROW2 }, { WIKI_TRIP_ROW3 } }
#define WIKI_TRIP_NROWS 4
/* the inputs one trip makes, at most: row 0's 30 glide moves, the button's down and up, and the
   30 moves back (tagpu_wikisim.c checks every row against it as it compiles) */
#define WIKI_TRIP_EVENTS 64

typedef struct { int glide, hold, gap, back, hover; } WIKITRIPROW;

/* THE PICK TRIP: a scripted `pick` of a kind at tick t is
   the glide to the popup's row of that kind, its press at t + WIKI_PICK_DOWN and release at
   t + WIKI_PICK_UP, and the glide back, WIKI_PICK ticks whatever the layout; the row is where the
   popup stands when the pick is fed. A `reclaim` is the build trip's to RECLAIM (WIKI_TRIP). */
#define WIKI_PICK       60
#define WIKI_PICK_DOWN  20
#define WIKI_PICK_UP    26

/* the lock of a RECLAIM the layout cannot reach: WIKIVIEW.locked and the hash name it "RECLAIM" */
#define WIKI_LOCK_RECLAIM (-2)

/* A unit type's sounds as the simulation needs them: its sounds.tdf category (-1 none) and, per
   kind, the variant count the engine reads at rec + 0x40 + 12 x kind. */
typedef struct {
    int     cat;
    int32_t count[WIKI_SND_KINDS];
    int     death;                  /* its death's WAV, an index of the load's sound table
                                       (WIKI3DUNIT.death_sound), -1 none; the simulation does not
                                       read it, the player does (tagpu_wiki.c death_play) */
} WIKISNDTYPE;

/* Where a sim's sounds come from: for the type named `name` its WIKISNDTYPE into `out`, 1; 0 when
   the source has none, which leaves the type silent. NULL in WIKISIMCTX: no sound at all. */
typedef struct WIKISNDSRC {
    void* ctx;
    int (*type)(void* ctx, const char* name, WIKISNDTYPE* out);
} WIKISNDSRC;

/* A CLICK of a game's seat (tagpu_wikisim.c click_action): the tick its input was fed on, the
   seat, and the seat's map cursor. A game keeps the WIKI_CLICKS most recent, the replay overlay's
   ring (OV_RING, tagpu_replay.c:484). */
#define WIKI_CLICKS 64
typedef struct { int tick, seat; double x, z; } WIKICLICK;

/* a game view's seat's cursor: its map cursor, `on` once it has moved and while it is over the map */
typedef struct { double x, z; int on; } WIKIVCURSOR;

enum { WIKI_SITE_PLACE, WIKI_SITE_REFUSED, WIKI_SITE_REMOVE };

typedef struct { int cx, cz, state; } WIKISITE;

/* tagpu_place.c's s_eval: the gesture's last evaluation; kind PLACE_NONE when there is none */
typedef struct {
    int      kind, type, n;
    WIKISITE site[PLACE_MAX_SITES];
} WIKIPLACE;

/* NONE: the cursor is off the map, over the sidebar or the dead band under a short map */
enum { WIKI_HOVER_GROUND, WIKI_HOVER_UNIT, WIKI_HOVER_PLACE, WIKI_HOVER_QUICK, WIKI_HOVER_NONE };

typedef struct { int kind, type, ox, oz, ok; } WIKIHOVER;

/* THE CURSOR'S THREE SHAPES, each the cursor the game shows there (MEASURED 2026-10-27, the
   installed index main+0x2CBE read back): nothing of the player's mobile selected, cursornormal;
   a mobile unit of the player's selected, cursormove -- at Interface Type 1 too, which Impure's
   contextual-cursor patch makes so -- and over a deposit as well, since Quick Build's ghost is
   the packet's alone and writes no engine cursor (tagpu_resource.c); a building armed from the
   build menu, cursornormal again, because 0x499200 calls the build cursor 0x4197D0 instead of
   choosing one and the click on the menu installed cursornormal. */
/* With reclaim armed it is cursorreclamate over the map, RECLAIM; tagpu_wikisim.c's cursor rule says
   where it is the arrow instead. */
enum { WIKI_CURSOR_ARROW, WIKI_CURSOR_MOVE, WIKI_CURSOR_BUILD, WIKI_CURSOR_RECLAIM, WIKI_CURSOR_N };

/* ---- the view: what the stage draws; the host's `view` writes it field for field ---- */

/* a feature standing, the view's feats: its id (the sim's), its row of the entry's table, its
   anchor cell, footprint and amounts, and whether its row names a model (a wreck) */
typedef struct {
    const char* id;
    int         def, ox, oz, fx, fz;
    double      metal, energy;
    int         wreck;
} WIKIVFEAT;

/* THE SWEEP as tagpu_sweep_fill publishes it, the view's reclaim: while reclaim is armed, the
   kind; during a drag past the threshold the ring (whole world units), its targets as indices into
   the view's feat in the ring's order, and the readout's counts and sums (a feature with metal is
   a wreck, one with energy alone a tree); otherwise the feature under the pointer the kind takes,
   bracketed (an index into feat, -1 none); and the popup, its rows and its tongue in the stage's
   coordinates (x from the map's left edge, z the column's row), with the row under the pointer
   (-1 none) */
typedef struct {
    int    kind;                    /* WIKI_KIND_* */
    int    has_ring, ring_x, ring_z, ring_r;
    int    ntarget;
    int    target[WIKI_FEATS_MAX];
    int    wrecks, trees;
    double metal, energy;
    int    hover;
    int    has_popup;
    int    rect[4], rows[WIKI_NKINDS][4], tongue[4];
    int    hot;
} WIKIVSWEEP;

/* a ghost, a queued site or a gesture's site: a building's centre and footprint */
typedef struct {
    int    type, fx, fz;
    double x, z;
    int    ok;                      /* the ghost: green, else red */
    int    state;                   /* a gesture's site: WIKI_SITE_* */
    int    order;                   /* a gesture's site: its place among the orders the release adds, 0 for none */
    int    owner;                   /* a queued site: the owner of its order */
} WIKIVSITE;

typedef struct {
    double from_x, from_z, to_x, to_z;
    int    kind;                    /* WIKI_ORDER_* */
} WIKIVLINE;

/* The sidebar as the stage draws it, the view's side; by value and pointer-free, so a copy
   holds across threads. */
typedef struct {
    int              index;         /* the layout side it shows (WIKISIMCTX.side): its prefix and
                                       PANELSIDE are the stage's */
    int              h;             /* the column's rows: the layout's height */
    int              screen;        /* the routed screen, an index < the layout's nscreen */
    char             unit[WIKI_ID_MAX];     /* the shown unit's id, "" for none */
    int              page;          /* the shown unit's page, 0 for none */
    int              nstate;        /* the screen's gadget count */
    int              state[WIKI_SIDE_GADGETS];  /* per gadget: -1 not drawn, 0 normal, 1 pressed
                                                   or the current tab, 2 greyed */
    char             under[WIKI_ID_MAX];    /* the unit under the cursor over the map (the
                                               minimap's high dot), "" for none */
} WIKIVSIDE;

/* What one input seat's state shows (with the reclaim's armed, kind and ring):
   the view's ghost, gesture sites and shape, spacing, cursor, keys, drag and armed mode, as
   WIKIVIEW holds them for the seat it is viewed as */
typedef struct {
    int              has_ghost;
    WIKIVSITE        ghost;
    int              nsite;
    const WIKIVSITE* site;
    int              shape;         /* PLACE_*, PLACE_NONE for none */
    int              spacing;       /* the armed building's, -1 when none is armed */
    double           cursor_x, cursor_z;
    int              cursor_shape, cursor_since;
    int              held[WIKI_NKEYS];
    int              has_drag;
    double           drag_x0, drag_z0, drag_x1, drag_z1;
    int              mode;          /* the armed type, or -1: contextual */
    int              rmode;         /* reclaim is armed: kind and the ring are its */
    int              kind;          /* WIKI_KIND_* */
    int              has_ring, ring_x, ring_z, ring_r;
} WIKIVSEAT;

typedef struct {
    int              w, h;
    double           surface_metal;
    const double*    metal;         /* w * h, row-major */
    const unsigned char* fog;       /* w * h: 0 clear, 1 grey, 2 black */
    int              nvent;
    const WIKIPOINT* vent;          /* each vent cell's centre */
    int              nunit;
    const WIKIVUNIT* unit;
    int              has_ghost;
    WIKIVSITE        ghost;
    int              nsite;
    const WIKIVSITE* site;
    int              shape;         /* PLACE_*, PLACE_NONE for none */
    int              spacing;       /* the armed building's, -1 when none is armed */
    int              nqueued;
    const WIKIVSITE* queued;
    int              nline;
    const WIKIVLINE* line;
    double           cursor_x, cursor_z;
    int              cursor_shape;  /* WIKI_CURSOR_* */
    int              cursor_since;  /* the tick its sequence began: the game restarts a cursor's
                                       animation when it installs another (SetUICursor
                                       0x4AB400), and the build shape's is the arrow's */
    int              held[WIKI_NKEYS];
    int              has_drag;
    double           drag_x0, drag_z0, drag_x1, drag_z1;
    const char*      caption;       /* NULL for none */
    double           pace;
    int              clock;         /* the demo's clock */
    int              waiting;       /* the WIKI_UNTIL_* a wait holds the clock for, 0 for none */
    int              stalled;       /* a wait ran past its max: the demo stopped */
    int              has_cob;       /* the sim runs its units' scripts: cob_hash is the host's */
    uint64_t         cob_hash;      /* tagpu_wikicob_hash from the FNV-1a offset basis */
    int              npart;         /* the nano spray's live particles, in the order made */
    const WIKIVPART* part;
    int              part_cut;      /* a particle or a burst was past its bound */
    int              nburst;        /* the second bursts waiting for their tick, in the order made */
    const WIKISPRAY* burst;
    int              has_sound;     /* the sim makes sound: the fields below are its */
    int              snd_next[WIKI_SND_KINDS];  /* each kind's next time (0x5086EC + 24 x kind) */
    int              snd_last;      /* the last reply with sound's time, [q+0x9D]            */
    int              nsndq;         /* the queue, highest priority first                     */
    const WIKISNDQ*  sndq;
    int              snd_total;     /* the events made since the sim was made                */
    int              nsnd;          /* the last min(snd_total, WIKI_SND_LOG), oldest first:
                                       event i is the (snd_total - nsnd + i)-th from 0         */
    const WIKISNDEV* snd;
    int              mode;          /* the armed type, or -1: contextual */
    int              rmode;         /* reclaim is armed (mode is -1 then): sweep is the view's */
    WIKIVSWEEP       sweep;
    int              nfeat;         /* the features standing, in the scene's order */
    const WIKIVFEAT* feat;
    int              ngone;         /* the anchor cells of the features reclaimed so far, in order:
                                       gone[2k], gone[2k + 1] */
    const int*       gone;
    int              has_side;      /* the sidebar is on: side is the view's */
    WIKIVSIDE        side;
    int              locked;        /* the type the demo stopped on, with the sidebar or without,
                                       or -1 while it plays: a scripted build the layout cannot
                                       reach, or a scene unit or a build of a cast the casting
                                       leaves unbound; WIKI_LOCK_RECLAIM for a scripted reclaim
                                       or pick the layout cannot reach */
    /* a game simulation's own part (`game` 1): its seats' names and economies, the economy's mode
       (0 refuse, 1 stall), whether a pass starved and a kill found no unit, the kills made, and
       each view unit's ledger, index-aligned with `unit` */
    int              game;
    int              nseat;
    const WIKISEAT*  seat;
    const WIKIECON*  econ;
    int              econ_mode, starved, bad_kill, nkill;
    const WIKILEDGER* ledger;
    /* a game simulation's overlays and sight (`game` 1): each input seat's cursor (ncursor = nin),
       the clicks kept, oldest first, the deaths in the order made, and each of the entry's types'
       sight figures, index-aligned with the bound entry's types (a unit's and a death's `type`) */
    int              ncursor;
    const WIKIVCURSOR* cursor;
    int              nclick;
    const WIKICLICK* click;
    int              ndeath;
    const WIKIDEATH* death;
    int              ntype;
    const WIKIVTYPE* type;
    /* a game simulation's input seats (WIKISIM.nin) and its viewed seat; with more than one, seats
       1..nin - 1's own parts, input[k - 1] for seat k (the view's inputs) */
    int              nin, viewed;
    const WIKIVSEAT* input;
} WIKIVIEW;

/* The context a sim is made for. tagpu_wikisim_init refuses an iface other than 0
   or 1 and, with a layout, a side that is not one of its sides. LIFETIME: `cast` must outlive the
   sim, which holds it (its bound types' titles point into it); in the DLL it lies inside the
   published WIKISIDE block, immutable and never freed. */
/* Where a sim's unit scripts come from (the drop-in's ctx.packs): for the type named `name`, its script
   and its posed model's facts into `out` (borrowed until the call returns), 1; or 0 when it has
   none, which quarantines it. NULL in WIKISIMCTX: no host, every unit at rest and every gate open. */
typedef struct WIKICOBSRC {
    void* ctx;
    int (*type)(void* ctx, const char* name, WIKICOBTYPE* out);
} WIKICOBSRC;

typedef struct {
    int                side;        /* the layout's side the player plays: its MAIN2, GEN and
                                       types; not read without a layout */
    const WIKICASTING* cast;        /* that side's casting, or NULL: the entry's own types */
    int                iface;       /* the Interface Type: 0 left-click, 1 right-click */
    const WIKICOBSRC*  cob;         /* the units' scripts, or NULL; read during init only */
    const WIKISNDSRC*  snd;         /* the units' sounds, or NULL; read during init only */
    const WIKICASTING* rival;       /* the opponent's casting, read by a game entry's rival types
                                       only, or NULL: they keep their own; the same lifetime as cast */
} WIKISIMCTX;

struct WIKISIMBOUND;

/* A SEAT'S INPUT STATE (ingame-wiki-replay-plan.md decision 16): its cursor, from the scene's; the shape the
   cursor shows and the tick its sequence began; the keys and buttons it holds, and where each
   button went down (OVER_*), which decides whether its release runs the map rules; the armed
   building (-1 none) and the drag; the placement gesture and its last evaluation; the spacing per
   type; the reclaim sweep -- armed (main+0x2CC3 == 12, exclusive with a building armed), its kind,
   and its gesture: the press point, the last point over the map, past the threshold or not; the
   hover; fed, the inputs the seat's script (seat 0's trips too) has fed since the sim was made,
   its kills and waits not counted: the replay layer's clash rule reads it, and no hash does; and
   moved, whether it has moved its cursor (the view's cursors). */
typedef struct {
    double           cursor_x, cursor_z;
    int              cursor_shape, cursor_since;
    int              held[WIKI_NKEYS];
    int              down_on[WIKI_NKEYS];
    int              mode;
    int              has_drag, drag_active, drag_t;     /* drag_t: the press's tick */
    double           drag_x0, drag_z0;
    WIKIHOVER        hover;
    int              has_gesture;
    PlaceCell        gesture;
    int              shift_placed;
    int              spacing[WIKI_MAX_DEFS];
    WIKIPLACE        place;
    int              rmode, kind, has_rg, rg_drag;
    double           rg_x0, rg_z0, rg_x1, rg_z1;
    int              fed;
    int              moved;         /* the seat has moved its cursor: the game records a seat's
                                       cursor from its first move. Not hashed: it decides nothing */
} WIKISEATIN;

/* ---- the simulation: the caller owns it; sim.tick and done are its to read, the rest is private ---- */

typedef struct {
    /* THE CORE'S STATE (tasim_state.h) and the definitions it plays: defs.entry is the entry the sim
       was made from, or with a casting the bound entry, which `bound` holds on the heap; its type
       names are the casting's. The core calls back through sim.hook, which init points at this
       WIKISIM, so a WIKISIM stays where init made it. */
    TASIM            sim;
    TASIM_DEFS       defs;
    int              done;
    int              side;          /* the layout side the sidebar shows, -1 with it off */
    /* a game's seats, as its scene names them (their economies are sim.econ) */
    WIKISEAT         seat[WIKI_SEATS];
    /* a game's clicks, a ring at nclicks % WIKI_CLICKS (nclicks the clicks made); not hashed */
    WIKICLICK        clicks[WIKI_CLICKS];
    int              nclicks;
    int              iface;         /* the Interface Type, 0 or 1 */

    /* the bound entry on the heap (a heap block rather than a field, so that defs.entry never points
       into the WIKISIM itself) */
    struct WIKISIMBOUND* bound;
    unsigned char    unbound[WIKI_MAX_DEFS];    /* type i is of a cast the casting leaves
                                                   unbound: a scene unit or a build of it locks */
    int              SH;            /* the scene's rows: the map is the scene's sim.W x SH, grown to
                                       sim.H rows beside the column */

    /* THE INPUT SEATS: in[k] for k < nin, a game simulation's seats its game
       script or a fork part names (input_seats), 1 for any other; and the acting seat, whose input
       state every input rule reads. The viewed seat, whose units' notices play, is sim.viewed (0
       until a fork takes over another seat, as the game's takeover makes the taken seat the viewed
       one, tagpu_replay.c tk_seats).
       THE ORDERING: act is 0 between calls. tagpu_wikisim_input and a fed script input act as
       their seat, tagpu_wikisim_view_as views as its seat and the step's tail visits each seat in
       turn, and each puts act back before it returns. Nothing the input rules call re-enters input
       or the view -- the COB host and the sound are called by the step and read units, never input
       state -- so every read of in[act] is the acting seat's.
       THE COLUMN IS SEAT 0'S: a game with the sidebar has nin 1 (init refuses more), so its router,
       gadgets, capture and trips only ever run with act 0. */
    WIKISEATIN       in[WIKI_SEATS];
    int              nin, act;
    const char*      caption;       /* the demo's, no seat's */

    /* the script: the demo's inputs resolved at `iface` (each action its button) into `resolved`,
       the sim's own copy, then ordered by tick, file order breaking ties */
    int              nscript, next;
    WIKIINPUT*       resolved;
    const WIKIINPUT** script;
    double           end;           /* the clock it is done on; never for Try it yourself */
    const char*      demo_id;       /* the demo's id, for a refused fork's message; it lives in the
                                       entry's document, which the caller keeps as long as the sim */
    /* THE DEMO'S CLOCK: the tick its inputs, trips and end are written in. It runs
       with sim.tick except while a wait holds it: waiting the WIKI_UNTIL_* it waits for (0 none),
       from the tick it began, at most wait_max ticks; one past its max stops the demo, stalled */
    int              clock, waiting, wait_from, wait_max, stalled;
    int              nbuilt, built_waits, wait_built;  /* the buildings of the player's finished so
                                                         far (TASIM_UNIT_BUILT), the BUILT waits
                                                         begun, and the count this one needs: its
                                                         place among them */

    /* the sidebar; `lay` NULL: off for the sim's whole life */
    const WIKILAYOUT* lay;
    /* DZ: in the frame the column starts at the top bar's first row (stage.js drawFrame),
       WIKI_FRAME_BAR above the map; a page's column starts with the map. 0 on a page, so every
       page demo's arithmetic is the same doubles. A column's row meets a world z only through
       row_of and the z = row + dz of the popup and a gadget's centre. */
    int              dz;
    int              trips;         /* a scripted demo with the sidebar: a build is its trip */
    int              ltype[WIKI_MAX_DEFS];  /* each entry type's index in lay->type, or -1 */
    int              screen;        /* the routed screen, an index < lay->nscreen */
    WIKIUNIT*        shown;         /* main+0x37E9C; the unit hook's LEAVE clears it with its unit */
    int              cap, cap_key;  /* the gadget of `screen` a press captured (-1 none), and the
                                       button that pressed it (gi+0x64, one slot, so a second
                                       press takes it over) */
    int              locked;        /* the type the demo stopped on (the first lock wins), or -1 */
    /* the trip's generated inputs, fed before the script's of the same tick */
    WIKIINPUT        trip_in[WIKI_TRIP_EVENTS];
    int              ntrip_in, trip_next;
    int              feeding;       /* the tick being fed, and between two ticks the one the
                                       sim stands at, since every step feeds its tick: the
                                       clock an input reads, and a kill's tick                 */

    /* scratch: the snap rules' (tasim_snap.h), the placement gesture's (tasim_place.h), the reclaim
       sweep's (tasim_sweep.h), and the selection they read, SNAP_UNITS_MAX slots; the view's arrays */
    struct SNAP_SCRATCH*  snap;
    struct PLACE_SCRATCH* place;
    struct SWEEP_SCRATCH* sweep;
    uint16_t*        snap_builders;
    WIKIVIEW         view;
    WIKIVUNIT*       vunit;
    WIKIVSITE*       vqueued;
    WIKIVLINE*       vline;
    WIKIVSITE        vsite[PLACE_MAX_SITES];
    WIKISNDEV        vsnd[WIKI_SND_LOG];
    WIKIVFEAT        vfeat[WIKI_FEATS_MAX];
    WIKILEDGER*      vledger;
    int              cvledger;
    WIKIVCURSOR      vcursor[WIKI_SEATS];
    WIKICLICK        vclick[WIKI_CLICKS];
    WIKIVTYPE        vtype[WIKI_MAX_DEFS];
    /* the view's seats 1..nin - 1: their parts, and their sites, PLACE_MAX_SITES a seat, allocated
       by init when nin is above 1 */
    WIKIVSEAT        vinput[WIKI_SEATS - 1];
    WIKIVSITE*       vinsite;
    int              cvunit, cvqueued, cvline;
} WIKISIM;

/* A demo played (`scripted`: the demo's inputs are fed at their ticks and it is done at its end)
   or create (Try it yourself on the demo's scene: no script, never done). `d` may be NULL for the
   entry's scene alone.

   `lay` is the sidebar's layout, or NULL for none. LIFETIME: it is WIKIDATA.fixture or the layout
   inside the published WIKISIDE block (that block's own allocation, tagpu_wikiasset.h), both
   immutable and never freed, so the sim may hold it; the render thread resolves its own copy
   of the pointer from the view's WIKISTAGE_LAYOUT_* value. A caller giving any other layout keeps
   it alive and unwritten as long as the sim. The sidebar is on when `lay` is given and, for play, the
   demo's `sidebar` says so, by default (and always for create) when the scene gives the player a
   mobile builder (owner 0, MaxVelocity > 0, WorkerTime > 0).

   THE FEATURES: the entry's scene's, then the demo's, each named by its id or f<k + 1>
   for the k-th, refused when its footprint leaves the scene's map, overlaps another feature or its
   id is taken. A blocking one stands in the way of a unit's walk, a scene unit's spread and a site;
   a scene structure overlapping any feature refuses the scene.

   `ctx` is the context (WIKISIMCTX); NULL is the absent context: side 0, no casting, Interface
   Type 1, the interface the demos are authored in.

   THE BOUND ENTRY, with a casting: every type of the entry names one of the
   ten casts and no two types of one side (a game entry's rival types, or the rest) one cast, or
   the sim is refused. Type i of a cast the casting binds becomes the casting's type under its
   name, with its title, footprint, figures (the economy's among them), canstop, activatewhenbuilt
   and G cells, and a build list holding entry type j of its own side exactly when the casting's
   list names j's cast; type i of a cast it leaves unbound keeps the entry's own definition and is
   marked unbound. A rival type binds to ctx->rival the same way, and keeps its own definition,
   not unbound, when ctx->rival is NULL. In a game, each bound type's wreck is its cast's corpse,
   put over the features table's row of its name or appended to the bound table. Two types bound
   to one name refuse the sim. The indices do not move, so the scene's units and the inputs'
   `build` name the same types as before; captions stay raw (the hash keeps them so).

   THE DATA RULE OF THE RESOLVED SCRIPT: a scripted demo's actions become their
   buttons at `iface` (tagpu_wikidata_button) in the sim's own copy of its inputs, and a press of
   a mouse button already held, or a release of one not held, refuses the demo with its tick
   (tagpu_wikidata_buttons). The player's inputs to Try it are buttons already and are not
   resolved.

   0 with a message in `err` when the layout fails tagpu_wikilayout_check, the context is refused,
   the entry does not bind to the casting, a scene structure leaves the placeable interior or
   overlaps one placed before it (its footprint may be the casting's), the scene selects a unit it
   does not have, the resolved script breaks its data rule, a scripted demo with the sidebar on
   breaks the build trip's data rule (tagpu_wikidata_trips), or memory runs out; then `s` holds
   nothing to free. */
/* `scripted`: TRY is Try it yourself, DEMO a demo's script, GAME a game entry's demo's game script
   (the drop-in's WikiSim.game) with its economy in refuse mode, GAME_STALL the same in stall mode
   (research/notes/ingame-wiki-replay-plan.md, decision 18): refuse, an authored script, is done,
   `starved`, at the first pass that cannot pay a grant; stall, a seat the player plays, carries an
   unpaid grant as the unit's debt, which refuses its requests until a pass pays it. A game entry
   plays only as a game and a game only from a game entry. */
enum { WIKI_PLAY_TRY, WIKI_PLAY_DEMO, WIKI_PLAY_GAME, WIKI_PLAY_GAME_STALL };
int  tagpu_wikisim_init(WIKISIM* s, const WIKIENTRY* e, const WIKIDEMO* d, int scripted, const WIKILAYOUT* lay,
                        const WIKISIMCTX* ctx, char* err, size_t errn);
void tagpu_wikisim_free(WIKISIM* s);

/* One input now, as the player gives it, between two ticks, acting as its seat (ev->seat): a cursor
   sequence it changes begins on the tick the sim stands at, the one the next view shows. An input
   of a seat outside 0..nin - 1 is ignored. */
void tagpu_wikisim_input(WIKISIM* s, const WIKIINPUT* ev);

/* One tick: the script's inputs of the next tick, then the units' work. */
void tagpu_wikisim_step(WIKISIM* s);

/* The view as `seat` sees it (the drop-in's view(seat)): its cursor, ghost, gesture sites, queued
   sites, drag, order lines, sweep, keys, spacing, hover and cursor shape; a seat outside 0..nin - 1
   views as seat 0. The units' `selected` is every seat's flag (the hash's item 3), which a frame
   filters by owner. It and its arrays hold until the next call on `s`. tagpu_wikisim_view is
   tagpu_wikisim_view_as(s, 0). */
const WIKIVIEW* tagpu_wikisim_view_as(WIKISIM* s, int seat);
const WIKIVIEW* tagpu_wikisim_view(WIKISIM* s);

/* The posed model of the view's unit `i` (tagpu_wikicob_model: TAGPU_COBVM_MODEL_BYTES(*nprim)
   bytes, the primitives in the posed order), NULL for a unit no script poses -- none run, its type
   quarantined, or `i` not a unit. Valid until the next call on `s`. */
const unsigned char* tagpu_wikisim_model(const WIKISIM* s, int i, int* nprim);

/* A unit's orders by id into `*o`, valid until the next call on `s`, and their count; -1 when
   there is no such unit. At most PLACE_LIST_CAP: every append stops there. */
int tagpu_wikisim_orders(const WIKISIM* s, const char* id, const WIKIORDER** o);

/* The state hash: 64-bit FNV-1a (offset basis 0xCBF29CE484222325, prime 0x100000001B3) over what
   the view and tagpu_wikisim_orders show (the host's `hash` op answers it, and every check compares
   it). It reads `s` and changes nothing, so it may be taken
   at any point, the view taken or not. The core's items -- 2, 3, 8's COB host, 9, 10, 11's
   features, 12 and 14 -- are fed by the core's feeders on the same running state (tasim_hash.h).
   Fed in this order, an integer as 4 bytes little-endian
   (two's complement), a double as its 8 IEEE-754 bytes little-endian, a string as its byte
   length (an integer) and then its bytes, "" for none:
     1. tick, done (0 or 1), the layout side the sidebar shows (-1 with it off), the Interface
        Type;
     2. the map: w, h, SurfaceMetal (double); every cell's metal (double) and then every cell's
        fog (integer 0..2), row-major; the vent count and each vent's centre x, z (doubles);
     3. the unit count, and for each unit in the view's order: id, type name, owner, x, z,
        hx, hz (its facing), selected (0 or 1), built, reclaimed, label; then the orders
        tagpu_wikisim_orders answers for its id: their count, and each one's kind
        (WIKI_ORDER_*), then for a move x, z (doubles), for a build the type name and ox, oz,
        for a guard the target id, for a reclaim the target id and 1 with the feature's centre
        x, z (doubles) for a feature, or 0 for a unit;
     4. the ghost: 1 and its type name, x, z (doubles) and ok (0 or 1), or 0;
     5. the gesture's sites: the shape (PLACE_*, PLACE_NONE without sites), the count, and each
        site's x, z (doubles) and state (WIKI_SITE_*);
     6. the cursor's x, z (doubles); each key in WIKI_KEY_* order, 1 held or 0; the armed type's
        name ("" in the contextual mode); the spacing (-1 when nothing is armed); the drag: 1 and
        x0, z0 (doubles), or 0; the caption;
     7. the sidebar: 1 when it is on, then its rows, the routed screen's name, the shown unit's id
        ("" for none), its page, the state count and each state, and the id of the unit under
        the cursor over the map ("" for none); or 0 alone. Then the locked type's name ("" for
        none), with the sidebar on or off;
     8. the demo's clock, the WIKI_UNTIL_* a wait holds it for (0 none), stalled (0 or 1); then
        the COB host: 1 and tagpu_wikicob_hash from the offset basis as 8 bytes little-endian, or
        0 alone without one;
     9. the nano spray: the particle count, and each particle's x, y, z, its step's x, y, z, its
        end and its colour, in the view's order; then part_cut (0 or 1) and the waiting second
        bursts' count, and each one's twelve box words (the first box's, then the second's), its
        due tick and its key, in the view's order;
    10. the sound: 1 when the sim has a sound source, then each of the WIKI_SND_KINDS kinds' next
        time, the last reply's time, the queue's count and each entry's kind, tick, unit id, type
        name and priority, then the events made since the sim was made (a count) and the last
        min(that count, WIKI_SND_LOG) of them, oldest first, each one's tick, kind, name, category
        and variant, then each unit's 1 when its first order's taken site has said cant, else 0, in
        the view's order; or 0 alone without a source;
    11. the features standing: their count and each one's id, its table row's name and its anchor
        cell ox, oz; the gone count and each cell's ox, oz; then 1 when reclaim is armed, its kind
        (WIKI_KIND_*) and the ring: 1 and its x, z and r, or 0; or 0 alone.
    12. a game simulation's only (nothing at all in any other): the seat count, and per seat its
        units, lost, out and cmd_lost, then its m, mmax, mi, mo, wm, e, emax, ei, eo and we; then
        the economy's mode (0 refuse, 1 stall), starved, bad_kill and the kills made; then each
        unit's ledger in the view's order, its eP, eR, eG, eD, mP, mR, mG and mD. Each seat's
        record ends with its kills and losses, after we.
    13. a game simulation with more than one input seat only (nothing otherwise): the input seats'
        count and the viewed seat; then for each seat k = 1..nin - 1 its ghost (item 4's form), its
        gesture's sites (item 5's), its cursor, keys, armed type, spacing and drag (item 6's, without
        the caption), and its reclaim armed, kind and ring (item 11's tail).
    14. a game simulation's only (nothing at all in any other): the deaths' count and each one's
        tick, owner, x, z (doubles) and type name, in the order made. The clicks and each seat's
        `moved` are not hashed: they decide nothing, and simparity compares the views.
   Items 1-12 read seat 0's input, item 3 every seat's selection. A game whose script and parts
   name no seat but 0 has one input seat and views seat 0, so item 13 adds nothing to its hash.
   The view's other fields follow from these: queued sites and order lines from the units and
   their orders, the cursor's shape from the mode and the selection, the sweep's targets, readout,
   hover and popup from the features, the ring and the cursor, a site's order number from
   the states, footprints from the types, the pace from the entry. The tick the cursor's shape
   began is history rather than state, and is left out. */
uint64_t tagpu_wikisim_hash(const WIKISIM* s);

/* THE LIT GRID of `seat` in a game's view `v` at the shown tick `g`, over the layout's sight table
   `t` (NULL or numtables 0: none), into `out`, (v->w >> 1) x (v->h >> 1) bytes row-major; their
   count, or 0 when the grid would pass WIKI_SIGHT_GRID. The rule, the engine's stamp reduced to
   the stage, is tasim_sight.c's comment (tasim_sight_lit); WIKI_SIGHT_GRID is tasim_state.h's. Without a
   table every block is 1 (Sight lights every block). Pure: it reads `v` and `t` only. */
int tagpu_wikisim_sight_lit(const WIKIVIEW* v, const WIKISIGHT* t, int seat, int g, uint8_t* out);

/* THE ARMY VALUE of `seat` in a game simulation (tasim_econ.c states the game's rule): the metal cost of each of its live, finished units of a mobile type that is not its
   commander, summed in f32 in the units' order; 0 for a seat it does not have. Pure. */
float tagpu_wikisim_army(const WIKISIM* s, int seat);

/* THE CONTACTS of `seat` in a game's view `v`, over its lit grid `lit` (tagpu_wikisim_sight_lit's
   for the same view): per view unit, index-aligned with v->unit, 1 when the radar pass would mark
   it, else 0, into `out` (v->nunit bytes). The rule is tasim_sight.c's comment. Pure. */
void tagpu_wikisim_contacts(const WIKIVIEW* v, const uint8_t* lit, int seat, uint8_t* out);

/* THE FORK (research/notes/ingame-wiki-replay-plan.md, decision 8): a game
   simulation's timeline handed over at R to `seat`, the world of R - 1 kept whole, and the `n`
   inputs at `in` (a fork's part, game inputs of any seat as the loader reads them, a kill's id in
   the entry's document) its script with the other seats' recorded inputs, its economy in
   `econ_mode` (WIKI_ECON_*) from here on, viewed as `seat`. 1, or 0 with a message in `err` and the
   sim untouched: refused unless it is a game standing at R - 1 that has not stopped (a lock, a pass
   that starved, a kill that found no unit), the mode is one of the two, the seat is one of its nin
   input seats, every input is at R or later, neither a caption nor a wait and of an input seat,
   the features standing and gone plus the part's kills stay within WIKI_FEATS_MAX, and the new
   script keeps the held-button rule seat by seat (tagpu_wikidata_buttons: the taken seat's hand
   from nothing, every other seat's from the buttons it holds at R - 1, which its carried inputs
   continue) and, with the sidebar's trips, their data rule (tagpu_wikidata_trips); or memory runs
   out.

   THE ORDERING: called between two ticks with the simulation at R - 1; feed(t) feeds exactly the
   inputs of tick <= t, so every input not yet fed has its tick >= R (one of a tick no feed reaches,
   below R, is dropped with it). Of those the fork drops the taken seat's, the recorded future it
   replaces, and every kill, since a death after R is not fated (ingame-wiki-replay-plan.md decision 8); every other seat's
   are carried, so they play on from their recorded inputs. The new script is the carried inputs
   and the part's, merged by tick, the carried first on a tie, each in its own order. The release
   (tagpu_wikisim_release) lets go of the taken seat alone, as the game's takeover leaves its player
   (tk_live: every unit deselected), and with seat 0 it drops the trips' inputs not yet fed; it
   comes before the new script is installed. With seat 0 and a script of seat 0 alone nothing is
   carried, and the fork is the part alone.
   THE LIFETIME: the sim copies the carried inputs and the part into a block of its own and frees
   the old script's; `in` is only read. */
int tagpu_wikisim_fork(WIKISIM* s, int R, int seat, const WIKIINPUT* in, int n, int econ_mode, char* err, size_t errn);

/* THE RELEASE, between two ticks: what the game's rebuild leaves of `seat`'s
   input after Back to now and in a fork (tagpu_wikisim.c says how that was measured): no key or
   button held, no drag, no placement or reclaim gesture, all let go without acting; nothing of the
   seat's selected and nothing armed; the spacing and the sweep's kind kept; every other seat's input
   untouched. Seat 0's release also lets go of the column's gadget capture, routes the column to
   MAIN2 and drops every generated (trip) input not yet fed, so a release costs the game at most the
   one order in hand, a trip it cuts off or a building or reclaim armed and not yet used (THE LIVE
   DATA RULE, tagpu_wikidata.c live_orders, keeps a live entry's orders so); the column and its
   trips are seat 0's. A seat outside 0..nin - 1 has nothing to let go. The fork calls it, and the
   replay layer at Back to now. */
void tagpu_wikisim_release(WIKISIM* s, int seat);

#endif
