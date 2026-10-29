#ifndef TAGPU_WIKISIM_H
#define TAGPU_WIKISIM_H
/* tagpu_wikisim -- the in-game wiki's simplified simulation: a scene and its inputs in, a view of
   the game out. A port of the mockup's tools/wiki/mockup/sim.js, function for function and under
   its names; research/notes/ingame-wiki.md, "The simulation's contract", is the specification of
   both. A demo is its scene and its inputs played through it, and Try it yourself is the same
   simulation taking the player's inputs instead.

   IT AGREES WITH sim.js BIT FOR BIT. Both visit units in creation order, use no clock and no
   randomness, and compute on doubles with + - * / and sqrt only, each rounded once, plus trunc,
   floor and comparisons, which are exact, so the same inputs give the same doubles.
   tools/wiki/simparity.mjs plays every demo of every entry, with the fixture's sidebar, the tall
   fixture's and none, with each casting of tools/wiki/casting-fixture.json and none, at both
   Interface Types, and simcheck.mjs's rule cases through both, and compares every field of the
   view and every unit's orders, tick by tick. The DLL compiles this file for SSE2 (Makefile, THE WIKI'S SIMULATION) so
   that no intermediate keeps x87's extra precision.

   ENGINE-FREE, WIN32-FREE AND WITHOUT GLOBALS. It reads no engine memory and names no engine
   address: where sim.js mirrors the rule code of tagpu_resource.c or tagpu_place.c, this file
   ports sim.js's version, and says which function each piece mirrors -- except what the game
   keeps engine-free in tagpu_placegeo.c, the shapes and the take-off rule, which it calls
   directly as the game does. Every piece of its state lives in the WIKISIM its caller owns
   (and on the heap that WIKISIM owns), so it runs on whichever thread owns the wiki window;
   one WIKISIM is used by one thread at a time. The WIKIENTRY (and the WIKIDEMO) it was made
   from must outlive it: it points into them. So must its sidebar layout and its context's
   casting, which it reads and never writes.

   A SIM IS MADE FOR A CONTEXT, WIKISIMCTX (sim.js's ctx): the layout's side the player plays,
   that side's casting (the mod's own type for each of the entry's casts, or none for the entry's
   own types) and the Interface Type. With a casting it plays the BOUND ENTRY, its own copy of
   the entry whose types are the casting's (tagpu_wikisim_init says how), and every type name it
   shows -- the view's, the hash's, WIKISIM.entry's -- is the bound one.

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

#define WIKI_CELL 16
#define WIKI_TPS  30

/* THE BUILD TRIP, sim.js's TRIP, TRIP_DOWN, TRIP_UP and TRIP_ROWS (tools/wiki/simparity.mjs
   compares them). A scripted demo with the sidebar plays a `build` input at tick t as the cursor's
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

/* THE PICK TRIP, sim.js's PICK, PICK_DOWN and PICK_UP: a scripted `pick` of a kind at tick t is
   the glide to the popup's row of that kind, its press at t + WIKI_PICK_DOWN and release at
   t + WIKI_PICK_UP, and the glide back, WIKI_PICK ticks whatever the layout; the row is where the
   popup stands when the pick is fed. A `reclaim` is the build trip's to RECLAIM (WIKI_TRIP). */
#define WIKI_PICK       60
#define WIKI_PICK_DOWN  20
#define WIKI_PICK_UP    26

/* the lock of a RECLAIM the layout cannot reach: WIKIVIEW.locked and the hash name it "RECLAIM" */
#define WIKI_LOCK_RECLAIM (-2)

