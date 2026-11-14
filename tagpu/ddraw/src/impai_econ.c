/* impai_econ.c -- ImpAI's economy manager (impai.h, the brain): the ladder that gives each idle builder its
   task, the type it builds within a category, the plant the seat builds next (THE CHOICE), and the factories'
   orders (THE GOVERNOR, THE ARMY ORDERS). The sites and the orders are impai_build.c's. Engine-free, no statics. */
#include <float.h>
#include <string.h>
#include "impai_brain.h"
#include "tagpu_simreg.h"

/* The economy manager. OWNER: the brain calls it only inside a live ImpAI seat's turn, which plays no puppet
   seat (tagpu_impai.c, impai_play), so a puppet's orders come from the recording. */
SIM_FEATURE(impai_econ, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval impai-raid");

/* the type's own menu offers an extractor */
static int builds_mex(IMPAI_DEC* d, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, n = d->q->ops->build_options(d->q, type, d->menu2, WQ_TYPES_MAX);
    for (k = 0; k < n; k++) if (d->menu2[k] < in->nti && in->ti[d->menu2[k]].cat == IMPAI_C_MEX) return 1;
    return 0;
}

/* ---- the type within a category, from the builder's menu; ties go to the lower type number ---- */

/* The ring places energy and plants around the anchor, so a type placed on water only (IMPAI_TF_WATER) can stand
   there only when the anchor's ground lies below the sea; on a dry anchor every site test of it fails. */
static int anchor_wet(IMPAI_DEC* d)
{
    const int32_t apos[3] = { (int32_t)((uint32_t)(d->in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(d->in->st->az * 16 + 8) << 16) };
    return d->q->ops->ground(d->q, apos) < (int)d->m.sea;
}

/* energy: the most egain per value, a water-only type only on a wet anchor */
static int choose_energy(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int wet = anchor_wet(d);
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_ENERGY || in->ti[t].value <= 0) continue;
        ti = &in->ti[t];
        if ((ti->flags & IMPAI_TF_WATER) && !wet) continue;
        if (!best || ti->egain * in->ti[best].value > in->ti[best].egain * ti->value ||
            (ti->egain * in->ti[best].value == in->ti[best].egain * ti->value && t < best))
            best = t;
    }
    return best;
}

/* an extractor: the most mgain */
static int choose_mex(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_MEX) continue;
        if (!best || in->ti[t].mgain > in->ti[best].mgain || (in->ti[t].mgain == in->ti[best].mgain && t < best)) best = t;
    }
    return best;
}

/* a maker: the most mgain per egain (its energy cost), then the least value */
static int choose_maker(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        float lhs, rhs;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_MAKER || in->ti[t].egain <= 0) continue;
        ti = &in->ti[t];
        if (!best) { best = t; continue; }
        lhs = ti->mgain * in->ti[best].egain;
        rhs = in->ti[best].mgain * ti->egain;
        if (lhs > rhs || (lhs == rhs && (ti->value < in->ti[best].value || (ti->value == in->ti[best].value && t < best))))
            best = t;
    }
    return best;
}

/* ---- THE CHOICE: which plant the seat builds next ----

   THE REACH (reach_mask_of): bit c is set when movement class c reaches a plausible enemy base from the base.
   - THE BASE'S COMPONENT: the label most of the blocks within REACH_RINGS rings of the anchor's 32-px block carry
     (base_label). The anchor is where the commander stood, and the content map can hold a larger class's block
     there impassable (a feature, a slope) where the plants' yards around it are not, or a pocket beside it that
     the yards' ground does not join; a class with no label within the rings has no ground at the base, and does
     not reach. RESIDUAL: past BASE_LABELS distinct labels in the rings the later ones are not counted.
   - THE TARGETS: with enemy structures seen or remembered (IMPAI_MIL.est_seen), the military's estimate, their
     value-weighted centroid; before that, every start position but the seat's own (the one nearest the anchor):
     the military's estimate is then only the nearest other start, which on Trout Farm is cut off by water from
     the anchor while the enemy, at another start, was land-linked (MEASURED 2026-11-14, plantchoice r1 and r3's
     Trout Farm games, and governor play1's b5 naval note). The starts are public: the skirmish screen shows them.
     An empty start stays plausible: which starts the players hold is not on that screen. RESIDUAL: an ally's
     start is a target too -- the lobby shows the teams, but the WORLDQ has no read of a player's start index
     and alliance yet, which excluding them needs. With fewer than two starts, the estimate; with no estimate
     either, every bit is set.
   - A class reaches when one target has a block of the base's component within REACH_RINGS rings (a start's own
     block may be impassable to a large class as the anchor's may). Any, not all: the extra mobility of a class
     counts only where the map needs it, and before the enemy is known the map needs it only when no plausible
     enemy is reached otherwise -- "a hover only where the route to the enemy needs water" (owner, 2026-11-14).
     RESIDUAL: until a structure is seen, a seat whose real enemy sits on the one start cut off from it builds land
     plants; the first enemy structure seen moves the targets to it.
   - Every bit is set with no reach table, no military or no anchor, and a class with no labels (-1) keeps its bit.
   A type reaches when it is a VTOL, its class's bit is set, or its class is past the mask (no evidence). The reach
   only filters: a hover's mobility adds nothing where the land classes reach, and is what makes its plant score
   where they do not. A route much shorter over water is not counted (owner's decision,
   research/notes/impai/decisions.md). BOUND: once a decision, for each class with labels, one spiral at the
   anchor and one at each target, each at most (2 REACH_RINGS + 1)^2 label reads, at most PLANT_STARTS targets.

   THE SCORE of plant P at the tech level T = max(ftop, 1), at most IMPAI_QTECH -- the enemy taken at the seat's own
   tech --: S(P) = the highest value (IMPAI_TVALUE.q[T - 1], impai_tinfo.c THE VALUES) of a type on P's menu of a
   gated class (IMPAI_GATED: the land fighting three) with a value above 0 that reaches; IMPAI_Q_NONE when there is
   none or no values. A plant is scored by its best unit; THE ARMY ORDERS then hold it to the classes in which its
   unit is that good.

   THE CANDIDATES: the distinct factory types on the menus of the seat's finished builder types (IMPAI_DEC.btype, at
   most IMPAI_SEA_BUILDERS, in census order, each menu ascending), then on the menus of THE UNLOCKS, at most
   IMPAI_PLANT_CANDS of them examined, each a menu read of at most PLANT_MENU types and one more for each
   constructor on it, kept when
   - its menu holds a constructor whose own menu holds an extractor;
   - it is placed on dry ground, or the anchor is wet (a water-only plant fits no site of the ring otherwise);
   - its way out has not been refused too often (impai_build.c, THE PLANT'S FALLBACK), unless `waived`;
   - THE BUILD TECH (IMPAI_TINFO.ctech) fits the want: a tech-up takes only a plant above the seat's highest
     (IMPAI_DEC.ftop); a first or further plant only one at or below max(ftop, lo), lo the least among the
     candidates, so the economy's income gate stays the only way up and a mod whose cheapest plant already
     brings tech 2 still has a first plant.
   The seat's menus and not the idle builder's: a builder's own menu would hide the plant it cannot build (an ARMCV
   sees only ARMAVP and ARMHP). A builder whose menu holds the choice builds it; one whose menu does not passes the
   factory rule, and the plant waits for a builder that offers it.
   THE UNLOCKS: the constructor types the seat's factories make of which it has no finished unit (plant_unlocks,
   the census's IMPAI_DEC.cons_done, at most PLANT_VIA). A plant only such a constructor builds is a candidate like
   any other, and when it is chosen the governor's next constructor is that type, at a factory that makes it
   (impai_econ_factories), once a decision and while none is a frame; the plant then waits for it. WHY: CORE's
   opening plant is CORVP and its governor walks the cheapest-army plant first, so the seat made CORCVs only and
   CORCK's CORALAB (+0.517) was no candidate: 6 of 7 CORE seats teched up to CORAVP at -0.396 (MEASURED
   2026-11-14, plantchoice r1 and r3). The governor's rules still bound every constructor: an unlock changes
   which type, never how many. So the unlocks are candidates only while the constructors are below cons_max, the
   count gov_cons tests: at the cap no constructor can come, and the best of the seat's own plants is taken.
   RESIDUAL: below the cap the governor's other rules can still hold the unlock back -- the army's share and the
   pressure -- and the plant waits while they do.

   THE BEST: the highest score, then a plant the seat's own builders build before one an unlock waits for, then the
   least value, then the lower type. The first plant (no factory of the
   seat's yet, frames included) is the least value, the opening A1 benchmarked: its first purpose is the
   constructors (owner's decision, 2026-11-14). On a wet anchor the least value too: the score ranks land armies,
   and a ring around an anchor below the sea may hold no site for a land plant (no bar seat-game's first plant was
   a shipyard, MEASURED 2026-11-14, so no bar start is wet). With no values every score ties and the choice is the
   least value. The runner-up is kept for the plant line. A tech-up whose best candidate scores below 0 still goes
   ahead (owner's decision, 2026-11-14): it brings the tech-2 constructors and economy, and THE ARMY ORDERS keep a
   plant whose army is poor here to constructors.

   WHY, MEASURED 2026-11-14 over 377 bar seat-games: the least value with the offer -- the highest classifier tech
   of a plant's units, a graph distance -- took CORE's hover plant as the tech-up in 167 of 170 seats (CORHP 1 883
   against CORAVP 2 006 and CORALAB 2 032), whose constructor builds nothing above tech 1, and its offer 2 ended the
   tech-up for good: no CORE seat built a plant of tech 2. */
