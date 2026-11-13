/* tasim_worldq.c -- worldq.h over the shared simulation core (tasim_worldq.h). research/notes/impai/shared-core.md,
   tasim_worldq. Each op reads or changes the TASIM its open was given and nothing else; no globals. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tasim.h"
#include "tasim_worldq.h"

static WQ_CORE* ctx_of(const WORLDQ* q) { return (WQ_CORE*)q->self; }

/* the entry's type of a WORLDQ type, -1 for none */
static int entry_type(const TASIM_DEFS* defs, int type) { return type >= 1 && type <= defs->entry->ndef ? type - 1 : -1; }

static WIKIUNIT* any_unit(const TASIM* s, int slot) { return slot >= 1 && slot <= s->nunit ? s->unit[slot - 1] : NULL; }

static WIKIUNIT* own_unit(const WORLDQ* q, int slot)
{
    WIKIUNIT* u = any_unit(ctx_of(q)->s, slot);
    return u && u->owner == q->seat ? u : NULL;
}

/* the slot of the unit of id `id`, WQ_NONE for none */
static int slot_of(const TASIM* s, const char* id)
{
    int i;
    for (i = 0; i < s->nunit; i++)
        if (!strcmp(s->unit[i]->id, id)) return i + 1;
    return (int)WQ_NONE;
}

static int byte_of(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : (int)v; }

static int explored(const TASIM* s, int cx, int cz)
{
    return cx >= 0 && cz >= 0 && cx < s->W && cz < s->H && s->fog[cz * s->W + cx] != 2;
}

static int metal(const TASIM* s, int cx, int cz) { return explored(s, cx, cz) ? byte_of(s->metal[cz * s->W + cx]) : -1; }

/* a footprint's first cell for a 16.16 centre: 0x4197D0's rounding, an arithmetic shift as its `sar` */
static int origin(int32_t centre, int foot) { return (int)((unsigned)centre - ((unsigned)foot << 19) + 0x80000u) >> 20; }

/* where an order stands in 16.16, as the engine's node holds it: a build at its footprint's centre, every
   other order at the point it was given (a feature's reclaim at the feature's centre) */
static void order_pos(const TASIM_DEFS* defs, const WIKIORDER* o, int32_t pos[3])
{
    if (o->k == WIKI_ORDER_BUILD) {
        pos[0] = (int32_t)((unsigned)(2 * o->ox + tasim_fpx(defs, o->type)) << 19);
        pos[2] = (int32_t)((unsigned)(2 * o->oz + tasim_fpz(defs, o->type)) << 19);
    } else {
        pos[0] = tasim_fix16(o->x);
        pos[2] = tasim_fix16(o->z);
    }
    pos[1] = 0;
}

static int action_of(const WIKIORDER* o)
{
    return o->k == WIKI_ORDER_MOVE ? CORE_ACT_MOVE : o->k == WIKI_ORDER_BUILD ? CORE_ACT_BUILD
         : o->k == WIKI_ORDER_RECLAIM ? CORE_ACT_RECLAIM : CORE_ACT_GUARD;
}

static void copy_order(const TASIM* s, const TASIM_DEFS* defs, const WIKIORDER* o, int place, WQ_ORDER* out)
{
    memset(out, 0, sizeof *out);
    out->place = (uint16_t)place;
    out->action = (uint8_t)action_of(o);
    out->target = (uint16_t)WQ_NONE;
    order_pos(defs, o, out->pos);
    if (o->k == WIKI_ORDER_BUILD) {
        out->build = 1;
        out->type = (uint16_t)(o->type + 1);
        out->started = out->owned = (uint8_t)tasim_started(s, defs, o);
    } else if ((o->k == WIKI_ORDER_RECLAIM && !o->feat) || o->k == WIKI_ORDER_GUARD)
        out->target = (uint16_t)slot_of(s, o->target);
}

