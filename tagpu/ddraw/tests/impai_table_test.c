/* impai_table_test.c -- the classes, the picture's features, the response table's shares, the type choice, the
   buy and the table file (src/impai_table.h), on hand cases with known answers, and A2's hand-written composition
   against IMPAI_TABLE_HAND on 50 fixed pictures. Engine-free, so the same source builds natively, under
   AddressSanitizer and UndefinedBehaviorSanitizer, and with the DLL's compiler under Wine
   (tests/impai_table_test.py); each build prints the same transcript, which the driver compares.

       impai_table_test              the cases; exit 0 when every one holds
       impai_table_test hand PATH    write IMPAI_TABLE_HAND as a table file
       impai_table_test load PATH    load a table file: "ok <hash>" and its numbers, or "refused <code> <why>"

   THE PREDICTOR here is the test's own: a pair rate from a fixed formula and an exponential by halving, a
   degree-6 Taylor polynomial and squaring back, each rounded once per operation on SSE2 in every build. It
   stands in for the fight predictor's (A2a) only to exercise the choice; its numbers mean nothing. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "impai_table.h"

static int fails, checks;

#define CHECK(c, ...)                                                                                       \
    do {                                                                                                    \
        checks++;                                                                                           \
        if (!(c)) {                                                                                         \
            fails++;                                                                                        \
            printf("FAIL line %d: ", __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                            \
            printf("\n");                                                                                   \
        }                                                                                                   \
    } while (0)

static int near(float a, float b)
{
    return fabsf(a - b) <= 1e-6f;
}

/* ---- the test's predictor ---- */

static float test_expf(float x)
{
    float y = x / 16.0f, p;
    int i;
    p = 1.0f + y * (1.0f + y * (0.5f + y * (1.0f / 6.0f + y * (1.0f / 24.0f + y * (1.0f / 120.0f + y / 720.0f)))));
    for (i = 0; i < 4; i++) p = p * p;
    return p;
}

#define NT 16
static float RATE[NT][NT];

static float test_rate(const void* ctx, uint16_t i, uint16_t j)
{
    (void)ctx;
    return i < NT && j < NT ? RATE[i][j] : 0.0f;
}

static const IMPAI_PREDICTOR PRED = { test_rate, test_expf, NULL };

/* ---- the classes ---- */

static void unit(TASIM_ROLE_IN* in, TASIM_ROLE_OUT* out, TASIM_ROLE role, TASIM_LAYER layer, uint32_t rflags,
                 uint32_t wflags)
{
    memset(in, 0, sizeof *in);
    memset(out, 0, sizeof *out);
    out->role = role;
    out->layer = layer;
    out->flags = rflags;
    if (wflags != 0xFFFFFFFFu) {
        in->nweapons = 1;
        in->w[0].flags = wflags;
    }
}

