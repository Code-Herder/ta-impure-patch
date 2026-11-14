/* impai_mil.c -- ImpAI's military manager (impai_mil.h): squads per layer, five tasks and a scout, engagement by
   the fight predictor, the commander kept out of fights it would lose, and the picture of the enemy the
   factories buy against. Engine-free, no statics: everything it knows comes through the WORLDQ and its
   arguments, everything it keeps is the caller's (IMPAI_MIL, the job table). */
#include <string.h>
#include "impai_mil.h"
#include "impai_brain.h"
#include "tagpu_simreg.h"

/* The military's decision. OWNER as tagpu_impai.c's impai_mil: it decides only for a live ImpAI seat, so a
   puppet's orders and its IMPAI_MIL come from the recording. */
SIM_FEATURE(impai_mil_decide, SIM_PUPPET_OWNER, "impai-fight stock-impai-fight impai-eco impai-observe stock-impai-observe impai-naval stock-impai-naval impai-raid impai-persona");

#define REACH_PAD    384        /* a squad engages a contact within its longest range plus this              */
#define BASE_PAD     512        /* a contact this far past base_r from the anchor, or from an own structure,
                                   threatens the base                                                       */
#define NEAR_DEF_PX  512        /* enemy defences this near a contact or a target join its side             */
#define TARGET_PX    1024       /* a wave's target: the enemy structure of most value this near the estimate */
#define ARRIVE_PX    256        /* a point is reached within this (scout_step, raid, attack_arrived)        */
#define RALLY_PX     512        /* a retreating member has arrived this near its point (arrived)            */
#define CMD_ARMY_PX  512        /* the army this near the commander fights beside it                        */
#define MICRO_PX     400        /* a damaged member steps back this far                                     */
#define SPREAD_PX    64         /* members travel to a 5 x 5 grid of points this far apart                  */
#define HEAD_NEAR_PX 128        /* a member already moving within this of its point gets no new order       */
#define SAFE_TICKS   300        /* the commander is the military's this long after a move to safety         */
#define RAID_MIN     3          /* a raid squad's least members [hand-written: the bar's to move]           */
#define RAID_MAX     6
#define SCOUT_LEGS   3
#define TARGETS      32         /* the seen enemies of a contact a fighting squad chooses among             */
#define FOCUS_TYPES  8          /* the target types the quickest kill is weighed over                       */
#define ETYPES       64         /* distinct enemy types the picture weighs                                  */
#define SIDE_IN      64         /* entries gathered for one side before impai_fight_side merges them        */
#define STARTS       16
#define SUB_BLOCKS   32         /* a move's point is moved at most this many 32-px blocks (1024 px)          */
#define SUB_CACHE    32         /* the substitutes one decision computes                                     */
#define NEAR_KEYS    8          /* the (class, label) pairs one reach test of a squad computes               */
#define RANGE_MAX    1024       /* a range read from the threat table is data: held to this for a reach test */

typedef struct MUNIT {          /* an army unit of the census                                              */
    uint16_t slot, type;
    uint8_t  sq, layer, klass, fresh;   /* its squad, IMPAI_SQUADS for none yet; TASIM_LAYER; IMPAI_K_*; 1 when
                                           it joined a squad this decision                                  */
    uint8_t  armed, attack;     /* its +0x110 bit 31, which the resolver reads as "has a weapon" (0x43F161..
                                   0x43F171); its type's IMPAI_TA_*                                          */
    int32_t  x, z, hp;
    uint32_t hp_max;
    float    value;
    int32_t  range, speed;
} MUNIT;

typedef struct MENEMY {         /* an enemy row of the seat's column                                       */
    uint16_t slot, type;        /* type WQ_NONE: a radar contact never identified                           */
    int32_t  x, z;              /* its block's centre, px                                                   */
    float    value;
    uint8_t  now, mobile, armed, contact;   /* seen now: 1 own sight or shared, 2 the radar pass alone, 0 not;
                                               its contact + 1                                              */
    uint8_t  owner, threat;     /* its row's owner, 0xFF never identified; 1 when it threatens the base
                                   (base_threats)                                                          */
    int16_t  cnext, next;       /* the next enemy of its 256-px cell; of its contact                        */
} MENEMY;

typedef struct MCONTACT {
    int64_t  sx, sz;
    int32_t  x, z;
    int32_t  tx, tz;            /* a threat's: its threatening member nearest the anchor, px (base_threats)  */
    uint32_t near_anchor;       /* its nearest member's distance from the anchor; a threat's, that of (tx, tz) */
    float    value;
    uint16_t n;
    int16_t  first;
    uint8_t  threat;            /* it threatens the base                                                    */
} MCONTACT;

typedef struct MSQ {            /* a squad's figures in this decision                                       */
    int64_t  sx, sz;
    int32_t  x, z, range, speed;
    uint16_t n;
    float    value;
    int      contact;           /* the contact it fights, -1 none                                           */
} MSQ;

typedef struct ETYPE { uint16_t type, n; float value; } ETYPE;
typedef struct OWNS { int32_t x, z; } OWNS;
typedef struct MSPARE { int32_t x, z, r; } MSPARE;     /* a friendly unit the D-gun spares: its point and clearance, px */
#define DGUN_SPARE_MAX 256      /* the friendly units the D-gun's guard holds; more near the commander: no shot  */
/* a move's substitute point (point_for): for a member of class `cls` on component `label`, the block nearest
   (bx, bz) of that component, (sx, sz), when `found` */
typedef struct MSUB { uint16_t label; uint8_t cls, found; int16_t bx, bz, sx, sz; } MSUB;

#define ACTS_NONE 0x7FFFFFFF  /* no budget: more actions than any decision spends (IMPAI_SQUADS + 3 at most) */

typedef struct MDEC {
    const IMPAI_MIL_IN* in;
    IMPAI_MIL_OUT*      out;
    const WORLDQ*       q;
    IMPAI_MIL*          st;
    const IMPAI_PLAN*  plan;
    uint32_t            now;
    int32_t             ax, az, rx, rz;     /* the anchor and the rally point, px                           */
    int                 gw, gh;             /* the 256-px grid                                              */
    int32_t             mw, mh;             /* the map, px, at least 1: the sweep cells' extent             */
    MUNIT               army[IMPAI_ARMY_MAX];
    int                 narmy;
    MSQ                 sq[IMPAI_SQUADS];
    int                 com;                /* the commander's census entry: slot, else 0                   */
    uint16_t            com_type;
    int32_t             com_x, com_z;
    int32_t             com_hp;             /* its health and its maximum (THE STAND, commander)          */
    uint32_t            com_hp_max;
    OWNS                own[IMPAI_STRUCT_MAX];
    int                 nown, own_mex;
    unsigned            tech;
    uint16_t            finished;
    MENEMY              en[IMPAI_ENEMY_MAX];
    int                 nen;
    MCONTACT            ct[IMPAI_CONTACTS];
    int                 nct;
    ETYPE               et[ETYPES];
    int                 net;
    uint16_t            sub;                /* the type a never-identified radar contact counts as          */
    int                 est_struct;         /* the estimate rests on enemy structures                      */
    WQ_START            starts[STARTS];
    int                 nstarts, own_start; /* the start nearest the anchor is the seat's own              */
    int                 predictions, roots;
    int                 acts;               /* THE BUDGET's actions left: the bucket's, ACTS_NONE without one */
    float               v;                  /* the median army unit's value                                 */
    IMPAI_FWORK         fw;                 /* the fight predictor's work, one prediction at a time        */
    int                 act_move;
    MSUB                sub_pt[SUB_CACHE];  /* the decision's substitute points (point_for)                 */
    int                 nsub_pt;
    MSPARE              spare[DGUN_SPARE_MAX];  /* the D-gun's guard (spare_list)                          */
    int                 nspare;
    uint8_t             shoot[IMPAI_ENEMY_MAX]; /* the D-gun's candidates, by enemy index                  */
    /* scratch beside the struct */
    int16_t*            cellhead;
    uint8_t*            label;
    uint8_t*            ownnear;
    int16_t*            ownidx;             /* each 256-px cell's own structure within BASE_PAD, its index + 1, 0
                                               none (THE PRESSURE)                                          */
    int16_t*            cells;
    IMPAI_MAPS          maps;
    int                 maps_built;
} MDEC;

#define ALIGN16(n)  (((n) + 15u) & ~(size_t)15u)
#define GRID_CELLS  ((size_t)IMPAI_GRID_MAX * IMPAI_GRID_MAX)
#define GRID_BYTES  (2 * ALIGN16(GRID_CELLS * sizeof(int16_t)) + 2 * ALIGN16(GRID_CELLS) + \
                     ALIGN16(IMPAI_ENEMY_MAX * sizeof(int16_t)))
#define MAPS_BYTES  (IMPAI_THREATS * ALIGN16(GRID_CELLS * sizeof(uint32_t)) + ALIGN16(GRID_CELLS * sizeof(int32_t)) + \
                     ALIGN16(GRID_CELLS * sizeof(uint16_t)) + \
                     ALIGN16((IMPAI_GRID_MAX + 1) * (size_t)IMPAI_GRID_MAX * sizeof(int32_t)))
_Static_assert(ALIGN16(sizeof(MDEC)) + GRID_BYTES + MAPS_BYTES <= IMPAI_MIL_SCRATCH_MAX,
               "a military decision's scratch fits IMPAI_MIL_SCRATCH_MAX");

size_t impai_mil_scratch_bytes(void) { return ALIGN16(sizeof(MDEC)) + GRID_BYTES + MAPS_BYTES; }

int impai_mil_due(unsigned gametime, unsigned seat, unsigned look)
{
    return look && gametime % look == (7u * seat + 8u) % 15u;
}

char impai_mil_letter(unsigned task)
{
    static const char L[IMPAI_SQ_TASKS] = { 'f', 'F', 'D', 'R', 'A', 'B', 'S' };
    return task < IMPAI_SQ_TASKS ? L[task] : '?';
}

static MDEC* carve(const IMPAI_MIL_IN* in)
{
    unsigned char* p = (unsigned char*)in->scratch;
    MDEC* d = (MDEC*)p;
    int k;
    memset(d, 0, sizeof *d);
    p += ALIGN16(sizeof *d);
    d->cellhead = (int16_t*)p; p += ALIGN16(GRID_CELLS * sizeof(int16_t));
    d->label = p; p += ALIGN16(GRID_CELLS);
    d->ownnear = p; p += ALIGN16(GRID_CELLS);
    d->ownidx = (int16_t*)p; p += ALIGN16(GRID_CELLS * sizeof(int16_t));
    d->cells = (int16_t*)p; p += ALIGN16(IMPAI_ENEMY_MAX * sizeof(int16_t));
    for (k = 0; k < IMPAI_THREATS; k++) {
        d->maps.threat[k] = (uint32_t*)p;
        p += ALIGN16(GRID_CELLS * sizeof(uint32_t));
    }
    d->maps.influence = (int32_t*)p; p += ALIGN16(GRID_CELLS * sizeof(int32_t));
    d->maps.presence = (uint16_t*)p; p += ALIGN16(GRID_CELLS * sizeof(uint16_t));
    d->maps.diff = (int32_t*)p;
    return d;
}

/* ---- small rules ---- */

static uint32_t dist(int32_t ax, int32_t az, int32_t bx, int32_t bz)
{
    const int64_t dx = (int64_t)ax - bx, dz = (int64_t)az - bz;
    return impai_isqrt((uint64_t)(dx * dx + dz * dz));
}

static uint8_t layer_of_class(unsigned k)
{
    if ((IMPAI_K_AIRMASK >> k) & 1u) return TASIM_LAYER_AIR;
    if (k >= IMPAI_K_SRAIDER && k <= IMPAI_K_SAA) return TASIM_LAYER_SEA;
    return TASIM_LAYER_LAND;
}

static int fc_type(const MDEC* d, unsigned t) { return d->in->fc && t < d->in->fc->nt; }

static void job_clear(IMPAI_JOB* j)
{
    memset(j, 0, sizeof *j);
    j->type = j->build = j->target = (uint16_t)WQ_NONE;
}

static void join(MDEC* d, MUNIT* u, int s)
{
    IMPAI_JOB* j = &d->in->job[u->slot];
    if (j->kind != IMPAI_JOB_SQUAD) {
        job_clear(j);
        j->kind = IMPAI_JOB_SQUAD;
        j->type = u->type;
        j->since = 0;
    }
    j->squad = (uint8_t)s;
    u->sq = (uint8_t)s;
    u->fresh = 1;
}

/* the squad of `task` on `layer`, else a free one made so, else -1 */
static int squad_of(MDEC* d, unsigned task, unsigned layer)
{
    int k;
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (d->st->sq[k].task == task && d->st->sq[k].layer == layer) return k;
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (d->st->sq[k].task == IMPAI_SQ_FREE) {
            IMPAI_SQUAD* s = &d->st->sq[k];
            memset(s, 0, sizeof *s);
            s->task = (uint8_t)task;
            s->layer = (uint8_t)layer;
            s->target = (uint16_t)WQ_NONE;
            s->since = d->now;
            return k;
        }
    return -1;
}

/* squad s freed: no task, no figures; its members are left with none (IMPAI_SQUADS), so squads() puts each in
   its layer's FORM squad */
static void free_squad(MDEC* d, int s)
{
    int k;
    for (k = 0; k < d->narmy; k++) if (d->army[k].sq == s) d->army[k].sq = IMPAI_SQUADS;
    memset(&d->st->sq[s], 0, sizeof d->st->sq[s]);
    d->st->sq[s].target = (uint16_t)WQ_NONE;
    memset(&d->sq[s], 0, sizeof d->sq[s]);
    d->sq[s].contact = -1;
}

/* squad `from`'s members and this decision's figures into squad `to`, and `from` freed. Before squads() has
   summed the figures both are zero, so it is a change of membership alone. */
static void merge(MDEC* d, int from, int to)
{
    MSQ* a = &d->sq[from];
    MSQ* b = &d->sq[to];
    int k;
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq == from) join(d, &d->army[k], to);
    b->n = (uint16_t)(b->n + a->n);
    b->value += a->value;
    b->sx += a->sx;
    b->sz += a->sz;
    if (a->range > b->range) b->range = a->range;
    if (a->speed > 0 && (!b->speed || a->speed < b->speed)) b->speed = a->speed;
    if (b->n) {
        b->x = (int32_t)(b->sx / b->n);
        b->z = (int32_t)(b->sz / b->n);
        d->st->sq[to].n = (uint8_t)(b->n < 255 ? b->n : 255);
        d->st->sq[to].value = b->value;
    }
    memset(&d->st->sq[from], 0, sizeof d->st->sq[from]);
    d->st->sq[from].target = (uint16_t)WQ_NONE;
    memset(a, 0, sizeof *a);
    a->contact = -1;
}

/* THE SQUAD TABLE CANNOT FILL. INVARIANT: at most one FORM, one DEFEND and one RETREAT squad a layer, at most
   IMPAI_ATTACKS_MAX ATTACK squads, IMPAI_RAIDS_MAX RAID squads and one SCOUT squad:
   3 TASIM_LAYERS + IMPAI_ATTACKS_MAX + IMPAI_RAIDS_MAX + 1 = 9 + 4 + 2 + 1 = 16 <= IMPAI_SQUADS. A layer with no
   FORM squad holds at most two squads of its own tasks, so at most 15 are taken and one is free: squad_of(FORM,
   layer) always succeeds, and no army unit is left without a squad (IMPAI_MIL_OUT.left_squad stays 0).
   EVERY TASK CHANGE GOES THROUGH become, which keeps the bound: FORM, DEFEND and RETREAT -- when the layer has a
   squad of the task already, s's members join it and s is freed, else s takes the task; ATTACK -- s takes it while
   fewer than IMPAI_ATTACKS_MAX others go, else its members reinforce its layer's ATTACK squad whose point is nearest
   the goal, and with none of its layer two ATTACK squads of another layer merge to make room (attack_room); RAID
   and SCOUT -- s takes them, raid holding them to IMPAI_RAIDS_MAX and scouting sending none while one is out.
   normalise makes the counts a fact at every decision's start too. Returns the squad that holds the task, -1 for
   none. */
_Static_assert(3 * TASIM_LAYERS + IMPAI_ATTACKS_MAX + IMPAI_RAIDS_MAX + 1 <= IMPAI_SQUADS,
               "the per-task bounds fit the squad table");

/* ROOM FOR A LAYER'S FIRST WAVE. INVARIANT: with IMPAI_ATTACKS_MAX ATTACK squads out and none of squad s's layer,
   they stand on the other TASIM_LAYERS - 1 layers, fewer than IMPAI_ATTACKS_MAX, so by the pigeonhole one layer
   holds two or more: of every such pair the two whose points are nearest each other merge, the higher index into
   the lower, ties to the lowest pair. So four land waves cannot keep a sea or air wave from ever going, and the
   ATTACK count, hence the table's arithmetic, is unchanged. Returns 0 with a squad freed, -1 with no pair (a table
   past the bound, which normalise has already sent to FORM). COST: 16 x 16 point distances. */
_Static_assert(IMPAI_ATTACKS_MAX > TASIM_LAYERS - 1, "a full ATTACK count with one layer empty shares a layer");
static int attack_room(MDEC* d, int s)
{
    int a, b, ba = -1, bb = -1;
    uint32_t best = 0;
    for (a = 0; a < IMPAI_SQUADS; a++) {
        const IMPAI_SQUAD* p = &d->st->sq[a];
        if (a == s || p->task != IMPAI_SQ_ATTACK) continue;
        for (b = a + 1; b < IMPAI_SQUADS; b++) {
            const IMPAI_SQUAD* r = &d->st->sq[b];
            uint32_t y;
            if (b == s || r->task != IMPAI_SQ_ATTACK || r->layer != p->layer) continue;
            y = dist(p->tx * 32 + 16, p->tz * 32 + 16, r->tx * 32 + 16, r->tz * 32 + 16);
            if (ba < 0 || y < best) { ba = a; bb = b; best = y; }
        }
    }
    if (ba < 0) return -1;
    merge(d, bb, ba);
    return 0;
}

/* THE ATTENTION: an operation is a squad holding DEFEND, RAID or ATTACK -- FORM is always held, a SCOUT is one
   scout and a RETREAT an operation coming home. A seat whose persona holds att_mil operations at once starts no
   other while it holds that many. ops counts them over the squad table; at most IMPAI_SQUADS. */
static int is_op(unsigned task)
{
    return task == IMPAI_SQ_DEFEND || task == IMPAI_SQ_RAID || task == IMPAI_SQ_ATTACK;
}

static int ops(const MDEC* d)
{
    int k, n = 0;
    for (k = 0; k < IMPAI_SQUADS; k++) n += is_op(d->st->sq[k].task);
    return n;
}

