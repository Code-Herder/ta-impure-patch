/* impai_brain.c -- ImpAI's decision (impai.h, the brain): its entry, the census of the seat's units and the
   checks of the jobs it gave them. The ladder and the factories are impai_econ.c's, the sites and the orders
   impai_build.c's. Engine-free and without statics: everything it knows comes through the WORLDQ and its
   arguments, everything it keeps is the caller's (IMPAI_BRAIN, IMPAI_JOB). */
#include <float.h>
#include <string.h>
#include "impai_brain.h"
#include "impai_react.h"
#include "tagpu_simreg.h"

/* The brain's entry, its census and the jobs' checks. OWNER: the brain decides only inside a live ImpAI
   seat's turn, which plays no puppet seat (tagpu_impai.c, impai_play); on a puppet level the level init's
   reset only clears the brain's state, which the recording then restores. */
SIM_FEATURE(impai_brain, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval impai-raid impai-persona");

/* THE PLAN (research/notes/impai/brain.md, The plan and the personas), which every level plays: a level is a
   persona (impai_react.c), never other decisions. Each field's meaning and limit is in impai.h beside it. The
   values are hand-written starting points for the bar's diagnosis to move, not principles: the economy is the row
   A1b's bench passed with, and the military the row the bar's Hard evidence was measured on. The governor's
   numbers (impai_econ.c, THE GOVERNOR) are hand-written starting points too:
   - army_per_cons 2: BARb's openers carry one builder in every two to three units;
   - stall_pm 200, hold_fac 1 (THE HOLD, economy): one threshold for the stalls and the hold, so the hold starts
     where a metal stall does; 200 leaves a buffer of 230-340 metal at the measured storages (1 150-1 700, p10-p90
     of the bar's seats); one factory keeps the army coming in a long hold. */
const IMPAI_PLAN IMPAI_PLAN_ROW = {
    .open_mex = 2, .open_energy = 2, .open_t = 1800,
    .factory_max = 2, .factory_per_m = 8.0f,
    .e_per_m = 8.0f,
    .cons_open = 1, .cons_max = 12, .army_per_cons = 2, .press_k = 1.5f, .press_hold = 900,
    .stall_pm = 200, .hold_fac = 1,
    .comm_r = 1024, .base_r = 1536,
    .gap = 2,
    .job_timeout = 2700, .fail_t = 5400,
    .idle_max = 32, .assign_max = 4, .site_tests = 32, .snaps = 4, .queue_max = 4, .roots = 8, .job_checks = 8,
    /* the military [hand-written: the bar's to move] */
    .mil_roots = 40, .react_confirm = 1, .predict_max = 12, .aa_struct_max = 6,
    .rally_px = 384,
    .attack_margin = 0.10f, .retreat_margin = -0.05f, .wave_value = 8.0f,
    .scout_period = 1800, .task_timeout = 2700,
    .aa_struct_v = 1000.0f, .dgun_reserve = 0.25f,
    .def_max = 4, .def_v = 600.0f,
    .sensor_max = 4,
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
#define HELD_BYTES   (2 * ALIGN16(GRID_CELLS) + ALIGN16(GRID_CELLS * sizeof(uint16_t)))
#define SCRATCH_BYTES (ALIGN16(sizeof(IMPAI_DEC)) + 3 * MENU_BYTES + RING_BYTES + MAPS_BYTES + HELD_BYTES + WAY_BYTES)

_Static_assert(SCRATCH_BYTES <= IMPAI_BRAIN_SCRATCH_MAX, "a decision's scratch fits IMPAI_BRAIN_SCRATCH_MAX");

size_t impai_brain_scratch_bytes(void) { return SCRATCH_BYTES; }

void impai_brain_reset(IMPAI_BRAIN* st, IMPAI_JOB* job, unsigned njob)
{
    unsigned i;
    memset(st, 0, sizeof *st);
    st->ax = st->az = IMPAI_NO_ANCHOR;
    st->explorer = (uint16_t)WQ_NONE;
    st->sea_none_type = st->sea_cur_type = (uint16_t)WQ_NONE;
    st->fscan = st->ascan = (uint16_t)WQ_NONE;
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

/* the slot holds anything of the job table's: a job, a builder's unreachable site (far_until 0: none) or its
   refused help (help_until 0: none) */
static int job_held(const IMPAI_JOB* j) { return j->type != WQ_NONE || j->far_until || j->help_until; }

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
    d->claims[d->nclaims].head = 0;
    d->claims[d->nclaims].feat = 0;
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

/* A reclaim job's feature as the game shows it to the seat (impai_build.c, THE FEATURE SHOWN): 1 there -- the one
   anchored at the job's cell, of the job's definition, still to be taken (impai_feat_takeable), in *f --, 0 gone:
   none, another anchored there since (a successor), or one playing its sequence, which a reclaim paid or a death
   began; -1 unknown. The draw's inputs are the memory's entry for the cell; with none the def is taken as gated,
   one the seat must see. */
static int reclaim_seen(const IMPAI_DEC* d, const IMPAI_JOB* j, WQ_FEAT* f)
{
    const int k = impai_fmem_find(d->in->st, j->cx, j->cz);
    IMPAI_FMEM e;
    if (k >= 0) e = d->in->st->fmem[k];
    else {
        memset(&e, 0, sizeof e);
        e.fx = e.fz = 1;
        e.gated = 1;
        e.owner = 10;
    }
    e.ax = j->cx;
    e.az = j->cz;
    e.def = j->feat;
    return impai_feat_seen(d, &e, f);
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
   what it left. A head that names a unit -- the D-gun's attack, a guard -- stays its maker's until the engine ends
   it. INVARIANT, an ordering by the job field and, for the commander, the window field: at every economy
   decision a finished builder is either the military's (the commander inside the window) or listed by the
   economy (idle, helper or jobbed), a head that names a unit aside; nothing rests on the engine finishing a move,
   so a constructor the governor counts (gov_c) is one the economy can task, that head aside. RESIDUAL:
   the D-gun's chase (impai_mil.c, THE D-GUN). */
static int handed_back(IMPAI_DEC* d, int slot, const IMPAI_JOB* job, int h, const WQ_ORDER* o)
{
    return job->kind == IMPAI_JOB_NONE && h == 1 && o->target == (uint16_t)WQ_NONE && o->action != d->act_build &&
           o->action != d->act_vtol_build && d->q->ops->orders(d->q, slot, d->olist, 2) == 1;
}

/* THE SENSORS' census (impai_econ.c): a building of the seat's, `type` below nti, built, a frame or at a head --
   one sensor structure more when it is one, and its coverage while the list has room when it has a radar or sonar
   distance: a plant or a tower with a radar covers what a radar tower would. The last sensor_max places are kept
   for sensor structures, so the ones the want counts are placed whatever other buildings carry a radar
   (sensor_max at most IMPAI_SENSORS / 2: impai_brain_test.c, check_plan). A rectangle is cells, inclusive, so its
   centre is (x0 + x1 + 1) x 8 px. */
static void sens_add(IMPAI_DEC* d, unsigned type, const IMPAI_RECT* r)
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    const int sensor = impai_sensor_struct(d->in, type);
    IMPAI_SENSE* s;
    if (sensor && d->sens_n < 0xFFFFu) d->sens_n++;
    if ((!t->radar && !t->sonar) || d->nsens >= IMPAI_SENSORS ||
        (!sensor && d->nsens >= IMPAI_SENSORS - d->in->plan->sensor_max))
        return;
    s = &d->sens[d->nsens++];
    s->x = ((int32_t)r->x0 + r->x1 + 1) * 8;
    s->z = ((int32_t)r->z0 + r->z1 + 1) * 8;
    s->radar = t->radar;
    s->sonar = t->sonar;
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
    if ((job->type != WQ_NONE && job->type != type) || (job->far_until && job->far_type != type) ||
        (job->help_until && job->help_type != type)) {
        if (job->type != WQ_NONE && job->type != type && job->kind == IMPAI_JOB_RECLAIM) d->out->harvest_lost++;
        job_clear(job);
    }
    t = &in->ti[type];
    if (t->cat != IMPAI_C_MEX &&
        (!u->finished || t->cat == IMPAI_C_FACTORY || t->workertime || (t->flags & IMPAI_TF_RECLAIM)) &&
        u->made_m > t->mgain)
        d->m_paid += u->made_m - t->mgain;   /* THE STEADY INCOME: a payout reaches a builder, a reclaimer, a frame */
    cat = t->cat < IMPAI_CATS ? t->cat : IMPAI_C_OTHER;
    building = !(t->flags & IMPAI_TF_MOBILE);
    d->count[cat]++;
    if (impai_water_plant(in, (unsigned)type)) d->sea_plants++;
    if (cat == IMPAI_C_FACTORY && impai_econ_air_plant(d, (unsigned)type)) d->air_plants++;
    if (u->finished) d->done[cat]++;
    if (u->finished && cat == IMPAI_C_CONS) d->gov_c++;
    if (u->finished && t->klass < IMPAI_KS) d->gov_a++;
    if (!d->lowest.slot) unit_ref(&d->lowest, u);
    if (cat == IMPAI_C_COMMANDER && !d->commander.slot) unit_ref(&d->commander, u);
    /* THE MAKERS' census (impai_econ.c): every finished onoffable maker with an energy cost counts, and the first
       IMPAI_MAKERS_MAX in slot order are listed to be switched; THE FLOWS' LAG, each maker whose activation bit
       disagrees with its ledger's last-pass energy ask */
    if (cat == IMPAI_C_MAKER && u->finished && (t->flags & IMPAI_TF_ONOFF) && t->egain > 0.0f) {
        if (u->active) d->mk_on_e += t->egain;
        else d->mk_off_n++;
        if (u->active && !(u->req_e > 0.0f)) {
            d->mk_lag_e += t->egain;
            d->mk_lag_m += t->mgain;
        } else if (!u->active && u->req_e > 0.0f) {
            d->mk_lag_e -= t->egain;
            d->mk_lag_m -= t->mgain;
        }
        if (d->nmk < IMPAI_MAKERS_MAX) {
            unit_ref(&d->mk[d->nmk], u);
            d->mk_active[d->nmk++] = (uint8_t)(u->active != 0);
        }
    }
    if (u->slot == in->st->explorer && job->kind == IMPAI_JOB_EXPLORE) d->explorer_live = 1;
    if ((cat == IMPAI_C_COMMANDER || cat == IMPAI_C_CONS) && u->finished && d->nbtype < IMPAI_SEA_BUILDERS) {
        int k;
        for (k = 0; k < d->nbtype && d->btype[k] != type; k++) {}
        if (k == d->nbtype) d->btype[d->nbtype++] = (uint16_t)type;
    }
    if (cat == IMPAI_C_CONS && (unsigned)type < WQ_TYPES_MAX)
        (u->finished ? d->cons_done : d->cons_frame)[type >> 5] |= 1u << (type & 31);
    /* THE COMMANDER'S WINDOW. While safe_until holds (impai_com_safe: after now, and no later than the window's
       length past it) the military has moved the commander out of a fight it would lose (impai_mil.c, THE
       FLIGHT), and while com_until marks it (impai_com_marked) an
       order the military gave it is still pending (THE COMMANDER'S MARKERS); in either the economy neither lists it
       idle, nor as a helper, nor checks its job: so two managers never order it in one window, by these
       fields and not by which runs first. It is still counted, and its head's build is still a claim. Outside
       the window the commander is listed by the ordinary tests and by THE HAND-BACK. */
    if (cat == IMPAI_C_COMMANDER && in->mil &&
        (impai_com_safe(in->mil->safe_until, d->now) || impai_com_marked(in->mil->com_until, d->now, in->persona))) {
        d->com_held = 1;
        h = d->q->ops->head(d->q, u->slot, &o);
        if (h == 1 && (o.action == d->act_build || o.action == d->act_vtol_build) && o.type && o.type < in->nti) {
            const IMPAI_RECT r = rect_of(d, o.type, o.pos);
            impai_claim_add(d, o.type, &r);
            if (impai_water_plant(in, o.type)) d->sea_plants++;
        }
        return;
    }
    h = d->q->ops->head(d->q, u->slot, &o);
    if (u->finished && t->klass < IMPAI_KS) d->kspent[t->klass] += t->value;  /* a frame: its node's */
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
            if (!d->claims_full) d->claims[d->nclaims - 1].feat = 1;
            if (k >= 0) d->m_inflight += in->st->fmem[k].metal;
        }
        /* a build order at the head is a claim on its site; its counts wait for THE HEADS' COUNTS */
        if (h == 1 && (o.action == d->act_build || o.action == d->act_vtol_build) && o.type && o.type < in->nti) {
            const IMPAI_RECT r = rect_of(d, o.type, o.pos);
            impai_claim_add(d, o.type, &r);
            if (!d->claims_full) d->claims[d->nclaims - 1].head = 1;
            if (impai_water_plant(in, o.type)) d->sea_plants++;
            d->pending[in->ti[o.type].cat < IMPAI_CATS ? in->ti[o.type].cat : IMPAI_C_OTHER]++;
        }
        if (u->finished) {
            IMPAI_UNITREF r;
            const int idle = idle_head(d, u->slot, h, &o) || handed_back(d, u->slot, job, h, &o);
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
    if (cat == IMPAI_C_OTHER && (t->flags & IMPAI_TF_MOBILE) && u->finished) {    /* THE ARMY'S LOOK */
        IMPAI_UNITREF r;
        unit_ref(&r, u);
        if (!d->afirst.slot) d->afirst = r;
        if (!d->aobs.slot && (in->st->ascan == WQ_NONE || u->slot > in->st->ascan)) d->aobs = r;
    }
    if (cat == IMPAI_C_FACTORY && u->finished) {
        IMPAI_UNITREF r;
        unit_ref(&r, u);
        if (idle_head(d, u->slot, h, &o)) {
            if (d->nfidle < IMPAI_IDLE_CAP) d->fidle[d->nfidle++] = r;
        } else {
            int k, made, n = d->q->ops->orders(d->q, u->slot, d->olist, 8);
            if (d->nfbusy < IMPAI_IDLE_CAP) {
                d->fbusy_q[d->nfbusy] = (uint8_t)(h == 1 && o.action == d->act_queue);
                d->fbusy[d->nfbusy++] = r;
            }
            /* THE PLANT'S PRICE: a factory at its BuildingBuild that asked metal in the last pass */
            if (h == 1 && o.action == d->act_queue && u->req_m > 0.0f) {
                d->price_m += u->req_m;
                d->price_e += u->req_e;
                d->price_n++;
            }
            /* THE QUEUED METAL: the metal cost of every unit the factory's count nodes hold past the one it makes
               -- the first unit of its first node --, which the store pays as the queue runs; THE FIVE, bounded
               (impai_econ.c) takes it off the store above the hold's line before a surplus order is sized, so the
               bound holds across decisions and not only within one. A cost not finite or below 0 counts none. */
            for (k = 0, made = 0; k < n; k++) {
                const WQ_ORDER* q = &d->olist[k];
                WQ_DEF_ECON e;
                int32_t c;
                if (q->action != d->act_queue || q->type >= in->nti || q->count <= 0) continue;
                c = q->count > IMPAI_QCOUNT_HELD ? IMPAI_QCOUNT_HELD : q->count;
                if (in->ti[q->type].cat == IMPAI_C_CONS) d->gov_c += c;
                else if (in->ti[q->type].klass < IMPAI_KS) {
                    d->gov_a += c;
                    d->kspent[in->ti[q->type].klass] += in->ti[q->type].value * (float)c;
                }
                if (d->q->ops->def_econ(d->q, q->type, &e) && e.cost_m >= 0.0f && e.cost_m <= FLT_MAX)
                    d->surplus_m += e.cost_m * (float)(c - !made);
                made = 1;
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
        sens_add(d, (unsigned)type, &r);
        if (!u->finished) {
            if (d->nnano < IMPAI_NANO_MAX) unit_ref(&d->nano[d->nnano++], u);
            if (cat == IMPAI_C_ENERGY) d->pe_extra += t->egain;
        }
    } else if (!u->finished && d->nframes < IMPAI_NANO_MAX)
        unit_ref(&d->frames[d->nframes++], u);
}

/* Over the seat's slot block in slot order, bounded by it and by IMPAI_CENSUS_MAX slots, which the engine's
   per-player ceiling never passes (impai.h). A slot whose unit is gone or of another type loses its job, and one
   whose job was an ordered reclaim (IMPAI_JOB_RECLAIM) counts in IMPAI_BRAIN_OUT.harvest_lost: the harvest's cost
   in builders, among the reclaims ordered; a walk to one ordered none. Returns the units counted. */
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
            if (in->job[slot].kind == IMPAI_JOB_RECLAIM) d->out->harvest_lost++;
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

/* THE HEADS' COUNTS, after the census: each builder's build head adds its anti-air structure (aa_n), its ground
   defence (def_n) and its energy (pe_extra) unless its site already holds a building of its type, a frame or
   finished, which the census counted. A head stays at its builder's list while the builder works on the frame it
   placed, so a head and its frame were the same building counted twice: a ground defence's second count could
   hold def_n at def_max with one tower standing, and an energy frame's doubled egain held the energy rule back.
   After the census and not in it, because a frame's slot can come after its builder's. A head's type was bounded
   by the census (below nti) before its claim was kept; claims_full, unreachable by IMPAI_CLAIM_MAX's bound, only
   leaves a head uncounted. BOUND: the census's claims, each against the census's buildings. */
static void head_counts(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i;
    for (i = 0; i < d->nclaims; i++) {
        const IMPAI_CLAIM* c = &d->claims[i];
        if (!c->head || c->type >= in->nti ||
            built_at(d, c->type, (c->r.x0 + c->r.x1) / 2, (c->r.z0 + c->r.z1) / 2, 0))
            continue;
        if (impai_aa_struct(in, c->type)) d->aa_n++;
        if (impai_def_struct(in, c->type)) d->def_n++;
        sens_add(d, c->type, &c->r);
        if (in->ti[c->type].cat == IMPAI_C_ENERGY) d->pe_extra += in->ti[c->type].egain;
    }
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
    const int64_t lim = impai_reach_limit(d, b->type, sfx, sfz);
    return lim >= 0 && (int64_t)impai_dist_px(b->pos, pcx, pcz) > lim;
}

int64_t impai_reach_limit(const IMPAI_DEC* d, int btype, int sfx, int sfz)
{
    const IMPAI_TINFO* bt;
    WQ_DEF def;
    int bfx, bfz;
    if (btype <= 0 || btype >= (int)d->in->nti || !d->q->ops->def(d->q, btype, &def)) return -1;
    bt = &d->in->ti[btype];
    bfx = bt->fx > 0 ? bt->fx : 1; bfz = bt->fz > 0 ? bt->fz : 1;
    if (sfx < 1) sfx = 1;
    if (sfz < 1) sfz = 1;
    return (int64_t)def.reach + impai_isqrt(64u * (uint64_t)(bfx * bfx + bfz * bfz)) +
           impai_isqrt(64u * (uint64_t)(sfx * sfx + sfz * sfz));
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
   refusal of the sea plant, its search's termination (impai_build.c, THE SEA PLANT). BOUND, on recorded data: the
   record is read live only while far_until is at most fail_t after now (record_live). */
static void build_unreached(IMPAI_DEC* d, IMPAI_JOB* j)
{
    j->far_cx = j->cx;
    j->far_cz = j->cz;
    j->far_type = j->type;
    j->far_until = d->now + d->in->plan->fail_t;
    if (impai_water_plant(d->in, j->build)) impai_sea_refusal(d);
}

/* 1 while a job record's `until` holds: after now, and no later than fail_t past it -- the most a record is ever
   given (build_unreached, help_checks). BOUND, on recorded data: a value restored from a recording past that holds
   nothing, so no record bars a builder for longer than fail_t whatever the stream carries, as THE COMMANDER'S
   WINDOW's safe_until (impai_com_safe). */
static int record_live(const IMPAI_DEC* d, uint32_t until)
{
    return until > d->now && until - d->now <= d->in->plan->fail_t;
}

int impai_far(const IMPAI_DEC* d, const IMPAI_UNITREF* b) { return record_live(d, d->in->job[b->slot].far_until); }

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
   still there or unknown -- records the builder's far record at the feature (THE FAR RECORD), as a site
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

/* THE RECLAIM'S CHECK, every decision, by what the game shows the seat only (reclaim_seen, impai_build.c's THE
   FEATURE MEMORY). Each way it ends has its reason:
   - SEEN GONE -- reclaimed, by this builder or by anything else, burnt, crushed, or playing the sequence a paid
     reclaim or a death began -- a fact for the whole seat: it leaves the memory, so no builder is sent to it
     again, the job ends with no failure, and a builder still on its way is listed idle so that this decision
     gives it its next task. The seat's own payouts are counted where the engine makes them (tagpu_impai.c,
     THE PAYOUT), not here: the feature alone cannot say who took it.
   - ARRIVED: a RECLAIM_GO whose feature the draw shows there is ordered to reclaim it
     (impai_reclaim_arrived), the decision's commands permitting; a refusal there is the builder's to avoid.
   - UNSAFE: the commander's feature, shown there, is a point THE COMMANDER'S SAFE DISTANCE refuses
     (impai_build.c): the job ends with no failure and no far record -- an enemy stopped it, not the way -- and
     the commander is listed idle, so that this decision gives it its next task.
   - REPLACED (reclaim_replaced): the builder is busy with an order that is not the job's; the job ends with no
     failure and the claim on the feature with it.
   - GIVEN UP: a builder idle, or past job_timeout and stopped, with its feature unknown or still there:
     it alone avoids it (reclaim_unreached). */
static void reclaim_check(IMPAI_DEC* d, const IMPAI_UNITREF* b, int idle, IMPAI_JOB* j, uint32_t age)
{
    IMPAI_BRAIN* st = d->in->st;
    WQ_FEAT f;
    const int seen = reclaim_seen(d, j, &f), ordered = j->kind == IMPAI_JOB_RECLAIM;
    if (seen == 0) {
        const int k = impai_fmem_find(st, j->cx, j->cz);
        if (k >= 0) impai_fmem_drop(d, k);
        if (ordered) d->out->rc_gone++;
        impai_job_end(j);
        if (!idle && d->nidle < d->in->plan->idle_max && d->nidle < IMPAI_IDLE_CAP) d->idle[d->nidle++] = *b;
        return;
    }
    if (seen > 0 && j->kind == IMPAI_JOB_RECLAIM_GO) {
        const int r = impai_reclaim_arrived(d, b, &f);
        if (r >= 0) return;
        if (r == IMPAI_RECLAIM_UNSAFE) {
            d->out->rc_unsafe++;
            impai_job_end(j);
            if (!idle && d->nidle < d->in->plan->idle_max && d->nidle < IMPAI_IDLE_CAP) d->idle[d->nidle++] = *b;
            return;
        }
        reclaim_unreached(d, j);
        impai_job_end(j);
        return;
    }
    if (!idle && reclaim_replaced(d, b, j)) {
        if (ordered) d->out->rc_replaced++;
        impai_job_end(j);
        return;
    }
    if (idle || (age > d->in->plan->job_timeout && job_stop(d, b))) {
        if (ordered) d->out->rc_given_up++;
        reclaim_unreached(d, j);
        impai_job_end(j);
    }
}

/* At most job_checks, in slot order. An idle builder's BUILD is done when a building of the type ordered
   covers the site; else the builder could not reach the site (THE REACH ABANDON), which holds it back (THE FAR
   RECORD), or the site failed. A busy one older than job_timeout with no frame of it at the site is stopped
   (job_stop) and the site failed. An EXPLORE ends when its cell is explored, or after job_timeout with the cell
   failed. A reclaim (reclaim_check) ends the moment the seat is shown its feature gone. */
/* THE HELP REFUSED, an ORDERING: a builder whose help ended while its frame stood unfinished is given no help on
   a frame at that frame's cell for fail_t (IMPAI_JOB.help_cx, help_cz, help_until).
   - THE ENGINE. HelpBuild 0x403F70, its state table at 0x40425C: state 0 (0x403FD9) walks through 0x438A00
     (0x40404E) to within the builder's builddistance (def +0x212) plus half of 16.0 (0x4FC928) x
     sqrt(fx*fx + fz + fz) of the target's def footprint (+0x14A, +0x14C; 0x403FF9..0x404045, the engine's own
     formula); state 1 (0x404069) answers flag 0x40 of its third argument with "I can't get there" (0x5015BC) and
     result 8, abandoned; with no target it shows "Construction terminated" (0x5015A4) and answers 8
     (0x403F9D..0x403FC1); its one normal end is the frame's completion, result 5 (state 4, 0x404223..0x404248;
     THE ASSIST, impai_build.c). DISASSEMBLED 2026-11-14.
   - WHY: THE ASSIST took the same frame again at the next decision, and the engine ended it again. MEASURED
     2026-11-14 on Trout Farm (bar-hota-troutfarm-ac-1), ticks 30 000-40 000: an ARMLAB made all 15 of its products
     at one point, cell (168, 414), and three ARMCVs north and west of the plant were given the help on each of
     them and stood Standby two ticks later, at every decision -- one ARMCV changed its head 246 times in 3 600
     ticks, while the one south of the plant helped. At Easy and Normal each such order is an action of the
     persona's budget, the yard's drain (THE YARD).
   - THE TEST, after the census, before the job checks: a builder of the jobbed list listed idle whose job is an
     ASSIST given more than the persona's reaction ago -- its order was given before this decision
     (impai_react.c, react <= look - 15), so the head is the engine's answer -- and whose target is still an
     unfinished unit of the census (nano or frames): the help ended short of its frame. Its record takes the
     frame's cell and its job ends; a help whose frame is finished or gone ended as it should, and only its job
     ends. The cell, not the slot: a plant's every product is a new slot made at its one point, so a slot would
     cost one refused order per product per builder.
   - NO FALSE REFUSAL from another manager: every military order to the commander ends its economy job first
     (impai_mil.c, release_commander), and every economy order that takes a helper elsewhere replaces its job.
   - BOUND, on recorded data: help_cx and help_cz are compared, never used as an index; help_until is read live
     only while it is at most fail_t after now (record_live), so a record restored from a recording holds one cell
     for fail_t at most. The walk
     is the jobbed list's (2 x IMPAI_IDLE_CAP), each against the census's lists (IMPAI_NANO_MAX each), and it
     gives no order.
   - TERMINATION: the record ends at help_until, and the builder may be sent to that cell's frames again; one
     still refused is refused once a fail_t, not once a decision.
   - RESIDUAL: the help's cause -- here the plant's body between the builder and its build point -- is not
     removed, and a builder that has since moved to where the help would succeed still passes that cell over
     until help_until. */
/* the 16-px footprint cell of a 16.16 world coordinate */
static int16_t foot_cell(int32_t fix) { return (int16_t)(fix >> 20); }

static void help_checks(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i, k;
    for (i = 0; i < d->njobbed; i++) {
        IMPAI_JOB* j = &in->job[d->jobbed[i].slot];
        const IMPAI_UNITREF* f = NULL;
        if (!d->jobbed_idle[i] || j->kind != IMPAI_JOB_ASSIST || d->now - j->since <= in->persona->react) continue;
        for (k = 0; k < d->nnano && !f; k++) if (d->nano[k].slot == j->target) f = &d->nano[k];
        for (k = 0; k < d->nframes && !f; k++) if (d->frames[k].slot == j->target) f = &d->frames[k];
        impai_job_end(j);
        if (f) {
            j->help_cx = foot_cell(f->pos[0]);
            j->help_cz = foot_cell(f->pos[2]);
            j->help_type = d->jobbed[i].type;
            j->help_until = d->now + in->plan->fail_t;
            d->out->help_refused++;
        }
    }
}

int impai_help_refused(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const int32_t pos[3])
{
    const IMPAI_JOB* j = &d->in->job[b->slot];
    return record_live(d, j->help_until) && j->help_cx == foot_cell(pos[0]) && j->help_cz == foot_cell(pos[2]);
}

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
            /* an explorer that stands idle with its cell unseen has arrived
               without seeing it, or lost its order: the cell fails as at the timeout, so it is not asked again
               for fail_t */
            if (d->q->ops->explored(d->q, j->cx, j->cz)) impai_job_end(j);
            else if (d->jobbed_idle[i] || age > in->plan->job_timeout) {
                impai_fail_add(d, j->cx, j->cz);
                impai_job_end(j);
            }
        }
    }
}

/* THE WALK CHECK, an ORDERING: a constructor's job the seat learns is threatened ends within one decision. For
   every busy constructor of the census's jobbed list -- never the commander, whose safety is the military's
   (impai_mil.c), so two managers never order it in one decision --, at its job's point: a BUILD's footprint
   centre, an EXPLORE's cell centre, an ASSIST's target where the census found it, a RECLAIM_GO's or a RECLAIM's
   feature anchor cell centre. A BUILD with no building of its type at the site yet, an EXPLORE, a RECLAIM_GO and a
   RECLAIM whose builder stands farther from its feature than its build reach (TASIM's sweep builder, the reach the
   reclaim's own score walks to) are walks, turned back when the point or the straight way there holds known ground
   threat (impai_walk_threatened; the builder's own cell is left out, so one walking out of threat goes on); a
   BUILD whose frame stands, an ASSIST and a RECLAIM within reach are work, turned back when the point's cell or the
   builder's own holds it -- the threat has come within reach of where it works. impai_turn_back ends the job; an
   EXPLORE's
   cell also fails for fail_t. The builder, holding the move and no job, is listed idle from the next decision on
   (THE HAND-BACK), and the ladder gives it a site THE ROUTE has tested (impai_build.c). A ground defence's builder
   is no exception: one sent to a site at the edge of the threat it answers is turned back by this work test once
   the threat's reach covers its own cell or the frame's; only the commander is exempt. Outside job_checks, which
   starts at the slot order's first builder each decision and would starve the later ones if every walk spent a
   check. BOUND: at most 2 x IMPAI_IDLE_CAP walks tested a decision, one route each; the orders within the roots
   the factories leave (THE FACTORIES' ROOTS), and on a contested decision within roots_max, since a move of no job
   is never held by the factories' turn (THE FACTORIES' TURN). The turn back is given within look_eco + react ticks of the seat's
   rows showing the threat -- one decision, then the persona's reaction: 15 at Hard, 105 at Easy -- when that
   decision has a command left. Under the economy's bucket (impai_react.c, THE BUDGET) one with none gives no turn
   back, and the builder waits for a decision with an action: the bucket refills one every 1800 / apm_eco ticks
   (180 at Easy, 75 at Normal), and the job checks' stops and the earlier turn backs take theirs first. MEASURED
   2026-11-13, Comet Catcher, every one of the 23 constructors a human's line killed on an expansion walk had
   known ground threat on its remaining way 34 to 2 619 ticks before it died, so Hard turns every one of them back
   and a lower level misses the warnings shorter than its look and reaction, the weakness the level is. RESIDUAL: a builder learns a stretch of the threat from inside its reach when the enemy
   outranges its sight (a CORCV sees 200 px, a Samson reaches 600), and turns back under fire; a reclaimer whose
   TASIM sweep inputs the world does not give is tested as work wherever it stands; an ASSIST's target that is not an unfinished
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
    const int cap = d->contested ? d->roots_max : d->root_cap;
    int i;
    for (i = 0; i < d->njobbed && d->roots < cap; i++) {
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
        } else if (reclaim_job(j)) {
            SweepBuilder sb;
            x = j->cx * 16 + 8;
            z = j->cz * 16 + 8;
            back = tasim_sweep_builder(d->q, b->slot, &sb) && (double)impai_dist_px(b->pos, x, z) > sb.reach
                       ? impai_walk_threatened(d, b, x, z)
                       : impai_threatened(d, x, z) || impai_threatened(d, b->pos[0] >> 16, b->pos[2] >> 16);
            if (back && impai_turn_back(d, b) && j->kind == IMPAI_JOB_RECLAIM) d->out->rc_turned++;
        }
    }
}

/* THE YARD, an ORDERING: no builder the economy lists idle or as a helper stands on a waiting factory's yard or its
   way out past one economy decision, and none is ordered there twice.
   - THE ENGINE. A factory's next product is created only when the engine's placement test passes its yard:
     BuildingBuild 0x402640 calls 0x47DB70 at 0x402899 with the unit to ignore pushed as 0 at 0x402896, and on a
     refusal sleeps 15 ticks (0x4028A2..0x4028A6) and asks again, for ever; for a mobile def the test refuses any
     footprint cell whose slot word names a unit other than the ignored one (0x47DCFD..0x47DD0A, the refusal at
     0x47DD99) -- DISASSEMBLED 2026-11-14, engine/map.md's "A lab can open its yard". So a builder standing on the
     yard holds the factory as long as it stands, and an idle builder with every rule closed stands for ever: on
     Trout Farm the seat's one factory waited 27 450 ticks on a constructor guarding it, and 61 350 in another game
     (MEASURED 2026-11-14, the closing run's four Trout Farm games).
   - THE WAY. A unit standing just beyond the yard's exit holds the factory too: the finished product cannot leave,
     stands on the yard, and the next one is refused. A factory's finished unit with no rally gets a Park whose
     goal lies just off the yard (0x4061A0, engine/orders.md), so a constructor the economy has not yet tasked
     waits on its factory's way out. MEASURED 2026-11-14 on Trout Farm (bar-nota-troutfarm-ca-2): two idle
     constructors standing under a plant's exit, Standby at their heads, kept its finished product on the yard and
     the plant waiting from GameTime 31 621 to at least 70 000, the seat defeated at 78 062.
   - THE REGION of a waiting factory is its footprint and the rectangles of its kept way out (IMPAI_YARDREG; the
     way: impai_build.c, THE WAY OUT, the plant's proven path for its largest product, kept while it stands). A
     WAITING FACTORY is a finished one of the census's busy list whose head is a BuildingBuild (fbusy_q) and whose
     footprint holds none of the census's unfinished mobile units. Each idle builder and helper that touches its
     region (impai_yard_holds) is moved off (impai_yard_leave) and leaves its list, so no later rule of this
     decision orders it again.
   - THE WALK OFF: a listed builder on the region whose one order is a point order -- naming no unit, no build, not
     the unit's idle behaviour (idle_head) -- to a point the move above could have chosen (impai_way_off: off the
     region, on the builder's own reach label, clear of the seat's buildings and of what holds still -- the
     decision's points and the idle builders it holds no point for; a helper, a jobbed builder or the commander
     passing by does not end a walk --, not through the plant), given by the economy within WALK_OFF_T (walk_live: its
     job NONE, its type the builder's, its order time at most WALK_OFF_T ago; and its cell the point's within a
     cell), is walking off already, and leaves its list with no order (`yard_left`). Any other is moved again: a
     point order through the plant, one to a point another builder now stands still on, or a walk the engine has
     not ended in WALK_OFF_T -- a move to a point a unit cannot stand on never ends (impai_build.c, THE WAY OFF),
     and trusted for ever it held its factory for good. So a builder that cannot leave is moved once a
     WALK_OFF_T, never to the point it last failed by (THE LAST POINT NOT AGAIN).
   - THE WALKS FIRST: before the decision moves any builder off a region, the point of every idle builder's live
     economy walk (walk_live, its one order a point order) joins the decision's (impai_way_to), so no builder
     is sent beside a walk whose turn in the lists comes later, or that has left the region and has no turn. A
     builder moved again replaces its own point (one point a builder), so the decision's points stay within the
     idle and helper lists' 2 x IMPAI_IDLE_CAP.
     WHY: the census lists such a builder idle (THE HAND-BACK), so the rule gave it the move again at every
     decision until it was off, each time one action of
     the economy's budget: MEASURED 2026-11-14, a constructor that could not leave a yard took 368 of the 407
     orders of an Easy seat's game (bar-eota-pincushion-ac-1) and 539 of 766 of a Normal one's
     (bar-nota-troutfarm-ca-2), the factories and the other builders starved of actions from GameTime 15 000 and
     31 600 to the end; in the Hard games the same builder was moved at 5 to 8 decisions in a row while it walked.
     So a builder is ordered off a region once a walk; one that cannot leave keeps its walk, and its factory waits
     on what holds it in. A WALK THROUGH THE PLANT IS NONE: the move's point is chosen so that the builder never
     has to cross the plant (impai_build.c, THE WAY OFF), and an order that would is not trusted as a walk off.
   - The commander inside THE COMMANDER'S WINDOW is in neither list and is left to the military; an idle builder
     whose BUILD or reclaim the job checks have not ended is left to them, as the ladder leaves it; a helper whose
     ASSIST the walk check ended this decision has its turn back, its one order of the decision; a busy builder
     only crosses a yard or a way, since no site lies on one (the claims). A builder that walks back to help the
     factory's next product is the help's, which ends with the product (THE ASSIST, impai_build.c), and no help
     targets a waiting factory: it has no frame. The army keeps off yards and ways where it is sent (impai_mil.c,
     travel_point's yard rule).
   BOUND: the walks first once a decision, at most IMPAI_IDLE_CAP head and order reads; at most IMPAI_IDLE_CAP
   factories, each against the census's IMPAI_NANO_MAX frames and its at most 2 x IMPAI_IDLE_CAP listed builders,
   two head reads and at most one move a builder (THE WAY OFF's search, impai_build.c), within the roots the factories
   leave (THE FACTORIES' ROOTS) less the root THE RELEASE keeps (impai_release_keep, impai_econ.c); the region is recorded data, a factory's kept way held to IMPAI_WAYS x
   IMPAI_WAY_RECTS. RESIDUAL: a frame past the census's IMPAI_NANO_MAX is not seen, and the builders by its factory
   are moved off a yard that is producing, one move each; a builder held in by what this rule does not move -- an
   enemy, a wreck, a unit of a plant with no kept way -- is moved once a WALK_OFF_T, and its factory waits. */
#define WALK_OFF_T 900          /* GameTime ticks a walk off is trusted for: 30 s, a 900-px walk at 1 px a tick */

/* 1 while b's job is the economy's walk: job NONE, its type b's, its order time at most WALK_OFF_T ago */
static int walk_live(const IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_JOB* j = &d->in->job[b->slot];
    return j->kind == IMPAI_JOB_NONE && j->type == b->type && j->since && d->now - j->since <= WALK_OFF_T;
}

/* the point of b's one order when it is a point order -- naming no unit, no build, not the unit's idle behaviour
   (idle_head) --, px, into (*x, *z): 1, else 0 */
static int walk_point(IMPAI_DEC* d, const IMPAI_UNITREF* b, int* x, int* z)
{
    WQ_ORDER o;
    const int h = d->q->ops->head(d->q, b->slot, &o);
    if (!(h == 1 && !idle_head(d, b->slot, h, &o) && o.target == (uint16_t)WQ_NONE && !o.build &&
          o.action != d->act_build && o.action != d->act_vtol_build &&
          d->q->ops->orders(d->q, b->slot, d->olist, 2) == 1))
        return 0;
    *x = o.pos[0] >> 16;
    *z = o.pos[2] >> 16;
    return 1;
}

static int walking_off(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_YARDREG* g)
{
    const IMPAI_JOB* j = &d->in->job[b->slot];
    int x, z, dcx, dcz;
    if (!walk_live(d, b) || !walk_point(d, b, &x, &z)) return 0;
    dcx = (x >> 4) - j->cx;
    dcz = (z >> 4) - j->cz;
    return dcx >= -1 && dcx <= 1 && dcz >= -1 && dcz <= 1 && impai_way_off(d, b, g, x, z);
}

/* THE WALKS FIRST (THE YARD): the decision's points open with every idle builder's live economy walk to a point */
static void walks_first(IMPAI_DEC* d)
{
    int k, x, z;
    for (k = 0; k < d->nidle; k++)
        if (walk_live(d, &d->idle[k]) && walk_point(d, &d->idle[k], &x, &z)) impai_way_to(d, &d->idle[k], x, z);
}

static int yard_leave_list(IMPAI_DEC* d, IMPAI_UNITREF* list, int* n, const IMPAI_YARDREG* g, int idle, int keep)
{
    int i = 0, k, moved = 0;
    while (i < *n && d->roots + keep < d->root_cap) {
        const IMPAI_JOB* j = &d->in->job[list[i].slot];
        int walk = 0;
        if ((idle && (j->kind == IMPAI_JOB_BUILD || reclaim_job(j))) || (!idle && j->kind != IMPAI_JOB_ASSIST) ||
            !impai_yard_holds(d, g, list[i].type, list[i].pos[0] >> 16, list[i].pos[2] >> 16)) {
            i++;
            continue;
        }
        if (walking_off(d, &list[i], g)) walk = 1;
        else if (!impai_yard_leave(d, &list[i], g)) {
            i++;
            continue;
        }
        for (k = i + 1; k < *n; k++) list[k - 1] = list[k];
        (*n)--;
        if (walk) d->out->yard_left++;
        else moved++;
    }
    return moved;
}

/* factory s's region: its footprint and its kept way's rectangles, the first kept way of its type at its first
   cell (impai_ways_keep's match) */
static void yard_region(const IMPAI_DEC* d, const IMPAI_STRUCT* s, IMPAI_YARDREG* g)
{
    const IMPAI_BRAIN* st = d->in->st;
    int k, i;
    g->r[0] = s->r;
    g->n = 1;
    g->type = s->type;
    for (k = 0; k < st->nway && k < IMPAI_WAYS; k++) {
        const IMPAI_WAYREC* w = &st->way[k];
        if (w->type != s->type || w->cx != s->r.x0 || w->cz != s->r.z0) continue;
        for (i = 0; i < w->n && i < IMPAI_WAY_RECTS; i++) {
            IMPAI_RECT* r = &g->r[g->n++];
            r->x0 = w->r[i][0]; r->z0 = w->r[i][1];
            r->x1 = w->r[i][2]; r->z1 = w->r[i][3];
        }
        return;
    }
}

static void yard_clear(IMPAI_DEC* d)
{
    const int keep = impai_release_keep(d);
    int f, k, first = 1;
    for (f = 0; f < d->nfbusy && d->roots + keep < d->root_cap; f++) {
        const IMPAI_STRUCT* s = NULL;
        IMPAI_YARDREG g;
        if (!d->fbusy_q[f]) continue;
        for (k = 0; k < d->nstructs && !s; k++)
            if (d->structs[k].slot == d->fbusy[f].slot) s = &d->structs[k];
        if (!s) continue;
        for (k = 0; k < d->nframes; k++)
            if (inside(&s->r, (d->frames[k].pos[0] >> 16) >> 4, (d->frames[k].pos[2] >> 16) >> 4)) break;
        if (k < d->nframes) continue;
        if (first) {
            walks_first(d);
            first = 0;
        }
        yard_region(d, s, &g);
        d->out->yard += (uint16_t)yard_leave_list(d, d->idle, &d->nidle, &g, 1, keep);
        d->out->yard += (uint16_t)yard_leave_list(d, d->helpers, &d->nhelpers, &g, 0, keep);
    }
}

/* ---- 3. the economy ---- */

/* The economy's wants. THE SECOND FACTORY WAITS FOR THE NAVAL JUDGEMENT (IMPAI_MIL.naval >= 0), an ORDERING: the
   military judges it (impai_mil.c, naval) and the seat's next factory after its first is a sea plant when it
   says so, so the order the two managers run in can never decide which kind of plant takes the slot -- a land
   plant cannot take the slot the sea plant was judged for because the economy ran first. A military that
   refuses every decision holds the seat at one factory; its level-end line's `refused=` shows it. THE SEA PLANT
   TAKES THE LAND PLANT'S METAL (plant_metal): factory_per_m's pacing, or the surplus that pays a further plant's
   price (plant_m), so whatever lets a land plant come past the first lets the sea plant come first, and a land
   plant never takes the slot a naval judgement asked a yard for -- but THE REACHING PLANT (plant_reach, below),
   which a seat none of whose constructors reaches the open metal is given whatever the judgement and the
   income: a seat cut off from the metal cannot pay for the yard either. It counts against factory_max as any
   plant, so the yard after it comes on the surplus (plant_m). The pacing holds: a plant started on the
   commander's own income alone spends the storage the economy runs on and holds its builder on a frame it cannot
   finish (MEASURED 2026-11-13, Trout Farm, Arm against Arm: a shipyard begun at a metal income of 1 a pass
   emptied the seat's metal by GameTime 4 207, and the seat completed no unit from 3 607 to 12 607); the surplus
   pays the plant out of what the seat does not spend (MEASURED 2026-11-15, round 3 at 57d39d71, b5's Red Triangle
   ac-1 and Trout Farm ca-1: the second plant came on the surplus at a steady income of 6.59 and 6.53 a pass
   against factory_per_m's 8.00, judged naval, while the sea plant asked the pacing alone -- a land plant, no sea
   search, no yard). It is wanted only while some builder offers one (impai_econ_sea_able): without that a seat
   none of whose builders can build one would hold its second factory for ever. */
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

/* THE SENSORS' WANT (impai_econ.c, THE SENSORS): the first point of this order that no sensor of its kind covers --
   none within three quarters of its distance --, into want_sensor and (sensor_x, sensor_z): the anchor; the
   seat's finished extractors, nearest the anchor first; the front, the anchor moved half way to the military's
   estimate of the enemy base and at most base_r; for sonar, the anchor, on a map where some sea army class has
   water (IMPAI_REACH.sea_blocks). The estimate is recorded data, an int16 block, so every product below stays
   within int64. BOUND: the census's structures, each against at most IMPAI_SENSORS sensors. */
static int sensed(const IMPAI_DEC* d, int32_t x, int32_t z, int kind)
{
    int k;
    for (k = 0; k < d->nsens; k++) {
        const uint32_t r = kind == IMPAI_SENSE_SONAR ? d->sens[k].sonar : d->sens[k].radar;
        const int64_t dx = (int64_t)x - d->sens[k].x, dz = (int64_t)z - d->sens[k].z, c = (int64_t)r * 3 / 4;
        if (r && dx * dx + dz * dz <= c * c) return 1;
    }
    return 0;
}

static void sensor_set(IMPAI_DEC* d, int kind, int32_t x, int32_t z)
{
    d->want_sensor = kind;
    d->sensor_x = x;
    d->sensor_z = z;
}

static void sensor_want(IMPAI_DEC* d, const IMPAI_MIL* m)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int32_t ax = in->st->ax * 16 + 8, az = in->st->az * 16 + 8;
    int64_t best = -1;
    int32_t bx = 0, bz = 0;
    int i;
    if (in->st->ax == IMPAI_NO_ANCHOR) return;
    if (!sensed(d, ax, az, IMPAI_SENSE_RADAR)) {
        sensor_set(d, IMPAI_SENSE_RADAR, ax, az);
        return;
    }
    for (i = 0; i < d->nstructs; i++) {
        const IMPAI_STRUCT* s = &d->structs[i];
        const int32_t x = ((int32_t)s->r.x0 + s->r.x1 + 1) * 8, z = ((int32_t)s->r.z0 + s->r.z1 + 1) * 8;
        const int64_t dd = ((int64_t)x - ax) * ((int64_t)x - ax) + ((int64_t)z - az) * ((int64_t)z - az);
        if (s->cat != IMPAI_C_MEX || !s->finished || sensed(d, x, z, IMPAI_SENSE_RADAR)) continue;
        if (best < 0 || dd < best) {
            best = dd;
            bx = x;
            bz = z;
        }
    }
    if (best >= 0) {
        sensor_set(d, IMPAI_SENSE_RADAR, bx, bz);
        return;
    }
    if (m && m->ebx != IMPAI_NO_ANCHOR) {
        const int64_t dx = (int64_t)(m->ebx * 32 + 16) - ax, dz = (int64_t)(m->ebz * 32 + 16) - az;
        const uint32_t de = impai_isqrt((uint64_t)(dx * dx + dz * dz));
        const uint32_t f = de / 2 < in->plan->base_r ? de / 2 : in->plan->base_r;
        if (de > 0) {
            const int32_t fx = ax + (int32_t)(dx * f / de), fz = az + (int32_t)(dz * f / de);
            if (!sensed(d, fx, fz, IMPAI_SENSE_RADAR)) {
                sensor_set(d, IMPAI_SENSE_RADAR, fx, fz);
                return;
            }
        }
    }
    if (in->reach && in->reach->sea_blocks && !sensed(d, ax, az, IMPAI_SENSE_SONAR))
        sensor_set(d, IMPAI_SENSE_SONAR, ax, az);
}

/* metal as whole units held to 0..10^9: a store or storage read from the record is data; NaN, a negative value and
   anything below 1 read 0 */
static int64_t metal_whole(float v) { return v >= 1.0f ? (v < 1e9f ? (int64_t)v : 1000000000) : 0; }

/* THE POSTURE, filled once a decision and before anything else reads the decision (impai_brain_decide): what the
   military's record says of the seat's map and its enemy, each flag of IMPAI_POSTURE (impai_brain.h) read from the
   record here and nowhere else in the economy. With no military record every flag stays 0 (carve clears the
   decision). INVARIANT, a bound on recorded data: every flag is a compare against the record, never an index.
   - THE PRESSURE (impai_mil.c): a press_until past now + press_hold holds nothing, so a byte restored from a
     recording can hold the constructors for at most press_hold;
   - THE BLIND SPOT is read through impai_blind, which bounds its stamp the same way;
   - dry, island and press_base are compared with 0 only; naval is copied, and its readers compare it with 1 and
     with 0. */
static void posture_read(IMPAI_DEC* d)
{
    const IMPAI_MIL* mil = d->in->mil;
    if (!mil) return;
    d->pos.naval = mil->naval;
    d->pos.dry = mil->dry != 0;
    d->pos.island = mil->island != 0;
    d->pos.blind = (uint8_t)impai_blind(mil, d->now);
    d->pos.pressed = mil->press_until > d->now && mil->press_until - d->now <= d->in->plan->press_hold;
    d->pos.at_base = d->pos.pressed && mil->press_base != 0;
}

/* the surplus and THE HOLD, read from the seat's record and the posture alone, before the census's roots are cut
   (root_cap_of) */
static void hold_read(IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const WQ_ECON* e = &d->e;
    d->surplus = e->metal_max > 0.0f && e->metal >= SURPLUS_FRAC * e->metal_max;
    /* THE HOLD. While the stored metal is below stall_pm thousandths of the storage, outside a surplus and outside
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
       stall would come straight back; stall_pm of the storage, where m_stall's store term begins, is the buffer
       the released factories start on.
       THE PRESSURE lifts it: the army is what a pressed seat needs, and want_def needs `pressed`, so the
       defences and the hold exclude each other by construction. Seen air (want_aa) is a reaction to a threat and
       is not held.
       INVARIANT, a bound on the seat's own record: the store and storage (+0x98, +0xA8, through the WORLDQ) are
       whole metal held to 0..10^9 (metal_whole), so the products stay below 10^9 x 65 535 < 2^47; storage it
       cannot read (0, NaN) never holds; and the hold is never set beside a surplus, by its own term, so an idle
       factory beside a store at SURPLUS_FRAC or more is never held -- stall_pm < SURPLUS_FRAC x 1000
       (impai_brain_test.c, check_plan) keeps the band between the two non-empty. The posture's `pressed` is
       bounded where it is read (posture_read). */
    {
        const int64_t s = metal_whole(e->metal), m = metal_whole(e->metal_max);
        d->hold = m > 0 && s * 1000 < m * (int64_t)plan->stall_pm && !d->surplus && !d->pos.pressed;
    }
}

/* THE VETOES: which gate holds which spend, one row a want. A decision's gates are the IMPAI_G_* bits of
   IMPAI_DEC.gates, set once (economy); a want is its need and no gate its row names:
   want = need && !(gates & impai_veto[want].gates). A want's need -- what the seat lacks, its income, its counts --
   stays in economy; what refuses the spend whatever the need is here and nowhere else.
   - The next plant (want_factory past the first: the pacing's, the surplus's, THE REACHING PLANT) waits on the
     military's judgement of the map, on both stalls and on THE HOLD. The first plant is held by no gate and has
     no row.
   - The sea plant waits on both stalls and the hold, and not on the judgement: its need is the judgement made
     naval, or THE FINISH AT SEA, which asks for none.
   - The tech-up waits on both stalls and the hold.
   - A ground defence waits on the energy stall alone: the pressure it needs lifts the hold, and a metal stall
     beside the pressure does not hold it (economy, THE GROUND DEFENCES).
   - An anti-air structure waits on nothing: seen air is a threat, answered whatever the economy (THE HOLD).
   - A sensor waits on both stalls and the hold.
   No row names the pressure or the surplus: each acts through THE HOLD, which neither stands beside, and through
   the needs. impai_brain_test.c, check_veto_rows, holds the table to one row a want at the want's own index. */
const IMPAI_VETO impai_veto[IMPAI_WANTS] = {
    [IMPAI_W_FACTORY] = { "want_factory", IMPAI_G_UNJUDGED | IMPAI_G_E_STALL | IMPAI_G_M_STALL | IMPAI_G_HOLD },
    [IMPAI_W_SEA]     = { "want_sea",     IMPAI_G_E_STALL | IMPAI_G_M_STALL | IMPAI_G_HOLD },
    [IMPAI_W_TECH]    = { "want_tech",    IMPAI_G_E_STALL | IMPAI_G_M_STALL | IMPAI_G_HOLD },
    [IMPAI_W_DEF]     = { "want_def",     IMPAI_G_E_STALL },
    [IMPAI_W_AA]      = { "want_aa",      0u },
    [IMPAI_W_SENSOR]  = { "want_sensor",  IMPAI_G_E_STALL | IMPAI_G_M_STALL | IMPAI_G_HOLD },
};

/* THE REASON TRACE (impai.h, IMPAI_WHY), read once economy has set every want: `need`, bit k, is want k as it
   would stand with every gate down (less the term read last, impai.h). INVARIANT: a want that stands was needed,
   and a gate is named only for a want needed and not standing -- so the first plant, which no gate holds, names
   none while THE HOLD is up beside it. impai_brain_test.c, why_check, holds every decision of the test to it. */
static void why_read(IMPAI_DEC* d, unsigned need)
{
    const int want[IMPAI_WANTS] = { [IMPAI_W_FACTORY] = d->want_factory, [IMPAI_W_SEA] = d->want_sea,
                                    [IMPAI_W_TECH] = d->want_tech,       [IMPAI_W_DEF] = d->want_def,
                                    [IMPAI_W_AA] = d->want_aa,           [IMPAI_W_SENSOR] = d->want_sensor };
    IMPAI_WHY* w = &d->out->why;
    int k;
    memset(w, 0, sizeof *w);
    w->set = 1;
    w->gates = (uint8_t)d->gates;
    w->naval = d->pos.naval;
    w->pos = (uint8_t)((d->pos.dry ? IMPAI_WHY_DRY : 0u) | (d->pos.island ? IMPAI_WHY_ISLAND : 0u) |
                       (d->pos.blind ? IMPAI_WHY_BLIND : 0u) | (d->pos.pressed ? IMPAI_WHY_PRESSED : 0u) |
                       (d->pos.at_base ? IMPAI_WHY_AT_BASE : 0u));
    for (k = 0; k < IMPAI_WANTS; k++) {
        const unsigned bit = 1u << k;
        if (need & bit) w->need |= (uint8_t)bit;
        if (want[k]) w->take |= (uint8_t)bit;
        if ((need & bit) && !want[k]) w->veto[k] = (uint8_t)(d->gates & impai_veto[k].gates);
        /* THE REQUESTS (impai.h), before the assignment: one that stands is open until a rung ends it */
        d->out->req[k] = (uint8_t)(want[k] ? IMPAI_RQ_OPEN : !(need & bit) ? IMPAI_RQ_NONE :
                                   w->veto[k] ? IMPAI_RQ_VETOED : IMPAI_RQ_HELD);
    }
}

/* the names the `why` line gives the gates, bit 0 first, and the posture's flags (IMPAI_WHY_*) */
static const char* const WHY_GATE[] = { "HOLD", "E_STALL", "M_STALL", "PRESSED", "SURPLUS", "UNJUDGED" };
static const char* const WHY_POS[] = { "dry", "island", "blind", "pressed", "at_base" };
_Static_assert((1u << sizeof WHY_GATE / sizeof WHY_GATE[0]) - 1u == IMPAI_G_ALL, "a name a gate");

/* BOUND: every write is held below `end`, which is left for the NUL */
static void why_put(char** p, const char* end, const char* s)
{
    while (*s && *p < end) *(*p)++ = *s++;
}

/* the names of `bits` among `name`'s `n`, joined by `+`; `-` for none */
static void why_names(char** p, const char* end, unsigned bits, const char* const* name, int n)
{
    int k, any = 0;
    for (k = 0; k < n; k++)
        if (bits & (1u << k)) {
            if (any++) why_put(p, end, "+");
            why_put(p, end, name[k]);
        }
    if (!any) why_put(p, end, "-");
}

int impai_why_text(const IMPAI_WHY* w, char* out, size_t cap)
{
    /* a want's name is its field's less `want_` (impai_veto[k].want), which check_veto_rows holds each row to */
    const char* want[IMPAI_WANTS];
    char* p = out;
    const char* end;
    int k, any = 0;
    if (!cap) return 0;
    end = out + cap - 1;
    for (k = 0; k < IMPAI_WANTS; k++) want[k] = impai_veto[k].want + 5;
    why_put(&p, end, "gates=");
    why_names(&p, end, w->gates, WHY_GATE, (int)(sizeof WHY_GATE / sizeof WHY_GATE[0]));
    why_put(&p, end, w->naval > 0 ? " naval=1" : w->naval < 0 ? " naval=-1" : " naval=0");
    why_put(&p, end, " pos=");
    why_names(&p, end, w->pos, WHY_POS, (int)(sizeof WHY_POS / sizeof WHY_POS[0]));
    why_put(&p, end, " need=");
    why_names(&p, end, w->need, want, IMPAI_WANTS);
    why_put(&p, end, " veto=");
    for (k = 0; k < IMPAI_WANTS; k++)
        if (w->veto[k]) {
            if (any++) why_put(&p, end, ",");
            why_put(&p, end, want[k]);
            why_put(&p, end, ":");
            why_names(&p, end, w->veto[k], WHY_GATE, (int)(sizeof WHY_GATE / sizeof WHY_GATE[0]));
        }
    if (!any) why_put(&p, end, "-");
    why_put(&p, end, " take=");
    why_names(&p, end, w->take, want, IMPAI_WANTS);
    *p = 0;
    return (int)(p - out);
}

int impai_why_due(IMPAI_WHY_SEEN* seen, uint32_t now, const IMPAI_WHY* w)
{
    if (!w->set) return 0;
    if (now < seen->after) seen->why.set = 0;   /* not above the last decision's: no remembered trace equals `w` */
    seen->after = now + 1;
    if (!memcmp(w, &seen->why, sizeof *w)) return 0;
    seen->why = *w;
    return 1;
}

/* the names the `plantwalk` line gives the rung's returns (IMPAI_PW_*) */
static const char* const PW_EXIT[] = { "notype", "menu", "sea", "left", "cut", "grew", "widest", "way", "roots" };
_Static_assert(sizeof PW_EXIT / sizeof PW_EXIT[0] == IMPAI_PW_EXITS, "a name a return");

/* `name` and then v in decimal. BOUND: a uint32 is at most ten digits, and why_put's: every write below `end` */
static void pw_field(char** p, const char* end, const char* name, unsigned v)
{
    char digits[10];
    int n = 0;
    do digits[n++] = (char)('0' + v % 10u); while ((v /= 10u) && n < (int)sizeof digits);
    why_put(p, end, name);
    while (n && *p < end) *(*p)++ = digits[--n];
}

int impai_plantwalk_text(const IMPAI_PLANTWALK* w, char* out, size_t cap)
{
    char* p = out;
    const char* end;
    if (!cap) return 0;
    end = out + cap - 1;
    pw_field(&p, end, "builder=", w->slot);
    pw_field(&p, end, "/", w->btype);
    pw_field(&p, end, " waived=", w->waived);
    pw_field(&p, end, " grow=", w->grow);
    pw_field(&p, end, " sites=", w->sites);
    pw_field(&p, end, " claimed=", w->claimed);
    pw_field(&p, end, " failed=", w->failed);
    pw_field(&p, end, " far=", w->far_rec);
    pw_field(&p, end, " way_failed=", w->way_failed);
    pw_field(&p, end, " coverage=", w->coverage);
    pw_field(&p, end, " beyond=", w->beyond);
    pw_field(&p, end, " threat=", w->threat);
    pw_field(&p, end, " tested=", w->tested);
    pw_field(&p, end, " passed=", w->passed);
    pw_field(&p, end, " way_refused=", w->way_refused);
    pw_field(&p, end, " unbuilt=", w->unbuilt);
    pw_field(&p, end, " cut=", w->cut);
    why_put(&p, end, " exit=");
    why_put(&p, end, w->exit < IMPAI_PW_EXITS ? PW_EXIT[w->exit] : "?");
    why_put(&p, end, w->sea == IMPAI_PW_SEA_UNASKED ? " sea=-" : w->sea < 0 ? " sea=none" : " sea=wait");
    *p = 0;
    return (int)(p - out);
}

int impai_plantwalk_due(IMPAI_PLANTWALK_SEEN* seen, uint32_t now, const IMPAI_PLANTWALK* w)
{
    if (!w->set) return 0;
    if (now < seen->after) seen->walk.set = 0;  /* not above the last decision's: no remembered trace equals `w` */
    seen->after = now + 1;
    if (!memcmp(w, &seen->walk, sizeof *w)) return 0;
    seen->walk = *w;
    return 1;
}

static void economy(IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const IMPAI_MIL* mil = d->in->mil;
    const WQ_ECON* e = &d->e;
    const float fac = (float)d->count[IMPAI_C_FACTORY];
    const int mex_done = d->done[IMPAI_C_MEX] > 0 ? d->done[IMPAI_C_MEX] : 1;
    const int judged = d->pos.naval >= 0;
    int plant_pace, plant_metal, plant_reach = 0, reach_need = 0, tech_metal = 0, no_patch, first;
    unsigned need = 0;              /* THE REASON TRACE: bit k, want k with every gate down (why_read)       */
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
       - A PAYOUT REACHES A BUILDER, A RECLAIMER OR A FRAME (engine/sim.md, the handicap's rows 9, 10 and 12):
         the reclaim completion credits the reclaimer (0x423918..0x42395B), BuildingBuild's refund the factory
         running it (0x40269F..0x4026DD), the unbuild the frame being unbuilt (0x41BBB1..0x41BBEE), and the
         destructor, at a death of kind 5 (+0x0A's high nibble, 0x486CA2), the killer (0x486CAD..0x486D1E, a
         reclaimer [INFERRED]). So only a frame (not finished), a factory, a unit of workertime above 0 or a
         canreclamate one has its excess subtracted -- a factory by its category, since a product other builders
         helped can be torn down on a factory of no workertime --, and the excess of any other unit stays income:
         a script or a mod that credits a finished structure's +0xD4 every pass makes income the def does not
         show. MEASURED 2026-11-14 on TA Twilight
         (bar-nota-pincushion-ac-1): its ARMADVSOL, metalmake, makesmetal and extractsmetal 0 in the def, made
         1.34, 2.69 or 4.03 a pass at every sample from GameTime 10 800, 18.8 of the seat's 27.1 a pass from six
         of them, which the excess rule alone counted as payouts.
       - The settle moves +0xD4 to +0xE4 (0x401B7E..0x401B8E) in the call that sums it into +0x9C (0x401A05). The
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
    {
        /* THE STALLS, below stall_pm of the storage, the plan's one store threshold, THE HOLD's too; 200 / 1000.0f
           is the float nearest 0.2, as 0.2f is. THE ENERGY STALL IS THE STORE'S: the store below the line,
           whatever the last pass's flows. The store sums the flows over every pass, so a store below the line is
           spending that outran the income lately, and one rises above it only on more coming in than going out:
           the stall ends by its own rule, the ladder's first, every idle builder building energy until it does.
           WHY NOT THE FLOWS: a pass's flows flip between a factory's products -- one asks nothing while its
           next is not begun -- and the stall read them: on Escalation's bar-nota-pincushion-ac-1 at GameTime 7 604
           the store stood at 6 % with 64 coming in against 23.7 asked, so no stall was declared and the second
           plant was ordered inside it (MEASURED 2026-11-15, the economy diagnosis at d665a00a). THE METAL STALL
           keeps the flows' test, beside THE HOLD, which reads the store alone (impai_brain_test.c, the hold's
           cases) */
        const float stall = (float)d->in->plan->stall_pm / 1000.0f;
        d->e_stall = e->energy < stall * e->energy_max;
        d->m_stall = e->metal < stall * e->metal_max && d->m_steady < e->metal_out;
    }
    /* THE GATES, once a decision: every want below reads them through its row of impai_veto */
    d->gates = (d->hold ? IMPAI_G_HOLD : 0u) | (d->e_stall ? IMPAI_G_E_STALL : 0u) | (d->m_stall ? IMPAI_G_M_STALL : 0u) |
               (d->pos.pressed ? IMPAI_G_PRESSED : 0u) | (d->surplus ? IMPAI_G_SURPLUS : 0u) | (judged ? 0u : IMPAI_G_UNJUDGED);
    d->out->pressed = d->pos.pressed;
    d->out->hold = (uint8_t)d->hold;
    d->out->e_empty = (uint8_t)(e->energy_max > 0.0f && e->energy < IMPAI_E_EMPTY * e->energy_max);
    d->out->f_idle = (uint16_t)d->nfidle;
    d->out->f_all = (uint16_t)(d->nfidle + d->nfbusy);
    d->pe_in = e->energy_in + d->pe_extra;
    d->pm_in = d->m_steady * (1.0f + (float)d->pending[IMPAI_C_MEX] / (float)mex_done);
    /* IN A SURPLUS THE FACTORIES ARE PACED BY WHAT THEY SPEND (THE PLANT'S PRICE), not held to factory_max: while
       the store is at SURPLUS_FRAC of its maximum or more, a further plant is wanted when the metal the seat makes
       and does not ask for pays what one of its busy plants asks.
       - THE PRICE is measured, not planned: the build step asks on the builder's own ledger (0x41BC5A), the
         settle moves each pass's request to the ledger's last-request fields (+0xE8 metal, +0xD0 energy;
         0x401B75..0x401B7B, 0x401B49..0x401B4D), and a request is recorded whether or not it is paid (0x4011C0),
         so a factory's +0xE8 is what it spends a pass at its rate. The price is the mean over the census's
         factories at a BuildingBuild that asked metal; a factory waiting on its yard or at the unit limit asks
         none and is left out, and with none asking there is no price and no surplus plant.
       - THE OVERFLOW (over_m): the surplus, the price measured, and m_steady less the seat's request (+0xA0) at
         least the price -- the seat makes a busy plant's spending a pass more than it asks. The metal producers
         wait on it (impai_econ.c, IN A SURPLUS THE METAL PRODUCERS WAIT): a further extractor's metal would only
         add to it.
       - THE METAL TERM (plant_m): the overflow, no idle factory -- in a surplus the governor gives every idle one
         an order (impai_econ.c, THE GOVERNOR, A SURPLUS), so one idle at the census is a plant between products,
         or one the persona's budget has not reached, and a new plant beside a factory with nothing to make, or no
         action to make it with, would spend nothing -- and fewer than IMPAI_IDLE_CAP factories, the census's
         factory lists' bound.
       - THE ENERGY TERM (plant_e): the energy a pass the seat makes less what it asks -- the ask with THE FLOWS'
         LAG of the makers switched since the pass (impai_econ.c, THE MAKERS) --, plus what its active makers cost
         (THE MAKERS switch them off when energy runs short), at least the price's energy. EVERY PLANT PAST THE
         FIRST WAITS ON IT -- the surplus plant, the next plant below factory_max and the tech-up -- since a plant
         spends its price a pass for as long as it makes, and the ask is the request, paid or not (the player's
         +0x94, engine/weapons.md, The settle). With no factory asking there is no price, and the term is the
         income at least the ask. With a plant's metal and not its energy, energy is the limit: plant_e_need is
         the energy a pass short, and THE LADDER's energy rule raises its target by it (impai_econ.c), so the
         energy comes first and the plant at a later decision. WHY: on Escalation, whose first products ask about
         3 000 energy each (ARMCK 3 045, ARMCV 3 615), the second plant came on metal alone and was built with the
         store at 0 (MEASURED 2026-11-15, the economy diagnosis at d665a00a).
       - Below factory_max the plan's pacing stands: a second plant at factory_per_m of steady income a pass for
         each factory, its energy permitting.
       INVARIANT, a bound: a surplus plant is wanted only while every factory is busy, at least one asking metal,
       and the unasked income pays the mean busy plant's request, so each plant past factory_max comes on income
       the plants standing do not spend, one pending at a time, and the count stays below IMPAI_IDLE_CAP. Every
       value is the census's or the record's, held to [0, 1e9) by the WORLDQ (wq_unit, the record's floats); a
       NaN fails each compare.
       Not gated on ftop_n: the measured end state, factories {T1: 2, T2: 1}, has ftop_n 1, below factory_max, so
       the tech-up's own test never holds and the seat stopped at three. WHY: the store stood full 50-67 % of the
       time from GameTime 9 000 in the yard fix's Hard games on Trout Farm and 26-31 % of the metal made was
       wasted, at 3-6 factories against an income of 37-54 (MEASURED 2026-11-14 from the recordings; the player's
       MetalWasted +0xD4); the plan's flat factory_per_m held the fifth plant until the steady income reached 40.
       A tech-up due beside it comes first (impai_econ.c, THE TECH-UP FIRST); otherwise THE CHOICE takes a plant
       of build tech at most the seat's highest, by its units' values, and with none the least value: a T1 plant
       while one is the cheapest -- a GAP, since in a surplus the dearer plant would spend more. Past factory_max
       m_short's want is the tech-up's tech_m only while the tech-up's own count holds (SHORT OF METAL, below). */
    {
        const float p_m = d->price_n ? d->price_m / (float)d->price_n : 0.0f;
        const float p_e = d->price_n ? d->price_e / (float)d->price_n : 0.0f;
        const float e_room = e->energy_in - (e->energy_out + d->mk_lag_e) + d->mk_on_e;
        d->over_m = d->surplus && d->price_n > 0 && d->m_steady - e->metal_out >= p_m;
        d->plant_m = d->over_m && d->nfidle == 0 && d->count[IMPAI_C_FACTORY] < IMPAI_IDLE_CAP;
        d->plant_e = e_room >= p_e;
        d->plant_e_need = p_e - e_room;
    }
    /* plant_metal: the next plant past the first is paid in metal; the energy term waits on it in want_factory.
       THE REACHING PLANT (impai_econ.c, THE METAL'S REACH): with no constructor of the seat's reaching the open
       patches, more of them than its extractors, and a plant on its menus whose constructor does, that plant is
       wanted whatever the income, below the factory lists' IMPAI_IDLE_CAP (plant_reach), and THE CHOICE's further
       plant is only such a plant (mr_only). The want is THE CHOICE's alone, never the sea plant's: a naval seat's
       yard waits on plant_metal as before, and while it is not wanted the reaching plant is the next one. WHY:
       with the want fed to want_sea, a naval Steel Jungle seat whose only builder of a shipyard was its
       commander, busy every decision, wanted the yard from GameTime 11 707 to the end and never ran THE CHOICE,
       which want_sea holds back, nor ordered the yard (MEASURED 2026-11-16, round 3's p:L5 ca-1 with a diagnostic
       build at 1322160c: `naval=1 chose=none sea_site=-`) */
    plant_pace = d->count[IMPAI_C_FACTORY] >= 1 &&
                 ((d->count[IMPAI_C_FACTORY] < plan->factory_max && d->m_steady >= d->factory_per_m * fac) || d->plant_m);
    plant_metal = plant_pace && !(d->gates & impai_veto[IMPAI_W_FACTORY].gates);
    if (d->count[IMPAI_C_FACTORY] >= 1 && !d->pending[IMPAI_C_FACTORY] && impai_econ_metal_short(d) &&
        impai_econ_reach_plant(d)) {
        d->mr_only = 1;
        reach_need = d->count[IMPAI_C_FACTORY] < IMPAI_IDLE_CAP;
        plant_reach = reach_need && !(d->gates & impai_veto[IMPAI_W_FACTORY].gates);
    }
    /* NO PATCH ON THE MAP: the first plant is also wanted at once when the seat's patch scan has read the whole map
       (IMPAI_BRAIN_IN.scanned) and published no patch, off a metal map. The scan reads the map's layout, so the map
       holds no closed metal patch at all: the opening's extractors have nothing to stand on, the commander would
       stand idle to open_t, and the plant is what the opening can start. The opening's own terms are unchanged.
       INVARIANT, a bound on when it fires: `scanned` is set only at a scan's last row (impai_know.c), so before the
       first whole scan nothing changes, and a seat that knows a patch, or stands on a metal map, whose extractors
       take the base ring and never a patch (impai_metal_map), never takes it. */
    no_patch = d->in->scanned && !d->in->nspots && !impai_metal_map(d);
    first = d->count[IMPAI_C_FACTORY] == 0 &&
            ((d->done[IMPAI_C_MEX] >= plan->open_mex && d->count[IMPAI_C_ENERGY] >= plan->open_energy) ||
             d->now >= plan->open_t || no_patch);
    d->want_factory = !d->pending[IMPAI_C_FACTORY] && (first || ((plant_metal || plant_reach) && d->plant_e));
    if (!d->pending[IMPAI_C_FACTORY] && (first || ((plant_pace || reach_need) && d->plant_e)))
        need |= 1u << IMPAI_W_FACTORY;
    /* THE FINISH AT SEA. While the military's dry hunt holds (impai_mil.c, dry_hunt: no enemy known on ground
       land-linked to the anchor for IMPAI_DRY_T, and one known off it within them), what is left of the enemy
       stands where the land army cannot walk, so the seat wants its sea plant whatever its pacing and its factory
       count below the factory lists' IMPAI_IDLE_CAP: the land plants have nothing left to make (impai_econ.c,
       THE FINISH'S BUY: only types with a water weapon are bought while the finish has a plant), and the plant's
       spending is the finish's; a frame of it left unfinished is resumed at the want's rung (impai_build.c, THE
       FRAME RESUMED). The search then reaches FAR_PATCH_K x comm_r of the
       anchor, on held ground past comm_r, and the commander may build there (impai_build.c, sea_radius), since the
       base's own shore can be too shallow for a shipyard -- Trout Farm's west start has no candidate within comm_r
       (MEASURED at 198899ef, bar-nota-troutfarm-aa-2: `sea_site=none`) and its nearest lies off a shore no land
       constructor reaches it from (impai_build.c, THE BUILDER'S REACH). The rest of the want is the sea plant's:
       one plant but for THE FINISH'S YARDS below, the stalls, the hold, the refusals and a builder that offers
       one. A further sea plant on an island is THE CHOICE's (impai_econ.c, THE ISLAND'S PLANTS).
       THE FINISH'S YARDS: past its first sea plant the finish wants one more while THE OVERFLOW stands (over_m:
       the surplus, a busy plant's price measured, and the steady income less the seat's request at least that
       price), every sea plant the census counts -- finished, a frame or a builder's order (sea_plants) -- is a
       finished factory at work, THE ENERGY TERM holds (plant_e), and the seat knows a target on or under the
       water (THE TARGETS KNOWN, impai_tg_known: FLOAT or UNDER), which is what a yard's army hits: the dry hunt
       also holds over an enemy on land the land army does not reach, where a further yard buys nothing that
       reaches it. It is THE METAL TERM (plant_m) read over the sea plants: in the dry hunt a land plant is
       offered only its types with a water weapon (impai_econ.c, army_menu), and one whose menu holds none stands
       idle by that rule, so plant_m's "no idle factory" may never hold there while the sea plants are what
       spends (decisions.md, *The finish's yards*). INVARIANT, a bound: a further yard is wanted only while no yard is idle, unfinished or on
       order, a busy plant asks metal, and the income nobody asks for pays the mean busy plant's request -- so
       each yard past the first comes on metal the full store is wasting, one at a time, below IMPAI_IDLE_CAP
       factories --, and under the sea plant's own gates: the stalls and the hold refuse it. Every value is the
       census's of this decision; the busy list is cut at IMPAI_IDLE_CAP, which only refuses. */
    d->sea_able = (uint8_t)(d->pos.dry && impai_econ_sea_able(d));
    {
        int sea_busy = 0, more, sea_need, k;
        for (k = 0; k < d->nfbusy; k++) sea_busy += impai_water_plant(d->in, d->fbusy[k].type);
        more = d->pos.dry && d->over_m && d->plant_e && sea_busy == (int)d->sea_plants && mil &&
               (impai_tg_known(mil, d->now) & ((1u << IMPAI_TG_FLOAT) | (1u << IMPAI_TG_UNDER)));
        /* the need but for a builder that offers one: the menus are read last, for a want nothing else refuses */
        sea_need = (d->pos.naval == 1 || d->pos.dry) && (!d->sea_plants || more) && d->count[IMPAI_C_FACTORY] >= 1 &&
                   (d->pos.dry ? d->count[IMPAI_C_FACTORY] < IMPAI_IDLE_CAP : plant_pace) &&
                   !d->pending[IMPAI_C_FACTORY] && d->in->st->sea_refused < IMPAI_SEA_REFUSALS;
        d->want_sea = sea_need && !(d->gates & impai_veto[IMPAI_W_SEA].gates) &&
                      (d->pos.dry ? d->sea_able : impai_econ_sea_able(d));
        if (sea_need) need |= 1u << IMPAI_W_SEA;
    }
    /* TECH-UP: with factory_max factories of its highest build tech (IMPAI_TINFO.ctech: the tech its plants'
       constructors build, frames included) and a steady metal income (THE STEADY INCOME) of the table's tech_m a
       pass, the next factory's build tech is higher (impai_econ.c, THE CHOICE) */
    if (d->in->mil) {
        const IMPAI_MIL* m = d->in->mil;
        const float tech_m = m->table.tech_m;
        const int tech_need = tech_m > 0 && d->m_steady >= tech_m && d->ftop_n >= plan->factory_max;
        int aa_need, def_need, sensor_need;
        tech_metal = tech_need && !(d->gates & impai_veto[IMPAI_W_TECH].gates);
        d->want_tech = !d->pending[IMPAI_C_FACTORY] && tech_metal && d->plant_e;
        if (!d->pending[IMPAI_C_FACTORY] && tech_need && d->plant_e) need |= 1u << IMPAI_W_TECH;
        /* ANTI-AIR STRUCTURES: one per aa_struct_v of seen enemy air value, at most aa_struct_max */
        aa_need = plan->aa_struct_v > 0 && d->aa_n < plan->aa_struct_max &&
                  m->air_seen >= plan->aa_struct_v * (float)(d->aa_n + 1);
        d->want_aa = aa_need && !(d->gates & impai_veto[IMPAI_W_AA].gates);
        if (aa_need) need |= 1u << IMPAI_W_AA;
        /* THE GROUND DEFENCES (impai_econ.c): one while THE PRESSURE stands at the base, one more for each def_v
           of pressing value, at most def_max, outside an energy stall. A NaN press_v fails the compare and wants
           nothing. A METAL STALL DOES NOT HOLD IT BACK, as the pressure lifts THE HOLD: a raid that lasts keeps
           the store low while the factories buy the army, so m_stall stood beside the pressure and the towers it
           is for never came. An energy stall still holds it back: the economy pass pays each resource at one
           fraction (0x401A4D..0x401AB1, engine/weapons.md, The settle), so in an energy stall a frame grows at the
           energy's fraction, and the stall's energy is the ladder's first rule. */
        def_need = d->pos.at_base && d->def_n < plan->def_max && plan->def_v > 0.0f &&
                   (float)d->def_n < 1.0f + m->press_v / plan->def_v;
        d->want_def = def_need && !(d->gates & impai_veto[IMPAI_W_DEF].gates);
        if (def_need) need |= 1u << IMPAI_W_DEF;
        if (d->want_def) def_point(d, m);
        /* THE SENSORS (impai_econ.c): with factory_max plants finished, outside the stalls and the hold, below
           sensor_max */
        sensor_need = d->done[IMPAI_C_FACTORY] >= plan->factory_max && d->sens_n < plan->sensor_max;
        if (sensor_need) need |= 1u << IMPAI_W_SENSOR;
        if (sensor_need && !(d->gates & impai_veto[IMPAI_W_SENSOR].gates)) sensor_want(d, m);
    }
    /* plant_e_need: the energy a pass short of THE ENERGY TERM, while a plant's metal is there and its energy is not */
    if (!((plant_metal || plant_reach || tech_metal) && !d->plant_e && d->plant_e_need > 0.0f)) d->plant_e_need = 0.0f;
    /* SHORT OF METAL for what the seat wants next: stalling on it, or making less a pass than its next want
       asks -- the next factory's factory_per_m x the factories it has (the second factory's test above, the
       first's taken as one), and with factory_max of them the tech-up's tech_m while the tech-up's own count
       holds, factory_max plants of the highest build tech (want_tech's). Past it -- a tech-up built, its plant
       the first of the new tech -- the seat wants no plant outside a surplus, and only a stall is short. A seat
       short of metal takes a feature before the buildings that spend (impai_econ.c, THE LADDER). A SURPLUS IS NEVER
       SHORT: with the store at SURPLUS_FRAC of its maximum or more (hold_read) the store pays the next want
       whatever the income, so the income's test holds only below it. WHY: the test read the income alone, and on
       Escalation's bar-nota-pincushion-ac-1 the commander reclaimed beside a full metal store, ahead of the
       energy the seat was stalling on (MEASURED 2026-11-15, the economy diagnosis at d665a00a). THE ROOM: what its
       store can still take, less what its reclaims under way will pay (the census); a payout past it is lost, the
       economy pass holding the store to its maximum (0x401AFE..0x401B27, engine/weapons.md, The settle). */
    {
        const int nf = d->count[IMPAI_C_FACTORY];
        float want_m = nf < plan->factory_max ? d->factory_per_m * (float)(nf > 1 ? nf : 1) : 0.0f;
        if (nf >= plan->factory_max && mil && d->ftop_n >= plan->factory_max) want_m = mil->table.tech_m;
        d->m_short = d->m_stall || (!d->surplus && d->m_steady < want_m);
        d->m_room = e->metal_max - e->metal - d->m_inflight;
    }
    why_read(d, need);
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
   THE SPLIT: the reserve is also at most roots_max / 2, so a budget of two or three actions gives the factories
   one and the builders the rest. The plan's queue_max is at most roots / 2 (impai_brain_test.c, check_plan), so
   with no budget the halving never binds.
   THE FACTORIES' TURN, an ORDERING: a decision of one command whose idle factories want a root -- the contested
   decision (IMPAI_DEC.contested) -- is the factories' when IMPAI_BRAIN.root_turn is 1 and the builders' when it
   is 0. On the factories' turn the root is reserved past the job checks' stops, the ladder, the yard and the
   makers, the factories' loop (impai_econ_factories) runs before the builders' assignment, and the builders are
   given whatever root it left; on the builders' turn nothing is reserved, the assignment runs first and the
   factories' loop takes the root it left. EVERY CONTESTED DECISION PASSES THE TURN, whether or not the factories
   spent the root, so the turns alternate: of any two contested decisions in a row one gives the factories the
   first claim on the root and the other gives everything else, whatever the budget's rate or burst, and what the
   factories' turn holds -- a job's stop, an arrived walk's reclaim, the yard's step, the makers' switch -- waits at
   most one contested decision. A turn that stayed with the factories until they spent a root held all of that for
   as long as an idle factory stood unordered (poor, unscored, an empty menu, past the rows' cap or refused): the
   yard's deadlock again; `turn_unspent=` on the eco line counts
   the factories' turns that spent nothing. A root the governor leaves on its turn is not lost: the builders take
   it. A turn back out of danger is never held by the factories' turn (order_mode: roots_max on a contested
   decision). WHY: unused actions stay in the bucket up to its burst, so a seat whose builders spent each action
   as it came never reached a decision of two, and its factories were never given one -- Normal's 24 economy
   actions a minute went to the builders while the factories stood idle beside full stores, 26 % of the samples on
   TA Zero and 14-18 % on TA Twilight (MEASURED 2026-11-15, the economy diagnosis at d665a00a). COST: on the
   factories' turn a stop, the yard's step and the makers wait for the next contested decision. */
static int root_cap_of(const IMPAI_DEC* d, int* contested)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const int floor = plan->hold_fac > d->nfbusy ? plan->hold_fac - d->nfbusy : 0;
    int reserve = d->nfidle < plan->queue_max ? d->nfidle : plan->queue_max;
    if (d->hold && floor < reserve) reserve = floor;
    *contested = d->roots_max == 1 && reserve > 0;
    if (*contested) reserve = d->in->st->root_turn ? 1 : 0;
    else if (reserve > d->roots_max / 2) reserve = d->roots_max / 2;
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
    d->held_own = (uint8_t*)p; p += ALIGN16(GRID_CELLS);
    d->held_foe = (uint8_t*)p; p += ALIGN16(GRID_CELLS);
    d->held_q = (uint16_t*)p; p += ALIGN16(GRID_CELLS * sizeof(uint16_t));
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
    posture_read(d);
    d->sea_type = out->sea_type = (uint16_t)WQ_NONE;
    q->ops->map(q, &d->m);
    if (q->ops->economy(q, &d->e) != WQ_OK) memset(&d->e, 0, sizeof d->e);
    d->act_build = q->ops->action(q, WQ_ACT_MOBILEBUILD);
    d->act_vtol_build = q->ops->action(q, WQ_ACT_VTOL_MOBILEBUILD);
    d->act_queue = q->ops->action(q, WQ_ACT_BUILDINGBUILD);
    d->act_park = q->ops->action(q, WQ_ACT_PARK);
    d->act_on = q->ops->action(q, WQ_ACT_ACTIVATE);
    d->act_off = q->ops->action(q, WQ_ACT_DEACTIVATE);
    d->e_per_m = in->e_per_m > 0.0f && in->e_per_m < 1e6f ? in->e_per_m : in->plan->e_per_m;
    d->factory_per_m = in->plan->factory_per_m;
    if (st->explorer != WQ_NONE && st->explorer >= in->njob) st->explorer = (uint16_t)WQ_NONE;
    /* THE MEMORY'S COUNT IS DATA, as every recorded field is: a restored nfmem past IMPAI_FEATS is held to it before
       anything walks the memory or its marks (the census's claims, the refresh, the place, the reclaim's scores),
       so no walk passes the arrays */
    if (st->nfmem > IMPAI_FEATS) st->nfmem = IMPAI_FEATS;
    out->census = (uint16_t)census(d);
    head_counts(d);
    hold_read(d);
    d->roots_max = in->acts < 0 || in->acts > (int)in->plan->roots ? (int)in->plan->roots : in->acts;
    d->root_cap = root_cap_of(d, &d->contested);
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
    impai_held_judge(d);
    base_box(d);
    impai_fmem_refresh(d);
    impai_fmem_observe(d);
    help_checks(d);
    job_checks(d);
    walk_checks(d);
    yard_clear(d);
    economy(d);
    impai_econ_makers(d);
    if (d->contested && st->root_turn) {
        const int before = d->roots;
        impai_econ_factories(d);
        st->root_turn = 0;
        if (d->roots == before) d->out->turn_unspent++;
        d->root_cap = d->roots_max;
        impai_econ_assign(d);
        impai_sea_untaken(d);
    } else {
        impai_econ_assign(d);
        impai_sea_untaken(d);
        impai_econ_factories(d);
        if (d->contested) st->root_turn = 1;
    }
    /* THE COMMANDER'S IDLE SHARE, for the log: a commander outside the military's window that ends the decision
       with no job is the economy's and given nothing, which THE BLAST'S APPROACH and THE COMMANDER'S SAFE DISTANCE
       can cause */
    out->blast_refused = (uint8_t)d->blast_hit;
    if (d->commander.slot && !d->com_held) {
        out->com_free = 1;
        out->com_idle = in->job[d->commander.slot].kind == IMPAI_JOB_NONE;
    }
    out->sites = (uint16_t)d->sites;
    out->snaps = (uint16_t)d->snaps;
    out->roots = (uint16_t)d->roots;
    out->queued = (uint16_t)d->queued;
    out->sea_type = d->sea_type;
    out->sea_answer = (int8_t)d->sea_answer;
    /* THE PLANT WALK'S TRACE is of a decision that ordered no plant */
    if (out->plant || out->sea_plants) memset(&out->walk, 0, sizeof out->walk);
    return 0;
}
