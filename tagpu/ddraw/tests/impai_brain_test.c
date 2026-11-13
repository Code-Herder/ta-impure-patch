/* impai_brain_test.c -- ImpAI's brain over a fake WORLDQ (worldq.h) made of plain arrays: a 128 x 128-cell map,
   a handful of unit types with their economy keys and build menus, one seat's units and orders, its economy.
   Engine-free, so the same source builds natively, under AddressSanitizer and UndefinedBehaviorSanitizer, and with
   the DLL's compiler under Wine (tests/impai_brain_test.py); each build prints the same transcript, which the
   driver compares.

   THE FAKE WORLD answers each op as worldq.h states it, reduced to what the brain reads: a unit's footprint
   blocks a site, a site's centre must be explored, a command is checked as the engine's adapter checks it and
   then applied to the fake's lists, so a later decision sees the orders an earlier one gave. Every command it
   accepts is logged. Its radar pass finds nothing, but when `radar_slot` names a slot: then the pass finds
   that slot's unit at its first call and completes at its second, so the knowledge tick's publish can be held
   to a slot that changed hands, or a unit that moved, while the pass was open. Its ground is 10 high with no
   water; with water, rectangles of a depth under the sea and land 10 above it (cell_h), and the movement
   classes and the site test answer by those heights. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "worldq.h"
#include "impai.h"
#include "impai_brain.h"
#include "impai_mil.h"

#define FW_W     128                    /* cells of 16 px                                                      */
#define FW_H     128
#define FW_SLOTS 2560                  /* past the military's army and enemy lists, so a case can fill both  */
#define FW_DEFS  16
#define FW_MENU  16
#define FW_LIST  16
#define FW_LOG   256
#define FW_WATER 48                     /* the water rectangles a world holds                                 */

/* the fake's action bytes, in WQ_ACT_* order, then the resolver's answers */
enum { A_MOBILEBUILD = 1, A_VTOL_MOBILEBUILD, A_RECLAIM, A_VTOL_RECLAIM, A_RESURRECT, A_BUILDINGBUILD,
       A_MOVE, A_GUARD, A_HELPBUILD, A_STOP, A_ATTACK, A_PATROL, A_DGUN };

typedef struct FDEF {
    WQ_DEF      d;
    WQ_DEF_ECON e;
    uint16_t    menu[FW_MENU];
    int         nmenu;
    uint32_t    hp;                     /* maxdamage                                                          */
    int         armed;                  /* its units carry +0x110 bit 31, a weapon                            */
    uint8_t     attack;                 /* its IMPAI_TA_* facts: the resolver's attack case reads them        */
    int         dgun;                   /* it answers the D-gun mode (candgun)                                */
} FDEF;

typedef struct FUNIT {
    int      live;
    uint8_t  owner, finished, seen;     /* seen: the seat sees this unit of another owner by sight now   */
    uint8_t  aim;                       /* WQ_AIM_*: how the resolver finds it as a target                */
    uint16_t type;
    int32_t  pos[3];
    WQ_ORDER list[FW_LIST];
    int      n;
    int      hp;
} FUNIT;

typedef struct FCMD { WQ_CMD c; int at; } FCMD;

typedef struct FWORLD {
    uint8_t  explored[FW_H][FW_W], metal[FW_H][FW_W];
    int      threshold;
    float    wind_mean, tide;
    FDEF     defs[FW_DEFS];
    int      ndefs;
    FUNIT    unit[FW_SLOTS];
    int      top;                       /* one past the highest slot ever made live: the site test's walk     */
    int      first[WQ_SEATS], last[WQ_SEATS];
    WQ_ECON  econ[WQ_SEATS];
    int      gametime;
    FCMD     log[FW_LOG];
    int      nlog;
    uint32_t birth[FW_SLOTS];           /* each slot's incarnation                                            */
    int      radar_slot;                /* the slot a scripted radar pass finds; 0: the pass finds nothing    */
    WQ_START starts[8];                 /* the level's start positions                                       */
    int      nstarts;
    int      mw, mh;                    /* the map's size in cells when the military's cases need more, else 0 */
    int      sea;                       /* the sea level                                                      */
    struct { int16_t x0, z0, x1, z1; int8_t depth; } water[FW_WATER];  /* cells inclusive; depth < 0: land */
    int      nwater;
    int      refuse_type;               /* a type every site test refuses, 0 none                             */
} FWORLD;

static FWORLD* fw(const WORLDQ* q) { return (FWORLD*)q->self; }

static int map_w(const FWORLD* w) { return w->mw ? w->mw : FW_W; }
static int map_h(const FWORLD* w) { return w->mh ? w->mh : FW_H; }

/* a cell's height: sea - depth of the last rectangle holding it, else 10 above the sea when the world has water,
   else 10 */
static int cell_h(const FWORLD* w, int cx, int cz)
{
    int k;
    for (k = w->nwater - 1; k >= 0; k--)
        if (cx >= w->water[k].x0 && cx <= w->water[k].x1 && cz >= w->water[k].z0 && cz <= w->water[k].z1)
            return w->sea - w->water[k].depth;
    return w->nwater ? w->sea + 10 : 10;
}

static void water_add(FWORLD* w, int x0, int z0, int x1, int z1, int depth)
{
    if (w->nwater >= FW_WATER) return;
    w->water[w->nwater].x0 = (int16_t)x0; w->water[w->nwater].z0 = (int16_t)z0;
    w->water[w->nwater].x1 = (int16_t)x1; w->water[w->nwater].z1 = (int16_t)z1;
    w->water[w->nwater].depth = (int8_t)depth;
    w->nwater++;
}

/* movement class c at a cell: 0 land (above sea - 12 and below sea + 100, a cliff), 1 sea (below sea - 3), 2
   amphibious everywhere, others nowhere; with no water and the sea at 0 class 0 is everywhere, as the fake had it
   before it had water */
static int pass_at(const FWORLD* w, int c, int cx, int cz)
{
    if (cx < 0 || cz < 0 || cx >= map_w(w) || cz >= map_h(w)) return 0;
    switch (c) {
    case 0: return cell_h(w, cx, cz) >= w->sea - 12 && cell_h(w, cx, cz) < w->sea + 100 ? 3 : 0;
    case 1: return cell_h(w, cx, cz) <= w->sea - 3 ? 3 : 0;
    case 2: return 3;
    }
    return 0;
}

static const FDEF* fdef(const FWORLD* w, int type) { return type > 0 && type < w->ndefs ? &w->defs[type] : NULL; }

static FUNIT* fown(const WORLDQ* q, int slot)
{
    FWORLD* w = fw(q);
    return slot > 0 && slot < FW_SLOTS && w->unit[slot].live && w->unit[slot].owner == q->seat ? &w->unit[slot] : NULL;
}

static int f_map(const WORLDQ* q, WQ_MAP* out)
{
    memset(out, 0, sizeof *out);
    out->w = fw(q)->mw ? fw(q)->mw : FW_W; out->h = fw(q)->mh ? fw(q)->mh : FW_H;
    out->threshold = fw(q)->threshold; out->sea = (uint8_t)fw(q)->sea; out->lostype = 2;
    out->list_cap = FW_LIST; out->wind_mean = fw(q)->wind_mean; out->tide = fw(q)->tide;
    return 1;
}

static int f_explored(const WORLDQ* q, int cx, int cz)
{
    return cx >= 0 && cz >= 0 && cx < FW_W && cz < FW_H && fw(q)->explored[cz][cx];
}

static int f_metal(const WORLDQ* q, int cx, int cz) { return f_explored(q, cx, cz) ? fw(q)->metal[cz][cx] : -1; }

static int f_coverage(const WORLDQ* q, int cx, int cz, int fx, int fz)
{
    int dx, dz, n = 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++) if (f_metal(q, cx + dx, cz + dz) > fw(q)->threshold) n++;
    return n;
}

static int f_mapped_point(const WORLDQ* q, const int32_t pos[3]) { return f_explored(q, pos[0] >> 20, pos[2] >> 20); }
static int f_ground(const WORLDQ* q, const int32_t pos[3]) { return cell_h(fw(q), pos[0] >> 20, pos[2] >> 20); }
static int f_ndefs(const WORLDQ* q) { return fw(q)->ndefs; }

static int f_def(const WORLDQ* q, int type, WQ_DEF* out)
{
    const FDEF* d = fdef(fw(q), type);
    memset(out, 0, sizeof *out);
    if (!d) return 0;
    *out = d->d;
    return 1;
}

static int f_def_econ(const WORLDQ* q, int type, WQ_DEF_ECON* out)
{
    const FDEF* d = fdef(fw(q), type);
    memset(out, 0, sizeof *out);
    if (!d) return 0;
    *out = d->e;
    return 1;
}

static int f_yard(const WORLDQ* q, int type, uint8_t* out, int cap)
{
    const FDEF* d = fdef(fw(q), type);
    if (!d || d->d.fx * d->d.fz > cap) return 0;
    memset(out, d->d.kind == WQ_KIND_GEO ? 0x8F : 0x2F, (size_t)(d->d.fx * d->d.fz));
    return d->d.fx * d->d.fz;
}

static int f_build_options(const WORLDQ* q, int builder_type, uint16_t* out, int cap)
{
    const FDEF* d = fdef(fw(q), builder_type);
    if (!d) return 0;
    if (d->nmenu > cap) return -1;
    memcpy(out, d->menu, (size_t)d->nmenu * sizeof *out);
    return d->nmenu;
}

/* a standing unit's yard: its footprint centred on its position, as a placed building stands */
static void yard_rect(const FWORLD* w, const FUNIT* u, int* x0, int* z0, int* x1, int* z1)
{
    const FDEF* d = fdef(w, u->type);
    const int fx = d ? d->d.fx : 1, fz = d ? d->d.fz : 1;
    *x0 = ((u->pos[0] >> 19) - fx) >> 1;
    *z0 = ((u->pos[2] >> 19) - fz) >> 1;
    *x1 = *x0 + fx - 1;
    *z1 = *z0 + fz - 1;
}

/* inside the placeable interior, the centre explored, no building's yard crossed; a type placed on water only
   fits where the footprint's highest corner lies at least its minwaterdepth under the sea, any other where its
   lowest lies on or above the sea -- with no water every ground is 10 above a sea at 0, so a water type fits
   nowhere -- and refuse_type nowhere */
static int f_site_ok(const WORLDQ* q, int type, int cx, int cz, int ignore, int* alt)
{
    const FWORLD* w = fw(q);
    const FDEF* d = fdef(w, type);
    int s, x, z, hi = -1000, lo = 1000;
    if (!d || type == w->refuse_type || cx < 1 || cz < 1 || cx + d->d.fx >= map_w(w) || cz + d->d.fz >= map_h(w) ||
        !f_explored(q, cx + d->d.fx / 2, cz + d->d.fz / 2))
        return 0;
    for (z = cz; z <= cz + d->d.fz; z++)
        for (x = cx; x <= cx + d->d.fx; x++) {
            const int v = cell_h(w, x, z);
            if (v > hi) hi = v;
            if (v < lo) lo = v;
        }
    if (d->e.minwater > 0 ? hi > w->sea - d->e.minwater : lo < w->sea) return 0;
    for (s = 1; s < w->top; s++) {
        const FUNIT* u = &w->unit[s];
        int x0, z0, x1, z1;
        if (!u->live || s == ignore || !fdef(w, u->type) || (fdef(w, u->type)->d.flags & WQ_DF_MOVER)) continue;
        yard_rect(w, u, &x0, &z0, &x1, &z1);
        if (cx <= x1 && cx + d->d.fx - 1 >= x0 && cz <= z1 && cz + d->d.fz - 1 >= z0) return 0;
    }
    if (alt) *alt = 10;
    return 1;
}

static int f_blocked_alt(const WORLDQ* q, int type, int cx, int cz) { (void)q; (void)type; (void)cx; (void)cz; return 10; }
static int f_feature(const WORLDQ* q, int cx, int cz, WQ_FEAT* out) { (void)q; (void)cx; (void)cz; memset(out, 0, sizeof *out); return 0; }
static int f_nslots(const WORLDQ* q) { (void)q; return FW_SLOTS; }

static int f_seat_units(const WORLDQ* q, int* first, int* last)
{
    *first = fw(q)->first[q->seat];
    *last = fw(q)->last[q->seat];
    return 1;
}

static int f_unit(const WORLDQ* q, int slot, WQ_UNIT* out)
{
    const FUNIT* u = fown(q, slot);
    memset(out, 0, sizeof *out);
    if (!u) return 0;
    out->slot = out->index = (uint16_t)slot;
    out->type = u->type;
    out->owner = u->owner;
    out->finished = u->finished;
    memcpy(out->pos, u->pos, sizeof out->pos);
    out->state = 0x10000000u | (fdef(fw(q), u->type) && fdef(fw(q), u->type)->armed ? 0x80000000u : 0u);
    out->hp = u->hp;
    out->hp_max = fdef(fw(q), u->type) ? fdef(fw(q), u->type)->hp : 0;
    return 1;
}

static int f_orders(const WORLDQ* q, int slot, WQ_ORDER* out, int cap)
{
    const FUNIT* u = fown(q, slot);
    int k;
    if (!u) return -1;
    for (k = 0; k < u->n && k < cap; k++) out[k] = u->list[k];
    return k;
}

static int f_head(const WORLDQ* q, int slot, WQ_ORDER* out)
{
    memset(out, 0, sizeof *out);
    return f_orders(q, slot, out, 1);
}

static int in_menu(const FDEF* d, int type)
{
    int k;
    for (k = 0; d && k < d->nmenu; k++) if (d->menu[k] == type) return 1;
    return 0;
}

/* The resolver's attack case (0x43F0E0 mode 3) for unit def `d` on a target found as `aim`, in the engine's
   order of tests (engine/orders.md, *What each resolver mode answers*). */
static int f_attack(const FDEF* d, unsigned aim)
{
    const unsigned a = d->attack, water = a & (IMPAI_TA_WATER1 | IMPAI_TA_WATER2);
    if (!(a & IMPAI_TA_CANATTACK)) return -1;
    if (!d->armed) return a & IMPAI_TA_KAMIKAZE ? A_ATTACK : -1;
    if (!(aim & WQ_AIM_AIRBORNE) && (a & IMPAI_TA_TOAIR1)) return -1;
    if ((aim & WQ_AIM_UNDER) ? !water : (a & IMPAI_TA_HOVER) && water) return -1;
    if ((a & IMPAI_TA_FLY) && (a & IMPAI_TA_DROP1) && (aim & WQ_AIM_FLIES)) return -1;
    return A_ATTACK;
}

/* the resolver's answers the brain asks: build (14), move (2), guard (7), assist (8), stop (10) */
static int f_can(const WORLDQ* q, int slot, int mode, int target_slot, const int32_t pos[3])
{
    const FWORLD* w = fw(q);
    const FUNIT* u = fown(q, slot);
    const FDEF* d = u ? fdef(w, u->type) : NULL;
    const int mobile = d && (d->d.flags & WQ_DF_MOVER);
    (void)pos;
    if (!d) return -1;
    if (target_slot != (int)WQ_NONE && (target_slot <= 0 || target_slot >= FW_SLOTS || !w->unit[target_slot].live))
        return -1;
    switch (mode) {
    case 14: return mobile && d->nmenu ? A_MOBILEBUILD : -1;
    case 2:  return mobile ? A_MOVE : -1;
    case 7:  return target_slot != (int)WQ_NONE ? A_GUARD : -1;
    case 8:  return target_slot != (int)WQ_NONE && d->nmenu ? A_HELPBUILD : -1;
    case 10: return A_STOP;
    case 3:  return target_slot != (int)WQ_NONE ? f_attack(d, w->unit[target_slot].aim) : d->armed ? A_ATTACK : -1;
    case 9:  return mobile ? A_PATROL : -1;
    case 4:  return d->dgun ? A_DGUN : -1;
    }
    return -1;
}

static int f_action(const WORLDQ* q, int act) { (void)q; return act >= 0 && act < WQ_ACTS ? act + 1 : -1; }

static void logged(FWORLD* w, const WQ_CMD* c)
{
    if (w->nlog < FW_LOG) {
        w->log[w->nlog].c = *c;
        w->log[w->nlog].at = w->gametime;
        w->nlog++;
    }
}

/* NEW replaces the list with the one order (no Shift; the fake keeps no 0x4 nodes); QUEUE appends to a factory's
   list or adds to its last node of the same type, as 0x43B0B0 does, after the adapter's own checks */
static int f_order(const WORLDQ* q, const WQ_CMD* c)
{
    FWORLD* w = fw(q);
    FUNIT* u = fown(q, c->unit);
    const FDEF* d = u ? fdef(w, u->type) : NULL;
    WQ_ORDER* o;
    if (!u || !d) return WQ_REFUSED;
    if (c->target != WQ_NONE && (c->target == 0 || c->target >= FW_SLOTS || !w->unit[c->target].live)) return WQ_REFUSED;
    if (c->verb == WQ_CMD_QUEUE) {
        if (c->count < 1 || c->count > WQ_QUEUE_MAX || !in_menu(d, c->type)) return WQ_REFUSED;
        if (u->n && u->list[u->n - 1].action == A_BUILDINGBUILD && u->list[u->n - 1].type == c->type)
            u->list[u->n - 1].count += c->count;
        else {
            if (u->n >= FW_LIST) return WQ_REFUSED;
            o = &u->list[u->n];
            memset(o, 0, sizeof *o);
            o->place = (uint16_t)u->n;
            o->action = A_BUILDINGBUILD;
            o->type = c->type;
            o->target = (uint16_t)WQ_NONE;
            o->count = c->count;
            u->n++;
        }
        logged(w, c);
        return WQ_OK;
    }
    if (c->verb != WQ_CMD_NEW) return WQ_REFUSED;
    o = &u->list[0];
    memset(o, 0, sizeof *o);
    o->action = c->action;
    o->build = c->action == A_MOBILEBUILD;
    o->type = c->type;
    o->target = c->target;
    memcpy(o->pos, c->pos, sizeof o->pos);
    o->flags = c->action == A_MOBILEBUILD || c->action == A_HELPBUILD ? 0x8u : 0;
    u->n = c->action == A_STOP ? 0 : 1;
    logged(w, c);
    return WQ_OK;
}

static int f_economy(const WORLDQ* q, WQ_ECON* out) { *out = fw(q)->econ[q->seat]; return WQ_OK; }
static int f_gametime(const WORLDQ* q) { return fw(q)->gametime; }

/* the 32-px block of a unit's 16.16 position */
static uint16_t fblock(int32_t v) { return (uint16_t)(v >> 21); }

static int f_observe(const WORLDQ* q, int slot, int contact, WQ_OBS* out)
{
    const FWORLD* w = fw(q);
    memset(out, 0, sizeof *out);
    if (slot <= 0 || slot >= FW_SLOTS || !w->unit[slot].live) return 0;
    if (fown(q, slot)) {
        out->seen = WQ_SEEN_OWN;
        out->owner = (uint8_t)q->seat;
        out->type = w->unit[slot].type;
    } else if (w->unit[slot].seen) {
        out->seen = WQ_SEEN_LOS;
        out->aim = w->unit[slot].aim;
        out->owner = w->unit[slot].owner;
        out->type = w->unit[slot].type;
    } else if (contact) {
        out->seen = WQ_SEEN_CONTACT;
        out->owner = 0xFF;
        out->type = (uint16_t)WQ_NONE;
    } else {
        return 0;
    }
    out->bx = fblock(w->unit[slot].pos[0]);
    out->bz = fblock(w->unit[slot].pos[2]);
    out->birth_known = 1;
    out->birth = w->birth[slot];
    return out->seen;
}

