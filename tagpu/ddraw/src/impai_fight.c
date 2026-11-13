/* impai_fight.c -- ImpAI's fight predictor (impai_fight.h).

   WHAT IT MODELS. Two groups of unit types, each a list of (type, count, health), whose fronts -- their
   nearest unit centres -- stand a gap apart, fight for IMPAI_FHORIZON ticks at most: IMPAI_FSTEPS0 steps of
   IMPAI_FSTEP0 ticks, then IMPAI_FSTEPS1 of IMPAI_FSTEP1. The engine facts it rests on (research/notes/
   engine/weapons.md):
     E1  the range test is flat and centre to centre: can-aim 0x49AA80 and can-engage 0x49ABB0 compare
         dx^2 + dz^2 of the two units' positions with range^2 (0x49AC47..0x49AC92), no height term;
     E1b a ballistic weapon must also pass the solver 0x49A890, so its reach on flat ground is
         min(range, v^2 / g), g the level's gravity main+0x14263;
     E2  a chaser walks to the range of its first armed slot of slots 0..2, whatever that slot can hurt, and
         stops on its first in-range tick (Attack_Chase 0x4034A0: the slot 0x4897E0 stored at 0x4035A1, its
         range 0x49ADF0 at 0x403623, can-engage at 0x4035D6), braking inside it (chase_reach);
     E3  every weapon slot starts loaded (the slot is zeroed at creation, 0x49E099), counts down once a tick
         whether or not it has a target (AutoAim 0x49E1A0, 0x49E1C8..0x49E1D2), and after a shot reloads for
         floor(floor((100 - 6L) reload / 100) (120 - floor(20 hp / maxhp)) / 100) ticks (0x49E48D..0x49E4EE);
     E4  what guides a shot (impai_fight.h, the kinds);
     E5  an area blast hurts a unit within aoe/2 of its box (0x49A120: R = aoe >> 1 at 0x49A152, the falloff
         e + (1 - e)(d/R - 1)^2 at 0x49A3B6..0x49A3E0);
     E6  a hit damages one unit (the apply 0x489CE0 subtracts from that unit's HP word at 0x489EB5): what a
         killing shot deals beyond the unit's hp reaches nobody else but through its area.
   Each step:
     1. every live type of a side that closes moves at its own maxvelocity toward the other side's front,
        until its gap to a live enemy type it can hurt is within its chase slot's reach less
        IMPAI_FSTANDOFF (E2), or, when it can hurt none, to contact; both sides move from the step's start;
     2. each weapon of each type fires volleys on the enemy types within its reach of it: the first at once,
        the next a reload later (E3), the countdown running down by the step's ticks whether it fires or not.
        It spreads its hits over those types, (1 - focus) by their counts and focus on the one it kills
        quickest; a hit deals at most one unit's hp (E6), and an area weapon's hit reaches `splash` x the
        overlap more units, the overlap its area over the target's spacing, squared, at the mean of the
        falloff over a disc (e + (1 - e)/6), never more than the other units of the type, each of which can
        die (E5);
     3. the damage on a type pools; a unit dies for each whole hp the pool holds, never more than the hits
        aimed at the type so far, and the pool keeps the remainder;
     4. it ends when a side has no unit, at the horizon, or in a step where nothing moved and nothing was in
        reach.
   A hit's probability is sigma(bias + lead x l) with its kind's coefficients, where for a target standing l is
   0 and for one moving at its maxvelocity v_t, its size s = 16 sqrt(fx fz) px and r_hit = (s + aoe) / 2 the
   radius a blast can land within and still hurt it (E5): l = v_t x the shot's flight time at its reach /
   r_hit, or for a HOMING shot of speed v, v_t / v; a type hit while moving part of a step is hit between the
   two by that part. Both sides' fire in a step is computed from the moved state and taken after both have
   fired, every side's figure by the same code, so a mirror fight's margin is 0 exactly and swapping the
   sides negates it.

   WHAT IT DOES NOT MODEL: terrain and height (the ballistic solver's slope effect), pathing and blocking,
   line of fire, armour (the victim's +0x10E bit 1 scales damage by def +0x1AA, 0x489BC3..0x489BF1, when COB
   sets it: COB state), veterancy's reload factor (E3's L, at 0: a fight's units start with no kills), a
   weapon's minimum range, turning and aiming time, a weapon's accuracy and spray (fitted to no signal), the
   engine's own target choice, each type's braking distance (IMPAI_FSTANDOFF stands for it), and a unit
   idling once the target of its one order dies. Those are what the coefficients absorb, fitted to the
   engine's fights (tools/impai/predfit.py); the predictor is a model of the engine, never a copy.

   ROUNDING. Every value is a float rounded once per operation (SSE2, no contraction: the Makefile's IMPAI
   rule), evaluated in the order written; the countdowns and the reach are integers; the only library call is
   sqrtf, which IEEE 754 rounds exactly, and every exponential goes through impai_expf. */
