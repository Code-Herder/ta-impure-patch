/* impai_fight_test.c -- the fight predictor (src/impai_fight.h) on a fixed synthetic content, run by
   tests/impai_fight_test.py in three builds that must print one transcript.

   The content is drawn from a fixed LCG: unit types of every layer, mobile and static, flying and submerged;
   weapons of every kind with and without [DAMAGE] rows, anti-air and water ones, ballistic ones capped by the
   level's gravity. 200 fixed fights are printed bit for bit, each with its kills and shots, and every fight is
   held to the sanity rules:
     - a mirror fight's margin is 0 exactly, and swapping the sides negates the margin exactly;
     - a side doubled against itself never loses, and an army of armed ground units doubled wins. A random
       doubled side can draw: an area weapon whose overlap reaches every unit of a type takes the same share of
       each side, and a step whose fire kills every armed unit of both sides leaves both the same share;
     - sides out of each other's range that do not close end at step 0 with margin 0;
     - the units a side loses never exceed the shots aimed at it;
     - the steps never pass IMPAI_FSTEPS, the shares stay in [0, 1];
   and the refusals: a type past the content, a weapon index past it, a weapon kind past IMPAI_FKINDS, a damage
   span past the pool, a side of more than IMPAI_FSIDE_TYPES entries. On a small content of its own: the kind
   table on the engine's flag words of ARMSUB, ARMMH, ARMROCK, CORRL, ARMAMB, ARMSH and ARMTHUND's bomb
   (homing, straight, straight, homing, ballistic, straight, dropped); the first volley -- one shooter in reach
   at step 0 whose burst x damage is the target's hp kills it in step 1 at a hit probability of 1; and a
   ballistic weapon's reach capped at v^2 / (g 65536) and uncapped at gravity 0 and past 65536; and the chase
   walking to its first armed slot's reach, not its longest; an entry flagged IMPAI_FE_NO_COMMANDFIRE fighting as
   its type without its CommandFire slot. impai_expf is held to exp's value within 2^-22 and its bits printed;
   the merge of a 20-entry side is printed; the buy's callback impai_fight_rate_of answers impai_fight_rate.
   Exit 0 when every rule held, 1 otherwise. */
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
static IMPAI_FCONTENT C = { T, NT, W, NW, D, ND, 8155 };
static IMPAI_FWORK   s_work;
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
    static const uint32_t kinds[] = {
        TASIM_WF_BEAM, 0, TASIM_WF_BALLISTIC, TASIM_WF_SELFPROP | TASIM_WF_GUIDANCE, TASIM_WF_DROPPED,
        TASIM_WF_SELFPROP | TASIM_WF_TWOPHASE | TASIM_WF_VLAUNCH | TASIM_WF_GUIDANCE,
        TASIM_WF_SELFPROP | TASIM_WF_TWOPHASE | TASIM_WF_TRACKS, TASIM_WF_SELFPROP };
    unsigned i, k, nd = 0;
    memset(T, 0, sizeof T);
    memset(W, 0, sizeof W);
    memset(D, 0, sizeof D);
    for (i = 1; i < NW; i++) {
        IMPAI_FWEAPON* w = &W[i];
        w->flags = kinds[below(8)];
        if (below(6) == 0) w->flags |= TASIM_WF_TOAIR;
        if (below(7) == 0) w->flags |= TASIM_WF_WATER;
        w->kind = (uint8_t)impai_fight_kind(w->flags);
        w->range = (int32_t)(150 + below(1050));
        w->velocity = (w->flags & TASIM_WF_BEAM) || below(5) == 0 ? 0 : (int32_t)(100000 + below(700000));
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
    const int rc = impai_fight_run(&C, &impai_fitparams, a, b, dist, &s_work, r, &st);
    printf("fight %d rc=%d sa=%08X sb=%08X m=%08X steps=%u killed=%u,%u shots=%08X,%08X\n", i, rc, bits(r->sa),
           bits(r->sb), bits(r->m), r->steps, st.killed[0], st.killed[1], bits(st.shots[0]), bits(st.shots[1]));
    if (rc) return;
    if (r->steps > IMPAI_FSTEPS) fail("steps past the bound", i);
    if (!(r->sa >= 0.0f && r->sa <= 1.0f && r->sb >= 0.0f && r->sb <= 1.0f)) fail("a share outside [0, 1]", i);
    if ((float)st.killed[0] > st.shots[0] * 1.00001f + 0.001f || (float)st.killed[1] > st.shots[1] * 1.00001f + 0.001f)
        fail("more kills than shots", i);
    /* bit for bit, but for the sign of a zero margin: sa - sb with sa == sb is +0 either way round */
    if (impai_fight_predict(&C, &impai_fitparams, b, a, dist, &s_work, &back) ||
        (bits(back.m) != bits(-r->m) && !(back.m == 0.0f && r->m == 0.0f)))
        fail("swapping the sides does not negate the margin", i);
}

