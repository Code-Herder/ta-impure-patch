#ifndef IMPAI_H
#define IMPAI_H
/* impai.h -- ImpAI's engine-free half: what each seat knows of every unit slot and the metal patches it has
   seen, kept by one tick a seat over a WORLDQ (worldq.h), the engine's (tagpu_impai.c) or the shared core's
   (tests/wikitest.c `impai`); the reach labels of a movement class; and the maps derived from all of it.
   research/notes/modules/impai.md, What a seat knows.

   THE INFORMATION RULE. A seat's row of a slot changes only through what the seat observes: the WORLDQ's
   observe (its own units, its line of sight, an ally's shared radar), its own radar pass, and its own sight of
   the block where it last knew something. A death or a slot reuse the seat cannot see changes nothing it knows,
   so no engine fact reaches a decision through a row.

   A RADAR CONTACT IS THE UNIT THE PASS FOUND. A pass spans ticks, and by its publish the unit whose bit it set
   may have died and its slot gone to another unit, or moved on. So the publish takes a contact only when the
   slot's unit is still the incarnation the pass recorded at the bit (WQ_RADAR_HIT.birth, equal to observe's
   birth, both known), and puts it at the block the pass recorded, never the live unit's. A slot that changed
   hands within the pass keeps no bit of its earlier unit (worldq.h, WQ_RADAR_HIT), so the classes the contact
   takes are the ones its own unit was found by. A world that gives no incarnation gives no radar contact.

   No file of this half includes the engine header: thread-split.allow lists each as engine-free, which the
   check holds to the default-deny an unlisted file keeps, so it cannot read anything but its arguments: what
   it keeps and derives is a function of the WORLDQ's answers, its own state and the content tables it is
   given. Integers only, but for impai_threat_from_roles's roundings of the
   classifier's figures and of the weapons' ground reach; no clock, no random generator. */
#include <stddef.h>
#include <stdint.h>
#include "worldq.h"
#include "tasim_snap.h"
#include "tasim_roles.h"
#include "impai_table.h"
#include "impai_fight.h"

/* IMPAI_SEEN.how: the classes the seat holds the slot by now */
#define IMPAI_OWN     0x01u
#define IMPAI_LOS     0x02u
#define IMPAI_SHARED  0x04u
#define IMPAI_RADAR   0x08u
#define IMPAI_SONAR   0x10u
#define IMPAI_GHOST   0x20u     /* none any longer: the row is the last thing known, at its block        */
#define IMPAI_NOW     (IMPAI_OWN | IMPAI_LOS | IMPAI_SHARED)
#define IMPAI_PASS    (IMPAI_RADAR | IMPAI_SONAR)

/* One seat's knowledge of one slot, 8 bytes. An all-zero row is empty: the seat knows nothing there. */
typedef struct IMPAI_SEEN {
    uint16_t type;              /* the type last identified; WQ_NONE when never identified                */
    uint8_t  how;               /* IMPAI_*                                                                */
    uint8_t  owner;             /* 0..9, 0xFF when never identified                                       */
    uint16_t bx, bz;            /* the 32-px block last known                                             */
} IMPAI_SEEN;

/* 1 when row r is a contact the seat holds now and has never identified: by sight, an ally's or the radar pass,
   its type not one of the nti types or its owner unknown. The military counts each as its substitute, the median
   identified enemy mobile type (impai_mil.c, picture; IMPAI_MIL.radar_as), and the builders' ground threat map
   does the same (impai_maps_build), through this one test. A ghost is not held now, and a structure's ghost is
   identified: neither is one. */
static inline int impai_row_unidentified_now(const IMPAI_SEEN* r, unsigned nti)
{
    const int ident = r->type != WQ_NONE && r->type < nti && r->owner < WQ_SEATS;
    return r->how && !(r->how & IMPAI_OWN) && !ident && (r->how & (IMPAI_NOW | IMPAI_PASS)) != 0;
}
typedef struct IMPAI_ROW { IMPAI_SEEN seat[WQ_SEATS]; } IMPAI_ROW;     /* keyed by unit slot            */
/* THE ROW'S AGE: the line-of-sight slice visits every slot once in ceil(slots / slice) ticks, so a row of a unit
   the seat sees is at most this many ticks old; tagpu_impai.c asserts its slice keeps it at the design point's
   slots */
#define IMPAI_ROW_AGE_MAX 8

/* A seat's metal patches: the published set in the first half, the scan in progress in the second, so a
   half-done scan is never read. */
#define IMPAI_CLUSTERS_HALF 256
#define IMPAI_CLUSTERS      (2 * IMPAI_CLUSTERS_HALF)

/* One seat's progress through its observation, plain values. */
typedef struct IMPAI_SEAT {
    uint16_t obs_cursor;        /* the next slot the line-of-sight slice looks at                         */
    uint16_t clu_row;           /* the patch scan's next row                                              */
    uint16_t nclusters;         /* the published patches                                                  */
    uint16_t npending;          /* the scan's patches so far                                              */
    WQ_RADAR radar;             /* the radar pass in progress                                             */
    uint32_t known_gen;         /* bumped on every write of this seat's rows or patches                   */
    uint32_t radar_done;        /* the gametime the last pass was published                               */
} IMPAI_SEAT;

/* Every seat's radar record of one slot (worldq.h, WQ_RADAR_HIT), 160 bytes: a table keyed by slot, as the
   rows are, passes one seat's column (WQ_HITS, stride sizeof(IMPAI_HITROW)). */
typedef struct IMPAI_HITROW { WQ_RADAR_HIT seat[WQ_SEATS]; } IMPAI_HITROW;

/* One seat's arrays, the caller's: its rows (nrows, at least the world's nslots), its radar pass's bits
   (nbytes, at least WQ_RADAR_BYTES(nslots)) and records (hits.n, at least nslots) and its IMPAI_CLUSTERS
   patches. */
typedef struct IMPAI_ARRAYS {
    IMPAI_ROW*  rows;
    int         nrows;
    uint8_t*    bits;
    int         nbytes;
    WQ_HITS     hits;
    SNAP_PATCH* clusters;
} IMPAI_ARRAYS;

/* The budgets of one seat's tick, each a count the loops are bounded by. */
typedef struct IMPAI_BUDGET {
    uint32_t obs_slice;         /* slots the line-of-sight slice looks at                                 */
    uint32_t radar_visits;      /* the radar pass's target visits, rounded up to one walk                 */
    uint32_t clu_rows;          /* map rows the patch scan reads                                          */
    uint32_t clu_flood;         /* cells its floods may reach                                             */
} IMPAI_BUDGET;

/* What one tick did, against its bounds: `slots` the observe calls (the slice's, and a completed pass's
   publish), `visits` the radar pass's, `cells` the patch scan's cells read, `flood` its floods' cells;
   `scans` 1 when the tick ended a patch scan's pass and published its patches. */
typedef struct IMPAI_WORK {
    uint32_t slots, visits, cells, flood, scans;
    uint32_t stale;             /* under WQ_HITS.stamp, the published pass's set bits whose record is older
                                   than the pass's start: 0 when every bit has its own pass's record      */
} IMPAI_WORK;

/* The tick's phase of a radar pass: a seat starts one when idle and gametime % 30 == impai_radar_phase(seat),
   staggered so two seats never start on one tick. */
int impai_radar_phase(int seat);

/* One tick of seat q->seat: the line-of-sight slice, the radar pass's step and, when it completes, its
   publish (A RADAR CONTACT IS THE UNIT THE PASS FOUND, above), then the patch scan's rows with `scr` (the
   caller's, for this call; NULL skips the scan). Returns 0, or -1 when the arrays are short of the world
   (nothing is written). */
int impai_know_tick(const WORLDQ* q, IMPAI_SEAT* st, const IMPAI_ARRAYS* a, const IMPAI_BUDGET* b,
                    SNAP_SCRATCH* scr, IMPAI_WORK* w);

/* ---- reach: which 32-px blocks a movement class can travel between ---- */

/* the class's code at cell (cx, cz), worldq.h's passable */
typedef int (*IMPAI_PASSFN)(const void* ctx, int mclass, int cx, int cz);
#define IMPAI_REACH_MANY 0xFFFFu    /* a component past the labels a u16 holds                            */

