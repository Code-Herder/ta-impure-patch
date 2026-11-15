/* tasim_sweep.c -- the reclaim sweep's rules over WORLDQ (tasim_sweep.h). research/notes/modules/reclaim-sweep.md
   states them; tagpu_sweep.c holds the gesture, the popup, the cursor and the click's tail. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_sweep.h"
#include "tagpu_simreg.h"

/* The reclaim rules, which the sweep UI (tagpu_sweep.c) and ImpAI's reclaim (impai_build.c, THE RECLAIM) order
   by. OWNER: their orders are given only on a live seat, a click's or ImpAI's turn, the WORLDQ refusing to
   command a puppet seat, and a puppet's come from its recording. */
SIM_FEATURE(sweep_rules, SIM_PUPPET_OWNER, "sweep-basic stock-sweep-basic impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval");

/* the kind: a NaN amount compares false, so it counts as none */
int tasim_sweep_kind_ok(float metal, float energy, unsigned kind)
{
    if (kind == SWEEP_KIND_METAL) return metal > 0.0f;
    if (kind == SWEEP_KIND_ENERGY) return energy > 0.0f;
    return 1;
}

/* A reclaimer: one of the seat's selected live units (+0x110 bit 4) whose type can reclaim (+0x245 bit
   0x400, the resolver's first test, 0x43F4F7). */
unsigned tasim_sweep_reclaimers(const WORLDQ* q, const SWEEP_IN* in, SWEEP_SCRATCH* scr)
{
    int i;
    scr->nb = 0;
    for (i = 0; i < in->nselected && scr->nb < SWEEP_BUILDERS_MAX; i++) {
        WQ_UNIT u;
        WQ_DEF d;
        if (q->ops->unit(q, in->selected[i], &u) && (u.state & 0x10u) && u.type && q->ops->def(q, u.type, &d) &&
            (d.flags & WQ_DF_RECLAIMER))
            scr->slot[scr->nb++] = in->selected[i];
    }
    return scr->nb;
}

/* the order bytes of the reclaim family, by the engine's own names RECLAIM, VTOL_RECLAIM and RESURRECT, the
   ones the resolver gives for a feature (0x440187, 0x43F694, 0x44004C) */
int tasim_sweep_actions(const WORLDQ* q, SWEEP_SCRATCH* scr)
{
    const int a = q->ops->action(q, WQ_ACT_RECLAIM), v = q->ops->action(q, WQ_ACT_VTOL_RECLAIM),
              r = q->ops->action(q, WQ_ACT_RESURRECT);
    if (a <= 0 || v <= 0 || r <= 0) return 0;
    scr->act[0] = (uint8_t)a; scr->act[1] = (uint8_t)v; scr->act[2] = (uint8_t)r;
    return 1;
}

static int family(const SWEEP_SCRATCH* scr, int a)
{
    return a > 0 && (a == scr->act[0] || a == scr->act[1] || a == scr->act[2]);
}

/* The order the resolver gives the reclaimer at `slot` for mode 12 with no target unit at `pos`, when it
   is of the reclaim family; 0 otherwise. Mode 12's answers with no target unit are RESURRECT (0x43F612),
   VTOL_RECLAIM or RECLAIM (0x43F68A..0x43F6A5) or none (0x43F6AA, 0x4401DC) -- the family or nothing
   (DISASSEMBLED); the family test guards it all the same, since the contextual case's copy of the same tests
   (0x43FF38..0x4401D9) can answer a move. The resolver's answer is the family's or none for every reclaimer
   alike -- it reads nothing of the unit but its def's canreclamate bit (0x43F4FD) and its player's mapped
   bit -- so a UI may ask it of one reclaimer for all; which order of the family it is depends on the unit
   (RESURRECT for a def with +0x245 bit 0x800, 0x43F5A2; VTOL_RECLAIM for one with +0x241 bit 11,
   0x43F68E..0x43F69F), so the commit asks it again for each builder. */