/* THE NANO SPRAY (research/notes/ingame-wiki-cob-plan.md, L4a): the engine's nano particles
   (0x4720D0 forward for a build, 0x472200 reverse for a reclaim; their class 0x4FD5B8) as the
   wiki's simulation state. A builder whose work is credited this tick asks its script's
   QueryNanoPiece (0x43E400, run-now) and emits from that piece's origin: every tick for a build
   (MobileBuild 0x403E43 -> 0x403E56 -> 0x403ECC, HelpBuild 0x404139 -> 0x4041C2), every second
   tick of a reclaim from its first (ReclaimUnit 0x4049AD -> 0x404A4E, re-armed by 0x439E80(order,
   2)). An emission is two bursts of five (0x473D50): one on its tick, one on the next (the
   walker 0x471EB0 spawns it again once the object's life of 1 has come, 0x472F60), the second
   only when the first made a particle. A particle runs straight from a point of one box to a
   point of the other in their distance's integer length >> 18 steps, so about 4 world units a
   tick (0x473EB6..0x473F04), each box shrunk
   to its middle 4/11 .. 7/11 on every axis (0x473B80..0x473D34); it moves and then dies once the
   tick is past its end (0x4739B0, 0x473B30), its colour cycling 0xA1..0xA7.

   ITS RANDOMNESS IS THE WIKI'S OWN: a stateless hash of the builder's id, the emission's place
   among the tick's, the tick and the draw, never the engine's CRT rand 0x4E4870 (decision 11) and
   never the host's RAND generator, whose state the scripts' hash carries.

   BOUNDED: at most WIKI_SPRAY_PARTS particles live and WIKI_SPRAY_BURSTS second bursts wait; a
   particle or a burst past its bound is not made, and the view says so (part_cut). sim.js holds
   the same bounds, so the two drop the same particles. */
#define WIKI_SPRAY_PARTS  1024
#define WIKI_SPRAY_BURSTS 256

/* one particle: its point and its step a tick in 16.16 world units (x, altitude, z), the last tick
   it is drawn, and its palette index */
typedef struct {
    int32_t x, y, z;
    int32_t sx, sy, sz;
    int32_t end;
    int32_t col;
} WIKIVPART;

/* an emission's second burst, waiting for its tick: its two boxes as 0x473B50 keeps them,
   {base x, y, z, extent x, y, z}, the first the one its particles start in */
typedef struct {
    int32_t  box[2][6];
    int      due;
    uint32_t key;
} WIKISPRAY;