static int f_block_lit(const WORLDQ* q, int bx, int bz) { return f_explored(q, bx * 2, bz * 2); }

static int f_radar_step(const WORLDQ* q, WQ_RADAR* st, uint8_t* bits, int nbytes, const WQ_HITS* hits,
                        int32_t* budget)
{
    const FWORLD* w = fw(q);
    const int k = w->radar_slot;
    (void)budget;
    if (nbytes < (int)WQ_RADAR_BYTES(FW_SLOTS) || hits->n < FW_SLOTS || hits->stride < sizeof(WQ_RADAR_HIT)) return 0;
    if (k > 0 && k < FW_SLOTS && st->stage == WQ_RS_IDLE) {
        memset(bits, 0, (size_t)nbytes);
        if (w->unit[k].live) {
            wq_radar_set(bits, (unsigned)k, WQ_RB_RADAR);
            WQ_RADAR_HIT* h = wq_hit(hits, (unsigned)k);
            h->birth = w->birth[k];
            h->bx = fblock(w->unit[k].pos[0]);
            h->bz = fblock(w->unit[k].pos[2]);
            h->known = 1;
        }
        st->stage = WQ_RS_EMIT;
        return 0;
    }
    if (k <= 0) memset(bits, 0, (size_t)nbytes);
    memset(st, 0, sizeof *st);
    return 1;
}

/* the scripted start positions */
static int f_starts(const WORLDQ* q, WQ_START* out, int cap)
{
    const FWORLD* w = fw(q);
    int k;
    for (k = 0; k < w->nstarts && k < cap; k++) out[k] = w->starts[k];
    return k;
}

static int f_passable(const WORLDQ* q, int mclass, int cx, int cz) { return pass_at(fw(q), mclass, cx, cz); }

static const WORLDQ_OPS FAKE = {
    f_map, f_explored, f_metal, f_coverage, f_mapped_point, f_ground, f_ndefs, f_def, f_def_econ, f_yard,
    f_build_options, f_site_ok, f_blocked_alt, f_feature, f_nslots, f_seat_units, f_unit, f_orders, f_head,
    f_can, f_action, f_order, f_economy, f_gametime, f_observe, f_block_lit, f_radar_step, f_passable, f_starts,
};

/* ---- the world the checks start from ---- */

/* the types: 1 commander, 2 constructor, 3 extractor, 4 solar, 5 wind, 6 maker, 7 factory, 8 tidal, 9 a turret */
enum { T_COM = 1, T_CONS, T_MEX, T_SOLAR, T_WIND, T_MAKER, T_FACTORY, T_TIDAL, T_TURRET, T_N };

static void def_set(FWORLD* w, int t, const char* name, int fx, int fz, int mobile, uint16_t workertime,
                    float cost_m, float cost_e)
{
    FDEF* d = &w->defs[t];
    memset(d, 0, sizeof *d);
    d->d.type = (uint16_t)t;
    d->d.fx = (int16_t)fx;
    d->d.fz = (int16_t)fz;
    d->d.yard = 1;
    d->d.workertime = workertime;
    d->d.mclass = mobile ? 0 : WQ_MCLASS_NONE;
    if (mobile) d->d.flags |= WQ_DF_MOVER;
    snprintf(d->d.name, sizeof d->d.name, "%s", name);
    d->e.cost_m = cost_m;
    d->e.cost_e = cost_e;
    d->e.buildtime = 1000;
    d->e.building = (uint8_t)!mobile;
}

static void menu_set(FWORLD* w, int t, const uint16_t* types, int n)
{
    memcpy(w->defs[t].menu, types, (size_t)n * sizeof *types);
    w->defs[t].nmenu = n;
    w->defs[t].d.flags |= WQ_DF_BUILDER;
}

static void world_init(FWORLD* w)
{
    static const uint16_t BASE[] = { T_MEX, T_SOLAR, T_WIND, T_MAKER, T_FACTORY, T_TIDAL, T_TURRET };
    static const uint16_t PLANT[] = { T_CONS };
    int x, z;
    memset(w, 0, sizeof *w);
    w->threshold = 0;
    w->wind_mean = 0.25f;
    w->ndefs = T_N;
    def_set(w, T_COM, "COMMANDER", 2, 2, 1, 300, 2500, 25000);
    w->defs[T_COM].e.energymake = 25;
    w->defs[T_COM].e.metalmake = 1.5f;
    def_set(w, T_CONS, "CONS", 2, 2, 1, 100, 120, 1500);
    def_set(w, T_MEX, "MEX", 3, 3, 0, 0, 50, 500);
    w->defs[T_MEX].d.kind = WQ_KIND_MEX;
    w->defs[T_MEX].d.strength = w->defs[T_MEX].e.extracts = 0.001f;
    w->defs[T_MEX].e.energyuse = 3;
    def_set(w, T_SOLAR, "SOLAR", 4, 4, 0, 0, 140, 600);
    w->defs[T_SOLAR].e.energyuse = -20;
    def_set(w, T_WIND, "WIND", 3, 3, 0, 0, 40, 170);
    w->defs[T_WIND].e.wind = 40;
    def_set(w, T_MAKER, "MAKER", 3, 3, 0, 0, 1, 1100);
    w->defs[T_MAKER].e.energyuse = 60;
    w->defs[T_MAKER].e.makesmetal = 1;
    def_set(w, T_FACTORY, "FACTORY", 6, 6, 0, 0, 600, 1200);
    def_set(w, T_TIDAL, "TIDAL", 3, 3, 0, 0, 80, 600);
    w->defs[T_TIDAL].e.tidal = 20;
    w->defs[T_TIDAL].e.minwater = 20;
    def_set(w, T_TURRET, "TURRET", 2, 2, 0, 0, 100, 900);
    menu_set(w, T_COM, BASE, (int)(sizeof BASE / sizeof BASE[0]));
    menu_set(w, T_CONS, BASE, (int)(sizeof BASE / sizeof BASE[0]));
    menu_set(w, T_FACTORY, PLANT, 1);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) w->explored[z][x] = x < 64 && z < 64;
    w->first[0] = 1; w->last[0] = 31;
    w->first[1] = 32; w->last[1] = 63;
    w->econ[0].metal = 500; w->econ[0].energy = 500;
    w->econ[0].metal_max = 1000; w->econ[0].energy_max = 1000;
    w->econ[0].metal_in = 1.5f; w->econ[0].energy_in = 25;
}

/* a unit of seat 0 at cell (cx, cz): a building's position is its footprint's centre, as placed */
static void unit_add(FWORLD* w, int slot, int type, int cx, int cz, int finished)
{
    FUNIT* u = &w->unit[slot];
    const FDEF* d = &w->defs[type];
    memset(u, 0, sizeof *u);
    u->live = 1;
    if (slot >= w->top) w->top = slot + 1;
    u->owner = 0;
    u->type = (uint16_t)type;
    u->finished = (uint8_t)finished;
    u->pos[0] = (int32_t)((2 * cx + (d->d.flags & WQ_DF_MOVER ? 1 : d->d.fx)) << 19);
    u->pos[2] = (int32_t)((2 * cz + (d->d.flags & WQ_DF_MOVER ? 1 : d->d.fz)) << 19);
}

/* ---- the checks ---- */

static int s_fail;
static int s_quiet;           /* 1: the economy game prints its summary alone */

#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                              printf(__VA_ARGS__); putchar('\n'); } } while (0)

/* the fake answers the ops A1 added as the header states them */
static void check_world(void)
{
    static FWORLD w;
    WORLDQ q = { &FAKE, &w, 0 };
    WQ_DEF_ECON e;
    WQ_ECON ec;
    WQ_ORDER o;
    WQ_MAP m;
    WQ_CMD c;
    world_init(&w);
    unit_add(&w, 1, T_COM, 20, 20, 1);
    unit_add(&w, 2, T_FACTORY, 30, 30, 1);
    CHECK(q.ops->map(&q, &m) && m.wind_mean == 0.25f && m.tide == 0, "map wind %g tide %g", m.wind_mean, m.tide);
    CHECK(q.ops->def_econ(&q, T_SOLAR, &e) == 1 && e.energyuse == -20 && e.building, "def_econ solar");
    CHECK(q.ops->def_econ(&q, T_N, &e) == 0 && e.cost_m == 0, "def_econ past ndefs");
    CHECK(q.ops->economy(&q, &ec) == WQ_OK && ec.energy_in == 25, "economy");
    CHECK(q.ops->head(&q, 2, &o) == 0, "an idle factory has no head");
    CHECK(q.ops->head(&q, 40, &o) == -1, "no unit, no head");
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_QUEUE; c.unit = 2; c.target = (uint16_t)WQ_NONE; c.type = T_CONS; c.count = 2;
    CHECK(q.ops->order(&q, &c) == WQ_OK, "queue");
    CHECK(q.ops->order(&q, &c) == WQ_OK && q.ops->head(&q, 2, &o) == 1 && o.count == 4 && o.type == T_CONS,
          "a second queue of the type adds to the last node: count %d", o.count);
    c.count = WQ_QUEUE_MAX + 1;
    CHECK(q.ops->order(&q, &c) == WQ_REFUSED, "a count past WQ_QUEUE_MAX is refused");
    c.count = 1; c.type = T_MEX;
    CHECK(q.ops->order(&q, &c) == WQ_REFUSED, "a type off the factory's menu is refused");
}

/* THE RADAR PASS'S PUBLISH (impai.h, A RADAR CONTACT IS THE UNIT THE PASS FOUND): a pass that finds an enemy
   unit at its first tick and completes at its second. Between the two, the unit dies and another takes its
   slot elsewhere -- the publish must make no contact of the slot -- or it moves -- the contact must stay at the
   block the pass found it in, never the live unit's. */
static void check_radar_reuse(void)
{
    static FWORLD w;
    static IMPAI_ROW rows[FW_SLOTS];
    static uint8_t bits[WQ_RADAR_BYTES(FW_SLOTS)];
    static IMPAI_HITROW hits[FW_SLOTS];
    static SNAP_PATCH clusters[IMPAI_CLUSTERS];
    const IMPAI_ARRAYS a = { rows, FW_SLOTS, bits, (int)sizeof bits,
                             { &hits[0].seat[0], FW_SLOTS, (unsigned)sizeof hits[0], 0 }, clusters };
    const IMPAI_BUDGET b = { 0, 100, 0, 0 };
    const IMPAI_SEEN* r = &rows[40].seat[0];
    WORLDQ q = { &FAKE, &w, 0 };
    IMPAI_SEAT st;
    IMPAI_WORK wk;
    int reused;
    for (reused = 1; reused >= 0; reused--) {
        world_init(&w);
        memset(rows, 0, sizeof rows);
        memset(hits, 0, sizeof hits);
        memset(&st, 0, sizeof st);
        unit_add(&w, 40, T_CONS, 10, 10, 1);
        w.unit[40].owner = 1;
        w.birth[40] = 7;
        w.radar_slot = 40;
        w.gametime = impai_radar_phase(0);
        CHECK(impai_know_tick(&q, &st, &a, &b, NULL, &wk) == 0 && st.radar.stage != WQ_RS_IDLE,
              "the scripted pass spans two ticks");
        unit_add(&w, 40, T_CONS, 50, 50, 1);
        w.unit[40].owner = (uint8_t)(reused ? 2 : 1);
        if (reused) w.birth[40] = 8;
        w.gametime++;
        CHECK(impai_know_tick(&q, &st, &a, &b, NULL, &wk) == 0 && st.radar.stage == WQ_RS_IDLE, "the pass completes");
        if (reused)
            CHECK(!r->how, "a slot that changed hands while the pass was open is no contact: how %02x block %u,%u",
                  r->how, r->bx, r->bz);
        else
            CHECK(r->how == IMPAI_RADAR && r->bx == 5 && r->bz == 5,
                  "the contact is where the pass found it: how %02x block %u,%u", r->how, r->bx, r->bz);
        printf("radar publish: %s how=%02x block=%u,%u\n", reused ? "reused slot" : "moved unit", r->how, r->bx,
               r->bz);
    }
}

/* ---- the brain over the fake world ---- */

static FWORLD s_w;
static IMPAI_TINFO s_ti[FW_DEFS];
static IMPAI_JOB s_job[FW_SLOTS];
static IMPAI_BRAIN s_st;
static SNAP_SCRATCH s_snap;
static void* s_scratch;
static uint16_t s_labels[(FW_W / 2) * (FW_H / 2)];
static IMPAI_REACH s_reach;
/* the level row the decisions play, Hard's unless a check says otherwise, and the military the economy reads
   (NULL: none, as at A1) */
static const IMPAI_LEVEL* s_lv = &IMPAI_LEVELS[IMPAI_LV_HARD];
static const IMPAI_MIL* s_milp;

/* the classifier's answers for the fake's types: the commander and the constructor mobile builders, the
   factory a standing one, the turret a defence */
static void roles_set(TASIM_ROLE_OUT* r)
{
    int t;
    memset(r, 0, T_N * sizeof *r);
    for (t = 1; t < T_N; t++) r[t].role = TASIM_ROLE_OTHER;
    r[T_COM].role = r[T_CONS].role = r[T_FACTORY].role = TASIM_ROLE_BUILDER;
    r[T_COM].flags = TASIM_RF_COMMANDER | TASIM_RF_MOBILE;
    r[T_CONS].flags = TASIM_RF_MOBILE;
    r[T_FACTORY].flags = TASIM_RF_FACTORY;
    r[T_TURRET].role = TASIM_ROLE_DEFENCE;
    r[T_TURRET].dps_ground = 1.0;
    r[T_COM].tech = r[T_CONS].tech = 1;
}

/* metal patches: A and C in the commander's component of class 0, B across a wall the labels draw */
static const SNAP_PATCH PATCHES[] = {
    { 40, 20, 40, 20, 42, 22, 9 },      /* A */
    { 52, 10, 52, 10, 54, 12, 9 },      /* B: unreachable */
    { 10, 45, 10, 45, 12, 47, 9 },      /* C */
};
#define NPATCH ((unsigned)(sizeof PATCHES / sizeof PATCHES[0]))
static const SNAP_PATCH* s_spots = PATCHES;
static unsigned s_nspots = NPATCH;

static void brain_world(void)
{
    TASIM_ROLE_OUT roles[T_N];
    WORLDQ q = { &FAKE, &s_w, 0 };
    unsigned i;
    int x, z;
    world_init(&s_w);
    s_spots = PATCHES;
    s_nspots = NPATCH;
    for (i = 0; i < NPATCH; i++)
        for (z = PATCHES[i].z0; z <= PATCHES[i].z1; z++)
            for (x = PATCHES[i].x0; x <= PATCHES[i].x1; x++) s_w.metal[z][x] = 200;
    roles_set(roles);
    CHECK(impai_tinfo_build(&q, roles, T_N, s_ti, FW_DEFS) == T_N, "tinfo");
    impai_brain_reset(&s_st, s_job, FW_SLOTS);
    for (i = 0; i < sizeof s_labels / sizeof s_labels[0]; i++)
        s_labels[i] = (uint16_t)((i % (FW_W / 2)) >= 24 && (i / (FW_W / 2)) < 16 ? 2 : 1);
    memset(&s_reach, 0, sizeof s_reach);
    s_reach.labels = s_labels;
    s_reach.present = 1u;
    s_reach.bw = FW_W / 2;
    s_reach.bh = FW_H / 2;
}

static int decide(IMPAI_BRAIN_OUT* out)
{
    WORLDQ q = { &FAKE, &s_w, 0 };
    IMPAI_BRAIN_IN in;
    memset(&in, 0, sizeof in);
    in.q = &q;
    in.st = &s_st;
    in.job = s_job; in.njob = FW_SLOTS;
    in.ti = s_ti; in.nti = (unsigned)s_w.ndefs;
    in.spots = s_spots; in.nspots = s_nspots;
    in.reach = &s_reach;
    in.lv = s_lv;
    in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
    in.snap = &s_snap;
    in.mil = s_milp;
    return impai_brain_decide(&in, out);
}

static const FCMD* last_cmd(int from) { return s_w.nlog > from ? &s_w.log[s_w.nlog - 1] : NULL; }

static void cmd_print(const FCMD* c)
{
    printf("  t=%d verb=%u action=%u unit=%u target=%d type=%u count=%d pos=%d,%d,%d\n", c->at, c->c.verb,
           c->c.action, c->c.unit, c->c.target == WQ_NONE ? -1 : (int)c->c.target, c->c.type, c->c.count,
           c->c.pos[0], c->c.pos[1], c->c.pos[2]);
}

/* the first cell of a command's footprint, from its centre position */
static int cmd_x0(const WQ_CMD* c) { return (int)((c->pos[0] >> 19) - s_w.defs[c->type].d.fx) / 2; }
static int cmd_z0(const WQ_CMD* c) { return (int)((c->pos[2] >> 19) - s_w.defs[c->type].d.fz) / 2; }

/* the type information: the categories and the gains as impai.h defines them */
static void check_tinfo(void)
{
    unsigned n[IMPAI_CATS];
    brain_world();
    CHECK(s_ti[T_COM].cat == IMPAI_C_COMMANDER && s_ti[T_CONS].cat == IMPAI_C_CONS && s_ti[T_MEX].cat == IMPAI_C_MEX &&
          s_ti[T_SOLAR].cat == IMPAI_C_ENERGY && s_ti[T_WIND].cat == IMPAI_C_ENERGY && s_ti[T_MAKER].cat == IMPAI_C_MAKER &&
          s_ti[T_FACTORY].cat == IMPAI_C_FACTORY && s_ti[T_TIDAL].cat == IMPAI_C_OTHER && s_ti[T_TURRET].cat == IMPAI_C_OTHER,
          "categories");
    CHECK(s_ti[T_SOLAR].egain == 20 && s_ti[T_WIND].egain == 10 && s_ti[T_MAKER].egain == 60 && s_ti[T_MAKER].mgain == 1,
          "gains: solar %g wind %g maker %g/%g", s_ti[T_SOLAR].egain, s_ti[T_WIND].egain, s_ti[T_MAKER].egain,
          s_ti[T_MAKER].mgain);
    CHECK(s_ti[T_TIDAL].flags & IMPAI_TF_WATER, "a tidal generator is placed on water");
    CHECK(s_ti[T_MEX].mgain == 0.001f && s_ti[T_MEX].value == 50 + 500 / 60.0f, "extractor: mgain %g value %g",
          s_ti[T_MEX].mgain, s_ti[T_MEX].value);
    impai_tinfo_count(s_ti, T_N, n);
    printf("tinfo mex=%u energy=%u maker=%u factory=%u cons=%u commander=%u other=%u\n", n[IMPAI_C_MEX],
           n[IMPAI_C_ENERGY], n[IMPAI_C_MAKER], n[IMPAI_C_FACTORY], n[IMPAI_C_CONS], n[IMPAI_C_COMMANDER],
           n[IMPAI_C_OTHER]);
}

/* the decision's phases: seats 0..9 never share a tick at decide 15 */
static void check_due(void)
{
    int seat, t, hits[15] = { 0 };
    for (seat = 0; seat < WQ_SEATS; seat++)
        for (t = 0; t < 15; t++) if (impai_brain_due((unsigned)(1500 + t), (unsigned)seat, s_lv)) hits[t]++;
    for (t = 0; t < 15; t++) CHECK(hits[t] <= 1, "two seats decide at phase %d", t);
}

