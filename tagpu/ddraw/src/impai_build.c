/* impai_build.c -- where ImpAI builds and how it orders it (impai.h, the brain): a metal patch for an
   extractor, the base ring for everything else, a cell to explore, a building to help. Every site is tested
   through the seat's own site test (WQ_SITE_SEAT) and every unit named by slot, the adapter re-deriving it
   before each command. Engine-free, no statics. */
#include <string.h>
#include "impai_brain.h"

/* ---- the order ---- */

/* A build of `type` at first cell (cx, cz), altitude `alt`: the resolver's build mode 14 for the builder at the
   footprint's centre, (2 origin + size) << 19 (tagpu_placegeo.h), then a NEW command without Shift, which
   replaces the builder's queue and keeps its standing orders (engine/orders.md). 1 when given; 0 when the
   resolver refuses (the site is then skipped as a refused site test) or the decision's commands are spent. */
static int build_at(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz, int alt)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* t = &in->ti[type];
    const IMPAI_RECT r = impai_rect_at(d, type, cx, cz);
    IMPAI_JOB* j = &in->job[b->slot];
    WQ_CMD c;
    int a;
    if (d->roots >= in->lv->roots) return 0;
    memset(&c, 0, sizeof c);
    c.pos[0] = (int32_t)((uint32_t)(2 * cx + t->fx) << 19);
    c.pos[1] = (int32_t)((uint32_t)(alt & 255) << 16);
    c.pos[2] = (int32_t)((uint32_t)(2 * cz + t->fz) << 19);
    if ((a = d->q->ops->can(d->q, b->slot, 14, (int)WQ_NONE, c.pos)) <= 0) return 0;
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)a;
    c.unit = b->slot;
    c.target = (uint16_t)WQ_NONE;
    c.type = (uint16_t)type;
    d->roots++;
    if (d->q->ops->order(d->q, &c) != WQ_OK) return 0;
    d->out->orders++;
    memset(j, 0, sizeof *j);
    j->type = b->type;
    j->build = (uint16_t)type;
    j->target = (uint16_t)WQ_NONE;
    j->cx = (int16_t)cx;
    j->cz = (int16_t)cz;
    j->kind = IMPAI_JOB_BUILD;
    j->since = d->now;
    impai_claim_add(d, type, &r);
    d->pending[t->cat < IMPAI_CATS ? t->cat : IMPAI_C_OTHER]++;
    return 1;
}

/* One order at a target or a point, resolver mode `mode`; the job it becomes. */
static int order_mode(IMPAI_DEC* d, const IMPAI_UNITREF* b, int mode, int target, const int32_t pos[3], int kind,
                      int cx, int cz)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_JOB* j = &in->job[b->slot];
    WQ_CMD c;
    int a;
    if (d->roots >= in->lv->roots) return 0;
    if ((a = d->q->ops->can(d->q, b->slot, mode, target, pos)) <= 0) return 0;
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)a;
    c.unit = b->slot;
    c.target = (uint16_t)target;
    memcpy(c.pos, pos, sizeof c.pos);
    d->roots++;
    if (d->q->ops->order(d->q, &c) != WQ_OK) return 0;
    d->out->orders++;
    memset(j, 0, sizeof *j);
    j->type = b->type;
    j->build = (uint16_t)WQ_NONE;
    j->target = (uint16_t)target;
    j->cx = (int16_t)cx;
    j->cz = (int16_t)cz;
    j->kind = (uint8_t)kind;
    j->since = d->now;
    return 1;
}

/* ---- a metal patch ---- */

/* the patch's box grown by a cell on every side */
static IMPAI_RECT patch_box(const SNAP_PATCH* p)
{
    IMPAI_RECT r;
    r.x0 = (int16_t)(p->x0 - 1); r.z0 = (int16_t)(p->z0 - 1);
    r.x1 = (int16_t)(p->x1 + 1); r.z1 = (int16_t)(p->z1 + 1);
    return r;
}

