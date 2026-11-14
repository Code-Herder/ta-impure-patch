/* impai_econ.c -- ImpAI's economy manager (impai.h, the brain): the ladder that gives each idle builder its
   task, the type it builds within a category, and the factories' orders (THE GOVERNOR). The sites and the orders
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
   factory whose menu is past PLANT_MENU types is not chosen [INFERRED: a retail plant offers a few dozen]. On
   tech-up only a plant whose offer is above the seat's highest (IMPAI_DEC.ftop) is a candidate, so a seat whose
   plants already make tech 2 builds no second plant of tech 2 there. A type whose way out has been refused too
   often is passed over for fail_t (impai_build.c, THE PLANT'S FALLBACK) unless `waived`. */
#define FACTORY_LOOK 8
#define PLANT_MENU   256
static int choose_factory(IMPAI_DEC* d, const uint16_t* menu, int n, int waived)
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
        if (up && in->ti[t].offer <= d->ftop) continue;
        if (!waived && impai_plant_off(d, t)) continue;
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
   only (GPL v2: no code taken). BOUND: def_max, at most 4 (impai_brain_test.c, check_levels); the score walks the
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
            /* every plant type the choice would take passed over (THE PLANT'S FALLBACK): the cramped exit */
            if (((t = choose_factory(d, d->menu, n, 0)) && impai_build_ring(d, b, t)) ||
                (!t && (t = choose_factory(d, d->menu, n, 1)) && impai_build_plant_waived(d, b, t))) {
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
            if (d->done[IMPAI_C_MEX] > 0 && d->done[IMPAI_C_MEX] >= in->lv->open_mex &&
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

/* Idle builders in slot order, then the helpers for the first two rules and the reclaim, at most assign_max of
   them, while the decision's commands last. An idle builder whose BUILD or reclaim job this decision did not
   check waits for the check, which may record its feature as one it could not reach. */
void impai_econ_assign(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i;
    for (i = 0; i < d->nidle && d->assigned < in->lv->assign_max && d->roots < in->lv->roots; i++) {
        const IMPAI_JOB* j = &in->job[d->idle[i].slot];
        if (j->kind == IMPAI_JOB_BUILD || j->kind == IMPAI_JOB_RECLAIM || j->kind == IMPAI_JOB_RECLAIM_GO) continue;
        if (assign(d, &d->idle[i], 0)) d->assigned++;
    }
    for (i = 0; i < d->nhelpers && d->assigned < in->lv->assign_max && d->roots < in->lv->roots; i++)
        if ((d->e_stall || d->want_factory || d->want_sea || d->m_short) && assign(d, &d->helpers[i], 1)) d->assigned++;
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
    d->roots++;
    return d->q->ops->order(d->q, &c);
}

/* THE ARMY for idle factory f, one unit, while the decision's commands last. The class is A3's (impai_table.h):
   of the classes the factory offers, the one furthest below its share of the army's value -- built, building and
   queued, kspent -- (impai_buy_next), the shares those of the seat's table over its military's picture for the
   classes some idle factory offers (`share`, impai_table_shares); the type the type choice's of the class
   (impai_class_type, the predictor's pair rate against the picture's enemy composition). `cheapest`: instead the
   cheapest army type of the factory's menu, any class with a value above 0, ties to the lower type -- the
   surplus's buy (THE GOVERNOR). 1 when ordered. One unit a factory a decision: the decision comes every
   `decide` ticks, so a factory is idle at most that long between two units, and the composition answers the
   picture as it moves. BOUND: a menu past IMPAI_MENU_MAX army types is cut there. */
static int queue_army(IMPAI_DEC* d, const IMPAI_UNITREF* f, const float* share, int cheapest)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_MIL* mil = in->mil;
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    float cost[IMPAI_KS];
    int32_t type[IMPAI_KS];
    IMPAI_MENU m;
    unsigned eligible;
    int k, t = -1;
    if (d->queued >= in->lv->queue_max || d->roots >= in->lv->roots) return 0;
    m.t = mt;
    m.n = army_menu(d, f, mt);
    if (cheapest) {
        uint32_t i;
        for (i = 0; i < m.n; i++)
            if (t < 0 || mt[i].value < in->ti[t].value || (mt[i].value == in->ti[t].value && mt[i].type < t))
                t = mt[i].type;
        if (t < 0) return 0;
        k = in->ti[t].klass;
    } else {
        eligible = impai_menu_classes(&m);
        for (k = 0; k < IMPAI_KS; k++) {
            type[k] = (eligible >> k & 1u) ? impai_class_type(&mil->table, &mil->pic, &m, (unsigned)k, in->pred) : -1;
            cost[k] = type[k] > 0 && (unsigned)type[k] < in->nti ? in->ti[type[k]].value : 0.0f;
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

/* THE GOVERNOR: 1 when an idle factory may queue a constructor of workertime w now */
static int gov_cons(const IMPAI_DEC* d, int w)
{
    const IMPAI_LEVEL* lv = d->in->lv;
    const int64_t c = d->gov_c;
    if (c >= lv->cons_max || d->pressed) return 0;                          /* the cap; THE PRESSURE       */
    if (c < lv->cons_open) return 1;                                        /* THE OPENING                 */
    if ((int64_t)d->gov_a < (int64_t)lv->army_per_cons * (c - lv->cons_open + 1)) return 0;   /* THE ARMY'S SHARE */
    return d->surplus || (double)(d->gov_w + w) <= (double)d->bp_per_m * (double)d->e.metal_in;   /* THE BUILD POWER */
}

/* THE GOVERNOR. Each idle factory, in slot order, while the decision's commands last, is given one order: a
   constructor -- its best (choose_cons) -- when all four rules hold, otherwise one army unit (queue_army). A
   constructor order the factory refuses leaves it for the army in the same decision.
   - THE OPENING. The first cons_open constructors need nothing.
   - THE ARMY'S SHARE. Past them, a constructor needs army >= army_per_cons x (constructors - cons_open + 1).
   - THE BUILD POWER. The constructors' summed workertime, the new one included, stays within bp_per_m x the last
     pass's metal income: BARb's build-power cap in TA's units (CircuitAI's UpdateBuildPower, read for its design
     only, GPL v2: no code taken). The commander is not counted: it is no factory's product, it works within
     comm_r, and its 300 would hold the factories at the opening until 12.5 metal a pass.
   - THE PRESSURE. No constructor while the military's stamp lasts (impai_mil.c, THE PRESSURE).
   - A SURPLUS. With the store at SURPLUS_FRAC of its maximum or more the build power is waived: income outruns
     spending. A factory whose table buy finds nothing then buys its cheapest army type, so no factory idles
     beside a full store; one with no army type at all is counted (IMPAI_BRAIN_OUT.fidle). The share, the cap and
     the pressure still hold.
   INVARIANT, a bound on counts: at every decision that queues a constructor, (1) the constructors are below
   cons_max; (2) below cons_open, or the army share holds; (3) below cons_open, or within the build power, or in
   surplus; (4) not pressed. Every count is the census's of the same decision plus this decision's own orders, in
   slot order (THE GOVERNOR'S COUNTS, impai_brain.h): no state, no timing. Without a military or a predictor the
   factories buy no army, as at A1.
   WHY, MEASURED: the first army unit came at a median GameTime of 13 500-15 000 against a first factory at 3 300-
   3 600, and at 12 000 the seats held 5-6 constructors and no army at every level (196 seat-games of the bar,
   2026-11-13); a constructor takes a factory 1 485-1 745 ticks against 436-521 for a raider; in a game against a
   human on Comet Catcher, 95 % of the factory-ticks went to constructors and 27 of them were lost. BOUND: the
   factories looked at are the census's, IMPAI_IDLE_CAP, each one menu read for its constructor and at most two
   for its army. */
void impai_econ_factories(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_LEVEL* lv = in->lv;
    const int army = in->mil && in->pred;
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    float share[IMPAI_KS];
    unsigned buildable = 0;
    int i;
    memset(share, 0, sizeof share);
    if (army) {
        for (i = 0; i < d->nfidle; i++) {
            IMPAI_MENU m;
            m.t = mt;
            m.n = army_menu(d, &d->fidle[i], mt);
            buildable |= impai_menu_classes(&m);
        }
        if (buildable) impai_table_shares(&in->mil->table, &in->mil->pic, buildable, share);
    }
    for (i = 0; i < d->nfidle && d->queued < lv->queue_max && d->roots < lv->roots; i++) {
        const IMPAI_UNITREF* f = &d->fidle[i];
        const int n = d->q->ops->build_options(d->q, f->type, d->menu, WQ_TYPES_MAX);
        const int t = n > 0 ? choose_cons(d, d->menu, n) : 0;
        if (t && gov_cons(d, in->ti[t].workertime) && queue_one(d, f, t) == WQ_OK) {
            d->gov_c++;
            d->gov_w += in->ti[t].workertime;
            d->queued++;
            d->out->orders++;
            continue;
        }
        if (army && (queue_army(d, f, share, 0) || (d->surplus && queue_army(d, f, share, 1)))) continue;
        if (d->surplus && d->queued < lv->queue_max && d->roots < lv->roots) d->out->fidle++;
    }
}
