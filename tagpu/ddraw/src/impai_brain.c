/* impai_brain.c -- ImpAI's decision (impai.h, the brain): its entry, the census of the seat's units and the
   checks of the jobs it gave them. The ladder and the factories are impai_econ.c's, the sites and the orders
   impai_build.c's. Engine-free and without statics: everything it knows comes through the WORLDQ and its
   arguments, everything it keeps is the caller's (IMPAI_BRAIN, IMPAI_JOB). */
#include <string.h>
#include "impai_brain.h"
#include "impai_react.h"
#include "tagpu_simreg.h"

/* The brain's entry, its census and the jobs' checks. OWNER: the brain decides only inside a live ImpAI
   seat's turn, which plays no puppet seat (tagpu_impai.c, impai_play); on a puppet level the level init's
   reset only clears the brain's state, which the recording then restores. */
SIM_FEATURE(impai_brain, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval impai-raid impai-mend impai-persona");

/* THE PLAN (research/notes/impai/brain.md, The plan and the personas), which every level plays: a level is a
   persona (impai_react.c), never other decisions. Each field's meaning and limit is in impai.h beside it. The
   values are hand-written starting points for the bar's diagnosis to move, not principles: the economy is the row
   A1b's bench passed with, and the military the row the bar's Hard evidence was measured on. The governor's
   numbers (impai_econ.c, THE GOVERNOR) are hand-written starting points too:
   - army_per_cons 2: BARb's openers carry one builder in every two to three units;
   - hold_pm 200, hold_fac 1 (THE HOLD, economy): 200 is m_stall's own 0.2, which leaves a buffer of 230-340 metal
     at the measured storages (1 150-1 700, p10-p90 of the bar's seats); one factory keeps the army coming in a
     long hold.
   The mend's [hand-written: the bar's to move, but mend_hp, the owner's rule: an army unit below half its health
   mends]: mend_wait 1 800 ticks is 60 s, the window a repairer has to arrive. MEASURED 2026-11-14 over the mend's
   first value runs (32 recordings, mend_wait then 900): the repair order came a median of 390 ticks after the
   mender's arrival (p90 795) and its first hit point a median of 540 after the order (p90 840), so the median
   repair began about 930 ticks after the arrival; the two p90s sum to about 1 650. mend_roots 6 and repair_max 2
   are Hard's row of the levels' tables before the plan, how many menders a decision sends and how many
   constructors repair at once -- what the plan decides; how many builders a level keeps busy in all is its
   persona's attention (impai_react.c). */
const IMPAI_PLAN IMPAI_PLAN_ROW = {
    .open_mex = 2, .open_energy = 2, .open_t = 1800,
    .factory_max = 2, .factory_per_m = 8.0f,
    .e_per_m = 8.0f,
    .cons_open = 1, .cons_max = 12, .army_per_cons = 2, .press_k = 1.5f, .press_hold = 900,
    .hold_pm = 200, .hold_fac = 1,
    .comm_r = 1024, .base_r = 1536,
    .gap = 2,
    .job_timeout = 2700, .fail_t = 5400,
    .idle_max = 32, .assign_max = 4, .site_tests = 32, .snaps = 4, .queue_max = 4, .roots = 8, .job_checks = 8,
    /* the military [hand-written: the bar's to move] */
    .mil_roots = 40, .react_confirm = 1, .predict_max = 12, .aa_struct_max = 6,
    .micro_hp = 0.35f, .micro_period = 90, .rally_px = 384,
    .attack_margin = 0.10f, .retreat_margin = -0.05f, .wave_value = 8.0f,
    .scout_period = 1800, .task_timeout = 2700,
    .aa_struct_v = 1000.0f, .dgun_reserve = 0.25f,
    .def_max = 4, .def_v = 600.0f,
    .mend_hp = 0.50f, .mend_wait = 1800, .mend_roots = 6, .repair_max = 2,
};

int impai_brain_due(unsigned gametime, unsigned seat, unsigned look)
{
    return look && gametime % look == (7u * seat) % look;
}

void impai_mil_reset(IMPAI_MIL* m, unsigned level, const IMPAI_PERSONA* persona, const IMPAI_TABLE* table,
                     int from_lever)
{
    unsigned k;
    memset(m, 0, sizeof *m);
    m->level = (uint8_t)(level < IMPAI_LVS ? level : IMPAI_LV_NORMAL);
    m->table_lever = (uint8_t)(from_lever != 0);
    m->ebx = m->ebz = IMPAI_NO_ANCHOR;
    m->radar_as = (uint16_t)WQ_NONE;
    m->naval = -1;
    if (table) m->table = *table;
    if (persona) m->persona = *persona;
    impai_exec_reset(&m->exec, &m->persona);
    for (k = 0; k < IMPAI_SQUADS; k++) m->sq[k].target = (uint16_t)WQ_NONE;
    for (k = 0; k < WQ_SEATS; k++) m->foe[k].slot = (uint16_t)WQ_NONE;
}

/* the scratch's parts, each rounded up to 16 bytes */
#define ALIGN16(n) (((n) + 15u) & ~(size_t)15u)
#define MENU_BYTES   ALIGN16(WQ_TYPES_MAX * sizeof(uint16_t))
#define RING_BYTES   ALIGN16(PLACE_MAX_SITES * sizeof(PlaceCell))
#define GRID_CELLS   ((size_t)IMPAI_GRID_MAX * IMPAI_GRID_MAX)
#define MAPS_BYTES   (IMPAI_THREATS * ALIGN16(GRID_CELLS * sizeof(uint32_t)) + ALIGN16(GRID_CELLS * sizeof(int32_t)) + \
                      ALIGN16(GRID_CELLS * sizeof(uint16_t)) + ALIGN16((IMPAI_GRID_MAX + 1) * (size_t)IMPAI_GRID_MAX * sizeof(int32_t)))
#define WAY_BYTES    (ALIGN16(IMPAI_WAY_PASS_BYTES) + ALIGN16(IMPAI_WAY_BLOCK_CELLS * sizeof(int32_t)))
#define SCRATCH_BYTES (ALIGN16(sizeof(IMPAI_DEC)) + 3 * MENU_BYTES + RING_BYTES + MAPS_BYTES + WAY_BYTES)

_Static_assert(SCRATCH_BYTES <= IMPAI_BRAIN_SCRATCH_MAX, "a decision's scratch fits IMPAI_BRAIN_SCRATCH_MAX");

size_t impai_brain_scratch_bytes(void) { return SCRATCH_BYTES; }

void impai_brain_reset(IMPAI_BRAIN* st, IMPAI_JOB* job, unsigned njob)
{
    unsigned i;
    memset(st, 0, sizeof *st);
    st->ax = st->az = IMPAI_NO_ANCHOR;
    st->explorer = (uint16_t)WQ_NONE;
    st->sea_none_type = st->sea_cur_type = (uint16_t)WQ_NONE;
    st->fscan = (uint16_t)WQ_NONE;
    for (i = 0; i < IMPAI_WAYS; i++) st->way[i].type = (uint16_t)WQ_NONE;
    for (i = 0; i < IMPAI_WAY_FAILS; i++) st->wfail[i].type = (uint16_t)WQ_NONE;
    for (i = 0; i < njob; i++) {
        memset(&job[i], 0, sizeof job[i]);
        job[i].type = job[i].build = job[i].target = (uint16_t)WQ_NONE;
    }
}

static void job_clear(IMPAI_JOB* j)
{
    memset(j, 0, sizeof *j);
    j->type = j->build = j->target = (uint16_t)WQ_NONE;
}

/* the slot holds anything of the job table's: a job, or a builder's unreachable site (far_until 0: none) */
static int job_held(const IMPAI_JOB* j) { return j->type != WQ_NONE || j->far_until; }

/* ---- small rules the three files share ---- */

uint32_t impai_isqrt(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* the integer distance in px from a 16.16 position to a point in px */
uint32_t impai_dist_px(const int32_t a[3], int32_t bx_px, int32_t bz_px)
{
    const int64_t dx = (int64_t)(a[0] >> 16) - bx_px, dz = (int64_t)(a[2] >> 16) - bz_px;
    return impai_isqrt((uint64_t)(dx * dx + dz * dz));
}

/* a building of `type` whose first cell is (cx, cz) */
IMPAI_RECT impai_rect_at(const IMPAI_DEC* d, int type, int cx, int cz)
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    IMPAI_RECT r;
    r.x0 = (int16_t)cx;
    r.z0 = (int16_t)cz;
    r.x1 = (int16_t)(cx + (t->fx > 0 ? t->fx : 1) - 1);
    r.z1 = (int16_t)(cz + (t->fz > 0 ? t->fz : 1) - 1);
    return r;
}

/* The first cell of a footprint of f cells whose centre is at 16.16 coordinate p: a placed building stands at
   (2 origin + f) << 19 (tagpu_placegeo.h), and an order's position is the same centre. */
static int origin_of(int32_t p, int f)
{
    return ((p >> 19) - f) >> 1;
}

static IMPAI_RECT rect_of(const IMPAI_DEC* d, int type, const int32_t pos[3])
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    return impai_rect_at(d, type, origin_of(pos[0], t->fx > 0 ? t->fx : 1), origin_of(pos[2], t->fz > 0 ? t->fz : 1));
}