#define FACTORY_LOOK 8
#define PLANT_MENU   256
#define PLANT_VIA    8          /* THE UNLOCKS: constructor types a pass looks through                       */
#define PLANT_LOOK   (2 * IMPAI_PLANT_CANDS)  /* plant menus a pass of THE CHOICE reads               */

#define REACH_RINGS  8          /* 256 px round the anchor or a target                                       */
#define PLANT_STARTS 16

#define BASE_LABELS  8          /* distinct labels base_label counts; a ninth and later one is not counted    */

/* class c's base component: the label most blocks within REACH_RINGS rings of block (bx, bz) carry, ties to the
   one met first in ring order -- the yards stand round the anchor, and one passable pocket next to it must not
   speak for the ground they stand on; 0 none; -1 the level has no labels for c. BOUND: (2 REACH_RINGS + 1)^2
   label reads and BASE_LABELS counters */
static int base_label(const IMPAI_REACH* r, unsigned c, int bx, int bz)
{
    int lab[BASE_LABELS], cnt[BASE_LABELS], n = 0, ring, i, k, dx, dz, best = -1;
    if (impai_reach_at(r, c, bx, bz) < 0) return -1;
    for (ring = 0; ring <= REACH_RINGS; ring++)
        for (i = 0; impai_ring_at(ring, i, &dx, &dz); i++) {
            const int l = impai_reach_at(r, c, bx + dx, bz + dz);
            if (l <= 0) continue;
            for (k = 0; k < n && lab[k] != l; k++) {}
            if (k == n) {
                if (n == BASE_LABELS) continue;
                lab[n] = l;
                cnt[n++] = 0;
            }
            cnt[k]++;
        }
    for (k = 0; k < n; k++)
        if (best < 0 || cnt[k] > cnt[best]) best = k;
    return best < 0 ? 0 : lab[best];
}

/* THE TARGETS' 32-px blocks into tx/tz: 0 none */
static int reach_targets(const IMPAI_DEC* d, int* tx, int* tz)
{
    const IMPAI_MIL* mil = d->in->mil;
    WQ_START st[PLANT_STARTS];
    const int64_t ax = (int64_t)d->in->st->ax * 16 + 8, az = (int64_t)d->in->st->az * 16 + 8;
    int ns = 0, own = 0, k, n = 0;
    if (!mil->est_seen && (ns = d->q->ops->starts(d->q, st, PLANT_STARTS)) >= 2) {
        for (k = 1; k < ns; k++)
            if ((st[k].x - ax) * (st[k].x - ax) + (st[k].z - az) * (st[k].z - az) <
                (st[own].x - ax) * (st[own].x - ax) + (st[own].z - az) * (st[own].z - az))
                own = k;
        for (k = 0; k < ns; k++)
            if (k != own) {
                tx[n] = st[k].x >> 5;
                tz[n++] = st[k].z >> 5;
            }
        return n;
    }
    if (mil->ebx == IMPAI_NO_ANCHOR) return 0;
    tx[0] = mil->ebx;
    tz[0] = mil->ebz;
    return 1;
}

static uint32_t reach_mask_of(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int tx[PLANT_STARTS], tz[PLANT_STARTS], nt, k, ox, oz;
    unsigned c;
    if (d->reach_done) return d->reach_mask;
    d->reach_done = 1;
    d->reach_mask = 0xFFFFFFFFu;
    if (!in->reach || !in->mil || in->st->ax == IMPAI_NO_ANCHOR || !(nt = reach_targets(d, tx, tz)))
        return d->reach_mask;
    for (c = 0; c < WQ_MCLASSES; c++) {
        const int a = base_label(in->reach, c, in->st->ax >> 1, in->st->az >> 1);
        int any = 0;
        if (a < 0) continue;
        for (k = 0; k < nt && a > 0 && !any; k++)
            any = impai_reach_near(in->reach, c, (uint16_t)a, tx[k], tz[k], REACH_RINGS, &ox, &oz);
        if (!any) d->reach_mask &= ~(1u << c);
    }
    return d->reach_mask;
}

static int reaches(const IMPAI_DEC* d, uint32_t mask, unsigned type)
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    return (t->flags & IMPAI_TF_VTOL) || t->mclass >= WQ_MCLASSES || (mask >> t->mclass & 1u);
}

/* the tech level the values are read at: the seat's highest build tech, at least 1, at most IMPAI_QTECH */
static unsigned value_tech(const IMPAI_DEC* d)
{
    const unsigned T = d->ftop > 1 ? d->ftop : 1u;
    return T < IMPAI_QTECH ? T : IMPAI_QTECH;
}

/* 1 when `type` is of a gated class with a value above 0, reaching by `mask` */
static int gated_reaching(const IMPAI_DEC* d, uint32_t mask, unsigned type)
{
    const IMPAI_TINFO* t;
    if (type >= d->in->nti) return 0;
    t = &d->in->ti[type];
    return t->klass < IMPAI_KS && (IMPAI_GATED >> t->klass & 1u) && t->value > 0.0f && reaches(d, mask, type);
}

/* S(P) over P's menu (THE SCORE) */
static float plant_score(const IMPAI_DEC* d, const uint16_t* menu, int n, unsigned T, uint32_t mask)
{
    float s = IMPAI_Q_NONE;
    int k;
    if (!d->in->tv) return s;
    for (k = 0; k < n; k++)
        if (gated_reaching(d, mask, menu[k]) && d->in->tv[menu[k]].q[T - 1] > s) s = d->in->tv[menu[k]].q[T - 1];
    return s;
}

typedef struct PLANT_CAND { uint16_t type, via; uint8_t ctech; float s; } PLANT_CAND;

/* a before b in THE BEST's order; `scored` 0 ranks by the rest alone. A plant the seat's own builders build goes
   before one that waits for an unlock at an equal score, and always with no values, so the choice with no values
   is the least value among the plants the seat can build now */
