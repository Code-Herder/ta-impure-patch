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

typedef struct IMPAI_RECT { int16_t x0, z0, x1, z1; } IMPAI_RECT;        /* cells, inclusive               */
typedef struct IMPAI_CLAIM { uint16_t type; IMPAI_RECT r; uint8_t way; } IMPAI_CLAIM;   /* way: a plant's way out */
typedef struct IMPAI_STRUCT { uint16_t slot, type; IMPAI_RECT r; uint8_t cat, finished; } IMPAI_STRUCT;
typedef struct IMPAI_UNITREF { uint16_t slot, type; int32_t pos[3]; } IMPAI_UNITREF;

#define IMPAI_SEA_BUILDERS 8         /* the builder types impai_econ_sea_able and the plant choice read the menus
                                        of                                                                     */
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
#define IMPAI_RECLAIM_R    768       /* px around a builder a look at the features in sight covers, and a
                                        reclaim's remembered features lie within (impai_build.c, THE RECLAIM) */
#define IMPAI_RECLAIM_LOOK 32        /* the nearest features in sight one look keeps                            */
#define IMPAI_RECLAIM_SCANS 2        /* builders a decision searches for a feature, each with its own look      */
#define IMPAI_RECLAIM_TRIES 4        /* reach tests a search                                                    */
#define IMPAI_RECLAIM_DECAY 1800     /* ticks: a remembered feature's confidence is DECAY / (DECAY + its age)   */
#define IMPAI_CMD_FOES     64        /* the seen armed enemies near the commander's ring a decision lists
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
    /* the census */
    uint16_t      count[IMPAI_CATS], done[IMPAI_CATS], pending[IMPAI_CATS];
    IMPAI_UNITREF idle[IMPAI_IDLE_CAP], helpers[IMPAI_IDLE_CAP], jobbed[2 * IMPAI_IDLE_CAP];
    uint8_t       jobbed_idle[2 * IMPAI_IDLE_CAP];
    IMPAI_UNITREF fidle[IMPAI_IDLE_CAP], fbusy[IMPAI_IDLE_CAP];
    int           nidle, nhelpers, njobbed, nfidle, nfbusy;
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
       1 500 x 2 041 < 2^31. pressed and surplus: impai_brain.c, economy. */
    int32_t       gov_c, gov_a;
    int           pressed, surplus;
    /* THE GROUND DEFENCES (impai_econ.c): the seat's ground defences, built, frames and head orders; whether the
       ladder builds one; and the defence's point it is built nearest, px (impai_brain.c, def_point). def_n counts
       every structure impai_def_struct takes, wherever it stands, an anti-air tower with a ground reach included
       by design (it fires on ground units when no aircraft is a candidate), so def_max such towers built against
       air hold the ground defences back */
    uint16_t      def_n;
    int           want_def;
    int32_t       def_x, def_z;
    IMPAI_UNITREF commander, lowest;            /* slot 0: none                                          */
    int           explorer_live;
    float         pe_extra;                     /* the energy the seat's claims and frames will add      */
    int           has_bbox;
    IMPAI_RECT    bbox;
    /* the economy: the level row's ratios with the decision's noise on them (impai_brain_decide) */
    float         e_per_m, factory_per_m;
    int           e_stall, m_stall, want_factory;
    /* the reclaim's economy (impai_brain.c, economy; impai_build.c, THE RECLAIM): the seat short of metal for
       what it wants next; the metal its reclaims under way will pay, and the room its store keeps for more */
    int           m_short;
    float         m_inflight, m_room;
    /* the army's production: each class's value built, building and queued; the highest build tech
       (IMPAI_TINFO.ctech) of the seat's factories, frames included, and how many have it; its anti-air
       structures, built, building and ordered */
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
    uint16_t      btype[IMPAI_SEA_BUILDERS];
    uint8_t       nbtype;
    /* THE UNLOCKS (impai_econ.c): bit t of cons_done, the seat has a finished constructor of type t; of
       cons_frame, a constructor frame of type t. Whole bitsets over WQ_TYPES_MAX, not a capped list, so no type
       the seat has reads as lacking however many types it holds */
    uint32_t      cons_done[WQ_TYPES_MAX / 32], cons_frame[WQ_TYPES_MAX / 32];
    int           want_sea;
    uint16_t      sea_type;
    int           sea_answer, sea_cx, sea_cz, sea_alt, sea_counted;
    /* what the decision has spent of its budgets, and the roots the orders before the factories' may reach
       (impai_brain.c, THE FACTORIES' ROOTS) */
    int           sites, snaps, roots, queued, assigned, stopped, explored_once, reclaim_scans;
    int           root_cap;
    /* THE COMMANDER'S SAFE DISTANCE (impai_build.c): the seen armed enemies within comm_r + IMPAI_CMD_SAFE_PX of
       the anchor, at their blocks' centres, px; listed at the decision's first commander ring walk (0 not yet, 1
       listed, -1 more than IMPAI_CMD_FOES, which refuses every commander ring site) */
    int32_t       cfoe_x[IMPAI_CMD_FOES], cfoe_z[IMPAI_CMD_FOES];
    int           ncfoe, cfoe_listed;
    /* the features a look finds in sight, the memory's scores (impai_build.c, THE RECLAIM), and the builder the
       decision's own look goes round (the census: the first reclaimer past IMPAI_BRAIN.fscan, else the first) */
    SWEEP_TARGET  rt[IMPAI_RECLAIM_LOOK];
    double        rscore[IMPAI_FEATS];
    IMPAI_UNITREF fobs, ffirst;
    uint16_t      rslot;                        /* the builder the last search looked round; 0 none      */
    /* THE CHOICE and THE ARMY ORDERS' per-decision memo (impai_econ.c): the reach mask (bit c: movement class c
       reaches a plausible enemy base from the base) once read; the plant choice once made -- the plant, the
       waived pass's plant when no type passes the refusals, the runner-up, both scores and the constructor the
       plant waits for (THE UNLOCKS, 0 none) --; and the distinct factory types the gate has read, each type's
       army classes and its best value in the three gated classes by the reach and without it (IMPAI_Q_NONE
       none), the reach mask, and the gated classes the gate reads without it (bit k) */
    uint32_t      reach_mask;
    int           reach_done;
    int           plant_done;
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