/* 1 when the persona has no room for another operation */
static int op_full(const MDEC* d)
{
    const unsigned att = d->in->persona->att_mil;
    return att && ops(d) >= (int)att;
}

static int become(MDEC* d, int s, unsigned task, int32_t x, int32_t z, uint16_t target)
{
    IMPAI_SQUAD* q = &d->st->sq[s];
    int k, to = -1, others = 0;
    uint32_t best = 0;
    if (task == IMPAI_SQ_FORM || task == IMPAI_SQ_DEFEND || task == IMPAI_SQ_RETREAT) {
        for (k = 0; k < IMPAI_SQUADS && to < 0; k++)
            if (k != s && d->st->sq[k].task == task && d->st->sq[k].layer == q->layer) to = k;
    } else if (task == IMPAI_SQ_ATTACK) {
        for (k = 0; k < IMPAI_SQUADS; k++) {
            const IMPAI_SQUAD* a = &d->st->sq[k];
            uint32_t y;
            if (k == s || a->task != IMPAI_SQ_ATTACK) continue;
            others++;
            if (a->layer != q->layer) continue;
            y = dist(a->tx * 32 + 16, a->tz * 32 + 16, x, z);
            if (to < 0 || y < best) { to = k; best = y; }
        }
        if (others < IMPAI_ATTACKS_MAX) to = -1;
    }
    /* THE ATTENTION, before attack_room's merge so that a refusal changes nothing: s would START an operation
       (not merge into a squad that holds one, nor move one it holds) with no room for it */
    if (to < 0 && is_op(task) && !is_op(q->task) && op_full(d)) return -1;
    if (task == IMPAI_SQ_ATTACK && to < 0 && others >= IMPAI_ATTACKS_MAX && attack_room(d, s) < 0) return -1;
    if (to >= 0) {
        merge(d, s, to);
        return to;
    }
    q->task = (uint8_t)task;
    q->tx = (int16_t)(x >> 5);
    q->tz = (int16_t)(z >> 5);
    q->target = target;
    q->seen = 0;
    q->flags = 0;
    q->since = d->now;
    return s;
}

static void back_to_form(MDEC* d, int s) { become(d, s, IMPAI_SQ_FORM, d->rx, d->rz, (uint16_t)WQ_NONE); }

/* THE NORMALISATION, at each decision's start, after the census and before the squads' figures: a squad whose
   task or layer is past its enum -- a recorded byte -- is freed; duplicates of a per-layer task (FORM, DEFEND,
   RETREAT) merge into the lowest-indexed; ATTACK squads past the IMPAI_ATTACKS_MAX-th, RAID past the
   IMPAI_RAIDS_MAX-th and SCOUT past the first go back to FORM (become). It makes become's bound a fact of every
   decision, whatever a recorded or restored table holds and whatever a later edit adds, rather than an
   induction over every transition. COST: 16 x 16 compares. */
static void normalise(MDEC* d)
{
    int k, j, attacks = 0, raids = 0, scouts = 0;
    for (k = 0; k < IMPAI_SQUADS; k++) {
        const IMPAI_SQUAD* q = &d->st->sq[k];
        if (q->task == IMPAI_SQ_FREE) continue;
        if (q->task >= IMPAI_SQ_TASKS || q->layer >= TASIM_LAYERS) {
            free_squad(d, k);
            continue;
        }
        if (q->task != IMPAI_SQ_FORM && q->task != IMPAI_SQ_DEFEND && q->task != IMPAI_SQ_RETREAT) continue;
        for (j = 0; j < k && !(d->st->sq[j].task == q->task && d->st->sq[j].layer == q->layer); j++) {}
        if (j < k) merge(d, k, j);
    }
    for (k = 0; k < IMPAI_SQUADS; k++) {
        const unsigned t = d->st->sq[k].task;
        if ((t == IMPAI_SQ_ATTACK && ++attacks > IMPAI_ATTACKS_MAX) ||
            (t == IMPAI_SQ_RAID && ++raids > IMPAI_RAIDS_MAX) || (t == IMPAI_SQ_SCOUT && ++scouts > 1))
            back_to_form(d, k);
    }
}

/* ---- 1. the army's census ---- */

/* the sweep cell (IMPAI_MIL.swept) holding (x, z) px, each axis held to 0..IMPAI_SWEEP_SIDE - 1 */
static int sweep_cell(const MDEC* d, int32_t x, int32_t z)
{
    int64_t cx = (int64_t)x * IMPAI_SWEEP_SIDE / d->mw, cz = (int64_t)z * IMPAI_SWEEP_SIDE / d->mh;
    if (cx < 0) cx = 0;
    if (cz < 0) cz = 0;
    if (cx >= IMPAI_SWEEP_SIDE) cx = IMPAI_SWEEP_SIDE - 1;
    if (cz >= IMPAI_SWEEP_SIDE) cz = IMPAI_SWEEP_SIDE - 1;
    return (int)(cz * IMPAI_SWEEP_SIDE + cx);
}

/* the sweep's stamp for an army unit standing in a cell now: GameTime / 64, 0 kept for never, held at 65 535 */
static uint16_t sweep_stamp(const MDEC* d)
{
    return (uint16_t)(d->now >> 6 == 0 ? 1u : d->now >> 6 > 0xFFFFu ? 0xFFFFu : d->now >> 6);
}

/* the centre of sweep cell c, px */
static void sweep_centre(const MDEC* d, int c, int32_t* x, int32_t* z)
{
    *x = (int32_t)(((int64_t)(c % IMPAI_SWEEP_SIDE) * 2 + 1) * d->mw / (2 * IMPAI_SWEEP_SIDE));
    *z = (int32_t)(((int64_t)(c / IMPAI_SWEEP_SIDE) * 2 + 1) * d->mh / (2 * IMPAI_SWEEP_SIDE));
}

/* a type's maxvelocity: from an army unit of the type already listed, else its def */
static int32_t type_speed(MDEC* d, unsigned type)
{
    WQ_DEF def;
    int k;
    for (k = 0; k < d->narmy; k++) if (d->army[k].type == type) return d->army[k].speed;
    return d->q->ops->def(d->q, (int)type, &def) ? def.speed : 0;
}

static void census(MDEC* d)
{
    const IMPAI_MIL_IN* in = d->in;
    int first, last, slot, k;
    if (!d->q->ops->seat_units(d->q, &first, &last)) return;
    if (first < 1) first = 1;
    if (last >= (int)in->njob) last = (int)in->njob - 1;
    for (slot = first; slot <= last; slot++) {
        IMPAI_JOB* j = &in->job[slot];
        const IMPAI_TINFO* t;
        WQ_UNIT u;
        MUNIT* m;
        int32_t speed;
        if (!d->q->ops->unit(d->q, slot, &u) || !u.type || u.type >= in->nti) continue;
        t = &in->ti[u.type];
        d->finished += u.finished;
        if (!u.finished) continue;
        if (!(t->flags & IMPAI_TF_MOBILE)) {
            if (d->nown < IMPAI_STRUCT_MAX) {
                d->own[d->nown].x = u.pos[0] >> 16;
                d->own[d->nown].z = u.pos[2] >> 16;
                d->nown++;
            }
            if (t->cat == IMPAI_C_MEX) d->own_mex++;
            if (t->cat == IMPAI_C_FACTORY && t->tech > d->tech) d->tech = t->tech;
            continue;
        }
        if (t->cat == IMPAI_C_COMMANDER && !d->com) {
            d->com = slot;
            d->com_type = u.type;
            d->com_x = u.pos[0] >> 16;
            d->com_z = u.pos[2] >> 16;
            d->com_hp = u.hp;
            d->com_hp_max = u.hp_max;
        }
        if (t->klass >= IMPAI_KS) continue;
        /* A recycled slot: a job of another type is voided, and a squad index past the table is a bound on
           a recorded byte, voided too */
        if (j->type != WQ_NONE && j->type != u.type) job_clear(j);
        if (j->kind == IMPAI_JOB_SQUAD && j->squad >= IMPAI_SQUADS) job_clear(j);
        if (d->narmy >= IMPAI_ARMY_MAX) {
            d->out->left_army++;
            continue;
        }
        speed = type_speed(d, u.type);
        m = &d->army[d->narmy++];
        memset(m, 0, sizeof *m);
        m->slot = (uint16_t)slot;
        m->type = u.type;
        m->klass = t->klass;
        m->layer = layer_of_class(t->klass);
        m->armed = (uint8_t)(u.state >> 31);
        m->attack = t->attack;
        m->x = u.pos[0] >> 16;
        m->z = u.pos[2] >> 16;
        m->hp = u.hp;
        m->hp_max = u.hp_max;
        m->value = t->value;
        m->range = in->view && in->view->threat && u.type < (unsigned)in->view->ntypes
                       ? in->view->threat[u.type].range : 0;
        m->speed = speed;
        m->sq = j->kind == IMPAI_JOB_SQUAD && d->st->sq[j->squad].task != IMPAI_SQ_FREE ? j->squad : IMPAI_SQUADS;
        /* THE SWEEP: the army unit stands in its cell now; at most IMPAI_ARMY_MAX writes */
        d->st->swept[sweep_cell(d, m->x, m->z)] = sweep_stamp(d);
    }
    /* the median army unit's value, v: by insertion over at most IMPAI_ARMY_MAX values */
    {
        float vals[IMPAI_ARMY_MAX];
        int i, n = 0;
        for (k = 0; k < d->narmy; k++) {
            const float x = d->army[k].value;
            for (i = n; i > 0 && vals[i - 1] > x; i--) vals[i] = vals[i - 1];
            vals[i] = x;
            n++;
        }
        d->v = n ? vals[n / 2] : 0.0f;
    }
}

/* each squad's members, value, centroid, range and speed; an army unit with no squad joins its layer's FORM
   squad; a squad with no member is free */
static void squads(MDEC* d)
{
    int k;
    for (k = 0; k < d->narmy; k++) {
        MUNIT* u = &d->army[k];
        int s = u->sq;
        if (s >= IMPAI_SQUADS) {
            if ((s = squad_of(d, IMPAI_SQ_FORM, u->layer)) < 0) {
                d->out->left_squad++;
                continue;
            }
            join(d, u, s);
        }
        d->sq[s].n++;
        d->sq[s].value += u->value;
        d->sq[s].sx += u->x;
        d->sq[s].sz += u->z;
        if (u->range > d->sq[s].range) d->sq[s].range = u->range;
        if (!d->sq[s].speed || (u->speed > 0 && u->speed < d->sq[s].speed)) d->sq[s].speed = u->speed;
    }
    for (k = 0; k < IMPAI_SQUADS; k++) {
        MSQ* s = &d->sq[k];
        IMPAI_SQUAD* q = &d->st->sq[k];
        s->contact = -1;
        if (q->task == IMPAI_SQ_FREE) continue;
        if (!s->n) {
            memset(q, 0, sizeof *q);
            q->target = (uint16_t)WQ_NONE;
            continue;
        }
        s->x = (int32_t)(s->sx / s->n);
        s->z = (int32_t)(s->sz / s->n);
        q->n = (uint8_t)(s->n < 255 ? s->n : 255);
        q->value = s->value;
    }
}

/* ---- 2. the picture ---- */

static void etype_add(MDEC* d, uint16_t type, float value)
{
    int k;
    for (k = 0; k < d->net && d->et[k].type != type; k++) {}
    if (k == d->net) {
        if (d->net >= ETYPES) return;
        d->et[d->net].type = type;
        d->et[d->net].n = 0;
        d->et[d->net].value = 0.0f;
        d->net++;
    }
    d->et[k].n++;
    d->et[k].value += value;
}

/* the type a never-identified radar contact counts as: the identified enemy mobile type of median value, else
   the seat's own army type of median value, else none */
static uint16_t substitute(const MDEC* d)
{
    uint16_t types[ETYPES > IMPAI_ARMY_MAX ? ETYPES : IMPAI_ARMY_MAX];
    float vals[ETYPES > IMPAI_ARMY_MAX ? ETYPES : IMPAI_ARMY_MAX];
    int k, i, n = 0;
    for (k = 0; k < d->net; k++) {
        const float x = d->in->ti[d->et[k].type].value;
        for (i = n; i > 0 && (vals[i - 1] > x || (vals[i - 1] == x && types[i - 1] > d->et[k].type)); i--) {
            vals[i] = vals[i - 1];
            types[i] = types[i - 1];
        }
        vals[i] = x;
        types[i] = d->et[k].type;
        n++;
    }
    if (n) return types[n / 2];
    for (k = 0; k < d->narmy; k++) {
        const float x = d->army[k].value;
        for (i = n; i > 0 && (vals[i - 1] > x || (vals[i - 1] == x && types[i - 1] > d->army[k].type)); i--) {
            vals[i] = vals[i - 1];
            types[i] = types[i - 1];
        }
        vals[i] = x;
        types[i] = d->army[k].type;
        n++;
    }
    return n ? types[n / 2] : (uint16_t)WQ_NONE;
}

/* The seat's column of the knowledge rows, bounded by the view's row count: an enemy row is one whose owner is
   neither the seat nor an ally, or a radar contact never identified. Seen now (own sight, an ally's, or the
   radar pass) counts as present; a ghost counts only for a structure. Every type is held below nti before it
   indexes a table. */
static void picture(MDEC* d)
{
    const IMPAI_VIEW* v = d->in->view;
    const IMPAI_MIL_IN* in = d->in;
    IMPAI_PICTURE_IN pin;
    float mobile_value = 0.0f, sx = 0.0f, sz = 0.0f, sw = 0.0f;
    int i, k;
    memset(&pin, 0, sizeof pin);
    for (i = 1; v && i < v->nrows; i++) {
        const IMPAI_SEEN* r = &v->rows[i].seat[v->seat];
        const int ident = r->type != WQ_NONE && r->type < in->nti && r->owner < WQ_SEATS;
        const int now = (r->how & (IMPAI_NOW | IMPAI_PASS)) != 0;
        MENEMY* e;
        if (!r->how || (r->how & IMPAI_OWN)) continue;
        if (ident && (r->owner == (unsigned)v->seat || ((v->allies >> r->owner) & 1u))) continue;
        /* THE COMMANDER'S MEMORY (IMPAI_MIL.foe): a foe's commander in the seat's own line of sight now
           (IMPAI_LOS; the seat's own units' rows are skipped above). Never an ally's (IMPAI_SHARED): that is a
           position only, and may describe a unit that has left the slot since. owner is below WQ_SEATS by ident's
           test */
        if (ident && (r->how & IMPAI_LOS) && in->ti[r->type].cat == IMPAI_C_COMMANDER) {
            IMPAI_FOE* f = &d->st->foe[r->owner];
            f->slot = (uint16_t)i;
            f->bx = (int16_t)r->bx;
            f->bz = (int16_t)r->bz;
            f->pad = 0;
            f->t = d->now;
        }
        if (!ident && !impai_row_unidentified_now(r, in->nti)) continue;
        if (ident && !now && (in->ti[r->type].flags & IMPAI_TF_MOBILE)) continue;
        if (d->nen >= IMPAI_ENEMY_MAX) {
            d->out->left_enemy++;
            continue;
        }
        e = &d->en[d->nen++];
        memset(e, 0, sizeof *e);
        e->slot = (uint16_t)i;
        e->type = ident ? r->type : (uint16_t)WQ_NONE;
        e->x = r->bx * 32 + 16;
        e->z = r->bz * 32 + 16;
        e->now = (uint8_t)(!now ? 0 : (r->how & IMPAI_NOW) ? 1 : 2);
        e->mobile = (uint8_t)(!ident || (in->ti[r->type].flags & IMPAI_TF_MOBILE));
        e->owner = (uint8_t)(ident ? r->owner : 0xFFu);
        e->cnext = e->next = -1;
        if (ident) {
            const IMPAI_TINFO* t = &in->ti[r->type];
            const IMPAI_THREAT* th = v->threat && r->type < (unsigned)v->ntypes ? &v->threat[r->type] : NULL;
            e->value = t->value;
            e->armed = (uint8_t)(th && (th->dps_ground || th->dps_air || th->dps_water));
            if (e->mobile && now) etype_add(d, r->type, t->value);
        }
    }
    d->sub = substitute(d);
    d->st->radar_as = d->sub;
    for (k = 0; k < d->nen; k++) {
        MENEMY* e = &d->en[k];
        if (e->type == WQ_NONE) {
            e->value = d->sub != WQ_NONE ? in->ti[d->sub].value : 0.0f;
            pin.total += e->value;
            continue;
        }
        {
            const IMPAI_TINFO* t = &in->ti[e->type];
            const IMPAI_THREAT* th = v->threat && e->type < (unsigned)v->ntypes ? &v->threat[e->type] : NULL;
            pin.total += e->value;
            if (e->mobile) {
                if (t->klass < IMPAI_KS) pin.kval[t->klass] += e->value;
                if (layer_of_class(t->klass < IMPAI_KS ? t->klass : 0) == TASIM_LAYER_AIR && t->klass < IMPAI_KS)
                    d->st->air_seen += e->value;
                mobile_value += e->value;
            } else {
                if (th && th->dps_air > th->dps_ground) pin.aa_struct += e->value;
                else if (th && th->dps_ground) pin.defence += e->value;
                if (t->cat == IMPAI_C_MEX) pin.enemy_extractors++;
                sx += e->value * (float)e->x;
                sz += e->value * (float)e->z;
                sw += e->value;
            }
        }
    }
    pin.own_extractors = (uint32_t)d->own_mex;
    pin.gametime = d->now;
    pin.tech = d->tech;
    impai_picture_features(&pin, &d->st->pic);
    /* the enemy's mobile composition for the type choice: its ETYPES most valuable types, at most
       IMPAI_PIC_ENEMY, by insertion; ties to the lower type */
    {
        IMPAI_PICTURE* p = &d->st->pic;
        p->ne = 0;
        for (k = 0; k < d->net; k++) {
            const ETYPE* x = &d->et[k];
            unsigned j = p->ne < IMPAI_PIC_ENEMY ? p->ne : IMPAI_PIC_ENEMY - 1;
            if (p->ne == IMPAI_PIC_ENEMY && !(x->value > p->e[j].share)) continue;
            while (j > 0 && (p->e[j - 1].share < x->value ||
                             (p->e[j - 1].share == x->value && p->e[j - 1].type > x->type))) {
                p->e[j] = p->e[j - 1];
                j--;
            }
            p->e[j].type = x->type;
            p->e[j].pad = 0;
            p->e[j].share = x->value;
            if (p->ne < IMPAI_PIC_ENEMY) p->ne++;
        }
        for (k = 0; k < (int)p->ne; k++) p->e[k].share = mobile_value > 0.0f ? p->e[k].share / mobile_value : 0.0f;
    }
    /* THE ENEMY BASE'S ESTIMATE: the value-weighted centroid of the enemy structures seen or remembered, else the
       start position nearest the seat's own start that is not it -- the map's start positions are public, the
       skirmish screen shows them */
    d->est_struct = sw > 0.0f;
    d->st->est_seen = (uint8_t)d->est_struct;
    if (d->est_struct) {
        d->st->ebx = (int16_t)((int32_t)(sx / sw) >> 5);
        d->st->ebz = (int16_t)((int32_t)(sz / sw) >> 5);
    } else if (d->nstarts > 1) {
        int best = -1;
        for (k = 0; k < d->nstarts; k++)
            if (k != d->own_start && (best < 0 || dist(d->starts[k].x, d->starts[k].z, d->ax, d->az) <
                                                  dist(d->starts[best].x, d->starts[best].z, d->ax, d->az)))
                best = k;
        d->st->ebx = (int16_t)(d->starts[best].x >> 5);
        d->st->ebz = (int16_t)(d->starts[best].z >> 5);
    }
}

