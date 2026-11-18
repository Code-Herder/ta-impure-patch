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
#include "impai_react.h"

#define FW_W     128                    /* cells of 16 px                                                      */
#define FW_H     128
#define FW_SLOTS 2560                  /* past the military's army and enemy lists, so a case can fill both  */
#define FW_DEFS  48
#define FW_MENU  16
#define FW_LIST  16
#define FW_LOG   256
#define FW_WATER 48                     /* the water rectangles a world holds                                 */
#define FW_FEATS 160                    /* the features a world holds: a full memory's IMPAI_FEATS and more    */
#define FW_COMMANDER 1                  /* the commander type of every world (T_COM, E_COM)                   */

/* the fake's action bytes, in WQ_ACT_* order to the park, then the resolver's answers, then the on and off */
enum { A_MOBILEBUILD = 1, A_VTOL_MOBILEBUILD, A_RECLAIM, A_VTOL_RECLAIM, A_RESURRECT, A_BUILDINGBUILD, A_PARK,
       A_MOVE, A_GUARD, A_HELPBUILD, A_STOP, A_ATTACK, A_PATROL, A_DGUN, A_REPAIRUNIT, A_ACTIVATE, A_DEACTIVATE };

typedef struct FDEF {
    WQ_DEF      d;
    WQ_DEF_ECON e;
    uint16_t    menu[FW_MENU];
    int         nmenu;
    uint32_t    hp;                     /* maxdamage                                                          */
    int         armed;                  /* its units carry +0x110 bit 31, a weapon                            */
    uint8_t     attack;                 /* its IMPAI_TA_* facts: the resolver's attack case reads them        */
    int         dgun;                   /* it answers the D-gun mode (candgun)                                */
    uint8_t     yard[64];               /* its yard map when `yarded`, fx x fz <= 64, else every cell `o`     */
    int         yarded;
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
    float    made_m;                    /* the metal its ledger produced in the last pass's period (+0xE4)   */
    float    req_m, req_e;              /* the metal and energy its ledger asked in the last pass (+0xE8, +0xD0) */
    uint8_t  active;                    /* its activation bit (+0x10E bit 0)                                  */
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
    int      refuse_box_type;           /* a type the site test refuses where its first cell lies in refuse_box  */
    struct { int16_t x0, z0, x1, z1; } refuse_box;
    int      refuse_queue_type;         /* a type every factory QUEUE of is refused, 0 none                   */
    int      yard_refuse;               /* a type whose yard map the world will not give, 0 none              */
    int8_t   cfoot[3][2];               /* the content map's class footprints, folded as the engine's are; 0 a 1 */
    struct { int16_t ax, az, fx, fz; uint16_t def; uint8_t flags, live, playing, gated, owner; float metal, energy; } feat[FW_FEATS];
    int      nfeat;                     /* the features: each covers its footprint from its anchor cell       */
    uint8_t  dark[FW_H / 2][FW_W / 2];  /* 1: an explored block the seat does not see now                     */
    int      dark_reads;                /* feature reads of a cell the seat has not explored                   */
    /* THE DECISION'S READS (check_reads_before_orders): the slots the decision in progress has ordered, a
       decision beginning where it reads GameTime -- impai_brain_decide and impai_mil_decide read it once, at
       their start --; which manager each slot was ordered by (bit IMPAI_SRC_*), `src` the manager deciding */
    uint8_t  ordered[FW_SLOTS], by[FW_SLOTS];
    int      src;
    int      nobirth;                   /* 1: observe gives no incarnation, as a world without the stamp      */
} FWORLD;

/* Over the whole run: the head or order-list reads of a slot the same decision had already ordered, the slots
   other than a commander both managers ordered, and the orders the fake took (check_reads_before_orders) */
static unsigned s_reads_after, s_shared_other, s_orders_taken;

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
    if (!d || type == fw(q)->yard_refuse || d->d.fx * d->d.fz > cap) return 0;
    if (d->yarded && d->d.fx * d->d.fz <= (int)sizeof d->yard) memcpy(out, d->yard, (size_t)(d->d.fx * d->d.fz));
    else memset(out, d->d.kind == WQ_KIND_GEO ? 0x8F : 0x2F, (size_t)(d->d.fx * d->d.fz));
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
   nowhere -- and refuse_type nowhere, nor refuse_box_type where its
   first cell lies in refuse_box */
static int f_site_ok(const WORLDQ* q, int type, int cx, int cz, int ignore, int* alt)
{
    const FWORLD* w = fw(q);
    const FDEF* d = fdef(w, type);
    int s, x, z, hi = -1000, lo = 1000;
    if (!d || type == w->refuse_type || cx < 1 || cz < 1 || cx + d->d.fx >= map_w(w) || cz + d->d.fz >= map_h(w) ||
        !f_explored(q, cx + d->d.fx / 2, cz + d->d.fz / 2))
        return 0;
    if (type == w->refuse_box_type && cx >= w->refuse_box.x0 && cx <= w->refuse_box.x1 && cz >= w->refuse_box.z0 &&
        cz <= w->refuse_box.z1)
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
static int f_block_lit(const WORLDQ* q, int bx, int bz);

/* the live feature whose footprint covers the cell, by its anchor, as the engine's grid answers a footprint tile,
   its anchor's height 0; a read of a cell the seat has not explored is counted, the no-cheat rule's evidence */
static int f_feature(const WORLDQ* q, int cx, int cz, WQ_FEAT* out)
{
    FWORLD* w = fw(q);
    int k;
    memset(out, 0, sizeof *out);
    if (!f_explored(q, cx, cz)) w->dark_reads++;
    for (k = 0; k < w->nfeat; k++)
        if (w->feat[k].live && cx >= w->feat[k].ax && cx < w->feat[k].ax + w->feat[k].fx && cz >= w->feat[k].az &&
            cz < w->feat[k].az + w->feat[k].fz) {
            out->ax = w->feat[k].ax; out->az = w->feat[k].az;
            out->fx = w->feat[k].fx; out->fz = w->feat[k].fz;
            out->def = w->feat[k].def; out->flags = w->feat[k].flags; out->playing = w->feat[k].playing;
            out->gated = w->feat[k].gated; out->owner = w->feat[k].owner;
            out->metal = w->feat[k].metal; out->energy = w->feat[k].energy;
            return 1;
        }
    return 0;
}
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
    out->made_m = u->made_m;
    out->req_m = u->req_m;
    out->req_e = u->req_e;
    out->active = u->active;
    return 1;
}

static int f_orders(const WORLDQ* q, int slot, WQ_ORDER* out, int cap)
{
    const FUNIT* u = fown(q, slot);
    int k;
    if (!u) return -1;
    s_reads_after += fw(q)->ordered[slot];
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

/* the resolver's answers the brain asks: build (14), move (2), guard (7), assist or repair (8), stop (10). Mode 8 is
   the engine's (0x43F46C, engine/orders.md): HELPBUILD for an unfinished target, here when the builder has a menu;
   REPAIRUNIT for a finished one its repair test 0x4899B0 accepts, here a builder with canreclamate
   (WQ_DF_RECLAIMER) and a target below its maximum health */
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
    case 8:
        if (target_slot == (int)WQ_NONE) return -1;
        if (!w->unit[target_slot].finished) return d->nmenu ? A_HELPBUILD : -1;
        return (d->d.flags & WQ_DF_RECLAIMER) && fdef(w, w->unit[target_slot].type) &&
                       w->unit[target_slot].hp < (int)fdef(w, w->unit[target_slot].type)->hp
                   ? A_REPAIRUNIT : -1;
    case 10: return A_STOP;
    case 3:  return target_slot != (int)WQ_NONE ? f_attack(d, w->unit[target_slot].aim) : d->armed ? A_ATTACK : -1;
    case 9:  return mobile ? A_PATROL : -1;
    case 4:  return d->dgun ? A_DGUN : -1;
    }
    return -1;
}

static int f_action(const WORLDQ* q, int act)
{
    (void)q;
    if (act == WQ_ACT_ACTIVATE) return A_ACTIVATE;
    if (act == WQ_ACT_DEACTIVATE) return A_DEACTIVATE;
    return act >= 0 && act <= WQ_ACT_PARK ? act + 1 : -1;
}

static void logged(FWORLD* w, const WQ_CMD* c)
{
    if (w->nlog < FW_LOG) {
        w->log[w->nlog].c = *c;
        w->log[w->nlog].at = w->gametime;
        w->nlog++;
    }
}

/* The adapter's checks (tagpu_worldq.c, order_check), which `order` and `order_test` both answer by: the unit the
   seat's, a target live, the incarnations a command names (WQ_CMD.inc) the slots' own (birth[]), a QUEUE's count
   and menu and, the fake's own bound, room in the list a QUEUE would append to; no verb but NEW and QUEUE. */
static int f_order_check(const WORLDQ* q, const WQ_CMD* c)
{
    FWORLD* w = fw(q);
    FUNIT* u = fown(q, c->unit);
    const FDEF* d = u ? fdef(w, u->type) : NULL;
    if (!u || !d) return WQ_REFUSED;
    if (c->target != WQ_NONE && (c->target == 0 || c->target >= FW_SLOTS || !w->unit[c->target].live)) return WQ_REFUSED;
    if ((c->inc & WQ_INC_UNIT) && c->birth != w->birth[c->unit]) return WQ_REFUSED;
    if ((c->inc & WQ_INC_TARGET) && (c->target == WQ_NONE || c->tbirth != w->birth[c->target])) return WQ_REFUSED;
    if (c->verb == WQ_CMD_QUEUE) {
        const int adds = u->n && u->list[u->n - 1].action == A_BUILDINGBUILD && u->list[u->n - 1].type == c->type;
        return c->count < 1 || c->count > WQ_QUEUE_MAX || !in_menu(d, c->type) || c->type == w->refuse_queue_type ||
                       (!adds && u->n >= FW_LIST)
                   ? WQ_REFUSED : WQ_OK;
    }
    return c->verb == WQ_CMD_NEW && wq_cmd_formed(c, A_DEACTIVATE + 1, (unsigned)w->ndefs, w->mw ? w->mw : FW_W,
                                                  w->mh ? w->mh : FW_H)
               ? WQ_OK : WQ_REFUSED;
}

static int f_order_test(const WORLDQ* q, const WQ_CMD* c) { return f_order_check(q, c); }

/* NEW replaces the list with the one order (no Shift; the fake keeps no 0x4 nodes); QUEUE appends to a factory's
   list or adds to its last node of the same type, as 0x43B0B0 does, after the adapter's own checks */
static int f_order(const WORLDQ* q, const WQ_CMD* c)
{
    FWORLD* w = fw(q);
    FUNIT* u = fown(q, c->unit);
    WQ_ORDER* o;
    if (f_order_check(q, c) != WQ_OK) return WQ_REFUSED;
    w->ordered[c->unit] = 1;
    w->by[c->unit] |= (uint8_t)(1u << w->src);
    s_shared_other += w->by[c->unit] == 3u && u->type != FW_COMMANDER;
    s_orders_taken++;
    if (c->verb == WQ_CMD_QUEUE) {
        if (u->n && u->list[u->n - 1].action == A_BUILDINGBUILD && u->list[u->n - 1].type == c->type)
            u->list[u->n - 1].count += c->count;
        else {
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
    if (c->action == A_ACTIVATE || c->action == A_DEACTIVATE) {     /* the node ends at once (0x403010, 0x403040) */
        if (fdef(w, u->type)->d.flags & WQ_DF_ONOFF) u->active = (uint8_t)(c->action == A_ACTIVATE);
        u->n = 0;
        logged(w, c);
        return WQ_OK;
    }
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
static int f_gametime(const WORLDQ* q)
{
    memset(fw(q)->ordered, 0, sizeof fw(q)->ordered);
    return fw(q)->gametime;
}

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
    out->birth_known = (uint8_t)!w->nobirth;
    out->birth = w->birth[slot];
    return out->seen;
}

static int f_block_lit(const WORLDQ* q, int bx, int bz)
{
    return f_explored(q, bx * 2, bz * 2) && !fw(q)->dark[bz][bx];
}

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

/* the content map: at every first cell of the window, the worst pass_at over the class's footprint there (cfoot,
   1 x 1 unless a case sets it), as the engine's map folds its class's footprint in */
static int f_passable(const WORLDQ* q, int mclass, int x0, int z0, int w, int h, uint8_t* out, size_t cap)
{
    const FWORLD* fwd = fw(q);
    const int fx = mclass >= 0 && mclass < 3 && fwd->cfoot[mclass][0] > 0 ? fwd->cfoot[mclass][0] : 1;
    const int fz = mclass >= 0 && mclass < 3 && fwd->cfoot[mclass][1] > 0 ? fwd->cfoot[mclass][1] : 1;
    int x, z, a, b;
    if (w < 1 || h < 1 || cap < WQ_PASS_CAP(w, h)) return 0;
    for (z = 0; z < h; z++)
        for (x = 0; x < w; x++) {
            int v = 3;
            for (b = 0; b < fz; b++)
                for (a = 0; a < fx; a++) {
                    const int c = pass_at(fwd, mclass, x0 + x + a, z0 + z + b);
                    if (c < v) v = c;
                }
            out[z * w + x] = (uint8_t)v;
        }
    return 1;
}

static const WORLDQ_OPS FAKE = {
    f_map, f_explored, f_metal, f_coverage, f_mapped_point, f_ground, f_ndefs, f_def, f_def_econ, f_yard,
    f_build_options, f_site_ok, f_blocked_alt, f_feature, f_nslots, f_seat_units, f_unit, f_orders, f_head,
    f_can, f_action, f_order, f_economy, f_gametime, f_observe, f_block_lit, f_radar_step, f_passable, f_starts,
    f_order_test,
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
    w->defs[T_COM].d.reach = w->defs[T_CONS].d.reach = 120;      /* builddistance, px */
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
    w->by[slot] = 0;
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

/* A UNIT SEEN GONE (impai_know.c, slice_slot): an enemy plant the seat sees by sight, its block dark, as the
   centre of a wide structure is when a corner of its box is what the seat sees. The unit dies -- the row is
   emptied, a radar class with it -- or it lives on out of sight, itself or another unit in its slot -- the row
   is a ghost at the block, and stays one when the unit then dies unseen. */
static void check_seen_gone(void)
{
    static FWORLD w;
    static IMPAI_ROW rows[FW_SLOTS];
    static uint8_t bits[WQ_RADAR_BYTES(FW_SLOTS)];
    static IMPAI_HITROW hits[FW_SLOTS];
    static SNAP_PATCH clusters[IMPAI_CLUSTERS];
    static const char* const NAME[] = { "dies in sight", "dies in sight, a radar contact too", "leaves sight",
                                        "leaves sight, its slot another unit's" };
    const IMPAI_ARRAYS a = { rows, FW_SLOTS, bits, (int)sizeof bits,
                             { &hits[0].seat[0], FW_SLOTS, (unsigned)sizeof hits[0], 0 }, clusters };
    const IMPAI_BUDGET b = { FW_SLOTS, 100, 0, 0 };
    const IMPAI_SEEN* r = &rows[40].seat[0];
    WORLDQ q = { &FAKE, &w, 0 };
    IMPAI_SEAT st;
    IMPAI_WORK wk;
    int c, k;
    for (c = 0; c < 4; c++) {
        const unsigned held = c == 1 ? IMPAI_LOS | IMPAI_RADAR : IMPAI_LOS;
        world_init(&w);
        memset(rows, 0, sizeof rows);
        memset(hits, 0, sizeof hits);
        memset(&st, 0, sizeof st);
        unit_add(&w, 40, T_FACTORY, 10, 10, 1);
        w.unit[40].owner = 1;
        w.unit[40].seen = 1;
        w.birth[40] = 7;
        w.radar_slot = c == 1 ? 40 : 0;
        w.gametime = impai_radar_phase(0);
        for (k = 0; k < 2; k++, w.gametime++) CHECK(impai_know_tick(&q, &st, &a, &b, NULL, &wk) == 0, "tick");
        CHECK(r->how == held && r->type == T_FACTORY, "%s: the seat holds the plant first: how %02x", NAME[c], r->how);
        w.dark[r->bz][r->bx] = 1;
        if (c < 2) {
            w.unit[40].live = 0;
        } else {
            w.unit[40].seen = 0;
            if (c == 3) w.birth[40] = 8;
        }
        {
            const unsigned bx = r->bx, bz = r->bz;
            CHECK(impai_know_tick(&q, &st, &a, &b, NULL, &wk) == 0, "tick");
            if (c < 2)
                CHECK(!r->how && !r->type && !r->bx && !r->bz, "%s: a unit seen gone leaves no row: how %02x",
                      NAME[c], r->how);
            else
                CHECK(r->how == IMPAI_GHOST && r->type == T_FACTORY && r->bx == bx && r->bz == bz,
                      "%s: a live unit out of sight is a ghost at its block: how %02x", NAME[c], r->how);
            if (c == 2) {
                w.unit[40].live = 0;
                w.gametime++;
                CHECK(impai_know_tick(&q, &st, &a, &b, NULL, &wk) == 0 && r->how == IMPAI_GHOST && r->bx == bx,
                      "a unit that dies out of sight stays a ghost: how %02x", r->how);
            }
        }
        printf("seen gone: %s how=%02x block=%u,%u\n", NAME[c], r->how, r->bx, r->bz);
    }
}

/* THE SCAN'S END (IMPAI_SEAT.scanned): the patch scan sets the fact at the tick that reads the map's last row, with a
   patch found or none, and not before; the published count is then that scan's */
static void check_scan_done(void)
{
    static FWORLD w;
    static IMPAI_ROW rows[FW_SLOTS];
    static uint8_t bits[WQ_RADAR_BYTES(FW_SLOTS)];
    static IMPAI_HITROW hits[FW_SLOTS];
    static SNAP_PATCH clusters[IMPAI_CLUSTERS];
    static SNAP_SCRATCH scr;
    const IMPAI_ARRAYS a = { rows, FW_SLOTS, bits, (int)sizeof bits,
                             { &hits[0].seat[0], FW_SLOTS, (unsigned)sizeof hits[0], 0 }, clusters };
    const IMPAI_BUDGET b = { 0, 100, FW_H / 2, 100000 };
    WORLDQ q = { &FAKE, &w, 0 };
    IMPAI_SEAT st;
    IMPAI_WORK wk;
    int metal, x, z;
    for (metal = 0; metal < 2; metal++) {
        world_init(&w);
        memset(rows, 0, sizeof rows);
        memset(hits, 0, sizeof hits);
        memset(clusters, 0, sizeof clusters);
        memset(&st, 0, sizeof st);
        if (metal)
            for (z = 10; z <= 12; z++) for (x = 10; x <= 12; x++) w.metal[z][x] = 200;
        w.gametime = impai_radar_phase(0) + 1;
        CHECK(impai_know_tick(&q, &st, &a, &b, &scr, &wk) == 0 && !st.scanned && !st.nclusters,
              "%s: half the map read, not scanned (scanned %u, %u published)", metal ? "a patch" : "no patch",
              st.scanned, st.nclusters);
        w.gametime++;
        CHECK(impai_know_tick(&q, &st, &a, &b, &scr, &wk) == 0 && st.scanned && st.nclusters == (unsigned)metal,
              "%s: the last row read, scanned (scanned %u, %u published)", metal ? "a patch" : "no patch",
              st.scanned, st.nclusters);
        printf("scan's end: %s scanned=%u published=%u\n", metal ? "a patch" : "no patch", st.scanned,
               st.nclusters);
    }
}

/* ---- the brain over the fake world ---- */

static FWORLD s_w;
static IMPAI_TINFO s_ti[FW_DEFS];
static IMPAI_JOB s_job[FW_SLOTS];
static IMPAI_BRAIN s_st;
static IMPAI_PLANTREF s_pref[FW_DEFS];      /* the seat's plant refusals, a type each, cleared with the state */
static SNAP_SCRATCH s_snap;
static void* s_scratch;
static uint16_t s_labels[(FW_W / 2) * (FW_H / 2)];
static IMPAI_REACH s_reach;
/* the plan the decisions play, the one row unless a check moves a field of a copy, the persona they carry it out
   by, Hard's unless a check says otherwise, and the military the economy reads (NULL: none, as at A1) */
static const IMPAI_PLAN* s_plan = &IMPAI_PLAN_ROW;
static const IMPAI_PERSONA* s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
static const IMPAI_MIL* s_milp;
static const IMPAI_VIEW* s_viewp;       /* the seat's knowledge the decisions read, NULL none */
static const IMPAI_TVALUE* s_tvp;       /* the values the decisions read (IMPAI_BRAIN_IN.tv), NULL none */

/* the classifier's answers for the fake's types: the commander and the constructor mobile builders, the
   factory a standing one, the turret a defence; the tech levels are content_set's */
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
}

/* The classifier's input for tech levels alone: a builder (+0x241 bit 6), a mover (canmove and move class 1)
   and a commander, as the engine filler copies them; build options none. */
static void tech_in(TASIM_ROLE_IN* in, int builder, int mobile, int commander)
{
    memset(in, 0, sizeof *in);
    in->flags241 = builder ? TASIM_F241_BUILDER : 0u;
    in->flags245 = (mobile ? TASIM_F245_CANMOVE : 0u) | (commander ? TASIM_F245_COMMANDER : 0u);
    in->bmcode = (uint8_t)(mobile ? 1 : 0);
}

/* Every builder's build options from the menus' table (impai.h, IMPAI_BMENU) as the engine filler takes them,
   the union over the sides (impai_menu_union) without the builder itself, packed into opt; 0, or -1 when opt is
   short. */
static int tech_options(TASIM_ROLE_IN* in, int n, const IMPAI_BMENU* rows, unsigned nrows, const uint16_t* pool,
                        unsigned npool, uint16_t* opt, int cap)
{
    int t, used = 0;
    for (t = 0; t < n; t++) {
        int got, k, m = 0;
        if (!(in[t].flags241 & TASIM_F241_BUILDER)) continue;
        if ((got = impai_menu_union(rows, nrows, pool, npool, (unsigned)t, opt + used, cap - used)) < 0) return -1;
        for (k = 0; k < got; k++) if (opt[used + k] != t) opt[used + m++] = opt[used + k];
        in[t].build = opt + used;
        in[t].nbuild = (uint16_t)m;
        used += m;
    }
    return 0;
}

/* The type information of the fake's n types as the level init builds it: their menus as the level init's table
   of one side, merged into build options as the engine filler takes them, and classified (tasim_roles_classify)
   for the tech levels; the table from those (impai_tinfo_build); and each type's build tech from the same options
   (impai_tinfo_ctech). A type is a mover or a commander by its roles' flags, a builder by WQ_DF_BUILDER. Only
   roles' tech levels are written: the rest is the case's own. impai_tinfo_build's answer, or -1. */
static int content_set(const FWORLD* w, int n, TASIM_ROLE_OUT* roles)
{
    WORLDQ wq = { &FAKE, (void*)w, 0 };
    TASIM_ROLE_IN in[FW_DEFS];
    TASIM_ROLE_OUT out[FW_DEFS];
    TASIM_ROLE_Q q;
    IMPAI_BMENU rows[FW_DEFS];
    uint16_t pool[FW_DEFS * FW_MENU], opt[FW_DEFS * FW_MENU];
    unsigned nrows = 0, npool = 0;
    void* work = malloc(tasim_roles_work(n));
    int t, k, v, got;
    if (!work || n > FW_DEFS) {
        CHECK(0, "content_set: %d types", n);
        free(work);
        return -1;
    }
    for (t = 0; t < n; t++) {
        const FDEF* d = &w->defs[t];
        uint8_t offered[FW_DEFS] = { 0 };
        tech_in(&in[t], (d->d.flags & WQ_DF_BUILDER) != 0, (roles[t].flags & TASIM_RF_MOBILE) != 0,
                (roles[t].flags & TASIM_RF_COMMANDER) != 0);
        if (t == 0 || !(d->d.flags & WQ_DF_BUILDER)) continue;
        for (k = 0; k < d->nmenu; k++) if (d->menu[k] < n) offered[d->menu[k]] = 1;
        memset(&rows[nrows], 0, sizeof rows[nrows]);
        rows[nrows].builder = (uint16_t)t;
        rows[nrows].at = npool;
        for (v = 1; v < n; v++) if (offered[v]) pool[npool++] = (uint16_t)v;   /* ascending, as the table holds it */
        rows[nrows].n = (uint16_t)(npool - rows[nrows].at);
        nrows++;
    }
    CHECK(tech_options(in, n, rows, nrows, pool, npool, opt, (int)(sizeof opt / sizeof opt[0])) == 0, "tech options");
    CHECK(tasim_roles_classify(in, n, out, &q, work) == 0, "tech levels");
    for (t = 0; t < n; t++) roles[t].tech = out[t].tech;
    if ((got = impai_tinfo_build(&wq, roles, n, s_ti, FW_DEFS)) == n) {
        impai_tinfo_ctech(in, roles, n, s_ti, FW_DEFS);
        /* a fake type's weapon hits a target above the sea (WHAT IT HITS); a case gives its water and anti-air
           types their own */
        for (t = 1; t < n; t++) s_ti[t].flags |= IMPAI_TF_HITDRY;
    }
    free(work);
    return got;
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
static unsigned s_scanned;          /* IMPAI_BRAIN_IN.scanned: 0, a scan under way, unless a case sets it */

static void brain_world(void)
{
    TASIM_ROLE_OUT roles[T_N];
    unsigned i;
    int x, z;
    world_init(&s_w);
    s_spots = PATCHES;
    s_nspots = NPATCH;
    s_scanned = 0;
    for (i = 0; i < NPATCH; i++)
        for (z = PATCHES[i].z0; z <= PATCHES[i].z1; z++)
            for (x = PATCHES[i].x0; x <= PATCHES[i].x1; x++) s_w.metal[z][x] = 200;
    roles_set(roles);
    CHECK(content_set(&s_w, T_N, roles) == T_N, "tinfo");
    impai_brain_reset(&s_st, s_job, FW_SLOTS);
    memset(s_pref, 0, sizeof s_pref);
    for (i = 0; i < sizeof s_labels / sizeof s_labels[0]; i++)
        s_labels[i] = (uint16_t)((i % (FW_W / 2)) >= 24 && (i / (FW_W / 2)) < 16 ? 2 : 1);
    memset(&s_reach, 0, sizeof s_reach);
    s_reach.labels = s_labels;
    s_reach.present = 1u;
    s_reach.bw = FW_W / 2;
    s_reach.bh = FW_H / 2;
}

/* the reaction's queue the economy's decisions order through, NULL: none, as at Hard (impai_react.c), and what
   it did */
static IMPAI_EXEC*        s_xp;
static IMPAI_REACT_COUNTS s_rc;

/* THE REASON TRACE (impai.h, IMPAI_WHY), held on every economy decision this test makes: a decision that stops
   before its economy traces nothing; the gates and the posture's pressure are the ones the decision hands out;
   a want taken was needed; a gate is named only from the want's own row, only while it is up, and only for a
   want needed and not taken; and a want with no term read last (every want but the sea plant and the sensor)
   that is needed and not taken names a gate. The tally is check_why's. */
static unsigned s_why_n, s_why_need[IMPAI_WANTS], s_why_veto[IMPAI_WANTS], s_why_take[IMPAI_WANTS], s_why_last;
/* THE REQUESTS (impai.h, IMPAI_RQ_*), held on the same decisions: a request not needed, vetoed or held is the
   trace's; one that stood ends open, answered, yielded or refused; only a plant's request yields, and only the
   sea plant's and the sensor's are refused; a plant's request is answered exactly when the decision ordered a
   plant by THE CHOICE (the plant line's), and then no plant's request stays open; the sensor's exactly when a
   sensor was ordered. The tally is check_requests's. */
static unsigned s_req[IMPAI_WANTS][IMPAI_RQ_STATES];

static int decide_traced(const IMPAI_BRAIN_IN* in, IMPAI_BRAIN_OUT* out)
{
    static const IMPAI_WHY NONE;
    const int rc = impai_brain_decide(in, out);
    const IMPAI_WHY* w = &out->why;
    int k;
    if (rc != 0) return rc;
    if (!w->set) {
        CHECK(!memcmp(w, &NONE, sizeof *w), "a decision that stops before its economy traces nothing");
        return rc;
    }
    s_why_n++;
    CHECK(w->set == 1 && !(w->gates & ~IMPAI_G_ALL) && !(w->need >> IMPAI_WANTS), "the trace's bits: gates %02X need %02X",
          w->gates, w->need);
    CHECK(!(w->gates & IMPAI_G_HOLD) == !out->hold && !(w->pos & IMPAI_WHY_PRESSED) == !out->pressed &&
              !(w->gates & IMPAI_G_PRESSED) == !out->pressed && !(w->gates & IMPAI_G_UNJUDGED) == !(w->naval < 0) &&
              (!(w->pos & IMPAI_WHY_AT_BASE) || (w->pos & IMPAI_WHY_PRESSED)),
          "the trace's gates and posture are the decision's: gates %02X pos %02X naval %d hold %u pressed %u", w->gates,
          w->pos, w->naval, out->hold, out->pressed);
    CHECK(!(w->take & ~w->need), "a want taken was needed: need %02X take %02X", w->need, w->take);
    for (k = 0; k < IMPAI_WANTS; k++) {
        const int need = w->need >> k & 1, take = w->take >> k & 1;
        s_why_need[k] += (unsigned)need;
        s_why_take[k] += (unsigned)take;
        s_why_veto[k] += w->veto[k] != 0;
        CHECK(!(w->veto[k] & ~(w->gates & impai_veto[k].gates)), "%s is vetoed by its row's gates that are up: %02X of %02X",
              impai_veto[k].want, w->veto[k], w->gates);
        CHECK(!w->veto[k] || (need && !take), "%s names a gate only needed and not taken", impai_veto[k].want);
        if (need && !take && !w->veto[k]) {
            s_why_last++;
            CHECK(k == IMPAI_W_SEA || k == IMPAI_W_SENSOR, "%s, needed and not taken, names its gate: gates %02X",
                  impai_veto[k].want, w->gates);
        }
        {
            const unsigned rq = out->req[k];
            const unsigned eco = !need ? IMPAI_RQ_NONE : take ? IMPAI_RQ_OPEN : w->veto[k] ? IMPAI_RQ_VETOED : IMPAI_RQ_HELD;
            const int plant = k == IMPAI_W_FACTORY || k == IMPAI_W_TECH;
            CHECK(rq < IMPAI_RQ_STATES && (take ? rq >= IMPAI_RQ_OPEN : rq == eco),
                  "%s's request is the trace's until a rung ends it: %u, need %d take %d veto %02X", impai_veto[k].want,
                  rq, need, take, w->veto[k]);
            CHECK(rq != IMPAI_RQ_YIELDED || plant, "%s's request yields to no other", impai_veto[k].want);
            CHECK(rq != IMPAI_RQ_REFUSED || k == IMPAI_W_SEA || k == IMPAI_W_SENSOR, "%s's request is never refused",
                  impai_veto[k].want);
            if (rq < IMPAI_RQ_STATES) s_req[k][rq]++;
        }
    }
    {
        const uint8_t* q = out->req;
        const int chosen = (q[IMPAI_W_FACTORY] == IMPAI_RQ_ANSWERED) + (q[IMPAI_W_TECH] == IMPAI_RQ_ANSWERED);
        const int ordered = chosen + (q[IMPAI_W_SEA] == IMPAI_RQ_ANSWERED);
        CHECK(chosen == (out->plant != 0) && ordered <= 1, "one plant a decision, THE CHOICE's with the plant line: "
              "factory %u sea %u tech %u plant %u", q[IMPAI_W_FACTORY], q[IMPAI_W_SEA], q[IMPAI_W_TECH], out->plant);
        CHECK(!ordered || (q[IMPAI_W_FACTORY] != IMPAI_RQ_OPEN && q[IMPAI_W_SEA] != IMPAI_RQ_OPEN &&
                           q[IMPAI_W_TECH] != IMPAI_RQ_OPEN),
              "a plant ordered leaves no plant's request open: factory %u sea %u tech %u", q[IMPAI_W_FACTORY],
              q[IMPAI_W_SEA], q[IMPAI_W_TECH]);
        CHECK((q[IMPAI_W_FACTORY] == IMPAI_RQ_YIELDED || q[IMPAI_W_TECH] == IMPAI_RQ_YIELDED) ? ordered == 1 : 1,
              "a plant's request yields only to a plant ordered");
        CHECK((q[IMPAI_W_SENSOR] == IMPAI_RQ_ANSWERED) == (out->sensors != 0), "the sensor's request and the sensors "
              "ordered: %u, %u", q[IMPAI_W_SENSOR], out->sensors);
    }
    return rc;
}

static int s_dec_acts = -1;        /* decide's action budget, -1 none */
/* the content's price of metal decide passes (IMPAI_BRAIN_IN.e_per_m): 0, the plan's, unless a case sets it */
static float s_dec_epm;

static int decide(IMPAI_BRAIN_OUT* out)
{
    WORLDQ q = { &FAKE, &s_w, 0 };
    IMPAI_EXEC_VIEW xv;
    IMPAI_BRAIN_IN in;
    int rc;
    memset(&in, 0, sizeof in);
    in.q = &q;
    in.st = &s_st;
    in.pref = s_pref;
    in.pref_stride = 1;
    in.job = s_job; in.njob = FW_SLOTS;
    in.ti = s_ti; in.nti = (unsigned)s_w.ndefs;
    in.spots = s_spots; in.nspots = s_nspots;
    in.scanned = s_scanned;
    in.reach = &s_reach;
    in.plan = s_plan;
    in.persona = s_persona;
    in.acts = s_dec_acts;
    in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
    in.snap = &s_snap;
    in.mil = s_milp;
    in.view = s_viewp;
    in.tv = s_tvp;
    in.e_per_m = s_dec_epm;
    if (s_xp) {
        in.q = impai_exec_view(&xv, &q, s_xp, s_persona, (uint32_t)s_w.gametime, IMPAI_SRC_ECO, &s_rc);
        in.acts = impai_exec_acts(s_xp, IMPAI_SRC_ECO, s_persona, (uint32_t)s_w.gametime);
    }
    rc = decide_traced(&in, out);
    if (rc == 0 && s_xp) {
        CHECK(in.acts < 0 || out->roots <= in.acts, "the economy's commands %u within its actions %d", out->roots,
              in.acts);
        impai_exec_spend(s_xp, IMPAI_SRC_ECO, s_persona, out->roots);
    }
    return rc;
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
    CHECK(s_ti[T_FACTORY].tech == 1 && s_ti[T_CONS].tech == 1 && s_ti[T_MEX].tech == 1,
          "the commander's menu is tech 1: factory %u cons %u mex %u", s_ti[T_FACTORY].tech, s_ti[T_CONS].tech,
          s_ti[T_MEX].tech);
    impai_tinfo_count(s_ti, T_N, n);
    printf("tinfo mex=%u energy=%u maker=%u factory=%u cons=%u commander=%u other=%u\n", n[IMPAI_C_MEX],
           n[IMPAI_C_ENERGY], n[IMPAI_C_MAKER], n[IMPAI_C_FACTORY], n[IMPAI_C_CONS], n[IMPAI_C_COMMANDER],
           n[IMPAI_C_OTHER]);
    /* THE GAIN, NET OF THE TYPE'S OWN UPKEEP (impai_tinfo.c): an unarmed building making 5 and using 5 with a radar
       of 1440 -- retail's ARMRAD -- is no energy building and keeps its radar; making 25 and using 5 it is one, of
       gain 20 */
    {
        TASIM_ROLE_OUT roles[T_N];
        int k;
        for (k = 0; k < 2; k++) {
            s_w.defs[T_TIDAL].e.tidal = 0;
            s_w.defs[T_TIDAL].e.minwater = 0;
            s_w.defs[T_TIDAL].e.energymake = k ? 25.0f : 5.0f;
            s_w.defs[T_TIDAL].e.energyuse = 5.0f;
            s_w.defs[T_TIDAL].e.radar = 1440;
            roles_set(roles);
            CHECK(content_set(&s_w, T_N, roles) == T_N, "tinfo, a radar of make %d", k ? 25 : 5);
            printf("tinfo, make %d use 5 radar 1440: cat %u egain %g radar %u\n", k ? 25 : 5, s_ti[T_TIDAL].cat,
                   (double)s_ti[T_TIDAL].egain, s_ti[T_TIDAL].radar);
            CHECK(k ? s_ti[T_TIDAL].cat == IMPAI_C_ENERGY && s_ti[T_TIDAL].egain == 20.0f
                    : s_ti[T_TIDAL].cat == IMPAI_C_OTHER && s_ti[T_TIDAL].radar == 1440,
                  "the gain net of the upkeep: make %d", k ? 25 : 5);
        }
        brain_world();
    }
}

/* THE ATTACK FACTS THROUGH THE FILLERS' MASK (impai_tinfo_attack): a role input built as both role fillers build
   it, the def's +0x245 word masked by TASIM_ROLE_F245, with canmove, canattack, candgun and bit 9 (no bit of the
   mask) set and weapon slot 0 a CommandFire weapon of range 160 and 400 energy a shot: the type can attack and
   D-guns 160 px for 400. Without canattack in the raw word, no IMPAI_TA_CANATTACK; without candgun, no D-gun. */
static void check_tinfo_attack(void)
{
    static const uint32_t RAW[3] = {
        TASIM_F245_CANMOVE | TASIM_F245_CANATTACK | TASIM_F245_CANDGUN | 1u << 9,
        TASIM_F245_CANMOVE | TASIM_F245_CANDGUN | 1u << 9,
        TASIM_F245_CANMOVE | TASIM_F245_CANATTACK | 1u << 9,
    };
    int k;
    for (k = 0; k < 3; k++) {
        TASIM_ROLE_IN in;
        IMPAI_TINFO t;
        memset(&in, 0, sizeof in);
        memset(&t, 0, sizeof t);
        in.flags245 = RAW[k] & TASIM_ROLE_F245;
        in.nweapons = 1;
        in.w[0].range = 160;
        in.w[0].energy_per_shot = 400.0f;
        in.w[0].flags = TASIM_WF_COMMANDFIRE & TASIM_ROLE_WF;
        impai_tinfo_attack(&in, &t);
        printf("tinfo attack raw=0x%05X masked=0x%05X attack=0x%02X dgun_range=%u dgun_e=%u\n", (unsigned)RAW[k],
               (unsigned)in.flags245, t.attack, t.dgun_range, t.dgun_e);
        CHECK(!!(t.attack & IMPAI_TA_CANATTACK) == (k != 1), "canattack through the mask, case %d", k);
        CHECK(k == 2 ? t.dgun_range == 0 && t.dgun_e == 0 : t.dgun_range == 160 && t.dgun_e == 400,
              "the D-gun through the mask, case %d: %u px, %u energy", k, t.dgun_range, t.dgun_e);
    }
}

/* WHAT IT HITS (impai_tinfo_attack): a type's hit facts from its weapon slots, through the fillers' mask -- a gun,
   a torpedo (waterweapon), a toairweapon, a NOTAIR slot's missile (no toairweapon: the ground too), a gun and a
   torpedo, a CommandFire weapon, a stockpile and an interceptor (no fact), and an empty slot list -- each case's
   flags beside a def flag of its own (IMPAI_TF_MOBILE), which stays. */
static void check_tinfo_hits(void)
{
    static const struct { uint32_t w0, w1; int n, notair0; unsigned want; const char* name; } C[] = {
        { 0, 0, 1, 0, IMPAI_TF_HITDRY, "a gun" },
        { TASIM_WF_WATER, 0, 1, 0, IMPAI_TF_HITWET, "a torpedo" },
        { TASIM_WF_TOAIR, 0, 1, 0, IMPAI_TF_HITAIR, "a toairweapon" },
        { 0, 0, 1, 1, IMPAI_TF_HITDRY | IMPAI_TF_HITAIR, "a NOTAIR slot's missile" },
        { 0, TASIM_WF_WATER, 2, 0, IMPAI_TF_HITDRY | IMPAI_TF_HITWET, "a gun and a torpedo" },
        { TASIM_WF_COMMANDFIRE, 0, 1, 0, 0, "a CommandFire weapon" },
        { TASIM_WF_STOCKPILE, TASIM_WF_INTERCEPTOR, 2, 0, 0, "a stockpile and an interceptor" },
        { 0, 0, 0, 0, 0, "no weapon" },
    };
    unsigned k;
    for (k = 0; k < sizeof C / sizeof C[0]; k++) {
        TASIM_ROLE_IN in;
        IMPAI_TINFO t;
        unsigned got;
        memset(&in, 0, sizeof in);
        memset(&t, 0, sizeof t);
        t.flags = IMPAI_TF_MOBILE | IMPAI_TF_HITDRY | IMPAI_TF_HITWET | IMPAI_TF_HITAIR;
        in.nweapons = (uint16_t)C[k].n;
        in.w[0].flags = C[k].w0 & TASIM_ROLE_WF;
        in.w[1].flags = C[k].w1 & TASIM_ROLE_WF;
        in.slot_notair[0] = (uint8_t)C[k].notair0;
        impai_tinfo_attack(&in, &t);
        got = t.flags & (IMPAI_TF_HITDRY | IMPAI_TF_HITWET | IMPAI_TF_HITAIR);
        printf("tinfo hits, %s: 0x%02X targets 0x%X\n", C[k].name, got, impai_type_hits(&t));
        CHECK(got == C[k].want && (t.flags & IMPAI_TF_MOBILE), "what it hits, %s: 0x%02X, want 0x%02X", C[k].name, got,
              C[k].want);
    }
}

/* THE GROUND REACH (impai_maps.c, impai_threat_from_roles): a Samson's missile -- slot 0 NOTAIR, no toairweapon,
   range 600, reload 75 ticks, burst 1, 40 by default and 90 on aircraft -- hits the ground at 40 / 75 x 480 = 256
   over 600 px while its role figures give it no ground damage or range and 90 / 75 x 480 = 576 in the air; the
   same weapon a toairweapon hits nothing on the ground; a commandfire slot adds nothing; a 180-px laser beside the
   missile leaves the reach the missile's 600. For three types with no anti-air slot -- a laser, a ballistic cannon
   of burst 3, both -- the reach is the role's ground figures, exactly. A Samson row's ground layer reaches a cell
   centre 496 px off and not one 701 px off. */
static void reach_input(TASIM_ROLE_IN* in, int notair)
{
    memset(in, 0, sizeof *in);
    in->flags245 = TASIM_F245_CANMOVE;
    in->bmcode = 1;
    in->cost_metal = 100.0f;
    in->slot_notair[0] = (uint8_t)notair;
}

static void reach_weapon(TASIM_ROLE_IN* in, int k, int32_t range, float reload, int32_t burst, int32_t dmg, float air,
                         uint32_t flags)
{
    in->w[k].range = range;
    in->w[k].reload_ticks = reload;
    in->w[k].burst = burst;
    in->w[k].damage_default = dmg;
    in->w[k].damage_air = air;
    in->w[k].flags = flags;
    if (in->nweapons < k + 1) in->nweapons = (uint16_t)(k + 1);
}

static void check_threat_reach(void)
{
    static uint32_t th[IMPAI_THREATS][IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    static int32_t infl[IMPAI_GRID_MAX * IMPAI_GRID_MAX], diff[(IMPAI_GRID_MAX + 1) * IMPAI_GRID_MAX];
    static uint16_t pres[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    static IMPAI_ROW rows[4];
    TASIM_ROLE_IN in[4];
    TASIM_ROLE_OUT out[4];
    TASIM_ROLE_Q q;
    IMPAI_THREAT t[4];
    IMPAI_VIEW v;
    IMPAI_MAPS m;
    void* work = malloc(tasim_roles_work(4));
    int c, k;
    if (!work) {
        CHECK(0, "the classifier's work");
        return;
    }
    for (c = 0; c < 4; c++) {
        reach_input(&in[1], 1);
        reach_weapon(&in[1], 0, 600, 75.0f, 1, 40, 90.0f, c == 1 ? TASIM_WF_TOAIR : 0u);
        if (c == 2) reach_weapon(&in[1], 1, 900, 30.0f, 1, 5000, 5000.0f, TASIM_WF_COMMANDFIRE);
        if (c == 3) reach_weapon(&in[1], 1, 180, 30.0f, 1, 10, 10.0f, 0u);
        reach_input(&in[0], 0);
        tasim_roles_classify(in, 2, out, &q, work);
        impai_threat_from_roles(in, out, 2, t);
        printf("ground reach, case %d: hit_ground=%u hit_range=%u dps_ground=%u range=%u dps_air=%u\n", c,
               t[1].hit_ground, t[1].hit_range, t[1].dps_ground, t[1].range, t[1].dps_air);
        if (c == 1) CHECK(!t[1].hit_ground && !t[1].hit_range, "a toairweapon hits nothing on the ground");
        else if (c == 3)
            CHECK(t[1].hit_range == 600 && t[1].hit_ground == 256 + 160, "the laser beside the missile: %u over %u",
                  t[1].hit_ground, t[1].hit_range);
        else
            CHECK(t[1].hit_ground == 256 && t[1].hit_range == 600 && t[1].dps_ground == 0 && t[1].range == 0 &&
                      t[1].dps_air == 576,
                  "the NOTAIR missile hits the ground, case %d", c);
    }
    for (c = 0; c < 3; c++) {
        reach_input(&in[1], 0);
        if (c != 1) reach_weapon(&in[1], 0, 180, 45.0f, 1, 25, 25.0f, 0u);
        if (c != 0) reach_weapon(&in[1], c == 1 ? 0 : 1, 450, 100.0f, 3, 110, 110.0f, TASIM_WF_BALLISTIC);
        reach_input(&in[0], 0);
        tasim_roles_classify(in, 2, out, &q, work);
        impai_threat_from_roles(in, out, 2, t);
        CHECK(t[1].hit_ground == t[1].dps_ground && t[1].hit_range == t[1].range && t[1].hit_ground,
              "no anti-air slot: the reach is the role's, case %d: %u/%u over %u/%u", c, t[1].hit_ground,
              t[1].dps_ground, t[1].hit_range, t[1].range);
    }
    reach_input(&in[1], 1);
    reach_weapon(&in[1], 0, 600, 75.0f, 1, 40, 90.0f, 0u);
    reach_input(&in[0], 0);
    tasim_roles_classify(in, 2, out, &q, work);
    impai_threat_from_roles(in, out, 2, t);
    free(work);
    memset(rows, 0, sizeof rows);
    memset(&v, 0, sizeof v);
    rows[2].seat[0].type = 1;
    rows[2].seat[0].how = IMPAI_LOS;
    rows[2].seat[0].owner = 1;
    rows[2].seat[0].bx = rows[2].seat[0].bz = 4;        /* the block's centre (144, 144) */
    v.seat = 0;
    v.rows = rows;
    v.nrows = 4;
    v.threat = t;
    v.ntypes = 2;
    v.w = FW_W;
    v.h = FW_H;
    memset(&m, 0, sizeof m);
    for (k = 0; k < IMPAI_THREATS; k++) m.threat[k] = th[k];
    m.influence = infl;
    m.presence = pres;
    m.diff = diff;
    CHECK(impai_maps_build(&v, &m, WQ_NONE) == 1, "the maps build");
    printf("ground reach on the map: cell (0, 2) %u, cell (2, 2) %u\n", th[IMPAI_THREAT_GROUND][2 * m.gw],
           th[IMPAI_THREAT_GROUND][2 * m.gw + 2]);
    CHECK(th[IMPAI_THREAT_GROUND][2 * m.gw] > 0, "the Samson's ground threat reaches the cell 496 px off");
    CHECK(th[IMPAI_THREAT_GROUND][2 * m.gw + 2] == 0, "and not the cell 701 px off");
}

/* THE TECH LEVELS of a level's types, from the menus' table as the engine filler takes them (impai_menu_union,
   tagpu_worldq.c THE FILLER) through the classifier's walk (tasim_roles.c, tech_levels), on retail's ARM K-bot
   chain: the commander builds the lab and a constructor; the lab, a K-bot and the constructor CK; CK the
   advanced lab; the advanced lab the advanced constructor ACK and a K-bot; ACK a plant (CORGANT's place, which
   tasim_run roles makes tech 2 on retail) whose unit is tech 3. Two sides: CK's options are side 1's alone, as
   a builder with no page of its own takes them from the seat's side's DL page, so only the union over the
   sides reaches the advanced lab. A plant is the tech of the factories before it on the chain, so the advanced
   lab, which one factory precedes, is tech 1 like the lab (tasim_run roles: ARMALAB 1, ARMLAB 1), and the
   plant ACK builds, two, is tech 2. */
enum { K_COM = 1, K_CONS, K_LAB, K_PW, K_CK, K_ALAB, K_ACK, K_ZEUS, K_GANT, K_KROG, K_N };
static void check_tech_levels(void)
{
    static const char* const NAME[K_N] = { "-", "COM", "CONS", "LAB", "PW", "CK", "ALAB", "ACK", "ZEUS", "GANT",
                                           "KROG" };
    static const uint16_t POOL[] = {
        K_CONS, K_LAB,                  /* side 0: COM   */
        K_LAB,                          /* side 0: CONS  */
        K_PW, K_CK,                     /* side 0: LAB   */
        K_ACK, K_ZEUS,                  /* side 0: ALAB  */
        K_GANT,                         /* side 0: ACK   */
        K_KROG,                         /* side 0: GANT  */
        K_CONS, K_LAB,                  /* side 1: COM   */
        K_ALAB,                         /* side 1: CK    */
    };
    static const IMPAI_BMENU ROWS[] = {
        { 0, 0, K_COM, 2, 0, 0 }, { 0, 0, K_CONS, 1, 0, 2 }, { 0, 0, K_LAB, 2, 0, 3 }, { 0, 0, K_CK, 0, 0, 5 },
        { 0, 0, K_ALAB, 2, 0, 5 }, { 0, 0, K_ACK, 1, 0, 7 }, { 0, 0, K_GANT, 1, 0, 8 },
        { 1, 0, K_COM, 2, 0, 9 }, { 1, 0, K_CK, 1, 0, 11 },
    };
    const unsigned nrows = (unsigned)(sizeof ROWS / sizeof ROWS[0]), npool = (unsigned)(sizeof POOL / sizeof POOL[0]);
    TASIM_ROLE_IN in[K_N];
    TASIM_ROLE_OUT out[K_N];
    TASIM_ROLE_Q q;
    uint16_t opt[32], u[4];
    void* work = malloc(tasim_roles_work(K_N));
    int t;
    if (!work) { CHECK(0, "tech levels: no scratch"); return; }
    CHECK(impai_menu_find(ROWS, nrows, npool, 0, K_CK) == 3 && impai_menu_find(ROWS, nrows, npool, 1, K_LAB) < 0,
          "a row is found by side and builder");
    CHECK(impai_menu_find(ROWS, nrows, npool - 1, 1, K_CK) < 0, "a row past the pool is none");
    CHECK(impai_menu_union(ROWS, nrows, POOL, npool, K_COM, u, 4) == 2 && u[0] == K_CONS && u[1] == K_LAB,
          "the sides' same options are one");
    CHECK(impai_menu_union(ROWS, nrows, POOL, npool, K_CK, u, 4) == 1 && u[0] == K_ALAB, "side 1's options");
    CHECK(impai_menu_union(ROWS, nrows, POOL, npool, K_COM, u, 1) == -1, "a union past its room is refused");
    for (t = 0; t < K_N; t++) {
        const int builder = t == K_COM || t == K_CONS || t == K_LAB || t == K_CK || t == K_ALAB || t == K_ACK ||
                            t == K_GANT;
        const int mobile = t == K_COM || t == K_CONS || t == K_PW || t == K_CK || t == K_ACK || t == K_ZEUS ||
                           t == K_KROG;
        tech_in(&in[t], builder, mobile, t == K_COM);
    }
    CHECK(tech_options(in, K_N, ROWS, nrows, POOL, npool, opt, (int)(sizeof opt / sizeof opt[0])) == 0, "options");
    CHECK(tasim_roles_classify(in, K_N, out, &q, work) == 0, "classify");
    CHECK(out[K_LAB].tech == 1, "a factory the commander builds is tech 1: %d", out[K_LAB].tech);
    CHECK(out[K_ALAB].tech == 1 && out[K_ZEUS].tech == 2 && out[K_ACK].tech == 2,
          "the advanced lab is tech 1 and its units tech 2: %d %d %d", out[K_ALAB].tech, out[K_ZEUS].tech,
          out[K_ACK].tech);
    CHECK(out[K_GANT].tech == 2 && out[K_KROG].tech == 3, "a tech-2 plant is tech 2 and its unit tech 3: %d %d",
          out[K_GANT].tech, out[K_KROG].tech);
    printf("tech");
    for (t = 1; t < K_N; t++) printf(" %s=%d", NAME[t], out[t].tech);
    putchar('\n');
    in[K_CK].nbuild = 0;
    CHECK(tasim_roles_classify(in, K_N, out, &q, work) == 0 && out[K_ALAB].tech == 0 && out[K_GANT].tech == 0,
          "without side 1's options nothing reaches the advanced lab: %d", out[K_ALAB].tech);
    free(work);
}

/* the decision's phases: seats 0..9 never share a tick at decide 15 */
static void check_due(void)
{
    int seat, t, hits[15] = { 0 };
    for (seat = 0; seat < WQ_SEATS; seat++)
        for (t = 0; t < 15; t++) if (impai_brain_due((unsigned)(1500 + t), (unsigned)seat, s_persona->look_eco)) hits[t]++;
    for (t = 0; t < 15; t++) CHECK(hits[t] <= 1, "two seats decide at phase %d", t);
}

/* THE VETOES' rows (impai_brain.c, impai_veto): one row for each of the economy's six wants, at the want's own
   index, naming no gate the decision does not set. A row left out of the table reads as no want's. */
static void check_veto_rows(void)
{
    static const char* const WANT[] = { "want_factory", "want_sea", "want_tech", "want_def", "want_aa", "want_sensor" };
    const int wants = (int)(sizeof WANT / sizeof WANT[0]);
    int k;
    CHECK(IMPAI_WANTS == wants, "the table has %d rows for %d wants", (int)IMPAI_WANTS, wants);
    for (k = 0; k < IMPAI_WANTS && k < wants; k++) {
        CHECK(impai_veto[k].want && !strcmp(impai_veto[k].want, WANT[k]), "row %d is not %s's", k, WANT[k]);
        CHECK(!(impai_veto[k].gates & ~IMPAI_G_ALL), "row %d names a gate outside the decision's (0x%X)", k,
              (unsigned)impai_veto[k].gates);
    }
}

/* THE LADDER'S ORDER (impai_econ.c, impai_ladder): the rungs in their order, each with the builders it is asked
   of, the requests it serves and waits for, its far-record rule and its gates. A change of the order, or of who is
   asked a rung, is a change of this list. */
static void check_ladder_rows(void)
{
#define RQ(k) (1u << IMPAI_W_##k)
    enum { M = IMPAI_ASK_MENU, B = IMPAI_ASK_BARE, H = IMPAI_ASK_HELPER, HB = IMPAI_ASK_HELPER_BARE };
    static const struct { const char* name; unsigned who, serves, waits, near, on, veto; } ROW[] = {
        { "stall-energy",  M | H,      0,                      0,       0, IMPAI_G_E_STALL, 0 },
        { "sea-frame",     M | H,      0,                      0,       1, 0, IMPAI_G_HOLD | IMPAI_G_M_STALL | IMPAI_G_E_STALL },
        { "sea-plant",     M | H,      RQ(SEA),                0,       1, 0, 0 },
        { "plant",         M | H,      RQ(FACTORY) | RQ(TECH), RQ(SEA), 1, 0, 0 },
        { "defence",       M | H,      RQ(DEF),                0,       0, 0, 0 },
        { "anti-air",      M | H,      RQ(AA),                 0,       0, 0, 0 },
        { "sensor",        M | H,      RQ(SENSOR),             0,       0, 0, 0 },
        { "extractor",     M,          0,                      0,       0, 0, 0 },
        { "short-reclaim", M | H | HB, 0,                      0,       0, 0, 0 },
        { "energy",        M,          0,                      0,       0, 0, 0 },
        { "maker",         M,          0,                      0,       0, 0, 0 },
        { "look",          M,          0,                      0,       0, 0, 0 },
        { "explore",       M | B,      0,                      0,       0, 0, 0 },
        { "harvest",       M | B,      0,                      0,       0, 0, 0 },
        { "assist",        M | B,      0,                      0,       0, 0, 0 },
    };
#undef RQ
    const int rows = (int)(sizeof ROW / sizeof ROW[0]);
    unsigned served = 0;
    int k;
    CHECK(impai_ladder_n == rows, "the ladder has %d rungs, not %d", impai_ladder_n, rows);
    for (k = 0; k < impai_ladder_n && k < rows; k++) {
        const IMPAI_RUNG* r = &impai_ladder[k];
        CHECK(r->name && !strcmp(r->name, ROW[k].name) && r->serve, "rung %d is %s", k, ROW[k].name);
        CHECK(r->who == ROW[k].who && r->serves == ROW[k].serves && r->waits == ROW[k].waits && r->near == ROW[k].near &&
                  r->on == ROW[k].on && r->veto == ROW[k].veto,
              "rung %s: who %X serves %02X waits %02X near %u on %02X veto %02X", ROW[k].name, r->who, r->serves,
              r->waits, r->near, r->on, r->veto);
        CHECK(!(r->serves & served), "rung %s serves a request a rung above serves", ROW[k].name);
        served |= r->serves;
    }
    CHECK(served == (1u << IMPAI_WANTS) - 1u, "every request has its rung: %02X", served);
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
    /* THE ENERGY STALL IS THE STORE'S (impai_brain.c, THE STALLS): the store at 10 % of 1 000 with more coming in
       than asked (64 against 23.7, Escalation's pass at 7 604) is a stall all the same, the wind generator; the
       store at 30 %, less coming in than asked, is none, and the commander takes its patch */
    {
        static const float E[2] = { 100.0f, 300.0f }, IN[2] = { 64.0f, 10.0f }, OUT[2] = { 23.7f, 30.0f };
        int k;
        for (k = 0; k < 2; k++) {
            brain_world();
            unit_add(&s_w, 1, T_COM, 20, 20, 1);
            s_w.econ[0].energy_max = 1000;
            s_w.econ[0].energy = E[k];
            s_w.econ[0].energy_in = IN[k];
            s_w.econ[0].energy_out = OUT[k];
            s_w.gametime = 300;
            CHECK(decide(&out) == 0, "decide");
            c = last_cmd(0);
            printf("the store's stall, energy %.0f in %.1f out %.1f: %s\n", (double)E[k], (double)IN[k], (double)OUT[k],
                   c ? (c->c.type == T_WIND ? "the wind generator" : c->c.type == T_MEX ? "the extractor" : "other")
                     : "nothing");
            CHECK(c && c->c.verb == WQ_CMD_NEW && c->c.type == (k ? T_MEX : T_WIND),
                  "%s", k ? "above the line, no stall: the patch" : "below the line, income above the ask: a stall");
        }
    }
}

/* a free metal patch: the commander takes the nearest reachable one; B, beyond the wall, is never chosen; the
   next constructor takes the other one, the first now claimed */
/* THE OPENING'S FAR PATCH: past comm_r (1024 px) from the commander's start at cell (2, 2), a rich patch about
   1290 px off, a poor one about 1110 px off, and a third about 2240 px off, past 2 x comm_r. With no factory and
   no extractor the commander walks to the nearest, the poor one, which the best-patch rule would pass over; with
   it taken, to the rich one; with only the third, nowhere. Nor with a factory standing, with open_mex extractors
   counted, after the seat lost its only factory, or at a speed whose job_timeout walk falls short of the patch. The
   commander's maxvelocity is CORCOM's, 1.07 px a tick; each patch and its surroundings are explored. */
static const SNAP_PATCH FAR_PATCHES[] = {
    { 58, 58, 58, 58, 60, 60, 9 },      /* rich, about 1290 px */
    { 50, 50, 50, 50, 51, 51, 4 },      /* poor, about 1110 px */
    { 100, 100, 100, 100, 102, 102, 9 } /* past 2 x comm_r */
};

static void far_patch_world(int factory, int mexes)
{
    int x, z;
    unsigned i;
    brain_world();
    s_spots = FAR_PATCHES;
    s_nspots = 2;
    for (i = 0; i < 3; i++)
        for (z = FAR_PATCHES[i].z0 - 4; z <= FAR_PATCHES[i].z1 + 4; z++)
            for (x = FAR_PATCHES[i].x0 - 4; x <= FAR_PATCHES[i].x1 + 4; x++) {
                s_w.explored[z][x] = 1;
                if (x >= FAR_PATCHES[i].x0 && x <= FAR_PATCHES[i].x1 && z >= FAR_PATCHES[i].z0 &&
                    z <= FAR_PATCHES[i].z1)
                    s_w.metal[z][x] = 200;
            }
    s_w.defs[T_COM].d.speed = 70124;
    unit_add(&s_w, 1, T_COM, 2, 2, 1);
    if (factory) unit_add(&s_w, 2, T_FACTORY, 10, 30, 1);
    if (mexes) {
        unit_add(&s_w, 3, T_MEX, 10, 10, 1);
        unit_add(&s_w, 4, T_MEX, 14, 10, 1);
    }
    s_w.gametime = 315;
}

static int type_orders(int type)
{
    int i, n = 0;
    for (i = 0; i < s_w.nlog; i++) n += s_w.log[i].c.verb == WQ_CMD_NEW && s_w.log[i].c.type == type;
    return n;
}

static int mex_orders(void) { return type_orders(T_MEX); }

/* the orders of action `action` (-1: any) given to unit `unit` since the log began */
static int count_unit(int unit, int action)
{
    int i, n = 0;
    for (i = 0; i < s_w.nlog; i++) n += s_w.log[i].c.unit == unit && (action < 0 || s_w.log[i].c.action == action);
    return n;
}

static void check_far_patch(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    far_patch_world(0, 0);
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.unit == 1 && c->c.type == T_MEX && cmd_x0(&c->c) >= 48 && cmd_x0(&c->c) <= 52 &&
          cmd_z0(&c->c) >= 48 && cmd_z0(&c->c) <= 52,
          "the opening's commander walks to the nearest far patch: %d,%d", c ? cmd_x0(&c->c) : -1,
          c ? cmd_z0(&c->c) : -1);
    s_spots = &FAR_PATCHES[0];
    s_nspots = 1;
    s_w.gametime += 15;
    s_w.unit[1].n = 0;
    impai_job_end(&s_job[1]);
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.unit == 1 && c->c.type == T_MEX && cmd_x0(&c->c) >= 56 && cmd_z0(&c->c) >= 56,
          "and with only the rich one, to it: %d,%d", c ? cmd_x0(&c->c) : -1, c ? cmd_z0(&c->c) : -1);
    far_patch_world(0, 0);
    s_spots = &FAR_PATCHES[2];
    s_nspots = 1;
    CHECK(decide(&out) == 0, "decide");
    CHECK(!mex_orders(), "a patch past 2 x comm_r is not walked to: %d extractor orders", mex_orders());
    far_patch_world(0, 0);
    s_w.defs[T_COM].d.speed = 26214;    /* 0.4 px a tick: 1080 px in Hard's job_timeout of 2700 */
    CHECK(decide(&out) == 0, "decide");
    CHECK(!mex_orders(), "nor one past the job_timeout walk: %d extractor orders", mex_orders());
    far_patch_world(1, 0);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!mex_orders(), "with a factory standing it keeps to comm_r: %d extractor orders", mex_orders());
    s_w.unit[2].live = 0;
    s_w.gametime += 15;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_st.opened && !mex_orders(), "nor after the seat lost it: %d extractor orders", mex_orders());
    far_patch_world(0, 1);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!mex_orders(), "with open_mex extractors counted it keeps to comm_r: %d extractor orders", mex_orders());
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

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
    CHECK(out.snaps <= s_plan->snaps && out.sites <= s_plan->site_tests, "the budgets hold");
}

/* THE PATCH ANOTHER PLAYER BUILT ON (impai_build.c): check_spot's commander at (30, 22), patch A its pick, and
   seat 1's extractor on A's cells as the seat's row holds it -- seen now, then a ghost --, or seat 1's constructor
   there instead: with the extractor, A is not free and the commander builds on C; with the constructor, which
   moves, on A. */
static void check_spot_foreign(void)
{
    static const char* const NAME[4] = { "nothing on A", "an extractor seen on A", "an extractor's ghost on A",
                                         "a constructor seen on A" };
    static IMPAI_ROW rows[FW_SLOTS];
    IMPAI_VIEW v;
    IMPAI_BRAIN_OUT out;
    int c;
    for (c = 0; c < 4; c++) {
        const FCMD* m;
        int on_a, on_c;
        brain_world();
        unit_add(&s_w, 1, T_COM, 30, 22, 1);
        s_w.gametime = 315;
        memset(rows, 0, sizeof rows);
        memset(&v, 0, sizeof v);
        v.seat = 0;
        v.rows = rows;
        v.nrows = FW_SLOTS;
        v.w = FW_W;
        v.h = FW_H;
        if (c) {
            rows[40].seat[0].type = (uint16_t)(c == 3 ? T_CONS : T_MEX);
            rows[40].seat[0].how = (uint8_t)(c == 2 ? IMPAI_GHOST : IMPAI_LOS);
            rows[40].seat[0].owner = 1;
            rows[40].seat[0].bx = 20;                       /* cells 40..41: patch A's 40..42 */
            rows[40].seat[0].bz = 10;
        }
        s_viewp = &v;
        CHECK(decide(&out) == 0, "decide: %s", NAME[c]);
        s_viewp = NULL;
        m = last_cmd(0);
        on_a = m && m->c.type == T_MEX && cmd_x0(&m->c) >= 39 && cmd_x0(&m->c) <= 42 && cmd_z0(&m->c) <= 22;
        on_c = m && m->c.type == T_MEX && cmd_z0(&m->c) >= 44;
        printf("spot, %s: on A %d, on C %d\n", NAME[c], on_a, on_c);
        CHECK(c == 1 || c == 2 ? !on_a && on_c : on_a, "the commander's extractor, %s: on A %d, on C %d", NAME[c],
              on_a, on_c);
    }
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

/* ---- the commander's reach (impai_build.c, THE COMMANDER'S REACH) ---- */

/* the BUILD orders since `from` that unit `unit` was given, and whether every one's footprint centre lies within
   `reach` px of the anchor (s_st.ax, az) */
static int builds_within(int from, int unit, uint32_t reach, int* all)
{
    const int32_t apos[3] = { (s_st.ax * 16 + 8) << 16, 0, (s_st.az * 16 + 8) << 16 };
    int k, n = 0;
    *all = 1;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->verb != WQ_CMD_NEW || c->unit != unit || c->action != A_MOBILEBUILD) continue;
        n++;
        if (impai_dist_px(apos, c->pos[0] >> 16, c->pos[2] >> 16) > reach) *all = 0;
    }
    return n;
}

/* a world of the commander at the anchor (20, 20) and, when `cons`, a constructor beside it, in an energy stall
   with no patch, so the ring's energy is the first thing each builds */
static void reach_world(int cons)
{
    brain_world();
    s_spots = NULL;
    s_nspots = 0;
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    if (cons) unit_add(&s_w, 2, T_CONS, 22, 22, 1);
    s_st.ax = 20;
    s_st.az = 20;
    s_w.econ[0].energy = 100;
    s_w.econ[0].energy_in = 10;
    s_w.econ[0].energy_out = 30;
    s_w.gametime = 300;
}

/* THE COMMANDER'S REACH ON THE RING: with a comm_r of 64 px every site of the first ring lies past it (the nearest
   is 80 px off), so the commander is given no ring site and the ring does not grow for it, while a constructor
   in the same decision takes one. With the whole first ring within comm_r and every site of the energy type
   refused, the ring grows. A ring site on a cell of the seat's ground threat map is neither builder's: the
   commander's rule, and a constructor's at a base site as on a walk. */
static void check_ring_reach(void)
{
    const IMPAI_PLAN* lv0 = s_plan;
    IMPAI_PLAN lv = *s_plan;
    static IMPAI_ROW rows[FW_SLOTS];
    IMPAI_THREAT th[FW_DEFS];
    IMPAI_VIEW v;
    IMPAI_BRAIN_OUT out;
    int n, all, k, cell;
    lv.comm_r = 64;
    s_plan = &lv;
    reach_world(0);
    CHECK(decide(&out) == 0, "decide");
    n = builds_within(0, 1, 64, &all);
    printf("ring reach, comm_r 64: commander builds=%d ring_grow=%u\n", n, s_st.ring_grow);
    CHECK(n == 0 && s_st.ring_grow == 0, "no ring site for the commander past its reach, the ring not grown: %d, %u",
          n, s_st.ring_grow);
    reach_world(1);
    CHECK(decide(&out) == 0, "decide");
    n = builds_within(0, 2, 0xFFFFFFFFu, &all);
    CHECK(builds_within(0, 1, 64, &all) == 0 && n == 1 && s_st.ring_grow == 0,
          "the constructor takes the ring site the commander may not: %d", n);
    s_plan = lv0;

    reach_world(0);
    s_w.refuse_type = T_WIND;
    CHECK(decide(&out) == 0, "decide");
    printf("ring reach, every wind site refused within comm_r: ring_grow=%u\n", s_st.ring_grow);
    CHECK(s_st.ring_grow > 0, "a ring refused within comm_r grows: %u", s_st.ring_grow);

    /* an enemy turret whose range covers the 256-px cell (1, 0), north of the anchor's, which holds the first ring
       site, and nothing else: neither the commander nor the constructor builds there */
    for (cell = 0; cell <= 1; cell++) {
        reach_world(1);
        memset(rows, 0, sizeof rows);
        memset(th, 0, sizeof th);
        memset(&v, 0, sizeof v);
        th[T_TURRET].dps_ground = th[T_TURRET].hit_ground = 10;
        th[T_TURRET].range = th[T_TURRET].hit_range = 50;
        th[T_TURRET].layer = TASIM_LAYER_LAND;
        v.seat = 0;
        v.rows = rows;
        v.nrows = FW_SLOTS;
        v.threat = th;
        v.ntypes = T_N;
        v.w = FW_W;
        v.h = FW_H;
        rows[40].seat[0].type = T_TURRET;
        rows[40].seat[0].how = IMPAI_LOS;
        rows[40].seat[0].owner = 1;
        rows[40].seat[0].bx = 12;                           /* its block's centre (400, 144) */
        rows[40].seat[0].bz = 4;
        if (cell) s_viewp = &v;
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        for (k = 0; k < s_w.nlog; k++) {
            const WQ_CMD* c = &s_w.log[k].c;
            const int in10 = (c->pos[0] >> 16) / 256 == 1 && (c->pos[2] >> 16) / 256 == 0;
            if (c->action != A_MOBILEBUILD) continue;
            printf("ring reach, %s: unit %u builds type %u at (%d, %d)\n", cell ? "threat on cell (1, 0)" : "no threat",
                   c->unit, c->type, c->pos[0] >> 16, c->pos[2] >> 16);
            if (cell) CHECK(!in10, "unit %u is given a ring site under ground threat", c->unit);
        }
    }
}

/* THE CUT RING GROWS (impai_build.c): reach_world's commander in an energy stall with four site tests a decision,
   fewer than a ring's sites. With the wind generator refused everywhere, each decision's first walk -- the stall's
   rung -- is cut after four refusals and counts as exhausted, so the ring grows by one a decision; the energy
   rung's walk after it finds the tests spent, tests nothing and grows nothing. With no site test to spend no walk
   tests a site and the ring stays. With the wind generator refused only where its first cell lies in the square
   6..29 round the anchor, which holds every site of ring 0, the ring grows once and the wind generator is built
   at the second decision on ring 1, outside the square: a seat whose nearest sites take nothing still builds. */
static void check_ring_cut(void)
{
    const IMPAI_PLAN* lv0 = s_plan;
    IMPAI_PLAN lv = *s_plan;
    IMPAI_BRAIN_OUT out;
    int k, all, n = 0;
    lv.site_tests = 4;
    s_plan = &lv;
    reach_world(0);
    s_w.refuse_type = T_WIND;
    for (k = 0; k < 3; k++) {
        CHECK(decide(&out) == 0, "decide");
        s_w.gametime += 15;
        printf("ring cut, every wind site refused, 4 site tests: decision %d ring_grow=%u\n", k, s_st.ring_grow);
        CHECK(s_st.ring_grow == k + 1, "a walk cut on refusals alone grows the ring once a decision: %u at decision %d",
              s_st.ring_grow, k);
    }
    lv.site_tests = 0;
    reach_world(0);
    s_w.refuse_type = T_WIND;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_st.ring_grow == 0, "a walk that tested no site grows nothing: %u", s_st.ring_grow);
    lv.site_tests = 4;
    reach_world(0);
    s_w.refuse_box_type = T_WIND;
    s_w.refuse_box.x0 = s_w.refuse_box.z0 = 6;
    s_w.refuse_box.x1 = s_w.refuse_box.z1 = 29;
    for (k = 0; k < 4 && !n; k++) {
        CHECK(decide(&out) == 0, "decide");
        s_w.gametime += 15;
        n = builds_within(0, 1, 0xFFFFFFFFu, &all);
        printf("ring cut, the ground round the anchor refused: decision %d ring_grow=%u builds=%d\n", k, s_st.ring_grow, n);
    }
    CHECK(n == 1 && k == 2, "the wind generator is built once the ring has grown past the refused square: %d builds in %d decisions",
          n, k);
    if (n) {
        const FCMD* c = last_cmd(0);
        const int x = cmd_x0(&c->c), z = cmd_z0(&c->c);
        printf("ring cut, the wind generator at (%d, %d), ring_grow=%u\n", x, z, s_st.ring_grow);
        CHECK((x < 6 || x > 29 || z < 6 || z > 29) && s_st.ring_grow == 0, "outside the refused square, the ring back at 0: (%d, %d), %u",
              x, z, s_st.ring_grow);
    }
    s_w.refuse_box_type = 0;
    s_plan = lv0;
}

/* THE RING'S UNSEEN SITES and THE RING'S LOOK (impai_build.c): reach_world's commander at the anchor (20, 20) in
   an energy stall, the seat's map explored for x, z < 32 alone and the wind generator refused wherever its first cell lies
   there, as a start hemmed in by ground no plant fits. Rings 0 and 1 lie on explored ground and are refused -- in
   the first decision, the stall's rung and the energy rung each asking the ring --, so the ring grows to 2,
   whose east and south sides stand at cell 34, their centres unexplored: from the second decision the commander is
   sent to see the nearest (a move, its job an explore of that centre's cell, within comm_r) and the ring stays at
   2 with no site test spent on unseen ground, at every decision until the seat has seen. With the ground seen
   the wind generator is built there, on the first site outside the refused box. A commander that stands idle with
   its look's cell unseen -- its order list emptied, as on arrival -- has that cell failed by the job's check and
   looks at another. A builder that walks nowhere (no label under it) is sent nowhere: the unseen sites are refused
   ones, and the ring grows as it does on a refusal. */
static void check_ring_unseen(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    int k, x, z, n, built = 0;
    unsigned i;
    reach_world(0);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = x < 32 && z < 32;
    s_w.refuse_box_type = T_WIND;
    s_w.refuse_box.x0 = s_w.refuse_box.z0 = 0;
    s_w.refuse_box.x1 = s_w.refuse_box.z1 = 31;
    for (k = 0; k < 40; k++) {
        const int32_t apos[3] = { (s_st.ax * 16 + 8) << 16, 0, (s_st.az * 16 + 8) << 16 };
        s_w.gametime += 15;
        n = s_w.nlog;
        CHECK(decide(&out) == 0, "decide");
        CHECK(out.sites < s_plan->site_tests, "unseen ground costs no site test: %u at decision %d", out.sites, k);
        if (!k) {
            CHECK(s_job[1].kind == IMPAI_JOB_NONE && s_st.ring_grow == 2 && s_w.nlog == n,
                  "the rings refused on explored ground grow: %u", s_st.ring_grow);
            continue;
        }
        c = last_cmd(n);
        if (k == 1 && c)
            printf("ring unseen: ring_grow=%u, the commander looks at cell %d,%d\n", s_st.ring_grow, s_job[1].cx,
                   s_job[1].cz);
        CHECK(s_st.ring_grow == 2, "the ring does not grow past unseen ground: %u at decision %d", s_st.ring_grow, k);
        CHECK(c && c->c.verb == WQ_CMD_NEW && c->c.unit == 1 && c->c.action == A_MOVE &&
                  s_job[1].kind == IMPAI_JOB_EXPLORE, "the commander is sent to look at decision %d", k);
        CHECK(!s_w.explored[s_job[1].cz][s_job[1].cx] && (s_job[1].cx >= 32 || s_job[1].cz >= 32) &&
                  impai_dist_px(apos, s_job[1].cx * 16 + 8, s_job[1].cz * 16 + 8) <= s_plan->comm_r,
              "at an unexplored centre within comm_r: %d,%d", s_job[1].cx, s_job[1].cz);
        impai_job_end(&s_job[1]);       /* the walk cut short, the site still unseen: the next decision looks again */
    }
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    for (k = 0; k < 3 && !built; k++) {
        s_w.gametime += 15;
        n = s_w.nlog;
        CHECK(decide(&out) == 0, "decide");
        c = last_cmd(n);
        built = c && c->c.action == A_MOBILEBUILD && c->c.type == T_WIND;
    }
    c = last_cmd(0);
    printf("ring unseen, the ground seen: built=%d ring_grow=%u\n", built, s_st.ring_grow);
    CHECK(built && (cmd_x0(&c->c) >= 32 || cmd_z0(&c->c) >= 32) && s_st.ring_grow == 0,
          "the wind generator on the ground the look showed: %d", built);

    reach_world(0);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = x < 32 && z < 32;
    s_w.refuse_box_type = T_WIND;
    s_w.refuse_box.x0 = s_w.refuse_box.z0 = 0;
    s_w.refuse_box.x1 = s_w.refuse_box.z1 = 31;
    for (k = 0; k < 2; k++) {
        s_w.gametime += 15;
        CHECK(decide(&out) == 0, "decide");
    }
    x = s_job[1].cx;
    z = s_job[1].cz;
    s_w.gametime += 15;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_job[1].kind == IMPAI_JOB_EXPLORE && s_job[1].cx == x && s_job[1].cz == z && !s_st.nfail,
          "a look on its way is left at it");
    s_w.unit[1].n = 0;
    s_w.gametime += 15;
    CHECK(decide(&out) == 0, "decide");
    for (k = 0, n = 0; k < s_st.nfail && k < IMPAI_FAILS; k++) n += s_st.fail[k].cx == x && s_st.fail[k].cz == z;
    printf("ring unseen, the look idle at %d,%d: failed %d, the next look at %d,%d, ring_grow=%u\n", x, z, n,
           s_job[1].cx, s_job[1].cz, s_st.ring_grow);
    CHECK(n == 1 && s_job[1].kind == IMPAI_JOB_EXPLORE && (s_job[1].cx != x || s_job[1].cz != z) &&
              !s_w.explored[s_job[1].cz][s_job[1].cx] && s_st.ring_grow == 2,
          "an idle look's cell fails and the commander looks at another: %d,%d", s_job[1].cx, s_job[1].cz);

    reach_world(0);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = x < 32 && z < 32;
    s_w.refuse_box_type = T_WIND;
    s_w.refuse_box.x0 = s_w.refuse_box.z0 = 0;
    s_w.refuse_box.x1 = s_w.refuse_box.z1 = 31;
    for (i = 0; i < sizeof s_labels / sizeof s_labels[0]; i++) s_labels[i] = 0;
    for (k = 0; k < 6; k++) {
        s_w.gametime += 15;
        CHECK(decide(&out) == 0, "decide");
    }
    printf("ring unseen, a builder that walks nowhere: orders=%d ring_grow=%u\n", s_w.nlog, s_st.ring_grow);
    CHECK(!s_w.nlog && s_job[1].kind == IMPAI_JOB_NONE && s_st.ring_grow > 2,
          "sites it cannot walk to see are refused ones, and the ring grows: %u", s_st.ring_grow);
    s_w.refuse_box_type = 0;
}

/* one enemy row of seat 0 at block (bx, bz), slot 40: type `type` (WQ_NONE: a contact never identified, owner
   0xFF), known by `how`, in the view v over the threat table th */
static void ring_view(IMPAI_VIEW* v, IMPAI_ROW* rows, IMPAI_THREAT* th, int type, unsigned how, int bx, int bz)
{
    memset(rows, 0, FW_SLOTS * sizeof *rows);
    memset(v, 0, sizeof *v);
    v->seat = 0;
    v->rows = rows;
    v->nrows = FW_SLOTS;
    v->threat = th;
    v->ntypes = T_N;
    v->w = FW_W;
    v->h = FW_H;
    rows[40].seat[0].type = (uint16_t)type;
    rows[40].seat[0].how = (uint8_t)how;
    rows[40].seat[0].owner = (uint8_t)(type == (int)WQ_NONE ? 0xFF : 1);
    rows[40].seat[0].bx = (uint16_t)bx;
    rows[40].seat[0].bz = (uint16_t)bz;
}

/* THE COMMANDER'S SAFE DISTANCE (impai_build.c): the commander alone at the anchor (20, 20) in an energy stall, a
   turret armed by its threat row (dps 10) whose ground reach, 50 px, covers only its own cell. Seen 424 px east
   at block (23, 10), within IMPAI_CMD_SAFE_PX of every first-ring site: no ring site, the ring not grown. Seen
   1 096 px east, block (44, 10): a site. An unarmed constructor's row 424 px east: a site. A contact never
   identified there, held by the radar: no site. A turret's ghost there, a structure remembered: no site. */
static void check_ring_cmd_safe(void)
{
    static const struct { const char* what; int type; unsigned how; int bx, want; } CASE[5] = {
        { "an armed turret 424 px east", T_TURRET, IMPAI_LOS, 23, 0 },
        { "an armed turret 1 096 px east", T_TURRET, IMPAI_LOS, 44, 1 },
        { "an unarmed constructor 424 px east", T_CONS, IMPAI_LOS, 23, 1 },
        { "a radar contact 424 px east", (int)WQ_NONE, IMPAI_RADAR, 23, 0 },
        { "a turret's ghost 424 px east", T_TURRET, IMPAI_GHOST, 23, 0 } };
    static IMPAI_ROW rows[FW_SLOTS];
    IMPAI_THREAT th[FW_DEFS];
    IMPAI_VIEW v;
    IMPAI_BRAIN_OUT out;
    int c, n, all;
    for (c = 0; c < 5; c++) {
        reach_world(0);
        memset(th, 0, sizeof th);
        th[T_TURRET].dps_ground = th[T_TURRET].hit_ground = 10;
        th[T_TURRET].range = th[T_TURRET].hit_range = 50;
        ring_view(&v, rows, th, CASE[c].type, CASE[c].how, CASE[c].bx, 10);
        s_viewp = &v;
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        n = builds_within(0, 1, 0xFFFFFFFFu, &all);
        printf("commander's safe distance, %s: builds %d, ring_grow %u\n", CASE[c].what, n, s_st.ring_grow);
        CHECK(n == CASE[c].want && s_st.ring_grow == 0, "commander's safe distance, %s: %d builds, ring_grow %u",
              CASE[c].what, n, s_st.ring_grow);
    }
}

/* GROWTH ANSWERS ONLY A LACK OF ROOM (impai_build.c, THE RING): the commander and a constructor with energy to
   spare, past open_t with no factory, under a turret seen at the anchor whose ground reach, 2 000 px, covers every
   ring site on the map, for twenty decisions: no ring site, the ring never grown and no plant type passed over.
   A plant type is passed over only by a factory ring exhausted at the eighth growth, so the growth is the half
   that bites here: the ring at 8 reaches past the 128-cell map, whose sites off the grid carry no threat and
   spend the decision's site tests instead. */
static void check_ring_raid(void)
{
    static IMPAI_ROW rows[FW_SLOTS];
    IMPAI_THREAT th[FW_DEFS];
    IMPAI_VIEW v;
    IMPAI_BRAIN_OUT out;
    int k, t, builds = 0, grown = 0, passed = 0;
    reach_world(1);
    s_w.gametime = 1800;
    s_w.econ[0].energy = 1000;
    s_w.econ[0].energy_in = 30;
    s_w.econ[0].energy_out = 0;
    memset(th, 0, sizeof th);
    th[T_TURRET].dps_ground = th[T_TURRET].hit_ground = 10;
    th[T_TURRET].range = th[T_TURRET].hit_range = 2000;
    ring_view(&v, rows, th, T_TURRET, IMPAI_LOS, 10, 10);
    s_viewp = &v;
    for (k = 0; k < 20; k++) {
        CHECK(decide(&out) == 0, "decide");
        if (s_st.ring_grow) grown++;
        s_w.gametime += 15;
    }
    s_viewp = NULL;
    for (k = 0; k < s_w.nlog; k++) builds += s_w.log[k].c.action == A_MOBILEBUILD;
    for (t = 1; t < T_N; t++) passed += s_pref[t].until != 0;
    printf("a raid over the base, twenty decisions: builds %d, decisions with the ring grown %d, plant types passed "
           "over %d\n", builds, grown, passed);
    CHECK(!builds && !grown && !passed, "a raid over the base grows the ring or passes a plant over: %d, %d, %d",
          builds, grown, passed);
}

/* NO BUILDER GUARDS (impai_build.c, THE ASSIST): a busy factory within comm_r and no frame, the factory waiting for
   its next product: neither the commander nor the constructor is given a guard, and the constructor no assist.
   With the product's frame on its yard, the constructor helps the frame with mode 8. */
static void check_assist_help(void)
{
    IMPAI_BRAIN_OUT out;
    FUNIT* f;
    int x, z;
    reach_world(1);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    unit_add(&s_w, 3, T_FACTORY, 26, 20, 1);
    s_w.econ[0].energy = 500;
    f = &s_w.unit[3];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    s_w.econ[0].energy_in = 1000;
    CHECK(decide(&out) == 0, "decide");
    printf("assist help, no frame: commander guards=%d orders=%d, constructor guards=%d helps=%d\n",
           count_unit(1, A_GUARD), count_unit(1, -1), count_unit(2, A_GUARD), count_unit(2, A_HELPBUILD));
    CHECK(count_unit(1, A_GUARD) == 0 && count_unit(2, A_GUARD) == 0 && count_unit(2, A_HELPBUILD) == 0,
          "no frame: no guard and no help");
    unit_add(&s_w, 4, T_CONS, 27, 22, 0);
    s_w.gametime += 15;
    s_w.nlog = 0;
    CHECK(decide(&out) == 0, "decide");
    printf("assist help, the product's frame on the yard: constructor guards=%d helps=%d, its job %u target %u\n",
           count_unit(2, A_GUARD), count_unit(2, A_HELPBUILD), s_job[2].kind, s_job[2].target);
    CHECK(count_unit(2, A_GUARD) == 0 && count_unit(2, A_HELPBUILD) == 1 && s_job[2].kind == IMPAI_JOB_ASSIST &&
              s_job[2].target == 4,
          "the constructor helps the product's frame");
}

/* THE UNIT'S OWN LABEL (impai_maps.c, impai_reach_unit): check_assist_help's world with no constructor, so the frame is
   the commander's to help, the commander at (328, 320) px -- its centre in block (10, 10), its first cell (20, 19) in
   block (10, 9) -- and the labels: (a) everywhere 1: it helps; (b) its centre's block 0, as beside a feature's ring:
   it helps still, by its first cell's block; (c) both blocks 0, the blocks round them 1, as a commander standing
   where a feature stood at the level init: it helps, by the label beside (THE LABEL BESIDE); (d) the blocks round
   them 0 too: it reaches nothing and is given no help. */
static int own_label_helps(int centre, int first, int ring)
{
    IMPAI_BRAIN_OUT out;
    FUNIT* f;
    int x, z;
    reach_world(0);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    for (x = 0; x < (int)(sizeof s_labels / sizeof s_labels[0]); x++) s_labels[x] = 1;
    for (x = 0; x < 9; x++) s_labels[(8 + x / 3) * (FW_W / 2) + 9 + x % 3] = (uint16_t)ring;
    s_labels[10 * (FW_W / 2) + 10] = (uint16_t)centre;
    s_labels[9 * (FW_W / 2) + 10] = (uint16_t)first;
    s_w.unit[1].pos[0] = 328 << 16;
    s_w.unit[1].pos[2] = 320 << 16;
    unit_add(&s_w, 3, T_FACTORY, 26, 20, 1);
    f = &s_w.unit[3];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    unit_add(&s_w, 4, T_CONS, 27, 22, 0);
    s_w.econ[0].energy = 500;
    s_w.econ[0].energy_in = 1000;
    CHECK(decide(&out) == 0, "decide");
    return count_unit(1, A_HELPBUILD);
}

static void check_own_label(void)
{
    const int a = own_label_helps(1, 1, 1), b = own_label_helps(0, 1, 1), c = own_label_helps(0, 0, 1),
              e = own_label_helps(0, 0, 0);
    printf("the unit's own label: helps with every block labelled %d, the centre's block unlabelled %d, both, the "
           "blocks round labelled %d, those too %d\n", a, b, c, e);
    CHECK(a == 1 && b == 1 && c == 1 && e == 0, "the commander's label is its first cell's block, else the one "
          "round it: %d %d %d %d", a, b, c, e);
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 320 << 16, 2, 2) == 0, "both blocks and those round them "
          "unlabelled read 0");
    s_labels[8 * (FW_W / 2) + 9] = 3;
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 320 << 16, 2, 2) == 3, "both blocks unlabelled: the label "
          "beside, (9, 8)'s");
    s_labels[10 * (FW_W / 2) + 11] = 3;
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 320 << 16, 2, 2) == 3, "two blocks beside, one label");
    s_labels[10 * (FW_W / 2) + 9] = 4;
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 320 << 16, 2, 2) == 0, "two labels beside read 0");
    s_labels[8 * (FW_W / 2) + 9] = s_labels[10 * (FW_W / 2) + 11] = s_labels[10 * (FW_W / 2) + 9] = 0;
    s_labels[9 * (FW_W / 2) + 10] = 5;
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 320 << 16, 2, 2) == 5, "the first cell's block's label");
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 336 << 16, 2, 2) == 0, "past the edge, one block: (10, 10)'s");
    s_labels[9 * (FW_W / 2) + 10] = 0;
    s_labels[10 * (FW_W / 2) + 10] = 7;
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 320 << 16, 2, 2) == 7,
          "the centre's block stands in for the first cell's unlabelled one");
    s_labels[8 * (FW_W / 2) + 9] = 3;
    CHECK(impai_reach_unit(&s_reach, 0, 328 << 16, 320 << 16, 2, 2) == 7,
          "the centre's block before the blocks beside, another label among them");
    s_labels[8 * (FW_W / 2) + 9] = 0;
    s_labels[10 * (FW_W / 2) + 10] = 0;
    s_labels[9 * (FW_W / 2) + 10] = 1;
    s_labels[9 * (FW_W / 2) + 9] = 6;
    CHECK(impai_reach_unit(&s_reach, 0, 312 << 16, 320 << 16, 2, 2) == 6,
          "a footprint 2 across at x 312: its first cell 19, block 9, west of its centre's cell 20");
    s_labels[9 * (FW_W / 2) + 9] = 1;
}

/* THE HELP REFUSED (impai_brain.c): check_assist_help's world, its constructor given the help on the product's
   frame. The engine ends the help short of the frame (the constructor's head empty, the frame unfinished): the next
   decision ends the job, records the frame's cell and gives the constructor no help on it. The plant's next product,
   a new slot at the same cell, is passed over too; a frame at another cell is helped; and after fail_t the cell is
   helped again. A help that ends at its frame's completion ends its job and records nothing. A record at a frame's
   cell restored past fail_t of now (record_live) holds nothing: the frame is helped. */
static int help_round(IMPAI_BRAIN_OUT* out, int ticks)
{
    s_w.unit[2].n = 0;
    impai_job_end(&s_job[2]);
    s_w.gametime += ticks;
    s_w.nlog = 0;
    return decide(out);
}

static void check_help_refused(void)
{
    IMPAI_BRAIN_OUT out;
    FUNIT* f;
    int x, z, refused, product, other, other_t, again;
    uint32_t until;
    reach_world(1);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    unit_add(&s_w, 3, T_FACTORY, 26, 20, 1);
    s_w.econ[0].energy = 500;
    f = &s_w.unit[3];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    s_w.econ[0].energy_in = 1000;
    unit_add(&s_w, 4, T_CONS, 27, 22, 0);
    CHECK(decide(&out) == 0, "decide");
    CHECK(count_unit(2, A_HELPBUILD) == 1 && s_job[2].kind == IMPAI_JOB_ASSIST && s_job[2].target == 4,
          "the constructor helps the product's frame");
    s_w.unit[2].n = 0;
    s_w.gametime += 15;
    s_w.nlog = 0;
    CHECK(decide(&out) == 0, "decide");
    refused = count_unit(2, A_HELPBUILD);
    printf("help refused: helps=%d its job %u, record %d,%d until %u (now %d), help_refused=%u\n", refused,
           s_job[2].kind, s_job[2].help_cx, s_job[2].help_cz, s_job[2].help_until, s_w.gametime, out.help_refused);
    CHECK(!refused && s_job[2].kind != IMPAI_JOB_ASSIST && s_job[2].help_cx == 27 && s_job[2].help_cz == 22 &&
              s_job[2].help_until == (uint32_t)s_w.gametime + s_plan->fail_t && out.help_refused == 1,
          "a help the engine ended short of its frame is recorded and not given again");
    s_w.unit[4].finished = 1;
    unit_add(&s_w, 5, T_CONS, 27, 22, 0);
    CHECK(help_round(&out, 15) == 0, "decide");
    product = count_unit(2, A_HELPBUILD);
    unit_add(&s_w, 5, T_CONS, 29, 22, 0);
    CHECK(help_round(&out, 15) == 0, "decide");
    other = count_unit(2, A_HELPBUILD);
    other_t = s_job[2].target;
    unit_add(&s_w, 5, T_CONS, 27, 22, 0);
    CHECK(help_round(&out, (int)s_plan->fail_t) == 0, "decide");
    again = count_unit(2, A_HELPBUILD);
    printf("help refused: the next product at the cell helps=%d, a frame at another cell helps=%d, after fail_t "
           "helps=%d\n", product, other, again);
    CHECK(!product, "the plant's next product, at the refused cell, is passed over");
    CHECK(other == 1 && other_t == 5, "a frame at another cell is helped");
    CHECK(again == 1 && s_job[2].kind == IMPAI_JOB_ASSIST, "after fail_t the cell is helped again");
    until = s_job[2].help_until;
    s_w.unit[5].finished = 1;
    s_w.unit[2].n = 0;
    s_w.gametime += 15;
    s_w.nlog = 0;
    CHECK(decide(&out) == 0, "decide");
    printf("help refused: the frame finished: help_refused=%u, its job %u, record until %u (was %u)\n",
           out.help_refused, s_job[2].kind, s_job[2].help_until, until);
    CHECK(!out.help_refused && s_job[2].help_until == until && s_job[2].kind != IMPAI_JOB_ASSIST,
          "a help that ends at its frame's completion records nothing");
    unit_add(&s_w, 6, T_CONS, 27, 22, 0);
    s_job[2].help_cx = 27;
    s_job[2].help_cz = 22;
    s_job[2].help_until = (uint32_t)s_w.gametime + 15 + s_plan->fail_t + 1;
    CHECK(help_round(&out, 15) == 0, "decide");
    printf("help refused: a record restored past fail_t: helps=%d\n", count_unit(2, A_HELPBUILD));
    CHECK(count_unit(2, A_HELPBUILD) == 1 && s_job[2].target == 6,
          "a record past fail_t of now holds nothing: the frame at its cell is helped");
}

/* THE COMMANDER HELPS AT HOME (impai_build.c): check_assist_guard's world with a comm_r of 64 px, the busy factory
   at (26, 20), and an unfinished mobile unit of the seat's -- the factory's frame -- at cell (fcx, fcz); with
   `building` a type, an unfinished one of it at (17, 17) as well, 34 px from the anchor for a solar collector.
   s_plan is the copy with the comm_r until the case's end. */
static IMPAI_PLAN s_hplan;
static void helps_world(int fcx, int fcz, int building)
{
    FUNIT* f;
    int x, z;
    s_hplan = IMPAI_PLAN_ROW;
    s_hplan.comm_r = 64;
    s_plan = &s_hplan;
    reach_world(1);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    unit_add(&s_w, 3, T_FACTORY, 26, 20, 1);
    f = &s_w.unit[3];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    unit_add(&s_w, 4, T_CONS, fcx, fcz, 0);
    if (building) unit_add(&s_w, 5, building, 17, 17, 0);
    s_w.econ[0].energy = 500;
    s_w.econ[0].energy_in = 1000;
}

/* one decision of the helps world with `metal` stored of its 1000: the commander's last order, its action and
   target, into *action and *target (0 and -1 with none); the constructor's helps into *chelps */
static void helps_decide(const char* name, float metal, IMPAI_BRAIN_OUT* out, int* action, int* target, int* chelps)
{
    const int from = s_w.nlog;
    int k;
    s_w.econ[0].metal = metal;
    CHECK(decide(out) == 0, "decide");
    *action = 0;
    *target = -1;
    *chelps = 0;
    for (k = from; k < s_w.nlog; k++) {
        if (s_w.log[k].c.unit == 1) {
            *action = s_w.log[k].c.action;
            *target = s_w.log[k].c.target == WQ_NONE ? -1 : (int)s_w.log[k].c.target;
        }
        *chelps += s_w.log[k].c.unit == 2 && s_w.log[k].c.action == A_HELPBUILD;
    }
    printf("helps at home, %s: commander action=%d target=%d, constructor helps=%d\n", name, *action, *target, *chelps);
}

static void helps_case(const char* name, int fcx, int fcz, int building, float metal, int* action, int* target,
                       int* chelps)
{
    IMPAI_BRAIN_OUT out;
    helps_world(fcx, fcz, building);
    helps_decide(name, metal, &out, action, target, chelps);
    s_plan = &IMPAI_PLAN_ROW;
}

/* The commander helps the factory's frame within comm_r with mode 8, and the constructor helps it too; a
   frame past comm_r gives the commander nothing; with a building frame within comm_r as well, the building comes
   first. */
static void check_commander_helps_factory(void)
{
    int action, target, chelps;
    helps_case("a frame 32 px from the anchor", 22, 20, 0, 500.0f, &action, &target, &chelps);
    CHECK(action == A_HELPBUILD && target == 4 && chelps == 1,
          "the commander and the constructor help the factory's frame within comm_r: %d %d %d", action, target,
          chelps);
    helps_case("a frame 160 px from the anchor", 30, 20, 0, 500.0f, &action, &target, &chelps);
    CHECK(action == 0, "the commander is given an order for a frame past comm_r: %d", action);
    helps_case("a frame and a building frame", 22, 20, T_SOLAR, 500.0f, &action, &target, &chelps);
    CHECK(action == A_HELPBUILD && target == 5, "the building frame comes before the factory's: %d %d", action, target);
}

/* THE HOLD'S ASSIST (impai_build.c, THE ASSIST): the helps world with 100 of 1000 stored, a hold. The factory's frame
   alone: the commander gets no order and the constructor no guard. The solar collector's frame beside it: the
   commander helps the solar. An unfinished factory within comm_r, the mobile frame past it: no help. At 500 the
   three cases of check_commander_helps_factory run as they do there. */
static void check_hold_assist(void)
{
    int action, target, chelps;
    helps_case("in a hold, a frame 32 px from the anchor", 22, 20, 0, 100.0f, &action, &target, &chelps);
    CHECK(action == 0 && chelps == 0, "in a hold the factory's frame is helped: %d %d", action, chelps);
    helps_case("in a hold, a frame and a building frame", 22, 20, T_SOLAR, 100.0f, &action, &target, &chelps);
    CHECK(action == A_HELPBUILD && target == 5, "in a hold the solar's frame is not helped: %d %d", action, target);
    helps_case("in a hold, a plant's frame", 30, 20, T_FACTORY, 100.0f, &action, &target, &chelps);
    CHECK(action != A_HELPBUILD && chelps == 0, "in a hold a plant's frame is helped: %d %d", action, target);
    check_commander_helps_factory();
}

/* THE RELEASE (impai_econ.c): the helps world over two decisions, at 500 stored -- the commander and the
   constructor help the factory's frame -- then at 100, a hold. The second decision releases both: each
   has exactly one order, the ladder's or a stop, none a guard or a help, and neither keeps its assist. With the
   solar's frame as well, both help the solar at 500, and at 100 neither is released. */
static void check_hold_release(void)
{
    IMPAI_BRAIN_OUT out;
    int action, target, chelps, k, u, from, n[3], bad = 0;
    helps_world(22, 20, 0);
    helps_decide("the release, at 500", 500.0f, &out, &action, &target, &chelps);
    CHECK(action == A_HELPBUILD && target == 4 && chelps == 1 && s_job[1].kind == IMPAI_JOB_ASSIST &&
              s_job[2].kind == IMPAI_JOB_ASSIST,
          "the release's first decision: the commander and the constructor help the frame");
    s_w.gametime += 15;
    from = s_w.nlog;
    helps_decide("the release, at 100", 100.0f, &out, &action, &target, &chelps);
    for (u = 1; u <= 2; u++) {
        n[u] = 0;
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.unit == u) {
                n[u]++;
                bad += s_w.log[k].c.action == A_GUARD || s_w.log[k].c.action == A_HELPBUILD;
            }
    }
    printf("the release: hold %u released %u, commander orders %d job %u, constructor orders %d job %u\n", out.hold,
           out.released, n[1], s_job[1].kind, n[2], s_job[2].kind);
    CHECK(out.hold && out.released == 2 && n[1] == 1 && n[2] == 1 && !bad && s_job[1].kind != IMPAI_JOB_ASSIST &&
              s_job[2].kind != IMPAI_JOB_ASSIST,
          "the hold releases each helper with one order that is no help");
    helps_world(22, 20, T_SOLAR);
    helps_decide("the release beside the solar, at 500", 500.0f, &out, &action, &target, &chelps);
    s_w.gametime += 15;
    helps_decide("the release beside the solar, at 100", 100.0f, &out, &action, &target, &chelps);
    printf("the release beside the solar: released %u, commander job %u target %d, constructor job %u target %d\n",
           out.released, s_job[1].kind, s_job[1].target == WQ_NONE ? -1 : (int)s_job[1].target, s_job[2].kind,
           s_job[2].target == WQ_NONE ? -1 : (int)s_job[2].target);
    CHECK(out.released == 0 && s_job[1].kind == IMPAI_JOB_ASSIST && s_job[1].target == 5,
          "the hold releases a helper on the solar's frame");
    s_plan = &IMPAI_PLAN_ROW;
}

/* THE RELEASE UNDER A BUDGET (impai_econ.c): the release's world, both helpers on the factory's work at 500 stored;
   then an idle constructor beside the anchor, and at 100, a hold in an energy stall, under a bucket of one command a decision (a
   persona of Hard's with apm_eco 120 and burst_eco 1, at react 0). The idle constructor would take the decision's
   one command; the idle loop leaves it to the release, so each decision releases one helper and both are off the
   factory's work after two decisions. So do THE MAKERS, an active maker the stall would switch off, and THE YARD,
   an idle constructor parked on the waiting lab's yard: both helpers released in two decisions, no switch and no
   yard move. */
static int count_action(int from, int action, int unit);

static void check_hold_release_budget(void)
{
    static IMPAI_EXEC x;
    IMPAI_PERSONA p = IMPAI_PERSONAS[IMPAI_LV_HARD];
    IMPAI_BRAIN_OUT out;
    int action, target, chelps, k, i, released = 0, roots_ok = 1;
    p.apm_eco = 120;
    p.burst_eco = 1;
    CHECK(impai_persona_ok(&p), "the one-command persona is a persona");
    helps_world(22, 20, 0);
    helps_decide("the release under a budget, at 500", 500.0f, &out, &action, &target, &chelps);
    CHECK(s_job[1].kind == IMPAI_JOB_ASSIST && s_job[2].kind == IMPAI_JOB_ASSIST, "both helpers on the factory's work");
    unit_add(&s_w, 6, T_CONS, 20, 22, 1);
    s_w.econ[0].energy = 0;                 /* an energy stall: the ladder has a solar for the idle constructor */
    s_w.econ[0].energy_in = 10;
    s_w.econ[0].energy_out = 30;
    s_persona = &p;
    impai_exec_reset(&x, &p);
    s_xp = &x;
    for (k = 0; k < 2; k++) {
        s_w.gametime += 15;
        helps_decide("the release under a budget, at 100", 100.0f, &out, &action, &target, &chelps);
        released += out.released;
        roots_ok &= out.roots <= 1;
        printf("the release under a budget, decision %d: roots %u released %u, jobs %u %u, the idle one %u\n", k + 1,
               out.roots, out.released, s_job[1].kind, s_job[2].kind, s_job[6].kind);
    }
    s_xp = NULL;
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    s_plan = &IMPAI_PLAN_ROW;
    CHECK(roots_ok && released == 2 && s_job[1].kind != IMPAI_JOB_ASSIST && s_job[2].kind != IMPAI_JOB_ASSIST,
          "one command a decision releases both helpers in two decisions: released %d", released);

    /* the release before THE MAKERS and THE YARD: the same world and stall, with an active onoffable maker the
       energy short would switch off and an idle constructor parked on the lab's yard, which is waiting; the one
       command a decision still releases both helpers in two decisions, and neither the switch nor the yard move
       takes it */
    for (k = 0; k < 2; k++) {
        int switched = 0, yarded = 0;
        helps_world(22, 20, 0);
        helps_decide("the release before the makers and the yard, at 500", 500.0f, &out, &action, &target, &chelps);
        if (k == 0) {
            s_w.defs[T_MAKER].d.flags |= WQ_DF_ONOFF;
            s_ti[T_MAKER].flags |= IMPAI_TF_ONOFF;
            unit_add(&s_w, 6, T_MAKER, 40, 40, 1);
            s_w.unit[6].active = 1;
            s_w.unit[6].req_e = s_ti[T_MAKER].egain;
        } else {
            unit_add(&s_w, 6, T_CONS, 27, 22, 1);
            memcpy(s_w.unit[6].pos, s_w.unit[3].pos, sizeof s_w.unit[6].pos);
            s_w.unit[6].list[0].action = A_PARK;
            s_w.unit[6].list[0].flags = 0x1001;
            s_w.unit[6].list[0].target = (uint16_t)WQ_NONE;
            s_w.unit[6].n = 1;
        }
        s_w.econ[0].energy = 0;
        s_w.econ[0].energy_in = 10;
        s_w.econ[0].energy_out = 30;
        s_persona = &p;
        impai_exec_reset(&x, &p);
        s_xp = &x;
        released = 0;
        roots_ok = 1;
        for (i = 0; i < 2; i++) {
            const int from = s_w.nlog;
            s_w.gametime += 15;
            helps_decide("the release before the makers and the yard, at 100", 100.0f, &out, &action, &target,
                         &chelps);
            released += out.released;
            roots_ok &= out.roots <= 1;
            switched += count_action(from, A_DEACTIVATE, 6);
            yarded += out.yard;
        }
        s_xp = NULL;
        s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
        s_plan = &IMPAI_PLAN_ROW;
        printf("the release before the %s: released %d, switched %d, yard %d\n", k ? "yard" : "makers", released,
               switched, yarded);
        CHECK(roots_ok && released == 2 && !switched && !yarded,
              "the release before the %s: released %d, switched %d, yard %d", k ? "yard" : "makers", released,
              switched, yarded);
    }

    /* the release before the economy helpers: the commander and the constructor help the solar's frame and a
       second constructor, a later slot, guards the factory -- no rule orders a guard, but a recording's state may
       hold one --; in a hold and an energy stall, where the first constructor's assist would be reassigned to the
       ladder, the decision's one command releases the second */
    helps_world(22, 20, T_SOLAR);
    helps_decide("the release before an economy helper, at 500", 500.0f, &out, &action, &target, &chelps);
    unit_add(&s_w, 7, T_CONS, 22, 24, 1);
    s_job[7].kind = IMPAI_JOB_ASSIST;
    s_job[7].type = T_CONS;
    s_job[7].target = 3;
    s_w.unit[7].list[0].action = A_GUARD;
    s_w.unit[7].list[0].target = 3;
    s_w.unit[7].n = 1;
    s_w.econ[0].energy = 0;
    s_w.econ[0].energy_in = 10;
    s_w.econ[0].energy_out = 30;
    s_persona = &p;
    impai_exec_reset(&x, &p);
    s_xp = &x;
    s_w.gametime += 15;
    helps_decide("the release before an economy helper, at 100", 100.0f, &out, &action, &target, &chelps);
    s_xp = NULL;
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    s_plan = &IMPAI_PLAN_ROW;
    printf("the release before an economy helper: roots %u released %u, jobs %u %u %u\n", out.roots, out.released,
           s_job[1].kind, s_job[2].kind, s_job[7].kind);
    CHECK(s_job[2].kind == IMPAI_JOB_ASSIST && s_job[2].target == 5, "the first constructor helps the solar's frame");
    CHECK(out.roots == 1 && out.released == 1 && s_job[7].kind != IMPAI_JOB_ASSIST,
          "the decision's one command releases the second constructor: released %u", out.released);
}

/* the caps: six idle constructors and two idle factories give at most assign_max builder orders and no queue,
   the constructors past the army's share with no army to buy (no military); two idle factories with none queue
   one constructor, the opening, the second held by the army's share (impai_econ.c, THE GOVERNOR) */
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
    CHECK(builds <= s_plan->assign_max, "builder orders %d past assign_max", builds);
    CHECK(out.roots <= s_plan->roots && out.queued <= s_plan->queue_max, "roots %u queued %u", out.roots,
          out.queued);
    CHECK(queues == 0, "six constructors stand, past the army's share: %d queued", queues);
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    s_w.gametime = 915;
    CHECK(decide(&out) == 0, "decide");
    for (i = 0, queues = 0; i < s_w.nlog; i++)
        if (s_w.log[i].c.verb == WQ_CMD_QUEUE && s_w.log[i].c.type == T_CONS && s_w.log[i].c.count == 1) queues++;
    CHECK(queues == 1, "the opening's one constructor, the second factory held by the army's share: %d", queues);
}

/* a constructor its factory parked: the Park GetBuilt gives a unit when the factory holds no rally never ends
   while it is the unit's one node, so the economy tasks the unit; a Park with a node behind it ends by itself
   and the unit is left to that node. The commander is busy on a build of its own. */
static void park_world(int behind)
{
    FUNIT* u;
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    s_w.unit[1].list[0].action = A_MOBILEBUILD;
    s_w.unit[1].list[0].type = T_SOLAR;
    s_w.unit[1].n = 1;
    unit_add(&s_w, 2, T_CONS, 30, 40, 1);
    u = &s_w.unit[2];
    u->list[0].action = A_PARK;
    u->list[0].flags = 0x1001;
    u->list[0].target = (uint16_t)WQ_NONE;
    u->n = 1;
    if (behind) {
        u->list[1].place = 1;
        u->list[1].action = A_MOVE;
        u->list[1].target = (uint16_t)WQ_NONE;
        u->list[1].pos[0] = u->pos[0];
        u->list[1].pos[2] = u->pos[2];
        u->n = 2;
    }
    s_st.ax = 20;
    s_st.az = 20;
    s_w.gametime = 960;
}

static void check_park(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    int i, to_cons = 0;
    park_world(0);
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.unit == 2 && c->c.verb == WQ_CMD_NEW && c->c.type == T_MEX,
          "the parked constructor builds an extractor");
    CHECK(s_job[2].kind == IMPAI_JOB_BUILD && s_job[2].type == T_CONS, "its job is kept");
    if (c) cmd_print(c);
    park_world(1);
    CHECK(decide(&out) == 0, "decide");
    for (i = 0; i < s_w.nlog; i++) to_cons += s_w.log[i].c.unit == 2;
    CHECK(!to_cons && s_job[2].kind == IMPAI_JOB_NONE, "a Park with a node behind it is not idle: %d orders",
          to_cons);
}

/* THE WAY OUT: the factory's yard is `o` but for two `c` cells at the middle of its south edge, and its menu's
   one product, the constructor, is 2 x 2 on class 0. The world of check_factory places it at some site with
   its way out proven: one 2 x 2 run from just below the exit straight south until it stands 16 cells clear,
   kept in the seat's state and claimed. A ring of cliff two cells out around that site -- the strip below the
   exit clear, the way closed further on -- refuses the site, which the way refusals' own ring holds, and the
   factory goes elsewhere. With
   the factory standing at its site the census claims its way again; with neither the factory nor its order
   (nor a builder to order it again) the way is dropped. The fake's class map is per cell, its footprint not folded in as the engine's is, so the
   cliff is a ring of single cells, which a one-cell step of the search cannot cross either. */
static void way_world(int ring_x0, int ring_z0, int ring_x1, int ring_z1)
{
    brain_world();
    s_w.defs[T_FACTORY].yarded = 1;
    memset(s_w.defs[T_FACTORY].yard, 0x2F, 36);
    s_w.defs[T_FACTORY].yard[5 * 6 + 2] = s_w.defs[T_FACTORY].yard[5 * 6 + 3] = 0x2D;
    if (ring_x0 >= 0) {
        water_add(&s_w, ring_x0, ring_z0, ring_x1, ring_z0, -100);
        water_add(&s_w, ring_x0, ring_z1, ring_x1, ring_z1, -100);
        water_add(&s_w, ring_x0, ring_z0, ring_x0, ring_z1, -100);
        water_add(&s_w, ring_x1, ring_z0, ring_x1, ring_z1, -100);
    }
    unit_add(&s_w, 1, T_COM, 20, 30, 1);
    unit_add(&s_w, 2, T_MEX, 40, 20, 1);
    unit_add(&s_w, 3, T_MEX, 10, 45, 1);
    unit_add(&s_w, 4, T_SOLAR, 24, 20, 1);
    unit_add(&s_w, 5, T_SOLAR, 24, 25, 1);
    s_w.gametime = 600;
}

/* the decision's claims hold the way rectangle (x0, z0, x1, z1) as a way */
static int way_claimed(int x0, int z0, int x1, int z1)
{
    const IMPAI_DEC* dd = (const IMPAI_DEC*)s_scratch;
    int i;
    for (i = 0; i < dd->nclaims; i++)
        if (dd->claims[i].way && dd->claims[i].type == T_FACTORY && dd->claims[i].r.x0 == x0 &&
            dd->claims[i].r.z0 == z0 && dd->claims[i].r.x1 == x1 && dd->claims[i].r.z1 == z1)
            return 1;
    return 0;
}

static void check_way(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    int x0 = -1, z0 = -1, x1, z1;
    way_world(-1, 0, 0, 0);
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.type == T_FACTORY, "the factory, its way out open");
    if (c) {
        x0 = cmd_x0(&c->c);
        z0 = cmd_z0(&c->c);
        CHECK(s_st.nway == 1 && s_st.way[0].type == T_FACTORY && s_st.way[0].cx == x0 && s_st.way[0].cz == z0 &&
              s_st.way[0].n == 1, "the ordered factory's way is kept: %d ways, %d rectangles", s_st.nway,
              s_st.way[0].n);
        CHECK(s_st.way[0].r[0][0] == x0 + 2 && s_st.way[0].r[0][1] == z0 + 6 && s_st.way[0].r[0][2] == x0 + 3 &&
              s_st.way[0].r[0][3] == z0 + 6 + 16 + 1, "straight south from the exit to 16 clear: %d,%d..%d,%d",
              s_st.way[0].r[0][0], s_st.way[0].r[0][1], s_st.way[0].r[0][2], s_st.way[0].r[0][3]);
        CHECK(way_claimed(x0 + 2, z0 + 6, x0 + 3, z0 + 23), "and claimed");
    }
    s_w.unit[1].n = 0;
    unit_add(&s_w, 6, T_FACTORY, x0, z0, 1);
    s_w.gametime = 615;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_st.nway == 1 && way_claimed(x0 + 2, z0 + 6, x0 + 3, z0 + 23), "the standing factory's way is claimed");
    s_w.unit[6].live = 0;
    s_w.unit[1].live = 0;
    s_w.gametime = 630;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_st.nway == 0, "with neither the factory nor its order, the way is dropped");

    way_world(x0 - 3, z0 - 3, x0 + 8, z0 + 9);
    CHECK(pass_at(&s_w, 0, x0 + 2, z0 + 6) == 3 && pass_at(&s_w, 0, x0 + 3, z0 + 8) == 3,
          "the strip below the exit is clear");
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.type == T_FACTORY, "the factory, its first site walled in");
    if (c) {
        int k, failed = 0;
        x1 = cmd_x0(&c->c);
        z1 = cmd_z0(&c->c);
        CHECK(x1 != x0 || z1 != z0, "not on the walled-in site: %d,%d", x1, z1);
        for (k = 0; k < s_st.nwfail; k++)
            failed |= s_st.wfail[k].cx == x0 && s_st.wfail[k].cz == z0 && s_st.wfail[k].type == T_FACTORY &&
                      s_st.wfail[k].until > (uint32_t)s_w.gametime;
        CHECK(failed, "the walled-in site's way is refused for the factory");
        CHECK(s_st.nfail == 0, "and nothing in the shared fail ring: %d", s_st.nfail);
        CHECK(s_st.nway == 1 && s_st.way[0].cx == x1 && s_st.way[0].cz == z1, "the new site's way is kept");
        printf("way: first site %d,%d, beside the walled-in one %d,%d\n", x0, z0, x1, z1);
    }

    /* a plant whose yard the world will not give is refused at every site, never taken as one nothing leaves */
    way_world(-1, 0, 0, 0);
    s_w.yard_refuse = T_FACTORY;
    CHECK(decide(&out) == 0, "decide");
    CHECK(!type_orders(T_FACTORY), "no factory without its yard: %d orders", type_orders(T_FACTORY));
    CHECK(s_st.nway == 0, "and no way kept: %d", s_st.nway);
}

/* THE WAY OUT'S BLOCKERS AND CLASSES, on check_way's open world, whose factory goes to (x0, z0) with its exit's
   start square at (x0 + 2, z0 + 6). Each case puts one thing there and says whether the factory still takes the
   site:
   - a solar collector of the seat's own, standing just below the exit: a building of its census closes the way;
   - another player's solar collector the seat's rows hold there, at its 32-px block: closes it too;
   - the same collector standing as a unit of seat 1 that the rows do not hold: the factory takes the site, as it
     would were nothing there -- no unseen unit reaches the rule;
   The level row's base_r is 0 here, so no building joins the base box and the ring is the same in every case.
   - a product of footprint 2 x 4 (its class's, folded into the content map as the engine folds it) and one blocked
     cell in its 4 x 4 square, outside the 2 x 4 its corner entry covers: the whole square is tested, so the way
     is refused;
   - a second product of class 1, the fake's sea class, with exits north and south and water north: the land
     class leaves south and the sea class north, each by its own path, so the factory takes the site and keeps
     both paths -- no one path suits both classes. */
static int factory_at(int x, int z)
{
    int i;
    for (i = 0; i < s_w.nlog; i++)
        if (s_w.log[i].c.verb == WQ_CMD_NEW && s_w.log[i].c.type == T_FACTORY && cmd_x0(&s_w.log[i].c) == x &&
            cmd_z0(&s_w.log[i].c) == z)
            return 1;
    return 0;
}

static void check_way_blockers(void)
{
    static IMPAI_ROW rows[FW_SLOTS];
    const IMPAI_PLAN* lv0 = s_plan;
    IMPAI_PLAN lv = *s_plan;
    IMPAI_VIEW v;
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    int x0, z0;
    lv.base_r = 0;
    s_plan = &lv;
    way_world(-1, 0, 0, 0);
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    if (!c || c->c.type != T_FACTORY) {
        CHECK(0, "the open world's factory");
        s_plan = lv0;
        return;
    }
    x0 = cmd_x0(&c->c);
    z0 = cmd_z0(&c->c);

    way_world(-1, 0, 0, 0);
    unit_add(&s_w, 6, T_SOLAR, x0 + 1, z0 + 7, 1);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!factory_at(x0, z0) && type_orders(T_FACTORY) == 1, "the seat's own building below the exit moves the "
          "factory: %d orders", type_orders(T_FACTORY));

    way_world(-1, 0, 0, 0);
    memset(rows, 0, sizeof rows);
    memset(&v, 0, sizeof v);
    v.seat = 0;
    v.rows = rows;
    v.nrows = FW_SLOTS;
    v.w = FW_W;
    v.h = FW_H;
    rows[40].seat[0].type = T_SOLAR;
    rows[40].seat[0].how = IMPAI_LOS;
    rows[40].seat[0].owner = 1;
    rows[40].seat[0].bx = (uint16_t)((x0 + 3) / 2);
    rows[40].seat[0].bz = (uint16_t)((z0 + 9) / 2);
    s_viewp = &v;
    CHECK(decide(&out) == 0, "decide");
    CHECK(!factory_at(x0, z0) && type_orders(T_FACTORY) == 1, "another player's building the rows hold below the "
          "exit moves the factory: %d orders", type_orders(T_FACTORY));

    way_world(-1, 0, 0, 0);
    memset(rows, 0, sizeof rows);
    unit_add(&s_w, 40, T_SOLAR, x0 + 1, z0 + 7, 1);
    s_w.unit[40].owner = 1;
    CHECK(decide(&out) == 0, "decide");
    CHECK(factory_at(x0, z0), "the same building unseen: the factory takes the site");
    s_viewp = NULL;

    way_world(-1, 0, 0, 0);
    s_ti[T_CONS].fz = 4;
    s_w.cfoot[0][0] = 2;
    s_w.cfoot[0][1] = 4;
    water_add(&s_w, x0 + 5, z0 + 8, x0 + 5, z0 + 8, -100);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!factory_at(x0, z0) && type_orders(T_FACTORY) == 1, "a cell of the 2 x 4 product's square its corner "
          "entry does not cover moves the factory: %d orders", type_orders(T_FACTORY));

    way_world(-1, 0, 0, 0);
    {
        static const uint16_t MENU[] = { T_CONS, T_TURRET };
        menu_set(&s_w, T_FACTORY, MENU, 2);
    }
    s_ti[T_TURRET].flags |= IMPAI_TF_MOBILE;
    s_ti[T_TURRET].mclass = 1;
    s_w.defs[T_FACTORY].yard[0 * 6 + 2] = s_w.defs[T_FACTORY].yard[0 * 6 + 3] = 0x2D;
    s_w.sea = 50;
    water_add(&s_w, x0 - 12, 0, x0 + 18, z0 - 1, 20);
    CHECK(decide(&out) == 0, "decide");
    CHECK(factory_at(x0, z0), "a land class south and a sea class north, each on its own path: the factory takes the "
          "site");
    CHECK(s_st.nway == 1 && s_st.way[0].n == 2 && s_st.way[0].r[0][1] > z0 && s_st.way[0].r[1][3] < z0,
          "the two classes' paths are kept, south and north: %d rectangles", s_st.nway ? s_st.way[0].n : -1);
    printf("way blockers: the open site %d,%d\n", x0, z0);
    s_plan = lv0;
}

/* THE WORK BUDGET: check_way's world with a 40-cell constructor, so one proof costs 53 448 cells (the raster's box
   of 118 x 118, then the class's window of 160 x 160 and its box again) and a second does not fit a decision's
   98 304. With the first site walled in, the decision refuses it, which spends 53 448, and defers the next: it
   refuses nothing more and orders nothing. The next decision, its budget whole, proves that site and orders the
   factory there, refusing nothing. */
static void work_world(int ring_x0, int ring_z0, int ring_x1, int ring_z1)
{
    way_world(ring_x0, ring_z0, ring_x1, ring_z1);
    s_w.defs[T_CONS].d.fx = s_w.defs[T_CONS].d.fz = 40;
    s_ti[T_CONS].fx = s_ti[T_CONS].fz = 40;
}

static void check_way_work(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    int x0 = -1, z0 = -1;
    work_world(-1, 0, 0, 0);
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.type == T_FACTORY, "the open world's factory, its one proof inside the budget");
    if (!c) return;
    x0 = cmd_x0(&c->c);
    z0 = cmd_z0(&c->c);
    work_world(x0 - 3, z0 - 3, x0 + 8, z0 + 9);
    CHECK(decide(&out) == 0, "decide");
    CHECK(type_orders(T_FACTORY) == 0, "the budget spent on the walled-in site: no factory");
    CHECK(s_st.nwfail == 1 && s_st.wfail[0].cx == x0 && s_st.wfail[0].cz == z0 && s_st.nfail == 0,
          "only the walled-in site refused, the next deferred: %d way refusals", s_st.nwfail);
    s_w.gametime += 15;
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(c && c->c.type == T_FACTORY && (cmd_x0(&c->c) != x0 || cmd_z0(&c->c) != z0) && s_st.nwfail == 1 &&
          s_st.nway == 1, "the next decision proves the deferred site and orders the factory: %d,%d, %d refusals",
          c ? cmd_x0(&c->c) : -1, c ? cmd_z0(&c->c) : -1, s_st.nwfail);
    printf("way work: walled-in site %d,%d, the deferred one proven at %d,%d\n", x0, z0, c ? cmd_x0(&c->c) : -1,
           c ? cmd_z0(&c->c) : -1);
}

/* THE PLANT'S FALLBACK: check_way's base on a plateau 20 cells a side, cliffs all round, so no plant site's way can
   stand 16 cells clear of it. The opening factory's way is refused WAY_REFUSALS times, every site held by the way refusals'
   own ring and none by the shared one; the type is then passed over, every type the choice would take is, and the
   plant goes to the first site the site test passes (the fake's reads no height), named in the decision's output. */
static void check_plateau(void)
{
    IMPAI_BRAIN_OUT out;
    int k, got = -1;
    way_world(-1, 0, 0, 0);
    water_add(&s_w, 0, 0, FW_W - 1, FW_H - 1, -100);
    water_add(&s_w, 10, 12, 29, 31, -10);
    for (k = 0; k < 6 && got < 0; k++) {
        CHECK(decide(&out) == 0, "decide: plateau %d", k);
        if (type_orders(T_FACTORY)) got = k;
        s_w.gametime += 15;
    }
    CHECK(got >= 0 && out.waived == T_FACTORY, "a plateau start orders its factory without a way: decision %d, "
          "waived %u", got, out.waived);
    CHECK(s_st.nfail == 0 && s_st.nwfail >= 16, "the refusals in their own ring: shared %d, way %d", s_st.nfail,
          s_st.nwfail);
    CHECK(s_st.nway == 0, "and no way kept: %d", s_st.nway);
    CHECK(s_pref[T_FACTORY].until > (uint32_t)s_w.gametime && s_pref[T_FACTORY].n == 0,
          "the plant type passed over in its own entry: until %u", s_pref[T_FACTORY].until);
    printf("plateau: the factory at decision %d, waived at %d,%d, %d way refusals\n", got, out.waived_cx,
           out.waived_cz, s_st.nwfail);
}

/* A SITE THE BUILDER COULD NOT REACH: a constructor far from patch A ends its BUILD job there with no extractor
   built, past its build distance (THE REACH ABANDON). No failure is recorded: the site is held back for this
   builder alone, its next order goes elsewhere, and the commander, within reach, takes patch A in the same
   decision. A unit of another type in the slot has no record. */
/* 1 when a build order since `from` sends slot `slot` beyond its reach: the abandon's compare (impai_brain.c,
   THE REACH ABANDON) on the fake's builddistance, 120 px, and footprints */
static int far_builds(int from, int slot)
{
    const FUNIT* u = &s_w.unit[slot];
    const int bf = s_w.defs[u->type].d.fx, bz = s_w.defs[u->type].d.fz;
    int k, n = 0;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        double dx, dz, gap;
        int sf, sz;
        if (c->verb != WQ_CMD_NEW || c->unit != slot || !c->type) continue;
        sf = s_w.defs[c->type].d.fx;
        sz = s_w.defs[c->type].d.fz;
        dx = (double)(c->pos[0] >> 16) - (double)(u->pos[0] >> 16);
        dz = (double)(c->pos[2] >> 16) - (double)(u->pos[2] >> 16);
        gap = sqrt(dx * dx + dz * dz) - 8.0 * sqrt((double)(bf * bf + bz * bz)) - 8.0 * sqrt((double)(sf * sf + sz * sz));
        n += gap > 122.0;           /* the brain truncates each term: within 2 px of this */
    }
    return n;
}

static void check_far(void)
{
    IMPAI_BRAIN_OUT out;
    int from, others = 0;
    brain_world();
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    unit_add(&s_w, 2, T_CONS, 30, 40, 1);
    s_job[2].type = T_CONS;
    s_job[2].build = T_MEX;
    s_job[2].target = (uint16_t)WQ_NONE;
    s_job[2].cx = 40;
    s_job[2].cz = 20;
    s_job[2].kind = IMPAI_JOB_BUILD;
    s_job[2].since = 100;
    s_st.ax = 20;
    s_st.az = 20;
    s_w.gametime = 400;
    s_w.econ[0].energy = 900;
    CHECK(decide(&out) == 0, "decide");
    CHECK(out.failed == 0 && s_st.nfail == 0, "an unreachable site fails nothing: failed %u nfail %u", out.failed,
          s_st.nfail);
    CHECK(s_job[2].far_cx == 40 && s_job[2].far_cz == 20 && s_job[2].far_type == T_CONS &&
          s_job[2].far_until == 400u + s_plan->fail_t, "the builder's record: %d,%d until %u", s_job[2].far_cx,
          s_job[2].far_cz, s_job[2].far_until);
    for (from = 0; from < s_w.nlog; from++) {
        const WQ_CMD* w = &s_w.log[from].c;
        CHECK(!(w->unit == 2 && w->type == T_MEX && cmd_x0(w) >= 39 && cmd_x0(w) <= 42 && cmd_z0(w) <= 22),
              "the constructor is sent back to patch A");
        if (w->unit != 2 && w->type == T_MEX && cmd_x0(w) >= 39 && cmd_x0(w) <= 42 && cmd_z0(w) <= 22) others++;
    }
    CHECK(others == 1, "a builder within reach takes patch A: %d orders there", others);
    CHECK(!far_builds(0, 2), "nor anywhere else beyond its reach: patch C is as far");
    /* no cycling: the record holds the constructor back from every site beyond its reach, decision after decision,
       and none of them fails */
    for (from = 0; from < 3; from++) {
        const int at = s_w.nlog;
        s_w.gametime += 15;
        CHECK(decide(&out) == 0, "decide");
        CHECK(!far_builds(at, 2) && s_st.nfail == 0, "decision %d: no site beyond the constructor's reach", from);
    }
    unit_add(&s_w, 2, T_COM, 30, 40, 1);
    s_w.gametime = 430;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_job[2].far_until == 0, "a unit of another type in the slot has no record");
}

/* THE FAR RECORD AND THE PLANT: a constructor in slot 1 whose record is live and the commander in slot 2, both
   idle, the opening factory wanted. The constructor comes first in slot order and stands within its reach of the
   ring's first sites, but a builder with a live record takes no plant: the commander, which can walk, does. The
   constructor far out instead: no site beyond its reach either. The constructor near with a record restored past
   fail_t of now (record_live): no record, and it takes the plant. */
static void check_far_plant(void)
{
    IMPAI_BRAIN_OUT out;
    int k, cons = 0, com = 0;
    brain_world();
    s_spots = NULL;
    s_nspots = 0;
    for (k = 0; k < 3; k++) {
        const int from = s_w.nlog;
        int i;
        cons = com = 0;
        unit_add(&s_w, 1, T_CONS, k == 1 ? 60 : 24, k == 1 ? 60 : 26, 1);
        unit_add(&s_w, 2, T_COM, 20, 20, 1);
        impai_brain_reset(&s_st, s_job, FW_SLOTS);
        memset(s_pref, 0, sizeof s_pref);
        s_job[1].far_type = T_CONS;
        s_st.ax = 20;
        s_st.az = 20;
        s_w.gametime = 2000 + 15 * k;
        s_job[1].far_until = (uint32_t)s_w.gametime + s_plan->fail_t + (k == 2 ? 1u : 0u);
        s_w.econ[0].energy = 900;
        CHECK(decide(&out) == 0, "decide");
        for (i = from; i < s_w.nlog; i++)
            if (s_w.log[i].c.verb == WQ_CMD_NEW && s_w.log[i].c.type == T_FACTORY) {
                cons += s_w.log[i].c.unit == 1;
                com += s_w.log[i].c.unit == 2;
            }
        printf("far plant, the constructor %s: its factory orders %d, the commander's %d\n",
               k == 2 ? "near, its record past fail_t" : k ? "far" : "near", cons, com);
        if (k == 2) {
            CHECK(cons == 1 && !com, "a record past fail_t of now holds nothing: the constructor takes the plant");
            continue;
        }
        CHECK(!cons && com == 1, "the plant goes to the builder that can walk");
        CHECK(!far_builds(from, 1), "and the constructor takes no site beyond its reach");
    }
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* THE CLAIM TABLE'S BOUND: a seat's whole block of IMPAI_CENSUS_MAX units, each with a claim of its own -- 100
   constructors with a build order at their head and the rest extractors -- and IMPAI_WAYS kept ways of
   IMPAI_WAY_RECTS rectangles, each claimed while its factory stands, then a decision's own orders: the table is
   never full. */
static void check_claims_bound(void)
{
    const IMPAI_DEC* dd = (const IMPAI_DEC*)s_scratch;
    IMPAI_BRAIN_OUT out;
    const int cons = 100, ways = IMPAI_WAYS, mexes = IMPAI_CENSUS_MAX - 1 - IMPAI_WAYS - 100;
    int k, j, slot = 1;
    brain_world();
    s_w.first[0] = 1;
    s_w.last[0] = IMPAI_CENSUS_MAX;
    s_w.first[1] = IMPAI_CENSUS_MAX + 1;
    s_w.last[1] = IMPAI_CENSUS_MAX + 2;
    unit_add(&s_w, slot++, T_COM, 20, 20, 1);
    for (k = 0; k < ways; k++) {
        IMPAI_WAYREC* w = &s_st.way[k];
        unit_add(&s_w, slot++, T_FACTORY, 2 + 7 * (k % 8), 70 + 7 * (k / 8), 1);
        w->type = T_FACTORY;
        w->cx = (int16_t)(2 + 7 * (k % 8));
        w->cz = (int16_t)(70 + 7 * (k / 8));
        w->n = IMPAI_WAY_RECTS;
        for (j = 0; j < IMPAI_WAY_RECTS; j++) {
            w->r[j][0] = (int16_t)(w->cx + j); w->r[j][1] = (int16_t)(w->cz + 6);
            w->r[j][2] = (int16_t)(w->cx + j); w->r[j][3] = (int16_t)(w->cz + 6);
        }
    }
    s_st.nway = (uint8_t)ways;
    for (k = 0; k < cons; k++) {
        FUNIT* u = &s_w.unit[slot];
        unit_add(&s_w, slot++, T_CONS, 4 + k % 50, 100 + k / 50, 1);
        u->list[0].action = A_MOBILEBUILD;
        u->list[0].type = T_SOLAR;
        u->list[0].target = (uint16_t)WQ_NONE;
        u->list[0].pos[0] = (int32_t)((2 * (4 + k % 50) + 4) << 19);
        u->list[0].pos[2] = (int32_t)((2 * 110 + 4) << 19);
        u->n = 1;
    }
    for (k = 0; k < mexes; k++) unit_add(&s_w, slot++, T_MEX, 3 * (k % 40), 3 * (k / 40) % 120, 1);
    s_st.ax = 20;
    s_st.az = 20;
    s_w.gametime = 900;
    CHECK(slot == IMPAI_CENSUS_MAX + 1, "the block is full: %d", slot - 1);
    CHECK(decide(&out) == 0 && out.census == IMPAI_CENSUS_MAX, "the census counts the whole block: %u", out.census);
    CHECK(!dd->claims_full && dd->nclaims >= cons + mexes + ways * IMPAI_WAY_RECTS && dd->nclaims <= IMPAI_CLAIM_MAX,
          "the table holds every claim: %d of %d", dd->nclaims, IMPAI_CLAIM_MAX);
    CHECK(s_st.nway == ways, "every kept way stays: %d", s_st.nway);
    printf("claims: %d of %d, full %d\n", dd->nclaims, IMPAI_CLAIM_MAX, dd->claims_full);
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
          s_st.fail[0].until == 400u + s_plan->fail_t, "the site fails until GameTime + fail_t");
    for (i = 0; i < s_w.nlog; i++) {
        const WQ_CMD* c = &s_w.log[i].c;
        if (c->verb == WQ_CMD_NEW && c->type && 26 >= cmd_x0(c) && 26 < cmd_x0(c) + s_w.defs[c->type].d.fx &&
            20 >= cmd_z0(c) && 20 < cmd_z0(c) + s_w.defs[c->type].d.fz)
            covered++;
    }
    CHECK(!covered, "an order covers the failed cell");
}

/* THE RECLAIM (impai_build.c): a reclaimable feature of the fake's at anchor cell (ax, az), fx x fz */
static int feat_add(int ax, int az, int fx, int fz, float metal)
{
    const int k = s_w.nfeat;
    if (k >= FW_FEATS) return -1;
    memset(&s_w.feat[k], 0, sizeof s_w.feat[k]);
    s_w.feat[k].ax = (int16_t)ax; s_w.feat[k].az = (int16_t)az;
    s_w.feat[k].fx = (int16_t)fx; s_w.feat[k].fz = (int16_t)fz;
    s_w.feat[k].def = (uint16_t)(10 + k);
    s_w.feat[k].flags = 0x80;
    s_w.feat[k].live = 1;
    s_w.feat[k].owner = 10;
    s_w.feat[k].metal = metal;
    s_w.nfeat = k + 1;
    return k;
}

/* A seat of a commander and a constructor that can reclaim, both moving a px a tick, a factory standing and
   busy, no patch and another unit exploring: so no rule that builds has a site, and the builders reach the
   reclaim. `stall`: the seat's metal is short (m_stall: under a fifth of its storage, spending more than it
   makes). */
static void reclaim_world(int stall)
{
    FUNIT* f;
    brain_world();
    s_spots = NULL;
    s_nspots = 0;
    s_w.defs[T_COM].d.flags |= WQ_DF_RECLAIMER;
    s_w.defs[T_CONS].d.flags |= WQ_DF_RECLAIMER;
    s_w.defs[T_COM].d.speed = s_w.defs[T_CONS].d.speed = 1 << 16;
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    unit_add(&s_w, 2, T_CONS, 22, 30, 1);
    unit_add(&s_w, 3, T_FACTORY, 6, 6, 1);
    unit_add(&s_w, 9, T_CONS, 50, 50, 1);
    f = &s_w.unit[3];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    f = &s_w.unit[9];
    f->list[0].action = A_MOVE;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->n = 1;
    s_job[9].type = T_CONS;
    s_job[9].build = s_job[9].target = (uint16_t)WQ_NONE;
    s_job[9].kind = IMPAI_JOB_EXPLORE;
    s_job[9].cx = s_job[9].cz = 120;
    s_job[9].since = 890;
    s_st.explorer = 9;
    s_st.ax = 20;
    s_st.az = 20;
    s_w.econ[0].metal = stall ? 50.0f : 900.0f;
    s_w.econ[0].metal_in = 1.0f;
    s_w.econ[0].metal_out = 6.0f;
    s_w.gametime = 900;
}

static int reclaim_orders(int unit)
{
    int i, n = 0;
    for (i = 0; i < s_w.nlog; i++) n += s_w.log[i].c.action == A_RECLAIM && (!unit || s_w.log[i].c.unit == unit);
    return n;
}

/* the feature an order names: its index in the fake, by the cell of the order's position */
static int reclaim_feat_of(const WQ_CMD* c)
{
    const int x = c->pos[0] >> 20, z = c->pos[2] >> 20;
    int k;
    for (k = 0; k < s_w.nfeat; k++)
        if (x >= s_w.feat[k].ax && x < s_w.feat[k].ax + s_w.feat[k].fx && z >= s_w.feat[k].az &&
            z < s_w.feat[k].az + s_w.feat[k].fz)
            return k;
    return -1;
}

/* the seat remembers feature k as shown at GameTime `seen`, and the draw does not show it now: a nodrawundergray def
   (WQ_FEAT.gated) of no owner, both of its draw's corner blocks out of the seat's sight. Lighting the anchor's block
   (s_w.dark) shows it again. */
static void reclaim_remember(int k, uint32_t seen)
{
    IMPAI_FMEM* e = &s_st.fmem[s_st.nfmem++];
    s_w.feat[k].gated = 1;
    e->ax = s_w.feat[k].ax; e->az = s_w.feat[k].az;
    e->def = s_w.feat[k].def;
    e->fx = (uint8_t)s_w.feat[k].fx; e->fz = (uint8_t)s_w.feat[k].fz;
    e->gated = 1;
    e->owner = s_w.feat[k].owner;
    e->metal = s_w.feat[k].metal;
    e->seen = seen;
    s_w.dark[s_w.feat[k].az >> 1][s_w.feat[k].ax >> 1] = 1;
    s_w.dark[(s_w.feat[k].az + s_w.feat[k].fz) >> 1][(s_w.feat[k].ax + s_w.feat[k].fx) >> 1] = 1;
}

/* a builder's head at the centre of feature k, as the reclaim order leaves it */
static void reclaim_head(int slot, int action, int k)
{
    FUNIT* u = &s_w.unit[slot];
    memset(&u->list[0], 0, sizeof u->list[0]);
    u->list[0].action = (uint8_t)action;
    u->list[0].target = (uint16_t)WQ_NONE;
    u->list[0].pos[0] = (int32_t)((uint32_t)(16 * s_w.feat[k].ax + 8 * s_w.feat[k].fx) << 16);
    u->list[0].pos[2] = (int32_t)((uint32_t)(16 * s_w.feat[k].az + 8 * s_w.feat[k].fz) << 16);
    u->n = 1;
}

/* the commander's reclaim job on feature k, ordered at `since` */
static void reclaim_job_set(int k, uint32_t since)
{
    s_job[1].type = T_COM;
    s_job[1].build = s_job[1].target = (uint16_t)WQ_NONE;
    s_job[1].cx = s_w.feat[k].ax;
    s_job[1].cz = s_w.feat[k].az;
    s_job[1].feat = s_w.feat[k].def;
    s_job[1].kind = IMPAI_JOB_RECLAIM;
    s_job[1].since = since;
}

/* the extractor orders the decisions gave unit `unit` */
static int unit_mex_orders(int unit)
{
    int i, n = 0;
    for (i = 0; i < s_w.nlog; i++) n += s_w.log[i].c.unit == unit && s_w.log[i].c.type == T_MEX;
    return n;
}

/* the feature of unit `unit`'s last reclaim order, -1 none */
static int reclaimed_by(int unit)
{
    int i, k = -1;
    for (i = 0; i < s_w.nlog; i++)
        if (s_w.log[i].c.unit == unit && s_w.log[i].c.action == A_RECLAIM) k = reclaim_feat_of(&s_w.log[i].c);
    return k;
}

/* THE COMMANDER'S SAFE DISTANCE beyond the ring (impai_build.c): an anti-air turret -- armed by its threat row,
   dps_air only, so no ground threat map refuses anything -- seen by the commander's seat.
   - THE ASSIST: the commander at the anchor (64, 64) with a comm_r of 64 px and two solar frames, A 18 px off and
     B 62 px off. The turret 573 px from A and 644 px from B: the commander helps B. None seen: A, the nearer.
   - THE RECLAIM: the reclaim world's commander, a 20-metal rock A beside it and another, B, 361 px from the anchor.
     The turret 1 056 px from A: the commander takes A, the nearer. The turret 385 px from A and 659 px from B:
     the commander takes B.
   - THE ARRIVAL: the commander walking to a remembered 300-metal rock, which comes into sight with the turret
     302 px from it: the walk ends with no reclaim, no failure and no far record.
   - THE SPOT: the opening's far patches with the turret 532 px from the poor one and 713 px from the rich: the
     commander walks to the rich one. The turret 554 px from the poor one and 373 px from the rich, 1 640 px from
     the anchor -- past the decision's list, which reaches comm_r + IMPAI_CMD_SAFE_PX + 1 = 1 625 px: neither. */
static void check_cmd_safe_orders(void)
{
    static IMPAI_ROW rows[FW_SLOTS];
    IMPAI_THREAT th[FW_DEFS];
    IMPAI_VIEW v;
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    int k, seen, target, far, n;
    memset(th, 0, sizeof th);
    th[T_TURRET].dps_air = 10;
    for (seen = 0; seen < 2; seen++) {
        helps_world(10, 10, 0);
        unit_add(&s_w, 1, T_COM, 64, 64, 1);
        s_st.ax = s_st.az = 64;
        unit_add(&s_w, 5, T_SOLAR, 61, 62, 0);
        unit_add(&s_w, 6, T_SOLAR, 65, 60, 0);
        ring_view(&v, rows, th, seen ? T_TURRET : (int)T_CONS, IMPAI_LOS, 15, 39);
        s_viewp = &v;
        s_w.econ[0].metal = 500.0f;
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        s_plan = &IMPAI_PLAN_ROW;
        for (k = 0, target = -1; k < s_w.nlog; k++)
            if (s_w.log[k].c.unit == 1 && s_w.log[k].c.action == A_HELPBUILD) target = s_w.log[k].c.target;
        printf("commander's safe distance, the assist, %s: helps %d\n", seen ? "the turret seen" : "none seen",
               target);
        CHECK(target == (seen ? 6 : 5), "the assist, %s: the commander helps %d", seen ? "the turret seen" : "none seen",
              target);
    }
    for (seen = 0; seen < 2; seen++) {
        int a, b;
        reclaim_world(1);
        a = feat_add(22, 22, 2, 2, 20);
        b = feat_add(10, 40, 2, 2, 20);
        ring_view(&v, rows, th, T_TURRET, IMPAI_LOS, seen ? 23 : 44, 10);
        s_viewp = &v;
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        for (k = 0, target = -1; k < s_w.nlog; k++)
            if (s_w.log[k].c.unit == 1 && s_w.log[k].c.action == A_RECLAIM) target = reclaim_feat_of(&s_w.log[k].c);
        printf("commander's safe distance, the reclaim, the turret %s: the commander reclaims %s\n",
               seen ? "385 px from A" : "1 056 px from A", target == a ? "A" : target == b ? "B" : "nothing");
        CHECK(target == (seen ? b : a), "the reclaim, the turret %s: the commander reclaims feature %d",
              seen ? "385 px from A" : "1 056 px from A", target);
    }
    reclaim_world(1);
    far = feat_add(30, 30, 2, 2, 300);
    reclaim_remember(far, 100);
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_job[1].kind == IMPAI_JOB_RECLAIM_GO && s_job[1].cx == 30, "the commander walks to the remembered rock");
    n = s_w.nlog;
    s_w.dark[15][15] = 0;
    s_w.gametime = 915;
    ring_view(&v, rows, th, T_TURRET, IMPAI_LOS, 23, 10);
    s_viewp = &v;
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    for (k = n, seen = 0; k < s_w.nlog; k++) seen += s_w.log[k].c.unit == 1 && reclaim_feat_of(&s_w.log[k].c) == far;
    printf("commander's safe distance, the arrival: job %u, far until %u, orders at the rock %d, failed %u\n",
           s_job[1].kind, s_job[1].far_until, seen, out.failed);
    CHECK(s_job[1].kind != IMPAI_JOB_RECLAIM_GO && s_job[1].kind != IMPAI_JOB_RECLAIM && !s_job[1].far_until &&
          !seen && out.failed == 0, "the arrival: job %u, far until %u, %d orders at the rock", s_job[1].kind,
          s_job[1].far_until, seen);
    far_patch_world(0, 0);
    ring_view(&v, rows, th, T_TURRET, IMPAI_LOS, 13, 13);
    s_viewp = &v;
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    c = last_cmd(0);
    printf("commander's safe distance, the far patch, the turret near the poor one: %d,%d\n",
           c && c->c.type == T_MEX ? cmd_x0(&c->c) : -1, c && c->c.type == T_MEX ? cmd_z0(&c->c) : -1);
    CHECK(c && c->c.unit == 1 && c->c.type == T_MEX && cmd_x0(&c->c) >= 56 && cmd_z0(&c->c) >= 56,
          "the far patch, the turret near the poor one: the commander walks to the rich one");
    far_patch_world(0, 0);
    ring_view(&v, rows, th, T_TURRET, IMPAI_LOS, 37, 37);
    s_viewp = &v;
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    printf("commander's safe distance, the far patch, the turret past the list: extractor orders %d\n", mex_orders());
    CHECK(!mex_orders(), "the far patch, the turret 1 640 px from the anchor: %d extractor orders", mex_orders());
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* THE RECLAIM'S ECONOMY (impai_econ.c, THE LADDER; impai_build.c, THE WORTH): once the first extractors are up, a
   feature that pays more metal a tick than an extractor makes wins over the extractor's site, and a poor far one
   does not; a feature is worth no more than the store's room, so a nearer small rock beats a rich one the store
   cannot hold, and each order takes its metal off the room for the next builder; a feature playing its sequence
   is never taken, and a job whose feature starts playing one -- a paid reclaim's -- ends with no record and its
   builder takes its next task in the same decision; a builder whose head another hand replaced ends its job. */
static void check_reclaim_economy(void)
{
    IMPAI_BRAIN_OUT out;
    int i, a, small, big;

    /* two finished extractors and a metal income of 1 a pass: an extractor makes 1 / 2 / 30 metal a tick */
    reclaim_world(1);
    s_spots = PATCHES;
    s_nspots = NPATCH;
    unit_add(&s_w, 4, T_MEX, 10, 45, 1);
    unit_add(&s_w, 5, T_MEX, 52, 10, 1);
    a = feat_add(22, 22, 2, 2, 100);
    CHECK(decide(&out) == 0, "decide");
    CHECK(reclaimed_by(1) == a && !unit_mex_orders(1), "a rich rock beside the commander waits for an extractor: "
          "feature %d, %d extractors", reclaimed_by(1), unit_mex_orders(1));

    reclaim_world(1);
    s_spots = PATCHES;
    s_nspots = NPATCH;
    unit_add(&s_w, 4, T_MEX, 10, 45, 1);
    unit_add(&s_w, 5, T_MEX, 52, 10, 1);
    feat_add(60, 20, 1, 1, 5);              /* 640 px off: 5 metal in about 540 ticks */
    CHECK(decide(&out) == 0, "decide");
    CHECK(!reclaim_orders(1) && unit_mex_orders(1) == 1, "a poor far rock wins over the extractor: %d reclaims, "
          "%d extractors", reclaim_orders(1), unit_mex_orders(1));

    /* a store with room for 300: a 5 000-metal rock beside the commander is worth 300 over its 2 516 ticks, a
       100-metal one farther off 100 over about 180 */
    reclaim_world(1);
    s_w.econ[0].metal_max = 350.0f;
    big = feat_add(22, 22, 2, 2, 5000);
    small = feat_add(16, 34, 2, 2, 100);
    CHECK(decide(&out) == 0, "decide");
    CHECK(reclaimed_by(1) == small, "the commander takes the small rock, not the %d-metal one the store cannot "
          "hold: feature %d", (int)s_w.feat[big].metal, reclaimed_by(1));

    /* room for 100: the first builder's order fills it */
    reclaim_world(1);
    s_w.econ[0].metal_max = 150.0f;
    feat_add(22, 22, 2, 2, 100);
    feat_add(23, 32, 2, 2, 100);
    CHECK(decide(&out) == 0, "decide");
    CHECK(reclaim_orders(0) == 1, "two builders reclaim into a store with room for one rock: %d orders",
          reclaim_orders(0));

    /* a rock playing its sequence */
    reclaim_world(1);
    a = feat_add(22, 22, 2, 2, 100);
    s_w.feat[a].playing = 1;
    CHECK(decide(&out) == 0, "decide");
    for (i = 0; i < s_w.nlog; i++)
        CHECK(s_w.log[i].c.action != A_RECLAIM || reclaim_feat_of(&s_w.log[i].c) != a,
              "a feature playing its sequence is taken");

    /* the commander's reclaim paid: its rock plays its sequence, its head is the engine's next, at no point; the
       constructor busy guarding the commander */
    reclaim_world(1);
    big = feat_add(30, 30, 2, 2, 300);
    feat_add(22, 22, 2, 2, 20);
    reclaim_job_set(big, 850);
    reclaim_head(2, A_GUARD, big);
    s_w.unit[2].list[0].target = 1;
    reclaim_head(1, A_STOP, big);
    s_w.unit[1].list[0].pos[0] = s_w.unit[1].list[0].pos[2] = 0;
    s_w.feat[big].playing = 1;
    CHECK(decide(&out) == 0, "decide");
    CHECK(!s_job[1].far_until && s_job[1].kind == IMPAI_JOB_RECLAIM && s_job[1].cx == 22 && s_job[1].cz == 22,
          "a paid reclaim ends with no record, the commander on to the next rock: kind %u at %d,%d, far until %u",
          s_job[1].kind, s_job[1].cx, s_job[1].cz, s_job[1].far_until);

    /* the commander's reclaim replaced by another hand's move */
    reclaim_world(1);
    big = feat_add(30, 30, 2, 2, 300);
    reclaim_job_set(big, 850);
    reclaim_head(1, A_MOVE, big);
    s_w.unit[1].list[0].pos[0] = s_w.unit[1].list[0].pos[2] = (int32_t)(80u << 16);
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_job[1].kind == IMPAI_JOB_NONE && !s_job[1].far_until && !reclaim_orders(1),
          "a replaced reclaim ends with no record and its builder left to its order: kind %u, far until %u",
          s_job[1].kind, s_job[1].far_until);
    printf("reclaim economy: ok\n");
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* THE RECLAIM: a metal-short seat's idle builders take the features of most metal a tick, each its own; a
   builder at work on one is left to it; a feature seen gone ends the job; one still there when its builder is
   idle is that builder's alone to avoid; a full store reclaims nothing, nor does a seat with the income it wants
   and no extractor of its own; the first extractors' sites win over a feature; a feature on unexplored ground is
   never taken; a remembered rock reclaimed out of sight is walked to, seen gone and forgotten by the whole seat,
   its builder given its next task in the same decision; a rich rock seen long ago loses to a poorer, nearer one
   in sight. No cell out of the seat's sight is ever read. */
/* THE HARVEST (impai_econ.c, THE LADDER) and THE ARMY'S LOOK (impai_build.c, THE FEATURE MEMORY). Each world is
   reclaim_world's with a second factory standing busy, so no plant is wanted.
   (a) the seat not short of metal -- 30 a pass against the 8 a factory wants --, its store at half its storage and
       its energy paid: the commander takes the 60-metal rock beside it, the one reclaim of the decision;
   (b) the same with the store at 0.9 of its storage, a surplus: no reclaim;
   (c) short of metal, the map explored, a 600-metal wreck at cell (100, 40) -- 1 258 px from the constructor and
       816 from the explorer, past every builder's look -- and a mobile army unit at (98, 44): the army's look
       remembers it and the constructor, within IMPAI_HARVEST_R, takes it; the army unit's slot is the cursor;
   (d) as (c) without the army unit: nothing remembers the wreck and no builder reclaims.
   THE HELD GROUND of the harvest (check_harvest_held): short of metal, the map explored, a 600-metal rock at cell
   (60, 30), in the constructor's own look and in 256-px cell (3, 1), two steps from the anchor's:
   (e) the constructor alone, an enemy's armed row in cell (5, 1), two steps off the rock and its 100-px reach far
       from it and from the way: the rock's cell is not held, and the constructor does not take it;
   (e') the commander alone, the same row: the commander, which keeps comm_r, takes it;
   (f) the constructor alone, no enemy row: it takes it. */
static void route_row(unsigned how, int bx, int bz, int range);

static void harvest_world(int k)
{
    FUNIT* f;
    int x, z;
    reclaim_world(k >= 2);
    unit_add(&s_w, 4, T_FACTORY, 12, 6, 1);
    f = &s_w.unit[4];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    s_w.econ[0].energy = 500.0f;
    s_w.econ[0].energy_in = 2000.0f;
    s_w.econ[0].energy_out = 0.0f;
    if (k < 2) {
        s_w.econ[0].metal = k ? 900.0f : 500.0f;
        s_w.econ[0].metal_in = 30.0f;
        s_w.econ[0].metal_out = 6.0f;
        feat_add(22, 22, 2, 2, 60);
        return;
    }
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    feat_add(100, 40, 2, 2, 600);
    s_ti[T_TURRET].flags |= IMPAI_TF_MOBILE;
    if (k == 2) unit_add(&s_w, 12, T_TURRET, 98, 44, 1);
}

/* the harvest's held ground: `foe` an enemy row in cell (5, 1); `who` 0 the constructor alone, 1 the commander */
static IMPAI_MIL s_hvmil;

static void harvest_held_world(int foe, int who)
{
    int x, z;
    reclaim_world(1);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    feat_add(60, 30, 2, 2, 600);
    s_w.unit[who ? 2 : 1].live = 0;
    impai_mil_reset(&s_hvmil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
    s_hvmil.ebx = s_hvmil.ebz = IMPAI_NO_ANCHOR;
    s_milp = &s_hvmil;
    route_row(foe ? IMPAI_LOS : 0, 5 * 8 + 2, 10, 100);
    s_ti[T_TURRET].flags |= IMPAI_TF_MOBILE;
}

static void check_harvest_held(void)
{
    static const char* const NAME[3] = { "the constructor, the rock's cell not held", "the commander, the same",
                                         "the constructor, no enemy row" };
    IMPAI_BRAIN_OUT out;
    int k;
    for (k = 0; k < 3; k++) {
        const int who = k == 1, slot = who ? 1 : 2;
        int got;
        harvest_held_world(k < 2, who);
        CHECK(decide(&out) == 0, "decide");
        s_milp = NULL;
        s_viewp = NULL;
        got = reclaim_orders(slot) + (s_job[slot].kind == IMPAI_JOB_RECLAIM_GO);
        printf("harvest's held ground, %s: takes %d\n", NAME[k], got);
        CHECK(k == 0 ? got == 0 : got == 1, "%s: takes %d", NAME[k], got);
    }
    s_ti[T_TURRET].flags &= ~IMPAI_TF_MOBILE;
}

static void check_harvest(void)
{
    static const char* const NAME[] = { "not short, half full", "a surplus", "a far wreck the army sees",
                                        "a far wreck nothing sees" };
    IMPAI_BRAIN_OUT out;
    int k;
    for (k = 0; k < 4; k++) {
        harvest_world(k);
        CHECK(decide(&out) == 0, "decide");
        printf("harvest, %s: reclaims %d (commander %d, constructor %d), remembered %u, army look %u\n", NAME[k],
               reclaim_orders(0) + (s_job[2].kind == IMPAI_JOB_RECLAIM_GO), reclaim_orders(1),
               reclaim_orders(2) + (s_job[2].kind == IMPAI_JOB_RECLAIM_GO), (unsigned)s_st.nfmem,
               (unsigned)s_st.ascan);
        if (k == 0) CHECK(reclaim_orders(1) == 1 && reclaim_orders(0) == 1, "%s: the commander harvests", NAME[k]);
        if (k == 1) CHECK(reclaim_orders(0) == 0, "%s: no harvest", NAME[k]);
        if (k == 2) CHECK((reclaim_orders(2) == 1 || s_job[2].kind == IMPAI_JOB_RECLAIM_GO) && s_st.ascan == 12,
                          "%s: the constructor goes for it", NAME[k]);
        if (k == 3) CHECK(reclaim_orders(0) == 0 && s_job[2].kind != IMPAI_JOB_RECLAIM_GO && s_st.nfmem == 0,
                          "%s: no reclaim", NAME[k]);
    }
}

/* THE STALL TAKES THE RECLAIMERS (impai_econ.c): reclaim_world's metal stall, both builders sent to reclaim at 900;
   at 915 the energy store at 10 % of its storage, an energy stall: each reclaimer is given the wind generator, its
   job a BUILD, and stall_taken counts those whose job was an ordered reclaim (IMPAI_JOB_RECLAIM), not a walk to one
   (RECLAIM_GO), which ordered none. With the commander's job given at 915 itself (THE RECLAIM'S CHECK's
   order on arrival) it keeps its reclaim and the constructor alone is taken; with the store at 50 %, no stall,
   both keep reclaiming. */
static void check_stall_reclaimers(void)
{
    static const char* const NAME[3] = { "an energy stall", "the commander's job given now", "no stall" };
    IMPAI_BRAIN_OUT out;
    int k, i, n, wind[3], ordered[3];
    for (k = 0; k < 3; k++) {
        reclaim_world(1);
        feat_add(22, 22, 2, 2, 20);
        feat_add(30, 30, 2, 2, 300);
        CHECK(decide(&out) == 0, "decide");
        CHECK(reclaim_orders(1) == 1 && reclaim_orders(2) == 1, "%s: both builders reclaim first", NAME[k]);
        for (i = 1; i < 3; i++) ordered[i] = s_job[i].kind == IMPAI_JOB_RECLAIM;
        n = s_w.nlog;
        s_w.gametime = 915;
        s_w.econ[0].energy = k == 2 ? 500.0f : 100.0f;
        if (k == 1) s_job[1].since = 915;
        CHECK(decide(&out) == 0, "decide");
        for (i = 0; i < 3; i++) wind[i] = 0;
        for (i = n; i < s_w.nlog; i++)
            if (s_w.log[i].c.type == T_WIND && s_w.log[i].c.unit <= 2) wind[s_w.log[i].c.unit]++;
        printf("the stall's reclaimers, %s: wind commander %d constructor %d, stall_taken %u, ordered %d %d, jobs %u %u\n",
               NAME[k], wind[1], wind[2], out.stall_taken, ordered[1], ordered[2], s_job[1].kind, s_job[2].kind);
        CHECK(wind[1] == (k == 0) && wind[2] == (k < 2) &&
              out.stall_taken == (unsigned)((wind[1] && ordered[1]) + (wind[2] && ordered[2])),
              "%s: the reclaimers given the stall's energy", NAME[k]);
        CHECK((s_job[1].kind == IMPAI_JOB_BUILD) == (k == 0) && (s_job[2].kind == IMPAI_JOB_BUILD) == (k < 2),
              "%s: the jobs", NAME[k]);
    }
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* THE HARVEST'S COST (impai_brain.c, census: IMPAI_BRAIN_OUT.harvest_lost): reclaim_world, and jobs on slots the
   census meets -- slot 20 empty with a RECLAIM job and slot 3 (the factory) holding a constructor's RECLAIM job
   count 2; slot 21 empty with a RECLAIM_GO job and slot 2 (the constructor) holding a factory's RECLAIM_GO job,
   walks that ordered no reclaim, an empty slot with a BUILD job and one with an EXPLORE count none; each job is
   cleared, so the next decision counts 0. */
static void check_harvest_lost(void)
{
    static const uint8_t KIND[6] = { IMPAI_JOB_RECLAIM, IMPAI_JOB_RECLAIM_GO, IMPAI_JOB_RECLAIM, IMPAI_JOB_BUILD,
                                     IMPAI_JOB_EXPLORE, IMPAI_JOB_RECLAIM_GO };
    static const uint16_t SLOT[6] = { 20, 21, 3, 22, 23, 2 };
    IMPAI_BRAIN_OUT out;
    int k;
    reclaim_world(1);
    for (k = 0; k < 6; k++) {
        IMPAI_JOB* j = &s_job[SLOT[k]];
        j->type = k == 5 ? T_FACTORY : T_CONS;
        j->build = j->target = (uint16_t)WQ_NONE;
        j->kind = KIND[k];
        j->cx = j->cz = 30;
        j->since = 800;
    }
    CHECK(decide(&out) == 0, "decide");
    printf("the harvest's cost: harvest_lost %u\n", out.harvest_lost);
    CHECK(out.harvest_lost == 2, "two builders gone on an ordered reclaim: %u", out.harvest_lost);
    for (k = 0; k < 5; k++) CHECK(s_job[SLOT[k]].kind == IMPAI_JOB_NONE, "the gone builder's job %d cleared", k);
    CHECK(s_job[2].type != T_FACTORY, "the replaced slot's job cleared");
    s_w.gametime += 15;
    CHECK(decide(&out) == 0, "decide");
    CHECK(out.harvest_lost == 0, "counted once: %u", out.harvest_lost);
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* WHAT THE FEATURE DRAW SHOWS THE SEAT (tasim_sweep.c, tasim_feature_drawn) on brain_world, explored for x, z < 64:
   a feature on explored ground whose block is dark is shown, one on unexplored ground is not; a nodrawundergray
   def (gated) of no owner with both corner blocks dark is not, its owner's is, and one whose far corner's block
   is lit is. THE SHEAR: at a height byte of 64 a corner's row moves up 32 px, a block: with the corners' sheared
   blocks dark and the unsheared ones lit, none; the first sheared block lit, shown. THE ROW ABOVE THE GRID: an
   anchor on row 0 at height 64, its far corner's sheared block (6, 0) dark: the first corner's sheared row is -1,
   so it reads its own row, block (5, 0) -- lit, shown; dark, not. */
static void check_feature_drawn(void)
{
    WORLDQ q = { &FAKE, &s_w, 0 };
    WQ_FEAT f;
    brain_world();
    memset(&f, 0, sizeof f);
    f.ax = f.az = 10;
    f.fx = f.fz = 2;
    f.owner = 10;
    s_w.dark[5][5] = s_w.dark[6][6] = 1;
    CHECK(tasim_feature_drawn(&q, &f), "a feature on explored ground out of sight is shown");
    f.ax = 70;
    CHECK(!tasim_feature_drawn(&q, &f), "a feature on unexplored ground is not");
    f.ax = 10;
    f.gated = 1;
    CHECK(!tasim_feature_drawn(&q, &f), "a gated feature of no owner, both corners dark, is not");
    f.owner = 0;
    CHECK(tasim_feature_drawn(&q, &f), "a gated feature of the seat's is");
    f.owner = 10;
    s_w.dark[6][6] = 0;
    CHECK(tasim_feature_drawn(&q, &f), "a gated feature whose far corner is lit is");
    brain_world();
    f.alt = 64;
    s_w.dark[4][5] = s_w.dark[5][6] = 1;
    CHECK(!tasim_feature_drawn(&q, &f), "the shear: the corners' sheared blocks dark, the own ones lit: not shown");
    s_w.dark[4][5] = 0;
    CHECK(tasim_feature_drawn(&q, &f), "the shear: the first corner's sheared block lit: shown");
    brain_world();
    f.az = 0;
    s_w.dark[0][6] = 1;
    CHECK(tasim_feature_drawn(&q, &f), "a sheared row above the grid reads the corner's own row");
    s_w.dark[0][5] = 1;
    CHECK(!tasim_feature_drawn(&q, &f), "the own row dark too: not shown");
}

/* THE FEATURE MEMORY under the draw's rule (impai_build.c), on reclaim_world's metal stall:
   (a) a 300-metal rock at (30, 30) on explored ground, its blocks dark: shown, so a builder is ordered to reclaim
       it at once -- a RECLAIM, no walk to see it;
   (b) the same rock remembered as shown at 100 and gone since, its blocks dark: the draw shows its place empty, so
       the refresh drops it and no builder walks there; remembered as a gated def of no owner, it stays a belief
       and the commander walks to it (the remembered rock's case, check_reclaim);
   (c) THE PLACE IN A FULL MEMORY: 128 remembered rocks, each shown at every decision, of 10 + i metal, and a rock
       at (24, 24): of 500 metal it takes the place of the 10-metal entry; of 5 it is not remembered; of 500 with
       the commander reclaiming the 10-metal one, it takes the 11-metal entry's place and the reclaim holds;
   (d) a rock remembered on unexplored ground at (70, 10): kept as a belief, and no unexplored cell read;
   (e) the commander's reclaim job on a rock the memory no longer holds, gone since, its blocks dark, the head at
       the anchor's cell (the memory's footprint gone with its entry): the def is taken as one the seat must see,
       so the job holds until sight or the engine ends it. */
static void check_feature_memory(void)
{
    IMPAI_BRAIN_OUT out;
    int k, i, rock;
    reclaim_world(1);
    rock = feat_add(30, 30, 2, 2, 300);
    s_w.dark[15][15] = s_w.dark[16][16] = 1;
    CHECK(decide(&out) == 0, "decide");
    printf("the feature memory, (a) shown out of sight: reclaims of it %d, jobs %u %u\n",
           (reclaimed_by(1) == rock) + (reclaimed_by(2) == rock), s_job[1].kind, s_job[2].kind);
    CHECK(reclaimed_by(1) == rock || reclaimed_by(2) == rock, "(a): the rock out of sight is reclaimed");
    for (k = 0; k < 2; k++) {
        reclaim_world(1);
        rock = feat_add(30, 30, 2, 2, 300);
        reclaim_remember(rock, 100);
        if (!k) {
            s_w.feat[rock].gated = 0;
            s_st.fmem[0].gated = 0;
        }
        s_w.feat[rock].live = 0;
        CHECK(decide(&out) == 0, "decide");
        printf("the feature memory, (b) gone %s: remembered %d, walk %d\n", k ? "gated" : "shown",
               impai_fmem_find(&s_st, 30, 30) >= 0, s_job[1].kind == IMPAI_JOB_RECLAIM_GO);
        CHECK((impai_fmem_find(&s_st, 30, 30) >= 0) == k && (s_job[1].kind == IMPAI_JOB_RECLAIM_GO) == k,
              "(b) gone, %s: %s", k ? "gated" : "shown", k ? "a belief walked to" : "dropped");
    }
    for (k = 0; k < 3; k++) {
        const float metal = k == 1 ? 5.0f : 500.0f;
        reclaim_world(1);
        for (i = 0; i < IMPAI_FEATS; i++) {
            const int f = feat_add(34 + 2 * (i % 15), 2 + 2 * (i / 15), 1, 1, 10.0f + (float)i);
            IMPAI_FMEM* e = &s_st.fmem[s_st.nfmem++];
            e->ax = s_w.feat[f].ax; e->az = s_w.feat[f].az;
            e->def = s_w.feat[f].def;
            e->fx = e->fz = 1;
            e->owner = 10;
            e->metal = s_w.feat[f].metal;
            e->seen = 100;
        }
        rock = feat_add(24, 24, 2, 2, metal);
        CHECK(rock >= 0, "the fake holds the rocks");
        s_w.econ[0].metal = 0.0f;
        if (k == 2) {
            reclaim_job_set(0, 800);
            reclaim_head(1, A_RECLAIM, 0);
        }
        CHECK(decide(&out) == 0, "decide");
        printf("the feature memory, (c) a full memory%s, a rock of %.0f: remembered %d, the 10-metal entry %d, "
               "the 11-metal %d, %u entries\n", k == 2 ? " its poorest claimed" : "", (double)metal,
               impai_fmem_find(&s_st, 24, 24) >= 0, impai_fmem_find(&s_st, 34, 2) >= 0,
               impai_fmem_find(&s_st, 36, 2) >= 0, s_st.nfmem);
        CHECK((impai_fmem_find(&s_st, 24, 24) >= 0) == (k != 1) && (impai_fmem_find(&s_st, 34, 2) >= 0) == (k != 0) &&
              (impai_fmem_find(&s_st, 36, 2) >= 0) == (k != 2) && s_st.nfmem == IMPAI_FEATS, "(c) a rock of %.0f: %s",
              (double)metal, k == 1 ? "not remembered" : k ? "the poorest unclaimed's place" : "the poorest's place");
        CHECK(k != 2 || (s_job[1].kind == IMPAI_JOB_RECLAIM && s_job[1].cx == 34), "(c) the claimed reclaim holds");
    }
    reclaim_world(1);
    s_st.fmem[0].ax = 70; s_st.fmem[0].az = 10;
    s_st.fmem[0].def = 99;
    s_st.fmem[0].fx = s_st.fmem[0].fz = 2;
    s_st.fmem[0].owner = 10;
    s_st.fmem[0].metal = 300.0f;
    s_st.fmem[0].seen = 100;
    s_st.nfmem = 1;
    CHECK(decide(&out) == 0, "decide");
    CHECK(impai_fmem_find(&s_st, 70, 10) >= 0 && !s_w.dark_reads, "(d) unexplored: kept, %d unexplored reads",
          s_w.dark_reads);
    reclaim_world(1);
    rock = feat_add(30, 30, 2, 2, 300);
    reclaim_job_set(rock, 800);
    reclaim_head(1, A_RECLAIM, rock);
    s_w.unit[1].list[0].pos[0] = s_w.unit[1].list[0].pos[2] = (int32_t)((uint32_t)(16 * 30 + 8) << 16);
    s_w.feat[rock].live = 0;
    s_w.dark[15][15] = s_w.dark[16][16] = 1;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_job[1].kind == IMPAI_JOB_RECLAIM && s_job[1].cx == 30, "(e) the job holds: kind %u", s_job[1].kind);
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* THE CONFIDENCE from the seat's own sight (impai_build.c): reclaim_world at GameTime 54 000, a 300-metal rock at
   (30, 30) and a 150-metal rock at (34, 34), both shown. With both blocks in the seat's sight the commander takes
   the rich rock; with the rich rock's blocks out of it, never covered by the seat's sight, its confidence is
   1 800 / 55 800 and the commander takes the poorer rock it sees. */
static void check_reclaim_confidence(void)
{
    IMPAI_BRAIN_OUT out;
    int k, rich, poor, got;
    for (k = 0; k < 2; k++) {
        reclaim_world(1);
        s_w.gametime = 54000;
        rich = feat_add(30, 30, 2, 2, 300);
        poor = feat_add(34, 34, 2, 2, 150);
        if (k) s_w.dark[15][15] = s_w.dark[16][16] = 1;
        CHECK(decide(&out) == 0, "decide");
        got = reclaimed_by(1);
        printf("the confidence, the rich rock %s: the commander reclaims %s\n", k ? "out of sight" : "in sight",
               got == rich ? "the rich rock" : got == poor ? "the poorer rock" : "neither");
        CHECK(got == (k ? poor : rich), "the confidence, the rich rock %s", k ? "out of sight" : "in sight");
    }
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* AN ENTRY ORDERED IS HELD at once (impai_build.c, impai_build_reclaim): 128 remembered rocks of 50 metal each,
   shown, the commander on the first; reclaim_world's constructor away from them, and a 400-metal rock the memory
   does not hold. The commander reclaims the first entry, the constructor's look finds the rich rock and its place
   goes to another entry than the one just ordered, so both reclaims' entries are remembered at the decision's
   end. Then a restored count past
   IMPAI_FEATS is held to it (THE MEMORY'S COUNT IS DATA, impai_brain.c). */
static void check_feature_memory_ordered(void)
{
    IMPAI_BRAIN_OUT out;
    int i, first = -1, rich;
    reclaim_world(1);
    for (i = 0; i < IMPAI_FEATS; i++) {
        const int f = feat_add(34 + 2 * (i % 15), 2 + 2 * (i / 15), 1, 1, 50.0f);
        IMPAI_FMEM* e = &s_st.fmem[s_st.nfmem++];
        e->ax = s_w.feat[f].ax; e->az = s_w.feat[f].az;
        e->def = s_w.feat[f].def;
        e->fx = e->fz = 1;
        e->owner = 10;
        e->metal = s_w.feat[f].metal;
        e->seen = 100;
        if (!i) first = f;
    }
    unit_add(&s_w, 1, T_COM, 34, 3, 1);
    rich = feat_add(38, 40, 1, 1, 400.0f);
    CHECK(first >= 0 && rich >= 0, "the fake holds the rocks");
    s_st.fscan = 0;
    s_w.econ[0].metal = 0.0f;
    CHECK(decide(&out) == 0, "decide");
    printf("an entry ordered: the commander's job %u at (%d, %d), remembered %d; the constructor's job %u at (%d, %d), "
           "remembered %d\n", s_job[1].kind, s_job[1].cx, s_job[1].cz, impai_fmem_find(&s_st, s_job[1].cx, s_job[1].cz) >= 0,
           s_job[2].kind, s_job[2].cx, s_job[2].cz, impai_fmem_find(&s_st, s_job[2].cx, s_job[2].cz) >= 0);
    CHECK(s_job[1].kind == IMPAI_JOB_RECLAIM && s_job[1].cx == s_w.feat[first].ax &&
          impai_fmem_find(&s_st, s_job[1].cx, s_job[1].cz) >= 0,
          "the commander's reclaim of the first entry, the entry still remembered");
    CHECK(s_job[2].kind == IMPAI_JOB_RECLAIM && s_job[2].cx == s_w.feat[rich].ax &&
          impai_fmem_find(&s_st, s_job[2].cx, s_job[2].cz) >= 0, "the constructor's reclaim of the rich rock, remembered");
    s_st.nfmem = 200;
    s_w.gametime += 15;
    CHECK(decide(&out) == 0, "decide");
    printf("a restored count of 200: %u after the decision\n", s_st.nfmem);
    CHECK(s_st.nfmem <= IMPAI_FEATS, "the memory's count held to IMPAI_FEATS");
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* THE PLACE IN A FULL MEMORY across a drop in the same decision (impai_build.c, fmem_put; impai_fmem_drop): 128
   remembered rocks, each shown, of 10 + i metal, the commander standing among them and reclaiming entry 5, made the
   poorest at 1 metal; a constructor's reclaim job names entry 2's anchor under another definition, so its check
   drops that entry after the decision's marks were made, and every entry past it moves down one; two 400-metal
   rocks beside the idle constructor, whose own search's look finds them after the drop; the dropped rock, still
   standing, is found again by the freed constructor's look. Three new entries, one free place: the 10-metal and
   11-metal entries' places are given, and the commander's entry -- moved, its mark with it -- is kept with its
   reclaim. */
static void check_feature_memory_drop(void)
{
    IMPAI_BRAIN_OUT out;
    int i, held = -1, gone = -1, a, b;
    reclaim_world(1);
    for (i = 0; i < IMPAI_FEATS; i++) {
        const int f = feat_add(34 + 2 * (i % 15), 2 + 2 * (i / 15), 1, 1, 10.0f + (float)i);
        IMPAI_FMEM* e = &s_st.fmem[s_st.nfmem++];
        e->ax = s_w.feat[f].ax; e->az = s_w.feat[f].az;
        e->def = s_w.feat[f].def;
        e->fx = e->fz = 1;
        e->owner = 10;
        e->metal = s_w.feat[f].metal;
        e->seen = 100;
        if (i == 2) gone = f;
        if (i == 5) held = f;
    }
    s_w.feat[held].metal = s_st.fmem[5].metal = 1.0f;
    unit_add(&s_w, 1, T_COM, 48, 10, 1);
    reclaim_job_set(held, 800);
    reclaim_head(1, A_RECLAIM, held);
    unit_add(&s_w, 4, T_CONS, 36, 6, 1);
    s_job[4].type = T_CONS;
    s_job[4].build = s_job[4].target = (uint16_t)WQ_NONE;
    s_job[4].cx = s_w.feat[gone].ax;
    s_job[4].cz = s_w.feat[gone].az;
    s_job[4].feat = (uint16_t)(s_w.feat[gone].def + 1000);
    s_job[4].kind = IMPAI_JOB_RECLAIM;
    s_job[4].since = 800;
    reclaim_head(4, A_RECLAIM, gone);
    a = feat_add(22, 33, 1, 1, 400.0f);
    b = feat_add(24, 33, 1, 1, 400.0f);
    CHECK(a >= 0 && b >= 0, "the fake holds the rocks");
    s_st.fscan = 0;
    s_w.econ[0].metal = 0.0f;
    CHECK(decide(&out) == 0, "decide");
    printf("the feature memory across a drop: the claimed entry %d (job %u), the dropped %d, the new rocks %d %d, "
           "the 10-metal entry %d, %u entries\n", impai_fmem_find(&s_st, s_w.feat[held].ax, s_w.feat[held].az) >= 0,
           s_job[1].kind, impai_fmem_find(&s_st, s_w.feat[gone].ax, s_w.feat[gone].az) >= 0,
           impai_fmem_find(&s_st, 22, 33) >= 0, impai_fmem_find(&s_st, 24, 33) >= 0,
           impai_fmem_find(&s_st, 34, 2) >= 0, s_st.nfmem);
    CHECK(impai_fmem_find(&s_st, 22, 33) >= 0 && impai_fmem_find(&s_st, 24, 33) >= 0 &&
          impai_fmem_find(&s_st, 34, 2) < 0 && impai_fmem_find(&s_st, 36, 2) < 0 && s_st.nfmem == IMPAI_FEATS,
          "both new rocks remembered, the 10- and 11-metal entries' places given");
    CHECK(impai_fmem_find(&s_st, s_w.feat[held].ax, s_w.feat[held].az) >= 0 && s_job[1].kind == IMPAI_JOB_RECLAIM,
          "the claimed entry kept, its mark moved with it: job %u", s_job[1].kind);
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

static void check_reclaim(void)
{
    IMPAI_BRAIN_OUT out;
    const FCMD* c;
    int i, rich, far, a = -1, b = -1, unseen, n;

    reclaim_world(1);
    feat_add(22, 22, 2, 2, 20);              /* beside the commander: 20 metal, 26 ticks and no walk          */
    rich = feat_add(30, 30, 2, 2, 300);      /* 300 metal, 166 ticks and a walk of about 90 px past its reach */
    feat_add(16, 34, 2, 2, 40);
    feat_add(40, 40, 1, 1, 0);               /* no metal: never a target                                      */
    unseen = feat_add(66, 20, 2, 2, 900);    /* beyond the explored quarter                                   */
    CHECK(decide(&out) == 0, "decide");
    CHECK(reclaim_orders(0) == 2 && out.reclaims == 2, "both builders reclaim: %d orders", reclaim_orders(0));
    for (i = 0; i < s_w.nlog; i++) {
        if (s_w.log[i].c.action != A_RECLAIM) continue;
        CHECK(s_w.log[i].c.target == WQ_NONE && s_w.log[i].c.verb == WQ_CMD_NEW && !s_w.log[i].c.shift,
              "a reclaim is an order by name at a point, replacing the list");
        if (s_w.log[i].c.unit == 1) a = reclaim_feat_of(&s_w.log[i].c);
        if (s_w.log[i].c.unit == 2) b = reclaim_feat_of(&s_w.log[i].c);
        cmd_print(&s_w.log[i]);
    }
    CHECK(a == rich, "the commander takes the most metal a tick, the rich rock: feature %d", a);
    CHECK(b >= 0 && b != a && b != unseen, "the constructor takes another, the first claimed: feature %d", b);
    CHECK(s_job[1].kind == IMPAI_JOB_RECLAIM && s_job[1].cx == 30 && s_job[1].cz == 30 &&
          s_job[1].feat == s_w.feat[rich].def && s_job[1].build == WQ_NONE && s_job[1].target == WQ_NONE,
          "the job names the feature by its anchor and its definition");
    n = s_w.nlog;
    s_w.gametime = 915;
    CHECK(decide(&out) == 0, "decide");
    for (i = n; i < s_w.nlog; i++)
        CHECK(s_w.log[i].c.unit != 1 && s_w.log[i].c.unit != 2, "a builder at its reclaim is ordered again");
    CHECK(s_job[1].kind == IMPAI_JOB_RECLAIM, "its job holds while the feature does");
    s_w.feat[rich].live = 0;
    s_w.unit[1].n = 0;          /* the handler ends an order whose feature is gone (0x404AF8) */
    s_w.gametime = 930;
    CHECK(decide(&out) == 0, "decide");
    CHECK(!s_job[1].far_until && !(s_job[1].cx == 30 && s_job[1].cz == 30),
          "a vanished feature ends the job, avoided by no one");
    CHECK(s_job[1].kind == IMPAI_JOB_RECLAIM && s_job[1].cx == 22 && s_job[1].cz == 22,
          "the commander, idle again, takes the next rock: %d,%d", s_job[1].cx, s_job[1].cz);
    printf("reclaim commander=%d constructor=%d\n", a, b);

    reclaim_world(1);
    far = feat_add(30, 30, 2, 2, 300);
    s_job[1].type = T_COM;
    s_job[1].build = s_job[1].target = (uint16_t)WQ_NONE;
    s_job[1].cx = 30;
    s_job[1].cz = 30;
    s_job[1].feat = s_w.feat[far].def;
    s_job[1].kind = IMPAI_JOB_RECLAIM;
    s_job[1].since = 800;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_job[1].far_cx == 30 && s_job[1].far_cz == 30 && s_job[1].far_until == 900u + s_plan->fail_t &&
          out.failed == 0 && s_st.nfail == 0, "the builder's record and no failed site: %d,%d until %u",
          s_job[1].far_cx, s_job[1].far_cz, s_job[1].far_until);
    CHECK(!reclaim_orders(1), "the commander is sent back to the feature it could not reclaim");
    c = last_cmd(0);
    CHECK(reclaim_orders(2) == 1 && c && c->c.unit == 2 && reclaim_feat_of(&c->c) == far,
          "another builder takes it");

    reclaim_world(0);
    s_w.econ[0].metal = s_w.econ[0].metal_max;
    feat_add(22, 22, 2, 2, 20);
    feat_add(30, 30, 2, 2, 300);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!reclaim_orders(0) && !out.reclaims, "a full store reclaims: %d orders", reclaim_orders(0));

    /* metal in store and the income its next factory wants (factory_per_m a factory), no extractor yet: not
       short, and no extractor's rate for a feature to beat */
    reclaim_world(0);
    s_w.econ[0].metal_in = s_plan->factory_per_m;
    feat_add(22, 22, 2, 2, 20);
    feat_add(30, 30, 2, 2, 300);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!reclaim_orders(0) && !out.reclaims, "a seat with the income it wants reclaims: %d orders",
          reclaim_orders(0));

    reclaim_world(1);
    s_spots = PATCHES;
    s_nspots = NPATCH;
    unit_add(&s_w, 1, T_COM, 30, 22, 1);
    feat_add(31, 23, 2, 2, 20);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!reclaim_orders(1), "the commander reclaims beside a free patch");
    for (i = 0, a = 0; i < s_w.nlog; i++) a += s_w.log[i].c.unit == 1 && s_w.log[i].c.type == T_MEX;
    CHECK(a == 1, "the commander builds the extractor: %d orders", a);

    reclaim_world(1);
    unit_add(&s_w, 2, T_CONS, 62, 20, 1);
    unit_add(&s_w, 1, T_COM, 60, 22, 1);
    feat_add(66, 20, 2, 2, 900);
    CHECK(decide(&out) == 0, "decide");
    CHECK(!reclaim_orders(0), "a feature on unexplored ground is taken: %d orders", reclaim_orders(0));
    CHECK(!s_w.dark_reads, "a cell out of sight is read: %d reads", s_w.dark_reads);

    /* a remembered rock, reclaimed by the enemy while out of sight */
    reclaim_world(1);
    far = feat_add(30, 30, 2, 2, 300);
    reclaim_remember(far, 100);
    s_w.feat[far].live = 0;
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    CHECK(s_job[1].kind == IMPAI_JOB_RECLAIM_GO && s_job[1].cx == 30 && s_job[1].feat == s_w.feat[far].def,
          "the commander walks to the remembered rock: kind %u", s_job[1].kind);
    for (i = 0, n = 0; i < s_w.nlog; i++)
        if (s_w.log[i].c.action == A_MOVE && s_w.log[i].c.unit == 1 && reclaim_feat_of(&s_w.log[i].c) == far) n++;
    CHECK(n == 1 && !reclaim_orders(0), "a move to it, and no reclaim on a belief: %d moves", n);
    for (i = 0; i < s_w.nlog; i++)
        CHECK(s_w.log[i].c.unit != 2 || reclaim_feat_of(&s_w.log[i].c) != far, "the constructor is sent to it too");
    n = s_w.nlog;
    s_w.dark[15][15] = 0;       /* the commander has come near: the rock's block is in sight, and the rock gone */
    s_w.gametime = 915;
    unit_add(&s_w, 4, T_SOLAR, 24, 20, 0);  /* a frame to help: the commander never guards the busy factory */
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_st.nfmem == 0 && impai_fmem_find(&s_st, 30, 30) < 0, "the seat forgets the rock: %u remembered",
          s_st.nfmem);
    CHECK(s_job[1].kind != IMPAI_JOB_RECLAIM_GO && !s_job[1].far_until && out.failed == 0,
          "the job ends with no failure: kind %u", s_job[1].kind);
    for (i = n, a = 0; i < s_w.nlog; i++) {
        CHECK(reclaim_feat_of(&s_w.log[i].c) != far, "a builder is sent to the rock again");
        a += s_w.log[i].c.unit == 1;
    }
    CHECK(a == 1, "the commander is given its next task in the same decision: %d orders", a);
    CHECK(!s_w.dark_reads, "a cell out of sight is read: %d reads", s_w.dark_reads);

    /* a rich rock seen 1800 ticks ago (confidence 1/2) loses to a poorer, nearer one in sight: 300 x 0.5 / 166
       against 100 / 66, where 300 / 166 would win */
    reclaim_world(1);
    unit_add(&s_w, 2, T_CONS, 50, 50, 1);
    far = feat_add(18, 22, 2, 2, 300);
    reclaim_remember(far, 0);
    s_w.gametime = 1800;
    a = feat_add(20, 20, 2, 2, 100);
    CHECK(decide(&out) == 0, "decide");
    c = last_cmd(0);
    for (i = 0, b = -1; i < s_w.nlog; i++)
        if (s_w.log[i].c.unit == 1) b = reclaim_feat_of(&s_w.log[i].c);
    CHECK(b == a && s_job[1].kind == IMPAI_JOB_RECLAIM, "the rock in sight wins: feature %d", b);
    CHECK(!s_w.dark_reads, "a cell out of sight is read: %d reads", s_w.dark_reads);
    printf("reclaim decay: chose %d of remembered %d and in sight %d\n", b, far, a);
    s_spots = PATCHES;
    s_nspots = NPATCH;
    check_reclaim_economy();
}

/* THE ROUTE (impai_build.c) and THE WALK CHECK (impai_brain.c) over one enemy row: a turret type of ground reach
   10 over `range` px at block (bx, bz), known by `how`; how 0, no row. The 256-px cells of the 128-cell map are
   8 a side; the builders below stand in cell (1, 1). */
static IMPAI_ROW    s_rrows[FW_SLOTS];
static IMPAI_THREAT s_rth[FW_DEFS];
static IMPAI_VIEW   s_rview;
static void route_row(unsigned how, int bx, int bz, int range)
{
    memset(s_rrows, 0, sizeof s_rrows);
    memset(s_rth, 0, sizeof s_rth);
    memset(&s_rview, 0, sizeof s_rview);
    s_rth[T_TURRET].hit_ground = 10;
    s_rth[T_TURRET].hit_range = (uint16_t)range;
    s_rview.seat = 0;
    s_rview.rows = s_rrows;
    s_rview.nrows = FW_SLOTS;
    s_rview.threat = s_rth;
    s_rview.ntypes = T_N;
    s_rview.w = FW_W;
    s_rview.h = FW_H;
    if (how) {
        s_rrows[40].seat[0].type = T_TURRET;
        s_rrows[40].seat[0].how = (uint8_t)how;
        s_rrows[40].seat[0].owner = 1;
        s_rrows[40].seat[0].bx = (uint16_t)bx;
        s_rrows[40].seat[0].bz = (uint16_t)bz;
    }
    s_viewp = &s_rview;
}

/* a row covering cell (2, 1) alone: its block's centre (624, 368), 100 px */
#define ROW_CELL21 19, 11, 100

/* THE BLAST'S APPROACH (impai_mil.c, impai_blast_nearer; impai_build.c, cmd_refused): reclaim_world's commander at
   (328, 328) px, builddistance 120, 1 px a tick, a foe's commander of the same speed remembered at (1136, 336),
   808 px east, on Hard (H = 30): its keep is 475 + 64 + 30 + (30 + age), 899 px at age 300.
   (a) seen 300 ticks ago, a 300-metal rock 328 px east and a 100-metal rock 264 px south: the walk east ends
   208 px on, 600 px from the foe, inside the keep and nearer than the commander's 808: refused, and the decision
   counted (blast_refused); the walk south ends 817 px off, farther: the commander takes the south rock;
   (b) the same, seen IMPAI_BLAST_SEEN_T + 15 ticks ago: the memory says nothing, and it takes the east rock;
   (c) seen 300 ticks ago, a 300-metal rock 88 px east -- nearer the foe, inside the keep, but within the
   builddistance, so it walks nowhere: never refused, and the commander takes it; a move to it (to_point), which
   walks to the rock itself, 720 px from the foe, is refused;
   (d) as (a), the foe's commander of a type whose speed reads 0 (the turret, classed a commander of blast_r 475):
   its approach takes the own commander's speed, the keep is 899 px again and the east rock is refused, where a
   foe speed of 0 would keep 569 px and pass the walk's end 600 px off. No foe memory at all refuses nothing, the
   opening's case. */
static IMPAI_MIL s_umil;
static int a_reclaim(const IMPAI_JOB* j) { return j->kind == IMPAI_JOB_RECLAIM || j->kind == IMPAI_JOB_RECLAIM_GO; }

static void check_blast_approach(void)
{
    static const char* const NAME[4] = { "seen 300 ticks ago", "seen past IMPAI_BLAST_SEEN_T",
                                         "a rock within the builddistance", "a foe whose speed reads 0" };
    IMPAI_BRAIN_OUT out;
    int c, east;
    for (c = 0; c < 4; c++) {
        const unsigned foe = c == 3 ? T_TURRET : T_COM;
        reclaim_world(1);
        s_w.unit[2].live = 0;
        s_w.gametime = 20000;
        s_ti[T_COM].blast_r = 475;
        s_ti[T_TURRET].cat = IMPAI_C_COMMANDER;
        s_ti[T_TURRET].blast_r = 475;
        CHECK(s_w.defs[T_TURRET].d.speed == 0 && s_w.defs[T_COM].d.speed == 1 << 16, "the speeds");
        east = c == 2 ? feat_add(25, 20, 2, 2, 300) : feat_add(40, 20, 2, 2, 300);
        feat_add(20, 36, 2, 2, 100);
        route_row(0, 0, 0, 0);
        s_rrows[41].seat[0].type = (uint16_t)foe;
        s_rrows[41].seat[0].how = IMPAI_GHOST;
        s_rrows[41].seat[0].owner = 1;
        s_rrows[41].seat[0].bx = 35;
        s_rrows[41].seat[0].bz = 10;
        impai_mil_reset(&s_umil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
        s_umil.foe[1].slot = 41;
        s_umil.foe[1].bx = 35;
        s_umil.foe[1].bz = 10;
        s_umil.foe[1].t = (uint32_t)s_w.gametime - (c == 1 ? IMPAI_BLAST_SEEN_T + 15u : 300u);
        s_milp = &s_umil;
        CHECK(decide(&out) == 0, "decide");
        if (c == 2) {
            WORLDQ q = { &FAKE, &s_w, 0 };
            const uint32_t now = (uint32_t)s_w.gametime;
            const unsigned nti = (unsigned)s_w.ndefs;
            CHECK(!impai_blast_nearer(&q, &s_umil, s_viewp, s_ti, nti, s_persona, now, T_COM, 0, 328, 328, 416, 328) &&
                      impai_blast_nearer(&q, &s_umil, s_viewp, s_ti, nti, s_persona, now, T_COM, 1, 328, 328, 416, 328),
                  "%s: a task there walks nowhere, a move there walks to the rock", NAME[c]);
        }
        s_milp = NULL;
        s_viewp = NULL;
        printf("the blast's approach, %s: the commander's job %u at (%d, %d), blast_refused %u com_free %u com_idle %u\n",
               NAME[c], s_job[1].kind, s_job[1].cx, s_job[1].cz, out.blast_refused, out.com_free, out.com_idle);
        if (c == 0 || c == 3)
            CHECK(a_reclaim(&s_job[1]) && s_job[1].cz == 36 && out.blast_refused == 1,
                  "%s: the commander passes the east rock for the south, counted", NAME[c]);
        else
            CHECK(a_reclaim(&s_job[1]) && s_job[1].cx == s_w.feat[east].ax && (c == 2 || !out.blast_refused),
                  "%s: the commander takes the east rock", NAME[c]);
        CHECK(out.com_free == 1 && out.com_idle == 0, "%s: the commander outside the window, given a job", NAME[c]);
    }
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* two patches a constructor at cell (20, 20) -- px (328, 328), the 256-px cell (1, 1) -- can take: N, whose
   centre (856, 344) is 528 px off in cell (3, 1), across cell (2, 1); F, whose centre (344, 920) is 592 px off
   in cell (1, 3) */
static const SNAP_PATCH ROUTE_PATCHES[] = {
    { 52, 20, 52, 20, 54, 22, 9 },      /* N */
    { 20, 56, 20, 56, 22, 58, 9 },      /* F */
};

static void route_world(void)
{
    unsigned i;
    int x, z;
    brain_world();
    s_spots = ROUTE_PATCHES;
    s_nspots = 2;
    for (i = 0; i < sizeof s_labels / sizeof s_labels[0]; i++) s_labels[i] = 1;
    for (i = 0; i < 2; i++)
        for (z = ROUTE_PATCHES[i].z0; z <= ROUTE_PATCHES[i].z1; z++)
            for (x = ROUTE_PATCHES[i].x0; x <= ROUTE_PATCHES[i].x1; x++) s_w.metal[z][x] = 200;
    unit_add(&s_w, 1, T_CONS, 20, 20, 1);
    s_st.ax = 20;
    s_st.az = 20;
    s_w.gametime = 300;
}

/* the patch the constructor was ordered to: 'N', 'F', or '-' for no extractor order */
static char route_patch(void)
{
    const FCMD* c = last_cmd(0);
    if (!c || c->c.type != T_MEX) return '-';
    return cmd_x0(&c->c) >= 50 ? 'N' : cmd_z0(&c->c) >= 54 ? 'F' : '?';
}

/* B2: with no row the constructor takes N, the nearer; with a turret seen now or remembered as a ghost over cell
   (2, 1), on N's straight way but not at N, it takes F; with the turret's reach over the constructor's own cell
   alone it still takes N, so a builder in threat is sent out of it. A feature in sight in cell (3, 1), its way
   across the row, is reclaimed by neither builder, and is with no row. With the commander no reclaimer, four
   300-metal features behind the row, each scoring above a 10-metal rock 224 px west of the constructor, the
   constructor takes the rock: a threatened route uses none of the IMPAI_RECLAIM_TRIES. The explorer's cell (4, 1) behind the row:
   no order, and the cell fails. */
/* THE EXPLORE CELL keeps to held ground: route_world's constructor, no patch, the explored quarter cells (0..3,
   0..3), an enemy's armed row in 256-px cell (6, 1): the nearest unexplored cell, (4, 1), is 3 steps from the anchor
   and 2 from the row, not held; (1, 4), as near and after it in row-major order, is held: it is explored. With no
   row (4, 1) is. */
static void check_explore_held(void)
{
    IMPAI_BRAIN_OUT out;
    int c;
    for (c = 0; c < 2; c++) {
        route_world();
        s_spots = NULL;
        s_nspots = 0;
        route_row(c ? 0 : IMPAI_LOS, 6 * 8 + 2, 10, 100);
        s_ti[T_TURRET].flags |= IMPAI_TF_MOBILE;
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        s_ti[T_TURRET].flags &= ~IMPAI_TF_MOBILE;
        printf("explore, %s: job %u at %d,%d\n", c ? "no row" : "a row in cell (6, 1)", s_job[1].kind, s_job[1].cx,
               s_job[1].cz);
        CHECK(s_job[1].kind == IMPAI_JOB_EXPLORE && s_job[1].cx == (c ? 72 : 24) && s_job[1].cz == (c ? 24 : 72),
              "the explorer takes the nearest held cell: %d,%d", s_job[1].cx, s_job[1].cz);
    }
}

static void check_route(void)
{
    static const char* const NAME[4] = { "no row", "a turret seen", "a ghost turret", "the turret over the builder" };
    IMPAI_BRAIN_OUT out;
    int c, k, i, reclaims;
    for (c = 0; c < 4; c++) {
        char got;
        route_world();
        if (c == 1) route_row(IMPAI_LOS, ROW_CELL21);
        if (c == 2) route_row(IMPAI_GHOST, ROW_CELL21);
        if (c == 3) route_row(IMPAI_LOS, 11, 11, 100);
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        got = route_patch();
        printf("route, %s: patch %c\n", NAME[c], got);
        CHECK(got == (c == 1 || c == 2 ? 'F' : 'N'), "route, %s: patch %c", NAME[c], got);
    }
    for (c = 0; c < 3; c++) {
        reclaim_world(1);
        for (i = 0; i < (int)(sizeof s_labels / sizeof s_labels[0]); i++) s_labels[i] = 1;
        k = feat_add(52, 30, 2, 2, 300);
        if (c) route_row(IMPAI_LOS, ROW_CELL21);
        if (c == 2) {
            int rock;
            s_w.defs[T_COM].d.flags &= ~WQ_DF_RECLAIMER;
            feat_add(52, 26, 2, 2, 300);
            feat_add(52, 22, 2, 2, 300);
            feat_add(56, 28, 2, 2, 300);
            rock = feat_add(8, 30, 2, 2, 10);
            CHECK(decide(&out) == 0, "decide");
            s_viewp = NULL;
            printf("route, four features behind the row and a rock clear of it: the constructor takes %d (rock %d)\n",
                   reclaimed_by(2), rock);
            CHECK(reclaimed_by(2) == rock, "a threatened route uses no try: the constructor takes the rock");
            continue;
        }
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        reclaims = reclaim_orders(0);
        printf("route, a feature beyond cell (2, 1), %s: reclaims %d\n", c ? "the row" : "no row", reclaims);
        CHECK(c ? reclaims == 0 : reclaims == 1 && (reclaimed_by(1) == k || reclaimed_by(2) == k),
              "route, the feature's way %s: %d reclaims", c ? "across the row" : "clear", reclaims);
    }
    route_world();
    s_spots = NULL;
    s_nspots = 0;
    route_row(IMPAI_LOS, ROW_CELL21);
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    for (k = 0, i = 0; k < s_st.nfail && k < IMPAI_FAILS; k++) i += s_st.fail[k].cx == 72 && s_st.fail[k].cz == 24;
    printf("route, the explorer's cell behind the row: orders %d, failed %d\n", s_w.nlog, i);
    CHECK(s_w.nlog == 0 && i == 1 && s_st.explorer == WQ_NONE, "the explorer is not sent across the row: %d orders",
          s_w.nlog);
}

/* THE HELD GROUND (impai_build.c): a constructor at cell (66, 20), the anchor at (20, 20), the map explored and one
   component -- on the 256-px grid the anchor in cell (1, 1), the constructor in (4, 1) --; patch H at (40, 22),
   cell (2, 1), 26 cells off the constructor, and patch E at (80, 20), cell (5, 1), 14 off. Armed rows of a mobile
   type with a ground reach of 100 px:
   (a) an enemy's in cell (6, 1): E is 4 steps from the anchor and 1 from it -- not held --, H 1 and 4: H;
   (b) the push: the seat's own in cell (6, 1), no enemy: E is held: E;
   (c) from (b)'s held ground, the seat's back in cell (3, 1) and an enemy in (7, 1): E 2 steps from each, held
       still by the hysteresis: E;
   (c') the same rows from no held ground: 4 x 2 > 3 x 2, E is not taken: H;
   (d) no view: every patch held: E, the nearer;
   (e) as (a) with the enemy's type a tower, which does not move: it holds its own cell alone, E is held: E;
   (f) a tower of the seat's own in E's cell and the enemy's mobile row in (6, 1): E is 0 steps from the seat's
       strength: E;
   (g) no rows, a start position in cell (6, 1) beside the seat's own at the anchor: the other start stands for the
       enemy: H;
   (h) a free-for-all: seat 1's row in cell (7, 7), 6 steps off E, and seat 2's in (6, 1): every enemy counts: H;
   (i) a radar contact never identified in cell (6, 1), the military counting contacts as the turret type: H;
   (i') the same with the military counting none (radar_as WQ_NONE): E;
   (j) an enemy aircraft armed against the ground in cell (6, 1), which holds no ground: E;
   (k) the contact of (i) counted as a tower type, which contests only its reach: E;
   (l) an enemy constructor, mobile and unarmed, in cell (6, 1): E;
   (m) no rows and no start, the military's estimate of the enemy base in cell (5, 1): the estimate is no source: E;
   (n) a free-for-all: seat 1's extractor in cell (12, 1) and seat 2's in (12, 13), the estimate between them by the
       seat in (5, 1): each base its own source, E 7 steps off the nearer: E;
   (o) seat 1's extractor in cell (12, 1) and a start in (6, 1): the enemy's base is known, the start no source: E;
   (o') seat 1 known by its army's row in (12, 1) alone, the start in (6, 1): the starts stand for its base: H;
   (p) seat 1's extractor in cell (6, 1), no start: its base is a source: H;
   THE SEATS (WQ_START.seated), no rows, the seat's start at the anchor seated by it and two more:
   (q) the start in (6, 1) left empty, seat 1's in (12, 1): E, 4 steps from the anchor and 7 from seat 1's;
   (r) the start in (6, 1) seated by seat 2, the seat's ally, seat 1's in (12, 1): E;
   (s) the start in (6, 1) seated by seat 1, the one in (12, 1) left empty: H;
   (t) as (s), seat 1's extractor known in (12, 13) and seat 2's army seen in (12, 1), its base not known: the
       starts stand for seat 2's base alone, and seat 1's start is none: E. */
static IMPAI_MIL s_hmil;

/* an enemy structure row of seat `owner` at 256-px cell (cx, cz), row `row` */
static void held_base(int row, int owner, int cx, int cz)
{
    s_rrows[row].seat[0].type = T_MEX;
    s_rrows[row].seat[0].how = IMPAI_LOS;
    s_rrows[row].seat[0].owner = (uint8_t)owner;
    s_rrows[row].seat[0].bx = (uint16_t)(cx * 8 + 2);
    s_rrows[row].seat[0].bz = (uint16_t)(cz * 8 + 2);
}

/* the seat's start at the anchor and another in 256-px cell (6, 1) */
static void held_starts(void)
{
    s_w.starts[0].x = s_w.starts[0].z = 328;
    s_w.starts[0].index = 0;
    s_w.starts[1].x = 6 * 256 + 128;
    s_w.starts[1].z = 256 + 128;
    s_w.starts[1].index = 1;
    s_w.nstarts = 2;
}

/* the seat's start at the anchor seated by it, one in 256-px cell (6, 1) seated `near` and one in (12, 1) seated
   `far` (WQ_START.seated) */
static void held_seats(int32_t near, int32_t far)
{
    held_starts();
    s_w.starts[0].seated = 1;
    s_w.starts[1].seated = near;
    s_w.starts[2].x = 12 * 256 + 128;
    s_w.starts[2].z = 256 + 128;
    s_w.starts[2].index = 2;
    s_w.starts[2].seated = far;
    s_w.nstarts = 3;
}

static void held_rows(int own_cell, int foe_cell, int mobile)
{
    route_row(0, 0, 0, 0);
    if (mobile) s_ti[T_TURRET].flags |= IMPAI_TF_MOBILE;
    else s_ti[T_TURRET].flags &= ~IMPAI_TF_MOBILE;
    s_rth[T_TURRET].hit_ground = 10;
    s_rth[T_TURRET].hit_range = 100;
    if (foe_cell >= 0) {
        s_rrows[40].seat[0].type = T_TURRET;
        s_rrows[40].seat[0].how = IMPAI_LOS;
        s_rrows[40].seat[0].owner = 1;
        s_rrows[40].seat[0].bx = (uint16_t)(foe_cell * 8 + 2);
        s_rrows[40].seat[0].bz = 10;
    }
    if (own_cell >= 0) {
        s_rrows[41].seat[0].type = T_TURRET;
        s_rrows[41].seat[0].how = IMPAI_OWN;
        s_rrows[41].seat[0].owner = 0;
        s_rrows[41].seat[0].bx = (uint16_t)(own_cell * 8 + 2);
        s_rrows[41].seat[0].bz = 10;
    }
}

static const SNAP_PATCH HOLD_PATCHES[] = {
    { 40, 22, 40, 22, 42, 24, 9 },      /* H */
    { 80, 20, 80, 20, 82, 22, 9 },      /* E */
};

static void check_held_ground(void)
{
    static const char* const NAME[28] = { "an enemy in cell (6, 1)", "the push", "the front back, held before",
                                         "the front back, not held before", "no view", "an enemy tower in (6, 1)",
                                         "a tower of the seat's in E's cell", "a start in (6, 1)",
                                         "two enemies", "a radar contact", "a contact counted as none",
                                         "an enemy aircraft", "a contact counted as a tower",
                                         "an enemy constructor", "the estimate in (5, 1), no start",
                                         "two enemies' bases, the estimate between them",
                                         "the enemy's base known, a start in (6, 1)",
                                         "the enemy's army alone, a start in (6, 1)",
                                         "the enemy's base in (6, 1)",
                                         "the start in (6, 1) left empty", "the start in (6, 1) an ally's",
                                         "the start in (6, 1) the enemy's",
                                         "the start in (6, 1) an enemy's whose base is known, another's army seen" };
    static const int OWN[23] = { -1, 6, 3, 3, -1, -1, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                 -1 };
    static const int FOE[23] = { 6, -1, 7, 7, -1, 6, 6, -1, 7, 6, 6, 6, 6, 6, -1, -1, -1, 12, -1, -1, -1, -1, 12 };
    static const char WANT[23] = { 'H', 'E', 'E', 'H', 'E', 'E', 'E', 'H', 'H', 'H', 'E', 'E', 'E', 'E',
                                   'E', 'E', 'E', 'H', 'H', 'E', 'E', 'H', 'E' };
    uint8_t held_b[sizeof s_st.held];
    IMPAI_BRAIN_OUT out;
    unsigned i;
    int c, x, z;
    for (c = 0; c < 23; c++) {
        const FCMD* cmd;
        char got;
        brain_world();
        if (c == 2) memcpy(s_st.held, held_b, sizeof held_b);
        s_spots = HOLD_PATCHES;
        s_nspots = 2;
        for (i = 0; i < sizeof s_labels / sizeof s_labels[0]; i++) s_labels[i] = 1;
        for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
        for (i = 0; i < 2; i++)
            for (z = HOLD_PATCHES[i].z0; z <= HOLD_PATCHES[i].z1; z++)
                for (x = HOLD_PATCHES[i].x0; x <= HOLD_PATCHES[i].x1; x++) s_w.metal[z][x] = 200;
        unit_add(&s_w, 1, T_CONS, 66, 20, 1);
        s_st.ax = 20;
        s_st.az = 20;
        s_w.gametime = 300;
        impai_mil_reset(&s_hmil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
        s_hmil.ebx = s_hmil.ebz = IMPAI_NO_ANCHOR;
        s_milp = &s_hmil;
        if (c != 4) held_rows(OWN[c], FOE[c], c < 5 || (c >= 8 && c != 12));
        if (c == 11) s_ti[T_TURRET].flags |= IMPAI_TF_VTOL;
        if (c == 13) s_rrows[40].seat[0].type = T_CONS;
        if (c >= 9 && c <= 12 && c != 11) {
            s_rrows[40].seat[0].type = (uint16_t)WQ_NONE;     /* a contact never identified */
            s_rrows[40].seat[0].owner = 0xFF;
            s_rrows[40].seat[0].how = IMPAI_RADAR;
            s_hmil.radar_as = c == 10 ? (uint16_t)WQ_NONE : (uint16_t)T_TURRET;
        }
        if (c == 7 || c == 16 || c == 17) held_starts();
        if (c == 19) held_seats(WQ_SEATED_NONE, 2);
        if (c == 20) {
            held_seats(3, 2);
            s_rview.allies = 1u << 2;
        }
        if (c == 21) held_seats(2, WQ_SEATED_NONE);
        if (c == 22) {
            held_seats(2, WQ_SEATED_NONE);
            held_base(43, 1, 12, 13);
            s_rrows[40].seat[0].owner = 2;
        }
        if (c == 14 || c == 15) {
            s_hmil.ebx = 5 * 8 + 2;
            s_hmil.ebz = 10;
        }
        if (c == 15) {
            held_base(43, 1, 12, 1);
            held_base(44, 2, 12, 13);
        }
        if (c == 16) held_base(43, 1, 12, 1);
        if (c == 18) held_base(43, 1, 6, 1);
        if (c == 8) {
            s_rrows[40].seat[0].bz = 58;            /* seat 1's row to cell (7, 7) */
            s_rrows[42].seat[0] = s_rrows[40].seat[0];
            s_rrows[42].seat[0].owner = 2;          /* seat 2's in (6, 1) */
            s_rrows[42].seat[0].bx = 6 * 8 + 2;
            s_rrows[42].seat[0].bz = 10;
        }
        if (c == 6) {
            s_rrows[40].seat[0].type = T_SOLAR;     /* the enemy's row a mobile armed type, the seat's a tower */
            s_rth[T_SOLAR].hit_ground = 10;
            s_rth[T_SOLAR].hit_range = 100;
            s_ti[T_SOLAR].flags |= IMPAI_TF_MOBILE;
        }
        CHECK(decide(&out) == 0, "decide");
        s_milp = NULL;
        s_viewp = NULL;
        if (c == 1) memcpy(held_b, s_st.held, sizeof held_b);
        cmd = last_cmd(0);
        got = !cmd || cmd->c.type != T_MEX ? '-' : cmd_x0(&cmd->c) >= 70 ? 'E' : 'H';
        printf("held ground, %s: patch %c\n", NAME[c], got);
        CHECK(got == WANT[c], "held ground, %s: patch %c", NAME[c], got);
    }
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* B3: a constructor at cell (20, 20) busy on an extractor's BUILD at N's site (first cell (52, 20)), ordered at
   GameTime 290. A row put on its way after the order: one move to the anchor, the job ended. With the extractor's
   frame at the site the way is no walk: nothing, unless the row's reach is over the site, which turns it back.
   The commander on the same job: nothing. A busy EXPLORE of cell (72, 24) with the row ahead: moved back and the
   cell failed. No row: nothing. */
static void walk_world(int unit, int kind)
{
    route_world();
    s_spots = NULL;
    s_nspots = 0;
    if (unit == 1) {
        memset(&s_w.unit[1], 0, sizeof s_w.unit[1]);
        unit_add(&s_w, 1, T_COM, 20, 20, 1);
    }
    s_job[1].type = (uint16_t)(unit == 1 ? T_COM : T_CONS);
    s_job[1].target = (uint16_t)WQ_NONE;
    s_job[1].since = 290;
    s_job[1].kind = (uint8_t)kind;
    s_w.unit[1].list[0].target = (uint16_t)WQ_NONE;
    s_w.unit[1].n = 1;
    if (kind == IMPAI_JOB_BUILD) {
        s_job[1].build = T_MEX;
        s_job[1].cx = 52;
        s_job[1].cz = 20;
        s_w.unit[1].list[0].action = A_MOBILEBUILD;
        s_w.unit[1].list[0].type = T_MEX;
        s_w.unit[1].list[0].pos[0] = (2 * 52 + 3) << 19;
        s_w.unit[1].list[0].pos[2] = (2 * 20 + 3) << 19;
    } else {
        s_job[1].build = (uint16_t)WQ_NONE;
        s_job[1].cx = 72;
        s_job[1].cz = 24;
        s_st.explorer = 1;
        s_w.unit[1].list[0].action = A_MOVE;
        s_w.unit[1].list[0].pos[0] = (72 * 16 + 8) << 16;
        s_w.unit[1].list[0].pos[2] = (24 * 16 + 8) << 16;
    }
}

static int moved_home(int from)
{
    int k, n = 0;
    for (k = from; k < s_w.nlog; k++)
        n += s_w.log[k].c.unit == 1 && s_w.log[k].c.action == A_MOVE && (s_w.log[k].c.pos[0] >> 16) == 328 &&
             (s_w.log[k].c.pos[2] >> 16) == 328;
    return n;
}

/* THE HAND-BACK of a turned-back constructor (impai_brain.c) and THE TURN BACK's reach (impai_build.c):
   - the walk across the row turned back, the next decision -- the move to the anchor still the builder's one
     order, no job, the seat now in an energy stall -- lists it idle, and the ladder gives it its next order;
   - with the move and a second node behind it, the list is not the builder's one order: it is not listed;
   - a frame at the site and a row of 100 px at the anchor's block (12, 12) threatening the anchor's 256-px cell
     (1, 1), the builder's own: it turns back to the nearest clear cell in ring order, (1, 0) at (384, 128); with
     that cell's centre block (12, 4) on a component the builder's class does not reach, to the next, (0, 1) at
     (128, 384). */
static void turned_back_case(const char* name, int second_node, int want_orders)
{
    IMPAI_BRAIN_OUT out;
    int from, k, n = 0;
    walk_world(2, IMPAI_JOB_BUILD);
    route_row(IMPAI_LOS, ROW_CELL21);
    CHECK(decide(&out) == 0, "decide");
    CHECK(moved_home(0) == 1 && s_job[1].kind == IMPAI_JOB_NONE, "the walk across the row turns back: %s", name);
    if (second_node) {
        s_w.unit[1].list[1] = s_w.unit[1].list[0];
        s_w.unit[1].n = 2;
    }
    s_w.gametime += 15;
    s_w.econ[0].energy = 100;
    s_w.econ[0].energy_in = 10;
    s_w.econ[0].energy_out = 30;
    from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    for (k = from; k < s_w.nlog; k++) n += s_w.log[k].c.unit == 1;
    printf("turned back, %s: orders to the builder %d, job %u\n", name, n, s_job[1].kind);
    CHECK(want_orders ? n >= 1 && s_job[1].kind != IMPAI_JOB_NONE : n == 0,
          "turned back, %s: %d orders to the builder", name, n);
}

static int turn_back_to(int wall, int32_t* x, int32_t* z)
{
    IMPAI_BRAIN_OUT out;
    int k;
    walk_world(2, IMPAI_JOB_BUILD);
    if (wall) s_labels[4 * (FW_W / 2) + 12] = 3;
    unit_add(&s_w, 5, T_MEX, 52, 20, 0);
    route_row(IMPAI_LOS, 12, 12, 100);
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    for (k = 0; k < s_w.nlog; k++)
        if (s_w.log[k].c.unit == 1 && s_w.log[k].c.action == A_MOVE) {
            *x = s_w.log[k].c.pos[0] >> 16;
            *z = s_w.log[k].c.pos[2] >> 16;
            return 1;
        }
    return 0;
}

static void check_turned_back(void)
{
    int32_t x = 0, z = 0;
    int moved;
    turned_back_case("the move its one order", 0, 1);
    turned_back_case("a node behind the move", 1, 0);
    moved = turn_back_to(0, &x, &z);
    printf("turn back, the anchor's cell threatened: to (%d, %d)\n", x, z);
    CHECK(moved && x == 384 && z == 128, "the turn back goes to the nearest clear cell: (%d, %d)", x, z);
    moved = turn_back_to(1, &x, &z);
    printf("turn back, the nearest clear cell unreachable: to (%d, %d)\n", x, z);
    CHECK(moved && x == 128 && z == 384, "the turn back goes to a cell the builder cannot reach: (%d, %d)", x, z);
}

/* THE WALK CHECK of a reclaim (impai_brain.c): a constructor's RECLAIM_GO or RECLAIM of a 300-metal rock at cell
   (`fcell`, 24), the builder at (`bcell`, 24) or at the anchor's (20, 20) for -1, T_CONS's build reach 120 px, one
   row at block (bx, bz) of `range` px:
   (r1) a RECLAIM_GO from the anchor, the row of ROW_CELL21 on the way: turned back;
   (r2) a RECLAIM from the anchor, 832 px off the rock, the same row: a walk still, turned back;
   (r3) a RECLAIM at work, the builder at (62, 24), 64 px off the rock at (66, 24), a row over the builder's own
        256-px cell (3, 1) alone: work, turned back -- a walk's test leaves the builder's own cell out;
   (r4) the same with the row of ROW_CELL21, on neither the builder, the rock nor the way between: it goes on. */
static int walk_reclaim(int kind, int fcell, int bcell, int bx, int bz, int range)
{
    IMPAI_BRAIN_OUT out;
    int k, from, moved = 0;
    walk_world(2, IMPAI_JOB_EXPLORE);
    s_st.explorer = (uint16_t)WQ_NONE;
    s_w.defs[T_CONS].d.flags |= WQ_DF_RECLAIMER;
    k = feat_add(fcell, 24, 2, 2, 300);
    if (bcell >= 0) {
        const FUNIT keep = s_w.unit[1];
        unit_add(&s_w, 1, T_CONS, bcell, 24, 1);
        s_w.unit[1].list[0] = keep.list[0];
        s_w.unit[1].n = keep.n;
    }
    s_job[1].kind = (uint8_t)kind;
    s_job[1].cx = (int16_t)fcell;
    s_job[1].cz = 24;
    s_job[1].feat = s_w.feat[k].def;
    s_w.unit[1].list[0].action = kind == IMPAI_JOB_RECLAIM ? A_RECLAIM : A_MOVE;
    s_w.unit[1].list[0].pos[0] = (fcell * 16 + 8) << 16;
    route_row(IMPAI_LOS, bx, bz, range);
    from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    for (k = from; k < s_w.nlog; k++) moved += s_w.log[k].c.unit == 1 && s_w.log[k].c.action == A_MOVE;
    s_w.defs[T_CONS].d.flags &= ~WQ_DF_RECLAIMER;
    return moved && s_job[1].kind == IMPAI_JOB_NONE;
}

static void check_walk_reclaim(void)
{
    const int r1 = walk_reclaim(IMPAI_JOB_RECLAIM_GO, 72, -1, ROW_CELL21);
    const int r2 = walk_reclaim(IMPAI_JOB_RECLAIM, 72, -1, ROW_CELL21);
    const int r3 = walk_reclaim(IMPAI_JOB_RECLAIM, 66, 62, 27, 11, 100);
    const int r4 = walk_reclaim(IMPAI_JOB_RECLAIM, 66, 62, ROW_CELL21);
    printf("walk back, the reclaims: go %d, far %d, at work under the row %d, at work beside it %d\n", r1, r2, r3, r4);
    CHECK(r1, "a RECLAIM_GO across the row turns back");
    CHECK(r2, "a RECLAIM beyond its reach across the row turns back");
    CHECK(r3, "a RECLAIM at work with the row over its own cell turns back");
    CHECK(!r4, "a RECLAIM at work with the row elsewhere goes on");
}

static void check_walk_back(void)
{
    IMPAI_BRAIN_OUT out;
    int k, failed;
    walk_world(2, IMPAI_JOB_BUILD);
    route_row(IMPAI_LOS, ROW_CELL21);
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    printf("walk back, the row on the way: moves home %d, job %u\n", moved_home(0), s_job[1].kind);
    CHECK(moved_home(0) == 1 && s_job[1].kind == IMPAI_JOB_NONE, "the walk across the row turns back, its job ended");

    walk_world(2, IMPAI_JOB_BUILD);
    unit_add(&s_w, 5, T_MEX, 52, 20, 0);
    route_row(IMPAI_LOS, ROW_CELL21);
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    CHECK(moved_home(0) == 0 && s_job[1].kind == IMPAI_JOB_BUILD, "a frame at the site: the row on the way is no walk's");

    walk_world(2, IMPAI_JOB_BUILD);
    unit_add(&s_w, 5, T_MEX, 52, 20, 0);
    route_row(IMPAI_LOS, 27, 11, 100);                 /* over cell (3, 1), the site's */
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    CHECK(moved_home(0) == 1 && s_job[1].kind == IMPAI_JOB_NONE, "a frame under the row's reach: turned back");

    walk_world(1, IMPAI_JOB_BUILD);
    route_row(IMPAI_LOS, ROW_CELL21);
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    CHECK(moved_home(0) == 0 && s_job[1].kind == IMPAI_JOB_BUILD, "the commander's walk is the military's");

    walk_world(2, IMPAI_JOB_EXPLORE);
    route_row(IMPAI_LOS, ROW_CELL21);
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    for (k = 0, failed = 0; k < s_st.nfail && k < IMPAI_FAILS; k++) failed += s_st.fail[k].cx == 72 && s_st.fail[k].cz == 24;
    printf("walk back, the explorer: moves home %d, failed %d\n", moved_home(0), failed);
    CHECK(moved_home(0) == 1 && failed == 1 && s_job[1].kind == IMPAI_JOB_NONE, "the explorer turns back, its cell failed");

    walk_world(2, IMPAI_JOB_BUILD);
    CHECK(decide(&out) == 0, "decide");
    CHECK(moved_home(0) == 0 && s_job[1].kind == IMPAI_JOB_BUILD, "no row: the walk goes on");
    check_turned_back();
}

/* A RADAR CONTACT on the way: route_row's row turned into a contact never identified (type WQ_NONE, owner 0xFF),
   held by `how`, and the military's state naming the turret as what such a contact counts as (radar_as) */
static IMPAI_MIL s_rmil;
static void radar_row(unsigned how, uint16_t radar_as)
{
    route_row(how, ROW_CELL21);
    s_rrows[40].seat[0].type = (uint16_t)WQ_NONE;
    s_rrows[40].seat[0].owner = 0xFF;
    impai_mil_reset(&s_rmil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], NULL, 0);
    s_rmil.radar_as = radar_as;
    s_milp = &s_rmil;
}

/* B2 and B3 over a radar contact never identified (impai_row_unidentified_now), counted as the military's
   substitute: held now by the radar pass, the constructor takes F over N and a BUILD walk across it turns back;
   with no substitute (WQ_NONE) or a recorded type past the table, N; a contact's ghost, not held now, N. The same
   predicate puts the contact in the military's picture (check_mil_radar_as). */
static void check_route_radar(void)
{
    static const struct { unsigned how; uint16_t as; char want; const char* what; } CASE[4] = {
        { IMPAI_RADAR, T_TURRET, 'F', "radar now, as the turret" },
        { IMPAI_RADAR, (uint16_t)WQ_NONE, 'N', "radar now, no substitute" },
        { IMPAI_RADAR, 0x7FFF, 'N', "radar now, a substitute past the table" },
        { IMPAI_GHOST, T_TURRET, 'N', "a ghost" } };
    IMPAI_BRAIN_OUT out;
    int c;
    for (c = 0; c < 4; c++) {
        char got;
        route_world();
        radar_row(CASE[c].how, CASE[c].as);
        CHECK(decide(&out) == 0, "decide");
        s_viewp = NULL;
        s_milp = NULL;
        got = route_patch();
        printf("route, a radar contact, %s: patch %c\n", CASE[c].what, got);
        CHECK(got == CASE[c].want, "route, a radar contact, %s: patch %c", CASE[c].what, got);
    }
    walk_world(2, IMPAI_JOB_BUILD);
    radar_row(IMPAI_RADAR, T_TURRET);
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    s_milp = NULL;
    printf("walk back, a radar contact on the way: moves home %d, job %u\n", moved_home(0), s_job[1].kind);
    CHECK(moved_home(0) == 1 && s_job[1].kind == IMPAI_JOB_NONE, "the walk across a radar contact turns back");

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
        CHECK(out.roots <= s_plan->roots && out.sites <= s_plan->site_tests &&
              out.snaps <= s_plan->snaps && out.queued <= s_plan->queue_max, "decision %d's budgets", i);
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

/* A game in the economy's own time: impai-eco's seat 1 on Comet Catcher, on the Hard row (s_plan), the level the
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
typedef char egame_commander_is_fw_commander[E_COM == FW_COMMANDER && T_COM == FW_COMMANDER ? 1 : -1];

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
    int   done_at[FW_SLOTS];            /* the GameTime a unit was finished, 0 at the start          */
    int   pass_t;                       /* the GameTime the last pass ended                          */
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
    s_scanned = 1;
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
    CHECK(content_set(&s_w, E_N, roles) == E_N, "tinfo");
    s_ti[E_PT].klass = IMPAI_K_SRAIDER;
    impai_brain_reset(&s_st, s_job, FW_SLOTS);
    memset(s_pref, 0, sizeof s_pref);
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
    s_eg.done_at[f] = s_w.gametime;
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
        s_eg.pass_t = s_w.gametime;
        s_eg.e_acc_in = s_eg.m_acc_in = s_eg.e_acc_out = s_eg.m_acc_out = 0;
    }
    s_w.econ[0].metal = s_eg.metal; s_w.econ[0].energy = s_eg.energy;
    s_w.econ[0].metal_max = s_eg.mstore; s_w.econ[0].energy_max = s_eg.estore;
    s_w.econ[0].metal_in = s_eg.m_in; s_w.econ[0].energy_in = s_eg.e_in;
    s_w.econ[0].metal_out = s_eg.m_out; s_w.econ[0].energy_out = s_eg.e_out;
}

/* What one economy game ends with: the units standing at 5400 by type, the orders' counts, the sites and snaps
   refused (IMPAI_BRAIN_OUT.failed, summed), the type of the first extractor ordered (0 none), and the GameTimes the
   first constructor was queued and impai-eco's bar first held, 0 never. */
typedef struct EGAME_END {
    int n[E_N];
    int orders, queues, wet, snaps, failed, mex_first;
    int queued_at, bar_at;
} EGAME_END;

/* a change to the economy game's world after egame_world, NULL none */
static void (*s_eg_tweak)(void);

/* impai-eco's bar: three extractors, an energy building and a plant finished, and a constructor queued */
static int egame_bar_holds(int queues)
{
    int s, n[E_N] = { 0 };
    for (s = 1; s < 32; s++) if (s_w.unit[s].live && s_w.unit[s].finished) n[s_w.unit[s].type]++;
    return n[E_MEX] >= 3 && n[E_SOLAR] + n[E_WIND] >= 1 && n[E_LAB] + n[E_VP] >= 1 && queues >= 1;
}

/* The game on `metal_map`'s numbers, the commander at cell (cx, cz); its orders and the line at 5400 printed.
   With a queue (s_xp), each tick gives the orders due before the decision, as the game's turn does
   (tagpu_impai.c, impai_turn), and the orders are counted as the fake takes them; the most builders busy after a
   decision is busy_max. */
static int s_busy_max;
static EGAME_END economy_game(const char* name, int metal_map, int cx, int cz)
{
    WORLDQ q = { &FAKE, &s_w, 0 };
    IMPAI_BRAIN_OUT out;
    EGAME_END e;
    int s;
    memset(&e, 0, sizeof e);
    s_busy_max = 0;
    egame_world(metal_map);
    if (s_eg_tweak) s_eg_tweak();
    unit_add(&s_w, 1, E_COM, cx, cz, 1);
    if (s_xp) impai_exec_reset(s_xp, s_persona);
    printf("economy game, %s\n", name);
    for (s_w.gametime = 1; s_w.gametime <= 5400; s_w.gametime++) {
        const int from = s_w.nlog;
        int k;
        if (s_xp && s_xp->npend) impai_exec_apply(s_xp, &q, s_persona, (uint32_t)s_w.gametime, &s_rc);
        if (impai_brain_due((unsigned)s_w.gametime, 0, s_persona->look_eco)) {
            CHECK(decide(&out) == 0, "decide at %d", s_w.gametime);
            e.snaps += out.snaps;
            e.failed += out.failed;
            if (out.busy > s_busy_max) s_busy_max = out.busy;
        }
        if (s_w.nlog > from) {
            for (k = from; k < s_w.nlog; k++) {
                e.orders++;
                if (!e.mex_first && s_w.log[k].c.verb == WQ_CMD_NEW && s_w.log[k].c.type < E_N &&
                    s_ti[s_w.log[k].c.type].cat == IMPAI_C_MEX)
                    e.mex_first = s_w.log[k].c.type;
                if (s_w.log[k].c.verb == WQ_CMD_QUEUE && !e.queues++) e.queued_at = s_w.gametime;
                e.wet += s_w.log[k].c.type == E_SY || s_w.log[k].c.type == E_UWMEX;
                if (!s_quiet) cmd_print(&s_w.log[k]);
            }
        }
        if (s_w.nlog >= FW_LOG - 16) s_w.nlog = 0;
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
       standing by the decision of 840), then the energy that income asks for, CORVP from 3225 with a CORCV queued
       when it stands, at 4080. Each standing extractor pays the 2.295 its nine cells of 255 give a pass, one finished
       inside the last pass the ticks it stood of it, the commander 1. */
    e = economy_game("Core Prime Industrial Area", 1, FW_W / 2, FW_H / 2);
    CHECK(!e.snaps, "no snap onto a patch on a metal map: %d", e.snaps);
    {
        float want = 1.0f;
        int s;
        for (s = 1; s < 32; s++)
            if (s_w.unit[s].live && s_w.unit[s].finished && s_w.unit[s].type == E_MEX && s_eg.done_at[s] <= s_eg.pass_t) {
                const int from = s_eg.done_at[s] > s_eg.pass_t - 29 ? s_eg.done_at[s] : s_eg.pass_t - 29;
                want += 2.295f * (float)(s_eg.pass_t - from + 1) / 30.0f;
            }
        CHECK(fabsf(s_eg.m_in - want) < 0.01f, "each extractor on the metal map pays 0.001 x 9 x 255 a pass: metal_in %.3f, "
              "%.3f from %d", s_eg.m_in, want, e.n[E_MEX]);
    }
    CHECK(e.n[E_MEX] >= 4, "four extractors by 5400 on the metal map: %d", e.n[E_MEX]);
    CHECK(e.n[E_SOLAR] + e.n[E_WIND] >= 1, "an energy building by 5400 on the metal map: %d",
          e.n[E_SOLAR] + e.n[E_WIND]);
    CHECK(e.n[E_LAB] + e.n[E_VP] >= 1 && e.queues >= 1,
          "a finished factory with a constructor queued by 5400 on the metal map: %d, %d", e.n[E_LAB] + e.n[E_VP],
          e.queues);
    CHECK(!e.wet, "no water-only type ordered on the metal map: %d", e.wet);
}

/* ---- THE EXTRACTOR FOR THE PATCH (impai_build.c) on the economy's types ---- */

/* Total Mayhem's shape: the water-only extractor extracts more than the land one (ARMUWMEX 0.001125 against ARMMEX
   0.0011 in Mayhem 11.3.0) */
static void tweak_richer_water_mex(void)
{
    s_w.defs[E_UWMEX].d.strength = s_w.defs[E_UWMEX].e.extracts = 0.0011f;
    s_ti[E_UWMEX].mgain = 0.0011f;
}

/* the economy's world with the commander at cell (100, 44), beside Comet Catcher's patch at (105, 43)
   (EPATCHES[1], the best for it by distance), which lies in water `depth` deep over cells 102..111 x 40..49 with
   the sea at 50 (depth 0: no water anywhere); `menu` the commander's (NULL: the economy's own) */
#define MEXG_X0 102
#define MEXG_X1 111
static void mex_ground_world(int depth, const uint16_t* menu, int n)
{
    egame_world(0);
    if (depth) {
        s_w.sea = 50;
        water_add(&s_w, MEXG_X0, 40, MEXG_X1, 49, depth);
    }
    if (menu) menu_set(&s_w, E_COM, menu, n);
    unit_add(&s_w, 1, E_COM, 100, 44, 1);
    s_w.gametime = 15;
}

/* the type of the NEW build order of an extractor since `from`, 0 none, its first cell into (x0, z0) */
static int mex_ordered(int from, int* x0, int* z0)
{
    int k;
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.verb == WQ_CMD_NEW && s_w.log[k].c.type < E_N && s_ti[s_w.log[k].c.type].cat == IMPAI_C_MEX) {
            *x0 = cmd_x0(&s_w.log[k].c);
            *z0 = cmd_z0(&s_w.log[k].c);
            return s_w.log[k].c.type;
        }
    return 0;
}

static void check_mex_ground(void)
{
    static const uint16_t LAND[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP, E_SY };
    static const uint16_t BOTH[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP, E_SY, E_UWMEX };
    static const uint16_t WATER_ONLY[] = { E_UWMEX, E_SOLAR };
    IMPAI_BRAIN_OUT out;
    EGAME_END e;
    int t, x0 = -1, z0 = -1, from;
    /* A. The richer water type on a dry map: every extractor is the land one, and no snap or site is refused */
    s_quiet = 1;
    s_eg_tweak = tweak_richer_water_mex;
    e = economy_game("Comet Catcher, the water extractor richer", 0, 324 - EGAME_DX, 35);
    s_eg_tweak = NULL;
    s_quiet = 0;
    CHECK(e.mex_first == E_MEX && !e.wet, "the land extractor first and no water type on the dry map: %d, %d",
          e.mex_first, e.wet);
    CHECK(!e.failed, "no site or snap refused: %d", e.failed);
    CHECK(e.n[E_MEX] >= IMPAI_PLAN_ROW.open_mex, "the extractors reach open_mex: %d", e.n[E_MEX]);
    /* B. A wet patch 30 deep: the water type there, the land type at the dry patch beside the constructor */
    mex_ground_world(30, NULL, 0);
    CHECK(decide(&out) == 0, "decide");
    t = mex_ordered(0, &x0, &z0);
    CHECK(t == E_UWMEX && x0 >= MEXG_X0 && x0 + 2 <= MEXG_X1 && !out.failed,
          "the water extractor at the wet patch: type %d at %d,%d, %u refused", t, x0, z0, out.failed);
    menu_set(&s_w, E_CK, BOTH, (int)(sizeof BOTH / sizeof BOTH[0]));
    unit_add(&s_w, 2, E_CK, 80, 20, 1);
    s_w.gametime = 30;
    from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide");
    t = mex_ordered(from, &x0, &z0);
    CHECK(t == E_MEX && x0 >= 76 && x0 <= 78 && !out.failed,
          "the land extractor at the dry patch: type %d at %d,%d, %u refused", t, x0, z0, out.failed);
    /* C. The wet patch with no water type on the menu: the land type is tried there, and the site test refuses it */
    mex_ground_world(30, LAND, (int)(sizeof LAND / sizeof LAND[0]));
    CHECK(decide(&out) == 0, "decide");
    CHECK(out.snaps == 1 && out.failed == 1 && !mex_ordered(0, &x0, &z0),
          "the land extractor tried at the wet patch and refused: %u snaps, %u refused", out.snaps, out.failed);
    /* D. A patch 5 deep, short of the water type's 15: the land type, refused below the sea; the patch then waits
       fail_t and the commander takes a dry one */
    mex_ground_world(5, NULL, 0);
    CHECK(decide(&out) == 0, "decide");
    CHECK(out.snaps == 1 && out.failed == 1 && !mex_ordered(0, &x0, &z0),
          "a shallow patch takes the land type, which is refused: %u snaps, %u refused", out.snaps, out.failed);
    s_w.gametime += 15;
    from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide");
    t = mex_ordered(from, &x0, &z0);
    CHECK(t == E_MEX && (x0 < MEXG_X0 || x0 > MEXG_X1) && !out.failed,
          "the shallow patch is not tried again within fail_t: type %d at %d,%d, %u refused", t, x0, z0, out.failed);
    /* E. The water type alone on a dry map: no extractor tried, and the ladder goes on to energy */
    mex_ground_world(0, WATER_ONLY, (int)(sizeof WATER_ONLY / sizeof WATER_ONLY[0]));
    s_w.econ[0].energy = 500;
    s_w.econ[0].energy_max = 1000;
    s_w.econ[0].energy_out = 10;
    CHECK(decide(&out) == 0, "decide");
    CHECK(!out.snaps && !out.failed && !mex_ordered(0, &x0, &z0) && type_orders(E_SOLAR) == 1,
          "no extractor tried, the solar collector built: %u snaps, %u refused, %d solar", out.snaps, out.failed,
          type_orders(E_SOLAR));
    /* Q1. Two land types: the most metal per value, not the most metal (TA Twilight's ARMAMEX, 0.00125 for 214,
       against ARMMEX's 0.001 for 82) */
    mex_ground_world(0, BOTH, (int)(sizeof BOTH / sizeof BOTH[0]));
    s_w.defs[E_UWMEX].e.minwater = 0;
    s_ti[E_UWMEX].flags &= (uint8_t)~IMPAI_TF_WATER;
    s_ti[E_UWMEX].mgain = 0.00125f;
    s_ti[E_UWMEX].value = 2.6f * s_ti[E_MEX].value;
    CHECK(decide(&out) == 0, "decide");
    t = mex_ordered(0, &x0, &z0);
    CHECK(t == E_MEX, "the most metal per value: type %d", t);
}

/* F. THE EXPLORE RULE: a constructor whose only extractor suits no patch -- a water-only type on the dry patches
   -- explores, where it would otherwise stand on patches it cannot build on */
static void check_mex_explore(void)
{
    IMPAI_BRAIN_OUT out;
    brain_world();
    s_w.defs[T_MEX].e.minwater = 15;
    s_ti[T_MEX].flags |= IMPAI_TF_WATER;
    unit_add(&s_w, 2, T_CONS, 30, 30, 1);
    s_w.gametime = 315;
    CHECK(decide(&out) == 0, "decide");
    CHECK(s_job[2].kind == IMPAI_JOB_EXPLORE && s_st.explorer == 2 && !mex_orders() && !out.snaps,
          "the constructor explores: job %d, explorer %u, %d extractor orders, %u snaps", s_job[2].kind,
          (unsigned)s_st.explorer, mex_orders(), out.snaps);
}

/* THE PLAN (impai_brain.c, IMPAI_PLAN_ROW): the economy game with a military, at Hard's persona, must be A1b's
   game exactly -- the plan's economy is A1b's row -- and A1b's bar is judged there; and the row's own rules: the
   governor's, the pressure outliving the longest gap between two military decisions any persona may have (150
   ticks), the ground defences', and the factories' reserve within half the decision's commands (THE SPLIT). */
static void check_plan(void)
{
    static IMPAI_MIL mil;
    const IMPAI_PLAN* p = &IMPAI_PLAN_ROW;
    EGAME_END ref, e;
    int t;
    s_quiet = 1;
    ref = economy_game("Comet Catcher, no military", 0, 324 - EGAME_DX, 35);
    impai_mil_reset(&mil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
    mil.naval = 0;              /* the economy game runs no military; its dry world's judgement is 0 */
    s_milp = &mil;
    e = economy_game("Comet Catcher, Hard", 0, 324 - EGAME_DX, 35);
    for (t = 1; t < E_N; t++)
        CHECK(e.n[t] == ref.n[t], "Hard's economy is A1b's: type %d %d against %d", t, e.n[t], ref.n[t]);
    CHECK(e.queues == ref.queues, "Hard's queues are A1b's: %d against %d", e.queues, ref.queues);
    CHECK(p->cons_open <= p->cons_max && p->army_per_cons >= 1,
          "the plan's governor: cons_open <= cons_max, army_per_cons >= 1");
    CHECK(p->stall_pm > 0 && p->stall_pm < (unsigned)(SURPLUS_FRAC * 1000.0f) && p->hold_fac >= 1 &&
              p->hold_fac <= IMPAI_IDLE_CAP,
          "the plan's hold: 0 < stall_pm < SURPLUS_FRAC x 1000, 1 <= hold_fac <= IMPAI_IDLE_CAP");
    CHECK(p->press_k > 0.0f && p->press_hold >= 2u * 150u,
          "the plan's pressure outlives the gap between two military decisions at the longest look");
    CHECK(p->def_max <= 4 && p->def_v > 0.0f, "the plan's ground defences: def_max <= 4, def_v > 0");
    CHECK(p->sensor_max >= 1 && p->sensor_max <= IMPAI_SENSORS / 2, "the plan's sensors: 1 <= sensor_max <= %d",
          IMPAI_SENSORS / 2);
    CHECK(p->queue_max <= p->roots / 2, "the plan's factory reserve within half its commands: %u of %u",
          p->queue_max, p->roots);
    CHECK(p->roots <= IMPAI_ROOTS_MAX && p->mil_roots <= IMPAI_MIL_ROOTS_MAX && p->rally_px <= p->comm_r,
          "the plan within its bounds, its rally point within comm_r");
    s_milp = NULL;
    s_quiet = 0;
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
    s_threat[t].dps_ground = s_threat[t].hit_ground = dmg;
    s_threat[t].range = s_threat[t].hit_range = range;
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

static uint32_t s_allies;           /* seat 0's allies in the military's view, bit j: seat j; mil_world clears it */
static void mil_world(void)
{
    TASIM_ROLE_OUT roles[M_N];
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
    CHECK(content_set(&s_w, M_N, roles) == M_N, "the military's tinfo");
    s_ti[M_RAIDER].klass = IMPAI_K_LRAIDER;
    s_ti[M_TANK].klass = IMPAI_K_LASSAULT;
    s_ti[M_SCOUT].klass = IMPAI_K_LSCOUT;
    s_ti[M_AA].klass = IMPAI_K_LAA;
    s_ti[M_DEST].klass = IMPAI_K_LASSAULT;
    for (t = T_COM; t < M_N; t++) s_ti[t].attack = s_w.defs[t].attack;
    s_ti[M_AA].flags = (uint8_t)((s_ti[M_AA].flags & ~IMPAI_TF_HITDRY) | IMPAI_TF_HITAIR);
    s_ti[M_SUB].flags = (uint8_t)((s_ti[M_SUB].flags & ~IMPAI_TF_HITDRY) | IMPAI_TF_HITWET);
    s_ti[M_DEST].flags |= IMPAI_TF_HITWET;
    s_fc.t = s_ft; s_fc.nt = M_N;
    s_fc.w = s_fwp; s_fc.nw = M_N;
    s_fc.d = NULL; s_fc.nd = 0;
    memset(s_rows, 0, sizeof s_rows);
    memset(s_hits, 0, sizeof s_hits);
    s_st.ax = 20;
    s_st.az = 20;
    impai_mil_reset(&s_mil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
    s_mil_reach = NULL;
    s_allies = 0;
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

/* an enemy army of `n` tanks seat 0 saw once and lost sight of: ghost rows at the map's far corner, which the
   picture passes over (a mobile not seen now) and THE LEAD counts, so a case of a rule outside the lead holds the
   seat out of it. Slots ENEMY_AWAY.. ENEMY_AWAY + n - 1, n at most FW_SLOTS - ENEMY_AWAY. */
#define ENEMY_AWAY 2400
static void enemy_away(int n)
{
    int k;
    for (k = 0; k < n && ENEMY_AWAY + k < FW_SLOTS; k++)
        enemy(ENEMY_AWAY + k, M_TANK, 500 - 2 * (k % 8), 250 - 2 * (k / 8), IMPAI_GHOST, 0);
}

/* seat 0's radar pass found the unit slot `slot` holds now: its record names the slot's incarnation */
static void radar_found(int slot)
{
    s_hits[slot].birth = s_w.birth[slot];
    s_hits[slot].bx = fblock(s_w.unit[slot].pos[0]);
    s_hits[slot].bz = fblock(s_w.unit[slot].pos[2]);
    s_hits[slot].known = 1;
}

/* the military bucket's actions mil_decide passes: -1, no budget, unless a case of THE BUDGET sets it */
static int s_macts = -1;

static int mil_decide(const IMPAI_PLAN* lv, int with_content, IMPAI_MIL_OUT* out)
{
    WORLDQ q = { &FAKE, &s_w, 0 };
    IMPAI_VIEW v;
    IMPAI_FCONTENT none;
    IMPAI_MIL_IN in;
    int rc;
    memset(&none, 0, sizeof none);
    memset(&v, 0, sizeof v);
    v.seat = 0;
    v.rows = s_rows;
    v.nrows = FW_SLOTS;
    v.allies = s_allies;
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
    in.plan = lv;
    in.persona = s_persona;
    in.acts = s_macts;
    in.reach = s_mil_reach;
    in.scratch = s_mscratch; in.scratch_bytes = impai_mil_scratch_bytes();
    s_w.src = IMPAI_SRC_MIL;
    rc = impai_mil_decide(&in, out);
    s_w.src = IMPAI_SRC_ECO;
    return rc;
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

/* THE BASE'S DEFENCE AND THE FIGHT: eight tanks at the base, six enemy tanks seen 256 px off it, a seventh only a
   ghost. At Hard (react_confirm 1) the first decision makes the FORM squad DEFEND and orders the fight with no
   prediction (THE HOME HOLDS), every target one the seat sees now, and EVERY MEMBER FIGHTS: the tank at 20 % of its
   health is ordered at a target like the rest and given no move, so every member the squad counts is one that
   fights (fight). */
static void check_mil_defence(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    for (k = 0; k < 8; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    s_w.unit[8].hp = 300;                        /* 20 % of a tank's health */
    for (k = 0; k < 6; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40, IMPAI_LOS, 1);
    enemy(1306, M_TANK, 20, 40, IMPAI_GHOST, 1);
    enemy(1307, T_MEX, 400, 24, IMPAI_GHOST, 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the defence decision runs");
    mil_print("defence", &o, from);
    CHECK(o.army == 8 && o.enemies == 7 && o.contacts == 1 && o.threats == 1, "the picture: %u army, %u enemies",
          o.army, o.enemies);
    CHECK(o.defends == 1 && s_mil.sq[0].task == IMPAI_SQ_DEFEND, "the FORM squad defends");
    CHECK(!o.predictions && !o.retreats, "the home holds: no prediction, no retreat");
    CHECK(count_action(from, A_ATTACK, -1) == 8 && targets_seen(from), "eight attack orders, every target seen now");
    CHECK(count_action(from, A_ATTACK, 8) == 1 && count_action(from, A_MOVE, 8) == 0,
          "the damaged tank fights: an attack order, no move");
    from = s_w.nlog;
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the second decision runs");
    CHECK(count_action(from, A_ATTACK, -1) == 0, "an order the unit's head already says is not given again");
    CHECK(count_action(from, A_MOVE, 8) == 0, "the damaged tank is given nothing else");
    mil_print("defence again", &o, from);
}

/* A BASE THREAT IS AN ARMED MOBILE ON THE SEAT'S SIDE (impai_mil.c, base_threats): the anchor at (648, 648) px,
   an own extractor 3 000 px east of it, twelve tanks south-west of the anchor out of reach of everything below.
   (a) An enemy extractor and an enemy constructor 150 px from the own extractor and an enemy constructor 300 px
   east of the anchor, all seen now: no threat, and the FORM squad, worth wave_value, attacks. (b) The same with
   an armed enemy tank 300 px east of the anchor: one threat, and the FORM squad defends at the tank's block.
   (c) A flyer there instead of the tank, which the tanks have no anti-air slot to hurt: one threat, no DEFEND. */
static void threat_world(int extra)
{
    int k;
    mil_world();
    s_st.ax = 40;
    s_st.az = 40;
    s_ft[T_CONS].hp = 500;                      /* the constructor in the fight content: mobile, unarmed */
    s_ft[T_CONS].value = s_ti[T_CONS].value;
    s_ft[T_CONS].speed = 70000;
    s_ft[T_CONS].flags = IMPAI_FT_MOBILE;
    for (k = 0; k < 12; k++) own(1 + k, M_TANK, 8 + 2 * (k % 6), 60 + 2 * (k / 6));
    own(20, T_MEX, 228, 40);
    enemy(1300, T_MEX, 237, 40, IMPAI_LOS, 1);
    enemy(1301, T_CONS, 228, 49, IMPAI_LOS, 1);
    enemy(1302, T_CONS, 59, 40, IMPAI_LOS, 1);
    if (extra == 1) enemy(1303, M_TANK, 59, 42, IMPAI_LOS, 1);
    if (extra == 2) {
        s_ft[M_SCOUT].flags |= IMPAI_FT_FLIES;
        enemy(1303, M_SCOUT, 59, 42, IMPAI_LOS, 1);
    }
}

static void check_mil_base_threat(void)
{
    static const char* const NAME[3] = { "structures and builders", "an armed tank", "a flyer the tanks cannot hurt" };
    IMPAI_MIL_OUT o;
    int extra, k;
    for (extra = 0; extra < 3; extra++) {
        int defend = -1;
        threat_world(extra);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the base threat decision runs");
        mil_print(NAME[extra], &o, s_w.nlog);
        for (k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_DEFEND) defend = k;
        if (extra == 0)
            CHECK(o.threats == 0 && !o.defends && o.attacks == 1, "no threat, the FORM squad attacks: threats %u",
                  o.threats);
        if (extra == 1)
            CHECK(o.threats == 1 && o.defends == 1 && defend >= 0 && s_mil.sq[defend].tx == 29 &&
                      s_mil.sq[defend].tz == 21,
                  "the tank is a threat, DEFEND at its block (%d, %d)", defend >= 0 ? s_mil.sq[defend].tx : -1,
                  defend >= 0 ? s_mil.sq[defend].tz : -1);
        if (extra == 2) CHECK(o.threats == 1 && !o.defends && defend < 0, "a threat the squad cannot hurt: no DEFEND");
    }
}

/* A TARGET GONE SINCE ITS ROW, on the Hard row: eight tanks against six enemy tanks the rows say are seen now, of which the
   two nearest the squad died since the slice last visited their slots (their rows still read LOS) and a third
   left the seat's sight. Every tank, the damaged one too, is ordered at one of the three still there, none at a
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
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the decision runs");
    mil_print("gone targets", &o, from);
    CHECK(o.enemies == 6 && o.contacts == 1, "the rows still hold all six");
    CHECK(o.refused == 0, "no order is refused: %u", o.refused);
    CHECK(count_action(from, A_ATTACK, -1) == 8, "eight attack orders");
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
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the decision runs");
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
    IMPAI_PLAN lv = IMPAI_PLAN_ROW;
    IMPAI_MIL_OUT o;
    int k, from, far;
    lv.mil_roots = 6;
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
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the decision runs");
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
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the decision runs");
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
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the decision runs");
    mil_print("anti-air against radar contacts", &o, from);
    CHECK(count_action(from, A_ATTACK, -1) == 6 && attacks_answered(from), "the six tanks attack");
    CHECK(o.refused == 2 && o.roots == 6 && s_w.nlog - from == 6,
          "the resolver refuses the K-bots: %u refused, %u orders", o.refused, o.roots);
}

/* RETREAT: two raiders against ten tanks 3 150 px from the anchor, past the anchor box, so no base threat: they
   predict below retreat_margin and fall back; with no prediction to be had (no content) the squad keeps its task
   and fights. */
static void check_mil_retreat(void)
{
    IMPAI_MIL_OUT o;
    int k, from, content;
    for (content = 1; content >= 0; content--) {
        mil_world();
        own(1, M_RAIDER, 200, 100);
        own(2, M_RAIDER, 202, 100);
        for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 196 + 2 * k, 116, IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, content, &o) == 0, "the retreat decision runs");
        mil_print(content ? "retreat" : "retreat, no content", &o, from);
        if (content)
            CHECK(o.retreats == 1 && count_action(from, A_MOVE, -1) == 2 && !count_action(from, A_ATTACK, -1) &&
                      s_mil.lost_t == (uint32_t)s_w.gametime,
                  "two raiders against ten tanks fall back, a prediction lost now (THE LEAD's evidence)");
        else
            CHECK(!o.retreats && o.refused_predictions == 1 && count_action(from, A_ATTACK, -1) == 2,
                  "a refused prediction changes no task");
    }
}

/* THE HOME HOLDS (impai_mil.c, home_holds): the same two raiders and ten tanks 1 810 px from the anchor, inside the
   anchor box, where the tanks threaten the base. The FORM squad defends, and as DEFEND in the box it fights with no
   prediction: no retreat, both raiders attack. At the next decision the raiders stand 2 380 px from the anchor,
   past the box, 264 px from the nearest tank, which still threatens the base: the DEFEND squad predicts and
   retreats. */
static void check_mil_home_holds(void)
{
    IMPAI_MIL_OUT o;
    int k, from;
    mil_world();
    own(1, M_RAIDER, 100, 100);
    own(2, M_RAIDER, 102, 100);
    for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 96 + 2 * k, 116, IMPAI_LOS, 1);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the home holds decision runs");
    mil_print("the home holds", &o, from);
    CHECK(o.threats == 1 && o.defends == 1 && !o.retreats && !o.predictions && count_action(from, A_ATTACK, 1) == 1 &&
              count_action(from, A_ATTACK, 2) == 1,
          "two raiders defending in the anchor box hold and attack: %u retreats, %u predictions", o.retreats,
          o.predictions);
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    own(1, M_RAIDER, 130, 120);
    own(2, M_RAIDER, 132, 120);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the defence past the box decision runs");
    mil_print("a defence past the anchor box", &o, from);
    CHECK(o.threats == 1 && o.retreats == 1 && !count_action(from, A_ATTACK, -1),
          "a DEFEND squad past the anchor box retreats: %u retreats", o.retreats);
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
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the attack decision runs");
    mil_print("attack", &o, from);
    CHECK(o.attacks == 1 && s_mil.waves[TASIM_LAYER_LAND] == 1 && s_mil.ebx == 200 && s_mil.ebz == 100,
          "one wave to the estimate (%d, %d)", s_mil.ebx, s_mil.ebz);
    CHECK(count_action(from, A_MOVE, -1) == 12 && !count_action(from, A_ATTACK, -1), "the twelve travel to the target");
    CHECK(!o.scouts, "no scout: the whole FORM squad went");
}

/* THE GOAL'S SIDE (impai_mil.c, side_goal): check_mil_attack's twelve tanks and an extractor remembered at cell
   (150, 100), a turret remembered beside it (armed, range 400, 4 000 hp, 150 a shot, value 300) and twenty solar plants
   remembered (unarmed, 20 000 hp each, value 200); a plant beside the turret (unarmed, 500 hp, value 50) holds the
   wave's goal there (THE GOAL: the plants before the estimate's structures):
   (a) the solars within GOAL_SIDE_PX of the goal: the wave is predicted against them and the turret, as THE
       STAY-OR-GO weighs a base's contact at the goal, and does not go;
   (b) no solar: the turret alone, the wave goes;
   (c) the solars 3 000 px from the goal: not the goal's, the wave goes. */
static void check_mil_goal_side(void)
{
    static const char* const NAME[3] = { "(a) the solars at the goal", "(b) the turret alone",
                                         "(c) the solars far off" };
    IMPAI_MIL_OUT o;
    int k, c, from;
    for (c = 0; c < 3; c++) {
        mil_world();
        s_w.defs[T_TURRET].hp = s_ft[T_TURRET].hp = 4000;
        s_w.defs[T_TURRET].armed = 1;
        s_ft[T_TURRET].value = 300.0f;
        s_ft[T_TURRET].w[0] = T_TURRET;
        s_fwp[T_TURRET].range = 400;
        s_fwp[T_TURRET].reload = 30;
        s_fwp[T_TURRET].burst = 1;
        s_fwp[T_TURRET].dmg = 150;
        s_threat[T_TURRET].dps_ground = s_threat[T_TURRET].hit_ground = 150;
        s_threat[T_TURRET].range = s_threat[T_TURRET].hit_range = 400;
        s_threat[T_TURRET].layer = TASIM_LAYER_LAND;
        s_w.defs[T_SOLAR].hp = s_ft[T_SOLAR].hp = 20000;
        s_ft[T_SOLAR].value = 200.0f;
        s_w.defs[T_FACTORY].hp = s_ft[T_FACTORY].hp = 500;
        s_ft[T_FACTORY].value = 50.0f;
        for (k = 0; k < 12; k++) own(1 + k, M_TANK, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
        enemy(1300, T_MEX, 150, 100, IMPAI_GHOST, 1);
        enemy(1301, T_TURRET, 154, 100, IMPAI_GHOST, 1);
        enemy(1302, T_FACTORY, 150, 104, IMPAI_GHOST, 1);
        for (k = 0; c != 1 && k < 20; k++)
            enemy(1310 + k, T_SOLAR, (c ? 340 : 160) + 6 * (k % 5), (c ? 186 : 86) + 6 * (k / 5), IMPAI_GHOST, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the goal's side decision runs");
        mil_print(NAME[c], &o, from);
        printf("the goal's side, %s: goal (%d, %d)\n", NAME[c], s_mil.ebx, s_mil.ebz);
        if (!c)
            CHECK(!o.attacks && o.predictions >= 1 && !o.refused_predictions, "%s: no wave, %u attacks", NAME[c],
                  o.attacks);
        else
            CHECK(o.attacks == 1 && !o.refused_predictions, "%s: the wave goes, %u attacks", NAME[c], o.attacks);
    }
}

/* THE ENEMY BASE'S ESTIMATE (impai_mil.c, picture), two starts besides the seat's, thirty tanks at home:
   (a) nothing known: the nearer other start, EST_START, no lead;
   (b) an extractor's ghost known: its block, EST_STRUCT, and THE LEAD begins;
   (c) the ghost gone: its block kept, EST_KEPT, and the lead begun at (b) holds;
   (d) the same, the lead not held: a kept estimate begins none;
   (e) a second structure seen elsewhere: the estimate follows it, EST_STRUCT, the lead begins;
   (f) a recorded est_seen past the enum, nothing known, the lead held: the nearer start, EST_START, the lead ends.
   The economy's reading of such a byte: check_plant_choice's starts. */
static void check_mil_estimate(void)
{
    static const WQ_START STARTS[3] = { { 400, 400, 0, 0 }, { 3216, 1616, 1, 0 }, { 7216, 3216, 2, 0 } };
    static const int LEAD[6] = { 0, 1, 1, 0, 1, 0 };
    IMPAI_MIL_OUT o;
    int c;
    static const char* const NAME[6] = { "(a) nothing known", "(b) an extractor known", "(c) its ghost gone, held",
                                         "(d) its ghost gone, not held", "(e) another structure",
                                         "(f) a byte past the enum" };
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 3;
    for (c = 0; c < 30; c++) own(1 + c, M_TANK, 24 + 2 * (c % 8), 24 + 2 * (c / 8));
    for (c = 0; c < 6; c++) {
        if (c == 1) enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 0);
        if (c == 2) s_rows[1300].seat[0].how = 0;
        if (c == 3) s_mil.lead = 0;
        if (c == 4) enemy(1301, T_MEX, 300, 60, IMPAI_GHOST, 0);
        if (c == 5) {
            s_rows[1301].seat[0].how = 0;
            s_mil.est_seen = 7;
        }
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the estimate's decision runs: %s", NAME[c]);
        printf("estimate %s: (%d, %d) est_seen %u lead %u (was %u)\n", NAME[c], s_mil.ebx, s_mil.ebz, s_mil.est_seen,
               o.lead, o.lead_was);
        CHECK(o.lead == LEAD[c], "%s: %s", NAME[c], LEAD[c] ? "the lead" : "no lead");
        if (c == 0 || c == 5)
            CHECK(s_mil.ebx == 3216 / 32 && s_mil.ebz == 1616 / 32 && s_mil.est_seen == EST_START,
                  "%s: the nearer other start", NAME[c]);
        if (c >= 1 && c <= 3)
            CHECK(s_mil.ebx == 200 && s_mil.ebz == 100 && s_mil.est_seen == (c == 1 ? EST_STRUCT : EST_KEPT),
                  "%s: the extractor's block, %s", NAME[c], c == 1 ? "seen" : "kept");
        if (c == 4)
            CHECK(s_mil.ebx == 150 && s_mil.ebz == 30 && s_mil.est_seen == EST_STRUCT, "%s: the new one", NAME[c]);
        s_w.gametime += 15;
    }
}

/* THE STARTS LOOKED AT (impai_mil.c, est_start) on check_mil_estimate's three starts, thirty tanks at home and
   nothing known: (a) the nearer other start, (3216, 1616); (b) a tank standing at it, its sweep cell stood in: the
   farther start, (7216, 3216); (c) a tank at both: the nearer again (the strait's case: check_mil_start_banks);
   (h) the nearer start left empty by the level (WQ_SEATED_NONE), nothing stood in: the farther start; (j) both
   other starts left empty: no estimate, the block left IMPAI_NO_ANCHOR; THE FOE'S STARTS, every start seated: (k)
   the nearer seated by an ally: the farther; (l) both seated by foes: the nearer.
   THE BASE'S FLOOR (lead): (d) an enemy extractor's ghost known and the thirty tanks: the lead; (e) the seat's own
   extractors worth more than the tanks' two thirds: the enemy's sum is at least its base, and no lead; (f) the
   same, every sweep cell of the map stood in now: the floor is gone, and the lead; (g) the same, the stamps
   IMPAI_DRY_T + 64 old: the floor is back, no lead; (i) the same, the nine cells round the ghost alone stood in
   now -- a raid on an outlying extractor --: the rest of the map not looked over holds the floor, no lead. */
static void check_mil_starts_looked(void)
{
    static const WQ_START STARTS[3] = { { 400, 400, 0, 0 }, { 3216, 1616, 1, 0 }, { 7216, 3216, 2, 0 } };
    IMPAI_MIL_OUT o;
    int c, n, k;
    float vt = 0.0f, vm, army_v = 0.0f, e_one = 0.0f, base_v = 0.0f;
    for (c = 0; c < 7; c++) {
        mil_world();
        memcpy(s_w.starts, STARTS, sizeof STARTS);
        s_w.nstarts = 3;
        if (c == 3 || c == 4) s_w.starts[1].seated = WQ_SEATED_NONE;
        if (c == 4) s_w.starts[2].seated = WQ_SEATED_NONE;
        if (c >= 5) {
            s_w.starts[0].seated = 1;
            s_w.starts[1].seated = c == 5 ? 3 : 2;
            s_w.starts[2].seated = 4;
            s_allies = c == 5 ? 1u << 2 : 0;
        }
        for (k = 0; k < 30; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
        if (c == 1 || c == 2) own(40, M_TANK, 3216 / 16, 1616 / 16);
        if (c == 2) own(41, M_TANK, 7216 / 16, 3216 / 16);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the starts' decision runs");
        s_w.gametime += 15;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the starts' second decision runs");
        printf("the starts looked at, case %d: the estimate (%d, %d) est_seen %u\n", c, s_mil.ebx * 32 + 16,
               s_mil.ebz * 32 + 16, s_mil.est_seen);
        CHECK(s_mil.est_seen == EST_START &&
              (c == 4   ? s_mil.ebx == IMPAI_NO_ANCHOR && s_mil.ebz == IMPAI_NO_ANCHOR
               : c == 1 || c == 3 || c == 5 ? s_mil.ebx == 7216 / 32 && s_mil.ebz == 3216 / 32
                                  : s_mil.ebx == 3216 / 32 && s_mil.ebz == 1616 / 32),
              "the starts looked at, case %d", c);
    }
    vt = s_ti[M_TANK].value;
    vm = s_ti[T_MEX].value;
    n = (int)(30.0f * vt / (1.5f * vm)) + 1;
    for (c = 0; c < 5; c++) {
        static const char* const FLOOR[5] = { "no base", "a base of extractors", "the map looked over",
                                              "looked over long ago", "the ground round the ghost alone looked over" };
        mil_world();
        memcpy(s_w.starts, STARTS, sizeof STARTS);
        s_w.nstarts = 3;
        s_w.gametime = 20000;
        for (k = 0; k < 30; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
        if (c) for (k = 0; k < n && k < 60; k++) own(60 + k, T_MEX, 4 + 2 * (k % 10), 60 + 2 * (k / 10));
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 0);
        if (c >= 2)
            for (k = 0; k < IMPAI_SWEEP; k++)
                s_mil.swept[k] = (uint16_t)((c == 2 ? (uint32_t)s_w.gametime : (uint32_t)s_w.gametime - IMPAI_DRY_T - 128u) >> 6);
        /* the ghost's block (200, 100), px (6 416, 3 216), is sweep cell (12, 12) of the 8 192 x 4 096 px map */
        if (c == 4)
            for (k = 0; k < 9; k++) s_mil.swept[(11 + k / 3) * IMPAI_SWEEP_SIDE + 11 + k % 3] = (uint16_t)(s_w.gametime >> 6);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the floor's decision runs");
        printf("the base's floor, %s: own %.0f enemy %.0f lead %u\n", FLOOR[c], (double)o.own_v, (double)o.enemy_v,
               o.lead);
        if (c == 0) {
            army_v = o.own_v;
            e_one = o.enemy_v;
        }
        if (c == 1) base_v = o.own_v - army_v;
        CHECK(c == 1 || c == 3 ? !o.lead && o.enemy_v >= (float)n * vm
              : c == 4 ? !o.lead && o.enemy_v >= (float)n * vm * (1.0f - 10.0f / IMPAI_SWEEP)
                       : o.lead == 1, "the base's floor, %s", FLOOR[c]);
    }
    /* (h) a foe at a time: the base of (e), one foe known by twice its extractors and another by one: the first
       counts what it has and the second the floor -- three bases, less the share of the map the thirty tanks stand
       in at home, and two commanders' floors, where one floor under the sum counted the second foe's one extractor
       in place of its base */
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 3;
    for (k = 0; k < 30; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
    for (k = 0; k < n && k < 60; k++) own(60 + k, T_MEX, 4 + 2 * (k % 10), 60 + 2 * (k / 10));
    for (k = 0; k < 2 * n && k < 120; k++) enemy(1300 + k, T_MEX, 380 + 2 * (k % 10), 180 + 2 * (k / 10), IMPAI_GHOST, 0);
    enemy(1450, T_MEX, 400, 60, IMPAI_GHOST, 0);
    s_rows[1450].seat[0].owner = 2;
    s_w.unit[1450].owner = 2;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the two foes' decision runs");
    {
        int looked = 0;
        float vmex, want;
        for (k = 0; k < IMPAI_SWEEP; k++)
            looked += s_mil.swept[k] && (uint32_t)s_mil.swept[k] * 64u + IMPAI_DRY_T >= (uint32_t)s_w.gametime;
        vmex = base_v / (float)(n < 60 ? n : 60);
        want = 2.0f * base_v + base_v * (float)(IMPAI_SWEEP - looked) / IMPAI_SWEEP + 2.0f * (e_one - vmex);
        printf("the base's floor, two foes: own %.0f enemy %.0f, three bases and two floors %.0f\n", (double)o.own_v,
               (double)o.enemy_v, (double)want);
        CHECK(o.enemy_v >= want - 1.0f, "the base's floor, a foe at a time");
    }
}

/* RAIDS AND SCOUTING: a tank, three raiders and a scout, too few for a wave; the remembered extractor is raided by
   the three raiders (the fastest type), and scouting sends the scout over the start positions, three legs, then
   back to FORM. Every start seated, start 2 by an ally (THE FOE'S STARTS): the legs pass it -- start 3 first, then
   start 1, then the sweep's cell, two starts reached. */
static void check_mil_raid_scout(void)
{
    static const WQ_START STARTS[] = { { 328, 328, 0, 0 }, { 6000, 328, 1, 0 }, { 328, 3000, 2, 0 }, { 6000, 3000, 3, 0 } };
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
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the raid decision runs");
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
        s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "a scout leg's decision runs");
    }
    CHECK(s >= 0 && s_mil.sq[s].task != IMPAI_SQ_SCOUT, "the scout is back after three legs");
    /* the scout's first leg past a start already looked at: scout_next 2, a tank standing at start 2 */
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 4;
    own(1, M_TANK, 24, 24);
    own(2, M_TANK, 328 / 16, 3000 / 16);
    own(7, M_SCOUT, 30, 30);
    s_mil.scout_next = 2;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the unseen start's decision runs");
    for (s = -1, k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    printf("scout, start 2 looked at: the first leg (%d, %d)\n", s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1,
           s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1);
    CHECK(s >= 0 && s_mil.sq[s].tx == 6000 / 32 && s_mil.sq[s].tz == 3000 / 32,
          "the scout passes the start already looked at: start 3 first");
    /* every start seated, start 2 by an ally */
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 4;
    for (k = 0; k < 4; k++) s_w.starts[k].seated = 1 + k;
    s_allies = 1u << 2;
    own(1, M_TANK, 24, 24);
    own(7, M_SCOUT, 30, 30);
    s_mil.scout_next = 2;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the allied start's decision runs");
    for (s = -1, k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    CHECK(s >= 0, "a scout goes with an ally's start");
    for (leg = 0; leg < 3 && s >= 0; leg++) {
        const int32_t x = s_mil.sq[s].tx * 32 + 16, z = s_mil.sq[s].tz * 32 + 16;
        int at = -1;
        for (k = 0; k < 4; k++)
            if (s_mil.sq[s].tx == STARTS[k].x / 32 && s_mil.sq[s].tz == STARTS[k].z / 32) at = k;
        printf("scout leg %d, start 2 an ally's: (%d, %d), start %d\n", leg, x, z, at);
        CHECK(at == (leg == 0 ? 3 : leg == 1 ? 1 : -1), "scout leg %d past the ally's start: start %d", leg, at);
        s_w.unit[7].pos[0] = x << 16;
        s_w.unit[7].pos[2] = z << 16;
        s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "a scout leg's decision runs");
    }
}

/* the commander given ARMCOM's retail costs (armcom.fbi: 29 854 metal, 34 125 energy), so its value, 30 423, is
   above any contact of the cases, as a retail commander's is above an army contact's */
static void armcom(void)
{
    s_w.defs[T_COM].e.cost_m = 29854.0f;
    s_w.defs[T_COM].e.cost_e = 34125.0f;
    s_ti[T_COM].value = s_ft[T_COM].value = 29854.0f + 34125.0f / (float)TASIM_ROLES_R;
}

/* the point of unit `unit`'s last move since `from`, px; 0 with none */
static int last_move(int from, int unit, int32_t* x, int32_t* z)
{
    int k, got = 0;
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.action == A_MOVE && s_w.log[k].c.unit == unit) {
            *x = s_w.log[k].c.pos[0] >> 16;
            *z = s_w.log[k].c.pos[2] >> 16;
            got = 1;
        }
    return got;
}

/* one commander case at Hard: the decision, its line, and whether the commander was moved, the point px */
static int com_case(const char* name, IMPAI_MIL_OUT* o, int32_t* x, int32_t* z)
{
    const int from = s_w.nlog;
    int moved;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, o) == 0, "the commander decision runs: %s", name);
    mil_print(name, o, from);
    moved = last_move(from, 1, x, z);
    printf("commander %s: safe=%u leash=%u move=%d", name, o->safe, o->leash, moved);
    if (moved) printf(" to (%d, %d), %u px from the anchor", *x, *z,
                      (unsigned)impai_isqrt((uint64_t)((int64_t)(*x - (s_st.ax * 16 + 8)) * (*x - (s_st.ax * 16 + 8)) +
                                                       (int64_t)(*z - (s_st.az * 16 + 8)) * (*z - (s_st.az * 16 + 8)))));
    putchar('\n');
    return moved;
}

static uint32_t from_anchor(int32_t x, int32_t z)
{
    const int64_t dx = x - (s_st.ax * 16 + 8), dz = z - (s_st.az * 16 + 8);
    return impai_isqrt((uint64_t)(dx * dx + dz * dz));
}

/* THE COMMANDER'S SAFETY (impai_mil.c, commander), the commander at ARMCOM's value, the anchor at cell (20, 20):
   - THE CONTACT: twelve tanks 300 px off with no army near: it flees, within comm_r, and is the military's for
     300 ticks, in which the economy gives it no order; a contact of unarmed constructors: it stays; twelve own
     tanks beside it against three raiders: it stays; two tanks against twelve enemy tanks: it flees;
   - THE STAND: three raiders 300 px off with no army near, which the commander beats by the margin: it stays;
     the same 1 120 px from the anchor, past comm_r: it flees;
   - THE FLIGHT: with own structures 800 px west (inside comm_r) and 3 000 px east (past the contact and comm_r),
     the commander at the anchor flees twelve tanks west;
   - THE BLAST: an enemy commander of blast_r 475 seen 552 px off, twelve own tanks beside ours, both commanders
     at 1 px a tick: keep = 475 + 64 + (30 + 30) = 599, so it flees; 584 px off, inside the keep by the own
     commander's approach alone, it flees; 904 px off it stays; seen 544 px east with
     an own structure 960 px north, the farthest, and the anchor and a structure west, safe: it flees west (THE
     FLIGHT OPENS THE GAP);
   - THE LEASH: 1 696 px from the anchor once the seat has had a factory, past 1 024 + 512: home to the anchor;
     before it, inside 2 048 + 512: no move.
   And every level's rally point lies within comm_r of the anchor. */
static void check_mil_commander(void)
{
    IMPAI_MIL_OUT o;
    IMPAI_BRAIN_OUT bo;
    int32_t x = 0, z = 0;
    int k, from, moved, far;

    mil_world();
    armcom();
    own(1, T_COM, 30, 20);
    for (k = 0; k < 12; k++) enemy(1300 + k, M_TANK, 48 + 2 * (k / 6), 16 + 2 * (k % 6), IMPAI_LOS, 1);
    moved = com_case("twelve tanks, no army", &o, &x, &z);
    CHECK(o.safe == 1 && moved && count_action(0, A_MOVE, 1) == 1 && from_anchor(x, z) <= IMPAI_PLAN_ROW.comm_r &&
              s_mil.safe_until == (uint32_t)s_w.gametime + 300,
          "the commander flees twelve tanks within comm_r and is the military's until %u", s_mil.safe_until);
    from = s_w.nlog;
    s_milp = &s_mil;
    s_w.gametime += 2;
    CHECK(decide(&bo) == 0, "the economy decides inside the window");
    for (k = from; k < s_w.nlog; k++) CHECK(s_w.log[k].c.unit != 1, "the economy orders the commander inside the window");
    CHECK(bo.com_free == 0 && bo.com_idle == 0, "the commander inside the window counted free: com_free %u com_idle %u",
          bo.com_free, bo.com_idle);
    s_milp = NULL;

    mil_world();
    armcom();
    own(1, T_COM, 30, 20);
    for (k = 0; k < 3; k++) enemy(1300 + k, T_CONS, 48, 18 + 2 * k, IMPAI_LOS, 1);
    moved = com_case("three unarmed constructors", &o, &x, &z);
    CHECK(!o.safe && !moved, "the commander flees unarmed constructors");

    for (far = 0; far <= 1; far++) {
        mil_world();
        armcom();
        own(1, T_COM, far ? 90 : 30, 20);
        for (k = 0; k < 3; k++) enemy(1300 + k, M_RAIDER, far ? 108 : 48, 18 + 2 * k, IMPAI_LOS, 1);
        moved = com_case(far ? "three raiders, no army, past comm_r" : "three raiders, no army", &o, &x, &z);
        if (far) CHECK(o.safe == 1 && moved, "the commander stands past comm_r");
        else CHECK(!o.safe && !moved && o.predictions == 1, "the commander flees three raiders it beats: %u predictions",
                   o.predictions);
    }

    mil_world();
    armcom();
    own(1, T_COM, 30, 20);
    for (k = 0; k < 12; k++) own(2 + k, M_TANK, 22 + 2 * (k % 6), 16 + 2 * (k / 6));
    for (k = 0; k < 3; k++) enemy(1300 + k, M_RAIDER, 48, 18 + 2 * k, IMPAI_LOS, 1);
    moved = com_case("twelve tanks against three raiders", &o, &x, &z);
    CHECK(!o.safe && !moved, "the commander flees a contact its army beats alone");

    mil_world();
    armcom();
    own(1, T_COM, 30, 20);
    own(2, M_TANK, 28, 18);
    own(3, M_TANK, 28, 22);
    for (k = 0; k < 12; k++) enemy(1300 + k, M_TANK, 44 + 2 * (k % 6), 16 + 2 * (k / 6), IMPAI_LOS, 1);
    moved = com_case("two tanks against twelve", &o, &x, &z);
    CHECK(o.safe == 1 && moved, "the commander stays where its army loses");

    mil_world();
    armcom();
    s_st.ax = s_st.az = 100;
    own(1, T_COM, 100, 100);
    own(20, T_SOLAR, 50, 100);
    own(21, T_SOLAR, 290, 100);
    for (k = 0; k < 12; k++) enemy(1300 + k, M_TANK, 118 + 2 * (k / 6), 96 + 2 * (k % 6), IMPAI_LOS, 1);
    moved = com_case("structures west and far east", &o, &x, &z);
    CHECK(o.safe == 1 && moved && x == 832 && z == 1632, "the flight goes to the west structure, not past comm_r: (%d, %d)",
          x, z);

    for (far = 0; far <= 1; far++) {
        mil_world();
        armcom();
        s_w.defs[T_COM].d.speed = 1 << 16;
        s_ti[T_COM].blast_r = 475;
        own(1, T_COM, 30, 20);
        for (k = 0; k < 12; k++) own(2 + k, M_TANK, 22 + 2 * (k % 6), 16 + 2 * (k / 6));
        enemy(1300, T_COM, far ? 86 : 64, 20, IMPAI_LOS, 1);
        s_job[1].kind = IMPAI_JOB_RECLAIM;
        s_job[1].type = T_COM;
        moved = com_case(far ? "an enemy commander 904 px off" : "an enemy commander 552 px off", &o, &x, &z);
        CHECK(o.rc_taken == !far, "the flight ends the commander's ordered reclaim, counted: rc_taken %u", o.rc_taken);
        if (far) CHECK(!o.safe && !moved, "the commander keeps clear of a blast 904 px off");
        else CHECK(o.safe == 1 && moved && x < 488, "the commander does not keep clear of a blast 552 px off");
    }
    mil_world();
    armcom();
    s_w.defs[T_COM].d.speed = 1 << 16;
    s_ti[T_COM].blast_r = 475;
    own(1, T_COM, 30, 20);
    for (k = 0; k < 12; k++) own(2 + k, M_TANK, 22 + 2 * (k % 6), 16 + 2 * (k / 6));
    enemy(1300, T_COM, 66, 20, IMPAI_LOS, 1);
    moved = com_case("an enemy commander 584 px off", &o, &x, &z);
    CHECK(o.safe == 1 && moved, "the commander does not keep clear of a blast 584 px off, inside its own approach");

    /* THE FLIGHT OPENS THE GAP: the enemy commander 544 px east, an own structure 960 px north (the farthest from it,
       a walk across its line), and the anchor 160 px and one more structure 448 px west, both safe and straight
       away: the flight goes west, to the anchor (the tie's) */
    mil_world();
    armcom();
    s_w.defs[T_COM].d.speed = 1 << 16;
    s_ti[T_COM].blast_r = 475;
    own(1, T_COM, 30, 20);
    own(20, T_SOLAR, 2, 18);
    own(21, T_SOLAR, 30, 78);
    enemy(1300, T_COM, 64, 20, IMPAI_LOS, 1);
    moved = com_case("an enemy commander east, structures west and north", &o, &x, &z);
    CHECK(o.safe == 1 && moved && x < 400 && z < 600, "the flight goes straight away, west: (%d, %d)", x, z);

    for (far = 0; far <= 1; far++) {
        mil_world();
        armcom();
        s_st.opened = (uint8_t)!far;
        own(1, T_COM, 126, 20);
        moved = com_case(far ? "1 696 px out before a factory" : "1 696 px out", &o, &x, &z);
        if (far) CHECK(!moved && !o.leash, "the leash before a factory");
        else CHECK(moved && o.leash == 1 && x == 328 && z == 328 && s_mil.safe_until == (uint32_t)s_w.gametime + 300,
                   "the leash brings the commander to the anchor and holds the window");
    }
}

/* THE COMMANDER'S HAND-BACK (impai_brain.c): the commander jobless at the anchor in an energy stall, a ring site
   within comm_r, its head one order of `action` at `target`, the military's window lapsed or, with `window` 1,
   open, with 2 restored far past the window's length; the orders the economy gave the commander. */
static int handback_case(const char* name, int action, int target, int window)
{
    IMPAI_BRAIN_OUT out;
    FUNIT* c;
    int n;
    reach_world(0);
    unit_add(&s_w, 40, T_COM, 30, 20, 1);
    s_w.unit[40].owner = 1;
    impai_mil_reset(&s_mil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
    s_mil.safe_until = window == 2 ? (uint32_t)s_w.gametime + 100000u
                     : window      ? (uint32_t)s_w.gametime + 100u
                                   : (uint32_t)s_w.gametime - 100u;
    c = &s_w.unit[1];
    memset(&c->list[0], 0, sizeof c->list[0]);
    c->list[0].action = (uint8_t)action;
    c->list[0].target = (uint16_t)target;
    c->list[0].pos[0] = c->list[0].pos[2] = 400 << 16;
    if (action == A_MOBILEBUILD) {
        c->list[0].build = 1;
        c->list[0].type = T_SOLAR;
    }
    c->n = 1;
    s_milp = &s_mil;
    CHECK(decide(&out) == 0, "decide");
    s_milp = NULL;
    n = count_unit(1, -1);
    printf("hand-back, %s: orders to the commander=%d\n", name, n);
    return n;
}

/* A move with no target is the economy's once the window has lapsed and the military's inside it; a D-gun's
   attack at an enemy stays the military's; a build at the head stays busy. */
static void check_commander_handback(void)
{
    CHECK(handback_case("a move, the window lapsed", A_MOVE, (int)WQ_NONE, 0) == 1,
          "the economy does not take back the commander from a lapsed window's move");
    CHECK(handback_case("a move inside the window", A_MOVE, (int)WQ_NONE, 1) == 0,
          "the economy orders the commander inside the military's window");
    CHECK(handback_case("a move, a window restored past its length", A_MOVE, (int)WQ_NONE, 2) == 1,
          "a restored window past SAFE_TICKS holds the commander from the economy");
    CHECK(handback_case("a D-gun at an enemy", A_DGUN, 40, 0) == 0, "the economy replaces the D-gun's attack");
    CHECK(handback_case("a build at the head", A_MOBILEBUILD, (int)WQ_NONE, 0) == 0,
          "the economy replaces the commander's build");
}

/* THE STAND'S D-GUN (impai_mil.c, stands): the commander at ARMCOM's value at cell (30, 20), alone against
   enemies about 300 px east, its fight content its 60-damage gun in slot 0 and in slot 1 a CommandFire shot of
   30 000 damage (the spare weapon record T_MAKER), its D-gun 140 px and 400 energy a shot as THE D-GUN's cases
   have it, Hard's reserve 0.25 of a 1 000 storage. `n` tanks seen now: with 1 000 energy the D-gun counts and
   it stands; with 640, short of 650, the stand fights without the shot and it flees; with the tanks radar
   contacts never identified -- a tank seen far off making the tank their substitute -- the D-gun has no target
   and it flees. Three raiders, which its gun beats alone, with 640 energy: it stands. */
static int stand_case(const char* name, int tanks, int raiders, float energy, int radar)
{
    IMPAI_MIL_OUT o;
    int32_t x = 0, z = 0;
    int k, moved;
    mil_world();
    armcom();
    s_fwp[T_MAKER].range = 200;
    s_fwp[T_MAKER].reload = 30;
    s_fwp[T_MAKER].burst = 1;
    s_fwp[T_MAKER].dmg = 30000;
    s_fwp[T_MAKER].flags = TASIM_WF_COMMANDFIRE;
    s_ft[T_COM].w[1] = T_MAKER;
    s_w.defs[T_COM].dgun = 1;
    s_ti[T_COM].dgun_range = 140;
    s_ti[T_COM].dgun_e = 400;
    s_w.econ[0].energy = energy;
    s_w.econ[0].energy_max = 1000;
    own(1, T_COM, 30, 20);
    for (k = 0; k < tanks; k++) {
        enemy(1300 + k, M_TANK, 48, 18 + 2 * k, radar ? IMPAI_RADAR : IMPAI_LOS, 1);
        if (radar) {
            s_rows[1300 + k].seat[0].type = (uint16_t)WQ_NONE;
            s_rows[1300 + k].seat[0].owner = 0xFF;
        }
    }
    if (radar) enemy(1320, M_TANK, 300, 200, IMPAI_LOS, 1);
    for (k = 0; k < raiders; k++) enemy(1310 + k, M_RAIDER, 48, 18 + 2 * k, IMPAI_LOS, 1);
    moved = com_case(name, &o, &x, &z);
    CHECK(o.predictions == 1, "the stand, %s: %u predictions, one wanted", name, o.predictions);
    return moved && o.safe == 1;
}

static void check_stand_dgun(void)
{
    CHECK(!stand_case("two tanks, energy for the D-gun", 2, 0, 1000.0f, 0), "the commander flees what its D-gun beats");
    CHECK(stand_case("two tanks, energy short of the reserve", 2, 0, 640.0f, 0),
          "the commander stands on a D-gun it may not fire");
    CHECK(stand_case("two tanks on the radar", 2, 0, 1000.0f, 1),
          "the commander stands on a D-gun with no target it may take");
    CHECK(!stand_case("three raiders, energy short of the reserve", 0, 3, 640.0f, 0),
          "the commander flees a contact its gun beats alone");
}

/* THE RING'S ORDER (impai_build.c): reach_world's energy stall with a finished wind generator on cells 19..21, so
   the base box is centred on the anchor's cell (20, 20) and the first ring's wind sites lie on all four sides, the
   nearest four 97 px from the anchor (96 one way, 16 the other); the builder, a commander or a constructor, at
   cell (bx, bz), the military's estimate at block (ebx, 10). The first wind site ordered, its centre px into *x,
   *z; 1 when one was ordered. */
static int ring_order_case(const char* name, int type, int bx, int bz, int ebx, int32_t* x, int32_t* z)
{
    IMPAI_BRAIN_OUT out;
    int k;
    reach_world(0);
    unit_add(&s_w, 1, type, bx, bz, 1);
    unit_add(&s_w, 2, T_WIND, 19, 19, 1);
    impai_mil_reset(&s_mil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
    s_mil.ebx = (int16_t)ebx;
    s_mil.ebz = 10;
    s_milp = &s_mil;
    CHECK(decide(&out) == 0, "decide");
    s_milp = NULL;
    for (k = 0; k < s_w.nlog; k++)
        if (s_w.log[k].c.action == A_MOBILEBUILD && s_w.log[k].c.type == T_WIND) {
            *x = s_w.log[k].c.pos[0] >> 16;
            *z = s_w.log[k].c.pos[2] >> 16;
            printf("ring order, %s: the first wind site at (%d, %d)\n", name, *x, *z);
            return 1;
        }
    printf("ring order, %s: no wind site\n", name);
    return 0;
}

/* A constructor takes the ring's site nearest the anchor, and of the four at that distance the one farthest from
   the estimate: west with the estimate east, east with it west. The commander's order starts where it stands: at
   the anchor, its first site is the constructor's; standing 96 px east, the east side's nearest. */
static void check_ring_order(void)
{
    const int32_t ax = 20 * 16 + 8, az = 20 * 16 + 8;
    const int gap = s_plan->gap, fx = s_w.defs[T_WIND].d.fx;
    PlaceCell ring[PLACE_MAX_SITES];
    unsigned n, k;
    int64_t least = INT64_MAX;
    int32_t x = 0, z = 0;
    n = place_ring(fx + gap, fx + gap, 19 - gap, 19 - gap, fx + 2 * gap, fx + 2 * gap, ring, PLACE_MAX_SITES);
    for (k = 0; k < n; k++) {
        const int64_t dx = (2 * (ring[k].cx + gap / 2) + fx) * 8 - ax, dz = (2 * (ring[k].cz + gap / 2) + fx) * 8 - az;
        if (dx * dx + dz * dz < least) least = dx * dx + dz * dz;
    }
    CHECK(ring_order_case("a constructor, the estimate east", T_CONS, 22, 22, 60, &x, &z) &&
              (int64_t)(x - ax) * (x - ax) + (int64_t)(z - az) * (z - az) == least && x < ax,
          "the constructor's first site, the nearest the anchor and west of it: (%d, %d)", x, z);
    CHECK(ring_order_case("a constructor, the estimate west", T_CONS, 22, 22, 0, &x, &z) &&
              (int64_t)(x - ax) * (x - ax) + (int64_t)(z - az) * (z - az) == least && x > ax,
          "the constructor's first site with the estimate west, east of the anchor: (%d, %d)", x, z);
    CHECK(ring_order_case("the commander at the anchor, the estimate east", T_COM, 20, 20, 60, &x, &z) &&
              (int64_t)(x - ax) * (x - ax) + (int64_t)(z - az) * (z - az) == least && x < ax,
          "the commander at the anchor takes the constructor's site: (%d, %d)", x, z);
    CHECK(ring_order_case("the commander 96 px east, the estimate east", T_COM, 26, 20, 60, &x, &z) && x > ax,
          "the commander standing east takes the east side's nearest site: (%d, %d)", x, z);
}

/* the squad table holds become's bound (impai_mil.c): at most one FORM, DEFEND and RETREAT squad a layer, at most
   IMPAI_ATTACKS_MAX ATTACK, IMPAI_RAIDS_MAX RAID and one SCOUT squad */
static int table_ok(void)
{
    int n[IMPAI_SQ_TASKS][TASIM_LAYERS], k, l, attacks = 0, raids = 0, scouts = 0;
    memset(n, 0, sizeof n);
    for (k = 0; k < IMPAI_SQUADS; k++) {
        const IMPAI_SQUAD* q = &s_mil.sq[k];
        if (q->task == IMPAI_SQ_FREE) continue;
        if (q->task >= IMPAI_SQ_TASKS || q->layer >= TASIM_LAYERS) return 0;
        n[q->task][q->layer]++;
    }
    for (l = 0; l < TASIM_LAYERS; l++) {
        if (n[IMPAI_SQ_FORM][l] > 1 || n[IMPAI_SQ_DEFEND][l] > 1 || n[IMPAI_SQ_RETREAT][l] > 1) return 0;
        attacks += n[IMPAI_SQ_ATTACK][l];
        raids += n[IMPAI_SQ_RAID][l];
        scouts += n[IMPAI_SQ_SCOUT][l];
    }
    return attacks <= IMPAI_ATTACKS_MAX && raids <= IMPAI_RAIDS_MAX && scouts <= 1;
}

/* EVERY BOUND: more army than IMPAI_ARMY_MAX, more enemy rows than IMPAI_ENEMY_MAX, more separated enemies than
   IMPAI_CONTACTS, a recorded squad table of sixteen ATTACK squads, more predictions asked than a plan's
   predict_max of 4 and more orders than its mil_roots of 6, and recorded bytes past their tables. */
static void check_mil_bounds(void)
{
    IMPAI_PLAN small = IMPAI_PLAN_ROW;
    IMPAI_MIL_OUT o;
    int k, from;
    small.predict_max = 4;
    small.mil_roots = 6;
    mil_world();
    for (k = 0; k < 600; k++) own(1 + k, M_TANK, 4 + 2 * (k % 40), 4 + 2 * (k / 40));
    for (k = 0; k < 2100; k++) enemy(k < 1279 ? 1279 - k : 1280 + (k - 1279), T_MEX, 480, 240, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the bounds decision runs");
    mil_print("army and enemy bounds", &o, s_w.nlog);
    CHECK(o.army == IMPAI_ARMY_MAX && o.enemies == IMPAI_ENEMY_MAX && o.left_army == 600 - IMPAI_ARMY_MAX &&
              o.left_enemy == 2100 - IMPAI_ENEMY_MAX && o.left_out == o.left_army + o.left_enemy,
          "army %u, enemies %u, left out %u (army %u, enemies %u)", o.army, o.enemies, o.left_out, o.left_army,
          o.left_enemy);

    mil_world();
    own(1, M_TANK, 4, 4);
    s_w.mh = 320;
    for (k = 0; k < 40; k++) enemy(1300 + k, M_TANK, 16 + 64 * (k % 8), 16 + 64 * (k / 8), IMPAI_LOS, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the contacts decision runs");
    mil_print("contact bound", &o, s_w.nlog);
    CHECK(o.contacts == IMPAI_CONTACTS && o.left_contact == 40 - IMPAI_CONTACTS && o.left_out == o.left_contact,
          "contacts %u, left out %u", o.contacts, o.left_out);

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
    CHECK(mil_decide(&small, 1, &o) == 0, "the squads decision runs");
    mil_print("squad, prediction and order bounds (a small plan)", &o, from);
    CHECK(table_ok() && o.left_squad == 0 && o.left_out == 0 && s_job[41].kind == IMPAI_JOB_SQUAD &&
              s_job[41].squad < IMPAI_SQUADS,
          "the table within its bounds, %u units left out; the index past the table voided and the unit in a squad",
          o.left_out);
    CHECK(o.predictions == small.predict_max, "predictions %u of the plan's %u", o.predictions, small.predict_max);
    CHECK(o.roots == small.mil_roots, "orders %u of the plan's %u", o.roots, small.mil_roots);

}

/* the military's tick and the economy's never coincide, for any seat at any level */
static void check_mil_due(void)
{
    unsigned lv, seat, t, both = 0, mil = 0;
    for (lv = 0; lv < IMPAI_LVS; lv++)
        for (seat = 0; seat < WQ_SEATS; seat++)
            for (t = 0; t < 1800; t++) {
                const int m = impai_mil_due(t, seat, IMPAI_PERSONAS[lv].look_mil);
                mil += (unsigned)m;
                both += m && impai_brain_due(t, seat, IMPAI_PERSONAS[lv].look_eco);
            }
    CHECK(!both, "%u ticks share a military and an economy decision", both);
    printf("mil due: %u military decisions over 1800 ticks, ten seats, three levels\n", mil);
}

/* THE ARMY'S PRODUCTION (impai_econ.c, factories_army): a finished factory offering the raider, the tank and the
   scout, fed metal, decides twenty times with the military's default table over an empty picture: the first unit
   is the scout (THE SCOUT FIRST), and each idle decision after queues one unit of the class furthest below its
   share, the scout aside (THE SCOUT IS ONE), so the value bought follows the shares. A twenty-first decision with
   every scout dead queues a scout; with the table's scout share 0, none. The shares of A3's hand table over the land
   classes are printed beside the spec's hand composition. */
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
    int n[M_N], k, from, i, first = -1, again = 0;
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
    for (i = 0; i <= 20; i++) {
        /* the twenty-first decision: every scout dead */
        if (i == 20)
            for (k = 100; k < 120; k++)
                if (s_w.unit[k].type == M_SCOUT) memset(&s_w.unit[k], 0, sizeof s_w.unit[k]);
        memset(&in, 0, sizeof in);
        in.q = &q;
        in.st = &s_st;
        in.pref = s_pref;
        in.pref_stride = 1;
        in.job = s_job; in.njob = FW_SLOTS;
        in.ti = s_ti; in.nti = M_N;
        in.spots = s_spots; in.nspots = s_nspots;
        in.scanned = s_scanned;
        in.reach = &s_reach;
        in.plan = &IMPAI_PLAN_ROW;
        in.persona = s_persona;
        in.acts = -1;
        in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
        in.snap = &s_snap;
        in.mil = &s_mil;
        in.pred = &pred;
        from = s_w.nlog;
        CHECK(decide_traced(&in, &bo) == 0, "a production decision runs");
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.unit == 2 && s_w.log[k].c.verb == WQ_CMD_QUEUE && s_w.log[k].c.type < M_N) {
                if (first < 0) first = s_w.log[k].c.type;
                if (i == 20) again = s_w.log[k].c.type;
                n[s_w.log[k].c.type]++;
                /* the factory finishes it at once: a unit of the type in a free slot, its queue empty */
                own(100 + i, s_w.log[k].c.type, 40 + i, 40);
                s_w.unit[2].n = 0;
            }
        s_w.gametime += 15;
    }
    s_milp = NULL;
    printf("production, 20 decisions: raider %d tank %d scout %d; the first %d, with every scout dead %d\n",
           n[M_RAIDER], n[M_TANK], n[M_SCOUT], first, again);
    CHECK(first == M_SCOUT, "the scout first: the first unit is the scout, %d", first);
    CHECK(again == M_SCOUT, "the scout first: with every scout dead the next unit is a scout, %d", again);
    CHECK(n[M_SCOUT] == 2, "the scout is one: %d scouts over the 21 decisions, the first and the dead one's", n[M_SCOUT]);
    for (k = 100; k < 121; k++)
        if (s_w.unit[k].type == M_SCOUT) memset(&s_w.unit[k], 0, sizeof s_w.unit[k]);
    s_w.unit[2].n = 0;
    s_mil.table.base[IMPAI_K_LSCOUT] = 0.0f;
    s_milp = &s_mil;
    in.ti = s_ti;
    from = s_w.nlog;
    CHECK(decide_traced(&in, &bo) == 0, "the no-scout-share decision runs");
    s_milp = NULL;
    for (again = -1, k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.unit == 2 && s_w.log[k].c.verb == WQ_CMD_QUEUE) again = s_w.log[k].c.type;
    s_mil.table = IMPAI_TABLE_HAND;
    CHECK(again >= 0 && again != M_SCOUT, "the scout first: no scout with the table's scout share 0, %d", again);
    CHECK(n[M_RAIDER] + n[M_TANK] + n[M_SCOUT] >= 15, "the factory builds army");
    CHECK(n[M_TANK] * s_ti[M_TANK].value > n[M_RAIDER] * s_ti[M_RAIDER].value &&
              n[M_RAIDER] * s_ti[M_RAIDER].value > n[M_SCOUT] * s_ti[M_SCOUT].value,
          "the value bought follows the shares: assault, raider, scout");
}

/* SCOUTING GOES DEEP (impai_mil.c, the owner's decision of 2026-11-17): check_mil_raid_scout's scout on its first
   leg; an armed enemy tank comes into sight beside it, within its reach: the scout stays a SCOUT and its leg goes
   on. */
static void check_mil_scout_deep(void)
{
    static const WQ_START STARTS[] = { { 328, 328, 0, 0 }, { 6000, 328, 1, 0 }, { 328, 3000, 2, 0 }, { 6000, 3000, 3, 0 } };
    IMPAI_MIL_OUT o;
    int k, s = -1, tx, tz;
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 4;
    own(1, M_TANK, 24, 24);
    own(7, M_SCOUT, 30, 30);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the deep scout's decision runs");
    for (k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    CHECK(s >= 0, "a scout goes");
    if (s < 0) return;
    tx = s_mil.sq[s].tx;
    tz = s_mil.sq[s].tz;
    s_w.unit[7].pos[0] = (int32_t)(tx * 32 + 16 - 1200) << 16;
    s_w.unit[7].pos[2] = (int32_t)(tz * 32 + 16) << 16;
    enemy(1300, M_TANK, (tx * 32 + 16 - 1100) / 16, (tz * 32 + 16) / 16, IMPAI_LOS, 1);
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the deep scout's contact decision runs");
    printf("the deep scout: task %d, leg (%d, %d) -> (%d, %d)\n", s_mil.sq[s].task, tx, tz, s_mil.sq[s].tx,
           s_mil.sq[s].tz);
    CHECK(s_mil.sq[s].task == IMPAI_SQ_SCOUT && s_mil.sq[s].tx == tx && s_mil.sq[s].tz == tz,
          "a contact does not end the scout's trip");
}

/* THE GOVERNOR (impai_econ.c) on check_production's harness: the military's world, a commander, one or two
   factories whose menu holds the constructor and the raider at cells (30, 30) and (40, 30), the economy reading
   the military, the pair rate 0, 30 metal a pass; a case moves one field of its copy of Hard's row. Constructors
   are units 10.., army units 100.. */
static IMPAI_PLAN s_glv;
static void gov_world(int factories)
{
    static const uint16_t MENU[] = { T_CONS, M_RAIDER };
    mil_world();
    menu_set(&s_w, T_FACTORY, MENU, 2);
    own(1, T_COM, 20, 20);
    unit_add(&s_w, 2, T_FACTORY, 30, 30, 1);
    if (factories > 1) unit_add(&s_w, 3, T_FACTORY, 40, 30, 1);
    s_w.econ[0].metal_in = 30.0f;
    s_glv = IMPAI_PLAN_ROW;
    s_milp = &s_mil;
}

static void gov_units(int type, int n, int at)
{
    int k;
    for (k = 0; k < n; k++) own(at + k, type, 50 + 2 * (k % 20), 60 + 2 * (k / 20));
}

/* one decision; the constructors and army units queued at the factories into *cons and *army */
static int s_gov_acts = -1;        /* gov_decide's action budget, -1 none */
static int gov_decide(IMPAI_BRAIN_OUT* bo, int* cons, int* army)
{
    static const IMPAI_PREDICTOR pred = { rate_zero, impai_expf, NULL };
    IMPAI_BRAIN_IN in;
    WORLDQ q = { &FAKE, &s_w, 0 };
    const int from = s_w.nlog;
    int k, rc;
    memset(&in, 0, sizeof in);
    in.q = &q;
    in.st = &s_st;
    in.pref = s_pref;
    in.pref_stride = 1;
    in.job = s_job; in.njob = FW_SLOTS;
    in.ti = s_ti; in.nti = M_N;
    in.spots = s_spots; in.nspots = s_nspots;
    in.scanned = s_scanned;
    in.reach = &s_reach;
    in.plan = &s_glv;
    in.persona = s_persona;
    in.acts = s_gov_acts;
    in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
    in.snap = &s_snap;
    in.mil = &s_mil;
    in.pred = &pred;
    in.view = s_viewp;
    rc = decide_traced(&in, bo);
    *cons = *army = 0;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->verb != WQ_CMD_QUEUE) continue;
        if (c->type == T_CONS) (*cons)++;
        else if (c->type < M_N && s_ti[c->type].klass < IMPAI_KS) (*army)++;
    }
    return rc;
}

/* the count of the last QUEUE logged from log entry `from` of type `type` (any type but the constructor for -1),
   0 for none */
static int queue_count(int from, int type)
{
    int k, n = 0;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->verb == WQ_CMD_QUEUE && (type >= 0 ? c->type == type : c->type != T_CONS)) n = c->count;
    }
    return n;
}

/* the factories' last decision finished at once: a unit of each type queued in a free slot from `at`, every
   factory's list empty */
static int gov_finish(int from, int at)
{
    int k;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->verb != WQ_CMD_QUEUE) continue;
        while (at < FW_SLOTS / 2 && s_w.unit[at].live) at++;
        own(at, c->type, 50 + (at % 40), 70 + (at / 40) % 40);
        s_w.unit[c->unit].n = 0;
    }
    return at;
}

/* a busy factory at slot 3 holding a constructor count node of `count`, with the constructor's frame on its yard */
static void gov_busy(int32_t count)
{
    FUNIT* f = &s_w.unit[3];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = count;
    f->n = 1;
    unit_add(&s_w, 9, T_CONS, 41, 31, 0);
}

/* THE OPENING AND THE ARMY'S SHARE, at 30 metal a pass: none of either, a constructor; one constructor finished
   and no army, army (the old quota's target, 14, queued a constructor here); with one army unit, army; with two,
   a constructor. A constructor frame on the busy second factory's yard, its count node 1, and two army units: a
   constructor at the first, the frame counted once (twice it would want four army units); the node's count 70 000,
   engine data, is held to 255, past cons_max: army. */
static void check_gov_opening(void)
{
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    static const struct { int cons, army; } CASE[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 1, 2 } };
    for (k = 0; k < 4; k++) {
        gov_world(1);
        gov_units(T_CONS, CASE[k].cons, 10);
        gov_units(M_RAIDER, CASE[k].army, 100);
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        printf("governor, %d constructors and %d army: queued %d constructors, %d army\n", CASE[k].cons,
               CASE[k].army, c, a);
        CHECK(k == 1 || k == 2 ? c == 0 && a == 1 : c == 1 && a == 0, "governor case %d: %d constructors, %d army", k, c,
              a);
    }
    for (k = 0; k < 2; k++) {
        gov_world(2);
        gov_busy(k ? 70000 : 1);
        gov_units(M_RAIDER, 2, 100);
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        printf("governor, a frame and its node of %d, two army: queued %d constructors, %d army\n", k ? 70000 : 1, c, a);
        CHECK(k ? c == 0 && a == 1 : c == 1 && a == 0, "governor, the frame counted once, node %d: %d, %d", k, c, a);
    }
    s_milp = NULL;
}

/* THE FACTORIES' ROOTS (impai_brain.c): gov_world's two idle factories, and ten constructors at cell (20, 20)
   whose BUILD walks to an extractor's site at (52, 20) cross a turret seen over the 256-px cell (2, 1): the walk
   check turns back six of them, Hard's eight roots less the two the idle factories keep, and each factory is
   given its order. With 100 stored of 1000, a hold, the factories keep only the floor's one root: seven turned
   back and one factory order. */
static void check_gov_turn_backs(void)
{
    IMPAI_BRAIN_OUT bo;
    IMPAI_VIEW v;
    int k, c, a, h, moves;
    for (h = 0; h < 2; h++) {
        moves = 0;
        gov_world(2);
        if (h) s_w.econ[0].metal = 100.0f;
        s_threat[T_TURRET].hit_ground = 10;
        s_threat[T_TURRET].hit_range = 100;
        enemy(1300, T_TURRET, 38, 22, IMPAI_LOS, 0);
        for (k = 0; k < 10; k++) {
            IMPAI_JOB* j = &s_job[10 + k];
            FUNIT* u;
            own(10 + k, T_CONS, 20, 20);
            u = &s_w.unit[10 + k];
            j->kind = IMPAI_JOB_BUILD;
            j->type = T_CONS;
            j->build = T_MEX;
            j->target = (uint16_t)WQ_NONE;
            j->cx = 52;
            j->cz = 20;
            j->since = (uint32_t)s_w.gametime - 10u;
            memset(&u->list[0], 0, sizeof u->list[0]);
            u->list[0].action = A_MOBILEBUILD;
            u->list[0].build = 1;
            u->list[0].type = T_MEX;
            u->list[0].target = (uint16_t)WQ_NONE;
            u->list[0].pos[0] = (2 * 52 + 3) << 19;
            u->list[0].pos[2] = (2 * 20 + 3) << 19;
            u->n = 1;
        }
        memset(&v, 0, sizeof v);
        v.seat = 0;
        v.rows = s_rows;
        v.nrows = FW_SLOTS;
        v.threat = s_threat;
        v.ntypes = M_N;
        v.w = s_w.mw;
        v.h = s_w.mh;
        s_viewp = &v;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        s_viewp = NULL;
        s_milp = NULL;
        for (k = 0; k < s_w.nlog; k++) moves += s_w.log[k].c.action == A_MOVE && s_w.log[k].c.unit >= 10;
        printf("the factories' roots, ten walks across a turret%s: turned back %d, factory orders %d, roots %u\n",
               h ? ", in a hold" : "", moves, c + a, bo.roots);
        CHECK(h ? moves == 7 && c + a == 1 && bo.hold : moves == 6 && c + a == 2,
              "the turn backs left the factories %d orders, %d turned back, hold %u", c + a, moves, bo.hold);
    }
}

/* THE TURNED-BACK HELPER (impai_econ.c, impai_econ_assign): gov_world's factory and a second one busy at (36, 18),
   on the 256-px cell a turret covers, and three constructors at (20, 20) guarding it (an assist, GUARD heads), the
   seat short of metal at 1 a pass with a reclaimable feature in sight at (24, 24), outside the threat. The walk
   check turns each one back; each has exactly one order that decision, the move, and no reclaim. */
static void check_hold_turnback(void)
{
    IMPAI_BRAIN_OUT bo;
    IMPAI_VIEW v;
    int k, c, a, moves = 0, reclaims = 0, n[3] = { 0, 0, 0 };
    gov_world(1);
    unit_add(&s_w, 3, T_FACTORY, 36, 18, 1);
    gov_busy(1);
    s_w.econ[0].metal_in = 1.0f;
    s_w.defs[T_CONS].d.flags |= WQ_DF_RECLAIMER;
    feat_add(24, 24, 1, 1, 40.0f);
    s_threat[T_TURRET].hit_ground = 10;
    s_threat[T_TURRET].hit_range = 100;
    enemy(1300, T_TURRET, 38, 22, IMPAI_LOS, 0);
    for (k = 0; k < 3; k++) {
        IMPAI_JOB* j = &s_job[10 + k];
        FUNIT* u;
        own(10 + k, T_CONS, 20, 20);
        u = &s_w.unit[10 + k];
        j->kind = IMPAI_JOB_ASSIST;
        j->type = T_CONS;
        j->build = (uint16_t)WQ_NONE;
        j->target = 3;
        j->since = (uint32_t)s_w.gametime - 10u;
        memset(&u->list[0], 0, sizeof u->list[0]);
        u->list[0].action = A_GUARD;
        u->list[0].target = 3;
        u->n = 1;
    }
    memset(&v, 0, sizeof v);
    v.seat = 0;
    v.rows = s_rows;
    v.nrows = FW_SLOTS;
    v.threat = s_threat;
    v.ntypes = M_N;
    v.w = s_w.mw;
    v.h = s_w.mh;
    s_viewp = &v;
    CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
    s_viewp = NULL;
    s_milp = NULL;
    s_w.defs[T_CONS].d.flags &= ~WQ_DF_RECLAIMER;
    for (k = 0; k < s_w.nlog; k++) {
        const WQ_CMD* cmd = &s_w.log[k].c;
        if (cmd->unit < 10 || cmd->unit > 12) continue;
        n[cmd->unit - 10]++;
        moves += cmd->action == A_MOVE;
        reclaims += cmd->action == A_RECLAIM;
    }
    printf("the turned-back helpers: moves %d, reclaims %d, orders %d %d %d\n", moves, reclaims, n[0], n[1], n[2]);
    CHECK(moves == 3 && !reclaims && n[0] == 1 && n[1] == 1 && n[2] == 1,
          "a turned-back helper is given a second order in its decision");
}

/* THE PRESSURE: no constructor, so the opening's would be free; a stamp 100 ticks ahead holds it, army; one that
   has run out, one past now + press_hold (a recorded value out of its bound) and none each give the constructor */
static void check_gov_pressure(void)
{
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 4; k++) {
        gov_world(1);
        s_mil.press_until = k == 0 ? (uint32_t)s_w.gametime + 100u
                          : k == 1 ? (uint32_t)s_w.gametime
                          : k == 2 ? (uint32_t)s_w.gametime + s_glv.press_hold + 1u : 0u;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        printf("governor, pressure case %d: queued %d constructors, %d army, pressed %u\n", k, c, a, bo.pressed);
        CHECK(k == 0 ? c == 0 && a == 1 && bo.pressed : c == 1 && a == 0 && !bo.pressed, "the pressure case %d: %d, %d",
              k, c, a);
    }
    s_milp = NULL;
}

/* THE POSTURE (impai_brain.c, posture_read): each flag follows its own field of the military's record and no
   other. A flag's bound is the record's: a press_until that has run out or lies past now + press_hold presses
   nothing, at the base or not; a blind stamp past now is no blind spot; dry and island are flags whatever non-zero
   byte the record holds; naval is the record's value. */
static void check_posture(void)
{
    /* blind: 0 no stamp, 1 a stamp at now, 2 one past now; press: 0 no stamp, 1 one 100 ticks ahead, 2 one past
       now + press_hold */
    static const struct { int8_t naval; uint8_t dry, island, base; int blind, press; IMPAI_POSTURE want; } C[] = {
        { -1, 0, 0, 0, 0, 0, { -1, 0, 0, 0, 0, 0 } },
        {  1, 0, 0, 0, 0, 0, {  1, 0, 0, 0, 0, 0 } },
        {  0, 2, 0, 0, 0, 0, {  0, 1, 0, 0, 0, 0 } },
        {  0, 0, 2, 0, 0, 0, {  0, 0, 1, 0, 0, 0 } },
        {  0, 0, 0, 0, 1, 0, {  0, 0, 0, 1, 0, 0 } },
        {  0, 0, 0, 0, 2, 0, {  0, 0, 0, 0, 0, 0 } },
        {  0, 0, 0, 0, 0, 1, {  0, 0, 0, 0, 1, 0 } },
        {  0, 0, 0, 1, 0, 1, {  0, 0, 0, 0, 1, 1 } },
        {  0, 0, 0, 1, 0, 0, {  0, 0, 0, 0, 0, 0 } },
        {  0, 0, 0, 1, 0, 2, {  0, 0, 0, 0, 0, 0 } },
    };
    const IMPAI_DEC* dd = (const IMPAI_DEC*)s_scratch;
    IMPAI_BRAIN_OUT bo;
    unsigned k;
    int c, a;
    for (k = 0; k < sizeof C / sizeof C[0]; k++) {
        const IMPAI_POSTURE* w = &C[k].want;
        uint32_t now;
        gov_world(1);
        now = (uint32_t)s_w.gametime;
        s_mil.naval = C[k].naval;
        s_mil.dry = C[k].dry;
        s_mil.island = C[k].island;
        s_mil.press_base = C[k].base;
        s_mil.blind_t = C[k].blind == 1 ? now : C[k].blind == 2 ? now + 1u : 0u;
        s_mil.press_until = C[k].press == 1 ? now + 100u : C[k].press == 2 ? now + s_glv.press_hold + 1u : 0u;
        CHECK(now > 0 && gov_decide(&bo, &c, &a) == 0, "decide");
        CHECK(dd->pos.naval == w->naval && dd->pos.dry == w->dry && dd->pos.island == w->island &&
              dd->pos.blind == w->blind && dd->pos.pressed == w->pressed && dd->pos.at_base == w->at_base &&
              bo.pressed == w->pressed,
              "the posture, case %u: naval %d, dry %u, island %u, blind %u, pressed %u, at the base %u", k,
              dd->pos.naval, dd->pos.dry, dd->pos.island, dd->pos.blind, dd->pos.pressed, dd->pos.at_base);
    }
    s_milp = NULL;
}

/* A REFUSED CONSTRUCTOR ORDER leaves the factory to the army in the same decision */
static void check_gov_refused(void)
{
    IMPAI_BRAIN_OUT bo;
    int c, a;
    int from;
    gov_world(1);
    s_w.refuse_queue_type = T_CONS;
    from = s_w.nlog;
    CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
    printf("governor, the constructor refused: queued %d constructors, %d army\n", c, a);
    CHECK(c == 0 && a == 1, "the factory whose constructor was refused queues army: %d, %d", c, a);
    CHECK(queue_count(from, -1) == 1, "outside a surplus an army order is one unit");
    s_milp = NULL;
}

/* A SURPLUS: a factory whose menu holds only a gunship, which the table buys none of: with the store at 0.5 no
   order and fidle 0; at 0.9 the gunship, its cheapest army type -- one with no action budget, WQ_QUEUE_MAX of it in
   one order with one. A factory whose menu holds only the constructor, cons_max of them standing, at 0.9: no order
   and fidle 1. */
static void check_gov_surplus(void)
{
    static const uint16_t AIR[] = { M_SCOUT }, CONS[] = { T_CONS };
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 3; k++) {
        int from;
        gov_world(1);
        menu_set(&s_w, T_FACTORY, AIR, 1);
        s_ti[M_SCOUT].klass = IMPAI_K_GUNSHIP;
        if (k) s_w.econ[0].metal = 0.9f * s_w.econ[0].metal_max;
        s_gov_acts = k == 2 ? 3 : -1;
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        s_gov_acts = -1;
        printf("governor, a gunship alone, store %.1f%s: queued %d army (count %d), fidle %u, army %u\n",
               k ? 0.9 : 0.5, k == 2 ? ", a budget of 3" : "", a, queue_count(from, M_SCOUT), bo.fidle, bo.army);
        CHECK(k ? a == 1 && bo.fidle == 0 : a == 0 && bo.fidle == 0, "the surplus buys the cheapest army type, case %d",
              k);
        if (k == 1) CHECK(queue_count(from, M_SCOUT) == 1 && bo.army == 1, "no budget: one unit an order");
        if (k == 2) CHECK(queue_count(from, M_SCOUT) == WQ_QUEUE_MAX && bo.army == WQ_QUEUE_MAX,
                          "a budgeted surplus order is a shift-click's units");
    }
    gov_world(1);
    menu_set(&s_w, T_FACTORY, CONS, 1);
    gov_units(T_CONS, s_glv.cons_max, 10);
    s_w.econ[0].metal = 0.9f * s_w.econ[0].metal_max;
    CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
    printf("governor, the constructor alone at cons_max in surplus: queued %d, fidle %u\n", c + a, bo.fidle);
    CHECK(c + a == 0 && bo.fidle == 1, "a factory with nothing to buy beside a full store is counted");
    s_milp = NULL;
}

/* THE SCOUT FIRST in a surplus (impai_econ.c, scout_due, queue_army) on a budget of 3, the store at 0.9:
   (a) the factory's menu a land scout and the tank, no scout standing: the order is the scout, one unit, never the
       surplus's five;
   (b) its menu a land scout and the anti-air K-bot, the table's scout share 0 and nothing in the air, so the table
       buys nothing: the surplus's cheapest buy takes the K-bot, though the scout is the cheaper. */
static void check_gov_scout_surplus(void)
{
    static const uint16_t MENU[2][2] = { { M_SCOUT, M_TANK }, { M_SCOUT, M_AA } };
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 2; k++) {
        int from;
        gov_world(1);
        menu_set(&s_w, T_FACTORY, MENU[k], 2);
        if (k) s_mil.table.base[IMPAI_K_LSCOUT] = 0.0f;
        s_w.econ[0].metal = 0.9f * s_w.econ[0].metal_max;
        s_gov_acts = 3;
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        s_gov_acts = -1;
        s_mil.table = IMPAI_TABLE_HAND;
        printf("the scout first in a surplus, case %d: scouts queued %d, tanks %d, anti-air %d\n", k,
               queue_count(from, M_SCOUT), queue_count(from, M_TANK), queue_count(from, M_AA));
        if (!k) CHECK(queue_count(from, M_SCOUT) == 1 && !queue_count(from, M_TANK), "(a) one scout");
        else CHECK(!queue_count(from, M_SCOUT) && queue_count(from, M_AA) >= 1, "(b) the cheapest buy takes no scout");
    }
    s_milp = NULL;
}

/* ONE AIR SCOUT A SEAT (impai_econ.c, scout_due, army_menu): a factory whose menu holds an air scout -- M_SCOUT of
   the air scout's class, hitting nothing, the table's share of it 0 -- and the tank:
   (a) THE BLIND SPOT stamped this tick: the order is the air scout, one unit, though it hits no target class;
   (b) never stamped: the tank;
   (c) stamped IMPAI_BLIND_HOLD + 1 ticks ago: the tank;
   (d) stamped this tick, an air scout standing: the tank. */
static void check_gov_air_scout(void)
{
    static const uint16_t MENU[] = { M_SCOUT, M_TANK };
    static const char* const NAME[4] = { "(a) blind now", "(b) never blind", "(c) blind long ago",
                                         "(d) blind now, a scout standing" };
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 4; k++) {
        int from;
        gov_world(1);
        menu_set(&s_w, T_FACTORY, MENU, 2);
        s_ti[M_SCOUT].klass = IMPAI_K_ASCOUT;
        s_ti[M_SCOUT].flags &= (uint8_t)~(IMPAI_TF_HITDRY | IMPAI_TF_HITWET | IMPAI_TF_HITAIR);
        s_mil.table.base[IMPAI_K_ASCOUT] = 0.0f;
        if (k == 2) s_w.gametime += 1800;      /* a multiple of the decisions' periods, past the hold */
        s_mil.blind_t = k == 1 ? 0u : k == 2 ? (uint32_t)s_w.gametime - IMPAI_BLIND_HOLD - 1u : (uint32_t)s_w.gametime;
        if (k == 3) unit_add(&s_w, 6, M_SCOUT, 34, 32, 1);
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        s_mil.table = IMPAI_TABLE_HAND;
        printf("one air scout a seat, %s: scouts queued %d, tanks %d\n", NAME[k], queue_count(from, M_SCOUT),
               queue_count(from, M_TANK));
        if (!k) CHECK(queue_count(from, M_SCOUT) == 1 && !queue_count(from, M_TANK), "%s: one air scout", NAME[k]);
        else CHECK(!queue_count(from, M_SCOUT) && queue_count(from, M_TANK) >= 1, "%s: the tank", NAME[k]);
    }
    s_ti[M_SCOUT].klass = IMPAI_K_LSCOUT;
    s_mil.blind_t = 0;
    s_milp = NULL;
}

/* THE FIVE, bounded, and THE ARMY'S ROWS, on a budget of 8 with the store at 900 of 1 000 and the hold's line at
   200, so 700 metal above it, each factory's menu the gunship alone:
   (a) one factory, the gunship at 300 metal: two units in the order (700 / 300);
   (b) two factories, the gunship at 150: the first order shares the 700 between the two factories, 2 units; the
       second takes what the first left past its first unit, (700 - 150) / 150, 3 units;
   (c) one factory, the gunship at 40, 510 army units standing: 2 units, what the rows leave;
   (d) the same with 512 standing: no order, and the factory counted idle;
   (e) the same with no budget: no order either;
   (f) two factories, the gunship at 40, 508 standing: one order of the 4 units the rows leave, which the governor
       counts whole, so the second factory takes none and is counted idle. */
static int queue_counts(int from, int* second)
{
    int k, n = 0, first = 0;
    *second = 0;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->verb != WQ_CMD_QUEUE || c->type != M_SCOUT) continue;
        if (n++ == 0) first = c->count;
        else *second = c->count;
    }
    return first;
}

/* THE FINISH AT SEA's army (impai_econ.c, army_menu): the governor's world, the submarine's weapon a water weapon,
   no constructor the plan allows (cons_open 0, army_per_cons 255), so the factory's order is army. Out of a dry
   hunt, the menu the constructor and the raider: the raider; in one, the factory a water plant -- the finish's
   plant -- with the submarine on the menu too: the submarine; in one, the water plant with the raider's menu: no
   army order at all; in one with no water plant and none a builder offers (THE FINISH'S BUY): the raider, bought
   as usual. */
static void mil_sea_types(void);

static void check_gov_dry(void)
{
    static const uint16_t MENU[] = { T_CONS, M_RAIDER, M_SUB };
    static const char* const NAME[4] = { "not dry, the raider's menu", "dry, the submarine on the menu",
                                         "dry, the raider's menu", "dry, no sea plant to be had" };
    IMPAI_BRAIN_OUT bo;
    int c, cons, army, k, raider, sub;
    for (c = 0; c < 4; c++) {
        int from;
        gov_world(1);
        mil_sea_types();
        s_ti[M_SUB].attack |= IMPAI_TA_WATER1;
        if (c == 1) menu_set(&s_w, T_FACTORY, MENU, 3);
        gov_units(M_RAIDER, 20, 100);
        if (c == 1 || c == 2) s_ti[T_FACTORY].flags |= IMPAI_TF_WATER;
        s_glv.cons_open = 0;
        s_glv.army_per_cons = 255;
        s_mil.dry = (uint8_t)(c > 0);
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &cons, &army) == 0, "decide: %s", NAME[c]);
        raider = sub = 0;
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.verb == WQ_CMD_QUEUE) {
                raider += s_w.log[k].c.type == M_RAIDER;
                sub += s_w.log[k].c.type == M_SUB;
            }
        printf("the finish's army, %s: constructors %d army %d (raider %d, submarine %d)\n", NAME[c], cons, army,
               raider, sub);
        if (c == 0) CHECK(army == 1 && raider == 1, "%s: the raider", NAME[c]);
        if (c == 1) CHECK(army == 1 && sub == 1 && !raider, "%s: the submarine alone", NAME[c]);
        if (c == 2) CHECK(!army, "%s: no army order", NAME[c]);
        if (c == 3) CHECK(army == 1 && raider == 1, "%s: the raider as usual", NAME[c]);
        s_ti[T_FACTORY].flags &= (uint8_t)~IMPAI_TF_WATER;
        s_mil.dry = 0;
        s_ti[M_SUB].attack &= (uint8_t)~IMPAI_TA_WATER1;
    }
    s_milp = NULL;
}

/* THE TARGETS BOUGHT FOR (impai_econ.c, army_menu): the governor's world, no constructor the plan allows, the
   factory's menu the submarine (waterweapons alone, 400) and the destroyer (a gun and a waterweapon, 500), both of
   the sea raiders, so the type choice with the picture empty takes the cheaper, the submarine, wherever it may:
   (a) no start and nothing known: no evidence, nothing narrowed, the submarine;
   (b) a foe's start on land: DRY expected, the destroyer;
   (c) (b) and an enemy ship known IMPAI_TG_HOLD ago: FLOAT known, the submarine;
   (d) (c) a tick older: no longer known, the destroyer;
   (e) (b) and an enemy known under the sea: the submarine;
   (f) the foe's start in the sea, 40 deep: FLOAT and UNDER expected, the submarine;
   (g) (b) with the submarine alone on the menu: no army order;
   (h) (f) with the raider alone on the menu, its gun no waterweapon: a gun hits what floats, the raider;
   (i) no start and an aircraft known, the raider alone on the menu: no start says where the base stands, so DRY
       is kept beside AIR, the raider;
   (j) (i) with one start, the seat's own: the raider;
   (k) (i) with two starts, the other left empty, so none can hold an enemy's base: the raider. */
static void check_army_targets(void)
{
    static const uint16_t BOTH[] = { M_SUB, M_DEST }, SUB[] = { M_SUB }, RAIDER[] = { M_RAIDER };
    static const char* const NAME[11] = { "(a) no evidence", "(b) a foe's start on land", "(c) a ship known",
                                         "(d) the ship known a tick past the hold", "(e) a submarine known",
                                         "(f) the foe's start in the sea", "(g) the submarine alone, a land start",
                                         "(h) the raider alone, the start in the sea",
                                         "(i) the raider alone, no start, an aircraft known",
                                         "(j) the raider alone, one start, an aircraft known",
                                         "(k) the raider alone, the other start empty, an aircraft known" };
    static const int WANT[11] = { M_SUB, M_DEST, M_SUB, M_DEST, M_SUB, M_SUB, 0, M_RAIDER, M_RAIDER, M_RAIDER,
                                  M_RAIDER };
    IMPAI_BRAIN_OUT bo;
    int c, cons, army, k;
    for (c = 0; c < 11; c++) {
        int from, sub = 0, dest = 0, raider = 0;
        gov_world(1);
        mil_sea_types();
        s_ti[M_DEST].klass = IMPAI_K_SRAIDER;
        menu_set(&s_w, T_FACTORY, c == 6 ? SUB : c >= 7 ? RAIDER : BOTH, c >= 6 ? 1 : 2);
        gov_units(M_RAIDER, 20, 100);
        s_glv.cons_open = 0;
        s_glv.army_per_cons = 255;
        s_w.gametime = 10000;
        if (c >= 8) s_mil.tg_t[IMPAI_TG_AIR] = 9000u;
        if (c >= 9) {
            s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
            s_w.starts[1] = (WQ_START){ 250 * 16 + 8, 125 * 16 + 8, 1, WQ_SEATED_NONE };
            s_w.nstarts = c == 9 ? 1 : 2;
        }
        if (c > 0 && c < 8) {
            s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
            s_w.starts[1] = (WQ_START){ 250 * 16 + 8, 125 * 16 + 8, 1, 0 };
            s_w.nstarts = 2;
        }
        if (c == 2) s_mil.tg_t[IMPAI_TG_FLOAT] = 10000u - IMPAI_TG_HOLD;
        if (c == 3) s_mil.tg_t[IMPAI_TG_FLOAT] = 10000u - IMPAI_TG_HOLD - 1u;
        if (c == 4) s_mil.tg_t[IMPAI_TG_UNDER] = 9000u;
        if (c == 5 || c == 7) {
            s_w.sea = 50;
            water_add(&s_w, 240, 115, 260, 135, 40);
        }
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &cons, &army) == 0, "decide: %s", NAME[c]);
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.verb == WQ_CMD_QUEUE) {
                sub += s_w.log[k].c.type == M_SUB;
                dest += s_w.log[k].c.type == M_DEST;
                raider += s_w.log[k].c.type == M_RAIDER;
            }
        printf("the targets bought for, %s: army %d (submarine %d, destroyer %d, raider %d)\n", NAME[c], army, sub,
               dest, raider);
        CHECK(WANT[c] ? army == 1 && sub == (WANT[c] == M_SUB) && dest == (WANT[c] == M_DEST) &&
                            raider == (WANT[c] == M_RAIDER)
                      : !army,
              "%s: %s", NAME[c], WANT[c] == M_SUB ? "the submarine" : WANT[c] == M_DEST ? "the destroyer"
                                 : WANT[c] ? "the raider" : "no army order");
    }
    s_milp = NULL;
}

/* THE ISLAND'S LAND BUDGET (impai_econ.c, island_classes): the governor's world, no constructor the plan allows, the
   raider's value 100 and the destroyer's 300 (a sea assault), twenty raiders of the seat's, so its land army's
   value is 2 000; on THE ISLAND the estimate stands at block (40, 4), on the land's second component, which the
   raider's class does not reach, so the economy's reach agrees (THE ISLAND'S AGREEMENT, read from the menus with
   no values):
   (a) not on THE ISLAND, the raider's menu: the raider (Q3);
   (b) on THE ISLAND, no sea army: no army order;
   (c) on THE ISLAND, twenty destroyers: the land army a quarter of the army's 8 000 exactly, no army order;
   (d) on THE ISLAND, twenty-one destroyers: below a quarter, the raider;
   (e) (b) with the menu the raider and the destroyer: the destroyer, the land class refused, not the order;
   (f) (b) with the estimate where the gov world keeps it, on the raider's component: the economy's reach disagrees,
       so no budget, the raider. */
static void check_island_budget(void)
{
    static const uint16_t RAIDER[] = { M_RAIDER }, BOTH[] = { M_RAIDER, M_DEST };
    static const char* const NAME[6] = { "(a) not the island", "(b) the island, no sea army",
                                         "(c) the island, a quarter exactly", "(d) the island, below a quarter",
                                         "(e) the island, the destroyer on the menu",
                                         "(f) the island, the estimate the raider reaches" };
    static const int DEST[6] = { 0, 0, 20, 21, 0, 0 };
    static const int WANT[6] = { M_RAIDER, 0, 0, M_RAIDER, M_DEST, M_RAIDER };
    IMPAI_BRAIN_OUT bo;
    int c, cons, army, k;
    for (c = 0; c < 6; c++) {
        int from, raider = 0, dest = 0;
        gov_world(1);
        mil_sea_types();
        menu_set(&s_w, T_FACTORY, c == 4 ? BOTH : RAIDER, c == 4 ? 2 : 1);
        s_ti[M_RAIDER].value = 100.0f;
        s_ti[M_DEST].value = 300.0f;
        gov_units(M_RAIDER, 20, 100);
        gov_units(M_DEST, DEST[c], 200);
        s_glv.cons_open = 0;
        s_glv.army_per_cons = 255;
        s_mil.island = (uint8_t)(c > 0);
        s_mil.ebx = (int16_t)(c > 0 && c < 5 ? 40 : s_mil.ebx);
        s_mil.ebz = (int16_t)(c > 0 && c < 5 ? 4 : s_mil.ebz);
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &cons, &army) == 0, "decide: %s", NAME[c]);
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.verb == WQ_CMD_QUEUE) {
                raider += s_w.log[k].c.type == M_RAIDER;
                dest += s_w.log[k].c.type == M_DEST;
            }
        printf("the island's land budget, %s: army %d (raider %d, destroyer %d)\n", NAME[c], army, raider, dest);
        CHECK(WANT[c] ? army == 1 && raider == (WANT[c] == M_RAIDER) && dest == (WANT[c] == M_DEST) : !army,
              "%s: %s", NAME[c], WANT[c] == M_RAIDER ? "the raider" : WANT[c] ? "the destroyer" : "no army order");
        s_mil.island = 0;
    }
    s_milp = NULL;
}

/* THE TARGETS KNOWN (impai_mil.c, targets_known): the military's world, enemies seen by the seat's sight. A tank and
   a ship (a type placed on water only) stamp DRY and FLOAT with the decision's GameTime and nothing else; a later
   decision that sees a submarine (held submerged by the fight content) and an aircraft stamps UNDER and AIR and
   leaves the earlier stamps; a radar contact never identified stamps DRY. */
static void check_targets_known(void)
{
    IMPAI_MIL_OUT o;
    uint32_t t1, t2;
    mil_world();
    mil_sea_types();
    s_ti[M_DEST].flags |= IMPAI_TF_WATER;
    s_ti[M_SCOUT].flags |= IMPAI_TF_VTOL;
    enemy(1300, M_TANK, 200, 100, IMPAI_LOS, 1);
    enemy(1301, M_DEST, 210, 100, IMPAI_LOS, 1);
    t1 = (uint32_t)s_w.gametime;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the targets known: the first decision runs");
    printf("the targets known, a tank and a ship at %u: %u %u %u %u\n", t1, s_mil.tg_t[IMPAI_TG_DRY],
           s_mil.tg_t[IMPAI_TG_FLOAT], s_mil.tg_t[IMPAI_TG_UNDER], s_mil.tg_t[IMPAI_TG_AIR]);
    CHECK(s_mil.tg_t[IMPAI_TG_DRY] == t1 && s_mil.tg_t[IMPAI_TG_FLOAT] == t1 && !s_mil.tg_t[IMPAI_TG_UNDER] &&
              !s_mil.tg_t[IMPAI_TG_AIR] &&
              impai_tg_known(&s_mil, t1) == ((1u << IMPAI_TG_DRY) | (1u << IMPAI_TG_FLOAT)),
          "a tank and a ship: DRY and FLOAT");
    memset(s_rows, 0, sizeof s_rows);
    s_w.unit[1300].live = s_w.unit[1301].live = 0;
    enemy(1302, M_SUB, 210, 110, IMPAI_LOS, 1);
    enemy(1303, M_SCOUT, 180, 100, IMPAI_LOS, 1);
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    t2 = (uint32_t)s_w.gametime;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the targets known: the second decision runs");
    printf("the targets known, a submarine and an aircraft at %u: %u %u %u %u\n", t2, s_mil.tg_t[IMPAI_TG_DRY],
           s_mil.tg_t[IMPAI_TG_FLOAT], s_mil.tg_t[IMPAI_TG_UNDER], s_mil.tg_t[IMPAI_TG_AIR]);
    CHECK(s_mil.tg_t[IMPAI_TG_DRY] == t1 && s_mil.tg_t[IMPAI_TG_FLOAT] == t1 && s_mil.tg_t[IMPAI_TG_UNDER] == t2 &&
              s_mil.tg_t[IMPAI_TG_AIR] == t2, "a submarine and an aircraft: UNDER and AIR, the earlier stamps kept");
    memset(s_rows, 0, sizeof s_rows);
    s_w.unit[1302].live = s_w.unit[1303].live = 0;
    enemy(1304, M_TANK, 200, 100, IMPAI_RADAR, 1);
    s_rows[1304].seat[0].type = (uint16_t)WQ_NONE;
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the targets known: the third decision runs");
    printf("the targets known, a radar contact at %d: %u %u %u %u\n", s_w.gametime, s_mil.tg_t[IMPAI_TG_DRY],
           s_mil.tg_t[IMPAI_TG_FLOAT], s_mil.tg_t[IMPAI_TG_UNDER], s_mil.tg_t[IMPAI_TG_AIR]);
    CHECK(s_mil.tg_t[IMPAI_TG_DRY] == (uint32_t)s_w.gametime && s_mil.tg_t[IMPAI_TG_FLOAT] == t1,
          "a radar contact never identified: DRY");
    s_ti[M_SCOUT].flags &= (uint8_t)~IMPAI_TF_VTOL;
}

/* THE FINISH'S ROWS (impai_econ.c, army_rows), the factory's menu the constructor and the raider, in a dry hunt the
   submarine (a water weapon) too, no constructor the plan allows (cons_max 0): on a level that can hold the dry
   hunt (land_total, sea_blocks),
   (a) 447 raiders, out of a dry hunt: one more raider, the land army's last row; (b) 448: no raider and no order,
   the rows past IMPAI_ARMY_MAX - IMPAI_FINISH_ROWS kept; (c) 448 raiders in a dry hunt at the water plant: the
   submarine; (d) 511 raiders and submarines together in a dry hunt: one submarine, the last row; on a level that
   cannot be dry, (e) 448 raiders: one more raider, every row the land army's. The refusal at (b) alone is counted
   (IMPAI_BRAIN_OUT.finish_rows). */
static void check_gov_finish_rows(void)
{
    static const uint16_t MENU[] = { T_CONS, M_RAIDER, M_SUB };
    static const char* const NAME[5] = { "447 raiders", "448 raiders", "448 raiders, dry",
                                         "511 raiders and submarines, dry", "448 raiders, no sea" };
    static const int N[5] = { 447, 448, 448, 511, 448 };
    IMPAI_BRAIN_OUT bo;
    int c, cons, army, k, raider, sub;
    for (c = 0; c < 5; c++) {
        int from;
        gov_world(1);
        mil_sea_types();
        s_ti[M_SUB].attack |= IMPAI_TA_WATER1;
        menu_set(&s_w, T_FACTORY, MENU, c == 2 || c == 3 ? 3 : 2);
        gov_units(M_RAIDER, c == 3 ? 448 : N[c], 100);
        if (c == 3) gov_units(M_SUB, N[c] - 448, 100 + 448);
        if (c == 2 || c == 3) s_ti[T_FACTORY].flags |= IMPAI_TF_WATER;
        s_reach.land_total = 1;
        s_reach.sea_blocks = c == 4 ? 0u : 1u;
        s_glv.cons_open = 0;
        s_glv.cons_max = 0;
        s_mil.dry = (uint8_t)(c == 2 || c == 3);
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &cons, &army) == 0, "decide: %s", NAME[c]);
        raider = sub = 0;
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.verb == WQ_CMD_QUEUE) {
                raider += s_w.log[k].c.type == M_RAIDER;
                sub += s_w.log[k].c.type == M_SUB;
            }
        printf("the finish's rows, %s: constructors %d army %d (raider %d, submarine %d)\n", NAME[c], cons, army,
               raider, sub);
        if (c == 0 || c == 4) CHECK(army == 1 && raider == 1, "%s: one more raider", NAME[c]);
        if (c == 1) CHECK(!army && !cons, "%s: the finish's rows kept, no order", NAME[c]);
        if (c == 2 || c == 3) CHECK(army == 1 && sub == 1 && !raider, "%s: the submarine", NAME[c]);
        CHECK(bo.finish_rows == (c == 1), "%s: %u refusals at the finish's rows", NAME[c], bo.finish_rows);
        s_ti[T_FACTORY].flags &= (uint8_t)~IMPAI_TF_WATER;
        s_mil.dry = 0;
        s_ti[M_SUB].attack &= (uint8_t)~IMPAI_TA_WATER1;
    }
    s_milp = NULL;
}

/* A MOVE OF NO JOB on the factories' turn (impai_brain.c, THE FACTORIES' TURN): turned_back_case's walk across the
   row, an idle factory beside it, one action, the factories' turn: the builder is turned back all the same. */
static void turn_back_on_factories_turn(void)
{
    IMPAI_BRAIN_OUT out;
    walk_world(2, IMPAI_JOB_BUILD);
    route_row(IMPAI_LOS, ROW_CELL21);
    unit_add(&s_w, 6, T_FACTORY, 40, 40, 1);
    s_st.root_turn = 1;
    s_dec_acts = 1;
    CHECK(decide(&out) == 0, "decide");
    s_dec_acts = -1;
    s_viewp = NULL;
    printf("the factories' turn, a builder in danger: turned back %d, roots %u\n", moved_home(0), out.roots);
    CHECK(moved_home(0) == 1 && out.roots == 1, "the turn back is never held by the factories' turn");
}

/* THE YARD ON THE FACTORIES' TURN (impai_brain.c, THE FACTORIES' TURN): check_yard_clear's helper on the waiting
   lab's yard, a second factory idle with an empty menu -- one the governor leaves unordered --, the turn the
   factories', and decisions of one action: the helper is moved off the yard within two. */
static void yard_world(void);
static void yard_on_factories_turn(void)
{
    static const uint16_t NONE[1] = { 0 };
    IMPAI_BRAIN_OUT out;
    int32_t x = 0, z = 0;
    int k, at = -1;
    yard_world();
    unit_add(&s_w, 6, T_FACTORY, 40, 40, 1);
    menu_set(&s_w, T_FACTORY, NONE, 0);
    memcpy(s_w.unit[2].pos, s_w.unit[3].pos, sizeof s_w.unit[2].pos);
    s_w.unit[2].list[0].action = A_GUARD;
    s_w.unit[2].list[0].target = 3;
    s_w.unit[2].n = 1;
    s_job[2].kind = IMPAI_JOB_ASSIST;
    s_job[2].type = T_CONS;
    s_job[2].build = (uint16_t)WQ_NONE;
    s_job[2].target = 3;
    s_job[2].since = (uint32_t)s_w.gametime;
    s_st.root_turn = 1;
    for (k = 0; k < 6 && at < 0; k++) {
        const int from = s_w.nlog;
        s_dec_acts = 1;
        CHECK(decide(&out) == 0, "decide");
        s_dec_acts = -1;
        if (last_move(from, 2, &x, &z)) at = k;
        s_w.gametime += 15;
    }
    printf("the factories' turn, the yard beside an unordered factory: the helper moved at decision %d\n", at);
    CHECK(at >= 0 && at <= 1, "the yard's step waits at most the factories' turn");
}

/* THE FACTORIES' TURN (impai_brain.c, root_cap_of): gov_world's idle commander and idle factory under a budget of
   one action. (a) turn 0: the commander's order, the factory none, the turn flipped to 1; (b) turn 1: the factory's
   order, the commander none, the turn flipped to 0; (c) turn 1 with the factory busy: no contest, the commander's
   order, the turn kept; (d) two actions, turn 0: one each, the turn kept; (e) turn 1 with a factory the governor
   leaves unordered, its menu empty: the commander's order, and the turn passes all the same. Then a builder in
   danger on the factories' turn (turn_back_on_factories_turn), the yard's step beside a factory left unordered
   (yard_on_factories_turn), and a run of six decisions of one action, the factory's list and the commander's
   emptied after each: three roots each, alternating. */
static void check_factory_turn(void)
{
    static const char* const NAME[5] = { "the builders' turn", "the factories' turn", "the factory busy",
                                         "two actions", "the factory unordered on its turn" };
    static const uint16_t NONE[1] = { 0 };
    IMPAI_BRAIN_OUT bo;
    int k, c, a, from, com, fac = 0, coms = 0, last = -1, alternates = 1;
    for (k = 0; k < 5; k++) {
        const int turn = k == 1 || k == 2 || k == 4;
        gov_world(1);
        if (k == 4) menu_set(&s_w, T_FACTORY, NONE, 0);
        if (k == 2) {
            FUNIT* f = &s_w.unit[2];
            f->list[0].action = A_BUILDINGBUILD;
            f->list[0].type = M_RAIDER;
            f->list[0].target = (uint16_t)WQ_NONE;
            f->list[0].count = 1;
            f->n = 1;
        }
        s_st.root_turn = (uint8_t)turn;
        s_gov_acts = k == 3 ? 2 : 1;
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &c, &a) == 0, "the turn's decision runs");
        s_gov_acts = -1;
        for (com = 0; from < s_w.nlog; from++) com += s_w.log[from].c.unit == 1;
        printf("the factories' turn, %s: commander %d factory %d, turn %u\n", NAME[k], com, c + a, s_st.root_turn);
        CHECK(com == (k != 1) && c + a == (k == 1 || k == 3), "%s: who is given the root", NAME[k]);
        CHECK(s_st.root_turn == (k < 2 || k == 4 ? !turn : turn), "%s: the turn", NAME[k]);
    }
    turn_back_on_factories_turn();
    yard_on_factories_turn();
    gov_world(1);
    for (k = 0; k < 6; k++) {
        int who;
        s_gov_acts = 1;
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &c, &a) == 0, "the run's decision runs");
        s_gov_acts = -1;
        for (com = 0; from < s_w.nlog; from++) com += s_w.log[from].c.unit == 1;
        who = c + a ? 1 : com ? 0 : -1;
        fac += c + a > 0;
        coms += com > 0;
        if (who < 0 || who == last) alternates = 0;
        last = who;
        s_w.unit[1].n = s_w.unit[2].n = 0;
        impai_job_end(&s_job[1]);
        s_w.gametime += 15;
    }
    printf("the factories' turn, a run of six: factory %d commander %d, alternating %d\n", fac, coms, alternates);
    CHECK(fac == 3 && coms == 3 && alternates, "six decisions of one action: three each, alternating");
    s_milp = NULL;
}

/* THE ECONOMY'S SHARES (IMPAI_BRAIN_OUT.e_empty, f_idle, f_all): two factories, one busy with a queue; the energy
   store at 4 % of its storage, then at 6 %: one idle of two, and an empty store counted at 4 % alone. */
static void check_eco_shares(void)
{
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 2; k++) {
        FUNIT* f;
        gov_world(2);
        f = &s_w.unit[3];
        f->list[0].action = A_BUILDINGBUILD;
        f->list[0].type = M_RAIDER;
        f->list[0].target = (uint16_t)WQ_NONE;
        f->list[0].count = 1;
        f->n = 1;
        s_w.econ[0].energy = (k ? 0.06f : 0.04f) * s_w.econ[0].energy_max;
        CHECK(gov_decide(&bo, &c, &a) == 0, "the shares' decision runs");
        printf("the economy's shares, energy at %d %%: e_empty %u f_idle %u f_all %u\n", k ? 6 : 4, bo.e_empty, bo.f_idle,
               bo.f_all);
        CHECK(bo.e_empty == !k && bo.f_idle == 1 && bo.f_all == 2, "the shares at %d %%", k ? 6 : 4);
    }
    s_milp = NULL;
}

/* THE FIVE, bounded (impai_econ.c, surplus_count) on a budgeted seat in a surplus, the store at 0.9 of the storage,
   the factory's menu one gunship: one factory at 300 metal a unit, two units; two at 150, two then three, the
   second order sharing what the first left; 510 army units, the rows' two; 512, none, with or without a budget,
   the factory counted idle; two factories at 508, one order of the rows' four. THE QUEUED METAL (impai_brain.c):
   two factories at 150, the second busy with a count node of three gunships -- two past the one it makes, 300 of
   the store --: the idle one's order is two units, where the whole store buys four and the made unit counted
   too leaves one. A NaN metal cost: one unit, never the queue's five. */
static void check_gov_five(void)
{
    static const uint16_t AIR[] = { M_SCOUT };
    static const char* const NAME[8] = { "one factory, 300 metal", "two factories, 150 metal",
                                         "510 army units", "512 army units", "512 army units, no budget",
                                         "two factories, 508 army units", "150 metal, the other factory's queue",
                                         "150 metal, a NaN cost" };
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 8; k++) {
        int from, first, second;
        gov_world(k == 1 || k == 5 || k == 6 ? 2 : 1);
        menu_set(&s_w, T_FACTORY, AIR, 1);
        s_ti[M_SCOUT].klass = IMPAI_K_GUNSHIP;
        s_w.defs[M_SCOUT].e.cost_m = k == 0 ? 300.0f : k == 1 || k == 6 ? 150.0f : k == 7 ? NAN : 40.0f;
        s_w.econ[0].metal = 0.9f * s_w.econ[0].metal_max;
        if (k == 6) {
            FUNIT* f = &s_w.unit[3];
            f->list[0].action = A_BUILDINGBUILD;
            f->list[0].type = M_SCOUT;
            f->list[0].target = (uint16_t)WQ_NONE;
            f->list[0].count = 3;
            f->n = 1;
        }
        if (k >= 2 && k <= 5) gov_units(M_RAIDER, k == 2 ? 510 : k == 5 ? 508 : 512, 100);
        if (k >= 6)
            for (c = 100; c < 100 + 512; c++) s_w.unit[c].live = 0;
        s_gov_acts = k == 4 ? -1 : 8;
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        s_gov_acts = -1;
        first = queue_counts(from, &second);
        printf("governor, the five bounded, %s: orders %d, counts %d %d, fidle %u, poor %u, army %u\n", NAME[k], a, first,
               second, bo.fidle, bo.poor, bo.army);
        if (k == 0) CHECK(a == 1 && first == 2, "the store pays two units of 300 above the hold's line");
        if (k == 1) CHECK(a == 2 && first == 2 && second == 3, "two orders share the store: 2 then 3");
        if (k == 2) CHECK(a == 1 && first == 2 && bo.army == 2, "the rows leave two units");
        if (k == 3 || k == 4)
            CHECK(a == 0 && bo.fidle == 1 && !bo.finish_rows,
                  "no army order past the rows, and the factory counted idle; no finish's rows refusal");
        if (k == 5) CHECK(a == 1 && first == 4 && bo.army == 4 && bo.fidle == 1,
                          "one order of the rows' four units, counted whole: none at the second factory");
        if (k == 6) CHECK(a == 1 && first == 2, "the other factory's two queued units take 300 of the store: two units");
        if (k == 7) CHECK(a == 1 && first == 1, "a NaN cost is no free unit: one");
    }
    s_ti[M_SCOUT].klass = IMPAI_K_LSCOUT;
    s_w.defs[M_SCOUT].e.cost_m = 40.0f;
    s_milp = NULL;
}

/* THE LOSS LOOP: two factories, six constructors and no army, 20 decisions, each queue finished at once and one
   constructor lost a decision. A quota that refills the constructors queues 40 of them and no army; the governor
   builds the army up to the share of the constructors standing and keeps it there: a lost constructor is replaced
   only while the army standing beside it covers the replacement's share (THE ARMY'S SHARE). Both sides of the share
   are held at every decision: each factory in turn queues a constructor exactly when the share, the opening and
   cons_max admit one over the units standing and this decision's orders before it, and army otherwise. */
static void check_gov_loss_loop(void)
{
    IMPAI_BRAIN_OUT bo;
    int c, a, i, k, f, at = 100, cons = 0, army = 0, standing = 0, wrong = 0;
    gov_world(2);
    gov_units(T_CONS, 6, 10);
    for (i = 0; i < 20; i++) {
        const int from = s_w.nlog;
        int sc = 0, sa = 0, pc = 0, pa = 0;
        for (k = 1; k < FW_SLOTS / 2; k++) {
            const FUNIT* u = &s_w.unit[k];
            if (!u->live || !u->finished) continue;
            sc += u->type == T_CONS;
            sa += s_ti[u->type].klass < IMPAI_KS;
        }
        for (f = 0; f < 2; f++) {
            if (sc < s_glv.cons_max &&
                (sc < s_glv.cons_open || sa >= s_glv.army_per_cons * (sc - s_glv.cons_open + 1))) {
                pc++;
                sc++;
            } else {
                pa++;
                sa++;
            }
        }
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide %d", i);
        if (c != pc || a != pa) {
            wrong++;
            printf("governor, the loss loop's decision %d: queued %d constructors, %d army; the share admits %d, %d\n", i,
                   c, a, pc, pa);
        }
        cons += c;
        army += a;
        at = gov_finish(from, at);
        for (k = 1; k < FW_SLOTS / 2; k++)
            if (s_w.unit[k].live && s_w.unit[k].type == T_CONS && s_w.unit[k].finished) {
                s_w.unit[k].live = 0;
                break;
            }
        s_w.gametime += 15;
        if (s_w.nlog >= FW_LOG - 16) s_w.nlog = 0;
    }
    for (k = 1; k < FW_SLOTS / 2; k++) standing += s_w.unit[k].live && s_w.unit[k].type == T_CONS;
    printf("governor, the loss loop: %d constructors, %d army queued, %d constructors standing\n", cons, army,
           standing);
    CHECK(army > 0 && army >= s_glv.army_per_cons * (standing - s_glv.cons_open),
          "the loss loop keeps the army's share: %d army, %d constructors standing", army, standing);
    CHECK(!wrong, "the loss loop: %d decisions queued other than the share admits", wrong);
    s_milp = NULL;
}

/* THE HOLD'S FLOOR (impai_econ.c, THE GOVERNOR): two idle factories, one constructor and no army, so each order is
   army. At 100 stored of 1000, a hold: one order, the other factory held. At 250: two orders. At 100 with the
   second factory busy: no order, the idle one held. At 100 under a pressure stamp: two orders, no hold. */
static void check_hold_floor(void)
{
    static const char* const NAME[4] = { "100 stored", "250 stored", "100 stored, one factory busy",
                                         "100 stored, pressed" };
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 4; k++) {
        gov_world(2);
        gov_units(T_CONS, 1, 10);
        if (k == 2) gov_busy(1);
        if (k == 3) s_mil.press_until = (uint32_t)s_w.gametime + 100u;
        s_w.econ[0].metal = k == 1 ? 250.0f : 100.0f;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        printf("the hold's floor, %s: queued %d constructors, %d army, hold %u held %u\n", NAME[k], c, a, bo.hold,
               bo.held);
        CHECK(k == 0   ? c + a == 1 && bo.held == 1 && bo.hold
              : k == 2 ? c + a == 0 && bo.held == 1 && bo.hold
                       : c + a == 2 && bo.held == 0 && !bo.hold,
              "the hold's floor, %s: %d orders, hold %u held %u", NAME[k], c + a, bo.hold, bo.held);
    }
    s_milp = NULL;
}

/* THE GOVERNOR WITHOUT A BUILD POWER: one constructor and ten army units at 1 metal a pass and 500 stored, a
   constructor; with cons_max constructors, army. At 100 stored, a hold, the hold refuses none: no constructor and
   no army, a constructor (the opening) at the floor's factory; one constructor and ten army, a constructor. */
static void check_hold_cons(void)
{
    static const char* const NAME[4] = { "one constructor, 500 stored", "cons_max constructors, 500 stored",
                                         "none, 100 stored", "one constructor, 100 stored" };
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 4; k++) {
        gov_world(1);
        gov_units(T_CONS, k == 1 ? s_glv.cons_max : k == 2 ? 0 : 1, 10);
        if (k != 2) gov_units(M_RAIDER, 10, 100);
        s_w.econ[0].metal_in = 1.0f;
        s_w.econ[0].metal = k >= 2 ? 100.0f : 500.0f;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        printf("the governor, %s: queued %d constructors, %d army, hold %u\n", NAME[k], c, a, bo.hold);
        CHECK(k == 1 ? c == 0 && a == 1 : c == 1 && a == 0, "the governor, %s: %d, %d", NAME[k], c, a);
        CHECK(!!bo.hold == (k >= 2), "the governor, %s: hold %u", NAME[k], bo.hold);
    }
    s_milp = NULL;
}

/* THE HOLD'S DATA (impai_brain.c, metal_whole): the store and the storage are data. NaN stored reads 0, a hold; -5
   reads 0, a hold; 10^30 stored is a surplus; a storage of 0 or NaN never holds; a storage of 10^30 with 500
   stored, a hold. */
static void check_hold_data(void)
{
    static const struct { const char* name; float metal, max; int hold; } CASE[6] = {
        { "NaN of 1000", NAN, 1000.0f, 1 }, { "-5 of 1000", -5.0f, 1000.0f, 1 }, { "10^30 of 1000", 1e30f, 1000.0f, 0 },
        { "100 of 0", 100.0f, 0.0f, 0 }, { "100 of NaN", 100.0f, NAN, 0 }, { "500 of 10^30", 500.0f, 1e30f, 1 },
    };
    IMPAI_BRAIN_OUT bo;
    int c, a, k;
    for (k = 0; k < 6; k++) {
        gov_world(1);
        s_w.econ[0].metal = CASE[k].metal;
        s_w.econ[0].metal_max = CASE[k].max;
        CHECK(gov_decide(&bo, &c, &a) == 0, "decide");
        printf("the hold's data, %s stored: hold %u\n", CASE[k].name, bo.hold);
        CHECK(bo.hold == CASE[k].hold, "the hold's data, %s: hold %u", CASE[k].name, bo.hold);
    }
    s_milp = NULL;
}

static void member(int slot, int type, int cx, int cz, int s);
static void squad_task(int s, unsigned task_, unsigned layer, int tbx, int tbz);

/* THE PRESSURE (impai_mil.c): an own extractor 2 600 px east of the anchor, past base_r + 512, an enemy raider seen
   300 px east of it and an enemy extractor at the enemy's far start. The seat has no army: a stamp of press_hold,
   at an expansion, o.press 1. With an own tank (worth 300, past press_k x 120): none. An enemy commander there
   instead: none. The raider 190 px from an enemy extractor and 300 px from ours: none. The raider within base_r of
   the anchor: a stamp at the base. A radar contact never identified there, the seat's one raider its substitute:
   a stamp. The same contact, the seat's one army unit -- its substitute -- of a type with no ground reach (an
   anti-air unit's row, hit_ground 0): none. The tank in an ATTACK squad instead, which is no home (impai_mil.c,
   THE HOME): none at the expansion, which the whole army answers, and a stamp at the base, which only the home
   defends -- and so with the recall unconfirmed (react_confirm unreached), where the home's weight alone stamps it. */
static void check_mil_pressure(void)
{
    static const char* const NAME[10] = { "a raider at the expansion", "an army that outweighs it", "an enemy commander",
                                          "a raider at home", "a raider at the base", "a radar contact",
                                          "a radar contact, its substitute with no ground reach",
                                          "an army that outweighs it, out on a wave",
                                          "a raider at the base, the army out on a wave",
                                          "a raider at the base, the army out on a wave, the recall unconfirmed" };
    IMPAI_PLAN unconfirmed = IMPAI_PLAN_ROW;
    IMPAI_MIL_OUT o;
    int k;
    unconfirmed.react_confirm = 255;
    for (k = 0; k < 10; k++) {
        const int stamp = k == 0 || k == 4 || k == 5 || k == 8 || k == 9;
        mil_world();
        own(20, T_MEX, 183, 20);
        enemy(1310, T_MEX, 480, 200, IMPAI_LOS, 1);
        if (k == 1) own(2, M_TANK, 30, 60);
        if (k >= 7) {
            squad_task(1, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 200, 100);
            member(2, M_TANK, 30, 60, 1);
        }
        if (k == 5) own(2, M_RAIDER, 10, 60);
        if (k == 6) {
            own(2, M_AA, 10, 60);
            s_threat[M_AA].hit_ground = 0;
        }
        if (k == 3) enemy(1311, T_MEX, 214, 20, IMPAI_LOS, 1);
        enemy(1300, k == 2 ? T_COM : M_RAIDER, k == 4 || k >= 8 ? 40 : 202, 20, k == 5 || k == 6 ? IMPAI_RADAR : IMPAI_LOS,
              1);
        if (k == 5 || k == 6) {
            s_rows[1300].seat[0].type = (uint16_t)WQ_NONE;
            s_rows[1300].seat[0].owner = 0xFF;
        }
        CHECK(mil_decide(k == 9 ? &unconfirmed : &IMPAI_PLAN_ROW, 1, &o) == 0, "the pressure decision runs");
        printf("pressure, %s: press %u until %u v %.0f block (%d, %d) base %u\n", NAME[k], o.press, s_mil.press_until,
               (double)s_mil.press_v, s_mil.press_bx, s_mil.press_bz, s_mil.press_base);
        if (stamp)
            CHECK(o.press == 1 && s_mil.press_until == (uint32_t)s_w.gametime + IMPAI_PLAN_ROW.press_hold &&
                      s_mil.press_base == (k == 4 || k >= 8),
                  "pressure, %s: a stamp, base %u", NAME[k], s_mil.press_base);
        else
            CHECK(!o.press && !s_mil.press_until, "pressure, %s: no stamp", NAME[k]);
    }
}

/* THE MILITARY'S SUBSTITUTE, which the builders' ground threat map reads (IMPAI_MIL.radar_as): a radar contact never
   identified, with no identified enemy seen, is listed and counts as the seat's own median army type, the tank */
static void check_mil_radar_as(void)
{
    IMPAI_MIL_OUT mo;
    mil_world();
    enemy(1300, M_RAIDER, 202, 20, IMPAI_RADAR, 1);
    s_rows[1300].seat[0].type = (uint16_t)WQ_NONE;
    s_rows[1300].seat[0].owner = 0xFF;
    own(2, M_TANK, 30, 60);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &mo) == 0, "decide");
    printf("radar contact in the military's picture: enemies %u, radar_as %u\n", mo.enemies, s_mil.radar_as);
    CHECK(mo.enemies == 1 && s_mil.radar_as == M_TANK, "the contact listed, counted as the seat's own median type");
}

/* NO ARMY CLASS WITHOUT DAMAGE (impai_table.c, impai_class_of) through the classifier: a land army of a tank, a
   raider, ARMFLASH's record (a 9-damage gun at a 9-tick reload, maxvelocity 2.6) and three types that deal
   nothing -- CORSPEC's retail record (no weapon, a jammer, 94 of value at maxvelocity 1.2) and two with one
   regular weapon whose [DAMAGE] default is 0, one fast and one slow. The three are no class; the others keep
   theirs. A commander that builds them all gives the classifier its tech levels. */
static void check_army_damage(void)
{
    enum { TANK, FLASH, RAID, SPEC, ZERO, ZERO2, COM, N };
    static const char* const NAME[N] = { "TANK", "ARMFLASH", "RAIDER", "CORSPEC", "ZERO", "ZERO2", "COMMANDER" };
    static const uint16_t BUILDS[] = { TANK, FLASH, RAID, SPEC, ZERO, ZERO2 };
    TASIM_ROLE_IN in[N];
    TASIM_ROLE_OUT out[N];
    TASIM_ROLE_Q q;
    void* work = malloc(tasim_roles_work(N));
    unsigned k[N];
    int i;
    memset(in, 0, sizeof in);
    for (i = 0; i < N; i++) {
        in[i].flags245 = TASIM_F245_CANMOVE;
        in[i].bmcode = 1;
        in[i].maxdamage = 1000;
        in[i].nweapons = 1;
        in[i].w[0].range = 300;
        in[i].w[0].reload_ticks = 30.0f;
        in[i].w[0].burst = 1;
        in[i].w[0].damage_default = 50;
    }
    in[TANK].cost_metal = 300.0f;
    in[TANK].maxvelocity_fx = 65536;
    in[FLASH].cost_metal = 110.0f;
    in[FLASH].maxvelocity_fx = (int32_t)(2.6 * 65536);
    in[FLASH].w[0].reload_ticks = 9.0f;
    in[FLASH].w[0].damage_default = 9;
    in[RAID].cost_metal = 120.0f;
    in[RAID].maxvelocity_fx = 2 * 65536;
    in[SPEC].cost_metal = 94.0f;
    in[SPEC].maxvelocity_fx = (int32_t)(1.2 * 65536);
    in[SPEC].jammer = 450;
    in[SPEC].nweapons = 0;
    in[ZERO].cost_metal = in[ZERO2].cost_metal = 100.0f;
    in[ZERO].maxvelocity_fx = 3 * 65536;
    in[ZERO2].maxvelocity_fx = 65536 / 2;
    in[ZERO].w[0].damage_default = in[ZERO2].w[0].damage_default = 0;
    in[COM].flags245 |= TASIM_F245_COMMANDER;
    in[COM].flags241 = TASIM_F241_BUILDER;
    in[COM].cost_metal = 2500.0f;
    in[COM].maxvelocity_fx = 65536;
    in[COM].nbuild = (uint16_t)(sizeof BUILDS / sizeof BUILDS[0]);
    in[COM].build = BUILDS;
    CHECK(work && tasim_roles_classify(in, N, out, &q, work) == 0, "the classifier takes the army");
    for (i = 0; i < N; i++) {
        k[i] = impai_class_of(&in[i], &out[i]);
        printf("army damage: %-8s role %-9s dps_ground=%.3f class %s\n", NAME[i], tasim_roles_name(out[i].role),
               out[i].dps_ground, impai_class_name(k[i]));
    }
    CHECK(k[SPEC] == IMPAI_K_NONE && k[ZERO] == IMPAI_K_NONE && k[ZERO2] == IMPAI_K_NONE,
          "the types that deal nothing are no class: %s %s %s", impai_class_name(k[SPEC]), impai_class_name(k[ZERO]),
          impai_class_name(k[ZERO2]));
    CHECK((k[FLASH] == IMPAI_K_LRAIDER || k[FLASH] == IMPAI_K_LSCOUT) && k[TANK] == IMPAI_K_LASSAULT &&
              k[RAID] < IMPAI_KS,
          "the armed types keep their classes: %s %s %s", impai_class_name(k[FLASH]), impai_class_name(k[TANK]),
          impai_class_name(k[RAID]));
    free(work);
}

/* THE SUBSTITUTE IS AN ARMY TYPE (impai_mil.c, substitute): a radar contact never identified beside identified
   enemies seen now, the seat's own army three tanks. (a) The enemy commander, a constructor and a raider: the
   raider, though the constructor is the median of the three by value. (b) The enemy commander alone: the seat's
   own median army type, the tank, not the commander. (c) The commander, a raider and an air scout that deals
   nothing, of more value than the raider: the raider -- the enemy's unarmed air scout is never the substitute --,
   and the picture's air seen 0, the scout counted in no class. (d) (c) with the air scout armed: the air scout, and
   its value seen in the air. */
static void check_mil_substitute(void)
{
    IMPAI_MIL_OUT o;
    int k, c;
    for (c = 0; c < 4; c++) {
        mil_world();
        for (k = 0; k < 3; k++) own(1 + k, M_TANK, 24 + 2 * k, 60);
        enemy(1300, M_RAIDER, 300, 20, IMPAI_RADAR, 1);
        s_rows[1300].seat[0].type = (uint16_t)WQ_NONE;
        s_rows[1300].seat[0].owner = 0xFF;
        enemy(1301, T_COM, 300, 120, IMPAI_LOS, 1);
        if (c != 1) {
            enemy(1302, c ? M_SCOUT : T_CONS, 304, 120, IMPAI_LOS, 1);
            enemy(1303, M_RAIDER, 308, 120, IMPAI_LOS, 1);
        }
        if (c >= 2) {
            s_ti[M_SCOUT].klass = IMPAI_K_ASCOUT;
            s_ti[M_SCOUT].flags |= IMPAI_TF_VTOL;
            s_ti[M_SCOUT].value = s_ti[M_RAIDER].value + 100.0f;
            s_threat[M_SCOUT].dps_ground = (uint16_t)(c == 3 ? 10 : 0);
            s_threat[M_SCOUT].dps_air = s_threat[M_SCOUT].dps_water = 0;
        }
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the substitute decision runs");
        printf("substitute, %s: values com %.0f cons %.0f raider %.0f; radar_as %u, air seen %.0f\n",
               c == 3 ? "a commander, an armed air scout and a raider"
               : c == 2 ? "a commander, an unarmed air scout and a raider"
               : c    ? "the commander alone"
                      : "a commander, a constructor and a raider",
               (double)s_ti[T_COM].value, (double)s_ti[T_CONS].value, (double)s_ti[M_RAIDER].value, s_mil.radar_as,
               (double)s_mil.air_seen);
        CHECK(s_mil.radar_as == (c == 1 ? M_TANK : c == 3 ? M_SCOUT : M_RAIDER), "the substitute: %u", s_mil.radar_as);
        if (c == 3) CHECK(s_mil.air_seen > 0.0f, "the armed air scout seen in the air: %.0f", (double)s_mil.air_seen);
        if (c == 2)
            CHECK(s_mil.air_seen == 0.0f, "the unarmed air scout in no class: air seen %.0f", (double)s_mil.air_seen);
    }
    s_ti[M_SCOUT].klass = IMPAI_K_LSCOUT;
    s_ti[M_SCOUT].flags &= (uint8_t)~IMPAI_TF_VTOL;
}

/* THE STAY-OR-GO (impai_mil.c, member_counts) far from the base, where a contact is no base threat, the estimate
   an enemy extractor remembered. Twelve tanks, worth more than wave_value tanks:
   (a) one gunship in reach -- the scout re-dressed as a flyer the tanks have no anti-air slot to hurt, which
       outruns them and deals 300 a shot: no retreat, and the FORM squad's wave goes;
   (b) twenty enemy tanks in reach instead: the squad retreats.
   Then three units, below the wave, so a prediction made is the stay-or-go's alone:
   (c) three tanks and a submarine in reach, which they cannot hurt but outrun: it enters, one prediction;
   (d) three tanks and the gunship: it does not, no prediction;
   (e) three anti-air K-bots, whose slot hurts aircraft, and the gunship: it enters, one prediction. */
static void gunship(void)
{
    s_ft[M_SCOUT].flags |= IMPAI_FT_FLIES;
    s_ft[M_SCOUT].hp = 5000;
    s_fwp[M_SCOUT].dmg = 300;
    s_ft[M_AA].air = 1;
}

/* THE AIR HELD (impai_mil.c): four enemy gunships seen now beside a tank give the picture its gunship share and
   air_seen; the gunships out of sight 300 ticks later, the share and air_seen are held; IMPAI_AIR_HOLD past the
   last sighting they are let go; seen again and out of sight 300 ticks later, a recorded held value that is not a
   number is let go at once. */
static void check_air_held(void)
{
    IMPAI_MIL_OUT o;
    float share[4], seen[4];
    int c, k;
    mil_world();
    gunship();
    s_ti[M_SCOUT].klass = IMPAI_K_GUNSHIP;
    enemy(1300, M_TANK, 60, 60, IMPAI_LOS, 1);
    for (c = 0; c < 4; c++) {
        for (k = 0; k < 4; k++) enemy(1310 + k, M_SCOUT, 50 + 2 * k, 40, c ? IMPAI_GHOST : IMPAI_LOS, !c);
        if (c == 1) s_w.gametime += 300;
        if (c == 2) s_w.gametime += IMPAI_AIR_HOLD + 1;
        if (c == 3) {
            s_w.gametime += 300;
            s_mil.air_held[IMPAI_K_GUNSHIP - IMPAI_K_FIGHTER] = NAN;
        }
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the air held decision runs");
        share[c] = s_mil.pic.f[IMPAI_F_GUNSHIP];
        seen[c] = s_mil.air_seen;
        if (c == 2) {
            for (k = 0; k < 4; k++) enemy(1310 + k, M_SCOUT, 50 + 2 * k, 40, IMPAI_LOS, 1);
            CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the air held decision runs");
        }
    }
    printf("the air held: gunship share seen %.3f, out of sight %.3f, past the hold %.3f, a NaN held %.3f; "
           "air_seen %.0f %.0f %.0f %.0f\n", (double)share[0], (double)share[1], (double)share[2], (double)share[3],
           (double)seen[0], (double)seen[1], (double)seen[2], (double)seen[3]);
    CHECK(share[0] > 0.0f && share[1] == share[0] && share[2] == 0.0f && share[3] == 0.0f,
          "the air held: shares %g %g %g %g", (double)share[0], (double)share[1], (double)share[2], (double)share[3]);
    CHECK(seen[0] == 4.0f * s_ti[M_SCOUT].value && seen[1] == seen[0] && seen[2] == 0.0f && seen[3] == 0.0f,
          "the air held: air_seen %g %g %g %g", (double)seen[0], (double)seen[1], (double)seen[2], (double)seen[3]);
}


/* THE STAY-OR-GO's defences (impai_mil.c, side_contact_counts): check_mil_stay_or_go's twelve tanks, and turrets
   seen in reach (check_mil_goal_side's: armed, range 400, 4 000 hp, 150 a shot), members of the contact the
   squad weighs and armed structures near it both:
   (a) three turrets, each weighed once: no retreat -- weighed as members and again as defences, six, it retreated;
   (b) six turrets: the squad retreats. */
static void check_mil_contact_defences(void)
{
    static const int N[2] = { 3, 6 };
    IMPAI_MIL_OUT o;
    int c, k;
    for (c = 0; c < 2; c++) {
        mil_world();
        s_w.defs[T_TURRET].hp = s_ft[T_TURRET].hp = 4000;
        s_w.defs[T_TURRET].armed = 1;
        s_ft[T_TURRET].value = 300.0f;
        s_ft[T_TURRET].w[0] = T_TURRET;
        s_fwp[T_TURRET].range = 400;
        s_fwp[T_TURRET].reload = 30;
        s_fwp[T_TURRET].burst = 1;
        s_fwp[T_TURRET].dmg = 150;
        s_threat[T_TURRET].dps_ground = s_threat[T_TURRET].hit_ground = 150;
        s_threat[T_TURRET].range = s_threat[T_TURRET].hit_range = 400;
        s_threat[T_TURRET].layer = TASIM_LAYER_LAND;
        for (k = 0; k < 12; k++) own(1 + k, M_TANK, 200 + 2 * (k % 6), 100 + 2 * (k / 6));
        enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
        for (k = 0; k < N[c]; k++) enemy(1300 + k, T_TURRET, 204 + 2 * (k % 4), 110 + 2 * (k / 4), IMPAI_LOS, 1);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the contact's defences decision runs");
        printf("the stay-or-go's defences, %d turrets seen: %u retreats, %u predictions\n", N[c], o.retreats,
               o.predictions);
        CHECK(o.predictions == 1 && o.retreats == (unsigned)c, "%d turrets: %u retreats", N[c], o.retreats);
    }
}

/* the stand-off's world: mil_world, a turret type of reach `reach` -- armed, 500 hp, 10 a shot, so the stay-or-go
   holds --, the tanks' ground reach 600 px, squad 1 an ATTACK of the land layer going to block (200, 50) */
static void stand_world(int reach)
{
    mil_world();
    s_threat[M_TANK].range = s_threat[M_TANK].hit_range = 600;
    s_w.defs[T_TURRET].hp = s_ft[T_TURRET].hp = 500;
    s_w.defs[T_TURRET].armed = 1;
    s_ft[T_TURRET].value = 300.0f;
    s_ft[T_TURRET].w[0] = T_TURRET;
    s_fwp[T_TURRET].range = reach;
    s_fwp[T_TURRET].reload = 30;
    s_fwp[T_TURRET].burst = 1;
    s_fwp[T_TURRET].dmg = 10;
    s_threat[T_TURRET].dps_ground = s_threat[T_TURRET].hit_ground = 10;
    s_threat[T_TURRET].range = s_threat[T_TURRET].hit_range = (uint16_t)reach;
    s_threat[T_TURRET].layer = TASIM_LAYER_LAND;
    enemy(1290, T_MEX, 400, 100, IMPAI_GHOST, 1);
    s_mil.sq[1].task = IMPAI_SQ_ATTACK;
    s_mil.sq[1].layer = TASIM_LAYER_LAND;
    s_mil.sq[1].target = (uint16_t)WQ_NONE;
    s_mil.sq[1].tx = 200;
    s_mil.sq[1].tz = 50;
    s_mil.sq[1].since = (uint32_t)s_w.gametime;
    s_mil.sq[1].counted = 1;
}

/* a tank of squad 1 in slot `slot` at cell (cx, cz) */
static void stand_tank(int slot, int cx, int cz)
{
    own(slot, M_TANK, cx, cz);
    s_job[slot].kind = IMPAI_JOB_SQUAD;
    s_job[slot].type = M_TANK;
    s_job[slot].squad = 1;
}

/* slot 20's orders from log entry `from` on: attacks on slot 1300, ground attacks, moves, the last point */
static void stand_orders(int from, int* att, int* gnd, int* mv, int* mx, int* mz)
{
    int k;
    *att = *gnd = *mv = 0;
    *mx = *mz = -1;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* m = &s_w.log[k].c;
        if (m->unit != 20) continue;
        if (m->action == A_ATTACK && m->target == 1300) (*att)++;
        if (m->action == A_ATTACK && m->target == (uint16_t)WQ_NONE) {
            (*gnd)++;
            *mx = m->pos[0] >> 16;
            *mz = m->pos[2] >> 16;
        }
        if (m->action == A_MOVE) {
            (*mv)++;
            *mx = m->pos[0] >> 16;
            *mz = m->pos[2] >> 16;
        }
    }
}

/* THE STAND-OFF (impai_mil.c, stand_off): stand_world's squad of twelve tanks going east; a turret of reach 400
   south of the squad's first tank (slot 20):
   (a) seen 304 px off, inside its reach + STAND_PAD (464): the tank moves out, to 464 + STAND_HYST = 528 px from it;
   (b) seen 544 px off: the tank attacks it, from where it stands;
   (c) remembered 544 px off, not seen: the tank attacks the ground at its block;
   (d) (c) with that block given up by the land layer: no attack, the tank travels on;
   (e) (c) with the turret's reach 480, which the tank's 600 does not pass by STAND_PAD + STAND_HYST: no attack and
       no move out, the tank travels on as in (d). */
static int task_of_slot(int slot);
static void check_mil_stand_off(void)
{
    static const char* const NAME[5] = { "(a) a turret seen inside its reach", "(b) a turret seen past it",
                                         "(c) a turret remembered", "(d) a turret remembered, given up",
                                         "(e) a turret remembered, not outranged" };
    IMPAI_MIL_OUT o;
    int c, k, from, att, gnd, mv, mx, mz, tx = -1, tz = -1;
    for (c = 0; c < 5; c++) {
        stand_world(c == 4 ? 480 : 400);
        for (k = 0; k < 12; k++) stand_tank(20 + k, 100 + 2 * (k % 6), 100 - 2 * (k / 6));
        enemy(1300, T_TURRET, 100, c ? 134 : 119, c < 2 ? IMPAI_LOS : IMPAI_GHOST, c < 2);
        if (c == 3) {
            s_mil.unreached[0].bx = 50;
            s_mil.unreached[0].bz = 67;
            s_mil.unreached[0].t = (uint32_t)s_w.gametime;
            s_mil.unreached[0].layer = TASIM_LAYER_LAND;
        }
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the stand-off decision runs");
        mil_print(NAME[c], &o, from);
        stand_orders(from, &att, &gnd, &mv, &mx, &mz);
        printf("the stand-off, %s: task %d, attacks %d, ground attacks %d, moves %d, point (%d, %d)\n", NAME[c],
               task_of_slot(20), att, gnd, mv, mx, mz);
        CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK, "%s: the squad still attacks", NAME[c]);
        if (c == 0) {
            const int dz = mz - (s_rows[1300].seat[0].bz * 32 + 16);
            CHECK(mv == 1 && !att && !gnd && mx >= 0 && dz <= -505 && dz >= -555,
                  "%s: moved out to 528 px from it, %d px", NAME[c], -dz);
        }
        if (c == 1) CHECK(att == 1 && !gnd && !mv, "%s: an attack on it", NAME[c]);
        if (c == 2)
            CHECK(gnd == 1 && !att && !mv && mx == s_rows[1300].seat[0].bx * 32 + 16 && mz == s_rows[1300].seat[0].bz * 32 + 16,
                  "%s: a ground attack at its block", NAME[c]);
        if (c >= 3) CHECK(!att && !gnd && mv == 1, "%s: no attack, a move", NAME[c]);
        if (c == 3) {
            tx = mx;
            tz = mz;
        }
        if (c == 4) CHECK(mx == tx && mz == tz, "%s: the travel's move, as (d)'s, not a move out", NAME[c]);
    }
}

static void check_mil_stay_or_go(void)
{
    static const char* const NAME[5] = { "a gunship over the wave", "twenty tanks against the wave",
                                         "a submarine against three tanks", "a gunship over three tanks",
                                         "a gunship over three anti-air K-bots" };
    IMPAI_MIL_OUT o;
    int c, k;
    for (c = 0; c < 5; c++) {
        const int n = c < 2 ? 12 : 3;
        mil_world();
        gunship();
        for (k = 0; k < n; k++) own(1 + k, c == 4 ? M_AA : M_TANK, 200 + 2 * (k % 6), 100 + 2 * (k / 6));
        enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
        if (c == 1)
            for (k = 0; k < 20; k++) enemy(1300 + k, M_TANK, 200 + 2 * (k % 10), 112 + 2 * (k / 10), IMPAI_LOS, 1);
        else
            enemy(1300, c == 2 ? M_SUB : M_SCOUT, 204, 108, IMPAI_LOS, 1);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the stay-or-go decision runs");
        mil_print(NAME[c], &o, s_w.nlog);
        if (c == 0) CHECK(!o.retreats && o.attacks == 1, "%s: no retreat, the wave goes", NAME[c]);
        if (c == 1) CHECK(o.retreats == 1 && !o.attacks, "%s: the squad retreats", NAME[c]);
        if (c >= 2) CHECK(o.predictions == (c == 3 ? 0u : 1u), "%s: %u predictions", NAME[c], o.predictions);
    }
}

/* THE PLANTS AS A WAVE'S GOAL (impai_mil.c, wave_goal): twelve tanks at the base; an outlying structure of 5 000 a
   ghost at cell (450, 40), which pulls the estimate to itself, the enemy's factory a ghost at (100, 200), and the
   enemy commander last seen 6 000 ticks ago, past IMPAI_FOE_FRESH. The wave goes for the factory; with no
   factory known, for the outlying structure. */
static void check_mil_wave_plant(void)
{
    IMPAI_MIL_OUT o;
    int c, k, a;
    for (c = 0; c < 2; c++) {
        mil_world();
        s_w.gametime = 38 + 15 * 400;
        for (k = 0; k < 12; k++) own(1 + k, M_TANK, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
        enemy(1300, T_SOLAR, 450, 40, IMPAI_GHOST, 1);
        s_ti[T_SOLAR].value = 5000.0f;
        if (!c) enemy(1301, T_FACTORY, 100, 200, IMPAI_GHOST, 1);
        s_mil.foe[1].slot = 1302;
        s_mil.foe[1].bx = 200;
        s_mil.foe[1].bz = 100;
        s_mil.foe[1].t = 38;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the wave goal decision runs");
        for (a = -1, k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_ATTACK) a = k;
        printf("wave goal, %s: attacks %u estimate (%d, %d) goal (%d, %d) target %d\n",
               c ? "no factory known" : "a factory known", o.attacks, s_mil.ebx, s_mil.ebz, a >= 0 ? s_mil.sq[a].tx : -1,
               a >= 0 ? s_mil.sq[a].tz : -1, a >= 0 ? s_mil.sq[a].target : -1);
        CHECK(o.attacks == 1 && a >= 0 && s_mil.sq[a].target == (c ? 1300 : 1301), "the wave's target: %d",
              a >= 0 ? s_mil.sq[a].target : -1);
    }
}

/* THE RECALL (impai_mil.c, recall): ten enemy tanks at the base, an ATTACK squad of twelve tanks far off.
   (a) Two raiders at home: their FORM squad defends and loses the prediction; the ATTACK squad comes home as a
       RETREAT of its own, not merged into the defence, the two raiders keep their attack orders (THE HOME
       HOLDS), and the land's wave count of 3 falls to 2 (a recalled ATTACK gives its wave back).
   (b) Twelve tanks at home instead: they win it, and the ATTACK goes on.
   (c) No army at home, two ATTACK squads far off: the empty home is a loss with no prediction, and the nearer
       ATTACK squad comes home, the land's count of 0 held at 0; at the next decision the home is still empty,
       but a land squad is on its way home, so the second ATTACK goes on.
   (d) The same on a plan of react_confirm 2: no recall at the threat's first decision, the recall at its second,
       the confirmation the base's and not a squad's.
   (e) THE WAVE'S COUNT: (a)'s ATTACK a lead's wave, which never counted: recalled, the count stays 3. */
static void recall_attack(int s, int first, int cx)
{
    int k;
    s_mil.sq[s].task = IMPAI_SQ_ATTACK;
    s_mil.sq[s].layer = TASIM_LAYER_LAND;
    s_mil.sq[s].target = (uint16_t)WQ_NONE;
    s_mil.sq[s].tx = 200;
    s_mil.sq[s].tz = 100;
    s_mil.sq[s].since = (uint32_t)s_w.gametime;
    s_mil.sq[s].counted = 1;
    for (k = 0; k < 12; k++) {
        own(first + k, M_TANK, cx + 2 * (k % 6), 150 + 2 * (k / 6));
        s_job[first + k].kind = IMPAI_JOB_SQUAD;
        s_job[first + k].type = M_TANK;
        s_job[first + k].squad = (uint8_t)s;
    }
}

static int task_of_slot(int slot) { return s_job[slot].squad < IMPAI_SQUADS ? s_mil.sq[s_job[slot].squad].task : -1; }

static void check_mil_recall(void)
{
    IMPAI_MIL_OUT o;
    int c, k, from;
    for (c = 0; c < 2; c++) {
        mil_world();
        enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
        for (k = 0; k < (c ? 12 : 2); k++) own(1 + k, c ? M_TANK : M_RAIDER, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
        recall_attack(1, 20, 300);
        s_mil.waves[TASIM_LAYER_LAND] = 3;
        for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 30 + 2 * (k % 5), 34 + 2 * (k / 5), IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the recall decision runs");
        mil_print(c ? "the recall, a home that holds" : "the recall", &o, from);
        if (!c) {
            CHECK(task_of_slot(20) == IMPAI_SQ_RETREAT && task_of_slot(31) == IMPAI_SQ_RETREAT &&
                      task_of_slot(1) == IMPAI_SQ_DEFEND && s_job[1].squad != s_job[20].squad && o.defends == 2,
                  "the ATTACK squad recalled as a RETREAT of its own: tasks %d %d, defends %u", task_of_slot(20),
                  task_of_slot(1), o.defends);
            CHECK(count_action(from, A_ATTACK, 1) == 1 && count_action(from, A_ATTACK, 2) == 1,
                  "the home defenders keep their attack orders");
            CHECK(s_mil.waves[TASIM_LAYER_LAND] == 2 && !s_mil.sq[s_job[20].squad].counted,
                  "the recalled ATTACK gives its wave back, its RETREAT counting none: count %u",
                  s_mil.waves[TASIM_LAYER_LAND]);
        } else
            CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK && s_mil.waves[TASIM_LAYER_LAND] == 3,
                  "the ATTACK goes on: task %d", task_of_slot(20));
    }
    mil_world();
    enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
    for (k = 0; k < 2; k++) own(1 + k, M_RAIDER, 24 + 2 * k, 24);
    recall_attack(1, 20, 300);
    s_mil.sq[1].counted = 0;
    s_mil.waves[TASIM_LAYER_LAND] = 3;
    for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 30 + 2 * (k % 5), 34 + 2 * (k / 5), IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(e) the uncounted recall's decision runs");
    printf("mil the recall of a wave that never counted: task %d count %u\n", task_of_slot(20),
           s_mil.waves[TASIM_LAYER_LAND]);
    CHECK(task_of_slot(20) == IMPAI_SQ_RETREAT && s_mil.waves[TASIM_LAYER_LAND] == 3,
          "(e) a recalled ATTACK that never counted gives nothing back");
    for (c = 0; c < 2; c++) {
        IMPAI_PLAN plan = IMPAI_PLAN_ROW;
        int d;
        plan.react_confirm = (uint8_t)(c + 1);
        mil_world();
        enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
        recall_attack(1, 20, 300);
        recall_attack(2, 40, 340);
        for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 30 + 2 * (k % 5), 34 + 2 * (k / 5), IMPAI_LOS, 1);
        for (d = 0; d < 1 + c; d++) {
            from = s_w.nlog;
            CHECK(mil_decide(&plan, 1, &o) == 0, "the empty home decision runs");
            printf("mil the recall, an empty home, react_confirm %d, decision %d: tasks %d %d defends %u "
                   "predictions %u\n", c + 1, d + 1, task_of_slot(20), task_of_slot(40), o.defends, o.predictions);
            if (d < c)
                CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK && task_of_slot(40) == IMPAI_SQ_ATTACK && !o.defends &&
                          !s_mil.lost_t,
                      "no recall before react_confirm decisions of the threat, no prediction lost");
            else
                CHECK(s_mil.lost_t == (uint32_t)s_w.gametime, "the home lost: a prediction lost now (THE LEAD)");
            s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
        }
        CHECK(task_of_slot(20) == IMPAI_SQ_RETREAT && task_of_slot(40) == IMPAI_SQ_ATTACK && o.defends == 1 &&
                  !s_mil.waves[TASIM_LAYER_LAND],
              "the empty home recalls the nearer ATTACK squad: tasks %d %d, count %u", task_of_slot(20),
              task_of_slot(40), s_mil.waves[TASIM_LAYER_LAND]);
        from = s_w.nlog;
        CHECK(mil_decide(&plan, 1, &o) == 0, "the decision after the recall runs");
        printf("mil the recall, an empty home, react_confirm %d, after: tasks %d %d defends %u predictions %u\n", c + 1,
               task_of_slot(20), task_of_slot(40), o.defends, o.predictions);
        CHECK(task_of_slot(20) == IMPAI_SQ_RETREAT && task_of_slot(40) == IMPAI_SQ_ATTACK && !o.defends,
              "a land squad on its way home: no second recall");
    }
}

/* squad s given `task` on `layer`, its goal block (200, 100), and n units of `type` from slot `first` at cells
   (cx, cz) on, in rows of six */
static void recall_squad(int s, unsigned task_, unsigned layer, int type, int first, int n, int cx, int cz)
{
    int k;
    squad_task(s, task_, layer, 200, 100);
    for (k = 0; k < n; k++) member(first + k, type, cx + 2 * (k % 6), cz + 2 * (k / 6), s);
}

/* the seat's towers: T_TURRET armed in the threat table and the fight content, 2 000 HP, 40 a shot at 400 px */
static void tower_def(void)
{
    mil_def(T_TURRET, "TOWER", 0, 2000, 0, 400, 40);
    s_ft[T_TURRET].flags = 0;
}

/* THE HOME (impai_mil.c, home_side) and THE RECALL's groups, an ATTACK squad far off in each case (cell 300 and
   on, 4 800 px from the anchor), the enemy's extractor a ghost at (400, 200):
   (a) THE WHOLE HOME: twelve tanks at home against three enemy tanks in the base, an air wave of six gunships and
       a sea wave of six destroyers out, both able to hurt the tanks. Neither is recalled, on one prediction: the
       two layers' groups are the same contact.
   (b) THE TOWERS AND THE COMMANDER, no army at home and an armed scout in the base: three towers hold it, and so
       does the commander, which stands; (c) the commander in its flight window is not the home: the land wave
       comes back.
   (d) A WIDE RAID, the anchor at the map's middle: nine contacts of one tank, eight 1 700 px round the anchor and
       one 480 px from the commander, sixteen tanks and the commander at home, a land and an air wave out. The
       home holds on one prediction, nothing is recalled, and the commander keeps its own prediction and stands;
       on a plan of predict_max 1 the commander's safety still has its prediction, the recall coming after it.
   (e) TWO PRONGS: six tanks at home, four enemy tanks 1 600 px east and four 1 600 px west. The home beats each,
       not both: the land wave comes back.
   (f) THE SQUADS' RESERVE, a plan of predict_max 2: two land waves each in a field fight it loses, two tanks
       against six, and a raid of four tanks on a home of three. The recall makes no prediction, and both waves
       make their retreat tests and retreat.
   (g) AN AIR RAID: three tanks at home under a gunship, a land wave of tanks and a land wave of anti-air K-bots
       out. The anti-air wave comes back and the tanks' wave goes on, and the home losing presses the economy,
       though its tanks outweigh the gunship.
   (h) A SQUAD OUT OF THE BOX IS NO HOME: twelve tanks of no squad 4 000 px out, which the defence makes DEFEND, and
       ten enemy tanks at the base: the wave comes back.
   (i) THE COMMANDER FIGHTS FOR NOTHING OF ITS OWN: two towers 1 000 px east of the anchor and the commander at
       it, worth about 13 x the two (2 917 against 2 x 115), so tough it outlives the fight (1 000 000 HP) and so
       weak it kills little (one HP a tick), against six tanks beside the towers, 1 100 px from the commander, which
       kill both towers and stay. The commander stays at home -- no contact within its safe distance --, and the
       land wave comes back: the home keeps nothing and the raid most of itself. With the commander's value in the
       margin, the home read as a win. */
static void check_mil_recall_home(void)
{
    IMPAI_MIL_OUT o;
    int c, k;
    /* (a) */
    mil_world();
    gunship();
    s_ti[M_SCOUT].klass = IMPAI_K_GUNSHIP;
    s_ti[M_DEST].klass = IMPAI_K_SASSAULT;
    enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
    for (k = 0; k < 12; k++) own(1 + k, M_TANK, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
    recall_squad(1, IMPAI_SQ_ATTACK, TASIM_LAYER_AIR, M_SCOUT, 20, 6, 300, 150);
    recall_squad(2, IMPAI_SQ_ATTACK, TASIM_LAYER_SEA, M_DEST, 40, 6, 340, 150);
    for (k = 0; k < 3; k++) enemy(1300 + k, M_TANK, 30 + 2 * k, 34, IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the whole home decision runs");
    mil_print("the recall, the whole home", &o, s_w.nlog);
    CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK && task_of_slot(40) == IMPAI_SQ_ATTACK && o.predictions == 1,
          "(a) the land army holds the base: the air and sea waves go on, tasks %d %d, predictions %u",
          task_of_slot(20), task_of_slot(40), o.predictions);
    /* (b), (c) */
    for (c = 0; c < 3; c++) {
        static const char* const NAME[3] = { "three towers", "the commander", "the commander in flight" };
        mil_world();
        enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
        tower_def();
        if (!c)
            for (k = 0; k < 3; k++) own(60 + k, T_TURRET, 22 + 4 * k, 30);
        else
            own(60, T_COM, 24, 24);
        if (c == 2) s_mil.safe_until = (uint32_t)s_w.gametime + 100;
        recall_attack(1, 20, 300);
        enemy(1300, M_SCOUT, 30, 34, IMPAI_LOS, 1);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the towers decision runs");
        printf("mil the recall, %s against a scout: task %d predictions %u safe %u\n", NAME[c], task_of_slot(20),
               o.predictions, o.safe);
        if (c < 2)
            CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK && !o.safe, "(b) %s hold the base: task %d", NAME[c],
                  task_of_slot(20));
        else
            CHECK(task_of_slot(20) == IMPAI_SQ_RETREAT, "(c) %s is no home: task %d", NAME[c], task_of_slot(20));
    }
    /* (d), (e) */
    for (c = 0; c < 3; c++) {
        IMPAI_PLAN plan = IMPAI_PLAN_ROW;
        static const int RX[8] = { 106, 75, 0, -75, -106, -75, 0, 75 }, RZ[8] = { 0, 75, 106, 75, 0, -75, -106, -75 };
        mil_world();
        gunship();
        s_ti[M_SCOUT].klass = IMPAI_K_GUNSHIP;
        s_st.ax = s_st.az = 128;
        enemy(1290, T_MEX, 480, 240, IMPAI_GHOST, 1);
        recall_squad(1, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, M_TANK, 200, 6, 440, 20);
        if (c == 2) plan.predict_max = 1;
        if (c != 1) {
            recall_squad(2, IMPAI_SQ_ATTACK, TASIM_LAYER_AIR, M_SCOUT, 220, 6, 470, 20);
            own(60, T_COM, 128, 128);
            for (k = 0; k < 16; k++) own(1 + k, M_TANK, 122 + 2 * (k % 6), 124 + 2 * (k / 6));
            for (k = 0; k < 8; k++) enemy(1300 + k, M_TANK, 128 + RX[k], 128 + RZ[k], IMPAI_LOS, 1);
            enemy(1310, M_TANK, 158, 128, IMPAI_LOS, 1);
        } else {
            for (k = 0; k < 6; k++) own(1 + k, M_TANK, 124 + 2 * (k % 3), 126 + 2 * (k / 3));
            for (k = 0; k < 4; k++) {
                enemy(1300 + k, M_TANK, 228 + 2 * (k % 2), 126 + 2 * (k / 2), IMPAI_LOS, 1);
                enemy(1310 + k, M_TANK, 26 + 2 * (k % 2), 126 + 2 * (k / 2), IMPAI_LOS, 1);
            }
        }
        CHECK(mil_decide(&plan, 1, &o) == 0, "the wide raid decision runs");
        mil_print(c == 1 ? "the recall, two prongs" : c ? "the recall, a wide raid, predict_max 1" : "the recall, a wide raid",
                  &o, s_w.nlog);
        if (c == 2)
            CHECK(o.predictions == 1 && !o.safe, "(d) predict_max 1: the commander's prediction, safe %u", o.safe);
        else if (!c)
            CHECK(o.threats == 9 && task_of_slot(200) == IMPAI_SQ_ATTACK && task_of_slot(220) == IMPAI_SQ_ATTACK &&
                      o.predictions == 2 && !o.safe,
                  "(d) a wide raid: threats %u, tasks %d %d, predictions %u, safe %u", o.threats, task_of_slot(200),
                  task_of_slot(220), o.predictions, o.safe);
        else
            CHECK(o.threats == 2 && task_of_slot(200) == IMPAI_SQ_RETREAT,
                  "(e) two prongs the home beats one at a time: threats %u, task %d", o.threats, task_of_slot(200));
    }
    /* (f) */
    {
        IMPAI_PLAN plan = IMPAI_PLAN_ROW;
        plan.predict_max = 2;
        mil_world();
        enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
        for (k = 0; k < 3; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
        for (k = 0; k < 4; k++) enemy(1300 + k, M_TANK, 30 + 2 * k, 34, IMPAI_LOS, 1);
        recall_squad(1, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, M_TANK, 20, 2, 300, 60);
        recall_squad(2, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, M_TANK, 40, 2, 300, 200);
        for (k = 0; k < 6; k++) {
            enemy(1310 + k, M_TANK, 304 + 2 * (k % 3), 70 + 2 * (k / 3), IMPAI_LOS, 1);
            enemy(1320 + k, M_TANK, 304 + 2 * (k % 3), 210 + 2 * (k / 3), IMPAI_LOS, 1);
        }
        CHECK(mil_decide(&plan, 1, &o) == 0, "the reserve decision runs");
        mil_print("the recall, the squads' reserve", &o, s_w.nlog);
        CHECK(o.retreats == 2 && o.defends == 1 && o.predictions == 2,
              "(f) the squads keep their tests: retreats %u, defends %u, predictions %u", o.retreats, o.defends,
              o.predictions);
    }
    /* (g) */
    mil_world();
    gunship();
    s_ti[M_SCOUT].klass = IMPAI_K_GUNSHIP;
    enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
    for (k = 0; k < 3; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    recall_squad(1, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, M_TANK, 20, 6, 300, 100);
    recall_squad(2, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, M_AA, 40, 6, 300, 200);
    enemy(1300, M_SCOUT, 30, 34, IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the air raid decision runs");
    mil_print("the recall, an air raid", &o, s_w.nlog);
    CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK && task_of_slot(40) == IMPAI_SQ_RETREAT && o.press == 1,
          "(g) the anti-air wave comes back, and the economy is pressed: tasks %d %d, press %u", task_of_slot(20),
          task_of_slot(40), o.press);
    /* (h) */
    mil_world();
    enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
    for (k = 0; k < 12; k++) own(1 + k, M_TANK, 270 + 2 * (k % 6), 20 + 2 * (k / 6));
    recall_attack(1, 20, 300);
    for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 30 + 2 * (k % 5), 34 + 2 * (k / 5), IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the far squad decision runs");
    mil_print("the recall, a squad out of the box", &o, s_w.nlog);
    CHECK(task_of_slot(1) == IMPAI_SQ_DEFEND && task_of_slot(20) == IMPAI_SQ_RETREAT,
          "(h) a squad out of the box is no home: tasks %d %d", task_of_slot(1), task_of_slot(20));
    /* (i) */
    mil_world();
    enemy(1290, T_MEX, 400, 200, IMPAI_GHOST, 1);
    tower_def();
    s_w.defs[T_COM].hp = s_ft[T_COM].hp = 1000000;
    s_fwp[T_COM].dmg = 30;
    for (k = 0; k < 2; k++) own(60 + k, T_TURRET, 80 + 4 * k, 20);
    own(62, T_COM, 20, 20);
    recall_attack(1, 20, 300);
    for (k = 0; k < 6; k++) enemy(1300 + k, M_TANK, 88 + 2 * (k % 3), 26 + 2 * (k / 3), IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the commander's raid decision runs");
    mil_print("the recall, towers lost and the commander alive", &o, s_w.nlog);
    CHECK(task_of_slot(20) == IMPAI_SQ_RETREAT && !o.safe,
          "(i) the commander stands and the land wave comes back: task %d, safe %u", task_of_slot(20), o.safe);
}

/* THE GROUND DEFENCES (impai_econ.c) on the governor's harness, the world's spare army types re-dressed as
   buildings of category OTHER, 2 x 2 cells: a light laser (reach 250, value 100), a missile tower (reach 400,
   value 80, an anti-air structure that also reaches ground), an anti-air gun (no ground reach, value 50) and a
   heavy laser (reach 400, value 400). The pair rate is the table below, HP a tick. The constructor's menu holds
   them beside the base's types; the commander's holds none. A seat pressed at the base by a raider (reach 200)
   at block (70, 10), 1 600 px east of the anchor. */
#define D_LLT M_SCOUT
#define D_RL  M_SUB
#define D_AA  M_AA
#define D_HLT M_DEST
static float s_drate[M_N][M_N];
static float rate_def(const void* ctx, uint16_t i, uint16_t j)
{
    (void)ctx;
    return i < M_N && j < M_N ? s_drate[i][j] : 0.0f;
}

static void def_tower(int t, const char* name, float cost, uint16_t hit, uint16_t reach, uint16_t air)
{
    def_set(&s_w, t, name, 2, 2, 0, 0, cost, 0);
    s_ti[t] = s_ti[T_TURRET];
    s_ti[t].value = cost;
    s_ti[t].klass = IMPAI_K_NONE;
    memset(&s_threat[t], 0, sizeof s_threat[t]);
    s_threat[t].dps_ground = s_threat[t].hit_ground = hit;
    s_threat[t].dps_air = air;
    s_threat[t].range = s_threat[t].hit_range = reach;
    s_threat[t].layer = TASIM_LAYER_LAND;
}

static void def_world(uint16_t enemy_type, float press_v)
{
    static const uint16_t MENU[] = { T_MEX, T_SOLAR, T_FACTORY, D_LLT, D_AA, D_RL, D_HLT };
    gov_world(1);
    s_w.econ[0].metal_in = 5.0f;
    def_tower(D_LLT, "LLT", 100, 20, 250, 0);
    def_tower(D_RL, "RL", 80, 6, 400, 60);
    def_tower(D_AA, "AAGUN", 50, 0, 600, 90);
    def_tower(D_HLT, "HLT", 400, 40, 400, 0);
    CHECK(impai_def_struct(&(IMPAI_BRAIN_IN){ .ti = s_ti, .nti = M_N,
                                              .view = &(IMPAI_VIEW){ .threat = s_threat, .ntypes = M_N } },
                           D_RL) &&
              !impai_def_struct(&(IMPAI_BRAIN_IN){ .ti = s_ti, .nti = M_N,
                                                   .view = &(IMPAI_VIEW){ .threat = s_threat, .ntypes = M_N } },
                                D_AA),
          "the missile tower is a ground defence, the anti-air gun is not");
    menu_set(&s_w, T_CONS, MENU, (int)(sizeof MENU / sizeof MENU[0]));
    own(10, T_CONS, 24, 20);
    memset(s_drate, 0, sizeof s_drate);
    s_drate[D_LLT][M_RAIDER] = s_drate[D_LLT][M_TANK] = 20.0f;
    s_drate[D_RL][M_RAIDER] = s_drate[D_RL][M_TANK] = 6.0f;
    s_drate[D_AA][M_RAIDER] = s_drate[D_AA][M_TANK] = 100.0f;
    s_drate[D_HLT][M_RAIDER] = s_drate[D_HLT][M_TANK] = 40.0f;
    s_mil.pic.ne = 1;
    s_mil.pic.e[0].type = enemy_type;
    s_mil.pic.e[0].share = 1.0f;
    s_mil.press_until = (uint32_t)s_w.gametime + 100u;
    s_mil.press_v = press_v;
    s_mil.press_bx = 70;
    s_mil.press_bz = 10;
    s_mil.press_base = 1;
}

/* one decision with the view of the threat rows: the tower types ordered built, at most 4, into tw[] with their
   centres; their count */
static int def_decide(int with_pred, uint16_t* tw, int32_t (*at)[2])
{
    static const IMPAI_PREDICTOR pred = { rate_def, impai_expf, NULL };
    IMPAI_BRAIN_IN in;
    IMPAI_BRAIN_OUT bo;
    IMPAI_VIEW v;
    WORLDQ q = { &FAKE, &s_w, 0 };
    const int from = s_w.nlog;
    int k, n = 0;
    memset(&v, 0, sizeof v);
    v.seat = 0;
    v.rows = s_rows;
    v.nrows = FW_SLOTS;
    v.threat = s_threat;
    v.ntypes = M_N;
    v.w = s_w.mw;
    v.h = s_w.mh;
    memset(&in, 0, sizeof in);
    in.q = &q;
    in.st = &s_st;
    in.pref = s_pref;
    in.pref_stride = 1;
    in.job = s_job; in.njob = FW_SLOTS;
    in.ti = s_ti; in.nti = M_N;
    in.spots = s_spots; in.nspots = s_nspots;
    in.scanned = s_scanned;
    in.reach = &s_reach;
    in.plan = &s_glv;
    in.persona = s_persona;
    in.acts = -1;
    in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
    in.snap = &s_snap;
    in.mil = &s_mil;
    in.view = &v;
    in.pred = with_pred ? &pred : NULL;
    CHECK(decide_traced(&in, &bo) == 0, "decide");
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->action != A_MOBILEBUILD || (c->type != D_LLT && c->type != D_RL && c->type != D_AA && c->type != D_HLT))
            continue;
        if (n < 4) {
            tw[n] = c->type;
            at[n][0] = c->pos[0] >> 16;
            at[n][1] = c->pos[2] >> 16;
        }
        n++;
    }
    return n;
}

static const char* def_name(uint16_t t)
{
    return t == D_LLT ? "LLT" : t == D_RL ? "RL" : t == D_AA ? "AAGUN" : t == D_HLT ? "HLT" : "?";
}

/* THE TYPE: against the raider (reach 200) the light laser, 20 / 100 a value, over the heavy laser's 40 / 400
   and the missile tower's 6 / 80; against the tank (reach 300) the heavy laser, the light laser outranged; the
   heavy laser water-only on the dry anchor, the missile tower; the anti-air gun, rate 100, never. Without a
   predictor, no tower. */
static void check_def_choice(void)
{
    uint16_t tw[4];
    int32_t at[4][2];
    int k, n;
    static const struct { uint16_t enemy; int water, pred; uint16_t want; const char* what; } CASE[4] = {
        { M_RAIDER, 0, 1, D_LLT, "the raider" },
        { M_TANK, 0, 1, D_HLT, "the tank, the light laser outranged" },
        { M_TANK, 1, 1, D_RL, "the tank, the heavy laser water-only on a dry anchor" },
        { M_RAIDER, 0, 0, 0, "no predictor" } };
    for (k = 0; k < 4; k++) {
        def_world(CASE[k].enemy, 100.0f);
        if (CASE[k].water) s_ti[D_HLT].flags |= IMPAI_TF_WATER;
        n = def_decide(CASE[k].pred, tw, at);
        printf("defence choice, %s: %d ordered, %s\n", CASE[k].what, n, n ? def_name(tw[0]) : "none");
        CHECK(CASE[k].want ? n == 1 && tw[0] == CASE[k].want : n == 0, "defence choice, %s", CASE[k].what);
    }
    s_milp = NULL;
}

/* THE WANT: pressed at the base with press_v = 2 def_v, one tower a decision until 3 stand (1 + 2), each finished
   at once; with 10 def_v, held at def_max. The ring stands around the factory, east of the anchor; the member
   east, the first tower on the ring's east side; the member west, on its west side. Pressed at an expansion, in an
   energy stall, or not pressed: none. In a metal stall, pressed at the base: the tower. */
static void check_def_want(void)
{
    uint16_t tw[4];
    int32_t at[4][2];
    int k, i, n, built, east_x = 0;
    for (k = 0; k < 2; k++) {
        const float mult = k ? 10.0f : 2.0f;
        const int cap = k ? IMPAI_PLAN_ROW.def_max : 3;
        def_world(M_RAIDER, mult * IMPAI_PLAN_ROW.def_v);
        for (i = built = 0; i < 8; i++) {
            const int from = s_w.nlog;
            int j;
            n = def_decide(1, tw, at);
            CHECK(n <= 1, "one tower a decision: %d", n);
            if (n && !built && !k) east_x = at[0][0];
            built += n;
            for (j = from; j < s_w.nlog; j++) {
                const WQ_CMD* c = &s_w.log[j].c;
                if (c->action == A_MOBILEBUILD) s_w.unit[c->unit].n = 0;
                if (c->action == A_MOBILEBUILD && c->type == tw[0] && n)
                    own(400 + built, c->type, (c->pos[0] >> 16) / 16, (c->pos[2] >> 16) / 16);
            }
            s_w.gametime += 15;
            s_mil.press_until = (uint32_t)s_w.gametime + 100u;
        }
        printf("defence want, press_v %.0f def_v: %d towers\n", (double)mult, built);
        CHECK(built == cap, "defence want, press_v %.0f def_v: %d towers, want %d", (double)mult, built, cap);
    }
    def_world(M_RAIDER, 100.0f);
    s_mil.press_bx = -40;
    n = def_decide(1, tw, at);
    printf("defence want, the member east: the first tower at x %d; west: %d at x %d\n", east_x, n, n ? at[0][0] : -1);
    CHECK(n == 1 && east_x > 480 && at[0][0] < 480, "the tower on the side of the pressing member");
    for (k = 0; k < 3; k++) {
        static const char* const WHAT[3] = { "pressed at an expansion", "an energy stall", "not pressed" };
        def_world(M_RAIDER, 100.0f);
        if (k == 0) s_mil.press_base = 0;
        if (k == 1) {
            s_w.econ[0].energy = 100;
            s_w.econ[0].energy_in = 10;
            s_w.econ[0].energy_out = 30;
        }
        if (k == 2) s_mil.press_until = 0;
        n = def_decide(1, tw, at);
        printf("defence want, %s: %d\n", WHAT[k], n);
        CHECK(n == 0, "defence want, %s: none", WHAT[k]);
    }
    def_world(M_RAIDER, 100.0f);
    s_w.econ[0].metal = 100;
    s_w.econ[0].metal_max = 1000;
    s_w.econ[0].metal_out = 30;
    n = def_decide(1, tw, at);
    printf("defence want, a metal stall: %d, %s\n", n, n ? def_name(tw[0]) : "none");
    CHECK(n == 1 && tw[0] == D_LLT, "defence want, a metal stall: the tower");
    s_milp = NULL;
}

/* THE SENSORS (impai_econ.c). Three sensor types on the builders' menus: RAD, radar 800 px for 50, 20 energy a pass;
   RADB, radar 1600 for 150, 90 energy, past the flows' 80 unless a case lowers it; SONAR, sonar 1200 for 60,
   water-only. The anchor is the commander's cell (20, 20), two factories finished at (30, 30) and (40, 30) --
   factory_max --, two constructors beside the anchor, the energy flows 100 in and 20 out, the map explored. */
#define S_RAD  M_SUB
#define S_RADB M_DEST
#define S_SON  M_AA

static void sensor_type(int t, const char* name, float cost, uint16_t radar, uint16_t sonar, float upkeep, int water)
{
    def_set(&s_w, t, name, 2, 2, 0, 0, cost, 0);
    s_w.defs[t].e.energyuse = upkeep;
    s_w.defs[t].e.radar = (int16_t)radar;
    s_w.defs[t].e.sonar = (int16_t)sonar;
    s_ti[t] = s_ti[T_TURRET];
    s_ti[t].cat = IMPAI_C_OTHER;
    s_ti[t].flags = (uint8_t)(water ? IMPAI_TF_WATER : 0u);
    s_ti[t].value = cost;
    s_ti[t].klass = IMPAI_K_NONE;
    s_ti[t].radar = radar;
    s_ti[t].sonar = sonar;
    memset(&s_threat[t], 0, sizeof s_threat[t]);
}

static void sensor_world(void)
{
    static const uint16_t MENU[] = { T_MEX, T_SOLAR, T_FACTORY, S_RAD, S_RADB, S_SON };
    int x, z;
    gov_world(2);
    s_w.econ[0].metal = 800.0f;
    s_w.econ[0].metal_max = 1000.0f;
    s_w.econ[0].metal_in = 5.0f;
    s_w.econ[0].energy = 800.0f;
    s_w.econ[0].energy_max = 1000.0f;
    s_w.econ[0].energy_in = 100.0f;
    s_w.econ[0].energy_out = 20.0f;
    sensor_type(S_RAD, "RAD", 50.0f, 800, 0, 20.0f, 0);
    sensor_type(S_RADB, "RADB", 150.0f, 1600, 0, 90.0f, 0);
    sensor_type(S_SON, "SONAR", 60.0f, 0, 1200, 20.0f, 1);
    menu_set(&s_w, T_CONS, MENU, (int)(sizeof MENU / sizeof MENU[0]));
    menu_set(&s_w, T_COM, MENU, (int)(sizeof MENU / sizeof MENU[0]));
    own(10, T_CONS, 24, 20);
    own(11, T_CONS, 24, 24);
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    s_mil.ebx = s_mil.ebz = IMPAI_NO_ANCHOR;
    s_reach.sea_blocks = 0;
}

/* one decision: the sensors ordered, their count; the first one's type and footprint centre, px, and the unit
   given it */
static int s_sensor_unit;
static int sensor_decide(uint16_t* type, int32_t* x, int32_t* z)
{
    IMPAI_BRAIN_IN in;
    IMPAI_BRAIN_OUT bo;
    IMPAI_VIEW v;
    WORLDQ q = { &FAKE, &s_w, 0 };
    const int from = s_w.nlog;
    int k, n = 0;
    memset(&v, 0, sizeof v);
    v.seat = 0;
    v.rows = s_rows;
    v.nrows = FW_SLOTS;
    v.threat = s_threat;
    v.ntypes = M_N;
    v.w = s_w.mw;
    v.h = s_w.mh;
    memset(&in, 0, sizeof in);
    in.q = &q;
    in.st = &s_st;
    in.pref = s_pref;
    in.pref_stride = 1;
    in.job = s_job; in.njob = FW_SLOTS;
    in.ti = s_ti; in.nti = M_N;
    in.spots = s_spots; in.nspots = s_nspots;
    in.scanned = s_scanned;
    in.reach = &s_reach;
    in.plan = &s_glv;
    in.persona = s_persona;
    in.acts = -1;
    in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
    in.snap = &s_snap;
    in.mil = &s_mil;
    in.view = &v;
    CHECK(decide_traced(&in, &bo) == 0, "decide");
    *type = 0;
    *x = *z = -1;
    s_sensor_unit = -1;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        if (c->action != A_MOBILEBUILD || (c->type != S_RAD && c->type != S_RADB && c->type != S_SON)) continue;
        if (!n++) {
            *type = c->type;
            *x = c->pos[0] >> 16;
            *z = c->pos[2] >> 16;
            s_sensor_unit = c->unit;
        }
    }
    CHECK(bo.sensors == (unsigned)n, "the decision's sensors= counts its orders: %u against %d", bo.sensors, n);
    return n;
}

static int64_t sq_px(int32_t x, int32_t z, int32_t px, int32_t pz)
{
    return ((int64_t)x - px) * ((int64_t)x - px) + ((int64_t)z - pz) * ((int64_t)z - pz);
}

static const char* sensor_name(uint16_t t)
{
    return t == S_RAD ? "RAD" : t == S_RADB ? "RADB" : t == S_SON ? "SONAR" : "none";
}

/* WHICH TYPES ARE SENSORS: the radar and the sonar are; a tower with a radar beside its gun, a mobile unit and a
   plant with a radar are not */
static void check_sensor_struct(void)
{
    const IMPAI_BRAIN_IN in = { .ti = s_ti, .nti = M_N, .view = &(IMPAI_VIEW){ .threat = s_threat, .ntypes = M_N } };
    IMPAI_TINFO keep[3];
    uint16_t hit;
    sensor_world();
    keep[0] = s_ti[T_TURRET];
    keep[1] = s_ti[M_RAIDER];
    keep[2] = s_ti[T_FACTORY];
    hit = s_threat[T_TURRET].hit_ground;
    s_ti[T_TURRET].radar = s_ti[M_RAIDER].radar = s_ti[T_FACTORY].radar = 900;
    s_threat[T_TURRET].hit_ground = 10;
    printf("sensor types: RAD %d, SONAR %d, the tower %d, the raider %d, the plant %d\n",
           impai_sensor_struct(&in, S_RAD), impai_sensor_struct(&in, S_SON), impai_sensor_struct(&in, T_TURRET),
           impai_sensor_struct(&in, M_RAIDER), impai_sensor_struct(&in, T_FACTORY));
    CHECK(impai_sensor_struct(&in, S_RAD) && impai_sensor_struct(&in, S_SON), "the radar and the sonar are sensors");
    CHECK(!impai_sensor_struct(&in, T_TURRET), "a tower with a radar is a defence");
    CHECK(!impai_sensor_struct(&in, M_RAIDER), "a mobile unit with a radar is no sensor structure");
    CHECK(!impai_sensor_struct(&in, T_FACTORY), "a plant with a radar is a plant");
    s_ti[T_TURRET] = keep[0];
    s_ti[M_RAIDER] = keep[1];
    s_ti[T_FACTORY] = keep[2];
    s_threat[T_TURRET].hit_ground = hit;
    s_milp = NULL;
}

/* THE TYPE: RADB, 1600^2 / 150, over RAD, 800^2 / 50 -- RAD by distance per value --, with both upkeeps within the
   flows' 80; RADB's past them, RAD; both past them, none */
static void check_sensor_type(void)
{
    static const struct { float rad, radb; uint16_t want; const char* what; } CASE[3] = {
        { 20.0f, 50.0f, S_RADB, "the most coverage per value" },
        { 20.0f, 90.0f, S_RAD, "RADB's upkeep past the flows" },
        { 90.0f, 90.0f, 0, "every upkeep past the flows" } };
    uint16_t t;
    int32_t x, z;
    int k, n;
    for (k = 0; k < 3; k++) {
        sensor_world();
        s_w.defs[S_RAD].e.energyuse = CASE[k].rad;
        s_w.defs[S_RADB].e.energyuse = CASE[k].radb;
        n = sensor_decide(&t, &x, &z);
        printf("sensor type, %s: %d ordered, %s\n", CASE[k].what, n, sensor_name(t));
        CHECK(CASE[k].want ? n == 1 && t == CASE[k].want : n == 0, "sensor type, %s", CASE[k].what);
    }
    s_milp = NULL;
}

/* THE WANT, each a radar whose centre lies within 600 px (three quarters of RAD's 800) of its point, one a decision:
   with nothing standing, the anchor; with a radar at the anchor, the nearer of two finished extractors 650 and
   1290 px out; the far one alone, on a ring around it past the base ring's reach, by a constructor though the
   commander is asked first; with the anchor covered and no extractor, the front half way to an estimate 1490 px
   east; for a map with sea, the anchor for sonar, which the water-only sonar on a dry anchor cannot take and a land
   one can; the anchor covered by a plant with a radar, none; the radar standing, none, and lost, wanted again --
   each builder's orders done between the decisions.
   Held back: four sensors standing (sensor_max), an energy stall -- with a radar of no upkeep and no energy type
   on any menu, so neither the upkeep nor the stall's energy rung holds it back --, one of the two factories
   unfinished. */
static void check_sensor_want(void)
{
    const int32_t ax = 20 * 16 + 8, az = 20 * 16 + 8;
    uint16_t t;
    int32_t x, z, ex, ez, fx;
    int n, k;
    /* the anchor */
    sensor_world();
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, nothing standing: %d %s at %d,%d (anchor %d,%d)\n", n, sensor_name(t), x, z, ax, az);
    CHECK(n == 1 && t == S_RAD && sq_px(x, z, ax, az) <= 600 * 600, "one radar covering the anchor");
    /* the nearer extractor */
    sensor_world();
    own(30, S_RAD, 22, 20);
    own(31, T_MEX, 100, 20);
    own(32, T_MEX, 60, 20);
    ex = 60 * 16 + 16;
    ez = 20 * 16 + 16;
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, extractors 650 and 1290 px out: %d %s at %d,%d (the nearer at %d,%d)\n", n, sensor_name(t),
           x, z, ex, ez);
    CHECK(n == 1 && sq_px(x, z, ex, ez) <= 600 * 600, "one radar covering the nearer extractor");
    /* the far extractor alone, on its own ring */
    sensor_world();
    own(30, S_RAD, 22, 20);
    own(31, T_MEX, 100, 20);
    ex = 100 * 16 + 16;
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, one extractor 1290 px out: %d %s at %d,%d by unit %d (it at %d,%d)\n", n, sensor_name(t), x,
           z, s_sensor_unit, ex, ez);
    CHECK(n == 1 && sq_px(x, z, ex, ez) <= 600 * 600 && s_sensor_unit != 1,
          "a radar on the far extractor's own ring, by a constructor");
    /* the front */
    sensor_world();
    own(30, S_RAD, 22, 20);
    s_mil.ebx = (int16_t)(1800 / 32);
    s_mil.ebz = (int16_t)(az / 32);
    fx = ax + (s_mil.ebx * 32 + 16 - ax) / 2;
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, the front: %d %s at %d,%d (the front at %d,%d)\n", n, sensor_name(t), x, z, fx, az);
    CHECK(n == 1 && sq_px(x, z, fx, az) <= 600 * 600, "a radar covering the front");
    /* the sonar */
    sensor_world();
    own(30, S_RAD, 22, 20);
    s_reach.sea_blocks = 1;
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, a map with sea, the sonar water-only on a dry anchor: %d\n", n);
    CHECK(n == 0, "no water-only sonar on a dry anchor");
    sensor_world();
    own(30, S_RAD, 22, 20);
    s_reach.sea_blocks = 1;
    s_ti[S_SON].flags = 0;
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, a map with sea, a land sonar: %d %s at %d,%d\n", n, sensor_name(t), x, z);
    CHECK(n == 1 && t == S_SON && sq_px(x, z, ax, az) <= 900 * 900, "a sonar covering the anchor");
    s_reach.sea_blocks = 0;
    /* the plant's radar */
    sensor_world();
    s_ti[T_FACTORY].radar = 900;
    n = sensor_decide(&t, &x, &z);
    s_ti[T_FACTORY].radar = 0;
    printf("sensor want, the anchor covered by the plant's radar: %d\n", n);
    CHECK(n == 0, "a plant's radar covers the anchor");
    /* standing, then lost */
    sensor_world();
    n = sensor_decide(&t, &x, &z);
    CHECK(n == 1 && s_sensor_unit > 0, "the first radar");
    own(30, t, (x - 16) / 16, (z - 16) / 16);
    for (k = 1; k < s_w.top; k++) s_w.unit[k].n = 0;
    s_w.gametime += 15;
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, the anchor covered: %d\n", n);
    CHECK(n == 0, "nothing more to cover");
    s_w.unit[30].live = 0;
    for (k = 1; k < s_w.top; k++) s_w.unit[k].n = 0;
    s_w.gametime += 15;
    n = sensor_decide(&t, &x, &z);
    printf("sensor want, the radar lost: %d %s\n", n, sensor_name(t));
    CHECK(n == 1 && t == S_RAD, "the radar lost is wanted again");
    /* held back */
    for (k = 0; k < 3; k++) {
        static const char* const WHAT[3] = { "sensor_max standing", "an energy stall", "one factory unfinished" };
        sensor_world();
        if (k == 0) {
            own(30, S_RAD, 120, 120);
            own(31, S_RAD, 120, 100);
            own(32, S_RAD, 100, 120);
            own(33, S_RAD, 110, 110);
        }
        if (k == 1) {
            static const uint16_t NO_ENERGY[] = { T_MEX, T_FACTORY, S_RAD };
            s_w.econ[0].energy = 100.0f;
            s_w.econ[0].energy_in = 10.0f;
            s_w.econ[0].energy_out = 30.0f;
            s_w.defs[S_RAD].e.energyuse = 0.0f;
            menu_set(&s_w, T_CONS, NO_ENERGY, 3);
            menu_set(&s_w, T_COM, NO_ENERGY, 3);
        }
        if (k == 2) s_w.unit[2].finished = 0;
        n = sensor_decide(&t, &x, &z);
        printf("sensor want, %s: %d\n", WHAT[k], n);
        CHECK(n == 0, "sensor want, %s: none", WHAT[k]);
    }
    s_milp = NULL;
}

/* THE D-GUN (impai_mil.c): the commander, its D-gun 140 px and 400 energy a shot, fires at a tank whose block
   centre is 104 px off (128 with the block's half diagonal), when the tank is seen now, stored energy covers
   the shot and Hard's reserve (0.25 of the storage), and the tank is worth the shot (400 / 60 metal); of two
   targets it takes the more valuable, and it does not repeat an order its head already holds. Each refusal on its own: a ghost, a radar contact never identified, energy
   short of the reserve, a target worth less than the shot, a target out of range, a def that cannot take the
   order, the commander's safety window, an enemy commander, and a target past comm_r from the anchor. Twelve
   tanks escort the commander, so the safety finds its army winning and keeps it in place. */
static uint16_t s_dgun_target;      /* the last D-gun order's target, WQ_NONE none */
static int dgun_case(const char* name, int expect, void (*setup)(void))
{
    IMPAI_MIL_OUT o;
    int from, n, k;
    mil_world();
    own(1, T_COM, 20, 20);
    for (k = 0; k < 12; k++) own(2 + k, M_TANK, 8 + 2 * (k % 6), 14 + 2 * (k / 6));
    s_w.defs[T_COM].dgun = 1;
    s_ti[T_COM].dgun_range = 140;
    s_ti[T_COM].dgun_e = 400;
    s_w.econ[0].energy = 1000;
    s_w.econ[0].energy_max = 1000;
    enemy(1300, M_TANK, 20, 26, IMPAI_LOS, 1);
    if (setup) setup();
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the D-gun decision runs: %s", name);
    n = count_action(from, A_DGUN, 1);
    s_dgun_target = (uint16_t)WQ_NONE;
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.action == A_DGUN && s_w.log[k].c.unit == 1) s_dgun_target = s_w.log[k].c.target;
    printf("dgun %s: %d order%s%s", name, n, n == 1 ? "" : "s", n ? "," : "\n");
    if (n) printf(" target %u\n", s_dgun_target);
    CHECK(n == expect && o.dgun == (uint8_t)expect, "D-gun %s: %d orders, %d wanted", name, n, expect);
    CHECK(!o.safe && !o.leash, "D-gun %s: the safety moved the commander", name);
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
static void dgun_window_far(void) { s_mil.safe_until = (uint32_t)s_w.gametime + 100000u; }
/* an enemy commander in range, both commanders standing still (speed 0), so the blast's keep is 64 px, inside the
   96 px it stands off: the safety keeps the commander, and the D-gun refuses the target */
static void dgun_commander(void)
{
    s_w.defs[T_COM].d.speed = 0;
    enemy(1300, T_COM, 20, 26, IMPAI_LOS, 1);
}
/* the commander, its escort and the tank 1 280 px east, past comm_r from the anchor */
static void dgun_reach(void)
{
    int k;
    s_w.unit[1].pos[0] += (80 * 2) << 19;
    for (k = 0; k < 12; k++) s_w.unit[2 + k].pos[0] += (80 * 2) << 19;
    enemy(1300, M_TANK, 100, 26, IMPAI_LOS, 1);
}

/* THE D-GUN'S GUARD. The commander stands at (328, 328) and the tank's block centre at (336, 432), 104 px south;
   the shot's radius is 24 px (retail's). */
static void dgun_beside(void) { own(14, M_TANK, 21, 27); }           /* 16 px from the tank's block centre */
static void dgun_between(void) { own(14, T_SOLAR, 20, 23); }         /* on the line, between the two */
static void dgun_behind(void) { own(14, M_TANK, 20, 30); }           /* on the line, 160 px out of 140's range */
static void dgun_aside(void) { own(14, M_TANK, 30, 23); }            /* 160 px east of the line */
static void dgun_frame(void)                                          /* an unfinished frame of the seat's */
{
    unit_add(&s_w, 14, T_SOLAR, 21, 27, 0);
}
static void dgun_ally(void)                                           /* an ally's tank, known by its row alone */
{
    IMPAI_SEEN* r = &s_rows[2500].seat[0];
    s_allies = 1u << 2;
    r->type = M_TANK;
    r->how = IMPAI_LOS;
    r->owner = 2;
    r->bx = 21 / 2;
    r->bz = 28 / 2;
}
static void dgun_own_row(void)                                        /* the seat's own unit outside its block */
{
    IMPAI_SEEN* r = &s_rows[2500].seat[0];
    r->type = M_TANK;
    r->how = IMPAI_OWN;
    r->owner = 0;
    r->bx = 21 / 2;
    r->bz = 28 / 2;
}
static void dgun_blast(void) { s_ti[M_TANK].blast_r = 32; }          /* 104 <= 32 + 64 + 24 */
static void dgun_blast_clear(void) { s_ti[M_TANK].blast_r = 8; }     /* 104 > 8 + 64 + 24 */
static void dgun_wide(void) { s_ti[T_COM].dgun_r = 120; own(14, M_TANK, 30, 23); }   /* the aside tank, a wide shot */
/* the tank has an own tank beside it; the raider, worth less, stands clear */
static void dgun_next(void)
{
    own(14, M_TANK, 21, 27);
    enemy(1301, M_RAIDER, 14, 24, IMPAI_LOS, 1);
}

/* THE COMMANDER'S JOB: the commander walks to a site with a BUILD job when the D-gun is ordered */
static void dgun_job(void)
{
    s_job[1].kind = IMPAI_JOB_BUILD;
    s_job[1].build = T_SOLAR;
    s_job[1].type = T_COM;
    s_job[1].cx = 30;
    s_job[1].cz = 30;
    s_job[1].since = 0;
}

static void check_mil_dgun(void)
{
    IMPAI_MIL_OUT o;
    int from;
    dgun_case("fires at a seen tank", 1, NULL);
    CHECK(s_dgun_target == 1300, "the D-gun's target is the tank");
    from = s_w.nlog;
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0 && count_action(from, A_DGUN, 1) == 0,
          "a D-gun order the head already holds is not given again");
    dgun_case("fires at the more valuable of two", 1, dgun_two);
    CHECK(s_dgun_target == 1300, "the tank (300) over the raider (120)");
    dgun_case("fires at the raider when the tank is gone", 1, dgun_gone);
    CHECK(s_dgun_target == 1301, "the raider, not the slot gone");
    dgun_case("refuses a ghost", 0, dgun_ghost);
    dgun_case("refuses a radar contact never identified", 0, dgun_radar);
    dgun_case("refuses below the energy reserve", 0, dgun_poor);
    dgun_case("refuses a target worth less than the shot", 0, dgun_cheap);
    dgun_case("refuses a target out of range", 0, dgun_far);
    dgun_case("refuses a commander that cannot take the order", 0, dgun_none);
    dgun_case("refuses inside the commander's safety window", 0, dgun_window);
    dgun_case("fires beside a window restored past its length", 1, dgun_window_far);
    dgun_case("refuses an enemy commander", 0, dgun_commander);
    dgun_case("refuses a target past the commander's reach", 0, dgun_reach);
    s_ti[T_COM].dgun_r = 24;
    dgun_case("refuses an own tank beside the target", 0, dgun_beside);
    dgun_case("refuses an own structure on the line", 0, dgun_between);
    dgun_case("refuses an own tank on the line past the target", 0, dgun_behind);
    dgun_case("refuses an own frame beside the target", 0, dgun_frame);
    dgun_case("refuses an ally's tank beside the target", 0, dgun_ally);
    dgun_case("refuses an own unit of the seat's rows beside the target", 0, dgun_own_row);
    dgun_case("refuses a target whose blast reaches the commander", 0, dgun_blast);
    dgun_case("fires at a target whose blast falls short", 1, dgun_blast_clear);
    dgun_case("fires past an own tank off the line", 1, dgun_aside);
    dgun_case("refuses that tank under a wider shot", 0, dgun_wide);
    dgun_case("fires at the next target when the first stands among its own", 1, dgun_next);
    CHECK(s_dgun_target == 1301, "the raider, the tank standing beside an own tank");
    s_ti[T_COM].dgun_r = 0;
    dgun_case("ends the commander's job with no failure", 1, dgun_job);
    CHECK(s_job[1].kind == IMPAI_JOB_NONE, "the D-gun leaves the commander's build job, kind %u", s_job[1].kind);
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

/* THE STAND-OFF's move out held outside the reach (impai_mil.c, step_out): one tank, slot 20, 264 px from a seen
   turret of reach 300, so inside 300 + STAND_PAD:
   (a) the turret south of the tank, the map's north edge 72 px behind it: the point out, 428 px north of the
       turret, held to the map, lies 336 px from it -- no move, the tank attacks the turret;
   (b) on the strait's world, the tank on the west bank 152 px east of the turret: the point out, 428 px east of
       it, is in the channel and its substitute the west bank 160 px from it -- no move, an attack;
   (c) on dry ground, as (b): the move out, to 428 px from it;
   (d) (a) with the turret remembered, not seen: no move and no ground attack from within its reach, the tank
       travels on;
   (e) (c) with a second seen turret, slot 1301, 608 px east of the first, their reaches + STAND_PAD overlapping:
       the point out, 428 px east of the first, is 180 px from the second -- no move, the tank attacks the first;
   (f) (e) with the tank at that point, the second its nearest: the point out of the second is 180 px from the
       first -- no move, the tank attacks the second. Without the second's check the tank went out of one into the
       other and back, each decision;
   (g) (e) with the second a harmless defence -- a submerged type whose one weapon is anti-air, so the fight content
       gives neither a rate on the other, its ground reach 300 all the same --: the point out is not refused, the
       move out as (c);
   (h) (g) with the second's weapon a ground weapon of 1 damage (the squad still wins its prediction): the tank
       cannot hurt it but it hurts the tank -- no move into its reach, the tank attacks the first. */
static void check_mil_stand_out(void)
{
    static const char* const NAME[8] = { "(a) the map's edge behind", "(b) the strait's water behind",
                                         "(c) dry ground behind", "(d) the map's edge behind, remembered",
                                         "(e) a second turret ahead", "(f) between the two, the second nearer",
                                         "(g) a harmless defence ahead", "(h) a defence it cannot hurt ahead" };
    IMPAI_MIL_OUT o;
    int c, k, from, att, gnd, mv, mx, mz;
    for (c = 0; c < 8; c++) {
        int att2 = 0;
        stand_world(300);
        if (c >= 6) {
            s_w.defs[T_TIDAL].armed = 1;
            s_ft[T_TIDAL] = s_ft[T_TURRET];
            s_ft[T_TIDAL].flags |= IMPAI_FT_SUBMERGED;
            s_ft[T_TIDAL].w[0] = T_TIDAL;
            s_fwp[T_TIDAL] = s_fwp[T_TURRET];
            s_fwp[T_TIDAL].flags = c == 6 ? TASIM_WF_TOAIR : 0;
            s_fwp[T_TIDAL].dmg = c == 6 ? s_fwp[T_TIDAL].dmg : 1;
            s_threat[T_TIDAL] = s_threat[T_TURRET];
        }
        if (c == 1) mil_water(W_STRAIT);
        if (c == 0 || c == 3) {
            stand_tank(20, 100, 4);
            enemy(1300, T_TURRET, 100, 20, c ? IMPAI_GHOST : IMPAI_LOS, !c);
        } else {
            stand_tank(20, c == 5 ? 256 : 238, 100);
            enemy(1300, T_TURRET, 228, 100, IMPAI_LOS, 1);
            if (c >= 4) enemy(1301, c >= 6 ? T_TIDAL : T_TURRET, 266, 100, IMPAI_LOS, 1);
        }
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the move out's decision runs");
        mil_print(NAME[c], &o, from);
        stand_orders(from, &att, &gnd, &mv, &mx, &mz);
        for (k = from; k < s_w.nlog; k++)
            att2 += s_w.log[k].c.unit == 20 && s_w.log[k].c.action == A_ATTACK && s_w.log[k].c.target == 1301;
        printf("the move out, %s: task %d, attacks %d + %d, moves %d, point (%d, %d)\n", NAME[c], task_of_slot(20), att,
               att2, mv, mx, mz);
        CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK, "%s: the squad still attacks", NAME[c]);
        if (c < 2) CHECK(att == 1 && !mv && !gnd, "%s: no move into the reach, an attack", NAME[c]);
        else if (c == 4 || c == 7)
            CHECK(att == 1 && !att2 && !mv && !gnd, "%s: no move into the second's reach", NAME[c]);
        else if (c == 5) CHECK(!att && att2 == 1 && !mv && !gnd, "%s: no move back into the first's reach", NAME[c]);
        else if (c == 3) CHECK(!att && !gnd && mv == 1 && mz > 72 + 48, "%s: the travel's move, %d", NAME[c], mz);
        else {
            const int dx = mx - (s_rows[1300].seat[0].bx * 32 + 16);
            CHECK(mv == 1 && !att && dx >= 405 && dx <= 455, "%s: moved out to 428 px from it, %d px", NAME[c], dx);
        }
    }
    s_w.defs[T_TIDAL].armed = 0;
}

/* THE STAND-OFF's progress (impai_mil.c, stand_off, in_reach): on the strait's world one tank, slot 20, on the
   west bank, its goal on the east bank, an ATTACK that comes no nearer it; a turret of reach 300 seen on the east
   bank:
   (a) 864 px off, the nearest west-bank point 816 px from it, past the tank's 600: the attack on it is no progress,
       the squad gives its goal up, the turret's block first into the ring and the goal after it; over the next
       60 decisions the stand-off passes the turret and the tank travels;
   (b) 624 px off, the west bank's edge 576 px from it: the attack is progress, the goal held through (a)'s time
       and as long again;
   (c) 576 px off, in the tank's range, its block given up by the land layer: the tank still attacks it. */
static int stand_progress(int tx, int decisions)
{
    IMPAI_MIL_OUT o;
    int i;
    stand_world(300);
    mil_water(W_STRAIT);
    s_mil.sq[1].tx = 150;
    s_mil.sq[1].tz = 50;
    s_mil.sq[1].closest = IMPAI_CLOSEST_NONE;
    stand_tank(20, 236, 100);
    enemy(1300, T_TURRET, tx, 100, IMPAI_LOS, 1);
    for (i = 0; i < decisions; i++) {
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the stand-off's progress decision runs");
        if (o.stalled) return i;
        s_w.gametime += 15;
    }
    return -1;
}

/* THE SHORE COMMANDER (impai_mil.c, stand_off, com_point, contact_closes; the owner's decision, 2026-11-17): on the
   strait's world a destroyer of reach 660, slot 20, the one member of an ATTACK sea squad, in the channel; an enemy
   commander seen on the east bank (from cell 272, block 136), its ground reach 200 and its D-gun's range 240, so
   its reach 240, and a damage that wins any fight it closes on; the squad's goal on the east bank 2 240 px south:
   (a) the destroyer 112 px off the east bank, the commander 288 px inland: no lost prediction -- the commander does
       not close, the stand-off holding the squad off its shore --, and a move out to 368 px from the bank's nearest
       block (240 + STAND_PAD + STAND_HYST) -- from the shore it can walk to, not from
       where it stands (656 px off, past 304);
   (b) the destroyer 384 px off the bank, the commander 832 px off, 456 px inland, past its range: no attack -- the
       engine would close the destroyer to 660 px of it, 204 px off the bank (attack_clear) -- and no move nearer
       than 304 px to the bank; the goal, which no water reaches, is arrived at (no substitute) and the squad moves
       on;
   (c) the destroyer 384 px off the bank, the commander at the bank's edge 400 px off: an attack on it from where it
       stands, no move;
   (d) (a) with the commander's D-gun range 0: its reach 200, the move out to 328 px from the bank;
   (e) (b)'s destroyer, the commander 936 px inland (so the wave's goal is not the commander, which the squad does
       not reach), and an enemy plant remembered in the channel 2 240 px south, 424 px off the bank, the goal: it
       travels there, unheld by the commander's landmass -- 360 px off the bank, where the band the stand-off once
       held a member in lay;
   (f) (e) with the plant on the channel's edge, 64 px off the bank: the travel point is moved out to 368 px from
       the bank's block nearest it (com_clear), at the plant's latitude;
   (g) (a) with a PT boat beside the destroyer, in the squad -- the submarine's type made a surface boat of reach
       200, which hits the commander and does not outrange its 240 --: the commander closes on that member
       (contact_closes holds only a squad every member of which stands it off), so the prediction is lost and the
       squad retreats;
   (h) (f) with the destroyer standing at the moved-out point (250, 233): it has arrived where travel sends it
       (arrived reads com_clear's point), so the ATTACK moves on -- back to FORM, its goal the same plant --
       rather than stalling short of the plant's own substitute at the bank;
   (i) (a) with an enemy submarine in the channel 720 px south, past the commander from the squad and past its own
       reach of 400, but in the commander's contact: the commander does not close, its contact's submarine does
       (contact_closes reads the contact's other members), so the prediction is made with the contact closing and
       lost, and the squad retreats -- with the submarine read as not closing, the contact stands off and the
       prediction holds;
   (j) (h) with the commander's D-gun range 500, so its reach + STAND_PAD (564) spans the 512-px channel: no point of
       the channel is clear of it (a strait narrower than the reach, decisions.md *What it does not reach*), the
       moved-out point's substitute on the west edge 512 px from the bank -- the destroyer is given no attack, never
       shelling the commander, and the arrival counts it arrived (no clear point), so the ATTACK moves on, back to
       FORM, and travels home;
   (k) (j) with a RAID squad, squad 0, of forty raiders each on an islet of its own in the channel's north, sent to
       the water among them, served before squad 1 so that its substitutes fill the decision's cache (SUB_CACHE):
       the clearance of the destroyer's point is undecided (com_clear, -1), so the arrival is undecided and the
       ATTACK keeps its goal, and the destroyer gets no order this decision, counted unreached -- read as no clear
       point, the full cache sent the squad back to FORM, as in (j). */
static void ashore_world(int ship_cx, int com_cx, int dgun)
{
    stand_world(300);
    mil_water(W_STRAIT);
    s_mil.sq[1].layer = TASIM_LAYER_SEA;
    s_mil.sq[1].tz = 120;
    s_threat[M_DEST].range = s_threat[M_DEST].hit_range = 660;
    s_fwp[M_DEST].range = 660;
    s_fwp[T_COM].dmg = 5000;
    s_threat[T_COM].dps_ground = s_threat[T_COM].hit_ground = 5000;
    s_ti[T_COM].dgun_range = (uint16_t)dgun;
    own(20, M_DEST, ship_cx, 100);
    s_job[20].kind = IMPAI_JOB_SQUAD;
    s_job[20].type = M_DEST;
    s_job[20].squad = 1;
    enemy(1300, T_COM, com_cx, 100, IMPAI_LOS, 1);
}

static void check_mil_shore_commander(void)
{
    static const char* const NAME[11] = { "(a) 112 px off the bank", "(b) off the bank, the commander far",
                                          "(c) off the bank, the commander at the edge",
                                          "(d) 112 px off the bank, no D-gun", "(e) a goal down the channel",
                                          "(f) a goal at the bank's edge", "(g) a PT boat beside the destroyer",
                                          "(h) at the moved-out point", "(i) an enemy submarine in the commander's contact",
                                          "(j) a strait narrower than the reach", "(k) (j), the substitutes' cache full" };
    static const int SHIP[11] = { 266, 249, 249, 266, 249, 249, 266, 250, 266, 250, 250 },
                     COM[11] = { 290, 300, 274, 290, 330, 330, 290, 330, 290, 330, 330 };
    IMPAI_MIL_OUT o;
    int c, k, from, att, gnd, mv, mx, mz;
    for (c = 0; c < 11; c++) {
        ashore_world(SHIP[c], COM[c], c == 3 ? 0 : c >= 9 ? 500 : 240);
        if (c == 4 || c == 5 || c == 7 || c >= 9) enemy(1301, T_FACTORY, c == 4 ? 245 : 266, 240, IMPAI_GHOST, 1);
        if (c == 7 || c >= 9) s_w.unit[20].pos[2] = (233 * 16 + 8) << 16;
        if (c == 10) {
            for (k = 0; k < 40; k++)
                water_add(&s_w, 244 + 4 * (k % 6), 4 + 4 * (k / 6), 244 + 4 * (k % 6), 4 + 4 * (k / 6), -10);
            fake_reach(&s_mreach, M_N);
            squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, 126, 8);
            for (k = 0; k < 40; k++) member(30 + k, M_RAIDER, 244 + 4 * (k % 6), 4 + 4 * (k / 6), 0);
        }
        if (c == 8) enemy(1302, M_SUB, 268, 145, IMPAI_LOS, 1);
        if (c == 6) {
            s_ft[M_SUB].flags &= ~IMPAI_FT_SUBMERGED;
            s_fwp[M_SUB].flags = 0;
            s_fwp[M_SUB].range = 200;
            s_threat[M_SUB].range = s_threat[M_SUB].hit_range = 200;
            own(21, M_SUB, 264, 104);
            s_job[21].kind = IMPAI_JOB_SQUAD;
            s_job[21].type = M_SUB;
            s_job[21].squad = 1;
        }
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the shore commander's decision runs");
        mil_print(NAME[c], &o, from);
        stand_orders(from, &att, &gnd, &mv, &mx, &mz);
        printf("the shore commander, %s: task %d, retreats %u, attacks %d, ground %d, moves %d, point (%d, %d)\n",
               NAME[c], task_of_slot(20), o.retreats, att, gnd, mv, mx, mz);
        if (c == 6 || c == 8)
            CHECK(s_mil.lost_t == (uint32_t)s_w.gametime && o.retreats == 1, "%s: the prediction lost, a retreat",
                  NAME[c]);
        else
            CHECK(s_mil.lost_t != (uint32_t)s_w.gametime && !o.retreats, "%s: no lost prediction, no retreat",
                  NAME[c]);
        if (c == 2 || c == 4 || c == 5)
            CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK, "%s: the squad still attacks", NAME[c]);
        if (c == 0 || c == 3) {
            const int want = c == 0 ? 368 : 328, dx = 4368 - mx;
            CHECK(mv == 1 && !att && !gnd && dx >= want - 24 && dx <= want + 24, "%s: moved out to %d px from the bank, %d",
                  NAME[c], want, dx);
        }
        if (c == 1)
            CHECK(!att && !gnd && (!mv || mx <= 4368 - 304) && task_of_slot(20) != IMPAI_SQ_ATTACK,
                  "%s: no attack, no move nearer the bank, moved on: (%d, %d)", NAME[c], mx, mz);
        if (c == 4)
            CHECK(mv == 1 && !att && mz >= 3700 && mx <= 4368 - 304, "%s: travels south, (%d, %d)", NAME[c], mx, mz);
        if (c == 7) CHECK(task_of_slot(20) == IMPAI_SQ_FORM, "%s: arrived, back to FORM", NAME[c]);
        if (c == 9)
            CHECK(task_of_slot(20) == IMPAI_SQ_FORM && !att && !gnd,
                  "%s: no clear point -- no attack, arrived, back to FORM", NAME[c]);
        if (c == 10) {
            int raid = 0;
            for (k = from; k < s_w.nlog; k++) raid += s_w.log[k].c.action == A_MOVE && s_w.log[k].c.unit >= 30;
            printf("the shore commander, %s: raid moves %d, unreached %u\n", NAME[c], raid, o.unreached);
            CHECK(task_of_slot(20) == IMPAI_SQ_ATTACK && !att && !gnd && !mv,
                  "%s: the clearance undecided -- the ATTACK kept, no order", NAME[c]);
        }
        if (c == 5) {
            const int dx = 4368 - mx;
            CHECK(mv == 1 && !att && mz >= 3700 && dx >= 368 - 24 && dx <= 368 + 24,
                  "%s: the point moved out to 368 px from the bank, %d at (%d, %d)", NAME[c], dx, mx, mz);
        }
        if (c == 2) CHECK(att == 1 && !mv && !gnd, "%s: an attack from where it stands", NAME[c]);
    }
    s_ti[T_COM].dgun_range = 0;
    s_ft[M_SUB].flags |= IMPAI_FT_SUBMERGED;
    s_fwp[M_SUB].flags = TASIM_WF_WATER;
    s_fwp[M_SUB].range = 400;
    s_threat[M_SUB].range = s_threat[M_SUB].hit_range = 400;
}

/* THE SHELLING HOLDS THE GOAL (impai_mil.c, stand_off, orders): on the strait's world one tank, slot 20, of reach
   600, on the west bank, and a turret of reach 300 remembered on the east bank 576 px from it, past 300 + STAND_PAD
   and within the tank's reach; the squad's goal the turret's block, whose substitute on the west bank the tank
   stands at, so it has arrived:
   (a) the first decision: a ground attack at the turret's block, and the goal held -- no move on;
   (b) decisions every 15 ticks: the goal held while the stall window lasts, then given up, the turret's block in
       the land layer's unreached ring, within task_timeout + TRAVEL_CAP (16 200) of the start;
   (c) the turret 720 px off, past the tank's reach from the bank: shelled, but no hold -- the arrived goal moves on
       at the first decision. */
static void check_mil_shell_ghost(void)
{
    IMPAI_MIL_OUT o;
    int from, att, gnd, mv, mx, mz, i, k, ring = 0;
    const uint32_t t0 = (uint32_t)IMPAI_DRY_T + 1000u;
    stand_world(300);
    mil_water(W_STRAIT);
    s_w.gametime = (int)t0;
    s_mil.sq[1].since = t0;
    s_mil.sq[1].closest = IMPAI_CLOSEST_NONE;
    stand_tank(20, 236, 100);
    s_mil.sq[1].tx = 136;
    s_mil.sq[1].tz = 50;
    enemy(1300, T_TURRET, 272, 100, IMPAI_GHOST, 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the shelling's decision runs");
    stand_orders(from, &att, &gnd, &mv, &mx, &mz);
    printf("the shelling holds the goal, (a): task %d, goal (%d, %d), ground attacks %d at (%d, %d)\n",
           s_mil.sq[1].task, s_mil.sq[1].tx, s_mil.sq[1].tz, gnd, mx, mz);
    CHECK(gnd == 1 && s_mil.sq[1].task == IMPAI_SQ_ATTACK && s_mil.sq[1].tx == 136 && s_mil.sq[1].tz == 50,
          "(a) the ghost shelled, the goal held");
    for (i = 1; i < 2000 && s_mil.sq[1].task == IMPAI_SQ_ATTACK && s_mil.sq[1].tx == 136; i++) {
        s_w.gametime += 15;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the shelling's next decision runs");
    }
    for (k = 0; k < IMPAI_UNREACHED; k++)
        ring |= s_mil.unreached[k].bx == 272 / 2 && s_mil.unreached[k].bz == 100 / 2;
    printf("the shelling holds the goal, (b): goal left after %u ticks, the turret's block in the ring %d\n",
           (unsigned)s_w.gametime - t0, ring);
    CHECK((uint32_t)s_w.gametime - t0 > IMPAI_PLAN_ROW.task_timeout / 2 &&
              (uint32_t)s_w.gametime - t0 <= IMPAI_PLAN_ROW.task_timeout + 16200u && ring,
          "(b) the goal held for the stall window, then given up with the turret's block");
    stand_world(300);
    mil_water(W_STRAIT);
    s_w.gametime = (int)t0;
    s_mil.sq[1].since = t0;
    s_mil.sq[1].closest = IMPAI_CLOSEST_NONE;
    stand_tank(20, 236, 100);
    s_mil.sq[1].tx = 140;
    s_mil.sq[1].tz = 50;
    enemy(1300, T_TURRET, 281, 100, IMPAI_GHOST, 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the shelling's (c) decision runs");
    stand_orders(from, &att, &gnd, &mv, &mx, &mz);
    printf("the shelling holds the goal, (c) out of reach: task %d, goal (%d, %d), ground attacks %d\n",
           s_mil.sq[1].task, s_mil.sq[1].tx, s_mil.sq[1].tz, gnd);
    CHECK(gnd == 1 && !(s_mil.sq[1].task == IMPAI_SQ_ATTACK && s_mil.sq[1].tx == 140 && s_mil.sq[1].tz == 50),
          "(c) shelled out of reach: the goal moved on");
}

static void check_mil_stand_progress(void)
{
    const IMPAI_GOALFAIL* f = s_mil.unreached;
    IMPAI_MIL_OUT o;
    int a, b, i, from, att, gnd, mv, mx, mz;
    a = stand_progress(290, 2000);
    printf("the stand-off's progress, (a) out of reach: given up at decision %d, ring (%d, %d) (%d, %d)\n", a, f[0].bx,
           f[0].bz, f[1].bx, f[1].bz);
    CHECK(a > 0 && f[0].bx == s_rows[1300].seat[0].bx && f[0].bz == s_rows[1300].seat[0].bz &&
              f[0].layer == TASIM_LAYER_LAND && s_mil.unreached_next == 2,
          "(a) the goal given up, the turret's block first");
    from = s_w.nlog;
    for (i = 0; i < 60; i++) {
        s_w.gametime += 15;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(a) the decisions after run");
    }
    stand_orders(from, &att, &gnd, &mv, &mx, &mz);
    printf("the stand-off's progress, (a) 60 decisions after: %d attacks, %d ground attacks, %d moves\n", att, gnd, mv);
    CHECK(!att && !gnd && mv >= 1, "(a) the given-up turret not stood at, the tank travels: %d attacks, %d moves", att,
          mv);
    b = stand_progress(275, 2 * a + 2);
    printf("the stand-off's progress, (b) in reach from the bank: given up at decision %d\n", b);
    CHECK(a > 0 && b < 0, "(b) the goal held through %d decisions", 2 * a + 2);
    stand_world(300);
    mil_water(W_STRAIT);
    stand_tank(20, 236, 100);
    enemy(1300, T_TURRET, 272, 100, IMPAI_LOS, 1);
    s_mil.unreached[0].bx = s_rows[1300].seat[0].bx;
    s_mil.unreached[0].bz = s_rows[1300].seat[0].bz;
    s_mil.unreached[0].t = (uint32_t)s_w.gametime;
    s_mil.unreached[0].layer = TASIM_LAYER_LAND;
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(c) the decision runs");
    stand_orders(from, &att, &gnd, &mv, &mx, &mz);
    printf("the stand-off's progress, (c) in range, given up: %d attacks, %d moves\n", att, mv);
    CHECK(att == 1 && !mv, "(c) a defence in range is attacked though its block is given up");
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
    /* THE EUCLIDEAN NEAREST (impai_reach_nearest, THE SHORE COMMANDER's point): from (0, 0), label 4 at ring 3's
       corner (3, 3), 18 squared, found first in ring order, and at ring 4's (4, 0), 16 squared: the nearer is (4, 0),
       past the first hit's ring; with radius 3, the corner; with (4, 0) gone, the corner */
    for (i = 0; i < 64; i++) lab[i] = 1;
    lab[3 * 8 + 3] = lab[0 * 8 + 4] = 4;
    CHECK(impai_reach_nearest(&r, 0, 4, 0, 0, 8, &ox, &oz) && ox == 4 && oz == 0, "the Euclidean nearest: %d,%d", ox,
          oz);
    CHECK(impai_reach_near(&r, 0, 4, 0, 0, 8, &ox, &oz) && ox == 3 && oz == 3, "ring order's first: %d,%d", ox, oz);
    CHECK(impai_reach_nearest(&r, 0, 4, 0, 0, 3, &ox, &oz) && ox == 3 && oz == 3, "the nearest within 3: %d,%d", ox,
          oz);
    lab[0 * 8 + 4] = 1;
    CHECK(impai_reach_nearest(&r, 0, 4, 0, 0, 8, &ox, &oz) && ox == 3 && oz == 3, "the corner alone: %d,%d", ox, oz);
    CHECK(!impai_reach_nearest(&r, 0, 5, 0, 0, 8, &ox, &oz), "the Euclidean nearest of no block: none");
    /* the radius held to the grid's farthest block: a centre 5 off the grid's west edge still finds the far corner,
       12 rings away, under a radius of 100 */
    for (i = 0; i < 64; i++) lab[i] = 1;
    lab[7 * 8 + 7] = 3;
    CHECK(impai_reach_near(&r, 0, 3, -5, 0, 100, &ox, &oz) && ox == 7 && oz == 7, "the far corner off the grid: %d,%d",
          ox, oz);
    CHECK(impai_reach_near(&r, 0, 3, 0, 0, 100, &ox, &oz) && ox == 7 && oz == 7, "the far corner from the grid: %d,%d",
          ox, oz);
    CHECK(!impai_reach_near(&r, 0, 4, 3, 3, 100, &ox, &oz), "no such label on the grid");
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
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the naval decision runs: %s", name);
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

/* THE STARTS LOOKED AT on the strait world (impai_mil.c, est_start): thirty tanks on the west bank, nothing known;
   the nearer other start on the east bank, which the tanks do not reach, is passed for the farther one on theirs. */
static void check_mil_start_banks(void)
{
    static const WQ_START BANKS[3] = { { 400, 400, 0, 0 }, { 4700, 400, 1, 0 }, { 3500, 3900, 2, 0 } };
    IMPAI_MIL_OUT o;
    int k;
    mil_world();
    mil_water(W_STRAIT);
    memcpy(s_w.starts, BANKS, sizeof BANKS);
    s_w.nstarts = 3;
    for (k = 0; k < 30; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the banks' decision runs");
    printf("the starts looked at, the far bank: the estimate (%d, %d)\n", s_mil.ebx * 32 + 16, s_mil.ebz * 32 + 16);
    CHECK(s_mil.est_seen == EST_START && s_mil.ebx == 3500 / 32 && s_mil.ebz == 3900 / 32,
          "the starts looked at: a start the land army does not reach is passed");
    s_mil_reach = NULL;
}

/* THE DRY HUNT (impai_mil.c, dry_hunt) on the strait world, the anchor at cell (20, 20) west of the channel:
   (a) no enemy known: no stamp, not dry; (b) an enemy extractor's ghost on the west bank: stamped now;
   (c) it gone and one on the east bank only: IMPAI_DRY_T on, not yet dry; 15 ticks more, dry; (d) an enemy VTOL
   seen over the west bank: still dry; (e) an enemy tank seen there: not dry, stamped now; (f) the east bank's
   ghost alone again, a stamp restored in the future: held to now, not dry; (g) a foe's commander remembered
   fresh on the west bank: stamped; (h) the same stale stamp on the dry map, no sea block: never dry. THE EVIDENCE:
   (j) the land stamp long past and no enemy known off the land: not dry, no off-land stamp; (k) an off-land stamp
   IMPAI_DRY_T + 15 old: not dry; the east bank's ghost then known: dry, stamped now; (l) dry, a radar contact never
   identified on the west bank: still dry, no land stamp -- the end needs an identity --; not dry, the same contact:
   stamped, delaying the hunt; (m) no row, a foe's commander remembered fresh on the east bank, on the channel's far
   side: the off-land evidence, dry, stamped now; (n) THE LAND LOOKED OVER: a tank on the west bank, the land stamp
   long past, nothing known: with the north-west sweep cell never stood in, not dry; once it is, every cell the land
   army reaches stood in since the stamp: dry, with no off-land stamp. */
static void dry_case(const char* name, IMPAI_MIL_OUT* o)
{
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, o) == 0, "the dry hunt's decision runs: %s", name);
    printf("dry hunt, %s: dry=%u land_t=%u now=%d\n", name, s_mil.dry, s_mil.land_t, s_w.gametime);
}

static void check_dry_hunt(void)
{
    IMPAI_MIL_OUT o;
    uint32_t t0;
    mil_world();
    mil_water(W_STRAIT);
    s_w.gametime = 3000;
    dry_case("(a) nothing known", &o);
    CHECK(!s_mil.dry && !s_mil.land_t, "(a) no enemy known yet: no stamp, not dry");
    enemy(1300, T_MEX, 100, 100, IMPAI_GHOST, 0);
    dry_case("(b) a ghost on the west bank", &o);
    CHECK(!s_mil.dry && s_mil.land_t == 3000u, "(b) a land-linked ghost stamps now");
    s_rows[1300].seat[0].how = 0;
    enemy(1301, T_MEX, 300, 100, IMPAI_GHOST, 0);
    t0 = s_mil.land_t;
    s_w.gametime = (int)(t0 + IMPAI_DRY_T);
    dry_case("(c) the east bank only, IMPAI_DRY_T on", &o);
    CHECK(!s_mil.dry && s_mil.land_t == t0, "(c) IMPAI_DRY_T on: not yet dry");
    s_w.gametime += 15;
    dry_case("(c) 15 ticks more", &o);
    CHECK(s_mil.dry == 1 && s_mil.land_t == t0, "(c) past IMPAI_DRY_T with only the east bank known: dry");
    s_ti[M_SCOUT].flags |= IMPAI_TF_VTOL;
    enemy(1302, M_SCOUT, 100, 120, IMPAI_LOS, 1);
    s_w.gametime += 15;
    dry_case("(d) a VTOL over the west bank", &o);
    CHECK(s_mil.dry == 1, "(d) an aircraft over land is no land enemy");
    s_ti[M_SCOUT].flags &= (uint8_t)~IMPAI_TF_VTOL;
    s_rows[1302].seat[0].how = 0;
    s_w.unit[1302].live = 0;
    enemy(1303, M_TANK, 100, 120, IMPAI_LOS, 1);
    s_w.gametime += 15;
    dry_case("(e) a tank on the west bank", &o);
    CHECK(!s_mil.dry && s_mil.land_t == (uint32_t)s_w.gametime, "(e) a land enemy seen again ends the dry hunt");
    s_rows[1303].seat[0].how = 0;
    s_w.unit[1303].live = 0;
    s_mil.land_t = (uint32_t)s_w.gametime + 100000u;
    s_w.gametime += 15;
    dry_case("(f) a stamp restored in the future", &o);
    CHECK(!s_mil.dry && s_mil.land_t == (uint32_t)s_w.gametime, "(f) a future stamp is held to now");
    s_mil.land_t = 1;
    s_mil.foe[1].slot = 1310;
    s_mil.foe[1].bx = 50;
    s_mil.foe[1].bz = 50;
    s_mil.foe[1].t = (uint32_t)s_w.gametime;
    s_w.gametime += 15;
    dry_case("(g) a foe's commander remembered on the west bank", &o);
    CHECK(!s_mil.dry && s_mil.land_t == (uint32_t)s_w.gametime, "(g) a fresh foe memory on land stamps");
    mil_world();
    mil_water(W_DRY);
    s_w.gametime = 30000;
    s_mil.land_t = 1;
    s_mil.dry = 1;
    dry_case("(h) the dry map", &o);
    CHECK(!s_mil.dry, "(h) no sea block: never dry");
    mil_world();
    mil_water(W_STRAIT);
    s_w.gametime = (int)IMPAI_DRY_T + 1000;
    s_mil.land_t = 1;
    dry_case("(j) nothing known off the land", &o);
    CHECK(!s_mil.dry && !s_mil.sea_t, "(j) no evidence of an enemy off the land: not dry");
    s_mil.sea_t = (uint32_t)s_w.gametime - IMPAI_DRY_T - 15u;
    s_w.gametime += 15;
    dry_case("(k) an off-land stamp past IMPAI_DRY_T", &o);
    CHECK(!s_mil.dry, "(k) evidence older than IMPAI_DRY_T: not dry");
    enemy(1301, T_MEX, 300, 100, IMPAI_GHOST, 0);
    s_w.gametime += 15;
    dry_case("(k) the east bank's ghost known", &o);
    CHECK(s_mil.dry && s_mil.sea_t == (uint32_t)s_w.gametime, "(k) an enemy known off the land: dry, stamped now");
    enemy(1302, M_TANK, 100, 120, IMPAI_RADAR, 1);
    s_rows[1302].seat[0].type = (uint16_t)WQ_NONE;
    s_rows[1302].seat[0].owner = 0xFF;
    t0 = s_mil.land_t;
    s_w.gametime += 15;
    dry_case("(l) dry, a radar contact on the west bank", &o);
    CHECK(s_mil.dry && s_mil.land_t == t0, "(l) a contact never identified ends no hunt");
    s_mil.dry = 0;
    s_w.gametime += 15;
    dry_case("(l) not dry, the same contact", &o);
    CHECK(!s_mil.dry && s_mil.land_t == (uint32_t)s_w.gametime, "(l) before the hunt, any contact stamps the land");
    mil_world();
    mil_water(W_STRAIT);
    s_w.gametime = (int)IMPAI_DRY_T + 1000;
    s_mil.land_t = 1;
    s_mil.foe[1].slot = 1310;
    s_mil.foe[1].bx = 150;
    s_mil.foe[1].bz = 50;
    s_mil.foe[1].t = (uint32_t)s_w.gametime;
    s_w.gametime += 15;
    dry_case("(m) a foe's commander remembered on the east bank", &o);
    CHECK(s_mil.dry && s_mil.sea_t == (uint32_t)s_w.gametime && s_mil.land_t == 1u,
          "(m) a fresh foe memory off the land is the evidence: dry, stamped now");
    {
        int c;
        mil_world();
        mil_water(W_STRAIT);
        own(1, M_TANK, 20, 20);
        s_w.gametime = (int)IMPAI_DRY_T + 1000;
        s_mil.land_t = 1;
        for (c = 0; c < IMPAI_SWEEP; c++) s_mil.swept[c] = (uint16_t)(s_w.gametime >> 6);
        s_mil.swept[0] = 0;
        dry_case("(n) one west-bank cell not looked at", &o);
        CHECK(!s_mil.dry, "(n) a cell the land army reaches not stood in: not dry");
        s_mil.swept[0] = (uint16_t)(s_w.gametime >> 6);
        s_w.gametime += 15;
        dry_case("(n) the land looked over", &o);
        CHECK(s_mil.dry && !s_mil.sea_t && s_mil.land_t == 1u, "(n) every cell the land army reaches stood in: dry");
    }
    /* (i) THE LEASH in a dry hunt: the commander 1 696 px out on the strait world's west bank, its land stamp long
       past -- past comm_r + LEASH_PAD_PX, within FAR_PATCH_K x comm_r + LEASH_PAD_PX: not leashed while dry, and
       leashed again once a land enemy is seen */
    {
        int32_t x, z;
        int moved;
        mil_world();
        armcom();
        mil_water(W_STRAIT);
        s_st.opened = 1;
        s_mil.land_t = 1;
        s_w.gametime = (int)IMPAI_DRY_T + 100;
        own(1, T_COM, 126, 20);
        enemy(1301, T_MEX, 300, 100, IMPAI_GHOST, 0);
        moved = com_case("(i) 1 696 px out in a dry hunt", &o, &x, &z);
        CHECK(s_mil.dry && !moved && !o.leash, "(i) the leash in a dry hunt: FAR_PATCH_K x comm_r");
        enemy(1300, T_MEX, 100, 100, IMPAI_LOS, 1);
        s_w.gametime += 15;
        moved = com_case("(i) a land enemy seen again", &o, &x, &z);
        CHECK(!s_mil.dry && moved && o.leash == 1, "(i) the dry hunt over: leashed at comm_r");
    }
    s_mil_reach = NULL;
}

/* THE FINISH'S WAVE (impai_mil.c, form_attack) on the strait world, an enemy factory's ghost on the east bank:
   (a) the land stamp long past, so dry, two submarines in the channel: a sea wave at two median units, the sea's
   count 1; (b) the stamp now, not dry: no sea wave below wave_value's eight; (c) dry, two tanks on the west bank
   and no submarine: no land wave, the land keeping wave_value. THE FINISH IS EXEMPT (THE ATTENTION): (d) dry, the
   two submarines, two land ATTACKs held at Easy's att_mil of 2: the sea wave goes and both land ATTACKs are kept,
   the operations counted 2; (e) the same at Hard's att_mil: the same; (f) not dry, nine submarines past
   wave_value, at Easy's: no sea wave, both land ATTACKs kept -- the exemption is the dry hunt's. THE FINISH'S END,
   (g): after the sea wave, a decision 15 ticks later, still dry, keeps the sea ATTACK; an enemy tank then seen on
   the west bank ends the dry hunt: at Easy with two land ATTACKs held, three operations past att_mil 2, the sea
   ATTACK goes back to FORM and gives its wave back, the land's kept; at Hard, no att_mil, and at Easy with one land
   ATTACK held, within att_mil, the sea ATTACK goes on; the tank a radar contact never identified ends nothing.
   And (h), dry at Easy's, one land ATTACK held, the two submarines and twelve tanks: both waves go, the sea's
   counting no room. */
static void held_squad(int s, unsigned task, int slot, int tx, int tz);

static void dry_wave(const char* name, int dry, int subs, int tanks, int held, IMPAI_MIL_OUT* o)
{
    int k, from;
    mil_world();
    mil_water(W_STRAIT);
    s_w.gametime = (int)IMPAI_DRY_T + 1000;
    s_mil.land_t = dry ? 1u : (uint32_t)s_w.gametime;
    s_ti[M_SUB].klass = IMPAI_K_SRAIDER;
    for (k = 0; k < subs; k++) own(40 + k, M_SUB, 244 + 2 * (k % 3), 40 + 2 * (k / 3));
    for (k = 0; k < tanks; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    for (k = 0; k < held; k++) held_squad(9 + k, IMPAI_SQ_ATTACK, 30 + k, 50, 60 + 4 * k);
    enemy(1301, T_FACTORY, 280, 100, IMPAI_GHOST, 1);
    /* not dry: an extractor's ghost on the west bank, a land enemy, so the map is no island (THE ISLAND) */
    if (!dry) enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, o) == 0, "the finish's wave decision runs: %s", name);
    mil_print(name, o, from);
    printf("finish's wave, %s: dry=%u waves land %u sea %u, counts land %u sea %u\n", name, s_mil.dry,
           o->waves[TASIM_LAYER_LAND], o->waves[TASIM_LAYER_SEA], s_mil.waves[TASIM_LAYER_LAND],
           s_mil.waves[TASIM_LAYER_SEA]);
}

/* THE LAND ARMY'S REACH (impai_mil.c, land_block) on the strait world, the anti-air K-bot moving by class 2, which
   crosses the channel, so that the content's land weights -- three land types of class 0 against one of class 2 --
   still read the east bank as off the anchor's land. The land stamp long past and an enemy factory's ghost on the
   east bank; the seat's land army at home on the west bank:
   (a) K-bots, which reach the factory: it is a land enemy, land_t is stamped now, the seat is not dry;
   (b) tanks, which do not: no stamp, dry;
   (c) one K-bot (150) beside three tanks (900): less than half the army reaches, dry; three K-bots (450) beside one
       tank (300): more than half, a land enemy;
   (d) no land army: the content's test, off the land, dry. */
static void army_reach_case(const char* name, int kbots, int tanks, int want_land)
{
    IMPAI_MIL_OUT o;
    int k;
    mil_world();
    s_ti[M_AA].mclass = 2;
    s_w.defs[M_AA].d.mclass = 2;
    mil_water(W_STRAIT);
    s_w.gametime = (int)IMPAI_DRY_T + 1000;
    s_mil.land_t = 1;
    for (k = 0; k < kbots; k++) own(1 + k, M_AA, 24 + 2 * k, 24);
    for (k = 0; k < tanks; k++) own(20 + k, M_TANK, 24 + 2 * k, 30);
    enemy(1301, T_FACTORY, 300, 100, IMPAI_GHOST, 0);
    dry_case(name, &o);
    printf("army's reach %s: dry=%u land_t=%u\n", name, s_mil.dry, s_mil.land_t);
    if (want_land)
        CHECK(s_mil.land_t == (uint32_t)s_w.gametime && !s_mil.dry, "%s: a land enemy, stamped, not dry", name);
    else
        CHECK(s_mil.land_t == 1u && s_mil.dry == 1, "%s: off the land, dry", name);
}

static void check_dry_army_reach(void)
{
    army_reach_case("(a) K-bots", 3, 0, 1);
    army_reach_case("(b) tanks", 0, 3, 0);
    army_reach_case("(c) one K-bot, three tanks", 1, 3, 0);
    army_reach_case("(c) three K-bots, one tank", 3, 1, 1);
    army_reach_case("(d) no land army", 0, 0, 0);
    s_mil_reach = NULL;
}

/* the strait world's dry hunt with the land stamp long past (1) and no off-land stamp, at IMPAI_DRY_T + 1 000 */
static void dry_start(void)
{
    mil_world();
    mil_water(W_STRAIT);
    s_w.gametime = (int)IMPAI_DRY_T + 1000;
    s_mil.land_t = 1;
}

/* layer L's goal given up at cell (cx, cz), `ago` ticks before now, into ring entry 0 */
static void given_up(unsigned L, int cx, int cz, uint32_t ago)
{
    IMPAI_GOALFAIL* f = &s_mil.unreached[0];
    f->bx = (int16_t)(cx / 2);
    f->bz = (int16_t)(cz / 2);
    f->t = (uint32_t)s_w.gametime - ago;
    f->layer = (uint8_t)L;
}

/* A GOAL GIVEN UP stamps nothing (impai_mil.c, dry_hunt): on the strait world, three tanks at home on the west
   bank, an extractor's ghost there and a factory's on the east bank, the land stamp long past.
   (a) Nothing given up: the extractor stamps the land, not dry.
   (b) The land layer gave up a goal at the extractor 100 ticks ago: no land stamp, the factory off the land -- dry.
   (c) The sea layer's: the land's stamp.
   (d) The land layer's, IMPAI_UNREACHED_T + 15 ago: the land's stamp.
   (e) No extractor; the foe's commander remembered fresh at the given-up goal: no land stamp, dry.
   THE END NEEDS AN ENEMY SEEN NOW, the seat dry already:
   (f) the land's goal given up past IMPAI_UNREACHED_T, the extractor's ghost: no land stamp, still dry;
   (g) the same, the foe's commander remembered fresh there: still dry;
   (h) the extractor seen now: the land's stamp, the hunt over. */
static void given_up_case(const char* name, unsigned L, uint32_t ago, int foe, int want_land)
{
    IMPAI_MIL_OUT o;
    int k;
    const int was_dry = name[1] >= 'f';
    dry_start();
    s_mil.dry = (uint8_t)was_dry;
    for (k = 0; k < 3; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    if (!foe) enemy(1300, T_MEX, 100, 100, name[1] == 'h' ? IMPAI_LOS : IMPAI_GHOST, name[1] == 'h');
    else {
        s_mil.foe[1].slot = 1310;
        s_mil.foe[1].bx = 50;
        s_mil.foe[1].bz = 50;
        s_mil.foe[1].t = (uint32_t)s_w.gametime;
    }
    enemy(1301, T_FACTORY, 300, 100, IMPAI_GHOST, 0);
    if (ago) given_up(L, 100, 100, ago);
    dry_case(name, &o);
    if (want_land)
        CHECK(s_mil.land_t == (uint32_t)s_w.gametime && !s_mil.dry, "%s: the land's stamp, not dry", name);
    else
        CHECK(s_mil.land_t == 1u && s_mil.dry && s_mil.sea_t == (uint32_t)s_w.gametime, "%s: no land stamp, dry",
              name);
}

static void check_dry_given_up(void)
{
    given_up_case("(a) nothing given up", TASIM_LAYER_LAND, 0, 0, 1);
    given_up_case("(b) the land's goal given up", TASIM_LAYER_LAND, 100, 0, 0);
    given_up_case("(c) the sea's goal given up", TASIM_LAYER_SEA, 100, 0, 1);
    given_up_case("(d) the land's, past IMPAI_UNREACHED_T", TASIM_LAYER_LAND, IMPAI_UNREACHED_T + 15, 0, 1);
    given_up_case("(e) the foe's commander at the land's given-up goal", TASIM_LAYER_LAND, 100, 1, 0);
    given_up_case("(f) dry, the ring's entry gone, the ghost", TASIM_LAYER_LAND, IMPAI_UNREACHED_T + 15, 0, 0);
    given_up_case("(g) dry, the ring's entry gone, the foe's memory", TASIM_LAYER_LAND, IMPAI_UNREACHED_T + 15, 1, 0);
    given_up_case("(h) dry, the extractor seen now", TASIM_LAYER_LAND, IMPAI_UNREACHED_T + 15, 0, 1);
    s_mil_reach = NULL;
}

/* UNDER THE SEA, A WATER WEAPON (impai_mil.c, dry_hunt) on the strait world, the land stamp long past, the anti-air
   K-bot of class 2 -- the hovercraft's class, which goes everywhere -- as the land army, three of them at home:
   (a) an enemy commander held by sonar alone in the channel: no K-bot carries a water weapon, so off the land -- dry;
   (b) the K-bots given a water weapon: the land's, stamped, not dry;
   (c) the commander seen (LOS), no water weapon: the land's -- a unit seen is not under the sea;
   (d) a submarine seen in the channel, a type the fight content holds submerged: off the land, dry;
   (e) no land army, the commander held by sonar alone on the west bank, which the content's test links to the
       anchor: off the land, dry.
   THE LAND ARMY'S REACH, as data: (f) the three K-bots and a tank whose value is not finite, the factory's ghost
   on the east bank: the tank counts nothing, the K-bots reach it -- the land's. A ghost at a block past the
   grid's edge (g), the tanks' army: the land's, never off it. */
static void under_case(const char* name, int kbots, int water, int type, unsigned how, int cx, int want_land)
{
    IMPAI_MIL_OUT o;
    int k;
    dry_start();
    s_ti[M_AA].mclass = 2;
    s_w.defs[M_AA].d.mclass = 2;
    if (water) s_ti[M_AA].attack |= IMPAI_TA_WATER1;
    for (k = 0; k < kbots; k++) own(1 + k, M_AA, 24 + 2 * k, 24);
    enemy(1300, type, cx, 100, how, (how & IMPAI_NOW) != 0);
    dry_case(name, &o);
    s_ti[M_AA].attack &= (uint8_t)~IMPAI_TA_WATER1;
    if (want_land)
        CHECK(s_mil.land_t == (uint32_t)s_w.gametime && !s_mil.dry, "%s: the land's, stamped, not dry", name);
    else
        CHECK(s_mil.land_t == 1u && s_mil.dry, "%s: off the land, dry", name);
}

static void check_dry_under(void)
{
    IMPAI_MIL_OUT o;
    float v;
    int k;
    under_case("(a) a sonar contact, no water weapon", 3, 0, T_COM, IMPAI_SONAR, 256, 0);
    under_case("(b) a sonar contact, a water weapon", 3, 1, T_COM, IMPAI_SONAR, 256, 1);
    under_case("(c) the commander seen", 3, 0, T_COM, IMPAI_LOS, 256, 1);
    under_case("(d) a submarine seen", 3, 0, M_SUB, IMPAI_LOS, 256, 0);
    under_case("(e) no land army, a sonar contact on the west bank", 0, 0, T_COM, IMPAI_SONAR, 100, 0);
    dry_start();
    s_ti[M_AA].mclass = 2;
    s_w.defs[M_AA].d.mclass = 2;
    for (k = 0; k < 3; k++) own(1 + k, M_AA, 24 + 2 * k, 24);
    own(10, M_TANK, 30, 30);
    v = s_ti[M_TANK].value;
    s_ti[M_TANK].value = NAN;
    enemy(1301, T_FACTORY, 300, 100, IMPAI_GHOST, 0);
    dry_case("(f) a tank of no finite value", &o);
    s_ti[M_TANK].value = v;
    CHECK(s_mil.land_t == (uint32_t)s_w.gametime && !s_mil.dry, "(f) a value not finite counts nothing: the land's");
    dry_start();
    for (k = 0; k < 3; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    enemy(1301, T_FACTORY, 300, 100, IMPAI_GHOST, 0);
    enemy(1302, T_MEX, 2 * (s_mreach.bw + 5), 100, IMPAI_GHOST, 0);
    dry_case("(g) a ghost past the grid", &o);
    CHECK(s_mil.land_t == (uint32_t)s_w.gametime && !s_mil.dry, "(g) a block past the grid is the land's");
    s_mil_reach = NULL;
}

static int land_attacks(void)
{
    int k, n = 0;
    for (k = 0; k < IMPAI_SQUADS; k++)
        n += s_mil.sq[k].task == IMPAI_SQ_ATTACK && s_mil.sq[k].layer == TASIM_LAYER_LAND;
    return n;
}

static void check_dry_wave(void)
{
    IMPAI_MIL_OUT o;
    int n, c;
    dry_wave("(a) dry, two submarines", 1, 2, 0, 0, &o);
    CHECK(s_mil.dry && o.waves[TASIM_LAYER_SEA] == 1 && s_mil.waves[TASIM_LAYER_SEA] == 1,
          "(a) dry: the sea wave goes at two median units");
    dry_wave("(b) not dry, two submarines", 0, 2, 0, 0, &o);
    CHECK(!s_mil.dry && !o.waves[TASIM_LAYER_SEA], "(b) not dry: no sea wave below wave_value");
    dry_wave("(c) dry, two tanks", 1, 0, 2, 0, &o);
    CHECK(s_mil.dry && !o.waves[TASIM_LAYER_LAND], "(c) dry: the land keeps wave_value");
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    dry_wave("(d) dry, two land ATTACKs at Easy's att_mil", 1, 2, 0, 2, &o);
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    n = land_attacks();
    printf("finish's room (d): land attacks %d, ops %u\n", n, o.ops);
    CHECK(o.waves[TASIM_LAYER_SEA] == 1 && n == 2 && o.ops == 2,
          "(d) the sea wave goes outside the attention, the land operations kept");
    dry_wave("(e) dry, two land ATTACKs at Hard's att_mil", 1, 2, 0, 2, &o);
    n = land_attacks();
    printf("finish's room (e): land attacks %d\n", n);
    CHECK(o.waves[TASIM_LAYER_SEA] == 1 && n == 2, "(e) room left: the land operations kept");
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    dry_wave("(f) not dry, nine submarines, two land ATTACKs at Easy's att_mil", 0, 9, 0, 2, &o);
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    n = land_attacks();
    printf("finish's room (f): land attacks %d\n", n);
    CHECK(!o.waves[TASIM_LAYER_SEA] && n == 2, "(f) not dry: THE ATTENTION holds the sea wave, the land kept");
    for (c = 0; c < 4; c++) {
        static const char* const NAME[4] = { "(g) Easy, two land ATTACKs held", "(g) Hard, two land ATTACKs held",
                                             "(g) Easy, one land ATTACK held", "(g) Easy, a radar contact on the shore" };
        const int held = c == 2 ? 1 : 2;
        int k, sea = 0;
        s_persona = &IMPAI_PERSONAS[c == 1 ? IMPAI_LV_HARD : IMPAI_LV_EASY];
        dry_wave(NAME[c], 1, 2, 0, held, &o);
        CHECK(o.waves[TASIM_LAYER_SEA] == 1, "%s: the sea wave first", NAME[c]);
        s_w.gametime += 15;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the finish's end decision runs, still dry");
        for (k = 0; k < IMPAI_SQUADS; k++)
            sea += s_mil.sq[k].task == IMPAI_SQ_ATTACK && s_mil.sq[k].layer == TASIM_LAYER_SEA;
        CHECK(s_mil.dry && sea == 1 && s_mil.waves[TASIM_LAYER_SEA] == 1, "%s: still dry, the sea ATTACK kept, no new wave",
              NAME[c]);
        enemy(1302, M_TANK, 100, 120, c == 3 ? IMPAI_RADAR : IMPAI_LOS, 1);
        if (c == 3) {
            s_rows[1302].seat[0].type = (uint16_t)WQ_NONE;
            s_rows[1302].seat[0].owner = 0xFF;
        }
        s_w.gametime += 15;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the finish's end decision runs");
        for (k = 0, sea = 0; k < IMPAI_SQUADS; k++)
            sea += s_mil.sq[k].task == IMPAI_SQ_ATTACK && s_mil.sq[k].layer == TASIM_LAYER_SEA;
        n = land_attacks();
        printf("finish's end, %s: dry=%u sea attacks %d land attacks %d sea waves %u\n", NAME[c], s_mil.dry, sea, n,
               s_mil.waves[TASIM_LAYER_SEA]);
        if (c == 0)
            CHECK(!s_mil.dry && !sea && n == 2 && !s_mil.waves[TASIM_LAYER_SEA],
                  "%s: past att_mil, the sea ATTACK back to FORM and its wave given back, the land's kept", NAME[c]);
        else if (c == 3)
            CHECK(s_mil.dry && sea == 1, "%s: a contact never identified ends no hunt", NAME[c]);
        else
            CHECK(!s_mil.dry && sea == 1 && n == held && s_mil.waves[TASIM_LAYER_SEA] == 1,
                  "%s: no room to give, the sea ATTACK goes on", NAME[c]);
    }
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    dry_wave("(h) dry, one land ATTACK held, two submarines and twelve tanks at Easy's", 1, 2, 12, 1, &o);
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    n = land_attacks();
    printf("finish's room (h): land attacks %d, ops %u\n", n, o.ops);
    CHECK(o.waves[TASIM_LAYER_SEA] == 1 && o.waves[TASIM_LAYER_LAND] == 1 && n == 2 && o.ops == 2,
          "(h) the land wave takes the attention's second room, the sea's counting none");
    s_mil_reach = NULL;
}

/* THE ISLAND (impai_mil.c, island) on the strait world: two submarines in the channel, an enemy plant's ghost on the
   east bank, the seat's start (328, 328) on the west bank and the other (6 416, 3 216) on the east, not dry -- the
   land stamp fresh --, the starts seated (`west` 1, 2, 4) or random (0, 3):
   (a) nothing the land army reaches, an island: the sea wave goes at two median units, the finish's size;
   (b) a third start on the west bank seated by a foe: the land reaches it, no island, no sea wave below wave_value;
   (c) that start left empty (WQ_SEATED_NONE): it holds no base, an island again;
   (d) an amphibious K-bot at home, class 2 crossing the channel: the land army reaches the east bank, no island;
   (e) an extractor's ghost on the west bank, a land enemy: no island;
   (f) the content's raider, tank and scout amphibious, so its land classes link the banks and the map is not
       naval, while the seat's land army, one K-bot of class 0, does not cross: no island off a naval map;
   (g) random starts with the third on the west bank, the enemy's plant known on the east: its occupant unknown,
       that start is no evidence once enemy structures are known, and the rows make an island;
   (h) the west bank's third start seated by a foe on a cliff block, 120 above the sea, which class 0 does not
       stand on: the land round it within IMPAI_REACH_RINGS is the land's, no island. */
static void island_case(const char* name, int west, int amph, int ghost, int want)
{
    IMPAI_MIL_OUT o;
    const int seated = west == 1 || west == 2 || west == 4;
    int k, from;
    mil_world();
    if (amph == 1) {
        s_ti[M_AA].mclass = 2;
        s_w.defs[M_AA].d.mclass = 2;
    }
    if (amph == 2) {
        s_ti[M_RAIDER].mclass = s_ti[M_TANK].mclass = s_ti[M_SCOUT].mclass = 2;
        s_w.defs[M_RAIDER].d.mclass = s_w.defs[M_TANK].d.mclass = s_w.defs[M_SCOUT].d.mclass = 2;
    }
    water_world(W_STRAIT);
    if (west == 4) water_add(&s_w, 100, 100, 101, 101, -120);
    mil_sea_types();
    fake_reach(&s_mreach, M_N);
    s_mil_reach = &s_mreach;
    s_w.gametime = (int)IMPAI_DRY_T + 1000;
    s_mil.land_t = (uint32_t)s_w.gametime;
    s_w.starts[0] = (WQ_START){ 328, 328, 0, seated ? 1 : 0 };
    s_w.starts[1] = (WQ_START){ 6416, 3216, 1, seated ? 2 : 0 };
    s_w.nstarts = 2;
    if (west) {
        s_w.starts[2] = (WQ_START){ 1616, 1616, 2, !seated ? 0 : west == 2 ? WQ_SEATED_NONE : 3 };
        s_w.nstarts = 3;
    }
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    if (amph) own(30, M_AA, 24, 24);
    if (ghost) enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    enemy(1301, T_FACTORY, 280, 100, IMPAI_GHOST, 1);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the island's decision runs: %s", name);
    mil_print(name, &o, from);
    printf("the island, %s: dry=%u naval=%d sea waves %u\n", name, s_mil.dry, s_mil.naval, o.waves[TASIM_LAYER_SEA]);
    CHECK(!s_mil.dry && s_mil.naval == (amph == 2 ? 0 : 1) && o.waves[TASIM_LAYER_SEA] == (unsigned)want,
          "%s: naval %d, %u sea waves, want %d", name, s_mil.naval, o.waves[TASIM_LAYER_SEA], want);
    CHECK(s_mil.island == (want != 0), "%s: the island %u", name, s_mil.island);
    if (want) {
        /* a decision with no anchor judges nothing, and leaves no island standing */
        const int ax = s_st.ax;
        s_st.ax = IMPAI_NO_ANCHOR;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0 && !s_mil.island, "%s, then no anchor: the island %u", name,
              s_mil.island);
        s_st.ax = ax;
    }
    s_mil_reach = NULL;
}

static void check_island(void)
{
    island_case("(a) an island", 0, 0, 0, 1);
    island_case("(b) a foe's start on the west bank", 1, 0, 0, 0);
    island_case("(c) the west bank's start left empty", 2, 0, 0, 1);
    island_case("(d) an amphibious K-bot at home", 0, 1, 0, 0);
    island_case("(e) a land enemy's ghost on the west bank", 0, 0, 1, 0);
    island_case("(f) the content's land classes link the banks, not naval", 0, 2, 0, 0);
    island_case("(g) random starts, the third on the west bank, the enemy's plant seen", 3, 0, 0, 1);
    island_case("(h) a foe's start on a cliff on the west bank", 4, 0, 0, 0);
}

/* THE SCOUT'S LAYER (impai_mil.c, scouting) on the strait world: the seat's start (328, 328) on the west bank, a foe's
   (4 656, 1 616) on the east bank 10 blocks past the channel, so a ship's substitute for it lies on the channel's east
   edge, block (135, 50); two submarines in the channel, an extractor's ghost on the west bank (no island).
   (a) no land army: the scout is a submarine, a SCOUT squad of the sea layer sent to the start; moved to the
       substitute, it has arrived, and its next leg is the sweep's;
   (b) a scout K-bot at home besides, and a foe's start on the west bank it reaches: the land scout goes first;
   (c) the foe's start at the centre of an islet in the channel, blocks 125..129 x 50..54 but the south-east corner:
       the leg's spread point, block (123, 48), is water the submarine reaches, so travel sends it there, while the
       start's own substitute, the corner (129, 54), lies 271 px from it, past ARRIVE_PX; at the spread point the
       scout has arrived, as THE ARRIVAL measures it. */
/* THE LAYER'S LAST HEALTHY UNIT (impai_mil.c, scout_pick) on check_scout_layer's strait world, its start across
   the channel, no land army:
   (a) two submarines, the second at 20 % of its health, an extractor's ghost known: the scout is the damaged one;
   (b) the healthy one alone, the ghost known: no scout, the layer's only healthy unit kept for its wave;
   (c) the healthy one alone, nothing known: the rule holds nothing -- it goes, here as the island's wave of one
       to the start (no land enemy known, so the seat is on an island);
   (d) (b) with a healthy destroyer of the sea scout class in an ATTACK squad: the scout class is not counted among
       the healthy, so the submarine is still kept. */
static void check_scout_healthy(void)
{
    static const char* const NAME[4] = { "(a) a damaged submarine beside a healthy one", "(b) one healthy, the enemy known",
                                         "(c) one healthy, nothing known", "(d) (b) and a scout-class ship away" };
    IMPAI_MIL_OUT o;
    int c, k, s, sub;
    for (c = 0; c < 4; c++) {
        mil_world();
        mil_water(W_STRAIT);
        s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
        s_w.starts[1] = (WQ_START){ 4656, 1616, 1, 0 };
        s_w.nstarts = 2;
        for (k = 0; k < (c ? 1 : 2); k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
        if (c == 3) {
            s_ti[M_DEST].klass = IMPAI_K_SSCOUT;
            own(41, M_DEST, 250, 40);
            s_job[41].kind = IMPAI_JOB_SQUAD;
            s_job[41].type = M_DEST;
            s_job[41].squad = 1;
            s_mil.sq[1].task = IMPAI_SQ_ATTACK;
            s_mil.sq[1].layer = TASIM_LAYER_SEA;
            s_mil.sq[1].target = (uint16_t)WQ_NONE;
            s_mil.sq[1].tx = 145;
            s_mil.sq[1].tz = 50;
            s_mil.sq[1].since = (uint32_t)s_w.gametime;
            s_mil.sq[1].counted = 1;
        }
        if (!c) s_w.unit[41].hp = (int)s_w.defs[M_SUB].hp / 5;
        if (c != 2) enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the last healthy unit's decision runs");
        for (s = -1, k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
        for (sub = -1, k = 40; k < 42; k++) if (s >= 0 && s_job[k].kind == IMPAI_JOB_SQUAD && s_job[k].squad == s) sub = k;
        printf("the layer's last healthy unit, %s: scouts %u, the scout %d, attacks %u\n", NAME[c], o.scouts, sub,
               o.attacks);
        if (c == 0) CHECK(o.scouts == 1 && sub == 41, "%s: the damaged one scouts, %d", NAME[c], sub);
        if (c == 1 || c == 3) CHECK(!o.scouts && sub < 0, "%s: no scout", NAME[c]);
        if (c == 2) CHECK(o.scouts + o.attacks == 1, "%s: it goes, %u scouts, %u attacks", NAME[c], o.scouts, o.attacks);
    }
}

/* THE SEA SCOUT (impai_mil.c, scout_pick): check_scout_healthy's (a) world with a submarine and a destroyer of the
   sea scout class, both healthy and as fast: the scout is the destroyer, its layer's scout class, though the
   submarine is the cheaper. */
static void check_scout_sea_class(void)
{
    IMPAI_MIL_OUT o;
    int k, s = -1, who = -1;
    mil_world();
    mil_water(W_STRAIT);
    s_ti[M_DEST].klass = IMPAI_K_SSCOUT;
    s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
    s_w.starts[1] = (WQ_START){ 4656, 1616, 1, 0 };
    s_w.nstarts = 2;
    own(40, M_SUB, 244, 40);
    own(41, M_DEST, 246, 40);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the sea scout class's decision runs");
    for (k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    for (k = 40; k < 42; k++) if (s >= 0 && s_job[k].kind == IMPAI_JOB_SQUAD && s_job[k].squad == s) who = k;
    printf("the sea scout class: scouts %u, the scout %d\n", o.scouts, who);
    CHECK(o.scouts == 1 && who == 41, "the sea scout class scouts, %d", who);
}

/* THE BLIND SPOT and THE AIR SCOUT'S TRIP (impai_mil.c, blind_spot, air_scout) on check_scout_layer's strait world:
   the channel's east edge at block 135, a foe's start on the east bank on row 50, two submarines in the channel (a
   ship's substitute for the start: block (135, 50)), an extractor's ghost on the west bank -- the estimate -- and a
   tank beside it, and an air scout -- M_SCOUT as a VTOL of the air scout's class, sight 300 -- at home:
   (a) the submarines' sight 0, the start 300 px past the edge: the blind spot stamped, the air scout sent to the
       start as a SCOUT squad beside the sea scout, and both still out at the next decision (ONE SCOUT SQUAD besides
       the air scout's trip);
   (b) their sight 400, the start 5 blocks past the edge along the axis: still blind -- THE EYES' DISC holds sight
       to 8 blocks and THE SHEAR's bound takes 4 off;
   (c) their sight 400, the start 4 blocks past the edge: in their eyes -- no blind spot, no trip;
   (d) the foe's start on the west bank, which the tank reaches: no blind spot;
   (e) (c) without the tank: the estimate -- the ghost on the west bank -- in no unit's sight: the trip goes there;
   (f) the air scout alone, the enemy known: it goes;
   (g) (a) with no air scout but an armed aircraft at home: the blind spot stamped, the aircraft sent nowhere;
   (h) RE-FLY ONLY WHEN NEEDED, (a) with a second blind start at (4 636, 3 600) and the air scout over the first:
       the first is eyed, so the trip goes to the second; over it, the trip turns home; at the rally point it is
       let go of, in no squad; within IMPAI_EYED_REFRESH no trip; past it, a trip again;
   (i) THE AIR SCOUT IS SCOUT ONLY: (c)'s world with the air scout in the air layer's FORM squad, as a recording
       made before the rule holds it: it is let go of and the squad freed;
   (j) (h)'s two blind starts with the air scout 600 px north of the second: the trip goes to the second, the
       nearer, not the first by its index;
   (k) THE SHEAR: (h) with the air scout 6 blocks south of the first start: an aircraft's stamp may stand 4 rows
       north of it, so the first is not eyed and the trip goes there; 4 blocks south of it: eyed, the trip goes to
       the second;
   (l) THE LEG TRIED: (a)'s one blind start, the air scout sent and never moved: past the leg's lifetime it turns
       home, and at the next decision it is still on its way home, not out to the same start again;
   (m) a dead air scout's SCOUT squad (squad 0, no member) and a sea scout's (squad 1) on its leg: the sea scout's
       squad stays a SCOUT squad on that leg, not sent back to FORM (and out again on a new trip). */
static void ascout_world(int start_x, int sub_sight)
{
    mil_world();
    mil_water(W_STRAIT);
    s_ti[M_SCOUT].klass = IMPAI_K_ASCOUT;
    s_ti[M_SCOUT].flags |= IMPAI_TF_VTOL;
    s_ti[M_SCOUT].sight = 300;
    s_ti[M_SUB].sight = (uint16_t)sub_sight;
    s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
    s_w.starts[1] = (WQ_START){ start_x, 1616, 1, 0 };
    s_w.nstarts = 2;
}

/* the squad of the air scout at slot 42 when a SCOUT squad holds it, else -1 */
static int ascout_sq(void)
{
    const int s = s_job[42].kind == IMPAI_JOB_SQUAD && s_job[42].squad < IMPAI_SQUADS ? s_job[42].squad : -1;
    return s >= 0 && s_mil.sq[s].task == IMPAI_SQ_SCOUT ? s : -1;
}

static int sea_scout_sq(void)
{
    int k;
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (s_mil.sq[k].task == IMPAI_SQ_SCOUT && s_mil.sq[k].layer == TASIM_LAYER_SEA) return k;
    return -1;
}

static void ascout_next(IMPAI_MIL_OUT* o, uint32_t ticks)
{
    s_w.gametime += (int)ticks;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, o) == 0, "the air scout's next decision runs");
}

static void check_air_scout_trip(void)
{
    static const char* const NAME[5] = { "(a) out of the ships' sight", "(b) 5 blocks past the edge",
                                         "(c) 4 blocks past the edge", "(d) a land start the tank reaches",
                                         "(e) the estimate out of every sight" };
    static const int START_X[5] = { 4636, 4496, 4464, 1616, 4464 };
    const uint32_t look = IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    IMPAI_MIL_OUT o;
    int c, k, s;
    for (c = 0; c < 5; c++) {
        ascout_world(START_X[c], c == 0 ? 0 : 400);
        for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
        own(42, M_SCOUT, 22, 22);
        if (c < 4) own(7, M_TANK, 96, 96);
        enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the air scout's decision runs");
        s = ascout_sq();
        printf("the air scout's trip, %s: blind_t %u, its squad %d, leg (%d, %d), scouts %u\n", NAME[c],
               s_mil.blind_t, s, s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1, s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1,
               o.scouts);
        if (c < 2) {
            CHECK(s_mil.blind_t == (uint32_t)s_w.gametime && s >= 0 && s_mil.sq[s].layer == TASIM_LAYER_AIR &&
                      s_mil.sq[s].tx == START_X[c] / 32 && s_mil.sq[s].tz == 1616 / 32 && o.scouts == 2,
                  "%s: the blind spot stamped and the air scout sent there beside the sea scout", NAME[c]);
            if (!c) {
                ascout_next(&o, look);
                CHECK(ascout_sq() == s && sea_scout_sq() >= 0, "%s: both scouts still out at the next decision",
                      NAME[c]);
            }
        } else if (c == 4) {
            CHECK(s_mil.blind_t == (uint32_t)s_w.gametime && s >= 0 && s_mil.sq[s].tx == 1616 / 32 &&
                      s_mil.sq[s].tz == 1616 / 32,
                  "%s: the air scout sent to the estimate", NAME[c]);
        } else {
            CHECK(!s_mil.blind_t && s < 0, "%s: no blind spot, no air trip", NAME[c]);
        }
    }
    ascout_world(4636, 0);
    own(42, M_SCOUT, 22, 22);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the lone air scout's decision runs");
    printf("the air scout's trip, (f) alone, the enemy known: its squad %d\n", ascout_sq());
    CHECK(ascout_sq() >= 0, "(f) the lone air scout goes");

    ascout_world(4636, 0);
    s_ti[M_SCOUT].klass = IMPAI_K_GUNSHIP;
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    own(42, M_SCOUT, 22, 22);
    own(7, M_TANK, 96, 96);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(g) the gunship's decision runs");
    printf("the air scout's trip, (g) an armed aircraft, no air scout: blind_t %u, its squad %d task %u\n",
           s_mil.blind_t, s_job[42].squad, s_job[42].squad < IMPAI_SQUADS ? s_mil.sq[s_job[42].squad].task : 99);
    CHECK(s_mil.blind_t == (uint32_t)s_w.gametime && s_job[42].kind == IMPAI_JOB_SQUAD &&
              s_mil.sq[s_job[42].squad].task == IMPAI_SQ_FORM,
          "(g) the blind spot stamped, the armed aircraft kept in FORM");

    ascout_world(4636, 0);
    s_w.starts[2] = (WQ_START){ 4636, 3600, 2, 0 };
    s_w.nstarts = 3;
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    own(42, M_SCOUT, 22, 22);
    s_w.unit[42].pos[0] = 4636 << 16;
    s_w.unit[42].pos[2] = 1616 << 16;
    own(7, M_TANK, 96, 96);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(h) the first decision runs");
    s = ascout_sq();
    printf("the air scout's trip, (h) over the first start: its squad %d, leg (%d, %d)\n", s,
           s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1, s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1);
    CHECK(s >= 0 && s_mil.sq[s].tx == 4636 / 32 && s_mil.sq[s].tz == 3600 / 32,
          "(h) the eyed start needs no look: the trip goes to the second");
    s_w.unit[42].pos[2] = 3600 << 16;
    ascout_next(&o, look);
    s = ascout_sq();
    printf("the air scout's trip, (h) over the second: its squad %d, flags %u\n", s, s >= 0 ? s_mil.sq[s].flags : 0);
    CHECK(s >= 0 && s_mil.sq[s].flags == 0xFF, "(h) every area eyed: the trip turns home");
    if (s >= 0) {
        s_w.unit[42].pos[0] = (s_mil.sq[s].tx * 32 + 16) << 16;
        s_w.unit[42].pos[2] = (s_mil.sq[s].tz * 32 + 16) << 16;
    }
    ascout_next(&o, look);
    printf("the air scout's trip, (h) home: its squad %d, its job %u\n", ascout_sq(), s_job[42].kind);
    CHECK(ascout_sq() < 0 && s_job[42].kind != IMPAI_JOB_SQUAD, "(h) home: let go of, in no squad");
    ascout_next(&o, look);
    CHECK(ascout_sq() < 0, "(h) within the refresh: no trip");
    ascout_next(&o, IMPAI_EYED_REFRESH);
    printf("the air scout's trip, (h) past the refresh: its squad %d\n", ascout_sq());
    CHECK(ascout_sq() >= 0, "(h) past the refresh: a trip again");

    ascout_world(4636, 0);
    s_w.starts[2] = (WQ_START){ 4636, 3600, 2, 0 };
    s_w.nstarts = 3;
    own(42, M_SCOUT, 22, 22);
    s_w.unit[42].pos[0] = 4636 << 16;
    s_w.unit[42].pos[2] = 3000 << 16;
    own(7, M_TANK, 96, 96);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(j) the decision runs");
    s = ascout_sq();
    printf("the air scout's trip, (j) nearer the second start: leg (%d, %d)\n", s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1,
           s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1);
    CHECK(s >= 0 && s_mil.sq[s].tz == 3600 / 32, "(j) the trip goes to the nearer area");

    ascout_world(4464, 400);
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    own(42, M_SCOUT, 22, 22);
    own(7, M_TANK, 96, 96);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    s_job[42].kind = IMPAI_JOB_SQUAD;
    s_job[42].type = M_SCOUT;
    s_job[42].squad = 3;
    s_mil.sq[3].task = IMPAI_SQ_FORM;
    s_mil.sq[3].layer = TASIM_LAYER_AIR;
    s_mil.sq[3].target = (uint16_t)WQ_NONE;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(i) the decision runs");
    printf("the air scout's trip, (i) in a FORM squad: its job %u, squad 3's task %u\n", s_job[42].kind,
           s_mil.sq[3].task);
    CHECK(s_job[42].kind != IMPAI_JOB_SQUAD && s_mil.sq[3].task == IMPAI_SQ_FREE,
          "(i) the air scout let go of, the FORM squad freed");

    for (c = 0; c < 2; c++) {
        const int south = c == 0 ? 6 : 4;
        ascout_world(4636, 0);
        s_w.starts[2] = (WQ_START){ 4636, 3600, 2, 0 };
        s_w.nstarts = 3;
        for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
        own(42, M_SCOUT, 22, 22);
        s_w.unit[42].pos[0] = 4636 << 16;
        s_w.unit[42].pos[2] = (1616 + 32 * south) << 16;
        own(7, M_TANK, 96, 96);
        enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(k) the decision runs");
        s = ascout_sq();
        printf("the air scout's trip, (k) %d blocks south of the first start: leg (%d, %d)\n", south,
               s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1, s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1);
        CHECK(s >= 0 && s_mil.sq[s].tz == (c == 0 ? 1616 : 3600) / 32,
              "(k) %d blocks south: %s", south, c == 0 ? "not eyed under the shear, the trip to the first"
                                                       : "eyed, the trip to the second");
    }

    ascout_world(4636, 0);
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    own(42, M_SCOUT, 22, 22);
    own(7, M_TANK, 96, 96);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(l) the decision runs");
    s = ascout_sq();
    CHECK(s >= 0 && s_mil.sq[s].tx == 4636 / 32 && s_mil.sq[s].flags != 0xFF, "(l) the air scout sent to the start");
    ascout_next(&o, 20000u);
    s = ascout_sq();
    printf("the air scout's trip, (l) past the leg's lifetime: its squad %d, flags %u, leg (%d, %d)\n", s,
           s >= 0 ? s_mil.sq[s].flags : 0, s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1,
           s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1);
    CHECK(s >= 0 && s_mil.sq[s].flags == 0xFF, "(l) the leg outlived: the trip turns home");
    ascout_next(&o, look);
    s = ascout_sq();
    printf("the air scout's trip, (l) the next decision: its squad %d, flags %u, leg (%d, %d)\n", s,
           s >= 0 ? s_mil.sq[s].flags : 0, s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1,
           s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1);
    CHECK(s < 0 || (s_mil.sq[s].flags == 0xFF && s_mil.sq[s].tx != 4636 / 32),
          "(l) the start tried: the trip does not turn out to it again");

    ascout_world(4636, 0);
    own(41, M_DEST, 246, 40);
    s_ti[M_DEST].klass = IMPAI_K_SSCOUT;
    own(7, M_TANK, 96, 96);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    s_mil.sq[0].task = IMPAI_SQ_SCOUT;
    s_mil.sq[0].layer = TASIM_LAYER_AIR;
    s_mil.sq[0].target = (uint16_t)WQ_NONE;
    s_mil.sq[0].tx = 4636 / 32;
    s_mil.sq[0].tz = 1616 / 32;
    s_mil.sq[0].since = (uint32_t)s_w.gametime;
    s_mil.sq[1].task = IMPAI_SQ_SCOUT;
    s_mil.sq[1].layer = TASIM_LAYER_SEA;
    s_mil.sq[1].target = (uint16_t)WQ_NONE;
    s_mil.sq[1].tx = 135;
    s_mil.sq[1].tz = 50;
    s_mil.sq[1].since = (uint32_t)s_w.gametime;
    s_job[41].kind = IMPAI_JOB_SQUAD;
    s_job[41].type = M_DEST;
    s_job[41].squad = 1;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(m) the decision runs");
    printf("the air scout's trip, (m) a dead air scout's squad: squad 1's task %u, the sea scout's squad %u\n",
           s_mil.sq[1].task, s_job[41].squad);
    CHECK(s_job[41].kind == IMPAI_JOB_SQUAD && s_job[41].squad == 1 && s_mil.sq[1].task == IMPAI_SQ_SCOUT &&
              s_mil.sq[1].tx == 135,
          "(m) the sea scout's squad stays a SCOUT squad on its leg");
    s_ti[M_SUB].sight = 0;
    s_ti[M_SCOUT].sight = 0;
    s_ti[M_SCOUT].klass = IMPAI_K_LSCOUT;
    s_ti[M_SCOUT].flags &= (uint8_t)~IMPAI_TF_VTOL;
}

/* THE WATER HELD (impai_mil.c, sea_held) on the strait world: an enemy destroyer seen in the channel holds its value;
   out of sight at the next decision the value is held; past IMPAI_SEA_HOLD it is let go. */
static void check_water_held(void)
{
    const uint32_t look = IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    IMPAI_MIL_OUT o;
    float seen, held;
    mil_world();
    mil_water(W_STRAIT);
    own(7, M_TANK, 96, 96);
    enemy(1320, M_DEST, 250, 60, IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the water held's decision runs");
    seen = s_mil.sea_held;
    s_w.unit[1320].live = 0;
    memset(&s_rows[1320], 0, sizeof s_rows[1320]);
    s_w.gametime += (int)look;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the water held's second decision runs");
    held = s_mil.sea_held;
    s_w.gametime += IMPAI_SEA_HOLD + 1;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the water held's third decision runs");
    printf("the water held: seen %.1f, held %.1f, let go %.1f\n", seen, held, s_mil.sea_held);
    CHECK(seen > 0.0f && held == seen && s_mil.sea_held == 0.0f, "the destroyer's value seen, held, let go");
}

static void check_scout_layer(void)
{
    IMPAI_MIL_OUT o;
    int k, s = -1, sub = -1;
    mil_world();
    mil_water(W_STRAIT);
    s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
    s_w.starts[1] = (WQ_START){ 4656, 1616, 1, 0 };
    s_w.nstarts = 2;
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);   /* a land enemy known: no island, so no wave takes the ships */
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the sea scout's decision runs");
    for (k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    for (k = 40; k < 42; k++) if (s >= 0 && s_job[k].kind == IMPAI_JOB_SQUAD && s_job[k].squad == s) sub = k;
    printf("the scout's layer (a): scouts %u, squad %d layer %u leg (%d, %d), the submarine %d\n", o.scouts, s,
           s >= 0 ? s_mil.sq[s].layer : 99, s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1, s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1,
           sub);
    CHECK(o.scouts == 1 && s >= 0 && s_mil.sq[s].layer == TASIM_LAYER_SEA && sub >= 0 &&
              s_mil.sq[s].tx == 4656 / 32 && s_mil.sq[s].tz == 1616 / 32,
          "(a) no land army: a submarine scouts the start across the channel");
    if (sub >= 0) {
        s_w.unit[sub].pos[0] = (135 * 32 + 16) << 16;
        s_w.unit[sub].pos[2] = (50 * 32 + 16) << 16;
    }
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the sea scout's second decision runs");
    printf("the scout's layer (a), at the substitute: task %u legs %u leg (%d, %d)\n", s >= 0 ? s_mil.sq[s].task : 99,
           s >= 0 ? s_mil.sq[s].flags : 99, s >= 0 ? s_mil.sq[s].tx * 32 + 16 : -1, s >= 0 ? s_mil.sq[s].tz * 32 + 16 : -1);
    CHECK(s >= 0 && s_mil.sq[s].task == IMPAI_SQ_SCOUT && s_mil.sq[s].flags == 1 &&
              !(s_mil.sq[s].tx == 4656 / 32 && s_mil.sq[s].tz == 1616 / 32),
          "(a) at the substitute the scout has arrived: its next leg is the sweep's");
    mil_world();
    mil_water(W_STRAIT);
    s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
    s_w.starts[1] = (WQ_START){ 4656, 1616, 1, 0 };
    s_w.starts[2] = (WQ_START){ 1616, 1616, 2, 0 };
    s_w.nstarts = 3;
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    own(7, M_SCOUT, 30, 30);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the land scout's decision runs");
    for (s = -1, k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    printf("the scout's layer (b): scouts %u, layer %u, the K-bot's squad %d\n", o.scouts,
           s >= 0 ? s_mil.sq[s].layer : 99, s_job[7].kind == IMPAI_JOB_SQUAD ? s_job[7].squad : -1);
    CHECK(o.scouts == 1 && s >= 0 && s_mil.sq[s].layer == TASIM_LAYER_LAND && s_job[7].kind == IMPAI_JOB_SQUAD &&
              s_job[7].squad == s,
          "(b) a land scout with a start to go to goes first");
    mil_world();
    water_world(W_STRAIT);
    water_add(&s_w, 250, 100, 259, 109, -10);
    water_add(&s_w, 258, 108, 259, 109, 40);
    mil_sea_types();
    fake_reach(&s_mreach, M_N);
    s_mil_reach = &s_mreach;
    s_w.starts[0] = (WQ_START){ 328, 328, 0, 0 };
    s_w.starts[1] = (WQ_START){ 127 * 32 + 16, 52 * 32 + 16, 1, 0 };
    s_w.nstarts = 2;
    for (k = 0; k < 2; k++) own(40 + k, M_SUB, 244 + 2 * k, 40);
    enemy(1310, T_MEX, 100, 100, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the islet's scout decision runs");
    for (s = -1, k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_SCOUT) s = k;
    for (sub = -1, k = 40; k < 42; k++) if (s >= 0 && s_job[k].kind == IMPAI_JOB_SQUAD && s_job[k].squad == s) sub = k;
    CHECK(o.scouts == 1 && s >= 0 && sub >= 0 && s_mil.sq[s].tx == 127 && s_mil.sq[s].tz == 52,
          "(c) a submarine scouts the islet's start");
    if (sub >= 0) {
        s_w.unit[sub].pos[0] = (123 * 32 + 16) << 16;
        s_w.unit[sub].pos[2] = (48 * 32 + 16) << 16;
    }
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the islet's scout's second decision runs");
    printf("the scout's layer (c), at the spread point: task %u legs %u leg block (%d, %d)\n",
           s >= 0 ? s_mil.sq[s].task : 99, s >= 0 ? s_mil.sq[s].flags : 99, s >= 0 ? s_mil.sq[s].tx : -1,
           s >= 0 ? s_mil.sq[s].tz : -1);
    CHECK(s >= 0 && s_mil.sq[s].task == IMPAI_SQ_SCOUT && s_mil.sq[s].flags == 1 &&
              !(s_mil.sq[s].tx == 127 && s_mil.sq[s].tz == 52),
          "(c) at the spread point travel sent it to, the scout has arrived: its next leg");
    s_mil_reach = NULL;
}

/* THE SEA PLANT on the economy's types: land over x 0..63, then COAST sea 40 deep, SHALLOW 20 deep, or LAKE, a
   lake over 64..79 cut from the sea by land past the commander's reach (sea from 128); FAR, land over 0..107 and
   the sea from 108, a shipyard's centre there 1 150 px from the anchor or more, past comm_r and within
   FAR_PATCH_K x comm_r, and inside the fake's explored 128 columns; STRIP, FAR with a strip 20 deep over x
   108..115 that neither class 0 stands on nor a shipyard fits, but for a pier of land over 108..113 x 56..57 and
   an island over 114..115 x 62..63, the deep sea from 116; the commander idle at
   cell (40, 64) on class 2, which walks the sea floor as the engine's commanders' TANKDS2 walks water 100 deep
   (the move class's +0x08, tasim_run defs), a finished K-bot lab, stores full, the military's estimate at block
   (240, 32) */
enum { C_COAST, C_SHALLOW, C_LAKE, C_FAR, C_STRIP, C_SPIT, C_SPIT_NEAR };
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
    } else if (kind == C_FAR || kind == C_STRIP || kind == C_SPIT || kind == C_SPIT_NEAR) {
        water_add(&s_w, 64, 0, 107, 127, -10);
        water_add(&s_w, 108, 0, 511, 127, 40);
        if (kind != C_FAR) water_add(&s_w, 108, 0, 115, 127, 20);
        if (kind == C_STRIP) {
            water_add(&s_w, 108, 56, 113, 57, -10);
            water_add(&s_w, 114, 62, 115, 63, -10);
        }
        if (kind == C_SPIT || kind == C_SPIT_NEAR) {
            water_add(&s_w, 108, 70, 126, 71, -10);
            water_add(&s_w, 125, kind == C_SPIT ? 66 : 64, 126, 69, -10);
        }
    } else
        water_add(&s_w, 64, 0, 511, 127, kind == C_SHALLOW ? 20 : 40);
    unit_add(&s_w, 1, E_COM, 40, 64, 1);
    unit_add(&s_w, 2, E_LAB, 20, 40, 1);
    s_w.defs[E_COM].d.mclass = 2;
    s_ti[E_COM].mclass = 2;
    s_w.econ[0].metal = s_w.econ[0].energy = s_w.econ[0].metal_max = s_w.econ[0].energy_max = 1000;
    s_w.econ[0].metal_in = metal_in;
    s_w.econ[0].energy_in = 100;
    s_w.econ[0].energy_out = 50;
    fake_reach(&s_reach, E_N);
    impai_mil_reset(&s_smil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
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
    printf("sea plant %s: sy=%d at %d,%d land=%d at %d,%d sea_plants=%u sites=%u sea_refused=%u factories=%u\n", name,
           *sy, x0, z0, *land, lx, lz, out.sea_plants, out.sites, s_st.sea_refused, out.count[IMPAI_C_FACTORY]);
    CHECK(out.sites <= s_plan->site_tests, "%s: %u site tests of %u", name, out.sites, s_plan->site_tests);
}

/* THE ISLAND'S FIRST PLANT (impai_econ.c): the coast world with no factory yet, past open_t, the commander the one
   builder, the estimate in the sea; the lab and the vehicle plant make constructors only, so no plant has a land
   army:
   (a) naval: the first plant is the shipyard, by the sea plant's search;
   (b) not naval: the vehicle plant, the least value;
   (c) naval, the lab's K-bot made a land fighting type and the estimate moved ashore, which its class reaches:
       the lab, the plant with a land army;
   (d) naval, the water too shallow for a shipyard (SHALLOW): the vehicle plant in the same decision;
   (e) naval, the commander's shipyard job ended unbuilt, its site out of the commander's reach: the decision that
       finds it counts one refusal and holds the commander from plants for fail_t (THE FAR RECORD), and the first
       decision past it builds the land plant, not the shipyard again;
   (f) (c) with the lab's build tech 3, past the vehicle plant's 1, so the pick cannot take it: the land army
       it holds does not count, and the first plant is the shipyard. */
static void check_island_plant(void)
{
    int sy, land;
    coast_world(C_COAST, 1, 9);
    s_w.unit[2].live = 0;
    sea_decide("the island's first plant", &sy, &land);
    CHECK(sy == 1 && !land, "(a) an island: the first plant is the shipyard");
    coast_world(C_COAST, 0, 9);
    s_w.unit[2].live = 0;
    sea_decide("the first plant, not naval", &sy, &land);
    CHECK(!sy && land == 1, "(b) not naval: the first plant is a land one");
    coast_world(C_COAST, 1, 9);
    s_w.unit[2].live = 0;
    s_ti[E_CK].klass = IMPAI_K_LASSAULT;
    s_smil.ebx = 10;
    {
        const int from = s_w.nlog;
        sea_decide("the first plant, a land army that reaches", &sy, &land);
        CHECK(!sy && land == 1 && builds_of(from, E_LAB, NULL, NULL) == 1, "(c) a land army that reaches: the lab");
    }
    coast_world(C_SHALLOW, 1, 9);
    s_w.unit[2].live = 0;
    sea_decide("the island's first plant, no water deep enough", &sy, &land);
    CHECK(!sy && land == 1, "(d) no site for a shipyard: the land plant in the same decision");
    coast_world(C_COAST, 1, 9);
    s_w.unit[2].live = 0;
    s_job[1].type = E_COM;
    s_job[1].build = E_SY;
    s_job[1].target = (uint16_t)WQ_NONE;
    s_job[1].cx = 64;
    s_job[1].cz = 60;
    s_job[1].kind = IMPAI_JOB_BUILD;
    s_job[1].since = 1900;
    sea_decide("the island's first plant, the shipyard's job failed", &sy, &land);
    CHECK(!sy && !land && s_st.sea_refused == 1, "(e) the shipyard's job failed: one refusal, %u", s_st.sea_refused);
    s_w.gametime += (int)s_plan->fail_t + 15;
    sea_decide("the island's first plant, after the failed job", &sy, &land);
    CHECK(!sy && land == 1, "(e) after the failed shipyard job: the land plant, not a second shipyard");
    coast_world(C_COAST, 1, 9);
    s_w.unit[2].live = 0;
    s_ti[E_CK].klass = IMPAI_K_LASSAULT;
    s_smil.ebx = 10;
    s_ti[E_LAB].ctech = 3;
    s_ti[E_VP].ctech = 1;
    sea_decide("the first plant, the lab's land army past the build tech", &sy, &land);
    CHECK(sy == 1 && !land, "(f) a land army only on a plant the pick cannot take: the shipyard");
}

/* THE ISLAND'S PLANTS (impai_econ.c, island_plant): the coast world, naval, the lab gone and a finished shipyard
   at cell (66, 100), so the next plant is the seat's second:
   (a) on THE ISLAND, no candidate with a land army: a second shipyard, by the sea plant's search, and no land
       plant;
   (b) on THE ISLAND, the sea plant's refusals spent, the lab and the vehicle plant offering constructors alone:
       THE CHOICE takes no land plant, and none is ordered;
   (c) (b) with the lab's menu also an air army type (the PT boat made a bomber): the lab, an air plant;
   (d) (c) with the bomber's value 0: no army type, no plant;
   (e) not on THE ISLAND: a land plant and no shipyard, Q3 and the one sea plant unchanged on a linked map;
   (f) on THE ISLAND with no factory yet: the first plant is the shipyard (THE ISLAND'S FIRST PLANT), which the
       rule for the plants past it does not touch;
   (g) (b) with the commander and so the anchor in the sea at cell (70, 64): THE CHOICE reads the water plants
       there, and takes the shipyard, a sea plant;
   (h) (a) at a metal income of 2 a pass, below factory_per_m's pacing: no second shipyard;
   (i) on THE ISLAND, the lab's K-bot a land fighting type and the estimate ashore, which its class reaches: the
       choice's reach disagrees with the military's verdict, so a land plant by the choice's own rule and no
       shipyard;
   (j) (i) with the lab's build tech 3, past the vehicle plant's 1, so the pick cannot take it: its land army does
       not count, and the second shipyard. */
static void check_island_plants(void)
{
    static const uint16_t AIRLAB[] = { E_CK, E_PT };
    static const char* const NAME[10] = { "(a) the island, a second shipyard", "(b) the island, no sea, no air plant",
                                         "(c) the island, no sea, the lab an air plant",
                                         "(d) the island, no sea, the bomber of no value", "(e) not the island",
                                         "(f) the island, the first plant", "(g) the island, no sea, a wet anchor",
                                         "(h) the island, the income below the pacing",
                                         "(i) the island, the lab's land army reaches",
                                         "(j) the island, the reaching lab past the build tech" };
    IMPAI_BRAIN_OUT out;
    int c, sy, land;
    for (c = 0; c < 10; c++) {
        int from;
        coast_world(C_COAST, 1, c == 7 ? 2.0f : 9.0f);
        if (c >= 8) {
            s_ti[E_CK].klass = IMPAI_K_LASSAULT;
            s_smil.ebx = 10;
        }
        if (c == 9) {
            s_ti[E_LAB].ctech = 3;
            s_ti[E_VP].ctech = 1;
        }
        s_w.unit[2].live = 0;
        if (c != 5) unit_add(&s_w, 3, E_SY, 66, 100, 1);
        if (c == 6) unit_add(&s_w, 1, E_COM, 70, 64, 1);
        s_smil.island = (uint8_t)(c != 4);
        if (c == 1 || c == 2 || c == 3 || c == 6) s_st.sea_refused = IMPAI_SEA_REFUSALS;
        if (c == 2 || c == 3) {
            menu_set(&s_w, E_LAB, AIRLAB, 2);
            s_ti[E_PT].klass = IMPAI_K_BOMBER;
            if (c == 3) s_ti[E_PT].value = 0.0f;
        }
        from = s_w.nlog;
        if (c == 6) {
            CHECK(decide(&out) == 0, "decide: %s", NAME[c]);
            printf("sea plant %s: plant %u want %u\n", NAME[c], out.plant, out.plant_want);
            CHECK(out.plant == E_SY && out.plant_want == 2, "%s: the shipyard chosen", NAME[c]);
            continue;
        }
        sea_decide(NAME[c], &sy, &land);
        if (c == 0 || c == 5 || c == 9) CHECK(sy == 1 && !land, "%s: the shipyard", NAME[c]);
        if (c == 1 || c == 3 || c == 7) CHECK(!sy && !land, "%s: no plant, %d %d", NAME[c], sy, land);
        if (c == 2) CHECK(!sy && land == 1 && builds_of(from, E_LAB, NULL, NULL) == 1, "%s: the lab", NAME[c]);
        if (c == 4) CHECK(!sy && land == 1, "%s: a land plant", NAME[c]);
        if (c == 8) CHECK(!sy && land == 1, "%s: a land plant", NAME[c]);
    }
    s_milp = NULL;
}

/* THE ISLAND'S PLANTS held for the sea plant, and THE ISLAND'S AGREEMENT aimed at the foe's starts too: the coast
   world, naval, on THE ISLAND, a finished shipyard at cell (66, 100), the lab's menu its K-bot and the underwater
   extractor's type made a bomber (an air plant; the PT boat stays the shipyard's sea army), the refusals not spent:
   (a) the commander's shipyard job ended unbuilt, so its far record holds it from plants, and an idle K-bot whose
       menu holds no shipyard: the K-bot builds no air plant, the sea plant held for the commander; past fail_t
       the commander's second shipyard;
   (b) every builder's menu without the shipyard (the commander's too): no builder offers the sea plant, so the
       air plant, the lab;
   (c) the K-bot a land fighting type, the estimate on structures seen at block (240, 32) in the sea, and the
       enemy's start at (320, 320) px on the land the K-bot's class reaches: the economy's reach disagrees, so the
       land plant by the choice's own rule and no shipyard; with that start left empty, the shipyard;
   (d) (c) at random starts (both seated 0): with enemy structures known a start seated at random is no evidence,
       as the military reads it, so the shipyard. */
static void check_island_hold(void)
{
    static const uint16_t AIRLAB[] = { E_CK, E_UWMEX };
    static const uint16_t CKNOSY[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP };
    static const uint16_t COMNOSY[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB, E_VP, E_UWMEX };
    int c, sy, land;
    for (c = 0; c < 5; c++) {
        int from;
        coast_world(C_COAST, 1, 9.0f);
        s_w.unit[2].live = 0;
        unit_add(&s_w, 3, E_SY, 66, 100, 1);
        s_smil.island = 1;
        menu_set(&s_w, E_LAB, AIRLAB, 2);
        s_ti[E_UWMEX].klass = IMPAI_K_BOMBER;
        if (c == 0) {
            unit_add(&s_w, 4, E_CK, 30, 60, 1);
            menu_set(&s_w, E_CK, CKNOSY, 6);
            s_job[1].type = E_COM;
            s_job[1].build = E_SY;
            s_job[1].target = (uint16_t)WQ_NONE;
            s_job[1].cx = 64;
            s_job[1].cz = 60;
            s_job[1].kind = IMPAI_JOB_BUILD;
            s_job[1].since = 1900;
        }
        if (c == 1) menu_set(&s_w, E_COM, COMNOSY, 7);
        if (c >= 2) {
            s_ti[E_CK].klass = IMPAI_K_LASSAULT;
            s_smil.est_seen = EST_STRUCT;
            s_w.starts[0].x = 640;
            s_w.starts[0].z = 1024;
            s_w.starts[0].seated = c == 4 ? 0 : 1;
            s_w.starts[1].x = 320;
            s_w.starts[1].z = 320;
            s_w.starts[1].seated = c == 2 ? 2 : c == 4 ? 0 : WQ_SEATED_NONE;
            s_w.nstarts = 2;
        }
        from = s_w.nlog;
        sea_decide(c == 0   ? "the island's plants held, the commander far"
                   : c == 1 ? "the island's plants, no sea plant offered"
                   : c == 2 ? "the island, a foe's start the land reaches"
                   : c == 3 ? "the island, that start left empty" : "the island, random starts, structures seen",
                   &sy, &land);
        if (c == 0) {
            CHECK(!sy && !land, "(a) the commander held far: no air plant from the K-bot, %d %d", sy, land);
            s_w.gametime += (int)s_plan->fail_t + 15;
            sea_decide("the island's plants held, past fail_t", &sy, &land);
            CHECK(sy == 1 && !land, "(a) past fail_t: the commander's second shipyard, %d %d", sy, land);
        }
        if (c == 1)
            CHECK(!sy && land == 1 && builds_of(from, E_LAB, NULL, NULL) == 1, "(b) no sea plant offered: the lab");
        if (c == 2) CHECK(!sy && land == 1, "(c) a foe's start the land reaches: a land plant, %d %d", sy, land);
        if (c == 3) CHECK(sy == 1 && !land, "(c) that start left empty: the shipyard, %d %d", sy, land);
        if (c == 4) CHECK(sy == 1 && !land, "(d) random starts, structures seen: the shipyard, %d %d", sy, land);
    }
    s_milp = NULL;
}

/* THE ISLAND'S PLANT ORDER (impai_econ.c): the coast world, naval, its land 32 x 64 blocks round the anchor, so the
   island is big by its area; ISLE_LAND, eight 3 x 3 patches on the land, ISLE_SMALL three of them, ISLE_WET ten
   in the sea 40 deep, where the shipyard's CORCS and its underwater extractor reach them.
   The first plant (no factory yet):
   (a) eight land patches, none at sea: a land plant first, no shipyard;
   (b) the eight and ten at sea: the sea's metal is more, the shipyard;
   (c) (a) with an enemy ship known (THE TARGETS KNOWN, FLOAT): sea defences, the shipyard;
   (d) three land patches: the island too small, the shipyard;
   (e) (d) in the shallow world, no site for a shipyard, the vehicle plant an air plant (its menu offers a bomber)
       and the cheapest: never air first, the lab.
   The plants past it, on THE ISLAND, the K-bot lab's menu offering a bomber (an air plant):
   (f) three land patches, one shipyard: the second shipyard;
   (g) two shipyards: the air plant, the lab;
   (h) two shipyards and an air plant already, no patch open, factory_max 8 and the incomes 30 and 1 000 so a
       fourth plant is wanted: the third shipyard;
   (i) eight land patches, one shipyard and no land plant: a land plant, the vehicle plant (the lab is an air
       plant), no shipyard;
   (j) (i) with a vehicle plant already, as (h)'s pacing: the land plant had, the shipyard;
   (k) the first plant on an island of 16 x 16 blocks (cells 24..55 x 48..79, the rest sea) holding eight patches:
       too small by its area, the shipyard;
   (l) (k) with the land plants' constructors on class 2, which walks the sea floor (an amphibious or hover
       constructor's shape), and the content's land army on class 0 (the summary's land_w, set): the island is
       class 0's, still too small, the shipyard -- measured by class 2's component, the whole map, it read big;
   (m) eight land patches and eight at sea: a tie is the sea's, the shipyard;
   (n) (b) with a K-bot and a vehicle constructor in the census before the commander, each offering the
       extractor and the lab 15 times, the commander's menu the 31st to the 33rd plant offered: each plant is read
       once, so the commander's shipyard is, and the sea's metal is more, the shipyard;
   (o) (b) with the content's land army on class 0 and the lab offering the shipyard's constructor besides its
       K-bot: that constructor is measured on each side, the island's patches for the lab and the sea's for the
       shipyard, so the sea's metal is more, the shipyard.
   THE AIR PLANT FOR THE BLIND, on (f)'s world (one shipyard, the lab an air plant):
   (p) an enemy area in need of the air scout (IMPAI_MIL.blind_t now), a PT boat of the seat's and no enemy sea
       army seen, no prediction lost, the income 9 against factory_per_m 8: the air plant, the lab, before the
       second shipyard;
   (q) (p) with an enemy sea army of twice the PT boat's value seen lately (IMPAI_MIL.sea_held): the water not held,
       the second shipyard;
   (r) (p) with an enemy sea army of the PT boat's value seen lately: not 1.5 times it, the second shipyard;
   (s) (p) without the PT boat: no sea army, the second shipyard;
   (t) (p) on an income of 7: the economy does not pay the air plant early, the second shipyard;
   (u) (p) with no area in need of the air scout: the second shipyard;
   (v) (p) with a vehicle plant in place of the shipyard: never ahead of the first sea plant, the shipyard;
   (w) (p) with the commander, the one builder offering the lab, held from plants by its far record (its shipyard
       job ended unbuilt) and an idle sea constructor: the air plant waits for no builder, the CS's shipyard;
   (x) (p) with the anchor at the head of a channel 40 deep to the sea and the ten patches at sea (a wet anchor,
       and the sea's metal in the CS's reach, where the pick offers the shipyard too) and the lab's value above the
       shipyard's, so the shipyard out-ranks it: the air plant is the pick all the same, the lab;
   (y) (w) with the lab's and the shipyard's plant refusals live, so the plant is the waived pass's: it waits for
       no builder either, the CS's shipyard;
   (z) THE AIR PLANT TAKES THE ENERGY BUILDER: (p) with the commander, the one builder offering the lab, busy on a
       wind generator's build ordered earlier, and an idle sea constructor: the commander's lab, its job the lab's;
   (aa) (z) in an energy stall: the air plant for the blind does not hold, the commander keeps its wind generator,
       no lab;
   (ab) (z) with the commander on an extractor's build: only an energy build is left for it, so the air plant waits
       for no builder, the CS's shipyard, and the commander keeps its extractor;
   (ac) (z) with no site anywhere for the lab: the take finds none, so the air plant waits for no site either -- the
       CS's shipyard in the same decision, and the commander keeps its wind generator; THE AIR PLANT'S OWN SEARCH
       spends its own site tests, so the sea plant's search still has the decision's;
   (ad) (p) with the commander idle and an idle CS, and no site anywhere for the lab: the idle commander offers the
       lab and places it nowhere, so the air plant is let go -- never neither --, the shipyard in the same decision;
   (ae) THE AIR PLANT'S OWN SEARCH: (z) with the lab refused wherever its first cell lies at column 52 or east of it,
       which holds every land site of the base ring at its first size: the commander's lab outside it, west of 52;
   (af) (ae) with the commander offering no lab and a K-bot constructor, east of a channel 40 deep over columns
       54..55, on a wind generator's build: the sites outside the base ring lie across the channel, which the K-bot's
       class does not cross (builder_reaches), so no lab, and the CS's shipyard;
   (ag) (ae) with an idle K-bot constructor offering the lab on an islet of its own at sea, asked before the
       commander (an idle builder): it reaches no site, so its search ends on its own reach refusals and spends none
       of the decision's site tests -- the commander's lab outside the base ring, west of 52, as (ae). */
static const SNAP_PATCH ISLE_SMALL_F[] = {
    { 26, 50, 26, 50, 28, 52, 9 }, { 34, 50, 34, 50, 36, 52, 9 }, { 46, 50, 46, 50, 48, 52, 9 },
    { 52, 58, 52, 58, 54, 60, 9 }, { 26, 70, 26, 70, 28, 72, 9 }, { 34, 76, 34, 76, 36, 78, 9 },
    { 46, 74, 46, 74, 48, 76, 9 }, { 52, 70, 52, 70, 54, 72, 9 },
};
static const SNAP_PATCH ISLE_PATCHES_F[] = {
    { 10, 10, 10, 10, 12, 12, 9 },  { 10, 40, 10, 40, 12, 42, 9 },  { 10, 90, 10, 90, 12, 92, 9 },
    { 30, 15, 30, 15, 32, 17, 9 },  { 30, 110, 30, 110, 32, 112, 9 }, { 50, 20, 50, 20, 52, 22, 9 },
    { 50, 100, 50, 100, 52, 102, 9 }, { 20, 120, 20, 120, 22, 122, 9 },
    { 80, 10, 80, 10, 82, 12, 9 },  { 80, 40, 80, 40, 82, 42, 9 },  { 80, 90, 80, 90, 82, 92, 9 },
    { 90, 20, 90, 20, 92, 22, 9 },  { 90, 110, 90, 110, 92, 112, 9 }, { 100, 30, 100, 30, 102, 32, 9 },
    { 100, 100, 100, 100, 102, 102, 9 }, { 110, 10, 110, 10, 112, 12, 9 }, { 110, 60, 110, 60, 112, 62, 9 },
    { 120, 120, 120, 120, 122, 122, 9 },
};
enum { ISLE_LAND_N = 8, ISLE_WET_N = 10 };
static void isle_patches(int land, int wet)
{
    static SNAP_PATCH p[ISLE_LAND_N + ISLE_WET_N];
    int i, n = 0, x, z;
    for (i = 0; i < land; i++) p[n++] = ISLE_PATCHES_F[i];
    for (i = 0; i < wet; i++) p[n++] = ISLE_PATCHES_F[ISLE_LAND_N + i];
    for (i = 0; i < n; i++)
        for (z = p[i].z0; z <= p[i].z1; z++)
            for (x = p[i].x0; x <= p[i].x1; x++) s_w.metal[z][x] = 200;
    s_spots = p;
    s_nspots = (unsigned)n;
    s_scanned = 1;
}

static void check_island_order(void)
{
    static const uint16_t AIRVP[] = { E_CV, E_UWMEX }, AIRLAB[] = { E_CK, E_UWMEX };
    static const char* const NAME[33] = { "(a) eight land patches", "(b) the sea's metal more",
                                         "(c) an enemy ship known", "(d) three land patches",
                                         "(e) no shipyard site, the vehicle plant of air",
                                         "(f) one shipyard", "(g) two shipyards", "(h) two shipyards and an air plant",
                                         "(i) eight land patches, one shipyard",
                                         "(j) eight land patches, one shipyard and a land plant",
                                         "(k) eight patches on a small island",
                                         "(l) a small island, amphibious constructors",
                                         "(m) eight land patches, eight at sea",
                                         "(n) the sea's metal more, the lab offered 30 times first",
                                         "(o) the sea's metal more, the lab offering the ship constructor",
                                         "(p) blind, the water held", "(q) blind, an enemy navy seen",
                                         "(r) blind, an equal navy seen", "(s) blind, no sea army",
                                         "(t) blind, the income short", "(u) not blind, the water held",
                                         "(v) blind, a land plant and no shipyard",
                                         "(w) blind, the commander held, a CS idle",
                                         "(x) blind, the shipyard out-ranks the lab",
                                         "(y) blind, the commander held, a CS idle, both plants refused",
                                         "(z) blind, the commander on a wind generator, a CS idle",
                                         "(aa) blind, the commander on a wind generator, an energy stall",
                                         "(ab) blind, the commander on an extractor, a CS idle",
                                         "(ac) blind, the commander on a wind generator, no lab site",
                                         "(ad) blind, the commander idle, no lab site",
                                         "(ae) blind, the commander on a wind generator, no lab site in the base ring",
                                         "(af) blind, a K-bot on a wind generator, the outer sites across water",
                                         "(ag) blind, an idle K-bot on an islet, the commander on a wind generator" };
    static IMPAI_PLAN plan;
    IMPAI_BRAIN_OUT out;
    int c, sy, land;
    plan = IMPAI_PLAN_ROW;
    plan.factory_max = 8;
    for (c = 0; c < 33; c++) {
        int from, vp, lab;
        const int blind = c >= 15;
        coast_world(c == 4 ? C_SHALLOW : C_COAST, 1, c == 7 || c == 9 ? 30.0f : c == 19 ? 7.0f : 9.0f);
        s_w.unit[2].live = 0;
        s_plan = c == 7 || c == 9 ? &plan : &IMPAI_PLAN_ROW;
        if (c == 11) {
            s_w.defs[E_CK].d.mclass = s_w.defs[E_CV].d.mclass = 2;
            s_ti[E_CK].mclass = s_ti[E_CV].mclass = 2;
        }
        if (c == 13) {
            static const uint16_t LABS[16] = { E_MEX, E_LAB, E_LAB, E_LAB, E_LAB, E_LAB, E_LAB, E_LAB,
                                               E_LAB, E_LAB, E_LAB, E_LAB, E_LAB, E_LAB, E_LAB, E_LAB };
            menu_set(&s_w, E_CK, LABS, 16);
            menu_set(&s_w, E_CV, LABS, 16);
            unit_add(&s_w, 1, E_CK, 30, 60, 1);
            unit_add(&s_w, 4, E_CV, 34, 60, 1);
            unit_add(&s_w, 6, E_COM, 40, 64, 1);
        }
        if (c == 14) {
            static const uint16_t LABCS[] = { E_CK, E_CS };
            menu_set(&s_w, E_LAB, LABCS, 2);
            s_reach.land_w[0] = 1;
        }
        if (c == 10 || c == 11) {
            water_add(&s_w, 0, 0, 63, 127, 40);
            water_add(&s_w, 24, 48, 55, 79, -10);
            fake_reach(&s_reach, E_N);
            if (c == 11) s_reach.land_w[0] = 1;
            s_spots = ISLE_SMALL_F;
            s_nspots = (unsigned)(sizeof ISLE_SMALL_F / sizeof ISLE_SMALL_F[0]);
        } else
            isle_patches(c == 0 || c == 1 || c == 2 || c == 8 || c == 9 || (c >= 12 && !blind) ? ISLE_LAND_N
                         : c == 7                                                          ? 0
                                                                                           : 3,
                         c == 1 || (c >= 13 && !blind) || c == 23 ? ISLE_WET_N : c == 12 ? ISLE_LAND_N : 0);
        s_ti[E_UWMEX].klass = IMPAI_K_BOMBER;
        if (c == 2) s_smil.tg_t[IMPAI_TG_FLOAT] = (uint32_t)s_w.gametime;
        if (c == 4) menu_set(&s_w, E_VP, AIRVP, 2);
        if ((c >= 5 && c <= 9) || blind) {
            menu_set(&s_w, E_LAB, AIRLAB, 2);
            s_smil.island = 1;
            unit_add(&s_w, 3, c == 21 ? E_VP : E_SY, c == 21 ? 30 : 66, c == 21 ? 20 : 100, 1);
            if (c == 6 || c == 7) unit_add(&s_w, 4, E_SY, 100, 100, 1);
            if (c == 7) unit_add(&s_w, 5, E_LAB, 30, 20, 1);
            if (c == 9) unit_add(&s_w, 5, E_VP, 30, 20, 1);
            s_w.econ[0].energy_in = 1000;
        }
        if (blind) {
            s_smil.blind_t = c == 20 ? 0u : (uint32_t)s_w.gametime;
            if (c != 18) unit_add(&s_w, 8, E_PT, 130, 100, 1);
            if (c == 23) {
                water_add(&s_w, 38, 62, 63, 66, 40);
                fake_reach(&s_reach, E_N);
                s_ti[E_LAB].value = s_ti[E_SY].value + 1000.0f;
            }
            if (c == 24) {
                s_pref[E_LAB].until = s_pref[E_SY].until = (uint32_t)s_w.gametime + 1000u;
            }
            if (c == 29) {
                unit_add(&s_w, 9, E_CS, 130, 104, 1);
                s_w.refuse_type = E_LAB;
            }
            if (c >= 25 && c != 29) {
                unit_add(&s_w, 9, E_CS, 130, 104, 1);
                s_w.unit[1].list[0].action = A_MOBILEBUILD;
                s_w.unit[1].list[0].type = c == 27 ? E_MEX : E_WIND;
                s_w.unit[1].list[0].pos[0] = 50 << 20;
                s_w.unit[1].list[0].pos[2] = 60 << 20;
                s_w.unit[1].n = 1;
                s_job[1].type = E_COM;
                s_job[1].build = c == 27 ? E_MEX : E_WIND;
                s_job[1].target = (uint16_t)WQ_NONE;
                s_job[1].cx = 50;
                s_job[1].cz = 60;
                s_job[1].kind = IMPAI_JOB_BUILD;
                s_job[1].since = (uint32_t)s_w.gametime - 10u;
                if (c == 26) s_w.econ[0].energy = 0;
                if (c == 28) s_w.refuse_type = E_LAB;
                if (c >= 30) {
                    s_w.refuse_box_type = E_LAB;
                    s_w.refuse_box.x0 = 52;
                    s_w.refuse_box.x1 = 511;
                    s_w.refuse_box.z0 = 0;
                    s_w.refuse_box.z1 = 127;
                }
                if (c == 32) {
                    static const uint16_t CKLAB[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB };
                    menu_set(&s_w, E_CK, CKLAB, 5);
                    water_add(&s_w, 200, 60, 203, 63, -10);
                    fake_reach(&s_reach, E_N);
                    unit_add(&s_w, 4, E_CK, 200, 60, 1);
                }
                if (c == 31) {
                    static const uint16_t COMNOLAB[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_VP, E_UWMEX };
                    static const uint16_t CKLAB[] = { E_MEX, E_SOLAR, E_WIND, E_MAKR, E_LAB };
                    menu_set(&s_w, E_COM, COMNOLAB, 6);
                    menu_set(&s_w, E_CK, CKLAB, 5);
                    water_add(&s_w, 54, 0, 55, 127, 40);
                    fake_reach(&s_reach, E_N);
                    s_job[1].kind = IMPAI_JOB_NONE;
                    s_w.unit[1].n = 0;
                    unit_add(&s_w, 4, E_CK, 58, 64, 1);
                    s_w.unit[4].list[0] = s_w.unit[1].list[0];
                    s_w.unit[4].n = 1;
                    s_job[4] = s_job[1];
                    s_job[4].type = E_CK;
                    s_job[4].kind = IMPAI_JOB_BUILD;
                }
            }
            if (c == 22 || c == 24) {
                unit_add(&s_w, 9, E_CS, 130, 104, 1);
                s_job[1].type = E_COM;
                s_job[1].build = E_SY;
                s_job[1].target = (uint16_t)WQ_NONE;
                s_job[1].cx = 64;
                s_job[1].cz = 60;
                s_job[1].kind = IMPAI_JOB_BUILD;
                s_job[1].since = 1900;
            }
            if (c == 16 || c == 17) {
                s_smil.sea_held = (c == 16 ? 2.0f : 1.0f) * s_ti[E_PT].value;
                s_smil.sea_held_t = (uint32_t)s_w.gametime;
            }
        }
        from = s_w.nlog;
        sea_decide(NAME[c], &sy, &land);
        (void)out;
        vp = builds_of(from, E_VP, NULL, NULL);
        lab = builds_of(from, E_LAB, NULL, NULL);
        printf("the island's plant order, %s: shipyard %d, vehicle plant %d, lab %d\n", NAME[c], sy, vp, lab);
        if (c == 0) CHECK(!sy && land == 1, "%s: a land plant first", NAME[c]);
        if (c == 1 || c == 2 || c == 3 || c == 5 || c == 7 || c == 9 || (c >= 10 && c != 15 && c != 23 && c < 25))
            CHECK(sy == 1 && !land, "%s: the shipyard", NAME[c]);
        if (c == 23) CHECK(!sy && lab == 1, "%s: the air plant, the lab", NAME[c]);
        if (c == 25)
            CHECK(!sy && lab == 1 && s_job[1].kind == IMPAI_JOB_BUILD && s_job[1].build == E_LAB,
                  "%s: the commander taken off the wind generator for the lab", NAME[c]);
        if (c == 26)
            CHECK(!lab && s_job[1].kind == IMPAI_JOB_BUILD && s_job[1].build == E_WIND,
                  "%s: the commander kept at its wind generator", NAME[c]);
        if (c == 29) CHECK(sy == 1 && !lab, "%s: the air plant let go, the shipyard", NAME[c]);
        if (c == 30 || c == 32) {
            int lx = -1;
            builds_of(from, E_LAB, &lx, NULL);
            CHECK(!sy && lab == 1 && lx >= 0 && lx < 52 && s_job[1].kind == IMPAI_JOB_BUILD && s_job[1].build == E_LAB,
                  "%s: the commander's lab outside the base ring, column %d", NAME[c], lx);
        }
        if (c == 31) CHECK(sy == 1 && !lab, "%s: no lab across the channel, the shipyard", NAME[c]);
        if (c == 27 || c == 28)
            CHECK(sy == 1 && !lab && s_job[1].kind == IMPAI_JOB_BUILD && s_job[1].build == (c == 27 ? E_MEX : E_WIND),
                  "%s: the CS's shipyard, the commander kept at its %s", NAME[c],
                  c == 27 ? "extractor" : "wind generator");
        if (c == 4) CHECK(!sy && lab == 1 && !vp, "%s: never air first, the lab", NAME[c]);
        if (c == 6 || c == 15) CHECK(!sy && lab == 1, "%s: the air plant, the lab", NAME[c]);
        if (c == 8) CHECK(!sy && vp == 1 && !lab, "%s: a land plant, the vehicle plant", NAME[c]);
    }
    s_plan = &IMPAI_PLAN_ROW;
    s_milp = NULL;
    s_w.refuse_type = 0;
    s_w.refuse_box_type = 0;
}

/* THE COMMANDER'S SAFE DISTANCE at the sea plant (impai_build.c, THE SEA PLANT): the coast world, where the
   commander is the one builder and the search's site is the shipyard at (64, 60), centre (1088, 1024). An enemy PT
   boat, armed against the air by its threat row, seen 1 092 px from the site: the shipyard is ordered there. Seen
   208 px from it: no order, the decision's refusal counted, and the cursor passed beyond the site, which no builder
   of the decision took (THE SITE NO BUILDER TAKES), so the next decision's search takes the next candidate. */
static void check_sea_cmd_safe(void)
{
    static IMPAI_ROW rows[FW_SLOTS];
    IMPAI_THREAT th[FW_DEFS];
    IMPAI_VIEW v;
    int sy, land, seen, r0 = -1, i0 = -1;
    memset(th, 0, sizeof th);
    th[E_PT].dps_air = 10;
    for (seen = 0; seen < 2; seen++) {
        coast_world(C_COAST, 1, 9);
        ring_view(&v, rows, th, E_PT, IMPAI_LOS, seen ? 40 : 60, seen ? 32 : 10);
        v.ntypes = E_N;
        s_viewp = &v;
        sea_decide(seen ? "the commander's safe distance, a PT boat at the site" :
                          "the commander's safe distance, a PT boat far off", &sy, &land);
        s_viewp = NULL;
        printf("  the cursor at ring %u, index %u\n", s_st.sea_r, s_st.sea_i);
        if (!seen) {
            CHECK(sy == 1, "a PT boat 1 092 px off: the shipyard ordered");
            r0 = s_st.sea_r;
            i0 = s_st.sea_i;
        } else
            CHECK(!sy && s_st.sea_refused == 1 && s_st.sea_r == r0 && s_st.sea_i == i0 + 1,
                  "a PT boat 208 px off: %d orders, %u refusals, the cursor at %u,%u (past the site %d,%d)", sy,
                  s_st.sea_refused, s_st.sea_r, s_st.sea_i, r0, i0);
    }
    /* the pass needs a command to spare: a decision whose assignment spent its last one may not have offered the
       site to every builder, and the cursor stays */
    {
        static IMPAI_DEC dd;
        IMPAI_BRAIN_IN bi;
        IMPAI_BRAIN_OUT bo;
        int spent;
        for (spent = 0; spent < 2; spent++) {
            memset(&dd, 0, sizeof dd);
            memset(&bi, 0, sizeof bi);
            memset(&bo, 0, sizeof bo);
            bi.st = &s_st;
            dd.in = &bi;
            dd.out = &bo;
            dd.sea_untaken = 1;
            dd.sea_answer = 1;
            dd.root_cap = 1;
            dd.roots = spent;
            s_st.sea_i = 5;
            impai_sea_untaken(&dd);
            printf("the site no builder takes, %s: the cursor at %u\n", spent ? "the commands spent" : "a command to spare",
                   s_st.sea_i);
            CHECK(s_st.sea_i == (spent ? 5 : 6), "the site no builder takes: passed only with a command to spare");
        }
    }
}

/* TECH-UP (impai_brain.c, economy; impai_econ.c, THE CHOICE) by the plants' build tech, on retail's ARM K-bot
   shape: the commander's menu holds the lab (FACTORY), whose constructor CONS builds the advanced lab ALAB, the
   advanced plant AVP and the hover plant HP; ALAB and AVP make the advanced constructor ACK, the only builder of the
   fusion plant FUS, and ALAB the K-bot ZEUS, AVP the tank BULL; HP makes the hover constructor HCONS, whose menu
   holds tech-1 buildings only, and the hover raider HRAID. The classifier makes the lab and the three plants tech 1
   (content_set), ACK, FUS, ZEUS, BULL, HCONS and HRAID tech 2. HP is cheaper than ALAB (1 700 against 1 850) and its
   units are tech 2, the retail hover plant's shape. Hard's row, with a military whose table's tech_m is Hard's, a
   metal income of 30 a pass against it, and an idle constructor. */
enum { U_ALAB = T_N, U_AVP, U_ACK, U_ZEUS, U_FUS, U_HP, U_HCONS, U_HRAID, U_BULL, U_N };
static IMPAI_MIL s_umil;
static IMPAI_TVALUE s_utv[FW_DEFS];
static void techup_world(void)
{
    static const uint16_t CONSMENU[] = { T_MEX, T_SOLAR, T_FACTORY, U_ALAB, U_AVP, U_HP };
    static const uint16_t ALABMENU[] = { U_ACK, U_ZEUS };
    static const uint16_t AVPMENU[] = { U_ACK, U_BULL };
    static const uint16_t ACKMENU[] = { T_MEX, T_SOLAR, U_FUS };
    static const uint16_t HPMENU[] = { U_HCONS, U_HRAID };
    static const uint16_t HCONSMENU[] = { T_MEX, T_SOLAR };
    TASIM_ROLE_OUT roles[U_N];
    int t;
    brain_world();
    s_w.ndefs = U_N;
    def_set(&s_w, U_ALAB, "ALAB", 6, 6, 0, 0, 1800, 3000);
    def_set(&s_w, U_AVP, "AVP", 6, 6, 0, 0, 2000, 3500);
    def_set(&s_w, U_ACK, "ACK", 2, 2, 1, 200, 400, 6000);
    def_set(&s_w, U_ZEUS, "ZEUS", 2, 2, 1, 0, 300, 4000);
    def_set(&s_w, U_FUS, "FUS", 5, 5, 0, 0, 3000, 20000);
    def_set(&s_w, U_HP, "HP", 6, 6, 0, 0, 1650, 3000);
    def_set(&s_w, U_HCONS, "HCONS", 2, 2, 1, 100, 150, 2500);
    def_set(&s_w, U_HRAID, "HRAID", 2, 2, 1, 0, 130, 1300);
    def_set(&s_w, U_BULL, "BULL", 2, 2, 1, 0, 400, 5000);
    menu_set(&s_w, T_CONS, CONSMENU, (int)(sizeof CONSMENU / sizeof CONSMENU[0]));
    menu_set(&s_w, U_ALAB, ALABMENU, (int)(sizeof ALABMENU / sizeof ALABMENU[0]));
    menu_set(&s_w, U_AVP, AVPMENU, (int)(sizeof AVPMENU / sizeof AVPMENU[0]));
    menu_set(&s_w, U_ACK, ACKMENU, (int)(sizeof ACKMENU / sizeof ACKMENU[0]));
    menu_set(&s_w, U_HP, HPMENU, (int)(sizeof HPMENU / sizeof HPMENU[0]));
    menu_set(&s_w, U_HCONS, HCONSMENU, (int)(sizeof HCONSMENU / sizeof HCONSMENU[0]));
    roles_set(roles);
    for (t = T_N; t < U_N; t++) {
        memset(&roles[t], 0, sizeof roles[t]);
        roles[t].role = TASIM_ROLE_OTHER;
    }
    roles[U_ALAB].role = roles[U_AVP].role = roles[U_HP].role = TASIM_ROLE_BUILDER;
    roles[U_ALAB].flags = roles[U_AVP].flags = roles[U_HP].flags = TASIM_RF_FACTORY;
    roles[U_ACK].role = roles[U_HCONS].role = TASIM_ROLE_BUILDER;
    roles[U_ACK].flags = roles[U_HCONS].flags = TASIM_RF_MOBILE;
    roles[U_ZEUS].role = roles[U_BULL].role = TASIM_ROLE_ASSAULT;
    roles[U_HRAID].role = TASIM_ROLE_RAIDER;
    roles[U_ZEUS].flags = roles[U_BULL].flags = roles[U_HRAID].flags = TASIM_RF_MOBILE;
    CHECK(content_set(&s_w, U_N, roles) == U_N, "the tech-up world's tinfo");
    s_ti[U_ZEUS].klass = s_ti[U_BULL].klass = IMPAI_K_LASSAULT;
    s_ti[U_HRAID].klass = IMPAI_K_LRAIDER;
    impai_mil_reset(&s_umil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
    s_umil.naval = 0;
    s_milp = &s_umil;
    s_tvp = NULL;
    s_w.econ[0].metal = s_w.econ[0].energy = s_w.econ[0].metal_max = s_w.econ[0].energy_max = 1000;
    s_w.econ[0].metal_in = 30;
    s_w.econ[0].energy_in = 100;
    s_w.econ[0].energy_out = 50;
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    unit_add(&s_w, 2, T_CONS, 24, 26, 1);
    s_w.gametime = 3000;
}

/* one decision of the tech-up world; the plants it ordered */
static void techup_decide(const char* name, int* lab, int* alab, int* avp, int* hp)
{
    IMPAI_BRAIN_OUT out;
    const int from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide: %s", name);
    *lab = builds_of(from, T_FACTORY, NULL, NULL);
    *alab = builds_of(from, U_ALAB, NULL, NULL);
    *avp = builds_of(from, U_AVP, NULL, NULL);
    *hp = builds_of(from, U_HP, NULL, NULL);
    printf("tech-up %s: lab=%d alab=%d avp=%d hp=%d\n", name, *lab, *alab, *avp, *hp);
}

/* SHORT OF METAL PAST THE TECH-UP (impai_brain.c, economy, SHORT OF METAL): the tech-up world at half its income's
   tech_m, half its store full, the constructor a reclaimer with a 20-metal rock beside it, no patch and no
   extractor built, so only a seat short of metal takes the rock. Two labs: the tech-up is the next want and its tech_m is not met --
   short, the rock reclaimed. Two labs and the advanced lab built: the tech-up done, no plant wanted, not short --
   no reclaim; with a quarter of the store full and more spent than comes in, a metal stall under stall_pm 300 and
   none under 200: the plan's threshold is the stall's. Two labs with the store at 900 of 1 000, a surplus: never
   short, whatever the income -- no reclaim (A SURPLUS IS NEVER SHORT). */
static void check_short_after_techup(void)
{
    const float tech_m = IMPAI_TABLE_DEFAULT.tech_m;
    IMPAI_BRAIN_OUT out;
    int done;
    for (done = 0; done < 3; done++) {
        techup_world();
        s_w.defs[T_CONS].d.flags |= WQ_DF_RECLAIMER;
        s_w.defs[T_CONS].d.speed = 1 << 16;
        s_w.econ[0].metal = done == 2 ? 900 : 500;
        s_w.econ[0].metal_in = tech_m / 2;
        s_w.econ[0].metal_out = 1;
        unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
        unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
        if (done == 1) unit_add(&s_w, 12, U_ALAB, 50, 30, 1);
        feat_add(25, 27, 2, 2, 20);
        s_spots = NULL;
        s_nspots = 0;
        CHECK(decide(&out) == 0, "decide: short past the tech-up");
        printf("short of metal, %s: reclaims %d\n",
               done == 2 ? "two labs, a surplus" : done ? "the tech-up built" : "two labs", reclaim_orders(0));
        CHECK(done ? !reclaim_orders(0) : reclaim_orders(0) == 1, "%s: %d reclaims",
              done == 2 ? "two labs, a surplus, not short"
              : done    ? "the tech-up built, not short"
                        : "two labs, short of the tech-up's income",
              reclaim_orders(0));
    }
    {
        /* the tech-up built and a quarter of the store full, spending past the income: under stall_pm 300 a metal
           stall, so short again -- the rock reclaimed; under Hard's 200 none */
        const IMPAI_PLAN* lv0 = s_plan;
        IMPAI_PLAN lv = *s_plan;
        int k;
        for (k = 0; k < 2; k++) {
            lv.stall_pm = (uint16_t)(k ? 300 : 200);
            s_plan = &lv;
            techup_world();
            s_w.defs[T_CONS].d.flags |= WQ_DF_RECLAIMER;
            s_w.defs[T_CONS].d.speed = 1 << 16;
            s_w.econ[0].metal = 250;
            s_w.econ[0].metal_in = tech_m / 2;
            s_w.econ[0].metal_out = tech_m;
            unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
            unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
            unit_add(&s_w, 12, U_ALAB, 50, 30, 1);
            feat_add(25, 27, 2, 2, 20);
            s_spots = NULL;
            s_nspots = 0;
            CHECK(decide(&out) == 0, "decide: a metal stall past the tech-up");
            s_plan = lv0;
            printf("short of metal, the tech-up built, a quarter stored, stall_pm %u: reclaims %d\n", lv.stall_pm,
                   reclaim_orders(0));
            CHECK(reclaim_orders(0) == k, "stall_pm %u, a quarter stored: %d reclaims", lv.stall_pm, reclaim_orders(0));
        }
    }
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* THE HOLD'S PLANTS (impai_brain.c, economy): the tech-up world with one lab -- the second factory's case -- and
   with two -- the tech-up's. At 1000 stored, the second lab, then the advanced lab; at 100, a hold, no plant in
   either; at 100 under a pressure stamp the plants as at 1000. */
static void check_hold_wants(void)
{
    static const char* const NAME[3] = { "1000 stored", "100 stored", "100 stored, pressed" };
    char name[64];
    int lab, alab, avp, hp, labs, k;
    for (labs = 1; labs <= 2; labs++)
        for (k = 0; k < 3; k++) {
            techup_world();
            unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
            if (labs == 2) unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
            if (k) s_w.econ[0].metal = 100.0f;
            if (k == 2) s_umil.press_until = (uint32_t)s_w.gametime + 100u;
            snprintf(name, sizeof name, "%s, %d lab%s", NAME[k], labs, labs > 1 ? "s" : "");
            techup_decide(name, &lab, &alab, &avp, &hp);
            if (k == 1)
                CHECK(!lab && !alab && !avp && !hp, "a plant in a hold, %s", name);
            else
                CHECK(labs == 1 ? lab == 1 && !alab && !avp && !hp : alab == 1 && !lab && !avp && !hp,
                      "the plants outside a hold, %s", name);
        }
    s_milp = NULL;
}

/* THE STEADY INCOME (impai_brain.c, economy): the tech-up world, whose commander makes 1.5 metal a pass and whose
   constructor and lab make none. A payout to the commander carries no plant want, a unit's own make rate stays
   income, metal credited to a finished structure that neither builds nor reclaims stays income, a frame's is a
   payout, and a payout past the
   whole income leaves the steady income at 0: no plant and the seat short of metal. */
static void check_steady_income(void)
{
    IMPAI_BRAIN_OUT out;
    int lab, alab, avp, hp;
    /* H. One lab and 76 a pass, 74.5 of it the commander's reclaim payout: steady 1.5 < 8, no second lab */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    s_w.econ[0].metal_in = 76.0f;
    s_w.unit[1].made_m = 76.0f;
    techup_decide("a commander's payout", &lab, &alab, &avp, &hp);
    CHECK(!lab && !alab && !avp && !hp, "a payout is not income: no plant");
    /* I. 9 a pass, the commander making its own 1.5: the second lab */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    s_w.econ[0].metal_in = 9.0f;
    s_w.unit[1].made_m = 1.5f;
    techup_decide("the commander's own make rate", &lab, &alab, &avp, &hp);
    CHECK(lab == 1 && !alab && !avp && !hp, "a unit's make rate stays income: the second lab");
    /* J. 8.5 a pass with a maker making its 1 (makesmetal): nothing subtracted, still the second lab */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 12, T_MAKER, 40, 20, 1);
    s_w.econ[0].metal_in = 8.5f;
    s_w.unit[1].made_m = 1.5f;
    s_w.unit[12].made_m = 1.0f;
    techup_decide("a maker's output", &lab, &alab, &avp, &hp);
    CHECK(lab == 1 && !alab && !avp && !hp, "a maker's output is income: the second lab");
    /* L. 9 a pass, 7.5 of it credited to a solar, whose def makes no metal and which neither builds nor reclaims:
       no payout reaches it, so it stays income -- the second lab; the same 7.5 on the lab, which builds, is a
       payout -- none */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 13, T_SOLAR, 45, 20, 1);
    s_w.econ[0].metal_in = 9.0f;
    s_w.unit[1].made_m = 1.5f;
    s_w.unit[13].made_m = 7.5f;
    techup_decide("a solar credited every pass", &lab, &alab, &avp, &hp);
    CHECK(lab == 1 && !alab && !avp && !hp, "a structure that neither builds nor reclaims takes no payout: the second lab");
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    s_w.econ[0].metal_in = 9.0f;
    s_w.unit[1].made_m = 1.5f;
    s_w.unit[10].made_m = 7.5f;
    techup_decide("a lab credited a tear-down", &lab, &alab, &avp, &hp);
    CHECK(!lab && !alab && !avp && !hp, "a factory's credit is a payout: no plant");
    /* and on the solar's frame, which an unbuild credits (engine/sim.md, row 10): a payout -- none */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 13, T_SOLAR, 45, 20, 0);
    s_w.econ[0].metal_in = 9.0f;
    s_w.unit[1].made_m = 1.5f;
    s_w.unit[13].made_m = 7.5f;
    techup_decide("a solar's frame credited an unbuild", &lab, &alab, &avp, &hp);
    CHECK(!lab && !alab && !avp && !hp, "a frame's credit is a payout: no plant");
    /* K. Two labs and 30 a pass, a constructor credited 100: steady 0, never negative -- no tech-up */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    s_w.unit[2].made_m = 100.0f;
    techup_decide("a payout past the income", &lab, &alab, &avp, &hp);
    CHECK(!lab && !alab && !avp && !hp, "a payout past the income: no plant");
    s_milp = NULL;
    /* and short of metal: the reclaim world's income is the factory_per_m its next factory wants, which reclaims
       nothing (check_reclaim); with a constructor credited 100 the steady income is 0 and its builders reclaim. The
       store at half its storage: a surplus is never short */
    reclaim_world(0);
    s_w.econ[0].metal = 500.0f;
    s_w.econ[0].metal_in = s_plan->factory_per_m;
    s_w.unit[2].made_m = 100.0f;
    feat_add(22, 22, 2, 2, 20);
    feat_add(30, 30, 2, 2, 300);
    CHECK(decide(&out) == 0, "decide");
    CHECK(reclaim_orders(0) > 0, "a payout past the income leaves the seat short of metal: %d reclaims",
          reclaim_orders(0));
    s_spots = PATCHES;
    s_nspots = NPATCH;
}

/* IN A SURPLUS THE METAL PRODUCERS WAIT (impai_econ.c) on the tech-up world, its store full: past the opening, one
   lab, the income at the plant's gate (factory_per_m a pass): no extractor, and the plant the income pays for is
   ordered instead; the income below the gate, 3 a pass: the extractors, which are the only way to the gate. At 700
   stored, or in the opening: the extractors. */
static void check_surplus_mex(void)
{
    static const char* const NAME[4] = { "past the opening in a surplus, the income at the plant's gate",
                                         "past the opening in a surplus, the income below it",
                                         "past the opening at 700 stored", "the opening in a surplus" };
    IMPAI_BRAIN_OUT out;
    int c, from, mex, lab;
    for (c = 0; c < 4; c++) {
        techup_world();
        s_w.econ[0].metal_in = c == 0 ? IMPAI_PLAN_ROW.factory_per_m : c == 1 ? 3.0f : 5.0f;
        if (c < 3) unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
        if (c == 2) s_w.econ[0].metal = 700.0f;
        if (c == 3) s_w.gametime = 300;
        from = s_w.nlog;
        CHECK(decide(&out) == 0, "decide: %s", NAME[c]);
        mex = builds_of(from, T_MEX, NULL, NULL);
        lab = builds_of(from, T_FACTORY, NULL, NULL);
        printf("surplus extractors, %s: mex=%d solar=%d lab=%d\n", NAME[c], mex, builds_of(from, T_SOLAR, NULL, NULL),
               lab);
        CHECK(c == 0 ? mex == 0 && lab == 1 : mex >= 1, "%s: %d extractors, %d labs", NAME[c], mex, lab);
    }
    s_milp = NULL;
}

/* THE YARD (impai_brain.c) on the reach world: a lab at (26, 20) whose head is a BuildingBuild of a constructor,
   its yard's one exit the south edge's cells 2 and 3 (THE WAY OUT's `c`, 0x2D; the rest `C` with 0x02, 0x2F).
   (a) No frame on its yard, the constructor at the lab's centre with an ASSIST of it under a guard: one decision
       moves it off the yard grown by its half-size, its job ended, one yard move counted.
   (b) The product's frame on the yard: no move.
   (c) The constructor idle on the yard with a lone Park: moved off, and never given a guard.
   (d) The commander on the yard inside THE COMMANDER'S WINDOW: untouched; outside it, moved off.
   (e) The helper of (a) with an enemy turret's reach over the lab: the walk check turns it back, and THE YARD
       neither moves it nor reads its head again (THE DECISION'S READS) -- one order, its turn back. */
static void yard_world(void)
{
    FUNIT* f;
    int x, z;
    reach_world(1);
    s_w.defs[T_FACTORY].yarded = 1;
    memset(s_w.defs[T_FACTORY].yard, 0x2F, 36);
    s_w.defs[T_FACTORY].yard[5 * 6 + 2] = s_w.defs[T_FACTORY].yard[5 * 6 + 3] = 0x2D;
    for (z = 0; z < FW_H; z++) for (x = 0; x < FW_W; x++) s_w.explored[z][x] = 1;
    unit_add(&s_w, 3, T_FACTORY, 26, 20, 1);
    f = &s_w.unit[3];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = T_CONS;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    s_w.econ[0].energy = 500;
    s_w.econ[0].energy_in = 1000;
}

/* 1 when (x, z) px lies on the lab's yard grown by unit `slot`'s half-size */
static int on_lab_yard(int slot, int32_t x, int32_t z)
{
    const int fx = s_ti[T_FACTORY].fx, fz = s_ti[T_FACTORY].fz;
    const int t = s_w.unit[slot].type;
    const int hb = (s_ti[t].fx > s_ti[t].fz ? s_ti[t].fx : s_ti[t].fz) * 8;
    return x >= 26 * 16 - hb && x < (26 + fx) * 16 + hb && z >= 20 * 16 - hb && z < (20 + fz) * 16 + hb;
}

static void check_yard_clear(void)
{
    unsigned reads;
    IMPAI_BRAIN_OUT out;
    FUNIT* u;
    int32_t x = 0, z = 0;
    int c, moved, from;
    for (c = 0; c < 2; c++) {
        yard_world();
        x = z = 0;
        if (c) unit_add(&s_w, 4, T_CONS, 27, 22, 0);
        memcpy(s_w.unit[2].pos, s_w.unit[3].pos, sizeof s_w.unit[2].pos);
        u = &s_w.unit[2];
        u->list[0].action = A_GUARD;
        u->list[0].target = 3;
        u->n = 1;
        s_job[2].kind = IMPAI_JOB_ASSIST;
        s_job[2].type = T_CONS;
        s_job[2].build = (uint16_t)WQ_NONE;
        s_job[2].target = 3;
        s_job[2].since = (uint32_t)s_w.gametime;
        from = s_w.nlog;
        CHECK(decide(&out) == 0, "decide");
        moved = last_move(from, 2, &x, &z);
        printf("the yard, %s: moved %d to (%d, %d), job %u, yard %u\n", c ? "the product's frame on it" : "no frame",
               moved, x, z, s_job[2].kind, out.yard);
        if (!c)
            CHECK(moved && !on_lab_yard(2, x, z) && s_job[2].kind == IMPAI_JOB_NONE && out.yard == 1 &&
                      count_action(from, A_MOVE, 2) == 1 && count_unit(2, -1) == 1,
                  "the helper on a waiting lab's yard is moved off it, one order");
        else
            CHECK(!moved && !out.yard, "the helper by a producing lab is not moved");
    }
    yard_world();
    memcpy(s_w.unit[2].pos, s_w.unit[3].pos, sizeof s_w.unit[2].pos);
    u = &s_w.unit[2];
    u->list[0].action = A_PARK;
    u->list[0].flags = 0x1001;
    u->list[0].target = (uint16_t)WQ_NONE;
    u->n = 1;
    from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide");
    moved = last_move(from, 2, &x, &z);
    printf("the yard, a parked constructor: moved %d to (%d, %d), guards %d, yard %u\n", moved, x, z,
           count_action(from, A_GUARD, 2), out.yard);
    CHECK(moved && !on_lab_yard(2, x, z) && !count_action(from, A_GUARD, 2) && out.yard == 1,
          "the parked constructor is moved off the yard and given no guard");
    for (c = 0; c < 2; c++) {
        yard_world();
        impai_mil_reset(&s_umil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
        s_umil.naval = 0;
        s_umil.safe_until = c ? 0 : (uint32_t)s_w.gametime + 300u;
        s_milp = &s_umil;
        memcpy(s_w.unit[1].pos, s_w.unit[3].pos, sizeof s_w.unit[1].pos);
        s_w.unit[2].live = 0;
        from = s_w.nlog;
        CHECK(decide(&out) == 0, "decide");
        moved = last_move(from, 1, &x, &z);
        printf("the yard, the commander %s its window: moved %d, orders %d, yard %u\n", c ? "outside" : "inside", moved,
               count_unit(1, -1), out.yard);
        CHECK(c ? moved && !on_lab_yard(1, x, z) && out.yard == 1 : !count_unit(1, -1) && !out.yard,
              "the commander on the yard: moved only outside its window");
        s_milp = NULL;
    }
    yard_world();
    memcpy(s_w.unit[2].pos, s_w.unit[3].pos, sizeof s_w.unit[2].pos);
    u = &s_w.unit[2];
    u->list[0].action = A_GUARD;
    u->list[0].target = 3;
    u->n = 1;
    s_job[2].kind = IMPAI_JOB_ASSIST;
    s_job[2].type = T_CONS;
    s_job[2].build = (uint16_t)WQ_NONE;
    s_job[2].target = 3;
    s_job[2].since = (uint32_t)s_w.gametime;
    route_row(IMPAI_LOS, 14, 11, 100);
    reads = s_reads_after;
    CHECK(decide(&out) == 0, "decide");
    s_viewp = NULL;
    printf("the yard, its helper turned back: orders %d given, yard %u, left %u, reads after its order %u, job %u\n",
           (int)out.roots, out.yard, out.yard_left, s_reads_after - reads, s_job[2].kind);
    CHECK(out.roots == 1 && !out.yard && !out.yard_left && s_reads_after == reads && s_job[2].kind == IMPAI_JOB_NONE,
          "a helper the walk check turned back is the yard's no more: one order, its head not read again");
}

/* THE WALK OFF (impai_brain.c, THE YARD): the constructor at the waiting lab's centre, handed back, its one order a
   move, at GameTime 3 000. (a) The move's point lies off the yard beyond its south exit, (464, 448) -- the walk the
   rule gives --, and its job is the economy's move there, given now: it is given nothing, at this decision and the
   next, each counted in yard_left. (b) The point lies on the yard: it is moved off, one order. (c) The point lies
   off the yard west of it, (320, 368), through the lab's west wall (THE WAY OFF): no walk off, so it is moved, out
   of the south exit, though it is the economy's move given now. (d) The walk of (a) given WALK_OFF_T + 1 ticks ago: moved again, never where its last point
   -- its job's cell's centre, (472, 456) -- would put it: the first ring's points and the next ring's south touch
   it, so the next ring's south-west, (419, 461). (e) The move of (a) with no job of the economy's: moved, one
   order, to (464, 448). (f) The walk of (a), given now, another constructor idle at (456, 440), on its point:
   moved where it touches neither that constructor nor its last point, the next ring's south-west, (419, 461).
   (g) The same, the other constructor on a live walk of its own to (648, 648): it holds not still, and the walk of
   (a) is trusted -- given nothing. (h) The move of (e), another constructor idle far off at (648, 648), off the
   region and so given no turn there, on a live walk to (464, 448): the walks first hold that point, and the first is moved
   where it does not touch it, the next ring's south, (464, 480). (i) The walk of (a), given now, a constructor
   helping the lab -- its one order a guard of it -- standing on its point at (456, 440): a helper may be passing
   through, and the walk is trusted -- given nothing. (j) The walk of (a), given now, another constructor whose own
   walk was given now and has ended -- no order left -- standing on its point at (456, 440): the decision holds no
   point for it, it holds still, and the first is moved as in (f), to (419, 461). */
static void check_yard_walk(void)
{
    static const char* const NAME[10] = { "a walk off the yard", "a move onto the yard", "a walk through the lab",
                                          "a walk lapsed", "a move not the economy's", "a point another stands on",
                                          "a point another walks from", "a point another walks to",
                                          "a point a helper stands on", "a point a walk ended on" };
    static const int32_t TO[10][2] = { { 464, 448 }, { 27 * 16, -1 }, { 20 * 16, -1 }, { 464, 448 }, { 464, 448 },
                                       { 464, 448 }, { 464, 448 }, { 464, 448 }, { 464, 448 }, { 464, 448 } };
    static const int32_t WANT[10][2] = { { 0, 0 },     { -1, -1 },   { -1, -1 },   { 419, 461 }, { 464, 448 },
                                         { 419, 461 }, { 0, 0 },     { 464, 480 }, { 0, 0 },     { 419, 461 } };
    IMPAI_BRAIN_OUT out;
    int32_t x = 0, z = 0;
    int c, moved, n, left = 0;
    for (c = 0; c < 10; c++) {
        FUNIT* u;
        yard_world();
        s_w.gametime = 3000;
        memcpy(s_w.unit[2].pos, s_w.unit[3].pos, sizeof s_w.unit[2].pos);
        u = &s_w.unit[2];
        u->list[0].action = A_MOVE;
        u->list[0].target = (uint16_t)WQ_NONE;
        u->list[0].pos[0] = (int32_t)((uint32_t)TO[c][0] << 16);
        u->list[0].pos[2] = TO[c][1] >= 0 ? (int32_t)((uint32_t)TO[c][1] << 16) : s_w.unit[3].pos[2];
        u->n = 1;
        if (c == 0 || c == 2 || c == 3 || c == 5 || c == 6 || c == 8 || c == 9) {
            s_job[2].kind = IMPAI_JOB_NONE;
            s_job[2].type = T_CONS;
            s_job[2].build = s_job[2].target = (uint16_t)WQ_NONE;
            s_job[2].cx = (int16_t)(TO[c][0] >> 4);
            s_job[2].cz = (int16_t)((TO[c][1] >= 0 ? TO[c][1] : s_w.unit[3].pos[2] >> 16) >> 4);
            s_job[2].since = (uint32_t)s_w.gametime - (c == 3 ? 901u : 0u);
        }
        if (c == 5 || c == 6 || c == 9) unit_add(&s_w, 4, T_CONS, 28, 27, 1);
        if (c == 9) {
            s_job[4].kind = IMPAI_JOB_NONE;
            s_job[4].type = T_CONS;
            s_job[4].build = s_job[4].target = (uint16_t)WQ_NONE;
            s_job[4].cx = 28;
            s_job[4].cz = 27;
            s_job[4].since = (uint32_t)s_w.gametime;
        }
        if (c == 7) unit_add(&s_w, 4, T_CONS, 40, 40, 1);
        if (c == 8) {
            FUNIT* h;
            unit_add(&s_w, 4, T_CONS, 28, 27, 1);
            h = &s_w.unit[4];
            h->list[0].action = A_GUARD;
            h->list[0].target = 3;
            h->n = 1;
            s_job[4].kind = IMPAI_JOB_ASSIST;
            s_job[4].type = T_CONS;
            s_job[4].build = (uint16_t)WQ_NONE;
            s_job[4].target = 3;
            s_job[4].since = (uint32_t)s_w.gametime;
        }
        if (c == 6 || c == 7) {
            const int32_t wx = c == 6 ? 648 : 464, wz = c == 6 ? 648 : 448;
            FUNIT* w = &s_w.unit[4];
            w->list[0].action = A_MOVE;
            w->list[0].target = (uint16_t)WQ_NONE;
            w->list[0].pos[0] = (int32_t)((uint32_t)wx << 16);
            w->list[0].pos[2] = (int32_t)((uint32_t)wz << 16);
            w->n = 1;
            s_job[4].kind = IMPAI_JOB_NONE;
            s_job[4].type = T_CONS;
            s_job[4].build = s_job[4].target = (uint16_t)WQ_NONE;
            s_job[4].cx = (int16_t)(wx >> 4);
            s_job[4].cz = (int16_t)(wz >> 4);
            s_job[4].since = (uint32_t)s_w.gametime;
        }
        CHECK(decide(&out) == 0, "decide");
        left = out.yard_left;
        if (!c) {
            s_w.gametime += 30;
            CHECK(decide(&out) == 0, "decide");
            left += out.yard_left;
        }
        moved = last_move(0, 2, &x, &z);
        n = count_unit(2, -1);
        printf("the yard, %s: orders %d, moved %d to (%d, %d), yard %u, left %d\n", NAME[c], n, moved, x, z, out.yard,
               left);
        if (!c || c == 6 || c == 8)
            CHECK(!n && !out.yard && left == (c ? 1 : 2), "the constructor walking off the yard, %s, is given nothing: "
                  "%d orders", NAME[c], n);
        else if (WANT[c][0] >= 0)
            CHECK(moved && n == 1 && x == WANT[c][0] && z == WANT[c][1] && out.yard == 1 && !left,
                  "the constructor, %s: moved to (%d, %d)", NAME[c], x, z);
        else
            CHECK(moved && n == 1 && !on_lab_yard(2, x, z) && z >= 26 * 16 && x >= 28 * 16 && x < 30 * 16 &&
                      out.yard == 1 && !left,
                  "the constructor %s is moved off it, out of the south exit: (%d, %d)",
                  c == 1 ? "sent onto the yard" : "walking through the lab", x, z);
    }
}

/* THE WAY OFF's points of the decision (impai_build.c, impai_way_to): one a builder -- a builder recorded again
   replaces its own point --, so every builder of the idle and helper lists recorded twice, as THE WALKS FIRST and a
   move can, fills the 2 x IMPAI_IDLE_CAP points exactly, each its second point, none dropped. */
static void check_way_to_one(void)
{
    static IMPAI_DEC dd;
    IMPAI_BRAIN_IN bi;
    IMPAI_UNITREF a;
    int k, ok = 1;
    memset(&dd, 0, sizeof dd);
    memset(&bi, 0, sizeof bi);
    memset(&a, 0, sizeof a);
    bi.ti = s_ti;
    bi.nti = FW_DEFS;
    dd.in = &bi;
    a.type = T_CONS;
    for (k = 0; k < 2 * IMPAI_IDLE_CAP; k++) {
        a.slot = (uint16_t)(1 + k);
        impai_way_to(&dd, &a, 32 * k, 16);
        impai_way_to(&dd, &a, 32 * k, 48);
    }
    for (k = 0; k < dd.nway_to; k++)
        if (dd.way_to[k].slot != 1 + k || dd.way_to[k].x != 32 * k || dd.way_to[k].z != 48) ok = 0;
    printf("the decision's points, every builder twice: %d points\n", dd.nway_to);
    CHECK(ok && dd.nway_to == 2 * IMPAI_IDLE_CAP, "one point a builder, its last: %d points", dd.nway_to);
}

/* THE WAY (impai_brain.c, THE YARD): the waiting lab, cells (26..31, 20..25), keeps a way out south of it, cells
   (27..30, 26..33), and a constructor idle with a lone Park stands on it at (456, 456), clear of the lab grown by
   its half-size. (a) THE WAY OFF's rings round it, south first -- the line from the lab's centre through it --
   then south-west and south-east: the nearest ring with a point off the way, 64 px out, gives south-west,
   (411, 501); south and south-east still touch the way. (b) With no way kept it is left where it stands.
   (c) With another plant's kept way, cells (22..26, 30..32), over that point -- its plant at cells (16..21,
   34..39) --, the same ring's next point clear of every way: west, (392, 456). (d) With a solar, cells (22..25, 30..33), over it instead: the
   same ring's next point the constructor touches no building at, west, (392, 456) -- a walk into the solar would
   end at its edge, on the way. */
static void check_yard_way(void)
{
    static const char* const NAME[4] = { "its way kept", "no way kept", "another plant's way south-west",
                                         "a solar south-west" };
    static const int32_t WANT[4][2] = { { 411, 501 }, { 0, 0 }, { 392, 456 }, { 392, 456 } };
    IMPAI_BRAIN_OUT out;
    int c;
    for (c = 0; c < 4; c++) {
        IMPAI_WAYREC* w = &s_st.way[0];
        FUNIT* u;
        int32_t x = 0, z = 0;
        int moved;
        yard_world();
        unit_add(&s_w, 2, T_CONS, 28, 28, 1);
        u = &s_w.unit[2];
        u->list[0].action = A_PARK;
        u->list[0].flags = 0x1001;
        u->list[0].target = (uint16_t)WQ_NONE;
        u->n = 1;
        if (c != 1) {
            memset(w, 0, sizeof *w);
            w->type = T_FACTORY;
            w->cx = 26;
            w->cz = 20;
            w->n = 1;
            w->r[0][0] = 27; w->r[0][1] = 26; w->r[0][2] = 30; w->r[0][3] = 33;
            s_st.nway = 1;
        }
        if (c == 2) {
            IMPAI_WAYREC* w2 = &s_st.way[1];
            unit_add(&s_w, 5, T_FACTORY, 16, 34, 1);
            memset(w2, 0, sizeof *w2);
            w2->type = T_FACTORY;
            w2->cx = 16;
            w2->cz = 34;
            w2->n = 1;
            w2->r[0][0] = 22; w2->r[0][1] = 30; w2->r[0][2] = 26; w2->r[0][3] = 32;
            s_st.nway = 2;
        }
        if (c == 3) unit_add(&s_w, 5, T_SOLAR, 22, 30, 1);
        CHECK(decide(&out) == 0, "decide");
        moved = last_move(0, 2, &x, &z);
        printf("the yard, a constructor on the way, %s: moved %d to (%d, %d), yard %u, ways %u\n", NAME[c], moved, x, z,
               out.yard, (unsigned)s_st.nway);
        CHECK(c == 1 ? !moved && !out.yard
                     : moved && x == WANT[c][0] && z == WANT[c][1] && out.yard == 1 && s_st.nway == (c == 2 ? 2 : 1),
              "the constructor on the way, %s: (%d, %d)", NAME[c], x, z);
    }
}

/* THE WAY OFF (impai_build.c): a constructor idle with a lone Park inside the waiting lab's footprint, at (464, 336),
   north of its centre (464, 368) -- where round 3's stood (impai_build.c, THE WAY OFF's WHY). (a) It is moved out of
   the lab's one exit, south, to (464, 448), never north through the lab. (b) The blocks straight south of the exit,
   column 14 from row 13, on no label, as past Steel Jungle ac-7's way: the first ring's next direction,
   south-west, (442, 438). (c) Every block from row 13 south on another label: no point beyond the exit's line is
   on the constructor's own label, so it is left where it stands. (d) The lab's yard map with no exit cell: it is
   searched round itself and tested on the region and its label alone, north, (464, 272). (e) The constructor
   flying: round itself, the region alone, north over the lab, (464, 272). (f) Its own block on no label: held to
   none, south out of the exit, (464, 448). (g) Standing just east of the lab, at (520, 368), its own block
   (16, 11) and the blocks west of column 16 on its label, every other block on another: each point on its label
   lies where the straight line crosses the lab, so it is left where it stands. (h) A second constructor inside the
   lab, at (464, 352): the first goes to (464, 448), and the second, never where it would touch the point the first
   was sent to this decision, to the next ring's south, (464, 480). */
static void check_yard_off(void)
{
    static const char* const NAME[8] = { "the exit open", "straight out on no label", "the exit on another label",
                                         "no exit", "flying", "no label of its own", "east of the lab",
                                         "two inside" };
    static const int32_t WANT[8][2] = { { 464, 448 }, { 442, 438 }, { 0, 0 }, { 464, 272 }, { 464, 272 },
                                        { 464, 448 }, { 0, 0 }, { 464, 448 } };
    static uint16_t keep[sizeof s_labels / sizeof s_labels[0]];
    const uint32_t vtol = s_ti[T_CONS].flags;
    IMPAI_BRAIN_OUT out;
    int c;
    for (c = 0; c < 8; c++) {
        FUNIT* u;
        int32_t x = 0, z = 0, x2 = 0, z2 = 0;
        int moved, moved2 = 0, bx, bz;
        yard_world();
        memcpy(keep, s_labels, sizeof keep);
        if (c == 7) {
            unit_add(&s_w, 4, T_CONS, 28, 21, 1);
            u = &s_w.unit[4];
            u->pos[0] = (int32_t)((uint32_t)464 << 16);
            u->pos[2] = (int32_t)((uint32_t)352 << 16);
            u->list[0].action = A_PARK;
            u->list[0].flags = 0x1001;
            u->list[0].target = (uint16_t)WQ_NONE;
            u->n = 1;
        }
        for (bz = 13; (c == 1 || c == 2) && bz < FW_H / 2; bz++)
            for (bx = 0; bx < FW_W / 2; bx++)
                if (c == 2 || bx == 14) s_labels[(size_t)bz * (FW_W / 2) + bx] = (uint16_t)(c == 2 ? 3 : 0);
        for (bz = 0; c == 6 && bz < FW_H / 2; bz++)
            for (bx = 16; bx < FW_W / 2; bx++)
                if (bx != 16 || bz != 11) s_labels[(size_t)bz * (FW_W / 2) + bx] = 3;
        if (c == 3) s_w.defs[T_FACTORY].yard[5 * 6 + 2] = s_w.defs[T_FACTORY].yard[5 * 6 + 3] = 0x2F;
        if (c == 4) s_ti[T_CONS].flags |= IMPAI_TF_VTOL;
        if (c == 5) s_labels[(size_t)10 * (FW_W / 2) + 14] = 0;
        u = &s_w.unit[2];
        u->pos[0] = (int32_t)((uint32_t)(c == 6 ? 520 : 464) << 16);
        u->pos[2] = (int32_t)((uint32_t)(c == 6 ? 368 : 336) << 16);
        u->list[0].action = A_PARK;
        u->list[0].flags = 0x1001;
        u->list[0].target = (uint16_t)WQ_NONE;
        u->n = 1;
        CHECK(decide(&out) == 0, "decide");
        memcpy(s_labels, keep, sizeof keep);
        s_ti[T_CONS].flags = vtol;
        moved = last_move(0, 2, &x, &z);
        if (c == 7) moved2 = last_move(0, 4, &x2, &z2);
        printf("the way off, %s: moved %d to (%d, %d), yard %u\n", NAME[c], moved, x, z, out.yard);
        CHECK(c == 2 || c == 6 ? !moved && !out.yard
                               : moved && x == WANT[c][0] && z == WANT[c][1] && out.yard == (c == 7 ? 2 : 1),
              "the constructor by the lab, %s: (%d, %d)", NAME[c], x, z);
        if (c == 7) {
            printf("the way off, two inside, the second: moved %d to (%d, %d)\n", moved2, x2, z2);
            CHECK(moved2 && x2 == 464 && z2 == 480, "(h) the second constructor clear of the first's point: (%d, %d)",
                  x2, z2);
        }
    }
}

static void check_techup(void)
{
    int lab, alab, avp, hp;
    const float tech_m = IMPAI_TABLE_DEFAULT.tech_m;
    techup_world();
    CHECK(s_ti[T_FACTORY].tech == 1 && s_ti[U_ALAB].tech == 1 && s_ti[U_AVP].tech == 1 && s_ti[U_HP].tech == 1 &&
              s_ti[U_HRAID].tech == 2 && s_ti[U_FUS].tech == 2,
          "the lab and the three plants are tech 1, the hover raider and the fusion plant 2: %u %u %u %u %u %u",
          s_ti[T_FACTORY].tech, s_ti[U_ALAB].tech, s_ti[U_AVP].tech, s_ti[U_HP].tech, s_ti[U_HRAID].tech,
          s_ti[U_FUS].tech);
    CHECK(s_ti[T_FACTORY].ctech == 1 && s_ti[U_ALAB].ctech == 2 && s_ti[U_AVP].ctech == 2 && s_ti[T_CONS].ctech == 1 &&
              s_ti[U_HP].ctech == 1,
          "build tech: lab %u, advanced lab %u, advanced plant %u, constructor %u, hover plant %u",
          s_ti[T_FACTORY].ctech, s_ti[U_ALAB].ctech, s_ti[U_AVP].ctech, s_ti[T_CONS].ctech, s_ti[U_HP].ctech);
    CHECK(s_w.econ[0].metal_in >= tech_m, "the income %g is at tech_m %g", s_w.econ[0].metal_in, tech_m);
    /* two labs stand, factory_max of the highest build tech, 1: the advanced lab, the least value of build tech 2,
       and never the hover plant, the cheapest plant whose units are tech 2 (the cause: the offer took it) */
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    techup_decide("two labs", &lab, &alab, &avp, &hp);
    CHECK(alab == 1 && !avp && !lab && !hp, "with two labs of build tech 1 the constructor builds the advanced lab");
    /* the same with values: the advanced plant's tank 0.5, the advanced lab's K-bot -0.2: the advanced plant */
    techup_world();
    memset(s_utv, 0, sizeof s_utv);
    s_utv[U_BULL].q[0] = s_utv[U_BULL].q[1] = 0.5f;
    s_utv[U_ZEUS].q[0] = s_utv[U_ZEUS].q[1] = -0.2f;
    s_utv[U_HRAID].q[0] = s_utv[U_HRAID].q[1] = 0.9f;
    s_tvp = s_utv;
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    techup_decide("two labs, valued", &lab, &alab, &avp, &hp);
    CHECK(avp == 1 && !alab && !lab && !hp, "the tech-up takes the plant of the best unit, never one of build tech 1");
    /* scored at the tech the candidates are built for: the advanced lab's K-bot 0.9 against tech 1 and -0.2 against
       tech 2, the advanced plant's tank 0.1 and 0.5: at tech 2, the advanced plant */
    techup_world();
    memset(s_utv, 0, sizeof s_utv);
    s_utv[U_ZEUS].q[0] = 0.9f;
    s_utv[U_ZEUS].q[1] = -0.2f;
    s_utv[U_BULL].q[0] = 0.1f;
    s_utv[U_BULL].q[1] = 0.5f;
    s_tvp = s_utv;
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    techup_decide("two labs, valued by tech", &lab, &alab, &avp, &hp);
    CHECK(avp == 1 && !alab && !lab && !hp, "the tech-up is scored at the next tech: the advanced plant");
    s_tvp = NULL;
    /* the hover plant's frame beside two labs: its build tech is 1, so the seat's highest stays 1 and the tech-up
       goes on (with the offer, its units' tech 2 ended the tech-up for good) */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    unit_add(&s_w, 12, U_HP, 50, 30, 0);
    techup_decide("a hover plant's frame", &lab, &alab, &avp, &hp);
    CHECK(alab == 1 && !avp && !hp, "a hover plant does not end the tech-up");
    /* one lab: not yet factory_max of the highest build tech, so no tech-up; the second factory is the ordinary
       choice, the least value of build tech 1, the lab */
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    techup_decide("one lab", &lab, &alab, &avp, &hp);
    CHECK(lab == 1 && !alab && !avp && !hp, "one lab: the second lab, no tech-up");
    /* the advanced lab and the advanced plant stand, two plants of build tech 2, the store outside a surplus:
       nothing builds more (in a surplus the factories are paced by income, check_surplus_plants) */
    techup_world();
    s_w.econ[0].metal = 700.0f;
    unit_add(&s_w, 10, U_ALAB, 30, 30, 1);
    unit_add(&s_w, 11, U_AVP, 40, 35, 1);
    techup_decide("two plants of build tech 2", &lab, &alab, &avp, &hp);
    CHECK(!alab && !avp && !lab && !hp, "plants of build tech 2: no other plant");
    /* a frame of the advanced lab beside two labs, the store outside a surplus: the seat's highest build tech is 2
       already */
    techup_world();
    s_w.econ[0].metal = 700.0f;
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    unit_add(&s_w, 12, U_ALAB, 50, 30, 0);
    techup_decide("an advanced lab's frame", &lab, &alab, &avp, &hp);
    CHECK(!alab && !avp && !hp, "an advanced lab under construction: no second plant of build tech 2");
    s_milp = NULL;
}

/* EACH BUILDING COUNTED ONCE (impai_brain.c, the census and THE HEADS' COUNTS): a builder's build head and the
   frame it placed are one building.
   (a) A tank in progress at a busy factory, its count node 1 and its frame standing, and raiders of about the
       tank's value: the idle factory buys the assault class, below its share -- the frame counted beside its node
       made the tank count twice and put another class furthest below its share.
   (b) Pressed at the base with press_v = def_v (two towers wanted): a light laser's frame with its builder's head
       on it, and an idle constructor that builds the second tower.
   (c) Seen air of twice aa_struct_v (two anti-air guns wanted): an anti-air gun's frame with its builder's head,
       and the idle constructor builds the second.
   (d) Energy income one and a half solar collectors short of 1.1 times the use, no metal patch: a solar frame with
       its builder's head, and one more energy building is ordered (the commander's wind generator). */
static void build_head(int slot, int type, const int32_t* pos)
{
    FUNIT* u = &s_w.unit[slot];
    memset(&u->list[0], 0, sizeof u->list[0]);
    u->list[0].action = A_MOBILEBUILD;
    u->list[0].build = 1;
    u->list[0].type = (uint16_t)type;
    u->list[0].target = (uint16_t)WQ_NONE;
    memcpy(u->list[0].pos, pos, sizeof u->list[0].pos);
    u->n = 1;
}

/* builder 11 at work on a frame of `type`, unit 12 at cell (50, 30) */
static void head_and_frame(int type)
{
    own(11, T_CONS, 48, 30);
    unit_add(&s_w, 12, type, 50, 30, 0);
    build_head(11, type, s_w.unit[12].pos);
}

static void check_frames_once(void)
{
    static const uint16_t ARMY[] = { M_RAIDER, M_TANK, M_SCOUT };
    IMPAI_BRAIN_OUT bo;
    uint16_t tw[4];
    int32_t at[4][2];
    int cons, army, k, n, nr, ns, from, bought = 0;
    float vt, vr;
    FUNIT* f;
    gov_world(2);
    menu_set(&s_w, T_FACTORY, ARMY, 3);
    vt = s_ti[M_TANK].value;
    vr = s_ti[M_RAIDER].value;
    nr = vr > 0.0f && vt / vr >= 1.0f ? (int)(vt / vr + 0.5f) : 1;
    CHECK(nr * vr > 0.6f * vt && nr * vr < 1.3f * vt, "the raiders' value %.0f is about the tank's %.0f",
          (double)(nr * vr), (double)vt);
    f = &s_w.unit[2];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = M_TANK;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    unit_add(&s_w, 5, M_TANK, 30, 32, 0);
    unit_add(&s_w, 6, M_SCOUT, 34, 32, 1);      /* a scout, so THE SCOUT FIRST holds no order */
    gov_units(M_RAIDER, nr, 100);
    from = s_w.nlog;
    gov_decide(&bo, &cons, &army);
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.unit == 3 && s_w.log[k].c.verb == WQ_CMD_QUEUE) bought = s_w.log[k].c.type;
    printf("frames once, a tank in progress and %d raiders: the idle factory buys type %d\n", nr, bought);
    CHECK(bought == M_TANK, "the tank in progress counted once: the assault class is bought, type %d", bought);
    s_milp = NULL;

    def_world(M_RAIDER, IMPAI_PLAN_ROW.def_v);
    head_and_frame(D_LLT);
    n = def_decide(1, tw, at);
    printf("frames once, a tower's frame and its head, two wanted: %d ordered, %s\n", n, n ? def_name(tw[0]) : "none");
    CHECK(n == 1, "the tower's frame and its builder's head are one tower: the second is ordered");

    def_world(M_RAIDER, 100.0f);
    s_mil.press_until = 0;
    s_mil.air_seen = 2.0f * IMPAI_PLAN_ROW.aa_struct_v;
    head_and_frame(D_AA);
    n = def_decide(1, tw, at);
    printf("frames once, an anti-air frame and its head, two wanted: %d ordered, %s\n", n, n ? def_name(tw[0]) : "none");
    CHECK(n == 1 && tw[0] == D_AA, "the anti-air frame and its builder's head are one gun: the second is ordered");

    def_world(M_RAIDER, 100.0f);
    s_mil.press_until = 0;
    s_w.econ[0].energy = s_w.econ[0].energy_max = 1000;
    s_w.econ[0].energy_out = 1000;
    s_w.econ[0].energy_in = 1100.0f - 1.5f * s_ti[T_SOLAR].egain;
    head_and_frame(T_SOLAR);
    ns = s_nspots;
    s_nspots = 0;
    from = s_w.nlog;
    n = def_decide(1, tw, at);
    s_nspots = ns;
    for (k = from, n = 0; k < s_w.nlog; k++)
        n += s_w.log[k].c.action == A_MOBILEBUILD && s_w.log[k].c.type < M_N &&
             s_ti[s_w.log[k].c.type].cat == IMPAI_C_ENERGY;
    printf("frames once, a solar's frame and its head, income %.1f of 1100: %d energy buildings ordered\n",
           (double)s_w.econ[0].energy_in, n);
    CHECK(n == 1, "the solar's frame and its builder's head are one solar: another energy building is ordered");
    s_milp = NULL;
}

/* THE LADDER'S HELPER (impai_econ.c): the seat's only constructor helps the factory's product, a frame on its
   yard. Pressed at the base, it leaves the help for the ground defence; with seen air of aa_struct_v and no
   pressure, for the anti-air gun; on the tech-up's world with two labs and the store at 700, outside a surplus so
   that no further plant is wanted, for the advanced lab. */
static void helper_helps(int slot, int frame, int type, int cx, int cz)
{
    IMPAI_JOB* j = &s_job[slot];
    FUNIT* u = &s_w.unit[slot];
    unit_add(&s_w, frame, type, cx, cz, 0);
    j->kind = IMPAI_JOB_ASSIST;
    j->type = s_w.unit[slot].type;
    j->build = (uint16_t)WQ_NONE;
    j->target = (uint16_t)frame;
    j->since = (uint32_t)s_w.gametime;
    memset(&u->list[0], 0, sizeof u->list[0]);
    u->list[0].action = A_HELPBUILD;
    u->list[0].target = (uint16_t)frame;
    u->n = 1;
}

static void check_helper_wants(void)
{
    uint16_t tw[4];
    int32_t at[4][2];
    int n, lab, alab, avp, hp;
    def_world(M_RAIDER, 100.0f);
    helper_helps(10, 13, M_RAIDER, 30, 31);
    n = def_decide(1, tw, at);
    printf("the helper, pressed at the base: %d ordered, %s\n", n, n ? def_name(tw[0]) : "none");
    CHECK(n == 1 && tw[0] == D_LLT && s_job[10].kind != IMPAI_JOB_ASSIST, "the helping constructor builds the tower");
    def_world(M_RAIDER, 100.0f);
    s_mil.press_until = 0;
    s_mil.air_seen = IMPAI_PLAN_ROW.aa_struct_v;
    helper_helps(10, 13, M_RAIDER, 30, 31);
    n = def_decide(1, tw, at);
    printf("the helper, air seen: %d ordered, %s\n", n, n ? def_name(tw[0]) : "none");
    CHECK(n == 1 && tw[0] == D_AA && s_job[10].kind != IMPAI_JOB_ASSIST,
          "the helping constructor builds the anti-air gun");
    s_milp = NULL;
    techup_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
    s_w.econ[0].metal = 700.0f;
    helper_helps(2, 12, T_CONS, 30, 31);
    techup_decide("a helper, two labs", &lab, &alab, &avp, &hp);
    CHECK(alab == 1 && !avp && !lab && !hp && s_job[2].kind != IMPAI_JOB_ASSIST,
          "the helping constructor builds the advanced lab");
}

/* a factory at its BuildingBuild of `type`, its ledger having asked req_m metal and req_e energy in the last pass */
static void factory_busy(int slot, int type, float req_m, float req_e)
{
    FUNIT* f = &s_w.unit[slot];
    f->list[0].action = A_BUILDINGBUILD;
    f->list[0].type = (uint16_t)type;
    f->list[0].target = (uint16_t)WQ_NONE;
    f->list[0].count = 1;
    f->n = 1;
    f->req_m = req_m;
    f->req_e = req_e;
}

/* IN A SURPLUS THE FACTORIES ARE PACED BY WHAT THEY SPEND (impai_brain.c, economy, THE PLANT'S PRICE), on the tech-up
   world, its income 30 a pass, its factories at their BuildingBuild each asking 5 metal and 10 energy a pass, the
   seat asking 15:
   (a) the end state the long games measured, two labs and an advanced lab -- three factories, one of the seat's
       highest build tech, so no tech-up is due --: in a surplus a further plant of build tech at most 2, wanted as
       a further plant (plant_want 2); with the store at 700 of 1 000, none;
   (b) two labs in a surplus: the tech-up is due beside the further plant and comes first, the advanced lab
       (plant_want 3);
   (c) the advanced lab and the advanced plant in a surplus: a tech-up is due and no menu offers one, so the further
       plant goes ahead, the lab, the least value with no values (THE TECH-UP FIRST);
   (d) the end state, its plants asking 28 of the 30: the 2 unasked do not pay the price of 5, none;
   (e) the end state with one factory idle: no plant, and no extractor, the overflow still the price;
   (f) the end state, no factory asking metal (each waiting at its yard): no price, none. */
static void check_surplus_plants(void)
{
    static const char* const NAME[7] = { "the end state in a surplus", "the end state at 700 stored",
                                         "two labs in a surplus", "two plants of build tech 2 in a surplus",
                                         "the end state, its plants asking 28", "the end state, a factory idle",
                                         "the end state, no factory asking metal" };
    IMPAI_BRAIN_OUT out;
    int c, from, lab, alab, avp, hp;
    for (c = 0; c < 7; c++) {
        const float req = c == 6 ? 0.0f : 5.0f;
        techup_world();
        if (c == 1) s_w.econ[0].metal = 700.0f;
        s_w.econ[0].metal_out = c == 4 ? 28.0f : 15.0f;
        unit_add(&s_w, 10, c == 3 ? U_ALAB : T_FACTORY, 30, 30, 1);
        unit_add(&s_w, 11, c == 3 ? U_AVP : T_FACTORY, 40, 35, 1);
        factory_busy(10, U_ZEUS, req, 10.0f);
        factory_busy(11, U_ZEUS, req, 10.0f);
        if (c != 2 && c != 3) {
            unit_add(&s_w, 12, U_ALAB, 50, 30, 1);
            if (c != 5) factory_busy(12, U_ZEUS, req, 10.0f);
        }
        from = s_w.nlog;
        CHECK(decide(&out) == 0, "decide: %s", NAME[c]);
        lab = builds_of(from, T_FACTORY, NULL, NULL);
        alab = builds_of(from, U_ALAB, NULL, NULL);
        avp = builds_of(from, U_AVP, NULL, NULL);
        hp = builds_of(from, U_HP, NULL, NULL);
        printf("surplus plants, %s: lab=%d alab=%d avp=%d hp=%d plant=%u want=%u\n", NAME[c], lab, alab, avp, hp,
               out.plant, out.plant_want);
        if (c == 0)
            CHECK(lab + alab + avp + hp == 1 && out.plant && s_ti[out.plant].ctech <= 2 && out.plant_want == 2,
                  "%s: one further plant of build tech at most 2", NAME[c]);
        if (c == 1 || c >= 4) CHECK(!lab && !alab && !avp && !hp && !out.plant, "%s: no plant", NAME[c]);
        if (c == 5) CHECK(!builds_of(from, T_MEX, NULL, NULL), "%s: the extractors wait", NAME[c]);
        if (c == 2) CHECK(alab == 1 && !lab && !avp && !hp && out.plant_want == 3, "%s: the tech-up first", NAME[c]);
        if (c == 3) CHECK(lab == 1 && !alab && !avp && !hp && out.plant_want == 2, "%s: the further plant", NAME[c]);
    }
    s_milp = NULL;
}

/* ENERGY THE LIMIT (impai_brain.c, THE PLANT'S PRICE; impai_econ.c, energy_target): the end state of the tech-up
   world at an income of 12, its three factories each asking 3 metal (the seat 9), the energy 120 made against 100
   asked -- above the ladder's own target, 1.1 x 100 and 8 x 12. With each factory asking 10 energy the plant pays,
   and is ordered; asking 30, the plant is 10 a pass short, so no plant and one energy building; outside a surplus
   neither, the extractors back. THE FLOWS' LAG: asking 30 beside an onoffable maker of 60 switched on since the
   pass -- active, its ledger asking nothing --: its 60 is added to the energy asked as it is to the makers' cost
   added back, the room 20 still: no plant. */
static void check_surplus_energy(void)
{
    static const char* const NAME[4] = { "energy pays", "energy the limit", "no surplus",
                                         "energy the limit, a maker on since the pass" };
    IMPAI_BRAIN_OUT out;
    int c, from, plants, energy;
    for (c = 0; c < 4; c++) {
        const float req_e = c == 0 ? 10.0f : 30.0f;
        techup_world();
        s_w.econ[0].metal_in = 12.0f;
        s_w.econ[0].metal_out = 9.0f;
        s_w.econ[0].energy_in = 120.0f;
        s_w.econ[0].energy_out = 100.0f;
        if (c == 2) s_w.econ[0].metal = 500.0f;
        unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
        unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
        unit_add(&s_w, 12, U_ALAB, 50, 30, 1);
        factory_busy(10, U_ZEUS, 3.0f, req_e);
        factory_busy(11, U_ZEUS, 3.0f, req_e);
        factory_busy(12, U_ZEUS, 3.0f, req_e);
        if (c == 3) {
            s_w.defs[T_MAKER].d.flags |= WQ_DF_ONOFF;
            s_ti[T_MAKER].flags |= IMPAI_TF_ONOFF;
            unit_add(&s_w, 13, T_MAKER, 60, 30, 1);
            s_w.unit[13].active = 1;
        }
        from = s_w.nlog;
        CHECK(decide(&out) == 0, "decide: %s", NAME[c]);
        plants = builds_of(from, T_FACTORY, NULL, NULL) + builds_of(from, U_ALAB, NULL, NULL) +
                 builds_of(from, U_AVP, NULL, NULL) + builds_of(from, U_HP, NULL, NULL);
        energy = builds_of(from, T_SOLAR, NULL, NULL) + builds_of(from, T_WIND, NULL, NULL);
        printf("surplus energy, %s: plants=%d energy=%d\n", NAME[c], plants, energy);
        if (c == 0) CHECK(plants == 1 && !energy, "%s: the plant", NAME[c]);
        if (c == 1) CHECK(!plants && energy == 1, "%s: energy first", NAME[c]);
        if (c == 2) CHECK(!plants && !energy, "%s: neither", NAME[c]);
        if (c == 3) CHECK(!plants, "%s: no plant", NAME[c]);
    }
    s_milp = NULL;
}

/* EVERY PLANT PAST THE FIRST WAITS ON THE ENERGY TERM (impai_brain.c, THE PLANT'S PRICE), below a surplus: the
   tech-up world with no patch, its store at 500, the energy 120 made against 100 asked. One lab and 12 a pass, the
   lab asking 3 metal: asking 10 energy, the second lab; asking 30, 10 a pass short -- no plant, and the energy
   rung's target raised to 130 builds one energy building. Two labs at the tech-up's income: asking 10 each, the
   advanced lab; asking 30, none. One lab at 5 a pass, below the second plant's 8, asking 30: no plant's metal, so
   no need -- the target stays at 110, below the 120 made, and no energy building. */
static void check_plant_energy(void)
{
    static const char* const NAME[5] = { "the second plant, energy pays", "the second plant, energy the limit",
                                         "the tech-up, energy pays", "the tech-up, energy the limit",
                                         "no plant's metal, energy short of its price" };
    const float tech_m = IMPAI_TABLE_DEFAULT.tech_m;
    IMPAI_BRAIN_OUT out;
    int c, from, lab, alab, energy;
    for (c = 0; c < 5; c++) {
        const float req_e = c & 1 || c == 4 ? 30.0f : 10.0f;
        techup_world();
        s_spots = NULL;
        s_nspots = 0;
        s_w.econ[0].metal = 500.0f;
        s_w.econ[0].metal_in = c == 4 ? 5.0f : c < 2 ? 12.0f : tech_m + 6.0f;
        s_w.econ[0].metal_out = c < 2 || c == 4 ? 3.0f : 6.0f;
        s_w.econ[0].energy_in = 120.0f;
        s_w.econ[0].energy_out = 100.0f;
        unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
        factory_busy(10, U_ZEUS, 3.0f, req_e);
        if (c == 2 || c == 3) {
            unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
            factory_busy(11, U_ZEUS, 3.0f, req_e);
        }
        from = s_w.nlog;
        CHECK(decide(&out) == 0, "decide: %s", NAME[c]);
        lab = builds_of(from, T_FACTORY, NULL, NULL);
        alab = builds_of(from, U_ALAB, NULL, NULL);
        energy = builds_of(from, T_SOLAR, NULL, NULL) + builds_of(from, T_WIND, NULL, NULL);
        printf("plant energy, %s: lab=%d alab=%d energy=%d\n", NAME[c], lab, alab, energy);
        if (c == 0) CHECK(lab == 1 && !alab, "%s: the second lab", NAME[c]);
        if (c == 1) CHECK(!lab && !alab && energy == 1, "%s: energy first", NAME[c]);
        if (c == 2) CHECK(alab == 1 && !lab, "%s: the advanced lab", NAME[c]);
        if (c == 3) CHECK(!lab && !alab, "%s: no plant", NAME[c]);
        if (c == 4) CHECK(!lab && !alab && !energy, "%s: no plant, no energy", NAME[c]);
    }
    s_spots = PATCHES;
    s_nspots = NPATCH;
    s_milp = NULL;
}

/* THE MAKERS (impai_econ.c): reach_world's commander and four onoffable makers (60 energy, 1 metal each) at slots
   20..23, the storage 1 000 of each; each maker's ledger asked its 60 in the last pass while it is active, as a pass
   settles (maker_settle) */
static void maker_settle(void)
{
    int k;
    for (k = 0; k < 4; k++) s_w.unit[20 + k].req_e = s_w.unit[20 + k].active ? s_ti[T_MAKER].egain : 0.0f;
}

static void makers_world(int active)
{
    int k;
    reach_world(0);
    s_w.defs[T_MAKER].d.flags |= WQ_DF_ONOFF;
    s_ti[T_MAKER].flags |= IMPAI_TF_ONOFF;
    for (k = 0; k < 4; k++) {
        unit_add(&s_w, 20 + k, T_MAKER, 30 + 4 * k, 40, 1);
        s_w.unit[20 + k].active = (uint8_t)active;
    }
    maker_settle();
    s_w.econ[0].metal = 300;
    s_w.econ[0].metal_max = s_w.econ[0].energy_max = 1000;
    s_w.econ[0].metal_in = 10;
    s_w.econ[0].metal_out = 10;
}

static void maker_set(float energy, float e_in, float e_out)
{
    s_w.econ[0].energy = energy;
    s_w.econ[0].energy_in = e_in;
    s_w.econ[0].energy_out = e_out;
}

/* the makers' switches at one decision `ticks` on: off and on counts, and a bit a maker slot switched (bit k: slot
   20 + k); 30 ticks or more on, an economy pass has settled since, and the ledgers ask what the bits say */
static void maker_decide(IMPAI_BRAIN_OUT* out, int ticks, int* off, int* on, unsigned* which)
{
    int k;
    if (ticks >= 30) maker_settle();
    s_w.gametime += ticks;
    s_w.nlog = 0;
    CHECK(decide(out) == 0, "decide");
    *off = *on = 0;
    *which = 0;
    for (k = 0; k < 4; k++) {
        const int f = count_unit(20 + k, A_DEACTIVATE), n = count_unit(20 + k, A_ACTIVATE);
        *off += f;
        *on += n;
        if (f || n) *which |= 1u << k;
    }
}

/* (a) energy short -- 200 of 1 000, 100 made against 230 asked --: off the fewest that cost the 130, three, the ties
   from the highest slot; (b) 300 ticks on, past the switches' dwell, the 180 they asked gone and 150 spare, the
   store at 450, inside the band: none; (c) the store at 700, 50 spare: none, a maker costs 60; (d) 300 ticks on,
   130 spare: on the two the spare pays, the lowest slots; (e) energy short again 15 ticks on: off only the maker no switch holds, slot 20;
   (f) the metal full -- 980 of 1 000, 30 made against 10 asked --, energy steady: all four off, the switch's most;
   (f') the metal at 850, in the surplus and below full, 130 energy spare: no switch either way, the metal's band;
   (g) the commander's ladder beside one off maker the dwell holds: no maker built; with it free, the switch on and
   a maker the spare still pays, 150 against 60 + 60; with 100 spare the switch alone;
   THE FLOWS' LAG: (h) energy short as in (a), then 15 ticks on in the same pass, its flows unchanged: the three
   switched off took their 180 off the energy asked, 50 against 100 made, no shortfall: none; (i) all four off, 130
   spare, on two, then 15 ticks on in the same pass: the two switched on added their 120, 10 spare: none; (j) the
   metal full -- 980, 12 made against 10 asked --: off two, the 2 they make, then 15 ticks on in the same pass: the
   two took their metal off the made, no overflow: none; (k) the ladder of (g) beside its one maker switched on since
   the pass -- active, its ledger asking nothing --, 150 made against 50 asked: the lag's 60 leaves 40, short of a
   maker's 60: no maker built. */
static void check_makers(void)
{
    IMPAI_BRAIN_OUT out;
    int off, on, built[3], c;
    unsigned which;
    makers_world(1);
    maker_set(200, 100, 230);
    maker_decide(&out, 0, &off, &on, &which);
    printf("makers, energy short: off=%d on=%d which=%x makers_off=%u\n", off, on, which, out.makers_off);
    CHECK(off == 3 && !on && which == 0xEu && out.makers_off == 3, "energy short: the three that cost the shortfall");
    maker_set(450, 200, 50);
    maker_decide(&out, 300, &off, &on, &which);
    printf("makers, inside the band: off=%d on=%d\n", off, on);
    CHECK(!off && !on, "inside the band: no switch");
    maker_set(700, 100, 50);
    maker_decide(&out, 15, &off, &on, &which);
    printf("makers, 50 spare: off=%d on=%d\n", off, on);
    CHECK(!off && !on, "50 spare: no maker paid");
    maker_set(700, 180, 50);
    maker_decide(&out, 300, &off, &on, &which);
    printf("makers, 130 spare: off=%d on=%d which=%x makers_on=%u\n", off, on, which, out.makers_on);
    CHECK(!off && on == 2 && which == 0x6u && out.makers_on == 2, "130 spare: on the two it pays, the lowest slots");
    maker_set(200, 100, 230);
    maker_decide(&out, 15, &off, &on, &which);
    printf("makers, short again: off=%d on=%d which=%x\n", off, on, which);
    CHECK(off == 1 && !on && which == 0x1u, "short again: only the maker no switch holds");
    makers_world(1);
    maker_set(500, 100, 100);
    s_w.econ[0].metal = 980;
    s_w.econ[0].metal_in = 30;
    maker_decide(&out, 0, &off, &on, &which);
    printf("makers, metal full: off=%d on=%d which=%x\n", off, on, which);
    CHECK(off == 4 && !on, "metal full: all four off");
    makers_world(0);
    maker_set(700, 180, 50);
    s_w.econ[0].metal = 850;
    maker_decide(&out, 0, &off, &on, &which);
    printf("makers, metal in the surplus, energy spare: off=%d on=%d\n", off, on);
    CHECK(!off && !on, "metal in the surplus: no switch");
    for (c = 0; c < 3; c++) {
        makers_world(0);
        s_w.unit[20].live = s_w.unit[22].live = s_w.unit[23].live = 0;
        s_w.econ[0].metal_in = 2;
        s_w.econ[0].metal_out = 2;
        maker_set(900, c == 2 ? 150 : 200, 50);
        if (!c) {
            s_job[21].type = T_MAKER;
            s_job[21].kind = IMPAI_JOB_SWITCH;
            s_job[21].since = (uint32_t)s_w.gametime;
        }
        maker_decide(&out, 15, &off, &on, &which);
        built[c] = builds_of(0, T_MAKER, NULL, NULL);
        printf("makers, the ladder %s: on=%d makers built=%d\n",
               !c ? "beside an off maker the dwell holds" : c == 1 ? "with 150 spare" : "with 100 spare", on, built[c]);
        if (!c) CHECK(!on && !built[c], "no maker built beside an off maker");
        if (c == 1) CHECK(on == 1 && built[c] == 1, "150 spare: the switch and a maker");
        if (c == 2) CHECK(on == 1 && !built[c], "100 spare: the switch alone");
    }
    makers_world(1);
    maker_set(200, 100, 230);
    maker_decide(&out, 0, &off, &on, &which);
    CHECK(off == 3 && !on, "the lag: the three off");
    maker_decide(&out, 15, &off, &on, &which);
    printf("makers, the same pass after three off: off=%d on=%d\n", off, on);
    CHECK(!off && !on, "the same pass after three off: the shortfall paid, none");
    makers_world(0);
    maker_set(700, 180, 50);
    maker_decide(&out, 0, &off, &on, &which);
    CHECK(!off && on == 2, "the lag: two on");
    maker_decide(&out, 15, &off, &on, &which);
    printf("makers, the same pass after two on: off=%d on=%d\n", off, on);
    CHECK(!off && !on, "the same pass after two on: the spare spent, none");
    makers_world(1);
    maker_set(500, 100, 100);
    s_w.econ[0].metal = 980;
    s_w.econ[0].metal_in = 12;
    maker_decide(&out, 0, &off, &on, &which);
    CHECK(off == 2 && !on, "the lag: two off for the metal's overflow: %d", off);
    maker_decide(&out, 15, &off, &on, &which);
    printf("makers, the same pass after two off for the metal: off=%d on=%d\n", off, on);
    CHECK(!off && !on, "the same pass after two off for the metal: none");
    makers_world(0);
    s_w.unit[20].live = s_w.unit[22].live = s_w.unit[23].live = 0;
    s_w.unit[21].active = 1;
    s_w.econ[0].metal_in = 2;
    s_w.econ[0].metal_out = 2;
    maker_set(900, 150, 50);
    maker_decide(&out, 15, &off, &on, &which);
    built[0] = builds_of(0, T_MAKER, NULL, NULL);
    printf("makers, the ladder beside a maker switched on since the pass: off=%d on=%d makers built=%d\n", off, on,
           built[0]);
    CHECK(!off && !on && !built[0], "the ladder reads the lag: no maker built");
}

/* THE BUILD TECH (impai_tinfo.c, impai_tinfo_ctech) on retail CORE's shape: the commander builds the lab and the
   vehicle plant; the lab's CK builds the advanced lab and the hover plant, the plant's CV the advanced plant and the
   hover plant; the hover plant's CH builds tech-1 buildings only; the advanced plants' ACK builds the fusion plant,
   which only it builds, so FUS is tech 2. The hover plant's build tech is 1, its units' tech 2: the offer, the
   highest tech of its mobile types, read 2. */
enum { C_VP = T_N, C_CV, C_CH, C_HP, C_ALAB, C_AVP, C_ACK, C_FUS, C_AK, C_GATOR, C_HRAID, C_CAN, C_REAP, C_SY, C_CS,
       C_N };
static void plant_world(void)
{
    static const uint16_t COMMENU[] = { T_MEX, T_SOLAR, T_FACTORY, C_VP, C_SY };
    static const uint16_t CKMENU[] = { T_MEX, T_SOLAR, C_ALAB, C_HP };
    static const uint16_t CVMENU[] = { T_MEX, T_SOLAR, C_AVP, C_HP };
    static const uint16_t CHMENU[] = { T_MEX, T_SOLAR };
    static const uint16_t ACKMENU[] = { T_MEX, C_FUS };
    static const uint16_t LABMENU[] = { T_CONS, C_AK };
    static const uint16_t VPMENU[] = { C_CV, C_GATOR };
    static const uint16_t HPMENU[] = { C_CH, C_HRAID };
    static const uint16_t ALABMENU[] = { C_ACK, C_CAN };
    static const uint16_t AVPMENU[] = { C_ACK, C_REAP };
    static const uint16_t SYMENU[] = { C_CS };
    static const uint16_t CSMENU[] = { T_MEX };
    TASIM_ROLE_OUT roles[C_N];
    int t;
    brain_world();
    s_w.ndefs = C_N;
    def_set(&s_w, T_FACTORY, "LAB", 6, 6, 0, 0, 600, 1200);
    def_set(&s_w, C_VP, "VP", 6, 6, 0, 0, 560, 1500);
    def_set(&s_w, C_CV, "CV", 2, 2, 1, 100, 140, 1600);
    def_set(&s_w, C_CH, "CH", 2, 2, 1, 100, 150, 2500);
    def_set(&s_w, C_HP, "HP", 6, 6, 0, 0, 1650, 3000);
    def_set(&s_w, C_ALAB, "ALAB", 6, 6, 0, 0, 1800, 3000);
    def_set(&s_w, C_AVP, "AVP", 6, 6, 0, 0, 2000, 3500);
    def_set(&s_w, C_ACK, "ACK", 2, 2, 1, 200, 400, 6000);
    def_set(&s_w, C_FUS, "FUS", 5, 5, 0, 0, 3000, 20000);
    def_set(&s_w, C_AK, "AK", 2, 2, 1, 0, 60, 500);
    def_set(&s_w, C_GATOR, "GATOR", 2, 2, 1, 0, 110, 900);
    def_set(&s_w, C_HRAID, "HRAID", 2, 2, 1, 0, 130, 1300);
    def_set(&s_w, C_CAN, "CAN", 2, 2, 1, 0, 450, 1700);
    def_set(&s_w, C_REAP, "REAP", 2, 2, 1, 0, 500, 1500);
    def_set(&s_w, C_SY, "SY", 6, 6, 0, 0, 450, 3000);
    s_w.defs[C_SY].e.minwater = 20;
    def_set(&s_w, C_CS, "CS", 3, 3, 1, 100, 200, 2000);
    s_w.defs[C_CS].d.mclass = 1;                                    /* the shipyard's constructor sails */
    s_w.defs[C_CH].d.mclass = s_w.defs[C_HRAID].d.mclass = 1;      /* the hovers travel by class 1 */
    menu_set(&s_w, T_COM, COMMENU, (int)(sizeof COMMENU / sizeof COMMENU[0]));
    menu_set(&s_w, T_CONS, CKMENU, (int)(sizeof CKMENU / sizeof CKMENU[0]));
    menu_set(&s_w, C_CV, CVMENU, (int)(sizeof CVMENU / sizeof CVMENU[0]));
    menu_set(&s_w, C_CH, CHMENU, (int)(sizeof CHMENU / sizeof CHMENU[0]));
    menu_set(&s_w, C_ACK, ACKMENU, (int)(sizeof ACKMENU / sizeof ACKMENU[0]));
    menu_set(&s_w, T_FACTORY, LABMENU, (int)(sizeof LABMENU / sizeof LABMENU[0]));
    menu_set(&s_w, C_VP, VPMENU, (int)(sizeof VPMENU / sizeof VPMENU[0]));
    menu_set(&s_w, C_HP, HPMENU, (int)(sizeof HPMENU / sizeof HPMENU[0]));
    menu_set(&s_w, C_ALAB, ALABMENU, (int)(sizeof ALABMENU / sizeof ALABMENU[0]));
    menu_set(&s_w, C_AVP, AVPMENU, (int)(sizeof AVPMENU / sizeof AVPMENU[0]));
    menu_set(&s_w, C_SY, SYMENU, (int)(sizeof SYMENU / sizeof SYMENU[0]));
    menu_set(&s_w, C_CS, CSMENU, (int)(sizeof CSMENU / sizeof CSMENU[0]));
    roles_set(roles);
    for (t = T_N; t < C_N; t++) {
        memset(&roles[t], 0, sizeof roles[t]);
        roles[t].role = TASIM_ROLE_OTHER;
    }
    roles[C_VP].role = roles[C_HP].role = roles[C_ALAB].role = roles[C_AVP].role = roles[C_SY].role =
        TASIM_ROLE_BUILDER;
    roles[C_VP].flags = roles[C_HP].flags = roles[C_ALAB].flags = roles[C_AVP].flags = roles[C_SY].flags =
        TASIM_RF_FACTORY;
    roles[C_CV].role = roles[C_CH].role = roles[C_ACK].role = roles[C_CS].role = TASIM_ROLE_BUILDER;
    roles[C_CV].flags = roles[C_CH].flags = roles[C_ACK].flags = roles[C_CS].flags = TASIM_RF_MOBILE;
    roles[C_AK].role = roles[C_GATOR].role = roles[C_HRAID].role = TASIM_ROLE_RAIDER;
    roles[C_CAN].role = roles[C_REAP].role = TASIM_ROLE_ASSAULT;
    roles[C_AK].flags = roles[C_GATOR].flags = roles[C_HRAID].flags = roles[C_CAN].flags = roles[C_REAP].flags =
        TASIM_RF_MOBILE;
    CHECK(content_set(&s_w, C_N, roles) == C_N, "the plant world's tinfo");
    s_ti[C_AK].klass = s_ti[C_GATOR].klass = s_ti[C_HRAID].klass = IMPAI_K_LRAIDER;
    s_ti[C_CAN].klass = s_ti[C_REAP].klass = IMPAI_K_LASSAULT;
    impai_mil_reset(&s_umil, IMPAI_LV_HARD, &IMPAI_PERSONAS[IMPAI_LV_HARD], &IMPAI_TABLE_DEFAULT, 0);
    s_umil.naval = 0;
    s_milp = &s_umil;
    s_w.econ[0].metal = s_w.econ[0].energy = s_w.econ[0].metal_max = s_w.econ[0].energy_max = 1000;
    s_w.econ[0].metal_in = 30;
    s_w.econ[0].energy_in = 100;
    s_w.econ[0].energy_out = 50;
    unit_add(&s_w, 1, T_COM, 20, 20, 1);
    s_w.gametime = 3000;
    /* the values: the advanced lab's CAN best, then the lab's AK, the plant's GATOR, the advanced plant's REAP,
       the hover raider last */
    memset(s_utv, 0, sizeof s_utv);
    for (t = 0; t < IMPAI_QTECH; t++) {
        s_utv[C_CAN].q[t] = 0.6f;
        s_utv[C_AK].q[t] = 0.4f;
        s_utv[C_GATOR].q[t] = 0.3f;
        s_utv[C_REAP].q[t] = 0.1f;
        s_utv[C_HRAID].q[t] = -0.3f;
    }
    s_tvp = s_utv;
}

static void check_ctech(void)
{
    unsigned offer = 0;
    int k;
    plant_world();
    for (k = 0; k < s_w.defs[C_HP].nmenu; k++)
        if (s_ti[s_w.defs[C_HP].menu[k]].flags & IMPAI_TF_MOBILE && s_ti[s_w.defs[C_HP].menu[k]].tech > offer)
            offer = s_ti[s_w.defs[C_HP].menu[k]].tech;
    printf("build tech: LAB %u VP %u HP %u (its units' tech, the offer: %u) ALAB %u AVP %u CK %u CH %u ACK %u "
           "FUS tech %u\n",
           s_ti[T_FACTORY].ctech, s_ti[C_VP].ctech, s_ti[C_HP].ctech, offer, s_ti[C_ALAB].ctech, s_ti[C_AVP].ctech,
           s_ti[T_CONS].ctech, s_ti[C_CH].ctech, s_ti[C_ACK].ctech, s_ti[C_FUS].tech);
    CHECK(s_ti[T_FACTORY].ctech == 1 && s_ti[C_VP].ctech == 1 && s_ti[C_HP].ctech == 1 && s_ti[C_ALAB].ctech == 2 &&
              s_ti[C_AVP].ctech == 2 && s_ti[T_CONS].ctech == 1 && s_ti[C_CH].ctech == 1 && s_ti[C_ACK].ctech == 2,
          "the build tech of retail CORE's shape");
    CHECK(offer == 2 && s_ti[C_FUS].tech == 2, "the hover plant's units are tech 2: %u", offer);
    s_tvp = NULL;
    s_milp = NULL;
}

/* THE BUILD TECH counts only the constructors that can build an extractor (owner's decision, 2026-11-14), on TA
   Zero's GoKT2AF shape: the plant makes a constructor whose menu is tech 1 with an extractor, and a jammer-builder,
   classed a constructor, whose menu is a tech-2 mine alone. The plant's build tech is 1, not 2; a plant making the
   jammer-builder alone has 0. */
static void check_ctech_mex(void)
{
    enum { X_CON = 1, X_MEX, X_JAM, X_MINE, X_PLANT, X_SOLAR, X_JPLANT, X_N };
    static const uint16_t CON[] = { X_MEX, X_SOLAR }, JAM[] = { X_MINE }, PLANT[] = { X_CON, X_JAM }, JPLANT[] = { X_JAM };
    static const uint8_t CAT[X_N] = { 0, IMPAI_C_CONS, IMPAI_C_MEX, IMPAI_C_CONS, IMPAI_C_OTHER, IMPAI_C_FACTORY,
                                      IMPAI_C_ENERGY, IMPAI_C_FACTORY };
    static const int TECH[X_N] = { 0, 1, 1, 2, 2, 1, 1, 1 };
    TASIM_ROLE_IN in[X_N];
    TASIM_ROLE_OUT roles[X_N];
    IMPAI_TINFO ti[X_N];
    int t;
    memset(in, 0, sizeof in);
    memset(roles, 0, sizeof roles);
    memset(ti, 0, sizeof ti);
    for (t = 0; t < X_N; t++) {
        roles[t].tech = TECH[t];
        ti[t].cat = CAT[t];
    }
    in[X_CON].build = CON;  in[X_CON].nbuild = 2;
    in[X_JAM].build = JAM;  in[X_JAM].nbuild = 1;
    in[X_PLANT].build = PLANT;  in[X_PLANT].nbuild = 2;
    in[X_JPLANT].build = JPLANT;  in[X_JPLANT].nbuild = 1;
    impai_tinfo_ctech(in, roles, X_N, ti, X_N);
    printf("build tech, extractor builders: CON %u JAM %u PLANT %u JPLANT %u\n", ti[X_CON].ctech, ti[X_JAM].ctech,
           ti[X_PLANT].ctech, ti[X_JPLANT].ctech);
    CHECK(ti[X_CON].ctech == 1 && ti[X_JAM].ctech == 2 && ti[X_PLANT].ctech == 1 && !ti[X_JPLANT].ctech,
          "a plant's build tech counts only its constructors that build an extractor");
}

/* THE EXTRACTOR STEP (impai_tinfo.c) on TA Zero's shape: the commander builds a tech-1 extractor (0.001307), a
   tech-1 plant and the tech-2 plant itself, so the graph reads every type tech 1; the tech-2 plant's constructor
   builds an extractor of twice the commander's (0.002614), the tech-1 plant's one of 1.14 times (TA Twilight's
   ARMAMEX against its commander's). The tech-2 constructor and plant are 2, the tech-1 ones 1, the commander 1. With
   no commander's extractor there is no step, and nothing is raised. */
static void check_ctech_step(void)
{
    enum { Y_COM = 1, Y_MEX1, Y_MEX2, Y_AMEX, Y_P1, Y_C1, Y_P2, Y_C2, Y_N };
    static const uint16_t COM[] = { Y_MEX1, Y_P1, Y_P2 }, C1[] = { Y_AMEX, Y_MEX1 }, C2[] = { Y_MEX2 },
                          P1[] = { Y_C1 }, P2[] = { Y_C2 };
    static const uint8_t CAT[Y_N] = { 0, IMPAI_C_OTHER, IMPAI_C_MEX, IMPAI_C_MEX, IMPAI_C_MEX, IMPAI_C_FACTORY,
                                      IMPAI_C_CONS, IMPAI_C_FACTORY, IMPAI_C_CONS };
    static const float GAIN[Y_N] = { 0, 0, 0.001307f, 0.002614f, 0.00149f, 0, 0, 0, 0 };
    TASIM_ROLE_IN in[Y_N];
    TASIM_ROLE_OUT roles[Y_N];
    IMPAI_TINFO ti[Y_N];
    int t, pass;
    for (pass = 0; pass < 2; pass++) {
        memset(in, 0, sizeof in);
        memset(roles, 0, sizeof roles);
        memset(ti, 0, sizeof ti);
        for (t = 1; t < Y_N; t++) {
            roles[t].tech = 1;
            ti[t].cat = CAT[t];
            ti[t].mgain = GAIN[t];
        }
        roles[Y_COM].flags = pass ? 0u : TASIM_RF_COMMANDER;
        in[Y_COM].build = COM;  in[Y_COM].nbuild = 3;
        in[Y_C1].build = C1;  in[Y_C1].nbuild = 2;
        in[Y_C2].build = C2;  in[Y_C2].nbuild = 1;
        in[Y_P1].build = P1;  in[Y_P1].nbuild = 1;
        in[Y_P2].build = P2;  in[Y_P2].nbuild = 1;
        impai_tinfo_ctech(in, roles, Y_N, ti, Y_N);
        printf("build tech, the extractor step%s: COM %u C1 %u P1 %u C2 %u P2 %u\n", pass ? " (no commander)" : "",
               ti[Y_COM].ctech, ti[Y_C1].ctech, ti[Y_P1].ctech, ti[Y_C2].ctech, ti[Y_P2].ctech);
        if (!pass)
            CHECK(ti[Y_COM].ctech == 1 && ti[Y_C1].ctech == 1 && ti[Y_P1].ctech == 1 && ti[Y_C2].ctech == 2 &&
                      ti[Y_P2].ctech == 2,
                  "the tech-2 plant past the extractor step");
        else
            CHECK(ti[Y_C2].ctech == 1 && ti[Y_P2].ctech == 1, "no commander's extractor, no step");
    }
}

/* THE CONTENT'S PRICE OF METAL (impai_tinfo.c, impai_tinfo_e_per_m): the lower median of energy per metal over the
   tech-1 types of an army class that cost metal -- five priced (2, 4, 7, 9, 12: 7), a tech-2 type, a type of no
   class and a free one beside them left out; four priced: the lower of the middle two. Then the keep it sets: the
   commander alone with its store full and no stall, metal coming in at 20 and energy at 50: at the plan's 8 the
   keep, 160, is past the energy income and its first build is an energy building; at a price of 1 the keep is 20,
   below it, and no energy building is ordered. */
static void check_content_price(void)
{
    enum { P_N = 9 };
    static const float E[P_N] = { 0, 200, 400, 700, 900, 1200, 10000, 50, 300 };
    static const float MC[P_N] = { 0, 100, 100, 100, 100, 100, 100, 100, 0 };
    static const int TECH[P_N] = { 0, 1, 1, 1, 1, 1, 2, 1, 1 };
    TASIM_ROLE_IN in[P_N];
    TASIM_ROLE_OUT roles[P_N];
    IMPAI_TINFO ti[P_N];
    IMPAI_BRAIN_OUT out;
    float five, four;
    int t, c, k, ebuilds[2];
    memset(in, 0, sizeof in);
    memset(roles, 0, sizeof roles);
    memset(ti, 0, sizeof ti);
    for (t = 1; t < P_N; t++) {
        in[t].cost_energy = E[t];
        in[t].cost_metal = MC[t];
        roles[t].tech = TECH[t];
        ti[t].klass = t == 7 ? IMPAI_K_NONE : IMPAI_K_LASSAULT;
    }
    five = impai_tinfo_e_per_m(in, roles, ti, P_N, P_N);
    four = impai_tinfo_e_per_m(in, roles, ti, P_N - 4, P_N);
    printf("the content's price of metal: five priced %.2f, four %.2f\n", (double)five, (double)four);
    CHECK(five == 7.0f && four == 4.0f, "the lower median: %g %g", (double)five, (double)four);
    for (c = 0; c < 2; c++) {
        reach_world(0);
        s_w.econ[0].energy = s_w.econ[0].energy_max = 1000;
        s_w.econ[0].metal = s_w.econ[0].metal_max = 1000;
        s_w.econ[0].energy_in = 50;
        s_w.econ[0].energy_out = 10;
        s_w.econ[0].metal_in = 20;
        s_dec_epm = c ? 1.0f : 0.0f;
        CHECK(decide(&out) == 0, "decide");
        ebuilds[c] = 0;
        for (k = 0; k < s_w.nlog; k++)
            ebuilds[c] += s_w.log[k].c.action == A_MOBILEBUILD && s_w.log[k].c.type < T_N &&
                        s_ti[s_w.log[k].c.type].cat == IMPAI_C_ENERGY;
    }
    s_dec_epm = 0.0f;
    printf("the content's price of metal, the keep: energy builds at the plan's %d, at a price of 1 %d\n", ebuilds[0],
           ebuilds[1]);
    CHECK(ebuilds[0] == 1 && ebuilds[1] == 0, "the keep follows the content's price: %d %d", ebuilds[0], ebuilds[1]);
}

/* one decision of the plant world; the plant builds it gave, by type, into n[C_N] */
static void plant_decide(const char* name, IMPAI_BRAIN_OUT* out, int* n)
{
    static const int PLANTS[] = { T_FACTORY, C_VP, C_HP, C_ALAB, C_AVP, C_SY };
    const int from = s_w.nlog;
    unsigned k;
    CHECK(decide(out) == 0, "decide: %s", name);
    memset(n, 0, C_N * sizeof *n);
    for (k = 0; k < sizeof PLANTS / sizeof PLANTS[0]; k++) n[PLANTS[k]] = builds_of(from, PLANTS[k], NULL, NULL);
    printf("plant choice %s: lab=%d vp=%d hp=%d alab=%d avp=%d sy=%d plant=%u want=%u T=%u reach=%08x s=%.2f "
           "next=%u s=%.2f\n", name, n[T_FACTORY], n[C_VP], n[C_HP], n[C_ALAB], n[C_AVP], n[C_SY], out->plant,
           out->plant_want, out->plant_T, (unsigned)out->plant_reach, (double)out->plant_s, out->plant_next,
           (double)out->plant_next_s);
}

/* a constructor of `type` at slot `slot`, finished, busy when `busy` with a list of two moves -- another hand's
   orders: a lone move with no job is the economy's (impai_brain.c, THE HAND-BACK) */
static void plant_builder(int slot, int type, int cx, int cz, int busy)
{
    int k;
    unit_add(&s_w, slot, type, cx, cz, 1);
    if (!busy) return;
    for (k = 0; k < 2; k++) {
        s_w.unit[slot].list[k].action = A_MOVE;
        s_w.unit[slot].list[k].target = (uint16_t)WQ_NONE;
        s_w.unit[slot].list[k].pos[0] = s_w.unit[slot].pos[0];
        s_w.unit[slot].list[k].pos[2] = s_w.unit[slot].pos[2];
    }
    s_w.unit[slot].n = 2;
}

/* the labels of classes 0 (land) and 1 (hover): the land class split at block x 32 when `split`, the hover
   class whole; the estimate at block (50, 10), the anchor's block (10, 10) */
static uint16_t s_plabels[2 * (FW_W / 2) * (FW_H / 2)];
static void plant_reach(int split)
{
    const unsigned blocks = (FW_W / 2) * (FW_H / 2);
    unsigned i;
    for (i = 0; i < blocks; i++) {
        s_plabels[i] = (uint16_t)(split && i % (FW_W / 2) >= 32 ? 2 : 1);
        s_plabels[blocks + i] = 1;
    }
    memset(&s_reach, 0, sizeof s_reach);
    s_reach.labels = s_plabels;
    s_reach.at[0] = 0;
    s_reach.at[1] = blocks;
    s_reach.present = 3u;
    s_reach.bw = FW_W / 2;
    s_reach.bh = FW_H / 2;
    s_umil.ebx = 50;
    s_umil.ebz = 10;
}

/* THE METAL'S REACH's world: the plant world with one lab standing at cells (30, 30) and an idle CK; the land
   class's labels 1 everywhere but, when `cut`, 2 at the three patches' anchor blocks, the hover class's 1 whole --
   no land constructor reaches a patch, the hover plant's CH every one; the estimate at block (50, 10). Income 1 a
   pass: no plant is wanted for the income. */
static void mr_world(int cut)
{
    const unsigned blocks = (FW_W / 2) * (FW_H / 2);
    unsigned i;
    plant_world();
    for (i = 0; i < blocks; i++) s_plabels[i] = s_plabels[blocks + i] = 1;
    if (cut)
        for (i = 0; i < NPATCH; i++) s_plabels[(unsigned)(PATCHES[i].az >> 1) * (FW_W / 2) + (PATCHES[i].ax >> 1)] = 2;
    memset(&s_reach, 0, sizeof s_reach);
    s_reach.labels = s_plabels;
    s_reach.at[0] = 0;
    s_reach.at[1] = blocks;
    s_reach.present = 3u;
    s_reach.bw = FW_W / 2;
    s_reach.bh = FW_H / 2;
    s_umil.ebx = 50;
    s_umil.ebz = 10;
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_builder(4, T_CONS, 26, 24, 0);
    s_w.econ[0].metal_in = 1;
}

/* the constructors of `type` queued since log entry `from` */
static int queued_of(int from, int type)
{
    int k, n = 0;
    for (k = from; k < s_w.nlog; k++) n += s_w.log[k].c.verb == WQ_CMD_QUEUE && s_w.log[k].c.type == type;
    return n;
}

/* THE METAL'S REACH (impai_econ.c) over the plant world's three patches:
   (a) no land constructor reaches one, at an income no plant is wanted for: the hover plant, whose CH does;
   (b) the land class reaching them: no plant;
   (c) three extractors standing, as many as the open patches: no plant; two: the hover plant;
   (d) a finished CH: no plant; (e) a hover plant standing: no plant;
   (f) at the income the second plant is wanted for, the patches cut off: the hover plant, never the lab;
   (g) the same with the hover plant of build tech 2, past the seat's 1: the lab, THE CHOICE's own;
   (h) THE GOVERNOR'S CONSTRUCTOR, with no constructor finished, of a lab making the CK and the CH: the CH while
       the CK reaches no patch, the CK -- the better workertime for its value -- while it does;
   (i) the extractor water-only, minwaterdepth 20, over the dry patches: the CH reaches them but builds nothing on
       them, so no plant;
   (j) the seat judged naval, its commander -- busy with another hand's moves -- the only builder of a shipyard
       that makes a sea raider: the reach's want is THE CHOICE's, the hover plant, and never the sea plant's, which
       no idle builder could order (Steel Jungle ca-1's seat);
   (k) a lab frame beside the finished lab, and (l) a lab frame alone: no plant, since a frame's constructors are
       not yet the seat's to judge. */
static void check_metal_reach(void)
{
    static const uint16_t LABMENU2[] = { T_CONS, C_CH, C_AK };
    IMPAI_BRAIN_OUT out;
    int n[C_N], c, from;
    mr_world(1);
    plant_decide("the reach, the metal cut off", &out, n);
    CHECK(n[C_HP] == 1 && out.plant == C_HP && out.plant_want == 2 && !n[T_FACTORY] && !n[C_VP],
          "(a) the metal out of the land constructors' reach: the hover plant");
    mr_world(0);
    plant_decide("the reach, the metal reached", &out, n);
    CHECK(!out.plant && !n[C_HP], "(b) the metal in reach: no plant");
    for (c = 3; c >= 2; c--) {
        int k;
        mr_world(1);
        for (k = 0; k < c; k++) unit_add(&s_w, 20 + k, T_MEX, 60 + 2 * k, 60, 1);
        plant_decide(c == 3 ? "the reach, three extractors held" : "the reach, two extractors held", &out, n);
        CHECK(c == 3 ? !out.plant && !n[C_HP] : out.plant == C_HP && n[C_HP] == 1,
              "(c) %d extractors against 3 open patches: plant %u", c, out.plant);
    }
    mr_world(1);
    plant_builder(6, C_CH, 24, 26, 0);
    plant_decide("the reach, a CH finished", &out, n);
    CHECK(!out.plant && !n[C_HP], "(d) a finished CH reaches the metal: no plant");
    mr_world(1);
    unit_add(&s_w, 11, C_HP, 40, 40, 1);
    plant_decide("the reach, a hover plant standing", &out, n);
    CHECK(!out.plant && !n[C_HP], "(e) a hover plant standing makes the CH: no plant");
    mr_world(1);
    s_w.econ[0].metal_in = 30;
    plant_decide("the reach, the second plant's income", &out, n);
    CHECK(out.plant == C_HP && n[C_HP] == 1 && !n[T_FACTORY], "(f) the second plant, the metal cut off: the hover plant");
    mr_world(1);
    s_w.econ[0].metal_in = 30;
    s_ti[C_HP].ctech = 2;
    plant_decide("the reach, the hover plant past the build tech", &out, n);
    s_ti[C_HP].ctech = 1;
    CHECK(out.plant == T_FACTORY && n[T_FACTORY] == 1 && !n[C_HP],
          "(g) the hover plant past the seat's build tech: the lab, THE CHOICE's own");
    for (c = 1; c >= 0; c--) {
        mr_world(c);
        s_w.unit[4].live = 0;
        menu_set(&s_w, T_FACTORY, LABMENU2, (int)(sizeof LABMENU2 / sizeof LABMENU2[0]));
        from = s_w.nlog;
        plant_decide(c ? "the reach, the governor, the metal cut off" : "the reach, the governor, the metal reached",
                     &out, n);
        printf("the reach, the governor, cut %d: CK %d CH %d queued\n", c, queued_of(from, T_CONS),
               queued_of(from, C_CH));
        CHECK(c ? queued_of(from, C_CH) == 1 && !queued_of(from, T_CONS)
                : queued_of(from, T_CONS) == 1 && !queued_of(from, C_CH),
              "(h) the governor's constructor, cut %d", c);
    }
    mr_world(1);
    s_ti[T_MEX].flags |= IMPAI_TF_WATER;
    s_w.defs[T_MEX].e.minwater = 20;
    plant_decide("the reach, the extractor water-only", &out, n);
    s_ti[T_MEX].flags &= ~IMPAI_TF_WATER;
    s_w.defs[T_MEX].e.minwater = 0;
    CHECK(!out.plant && !n[C_HP], "(i) a water-only extractor over dry patches: no plant");
    {
        static const uint16_t SYMENU2[] = { C_CS, C_HRAID };
        mr_world(1);
        menu_set(&s_w, C_SY, SYMENU2, (int)(sizeof SYMENU2 / sizeof SYMENU2[0]));
        s_ti[C_HRAID].klass = IMPAI_K_SRAIDER;
        plant_builder(1, T_COM, 20, 20, 1);
        s_umil.naval = 1;
        plant_decide("the reach, a naval seat", &out, n);
        s_umil.naval = 0;
        s_ti[C_HRAID].klass = IMPAI_K_LRAIDER;
        CHECK(out.plant == C_HP && n[C_HP] == 1 && !n[C_SY], "(j) a naval seat short of the reach: the hover plant");
    }
    for (c = 0; c < 2; c++) {
        mr_world(1);
        if (c) s_w.unit[10].live = 0;
        unit_add(&s_w, 12, T_FACTORY, 40, 40, 0);
        plant_decide(c ? "the reach, a lab frame alone" : "the reach, a lab frame beside the lab", &out, n);
        CHECK(!out.plant && !n[C_HP], "(%c) a plant frame standing: no plant, plant %u", c ? 'l' : 'k', out.plant);
    }
}

/* THE CHOICE (impai_econ.c) over the seat's menus, the build tech, the reach, the wet anchor and the ties */
static void check_plant_choice(void)
{
    IMPAI_BRAIN_OUT out;
    int n[C_N];
    /* THE SEAT'S MENUS: a tech-up with a lab and a vehicle plant standing. The idle CV's menu holds the advanced
       plant and the hover plant; the busy CK's holds the advanced lab, whose CAN is the best unit. The CV orders no
       plant and the want holds; the CK idle at the next decision orders the advanced lab */
    plant_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, C_VP, 40, 35, 1);
    plant_builder(3, C_CV, 24, 26, 0);
    plant_builder(4, T_CONS, 26, 24, 1);
    plant_decide("the seat's menus, the CK busy", &out, n);
    CHECK(!n[C_ALAB] && !n[C_AVP] && !n[C_HP] && !out.plant, "the CV orders no plant its menu lacks");
    s_w.unit[4].n = 0;
    s_w.gametime += 15;
    plant_decide("the seat's menus, the CK idle", &out, n);
    CHECK(n[C_ALAB] == 1 && !n[C_AVP] && !n[C_HP] && out.plant == C_ALAB && out.plant_want == 3 &&
              out.plant_next == C_AVP && out.plant_T == 2,
          "the CK orders the advanced lab, the seat's choice, the advanced plant the runner-up, scored at tech 2");
    /* THE ORDINARY CHOICE: one lab standing; the advanced lab's unit is the best, but the second plant is of the
       seat's build tech, 1: the lab, its AK the best of them, the vehicle plant the runner-up */
    plant_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_builder(4, T_CONS, 26, 24, 0);
    plant_decide("the second plant", &out, n);
    CHECK(n[T_FACTORY] == 1 && !n[C_ALAB] && !n[C_AVP] && !n[C_HP] && out.plant == T_FACTORY && out.plant_want == 2 &&
              out.plant_T == 1 && out.plant_s == 0.4f && out.plant_next == C_VP && out.plant_next_s == 0.3f,
          "the second plant is the lab, by its AK");
    /* THE TARGETS BOUGHT FOR in THE SCORE: the second plant with the AK's weapons waterweapons alone and a foe's
       start on land, so DRY alone expected: the AK hits nothing the seat buys for, the lab has no land army, and the
       vehicle plant, by its GATOR, is the plant */
    plant_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_builder(4, T_CONS, 26, 24, 0);
    s_ti[C_AK].flags = (uint8_t)((s_ti[C_AK].flags & ~IMPAI_TF_HITDRY) | IMPAI_TF_HITWET);
    s_w.starts[0] = (WQ_START){ 328, 328, 0, 1 };
    s_w.starts[1] = (WQ_START){ 50 * 32 + 16, 10 * 32 + 16, 1, 2 };
    s_w.nstarts = 2;
    plant_decide("the second plant, the AK's waterweapons", &out, n);
    s_w.nstarts = 0;
    CHECK(n[C_VP] == 1 && !n[T_FACTORY] && out.plant == C_VP && out.plant_s == 0.3f,
          "the AK hits no target the seat buys for: the vehicle plant");
    /* THE REACH: the land class's labels differ between the anchor's block and the estimate's, the hover class's
       agree: the hover plant, whose CK builds it; with the labels equal the lab */
    plant_world();
    plant_reach(1);
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_builder(4, T_CONS, 26, 24, 0);
    plant_decide("the land route missing", &out, n);
    CHECK(n[C_HP] == 1 && !n[T_FACTORY] && !n[C_VP] && out.plant == C_HP && out.plant_s == -0.3f &&
              out.plant_next_s == IMPAI_Q_NONE && out.plant_reach == ~1u,
          "no land class reaches: the hover plant");
    plant_world();
    plant_reach(0);
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_builder(4, T_CONS, 26, 24, 0);
    plant_decide("the land route there", &out, n);
    CHECK(n[T_FACTORY] == 1 && !n[C_HP] && out.plant_reach == ~0u, "the land route there: the lab");
    /* THE TARGETS are THE FOE'S STARTS: the route east missing, the seat's start at the anchor, a start on its own
       side at block (20, 10) and a foe's at (50, 10) across. Left empty, the near start is no target and the land
       class reaches none: the hover plant; seated by a foe, it is a target: the lab */
    for (int c = 0; c < 2; c++) {
        plant_world();
        plant_reach(1);
        s_w.starts[0] = (WQ_START){ 328, 328, 0, 1 };
        s_w.starts[1] = (WQ_START){ 20 * 32 + 16, 10 * 32 + 16, 1, c ? 3 : WQ_SEATED_NONE };
        s_w.starts[2] = (WQ_START){ 50 * 32 + 16, 10 * 32 + 16, 2, 2 };
        s_w.nstarts = 3;
        unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
        plant_builder(4, T_CONS, 26, 24, 0);
        plant_decide(c ? "the near start a foe's" : "the near start left empty", &out, n);
        CHECK(c ? n[T_FACTORY] == 1 && !n[C_HP] && out.plant_reach == ~0u
                : n[C_HP] == 1 && !n[T_FACTORY] && out.plant_reach == ~1u,
              "the near start %s", c ? "a foe's: the lab" : "left empty, no target: the hover plant");
    }
    s_w.nstarts = 0;
    /* THE BASE'S COMPONENT: the land class's anchor block and the two rings round it impassable, the route
       missing: the nearest labelled block is the base's component, so the land class does not reach -- the
       anchor's 0 is no evidence that it does */
    plant_world();
    plant_reach(1);
    {
        int bx, bz;
        for (bz = 8; bz <= 12; bz++)
            for (bx = 8; bx <= 12; bx++) s_plabels[bz * (FW_W / 2) + bx] = 0;
    }
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_builder(4, T_CONS, 26, 24, 0);
    plant_decide("the anchor's block impassable", &out, n);
    CHECK(n[C_HP] == 1 && !n[T_FACTORY] && out.plant_reach == ~1u, "the anchor's block impassable: the hover plant");
    /* THE BASE'S COMPONENT BY MOST BLOCKS: the anchor's own block a one-block pocket of another label, every block
       round it the estimate's: the land class reaches, the lab */
    plant_world();
    plant_reach(0);
    s_plabels[10 * (FW_W / 2) + 10] = 3;
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_builder(4, T_CONS, 26, 24, 0);
    plant_decide("a pocket at the anchor", &out, n);
    CHECK(n[T_FACTORY] == 1 && !n[C_HP] && out.plant_reach == ~0u, "a pocket at the anchor: the lab");
    /* THE TARGETS: the estimate, the nearest other start, off the land component; a third start on it. Before an
       enemy structure is seen every other start is a target, and the land class reaches one: the lab. With the
       estimate resting on structures, seen now or kept, the estimate alone: the hover plant. A recorded est_seen past
       the enum reads as none seen, as the military reads it (impai_est_known) */
    {
        static const WQ_START STARTS[3] = { { 328, 328, 0, 0 }, { 1616, 336, 1, 0 }, { 336, 1616, 2, 0 } };
        static const uint8_t SEEN[4] = { EST_START, EST_STRUCT, EST_KEPT, 7 };
        int c;
        for (c = 0; c < 4; c++) {
            const int seen = SEEN[c] == EST_STRUCT || SEEN[c] == EST_KEPT;
            plant_world();
            plant_reach(1);
            memcpy(s_w.starts, STARTS, sizeof STARTS);
            s_w.nstarts = 3;
            s_umil.est_seen = SEEN[c];
            unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
            plant_builder(4, T_CONS, 26, 24, 0);
            plant_decide(seen ? "the starts, structures seen" : "the starts, none seen", &out, n);
            CHECK(seen ? n[C_HP] == 1 && !n[T_FACTORY] && out.plant_reach == ~1u
                       : n[T_FACTORY] == 1 && !n[C_HP] && out.plant_reach == ~0u,
                  "the starts, est_seen %u: %s", SEEN[c],
                  seen ? "the estimate alone, the hover plant" : "one start reached, the lab");
        }
    }
    /* THE UNLOCKS: a tech-up with a lab and a vehicle plant standing, a CV the only constructor and two raiders
       built. The advanced lab's CAN is the best unit, and only a CK, which the lab makes, builds it: no plant is
       ordered, and the lab's constructor is the CK. With the CK a frame, no unlock; with it finished and idle, the
       advanced lab */
    plant_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, C_VP, 40, 35, 1);
    plant_builder(3, C_CV, 24, 26, 1);
    unit_add(&s_w, 6, C_AK, 50, 50, 1);
    unit_add(&s_w, 7, C_AK, 52, 50, 1);
    {
        const int from = s_w.nlog;
        int k, ck = 0, at = 0;
        plant_decide("the unlock", &out, n);
        for (k = from; k < s_w.nlog; k++)
            if (s_w.log[k].c.verb == WQ_CMD_QUEUE && s_w.log[k].c.type == T_CONS) {
                ck++;
                at = s_w.log[k].c.unit;
            }
        printf("plant choice the unlock: CK queued %d at slot %d, unlock %u for %u\n", ck, at, out.unlock,
               out.unlock_plant);
        CHECK(!out.plant && !n[C_AVP] && !n[C_HP] && ck == 1 && at == 10 && out.unlock == T_CONS &&
                  out.unlock_plant == C_ALAB,
              "the lab makes the CK the advanced lab needs");
    }
    {
        /* THE KEPT UNLOCK: the next decision with the steady income under the tech-up's, so no plant is wanted and
           the choice does not run, the lab busy with a raider and the vehicle plant idle: the CK is still the
           governor's constructor, and the vehicle plant, which does not make it, queues no CV. Then a CK finished,
           still with no plant wanted: the unlock is dropped */
        const int from = s_w.nlog;
        const float in0 = s_w.econ[0].metal_in;
        FUNIT* lab = &s_w.unit[10];
        int k, cv = 0;
        memset(lab->list, 0, sizeof lab->list);
        lab->list[0].action = A_BUILDINGBUILD;
        lab->list[0].type = C_AK;
        lab->list[0].target = (uint16_t)WQ_NONE;
        lab->list[0].count = 1;
        lab->n = 1;
        s_w.econ[0].metal_in = 1;
        s_w.gametime += 15;
        plant_decide("the unlock kept", &out, n);
        for (k = from; k < s_w.nlog; k++) cv += s_w.log[k].c.verb == WQ_CMD_QUEUE && s_w.log[k].c.type == C_CV;
        printf("plant choice the unlock kept: CV queued %d, kept unlock %u for %u\n", cv, s_st.unlock,
               s_st.unlock_plant);
        CHECK(!cv && s_st.unlock == T_CONS && s_st.unlock_plant == C_ALAB,
              "no plant wanted: the unlock kept, %d CVs queued", cv);
        /* a CK finished while still no plant is wanted: no unlock is left to keep */
        unit_add(&s_w, 5, T_CONS, 26, 24, 1);
        s_w.gametime += 15;
        plant_decide("the unlock kept, its CK finished", &out, n);
        printf("plant choice the unlock kept, its CK finished: kept unlock %u for %u\n", s_st.unlock, s_st.unlock_plant);
        CHECK(!s_st.unlock && !s_st.unlock_plant, "a finished CK with no plant wanted: the kept unlock dropped");
        s_w.unit[5].live = 0;
        impai_job_end(&s_job[5]);
        s_w.econ[0].metal_in = in0;
        lab->n = 0;
    }
    unit_add(&s_w, 5, T_CONS, 32, 26, 0);
    s_w.gametime += 15;
    plant_decide("the unlock's frame", &out, n);
    CHECK(!out.plant && !out.unlock, "the CK a frame: no unlock, no plant");
    unit_add(&s_w, 5, T_CONS, 26, 24, 1);
    s_w.gametime += 15;
    plant_decide("the unlock done", &out, n);
    CHECK(n[C_ALAB] == 1 && out.plant == C_ALAB && out.plant_s == 0.6f, "the CK orders the advanced lab");
    CHECK(!s_st.unlock && !s_st.unlock_plant, "the unlock's constructor finished: none kept");
    /* THE UNLOCK AT THE CAP: the same seat with cons_max constructors, CVs, one of them idle: no constructor can
       come, so no unlock is a candidate, and the idle CV orders the advanced plant, the best of the seat's own */
    plant_world();
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, C_VP, 40, 35, 1);
    {
        const int from = s_w.nlog;
        int k, ck = 0;
        for (k = 0; k < IMPAI_PLAN_ROW.cons_max; k++)
            plant_builder(20 + k, C_CV, 20 + 2 * (k % 6), 40 + 2 * (k / 6), k > 0);
        for (k = 0; k < 2 * IMPAI_PLAN_ROW.cons_max; k++) unit_add(&s_w, 40 + k, C_AK, 60, 60 + k, 1);
        plant_decide("the unlock at the cap", &out, n);
        for (k = from; k < s_w.nlog; k++) ck += s_w.log[k].c.verb == WQ_CMD_QUEUE && s_w.log[k].c.type == T_CONS;
        CHECK(n[C_AVP] == 1 && out.plant == C_AVP && !out.unlock && !ck, "at cons_max the seat's own plant");
    }
    /* THE UNLOCK AT THE ATTENTION'S CAP: the same seat under a persona of att_eco 4, four CVs, one of them idle,
       below cons_max: the governor keeps no more constructors than the seat keeps busy, so no constructor can come
       here either, no unlock is a candidate, and the idle CV orders the advanced plant */
    {
        IMPAI_PERSONA p = IMPAI_PERSONAS[IMPAI_LV_HARD];
        int k, from, ck = 0;
        p.att_eco = 4;
        CHECK(impai_persona_ok(&p), "the persona of att_eco 4 is a persona");
        plant_world();
        unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
        unit_add(&s_w, 11, C_VP, 40, 35, 1);
        for (k = 0; k < 4; k++) plant_builder(20 + k, C_CV, 20 + 2 * k, 40, k > 0);
        for (k = 0; k < 8; k++) unit_add(&s_w, 40 + k, C_AK, 60, 60 + k, 1);
        from = s_w.nlog;
        s_persona = &p;
        plant_decide("the unlock at the attention's cap", &out, n);
        s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
        for (k = from; k < s_w.nlog; k++) ck += s_w.log[k].c.verb == WQ_CMD_QUEUE && s_w.log[k].c.type == T_CONS;
        CHECK(n[C_AVP] == 1 && out.plant == C_AVP && !out.unlock && !ck,
              "at att_eco constructors the seat's own plant: plant %u unlock %u, %d CK queued", out.plant, out.unlock,
              ck);
    }
    /* THE UNLOCK WITH NO VALUES: the advanced lab, the cheaper, waits for a CK and the advanced plant does not:
       with no scores the seat's own plant goes first, the choice as with no unlocks */
    plant_world();
    s_tvp = NULL;
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    unit_add(&s_w, 11, C_VP, 40, 35, 1);
    plant_builder(3, C_CV, 24, 26, 0);
    unit_add(&s_w, 6, C_AK, 50, 50, 1);
    unit_add(&s_w, 7, C_AK, 52, 50, 1);
    plant_decide("the unlock with no values", &out, n);
    CHECK(n[C_AVP] == 1 && out.plant == C_AVP && !out.unlock, "with no values the seat's own plant");
    /* THE WET ANCHOR: the commander below the sea, a lab standing on land far off, the lab's AK rated first: the
       least value, the shipyard */
    plant_world();
    s_w.sea = 50;
    water_add(&s_w, 0, 0, 40, 63, 40);
    unit_add(&s_w, 10, T_FACTORY, 100, 100, 1);
    plant_decide("a wet anchor", &out, n);
    CHECK(n[C_SY] == 1 && !n[T_FACTORY] && !n[C_VP] && out.plant == C_SY, "a wet anchor: the least value");
    /* TIES: the lab's AK and the plant's GATOR rated alike: the least value, the vehicle plant */
    plant_world();
    s_utv[C_GATOR] = s_utv[C_AK];
    unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
    plant_decide("tied scores", &out, n);
    CHECK(n[C_VP] == 1 && !n[T_FACTORY] && out.plant_next == T_FACTORY, "equal scores: the least value");
    /* THE OPENING: no factory yet, the lab's AK the best: the least value with a land army, the vehicle plant
       (owner's decision) */
    plant_world();
    s_w.gametime = 2000;
    plant_decide("the opening", &out, n);
    CHECK(n[C_VP] == 1 && !n[T_FACTORY] && out.plant_want == 1, "the first plant is the least value");
    /* the vehicle plant's GATOR no land army (TA Zero's air plant): the lab, the cheapest plant that has one */
    plant_world();
    s_ti[C_GATOR].klass = IMPAI_K_NONE;
    s_w.gametime = 2000;
    plant_decide("the opening, the cheapest plant with no land army", &out, n);
    CHECK(n[T_FACTORY] == 1 && !n[C_VP] && out.plant_want == 1, "the first plant is the cheapest with a land army");
    /* and with no plant that has one, the least value */
    plant_world();
    s_ti[C_GATOR].klass = s_ti[C_AK].klass = IMPAI_K_NONE;
    s_w.gametime = 2000;
    plant_decide("the opening, no land army anywhere", &out, n);
    CHECK(n[C_VP] == 1 && !n[T_FACTORY] && out.plant_want == 1, "with no land army anywhere the least value");
    /* THE BLIND START (impai_brain.c): before open_t, no factory, the scan whole and no patch published: the first
       plant at once */
    plant_world();
    s_w.gametime = 300;
    s_spots = NULL;
    s_nspots = 0;
    s_scanned = 1;
    plant_decide("a blind start", &out, n);
    CHECK(n[C_VP] == 1 && !n[T_FACTORY] && out.plant_want == 1, "a blind start: the first plant before open_t");
    /* the scan still under way: none published yet is no evidence, and the opening waits as before */
    plant_world();
    s_w.gametime = 300;
    s_spots = NULL;
    s_nspots = 0;
    plant_decide("a scan under way", &out, n);
    CHECK(!n[C_VP] && !n[T_FACTORY] && !n[C_SY] && !out.plant, "before a whole scan: no plant before open_t");
    /* a patch in sight: the opening's extractors first, as before */
    plant_world();
    s_w.gametime = 300;
    s_scanned = 1;
    plant_decide("a start with patches", &out, n);
    CHECK(!n[C_VP] && !n[T_FACTORY] && !n[C_SY] && !out.plant, "patches known: no plant before open_t");
    /* a metal map: no patch is ever published, and the extractors take the base ring */
    plant_world();
    s_w.gametime = 300;
    s_w.threshold = 255;
    s_spots = NULL;
    s_nspots = 0;
    s_scanned = 1;
    plant_decide("a metal map", &out, n);
    CHECK(!n[C_VP] && !n[T_FACTORY] && !n[C_SY] && !out.plant, "a metal map: no plant before open_t");
    s_tvp = NULL;
    s_milp = NULL;
    brain_world();
}

/* THE VALUES (impai_tinfo.c, impai_tinfo_values) over the military's fight content: its cheap fast raider and slow
   tank, a slow long-range artillery unit and a fragile cheap long-range homing unit; the destroyer's class is
   taken away, so the gated types are those four. */
enum { V_ARTY = M_N, V_HOMING, V_PAST, V_N };
static IMPAI_FWORK s_vwork;
static void values_content(unsigned raider, unsigned tank, unsigned arty, unsigned homing)
{
    static const struct { int t; uint8_t klass; } K[4] = {
        { M_RAIDER, IMPAI_K_LRAIDER }, { M_TANK, IMPAI_K_LASSAULT }, { V_ARTY, IMPAI_K_LARTY },
        { V_HOMING, IMPAI_K_LRAIDER } };
    const unsigned tech[4] = { raider, tank, arty, homing };
    int k;
    mil_world();
    mil_def(V_ARTY, "ARTY", 250, 700, 50000, 900, 120);
    s_fwp[V_ARTY].reload = 120;
    mil_def(V_HOMING, "HOMING", 140, 150, 90000, 900, 60);
    s_fwp[V_HOMING].kind = IMPAI_FK_HOMING;
    s_fwp[V_HOMING].velocity = 3 << 15;
    s_fwp[V_HOMING].reload = 60;
    s_fc.nt = s_fc.nw = V_PAST;
    s_ti[M_DEST].klass = IMPAI_K_NONE;
    for (k = 0; k < 4; k++) {
        IMPAI_TINFO* t = &s_ti[K[k].t];
        if (K[k].t >= M_N) memset(t, 0, sizeof *t);
        t->klass = K[k].klass;
        t->flags |= IMPAI_TF_MOBILE;
        t->value = s_ft[K[k].t].value;
        t->tech = (uint8_t)tech[k];
    }
}

static void values_print(const char* name, const IMPAI_TVALUE* tv, const IMPAI_TVALUE_STATS* st)
{
    printf("values %s: types=%u fields=%u,%u,%u,%u runs=%u refused=%u raider %.4f %.4f tank %.4f %.4f arty %.4f "
           "%.4f homing %.4f %.4f\n", name, st->scored, st->field[0], st->field[1], st->field[2], st->field[3],
           st->runs, st->refused, (double)tv[M_RAIDER].q[0], (double)tv[M_RAIDER].q[1], (double)tv[M_TANK].q[0],
           (double)tv[M_TANK].q[1], (double)tv[V_ARTY].q[0], (double)tv[V_ARTY].q[1], (double)tv[V_HOMING].q[0],
           (double)tv[V_HOMING].q[1]);
}

static void check_values(void)
{
    static IMPAI_TVALUE tv[64];
    static IMPAI_FTYPE ft[41];
    static IMPAI_FWEAPON fwp[3];
    static IMPAI_TINFO ti[41];
    IMPAI_TVALUE_STATS st;
    IMPAI_FCONTENT c;
    int t, k, inrange = 1, bad = 0;
    /* the raider alone at tech 1: a field of one type gives it 0, and the homing unit loses to it; the field of
       tech 2 holds all four, and tech 3 and 4 copy it */
    values_content(1, 2, 2, 2);
    CHECK(impai_tinfo_values(&s_fc, &impai_fitparams, s_ti, V_PAST, tv, &s_vwork, &st) == 0, "the values build");
    values_print("raider at tech 1", tv, &st);
    for (t = 0; t < V_PAST; t++)
        for (k = 0; k < IMPAI_QTECH; k++) inrange &= tv[t].q[k] >= -1.0f && tv[t].q[k] <= 1.0f;
    CHECK(inrange, "every value in [-1, 1]");
    CHECK(st.scored == 4 && st.field[0] == 1 && st.field[1] == 4 && st.field[2] == 4 && st.field[3] == 4 &&
              st.runs == 4 * (1 + 4) && !st.refused,
          "four types scored on fields of 1 and 4, tech 3 and 4 copied: %u runs", st.runs);
    CHECK(tv[M_RAIDER].q[0] == 0.0f, "a field of one type gives it 0: %g", (double)tv[M_RAIDER].q[0]);
    CHECK(tv[M_RAIDER].q[0] > tv[V_HOMING].q[0], "the raider's value beats the homing unit's at tech 1");
    CHECK(tv[V_HOMING].q[2] == tv[V_HOMING].q[1] && tv[V_HOMING].q[3] == tv[V_HOMING].q[1],
          "equal fields are copied");
    /* the tank alone at tech 1: the homing unit beats it one to one */
    values_content(2, 1, 2, 2);
    CHECK(impai_tinfo_values(&s_fc, &impai_fitparams, s_ti, V_PAST, tv, &s_vwork, &st) == 0, "the values build");
    values_print("tank at tech 1", tv, &st);
    CHECK(tv[V_HOMING].q[0] > 0.0f, "the homing unit beats the slow tank: %g", (double)tv[V_HOMING].q[0]);
    /* a gated type past the fight content's types: its runs refused, its values 0 */
    values_content(1, 2, 2, 2);
    memset(&s_ti[V_PAST], 0, sizeof s_ti[V_PAST]);
    s_ti[V_PAST].klass = IMPAI_K_LRAIDER;
    s_ti[V_PAST].flags = IMPAI_TF_MOBILE;
    s_ti[V_PAST].value = 100.0f;
    s_ti[V_PAST].tech = 1;
    CHECK(impai_tinfo_values(&s_fc, &impai_fitparams, s_ti, V_N, tv, &s_vwork, &st) == 0, "the values build");
    values_print("a type past the content", tv, &st);
    CHECK(st.scored == 5 && st.refused == 1 + 4 && tv[V_PAST].q[0] == 0.0f && tv[V_PAST].q[1] == 0.0f,
          "a type past the content: %u refused", st.refused);
    /* forty raiders of tech 1, values 101..140: the field is 32 of them by the value stride, the sorted list's
       floor(k 40 / 32)-th. Each type off the stride carries a weapon of a kind past the content's, so every run
       that names one is refused: the refusals are those types' own 8 x 32 runs and none more exactly when the
       field holds none of them */
    memset(ft, 0, sizeof ft);
    memset(fwp, 0, sizeof fwp);
    memset(ti, 0, sizeof ti);
    fwp[1].range = 200;
    fwp[1].reload = 30;
    fwp[1].burst = 1;
    fwp[1].dmg = 30;
    fwp[2] = fwp[1];
    fwp[2].kind = IMPAI_FKINDS;
    for (t = 1; t <= 40; t++) {
        int stride = 0;
        for (k = 0; k < IMPAI_QFIELD; k++) stride |= t == 1 + k * 40 / IMPAI_QFIELD;
        ft[t].hp = 400;
        ft[t].value = (float)(100 + t);
        ft[t].speed = 160000;
        ft[t].flags = IMPAI_FT_MOBILE;
        ft[t].fx = ft[t].fz = 2;
        ft[t].w[0] = (uint16_t)(stride ? 1 : 2);
        bad += !stride;
        ti[t].klass = IMPAI_K_LRAIDER;
        ti[t].flags = IMPAI_TF_MOBILE;
        ti[t].value = ft[t].value;
        ti[t].tech = 1;
    }
    memset(&c, 0, sizeof c);
    c.t = ft;
    c.nt = 41;
    c.w = fwp;
    c.nw = 3;
    CHECK(impai_tinfo_values(&c, &impai_fitparams, ti, 41, tv, &s_vwork, &st) == 0, "the values build");
    printf("values, forty raiders: types=%u fields=%u,%u,%u,%u runs=%u refused=%u off the stride %d\n", st.scored,
           st.field[0], st.field[1], st.field[2], st.field[3], st.runs, st.refused, bad);
    CHECK(st.field[0] == IMPAI_QFIELD && st.runs == 40 * IMPAI_QFIELD && bad == 8 &&
              st.refused == (uint32_t)bad * IMPAI_QFIELD,
          "forty field types keep 32, the stride's: %u refused", st.refused);
}

/* THE ARMY ORDERS (impai_econ.c) on the governor's harness: the vehicle plant (FACTORY, its raider RAIDER +0.4
   and its tank TANK -0.6), the hover plant (HP, its raider HRAID -0.3 and its artillery HARTY -0.5), the
   advanced lab (ALAB, its CAN +0.5) and a cheap raider RAID2 (+0.1); an empty picture; each decision's queue
   finished at once. */
enum { G_HP = M_N, G_ALAB, G_HRAID, G_HARTY, G_CAN, G_RAID2, G_N };
enum { G_LSCOUT = G_N, G_YARD, G_AIR, G_F0, G_FN = G_F0 + 15 };    /* the bound's, the walk's and the yard's types */
static IMPAI_TVALUE s_gtv[FW_DEFS];
static unsigned s_gnti = G_N;                                     /* the type count gate_run decides over */

static void gate_unit(int t, const char* name, float cost, uint8_t klass, float q)
{
    int k;
    mil_def(t, name, cost, 600, 90000, 300, 40);
    memset(&s_ti[t], 0, sizeof s_ti[t]);
    s_ti[t].cat = IMPAI_C_OTHER;
    s_ti[t].flags = IMPAI_TF_MOBILE | IMPAI_TF_HITDRY;
    s_ti[t].value = cost;
    s_ti[t].klass = klass;
    s_ti[t].tech = 1;
    for (k = 0; k < IMPAI_QTECH; k++) s_gtv[t].q[k] = q;
}

static void gate_plant(int t, const char* name, const uint16_t* menu, int n)
{
    def_set(&s_w, t, name, 6, 6, 0, 0, 1700, 0);
    menu_set(&s_w, t, menu, n);
    memset(&s_ti[t], 0, sizeof s_ti[t]);
    s_ti[t].cat = IMPAI_C_FACTORY;
    s_ti[t].value = 1700;
    s_ti[t].tech = s_ti[t].ctech = 1;
    s_ti[t].fx = s_ti[t].fz = 6;
    s_ti[t].klass = IMPAI_K_NONE;
}

/* the governor's world with the vehicle plant's menu, the hover plant's (none when nhp is 0) and the advanced lab
   when `alab` */
static void gate_world(const uint16_t* vp, int nvp, const uint16_t* hp, int nhp, int alab)
{
    static const uint16_t ALAB[] = { G_CAN };
    int k;
    gov_world(1);
    s_w.ndefs = G_N;
    memset(s_gtv, 0, sizeof s_gtv);
    for (k = 0; k < IMPAI_QTECH; k++) {
        s_gtv[M_RAIDER].q[k] = 0.4f;
        s_gtv[M_TANK].q[k] = -0.6f;
    }
    gate_unit(G_HRAID, "HRAID", 130, IMPAI_K_LRAIDER, -0.3f);
    gate_unit(G_HARTY, "HARTY", 370, IMPAI_K_LARTY, -0.5f);
    gate_unit(G_CAN, "CAN", 480, IMPAI_K_LASSAULT, 0.5f);
    gate_unit(G_RAID2, "RAID2", 60, IMPAI_K_LRAIDER, 0.1f);
    gate_plant(G_HP, "HP", nhp ? hp : ALAB, nhp ? nhp : 1);
    gate_plant(G_ALAB, "ALAB", ALAB, 1);
    menu_set(&s_w, T_FACTORY, vp, nvp);
    if (nhp) unit_add(&s_w, 3, G_HP, 40, 30, 1);
    if (alab) unit_add(&s_w, 5, G_ALAB, 50, 30, 1);
}

/* 20 decisions, each queue finished at once; the units queued at slot f of type t into q[f][t], the first
   constructor's factory into *first_cons; the decisions' poor factories summed */
static unsigned gate_run(int q[6][G_N], int* first_cons)
{
    static const IMPAI_PREDICTOR pred = { rate_zero, impai_expf, NULL };
    IMPAI_BRAIN_IN in;
    IMPAI_BRAIN_OUT bo;
    WORLDQ wq = { &FAKE, &s_w, 0 };
    unsigned poor = 0;
    int i, k, at = 100;
    memset(q, 0, 6 * sizeof q[0]);
    *first_cons = 0;
    for (i = 0; i < 20; i++) {
        const int from = s_w.nlog;
        memset(&in, 0, sizeof in);
        in.q = &wq;
        in.st = &s_st;
        in.pref = s_pref;
        in.pref_stride = 1;
        in.job = s_job; in.njob = FW_SLOTS;
        in.ti = s_ti; in.nti = s_gnti;
        in.spots = s_spots; in.nspots = s_nspots;
        in.scanned = s_scanned;
        in.reach = &s_reach;
        in.plan = &s_glv;
        in.persona = s_persona;
        in.acts = -1;
        in.scratch = s_scratch; in.scratch_bytes = impai_brain_scratch_bytes();
        in.snap = &s_snap;
        in.mil = &s_mil;
        in.pred = &pred;
        in.tv = s_gtv;
        CHECK(decide_traced(&in, &bo) == 0, "the gate's decision %d", i);
        poor += bo.poor;
        for (k = from; k < s_w.nlog; k++) {
            const WQ_CMD* c = &s_w.log[k].c;
            if (c->verb != WQ_CMD_QUEUE || c->unit >= 6 || c->type >= G_N) continue;
            q[c->unit][c->type]++;
            if (c->type == T_CONS && !*first_cons) *first_cons = c->unit;
        }
        at = gov_finish(from, at);
        s_w.gametime += 15;
        if (s_w.nlog >= FW_LOG - 16) s_w.nlog = 0;
    }
    return poor;
}

static void check_plant_gate(void)
{
    static const uint16_t VP[] = { M_RAIDER, M_TANK }, HP[] = { G_HRAID, G_HARTY }, HPRAID[] = { G_HRAID };
    static const uint16_t VPC[] = { T_CONS, M_RAIDER, M_TANK }, HPC[] = { T_CONS, G_HRAID };
    static const uint16_t VPTWO[] = { M_RAIDER, G_RAID2 };
    int q[6][G_N], linked[6][G_N];
    unsigned poor;
    int first_cons, k;
    /* THE CLASSES: the hover plant never a raider (-0.3, below the vehicle plant's +0.4 and below 0), its
       artillery (the only maker); the vehicle plant raiders and tanks (the only assault maker) */
    gate_world(VP, 2, HP, 2, 0);
    poor = gate_run(q, &first_cons);
    printf("gate, the classes: VP raider %d tank %d, HP raider %d artillery %d, poor %u\n", q[2][M_RAIDER],
           q[2][M_TANK], q[3][G_HRAID], q[3][G_HARTY], poor);
    CHECK(!q[3][G_HRAID] && q[3][G_HARTY] > 0 && q[2][M_RAIDER] > 0 && q[2][M_TANK] > 0 && !poor,
          "the hover plant builds artillery only, the vehicle plant raiders and tanks");
    memcpy(linked, q, sizeof q);
    /* THE BEST MAKER: the advanced lab's CAN (+0.5) takes the assault from the vehicle plant's TANK (-0.6) */
    gate_world(VP, 2, HP, 2, 1);
    gate_run(q, &first_cons);
    printf("gate, the best maker: VP raider %d tank %d, ALAB CAN %d\n", q[2][M_RAIDER], q[2][M_TANK], q[5][G_CAN]);
    CHECK(!q[2][M_TANK] && q[2][M_RAIDER] > 0 && q[5][G_CAN] > 0, "the vehicle plant is no longer given assault");
    /* A POOR PLANT: the hover plant with its raider alone is beaten in every class: no army order, counted */
    gate_world(VP, 2, HPRAID, 1, 0);
    poor = gate_run(q, &first_cons);
    printf("gate, a poor plant: HP raider %d, VP raider %d tank %d, poor %u\n", q[3][G_HRAID], q[2][M_RAIDER],
           q[2][M_TANK], poor);
    CHECK(!q[3][G_HRAID] && poor == 20 && q[2][M_RAIDER] > 0, "the poor plant takes no army order and is counted");
    /* the governor allows one constructor (THE OPENING, cons_open 1): it goes to the poor plant, the lower A */
    gate_world(VPC, 3, HPC, 2, 0);
    gate_run(q, &first_cons);
    printf("gate, one constructor: at slot %d, constructors VP %d HP %d, HP raider %d\n", first_cons, q[2][T_CONS],
           q[3][T_CONS], q[3][G_HRAID]);
    CHECK(first_cons == 3 && !q[3][G_HRAID], "the constructor goes to the poor plant");
    /* THE TYPE BEFORE CONTACT: the raider class's best value, RAIDER, not its cheapest, RAID2; with a picture the
       type choice's, here the cheapest (the pair rate 0) */
    for (k = 0; k < 2; k++) {
        gate_world(VPTWO, 2, NULL, 0, 0);
        if (k) {
            s_mil.pic.ne = 1;
            s_mil.pic.e[0].type = M_TANK;
            s_mil.pic.e[0].share = 1.0f;
        }
        gate_run(q, &first_cons);
        printf("gate, the type %s: RAIDER %d RAID2 %d\n", k ? "with a picture" : "before contact", q[2][M_RAIDER],
               q[2][G_RAID2]);
        CHECK(k ? q[2][G_RAID2] > 0 && !q[2][M_RAIDER] : q[2][M_RAIDER] > 0 && !q[2][G_RAID2],
              "the type %s", k ? "with a picture is the type choice's" : "before contact is the best value");
    }
    /* THE GATE'S BOUND: sixteen factory types, the gate's every row -- the vehicle plant's two raiders and fifteen
       plants each of a land scout, class 4, no gated class -- and the reach mask with movement class 23 cleared,
       0xFF7FFFFF, below -2 as a float. A class read past the gate's three rows would write -2 into the memo past
       them -- row 15's class 4 is gate_mask's place -- and the raider class's type before contact would find no
       reaching type and fall to the cheapest, RAID2. Bounded: RAIDER */
    {
        static uint16_t labels[3 * (FW_W / 2) * (FW_H / 2)];
        static const uint16_t SCOUT[] = { G_LSCOUT };
        const unsigned blocks = (FW_W / 2) * (FW_H / 2);
        unsigned b;
        char name[16];
        gate_world(VPTWO, 2, NULL, 0, 0);
        gate_unit(G_LSCOUT, "LSCOUT", 50, IMPAI_K_LSCOUT, 0.0f);
        for (k = 0; k < G_FN - G_F0; k++) {
            snprintf(name, sizeof name, "F%d", k);
            gate_plant(G_F0 + k, name, SCOUT, 1);
            unit_add(&s_w, 10 + k, G_F0 + k, 70 + 8 * (k % 5), 60 + 8 * (k / 5), 1);
        }
        s_w.ndefs = G_FN;
        s_gnti = G_FN;
        for (b = 0; b < blocks; b++) {
            labels[b] = labels[blocks + b] = 1;
            labels[2 * blocks + b] = (uint16_t)(b % (FW_W / 2) >= 32 ? 2 : 1);
        }
        memset(&s_reach, 0, sizeof s_reach);
        s_reach.labels = labels;
        s_reach.at[1] = blocks;
        s_reach.at[23] = 2 * blocks;
        s_reach.present = 3u | 1u << 23;
        s_reach.bw = FW_W / 2;
        s_reach.bh = FW_H / 2;
        s_mil.ebx = 50;
        s_mil.ebz = 10;
        gate_run(q, &first_cons);
        printf("gate, sixteen factory types and a mask below -2: RAIDER %d RAID2 %d\n", q[2][M_RAIDER], q[2][G_RAID2]);
        CHECK(q[2][M_RAIDER] > 0 && !q[2][G_RAID2], "the gate's rows hold every factory type's classes in bound");
        s_gnti = G_N;
    }
    /* THE WALK: a plant that offers no gated class -- a yard of an air scout and the constructor -- whose A is
       below every land plant's: the vehicle plant takes every constructor of the twenty decisions, the yard none,
       even when the vehicle plant takes an army order first */
    {
        static const uint16_t YARD[] = { T_CONS, G_AIR };
        gate_world(VPC, 3, NULL, 0, 0);
        gate_unit(G_AIR, "AIR", 70, IMPAI_K_ASCOUT, 0.0f);
        gate_plant(G_YARD, "YARD", YARD, 2);
        unit_add(&s_w, 4, G_YARD, 50, 50, 1);
        s_w.ndefs = G_YARD + 2;
        s_gnti = G_YARD + 2;
        gate_run(q, &first_cons);
        printf("gate, the walk: the first constructor at slot %d, constructors VP %d yard %d\n", first_cons,
               q[2][T_CONS], q[4][T_CONS]);
        CHECK(first_cons == 2 && q[2][T_CONS] > 0 && !q[4][T_CONS],
              "the land plant takes the constructors, a plant the values do not score none");
        s_gnti = G_N;
    }
    /* NO LAND CLASS REACHES: the estimate's block off the land class's component, every gated type a land one:
       the gate reads the values without the reach (owner's decision), as on the linked map */
    gate_world(VP, 2, HP, 2, 0);
    plant_reach(1);
    s_mil.ebx = s_umil.ebx;
    s_mil.ebz = s_umil.ebz;
    poor = gate_run(q, &first_cons);
    printf("gate, no land class reaches: VP raider %d tank %d, HP raider %d artillery %d, poor %u\n", q[2][M_RAIDER],
           q[2][M_TANK], q[3][G_HRAID], q[3][G_HARTY], poor);
    CHECK(!memcmp(q, linked, sizeof q) && !poor, "no reaching land unit: the linked map's orders");
    /* THE HOVERS REACH, the land types do not: the raiders and the artillery are the hover plant's, the seat's
       only reaching makers; no assault type reaches, so the assault is read without the reach and stays the
       vehicle plant's, by value, for the base's defence */
    gate_world(VP, 2, HP, 2, 0);
    s_ti[G_HRAID].mclass = s_ti[G_HARTY].mclass = 1;
    plant_reach(1);
    s_mil.ebx = s_umil.ebx;
    s_mil.ebz = s_umil.ebz;
    poor = gate_run(q, &first_cons);
    printf("gate, the hovers reach: VP raider %d tank %d, HP raider %d artillery %d, poor %u\n", q[2][M_RAIDER],
           q[2][M_TANK], q[3][G_HRAID], q[3][G_HARTY], poor);
    CHECK(!q[2][M_RAIDER] && q[2][M_TANK] > 0 && !poor && q[3][G_HRAID] > 0 && q[3][G_HARTY] > 0,
          "the hovers take the classes they reach in, the vehicle plant the assault no unit reaches in");
    /* AN AMPHIBIOUS UNIT REACHES (Trout Farm's two vehicle plants beside an advanced plant): the advanced lab's CAN
       reaches, the vehicle plant's raider and tank do not. The assault is CAN's; the raider class, which no unit
       reaches, is read without the reach and is the vehicle plant's: no plant sits poor */
    gate_world(VP, 2, NULL, 0, 1);
    s_ti[G_CAN].mclass = 1;
    plant_reach(1);
    s_mil.ebx = s_umil.ebx;
    s_mil.ebz = s_umil.ebz;
    poor = gate_run(q, &first_cons);
    printf("gate, an amphibious unit reaches: VP raider %d tank %d, ALAB CAN %d, poor %u\n", q[2][M_RAIDER],
           q[2][M_TANK], q[5][G_CAN], poor);
    CHECK(q[2][M_RAIDER] > 0 && !q[2][M_TANK] && q[5][G_CAN] > 0 && !poor,
          "one reaching class leaves the other classes to the land plants");
    /* A POOR PLANT IN SURPLUS: the vehicle plant's raider alone, the hover raider reaching, so the vehicle plant is
       poor; with the store at 0.9 of its maximum it buys its cheapest army type, the store being full */
    for (k = 0; k < 2; k++) {
        static const uint16_t VPRAID[] = { M_RAIDER };
        gate_world(VPRAID, 1, HPRAID, 1, 0);
        s_ti[G_HRAID].mclass = 1;
        plant_reach(1);
        s_mil.ebx = s_umil.ebx;
        s_mil.ebz = s_umil.ebz;
        if (k) s_w.econ[0].metal = 0.9f * s_w.econ[0].metal_max;
        poor = gate_run(q, &first_cons);
        printf("gate, a poor plant, store %.1f: VP raider %d, HP raider %d, poor %u\n", k ? 0.9 : 0.5, q[2][M_RAIDER],
               q[3][G_HRAID], poor);
        CHECK(k ? q[2][M_RAIDER] == 20 && !poor : !q[2][M_RAIDER] && poor == 20,
              "a poor plant %s", k ? "in surplus buys army" : "outside a surplus takes no army order");
    }
    /* THE ISLAND'S LAND BUDGET waits for the economy's reach (impai_econ.c, island_classes): the military judges an
       island, but with no reach table every class reaches, so the vehicle plant's raiders and tanks are no Q3
       fallback and are not capped, though the seat holds no other army */
    gate_world(VP, 2, NULL, 0, 0);
    s_mil.island = 1;
    gate_run(q, &first_cons);
    s_mil.island = 0;
    printf("gate, the island verdict with the land reaching: VP raider %d tank %d\n", q[2][M_RAIDER], q[2][M_TANK]);
    CHECK(q[2][M_RAIDER] + q[2][M_TANK] >= 10, "the island's budget waits for the economy's reach");
    brain_world();
    s_milp = NULL;
}

static void check_sea_plant(void)
{
    const uint32_t fail_t = IMPAI_PLAN_ROW.fail_t;
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
    /* THE LAND PLANT'S METAL: an income of 6, below factory_per_m's 8, the store full and the lab asking 2 of the
       3 unspent -- the surplus pays a further plant: naval, the shipyard; not naval, the land plant */
    for (k = 0; k < 2; k++) {
        coast_world(C_COAST, !k, 6);
        s_w.econ[0].metal_out = 3.0f;
        factory_busy(2, E_CK, 2.0f, 5.0f);
        sea_decide(k ? "COAST, not naval, a surplus" : "COAST, a surplus", &sy, &land);
        CHECK(k ? !sy && land == 1 : sy == 1 && !land, "a surplus that pays a further plant: %s",
              k ? "the land plant" : "the sea plant, not a land one");
    }
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
    /* THE CURSOR: a failure in the shared ring -- a patch's, say -- outlasts three decisions whose search refuses
       site_tests sites each; the cursor passes each decision's refusals, so no site is tested twice, and it starts
       again at the first candidate once fail_t has passed */
    coast_world(C_COAST, 1, 9);
    s_w.refuse_type = E_SY;
    s_st.fail[0].cx = 3;
    s_st.fail[0].cz = 3;
    s_st.fail[0].until = (uint32_t)s_w.gametime + fail_t;
    s_st.nfail = 1;
    s_st.fail_next = 1;
    {
        unsigned prev = 0;
        for (k = 0; k < 3; k++) {
            unsigned at;
            sea_decide("refused, the shared ring's failure", &sy, &land);
            at = (unsigned)s_st.sea_r << 16 | s_st.sea_i;
            CHECK(at > prev, "the cursor moves on: %u after %u", at, prev);
            prev = at;
            s_w.gametime += 15;
        }
    }
    CHECK(s_st.nfail == 1 && s_st.fail[0].cx == 3 && s_st.fail[0].cz == 3 &&
          s_st.fail[0].until > (uint32_t)s_w.gametime, "the earlier failure holds: %d in the ring", s_st.nfail);
    s_w.gametime += (int)fail_t;
    sea_decide("refused, fail_t on", &sy, &land);
    CHECK(s_st.sea_from == (uint32_t)s_w.gametime, "fail_t on, the walk starts again at the first candidate");
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

/* the unit given the last NEW build order of `type` since `from`, -1 none */
static int build_unit(int from, int type)
{
    int k, u = -1;
    for (k = from; k < s_w.nlog; k++)
        if (s_w.log[k].c.verb == WQ_CMD_NEW && s_w.log[k].c.type == type) u = s_w.log[k].c.unit;
    return u;
}

/* THE FINISH AT SEA (impai_brain.c, economy; impai_build.c, THE SEA PLANT) on the FAR coast, its sea past comm_r:
   (a) naval, not dry: no candidate within comm_r, remembered against the radius, and the land factory;
   (b) the same memory, dry, a constructor idle beside the commander: the shipyard past comm_r, the commander's,
   the first builder, its reach widened in the dry hunt (THE COMMANDER'S REACH, build_at);
   (c) dry with factory_max labs and an income of 3, below the second factory's pacing: the shipyard still; with
   IMPAI_IDLE_CAP labs, none -- a factory past it would fall out of the factory lists;
   (d) dry, the commander held by the military's window: the constructor's, on the shore beside the water;
   (e) not naval and not dry: no shipyard; (f) judged not naval, dry: the shipyard -- the dry hunt is the want;
   (g) dry, an enemy PT boat's row in the 256-px cell (7, 4) by the far water: that water is not held, no
   shipyard. THE BUILDER'S REACH (impai_build.c) on the STRIP coast, the shipyard's site (116, 60), centre
   (1920, 1024), its nearest class-0 block of the mainland's label the pier's end (56, 28), 158 px off and four
   blocks on each axis, past CORCK's limit of 112 (no builddistance, 22 and 90 for the footprints), and the
   island's block (57, 31) 82 px off but of another label: (h) the commander held by the window,
   the constructor alone: no shipyard, its refusal counted; (i) the commander's class class 0 too, and a sea
   constructor idle in the deep water at slot 4: the sea constructor's shipyard, the site kept past the two
   refusals before it, one counted; (j) the commander held, the constructor standing in the strip at cell
   (114, 66), 97 px from the site's centre, on a block no class-0 cell labels: the shipyard, built from where it
   stands. THE FRAME RESUMED (impai_build.c) on the FAR coast, an unfinished shipyard of the seat's at (b)'s site,
   past comm_r: (k) dry, the commander idle: it helps the frame; (l) not dry: it does not -- THE ASSIST keeps it
   within comm_r --; (m) dry, the commander held, the land constructor idle on the shore and a sea constructor idle
   in the deep water at cell (118, 64): the sea constructor helps it, and the land one does not -- HelpBuild walks
   to within builddistance and half the frame's side, less than MobileBuild's reach that built from the shore
   (engine/orders.md, *HelpBuild's walk and its abandon*; builder_near) --; (n) dry, the commander already helping
   it: no order; (o) dry, the commander idle, a lab's frame of the seat's at cell (106, 64), past comm_r and nearer
   it than the shipyard's: it helps the shipyard -- the rung is the sea plant's alone. THE BUILDER'S REACH again,
   the commander held: on the SPIT coast the strip's shipyard site and a spit of the mainland's along cells
   z 70..71 to a nub at x 125..126, z 66..69; (p) the nub's block (62, 33), its centre 93 px from the site's
   centre, within CORCK's limit of 112, its square of the centres a CORCK on its entries stands at -- (1992, 1064)
   to (2024, 1096) -- 126 px off at its far corner: no shipyard, the refusal counted; (q) on the SPIT_NEAR coast
   the nub reaching z 64, its block (62, 32) whose square's far corner is 111 px off: the constructor's shipyard;
   (r) (p) with a constructor of footprint 3 x 3, its limit 123: its square on the nub's block, pushed 16 px
   out by its footprint -- (2000, 1072) to (2032, 1104) --, 138 px off at its far corner, where the block's own
   square's far corner is 115 px off: no shipyard, the refusal counted. */
static void far_world_at(int kind, int naval, int dry, int cons, float metal_in)
{
    coast_world(kind, naval, metal_in);
    s_smil.dry = (uint8_t)dry;
    if (cons) unit_add(&s_w, 3, E_CK, 44, 64, 1);
}

static void far_world(int naval, int dry, int cons, float metal_in) { far_world_at(C_FAR, naval, dry, cons, metal_in); }

/* the military's window over the commander, from now */
static void com_window(void) { s_smil.safe_until = (uint32_t)s_w.gametime + 100; }

static int orders_to(int from, int unit);

static void check_sea_finish(void)
{
    int sy, land, k, from, x0 = -1, z0 = -1, unit, fx0, fz0;
    IMPAI_BRAIN_OUT out;
    far_world(1, 0, 1, 9);
    sea_decide("the finish, (a) naval, not dry", &sy, &land);
    CHECK(!sy && land == 1 && s_st.sea_none_type == E_SY && !s_st.sea_none_far,
          "(a) not dry: no shipyard within comm_r, the land factory, the memory at comm_r's radius");
    far_world(1, 1, 1, 9);
    s_st.ax = 40;
    s_st.az = 64;
    s_st.sea_none_type = E_SY;
    s_st.sea_none_ax = 40;
    s_st.sea_none_az = 64;
    s_st.sea_none_ex = 240;
    s_st.sea_none_ez = 32;
    s_st.sea_none_far = 0;
    from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide: the finish (b)");
    sy = builds_of(from, E_SY, &x0, &z0);
    unit = build_unit(from, E_SY);
    printf("sea plant the finish, (b) dry: sy=%d at %d,%d by unit %d, sea_refused=%u\n", sy, x0, z0, unit,
           s_st.sea_refused);
    CHECK(sy == 1 && x0 >= 108 && unit == 1 && !s_st.sea_refused,
          "(b) dry: the shipyard past comm_r, the commander's");
    fx0 = x0;
    fz0 = z0;
    far_world(1, 1, 1, 3);
    for (k = 0; k < IMPAI_PLAN_ROW.factory_max; k++) unit_add(&s_w, 20 + k, E_LAB, 4 + 8 * k, 100, 1);
    sea_decide("the finish, (c) dry, factory_max labs, income 3", &sy, &land);
    CHECK(sy == 1 && !land, "(c) dry: the shipyard past factory_max and the pacing");
    far_world(1, 1, 1, 9);
    s_w.last[0] = 63;
    for (k = 0; k < IMPAI_IDLE_CAP; k++) unit_add(&s_w, 20 + k, E_LAB, 4 + 8 * (k % 4), 60 + 8 * (k / 4), 1);
    sea_decide("the finish, (c) dry, IMPAI_IDLE_CAP labs", &sy, &land);
    CHECK(!sy, "(c) dry: no shipyard past the factory lists' IMPAI_IDLE_CAP");
    far_world(1, 1, 1, 9);
    com_window();
    from = s_w.nlog;
    sea_decide("the finish, (d) dry, the commander held", &sy, &land);
    unit = build_unit(from, E_SY);
    CHECK(sy == 1 && unit == 3 && !s_st.sea_refused, "(d) the commander held: the constructor's, from the shore");
    far_world(0, 0, 1, 9);
    sea_decide("the finish, (e) not naval, not dry", &sy, &land);
    CHECK(!sy, "(e) neither naval nor dry: no shipyard");
    far_world(0, 1, 1, 9);
    sea_decide("the finish, (f) not naval, dry", &sy, &land);
    CHECK(sy == 1, "(f) the dry hunt wants the shipyard on a map judged land");
    far_world(1, 1, 1, 9);
    route_row(IMPAI_LOS, 7 * 8 + 4, 4 * 8 + 4, 100);
    s_rrows[40].seat[0].type = E_PT;
    s_rth[E_PT].hit_ground = 10;
    s_rth[E_PT].hit_range = 100;
    sea_decide("the finish, (g) dry, an enemy boat by the far water", &sy, &land);
    s_viewp = NULL;
    CHECK(!sy, "(g) the far water not held: no shipyard");
    far_world_at(C_STRIP, 1, 1, 1, 9);
    com_window();
    sea_decide("the finish, (h) the strip, the constructor alone", &sy, &land);
    CHECK(!sy && s_st.sea_refused == 1, "(h) the constructor cannot reach the deep water: no shipyard, one refusal");
    far_world_at(C_STRIP, 1, 1, 1, 9);
    s_ti[E_COM].mclass = 0;
    unit_add(&s_w, 4, E_CS, 120, 64, 1);
    from = s_w.nlog;
    sea_decide("the finish, (i) the strip, a sea constructor last", &sy, &land);
    unit = build_unit(from, E_SY);
    CHECK(sy == 1 && unit == 4 && s_st.sea_refused == 1,
          "(i) the site kept past two builders that cannot reach it: the sea constructor's, one refusal");
    far_world_at(C_STRIP, 1, 1, 0, 9);
    com_window();
    unit_add(&s_w, 3, E_CK, 114, 66, 1);
    from = s_w.nlog;
    sea_decide("the finish, (j) the strip, the constructor within its limit", &sy, &land);
    unit = build_unit(from, E_SY);
    CHECK(sy == 1 && unit == 3, "(j) a constructor standing within its limit builds from there, whatever its block");
    for (k = 0; k < 5; k++) {
        static const char* const NAME[5] = { "(k) dry, the commander idle", "(l) not dry", "(m) dry, the commander held",
                                             "(n) dry, the commander helping it", "(o) dry, a lab's frame nearer" };
        far_world(1, k != 1, 1, 9);
        unit_add(&s_w, 5, E_SY, fx0, fz0, 0);
        if (k == 4) unit_add(&s_w, 6, E_LAB, 106, 64, 0);
        if (k == 2) {
            com_window();
            unit_add(&s_w, 4, E_CS, 118, 64, 1);
        }
        if (k == 3) {
            s_job[1].type = E_COM;
            s_job[1].build = (uint16_t)WQ_NONE;
            s_job[1].target = 5;
            s_job[1].kind = IMPAI_JOB_ASSIST;
            s_job[1].since = (uint32_t)s_w.gametime;
            s_w.unit[1].list[0].action = A_HELPBUILD;
            s_w.unit[1].list[0].target = 5;
            s_w.unit[1].n = 1;
        }
        from = s_w.nlog;
        CHECK(decide(&out) == 0, "decide: the frame resumed");
        printf("sea plant the frame resumed, %s: commander job %u on %u, constructor job %u on %u, sea constructor job "
               "%u on %u, orders %d\n", NAME[k], s_job[1].kind, s_job[1].target, s_job[3].kind, s_job[3].target,
               s_job[4].kind, s_job[4].target, s_w.nlog - from);
        if (k == 0 || k == 3 || k == 4)
            CHECK(s_job[1].kind == IMPAI_JOB_ASSIST && s_job[1].target == 5, "%s: the commander helps the frame", NAME[k]);
        if (k == 1)
            CHECK(!(s_job[1].kind == IMPAI_JOB_ASSIST && s_job[1].target == 5), "%s: no help past comm_r", NAME[k]);
        if (k == 2)
            CHECK(s_job[4].kind == IMPAI_JOB_ASSIST && s_job[4].target == 5 &&
                      !(s_job[3].kind == IMPAI_JOB_ASSIST && s_job[3].target == 5),
                  "%s: the sea constructor helps it, the land constructor cannot reach it", NAME[k]);
        if (k == 3) CHECK(!orders_to(from, 1), "%s: no new order to the commander", NAME[k]);
    }
    far_world_at(C_SPIT, 1, 1, 1, 9);
    com_window();
    sea_decide("the finish, (p) the spit's nub, its block's centre within the limit", &sy, &land);
    CHECK(!sy && s_st.sea_refused == 1,
          "(p) a block whose centre is within the limit and its standing centres are not: no shipyard, one refusal");
    far_world_at(C_SPIT_NEAR, 1, 1, 1, 9);
    com_window();
    from = s_w.nlog;
    sea_decide("the finish, (q) the nub's block wholly within the limit", &sy, &land);
    unit = build_unit(from, E_SY);
    CHECK(sy == 1 && unit == 3, "(q) a block whose every standing centre is within the limit: the constructor's");
    {
        const int16_t fx = s_ti[E_CK].fx, fz = s_ti[E_CK].fz;
        far_world_at(C_SPIT, 1, 1, 1, 9);
        com_window();
        s_ti[E_CK].fx = s_ti[E_CK].fz = 3;
        sea_decide("the finish, (r) the spit's nub, a constructor of 3 x 3", &sy, &land);
        s_ti[E_CK].fx = fx;
        s_ti[E_CK].fz = fz;
        CHECK(!sy && s_st.sea_refused == 1, "(r) a 3 x 3 constructor's square, pushed 16 px out of the block, past "
              "the limit: no shipyard, one refusal");
    }
    s_milp = NULL;
}

/* THE FINISH'S YARDS (impai_brain.c, economy) on the FAR coast in the dry hunt, a finished shipyard of the seat's at
   the finish's own site, the store full, an income of 30 a pass, the seat asking 15, an enemy on the water counted
   by the military at this GameTime (IMPAI_MIL.tg_t, THE TARGETS KNOWN):
   (a) the yard at its BuildingBuild asking 5 metal a pass: one more shipyard, at another site;
   (b) the yard idle: none -- the governor has an order for it first;
   (c) the seat asking 28 of the 30: the 2 unasked do not pay the price of 5, none;
   (d) the store at 700 of 1 000, below the surplus: none;
   (e) a second shipyard's frame standing beside the busy yard: none, one at a time;
   (f) the yard waiting at its yard, asking no metal: no price, none;
   (g) naval and not dry on the near coast, where a shipyard has a site within comm_r, the yard and the lab both
       busy, so THE METAL TERM holds: no further shipyard -- outside the dry hunt the overflow's plant is THE
       CHOICE's;
   (h) (a) with only an enemy on dry ground counted: none -- a yard's army hits nothing the seat knows;
   (i) (a) with the enemy on the water counted IMPAI_TG_HOLD + 1 ticks ago: none;
   (j) (a) with an enemy under the water counted instead: one more shipyard. */
static void check_finish_yards(void)
{
    static const char* const NAME[10] = { "(a) the yard busy in a surplus", "(b) the yard idle", "(c) the seat asking 28",
                                          "(d) 700 stored", "(e) a second yard's frame", "(f) the yard asking no metal",
                                          "(g) not dry", "(h) a target on dry ground alone", "(i) the sea target stale",
                                          "(j) a target under the water" };
    IMPAI_BRAIN_OUT out;
    int c, from, sy, land, x0 = -1, z0 = -1, fx0, fz0;
    far_world(1, 1, 1, 9);
    from = s_w.nlog;
    CHECK(decide(&out) == 0, "decide: the finish's first yard");
    CHECK(builds_of(from, E_SY, &x0, &z0) == 1, "the finish's first yard is ordered");
    fx0 = x0;
    fz0 = z0;
    for (c = 0; c < 10; c++) {
        if (c == 6) far_world_at(C_COAST, 1, 0, 1, 30);
        else far_world(1, 1, 1, 30);
        s_w.gametime = 20000;
        s_smil.tg_t[c == 7 ? IMPAI_TG_DRY : c == 9 ? IMPAI_TG_UNDER : IMPAI_TG_FLOAT] =
            (uint32_t)s_w.gametime - (c == 8 ? IMPAI_TG_HOLD + 1u : 0u);
        s_w.econ[0].metal_out = c == 2 ? 28.0f : 15.0f;
        if (c == 3) s_w.econ[0].metal = 700.0f;
        unit_add(&s_w, 5, E_SY, c == 6 ? 64 : fx0, c == 6 ? 60 : fz0, 1);
        if (c != 1) factory_busy(5, E_PT, c == 5 ? 0.0f : 5.0f, 10.0f);
        if (c == 4) unit_add(&s_w, 6, E_SY, fx0, fz0 + 24, 0);
        if (c == 6) factory_busy(2, E_CK, 5.0f, 10.0f);
        from = s_w.nlog;
        sea_decide(NAME[c], &sy, &land);
        x0 = z0 = -1;
        builds_of(from, E_SY, &x0, &z0);
        if (c == 0 || c == 9)
            CHECK(sy == 1 && (x0 != fx0 || z0 != fz0), "%s: one more shipyard, at %d,%d beside %d,%d", NAME[c], x0, z0,
                  fx0, fz0);
        else
            CHECK(!sy, "%s: no further shipyard", NAME[c]);
    }
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

/* ---- THE TASK'S LIFETIME (impai_mil.c) ---- */

/* the travel the military gives squad s's task: 2 x the rally point's distance from the task's point at the
   squad's speed `speed`, held to 16 200 ticks; the rally point is the anchor, these worlds holding no estimate */
static uint32_t lifetime_travel(int s, int32_t speed)
{
    const int64_t dx = (int64_t)(s_mil.sq[s].tx * 32 + 16) - (s_st.ax * 16 + 8);
    const int64_t dz = (int64_t)(s_mil.sq[s].tz * 32 + 16) - (s_st.az * 16 + 8);
    const int64_t t = 2 * (int64_t)impai_isqrt((uint64_t)(dx * dx + dz * dz)) * 65536 / speed;
    return t < 16200 ? (uint32_t)t : 16200u;
}

/* a RETREAT's travel: 2 x the whole way home, from the point squad s's retreat began (its tx, tz) to the rally
   point at `speed`, held to 16 200 ticks */
static uint32_t lifetime_travel_from(int s, int32_t speed)
{
    const int64_t dx = (int64_t)(s_mil.sq[s].tx * 32 + 16) - (s_st.ax * 16 + 8);
    const int64_t dz = (int64_t)(s_mil.sq[s].tz * 32 + 16) - (s_st.az * 16 + 8);
    const int64_t t = 2 * (int64_t)impai_isqrt((uint64_t)(dx * dx + dz * dz)) * 65536 / speed;
    return t < 16200 ? (uint32_t)t : 16200u;
}

/* decisions every look_mil of s_persona from squad s's since until it no longer holds `task`, for at most 40 000 ticks; after
   each, every RAID, RETREAT and SCOUT squad and every DEFEND squad (no case gives one a contact) has held its task
   at most task_timeout + 16 200 ticks. The decision's GameTime that ended it, 0 none. */
static uint32_t lifetime_run(const IMPAI_PLAN* lv, int s, unsigned task)
{
    const uint32_t since = s_mil.sq[s].since;
    IMPAI_MIL_OUT o;
    uint32_t t;
    int k;
    for (t = since + s_persona->look_mil; t <= since + 40000u; t += s_persona->look_mil) {
        s_w.gametime = (int)t;
        CHECK(mil_decide(lv, 1, &o) == 0, "a lifetime decision runs");
        for (k = 0; k < IMPAI_SQUADS; k++) {
            const IMPAI_SQUAD* q = &s_mil.sq[k];
            if (q->task == IMPAI_SQ_RAID || q->task == IMPAI_SQ_RETREAT || q->task == IMPAI_SQ_SCOUT ||
                q->task == IMPAI_SQ_DEFEND)
                CHECK(t - q->since <= (uint32_t)lv->task_timeout + 16200u, "squad %d holds task %u %u ticks", k, q->task,
                      t - q->since);
        }
        if (s_mil.sq[s].task != task) return t;
    }
    return 0;
}

/* one case: squad 0 held `task` until `end`; it must end at the first decision past since + task_timeout + travel */
static void lifetime_check(const char* name, const IMPAI_PLAN* lv, uint32_t since, uint32_t travel, uint32_t end)
{
    const uint32_t due = since + lv->task_timeout + travel;
    printf("lifetime %s, look_mil %u: since %u, travel %u, ended at %u (due after %u)\n", name, s_persona->look_mil,
           since, travel, end, due);
    CHECK(end > due && end <= due + s_persona->look_mil, "%s: ended at %u, due after %u", name, end, due);
}

/* the task of the squad army unit `slot` is in, -1 for none */
static int squad_task_of(int slot) { return s_job[slot].kind == IMPAI_JOB_SQUAD ? s_mil.sq[s_job[slot].squad].task : -1; }

/* THE TASK'S LIFETIME, at each persona's look: a sea RETREAT squad held up the strait 2 900 px from its rally point,
   its travel the whole way from where its retreat began, its centroid's block, a SCOUT
   whose point lies far down its own bank, a DEFEND squad whose threat stays out of its reach, and a RAID of tanks
   (1.07 px a tick) whose point lies 4 000 px off, none of which arrives (the fake's units never move): each goes back to FORM at the first decision past its since +
   task_timeout + its travel, and none holds its task longer than task_timeout + 16 200 ticks meanwhile. */
static void check_mil_lifetimes(void)
{
    unsigned l;
    int k;
    for (l = 0; l < IMPAI_LVS; l++) {
        const IMPAI_PLAN* lv = &IMPAI_PLAN_ROW;
        uint32_t since, travel;
        s_persona = &IMPAI_PERSONAS[l];

        mil_world();
        s_st.ax = 220;
        s_st.az = 40;
        mil_water(W_STRAIT);
        squad_task(0, IMPAI_SQ_RETREAT, TASIM_LAYER_SEA, (253 * 16 + 8) >> 5, (220 * 16 + 8) >> 5);
        member(1, M_DEST, 250, 220, 0);
        member(2, M_DEST, 256, 220, 0);
        since = s_mil.sq[0].since;
        travel = lifetime_travel_from(0, 60000);
        lifetime_check("a sea retreat held up the strait", lv, since, travel, lifetime_run(lv, 0, IMPAI_SQ_RETREAT));
        /* the same squad at the channel's edge nearest the land rally point has arrived where it can stop: the
           substitute move_to gives it (arrived) */
        s_w.unit[1].pos[2] = s_w.unit[2].pos[2] = (40 * 16 + 8) << 16;
        squad_task(0, IMPAI_SQ_RETREAT, TASIM_LAYER_SEA, (220 * 16 + 8) >> 5, (40 * 16 + 8) >> 5);
        s_job[1].squad = s_job[2].squad = 0;
        s_w.gametime += (int)s_persona->look_mil;
        {
            IMPAI_MIL_OUT o;
            CHECK(mil_decide(lv, 1, &o) == 0 && squad_task_of(1) == IMPAI_SQ_FORM && squad_task_of(2) == IMPAI_SQ_FORM,
                  "a sea retreat at its substitute beside the land rally point ends at once");
        }

        mil_world();
        mil_water(W_STRAIT);
        squad_task(0, IMPAI_SQ_SCOUT, TASIM_LAYER_LAND, 110, 120);
        member(1, M_SCOUT, 40, 40, 0);
        since = s_mil.sq[0].since;
        travel = lifetime_travel(0, 200000);
        lifetime_check("a scout far down its own bank", lv, since, travel, lifetime_run(lv, 0, IMPAI_SQ_SCOUT));
        /* a leg across the strait, where travel gives the scout no point and no substitute, is over at once (THE
           ARRIVAL): with fewer than two starts there is no next leg, and the scout is back in FORM */
        mil_world();
        mil_water(W_STRAIT);
        squad_task(0, IMPAI_SQ_SCOUT, TASIM_LAYER_LAND, 200, 50);
        member(1, M_SCOUT, 40, 40, 0);
        s_w.gametime += (int)s_persona->look_mil;
        {
            IMPAI_MIL_OUT o;
            CHECK(mil_decide(lv, 1, &o) == 0 && squad_task_of(1) == IMPAI_SQ_FORM,
                  "a scout whose leg lies across the strait, with no point to travel to, ends it at once");
        }

        mil_world();
        for (k = 0; k < 4; k++) member(1 + k, M_TANK, 20 + 2 * k, 20, 0);
        enemy(1300, M_TANK, 113, 20, IMPAI_LOS, 1);
        squad_task(0, IMPAI_SQ_DEFEND, TASIM_LAYER_LAND, 113 / 2, 20 / 2);
        since = s_mil.sq[0].since;
        travel = lifetime_travel(0, 70000);
        lifetime_check("a defence out of reach", lv, since, travel, lifetime_run(lv, 0, IMPAI_SQ_DEFEND));

        mil_world();
        for (k = 0; k < 4; k++) member(1 + k, M_TANK, 20 + 2 * k, 20, 0);
        squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, (328 + 4000) >> 5, 328 >> 5);
        since = s_mil.sq[0].since;
        travel = lifetime_travel(0, 70000);
        lifetime_check("a raid 4 000 px off", lv, since, travel, lifetime_run(lv, 0, IMPAI_SQ_RAID));
    }
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
}

/* THE WHOLE WAY HOME (impai_mil.c, THE TASK'S LIFETIME) at Hard's look: a land RETREAT of two tanks (1.07 px a tick)
   begun about 6 000 px east of the rally point, past the 2 880 px they walk in task_timeout, its members walked home
   at their speed between decisions. It holds RETREAT at every decision until both stand within RALLY_PX of the
   rally point, and goes back to FORM once they reach their places round it (arrived: travel's spread points),
   within ten looks -- counted from the way still left, it went back to FORM some 600 px out. A wave of two tanks that loses its field fight against six farther out than the
   layer's RETREAT began joins it, moves the squad's point out there and starts its lifetime again; one losing
   nearer home joins it and changes neither. */
static void check_mil_retreat_home(void)
{
    const int32_t v = 70000;
    IMPAI_MIL_OUT o;
    uint32_t t, ended = 0, arrived_at = 0, since;
    int k, x0, x1;
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    mil_world();
    squad_task(0, IMPAI_SQ_RETREAT, TASIM_LAYER_LAND, (396 * 16 + 8) >> 5, (20 * 16 + 8) >> 5);
    member(1, M_TANK, 395, 20, 0);
    member(2, M_TANK, 397, 20, 0);
    for (t = (uint32_t)s_w.gametime + s_persona->look_mil; t < (uint32_t)s_w.gametime + 20000u && !ended;
         t += s_persona->look_mil) {
        const int64_t step = (int64_t)v * s_persona->look_mil;
        for (k = 1; k <= 2; k++) {
            const int64_t x = (int64_t)s_w.unit[k].pos[0] - step, home = (int64_t)(s_st.ax * 16 + 8) << 16;
            s_w.unit[k].pos[0] = (int32_t)(x > home ? x : home);
        }
        x0 = s_w.unit[1].pos[0] >> 16;
        x1 = s_w.unit[2].pos[0] >> 16;
        if (!arrived_at && x0 - (s_st.ax * 16 + 8) <= 512 && x1 - (s_st.ax * 16 + 8) <= 512) arrived_at = t;
        s_w.gametime = (int)t;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the way home's decision runs");
        if (s_mil.sq[0].task != IMPAI_SQ_RETREAT) ended = t;
    }
    printf("the whole way home: arrived at %u, RETREAT ended at %u\n", arrived_at, ended);
    CHECK(arrived_at && ended >= arrived_at && ended <= arrived_at + 10u * s_persona->look_mil,
          "the retreat ends on arriving, not on the way: arrived %u, ended %u", arrived_at, ended);
    /* the join */
    for (k = 0; k < 2; k++) {
        mil_world();
        s_w.gametime = 5000;
        squad_task(0, IMPAI_SQ_RETREAT, TASIM_LAYER_LAND, (200 * 16 + 8) >> 5, (20 * 16 + 8) >> 5);
        s_mil.sq[0].since = 1000;
        member(1, M_TANK, 199, 20, 0);
        member(2, M_TANK, 201, 20, 0);
        recall_squad(1, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, M_TANK, 20, 2, k ? 100 : 400, 60);
        for (t = 0; t < 6; t++)
            enemy(1300 + (int)t, M_TANK, (k ? 104 : 404) + 2 * ((int)t % 3), 70 + 2 * ((int)t / 3), IMPAI_LOS, 1);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the join's decision runs");
        since = s_mil.sq[0].since;
        printf("the whole way home, a wave joining from %s: task %d, point block %d, since %u\n",
               k ? "nearer" : "farther", task_of_slot(20), s_mil.sq[0].tx, since);
        CHECK(task_of_slot(20) == IMPAI_SQ_RETREAT && s_job[20].squad == 0, "the beaten wave joins the RETREAT");
        if (!k)
            CHECK(s_mil.sq[0].tx > ((200 * 16 + 8) >> 5) && since == (uint32_t)s_w.gametime,
                  "joining from farther moves the point and restarts the lifetime");
        else
            CHECK(s_mil.sq[0].tx == ((200 * 16 + 8) >> 5) && since == 1000u, "joining from nearer changes neither");
    }
}

/* KEEPING TO THE LAYER: the military's moves, waves, raids, scouts and predictions, and the explorer */
static void check_layers(void)
{
    IMPAI_MIL_OUT o;
    IMPAI_BRAIN_OUT bo;
    int k, from, n, all, far;
    /* a RAID squad of four land raiders west of the strait, its point east: each moves to the west bank (a RAID
       keeps its point; an ATTACK's is chosen again at each decision) */
    for (far = 0; far <= 1; far++) {
        const int tbx = far ? 159 : 140;
        mil_world();
        mil_water(W_STRAIT);
        squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, tbx, 50);
        for (k = 0; k < 4; k++) member(1 + k, M_RAIDER, 200 + 2 * k, 100, 0);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the layer decision runs");
        n = moves_in(from, 0, 239, tbx, 50, &all);
        printf("layer attack across, point at block %d: moves=%d unreached=%u\n", tbx, n, o.unreached);
        if (far) CHECK(!n && o.unreached == 4, "no west-bank block within SUB_BLOCKS: no move, four unreached");
        else CHECK(n == 4 && all && !o.unreached, "every move on the west bank, near the point");
    }
    /* a land FORM squad worth a wave with the only known structure across: the wave sweeps its own bank, every
       move west of the strait; on the dry map it goes for the structure */
    for (k = 0; k < 2; k++) {
        int i, across = 0;
        mil_world();
        mil_water(k ? W_DRY : W_STRAIT);
        for (i = 0; i < 12; i++) own(1 + i, M_TANK, 24 + 2 * (i % 6), 24 + 2 * (i / 6));
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the wave decision runs");
        for (i = from; i < s_w.nlog; i++) across += s_w.log[i].c.action == A_MOVE && (s_w.log[i].c.pos[0] >> 20) >= 240;
        printf("layer wave, %s: attacks=%u raids=%u moves=%d across=%d point=%d,%d\n", k ? "DRY" : "STRAIT",
               o.attacks, o.raids, count_action(from, A_MOVE, -1), across, s_mil.sq[0].tx, s_mil.sq[0].tz);
        CHECK(o.attacks == 1 && !o.raids && count_action(from, A_MOVE, -1) == 12 && across == 12 * k,
              "a land wave goes only where it reaches: %d of 12 moves across", across);
    }
    /* a sea FORM squad in the channel, its rally point on land: it moves in the channel */
    mil_world();
    s_st.ax = 220;
    s_st.az = 40;
    mil_water(W_STRAIT);
    own(1, M_DEST, 250, 40);
    own(2, M_DEST, 256, 40);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the sea squad's decision runs");
    n = moves_in(from, 240, 271, 110, 20, &all);
    printf("layer sea squad, rally on land: moves=%d in the channel=%d unreached=%u\n", n, all, o.unreached);
    CHECK(n == 2 && all, "the sea squad keeps to the channel");
    /* a land scout on island W, every other start on E: no scout; on the dry map it goes */
    for (k = 0; k < 2; k++) {
        static const WQ_START STARTS[] = { { 1000, 2000, 0, 0 }, { 7000, 2000, 1, 0 }, { 7000, 1500, 2, 0 } };
        mil_world();
        s_st.ax = 62;
        s_st.az = 125;
        memcpy(s_w.starts, STARTS, sizeof STARTS);
        s_w.nstarts = 3;
        mil_water(k ? W_DRY : W_SEA);
        own(7, M_SCOUT, 60, 120);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the scout decision runs");
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
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the raid decision runs");
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
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the cross decision runs");
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
    squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, 130, 34);
    for (k = 0; k < 40; k++) member(1 + k, M_RAIDER, 200 + 4 * (k % 8), 60 + 4 * (k / 8), 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the cache decision runs");
    n = count_action(from, A_MOVE, -1);
    printf("layer substitutes: moves=%d unreached=%u roots=%u\n", n, o.unreached, o.roots);
    CHECK(n == 32 && o.unreached == 8, "SUB_CACHE substitutes, the rest unreached");
    /* a dry map of two land components: five raiders travel to a point 3 cells west of the cliff; the two whose
       spread points fall on the cliff go to the point itself, not to a substitute */
    mil_world();
    mil_water(W_WALL);
    squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, 118, 50);
    for (k = 0; k < 5; k++) member(1 + k, M_RAIDER, 200 + 2 * k, 100, 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the travel decision runs");
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
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the reach fight decision runs");
        printf("layer fight across, %s: raider attacks=%d moves=%d tank attacks=%d predictions=%u\n",
               k ? "DRY" : "WALL", count_action(from, A_ATTACK, 1), count_action(from, A_MOVE, 1),
               count_action(from, A_ATTACK, 2), o.predictions);
        CHECK(count_action(from, A_ATTACK, 2) == 1 && count_action(from, A_ATTACK, 1) == k &&
              !count_action(from, A_MOVE, 1), "the raider attacks across only where it can travel");
    }
    /* the explorer: a sea constructor explores water, a land one land; land over x 0..63, explored to x 47. The scan
       is under way in these two cases, so the opening waits and no plant comes before the explore rung (THE BLIND
       START would ask one of a seat with no factory and none published) */
    for (k = 0; k < 2; k++) {
        int x, z, cx = -1;
        coast_world(C_COAST, 0, 3);
        s_scanned = 0;
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
        s_scanned = 0;
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

/* THE SQUAD TABLE NEVER FILLS (impai_mil.c, become and normalise): sixty decisions, each adding two tanks in the
   field, 1 300 px from the anchor and as far from the rally point, and toggling an armed enemy tank 300 px from
   the anchor; every third decision thirty enemy tanks stand beside the field army, nearer a structure of their own
   side than the anchor -- no base threat --, which every squad there predicts it loses and, its members far from
   where a retreat takes them (THE RETREAT HOME), retreats. After every decision the table holds become's bound and
   no unit is left without a squad. Then a recorded table of three land FORM squads and five ATTACK squads: the
   normalisation leaves one FORM and four ATTACK, every member in a squad. */
static void check_mil_table(void)
{
    IMPAI_MIL_OUT o;
    int i, k, ok = 1, retreats = 0, attacks = 0, defends = 0, used = 0;
    mil_world();
    enemy(1299, T_MEX, 400, 200, IMPAI_GHOST, 1);
    enemy(1298, T_SOLAR, 105, 55, IMPAI_GHOST, 1);
    for (i = 0; i < 60; i++) {
        own(1 + 2 * i, M_TANK, 100 + (i % 10) * 2, 30 + (i / 10) * 2);
        own(2 + 2 * i, M_RAIDER, 101 + (i % 10) * 2, 31 + (i / 10) * 2);
        if (i % 2) enemy(1300, M_TANK, 39, 20, IMPAI_LOS, 1);
        else {
            s_rows[1300].seat[0].how = 0;
            s_w.unit[1300].live = 0;
        }
        for (k = 0; k < 30; k++) {
            if (i % 3 == 2) enemy(1310 + k, M_TANK, 100 + (k % 10) * 2, 44 + (k / 10) * 2, IMPAI_LOS, 1);
            else {
                s_rows[1310 + k].seat[0].how = 0;
                s_w.unit[1310 + k].live = 0;
            }
        }
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "decision %d runs", i);
        for (k = 0, used = 0; k < IMPAI_SQUADS; k++) used += s_mil.sq[k].task != IMPAI_SQ_FREE;
        retreats += o.retreats;
        attacks += o.attacks;
        defends += o.defends;
        if (!table_ok() || o.left_squad || used > IMPAI_SQUADS) ok = 0;
        s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    }
    mil_print("sixty decisions", &o, s_w.nlog);
    printf("mil table: retreats=%d attacks=%d defends=%d squads used at the end=%d\n", retreats, attacks, defends,
           used);
    CHECK(ok && retreats > 0 && defends > 0, "the table within its bounds at every decision, no unit left out");

    mil_world();
    for (k = 0; k < 3; k++) {
        squad_task(k, IMPAI_SQ_FORM, TASIM_LAYER_LAND, 0, 0);
        member(1 + k, M_TANK, 24 + 2 * k, 24, k);
    }
    for (k = 3; k < 8; k++) {
        squad_task(k, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 200, 100);
        member(1 + k, M_TANK, 60 + 2 * k, 60, k);
    }
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the normalisation decision runs");
    mil_print("three FORM and five ATTACK squads", &o, s_w.nlog);
    for (k = 1; k <= 8; k++)
        if (s_job[k].kind != IMPAI_JOB_SQUAD || s_mil.sq[s_job[k].squad].task == IMPAI_SQ_FREE) ok = 0;
    CHECK(ok && o.tasks[IMPAI_SQ_FORM] == 1 && o.tasks[IMPAI_SQ_ATTACK] == IMPAI_ATTACKS_MAX && o.left_squad == 0,
          "one FORM (%u) and four ATTACK (%u), every member in a squad", o.tasks[IMPAI_SQ_FORM],
          o.tasks[IMPAI_SQ_ATTACK]);
}

/* the attack squad's index, -1 for none */
static int attack_squad(void)
{
    int k;
    for (k = 0; k < IMPAI_SQUADS; k++) if (s_mil.sq[k].task == IMPAI_SQ_ATTACK) return k;
    return -1;
}

/* the moves since `from` whose point lies within `r` px of (x, z), and their count in all */
static int moves_near(int from, int32_t x, int32_t z, uint32_t r, int* all)
{
    int k, n = 0;
    *all = 0;
    for (k = from; k < s_w.nlog; k++) {
        const WQ_CMD* c = &s_w.log[k].c;
        int64_t dx, dz;
        if (c->action != A_MOVE) continue;
        (*all)++;
        dx = (c->pos[0] >> 16) - x;
        dz = (c->pos[2] >> 16) - z;
        n += impai_isqrt((uint64_t)(dx * dx + dz * dz)) <= r;
    }
    return n;
}

/* A WAVE DOES NOT STAND (impai_mil.c, orders and tasks): an ATTACK squad of eight tanks far from the base, its goal
   the one known enemy structure 2 000 px east, meets a contact of one submarine under the sea 300 px off, which no
   tank can attack (no waterweapon): no attack order, and every tank moves on toward the structure, none to the
   centroid. The retreat margin is put out of play (-2), so what is tested is the order and not the prediction.
   Then the structure dies: the next decision the squad is still ATTACK with a new goal, and no tank is sent to
   the rally point. */
static void check_mil_wave_moves_on(void)
{
    IMPAI_PLAN lv = IMPAI_PLAN_ROW;
    IMPAI_MIL_OUT o;
    int k, from, near, all, s;
    lv.retreat_margin = -2.0f;
    mil_world();
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 166, 50);
    for (k = 0; k < 8; k++) member(1 + k, M_TANK, 200 + 2 * k, 100, 0);
    enemy(1300, T_MEX, 332, 100, IMPAI_GHOST, 1);
    enemy(1301, M_SUB, 226, 100, IMPAI_LOS, 1);
    s_w.unit[1301].aim = WQ_AIM_UNDER;
    from = s_w.nlog;
    CHECK(mil_decide(&lv, 1, &o) == 0, "the decision runs");
    mil_print("a wave meets a submarine", &o, from);
    near = moves_near(from, s_w.unit[1300].pos[0] >> 16, s_w.unit[1300].pos[2] >> 16, 300, &all);
    CHECK(o.contacts == 1 && !count_action(from, A_ATTACK, -1) && all == 8 && near == 8,
          "no attack, the eight move on to the structure: %d of %d moves there", near, all);
    s_rows[1300].seat[0].how = 0;
    s_w.unit[1300].live = 0;
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    from = s_w.nlog;
    CHECK(mil_decide(&lv, 1, &o) == 0, "the decision after the structure died runs");
    s = attack_squad();
    mil_print("the wave's structure died", &o, from);
    near = moves_near(from, (s_st.ax * 16 + 8) + 384, s_st.az * 16 + 8, 600, &all);
    CHECK(s >= 0 && !(s_mil.sq[s].tx == 166 && s_mil.sq[s].tz == 50) && s_mil.sq[s].target == WQ_NONE && !near,
          "still ATTACK (%d) with a new goal (%d, %d), %d moves near the rally point", s, s >= 0 ? s_mil.sq[s].tx : -1,
          s >= 0 ? s_mil.sq[s].tz : -1, near);
}

/* THE WAVES HUNT (impai_mil.c, wave_goal, the commander's memory and the sweep): (a) twelve tanks, one enemy
   extractor remembered far off; a wave goes for it, and when the enemy commander is seen 800 px from it the
   wave's goal is the commander's block and slot. (b) The commander leaves sight: the goal stays its last block,
   no slot, until a tank stands in that sweep cell; then the extractor. (c) The extractor is gone and the
   commander unknown: the goal is the sweep's, the cell nearest the estimate an army unit never stood in, and
   with the squad arriving at each, three decisions visit three distinct cells. (d) A contact of a factory and the
   commander -- a factory, which no raid goes for, so no raid takes tanks out of the fighting squad: every tank
   attacks the commander. (e) All 256 cells swept at one time and no estimate: the goal is the lowest index of the
   four cells nearest the squad. */
static void check_mil_hunt(void)
{
    const IMPAI_PLAN* lv = &IMPAI_PLAN_ROW;
    IMPAI_MIL_OUT o;
    int k, i, s, from, cells[3];
    mil_world();
    for (k = 0; k < 12; k++) own(1 + k, M_TANK, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
    CHECK(mil_decide(lv, 1, &o) == 0 && o.attacks == 1, "(a) the wave goes");
    enemy(1310, T_COM, 350, 200, IMPAI_LOS, 1);
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(lv, 1, &o) == 0, "(a) the decision with the commander seen runs");
    s = attack_squad();
    printf("hunt (a): goal %d,%d target %d, foe[1] slot %u at %d,%d t=%u\n", s >= 0 ? s_mil.sq[s].tx : -1,
           s >= 0 ? s_mil.sq[s].tz : -1, s >= 0 ? s_mil.sq[s].target : -1, s_mil.foe[1].slot, s_mil.foe[1].bx,
           s_mil.foe[1].bz, s_mil.foe[1].t);
    CHECK(s >= 0 && s_mil.sq[s].tx == 175 && s_mil.sq[s].tz == 100 && s_mil.sq[s].target == 1310 &&
              s_mil.foe[1].slot == 1310,
          "(a) the commander is the goal");
    if (s < 0) return;
    s_rows[1310].seat[0].how = IMPAI_GHOST;
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(lv, 1, &o) == 0, "(b) the decision with the commander out of sight runs");
    printf("hunt (b): goal %d,%d target %d\n", s_mil.sq[s].tx, s_mil.sq[s].tz, s_mil.sq[s].target);
    CHECK(s_mil.sq[s].task == IMPAI_SQ_ATTACK && s_mil.sq[s].tx == 175 && s_mil.sq[s].tz == 100 &&
              s_mil.sq[s].target == WQ_NONE,
          "(b) the commander's last block, no slot");
    s_w.unit[1].pos[0] = (350 * 16 + 8) << 16;     /* a tank stands in the commander's sweep cell */
    s_w.unit[1].pos[2] = (200 * 16 + 8) << 16;
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(lv, 1, &o) == 0, "(b) the decision with the cell swept runs");
    printf("hunt (b) swept: goal %d,%d target %d\n", s_mil.sq[s].tx, s_mil.sq[s].tz, s_mil.sq[s].target);
    CHECK(s_mil.sq[s].tx == 200 && s_mil.sq[s].tz == 100 && s_mil.sq[s].target == 1300, "(b) then the extractor");
    s_rows[1300].seat[0].how = 0;
    s_w.unit[1300].live = 0;
    for (i = 0; i < 3; i++) {
        int32_t x, z;
        int c;
        s_w.gametime += 64;
        CHECK(mil_decide(lv, 1, &o) == 0, "(c) a sweep decision runs");
        x = s_mil.sq[s].tx * 32 + 16;
        z = s_mil.sq[s].tz * 32 + 16;
        c = (z * 16 / (s_w.mh * 16)) * 16 + x * 16 / (s_w.mw * 16);
        cells[i] = c;
        printf("hunt (c) %d: goal %d,%d cell %d swept %u target %d\n", i, s_mil.sq[s].tx, s_mil.sq[s].tz, c,
               s_mil.swept[c], s_mil.sq[s].target);
        CHECK(s_mil.sq[s].task == IMPAI_SQ_ATTACK && s_mil.swept[c] == 0 && s_mil.sq[s].target == WQ_NONE,
              "(c) a cell no army unit stood in");
        for (k = 1; k <= 12; k++) {
            s_w.unit[k].pos[0] = x << 16;
            s_w.unit[k].pos[2] = z << 16;
        }
    }
    CHECK(cells[0] == 12 * 16 + 12 && cells[0] != cells[1] && cells[1] != cells[2] && cells[0] != cells[2],
          "(c) the first cell is the estimate's, then three distinct cells: %d %d %d", cells[0], cells[1], cells[2]);

    mil_world();
    for (k = 0; k < 8; k++) own(1 + k, M_TANK, 200 + 2 * k, 100);
    enemy(1300, T_FACTORY, 212, 112, IMPAI_LOS, 1);
    enemy(1310, T_COM, 206, 114, IMPAI_LOS, 1);
    from = s_w.nlog;
    CHECK(mil_decide(lv, 1, &o) == 0, "(d) the focus decision runs");
    mil_print("hunt (d): a factory and the commander", &o, from);
    for (k = from, i = 0; k < s_w.nlog; k++)
        if (s_w.log[k].c.action == A_ATTACK) i += s_w.log[k].c.target == 1310 ? 1 : 100;
    CHECK(i == 8, "(d) the eight tanks attack the commander");

    mil_world();
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 0, 0);
    member(1, M_TANK, 0, 0, 0);
    s_w.unit[1].pos[0] = 1024 << 16;
    s_w.unit[1].pos[2] = 512 << 16;
    for (k = 0; k < IMPAI_SWEEP; k++) s_mil.swept[k] = 1;
    CHECK(mil_decide(lv, 1, &o) == 0, "(e) the tie decision runs");
    printf("hunt (e): goal %d,%d\n", s_mil.sq[0].tx, s_mil.sq[0].tz);
    CHECK(s_mil.sq[0].task == IMPAI_SQ_ATTACK && s_mil.sq[0].tx == 24 && s_mil.sq[0].tz == 12,
          "(e) the lowest index of the four nearest, cell 17 at (768, 384)");
}

/* the squads of `task` on `layer`, -1 for any layer */
static int squads_of(unsigned task_, int layer)
{
    int k, n = 0;
    for (k = 0; k < IMPAI_SQUADS; k++)
        n += s_mil.sq[k].task == task_ && (layer < 0 || s_mil.sq[k].layer == (unsigned)layer);
    return n;
}

/* A MIXED SQUAD SWEEPS WHERE ITS ARMY GOES (impai_mil.c, sweep_goal): on the SEA map, an ATTACK squad on island W
   of a hovercraft, listed first, and eight tanks, nothing of the enemy known but its start on island E. The
   hovercraft alone reaches every cell, the tanks only island W's: the goal is the cell nearest the estimate whose
   centre the tanks, most of the squad's value, reach -- on island W -- and the squad leaves the point it stood at. */
static void check_mil_mixed_sweep(void)
{
    static const WQ_START STARTS[] = { { 1000, 2000, 0, 0 }, { 7000, 2000, 1, 0 } };
    IMPAI_MIL_OUT o;
    int k, from, n, all;
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 2;
    s_ti[M_RAIDER].mclass = 2;                          /* a hovercraft: class 2 goes everywhere */
    s_w.defs[M_RAIDER].d.mclass = 2;
    mil_water(W_SEA);
    s_st.ax = 62;
    s_st.az = 125;
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 31, 62);
    member(1, M_RAIDER, 60, 120, 0);
    for (k = 0; k < 8; k++) member(2 + k, M_TANK, 60 + 2 * (k % 4), 124 + 2 * (k / 4), 0);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the mixed sweep decision runs");
    n = moves_in(from, 16, 111, 40, 68, &all);
    printf("mixed sweep: task %c goal %d,%d moves=%d on island W=%d unreached=%u\n",
           impai_mil_letter(s_mil.sq[0].task), s_mil.sq[0].tx, s_mil.sq[0].tz, n, all, o.unreached);
    CHECK(s_mil.sq[0].task == IMPAI_SQ_ATTACK && s_mil.sq[0].tx == 40 && s_mil.sq[0].tz == 68,
          "the goal is island W's cell nearest the estimate, (1280, 2176)");
    CHECK(n == 9 && all && !o.unreached, "the nine move on island W");
}

/* THE WAVE PER LAYER (impai_mil.c, form_attack and tasks' RESET), on check_mil_wave_plant's world: sixteen tanks
   at the base, the enemy's factory a ghost at (100, 200).
   (a) The sea's count at 40, the land's at 2: the land wave forms ((8 + 2) tanks' worth), and only the land's count
       rises; with the land's count at 40 it does not.
   (b) Twenty submarines worth ten tanks each beside them, a sea FORM squad: the land threshold reads the land
       median, and the land wave still forms.
   (c) The submarines alone, the sea's count 0 and the land's 5: a sea wave, which raises only the sea's count.
   (d) Eight tanks of an ATTACK squad at their structure target's point, the target gone from the seat's rows:
       the land's count is reset, once, and the sea's kept; the target still held, the squad 2 000 px off it, or a
       mobile target (a commander out of sight) gone: no reset.
   The seat's twenty submarines put it far ahead of a factory alone, so an enemy army it lost sight of holds it
   out of THE LEAD, whose waves count nothing (enemy_away). */
static void wave_world(int tanks, int subs)
{
    int k;
    mil_world();
    s_w.gametime = 38 + 15 * 400;
    for (k = 0; k < tanks; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
    s_ti[M_SUB].klass = IMPAI_K_SRAIDER;
    s_ti[M_SUB].value = 10.0f * s_ti[M_TANK].value;
    for (k = 0; k < subs; k++) own(40 + k, M_SUB, 60 + 2 * (k % 10), 24 + 2 * (k / 10));
    enemy(1301, T_FACTORY, 100, 200, IMPAI_GHOST, 1);
    enemy_away(150);
}

static void check_mil_wave_layers(void)
{
    IMPAI_MIL_OUT o;
    int c;
    for (c = 0; c < 2; c++) {
        wave_world(16, 0);
        s_mil.waves[TASIM_LAYER_SEA] = 40;
        s_mil.waves[TASIM_LAYER_LAND] = (uint16_t)(c ? 40 : 2);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the layers' wave decision runs");
        printf("waves by layer (a), the land's count %d: land waves %u, counts land %u sea %u\n", c ? 40 : 2,
               o.waves[TASIM_LAYER_LAND], s_mil.waves[TASIM_LAYER_LAND], s_mil.waves[TASIM_LAYER_SEA]);
        if (!c)
            CHECK(o.waves[TASIM_LAYER_LAND] == 1 && s_mil.waves[TASIM_LAYER_LAND] == 3 &&
                      s_mil.waves[TASIM_LAYER_SEA] == 40 && squads_of(IMPAI_SQ_ATTACK, TASIM_LAYER_LAND) == 1,
                  "(a) the land wave forms beside the sea's 40 waves, and only the land's count rises");
        else
            CHECK(!o.attacks && s_mil.waves[TASIM_LAYER_LAND] == 40, "(a) at the land's own 40 no land wave");
    }
    wave_world(16, 20);
    s_mil.waves[TASIM_LAYER_SEA] = 40;
    s_mil.waves[TASIM_LAYER_LAND] = 2;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the layers' wave decision runs");
    printf("waves by layer (b), twenty submarines worth ten tanks each: land waves %u sea waves %u\n",
           o.waves[TASIM_LAYER_LAND], o.waves[TASIM_LAYER_SEA]);
    CHECK(o.waves[TASIM_LAYER_LAND] == 1 && !o.waves[TASIM_LAYER_SEA],
          "(b) the land threshold reads the land median, not the submarines'");
    wave_world(0, 20);
    s_mil.waves[TASIM_LAYER_LAND] = 5;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the layers' wave decision runs");
    printf("waves by layer (c), the submarines alone: sea waves %u, counts land %u sea %u\n", o.waves[TASIM_LAYER_SEA],
           s_mil.waves[TASIM_LAYER_LAND], s_mil.waves[TASIM_LAYER_SEA]);
    CHECK(o.waves[TASIM_LAYER_SEA] == 1 && s_mil.waves[TASIM_LAYER_SEA] == 1 && s_mil.waves[TASIM_LAYER_LAND] == 5,
          "(c) a sea wave raises the sea's count alone");
    for (c = 0; c < 4; c++) {
        int k;
        static const char* const NAME[4] = { "the target gone", "the target held", "the squad 2 000 px off",
                                             "a commander target gone" };
        mil_world();
        s_w.gametime = 38 + 15 * 400;
        squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 50, 100);
        s_mil.sq[0].target = 1300;
        s_mil.sq[0].flags = (uint8_t)(c != 3);
        for (k = 0; k < 8; k++) member(1 + k, M_TANK, (c == 2 ? 230 : 98) + 2 * (k % 4), 198 + 2 * (k / 4), 0);
        if (c == 1) enemy(1300, T_FACTORY, 100, 200, IMPAI_LOS, 1);
        enemy(1301, T_FACTORY, 400, 40, IMPAI_GHOST, 1);
        s_mil.waves[TASIM_LAYER_LAND] = 7;
        s_mil.waves[TASIM_LAYER_SEA] = 4;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the reset decision runs");
        printf("waves by layer (d), %s: resets %u, counts land %u sea %u, target %d structure %u\n", NAME[c],
               o.wave_resets, s_mil.waves[TASIM_LAYER_LAND], s_mil.waves[TASIM_LAYER_SEA], s_mil.sq[0].target,
               s_mil.sq[0].flags);
        if (!c)
            CHECK(o.wave_resets == 1 && !s_mil.waves[TASIM_LAYER_LAND] && s_mil.waves[TASIM_LAYER_SEA] == 4,
                  "(d) a wave that destroyed its target resets its layer's count");
        else
            CHECK(!o.wave_resets && s_mil.waves[TASIM_LAYER_LAND] == 7, "(d) %s: no reset", NAME[c]);
    }
}

/* THE ARRIVAL A FULL CACHE CANNOT DECIDE (impai_mil.c, point_for and arrived), on the SEA map: squad 0, served first,
   an ATTACK of 40 submarines each on a one-block lake of its own label east of island W, sent to a point on the
   island, which fills the 32 substitutes of the decision; squad 1, four submarines on the open sea, an ATTACK on the
   enemy's factory at island W's east coast, whose point needs a substitute the full cache cannot give. Squad 1
   stays ATTACK: a full cache neither arrives it nor sends it home. */
static void check_mil_arrival_cache(void)
{
    const size_t blocks = (size_t)(512 / 2) * (256 / 2);
    IMPAI_MIL_OUT o;
    int k;
    mil_world();
    mil_water(W_SEA);
    s_w.gametime = 38 + 15 * 400;
    enemy(1301, T_FACTORY, 106, 120, IMPAI_GHOST, 1);
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_SEA, 40, 60);
    squad_task(1, IMPAI_SQ_ATTACK, TASIM_LAYER_SEA, 53, 60);
    s_mil.next_squad = 0;
    for (k = 0; k < 40; k++) {
        const int bx = 60 + 2 * (k % 10), bz = 50 + 2 * (k / 10);
        s_flabels[blocks + (size_t)bz * 256 + (size_t)bx] = (uint16_t)(1000 + k);
        member(100 + k, M_SUB, 2 * bx, 2 * bz, 0);
    }
    for (k = 0; k < 4; k++) member(200 + k, M_SUB, 180 + 2 * k, 120, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the full cache's decision runs");
    printf("the arrival with the cache full: squad 1 task %c goal %d,%d, unreached %u\n",
           impai_mil_letter(s_mil.sq[1].task), s_mil.sq[1].tx, s_mil.sq[1].tz, o.unreached);
    CHECK(s_mil.sq[1].task == IMPAI_SQ_ATTACK, "a full cache sends the ATTACK home: task %c",
          impai_mil_letter(s_mil.sq[1].task));
}

/* AN ATTACK THAT HAS ARRIVED MOVES ON (impai_mil.c, move_on): (a) eight tanks stand at their point, a remembered
   enemy commander's block just past a sweep cell's edge from them, nothing seen: the cell is stamped, the memory
   is spent, and the goal is the remembered extractor far off. (b) Eight tanks stand at an extractor they see and
   cannot attack (no canattack): with no other goal the squad goes back to FORM, and stays FORM at the next
   decision while the extractor is still in reach. */
static void check_mil_arrived(void)
{
    IMPAI_MIL_OUT o;
    int k, from, cell;
    mil_world();
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 32, 60);
    for (k = 0; k < 8; k++) member(1 + k, M_TANK, 62, 121, 0);   /* (1000, 1944): sweep cell 113, the point 114 */
    s_mil.foe[1].slot = 1310;
    s_mil.foe[1].bx = 32;
    s_mil.foe[1].bz = 60;
    s_mil.foe[1].t = 10;
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
    cell = 7 * IMPAI_SWEEP_SIDE + 2;
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(a) the arrival decision runs");
    printf("arrived (a): task %c goal %d,%d target %d swept[%d]=%u moves=%d\n", impai_mil_letter(s_mil.sq[0].task),
           s_mil.sq[0].tx, s_mil.sq[0].tz, s_mil.sq[0].target, cell, s_mil.swept[cell],
           count_action(from, A_MOVE, -1));
    CHECK(s_mil.sq[0].task == IMPAI_SQ_ATTACK && s_mil.sq[0].tx == 200 && s_mil.sq[0].tz == 100 &&
              s_mil.sq[0].target == 1300 && s_mil.swept[cell] != 0 && count_action(from, A_MOVE, -1) == 8,
          "(a) the commander's cell stamped, the wave moves on to the extractor");

    mil_world();
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 100, 50);
    for (k = 0; k < 8; k++) member(1 + k, M_TANK, 196 + 2 * (k % 4), 98 + 2 * (k / 4), 0);
    s_ti[M_TANK].attack = 0;
    enemy(1300, T_MEX, 200, 100, IMPAI_LOS, 1);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(b) the standing decision runs");
    mil_print("arrived (b): an extractor the tanks cannot attack", &o, from);
    CHECK(squads_of(IMPAI_SQ_ATTACK, -1) == 0 && s_job[1].kind == IMPAI_JOB_SQUAD &&
              s_mil.sq[s_job[1].squad].task == IMPAI_SQ_FORM && !count_action(from, A_ATTACK, -1),
          "(b) no ATTACK left, the tanks in FORM");
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(b) the next decision runs");
    CHECK(squads_of(IMPAI_SQ_ATTACK, -1) == 0 && !o.attacks, "(b) no wave forms at the extractor again");
}

/* ROOM FOR A LAYER'S FIRST WAVE (impai_mil.c, attack_room): four land ATTACK squads out, two of them with points
   64 px apart, and a sea FORM squad in the strait worth a wave: the sea wave goes, the two land squads whose points
   are nearest merge, and four ATTACK squads stay out; the higher's wave counted, so the merged one's has
   (THE WAVE'S COUNT). */
static void check_mil_attack_room(void)
{
    static const WQ_START STARTS[] = { { 1000, 2000, 0, 0 }, { 7000, 2000, 1, 0 } };
    static const int16_t PT[4][2] = { { 100, 50 }, { 102, 50 }, { 60, 100 }, { 20, 120 } };
    IMPAI_MIL_OUT o;
    int k, sea = -1;
    mil_world();
    memcpy(s_w.starts, STARTS, sizeof STARTS);
    s_w.nstarts = 2;
    mil_water(W_STRAIT);
    s_st.ax = 62;
    s_st.az = 125;
    squad_task(0, IMPAI_SQ_FORM, TASIM_LAYER_SEA, 0, 0);
    for (k = 0; k < 10; k++) member(1 + k, M_DEST, 244 + 2 * (k % 5), 40 + 2 * (k / 5), 0);
    for (k = 0; k < 4; k++) {
        squad_task(1 + k, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, PT[k][0], PT[k][1]);
        member(11 + 2 * k, M_TANK, 2 * PT[k][0], 2 * PT[k][1], 1 + k);
        member(12 + 2 * k, M_TANK, 2 * PT[k][0] + 2, 2 * PT[k][1], 1 + k);
    }
    s_mil.sq[2].counted = 1;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the attack room decision runs");
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (s_mil.sq[k].task == IMPAI_SQ_ATTACK && s_mil.sq[k].layer == TASIM_LAYER_SEA) sea = k;
    mil_print("a sea wave beside four land waves", &o, s_w.nlog);
    CHECK(o.attacks == 1 && sea >= 0 && o.tasks[IMPAI_SQ_ATTACK] == IMPAI_ATTACKS_MAX &&
              squads_of(IMPAI_SQ_ATTACK, TASIM_LAYER_LAND) == 3,
          "the sea wave goes (squad %d), three land ATTACK squads left", sea);
    CHECK(s_mil.sq[2].task == IMPAI_SQ_FREE && s_job[13].squad == 1 && s_job[14].squad == 1 && s_job[11].squad == 1 &&
              s_mil.sq[1].counted == 1,
          "the nearer pair merged, the higher index into the lower, its counted wave carried");
}

/* A THREAT AGAINST ITS OWN SIDE'S BASE (impai_mil.c, base_threats): two enemies, owner 1's extractors far west of
   the anchor, owner 2's about as far east, so the value-weighted centre of every enemy structure falls 96 px west
   of the anchor, nearer owner 1's tank 400 px west of it than the anchor is. The tank stands nearer the anchor
   than any structure of its own side: a threat, and the FORM squad defends. With no structure known the start
   positions stand in: the tank 400 px west is a threat, and one 1 376 px west, 128 px from the enemy start and
   inside the anchor box, is none -- and a threat when the level seated an ally at that start (THE FOE'S STARTS):
   no enemy's base stands there. */
static void check_mil_two_sides(void)
{
    static const WQ_START STARTS[] = { { 4104, 2056, 0, 0 }, { 2600, 2056, 1, 0 } };
    IMPAI_MIL_OUT o;
    int k, round;
    for (round = 0; round < 4; round++) {
        mil_world();
        s_st.ax = 256;
        s_st.az = 128;
        memcpy(s_w.starts, STARTS, sizeof STARTS);
        s_w.nstarts = 2;
        if (round == 3) {
            s_w.starts[0].seated = 1;
            s_w.starts[1].seated = 3;
            s_allies = 1u << 2;
        }
        for (k = 0; k < 12; k++) own(1 + k, M_TANK, 250 + 2 * (k % 6), 140 + 2 * (k / 6));
        if (round == 0) {
            static const int X[4] = { 40, 60, 440, 460 };
            for (k = 0; k < 4; k++) {
                enemy(1300 + k, T_MEX, X[k], 128, IMPAI_GHOST, 1);
                if (k >= 2) {
                    s_rows[1300 + k].seat[0].owner = 2;
                    s_w.unit[1300 + k].owner = 2;
                }
            }
        }
        enemy(1310, M_TANK, round >= 2 ? 170 : 231, 128, IMPAI_LOS, 1);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the two sides decision runs");
        printf("two sides, round %d: ebx %d threats=%u defends=%u\n", round, s_mil.ebx, o.threats, o.defends);
        if (round == 0)
            CHECK(s_mil.ebx == 125 && o.threats == 1 && o.defends == 1,
                  "the centre of both sides' structures 96 px west of the anchor, the west tank a threat");
        if (round == 1) CHECK(o.threats == 1 && o.defends == 1, "no structure known: the starts stand in");
        if (round == 2) CHECK(o.threats == 0 && !o.defends, "a tank beside its own start: no threat");
        if (round == 3) CHECK(o.threats == 1, "a tank beside an ally's start: a threat");
    }
}

/* THE SIDE THAT FIGHTS (impai_mil.c, side_engaged): check_mil_retreat's two raiders against ten tanks, now an
   ATTACK squad, (a) alone: they fall back; (b) beside another ATTACK squad of thirty tanks standing in reach of the
   same ten: neither falls back; (c) the thirty in RETREAT instead: a squad leaving counts nothing, and the raiders
   fall back (into the layer's RETREAT); (d) the thirty attacking 1 600 px off, out of their reach of the ten: the
   raiders fall back. */
static void check_mil_side_engaged(void)
{
    static const char* const NAME[4] = { "(a) alone", "(b) beside thirty tanks attacking", "(c) beside thirty in RETREAT",
                                         "(d) thirty attacking out of reach" };
    IMPAI_MIL_OUT o;
    int k, c;
    for (c = 0; c < 4; c++) {
        mil_world();
        squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 100, 58);
        member(1, M_RAIDER, 200, 100, 0);
        member(2, M_RAIDER, 202, 100, 0);
        if (c) {
            squad_task(1, c == 2 ? IMPAI_SQ_RETREAT : IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 100, 58);
            for (k = 0; k < 30; k++) member(10 + k, M_TANK, 196 + 2 * (k % 10), (c == 3 ? 8 : 108) + 2 * (k / 10), 1);
        }
        for (k = 0; k < 10; k++) enemy(1300 + k, M_TANK, 196 + 2 * k, 116, IMPAI_LOS, 1);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the side's decision runs: %s", NAME[c]);
        printf("the side that fights, %s: %u retreats, the raiders' squad %c\n", NAME[c], o.retreats,
               impai_mil_letter((unsigned)squad_task_of(1)));
        if (c == 1)
            CHECK(!o.retreats && squad_task_of(1) == IMPAI_SQ_ATTACK && squad_task_of(10) == IMPAI_SQ_ATTACK,
                  "%s: no retreat", NAME[c]);
        else
            CHECK(o.retreats == 1 && squad_task_of(1) == IMPAI_SQ_RETREAT, "%s: the raiders fall back", NAME[c]);
    }
}

/* THE RETREAT RELEASES ON ARRIVAL (impai_mil.c, release_arrived): six tanks at the rally point and two 2 000 px off
   it: the six join the layer's FORM squad and the two stay in RETREAT; the two arrive, they join it too and the
   RETREAT squad is freed. */
static void check_mil_retreat_members(void)
{
    IMPAI_MIL_OUT o;
    int k, home = 1;
    mil_world();
    squad_task(0, IMPAI_SQ_RETREAT, TASIM_LAYER_LAND, 10, 10);
    for (k = 0; k < 6; k++) member(1 + k, M_TANK, 18 + 2 * (k % 3), 18 + 2 * (k / 3), 0);
    member(7, M_TANK, 145, 20, 0);
    member(8, M_TANK, 145, 22, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the retreat decision runs");
    for (k = 1; k <= 6; k++) home &= squad_task_of(k) == IMPAI_SQ_FORM;
    printf("retreat members: the six home %s, the two %c\n", home ? "FORM" : "not FORM",
           impai_mil_letter((unsigned)squad_task_of(7)));
    CHECK(home, "the six members home: FORM");
    CHECK(squad_task_of(7) == IMPAI_SQ_RETREAT && squad_task_of(8) == IMPAI_SQ_RETREAT && s_mil.sq[0].n == 2,
          "two members still on their way: RETREAT");
    for (k = 7; k <= 8; k++) {
        s_w.unit[k].pos[0] = (24 * 16 + 8) << 16;
        s_w.unit[k].pos[2] = (24 * 16 + 8) << 16;
    }
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the rallied decision runs");
    CHECK(squad_task_of(7) == IMPAI_SQ_FORM && squad_task_of(8) == IMPAI_SQ_FORM && s_mil.sq[0].task != IMPAI_SQ_RETREAT,
          "every member arrived: FORM, and no RETREAT squad left");
}

/* check_mil_at_rally's world: mil_world, the anchor at cell (20, 20), the seat's start beside it at (328, 328) px
   and the other at (6 416, 3 216), no structure known, so the estimate is the other start */
static void rally_starts(void)
{
    mil_world();
    s_st.ax = s_st.az = 20;
    s_w.starts[0].x = s_w.starts[0].z = 328;
    s_w.starts[0].index = 0;
    s_w.starts[1].x = 6416;
    s_w.starts[1].z = 3216;
    s_w.starts[1].index = 1;
    s_w.nstarts = 2;
}

/* AT THE RALLY and THE RETREAT HOME (impai_mil.c): check_mil_stay_or_go's three tanks and a submarine they cannot
   hurt but outrun, so the submarine enters their stay-or-go and the defence makes them no DEFEND; the anchor at
   cell (20, 20), no structure known, so the estimate is the other start, (6 416, 3 216) px, and the rally point
   lies 384 px from the anchor towards it, about (675, 493) px. (a) The tanks at the rally, the submarine 128 px
   south: no prediction, no retreat, the tanks the scouting leaves FORM. (b) The same 2 400 px out: one prediction,
   lost, a retreat, as check_mil_stay_or_go's (c). (c) At the rally against an enemy turret in place of the
   submarine, a contact that does not threaten the base (a structure is no base threat): one prediction. (d) The
   tanks a RAID squad at the rally, its point 2 400 px out, the submarine 128 px south: one prediction, lost, every
   member already where a retreat would take it -- no retreat, the squad back to FORM, the loss stamped for the
   lead, and no order at the submarine it lost to. (e) The same raid
   2 400 px out: one prediction, a retreat. (f) The member's index is the one it takes in the layer's RETREAT squad
   it would join (below). (g) Three destroyers whose rally is ashore with no water within 1 024 px, each heading an
   attack: a sea ATTACK up the strait against six loses, retreats, and each is moved to the water nearest the rally,
   its HOME POINT, and is still on its way at the next decision; a FORM squad there with no contact is moved there
   too; with the water past HOME_BLOCKS the lost ATTACK retreats and each is stopped once, but one walking off its
   factory's yard (a lone Park); at its home point the lost ATTACK takes no retreat; and of five members in five
   components the fifth, past HOME_SPIRALS, is given nothing (below). (h) A DEFEND that loses at home keeps its
   task and travels home (below). */
static void check_mil_at_rally(void)
{
    static const char* const NAME[5] = { "a submarine at the rally", "a submarine 2 400 px out", "a turret at the rally",
                                         "a raid at the rally, a submarine", "a raid 2 400 px out, a submarine" };
    IMPAI_MIL_OUT o;
    int c, k, from, at;
    for (c = 0; c < 5; c++) {
        const int out = c == 1 || c == 4, bx = out ? 180 : 40, bz = out ? 100 : 29;
        mil_world();
        gunship();
        s_st.ax = s_st.az = 20;
        s_w.starts[0].x = s_w.starts[0].z = 328;
        s_w.starts[0].index = 0;
        s_w.starts[1].x = 6416;
        s_w.starts[1].z = 3216;
        s_w.starts[1].index = 1;
        s_w.nstarts = 2;
        if (c >= 3) {
            squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, c == 3 ? 90 : 120, c == 3 ? 50 : 70);
            for (k = 0; k < 3; k++) member(1 + k, M_TANK, bx + 2 * k, bz, 0);
        } else
            for (k = 0; k < 3; k++) own(1 + k, M_TANK, bx + 2 * k, bz);
        enemy(1300, c == 2 ? T_TURRET : M_SUB, bx + 2, bz + 8, IMPAI_LOS, 1);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the rally decision runs");
        mil_print(NAME[c], &o, from);
        for (k = from, at = 0; k < s_w.nlog; k++) at += s_w.log[k].c.target == 1300;
        if (!c)
            CHECK(!o.predictions && !o.retreats && squad_task_of(2) == IMPAI_SQ_FORM && squad_task_of(3) == IMPAI_SQ_FORM,
                  "at the rally: %u predictions, %u retreats, tasks %d %d", o.predictions, o.retreats, squad_task_of(2),
                  squad_task_of(3));
        else if (c == 3)
            CHECK(o.predictions == 1 && !o.retreats && squad_task_of(2) == IMPAI_SQ_FORM && !at && s_mil.lost_t,
                  "%s: %u predictions, %u retreats, task %d, %d orders at the submarine, lost at %u", NAME[c],
                  o.predictions, o.retreats, squad_task_of(2), at, s_mil.lost_t);
        else if (c == 2)
            CHECK(o.predictions == 1, "%s: %u predictions", NAME[c], o.predictions);
        else
            CHECK(o.predictions == 1 && o.retreats == 1, "%s: %u predictions, %u retreats", NAME[c], o.predictions,
                  o.retreats);
    }
    /* (f) the layer's RETREAT squad holds a tank 2 400 px out, slot 1, so a FORM tank, slot 2, joining it takes
       index 1, spread point (610, 364) px; the FORM tank stands at (72, 360), 538 px from that point and 474 from
       index 0's, 616 px from the rally: predicted, lost, and not home -- a retreat */
    mil_world();
    gunship();
    s_st.ax = s_st.az = 20;
    s_w.starts[0].x = s_w.starts[0].z = 328;
    s_w.starts[0].index = 0;
    s_w.starts[1].x = 6416;
    s_w.starts[1].z = 3216;
    s_w.starts[1].index = 1;
    s_w.nstarts = 2;
    squad_task(0, IMPAI_SQ_RETREAT, TASIM_LAYER_LAND, 674 >> 5, 492 >> 5);
    member(1, M_TANK, 180, 100, 0);
    own(2, M_TANK, 4, 22);
    enemy(1300, M_SUB, 4, 30, IMPAI_LOS, 1);
    from = s_w.nlog;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the joined retreat's decision runs");
    mil_print("a tank whose retreat joins the layer's", &o, from);
    CHECK(o.predictions == 1 && o.retreats == 1, "a tank home at its own index, not at the index it joins at: %u "
          "predictions, %u retreats", o.predictions, o.retreats);
    /* (g) the rally about (675, 493) px, block (21, 15): no block of the destroyers' component within SUB_BLOCKS of
       it. Each destroyer heads an attack, at the force it meets or, for a FORM squad, at the ground. In the ponds,
       one destroyer in each of five one-block ponds 79 to 95 blocks from the rally: five components, one more than
       HOME_SPIRALS. */
    for (c = 0; c < 5; c++) {
        static const char* const GN[5] = { "a lost sea ATTACK, the strait", "a sea FORM, the strait",
                                           "a lost sea ATTACK, the water past HOME_BLOCKS",
                                           "a lost sea ATTACK at its home point", "a sea FORM in five ponds" };
        const int n = c == 4 ? 5 : 3, form = c == 1 || c == 4;
        const int x0 = c == 2 ? 450 : c == 3 ? 241 : 250, z0 = c == 3 ? 30 : 100;
        int moves = 0, stops = 0, near_home = 0, heads = 0, later = 0, ordered = 0;
        rally_starts();
        /* an amphibious K-bot at home (class 2 crosses the water): the land army reaches the other start, so the
           map is no island and the sea squad's waves keep wave_value (impai_mil.c, THE ISLAND) */
        s_ti[M_AA].mclass = 2;
        s_w.defs[M_AA].d.mclass = 2;
        if (c == 2 || c == 4) {
            s_w.nwater = 0;
            s_w.sea = 50;
            if (c == 2) water_add(&s_w, 440, 0, 471, 255, 40);
            for (k = 0; c == 4 && k < 5; k++) water_add(&s_w, 200 + 8 * k, 40, 201 + 8 * k, 41, 40);
            mil_sea_types();
            fake_reach(&s_mreach, M_N);
            s_mil_reach = &s_mreach;
        } else
            mil_water(W_STRAIT);
        squad_task(0, form ? IMPAI_SQ_FORM : IMPAI_SQ_ATTACK, TASIM_LAYER_SEA, 128, 120);
        for (k = 0; k < n; k++) member(1 + k, M_DEST, c == 4 ? 200 + 8 * k : x0 + 2 * k, c == 4 ? 40 : z0, 0);
        own(30, M_AA, 24, 24);
        if (!form)
            for (k = 0; k < 6; k++) enemy(1300 + k, M_DEST, x0 + 2 * k, z0 + 8, IMPAI_LOS, 1);
        for (k = 1; k <= n; k++) {
            FUNIT* u = &s_w.unit[k];
            memset(&u->list[0], 0, sizeof u->list[0]);
            u->list[0].action = A_ATTACK;
            u->list[0].target = (uint16_t)(form ? WQ_NONE : 1300);
            u->list[0].pos[0] = (int32_t)((uint32_t)(x0 * 16 + 8) << 16);
            u->list[0].pos[2] = (int32_t)((uint32_t)((z0 + 8) * 16 + 8) << 16);
            u->n = 1;
        }
        if (c == 2) {           /* the third holds a factory's lone Park instead, walking off its yard */
            s_w.unit[3].list[0].action = A_PARK;
            s_w.unit[3].list[0].target = (uint16_t)WQ_NONE;
        }
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the sea squad's decision runs: %s", GN[c]);
        mil_print(GN[c], &o, from);
        for (k = from; k < s_w.nlog; k++) {
            const WQ_CMD* m = &s_w.log[k].c;
            const int bx = m->pos[0] >> 21, bz = m->pos[2] >> 21;
            if (m->unit < 1 || m->unit > n) continue;
            ordered |= 1 << m->unit;
            moves += m->action == A_MOVE;
            stops += m->action == A_STOP;
            near_home += m->action == A_MOVE && bx >= 120 && bx <= 135 && bz >= 11 && bz <= 19;
        }
        for (k = 1; k <= n; k++) heads += s_w.unit[k].n == 0 || s_w.unit[k].list[0].action != A_ATTACK;
        printf("%s: moves=%d near the home point=%d stops=%d heads replaced=%d ordered=0x%x unreached=%u\n", GN[c],
               moves, near_home, stops, heads, (unsigned)ordered, o.unreached);
        if (c == 0)
            CHECK(o.predictions == 1 && o.retreats == 1 && squad_task_of(1) == IMPAI_SQ_RETREAT && moves == 3 &&
                      near_home == 3 && heads == 3,
                  "%s: %u retreats, task %d, %d moves to the water nearest the rally, %d heads replaced", GN[c],
                  o.retreats, squad_task_of(1), near_home, heads);
        if (c == 1)
            CHECK(!o.retreats && squad_task_of(1) == IMPAI_SQ_FORM && moves == 3 && near_home == 3 && heads == 3,
                  "%s: task %d, %d moves to the water nearest the rally, %d heads replaced", GN[c], squad_task_of(1),
                  near_home, heads);
        if (c == 3)
            CHECK(o.predictions == 1 && !o.retreats && squad_task_of(1) == IMPAI_SQ_FORM,
                  "%s: home, no retreat: %u predictions, %u retreats, task %d", GN[c], o.predictions, o.retreats,
                  squad_task_of(1));
        if (c == 4)
            CHECK(moves == 4 && !stops && ordered == 0x1E && o.unreached == 1 && s_w.unit[5].list[0].action == A_ATTACK,
                  "%s: %d moves, %d stops, ordered 0x%x, %u unreached: the fifth component's search is past the cap, "
                  "and a capacity stops nothing", GN[c], moves, stops, (unsigned)ordered, o.unreached);
        if (c == 2) {
            CHECK(o.predictions == 1 && o.retreats == 1 && !moves && stops == 2 && ordered == 0x6 &&
                      s_w.unit[3].n == 1 && s_w.unit[3].list[0].action == A_PARK,
                  "%s: %u retreats, %d moves, %d stops, ordered 0x%x, the Park kept", GN[c], o.retreats, moves, stops,
                  (unsigned)ordered);
            /* the engine leaves a stopped unit one Standby the next append drops (WQ_OF_DROP); the fake, none */
            memset(&s_w.unit[2].list[0], 0, sizeof s_w.unit[2].list[0]);
            s_w.unit[2].list[0].action = A_PATROL;
            s_w.unit[2].list[0].target = (uint16_t)WQ_NONE;
            s_w.unit[2].list[0].flags = WQ_OF_DROP;
            s_w.unit[2].n = 1;
        }
        if (c != 0 && c != 2) continue;
        from = s_w.nlog;
        s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the next decision runs: %s", GN[c]);
        for (k = from; k < s_w.nlog; k++) later += s_w.log[k].c.unit >= 1 && s_w.log[k].c.unit <= 3;
        printf("%s, the next decision: %d orders to the destroyers, task %d\n", GN[c], later, squad_task_of(1));
        if (c == 0)
            CHECK(squad_task_of(1) == IMPAI_SQ_RETREAT, "%s: on its way to its home point, still RETREAT: task %d",
                  GN[c], squad_task_of(1));
        else
            CHECK(!later, "%s: no unit standing, dropping its default order or parking is stopped: %d orders", GN[c],
                  later);
    }
    /* (h) a held DEFEND: the rally 2 400 px out in this plan, about (2 496, 1 357) px, past THE ANCHOR BOX, three
       tanks defending there against a tank 640 px from the anchor -- a base threat they can hurt, out of their
       reach -- lose to eight tanks 128 px south, which threaten nothing. Every member stands home: no retreat, and
       across two decisions the squad stays DEFEND, no defence begun again, every order it is given a move within
       RALLY_PX of the rally, none at the threat. */
    {
        IMPAI_PLAN far = IMPAI_PLAN_ROW;
        int round, off = 0;
        far.rally_px = 2400;
        rally_starts();
        squad_task(0, IMPAI_SQ_DEFEND, TASIM_LAYER_LAND, 968 >> 5, 328 >> 5);
        for (k = 0; k < 3; k++) member(1 + k, M_TANK, 156 + 2 * k, 84, 0);
        enemy(1300, M_TANK, 60, 20, IMPAI_LOS, 1);
        for (k = 0; k < 8; k++) enemy(1310 + k, M_TANK, 154 + 2 * k, 92, IMPAI_LOS, 1);
        for (round = 0; round < 2; round++) {
            from = s_w.nlog;
            CHECK(mil_decide(&far, 1, &o) == 0, "the held defence's decision runs");
            mil_print("a held DEFEND", &o, from);
            for (k = from; k < s_w.nlog; k++) {
                const WQ_CMD* m = &s_w.log[k].c;
                const int64_t dx = (m->pos[0] >> 16) - 2496, dz = (m->pos[2] >> 16) - 1357;
                if (m->unit >= 1 && m->unit <= 3 && (m->action != A_MOVE || dx * dx + dz * dz > 512 * 512)) off++;
            }
            printf("a held DEFEND, decision %d: task %d, predictions=%u retreats=%u defends=%u orders off home=%d\n",
                   round, squad_task_of(1), o.predictions, o.retreats, o.defends, off);
            CHECK(o.predictions == 1 && !o.retreats && !o.defends && squad_task_of(1) == IMPAI_SQ_DEFEND && !off,
                  "a held DEFEND, decision %d: task %d, %u retreats, %u defences begun, %d orders off home", round,
                  squad_task_of(1), o.retreats, o.defends, off);
            s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
        }
    }
}

/* THE COMMANDER'S MEMORY IS THE SEAT'S OWN SIGHT (impai_mil.c, picture and wave_goal): a foe's commander shared by
   an ally (IMPAI_SHARED) is not remembered, one in the seat's own line of sight is; and a recorded entry of the
   seat's own is never a goal. */
static void check_mil_foe_sight(void)
{
    IMPAI_MIL_OUT o;
    int k;
    mil_world();
    enemy(1310, T_COM, 300, 100, IMPAI_SHARED, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the shared decision runs");
    CHECK(s_mil.foe[1].slot == WQ_NONE, "an ally's sight writes no memory");
    s_rows[1310].seat[0].how = IMPAI_LOS;
    s_w.gametime += IMPAI_PERSONAS[IMPAI_LV_HARD].look_mil;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the sight decision runs");
    CHECK(s_mil.foe[1].slot == 1310 && s_mil.foe[1].bx == 150 && s_mil.foe[1].bz == 50,
          "the seat's own sight writes it");
    mil_world();
    squad_task(0, IMPAI_SQ_ATTACK, TASIM_LAYER_LAND, 0, 0);
    for (k = 0; k < 4; k++) member(1 + k, M_TANK, 24 + 2 * k, 24, 0);
    s_mil.foe[0].slot = 5;
    s_mil.foe[0].bx = 200;
    s_mil.foe[0].bz = 100;
    s_mil.foe[0].t = 10;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the own entry decision runs");
    printf("foe sight: goal %d,%d\n", s_mil.sq[0].tx, s_mil.sq[0].tz);
    CHECK(!(s_mil.sq[0].tx == 200 && s_mil.sq[0].tz == 100), "the seat's own entry is no goal");
}

/* ---- travel's spread points (impai_mil.c) and THE ROUTE (impai_maps.c) ---- */

/* 1 when a 2 x 2 unit at (x, z) px touches the cell rectangle (x0..x1, z0..z1) grown by its half-size, 16 px */
static int touches(int32_t x, int32_t z, int x0, int z0, int x1, int z1)
{
    return x >= x0 * 16 - 16 && x < (x1 + 1) * 16 + 16 && z >= z0 * 16 - 16 && z < (z1 + 1) * 16 + 16;
}

/* THE YARD RULE (impai_mil.c, travel_point): a raid of 25 tanks travels to block (60, 30), (1936, 976) px. A factory
   of the seat's, cells (118..123, 58..63), stands on the point, and the economy keeps its way out south of it, cells
   (118..123, 64..70): eight of the 25 spread points touch one of the two. (a) No member is sent onto either, and
   every member is sent to a point of the grid. (b) With the factory and the way elsewhere, the spread is the plain
   5 x 5 grid, eight of its points on those cells. */
static void check_mil_yard_spread(void)
{
    static const char* const NAME[2] = { "a factory and its way on the point", "neither" };
    IMPAI_MIL_OUT o;
    int c, k;
    for (c = 0; c < 2; c++) {
        int from, moves = 0, on = 0, grid = 0;
        mil_world();
        squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, 60, 30);
        for (k = 0; k < 25; k++) member(1 + k, M_TANK, 200 + 2 * (k % 5), 100 + 2 * (k / 5), 0);
        own(40, T_FACTORY, c ? 18 : 118, 58);
        memset(&s_st.way[0], 0, sizeof s_st.way[0]);
        s_st.way[0].type = T_FACTORY;
        s_st.way[0].cx = (int16_t)(c ? 18 : 118);
        s_st.way[0].cz = 58;
        s_st.way[0].n = 1;
        s_st.way[0].r[0][0] = (int16_t)(c ? 18 : 118);
        s_st.way[0].r[0][1] = 64;
        s_st.way[0].r[0][2] = (int16_t)(c ? 23 : 123);
        s_st.way[0].r[0][3] = 70;
        s_st.nway = 1;
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the travel decision runs");
        for (k = from; k < s_w.nlog; k++) {
            const WQ_CMD* m = &s_w.log[k].c;
            const int32_t x = m->pos[0] >> 16, z = m->pos[2] >> 16;
            if (m->action != A_MOVE) continue;
            moves++;
            on += touches(x, z, 118, 58, 123, 63) || touches(x, z, 118, 64, 123, 70);
            grid += (x - 1808) % 64 == 0 && (z - 848) % 64 == 0 && x >= 1808 && x <= 2064 && z >= 848 && z <= 1104;
        }
        printf("mil yard spread, %s: moves=%d on a yard or way=%d on the grid=%d unreached=%u\n", NAME[c], moves, on,
               grid, o.unreached);
        CHECK(moves == 25 && on == (c ? 8 : 0) && grid == 25, "the raid's 25 members, %s: %d moves, %d on its cells",
              NAME[c], moves, on);
    }
    s_st.nway = 0;
}

/* THE SPREAD HELD TO THE MAP (impai_mil.c, spread): a raid of 25 tanks, the level with no reach labels so every
   spread point counts as reached -- as every point does for a VTOL --, travels to a corner block: (0, 0), at
   (16, 16) px, and (255, 127), at (8176, 4080), on the 8192 x 4096 px map. Every member's move is given and lies on
   the map, the off-map points held to its edge; none is refused. */
static void check_mil_spread_edge(void)
{
    static const int CORNER[2][2] = { { 0, 0 }, { 255, 127 } };
    IMPAI_MIL_OUT o;
    int c, k;
    for (c = 0; c < 2; c++) {
        int from, moves = 0, on_map = 0;
        mil_world();
        squad_task(0, IMPAI_SQ_RAID, TASIM_LAYER_LAND, CORNER[c][0], CORNER[c][1]);
        for (k = 0; k < 25; k++) member(1 + k, M_TANK, 200 + 2 * (k % 5), 100 + 2 * (k / 5), 0);
        from = s_w.nlog;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the travel decision runs");
        for (k = from; k < s_w.nlog; k++) {
            const WQ_CMD* m = &s_w.log[k].c;
            const int32_t x = m->pos[0] >> 16, z = m->pos[2] >> 16;
            if (m->action != A_MOVE) continue;
            moves++;
            on_map += x >= 0 && x < s_w.mw * 16 && z >= 0 && z < s_w.mh * 16;
        }
        printf("mil spread at the edge, block (%d, %d): moves=%d on the map=%d refused=%u\n", CORNER[c][0],
               CORNER[c][1], moves, on_map, o.refused);
        CHECK(moves == 25 && on_map == 25 && !o.refused, "the raid to block (%d, %d): %d moves, %d on the map",
              CORNER[c][0], CORNER[c][1], moves, on_map);
    }
}

/* THE ROUTE'S SAMPLER (impai_maps.c), on hand-made maps: it answers as the reference below, the builders' route
   test written out plainly, over segments from and to anywhere, off the grid and past the coordinate limit
   included. */
static int64_t old_cell_of_px(int64_t v) { return v >= 0 ? v / IMPAI_CELL_PX : -((-v + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX); }
static int64_t old_coord_held(int v) { return v < 0 ? 0 : v > 32767 ? 32767 : v; }
static int old_route(const IMPAI_MAPS* m, int64_t x0, int64_t z0, int x_px, int z_px)
{
    const int64_t dx = old_coord_held(x_px) - x0, dz = old_coord_held(z_px) - z0;
    const int64_t ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
    const int64_t n = (ax > az ? ax : az) / 32 + 1, ox = old_cell_of_px(x0), oz = old_cell_of_px(z0);
    int64_t i, lx = ox, lz = oz;
    for (i = 0; i <= n; i++) {
        const int64_t gx = old_cell_of_px(x0 + dx * i / n), gz = old_cell_of_px(z0 + dz * i / n);
        if ((gx == ox && gz == oz) || (gx == lx && gz == lz)) continue;
        lx = gx;
        lz = gz;
        if (gx >= 0 && gz >= 0 && gx < m->gw && gz < m->gh && m->threat[IMPAI_THREAT_GROUND][gz * m->gw + gx] > 0)
            return 1;
    }
    return 0;
}

static void check_route_rules(void)
{
    static uint32_t g[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    IMPAI_MAPS m;
    uint32_t seed = 12345u;
    int k, same = 0, threatened = 0;
    memset(&m, 0, sizeof m);
    m.threat[IMPAI_THREAT_GROUND] = g;
    m.gw = 8;
    m.gh = 8;
    for (k = 0; k < 64; k++) {
        seed = seed * 1103515245u + 12345u;
        g[k] = (seed >> 16) % 4 == 0 ? (seed >> 8) % 7 + 1 : 0;
    }
    for (k = 0; k < 4000; k++) {
        int32_t v[4];
        int i, a, b;
        for (i = 0; i < 4; i++) {
            seed = seed * 1103515245u + 12345u;
            v[i] = (int32_t)((seed >> 8) % 2600) - 300;
        }
        if (k % 10 == 0) v[2] = 40000;
        a = impai_route_threat(&m, v[0], v[1], v[2], v[3]);
        b = old_route(&m, v[0], v[1], v[2], v[3]);
        same += a == b;
        threatened += a;
    }
    printf("route rules: as the reference %d of 4000, %d threatened\n", same, threatened);
    CHECK(same == 4000 && threatened > 0 && threatened < 4000, "the route answers as the reference: %d of 4000", same);
    memset(g, 0, sizeof g);
    m.gw = 2;
    m.gh = 1;
    g[0] = 5; g[1] = 3;
    CHECK(impai_route_threat(&m, 128, 128, 896, 128) && !impai_route_threat(&m, 600, 128, 896, 128) &&
              !impai_route_threat(&m, 128, 128, 200, 100),
          "the cells past the start's are read, the start's own is not, cells off the grid read 0");
}

static int orders_to(int from, int unit)
{
    int k, n = 0;
    for (k = from; k < s_w.nlog; k++) n += s_w.log[k].c.unit == unit;
    return n;
}

/* THE PLAN'S ONE STORE THRESHOLD (impai_brain.c, THE STALLS and THE HOLD): the governor's world, one idle constructor
   and a quarter of each storage stored, more energy spent than comes in and the income past the plan's e_per_m. At
   stall_pm 200 neither store is below it: no hold, and no energy stall, so the constructor's first order is no
   energy building. At 300 both are: the hold, and the energy stall's rung sends it to an energy building. */
static void check_stall_pm(void)
{
    IMPAI_BRAIN_OUT bo;
    int c, a, k, from, i, energy;
    for (k = 0; k < 2; k++) {
        gov_world(1);
        own(4, T_CONS, 24, 24);
        s_w.econ[0].energy = 250;
        s_w.econ[0].energy_max = 1000;
        s_w.econ[0].energy_in = 300;
        s_w.econ[0].energy_out = 400;
        s_w.econ[0].metal = 250;
        s_w.econ[0].metal_max = 1000;
        s_glv.stall_pm = (uint16_t)(k ? 300 : 200);
        from = s_w.nlog;
        CHECK(gov_decide(&bo, &c, &a) == 0, "the stall_pm decision runs");
        energy = -1;
        for (i = from; i < s_w.nlog && energy < 0; i++)
            if (s_w.log[i].c.unit == 4 && s_w.log[i].c.action == A_MOBILEBUILD)
                energy = s_w.log[i].c.type < M_N && s_ti[s_w.log[i].c.type].cat == IMPAI_C_ENERGY;
        printf("stall_pm %u, a quarter stored: hold %u, the constructor's first build energy %d\n", s_glv.stall_pm,
               bo.hold, energy);
        CHECK(k ? energy == 1 && bo.hold : energy == 0 && !bo.hold, "stall_pm %u: hold %u, energy build %d",
              s_glv.stall_pm, bo.hold, energy);
    }
    s_milp = NULL;
}

/* ---- the persona and the reaction (impai_react.c) ---- */

/* the calibration ladder's rows L0..L5 (research/notes/impai/brain.md, The plan and the personas), each a persona
   impai_persona_ok must accept */
static const IMPAI_PERSONA LADDER[] = {
    { 75, 150, 60, 5, 7, 2, 2, 3, 1, { 0, 0 } },
    { 60, 90, 45, 10, 15, 3, 3, 4, 2, { 0, 0 } },
    { 45, 60, 30, 16, 24, 4, 4, 6, 2, { 0, 0 } },
    { 30, 45, 15, 24, 36, 4, 6, 8, 3, { 0, 0 } },
    { 15, 30, 0, 40, 80, 6, 10, 12, 4, { 0, 0 } },
    { 15, 30, 0, 0, 0, 0, 0, 0, 0, { 0, 0 } },
};

/* a limit compared so that 0, no limit, is the most: 1 when a's is no more than b's */
static int limit_le(unsigned a, unsigned b) { return !b || (a && a <= b); }

/* THE PERSONAS: every row and every ladder row accepted; a row breaking each rule refused; Hard's row the no-op
   persona (looks 15 and 30, no reaction, no budget, no attention); Easy no stronger than Normal, Normal than Hard,
   in every limit. */
static void check_personas(void)
{
    const IMPAI_PERSONA* e = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    const IMPAI_PERSONA* n = &IMPAI_PERSONAS[IMPAI_LV_NORMAL];
    const IMPAI_PERSONA* h = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    unsigned k;
    int bad;
    for (k = 0; k < IMPAI_LVS; k++) CHECK(impai_persona_ok(&IMPAI_PERSONAS[k]), "level %u's persona", k);
    for (k = 0; k < sizeof LADDER / sizeof LADDER[0]; k++) CHECK(impai_persona_ok(&LADDER[k]), "ladder row L%u", k);
    for (bad = 0; bad < 12; bad++) {
        IMPAI_PERSONA p = *n;
        switch (bad) {
        case 0: p.look_eco = 20; break;             /* a look not a multiple of 15 */
        case 1: p.look_mil = 165; break;            /* a look past 150 */
        case 2: p.look_eco = 0; break;              /* no look */
        case 3: p.react = 16; break;                /* past the shorter look less 15 */
        case 4: p.apm_eco = 601; break;             /* a rate past 600 */
        case 5: p.burst_mil = 0; break;             /* a rate with no burst */
        case 6: p.apm_eco = 0; break;               /* a burst with no rate */
        case 7: p.burst_eco = 61; break;            /* a burst past 60 */
        case 8: p.att_eco = IMPAI_IDLE_CAP + 1; break;
        case 9: p.att_mil = IMPAI_SQUADS + 1; break;
        case 10: p.pad[1] = 1; break;
        default: p.react = 0xFFFF; break;
        }
        CHECK(!impai_persona_ok(&p), "a persona breaking rule %d refused", bad);
    }
    CHECK(h->look_eco == 15 && h->look_mil == 30 && !h->react && !h->apm_eco && !h->burst_eco && !h->apm_mil &&
              !h->burst_mil && !h->att_eco && !h->att_mil,
          "Hard's persona is no limit at all");
    CHECK(e->look_eco >= n->look_eco && n->look_eco >= h->look_eco && e->look_mil >= n->look_mil &&
              n->look_mil >= h->look_mil && e->react >= n->react && n->react >= h->react &&
              limit_le(e->apm_eco, n->apm_eco) && limit_le(n->apm_eco, h->apm_eco) &&
              limit_le(e->apm_mil, n->apm_mil) && limit_le(n->apm_mil, h->apm_mil) &&
              limit_le(e->burst_eco, n->burst_eco) && limit_le(n->burst_eco, h->burst_eco) &&
              limit_le(e->burst_mil, n->burst_mil) && limit_le(n->burst_mil, h->burst_mil) &&
              limit_le(e->att_eco, n->att_eco) && limit_le(n->att_eco, h->att_eco) &&
              limit_le(e->att_mil, n->att_mil) && limit_le(n->att_mil, h->att_mil),
          "Easy <= Normal <= Hard in every limit");
    printf("personas: easy look=%u,%u react=%u normal look=%u,%u react=%u hard look=%u,%u react=%u\n", e->look_eco,
           e->look_mil, e->react, n->look_eco, n->look_mil, n->react, h->look_eco, h->look_mil, h->react);
}

/* a NEW command of `action` to `unit`, at `target` (WQ_NONE none) */
static WQ_CMD react_cmd(int unit, int action, int target)
{
    WQ_CMD c;
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)action;
    c.unit = (uint16_t)unit;
    c.target = (uint16_t)target;
    c.pos[0] = 100 << 16;
    c.pos[2] = 100 << 16;
    return c;
}

static int pend_tail_zero(const IMPAI_EXEC* x)
{
    static const IMPAI_PEND zero;
    unsigned k;
    for (k = x->npend; k < IMPAI_PEND_MAX; k++)
        if (memcmp(&x->pend[k], &zero, sizeof zero)) return 0;
    return 1;
}

/* THE REACTION over the fake, at Easy's react of 45: (a) at Hard the view is the WORLDQ itself; (b) an economy
   batch of two at 38 and a military batch of one at 53 reach the world at 83 and 98, in decision order, and
   nothing before; (c) the enqueue answers as the immediate order does, for a refused QUEUE and a dead target;
   (d) a killed actor, a reused actor slot and a reused target are refused at the apply; (e) the queue refuses at
   IMPAI_PEND_MAX; (f) restored bytes past the bound -- npend 9999, an entry due past the reaction, a bad verb --
   are dropped and counted, and the tail is zero; (g) an actor with no incarnation is refused at the enqueue. */
static void check_react_queue(void)
{
    static IMPAI_EXEC x;
    const IMPAI_PERSONA* p = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    WORLDQ q = { &FAKE, &s_w, 0 };
    IMPAI_EXEC_VIEW ve, vm;
    IMPAI_REACT_COUNTS n;
    const WORLDQ* v;
    const WORLDQ* m;
    WQ_CMD c;
    int from, k;
    mil_world();
    own(1, M_TANK, 4, 4);
    own(2, M_TANK, 6, 4);
    own(3, M_TANK, 8, 4);
    enemy(1300, M_TANK, 20, 20, IMPAI_LOS, 1);
    memset(&n, 0, sizeof n);
    impai_exec_reset(&x, p);
    CHECK(impai_exec_view(&ve, &q, &x, &IMPAI_PERSONAS[IMPAI_LV_HARD], 38, IMPAI_SRC_ECO, &n) == &q,
          "(a) Hard orders through the WORLDQ itself");

    from = s_w.nlog;
    v = impai_exec_view(&ve, &q, &x, p, 38, IMPAI_SRC_ECO, &n);
    c = react_cmd(1, A_MOVE, WQ_NONE);
    CHECK(v->ops->order(v, &c) == WQ_OK, "(b) the first economy order is taken");
    c = react_cmd(2, A_MOVE, WQ_NONE);
    CHECK(v->ops->order(v, &c) == WQ_OK, "(b) the second economy order is taken");
    m = impai_exec_view(&vm, &q, &x, p, 53, IMPAI_SRC_MIL, &n);
    c = react_cmd(3, A_ATTACK, 1300);
    CHECK(m->ops->order(m, &c) == WQ_OK, "(b) the military order is taken");
    CHECK(s_w.nlog == from && x.npend == 3 && x.pend[0].due == 83 && x.pend[2].due == 98 &&
              (x.pend[2].inc & WQ_INC_TARGET) && x.pend[2].src == IMPAI_SRC_MIL,
          "(b) three pending, due at 83 and 98, nothing given: %d given, %u pending", s_w.nlog - from, x.npend);
    impai_exec_apply(&x, &q, p, 82, &n);
    CHECK(s_w.nlog == from && x.npend == 3, "(b) nothing due at 82");
    impai_exec_apply(&x, &q, p, 83, &n);
    CHECK(s_w.nlog == from + 2 && s_w.log[from].c.unit == 1 && s_w.log[from + 1].c.unit == 2 && x.npend == 1 &&
              x.pend[0].unit == 3 && pend_tail_zero(&x),
          "(b) the economy batch given at 83 in its order, the military's kept and moved to the front");
    impai_exec_apply(&x, &q, p, 98, &n);
    CHECK(s_w.nlog == from + 3 && s_w.log[from + 2].c.unit == 3 && s_w.log[from + 2].c.target == 1300 &&
              !x.npend && pend_tail_zero(&x) && n.given == 3 && !n.late && !n.refused,
          "(b) the military's given at 98: given %u late %u refused %u", n.given, n.late, n.refused);

    c = react_cmd(1, A_MOVE, WQ_NONE);
    c.verb = WQ_CMD_QUEUE;
    c.type = M_TANK;
    c.count = 1;
    CHECK(v->ops->order(v, &c) == WQ_REFUSED && q.ops->order_test(&q, &c) == WQ_REFUSED && !x.npend,
          "(c) a QUEUE the unit's menu refuses is refused at once, as the immediate order");
    s_w.unit[1300].live = 0;
    c = react_cmd(3, A_ATTACK, 1300);
    CHECK(m->ops->order(m, &c) == WQ_REFUSED && q.ops->order_test(&q, &c) == WQ_REFUSED && !x.npend,
          "(c) an attack on a dead target is refused at once, as the immediate order");
    s_w.unit[1300].live = 1;

    memset(&n, 0, sizeof n);
    v = impai_exec_view(&ve, &q, &x, p, 200, IMPAI_SRC_ECO, &n);
    c = react_cmd(1, A_MOVE, WQ_NONE);
    v->ops->order(v, &c);
    c = react_cmd(2, A_MOVE, WQ_NONE);
    v->ops->order(v, &c);
    c = react_cmd(3, A_ATTACK, 1300);
    v->ops->order(v, &c);
    s_w.unit[1].live = 0;                   /* killed */
    s_w.birth[2]++;                         /* its slot taken by another unit of the seat's */
    s_w.birth[1300]++;                      /* the target's slot taken by another unit */
    from = s_w.nlog;
    impai_exec_apply(&x, &q, p, 245, &n);
    CHECK(s_w.nlog == from && n.refused == 3 && !n.given && !x.npend,
          "(d) a killed actor, a reused actor slot and a reused target refused: refused %u", n.refused);
    s_w.unit[1].live = 1;

    memset(&n, 0, sizeof n);
    v = impai_exec_view(&ve, &q, &x, p, 300, IMPAI_SRC_ECO, &n);
    for (k = 0; k <= IMPAI_PEND_MAX; k++) {
        c = react_cmd(2, A_MOVE, WQ_NONE);
        v->ops->order(v, &c);
    }
    CHECK(x.npend == IMPAI_PEND_MAX && n.full == 1 && n.pend_max == IMPAI_PEND_MAX,
          "(e) the queue refuses at %d: %u pending, full %u", IMPAI_PEND_MAX, x.npend, n.full);

    memset(&n, 0, sizeof n);
    x.npend = 9999;
    x.pend[0].due = 300 + p->react + 1;     /* past the reaction */
    x.pend[1].verb = 7;                     /* no verb the enqueue writes */
    x.pend[2].count = 9;                    /* a NEW with a count */
    from = s_w.nlog;
    impai_exec_apply(&x, &q, p, 300, &n);
    CHECK(x.npend == IMPAI_PEND_MAX - 3 && n.bad == 9999 - IMPAI_PEND_MAX + 3 && s_w.nlog == from &&
              pend_tail_zero(&x),
          "(f) restored bytes past the bound dropped and counted: %u pending, bad %u", x.npend, n.bad);
    impai_exec_apply(&x, &q, p, 345, &n);
    CHECK(!x.npend && n.given == IMPAI_PEND_MAX - 3 && pend_tail_zero(&x), "(f) the rest given at their tick");

    memset(&n, 0, sizeof n);
    s_w.nobirth = 1;
    c = react_cmd(2, A_MOVE, WQ_NONE);
    CHECK(v->ops->order(v, &c) == WQ_REFUSED && n.unborn == 1 && !x.npend,
          "(g) an actor the world gives no incarnation is refused at the enqueue");
    s_w.nobirth = 0;
    c = react_cmd(2, A_MOVE, WQ_NONE);
    c.verb = WQ_CMD_REMOVE;
    CHECK(v->ops->order(v, &c) == WQ_REFUSED && n.verb == 1 && !x.npend, "(g) a REMOVE is refused at the enqueue");
}

/* THE BUDGET: an exact refill of apm units a tick, held to the burst; restored bytes -- tok -5 and 10^9, a filled
   tick past now -- and a gap of 2^31 are held; spending what acts answers keeps the bucket at or above 0; no rate,
   no budget. */
/* A PENDING ORDER IS DATA (impai_react.c, THE BOUND; worldq.h, wq_cmd_formed). (a) Restored entries, well formed
   for the queue, that name an action byte of 200 and of 0, a type past the defs, a point west of the map and one
   south of it, and a target without its incarnation bit: each refused by the world's order at the apply, and the
   one good entry beside them given. (b) A delayed attack on a radar-only contact is bound to the contact's
   incarnation: the slot taken by another unit before the apply, the attack is refused. */
static void check_pend_bounds(void)
{
    static IMPAI_EXEC x;
    const IMPAI_PERSONA* p = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    WORLDQ q = { &FAKE, &s_w, 0 };
    IMPAI_EXEC_VIEW vm;
    IMPAI_REACT_COUNTS n;
    const WORLDQ* m;
    WQ_CMD c;
    int k, from;
    mil_world();
    own(2, M_TANK, 6, 4);
    own(3, M_TANK, 8, 4);
    enemy(1300, M_TANK, 20, 20, IMPAI_LOS, 1);
    impai_exec_reset(&x, p);
    for (k = 0; k < 7; k++) {
        IMPAI_PEND* e = &x.pend[k];
        e->due = 400;
        e->birth = s_w.birth[2];
        e->unit = 2;
        e->target = (uint16_t)WQ_NONE;
        e->pos[0] = 100 << 16;
        e->pos[2] = 100 << 16;
        e->verb = WQ_CMD_NEW;
        e->action = A_MOVE;
        e->src = IMPAI_SRC_ECO;
        e->inc = WQ_INC_UNIT;
    }
    x.pend[1].action = 200;
    x.pend[2].action = 0;
    x.pend[3].type = (uint16_t)s_w.ndefs;
    x.pend[4].pos[0] = -(1 << 16);
    x.pend[5].pos[2] = (int32_t)((int64_t)s_w.mh * 16 << 16);
    x.pend[6].action = A_ATTACK;
    x.pend[6].target = 1300;
    x.pend[6].tbirth = s_w.birth[1300] + 1u;
    x.npend = 7;
    memset(&n, 0, sizeof n);
    from = s_w.nlog;
    impai_exec_apply(&x, &q, p, 400, &n);
    printf("pend bounds (a): given %u refused %u bad %u, orders %d\n", n.given, n.refused, n.bad, s_w.nlog - from);
    CHECK(n.given == 1 && n.refused == 6 && !n.bad && s_w.nlog == from + 1 && s_w.log[from].c.action == A_MOVE &&
              !x.npend,
          "(a) every malformed restored entry refused by the world, the good one given");

    enemy(1301, M_TANK, 22, 20, IMPAI_RADAR, 1);
    memset(&n, 0, sizeof n);
    m = impai_exec_view(&vm, &q, &x, p, 500, IMPAI_SRC_MIL, &n);
    c = react_cmd(3, A_ATTACK, 1301);
    CHECK(m->ops->order(m, &c) == WQ_OK && x.npend == 1 && (x.pend[0].inc & WQ_INC_TARGET) &&
              x.pend[0].tbirth == s_w.birth[1301],
          "(b) the attack on the radar-only contact is bound to its incarnation");
    s_w.birth[1301]++;
    from = s_w.nlog;
    impai_exec_apply(&x, &q, p, 545, &n);
    printf("pend bounds (b): given %u refused %u\n", n.given, n.refused);
    CHECK(n.refused == 1 && !n.given && s_w.nlog == from, "(b) the slot taken before the apply: the attack refused");
}

static void check_bucket(void)
{
    static IMPAI_EXEC x;
    IMPAI_PERSONA p = IMPAI_PERSONAS[IMPAI_LV_EASY];
    int k, a, low = 0;
    impai_exec_reset(&x, &p);
    CHECK(x.tok[IMPAI_SRC_ECO] == p.burst_eco * 1800 && x.tok[IMPAI_SRC_MIL] == p.burst_mil * 1800,
          "the reset fills both buckets");
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 0) == 3, "a full bucket: the burst");
    impai_exec_spend(&x, IMPAI_SRC_ECO, &p, 3);
    CHECK(x.tok[IMPAI_SRC_ECO] == 0, "spent to 0");
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 179) == 0 && x.tok[IMPAI_SRC_ECO] == 1790,
          "179 ticks at 10 a minute: 1790 units, no action");
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 180) == 1 && x.tok[IMPAI_SRC_ECO] == 1800,
          "180 ticks: exactly one action");
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 100000) == 3 && x.tok[IMPAI_SRC_ECO] == 5400, "held to the burst");
    CHECK(impai_exec_acts(&x, IMPAI_SRC_MIL, &p, 100000) == 3, "the military's bucket is its own");
    x.tok[IMPAI_SRC_ECO] = -5;
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 100000) == 0 && x.tok[IMPAI_SRC_ECO] == 0, "a restored -5 reads 0");
    x.tok[IMPAI_SRC_ECO] = 1000000000;
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 100000) == 3 && x.tok[IMPAI_SRC_ECO] == 5400,
          "a restored 10^9 reads the burst");
    x.tok[IMPAI_SRC_ECO] = 0;
    x.filled[IMPAI_SRC_ECO] = 200000;
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 100000) == 0 && x.filled[IMPAI_SRC_ECO] == 100000,
          "a filled tick past now refills nothing");
    x.filled[IMPAI_SRC_ECO] = 0;
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &p, 0x80000000u) == 3 && x.tok[IMPAI_SRC_ECO] == 5400,
          "a gap of 2^31 refills the burst and no more");
    impai_exec_reset(&x, &p);
    for (k = 0; k < 20000; k += 60) {
        a = impai_exec_acts(&x, IMPAI_SRC_ECO, &p, (uint32_t)k);
        impai_exec_spend(&x, IMPAI_SRC_ECO, &p, a);
        if (x.tok[IMPAI_SRC_ECO] < 0) low = 1;
    }
    CHECK(!low, "spending what acts answers never takes the bucket below 0");
    CHECK(impai_exec_acts(&x, IMPAI_SRC_ECO, &IMPAI_PERSONAS[IMPAI_LV_HARD], 0) == -1, "Hard: no budget");
}

/* a squad of seat 0's holding `task` at block (tx, tz) of the land layer, with army unit `slot` as its member,
   standing at home far from the point */
static void held_squad(int s, unsigned task, int slot, int tx, int tz)
{
    own(slot, M_TANK, 40 + 2 * (slot % 8), 44);
    s_mil.sq[s].task = (uint8_t)task;
    s_mil.sq[s].layer = TASIM_LAYER_LAND;
    s_mil.sq[s].target = (uint16_t)WQ_NONE;
    s_mil.sq[s].tx = (int16_t)tx;
    s_mil.sq[s].tz = (int16_t)tz;
    s_mil.sq[s].since = (uint32_t)s_w.gametime;
    s_mil.sq[s].closest = IMPAI_CLOSEST_NONE;
    s_job[slot].kind = IMPAI_JOB_SQUAD;
    s_job[slot].type = M_TANK;
    s_job[slot].squad = (uint8_t)s;
}

static int tasks_held(unsigned task)
{
    int k, n = 0;
    for (k = 0; k < IMPAI_SQUADS; k++) n += s_mil.sq[k].task == task;
    return n;
}

/* THE BUDGET'S RESERVES (impai_mil.c, acts_kept). (a) The squads never spend the bucket's last action: with three
   squads to serve -- two ATTACKs held far off and the FORM squad -- at Easy's burst of 3, a decision with 3, 2 and 1
   actions spends 2, 1 and 0 of them. (b) A marker past one reaction holds nothing (impai_com_marked). */
static void check_budget_reserves(void)
{
    IMPAI_PERSONA b = IMPAI_PERSONAS[IMPAI_LV_EASY];
    IMPAI_MIL_OUT o;
    int k, acts;
    s_persona = &b;
    for (acts = 3; acts >= 1; acts--) {
        mil_world();
        held_squad(9, IMPAI_SQ_ATTACK, 30, 200, 100);
        held_squad(10, IMPAI_SQ_ATTACK, 31, 200, 104);
        for (k = 0; k < 4; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
        s_macts = acts;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(a) the reserve's decision runs");
        printf("budget reserves (a), %d actions: spent %u roots %u\n", acts, o.acts, o.roots);
        CHECK(o.acts == (unsigned)(acts - 1), "(a) %d actions: %u spent, the last kept", acts, o.acts);
    }
    s_macts = -1;
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    CHECK(impai_com_marked(145, 100, &IMPAI_PERSONAS[IMPAI_LV_EASY]) &&
              !impai_com_marked(146, 100, &IMPAI_PERSONAS[IMPAI_LV_EASY]) &&
              !impai_com_marked(100, 100, &IMPAI_PERSONAS[IMPAI_LV_EASY]) &&
              !impai_com_marked(0xFFFFFFFFu, 100, &IMPAI_PERSONAS[IMPAI_LV_EASY]) &&
              !impai_com_marked(145, 100, &IMPAI_PERSONAS[IMPAI_LV_HARD]),
          "(c) a marker holds after now and within one reaction only");
}

/* DEFENCE IS EXEMPT FROM THE ATTENTION (the owner's decision, 2026-11-14): at Easy's att_mil of 2, with two ATTACK
   waves held far off, eight tanks at the base and six enemy tanks seen 256 px off it, the FORM squad still DEFENDs */
static void check_attention_defence(void)
{
    IMPAI_MIL_OUT o;
    int k;
    mil_world();
    held_squad(9, IMPAI_SQ_ATTACK, 30, 200, 100);
    held_squad(10, IMPAI_SQ_ATTACK, 31, 200, 104);
    own(30, M_TANK, 300, 150);                   /* both waves on their way, far from the base's fight */
    own(31, M_TANK, 304, 150);
    for (k = 0; k < 8; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
    for (k = 0; k < 6; k++) enemy(1300 + k, M_TANK, 24 + 2 * k, 40, IMPAI_LOS, 1);
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the defence-exempt decision runs");
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    printf("attention defence: defends=%u ops=%u attacks held=%d defend held=%d\n", o.defends, o.ops,
           tasks_held(IMPAI_SQ_ATTACK), tasks_held(IMPAI_SQ_DEFEND));
    CHECK(o.defends == 1 && tasks_held(IMPAI_SQ_DEFEND) == 1 && tasks_held(IMPAI_SQ_ATTACK) == 2 && o.ops == 2,
          "a DEFEND forms past att_mil with two waves out");
}

/* THE ATTENTION, the military's: with att_mil 1 and an ATTACK held far off, (a) the raid of check_mil_raid_scout
   does not go and leaves every raider in a squad with a task, and goes with att_mil 2; (b) the twelve tanks of
   check_mil_attack start no wave; (c) with IMPAI_ATTACKS_MAX ATTACKs held, past att_mil 1 as restored bytes may
   hold them, the wave reinforces one: a merge, which the attention allows. */
static void check_attention_mil(void)
{
    IMPAI_PERSONA one = IMPAI_PERSONAS[IMPAI_LV_HARD], two;
    IMPAI_MIL_OUT o;
    int k, att;
    one.att_mil = 1;
    two = one;
    two.att_mil = 2;
    for (att = 1; att <= 2; att++) {
        mil_world();
        held_squad(9, IMPAI_SQ_ATTACK, 30, 200, 100);
        own(1, M_TANK, 24, 24);
        for (k = 0; k < 3; k++) own(4 + k, M_RAIDER, 24 + 2 * k, 28);
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        s_persona = att == 1 ? &one : &two;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(a) the raid decision runs");
        s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
        printf("attention mil (a), att_mil %d: raids=%u ops=%u\n", att, o.raids, o.ops);
        if (att == 1) {
            CHECK(!o.raids && !tasks_held(IMPAI_SQ_RAID) && o.ops == 1, "(a) no raid past one operation");
            for (k = 4; k < 7; k++)
                CHECK(s_mil.sq[s_job[k].squad].task != IMPAI_SQ_FREE, "(a) raider %d in a squad with a task", k);
        } else {
            CHECK(o.raids == 1 && o.ops == 2, "(a) the raid goes with room for it");
        }
    }

    mil_world();
    held_squad(9, IMPAI_SQ_ATTACK, 30, 200, 100);
    for (k = 0; k < 12; k++) own(1 + k, M_TANK, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
    s_persona = &one;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(b) the wave decision runs");
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    printf("attention mil (b): attacks=%u ops=%u\n", o.attacks, o.ops);
    CHECK(!o.attacks && tasks_held(IMPAI_SQ_ATTACK) == 1, "(b) no wave past one operation");

    mil_world();
    for (k = 0; k < IMPAI_ATTACKS_MAX; k++) held_squad(9 + k, IMPAI_SQ_ATTACK, 30 + k, 200 + 4 * k, 100);
    for (k = 0; k < 12; k++) own(1 + k, M_TANK, 24 + 2 * (k % 6), 24 + 2 * (k / 6));
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
    s_persona = &one;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(c) the reinforcing decision runs");
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    printf("attention mil (c): attacks=%u ops=%u\n", o.attacks, o.ops);
    CHECK(o.attacks == 1 && tasks_held(IMPAI_SQ_ATTACK) == IMPAI_ATTACKS_MAX, "(c) the wave reinforces a held one");
}

/* THE LEAD (impai_mil.c, lead), on the military's world: tanks of 300 each, the content's commander worth F, an
   enemy extractor's ghost worth X known in every case but (a) -- the estimate resting on a structure, THE
   EVIDENCE --, the seat's start before it.
   (a) Nothing known: the enemy is the commander's floor, F, and no lead at twice 2.5 F: no evidence.
   (b) The extractor known: the enemy is X + F, its owner's floor; the seat enters the lead at 2.5 (X + F) and not
       below; held, it ends below 2 (X + F) and not at it; not held, 2 (X + F) begins nothing.
   (c) At 2.5 (X + F) with one enemy tank remembered out of sight: no lead, the ghost counting; the same ghost an
       ally's: the lead; an enemy commander's ghost: its value and no floor beside it; the commander seen once
       (IMPAI_MIL.foe) and its row gone: no floor, the enemy X; a second foe's extractor: X + X and two floors.
   (g) THE EVIDENCE: a prediction lost IMPAI_DRY_T ago: no lead; 15 ticks later: the lead; held, a prediction
       lost now ends it.
   (h) THE MOBILE MEMORY: five enemy tanks seen, gone from the rows 15 ticks later: the enemy counts their 1 500 at
       (1 - 15 / IMPAI_DRY_T); IMPAI_DRY_T after the sighting, nothing; six seen 15 ticks after the five: the
       memory 1 800, stamped now.
   (d) IN THE LEAD at Easy's att_mil 2 with two ATTACKs held and the land's wave count at 40: thirty tanks form a
       third ATTACK -- outside the attention, so the operations counted are 0, at the base size -- and the count
       stays 40; out of the lead (an enemy army remembered) none.
   (e) THE LEAD'S END: the next decision with that army remembered ends the lead, and the three ATTACKs come back
       to att_mil's two, the operations counted 2; the one demoted, a held ATTACK whose wave counted, gives it back,
       the count 40 to 39 (THE WAVE'S COUNT).
   (f) THE DRY HUNT on the strait world, no land stamp ever, an extractor's ghost on the east bank: in the lead
       and out of it the seat is not dry -- the lead starts no hunt.
   (i) THE LEAD'S HOME FLOOR: forty tanks at home, the lead held, the memory at 1 500: the wave leaves five tanks
       (1 500) at home in the FORM squad and thirty-five go; the memory at 0: all forty go, the FORM squad itself
       the ATTACK; out of the lead with the memory at 1 500: all forty; in the lead at 1 500 with a tower worth
       1 500 at home besides, the home holding the memory without the FORM squad: all forty.
   (j) THE LEAD'S WAVE at the base: in the lead, five tanks at home, below wave_value's eight median units: no
       wave; ten (3 000) with the memory at 1 500: five would go, below the base, so none. */
static int lead_case(int tanks, int held_lead, int known, IMPAI_MIL_OUT* o)
{
    int k;
    mil_world();
    for (k = 0; k < tanks; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
    if (known) enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 0);
    s_mil.lead = (uint8_t)held_lead;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, o) == 0, "the lead's decision runs");
    return o->lead;
}

static int near_f(float a, float b) { return fabsf(a - b) <= 0.01f; }

/* the units of the land FORM squad, and of the land ATTACK squads together */
static int land_form_n(void)
{
    int k, n = 0;
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (s_mil.sq[k].task == IMPAI_SQ_FORM && s_mil.sq[k].layer == TASIM_LAYER_LAND) n += s_mil.sq[k].n;
    return n;
}

static int land_attack_n(void)
{
    int k, n = 0;
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (s_mil.sq[k].task == IMPAI_SQ_ATTACK && s_mil.sq[k].layer == TASIM_LAYER_LAND) n += s_mil.sq[k].n;
    return n;
}

static void check_mil_lead(void)
{
    const float F = s_ti[T_COM].value, X = s_ti[T_MEX].value;
    IMPAI_PERSONA easy = IMPAI_PERSONAS[IMPAI_LV_EASY];
    IMPAI_MIL_OUT o;
    int k, n_in, n_out;
    uint32_t t0;
    n_in = (int)(2.5f * (X + F) / 300.0f) + 1;      /* the fewest tanks worth 2.5 (X + F) */
    n_out = (int)(2.0f * (X + F) / 300.0f) + 1;     /* the fewest worth 2 (X + F) */
    printf("lead: F=%.1f X=%.1f, %d tanks to begin it, %d to hold it\n", F, X, n_in, n_out);
    lead_case(2 * n_in, 0, 0, &o);
    printf("lead (a) %d tanks, nothing known: own %.0f enemy %.0f lead %u\n", 2 * n_in, o.own_v, o.enemy_v, o.lead);
    CHECK(!o.lead && o.enemy_v == F, "(a) nothing known: the floor, and no lead with no evidence");
    lead_case(n_in - 1, 0, 1, &o);
    printf("lead (b) %d tanks: own %.0f enemy %.0f lead %u\n", n_in - 1, o.own_v, o.enemy_v, o.lead);
    CHECK(!o.lead && near_f(o.enemy_v, X + F) && o.own_v == 300.0f * (float)(n_in - 1),
          "(b) the extractor and its owner's floor, no lead");
    CHECK(lead_case(n_in, 0, 1, &o) && !o.lead_was, "(b) at 2.5 (X + F) the lead begins");
    CHECK(lead_case(n_out, 1, 1, &o) && o.lead_was, "(b) held at 2 (X + F): the lead holds");
    CHECK(!lead_case(n_out - 1, 1, 1, &o) && o.lead_was, "(b) held below 2 (X + F): the lead ends");
    CHECK(!lead_case(n_out, 0, 1, &o), "(b) not held, 2 (X + F) begins nothing");

    mil_world();
    for (k = 0; k < n_in; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 0);
    enemy(1301, M_TANK, 300, 200, IMPAI_GHOST, 0);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(c) the remembered tank's decision runs");
    printf("lead (c) a tank remembered: enemy %.0f lead %u\n", o.enemy_v, o.lead);
    CHECK(!o.lead && near_f(o.enemy_v, X + 300.0f + F), "(c) a mobile ghost counts");
    s_rows[1301].seat[0].owner = 2;
    s_allies = 1u << 2;
    s_mil.lead = 0;
    s_mil.mob_v = 0.0f;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0 && o.lead && near_f(o.enemy_v, X + F),
          "(c) an ally's ghost counts nothing");
    s_allies = 0;
    s_rows[1301].seat[0].how = 0;
    enemy(1302, T_COM, 300, 200, IMPAI_GHOST, 0);
    s_mil.lead = 0;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(c) the remembered commander's decision runs");
    printf("lead (c) a commander remembered: enemy %.0f lead %u\n", o.enemy_v, o.lead);
    CHECK(near_f(o.enemy_v, X + F) && o.lead, "(c) a commander known: its value, no floor beside it");
    s_rows[1302].seat[0].how = 0;
    s_mil.mob_v = 0.0f;
    s_mil.foe[1].slot = 1302;
    s_mil.foe[1].bx = 150;
    s_mil.foe[1].bz = 100;
    s_mil.foe[1].t = (uint32_t)s_w.gametime;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(c) the commander seen once, its row gone: the decision runs");
    printf("lead (c) the commander seen once and gone: enemy %.0f\n", o.enemy_v);
    CHECK(near_f(o.enemy_v, X), "(c) a commander seen once counts no floor when its row is gone");
    s_mil.foe[1].slot = (uint16_t)WQ_NONE;
    enemy(1303, T_MEX, 410, 200, IMPAI_GHOST, 0);
    s_rows[1303].seat[0].owner = 2;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(c) two foes' decision runs");
    printf("lead (c) two foes named: enemy %.0f\n", o.enemy_v);
    CHECK(near_f(o.enemy_v, X + X + 2.0f * F), "(c) two foes named, no commander of either: two floors");

    for (k = 0; k < 3; k++) {
        mil_world();
        s_w.gametime = (int)IMPAI_DRY_T + 1000;
        for (n_out = 0; n_out < n_in; n_out++) own(1 + n_out, M_TANK, 24 + 2 * (n_out % 8), 24 + 2 * (n_out / 8));
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 0);
        s_mil.lost_t = (uint32_t)s_w.gametime - (k == 1 ? IMPAI_DRY_T + 15u : k == 0 ? IMPAI_DRY_T : 0u);
        s_mil.lead = (uint8_t)(k == 2);
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(g) the evidence's decision runs");
        printf("lead (g) a prediction lost %u ago, held %d: lead %u\n", (uint32_t)s_w.gametime - s_mil.lost_t,
               k == 2, o.lead);
        if (k == 0) CHECK(!o.lead, "(g) a prediction lost IMPAI_DRY_T ago: no lead");
        if (k == 1) CHECK(o.lead, "(g) lost IMPAI_DRY_T + 15 ago: the lead");
        if (k == 2) CHECK(!o.lead && o.lead_was, "(g) held, a prediction lost now: the lead ends");
    }

    mil_world();
    s_w.gametime = 3000;
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 0);
    for (k = 0; k < 5; k++) enemy(1310 + k, M_TANK, 300, 200 + 2 * k, IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(h) the sighting's decision runs");
    t0 = (uint32_t)s_w.gametime;
    printf("lead (h) five tanks seen: enemy %.0f memory %.0f at %u\n", o.enemy_v, s_mil.mob_v, s_mil.mob_t);
    CHECK(near_f(o.enemy_v, X + 1500.0f + F) && s_mil.mob_v == 1500.0f && s_mil.mob_t == t0,
          "(h) the tanks seen: the memory 1 500, stamped now");
    for (k = 0; k < 5; k++) {
        s_rows[1310 + k].seat[0].how = 0;
        s_w.unit[1310 + k].live = 0;
    }
    s_w.gametime += 15;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(h) the decision after runs");
    printf("lead (h) gone 15 ticks later: enemy %.2f\n", o.enemy_v);
    CHECK(near_f(o.enemy_v, X + 1500.0f * (1.0f - 15.0f / (float)IMPAI_DRY_T) + F) && s_mil.mob_t == t0,
          "(h) gone from the rows: the memory decays, its stamp kept");
    s_w.gametime = (int)(t0 + IMPAI_DRY_T);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0 && near_f(o.enemy_v, X + F),
          "(h) IMPAI_DRY_T after the sighting the memory counts nothing");
    s_w.gametime = (int)t0 + 15;
    s_mil.mob_v = 1500.0f;
    s_mil.mob_t = t0;
    for (k = 0; k < 6; k++) enemy(1310 + k, M_TANK, 300, 200 + 2 * k, IMPAI_LOS, 1);
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0 && s_mil.mob_v == 1800.0f &&
              s_mil.mob_t == (uint32_t)s_w.gametime,
          "(h) a larger sum replaces the memory");

    for (k = 0; k < 2; k++) {
        mil_world();
        held_squad(9, IMPAI_SQ_ATTACK, 60, 200, 100);
        held_squad(10, IMPAI_SQ_ATTACK, 61, 200, 104);
        own(60, M_TANK, 300, 150);
        own(61, M_TANK, 304, 150);
        for (n_out = 0; n_out < 30; n_out++) own(1 + n_out, M_TANK, 24 + 2 * (n_out % 8), 24 + 2 * (n_out / 8));
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        s_mil.waves[TASIM_LAYER_LAND] = 40;
        if (k) enemy_away(150);
        s_persona = &easy;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(d) the lead's wave decision runs");
        s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
        printf("lead (d) %s: lead %u attacks %u ops %u held %d waves %u\n", k ? "an army remembered" : "in the lead",
               o.lead, o.attacks, o.ops, tasks_held(IMPAI_SQ_ATTACK), s_mil.waves[TASIM_LAYER_LAND]);
        if (!k)
            CHECK(o.lead && o.attacks == 1 && !o.ops && tasks_held(IMPAI_SQ_ATTACK) == 3 &&
                      s_mil.waves[TASIM_LAYER_LAND] == 40,
                  "(d) in the lead a third ATTACK forms past att_mil 2, none counted, and the wave count stays");
        else
            CHECK(!o.lead && !o.attacks && tasks_held(IMPAI_SQ_ATTACK) == 2, "(d) out of the lead no wave");
    }

    mil_world();
    held_squad(9, IMPAI_SQ_ATTACK, 60, 200, 100);
    held_squad(10, IMPAI_SQ_ATTACK, 61, 200, 104);
    own(60, M_TANK, 300, 150);
    own(61, M_TANK, 304, 150);
    for (k = 0; k < 30; k++) own(1 + k, M_TANK, 24 + 2 * (k % 8), 24 + 2 * (k / 8));
    enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
    s_mil.sq[9].counted = s_mil.sq[10].counted = 1;
    s_mil.waves[TASIM_LAYER_LAND] = 40;
    s_persona = &easy;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0 && o.lead && tasks_held(IMPAI_SQ_ATTACK) == 3 &&
              s_mil.waves[TASIM_LAYER_LAND] == 40,
          "(e) three ATTACKs in the lead, the count 40");
    enemy_away(150);
    s_w.gametime += 15;
    CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(e) the lead's end decision runs");
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
    printf("lead (e) the lead's end: lead %u was %u ops %u held %d\n", o.lead, o.lead_was, o.ops,
           tasks_held(IMPAI_SQ_ATTACK));
    CHECK(!o.lead && o.lead_was && o.ops == 2 && tasks_held(IMPAI_SQ_ATTACK) == 2 &&
              s_mil.waves[TASIM_LAYER_LAND] == 39,
          "(e) the lead's end gives the room back: two ATTACKs at att_mil 2, the counted wave given back");

    for (k = 0; k < 2; k++) {
        int t;
        mil_world();
        mil_water(W_STRAIT);
        s_w.gametime = 3000;
        for (t = 0; t < 30; t++) own(1 + t, M_TANK, 24 + 2 * (t % 8), 24 + 2 * (t / 8));
        enemy(1301, T_MEX, 300, 100, IMPAI_GHOST, 0);
        if (k) enemy_away(150);
        dry_case(k ? "(f) out of the lead, no land stamp" : "(f) in the lead, no land stamp", &o);
        CHECK(o.lead == !k && !s_mil.dry && !s_mil.land_t, "(f) %s the lead, no land stamp: not dry",
              k ? "out of" : "in");
    }
    s_mil_reach = NULL;

    for (k = 0; k < 8; k++) {
        static const char* const NAME[8] = { "in the lead, the memory 1 500", "in the lead, the memory 0",
                                             "out of the lead, the memory 1 500",
                                             "in the lead, the memory 1 500, a tower of 1 500 at home",
                                             "in the lead, the enemy's commander remembered",
                                             "in the lead, the commander and five tanks remembered",
                                             "in the lead, five radar contacts never identified",
                                             "in the lead, the memory 1 500 half decayed" };
        static const int FORM[8] = { 5, 0, 0, 0, 0, 5, 5, 3 };
        int t;
        mil_world();
        if (k == 7) s_w.gametime = (int)IMPAI_DRY_T + 3000;
        for (t = 0; t < 40; t++) own(1 + t, M_TANK, 24 + 2 * (t % 8), 24 + 2 * (t / 8));
        if (k == 3) {
            tower_def();
            s_ti[T_TURRET].value = 1500.0f;
            own(60, T_TURRET, 22, 30);
        }
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        if (k == 4 || k == 5) enemy(1301, T_COM, 410, 200, IMPAI_GHOST, 1);
        if (k == 5 || k == 6)
            for (t = 0; t < 5; t++) {
                enemy(1310 + t, M_TANK, 400, 210 + 2 * t, k == 5 ? IMPAI_GHOST : IMPAI_RADAR, 1);
                if (k == 6) {
                    s_rows[1310 + t].seat[0].type = (uint16_t)WQ_NONE;
                    s_rows[1310 + t].seat[0].owner = 0xFF;
                }
            }
        s_mil.lead = (uint8_t)(k != 2);
        s_mil.mob_v = s_mil.raid_v = k == 1 || (k >= 4 && k <= 6) ? 0.0f : 1500.0f;
        s_mil.mob_t = s_mil.raid_t = (uint32_t)s_w.gametime - (k == 7 ? IMPAI_DRY_T / 2 : 0u);
        s_mil.lost_t = k == 2 ? (uint32_t)s_w.gametime : 0u;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(i) the home floor's decision runs");
        printf("lead (i) %s: lead %u enemy %.0f memories %.0f / %.0f form %d attack %d\n", NAME[k], o.lead,
               o.enemy_v, s_mil.mob_v, s_mil.raid_v, land_form_n(), land_attack_n());
        CHECK(o.lead == (k != 2) && land_form_n() == FORM[k] && land_attack_n() == 40 - FORM[k],
              "(i) %s: %d kept, %d go", NAME[k], FORM[k], 40 - FORM[k]);
        if (k == 4 || k == 5)
            CHECK(near_f(o.enemy_v, s_ti[T_MEX].value + F + (k == 5 ? 1500.0f : 0.0f)) &&
                      near_f(s_mil.mob_v, F + (k == 5 ? 1500.0f : 0.0f)) &&
                      near_f(s_mil.raid_v, k == 5 ? 1500.0f : 0.0f),
                  "(i) %s: the ratio counts the commander, the floor does not", NAME[k]);
    }

    /* (k): two layers' lead waves in one decision, forty tanks and twenty destroyers at home, the raid memory 1 500:
       the second wave reads the home the first left, so the home keeps 1 500 */
    {
        int t, form_v = 0;
        mil_world();
        mil_sea_types();
        for (t = 0; t < 40; t++) own(1 + t, M_TANK, 24 + 2 * (t % 8), 24 + 2 * (t / 8));
        for (t = 0; t < 20; t++) own(50 + t, M_DEST, 24 + 2 * (t % 10), 36 + 2 * (t / 10));
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        s_mil.lead = 1;
        s_mil.mob_v = s_mil.raid_v = 1500.0f;
        s_mil.mob_t = s_mil.raid_t = (uint32_t)s_w.gametime;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(k) two layers' lead waves: the decision runs");
        for (t = 0; t < IMPAI_SQUADS; t++)
            if (s_mil.sq[t].task == IMPAI_SQ_FORM) form_v += (int)s_mil.sq[t].value;
        printf("lead (k) two layers: lead %u attacks %u land %u sea %u, kept at home %d\n", o.lead, o.attacks,
               o.waves[TASIM_LAYER_LAND], o.waves[TASIM_LAYER_SEA], form_v);
        CHECK(o.lead && o.waves[TASIM_LAYER_LAND] == 1 && o.waves[TASIM_LAYER_SEA] == 1 && form_v >= 1500 &&
                  form_v < 1500 + 500,
              "(k) both layers' waves go and the home keeps the raid memory: %d", form_v);
    }

    /* (j): the lead held by 35 tanks of a held ATTACK squad */
    for (n_in = 5; n_in <= 10; n_in += 5) {
        mil_world();
        for (k = 0; k < 35; k++) held_squad(9, IMPAI_SQ_ATTACK, 100 + k, 200, 100);
        for (k = 0; k < n_in; k++) own(1 + k, M_TANK, 24 + 2 * k, 24);
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        s_mil.lead = 1;
        s_mil.mob_v = s_mil.raid_v = n_in == 10 ? 1500.0f : 0.0f;
        s_mil.mob_t = s_mil.raid_t = (uint32_t)s_w.gametime;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(j) the lead's small FORM squad's decision runs");
        printf("lead (j) %d tanks at home: lead %u attacks %u form %d attack %d\n", n_in, o.lead, o.attacks,
               land_form_n(), land_attack_n());
        CHECK(o.lead && !o.attacks && land_form_n() == n_in && land_attack_n() == 35,
              "(j) in the lead, %d at home: no wave below the base", n_in);
    }

    /* (l) THE RECORDED FIELDS' BOUNDS (impai.h, IMPAI_MIL: lost_t, the two memories), as a restore may hand them:
       thirty tanks, the extractor's ghost, nothing mobile known.
       (0) the memories stamped 100 000 ticks ahead, 1 500 each: read as stamped now, the whole 1 500 counted;
       (1) lost_t 100 000 ticks ahead: a prediction lost now, no lead;
       (2) mob_v NaN, raid_v infinite: both 0, the ratio the extractor and the floor, the lead;
       (3) mob_v -5, raid_v minus infinity: both 0 */
    for (k = 0; k < 4; k++) {
        const uint32_t ahead = 100000u;
        int t;
        mil_world();
        s_w.gametime = 3000;
        for (t = 0; t < 30; t++) own(1 + t, M_TANK, 24 + 2 * (t % 8), 24 + 2 * (t / 8));
        enemy(1300, T_MEX, 400, 200, IMPAI_GHOST, 1);
        if (k == 0) {
            s_mil.mob_v = s_mil.raid_v = 1500.0f;
            s_mil.mob_t = s_mil.raid_t = (uint32_t)s_w.gametime + ahead;
        }
        if (k == 1) s_mil.lost_t = (uint32_t)s_w.gametime + ahead;
        if (k == 2) {
            s_mil.mob_v = NAN;
            s_mil.raid_v = INFINITY;
        }
        if (k == 3) {
            s_mil.mob_v = -5.0f;
            s_mil.raid_v = -INFINITY;
        }
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(l) the bounds' decision runs");
        printf("lead (l) case %d: enemy %.0f lead %u mob %g at %u raid %g at %u lost %u\n", k, o.enemy_v, o.lead,
               (double)s_mil.mob_v, s_mil.mob_t, (double)s_mil.raid_v, s_mil.raid_t, s_mil.lost_t);
        if (k == 0)
            CHECK(near_f(o.enemy_v, X + F + 1500.0f) && s_mil.mob_t == (uint32_t)s_w.gametime &&
                      s_mil.raid_t == (uint32_t)s_w.gametime && s_mil.mob_v == 1500.0f && s_mil.raid_v == 1500.0f,
                  "(l) memories stamped ahead: held to now, counted whole");
        if (k == 1) CHECK(!o.lead && s_mil.lost_t == (uint32_t)s_w.gametime, "(l) lost_t ahead: held to now, no lead");
        if (k >= 2)
            CHECK(o.lead && near_f(o.enemy_v, X + F) && s_mil.mob_v == 0.0f && s_mil.raid_v == 0.0f,
                  "(l) memories not finite or below 0: read as 0");
    }
}

/* THE GOAL'S PROGRESS (impai_mil.c, goal_stalled, give_up, goal_failed): an ATTACK squad of one tank standing at
   home, its goal an enemy factory's ghost 3 000 px off, which it never comes nearer. Its window W, task_timeout +
   its travel, is past task_timeout + TRAVEL_K x (the anchor's distance to the goal - rally_px) at the tank's speed.
   (a) It holds the goal through task_timeout + that travel, gives it up within TRAVEL_CAP more: the goal into the
       ring with its layer and time, its sweep cell stamped, the squad on to another goal and still an ATTACK.
   (b) The factory is passed over while the ring holds it, at IMPAI_UNREACHED_T too, and is the goal again past it,
       the squad's goal set far from it before each decision so that it is a goal taken afresh.
   (c) The tank brought 320 px nearer half way through: the window starts again there, the same W.
   (d) An enemy commander seen half way through at the map's far side, farther than the factory, so coming no nearer
       it is no progress: the goal moves to it (THE COMMANDER COMES FIRST) and its window starts there, past
       task_timeout + its own travel. */
static int s_com_at;                /* progress_run: the GameTime an enemy commander comes into sight, 0 none */

static int progress_run(int move_at, uint32_t* t0)
{
    IMPAI_MIL_OUT o;
    int i;
    for (i = 0; i < 2000; i++) {
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "the progress decision runs");
        if (!i) *t0 = (uint32_t)s_w.gametime;
        if (o.stalled) return s_w.gametime;
        if (move_at && s_w.gametime >= move_at) {
            s_w.unit[30].pos[0] += 300 << 16;
            s_w.unit[30].pos[2] += 115 << 16;
            move_at = 0;
        }
        if (s_com_at && s_w.gametime >= s_com_at) {
            enemy(1301, T_COM, 440, 220, IMPAI_LOS, 1);
            s_com_at = 0;
        }
        s_w.gametime += 15;
    }
    return -1;
}

static void progress_world(void)
{
    mil_world();
    held_squad(9, IMPAI_SQ_ATTACK, 30, 200, 100);
    enemy(1300, T_FACTORY, 200, 100, IMPAI_GHOST, 0);
}

static void check_mil_progress(void)
{
    const int32_t gx = 100 * 32 + 16, gz = 50 * 32 + 16, ax = s_st.ax * 16 + 8, az = s_st.az * 16 + 8;
    const double anchor_d = sqrt((double)(gx - ax) * (gx - ax) + (double)(gz - az) * (gz - az));
    const double floor_w = IMPAI_PLAN_ROW.task_timeout +
                           2.0 * (anchor_d - IMPAI_PLAN_ROW.rally_px) * 65536.0 / (double)s_w.defs[M_TANK].d.speed;
    const IMPAI_GOALFAIL* f = &s_mil.unreached[0];
    uint32_t t0, t1;
    int tg, tg2, i, back;
    unsigned cell;
    progress_world();
    tg = progress_run(0, &t0);
    printf("progress (a): goal (%d, %d) given up at %d, %d after its first measure; W at least %.0f\n",
           f->bx, f->bz, tg, tg - (int)t0, floor_w);
    CHECK(tg > 0 && (double)(tg - (int)t0) > floor_w &&
              tg - (int)t0 <= (int)IMPAI_PLAN_ROW.task_timeout + 16200 + 15,
          "(a) held through task_timeout + its travel, given up within TRAVEL_CAP more");
    cell = (unsigned)((int64_t)gz * IMPAI_SWEEP_SIDE / (s_w.mh * 16) * IMPAI_SWEEP_SIDE +
                      (int64_t)gx * IMPAI_SWEEP_SIDE / (s_w.mw * 16));
    CHECK(f->bx == 100 && f->bz == 50 && f->layer == TASIM_LAYER_LAND && f->t == (uint32_t)tg &&
              s_mil.unreached_next == 1 && s_mil.swept[cell],
          "(a) the goal in the ring with its layer and time, its sweep cell stamped");
    CHECK(s_mil.sq[9].task == IMPAI_SQ_ATTACK && !(s_mil.sq[9].tx == 100 && s_mil.sq[9].tz == 50),
          "(a) the squad on to another goal");
    for (i = 0, back = 0; i < 20; i++) {
        IMPAI_MIL_OUT o;
        s_w.gametime += 15;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(b) the ring's decision runs");
        back |= s_mil.sq[9].tx == 100 && s_mil.sq[9].tz == 50;
    }
    CHECK(!back, "(b) the factory passed over while the ring holds it");
    for (i = 0; i < 2; i++) {
        /* the squad's goal just taken far from the factory, so that the factory is a new goal when chosen */
        IMPAI_MIL_OUT o;
        s_mil.sq[9].tx = 20;
        s_mil.sq[9].tz = 100;
        s_mil.sq[9].closest = IMPAI_CLOSEST_NONE;
        s_w.gametime = tg + (int)IMPAI_UNREACHED_T + 15 * i;
        CHECK(mil_decide(&IMPAI_PLAN_ROW, 1, &o) == 0, "(b) the expiry's decision runs");
        printf("progress (b): %d ticks after the give-up the goal (%d, %d)\n", s_w.gametime - tg, s_mil.sq[9].tx,
               s_mil.sq[9].tz);
        back = s_mil.sq[9].task == IMPAI_SQ_ATTACK && s_mil.sq[9].tx == 100 && s_mil.sq[9].tz == 50;
        if (!i) CHECK(!back, "(b) at IMPAI_UNREACHED_T the ring still holds the factory");
        else CHECK(back, "(b) past IMPAI_UNREACHED_T the factory is the goal again");
    }
    progress_world();
    t1 = (uint32_t)s_w.gametime;
    tg2 = progress_run((int)t1 + (tg - (int)t0) / 2, &t0);
    printf("progress (c): moved at %d, given up at %d: %d after the move against %d\n",
           (int)t0 + (tg - (int)t0) / 2, tg2, tg2 - ((int)t0 + (tg - (int)t0) / 2), tg - (int)t0);
    CHECK(tg2 > 0 && tg2 - (tg - (int)t0) >= (int)t0 + (tg - (int)t0) / 2 - 15 &&
              tg2 - (tg - (int)t0) <= (int)t0 + (tg - (int)t0) / 2 + 30,
          "(c) progress starts the window again: the same W after the move");
    {
        const int32_t bx = 220 * 32 + 16, bz = 110 * 32 + 16;
        const double b_d = sqrt((double)(bx - ax) * (bx - ax) + (double)(bz - az) * (bz - az));
        const double floor_b = IMPAI_PLAN_ROW.task_timeout +
                               2.0 * (b_d - IMPAI_PLAN_ROW.rally_px) * 65536.0 / (double)s_w.defs[M_TANK].d.speed;
        int at;
        progress_world();
        at = s_w.gametime + (tg - (int)t0) / 2;
        s_com_at = at;
        tg2 = progress_run(0, &t0);
        printf("progress (d): the commander at %d, given up at %d (%d after; W at least %.0f), goal (%d, %d)\n",
               at, tg2, tg2 - at, floor_b, s_mil.unreached[0].bx, s_mil.unreached[0].bz);
        CHECK(tg2 > 0 && s_mil.unreached[0].bx == 220 && s_mil.unreached[0].bz == 110 && (double)(tg2 - at) > floor_b,
              "(d) a goal moved past ARRIVE_PX is measured afresh: its own window from the move");
    }
}

/* THE ATTENTION, the economy's, and the reaction's whole path. (a) The economy game through the queue and the
   bucket at Easy's persona: each decision's commands within its actions (decide), its busy builders within
   att_eco, and every order given exactly react ticks after its decision. (b) check_caps' decision, six idle
   constructors: Hard gives more than two builder orders, att_eco 2 at most two, its busy builders two at most. (c)
   check_gov_opening's last case, one constructor and two army units, where Hard queues a second constructor:
   with att_eco 1 the governor queues army instead, and with att_eco 2 too, the commander being the second
   builder the attention counts; with the commander gone, the second constructor. */
static void check_attention_eco(void)
{
    static IMPAI_EXEC x;
    IMPAI_PERSONA two = IMPAI_PERSONAS[IMPAI_LV_HARD], one = IMPAI_PERSONAS[IMPAI_LV_HARD];
    IMPAI_BRAIN_OUT out;
    EGAME_END e;
    int k, i, builds, c, a;
    two.att_eco = 2;
    one.att_eco = 1;
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_EASY];
    s_xp = &x;
    memset(&s_rc, 0, sizeof s_rc);
    s_quiet = 1;
    e = economy_game("Comet Catcher, Easy", 0, 324 - EGAME_DX, 35);
    s_quiet = 0;
    s_xp = NULL;
    printf("react easy: given=%u late=%u refused=%u full=%u bad=%u unborn=%u verb=%u pend_max=%u busy_max=%d\n",
           s_rc.given, s_rc.late, s_rc.refused, s_rc.full, s_rc.bad, s_rc.unborn, s_rc.verb, s_rc.pend_max,
           s_busy_max);
    CHECK(s_rc.given == (unsigned)e.orders && !s_rc.late && !s_rc.full && !s_rc.bad && !s_rc.unborn && !s_rc.verb,
          "(a) every order given at its due tick, none malformed");
    CHECK(s_rc.pend_max <= IMPAI_PLAN_ROW.roots, "(a) one economy batch pending at most: %u", s_rc.pend_max);
    CHECK(s_busy_max <= s_persona->att_eco, "(a) busy builders within att_eco: %d of %u", s_busy_max,
          s_persona->att_eco);

    for (k = 0; k < 2; k++) {
        brain_world();
        unit_add(&s_w, 1, T_COM, 20, 20, 1);
        for (i = 0; i < 6; i++) unit_add(&s_w, 2 + i, T_CONS, 14 + 2 * i, 30, 1);
        unit_add(&s_w, 10, T_FACTORY, 30, 30, 1);
        unit_add(&s_w, 11, T_FACTORY, 40, 35, 1);
        s_w.gametime = 900;
        s_persona = k ? &two : &IMPAI_PERSONAS[IMPAI_LV_HARD];
        CHECK(decide(&out) == 0, "(b) decide");
        for (i = 0, builds = 0; i < s_w.nlog; i++) builds += s_w.log[i].c.verb != WQ_CMD_QUEUE;
        printf("attention eco (b), att_eco %u: builder orders %d busy %u\n", s_persona->att_eco, builds, out.busy);
        CHECK(k ? builds <= 2 && out.busy <= 2 : builds > 2,
              "(b) builder orders %d, busy %u", builds, out.busy);
    }

    gov_world(1);
    gov_units(T_CONS, 1, 10);
    gov_units(M_RAIDER, 2, 100);
    s_persona = &one;
    CHECK(gov_decide(&out, &c, &a) == 0, "(c) decide");
    printf("attention eco (c), att_eco 1: queued %d constructors, %d army\n", c, a);
    CHECK(!c && a == 1, "(c) no second constructor past one busy builder: %d, %d", c, a);
    {
        IMPAI_PERSONA p2 = IMPAI_PERSONAS[IMPAI_LV_HARD];
        int dead;
        p2.att_eco = 2;
        for (dead = 0; dead < 2; dead++) {
            gov_world(1);
            gov_units(T_CONS, 1, 10);
            gov_units(M_RAIDER, 2, 100);
            if (dead) s_w.unit[1].live = 0;
            s_persona = &p2;
            CHECK(gov_decide(&out, &c, &a) == 0, "(c) decide");
            printf("attention eco (c), att_eco 2, %s: queued %d constructors, %d army\n",
                   dead ? "no commander" : "the commander", c, a);
            CHECK(dead ? c == 1 : !c && a == 1, "(c) att_eco 2, %s: %d constructors, %d army",
                  dead ? "no commander" : "the commander", c, a);
        }
    }
    s_milp = NULL;
    s_persona = &IMPAI_PERSONAS[IMPAI_LV_HARD];
}

/* THE REASON TRACE's text (impai_brain.c, impai_why_text), and the tally of decide_traced over every economy
   decision of this test: each want was seen needed, vetoed and taken where the fixtures reach it. The text of
   one trace by hand -- the first want held by two gates of its row, the fifth taken, the sea plant needed under
   no gate --; the longest line, a trace of every bit set, fits IMPAI_WHY_TEXT, and a buffer too short is ended
   inside itself. */
static void check_why(void)
{
    IMPAI_WHY w = { 1, IMPAI_G_HOLD | IMPAI_G_M_STALL | IMPAI_G_SURPLUS, 1, IMPAI_WHY_DRY | IMPAI_WHY_ISLAND,
                    1u << IMPAI_W_FACTORY | 1u << IMPAI_W_SEA | 1u << IMPAI_W_AA, 1u << IMPAI_W_AA,
                    { [IMPAI_W_FACTORY] = IMPAI_G_HOLD | IMPAI_G_M_STALL } };
    static const IMPAI_WHY NONE = { 1, 0, -1, 0, 0, 0, { 0 } };
    char t[IMPAI_WHY_TEXT], cut[9];
    int k, n;
    n = impai_why_text(&w, t, sizeof t);
    CHECK(!strcmp(t, "gates=HOLD+M_STALL+SURPLUS naval=1 pos=dry+island need=factory+sea+aa veto=factory:HOLD+M_STALL "
                     "take=aa") && n == (int)strlen(t), "the trace's text: `%s`", t);
    impai_why_text(&NONE, t, sizeof t);
    CHECK(!strcmp(t, "gates=- naval=-1 pos=- need=- veto=- take=-"), "the empty trace's text: `%s`", t);
    memset(&w, 0xFF, sizeof w);
    w.gates = IMPAI_G_ALL;
    w.naval = -1;
    for (k = 0; k < IMPAI_WANTS; k++) w.veto[k] = IMPAI_G_ALL;
    n = impai_why_text(&w, t, sizeof t);
    CHECK(n == 481 && n < IMPAI_WHY_TEXT - 1 && n == (int)strlen(t), "the longest trace fits its buffer: %d of %d", n,
          IMPAI_WHY_TEXT);
    memset(cut, 'x', sizeof cut);
    CHECK(impai_why_text(&w, cut, 8) == 7 && !memcmp(cut, "gates=H\0x", 9), "a short buffer is ended inside itself");
    {
        /* THE WHY LINE's rule (impai_why_due): a line at the first trace and at each change, none between; a line
           whatever the trace at a GameTime not above the last decision's -- the first decision after a restore
           behind it, and the same GameTime again --, the rule going on from there; a trace not set writes nothing
           and moves nothing, so the decision after it at a lower GameTime than its own is still in order */
        IMPAI_WHY_SEEN seen;
        IMPAI_WHY a = NONE, b = NONE, unset = NONE;
        b.gates = IMPAI_G_HOLD;
        unset.set = 0;
        memset(&seen, 0, sizeof seen);
        CHECK(impai_why_due(&seen, 0, &a) == 1 && impai_why_due(&seen, 30, &a) == 0 &&
                  impai_why_due(&seen, 60, &b) == 1 && impai_why_due(&seen, 90, &b) == 0,
              "a `why` line at the first trace and at each change, none between");
        CHECK(impai_why_due(&seen, 30, &b) == 1 && impai_why_due(&seen, 60, &b) == 0 &&
                  impai_why_due(&seen, 60, &b) == 1 && impai_why_due(&seen, 90, &b) == 0,
              "a decision at a GameTime not above the seat's last writes its line whatever its trace");
        CHECK(impai_why_due(&seen, 300, &unset) == 0 && impai_why_due(&seen, 120, &b) == 0 &&
                  !memcmp(&seen.why, &b, sizeof b) && seen.after == 121,
              "a trace not set writes no line and moves nothing");
    }
    /* the fixtures reach a veto of every want whose row names a gate but the sea plant's, which no decision of
       this test holds back; the anti-air's row names none */
    CHECK(s_why_veto[IMPAI_W_FACTORY] && s_why_veto[IMPAI_W_TECH] && s_why_veto[IMPAI_W_DEF] &&
              s_why_veto[IMPAI_W_SENSOR] && !s_why_veto[IMPAI_W_AA] && s_why_last,
          "the test's decisions see each want vetoed, and one held by its last term");
    printf("the reason trace: %u decisions; needed, vetoed, taken:", s_why_n);
    for (k = 0; k < IMPAI_WANTS; k++)
        printf(" %s %u, %u, %u%s", impai_veto[k].want + 5, s_why_need[k], s_why_veto[k], s_why_take[k],
               k + 1 < IMPAI_WANTS ? ";" : "");
    printf("; needed and held by the term read last: %u; the longest line %d bytes\n", s_why_last, n);
}

/* THE REQUESTS' tally over every economy decision the test made (decide_traced) */
static void check_requests(void)
{
    static const char* const STATE[IMPAI_RQ_STATES] = { "none", "vetoed", "held", "open", "answered", "yielded", "refused" };
    int k, st;
    /* the fixtures reach every end a rung gives a request but the sensor's refusal: no decision of this test asks
       a builder that offers a sensor and places none */
    CHECK(s_req[IMPAI_W_FACTORY][IMPAI_RQ_ANSWERED] && s_req[IMPAI_W_FACTORY][IMPAI_RQ_YIELDED] &&
              s_req[IMPAI_W_SEA][IMPAI_RQ_ANSWERED] && s_req[IMPAI_W_SEA][IMPAI_RQ_REFUSED] &&
              s_req[IMPAI_W_TECH][IMPAI_RQ_ANSWERED] && s_req[IMPAI_W_TECH][IMPAI_RQ_YIELDED] &&
              s_req[IMPAI_W_DEF][IMPAI_RQ_ANSWERED] && s_req[IMPAI_W_AA][IMPAI_RQ_ANSWERED] &&
              s_req[IMPAI_W_SENSOR][IMPAI_RQ_ANSWERED] && !s_req[IMPAI_W_SENSOR][IMPAI_RQ_REFUSED],
          "the test's decisions see each request answered, the plants' yield and the sea plant's refused");
    printf("the requests, as the decisions left them:");
    for (k = 0; k < IMPAI_WANTS; k++) {
        printf(" %s", impai_veto[k].want + 5);
        for (st = 0; st < IMPAI_RQ_STATES; st++)
            if (s_req[k][st]) printf(" %s %u", STATE[st], s_req[k][st]);
        printf("%s", k + 1 < IMPAI_WANTS ? ";" : "\n");
    }
}

/* THE DECISION'S READS, over every decision this test made at Hard: no decision reads the head or the order list
   of a unit it has already ordered, so a decision whose orders wait in the reaction's queue plans exactly as one
   whose orders reached the world at once; and no unit but a commander is ordered by both managers. */
static void check_reads_before_orders(void)
{
    printf("decisions' reads: %u orders taken, %u reads of a slot already ordered, %u non-commander slots both "
           "managers ordered\n", s_orders_taken, s_reads_after, s_shared_other);
    CHECK(!s_reads_after, "no read of a unit after the same decision ordered it: %u", s_reads_after);
    CHECK(!s_shared_other, "only commanders are ordered by both managers: %u", s_shared_other);
}

static void check_military(void)
{
    if (!(s_mscratch = malloc(impai_mil_scratch_bytes()))) {
        s_fail++;
        return;
    }
    check_mil_due();
    check_mil_defence();
    check_mil_base_threat();
    check_mil_gone_target();
    check_mil_aa_in_ground_fight();
    check_mil_budget();
    check_mil_slot_taken();
    check_mil_radar_aim();
    check_mil_sub_mixed_weapons();
    check_mil_retreat();
    check_mil_home_holds();
    check_mil_attack();
    check_mil_goal_side();
    check_mil_contact_defences();
    check_mil_stand_off();
    check_mil_scout_deep();
    check_scout_sea_class();
    check_mil_stand_out();
    check_mil_stand_progress();
    check_mil_shell_ghost();
    check_mil_shore_commander();
    check_mil_estimate();
    check_mil_starts_looked();
    check_mil_raid_scout();
    check_mil_commander();
    check_commander_handback();
    check_stand_dgun();
    check_ring_order();
    check_mil_bounds();
    check_mil_table();
    check_mil_wave_moves_on();
    check_mil_hunt();
    check_mil_arrived();
    check_mil_two_sides();
    check_mil_retreat_members();
    check_mil_side_engaged();
    check_mil_at_rally();
    check_mil_foe_sight();
    check_mil_dgun();
    check_production();
    check_gov_opening();
    check_gov_pressure();
    check_posture();
    check_gov_refused();
    check_gov_surplus();
    check_gov_scout_surplus();
    check_gov_air_scout();
    check_gov_five();
    check_gov_dry();
    check_army_targets();
    check_island_budget();
    check_targets_known();
    check_gov_finish_rows();
    check_eco_shares();
    check_factory_turn();
    check_stall_reclaimers();
    check_plant_energy();
    check_harvest_lost();
    check_feature_drawn();
    check_feature_memory();
    check_feature_memory_drop();
    check_feature_memory_ordered();
    check_blast_approach();
    check_reclaim_confidence();
    check_gov_loss_loop();
    check_hold_floor();
    check_hold_cons();
    check_hold_data();
    check_gov_turn_backs();
    check_hold_turnback();
    check_mil_pressure();
    check_mil_radar_as();
    check_mil_substitute();
    check_mil_stay_or_go();
    check_mil_wave_plant();
    check_mil_recall();
    check_mil_recall_home();
    check_air_held();
    check_mil_wave_layers();
    check_mil_arrival_cache();
    check_def_choice();
    check_def_want();
    check_sensor_struct();
    check_sensor_type();
    check_sensor_want();
    check_reach_summary();
    check_naval();
    check_dry_hunt();
    check_mil_start_banks();
    check_dry_wave();
    check_island();
    check_scout_layer();
    check_air_scout_trip();
    check_water_held();
    check_scout_healthy();
    check_dry_army_reach();
    check_dry_given_up();
    check_dry_under();
    check_layers();
    check_mil_mixed_sweep();
    check_mil_attack_room();
    check_mil_lifetimes();
    check_mil_retreat_home();
    check_mil_yard_spread();
    check_mil_spread_edge();
    check_stall_pm();
    check_personas();
    check_react_queue();
    check_pend_bounds();
    check_bucket();
    check_attention_mil();
    check_attention_defence();
    check_budget_reserves();
    check_mil_lead();
    check_mil_progress();
    free(s_mscratch);
}

int main(void)
{
    if (!(s_scratch = malloc(impai_brain_scratch_bytes()))) return 2;
    check_world();
    s_reads_after = s_shared_other = s_orders_taken = 0;   /* check_world drives the ops by hand, outside a decision */
    check_radar_reuse();
    check_seen_gone();
    check_scan_done();
    check_tinfo();
    check_tinfo_attack();
    check_tinfo_hits();
    check_army_damage();
    check_threat_reach();
    check_tech_levels();
    check_due();
    check_veto_rows();
    check_ladder_rows();
    check_stall();
    check_spot();
    check_spot_foreign();
    check_far_patch();
    check_factory();
    check_ring_reach();
    check_ring_cut();
    check_ring_unseen();
    check_ring_cmd_safe();
    check_cmd_safe_orders();
    check_ring_raid();
    check_assist_help();
    check_own_label();
    check_help_refused();
    check_commander_helps_factory();
    check_hold_assist();
    check_hold_release();
    check_hold_release_budget();
    check_caps();
    check_park();
    check_way();
    check_way_blockers();
    check_way_work();
    check_plateau();
    check_far();
    check_far_plant();
    check_claims_bound();
    check_fail();
    check_reclaim();
    check_harvest();
    check_harvest_held();
    check_route();
    check_explore_held();
    check_held_ground();
    check_route_rules();
    check_route_radar();
    check_walk_back();
    check_walk_reclaim();
    check_game();
    check_economy_game();
    check_mex_ground();
    check_mex_explore();
    check_plan();
    check_attention_eco();
    check_sea_plant();
    check_island_plant();
    check_island_plants();
    check_island_hold();
    check_island_order();
    check_sea_finish();
    check_finish_yards();
    check_sea_cmd_safe();
    check_techup();
    check_short_after_techup();
    check_helper_wants();
    check_frames_once();
    check_hold_wants();
    check_surplus_plants();
    check_surplus_energy();
    check_makers();
    check_steady_income();
    check_surplus_mex();
    check_yard_clear();
    check_yard_walk();
    check_way_to_one();
    check_yard_way();
    check_yard_off();
    check_ctech();
    check_ctech_mex();
    check_ctech_step();
    check_content_price();
    check_values();
    check_plant_choice();
    check_metal_reach();
    check_plant_gate();
    check_military();
    check_why();
    check_requests();
    check_reads_before_orders();
    free(s_scratch);
    printf("impai_brain_test: %s\n", s_fail ? "FAILED" : "ok");
    return s_fail ? 1 : 0;
}

