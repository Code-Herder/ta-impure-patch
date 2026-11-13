#ifndef TASIM_STATE_H
#define TASIM_STATE_H
/* tasim_state.h -- the shared simulation core's state: TASIM, everything a simulation of the core is, and
   TASIM_DEFS, the definitions it plays, with the records both are made of. research/notes/impai/shared-core.md,
   *The state*.

   THE WIKI'S REPRESENTATIONS. TASIM is the core's share of the in-game wiki's simulation, in the forms the wiki
   plays: units on the heap in creation order with string ids and double positions, each with its order list;
   the features standing; the map; a game's seats and deaths; the COB host and its facts; the spray and the
   sound; the movement's scratch. The wiki's front end embeds one (WIKISIM.sim) beside its definitions (WIKISIM.defs);
   a headless caller owns one of its own. TASIM_DEFS is never written by the core.

   THE BOUNDARY. No core file includes tagpu_wikisim.h: the core reads its TASIM, its TASIM_DEFS and the records
   below, the entry's rows (tagpu_wikidata.h) and the COB host (tagpu_wikicob.h), and calls into its owner
   only through the unit hook. tests/tasim_link_test.c links the core without the front end.

   ENGINE-FREE, WIN32-FREE AND WITHOUT GLOBALS; NO CLOCK AND NO RANDOMNESS: every build agrees bit for bit
   (tagpu_wikisim.h, EVERY BUILD AGREES BIT FOR BIT, states how). One TASIM is used by one thread at a time. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_placegeo.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikicob.h"

#define WIKI_CELL 16
#define WIKI_TPS  30

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
   particle or a burst past its bound is not made, and the view says so (part_cut). */
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
/* A UNIT'S DEATH, an event of a game simulation's (WIKI_PLAY_GAME): kind WIKI_SND_DEATH, `name` the
   type's index in the entry, `cat` -1 and `variant` the map cell under the unit's centre,
   cz x W + cx. Past the kinds, so it indexes no per-kind table, and tagpu_wikisound_reply refuses
   it. The destructor explodes a finished unit through 0x49B000 with its ExplodeAs weapon, whose
   soundhit 0x499FE9 plays through 0x47F300, gated by the viewed player's sight
   (0x47F3E8..0x47F4A8) and with no owner test (DISASSEMBLED): the player plays the type's death
   WAV (WIKISNDTYPE.death) when the viewed seat's fog at the cell is not black. */
#define WIKI_SND_DEATH  WIKI_SND_KINDS
/* the kind table 0x5086D8's priority (+4) and interval in seconds (+8) by kind, dumped from the
   retail executable's .data */
#define WIKI_SND_PRIORITIES { 0, 10, 9, 4, 4, 5, 3, 8, 3, 4, 3, 2, 7, 7, 7, 7, 4, 10, 10, 10, 10, 10, 10, 10 }
#define WIKI_SND_INTERVALS  { 0, 0, 20, 2, 2, 1, 4, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 }

enum { WIKI_SND_SELECT = 1, WIKI_SND_ACTIVATE = 3, WIKI_SND_DEACTIVATE = 4, WIKI_SND_OK = 5,
       WIKI_SND_ARRIVED = 6, WIKI_SND_CANT = 7, WIKI_SND_COMPLETE = 8, WIKI_SND_BUILD = 9,
       WIKI_SND_WORKING = 11 };

/* the allsound.tdf names the wiki plays, by index, and their spelling: the strings the engine's
   callers push to 0x47F1A0 (0x508D8C, 0x509660, 0x509650, 0x5026B8, 0x5027EC, 0x5027F8, 0x502850,
   0x50271C), and "Options" as the in-game menu's controls and tagpu_menu.c's play it. allsound.tdf's
   names match without case (0x4F8A70). tools/wiki/gfx.py SND_NAMES spells the same list. */
enum { WIKI_SNDNAME_MULTI, WIKI_SNDNAME_OKTOBUILD, WIKI_SNDNAME_NOTOKTOBUILD, WIKI_SNDNAME_ADDBUILD,
       WIKI_SNDNAME_BUILDBUTTON, WIKI_SNDNAME_ORDERSBUTTON, WIKI_SNDNAME_NEXTBUILDMENU,
       WIKI_SNDNAME_IMMEDIATE, WIKI_SNDNAME_OPTIONS, WIKI_SNDNAME_SPECIAL, WIKI_NSNDNAME };
#define WIKI_SNDNAME_STRINGS { "SelectMultipleUnits", "oktobuild", "notoktobuild", "addbuild", "buildbutton", \
                               "ordersbutton", "nextbuildmenu", "immediateorders", "Options", "specialorders" }

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
                                       the order's identity, which a COB work rests on */
    int    type;                    /* BUILD: the building's type */
    int    ox, oz;                  /* BUILD: its footprint's first cell */
    double x, z;                    /* MOVE: where to; a feature's RECLAIM: its centre */
    int    feat;                    /* RECLAIM: the target is a feature, `target` its id */
    char   target[WIKI_ID_MAX];     /* RECLAIM, GUARD: the unit's id, or the feature's */
} WIKIORDER;