/* 1 when `type` is a water factory, a plant placed on water only: what the seat's sea plant is */
static inline int impai_water_plant(const IMPAI_BRAIN_IN* in, unsigned type)
{
    return type < in->nti && in->ti[type].cat == IMPAI_C_FACTORY && (in->ti[type].flags & IMPAI_TF_WATER);
}

/* a job that ends: its task cleared, the builder's far record kept (IMPAI_JOB.far_*), which belongs to the unit
   and not to the task and goes only with the slot's unit (the census's clear) */
static inline void impai_job_end(IMPAI_JOB* j)
{
    const int16_t fx = j->far_cx, fz = j->far_cz;
    const uint16_t ft = j->far_type;
    const uint32_t until = j->far_until;
    memset(j, 0, sizeof *j);
    j->type = j->build = j->target = (uint16_t)WQ_NONE;
    j->far_cx = fx;
    j->far_cz = fz;
    j->far_type = ft;
    j->far_until = until;
}

/* impai_brain.c */
void impai_fail_add(IMPAI_DEC* d, int cx, int cz);
int  impai_failed_in(const IMPAI_DEC* d, const IMPAI_RECT* r);
int  impai_claimed(const IMPAI_DEC* d, const IMPAI_RECT* r);
void impai_claim_add(IMPAI_DEC* d, int type, const IMPAI_RECT* r);
IMPAI_RECT impai_rect_at(const IMPAI_DEC* d, int type, int cx, int cz);
uint32_t impai_isqrt(uint64_t v);
uint32_t impai_dist_px(const int32_t a[3], int32_t bx_px, int32_t bz_px);

/* impai_econ.c */
void impai_econ_assign(IMPAI_DEC* d);
void impai_econ_factories(IMPAI_DEC* d);
/* 1 when some builder type of the census, at most IMPAI_SEA_BUILDERS, offers a sea plant; it may overwrite d->menu */
int  impai_econ_sea_able(IMPAI_DEC* d);
/* one more refusal of the sea plant, at most one a decision and one a fail_t (impai_build.c, THE SEA PLANT) */
void impai_sea_refusal(IMPAI_DEC* d);

/* impai_build.c: each 1 when the builder was given its order */
int  impai_build_spot(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
int  impai_build_ring(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* `type` at the ring site nearest the defence's point (d->def_x, d->def_z; impai_econ.c, THE GROUND DEFENCES) */
int  impai_build_def(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
int  impai_build_explore(IMPAI_DEC* d, const IMPAI_UNITREF* b);
int  impai_build_assist(IMPAI_DEC* d, const IMPAI_UNITREF* b);
/* the plant of `type` at the first ring site the site test passes, without a way out (impai_build.c, THE PLANT'S
   FALLBACK); 1 ordered */
int  impai_build_plant_waived(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* 1 while the factory choice passes plant type `type` over (impai_build.c, THE PLANT'S FALLBACK) */
int  impai_plant_off(const IMPAI_DEC* d, int type);
/* 1 when b is sent to a feature paying more than `min_rate` metal a tick (THE RECLAIM) */
int  impai_build_reclaim(IMPAI_DEC* d, const IMPAI_UNITREF* b, double min_rate);
/* THE FEATURE MEMORY (impai_build.c): the decision's refresh of what the seat sees and its one look; the
   remembered feature anchored at (ax, az), its index or -1; its removal; whether the anchor's block is in the
   seat's sight now; and a RECLAIM_GO's order once its feature is in sight and there, 1 ordered, 0 the decision's
   commands spent, -1 refused */
void impai_fmem_refresh(IMPAI_DEC* d);
void impai_fmem_observe(IMPAI_DEC* d);
int  impai_fmem_find(const IMPAI_BRAIN* st, int ax, int az);
void impai_fmem_drop(IMPAI_BRAIN* st, int k);
int  impai_feat_lit(const IMPAI_DEC* d, int ax, int az);
/* 1 for a feature a reclaim can take: reclaimable, with metal, and not playing its sequence */
int  impai_feat_takeable(const WQ_FEAT* f);
int  impai_reclaim_arrived(IMPAI_DEC* d, const IMPAI_UNITREF* b, const WQ_FEAT* f);
/* 1 ordered, 0 not this decision, -1 no site for it at all (impai_build.c, THE SEA PLANT) */
int  impai_build_sea_plant(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* ground threat on the seat's maps at the 256-px cell of a point; and at it or on builder b's straight way there
   (impai_build.c, THE ROUTE) */
int  impai_threatened(IMPAI_DEC* d, int x_px, int z_px);
int  impai_walk_threatened(IMPAI_DEC* d, const IMPAI_UNITREF* b, int x_px, int z_px);
/* builder b sent back out of known ground threat, its job ended (impai_build.c, THE TURN BACK); 1 ordered */
int  impai_turn_back(IMPAI_DEC* d, const IMPAI_UNITREF* b);
/* the patch builder b would take for an extractor of `type` (-1: none known), or NULL (impai_build.c) */
const SNAP_PATCH* impai_spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
/* the seat's kept ways out, each a claim while its plant stands or is ordered (impai_build.c, THE WAY OUT) */
void impai_ways_keep(IMPAI_DEC* d);
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
