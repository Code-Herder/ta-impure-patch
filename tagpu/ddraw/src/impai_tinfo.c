/* impai_tinfo.c -- what ImpAI's brain knows of each unit type (impai.h, IMPAI_TINFO): its category, its gains
   per economy pass on this map, its value. Engine-free: a function of the WORLDQ's def, def_econ and map and of
   the role classifier's answer, so the DLL and tasim build the same table. Single-precision floats, each
   operation rounded once on SSE2 (the Makefile's rule for impai_*.c). The build tech and the army types' values
   (THE VALUES, from the fight predictor). And the lookups of the build menus' table (impai.h, IMPAI_BMENU): the
   seat's menu and the classifier's build options, its union over the sides. */
#include <string.h>
#include "impai.h"
#include "tagpu_simreg.h"

/* The type information. RUNS: content the level init builds beside the threat table on every level a seat
   can play ImpAI on, live or puppet, from the same defs, so a puppet's is the live game's. */
SIM_FEATURE(impai_tinfo, SIM_PUPPET_RUNS, "impai-observe impai-observe-perm stock-impai-observe impai-eco");

/* A building with no weapon: the classifier makes a standing type with a regular weapon DEFENCE
   (tasim_roles.c, role_of), and counts damage only for regular weapons. */
static int unarmed(const TASIM_ROLE_OUT* r)
{
    return !r || (r->role != TASIM_ROLE_DEFENCE && r->dps_ground == 0.0 && r->dps_air == 0.0);
}

/* THE CATEGORY, the first that holds: a commander (the def's commander tag); an extractor or a geothermal
   plant (WQ_DEF.kind, the snap's own rule); a factory (a standing builder); a constructor (a mobile builder
   that works); a metal maker (a building with makesmetal that uses energy); an energy building (a building
   with no weapon and no makesmetal whose gain is above 0); anything else. THE GAIN is net of the type's own
   upkeep, energymake less energyuse (a negative energyuse, a solar collector's, is a gain), plus its wind and
   tide: a sensor's make only offsets its use -- retail's ARMRAD makes 5 and uses 5, ARMSONAR 9 and 9 (MEASURED
   2026-11-15, `native.py defs`, def +0x1C2 and +0x1C6) --, so it is no energy building, and the sensors' rule
   (impai_econ.c, THE SENSORS) finds it in category OTHER. */
static int category(const WQ_DEF* d, const WQ_DEF_ECON* e, const TASIM_ROLE_OUT* r, float egain)
{
    const uint32_t rf = r ? r->flags : 0;
    if (rf & TASIM_RF_COMMANDER) return IMPAI_C_COMMANDER;
    if (d->kind == WQ_KIND_MEX) return IMPAI_C_MEX;
    if (d->kind == WQ_KIND_GEO) return IMPAI_C_GEO;
    if (rf & TASIM_RF_FACTORY) return IMPAI_C_FACTORY;
    if (r && r->role == TASIM_ROLE_BUILDER && (rf & TASIM_RF_MOBILE) && d->workertime > 0) return IMPAI_C_CONS;
    if (e->building && e->makesmetal > 0 && e->energyuse > 0) return IMPAI_C_MAKER;
    if (e->building && unarmed(r) && e->makesmetal == 0 && egain > 0) return IMPAI_C_ENERGY;
    return IMPAI_C_OTHER;
}

/* PLACED ON WATER ONLY (IMPAI_TF_WATER) is the site test's answer, and the site test reads it from more than one
   key (engine/orders.md, the 0x47D2E0 row, *The height tail*, DISASSEMBLED):
   - minwaterdepth above 0: the footprint's highest cell must lie that far under the sea;
   - a tidal generator, which earns nothing off the water;
   - A YARD OF WATER CELLS AND NO LAND CELL, whatever its depth keys say: with no land cell the test takes its
     water path, whose altitude is the sea less the type's waterline, and refuses a footprint whose water cells
     reach above it. Escalation's floating wind generator is this type: minwaterdepth 0, a yard of `w`.
   yard_afloat answers the third for a building: a mobile type is placed by its factory, never by the site test,
   and keeps the keys' answer. INVARIANT, a bound on what the flag claims: a type flagged here is refused by the
   site test wherever one of its water cells lies above sea - waterline, and it has a water cell, so on ground
   that far above the sea it has no site at all. The waterline is not read: [INFERRED: no def's is negative] one
   below 0 would let such a type stand on a shore that little above the sea, and the flag then costs the seat
   that type off a wet anchor -- the next type of the menu is taken -- and never orders a site the test refuses.
   A yard that cannot be read whole (no yard map, a footprint past WQ_FOOT_MAX) answers 0: the keys decide. */