/* the scratch impai_reach_label needs for a w x h map, bytes */
size_t impai_reach_work(int w, int h);
/* THE REACH LABELS of class `mclass` on a w x h map (even, from the WORLDQ's map): the passable cells (code
   not 0) labelled by 4-connectivity, 1 up, and each 32-px block given the label of its first passable cell in
   row-major order, 0 when it has none, into `labels`, (w / 2) x (h / 2). Returns the components found. */
int impai_reach_label(IMPAI_PASSFN fn, const void* ctx, int mclass, int w, int h, uint16_t* labels, void* work);

/* ---- the maps ---- */

/* A unit type's threat and worth, from the role classifier (tasim_roles.h): damage per second against ground,
   air and water x 16 (per tick x 30 x 16), its ground range in px and its value (metal + energy / R), each
   rounded once and held to its field; its TASIM_LAYER and TASIM_RF_* flags' low byte; and THE GROUND REACH,
   hit_ground and hit_range (impai_maps.c, impai_threat_from_roles), which the ground threat layer stamps. The
   role figures say what a type is for, the reach what it hits: a NOTAIR slot is anti-air by its role and still
   shoots ground units. 20 bytes. */
typedef struct IMPAI_THREAT {
    uint16_t dps_ground, dps_air, dps_water;
    uint16_t range;
    uint32_t value;
    uint8_t  layer, flags;
    uint16_t hit_ground, hit_range;     /* THE GROUND REACH: dps x 16 against ground units, and its range, px  */
    uint16_t pad;
} IMPAI_THREAT;
_Static_assert(sizeof(IMPAI_THREAT) == 20, "IMPAI_THREAT is 20 bytes, as its comment says");
/* the threat rows of n types from the classifier's input and answer (impai_maps.c) */
void impai_threat_from_roles(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* out, int n, IMPAI_THREAT* t);

#define IMPAI_CELL_PX  256      /* a map cell of the maps                                                */
#define IMPAI_GRID_MAX 128      /* 32767 px, the coordinate limit, over 256                              */
enum { IMPAI_THREAT_GROUND, IMPAI_THREAT_AIR, IMPAI_THREAT_WATER, IMPAI_THREATS };

/* What the maps are a function of: one seat's rows and patches, its allies (bit j: record j), the threat
   table (NULL: no threat, no influence) indexed by type and the map's size in 16-px cells. `hits` is the
   seat's column of its radar records (IMPAI_HITROW), which the maps do not read and the military tells a
   slot's incarnation by (impai_mil.c, target_now); n 0 for none. */
typedef struct IMPAI_VIEW {
    int                 seat;
    const IMPAI_ROW*    rows;
    int                 nrows;
    const SNAP_PATCH*   clusters;
    int                 nclusters;
    uint32_t            allies;
    const IMPAI_THREAT* threat;
    int                 ntypes;
    int                 w, h;
    WQ_HITS             hits;
} IMPAI_VIEW;

/* The maps, gw x gh cells of IMPAI_CELL_PX, into the caller's arrays of IMPAI_GRID_MAX^2 each:
   threat[layer], each identified enemy row's dps stamped on the cells whose centres lie within its type's
   range of its block's centre -- on the ground layer its ground reach, hit_ground over hit_range; influence, own rows' value minus identified enemies', summed over the 3 x 3
   cells around each cell; presence, the enemy rows known only as radar contacts or ghosts. `radar_as`: on the
   ground layer each contact never identified (impai_row_unidentified_now) stamps that type's ground reach at its
   block, as the military counts it (IMPAI_MIL.radar_as); WQ_NONE, or a type past the view's, stamps none. The
   builders pass the military's last, the military its decision's own (impai_mil.c, mil_maps), so both read one
   threat map; the overlay passes WQ_NONE. */
typedef struct IMPAI_MAPS {
    int       gw, gh, threat_built;
    uint32_t* threat[IMPAI_THREATS];
    int32_t*  influence;
    uint16_t* presence;
    int32_t*  diff;             /* scratch, (IMPAI_GRID_MAX + 1) x IMPAI_GRID_MAX                          */
} IMPAI_MAPS;
int impai_maps_build(const IMPAI_VIEW* v, IMPAI_MAPS* m, uint16_t radar_as);

/* THE ROUTE (impai_maps.c, the comment over impai_route_threat): the straight segment from (x0, z0) to (x1, z1) px
   on map m's ground threat layer, sampled once, judged by `rule` -- CLEAR: 1 when any cell past the start's holds
   known ground threat (the builders' rule); OUT: 1 when a cell's threat rises above the one before it, the start's
   first, or the last cell's is above 0 (THE MEND's way out). 0 otherwise. */
enum { IMPAI_ROUTE_CLEAR, IMPAI_ROUTE_OUT };
int     impai_route_threat(const IMPAI_MAPS* m, int64_t x0, int64_t z0, int x1, int z1, int rule);
/* the 256-px cell of a px coordinate, by floor division, so a coordinate below 0 lies off the grid */
int64_t impai_cell_of_px(int64_t v);

/* FNV-1a 64 digests: of one seat's column of the rows and its published patches, and of built maps */
uint64_t impai_known_digest(const IMPAI_VIEW* v);
uint64_t impai_maps_digest(const IMPAI_MAPS* m);

/* ---- type information: what the brain knows of a unit type (impai_tinfo.c) ---- */

/* A type's category, the first that holds (impai_tinfo_build) */
enum { IMPAI_C_NONE, IMPAI_C_MEX, IMPAI_C_GEO, IMPAI_C_ENERGY, IMPAI_C_MAKER, IMPAI_C_FACTORY,
       IMPAI_C_CONS, IMPAI_C_COMMANDER, IMPAI_C_OTHER, IMPAI_CATS };
#define IMPAI_TF_MOBILE 0x01u
#define IMPAI_TF_VTOL   0x02u
#define IMPAI_TF_WATER  0x04u           /* minwaterdepth > 0 or a tidal generator: placed on water only        */
#define IMPAI_TF_ONOFF  0x08u           /* onoffable (WQ_DF_ONOFF): the on and off orders switch it            */
#define IMPAI_TF_RECLAIM 0x10u          /* canreclamate (WQ_DF_RECLAIMER): the reclaim order's first test       */

/* One type, 32 bytes. The gains are per 30-tick economy pass on this map. */
/* IMPAI_TINFO.attack: the def's facts the order resolver's attack case (0x43F0E0 mode 3) tests of the unit
   ordered (DISASSEMBLED, engine/orders.md, *What each resolver mode answers*; impai_mil.c, can_attack) */
#define IMPAI_TA_CANATTACK 0x01u    /* +0x245 bit 4 (0x43F154)                                             */
#define IMPAI_TA_KAMIKAZE  0x02u    /* +0x241 bit 28 (0x43F38E)                                            */
#define IMPAI_TA_TOAIR1    0x04u    /* weapon 1 toairweapon, +0x111 bit 17 (0x43F1E1)                      */
#define IMPAI_TA_WATER1    0x08u    /* weapon 1 waterweapon, bit 16 (0x43F21B, 0x43F24F)                   */
#define IMPAI_TA_WATER2    0x10u    /* a weapon 2, waterweapon (0x43F223..0x43F230, 0x43F25B..0x43F264)    */
#define IMPAI_TA_HOVER     0x20u    /* +0x241 bit 12, canhover (0x43F244)                                  */
#define IMPAI_TA_FLY       0x40u    /* +0x241 bit 11, canfly (0x43F27E)                                    */
#define IMPAI_TA_DROP1     0x80u    /* weapon 1 dropped, +0x111 bit 8 (0x43F29D)                           */