/* THE NAVAL JUDGEMENT, each decision right after the picture, so it follows the estimate as it moves:
   naval = sea_blocks > 0 && (mostly water || the estimate is off the anchor's land component), into IMPAI_MIL.naval
   (impai.h; the economy's second factory waits for it, impai_brain.c, economy).
   - MOSTLY WATER: more than half of the level's 32-px blocks carry a label of some class a sea army type uses
     (impai_mostly_water). Labels, not heights, so lava and land-locked shallows no sea class reaches do not count.
     The half puts Red Triangle (75 % of its cells under the sea) in and Trout Farm (40 %) and Lake Shore (39 %)
     out (MEASURED 2026-11-13 from the install's TNT heights).
   - OFF THE ANCHOR'S LAND COMPONENT, by impai_land_link (the classes of the content's land army types, hover and
     amphibious ones included, holding at least half of them): with no enemy structure known, the estimate's
     block is not land-linked to the anchor's; with structures known, less than half of their value stands on
     land-linked blocks -- their value and not the estimate's centroid, because a value-weighted centroid of two
     islands falls in the sea. No land army type, no estimate, a block off the grid: not off, no evidence.
   - A map no sea army reaches is never naval (sea_blocks 0: a lava map with separated starts), and a level with
     no labels judges 0.
   THE TERRAIN IS PUBLIC: the labels answer over the whole map, as the economy's reach test does, because the
   skirmish screen shows the map's picture, land and water, to every seat, as it shows the start positions the
   estimate uses. COST: at most IMPAI_ENEMY_MAX structures, each at most WQ_MCLASSES label pairs. */
static void naval(MDEC* d)
{
    const IMPAI_REACH* r = d->in->reach;
    const int abx = d->ax >> 5, abz = d->az >> 5;
    int off = 0, k;
    if (!r || !r->labels || !r->sea_blocks) {
        d->st->naval = 0;
        return;
    }
    if (!impai_mostly_water(r) && r->land_total && d->st->ebx != IMPAI_NO_ANCHOR) {
        if (!d->est_struct)
            off = !impai_land_link(r, abx, abz, d->st->ebx, d->st->ebz);
        else {
            float linked = 0.0f, total = 0.0f;
            for (k = 0; k < d->nen; k++) {
                const MENEMY* e = &d->en[k];
                if (e->mobile || e->type == WQ_NONE) continue;
                total += e->value;
                if (impai_land_link(r, abx, abz, e->x >> 5, e->z >> 5)) linked += e->value;
            }
            off = 2.0f * linked < total;
        }
    }
    d->st->naval = (int8_t)(impai_mostly_water(r) || off);
}

/* THE CONTACTS: the enemies seen now, joined by a flood over 256-px cells with an enemy within IMPAI_CONTACT_PX
   (3 cells, Chebyshev), seeded nearest the anchor first, so that when IMPAI_CONTACTS is reached the contacts
   left out are the farthest. Bounded: each enemy enters one cell list once, each occupied cell is flooded
   once and looks at 7 x 7 cells. */
static void contacts(MDEC* d)
{
    const int span = IMPAI_CONTACT_PX / IMPAI_CELL_PX;
    int16_t bucket[IMPAI_GRID_MAX], bnext[IMPAI_ENEMY_MAX];
    int k, nocc = 0, r;
    memset(d->label, 0, (size_t)d->gw * (size_t)d->gh);
    for (k = 0; k < d->gw * d->gh; k++) d->cellhead[k] = -1;
    for (k = 0; k < IMPAI_GRID_MAX; k++) bucket[k] = -1;
    for (k = 0; k < d->nen; k++) {
        MENEMY* e = &d->en[k];
        int cx = e->x / IMPAI_CELL_PX, cz = e->z / IMPAI_CELL_PX, c;
        if (!e->now) continue;
        if (cx >= d->gw) cx = d->gw - 1;
        if (cz >= d->gh) cz = d->gh - 1;
        c = cz * d->gw + cx;
        if (d->cellhead[c] < 0) {
            const int ax = d->ax / IMPAI_CELL_PX, az = d->az / IMPAI_CELL_PX;
            int ring = cx > ax ? cx - ax : ax - cx;
            if ((cz > az ? cz - az : az - cz) > ring) ring = cz > az ? cz - az : az - cz;
            if (ring >= IMPAI_GRID_MAX) ring = IMPAI_GRID_MAX - 1;
            d->cells[nocc] = (int16_t)c;
            bnext[nocc] = bucket[ring];
            bucket[ring] = (int16_t)nocc;
            nocc++;
        }
        e->cnext = d->cellhead[c];
        d->cellhead[c] = (int16_t)k;
    }
    for (r = 0; r < IMPAI_GRID_MAX; r++)
        for (k = bucket[r]; k >= 0; k = bnext[k]) {
            int16_t queue[IMPAI_ENEMY_MAX];
            int qh = 0, qt = 0;
            MCONTACT* ct;
            if (d->label[d->cells[k]]) continue;
            if (d->nct >= IMPAI_CONTACTS) {
                int16_t e;
                for (e = d->cellhead[d->cells[k]]; e >= 0; e = d->en[e].cnext) d->out->left_contact++;
                d->label[d->cells[k]] = 0xFF;
                continue;
            }
            ct = &d->ct[d->nct];
            memset(ct, 0, sizeof *ct);
            ct->first = -1;
            ct->near_anchor = 0xFFFFFFFFu;
            d->label[d->cells[k]] = (uint8_t)(d->nct + 1);
            queue[qt++] = d->cells[k];
            while (qh < qt) {
                const int c = queue[qh++], cx = c % d->gw, cz = c / d->gw;
                int dx, dz;
                int16_t e;
                for (e = d->cellhead[c]; e >= 0; e = d->en[e].cnext) {
                    MENEMY* m = &d->en[e];
                    const uint32_t da = dist(m->x, m->z, d->ax, d->az);
                    m->contact = (uint8_t)(d->nct + 1);
                    m->next = ct->first;
                    ct->first = e;
                    ct->n++;
                    ct->value += m->value;
                    ct->sx += m->x;
                    ct->sz += m->z;
                    if (da < ct->near_anchor) ct->near_anchor = da;
                }
                for (dz = -span; dz <= span; dz++)
                    for (dx = -span; dx <= span; dx++) {
                        const int nx = cx + dx, nz = cz + dz;
                        int n;
                        if (nx < 0 || nz < 0 || nx >= d->gw || nz >= d->gh) continue;
                        n = nz * d->gw + nx;
                        if (d->cellhead[n] < 0 || d->label[n] || qt >= IMPAI_ENEMY_MAX) continue;
                        d->label[n] = (uint8_t)(d->nct + 1);
                        queue[qt++] = (int16_t)n;
                    }
            }
            ct->x = (int32_t)(ct->sx / ct->n);
            ct->z = (int32_t)(ct->sz / ct->n);
            d->nct++;
        }
    d->out->contacts = (uint16_t)d->nct;
}

/* ---- 3. the base's threats ---- */

/* 1 when member e, at da px from the anchor, stands nearer the anchor than its own side's base: than every known
   enemy structure of its owner (of every foe for a contact never identified), else, with none known, every start
   position but the seat's own; with neither, 1. Squared distances, exact. `st` lists the enemy structures. */
static int nearer_anchor(const MDEC* d, const MENEMY* e, uint32_t da, const int16_t* st, int nst)
{
    const uint64_t da2 = (uint64_t)da * da;
    int k, known = 0;
    for (k = 0; k < nst; k++) {
        const MENEMY* b = &d->en[st[k]];
        const int64_t dx = (int64_t)b->x - e->x, dz = (int64_t)b->z - e->z;
        if (e->owner != 0xFFu && b->owner != e->owner) continue;
        if ((uint64_t)(dx * dx + dz * dz) <= da2) return 0;
        known = 1;
    }
    if (known) return 1;
    for (k = 0; k < d->nstarts; k++) {
        const int64_t dx = (int64_t)d->starts[k].x - e->x, dz = (int64_t)d->starts[k].z - e->z;
        if (k != d->own_start && (uint64_t)(dx * dx + dz * dz) <= da2) return 0;
    }
    return 1;
}

/* 1 when contact member e threatens the base (base_threats, the invariant over it) */
static int threat_member(const MDEC* d, const MENEMY* e, const int16_t* st, int nst)
{
    const uint32_t da = dist(e->x, e->z, d->ax, d->az);
    int cx = e->x / IMPAI_CELL_PX, cz = e->z / IMPAI_CELL_PX;
    if (!e->mobile || !(e->armed || e->type == WQ_NONE)) return 0;
    if (cx >= d->gw) cx = d->gw - 1;
    if (cz >= d->gh) cz = d->gh - 1;
    if (da > (uint32_t)d->plan->base_r + BASE_PAD && !d->ownnear[cz * d->gw + cx]) return 0;
    return nearer_anchor(d, e, da, st, nst);
}

_Static_assert(IMPAI_STRUCT_MAX < 32767, "an own structure's index + 1 fits ownidx's int16");

/* 1 when the substitute a radar contact never identified counts as (MDEC.sub) has a ground reach: the threat row
   the builders' ground map stamps for such a contact (impai_maps.c, impai_maps_build's radar_as) */
static int sub_hits_ground(const MDEC* d)
{
    const IMPAI_VIEW* v = d->in->view;
    return v && v->threat && d->sub < (unsigned)v->ntypes && v->threat[d->sub].hit_ground > 0;
}

/* 1 when contact member m presses the seat (THE PRESSURE, below), its base verdict (MENEMY.threat) into *base */
static int press_member(const MDEC* d, const MENEMY* m, const int16_t* st, int nst, int* base)
{
    int cx = m->x / IMPAI_CELL_PX, cz = m->z / IMPAI_CELL_PX, s;
    *base = m->threat;
    if (!m->mobile || !(m->armed || m->type == WQ_NONE)) return 0;
    if (m->type == WQ_NONE && !sub_hits_ground(d)) return 0;
    if (m->type != WQ_NONE && (m->type >= d->in->nti || d->in->ti[m->type].klass >= IMPAI_KS)) return 0;
    if (m->threat) return 1;
    if (cx >= d->gw) cx = d->gw - 1;
    if (cz >= d->gh) cz = d->gh - 1;
    if ((s = d->ownidx[cz * d->gw + cx]) <= 0) return 0;
    return nearer_anchor(d, m, dist(m->x, m->z, d->own[s - 1].x, d->own[s - 1].z), st, nst);
}

/* THE BASE'S THREATS. INVARIANT: a contact is a threat only through a member that is MOBILE AND ARMED -- an
   identified type with any dps in the threat table, or a radar contact never identified, which the picture
   already counts as a mobile type -- that stands within base_r + BASE_PAD of the anchor or within BASE_PAD (two
   256-px cells, Chebyshev) of an own structure that itself stands within base_r of the anchor (THE ANCHOR BOX, the
   bound), and that stands NEARER THE ANCHOR THAN ITS OWN SIDE'S BASE (nearer_anchor): than every known enemy
   structure of its owner, else than every start position but the seat's own. So a structure, an unarmed builder,
   or a unit at home among its own side's structures never holds a FORM squad in DEFEND, and a threat ends when its
   armed mobiles die or leave that box. Each member's own owner, and not one estimate of every enemy's base,
   because the value-weighted centre of two enemies' structures on opposite sides of the seat falls near the seat's
   own base and would hold every enemy nearer it than the anchor. The contact's (tx, tz) and near_anchor are its
   threatening member nearest the anchor, where DEFEND goes; each member's verdict is kept (MENEMY.threat).
   WHY THE HALF-MAP BOUND: base_r + BASE_PAD is 2 048 px at every level, which reaches into the enemy base on a
   small map -- the enemy commander stood 2 674..4 767 px from ImpAI's anchor at the cap in the 16 Pincushion games
   of b7 r1's 50 cap games, and 2 629..3 380 px in the five Lava Mania ones (MEASURED 2026-11-13 from the
   recordings' s_brain and unit images) -- and the economy's outlying extractors stand beside the enemy's. KNOWN
   GAPS: an outlying structure raided by armed enemies is no base threat, and neither is an armed enemy nearer one
   of its own side's structures than the anchor -- one the enemy built inside the anchor box among them; only a
   squad that already has the contact within its reach (tasks) answers either. COST: the anchor box first, then
   for each member in it at most IMPAI_ENEMY_MAX structures, and the members and the structures are both enemy
   rows, at most IMPAI_ENEMY_MAX together. */
/* THE PRESSURE, stamped here for the economy (IMPAI_MIL.press_*; impai_econ.c, THE GOVERNOR and THE GROUND
   DEFENCES).
   - WHAT PRESSES: an army-class enemy (IMPAI_TINFO.klass), or a radar contact never identified while the
     substitute it counts as has a ground reach (sub_hits_ground, the reach the builders' ground map stamps for
     it), seen now and mobile and armed as THE BASE'S THREATS takes it, standing at the base by that rule or
     within BASE_PAD (two 256-px cells, Chebyshev) of any own structure at any distance from the anchor and nearer
     that structure than its own side's base -- the base rule's own test (nearer_anchor), the structure in the
     anchor's place, so an enemy at home beside the seat's forward extractor does not press. Each cell names its
     first own structure in census order (MDEC.ownidx).
   - THE STAMP: the pressing members' value against the seat's whole army; while the army is worth less than
     press_k times it, the stamp holds the economy's constructors for press_hold ticks, with the value and the
     pressing member nearest the anchor.
   - WHY NO ENEMY COMMANDER: its value (24 109-30 423 retail) would hold the constructors whenever it walks among
     the expansions -- MEASURED 2026-11-13, an enemy armed unit or commander came within 1 500 px of the anchor
     from GameTime 6 600 at Lava Mania's second start; the commander's safety and the hunt answer it. Builders
     never press either.
   - WHAT IT ANSWERS: raids. MEASURED 2026-11-13: 895 constructor frames killed on the yard at the bar's second
     start; 26 of 27 constructors a human's Samson line killed on Comet Catcher.
   - WHY LOCAL: a seat sees enemy mobiles only now (picture), so BARb's global army ratio has no figure here.
   RESIDUAL: a radar contact carries no type, so one aircraft on the radar -- a scout passing over an extractor --
   presses while the enemy's median seen mobile type has a ground reach; with no ground reach, or no substitute,
   no radar contact presses. COST: the marking is at most IMPAI_STRUCT_MAX x 25 cells, and each member costs O(1)
   beyond base_threats' own nearer_anchor. */
static void base_threats(MDEC* d)
{
    const int span = BASE_PAD / IMPAI_CELL_PX;
    int16_t st[IMPAI_ENEMY_MAX];
    int k, nst = 0, press_base = 0;
    const MENEMY* pm = NULL;
    uint32_t pda = 0;
    float P = 0.0f, own_v = 0.0f;
    memset(d->ownnear, 0, (size_t)d->gw * (size_t)d->gh);
    memset(d->ownidx, 0, (size_t)d->gw * (size_t)d->gh * sizeof *d->ownidx);
    for (k = 0; k < d->nown; k++) {
        const int cx = d->own[k].x / IMPAI_CELL_PX, cz = d->own[k].z / IMPAI_CELL_PX;
        const int near = dist(d->own[k].x, d->own[k].z, d->ax, d->az) <= d->plan->base_r;
        int dx, dz;
        for (dz = -span; dz <= span; dz++)
            for (dx = -span; dx <= span; dx++)
                if (cx + dx >= 0 && cz + dz >= 0 && cx + dx < d->gw && cz + dz < d->gh) {
                    const int c = (cz + dz) * d->gw + cx + dx;
                    if (near) d->ownnear[c] = 1;
                    if (!d->ownidx[c]) d->ownidx[c] = (int16_t)(k + 1);
                }
    }
    for (k = 0; k < d->nen; k++)
        if (!d->en[k].mobile && d->en[k].type != WQ_NONE) st[nst++] = (int16_t)k;
    for (k = 0; k < d->nct; k++) {
        MCONTACT* c = &d->ct[k];
        int16_t e;
        for (e = c->first; e >= 0; e = d->en[e].next) {
            MENEMY* m = &d->en[e];
            uint32_t da;
            int base;
            m->threat = (uint8_t)threat_member(d, m, st, nst);
            if (press_member(d, m, st, nst, &base)) {
                const uint32_t pa = dist(m->x, m->z, d->ax, d->az);
                P += m->value;
                if (!pm || pa < pda) {
                    pm = m;
                    pda = pa;
                    press_base = base;
                }
            }
            if (!m->threat) continue;
            da = dist(m->x, m->z, d->ax, d->az);
            if (!c->threat || da < c->near_anchor) {
                c->near_anchor = da;
                c->tx = m->x;
                c->tz = m->z;
            }
            c->threat = 1;
        }
        d->out->threats = (uint16_t)(d->out->threats + c->threat);
    }
    for (k = 0; k < d->narmy; k++) own_v += d->army[k].value;
    if (pm && P > 0.0f && own_v < d->plan->press_k * P) {
        d->st->press_until = d->now + d->plan->press_hold;
        d->st->press_v = P;
        d->st->press_bx = (int16_t)(pm->x >> 5);
        d->st->press_bz = (int16_t)(pm->z >> 5);
        d->st->press_base = (uint8_t)press_base;
        d->out->press = 1;
    }
}

/* ---- the predictor ---- */

typedef struct SIDEIN { IMPAI_FENTRY e[SIDE_IN]; unsigned n; } SIDEIN;