static int overlap(const IMPAI_RECT* a, const IMPAI_RECT* b)
{
    return a->x0 <= b->x1 && b->x0 <= a->x1 && a->z0 <= b->z1 && b->z0 <= a->z1;
}

static int inside(const IMPAI_RECT* r, int cx, int cz)
{
    return cx >= r->x0 && cx <= r->x1 && cz >= r->z0 && cz <= r->z1;
}

void impai_fail_add(IMPAI_DEC* d, int cx, int cz)
{
    IMPAI_BRAIN* st = d->in->st;
    IMPAI_FAIL* f = &st->fail[st->fail_next % IMPAI_FAILS];
    f->cx = (int16_t)cx;
    f->cz = (int16_t)cz;
    f->until = d->now + d->in->plan->fail_t;
    st->fail_next = (uint16_t)((st->fail_next + 1u) % IMPAI_FAILS);
    if (st->nfail < IMPAI_FAILS) st->nfail++;
    d->out->failed++;
}

/* a failed site or cell still avoided lies in r */
int impai_failed_in(const IMPAI_DEC* d, const IMPAI_RECT* r)
{
    const IMPAI_BRAIN* st = d->in->st;
    int i;
    for (i = 0; i < st->nfail && i < IMPAI_FAILS; i++)
        if (st->fail[i].until > d->now && inside(r, st->fail[i].cx, st->fail[i].cz)) return 1;
    return 0;
}

/* A CLAIM IS NEVER DROPPED. The table holds IMPAI_CLAIM_MAX, its bound (impai.h), so it is never full; were one
   ever without room, it marks the decision full, and a full decision answers every test claimed, so it places
   nothing rather than place over a site or a plant's way out it could not hold. */
_Static_assert(IMPAI_CLAIM_MAX >= IMPAI_CENSUS_MAX + IMPAI_WAYS * IMPAI_WAY_RECTS + IMPAI_ROOTS_MAX,
               "the claim table holds a census's claims, the kept ways' and one a command");
int impai_claimed(const IMPAI_DEC* d, const IMPAI_RECT* r)
{
    int i;
    if (d->claims_full) return 1;
    for (i = 0; i < d->nclaims; i++) if (overlap(&d->claims[i].r, r)) return 1;
    return 0;
}

void impai_claim_add(IMPAI_DEC* d, int type, const IMPAI_RECT* r)
{
    if (d->nclaims >= IMPAI_CLAIM_MAX) {
        d->claims_full = 1;
        return;
    }
    d->claims[d->nclaims].type = (uint16_t)type;
    d->claims[d->nclaims].r = *r;
    d->claims[d->nclaims].way = 0;
    d->nclaims++;
}

IMPAI_RECT impai_feat_rect(int ax, int az, int fx, int fz)
{
    IMPAI_RECT r;
    r.x0 = (int16_t)ax;
    r.z0 = (int16_t)az;
    r.x1 = (int16_t)(ax + (fx > 0 ? fx : 1) - 1);
    r.z1 = (int16_t)(az + (fz > 0 ? fz : 1) - 1);
    return r;
}

/* A reclaim job's feature as the seat's own sight shows it, read only while its anchor's block is in sight: 1
   there -- the one anchored at the job's cell, of the job's definition, still to be taken (impai_feat_takeable),
   in *f --, 0 gone: none, another anchored there since (a successor), or one playing its sequence, which a
   reclaim paid or a death began; -1 not in sight, unknown. */
static int reclaim_seen(const IMPAI_DEC* d, const IMPAI_JOB* j, WQ_FEAT* f)
{
    if (!impai_feat_lit(d, j->cx, j->cz)) return -1;
    return d->q->ops->feature(d->q, j->cx, j->cz, f) && f->ax == j->cx && f->az == j->cz && f->def == j->feat &&
           impai_feat_takeable(f);
}

static int reclaim_job(const IMPAI_JOB* j) { return j->kind == IMPAI_JOB_RECLAIM || j->kind == IMPAI_JOB_RECLAIM_GO; }

/* ---- 1. the census ---- */

static void unit_ref(IMPAI_UNITREF* r, const WQ_UNIT* u)
{
    r->slot = u->slot;
    r->type = u->type;
    memcpy(r->pos, u->pos, sizeof r->pos);
}

/* THE IDLE TEST is the stock builder planner's (0x408100: engine/orders.md, The ten slots): no head, or a head
   whose +0x42 has bit 0x4000 -- the unit's own idle behaviour (its FBI default order, or what the engine pushed
   in front of it), which any real order replaces (scout I6: no descriptor carries the bit; the controller sets
   it on the default order it makes, 0x43BA08, and 0x43ADC0 drops such a head). The stock factory planner's test
   is the same list's emptiness (0x40878B); a factory with a defaultmissiontype holds a 0x4000 head when idle,
   so the factory takes the general test too. A LONE PARK is idle as well: GetBuilt 0x402DA0 gives a factory's
   finished unit the factory's QMove and QPatrol orders, and with none a PARK (0x402EE2..0x402EFA, through
   0x438760 and 0x43ADC0 with Shift), whose handler 0x4061A0 moves it off the yard and then ends only when another
   node follows it (0x4061D7..0x4061DC) or on its event bit 0x20 (0x4061C3), otherwise sleeping 30 ticks at a time
   through 0x439E80 for ever (DISASSEMBLED). Its flag word carries no 0x4000, so the stock test reads it as busy
   and a constructor the factory made would hold its Park for the whole game (MEASURED 2026-11-13 on Lava Mania,
   a CORCV out of a CORVP: flags 0x1001, parked 64 px south of the plant's yard to GameTime 36 000, while the
   next CORCV, finished on the yard, never left it and the plant built nothing more -- the parked one standing
   in its way [INFERRED]). Any order replaces it: its flags lack WQ_OF_KEEP. The Park counts only as the unit's
   one node, read from its list. */
static int idle_head(IMPAI_DEC* d, int slot, int h, const WQ_ORDER* o)
{
    if (h == 0 || (h == 1 && (o->flags & WQ_OF_DROP))) return 1;
    return h == 1 && d->act_park > 0 && o->action == d->act_park && d->q->ops->orders(d->q, slot, d->olist, 2) == 1;
}

/* THE HAND-BACK, of the commander and of every constructor. A builder with no job is the economy's whatever its
   head, when that head is its one order, names no unit and is no build: it is listed idle, and the decision's
   ladder replaces the head. One order, because every order the economy and the military give replaces the list
   (WQ_CMD_NEW), so a list of more holds what the engine put there, as a Park with a node behind it (THE IDLE
   TEST). The military holds the commander only inside its window (IMPAI_MIL.safe_until, THE COMMANDER'S WINDOW
   below), so for the commander this holds outside it. The moves such a builder holds are the military's flight
   and leash, to an own structure's centre or to the anchor (impai_mil.c, THE FLIGHT), and the economy's own turn
   back, to the anchor or the nearest unthreatened cell (impai_build.c, THE TURN BACK); the engine's ground move
   ends only on its arrival event (0x4031D0: state 1 returns 5 on event 0x20, else 9; the dispatch sets +0x42 bit
   0x800000 on a 9 and starts a lone node over after 30 to 59 ticks, 0x43B8E5..0x43B95C; DISASSEMBLED), which a
   unit sent to a point it cannot stand on never reaches: MEASURED in the bar's cmdr run (65 games, the
   commander's head every 300 ticks), the commander sent to a structure's centre stopped 39-51 px short (p10-p90)
   with the move holding its head in 5 090 samples, 91 % of them with no armed enemy within 800 px, for up to
   67 200 ticks. Any economy order replaces the list (WQ_CMD_NEW), so such a move outlives its maker's purpose by
   at most one economy decision that has work for the builder, and the ladder's every site is threat-tested --
   its cell, and for a constructor THE ROUTE (impai_build.c) -- so a turned-back constructor is not sent back into
   what it left. A head that names a unit -- the D-gun's attack, a guard, a repair -- stays its maker's until the
   engine ends it; RepairUnit ends at its target's full health or death and follows a target that walks
   (impai_build.c, THE REPAIR), which is why THE REPAIR serves only a mender standing at its haven. INVARIANT, an ordering by the job field and, for the commander, the window field: at every economy
   decision a finished builder is either the military's (the commander inside the window) or listed by the
   economy (idle, helper or jobbed), a head that names a unit aside; nothing rests on the engine finishing a move,
   so a constructor the governor counts (gov_c) is one the economy can task, that head aside. RESIDUAL:
   the D-gun's chase (impai_mil.c, THE D-GUN). */
static int handed_back(IMPAI_DEC* d, int slot, const IMPAI_JOB* job, int h, const WQ_ORDER* o)
{
    return job->kind == IMPAI_JOB_NONE && h == 1 && o->target == (uint16_t)WQ_NONE && o->action != d->act_build &&
           o->action != d->act_vtol_build && d->q->ops->orders(d->q, slot, d->olist, 2) == 1;
}