int tasim_sweep_order_at(const WORLDQ* q, const SWEEP_SCRATCH* scr, int slot, const int32_t pos[3])
{
    const int a = q->ops->can(q, slot, WQ_CAN_RECLAIM, (int)WQ_NONE, pos);
    return family(scr, a) ? a : 0;
}

void tasim_sweep_target_of(const WORLDQ* q, const WQ_MAP* m, const WQ_FEAT* f, SWEEP_TARGET* t)
{
    memset(t, 0, sizeof *t);
    t->ax = f->ax; t->az = f->az;
    t->cell = f->az * m->w + f->ax;
    t->fx = f->fx; t->fz = f->fz;
    t->cx = 16 * f->ax + 8 * f->fx;
    t->cz = 16 * f->az + 8 * f->fz;
    t->def = f->def;
    t->metal = f->metal; t->energy = f->energy;
    t->pos[0] = (int32_t)((uint32_t)t->cx << 16);
    t->pos[2] = (int32_t)((uint32_t)t->cz << 16);
    t->pos[1] = (int32_t)((uint32_t)q->ops->ground(q, t->pos) << 16);
}

/* WHAT THE FEATURE DRAW SHOWS THE SEAT (engine/map.md, *What the feature draw shows a player*): DrawGameScreen's
   pre-pass 0x4698A4..0x469936 draws an anchor with no sight test (0x46A610) unless its def has +0xFF bit 3,
   nodrawundergray, and draws such a one only when its owner bits, cell +0x0C bits 3-6, are the viewed
   player's index (0x4698E1..0x4698F0) or 0x4658E0 passes: the corners (x, z) and (x + fx, z + fz), in px at
   16 a cell as 16-bit words, each with its row sheared by half the anchor's height byte, read on the viewer's
   sight counters under LosType bit 1 and on its mapped bit otherwise -- block_lit, the seat's own grid. The
   corner's row is the live rule's (tagpu_patches.c, fix_los_shear, LOS_FEATURE): a sheared row above the
   grid is read at the corner's own row. The fog overlay 0x4848E0, drawn over everything, blacks the
   unexplored blocks, so a feature is drawn for the seat only where its anchor's cell is explored. One explored
   test, one cell read and at most four block tests. */
static int corner_lit(const WORLDQ* q, int cx, int cz, int alt)
{
    const int x = (int16_t)(uint16_t)((unsigned)cx << 4), z = (int16_t)(uint16_t)((unsigned)cz << 4);
    const int row = (z - (alt >> 1)) >> 5;
    return q->ops->block_lit(q, x >> 5, row) || (row < 0 && q->ops->block_lit(q, x >> 5, z >> 5));
}

int tasim_feature_drawn(const WORLDQ* q, const WQ_FEAT* f)
{
    if (!q->ops->explored(q, f->ax, f->az)) return 0;
    if (!f->gated || f->owner == q->seat) return 1;
    return corner_lit(q, f->ax, f->az, f->alt) || corner_lit(q, f->ax + f->fx, f->az + f->fz, f->alt);
}

/* A target of the kind, by the click's own rules: reclaimable (FeatureDef +0xFE bit 7) and mapped at its
   order position (0x408090 for the local seat, the same arithmetic as the resolver's inline test and
   patched by the same fix: tagpu_patches.c, LOS_MAPPED) -- or, with seat_sight, what the feature draw shows
   the seat (tasim_feature_drawn), which the caller tested: the reclaim handler 0x404AD0 tests the feature and
   its reclaimable bit and no map (engine/orders.md, *The reclaim order's countdown*), so an order by name needs
   the seat's own test. */
static int takes(const WORLDQ* q, const WQ_FEAT* f, unsigned kind, const SWEEP_TARGET* t, int seat_sight)
{
    return (f->flags & 0x80u) && tasim_sweep_kind_ok(f->metal, f->energy, kind) &&
           (seat_sight || q->ops->mapped_point(q, t->pos));
}