/* one unit of `type` at health hp_frac into side s: it joins the entry of its type and flags (IMPAI_FE_*) */
static void side_add_flags(SIDEIN* s, uint16_t type, uint16_t hp_frac, uint16_t flags)
{
    unsigned k;
    if (type == WQ_NONE) return;
    for (k = 0; k < s->n; k++)
        if (s->e[k].type == type && s->e[k].flags == flags) {
            s->e[k].hp_frac = (uint16_t)(((uint32_t)s->e[k].hp_frac * s->e[k].n + hp_frac) / (s->e[k].n + 1u));
            s->e[k].n++;
            return;
        }
    if (s->n >= SIDE_IN) return;
    s->e[s->n].type = type;
    s->e[s->n].n = 1;
    s->e[s->n].hp_frac = hp_frac;
    s->e[s->n].flags = flags;
    s->n++;
}

static void side_add(SIDEIN* s, uint16_t type, uint16_t hp_frac) { side_add_flags(s, type, hp_frac, 0); }

static uint16_t hp_frac_of(int32_t hp, uint32_t hp_max)
{
    if (!hp_max || hp <= 0) return hp_max ? 0 : 65535;
    return (uint32_t)hp >= hp_max ? 65535 : (uint16_t)((uint64_t)hp * 65535u / hp_max);
}

static uint16_t hp_frac(const MUNIT* u) { return hp_frac_of(u->hp, u->hp_max); }

/* the armed enemy structures within NEAR_DEF_PX of (x, z), seen or remembered */
static void side_defences(const MDEC* d, SIDEIN* s, int32_t x, int32_t z)
{
    int k;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* e = &d->en[k];
        if (!e->mobile && e->armed && dist(e->x, e->z, x, z) <= NEAR_DEF_PX) side_add(s, e->type, 65535);
    }
}

/* m, the predicted margin of side a against side b with their fronts `gap_px` apart: 1 when b is empty, -1 when
   a is; 0 for a prediction made, -1 when the predictor refused or the decision has spent its predictions, with *m
   untouched. The gap is the fronts' distance, a side's nearest unit
   to the other's, because the predictor tests its weapons' ranges against it, centre to centre as the engine
   does (impai_fight.c, E1). `ca`, `cb`: whether each side CLOSES, can come within range of the other on its own
   component (squad_reaches, contact_closes); a side that cannot keeps speed 0 in the model, which is what an
   unreachable side does. Sea against sea in one water body closes on both sides, the calibrated naval kind; a
   prediction with a side that cannot close has no reference kind and is counted (IMPAI_MIL_OUT.cross). */
static int predict(MDEC* d, const SIDEIN* a, const SIDEIN* b, int32_t gap_px, int ca, int cb, float* m)
{
    IMPAI_FSIDE sa, sb;
    IMPAI_FRESULT r;
    if (!b->n) { *m = 1.0f; return 0; }
    if (!a->n) { *m = -1.0f; return 0; }
    if (d->predictions >= d->plan->predict_max || d->predictions >= IMPAI_PREDICT_CAP || !d->in->fc || !d->in->fit)
        return -1;
    d->predictions++;
    d->out->predictions++;
    if (impai_fight_side(d->in->fc, a->e, a->n, ca, &sa) < 0 || impai_fight_side(d->in->fc, b->e, b->n, cb, &sb) < 0 ||
        impai_fight_predict(d->in->fc, d->in->fit, &sa, &sb, gap_px, &d->fw, &r) < 0) {
        d->out->refused_predictions++;
        return -1;
    }
    if (!ca || !cb) d->out->cross++;
    *m = r.m;
    return 0;
}

static void side_squad(const MDEC* d, int s, SIDEIN* out)
{
    int k;
    memset(out, 0, sizeof *out);
    for (k = 0; k < d->narmy; k++) if (d->army[k].sq == s) side_add(out, d->army[k].type, hp_frac(&d->army[k]));
}

static void side_contact(const MDEC* d, int c, SIDEIN* out)
{
    int16_t e;
    memset(out, 0, sizeof *out);
    for (e = d->ct[c].first; e >= 0; e = d->en[e].next)
        side_add(out, d->en[e].type != WQ_NONE ? d->en[e].type : d->sub, 65535);
    side_defences(d, out, d->ct[c].x, d->ct[c].z);
}

/* a contact's member nearest (x, z), ties to the earlier in its list; -1 for a contact of none */
static int16_t contact_nearest(const MDEC* d, int c, int32_t x, int32_t z)
{
    uint32_t best = 0xFFFFFFFFu;
    int16_t e, at = -1;
    for (e = d->ct[c].first; e >= 0; e = d->en[e].next) {
        const uint32_t y = dist(x, z, d->en[e].x, d->en[e].z);
        if (y < best) { best = y; at = e; }
    }
    return at;
}

/* the distance from a squad's centroid to a contact's nearest member */
static uint32_t to_contact(const MDEC* d, int s, int c)
{
    const int16_t e = contact_nearest(d, c, d->sq[s].x, d->sq[s].z);
    return e >= 0 ? dist(d->sq[s].x, d->sq[s].z, d->en[e].x, d->en[e].z) : 0xFFFFFFFFu;
}

/* THE FRONT GAP of squad s to the point (x, z): its member nearest the point's distance from it, held to
   INT32_MAX (a squad of no member is that far); the point stands for the other side's front (impai_fight.c,
   E1). Bounded by the army's census. */
static int32_t squad_gap(const MDEC* d, int s, int32_t x, int32_t z)
{
    uint32_t best = 0x7FFFFFFFu;
    int k;
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq == s) {
            const uint32_t y = dist(d->army[k].x, d->army[k].z, x, z);
            if (y < best) best = y;
        }
    return (int32_t)best;
}

/* ---- keeping to the layer: every point a member is sent to, tested against the reach labels ---- */

/* a range the threat table gives, as data: held to 0..RANGE_MAX */
static int32_t range_held(int32_t range) { return range < 0 ? 0 : range > RANGE_MAX ? RANGE_MAX : range; }

/* 1 when a unit of `type` at (ux, uz) px can travel to (x, z): the labels of its class at the two blocks are
   equal. No test, 1, for a VTOL, a class the level has no labels for, or a unit on a block of label 0. */
static int unit_reaches(const MDEC* d, unsigned type, int32_t ux, int32_t uz, int32_t x, int32_t z)
{
    const IMPAI_TINFO* t;
    int a;
    if (!d->in->reach || type >= d->in->nti) return 1;
    t = &d->in->ti[type];
    if ((t->flags & IMPAI_TF_VTOL) || (a = impai_reach_at(d->in->reach, t->mclass, ux >> 5, uz >> 5)) <= 0) return 1;
    return impai_reach_at(d->in->reach, t->mclass, x >> 5, z >> 5) == a;
}

/* THE SUBSTITUTE POINT for a unit of `type` at (ux, uz) sent to (*x, *z): 1 with the point unchanged when the unit
   reaches it; else the centre of the block nearest the point, in ring order within SUB_BLOCKS rings, that holds
   the unit's own component (impai_reach_near), 1; 0 when there is none. Keyed by (class, label, the point's
   block) and cached, found or not, so a squad sent to one point costs one spiral a class. BOUND: at most
   SUB_CACHE spirals a decision, each at most (2 SUB_BLOCKS + 1)^2 = 4225 label reads; with the cache full the
   answer is 0. */
static int point_for(MDEC* d, unsigned type, int32_t ux, int32_t uz, int32_t* x, int32_t* z)
{
    const IMPAI_REACH* r = d->in->reach;
    unsigned cls;
    int label, bx, bz, k, ox, oz;
    MSUB* m;
    if (unit_reaches(d, type, ux, uz, *x, *z)) return 1;
    cls = d->in->ti[type].mclass;
    label = impai_reach_at(r, cls, ux >> 5, uz >> 5);
    bx = *x >> 5;
    bz = *z >> 5;
    for (k = 0; k < d->nsub_pt; k++) {
        m = &d->sub_pt[k];
        if (m->cls == cls && m->label == label && m->bx == bx && m->bz == bz) break;
    }
    if (k == d->nsub_pt) {
        if (d->nsub_pt >= SUB_CACHE) return 0;
        m = &d->sub_pt[d->nsub_pt++];
        m->cls = (uint8_t)cls;
        m->label = (uint16_t)label;
        m->bx = (int16_t)bx;
        m->bz = (int16_t)bz;
        m->found = (uint8_t)impai_reach_near(r, cls, (uint16_t)label, bx, bz, SUB_BLOCKS, &ox, &oz);
        m->sx = (int16_t)(m->found ? ox : 0);
        m->sz = (int16_t)(m->found ? oz : 0);
    }
    m = &d->sub_pt[k];
    if (!m->found) return 0;
    *x = m->sx * 32 + 16;
    *z = m->sz * 32 + 16;
    return 1;
}

/* the point of the k-th member of a squad travelling to (x, z): a 5 x 5 grid SPREAD_PX apart */
static void spread(int k, int32_t* x, int32_t* z)
{
    *x += ((k % 5) - 2) * SPREAD_PX;
    *z += (((k / 5) % 5) - 2) * SPREAD_PX;
}

/* 1 when army unit u is a damaged member that steps back (micro_hp) rather than fights */
static int damaged(const MDEC* d, const MUNIT* u)
{
    return d->plan->micro_hp > 0.0f && u->hp_max && (float)u->hp < d->plan->micro_hp * (float)u->hp_max;
}

/* the point travel gives u, the n-th member of a squad sent to (x, z): its spread point when it reaches it
   (unit_reaches), else the point itself, which move_to substitutes (point_for) */
static void travel_point(const MDEC* d, const MUNIT* u, int n, int32_t x, int32_t z, int32_t* px, int32_t* pz)
{
    *px = x;
    *pz = z;
    spread(n, px, pz);
    if (unit_reaches(d, u->type, u->x, u->z, *px, *pz)) return;
    *px = x;
    *pz = z;
}

/* THE ARRIVAL of squad s sent to (x, z). A member has arrived when it stands within `r` px of the point travel
   gives it -- its spread point, or the point or the substitute move_to gives it (point_for) -- or when travel
   gives it none: no substitute, or, after a fight that ordered no attack (`fought`), a damaged member, which is
   fight's. Each member is held to the point it is actually sent to, so a squad whose members stop at substitutes
   off the point (a sea squad at a coast target) has arrived where they stop. The members are counted in travel's
   order, so the spread points are travel's. Into *at the arrived members' value and *total the squad's, each
   summed in the members' order; returns 1 when every member has arrived. COST: point_for's, whose cache travel
   then reads. */
static int arrived(MDEC* d, int s, int32_t x, int32_t z, uint32_t r, int fought, float* at, float* total)
{
    int k, n = 0, all = 1;
    *at = *total = 0.0f;
    for (k = 0; k < d->narmy; k++) {
        const MUNIT* u = &d->army[k];
        int32_t px, pz;
        if (u->sq != s) continue;
        *total += u->value;
        if (fought && damaged(d, u)) {
            n++;
            *at += u->value;
            continue;
        }
        travel_point(d, u, n++, x, z, &px, &pz);
        if (!point_for(d, u->type, u->x, u->z, &px, &pz) || dist(u->x, u->z, px, pz) <= r) *at += u->value;
        else all = 0;
    }
    return all;
}

/* 1 when ATTACK squad s has arrived at (x, z): members holding at least half its value within ARRIVE_PX of the
   points they are sent to (arrived), so a straggler the engine cannot bring up does not hold the wave there */
static int attack_arrived(MDEC* d, int s, int32_t x, int32_t z, int fought)
{
    float at, total;
    arrived(d, s, x, z, ARRIVE_PX, fought, &at, &total);
    return 2.0f * at >= total;
}

/* THE REACH TABLE of one squad test: whether class `cls`'s component `label` has a block within the test's
   radius of its point, NEAR_KEYS of them computed (squad_reaches) */
typedef struct NEARKEY { uint16_t label; uint8_t cls, ok; } NEARKEY;

/* 1 when at least half of squad s's value can come within range of (x, z): a member reaches by its class's label,
   or by a block of its own component within min(the squad's range, RANGE_MAX) / 32 blocks of the point
   (impai_reach_near), so a sea wave goes for a coast target it can shell. A member with no test (unit_reaches)
   reaches. BOUND: NEAR_KEYS (class, label) pairs are computed, each at most (2 RANGE_MAX / 32 + 1)^2 label reads;
   a member past them counts as not reaching. */
static int squad_reaches(const MDEC* d, int s, int32_t x, int32_t z)
{
    const IMPAI_REACH* r = d->in->reach;
    const int radius = range_held(d->sq[s].range) / 32;
    NEARKEY key[NEAR_KEYS];
    float total = 0.0f, near = 0.0f;
    int k, j, nkey = 0, ox, oz;
    for (k = 0; k < d->narmy; k++) {
        const MUNIT* u = &d->army[k];
        unsigned cls;
        int label;
        if (u->sq != s) continue;
        total += u->value;
        if (unit_reaches(d, u->type, u->x, u->z, x, z)) {
            near += u->value;
            continue;
        }
        cls = d->in->ti[u->type].mclass;
        label = impai_reach_at(r, cls, u->x >> 5, u->z >> 5);
        for (j = 0; j < nkey && !(key[j].cls == cls && key[j].label == label); j++) {}
        if (j == nkey) {
            if (nkey >= NEAR_KEYS) continue;
            key[nkey].cls = (uint8_t)cls;
            key[nkey].label = (uint16_t)label;
            key[nkey].ok = (uint8_t)impai_reach_near(r, cls, (uint16_t)label, x >> 5, z >> 5, radius, &ox, &oz);
            nkey++;
        }
        if (key[j].ok) near += u->value;
    }
    return 2.0f * near >= total;
}

/* 1 when contact member e can come within range of squad s: its type (d->sub for one never identified) reaches,
   by its class's label or within min(its threat range, RANGE_MAX) of a block of its own component, the block of
   s's member nearest it. No test, 1, for no type, a VTOL, or a class with no labels. */
static int contact_closes(const MDEC* d, int16_t e, int s)
{
    const IMPAI_REACH* r = d->in->reach;
    const MENEMY* m = &d->en[e];
    const unsigned type = m->type != WQ_NONE ? m->type : d->sub;
    const IMPAI_VIEW* v = d->in->view;
    uint32_t best = 0xFFFFFFFFu;
    int32_t mx = m->x, mz = m->z;
    int k, label, ox, oz;
    unsigned cls;
    if (!r || type == WQ_NONE || type >= d->in->nti || (d->in->ti[type].flags & IMPAI_TF_VTOL)) return 1;
    cls = d->in->ti[type].mclass;
    if ((label = impai_reach_at(r, cls, m->x >> 5, m->z >> 5)) <= 0) return 1;
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq == s) {
            const uint32_t y = dist(d->army[k].x, d->army[k].z, m->x, m->z);
            if (y < best) { best = y; mx = d->army[k].x; mz = d->army[k].z; }
        }
    if (impai_reach_at(r, cls, mx >> 5, mz >> 5) == label) return 1;
    return impai_reach_near(r, cls, (uint16_t)label, mx >> 5, mz >> 5,
                            range_held(v && v->threat && type < (unsigned)v->ntypes ? v->threat[type].range : 0) / 32,
                            &ox, &oz);
}

/* ---- 4. the squads' tasks ---- */

/* the enemy structure of most value within TARGET_PX of the estimate, seen or remembered; -1 none */
static int wave_target(const MDEC* d)
{
    const int32_t ex = d->st->ebx * 32 + 16, ez = d->st->ebz * 32 + 16;
    int k, best = -1;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* e = &d->en[k];
        if (e->mobile || dist(e->x, e->z, ex, ez) > TARGET_PX) continue;
        if (best < 0 || e->value > d->en[best].value || (e->value == d->en[best].value && e->slot < d->en[best].slot))
            best = k;
    }
    return best;
}

/* the seat still knows an enemy at the slot a task goes for */
static int target_known(const MDEC* d, uint16_t slot)
{
    int k;
    if (slot == WQ_NONE) return 0;
    for (k = 0; k < d->nen; k++) if (d->en[k].slot == slot) return 1;
    return 0;
}

/* THE SWEEP'S GOAL for squad s: of the IMPAI_SWEEP cells whose centre members holding at least half the squad's
   value reach, the one an army unit of the seat stood in least recently (IMPAI_MIL.swept), ties to the nearest
   (nx, nz), then to the lowest index; its centre into (*x, *z), 1, or 0 when there is none. A member reaches a
   centre as unit_reaches says: by its class's label at the two blocks, or with no test (a VTOL, a level with no
   labels, a member on a block of label 0) at every centre. The members' (class, label) pairs are gathered once,
   at most NEAR_KEYS of them, and a member past them reaches no centre. INVARIANT: squad_reaches holds at the goal
   by construction -- it counts every member counted here, by the same test, first, and both sum the members'
   values in the members' order, so its sum is never the smaller (round-to-nearest is monotone) -- and the goal
   needs no second test. So a mixed squad goes where its army goes, not where its first member alone goes.
   BOUND: IMPAI_SWEEP cells, each at most NEAR_KEYS label reads and the squad's members. */
static int sweep_goal(const MDEC* d, int s, int32_t nx, int32_t nz, int32_t* x, int32_t* z)
{
    const IMPAI_REACH* r = d->in->reach;
    enum { KEY_ALWAYS = 0xFE, KEY_NONE = 0xFF };
    int16_t mem[IMPAI_ARMY_MAX];
    uint8_t mkey[IMPAI_ARMY_MAX], ok[NEAR_KEYS];
    NEARKEY key[NEAR_KEYS];
    float total = 0.0f;
    uint32_t bd = 0;
    int k, j, c, nm = 0, nkey = 0, best = -1;
    for (k = 0; k < d->narmy; k++) {
        const MUNIT* u = &d->army[k];
        const IMPAI_TINFO* t = &d->in->ti[u->type];
        int label;
        if (u->sq != s) continue;
        total += u->value;
        mem[nm] = (int16_t)k;
        if (!r || (t->flags & IMPAI_TF_VTOL) || (label = impai_reach_at(r, t->mclass, u->x >> 5, u->z >> 5)) <= 0) {
            mkey[nm++] = KEY_ALWAYS;
            continue;
        }
        for (j = 0; j < nkey && !(key[j].cls == t->mclass && key[j].label == label); j++) {}
        if (j == nkey && nkey < NEAR_KEYS) {
            key[nkey].cls = (uint8_t)t->mclass;
            key[nkey].label = (uint16_t)label;
            nkey++;
        }
        mkey[nm++] = (uint8_t)(j < nkey ? j : KEY_NONE);
    }
    for (c = 0; c < IMPAI_SWEEP; c++) {
        int32_t cx, cz;
        uint32_t y;
        float at = 0.0f;
        if (best >= 0 && d->st->swept[c] > d->st->swept[best]) continue;
        sweep_centre(d, c, &cx, &cz);
        y = dist(cx, cz, nx, nz);
        if (best >= 0 && d->st->swept[c] == d->st->swept[best] && y >= bd) continue;
        for (j = 0; j < nkey; j++) ok[j] = (uint8_t)(impai_reach_at(r, key[j].cls, cx >> 5, cz >> 5) == key[j].label);
        for (k = 0; k < nm; k++)
            if (mkey[k] == KEY_ALWAYS || (mkey[k] != KEY_NONE && ok[mkey[k]])) at += d->army[mem[k]].value;
        if (!(2.0f * at >= total)) continue;
        best = c;
        bd = y;
    }
    if (best < 0) return 0;
    sweep_centre(d, best, x, z);
    return 1;
}