/* the vent at cell (cx, cz), -1 for none */
static int vent_at(const TASIM* s, int cx, int cz)
{
    int i;
    for (i = 0; i < s->nvent; i++)
        if (s->vent[i].cx == cx && s->vent[i].cz == cz) return i;
    return -1;
}

/* ---- the ops ---- */

static int wq_map(const WORLDQ* q, WQ_MAP* out)
{
    const TASIM* s = ctx_of(q)->s;
    memset(out, 0, sizeof *out);
    out->w = s->W;
    out->h = s->H;
    out->threshold = byte_of(s->SM);
    out->list_cap = PLACE_LIST_CAP;     /* tasim_push_order's */
    return 1;
}

static int wq_explored(const WORLDQ* q, int cx, int cz) { return explored(ctx_of(q)->s, cx, cz); }
static int wq_metal(const WORLDQ* q, int cx, int cz) { return metal(ctx_of(q)->s, cx, cz); }

/* Inside the map, four reads of the sums init builds (TASIM.rich, tagpu_wikisim.c rich_sums): Quick Build
   asks it for every candidate of the snap search round every seat's cursor, and read cell by cell it was
   55 % of a players replay's time (MEASURED 2026-11-02, gprof over the five players demos, native -O2). The
   sums count the cells whose scene value is above SurfaceMetal, which for the whole values the scenes hold
   is the same test as the bytes' here. A footprint past an edge is read cell by cell. */
static int wq_coverage(const WORLDQ* q, int cx, int cz, int fx, int fz)
{
    const TASIM* s = ctx_of(q)->s;
    int i, j, n = 0, threshold = byte_of(s->SM);
    if (cx >= 0 && cz >= 0 && fx >= 0 && fz >= 0 && cx + fx <= s->W && cz + fz <= s->H) {
        const int* R = s->rich;
        const int w = s->W + 1;
        return R[(cz + fz) * w + cx + fx] - R[cz * w + cx + fx] - R[(cz + fz) * w + cx] + R[cz * w + cx];
    }
    for (j = cz; j < cz + fz; j++)
        for (i = cx; i < cx + fx; i++)
            if (metal(s, i, j) > threshold) n++;
    return n;
}

/* the stage's mapped test is its explored map at the point's cell */
static int wq_mapped_point(const WORLDQ* q, const int32_t pos[3])
{
    return explored(ctx_of(q)->s, pos[0] >> 20, pos[2] >> 20);
}

static int wq_ground(const WORLDQ* q, const int32_t pos[3]) { (void)q; (void)pos; return 0; }

static int wq_ndefs(const WORLDQ* q) { return ctx_of(q)->defs->entry->ndef + 1; }

/* A builder is a type whose build list names something (every type a menu would offer a building to is one
   [INFERRED]: the casts that build all have lists); a helper, the engine's acceptance 0x4899B0's rule, a
   builder that can reclaim with a worker time (research/notes/impai/decisions.md, scout H); no cast flies. A
   mobile type is no resource building, as a type with a move class is none in the engine. */
static int wq_def(const WORLDQ* q, int type, WQ_DEF* out)
{
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    const int t = entry_type(defs, type);
    const WIKIDEF* d;
    double v;
    memset(out, 0, sizeof *out);
    if (t < 0) return 0;
    d = tasim_def(defs, t);
    out->type = (uint16_t)type;
    out->fx = (int16_t)d->fx;
    out->fz = (int16_t)d->fz;
    out->kind = (uint8_t)(tasim_mobile(defs, t) ? WQ_KIND_NONE : tasim_kind(defs, t));
    out->yard = 1;
    out->workertime = (uint16_t)(d->worker_time <= 0 ? 0 : d->worker_time >= 65535 ? 65535 : d->worker_time);
    out->reach = (uint16_t)(d->build_distance <= 0 ? 0 : d->build_distance >= 65535 ? 65535 : d->build_distance);
    out->speed = tasim_fix16(d->max_velocity);
    v = out->kind == WQ_KIND_MEX ? d->extracts : d->energy_make;
    out->strength = out->kind && v > 0 ? (float)v : 0;
    if (d->builds) out->flags |= WQ_DF_BUILDER;
    if (d->builds && d->canreclamate && d->worker_time > 0) out->flags |= WQ_DF_ASSIST;
    if (d->canreclamate) out->flags |= WQ_DF_RECLAIMER;
    if (tasim_mobile(defs, t)) out->flags |= WQ_DF_MOVER;
    snprintf(out->name, sizeof out->name, "%s", d->name);
    out->mclass = tasim_mobile(defs, t) ? 0 : WQ_MCLASS_NONE;     /* the stage's one class (wq_passable) */
    return 1;
}