static void test_classes(void)
{
    static const struct { TASIM_ROLE role; TASIM_LAYER layer; uint32_t rflags, wflags; unsigned want; const char* what; }
    C[] = {
        { TASIM_ROLE_RAIDER,    TASIM_LAYER_LAND, TASIM_RF_MOBILE, 0, IMPAI_K_LRAIDER,  "land raider" },
        { TASIM_ROLE_ASSAULT,   TASIM_LAYER_LAND, TASIM_RF_MOBILE, 0, IMPAI_K_LASSAULT, "land assault" },
        { TASIM_ROLE_ARTILLERY, TASIM_LAYER_LAND, TASIM_RF_MOBILE, TASIM_WF_BALLISTIC, IMPAI_K_LARTY, "land artillery" },
        { TASIM_ROLE_ANTIAIR,   TASIM_LAYER_LAND, TASIM_RF_MOBILE, TASIM_WF_TOAIR, IMPAI_K_LAA, "land anti-air" },
        { TASIM_ROLE_SCOUT,     TASIM_LAYER_LAND, TASIM_RF_MOBILE, 0xFFFFFFFFu, IMPAI_K_LSCOUT, "land scout" },
        { TASIM_ROLE_RAIDER,    TASIM_LAYER_SEA,  TASIM_RF_MOBILE, 0, IMPAI_K_SRAIDER,  "sea raider" },
        { TASIM_ROLE_ASSAULT,   TASIM_LAYER_SEA,  TASIM_RF_MOBILE, 0, IMPAI_K_SASSAULT, "sea assault" },
        { TASIM_ROLE_ARTILLERY, TASIM_LAYER_SEA,  TASIM_RF_MOBILE, 0, IMPAI_K_SARTY,    "sea artillery" },
        { TASIM_ROLE_ANTIAIR,   TASIM_LAYER_SEA,  TASIM_RF_MOBILE, TASIM_WF_TOAIR, IMPAI_K_SAA, "sea anti-air" },
        { TASIM_ROLE_SCOUT,     TASIM_LAYER_SEA,  TASIM_RF_MOBILE, 0xFFFFFFFFu, IMPAI_K_NONE, "a sea scout is no class" },
        { TASIM_ROLE_ANTIAIR,   TASIM_LAYER_AIR,  TASIM_RF_MOBILE, TASIM_WF_TOAIR, IMPAI_K_FIGHTER, "fighter" },
        { TASIM_ROLE_ANTIAIR,   TASIM_LAYER_AIR,  TASIM_RF_MOBILE, TASIM_WF_TOAIR | TASIM_WF_DROPPED, IMPAI_K_FIGHTER,
          "anti-air is tested before the drop" },
        { TASIM_ROLE_ASSAULT,   TASIM_LAYER_AIR,  TASIM_RF_MOBILE, TASIM_WF_DROPPED, IMPAI_K_BOMBER, "bomber" },
        { TASIM_ROLE_SCOUT,     TASIM_LAYER_AIR,  TASIM_RF_MOBILE, TASIM_WF_DROPPED, IMPAI_K_BOMBER,
          "the drop is tested before the scout" },
        { TASIM_ROLE_SCOUT,     TASIM_LAYER_AIR,  TASIM_RF_MOBILE, 0xFFFFFFFFu, IMPAI_K_ASCOUT, "air scout" },
        { TASIM_ROLE_RAIDER,    TASIM_LAYER_AIR,  TASIM_RF_MOBILE, 0, IMPAI_K_GUNSHIP, "gunship" },
        { TASIM_ROLE_RAIDER,    TASIM_LAYER_AIR,  TASIM_RF_MOBILE, TASIM_WF_DROPPED | IMPAI_WF_BEAMWEAPON,
          IMPAI_K_GUNSHIP, "a dropped beam is a beam" },
        { TASIM_ROLE_ARTILLERY, TASIM_LAYER_AIR,  TASIM_RF_MOBILE, TASIM_WF_DROPPED | TASIM_WF_INTERCEPTOR,
          IMPAI_K_GUNSHIP, "an interceptor's drop does not count" },
        { TASIM_ROLE_BUILDER,   TASIM_LAYER_LAND, TASIM_RF_MOBILE, 0xFFFFFFFFu, IMPAI_K_NONE, "builder" },
        { TASIM_ROLE_DEFENCE,   TASIM_LAYER_LAND, 0, 0, IMPAI_K_NONE, "defence" },
        { TASIM_ROLE_OTHER,     TASIM_LAYER_LAND, TASIM_RF_MOBILE, 0xFFFFFFFFu, IMPAI_K_NONE, "other" },
        { TASIM_ROLE_RAIDER,    TASIM_LAYER_LAND, 0, 0, IMPAI_K_NONE, "not mobile" },
        { TASIM_ROLE_ASSAULT,   TASIM_LAYER_LAND, TASIM_RF_MOBILE | TASIM_RF_COMMANDER, 0, IMPAI_K_NONE, "commander" },
        { TASIM_ROLE_ASSAULT,   TASIM_LAYER_AIR,  TASIM_RF_MOBILE | TASIM_RF_TRANSPORT, 0, IMPAI_K_NONE, "transport" },
        { TASIM_ROLE_RAIDER,    TASIM_LAYER_LAND, TASIM_RF_MOBILE | TASIM_RF_KAMIKAZE, 0, IMPAI_K_NONE, "kamikaze" },
        { TASIM_ROLE_ASSAULT,   TASIM_LAYER_LAND, TASIM_RF_MOBILE, TASIM_WF_STOCKPILE, IMPAI_K_NONE, "stockpile" },
    };
    unsigned i;
    for (i = 0; i < sizeof C / sizeof C[0]; i++) {
        TASIM_ROLE_IN in;
        TASIM_ROLE_OUT out;
        unsigned got;
        unit(&in, &out, C[i].role, C[i].layer, C[i].rflags, C[i].wflags);
        got = impai_class_of(&in, &out);
        printf("class %-40s %s\n", C[i].what, impai_class_name(got));
        CHECK(got == C[i].want, "%s: %s, want %s", C[i].what, impai_class_name(got), impai_class_name(C[i].want));
    }
}

/* The classifier and the classes end to end on a tiny mod: a commander, its factory and an army on each layer. */
static void test_classes_from_the_classifier(void)
{
    enum { COM, FAC, RAID, TANK, ARTY, MISS, SCOUT, FIGHT, BOMB, PEEP, GUN, N };
    static const uint16_t com_builds[] = { FAC }, fac_builds[] = { RAID, TANK, ARTY, MISS, SCOUT, FIGHT, BOMB,
                                                                   PEEP, GUN };
    static const unsigned want[N] = { IMPAI_K_NONE, IMPAI_K_NONE, IMPAI_K_LRAIDER, IMPAI_K_LASSAULT, IMPAI_K_LARTY,
                                      IMPAI_K_LAA, IMPAI_K_LSCOUT, IMPAI_K_FIGHTER, IMPAI_K_BOMBER, IMPAI_K_ASCOUT,
                                      IMPAI_K_GUNSHIP };
    TASIM_ROLE_IN in[N];
    TASIM_ROLE_OUT out[N];
    TASIM_ROLE_Q q;
    void* work = malloc(tasim_roles_work(N));
    int i, rc;
    memset(in, 0, sizeof in);
    for (i = 0; i < N; i++) {
        in[i].flags245 = TASIM_F245_CANMOVE;
        in[i].bmcode = 1;
        in[i].cost_metal = 100.0f;
        in[i].maxdamage = 1000;
        in[i].maxvelocity_fx = 65536;
        in[i].nweapons = 1;
        in[i].w[0].range = 300;
        in[i].w[0].reload_ticks = 30.0f;
        in[i].w[0].burst = 1;
        in[i].w[0].damage_default = 50;
        in[i].w[0].damage_air = 50.0f;
    }
    in[COM].flags245 |= TASIM_F245_COMMANDER;
    in[COM].flags241 = TASIM_F241_BUILDER;
    in[COM].nbuild = 1;
    in[COM].build = com_builds;
    in[FAC].bmcode = 0;
    in[FAC].flags241 = TASIM_F241_BUILDER;
    in[FAC].nweapons = 0;
    in[FAC].nbuild = sizeof fac_builds / sizeof fac_builds[0];
    in[FAC].build = fac_builds;
    in[RAID].maxvelocity_fx = 3 * 65536;
    in[RAID].cost_metal = 120.0f;
    in[TANK].maxvelocity_fx = in[ARTY].maxvelocity_fx = 32768;   /* below the land median: assault */
    in[ARTY].w[0].range = 1200;
    in[ARTY].w[0].flags = TASIM_WF_BALLISTIC;
    in[MISS].w[0].flags = TASIM_WF_TOAIR;
    in[SCOUT].nweapons = 0;
    in[SCOUT].maxvelocity_fx = 4 * 65536;
    in[SCOUT].cost_metal = 30.0f;
    for (i = FIGHT; i <= GUN; i++) in[i].flags241 = TASIM_F241_CANFLY;
    in[FIGHT].w[0].flags = TASIM_WF_TOAIR;
    in[BOMB].w[0].flags = TASIM_WF_DROPPED;
    in[BOMB].cost_metal = in[GUN].cost_metal = 300.0f;            /* above the air's tenth: not a scout */
    in[PEEP].nweapons = 0;
    rc = tasim_roles_classify(in, N, out, &q, work);
    CHECK(rc == 0, "the classifier refused the mod: %d", rc);
    for (i = 0; i < N; i++) {
        unsigned got = impai_class_of(&in[i], &out[i]);
        printf("classified %2d %-10s %-6s %s\n", i, tasim_roles_name(out[i].role),
               tasim_roles_layer_name(out[i].layer), impai_class_name(got));
        CHECK(got == want[i], "type %d: %s, want %s", i, impai_class_name(got), impai_class_name(want[i]));
    }
    free(work);
}