static void census_unit(IMPAI_DEC* d, const WQ_UNIT* u)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_JOB* job = &in->job[u->slot];
    const IMPAI_TINFO* t;
    const int type = u->type;
    WQ_ORDER o;
    int h, cat, building;
    if (!type || (unsigned)type >= in->nti) {
        job_clear(job);
        return;
    }
    if ((job->type != WQ_NONE && job->type != type) || (job->far_until && job->far_type != type))
        job_clear(job);
    t = &in->ti[type];
    if (t->cat != IMPAI_C_MEX && u->made_m > t->mgain) d->m_paid += u->made_m - t->mgain;   /* THE STEADY INCOME */
    cat = t->cat < IMPAI_CATS ? t->cat : IMPAI_C_OTHER;
    building = !(t->flags & IMPAI_TF_MOBILE);
    d->count[cat]++;
    if (impai_water_plant(in, (unsigned)type)) d->sea_plants++;
    if (u->finished) d->done[cat]++;
    if (u->finished && cat == IMPAI_C_CONS) d->gov_c++;
    if (u->finished && t->klass < IMPAI_KS) d->gov_a++;
    /* THE REPAIR's menders (impai_build.c): an army unit the military holds at its haven (IMPAI_JOB_MEND), below
       full health and within IMPAI_MEND_NEAR_PX of it. The haven is recorded data, an int16 block, and the
       distance is 64-bit, so any value is only a far haven. A MEND_GO unit is never listed: RepairUnit repairs a
       standing target only (THE REPAIR). */
    if (u->finished && t->klass < IMPAI_KS && job->kind == IMPAI_JOB_MEND && u->hp_max &&
        (int64_t)u->hp < (int64_t)u->hp_max && d->nmend < IMPAI_IDLE_CAP &&
        impai_dist_px(u->pos, job->cx * 32 + 16, job->cz * 32 + 16) <= IMPAI_MEND_NEAR_PX)
        unit_ref(&d->mend[d->nmend++], u);
    if (!d->lowest.slot) unit_ref(&d->lowest, u);
    if (cat == IMPAI_C_COMMANDER && !d->commander.slot) unit_ref(&d->commander, u);
    if (u->slot == in->st->explorer && job->kind == IMPAI_JOB_EXPLORE) d->explorer_live = 1;
    if ((cat == IMPAI_C_COMMANDER || cat == IMPAI_C_CONS) && u->finished && d->nbtype < IMPAI_SEA_BUILDERS) {
        int k;
        for (k = 0; k < d->nbtype && d->btype[k] != type; k++) {}
        if (k == d->nbtype) d->btype[d->nbtype++] = (uint16_t)type;
    }
    if (cat == IMPAI_C_CONS && (unsigned)type < WQ_TYPES_MAX)
        (u->finished ? d->cons_done : d->cons_frame)[type >> 5] |= 1u << (type & 31);
    /* THE COMMANDER'S WINDOW. While GameTime < safe_until the military has moved the commander out of a fight
       it would lose (impai_mil.c, the commander's safety), and while com_until marks it (impai_com_marked) an
       order the military gave it is still pending (THE COMMANDER'S MARKERS); in either the economy neither lists it
       idle, nor as a helper, nor checks its job: so two managers never order it in one window, by these
       fields and not by which runs first. It is still counted, and its head's build is still a claim. Outside
       the window the commander is listed by the ordinary tests and by THE HAND-BACK. */
    if (cat == IMPAI_C_COMMANDER && in->mil &&
        (d->now < in->mil->safe_until || impai_com_marked(in->mil->com_until, d->now, in->persona))) {
        h = d->q->ops->head(d->q, u->slot, &o);
        if (h == 1 && (o.action == d->act_build || o.action == d->act_vtol_build) && o.type && o.type < in->nti) {
            const IMPAI_RECT r = rect_of(d, o.type, o.pos);
            impai_claim_add(d, o.type, &r);
            if (impai_water_plant(in, o.type)) d->sea_plants++;
        }
        return;
    }
    h = d->q->ops->head(d->q, u->slot, &o);
    if (t->klass < IMPAI_KS) d->kspent[t->klass] += t->value;
    if (cat == IMPAI_C_FACTORY) {           /* a frame too: a plant under construction is what the seat will make */
        if (t->ctech > d->ftop) {
            d->ftop = t->ctech;
            d->ftop_n = 0;
        }
        if (t->ctech == d->ftop) d->ftop_n++;
    }
    if (building && impai_aa_struct(in, type)) d->aa_n++;
    if (impai_def_struct(in, (unsigned)type)) d->def_n++;
    if (cat == IMPAI_C_COMMANDER || cat == IMPAI_C_CONS) {
        /* a reclaim's feature is a claim while its builder is on its way to it or at work on it, so no other
           builder is sent to it; an idle one's job ends in this decision's checks, and the feature is free for
           another. Its footprint is the memory's (THE FEATURE MEMORY), a cell where the memory holds none, and
           its metal, as the memory last saw it, is on its way to the seat's store (impai_build.c, THE ROOM) */
        if (reclaim_job(job) && !idle_head(d, u->slot, h, &o)) {
            const int k = impai_fmem_find(in->st, job->cx, job->cz);
            const IMPAI_RECT r = impai_feat_rect(job->cx, job->cz, k >= 0 ? in->st->fmem[k].fx : 1,
                                                 k >= 0 ? in->st->fmem[k].fz : 1);
            impai_claim_add(d, WQ_NONE, &r);
            if (k >= 0) d->m_inflight += in->st->fmem[k].metal;
        }
        /* a build order at the head is a claim on its site */
        if (h == 1 && (o.action == d->act_build || o.action == d->act_vtol_build) && o.type && o.type < in->nti) {
            const IMPAI_RECT r = rect_of(d, o.type, o.pos);
            impai_claim_add(d, o.type, &r);
            if (impai_aa_struct(in, o.type)) d->aa_n++;
            if (impai_def_struct(in, o.type)) d->def_n++;
            if (impai_water_plant(in, o.type)) d->sea_plants++;
            d->pending[in->ti[o.type].cat < IMPAI_CATS ? in->ti[o.type].cat : IMPAI_C_OTHER]++;
            if (in->ti[o.type].cat == IMPAI_C_ENERGY) d->pe_extra += in->ti[o.type].egain;
        }
        if (u->finished) {
            IMPAI_UNITREF r;
            const int idle = idle_head(d, u->slot, h, &o) || handed_back(d, u->slot, job, h, &o);
            /* a repair at work serves its mender (THE REPAIR); counted past the list's IMPAI_IDLE_CAP too, so a
               full list holds every further repair back rather than double one */
            if (!idle && job->kind == IMPAI_JOB_REPAIR) {
                if (d->nserved < IMPAI_IDLE_CAP) d->served[d->nserved] = job->target;
                d->nserved++;
            }
            unit_ref(&r, u);
            if (!d->ffirst.slot) d->ffirst = r;
            if (!d->fobs.slot && (in->st->fscan == WQ_NONE || u->slot > in->st->fscan)) d->fobs = r;
            if (idle && d->nidle < in->plan->idle_max && d->nidle < IMPAI_IDLE_CAP) d->idle[d->nidle++] = r;
            else if (!idle && job->kind == IMPAI_JOB_ASSIST && d->nhelpers < IMPAI_IDLE_CAP) d->helpers[d->nhelpers++] = r;
            if (job->kind != IMPAI_JOB_NONE && d->njobbed < 2 * IMPAI_IDLE_CAP) {
                d->jobbed_idle[d->njobbed] = (uint8_t)idle;
                d->jobbed[d->njobbed++] = r;
                d->nbusy += !idle;
            }
        }
    }
    if (cat == IMPAI_C_FACTORY && u->finished) {
        IMPAI_UNITREF r;
        unit_ref(&r, u);
        if (idle_head(d, u->slot, h, &o)) {
            if (d->nfidle < IMPAI_IDLE_CAP) d->fidle[d->nfidle++] = r;
        } else {
            int k, n = d->q->ops->orders(d->q, u->slot, d->olist, 8);
            if (d->nfbusy < IMPAI_IDLE_CAP) {
                d->fbusy_q[d->nfbusy] = (uint8_t)(h == 1 && o.action == d->act_queue);
                d->fbusy[d->nfbusy++] = r;
            }
            for (k = 0; k < n; k++) {
                const WQ_ORDER* q = &d->olist[k];
                int32_t c;
                if (q->action != d->act_queue || q->type >= in->nti || q->count <= 0) continue;
                c = q->count > IMPAI_QCOUNT_HELD ? IMPAI_QCOUNT_HELD : q->count;
                if (in->ti[q->type].cat == IMPAI_C_CONS) d->gov_c += c;
                else if (in->ti[q->type].klass < IMPAI_KS) {
                    d->gov_a += c;
                    d->kspent[in->ti[q->type].klass] += in->ti[q->type].value * (float)c;
                }
            }
        }
    }
    if (building) {
        const IMPAI_RECT r = rect_of(d, type, u->pos);
        if (cat == IMPAI_C_MEX || cat == IMPAI_C_GEO) impai_claim_add(d, type, &r);
        if (d->nstructs < IMPAI_CENSUS_MAX) {
            IMPAI_STRUCT* s = &d->structs[d->nstructs++];
            s->slot = u->slot; s->type = (uint16_t)type; s->r = r; s->cat = (uint8_t)cat; s->finished = u->finished;
        }
        if (!u->finished) {
            if (d->nnano < IMPAI_NANO_MAX) unit_ref(&d->nano[d->nnano++], u);
            if (cat == IMPAI_C_ENERGY) d->pe_extra += t->egain;
        }
    } else if (!u->finished && d->nframes < IMPAI_NANO_MAX)
        unit_ref(&d->frames[d->nframes++], u);
}

