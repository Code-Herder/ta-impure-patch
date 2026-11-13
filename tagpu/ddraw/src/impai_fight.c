/* impai_fight.c -- ImpAI's fight predictor (impai_fight.h).

   WHAT IT MODELS. Two groups of unit types, each a list of (type, count, health), whose centroids are a
   distance apart, fight in steps of IMPAI_FSTEP ticks, IMPAI_FSTEPS at most:
     1. each side that closes moves at its slowest mobile type's maxvelocity x `close`, until the distance is
        within the shortest of its types' ranges against a target they can hurt, so its long-range types
        fire first and its short-range ones join once it has closed;
     2. each type spreads its fire over the enemy types in its reach: (1 - focus) by their counts and focus
        on the one it kills quickest (least hp over its rate on it); an area weapon's damage reaches
        `splash` x the overlap more units, the overlap its area over the target's spacing, squared, at the
        mean of the engine's falloff over the area (e + (1 - e)/6: 0x49A3B6's e + (1 - e)(d/R - 1)^2 averaged
        over a disc), never more than the other units of the type;
     3. the damage on a type pools; a unit dies for each whole hp the pool holds, never more than the shots
        aimed at the type so far (a shot kills at most one unit), and the pool keeps the remainder;
     4. it ends when a side has no unit, at the last step, or when no type of either side reaches the other
        and neither side closes.
   A unit's rate on another is the engine's numbers through the fitted hit probability (pair_rate). Both
   sides' fire in a step is computed from the counts the step began with, and every side's figure by the
   same code, so a mirror fight's margin is 0 exactly and swapping the sides negates it.

   WHAT IT DOES NOT MODEL: terrain, pathing and blocking, line of fire, turning and aiming time, a
   formation's depth, repair and regeneration, the target choice of the engine's own scan, a weapon's
   minimum range and the order a fight's units arrive in. Those are what the coefficients absorb, fitted
   to the engine's fights (tools/impai/predfit.py); the predictor is a model of the engine, never a copy.

   ROUNDING. Every value is a float rounded once per operation (SSE2, no contraction: the Makefile's
   IMPAI rule), evaluated in the order written; the only library call is sqrtf, which IEEE 754 rounds
   exactly, and every exponential goes through impai_expf. */
#include <math.h>
#include <string.h>
#include "impai_fight.h"

#define TWO_PI      6.28318530717958647692f
#define SPACING_PAD 32.0f      /* a unit's spacing: its size and this much room                        */

int impai_fight_kind(uint32_t flags)
{
    if (flags & TASIM_WF_BEAM) return IMPAI_FK_BEAM;
    if (flags & TASIM_WF_DROPPED) return IMPAI_FK_DROPPED;
    if (flags & (TASIM_WF_GUIDANCE | TASIM_WF_TRACKS | TASIM_WF_VLAUNCH)) return IMPAI_FK_GUIDED;
    if (flags & TASIM_WF_BALLISTIC) return IMPAI_FK_BALLISTIC;
    return IMPAI_FK_DIRECT;
}

/* exp(x) = 2^k e^r: k the nearest whole number to x / ln 2, r = x - k ln 2 taken in two parts (Cody and
   Waite: ln 2's high part 0x1.62e4p-1 has 15 significant bits, so k ln2_hi is exact for |k| <= 127, and
   the low part is the float nearest ln 2 less it), e^r by its Taylor
   polynomial of degree 7 on |r| <= ln 2 / 2 (the remainder below r^8 / 8! < 2^-27 of e^r, under the float's
   own rounding), and 2^k written into the exponent bits. x is held to [-87, 88], where 2^k stays a normal
   float, so the answer is positive and finite. */
