/* impai_brain.c -- ImpAI's decision (impai.h, the brain): its entry, the census of the seat's units and the
   checks of the jobs it gave them. The ladder and the factories are impai_econ.c's, the sites and the orders
   impai_build.c's. Engine-free and without statics: everything it knows comes through the WORLDQ and its
   arguments, everything it keeps is the caller's (IMPAI_BRAIN, IMPAI_JOB). */
#include <string.h>
#include "impai_brain.h"
#include "tagpu_simreg.h"

/* The brain's entry, its census and the jobs' checks. OWNER: the brain decides only inside a live ImpAI
   seat's turn, which plays no puppet seat (tagpu_impai.c, impai_play); on a puppet level the level init's
   reset only clears the brain's state, which the recording then restores. */
SIM_FEATURE(impai_brain, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval");

/* THE LEVEL ROWS, Easy, Normal and Hard (research/notes/impai/brain.md, The levels). Each field's meaning and
   limit is in impai.h beside it. The values are hand-written starting points for the bar's diagnosis to move,
   not principles: Hard's economy is the row A1b's bench passed with, and Easy's and Normal's economy differ
   from it only in `decide` and in the noise on its ratios. tech_scale is each level's tech-up income over
   Hard's, 12, 10 and 8 a pass against the table's tech_m (impai_table.h), which is Hard's. */
#define ECONOMY(decide)                                                                                     \
    decide,             /* decide                                       */                                 \
    2, 2, 1800,         /* open_mex, open_energy, open_t                */                                 \
    2, 8.0f,            /* factory_max, factory_per_m                   */                                 \
    8.0f,               /* e_per_m                                      */                                 \
    4, 12, 3.0f,        /* cons_base, cons_max, cons_per_m              */                                 \
    1024, 1536,         /* comm_r, base_r                               */                                 \
    2,                  /* gap                                          */                                 \
    2700, 5400,         /* job_timeout, fail_t                          */                                 \
    32, 4, 32, 4, 4, 8, 8   /* idle_max, assign_max, site_tests, snaps, queue_max, roots, job_checks */
const IMPAI_LEVEL IMPAI_LEVELS[IMPAI_LVS] = {
    /* mil_period, mil_roots, react_confirm, predict_max, focus, raids, aa_struct_max; micro_hp, micro_period,
       rally_px; noise, attack_margin, retreat_margin, wave_value; scout_period, task_timeout; tech_scale,
       aa_struct_v */
    { ECONOMY(30), 150,  6, 2,  4, 0, 0, 2, 0.0f,  0, 384, 0.25f, 0.30f, -0.30f, 6.0f, 5400, 5400, 1.50f, 2000.0f, 0.75f },
    { ECONOMY(15),  60, 16, 1,  8, 1, 1, 4, 0.0f,  0, 384, 0.10f, 0.15f, -0.15f, 8.0f, 2700, 3600, 1.25f, 1500.0f, 0.50f },
    { ECONOMY(15),  30, 40, 1, 12, 1, 2, 6, 0.35f, 90, 384, 0.0f, 0.10f, -0.05f, 8.0f, 1800, 2700, 1.00f, 1000.0f, 0.25f },
};

int impai_brain_due(unsigned gametime, unsigned seat, const IMPAI_LEVEL* lv)
{
    return lv->decide && gametime % lv->decide == (7u * seat) % lv->decide;
}

float impai_noise(uint64_t seed, uint32_t gametime, unsigned tag, unsigned i)
{
    uint64_t z = seed ^ ((uint64_t)gametime << 16) ^ ((uint64_t)tag << 8) ^ (uint64_t)i;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (float)(uint32_t)(z >> 40) / 8388608.0f - 1.0f;     /* 24 bits: exact in a float */
}

void impai_mil_reset(IMPAI_MIL* m, unsigned level, uint64_t seed, const IMPAI_TABLE* table, int from_lever)
{
    unsigned k;
    memset(m, 0, sizeof *m);
    m->seed = seed;
    m->level = (uint8_t)(level < IMPAI_LVS ? level : IMPAI_LV_NORMAL);
    m->table_lever = (uint8_t)(from_lever != 0);
    m->ebx = m->ebz = IMPAI_NO_ANCHOR;
    m->naval = -1;
    if (table) m->table = *table;
    for (k = 0; k < IMPAI_SQUADS; k++) m->sq[k].target = (uint16_t)WQ_NONE;
}

/* the scratch's parts, each rounded up to 16 bytes */
#define ALIGN16(n) (((n) + 15u) & ~(size_t)15u)
#define MENU_BYTES   ALIGN16(WQ_TYPES_MAX * sizeof(uint16_t))
#define RING_BYTES   ALIGN16(PLACE_MAX_SITES * sizeof(PlaceCell))
#define GRID_CELLS   ((size_t)IMPAI_GRID_MAX * IMPAI_GRID_MAX)
#define MAPS_BYTES   (IMPAI_THREATS * ALIGN16(GRID_CELLS * sizeof(uint32_t)) + ALIGN16(GRID_CELLS * sizeof(int32_t)) + \
                      ALIGN16(GRID_CELLS * sizeof(uint16_t)) + ALIGN16((IMPAI_GRID_MAX + 1) * (size_t)IMPAI_GRID_MAX * sizeof(int32_t)))