/* The economy keys the entry's types carry: costs, build time, energy and metal made, extraction, storage. The
   core has no energy use, wind, tide or metal maker, and no water, so those read 0. */
static float econ_key(double v) { return v > 0 && v < 3.0e38 ? (float)v : 0; }

static int wq_def_econ(const WORLDQ* q, int type, WQ_DEF_ECON* out)
{
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    const int t = entry_type(defs, type);
    const WIKIDEF* d;
    memset(out, 0, sizeof *out);
    if (t < 0) return 0;
    d = tasim_def(defs, t);
    out->cost_m = econ_key(d->build_cost_metal);
    out->cost_e = econ_key(d->build_cost_energy);
    out->buildtime = d->build_time > 0 && d->build_time < 4294967295.0 ? (uint32_t)d->build_time : 0;
    out->energymake = econ_key(d->energy_make);
    out->metalmake = econ_key(d->metal_make);
    out->extracts = econ_key(d->extracts);
    out->estore = econ_key(d->energy_storage);
    out->mstore = econ_key(d->metal_storage);
    out->building = !tasim_mobile(defs, t);
    return 1;
}

/* the yard the engine gives a building with no YardMap key, 0x2F a cell, and 0x8F under a G cell */
static int wq_yard(const WORLDQ* q, int type, uint8_t* out, int cap)
{
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    const int t = entry_type(defs, type);
    int fx, fz, dx, dz;
    if (t < 0) return 0;
    fx = tasim_fpx(defs, t);
    fz = tasim_fpz(defs, t);
    if (fx < 1 || fz < 1 || fx * fz > cap) return 0;
    for (dz = 0; dz < fz; dz++)
        for (dx = 0; dx < fx; dx++) out[dz * fx + dx] = tasim_g_cell(defs, t, dx, dz) ? 0x8F : 0x2F;
    return fx * fz;
}

/* the build list, in definition order */
static int wq_build_options(const WORLDQ* q, int builder_type, uint16_t* out, int cap)
{
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    const int b = entry_type(defs, builder_type);
    int t, n = 0;
    if (b < 0) return 0;
    for (t = 0; t < defs->entry->ndef; t++)
        if ((tasim_def(defs, b)->builds >> t) & 1) {
            if (n >= cap) return -1;
            out[n++] = (uint16_t)(t + 1);
        }
    return n;
}

/* The core's site test, with the engine's refusal of an unmapped centre (0x47D2E0 with a player, which every
   caller passes): the footprint's centre cell explored, then tasim_site_ok with `ignore_slot`'s unit gone. */
static int wq_site_ok(const WORLDQ* q, int type, int cx, int cz, int ignore_slot, int* alt)
{
    const TASIM* s = ctx_of(q)->s;
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    const int t = entry_type(defs, type);
    const WIKIUNIT* ignore = ignore_slot ? any_unit(s, ignore_slot) : NULL;
    if (t < 0 || (ignore_slot && !ignore) || cx < -CELL_BOUND || cx > CELL_BOUND || cz < -CELL_BOUND || cz > CELL_BOUND)
        return 0;
    if (alt) *alt = 0;
    return explored(s, cx + tasim_fpx(defs, t) / 2, cz + tasim_fpz(defs, t) / 2) && tasim_site_ok(s, defs, t, cx, cz, ignore);
}

static int wq_blocked_alt(const WORLDQ* q, int type, int cx, int cz) { (void)q; (void)type; (void)cx; (void)cz; return 0; }