float impai_expf(float x)
{
    const float ln2_hi = 0.693145751953125f, ln2_lo = 1.42860676533018e-06f, inv_ln2 = 1.44269502162933f;
    union { float f; uint32_t u; } two_k;
    float t, r, e;
    int32_t k;
    if (x != x) return x;
    if (x > 88.0f) x = 88.0f;
    if (x < -87.0f) x = -87.0f;
    t = x * inv_ln2;
    k = (int32_t)(t >= 0.0f ? t + 0.5f : t - 0.5f);
    r = (x - (float)k * ln2_hi) - (float)k * ln2_lo;
    e = 1.0f + r * (1.0f + r * (0.5f + r * (0.166666671633720f + r * (0.0416666679084301f +
        r * (0.00833333376795053f + r * (0.00138888892251998f + r * 0.000198412701138295f))))));
    two_k.u = (uint32_t)(k + 127) << 23;
    return e * two_k.f;
}

static float sigmoid(float z)
{
    return 1.0f / (1.0f + impai_expf(-z));
}

float impai_fight_pwin(const IMPAI_FIT* p, float m)
{
    return sigmoid(m / (p->tau > 1e-6f ? p->tau : 1e-6f));
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* a unit's size in px: 16 px a footprint cell, the square root of its cells */
static float size_px(const IMPAI_FTYPE* t)
{
    const unsigned cells = (unsigned)t->fx * t->fz;
    return 16.0f * sqrtf((float)(cells ? cells : 1u));
}

/* weapon w's damage on type j: its [DAMAGE] row for j by binary search over its rows, sorted by type, else
   its default. The caller has bounded the span to the pool. */
static int32_t damage_on(const IMPAI_FCONTENT* c, const IMPAI_FWEAPON* w, unsigned j)
{
    unsigned lo = w->d0, hi = w->d0 + w->nd;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (c->d[mid].type < j) lo = mid + 1;
        else hi = mid;
    }
    return lo < w->d0 + w->nd && c->d[lo].type == j ? c->d[lo].dmg : (int32_t)w->dmg;
}

/* What one unit of type i does to units of type j: r its damage a tick, q its shots a tick that hit, ov
   its area's overlap factor (the step's splash), range the longest range among its weapons that hurt j. */
typedef struct PAIR { float r, q, ov; int32_t range; } PAIR;

/* THE PAIR RATE. Weapon w of i hurts j when the classifier's rules let it (tasim_roles.h): an aircraft only
   through an anti-air slot (IMPAI_FTYPE.air), anything else not through a toairweapon, and a submerged unit
   only through a water weapon; and its damage on j, the [DAMAGE] row else the default, is positive. Its hit
   probability is sigma(h0 + h1 lead + h2 spread + h3 area) with its kind's coefficients, where, j's size
   s = 16 sqrt(fx fz) px,
     lead   = j's speed x the shot's flight time at its range (range / velocity; 0 for a beam or an instant
              weapon) / s, how far j moves while the shot flies;
     spread = (accuracy + sprayangle) / 65536 x 2 pi x range / s, the arc the shot can miss by at its range;
     area   = areaofeffect / s.
   r = sum of burst x damage x p / reload, q = sum of burst x p / reload, both with burst and reload at least
   1. -1 when a weapon index or its damage span is past the content. */