/* THE WAVE'S GOAL, chosen when a wave forms (form_attack), again at each decision for each ATTACK squad (tasks),
   and when an ATTACK has arrived with nothing to attack (move_on): the first of these that holds and that the
   squad reaches (squad_reaches, so no land wave goes across a strait and a sea wave goes for a coast target it can
   shell), at most three reach tests and the sweep's own:
   1. an enemy commander the seat sees now, the nearest the squad: its block, its slot;
   2. a foe's commander remembered (IMPAI_MIL.foe), last seen within IMPAI_FOE_FRESH ticks in a sweep cell no army
      unit of the seat has stood in since: the nearest the squad, its block, no slot. Never the seat's own or an
      ally's entry, which the memory never writes but a recorded table may hold;
   3. the enemy structure of most value within TARGET_PX of the estimate, seen or remembered (wave_target);
   4. the sweep (sweep_goal) for the squad, ties to the nearest the estimate (the squad's centroid with none), so
      with nothing known it starts at the enemy's start position and spreads out from it.
   THE COMMANDER COMES FIRST because it is the win condition of a commander-death game: nothing else ends one, and
   a wave that razes structures leaves the commander to rebuild them. NO CHEATING: 1 reads rows the seat sees now,
   2 the seat's own memory of them, 3 its rows and ghosts, 4 its own army's positions. 1 with the goal in (*x, *z)
   and *target, or 0 with none, the outputs untouched. BOUNDS: en[] below nen, foe[] below WQ_SEATS, swept[] below
   IMPAI_SWEEP. 0 means no cell holds members worth half the squad on one component; the caller keeps the point it
   had, and an ATTACK that arrives there moves on or goes back to FORM (move_on). */
static int wave_goal(const MDEC* d, int s, int32_t* x, int32_t* z, uint16_t* target)
{
    const IMPAI_VIEW* v = d->in->view;
    const int est = d->st->ebx != IMPAI_NO_ANCHOR;
    const int32_t ex = est ? d->st->ebx * 32 + 16 : d->sq[s].x, ez = est ? d->st->ebz * 32 + 16 : d->sq[s].z;
    int32_t gx, gz;
    uint32_t bd = 0;
    int k, best = -1;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* e = &d->en[k];
        uint32_t y;
        if (!e->now || e->type == WQ_NONE || d->in->ti[e->type].cat != IMPAI_C_COMMANDER) continue;
        y = dist(e->x, e->z, d->sq[s].x, d->sq[s].z);
        if (best < 0 || y < bd) { best = k; bd = y; }
    }
    if (best >= 0 && squad_reaches(d, s, d->en[best].x, d->en[best].z)) {
        *x = d->en[best].x;
        *z = d->en[best].z;
        *target = d->en[best].slot;
        return 1;
    }
    best = -1;
    for (k = 0; k < WQ_SEATS; k++) {
        /* slot 0 holds no unit (the rows start at 1), so a zeroed memory remembers nothing */
        const IMPAI_FOE* f = &d->st->foe[k];
        const int32_t fx = f->bx * 32 + 16, fz = f->bz * 32 + 16;
        uint32_t y;
        if (f->slot == WQ_NONE || !f->slot || d->now < f->t || d->now - f->t > IMPAI_FOE_FRESH ||
            d->st->swept[sweep_cell(d, fx, fz)] > (f->t >> 6) || (v && (k == v->seat || ((v->allies >> k) & 1u))))
            continue;
        y = dist(fx, fz, d->sq[s].x, d->sq[s].z);
        if (best < 0 || y < bd) { best = k; bd = y; }
    }
    if (best >= 0 && squad_reaches(d, s, d->st->foe[best].bx * 32 + 16, d->st->foe[best].bz * 32 + 16)) {
        *x = d->st->foe[best].bx * 32 + 16;
        *z = d->st->foe[best].bz * 32 + 16;
        *target = (uint16_t)WQ_NONE;
        return 1;
    }
    if (est && (k = wave_target(d)) >= 0 && squad_reaches(d, s, d->en[k].x, d->en[k].z)) {
        *x = d->en[k].x;
        *z = d->en[k].z;
        *target = d->en[k].slot;
        return 1;
    }
    if (!sweep_goal(d, s, ex, ez, &gx, &gz)) return 0;
    *x = gx;
    *z = gz;
    *target = (uint16_t)WQ_NONE;
    return 1;
}

static void form_attack(MDEC* d, int s)
{
    SIDEIN a, b;
    float m;
    int k;
    int32_t tx, tz;
    uint16_t t;
    if (d->st->ebx == IMPAI_NO_ANCHOR || d->v <= 0.0f ||
        d->sq[s].value < (d->plan->wave_value + (float)d->st->waves) * d->v || !wave_goal(d, s, &tx, &tz, &t) ||
        attack_arrived(d, s, tx, tz, 0))
        return;
    side_squad(d, s, &a);
    memset(&b, 0, sizeof b);
    for (k = 0; k < d->nen; k++)
        if (d->en[k].now && d->en[k].mobile) side_add(&b, d->en[k].type != WQ_NONE ? d->en[k].type : d->sub, 65535);
    side_defences(d, &b, tx, tz);
    if (predict(d, &a, &b, squad_gap(d, s, tx, tz), 1, 1, &m) < 0 ||
        !(m > d->plan->attack_margin))
        return;
    /* past IMPAI_ATTACKS_MAX the wave reinforces an ATTACK squad of its layer (become); a wave all the same */
    if ((k = become(d, s, IMPAI_SQ_ATTACK, tx, tz, t)) < 0) return;
    d->st->sq[k].value = d->sq[k].value;
    d->st->waves++;
    d->st->wave_t = d->now;
    d->out->attacks++;
}

/* RAIDS: while fewer than IMPAI_RAIDS_MAX raid squads go, up to RAID_MAX units of the
   fastest type of a land FORM squad but its scouts, at least RAID_MIN, go for the seen or remembered enemy
   extractor or energy building of least threat on its cell of the ground threat map (ties to the nearer, then the
   lower slot) that the first listed unit of the type reaches (unit_reaches) */
static void raid(MDEC* d)
{
    int k, raids = 0, form = -1, best = -1, s, n = 0, first = -1;
    uint32_t best_threat = 0;
    uint16_t fast = 0;
    int32_t fast_speed = 0;
    for (k = 0; k < IMPAI_SQUADS; k++) {
        raids += d->st->sq[k].task == IMPAI_SQ_RAID;
        if (d->st->sq[k].task == IMPAI_SQ_FORM && d->st->sq[k].layer == TASIM_LAYER_LAND) form = k;
    }
    if (raids >= IMPAI_RAIDS_MAX || op_full(d) || form < 0 || !d->in->view || !d->in->view->threat)
        return;
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq == form && d->army[k].klass != IMPAI_K_LSCOUT && d->army[k].speed > fast_speed) {
            fast_speed = d->army[k].speed;
            fast = d->army[k].type;
        }
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq == form && d->army[k].type == fast) {
            if (first < 0) first = k;
            n++;
        }
    if (n < RAID_MIN) return;
    if (!d->maps_built) d->maps_built = impai_maps_build(d->in->view, &d->maps, WQ_NONE) ? 1 : -1;
    if (d->maps_built < 0) return;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* e = &d->en[k];
        int cx, cz;
        uint32_t th;
        if (e->mobile || e->type == WQ_NONE) continue;
        if (d->in->ti[e->type].cat != IMPAI_C_MEX && d->in->ti[e->type].cat != IMPAI_C_ENERGY &&
            d->in->ti[e->type].cat != IMPAI_C_GEO)
            continue;
        if (!unit_reaches(d, fast, d->army[first].x, d->army[first].z, e->x, e->z)) continue;
        cx = e->x / IMPAI_CELL_PX;
        cz = e->z / IMPAI_CELL_PX;
        th = cx < d->maps.gw && cz < d->maps.gh ? d->maps.threat[IMPAI_THREAT_GROUND][cz * d->maps.gw + cx] : 0;
        if (best < 0 || th < best_threat ||
            (th == best_threat && dist(e->x, e->z, d->sq[form].x, d->sq[form].z) <
                                  dist(d->en[best].x, d->en[best].z, d->sq[form].x, d->sq[form].z))) {
            best = k;
            best_threat = th;
        }
    }
    if (best < 0) return;
    for (s = 0; s < IMPAI_SQUADS && d->st->sq[s].task != IMPAI_SQ_FREE; s++) {}
    if (s == IMPAI_SQUADS) return;
    d->st->sq[s].layer = TASIM_LAYER_LAND;
    /* never leaves members in a squad that holds no task: THE ATTENTION was tested above, and any refusal ends
       the raid before a member joins */
    if (become(d, s, IMPAI_SQ_RAID, d->en[best].x, d->en[best].z, d->en[best].slot) != s) return;
    for (k = 0, n = 0; k < d->narmy && n < RAID_MAX; k++)
        if (d->army[k].sq == form && d->army[k].type == fast) {
            d->sq[form].n--;
            d->sq[form].value -= d->army[k].value;
            d->sq[form].sx -= d->army[k].x;
            d->sq[form].sz -= d->army[k].z;
            join(d, &d->army[k], s);
            d->sq[s].n++;
            d->sq[s].value += d->army[k].value;
            d->sq[s].sx += d->army[k].x;
            d->sq[s].sz += d->army[k].z;
            if (d->army[k].range > d->sq[s].range) d->sq[s].range = d->army[k].range;
            d->sq[s].speed = d->army[k].speed;
            n++;
        }
    d->sq[s].x = (int32_t)(d->sq[s].sx / d->sq[s].n);
    d->sq[s].z = (int32_t)(d->sq[s].sz / d->sq[s].n);
    d->sq[s].contact = -1;
    d->st->sq[s].n = (uint8_t)n;
    d->out->raids++;
}

/* The next scouting point of a unit of `type` at (ux, uz): start positions round the list from scout_next, the
   seat's own and every one the unit does not reach (unit_reaches) skipped, at most nstarts looks; -1 for none,
   scout_next unchanged. BOUND: scout_next is recorded state, taken modulo the count before it indexes the list. */
static int next_point(MDEC* d, unsigned type, int32_t ux, int32_t uz)
{
    int n;
    for (n = 0; n < d->nstarts; n++) {
        const int k = (d->st->scout_next % d->nstarts + n) % d->nstarts;
        if (k == d->own_start || !unit_reaches(d, type, ux, uz, d->starts[k].x, d->starts[k].z)) continue;
        d->st->scout_next = (uint8_t)((k + 1) % d->nstarts);
        return k;
    }
    return -1;
}

/* the start positions other than the seat's own that a unit of `type` at (ux, uz) reaches (unit_reaches) */
static int starts_reached(const MDEC* d, unsigned type, int32_t ux, int32_t uz)
{
    int k, n = 0;
    for (k = 0; k < d->nstarts; k++)
        n += k != d->own_start && unit_reaches(d, type, ux, uz, d->starts[k].x, d->starts[k].z);
    return n;
}

/* SCOUTING: every scout_period, or while the estimate rests on no enemy structure, and with no scout out: the
   dearest scout-class unit of a FORM squad, else its cheapest unit among the fastest quarter of speeds, goes as a
   squad of one to the start positions other than the seat's own, from scout_next on, round the list: the one
   it visited least recently first, by construction, of those it reaches; with none it does not go. Once its legs
   have visited every start it reaches, the next leg is the sweep's cell (sweep_goal) with ties to the nearest the
   scout. It ends at the third point, at a contact, or with no point left it reaches. */
static void scouting(MDEC* d)
{
    int k, s, pick = -1, fastest = 0;
    for (k = 0; k < IMPAI_SQUADS; k++) if (d->st->sq[k].task == IMPAI_SQ_SCOUT) return;
    if (d->nstarts < 2 || (d->st->scout_t && d->now - d->st->scout_t < d->plan->scout_period && d->est_struct)) return;
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq < IMPAI_SQUADS && d->st->sq[d->army[k].sq].task == IMPAI_SQ_FORM &&
            d->army[k].layer == TASIM_LAYER_LAND && d->army[k].speed > fastest)
            fastest = d->army[k].speed;
    for (k = 0; k < d->narmy; k++) {
        const MUNIT* u = &d->army[k];
        if (u->sq >= IMPAI_SQUADS || d->st->sq[u->sq].task != IMPAI_SQ_FORM || u->layer != TASIM_LAYER_LAND) continue;
        if (u->klass == IMPAI_K_LSCOUT) {
            if (pick < 0 || d->army[pick].klass != IMPAI_K_LSCOUT || u->value > d->army[pick].value) pick = k;
        } else if (u->speed * 4 >= fastest * 3 && (pick < 0 || (d->army[pick].klass != IMPAI_K_LSCOUT &&
                                                                 u->value < d->army[pick].value))) {
            pick = k;
        }
    }
    if (pick < 0) return;
    for (s = 0; s < IMPAI_SQUADS && d->st->sq[s].task != IMPAI_SQ_FREE; s++) {}
    if (s == IMPAI_SQUADS || (k = next_point(d, d->army[pick].type, d->army[pick].x, d->army[pick].z)) < 0) return;
    {
        MUNIT* u = &d->army[pick];
        const int from = u->sq;
        d->sq[from].n--;
        d->sq[from].value -= u->value;
        d->sq[from].sx -= u->x;
        d->sq[from].sz -= u->z;
        d->st->sq[s].layer = TASIM_LAYER_LAND;
        become(d, s, IMPAI_SQ_SCOUT, d->starts[k].x, d->starts[k].z, (uint16_t)WQ_NONE);
        join(d, u, s);
        d->sq[s].n = 1;
        d->sq[s].value = u->value;
        d->sq[s].x = u->x;
        d->sq[s].z = u->z;
        d->sq[s].range = u->range;
        d->sq[s].speed = u->speed;
        d->sq[s].contact = -1;
        d->st->sq[s].n = 1;
    }
    d->st->scout_t = d->now;
    d->out->scouts++;
}

static void scout_step(MDEC* d, int s)
{
    IMPAI_SQUAD* q = &d->st->sq[s];
    int32_t x, z;
    int k, m;
    if (dist(d->sq[s].x, d->sq[s].z, q->tx * 32 + 16, q->tz * 32 + 16) > ARRIVE_PX) return;
    for (m = 0; m < d->narmy && d->army[m].sq != s; m++) {}
    if (++q->flags >= SCOUT_LEGS || d->nstarts < 2 || m == d->narmy) {
        back_to_form(d, s);
        return;
    }
    {
        const MUNIT* u = &d->army[m];
        if (q->flags < starts_reached(d, u->type, u->x, u->z)) {
            if ((k = next_point(d, u->type, u->x, u->z)) < 0) {
                back_to_form(d, s);
                return;
            }
            x = d->starts[k].x;
            z = d->starts[k].z;
        } else if (!sweep_goal(d, s, u->x, u->z, &x, &z)) {
            back_to_form(d, s);
            return;
        }
    }
    q->tx = (int16_t)(x >> 5);
    q->tz = (int16_t)(z >> 5);
}

/* THE TASK'S LIFETIME: a RAID, RETREAT or SCOUT squad, and a DEFEND squad with no contact in its reach, goes back
   to FORM once now - since passes task_timeout + its travel, so task_timeout is the plan's time AT the task's
   point. The travel is TRAVEL_K times the straight line from the rally point to the task's point at the squad's
   slowest member's maxvelocity (MSQ.speed, 16.16 px a tick), held to TRAVEL_CAP; a speed of 0 takes TRAVEL_CAP.
   INVARIANT, a bound on a recorded tick difference: after every decision no RAID, RETREAT or SCOUT squad, nor a
   DEFEND squad with no contact in its reach, has held its task longer than task_timeout + TRAVEL_CAP (every squad
   is looked at in every decision; a DEFEND in a fight stays in it), so no point it cannot reach parks it -- a sea
   RETREAT whose rally point is on land, a SCOUT leg it never arrives at, a DEFEND whose threat stays out of its
   reach. Counted from the launch alone, the timeout recalled the slow half of a wave before it crossed the map: at
   0.8 px a tick (CORLEVLR) Pin Cushion's 2 913 px take 3 641 ticks against Hard's 2 700, Trout Farm's 4 742 px
   5 928 against Easy's 5 400 (b7 r1, from the FBIs' maxvelocity). An ATTACK has no lifetime: its goal is chosen
   again at every decision among the points the squad reaches (wave_goal), and it ends by retreat or on arriving
   with nothing to attack and no goal it has not arrived at (move_on), so no point parks it either. */
#define TRAVEL_K   2
#define TRAVEL_CAP 16200        /* ticks, nine game minutes [hand-written: the bar's to move, as TRAVEL_K]   */
static uint32_t travel_ticks(const MDEC* d, int s)
{
    const IMPAI_SQUAD* q = &d->st->sq[s];
    int64_t t;
    if (d->sq[s].speed <= 0) return TRAVEL_CAP;
    t = (int64_t)TRAVEL_K * dist(d->rx, d->rz, q->tx * 32 + 16, q->tz * 32 + 16) * 65536 / d->sq[s].speed;
    return t < TRAVEL_CAP ? (uint32_t)t : TRAVEL_CAP;
}

static int task_over(const MDEC* d, int s)
{
    return d->now - d->st->sq[s].since > (uint32_t)d->plan->task_timeout + travel_ticks(d, s);
}

/* Each squad, round-robin from next_squad. AN ATTACK squad's goal is chosen again first (wave_goal), before and
   whatever the contact test finds, so the goal follows what the seat knows. A RAID ends first, before the
   contact test, at its lifetime (THE TASK'S LIFETIME), when its target is no longer known, or on arrival with
   none; a SCOUT, a RETREAT and a DEFEND end at their lifetime too, a DEFEND with no contact in its reach, and a
   RETREAT also when every member has arrived at the rally point (arrived). Then a contact within the squad's
   reach is predicted -- below retreat_margin it falls back to the rally point, otherwise it fights -- and a FORM
   squad with none forms a wave. Every prediction comes out of the plan's predict_max; a squad left without one
   keeps its task. An ATTACK has no lifetime: it ends by retreat, or when it has arrived with nothing to attack
   and no goal it has not arrived at (orders, move_on). */