/* IMPAI_FT_SUBMERGED from a sea type's def (impai_fight_submerged), on the measured types: a type with a mover that
   is upright, or no floater, stands on the sea floor and is submerged whatever its waterline -- Total Mayhem's
   submarines of waterline 0 (CORSUB, top 15.65; corxsub, an upright floater), retail's whose top passes its
   waterline (CORSSUB, 42.50 over 25), the one that is neither (ARMACSUB) and one of that class above its
   waterline --; a floater that is not upright stands at the sea less its waterline and a structure there too, each
   submerged only when its top is below it. */
static void submerged_types(void)
{
    static const struct { const char* name; double top; unsigned wl; int mover, floater, upright, want; } K[] = {
        { "mayhem CORSUB", 15.65, 0, 1, 0, 1, 1 },     { "mayhem corxsub", 33.09, 0, 1, 1, 1, 1 },
        { "retail CORSSUB", 42.50, 25, 1, 0, 1, 1 },   { "retail ARMSUB", 14.07, 20, 1, 0, 1, 1 },
        { "retail ARMACSUB", 17.80, 20, 1, 0, 0, 1 },  { "retail CORPT", 13.00, 0, 1, 1, 0, 0 },
        { "tazero CoreT1Destroyer", 42.50, 1, 1, 1, 0, 0 },
        { "a floater below its waterline", 2.00, 3, 1, 1, 0, 1 },
        { "a mover neither floater nor upright, its top past its waterline", 30.00, 20, 1, 0, 0, 1 },
        { "a structure afloat", 20.00, 10, 0, 0, 0, 0 }, { "a structure under", 8.00, 10, 0, 0, 0, 1 },
        { "a structure with the upright bit", 20.00, 10, 0, 0, 1, 0 } };
    unsigned k;
    for (k = 0; k < sizeof K / sizeof K[0]; k++) {
        const int got = impai_fight_submerged((int32_t)(K[k].top * 65536.0), K[k].wl, K[k].mover, K[k].floater,
                                              K[k].upright);
        printf("submerged %s top=%.2f wl=%u mover=%d floater=%d upright=%d: %d\n", K[k].name, K[k].top, K[k].wl,
               K[k].mover, K[k].floater, K[k].upright, got);
        if (got != K[k].want) fail(K[k].name, (int)k);
    }
}

/* The kind table, the first volley and the reach cap, on a content of their own: a static shooter type 1
   with a straight weapon 1 (reach 500, 800 damage, reload 300), a static target type 2 of 800 hp, and a
   ballistic weapon 2 of range 2000 at 5 px a tick. */