/* an energy stall: the idle commander builds the best energy per value its menu offers, on the ring */
static void check_stall(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    s_w.econ[0].energy = 100;
    s_w.econ[0].energy_in = 10;
    s_w.econ[0].energy_out = 30;
    s_w.gametime = 300;
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.verb == WQ_CMD_NEW && c->c.type == T_WIND && c->c.action == A_MOBILEBUILD,
          "a stall builds the wind generator (the best egain per value)");
    CHECK(s_job[1].kind == IMPAI_JOB_BUILD && s_job[1].build == T_WIND && s_job[1].type == T_COM, "the job is kept");
    CHECK(s_st.ax == 20 && s_st.az == 20, "the anchor is the commander's cell");
    if (c) cmd_print(c);
}

/* a free metal patch: the commander takes the nearest reachable one; B, beyond the wall, is never chosen; the
   next constructor takes the other one, the first now claimed */
static void check_spot(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    brain_world();
    unit_add(&s_w, 1, T_COM, 30, 22, 1);
    s_w.gametime = 315;
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.type == T_MEX, "the commander builds an extractor");
    CHECK(c && cmd_x0(&c->c) >= 39 && cmd_x0(&c->c) <= 42, "on patch A: first cell %d", c ? cmd_x0(&c->c) : -1);
    unit_add(&s_w, 2, T_CONS, 30, 40, 1);
    s_w.gametime = 330;
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(1);
    CHECK(c && c->c.unit == 2 && c->c.type == T_MEX && cmd_z0(&c->c) >= 44,
          "the constructor takes C, A being claimed and B out of its reach");
    unit_add(&s_w, 3, T_CONS, 50, 12, 1);
    s_w.gametime = 345;
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(2);
    CHECK(c && c->c.unit == 3 && c->c.type == T_MEX && cmd_x0(&c->c) >= 51,
          "the constructor beside B, in B's component, takes B");
    CHECK(out.snaps <= s_lv->snaps && out.sites <= s_lv->site_tests, "the budgets hold");
}

/* the opening factory once two extractors and two energy buildings stand, on the ring around the base */
static void check_factory(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    unit_add(&s_w, 2, T_MEX, 40, 20, 1);
    unit_add(&s_w, 3, T_MEX, 10, 45, 1);
    unit_add(&s_w, 4, T_SOLAR, 24, 20, 1);
    unit_add(&s_w, 5, T_SOLAR, 24, 25, 1);
    s_w.gametime = 600;
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.type == T_FACTORY, "the opening factory");
    CHECK(c && (cmd_x0(&c->c) + 6 <= 22 || cmd_x0(&c->c) >= 30 || cmd_z0(&c->c) + 6 <= 18 || cmd_z0(&c->c) >= 31),
          "outside the base box: %d,%d", c ? cmd_x0(&c->c) : 0, c ? cmd_z0(&c->c) : 0);
    if (c) cmd_print(c);
}

/* the caps: six idle constructors and two idle factories give at most assign_max builder orders and no queue,
   the constructors past their target; two idle factories with none queue one constructor each */
static void check_caps(void)
{
    IMPAI_BRAIN_OUT out;
    int i, queues = 0, builds = 0;
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    for (i = 0; i < 6; i++) unit_add(&s_w, 2 + i, T_CONS, 14 + 2 * i, 30, 1);
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    s_w.gametime = 900;
    CHECK(decide(&out) == 0, "decide");
    for (i = 0; i < s_w.nlog; i++) {
        if (s_w.log[i].c.verb == WQ_CMD_QUEUE) queues++;
        else builds++;
    }
    CHECK(builds <= s_lv->assign_max, "builder orders %d past assign_max", builds);
    CHECK(out.roots <= s_lv->roots && out.queued <= s_lv->queue_max, "roots %u queued %u", out.roots,
          out.queued);
    CHECK(queues == 0, "six constructors stand, past the target of four: %d queued", queues);
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    s_w.gametime = 915;
    CHECK(decide(&out) == 0, "decide");
    for (i = 0, queues = 0; i < s_w.nlog; i++)
        if (s_w.log[i].c.verb == WQ_CMD_QUEUE && s_w.log[i].c.type == T_CONS && s_w.log[i].c.count == 1) queues++;
    CHECK(queues == 2, "two idle factories queue one constructor each: %d", queues);
}

/* the fail ring: a builder idle again with no building at its BUILD site fails the site, which no order of the
   same decision covers */
static void check_fail(void)
{
    IMPAI_BRAIN_OUT out;
    int i, covered = 0;
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    s_job[1].type = T_COM;
    s_job[1].build = T_SOLAR;
    s_job[1].target = (uint16_t)WQ_NONE;
    s_job[1].cx = 26;
    s_job[1].cz = 20;
    s_job[1].kind = IMPAI_JOB_BUILD;
    s_job[1].since = 100;
    s_st.ax = 20;
    s_st.az = 20;
    s_w.econ[0].energy = 100;
    s_w.econ[0].energy_in = 10;
    s_w.econ[0].energy_out = 30;
    s_w.gametime = 400;
    CHECK(decide(&out) == 0, "decide");
    CHECK(out.failed == 1 && s_st.nfail == 1 && s_st.fail[0].cx == 26 && s_st.fail[0].cz == 20 &&
          s_st.fail[0].until == 400u + s_lv->fail_t, "the site fails until GameTime + fail_t");
    for (i = 0; i < s_w.nlog; i++) {
        const WQ_CMD* c = &s_w.log[i].c;
        if (c->verb == WQ_CMD_NEW && c->type && 26 >= cmd_x0(c) && 26 < cmd_x0(c) + s_w.defs[c->type].d.fx &&
            20 >= cmd_z0(c) && 20 < cmd_z0(c) + s_w.defs[c->type].d.fz)
            covered++;
    }
    CHECK(!covered, "an order covers the failed cell");
}

/* The fake's progress between decisions: a builder's build order stands finished at its site and the builder is
   idle; a factory's head makes one constructor; the economy is the standing buildings' sum. */
static void progress(void)
{
    int s, k;
    float ein = 25, eout = 0, min = 1.5f;
    for (s = 1; s < 32; s++) {
        FUNIT* u = &s_w.unit[s];
        if (!u->live || !u->n) continue;
        if (u->list[0].action == A_MOBILEBUILD) {
            const FDEF* d = &s_w.defs[u->list[0].type];
            for (k = 1; k < 32 && s_w.unit[k].live; k++) {}
            if (k < 32) unit_add(&s_w, k, u->list[0].type, (int)((u->list[0].pos[0] >> 19) - d->d.fx) / 2,
                                 (int)((u->list[0].pos[2] >> 19) - d->d.fz) / 2, 1);
            u->n = 0;
        } else if (u->list[0].action == A_BUILDINGBUILD) {
            for (k = 1; k < 32 && s_w.unit[k].live; k++) {}
            if (k < 32) unit_add(&s_w, k, T_CONS, (int)(u->pos[0] >> 20) + 4, (int)(u->pos[2] >> 20) + 4, 1);
            if (--u->list[0].count <= 0) {
                memmove(&u->list[0], &u->list[1], (size_t)(u->n - 1) * sizeof u->list[0]);
                u->n--;
            }
        } else
            u->n = 0;
    }
    for (s = 1; s < 32; s++) {
        const FUNIT* u = &s_w.unit[s];
        const FDEF* d = &s_w.defs[u->type];
        if (!u->live || !u->finished) continue;
        if (u->type == T_MEX) min += 2.0f;
        ein += d->e.energyuse < 0 ? -d->e.energyuse : 0;
        ein += d->e.wind * s_w.wind_mean;
        eout += d->e.energyuse > 0 ? d->e.energyuse : 0;
    }
    s_w.econ[0].energy_in = ein;
    s_w.econ[0].energy_out = eout;
    s_w.econ[0].metal_in = min;
}

/* A game in miniature: a commander alone, sixty decisions with the fake's progress between them. The brain must
   build extractors on both reachable patches, energy, a factory and constructors; the transcript is printed for
   the builds to compare, and a second run must give the same commands. */
typedef struct GAME { unsigned long long digest; int mex, energy, factories, cons; } GAME;

static GAME run_game(int print)
{
    IMPAI_BRAIN_OUT out;
    GAME g;
    int i, k, n[T_N] = { 0 };
    memset(&g, 0, sizeof g);
    g.digest = 1469598103934665603ull;
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    for (i = 0; i < 60; i++) {
        const int from = s_w.nlog;
        s_w.gametime = 15 * (i + 1);
        CHECK(decide(&out) == 0, "decide %d", i);
        CHECK(out.roots <= s_lv->roots && out.sites <= s_lv->site_tests &&
              out.snaps <= s_lv->snaps && out.queued <= s_lv->queue_max, "decision %d's budgets", i);
        for (k = from; k < s_w.nlog; k++) {
            const WQ_CMD* c = &s_w.log[k].c;
            const int32_t v[9] = { s_w.log[k].at, c->verb, c->action, c->unit, c->target, c->type, c->count,
                                   c->pos[0], c->pos[2] };
            int j;
            if (print) cmd_print(&s_w.log[k]);
            for (j = 0; j < 9; j++) g.digest = (g.digest ^ (uint32_t)v[j]) * 1099511628211ull;
        }
        progress();
        if (s_w.nlog >= FW_LOG - 16) s_w.nlog = 0;
    }
    for (k = 1; k < 32; k++) if (s_w.unit[k].live) n[s_w.unit[k].type]++;
    g.mex = n[T_MEX];
    g.energy = n[T_SOLAR] + n[T_WIND] + n[T_TIDAL];
    g.factories = n[T_FACTORY];
    g.cons = n[T_CONS];
    if (print)
        printf("game: mex=%d energy=%d makers=%d factories=%d cons=%d anchor=%d,%d nfail=%u\n", g.mex, g.energy,
               n[T_MAKER], g.factories, g.cons, s_st.ax, s_st.az, s_st.nfail);
    return g;
}

static void check_game(void)
{
    const GAME a = run_game(1), b = run_game(0);
    CHECK(!memcmp(&a, &b, sizeof a), "two runs differ: %016llx %016llx", a.digest, b.digest);
    CHECK(a.mex >= 2, "extractors on both reachable patches: %d", a.mex);
    CHECK(a.energy >= 2, "energy: %d", a.energy);
    CHECK(a.factories >= 1, "a factory: %d", a.factories);
    CHECK(a.cons >= 1, "constructors: %d", a.cons);
    printf("game digest %016llx\n", a.digest);
}

/* ---- the economy game ---- */

/* A game in the economy's own time: impai-eco's seat 1 on Comet Catcher, on the Hard row (s_lv), the level the
   test group pins it to (tools/impai/level_lever.py), with the retail game's CORE numbers (its
   FBIs: costs, build times, worker times, gains, footprints, the constructors' speeds), a commander alone on a
   1000/1000 start, and a tick-by-tick model of what the engine does between decisions -- a builder walks to its
   site at its speed (the commander's as measured, below), a frame stands there and grows by workertime / buildtime
   a second, its cost drawn as it grows and the growth cut to what the stores can pay, an extractor pays its
   extractsmetal times the metal under its footprint a second and every finished unit its gains and upkeep, a
   factory makes constructors one at a time. The commander's menu
   holds CORE's land plants and the shipyard with its water-only constructor and extractor, which the dry map
   refuses everywhere (f_site_ok), as the retail menus do (CORCOM2.GUI, CORSY1.GUI, the download entries of
   CORCS). The wind's mean ratio is 0.25, under which the solar collector wins the energy choice, as it did in the
   impai-eco game (MEASURED 2026-11-12: CORSOLAR built). The brain decides on seat 0's phase, every 15 ticks, to
   GameTime 5400, impai-eco's end: what it has built by then is what that game can be held to.
   THE METAL MAP is the same game on Core Prime Industrial Area's numbers (its OTA: SurfaceMetal 255, no feature,
   wind 25..1500): the threshold 255, every cell 255, no patch, and the wind's mean ratio over 5000 0.1524
   (tagpu_worldq.c, THE WIND'S MEAN). An extractor there pays 0.001 x 9 x 255 = 2.295 a second wherever it stands,
   and the brain places it on the base ring (impai_econ.c, metal_map). */
enum { E_COM = 1, E_CK, E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP, E_SY, E_CV, E_CS, E_UWMEX, E_PT, E_N };

/* Comet Catcher's patches within 1100 px of seat 1's start, cell (324, 35), less 240 cells in x so they fit the
   fake's 128: every 3 x 3 block of metal above the background 3, with its metal (MEASURED 2026-11-12: the
   feature grid's byte +7 in impai-eco's recording at GameTime 30). The game's extractors stood on the first four
   by distance: 223, 112, 223 and 223 under CORMEX's 0.001, so 8.0 metal a second with the commander's 1, the
   income the game reached (impai: eco ... metal_in=8.1). */
#define EGAME_DX 240
static const SNAP_PATCH EPATCHES[] = {
    { 76, 15, 76, 15, 78, 17, 9 },
    { 105, 43, 105, 43, 107, 45, 9 },
    { 70, 55, 70, 55, 72, 57, 9 },
    { 116, 17, 116, 17, 118, 19, 9 },
    { 118, 70, 118, 70, 120, 72, 9 },
    { 110, 81, 110, 81, 112, 83, 9 },
    { 25, 59, 25, 59, 27, 61, 9 },
};
static const uint8_t EMETAL[] = { 223, 112, 223, 223, 56, 223, 112 };
#define NEPATCH ((unsigned)(sizeof EPATCHES / sizeof EPATCHES[0]))

typedef struct EGAME {
    float metal, energy, mstore, estore;
    float m_in, e_in, m_out, e_out;     /* the last pass's, a second, as WQ_ECON reports them        */
    float m_acc_in, e_acc_in, m_acc_out, e_acc_out;   /* the pass in progress                     */
    float built[FW_SLOTS];              /* a frame's fraction built                                  */
    int   born[FW_SLOTS];               /* the GameTime a frame appeared                             */
    int   at_site[FW_SLOTS];            /* a factory's frame in progress, 0 none                     */
    int   first[E_N];                   /* the GameTime the first of a type was finished, 0 never    */
} EGAME;

static EGAME s_eg;

static void egame_def(FWORLD* w, int t, const char* name, int fx, int fz, int mobile, uint16_t wt, float cm, float ce,
                      uint32_t bt)
{
    def_set(w, t, name, fx, fz, mobile, wt, cm, ce);
    w->defs[t].e.buildtime = bt;
}

static void egame_world(int metal_map)
{
    static const uint16_t COMMENU[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP, E_SY, E_UWMEX };
    static const uint16_t CKMENU[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP, E_SY };
    static const uint16_t LABMENU[] = { E_CK };
    static const uint16_t VPMENU[] = { E_CV };
    static const uint16_t SYMENU[] = { E_CS, E_PT };
    static const uint16_t CSMENU[] = { E_SY, E_UWMEX };
    TASIM_ROLE_OUT roles[E_N];
    WORLDQ q = { &FAKE, &s_w, 0 };
    unsigned i;
    int x, z, t;
    memset(&s_w, 0, sizeof s_w);
    s_w.threshold = metal_map ? 255 : 3;
    s_w.wind_mean = metal_map ? 0.1524f : 0.25f;
    s_w.ndefs = E_N;
    egame_def(&s_w, E_COM, "CORCOM", 2, 2, 1, 300, 23512, 35838, 97852);
    s_w.defs[E_COM].e.energymake = 25;
    s_w.defs[E_COM].e.metalmake = 1;
    egame_def(&s_w, E_CK, "CORCK", 2, 2, 1, 80, 130, 2540, 5818);
    s_w.defs[E_CK].e.energymake = 8.5f;
    s_w.defs[E_CK].e.metalmake = 0.25f;
    egame_def(&s_w, E_MEX, "CORMEX", 3, 3, 0, 0, 51, 514, 1874);
    s_w.defs[E_MEX].d.kind = WQ_KIND_MEX;
    s_w.defs[E_MEX].d.strength = s_w.defs[E_MEX].e.extracts = 0.001f;
    s_w.defs[E_MEX].e.energyuse = 3;
    egame_def(&s_w, E_SOLAR, "CORSOLAR", 5, 5, 0, 0, 141, 790, 2416);
    s_w.defs[E_SOLAR].e.energyuse = -20;
    egame_def(&s_w, E_WIND, "CORWIN", 4, 4, 0, 0, 55, 523, 1687);
    s_w.defs[E_WIND].e.wind = 30;
    egame_def(&s_w, E_MAKR, "CORMAKR", 4, 3, 0, 0, 0, 700, 2682);
    s_w.defs[E_MAKR].e.energyuse = 60;
    s_w.defs[E_MAKR].e.makesmetal = 1;
    egame_def(&s_w, E_LAB, "CORLAB", 6, 6, 0, 100, 680, 1250, 7000);
    s_w.defs[E_LAB].e.estore = s_w.defs[E_LAB].e.mstore = 100;
    egame_def(&s_w, E_VP, "CORVP", 7, 7, 0, 100, 600, 1100, 6700);
    s_w.defs[E_VP].e.estore = s_w.defs[E_VP].e.mstore = 100;
    egame_def(&s_w, E_SY, "CORSY", 8, 8, 0, 100, 600, 750, 6000);
    s_w.defs[E_SY].e.minwater = 30;
    egame_def(&s_w, E_CV, "CORCV", 3, 3, 1, 100, 175, 2145, 5146);
    s_w.defs[E_CV].e.energymake = 9.2f;
    s_w.defs[E_CV].e.metalmake = 0.25f;
    egame_def(&s_w, E_CS, "CORCS", 4, 4, 1, 125, 260, 2375, 5537);
    s_w.defs[E_CS].e.minwater = 30;
    egame_def(&s_w, E_UWMEX, "CORUWMEX", 3, 3, 0, 0, 56, 600, 1900);
    s_w.defs[E_UWMEX].d.kind = WQ_KIND_MEX;
    s_w.defs[E_UWMEX].d.strength = s_w.defs[E_UWMEX].e.extracts = 0.001f;
    s_w.defs[E_UWMEX].e.minwater = 15;
    /* CORPT, the shipyard's sea raider, and CORCS travel by the sea class, 1 (the fake's pass_at) */
    egame_def(&s_w, E_PT, "CORPT", 3, 3, 1, 0, 105, 900, 2000);
    s_w.defs[E_PT].e.minwater = 3;
    s_w.defs[E_PT].d.mclass = 1;
    s_w.defs[E_CS].d.mclass = 1;
    menu_set(&s_w, E_COM, COMMENU, (int)(sizeof COMMENU / sizeof COMMENU[0]));
    menu_set(&s_w, E_CK, CKMENU, (int)(sizeof CKMENU / sizeof CKMENU[0]));
    menu_set(&s_w, E_LAB, LABMENU, 1);
    menu_set(&s_w, E_VP, VPMENU, 1);
    menu_set(&s_w, E_SY, SYMENU, (int)(sizeof SYMENU / sizeof SYMENU[0]));
    menu_set(&s_w, E_CV, CKMENU, (int)(sizeof CKMENU / sizeof CKMENU[0]));
    menu_set(&s_w, E_CS, CSMENU, (int)(sizeof CSMENU / sizeof CSMENU[0]));
    for (z = 0; z < FW_H; z++)
        for (x = 0; x < FW_W; x++) {
            s_w.explored[z][x] = 1;
            s_w.metal[z][x] = (uint8_t)(metal_map ? 255 : 3);
        }
    s_spots = metal_map ? NULL : EPATCHES;
    s_nspots = metal_map ? 0 : NEPATCH;
    for (i = 0; i < s_nspots; i++)
        for (z = EPATCHES[i].z0; z <= EPATCHES[i].z1; z++)
            for (x = EPATCHES[i].x0; x <= EPATCHES[i].x1; x++) s_w.metal[z][x] = EMETAL[i];
    s_w.first[0] = 1; s_w.last[0] = 31;
    s_w.first[1] = 32; s_w.last[1] = 63;
    memset(roles, 0, sizeof roles);
    for (t = 1; t < E_N; t++) roles[t].role = TASIM_ROLE_OTHER;
    roles[E_COM].role = roles[E_CK].role = roles[E_CV].role = roles[E_CS].role = TASIM_ROLE_BUILDER;
    roles[E_LAB].role = roles[E_VP].role = roles[E_SY].role = TASIM_ROLE_BUILDER;
    roles[E_COM].flags = TASIM_RF_COMMANDER | TASIM_RF_MOBILE;
    roles[E_CK].flags = roles[E_CV].flags = roles[E_CS].flags = TASIM_RF_MOBILE;
    roles[E_LAB].flags = roles[E_VP].flags = roles[E_SY].flags = TASIM_RF_FACTORY;
    CHECK(impai_tinfo_build(&q, roles, E_N, s_ti, FW_DEFS) == E_N, "tinfo");
    s_ti[E_PT].klass = IMPAI_K_SRAIDER;
    impai_brain_reset(&s_st, s_job, FW_SLOTS);
    memset(&s_reach, 0, sizeof s_reach);
    memset(&s_eg, 0, sizeof s_eg);
    s_eg.metal = s_eg.energy = s_eg.mstore = s_eg.estore = 1000;
}

