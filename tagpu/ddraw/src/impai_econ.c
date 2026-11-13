/* impai_econ.c -- ImpAI's economy manager (impai.h, the brain): the ladder that gives each idle builder its
   task, the type it builds within a category, and the factories' constructor queues. The sites and the orders
   are impai_build.c's. Engine-free, no statics. */
#include <float.h>
#include <string.h>
#include "impai_brain.h"
#include "tagpu_simreg.h"

/* The economy manager. OWNER: the brain calls it only inside a live ImpAI seat's turn, which plays no puppet
   seat (tagpu_impai.c, impai_play), so a puppet's orders come from the recording. */
SIM_FEATURE(impai_econ, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval");

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

/* a factory: the least value among those whose own menu offers a constructor that can build an extractor, a
   water-only plant only on a wet anchor (a shipyard: CORSY's value, 612.5, is the least of CORE's four plants);
   at most FACTORY_LOOK candidates are examined, each costing a menu read and one per constructor on it. A
   factory whose menu is past PLANT_MENU types is not chosen [INFERRED: a retail plant offers a few dozen]. */
#define FACTORY_LOOK 8
#define PLANT_MENU   256
static int choose_factory(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    uint16_t plant[PLANT_MENU];
    const int wet = anchor_wet(d);
    const int up = d->want_tech && !d->want_factory;
    int k, j, best = 0, looked = 0;
    for (k = 0; k < n && looked < FACTORY_LOOK; k++) {
        const int t = menu[k];
        int np, ok = 0;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_FACTORY) continue;
        if (up && in->ti[t].tech <= d->ftop) continue;
        if ((in->ti[t].flags & IMPAI_TF_WATER) && !wet) continue;
        if (best && (in->ti[t].value > in->ti[best].value || (in->ti[t].value == in->ti[best].value && t > best))) continue;
        looked++;
        np = d->q->ops->build_options(d->q, t, plant, (int)(sizeof plant / sizeof plant[0]));
        for (j = 0; j < np && !ok; j++)
            ok = plant[j] < in->nti && in->ti[plant[j]].cat == IMPAI_C_CONS && builds_mex(d, plant[j]);
        if (ok) best = t;
    }
    return best;
}

/* THE SEA PLANT's type: the least value among the menu's water factories (impai_water_plant) whose own menu
   offers a sea army type (klass IMPAI_K_SRAIDER..IMPAI_K_SAA, a value above 0), ties to the lower type; at most
   FACTORY_LOOK candidates examined, each a menu read of at most PLANT_MENU types. Writes neither d->menu nor
   d->menu2. Sea army types are bought only where a factory offers them (factories_army), so this is the plant
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
   site falls through. A helper (a builder assisting) answers the first two rules only. The second is the
   factory: while the seat wants its sea plant (impai_brain.c, economy) a builder whose menu offers one searches
   for its site, and a land plant waits; a search that finds no site for any sea plant ends the want, so the
   next factory is a land one, in the same decision. A builder with no sea plant on its menu builds no factory
   while the want lasts: another builder offers one (impai_econ_sea_able). An extractor's site is a patch, or on
   a metal map the ring. A constructor explores only when no patch is left for it -- none free, reachable on its
   class and unthreatened -- so a patch it can never reach does not keep it home. */