static void tasks(MDEC* d)
{
    const IMPAI_PLAN* plan = d->plan;
    int i;
    for (i = 0; i < IMPAI_SQUADS; i++) {
        const int s = (d->st->next_squad + i) % IMPAI_SQUADS;
        IMPAI_SQUAD* q = &d->st->sq[s];
        int32_t gx, gz;
        uint16_t gt;
        int c, best = -1;
        uint32_t bd = 0;
        if (q->task == IMPAI_SQ_FREE || !d->sq[s].n) continue;
        if (q->task == IMPAI_SQ_ATTACK && wave_goal(d, s, &gx, &gz, &gt)) {
            q->tx = (int16_t)(gx >> 5);
            q->tz = (int16_t)(gz >> 5);
            q->target = gt;
        }
        if (q->task == IMPAI_SQ_RAID &&
            (task_over(d, s) || (q->target != WQ_NONE && !target_known(d, q->target)) ||
             (q->target == WQ_NONE && dist(d->sq[s].x, d->sq[s].z, q->tx * 32 + 16, q->tz * 32 + 16) <= ARRIVE_PX))) {
            back_to_form(d, s);
            continue;
        }
        for (c = 0; c < d->nct; c++) {
            const uint32_t x = to_contact(d, s, c);
            if (x <= (uint32_t)d->sq[s].range + REACH_PAD && (best < 0 || x < bd)) { best = c; bd = x; }
        }
        if (q->task == IMPAI_SQ_SCOUT) {
            if (best >= 0 || task_over(d, s)) back_to_form(d, s);
            else scout_step(d, s);
            continue;
        }
        /* A RETREAT ENDS WHEN EVERY MEMBER HAS ARRIVED (arrived, RALLY_PX), not its centroid: a squad that
           retreats while its layer's RETREAT squad is out joins it (become), and a centroid there could end the
           retreat with the newcomers still in the fight. Per member rather than a second RETREAT squad until the
           newcomers arrive, because the table's arithmetic holds one RETREAT squad a layer (become). */
        if (q->task == IMPAI_SQ_RETREAT) {
            float at, total;
            if (arrived(d, s, d->rx, d->rz, RALLY_PX, 0, &at, &total) || task_over(d, s)) back_to_form(d, s);
            continue;
        }
        if (q->task == IMPAI_SQ_DEFEND && best < 0 && task_over(d, s)) {
            back_to_form(d, s);
            continue;
        }
        if (best >= 0) {
            SIDEIN a, b;
            float m;
            const int16_t e = contact_nearest(d, best, d->sq[s].x, d->sq[s].z);
            side_squad(d, s, &a);
            side_contact(d, best, &b);
            if (e >= 0 &&
                predict(d, &a, &b, squad_gap(d, s, d->en[e].x, d->en[e].z),
                        squad_reaches(d, s, d->en[e].x, d->en[e].z), contact_closes(d, e, s), &m) == 0 &&
                m < plan->retreat_margin) {
                become(d, s, IMPAI_SQ_RETREAT, d->rx, d->rz, (uint16_t)WQ_NONE);
                d->out->retreats++;
                continue;
            }
            d->sq[s].contact = best;
            continue;
        }
        if (q->task == IMPAI_SQ_FORM) form_attack(d, s);
    }
}

/* 1 when squad s can hurt threat c: the sum of the fight content's pair rates of the squad's types on the
   contact's threatening types (threat_member; a radar contact never identified as d->sub) is above 0 -- the
   predictor's own rule (impai_fight.c, hurts: anti-air slots for flyers, waterweapons for the submerged), so a
   ground squad is not pinned by aircraft it cannot shoot. BOUND: at most FOCUS_TYPES types a side; a type past
   the content counts nothing, and with no content no squad can hurt anything. */
static int squad_hurts(const MDEC* d, int s, int c)
{
    uint16_t sa[FOCUS_TYPES], sb[FOCUS_TYPES];
    int na = 0, nb = 0, i, j, k;
    int16_t e;
    float sum = 0.0f;
    for (k = 0; k < d->narmy && na < FOCUS_TYPES; k++) {
        const uint16_t t = d->army[k].type;
        if (d->army[k].sq != s || !fc_type(d, t)) continue;
        for (i = 0; i < na && sa[i] != t; i++) {}
        if (i == na) sa[na++] = t;
    }
    for (e = d->ct[c].first; e >= 0 && nb < FOCUS_TYPES; e = d->en[e].next) {
        const uint16_t t = d->en[e].type != WQ_NONE ? d->en[e].type : d->sub;
        if (!d->en[e].threat || t == WQ_NONE || !fc_type(d, t)) continue;
        for (j = 0; j < nb && sb[j] != t; j++) {}
        if (j == nb) sb[nb++] = t;
    }
    for (i = 0; i < na; i++)
        for (j = 0; j < nb; j++) {
            float r;
            int32_t range;
            if (impai_fight_rate(d->in->fc, d->in->fit, sa[i], sb[j], &r, &range) == 0) sum += r;
        }
    return sum > 0.0f;
}

/* THE BASE'S DEFENCE: a threat to the base (base_threats) that has persisted react_confirm decisions takes each
   layer's FORM squad, and every DEFEND squad, to the threat nearest the anchor that the squad can hurt
   (squad_hurts), at its threatening member nearest the anchor; a DEFEND squad with no such threat left goes back
   to FORM. */
static void defence(MDEC* d)
{
    int k, c;
    for (k = 0; k < IMPAI_SQUADS; k++) {
        IMPAI_SQUAD* q = &d->st->sq[k];
        int near = -1;
        if (q->task != IMPAI_SQ_FORM && q->task != IMPAI_SQ_DEFEND) continue;
        for (c = 0; c < d->nct; c++)
            if (d->ct[c].threat && (near < 0 || d->ct[c].near_anchor < d->ct[near].near_anchor) && squad_hurts(d, k, c))
                near = c;
        if (near < 0) {
            q->seen = 0;
            if (q->task == IMPAI_SQ_DEFEND) back_to_form(d, k);
            continue;
        }
        if (q->seen < 0xFFFF) q->seen++;
        if (q->seen < d->plan->react_confirm) continue;
        if (q->task == IMPAI_SQ_FORM) {
            const uint16_t seen = q->seen;
            const int r = become(d, k, IMPAI_SQ_DEFEND, d->ct[near].tx, d->ct[near].tz, (uint16_t)WQ_NONE);
            if (r == k) q->seen = seen;
            if (r >= 0) d->out->defends++;          /* THE ATTENTION may refuse it */
        } else {
            q->tx = (int16_t)(d->ct[near].tx >> 5);
            q->tz = (int16_t)(d->ct[near].tz >> 5);
        }
    }
}

/* ---- 5. the orders ---- */

/* can_attack's aim for a target whose observation does not identify it (SHARED, CONTACT): observe gives no
   WQ_AIM_* there, the facts being its type's and its state's, which the seat does not see */
#define AIM_HIDDEN 0x80u

/* THE TARGET IS THERE THIS TURN: 1 when the seat's own observation of this turn finds, at enemy e's slot, the
   unit e's row is about. A row is only as fresh as the line-of-sight slice's last visit of its slot, which
   covers every slot once in nslots / OBS_SLICE ticks (8 at the 15 001 slots of the raised limit), and a radar
   row as its pass, so the unit it was written for may have died since and another taken its slot; observe
   answers for whichever unit holds the slot now. What the observation tells decides:
   - an identified observation (LOS) of an identified row: the type and owner the row names. The row's owner
     is an enemy (picture), so a unit that passes is an enemy of the type the row says, which the seat sees
     now, whichever incarnation it is; a slot taken since by another type or owner -- an ally's unit, one of
     the seat's own -- fails;
   - any other: the slot's incarnation is the seat's radar record of it (WQ_RADAR_HIT, the view's hits), as the
     publish tests a contact (impai_know.c): a unit the seat's own radar found, in the published pass or in
     the one in progress, whose records are written as it finds each unit. An incarnation unknown on either
     side, or no records, matches nothing.
   A slot that holds no unit now fails (observe answers 0). The order follows in the same call on the game
   thread, so what this finds holds when the order is given; a unit pending death (+0x110 bit 14), reaped at
   its owner's next unit tick, is still observed, and an order at it is refused (wq_can). *aim is the
   observation's WQ_AIM_* where it identifies the unit, else AIM_HIDDEN; can_attack reads it. */
static int target_now(const MDEC* d, const MENEMY* e, uint8_t* aim)
{
    const IMPAI_VIEW* v = d->in->view;
    const IMPAI_SEEN* r = &v->rows[e->slot].seat[v->seat];
    const WQ_RADAR_HIT* h;
    WQ_OBS o;
    *aim = AIM_HIDDEN;
    if (d->q->ops->observe(d->q, e->slot, e->now == 2, &o) <= WQ_SEEN_OWN) return 0;
    if (o.seen == WQ_SEEN_LOS) *aim = o.aim;
    if (o.seen == WQ_SEEN_LOS && e->type != WQ_NONE) return o.type == e->type && o.owner == r->owner;
    if (!v->hits.at || e->slot >= v->hits.n || v->hits.stride < sizeof(WQ_RADAR_HIT)) return 0;
    h = wq_hit(&v->hits, e->slot);
    return h->known && o.birth_known && o.birth == h->birth;
}

/* 1 when the order resolver 0x43F0E0 answers an attack (mode 3) for army unit u on an enemy whose WQ_AIM_* is
   `aim`, its tests in its order (DISASSEMBLED, engine/orders.md, *What each resolver mode answers*):
   - no canattack (+0x245 bit 4): refused (0x43F154..0x43F15B);
   - no weapon (+0x110 bit 31): only a kamikaze (+0x241 bit 28) is answered (0x43F161..0x43F171,
     0x43F38E..0x43F39A);
   - a target not airborne, with weapon 1 toairweapon: refused (0x43F1D4..0x43F1EB);
   - a target under the sea: refused unless weapon 1, or a weapon 2, is a waterweapon (0x43F1F1..0x43F236);
   - a target not under it, for a canhover unit with either a waterweapon: refused (0x43F240..0x43F277);
   - a canfly unit whose weapon 1 is dropped, on a target whose type flies: refused (0x43F27E..0x43F31D).
   A mobile unit with a weapon is then answered (ATTACK_CHASE, 0x43F348): every army unit has a mover, army
   being mobile and mobile bmcode 1, the creates' own test for one (0x4860BB, 0x4862CF; tasim_roles.c), unless
   the mover's allocation failed. Read with the target's state of this turn (target_now) and the unit's flag
   word of this decision's census, it answers as the resolver will in the same call. A target whose aim is
   AIM_HIDDEN is taken on the unit's own two tests alone: the rest are the target's, which the seat does not
   see, so the resolver decides them when the order is asked (order: a refusal counted, no order spent). */
static int can_attack(const MUNIT* u, uint8_t aim)
{
    const unsigned a = u->attack, water = a & (IMPAI_TA_WATER1 | IMPAI_TA_WATER2);
    if (!(a & IMPAI_TA_CANATTACK)) return 0;
    if (!u->armed) return (a & IMPAI_TA_KAMIKAZE) != 0;
    if (aim & AIM_HIDDEN) return 1;
    if (!(aim & WQ_AIM_AIRBORNE) && (a & IMPAI_TA_TOAIR1)) return 0;
    if (aim & WQ_AIM_UNDER) {
        if (!water) return 0;
    } else if ((a & IMPAI_TA_HOVER) && water) {
        return 0;
    }
    return !((a & IMPAI_TA_FLY) && (a & IMPAI_TA_DROP1) && (aim & WQ_AIM_FLIES));
}

/* One order of resolver mode `mode` to army unit u, at target slot `target` or point (x, z): none when the
   decision's orders are spent, when the resolver answers 0 or -1 (counted), or when the unit's head already
   says it -- the same action and target, or a move within HEAD_NEAR_PX of the point; for the commander, not
   while an economy order to it is pending, which will replace that head (impai_brain.c, THE COMMANDER'S
   MARKERS). No node pointer is kept: the head is a copy. An order to the commander the WORLDQ takes sets the
   military's marker. 1 when an order was given, 2 when the head already says it, 0 otherwise. */
static int order(MDEC* d, uint16_t slot, int mode, uint16_t target, int32_t x, int32_t z)
{
    WQ_ORDER o;
    WQ_CMD c;
    int a, h;
    if (d->roots >= d->plan->mil_roots) return 0;
    memset(&c, 0, sizeof c);
    c.pos[0] = (int32_t)((uint32_t)x << 16);
    c.pos[2] = (int32_t)((uint32_t)z << 16);
    c.pos[1] = (int32_t)((uint32_t)(d->q->ops->ground(d->q, c.pos) & 0x7FFF) << 16);
    if ((a = d->q->ops->can(d->q, slot, mode, target, c.pos)) <= 0) {
        d->out->refused++;
        return 0;
    }
    h = d->q->ops->head(d->q, slot, &o);
    if (h == 1 && o.action == a && !(d->com && slot == d->com && d->now < d->in->brain->com_until) &&
        (mode == WQ_CAN_ATTACK || mode == WQ_CAN_DGUN
             ? o.target == target : dist(o.pos[0] >> 16, o.pos[2] >> 16, x, z) <= HEAD_NEAR_PX))
        return 2;
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)a;
    c.unit = slot;
    c.target = target;
    d->roots++;
    d->st->pulse_roots++;
    if (d->q->ops->order(d->q, &c) != WQ_OK) d->out->refused++;
    else if (d->com && slot == d->com && d->in->persona->react) d->st->com_until = d->now + d->in->persona->react;
    return 1;
}

/* A MOVE of army unit `slot`, of `type` at (ux, uz), to (x, z) or its substitute (point_for): EVERY MOVE ORDER OF
   THE MILITARY GOES THROUGH IT, so no member is sent where its class cannot travel. With no substitute the move
   is not given and is counted (IMPAI_MIL_OUT.unreached). As order: 1 given, 2 already at the head, 0 neither. */
static int move_to(MDEC* d, uint16_t slot, unsigned type, int32_t ux, int32_t uz, int32_t x, int32_t z)
{
    if (!point_for(d, type, ux, uz, &x, &z)) {
        d->out->unreached++;
        return 0;
    }
    return order(d, slot, WQ_CAN_MOVE, (uint16_t)WQ_NONE, x, z);
}

/* The candidate member u attacks: the focus where the resolver answers u for it and u has it within its range
   plus REACH_PAD, else u's nearest candidate the resolver answers it for that u has within that range or can
   travel to (unit_reaches); -1 for none. */
static int target_of(const MDEC* d, const MUNIT* u, const int16_t* cand, const uint8_t* caim, int nc, int focus,
                     int fi)
{
    uint32_t bd = 0;
    int target = -1, i;
    if (focus >= 0 && can_attack(u, caim[fi]) &&
        dist(u->x, u->z, d->en[focus].x, d->en[focus].z) <= (uint32_t)u->range + REACH_PAD)
        return focus;
    for (i = 0; i < nc; i++) {
        const uint32_t x = dist(u->x, u->z, d->en[cand[i]].x, d->en[cand[i]].z);
        if (can_attack(u, caim[i]) && (target < 0 || x < bd) &&
            (x <= (uint32_t)u->range + REACH_PAD ||
             unit_reaches(d, u->type, u->x, u->z, d->en[cand[i]].x, d->en[cand[i]].z))) {
            target = cand[i];
            bd = x;
        }
    }
    return target;
}

/* A fighting squad's orders. The candidates are the TARGETS seen-now members of its contact nearest its
   centroid that are there this turn (target_now), armed or mobile ones before the rest. Every member attacks
   the FOCUS, the candidate of least hp / sum of the squad's pair rates on it -- the quickest kill, over
   FOCUS_TYPES types, by the type's maximum health, which is all the seat knows of an enemy's -- where it
   reaches it, else its own nearest candidate. A member attacks only a candidate the
   resolver answers it for (can_attack); one with none -- an anti-air unit in a ground fight, a unit with no
   waterweapon against submarines -- keeps with the squad: it moves to the centroid when it is farther than
   SPREAD_PX from it, and is given nothing nearer, so a member that can attack nothing costs no order while it
   stands with the squad. A member attacks a candidate only where it has it within its range plus REACH_PAD or
   can travel to it (target_of, unit_reaches), so no ship is sent at a target inland and no tank at one across
   the water; its moves go through move_to. Micro: a member under micro_hp of its health steps MICRO_PX towards
   the rally point, once in micro_period, and is given nothing between two steps. Two passes over the members,
   so the decision's orders (mil_roots) go to the attacks and the steps back first and to the centroid moves
   only with what is left; the second pass runs only when some member attacks. An enemy commander among the
   candidates is the focus before any quickest kill: it is the win condition (wave_goal). Only a unit
   the seat sees now is ever ordered at. Returns the members attacking: an attack order given now, or one the
   unit's head already holds. */