static int plant_before(const IMPAI_DEC* d, const PLANT_CAND* a, const PLANT_CAND* b, int scored)
{
    const float va = d->in->ti[a->type].value, vb = d->in->ti[b->type].value;
    if (scored && a->s != b->s) return a->s > b->s;
    if (!a->via != !b->via) return !a->via;
    return va < vb || (va == vb && a->type < b->type);
}

static int type_bit(const uint32_t* bits, unsigned t) { return t < WQ_TYPES_MAX && (bits[t >> 5] >> (t & 31) & 1u); }

/* THE UNLOCKS: the constructor types on the menus of the seat's factory types (fidle, fbusy) of which it has no
   finished unit (IMPAI_DEC.cons_done), into via[], at most PLANT_VIA; their count */
static int plant_unlocks(IMPAI_DEC* d, uint16_t* via)
{
    const IMPAI_BRAIN_IN* in = d->in;
    uint16_t seen[IMPAI_GATE_TYPES];
    int nseen = 0, nv = 0, i, k, j;
    for (i = 0; i < d->nfidle + d->nfbusy && nseen < IMPAI_GATE_TYPES; i++) {
        const unsigned f = i < d->nfidle ? d->fidle[i].type : d->fbusy[i - d->nfidle].type;
        int n, dup = 0;
        for (j = 0; j < nseen; j++) dup |= seen[j] == f;
        if (dup) continue;
        seen[nseen++] = (uint16_t)f;
        n = d->q->ops->build_options(d->q, (int)f, d->menu3, WQ_TYPES_MAX);
        for (k = 0; k < n && nv < PLANT_VIA; k++) {
            const unsigned t = d->menu3[k];
            if (t >= in->nti || in->ti[t].cat != IMPAI_C_CONS || in->ti[t].value <= 0.0f || type_bit(d->cons_done, t))
                continue;
            for (j = 0, dup = 0; j < nv; j++) dup |= via[j] == t;
            if (!dup) via[nv++] = (uint16_t)t;
        }
    }
    return nv;
}

/* One pass of THE CHOICE: its plant, 0 none, the constructor it needs first (THE UNLOCKS) into *via, 0 none, and
   its runner-up and their scores. A plant is read once (looked[]) and takes a candidate's place only when it
   passes the filters a single plant can fail -- its constructor, and a tech-up's build tech -- so a plant that
   can never be chosen holds no place. BOUND: at most PLANT_LOOK plant menus read, IMPAI_PLANT_CANDS kept */
static int plant_pick(IMPAI_DEC* d, int waived, uint16_t* via, uint16_t* next, float* s, float* next_s)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int wet = anchor_wet(d);
    const int up = d->want_tech && !d->want_factory;
    const int scored = in->tv && !wet && d->count[IMPAI_C_FACTORY] > 0;
    const unsigned T = value_tech(d);
    const uint32_t mask = reach_mask_of(d);
    PLANT_CAND c[IMPAI_PLANT_CANDS];
    uint16_t plant[PLANT_MENU], src[IMPAI_SEA_BUILDERS + PLANT_VIA], looked[PLANT_LOOK];
    int nc = 0, nl = 0, b, k, j, best = -1, second = -1, nsrc = d->nbtype, nown = d->nbtype;
    unsigned lo = 256;
    memcpy(src, d->btype, (size_t)d->nbtype * sizeof src[0]);
    if (d->count[IMPAI_C_FACTORY] > 0 && d->gov_c < in->plan->cons_max) nsrc += plant_unlocks(d, src + nsrc);
    for (b = 0; b < nsrc; b++) {
        const int n = d->q->ops->build_options(d->q, src[b], d->menu3, WQ_TYPES_MAX);
        for (k = 0; k < n && nc < IMPAI_PLANT_CANDS && nl < PLANT_LOOK; k++) {
            const unsigned t = d->menu3[k];
            int seen = 0, np, ok = 0;
            if (t >= in->nti || in->ti[t].cat != IMPAI_C_FACTORY) continue;
            for (j = 0; j < nl; j++) seen |= looked[j] == t;
            if (seen || ((in->ti[t].flags & IMPAI_TF_WATER) && !wet) || (!waived && impai_plant_off(d, (int)t)) ||
                (up && in->ti[t].ctech <= d->ftop))
                continue;
            looked[nl++] = (uint16_t)t;
            np = d->q->ops->build_options(d->q, (int)t, plant, PLANT_MENU);
            for (j = 0; j < np && !ok; j++)
                ok = plant[j] < in->nti && in->ti[plant[j]].cat == IMPAI_C_CONS && builds_mex(d, plant[j]);
            if (!ok) continue;
            c[nc].type = (uint16_t)t;
            c[nc].via = (uint16_t)(b < nown ? 0 : src[b]);
            c[nc].ctech = in->ti[t].ctech;
            c[nc].s = plant_score(d, plant, np, T, mask);
            if (c[nc].ctech < lo) lo = c[nc].ctech;
            nc++;
        }
    }
    for (k = 0; k < nc; k++) {
        if (!up && c[k].ctech > (d->ftop > lo ? d->ftop : lo)) continue;
        if (best < 0 || plant_before(d, &c[k], &c[best], scored)) {
            second = best;
            best = k;
        } else if (second < 0 || plant_before(d, &c[k], &c[second], scored))
            second = k;
    }
    *via = best >= 0 ? c[best].via : 0;
    *next = second >= 0 ? c[second].type : 0;
    *s = best >= 0 ? c[best].s : IMPAI_Q_NONE;
    *next_s = second >= 0 ? c[second].s : IMPAI_Q_NONE;
    return best >= 0 ? c[best].type : 0;
}

/* THE CHOICE, once a decision: the plant, else the waived pass's (the cramped exit, THE PLANT'S FALLBACK) */
static void plant_choice(IMPAI_DEC* d)
{
    uint16_t next, via;
    float s, next_s;
    d->plant_done = 1;
    d->plant_t = (uint16_t)plant_pick(d, 0, &d->plant_via, &d->plant_next, &d->plant_s, &d->plant_next_s);
    if (d->plant_t) return;
    d->plant_tw = (uint16_t)plant_pick(d, 1, &via, &next, &s, &next_s);
    if (!d->plant_tw) return;
    d->plant_via = via;
    d->plant_next = next;
    d->plant_s = s;
    d->plant_next_s = next_s;
}

static int on_menu(const uint16_t* menu, int n, unsigned type)
{
    int k;
    for (k = 0; k < n; k++)
        if (menu[k] == type) return 1;
    return 0;
}

/* the plant ordered, for the plant line (IMPAI_BRAIN_OUT.plant*), before the wants are cleared */
static void plant_ordered(IMPAI_DEC* d, int type)
{
    IMPAI_BRAIN_OUT* o = d->out;
    o->plant = (uint16_t)type;
    o->plant_next = d->plant_next;
    o->plant_s = d->plant_s;
    o->plant_next_s = d->plant_next_s;
    o->plant_want = (uint8_t)(d->count[IMPAI_C_FACTORY] == 0 ? 1 : d->want_factory ? 2 : 3);
    o->plant_T = (uint8_t)value_tech(d);
    o->plant_reach = reach_mask_of(d);
}

/* THE SEA PLANT's type: the least value among the menu's water factories (impai_water_plant) whose own menu
   offers a sea army type (klass IMPAI_K_SRAIDER..IMPAI_K_SAA, a value above 0), ties to the lower type; at most
   FACTORY_LOOK candidates examined, each a menu read of at most PLANT_MENU types. Writes neither d->menu nor
   d->menu2. Sea army types are bought only where a factory offers them (THE ARMY ORDERS), so this is the plant
   that can make them. */