/* ---- the picture ---- */

static void test_picture(void)
{
    IMPAI_PICTURE_IN in;
    IMPAI_PICTURE p;
    memset(&in, 0, sizeof in);
    memset(&p, 0, sizeof p);
    in.total = 1000.0f;
    in.kval[IMPAI_K_LRAIDER] = 100.0f;
    in.kval[IMPAI_K_LASSAULT] = 200.0f;
    in.kval[IMPAI_K_LARTY] = 50.0f;
    in.kval[IMPAI_K_SRAIDER] = 30.0f;
    in.kval[IMPAI_K_SAA] = 20.0f;
    in.kval[IMPAI_K_FIGHTER] = 40.0f;
    in.kval[IMPAI_K_BOMBER] = 60.0f;
    in.kval[IMPAI_K_GUNSHIP] = 10.0f;
    in.kval[IMPAI_K_LAA] = 25.0f;
    in.defence = 300.0f;
    in.aa_struct = 75.0f;
    in.own_extractors = 6;
    in.enemy_extractors = 4;
    in.gametime = 36000;
    in.tech = 2;
    impai_picture_features(&in, &p);
    CHECK(near(p.f[IMPAI_F_LRAIDER], 0.1f) && near(p.f[IMPAI_F_LASSAULT], 0.2f) && near(p.f[IMPAI_F_LARTY], 0.05f),
          "land shares");
    CHECK(near(p.f[IMPAI_F_SEA], 0.05f), "sea share %g", p.f[IMPAI_F_SEA]);
    CHECK(near(p.f[IMPAI_F_FIGHTER], 0.04f) && near(p.f[IMPAI_F_BOMBER], 0.06f) && near(p.f[IMPAI_F_GUNSHIP], 0.01f),
          "air shares");
    CHECK(near(p.f[IMPAI_F_DEFENCE], 0.3f) && near(p.f[IMPAI_F_AA], 0.1f), "defence %g, aa %g",
          p.f[IMPAI_F_DEFENCE], p.f[IMPAI_F_AA]);
    CHECK(near(p.f[IMPAI_F_INCOME], 0.75f), "income %g", p.f[IMPAI_F_INCOME]);
    CHECK(near(p.minute, 20.0f) && near(p.f[IMPAI_F_TIME], 0.5f), "minute %g time %g", p.minute, p.f[IMPAI_F_TIME]);
    CHECK(near(p.f[IMPAI_F_TECH], 2.0f / 3.0f), "tech %g", p.f[IMPAI_F_TECH]);
    in.enemy_extractors = 1;
    in.own_extractors = 9;
    impai_picture_features(&in, &p);
    CHECK(p.f[IMPAI_F_INCOME] == 1.0f, "the income's ratio is capped at 2: %g", p.f[IMPAI_F_INCOME]);
    in.enemy_extractors = 0;
    impai_picture_features(&in, &p);
    CHECK(p.f[IMPAI_F_INCOME] == 1.0f, "no enemy extractor seen, one held: %g", p.f[IMPAI_F_INCOME]);
    in.own_extractors = 0;
    impai_picture_features(&in, &p);
    CHECK(p.f[IMPAI_F_INCOME] == 0.5f, "neither: %g", p.f[IMPAI_F_INCOME]);
    in.total = 0.0f;
    in.gametime = 200000;
    in.tech = 7;
    impai_picture_features(&in, &p);
    CHECK(p.f[IMPAI_F_LRAIDER] == 0.0f && p.f[IMPAI_F_AA] == 0.0f, "no value seen: every share 0");
    CHECK(p.f[IMPAI_F_TIME] == 1.0f && p.f[IMPAI_F_TECH] == 1.0f, "time and tech capped at 1");
    printf("picture: features hold\n");
}