typedef struct IMPAI_TINFO {
    uint8_t  cat, tech, flags, mclass;  /* IMPAI_C_*, the classifier's tech level, IMPAI_TF_*, WQ_DEF.mclass  */
    float    egain;     /* energymake + max(0, -energyuse) + windgenerator x the wind's mean + tidalgenerator x
                           the tide; for a maker, its energyuse: what it costs                                  */
    float    mgain;     /* metalmake + makesmetal; for an extractor, extractsmetal                              */
    float    value;     /* metal + energy / TASIM_ROLES_R                                                       */
    uint16_t workertime;
    int16_t  fx, fz;
    uint8_t  klass;             /* its army class, impai_table.h's IMPAI_K_*, IMPAI_K_NONE for none            */
    uint8_t  attack;            /* IMPAI_TA_*, from the content build                                          */
    uint8_t  ctech;             /* THE BUILD TECH (impai_tinfo_ctech): the highest classifier tech the type puts
                                   within the seat's reach -- a builder's, its own menu's; a factory's, its
                                   constructors' that can build an extractor; 0 none. What a tech-up compares (impai_econ.c, THE CHOICE): a
                                   plant's units' tech is a graph distance (tasim_roles.c, tech_levels), and the
                                   hover plant, built by tech-1 constructors, makes units of tech 2 with a
                                   constructor whose menu holds nothing above 1 (MEASURED 2026-11-14 on the
                                   install's menus: CORCH, ARMCH)                                              */
    uint8_t  dgun_r;            /* its D-gun's radius: half that weapon's areaofeffect (+0xD6), px            */
    uint16_t dgun_range;        /* its D-gun: the range px of its longest CommandFire weapon when the def can
                                   take the order (+0x245 bit 14), 0 none; none too when that weapon's radius
                                   passes 255 px, which dgun_r cannot hold                                     */
    uint16_t dgun_e;            /* that weapon's energy a shot (+0xC0), held to 0..65535                       */
    uint16_t blast_r;           /* half its ExplodeAs areaofeffect, px: the radius its death blast reaches
                                   (TASIM_ROLE_IN.explode_aoe); 0 none                                          */
} IMPAI_TINFO;
_Static_assert(sizeof(IMPAI_TINFO) == 32, "IMPAI_TINFO is 32 bytes, as its comment says");

/* Every type of q's world into ti[0..ndefs), ti[0] none: from its def, its economy keys, the map's wind and
   tide and the role classifier's answer for it (roles[t] for type t; with roles NULL or t >= nroles there is
   none, so the type is no commander, factory or constructor). Returns ndefs, or -1 when nti is short of it
   (nothing promised). */
int impai_tinfo_build(const WORLDQ* q, const TASIM_ROLE_OUT* roles, int nroles, IMPAI_TINFO* ti, int nti);
/* a type's IMPAI_TA_* facts (attack) and its D-gun (dgun_range, dgun_e) from its role input, the bits the role
   fillers copy (tasim_roles.h, TASIM_ROLE_F245) and its weapon slots: impai_tinfo.c, the comment over it */
void impai_tinfo_attack(const TASIM_ROLE_IN* in, IMPAI_TINFO* t);
/* how many types of each category ti[0..n) holds */
void impai_tinfo_count(const IMPAI_TINFO* ti, int n, unsigned count[IMPAI_CATS]);
/* Every type's build tech (IMPAI_TINFO.ctech) into ti[1 .. min(n, nti)) from the classifier's input and answer for
   the level's n types: a builder's the highest classifier tech among its build options -- the build menus merged
   over the sides, the input the tech walk read --, a factory's the highest build tech of the constructors
   (IMPAI_C_CONS) its menu holds whose own options hold an extractor (IMPAI_C_MEX), anything else's 0 (impai_tinfo.c, the comment over it). BOUND: an option at or
   past n is skipped. */
void impai_tinfo_ctech(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* roles, int n, IMPAI_TINFO* ti, int nti);

/* THE VALUES (impai_tinfo.c, impai_tinfo_values): an army type's value per metal on the tech levels 1..QTECH, the
   fight predictor's equal-value margin against the field an enemy of that tech could build, in [-1, 1] */
#define IMPAI_QTECH  4
#define IMPAI_QFIELD 32
#define IMPAI_Q_NONE (-2.0f)    /* below every value, which lies in [-1, 1]: no type to score                */
#define IMPAI_GATED  ((1u << IMPAI_K_LRAIDER) | (1u << IMPAI_K_LASSAULT) | (1u << IMPAI_K_LARTY))
typedef struct IMPAI_TVALUE { float q[IMPAI_QTECH]; } IMPAI_TVALUE;
_Static_assert(sizeof(IMPAI_TVALUE) == 16, "IMPAI_TVALUE is 16 bytes");
/* what a build of the values did: the types scored, the runs made, those refused (a run the predictor refused or
   whose margin was not finite, counted 0), and each tech level's field size */
typedef struct IMPAI_TVALUE_STATS { uint32_t scored, runs, refused; uint8_t field[IMPAI_QTECH]; } IMPAI_TVALUE_STATS;
/* The values of the types ti[1 .. nti) into out[0 .. nti), every row zeroed first, with the predictor's content c
   (its gravity taken as given: the caller passes 0), its coefficients p and its work w. 0, or -1 with nothing
   promised when an argument is NULL. */
int  impai_tinfo_values(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_TINFO* ti, unsigned nti,
                        IMPAI_TVALUE* out, IMPAI_FWORK* w, IMPAI_TVALUE_STATS* st);

/* THE MENUS (tagpu_impai.c, impai_menus_build): one row a (side, builder type), sorted by (side, builder),
   its build menu's types ascending and without repeats at pool[at .. at + n). */
#define IMPAI_MENU_SIDES 5u             /* the engine's five side records (tagpu_resource.c, seat_side) */
typedef struct IMPAI_BMENU { uint8_t side, pad; uint16_t builder, n, pad2; uint32_t at; } IMPAI_BMENU;

/* The row of (side, builder) among rows[0..nrows): its index, or -1 when there is none or its types do not lie
   inside a pool of npool. */
int impai_menu_find(const IMPAI_BMENU* rows, unsigned nrows, unsigned npool, unsigned side, unsigned builder);

/* Builder type `builder`'s build options as the role classifier takes them (tasim_roles.h, TASIM_ROLE_IN.build):
   the union of its rows of every side, ascending, into out[0..cap). The count, or -1 when cap is short. A
   builder's own pages are the same whatever the side, and only a page it has none of comes from the seat's side
   (tagpu_resource.c, page_file); the classifier's input has no seat, so the union gives a builder every type a
   seat of any side can have it build. */
int impai_menu_union(const IMPAI_BMENU* rows, unsigned nrows, const uint16_t* pool, unsigned npool,
                     unsigned builder, uint16_t* out, int cap);

/* ---- the brain: one seat's economy, a decision every look_eco ticks of its persona (impai_brain.c,
   impai_econ.c, impai_build.c; research/notes/impai/brain.md) ----

   WHAT IT READS. The WORLDQ (a WQ_SITE_SEAT view: the seat's own site test), its own state below, the type
   table, the seat's published metal patches, its reach labels and its view for the threat map, the plan and
   the seat's persona. It keeps unit slots and type numbers only, names a unit by slot in every command, and
   draws no random number and reads no clock: every choice is broken by a slot, a type number or a cell, and the
   census and the choices go in slot order, so the same world gives the same orders.

   WHAT BOUNDS IT. Every loop is bounded by a count: the seat's slot block, or a field of the plan. */

#define IMPAI_FAILS      32
#define IMPAI_SEA_REFUSALS 16           /* a seat's counted decisions with no sea plant before it is given up */
#define IMPAI_NO_ANCHOR  (-32768)
#define IMPAI_STRUCT_MAX 512            /* the military's list of the seat's own buildings                    */
#define IMPAI_NANO_MAX   32             /* unfinished buildings an assist may pick from                       */
#define IMPAI_IDLE_CAP   32             /* the idle builders, helpers and idle factories a census lists       */
/* THE CENSUS'S BOUND: the slots of the seat's block a census walks, the engine's units a player under the raised
   limits (TAGPU_LIM_UNITS, which tagpu_impai.c asserts is no more), so a census lists every unit of the seat's
   and every building in IMPAI_DEC.structs. */
#define IMPAI_CENSUS_MAX 1500
#define IMPAI_ROOTS_MAX  32             /* the plan's `roots` at most: impai_brain_decide refuses a plan past it */
#define IMPAI_QCOUNT_HELD 255           /* a factory count node's count is held to 0..this before it is summed:
                                           a bound on engine data, not a claim about the engine's largest count */