static int pair_rate(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, unsigned i, unsigned j, PAIR* o)
{
    const IMPAI_FTYPE* ti = &c->t[i];
    const IMPAI_FTYPE* tj = &c->t[j];
    const float size = size_px(tj), spacing = size + SPACING_PAD;
    const float speed = (tj->flags & IMPAI_FT_MOBILE) ? (float)tj->speed / 65536.0f : 0.0f;
    float r = 0.0f, q = 0.0f, wov = 0.0f;
    int32_t range = 0;
    int k;
    for (k = 0; k < IMPAI_FW_SLOTS; k++) {
        const unsigned wi = ti->w[k];
        const IMPAI_FWEAPON* w;
        const float* h;
        float flight, lead, spread, area, hit, shots, rate, edge, over;
        int32_t dmg;
        if (!wi) continue;
        if (wi >= c->nw) return -1;
        w = &c->w[wi];
        if (w->kind >= IMPAI_FKINDS || w->d0 > c->nd || w->nd > c->nd - w->d0) return -1;
        if (tj->flags & IMPAI_FT_FLIES) {
            if (!(ti->air >> k & 1u)) continue;
        } else {
            if (w->flags & TASIM_WF_TOAIR) continue;
            if ((tj->flags & IMPAI_FT_SUBMERGED) && !(w->flags & TASIM_WF_WATER)) continue;
        }
        if ((dmg = damage_on(c, w, j)) <= 0) continue;
        flight = w->kind != IMPAI_FK_BEAM && w->velocity > 0 ? (float)w->range / ((float)w->velocity / 65536.0f)
                                                              : 0.0f;
        lead = speed * flight / size;
        spread = ((float)w->accuracy + (float)w->spray) / 65536.0f * TWO_PI * (float)w->range / size;
        area = (float)w->aoe / size;
        h = p->hit[w->kind];
        hit = sigmoid(h[IMPAI_FH_BIAS] + h[IMPAI_FH_LEAD] * lead + h[IMPAI_FH_SPREAD] * spread +
                      h[IMPAI_FH_AREA] * area);
        shots = (float)(w->burst > 1 ? w->burst : 1) * hit / (float)(w->reload > 1 ? w->reload : 1);
        rate = shots * (float)dmg;
        edge = clamp01(w->edge);
        over = (float)w->aoe / spacing;
        r += rate;
        q += shots;
        wov += rate * (over * over * (edge + (1.0f - edge) / 6.0f));
        if (w->range > range) range = w->range;
    }
    o->r = r;
    o->q = q;
    o->ov = r > 0.0f ? wov / r : 0.0f;
    o->range = range;
    return 0;
}

int impai_fight_rate(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, unsigned i, unsigned j, float* rate,
                     int32_t* range)
{
    PAIR x;
    if (i >= c->nt || j >= c->nt || pair_rate(c, p, i, j, &x)) return -1;
    *rate = x.r;
    *range = x.range;
    return 0;
}

float impai_fight_rate_of(const void* ctx, uint16_t i, uint16_t j)
{
    const IMPAI_FCTX* x = (const IMPAI_FCTX*)ctx;
    float rate;
    int32_t range;
    return impai_fight_rate(x->c, x->p, i, j, &rate, &range) ? 0.0f : rate;
}

/* One side as the steps see it: per entry its type, its live count, the effective hp of a unit (its type's
   hp at the entry's health), the damage pooled and the shots aimed at it not yet spent on a kill. */
typedef struct SIDE {
    unsigned ne;
    uint16_t type[IMPAI_FSIDE_TYPES];
    int32_t  n[IMPAI_FSIDE_TYPES];
    float    hpe[IMPAI_FSIDE_TYPES], pool[IMPAI_FSIDE_TYPES], spool[IMPAI_FSIDE_TYPES];
    float    hv0;
} SIDE;

/* its value x health fraction: value x (n x hpe - pool) / hp over the entries */
static float side_hv(const IMPAI_FCONTENT* c, const SIDE* s)
{
    float hv = 0.0f;
    unsigned e;
    for (e = 0; e < s->ne; e++) {
        const IMPAI_FTYPE* t = &c->t[s->type[e]];
        const float left = (float)s->n[e] * s->hpe[e] - s->pool[e];
        if (left > 0.0f) hv += t->value * left / (float)t->hp;
    }
    return hv;
}

static int side_load(const IMPAI_FCONTENT* c, const IMPAI_FSIDE* in, SIDE* s)
{
    unsigned e;
    memset(s, 0, sizeof *s);
    if (in->ne > IMPAI_FSIDE_TYPES) return -1;
    s->ne = in->ne;
    for (e = 0; e < s->ne; e++) {
        const IMPAI_FENTRY* x = &in->e[e];
        float hpe;
        if (x->type >= c->nt || !c->t[x->type].hp) return -1;
        hpe = (float)c->t[x->type].hp * ((float)x->hp_frac / 65535.0f);
        s->type[e] = x->type;
        s->n[e] = x->n;
        s->hpe[e] = hpe < 1.0f ? 1.0f : hpe;
    }
    s->hv0 = side_hv(c, s);
    return s->hv0 > 0.0f ? 0 : -1;
}