/* THE SOUND (research/notes/ingame-wiki-cob-plan.md, decisions 12 and 13, L5): what the engine would
   say for the wiki's units and buttons, as the wiki's simulation state. The engine's own entries are
   never called (they spend the game's cooldowns and its notice queue); their rules are repeated
   here, DISASSEMBLED from the retail executable:

   A UNIT'S REPLY is a notice, 0x47F780(unit, kind, text): only for a unit of the viewed player
   (unit+0xFF == main+0x2A43; the wiki's owner 0), alive and not dying. Its push 0x47FAD0 refuses
   it while GameTime < the kind's next time (0x47FAF0, `jb`) and while a notice of its kind is
   queued (0x47FB0A); a full queue of WIKI_SND_QUEUE drops its last entry, shown as text only
   (0x47FB27 calls 0x47FD70(7, 0, 1)); the notice goes in before the first entry of a lower
   priority (0x47FBA4..0x47FBC0), {kind, GameTime, unit, priority}. The consumer 0x47F680, once
   a main-loop pass (0x4999A6) -- here once a tick, after the tick's work -- takes entry 0: with
   sound only when GameTime >= the last reply's time + WIKI_SND_GAP ([q+0xA1], set to 30 at the
   queue's creation 0x47EE07), and then the last time is GameTime whatever follows (0x47F6D7). The
   player 0x47FD70 picks a variant of the unit's sounds.tdf category (def+0x20E) for the kind,
   rand() * count / 0x8000 (0x47FDC6..0x47FDD6), and under four gates -- priority > 10 - unitchat,
   count > 0, sound asked for, speechfx (main+0x37F19 bit 0x40) -- writes the kind's next time,
   GameTime + 30 x its interval (0x47FEFD), even when fxvol, Sound Mode or NoDirectSound then
   silence the play (0x47FEB0..0x47FEC2 jump to the write). A level's start empties the queue and
   zeroes the last time and every next time (0x47EE30, from 0x491898), so a level's own units say
   nothing in its first second. A wiki sim is a scene of a game under way: its scene's units say
   nothing as they are laid out, and it starts with every next time 0 and the last reply
   WIKI_SND_GAP before tick 0, so the first reply the demo or the player causes is heard.

   WHAT THE WIKI KEEPS of those gates: count and the sound asked for are simulation state; unitchat
   and speechfx are the player's settings and are applied where the sound is played
   (tagpu_wiki.c), with the simulation run at their defaults (unitchat 10, so every kind's priority
   passes; speechfx on: the options' defaults 0x45C59C, 0x45C5D2). So a player who lowered
   unitchat hears the kinds it lets through, but the next time of a kind it silences is written
   here where the engine leaves it unwritten. GameTime is the tick being stepped, or between two
   ticks the tick the sim stands at. The variant is the wiki's own generator (snd_draw), never
   the CRT rand.

   THE TRIGGERS, each the engine's: select (kind 1) when a click leaves its unit selected
   (0x48C8B9, 0x48C997) or a box selects exactly one (0x48C668), and the allsound name
   SelectMultipleUnits when a box selects more (0x48C681); ok (5) when a unit's order is first run
   (0x438880 at each order handler's start, gated on the order node's +0x42 bit 0x2000, which it
   clears; that every order a click gives carries the bit is [INFERRED]: where it is set was not
   found), never for the scene's own orders; arrived (6) when a move ends (Move_Ground 0x4031FB);
   cant (7) when a builder in range finds its site refused, the wiki's analogue of MobileBuild's
   first failed site test (0x403CDF; the engine then retries ten times 30 ticks apart, the wiki
   drops the order at once); build (9) when a builder places a nanoframe (0x403DA0); unitcomplete
   (8) for each builder whose build finished this tick (0x403F39, 0x40422F); working (11) when a
   reclaim first makes progress on an order (ReclaimUnit after its stance wait, 0x4048BE);
   activate (3) and deactivate (4) when ACTIVATION changes for a unit whose script runs (0x48B110,
   0x48B135 in the state setter 0x48B090). A NAMED SOUND (0x47F1A0, an allsound.tdf name, played
   at once, no queue and no cooldown) for the sidebar's buttons as 0x41AA00 plays them -- ORDERS
   `ordersbutton` (0x41AB04), BUILD `buildbutton` (0x41AB45), a build button that arms `addbuild`
   (0x41ABA3), PREV and NEXT `nextbuildmenu` on every press (0x41C180 calls 0x41BDE0 or 0x41BF10,
   which play it at 0x41BF01 and 0x41C04C, with a page to step or not), STOP
   `immediateorders` (0x419CF2 -> 0x41A0EA) -- and for a placement, `oktobuild` and `notoktobuild`
   (0x498FA4, 0x49901D in stock's click, `oktobuild` once a gesture in tagpu_place.c's commit).

   NOT POSITIONAL: the reply 0x47FD70 plays by file through 0x4D0640(name, -585, 0) and the named
   sound through 0x4CF570(buffer, -585, 0), neither with a point, so no event carries one: the
   engine attenuates neither by its camera, and the stage's camera attenuates nothing either.

   BOUNDED: the queue holds WIKI_SND_QUEUE entries; the events are a ring of WIKI_SND_LOG, which
   the player drains after every frame's steps (tagpu_wiki.c) and the hash reads whole. */
#define WIKI_SND_KINDS  24          /* the kind table 0x5086D8's records, 24 bytes each           */
#define WIKI_SND_QUEUE  8           /* 0x51E68C's entries, 0x11 bytes each, count at +0x99        */
#define WIKI_SND_GAP    30          /* [q+0xA1]: one reply with sound a second                    */
#define WIKI_SND_LOG    32
/* the kind table 0x5086D8's priority (+4) and interval in seconds (+8) by kind, dumped from the
   retail executable's .data; sim.js SND_PRIORITY and SND_INTERVAL are the same */
#define WIKI_SND_PRIORITIES { 0, 10, 9, 4, 4, 5, 3, 8, 3, 4, 3, 2, 7, 7, 7, 7, 4, 10, 10, 10, 10, 10, 10, 10 }
#define WIKI_SND_INTERVALS  { 0, 0, 20, 2, 2, 1, 4, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 }