/* How the commander walks and builds (MEASURED 2026-11-12, impai-eco's recording, seat 1's commander over its eight
   builds: three 3 x 3 extractors, four 5 x 5 solar collectors and the 7 x 7 CORVP):
   - it stops for a build when its offset from the site's centre on each axis is at most half the footprint (8 px a
     cell) plus EGAME_STOP: its larger offset was 22 or 23 over the half at every stop, whatever the approach, so
     61 to 63 px from an extractor approached on the diagonal, 89 from a collector so approached, 102 from the plant;
   - the frame appears on the tick it stops and grows from EGAME_LAG ticks later: each extractor finished 231 ticks
     after its frame appeared, against the 187 of CORMEX's build time at the commander's workertime, stores full;
   - it makes 0.95 to 0.98 px a tick of straight line on each walk of 240 px or more (CORCOM's maxvelocity is 1.07;
     the path curves).
   The constructors are taken to stop as it does [INFERRED]; their speeds are their FBIs' maxvelocity. */
#define EGAME_STOP 23.0f
#define EGAME_LAG 44

/* impai-eco's Fleas are ordered onto seat 1's commander at EGAME_FLEA (tools/replay/fixtures.json), after the
   bar holds, because a D-gun shot, which the military may order (impai_mil.c, dgun), costs 400
   energy (CORE_DISINTEGRATOR's energypershot), and one before then can cost the bar: when the wrapper ran the
   weapon scan with argument 1, which fires CommandFire weapons on their own, and a Peewee was ordered onto the
   commander at 900, the shot came after 1438 (MEASURED 2026-11-12, the recording's seat 1: energy 645 at 1438 and
   259 at 1441, the pass's energy_out 411 with the upkeep), the third extractor stood with the store under the stall
   line, and the ladder built four solar collectors before it ordered the plant, at 4703. Once the bar holds no
   number of shots undoes it: the eco line counts the most of each category any census saw.
   The game runs behind this model on its walks: by 34 ticks at the first two extractors, 248 at the third, whose
   patch the game chose the other way of a near tie, and 503 at the plant's frame (MEASURED, this model with 400
   energy taken from the store at 1438, against the same recording). EGAME_WALK_LAG bounds it, so the bar holds
   here by EGAME_FLEA less EGAME_WALK_LAG. */
#define EGAME_FLEA 4700
#define EGAME_WALK_LAG 600

static float egame_speed(int type) { return type == E_COM ? 0.97f : type == E_CK ? 0.9f : type == E_CV ? 1.3f : 0; }

/* a step of `speed` px toward (x, z) px; 1 when within `reach` */
static int egame_walk(FUNIT* u, float x, float z, float reach)
{
    const float ux = (float)u->pos[0] / 65536.0f, uz = (float)u->pos[2] / 65536.0f, dx = x - ux, dz = z - uz;
    const float d = sqrtf(dx * dx + dz * dz), v = egame_speed(u->type);
    if (d <= reach) return 1;
    u->pos[0] = (int32_t)((ux + dx * (v / d)) * 65536.0f);
    u->pos[2] = (int32_t)((uz + dz * (v / d)) * 65536.0f);
    return 0;
}

/* a step toward the site of a `type` centred at (x, z) px; 1 when the builder stands where it builds from */
static int egame_walk_site(FUNIT* u, float x, float z, int type)
{
    const FDEF* d = &s_w.defs[type];
    const float dx = x - (float)u->pos[0] / 65536.0f, dz = z - (float)u->pos[2] / 65536.0f;
    if (fabsf(dx) <= 8.0f * (float)d->d.fx + EGAME_STOP && fabsf(dz) <= 8.0f * (float)d->d.fz + EGAME_STOP) return 1;
    return egame_walk(u, x, z, 0.0f);
}

static int egame_free_slot(void)
{
    int k;
    for (k = 1; k < 32 && s_w.unit[k].live; k++) {}
    return k < 32 ? k : 0;
}

/* `wt` workertime on frame `f` this tick, its draw cut to what the stores hold; 1 when it finished */
static int egame_grow(int f, float wt)
{
    FUNIT* u = &s_w.unit[f];
    const FDEF* d = &s_w.defs[u->type];
    const float step = wt / (float)d->e.buildtime / 30.0f;
    const float need_m = d->e.cost_m * step, need_e = d->e.cost_e * step;
    float k = 1.0f;
    s_eg.m_acc_out += need_m;
    s_eg.e_acc_out += need_e;
    if (need_m > 0 && s_eg.metal < need_m) k = s_eg.metal / need_m;
    if (need_e > 0 && s_eg.energy < need_e * k) k = s_eg.energy / need_e;
    s_eg.metal -= need_m * k;
    s_eg.energy -= need_e * k;
    s_eg.built[f] += step * k;
    if (s_eg.built[f] < 1.0f) return 0;
    u->finished = 1;
    if (!s_eg.first[u->type]) s_eg.first[u->type] = s_w.gametime;
    return 1;
}

static int egame_frame_at(int type, const int32_t pos[3])
{
    int k;
    for (k = 1; k < 32; k++)
        if (s_w.unit[k].live && s_w.unit[k].type == type && s_w.unit[k].pos[0] == pos[0] && s_w.unit[k].pos[2] == pos[2])
            return k;
    return 0;
}

static void egame_unit(int s)
{
    FUNIT* u = &s_w.unit[s];
    const FDEF* d = &s_w.defs[u->type];
    WQ_ORDER* o = &u->list[0];
    if (!u->live || !u->finished || !u->n) return;
    if (o->action == A_MOBILEBUILD) {
        int f;
        if (!egame_walk_site(u, (float)(o->pos[0] >> 16), (float)(o->pos[2] >> 16), o->type)) return;
        if (!(f = egame_frame_at(o->type, o->pos))) {
            if (!(f = egame_free_slot())) return;
            memset(&s_w.unit[f], 0, sizeof s_w.unit[f]);
            s_w.unit[f].live = 1;
            if (f >= s_w.top) s_w.top = f + 1;
            s_w.unit[f].type = o->type;
            memcpy(s_w.unit[f].pos, o->pos, sizeof o->pos);
            s_eg.built[f] = 0;
            s_eg.born[f] = s_w.gametime;
        }
        if (s_w.gametime - s_eg.born[f] < EGAME_LAG) return;
        if (s_w.unit[f].finished || egame_grow(f, d->d.workertime)) u->n = 0;
    } else if (o->action == A_HELPBUILD || o->action == A_GUARD) {
        FUNIT* t = o->target < FW_SLOTS ? &s_w.unit[o->target] : NULL;
        if (!t || !t->live) { u->n = 0; return; }
        if (!egame_walk_site(u, (float)(t->pos[0] >> 16), (float)(t->pos[2] >> 16), t->type)) return;
        if (o->action == A_HELPBUILD) {
            if (s_w.gametime - s_eg.born[o->target] < EGAME_LAG) return;
            if (t->finished || egame_grow(o->target, d->d.workertime)) u->n = 0;
        } else if (t->n && t->list[0].action == A_BUILDINGBUILD) {
            const int f = s_eg.at_site[o->target];
            if (f && s_w.unit[f].live && !s_w.unit[f].finished) egame_grow(f, d->d.workertime);
        }
    } else if (o->action == A_MOVE) {
        if (egame_walk(u, (float)(o->pos[0] >> 16), (float)(o->pos[2] >> 16), 8.0f)) u->n = 0;
    } else if (o->action == A_BUILDINGBUILD) {
        int f = s_eg.at_site[s];
        if (!f || !s_w.unit[f].live || s_w.unit[f].finished) {
            if (!(f = egame_free_slot())) return;
            memset(&s_w.unit[f], 0, sizeof s_w.unit[f]);
            s_w.unit[f].live = 1;
            if (f >= s_w.top) s_w.top = f + 1;
            s_w.unit[f].type = o->type;
            s_w.unit[f].pos[0] = u->pos[0];
            s_w.unit[f].pos[2] = u->pos[2] + (int32_t)((8 * d->d.fz + 16) << 16);
            s_eg.built[f] = 0;
            s_eg.born[f] = s_w.gametime;
            s_eg.at_site[s] = f;
        }
        if (egame_grow(f, d->d.workertime)) {
            s_eg.at_site[s] = 0;
            if (--o->count <= 0) {
                memmove(&u->list[0], &u->list[1], (size_t)(u->n - 1) * sizeof u->list[0]);
                u->n--;
            }
        }
    } else
        u->n = 0;
}

/* one tick: the units' work, then the gains and the upkeep of every finished unit; each 30 ticks the pass's
   rates are published as WQ_ECON reports them */
static void egame_tick(void)
{
    int s;
    float mstore = 1000, estore = 1000;     /* the start's storage (impai-eco's econ line: 1000 and 1000) */
    float e_gain = 0, m_gain = 0;
    for (s = 1; s < 32; s++) egame_unit(s);
    for (s = 1; s < 32; s++) {
        const FUNIT* u = &s_w.unit[s];
        const FDEF* d = &s_w.defs[u->type];
        const float use = d->e.energyuse > 0 ? d->e.energyuse / 30.0f : 0.0f;
        float e, m;
        int paid;
        if (!u->live || !u->finished) continue;
        /* a unit with an upkeep works only while the store, what the builders left of it, pays it: impai-eco's
           extractors gave no metal while a build spent its stored energy (MEASURED 2026-11-12, the recording's
           seat 1: metal_in 1.00, the commander's alone, at GameTimes 2902, 4282 and 4897 with four extractors
           standing and the energy under 25); the tick's gains come in after every upkeep */
        paid = use > 0 && s_eg.energy >= use;
        e = d->e.energymake + (d->e.energyuse < 0 ? -d->e.energyuse : 0) + d->e.wind * s_w.wind_mean;
        m = d->e.metalmake;
        if (u->type == E_MEX && paid) {
            int x0, z0, x1, z1, x, z;
            yard_rect(&s_w, u, &x0, &z0, &x1, &z1);
            for (z = z0; z <= z1; z++) for (x = x0; x <= x1; x++) m += d->e.extracts * (float)s_w.metal[z][x];
        }
        if (u->type == E_MAKR && paid) m += 1.0f;
        if (paid) {
            s_eg.e_acc_out += use;
            s_eg.energy -= use;
        }
        e_gain += e / 30.0f;
        m_gain += m / 30.0f;
        estore += d->e.estore;
        mstore += d->e.mstore;
    }
    s_eg.e_acc_in += e_gain;
    s_eg.m_acc_in += m_gain;
    s_eg.energy += e_gain;
    s_eg.metal += m_gain;
    s_eg.estore = estore;
    s_eg.mstore = mstore;
    if (s_eg.energy > estore) s_eg.energy = estore;
    if (s_eg.metal > mstore) s_eg.metal = mstore;
    if (s_w.gametime % 30 == 0) {
        s_eg.e_in = s_eg.e_acc_in; s_eg.m_in = s_eg.m_acc_in;
        s_eg.e_out = s_eg.e_acc_out; s_eg.m_out = s_eg.m_acc_out;
        s_eg.e_acc_in = s_eg.m_acc_in = s_eg.e_acc_out = s_eg.m_acc_out = 0;
    }
    s_w.econ[0].metal = s_eg.metal; s_w.econ[0].energy = s_eg.energy;
    s_w.econ[0].metal_max = s_eg.mstore; s_w.econ[0].energy_max = s_eg.estore;
    s_w.econ[0].metal_in = s_eg.m_in; s_w.econ[0].energy_in = s_eg.e_in;
    s_w.econ[0].metal_out = s_eg.m_out; s_w.econ[0].energy_out = s_eg.e_out;
}

/* What one economy game ends with: the units standing at 5400 by type, the orders' counts, and the GameTimes the
   first constructor was queued and impai-eco's bar first held, 0 never. */
typedef struct EGAME_END {
    int n[E_N];
    int orders, queues, wet, snaps;
    int queued_at, bar_at;
} EGAME_END;

/* impai-eco's bar: three extractors, an energy building and a plant finished, and a constructor queued */
static int egame_bar_holds(int queues)
{
    int s, n[E_N] = { 0 };
    for (s = 1; s < 32; s++) if (s_w.unit[s].live && s_w.unit[s].finished) n[s_w.unit[s].type]++;
    return n[E_MEX] >= 3 && n[E_SOLAR] + n[E_WIND] >= 1 && n[E_LAB] + n[E_VP] >= 1 && queues >= 1;
}

/* The game on `metal_map`'s numbers, the commander at cell (cx, cz); its orders and the line at 5400 printed. */
static EGAME_END economy_game(const char* name, int metal_map, int cx, int cz)
{
    IMPAI_BRAIN_OUT out;
    EGAME_END e;
    int s;
    memset(&e, 0, sizeof e);
    egame_world(metal_map);
    unit_add(&s_w, 1, E_COM, cx, cz, 1);
    printf("economy game, %s\n", name);
    for (s_w.gametime = 1; s_w.gametime <= 5400; s_w.gametime++) {
        if (impai_brain_due((unsigned)s_w.gametime, 0, s_lv)) {
            const int from = s_w.nlog;
            int k;
            CHECK(decide(&out) == 0, "decide at %d", s_w.gametime);
            e.snaps += out.snaps;
            for (k = from; k < s_w.nlog; k++) {
                e.orders++;
                if (s_w.log[k].c.verb == WQ_CMD_QUEUE && !e.queues++) e.queued_at = s_w.gametime;
                e.wet += s_w.log[k].c.type == E_SY || s_w.log[k].c.type == E_UWMEX;
                if (!s_quiet) cmd_print(&s_w.log[k]);
            }
            if (s_w.nlog >= FW_LOG - 16) s_w.nlog = 0;
        }
        egame_tick();
        if (!e.bar_at && egame_bar_holds(e.queues)) e.bar_at = s_w.gametime;
    }
    for (s = 1; s < 32; s++) if (s_w.unit[s].live && s_w.unit[s].finished) e.n[s_w.unit[s].type]++;
    printf("economy game at 5400: orders=%d mex=%d solar=%d wind=%d makers=%d plants=%d cons=%d metal=%.0f "
           "energy=%.0f metal_in=%.3f first mex=%d energy=%d plant=%d queued=%d bar=%d\n", e.orders, e.n[E_MEX],
           e.n[E_SOLAR], e.n[E_WIND], e.n[E_MAKR], e.n[E_LAB] + e.n[E_VP], e.n[E_CK] + e.n[E_CV], s_eg.metal,
           s_eg.energy, s_eg.m_in, s_eg.first[E_MEX], s_eg.first[E_SOLAR] ? s_eg.first[E_SOLAR] : s_eg.first[E_WIND],
           s_eg.first[E_VP] ? s_eg.first[E_VP] : s_eg.first[E_LAB], e.queued_at, e.bar_at);
    return e;
}

static void check_economy_game(void)
{
    EGAME_END e;
    /* After the third extractor, at 1959, the plant comes before a fourth (open_t, 1800): CORVP from 1965, finished
       by 3058 with a CORCV queued, then the solar collectors from 4055, each taking the energy the plant's
       constructor leaves. */
    e = economy_game("Comet Catcher", 0, 324 - EGAME_DX, 35);
    CHECK(e.n[E_MEX] >= 3, "three extractors by 5400: %d", e.n[E_MEX]);
    CHECK(e.n[E_SOLAR] + e.n[E_WIND] >= 1, "an energy building by 5400: %d", e.n[E_SOLAR] + e.n[E_WIND]);
    CHECK(e.n[E_LAB] + e.n[E_VP] >= 1 && e.queues >= 1, "a finished factory with a constructor queued by 5400: %d, %d",
          e.n[E_LAB] + e.n[E_VP], e.queues);
    CHECK(!e.wet, "no water-only type ordered on the dry map: %d", e.wet);
    CHECK(e.bar_at && e.bar_at + EGAME_WALK_LAG <= EGAME_FLEA,
          "the bar before the Fleas' order, the walks' lag allowed: %d", e.bar_at);
    /* On the metal map every ring site pays what a rich patch does, so the extractors come first and fast (three
       by 859), then the energy that income asks for, CORVP from 3270 with a CORCV queued when it stands, at 4160,
       and a second plant ordered. Each standing extractor pays the 2.295 its nine cells of 255 give, the
       commander 1. */
    e = economy_game("Core Prime Industrial Area", 1, FW_W / 2, FW_H / 2);
    CHECK(!e.snaps, "no snap onto a patch on a metal map: %d", e.snaps);
    CHECK(fabsf(s_eg.m_in - (1.0f + 2.295f * (float)e.n[E_MEX])) < 0.01f,
          "each extractor on the metal map pays 0.001 x 9 x 255: metal_in %.3f with %d", s_eg.m_in, e.n[E_MEX]);
    CHECK(e.n[E_MEX] >= 4, "four extractors by 5400 on the metal map: %d", e.n[E_MEX]);
    CHECK(e.n[E_SOLAR] + e.n[E_WIND] >= 1, "an energy building by 5400 on the metal map: %d",
          e.n[E_SOLAR] + e.n[E_WIND]);
    CHECK(e.n[E_LAB] + e.n[E_VP] >= 1 && e.queues >= 1,
          "a finished factory with a constructor queued by 5400 on the metal map: %d, %d", e.n[E_LAB] + e.n[E_VP],
          e.queues);
    CHECK(!e.wet, "no water-only type ordered on the metal map: %d", e.wet);
}

/* THE LEVELS (impai_brain.c, IMPAI_LEVELS): the economy game on each row, with a military whose seed gives
   Easy's and Normal's ratios their noise. Hard's must be A1b's game exactly -- its noise is 0, so its ratios'
   factor is exactly 1 -- and A1b's bar is judged there; Easy's and Normal's are reported, slower by design. */