static int yard_afloat(const WORLDQ* q, int type, const WQ_DEF* d)
{
    uint8_t yard[WQ_FOOT_MAX * WQ_FOOT_MAX];
    int k, n, water = 0;
    if (d->fx < 1 || d->fz < 1 || d->fx > WQ_FOOT_MAX || d->fz > WQ_FOOT_MAX) return 0;
    n = d->fx * d->fz;
    if (q->ops->yard(q, type, yard, (int)sizeof yard) != n) return 0;
    for (k = 0; k < n; k++) {
        if (yard[k] & WQ_YARD_LAND) return 0;
        water |= yard[k] & WQ_YARD_WATER;
    }
    return water != 0;
}

int impai_tinfo_build(const WORLDQ* q, const TASIM_ROLE_OUT* roles, int nroles, IMPAI_TINFO* ti, int nti)
{
    const int n = q->ops->ndefs(q);
    WQ_MAP m;
    int t;
    if (n < 1 || n > nti) return -1;
    memset(ti, 0, (size_t)n * sizeof *ti);
    for (t = 0; t < n; t++) ti[t].klass = IMPAI_K_NONE;      /* the caller classes the army: impai_class_of */
    q->ops->map(q, &m);
    for (t = 1; t < n; t++) {
        const TASIM_ROLE_OUT* r = roles && t < nroles ? &roles[t] : NULL;
        IMPAI_TINFO* o = &ti[t];
        WQ_DEF d;
        WQ_DEF_ECON e;
        float egain;
        if (!q->ops->def(q, t, &d) || !q->ops->def_econ(q, t, &e)) continue;
        egain = e.energymake - e.energyuse + e.wind * m.wind_mean + e.tidal * m.tide;
        o->cat = (uint8_t)category(&d, &e, r, egain);
        o->tech = (uint8_t)(r && r->tech > 0 ? (r->tech < 255 ? r->tech : 255) : 0);
        o->flags = (uint8_t)(((r ? (r->flags & TASIM_RF_MOBILE) != 0 : (d.flags & WQ_DF_MOVER) != 0) ? IMPAI_TF_MOBILE : 0) |
                             ((d.flags & WQ_DF_VTOL) ? IMPAI_TF_VTOL : 0) |
                             (e.minwater > 0 || e.tidal > 0 || (e.building && yard_afloat(q, t, &d))
                                  ? IMPAI_TF_WATER : 0) |
                             ((d.flags & WQ_DF_ONOFF) ? IMPAI_TF_ONOFF : 0) |
                             ((d.flags & WQ_DF_RECLAIMER) ? IMPAI_TF_RECLAIM : 0));
        o->mclass = d.mclass;
        o->egain = o->cat == IMPAI_C_MAKER ? e.energyuse : egain;
        o->mgain = o->cat == IMPAI_C_MEX ? e.extracts : e.metalmake + (float)e.makesmetal;
        o->value = e.cost_m + e.cost_e / (float)TASIM_ROLES_R;
        o->workertime = d.workertime;
        o->fx = d.fx;
        o->fz = d.fz;
        o->radar = (uint16_t)e.radar;
        o->sonar = (uint16_t)e.sonar;
    }
    return n;
}