/* A vent is a one-cell geothermal feature; any other cell answers the feature standing on it. */
static int wq_feature(const WORLDQ* q, int cx, int cz, WQ_FEAT* out)
{
    const TASIM* s = ctx_of(q)->s;
    const WIKIFEAT* f;
    int i;
    memset(out, 0, sizeof *out);
    if (cx < 0 || cz < 0 || cx >= s->W || cz >= s->H) return 0;
    if (vent_at(s, cx, cz) >= 0) {
        out->ax = cx;
        out->az = cz;
        out->def = (uint16_t)WQ_NONE;
        out->fx = out->fz = 1;
        out->flags = 0x20;
        return 1;
    }
    for (i = 0; i < s->nfeat; i++) {
        f = &s->feat[i];
        if (cx >= f->ox && cx < f->ox + f->fx && cz >= f->oz && cz < f->oz + f->fz) {
            out->ax = f->ox;
            out->az = f->oz;
            out->def = (uint16_t)f->def;
            out->fx = (int16_t)(f->fx < 1 ? 1 : f->fx > 16 ? 16 : f->fx);
            out->fz = (int16_t)(f->fz < 1 ? 1 : f->fz > 16 ? 16 : f->fz);
            out->flags = (uint8_t)((f->blocking ? 0x40 : 0) | (f->reclaimable ? 0x80 : 0));
            out->metal = (float)f->metal;
            out->energy = (float)f->energy;
            return 1;
        }
    }
    return 0;
}

static int wq_nslots(const WORLDQ* q) { return ctx_of(q)->s->nunit + 1; }

static int wq_seat_units(const WORLDQ* q, int* first, int* last)
{
    *first = 1;
    *last = ctx_of(q)->s->nunit;
    return 1;
}

static int wq_unit(const WORLDQ* q, int slot, WQ_UNIT* out)
{
    const WIKIUNIT* u = own_unit(q, slot);
    memset(out, 0, sizeof *out);
    if (!u) return 0;
    out->slot = out->index = (uint16_t)slot;
    out->type = (uint16_t)(u->type + 1);
    out->owner = (uint8_t)u->owner;
    out->finished = u->built >= 1;
    out->pos[0] = tasim_fix16(u->x);
    out->pos[2] = tasim_fix16(u->z);
    out->state = 0x10000000u | (u->sel ? 0x10u : 0);
    return 1;
}

static int wq_orders(const WORLDQ* q, int slot, WQ_ORDER* out, int cap)
{
    const TASIM* s = ctx_of(q)->s;
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    const WIKIUNIT* u = own_unit(q, slot);
    int j;
    if (!u) return -1;
    for (j = 0; j < u->norder && j < cap; j++) copy_order(s, defs, &u->order[j], j, &out[j]);
    return j;
}

static int wq_head(const WORLDQ* q, int slot, WQ_ORDER* out)
{
    memset(out, 0, sizeof *out);
    return wq_orders(q, slot, out, 1);
}

/* The resolver's two modes the rules ask. Reclaim (12): a unit that can reclaim, of a live unit other than
   itself, or with no target of a reclaimable feature at the point on explored ground. Guard (7): another live
   unit of the seat's. */
static int wq_can(const WORLDQ* q, int slot, int mode, int target_slot, const int32_t pos[3])
{
    const TASIM* s = ctx_of(q)->s;
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    const WIKIUNIT* u = own_unit(q, slot);
    const WIKIUNIT* t = target_slot != (int)WQ_NONE ? any_unit(s, target_slot) : NULL;
    if (!u || (target_slot != (int)WQ_NONE && !t)) return -1;
    if (mode == WQ_CAN_RECLAIM) {
        const WIKIFEAT* f;
        if (!tasim_def(defs, u->type)->canreclamate) return -1;
        if (t) return t != u ? CORE_ACT_RECLAIM : -1;
        f = tasim_feat_at(s, pos[0] / 65536.0, pos[2] / 65536.0);
        return f && f->reclaimable && explored(s, pos[0] >> 20, pos[2] >> 20) ? CORE_ACT_RECLAIM : -1;
    }
    if (mode == WQ_CAN_GUARD) return t && t != u && t->owner == q->seat ? CORE_ACT_GUARD : -1;
    return -1;
}