static int choose_sea_plant(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    uint16_t plant[PLANT_MENU];
    int k, j, best = 0, looked = 0;
    for (k = 0; k < n && looked < FACTORY_LOOK; k++) {
        const int t = menu[k];
        int np, ok = 0;
        if (!impai_water_plant(in, (unsigned)t)) continue;
        if (best && (in->ti[t].value > in->ti[best].value || (in->ti[t].value == in->ti[best].value && t > best))) continue;
        looked++;
        np = d->q->ops->build_options(d->q, t, plant, (int)(sizeof plant / sizeof plant[0]));
        for (j = 0; j < np && !ok; j++)
            ok = plant[j] < in->nti && in->ti[plant[j]].klass >= IMPAI_K_SRAIDER && in->ti[plant[j]].klass <= IMPAI_K_SAA &&
                 in->ti[plant[j]].value > 0.0f;
        if (ok) best = t;
    }
    return best;
}

int impai_econ_sea_able(IMPAI_DEC* d)
{
    int k, n;
    for (k = 0; k < d->nbtype; k++)
        if ((n = d->q->ops->build_options(d->q, d->btype[k], d->menu, WQ_TYPES_MAX)) > 0 && choose_sea_plant(d, d->menu, n))
            return 1;
    return 0;
}

/* an anti-air structure: the cheapest of the menu's (impai_aa_struct), ties to the lower type */
static int choose_aa(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        if (!impai_aa_struct(in, (unsigned)t)) continue;
        if (!best || in->ti[t].value < in->ti[best].value || (in->ti[t].value == in->ti[best].value && t < best))
            best = t;
    }
    return best;
}

/* THE GROUND DEFENCES. While THE PRESSURE stands at the base (impai_mil.c) the ladder builds a ground defence
   (impai_def_struct) before an anti-air structure: one, and one more for each def_v of pressing value, at most
   def_max (impai_brain.c, economy), on the ring site nearest THE DEFENCE'S POINT, the anchor moved rally_px
   towards the pressing member (impai_build_def). Its site and its walk obey the ring's threat rules like any
   other: a site inside known ground threat is skipped, so the tower stands at the threat's edge, and a missile
   tower's range covers it from there. No defence is built before a raid: the governor's army is the seat's
   answer to what it has not seen yet.
   THE TYPE is the one worth most against what the seat sees, by cost:
       score(t) = sum over the picture's enemy types e of share_e x rate(t, e) x [reach_t >= reach_e] / value_t
   with rate the fight predictor's pair rate (HP a tick), share the seen enemy mobile value's, reach the ground
   reach (IMPAI_THREAT.hit_range) and value the type's. A tower the enemy outranges takes its damage and returns
   none, so its rate counts only against what it outranges. Ties to the lower type; a water-only type only on a
   wet anchor; only a score above 0 qualifies, so with no picture or no predictor no defence is built. No unit
   list: a mod's towers are scored by the same rule. OFFLINE, against the opponent's T1 lab and vehicle-plant
   army at equal shares: retail ARM takes ARMRL (7.19 against ARMLLT 6.57), CORE CORRL (7.32 against CORLLT
   4.91), the LLT winning both without the reach term; Total Mayhem ARM ARMRL, CORE CORHLT (6.99 against CORRL
   6.84); ProTA ARM ARMRL, CORE CORPLAS.
   WHY, MEASURED: 895 constructor frames killed on the yard at the bar's second start (2026-11-13), and no seat
   ever built a ground defence. BARb's AiMakeDefence answers a mobile threat with a tower, read for its design
   only (GPL v2: no code taken). BOUND: def_max, at most 4 (impai_brain_test.c, check_plan); the score walks the
   menu (WQ_TYPES_MAX) x the picture (IMPAI_PIC_ENEMY). */
static int choose_def(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_PICTURE* pic = in->mil ? &in->mil->pic : NULL;
    const uint32_t ne = pic && pic->ne <= IMPAI_PIC_ENEMY ? pic->ne : 0;
    const int wet = anchor_wet(d);
    float best_s = 0.0f;
    int k, best = 0;
    uint32_t e;
    if (!in->pred || !in->pred->rate || !ne) return 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        float s = 0.0f;
        if (!impai_def_struct(in, (unsigned)t) || !(in->ti[t].value > 0.0f)) continue;
        if ((in->ti[t].flags & IMPAI_TF_WATER) && !wet) continue;
        for (e = 0; e < ne; e++) {
            const unsigned et = pic->e[e].type;
            if (et >= (unsigned)in->view->ntypes || !(pic->e[e].share > 0.0f)) continue;
            if (in->view->threat[t].hit_range >= in->view->threat[et].hit_range)
                s += pic->e[e].share * in->pred->rate(in->pred->ctx, (uint16_t)t, (uint16_t)et);
        }
        s /= in->ti[t].value;
        if (s > best_s) {
            best_s = s;
            best = t;
        }
    }
    return best;
}

/* a factory's constructor: the most workertime per value among those whose own menu holds an extractor */
static int choose_cons(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        float lhs, rhs;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_CONS || in->ti[t].value <= 0) continue;
        ti = &in->ti[t];
        if (best) {
            lhs = (float)ti->workertime * in->ti[best].value;
            rhs = (float)in->ti[best].workertime * ti->value;
            if (lhs < rhs || (lhs == rhs && t > best)) continue;
        }
        if (builds_mex(d, t)) best = t;
    }
    return best;
}

/* ---- 4. the builders ---- */

/* A METAL MAP: the threshold is 255 (SurfaceMetal, game state +0xD30, held to 255), so no cell can stand above
   it and the patch scan publishes no patch whatever the seat explores; the loader seeds every cell with the
   background (0x4839B3..0x4839E6, engine/map.md), and an extractor yields extractsmetal x its footprint's metal
   wherever it stands. Core Prime Industrial Area is one: SurfaceMetal 255, no metal feature, every cell 255.
   The stock AI places its extractors there by its general site search: it takes the spot search only when
   SurfaceMetal is below a draw of 0x4B6C30(255), 0..254 (0x40C150..0x40C16C), and the general search 0x40A5D0
   refuses a site only when its footprint holds more than twice the background over its area
   (0x40A5EC..0x40A602, 0x40A76F), which a footprint of 255s never does (DISASSEMBLED). So does ImpAI: the
   extractor goes to the base ring like any other building, and the ladder alone decides how many. */
static int metal_map(const IMPAI_DEC* d) { return d->m.threshold >= 255; }

/* THE LADDER for builder b, the first rule with a type in b's menu and a site winning; a rule that finds no
   site falls through. A helper (a builder assisting) answers the first two rules, and the reclaim. The second
   is the factory: while the seat wants its sea plant (impai_brain.c, economy) a builder whose menu offers one
   searches for its site, and a land plant waits; a search that finds no site for any sea plant ends the want,
   so the next factory is a land one, in the same decision. A builder with no sea plant on its menu builds no
   factory while the want lasts: another builder offers one (impai_econ_sea_able). A builder whose far record is
   live takes neither rule (impai_brain.c, THE FAR RECORD), so the plant goes to one that can walk. An
   extractor's site is a patch, or on a metal map the ring. A constructor explores only when no patch is left
   for it -- none free, reachable on its class and unthreatened -- so a patch it can never reach does not keep it
   home.
   THE RECLAIM (impai_build.c) is asked twice, each time for a feature worth more than what the builder would
   do instead, and never before the plants or the seat's first open_mex extractors:
   - BEFORE AN EXTRACTOR, once open_mex are finished: a feature wins over the extractor b has a site for when it
     pays b more metal a tick than an extractor makes, the seat's metal income a pass over its finished
     extractors (the share pm_in projects by) over the pass's 30 ticks (engine/weapons.md, The settle). A
     reclaim pays in ticks what an extractor makes in minutes -- the countdown has no worker-time term and runs
     about half a tick a metal (engine/orders.md) -- so while the store has room a builder clears the features near it
     first; a full store makes every feature worth nothing, and the extractors come back.
   - BEFORE THE BUILDINGS THAT SPEND, while the seat is short of metal (impai_brain.c, economy: m_short): any
     feature with worth, ahead of energy, makers, exploring and the assist (for a builder with no menu, the
     assist alone). A helper's assist is a build's help, so a seat short of metal takes a helper to a feature as
     well. */