static int assign(IMPAI_DEC* d, const IMPAI_UNITREF* b, int helper)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* bt = &in->ti[b->type];
    const int n = d->q->ops->build_options(d->q, b->type, d->menu, WQ_TYPES_MAX);
    const float keep = d->e_per_m * d->pm_in;
    int t;
    if (n > 0) {
        if (d->e_stall && (t = choose_energy(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->pe_in += in->ti[t].egain;
            return 1;
        }
        if (d->want_sea && (t = choose_sea_plant(d, d->menu, n))) {
            const int y = impai_build_sea_plant(d, b, t);
            if (y > 0) {
                d->want_sea = d->want_factory = d->want_tech = 0;
                return 1;
            }
            if (y < 0) d->want_sea = 0;
        }
        if ((d->want_factory || d->want_tech) && !d->want_sea && (t = choose_factory(d, d->menu, n)) &&
            impai_build_ring(d, b, t)) {
            d->want_factory = d->want_tech = 0;
            return 1;
        }
        if (helper) return 0;
        if (d->want_aa && (t = choose_aa(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->want_aa = 0;
            return 1;
        }
        if ((t = choose_mex(d, d->menu, n)) &&
            (metal_map(d) ? impai_build_ring(d, b, t) : impai_build_spot(d, b, t)))
            return 1;
        if (d->pe_in < (keep > 1.1f * d->e.energy_out ? keep : 1.1f * d->e.energy_out) &&
            (t = choose_energy(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->pe_in += in->ti[t].egain;
            return 1;
        }
        if (d->e.energy > 0.8f * d->e.energy_max && (t = choose_maker(d, d->menu, n)) &&
            d->e.energy_in - d->e.energy_out > in->ti[t].egain && impai_build_ring(d, b, t))
            return 1;
    }
    if (helper) return 0;
    if (bt->cat == IMPAI_C_CONS && !d->explorer_live && !impai_spot_pick(d, b) && impai_build_explore(d, b)) return 1;
    return impai_build_assist(d, b);
}

/* Idle builders in slot order, then the helpers for the first two rules, at most assign_max of them, while the
   decision's commands last. An idle builder whose BUILD job this decision did not check waits for the check. */
void impai_econ_assign(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i;
    for (i = 0; i < d->nidle && d->assigned < in->lv->assign_max && d->roots < in->lv->roots; i++) {
        const IMPAI_JOB* j = &in->job[d->idle[i].slot];
        if (j->kind == IMPAI_JOB_BUILD) continue;
        if (assign(d, &d->idle[i], 0)) d->assigned++;
    }
    for (i = 0; i < d->nhelpers && d->assigned < in->lv->assign_max && d->roots < in->lv->roots; i++)
        if ((d->e_stall || d->want_factory || d->want_sea) && assign(d, &d->helpers[i], 1)) d->assigned++;
}

/* ---- 5. the factories ---- */

/* factory f's army types into mt (at most IMPAI_MENU_MAX): its menu's types with an army class and a value */
static uint32_t army_menu(IMPAI_DEC* d, const IMPAI_UNITREF* f, IMPAI_MENU_TYPE* mt)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int n = d->q->ops->build_options(d->q, f->type, d->menu, WQ_TYPES_MAX);
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

/* THE ARMY: each idle factory the constructors did not take queues one army unit, while the decision's
   commands last. The class is A3's (impai_table.h): the shares of the seat's table over its military's
   picture, for the classes some idle factory offers (impai_table_shares), and of those the factory offers the
   one furthest below its share of the army's value -- built, building and queued, kspent -- (impai_buy_next);
   the type of the class the type choice's (impai_class_type, the predictor's pair rate against the picture's
   enemy composition). One unit a factory a decision: the decision comes every `decide` ticks, so a factory is
   idle at most that long between two units, and the composition answers the picture as it moves. BOUND: a
   menu past IMPAI_MENU_MAX army types is cut there; the factories looked at are the census's, IMPAI_IDLE_CAP. */
static void factories_army(IMPAI_DEC* d, const uint8_t* taken)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_MIL* mil = in->mil;
    const IMPAI_LEVEL* lv = in->lv;
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    float share[IMPAI_KS], cost[IMPAI_KS];
    int32_t type[IMPAI_KS];
    unsigned buildable = 0;
    int i, k;
    if (!mil || !in->pred) return;
    for (i = 0; i < d->nfidle; i++) {
        IMPAI_MENU m;
        if (taken[i]) continue;
        m.t = mt;
        m.n = army_menu(d, &d->fidle[i], mt);
        buildable |= impai_menu_classes(&m);
    }
    if (!buildable) return;
    impai_table_shares(&mil->table, &mil->pic, buildable, share);
    for (i = 0; i < d->nfidle && d->queued < lv->queue_max && d->roots < lv->roots; i++) {
        const IMPAI_UNITREF* f = &d->fidle[i];
        IMPAI_MENU m;
        unsigned eligible;
        WQ_CMD c;
        if (taken[i]) continue;
        m.t = mt;
        m.n = army_menu(d, f, mt);
        eligible = impai_menu_classes(&m);
        for (k = 0; k < IMPAI_KS; k++) {
            type[k] = (eligible >> k & 1u) ? impai_class_type(&mil->table, &mil->pic, &m, (unsigned)k, in->pred) : -1;
            cost[k] = type[k] > 0 && (unsigned)type[k] < in->nti ? in->ti[type[k]].value : 0.0f;
        }
        if ((k = impai_buy_next(share, d->kspent, cost, eligible, FLT_MAX)) < 0 || type[k] <= 0) continue;
        memset(&c, 0, sizeof c);
        c.verb = WQ_CMD_QUEUE;
        c.unit = f->slot;
        c.target = (uint16_t)WQ_NONE;
        c.type = (uint16_t)type[k];
        c.count = 1;
        d->roots++;
        if (d->q->ops->order(d->q, &c) != WQ_OK) continue;
        d->kspent[k] += cost[k];
        d->queued++;
        d->out->orders++;
        d->out->army++;
    }
}

/* The constructors kept: min(cons_max, cons_base + floor(metal_in / cons_per_m)). Each idle factory in slot
   order queues one of its best constructor while the seat has fewer, counting those queued, at most
   queue_max commands; the idle factories left queue army (factories_army). */
void impai_econ_factories(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_LEVEL* lv = in->lv;
    const float more = d->cons_per_m > 0 ? d->e.metal_in / d->cons_per_m : 0.0f;
    const int extra = more > 0 ? (more < 255.0f ? (int)more : 255) : 0;
    const int target = lv->cons_base + extra < lv->cons_max ? lv->cons_base + extra : lv->cons_max;
    uint8_t taken[IMPAI_IDLE_CAP];
    int i;
    memset(taken, 0, sizeof taken);
    for (i = 0; i < d->nfidle && d->queued < lv->queue_max && d->roots < lv->roots &&
                d->count[IMPAI_C_CONS] + d->queued_cons < target; i++) {
        const IMPAI_UNITREF* f = &d->fidle[i];
        const int n = d->q->ops->build_options(d->q, f->type, d->menu, WQ_TYPES_MAX);
        const int t = n > 0 ? choose_cons(d, d->menu, n) : 0;
        WQ_CMD c;
        if (!t) continue;
        memset(&c, 0, sizeof c);
        c.verb = WQ_CMD_QUEUE;
        c.unit = f->slot;
        c.target = (uint16_t)WQ_NONE;
        c.type = (uint16_t)t;
        c.count = 1;
        d->roots++;
        taken[i] = 1;
        if (d->q->ops->order(d->q, &c) != WQ_OK) continue;
        d->queued++;
        d->queued_cons++;
        d->out->orders++;
    }
    factories_army(d, taken);
}