/* THE TYPE'S ATTACK FACTS, from its role input and never from a second read of the def, so the DLL and tasim
   answer alike. INVARIANT: every def and weapon bit tested here is a bit both role fillers copy (tasim_roles.h,
   TASIM_ROLE_F241, TASIM_ROLE_F245, TASIM_ROLE_WF), asserted at compile time below: a bit outside the masks reads
   0 on both sides, so a fact tested on one would hold for no type.
   - IMPAI_TA_*: the def's tests the order resolver's attack case (0x43F0E0 mode 3) makes of the unit ordered,
     `canattack` first (+0x245 bit 4, 0x43F154 test byte [esi+0x245],0x10, esi the def; DISASSEMBLED), from its
     flags and its weapons 1 and 2, the role input's slots 0 and 1 (the role fillers leave an empty slot zeroed
     and end at the type's count). The resolver reads the unit's own slot weapons, +0x10 and +0x2C, which are its
     def's: their one writer is the slot init 0x49E070 at 0x49E0B0, called only by the creates (0x4860AC,
     0x4862C4; tagpu_weapons.c's my_StartWeapons in its place writes the same def weapons), and nothing re-types a
     live unit (DISASSEMBLED, engine/units.md). A saved game loaded under changed content breaks it: the slot comes
     from the new def, its bit 1 from the save (0x48766E).
   - THE D-GUN: its longest CommandFire weapon when the def takes the resolver's mode 4 (+0x245 bit 14, `candgun`,
     0x43F7E8 mov ecx,[esi+0x245]; shr ecx,0xE; test cl,1 .. 0x43F7F4; DISASSEMBLED), its range held to 1..65535
     px, its energy a shot (+0xC0) to 0..65535 and its radius, half its areaofeffect, which the military's guard
     keeps every friendly unit clear of (impai_mil.c, THE D-GUN'S GUARD); none otherwise, and none when the radius
     passes the 255 px dgun_r holds, so no guard is ever narrower than the shot (retail ARM_ and
     CORE_DISINTEGRATOR: areaofeffect 48, a 24 px radius; MEASURED 2026-11-13, `tasim_run defs`).
   - WHAT IT HITS (IMPAI_TF_HITDRY, HITWET, HITAIR; impai.h, THE TARGET CLASSES): of its regular weapons -- the
     classifier's, not CommandFire, stockpile or interceptor (tasim_roles.c, regular) --, one that is neither a
     waterweapon nor a toairweapon, one that is a waterweapon, one in an anti-air slot (tasim_roles_slot_antiair,
     the classifier's and the fight content's rule). The other flags are the def's and stay.
   BOUND: nweapons is held to the input's slots; an energy not finite reads as 0. */
_Static_assert(((TASIM_F245_CANATTACK | TASIM_F245_CANDGUN) & ~TASIM_ROLE_F245) == 0,
               "the type facts test only +0x245 bits the role fillers copy");
_Static_assert(((TASIM_F241_KAMIKAZE | TASIM_F241_CANHOVER | TASIM_F241_CANFLY) & ~TASIM_ROLE_F241) == 0,
               "the type facts test only +0x241 bits the role fillers copy");
_Static_assert(((TASIM_WF_TOAIR | TASIM_WF_WATER | TASIM_WF_DROPPED | TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE |
                 TASIM_WF_INTERCEPTOR) & ~TASIM_ROLE_WF) == 0,
               "the type facts test only +0x111 bits the role fillers copy");
