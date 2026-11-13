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
SIM_FEATURE(impai_mil_decide, SIM_PUPPET_OWNER, "impai-fight stock-impai-fight impai-eco impai-observe stock-impai-observe");

#define REACH_PAD    384        /* a squad engages a contact within its longest range plus this              */
#define BASE_PAD     512        /* a contact this far past base_r from the anchor, or from an own structure,
                                   threatens the base                                                       */
#define NEAR_DEF_PX  512        /* enemy defences this near a contact or a target join its side             */
#define TARGET_PX    1024       /* a wave's target: the enemy structure of most value this near the estimate */
#define ARRIVE_PX    256        /* a point is reached within this                                           */
#define RALLY_PX     512        /* a retreat ends this near the rally point                                 */
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

typedef struct MUNIT {          /* an army unit of the census                                              */
    uint16_t slot, type;
    uint8_t  sq, layer, klass, fresh;   /* its squad, IMPAI_SQUADS for none yet; TASIM_LAYER; IMPAI_K_*; 1 when
                                           it joined a squad this decision                                  */
    int32_t  x, z, hp;
    uint32_t hp_max;
    float    value;
    int32_t  range, speed;
} MUNIT;

typedef struct MENEMY {         /* an enemy row of the seat's column                                       */
    uint16_t slot, type;        /* type WQ_NONE: a radar contact never identified                           */
    int32_t  x, z;              /* its block's centre, px                                                   */
    float    value;
    uint8_t  now, mobile, armed, contact;   /* seen now (own sight, shared, or the radar pass); its contact + 1 */
    int16_t  cnext, next;       /* the next enemy of its 256-px cell; of its contact                        */
} MENEMY;

typedef struct MCONTACT {
    int64_t  sx, sz;
    int32_t  x, z;
    uint32_t near_anchor;       /* its nearest member's distance from the anchor                            */
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

typedef struct MDEC {
    const IMPAI_MIL_IN* in;
    IMPAI_MIL_OUT*      out;
    const WORLDQ*       q;
    IMPAI_MIL*          st;
    const IMPAI_LEVEL*  lv;
    uint32_t            now;
    int32_t             ax, az, rx, rz;     /* the anchor and the rally point, px                           */
    int                 gw, gh;             /* the 256-px grid                                              */
    MUNIT               army[IMPAI_ARMY_MAX];
    int                 narmy;
    MSQ                 sq[IMPAI_SQUADS];
    int                 com;                /* the commander's census entry: slot, else 0                   */
    uint16_t            com_type;
    int32_t             com_x, com_z;
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
    float               v;                  /* the median army unit's value                                 */
    int                 act_move;
    /* scratch beside the struct */
    int16_t*            cellhead;
    uint8_t*            label;
    uint8_t*            ownnear;
    int16_t*            cells;
    IMPAI_MAPS          maps;
    int                 maps_built;
} MDEC;

#define ALIGN16(n)  (((n) + 15u) & ~(size_t)15u)
#define GRID_CELLS  ((size_t)IMPAI_GRID_MAX * IMPAI_GRID_MAX)
#define GRID_BYTES  (ALIGN16(GRID_CELLS * sizeof(int16_t)) + 2 * ALIGN16(GRID_CELLS) + \
                     ALIGN16(IMPAI_ENEMY_MAX * sizeof(int16_t)))
#define MAPS_BYTES  (IMPAI_THREATS * ALIGN16(GRID_CELLS * sizeof(uint32_t)) + ALIGN16(GRID_CELLS * sizeof(int32_t)) + \
                     ALIGN16(GRID_CELLS * sizeof(uint16_t)) + \
                     ALIGN16((IMPAI_GRID_MAX + 1) * (size_t)IMPAI_GRID_MAX * sizeof(int32_t)))
_Static_assert(ALIGN16(sizeof(MDEC)) + GRID_BYTES + MAPS_BYTES <= IMPAI_MIL_SCRATCH_MAX,
               "a military decision's scratch fits IMPAI_MIL_SCRATCH_MAX");

size_t impai_mil_scratch_bytes(void) { return ALIGN16(sizeof(MDEC)) + GRID_BYTES + MAPS_BYTES; }

