#include <stdlib.h>
#include <string.h>
#include "tasim_roles.h"

/* INVARIANT: every def and weapon bit the classifier tests is one both role fillers copy (tasim_roles.h,
   TASIM_ROLE_F241, TASIM_ROLE_F245, TASIM_ROLE_WF): a bit outside the masks reads 0 on both sides. Impure's
   TASIM_WF_NOTTOAIR is bit 31, which each filler sets from its own key read beside the mask. */
_Static_assert(((TASIM_F241_BUILDER | TASIM_F241_CANFLY | TASIM_F241_CANHOVER | TASIM_F241_AMPHIBIOUS |
                 TASIM_F241_KAMIKAZE | TASIM_F241_ANTIWEAPONS) & ~TASIM_ROLE_F241) == 0,
               "the classifier tests only +0x241 bits the role fillers copy");
_Static_assert(((TASIM_F245_CANMOVE | TASIM_F245_COMMANDER) & ~TASIM_ROLE_F245) == 0,
               "the classifier tests only +0x245 bits the role fillers copy");
_Static_assert(((TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | TASIM_WF_INTERCEPTOR | TASIM_WF_TOAIR | TASIM_WF_WATER |
                 TASIM_WF_BALLISTIC | TASIM_WF_VLAUNCH | TASIM_WF_PARALYZER) & ~TASIM_ROLE_WF) == 0,
               "the classifier tests only +0x111 bits the role fillers copy");

/* The thresholds, in one table: each is a percentile over the mobile non-builder units of the
   unit's layer group, so a mod with faster or dearer units moves them with it. */
static const struct {
    double raider_speed;        /* raider: speed at or above this percentile             */
    double scout_speed;         /* scout: speed at or above this ...                     */
    double scout_value;         /* ... and value at or below this one                    */
    double artillery_range;     /* artillery: ground range at or above this percentile   */
} TH = { 0.50, 0.75, 0.10, 0.75 };

static const char* const ROLE_NAMES[TASIM_ROLES] = {
    "builder", "defence", "other", "antiair", "scout", "artillery", "raider", "assault"
};
static const char* const LAYER_NAMES[TASIM_LAYERS] = { "land", "sea", "air" };

const char* tasim_roles_name(TASIM_ROLE r)
{
    return (unsigned)r < TASIM_ROLES ? ROLE_NAMES[r] : "?";
}

const char* tasim_roles_layer_name(TASIM_LAYER l)
{
    return (unsigned)l < TASIM_LAYERS ? LAYER_NAMES[l] : "?";
}

static int cmp_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

double tasim_roles_quantile(double* x, int n, double q)
{
    double pos, frac;
    int lo;
    if (n <= 0) return 0.0;
    qsort(x, (size_t)n, sizeof *x, cmp_double);
    pos = q * (double)(n - 1);
    lo = (int)pos;
    if (lo >= n - 1) return x[n - 1];
    frac = pos - (double)lo;
    return x[lo] + frac * (x[lo + 1] - x[lo]);
}

double tasim_roles_maker_ratio(const TASIM_ROLE_IN* in, int n)
{
    double best_cost = 0.0, best = 0.0;
    int i, found = 0;
    for (i = 0; i < n; i++) {
        double r;
        if (!in[i].makesmetal || in[i].energyuse <= 0.0f) continue;
        r = (double)in[i].energyuse / (double)in[i].makesmetal;
        if (!found || in[i].cost_metal < best_cost || (in[i].cost_metal == best_cost && r < best)) {
            best_cost = in[i].cost_metal;
            best = r;
            found = 1;
        }
    }
    return best;
}

/* A weapon is regular when it fires on its own at what it meets: not a D-gun's commandfire,
   not a stockpile, not an interceptor. */
static int regular(const TASIM_ROLE_WEAPON* w)
{
    return !(w->flags & (TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | TASIM_WF_INTERCEPTOR));
}