/* IMPAI_JOB_RECLAIM: reclaiming a feature in the seat's sight when ordered; IMPAI_JOB_RECLAIM_GO: walking to a
   remembered one to see whether it is still there (impai_build.c, THE RECLAIM). IMPAI_JOB_MEND_GO: a damaged army
   unit walking to its haven; IMPAI_JOB_MEND: one standing there to be repaired (impai_mil.c, THE MEND);
   IMPAI_JOB_REPAIR: a constructor repairing a mender (impai_build.c, THE REPAIR). IMPAI_JOB_SWITCH: a maker the
   economy switched on or off, `since` the GameTime of the switch (impai_econ.c, THE MAKERS). Appended, so no value
   moves. */
enum { IMPAI_JOB_NONE, IMPAI_JOB_BUILD, IMPAI_JOB_ASSIST, IMPAI_JOB_EXPLORE, IMPAI_JOB_SQUAD, IMPAI_JOB_RECLAIM,
       IMPAI_JOB_RECLAIM_GO, IMPAI_JOB_MEND_GO, IMPAI_JOB_MEND, IMPAI_JOB_REPAIR, IMPAI_JOB_SWITCH };
#define IMPAI_MEND_ROOTS_MAX 16         /* the plan's mend_roots at most: the mend's candidate buffer is twice it */
#define IMPAI_MEND_NEAR_PX   512        /* a mender within this of its haven is repaired; past it its repairer is
                                           turned back (impai_build.c, THE REPAIR; impai_brain.c, THE WALK CHECK) */

/* THE JOB TABLE, slot-keyed: the task ImpAI gave the unit in this slot, 28 bytes. A job holds only while the
   slot holds a live unit of the seat's of `type`: the census voids it otherwise. A slot reused by a unit of the
   same type inherits it, which costs at most one misjudged failure (a site avoided for fail_t): the unit's
   UnitInGameIndex is its slot again (+0xA8, written once a level at 0x485603), so nothing tells the two apart.
   The fields mean different things per kind. The army's, written by the military alone (impai_mil.c, THE MEND):
   - MEND_GO, a mender walking to its haven: cx, cz the haven's 32-px block; squad the squad it left (IMPAI_SQUADS
     or past: none); since the GameTime of the entry; feat the travel bound, ticks.
   - MEND, a mender standing at its haven: cx, cz and squad as MEND_GO; since the GameTime of the arrival or of its
     health's last rise; feat its hit points at `since`.
   - SQUAD: feat THE MEND'S MARK, 0 or the hit points a mend released the unit at unrepaired.
   - NONE after a release: feat the mark, which the join to FORM carries over.
   The economy's REPAIR (impai_build.c, THE REPAIR): target the mender's slot, since the GameTime of the order.
   Each keeps `type` the unit's. */
typedef struct IMPAI_JOB {
    uint16_t type;              /* the unit's type when tasked; WQ_NONE: no job                           */
    uint16_t build;             /* the type ordered; WQ_NONE                                               */
    uint16_t target;            /* an assist's or a repair's target slot; WQ_NONE                          */
    int16_t  cx, cz;            /* the site's first cell, the cell explored, the reclaimed feature's
                                   anchor cell, or a mender's haven block                                  */
    uint8_t  kind, squad;       /* IMPAI_JOB_*; IMPAI_JOB_SQUAD: its squad, below IMPAI_SQUADS; a mender:
                                   the squad it left                                                       */
    uint32_t since;             /* GameTime of the order; IMPAI_JOB_SQUAD: of its last step back          */
    int16_t  far_cx, far_cz;    /* THE FAR RECORD (impai_brain.c): the last site or feature this builder could
                                   not reach. Until far_until that spot is avoided by this builder alone, and
                                   so is every site or feature beyond its reach from where it stands; kept
                                   across its jobs, voided with the unit as a job is, by far_type          */
    uint16_t far_type;          /* the builder's type when it was recorded                                */
    uint16_t feat;              /* IMPAI_JOB_RECLAIM(_GO): the feature's definition, which with its anchor cell
                                   names it (impai_build.c, THE RECLAIM); an army unit's: a value of THE
                                   MEND's (the list above); 0 otherwise. A level's feature definitions are
                                   its content, the same numbers in its replay, and the mend's are ticks and
                                   hit points, so the value is plain and no ID of the job's layout          */
    uint32_t far_until;         /* 0: no record                                                            */
    int16_t  help_cx, help_cz;  /* THE HELP REFUSED (impai_brain.c): the cell of the frame this builder's
                                   last help ended at before the frame was finished. Until help_until THE
                                   ASSIST gives this builder no help on a frame at that cell -- a plant's
                                   products are all made at its one build point --; kept across its jobs,
                                   voided with the unit by help_type, as the far record is                  */
    uint16_t help_type;         /* the builder's type when it was recorded                                */
    uint16_t pad;               /* 0                                                                       */
    uint32_t help_until;        /* 0: no record                                                            */
} IMPAI_JOB;
_Static_assert(sizeof(IMPAI_JOB) == 40, "IMPAI_JOB is 40 bytes with no padding");

typedef struct IMPAI_FAIL { int16_t cx, cz; uint32_t until; } IMPAI_FAIL;

/* THE PLANT REFUSALS (impai_build.c, THE PLANT'S FALLBACK), one a unit type for each seat: the type's refused ways
   in a row, and the GameTime until which the factory choice passes it over. The plant types a seat's menus can
   offer are at most the world's types, so with an entry for every type none is ever evicted; the caller keeps
   them (IMPAI_BRAIN_IN.pref), outside IMPAI_BRAIN, whose rows a recording carries whole. */
typedef struct IMPAI_PLANTREF { uint32_t until; uint8_t n, pad[3]; } IMPAI_PLANTREF;

/* A plant site the way out refused (impai_build.c, THE WAY OUT), for plants of `type`, until a GameTime. */
#define IMPAI_WAY_FAILS 32
typedef struct IMPAI_WAYFAIL { int16_t cx, cz; uint16_t type, pad; uint32_t until; } IMPAI_WAYFAIL;

#define IMPAI_WAYS      16       /* the plants a seat keeps a way out for (impai_build.c, THE WAY OUT)     */
#define IMPAI_WAY_RECTS 8        /* the cell rectangles one way out is kept as                            */
/* THE CLAIM TABLE'S BOUND, so a full table is unreachable: a census unit adds at most one claim -- an extractor's
   or geothermal's site, or the build order at a builder's head, and a unit is one or the other --, the kept ways
   add at most IMPAI_WAYS x IMPAI_WAY_RECTS (a way joins them only while fewer than IMPAI_WAYS are kept), and each
   of the decision's commands at most one build order's site (impai_brain.c asserts it). */
#define IMPAI_CLAIM_MAX (IMPAI_CENSUS_MAX + IMPAI_WAYS * IMPAI_WAY_RECTS + IMPAI_ROOTS_MAX)
/* A plant's proven way out, kept for as long as the plant stands or is ordered: the plant's type and first
   cell, and the cells of the path near it, inflated by its largest product's footprint, as `n` rectangles
   (x0, z0, x1, z1, cells inclusive). Type WQ_NONE: an unused entry. */
typedef struct IMPAI_WAYREC {
    uint16_t type;
    int16_t  cx, cz;
    uint8_t  n, pad;
    int16_t  r[IMPAI_WAY_RECTS][4];
} IMPAI_WAYREC;

/* A feature the seat has seen in its own sight, which it may reclaim (impai_build.c, THE FEATURE MEMORY): its
   anchor cell and definition, which name it; its footprint and amounts as last seen; and the GameTime it was last
   seen. Out of sight it is a belief: anything may have taken it since. 20 bytes. */
#define IMPAI_FEATS 128          /* the features a seat remembers                                          */
typedef struct IMPAI_FMEM {
    int16_t  ax, az;
    uint16_t def;
    uint8_t  fx, fz;
    float    metal, energy;
    uint32_t seen;
} IMPAI_FMEM;

/* One seat's standing state between decisions. */
/* the opening's far patch lies within FAR_PATCH_K x comm_r of the anchor (impai_build.c, THE OPENING'S FAR PATCH),
   the one commander site past comm_r, and the military's leash allows the commander as far before the first
   factory (impai_mil.c, THE LEASH) */