int tasim_sweep_feature_takes(const WORLDQ* q, int x, int z, unsigned kind, SWEEP_TARGET* out)
{
    WQ_MAP m;
    WQ_FEAT f;
    q->ops->map(q, &m);
    if (!q->ops->feature(q, x, z, &f)) return 0;
    tasim_sweep_target_of(q, &m, &f, out);
    return takes(q, &f, kind, out, 0);
}

/* the targets' order: nearer the centre first, then z, then x, then the anchor cell */
static int later(const SWEEP_TARGET* a, const SWEEP_TARGET* b)
{
    if (a->d2 != b->d2) return a->d2 > b->d2;
    if (a->cz != b->cz) return a->cz > b->cz;
    if (a->cx != b->cx) return a->cx > b->cx;
    return a->cell > b->cell;
}

static void sift_down(SWEEP_TARGET* t, unsigned n, unsigned i)
{
    for (;;) {
        unsigned l = 2 * i + 1, r = l + 1, m = i;
        SWEEP_TARGET tmp;
        if (l < n && later(&t[l], &t[m])) m = l;
        if (r < n && later(&t[r], &t[m])) m = r;
        if (m == i) return;
        tmp = t[i]; t[i] = t[m]; t[m] = tmp;
        i = m;
    }
}

static int floor16(int v) { return v >= 0 ? v / 16 : -((15 - v) / 16); }

/* WHICH FEATURES A RING HOLDS, nearest the centre first, at most `cap`; `*more` the ones past it. Anchors
   only -- a cell whose feature is anchored on the cell itself -- so no feature counts twice and no hop is
   followed; the cells scanned are the ring's box, widened below by a footprint's reach and cut to the map.
   A feature is in when its centre is (sweep_inside, 64-bit) and `takes` holds.
   WHY THIS IS THE RESOLVER'S ACCEPTANCE: for no target unit the resolver gives a reclaimer a reclaim-family
   order exactly when the feature 0x4815A0 finds at the position is reclaimable and the mapped bit there is
   set (mode 12's case, 0x43F4F7..0x43F6AA); the position lies in the anchor's footprint, so 0x4815A0 finds
   this anchor, directly or by its hop; and 0x408090 is the same mapped arithmetic. The commit asks the
   resolver itself before every order and counts a refusal as a mismatch, the self-check of this
   equivalence, which must read 0. The resolver is not asked here: each call reaches 0x438760's name search,
   and a forest is thousands of anchors a frame. With seat_sight the caller orders RECLAIM by name, which the
   handler takes for any reclaimable feature at the position; the test is what the feature draw shows the seat
   (tasim_feature_drawn), and a cell on ground the seat has not explored is never read. */
unsigned tasim_sweep_targets(const WORLDQ* q, const SWEEP_IN* in, SWEEP_TARGET* out, unsigned cap, unsigned* more)
{
    WQ_MAP m;
    int x0, x1, z0, z1, x, z;
    unsigned n = 0, i;
    q->ops->map(q, &m);
    x0 = floor16(in->c[0] - in->r) - SWEEP_FOOT_MAX / 2; x1 = floor16(in->c[0] + in->r);
    z0 = floor16(in->c[2] - in->r) - SWEEP_FOOT_MAX / 2; z1 = floor16(in->c[2] + in->r);
    *more = 0;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= m.w) x1 = m.w - 1;
    if (z1 >= m.h) z1 = m.h - 1;
    for (z = z0; z <= z1; z++)
        for (x = x0; x <= x1; x++) {
            WQ_FEAT f;
            SWEEP_TARGET t;
            if (in->seat_sight && !q->ops->explored(q, x, z)) continue;
            if (!q->ops->feature(q, x, z, &f) || f.ax != x || f.az != z) continue;
            if (in->seat_sight && !tasim_feature_drawn(q, &f)) continue;
            tasim_sweep_target_of(q, &m, &f, &t);
            if (!sweep_inside(in->c[0], in->c[2], in->r, t.cx, t.cz) || !takes(q, &f, in->kind, &t, in->seat_sight))
                continue;
            t.d2 = (uint64_t)((int64_t)(t.cx - in->c[0]) * (t.cx - in->c[0])) +
                   (uint64_t)((int64_t)(t.cz - in->c[2]) * (t.cz - in->c[2]));
            /* the nearest `cap`, kept as a max-heap of the order above */
            if (n < cap) {
                unsigned k = n++;
                out[k] = t;
                while (k && later(&out[k], &out[(k - 1) / 2])) {
                    SWEEP_TARGET tmp = out[k]; out[k] = out[(k - 1) / 2]; out[(k - 1) / 2] = tmp;
                    k = (k - 1) / 2;
                }
            } else {
                (*more)++;
                if (cap && later(&out[0], &t)) { out[0] = t; sift_down(out, n, 0); }
            }
        }
    /* the heap sorted in place, nearest first */
    for (i = n; i > 1; i--) {
        SWEEP_TARGET tmp = out[0]; out[0] = out[i - 1]; out[i - 1] = tmp;
        sift_down(out, i - 1, 0);
    }
    return n;
}