/* a patch no claim and no failure touches */
static int patch_free(const IMPAI_DEC* d, const SNAP_PATCH* p)
{
    const IMPAI_RECT r = patch_box(p);
    return !impai_claimed(d, &r) && !impai_failed_in(d, &r);
}

/* The reach labels of the builder's class at its block and at the patch's anchor block agree, both nonzero.
   A VTOL builder, one with no class of the world's, or a level with no labels for it skips the test. */
static int reachable(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const SNAP_PATCH* p)
{
    const IMPAI_REACH* rc = d->in->reach;
    const IMPAI_TINFO* t = &d->in->ti[b->type];
    const int bx = b->pos[0] >> 21, bz = b->pos[2] >> 21, px = p->ax >> 1, pz = p->az >> 1;
    const uint16_t* l;
    if (!rc || !rc->labels || (t->flags & IMPAI_TF_VTOL) || t->mclass >= WQ_MCLASSES || !(rc->present >> t->mclass & 1u))
        return 1;
    if (bx < 0 || bz < 0 || px < 0 || pz < 0 || bx >= rc->bw || bz >= rc->bh || px >= rc->bw || pz >= rc->bh) return 0;
    l = rc->labels + rc->at[t->mclass];
    return l[(size_t)bz * rc->bw + bx] && l[(size_t)bz * rc->bw + bx] == l[(size_t)pz * rc->bw + px];
}

/* ground threat at the 256-px cell of a point, from the seat's maps, built the first time a decision needs them */
static int threatened(IMPAI_DEC* d, int x_px, int z_px)
{
    const IMPAI_VIEW* v = d->in->view;
    int gx, gz;
    if (!d->maps_built) d->maps_built = v && v->threat && impai_maps_build(v, &d->maps) ? 1 : -1;
    if (d->maps_built < 0) return 0;
    gx = x_px / IMPAI_CELL_PX;
    gz = z_px / IMPAI_CELL_PX;
    return gx >= 0 && gz >= 0 && gx < d->maps.gw && gz < d->maps.gh &&
           d->maps.threat[IMPAI_THREAT_GROUND][gz * d->maps.gw + gx] > 0;
}

/* THE SPOT for builder b: of the seat's free patches -- for the commander within comm_r of the anchor, reachable
   on b's class, under no ground threat -- the one with the best cells x 4096 / (4096 + d), d the integer
   distance in px from b to the patch's centre, compared exactly in integers; ties to the lower (az, ax). NULL
   when b has none. */
const SNAP_PATCH* impai_spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    const SNAP_PATCH* best = NULL;
    uint32_t best_d = 0;
    unsigned i;
    for (i = 0; i < in->nspots; i++) {
        const SNAP_PATCH* p = &in->spots[i];
        const int px = (p->x0 + p->x1 + 1) * 8, pz = (p->z0 + p->z1 + 1) * 8;
        uint32_t dist;
        if (!p->cells || !patch_free(d, p)) continue;
        if (commander && impai_dist_px(apos, px, pz) > in->lv->comm_r) continue;
        if (!reachable(d, b, p) || threatened(d, px, pz)) continue;
        dist = impai_dist_px(b->pos, px, pz);
        if (best) {
            const uint64_t mine = (uint64_t)p->cells * (4096u + (uint64_t)best_d);
            const uint64_t theirs = (uint64_t)best->cells * (4096u + (uint64_t)dist);
            if (mine < theirs || (mine == theirs && (p->az > best->az || (p->az == best->az && p->ax >= best->ax))))
                continue;
        }
        best = p;
        best_d = dist;
    }
    return best;
}

/* The extractor on b's spot: the snap places it (tasim_snap_site, the build cursor's scorer: the footprint over
   the most explored metal within the patch's reach) and the site test decides. A patch the snap or the test
   refuses fails for fail_t. */