int impai_mil_due(unsigned gametime, unsigned seat, const IMPAI_LEVEL* lv)
{
    return lv->mil_period && gametime % lv->mil_period == (7u * seat + 8u) % 15u;
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

/* a squad becomes `task`, from now */
static void task(MDEC* d, int s, unsigned task_, int32_t x, int32_t z, uint16_t target)
{
    IMPAI_SQUAD* q = &d->st->sq[s];
    q->task = (uint8_t)task_;
    q->tx = (int16_t)(x >> 5);
    q->tz = (int16_t)(z >> 5);
    q->target = target;
    q->seen = 0;
    q->flags = 0;
    q->since = d->now;
}

/* squad `from`'s members into its layer's FORM squad, or `from` becomes it when there is none */
static void back_to_form(MDEC* d, int from)
{
    int k, to = -1;
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (k != from && d->st->sq[k].task == IMPAI_SQ_FORM && d->st->sq[k].layer == d->st->sq[from].layer) to = k;
    if (to < 0) {
        task(d, from, IMPAI_SQ_FORM, d->rx, d->rz, (uint16_t)WQ_NONE);
        return;
    }
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq == from) join(d, &d->army[k], to);
    d->sq[to].n = (uint16_t)(d->sq[to].n + d->sq[from].n);
    d->sq[to].value += d->sq[from].value;
    d->sq[from].n = 0;
    memset(&d->st->sq[from], 0, sizeof d->st->sq[from]);
    d->st->sq[from].target = (uint16_t)WQ_NONE;
}

/* ---- 1. the army's census ---- */

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
        }
        if (t->klass >= IMPAI_KS) continue;
        /* A recycled slot: a job of another type is voided, and a squad index past the table is a bound on
           a recorded byte, voided too */
        if (j->type != WQ_NONE && j->type != u.type) job_clear(j);
        if (j->kind == IMPAI_JOB_SQUAD && j->squad >= IMPAI_SQUADS) job_clear(j);
        if (d->narmy >= IMPAI_ARMY_MAX) {
            d->out->left_out++;
            continue;
        }
        speed = type_speed(d, u.type);
        m = &d->army[d->narmy++];
        memset(m, 0, sizeof *m);
        m->slot = (uint16_t)slot;
        m->type = u.type;
        m->klass = t->klass;
        m->layer = layer_of_class(t->klass);
        m->x = u.pos[0] >> 16;
        m->z = u.pos[2] >> 16;
        m->hp = u.hp;
        m->hp_max = u.hp_max;
        m->value = t->value;
        m->range = in->view && in->view->threat && u.type < (unsigned)in->view->ntypes
                       ? in->view->threat[u.type].range : 0;
        m->speed = speed;
        m->sq = j->kind == IMPAI_JOB_SQUAD && d->st->sq[j->squad].task != IMPAI_SQ_FREE ? j->squad : IMPAI_SQUADS;
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
                d->out->left_out++;
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
        if (!ident && !now) continue;
        if (ident && !now && (in->ti[r->type].flags & IMPAI_TF_MOBILE)) continue;
        if (d->nen >= IMPAI_ENEMY_MAX) {
            d->out->left_out++;
            continue;
        }
        e = &d->en[d->nen++];
        memset(e, 0, sizeof *e);
        e->slot = (uint16_t)i;
        e->type = ident ? r->type : (uint16_t)WQ_NONE;
        e->x = r->bx * 32 + 16;
        e->z = r->bz * 32 + 16;
        e->now = (uint8_t)now;
        e->mobile = (uint8_t)(!ident || (in->ti[r->type].flags & IMPAI_TF_MOBILE));
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
                for (e = d->cellhead[d->cells[k]]; e >= 0; e = d->en[e].cnext) d->out->left_out++;
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

static void base_threats(MDEC* d)
{
    const int span = BASE_PAD / IMPAI_CELL_PX;
    int k;
    memset(d->ownnear, 0, (size_t)d->gw * (size_t)d->gh);
    for (k = 0; k < d->nown; k++) {
        const int cx = d->own[k].x / IMPAI_CELL_PX, cz = d->own[k].z / IMPAI_CELL_PX;
        int dx, dz;
        for (dz = -span; dz <= span; dz++)
            for (dx = -span; dx <= span; dx++)
                if (cx + dx >= 0 && cz + dz >= 0 && cx + dx < d->gw && cz + dz < d->gh)
                    d->ownnear[(cz + dz) * d->gw + cx + dx] = 1;
    }
    for (k = 0; k < d->nct; k++) {
        MCONTACT* c = &d->ct[k];
        int16_t e;
        if (c->near_anchor <= (uint32_t)d->lv->base_r + BASE_PAD) c->threat = 1;
        for (e = c->first; e >= 0 && !c->threat; e = d->en[e].next) {
            int cx = d->en[e].x / IMPAI_CELL_PX, cz = d->en[e].z / IMPAI_CELL_PX;
            if (cx >= d->gw) cx = d->gw - 1;
            if (cz >= d->gh) cz = d->gh - 1;
            if (d->ownnear[cz * d->gw + cx]) c->threat = 1;
        }
        d->out->threats = (uint16_t)(d->out->threats + c->threat);
    }
}