static void small_content(void)
{
    static const struct { const char* name; uint32_t flags; int kind; } K[] = {
        { "ARMSUB", 0xB13001u, IMPAI_FK_HOMING },     { "ARMMH", 0x9141211u, IMPAI_FK_STRAIGHT },
        { "ARMROCK", 0x1C0201u, IMPAI_FK_STRAIGHT },  { "CORRL", 0x1C3201u, IMPAI_FK_HOMING },
        { "ARMAMB", 0x80202u, IMPAI_FK_BALLISTIC },   { "ARMSH", 0x80809u, IMPAI_FK_STRAIGHT },
        { "ARMTHUND", 0x4000100u, IMPAI_FK_DROPPED } };
    static IMPAI_FTYPE t[3];
    static IMPAI_FWEAPON w[3];
    IMPAI_FCONTENT c = { t, 3, w, 3, D, 0, 8155 };
    IMPAI_FIT sure = impai_fitparams;
    IMPAI_FSIDE a, b;
    IMPAI_FRESULT r;
    IMPAI_FSTATS st;
    float rate;
    int32_t range;
    unsigned k;
    for (k = 0; k < sizeof K / sizeof K[0]; k++) {
        const int got = impai_fight_kind(K[k].flags);
        printf("kind %s %08X %d\n", K[k].name, (unsigned)K[k].flags, got);
        if (got != K[k].kind) fail(K[k].name, (int)k);
    }
    memset(t, 0, sizeof t);
    memset(w, 0, sizeof w);
    t[1].hp = 500; t[1].value = 100.0f; t[1].fx = t[1].fz = 2; t[1].w[0] = 1;
    t[2].hp = 800; t[2].value = 100.0f; t[2].fx = t[2].fz = 2; t[2].w[0] = 2;
    w[1].range = 500; w[1].reload = 300; w[1].burst = 1; w[1].dmg = 800; w[1].kind = IMPAI_FK_STRAIGHT;
    w[2].range = 2000; w[2].velocity = 5 * 65536; w[2].reload = 60; w[2].burst = 1; w[2].dmg = 10;
    w[2].flags = TASIM_WF_BALLISTIC; w[2].kind = IMPAI_FK_BALLISTIC;
    for (k = 0; k < IMPAI_FKINDS; k++) sure.hit[k][IMPAI_FH_BIAS] = 30.0f;   /* sigma(30) is 1.0f */
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.ne = b.ne = 1;
    a.e[0].type = 1; a.e[0].n = 1; a.e[0].hp_frac = 65535;
    b.e[0].type = 2; b.e[0].n = 1; b.e[0].hp_frac = 65535;
    if (impai_fight_run(&c, &sure, &a, &b, 100, &s_work, &r, &st) || st.killed[1] != 1 || r.steps != 1 ||
        bits(r.sb) != 0u)
        fail("the first volley kills in step 1", -1);
    printf("first volley: steps=%u killed=%u,%u sa=%08X sb=%08X\n", r.steps, st.killed[0], st.killed[1],
           bits(r.sa), bits(r.sb));
    /* the reach: 5^2 x 65536 / 8155 = 200 px, the range 2000 with no gravity or one past the bound */
    if (impai_fight_rate(&c, &sure, 2, 1, &rate, &range) || range != 200) fail("the ballistic reach at gravity 8155", -1);
    c.gravity = 0;
    if (impai_fight_rate(&c, &sure, 2, 1, &rate, &range) || range != 2000) fail("the ballistic reach at gravity 0", -1);
    c.gravity = 65537;
    if (impai_fight_rate(&c, &sure, 2, 1, &rate, &range) || range != 2000) fail("the ballistic reach past the bound", -1);
    printf("reach: 200 at gravity 8155, 2000 at 0 and 65537\n");
}

/* THE CHASE WALKS TO ITS FIRST ARMED SLOT'S REACH (impai_fight.c, chase_reach): a mover with a 260-px weapon
   in slot 0 and a 700-px one in slot 1, 668 px from a target that has no weapon and cannot die, walks in, so
   its short weapon comes into reach and it fires more than the same type standing still; with the two
   weapons the other way round it stands where it is, 700 less the stand-off, and fires as the standing one
   does. */