/* A FEATURE STANDING on the map: its id, its row of the entry's table and that
   row's figures, its anchor cell and its centre */
typedef struct {
    char   id[WIKI_ID_MAX];
    int    def;
    int    ox, oz, fx, fz;
    double x, z, metal, energy;
    int    height, blocking, reclaimable;
} WIKIFEAT;

/* a rect in world units; the movement's bodies (tasim_move.c, MOVEMENT) */
typedef struct { double x0, z0, x1, z1; } WIKIRECT;

/* A UNIT'S ECONOMY LEDGER, a game simulation's: produced, requested, granted and debt per resource,
   the engine's unit+0xBC/+0xC0/+0xC4/+0xC8 (energy) and +0xD4/+0xD8/+0xDC/+0xE0 (metal), f32 there
   (research/notes/engine/weapons.md, *The ledger*) */
typedef struct { double eP, eR, eG, eD, mP, mR, mG, mD; } WIKILEDGER;

/* A SEAT'S ECONOMY, the engine's player record (0x401360, the economy pass): m, e stored (+0x98,
   +0x8C); bm, be the base storage, the seat's settings (+0xE0, +0xDC), which the seat setup
   0x496EE0 floors at 200 (0x496F64..0x496F97) and the level start 0x465E30 also stores as the
   current (0x465EF5..0x465F1A); mmax, emax the storage (+0xA8, +0xA4), which the player init
   zeroes (0x46478F, 0x464795) until the first pass (a seat's settings are WIKI_SEAT_MIN or more,
   so the floor holds by data); mi, mo, ei, eo the last pass's income (produced) and spending (requested)
   (+0x9C, +0xA0, +0x90, +0x94); wm, we the wasted totals (+0xD4, +0xCC); units alive, a nanoframe
   included (+0x144, incremented at each create 0x486187, 0x486322); lost, the commanders lost (+0x106); out, the count gone from above 0 to 0;
   cmd_lost, a commander lost (+0x149 bit 0 cleared: the base storage stops counting); kills and
   losses, the units it killed and lost (+0xFC, +0xFE; tasim_world.c tasim_kill) */
typedef struct {
    double m, e, bm, be, mmax, emax, mi, mo, ei, eo, wm, we;
    int    units, lost, out, cmd_lost, kills, losses;
} WIKIECON;

/* A DEATH in a game (tasim_world.c tasim_kill), for the replay's death vision: the tick the kill was
   fed on, the unit's owner, its type (an index of the entry's types) and its place. Only a death
   the engine's temporary sight record add keeps is one (tasim_kill states the rule). */
typedef struct { int tick, owner, type; double x, z; } WIKIDEATH;

/* a game view's type's sight figures (WIKIDEF's), the stamp's and the radar pass's: activates is
   ActivateWhenBuilt (def+0x241 bit 18), the only way a wiki unit is switched on */
typedef struct { int sight_distance, radar_distance, sonar_distance, activates, model_height; } WIKIVTYPE;

typedef struct WIKIUNIT {
    char             id[WIKI_ID_MAX];
    int              type, owner, sel;
    double           built, reclaimed;  /* fractions, 0..1 */
    int              bp, page;          /* the owner's (TASIM_UNIT_ENTER): with the wiki's sidebar,
                                           unit+0x110 bit 22 (a numbered page shown rather than
                                           the orders page) and bits 23..25 (the page, 1..n; 0 for
                                           a type without pages) */
    const char*      label;
    double           x, z;              /* its centre */
    double           hx, hz;            /* its facing, a unit vector (walk says why not an angle);
                                           a structure's is (0, 1) */
    int              ox, oz;            /* a structure's first footprint cell; unused for a mobile unit */
    int              norder, corder;
    WIKIORDER*       order;
    /* one tick's work (tasim_advance): the structure it worked on and how, and on a structure the work
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
    /* movement (tasim_move.c, MOVEMENT): whether it stepped on the last tick; whether this
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
    WIKILEDGER       led;               /* a game simulation's; 0 in any other */
} WIKIUNIT;

/* a body another unit's step must not newly overlap: a unit's, or a claim's (u NULL) */
typedef struct { WIKIRECT r; const WIKIUNIT* u; } WIKIBODY;

/* the planner's heap entry: its f, the order it was pushed in, its node */
typedef struct { double f; int seq, n; } WIKIHEAPNODE;

/* a view's unit (WIKIVIEW.unit), which the sight rules read (TASIM_SIGHTIN) */
typedef struct {
    const char* id;
    int         type, owner, fx, fz, selected;
    double      x, z, hx, hz, built, reclaimed;
    const char* label;
    int         cant;   /* its first order's taken site has said cant */
} WIKIVUNIT;