void impai_tinfo_attack(const TASIM_ROLE_IN* in, IMPAI_TINFO* t)
{
    const unsigned nw = in->nweapons < TASIM_ROLE_WEAPONS ? in->nweapons : TASIM_ROLE_WEAPONS;
    const uint32_t w1 = nw > 0 ? in->w[0].flags : 0u, w2 = nw > 1 ? in->w[1].flags : 0u;
    unsigned k, r = 0, hits = 0;
    for (k = 0; k < nw; k++) {
        const uint32_t f = in->w[k].flags;
        if (f & (TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | TASIM_WF_INTERCEPTOR)) continue;
        if (!(f & (TASIM_WF_WATER | TASIM_WF_TOAIR))) hits |= IMPAI_TF_HITDRY;
        if (f & TASIM_WF_WATER) hits |= IMPAI_TF_HITWET;
        if (tasim_roles_slot_antiair(in, (int)k)) hits |= IMPAI_TF_HITAIR;
    }
    t->flags = (uint8_t)((t->flags & ~(IMPAI_TF_HITDRY | IMPAI_TF_HITWET | IMPAI_TF_HITAIR)) | hits);
    t->attack = (uint8_t)(((in->flags245 & TASIM_F245_CANATTACK) ? IMPAI_TA_CANATTACK : 0u) |
                          ((in->flags241 & TASIM_F241_KAMIKAZE) ? IMPAI_TA_KAMIKAZE : 0u) |
                          ((w1 & TASIM_WF_TOAIR) ? IMPAI_TA_TOAIR1 : 0u) |
                          ((w1 & TASIM_WF_WATER) ? IMPAI_TA_WATER1 : 0u) |
                          ((w2 & TASIM_WF_WATER) ? IMPAI_TA_WATER2 : 0u) |
                          ((in->flags241 & TASIM_F241_CANHOVER) ? IMPAI_TA_HOVER : 0u) |
                          ((in->flags241 & TASIM_F241_CANFLY) ? IMPAI_TA_FLY : 0u) |
                          ((w1 & TASIM_WF_DROPPED) ? IMPAI_TA_DROP1 : 0u));
    t->dgun_range = t->dgun_e = 0;
    t->dgun_r = 0;
    if (!(in->flags245 & TASIM_F245_CANDGUN)) return;
    for (k = 0; k < nw; k++) {
        const TASIM_ROLE_WEAPON* w = &in->w[k];
        if (!(w->flags & TASIM_WF_COMMANDFIRE) || w->range <= 0 || w->range <= (int32_t)t->dgun_range) continue;
        t->dgun_range = (uint16_t)(w->range < 65535 ? w->range : 65535);
        t->dgun_e = (uint16_t)(w->energy_per_shot >= 0.0f && w->energy_per_shot < 65535.0f
                                   ? (unsigned)w->energy_per_shot : w->energy_per_shot >= 65535.0f ? 65535u : 0u);
        r = w->aoe / 2u;
    }
    if (r > 255u) t->dgun_range = t->dgun_e = 0;
    else t->dgun_r = (uint8_t)r;
}

void impai_tinfo_count(const IMPAI_TINFO* ti, int n, unsigned count[IMPAI_CATS])
{
    int t;
    memset(count, 0, IMPAI_CATS * sizeof *count);
    for (t = 1; t < n; t++) count[ti[t].cat < IMPAI_CATS ? ti[t].cat : IMPAI_C_OTHER]++;
}

/* 1 when type v's build options hold an extractor (IMPAI_C_MEX); an option at or past m is skipped */
static int builds_mex(const TASIM_ROLE_IN* in, const IMPAI_TINFO* ti, int m, unsigned v)
{
    int k;
    for (k = 0; k < in[v].nbuild; k++)
        if (in[v].build[k] < (unsigned)m && ti[in[v].build[k]].cat == IMPAI_C_MEX) return 1;
    return 0;
}

/* THE EXTRACTOR STEP: an extractor extracting at least MEX_STEP times the best extractor a commander builds opens the
   next tech's economy, whatever the build graph says. MEASURED 2026-11-15 on the five installs' menus (the best
   extractor each plant's constructors can build, against the commanders'): every tech-1 plant within 1.14 times
   (TA Twilight's tech-1 constructors' ARMAMEX, 0.00125 against 0.0011), every tech-2 plant at 2.0 times or more (TA
   Zero's ARMT2MEX, 0.002614 against 0.001307; retail, Escalation, Total Mayhem and Twilight 2.8 to 3.4). */
#define MEX_STEP 1.5f

/* the best extraction (IMPAI_TINFO.mgain) of the extractors on any commander's menu; 0 for none. BOUND: n x the
   longest list. */
static float commander_mex(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* roles, const IMPAI_TINFO* ti, int m)
{
    float best = 0.0f;
    int t, k;
    for (t = 1; t < m; t++) {
        if (!(roles[t].flags & TASIM_RF_COMMANDER)) continue;
        for (k = 0; k < in[t].nbuild; k++) {
            const unsigned v = in[t].build[k];
            if (v < (unsigned)m && ti[v].cat == IMPAI_C_MEX && ti[v].mgain > best) best = ti[v].mgain;
        }
    }
    return best;
}