/* A PARALYSER DEALS NO DAMAGE. A weapon with `paralyzer` (+0x111 bit 7, parsed at 0x42EBBB) hits with damage
   kind 2 (0x499E20..0x499E32), and the damage apply 0x489CE0 leaves a kind-2 hit by 0x489DF5 without touching
   hit points (engine/weapons.md, engine/network.md; DISASSEMBLED): its `damage` goes to the paralysis alone
   [INFERRED: its length]. So its [DAMAGE] figures are no damage per second, and its range is no reach of one: a regular
   paralyser makes its type armed for the role (a standing type stays a defence) and adds nothing to dps_ground,
   dps_air, dps_water, range_ground or the artillery test. A type whose regular weapons all paralyse has damage
   0 everywhere the brain reads it -- no army class (impai_table.c, NO ARMY CLASS WITHOUT DAMAGE), no threat
   row -- and a mixed type is its other weapons'. The same rule stands in THE GROUND REACH (impai_maps.c) and
   in the fight predictor's damage (impai_fight.c, damage_on). */
static int paralyses(const TASIM_ROLE_WEAPON* w)
{
    return (w->flags & TASIM_WF_PARALYZER) != 0;
}

static double dps(double damage, const TASIM_ROLE_WEAPON* w)
{
    int burst = w->burst > 1 ? w->burst : 1;
    if (w->reload_ticks <= 0.0f) return 0.0;
    return damage * (double)burst / (double)w->reload_ticks;
}

/* A slot is anti-air when the unit's bad-target category for it is NOTAIR (the slot passes over
   every target that is not an aircraft) or its weapon is a toairweapon, and the weapon does not
   carry Impure's `nottoair`, which refuses every flying target; its damage against air is then the
   weapon's [DAMAGE] against the mod's aircraft. Only slots 0..2 have a bad-target category
   (+0x231, +0x235, +0x239). */
int tasim_roles_slot_antiair(const TASIM_ROLE_IN* u, int k)
{
    if (k < 0 || k >= TASIM_ROLE_WEAPONS || (u->w[k].flags & TASIM_WF_NOTTOAIR)) return 0;
    return (k < TASIM_ROLE_BADSLOTS && u->slot_notair[k]) || (u->w[k].flags & TASIM_WF_TOAIR);
}

static TASIM_LAYER layer_of(const TASIM_ROLE_IN* u)
{
    if (u->flags241 & TASIM_F241_CANFLY) return TASIM_LAYER_AIR;
    if (!(u->flags241 & (TASIM_F241_CANHOVER | TASIM_F241_AMPHIBIOUS)) && u->minwaterdepth > 0)
        return TASIM_LAYER_SEA;
    return TASIM_LAYER_LAND;
}

/* has_regular: a regular slot, a paralyser included, which arms the type for its role; has_damaging: a regular
   slot that deals damage; first_regular_aa: the first of those is an anti-air slot. THE PRIMARY WEAPON is the
   first regular slot that deals damage: a paralyser deals none (A PARALYSER DEALS NO DAMAGE), so a ground
   paralyser in front of an anti-air missile leaves the missile primary and the type anti-air, and an anti-air
   paralyser in front of a ground gun leaves the type its ground role. */
typedef struct {
    int has_regular, has_damaging, has_any, first_regular_aa, ground_arty;
} Arms;

