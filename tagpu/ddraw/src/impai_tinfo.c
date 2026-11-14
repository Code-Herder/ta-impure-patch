/* impai_tinfo.c -- what ImpAI's brain knows of each unit type (impai.h, IMPAI_TINFO): its category, its gains
   per economy pass on this map, its value. Engine-free: a function of the WORLDQ's def, def_econ and map and of
   the role classifier's answer, so the DLL and tasim build the same table. Single-precision floats, each
   operation rounded once on SSE2 (the Makefile's rule for impai_*.c). And the lookups of the build menus'
   table (impai.h, IMPAI_BMENU): the seat's menu and the classifier's build options, its union over the sides. */
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
   with no weapon and no makesmetal whose gain is above 0); anything else. */
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
        egain = e.energymake + (e.energyuse < 0 ? -e.energyuse : 0.0f) + e.wind * m.wind_mean + e.tidal * m.tide;
        o->cat = (uint8_t)category(&d, &e, r, egain);
        o->tech = (uint8_t)(r && r->tech > 0 ? (r->tech < 255 ? r->tech : 255) : 0);
        o->flags = (uint8_t)(((r ? (r->flags & TASIM_RF_MOBILE) != 0 : (d.flags & WQ_DF_MOVER) != 0) ? IMPAI_TF_MOBILE : 0) |
                             ((d.flags & WQ_DF_VTOL) ? IMPAI_TF_VTOL : 0) |
                             (e.minwater > 0 || e.tidal > 0 ? IMPAI_TF_WATER : 0));
        o->mclass = d.mclass;
        o->egain = o->cat == IMPAI_C_MAKER ? e.energyuse : egain;
        o->mgain = o->cat == IMPAI_C_MEX ? e.extracts : e.metalmake + (float)e.makesmetal;
        o->value = e.cost_m + e.cost_e / (float)TASIM_ROLES_R;
        o->workertime = d.workertime;
        o->fx = d.fx;
        o->fz = d.fz;
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
   BOUND: nweapons is held to the input's slots; an energy not finite reads as 0. */
_Static_assert(((TASIM_F245_CANATTACK | TASIM_F245_CANDGUN) & ~TASIM_ROLE_F245) == 0,
               "the type facts test only +0x245 bits the role fillers copy");
_Static_assert(((TASIM_F241_KAMIKAZE | TASIM_F241_CANHOVER | TASIM_F241_CANFLY) & ~TASIM_ROLE_F241) == 0,
               "the type facts test only +0x241 bits the role fillers copy");
_Static_assert(((TASIM_WF_TOAIR | TASIM_WF_WATER | TASIM_WF_DROPPED | TASIM_WF_COMMANDFIRE) & ~TASIM_ROLE_WF) == 0,
               "the type facts test only +0x111 bits the role fillers copy");
void impai_tinfo_attack(const TASIM_ROLE_IN* in, IMPAI_TINFO* t)
{
    const unsigned nw = in->nweapons < TASIM_ROLE_WEAPONS ? in->nweapons : TASIM_ROLE_WEAPONS;
    const uint32_t w1 = nw > 0 ? in->w[0].flags : 0u, w2 = nw > 1 ? in->w[1].flags : 0u;
    unsigned k, r = 0;
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

void impai_tinfo_offers(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* roles, int n, IMPAI_TINFO* ti, int nti)
{
    const int m = n < nti ? n : nti;
    int t, k;
    for (t = 1; t < m; t++) {
        int offer = 0;
        for (k = 0; k < in[t].nbuild; k++) {
            const unsigned v = in[t].build[k];
            if (v < (unsigned)n && (roles[v].flags & TASIM_RF_MOBILE) && roles[v].tech > offer) offer = roles[v].tech;
        }
        ti[t].offer = (uint8_t)(offer < 255 ? offer : 255);
    }
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