#define FAR_PATCH_K 2

/* THE MEND'S SERVICE: what each economy decision offers the military's mend, read by the next military decisions
   through IMPAI_MIL_IN.serve (impai_mil.c, THE MEND's gate and THE HAVEN). The base box (impai_brain.c, base_box),
   and the repair as it met the decision (impai_build.c, THE REPAIR): the repairers it could take or had serving,
   the economy's attention room and the energy stall. Written after the census -- none, before the base box and the
   repair fill it -- so a decision with no unit offers nothing. Recorded with IMPAI_BRAIN, and data to the
   military: the box is held to the map before a point is built from it, every count is only compared. */
typedef struct IMPAI_SERVE {
    int16_t  bx0, bz0, bx1, bz1;  /* the base box, cells, inclusive; bx1 < bx0 or bz1 < bz0: none               */
    uint8_t  repairers;           /* repairers idle or helping that the repair could take, and those repairing,
                                     held to 255                                                               */
    uint8_t  room;                /* 1: the economy's attention had room for one more busy builder              */
    uint8_t  e_stall;             /* 1: the energy stall, in which a repair adds no hit point                    */
    uint8_t  pad;
} IMPAI_SERVE;

typedef struct IMPAI_BRAIN {
    int16_t    ax, az;          /* the base anchor's cell; IMPAI_NO_ANCHOR before the first decision      */
    uint8_t    ring_grow;       /* how far the base ring has been pushed out, 0..8                        */
    uint8_t    nfail;
    uint16_t   fail_next;
    uint16_t   explorer;        /* the slot sent exploring; WQ_NONE                                        */
    uint8_t    sea_refused;     /* the sea plant's counted refusals this level -- a decision whose search
                                   placed none, a failed water-factory job -- at most one a decision and
                                   one a fail_t; at IMPAI_SEA_REFUSALS it is given up (impai_build.c, THE
                                   SEA PLANT)                                                              */
    uint8_t    sea_none_far;    /* 1 when that search reached THE FINISH AT SEA's radius (impai_build.c)    */
    uint32_t   sea_counted_at;  /* the GameTime of the last counted refusal                                */
    uint16_t   sea_none_type;   /* the sea plant type whose search found no candidate at all, for the       */
    int16_t    sea_none_ax, sea_none_az, sea_none_ex, sea_none_ez;  /* anchor and estimate blocks it was
                                   searched for; WQ_NONE for none. The candidates are content, these and the
                                   radius (impai_build.c), so the answer holds while they do               */
    IMPAI_FAIL fail[IMPAI_FAILS];   /* a ring of failed sites and explore cells, each until a GameTime    */
    IMPAI_WAYREC way[IMPAI_WAYS];  /* the plants' kept ways out, the first nway used                     */
    uint8_t    nway;
    uint8_t    nwfail, wfail_next;
    uint8_t    opened;          /* 1 once a census has counted a factory of the seat's, frames included: the
                                   opening is over for the level (impai_build.c, THE OPENING'S FAR PATCH),
                                   whose commander may walk FAR_PATCH_K x comm_r from the anchor           */
    IMPAI_WAYFAIL wfail[IMPAI_WAY_FAILS];   /* a ring of the plant sites the way out refused, apart from `fail`
                                               so no number of them can push a failed site out of it        */
    /* THE SEA SEARCH'S CURSOR (impai_build.c, THE SEARCH): the next candidate the sea plant's search examines,
       ring sea_r index sea_i, for the type and the estimate's block it walks for, from GameTime sea_from; type
       WQ_NONE: none, the walk starts at the first */
    uint16_t   sea_cur_type;
    int16_t    sea_cur_ex, sea_cur_ez;
    uint16_t   sea_r, sea_i;
    uint32_t   sea_from;
    uint8_t    nfmem;
    uint16_t   fscan;           /* the slot of the builder the last decision looked around; WQ_NONE      */
    uint16_t   ascan;           /* the slot of the army unit the last decision looked around; WQ_NONE    */
    IMPAI_FMEM fmem[IMPAI_FEATS];  /* the features the seat remembers, the first nfmem used, in the order
                                   first seen                                                             */
    uint32_t   com_until;       /* THE COMMANDER'S MARKERS (impai_brain.c, impai_give): the economy's order
                                   to the commander is pending until this GameTime                        */
    IMPAI_SERVE serve;          /* THE MEND'S SERVICE, the last decision's                                 */
    uint16_t   unlock;          /* THE KEPT UNLOCK (impai_econ.c): the constructor type the plant choice last
                                   waited for, 0 none, and unlock_plant the plant it waited to build       */
    uint16_t   unlock_plant;
    uint8_t    held[IMPAI_GRID_MAX * IMPAI_GRID_MAX / 8];  /* THE HELD GROUND (impai_build.c): bit gx & 7 of byte
                                   (gz IMPAI_GRID_MAX + gx) >> 3 is 1 while 256-px cell (gx, gz) is held, as the
                                   last decision that judged it left it; all 0 at the level's start          */
} IMPAI_BRAIN;

/* THE PLAN: every constant a decision decides by, what it limits beside it. One row, IMPAI_PLAN_ROW
   (impai_brain.c), which every level plays: a level changes how the decisions are carried out, its persona
   (IMPAI_PERSONA below), never what they decide. */
enum { IMPAI_LV_EASY, IMPAI_LV_NORMAL, IMPAI_LV_HARD, IMPAI_LVS };
typedef struct IMPAI_PLAN {
    uint8_t  open_mex, open_energy;  /* the first factory once this many extractors and energy buildings..  */
    uint16_t open_t;            /* ...or from this GameTime                                               */
    uint8_t  factory_max;       /* factories built in all                                                 */
    float    factory_per_m;     /* another factory per this metal income a pass                           */
    float    e_per_m;           /* the energy income kept per metal income                                */
    uint8_t  cons_open, cons_max;    /* constructors the governor queues free of its rules; the most it keeps
                                        (impai_econ.c, THE GOVERNOR)                                        */
    uint8_t  army_per_cons;     /* past cons_open, army units each further constructor needs (THE ARMY'S SHARE) */
    float    press_k;           /* the military stamps pressure while the seat's army is worth less than this
                                   x the pressing value (impai_mil.c, THE PRESSURE)                        */
    uint16_t press_hold;        /* ticks a pressure stamp holds the constructors                          */
    uint16_t stall_pm;          /* the store, in thousandths of its storage, below which a stall begins
                                   (m_stall, e_stall) and THE HOLD holds what is not economy (impai_brain.c,
                                   economy); below SURPLUS_FRAC x 1000                                     */
    uint8_t  hold_fac;          /* the factories busy at most in a hold (impai_econ.c, THE GOVERNOR); at least
                                   1, at most IMPAI_IDLE_CAP                                               */
    uint16_t comm_r, base_r;    /* px from the anchor: the commander's reach; the base box's               */
    uint8_t  gap;               /* cells between two base buildings                                       */
    uint16_t job_timeout;       /* ticks a build or an exploration may take before it is given up         */
    uint16_t fail_t;            /* ticks a failed site or cell is avoided                                 */
    uint8_t  idle_max;          /* idle builders a decision considers (<= IMPAI_IDLE_CAP)                 */
    uint8_t  assign_max;        /* builders a decision tasks                                              */
    uint8_t  site_tests;        /* site tests a decision makes                                            */
    uint8_t  snaps;             /* snaps onto a metal patch a decision makes                              */
    uint8_t  queue_max;         /* factory queue commands a decision gives                                */
    uint8_t  roots;             /* commands a decision gives in all                                       */
    uint8_t  job_checks;        /* jobs a decision checks                                                 */
    /* the military (impai_mil.c) */
    uint8_t  mil_roots;         /* orders a military decision gives, at most IMPAI_MIL_ROOTS_MAX          */
    uint8_t  react_confirm;     /* decisions a base threat persists before a squad answers it             */
    uint8_t  predict_max;       /* predictions a military decision makes, at most IMPAI_PREDICT_CAP       */
    uint8_t  aa_struct_max;     /* anti-air structures the base ring builds at most                       */
    float    micro_hp;          /* a fighting member under this share of its health steps back; 0 never   */
    uint16_t micro_period;      /* ticks between two steps back of one member                             */
    uint16_t rally_px;          /* the rally point: the anchor moved this far towards the enemy base      */
    float    attack_margin;     /* the predicted margin a wave needs                                      */
    float    retreat_margin;    /* below it a squad pulls back                                            */
    float    wave_value;        /* a wave's value, in v (its layer's median army unit's value); +1 v each
                                   wave of the layer since its last wave destroyed its target            */
    uint16_t scout_period;      /* ticks between scouting runs                                            */
    uint16_t task_timeout;      /* ticks a raid, defence, retreat or scout may last at its point, past its
                                   travel there (impai_mil.c, THE TASK'S LIFETIME); an attack has none
                                   (impai_mil.c, move_on)                                                 */
    float    aa_struct_v;       /* seen enemy air value for each anti-air structure                       */
    float    dgun_reserve;      /* the D-gun fires only while stored energy is at least the shot plus this
                                   share of the seat's energy storage                                       */
    uint8_t  def_max;           /* ground defences the base ring builds at most, against THE PRESSURE at the
                                   base (impai_econ.c, THE GROUND DEFENCES); at most 4                      */
    float    def_v;             /* the pressing value for each ground defence past the first              */
    /* THE MEND (impai_mil.c) and THE REPAIR (impai_build.c) */
    float    mend_hp;           /* an army unit below this share of its health mends; 0 never              */
    uint16_t mend_wait;         /* ticks a mender's health may stay unchanged at its haven before its mend
                                   ends; at least the longest look_mil, 150                                */
    uint8_t  mend_roots;        /* orders the mend gives a decision, at most IMPAI_MEND_ROOTS_MAX and with
                                   mend_roots + 2 <= mil_roots: the commander's two                        */
    uint8_t  repair_max;        /* constructors on repairs at once, at most assign_max                     */
} IMPAI_PLAN;
extern const IMPAI_PLAN IMPAI_PLAN_ROW;
#define IMPAI_MIL_ROOTS_MAX 48          /* the plan's mil_roots at most: impai_mil_decide refuses a plan past it */