enum { WIKI_SND_SELECT = 1, WIKI_SND_ACTIVATE = 3, WIKI_SND_DEACTIVATE = 4, WIKI_SND_OK = 5,
       WIKI_SND_ARRIVED = 6, WIKI_SND_CANT = 7, WIKI_SND_COMPLETE = 8, WIKI_SND_BUILD = 9,
       WIKI_SND_WORKING = 11 };

/* the allsound.tdf names the wiki plays, by index, and their spelling: the strings the engine's
   callers push to 0x47F1A0 (0x508D8C, 0x509660, 0x509650, 0x5026B8, 0x5027EC, 0x5027F8, 0x502850,
   0x50271C), and "Options" as the in-game menu's controls and tagpu_menu.c's play it. allsound.tdf's
   names match without case (0x4F8A70). sim.js SND_NAMES and tools/wiki/gfx.py SND_NAMES spell the
   same list. */
enum { WIKI_SNDNAME_MULTI, WIKI_SNDNAME_OKTOBUILD, WIKI_SNDNAME_NOTOKTOBUILD, WIKI_SNDNAME_ADDBUILD,
       WIKI_SNDNAME_BUILDBUTTON, WIKI_SNDNAME_ORDERSBUTTON, WIKI_SNDNAME_NEXTBUILDMENU,
       WIKI_SNDNAME_IMMEDIATE, WIKI_SNDNAME_OPTIONS, WIKI_SNDNAME_SPECIAL, WIKI_NSNDNAME };
#define WIKI_SNDNAME_STRINGS { "SelectMultipleUnits", "oktobuild", "notoktobuild", "addbuild", "buildbutton", \
                               "ordersbutton", "nextbuildmenu", "immediateorders", "Options", "specialorders" }

/* A unit type's sounds as the simulation needs them: its sounds.tdf category (-1 none) and, per
   kind, the variant count the engine reads at rec + 0x40 + 12 x kind. */
typedef struct {
    int     cat;
    int32_t count[WIKI_SND_KINDS];
} WIKISNDTYPE;

/* Where a sim's sounds come from: for the type named `name` its WIKISNDTYPE into `out`, 1; 0 when
   the source has none, which leaves the type silent. NULL in WIKISIMCTX: no sound at all. */
typedef struct WIKISNDSRC {
    void* ctx;
    int (*type)(void* ctx, const char* name, WIKISNDTYPE* out);
} WIKISNDSRC;

/* a queued reply, 0x47FAD0's entry: the unit by id and type, which the play reads its category by */
typedef struct {
    int  kind, tick, type, priority;
    char unit[WIKI_ID_MAX];
} WIKISNDQ;

/* a sound the simulation made: a reply (`kind` 1..23, `name` -1, its category and variant) or a
   named sound (`kind` 0, `name` a WIKI_SNDNAME_*, `cat` -1, `variant` 0), on `tick` */
typedef struct {
    int tick, kind, name, cat, variant;
} WIKISNDEV;

enum { WIKI_ORDER_MOVE, WIKI_ORDER_BUILD, WIKI_ORDER_RECLAIM, WIKI_ORDER_GUARD };

typedef struct {
    int    k;                       /* WIKI_ORDER_* */
    int    serial;                  /* the sim's count of orders made when this one was, from 1:
                                       sim.js's order object, whose identity a COB work rests on */
    int    type;                    /* BUILD: the building's type */
    int    ox, oz;                  /* BUILD: its footprint's first cell */
    double x, z;                    /* MOVE: where to; a feature's RECLAIM: its centre */
    int    feat;                    /* RECLAIM: the target is a feature, `target` its id */
    char   target[WIKI_ID_MAX];     /* RECLAIM, GUARD: the unit's id, or the feature's */
} WIKIORDER;

/* A FEATURE STANDING on the map, sim.js's feats: its id, its row of the entry's table and that
   row's figures, its anchor cell and its centre */
typedef struct {
    char   id[WIKI_ID_MAX];
    int    def;
    int    ox, oz, fx, fz;
    double x, z, metal, energy;
    int    height, blocking, reclaimable;
} WIKIFEAT;

/* a rect in world units; the movement's bodies (tagpu_wikisim.c, MOVEMENT) */
typedef struct { double x0, z0, x1, z1; } WIKIRECT;