#define WAY_BYTES    (ALIGN16(IMPAI_WAY_PASS_BYTES) + ALIGN16(IMPAI_WAY_BLOCK_CELLS * sizeof(int32_t)))
#define SCRATCH_BYTES (ALIGN16(sizeof(IMPAI_DEC)) + 2 * MENU_BYTES + RING_BYTES + MAPS_BYTES + WAY_BYTES)

_Static_assert(SCRATCH_BYTES <= IMPAI_BRAIN_SCRATCH_MAX, "a decision's scratch fits IMPAI_BRAIN_SCRATCH_MAX");

size_t impai_brain_scratch_bytes(void) { return SCRATCH_BYTES; }

void impai_brain_reset(IMPAI_BRAIN* st, IMPAI_JOB* job, unsigned njob)
{
    unsigned i;
    memset(st, 0, sizeof *st);
    st->ax = st->az = IMPAI_NO_ANCHOR;
    st->explorer = (uint16_t)WQ_NONE;
    st->sea_none_type = st->sea_cur_type = (uint16_t)WQ_NONE;
    for (i = 0; i < IMPAI_WAYS; i++) st->way[i].type = (uint16_t)WQ_NONE;
    for (i = 0; i < IMPAI_WAY_FAILS; i++) st->wfail[i].type = (uint16_t)WQ_NONE;
    for (i = 0; i < IMPAI_PLANT_REFS; i++) st->pref[i].type = (uint16_t)WQ_NONE;
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
    f->until = d->now + d->in->lv->fail_t;
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
    cat = t->cat < IMPAI_CATS ? t->cat : IMPAI_C_OTHER;
    building = !(t->flags & IMPAI_TF_MOBILE);
    d->count[cat]++;
    if (impai_water_plant(in, (unsigned)type)) d->sea_plants++;
    if (u->finished) d->done[cat]++;
    if (!d->lowest.slot) unit_ref(&d->lowest, u);
    if (cat == IMPAI_C_COMMANDER && !d->commander.slot) unit_ref(&d->commander, u);
    if (u->slot == in->st->explorer && job->kind == IMPAI_JOB_EXPLORE) d->explorer_live = 1;
    if ((cat == IMPAI_C_COMMANDER || cat == IMPAI_C_CONS) && u->finished && d->nbtype < IMPAI_SEA_BUILDERS) {
        int k;
        for (k = 0; k < d->nbtype && d->btype[k] != type; k++) {}
        if (k == d->nbtype) d->btype[d->nbtype++] = (uint16_t)type;
    }
    /* THE COMMANDER'S WINDOW. While GameTime < safe_until the military has moved the commander out of a fight
       it would lose (impai_mil.c, the commander's safety), and the economy neither lists it idle, nor as a
       helper, nor checks its job: so two managers never order it in one window, by this field and not by
       which runs first. It is still counted, and its head's build is still a claim. */
    if (cat == IMPAI_C_COMMANDER && in->mil && d->now < in->mil->safe_until) {
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
        if (t->offer > d->ftop) {
            d->ftop = t->offer;
            d->ftop_n = 0;
        }
        if (t->offer == d->ftop) d->ftop_n++;
    }
    if (building && impai_aa_struct(in, type)) d->aa_n++;
    if (cat == IMPAI_C_COMMANDER || cat == IMPAI_C_CONS) {
        /* a build order at the head is a claim on its site */
        if (h == 1 && (o.action == d->act_build || o.action == d->act_vtol_build) && o.type && o.type < in->nti) {
            const IMPAI_RECT r = rect_of(d, o.type, o.pos);
            impai_claim_add(d, o.type, &r);
            if (impai_aa_struct(in, o.type)) d->aa_n++;
            if (impai_water_plant(in, o.type)) d->sea_plants++;
            d->pending[in->ti[o.type].cat < IMPAI_CATS ? in->ti[o.type].cat : IMPAI_C_OTHER]++;
            if (in->ti[o.type].cat == IMPAI_C_ENERGY) d->pe_extra += in->ti[o.type].egain;
        }
        if (u->finished) {
            IMPAI_UNITREF r;
            const int idle = idle_head(d, u->slot, h, &o);
            unit_ref(&r, u);
            if (idle && d->nidle < in->lv->idle_max && d->nidle < IMPAI_IDLE_CAP) d->idle[d->nidle++] = r;
            else if (!idle && job->kind == IMPAI_JOB_ASSIST && d->nhelpers < IMPAI_IDLE_CAP) d->helpers[d->nhelpers++] = r;
            if (job->kind != IMPAI_JOB_NONE && d->njobbed < 2 * IMPAI_IDLE_CAP) {
                d->jobbed_idle[d->njobbed] = (uint8_t)idle;
                d->jobbed[d->njobbed++] = r;
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
            if (d->nfbusy < IMPAI_IDLE_CAP) d->fbusy[d->nfbusy++] = r;
            for (k = 0; k < n; k++) {
                const WQ_ORDER* q = &d->olist[k];
                if (q->action != d->act_queue || q->type >= in->nti || q->count <= 0) continue;
                if (in->ti[q->type].cat == IMPAI_C_CONS) d->queued_cons += q->count;
                else if (in->ti[q->type].klass < IMPAI_KS)
                    d->kspent[in->ti[q->type].klass] += in->ti[q->type].value * (float)q->count;
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
    }
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
            impai_dist_px(apos, (s->r.x0 + s->r.x1 + 1) * 8, (s->r.z0 + s->r.z1 + 1) * 8) > d->in->lv->base_r)
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
    if (st->sea_refused && d->now - st->sea_counted_at < d->in->lv->fail_t) return;
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
   (0x40605C) -- so this is the same compare on the census's reading, not a replay of the engine's. A site whose
   type has no information, or a builder with no def, is not beyond reach. */
static int out_of_reach(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int pcx, int pcz)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO *bt = &in->ti[b->type], *st;
    WQ_DEF def;
    int bfx, bfz, sfx, sfz;
    int64_t gap;
    if (type < 0 || type >= (int)in->nti || !d->q->ops->def(d->q, b->type, &def)) return 0;
    st = &in->ti[type];
    bfx = bt->fx > 0 ? bt->fx : 1; bfz = bt->fz > 0 ? bt->fz : 1;
    sfx = st->fx > 0 ? st->fx : 1; sfz = st->fz > 0 ? st->fz : 1;
    gap = (int64_t)impai_dist_px(b->pos, pcx, pcz) -
          impai_isqrt(64u * (uint64_t)(bfx * bfx + bfz * bfz)) - impai_isqrt(64u * (uint64_t)(sfx * sfx + sfz * sfz));
    return gap > (int64_t)def.reach;
}

static int site_out_of_reach(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz)
{
    const IMPAI_TINFO* st = type >= 0 && type < (int)d->in->nti ? &d->in->ti[type] : NULL;
    const int sfx = st && st->fx > 0 ? st->fx : 1, sfz = st && st->fz > 0 ? st->fz : 1;
    return out_of_reach(d, b, type, (2 * cx + sfx) * 8, (2 * cz + sfz) * 8);
}

/* THE FAR RECORD. A builder that ended its build beyond its reach is the one held back, for fail_t: no other
   builder and no site is, since the cause may be where the builder stands (a constructor walled in by its own
   plant's neighbours, MEASURED 2026-11-13 on Pincushion and Trout Farm, every patch of the map failed in turn).
   While the record is live the builder is given no site beyond its reach from where it stands -- the same compare
   as the abandon's -- so it cannot be sent from one unreachable site to the next, each failing in turn; and it
   takes no factory, sea-plant or tech-up order (impai_econ.c, assign), which a builder that can walk is given, so
   it cannot hold the seat's one plant want either. TERMINATION: the record ends at far_until, and the builder
   then takes any site again; a builder still walled in then ends one more build beyond reach and is held back
   one more fail_t, so its sites cost one job each fail_t, not one each decision. A water factory's still counts a
   refusal of the sea plant, its search's termination (impai_build.c, THE SEA PLANT). */
static void build_unreached(IMPAI_DEC* d, IMPAI_JOB* j)
{
    j->far_type = j->type;
    j->far_until = d->now + d->in->lv->fail_t;
    if (impai_water_plant(d->in, j->build)) impai_sea_refusal(d);
}

int impai_far(const IMPAI_DEC* d, const IMPAI_UNITREF* b) { return d->in->job[b->slot].far_until > d->now; }

int impai_far_px(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int pcx, int pcz)
{
    return impai_far(d, b) && out_of_reach(d, b, type, pcx, pcz);
}

int impai_far_at(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz)
{
    return impai_far(d, b) && site_out_of_reach(d, b, type, cx, cz);
}

/* At most job_checks, in slot order. An idle builder's BUILD is done when a building of the type ordered
   covers the site; else the builder could not reach the site (THE REACH ABANDON), which holds it back (THE FAR
   RECORD), or the site
   failed. A busy one older than job_timeout with no frame of it at the site is stopped (one STOP a decision:
   resolver mode 10, then the order) and the site failed. An EXPLORE ends when its cell is explored, or after
   job_timeout with the cell failed. */
static void job_checks(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i, checks = 0;
    for (i = 0; i < d->njobbed && checks < in->lv->job_checks; i++) {
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
            } else if (age > in->lv->job_timeout) {
                checks++;
                if (!built_at(d, j->build, j->cx, j->cz, 1) && !d->stopped && d->roots < in->lv->roots) {
                    WQ_CMD c;
                    const int a = d->q->ops->can(d->q, b->slot, 10, (int)WQ_NONE, b->pos);
                    d->stopped = 1;
                    if (a > 0) {
                        memset(&c, 0, sizeof c);
                        c.verb = WQ_CMD_NEW;
                        c.action = (uint8_t)a;
                        c.unit = b->slot;
                        c.target = (uint16_t)WQ_NONE;
                        memcpy(c.pos, b->pos, sizeof c.pos);
                        d->roots++;
                        d->q->ops->order(d->q, &c);
                    }
                    build_failed(d, j);
                    impai_job_end(j);
                }
            }
        } else if (j->kind == IMPAI_JOB_EXPLORE) {
            checks++;
            if (d->q->ops->explored(d->q, j->cx, j->cz)) impai_job_end(j);
            else if (age > in->lv->job_timeout) {
                impai_fail_add(d, j->cx, j->cz);
                impai_job_end(j);
            }
        }
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
static void economy(IMPAI_DEC* d)
{
    const IMPAI_LEVEL* lv = d->in->lv;
    const IMPAI_MIL* mil = d->in->mil;
    const WQ_ECON* e = &d->e;
    const float fac = (float)d->count[IMPAI_C_FACTORY];
    const int mex_done = d->done[IMPAI_C_MEX] > 0 ? d->done[IMPAI_C_MEX] : 1;
    const int judged = !mil || mil->naval >= 0;
    d->e_stall = e->energy < 0.2f * e->energy_max && e->energy_in < e->energy_out;
    d->m_stall = e->metal < 0.2f * e->metal_max && e->metal_in < e->metal_out;
    d->pe_in = e->energy_in + d->pe_extra;
    d->pm_in = e->metal_in * (1.0f + (float)d->pending[IMPAI_C_MEX] / (float)mex_done);
    d->want_factory = !d->pending[IMPAI_C_FACTORY] &&
        ((d->count[IMPAI_C_FACTORY] == 0 &&
          ((d->done[IMPAI_C_MEX] >= lv->open_mex && d->count[IMPAI_C_ENERGY] >= lv->open_energy) || d->now >= lv->open_t)) ||
         (d->count[IMPAI_C_FACTORY] >= 1 && d->count[IMPAI_C_FACTORY] < lv->factory_max && judged &&
          e->metal_in >= d->factory_per_m * fac && !d->e_stall && !d->m_stall));
    d->want_sea = mil && mil->naval == 1 && !d->sea_plants && d->count[IMPAI_C_FACTORY] >= 1 &&
                  d->count[IMPAI_C_FACTORY] < lv->factory_max && !d->pending[IMPAI_C_FACTORY] &&
                  e->metal_in >= d->factory_per_m * fac && !d->e_stall && !d->m_stall &&
                  d->in->st->sea_refused < IMPAI_SEA_REFUSALS && impai_econ_sea_able(d);
    /* TECH-UP: with factory_max factories of its highest offer (IMPAI_TINFO.offer: the tech its plants make, frames
       included) and a metal income of tech_m a pass -- the table's tech_m, Hard's, times the level's tech_scale --
       the next factory offers a higher tech (impai_econ.c, choose_factory) */
    if (d->in->mil) {
        const IMPAI_MIL* m = d->in->mil;
        const float tech_m = m->table.tech_m * lv->tech_scale;
        d->want_tech = !d->pending[IMPAI_C_FACTORY] && tech_m > 0 && e->metal_in >= tech_m &&
                       d->ftop_n >= lv->factory_max && !d->e_stall && !d->m_stall;
        /* ANTI-AIR STRUCTURES: one per aa_struct_v of seen enemy air value, at most aa_struct_max */
        d->want_aa = lv->aa_struct_v > 0 && d->aa_n < lv->aa_struct_max &&
                     m->air_seen >= lv->aa_struct_v * (float)(d->aa_n + 1);
    }
}

/* ---- the entry ---- */

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
    if (!q || !st || !in->job || !in->ti || !in->lv || !in->snap || !in->scratch ||
        in->scratch_bytes < impai_brain_scratch_bytes() || (int)in->nti != q->ops->ndefs(q) ||
        (int)in->njob < q->ops->nslots(q) || (in->nspots && !in->spots) || in->lv->idle_max > IMPAI_IDLE_CAP ||
        in->lv->roots > IMPAI_ROOTS_MAX)
        return -1;
    memset(out, 0, sizeof *out);
    d = carve(in);
    d->in = in;
    d->out = out;
    d->q = q;
    d->now = (uint32_t)q->ops->gametime(q);
    d->sea_type = (uint16_t)WQ_NONE;
    q->ops->map(q, &d->m);
    if (q->ops->economy(q, &d->e) != WQ_OK) memset(&d->e, 0, sizeof d->e);
    d->act_build = q->ops->action(q, WQ_ACT_MOBILEBUILD);
    d->act_vtol_build = q->ops->action(q, WQ_ACT_VTOL_MOBILEBUILD);
    d->act_queue = q->ops->action(q, WQ_ACT_BUILDINGBUILD);
    d->act_park = q->ops->action(q, WQ_ACT_PARK);
    /* THE ECONOMY'S NOISE (impai_noise, tag 2): each ratio times 1 + noise x a draw of [-1, 1); at noise 0
       (Hard) the factor is exactly 1, so Hard's economy is A1b's */
    d->e_per_m = in->lv->e_per_m;
    d->factory_per_m = in->lv->factory_per_m;
    d->cons_per_m = in->lv->cons_per_m;
    if (in->mil && in->lv->noise > 0.0f) {
        d->e_per_m *= 1.0f + in->lv->noise * impai_noise(in->mil->seed, d->now, 2, 0);
        d->factory_per_m *= 1.0f + in->lv->noise * impai_noise(in->mil->seed, d->now, 2, 1);
        d->cons_per_m *= 1.0f + in->lv->noise * impai_noise(in->mil->seed, d->now, 2, 2);
    }
    if (st->explorer != WQ_NONE && st->explorer >= in->njob) st->explorer = (uint16_t)WQ_NONE;
    out->census = (uint16_t)census(d);
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
    job_checks(d);
    economy(d);
    impai_econ_assign(d);
    impai_econ_factories(d);
    out->sites = (uint16_t)d->sites;
    out->snaps = (uint16_t)d->snaps;
    out->roots = (uint16_t)d->roots;
    out->queued = (uint16_t)d->queued;
    return 0;
}