static int assign(IMPAI_DEC* d, const IMPAI_UNITREF* b, int helper)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* bt = &in->ti[b->type];
    const int n = d->q->ops->build_options(d->q, b->type, d->menu, WQ_TYPES_MAX);
    const float keep = d->e_per_m * d->pm_in;
    const int far = impai_far(d, b);
    int t;
    if (n > 0) {
        if (d->e_stall && (t = choose_energy(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->pe_in += in->ti[t].egain;
            return 1;
        }
        if (d->want_sea && !far && (t = choose_sea_plant(d, d->menu, n))) {
            const int y = impai_build_sea_plant(d, b, t);
            if (y > 0) {
                d->want_sea = d->want_factory = d->want_tech = 0;
                return 1;
            }
            if (y < 0) d->want_sea = 0;
        }
        if ((d->want_factory || d->want_tech) && !d->want_sea && !far) {
            /* THE CHOICE is the seat's, built by a builder whose menu holds it; with every plant type it would
               take passed over (THE PLANT'S FALLBACK), the waived pass's, the cramped exit. plant_choice reads
               menu2 and menu3 only, so this builder's menu is still d->menu */
            if (!d->plant_done) plant_choice(d);
            if ((d->plant_t && on_menu(d->menu, n, d->plant_t) && impai_build_ring(d, b, (t = d->plant_t))) ||
                (!d->plant_t && d->plant_tw && on_menu(d->menu, n, d->plant_tw) &&
                 impai_build_plant_waived(d, b, (t = d->plant_tw)))) {
                plant_ordered(d, t);
                d->want_factory = d->want_tech = 0;
                return 1;
            }
        }
    }
    if (helper) return d->m_short && impai_build_reclaim(d, b, 0.0);
    if (n > 0) {
        if (d->want_def && (t = choose_def(d, d->menu, n)) && impai_build_def(d, b, t)) {
            d->want_def = 0;
            return 1;
        }
        if (d->want_aa && (t = choose_aa(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->want_aa = 0;
            return 1;
        }
        if ((t = choose_mex(d, d->menu, n))) {
            if (d->done[IMPAI_C_MEX] > 0 && d->done[IMPAI_C_MEX] >= in->plan->open_mex &&
                (metal_map(d) || impai_spot_pick(d, b, t)) &&
                impai_build_reclaim(d, b, (double)d->e.metal_in / (double)d->done[IMPAI_C_MEX] / 30.0))
                return 1;
            if (metal_map(d) ? impai_build_ring(d, b, t) : impai_build_spot(d, b, t)) return 1;
        }
        if (d->m_short && impai_build_reclaim(d, b, 0.0)) return 1;
        if (d->pe_in < (keep > 1.1f * d->e.energy_out ? keep : 1.1f * d->e.energy_out) &&
            (t = choose_energy(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->pe_in += in->ti[t].egain;
            return 1;
        }
        if (d->e.energy > 0.8f * d->e.energy_max && (t = choose_maker(d, d->menu, n)) &&
            d->e.energy_in - d->e.energy_out > in->ti[t].egain && impai_build_ring(d, b, t))
            return 1;
    }
    if (bt->cat == IMPAI_C_CONS && !d->explorer_live) {
        const int mex = n > 0 ? choose_mex(d, d->menu, n) : 0;      /* 0: none, never a chosen type */
        if (!impai_spot_pick(d, b, mex ? mex : -1) && impai_build_explore(d, b)) return 1;
    }
    if (n <= 0 && d->m_short && impai_build_reclaim(d, b, 0.0)) return 1;
    return impai_build_assist(d, b);
}

/* THE REPAIR first (impai_build.c), then idle builders in slot order, then the helpers for the first two rules
   and the reclaim, at most assign_max of them, while the decision's commands last. An idle builder whose BUILD or
   reclaim job this decision did not check waits for the check, which may record its feature as one it could not
   reach. A helper whose job is no longer an assist is skipped: the census lists the helpers before the walk checks run (impai_brain.c), and a
   helper the walk check turned back this decision holds the turn back's move and no job, which a second order
   here would replace -- a reclaim's choice tests the feature's cell for threat, but not THE ROUTE.
   THE ATTENTION: a persona's att_eco holds the builders busy at once, the census's busy ones and this decision's
   assignments (impai_econ_room), so an idle builder past it waits; a helper reassigned stays one busy builder,
   and needs no test.
   THE RELEASE, in THE HOLD (impai_brain.c, economy): a helper whose target is not an economy building's frame --
   a factory it guards, a factory's product, a plant's frame -- is given the ladder an idle builder gets, and is
   stopped (resolver mode 10, then the order, as the job checks' stop) when the ladder has nothing for it.
   INVARIANT, an ordering by the job field: the helper's job ends only together with an order that replaces its
   head -- the ladder's order, or a stop the resolver accepted and the WORLDQ queued -- so no finished builder is
   left with a busy head and no job, and THE HAND-BACK's invariant stays true: every finished builder is listed
   idle, as a helper or as jobbed. A guard never ends by itself, so an unlisted guard would be lost for the game.
   [INFERRED: a stopped builder's list empties and the controller's Standby, flagged 0x4000 (0x43B9AD..0x43BA3A,
   engine/orders.md), makes it idle at the next census, as the job checks' stop relies on.] BOUND: the census's
   helpers, at most IMPAI_IDLE_CAP, each with one search of at most IMPAI_NANO_MAX frames; every order within
   assign_max and the roots. RESIDUAL: an economy frame past the census's IMPAI_NANO_MAX is not found, and its
   helper is released (one order).
   THE RELEASE IS NEVER STARVED BY A BUDGET: with the economy's bucket limiting the decision (IMPAI_BRAIN_IN.acts
   0 or more) and a helper to release in the hold, the idle loop leaves the decision's last root to the release,
   so each decision with a command to give releases one; idle builders, which appear whenever a build ends, cannot
   take every root decision after decision. With no budget the order is unchanged. */
int impai_econ_room(const IMPAI_DEC* d)
{
    const int att = d->in->persona->att_eco;
    return !att || d->nbusy + d->assigned < att;
}

/* 1 when helper h assists an economy building's frame the census found (THE RELEASE's test) */
static int helper_eco(const IMPAI_DEC* d, const IMPAI_UNITREF* h)
{
    const uint16_t target = d->in->job[h->slot].target;
    int k;
    for (k = 0; k < d->nnano; k++)
        if (d->nano[k].slot == target) return IMPAI_ECO_CAT(d->in->ti[d->nano[k].type].cat);
    return 0;
}

void impai_econ_assign(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i, idle_cap;
    impai_build_repair(d);
    idle_cap = d->root_cap;
    if (in->acts >= 0 && d->hold && idle_cap > 0)
        for (i = 0; i < d->nhelpers; i++)
            if (in->job[d->helpers[i].slot].kind == IMPAI_JOB_ASSIST && !helper_eco(d, &d->helpers[i])) {
                idle_cap--;
                break;
            }
    for (i = 0; i < d->nidle && d->assigned < in->plan->assign_max && d->roots < idle_cap && impai_econ_room(d);
         i++) {
        const IMPAI_JOB* j = &in->job[d->idle[i].slot];
        if (j->kind == IMPAI_JOB_BUILD || j->kind == IMPAI_JOB_RECLAIM || j->kind == IMPAI_JOB_RECLAIM_GO) continue;
        if (assign(d, &d->idle[i], 0)) d->assigned++;
    }
    /* the busy builders the decision leaves, counted before the helpers, whom the census already counts busy */
    d->out->busy = (uint16_t)(d->nbusy + d->assigned);
    for (i = 0; i < d->nhelpers && d->assigned < in->plan->assign_max && d->roots < d->root_cap; i++) {
        const IMPAI_UNITREF* h = &d->helpers[i];
        IMPAI_JOB* j = &in->job[h->slot];
        if (j->kind != IMPAI_JOB_ASSIST) continue;
        if (d->hold && !helper_eco(d, h)) {
            int ok = assign(d, h, 0), a;
            if (!ok && d->roots < d->root_cap && (a = d->q->ops->can(d->q, h->slot, 10, (int)WQ_NONE, h->pos)) > 0) {
                WQ_CMD c;
                memset(&c, 0, sizeof c);
                c.verb = WQ_CMD_NEW;
                c.action = (uint8_t)a;
                c.unit = h->slot;
                c.target = (uint16_t)WQ_NONE;
                memcpy(c.pos, h->pos, sizeof c.pos);
                if (impai_give(d, &c) == WQ_OK) {
                    impai_job_end(j);
                    ok = 1;
                }
            }
            if (ok) {
                d->assigned++;
                d->out->released++;
            }
            continue;
        }
        if ((d->e_stall || d->want_factory || d->want_sea || d->m_short) && assign(d, h, 1)) d->assigned++;
    }
}

/* ---- 5. the factories ---- */

/* factory type `type`'s army types into mt (at most IMPAI_MENU_MAX): its menu's types with an army class and a
   value; reads into d->menu */
static uint32_t army_menu(IMPAI_DEC* d, unsigned type, IMPAI_MENU_TYPE* mt)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int n = d->q->ops->build_options(d->q, (int)type, d->menu, WQ_TYPES_MAX);
    uint32_t k, m = 0;
    for (k = 0; n > 0 && k < (uint32_t)n && m < IMPAI_MENU_MAX; k++) {
        const unsigned t = d->menu[k];
        if (t >= in->nti || in->ti[t].klass >= IMPAI_KS || !(in->ti[t].value > 0.0f)) continue;
        mt[m].type = (uint16_t)t;
        mt[m].klass = in->ti[t].klass;
        mt[m].pad = 0;
        mt[m].value = in->ti[t].value;
        m++;
    }
    return m;
}

/* a QUEUE of one unit of `type` at factory f, counted against the decision's commands: its order's answer */
static int queue_one(IMPAI_DEC* d, const IMPAI_UNITREF* f, int type)
{
    WQ_CMD c;
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_QUEUE;
    c.unit = f->slot;
    c.target = (uint16_t)WQ_NONE;
    c.type = (uint16_t)type;
    c.count = 1;
    return impai_give(d, &c);
}

/* THE ARMY ORDERS: which army classes each idle factory may take.
       fbest(f, k) = the highest value (impai_tinfo.c, THE VALUES, at THE SCORE's tech level T) of f's menu types of
                     gated class k with a value above 0 that reach (THE REACH); IMPAI_Q_NONE for none
       kbest(k)    = the highest fbest(f, k) over the distinct types of the seat's finished factories, idle or busy
       eligible(f) = f's army classes outside IMPAI_GATED, and each gated class k with
                     fbest(f, k) > IMPAI_Q_NONE and fbest(f, k) >= min(0, kbest(k))
       poor(f)     = f offers a gated class and none is eligible
       A(f)        = the highest fbest(f, k), IMPAI_Q_NONE for none
   - A class's unit that holds its value against the field (q >= 0) may be built on any factory that has one;
     below 0 only on the factories whose unit is the seat's best of the class. So a plant whose army is poor here
     builds none of it, and a class only one plant offers stays that plant's, which keeps every class the seat
     can build in the table's composition. RESIDUAL: such a class is built there however low its value, because
     the table wants it.
   - A poor plant takes the governor's constructor when the governor allows one, else no army order outside a
     surplus (A SURPLUS below). It is counted (IMPAI_BRAIN_OUT.poor) when it is left without an order.
   - The governor walks the idle factories that offer a gated class in ascending A, then those that offer none --
     a shipyard, an air plant -- ties in slot order, so a constructor goes to the land plant whose army is worth
     least here; and a plant the values do not score makes a constructor only when no scored plant is idle this
     decision, so a land plant that took an army order instead does not pass the constructor to the yard behind
     it. WHY: such a plant's A is IMPAI_Q_NONE, below every scored plant's, and sorted by it alone the shipyard and
     the air plant took the governor's constructors (CORCS, CORCA) before any land plant; sorted after them alone,
     the yard still took 6 of 7 in the brain test's twenty decisions, each land army order meeting the army's
     share for the yard's constructor after it. A naval seat whose land plants are all busy still makes sea
     constructors, which is meant: its sea plant has work for them.
   - A CLASS NO UNIT OF THE SEAT REACHES: when no type of gated class k on the seat's factories reaches, class k
     is read without the reach, as on a linked map, so the land plants still build that class by value for the
     base's defence (owner's decision, 2026-11-14: with no land route, land factories still build land army by
     value). Per class and not per seat: one amphibious unit that reaches on the seat's advanced plant made every
     other plant poor, and Trout Farm's two vehicle plants sat without an order for 726 factory-decisions
     (MEASURED 2026-11-14, plantchoice r3, bar-eota-troutfarm-cc-1). The plant choice keeps the reach either way.
   - A SURPLUS: a poor plant beside a store in surplus buys its cheapest army type of any class of its menu, as an
     ungated factory's surplus buy: the store is full, so the metal buys nothing else (THE GOVERNOR, A SURPLUS).
   - With no values, or a factory type past IMPAI_GATE_TYPES, the factory is ungated: every class of its menu is
     eligible, it is not poor, and it walks with the plants that offer no gated class.
   - RESIDUAL, THE TYPE AFTER CONTACT: with a picture, a gated class's type is impai_class_type's -- the pair rate
     against the seen enemy per value -- which reads neither the values nor the reach, so an eligible factory may
     then build a type of the class that does not reach the enemy, where THE TYPE BEFORE CONTACT would not.
   WHY, MEASURED 2026-11-14 over the bar's recordings: army orders went to any idle plant, and 31 % of CORE's army
   units were hovers from the hover plant whatever their value. BOUND: one menu read for each distinct factory type
   of the census's lists, at most IMPAI_GATE_TYPES, and one for each idle factory of a type past them. */
/* type `type`'s army classes and its best value in each gated class by `mask` and without the reach, into
   *classes, best[] and all[]. BOUND: a menu type's class indexes best[] and all[] only once tested below
   IMPAI_GATE_K -- an army class runs to IMPAI_KS (13), and the rows sit inside IMPAI_DEC, where an overrun
   would write the memo's next fields unseen by any sanitizer */
static void gate_read(IMPAI_DEC* d, unsigned type, unsigned T, uint32_t mask, uint32_t* classes,
                      float best[IMPAI_GATE_K], float all[IMPAI_GATE_K])
{
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    IMPAI_MENU m;
    uint32_t i;
    unsigned k;
    for (k = 0; k < IMPAI_GATE_K; k++) best[k] = all[k] = IMPAI_Q_NONE;
    m.t = mt;
    m.n = army_menu(d, type, mt);
    *classes = impai_menu_classes(&m);
    for (i = 0; i < m.n; i++) {
        const unsigned t = mt[i].type;
        float q;
        if ((k = mt[i].klass) >= IMPAI_GATE_K) continue;
        q = gated_reaching(d, 0xFFFFFFFFu, t) ? d->in->tv[t].q[T - 1] : IMPAI_Q_NONE;
        if (q > all[k]) all[k] = q;
        if (gated_reaching(d, mask, t) && q > best[k]) best[k] = q;
    }
}

static int gate_find(const IMPAI_DEC* d, unsigned type)
{
    int g;
    for (g = 0; g < d->ngate; g++)
        if (d->gate_type[g] == type) return g;
    return -1;
}

/* the distinct factory types of fidle and fbusy into the memo, read by `mask` and without it; the gated classes
   no type reaches by `mask` into d->gate_noreach */
static void gate_types(IMPAI_DEC* d, unsigned T, uint32_t mask)
{
    int i, k;
    uint32_t reached = 0;
    d->ngate = 0;
    for (i = 0; i < d->nfidle + d->nfbusy; i++) {
        const unsigned type = i < d->nfidle ? d->fidle[i].type : d->fbusy[i - d->nfidle].type;
        if (gate_find(d, type) >= 0 || d->ngate >= IMPAI_GATE_TYPES) continue;
        d->gate_type[d->ngate] = (uint16_t)type;
        gate_read(d, type, T, mask, &d->gate_classes[d->ngate], d->gate_best[d->ngate], d->gate_all[d->ngate]);
        for (k = 0; k < IMPAI_GATE_K; k++)
            if (d->gate_best[d->ngate][k] > IMPAI_Q_NONE) reached |= 1u << k;
        d->ngate++;
    }
    d->gate_noreach = IMPAI_GATED & ~reached;
}

/* factory type g's best value in gated class k as the gate reads it: by the reach, or without it for a class no
   type of the seat reaches */
static float gate_fb(const IMPAI_DEC* d, int g, int k)
{
    if ((unsigned)g >= IMPAI_GATE_TYPES || (unsigned)k >= IMPAI_GATE_K) return IMPAI_Q_NONE;
    return (d->gate_noreach >> k & 1u) ? d->gate_all[g][k] : d->gate_best[g][k];
}

/* each idle factory's eligible classes, poverty, A (above) and whether it is unscored -- it offers no gated
   class, or the gate does not read it; with no values, every factory ungated and unscored */
static void plant_gate(IMPAI_DEC* d, uint32_t* elig, uint8_t* poor, float* A, uint8_t* unscored)
{
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    const unsigned T = value_tech(d);
    float kbest[IMPAI_GATE_K] = { IMPAI_Q_NONE, IMPAI_Q_NONE, IMPAI_Q_NONE };
    int i, g, k;
    d->gate_mask = reach_mask_of(d);
    d->gate_noreach = 0;
    if (d->in->tv) gate_types(d, T, d->gate_mask);
    for (g = 0; d->in->tv && g < d->ngate; g++)
        for (k = 0; k < IMPAI_GATE_K; k++)
            if (gate_fb(d, g, k) > kbest[k]) kbest[k] = gate_fb(d, g, k);
    for (i = 0; i < d->nfidle; i++) {
        const int at = d->in->tv ? gate_find(d, d->fidle[i].type) : -1;
        uint32_t e;
        poor[i] = 0;
        A[i] = IMPAI_Q_NONE;
        unscored[i] = (uint8_t)(at < 0 || !(d->gate_classes[at] & IMPAI_GATED));
        if (at < 0) {
            IMPAI_MENU m;
            m.t = mt;
            m.n = army_menu(d, d->fidle[i].type, mt);
            elig[i] = impai_menu_classes(&m);
            continue;
        }
        e = d->gate_classes[at] & ~IMPAI_GATED;
        for (k = 0; k < IMPAI_GATE_K; k++) {
            const float fb = gate_fb(d, at, k), floor_k = kbest[k] < 0.0f ? kbest[k] : 0.0f;
            if (fb > IMPAI_Q_NONE && fb >= floor_k) e |= 1u << k;
            if (fb > A[i]) A[i] = fb;
        }
        elig[i] = e;
        poor[i] = (uint8_t)((d->gate_classes[at] & IMPAI_GATED) && !(e & IMPAI_GATED));
    }
}

/* THE TYPE BEFORE CONTACT for gated class k on menu m: with the picture empty, the type of most value per metal
   (THE VALUES) among m's class-k types that reach as the gate reads class k, ties to the lower type; -1 none. A plant
   chosen for its best unit then builds that unit, not the cheapest of its class, which is what the type choice
   gives an empty picture (impai_table.c, impai_class_type) */
static int class_best(const IMPAI_DEC* d, const IMPAI_MENU* m, unsigned k, unsigned T)
{
    const uint32_t mask = (d->gate_noreach >> k & 1u) ? 0xFFFFFFFFu : d->gate_mask;
    float best = IMPAI_Q_NONE;
    int t = -1;
    uint32_t i;
    for (i = 0; i < m->n; i++) {
        const unsigned u = m->t[i].type;
        if (m->t[i].klass != k || !gated_reaching(d, mask, u)) continue;
        if (t < 0 || d->in->tv[u].q[T - 1] > best || (d->in->tv[u].q[T - 1] == best && (int)u < t)) {
            best = d->in->tv[u].q[T - 1];
            t = (int)u;
        }
    }
    return t;
}

/* THE ARMY for idle factory f, one unit, while the decision's commands last. The class is A3's (impai_table.h):
   of the classes `eligible` holds (THE ARMY ORDERS), the one furthest below its share of the army's value -- built,
   building and queued, kspent -- (impai_buy_next), the shares those of the seat's table over its military's
   picture for the classes some idle factory not poor may take (`share`, impai_table_shares). The type: for a gated
   class with the picture empty and the values given, THE TYPE BEFORE CONTACT; else the type choice's of the class
   (impai_class_type, the predictor's pair rate against the picture's enemy composition). `cheapest`: instead the
   cheapest army type of the factory's menu in an eligible class with a value above 0, ties to the lower type --
   the surplus's buy (THE GOVERNOR). 1 when ordered. One unit a factory a decision: the decision comes every
   look_eco ticks of the persona and its order is given react ticks later, so a factory is idle at most look_eco +
   react ticks between two units, and the composition answers the picture as it moves. BOUND: a menu past IMPAI_MENU_MAX army types is cut there. */
static int queue_army(IMPAI_DEC* d, const IMPAI_UNITREF* f, const float* share, uint32_t eligible, int cheapest)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_MIL* mil = in->mil;
    const unsigned T = value_tech(d);
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    float cost[IMPAI_KS];
    int32_t type[IMPAI_KS];
    IMPAI_MENU m;
    int k, t = -1;
    if (d->queued >= in->plan->queue_max || d->roots >= d->roots_max) return 0;
    m.t = mt;
    m.n = army_menu(d, f->type, mt);
    if (cheapest) {
        uint32_t i;
        for (i = 0; i < m.n; i++)
            if ((eligible >> mt[i].klass & 1u) &&
                (t < 0 || mt[i].value < in->ti[t].value || (mt[i].value == in->ti[t].value && mt[i].type < t)))
                t = mt[i].type;
        if (t < 0) return 0;
        k = in->ti[t].klass;
    } else {
        for (k = 0; k < IMPAI_KS; k++) {
            type[k] = -1;
            cost[k] = 0.0f;
            if (!(eligible >> k & 1u)) continue;
            if (in->tv && mil->pic.ne == 0 && (IMPAI_GATED >> k & 1u)) type[k] = class_best(d, &m, (unsigned)k, T);
            if (type[k] < 0) type[k] = impai_class_type(&mil->table, &mil->pic, &m, (unsigned)k, in->pred);
            if (type[k] > 0 && (unsigned)type[k] < in->nti) cost[k] = in->ti[type[k]].value;
        }
        if ((k = impai_buy_next(share, d->kspent, cost, eligible, FLT_MAX)) < 0 || type[k] <= 0) return 0;
        t = type[k];
    }
    if (queue_one(d, f, t) != WQ_OK) return 0;
    d->kspent[k] += in->ti[t].value;
    d->gov_a++;
    d->queued++;
    d->out->orders++;
    d->out->army++;
    return 1;
}

/* THE GOVERNOR: 1 when an idle factory may queue a constructor now */
static int gov_cons(const IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const int64_t c = d->gov_c;
    const int64_t att = d->in->persona->att_eco;
    if (c >= plan->cons_max || (att && c >= att) || d->pressed) return 0;   /* the caps; THE PRESSURE      */
    if (c < plan->cons_open) return 1;                                        /* THE OPENING                 */
    return (int64_t)d->gov_a >= (int64_t)plan->army_per_cons * (c - plan->cons_open + 1);   /* THE ARMY'S SHARE */
}

/* THE GOVERNOR. Each idle factory, in THE ARMY ORDERS' walk (the plants offering a gated class in ascending A, then
   the rest, ties in slot order), while the decision's commands last, is given one order: a constructor -- its best
   (choose_cons), or while the plant choice waits for an unlock (THE UNLOCKS) that constructor at a factory that
   makes it and none at any other until one is queued -- when all three rules
   hold, otherwise one army unit in a class it may take (queue_army), and a poor plant nothing outside a surplus.
   A constructor order the factory refuses leaves it for the army in the same decision.
   - THE OPENING. The first cons_open constructors need nothing.
   - THE ARMY'S SHARE. Past them, a constructor needs army >= army_per_cons x (constructors - cons_open + 1).
   - THE PRESSURE. No constructor while the military's stamp lasts (impai_mil.c, THE PRESSURE).
   - A SURPLUS. With the store at SURPLUS_FRAC of its maximum or more, income outruns spending: a factory whose
     table buy finds nothing buys its cheapest army type of a class it may take, and a poor plant its cheapest army
     type of any class, so no factory idles beside a full store; one with no such type is counted
     (IMPAI_BRAIN_OUT.fidle, a poor one .poor). The share, the cap and the pressure still hold.
   - THE HOLD'S FLOOR (impai_brain.c, economy). In a hold an idle factory is given no order once the busy ones and
     this walk's orders reach hold_fac, and is counted (IMPAI_BRAIN_OUT.held). The walk's order is unchanged: the
     hold decides how many factories run, not which plant does what, and the floor's factory takes whatever the
     governor gives it, a constructor included; a poor plant left without an order uses up no floor. A unit in
     progress is never stopped: each order is one unit, so a factory busy at the hold's start is held from its
     next idle decision on. The hold refuses no constructor: constructors are the economy's growth, and the
     measured stall is one of too little income; a rule that refused them in a hold would hold them for most of
     the game [INFERRED from the store's runs below 20 % of its storage: 239 passes at the median, p90 543,
     MEASURED 2026-11-14 over the same seat-games as below]. INVARIANT, a bound on counts: d->queued counts only
     this walk's orders, so nfbusy + queued is the factories busy once the walk ends, and in a hold the walk gives
     at most max(0, hold_fac - nfbusy) orders -- at most max(nfbusy, hold_fac) finished factories busy after it. nfbusy is the census's, held to IMPAI_IDLE_CAP: a census past the cap
     counts fewer, never more, and hold_fac <= IMPAI_IDLE_CAP (impai_brain_test.c, check_plan).
   NO RULE TIES THE CONSTRUCTORS TO THE INCOME: constructors build the extractors that pay for them, so a cap
   proportional to income holds a seat to the income it already makes. MEASURED 2026-11-14: such a cap (24
   workertime a metal a pass) was the binding rule in 59 % of the samples past GameTime 9 000 (57 ImpAI seat-games
   of the bar) and held the seats at 2-4 constructors against 6-11.
   INVARIANT, a bound on counts: at every decision that queues a constructor, (1) the constructors are below
   cons_max; (2) below cons_open, or the army share holds; (3) not pressed. Every count is the census's of the
   same decision plus this decision's own orders, in
   the walk's order (THE GOVERNOR'S COUNTS, impai_brain.h): no state, no timing. Without a military or a predictor
   the factories buy no army, as at A1, and are not gated.
   WHY, MEASURED: the first army unit came at a median GameTime of 13 500-15 000 against a first factory at 3 300-
   3 600, and at 12 000 the seats held 5-6 constructors and no army at every level (196 seat-games of the bar,
   2026-11-13); a constructor takes a factory 1 485-1 745 ticks against 436-521 for a raider; in a game against a
   human on Comet Catcher, 95 % of the factory-ticks went to constructors and 27 of them were lost. BOUND: the
   factories looked at are the census's, IMPAI_IDLE_CAP, each one menu read for its constructor and at most two
   for its army, beside THE ARMY ORDERS' reads; the order is an insertion sort of at most IMPAI_IDLE_CAP. */
void impai_econ_factories(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_PLAN* plan = in->plan;
    const int army = in->mil && in->pred;
    uint32_t elig[IMPAI_IDLE_CAP];
    uint8_t poor[IMPAI_IDLE_CAP], order[IMPAI_IDLE_CAP], unscored[IMPAI_IDLE_CAP];
    float A[IMPAI_IDLE_CAP], share[IMPAI_KS];
    unsigned buildable = 0, unlock;
    int i, j, t, scored_idle = 0;
    memset(share, 0, sizeof share);
    memset(poor, 0, sizeof poor);
    memset(unscored, 1, sizeof unscored);
    for (i = 0; i < d->nfidle; i++) A[i] = IMPAI_Q_NONE;
    if (army) {
        plant_gate(d, elig, poor, A, unscored);
        for (i = 0; i < d->nfidle; i++)
            if (!poor[i]) buildable |= elig[i];
        if (buildable) impai_table_shares(&in->mil->table, &in->mil->pic, buildable, share);
    }
    for (i = 0; i < d->nfidle; i++) {
        for (j = i; j > 0 && (unscored[order[j - 1]] > unscored[i] ||
                              (unscored[order[j - 1]] == unscored[i] && A[order[j - 1]] > A[i]));
             j--)
            order[j] = order[j - 1];
        order[j] = (uint8_t)i;
        scored_idle |= !unscored[i];
    }
    if ((d->want_factory || d->want_tech) && !d->want_sea && !d->plant_done) plant_choice(d);
    unlock = d->plant_t || d->plant_tw ? d->plant_via : 0;
    if (type_bit(d->cons_frame, unlock)) unlock = 0;
    for (j = 0; j < d->nfidle && d->queued < plan->queue_max && d->roots < d->roots_max; j++) {
        const int at = order[j];
        const IMPAI_UNITREF* f = &d->fidle[at];
        int n;
        if (d->hold && d->nfbusy + d->queued >= plan->hold_fac) {
            d->out->held++;
            continue;
        }
        n = d->q->ops->build_options(d->q, f->type, d->menu, WQ_TYPES_MAX);
        t = n <= 0 || (unscored[at] && scored_idle) ? 0
            : !unlock ? choose_cons(d, d->menu, n) : on_menu(d->menu, n, unlock) ? (int)unlock : 0;
        if (t && gov_cons(d) && queue_one(d, f, t) == WQ_OK) {
            d->gov_c++;
            d->queued++;
            d->out->orders++;
            if (unlock && t == (int)unlock) {
                d->out->unlock = (uint16_t)unlock;
                d->out->unlock_plant = d->plant_t ? d->plant_t : d->plant_tw;
                unlock = 0;
            }
            continue;
        }
        if (poor[at] && !d->surplus) {
            d->out->poor++;
            continue;
        }
        if (poor[at]) {
            IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
            IMPAI_MENU m;
            m.t = mt;
            m.n = army_menu(d, f->type, mt);
            if (queue_army(d, f, share, impai_menu_classes(&m), 1)) continue;
            d->out->poor++;
            continue;
        }
        if (army && (queue_army(d, f, share, elig[at], 0) || (d->surplus && queue_army(d, f, share, elig[at], 1))))
            continue;
        if (d->surplus && d->queued < plan->queue_max && d->roots < d->roots_max) d->out->fidle++;
    }
}