/* THE BUILD TECH. A builder's is the highest classifier tech among its build options (TASIM_ROLE_OUT.tech, 0
   ignored), and at least 2 when one of them is an extractor past THE EXTRACTOR STEP; a factory's the highest build tech of the constructors (IMPAI_C_CONS) its menu holds that can build an
   extractor -- the constructors a plant's candidacy already asks for (impai_econ.c, THE CHOICE) --; anything
   else's 0 (owner's decision, 2026-11-14). The extractor test keeps out a builder that only lays mines, decoys or
   jammers: TA Zero's GoKT2AirJammer, classed a tech-2 constructor, made its air plant a tech-up candidate whose
   real constructor builds nothing above tech 1 (MEASURED 2026-11-14 from the roles dump). WHY the constructors: a
   tech-up exists to reach what the seat's builders cannot -- the next tech's extractors, energy and plants --, and
   a plant's units' tech is a graph distance (tasim_roles.c, tech_levels: the least number of factories before them
   on a build chain from a commander), so a plant the tech-1 constructors build makes units of tech 2 whatever its
   constructor can build. The hover plant is that case: CORHP's units read tech 2, and its CORCH builds nothing
   above tech 1. MEASURED 2026-11-14 on the install's menus (the install loader, tasim_load) and the classifier's
   tech (tasim_run roles), every constructor counted: CORLAB, CORVP, CORAP, CORSY, CORHP, ARMLAB, ARMVP, ARMAP, ARMSY
   and ARMHP are 1; CORALAB, CORAVP, CORAAP, ARMALAB, ARMAVP and ARMAAP 2; CORGANT 0, having no constructor; the
   commanders, the tech-1 constructors and CORCH/ARMCH 1. [INFERRED, not re-measured] the same with the extractor
   test: every retail plant's constructors build an extractor. WHY THE STEP: the graph's distance counts factories
   on the way from a commander, and a content whose commander builds its tech-2 plant itself has none: TA Zero's
   ARMCOMMANDER builds ARMT2GF beside its tech-1 plants, so ARMT2GF, its ARMT2CONTANK and that one's ARMT2MEX all
   read tech 1, no plant of the content scored above 1, and the seat ordered ARMT2GF (1 100 metal, 2 640 energy) as
   a further tech-1 plant at 3 187 with three extractors and its energy store empty (MEASURED, the mods batch at
   d665a00a, bar-nota-lavamania-ca-1). The extractor is the economy a tech-up exists to reach, and the step is
   the content's own ratio against its commanders' extractors; it only raises, so a content whose graph already
   separates its techs keeps them. Factories are never IMPAI_C_CONS, so the second pass
   reads only the first pass's rows. BOUND: the first pass n x the longest list; the second, for each factory's
   option, one more scan of that constructor's list, n x the longest list squared; an option at or past n, or at
   or past min(n, nti) for a row the second pass reads, is skipped. */
void impai_tinfo_ctech(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* roles, int n, IMPAI_TINFO* ti, int nti)
{
    const int m = n < nti ? n : nti;
    const float step = MEX_STEP * commander_mex(in, roles, ti, m);
    int t, k;
    for (t = 1; t < m; t++) {
        int b = 0;
        for (k = 0; k < in[t].nbuild; k++) {
            const unsigned v = in[t].build[k];
            if (v < (unsigned)n && roles[v].tech > b) b = roles[v].tech;
            if (v < (unsigned)m && b < 2 && step > 0.0f && ti[v].cat == IMPAI_C_MEX && ti[v].mgain >= step) b = 2;
        }
        ti[t].ctech = (uint8_t)(b < 255 ? b : 255);
    }
    for (t = 1; t < m; t++) {
        unsigned c = 0;
        if (ti[t].cat != IMPAI_C_FACTORY) continue;
        for (k = 0; k < in[t].nbuild; k++) {
            const unsigned v = in[t].build[k];
            if (v < (unsigned)m && ti[v].cat == IMPAI_C_CONS && ti[v].ctech > c && builds_mex(in, ti, m, v))
                c = ti[v].ctech;
        }
        ti[t].ctech = (uint8_t)c;
    }
}

/* type t is one THE CONTENT'S PRICE OF METAL counts: its energy per metal into *r */
static int priced(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* roles, const IMPAI_TINFO* ti, int t, float* r)
{
    if (ti[t].klass == IMPAI_K_NONE || roles[t].tech != 1 || !(in[t].cost_metal > 0.0f) ||
        !(in[t].cost_energy >= 0.0f))
        return 0;
    *r = in[t].cost_energy / in[t].cost_metal;
    return *r < 1e30f;
}