/* Over the seat's slot block in slot order, bounded by it and by IMPAI_CENSUS_MAX slots, which the engine's
   per-player ceiling never passes (impai.h). A slot whose unit is gone or of another type loses its job. Returns
   the units counted. */
static int census(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int first, last, slot, n = 0;
    if (!d->q->ops->seat_units(d->q, &first, &last)) return 0;
    if (first < 1) first = 1;
    if (last >= (int)in->njob) last = (int)in->njob - 1;
    if (last - first >= IMPAI_CENSUS_MAX) last = first + IMPAI_CENSUS_MAX - 1;
    for (slot = first; slot <= last; slot++) {
        WQ_UNIT u;
        if (!d->q->ops->unit(d->q, slot, &u)) {
            if (job_held(&in->job[slot])) job_clear(&in->job[slot]);
            continue;
        }
        n++;
        census_unit(d, &u);
    }
    return n;
}

/* the base box: every building that is not an extractor or a plant within base_r of the anchor */
static void base_box(IMPAI_DEC* d)
{
    const IMPAI_BRAIN* st = d->in->st;
    const int32_t apos[3] = { (int32_t)((uint32_t)(st->ax * 16 + 8) << 16), 0, (int32_t)((uint32_t)(st->az * 16 + 8) << 16) };
    int i;
    for (i = 0; i < d->nstructs; i++) {
        const IMPAI_STRUCT* s = &d->structs[i];
        if (s->cat == IMPAI_C_MEX || s->cat == IMPAI_C_GEO ||
            impai_dist_px(apos, (s->r.x0 + s->r.x1 + 1) * 8, (s->r.z0 + s->r.z1 + 1) * 8) > d->in->plan->base_r)
            continue;
        if (!d->has_bbox) { d->bbox = s->r; d->has_bbox = 1; continue; }
        if (s->r.x0 < d->bbox.x0) d->bbox.x0 = s->r.x0;
        if (s->r.z0 < d->bbox.z0) d->bbox.z0 = s->r.z0;
        if (s->r.x1 > d->bbox.x1) d->bbox.x1 = s->r.x1;
        if (s->r.z1 > d->bbox.z1) d->bbox.z1 = s->r.z1;
    }
}

/* ---- 2. the job checks ---- */

static const IMPAI_STRUCT* built_at(const IMPAI_DEC* d, int type, int cx, int cz, int unfinished_only)
{
    int i;
    for (i = 0; i < d->nstructs; i++)
        if (d->structs[i].type == type && inside(&d->structs[i].r, cx, cz) &&
            (!unfinished_only || !d->structs[i].finished))
            return &d->structs[i];
    return NULL;
}

void impai_sea_refusal(IMPAI_DEC* d)
{
    IMPAI_BRAIN* st = d->in->st;
    if (d->sea_counted) return;
    d->sea_counted = 1;
    if (st->sea_refused && d->now - st->sea_counted_at < d->in->plan->fail_t) return;
    st->sea_counted_at = d->now;
    if (st->sea_refused < 255) st->sea_refused++;
}

/* a failed BUILD job's site: avoided for fail_t, and when the job was a water factory's, a refusal of the seat's
   sea plant (impai_build.c, THE SEA PLANT's termination) */
static void build_failed(IMPAI_DEC* d, const IMPAI_JOB* j)
{
    impai_fail_add(d, j->cx, j->cz);
    if (impai_water_plant(d->in, j->build)) impai_sea_refusal(d);
}

/* THE REACH ABANDON, MobileBuild's own test (0x403A20, its state 1 on the move's end event 0x40,
   0x403B63..0x403C56, DISASSEMBLED): the integer distance from the builder (+0x6A, +0x72) to the site's centre
   (node +0x22, +0x2A), less the truncated 8 x hypot of the builder's footprint (+0x7E, +0x80, its def's +0x14A)
   and 8 x hypot of the site type's (+0x14A, +0x14C), compared with the builder's builddistance (def +0x212):
   past it the order ends with "I can't reach the construction site" (0x501580, 0x403C3D..0x403C56, result 8),
   and the controller puts the unit's Standby at the head (engine/orders.md). 1 when the builder stands where
   that test fails, read at the census after the job ended: the builder never came within reach of the site,
   which says nothing of the site. The distance is the census's integer px (impai_dist_px) where the engine
   truncates the 16.16 one, and the builder may have moved since the order ended -- Standby's state 0 stops it
   (0x40605C) -- so this is the same compare on the census's reading, not a replay of the engine's. A builder
   with no def is not beyond reach. The site's footprint is sfx x sfz cells, its centre at px (pcx, pcz). */
static int foot_out_of_reach(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int pcx, int pcz, int sfx, int sfz)
{
    const IMPAI_TINFO* bt = &d->in->ti[b->type];
    WQ_DEF def;
    int bfx, bfz;
    int64_t gap;
    if (!d->q->ops->def(d->q, b->type, &def)) return 0;
    bfx = bt->fx > 0 ? bt->fx : 1; bfz = bt->fz > 0 ? bt->fz : 1;
    if (sfx < 1) sfx = 1;
    if (sfz < 1) sfz = 1;
    gap = (int64_t)impai_dist_px(b->pos, pcx, pcz) -
          impai_isqrt(64u * (uint64_t)(bfx * bfx + bfz * bfz)) - impai_isqrt(64u * (uint64_t)(sfx * sfx + sfz * sfz));
    return gap > (int64_t)def.reach;
}

/* the same for a site of `type`; a type with no information is not beyond reach */
static int out_of_reach(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int pcx, int pcz)
{
    if (type < 0 || type >= (int)d->in->nti) return 0;
    return foot_out_of_reach(d, b, pcx, pcz, d->in->ti[type].fx, d->in->ti[type].fz);
}

static int site_out_of_reach(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz)
{
    const IMPAI_TINFO* st = type >= 0 && type < (int)d->in->nti ? &d->in->ti[type] : NULL;
    const int sfx = st && st->fx > 0 ? st->fx : 1, sfz = st && st->fz > 0 ? st->fz : 1;
    return out_of_reach(d, b, type, (2 * cx + sfx) * 8, (2 * cz + sfz) * 8);
}

/* THE FAR RECORD. A builder that ended its build beyond its reach, or a reclaim short of its feature, is the one
   held back, for fail_t: no other builder and no site is, since the cause may be where the builder stands (a
   constructor walled in by its own plant's neighbours, MEASURED 2026-11-13 on Pincushion and Trout Farm, every
   patch of the map failed in turn). While the record is live the builder avoids the spot it recorded
   (IMPAI_JOB.far_cx, far_cz), and is given no site and no feature beyond its reach from where it stands -- the
   same compare as the abandon's -- so it cannot be sent from one unreachable site to the next, each failing in
   turn, nor back to the spot it was refused at while standing by it; and it
   takes no factory, sea-plant or tech-up order (impai_econ.c, assign), which a builder that can walk is given, so
   it cannot hold the seat's one plant want either. TERMINATION: the record ends at far_until, and the builder
   then takes any site again; a builder still walled in then ends one more build beyond reach and is held back
   one more fail_t, so its sites cost one job each fail_t, not one each decision. A water factory's still counts a
   refusal of the sea plant, its search's termination (impai_build.c, THE SEA PLANT). */
static void build_unreached(IMPAI_DEC* d, IMPAI_JOB* j)
{
    j->far_cx = j->cx;
    j->far_cz = j->cz;
    j->far_type = j->type;
    j->far_until = d->now + d->in->plan->fail_t;
    if (impai_water_plant(d->in, j->build)) impai_sea_refusal(d);
}

int impai_far(const IMPAI_DEC* d, const IMPAI_UNITREF* b) { return d->in->job[b->slot].far_until > d->now; }

int impai_far_in(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_RECT* r)
{
    const IMPAI_JOB* j = &d->in->job[b->slot];
    return impai_far(d, b) && inside(r, j->far_cx, j->far_cz);
}

int impai_far_px(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int pcx, int pcz)
{
    return impai_far(d, b) && out_of_reach(d, b, type, pcx, pcz);
}

int impai_far_at(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz)
{
    const IMPAI_RECT r = impai_rect_at(d, type, cx, cz);
    return impai_far_in(d, b, &r) || (impai_far(d, b) && site_out_of_reach(d, b, type, cx, cz));
}

int impai_far_feat(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_RECT* r, int pcx, int pcz)
{
    return impai_far_in(d, b, r) ||
           (impai_far(d, b) && foot_out_of_reach(d, b, pcx, pcz, r->x1 - r->x0 + 1, r->z1 - r->z0 + 1));
}

/* A reclaim's feature this builder did not get to -- its order ended or its lifetime ran out with the feature
   still there or out of sight -- records the builder's far record at the feature (THE FAR RECORD), as a site
   it could not reach does: the cause may be where it stands, so no other builder is held back. */
static void reclaim_unreached(IMPAI_DEC* d, IMPAI_JOB* j)
{
    j->far_cx = j->cx;
    j->far_cz = j->cz;
    j->far_type = j->type;
    j->far_until = d->now + d->in->plan->fail_t;
}