static void chase_content(void)
{
    static IMPAI_FTYPE t[4];
    static IMPAI_FWEAPON w[3];
    const IMPAI_FCONTENT c = { t, 4, w, 3, D, 0, 8155 };
    IMPAI_FIT sure = impai_fitparams;
    IMPAI_FSIDE a, b;
    IMPAI_FRESULT r;
    IMPAI_FSTATS walks, stands;
    unsigned k, order;
    memset(w, 0, sizeof w);
    w[1].range = 260; w[1].reload = 30; w[1].burst = 1; w[1].dmg = 1; w[1].kind = IMPAI_FK_STRAIGHT;
    w[2].range = 700; w[2].reload = 30; w[2].burst = 1; w[2].dmg = 1; w[2].kind = IMPAI_FK_STRAIGHT;
    for (k = 0; k < IMPAI_FKINDS; k++) sure.hit[k][IMPAI_FH_BIAS] = 30.0f;
    for (order = 0; order < 2; order++) {
        memset(t, 0, sizeof t);
        t[1].hp = 500; t[1].value = 100.0f; t[1].fx = t[1].fz = 2; t[1].flags = IMPAI_FT_MOBILE;
        t[1].speed = 2 * 65536;
        t[1].w[0] = (uint16_t)(order ? 2 : 1);
        t[1].w[1] = (uint16_t)(order ? 1 : 2);
        t[2] = t[1];
        t[2].flags = 0;
        t[2].speed = 0;
        t[3].hp = 4000000000u; t[3].value = 100.0f; t[3].fx = t[3].fz = 2;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        a.ne = b.ne = 1;
        a.closes = 1;
        a.e[0].n = 1; a.e[0].hp_frac = 65535;
        b.e[0].type = 3; b.e[0].n = 1; b.e[0].hp_frac = 65535;
        a.e[0].type = 1;
        if (impai_fight_run(&c, &sure, &a, &b, 668, &s_work, &r, &walks)) fail("the chase: the mover's run", (int)order);
        a.e[0].type = 2;
        if (impai_fight_run(&c, &sure, &a, &b, 668, &s_work, &r, &stands)) fail("the chase: the standing run", (int)order);
        printf("chase: slot 0 reach %d: shots %08X walking, %08X standing\n", order ? 700 : 260,
               bits(walks.shots[1]), bits(stands.shots[1]));
        if (order ? walks.shots[1] != stands.shots[1] : !(walks.shots[1] > stands.shots[1]))
            fail(order ? "the chase walks past its first slot's reach" : "the chase stops short of its first slot's reach",
                 (int)order);
    }
}

/* THE CHASE TAKES SLOTS 0..2 ALONE (0x4897E0): a mover whose one weapon, 700 px, sits in slot `slot`, 668 px from
   a target that cannot die and whose 100-px weapon hurts it. In slot 2 the chase walks to that weapon's reach
   less the stand-off and stops out of the target's; in slot 3 the engine's chase takes slot 0, empty, and
   walks to contact, inside the target's reach, where the target's shots land. */
static void chase_slots(void)
{
    static IMPAI_FTYPE t[3];
    static IMPAI_FWEAPON w[3];
    const IMPAI_FCONTENT c = { t, 3, w, 3, D, 0, 8155 };
    IMPAI_FIT sure = impai_fitparams;
    IMPAI_FSIDE a, b;
    IMPAI_FRESULT r;
    IMPAI_FSTATS st;
    unsigned k, slot;
    memset(w, 0, sizeof w);
    w[1].range = 700; w[1].reload = 30; w[1].burst = 1; w[1].dmg = 1; w[1].kind = IMPAI_FK_STRAIGHT;
    w[2].range = 100; w[2].reload = 30; w[2].burst = 1; w[2].dmg = 1; w[2].kind = IMPAI_FK_STRAIGHT;
    for (k = 0; k < IMPAI_FKINDS; k++) sure.hit[k][IMPAI_FH_BIAS] = 30.0f;
    for (slot = 2; slot <= 3; slot++) {
        memset(t, 0, sizeof t);
        t[1].hp = 4000000000u; t[1].value = 100.0f; t[1].fx = t[1].fz = 2; t[1].flags = IMPAI_FT_MOBILE;
        t[1].speed = 2 * 65536;
        t[1].w[slot] = 1;
        t[2].hp = 4000000000u; t[2].value = 100.0f; t[2].fx = t[2].fz = 2; t[2].w[0] = 2;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        a.ne = b.ne = 1;
        a.closes = 1;
        a.e[0].type = 1; a.e[0].n = 1; a.e[0].hp_frac = 65535;
        b.e[0].type = 2; b.e[0].n = 1; b.e[0].hp_frac = 65535;
        if (impai_fight_run(&c, &sure, &a, &b, 668, &s_work, &r, &st)) fail("the chase slots: the run", (int)slot);
        printf("chase: the weapon in slot %u: the target's shots %08X\n", slot,bits(st.shots[0]));
        if (slot == 2 ? st.shots[0] != 0.0f : !(st.shots[0] > 0.0f))
            fail(slot == 2 ? "the chase walked past slot 2's reach" : "the chase took slot 3", (int)slot);
    }
}

