/* rolestest -- tasim_roles on synthetic inputs: each precedence branch, the percentile's edges,
   the tech levels' walk and the metal maker's ratio. Built and run by `tools/tasim/native.py
   test` under ASan and UBSan; exit 0 when nothing failed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_roles.h"

static int s_fail;

#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static TASIM_ROLE_IN unit(const char* name, int mobile, double speed, float metal)
{
    TASIM_ROLE_IN u;
    memset(&u, 0, sizeof u);
    snprintf(u.name, sizeof u.name, "%s", name);
    if (mobile) { u.flags245 = TASIM_F245_CANMOVE; u.bmcode = 1; }
    u.maxvelocity_fx = (int32_t)(speed * 65536.0);
    u.cost_metal = metal;
    u.maxdamage = 100;
    return u;
}

static void gun(TASIM_ROLE_IN* u, int range, int damage, float reload, uint32_t flags)
{
    TASIM_ROLE_WEAPON* w = &u->w[u->nweapons++];
    w->range = range;
    w->damage_default = damage;
    w->damage_air = (float)damage;
    w->reload_ticks = reload;
    w->flags = flags;
}

static TASIM_ROLE_OUT* classify(TASIM_ROLE_IN* in, int n, TASIM_ROLE_Q* q, int* rc)
{
    TASIM_ROLE_OUT* out = calloc((size_t)n, sizeof *out);
    void* work = malloc(tasim_roles_work(n));
    *rc = tasim_roles_classify(in, n, out, q, work);
    free(work);
    return out;
}

static void test_quantile(void)
{
    double one[1] = { 5.0 }, ties[4] = { 2.0, 2.0, 2.0, 2.0 }, four[4] = { 4.0, 1.0, 3.0, 2.0 };
    CHECK(tasim_roles_quantile(one, 1, 0.75) == 5.0, "one value is every percentile");
    CHECK(tasim_roles_quantile(ties, 4, 0.10) == 2.0, "ties");
    /* numpy.quantile([1, 2, 3, 4], 0.75, method="linear") == 3.25 */
    CHECK(fabs(tasim_roles_quantile(four, 4, 0.75) - 3.25) < 1e-12, "linear interpolation");
    CHECK(tasim_roles_quantile(four, 0, 0.5) == 0.0, "no value");
}

/* the commander builds a lab and a solar; the lab a kbot and a constructor; the constructor an
   advanced lab, which builds the heavy kbot; one unit nothing builds */
static void test_branches_and_tech(void)
{
    enum { COM, LAB, SOLAR, PW, CK, ALAB, ZEUS, LLT, AA, ART, SCOUT, FAST, TRANS, KAMI, NUKE, LONE, N };
    TASIM_ROLE_IN in[N];
    TASIM_ROLE_Q q;
    TASIM_ROLE_OUT* out;
    static const uint16_t com_b[] = { LAB, SOLAR }, lab_b[] = { PW, CK, AA, ART, SCOUT, FAST, TRANS, KAMI, NUKE },
                          ck_b[] = { ALAB, LLT }, alab_b[] = { ZEUS };
    int rc;
    in[COM] = unit("COM", 1, 1.0, 2500); in[COM].flags241 = TASIM_F241_BUILDER;
    in[COM].flags245 |= TASIM_F245_COMMANDER; in[COM].build = com_b; in[COM].nbuild = 2;
    in[LAB] = unit("LAB", 0, 0, 500); in[LAB].flags241 = TASIM_F241_BUILDER; in[LAB].build = lab_b; in[LAB].nbuild = 9;
    in[SOLAR] = unit("SOLAR", 0, 0, 100);
    in[PW] = unit("PW", 1, 1.5, 45); gun(&in[PW], 180, 10, 9, 0);
    in[CK] = unit("CK", 1, 1.0, 120); in[CK].flags241 = TASIM_F241_BUILDER; in[CK].build = ck_b; in[CK].nbuild = 2;
    in[ALAB] = unit("ALAB", 0, 0, 2000); in[ALAB].flags241 = TASIM_F241_BUILDER; in[ALAB].build = alab_b; in[ALAB].nbuild = 1;
    in[ZEUS] = unit("ZEUS", 1, 0.6, 300); gun(&in[ZEUS], 180, 60, 30, 0);
    in[LLT] = unit("LLT", 0, 0, 90); gun(&in[LLT], 400, 75, 30, 0);
    in[AA] = unit("AA", 1, 1.2, 100); gun(&in[AA], 600, 20, 30, 0); in[AA].slot_notair[0] = 1;
    gun(&in[AA], 200, 5, 30, 0);
    in[ART] = unit("ART", 1, 0.8, 200); gun(&in[ART], 900, 100, 60, TASIM_WF_BALLISTIC);
    in[SCOUT] = unit("SCOUT", 1, 3.0, 30); gun(&in[SCOUT], 100, 1, 30, 0);
    in[FAST] = unit("FAST", 1, 2.0, 400); gun(&in[FAST], 150, 10, 30, 0);
    in[TRANS] = unit("TRANS", 1, 1.0, 100); in[TRANS].transportcapacity = 1;
    in[KAMI] = unit("KAMI", 1, 1.0, 100); in[KAMI].flags241 = TASIM_F241_KAMIKAZE; gun(&in[KAMI], 10, 900, 30, 0);
    in[NUKE] = unit("NUKE", 1, 1.0, 100); gun(&in[NUKE], 2000, 0, 30, TASIM_WF_INTERCEPTOR | TASIM_WF_STOCKPILE);
    in[LONE] = unit("LONE", 1, 0.5, 100);
    out = classify(in, N, &q, &rc);
    CHECK(rc == 0, "classify returned %d", rc);
    CHECK(out[COM].role == TASIM_ROLE_BUILDER && (out[COM].flags & TASIM_RF_COMMANDER), "commander");
    CHECK(out[LAB].role == TASIM_ROLE_BUILDER && (out[LAB].flags & TASIM_RF_FACTORY), "factory");
    CHECK(out[CK].role == TASIM_ROLE_BUILDER && !(out[CK].flags & TASIM_RF_FACTORY), "a mobile builder is no factory");
    CHECK(out[LLT].role == TASIM_ROLE_DEFENCE, "defence");
    CHECK(out[SOLAR].role == TASIM_ROLE_OTHER, "a static without a weapon");
    CHECK(out[TRANS].role == TASIM_ROLE_OTHER && (out[TRANS].flags & TASIM_RF_TRANSPORT), "transport");
    CHECK(out[KAMI].role == TASIM_ROLE_OTHER, "kamikaze");
    CHECK(out[NUKE].role == TASIM_ROLE_OTHER, "only stockpile and interceptor weapons");
    CHECK(out[AA].role == TASIM_ROLE_ANTIAIR, "a NOTAIR primary is anti-air, its second gun aside (%s)",
          tasim_roles_name(out[AA].role));
    CHECK(out[AA].dps_air > 0 && out[AA].range_ground == 200, "the anti-air slot is out of the ground figures");
    CHECK(out[LONE].role == TASIM_ROLE_SCOUT, "a mobile with no weapon");
    CHECK(out[ART].role == TASIM_ROLE_ARTILLERY, "artillery (%s, range %d against P75 %g)",
          tasim_roles_name(out[ART].role), out[ART].range_ground, q.range75[TASIM_LAYER_LAND]);
    CHECK(out[SCOUT].role == TASIM_ROLE_SCOUT, "fast and cheap (%s)", tasim_roles_name(out[SCOUT].role));
    CHECK(out[FAST].role == TASIM_ROLE_RAIDER, "fast and dear (%s)", tasim_roles_name(out[FAST].role));
    CHECK(out[ZEUS].role == TASIM_ROLE_ASSAULT, "slow (%s)", tasim_roles_name(out[ZEUS].role));
    CHECK(out[COM].tech == 1 && out[LAB].tech == 1 && out[PW].tech == 1, "tech 1: %d %d %d",
          out[COM].tech, out[LAB].tech, out[PW].tech);
    CHECK(out[CK].tech == 1 && out[ALAB].tech == 1, "a lab's constructor and its lab: %d %d",
          out[CK].tech, out[ALAB].tech);
    CHECK(out[ZEUS].tech == 2, "two factories before the heavy kbot: %d", out[ZEUS].tech);
    CHECK(out[LLT].tech == 1, "the constructor's tower: %d", out[LLT].tech);
    CHECK(out[LONE].tech == 0, "no chain reaches it: %d", out[LONE].tech);
    CHECK(fabs(out[PW].value - (45.0 + 0.0 / TASIM_ROLES_R)) < 1e-9, "value");
    free(out);
}