/* THE COMMANDER'S MARKERS. The commander is the one unit both managers order, and with a reaction an order of
   either can be pending while the other decides over a head it is about to replace. So each leaves a marker of
   its own intent, set at decision time from the order's answer: IMPAI_BRAIN.com_until here, IMPAI_MIL.com_until
   in the military's order() (impai_mil.c), each the decision's GameTime + react. The economy's census treats the
   commander as the military's while either of the military's fields lasts (THE COMMANDER'S WINDOW); the
   military skips its head's dedupe on the commander while the economy's lasts, so its flight is not dropped
   against a head the economy's pending order replaces. The orders are given first in first out, in decision
   order, so the later decision's order is the last given, as with no reaction. A persona with no reaction
   leaves both markers alone, and every test of them is false; a marker counts only within one reaction of now
   (impai_com_marked), so a recorded one is bounded as press_until is. */
int impai_give(IMPAI_DEC* d, const WQ_CMD* c)
{
    int rc;
    d->roots++;
    rc = d->q->ops->order(d->q, c);
    if (rc == WQ_OK && d->in->persona->react && d->commander.slot && c->unit == d->commander.slot)
        d->in->st->com_until = d->now + d->in->persona->react;
    return rc;
}

/* THE STOP of a job past its lifetime: one a decision while the commands last -- resolver mode 10, then the
   order. 1 when the decision may stop it (whether or not the resolver answered); 0 when its stop is spent. */
static int job_stop(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    WQ_CMD c;
    int a;
    if (d->stopped || d->roots >= d->root_cap) return 0;
    d->stopped = 1;
    if ((a = d->q->ops->can(d->q, b->slot, 10, (int)WQ_NONE, b->pos)) > 0) {
        memset(&c, 0, sizeof c);
        c.verb = WQ_CMD_NEW;
        c.action = (uint8_t)a;
        c.unit = b->slot;
        c.target = (uint16_t)WQ_NONE;
        memcpy(c.pos, b->pos, sizeof c.pos);
        impai_give(d, &c);
    }
    return 1;
}

/* 1 when the builder's head order is no longer the job's: a busy head whose point lies outside the feature's
   footprint (the memory's, a cell where it holds none). A reclaim is ordered at the feature's centre and a walk
   to a remembered one at the same point (impai_build.c, THE ORDER), so a head anywhere else was given by
   another hand -- the military's, for the commander -- and the reclaim it replaced is over: its countdown lives
   on the node (engine/orders.md, The reclaim order's countdown). */
static int reclaim_replaced(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_JOB* j)
{
    const int k = impai_fmem_find(d->in->st, j->cx, j->cz);
    const IMPAI_RECT r = impai_feat_rect(j->cx, j->cz, k >= 0 ? d->in->st->fmem[k].fx : 1,
                                         k >= 0 ? d->in->st->fmem[k].fz : 1);
    WQ_ORDER o;
    if (d->q->ops->head(d->q, b->slot, &o) != 1) return 0;
    return !inside(&r, o.pos[0] >> 20, o.pos[2] >> 20);
}

/* THE RECLAIM'S CHECK, every decision, by the seat's own sight only (reclaim_seen; the cell is never read out
   of it). Each way it ends has its reason:
   - SEEN GONE -- reclaimed, by this builder or by anything else, burnt, crushed, or playing the sequence a paid
     reclaim or a death began -- a fact for the whole seat: it leaves the memory, so no builder is sent to it
     again, the job ends with no failure, and a builder still on its way is listed idle so that this decision
     gives it its next task. The seat's own payouts are counted where the engine makes them (tagpu_impai.c,
     THE PAYOUT), not here: the feature alone cannot say who took it.
   - ARRIVED: a RECLAIM_GO whose feature comes into sight and is there is ordered to reclaim it
     (impai_reclaim_arrived), the decision's commands permitting; a refusal there is the builder's to avoid.
   - REPLACED (reclaim_replaced): the builder is busy with an order that is not the job's; the job ends with no
     failure and the claim on the feature with it.
   - GIVEN UP: a builder idle, or past job_timeout and stopped, with its feature out of sight or still there:
     it alone avoids it (reclaim_unreached). */
static void reclaim_check(IMPAI_DEC* d, const IMPAI_UNITREF* b, int idle, IMPAI_JOB* j, uint32_t age)
{
    IMPAI_BRAIN* st = d->in->st;
    WQ_FEAT f;
    const int seen = reclaim_seen(d, j, &f);
    if (seen == 0) {
        const int k = impai_fmem_find(st, j->cx, j->cz);
        if (k >= 0) impai_fmem_drop(st, k);
        impai_job_end(j);
        if (!idle && d->nidle < d->in->plan->idle_max && d->nidle < IMPAI_IDLE_CAP) d->idle[d->nidle++] = *b;
        return;
    }
    if (seen > 0 && j->kind == IMPAI_JOB_RECLAIM_GO) {
        const int r = impai_reclaim_arrived(d, b, &f);
        if (r >= 0) return;
        reclaim_unreached(d, j);
        impai_job_end(j);
        return;
    }
    if (!idle && reclaim_replaced(d, b, j)) {
        impai_job_end(j);
        return;
    }
    if (idle || (age > d->in->plan->job_timeout && job_stop(d, b))) {
        reclaim_unreached(d, j);
        impai_job_end(j);
    }
}

/* At most job_checks, in slot order. An idle builder's BUILD is done when a building of the type ordered
   covers the site; else the builder could not reach the site (THE REACH ABANDON), which holds it back (THE FAR
   RECORD), or the site failed. A busy one older than job_timeout with no frame of it at the site is stopped
   (job_stop) and the site failed. An EXPLORE ends when its cell is explored, or after job_timeout with the cell
   failed. A reclaim (reclaim_check) ends the moment the seat sees its feature gone. */
static void job_checks(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i, checks = 0;
    for (i = 0; i < d->njobbed && checks < in->plan->job_checks; i++) {
        const IMPAI_UNITREF* b = &d->jobbed[i];
        IMPAI_JOB* j = &in->job[b->slot];
        const uint32_t age = d->now - j->since;
        if (j->kind == IMPAI_JOB_BUILD) {
            if (d->jobbed_idle[i]) {
                checks++;
                if (!built_at(d, j->build, j->cx, j->cz, 0)) {
                    if (site_out_of_reach(d, b, j->build, j->cx, j->cz)) build_unreached(d, j);
                    else build_failed(d, j);
                }
                impai_job_end(j);
            } else if (age > in->plan->job_timeout) {
                checks++;
                if (!built_at(d, j->build, j->cx, j->cz, 1) && job_stop(d, b)) {
                    build_failed(d, j);
                    impai_job_end(j);
                }
            }
        } else if (reclaim_job(j)) {
            checks++;
            reclaim_check(d, b, d->jobbed_idle[i], j, age);
        } else if (j->kind == IMPAI_JOB_EXPLORE) {
            checks++;
            if (d->q->ops->explored(d->q, j->cx, j->cz)) impai_job_end(j);
            else if (age > in->plan->job_timeout) {
                impai_fail_add(d, j->cx, j->cz);
                impai_job_end(j);
            }
        }
    }
}

/* THE WALK CHECK, an ORDERING: a constructor's job the seat learns is threatened ends within one decision. For
   every busy constructor of the census's jobbed list -- never the commander, whose safety is the military's
   (impai_mil.c), so two managers never order it in one decision --, at its job's point: a BUILD's footprint
   centre, an EXPLORE's cell centre, an ASSIST's target where the census found it. A BUILD with no building of
   its type at the site yet, and an EXPLORE, are walks, turned back when the point or the straight way there holds
   known ground threat (impai_walk_threatened; the builder's own cell is left out, so one walking out of threat
   goes on); a BUILD whose frame stands and an ASSIST are work, turned back when the point's cell or the builder's
   own holds it -- the threat has come within reach of where it works; a REPAIR is work at its mender, and turned
   back too when the mend it serves has ended (its own comment, below). impai_turn_back ends the job; an EXPLORE's
   cell also fails for fail_t. The builder, holding the move and no job, is listed idle from the next decision on
   (THE HAND-BACK), and the ladder gives it a site THE ROUTE has tested (impai_build.c). A ground defence's builder
   is no exception: one sent to a site at the edge of the threat it answers is turned back by this work test once
   the threat's reach covers its own cell or the frame's; only the commander is exempt. Outside job_checks, which
   starts at the slot order's first builder each decision and would starve the later ones if every walk spent a
   check. BOUND: at most 2 x IMPAI_IDLE_CAP walks tested a decision, one route each; the orders within the roots
   the factories leave (THE FACTORIES' ROOTS). The turn back is given within look_eco + react ticks of the seat's
   rows showing the threat -- one decision, then the persona's reaction: 15 at Hard, 105 at Easy -- when that
   decision has a command left. Under the economy's bucket (impai_react.c, THE BUDGET) one with none gives no turn
   back, and the builder waits for a decision with an action: the bucket refills one every 1800 / apm_eco ticks
   (180 at Easy, 75 at Normal), and the job checks' stops and the earlier turn backs take theirs first. MEASURED
   2026-11-13, Comet Catcher, every one of the 23 constructors a human's line killed on an expansion walk had
   known ground threat on its remaining way 34 to 2 619 ticks before it died, so Hard turns every one of them back
   and a lower level misses the warnings shorter than its look and reaction, the weakness the level is. RESIDUAL: a builder learns a stretch of the threat from inside its reach when the enemy
   outranges its sight (a CORCV sees 200 px, a Samson reaches 600), and turns back under fire; a RECLAIM_GO is
   tested at its choice only (its walk is within IMPAI_RECLAIM_R); an ASSIST's target that is not an unfinished
   building or mobile unit of the census's IMPAI_NANO_MAX each, nor a busy factory of its IMPAI_IDLE_CAP (a guard
   a recording holds), is not tested. */
