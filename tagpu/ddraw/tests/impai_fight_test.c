/* impai_fight_test.c -- the fight predictor (src/impai_fight.h) on a fixed synthetic content, run by
   tests/impai_fight_test.py in three builds that must print one transcript.

   The content is drawn from a fixed LCG: unit types of every layer, mobile and static, flying and submerged;
   weapons of every kind with and without [DAMAGE] rows, anti-air and water ones. 200 fixed fights are
   printed bit for bit, each with its kills and shots, and every fight is held to the sanity rules:
     - a mirror fight's margin is 0 exactly, and swapping the sides negates the margin exactly;
     - a side doubled against itself never loses, and an army of armed ground units doubled wins. A random
       doubled side can draw: an area weapon whose overlap reaches every unit of a type takes the same share of
       each side, and a step whose fire kills every armed unit of both sides leaves both the same share;
     - sides out of each other's range that do not close end at step 0 with margin 0;
     - the units a side loses never exceed the shots aimed at it;
     - the steps never pass IMPAI_FSTEPS, the shares stay in [0, 1];
   and the refusals: a type past the content, a weapon index past it, a damage span past the pool, a side of
   more than IMPAI_FSIDE_TYPES entries. impai_expf is held to exp's value within 2^-22 and its bits printed;
   the merge of a 20-entry side is printed. Exit 0 when every rule held, 1 otherwise. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "impai_fight.h"

#define NT 24
#define NW 40
#define ND 64

static IMPAI_FTYPE   T[NT];
static IMPAI_FWEAPON W[NW];
static IMPAI_FDMG    D[ND];
static IMPAI_FCONTENT C = { T, NT, W, NW, D, ND };
static uint32_t s_seed = 12345u;
static int s_fail;

static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 8;
}

static unsigned below(unsigned n) { return rnd() % n; }

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void fail(const char* what, int i)
{
    printf("FAIL %s (fight %d)\n", what, i);
    s_fail++;
}

static void make_content(void)
{
    static const uint32_t kinds[] = { TASIM_WF_BEAM, 0, TASIM_WF_BALLISTIC, TASIM_WF_GUIDANCE, TASIM_WF_DROPPED,
                                      TASIM_WF_VLAUNCH, TASIM_WF_TRACKS };
    unsigned i, k, nd = 0;
    memset(T, 0, sizeof T);
    memset(W, 0, sizeof W);
    memset(D, 0, sizeof D);
    for (i = 1; i < NW; i++) {
        IMPAI_FWEAPON* w = &W[i];
        w->flags = kinds[below(7)];
        if (below(6) == 0) w->flags |= TASIM_WF_TOAIR;
        if (below(7) == 0) w->flags |= TASIM_WF_WATER;
        w->kind = (uint8_t)impai_fight_kind(w->flags);
        w->range = (int32_t)(150 + below(1050));
        w->velocity = w->kind == IMPAI_FK_BEAM || below(5) == 0 ? 0 : (int32_t)(100000 + below(700000));
        w->reload = (uint16_t)(below(8) == 0 ? 0 : 15 + below(135));
        w->burst = (uint16_t)(below(4));
        w->dmg = (uint16_t)(10 + below(390));
        w->aoe = (uint16_t)(below(3) == 0 ? 0 : 8 + below(120));
        w->edge = (float)below(101) / 100.0f;
        w->accuracy = (uint16_t)below(800);
        w->spray = (uint16_t)below(1200);
        if (i <= 10 && nd + 4 <= ND) {     /* rows for 1..4 types, ascending */
            unsigned t = 1 + below(5), n = 1 + below(4);
            w->d0 = nd;
            for (k = 0; k < n && t < NT; k++, t += 1 + below(6)) {
                D[nd].type = (uint16_t)t;
                D[nd].dmg = below(5) == 0 ? 0 : (int32_t)(5 + below(900));
                nd++;
            }
            w->nd = (uint16_t)(nd - w->d0);
        }
    }
    for (i = 1; i < NT; i++) {
        IMPAI_FTYPE* t = &T[i];
        const unsigned kind = below(10);
        unsigned nwp = 1 + below(3);
        t->hp = 200 + below(3800);
        t->value = (float)(50 + below(1450));
        t->fx = (uint8_t)(1 + below(6));
        t->fz = (uint8_t)(1 + below(6));
        t->role = (uint8_t)below(TASIM_ROLES);
        if (kind < 6) {                    /* ground mobile */
            t->flags = IMPAI_FT_MOBILE;
            t->speed = (int32_t)(32768 + below(163840));
            t->layer = TASIM_LAYER_LAND;
        } else if (kind == 6) {            /* a structure */
            t->layer = TASIM_LAYER_LAND;
        } else if (kind == 7) {            /* an aircraft */
            t->flags = IMPAI_FT_MOBILE | IMPAI_FT_FLIES;
            t->speed = (int32_t)(200000 + below(200000));
            t->layer = TASIM_LAYER_AIR;
        } else {                           /* a ship, every other one submerged */
            t->flags = IMPAI_FT_MOBILE | (kind == 9 ? IMPAI_FT_SUBMERGED : 0u);
            t->speed = (int32_t)(40000 + below(100000));
            t->layer = TASIM_LAYER_SEA;
        }
        if (below(8) == 0) nwp = 0;
        for (k = 0; k < nwp; k++) {
            const unsigned slot = below(IMPAI_FW_SLOTS), wi = 1 + below(NW - 1);
            t->w[slot] = (uint16_t)wi;
            if (W[wi].flags & TASIM_WF_TOAIR) t->air |= (uint16_t)(1u << slot);
        }
    }
}