/* ---- the shares ---- */

#define LANDMASK ((1u << IMPAI_K_LRAIDER) | (1u << IMPAI_K_LASSAULT) | (1u << IMPAI_K_LARTY) | (1u << IMPAI_K_LAA) | \
                  (1u << IMPAI_K_LSCOUT))
#define SEAMASK  ((1u << IMPAI_K_SRAIDER) | (1u << IMPAI_K_SASSAULT) | (1u << IMPAI_K_SARTY) | (1u << IMPAI_K_SAA))

static void print_shares(const char* what, const float s[IMPAI_KS])
{
    unsigned k;
    printf("shares %-34s", what);
    for (k = 0; k < IMPAI_KS; k++) printf(" %.9g", s[k]);
    printf("\n");
}

static float sum(const float s[IMPAI_KS])
{
    float t = 0.0f;
    unsigned k;
    for (k = 0; k < IMPAI_KS; k++) t += s[k];
    return t;
}

static void test_shares(void)
{
    IMPAI_PICTURE p;
    IMPAI_TABLE t = IMPAI_TABLE_HAND;
    float s[IMPAI_KS];
    memset(&p, 0, sizeof p);

    impai_table_shares(&t, &p, LANDMASK, s);
    print_shares("hand, land, nothing seen", s);
    CHECK(near(s[IMPAI_K_LRAIDER], 0.25f / 0.9f) && near(s[IMPAI_K_LASSAULT], 0.45f / 0.9f) &&
          near(s[IMPAI_K_LARTY], 0.15f / 0.9f) && near(s[IMPAI_K_LSCOUT], 0.05f / 0.9f) && s[IMPAI_K_LAA] == 0.0f,
          "the base shares, normalised");
    CHECK(near(sum(s), 1.0f), "they sum to 1: %g", sum(s));

    p.f[IMPAI_F_FIGHTER] = 0.1f;
    p.f[IMPAI_F_GUNSHIP] = 0.1f;
    impai_table_shares(&t, &p, LANDMASK, s);
    print_shares("hand, land, air 0.2", s);
    CHECK(near(s[IMPAI_K_LAA], 0.3f), "anti-air 1.5 x 0.2: %g", s[IMPAI_K_LAA]);
    CHECK(near(s[IMPAI_K_LASSAULT], 0.45f / 0.9f * 0.7f), "the rest scaled to 0.7: %g", s[IMPAI_K_LASSAULT]);

    p.f[IMPAI_F_BOMBER] = 0.3f;
    impai_table_shares(&t, &p, LANDMASK, s);
    print_shares("hand, land, air 0.5", s);
    CHECK(near(s[IMPAI_K_LAA], 0.35f), "anti-air capped at 0.35: %g", s[IMPAI_K_LAA]);
    CHECK(near(sum(s), 1.0f), "they sum to 1: %g", sum(s));

    impai_table_shares(&t, &p, LANDMASK & ~(1u << IMPAI_K_LAA), s);
    print_shares("hand, no anti-air buildable", s);
    CHECK(s[IMPAI_K_LAA] == 0.0f && near(s[IMPAI_K_LRAIDER], 0.25f / 0.9f), "no anti-air, the rest normalised");

    impai_table_shares(&t, &p, 1u << IMPAI_K_LAA, s);
    CHECK(near(s[IMPAI_K_LAA], 1.0f), "only anti-air buildable, and air seen: all of it (%g)", s[IMPAI_K_LAA]);
    memset(&p, 0, sizeof p);
    impai_table_shares(&t, &p, 1u << IMPAI_K_LAA, s);
    CHECK(sum(s) == 0.0f, "only anti-air buildable, no air seen: nothing to buy");
    impai_table_shares(&t, &p, 0u, s);
    CHECK(sum(s) == 0.0f, "nothing buildable: nothing to buy");

    p.f[IMPAI_F_FIGHTER] = 0.2f;
    impai_table_shares(&t, &p, LANDMASK | SEAMASK, s);
    print_shares("hand, land and sea, air 0.2", s);
    CHECK(near(s[IMPAI_K_LAA], 0.15f) && near(s[IMPAI_K_SAA], 0.15f),
          "the AA group's 0.3 shared by its two buildable members: %g %g", s[IMPAI_K_LAA], s[IMPAI_K_SAA]);

    /* the air classes: none before air_from_t */
    memset(&p, 0, sizeof p);
    t.base[IMPAI_K_FIGHTER] = 0.2f;
    p.minute = 9.5f;
    impai_table_shares(&t, &p, LANDMASK | IMPAI_K_AIRMASK, s);
    CHECK(s[IMPAI_K_FIGHTER] == 0.0f, "no fighter before minute 10: %g", s[IMPAI_K_FIGHTER]);
    p.minute = 10.0f;
    impai_table_shares(&t, &p, LANDMASK | IMPAI_K_AIRMASK, s);
    CHECK(near(s[IMPAI_K_FIGHTER], 0.2f / 1.1f), "fighters from minute 10: %g", s[IMPAI_K_FIGHTER]);

    /* the bombers' response: defences the anti-air does not cover */
    t = IMPAI_TABLE_HAND;
    t.r[IMPAI_R_BOMBER_LOWAA].gain = 1.0f;
    p.minute = 30.0f;
    p.f[IMPAI_F_DEFENCE] = 0.5f;
    p.f[IMPAI_F_AA] = 0.2f;
    impai_table_shares(&t, &p, LANDMASK | IMPAI_K_AIRMASK, s);
    CHECK(near(s[IMPAI_K_BOMBER], 0.3f / 1.2f), "bombers 1 x (0.5 - 0.2) of 1.2: %g", s[IMPAI_K_BOMBER]);
    p.f[IMPAI_F_AA] = 0.7f;
    impai_table_shares(&t, &p, LANDMASK | IMPAI_K_AIRMASK, s);
    CHECK(s[IMPAI_K_BOMBER] == 0.0f, "anti-air covering the defences: no bombers (%g)", s[IMPAI_K_BOMBER]);

    /* a negative gain clamps the share at 0, a large one at 1 */
    t = IMPAI_TABLE_HAND;
    t.r[IMPAI_R_LASSAULT_LRAIDER].gain = -5.0f;
    memset(&p, 0, sizeof p);
    p.f[IMPAI_F_LRAIDER] = 0.5f;
    impai_table_shares(&t, &p, LANDMASK, s);
    CHECK(s[IMPAI_K_LASSAULT] == 0.0f, "assault clamped at 0: %g", s[IMPAI_K_LASSAULT]);
    t.r[IMPAI_R_LASSAULT_LRAIDER].gain = 9.0f;
    impai_table_shares(&t, &p, LANDMASK, s);
    CHECK(near(s[IMPAI_K_LASSAULT], 1.0f / 1.45f), "assault clamped at 1 before normalising: %g", s[IMPAI_K_LASSAULT]);

    /* the sea group answers seen sea */
    t = IMPAI_TABLE_HAND;
    t.r[IMPAI_R_SEA_SEA].gain = 0.4f;
    memset(&p, 0, sizeof p);
    p.f[IMPAI_F_SEA] = 1.0f;
    impai_table_shares(&t, &p, SEAMASK & ~(1u << IMPAI_K_SAA), s);
    print_shares("sea response, three sea classes", s);
    CHECK(near(s[IMPAI_K_SRAIDER], (0.25f + 0.4f / 3.0f) / (0.85f + 0.4f)), "sea raiders: %g", s[IMPAI_K_SRAIDER]);
}