/* AN ENTRY FLAGGED IMPAI_FE_NO_COMMANDFIRE FIRES NO COMMANDFIRE WEAPON: a commander-like type with a weak gun in
   slot 0 and a CommandFire shot in slot 1 that kills the target in one hit, against a target that hurts it. The
   flagged entry's run is bit for bit the run of the same type without its slot 1, and the unflagged entry kills
   the target where the flagged one does not. */
static void commandfire_flag(void)
{
    static IMPAI_FTYPE t[4];
    static IMPAI_FWEAPON w[3];
    const IMPAI_FCONTENT c = { t, 4, w, 3, D, 0, 8155 };
    IMPAI_FIT sure = impai_fitparams;
    IMPAI_FSIDE a, b;
    IMPAI_FRESULT r[3];
    IMPAI_FSTATS st[3];
    unsigned k;
    memset(t, 0, sizeof t);
    memset(w, 0, sizeof w);
    w[1].range = 300; w[1].reload = 30; w[1].burst = 1; w[1].dmg = 5; w[1].kind = IMPAI_FK_STRAIGHT;
    w[2].range = 300; w[2].reload = 30; w[2].burst = 1; w[2].dmg = 30000; w[2].kind = IMPAI_FK_STRAIGHT;
    w[2].flags = TASIM_WF_COMMANDFIRE;
    for (k = 0; k < IMPAI_FKINDS; k++) sure.hit[k][IMPAI_FH_BIAS] = 30.0f;
    t[1].hp = 3000; t[1].value = 100.0f; t[1].fx = t[1].fz = 2; t[1].w[0] = 1; t[1].w[1] = 2;
    t[1].flags = IMPAI_FT_COMMANDER;
    t[3] = t[1];
    t[3].w[1] = 0;
    t[2].hp = 2000; t[2].value = 100.0f; t[2].fx = t[2].fz = 2; t[2].w[0] = 1;
    memset(&b, 0, sizeof b);
    b.ne = 1;
    b.e[0].type = 2; b.e[0].n = 1; b.e[0].hp_frac = 65535;
    for (k = 0; k < 3; k++) {
        memset(&a, 0, sizeof a);
        a.ne = 1;
        a.e[0].type = (uint16_t)(k == 2 ? 3 : 1);
        a.e[0].n = 1;
        a.e[0].hp_frac = 65535;
        a.e[0].flags = (uint16_t)(k == 1 ? IMPAI_FE_NO_COMMANDFIRE : 0);
        if (impai_fight_run(&c, &sure, &a, &b, 100, &s_work, &r[k], &st[k]))
            fail("the CommandFire flag: the run", (int)k);
    }
    printf("commandfire: killed %u with the shot, %u flagged, %u without the slot; m %08X %08X %08X\n", st[0].killed[1],
           st[1].killed[1], st[2].killed[1], bits(r[0].m), bits(r[1].m), bits(r[2].m));
    if (st[0].killed[1] != 1 || st[1].killed[1] != 0 || bits(r[1].m) != bits(r[2].m) ||
        bits(r[1].sa) != bits(r[2].sa) || bits(r[1].sb) != bits(r[2].sb) || r[1].steps != r[2].steps)
        fail("a flagged entry fired its CommandFire weapon", -1);
}

