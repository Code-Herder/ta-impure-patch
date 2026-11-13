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
   given. Integers only, but for impai_threat_from_roles's one rounding of the
   classifier's figures; no clock, no random generator. */
#include <stddef.h>
#include <stdint.h>
#include "worldq.h"
#include "tasim_snap.h"
#include "tasim_roles.h"
#include "impai_table.h"

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
typedef struct IMPAI_ROW { IMPAI_SEEN seat[WQ_SEATS]; } IMPAI_ROW;     /* keyed by unit slot            */

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
   rounded once and held to its field; its TASIM_LAYER and TASIM_RF_* flags' low byte. 16 bytes. */
typedef struct IMPAI_THREAT {
    uint16_t dps_ground, dps_air, dps_water;
    uint16_t range;
    uint32_t value;
    uint8_t  layer, flags;
    uint16_t pad;
} IMPAI_THREAT;
void impai_threat_from_roles(const TASIM_ROLE_OUT* out, int n, IMPAI_THREAT* t);

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
   range of its block's centre; influence, own rows' value minus identified enemies', summed over the 3 x 3
   cells around each cell; presence, the enemy rows known only as radar contacts or ghosts. */
typedef struct IMPAI_MAPS {
    int       gw, gh, threat_built;
    uint32_t* threat[IMPAI_THREATS];
    int32_t*  influence;
    uint16_t* presence;
    int32_t*  diff;             /* scratch, (IMPAI_GRID_MAX + 1) x IMPAI_GRID_MAX                          */
} IMPAI_MAPS;
int impai_maps_build(const IMPAI_VIEW* v, IMPAI_MAPS* m);

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

/* One type, 28 bytes. The gains are per 30-tick economy pass on this map. */
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
    uint16_t dgun_range;        /* its D-gun: the range px of its longest CommandFire weapon when the def can
                                   take the order (+0x245 bit 14), 0 none                                       */
    uint16_t dgun_e;            /* that weapon's energy a shot (+0xC0), held to 0..65535                       */
} IMPAI_TINFO;

/* Every type of q's world into ti[0..ndefs), ti[0] none: from its def, its economy keys, the map's wind and
   tide and the role classifier's answer for it (roles[t] for type t; with roles NULL or t >= nroles there is
   none, so the type is no commander, factory or constructor). Returns ndefs, or -1 when nti is short of it
   (nothing promised). */
int impai_tinfo_build(const WORLDQ* q, const TASIM_ROLE_OUT* roles, int nroles, IMPAI_TINFO* ti, int nti);
/* how many types of each category ti[0..n) holds */
void impai_tinfo_count(const IMPAI_TINFO* ti, int n, unsigned count[IMPAI_CATS]);

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

/* ---- the brain: one seat's economy, a decision every `decide` ticks (impai_brain.c, impai_econ.c,
   impai_build.c; research/notes/impai/brain.md) ----

   WHAT IT READS. The WORLDQ (a WQ_SITE_SEAT view: the seat's own site test), its own state below, the type
   table, the seat's published metal patches, its reach labels and its view for the threat map, and the level
   row. It keeps unit slots and type numbers only, names a unit by slot in every command, and draws no random
   number and reads no clock: every choice is broken by a slot, a type number or a cell, and the census and the
   choices go in slot order, so the same world gives the same orders.

   WHAT BOUNDS IT. Every loop is bounded by a count: the seat's slot block, or a field of the level row. */

#define IMPAI_FAILS      32
#define IMPAI_SEA_REFUSALS 16           /* a seat's counted decisions with no sea plant before it is given up */
#define IMPAI_NO_ANCHOR  (-32768)
#define IMPAI_STRUCT_MAX 512            /* the census's buildings, the job checks' search                     */
#define IMPAI_NANO_MAX   32             /* unfinished buildings an assist may pick from                       */
#define IMPAI_CLAIM_MAX  128            /* sites held by a build order, an extractor or a plant              */
#define IMPAI_IDLE_CAP   32             /* the idle builders, helpers and idle factories a census lists       */

enum { IMPAI_JOB_NONE, IMPAI_JOB_BUILD, IMPAI_JOB_ASSIST, IMPAI_JOB_EXPLORE, IMPAI_JOB_SQUAD };

/* THE JOB TABLE, slot-keyed: the task ImpAI gave the unit in this slot, 16 bytes. A job holds only while the
   slot holds a live unit of the seat's of `type`: the census voids it otherwise. A slot reused by a unit of the
   same type inherits it, which costs at most one misjudged failure (a site avoided for fail_t): the unit's
   UnitInGameIndex is its slot again (+0xA8, written once a level at 0x485603), so nothing tells the two apart. */
typedef struct IMPAI_JOB {
    uint16_t type;              /* the unit's type when tasked; WQ_NONE: no job                           */
    uint16_t build;             /* the type ordered; WQ_NONE                                               */
    uint16_t target;            /* an assist's target slot; WQ_NONE                                        */
    int16_t  cx, cz;            /* the site's first cell, or the cell explored                            */
    uint8_t  kind, squad;       /* IMPAI_JOB_*; IMPAI_JOB_SQUAD: its squad, below IMPAI_SQUADS            */
    uint32_t since;             /* GameTime of the order; IMPAI_JOB_SQUAD: of its last step back          */
} IMPAI_JOB;