static const IMPAI_UNITREF* assist_target(const IMPAI_DEC* d, uint16_t slot)
{
    int k;
    for (k = 0; k < d->nnano; k++) if (d->nano[k].slot == slot) return &d->nano[k];
    for (k = 0; k < d->nframes; k++) if (d->frames[k].slot == slot) return &d->frames[k];
    for (k = 0; k < d->nfbusy; k++) if (d->fbusy[k].slot == slot) return &d->fbusy[k];
    return NULL;
}

static void walk_checks(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i;
    for (i = 0; i < d->njobbed && d->roots < d->root_cap; i++) {
        const IMPAI_UNITREF* b = &d->jobbed[i];
        IMPAI_JOB* j = &in->job[b->slot];
        int x, z, back;
        if (d->jobbed_idle[i] || in->ti[b->type].cat != IMPAI_C_CONS) continue;
        if (j->kind == IMPAI_JOB_BUILD && j->build < in->nti) {
            const IMPAI_TINFO* t = &in->ti[j->build];
            x = (2 * j->cx + (t->fx > 0 ? t->fx : 1)) * 8;
            z = (2 * j->cz + (t->fz > 0 ? t->fz : 1)) * 8;
            back = built_at(d, j->build, j->cx, j->cz, 0)
                       ? impai_threatened(d, x, z) || impai_threatened(d, b->pos[0] >> 16, b->pos[2] >> 16)
                       : impai_walk_threatened(d, b, x, z);
            if (back) impai_turn_back(d, b);
        } else if (j->kind == IMPAI_JOB_ASSIST) {
            const IMPAI_UNITREF* t = assist_target(d, j->target);
            if (t && (impai_threatened(d, t->pos[0] >> 16, t->pos[2] >> 16) ||
                      impai_threatened(d, b->pos[0] >> 16, b->pos[2] >> 16)))
                impai_turn_back(d, b);
        } else if (j->kind == IMPAI_JOB_EXPLORE) {
            const int cx = j->cx, cz = j->cz;
            if (impai_walk_threatened(d, b, cx * 16 + 8, cz * 16 + 8) && impai_turn_back(d, b)) impai_fail_add(d, cx, cz);
        } else if (j->kind == IMPAI_JOB_REPAIR) {
            /* THE REPAIR'S END. INVARIANT, an ordering by the job field: a repairer serves only a unit the military
               holds in IMPAI_JOB_MEND within IMPAI_MEND_NEAR_PX of its haven. RepairUnit ends by itself at the
               mender's full health or death (impai_build.c, THE REPAIR), and every other end of the mend -- the
               military's release, the slot taken by another unit, the mender walked off its haven -- is read
               here, with THE ASSIST's work test on the threat, and the first walk check that reaches the
               repairer with a command left turns it back. The target slot and the mender's job are recorded
               data, each bounded by its table before it is read. RESIDUAL: while no command is left, the
               repairer keeps its order (THE TURN BACK's bound). */
            const unsigned t = j->target;
            const IMPAI_JOB* mj = t < in->njob ? &in->job[t] : NULL;
            WQ_UNIT m;
            const int back = !mj || mj->kind != IMPAI_JOB_MEND || !d->q->ops->unit(d->q, (int)t, &m) ||
                             m.type != mj->type ||
                             impai_dist_px(m.pos, mj->cx * 32 + 16, mj->cz * 32 + 16) > IMPAI_MEND_NEAR_PX ||
                             impai_threatened(d, m.pos[0] >> 16, m.pos[2] >> 16) ||
                             impai_threatened(d, b->pos[0] >> 16, b->pos[2] >> 16);
            if (back) impai_turn_back(d, b);
        }
    }
}

/* THE YARD, an ORDERING: no builder the economy lists idle or as a helper stands on a waiting factory's yard past
   one economy decision. A factory's next product is created only when the engine's placement test passes its yard:
   BuildingBuild 0x402640 calls 0x47DB70 at 0x402899 with the unit to ignore pushed as 0 at 0x402896, and on a
   refusal sleeps 15 ticks (0x4028A2..0x4028A6) and asks again, for ever; for a mobile def the test refuses any
   footprint cell whose slot word names a unit other than the ignored one (0x47DCFD..0x47DD0A, the refusal at
   0x47DD99) -- DISASSEMBLED 2026-11-14, engine/map.md's "A lab can open its yard". So a builder standing on the
   yard holds the factory as long as it stands, and an idle builder with every rule closed stands for ever: on
   Trout Farm the seat's one factory waited 27 450 ticks on a constructor guarding it, and 61 350 in another game
   (MEASURED 2026-11-14, the closing run's four Trout Farm games). A WAITING FACTORY is a finished one of the
   census's busy list whose head is a BuildingBuild (fbusy_q) and whose footprint holds none of the census's
   unfinished mobile units; each idle builder and helper on its yard is moved off (impai_yard_leave) and leaves
   its list, so no later rule of this decision orders it again. The commander inside THE COMMANDER'S WINDOW is in
   neither list and is left to the military; an idle builder whose BUILD or reclaim the job checks have not ended
   is left to them, as the ladder leaves it; a busy builder only crosses a yard, since no site lies on one. A
   builder that walks back to help the factory's next product is the help's, which ends with the product (THE
   ASSIST, impai_build.c). BOUND: at most IMPAI_IDLE_CAP factories, each against the census's IMPAI_NANO_MAX
   frames and its at most 2 x IMPAI_IDLE_CAP listed builders, one move a builder, within the roots the factories
   leave (THE FACTORIES' ROOTS). The factory waits at most look_eco + react ticks on the economy's builders per
   product, when the decision has a command left. RESIDUAL: a frame past the census's IMPAI_NANO_MAX is not seen,
   and the builders by its factory are moved off a yard that is producing, one move each. */
static int yard_leave_list(IMPAI_DEC* d, IMPAI_UNITREF* list, int* n, const IMPAI_RECT* r, int idle)
{
    int i = 0, k, moved = 0;
    while (i < *n && d->roots < d->root_cap) {
        const IMPAI_JOB* j = &d->in->job[list[i].slot];
        if ((idle && (j->kind == IMPAI_JOB_BUILD || reclaim_job(j))) || !impai_yard_leave(d, &list[i], r)) {
            i++;
            continue;
        }
        for (k = i + 1; k < *n; k++) list[k - 1] = list[k];
        (*n)--;
        moved++;
    }
    return moved;
}

static void yard_clear(IMPAI_DEC* d)
{
    int f, k;
    for (f = 0; f < d->nfbusy && d->roots < d->root_cap; f++) {
        const IMPAI_RECT* r = NULL;
        if (!d->fbusy_q[f]) continue;
        for (k = 0; k < d->nstructs && !r; k++)
            if (d->structs[k].slot == d->fbusy[f].slot) r = &d->structs[k].r;
        if (!r) continue;
        for (k = 0; k < d->nframes; k++)
            if (inside(r, (d->frames[k].pos[0] >> 16) >> 4, (d->frames[k].pos[2] >> 16) >> 4)) break;
        if (k < d->nframes) continue;
        d->out->yard += (uint16_t)yard_leave_list(d, d->idle, &d->nidle, r, 1);
        d->out->yard += (uint16_t)yard_leave_list(d, d->helpers, &d->nhelpers, r, 0);
    }
}

/* ---- 3. the economy ---- */

/* The economy's wants. THE SECOND FACTORY WAITS FOR THE NAVAL JUDGEMENT (IMPAI_MIL.naval >= 0), an ORDERING: the
   military judges it (impai_mil.c, naval) and the seat's next factory after its first is a sea plant when it
   says so, so the order the two managers run in can never decide which kind of plant takes the slot -- a land
   plant cannot take the slot the sea plant was judged for because the economy ran first. A military that
   refuses every decision holds the seat at one factory; its level-end line's `refused=` shows it. THE SEA PLANT
   takes factory_per_m's pacing like any second factory: a plant started on the commander's own income alone
   spends the storage the economy runs on and holds its builder on a frame it cannot finish (MEASURED
   2026-11-13, Trout Farm, Arm against Arm: a shipyard begun at a metal income of 1 a pass emptied the seat's
   metal by GameTime 4 207, and the seat completed no unit from 3 607 to 12 607). It is wanted only while some
   builder offers one (impai_econ_sea_able): without that a seat none of whose builders can build one would
   hold its second factory for ever. */
/* THE DEFENCE'S POINT (impai_econ.c, THE GROUND DEFENCES): the anchor moved rally_px towards the pressing member's
   block, as the military's rally point is moved towards the enemy base, or that block itself when nearer. The
   block is recorded data: an int16 block, at most 2^20 px from the anchor, so the product below stays within
   int64, and the point it gives only keys the ring's order -- the ring's sites are the base box's whatever it is. */