static Arms derive(const TASIM_ROLE_IN* u, TASIM_ROLE_OUT* o)
{
    Arms a = { 0, 0, 0, 0, 0 };
    int k, n = u->nweapons < TASIM_ROLE_WEAPONS ? u->nweapons : TASIM_ROLE_WEAPONS;
    o->layer = layer_of(u);
    o->value = (double)u->cost_metal + (double)u->cost_energy / TASIM_ROLES_R;
    o->speed = (double)u->maxvelocity_fx / 65536.0;
    o->flags = 0;
    /* bmcode 1 is the engine's own test for a mover (the creates 0x4860BB, 0x4862CF; a canfly type branches off
       before the resolver asks for it); 0 is a building (bit 29, 0x485A9E), and any other value, the raw low
       byte of atoi (0x42C422), is neither and cannot be ordered to chase (engine/units.md). */
    if ((u->flags245 & TASIM_F245_CANMOVE) && u->bmcode == 1) o->flags |= TASIM_RF_MOBILE;
    if (u->flags245 & TASIM_F245_COMMANDER) o->flags |= TASIM_RF_COMMANDER;
    if (u->transportcapacity) o->flags |= TASIM_RF_TRANSPORT;
    if (u->flags241 & TASIM_F241_KAMIKAZE) o->flags |= TASIM_RF_KAMIKAZE;
    if (u->flags241 & TASIM_F241_ANTIWEAPONS) o->flags |= TASIM_RF_ANTIWEAPONS;
    if (u->radar) o->flags |= TASIM_RF_RADAR;
    if (u->jammer) o->flags |= TASIM_RF_JAMMER;
    o->dps_ground = o->dps_air = o->dps_water = 0.0;
    o->range_ground = 0;
    for (k = 0; k < n; k++) {
        const TASIM_ROLE_WEAPON* w = &u->w[k];
        a.has_any = 1;
        if (!regular(w)) continue;
        a.has_regular = 1;
        if (paralyses(w)) continue;
        if (!a.has_damaging) a.first_regular_aa = tasim_roles_slot_antiair(u, k);
        a.has_damaging = 1;
        if (tasim_roles_slot_antiair(u, k)) {
            o->dps_air += dps((double)w->damage_air, w);
            continue;
        }
        o->dps_ground += dps((double)w->damage_default, w);
        if (w->flags & TASIM_WF_WATER) o->dps_water += dps((double)w->damage_default, w);
        if (w->range > o->range_ground) o->range_ground = w->range;
        if (w->flags & (TASIM_WF_BALLISTIC | TASIM_WF_VLAUNCH)) a.ground_arty = 1;
    }
    return a;
}

/* The population a threshold is taken over: mobile, not a builder. */
static int in_population(const TASIM_ROLE_IN* u, const TASIM_ROLE_OUT* o)
{
    return (o->flags & TASIM_RF_MOBILE) && !(u->flags241 & TASIM_F241_BUILDER);
}

static void thresholds(const TASIM_ROLE_IN* in, int n, const TASIM_ROLE_OUT* out,
                       TASIM_ROLE_Q* q, double* x)
{
    int L, i, m;
    for (L = 0; L < TASIM_LAYERS; L++) {
        m = 0;
        for (i = 0; i < n; i++) if (out[i].layer == (TASIM_LAYER)L && in_population(&in[i], &out[i])) x[m++] = out[i].speed;
        q->population[L] = m;
        q->speed50[L] = tasim_roles_quantile(x, m, TH.raider_speed);
        q->speed75[L] = tasim_roles_quantile(x, m, TH.scout_speed);
        m = 0;
        for (i = 0; i < n; i++) if (out[i].layer == (TASIM_LAYER)L && in_population(&in[i], &out[i])) x[m++] = out[i].value;
        q->value10[L] = tasim_roles_quantile(x, m, TH.scout_value);
        m = 0;
        for (i = 0; i < n; i++) if (out[i].layer == (TASIM_LAYER)L && in_population(&in[i], &out[i])) x[m++] = (double)out[i].range_ground;
        q->range75[L] = tasim_roles_quantile(x, m, TH.artillery_range);
    }
}

