/* impai_build.c -- where ImpAI builds and how it orders it (impai.h, the brain): a metal patch for an
   extractor, the base ring for everything else, a cell to explore, a building to help. Every site is tested
   through the seat's own site test (WQ_SITE_SEAT) and every unit named by slot, the adapter re-deriving it
   before each command. Engine-free, no statics. */
#include <string.h>
#include "impai_brain.h"
#include "tagpu_simreg.h"

/* The sites and the build orders. OWNER: the brain calls them only inside a live ImpAI seat's turn, which
   plays no puppet seat (tagpu_impai.c, impai_play), so a puppet's orders come from the recording. */
SIM_FEATURE(impai_build, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval");

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

/* The builder's class's label at its own block, which a reach test of it compares: -1 for no test -- a VTOL
   builder, one with no class of the world's, a level with no labels for it -- 0 off the grid or on a block of
   no label, which reaches nothing. */
static int builder_label(const IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_TINFO* t = &d->in->ti[b->type];
    if (!d->in->reach || (t->flags & IMPAI_TF_VTOL)) return -1;
    return impai_reach_at(d->in->reach, t->mclass, b->pos[0] >> 21, b->pos[2] >> 21);
}

/* The reach labels of the builder's class at its block and at block (px, pz) agree, both nonzero; no test as
   builder_label says. */
static int reachable_block(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int px, int pz)
{
    const int l = builder_label(d, b);
    if (l < 0) return 1;
    return l && impai_reach_at(d->in->reach, d->in->ti[b->type].mclass, px, pz) == l;
}

/* the patch's anchor block is reachable on the builder's class */
static int reachable(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const SNAP_PATCH* p)
{
    return reachable_block(d, b, p->ax >> 1, p->az >> 1);
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

/* THE RING for energy, factories, makers, and extractors on a metal map (impai_econ.c, metal_map): the base box
   (or the commander's footprint at the anchor) grown on every side by gap + ring_grow x (fx + gap) cells, and
   place_ring's sites of fx + gap cells around it, each origin moved gap / 2 into its cell so that neighbours
   keep gap cells between them. In ring order a site is skipped when it overlaps a claim grown by gap, covers
   explored metal above the threshold (the patches are the extractors'; a metal map has none), or holds
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

/* ---- the sea plant ---- */

#define SEA_EST_BLOCKS 48       /* the estimate's water: sea_ref labels within this many blocks of it (1536 px) */
#define SEA_LABELS     8        /* of them, the most kept                                                       */
#define SEA_LOOK       64       /* the content candidates a decision tests for claims and failures              */

/* a refusal of the sea plant at first cell (cx, cz): the site avoided for fail_t, and one more refusal */
static void sea_refused(IMPAI_DEC* d, int cx, int cz)
{
    IMPAI_BRAIN* st = d->in->st;
    impai_fail_add(d, cx, cz);
    if (st->sea_refused < 255) st->sea_refused++;
}

/* THE SEA PLANT of `type` for builder b: 1 ordered, 0 not in this decision, -1 no site for it at all.
   THE ESTIMATE'S WATER: the distinct sea_ref labels within SEA_EST_BLOCKS of the military's estimate (any label
   with no estimate); none, and a sea army could not come near the enemy: -1. THE CANDIDATES: centre blocks in
   ring order (impai_ring_at) around the anchor's block, each within comm_r of the anchor so the commander may
   build any of them, the footprint's origin at an even cell (fx + 1) / 2 blocks across. A CONTENT candidate is
   one inside the site test's own bounds (cx >= 1, cz >= 1, cx + fx < W, cz + fz < H: 0x47D302..0x47D352), whose
   centre block carries one of the estimate's labels and whose every footprint block's top is at most sea -
   minwaterdepth. The first SEA_LOOK content candidates of the decision are then held against the TRANSIENT
   state -- the footprint grown by gap claimed, the footprint holding a failure -- and the site test decides the
   rest.
   THE DEPTH PREFILTER is exact enough to trust and still not the safety argument. The engine's site test
   0x47D2E0 keeps, over the footprint's yard cells, the land extremes from the cells' bytes +0x06 and +0x05
   (0x47D4F9..0x47D518: yard bit 0x08) and the water maximum from +0x05 (0x47D51C..0x47D52C: bit 0x10), and
   refuses when max(land maximum, water maximum) is above sea - minwaterdepth (0x47D7D7..0x47D7F0); a shipyard's
   yard is all `C` and `w`, bit 0x10 (0x42CFB2..0x42D03C), so that compare is the one that binds (DISASSEMBLED).
   The block tops are those same +0x05 bytes, the most over a block's four cells (tagpu_impai.c, cell_top), and
   the footprint's blocks cover its cells -- a superset when fx is odd -- so a candidate the filter passes also
   passes that compare. The site test, the seat's own (WQ_SITE_SEAT), decides everything.
   TERMINATION, by construction: -1 comes only from the content (no candidate) or from IMPAI_SEA_REFUSALS, 0 only
   from this decision's budgets (SEA_LOOK, site_tests, roots) or from transient exclusions, each of which ends --
   a failure after fail_t, a claim when its build ends -- and every retest succeeds or adds to the refusals. So
   the want ends in a sea plant or a land factory, at worst 16 x fail_t ticks on.
   BOUND, a call: at most (2 comm_r / 32 + 1)^2 ring blocks (4225 at comm_r 1024), each one label read, at most
   SEA_LABELS compares and fbx x fbz top reads; SEA_LOOK claim and failure tests a decision; site_tests site
   tests a decision; impai_sea_labels' (2 SEA_EST_BLOCKS + 1)^2 = 9409 label reads.
   NO BUILDER REACH TEST: a builder that cannot get to the site fails its job, which counts (impai_brain.c,
   job_checks). Testing it here would answer 0 to a builder that cannot reach while a candidate exists, and a
   seat whose only sea-plant builders cannot reach would hold its second factory for ever. */
int impai_build_sea_plant(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_REACH* rc = in->reach;
    IMPAI_BRAIN* st = in->st;
    const IMPAI_TINFO* t = &in->ti[type];
    const int fx = t->fx > 0 ? t->fx : 1, fz = t->fz > 0 ? t->fz : 1, fbx = (fx + 1) / 2, fbz = (fz + 1) / 2;
    const int gap = in->lv->gap, rings = in->lv->comm_r / 32;
    const int32_t apos[3] = { (int32_t)((uint32_t)(st->ax * 16 + 8) << 16), 0, (int32_t)((uint32_t)(st->az * 16 + 8) << 16) };
    uint16_t want[SEA_LABELS];
    WQ_DEF_ECON e;
    int nwant = -1, need, r, i, dx, dz, cands = 0;
    if (!rc || !rc->labels || !rc->top || rc->sea_ref >= WQ_MCLASSES || !(rc->present >> rc->sea_ref & 1u)) return -1;
    if (!d->q->ops->def_econ(d->q, type, &e) || e.minwater <= 0 || (need = (int)d->m.sea - e.minwater) < 0) return -1;
    if (in->mil && in->mil->ebx != IMPAI_NO_ANCHOR &&
        !(nwant = impai_sea_labels(rc, in->mil->ebx, in->mil->ebz, SEA_EST_BLOCKS, want, SEA_LABELS)))
        return -1;
    for (r = 0; r <= rings; r++)
        for (i = 0; impai_ring_at(r, i, &dx, &dz); i++) {
            const int bx = (st->ax >> 1) + dx, bz = (st->az >> 1) + dz, ox = bx - fbx / 2, oz = bz - fbz / 2;
            const int cx = 2 * ox, cz = 2 * oz;
            IMPAI_RECT site, wide;
            int label, k, j, deep = 1, alt = 0;
            if (impai_dist_px(apos, bx * 32 + 16, bz * 32 + 16) > in->lv->comm_r) continue;
            if (cx < 1 || cz < 1 || cx + fx >= d->m.w || cz + fz >= d->m.h || ox + fbx > rc->bw || oz + fbz > rc->bh)
                continue;
            if ((label = impai_reach_at(rc, rc->sea_ref, bx, bz)) <= 0) continue;
            for (k = 0; k < nwant && want[k] != label; k++) {}
            if (nwant >= 0 && k == nwant) continue;
            for (k = 0; k < fbz && deep; k++)
                for (j = 0; j < fbx && deep; j++) deep = rc->top[(size_t)(oz + k) * rc->bw + ox + j] <= need;
            if (!deep) continue;
            cands++;
            if (d->sea_looked >= SEA_LOOK) return 0;
            d->sea_looked++;
            site = wide = impai_rect_at(d, type, cx, cz);
            wide.x0 = (int16_t)(wide.x0 - gap); wide.z0 = (int16_t)(wide.z0 - gap);
            wide.x1 = (int16_t)(wide.x1 + gap); wide.z1 = (int16_t)(wide.z1 + gap);
            if (impai_claimed(d, &wide) || impai_failed_in(d, &site)) continue;
            if (d->sites >= in->lv->site_tests) return 0;
            d->sites++;
            if (d->q->ops->site_ok(d->q, type, cx, cz, 0, &alt)) {
                if (build_at(d, b, type, cx, cz, alt)) {
                    d->out->sea_plants++;
                    return 1;
                }
                /* the resolver refused, a refusal like the site test's; with the roots spent, nothing was asked */
                if (d->roots >= in->lv->roots) return 0;
            }
            sea_refused(d, cx, cz);
            if (st->sea_refused >= IMPAI_SEA_REFUSALS) return -1;
        }
    return cands ? 0 : -1;
}

/* ---- exploring ---- */

/* THE EXPLORE CELL, once a decision: of the 256-px cells, the one nearest the anchor (integer distance, ties
   in row-major order) whose centre cell the seat has not explored, that holds no failure and whose block b can
   travel to on its class (reachable_block: a sea constructor explores water, a land one its own island, so
   neither holds the seat's one explorer on a cell it never arrives at); b moves there (resolver mode 2), at the
   ground's height, and becomes the seat's one explorer. At most IMPAI_GRID_MAX^2 cells are looked at, one label
   read each. */
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
            if (d->q->ops->explored(d->q, cx, cz) || impai_failed_in(d, &r) || !reachable_block(d, b, cx >> 1, cz >> 1))
                continue;
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

/* A builder b can come near enough to help a building of `type` at (x, z) px: some block within R = min(its
   builddistance + half the building's larger side, NEAR_PX_MAX) px of the building's centre holds b's own
   component (impai_reach_near), so a sea constructor helps a shipyard and not a plant inland. No test as
   builder_label says. BOUND: at most (2 NEAR_PX_MAX / 32 + 1)^2 = 4225 label reads. */
#define NEAR_PX_MAX 1024
static int builder_near(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int32_t x, int32_t z)
{
    const int l = builder_label(d, b);
    WQ_DEF def;
    int32_t rpx;
    int f, ox, oz;
    if (l < 0) return 1;
    if (!l) return 0;
    f = type > 0 && (unsigned)type < d->in->nti ? (d->in->ti[type].fx > d->in->ti[type].fz ? d->in->ti[type].fx
                                                                                          : d->in->ti[type].fz) : 0;
    rpx = (d->q->ops->def(d->q, b->type, &def) ? def.reach : 0) + 8 * (f > 0 ? f : 0);
    if (rpx > NEAR_PX_MAX) rpx = NEAR_PX_MAX;
    return impai_reach_near(d->in->reach, d->in->ti[b->type].mclass, (uint16_t)l, x >> 5, z >> 5, rpx / 32, &ox, &oz);
}

/* THE ASSIST: the nearest unfinished building of the seat's, helped with resolver mode 8 -- which tests no
   alliance, so the census's own-units-only list is the test -- else the nearest finished factory of the
   seat's that is busy, guarded (mode 7), each one b can come near enough to (builder_near). The commander helps
   only within comm_r of the anchor. Ties go to the lower slot. BOUND: builder_near's reads for at most
   IMPAI_NANO_MAX + IMPAI_IDLE_CAP targets. */
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
            if (best && dist >= best_d) continue;
            if (!builder_near(d, b, list[i].type, list[i].pos[0] >> 16, list[i].pos[2] >> 16)) continue;
            best = &list[i];
            best_d = dist;
        }
        if (best) return order_mode(d, b, pass ? 7 : 8, best->slot, best->pos, IMPAI_JOB_ASSIST, 0, 0);
    }
    return 0;
}