static void def_point(IMPAI_DEC* d, const IMPAI_MIL* m)
{
    const int32_t ax = d->in->st->ax * 16 + 8, az = d->in->st->az * 16 + 8;
    const int32_t px = m->press_bx * 32 + 16, pz = m->press_bz * 32 + 16;
    const int64_t dx = (int64_t)px - ax, dz = (int64_t)pz - az;
    const uint32_t de = impai_isqrt((uint64_t)(dx * dx + dz * dz));
    const uint32_t r = d->in->plan->rally_px;
    d->def_x = de > r ? ax + (int32_t)(dx * r / de) : px;
    d->def_z = de > r ? az + (int32_t)(dz * r / de) : pz;
}

/* metal as whole units held to 0..10^9: a store or storage read from the record is data; NaN, a negative value and
   anything below 1 read 0 */
static int64_t metal_whole(float v) { return v >= 1.0f ? (v < 1e9f ? (int64_t)v : 1000000000) : 0; }

/* the surplus, the pressure and THE HOLD, read from the seat's record and its military alone, before the census's
   roots are cut (root_cap_of) */
static void hold_read(IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const IMPAI_MIL* mil = d->in->mil;
    const WQ_ECON* e = &d->e;
    d->surplus = e->metal_max > 0.0f && e->metal >= SURPLUS_FRAC * e->metal_max;
    /* THE PRESSURE as the economy reads it (impai_mil.c): a recorded press_until is data, and one past now +
       press_hold holds nothing, so a byte restored from a recording can hold the constructors for at most
       press_hold */
    d->pressed = mil && mil->press_until > d->now && mil->press_until - d->now <= plan->press_hold;
    /* THE HOLD. While the stored metal is below hold_pm thousandths of the storage, outside a surplus and outside
       THE PRESSURE, the seat holds every spend that is not economy: at most hold_fac factories busy (impai_econ.c,
       THE GOVERNOR), no plant past the first (want_factory's second clause, want_sea, want_tech below), the assist
       only on an economy building's frame (impai_build.c, THE ASSIST), and a helper on anything else given the
       ladder or stopped (impai_econ.c, THE RELEASE). The economy's own buildings and every builder's own build go
       on, so a plant begun before the hold is finished by its builder.
       WHY: the economy pass pays a seat's requests at one fraction per resource (0x401A4D..0x401AB1,
       engine/weapons.md, The settle), so nothing but what is asked decides what is paid. MEASURED 2026-11-14 over
       57 ImpAI seat-games of the bar, GameTime 9 000..27 000: a stalled seat was paid 44 % of what it asked; its
       factories asked 43 % of it, plants and help on factories' products 41 %, and the economy's buildings 15 %.
       THE STORE ALONE, not m_stall: m_stall also needs income below demand, which turns false the pass after the
       hold cuts the demand, so the factories would be released into an empty store at the next decision and the
       stall would come straight back; hold_pm of the storage is the buffer the released factories start on.
       THE PRESSURE lifts it: the army is what a pressed seat needs, and want_def needs `pressed`, so the
       defences and the hold exclude each other by construction. Seen air (want_aa) is a reaction to a threat and
       is not held.
       INVARIANT, a bound on the seat's own record: the store and storage (+0x98, +0xA8, through the WORLDQ) are
       whole metal held to 0..10^9 (metal_whole), so the products stay below 10^9 x 65 535 < 2^47; storage it
       cannot read (0, NaN) never holds; and the hold is never set beside a surplus, by its own term, so an idle
       factory beside a store at SURPLUS_FRAC or more is never held -- hold_pm < SURPLUS_FRAC x 1000
       (impai_brain_test.c, check_plan) keeps the band between the two non-empty. `pressed` is the seat's own
       stamp, bounded above. */
    {
        const int64_t s = metal_whole(e->metal), m = metal_whole(e->metal_max);
        d->hold = m > 0 && s * 1000 < m * (int64_t)plan->hold_pm && !d->surplus && !d->pressed;
    }
}

static void economy(IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const IMPAI_MIL* mil = d->in->mil;
    const WQ_ECON* e = &d->e;
    const float fac = (float)d->count[IMPAI_C_FACTORY];
    const int mex_done = d->done[IMPAI_C_MEX] > 0 ? d->done[IMPAI_C_MEX] : 1;
    const int judged = !mil || mil->naval >= 0;
    /* THE STEADY INCOME: the seat's metal a pass, less what the last pass settled from writers outside it.
       - The economy pass 0x401360 writes a unit's produced metal +0xD4 only with an extractor's ExtractsMetal x
         SetSpeed (0x401480..0x401545), a maker's makesmetal (0x4014CA..0x4014F7) and a finished unit's MetalMake
         (0x4016DC..0x401795). A type-2 owner's difficulty scales each by 0.5, 0.7 or 1 (0x401504..0x40153A,
         0x401759..0x40178D; engine/sim.md, Difficulty and the AI handicap). So a unit that is not an extractor
         produces at most its metalmake + makesmetal (IMPAI_TINFO.mgain) a pass.
       - Between passes +0xD4 is written by the reclaim completion 0x4237D0 (a feature's metal, 0x42395B),
         BuildingBuild (a product torn down unfinished, its built fraction of its metal cost, 0x402666..0x4026E3),
         the unbuild (0x41BBD0..0x41BBF4) and the destructor (a reclaimed unit's built fraction of its metal cost,
         0x486CA2..0x486D1E): payouts, credits and refunds, never income that recurs (replay-hold.tsv's +0xD4 row).
       - The settle moves +0xD4 to +0xE4 (0x401B75..0x401B7B) in the call that sums it into +0x9C (0x401A05). The
         part of a non-extractor's +0xE4 above its mgain (the census, WQ_UNIT.made_m) is what the gates must not
         count. MEASURED 2026-11-14 on 28 games of ImpAI against OTA AI on six setups: 17 of 65 plant wants rode on
         such a payout, 13 of Twilight's 14, each below its gate without it.
       - BOUND: each term is in [0, +0xE4] and each +0xE4 is one of +0x9C's summands, so m_steady is in
         [0, metal_in]; a NaN metal_in fails the compare and reads 0.
       - ORDERING: the decision reads +0x9C and every +0xE4 on the game thread between two passes, so all of them
         come from one settle.
       - RESIDUAL: a payout to a unit that died since the pass leaves with it and counts once as income; an
         extractor that reclaims has its payouts counted; a yardless extractsmetal structure, which the WORLDQ does
         not class MEX, loses its extraction from m_steady; the energy twin -- tree payouts in energy_in, and so in
         pe_in and e_stall -- is not subtracted.
       Every income gate below and the reclaim's extractor rate (impai_econ.c, THE LADDER) read m_steady:
       metal_in is read here alone. */
    d->m_steady = e->metal_in > d->m_paid ? e->metal_in - d->m_paid : 0.0f;
    d->e_stall = e->energy < 0.2f * e->energy_max && e->energy_in < e->energy_out;
    d->m_stall = e->metal < 0.2f * e->metal_max && d->m_steady < e->metal_out;
    d->out->pressed = (uint8_t)d->pressed;
    d->out->hold = (uint8_t)d->hold;
    d->pe_in = e->energy_in + d->pe_extra;
    d->pm_in = d->m_steady * (1.0f + (float)d->pending[IMPAI_C_MEX] / (float)mex_done);
    /* IN A SURPLUS THE FACTORIES ARE PACED BY INCOME, not held to factory_max: a plant past factory_max is wanted
       while the store is at SURPLUS_FRAC of its maximum or more, on the second factory's own terms -- factory_per_m
       of steady income a pass for each factory standing, no stall, no hold, one plant at a time (pending). INVARIANT,
       a bound: a plant is wanted only while m_steady >= factory_per_m x the count, so the count is at most
       m_steady / factory_per_m + 1, bounded by the map's income, and grows by at most one pending plant at a time.
       Not gated on ftop_n: the measured end state, factories {T1: 2, T2: 1}, has ftop_n 1, below factory_max, so the
       tech-up's own test never holds and the seat stopped at three. WHY: every long game against the stock AI ended
       with 3 factories spending about 25-36 metal a second of an income of 56-95, the store full, while the stock AI
       built 12-22 (MEASURED 2026-11-14 from the recordings). A tech-up due beside it comes first (impai_econ.c,
       THE TECH-UP FIRST); otherwise THE CHOICE takes a plant of build tech at most the seat's highest, by its
       units' values, and with none the least value: a T1 plant while one is the cheapest -- a GAP, since in a
       surplus the dearer plant would spend more. m_short's want stays the tech-up's tech_m past factory_max. */
    d->want_factory = !d->pending[IMPAI_C_FACTORY] &&
        ((d->count[IMPAI_C_FACTORY] == 0 &&
          ((d->done[IMPAI_C_MEX] >= plan->open_mex && d->count[IMPAI_C_ENERGY] >= plan->open_energy) || d->now >= plan->open_t)) ||
         (d->count[IMPAI_C_FACTORY] >= 1 && (d->count[IMPAI_C_FACTORY] < plan->factory_max || d->surplus) && judged &&
          d->m_steady >= d->factory_per_m * fac && !d->e_stall && !d->m_stall && !d->hold));
    d->want_sea = mil && mil->naval == 1 && !d->sea_plants && d->count[IMPAI_C_FACTORY] >= 1 &&
                  d->count[IMPAI_C_FACTORY] < plan->factory_max && !d->pending[IMPAI_C_FACTORY] &&
                  d->m_steady >= d->factory_per_m * fac && !d->e_stall && !d->m_stall &&
                  d->in->st->sea_refused < IMPAI_SEA_REFUSALS && !d->hold && impai_econ_sea_able(d);
    /* TECH-UP: with factory_max factories of its highest build tech (IMPAI_TINFO.ctech: the tech its plants'
       constructors build, frames included) and a steady metal income (THE STEADY INCOME) of the table's tech_m a
       pass, the next factory's build tech is higher (impai_econ.c, THE CHOICE) */
    if (d->in->mil) {
        const IMPAI_MIL* m = d->in->mil;
        const float tech_m = m->table.tech_m;
        d->want_tech = !d->pending[IMPAI_C_FACTORY] && tech_m > 0 && d->m_steady >= tech_m &&
                       d->ftop_n >= plan->factory_max && !d->e_stall && !d->m_stall && !d->hold;
        /* ANTI-AIR STRUCTURES: one per aa_struct_v of seen enemy air value, at most aa_struct_max */
        d->want_aa = plan->aa_struct_v > 0 && d->aa_n < plan->aa_struct_max &&
                     m->air_seen >= plan->aa_struct_v * (float)(d->aa_n + 1);
        /* THE GROUND DEFENCES (impai_econ.c): one while THE PRESSURE stands at the base, one more for each def_v
           of pressing value, at most def_max. A NaN press_v fails the compare and wants nothing. */
        d->want_def = d->pressed && m->press_base && d->def_n < plan->def_max && plan->def_v > 0.0f &&
                      (float)d->def_n < 1.0f + m->press_v / plan->def_v && !d->e_stall && !d->m_stall;
        if (d->want_def) def_point(d, m);
    }
    /* SHORT OF METAL for what the seat wants next: stalling on it, or making less a pass than its next want
       asks -- the next factory's factory_per_m x the factories it has (the second factory's test above, the
       first's taken as one), and with factory_max of them the tech-up's tech_m. A seat short of metal takes a
       feature before the buildings that spend (impai_econ.c, THE LADDER). THE ROOM: what its store can still
       take, less what its reclaims under way will pay (the census); a payout past it is lost, the economy pass
       holding the store to its maximum (0x401AFE..0x401B27, engine/weapons.md, The settle). */
    {
        const int nf = d->count[IMPAI_C_FACTORY];
        float want_m = nf < plan->factory_max ? d->factory_per_m * (float)(nf > 1 ? nf : 1) : 0.0f;
        if (nf >= plan->factory_max && mil) want_m = mil->table.tech_m;
        d->m_short = d->m_stall || d->m_steady < want_m;
        d->m_room = e->metal_max - e->metal - d->m_inflight;
    }
}