static int fight(MDEC* d, int s)
{
    const MCONTACT* ct = &d->ct[d->sq[s].contact];
    int16_t cand[TARGETS];
    uint32_t cd[TARGETS];
    uint8_t caim[TARGETS];
    int nc = 0, k, focus = -1, fi = -1, attacking = 0;
    int16_t e;
    for (e = ct->first; e >= 0; e = d->en[e].next) {
        const MENEMY* m = &d->en[e];
        const uint32_t x = dist(m->x, m->z, d->sq[s].x, d->sq[s].z) + (m->armed || m->mobile ? 0u : 4096u);
        uint8_t aim;
        int i;
        if (!m->now) continue;
        if (nc == TARGETS && x >= cd[nc - 1]) continue;
        if (!target_now(d, m, &aim)) continue;
        i = nc < TARGETS ? nc++ : nc - 1;
        while (i > 0 && cd[i - 1] > x) { cand[i] = cand[i - 1]; cd[i] = cd[i - 1]; caim[i] = caim[i - 1]; i--; }
        cand[i] = e;
        cd[i] = x;
        caim[i] = aim;
    }
    if (!nc) return 0;
    for (k = 0; k < nc && focus < 0; k++) {
        const uint16_t t = d->en[cand[k]].type;
        if (t != WQ_NONE && t < d->in->nti && d->in->ti[t].cat == IMPAI_C_COMMANDER) {
            focus = cand[k];
            fi = k;
        }
    }
    if (focus < 0 && d->in->fc && d->in->fc->nt) {
        uint16_t types[FOCUS_TYPES];
        float best = 0.0f;
        int nt = 0, i, j;
        for (i = 0; i < nc; i++) {
            const uint16_t t = d->en[cand[i]].type;
            for (j = 0; j < nt && types[j] != t; j++) {}
            if (j == nt && nt < FOCUS_TYPES && t != WQ_NONE && fc_type(d, t)) types[nt++] = t;
        }
        for (j = 0; j < nt; j++) {
            float sum = 0.0f;
            for (k = 0; k < d->narmy; k++) {
                float r;
                int32_t range;
                if (d->army[k].sq != s || !fc_type(d, d->army[k].type)) continue;
                if (impai_fight_rate(d->in->fc, d->in->fit, d->army[k].type, types[j], &r, &range) == 0) sum += r;
            }
            if (sum > 0.0f) {
                const float kill = (float)d->in->fc->t[types[j]].hp / sum;
                if (focus < 0 || kill < best) {
                    best = kill;
                    for (i = 0; i < nc && d->en[cand[i]].type != types[j]; i++) {}
                    focus = cand[i];
                    fi = i;
                }
            }
        }
    }
    for (k = 0; k < d->narmy && d->roots < d->plan->mil_roots; k++) {
        MUNIT* u = &d->army[k];
        IMPAI_JOB* j = &d->in->job[u->slot];
        int target;
        if (u->sq != s) continue;
        if (damaged(d, u)) {
            /* a damaged member steps back, and is left on its way between two steps */
            if (!j->since || d->now - j->since >= d->plan->micro_period) {
                const uint32_t dr = dist(u->x, u->z, d->rx, d->rz);
                int32_t x = d->rx, z = d->rz;
                if (dr > MICRO_PX) {
                    x = u->x + (int32_t)((int64_t)(d->rx - u->x) * MICRO_PX / dr);
                    z = u->z + (int32_t)((int64_t)(d->rz - u->z) * MICRO_PX / dr);
                }
                if (move_to(d, u->slot, u->type, u->x, u->z, x, z) == 1) j->since = d->now;
            }
            continue;
        }
        if ((target = target_of(d, u, cand, caim, nc, focus, fi)) >= 0 &&
            order(d, u->slot, WQ_CAN_ATTACK, d->en[target].slot, d->en[target].x, d->en[target].z) > 0)
            attacking++;
    }
    for (k = 0; k < d->narmy && d->roots < d->plan->mil_roots && attacking; k++) {
        const MUNIT* u = &d->army[k];
        if (u->sq != s || damaged(d, u) || target_of(d, u, cand, caim, nc, focus, fi) >= 0 ||
            dist(u->x, u->z, d->sq[s].x, d->sq[s].z) <= SPREAD_PX)
            continue;
        move_to(d, u->slot, u->type, u->x, u->z, d->sq[s].x, d->sq[s].z);
    }
    return attacking;
}

/* A squad's members to (x, z): each to its spread point when it reaches it, else to the unspread point or its
   substitute (move_to), whose cache key is the squad's one point, so a squad costs one spiral a class. After a
   fight that ordered no attack (`fought`), a damaged member is fight's -- stepped back now, or left on its way
   between two steps -- and is given nothing here. */
static void travel(MDEC* d, int s, int32_t x, int32_t z, int fought)
{
    int k, n = 0;
    for (k = 0; k < d->narmy && d->roots < d->plan->mil_roots; k++) {
        const MUNIT* u = &d->army[k];
        int32_t px, pz;
        if (u->sq != s) continue;
        if (fought && damaged(d, u)) {
            n++;
            continue;
        }
        travel_point(d, u, n++, x, z, &px, &pz);
        move_to(d, u->slot, u->type, u->x, u->z, px, pz);
    }
}

/* AN ATTACK THAT HAS ARRIVED WITH NOTHING TO ATTACK MOVES ON (orders): the point's sweep cell is stamped as its
   own members' (sweep_stamp), so the remembered commander and the sweep's cell there are spent, and the goal is
   chosen again (wave_goal); a goal the squad has not arrived at is taken, else -- no goal, or the same place, as a
   commander it sees but cannot attack or a structure it cannot hit -- the squad goes back to FORM. INVARIANT: an
   ATTACK squad never keeps a point it has arrived at while no member attacks, so it cannot hold an ATTACK slot
   standing; and form_attack forms no wave at a point the squad has arrived at, so FORM cannot turn it straight
   back. */
static void move_on(MDEC* d, int s, int fought)
{
    IMPAI_SQUAD* q = &d->st->sq[s];
    int32_t gx, gz;
    uint16_t gt;
    d->st->swept[sweep_cell(d, q->tx * 32 + 16, q->tz * 32 + 16)] = sweep_stamp(d);
    if (wave_goal(d, s, &gx, &gz, &gt) && !attack_arrived(d, s, gx, gz, fought)) {
        q->tx = (int16_t)(gx >> 5);
        q->tz = (int16_t)(gz >> 5);
        q->target = gt;
        return;
    }
    back_to_form(d, s);
}

/* THE BUDGET (impai_react.c): one squad served is one action, a group order as a player gives it, its step-backs
   and its members' orders all part of it; a squad served with no order given costs none. With no action left the
   round robin stops at the squad, which the next decision serves first, as at the order bound. */
static void orders(MDEC* d)
{
    int i, start = d->st->next_squad % IMPAI_SQUADS, stop = -1;
    for (i = 0; i < IMPAI_SQUADS; i++) {
        const int s = (start + i) % IMPAI_SQUADS;
        const IMPAI_SQUAD* q = &d->st->sq[s];
        const int roots = d->roots;
        if (d->roots >= d->plan->mil_roots) { stop = s; break; }
        if (q->task == IMPAI_SQ_FREE || !d->sq[s].n) continue;
        if (d->acts < 1) { stop = s; break; }
        /* A SQUAD NEVER STANDS WHILE IT HAS NOTHING TO FIGHT. INVARIANT: in each decision a squad with a task
           point either has a member attacking (fight's count: ordered now or already at its head) or its members
           are sent on to its point as with no contact -- FORM and RETREAT to the rally point, DEFEND, ATTACK and
           RAID to theirs -- and an ATTACK's goal was chosen before, and whatever, the contact test found (tasks).
           So a contact holding nothing the squad can attack -- a submarine for a land squad, a target across
           water -- cannot hold a wave at the centroid. An ATTACK that has arrived at its point moves on or goes
           back to FORM first (move_on), so it is never sent to where it stands. */
        const int fought = d->sq[s].contact >= 0 && q->task != IMPAI_SQ_RETREAT && q->task != IMPAI_SQ_SCOUT;
        if (!(fought && fight(d, s) > 0)) {
            if (q->task == IMPAI_SQ_ATTACK && attack_arrived(d, s, q->tx * 32 + 16, q->tz * 32 + 16, fought))
                move_on(d, s, fought);
            if (q->task == IMPAI_SQ_FORM || q->task == IMPAI_SQ_RETREAT) travel(d, s, d->rx, d->rz, fought);
            else if (q->task != IMPAI_SQ_FREE) travel(d, s, q->tx * 32 + 16, q->tz * 32 + 16, fought);
        }
        if (d->roots > roots) d->acts--;
    }
    d->st->next_squad = (uint8_t)(stop >= 0 ? stop : (start + 1) % IMPAI_SQUADS);
}

/* ---- 7. the commander's safety ---- */

#define BLAST_PAD_PX  64        /* the commander keeps this far past a seen enemy commander's blast radius: MEASURED
                                   in the bar's round 1, 14 of 14 mutual kills with the commanders <= 484 px apart
                                   the step before, against a 475 px radius                                    */