/* ---- the type choice and the buy ---- */

/* the menu: types 1..9 on land, 10..11 in the air */
static IMPAI_MENU_TYPE MENU[] = {
    { 3, IMPAI_K_LRAIDER, 0, 100.0f },  /* listed before type 1, its twin: the tie goes to type 1          */
    { 1, IMPAI_K_LRAIDER, 0, 100.0f },
    { 2, IMPAI_K_LRAIDER, 0, 50.0f },
    { 4, IMPAI_K_LASSAULT, 0, 250.0f },
    { 5, IMPAI_K_LASSAULT, 0, 400.0f },
    { 6, IMPAI_K_LARTY, 0, 300.0f },
    { 7, IMPAI_K_LAA, 0, 150.0f },
    { 8, IMPAI_K_LSCOUT, 0, 40.0f },
    { 9, IMPAI_K_LSCOUT, 0, 0.0f },     /* no value: never chosen                                          */
    { 10, IMPAI_K_FIGHTER, 0, 200.0f },
    { 11, IMPAI_K_BOMBER, 0, 350.0f },
};
#define NMENU (sizeof MENU / sizeof MENU[0])

static void rates(void)
{
    int i, j;
    for (i = 0; i < NT; i++)
        for (j = 0; j < NT; j++) RATE[i][j] = (float)((i * 7 + j * 3) % 11) / 10.0f;
    /* the raiders against enemy type 12: type 1 and its twin 3 counter it, type 2 barely */
    RATE[1][12] = RATE[3][12] = 2.0f;
    RATE[2][12] = 0.4f;
}

static void test_type_choice(void)
{
    IMPAI_MENU m = { MENU, NMENU };
    IMPAI_TABLE t = IMPAI_TABLE_HAND;
    IMPAI_PICTURE p;
    int got;
    rates();
    memset(&p, 0, sizeof p);
    p.ne = 1;
    p.e[0].type = 12;
    p.e[0].share = 1.0f;

    t.counter = 0.0f;
    got = impai_class_type(&t, &p, &m, IMPAI_K_LRAIDER, &PRED);
    CHECK(got == 2, "counter 0 buys the cheapest raider: %d", got);
    printf("type choice: counter 0 -> %d", got);
    t.counter = 5.0f;
    got = impai_class_type(&t, &p, &m, IMPAI_K_LRAIDER, &PRED);
    CHECK(got == 1, "counter 5 buys the best counter, the lower of the twins: %d", got);
    printf(", counter 5 -> %d\n", got);
    /* e_2 = 0.2: exp(5) / 100 against exp(1) / 50, and at counter 1 exp(1) / 100 against exp(0.2) / 50 */
    t.counter = 1.0f;
    got = impai_class_type(&t, &p, &m, IMPAI_K_LRAIDER, &PRED);
    CHECK(got == 1, "counter 1: e / 100 = 0.0272 beats exp(0.2) / 50 = 0.0244: %d", got);
    t.counter = 0.5f;
    got = impai_class_type(&t, &p, &m, IMPAI_K_LRAIDER, &PRED);
    CHECK(got == 2, "counter 0.5: exp(0.5) / 100 = 0.0165 loses to exp(0.1) / 50 = 0.0221: %d", got);
    t.counter = 5.0f;
    got = impai_class_type(&t, &p, &m, IMPAI_K_LRAIDER, NULL);
    CHECK(got == 2, "no predictor: the cheapest: %d", got);
    p.ne = 0;
    got = impai_class_type(&t, &p, &m, IMPAI_K_LRAIDER, &PRED);
    CHECK(got == 2, "no enemy seen: the cheapest: %d", got);
    got = impai_class_type(&t, &p, &m, IMPAI_K_LSCOUT, &PRED);
    CHECK(got == 8, "a type of no value is never chosen: %d", got);
    got = impai_class_type(&t, &p, &m, IMPAI_K_SRAIDER, &PRED);
    CHECK(got == -1, "a class the menu lacks: %d", got);
    p.ne = 1;
    RATE[1][12] = RATE[3][12] = NAN;
    got = impai_class_type(&t, &p, &m, IMPAI_K_LRAIDER, &PRED);
    CHECK(got == 2, "a predictor's NaN counts 0: %d", got);
    rates();
}