/* WHY THE MEDIAN OF THE ARMY: the keep is the energy income that lets the metal income be spent, and what a content
   spends its metal on in the opening is its tech-1 army, each at its own price; a median is no hostage to one
   outlier's price (retail's tech-1 builders' sum is 1.72 for the commander's 2 500 metal alone). MEASURED
   2026-11-15 from the five installs' roles dumps (tasim_run roles), the median over the tech-1 mobile types of
   the army roles: retail 8.63, Total Mayhem 8.65, TA Twilight 9.71, Escalation 12.07, TA Zero 2.40 -- against the
   plan's one 8, which starved Escalation's energy-heavy opening and kept four times the energy TA Zero spends. */
float impai_tinfo_e_per_m(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* roles, const IMPAI_TINFO* ti, int n, int nti)
{
    const int m = n < nti ? n : nti;
    int t, u, c = 0;
    float r, s;
    for (t = 1; t < m; t++) c += priced(in, roles, ti, t, &r);
    for (t = 1; t < m; t++) {
        int below = 0, at = 0;
        if (!priced(in, roles, ti, t, &r)) continue;
        for (u = 1; u < m; u++)
            if (priced(in, roles, ti, u, &s)) {
                below += s < r;
                at += s <= r;
            }
        if (below <= (c - 1) / 2 && (c - 1) / 2 < at) return r;
    }
    return 0.0f;
}

/* ---- THE VALUES ----

   q_T(t), type t's value per metal against what an enemy of tech T could field, for every type of a gated class
   (IMPAI_GATED: the land fighting classes, hover and amphibious types among them, impai_class_of) with a value
   above 0, and T = 1 .. IMPAI_QTECH:
       F_T     the types of a gated class, mobile, of classifier tech 1..T, with a value above 0 in both the type
               information and the fight content, sorted by (the content's value, type); past IMPAI_QFIELD of them
               the sorted list's floor(k n / IMPAI_QFIELD)-th for k = 0 .. IMPAI_QFIELD - 1, a value stride
       m(t, e) impai_fight_predict's margin sa - sb of {t, n_t} against {e, n_e}, both closing at full health,
               n_x = min(65535, round(V / value_x)), V = 32 x max(value_t, value_e), the fronts the longer of the
               two pair reaches apart (impai_fight_rate's `range`, at least 1)
       q_T(t)  the mean of m(t, e) over e in F_T; 0 with F_T empty; a refused or non-finite run counts 0
   THE FIELD is every side's, with equal weights: it depends neither on a seat's table (a tournament table differs
   per seat) nor on its reach (the values are content, built once a level).
   THE PAIR is equal value, at least 32 units a side, meeting where the longer weapon first reaches. A mixed fight
   against the whole field (one run a type) moved with the fight's scale and gap: CORLAB's best unit from -0.31 to
   +0.44 across V 3 000..12 000 and gaps 300..1 000; the pair mean moved by at most 0.03 across V 2 000..12 000 at a
   gap past both ranges, and with a fixed gap below the longer range it still moved (300 against 600: CORHP -0.51
   against -0.29), which the pair's own reach removes (MEASURED 2026-11-14, this C through a native library over
   the retail fight content). The pair rates' Lanchester square law, which needs no run, agreed with the
   predictor's sign on 87 % of the 1 891 retail land pairs and was wrong exactly on the fast units (ARMAH against
   ARMSTUMP +0.95 by the rates, -0.32 by the predictor): the rates price a moving target's misses that a closed
   fight does not have.
   THE BOUND: q is in [-1, 1], a mean of margins each in [-1, 1], and is held there; t against itself gives 0 by
   symmetry (impai_fight.c: a mirror fight's margin is 0 exactly). Past a value ratio of 2 047 the cheaper side is
   held at 65 535 units and fights below equal value; retail's largest ratio among the scored types is 486,
   CORKROG against ARMPW (MEASURED).
   NO GRAVITY: the level's gravity at the level init is not established (tagpu_impai.c, THE FIGHT PREDICTOR'S
   CONTENT), so the caller passes the content with gravity 0, no ballistic cap. At retail's 8 155 the cap shortens
   7 weapons, ARMBATS', ARMCRUS', ARMGUARD's, CORBATS', CORCRUS' and CORPUN's, none of a gated type (MEASURED), so
   the values equal the decisions' predictor for every type they score.
   THE CALIBRATION GAP: CORAH, ARMAH, CORSNAP, ARMANAC and CORMH have no reference fight, the model has no per-type
   term, and a homing shot's hit coefficients are held at their prior (tools/impai/predfit.py), so the hovers'
   values are the model's mechanics extrapolated (research/notes/impai/calibration.md).
   THE COST: each field is chosen by rank, n^2 comparisons for its n candidates, and a field equal to the one below
   it is copied, not run again; at most (nti - 1) x IMPAI_QTECH x IMPAI_QFIELD runs. Retail (MEASURED 2026-11-18,
   the level init's line): 46 scored types, fields of 12, 32, 32 and 32 (T4 = T3, copied), 3 496 runs. The level init's line states the
   build's own count and time. Engine-free and deterministic: every float is rounded once on SSE2 (the Makefile's
   rule for impai_*.c), each sum taken in the field's order. */