static void make_side(IMPAI_FSIDE* s, unsigned maxe)
{
    unsigned e;
    memset(s, 0, sizeof *s);
    s->ne = (uint8_t)(1 + below(maxe));
    s->closes = (uint8_t)(below(4) != 0);
    for (e = 0; e < s->ne; e++) {
        s->e[e].type = (uint16_t)(1 + below(NT - 1));
        s->e[e].n = (uint16_t)(1 + below(12));
        s->e[e].hp_frac = (uint16_t)(below(3) == 0 ? 20000 + below(45535) : 65535);
    }
}

static void check_run(int i, const IMPAI_FSIDE* a, const IMPAI_FSIDE* b, int32_t dist, IMPAI_FRESULT* r)
{
    IMPAI_FSTATS st;
    IMPAI_FRESULT back;
    const int rc = impai_fight_run(&C, &impai_fitparams, a, b, dist, r, &st);
    printf("fight %d rc=%d sa=%08X sb=%08X m=%08X steps=%u killed=%u,%u shots=%08X,%08X\n", i, rc, bits(r->sa),
           bits(r->sb), bits(r->m), r->steps, st.killed[0], st.killed[1], bits(st.shots[0]), bits(st.shots[1]));
    if (rc) return;
    if (r->steps > IMPAI_FSTEPS) fail("steps past the bound", i);
    if (!(r->sa >= 0.0f && r->sa <= 1.0f && r->sb >= 0.0f && r->sb <= 1.0f)) fail("a share outside [0, 1]", i);
    if ((float)st.killed[0] > st.shots[0] * 1.00001f + 0.001f || (float)st.killed[1] > st.shots[1] * 1.00001f + 0.001f)
        fail("more kills than shots", i);
    if (impai_fight_predict(&C, &impai_fitparams, b, a, dist, &back) || back.m != -r->m)
        fail("swapping the sides does not negate the margin", i);
}