typedef struct WIKIUNIT {
    char             id[WIKI_ID_MAX];
    int              type, owner, sel;
    double           built, reclaimed;  /* fractions, 0..1 */
    int              bp, page;          /* with the sidebar, unit+0x110 bit 22 (a numbered page
                                           shown rather than the orders page) and bits 23..25
                                           (the page, 1..n; 0 for a type without pages) */
    const char*      label;
    double           x, z;              /* its centre */
    double           hx, hz;            /* its facing, a unit vector (walk says why not an angle);
                                           a structure's is (0, 1) */
    int              ox, oz;            /* a structure's first footprint cell; unused for a mobile unit */
    int              norder, corder;
    WIKIORDER*       order;
    /* one tick's work (advance): the structure it worked on and how, and on a structure the work
       credited to it */
    struct WIKIUNIT* job;
    int              job_how, worked;
    double           work_build, work_reclaim;
    /* the unit's script (tagpu_wikicob.h): its handle in the host, 0 for none; the structure it
       works on, by id, which its StartBuilding went for ("" for none), the serial of the order
       that work came from, and whether its stance has been seen on that work; this tick's job by
       id (a reclaim frees its target mid-tick); whether it stepped this tick, and whether it was
       moving after the last */
    int              cob;
    char             work[WIKI_ID_MAX];
    int              work_serial, stanced;
    char             job_id[WIKI_ID_MAX];
    int              stepped, moving;
    /* movement (tagpu_wikisim.c, MOVEMENT): whether it stepped on the last tick; whether this
       tick's step turned off its line, and whether it stepped off a structure or a claim instead
       of its orders; its leg -- the node it walks to, and the serial of the order and the kind of
       target it was planned for -- when `leg` */
    int              went, turned, evacuated;
    int              leg, leg_serial, leg_kind;
    double           leg_x, leg_z;
    /* the spray: how its work was credited this tick (HOW_* + 1, 0 none) and the structure's
       id, type and centre then (a reclaim may free it before the unit's turn sprays); and the
       tick of its last reclaim spray (0 none) and the structure that one was on */
    int              spray, spray_type, spray_at;
    double           spray_x, spray_z;
    char             spray_id[WIKI_ID_MAX], spray_on[WIKI_ID_MAX];
    int              spray_box[5];      /* a feature's spray: its ox, oz, fx, fz and height */
    /* a feature's reclaim (0x404AD0): the serial of the order it is for (0 none), its countdown,
       the tick of its next working call and whether it paid out; and whether `work` names a
       feature */
    int              rc_serial, rc_c, rc_next, rc_due, rc_paid, work_feat;
    /* the sound's triggers: the serial of the order whose first run said ok (0 none), of the order
       whose reclaim said working, of the order whose taken site said cant, whether the job
       credited this tick was a build that finished, and ACTIVATION as the last notice saw it */
    int              begun, said, cant, completed, active;
} WIKIUNIT;

/* a body another unit's step must not newly overlap: a unit's, or a claim's (u NULL) */
typedef struct { WIKIRECT r; const WIKIUNIT* u; } WIKIBODY;

/* the planner's heap entry: its f, the order it was pushed in, its node */
typedef struct { double f; int seq, n; } WIKIHEAPNODE;

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
/* With reclaim armed it is cursorreclamate over the map, RECLAIM; sim.js's reclaimCursor says where
   it is the arrow instead. */
enum { WIKI_CURSOR_ARROW, WIKI_CURSOR_MOVE, WIKI_CURSOR_BUILD, WIKI_CURSOR_RECLAIM, WIKI_CURSOR_N };

/* ---- the view: what the stage draws, sim.js's view() field for field ---- */

/* a feature standing, sim.js's view.feats: its id (the sim's), its row of the entry's table, its
   anchor cell, footprint and amounts */
typedef struct {
    const char* id;
    int         def, ox, oz, fx, fz;
    double      metal, energy;
} WIKIVFEAT;

/* THE SWEEP as tagpu_sweep_fill publishes it, sim.js's view.reclaim: while reclaim is armed, the
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

typedef struct {
    const char* id;
    int         type, owner, fx, fz, selected;
    double      x, z, hx, hz, built, reclaimed;
    const char* label;
    int         cant;   /* its first order's taken site has said cant (sim.js's sound.cant) */
} WIKIVUNIT;

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

