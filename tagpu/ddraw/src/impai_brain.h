#ifndef IMPAI_BRAIN_H
#define IMPAI_BRAIN_H
/* impai_brain.h -- the brain's own half, shared by impai_brain.c (the census, the job checks, the entry),
   impai_econ.c (the ladder, the type choice, the factories) and impai_build.c (the sites and the orders): one
   decision's working state, IMPAI_DEC, which lives in the caller's scratch for the call and nothing else. */
#include <string.h>
#include "impai.h"
#include "tagpu_placegeo.h"
#include "tasim_sweep.h"

/* A SURPLUS: the metal store at this share of its maximum or more, where an idle factory the table's buy leaves
   without an order buys its cheapest army type (impai_econ.c, THE GOVERNOR), the same share as the maker's energy
   rule (impai_econ.c, assign) [hand-written: the bar's to move] */
#define SURPLUS_FRAC 0.8f
#define IMPAI_MAKERS_MAX 64     /* the makers a census lists for THE MAKERS (impai_econ.c)                   */

/* an economy building's category: what THE HOLD still builds and helps (impai_brain.c, economy). A macro, so the
   replay gate's coverage list gains no function */
#define IMPAI_ECO_CAT(c) ((c) == IMPAI_C_MEX || (c) == IMPAI_C_GEO || (c) == IMPAI_C_ENERGY || (c) == IMPAI_C_MAKER)

typedef struct IMPAI_RECT { int16_t x0, z0, x1, z1; } IMPAI_RECT;        /* cells, inclusive               */
/* way: a plant's way out; head: a builder's build head (impai_brain.c, THE HEADS' COUNTS) */
/* a claim on a rectangle: `way` a kept way out's, `head` a build order's at a builder's head, `feat` a reclaim job's
   feature, its rectangle anchored at the feature's anchor (impai_brain.c, census), `room` THE AIR PLANT'S ROOM's
   (impai_econ.c), which an air plant passes (impai_claimed_for) */
typedef struct IMPAI_CLAIM { uint16_t type; IMPAI_RECT r; uint8_t way, head, feat, room; } IMPAI_CLAIM;
typedef struct IMPAI_STRUCT { uint16_t slot, type; IMPAI_RECT r; uint8_t cat, finished; } IMPAI_STRUCT;
/* one of the seat's buildings with a radar or sonar distance (impai_econ.c, THE SENSORS): its footprint's centre,
   px, and its distances */
typedef struct IMPAI_SENSE { int32_t x, z; uint16_t radar, sonar; } IMPAI_SENSE;
enum { IMPAI_SENSE_NONE, IMPAI_SENSE_RADAR, IMPAI_SENSE_SONAR };
typedef struct IMPAI_UNITREF { uint16_t slot, type; int32_t pos[3]; } IMPAI_UNITREF;
/* THE YARD's region of a waiting factory (impai_brain.c, THE YARD): its footprint, r[0], then the rectangles of its
   kept way out (IMPAI_BRAIN.way; impai_build.c, THE WAY OUT), the first n used; the factory's type, whose exits
   THE WAY OFF reads */
typedef struct IMPAI_YARDREG { IMPAI_RECT r[1 + IMPAI_WAY_RECTS]; int n; uint16_t type; } IMPAI_YARDREG;
/* a point a builder leaves a yard for this decision, px, and the builder's half-size and slot (impai_build.c,
   THE WAY OFF) */
typedef struct IMPAI_WAYTO { int32_t x, z, h; uint16_t slot, pad; } IMPAI_WAYTO;

#define IMPAI_SEA_BUILDERS 8         /* the builder types impai_econ_sea_able and the plant choice read the menus
                                        of                                                                     */
#define IMPAI_MR_MEMO      16        /* constructor types THE METAL'S REACH remembers its answer for, a decision */
#define IMPAI_PLANT_CANDS  16        /* the plant types the plant choice considers (impai_econ.c, THE CHOICE)   */
#define IMPAI_GATE_TYPES   16        /* the factory types the army orders' gate reads (THE ARMY ORDERS)        */
/* THE ARMY ORDERS' gated classes are classes 0 .. IMPAI_GATE_K - 1 (IMPAI_GATED, impai.h), the index of the
   memo's per-class rows: a class is tested below IMPAI_GATE_K before it indexes one (impai_econ.c, gate_read) */
#define IMPAI_GATE_K       3
_Static_assert(IMPAI_GATED == (1u << IMPAI_GATE_K) - 1u, "the gated classes are 0 .. IMPAI_GATE_K - 1");
#define IMPAI_EXIT_TYPES   8         /* the plant types a decision keeps the exits of                          */
#define IMPAI_WAY_NODES    4096      /* the way-out search's budget, nodes (impai_build.c, THE WAY OUT; its
                                        cost, THE COST)                                                       */
