/* impai_fight.c -- A STUB of the fight predictor's model (impai_fight.h), which the predictor's own
   impai_fight.c replaces whole: every entry point the header declares, over the same content and the same
   coefficients (impai_fitparams.c), with a plain model, so that the military and the factories are built and
   tested against the interface before the fitted model is in this tree. Its margins are NOT held to the
   engine: every shot hits, fire is spread by the target's count, there is no lead, spread, focus, splash or
   overkill bound, and the merge of a side past IMPAI_FSIDE_TYPES folds a type onto the kept one of the nearest
   value rather than by role; IMPAI_FSTATS.shots holds the HP aimed at each side, not shots. Nothing outside
   impai_fight.h's names reaches into it.

   Engine-free, deterministic, every loop bounded by the header's constants; floats rounded once per operation
   on SSE2 (the Makefile's IMPAI rule), and no libm transcendental, so its three builds agree. */
#include <string.h>
#include "impai_fight.h"
#include "tagpu_simreg.h"

/* The fight predictor. OWNER: only the military's decision calls it, for a live ImpAI seat (impai_mil.c). */
SIM_FEATURE(impai_fight, SIM_PUPPET_OWNER, "impai-fight stock-impai-fight impai-eco impai-observe stock-impai-observe");

int impai_fight_kind(uint32_t flags)
{
    if (flags & TASIM_WF_BEAM) return IMPAI_FK_BEAM;
    if (flags & TASIM_WF_DROPPED) return IMPAI_FK_DROPPED;
    if (flags & (TASIM_WF_GUIDANCE | TASIM_WF_TRACKS | TASIM_WF_VLAUNCH)) return IMPAI_FK_GUIDED;
    if (flags & TASIM_WF_BALLISTIC) return IMPAI_FK_BALLISTIC;
    return IMPAI_FK_DIRECT;
}

/* exp(x) by range reduction to [-ln2/2, ln2/2] and a degree-6 Taylor polynomial, 2^n put in the exponent
   bits: the same bits in every build. x is held to [-87, 88], inside a normal float's range. */
float impai_expf(float x)
{
    const float ln2 = 0.693147182f;
    float r, p;
    int n;
    union { float f; uint32_t u; } s;
    if (!(x > -87.0f)) x = -87.0f;
    if (x > 88.0f) x = 88.0f;
    n = (int)(x / ln2 + (x >= 0.0f ? 0.5f : -0.5f));
    r = x - (float)n * ln2;
    p = 1.0f + r * (1.0f + r * (0.5f + r * (0.166666672f + r * (0.0416666679f + r * (0.00833333377f +
        r * 0.00138888892f)))));
    s.u = (uint32_t)(n + 127) << 23;
    return p * s.f;
}

/* the [DAMAGE] row of type j among a weapon's rows, else its default */
static float damage_vs(const IMPAI_FCONTENT* c, const IMPAI_FWEAPON* w, unsigned j)
{
    unsigned lo = w->d0, hi = w->d0 + w->nd;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (c->d[mid].type < j) lo = mid + 1;
        else hi = mid;
    }
    return lo < w->d0 + w->nd && c->d[lo].type == j ? (float)c->d[lo].dmg : (float)w->dmg;
}

int impai_fight_rate(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, unsigned i, unsigned j, float* rate,
                     int32_t* range)
{
    const IMPAI_FTYPE *a, *b;
    unsigned k;
    (void)p;
    *rate = 0.0f;
    *range = 0;
    if (i >= c->nt || j >= c->nt) return -1;
    a = &c->t[i];
    b = &c->t[j];
    for (k = 0; k < IMPAI_FW_SLOTS; k++) {
        const IMPAI_FWEAPON* w;
        if (!a->w[k]) continue;
        if (a->w[k] >= c->nw) return -1;
        w = &c->w[a->w[k]];
        if (w->nd && ((uint64_t)w->d0 + w->nd > c->nd)) return -1;
        if (b->flags & IMPAI_FT_FLIES) { if (!(a->air >> k & 1u)) continue; }
        else if (w->flags & TASIM_WF_TOAIR) continue;
        if ((b->flags & IMPAI_FT_SUBMERGED) && !(w->flags & TASIM_WF_WATER)) continue;
        *rate += (float)(w->burst ? w->burst : 1) * damage_vs(c, w, j) / (float)(w->reload ? w->reload : 1);
        if (w->range > *range) *range = w->range;
    }
    return 0;
}