static int wq_action(const WORLDQ* q, int act)
{
    static const int BYTE[WQ_ACTS] = { CORE_ACT_BUILD, CORE_ACT_VTOL_BUILD, CORE_ACT_RECLAIM, CORE_ACT_VTOL_RECLAIM,
                                       CORE_ACT_RESURRECT, -1, -1 };
    (void)q;
    return act >= 0 && act < WQ_ACTS ? BYTE[act] : -1;
}

/* the order a command gives, 0 when it names nothing the core can order: a build of a type with its first
   cell from the position, a reclaim of the target unit or of the feature at the point, a guard of the target
   unit, a move. A unit-targeted order keeps the command's point, as the engine's node does. */
static int make_order(const TASIM* s, const TASIM_DEFS* defs, const WQ_CMD* cmd, const WIKIUNIT* target, WIKIORDER* o)
{
    const WIKIFEAT* f;
    int t;
    memset(o, 0, sizeof *o);
    switch (cmd->action) {
    case CORE_ACT_BUILD:
        if ((t = entry_type(defs, cmd->type)) < 0) return 0;
        *o = tasim_order_build(t, origin(cmd->pos[0], tasim_fpx(defs, t)), origin(cmd->pos[2], tasim_fpz(defs, t)));
        return 1;
    case CORE_ACT_RECLAIM:
        if (target) *o = tasim_order_target(WIKI_ORDER_RECLAIM, target->id);
        else if ((f = tasim_feat_at(s, cmd->pos[0] / 65536.0, cmd->pos[2] / 65536.0)) != NULL) {
            o->k = WIKI_ORDER_RECLAIM;
            o->feat = 1;
            snprintf(o->target, sizeof o->target, "%s", f->id);
            o->x = f->x;
            o->z = f->z;
            return 1;
        } else return 0;
        break;
    case CORE_ACT_GUARD:
        if (!target) return 0;
        *o = tasim_order_target(WIKI_ORDER_GUARD, target->id);
        break;
    case CORE_ACT_MOVE:
        o->k = WIKI_ORDER_MOVE;
        break;
    default:
        return 0;
    }
    o->x = cmd->pos[0] / 65536.0;
    o->z = cmd->pos[2] / 65536.0;
    return 1;
}

/* NEW is 0x43ADC0's: without Shift the list is replaced (no core order survives a replace), then the order is
   appended. CLICK is 0x43AFC0's: with Shift the first order of the same action within 16 world units on both
   axes (place_within) is taken off instead of one added. REMOVE takes off the order at `expect`'s place when it
   is still the one `orders` copied. Every append is tasim_push_order's, bounded by PLACE_LIST_CAP. The core
   keeps no factory queue, so QUEUE is refused. */
static int wq_order(const WORLDQ* q, const WQ_CMD* cmd)
{
    TASIM* s = ctx_of(q)->s;
    const TASIM_DEFS* defs = ctx_of(q)->defs;
    WIKIUNIT* u = own_unit(q, cmd->unit);
    const WIKIUNIT* target = cmd->target != WQ_NONE ? any_unit(s, cmd->target) : NULL;
    WIKIORDER o;
    WQ_ORDER have;
    int j;
    if (!u || (cmd->target != WQ_NONE && !target)) return WQ_REFUSED;
    if (cmd->verb == WQ_CMD_REMOVE) {
        const WQ_ORDER* e = &cmd->expect;
        if (e->place >= u->norder) return WQ_REFUSED;
        copy_order(s, defs, &u->order[e->place], e->place, &have);
        if (have.action != e->action || have.pos[0] != e->pos[0] || have.pos[2] != e->pos[2] || have.flags != e->flags)
            return WQ_REFUSED;
        tasim_splice_orders(u, e->place, 1);
        return WQ_OK;
    }
    if ((cmd->verb != WQ_CMD_NEW && cmd->verb != WQ_CMD_CLICK) || !make_order(s, defs, cmd, target, &o)) return WQ_REFUSED;
    if (cmd->verb == WQ_CMD_CLICK && cmd->shift)
        for (j = 0; j < u->norder; j++) {
            copy_order(s, defs, &u->order[j], j, &have);
            if (have.action == cmd->action && place_within(cmd->pos[0], cmd->pos[2], have.pos[0], have.pos[2])) {
                tasim_splice_orders(u, j, 1);
                return WQ_OK;
            }
        }
    tasim_assign(s, u, &o, 1, cmd->shift);
    return WQ_OK;
}