#define IMPAI_WAY_BOX      160       /* the side of the box it searches, cells                                 */
#define IMPAI_RECLAIM_R    768       /* px around a unit a look at the features shown covers (impai_build.c,
                                        THE FEATURE MEMORY)                                                  */
#define IMPAI_HARVEST_R    2048      /* px around a builder a reclaim's remembered features lie within
                                        (impai_build.c, THE RECLAIM)                                         */
#define IMPAI_RECLAIM_LOOK 32        /* the nearest features shown one look keeps                               */
#define IMPAI_RECLAIM_SCANS 2        /* builders a decision searches for a feature, each with its own look      */
#define IMPAI_RECLAIM_TRIES 4        /* reach tests a search                                                    */
#define IMPAI_RECLAIM_DECAY 1800     /* ticks: a remembered feature's confidence is DECAY / (DECAY + its age)   */
#define IMPAI_CMD_FOES     64        /* the seen armed enemies near the commander's reach a decision lists
                                        (impai_build.c, THE COMMANDER'S SAFE DISTANCE)                        */

/* A plant type's exits (impai_build.c, THE WAY OUT): each edge of the footprint that holds yard cells, with the
   run of them along it, first to last, relative to the plant's first cell; `classes` its ground products' movement
   classes, a bit each, and for each class c csize[c] the largest footprint side of its products and (cfx[c],
   cfz[c]) the least on each axis, the class's own footprint; `size` the largest csize. n = 0: nothing leaves on the
   ground -- no yard cell on an edge, or no ground product with a class. read = 0: the yard, the build menu or a
   product's footprint could not be read whole, which refuses the plant. */
typedef struct IMPAI_EXITS {
    uint16_t   type;
    uint8_t    n, size, read;
    uint32_t   classes;
    uint8_t    edge[4];              /* 0 north, 1 south, 2 west, 3 east                                      */
    int16_t    a[4], e[4];
    uint8_t    csize[WQ_MCLASSES], cfx[WQ_MCLASSES], cfz[WQ_MCLASSES];
} IMPAI_EXITS;

/* the way-out search's scratch beside IMPAI_DEC (impai_brain.c, carve): the content map of the box's places, and
   the blocker raster's sums over the box (impai_build.c, THE WAY OUT) */
#define IMPAI_WAY_PASS_BYTES  WQ_PASS_CAP(IMPAI_WAY_BOX, IMPAI_WAY_BOX)
#define IMPAI_WAY_BLOCK_CELLS ((size_t)(IMPAI_WAY_BOX + 2) * (size_t)(IMPAI_WAY_BOX + 2))

typedef struct IMPAI_DEC {
    const IMPAI_BRAIN_IN* in;
    IMPAI_BRAIN_OUT*      out;
    const WORLDQ*         q;
    WQ_MAP                m;
    WQ_ECON               e;
    uint32_t              now;
    int                   act_build, act_vtol_build, act_queue, act_park;   /* the world's action bytes; -1 none */
    int                   act_on, act_off;      /* the on and off orders' bytes; -1 none (THE MAKERS)              */
    /* the census */
    uint16_t      count[IMPAI_CATS], done[IMPAI_CATS], pending[IMPAI_CATS];
    IMPAI_UNITREF idle[IMPAI_IDLE_CAP], helpers[IMPAI_IDLE_CAP], jobbed[2 * IMPAI_IDLE_CAP];
    uint8_t       jobbed_idle[2 * IMPAI_IDLE_CAP];
    IMPAI_UNITREF fidle[IMPAI_IDLE_CAP], fbusy[IMPAI_IDLE_CAP];
    uint8_t       fbusy_q[IMPAI_IDLE_CAP];      /* 1: that busy factory's head is a BuildingBuild (THE YARD)  */
    /* THE WAY OFF's points of the decision: one a builder -- an idle builder's live walk (THE WALKS FIRST), or a
       builder THE YARD moves off a region, its own point replaced --, so at most the idle and helper lists'
       2 x IMPAI_IDLE_CAP */
    IMPAI_WAYTO   way_to[2 * IMPAI_IDLE_CAP];
    int           nway_to;
    int           nidle, nhelpers, njobbed, nfidle, nfbusy;
    int           nbusy;                        /* finished builders at a job, not idle (THE ATTENTION)  */
    IMPAI_CLAIM   claims[IMPAI_CLAIM_MAX];
    int           nclaims, claims_full;         /* claims_full: one was not kept, so every test is claimed;
                                                   unreachable by IMPAI_CLAIM_MAX's bound                   */
    IMPAI_EXITS   exits[IMPAI_EXIT_TYPES];      /* the decision's plant types' exits, in first use       */
    int           nexits;
    uint8_t       yard[WQ_FOOT_MAX * WQ_FOOT_MAX];
    /* the way-out search (impai_build.c, THE WAY OUT): its visited bits over the box, its queue (a node's
       first cell and the queue index it came from), the class searches this decision ran, the last way proven,
       and its scratch: the content map of the box's places and the blocker raster */
    uint8_t       way_seen[IMPAI_WAY_BOX * IMPAI_WAY_BOX / 8];
    int16_t       way_x[IMPAI_WAY_NODES], way_z[IMPAI_WAY_NODES], way_from[IMPAI_WAY_NODES];
    int           way_ok;                       /* `way` is the proven way of its plant and site         */
    int32_t       way_work;                     /* the way out's work this decision, cells (impai_build.c,
                                                   THE WORK BUDGET)                                       */
    int           way_waive;                    /* 1: the plant ordered now goes without a way (THE PLANT'S
                                                   FALLBACK)                                              */
    IMPAI_WAYREC way;
    uint8_t*      way_pass;                     /* IMPAI_WAY_PASS_BYTES                                  */
    int32_t*      way_block;                    /* IMPAI_WAY_BLOCK_CELLS                                 */
    IMPAI_STRUCT  structs[IMPAI_CENSUS_MAX];    /* every building of the census                           */
    int           nstructs;
    IMPAI_UNITREF nano[IMPAI_NANO_MAX];
    int           nnano;
    IMPAI_UNITREF frames[IMPAI_NANO_MAX];       /* the seat's unfinished mobile units: a factory's products on
                                                   its yard (impai_build.c, THE COMMANDER HELPS AT HOME)   */
    int           nframes;
    /* THE GOVERNOR'S COUNTS (impai_econ.c, THE GOVERNOR). The seat's constructors and army units are each its
       finished units plus its factories' count nodes. A count node holds the unit in progress (worldq.h
       WQ_ORDER.count, +0x3A), so a frame is counted there and never again by the census. A frame whose factory is
       gone is not counted, which errs toward the army. BOUND: a census lists at most IMPAI_CENSUS_MAX units, each
       factory's walk at most 8 nodes of at most IMPAI_QCOUNT_HELD units each, so gov_c and gov_a stay below
       1 500 x 2 041 < 2^31. pressed, surplus and hold (THE HOLD): impai_brain.c, economy. */
    int32_t       gov_c, gov_a;
    float         surplus_m;    /* the metal committed past the unit each factory makes: the busy factories'
                                   queued units at the census (impai_brain.c, THE QUEUED METAL) and this
                                   decision's surplus orders past their first (impai_econ.c, THE FIVE,
                                   bounded)                                                                  */
    int           pressed, surplus, hold;
    /* THE GROUND DEFENCES (impai_econ.c): the seat's ground defences, built, frames and head orders; whether the
       ladder builds one; and the defence's point it is built nearest, px (impai_brain.c, def_point). def_n counts
       every structure impai_def_struct takes, wherever it stands, an anti-air tower with a ground reach included
       by design (it fires on ground units when no aircraft is a candidate), so def_max such towers built against
       air hold the ground defences back */
    uint16_t      def_n;
    int           want_def;
    int32_t       def_x, def_z;
    /* THE SENSORS (impai_econ.c): the seat's sensor structures, built, frames and head orders, each once (sens_n);
       the first IMPAI_SENSORS of its buildings with a radar or sonar distance, sensor structures or not (sens);
       the want's kind (IMPAI_SENSE_*) and its point, px (impai_brain.c, sensor_want) */
    IMPAI_SENSE   sens[IMPAI_SENSORS];
    uint16_t      sens_n, nsens;
    int           want_sensor;
    int32_t       sensor_x, sensor_z;
    IMPAI_UNITREF commander, lowest;            /* slot 0: none                                          */
    int           explorer_live;
    float         pe_extra;                     /* the energy the seat's frames and build heads will add,
                                                   each building once (THE HEADS' COUNTS)                 */
    int           has_bbox;
    IMPAI_RECT    bbox;
    /* the economy: the plan's ratios (impai_brain_decide)                                              */
    float         e_per_m, factory_per_m;
    /* THE STEADY INCOME (impai_brain.c, economy): the census's one-off metal of the last pass -- each unit's
       but an extractor's, past its own make rate -- and the metal a pass less it, which every income gate reads */
    float         m_paid, m_steady;
    int           e_stall, m_stall, want_factory;
    /* THE PLANT'S PRICE (impai_brain.c, economy): the metal and the energy the seat's busy factories asked in the
       last pass, summed over the price_n of them that asked metal; the overflow (the producers' wait), the plant's
       metal term and energy term; and
       the energy a pass the seat is short of for the next plant, which THE LADDER's energy rule adds to its
       target */
    float         price_m, price_e;
    int           price_n, over_m, plant_m, plant_e;
    float         plant_e_need;
    /* THE MAKERS (impai_econ.c): the finished onoffable makers with an energy cost, in slot order, at most
       IMPAI_MAKERS_MAX, each with its activation bit; the energy a pass the active ones cost and the count of the
       inactive ones, over all of them, as this decision's switches leave them; the energy cost of the ones this
       decision switched on */
    IMPAI_UNITREF mk[IMPAI_MAKERS_MAX];
    uint8_t       mk_active[IMPAI_MAKERS_MAX];
    int           nmk, mk_off_n;
    float         mk_on_e, mk_new_e;
    float         mk_lag_e, mk_lag_m;   /* THE FLOWS' LAG (impai_econ.c, THE MAKERS): the energy a pass and the metal
                                           the makers switched since the last pass add to its flows, below 0 for
                                           those switched off                                                    */
    /* the reclaim's economy (impai_brain.c, economy; impai_build.c, THE RECLAIM): the seat short of metal for
       what it wants next; the metal its reclaims under way will pay, and the room its store keeps for more */
    int           m_short;
    float         m_inflight, m_room;
    /* the army's production: each class's value built and queued -- the finished units and the factories'
       count nodes, which hold the unit in progress (gov_a's rule), so a frame is counted once, by its node --; the
       highest build tech (IMPAI_TINFO.ctech) of the seat's factories, frames included, and how many have it; its
       anti-air structures, built, building and ordered, each once (THE HEADS' COUNTS) */
    float         kspent[IMPAI_KS];
    uint8_t       ftop;
    uint16_t      ftop_n, aa_n;
    int           want_tech, want_aa;
    float         pe_in, pm_in;
    /* the sea plant (impai_econ.c, impai_build.c): the seat's water factories, frames and head orders included;
       the distinct types of its finished commanders and constructors, in census order; the want; the
       decision's one search -- its type (WQ_NONE before it), its answer (1 a site the test accepted, at
       (sea_cx, sea_cz) with sea_alt; 0 none this decision; -1 none at all) -- and whether the decision has
       counted its refusal */
    uint16_t      sea_plants;
    uint16_t      air_plants;   /* its air plants, finished and frames (impai_econ_air_plant)                 */
    uint16_t      btype[IMPAI_SEA_BUILDERS];
    uint8_t       nbtype;
    /* THE UNLOCKS (impai_econ.c): bit t of cons_done, the seat has a finished constructor of type t; of
       cons_frame, a constructor frame of type t. Whole bitsets over WQ_TYPES_MAX, not a capped list, so no type
       the seat has reads as lacking however many types it holds */
    uint32_t      cons_done[WQ_TYPES_MAX / 32], cons_frame[WQ_TYPES_MAX / 32];
    int           want_sea;
    uint8_t       sea_far;      /* THE FINISH AT SEA (impai_brain.c, economy): the military's dry hunt holds, so
                                   the search reaches FAR_PATCH_K x comm_r, the commander as far           */
    uint8_t       sea_able;     /* in THE FINISH AT SEA, a builder offers a sea plant (impai_econ_sea_able)  */
    uint16_t      sea_type;
    /* THE METAL'S REACH (impai_econ.c), judged at first use in a decision (mr_done): more patches lie open than the
       seat has extractors (mr_open); a finished constructor of the seat's reaches one (mr_units); a constructor
       type on a menu of its finished factories does (mr_make); THE REACHING PLANT stands on the seat's menus, so
       the further plant is only one (mr_only); the constructor types already judged and their answers */
    uint8_t       mr_done, mr_open, mr_units, mr_make, mr_only, nmr;
    uint16_t      mr_type[IMPAI_MR_MEMO];
    uint8_t       mr_val[IMPAI_MR_MEMO];
    int           sea_answer, sea_cx, sea_cz, sea_alt, sea_counted;
    int           sea_untaken;                  /* a builder was refused the site (THE SITE NO BUILDER TAKES)  */
    int           blast_hit;                    /* THE BLAST'S APPROACH refused the commander a point          */
    int           com_held;                     /* the commander is the military's (THE COMMANDER'S WINDOW)    */
    /* what the decision has spent of its budgets, and the roots the orders before the factories' may reach
       (impai_brain.c, THE FACTORIES' ROOTS) */
    int           sites, snaps, roots, queued, assigned, stopped, explored_once, reclaim_scans;
    int           roots_max, root_cap;
    int           contested;                    /* THE FACTORIES' TURN's decision (impai_brain.c, root_cap_of) */
    int           busied;                       /* idle builders this decision made busy (THE ATTENTION)  */
    int           air_sites;                    /* THE AIR PLANT'S OWN SEARCH's site tests (impai_build.c) */
    int           air_reach;                    /* its reach refusals for the builder asked (impai_build.c) */
    /* THE COMMANDER'S SAFE DISTANCE (impai_build.c): the seen armed enemies within comm_r + IMPAI_CMD_SAFE_PX + 1
       of the anchor, at their blocks' centres, px; listed at the decision's first test of a point within comm_r
       (0 not yet, 1 listed, -1 more than IMPAI_CMD_FOES, which refuses every such point) */
    int32_t       cfoe_x[IMPAI_CMD_FOES], cfoe_z[IMPAI_CMD_FOES];
    int           ncfoe, cfoe_listed;
    /* the features a look finds shown, the memory's scores (impai_build.c, THE RECLAIM), and the builder the
       decision's own look goes round (the census: the first reclaimer past IMPAI_BRAIN.fscan, else the first) */
    SWEEP_TARGET  rt[IMPAI_RECLAIM_LOOK];
    double        rscore[IMPAI_FEATS];
    uint8_t       fheld[IMPAI_FEATS];           /* the memory's entries a reclaim job claims, for THE PLACE IN A
                                                   FULL MEMORY (impai_build.c), set by impai_fmem_observe  */
    IMPAI_UNITREF fobs, ffirst, aobs, afirst;
    uint16_t      rslot;                        /* the builder the last search looked round; 0 none      */
    /* THE CHOICE and THE ARMY ORDERS' per-decision memo (impai_econ.c): the reach mask (bit c: movement class c
       reaches a plausible enemy base from the base) once read; the plant choice once made -- the plant, the
       waived pass's plant when no type passes the refusals, the runner-up, both scores, the constructor the
       plant waits for (THE UNLOCKS, 0 none), whether the pass that found it was a tech-up's, and whether the
       plant is the sea plant (THE ISLAND'S FIRST PLANT, THE ISLAND'S PLANTS) and whether a builder of the seat's
       offers one --; and the distinct factory types the gate has
       read, each type's army classes and its best value in the three gated classes by the reach and without it
       (IMPAI_Q_NONE none), the reach mask, and the gated classes the gate reads without it (bit k); THE TARGETS
       BOUGHT FOR, the target classes the army buys types to hit (bit g, impai.h THE TARGET CLASSES), once read;
       THE ISLAND'S AGREEMENT, once read: 0 not yet, 1 the economy's reach agrees, 2 not; THE ISLAND'S PLANT
       ORDER, once read: 0 not yet, ISLE_SEA, ISLE_LAND (impai_econ.c); 1 in plant_air_blind while THE AIR PLANT
       FOR THE BLIND took the plant from the sea plant in this decision (impai_econ.c) */
    uint32_t      reach_mask;
    int           reach_done, isle_agree, isle_order;
    unsigned      tg_mask;
    int           tg_done;
    int           plant_done, plant_up, plant_sea, plant_sea_menu, plant_air_blind;
    uint16_t      plant_t, plant_tw, plant_next, plant_via;
    float         plant_s, plant_next_s;
    uint16_t      gate_type[IMPAI_GATE_TYPES];
    uint32_t      gate_classes[IMPAI_GATE_TYPES];
    float         gate_best[IMPAI_GATE_TYPES][IMPAI_GATE_K], gate_all[IMPAI_GATE_TYPES][IMPAI_GATE_K];
    int           ngate;
    uint32_t      gate_mask, gate_noreach;
    /* scratch beside the struct: three build menus (the builder's, a plant's or constructor's, and the plant
       choice's builder menu), a short order list, the ring's sites, the threat maps */
    uint16_t*     menu;
    uint16_t*     menu2;
    uint16_t*     menu3;
    WQ_ORDER      olist[8];
    PlaceCell*    ring;
    IMPAI_MAPS    maps;
    int           maps_built;                   /* 0 not yet, 1 built, -1 none to build                  */
    /* THE HELD GROUND (impai_build.c): each 256-px cell's steps from the seat's strength and from the enemy's,
       the walk's queue, IMPAI_GRID_MAX^2 each; held_built 0 not judged this decision, 1 judged, -1 no maps */
    uint8_t      *held_own, *held_foe;
    uint16_t*     held_q;
    int           held_built;
    /* THE PATCH ANOTHER PLAYER BUILT ON (impai_build.c): bit k set when a building of another player's the seat's
       rows hold touches spots[k]'s box; foreign_built 0 not judged this decision, 1 judged */
    uint8_t       foreign[IMPAI_CLUSTERS_HALF / 8];
    int           foreign_built;
} IMPAI_DEC;

/* 1 when `type` is an anti-air structure: a building whose threat row's anti-air dps passes its ground dps;
   0 with no threat table */
static inline int impai_aa_struct(const IMPAI_BRAIN_IN* in, unsigned type)
{
    const IMPAI_THREAT* th;
    if (!in->view || !in->view->threat || type >= (unsigned)in->view->ntypes || type >= in->nti) return 0;
    th = &in->view->threat[type];
    return !(in->ti[type].flags & IMPAI_TF_MOBILE) && th->dps_air > th->dps_ground;
}

/* 1 when `type` is a ground defence: a building of category OTHER with a ground reach (IMPAI_THREAT.hit_ground,
   the engine's target rule, impai_maps.c THE GROUND REACH), whatever its role; 0 with no threat table. A missile
   tower is one, and an anti-air structure (impai_aa_struct) too: by the engine's rule it fires on ground units
   when no aircraft is a candidate. Category OTHER keeps out plants, extractors and energy that carry a weapon. */
static inline int impai_def_struct(const IMPAI_BRAIN_IN* in, unsigned type)
{
    if (!in->view || !in->view->threat || type >= (unsigned)in->view->ntypes || type >= in->nti) return 0;
    return !(in->ti[type].flags & IMPAI_TF_MOBILE) && in->ti[type].cat == IMPAI_C_OTHER &&
           in->view->threat[type].hit_ground > 0;
}

/* 1 when `type` is a sensor structure (impai_econ.c, THE SENSORS): a building of category OTHER with a radar or
   sonar distance and no reach of its own -- no ground reach and no anti-air rate --, so a tower carrying a radar
   beside its gun is a defence, and a plant or an extractor with a radar is its category; 0 with no threat table */
static inline int impai_sensor_struct(const IMPAI_BRAIN_IN* in, unsigned type)
{
    const IMPAI_TINFO* t;
    if (!in->view || !in->view->threat || type >= (unsigned)in->view->ntypes || type >= in->nti) return 0;
    t = &in->ti[type];
    return !(t->flags & IMPAI_TF_MOBILE) && t->cat == IMPAI_C_OTHER && (t->radar || t->sonar) &&
           !in->view->threat[type].hit_ground && !in->view->threat[type].dps_air;
}

/* 1 when `type` is a water factory, a plant placed on water only: what the seat's sea plant is */
static inline int impai_water_plant(const IMPAI_BRAIN_IN* in, unsigned type)
{
    return type < in->nti && in->ti[type].cat == IMPAI_C_FACTORY && (in->ti[type].flags & IMPAI_TF_WATER);
}

/* a job that ends: its task cleared, the builder's far record kept (IMPAI_JOB.far_*), which belongs to the unit
   and not to the task and goes only with the slot's unit (the census's clear) */
/* THE ATTENTION (impai_econ.c): 1 while the economy may make another builder busy -- the persona's att_eco, 0 no
   limit, above the census's busy builders and the decision's assignments */
int impai_econ_room(const IMPAI_DEC* d);
/* 1 while THE RELEASE needs the decision's last root (impai_econ.c, IT IS NEVER STARVED BY A BUDGET): a budgeted
   decision in THE HOLD with a helper to release */
int impai_release_keep(const IMPAI_DEC* d);

/* THE ECONOMY'S ORDERS: every order the economy gives goes through here -- the command it costs, the order, and
   the commander's marker (impai_brain.c, THE COMMANDER'S MARKERS). The order's answer. */
int impai_give(IMPAI_DEC* d, const WQ_CMD* c);

static inline void impai_job_end(IMPAI_JOB* j)
{
    const int16_t fx = j->far_cx, fz = j->far_cz;
    const uint16_t ft = j->far_type;
    const uint32_t until = j->far_until;
    const int16_t hx = j->help_cx, hz = j->help_cz;
    const uint16_t hy = j->help_type;
    const uint32_t hu = j->help_until;
    memset(j, 0, sizeof *j);
    j->type = j->build = j->target = (uint16_t)WQ_NONE;
    j->far_cx = fx;
    j->far_cz = fz;
    j->far_type = ft;
    j->far_until = until;
    j->help_cx = hx;
    j->help_cz = hz;
    j->help_type = hy;
    j->help_until = hu;
}

/* impai_brain.c */
void impai_fail_add(IMPAI_DEC* d, int cx, int cz);
int  impai_failed_in(const IMPAI_DEC* d, const IMPAI_RECT* r);
int  impai_claimed(const IMPAI_DEC* d, const IMPAI_RECT* r);
int  impai_claimed_for(const IMPAI_DEC* d, const IMPAI_RECT* r, int air);
void impai_claim_add(IMPAI_DEC* d, int type, const IMPAI_RECT* r);
IMPAI_RECT impai_rect_at(const IMPAI_DEC* d, int type, int cx, int cz);
uint32_t impai_isqrt(uint64_t v);
uint32_t impai_dist_px(const int32_t a[3], int32_t bx_px, int32_t bz_px);

/* impai_econ.c */
void impai_econ_makers(IMPAI_DEC* d);
void impai_econ_assign(IMPAI_DEC* d);
void impai_econ_factories(IMPAI_DEC* d);
/* 1 when some builder type of the census, at most IMPAI_SEA_BUILDERS, offers a sea plant; it may overwrite d->menu */
int  impai_econ_sea_able(IMPAI_DEC* d);
/* 1 when `type` is an air plant: a factory not placed on water only whose menu offers an air army type */
int  impai_econ_air_plant(IMPAI_DEC* d, unsigned type);
/* THE AIR PLANT'S ROOM (impai_econ.c): after the census, the kept room checked and claimed, or let go */
void impai_econ_room_keep(IMPAI_DEC* d);
/* the room's footprint and way claimed, each claim marked `room` */
void impai_econ_room_claim(IMPAI_DEC* d);
/* THE METAL'S REACH (impai_econ.c): 1 past the seat's first finished factory, with no plant frame standing, when
   more patches lie open than it has extractors and no constructor it has or its finished factories make reaches
   one; judges mr_open, mr_units and mr_make on the way */
int  impai_econ_metal_short(IMPAI_DEC* d);
/* 1 on A METAL MAP (impai_econ.c): the threshold 255, so no patch is ever published and the extractors stand on the
   base ring */
int  impai_metal_map(const IMPAI_DEC* d);
/* 1 when a plant on the menus of the seat's builders, placed on its ground, makes a constructor that reaches an
   open patch (THE METAL'S REACH) */
int  impai_econ_reach_plant(IMPAI_DEC* d);
/* one more refusal of the sea plant, at most one a decision and one a fail_t (impai_build.c, THE SEA PLANT) */
void impai_sea_refusal(IMPAI_DEC* d);
/* THE REACH ABANDON's limit (impai_brain.c): the farthest, px, a builder of `btype` may stand from the centre of an
   sfx x sfz footprint and still build it -- its builddistance plus both footprints' truncated 8 x hypot; -1 for a
   type with no information or no def */
int64_t impai_reach_limit(const IMPAI_DEC* d, int btype, int sfx, int sfz);

/* THE EXTRACTOR FOR THE PATCH (impai_econ.c, choose_mex; impai_build.c, impai_mex_at): a builder's extractor types,
   one for each kind of ground, 0 none; wet_min the water type's minwaterdepth, above 0 whenever wet is set */
typedef struct IMPAI_MEX { uint16_t dry, wet; int16_t wet_min; } IMPAI_MEX;

/* impai_build.c: each 1 when the builder was given its order. impai_build_spot: the extractor of `mex` for the
   ground at the patch impai_spot_pick gives */
int  impai_build_spot(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_MEX* mex);
/* impai_build.c: the published patches that are open -- free of claims, failures and other players' buildings, on
   held ground -- with their anchor's block on reach label `label` of class `mclass` (label -1: any) and, with mex,
   an extractor of mex for their ground (THE METAL'S REACH). BOUND: nspots patches */
int  impai_patch_open_to(IMPAI_DEC* d, unsigned mclass, int label, const IMPAI_MEX* mex);
int  impai_build_ring(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* `type` at the ring site nearest the defence's point (d->def_x, d->def_z; impai_econ.c, THE GROUND DEFENCES) */
int  impai_build_def(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* `type` covering the sensor want's point (d->sensor_x, d->sensor_z; impai_econ.c, THE SENSORS): 1 ordered, 0 not,
   -1 a site this builder never takes (impai_build.c) */
int  impai_build_sensor(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
int  impai_build_explore(IMPAI_DEC* d, const IMPAI_UNITREF* b);
int  impai_build_assist(IMPAI_DEC* d, const IMPAI_UNITREF* b);
/* the plant of `type` at the first ring site the site test passes, without a way out (impai_build.c, THE PLANT'S
   FALLBACK); 1 ordered */
int  impai_build_plant_waived(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* the air plant for the blind of `type`: the base ring's site, else a site outside it (impai_build.c, THE AIR PLANT'S
   OWN SEARCH), without a way out when `waived`; 1 ordered */
int  impai_build_air(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int waived);
/* THE AIR PLANT'S ROOM's search: the site impai_build_air would order for builder b, kept as the room instead;
   1 kept */
int  impai_build_air_room(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* 1 while the factory choice passes plant type `type` over (impai_build.c, THE PLANT'S FALLBACK) */
int  impai_plant_off(const IMPAI_DEC* d, int type);
/* 1 when b is sent to a feature paying more than `min_rate` metal a tick (THE RECLAIM) */
int  impai_build_reclaim(IMPAI_DEC* d, const IMPAI_UNITREF* b, double min_rate);
/* THE FEATURE MEMORY (impai_build.c): the decision's refresh of what the seat is shown and its looks; the
   remembered feature anchored at (ax, az), its index or -1; its removal */
void impai_fmem_refresh(IMPAI_DEC* d);
void impai_fmem_observe(IMPAI_DEC* d);
int  impai_fmem_find(const IMPAI_BRAIN* st, int ax, int az);
void impai_fmem_drop(IMPAI_DEC* d, int k);
/* THE FEATURE SHOWN (impai_build.c): 1 when the feature remembered as e is there and the game draws it for the
   seat, read into *f, and a reclaim can take it; 0 when the seat knows it is not -- the draw shows its place
   without it, or it can no longer be taken; -1 when the seat cannot know */
int  impai_feat_seen(const IMPAI_DEC* d, const IMPAI_FMEM* e, WQ_FEAT* f);
/* 1 for a feature a reclaim can take: reclaimable, with metal, and not playing its sequence */
int  impai_feat_takeable(const WQ_FEAT* f);
/* the reclaim of a walk's feature the draw shows there: 1 ordered, 0 not this decision (the roots spent), -1 refused
   (the builder avoids it, THE FAR RECORD), IMPAI_RECLAIM_UNSAFE the commander refused by THE COMMANDER'S SAFE
   DISTANCE (impai_build.c) */
#define IMPAI_RECLAIM_UNSAFE (-2)
int  impai_reclaim_arrived(IMPAI_DEC* d, const IMPAI_UNITREF* b, const WQ_FEAT* f);
/* 1 ordered, 0 not this decision, -1 no site for it at all (impai_build.c, THE SEA PLANT) */
int  impai_build_sea_plant(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* THE SITE NO BUILDER TAKES (impai_build.c), after the decision's assignment */
void impai_sea_untaken(IMPAI_DEC* d);
/* THE FRAME RESUMED (impai_build.c): b helps an unfinished water plant's frame, or already does; 1, else 0 */
/* 1 when the sea search has found no candidate at all for the anchor, the estimate and the radius it holds now,
   whatever the type (IMPAI_BRAIN.sea_none_*) */
int  impai_sea_none_now(const IMPAI_DEC* d);
int  impai_build_sea_frame(IMPAI_DEC* d, const IMPAI_UNITREF* b);
/* ground threat on the seat's maps at the 256-px cell of a point; and at it or on builder b's straight way there
   (impai_build.c, THE ROUTE) */
int  impai_threatened(IMPAI_DEC* d, int x_px, int z_px);
int  impai_walk_threatened(IMPAI_DEC* d, const IMPAI_UNITREF* b, int x_px, int z_px);
/* builder b sent back out of known ground threat, its job ended (impai_build.c, THE TURN BACK); 1 ordered */
int  impai_turn_back(IMPAI_DEC* d, const IMPAI_UNITREF* b);
/* 1 when a unit of `type` standing at px (x, z) touches region g: lies inside one of its rectangles grown by the
   type's half-size (impai_build.c, THE YARD's move) */
int  impai_yard_holds(const IMPAI_DEC* d, const IMPAI_YARDREG* g, int type, int64_t x, int64_t z);
/* THE WALK OFF's trust in b's walk to px (x, z) (impai_build.c, THE WAY OFF): off g, on b's class's own reach
   label, clear of the seat's buildings and of what holds still -- the decision's other points and the idle builders
   it holds no point for --, and reached from where b stands without crossing g's plant */
int  impai_way_off(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_YARDREG* g, int64_t x, int64_t z);
/* the point (x, z) px builder b leaves a yard for this decision, into IMPAI_DEC.way_to: b's own point replaced,
   one a builder */
void impai_way_to(IMPAI_DEC* d, const IMPAI_UNITREF* b, int64_t x, int64_t z);
int  impai_yard_leave(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_YARDREG* g);
/* the patch impai_build_spot would take for builder b, without ordering, or NULL; its extractor into *type (type
   may be NULL). mex NULL: any free patch, its far test with no type -- the builder offers no extractor */
const SNAP_PATCH* impai_spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_MEX* mex, int* type);
/* the extractor of `mex` for the ground at world px (px, pz): 0 none (impai_build.c, THE EXTRACTOR FOR THE PATCH) */
int  impai_mex_at(const IMPAI_DEC* d, const IMPAI_MEX* mex, int px, int pz);
/* the seat's kept ways out, each a claim while its plant stands or is ordered (impai_build.c, THE WAY OUT) */
void impai_ways_keep(IMPAI_DEC* d);
/* 1 while builder b's refused help names the 16-px cell of the 16.16 point pos (impai_brain.c, THE HELP
   REFUSED) */
int  impai_help_refused(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const int32_t pos[3]);
/* 1 while the builder's far record is live (impai_brain.c, THE FAR RECORD) */
int  impai_far(const IMPAI_DEC* d, const IMPAI_UNITREF* b);
/* 1 when the builder's far record is live and a site of `type` centred at px (pcx, pcz) lies beyond its reach from
   where it stands (THE REACH ABANDON's compare) */
int  impai_far_px(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int pcx, int pcz);
/* 1 while the builder's far record is live and its recorded spot (IMPAI_JOB.far_cx, far_cz) lies in r */
int  impai_far_in(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_RECT* r);
/* for the site of `type` at first cell (cx, cz): 1 while the record is live and the site holds the recorded spot
   or lies beyond the builder's reach */
int  impai_far_at(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz);
/* the same for a feature of footprint r centred at px (pcx, pcz) */
int  impai_far_feat(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_RECT* r, int pcx, int pcz);
/* the footprint of the feature anchored at (ax, az), fx x fz cells */
IMPAI_RECT impai_feat_rect(int ax, int az, int fx, int fz);

#endif