static void check_levels(void)
{
    static IMPAI_MIL mil;
    EGAME_END ref, e;
    int lv, t;
    s_quiet = 1;
    ref = economy_game("Comet Catcher, no military", 0, 324 - EGAME_DX, 35);
    for (lv = 0; lv < IMPAI_LVS; lv++) {
        impai_mil_reset(&mil, (unsigned)lv, 0x2545F4914F6CDD1Dull, &IMPAI_TABLE_DEFAULT, 0);
        mil.naval = 0;              /* the economy game runs no military; its dry world's judgement is 0 */
        s_lv = &IMPAI_LEVELS[lv];
        s_milp = &mil;
        e = economy_game(lv == IMPAI_LV_EASY     ? "Comet Catcher, Easy"
                         : lv == IMPAI_LV_NORMAL ? "Comet Catcher, Normal"
                                                 : "Comet Catcher, Hard",
                         0, 324 - EGAME_DX, 35);
        if (lv == IMPAI_LV_HARD) {
            for (t = 1; t < E_N; t++)
                CHECK(e.n[t] == ref.n[t], "Hard's economy is A1b's: type %d %d against %d", t, e.n[t], ref.n[t]);
            CHECK(e.queues == ref.queues, "Hard's queues are A1b's: %d against %d", e.queues, ref.queues);
        }
        CHECK(IMPAI_LEVELS[lv].mil_period % 15 == 0, "level %d's mil_period is a multiple of 15", lv);
    }
    s_lv = &IMPAI_LEVELS[IMPAI_LV_HARD];
    s_milp = NULL;
    s_quiet = 0;
    for (t = 0; t < 4; t++)
        printf("noise seed=%016llx t=%d: %.8f %.8f\n", (unsigned long long)mil.seed, 15 * t,
               (double)impai_noise(mil.seed, 15u * (unsigned)t, 1, 0), (double)impai_noise(mil.seed, 15u * (unsigned)t, 2, 1));
}

/* ---- the military over the fake world (impai_mil.c) ----

   Three army types beside the economy's: a raider, a tank and a scout, every one armed with one weapon, and the
   fight predictor's content made of them. The world is 512 x 256 cells (8192 x 4096 px) so that 32 contacts fit;
   seat 0 holds slots 1..1279, seat 1 1280..2559. An enemy is a knowledge row of seat 0 (how, owner 1, type,
   block), and a live unit of seat 1 when an order is to reach it. */
enum { M_RAIDER = T_N, M_TANK, M_SCOUT, M_AA, M_SUB, M_DEST, M_N };

static IMPAI_MIL     s_mil;
static IMPAI_ROW     s_rows[FW_SLOTS];
static WQ_RADAR_HIT  s_hits[FW_SLOTS];  /* seat 0's radar records, the view's hits */
static IMPAI_THREAT  s_threat[FW_DEFS];
static IMPAI_FTYPE   s_ft[FW_DEFS];
static IMPAI_FWEAPON s_fwp[FW_DEFS];
static IMPAI_FCONTENT s_fc;
static void*         s_mscratch;
static const IMPAI_REACH* s_mil_reach;      /* the military's reach labels, NULL none (mil_world) */

static void mil_def(int t, const char* name, float cost, uint32_t hp, int32_t speed, uint16_t range, uint16_t dmg)
{
    if (t >= T_N) def_set(&s_w, t, name, 2, 2, 1, 0, cost, 0);
    s_w.defs[t].hp = hp;
    s_w.defs[t].armed = 1;
    s_w.defs[t].attack = IMPAI_TA_CANATTACK;
    s_w.defs[t].d.speed = speed;
    s_threat[t].dps_ground = dmg;
    s_threat[t].range = range;
    s_threat[t].layer = TASIM_LAYER_LAND;
    s_ft[t].hp = hp;
    s_ft[t].value = s_w.defs[t].e.cost_m + s_w.defs[t].e.cost_e / (float)TASIM_ROLES_R;
    s_ft[t].speed = speed;
    s_ft[t].w[0] = (uint16_t)t;
    s_ft[t].flags = IMPAI_FT_MOBILE;
    s_fwp[t].range = range;
    s_fwp[t].reload = 30;
    s_fwp[t].burst = 1;
    s_fwp[t].dmg = dmg;
}

static void mil_world(void)
{
    TASIM_ROLE_OUT roles[M_N];
    WORLDQ q = { &FAKE, &s_w, 0 };
    int t;
    brain_world();
    s_w.ndefs = M_N;
    s_w.mw = 512;
    s_w.mh = 256;
    s_w.first[0] = 1; s_w.last[0] = 1279;
    s_w.first[1] = 1280; s_w.last[1] = 2559;
    memset(s_threat, 0, sizeof s_threat);
    memset(s_ft, 0, sizeof s_ft);
    memset(s_fwp, 0, sizeof s_fwp);
    mil_def(T_COM, "COMMANDER", 0, 3000, 70000, 200, 60);
    mil_def(M_RAIDER, "RAIDER", 120, 400, 160000, 200, 30);
    mil_def(M_TANK, "TANK", 300, 1500, 70000, 300, 60);
    mil_def(M_SCOUT, "SCOUT", 40, 200, 200000, 150, 5);
    /* an anti-air K-bot (weapon 1 toairweapon), a submarine, and a destroyer with a gun and a torpedo */
    mil_def(M_AA, "AAKBOT", 150, 600, 70000, 600, 40);
    s_w.defs[M_AA].attack |= IMPAI_TA_TOAIR1;
    s_fwp[M_AA].flags = TASIM_WF_TOAIR;
    mil_def(M_SUB, "SUB", 400, 1200, 60000, 400, 120);
    s_w.defs[M_SUB].attack |= IMPAI_TA_WATER1;
    s_fwp[M_SUB].flags = TASIM_WF_WATER;
    s_ft[M_SUB].flags |= IMPAI_FT_SUBMERGED;
    mil_def(M_DEST, "DESTROYER", 500, 2000, 60000, 300, 80);
    s_w.defs[M_DEST].attack |= IMPAI_TA_WATER2;
    s_ft[M_DEST].w[1] = M_SUB;
    s_ft[T_COM].flags |= IMPAI_FT_COMMANDER;
    roles_set(roles);
    for (t = T_N; t < M_N; t++) {
        memset(&roles[t], 0, sizeof roles[t]);
        roles[t].flags = TASIM_RF_MOBILE;
        roles[t].layer = TASIM_LAYER_LAND;
    }
    roles[M_RAIDER].role = TASIM_ROLE_RAIDER;
    roles[M_TANK].role = TASIM_ROLE_ASSAULT;
    roles[M_SCOUT].role = TASIM_ROLE_SCOUT;
    CHECK(impai_tinfo_build(&q, roles, M_N, s_ti, FW_DEFS) == M_N, "the military's tinfo");
    s_ti[M_RAIDER].klass = IMPAI_K_LRAIDER;
    s_ti[M_TANK].klass = IMPAI_K_LASSAULT;
    s_ti[M_SCOUT].klass = IMPAI_K_LSCOUT;
    s_ti[M_AA].klass = IMPAI_K_LAA;
    s_ti[M_DEST].klass = IMPAI_K_LASSAULT;
    for (t = T_COM; t < M_N; t++) s_ti[t].attack = s_w.defs[t].attack;
    s_fc.t = s_ft; s_fc.nt = M_N;
    s_fc.w = s_fwp; s_fc.nw = M_N;
    s_fc.d = NULL; s_fc.nd = 0;
    memset(s_rows, 0, sizeof s_rows);
    memset(s_hits, 0, sizeof s_hits);
    s_st.ax = 20;
    s_st.az = 20;
    impai_mil_reset(&s_mil, IMPAI_LV_HARD, 0x2545F4914F6CDD1Dull, &IMPAI_TABLE_DEFAULT, 0);
    s_mil_reach = NULL;
    s_w.gametime = 38;                       /* a military tick of seat 0 at every level: 38 % 15 == 8 */
}

/* an army unit of seat 0 at cell (cx, cz), finished, at full health */
static void own(int slot, int type, int cx, int cz)
{
    unit_add(&s_w, slot, type, cx, cz, 1);
    s_w.unit[slot].hp = (int)s_w.defs[type].hp;
}

/* an enemy at cell (cx, cz), known to seat 0 by `how`; live as a unit of seat 1 when `live`, which the seat
   sees by sight when `how` says it does */
static void enemy(int slot, int type, int cx, int cz, unsigned how, int live)
{
    IMPAI_SEEN* r = &s_rows[slot].seat[0];
    if (live) {
        unit_add(&s_w, slot, type, cx, cz, 1);
        s_w.unit[slot].owner = 1;
        s_w.unit[slot].hp = (int)s_w.defs[type].hp;
        s_w.unit[slot].seen = (uint8_t)((how & (IMPAI_LOS | IMPAI_SHARED)) != 0);
    }
    r->type = (uint16_t)type;
    r->how = (uint8_t)how;
    r->owner = 1;
    r->bx = (uint16_t)(cx / 2);
    r->bz = (uint16_t)(cz / 2);
}

/* seat 0's radar pass found the unit slot `slot` holds now: its record names the slot's incarnation */
static void radar_found(int slot)
{
    s_hits[slot].birth = s_w.birth[slot];
    s_hits[slot].bx = fblock(s_w.unit[slot].pos[0]);
    s_hits[slot].bz = fblock(s_w.unit[slot].pos[2]);
    s_hits[slot].known = 1;
}

static int mil_decide(const IMPAI_LEVEL* lv, int with_content, IMPAI_MIL_OUT* out)
{
    WORLDQ q = { &FAKE, &s_w, 0 };
    IMPAI_VIEW v;
    IMPAI_FCONTENT none;
    IMPAI_MIL_IN in;
    memset(&none, 0, sizeof none);
    memset(&v, 0, sizeof v);
    v.seat = 0;
    v.rows = s_rows;
    v.nrows = FW_SLOTS;
    v.threat = s_threat;
    v.ntypes = M_N;
    v.w = s_w.mw;
    v.h = s_w.mh;
    v.hits.at = s_hits;
    v.hits.n = FW_SLOTS;
    v.hits.stride = (unsigned)sizeof(WQ_RADAR_HIT);
    memset(&in, 0, sizeof in);
    in.q = &q;
    in.view = &v;
    in.st = &s_mil;
    in.job = s_job; in.njob = FW_SLOTS;
    in.ti = s_ti; in.nti = M_N;
    in.fc = with_content ? &s_fc : &none;
    in.fit = &impai_fitparams;
    in.brain = &s_st;
    in.lv = lv;
    in.reach = s_mil_reach;
    in.scratch = s_mscratch; in.scratch_bytes = impai_mil_scratch_bytes();
    return impai_mil_decide(&in, out);
}

static void mil_print(const char* name, const IMPAI_MIL_OUT* o, int from)
{
    char sq[IMPAI_SQUADS + 1];
    int k, n = 0;
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (s_mil.sq[k].task != IMPAI_SQ_FREE) sq[n++] = impai_mil_letter(s_mil.sq[k].task);
    sq[n] = 0;
    printf("mil %s: army=%u enemies=%u contacts=%u threats=%u predictions=%u/%u roots=%u refused=%u left_out=%u "
           "attack=%u raid=%u retreat=%u scout=%u defend=%u safe=%u squads=%s\n", name, o->army, o->enemies,
           o->contacts, o->threats, o->predictions, o->refused_predictions, o->roots, o->refused, o->left_out,
           o->attacks, o->raids, o->retreats, o->scouts, o->defends, o->safe, n ? sq : "-");
    for (k = from; k < s_w.nlog && k < from + 12; k++) cmd_print(&s_w.log[k]);
}

/* the seat sees the target of every attack order it gave since `from` now */
static int targets_seen(int from)
{
    int k;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->action != A_ATTACK) continue;
        if (c->target == WQ_NONE || c->target >= FW_SLOTS || !(s_rows[c->target].seat[0].how & (IMPAI_NOW | IMPAI_PASS)))
            return 0;
    }
    return 1;
}

static int count_action(int from, int action, int unit)
{
    int k, n = 0;
    for (k = from; k < s_w.nlog; k++)
        n += s_w.log[k].c.action == action && (unit < 0 || s_w.log[k].c.unit == unit);
    return n;
}

/* THE BASE'S DEFENCE AND THE FIGHT: eight tanks at the base, six enemy tanks seen 256 px off it, a seventh only
   a ghost. At Hard (react_confirm 1) the first decision makes the FORM squad DEFEND, predicts the fight and
   orders it, every target one the seat sees now, and the tank under micro_hp steps back towards the rally
   point instead. */
static void check_mil_defence(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    for (k = 0; k < 8; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    s_w.unit[8].hp = 300;                        /* 20 % of a tank's health: under Hard's micro_hp, 0.35 */
    for (k = 0; k < 6; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40, IMPAI_LOS, 1);
    enemy(1306, M_TANK, 20, 40, IMPAI_GHOST, 1);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the defence decision runs");
    mil_print("defence", &o, from);
    CHECK(o.army == 8 && o.enemies == 6 && o.contacts == 1 && o.threats == 1, "the picture: %u army, %u enemies",
          o.army, o.enemies);
    CHECK(o.defends == 1 && s_mil.sq[0].task == IMPAI_SQ_DEFEND, "the FORM squad defends");
    CHECK(o.predictions == 1 && !o.retreats, "one prediction, no retreat");
    CHECK(count_action(from, A_ATTACK, -1) == 7 && targets_seen(from), "seven attack orders, every target seen now");
    CHECK(count_action(from, A_MOVE, 8) == 1 && count_action(from, A_ATTACK, 8) == 0,
          "the damaged tank steps back");
    from = s_w.nlog;
    s_w.gametime += IMPAI_LEVELS[IMPAI_LV_HARD].mil_period;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the second decision runs");
    CHECK(count_action(from, A_ATTACK, -1) == 0, "an order the unit's head already says is not given again");
    CHECK(count_action(from, A_MOVE, 8) == 0, "micro waits micro_period");
    mil_print("defence again", &o, from);
}

/* A TARGET GONE SINCE ITS ROW: eight tanks against six enemy tanks the rows say are seen now, of which the
   two nearest the squad died since the slice last visited their slots (their rows still read LOS) and a third
   left the seat's sight. Every tank but the damaged one is ordered at one of the three still there, none at a
   slot that is gone, and no order is refused. The same with the gone two of the type the focus picks: the
   focus moves to a tank that is there. */
static void check_mil_gone_target(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    for (k = 0; k < 8; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    s_w.unit[8].hp = 300;
    for (k = 0; k < 6; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40 - (k < 2 ? 4 : 0), IMPAI_LOS, k >= 2);
    s_w.unit[1302].seen = 0;
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the decision runs");
    mil_print("gone targets", &o, from);
    CHECK(o.enemies == 6 && o.contacts == 1, "the rows still hold all six");
    CHECK(o.refused == 0, "no order is refused: %u", o.refused);
    CHECK(count_action(from, A_ATTACK, -1) == 7, "seven attack orders");
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.action == A_ATTACK)
            CHECK(s_w.log[k].c.target >= 1303 && s_w.log[k].c.target <= 1305,
                  "an attack at a tank the seat sees now, not %u", s_w.log[k].c.target);
}

/* THE RESOLVER'S RULE, from the military's side: no member is ordered to attack what the resolver refuses it,
   and a member with nothing it can attack keeps with the squad. Each order since `from` is checked against the
   mock resolver's attack case on its target. */
static int attacks_answered(int from)
{
    int k;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->action != A_ATTACK) continue;
        if (f_attack(&s_w.defs[s_w.unit[c->unit].type], s_w.unit[c->target].aim) < 0) return 0;
    }
    return 1;
}

/* the target of unit `unit`'s last attack order since `from`, WQ_NONE for none */
static uint16_t attack_target(int from, int unit)
{
    uint16_t t = (uint16_t)WQ_NONE;
    int k;
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.action == A_ATTACK && s_w.log[k].c.unit == unit) t = s_w.log[k].c.target;
    return t;
}

/* how far own unit `unit` stands from the centroid of own units `first`..`last`, px, as the squad's centroid is
   taken: the mean of the members' whole-pixel positions */
static uint32_t off_centroid(int unit, int first, int last)
{
    int64_t sx = 0, sz = 0, dx, dz;
    int k;
    for (k = first; k <= last; k++) {
        sx += s_w.unit[k].pos[0] >> 16;
        sz += s_w.unit[k].pos[2] >> 16;
    }
    dx = (s_w.unit[unit].pos[0] >> 16) - sx / (last - first + 1);
    dz = (s_w.unit[unit].pos[2] >> 16) - sz / (last - first + 1);
    return impai_isqrt((uint64_t)(dx * dx + dz * dz));
}

/* AN ANTI-AIR UNIT IN A GROUND FIGHT: six tanks and two anti-air K-bots against five tanks on the ground. The
   tanks attack; the K-bots' weapon 1 is toairweapon, which the resolver refuses on a target not airborne, so
   they keep with the squad instead, and nothing is refused: beside it (within SPREAD_PX, 64 px, of its
   centroid) they are given nothing, so they cost no order; 14 cells behind it each moves to the centroid. */