/* THE PERSONA: how a level carries out the plan's decisions (impai_react.c), one row a level in IMPAI_PERSONAS,
   or a seat's own from the persona lever (tagpu_impai.c), kept in IMPAI_MIL.persona. Its looks are how often
   each manager decides, its reaction the ticks from a decision to its orders, its budget each manager's actions
   a minute and the most it saves up, its attention the builders the economy keeps busy and the operations the
   military holds at once. Every rule is impai_persona_ok's, its reason beside it. 16 bytes, recorded inside
   IMPAI_MIL. */
typedef struct IMPAI_PERSONA {
    uint16_t look_eco;              /* ticks between economy decisions: 15..150, a multiple of 15                */
    uint16_t look_mil;              /* ticks between military decisions: 15..150, a multiple of 15               */
    uint16_t react;                 /* ticks from a decision to its orders: 0..min(look_eco, look_mil) - 15      */
    uint16_t apm_eco, apm_mil;      /* each manager's actions a minute (1800 ticks), 1..600; 0 = no budget       */
    uint8_t  burst_eco, burst_mil;  /* each bucket's size in actions: 1..60 with a rate, 0 without              */
    uint8_t  att_eco;               /* builders busy at once the economy keeps: 1..IMPAI_IDLE_CAP; 0 = no limit  */
    uint8_t  att_mil;               /* operations (RAID, ATTACK; DEFEND exempt) at once: 1..IMPAI_SQUADS; 0 = none */
    uint8_t  pad[2];                /* 0                                                                         */
} IMPAI_PERSONA;
_Static_assert(sizeof(IMPAI_PERSONA) == 16, "IMPAI_PERSONA is recorded inside IMPAI_MIL");
/* Easy's, Normal's and Hard's rows: Hard's is no limit at all, so Hard plays the plan exactly as it decides it */
extern const IMPAI_PERSONA IMPAI_PERSONAS[IMPAI_LVS];
/* 1 when every rule of IMPAI_PERSONA holds (impai_react.c, the reason of each) */
int impai_persona_ok(const IMPAI_PERSONA* p);
/* 1 while a commander marker (IMPAI_BRAIN.com_until, IMPAI_MIL.com_until) holds at `now`: after now and no later
   than now + react, so a recorded marker past the reaction holds nothing (impai_react.c) */
int impai_com_marked(uint32_t until, uint32_t now, const IMPAI_PERSONA* p);
/* 1 while the military's window on the commander (IMPAI_MIL.safe_until) holds at `now`: after now and no later
   than now + the window's length, so a recorded window past it holds nothing (impai_mil.c, THE FLIGHT) */
int impai_com_safe(uint32_t until, uint32_t now);

/* The reach labels of the level's movement classes (impai_reach_label's output, class after class, bw x bh
   blocks each, class c at labels + at[c] when present has bit c), the blocks' heights and the summary the
   naval rule reads (impai_reach_summary). A sea army type is one of klass IMPAI_K_SRAIDER..IMPAI_K_SAA, a land
   army type a mobile type that is not a VTOL of klass IMPAI_K_LRAIDER..IMPAI_K_LSCOUT. */
typedef struct IMPAI_REACH {
    const uint16_t* labels;
    uint32_t        at[WQ_MCLASSES];
    uint32_t        present;
    uint16_t        bw, bh;
    const uint8_t*  top;                /* bw x bh: each block's highest 2x2 maximum, 0..255; NULL: none        */
    uint32_t        sea_blocks;         /* blocks where some sea army class has a label (impai_reach_summary)  */
    uint32_t        land_total;         /* the content's land army types on labelled classes                   */
    uint16_t        land_w[WQ_MCLASSES];    /* of them, class c's                                              */
    uint8_t         sea_ref;            /* the class most sea army types use, ties to the lower; 0xFF none     */
} IMPAI_REACH;

/* a cell's height, the engine's or a fake's: 0..255, 255 where it has none */
typedef int (*IMPAI_TOPFN)(const void* ctx, int cx, int cz);
/* top[bz x (w / 2) + bx]: the most of fn over the block's four cells, each held to 0..255; (w/2)(h/2) blocks,
   four calls each */
void impai_reach_top(IMPAI_TOPFN fn, const void* ctx, int w, int h, uint8_t* top);
/* THE RING ORDER: the i-th block of ring r (Chebyshev distance r) around a block, into (dx, dz); 0 past its
   8r blocks (ring 0 is the block alone). Ordered by k, the offset along the side, ascending -- so by squared
   distance r^2 + k^2 -- and within one k by (dz, dx). */
int  impai_ring_at(int r, int i, int* dx, int* dz);
/* the label of class c at block (bx, bz): -1 when the level has no labels for c, 0 off the grid */
int  impai_reach_at(const IMPAI_REACH* r, unsigned c, int bx, int bz);
/* the first block in ring order within `radius` rings of (bx, bz) whose class-c label is `label`, into (ox,
   oz): 1, or 0 for none. Off-grid blocks are skipped. BOUND: at most (2 radius + 1)^2 label reads */
int  impai_reach_near(const IMPAI_REACH* r, unsigned c, uint16_t label, int bx, int bz, int radius,
                      int* ox, int* oz);
/* sea_blocks, land_total, land_w and sea_ref from the type information ti[1..nti) over the classes the level
   has labels for. BOUND: nti types, and each sea class's labels read once, at most WQ_MCLASSES x bw x bh */
void impai_reach_summary(IMPAI_REACH* r, const IMPAI_TINFO* ti, unsigned nti);
/* more than half of the level's blocks carry a label of some sea army class */
int  impai_mostly_water(const IMPAI_REACH* r);
/* 1 when blocks a and b are LAND-LINKED: the classes whose labels agree there, both nonzero, hold at least half
   of land_total; 1 too with no evidence (land_total 0, a block off the grid) */
int  impai_land_link(const IMPAI_REACH* r, int ax, int az, int bx, int bz);
/* the distinct nonzero sea_ref labels within `radius` rings of (bx, bz), nearest ring first, at most cap, into
   out: their count. BOUND: at most (2 radius + 1)^2 label reads */