enum { WIKI_ECON_REFUSE, WIKI_ECON_STALL };

/* THE SIGHT GRID's bound and THE DEATH VISION's span (tasim_sight.c states both rules) */
#define WIKI_SIGHT_GRID  ((WIKI_MAP_MAX / 2) * (WIKI_MAP_MAX / 2))
#define WIKI_SIGHT_SPAN  60      /* a death at tick d lights its seat's grid through d + 60: the
                                    temporary sight record's expiry (tasim_sight.c) */

/* What the sight rules read of a game's view (tasim_sight.c): its map's size, whether it is a game, its units,
   its types' sight figures (index-aligned with the bound entry's types, a unit's and a death's `type`) and its
   deaths. Borrowed for the call. */
typedef struct {
    int              w, h, game;
    int              nunit;
    const WIKIVUNIT* unit;
    int              ntype;
    const WIKIVTYPE* type;
    int              ndeath;
    const WIKIDEATH* death;
} TASIM_SIGHTIN;

/* THE UNIT HOOK, the core's one call into its owner, at the three points a unit's life touches the owner's
   state: ENTER once the unit is in the list and before its script spawns (the wiki sets its page state, bp and
   page, from its sidebar's layout); LEAVE before the unit is freed (the wiki forgets it as the sidebar's shown
   unit); BUILT when a structure's build reaches 1 (the wiki's demo clock counts the player's).
   THE ORDERING: the hook runs synchronously, at the instant and on the thread of the core's call, so every read
   and write it makes keeps its place among the core's.
   THE CONTRACT, which that ordering rests on: the hook writes its owner's state and the unit's owner fields (bp,
   page) only, reads nothing of the core but the unit it is given, and never calls into the core. NULL: no
   owner to tell, a headless TASIM. */
enum { TASIM_UNIT_ENTER, TASIM_UNIT_LEAVE, TASIM_UNIT_BUILT };
typedef void (*TASIM_HOOK)(void* ctx, WIKIUNIT* u, int ev);

/* THE DEFINITIONS a TASIM plays, read and never written by the core: the entry (with a casting the bound entry,
   tagpu_wikisim_init says how), its unit types and its features table. It and everything it points into
   outlive the TASIM. */
typedef struct {
    const WIKIENTRY* entry;
} TASIM_DEFS;

/* THE CORE'S STATE. The caller owns it and everything on the heap it points to: tasim_map_alloc makes the map's
   arrays; the caller the vents, the features and the gone cells, the COB host and the spray's pools; the core
   grows the unit list, the units' orders and its scratch as it needs them; tasim_free frees them all. `hook`
   and `hook_ctx` are its owner's and never hashed. */
typedef struct {
    int              tick;          /* the ticks stepped */
    /* THE MAP: W x H cells, SurfaceMetal and the entry's pace; each cell's metal and fog (0 clear, 1 grey,
       2 black), row-major; `rich`, (W + 1) x (H + 1), the cells metal_at finds above SurfaceMetal summed from
       the map's top-left corner, so a footprint's coverage is four reads -- its owner builds it once metal is
       final, and nothing writes metal after; the vents, each one's cell and its centre */
    int              W, H;
    double           SM, PACE;
    double*          metal;
    unsigned char*   fog;
    int*             rich;
    int              nvent;
    PlaceCell*       vent;
    WIKIPOINT*       vent_centre;
    WIKIUNIT**       unit;          /* in creation order; a reclaimed one leaves the list */
    int              nunit, cunit, next_id;
    int              next_serial;   /* the orders made, the last serial given */
    /* the features standing, in the scene's order; the anchor cells of those reclaimed, gone[2k],
       gone[2k + 1]; both at most WIKI_FEATS_MAX */
    WIKIFEAT*        feat;
    int              nfeat, ngone;
    int*             gone;
    /* A GAME SIMULATION (WIKI_PLAY_GAME, _STALL): its seats' economies, the economy's mode (WIKI_ECON_*),
       whether a pass starved and a kill found no unit, the kills made. 0 and untouched in any other sim. */
    int              game, econ_mode, nseat, starved, bad_kill, nkill;
    WIKIECON         econ[WIKI_SEATS];
    /* a game's deaths the sight record add keeps, in the order made and never removed. BOUND: at most
       WIKI_FEATS_MAX, which the data rule holds a game's kills to, its script's and every fork part's; one
       past it is not recorded, in either twin. */
    WIKIDEATH        death[WIKI_FEATS_MAX];
    int              ndeath;
    /* the viewed seat (the engine's main+0x2A43), whose units' notices play: 0 until its owner moves it
       (tasim_set_viewed) */
    int              viewed;
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
    TASIM_HOOK       hook;
    void*            hook_ctx;
} TASIM;

#endif