static void check_mil_aa_in_ground_fight(void)
{
    IMPAI_MIL_OUT o;
    int k, from, far;
    for (far = 0; far <= 1; far++) {
        mil_world();
        for (k = 0; k < 6; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
        own(7, M_AA, 26, far ? 8 : 22);
        own(8, M_AA, 30, far ? 8 : 22);
        for (k = 0; k < 5; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40, IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the decision runs");
        mil_print(far ? "anti-air behind a ground fight" : "anti-air in a ground fight", &o, from);
        CHECK(o.refused == 0, "no order is refused: %u", o.refused);
        CHECK(count_action(from, A_ATTACK, -1) == 6 && attacks_answered(from), "the six tanks attack");
        CHECK((off_centroid(7, 1, 8) > 64) == far && (off_centroid(8, 1, 8) > 64) == far,
              "the K-bots stand %u and %u px off the centroid", off_centroid(7, 1, 8), off_centroid(8, 1, 8));
        CHECK(count_action(from, A_ATTACK, 7) == 0 && count_action(from, A_MOVE, 7) == far &&
              count_action(from, A_ATTACK, 8) == 0 && count_action(from, A_MOVE, 8) == far,
              "%s", far ? "each anti-air K-bot moves to the squad" : "the K-bots beside the squad are given nothing");
        CHECK(o.roots == 6 + 2 * far, "orders %u", o.roots);
    }
}

/* THE ORDER BUDGET GOES TO THE ATTACKS: the same fight under Easy's six orders a decision (mil_roots), the four
   anti-air K-bots in the census before the six tanks. Beside the squad the K-bots take none of the six; behind
   it they wait for what the attacks leave, and here the six tanks take all six. */
static void check_mil_budget(void)
{
    IMPAI_LEVEL lv = IMPAI_LEVELS[IMPAI_LV_HARD];
    IMPAI_MIL_OUT o;
    int k, from, far;
    lv.mil_roots = IMPAI_LEVELS[IMPAI_LV_EASY].mil_roots;
    for (far = 0; far <= 1; far++) {
        mil_world();
        for (k = 0; k < 4; k++) own(1 + k, M_AA, 28 + k % 2, far ? 8 : 24 + k / 2);
        for (k = 0; k < 6; k++) own(5 + k, M_TANK, 24 + 2 * k, 24);
        for (k = 0; k < 5; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40, IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&lv, 1, &o) == 0, "the budget decision runs");
        mil_print(far ? "six orders, anti-air behind" : "six orders, anti-air beside", &o, from);
        for (k = 1; k <= 4; k++)
            CHECK((off_centroid(k, 1, 10) > 64) == far, "K-bot %d stands %u px off the centroid", k,
                  off_centroid(k, 1, 10));
        CHECK(o.roots == lv.mil_roots && count_action(from, A_ATTACK, -1) == 6 && count_action(from, A_MOVE, -1) == 0,
              "the six orders are the six tanks' attacks: %u orders, %d attacks, %d moves", o.roots,
              count_action(from, A_ATTACK, -1), count_action(from, A_MOVE, -1));
    }
}

/* A SUBMARINE AND MIXED WEAPONS: four tanks and two destroyers (a gun, and a torpedo as weapon 2) against a
   submarine under the sea nearest them and two tanks beyond it. The resolver answers a target under the sea
   only for a unit with a waterweapon as weapon 1 or 2: the destroyers may attack the submarine, the tanks
   only the surface tanks, and nothing is refused. With the surface tanks gone, the tanks have nothing to
   attack and keep with the squad: the one farther than SPREAD_PX (64 px) from its centroid moves to it, the
   others are given nothing. */
static void check_mil_sub_mixed_weapons(void)
{
    IMPAI_MIL_OUT o;
    int k, from, gone;
    for (gone = 0; gone <= 1; gone++) {
        mil_world();
        for (k = 0; k < 4; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
        own(5, M_DEST, 32, 24);
        own(6, M_DEST, 34, 24);
        enemy(1300, M_SUB, 28, 32, IMPAI_LOS, 1);
        s_w.unit[1300].aim = WQ_AIM_UNDER;
        if (!gone) {
            enemy(1301, M_TANK, 24, 42, IMPAI_LOS, 1);
            enemy(1302, M_TANK, 34, 42, IMPAI_LOS, 1);
        }
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the decision runs");
        mil_print(gone ? "a submarine alone" : "a submarine and two tanks", &o, from);
        CHECK(o.refused == 0, "no order is refused: %u", o.refused);
        CHECK(attacks_answered(from), "every attack is one the resolver answers");
        for (k = 1; k <= 4; k++)
            CHECK(gone ? count_action(from, A_ATTACK, k) == 0 &&
                             count_action(from, A_MOVE, k) == (off_centroid(k, 1, 6) > 64)
                       : count_action(from, A_ATTACK, k) == 1 && attack_target(from, k) != 1300,
                  "tank %d: %s", k, gone ? "keeps with the squad" : "attacks a surface tank");
        if (gone)
            CHECK(count_action(from, A_MOVE, -1) == 1 && count_action(from, A_MOVE, 1) == 1,
                  "tank 1 alone, %u px off the centroid, moves", off_centroid(1, 1, 6));
        CHECK(count_action(from, A_ATTACK, 5) == 1 && count_action(from, A_ATTACK, 6) == 1,
              "the destroyers attack");
    }
}

/* A SLOT TAKEN SINCE ITS ROW: six tanks against five enemy rows, each of whose slot holds a unit now. 1300's row
   says a tank of seat 1, and a raider of seat 1 holds the slot; 1301's a tank of seat 1, and a tank of seat 2
   holds it; 1302 is a radar contact the pass found, whose slot another unit holds since (its incarnation is
   not the record's). None of the three is attacked. 1303, a radar contact the pass found, and 1304, a tank in
   sight, are, and no order is refused. */
static void check_mil_slot_taken(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    for (k = 0; k < 6; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    for (k = 0; k < 5; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40, k == 2 || k == 3 ? IMPAI_RADAR : IMPAI_LOS, 1);
    s_w.unit[1300].type = M_RAIDER;
    s_w.unit[1301].owner = 2;
    radar_found(1302);
    radar_found(1303);
    s_w.birth[1302]++;
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the decision runs");
    mil_print("slots taken since their rows", &o, from);
    CHECK(o.enemies == 5 && o.refused == 0, "five enemy rows, %u, and no order refused, %u", o.enemies, o.refused);
    CHECK(count_action(from, A_ATTACK, -1) == 6, "six attack orders");
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.action == A_ATTACK)
            CHECK(s_w.log[k].c.target == 1303 || s_w.log[k].c.target == 1304,
                  "an attack at the unit its row is about, not %u", s_w.log[k].c.target);
}

/* A RADAR CONTACT'S RESOLVER TESTS ARE THE TARGET'S: six tanks and two anti-air K-bots against five tanks the
   seat knows by its radar alone. Whether a target is airborne, under the sea or of a type that flies is not the
   seat's to see on a radar contact (observe gives no aim there), so each member is ordered at one, and the
   resolver refuses the K-bots' -- counted as refused and costing no order -- as it did before the military
   read the resolver's tests. */
static void check_mil_radar_aim(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    for (k = 0; k < 6; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    own(7, M_AA, 26, 22);
    own(8, M_AA, 30, 22);
    for (k = 0; k < 5; k++) {
        enemy(1300 + k, M_TANK, 24 + 2 * k, 40, IMPAI_RADAR, 1);
        radar_found(1300 + k);
    }
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the decision runs");
    mil_print("anti-air against radar contacts", &o, from);
    CHECK(count_action(from, A_ATTACK, -1) == 6 && attacks_answered(from), "the six tanks attack");
    CHECK(o.refused == 2 && o.roots == 6 && s_w.nlog - from == 6,
          "the resolver refuses the K-bots: %u refused, %u orders", o.refused, o.roots);
}

/* RETREAT: two raiders against ten tanks predict below retreat_margin and fall back; with no prediction to be
   had (no content) the squad keeps its task and fights. */
static void check_mil_retreat(void)
{
    IMPAI_MIL_OUT o;
    int k, from, content;
    for (content = 1; content >= 0; content--) {
        mil_world();
        own(1, M_RAIDER, 100, 100);
        own(2, M_RAIDER, 102, 100);
        for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 96 + 2 * k, 116, IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], content, &o) == 0, "the retreat decision runs");
        mil_print(content ? "retreat" : "retreat, no content", &o, from);
        if (content)
            CHECK(o.retreats == 1 && count_action(from, A_MOVE, -1) == 2 && !count_action(from, A_ATTACK, -1),
                  "two raiders against ten tanks fall back");
        else
            CHECK(!o.retreats && o.refused_predictions == 1 && count_action(from, A_ATTACK, -1) == 2,
                  "a refused prediction changes no task");
    }
}

/* THE ATTACK WAVE: twelve tanks and no enemy mobile seen; the enemy's extractor remembered far off makes the
   estimate, the wave is worth wave_value army units and the prediction against the target's defences (none)
   is 1, so the squad attacks, travelling to the target in a spread. */
static void check_mil_attack(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    for (k = 0; k < 12; k++) own(1 + k, M_TANK, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the attack decision runs");
    mil_print("attack", &o, from);
    CHECK(o.attacks == 1 && s_mil.waves == 1 && s_mil.ebx == 200 && s_mil.ebz == 100,
          "one wave to the estimate (%d, %d)", s_mil.ebx, s_mil.ebz);
    CHECK(count_action(from, A_MOVE, -1) == 12 && !count_action(from, A_ATTACK, -1), "the twelve travel to the target");
    CHECK(!o.scouts, "no scout: the whole FORM squad went");
}

/* RAIDS AND SCOUTING: a tank, three raiders and a scout, too few for a wave; the remembered extractor is raided by
   the three raiders (the fastest type), and scouting sends the scout over the start positions, three legs, then
   back to FORM. */
static void check_mil_raid_scout(void)
{
    static const WQ_START STARTS[] = { { 328, 328, 0 }, { 6000, 328, 1 }, { 328, 3000, 2 }, { 6000, 3000, 3 } };
    IMPAI_MIL_OUT o;
    int k, from, leg, s = -1;
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 4;
    own(1, M_TANK, 24, 24);
    for (k = 0; k < 3; k++) own(4 + k, M_RAIDER, 24 + 2 * k, 28);
    own(7, M_SCOUT, 30, 30);
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
    s_mil.scout_next = 250;                  /* a recorded byte past the list: taken modulo its count */
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the raid decision runs");
    mil_print("raid and scout", &o, from);
    CHECK(o.raids == 1 && o.scouts == 1 && !o.attacks, "one raid and one scout");
    for (k = 0; k < IMPAI_SQUADS; k++) {
        if (s_mil.sq[k].task == IMPAI_SQ_RAID) CHECK(s_mil.sq[k].n == 3, "the raid takes the three raiders");
        if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    }
    CHECK(s >= 0 && s_job[7].kind == IMPAI_JOB_SQUAD && s_job[7].squad == s, "the scout is a squad of one");
    for (leg = 0; leg < 3 && s >= 0; leg++) {
        const int32_t x = s_mil.sq[s].tx * 32 + 16, z = s_mil.sq[s].tz * 32 + 16;
        printf("scout leg %d: (%d, %d)\n", leg, x, z);
        CHECK(!(x < 1000 && z < 1000), "a leg never goes to the seat's own start");
        s_w.unit[7].pos[0] = x << 16;
        s_w.unit[7].pos[2] = z << 16;
        s_w.gametime += IMPAI_LEVELS[IMPAI_LV_HARD].mil_period;
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "a scout leg's decision runs");
    }
    CHECK(s >= 0 && s_mil.sq[s].task != IMPAI_SQ_SCOUT, "the scout is back after three legs");
}

/* THE COMMANDER'S SAFETY: twelve tanks near the commander, worth more than it, predict it the loser: it moves
   away and is the military's for 300 ticks, in which the economy gives it no order. With no prediction to be
   had it moves too. */
static void check_mil_commander(void)
{
    IMPAI_MIL_OUT o;
    IMPAI_BRAIN_OUT bo;
    int k, from, content;
    for (content = 1; content >= 0; content--) {
        mil_world();
        own(1, T_COM, 20, 20);
        for (k = 0; k < 12; k++) enemy(1300 + k, M_TANK, 20 + 2 * (k % 6), 40 + 2 * (k / 6), IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], content, &o) == 0, "the commander decision runs");
        mil_print(content ? "commander" : "commander, no content", &o, from);
        CHECK(o.safe == 1 && s_mil.safe_until == (uint32_t)s_w.gametime + 300 && count_action(from, A_MOVE, 1) == 1,
              "the commander moves away and is the military's until %u", s_mil.safe_until);
    }
    from = s_w.nlog;
    s_milp = &s_mil;
    s_w.gametime += 2;
    CHECK(decide(&bo) == 0, "the economy decides inside the window");
    for (k = from; k < s_w.nlog; k++) CHECK(s_w.log[k].c.unit != 1, "the economy orders the commander inside the window");
    s_milp = NULL;
}

/* EVERY BOUND: more army than IMPAI_ARMY_MAX, more enemy rows than IMPAI_ENEMY_MAX, more separated enemies than
   IMPAI_CONTACTS, a squad table full, more predictions asked than Easy's predict_max and more orders than its
   mil_roots, and recorded bytes past their tables. */
static void check_mil_bounds(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    for (k = 0; k < 600; k++) own(1 + k, M_TANK, 4 + 2 * (k % 40), 4 + 2 * (k / 40));
    for (k = 0; k < 2100; k++) enemy(k < 1279 ? 1279 - k : 1280 + (k - 1279), T_MEX, 480, 240, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the bounds decision runs");
    mil_print("army and enemy bounds", &o, s_w.nlog);
    CHECK(o.army == IMPAI_ARMY_MAX && o.enemies == IMPAI_ENEMY_MAX && o.left_out >= 600 - IMPAI_ARMY_MAX,
          "army %u, enemies %u, left out %u", o.army, o.enemies, o.left_out);

    mil_world();
    own(1, M_TANK, 4, 4);
    s_w.mh = 320;
    for (k = 0; k < 40; k++) enemy(1300 + k, M_TANK, 16 + 64 * (k % 8), 16 + 64 * (k / 8), IMPAI_LOS, 0);
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the contacts decision runs");
    mil_print("contact bound", &o, s_w.nlog);
    CHECK(o.contacts == IMPAI_CONTACTS && o.left_out == 40 - IMPAI_CONTACTS, "contacts %u, left out %u", o.contacts,
          o.left_out);

    mil_world();
    for (k = 0; k < IMPAI_SQUADS; k++) {
        s_mil.sq[k].task = IMPAI_SQ_ATTACK;
        s_mil.sq[k].layer = TASIM_LAYER_AIR;
        s_mil.sq[k].target = (uint16_t)WQ_NONE;
        s_mil.sq[k].tx = 200; s_mil.sq[k].tz = 100;
        s_mil.sq[k].since = (uint32_t)s_w.gametime;
        own(1 + k, M_TANK, 24 + 2 * k, 24);
        s_job[1 + k].kind = IMPAI_JOB_SQUAD;
        s_job[1 + k].type = M_TANK;
        s_job[1 + k].squad = (uint8_t)k;
    }
    own(40, M_TANK, 24, 30);
    s_job[41].kind = IMPAI_JOB_SQUAD;
    s_job[41].type = M_TANK;
    s_job[41].squad = 200;                   /* a recorded squad index past the table */
    own(41, M_TANK, 26, 30);
    for (k = 0; k < 4; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40, IMPAI_LOS, 1);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_EASY], 1, &o) == 0, "the squads decision runs");
    mil_print("squad, prediction and order bounds (Easy)", &o, from);
    CHECK(o.left_out == 2 && s_job[41].kind != IMPAI_JOB_SQUAD, "no squad left for two; the index past the table voided");
    CHECK(o.predictions == IMPAI_LEVELS[IMPAI_LV_EASY].predict_max, "predictions %u of Easy's %u", o.predictions,
          IMPAI_LEVELS[IMPAI_LV_EASY].predict_max);
    CHECK(o.roots == IMPAI_LEVELS[IMPAI_LV_EASY].mil_roots, "orders %u of Easy's %u", o.roots,
          IMPAI_LEVELS[IMPAI_LV_EASY].mil_roots);
}

/* the military's tick and the economy's never coincide, for any seat at any level */
static void check_mil_due(void)
{
    unsigned lv, seat, t, both = 0, mil = 0;
    for (lv = 0; lv < IMPAI_LVS; lv++)
        for (seat = 0; seat < WQ_SEATS; seat++)
            for (t = 0; t < 1800; t++) {
                const int m = impai_mil_due(t, seat, &IMPAI_LEVELS[lv]);
                mil += (unsigned)m;
                both += m && impai_brain_due(t, seat, &IMPAI_LEVELS[lv]);
            }
    CHECK(!both, "%u ticks share a military and an economy decision", both);
    printf("mil due: %u military decisions over 1800 ticks, ten seats, three levels\n", mil);
}

/* THE ARMY'S PRODUCTION (impai_econ.c, factories_army): a finished factory offering the raider, the tank and the
   scout, fed metal, decides twenty times with the military's default table over an empty picture; each idle
   decision queues one unit of the class furthest below its share, so the value bought follows the shares. The
   shares of A3's hand table over the land classes are printed beside the spec's hand composition. */
static float rate_zero(const void* ctx, uint16_t i, uint16_t j)
{
    (void)ctx; (void)i; (void)j;
    return 0.0f;
}

static void check_production(void)
{
    static const uint16_t ARMY[] = { M_RAIDER, M_TANK, M_SCOUT };
    const IMPAI_PREDICTOR pred = { rate_zero, impai_expf, NULL };
    IMPAI_BRAIN_IN in;
    IMPAI_BRAIN_OUT bo;
    WORLDQ q = { &FAKE, &s_w, 0 };
    float s[IMPAI_KS];
    int n[M_N], k, from, i;
    impai_table_shares(&IMPAI_TABLE_HAND, &s_mil.pic, (1u << IMPAI_K_LRAIDER) | (1u << IMPAI_K_LASSAULT) |
                                                       (1u << IMPAI_K_LARTY) | (1u << IMPAI_K_LSCOUT), s);
    printf("hand table, empty picture, land: raider %.3f assault %.3f artillery %.3f scout %.3f anti-air %.3f "
           "(spec: 0.25 0.45 0.15 0.05)\n", (double)s[IMPAI_K_LRAIDER], (double)s[IMPAI_K_LASSAULT],
           (double)s[IMPAI_K_LARTY], (double)s[IMPAI_K_LSCOUT], (double)s[IMPAI_K_LAA]);
    mil_world();
    memset(s_mil.pic.f, 0, sizeof s_mil.pic.f);
    menu_set(&s_w, T_FACTORY, ARMY, 3);
    own(1, T_COM, 20, 20);
    unit_add(&s_w, 2, T_FACTORY, 30, 30, 1);
    s_w.econ[0].metal_in = 3.0f;
    memset(n, 0, sizeof n);
    s_milp = &s_mil;
    for (i = 0; i < 20; i++) {
        memset(&in, 0, sizeof in);
        in.q = &q;
        in.st = &s_st;
        in.job = s_job; in.njob = FW_SLOTS;
        in.ti = s_ti; in.nti = M_N;
        in.spots = s_spots; in.nspots = s_nspots;
        in.reach = &s_reach;
        in.lv = &IMPAI_LEVELS[IMPAI_LV_HARD];
        in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
        in.snap = &s_snap;
        in.mil = &s_mil;
        in.pred = &pred;
        from = s_w.nlog;
        CHECK(impai_brain_decide(&in, &bo) == 0, "a production decision runs");
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.unit == 2 && s_w.log[k].c.verb == WQ_CMD_QUEUE && s_w.log[k].c.type < M_N) {
                n[s_w.log[k].c.type]++;
                /* the factory finishes it at once: a unit of the type in a free slot, its queue empty */
                own(100 + i, s_w.log[k].c.type, 40 + i, 40);
                s_w.unit[2].n = 0;
            }
        s_w.gametime += 15;
    }
    s_milp = NULL;
    printf("production, 20 decisions: raider %d tank %d scout %d\n", n[M_RAIDER], n[M_TANK], n[M_SCOUT]);
    CHECK(n[M_RAIDER] + n[M_TANK] + n[M_SCOUT] >= 15, "the factory builds army");
    CHECK(n[M_TANK] * s_ti[M_TANK].value > n[M_RAIDER] * s_ti[M_RAIDER].value &&
              n[M_RAIDER] * s_ti[M_RAIDER].value > n[M_SCOUT] * s_ti[M_SCOUT].value,
          "the value bought follows the shares: assault, raider, scout");
}

/* THE D-GUN (impai_mil.c): the commander, its D-gun 140 px and 400 energy a shot, fires at a tank whose block
   centre is 104 px off (128 with the block's half diagonal), when the tank is seen now, stored energy covers
   the shot and Hard's reserve (0.25 of the storage), and the tank is worth the shot (400 / 60 metal); of two
   targets it takes the more valuable, and it does not repeat an order its head already holds. Each refusal on its own: a ghost, a radar contact never identified, energy
   short of the reserve, a target worth less than the shot, a target out of range, a def that cannot take the
   order, and the commander's safety window. */
static int dgun_case(const char* name, int expect, void (*setup)(void))
{
    IMPAI_MIL_OUT o;
    int from, n;
    mil_world();
    own(1, T_COM, 20, 20);
    s_w.defs[T_COM].dgun = 1;
    s_ti[T_COM].dgun_range = 140;
    s_ti[T_COM].dgun_e = 400;
    s_w.econ[0].energy = 1000;
    s_w.econ[0].energy_max = 1000;
    enemy(1300, M_TANK, 20, 26, IMPAI_LOS, 1);
    if (setup) setup();
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the D-gun decision runs: %s", name);
    n = count_action(from, A_DGUN, 1);
    printf("dgun %s: %d order%s%s", name, n, n == 1 ? "" : "s", n ? "," : "\n");
    if (n) printf(" target %u\n", s_w.log[s_w.nlog - 1].c.target);
    CHECK(n == expect && o.dgun == (uint8_t)expect, "D-gun %s: %d orders, %d wanted", name, n, expect);
    return n;
}