static void print_army(const char* what, const IMPAI_ARMY* a)
{
    unsigned k;
    printf("army %-30s units=%u spent=%.9g", what, (unsigned)a->units, a->spent);
    for (k = 0; k < IMPAI_KS; k++)
        if (a->n[k]) printf(" %s:%dx%u", impai_class_name(k), a->type[k], (unsigned)a->n[k]);
    printf("\n");
}

static void test_buy(void)
{
    IMPAI_MENU m = { MENU, NMENU };
    IMPAI_TABLE t = IMPAI_TABLE_HAND;
    IMPAI_PICTURE p;
    IMPAI_ARMY a;
    IMPAI_MENU_TYPE bad[2];
    unsigned k;
    int rc;
    rates();
    memset(&p, 0, sizeof p);
    p.ne = 1;
    p.e[0].type = 12;
    p.e[0].share = 1.0f;

    rc = impai_buy(&t, &p, 3000.0f, &m, &PRED, &a);
    print_army("hand, 3000, nothing seen", &a);
    CHECK(rc == 0 && a.spent <= 3000.0f && a.units > 0, "a buy within the budget");
    CHECK(a.n[IMPAI_K_LAA] == 0 && a.n[IMPAI_K_FIGHTER] == 0 && a.n[IMPAI_K_BOMBER] == 0,
          "the hand table buys no anti-air with no air seen, and no aircraft");
    /* each class's value within the dearest unit bought (300) of its share of the whole: the deficit rule keeps
       every class within a unit of its share until the budget's end, where the class furthest behind may no
       longer fit and a cheaper one takes the remainder */
    {
        float unit_cost[IMPAI_KS] = { 100.0f, 250.0f, 300.0f, 150.0f, 40.0f };
        for (k = 0; k < IMPAI_K_SRAIDER; k++) {
            float v = (float)a.n[k] * unit_cost[k];
            CHECK(fabsf(v - a.share[k] * a.spent) <= 300.0f, "class %s: %g of %g, share %g",
                  impai_class_name(k), v, a.spent, a.share[k]);
        }
    }
    CHECK(3000.0f - a.spent < 40.0f, "no unit fits in what is left: %g", 3000.0f - a.spent);

    p.f[IMPAI_F_BOMBER] = 0.4f;
    rc = impai_buy(&t, &p, 3000.0f, &m, &PRED, &a);
    print_army("hand, 3000, bombers 0.4", &a);
    CHECK(rc == 0 && a.n[IMPAI_K_LAA] > 0 && a.type[IMPAI_K_LAA] == 7, "anti-air bought against bombers");

    t.base[IMPAI_K_BOMBER] = 0.3f;
    p.minute = 12.0f;
    rc = impai_buy(&t, &p, 8000.0f, &m, &PRED, &a);
    print_army("bombers from minute 10, 8000", &a);
    CHECK(rc == 0 && a.n[IMPAI_K_BOMBER] > 0 && a.type[IMPAI_K_BOMBER] == 11, "bombers bought after air_from_t");
    t = IMPAI_TABLE_HAND;

    rc = impai_buy(&t, &p, 0.0f, &m, &PRED, &a);
    CHECK(rc == 0 && a.units == 0, "V = 0 buys nothing");
    rc = impai_buy(&t, &p, 39.0f, &m, &PRED, &a);
    CHECK(rc == 0 && a.units == 0, "V below the cheapest unit buys nothing");
    rc = impai_buy(&t, &p, 1e9f, &m, &PRED, &a);
    CHECK(rc == 0 && a.units == IMPAI_BUY_UNITS, "a huge V stops at the bound: %u", (unsigned)a.units);

    rc = impai_buy(&t, &p, NAN, &m, &PRED, &a);
    CHECK(rc == -1, "a NaN budget is refused");
    bad[0] = MENU[0];
    bad[1] = MENU[1];
    bad[1].klass = IMPAI_KS;
    rc = impai_buy(&t, &p, 1000.0f, &(IMPAI_MENU){ bad, 2 }, &PRED, &a);
    CHECK(rc == -1, "a menu type past the classes is refused");
    p.ne = IMPAI_PIC_ENEMY + 1;
    rc = impai_buy(&t, &p, 1000.0f, &m, &PRED, &a);
    CHECK(rc == -1, "a picture past its enemy types is refused");
    rc = impai_buy(&t, &p, 1000.0f, &(IMPAI_MENU){ MENU, IMPAI_MENU_MAX + 1 }, &PRED, &a);
    CHECK(rc == -1, "a menu past its bound is refused");

    {
        float share[IMPAI_KS] = { 0.5f, 0.5f }, spent[IMPAI_KS] = { 0 }, cost[IMPAI_KS] = { 100.0f, 100.0f };
        CHECK(impai_buy_next(share, spent, cost, 3u, 1e30f) == 0, "a tie goes to the lower class");
        spent[0] = 100.0f;
        CHECK(impai_buy_next(share, spent, cost, 3u, 1e30f) == 1, "then the class behind its share");
        CHECK(impai_buy_next(share, spent, cost, 1u, 1e30f) == 0, "an ineligible class is never named");
        CHECK(impai_buy_next(share, spent, cost, 3u, 99.0f) == -1, "nothing fits");
    }
}