/* AN ENTRY FLAGGED IMPAI_FE_NO_VALUE FIGHTS FOR NOTHING OF ITS OWN (impai_fight.c, THE MARGIN): a tough, weak
   type worth 100 x a tower's (1 000 000 HP, 5 a shot, value 10 000) beside a tower (2 000 HP, value 100) against two
   tanks (2 000 HP, 30 a shot, value 100) that kill the tower and outlive the run. Flagged, the side keeps the
   tower's share, 0, and the margin is -sb; unflagged, its kept share is about the tough type's health. The flagged
   type alone is a side with no value of its own: predicted, where an unflagged side of no value is refused; hurt,
   it keeps 0 and the margin is -sb; 5 000 px apart with neither side closing, untouched, it keeps 1 and the margin
   is 0 -- a fight in which neither side hurts the other.
   In a side past IMPAI_FSIDE_TYPES entries, no unit joins the flagged entry (THE MERGE). */
static void novalue_flag(void)
{
    static IMPAI_FTYPE t[4];
    static IMPAI_FWEAPON w[3];
    const IMPAI_FCONTENT c = { t, 4, w, 3, D, 0, 8155 };
    IMPAI_FIT sure = impai_fitparams;
    IMPAI_FSIDE a, b;
    IMPAI_FENTRY many[IMPAI_FSIDE_TYPES + 1];
    IMPAI_FRESULT r[3];
    unsigned k;
    memset(t, 0, sizeof t);
    memset(w, 0, sizeof w);
    w[1].range = 300; w[1].reload = 30; w[1].burst = 1; w[1].dmg = 5; w[1].kind = IMPAI_FK_STRAIGHT;
    w[2].range = 300; w[2].reload = 30; w[2].burst = 1; w[2].dmg = 30; w[2].kind = IMPAI_FK_STRAIGHT;
    for (k = 0; k < IMPAI_FKINDS; k++) sure.hit[k][IMPAI_FH_BIAS] = 30.0f;
    t[1].hp = 1000000; t[1].value = 10000.0f; t[1].fx = t[1].fz = 2; t[1].w[0] = 1;
    t[2].hp = 2000; t[2].value = 100.0f; t[2].fx = t[2].fz = 2; t[2].w[0] = 1;
    t[3].hp = 2000; t[3].value = 100.0f; t[3].fx = t[3].fz = 2; t[3].w[0] = 2;
    memset(&b, 0, sizeof b);
    b.ne = 1;
    b.e[0].type = 3; b.e[0].n = 2; b.e[0].hp_frac = 65535;
    for (k = 0; k < 2; k++) {
        memset(&a, 0, sizeof a);
        a.ne = 2;
        a.e[0].type = 1; a.e[0].n = 1; a.e[0].hp_frac = 65535;
        a.e[0].flags = (uint16_t)(k ? IMPAI_FE_NO_VALUE : 0);
        a.e[1].type = 2; a.e[1].n = 1; a.e[1].hp_frac = 65535;
        if (impai_fight_predict(&c, &sure, &a, &b, 100, &s_work, &r[k])) fail("the no-value flag: the run", (int)k);
    }
    memset(&a, 0, sizeof a);
    a.ne = 1;
    a.e[0].type = 1; a.e[0].n = 1; a.e[0].hp_frac = 65535; a.e[0].flags = IMPAI_FE_NO_VALUE;
    if (impai_fight_predict(&c, &sure, &a, &b, 100, &s_work, &r[2])) fail("the no-value flag: a side of it alone", -1);
    {
        IMPAI_FRESULT apart;
        if (impai_fight_predict(&c, &sure, &a, &b, 5000, &s_work, &apart)) fail("the no-value flag: apart", -1);
        printf("novalue apart: sa %.3f sb %.3f m %.3f\n", (double)apart.sa, (double)apart.sb, (double)apart.m);
        if (apart.sa != 1.0f || apart.sb != 1.0f || apart.m != 0.0f) fail("an untouched no-value side lost", -1);
    }
    printf("novalue: unflagged sa %.3f sb %.3f m %.3f; flagged sa %.3f sb %.3f m %.3f; alone sa %.3f m %.3f\n",
           (double)r[0].sa, (double)r[0].sb, (double)r[0].m, (double)r[1].sa, (double)r[1].sb, (double)r[1].m,
           (double)r[2].sa, (double)r[2].m);
    if (!(r[0].sa > 0.9f) || r[1].sa != 0.0f || !(r[1].sb > 0.0f) || r[1].m != -r[1].sb || r[2].sa != 0.0f ||
        r[2].m != -r[2].sb)
        fail("a flagged entry's value counted in the margin", -1);
    t[2].value = 0.0f;
    memset(&a, 0, sizeof a);
    a.ne = 1;
    a.e[0].type = 2; a.e[0].n = 1; a.e[0].hp_frac = 65535;
    if (impai_fight_predict(&c, &sure, &a, &b, 100, &s_work, &r[2]) != -1) fail("a side of no value predicted", -1);
    t[2].value = 100.0f;
    memset(many, 0, sizeof many);
    for (k = 0; k <= IMPAI_FSIDE_TYPES; k++) {
        many[k].type = (uint16_t)(k == 0 ? 1 : k == IMPAI_FSIDE_TYPES ? 1 : 2);
        many[k].n = (uint16_t)(k == IMPAI_FSIDE_TYPES ? 50 : 1);
        many[k].hp_frac = 65535;
        many[k].flags = (uint16_t)(k == 0 ? IMPAI_FE_NO_VALUE : 0);
    }
    many[IMPAI_FSIDE_TYPES].hp_frac = 1;
    if (impai_fight_side(&c, many, IMPAI_FSIDE_TYPES + 1, 1, &a)) fail("the no-value merge", -1);
    for (k = 0; k < a.ne; k++)
        if ((a.e[k].flags & IMPAI_FE_NO_VALUE) && a.e[k].n != 1) fail("a unit joined a flagged entry", (int)a.e[k].n);
}