/* ---- the predictor ---- */

typedef struct SIDEIN { IMPAI_FENTRY e[SIDE_IN]; unsigned n; } SIDEIN;

static void side_add(SIDEIN* s, uint16_t type, uint16_t hp_frac)
{
    unsigned k;
    if (type == WQ_NONE) return;
    for (k = 0; k < s->n; k++)
        if (s->e[k].type == type) {
            s->e[k].hp_frac = (uint16_t)(((uint32_t)s->e[k].hp_frac * s->e[k].n + hp_frac) / (s->e[k].n + 1u));
            s->e[k].n++;
            return;
        }
    if (s->n >= SIDE_IN) return;
    s->e[s->n].type = type;
    s->e[s->n].n = 1;
    s->e[s->n].hp_frac = hp_frac;
    s->n++;
}

static uint16_t hp_frac(const MUNIT* u)
{
    if (!u->hp_max || u->hp <= 0) return u->hp_max ? 0 : 65535;
    return (uint32_t)u->hp >= u->hp_max ? 65535 : (uint16_t)((uint64_t)u->hp * 65535u / u->hp_max);
}

/* the armed enemy structures within NEAR_DEF_PX of (x, z), seen or remembered */
static void side_defences(const MDEC* d, SIDEIN* s, int32_t x, int32_t z)
{
    int k;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* e = &d->en[k];
        if (!e->mobile && e->armed && dist(e->x, e->z, x, z) <= NEAR_DEF_PX) side_add(s, e->type, 65535);
    }
}

/* m, the predicted margin of side a against side b at `dist_px`, with the level's noise (tag 1, i): 1 when b is
   empty, -1 when a is; 0 for a prediction made, -1 when the predictor refused or the decision has spent its
   predictions, with *m untouched */