/* A2's hand-written composition (a2-spec.md section 4.3 step 8) as A2 writes it -- raider 0.25, assault 0.45,
   artillery 0.15, scout 0.05, anti-air min(0.35, 1.5 x seen air value / seen enemy value), the rest scaled to
   1 - anti-air -- against IMPAI_TABLE_HAND, on 50 pictures drawn from a fixed generator, each bought through
   the same deficit rule and type choice: the two must buy the same army. */
static uint32_t lcg(uint32_t* s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void test_hand_is_a2(void)
{
    static const float a2_base[IMPAI_KS] = { 0.25f, 0.45f, 0.15f, 0.0f, 0.05f };
    IMPAI_MENU m = { MENU, 8 };         /* the land types */
    uint32_t seed = 12345u;
    int i, same = 0;
    rates();
    for (i = 0; i < 50; i++) {
        IMPAI_PICTURE p;
        IMPAI_ARMY a;
        float want[IMPAI_KS], spent[IMPAI_KS], cost[IMPAI_KS], air, aa, V, left;
        uint16_t n[IMPAI_KS];
        int type[IMPAI_KS], k, f, equal = 1;
        memset(&p, 0, sizeof p);
        for (f = 0; f < IMPAI_FEATURES; f++) p.f[f] = (float)(lcg(&seed) % 1000u) / 4000.0f;
        p.minute = (float)(lcg(&seed) % 45u);
        p.ne = 1 + lcg(&seed) % 3u;
        for (f = 0; f < (int)p.ne; f++) {
            p.e[f].type = (uint16_t)(1 + lcg(&seed) % 13u);
            p.e[f].share = 1.0f / (float)p.ne;
        }
        V = (float)(1000 + lcg(&seed) % 9000u);
        impai_buy(&IMPAI_TABLE_HAND, &p, V, &m, &PRED, &a);

        air = p.f[IMPAI_F_FIGHTER] + p.f[IMPAI_F_BOMBER] + p.f[IMPAI_F_GUNSHIP];
        aa = 1.5f * air < 0.35f ? 1.5f * air : 0.35f;
        for (k = 0; k < IMPAI_KS; k++) {
            want[k] = k == IMPAI_K_LAA ? aa : a2_base[k] * (1.0f - aa) / 0.9f;
            type[k] = want[k] > 0.0f ? impai_class_type(&IMPAI_TABLE_HAND, &p, &m, (unsigned)k, &PRED) : -1;
            cost[k] = 0.0f;
            spent[k] = 0.0f;
            n[k] = 0;
            for (f = 0; type[k] >= 0 && f < (int)m.n; f++)
                if (m.t[f].type == type[k]) cost[k] = m.t[f].value;
        }
        for (left = V;;) {
            int c = impai_buy_next(want, spent, cost, (1u << IMPAI_KS) - 1u, left);
            if (c < 0) break;
            n[c]++;
            spent[c] += cost[c];
            left = V - (spent[0] + spent[1] + spent[2] + spent[3] + spent[4]);
        }
        for (k = 0; k < IMPAI_KS; k++) {
            if (a.n[k] != n[k] || (n[k] && a.type[k] != type[k])) equal = 0;
            if (fabsf(a.share[k] - want[k]) > 1e-6f) equal = 0;
        }
        if (!equal) {
            print_army("table", &a);
            printf("a2   ");
            for (k = 0; k < IMPAI_KS; k++) printf(" %s:%dx%u", impai_class_name((unsigned)k), type[k], n[k]);
            printf("\n");
        }
        CHECK(equal, "picture %d (V %g): the hand table and A2's composition buy different armies", i, V);
        same += equal;
    }
    printf("hand table = A2's composition on %d of 50 pictures\n", same);
}

/* ---- the file ---- */

static void rehash(uint8_t f[IMPAI_TABLE_FILE_BYTES])
{
    IMPAI_TABLE t;
    memcpy(&t, f, IMPAI_TABLE_BYTES);
    impai_table_file(&t, f);
}

static void test_file(void)
{
    uint8_t f[IMPAI_TABLE_FILE_BYTES], g[IMPAI_TABLE_FILE_BYTES];
    IMPAI_TABLE t, u;
    uint64_t h = 0;
    float nan = NAN, big = 1.5f, neg = -0.5f;
    int rc;
    impai_table_file(&IMPAI_TABLE_HAND, f);
    rc = impai_table_load(f, sizeof f, &t, &h);
    CHECK(rc == IMPAI_TABLE_OK && h == impai_table_hash(&IMPAI_TABLE_HAND) &&
          memcmp(&t, &IMPAI_TABLE_HAND, sizeof t) == 0, "the hand table round-trips");
    printf("table hand    hash=%016llx\n", (unsigned long long)impai_table_hash(&IMPAI_TABLE_HAND));
    printf("table default hash=%016llx\n", (unsigned long long)impai_table_hash(&IMPAI_TABLE_DEFAULT));

    u = IMPAI_TABLE_HAND;
    CHECK(impai_table_load(f, sizeof f - 1, &u, &h) == IMPAI_TABLE_ESIZE, "a short file is refused");
    CHECK(memcmp(&u, &IMPAI_TABLE_HAND, sizeof u) == 0, "a refusal leaves the table untouched");
    memcpy(g, f, sizeof f);
    g[7] ^= 1u;
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_EHASH, "a changed byte is refused by the hash");
    memcpy(g, f, sizeof f);
    g[IMPAI_TABLE_BYTES + 3] ^= 0x80u;
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_EHASH, "a changed trailer is refused");

    memcpy(g, f, sizeof f);
    g[offsetof(IMPAI_TABLE, r) + 8 * 2] = IMPAI_FROWS;
    rehash(g);
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_EROW, "a feature past the count is refused");
    memcpy(g, f, sizeof f);
    g[offsetof(IMPAI_TABLE, r) + 8 * 5 + 1] = IMPAI_KROWS;
    rehash(g);
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_EROW, "a class past the count is refused");
    memcpy(g, f, sizeof f);
    g[offsetof(IMPAI_TABLE, r) + 8 * 5 + 1] = IMPAI_KROWS - 1;
    rehash(g);
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_OK, "the last group is a class a row may name");
    memcpy(g, f, sizeof f);
    g[offsetof(IMPAI_TABLE, r) + 8 * 3 + 2] = 1;
    rehash(g);
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_EPAD, "a pad that is not zero is refused");
    memcpy(g, f, sizeof f);
    memcpy(g + offsetof(IMPAI_TABLE, r) + 8 * 4 + 4, &nan, 4);
    rehash(g);
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_ENAN, "a NaN gain is refused");
    memcpy(g, f, sizeof f);
    memcpy(g + offsetof(IMPAI_TABLE, counter), &nan, 4);
    rehash(g);
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_ENAN, "a NaN counter is refused");
    memcpy(g, f, sizeof f);
    {
        float inf = INFINITY;
        memcpy(g + offsetof(IMPAI_TABLE, base) + 4 * 6, &inf, 4);
    }
    rehash(g);
    CHECK(impai_table_load(g, sizeof g, &u, &h) == IMPAI_TABLE_ENAN, "an infinite share is refused");

    memcpy(g, f, sizeof f);
    memcpy(g + offsetof(IMPAI_TABLE, base) + 4 * 1, &big, 4);
    memcpy(g + offsetof(IMPAI_TABLE, aa_cap), &neg, 4);
    rehash(g);
    rc = impai_table_load(g, sizeof g, &u, &h);
    CHECK(rc == IMPAI_TABLE_OK && u.base[1] == 1.0f && u.aa_cap == 0.0f, "a share and the cap are clamped to [0, 1]");
    printf("file: the refusals hold\n");
}