/* A NaN STOPS NOTHING AND KILLS NONE: every hit bias, the focus and the splash NaN, and a weapon's edge NaN. The
   hit probabilities, the damage and so the pools are NaN; each step's kills are then 0, never a NaN converted
   to an integer (the asan build traps one), and the run ends at its bound with every unit standing. */
static void nan_fit(void)
{
    static IMPAI_FTYPE t[2];
    static IMPAI_FWEAPON w[2];
    const IMPAI_FCONTENT c = { t, 2, w, 2, D, 0, 8155 };
    IMPAI_FIT bad = impai_fitparams;
    IMPAI_FSIDE a;
    IMPAI_FRESULT r;
    IMPAI_FSTATS st;
    unsigned k;
    memset(t, 0, sizeof t);
    memset(w, 0, sizeof w);
    w[1].range = 500; w[1].reload = 30; w[1].burst = 1; w[1].dmg = 400; w[1].aoe = 64; w[1].edge = NAN;
    w[1].kind = IMPAI_FK_STRAIGHT;
    t[1].hp = 500; t[1].value = 100.0f; t[1].fx = t[1].fz = 2; t[1].w[0] = 1;
    for (k = 0; k < IMPAI_FKINDS; k++) bad.hit[k][IMPAI_FH_BIAS] = NAN;
    bad.focus = NAN;
    bad.splash = NAN;
    memset(&a, 0, sizeof a);
    a.ne = 1;
    a.e[0].type = 1; a.e[0].n = 3; a.e[0].hp_frac = 65535;
    if (impai_fight_run(&c, &bad, &a, &a, 100, &s_work, &r, &st)) fail("the NaN fit: the run", -1);
    printf("NaN fit: steps=%u killed=%u,%u\n", r.steps, st.killed[0], st.killed[1]);
    if (st.killed[0] || st.killed[1] || r.steps > IMPAI_FSTEPS) fail("the NaN fit killed", -1);
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
            if (impai_fight_predict(&C, &impai_fitparams, &a, &a, dist, &s_work, &r) || bits(r.m) != 0u ||
                bits(r.sa) != bits(r.sb))
                fail("a mirror fight's margin is not 0", i);
        }
        if (i % 5 == 1) {                  /* doubled against itself, with and without splash */
            IMPAI_FIT no_splash = impai_fitparams;
            two = a;
            for (e = 0; e < two.ne; e++) two.e[e].n = (uint16_t)(2 * two.e[e].n);
            two.closes = a.closes = 1;
            no_splash.splash = 0.0f;
            if (impai_fight_predict(&C, &impai_fitparams, &two, &a, dist, &s_work, &r) || r.m < 0.0f ||
                impai_fight_predict(&C, &no_splash, &two, &a, dist, &s_work, &r) || r.m < 0.0f)
                fail("a doubled side loses", i);
        }
        if (i % 5 == 2) {                  /* out of range, neither closing */
            a.closes = b.closes = 0;
            if (impai_fight_predict(&C, &impai_fitparams, &a, &b, 100000, &s_work, &r) || r.steps || bits(r.m) != 0u)
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
    if (!a.ne || impai_fight_predict(&C, &impai_fitparams, &two, &a, 600, &s_work, &r) || !(r.m > 0.0f)) fail("the doubled army", -1);
    printf("doubled army: types=%u m=%08X steps=%u\n", a.ne, bits(r.m), r.steps);
    {   /* the buy's predictor callback answers the pair rate, and 0 for a refused pair */
        const IMPAI_FCTX ctx = { &C, &impai_fitparams };
        if (impai_fight_rate_of(&ctx, NT, 1) != 0.0f) fail("the callback took a type past the content", -1);
        for (e = 1; e < NT; e++)
            if (!impai_fight_rate(&C, &impai_fitparams, e, 1, &rate, &range) &&
                bits(impai_fight_rate_of(&ctx, (uint16_t)e, 1)) != bits(rate))
                fail("the callback's pair rate", (int)e);
    }
    /* the refusals */
    b = a;
    b.e[0].type = NT;
    if (impai_fight_predict(&C, &impai_fitparams, &a, &b, 600, &s_work, &r) != -1) fail("a type past the content", -1);
    b = a;
    b.ne = IMPAI_FSIDE_TYPES + 1;
    if (impai_fight_predict(&C, &impai_fitparams, &a, &b, 600, &s_work, &r) != -1) fail("a side past its entries", -1);
    {
        const uint16_t keep = T[a.e[0].type].w[0];
        const unsigned kept_d0 = W[1].d0;
        T[a.e[0].type].w[0] = NW;
        if (impai_fight_predict(&C, &impai_fitparams, &a, &a, 600, &s_work, &r) != -1) fail("a weapon past the content", -1);
        T[a.e[0].type].w[0] = keep;
        for (e = 0; e < IMPAI_FW_SLOTS && !T[a.e[0].type].w[e]; e++) {}
        if (e < IMPAI_FW_SLOTS) {          /* a weapon kind past the enum */
            const unsigned wi = T[a.e[0].type].w[e];
            const uint8_t kind = W[wi].kind;
            W[wi].kind = IMPAI_FKINDS;
            if (impai_fight_predict(&C, &impai_fitparams, &a, &a, 600, &s_work, &r) != -1)
                fail("a weapon kind past the content", -1);
            W[wi].kind = kind;
        }
        W[1].d0 = ND;
        W[1].nd = W[1].nd ? W[1].nd : 1;
        for (e = 1; e < NT; e++)
            for (i = 0; i < IMPAI_FW_SLOTS; i++)
                if (T[e].w[i] == 1) b.e[0].type = (uint16_t)e;
        b.ne = 1;
        b.e[0].n = 1;
        if (b.e[0].type < NT && T[b.e[0].type].hp &&
            impai_fight_predict(&C, &impai_fitparams, &b, &a, 600, &s_work, &r) != -1)
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
    memset(many, 0, sizeof many);
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
    submerged_types();
    small_content();
    chase_content();
    chase_slots();
    nan_fit();
    commandfire_flag();
    novalue_flag();
    printf("impai_fight_test: %s\n", s_fail ? "FAIL" : "ok");
    return s_fail ? 1 : 0;
}