/* a shorter chain through fewer factories wins over a longer one */
static void test_tech_least_factories(void)
{
    enum { COM, F1, F2, F3, X, N };
    TASIM_ROLE_IN in[N];
    TASIM_ROLE_Q q;
    TASIM_ROLE_OUT* out;
    static const uint16_t com_b[] = { F1, X }, f1_b[] = { F2 }, f2_b[] = { F3 }, f3_b[] = { X };
    int rc, i;
    for (i = 0; i < N; i++) in[i] = unit("U", 0, 0, 10);
    in[COM] = unit("COM", 1, 1, 10); in[COM].flags241 = TASIM_F241_BUILDER;
    in[COM].flags245 |= TASIM_F245_COMMANDER; in[COM].build = com_b; in[COM].nbuild = 2;
    in[F1].flags241 = in[F2].flags241 = in[F3].flags241 = TASIM_F241_BUILDER;
    in[F1].build = f1_b; in[F1].nbuild = 1; in[F2].build = f2_b; in[F2].nbuild = 1;
    in[F3].build = f3_b; in[F3].nbuild = 1;
    out = classify(in, N, &q, &rc);
    CHECK(rc == 0, "rc %d", rc);
    CHECK(out[F3].tech == 2 && out[X].tech == 1, "F3 %d X %d", out[F3].tech, out[X].tech);
    free(out);
    in[COM].flags245 &= ~TASIM_F245_COMMANDER;
    out = classify(in, N, &q, &rc);
    CHECK(rc == -1, "no commander: %d", rc);
    free(out);
}

static void test_maker_ratio(void)
{
    TASIM_ROLE_IN in[3];
    in[0] = unit("CHEAP", 0, 0, 1); in[0].energyuse = 60; in[0].makesmetal = 1;
    in[1] = unit("DEAR", 0, 0, 500); in[1].energyuse = 600; in[1].makesmetal = 12;
    in[2] = unit("TIE", 0, 0, 1); in[2].energyuse = 50; in[2].makesmetal = 1;
    CHECK(tasim_roles_maker_ratio(in, 2) == 60.0, "the cheapest maker by metal");
    CHECK(tasim_roles_maker_ratio(in, 3) == 50.0, "a tie on cost takes the lower ratio");
    CHECK(tasim_roles_maker_ratio(in + 1, 0) == 0.0, "no maker");
}

int main(void)
{
    test_quantile();
    test_branches_and_tech();
    test_tech_least_factories();
    test_maker_ratio();
    printf("rolestest: %s (%d failed)\n", s_fail ? "FAIL" : "ok", s_fail);
    return s_fail ? 1 : 0;
}