float impai_fight_rate_of(const void* ctx, uint16_t i, uint16_t j)
{
    const IMPAI_FCTX* x = (const IMPAI_FCTX*)ctx;
    float r;
    int32_t range;
    return impai_fight_rate(x->c, x->p, i, j, &r, &range) == 0 ? r : 0.0f;
}

float impai_fight_pwin(const IMPAI_FIT* p, float m)
{
    return 1.0f / (1.0f + impai_expf(-m / (p->tau > 0.0f ? p->tau : 1.0f)));
}

int impai_fight_side(const IMPAI_FCONTENT* c, const IMPAI_FENTRY* in, unsigned n, int closes, IMPAI_FSIDE* out)
{
    unsigned i, k;
    memset(out, 0, sizeof *out);
    out->closes = (uint8_t)(closes != 0);
    for (i = 0; i < n; i++) {
        if (in[i].type >= c->nt) return -1;
        if (!in[i].n) continue;
        for (k = 0; k < out->ne && out->e[k].type != in[i].type; k++) {}
        if (k < out->ne) { out->e[k].n = (uint16_t)(out->e[k].n + in[i].n); continue; }
        if (out->ne < IMPAI_FSIDE_TYPES) { out->e[out->ne++] = in[i]; continue; }
        /* past the bound: onto the kept type of the nearest value -- the model's input approximated */
        {
            unsigned best = 0;
            float bd = -1.0f;
            for (k = 0; k < out->ne; k++) {
                const float dv = c->t[out->e[k].type].value - c->t[in[i].type].value;
                const float ad = dv < 0 ? -dv : dv;
                if (bd < 0 || ad < bd) { bd = ad; best = k; }
            }
            if (c->t[out->e[best].type].value > 0) {
                const float add = (float)in[i].n * c->t[in[i].type].value / c->t[out->e[best].type].value + 0.5f;
                out->e[best].n = (uint16_t)(out->e[best].n + (add < 60000.0f ? (unsigned)add : 60000u));
            }
        }
    }
    return 0;
}

/* a side's state through the fight: each entry's health pool, HP */
typedef struct SIDE { const IMPAI_FSIDE* s; float pool[IMPAI_FSIDE_TYPES], start; } SIDE;

static float side_value(const IMPAI_FCONTENT* c, const SIDE* s)
{
    float v = 0.0f;
    unsigned k;
    for (k = 0; k < s->s->ne; k++) {
        const IMPAI_FTYPE* t = &c->t[s->s->e[k].type];
        v += t->value * s->pool[k] / (float)t->hp;
    }
    return v;
}

static int side_init(const IMPAI_FCONTENT* c, const IMPAI_FSIDE* f, SIDE* s)
{
    unsigned k;
    s->s = f;
    s->start = 0.0f;
    if (f->ne > IMPAI_FSIDE_TYPES) return -1;
    for (k = 0; k < f->ne; k++) {
        const IMPAI_FTYPE* t;
        if (f->e[k].type >= c->nt) return -1;
        t = &c->t[f->e[k].type];
        if (!t->hp) return -1;
        s->pool[k] = (float)f->e[k].n * (float)t->hp * ((float)f->e[k].hp_frac / 65535.0f);
        s->start += (float)f->e[k].n * t->value;
    }
    return s->start > 0.0f ? 0 : -1;
}