/* the targets in anchor-cell order, for target_at: cell_order holds their indices sorted (an insertion
   sort: the order is total, the cells being distinct anchors) */
static void sort_targets(SWEEP_SCRATCH* scr, unsigned n)
{
    unsigned i, j;
    for (i = 0; i < n; i++) {
        const uint16_t k = (uint16_t)i;
        for (j = i; j > 0 && scr->t[scr->cell_order[j - 1]].cell > scr->t[k].cell; j--)
            scr->cell_order[j] = scr->cell_order[j - 1];
        scr->cell_order[j] = k;
    }
}

/* the target anchored at `cell`, or -1; after sort_targets(n) */
static int target_at(const SWEEP_SCRATCH* scr, unsigned n, int cell)
{
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (scr->t[scr->cell_order[mid]].cell < cell) lo = mid + 1; else hi = mid;
    }
    return lo < n && scr->t[scr->cell_order[lo]].cell == cell ? (int)scr->cell_order[lo] : -1;
}

/* The ring's target an order reclaims, or -1: a reclaim-family order whose feature, found at its position as
   the Reclaim handler finds it (0x421DA0 at order +0x22), is one of the targets. THE DOOMED LEAD: `doomed`
   marks an order the next append takes off -- 0x43ADC0 destroys the head of +0x5C while it carries 0x4000
   (0x43AE7C..0x43AECE), on every call, append or not -- so a leading run of such orders reclaims nothing
   that lasts, and its targets are queued again. */
static int order_target(const WORLDQ* q, const SWEEP_SCRATCH* scr, const WQ_ORDER* o, unsigned n, int doomed)
{
    WQ_MAP m;
    WQ_FEAT f;
    int x, z;
    if (doomed || !family(scr, o->action)) return -1;
    q->ops->map(q, &m);
    x = o->pos[0] >> 20; z = o->pos[2] >> 20;
    if (x < 0 || z < 0 || x >= m.w || z >= m.h || !q->ops->feature(q, x, z, &f)) return -1;
    return target_at(scr, n, f.az * m.w + f.ax);
}

/* WITHOUT SHIFT A RING REPLACES AS A STOCK ORDER DOES, BUT FOR WHAT IT TARGETS: of each reclaimer's list it
   takes off what 0x43ADC0's replace takes off -- every order of the +0x5C list without +0x42 bit 2
   (0x43AE14..0x43AE59) -- except an order that already reclaims one of the ring's targets, which stays where
   it is, so a builder at work on one goes on with it. An order carrying 0x40000 belongs on +0x60 (0x439F8F
   picks that list for it), so it is never taken. The places are read in one walk, then taken off in
   DESCENDING PLACE: a removal moves no place still to come, and the head goes last; each is found again at
   its place and taken only while it still carries the action, position and flags the walk read. Returns
   the orders taken off; `*stale` counts a place that held something else by then, which stays. After
   sort_targets(n). */