static int wq_economy(const WORLDQ* q, WQ_ECON* out)
{
    const TASIM* s = ctx_of(q)->s;
    memset(out, 0, sizeof *out);
    if (!s->game) return WQ_UNSUPPORTED;
    out->metal = (float)s->econ[q->seat].m;
    out->energy = (float)s->econ[q->seat].e;
    out->metal_max = (float)s->econ[q->seat].mmax;
    out->energy_max = (float)s->econ[q->seat].emax;
    return WQ_OK;
}

static int wq_gametime(const WORLDQ* q) { return ctx_of(q)->s->tick; }

/* ---- what a seat sees, from the sight the caller computed (tasim_sight.c) into the open's lit and con ---- */

static int asr5(int v) { return v >> 5; }

/* the 32-px block of a unit's centre, unsheared at the stage's altitude 0, as tagpu_wikisim_contacts reads it */
static void block_at(const TASIM* s, const WIKIUNIT* u, int* bx, int* bz)
{
    *bx = asr5((int)floor(u->x));
    *bz = asr5((int)floor(u->z));
    (void)s;
}

static int lit_at(const WQ_CORE* c, int bx, int bz)
{
    const int gw = c->s->W >> 1, gh = c->s->H >> 1;
    return c->lit && bx >= 0 && bz >= 0 && bx < gw && bz < gh && c->lit[bz * gw + bx] != 0;
}

/* The stage shares nothing between seats, has no sea and nothing flies, so a unit is OWN, LOS when its block is
   lit, or the CONTACT the caller vouches for: the engine's classes as they reduce on it. ITS INCARNATION IS ITS
   SLOT: the core's radar pass completes in the call that starts it (radar_step), so its records and the
   publish that follows in the same tick see the same units, and no slot changes hands between the two. */
static int wq_observe(const WORLDQ* q, int slot, int contact, WQ_OBS* out)
{
    const WQ_CORE* c = ctx_of(q);
    const WIKIUNIT* u = any_unit(c->s, slot);
    const int gw = c->s->W >> 1, gh = c->s->H >> 1;
    int bx, bz;
    memset(out, 0, sizeof *out);
    if (!u || u->owner < 0 || u->owner >= WIKI_SEATS || gw < 1 || gh < 1) return 0;
    block_at(c->s, u, &bx, &bz);
    if (u->owner == q->seat) out->seen = WQ_SEEN_OWN;
    else if (lit_at(c, bx, bz)) out->seen = WQ_SEEN_LOS;
    else if (contact) out->seen = WQ_SEEN_CONTACT;
    else return 0;
    out->owner = out->seen == WQ_SEEN_CONTACT ? 0xFF : (uint8_t)u->owner;
    out->type = out->seen == WQ_SEEN_CONTACT ? (uint16_t)WQ_NONE : (uint16_t)(u->type + 1);
    out->bx = (uint16_t)(bx < 0 ? 0 : bx >= gw ? gw - 1 : bx);
    out->bz = (uint16_t)(bz < 0 ? 0 : bz >= gh ? gh - 1 : bz);
    out->layer = WQ_LAYER_SURFACE;
    out->birth_known = 1;
    out->birth = (uint32_t)slot;
    return out->seen;
}