static int side_alive(const SIDE* s)
{
    unsigned e;
    for (e = 0; e < s->ne; e++)
        if (s->n[e] > 0) return 1;
    return 0;
}

/* how far side x closes in a step: its slowest live mobile type's speed x close x the step, 0 when it does
   not close or has no live mobile type */
static float side_closing(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const SIDE* x, int closes)
{
    float v = -1.0f;
    unsigned e;
    if (!closes) return 0.0f;
    for (e = 0; e < x->ne; e++) {
        const IMPAI_FTYPE* t = &c->t[x->type[e]];
        float s;
        if (x->n[e] <= 0 || !(t->flags & IMPAI_FT_MOBILE) || t->speed <= 0) continue;
        s = (float)t->speed / 65536.0f;
        if (v < 0.0f || s < v) v = s;
    }
    return v < 0.0f ? 0.0f : v * (p->close > 0.0f ? p->close : 0.0f) * (float)IMPAI_FSTEP;
}

/* where side x stops closing: the shortest, over its live types that can hurt a live enemy type, of the
   type's longest range against one; 0 when none can, so it closes to contact */
static float side_stop(const SIDE* x, const SIDE* y, const PAIR pr[IMPAI_FSIDE_TYPES][IMPAI_FSIDE_TYPES])
{
    float stop = -1.0f;
    unsigned i, j;
    for (i = 0; i < x->ne; i++) {
        int32_t longest = -1;
        if (x->n[i] <= 0) continue;
        for (j = 0; j < y->ne; j++)
            if (y->n[j] > 0 && pr[i][j].r > 0.0f && pr[i][j].range > longest) longest = pr[i][j].range;
        if (longest >= 0 && (stop < 0.0f || (float)longest < stop)) stop = (float)longest;
    }
    return stop < 0.0f ? 0.0f : stop;
}

static int reaches(const SIDE* x, const SIDE* y, const PAIR pr[IMPAI_FSIDE_TYPES][IMPAI_FSIDE_TYPES], float d)
{
    unsigned i, j;
    for (i = 0; i < x->ne; i++)
        for (j = 0; j < y->ne; j++)
            if (x->n[i] > 0 && y->n[j] > 0 && pr[i][j].r > 0.0f && d <= (float)pr[i][j].range) return 1;
    return 0;
}

/* side x's fire on side y in one step, into dmg[] and shots[] by y's entries */
static void side_fire(const SIDE* x, const SIDE* y, const PAIR pr[IMPAI_FSIDE_TYPES][IMPAI_FSIDE_TYPES], float d,
                      const IMPAI_FIT* p, float* dmg, float* shots)
{
    const float focus = clamp01(p->focus), splash = p->splash > 0.0f ? p->splash : 0.0f;
    unsigned i, j;
    for (j = 0; j < y->ne; j++) dmg[j] = shots[j] = 0.0f;
    for (i = 0; i < x->ne; i++) {
        float total = 0.0f, best = 0.0f;
        int star = -1;
        if (x->n[i] <= 0) continue;
        for (j = 0; j < y->ne; j++) {
            float ttk;
            if (y->n[j] <= 0 || pr[i][j].r <= 0.0f || d > (float)pr[i][j].range) continue;
            total += (float)y->n[j];
            ttk = y->hpe[j] / pr[i][j].r;
            if (star < 0 || ttk < best) { star = (int)j; best = ttk; }
        }
        if (star < 0) continue;
        for (j = 0; j < y->ne; j++) {
            float share, over;
            if (y->n[j] <= 0 || pr[i][j].r <= 0.0f || d > (float)pr[i][j].range) continue;
            share = (1.0f - focus) * (float)y->n[j] / total + ((int)j == star ? focus : 0.0f);
            over = pr[i][j].ov < (float)(y->n[j] - 1) ? pr[i][j].ov : (float)(y->n[j] - 1);
            dmg[j] += (float)x->n[i] * share * pr[i][j].r * (float)IMPAI_FSTEP * (1.0f + splash * over);
            shots[j] += (float)x->n[i] * share * pr[i][j].q * (float)IMPAI_FSTEP;
        }
    }
}