static int value_gated(const IMPAI_TINFO* t)
{
    return t->klass < IMPAI_KS && (IMPAI_GATED >> t->klass & 1u) && t->value > 0.0f;
}

/* a field's order: the content's value, then the type */
static int value_before(const IMPAI_FCONTENT* c, unsigned a, unsigned b)
{
    return c->t[a].value < c->t[b].value || (c->t[a].value == c->t[b].value && a < b);
}

static int value_in_field(const IMPAI_FCONTENT* c, const IMPAI_TINFO* ti, unsigned t, unsigned T)
{
    return value_gated(&ti[t]) && (ti[t].flags & IMPAI_TF_MOBILE) && ti[t].tech >= 1 && ti[t].tech <= T &&
           c->t[t].value > 0.0f;
}

/* F_T of the types below nt into f (IMPAI_QFIELD of them): its size. Each candidate's rank is counted over the
   others, so it lands at its stride's place without a sort buffer. BOUND: n^2 comparisons, n the candidates */
static unsigned value_field(const IMPAI_FCONTENT* c, const IMPAI_TINFO* ti, unsigned nt, unsigned T, uint16_t* f)
{
    unsigned t, e, n = 0, k;
    for (t = 1; t < nt; t++) n += (unsigned)value_in_field(c, ti, t, T);
    for (t = 1; t < nt; t++) {
        unsigned r = 0;
        if (!value_in_field(c, ti, t, T)) continue;
        for (e = 1; e < nt; e++) r += (unsigned)(e != t && value_in_field(c, ti, e, T) && value_before(c, e, t));
        if (n <= IMPAI_QFIELD) {
            f[r] = (uint16_t)t;
            continue;
        }
        for (k = 0; k < IMPAI_QFIELD; k++)
            if ((unsigned)((uint64_t)k * n / IMPAI_QFIELD) == r) f[k] = (uint16_t)t;
    }
    return n < IMPAI_QFIELD ? n : IMPAI_QFIELD;
}

/* the units of value V of a type of value v: round(V / v), at most 65 535 */
static uint16_t value_count(float V, float v)
{
    const float x = V / v;
    return x < 65534.5f ? (uint16_t)(x + 0.5f) : (uint16_t)65535u;
}

/* m(t, e) into *m: 0, or -1 for a refused run or a margin outside [-1, 1], which a finite one never is */
static int value_pair(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, unsigned t, unsigned e, IMPAI_FWORK* w,
                      float* m)
{
    IMPAI_FENTRY ea, eb;
    IMPAI_FSIDE a, b;
    IMPAI_FRESULT r;
    float rate, V;
    int32_t gt, ge, gap;
    if (t >= c->nt || e >= c->nt || impai_fight_rate(c, p, t, e, &rate, &gt) ||
        impai_fight_rate(c, p, e, t, &rate, &ge))
        return -1;
    gap = gt > ge ? gt : ge;
    if (gap < 1) gap = 1;
    V = 32.0f * (c->t[t].value > c->t[e].value ? c->t[t].value : c->t[e].value);
    ea.type = (uint16_t)t;
    ea.n = value_count(V, c->t[t].value);
    ea.hp_frac = 65535;
    eb.type = (uint16_t)e;
    eb.n = value_count(V, c->t[e].value);
    eb.hp_frac = 65535;
    /* every weapon fires, CommandFire included: the values are content, the same for every seat, and holding a
       D-gun back is THE STAND's call for a commander at one moment (impai_mil.c), not a type's worth */
    ea.flags = eb.flags = 0;
    if (impai_fight_side(c, &ea, 1, 1, &a) || impai_fight_side(c, &eb, 1, 1, &b) ||
        impai_fight_predict(c, p, &a, &b, gap, w, &r) || !(r.m >= -1.0f && r.m <= 1.0f))
        return -1;
    *m = r.m;
    return 0;
}