static int predict(MDEC* d, const SIDEIN* a, const SIDEIN* b, int32_t dist_px, unsigned i, float* m)
{
    IMPAI_FSIDE sa, sb;
    IMPAI_FRESULT r;
    if (!b->n) { *m = 1.0f; return 0; }
    if (!a->n) { *m = -1.0f; return 0; }
    if (d->predictions >= d->lv->predict_max || d->predictions >= IMPAI_PREDICT_CAP || !d->in->fc || !d->in->fit)
        return -1;
    d->predictions++;
    d->out->predictions++;
    if (impai_fight_side(d->in->fc, a->e, a->n, 1, &sa) < 0 || impai_fight_side(d->in->fc, b->e, b->n, 1, &sb) < 0 ||
        impai_fight_predict(d->in->fc, d->in->fit, &sa, &sb, dist_px, &r) < 0) {
        d->out->refused_predictions++;
        return -1;
    }
    *m = r.m + (d->lv->noise > 0.0f ? d->lv->noise * impai_noise(d->st->seed, d->now, 1, i) : 0.0f);
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

/* the distance from a squad's centroid to a contact's nearest member */
static uint32_t to_contact(const MDEC* d, int s, int c)
{
    uint32_t best = 0xFFFFFFFFu;
    int16_t e;
    for (e = d->ct[c].first; e >= 0; e = d->en[e].next) {
        const uint32_t x = dist(d->sq[s].x, d->sq[s].z, d->en[e].x, d->en[e].z);
        if (x < best) best = x;
    }
    return best;
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

static void form_attack(MDEC* d, int s)
{
    IMPAI_SQUAD* q = &d->st->sq[s];
    SIDEIN a, b;
    float m;
    int k, t;
    int32_t tx, tz;
    if (d->st->ebx == IMPAI_NO_ANCHOR || d->v <= 0.0f ||
        d->sq[s].value < (d->lv->wave_value + (float)d->st->waves) * d->v)
        return;
    t = wave_target(d);
    tx = t >= 0 ? d->en[t].x : d->st->ebx * 32 + 16;
    tz = t >= 0 ? d->en[t].z : d->st->ebz * 32 + 16;
    side_squad(d, s, &a);
    memset(&b, 0, sizeof b);
    for (k = 0; k < d->nen; k++)
        if (d->en[k].now && d->en[k].mobile) side_add(&b, d->en[k].type != WQ_NONE ? d->en[k].type : d->sub, 65535);
    side_defences(d, &b, tx, tz);
    if (predict(d, &a, &b, (int32_t)dist(d->sq[s].x, d->sq[s].z, tx, tz), (unsigned)s, &m) < 0 ||
        !(m > d->lv->attack_margin))
        return;
    task(d, s, IMPAI_SQ_ATTACK, tx, tz, t >= 0 ? d->en[t].slot : (uint16_t)WQ_NONE);
    q->value = d->sq[s].value;
    d->st->waves++;
    d->st->wave_t = d->now;
    d->out->attacks++;
}

/* RAIDS: while fewer than the level's raid squads go, up to RAID_MAX units of the fastest type of a land FORM
   squad but its scouts, at least RAID_MIN, go for the seen or remembered enemy extractor or energy building of
   least threat on its cell of the ground threat map (ties to the nearer, then the lower slot) */
static void raid(MDEC* d)
{
    int k, raids = 0, form = -1, best = -1, s, n = 0;
    uint32_t best_threat = 0;
    uint16_t fast = 0;
    int32_t fast_speed = 0;
    for (k = 0; k < IMPAI_SQUADS; k++) {
        raids += d->st->sq[k].task == IMPAI_SQ_RAID;
        if (d->st->sq[k].task == IMPAI_SQ_FORM && d->st->sq[k].layer == TASIM_LAYER_LAND) form = k;
    }
    if (raids >= d->lv->raids || form < 0 || !d->in->view || !d->in->view->threat) return;
    for (k = 0; k < d->narmy; k++)
        if (d->army[k].sq == form && d->army[k].klass != IMPAI_K_LSCOUT && d->army[k].speed > fast_speed) {
            fast_speed = d->army[k].speed;
            fast = d->army[k].type;
        }
    for (k = 0; k < d->narmy; k++) n += d->army[k].sq == form && d->army[k].type == fast;
    if (n < RAID_MIN) return;
    if (!d->maps_built) d->maps_built = impai_maps_build(d->in->view, &d->maps) ? 1 : -1;
    if (d->maps_built < 0) return;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* e = &d->en[k];
        int cx, cz;
        uint32_t th;
        if (e->mobile || e->type == WQ_NONE) continue;
        if (d->in->ti[e->type].cat != IMPAI_C_MEX && d->in->ti[e->type].cat != IMPAI_C_ENERGY &&
            d->in->ti[e->type].cat != IMPAI_C_GEO)
            continue;
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
    task(d, s, IMPAI_SQ_RAID, d->en[best].x, d->en[best].z, d->en[best].slot);
    d->st->sq[s].layer = TASIM_LAYER_LAND;
    for (k = 0, n = 0; k < d->narmy && n < RAID_MAX; k++)
        if (d->army[k].sq == form && d->army[k].type == fast) {
            d->sq[form].n--;
            d->sq[form].value -= d->army[k].value;
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

/* The next scouting point: start positions round the list from scout_next, the seat's own skipped. BOUND:
   scout_next is recorded state, taken modulo the count before it indexes the list. */
static int next_point(MDEC* d)
{
    int k = d->st->scout_next % d->nstarts;
    if (k == d->own_start) k = (k + 1) % d->nstarts;
    d->st->scout_next = (uint8_t)((k + 1) % d->nstarts);
    return k;
}

/* SCOUTING: every scout_period, or while the estimate rests on no enemy structure, and with no scout out: the
   dearest scout-class unit of a FORM squad, else its cheapest unit among the fastest quarter of speeds, goes as a
   squad of one to the start positions other than the seat's own, from scout_next on, round the list: the one
   it visited least recently first, by construction. It ends at the third point or at a contact. */
static void scouting(MDEC* d)
{
    int k, s, pick = -1, fastest = 0;
    for (k = 0; k < IMPAI_SQUADS; k++) if (d->st->sq[k].task == IMPAI_SQ_SCOUT) return;
    if (d->nstarts < 2 || (d->st->scout_t && d->now - d->st->scout_t < d->lv->scout_period && d->est_struct)) return;
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
    if (s == IMPAI_SQUADS) return;
    k = next_point(d);
    {
        MUNIT* u = &d->army[pick];
        const int from = u->sq;
        d->sq[from].n--;
        d->sq[from].value -= u->value;
        task(d, s, IMPAI_SQ_SCOUT, d->starts[k].x, d->starts[k].z, (uint16_t)WQ_NONE);
        d->st->sq[s].layer = TASIM_LAYER_LAND;
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
    int k;
    if (dist(d->sq[s].x, d->sq[s].z, q->tx * 32 + 16, q->tz * 32 + 16) > ARRIVE_PX) return;
    if (++q->flags >= SCOUT_LEGS || d->nstarts < 2) {
        back_to_form(d, s);
        return;
    }
    k = next_point(d);
    q->tx = (int16_t)(d->starts[k].x >> 5);
    q->tz = (int16_t)(d->starts[k].z >> 5);
}

/* Each squad, round-robin from next_squad: a contact within its reach is predicted -- below retreat_margin it
   falls back to the rally point, otherwise it fights -- and a squad with none follows its task. Every prediction
   comes out of the level's predict_max; a squad left without one keeps its task. */
static void tasks(MDEC* d)
{
    const IMPAI_LEVEL* lv = d->lv;
    int i;
    for (i = 0; i < IMPAI_SQUADS; i++) {
        const int s = (d->st->next_squad + i) % IMPAI_SQUADS;
        IMPAI_SQUAD* q = &d->st->sq[s];
        int c, best = -1;
        uint32_t bd = 0;
        if (q->task == IMPAI_SQ_FREE || !d->sq[s].n) continue;
        for (c = 0; c < d->nct; c++) {
            const uint32_t x = to_contact(d, s, c);
            if (x <= (uint32_t)d->sq[s].range + REACH_PAD && (best < 0 || x < bd)) { best = c; bd = x; }
        }
        if (q->task == IMPAI_SQ_SCOUT) {
            if (best >= 0) back_to_form(d, s);
            else scout_step(d, s);
            continue;
        }
        if (q->task == IMPAI_SQ_RETREAT) {
            if (dist(d->sq[s].x, d->sq[s].z, d->rx, d->rz) <= RALLY_PX) back_to_form(d, s);
            continue;
        }
        if (best >= 0) {
            SIDEIN a, b;
            float m;
            side_squad(d, s, &a);
            side_contact(d, best, &b);
            if (predict(d, &a, &b, (int32_t)bd, (unsigned)s, &m) == 0 && m < lv->retreat_margin) {
                task(d, s, IMPAI_SQ_RETREAT, d->rx, d->rz, (uint16_t)WQ_NONE);
                d->out->retreats++;
                continue;
            }
            d->sq[s].contact = best;
            continue;
        }
        switch (q->task) {
        case IMPAI_SQ_FORM:
            form_attack(d, s);
            break;
        case IMPAI_SQ_ATTACK:
        case IMPAI_SQ_RAID:
            if (d->now - q->since > lv->task_timeout ||
                (q->target != WQ_NONE && !target_known(d, q->target)) ||
                (q->target == WQ_NONE && dist(d->sq[s].x, d->sq[s].z, q->tx * 32 + 16, q->tz * 32 + 16) <= ARRIVE_PX))
                back_to_form(d, s);
            break;
        case IMPAI_SQ_DEFEND:
            break;
        default:
            break;
        }
    }
}

/* THE BASE'S DEFENCE: a threat to the base that has persisted react_confirm decisions takes each layer's FORM
   squad and every DEFEND squad to the threat nearest the anchor; with no threat left a DEFEND squad goes back
   to FORM. */
static void defence(MDEC* d)
{
    int k, c, near = -1;
    for (c = 0; c < d->nct; c++)
        if (d->ct[c].threat && (near < 0 || d->ct[c].near_anchor < d->ct[near].near_anchor)) near = c;
    for (k = 0; k < IMPAI_SQUADS; k++) {
        IMPAI_SQUAD* q = &d->st->sq[k];
        if (q->task != IMPAI_SQ_FORM && q->task != IMPAI_SQ_DEFEND) continue;
        if (near < 0) {
            q->seen = 0;
            if (q->task == IMPAI_SQ_DEFEND) back_to_form(d, k);
            continue;
        }
        if (q->seen < 0xFFFF) q->seen++;
        if (q->seen < d->lv->react_confirm) continue;
        if (q->task == IMPAI_SQ_FORM) {
            const uint16_t seen = q->seen;
            task(d, k, IMPAI_SQ_DEFEND, d->ct[near].x, d->ct[near].z, (uint16_t)WQ_NONE);
            q->seen = seen;
            d->out->defends++;
        } else {
            q->tx = (int16_t)(d->ct[near].x >> 5);
            q->tz = (int16_t)(d->ct[near].z >> 5);
        }
    }
}

/* ---- 5. the orders ---- */

/* One order of resolver mode `mode` to army unit u, at target slot `target` or point (x, z): none when the
   decision's orders are spent, when the resolver answers 0 or -1 (counted), or when the unit's head already
   says it -- the same action and target, or a move within HEAD_NEAR_PX of the point. No node pointer is kept:
   the head is a copy. 1 when an order was given. */
static int order(MDEC* d, uint16_t slot, int mode, uint16_t target, int32_t x, int32_t z)
{
    WQ_ORDER o;
    WQ_CMD c;
    int a, h;
    if (d->roots >= d->lv->mil_roots) return 0;
    memset(&c, 0, sizeof c);
    c.pos[0] = (int32_t)((uint32_t)x << 16);
    c.pos[2] = (int32_t)((uint32_t)z << 16);
    c.pos[1] = (int32_t)((uint32_t)(d->q->ops->ground(d->q, c.pos) & 0x7FFF) << 16);
    if ((a = d->q->ops->can(d->q, slot, mode, target, c.pos)) <= 0) {
        d->out->refused++;
        return 0;
    }
    h = d->q->ops->head(d->q, slot, &o);
    if (h == 1 && o.action == a &&
        (mode == WQ_CAN_ATTACK || mode == WQ_CAN_DGUN
             ? o.target == target : dist(o.pos[0] >> 16, o.pos[2] >> 16, x, z) <= HEAD_NEAR_PX))
        return 0;
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)a;
    c.unit = slot;
    c.target = target;
    d->roots++;
    d->st->pulse_roots++;
    if (d->q->ops->order(d->q, &c) != WQ_OK) d->out->refused++;
    return 1;
}

/* the point of the k-th member of a squad travelling to (x, z): a 5 x 5 grid SPREAD_PX apart */
static void spread(int k, int32_t* x, int32_t* z)
{
    *x += ((k % 5) - 2) * SPREAD_PX;
    *z += (((k / 5) % 5) - 2) * SPREAD_PX;
}

/* A fighting squad's orders. The candidates are the TARGETS seen-now members of its contact nearest its
   centroid, armed or mobile ones before the rest. With focus, every member attacks the candidate of least
   hp / sum of the squad's pair rates on it -- the quickest kill, over FOCUS_TYPES types, by the type's maximum
   health, which is all the seat knows of an enemy's -- where it reaches it, else its own nearest candidate;
   without, each its nearest. Micro: a member under micro_hp of its health steps MICRO_PX towards the rally
   point, once in micro_period, and is given nothing between two steps. Only a unit the seat sees now is ever
   ordered at. */
static void fight(MDEC* d, int s)
{
    const MCONTACT* ct = &d->ct[d->sq[s].contact];
    int16_t cand[TARGETS];
    uint32_t cd[TARGETS];
    int nc = 0, k, focus = -1;
    int16_t e;
    for (e = ct->first; e >= 0; e = d->en[e].next) {
        const MENEMY* m = &d->en[e];
        const uint32_t x = dist(m->x, m->z, d->sq[s].x, d->sq[s].z) + (m->armed || m->mobile ? 0u : 4096u);
        int i;
        if (!m->now) continue;
        if (nc == TARGETS && x >= cd[nc - 1]) continue;
        i = nc < TARGETS ? nc++ : nc - 1;
        while (i > 0 && cd[i - 1] > x) { cand[i] = cand[i - 1]; cd[i] = cd[i - 1]; i--; }
        cand[i] = e;
        cd[i] = x;
    }
    if (!nc) return;
    if (d->lv->focus && d->in->fc && d->in->fc->nt) {
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
                }
            }
        }
    }
    for (k = 0; k < d->narmy && d->roots < d->lv->mil_roots; k++) {
        MUNIT* u = &d->army[k];
        IMPAI_JOB* j = &d->in->job[u->slot];
        int target = focus, i;
        if (u->sq != s) continue;
        if (d->lv->micro_hp > 0.0f && u->hp_max && (float)u->hp < d->lv->micro_hp * (float)u->hp_max) {
            /* a damaged member steps back, and is left on its way between two steps */
            if (!j->since || d->now - j->since >= d->lv->micro_period) {
                const uint32_t dr = dist(u->x, u->z, d->rx, d->rz);
                int32_t x = d->rx, z = d->rz;
                if (dr > MICRO_PX) {
                    x = u->x + (int32_t)((int64_t)(d->rx - u->x) * MICRO_PX / dr);
                    z = u->z + (int32_t)((int64_t)(d->rz - u->z) * MICRO_PX / dr);
                }
                if (order(d, u->slot, WQ_CAN_MOVE, (uint16_t)WQ_NONE, x, z)) j->since = d->now;
            }
            continue;
        }
        if (target < 0 || dist(u->x, u->z, d->en[target].x, d->en[target].z) > (uint32_t)u->range + REACH_PAD) {
            uint32_t bd = 0;
            target = -1;
            for (i = 0; i < nc; i++) {
                const uint32_t x = dist(u->x, u->z, d->en[cand[i]].x, d->en[cand[i]].z);
                if (target < 0 || x < bd) { target = cand[i]; bd = x; }
            }
        }
        order(d, u->slot, WQ_CAN_ATTACK, d->en[target].slot, d->en[target].x, d->en[target].z);
    }
}

static void travel(MDEC* d, int s, int32_t x, int32_t z)
{
    int k, n = 0;
    for (k = 0; k < d->narmy && d->roots < d->lv->mil_roots; k++) {
        int32_t px = x, pz = z;
        if (d->army[k].sq != s) continue;
        spread(n++, &px, &pz);
        order(d, d->army[k].slot, WQ_CAN_MOVE, (uint16_t)WQ_NONE, px, pz);
    }
}

static void orders(MDEC* d)
{
    int i, start = d->st->next_squad % IMPAI_SQUADS, stop = -1;
    for (i = 0; i < IMPAI_SQUADS; i++) {
        const int s = (start + i) % IMPAI_SQUADS;
        const IMPAI_SQUAD* q = &d->st->sq[s];
        if (d->roots >= d->lv->mil_roots) { stop = s; break; }
        if (q->task == IMPAI_SQ_FREE || !d->sq[s].n) continue;
        if (d->sq[s].contact >= 0 && q->task != IMPAI_SQ_RETREAT && q->task != IMPAI_SQ_SCOUT) fight(d, s);
        else if (q->task == IMPAI_SQ_FORM || q->task == IMPAI_SQ_RETREAT) travel(d, s, d->rx, d->rz);
        else travel(d, s, q->tx * 32 + 16, q->tz * 32 + 16);
    }
    d->st->next_squad = (uint8_t)(stop >= 0 ? stop : (start + 1) % IMPAI_SQUADS);
}

/* ---- 7. the commander's safety ---- */

/* A contact worth more than the commander within IMPAI_CMD_SAFE_PX of it is predicted against the commander and
   the army within CMD_ARMY_PX of it; a margin below 0, or no prediction to be had, moves the commander to
   whichever of the rally point and the own structure farthest from the contact lies farther from it, and
   makes it the military's for SAFE_TICKS (IMPAI_MIL.safe_until, which the economy's census honours). Run
   before the squads' orders, so that the decision's order budget cannot starve it. */
static void commander(MDEC* d)
{
    const float cv = d->com ? d->in->ti[d->com_type].value : 0.0f;
    int c, worst = -1, k;
    if (!d->com) return;
    for (c = 0; c < d->nct; c++) {
        int16_t e;
        uint32_t near = 0xFFFFFFFFu;
        for (e = d->ct[c].first; e >= 0; e = d->en[e].next) {
            const uint32_t x = dist(d->en[e].x, d->en[e].z, d->com_x, d->com_z);
            if (x < near) near = x;
        }
        if (near <= IMPAI_CMD_SAFE_PX && d->ct[c].value > cv && (worst < 0 || d->ct[c].value > d->ct[worst].value))
            worst = c;
    }
    if (worst < 0) return;
    {
        SIDEIN a, b;
        float m = -1.0f;
        int32_t x = d->rx, z = d->rz;
        uint32_t far = dist(d->rx, d->rz, d->ct[worst].x, d->ct[worst].z);
        memset(&a, 0, sizeof a);
        side_add(&a, d->com_type, 65535);
        for (k = 0; k < d->narmy; k++)
            if (dist(d->army[k].x, d->army[k].z, d->com_x, d->com_z) <= CMD_ARMY_PX)
                side_add(&a, d->army[k].type, hp_frac(&d->army[k]));
        side_contact(d, worst, &b);
        if (predict(d, &a, &b, (int32_t)dist(d->com_x, d->com_z, d->ct[worst].x, d->ct[worst].z), IMPAI_SQUADS, &m) == 0
            && !(m < 0.0f))
            return;
        for (k = 0; k < d->nown; k++) {
            const uint32_t x2 = dist(d->own[k].x, d->own[k].z, d->ct[worst].x, d->ct[worst].z);
            if (x2 > far) { far = x2; x = d->own[k].x; z = d->own[k].z; }
        }
        order(d, (uint16_t)d->com, WQ_CAN_MOVE, (uint16_t)WQ_NONE, x, z);
        d->st->safe_until = d->now + SAFE_TICKS;
        d->out->safe++;
    }
}

/* ---- 7b. the D-gun ----

   THE D-GUN IS ORDERED, never automatic: an ImpAI seat's weapon scan runs with argument 0, which leaves
   CommandFire weapons alone (0x408A88..0x408A96), as a human seat's does. The commander D-guns one target when
   all three hold: the target is SEEN NOW (an identified enemy row with IMPAI_NOW or IMPAI_PASS -- never a ghost,
   never a radar contact never identified); the seat's stored energy is at least the shot's plus dgun_reserve of
   its energy storage; and the target is WORTH THE SHOT, its value at least the shot's energy in metal (energy /
   TASIM_ROLES_R). Of those within the D-gun's range of the commander -- the row's block centre plus the block's
   half diagonal, 24 px, inside the weapon's range -- the most valuable, ties to the lower slot. Not while the
   commander is the military's for its safety. BOUNDS: the range and the energy are the content's, held at the
   build; the economy's floats are compared so that a value not finite refuses the shot. */
static void dgun(MDEC* d)
{
    const IMPAI_TINFO* ct;
    WQ_ECON e;
    float need, worth;
    int k, best = -1;
    if (!d->com || d->com_type >= d->in->nti || d->now < d->st->safe_until) return;
    ct = &d->in->ti[d->com_type];
    if (!ct->dgun_range) return;
    if (d->q->ops->economy(d->q, &e) != WQ_OK) return;
    need = (float)ct->dgun_e + d->lv->dgun_reserve * e.energy_max;
    if (!(e.energy >= need)) return;
    worth = (float)ct->dgun_e / (float)TASIM_ROLES_R;
    for (k = 0; k < d->nen; k++) {
        const MENEMY* m = &d->en[k];
        if (!m->now || m->type == WQ_NONE || !(m->value >= worth)) continue;
        if (dist(m->x, m->z, d->com_x, d->com_z) + 24u > ct->dgun_range) continue;
        if (best < 0 || m->value > d->en[best].value ||
            (m->value == d->en[best].value && m->slot < d->en[best].slot))
            best = k;
    }
    if (best < 0) return;
    if (order(d, (uint16_t)d->com, WQ_CAN_DGUN, d->en[best].slot, d->en[best].x, d->en[best].z)) d->out->dgun = 1;
}

/* ---- the entry ----

   ONE MILITARY DECISION, in order: (1) the army's census over the seat's slot block; (2) the picture of the
   enemy over the seat's column of the knowledge rows, the base's estimate and the contacts; (3) the base's
   threats and its defence; (7) the commander's safety, then its D-gun; (4) the squads' tasks with their
   predictions, raids and scouting; (5) the orders; (8) the picture kept for the factories; (9) the pulse's
   counts.

   BOUNDS. Every slot taken from a table -- a job row, a squad's target, a knowledge row -- is below the
   table's count before it indexes it, and is re-read through the WORLDQ, which answers from the engine's
   unit array under the adapter's bounds (tagpu_worldq.c, ORDERS OUT): a recycled slot costs one wrong order
   or one wrong membership, the resolver answering for the unit as it is now. A squad index is below
   IMPAI_SQUADS, a type below nti before any table is read by it, and the predictor refuses a type past its
   content. NO CHEATING: a target is ordered only while the seat sees it now (IMPAI_NOW | IMPAI_PASS on its own
   row), and the picture reads the seat's column alone. COST: units at most nslots, listed at most
   IMPAI_ARMY_MAX; enemies IMPAI_ENEMY_MAX; contacts IMPAI_CONTACTS; squads IMPAI_SQUADS; predictions
   predict_max (<= IMPAI_PREDICT_CAP); orders mil_roots; the maps IMPAI_GRID_MAX^2 cells.

   THE TICK RULE (impai_mil_due) keeps this decision and the economy's on different ticks of the same seat. */
int impai_mil_decide(const IMPAI_MIL_IN* in, IMPAI_MIL_OUT* out)
{
    MDEC* d;
    const WORLDQ* q = in->q;
    WQ_MAP m;
    int k;
    if (!q || !in->st || !in->job || !in->ti || !in->lv || !in->brain || !in->scratch ||
        in->scratch_bytes < impai_mil_scratch_bytes() || (int)in->nti != q->ops->ndefs(q) ||
        (int)in->njob < q->ops->nslots(q))
        return -1;
    memset(out, 0, sizeof *out);
    d = carve(in);
    d->in = in;
    d->out = out;
    d->q = q;
    d->st = in->st;
    d->lv = in->lv;
    d->now = (uint32_t)q->ops->gametime(q);
    q->ops->map(q, &m);
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
    /* the rally point: the anchor moved rally_px towards the estimate */
    d->rx = d->ax;
    d->rz = d->az;
    if (d->st->ebx != IMPAI_NO_ANCHOR) {
        const int32_t ex = d->st->ebx * 32 + 16, ez = d->st->ebz * 32 + 16;
        const uint32_t de = dist(d->ax, d->az, ex, ez);
        if (de > d->lv->rally_px) {
            d->rx = d->ax + (int32_t)((int64_t)(ex - d->ax) * d->lv->rally_px / de);
            d->rz = d->az + (int32_t)((int64_t)(ez - d->az) * d->lv->rally_px / de);
        }
    }
    out->army = (uint16_t)d->narmy;
    out->enemies = (uint16_t)d->nen;
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
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (d->st->sq[k].task < IMPAI_SQ_TASKS) out->tasks[d->st->sq[k].task]++;
    return 0;
}