int impai_build_spot(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const SNAP_PATCH* best = impai_spot_pick(d, b);
    SNAP_CHOICE c;
    int32_t seed[3];
    int radius, alt = 0, w, h;
    uint32_t diag;
    if (!best || d->snaps >= in->lv->snaps) return 0;
    d->snaps++;
    w = best->x1 - best->x0 + 1;
    h = best->z1 - best->z0 + 1;
    diag = impai_isqrt((uint64_t)(w * w + h * h) * 256u);         /* px */
    radius = (int)((32u + diag / 2u + 15u) / 16u);
    if (radius > SNAP_RADIUS_MAX) radius = SNAP_RADIUS_MAX;
    seed[0] = (int32_t)((uint32_t)((best->x0 + best->x1 + 1) * 8) << 16);
    seed[1] = 0;
    seed[2] = (int32_t)((uint32_t)((best->z0 + best->z1 + 1) * 8) << 16);
    memset(&c, 0, sizeof c);
    if (!tasim_snap_site(d->q, NULL, type, seed, seed, radius, 0, 0, &c, in->snap)) {
        impai_fail_add(d, best->ax, best->az);
        return 0;
    }
    if (d->sites >= in->lv->site_tests) return 0;
    d->sites++;
    if (!d->q->ops->site_ok(d->q, type, c.x, c.z, 0, &alt)) {
        impai_fail_add(d, c.x, c.z);
        return 0;
    }
    return build_at(d, b, type, c.x, c.z, alt);
}

/* ---- the base ring ---- */

/* THE RING for energy, factories and makers: the base box (or the commander's footprint at the anchor) grown on
   every side by gap + ring_grow x (fx + gap) cells, and place_ring's sites of fx + gap cells around it, each
   origin moved gap / 2 into its cell so that neighbours keep gap cells between them. In ring order a site is
   skipped when it overlaps a claim grown by gap, covers explored metal (the metal is the extractors'), or holds
   a failure; the site test decides the rest, against the decision's site tests. A ring exhausted with tests
   left pushes the next decision's ring out (ring_grow, at most 8); a build brings it back to 0. */
int impai_build_ring(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_BRAIN* st = in->st;
    const IMPAI_TINFO* t = &in->ti[type];
    const int gap = in->lv->gap, fx = t->fx > 0 ? t->fx : 1, fz = t->fz > 0 ? t->fz : 1;
    const int grow = gap + st->ring_grow * (fx + gap);
    IMPAI_RECT box;
    unsigned n, k;
    if (d->has_bbox) box = d->bbox;
    else {
        const IMPAI_TINFO* ct = d->commander.slot ? &in->ti[d->commander.type] : NULL;
        box.x0 = st->ax;
        box.z0 = st->az;
        box.x1 = (int16_t)(st->ax + (ct && ct->fx > 0 ? ct->fx : 1) - 1);
        box.z1 = (int16_t)(st->az + (ct && ct->fz > 0 ? ct->fz : 1) - 1);
    }
    n = place_ring(fx + gap, fz + gap, box.x0 - grow, box.z0 - grow, box.x1 - box.x0 + 1 + 2 * grow,
                   box.z1 - box.z0 + 1 + 2 * grow, d->ring, PLACE_MAX_SITES);
    for (k = 0; k < n; k++) {
        const int cx = d->ring[k].cx + gap / 2, cz = d->ring[k].cz + gap / 2;
        IMPAI_RECT r = impai_rect_at(d, type, cx, cz), wide = r;
        int alt = 0;
        wide.x0 = (int16_t)(wide.x0 - gap); wide.z0 = (int16_t)(wide.z0 - gap);
        wide.x1 = (int16_t)(wide.x1 + gap); wide.z1 = (int16_t)(wide.z1 + gap);
        if (impai_claimed(d, &wide) || impai_failed_in(d, &r) || d->q->ops->coverage(d->q, cx, cz, fx, fz) > 0) continue;
        if (d->sites >= in->lv->site_tests) return 0;
        d->sites++;
        if (!d->q->ops->site_ok(d->q, type, cx, cz, 0, &alt)) continue;
        if (build_at(d, b, type, cx, cz, alt)) {
            st->ring_grow = 0;
            return 1;
        }
        if (d->roots >= in->lv->roots) return 0;
    }
    if (st->ring_grow < 8) st->ring_grow++;
    return 0;
}