int  impai_sea_labels(const IMPAI_REACH* r, int bx, int bz, int radius, uint16_t* out, int cap);

typedef struct IMPAI_BRAIN_IN {
    const WORLDQ*      q;               /* the seat's instance, WQ_SITE_SEAT in the engine                    */
    IMPAI_BRAIN*       st;
    IMPAI_JOB*         job;  unsigned njob;     /* slot-indexed, njob >= nslots                              */
    const IMPAI_TINFO* ti;   unsigned nti;      /* type-indexed, nti == ndefs                                */
    const SNAP_PATCH*  spots; unsigned nspots;  /* the seat's published metal patches                       */
    const IMPAI_REACH* reach;           /* NULL: no reach test                                                */
    const IMPAI_VIEW*  view;            /* the threat map's input; NULL or no threat table: no threat test    */
    const IMPAI_PLAN*  plan;
    const IMPAI_PERSONA* persona;       /* the seat's, validated by the caller (impai_persona_ok)            */
    int                acts;            /* the economy bucket's actions (impai_exec_acts); -1 no budget      */
    void*              scratch; size_t scratch_bytes;   /* impai_brain_scratch_bytes(), the caller's        */
    SNAP_SCRATCH*      snap;
    const struct IMPAI_MIL* mil;        /* the seat's military: the commander's window, the table and picture
                                           the factories buy by; NULL: none of them                         */
    const IMPAI_PREDICTOR* pred;        /* the pair rate the type choice weighs (impai_fight_rate_of); NULL,
                                           or no military: the factories buy no army                        */
    IMPAI_PLANTREF*    pref; unsigned pref_stride;  /* the seat's plant refusals, type t's at
                                                       pref[t x pref_stride], nti of them                   */
    const IMPAI_TVALUE* tv;             /* the values, nti rows (impai_tinfo_values); NULL: every plant scores
                                           alike and the choice is the least value, the factories ungated     */
} IMPAI_BRAIN_IN;

/* ---- the military's standing state (impai_mil.c; research/notes/impai/brain.md, The military) ---- */

#define IMPAI_SQUADS   16
#define IMPAI_ATTACKS_MAX 4             /* ATTACK squads at once (impai_mil.c, become)                           */
#define IMPAI_RAIDS_MAX   2             /* RAID squads at once                                                   */
#define IMPAI_CONTACTS 32
enum { IMPAI_SQ_FREE, IMPAI_SQ_FORM, IMPAI_SQ_DEFEND, IMPAI_SQ_RAID, IMPAI_SQ_ATTACK, IMPAI_SQ_RETREAT,
       IMPAI_SQ_SCOUT, IMPAI_SQ_TASKS };

typedef struct IMPAI_SQUAD {          /* 20 bytes                                                              */
    uint8_t  task, layer, n, flags;   /* IMPAI_SQ_*; TASIM_LAYER; members at the last decision; a scout's leg,
                                         an ATTACK's 1 when its target is a structure (impai_mil.c, aim)      */
    int16_t  tx, tz;                  /* the task's point, 32-px block                                         */
    uint16_t target;                  /* the unit slot it goes for, WQ_NONE                                    */
    uint16_t seen;                    /* decisions the task's contact has persisted (react_confirm)            */
    uint32_t since;                   /* GameTime the task began                                               */
    float    value;                   /* its members' value at the last decision                               */
} IMPAI_SQUAD;

/* THE HUNT'S MEMORY (impai_mil.c, wave_goal): each foe's commander as the seat last saw it, and the map in 16 x 16
   sweep cells with the time an army unit of the seat last stood in each */
#define IMPAI_SWEEP_SIDE 16
#define IMPAI_SWEEP      (IMPAI_SWEEP_SIDE * IMPAI_SWEEP_SIDE)
#define IMPAI_DRY_T      5400           /* THE DRY HUNT: ticks with no land-linked enemy known (impai_mil.c)   */
#define IMPAI_FOE_FRESH  5400           /* ticks a commander's last seen block stays a goal [hand-written: the
                                           bar's to move]                                                        */
typedef struct IMPAI_FOE {            /* 12 bytes                                                              */
    uint16_t slot;                    /* its unit slot; WQ_NONE: never seen                                    */
    int16_t  bx, bz;                  /* its 32-px block when last seen                                        */
    uint16_t pad;
    uint32_t t;                       /* the GameTime it was last seen                                         */
} IMPAI_FOE;

/* THE REACTION'S QUEUE (impai_react.c): the orders a seat's decisions gave, each kept until its due tick, the
   decision's GameTime + the persona's react. At most one batch of each manager is pending (impai_react.c, THE
   BOUND), so IMPAI_PEND_MAX is one economy decision's commands and one military decision's. Recorded inside
   IMPAI_MIL, slots and types as IDs (tagpu_impai.c, s_mil_layout). */
enum { IMPAI_SRC_ECO, IMPAI_SRC_MIL, IMPAI_SRCS };
#define IMPAI_PEND_MAX (IMPAI_ROOTS_MAX + IMPAI_MIL_ROOTS_MAX)     /* 80: one batch of each manager */
typedef struct IMPAI_PEND {          /* one order a decision gave, kept until its due tick                 */
    uint32_t due;                    /* GameTime it is given at: its decision's + react                    */
    uint32_t birth;                  /* the actor's incarnation at the decision (WQ_OBS.birth)             */
    uint32_t tbirth;                 /* the target's, when it names one                                    */
    int32_t  pos[3];                 /* 16.16, as WQ_CMD                                                   */
    uint16_t unit, target, type;     /* slots (target WQ_NONE = none) and the build type                  */
    uint8_t  verb, action, shift, count;   /* WQ_CMD_NEW or WQ_CMD_QUEUE; count 1..WQ_QUEUE_MAX for QUEUE,
                                              0 for NEW                                                    */
    uint8_t  src, inc;               /* IMPAI_SRC_*, WQ_INC_*                                              */
} IMPAI_PEND;
_Static_assert(sizeof(IMPAI_PEND) == 36, "IMPAI_PEND is 36 bytes with no padding");
typedef struct IMPAI_EXEC {
    int32_t    tok[IMPAI_SRCS];      /* each manager's bucket, 1/1800 of an action a unit: 0..burst x 1800  */
    uint32_t   filled[IMPAI_SRCS];   /* GameTime of each bucket's last refill                               */
    uint16_t   npend, pad;           /* entries pend[0..npend) in decision order; npend <= IMPAI_PEND_MAX   */
    IMPAI_PEND pend[IMPAI_PEND_MAX]; /* entries past npend are zero                                         */
} IMPAI_EXEC;
_Static_assert(sizeof(IMPAI_EXEC) == 2900, "IMPAI_EXEC is 2900 bytes");

/* One seat's military. THE TABLE is how its army is bought (impai_table.h), set at the level init and kept
   here, so a replay or a takeover buys as the game did whatever lever chose it. THE PICTURE is what the seat
   had seen of the enemy at its last military decision, which its factories buy against. */