static int wq_block_lit(const WORLDQ* q, int bx, int bz) { return lit_at(ctx_of(q), bx, bz); }

/* The whole pass at once from the caller's contacts (tagpu_wikisim_contacts, one byte a unit in slot order):
   the seat's own units kept, as loop 1 keeps them, and every contact a radar and sonar one, the stage's
   altitude 0 being the sea level, where both tests hold together. One visit a unit; each bit set writes the
   slot's record, its incarnation the slot (wq_observe). */
static int wq_radar_step(const WORLDQ* q, WQ_RADAR* st, uint8_t* bits, int nbytes, const WQ_HITS* hits,
                         int32_t* budget)
{
    const WQ_CORE* c = ctx_of(q);
    const int n = c->s->nunit + 1;
    const int gw = c->s->W >> 1, gh = c->s->H >> 1;
    int slot, bx, bz;
    if (nbytes < (int)WQ_RADAR_BYTES(n) || hits->n < n || hits->stride < sizeof(WQ_RADAR_HIT)) return 0;
    memset(bits, 0, WQ_RADAR_BYTES(n));
    for (slot = 1; slot < n; slot++) {
        const WIKIUNIT* u = c->s->unit[slot - 1];
        WQ_RADAR_HIT* h = wq_hit(hits, (unsigned)slot);
        (*budget)--;
        if (u->owner == q->seat) wq_radar_set(bits, (unsigned)slot, WQ_RB_RADAR | WQ_RB_SONAR | WQ_RB_KEPT);
        else if (c->con && c->con[slot - 1]) wq_radar_set(bits, (unsigned)slot, WQ_RB_RADAR | WQ_RB_SONAR);
        else continue;
        block_at(c->s, u, &bx, &bz);
        h->birth = (uint32_t)slot;
        h->bx = (uint16_t)(bx < 0 ? 0 : bx >= gw ? (gw > 0 ? gw - 1 : 0) : bx);
        h->bz = (uint16_t)(bz < 0 ? 0 : bz >= gh ? (gh > 0 ? gh - 1 : 0) : bz);
        h->known = 1;
        if (hits->stamp) h->at = (uint32_t)c->s->tick;
    }
    st->stage = WQ_RS_IDLE;
    st->cursor = 0;
    st->start = (uint32_t)c->s->tick;
    return 1;
}

/* The stage is flat and dry: its one class, 0, is good on every cell of the map. */
static int wq_passable(const WORLDQ* q, int mclass, int cx, int cz)
{
    const TASIM* s = ctx_of(q)->s;
    return mclass == 0 && cx >= 0 && cz >= 0 && cx < s->W && cz < s->H ? 3 : 0;
}

/* The stage has no start positions. */
static int wq_starts(const WORLDQ* q, WQ_START* out, int cap)
{
    (void)q; (void)out; (void)cap;
    return WQ_UNSUPPORTED;
}

static const WORLDQ_OPS OPS = {
    wq_map, wq_explored, wq_metal, wq_coverage, wq_mapped_point, wq_ground, wq_ndefs, wq_def, wq_def_econ, wq_yard,
    wq_build_options, wq_site_ok, wq_blocked_alt, wq_feature, wq_nslots, wq_seat_units, wq_unit, wq_orders, wq_head,
    wq_can, wq_action, wq_order, wq_economy, wq_gametime, wq_observe, wq_block_lit, wq_radar_step, wq_passable,
    wq_starts,
};

int tasim_worldq_open(WQ_CORE* ctx, WORLDQ* out, TASIM* s, const TASIM_DEFS* defs, int seat)
{
    memset(out, 0, sizeof *out);
    if (seat < 0 || seat >= WIKI_SEATS) return 0;
    ctx->s = s;
    ctx->defs = defs;
    ctx->seat = seat;
    ctx->lit = ctx->con = NULL;
    out->ops = &OPS;
    out->self = ctx;
    out->seat = seat;
    return 1;
}