typedef struct IMPAI_FAIL { int16_t cx, cz; uint32_t until; } IMPAI_FAIL;

/* One seat's standing state between decisions. */
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
    uint8_t    pad;
    uint32_t   sea_counted_at;  /* the GameTime of the last counted refusal                                */
    uint16_t   sea_none_type;   /* the sea plant type whose search found no candidate at all, for the       */
    int16_t    sea_none_ax, sea_none_az, sea_none_ex, sea_none_ez;  /* anchor and estimate blocks it was
                                   searched for; WQ_NONE for none. The candidates are content and these
                                   (impai_build.c), so the answer holds while they do                      */
    IMPAI_FAIL fail[IMPAI_FAILS];   /* a ring of failed sites and explore cells, each until a GameTime    */
} IMPAI_BRAIN;

/* THE LEVEL ROW: every constant a decision uses, what it limits beside it. The three levels are rows of one
   table, IMPAI_LEVELS (impai_brain.c), chosen per seat at the level init (IMPAI_MIL.level). */
enum { IMPAI_LV_EASY, IMPAI_LV_NORMAL, IMPAI_LV_HARD, IMPAI_LVS };
typedef struct IMPAI_LEVEL {
    uint16_t decide;            /* ticks between two decisions of a seat                                  */
    uint8_t  open_mex, open_energy;  /* the first factory once this many extractors and energy buildings..  */
    uint16_t open_t;            /* ...or from this GameTime                                               */
    uint8_t  factory_max;       /* factories built in all                                                 */
    float    factory_per_m;     /* another factory per this metal income a pass                           */
    float    e_per_m;           /* the energy income kept per metal income                                */
    uint8_t  cons_base, cons_max;    /* constructors the factories keep: base, at most..                  */
    float    cons_per_m;        /* ...and one more per this metal income a pass                           */
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
    uint16_t mil_period;        /* ticks between military decisions, the reaction delay: a multiple of 15  */
    uint8_t  mil_roots;         /* orders a military decision gives                                       */
    uint8_t  react_confirm;     /* decisions a base threat persists before a squad answers it             */
    uint8_t  predict_max;       /* predictions a military decision makes, at most IMPAI_PREDICT_CAP       */
    uint8_t  focus;             /* 1: a fighting squad fires on the quickest kill, 0: each on its nearest  */
    uint8_t  raids;             /* raid squads at once                                                    */
    uint8_t  aa_struct_max;     /* anti-air structures the base ring builds at most                       */
    float    micro_hp;          /* a fighting member under this share of its health steps back; 0 never   */
    uint16_t micro_period;      /* ticks between two steps back of one member                             */
    uint16_t rally_px;          /* the rally point: the anchor moved this far towards the enemy base      */
    float    noise;             /* the amplitude of impai_noise on the margins and the economy's ratios   */
    float    attack_margin;     /* the predicted margin a wave needs                                      */
    float    retreat_margin;    /* below it a squad pulls back                                            */
    float    wave_value;        /* a wave's value, in v (the median army type's value); +1 v each wave    */
    uint16_t scout_period;      /* ticks between scouting runs                                            */
    uint16_t task_timeout;      /* ticks an attack or a raid may last                                     */
    float    tech_scale;        /* tech-up's income, as a factor of the table's tech_m, which is Hard's   */
    float    aa_struct_v;       /* seen enemy air value for each anti-air structure                       */
    float    dgun_reserve;      /* the D-gun fires only while stored energy is at least the shot plus this
                                   share of the seat's energy storage                                       */
} IMPAI_LEVEL;
extern const IMPAI_LEVEL IMPAI_LEVELS[IMPAI_LVS];

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
    const IMPAI_LEVEL* lv;
    void*              scratch; size_t scratch_bytes;   /* impai_brain_scratch_bytes(), the caller's        */
    SNAP_SCRATCH*      snap;
    const struct IMPAI_MIL* mil;        /* the seat's military: the noise seed, the commander's window, the
                                           table and picture the factories buy by; NULL: none of them      */
    const IMPAI_PREDICTOR* pred;        /* the pair rate the type choice weighs (impai_fight_rate_of); NULL,
                                           or no military: the factories buy no army                        */
} IMPAI_BRAIN_IN;

/* ---- the military's standing state (impai_mil.c; research/notes/impai/brain.md, The military) ---- */

#define IMPAI_SQUADS   16
#define IMPAI_CONTACTS 32
enum { IMPAI_SQ_FREE, IMPAI_SQ_FORM, IMPAI_SQ_DEFEND, IMPAI_SQ_RAID, IMPAI_SQ_ATTACK, IMPAI_SQ_RETREAT,
       IMPAI_SQ_SCOUT, IMPAI_SQ_TASKS };