/* The sidebar as the stage draws it, sim.js's view.side; by value and pointer-free, so a copy
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
} WIKIVIEW;

/* The context a sim is made for, sim.js's ctx. tagpu_wikisim_init refuses an iface other than 0
   or 1 and, with a layout, a side that is not one of its sides. LIFETIME: `cast` must outlive the
   sim, which holds it (its bound types' titles point into it); in the DLL it lies inside the
   published WIKISIDE block, immutable and never freed. */
/* Where a sim's unit scripts come from (sim.js's ctx.cob): for the type named `name`, its script
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
} WIKISIMCTX;

struct WIKISIMBOUND;

/* ---- the simulation: the caller owns it; tick and done are its to read, the rest is private ---- */

typedef struct {
    int              tick, done;
    int              side;          /* the layout side the sidebar shows, -1 with it off */
    int              iface;         /* the Interface Type, 0 or 1 */

    /* the entry it plays: the one it was made from, or with a casting the bound entry, which
       `bound` holds on the heap (a heap block rather than a field, so that a WIKISIM moved by
       value points at nothing of its old place); its type names are the casting's */
    const WIKIENTRY* entry;
    struct WIKISIMBOUND* bound;
    unsigned char    unbound[WIKI_MAX_DEFS];    /* type i is of a cast the casting leaves
                                                   unbound: a scene unit or a build of it locks */
    int              W, H;          /* the map: the scene's W x SH, grown to H rows beside the column */
    int              SH;
    double           SM, PACE;
    double*          metal;
    unsigned char*   fog;
    int              nvent;
    PlaceCell*       vent;
    WIKIPOINT*       vent_centre;
    WIKIUNIT**       unit;          /* in creation order; a reclaimed one leaves the list */
    int              nunit, cunit, next_id;

    /* sim.js's st */
    double           cursor_x, cursor_z;
    int              cursor_shape, cursor_since;    /* the shape shown, and the tick its
                                                       sequence began                           */
    int              held[WIKI_NKEYS];
    int              mode;
    int              has_drag, drag_active, drag_t;     /* drag_t: the press's tick */
    double           drag_x0, drag_z0;
    const char*      caption;
    WIKIHOVER        hover;
    int              has_gesture;
    PlaceCell        gesture;
    int              shift_placed;
    int              spacing[WIKI_MAX_DEFS];
    WIKIPLACE        place;
    /* the reclaim sweep: armed (main+0x2CC3 == 12, exclusive with a building armed), its kind, and
       its gesture -- the press point, the last point over the map, past the threshold or not */
    int              rmode, kind, has_rg, rg_drag;
    double           rg_x0, rg_z0, rg_x1, rg_z1;

    /* the features standing, sim.js's feats, in the scene's order; the anchor cells of those
       reclaimed, gone[2k], gone[2k + 1]; both at most WIKI_FEATS_MAX */
    WIKIFEAT*        feat;
    int              nfeat, ngone;
    int*             gone;

    /* the script: the demo's inputs resolved at `iface` (each action its button) into `resolved`,
       the sim's own copy, then ordered by tick, file order breaking ties */
    int              nscript, next;
    WIKIINPUT*       resolved;
    const WIKIINPUT** script;
    double           end;           /* the clock it is done on; never for Try it yourself */
    /* THE DEMO'S CLOCK (sim.js's clock): the tick its inputs, trips and end are written in. It runs
       with `tick` except while a wait holds it: waiting the WIKI_UNTIL_* it waits for (0 none),
       from the tick it began, at most wait_max ticks; one past its max stops the demo, stalled */
    int              clock, waiting, wait_from, wait_max, stalled;
    int              nbuilt, built_waits, wait_built;  /* the buildings of the player's finished so
                                                         far, the BUILT waits begun, and the count
                                                         this one needs: its place among them */
    int              next_serial;   /* the orders made, the last serial given */

    /* the units' scripts: the host, or NULL without one; GET's facts, scratch for every call */
    WIKICOB*         cob;
    int              ncob;          /* the handles given: the last unit's */
    WIKICOBFACT*     fact;
    int              cfact;
    /* the nano spray, with a host only: WIKI_SPRAY_PARTS particles and WIKI_SPRAY_BURSTS second
       bursts on the heap, the emissions of the tick being stepped, and whether a bound dropped one */
    WIKIVPART*       part;
    int              npart;
    WIKISPRAY*       burst;
    int              nburst, nemit, part_cut;

    /* the sound (THE SOUND, above), with a source only: each type's category and counts, the
       time `now` (the tick being stepped, or between ticks the tick the sim stands at), the
       cooldowns, the queue, the last reply's time, the events as a ring at snd_total %
       WIKI_SND_LOG, the serial of the last order the scene gave, which says no ok, and while the
       scene is laid out (snd_quiet, 0 after init) no notice at all */
    int              snd_on, snd_quiet;
    int              snd_cat[WIKI_MAX_DEFS];
    int32_t          snd_count[WIKI_MAX_DEFS][WIKI_SND_KINDS];
    int              now;
    int              snd_next[WIKI_SND_KINDS];
    WIKISNDQ         sndq[WIKI_SND_QUEUE];
    int              nsndq, snd_last;
    WIKISNDEV        snd[WIKI_SND_LOG];
    int              snd_total;
    int              scene_serial;

    /* the sidebar, sim.js's sb; `lay` NULL: off for the sim's whole life */
    const WIKILAYOUT* lay;
    int              trips;         /* a scripted demo with the sidebar: a build is its trip */
    int              ltype[WIKI_MAX_DEFS];  /* each entry type's index in lay->type, or -1 */
    int              screen;        /* the routed screen, an index < lay->nscreen */
    WIKIUNIT*        shown;         /* main+0x37E9C; remove_unit clears it with its unit */
    int              cap, cap_key;  /* the gadget of `screen` a press captured (-1 none), and the
                                       button that pressed it */
    int              down_on[WIKI_NKEYS];   /* where each mouse button went down (OVER_*) */
    int              locked;        /* the type the demo stopped on (the first lock wins), or -1 */
    /* the trip's generated inputs, fed before the script's of the same tick */
    WIKIINPUT        trip_in[WIKI_TRIP_EVENTS];
    int              ntrip_in, trip_next;
    int              feeding;       /* the tick being fed, and between two ticks the one the
                                       sim stands at, since every step feeds its tick: the
                                       clock an input reads                                    */

    /* scratch: distinct's flood; take_offs' one builder's orders and the site taking each off,
       sized for any list since push_order holds every list to PLACE_LIST_CAP; the view's arrays */
    uint32_t*        mark;
    uint32_t         gen;
    int*             todo;
    PlaceOrder       q[PLACE_LIST_CAP];
    uint16_t         by[PLACE_LIST_CAP];
    WIKIVIEW         view;
    WIKIVUNIT*       vunit;
    WIKIVSITE*       vqueued;
    WIKIVLINE*       vline;
    WIKIVSITE        vsite[PLACE_MAX_SITES];
    WIKISNDEV        vsnd[WIKI_SND_LOG];
    WIKIVFEAT        vfeat[WIKI_FEATS_MAX];
    int              cvunit, cvqueued, cvline;
    /* the movement's scratch: one unit's bodies (two a unit: its own and a claim, and the site it
       heads for) and the walking
       ones among them; the planner's held cells, then per node its state, cost and parent, W x H
       of each; its heap, 8 x W x H + 4 */
    WIKIBODY*        obs;
    WIKIRECT*        mover;
    int              cobs, cmover;
    unsigned char*   occ;
    unsigned char*   known;
    unsigned char*   shut;
    double*          pg;
    int*             par;
    WIKIHEAPNODE*    heap;
    int              nheap, heap_seq;
    int              oom;           /* an allocation failed: the sim went on without what it could not add */
} WIKISIM;