static unsigned cancel_others(const WORLDQ* q, SWEEP_SCRATCH* scr, unsigned n, unsigned* stale)
{
    unsigned b, removed = 0;
    for (b = 0; b < scr->nb; b++) {
        int k, nc = 0, lead = 1, count = q->ops->orders(q, scr->slot[b], scr->list, WQ_LIST_MAX);
        for (k = 0; k < count; k++) {
            const WQ_ORDER* o = &scr->list[k];
            lead = lead && (o->flags & WQ_OF_DROP);
            if ((o->flags & (WQ_OF_SECOND | WQ_OF_KEEP)) || order_target(q, scr, o, n, lead) >= 0) continue;
            scr->cut[nc++] = (uint16_t)k;
        }
        while (nc--) {
            const WQ_ORDER* o = &scr->list[scr->cut[nc]];
            WQ_CMD cmd;
            memset(&cmd, 0, sizeof cmd);
            cmd.verb = WQ_CMD_REMOVE;
            cmd.unit = scr->slot[b];
            cmd.target = (uint16_t)WQ_NONE;
            cmd.expect = *o;
            if (q->ops->order(q, &cmd) == WQ_OK) removed++;
            else ++*stale;
        }
    }
    return removed;
}

/* Every target that a reclaim-family order in a reclaimer's list already reclaims is dropped, so no
   feature is queued twice. Returns the targets kept, compacted in their order. After sort_targets(n). */
static unsigned drop_queued(const WORLDQ* q, SWEEP_SCRATCH* scr, unsigned n)
{
    unsigned i, b, k;
    memset(scr->drop, 0, n);
    for (b = 0; b < scr->nb; b++) {
        int j, t, lead = 1, count = q->ops->orders(q, scr->slot[b], scr->list, WQ_LIST_MAX);
        for (j = 0; j < count; j++) {
            lead = lead && (scr->list[j].flags & WQ_OF_DROP);
            if ((t = order_target(q, scr, &scr->list[j], n, lead)) >= 0) scr->drop[t] = 1;
        }
    }
    for (i = k = 0; i < n; i++)
        if (!scr->drop[i]) scr->t[k++] = scr->t[i];
    return k;
}

int tasim_sweep_builder(const WORLDQ* q, int slot, SweepBuilder* out)
{
    WQ_UNIT u;
    WQ_DEF d;
    memset(out, 0, sizeof *out);
    if (!q->ops->unit(q, slot, &u) || !q->ops->def(q, u.type, &d)) return 0;
    out->x = u.pos[0] >> 16;
    out->z = u.pos[2] >> 16;
    out->speed = (double)d.speed / 65536.0;
    out->reach = (double)d.reach;
    return 1;
}

double tasim_sweep_ticks(double speed, double reach, int32_t px, int32_t pz, int32_t cx, int32_t cz,
                         float energy, float metal)
{
    const double dx = (double)(cx - px), dz = (double)(cz - pz), dd = sqrt(dx * dx + dz * dz);
    const double travel = speed > 0.0 && dd > reach ? (dd - reach) / speed : 0.0;
    return travel + (double)sweep_reclaim_ticks(energy, metal);
}

/* The reclaimer's split inputs, from its list: a ring only appends, so a builder starts at its last order
   that has a position, its load the model's ticks over its queued reclaim-family orders from its position
   through each. Its room keeps the list under SWEEP_LIST_CAP. Every position in
   whole world px, a 16.16 word >> 16. */