#define LEASH_PAD_PX  512       /* past the commander's reach before the military brings it home              */
#define COM_STAY_MARGIN 0.5f    /* the margin the commander's own side must be predicted to win by for it to stand
                                   (THE STAND) [hand-written: the bar's to move]                               */
#define SPEED_HELD    (16 << 16)    /* a maxvelocity read as data: held to 16 px a tick, so THE BLAST's keep,
                                       2^20 x (H + H + age) with H at most 150 + 135 and age at most 150, stays
                                       below 2^20 x 720 < 2^31 (its sum is int64 all the same)                 */

static int32_t speed_held(int32_t s) { return s < 0 ? 0 : s > SPEED_HELD ? SPEED_HELD : s; }

/* THE COMMANDER'S JOB: every order the military gives the commander -- a flight, the leash, a D-gun -- replaces
   its list (WQ_CMD_NEW), so the economy job it held ends here with no failure, its far record kept
   (impai_job_end), as a reclaim another hand replaced does (impai_brain.c, REPLACED): the site is not failed for
   the seat, nor the commander held back from it, because the military took the commander on its way; a frame it
   began stays a building of the seat's, which the assist finishes. A squad job is the military's own, and a
   commander holds none. */
static void release_commander(MDEC* d)
{
    IMPAI_JOB* j = &d->in->job[d->com];
    if (j->kind != IMPAI_JOB_NONE && j->kind != IMPAI_JOB_SQUAD) impai_job_end(j);
}

/* THE FLIGHT from the point (tx, tz): the commander moves to whichever of the anchor and the own structures within
   comm_r of the anchor lies farthest from the point, ties to the anchor, then to the lower own[] index, and is the
   military's for SAFE_TICKS (IMPAI_MIL.safe_until, which the economy's census honours) when the move is given or
   already at its head -- so a move with no point, or one the resolver refused, leaves it to the economy. The
   destination is in the base by construction, the anchor's substitute aside (move_to; the anchor is the
   commander's own start cell, so its component's). MEASURED in the bar's round 1: with the farthest own structure
   anywhere, 94 of the 105 distinct safety moves went past comm_r, median 2 470 px, at most 6 553 px. A
   structure's point is its centre, which the commander cannot reach, so the move never ends by itself (MEASURED
   in the bar's cmdr run: it stopped 39-51 px short, p10-p90, the move holding its head); the economy's hand-back,
   not the move's end, returns it to the economy once the window lapses (impai_brain.c, THE HAND-BACK). RESIDUAL:
   a threat between the commander and every candidate is fled through. */
static int flee(MDEC* d, int32_t tx, int32_t tz)
{
    int32_t x = d->ax, z = d->az;
    uint32_t far = dist(d->ax, d->az, tx, tz);
    int k, r;
    if (d->acts < 1) return 0;
    for (k = 0; k < d->nown; k++) {
        uint32_t y;
        if (dist(d->own[k].x, d->own[k].z, d->ax, d->az) > d->plan->comm_r) continue;
        if ((y = dist(d->own[k].x, d->own[k].z, tx, tz)) > far) {
            far = y;
            x = d->own[k].x;
            z = d->own[k].z;
        }
    }
    if ((r = move_to(d, (uint16_t)d->com, d->com_type, d->com_x, d->com_z, x, z)) > 0) {
        d->st->safe_until = d->now + SAFE_TICKS;
        d->out->safe++;
        if (r == 1) {
            release_commander(d);
            d->acts--;
        }
    }
    return r;
}

static const IMPAI_TINFO* dgun_armed(MDEC* d, float* worth);
static int dgun_class(const MDEC* d, const MENEMY* m, float worth);

/* 1 when THE D-GUN's own rules could fire the commander's D-gun at a member of contact c this decision: the
   commander armed with one now (dgun_armed: a D-gun type, outside the military's window, the stored energy for
   the shot and dgun_reserve) and a member it would take as a target (dgun_class: seen now, identified -- never a
   radar contact --, worth the shot, no commander). Where the member and the commander stand is not tested --
   the range, the reach and the guard (dgun) --: the stand predicts the fight as the two sides close. */
static int dgun_could(MDEC* d, int c)
{
    float worth;
    int16_t e;
    if (!dgun_armed(d, &worth)) return 0;
    for (e = d->ct[c].first; e >= 0; e = d->en[e].next)
        if (dgun_class(d, &d->en[e], worth)) return 1;
    return 0;
}

/* THE STAND (commander, step 2): 1 when the commander, within comm_r of the anchor, and its army `army` are
   predicted to beat contact c, side b, by COM_STAY_MARGIN or more, the gap the fronts' with the commander among
   side a's members; enemy e is the contact's member nearest the commander. The commander's CommandFire weapons
   count only while its D-gun could fire at the contact this decision (dgun_could); otherwise its entry is
   flagged IMPAI_FE_NO_COMMANDFIRE and fights with its other weapons alone. A refused or spent prediction is 0. */
static int stands(MDEC* d, const SIDEIN* army, const SIDEIN* b, int c, int16_t e, uint32_t gap)
{
    SIDEIN a = *army;
    float m = -1.0f;
    const uint32_t y = dist(d->com_x, d->com_z, d->en[e].x, d->en[e].z);
    if (dist(d->com_x, d->com_z, d->ax, d->az) > d->plan->comm_r) return 0;
    side_add_flags(&a, d->com_type, hp_frac_of(d->com_hp, d->com_hp_max),
                   (uint16_t)(dgun_could(d, c) ? 0u : IMPAI_FE_NO_COMMANDFIRE));
    return predict(d, &a, b, (int32_t)(y < gap ? y : gap), 1, 1, &m) == 0 && m >= COM_STAY_MARGIN;
}

/* THE COMMANDER'S SAFETY, three steps; the first that moves the commander ends it. Run before the squads'
   orders, so that the decision's order budget cannot starve it. Everything it reads is the seat's own: its
   column of the knowledge rows (the commander's memory, IMPAI_MIL.foe, written from it in picture; the contacts)
   and its own units.
   1. THE BLAST. A commander within the blast radius of a dying commander dies with it, whoever killed which, and
      the D-gun's range (240 px, ARMCOM's and CORCOM's) is inside that radius (475 px), so no D-gun duel ends
      otherwise. Of the foes' commanders the memory holds as seen within the last look_mil ticks -- the row at
      its slot still a commander type, else the slot is another unit's now -- the nearest within its KEEP flees
      it: keep = its type's blast_r + BLAST_PAD_PX + the approach of both over THE HORIZON H = look_mil + react,
      the ticks from this decision until the next one's orders can move the commander -- the own commander's
      speed over H and the foe's over H plus the time since it was seen -- so the gap is still open when the
      military can act again. A speed is the type's maxvelocity held to SPEED_HELD; a foe type whose speed reads 0 takes the
      own commander's. RESIDUAL: a foe's commander faster than ours can follow it; other large blasts -- fusion
      plants, the Krogoth -- are not kept clear of.
   2. THE CONTACT. The contact of most value with a member within IMPAI_CMD_SAFE_PX of the commander that is armed
      or never identified is predicted: side a the army within CMD_ARMY_PX of the commander, WITHOUT the
      commander, side b the contact (side_contact), the gap the distance from e*, the contact's member nearest
      the commander, to side a's member nearest it. The commander's death ends the side, so it stays where its
      army would win alone (a margin of at least 0) or where THE STAND holds; otherwise it flees e*, and an empty
      side a predicts -1 without spending a prediction. This first prediction leaves the commander off side a
      because the fight content counts its D-gun as firing on its own (tagpu_worldq.c, the fight filler), which an
      ImpAI seat's weapon scan never does; and there is no value gate: the commander's cost (30 423 for ARMCOM,
      24 109 for CORCOM, metal + energy / 60 from the retail FBIs) was above every army contact, and the old gate
      left safe=0 in 162 of the round's 222 seat-games. RESIDUAL: past comm_r a lone armed scout still steps the
      commander back for SAFE_TICKS, and IMPAI_CMD_SAFE_PX does not scale with Easy's 150-tick decision.
      THE STAND (stands): when the army alone does not win, the commander stays where side a WITH the commander
      wins by COM_STAY_MARGIN or more, while it stands within comm_r of the anchor; otherwise it flees as above,
      from a contact that is close in the prediction as from a stronger one. The fight content fires a
      CommandFire weapon on its own, while an ImpAI seat fires it only by THE D-GUN's rule (its weapon scan runs
      with argument 0, 0x408A88..0x408A96, DISASSEMBLED: research/notes/impai/decisions.md), so the commander's
      D-gun counts only while that rule could fire it at the contact this decision -- outside the window, the
      stored energy for the shot and dgun_reserve, a member seen now, identified, worth the shot and no commander
      (dgun_could) --, and otherwise its entry fights without it (IMPAI_FE_NO_COMMANDFIRE). RESIDUAL: a counted
      D-gun fires at its reload in the prediction, where the seat orders it at one target a decision and the
      guard can refuse that target; the margin is what bounds that overrating [INFERRED]. MEASURED in
      the bar's cmdr run (65 games, the commander's head every 300 ticks): 249 of 461 flight runs began with 3 or
      fewer armed enemies within 800 px, and in ne-troutfarm-ca-2 the commander fled 1-8 raiders around its base
      for 11 000 ticks while 78 constructor frames died on its factory's yard. The second prediction counts
      against predict_max as every prediction does, so a decision with none left flees.
   3. THE LEASH, a bound on where the commander stands, whatever moved it: farther than its reach + LEASH_PAD_PX
      from the anchor -- the reach comm_r once the seat has had a factory (IMPAI_BRAIN.opened), FAR_PATCH_K x
      comm_r before, the opening's far patch -- it moves to the anchor, which holds the window as the flight
      does. The engine moves units between decisions (defaults, pushes), so between two decisions the
      commander passes the leash by at most its speed x (look_mil + react). */
static void commander(MDEC* d)
{
    const IMPAI_PLAN* plan = d->plan;
    const IMPAI_VIEW* v = d->in->view;
    const uint32_t look = d->in->persona->look_mil, H = look + d->in->persona->react;
    int32_t sp_own;
    int c, k, r, worst = -1, foe = -1;
    uint32_t foe_d = 0, reach;
    if (!d->com) return;
    sp_own = speed_held(type_speed(d, d->com_type));
    for (k = 0; k < WQ_SEATS && v; k++) {
        const IMPAI_FOE* f = &d->st->foe[k];
        const IMPAI_SEEN* r;
        uint32_t age, keep, y;
        int32_t sp;
        if (f->slot == WQ_NONE || !f->slot || f->slot >= v->nrows || d->now < f->t || (age = d->now - f->t) > look)
            continue;
        r = &v->rows[f->slot].seat[v->seat];
        if (r->type == WQ_NONE || r->type >= d->in->nti || d->in->ti[r->type].cat != IMPAI_C_COMMANDER) continue;
        if (!(sp = speed_held(type_speed(d, r->type)))) sp = sp_own;
        keep = (uint32_t)d->in->ti[r->type].blast_r + BLAST_PAD_PX +
               (uint32_t)(((int64_t)sp_own * H + (int64_t)sp * (H + age)) >> 16);
        y = dist(f->bx * 32 + 16, f->bz * 32 + 16, d->com_x, d->com_z);
        if (y <= keep && (foe < 0 || y < foe_d)) {
            foe = k;
            foe_d = y;
        }
    }
    if (foe >= 0 && flee(d, d->st->foe[foe].bx * 32 + 16, d->st->foe[foe].bz * 32 + 16) > 0) return;
    for (c = 0; c < d->nct; c++) {
        int16_t e;
        int near = 0;
        for (e = d->ct[c].first; e >= 0 && !near; e = d->en[e].next)
            near = (d->en[e].armed || d->en[e].type == WQ_NONE) &&
                   dist(d->en[e].x, d->en[e].z, d->com_x, d->com_z) <= IMPAI_CMD_SAFE_PX;
        if (near && (worst < 0 || d->ct[c].value > d->ct[worst].value)) worst = c;
    }
    if (worst >= 0) {
        SIDEIN a, b;
        float m = -1.0f;
        const int16_t e = contact_nearest(d, worst, d->com_x, d->com_z);
        uint32_t gap = 0x7FFFFFFFu;
        memset(&a, 0, sizeof a);
        for (k = 0; k < d->narmy; k++)
            if (dist(d->army[k].x, d->army[k].z, d->com_x, d->com_z) <= CMD_ARMY_PX) {
                side_add(&a, d->army[k].type, hp_frac(&d->army[k]));
                if (e >= 0) {
                    const uint32_t y = dist(d->army[k].x, d->army[k].z, d->en[e].x, d->en[e].z);
                    if (y < gap) gap = y;
                }
            }
        side_contact(d, worst, &b);
        if (e >= 0 && !(predict(d, &a, &b, (int32_t)gap, 1, 1, &m) == 0 && !(m < 0.0f)) &&
            !stands(d, &a, &b, worst, e, gap) && flee(d, d->en[e].x, d->en[e].z) > 0)
            return;
    }
    reach = (uint32_t)(d->in->brain->opened ? 1 : FAR_PATCH_K) * plan->comm_r;
    if (dist(d->com_x, d->com_z, d->ax, d->az) > reach + LEASH_PAD_PX && d->acts >= 1 &&
        (r = move_to(d, (uint16_t)d->com, d->com_type, d->com_x, d->com_z, d->ax, d->az)) > 0) {
        d->st->safe_until = d->now + SAFE_TICKS;
        d->out->leash = 1;
        if (r == 1) {
            release_commander(d);
            d->acts--;
        }
    }
}

/* ---- 7b. the D-gun ----

   THE D-GUN IS ORDERED, never automatic: an ImpAI seat's weapon scan runs with argument 0, which leaves
   CommandFire weapons alone (0x408A88..0x408A96), as a human seat's does. The commander D-guns one target when
   all of these hold: the target is SEEN NOW (an identified enemy row with IMPAI_NOW or IMPAI_PASS -- never a ghost,
   never a radar contact never identified); the seat's stored energy is at least the shot's plus dgun_reserve of
   its energy storage; the target is WORTH THE SHOT, its value at least the shot's energy in metal (energy /
   TASIM_ROLES_R); and THE D-GUN'S GUARD passes it (spares). Of those within the D-gun's range of the commander --
   the row's block centre plus the block's half diagonal, 24 px, inside the weapon's range -- the most valuable,
   ties to the lower slot, so a target among the seat's own units is passed over for the next. NEVER A COMMANDER:
   its death blast reaches past the D-gun's range (THE BLAST, commander), so the shot is a draw; the safety
   already keeps the commander farther from a seen one, and this holds it for one it has not kept clear of. NEVER
   PAST THE REACH: a target farther than comm_r from the anchor is not shot, because an ordered attack chases its
   target (Attack_Chase 0x4034A0, DISASSEMBLED, engine/orders.md); RESIDUAL: a chase that starts inside the reach
   can follow its target out until the target dies. Not while the commander is the military's for its safety or
   its leash. BOUNDS: the range, the energy and the radius are the content's, held at the build; the economy's
   floats are compared so that a value not finite refuses the shot. A target must be there this turn
   (target_now). The order ends the commander's economy job with no failure (THE COMMANDER'S JOB). */

/* THE D-GUN'S GUARD: a shot is refused when it could reach a friendly unit -- one of the seat's own, its
   structures and unfinished frames included, or an ally's -- or the commander itself:
   - THE LINE. A unit whose point lies within the shot's radius (IMPAI_TINFO.dgun_r) plus its clearance of the
     line from the commander toward the target, out to the weapon's whole range past the commander whatever the
     shot does at its target [INFERRED: a shot ends at its range, as the line-of-sight path's death at
     GameTime + (range << 16) / weaponvelocity does, 0x49CB1C..0x49CB3A, engine/weapons.md]. The target is known
     by its 32-px block only, so the line may point anywhere through a disc of 24 px round the block's centre:
     at a distance t along it the band widens by 24 t / (d - 24), d the block centre's distance, the divisor
     held at 1 or more, so a target within 25 px of the commander widens it past any range.
   - THE BLAST. A unit within the target's death blast (IMPAI_TINFO.blast_r, or the shot's radius when larger)
     plus 24 px of the block and its clearance; and the commander itself within the target's blast plus
     BLAST_PAD_PX and 24 px, the rule THE BLAST keeps against a seen commander.
   A unit's clearance is the half diagonal of its type's footprint plus DGUN_CLEAR_PX. The seat's own units are
   read at their positions this turn, by a walk of its slot block; an own unit outside the block and an ally's
   are read from the seat's rows, at their block's centre, so theirs adds 24 px of the block and the farthest
   their type moves in IMPAI_ROW_AGE_MAX ticks, the row's age. INVARIANT: no friendly unit stands in the band
   or the blast when the order is given; when more than DGUN_SPARE_MAX of them stand near enough to matter, or
   the seat's units cannot be read, no shot. RESIDUAL: a unit that walks into the line after the order, and a
   second shot at the same target, which the order keeps firing until the target dies, are not tested again;
   one retail shot does 30 000 damage (engine/weapons.md), past every retail unit's health. COST: one walk of
   the seat's slot block and of the view's rows, only when a target passes every other test. */
#define DGUN_CLEAR_PX 32        /* past the shot's radius and a footprint [hand-written: the bar's to move]      */
#define BLOCK_HALF_PX 24        /* a 32-px block's half diagonal, 22.6, rounded up                              */

/* the half diagonal of a type's footprint, px, a footprint past 1..WQ_FOOT_MAX cells taken as one cell */
static int32_t foot_r(const IMPAI_TINFO* t)
{
    const int64_t fx = t->fx > 0 && t->fx <= WQ_FOOT_MAX ? t->fx * 16 : 16;
    const int64_t fz = t->fz > 0 && t->fz <= WQ_FOOT_MAX ? t->fz * 16 : 16;
    return (int32_t)(impai_isqrt((uint64_t)(fx * fx + fz * fz)) / 2 + 1);
}

static int spare_add(MDEC* d, int32_t x, int32_t z, int32_t r, uint32_t reach)
{
    if (dist(x, z, d->com_x, d->com_z) > reach + (uint32_t)r) return 0;
    if (d->nspare >= DGUN_SPARE_MAX) return -1;
    d->spare[d->nspare].x = x;
    d->spare[d->nspare].z = z;
    d->spare[d->nspare].r = r;
    d->nspare++;
    return 0;
}

/* the friendly units within `reach` (plus their own clearance) of the commander into d->spare: -1 when the seat's
   units cannot be read or more than DGUN_SPARE_MAX of them stand there */
static int spare_list(MDEC* d, uint32_t reach)
{
    const IMPAI_MIL_IN* in = d->in;
    const IMPAI_VIEW* v = in->view;
    int first, last, slot, i;
    d->nspare = 0;
    if (!v || !d->q->ops->seat_units(d->q, &first, &last)) return -1;
    for (slot = first < 1 ? 1 : first; slot <= last && slot < (int)in->njob; slot++) {
        WQ_UNIT u;
        if (slot == d->com || !d->q->ops->unit(d->q, slot, &u) || !u.type || u.type >= in->nti) continue;
        if (spare_add(d, u.pos[0] >> 16, u.pos[2] >> 16, foot_r(&in->ti[u.type]) + DGUN_CLEAR_PX, reach) < 0)
            return -1;
    }
    for (i = 1; i < v->nrows; i++) {
        const IMPAI_SEEN* w = &v->rows[i].seat[v->seat];
        int32_t sp;
        if (i == d->com || !w->how || w->type == WQ_NONE || w->type >= in->nti || w->owner >= WQ_SEATS) continue;
        if (w->how & IMPAI_OWN) {
            if (i >= first && i <= last) continue;
        } else if (w->owner == (unsigned)v->seat || !((v->allies >> w->owner) & 1u)) {
            continue;
        }
        sp = type_speed(d, w->type);
        sp = sp > 0 ? (int32_t)(((int64_t)sp + 65535) >> 16) : 0;
        if (sp > 255) sp = 255;
        if (spare_add(d, w->bx * 32 + 16, w->bz * 32 + 16,
                      foot_r(&in->ti[w->type]) + DGUN_CLEAR_PX + BLOCK_HALF_PX + IMPAI_ROW_AGE_MAX * sp, reach) < 0)
            return -1;
    }
    return 0;
}

/* 1 when the guard lets the commander D-gun target m (THE D-GUN'S GUARD); d->spare holds every friendly unit
   that can stand in the line or a candidate's blast */
static int spares(const MDEC* d, const MENEMY* m, uint32_t range, uint32_t shot_r)
{
    const uint32_t blast = d->in->ti[m->type].blast_r, hit = blast > shot_r ? blast : shot_r;
    const int64_t ax = m->x - d->com_x, az = m->z - d->com_z;
    const int64_t dd = (int64_t)dist(d->com_x, d->com_z, m->x, m->z);
    const int64_t den = dd - BLOCK_HALF_PX > 1 ? dd - BLOCK_HALF_PX : 1;
    int k;
    if (dd <= (int64_t)blast + BLAST_PAD_PX + BLOCK_HALF_PX) return 0;
    for (k = 0; k < d->nspare; k++) {
        const MSPARE* n = &d->spare[k];
        const int64_t px = n->x - d->com_x, pz = n->z - d->com_z, w = (int64_t)shot_r + n->r;
        int64_t t, lat, tc;
        if (dist(n->x, n->z, m->x, m->z) <= hit + BLOCK_HALF_PX + (uint32_t)n->r) return 0;
        t = (px * ax + pz * az) / dd;
        lat = px * az - pz * ax;
        lat = (lat < 0 ? -lat : lat) / dd;
        if (t < -w || t > (int64_t)range + w) continue;
        tc = t < 0 ? 0 : t > (int64_t)range ? (int64_t)range : t;
        if (lat <= w + BLOCK_HALF_PX * tc / den) return 0;
    }
    return 1;
}

/* The commander's type when it can fire its D-gun now by THE D-GUN's rules that do not depend on a target: a
   commander of a type with a D-gun, outside the military's window, the seat's stored energy at least the shot
   plus dgun_reserve of its storage; the shot's worth, its energy in metal, into *worth. NULL otherwise. */
static const IMPAI_TINFO* dgun_armed(MDEC* d, float* worth)
{
    const IMPAI_TINFO* ct;
    WQ_ECON e;
    float need;
    if (!d->com || d->com_type >= d->in->nti || d->now < d->st->safe_until) return NULL;
    ct = &d->in->ti[d->com_type];
    if (!ct->dgun_range) return NULL;
    if (d->q->ops->economy(d->q, &e) != WQ_OK) return NULL;
    need = (float)ct->dgun_e + d->plan->dgun_reserve * e.energy_max;
    if (!(e.energy >= need)) return NULL;
    *worth = (float)ct->dgun_e / (float)TASIM_ROLES_R;
    return ct;
}

/* 1 when enemy m is of the class THE D-GUN takes as a target: seen now, identified, worth the shot, no commander.
   The picture holds an identified enemy's type below nti. */
static int dgun_class(const MDEC* d, const MENEMY* m, float worth)
{
    return m->now && m->type != WQ_NONE && m->value >= worth && d->in->ti[m->type].cat != IMPAI_C_COMMANDER;
}

static void dgun(MDEC* d)
{
    const IMPAI_TINFO* ct;
    float worth = 0.0f;
    uint32_t reach = 0;
    int k, n = 0, best = -1;
    if (d->acts < 1 || !(ct = dgun_armed(d, &worth))) return;
    /* the candidates first, so that the guard's walks run only when some target passes every other test */
    for (k = 0; k < d->nen; k++) {
        const MENEMY* m = &d->en[k];
        d->shoot[k] = 0;
        if (!dgun_class(d, m, worth) || dist(m->x, m->z, d->ax, d->az) > d->plan->comm_r) continue;
        if (dist(m->x, m->z, d->com_x, d->com_z) + 24u > ct->dgun_range) continue;
        d->shoot[k] = 1;
        if (d->in->ti[m->type].blast_r > reach) reach = d->in->ti[m->type].blast_r;
        n++;
    }
    if (!n) return;
    reach = (uint32_t)ct->dgun_range + (reach > ct->dgun_r ? reach : ct->dgun_r) + BLOCK_HALF_PX;
    if (spare_list(d, reach) < 0) return;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* m = &d->en[k];
        uint8_t aim;
        if (!d->shoot[k]) continue;
        if (!(best < 0 || m->value > d->en[best].value ||
              (m->value == d->en[best].value && m->slot < d->en[best].slot)) ||
            !spares(d, m, ct->dgun_range, ct->dgun_r) || !target_now(d, m, &aim))
            continue;
        best = k;
    }
    if (best < 0) return;
    if (order(d, (uint16_t)d->com, WQ_CAN_DGUN, d->en[best].slot, d->en[best].x, d->en[best].z) == 1) {
        d->out->dgun = 1;
        release_commander(d);
        d->acts--;
    }
}

/* ---- the entry ----

   ONE MILITARY DECISION, in order: (1) the army's census over the seat's slot block, which marks the sweep's
   cells; (2) the picture of the enemy over the seat's column of the knowledge rows, the commander's memory, the
   base's estimate, the naval judgement; the squad table's normalisation, the squads' figures and the contacts;
   (3) the base's threats and its defence; (7) the commander's safety -- the blast, the contact and the leash,
   every flight inside comm_r -- then its D-gun, never at a commander nor past the reach; (4) the squads' tasks
   with their goals and predictions, raids and scouting; (5) the orders; (8) the picture kept for the factories;
   (9) the pulse's counts.

   BOUNDS. Every slot taken from a table -- a job row, a squad's target, a knowledge row -- is below the
   table's count before it indexes it, and is re-read through the WORLDQ, which answers from the engine's
   unit array under the adapter's bounds (tagpu_worldq.c, ORDERS OUT): a recycled slot costs one wrong order
   or one wrong membership, the resolver answering for the unit as it is now. A squad index is below
   IMPAI_SQUADS, a type below nti before any table is read by it, and the predictor refuses a type past its
   content. NO CHEATING: a target is ordered only while the seat sees it now (IMPAI_NOW | IMPAI_PASS on its own
   row), the picture and the commander's memory read the seat's column alone, and the sweep the seat's own army.
   COST: units at most nslots, listed at most IMPAI_ARMY_MAX; enemies IMPAI_ENEMY_MAX; contacts IMPAI_CONTACTS;
   squads IMPAI_SQUADS; predictions predict_max (<= IMPAI_PREDICT_CAP); orders mil_roots; the maps
   IMPAI_GRID_MAX^2 cells; substitute points SUB_CACHE spirals; each squad's reach test NEAR_KEYS spirals; the
   hunt's goals, for at most IMPAI_ATTACKS_MAX ATTACK squads, a forming wave and the scout, IMPAI_SWEEP cells each.

   THE TICK RULE (impai_mil_due) keeps this decision and the economy's on different ticks of the same seat. */
int impai_mil_decide(const IMPAI_MIL_IN* in, IMPAI_MIL_OUT* out)
{
    MDEC* d;
    const WORLDQ* q = in->q;
    WQ_MAP m;
    int k;
    if (!q || !in->st || !in->job || !in->ti || !in->plan || !in->persona || !in->brain || !in->scratch ||
        in->plan->mil_roots > IMPAI_MIL_ROOTS_MAX ||
        in->scratch_bytes < impai_mil_scratch_bytes() || (int)in->nti != q->ops->ndefs(q) ||
        (int)in->njob < q->ops->nslots(q))
        return -1;
    memset(out, 0, sizeof *out);
    d = carve(in);
    d->in = in;
    d->out = out;
    d->q = q;
    d->st = in->st;
    d->plan = in->plan;
    d->now = (uint32_t)q->ops->gametime(q);
    d->acts = in->acts < 0 ? ACTS_NONE : in->acts;
    q->ops->map(q, &m);
    d->mw = m.w > 0 ? m.w * 16 : 1;
    d->mh = m.h > 0 ? m.h * 16 : 1;
    d->gw = (m.w * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX;
    d->gh = (m.h * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX;
    if (d->gw < 1) d->gw = 1;
    if (d->gh < 1) d->gh = 1;
    if (d->gw > IMPAI_GRID_MAX) d->gw = IMPAI_GRID_MAX;
    if (d->gh > IMPAI_GRID_MAX) d->gh = IMPAI_GRID_MAX;
    if (in->brain->ax == IMPAI_NO_ANCHOR) return 0;
    d->ax = in->brain->ax * 16 + 8;
    d->az = in->brain->az * 16 + 8;
    if ((d->nstarts = q->ops->starts(q, d->starts, STARTS)) < 0) d->nstarts = 0;
    for (k = 1; k < d->nstarts; k++)
        if (dist(d->starts[k].x, d->starts[k].z, d->ax, d->az) <
            dist(d->starts[d->own_start].x, d->starts[d->own_start].z, d->ax, d->az))
            d->own_start = k;
    d->st->air_seen = 0.0f;
    census(d);
    picture(d);
    naval(d);
    /* the rally point: the anchor moved rally_px towards the estimate */
    d->rx = d->ax;
    d->rz = d->az;
    if (d->st->ebx != IMPAI_NO_ANCHOR) {
        const int32_t ex = d->st->ebx * 32 + 16, ez = d->st->ebz * 32 + 16;
        const uint32_t de = dist(d->ax, d->az, ex, ez);
        if (de > d->plan->rally_px) {
            d->rx = d->ax + (int32_t)((int64_t)(ex - d->ax) * d->plan->rally_px / de);
            d->rz = d->az + (int32_t)((int64_t)(ez - d->az) * d->plan->rally_px / de);
        }
    }
    out->army = (uint16_t)d->narmy;
    out->enemies = (uint16_t)d->nen;
    normalise(d);
    squads(d);
    contacts(d);
    base_threats(d);
    defence(d);
    commander(d);
    if (!d->out->safe) dgun(d);
    tasks(d);
    raid(d);
    scouting(d);
    orders(d);
    out->roots = (uint16_t)d->roots;
    out->acts = (uint16_t)(in->acts < 0 ? 0 : in->acts - d->acts);
    out->ops = (uint8_t)ops(d);
    out->left_out = (uint16_t)(out->left_squad + out->left_army + out->left_enemy + out->left_contact);
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (d->st->sq[k].task < IMPAI_SQ_TASKS) out->tasks[d->st->sq[k].task]++;
    return 0;
}