static void dgun_two(void) { enemy(1301, M_RAIDER, 22, 26, IMPAI_LOS, 1); }
static void dgun_ghost(void) { s_rows[1300].seat[0].how = IMPAI_GHOST; }
static void dgun_gone(void) { dgun_two(); s_w.unit[1300].live = 0; }        /* the tank died since its row */
static void dgun_radar(void)
{
    s_rows[1300].seat[0].how = IMPAI_RADAR;
    s_rows[1300].seat[0].type = (uint16_t)WQ_NONE;
    s_rows[1300].seat[0].owner = 0xFF;
}
static void dgun_poor(void) { s_w.econ[0].energy = 640; }          /* 400 + 0.25 x 1000 = 650 */
static void dgun_cheap(void) { s_ti[M_TANK].value = 6.0f; }        /* 400 / 60 = 6.67 */
static void dgun_far(void) { enemy(1300, M_TANK, 20, 30, IMPAI_LOS, 1); }   /* 168 px off, 192 with the half diagonal */
static void dgun_none(void) { s_ti[T_COM].dgun_range = 0; }
static void dgun_window(void) { s_mil.safe_until = (uint32_t)s_w.gametime + 100; }

static void check_mil_dgun(void)
{
    IMPAI_MIL_OUT o;
    int from;
    dgun_case("fires at a seen tank", 1, NULL);
    CHECK(s_w.log[s_w.nlog - 1].c.target == 1300, "the D-gun's target is the tank");
    from = s_w.nlog;
    s_w.gametime += IMPAI_LEVELS[IMPAI_LV_HARD].mil_period;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0 && count_action(from, A_DGUN, 1) == 0,
          "a D-gun order the head already holds is not given again");
    dgun_case("fires at the more valuable of two", 1, dgun_two);
    CHECK(s_w.log[s_w.nlog - 1].c.target == 1300, "the tank (300) over the raider (120)");
    dgun_case("fires at the raider when the tank is gone", 1, dgun_gone);
    CHECK(s_w.log[s_w.nlog - 1].c.target == 1301, "the raider, not the slot gone");
    dgun_case("refuses a ghost", 0, dgun_ghost);
    dgun_case("refuses a radar contact never identified", 0, dgun_radar);
    dgun_case("refuses below the energy reserve", 0, dgun_poor);
    dgun_case("refuses a target worth less than the shot", 0, dgun_cheap);
    dgun_case("refuses a target out of range", 0, dgun_far);
    dgun_case("refuses a commander that cannot take the order", 0, dgun_none);
    dgun_case("refuses inside the commander's safety window", 0, dgun_window);
}

/* ---- the naval rule: the judgement (impai_mil.c, naval), the sea plant (impai_build.c, THE SEA PLANT) and
   keeping to the layer ----

   The reach labels of classes 0..2 over the fake's map by its heights (pass_at), with the block tops and the
   summary over the case's types: what tagpu_impai.c builds at a level init from the engine's maps. */
static uint16_t s_flabels[3 * (512 / 2) * (256 / 2)];
static uint8_t  s_ftop[(512 / 2) * (256 / 2)];
static uint32_t s_fwork[512 * 256 * 2];
static IMPAI_REACH s_mreach;

static int fpass(const void* ctx, int c, int cx, int cz) { return pass_at((const FWORLD*)ctx, c, cx, cz); }

/* a cell's top as the engine's +0x05: the most of the heights at the cell and its right, lower and lower-right
   neighbours, 255 on the last column or row (never written there, engine/map.md) */
static int f_top(const void* ctx, int cx, int cz)
{
    const FWORLD* w = (const FWORLD*)ctx;
    int k, v, most = 0;
    if (cx >= map_w(w) - 1 || cz >= map_h(w) - 1) return 255;
    for (k = 0; k < 4; k++)
        if ((v = cell_h(w, cx + k % 2, cz + k / 2)) > most) most = v;
    return most;
}

static void fake_reach(IMPAI_REACH* r, int ntypes)
{
    const int w = map_w(&s_w), h = map_h(&s_w);
    const size_t blocks = (size_t)(w / 2) * (size_t)(h / 2);
    int c;
    memset(r, 0, sizeof *r);
    for (c = 0; c < 3; c++) {
        r->at[c] = (uint32_t)((size_t)c * blocks);
        impai_reach_label(fpass, &s_w, c, w, h, s_flabels + (size_t)c * blocks, s_fwork);
    }
    impai_reach_top(f_top, &s_w, w, h, s_ftop);
    r->labels = s_flabels;
    r->present = 7;
    r->bw = (uint16_t)(w / 2);
    r->bh = (uint16_t)(h / 2);
    r->top = s_ftop;
    impai_reach_summary(r, s_ti, (unsigned)ntypes);
}

/* the military's worlds on its 512 x 256 map: DRY as before; STRAIT, a channel 40 deep over x 240..271, the full
   height (6 % water); SEA, all 40 deep but islands W (16..111 x 64..191) and E (400..495 x 64..191) (81 %); WALL,
   dry, a cliff 120 high over x 240..271, the full height, so class 0 has two components and no water */
enum { W_DRY, W_STRAIT, W_SEA, W_WALL };
static void water_world(int kind)
{
    s_w.nwater = 0;
    s_w.sea = kind == W_DRY || kind == W_WALL ? 0 : 50;
    if (kind == W_STRAIT) water_add(&s_w, 240, 0, 271, 255, 40);
    if (kind == W_WALL) water_add(&s_w, 240, 0, 271, 255, -120);
    if (kind == W_SEA) {
        water_add(&s_w, 0, 0, 511, 255, 40);
        water_add(&s_w, 16, 64, 111, 191, -10);
        water_add(&s_w, 400, 64, 495, 191, -10);
    }
}

/* the destroyer and the submarine as the sea army, travelling by class 1 */
static void mil_sea_types(void)
{
    s_ti[M_DEST].klass = IMPAI_K_SASSAULT;
    s_ti[M_SUB].klass = IMPAI_K_SRAIDER;
    s_ti[M_DEST].mclass = s_ti[M_SUB].mclass = 1;
    s_w.defs[M_DEST].d.mclass = s_w.defs[M_SUB].d.mclass = 1;
}

/* the military's case on world `kind`, its labels given to the decision */
static void mil_water(int kind)
{
    water_world(kind);
    mil_sea_types();
    fake_reach(&s_mreach, M_N);
    s_mil_reach = &s_mreach;
}

/* THE REACH PRIMITIVES: the summary's thresholds and figures, a block top, the ring order, the nearest block of
   a component and the estimate's labels */
static void check_reach_summary(void)
{
    static const int8_t RING[] = {
        0, -1, -1, 0, 1, 0, 0, 1, -1, -1, 1, -1, -1, 1, 1, 1,
        0, -2, -2, 0, 2, 0, 0, 2, -1, -2, 1, -2, -2, -1, 2, -1, -2, 1, 2, 1, -1, 2, 1, 2, -2, -2, 2, -2, -2, 2, 2, 2 };
    IMPAI_REACH r;
    uint16_t lab[8 * 8], out[8];
    int i, k, dx, dz, ox = -1, oz = -1, n, ok;
    ok = impai_ring_at(0, 0, &dx, &dz) && dx == 0 && dz == 0 && !impai_ring_at(0, 1, &dx, &dz);
    for (k = 0, i = 0; i < 8; i++, k += 2) ok &= impai_ring_at(1, i, &dx, &dz) && dx == RING[k] && dz == RING[k + 1];
    ok &= !impai_ring_at(1, 8, &dx, &dz);
    for (i = 0; i < 16; i++, k += 2) ok &= impai_ring_at(2, i, &dx, &dz) && dx == RING[k] && dz == RING[k + 1];
    ok &= !impai_ring_at(2, 16, &dx, &dz);
    CHECK(ok, "the ring order of rings 0, 1 and 2");

    /* HALF, 128 x 128: water over columns 0..63 is exactly half the blocks, not mostly; over 0..65 it is */
    mil_world();
    mil_sea_types();
    s_w.mw = s_w.mh = 128;
    s_w.sea = 50;
    water_add(&s_w, 0, 0, 63, 127, 40);
    fake_reach(&r, M_N);
    printf("reach HALF 64: sea_blocks=%u of %u mostly=%d\n", r.sea_blocks, (unsigned)r.bw * r.bh, impai_mostly_water(&r));
    CHECK(r.sea_blocks == 32 * 64 && !impai_mostly_water(&r), "half the blocks is not mostly water: %u", r.sea_blocks);
    s_w.nwater = 0;
    water_add(&s_w, 0, 0, 65, 127, 40);
    fake_reach(&r, M_N);
    printf("reach HALF 66: sea_blocks=%u of %u mostly=%d\n", r.sea_blocks, (unsigned)r.bw * r.bh, impai_mostly_water(&r));
    CHECK(r.sea_blocks == 33 * 64 && impai_mostly_water(&r), "past half is mostly water: %u", r.sea_blocks);

    /* SEA and DRY on the military's map, with its types: four land army types on class 0, two sea ones on 1 */
    mil_world();
    mil_water(W_SEA);
    r = s_mreach;
    printf("reach SEA: sea_blocks=%u of %u mostly=%d land_total=%u land_w=%u,%u,%u sea_ref=%u top=%u,%u,%u\n",
           r.sea_blocks, (unsigned)r.bw * r.bh, impai_mostly_water(&r), r.land_total, r.land_w[0], r.land_w[1],
           r.land_w[2], r.sea_ref, r.top[32 * 256 + 6], r.top[32 * 256 + 7], r.top[32 * 256 + 255]);
    CHECK(impai_mostly_water(&r) && r.land_total == 4 && r.land_w[0] == 4 && !r.land_w[1] && !r.land_w[2] &&
          r.sea_ref == 1, "SEA's summary");
    /* block (7, 32): cells 14..15 x 64..65, whose corners reach the island's column 16, 10 above the sea */
    CHECK(r.top[32 * 256 + 6] == 10 && r.top[32 * 256 + 7] == 60 && r.top[32 * 256 + 255] == 255,
          "the block tops: the water's 10, the island's edge 60, the last column 255");
    mil_world();
    mil_water(W_DRY);
    CHECK(!s_mreach.sea_blocks && !impai_mostly_water(&s_mreach), "DRY has no sea block: %u", s_mreach.sea_blocks);

    /* the nearest block of a component on an 8 x 8 grid of label 1 */
    memset(&r, 0, sizeof r);
    for (i = 0; i < 64; i++) lab[i] = 1;
    lab[1 * 8 + 3] = lab[3 * 8 + 5] = 2;          /* ring 2 of (3, 3): (0, -2) first, (2, 0) third */
    r.labels = lab;
    r.present = 1;
    r.bw = r.bh = 8;
    CHECK(impai_reach_near(&r, 0, 2, 3, 3, 2, &ox, &oz) && ox == 3 && oz == 1, "ring order breaks a tie: %d,%d", ox, oz);
    CHECK(!impai_reach_near(&r, 0, 2, 3, 3, 1, &ox, &oz), "none past the radius");
    lab[4 * 8 + 4] = 2;                           /* ring 1's corner */
    CHECK(impai_reach_near(&r, 0, 2, 3, 3, 2, &ox, &oz) && ox == 4 && oz == 4, "the nearer ring first: %d,%d", ox, oz);
    CHECK(impai_reach_near(&r, 0, 2, 0, 0, 3, &ox, &oz) && ox == 3 && oz == 1, "off-grid blocks skipped: %d,%d", ox, oz);
    CHECK(impai_reach_at(&r, 0, 8, 0) == 0 && impai_reach_at(&r, 1, 0, 0) == -1, "off the grid 0, no labels -1");
    /* the estimate's labels: every block its own label, cap 3 */
    for (i = 0; i < 64; i++) lab[i] = (uint16_t)(1 + i);
    r.sea_ref = 0;
    n = impai_sea_labels(&r, 4, 4, 3, out, 3);
    printf("reach near: tie %d,%d; labels %d: %u %u %u\n", ox, oz, n, out[0], out[1], out[2]);
    CHECK(n == 3 && out[0] == 37 && out[1] == 29 && out[2] == 36, "the estimate's labels, nearest first, cap 3");
    r.sea_ref = 0xFF;
    CHECK(!impai_sea_labels(&r, 4, 4, 3, out, 3), "no sea class, no label");
}

/* one naval judgement of the military at (20, 20) with a start there and one at (sx, sz) px */
static int naval_of(const char* name, int sx, int sz)
{
    IMPAI_MIL_OUT o;
    s_w.starts[0].x = s_w.starts[0].z = 328;
    s_w.starts[0].index = 0;
    s_w.starts[1].x = sx;
    s_w.starts[1].z = sz;
    s_w.starts[1].index = 1;
    s_w.nstarts = 2;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the naval decision runs: %s", name);
    printf("naval %s: %d (sea_blocks=%u land_total=%u estimate=%d,%d)\n", name, s_mil.naval,
           s_mil_reach ? s_mil_reach->sea_blocks : 0u, s_mil_reach ? s_mil_reach->land_total : 0u, s_mil.ebx, s_mil.ebz);
    return s_mil.naval;
}

/* THE JUDGEMENT: a water map and a land map for each branch */
static void check_naval(void)
{
    int k;
    mil_world(); mil_water(W_DRY);
    CHECK(naval_of("DRY", 6000, 328) == 0, "a dry map is never naval");
    mil_world(); mil_water(W_SEA);
    CHECK(naval_of("SEA", 6000, 328) == 1, "a map mostly water is naval before any structure is seen");
    mil_world(); mil_water(W_STRAIT);
    CHECK(naval_of("STRAIT, the other start across", 6000, 328) == 1, "an estimate across the strait is naval");
    mil_world(); mil_water(W_STRAIT);
    CHECK(naval_of("STRAIT, the other start on this side", 3000, 328) == 0, "an estimate on this side is not");
    /* the structures' value, not their centroid: three across and two on this side, then two and three */
    for (k = 0; k < 2; k++) {
        int i;
        mil_world(); mil_water(W_STRAIT);
        for (i = 0; i < 5; i++) enemy(1300 + i, T_MEX, i < 3 - k ? 400 : 100, 100, IMPAI_GHOST, 0);
        CHECK(naval_of(k ? "STRAIT, two structures across, three near" : "STRAIT, three structures across, two near",
                       3000, 328) == !k, "less than half the structures' value on land-linked blocks is naval");
    }
    /* the land army's classes: three of four types amphibious link the banks, one of four does not */
    mil_world();
    s_ti[M_RAIDER].mclass = s_ti[M_TANK].mclass = s_ti[M_SCOUT].mclass = 2;
    mil_water(W_STRAIT);
    CHECK(naval_of("STRAIT, three of four land types amphibious", 6000, 328) == 0, "amphibious majority links");
    mil_world();
    s_ti[M_RAIDER].mclass = 2;
    mil_water(W_STRAIT);
    CHECK(naval_of("STRAIT, one of four land types amphibious", 6000, 328) == 1, "amphibious minority does not");
    mil_world();
    s_ti[M_RAIDER].klass = s_ti[M_TANK].klass = s_ti[M_SCOUT].klass = s_ti[M_AA].klass = IMPAI_K_NONE;
    mil_water(W_STRAIT);
    CHECK(naval_of("STRAIT, no land army type", 6000, 328) == 0, "no land army type is no evidence");
    mil_world();
    CHECK(naval_of("no reach labels", 6000, 328) == 0, "no labels judge 0");
}

/* THE SEA PLANT on the economy's types: land over x 0..63, then COAST sea 40 deep, SHALLOW 20 deep, or LAKE, a
   lake over 64..79 cut from the sea by land past the commander's reach (sea from 128); the commander idle at cell
   (40, 64), a finished K-bot lab, stores full, the military's estimate at block (240, 32) */
enum { C_COAST, C_SHALLOW, C_LAKE };
static IMPAI_MIL s_smil;

static void coast_world(int kind, int naval, float metal_in)
{
    egame_world(0);
    s_spots = NULL;
    s_nspots = 0;
    s_w.mw = 512;
    s_w.mh = 128;
    s_w.sea = 50;
    water_add(&s_w, 0, 0, 63, 127, -10);
    if (kind == C_LAKE) {
        water_add(&s_w, 64, 0, 79, 127, 40);
        water_add(&s_w, 80, 0, 127, 127, -10);
        water_add(&s_w, 128, 0, 511, 127, 40);
    } else
        water_add(&s_w, 64, 0, 511, 127, kind == C_SHALLOW ? 20 : 40);
    unit_add(&s_w, 1, E_COM, 40, 64, 1);
    unit_add(&s_w, 2, E_LAB, 20, 40, 1);
    s_w.econ[0].metal = s_w.econ[0].energy = s_w.econ[0].metal_max = s_w.econ[0].energy_max = 1000;
    s_w.econ[0].metal_in = metal_in;
    s_w.econ[0].energy_in = 100;
    s_w.econ[0].energy_out = 50;
    fake_reach(&s_reach, E_N);
    impai_mil_reset(&s_smil, IMPAI_LV_HARD, 0x2545F4914F6CDD1Dull, &IMPAI_TABLE_DEFAULT, 0);
    s_smil.naval = (int8_t)naval;
    s_smil.ebx = 240;
    s_smil.ebz = 32;
    s_milp = &s_smil;
    s_w.gametime = 2000;
}

/* the NEW build orders of `type` since `from`, the last at *at (its first cell) */
static int builds_of(int from, int type, int* x0, int* z0)
{
    int k, n = 0;
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.verb == WQ_CMD_NEW && s_w.log[k].c.type == type) {
            n++;
            if (x0) *x0 = cmd_x0(&s_w.log[k].c);
            if (z0) *z0 = cmd_z0(&s_w.log[k].c);
        }
    return n;
}

/* one decision; its sea plants and land plants ordered */
static void sea_decide(const char* name, int* sy, int* land)
{
    IMPAI_BRAIN_OUT out;
    const int from = s_w.nlog;
    int x0 = -1, z0 = -1, lx = -1, lz = -1;
    CHECK(decide(&out) == 0, "decide: %s", name);
    *sy = builds_of(from, E_SY, &x0, &z0);
    *land = builds_of(from, E_LAB, &lx, &lz) + builds_of(from, E_VP, &lx, &lz);
    printf("sea plant %s: sy=%d at %d,%d land=%d at %d,%d sea_plants=%u sites=%u sea_refused=%u\n", name, *sy, x0, z0,
           *land, lx, lz, out.sea_plants, out.sites, s_st.sea_refused);
    CHECK(out.sites <= s_lv->site_tests, "%s: %u site tests of %u", name, out.sites, s_lv->site_tests);
}