#include <math.h>
#include <string.h>
#include "impai_fight.h"
#include "tagpu_simreg.h"

/* The fight predictor. OWNER: only the military's decision calls it, for a live ImpAI seat (impai_mil.c). */
SIM_FEATURE(impai_fight, SIM_PUPPET_OWNER, "impai-fight stock-impai-fight impai-eco impai-observe stock-impai-observe");

#define SPACING_PAD 32.0f      /* a unit's spacing: its size and this much room                        */
#define GRAVITY_MAX 65536      /* the gravity bound tagpu_rangeown.c applies                           */

int impai_fight_kind(uint32_t flags)
{
    if (flags & TASIM_WF_DROPPED) return IMPAI_FK_DROPPED;
    if ((flags & TASIM_WF_SELFPROP) &&
        (((flags & TASIM_WF_GUIDANCE) && !(flags & TASIM_WF_TWOPHASE)) ||
         ((flags & TASIM_WF_TWOPHASE) && (flags & TASIM_WF_TRACKS))))
        return IMPAI_FK_HOMING;
    if (flags & TASIM_WF_BALLISTIC) return IMPAI_FK_BALLISTIC;
    return IMPAI_FK_STRAIGHT;
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

/* v held to [0, 1], a NaN to 0: the comparisons are false on a NaN, so it takes the first arm */
static float clamp01(float v)
{
    return !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;
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

/* the content's weapon wi, or NULL when the index, its kind or its [DAMAGE] span is past the content */
static const IMPAI_FWEAPON* weapon_of(const IMPAI_FCONTENT* c, unsigned wi)
{
    const IMPAI_FWEAPON* w;
    if (wi >= c->nw) return NULL;
    w = &c->w[wi];
    if (w->kind >= IMPAI_FKINDS || w->d0 > c->nd || w->nd > c->nd - w->d0) return NULL;
    return w;
}

/* Weapon w of slot k of type ti hurts type tj when the classifier's rules let it (tasim_roles.h): an aircraft
   only through an anti-air slot (IMPAI_FTYPE.air), anything else not through a toairweapon, and a submerged
   unit only through a water weapon. */
static int hurts(const IMPAI_FTYPE* ti, int k, const IMPAI_FWEAPON* w, const IMPAI_FTYPE* tj)
{
    if (tj->flags & IMPAI_FT_FLIES) return (ti->air >> k & 1u) != 0;
    if (w->flags & TASIM_WF_TOAIR) return 0;
    if ((tj->flags & IMPAI_FT_SUBMERGED) && !(w->flags & TASIM_WF_WATER)) return 0;
    return 1;
}

/* Weapon w's reach in px: its range, and for a ballistic weapon on a level of gravity g the solver's
   v^2 / g as well (E1b, 0x49A890), v its +0x68 16.16 px a tick and g main+0x14263 16.16 px a tick squared:
   v^2 / (g 65536) px, in 64 bits (at most 2^62 / 2^16) and compared with the range before it narrows. A
   gravity outside 1..GRAVITY_MAX is no cap. */
static int32_t reach_of(const IMPAI_FCONTENT* c, const IMPAI_FWEAPON* w)
{
    int32_t range = w->range;
    if ((w->flags & TASIM_WF_BALLISTIC) && w->velocity > 0 && c->gravity > 0 && c->gravity <= GRAVITY_MAX) {
        const int64_t reach = (int64_t)w->velocity * w->velocity / ((int64_t)c->gravity * 65536);
        if (reach < range) range = (int32_t)reach;
    }
    return range;
}

/* The hit probabilities of weapon w, reach `range`, on type tj: p0 on it standing, p1 on it moving at its
   maxvelocity (the file's header, the lead) */
static void hit_of(const IMPAI_FIT* p, const IMPAI_FWEAPON* w, int32_t range, const IMPAI_FTYPE* tj, float* p0,
                   float* p1)
{
    const float* h = p->hit[w->kind];
    const float v = (float)w->velocity / 65536.0f;
    const float tf = v > 0.0f ? (float)range / v : 0.0f;
    const float rhit = 0.5f * (size_px(tj) + (float)w->aoe);
    const float vt = (tj->flags & IMPAI_FT_MOBILE) ? (float)tj->speed / 65536.0f : 0.0f;
    *p0 = sigmoid(h[IMPAI_FH_BIAS]);
    if (w->kind == IMPAI_FK_HOMING)
        *p1 = sigmoid(h[IMPAI_FH_BIAS] + h[IMPAI_FH_LEAD] * (v > 0.0f ? vt / v : 0.0f));
    else
        *p1 = sigmoid(h[IMPAI_FH_BIAS] + h[IMPAI_FH_LEAD] * (vt * tf / rhit));
}

/* the volleys' reload of weapon w at an entry's health hp_frac / 65535 (E3, 0x49E48D..0x49E4EE), the level
   factor (100 - 6L) at L = 0: at least 1. At most 120 x 65535 / 100 = 78642 ticks. */
static int32_t reload_of(const IMPAI_FWEAPON* w, uint16_t hp_frac)
{
    const int32_t h20 = (int32_t)((uint32_t)hp_frac * 20u / 65535u);
    int32_t rl = w->reload > 1 ? w->reload : 1;
    rl = (120 - h20) * rl / 100;
    return rl > 1 ? rl : 1;
}

int impai_fight_rate(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, unsigned i, unsigned j, float* rate,
                     int32_t* range)
{
    const IMPAI_FTYPE* ti;
    const IMPAI_FTYPE* tj;
    float r = 0.0f;
    int32_t longest = 0;
    int k;
    if (i >= c->nt || j >= c->nt) return -1;
    ti = &c->t[i];
    tj = &c->t[j];
    for (k = 0; k < IMPAI_FW_SLOTS; k++) {
        const unsigned wi = ti->w[k];
        const IMPAI_FWEAPON* w;
        int32_t d, reach;
        float p0, p1, dmg;
        if (!wi) continue;
        if (!(w = weapon_of(c, wi))) return -1;
        if (!hurts(ti, k, w, tj) || (d = damage_on(c, w, j)) <= 0 || (reach = reach_of(c, w)) <= 0) continue;
        hit_of(p, w, reach, tj, &p0, &p1);
        dmg = tj->hp && (float)d > (float)tj->hp ? (float)tj->hp : (float)d;
        r += (float)(w->burst > 1 ? w->burst : 1) * dmg * p1 / (float)(w->reload > 1 ? w->reload : 1);
        if (reach > longest) longest = reach;
    }
    *rate = r;
    *range = longest;
    return 0;
}

float impai_fight_rate_of(const void* ctx, uint16_t i, uint16_t j)
{
    const IMPAI_FCTX* x = (const IMPAI_FCTX*)ctx;
    float rate;
    int32_t range;
    return impai_fight_rate(x->c, x->p, i, j, &rate, &range) ? 0.0f : rate;
}

/* its value x health fraction: value x (n x hpe - pool) / hp over the entries */
static float side_hv(const IMPAI_FCONTENT* c, const IMPAI_FSTATE* s)
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

/* the side's entries: count, effective hp (its type's hp at the entry's health, at least 1) and speed (its
   type's maxvelocity when the side closes and the type moves) */
static int side_load(const IMPAI_FCONTENT* c, const IMPAI_FSIDE* in, IMPAI_FSTATE* s)
{
    unsigned e;
    memset(s, 0, sizeof *s);
    if (in->ne > IMPAI_FSIDE_TYPES) return -1;
    s->ne = in->ne;
    for (e = 0; e < s->ne; e++) {
        const IMPAI_FENTRY* x = &in->e[e];
        const IMPAI_FTYPE* t;
        float hpe;
        if (x->type >= c->nt || !c->t[x->type].hp) return -1;
        t = &c->t[x->type];
        hpe = (float)t->hp * ((float)x->hp_frac / 65535.0f);
        s->type[e] = x->type;
        s->n[e] = x->n;
        s->hpe[e] = hpe < 1.0f ? 1.0f : hpe;
        s->speed[e] = in->closes && (t->flags & IMPAI_FT_MOBILE) && t->speed > 0 ? (float)t->speed / 65536.0f : 0.0f;
    }
    s->hv0 = side_hv(c, s);
    return s->hv0 > 0.0f ? 0 : -1;
}

/* The reach the engine's chase walks to: that of its first armed slot of the three the engine tests.
   Attack_Chase takes its slot from 0x4897E0 (stored at 0x4035A1): slot 0 when its flag byte, unit +0x1F, has
   bit 0x2, else slot 1 when +0x3B has it, else slot 2 when +0x57 has it, and slot 0 when none of the three
   does (0x4897E0..0x4897F9, `and al,dl` leaving 0); slots 3..9 are never taken. It walks toward the target --
   that slot's range read by 0x49ADF0 at 0x403623, the move's arrival radius half of it (0x403660) -- until
   can-engage for that slot passes (0x4035D6), whether or not that slot can hurt the target, a failed
   can-engage keeping the order running. The slot's creation 0x49E070 sets bit 0x2 when the slot's weapon has
   an ID byte +0x10A other than 0 (0x49E0C2..0x49E0E8), the null record's being 0: a slot holding any weapon,
   a stockpile or interceptor one included. DISASSEMBLED.
   The reach is reach_of's, the range capped by the ballistic solver, where the engine's radius is the raw
   +0xDC: the distance the slot can engage from [INFERRED: can-engage 0x49ABB0 includes the solver's test].
   NOT MODELLED: the content leaves out a stockpile or interceptor weapon (tagpu_worldq_fight_in), so a slot
   holding one reads as empty here, and its type closes to its next armed slot's reach where the engine's
   chase walks to the left-out weapon's. 0 when none of the three holds a weapon the content has, or that
   weapon cannot reach; -1 for a weapon past the content. */
#define CHASE_SLOTS 3
static int32_t chase_reach(const IMPAI_FCONTENT* c, const IMPAI_FTYPE* t)
{
    int k;
    for (k = 0; k < CHASE_SLOTS; k++) {
        const IMPAI_FWEAPON* w;
        int32_t r;
        if (!t->w[k]) continue;
        if (!(w = weapon_of(c, t->w[k]))) return -1;
        r = reach_of(c, w);
        return r > 0 ? r : 0;
    }
    return 0;
}

/* Side x's weapons against side y: one weapon entry for each slot of each entry whose weapon hurts at least
   one of y's entries, with its reach, reload, burst, a loaded countdown, and on each of y's entries its
   damage a hit (at most one unit's effective hp, E6), its hit probabilities and its area's overlap; and each
   pair's stand-off, the reach of the slot the engine's chase walks to (chase_reach, E2) less IMPAI_FSTANDOFF,
   set when some slot of the entry hurts the enemy entry, -1 for none. -1 for a weapon past the content. */
static int side_arm(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* xin, IMPAI_FSTATE* x,
                    const IMPAI_FSTATE* y)
{
    unsigned e, j;
    int k;
    for (e = 0; e < x->ne; e++)
        for (j = 0; j < y->ne; j++) x->stop[e][j] = -1;
    for (e = 0; e < x->ne; e++) {
        const IMPAI_FTYPE* ti = &c->t[x->type[e]];
        const int32_t chase = chase_reach(c, ti);
        if (chase < 0) return -1;
        for (k = 0; k < IMPAI_FW_SLOTS; k++) {
            const unsigned wi = ti->w[k];
            const IMPAI_FWEAPON* w;
            int32_t range;
            unsigned slot;
            int any = 0;
            if (!wi) continue;
            if (!(w = weapon_of(c, wi))) return -1;
            if ((range = reach_of(c, w)) <= 0) continue;
            if (x->nw >= IMPAI_FSIDE_WEAPONS) return -1;
            slot = x->nw;
            for (j = 0; j < y->ne; j++) {
                const IMPAI_FTYPE* tj = &c->t[y->type[j]];
                int32_t d;
                float edge, over;
                x->dmg[slot][j] = 0.0f;
                if (!hurts(ti, k, w, tj) || (d = damage_on(c, w, y->type[j])) <= 0) continue;
                any = 1;
                x->dmg[slot][j] = (float)d > y->hpe[j] ? y->hpe[j] : (float)d;
                hit_of(p, w, range, tj, &x->p0[slot][j], &x->p1[slot][j]);
                edge = clamp01(w->edge);
                over = (float)w->aoe / (size_px(tj) + SPACING_PAD);
                x->ov[slot][j] = over * over * (edge + (1.0f - edge) / 6.0f);
                if (x->stop[e][j] < 0) x->stop[e][j] = chase;
            }
            if (!any) continue;
            x->we[slot] = (uint8_t)e;
            x->wrange[slot] = range;
            x->wreload[slot] = reload_of(w, xin->e[e].hp_frac);
            x->wburst[slot] = w->burst > 1 ? w->burst : 1;
            x->wready[slot] = 0;
            x->nw++;
        }
    }
    for (e = 0; e < x->ne; e++)
        for (j = 0; j < y->ne; j++)
            if (x->stop[e][j] >= 0) x->stop[e][j] = x->stop[e][j] > IMPAI_FSTANDOFF ? x->stop[e][j] - IMPAI_FSTANDOFF : 0;
    return 0;
}

/* the gap between x's entry i and y's entry j: the fronts' gap less both entries' travel, at least 0 */
static float gap_of(float D, const IMPAI_FSTATE* x, unsigned i, const IMPAI_FSTATE* y, unsigned j)
{
    const float d = D - x->travel[i] - y->travel[j];
    return d > 0.0f ? d : 0.0f;
}

/* how far x's entry i moves in a step of dt ticks, from the step's starting state: to its stand-off from the
   nearest live enemy entry it can hurt, else to contact with the nearest live one, at most its speed x dt */
static float move_of(float D, const IMPAI_FSTATE* x, unsigned i, const IMPAI_FSTATE* y, int dt)
{
    float room = -1.0f, step;
    unsigned j;
    int any_hurt = 0;
    if (x->n[i] <= 0 || x->speed[i] <= 0.0f) return 0.0f;
    for (j = 0; j < y->ne; j++) {
        float r;
        if (y->n[j] <= 0 || x->stop[i][j] < 0) continue;
        any_hurt = 1;
        r = gap_of(D, x, i, y, j) - (float)x->stop[i][j];
        if (r <= 0.0f) return 0.0f;
        if (room < 0.0f || r < room) room = r;
    }
    if (!any_hurt) {
        for (j = 0; j < y->ne; j++) {
            float d;
            if (y->n[j] <= 0) continue;
            d = gap_of(D, x, i, y, j);
            if (room < 0.0f || d < room) room = d;
        }
        if (room <= 0.0f) return 0.0f;
    }
    step = x->speed[i] * (float)dt;
    return room < step ? room : step;
}

/* Side x's fire on side y in a step of dt ticks, into dmg[] and shots[] by y's entries: 1 when any weapon
   had a target in reach. THE COUNTDOWN (E3): a weapon starts loaded; in a step it fires
   N = 1 + (dt - 1 - ready) / reload volleys when ready < dt and none otherwise, and ready runs down by dt
   either way, so ready is in [0, reload) after a volley. Integer: N <= dt <= IMPAI_FSTEP1 and the reload at
   most 78642, so N x reload fits. */
static int side_fire(float D, IMPAI_FSTATE* x, const IMPAI_FSTATE* y, const IMPAI_FIT* p, int dt, float* dmg,
                     float* shots)
{
    const float focus = clamp01(p->focus), splash = p->splash > 0.0f ? p->splash : 0.0f;
    unsigned e, j;
    int fired = 0;
    for (j = 0; j < y->ne; j++) dmg[j] = shots[j] = 0.0f;
    for (e = 0; e < x->nw; e++) {
        const unsigned i = x->we[e];
        float total = 0.0f, best = 0.0f, pj[IMPAI_FSIDE_TYPES];
        int star = -1;
        int32_t volleys;
        if (x->n[i] <= 0) continue;
        for (j = 0; j < y->ne; j++) {
            float ttk;
            pj[j] = 0.0f;
            if (y->n[j] <= 0 || x->dmg[e][j] <= 0.0f || gap_of(D, x, i, y, j) > (float)x->wrange[e]) continue;
            pj[j] = x->p0[e][j] + y->frac[j] * (x->p1[e][j] - x->p0[e][j]);
            total += (float)y->n[j];
            ttk = y->hpe[j] / (x->dmg[e][j] * (pj[j] > 1e-6f ? pj[j] : 1e-6f));
            if (star < 0 || ttk < best) { star = (int)j; best = ttk; }
        }
        if (star < 0) {
            x->wready[e] = x->wready[e] > dt ? x->wready[e] - dt : 0;
            continue;
        }
        fired = 1;
        if (x->wready[e] < dt) {
            volleys = 1 + (dt - 1 - x->wready[e]) / x->wreload[e];
            x->wready[e] = x->wready[e] + volleys * x->wreload[e] - dt;
        } else {
            volleys = 0;
            x->wready[e] -= dt;
        }
        /* SPLASH KILLS (E5): a blast reaches each unit within aoe/2 of its box and each may die, so every unit
           it reaches counts as a hit toward the kill cap */
        for (j = 0; j < y->ne; j++) {
            float share, over, hits;
            if (pj[j] <= 0.0f) continue;
            share = (1.0f - focus) * (float)y->n[j] / total + ((int)j == star ? focus : 0.0f);
            hits = (float)x->n[i] * share * (float)volleys * (float)x->wburst[e] * pj[j];
            over = x->ov[e][j] < (float)(y->n[j] - 1) ? x->ov[e][j] : (float)(y->n[j] - 1);
            dmg[j] += hits * x->dmg[e][j] * (1.0f + splash * over);
            shots[j] += hits * (1.0f + splash * over);
        }
    }
    return fired;
}

/* the step's damage and hits on side y applied: its kills, never more than the units, the whole hp in the
   pool or the hits aimed at it so far; the units killed */
static uint32_t side_take(IMPAI_FSTATE* y, const float* dmg, const float* shots)
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
        if (!(by_hp >= 0.0f)) by_hp = 0.0f;     /* a NaN kills none: converting it to an integer is undefined */
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

static int side_alive(const IMPAI_FSTATE* s)
{
    unsigned e;
    for (e = 0; e < s->ne; e++)
        if (s->n[e] > 0) return 1;
    return 0;
}

/* both sides' moves in a step of dt ticks, from the step's starting state: 1 when any entry moved */
static int sides_move(float D, IMPAI_FSTATE* sa, IMPAI_FSTATE* sb, int dt)
{
    float ma[IMPAI_FSIDE_TYPES], mb[IMPAI_FSIDE_TYPES];
    unsigned i;
    int moved = 0;
    for (i = 0; i < sa->ne; i++) ma[i] = move_of(D, sa, i, sb, dt);
    for (i = 0; i < sb->ne; i++) mb[i] = move_of(D, sb, i, sa, dt);
    for (i = 0; i < sa->ne; i++) {
        sa->travel[i] += ma[i];
        sa->frac[i] = sa->speed[i] > 0.0f ? ma[i] / (sa->speed[i] * (float)dt) : 0.0f;
        moved |= ma[i] > 0.0f;
    }
    for (i = 0; i < sb->ne; i++) {
        sb->travel[i] += mb[i];
        sb->frac[i] = sb->speed[i] > 0.0f ? mb[i] / (sb->speed[i] * (float)dt) : 0.0f;
        moved |= mb[i] > 0.0f;
    }
    return moved;
}

int impai_fight_run(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a, const IMPAI_FSIDE* b,
                    int32_t gap_px, IMPAI_FWORK* w, IMPAI_FRESULT* out, IMPAI_FSTATS* st)
{
    IMPAI_FSTATE* sa = &w->a;
    IMPAI_FSTATE* sb = &w->b;
    float dmg_a[IMPAI_FSIDE_TYPES], dmg_b[IMPAI_FSIDE_TYPES], shots_a[IMPAI_FSIDE_TYPES], shots_b[IMPAI_FSIDE_TYPES];
    const float D = gap_px > 0 ? (float)gap_px : 0.0f;
    unsigned i, j, step;
    memset(out, 0, sizeof *out);
    if (st) memset(st, 0, sizeof *st);
    if (side_load(c, a, sa) || side_load(c, b, sb)) return -1;
    if (side_arm(c, p, a, sa, sb) || side_arm(c, p, b, sb, sa)) return -1;
    for (step = 0; step < IMPAI_FSTEPS && side_alive(sa) && side_alive(sb); step++) {
        const int dt = step < IMPAI_FSTEPS0 ? IMPAI_FSTEP0 : IMPAI_FSTEP1;
        const int moved = sides_move(D, sa, sb, dt);
        const int fa = side_fire(D, sa, sb, p, dt, dmg_b, shots_b);
        const int fb = side_fire(D, sb, sa, p, dt, dmg_a, shots_a);
        if (!moved && !fa && !fb) break;
        if (st) {
            for (j = 0; j < sb->ne; j++) st->shots[1] += shots_b[j];
            for (i = 0; i < sa->ne; i++) st->shots[0] += shots_a[i];
            st->killed[1] += side_take(sb, dmg_b, shots_b);
            st->killed[0] += side_take(sa, dmg_a, shots_a);
        } else {
            side_take(sb, dmg_b, shots_b);
            side_take(sa, dmg_a, shots_a);
        }
        out->steps = (uint16_t)(step + 1);
    }
    out->sa = side_hv(c, sa) / sa->hv0;
    out->sb = side_hv(c, sb) / sb->hv0;
    out->m = out->sa - out->sb;
    return 0;
}

int impai_fight_predict(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a,
                        const IMPAI_FSIDE* b, int32_t gap_px, IMPAI_FWORK* w, IMPAI_FRESULT* out)
{
    return impai_fight_run(c, p, a, b, gap_px, w, out, NULL);
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