static void builder_inputs(const WORLDQ* q, SWEEP_SCRATCH* scr, unsigned b, SweepBuilder* out)
{
    WQ_MAP m;
    int k, count, px, pz;
    if (!tasim_sweep_builder(q, scr->slot[b], out) ||
        (count = q->ops->orders(q, scr->slot[b], scr->list, WQ_LIST_MAX)) < 0) {   /* no room */
        memset(out, 0, sizeof *out);
        return;
    }
    q->ops->map(q, &m);
    px = out->x;
    pz = out->z;
    for (k = 0; k < count; k++) {
        const int32_t* pos = scr->list[k].pos;
        WQ_FEAT f;
        int x = pos[0] >> 20, z = pos[2] >> 20;
        if (pos[0] > 0 && pos[2] > 0) { out->x = pos[0] >> 16; out->z = pos[2] >> 16; }
        if (family(scr, scr->list[k].action) && x >= 0 && z >= 0 && x < m.w && z < m.h && q->ops->feature(q, x, z, &f)) {
            const int cx = 16 * f.ax + 8 * f.fx, cz = 16 * f.az + 8 * f.fz;
            out->load += tasim_sweep_ticks(out->speed, out->reach, px, pz, cx, cz, f.energy, f.metal);
            px = cx; pz = cz;
        }
    }
    out->room = (unsigned)count >= SWEEP_LIST_CAP ? 0u : SWEEP_LIST_CAP - (unsigned)count;
}

/* THE COMMIT: the ring's targets, split nearest-first across the selected reclaimers, each issued as the
   order the resolver gives that builder there. WHAT IT TARGETS IS ONLY ADDED TO: a builder already
   reclaiming one of the ring's targets keeps that order where it is, and no feature is queued twice.
   Without Shift the rest of each builder's list goes, as a stock order's replace takes it (cancel_others);
   with Shift it all stays. Every new order goes through the new-order root WITH ITS APPEND ARGUMENT 1:
   the click root with Shift first cancels a same-action order within 16 world units and creates nothing
   (the toggle), so it would take back a neighbouring 1x1 feature's order instead of adding this one; and
   the new-order root's replace would take off a builder's order on one of the ring's own targets, which
   cancel_others spares. The resolver is asked again for every order: the preview is never trusted, and the
   order byte is the builder's own (VTOL_RECLAIM for a VTOL, RESURRECT for a resurrector). A refusal is a
   mismatch. */
int tasim_sweep_commit(const WORLDQ* q, const SWEEP_IN* in, SWEEP_RES* res, SWEEP_SCRATCH* scr)
{
    unsigned n, m = 0, k;
    memset(res, 0, sizeof *res);
    if (!tasim_sweep_reclaimers(q, in, scr) || !tasim_sweep_actions(q, scr)) return 0;
    res->builders = scr->nb;
    n = res->found = tasim_sweep_targets(q, in, scr->t, SWEEP_MAX_TARGETS, &res->more);
    if (n) {
        sort_targets(scr, n);
        if (!in->shift) res->cancelled = cancel_others(q, scr, n, &res->stale);
        n = drop_queued(q, scr, n);
    }
    if (n) {
        WQ_CMD cmd;
        for (k = 0; k < scr->nb; k++) builder_inputs(q, scr, k, &scr->b[k]);
        for (k = 0; k < n; k++) {
            scr->st[k].x = scr->t[k].cx; scr->st[k].z = scr->t[k].cz;
            scr->st[k].ticks = sweep_reclaim_ticks(scr->t[k].energy, scr->t[k].metal);
        }
        m = sweep_split(scr->b, scr->nb, scr->st, n, scr->who, scr->seq);
        memset(&cmd, 0, sizeof cmd);
        cmd.verb = WQ_CMD_NEW;
        cmd.shift = 1;
        cmd.target = (uint16_t)WQ_NONE;
        for (k = 0; k < m; k++) {
            const SWEEP_TARGET* t = &scr->t[scr->seq[k]];
            const int slot = scr->slot[scr->who[k]];
            int action;
            if (!(action = tasim_sweep_order_at(q, scr, slot, t->pos))) { res->mismatch++; continue; }
            cmd.unit = (uint16_t)slot;
            cmd.action = (uint8_t)action;
            memcpy(cmd.pos, t->pos, sizeof cmd.pos);
            if (q->ops->order(q, &cmd) == WQ_OK) res->issued++;
        }
        for (k = 0; k < scr->nb; k++) res->capped += m < n && !scr->b[k].room;
    }
    res->taken = n;
    res->unassigned = n - m;
    return 1;
}