int main(void)
{
    IMPAI_FSIDE a, b, two;
    IMPAI_FRESULT r;
    IMPAI_FENTRY many[20];
    float rate;
    int32_t range;
    int i;
    unsigned e;
    make_content();
    for (i = 0; i < 200; i++) {
        const int32_t dist = (int32_t)below(2400);
        make_side(&a, 4);
        make_side(&b, 4);
        check_run(i, &a, &b, dist, &r);
        if (i % 5 == 0) {                  /* the mirror */
            if (impai_fight_predict(&C, &impai_fitparams, &a, &a, dist, &r) || bits(r.m) != 0u ||
                bits(r.sa) != bits(r.sb))
                fail("a mirror fight's margin is not 0", i);
        }
        if (i % 5 == 1) {                  /* doubled against itself, with and without splash */
            IMPAI_FIT no_splash = impai_fitparams;
            two = a;
            for (e = 0; e < two.ne; e++) two.e[e].n = (uint16_t)(2 * two.e[e].n);
            two.closes = a.closes = 1;
            no_splash.splash = 0.0f;
            if (impai_fight_predict(&C, &impai_fitparams, &two, &a, dist, &r) || r.m < 0.0f ||
                impai_fight_predict(&C, &no_splash, &two, &a, dist, &r) || r.m < 0.0f)
                fail("a doubled side loses", i);
        }
        if (i % 5 == 2) {                  /* out of range, neither closing */
            a.closes = b.closes = 0;
            if (impai_fight_predict(&C, &impai_fitparams, &a, &b, 100000, &r) || r.steps || bits(r.m) != 0u)
                fail("sides out of range that do not close fought", i);
        }
    }
    /* a doubled side of armed ground units, closing: it wins */
    memset(&a, 0, sizeof a);
    for (e = 1; e < NT && a.ne < 3; e++)
        if (T[e].layer == TASIM_LAYER_LAND && (T[e].flags & IMPAI_FT_MOBILE) &&
            !impai_fight_rate(&C, &impai_fitparams, e, e, &rate, &range) && rate > 0.0f) {
            a.e[a.ne].type = (uint16_t)e;
            a.e[a.ne].n = 4;
            a.e[a.ne++].hp_frac = 65535;
        }
    a.closes = 1;
    two = a;
    for (e = 0; e < two.ne; e++) two.e[e].n = 8;
    if (!a.ne || impai_fight_predict(&C, &impai_fitparams, &two, &a, 600, &r) || !(r.m > 0.0f)) fail("the doubled army", -1);
    printf("doubled army: types=%u m=%08X steps=%u\n", a.ne, bits(r.m), r.steps);
    /* the refusals */
    b = a;
    b.e[0].type = NT;
    if (impai_fight_predict(&C, &impai_fitparams, &a, &b, 600, &r) != -1) fail("a type past the content", -1);
    b = a;
    b.ne = IMPAI_FSIDE_TYPES + 1;
    if (impai_fight_predict(&C, &impai_fitparams, &a, &b, 600, &r) != -1) fail("a side past its entries", -1);
    {
        const uint16_t keep = T[a.e[0].type].w[0];
        const unsigned kept_d0 = W[1].d0;
        T[a.e[0].type].w[0] = NW;
        if (impai_fight_predict(&C, &impai_fitparams, &a, &a, 600, &r) != -1) fail("a weapon past the content", -1);
        T[a.e[0].type].w[0] = keep;
        W[1].d0 = ND;
        W[1].nd = W[1].nd ? W[1].nd : 1;
        for (e = 1; e < NT; e++)
            for (i = 0; i < IMPAI_FW_SLOTS; i++)
                if (T[e].w[i] == 1) b.e[0].type = (uint16_t)e;
        b.ne = 1;
        b.e[0].n = 1;
        if (b.e[0].type < NT && T[b.e[0].type].hp &&
            impai_fight_predict(&C, &impai_fitparams, &b, &a, 600, &r) != -1)
            fail("a damage span past the pool", -1);
        W[1].d0 = kept_d0;
    }
    /* impai_expf */
    for (i = -400; i <= 400; i++) {
        const float x = (float)i / 20.0f, y = impai_expf(x);
        const double want = exp((double)x);
        if (fabs((double)y - want) > want * 2.384185791015625e-07) fail("impai_expf past 2^-22", i);
        if (i % 40 == 0) printf("expf %d %08X\n", i, bits(y));
    }
    /* the merge */
    for (e = 0; e < 20; e++) {
        many[e].type = (uint16_t)(1 + e % (NT - 1));
        many[e].n = (uint16_t)(1 + e % 7);
        many[e].hp_frac = 65535;
    }
    if (impai_fight_side(&C, many, 20, 1, &a)) fail("the merge", -1);
    printf("merge ne=%u:", a.ne);
    for (e = 0; e < a.ne; e++) printf(" %u*%u", a.e[e].type, a.e[e].n);
    printf("\n");
    if (a.ne != IMPAI_FSIDE_TYPES) fail("the merge kept another count", -1);
    many[3].type = NT;
    if (impai_fight_side(&C, many, 20, 1, &a) != -1) fail("the merge took a type past the content", -1);
    printf("impai_fight_test: %s\n", s_fail ? "FAIL" : "ok");
    return s_fail ? 1 : 0;
}