typedef struct IMPAI_SQUAD {          /* 20 bytes                                                              */
    uint8_t  task, layer, n, flags;   /* IMPAI_SQ_*; TASIM_LAYER; members at the last decision; a scout's leg   */
    int16_t  tx, tz;                  /* the task's point, 32-px block                                         */
    uint16_t target;                  /* the unit slot it goes for, WQ_NONE                                    */
    uint16_t seen;                    /* decisions the task's contact has persisted (react_confirm)            */
    uint32_t since;                   /* GameTime the task began                                               */
    float    value;                   /* its members' value at the last decision                               */
} IMPAI_SQUAD;

/* One seat's military. THE TABLE is how its army is bought (impai_table.h), set at the level init and kept
   here, so a replay or a takeover buys as the game did whatever lever chose it. THE PICTURE is what the seat
   had seen of the enemy at its last military decision, which its factories buy against. */
typedef struct IMPAI_MIL {
    uint64_t seed;                    /* ImpAI's noise seed: 0x51FC88 at the level init, xor the seat           */
    uint8_t  level, next_squad;       /* IMPAI_LV_*; the round-robin's start                                   */
    uint8_t  table_lever, scout_next; /* 1: the table came from tagpu_impai_table.on; the next scouting point  */
    int16_t  ebx, ebz;                /* the enemy base's estimate, 32-px block; IMPAI_NO_ANCHOR               */
    uint32_t scout_t, wave_t;         /* GameTime of the last scouting run, of the last attack wave            */
    uint32_t safe_until;              /* the commander is the military's until this GameTime                   */
    uint32_t pulse_roots;             /* military roots given: the pulse line's count                          */
    uint16_t waves;                   /* attack waves sent                                                     */
    int8_t   naval;                   /* the naval judgement of the last military decision (impai_mil.c,
                                         naval): 1 the map is mostly water or the estimate is off the anchor's
                                         land component, so the seat's next factory after its first is a sea
                                         plant (impai_brain.c, economy); 0 not; -1 not judged yet, which holds
                                         the second factory                                                    */
    uint8_t  pad;
    float    air_seen;                /* the seen enemy air value: the anti-air structures' rule               */
    IMPAI_TABLE   table;
    IMPAI_PICTURE pic;
    IMPAI_SQUAD   sq[IMPAI_SQUADS];
} IMPAI_MIL;

/* ImpAI's noise, in [-1, 1): splitmix64's finaliser of seed ^ (gametime << 16) ^ (tag << 8) ^ i, its top 24
   bits. WHY THE SEED IS READ, NEVER DRAWN: a draw of 0x4B6C30 would move the simulation's one stream, so every
   OTA AI decision and every weapon's spread after it would differ from the same game without ImpAI; reading
   the state at the level init changes nothing. WHY IT IS STATELESS: a function of the seed and the decision
   keeps no stream position, so nothing can drift between a game, its replay and a takeover -- the seed, in
   IMPAI_MIL, is the only state. Tags: 1 a squad's margin, 2 the economy's ratios. */
float impai_noise(uint64_t seed, uint32_t gametime, unsigned tag, unsigned i);
/* a seat's military at a level's start: no squad, no estimate; the level, the seed and the table given */
void  impai_mil_reset(IMPAI_MIL* m, unsigned level, uint64_t seed, const IMPAI_TABLE* table, int from_lever);

/* What one decision did, against the level row's bounds, and the census's counts. */
typedef struct IMPAI_BRAIN_OUT {
    uint16_t census, sites, snaps, roots, queued, failed, orders;
    uint16_t count[IMPAI_CATS];
    uint16_t army;                      /* army units queued                                                     */
    uint16_t sea_plants;                /* sea plant orders given                                                */
} IMPAI_BRAIN_OUT;

/* 1 when seat `seat` decides at `gametime`: GameTime % decide == 7 x seat % decide, so with decide 15 the ten
   seats' phases are 0, 7, 14, 6, 13, 5, 12, 4, 11, 3 and no two seats decide on one tick. */
int    impai_brain_due(unsigned gametime, unsigned seat, const IMPAI_LEVEL* lv);
/* the scratch one decision takes: its census and lists, two build menus, the threat map's arrays. At most
   IMPAI_BRAIN_SCRATCH_MAX, which impai_brain.c asserts at compile time, so a caller may hold it statically. */
#define IMPAI_BRAIN_SCRATCH_MAX (512u * 1024u)
size_t impai_brain_scratch_bytes(void);
/* the decision's state at a level's start: no anchor, no failure, no explorer, every job none */
void   impai_brain_reset(IMPAI_BRAIN* st, IMPAI_JOB* job, unsigned njob);
/* One decision: the census, the job checks, the economy, the builders, the factories. 0, or -1 when the input
   is refused (a table short of the world, the scratch short), with nothing written. */
int    impai_brain_decide(const IMPAI_BRAIN_IN* in, IMPAI_BRAIN_OUT* out);

#endif