/* ---- the modes the tools use ---- */

static int write_hand(const char* path)
{
    uint8_t f[IMPAI_TABLE_FILE_BYTES];
    FILE* o = fopen(path, "wb");
    impai_table_file(&IMPAI_TABLE_HAND, f);
    if (!o || fwrite(f, 1, sizeof f, o) != sizeof f || fclose(o) != 0) {
        printf("could not write %s\n", path);
        return 2;
    }
    printf("ok %016llx\n", (unsigned long long)impai_table_hash(&IMPAI_TABLE_HAND));
    return 0;
}

static int load(const char* path)
{
    uint8_t b[IMPAI_TABLE_FILE_BYTES + 1];
    IMPAI_TABLE t;
    uint64_t h = 0;
    size_t n;
    unsigned k;
    int rc;
    FILE* i = fopen(path, "rb");
    if (!i) {
        printf("could not read %s\n", path);
        return 2;
    }
    n = fread(b, 1, sizeof b, i);
    fclose(i);
    rc = impai_table_load(b, n, &t, &h);
    if (rc != IMPAI_TABLE_OK) {
        printf("refused %d %s\n", rc, impai_table_error(rc));
        return 1;
    }
    printf("ok %016llx\nbase", (unsigned long long)h);
    for (k = 0; k < IMPAI_KS; k++) printf(" %.9g", t.base[k]);
    printf("\nrows");
    for (k = 0; k < IMPAI_RESPONSES; k++) printf(" %u:%u:%.9g", t.r[k].feature, t.r[k].klass, t.r[k].gain);
    printf("\ncounter %.9g air_from_t %.9g aa_cap %.9g tech_m %.9g\n", t.counter, t.air_from_t, t.aa_cap, t.tech_m);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "hand") == 0) return write_hand(argv[2]);
    if (argc == 3 && strcmp(argv[1], "load") == 0) return load(argv[2]);
    if (argc != 1) {
        printf("usage: impai_table_test [hand PATH | load PATH]\n");
        return 2;
    }
    test_classes();
    test_classes_from_the_classifier();
    test_picture();
    test_shares();
    test_type_choice();
    test_buy();
    test_hand_is_a2();
    test_file();
    printf("impai_table_test: %d of %d checks hold\n", checks - fails, checks);
    return fails ? 1 : 0;
}