/* the step's damage and shots on side y applied: its kills, never more than the units, the whole hp in the
   pool or the shots aimed at it so far; the units killed */
static uint32_t side_take(SIDE* y, const float* dmg, const float* shots)
{
    uint32_t killed = 0;
    unsigned j;
    for (j = 0; j < y->ne; j++) {
        float by_hp;
        int32_t kills;
        if (y->n[j] <= 0) continue;
        y->pool[j] += dmg[j];
        y->spool[j] += shots[j];
        by_hp = y->pool[j] / y->hpe[j];
        kills = by_hp >= (float)y->n[j] ? y->n[j] : (int32_t)by_hp;
        if (y->spool[j] < (float)kills) kills = (int32_t)y->spool[j];
        y->n[j] -= kills;
        y->pool[j] -= (float)kills * y->hpe[j];
        y->spool[j] -= (float)kills;
        if (y->pool[j] < 0.0f) y->pool[j] = 0.0f;
        if (y->n[j] <= 0) {
            y->pool[j] = y->spool[j] = 0.0f;
        } else if (y->pool[j] > (float)y->n[j] * y->hpe[j]) {
            y->pool[j] = (float)y->n[j] * y->hpe[j];
        }
        killed += (uint32_t)kills;
    }
    return killed;
}

int impai_fight_run(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a, const IMPAI_FSIDE* b,
                    int32_t dist_px, IMPAI_FRESULT* out, IMPAI_FSTATS* st)
{
    PAIR ab[IMPAI_FSIDE_TYPES][IMPAI_FSIDE_TYPES], ba[IMPAI_FSIDE_TYPES][IMPAI_FSIDE_TYPES];
    float dmg_a[IMPAI_FSIDE_TYPES], dmg_b[IMPAI_FSIDE_TYPES], shots_a[IMPAI_FSIDE_TYPES], shots_b[IMPAI_FSIDE_TYPES];
    SIDE sa, sb;
    float d = dist_px > 0 ? (float)dist_px : 0.0f;
    unsigned i, j, step;
    memset(out, 0, sizeof *out);
    if (st) memset(st, 0, sizeof *st);
    if (side_load(c, a, &sa) || side_load(c, b, &sb)) return -1;
    for (i = 0; i < sa.ne; i++)
        for (j = 0; j < sb.ne; j++)
            if (pair_rate(c, p, sa.type[i], sb.type[j], &ab[i][j]) || pair_rate(c, p, sb.type[j], sa.type[i], &ba[j][i]))
                return -1;
    for (step = 0; step < IMPAI_FSTEPS && side_alive(&sa) && side_alive(&sb); step++) {
        const float va = side_closing(c, p, &sa, a->closes), vb = side_closing(c, p, &sb, b->closes);
        const float stop_a = side_stop(&sa, &sb, ab), stop_b = side_stop(&sb, &sa, ba);
        const float ma = va > 0.0f && d > stop_a ? (va < d - stop_a ? va : d - stop_a) : 0.0f;
        const float mb = vb > 0.0f && d > stop_b ? (vb < d - stop_b ? vb : d - stop_b) : 0.0f;
        if (ma <= 0.0f && mb <= 0.0f && !reaches(&sa, &sb, ab, d) && !reaches(&sb, &sa, ba, d)) break;
        d -= ma + mb;
        if (d < 0.0f) d = 0.0f;
        side_fire(&sa, &sb, ab, d, p, dmg_b, shots_b);
        side_fire(&sb, &sa, ba, d, p, dmg_a, shots_a);
        if (st) {
            for (j = 0; j < sb.ne; j++) st->shots[1] += shots_b[j];
            for (i = 0; i < sa.ne; i++) st->shots[0] += shots_a[i];
            st->killed[1] += side_take(&sb, dmg_b, shots_b);
            st->killed[0] += side_take(&sa, dmg_a, shots_a);
        } else {
            side_take(&sb, dmg_b, shots_b);
            side_take(&sa, dmg_a, shots_a);
        }
        out->steps = (uint16_t)(step + 1);
    }
    out->sa = side_hv(c, &sa) / sa.hv0;
    out->sb = side_hv(c, &sb) / sb.hv0;
    out->m = out->sa - out->sb;
    return 0;
}