int impai_tinfo_values(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_TINFO* ti, unsigned nti,
                       IMPAI_TVALUE* out, IMPAI_FWORK* w, IMPAI_TVALUE_STATS* st)
{
    uint16_t f[IMPAI_QTECH][IMPAI_QFIELD];
    unsigned nf[IMPAI_QTECH], same[IMPAI_QTECH], nt, T, t, k;
    if (!c || !p || !ti || !out || !w || !st) return -1;
    nt = nti < c->nt ? nti : c->nt;
    memset(st, 0, sizeof *st);
    memset(out, 0, (size_t)nti * sizeof *out);
    for (T = 0; T < IMPAI_QTECH; T++) {
        nf[T] = value_field(c, ti, nt, T + 1, f[T]);
        same[T] = T > 0 && nf[T] == nf[T - 1] && !memcmp(f[T], f[T - 1], nf[T] * sizeof f[T][0]);
        st->field[T] = (uint8_t)nf[T];
    }
    for (t = 1; t < nti; t++) {
        if (!value_gated(&ti[t])) continue;
        st->scored++;
        for (T = 0; T < IMPAI_QTECH; T++) {
            float sum = 0.0f, q;
            if (same[T]) {
                out[t].q[T] = out[t].q[T - 1];
                continue;
            }
            for (k = 0; k < nf[T]; k++) {
                float m;
                st->runs++;
                if (value_pair(c, p, t, f[T][k], w, &m)) st->refused++;
                else sum += m;
            }
            q = nf[T] ? sum / (float)nf[T] : 0.0f;
            out[t].q[T] = q < -1.0f ? -1.0f : q > 1.0f ? 1.0f : q;
        }
    }
    return 0;
}

int impai_menu_find(const IMPAI_BMENU* rows, unsigned nrows, unsigned npool, unsigned side, unsigned builder)
{
    const uint32_t key = (uint32_t)side << 16 | builder;
    unsigned lo = 0, hi = nrows;
    if (side > 0xFFu || builder > 0xFFFFu) return -1;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        const uint32_t k = (uint32_t)rows[mid].side << 16 | rows[mid].builder;
        if (k < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= nrows || rows[lo].side != side || rows[lo].builder != builder) return -1;
    if (rows[lo].at > npool || rows[lo].n > npool - rows[lo].at) return -1;
    return (int)lo;
}

/* A merge of the sides' rows, each ascending without repeats: every step takes the least head and moves past
   it in every row that holds it. BOUND: a row's cursor stays below its count, and out is written below cap. */
int impai_menu_union(const IMPAI_BMENU* rows, unsigned nrows, const uint16_t* pool, unsigned npool,
                     unsigned builder, uint16_t* out, int cap)
{
    const uint16_t* list[IMPAI_MENU_SIDES];
    unsigned len[IMPAI_MENU_SIDES], at[IMPAI_MENU_SIDES], s, nl = 0;
    int n = 0;
    for (s = 0; s < IMPAI_MENU_SIDES; s++) {
        const int r = impai_menu_find(rows, nrows, npool, s, builder);
        if (r < 0 || !rows[r].n) continue;
        list[nl] = pool + rows[r].at;
        len[nl] = rows[r].n;
        at[nl++] = 0;
    }
    for (;;) {
        unsigned least = 0x10000u;
        for (s = 0; s < nl; s++)
            if (at[s] < len[s] && list[s][at[s]] < least) least = list[s][at[s]];
        if (least > 0xFFFFu) return n;
        if (n == cap) return -1;
        out[n++] = (uint16_t)least;
        for (s = 0; s < nl; s++)
            if (at[s] < len[s] && list[s][at[s]] == least) at[s]++;
    }
}