typedef struct IMPAI_MIL {
    uint8_t  level, next_squad;       /* IMPAI_LV_*; the round-robin's start                                   */
    uint8_t  table_lever, scout_next; /* 1: the table came from tagpu_impai_table.on; the next scouting point  */
    int16_t  ebx, ebz;                /* the enemy base's estimate, 32-px block; IMPAI_NO_ANCHOR               */
    uint32_t scout_t, wave_t;         /* GameTime of the last scouting run, of the last attack wave            */
    uint32_t safe_until;              /* the commander is the military's until this GameTime, read through
                                         impai_com_safe                                                        */
    uint32_t com_until;               /* the military's order to the commander is pending until this GameTime
                                         (impai_brain.c, THE COMMANDER'S MARKERS)                              */
    uint32_t pulse_roots;             /* military roots given: the pulse line's count                          */
    uint16_t waves[TASIM_LAYERS];     /* each layer's waves since its last wave destroyed its target, held to
                                         65 535 (impai_mil.c, THE WAVE): recorded data, a count               */
    int8_t   naval;                   /* the naval judgement of the last military decision (impai_mil.c,
                                         naval): 1 the map is mostly water or the estimate is off the anchor's
                                         land component, so the seat's next factory after its first is a sea
                                         plant (impai_brain.c, economy); 0 not; -1 not judged yet, which holds
                                         the second factory                                                    */
    uint8_t  est_seen;                /* 1: the estimate rests on enemy structures seen or remembered; 0: it is a
                                         start position's guess (impai_mil.c, THE ENEMY BASE'S ESTIMATE)       */
    float    air_seen;                /* the seen enemy air value: the anti-air structures' rule               */
    IMPAI_TABLE   table;
    IMPAI_PICTURE pic;
    IMPAI_SQUAD   sq[IMPAI_SQUADS];
    /* each seat's commander as this seat last saw it: written only from the seat's own column, at a row in its
       own line of sight now (IMPAI_LOS, never an ally's IMPAI_SHARED position) -- nothing reads where the
       commander is otherwise, so this is no cheating -- and
       forgotten by the sweep (an army unit of the seat standing in its cell since) or after IMPAI_FOE_FRESH,
       never by a read of the other seat */
    IMPAI_FOE     foe[WQ_SEATS];
    /* the GameTime / 64 at which an army unit of the seat last stood in each 16 x 16 sweep cell of the map, 0
       never: 16 bits hold 65 535 x 64 ticks, 38 game hours at 30 ticks a second, so it never wraps in a game,
       and a later time is held at 65 535 */
    uint16_t      swept[IMPAI_SWEEP];
    /* THE PRESSURE (impai_mil.c): the last military decision that found the seat outmatched near its base or its
       structures stamps it; the economy holds its constructors while it lasts (impai_econ.c, THE GOVERNOR) and
       builds ground defences against it (impai_econ.c, THE GROUND DEFENCES). Seat-sight data only. */
    uint32_t      press_until;        /* GameTime + press_hold at the stamp; 0 never                           */
    float         press_v;            /* the pressing value at the stamp                                       */
    int16_t       press_bx, press_bz; /* the pressing member nearest the anchor, 32-px block                   */
    uint8_t       press_base;         /* 1 when that member threatens the base (THE BASE'S THREATS)            */
    uint8_t       threat_seen;        /* military decisions in a row with a base threat standing, held to 255: THE
                                         RECALL's confirmation (impai_mil.c). Recorded data, only compared        */
    uint16_t      radar_as;           /* the type the last military decision counted a contact never identified
                                         as (impai_mil.c, substitute), WQ_NONE none; recorded data, so its readers
                                         hold it below their type count                                       */
    /* THE DRY HUNT (impai_mil.c, dry_hunt): the last military decision that knew an enemy on a block land-linked to
       the anchor's, 0 never -- recorded data, held to now --, and 1 in `dry` while none has been known for
       IMPAI_DRY_T ticks since: the economy then builds the finish at sea (impai_brain.c, economy) */
    uint32_t      land_t;
    uint8_t       dry;
    /* the seat's persona, set with the level and kept for the seat's life, so a replay or a takeover carries
       out the decisions as the game did whatever lever chose it. Recorded data: the glue reads it only through
       impai_persona_ok (tagpu_impai.c, seat_persona) */
    IMPAI_PERSONA persona;
    IMPAI_EXEC    exec;               /* the reaction's queue and the budgets (impai_react.c)                   */
} IMPAI_MIL;

/* a seat's military at a level's start: no squad, no estimate; the level, the persona and the table given */
void  impai_mil_reset(IMPAI_MIL* m, unsigned level, const IMPAI_PERSONA* persona, const IMPAI_TABLE* table,
                      int from_lever);

/* What one decision did, against the plan's bounds, and the census's counts. */
typedef struct IMPAI_BRAIN_OUT {
    uint16_t census, sites, snaps, roots, queued, failed, orders;
    uint16_t count[IMPAI_CATS];
    uint16_t army;                      /* army units queued                                                     */
    uint16_t sea_plants;                /* sea plant orders given                                                */
    uint16_t waived;                    /* a plant ordered without a way out (impai_build.c, THE PLANT'S
                                           FALLBACK): its type, at first cell (waived_cx, waived_cz); 0 none      */
    int16_t  waived_cx, waived_cz;
    uint16_t reclaims;                  /* reclaim orders given                                                  */
    float    reclaim_metal;             /* the metal of the features the reclaim orders named                   */
    uint16_t fidle;                     /* idle factories left without an order beside a store in surplus
                                           (impai_econ.c, THE GOVERNOR)                                         */
    uint8_t  pressed;                   /* 1 when the military's pressure held the constructors                 */
    /* THE CHOICE (impai_econ.c): the plant ordered this decision, 0 none, and the runner-up; their scores
       (IMPAI_Q_NONE for none); the want it answered (1 the first plant, 2 a factory, 3 a tech-up); the tech level
       the scores were read at; and the reach mask the choice used (bit c: movement class c reaches a plausible
       enemy base, impai_econ.c THE REACH) */
    uint16_t plant, plant_next;
    float    plant_s, plant_next_s;
    uint8_t  plant_want, plant_T;
    uint16_t poor;                      /* idle factories THE ARMY ORDERS gated out as poor, left without an order */
    uint32_t plant_reach;
    uint16_t unlock, unlock_plant;      /* THE UNLOCKS (impai_econ.c): the constructor a factory was given this
                                           decision because the plant the choice wants needs it, and that plant;
                                           0 none                                                              */
    uint16_t busy;                      /* builders busy after the decision: the census's busy ones and the idle
                                           ones it assigned (impai_econ.c, THE ATTENTION)                       */
    uint16_t sea_type;                  /* the decision's sea plant search (impai_build.c, THE SEARCH): the type
                                           searched, WQ_NONE when none was, and its answer -- 1 a site, 0 none
                                           this decision, -1 no candidate at all -- for the second-factory line  */
    int8_t   sea_answer;
    uint8_t  hold;                      /* 1 when THE HOLD held this decision (impai_brain.c, economy)          */
    uint16_t held;                      /* idle factories THE HOLD left without an order (THE GOVERNOR)          */
    uint16_t released;                  /* helpers THE HOLD took off work that is not economy (impai_econ.c,
                                           THE RELEASE)                                                         */
    uint16_t repairs;                   /* repair orders given (impai_build.c, THE REPAIR)                     */
    uint16_t yard;                      /* builders moved off a waiting factory's yard or way (impai_brain.c, THE
                                           YARD)                                                                */
    uint16_t yard_left;                 /* builders on one walking off it already, given nothing (THE WALK OFF)  */
    uint16_t help_refused;              /* helps the engine ended before their frame was finished (THE HELP
                                           REFUSED, impai_brain.c)                                              */
    uint16_t makers_off, makers_on;     /* makers switched off and on (impai_econ.c, THE MAKERS)                */
} IMPAI_BRAIN_OUT;

/* 1 when seat `seat` decides at `gametime`: GameTime % look == 7 x seat % look, its persona's look_eco. Every look
   is a multiple of 15 (impai_persona_ok), so a seat decides only on ticks of 7 x seat (mod 15): the ten seats'
   phases are 0, 7, 14, 6, 13, 5, 12, 4, 11, 3, and no two seats' economies share a tick whatever their looks. */
int    impai_brain_due(unsigned gametime, unsigned seat, unsigned look);
/* the scratch one decision takes: its census and lists, three build menus, the threat map's arrays, the way-out
   search's content map and blocker raster. At most IMPAI_BRAIN_SCRATCH_MAX, which impai_brain.c asserts at compile
   time, so a caller may hold it statically. */
#define IMPAI_BRAIN_SCRATCH_MAX (768u * 1024u)
size_t impai_brain_scratch_bytes(void);
/* the decision's state at a level's start: no anchor, no failure, no explorer, every job none */
void   impai_brain_reset(IMPAI_BRAIN* st, IMPAI_JOB* job, unsigned njob);
/* One decision: the census, the job checks, the economy, the builders, the factories. 0, or -1 when the input
   is refused (a table short of the world, the scratch short), with nothing written. */
int    impai_brain_decide(const IMPAI_BRAIN_IN* in, IMPAI_BRAIN_OUT* out);

#endif