int impai_fight_predict(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a,
                        const IMPAI_FSIDE* b, int32_t dist_px, IMPAI_FRESULT* out)
{
    return impai_fight_run(c, p, a, b, dist_px, out, NULL);
}

/* THE MERGE, an approximation of the model's INPUT, not of the engine: a side of more than
   IMPAI_FSIDE_TYPES entries keeps the IMPAI_FSIDE_TYPES of most value (value x count x health; ties to the
   earlier entry), and each other entry's units join the kept entry of the same role and layer whose type's
   value is nearest (ties to the earlier kept entry), else of the same layer, its count scaled by the ratio of
   the two values and rounded; an entry with neither is left out. Entries of no unit are left out. */
static float entry_value(const IMPAI_FCONTENT* c, const IMPAI_FENTRY* e)
{
    return c->t[e->type].value * (float)e->n * ((float)e->hp_frac / 65535.0f);
}

int impai_fight_side(const IMPAI_FCONTENT* c, const IMPAI_FENTRY* in, unsigned n, int closes, IMPAI_FSIDE* out)
{
    unsigned kept[IMPAI_FSIDE_TYPES];
    unsigned nk = 0, e, k, pass;
    memset(out, 0, sizeof *out);
    out->closes = (uint8_t)(closes ? 1 : 0);
    for (e = 0; e < n; e++)
        if (in[e].type >= c->nt) return -1;
    while (nk < IMPAI_FSIDE_TYPES) {
        int best = -1;
        for (e = 0; e < n; e++) {
            int taken = 0;
            if (!in[e].n) continue;
            for (k = 0; k < nk; k++) taken |= kept[k] == e;
            if (!taken && (best < 0 || entry_value(c, &in[e]) > entry_value(c, &in[best]))) best = (int)e;
        }
        if (best < 0) break;
        kept[nk] = (unsigned)best;
        out->e[nk++] = in[best];
    }
    out->ne = (uint8_t)nk;
    for (e = 0; e < n; e++) {
        const IMPAI_FTYPE* te = &c->t[in[e].type];
        int taken = 0, into = -1;
        float gap = 0.0f;
        if (!in[e].n) continue;
        for (k = 0; k < nk; k++) taken |= kept[k] == e;
        if (taken) continue;
        for (pass = 0; pass < 2 && into < 0; pass++)
            for (k = 0; k < nk; k++) {
                const IMPAI_FTYPE* tk = &c->t[out->e[k].type];
                const float g = fabsf(tk->value - te->value);
                if (tk->layer != te->layer || (pass == 0 && tk->role != te->role) || tk->value <= 0.0f) continue;
                if (into < 0 || g < gap) { into = (int)k; gap = g; }
            }
        if (into >= 0) {
            const float add = (float)in[e].n * te->value / c->t[out->e[into].type].value + 0.5f;
            const uint32_t sum = (uint32_t)out->e[into].n + (add >= 65535.0f ? 65535u : (uint32_t)add);
            out->e[into].n = (uint16_t)(sum > 65535u ? 65535u : sum);
        }
    }
    return 0;
}