/* sim.js's play (`scripted`: the demo's inputs are fed at their ticks and it is done at its end)
   or create (Try it yourself on the demo's scene: no script, never done). `d` may be NULL for the
   entry's scene alone.

   `lay` is the sidebar's layout, or NULL for none. LIFETIME: it is WIKIDATA.fixture or the layout
   inside the published WIKISIDE block (that block's own allocation, tagpu_wikiasset.h), both
   immutable and never freed, so the sim may hold it; the render thread resolves its own copy
   of the pointer from the view's WIKISTAGE_LAYOUT_* value. A caller giving any other layout keeps
   it alive and unwritten as long as the sim. The sidebar is on when `lay` is given and, for play, the
   demo's `sidebar` says so, by default (and always for create) when the scene gives the player a
   mobile builder (owner 0, MaxVelocity > 0, WorkerTime > 0).

   THE FEATURES (sim.js's): the entry's scene's, then the demo's, each named by its id or f<k + 1>
   for the k-th, refused when its footprint leaves the scene's map, overlaps another feature or its
   id is taken. A blocking one stands in the way of a unit's walk, a scene unit's spread and a site;
   a scene structure overlapping any feature refuses the scene.

   `ctx` is the context (WIKISIMCTX); NULL is sim.js's absent ctx: side 0, no casting, Interface
   Type 1, the interface the demos are authored in.

   THE BOUND ENTRY, with a casting (sim.js's binder): every type of the entry names one of the
   nine casts and no cast twice, or the sim is refused. Type i of a cast the casting binds becomes
   the casting's type under its name, with its title, footprint, figures, canstop, activatewhenbuilt
   and G cells, and
   a build list holding entry type j exactly when the casting's list names j's cast; type i of a
   cast it leaves unbound keeps the entry's own definition and is marked unbound. Two types bound
   to one name refuse the sim. The indices do not move, so the scene's units and the inputs'
   `build` name the same types as before; captions stay raw (the hash keeps them so).

   THE DATA RULE OF THE RESOLVED SCRIPT (sim.js's resolve): a scripted demo's actions become their
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
int  tagpu_wikisim_init(WIKISIM* s, const WIKIENTRY* e, const WIKIDEMO* d, int scripted, const WIKILAYOUT* lay,
                        const WIKISIMCTX* ctx, char* err, size_t errn);
void tagpu_wikisim_free(WIKISIM* s);

/* One input now, as the player gives it, between two ticks: a cursor sequence it changes begins
   on the tick the sim stands at, the one the next view shows. */