/* ---- exploring ---- */

/* THE EXPLORE CELL, once a decision: of the 256-px cells, the one nearest the anchor (integer distance, ties
   in row-major order) whose centre cell the seat has not explored and that holds no failure; b moves there
   (resolver mode 2), at the ground's height, and becomes the seat's one explorer. At most IMPAI_GRID_MAX^2
   cells are looked at. */
int impai_build_explore(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int gw = (d->m.w * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX, gh = (d->m.h * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX;
    const int per = IMPAI_CELL_PX / 16;
    int gx, gz, bx = -1, bz = -1;
    uint64_t best = 0;
    int32_t pos[3];
    if (d->explored_once || gw < 1 || gh < 1 || gw > IMPAI_GRID_MAX || gh > IMPAI_GRID_MAX) return 0;
    d->explored_once = 1;
    for (gz = 0; gz < gh; gz++)
        for (gx = 0; gx < gw; gx++) {
            int cx = gx * per + per / 2, cz = gz * per + per / 2;
            IMPAI_RECT r;
            int64_t dx, dz;
            uint64_t dd;
            if (cx >= d->m.w) cx = d->m.w - 1;
            if (cz >= d->m.h) cz = d->m.h - 1;
            r.x0 = r.x1 = (int16_t)cx;
            r.z0 = r.z1 = (int16_t)cz;
            if (d->q->ops->explored(d->q, cx, cz) || impai_failed_in(d, &r)) continue;
            dx = (int64_t)cx - in->st->ax;
            dz = (int64_t)cz - in->st->az;
            dd = (uint64_t)(dx * dx + dz * dz);
            if (bx < 0 || dd < best) { best = dd; bx = cx; bz = cz; }
        }
    if (bx < 0) return 0;
    pos[0] = (int32_t)((uint32_t)(bx * 16 + 8) << 16);
    pos[1] = 0;
    pos[2] = (int32_t)((uint32_t)(bz * 16 + 8) << 16);
    pos[1] = (int32_t)((uint32_t)(d->q->ops->ground(d->q, pos) & 0x7FFF) << 16);
    if (!order_mode(d, b, 2, (int)WQ_NONE, pos, IMPAI_JOB_EXPLORE, bx, bz)) return 0;
    in->st->explorer = b->slot;
    d->explorer_live = 1;
    return 1;
}

/* ---- assisting ---- */

/* THE ASSIST: the nearest unfinished building of the seat's, helped with resolver mode 8 -- which tests no
   alliance, so the census's own-units-only list is the test -- else the nearest finished factory of the
   seat's that is busy, guarded (mode 7). The commander helps only within comm_r of the anchor. Ties go to the
   lower slot. */
int impai_build_assist(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    const IMPAI_UNITREF* best = NULL;
    uint32_t best_d = 0;
    int i, pass;
    for (pass = 0; pass < 2 && !best; pass++) {
        const IMPAI_UNITREF* list = pass ? d->fbusy : d->nano;
        const int n = pass ? d->nfbusy : d->nnano;
        for (i = 0; i < n; i++) {
            const uint32_t dist = impai_dist_px(b->pos, list[i].pos[0] >> 16, list[i].pos[2] >> 16);
            if (list[i].slot == b->slot) continue;
            if (commander && impai_dist_px(apos, list[i].pos[0] >> 16, list[i].pos[2] >> 16) > in->lv->comm_r) continue;
            if (!best || dist < best_d) { best = &list[i]; best_d = dist; }
        }
        if (best) return order_mode(d, b, pass ? 7 : 8, best->slot, best->pos, IMPAI_JOB_ASSIST, 0, 0);
    }
    return 0;
}