/* ---- the entry ---- */

/* THE FACTORIES' ROOTS, a bound on counts: the decision gives at most roots_max commands, the plan's roots or
   fewer when the economy's bucket holds fewer actions (impai_react.c, THE BUDGET; a command the order refused
   costs its action as a misclick costs a player one). The orders before the factories' -- the job checks' stops,
   the walk checks' turn backs, the ladder's -- spend at most roots_max less the factories' reserve, so every
   idle factory up to the reserve has a root for its order (impai_econ.c, THE GOVERNOR), however many builders a
   raid turns back. A decision with no idle factory reserves none, and one in THE HOLD no more than the floor's
   max(0, hold_fac - nfbusy), the most orders its walk gives (impai_econ.c, THE HOLD'S FLOOR). The factories' own
   loop runs to roots_max.
   THE SPLIT: the reserve is also at most roots_max / 2, so a budget of one or two actions goes to the builders
   first and the factories take what is left. The plan's queue_max is at most roots / 2 (impai_brain_test.c,
   check_plan), so with no budget the halving never binds. */
static int root_cap_of(const IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const int floor = plan->hold_fac > d->nfbusy ? plan->hold_fac - d->nfbusy : 0;
    int reserve = d->nfidle < plan->queue_max ? d->nfidle : plan->queue_max;
    if (d->hold && floor < reserve) reserve = floor;
    if (reserve > d->roots_max / 2) reserve = d->roots_max / 2;
    return d->roots_max - reserve;
}

/* the scratch carved into the decision's struct and its arrays, each at a 16-byte boundary, in SCRATCH_BYTES' order */
static IMPAI_DEC* carve(const IMPAI_BRAIN_IN* in)
{
    unsigned char* p = (unsigned char*)in->scratch;
    IMPAI_DEC* d = (IMPAI_DEC*)p;
    int k;
    memset(d, 0, sizeof *d);
    p += ALIGN16(sizeof *d);
    d->menu = (uint16_t*)p; p += MENU_BYTES;
    d->menu2 = (uint16_t*)p; p += MENU_BYTES;
    d->menu3 = (uint16_t*)p; p += MENU_BYTES;
    d->ring = (PlaceCell*)p; p += RING_BYTES;
    for (k = 0; k < IMPAI_THREATS; k++) { d->maps.threat[k] = (uint32_t*)p; p += ALIGN16(GRID_CELLS * sizeof(uint32_t)); }
    d->maps.influence = (int32_t*)p; p += ALIGN16(GRID_CELLS * sizeof(int32_t));
    d->maps.presence = (uint16_t*)p; p += ALIGN16(GRID_CELLS * sizeof(uint16_t));
    d->maps.diff = (int32_t*)p; p += ALIGN16((IMPAI_GRID_MAX + 1) * (size_t)IMPAI_GRID_MAX * sizeof(int32_t));
    d->way_pass = (uint8_t*)p; p += ALIGN16(IMPAI_WAY_PASS_BYTES);
    d->way_block = (int32_t*)p;
    return d;
}

int impai_brain_decide(const IMPAI_BRAIN_IN* in, IMPAI_BRAIN_OUT* out)
{
    IMPAI_DEC* d;
    const WORLDQ* q = in->q;
    IMPAI_BRAIN* st = in->st;
    int k;
    if (!q || !st || !in->job || !in->ti || !in->plan || !in->persona || !in->snap || !in->scratch || !in->pref || !in->pref_stride ||
        in->scratch_bytes < impai_brain_scratch_bytes() || (int)in->nti != q->ops->ndefs(q) ||
        (int)in->njob < q->ops->nslots(q) || (in->nspots && !in->spots) ||
        in->plan->idle_max > IMPAI_IDLE_CAP || in->plan->roots > IMPAI_ROOTS_MAX)
        return -1;
    memset(out, 0, sizeof *out);
    d = carve(in);
    d->in = in;
    d->out = out;
    d->q = q;
    d->now = (uint32_t)q->ops->gametime(q);
    d->sea_type = out->sea_type = (uint16_t)WQ_NONE;
    q->ops->map(q, &d->m);
    if (q->ops->economy(q, &d->e) != WQ_OK) memset(&d->e, 0, sizeof d->e);
    d->act_build = q->ops->action(q, WQ_ACT_MOBILEBUILD);
    d->act_vtol_build = q->ops->action(q, WQ_ACT_VTOL_MOBILEBUILD);
    d->act_queue = q->ops->action(q, WQ_ACT_BUILDINGBUILD);
    d->act_park = q->ops->action(q, WQ_ACT_PARK);
    d->e_per_m = in->plan->e_per_m;
    d->factory_per_m = in->plan->factory_per_m;
    if (st->explorer != WQ_NONE && st->explorer >= in->njob) st->explorer = (uint16_t)WQ_NONE;
    out->census = (uint16_t)census(d);
    hold_read(d);
    d->roots_max = in->acts < 0 || in->acts > (int)in->plan->roots ? (int)in->plan->roots : in->acts;
    d->root_cap = root_cap_of(d);
    if (d->count[IMPAI_C_FACTORY]) st->opened = 1;
    impai_ways_keep(d);
    for (k = 0; k < IMPAI_CATS; k++) out->count[k] = d->count[k];
    if (!d->lowest.slot) return 0;
    if (st->ax == IMPAI_NO_ANCHOR) {
        const IMPAI_UNITREF* a = d->commander.slot ? &d->commander : &d->lowest;
        st->ax = (int16_t)(a->pos[0] >> 20);
        st->az = (int16_t)(a->pos[2] >> 20);
    }
    if (!d->explorer_live) st->explorer = (uint16_t)WQ_NONE;
    base_box(d);
    impai_fmem_refresh(d);
    impai_fmem_observe(d);
    job_checks(d);
    walk_checks(d);
    yard_clear(d);
    economy(d);
    impai_econ_assign(d);
    impai_econ_factories(d);
    out->sites = (uint16_t)d->sites;
    out->snaps = (uint16_t)d->snaps;
    out->roots = (uint16_t)d->roots;
    out->queued = (uint16_t)d->queued;
    out->sea_type = d->sea_type;
    out->sea_answer = (int8_t)d->sea_answer;
    return 0;
}