void tagpu_wikisim_input(WIKISIM* s, const WIKIINPUT* ev);

/* One tick: the script's inputs of the next tick, then the units' work. */
void tagpu_wikisim_step(WIKISIM* s);

/* The view as it stands; it and its arrays hold until the next call on `s`. */
const WIKIVIEW* tagpu_wikisim_view(WIKISIM* s);

/* The posed model of the view's unit `i` (tagpu_wikicob_model: TAGPU_COBVM_MODEL_BYTES(*nprim)
   bytes, the primitives in the posed order), NULL for a unit no script poses -- none run, its type
   quarantined, or `i` not a unit. Valid until the next call on `s`. */
const unsigned char* tagpu_wikisim_model(const WIKISIM* s, int i, int* nprim);

/* A unit's orders by id into `*o`, valid until the next call on `s`, and their count; -1 when
   there is no such unit. At most PLACE_LIST_CAP: every append stops there. */
int tagpu_wikisim_orders(const WIKISIM* s, const char* id, const WIKIORDER** o);

/* The state hash: 64-bit FNV-1a (offset basis 0xCBF29CE484222325, prime 0x100000001B3) over what
   the view and tagpu_wikisim_orders show, so tools/wiki/simparity.mjs computes the same number
   from sim.js's view() and orders() alone. It reads `s` and changes nothing, so it may be taken
   at any point, the view taken or not. Fed in this order, an integer as 4 bytes little-endian
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
   The view's other fields follow from these: queued sites and order lines from the units and
   their orders, the cursor's shape from the mode and the selection, the sweep's targets, readout,
   hover and popup from the features, the ring and the cursor, a site's order number from
   the states, footprints from the types, the pace from the entry. The tick the cursor's shape
   began is history rather than state, and is left out. */
uint64_t tagpu_wikisim_hash(const WIKISIM* s);


#endif