static TASIM_ROLE role_of(const TASIM_ROLE_IN* u, TASIM_ROLE_OUT* o, Arms a, const TASIM_ROLE_Q* q)
{
    int mobile = (o->flags & TASIM_RF_MOBILE) != 0;
    TASIM_LAYER L = o->layer;
    if (u->flags241 & TASIM_F241_BUILDER) {
        if (!mobile) o->flags |= TASIM_RF_FACTORY;
        return TASIM_ROLE_BUILDER;
    }
    if (!mobile && a.has_regular) return TASIM_ROLE_DEFENCE;
    if (!mobile || (u->transportcapacity && !a.has_regular) ||
        (u->flags241 & TASIM_F241_KAMIKAZE) || (a.has_any && !a.has_regular))
        return TASIM_ROLE_OTHER;
    /* anti-air: the primary weapon (Arms) is an anti-air slot and it hurts aircraft; a unit
       whose first damaging weapon is for the ground and which carries a missile besides stays a
       ground role */
    if (a.first_regular_aa && o->dps_air > 0.0) return TASIM_ROLE_ANTIAIR;
    if (!a.has_regular) return TASIM_ROLE_SCOUT;
    if (a.ground_arty && (double)o->range_ground >= q->range75[L]) return TASIM_ROLE_ARTILLERY;
    if (o->speed >= q->speed75[L] && o->value <= q->value10[L]) return TASIM_ROLE_SCOUT;
    if (o->speed >= q->speed50[L]) return TASIM_ROLE_RAIDER;
    return TASIM_ROLE_ASSAULT;
}

/* the scratch space: n doubles for the percentiles, then each unit's Arms, its distance and a
   queue slot */
size_t tasim_roles_work(int n)
{
    return (size_t)(n > 0 ? n : 1) * (sizeof(double) + sizeof(Arms) + 2 * sizeof(int));
}

/* tech(u) = max(1, the least number of factories strictly before u on a build chain from a
   commander): a breadth-first walk from every commander in which an edge out of a factory costs
   1 and any other edge 0, one cost layer at a time. Layer d closes over its 0-cost edges, each
   node entering the queue once, when its distance becomes d; an edge out of a factory marks d + 1,
   which a 0-cost edge met later in the same layer lowers to d. The marks of d + 1 are the next
   layer's queue. BOUND: a node is queued at most once per layer and the queue holds n. */
static int tech_levels(const TASIM_ROLE_IN* in, int n, TASIM_ROLE_OUT* out, int* dist, int* queue)
{
    int i, layer, head, tail, ncmd = 0;
    for (i = 0; i < n; i++) dist[i] = -1;
    tail = 0;
    for (i = 0; i < n; i++)
        if (in[i].flags245 & TASIM_F245_COMMANDER) { dist[i] = 0; queue[tail++] = i; ncmd++; }
    if (!ncmd) return -1;
    for (layer = 0; tail > 0 && layer <= n; layer++) {
        int next = 0;
        /* close this layer over 0-cost edges, collecting 1-cost successors for the next */
        for (head = 0; head < tail; head++) {
            int u = queue[head], k;
            int factory = (in[u].flags241 & TASIM_F241_BUILDER) && !(out[u].flags & TASIM_RF_MOBILE);
            for (k = 0; k < in[u].nbuild; k++) {
                int v = in[u].build[k];
                if (v < 0 || v >= n) return -2;
                if (dist[v] >= 0 && dist[v] <= layer + factory) continue;
                if (factory) {
                    dist[v] = layer + 1;
                } else {
                    dist[v] = layer;
                    queue[tail++] = v;
                }
            }
        }
        for (i = 0; i < n; i++) if (dist[i] == layer + 1) queue[next++] = i;
        tail = next;
    }
    for (i = 0; i < n; i++) out[i].tech = dist[i] < 0 ? 0 : (dist[i] > 1 ? dist[i] : 1);
    return 0;
}

int tasim_roles_classify(const TASIM_ROLE_IN* in, int n, TASIM_ROLE_OUT* out, TASIM_ROLE_Q* q,
                         void* work)
{
    int m = n > 0 ? n : 1, i;
    double* x = (double*)work;
    Arms* arms = (Arms*)(x + m);
    int* dist = (int*)(arms + m);
    int* queue = dist + m;
    memset(q, 0, sizeof *q);
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) arms[i] = derive(&in[i], &out[i]);
    thresholds(in, n, out, q, x);
    for (i = 0; i < n; i++) out[i].role = role_of(&in[i], &out[i], arms[i], q);
    return tech_levels(in, n, out, dist, queue);
}