/* the damage side x deals each entry of side y this step, at distance d */
static int fire(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const SIDE* x, const SIDE* y, float d,
                float dmg[IMPAI_FSIDE_TYPES], int* reach)
{
    unsigned i, j;
    for (j = 0; j < y->s->ne; j++) dmg[j] = 0.0f;
    for (i = 0; i < x->s->ne; i++) {
        const IMPAI_FTYPE* a = &c->t[x->s->e[i].type];
        float rate[IMPAI_FSIDE_TYPES], alive = x->pool[i] / (float)a->hp, wsum = 0.0f;
        for (j = 0; j < y->s->ne; j++) {
            int32_t range;
            rate[j] = 0.0f;
            if (impai_fight_rate(c, p, x->s->e[i].type, y->s->e[j].type, &rate[j], &range) < 0) return -1;
            if (rate[j] > 0.0f && (float)range >= d && y->pool[j] > 0.0f) {
                wsum += y->pool[j] / (float)c->t[y->s->e[j].type].hp;
                *reach = 1;
            } else {
                rate[j] = 0.0f;
            }
        }
        if (alive <= 0.0f || wsum <= 0.0f) continue;
        for (j = 0; j < y->s->ne; j++)
            if (rate[j] > 0.0f)
                dmg[j] += alive * rate[j] * (float)IMPAI_FSTEP * (y->pool[j] / (float)c->t[y->s->e[j].type].hp) / wsum;
    }
    return 0;
}

/* the slowest mobile type's speed, px a step; 0 when none moves */
static float closing(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const SIDE* s)
{
    float v = 0.0f;
    unsigned k;
    int any = 0;
    for (k = 0; k < s->s->ne; k++) {
        const IMPAI_FTYPE* t = &c->t[s->s->e[k].type];
        const float sp = (float)t->speed / 65536.0f * (float)IMPAI_FSTEP;
        if (!(t->flags & IMPAI_FT_MOBILE) || s->pool[k] <= 0.0f) continue;
        if (!any || sp < v) v = sp;
        any = 1;
    }
    return v * p->close;
}

int impai_fight_run(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a, const IMPAI_FSIDE* b,
                    int32_t dist_px, IMPAI_FRESULT* out, IMPAI_FSTATS* st)
{
    SIDE A, B;
    float d = (float)(dist_px > 0 ? dist_px : 0), da[IMPAI_FSIDE_TYPES], db[IMPAI_FSIDE_TYPES];
    unsigned step, k;
    memset(out, 0, sizeof *out);
    memset(st, 0, sizeof *st);
    if (side_init(c, a, &A) < 0 || side_init(c, b, &B) < 0) return -1;
    for (step = 0; step < IMPAI_FSTEPS; step++) {
        int ra = 0, rb = 0;
        float va, vb;
        if (fire(c, p, &A, &B, d, db, &ra) < 0 || fire(c, p, &B, &A, d, da, &rb) < 0) return -1;
        /* a side closes while none of its types has a target in range */
        va = a->closes && !ra ? closing(c, p, &A) : 0.0f;
        vb = b->closes && !rb ? closing(c, p, &B) : 0.0f;
        if (!ra && !rb && va + vb <= 0.0f) break;
        for (k = 0; k < A.s->ne; k++) {
            st->shots[0] += da[k];
            A.pool[k] = A.pool[k] > da[k] ? A.pool[k] - da[k] : 0.0f;
        }
        for (k = 0; k < B.s->ne; k++) {
            st->shots[1] += db[k];
            B.pool[k] = B.pool[k] > db[k] ? B.pool[k] - db[k] : 0.0f;
        }
        d -= va + vb;
        if (d < 0.0f) d = 0.0f;
        if (side_value(c, &A) <= 0.0f || side_value(c, &B) <= 0.0f) { step++; break; }
    }
    out->sa = side_value(c, &A) / A.start;
    out->sb = side_value(c, &B) / B.start;
    out->m = out->sa - out->sb;
    out->steps = (uint16_t)step;
    for (k = 0; k < A.s->ne; k++)
        st->killed[0] += a->e[k].n - (uint32_t)(A.pool[k] / (float)c->t[a->e[k].type].hp + 0.999f);
    for (k = 0; k < B.s->ne; k++)
        st->killed[1] += b->e[k].n - (uint32_t)(B.pool[k] / (float)c->t[b->e[k].type].hp + 0.999f);
    return 0;
}

int impai_fight_predict(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a,
                        const IMPAI_FSIDE* b, int32_t dist_px, IMPAI_FRESULT* out)
{
    IMPAI_FSTATS st;
    return impai_fight_run(c, p, a, b, dist_px, out, &st);
}