static void check_sea_plant(void)
{
    const uint32_t fail_t = IMPAI_LEVELS[IMPAI_LV_HARD].fail_t;
    int sy, land, x0 = -1, z0 = -1, x, z, deep = 1, k;
    coast_world(C_COAST, 1, 9);
    {
        const int from = s_w.nlog;
        IMPAI_BRAIN_OUT out;
        CHECK(decide(&out) == 0, "decide: COAST");
        sy = builds_of(from, E_SY, &x0, &z0);
        for (z = z0; z <= z0 + 8; z++)
            for (x = x0; x <= x0 + 8; x++) deep &= cell_h(&s_w, x, z) <= s_w.sea - 30;
        printf("sea plant COAST: sy=%d at %d,%d sea_plants=%u deep=%d\n", sy, x0, z0, out.sea_plants, deep);
        /* ring 14 of the anchor's block (20, 32), k 0, (14, 0): centre block (34, 32), origin block (32, 30) */
        CHECK(sy == 1 && x0 == 64 && z0 == 60 && deep && out.sea_plants == 1, "the shipyard at (64, 60)");
    }
    coast_world(C_COAST, 1, 3);
    sea_decide("COAST, low income", &sy, &land);
    CHECK(!sy && !land, "the sea plant waits for the income a second factory needs");
    coast_world(C_COAST, 0, 3);
    sea_decide("COAST, not naval", &sy, &land);
    CHECK(!sy && !land, "not naval: no shipyard, and no factory at this income");
    coast_world(C_COAST, 0, 9);
    sea_decide("COAST, not naval, income", &sy, &land);
    CHECK(!sy && land == 1, "not naval: the second factory a land one");
    coast_world(C_COAST, -1, 9);
    sea_decide("COAST, not judged", &sy, &land);
    CHECK(!sy && !land, "the second factory waits for the judgement");
    coast_world(C_SHALLOW, 1, 9);
    sea_decide("SHALLOW", &sy, &land);
    CHECK(!sy && land == 1, "no water deep enough: the land factory in the same decision");
    CHECK(s_st.sea_none_type == E_SY && s_st.sea_none_ex == 240 && s_st.sea_none_ez == 32,
          "no candidate is remembered against the type, the anchor and the estimate");
    coast_world(C_SHALLOW, 1, 3);
    sea_decide("SHALLOW, low income", &sy, &land);
    CHECK(!sy && !land, "no water deep enough, and no income for a land factory");
    coast_world(C_LAKE, 1, 9);
    sea_decide("LAKE", &sy, &land);
    CHECK(!sy && land == 1, "a lake that is not the estimate's water: the land factory");
    coast_world(C_COAST, 1, 9);
    {
        static const uint16_t DRY[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP, E_UWMEX };
        menu_set(&s_w, E_COM, DRY, (int)(sizeof DRY / sizeof DRY[0]));
    }
    sea_decide("no shipyard on any menu", &sy, &land);
    CHECK(!sy && land == 1, "no builder offers a sea plant: the land factory");
    coast_world(C_COAST, 1, 9);
    unit_add(&s_w, 3, E_SY, 64, 60, 0);
    sea_decide("a shipyard frame", &sy, &land);
    CHECK(!sy && !land, "a shipyard standing as a frame: no order");
    s_w.unit[3].live = 0;
    s_w.gametime += 15;
    sea_decide("the frame gone", &sy, &land);
    CHECK(sy == 1, "the frame gone: the shipyard ordered again");
    /* every site refused, on Hard's row: a decision's site_tests (32) refusals count one, a decision within
       fail_t of it none, and each fail_t one more; at the sixteenth the sea plant is given up, and the next
       decision orders the land factory (the sixteenth spent the decision's site tests) */
    coast_world(C_COAST, 1, 9);
    s_w.refuse_type = E_SY;
    sea_decide("refused, first", &sy, &land);
    CHECK(!sy && !land && s_st.sea_refused == 1, "one refusal a decision: %u", s_st.sea_refused);
    s_w.gametime += 15;
    sea_decide("refused, within fail_t", &sy, &land);
    CHECK(!sy && !land && s_st.sea_refused == 1, "none more within fail_t: %u", s_st.sea_refused);
    for (k = 2; k <= 16; k++) {
        s_w.gametime += (int)fail_t;
        {
            IMPAI_BRAIN_OUT out;
            const int from = s_w.nlog;
            CHECK(decide(&out) == 0, "decide: refused, window %d", k);
            sy = builds_of(from, E_SY, NULL, NULL);
            land = builds_of(from, E_LAB, NULL, NULL) + builds_of(from, E_VP, NULL, NULL);
        }
        CHECK(!sy && !land && s_st.sea_refused == k, "window %d: %u refusals, land %d", k, s_st.sea_refused, land);
    }
    s_w.gametime += 15;
    sea_decide("refused sixteen times, the next decision", &sy, &land);
    CHECK(!sy && land == 1, "the sea plant given up: the land factory");
    /* unmapped: the water unexplored, no site is tested and the decision counts one refusal; with the near water
       explored, the shipyard there */
    coast_world(C_COAST, 1, 9);
    for (z = 0; z < FW_H; z++) for (x = 64; x < FW_W; x++) s_w.explored[z][x] = 0;
    sea_decide("unmapped", &sy, &land);
    CHECK(!sy && !land && s_st.sea_refused == 1, "unexplored water: no site test, one refusal");
    for (z = 0; z < FW_H; z++) for (x = 64; x < 80; x++) s_w.explored[z][x] = 1;
    s_w.gametime += 15;
    sea_decide("unmapped, the near water explored", &sy, &land);
    CHECK(sy == 1, "the shipyard on the explored water");
    /* the nearest water held by standing underwater extractors, more candidates than SEA_LOOK: the shipyard
       beyond them */
    coast_world(C_COAST, 1, 9);
    s_w.last[0] = 63;
    for (k = 0; k < 48; k++) unit_add(&s_w, 10 + k, E_UWMEX, 66 + 12 * (k % 3), 2 + 8 * (k / 3), 1);
    {
        const int from = s_w.nlog;
        IMPAI_BRAIN_OUT out;
        CHECK(decide(&out) == 0, "decide: claimed");
        sy = builds_of(from, E_SY, &x0, &z0);
        printf("sea plant beyond 48 standing extractors: sy=%d at %d,%d sites=%u sea_refused=%u\n", sy, x0, z0,
               out.sites, s_st.sea_refused);
        CHECK(sy == 1 && x0 >= 95, "the claims cost no SEA_LOOK: the shipyard beyond them");
    }
    /* a failed shipyard job counts */
    coast_world(C_COAST, 0, 3);
    s_job[1].type = E_COM;
    s_job[1].build = E_SY;
    s_job[1].target = (uint16_t)WQ_NONE;
    s_job[1].cx = 64;
    s_job[1].cz = 60;
    s_job[1].kind = IMPAI_JOB_BUILD;
    s_job[1].since = 1900;
    sea_decide("a failed shipyard job", &sy, &land);
    CHECK(s_st.sea_refused == 1, "a failed shipyard job is a refusal: %u", s_st.sea_refused);
    s_milp = NULL;
}

/* an army unit of seat 0 in squad s */
static void member(int slot, int type, int cx, int cz, int s)
{
    own(slot, type, cx, cz);
    s_job[slot].kind = IMPAI_JOB_SQUAD;
    s_job[slot].type = (uint16_t)type;
    s_job[slot].squad = (uint8_t)s;
}

static void squad_task(int s, unsigned task_, unsigned layer, int tbx, int tbz)
{
    memset(&s_mil.sq[s], 0, sizeof s_mil.sq[s]);
    s_mil.sq[s].task = (uint8_t)task_;
    s_mil.sq[s].layer = (uint8_t)layer;
    s_mil.sq[s].target = (uint16_t)WQ_NONE;
    s_mil.sq[s].tx = (int16_t)tbx;
    s_mil.sq[s].tz = (int16_t)tbz;
    s_mil.sq[s].since = (uint32_t)s_w.gametime;
}

/* the moves since `from`: their count, and whether every one lands at cells x0..x1 within SUB_BLOCKS (32) blocks of
   block (tbx, tbz) */
static int moves_in(int from, int x0, int x1, int tbx, int tbz, int* all)
{
    int k, n = 0;
    *all = 1;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        const int cx = c->pos[0] >> 20, bx = c->pos[0] >> 21, bz = c->pos[2] >> 21;
        if (c->action != A_MOVE) continue;
        n++;
        if (cx < x0 || cx > x1 || abs(bx - tbx) > 32 || abs(bz - tbz) > 32) *all = 0;
    }
    return n;
}

/* KEEPING TO THE LAYER: the military's moves, waves, raids, scouts and predictions, and the explorer */
static void check_layers(void)
{
    IMPAI_MIL_OUT o;
    IMPAI_BRAIN_OUT bo;
    int k, from, n, all, far;
    /* an ATTACK squad of four land raiders west of the strait, its point east: each moves to the west bank */
    for (far = 0; far <= 1; far++) {
        const int tbx = far ? 159 : 140;
        mil_world();
        mil_water(W_STRAIT);
        squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, tbx, 50);
        for (k = 0; k < 4; k++) member(1 + k, M_RAIDER, 200 + 2 * k, 100, 0);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the layer decision runs");
        n = moves_in(from, 0, 239, tbx, 50, &all);
        printf("layer attack across, point at block %d: moves=%d unreached=%u\n", tbx, n, o.unreached);
        if (far) CHECK(!n && o.unreached == 4, "no west-bank block within SUB_BLOCKS: no move, four unreached");
        else CHECK(n == 4 && all && !o.unreached, "every move on the west bank, near the point");
    }
    /* a land FORM squad worth a wave with the estimate across: no wave; on the dry map the wave goes */
    for (k = 0; k < 2; k++) {
        int i;
        mil_world();
        mil_water(k ? W_DRY : W_STRAIT);
        for (i = 0; i < 12; i++) own(1 + i, M_TANK, 24 + 2 * (i % 6), 24 + 2 * (i / 6));
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the wave decision runs");
        printf("layer wave, %s: attacks=%u raids=%u\n", k ? "DRY" : "STRAIT", o.attacks, o.raids);
        CHECK(o.attacks == (uint8_t)k && !o.raids, "a land wave goes only where it reaches: %u", o.attacks);
    }
    /* a sea FORM squad in the channel, its rally point on land: it moves in the channel */
    mil_world();
    s_st.ax = 220;
    s_st.az = 40;
    mil_water(W_STRAIT);
    own(1, M_DEST, 250, 40);
    own(2, M_DEST, 256, 40);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the sea squad's decision runs");
    n = moves_in(from, 240, 271, 110, 20, &all);
    printf("layer sea squad, rally on land: moves=%d in the channel=%d unreached=%u\n", n, all, o.unreached);
    CHECK(n == 2 && all, "the sea squad keeps to the channel");
    /* a land scout on island W, every other start on E: no scout; on the dry map it goes */
    for (k = 0; k < 2; k++) {
        static const WQ_START STARTS[] = { { 1000, 2000, 0 }, { 7000, 2000, 1 }, { 7000, 1500, 2 } };
        mil_world();
        s_st.ax = 62;
        s_st.az = 125;
        memcpy(s_w.starts, STARTS, sizeof STARTS);
        s_w.nstarts = 3;
        mil_water(k ? W_DRY : W_SEA);
        own(7, M_SCOUT, 60, 120);
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the scout decision runs");
        printf("layer scout, %s: scouts=%u\n", k ? "DRY" : "SEA", o.scouts);
        CHECK(o.scouts == (uint8_t)k, "a scout goes only to a start it reaches");
    }
    /* a raid on an extractor across the strait does not go; on this side it does */
    for (k = 0; k < 2; k++) {
        mil_world();
        mil_water(W_STRAIT);
        own(1, M_TANK, 24, 24);
        own(4, M_RAIDER, 24, 28);
        own(5, M_RAIDER, 26, 28);
        own(6, M_RAIDER, 28, 28);
        enemy(1300, T_MEX, k ? 150 : 400, 200, IMPAI_GHOST, 1);
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the raid decision runs");
        printf("layer raid, extractor %s: raids=%u\n", k ? "near" : "across", o.raids);
        CHECK(o.raids == (uint8_t)k, "a raid goes only where it reaches");
    }
    /* two raiders on the bank against a destroyer mid-channel, beyond every land block's reach of their range: a
       prediction with a side that cannot close; on the dry map both close */
    for (k = 0; k < 2; k++) {
        mil_world();
        mil_water(k ? W_DRY : W_STRAIT);
        own(1, M_RAIDER, 236, 100);
        own(2, M_RAIDER, 238, 100);
        enemy(1300, M_DEST, 256, 100, IMPAI_LOS, 1);
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the cross decision runs");
        printf("layer cross, %s: predictions=%u cross=%u\n", k ? "DRY" : "STRAIT", o.predictions, o.cross);
        CHECK(o.predictions >= 1 && o.cross == (k ? 0 : 1), "cross %u", o.cross);
    }
    /* the substitutes' cache: forty raiders, each on an island of its own one cell wide, one ATTACK squad sent to
       open water within SUB_BLOCKS of every island: SUB_CACHE moves given, the rest unreached */
    mil_world();
    water_world(W_SEA);
    for (k = 0; k < 40; k++) water_add(&s_w, 200 + 4 * (k % 8), 60 + 4 * (k / 8), 200 + 4 * (k % 8), 60 + 4 * (k / 8), -10);
    mil_sea_types();
    fake_reach(&s_mreach, M_N);
    s_mil_reach = &s_mreach;
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 130, 34);
    for (k = 0; k < 40; k++) member(1 + k, M_RAIDER, 200 + 4 * (k % 8), 60 + 4 * (k / 8), 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the cache decision runs");
    n = count_action(from, A_MOVE, -1);
    printf("layer substitutes: moves=%d unreached=%u roots=%u\n", n, o.unreached, o.roots);
    CHECK(n == 32 && o.unreached == 8, "SUB_CACHE substitutes, the rest unreached");
    /* a dry map of two land components: five raiders travel to a point 3 cells west of the cliff; the two whose
       spread points fall on the cliff go to the point itself, not to a substitute */
    mil_world();
    mil_water(W_WALL);
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 118, 50);
    for (k = 0; k < 5; k++) member(1 + k, M_RAIDER, 200 + 2 * k, 100, 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the travel decision runs");
    n = moves_in(from, 0, 239, 118, 50, &all);
    {
        int at = 0;
        for (k = from; k < s_w.nlog; k++) at += s_w.log[k].c.action == A_MOVE && (s_w.log[k].c.pos[0] >> 20) == 237;
        printf("layer travel, WALL: moves=%d west=%d at the point=%d unreached=%u sea_blocks=%u\n", n, all, at,
               o.unreached, s_mreach.sea_blocks);
        CHECK(n == 5 && all && at == 3 && !o.unreached && !s_mreach.sea_blocks,
              "spread points on the cliff fall back to the point");
    }
    /* the same map: a raider (range 200) and a tank (range 300) west of the cliff, a scout 624 px east of it. The
       tank has it within 300 + REACH_PAD and attacks; the raider neither has it within 584 nor can travel to it,
       so it attacks nothing and, beside the squad's centroid, is given nothing. On the dry map both attack. */
    for (k = 0; k < 2; k++) {
        mil_world();
        mil_water(k ? W_DRY : W_WALL);
        own(1, M_RAIDER, 236, 100);
        own(2, M_TANK, 236, 102);
        enemy(1300, M_SCOUT, 275, 101, IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_LEVELS[IMPAI_LV_HARD], 1, &o) == 0, "the reach fight decision runs");
        printf("layer fight across, %s: raider attacks=%d moves=%d tank attacks=%d predictions=%u\n",
               k ? "DRY" : "WALL", count_action(from, A_ATTACK, 1), count_action(from, A_MOVE, 1),
               count_action(from, A_ATTACK, 2), o.predictions);
        CHECK(count_action(from, A_ATTACK, 2) == 1 && count_action(from, A_ATTACK, 1) == k &&
              !count_action(from, A_MOVE, 1), "the raider attacks across only where it can travel");
    }
    /* the explorer: a sea constructor explores water, a land one land; land over x 0..63, explored to x 47 */
    for (k = 0; k < 2; k++) {
        int x, z, cx = -1;
        coast_world(C_COAST, 0, 3);
        s_milp = NULL;
        s_w.unit[1].live = s_w.unit[2].live = 0;
        if (k) unit_add(&s_w, 3, E_CK, 40, 70, 1);
        else unit_add(&s_w, 3, E_CS, 80, 64, 1);
        s_st.ax = 40;
        s_st.az = 64;
        s_w.gametime = 900;
        for (z = 0; z < FW_H; z++) for (x = 48; x < FW_W; x++) s_w.explored[z][x] = 0;
        from = s_w.nlog;
        CHECK(decide(&bo) == 0, "the explorer's decision runs");
        for (x = from; x < s_w.nlog; x++)
            if (s_w.log[x].c.action == A_MOVE && s_w.log[x].c.unit == 3) cx = s_w.log[x].c.pos[0] >> 20;
        printf("layer explore, %s constructor: cell x %d\n", k ? "land" : "sea", cx);
        CHECK(k ? cx == 56 : cx == 72, "the explore cell keeps to the constructor's layer: %d", cx);
    }
    /* the land constructor again, a rock 140 high over cells 52..61 x 52..61 at that cell's centre: it explores
       the cell at a block of its own inside it */
    {
        int x, z, cx = -1, cz = -1;
        coast_world(C_COAST, 0, 3);
        s_milp = NULL;
        s_w.unit[1].live = s_w.unit[2].live = 0;
        unit_add(&s_w, 3, E_CK, 40, 70, 1);
        water_add(&s_w, 52, 52, 61, 61, -140);
        fake_reach(&s_reach, E_N);
        s_st.ax = 40;
        s_st.az = 64;
        s_w.gametime = 900;
        for (z = 0; z < FW_H; z++) for (x = 48; x < FW_W; x++) s_w.explored[z][x] = 0;
        from = s_w.nlog;
        CHECK(decide(&bo) == 0, "the explorer's decision runs");
        for (x = from; x < s_w.nlog; x++)
            if (s_w.log[x].c.action == A_MOVE && s_w.log[x].c.unit == 3) {
                cx = s_w.log[x].c.pos[0] >> 20;
                cz = s_w.log[x].c.pos[2] >> 20;
            }
        printf("layer explore, a rock at the cell's centre: cell %d,%d\n", cx, cz);
        CHECK(cx >= 48 && cx <= 63 && cz >= 48 && cz <= 63 && (cx < 52 || cx > 61 || cz < 52 || cz > 61),
              "the cell is explored at a block of the constructor's beside the rock");
    }
}

static void check_military(void)
{
    if (!(s_mscratch = malloc(impai_mil_scratch_bytes()))) {
        s_fail++;
        return;
    }
    check_mil_due();
    check_mil_defence();
    check_mil_gone_target();
    check_mil_aa_in_ground_fight();
    check_mil_budget();
    check_mil_slot_taken();
    check_mil_radar_aim();
    check_mil_sub_mixed_weapons();
    check_mil_retreat();
    check_mil_attack();
    check_mil_raid_scout();
    check_mil_commander();
    check_mil_bounds();
    check_mil_dgun();
    check_production();
    check_reach_summary();
    check_naval();
    check_layers();
    free(s_mscratch);
}

int main(void)
{
    if (!(s_scratch = malloc(impai_brain_scratch_bytes()))) return 2;
    check_world();
    check_radar_reuse();
    check_tinfo();
    check_due();
    check_stall();
    check_spot();
    check_factory();
    check_caps();
    check_fail();
    check_game();
    check_economy_game();
    check_levels();
    check_sea_plant();
    check_military();
    free(s_scratch);
    printf("impai_brain_test: %s\n", s_fail ? "FAILED" : "ok");
    return s_fail ? 1 : 0;
}

