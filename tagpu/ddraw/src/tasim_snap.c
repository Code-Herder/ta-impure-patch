/* tasim_snap.c -- the resource buildings' rules over WORLDQ (tasim_snap.h). research/notes/modules/resource-snapping.md
   states the rules; the input and placement contracts are its and exe-reverse-engineering.md's. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_snap.h"

/* One decision's view: the world, its map, what the UI gave and the caller's scratch. */
typedef struct {
    const WORLDQ*        q;
    WQ_MAP               m;
    int                  first, last;
    const SNAP_QUICK_IN* in;            /* NULL for the cursor's snap, which reads no plan */
    SNAP_SCRATCH*        scr;
} SNAP;

typedef struct { WQ_ORDER build, reclaim; int has_reclaim; } CANCEL;

static void snap_open(SNAP* c, const WORLDQ* q, const SNAP_QUICK_IN* in, SNAP_SCRATCH* scr)
{
    c->q = q;
    q->ops->map(q, &c->m);
    q->ops->seat_units(q, &c->first, &c->last);
    c->in = in;
    c->scr = scr;
}

static int footprint(const WQ_DEF* d, int* x, int* z)
{
    *x = d->fx; *z = d->fz;
    return *x > 0 && *z > 0 && *x <= WQ_FOOT_MAX && *z <= WQ_FOOT_MAX;
}

static int def(const SNAP* c, int type, WQ_DEF* d) { return c->q->ops->def(c->q, type, d); }

/* the UI's selection holds the slot (SNAP_QUICK_IN.builders is ascending) */
static int selected(const SNAP* c, int slot)
{
    int lo = 0, hi = c->in->nbuilders;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c->in->builders[mid] < slot) lo = mid + 1;
        else hi = mid;
    }
    return lo < c->in->nbuilders && c->in->builders[lo] == slot;
}

/* the builder type's build menu offers `type` */
static int offers(const SNAP* c, int builder_type, int type)
{
    int n = c->q->ops->build_options(c->q, builder_type, c->scr->opts, WQ_TYPES_MAX), lo = 0, hi;
    if (n <= 0 || type <= 0) return 0;
    hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c->scr->opts[mid] < type) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && c->scr->opts[lo] == type;
}

static int can_build(const SNAP* c, const WQ_UNIT* u, int type)
{
    WQ_DEF d;
    return u->type && def(c, u->type, &d) && (d.flags & WQ_DF_BUILDER) && offers(c, u->type, type);
}

static int can_assist(const SNAP* c, const WQ_UNIT* u)
{
    WQ_DEF d;
    return u->type && def(c, u->type, &d) && (d.flags & WQ_DF_ASSIST);
}

static int metal(const SNAP* c, int x, int z) { return c->q->ops->metal(c->q, x, z); }

/* The footprint inside the map's placeable interior and every cell of it explored: what fits() asks before
   the site test, and all a queued plan's site has to satisfy. 32767 px is the most a 16.16 position holds. */
static int site_known(const SNAP* c, int type, int x, int z)
{
    WQ_DEF d;
    int fx, fz, dx, dz;
    if (!def(c, type, &d) || !footprint(&d, &fx, &fz) || x < 1 || z < 1 || x + fx >= c->m.w || z + fz >= c->m.h ||
        x * 16 + fx * 8 > 32767 || z * 16 + fz * 8 > 32767) return 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++)
        if (!c->q->ops->explored(c->q, x + dx, z + dz)) return 0;
    return 1;
}

static int fits(const SNAP* c, int type, int x, int z, int ignore, int* height)
{
    return site_known(c, type, x, z) && c->q->ops->site_ok(c->q, type, x, z, ignore, height);
}

void tasim_snap_position(const WORLDQ* q, const SNAP_CHOICE* c, int32_t pos[3])
{
    WQ_DEF d;
    int fx = 0, fz = 0;
    if (q->ops->def(q, c->type, &d)) { fx = d.fx; fz = d.fz; }
    pos[0] = (c->x * 16 + fx * 8) * 65536;
    pos[1] = c->height * 65536;
    pos[2] = (c->z * 16 + fz * 8) * 65536;
}

/* A closed, explored metal component is a patch. A component reaching the finite window is broad metal
   ground; unexplored neighbours cannot prove closure. No unseen metal byte is consulted. Eight neighbours
   keep diagonal deposit cells in one patch. The arena bound, SNAP_PATCH_CAP cells, is independent of map
   size. The anchor -- the component's least (z, x) -- names the deposit for same_plan. The flood from the
   metal cell (sx, sz): 1 for a patch, with its anchor, bounds and size in `out`; `*cells` the cells it
   queued, whatever it answers. */
static int flood(const SNAP* c, int sx, int sz, SNAP_PATCH* out, unsigned* cells)
{
    uint8_t* seen = c->scr->seen;
    uint16_t* queue = c->scr->queue;
    int left = sx - SNAP_PATCH_R, top = sz - SNAP_PATCH_R, ax = sx, az = sz, x0 = sx, z0 = sz, x1 = sx, z1 = sz;
    unsigned head = 0, tail = 0;
    int boundary = 0, edges = 0, closed = 0;
    memset(seen, 0, SNAP_PATCH_CAP);
    queue[tail++] = SNAP_PATCH_R * SNAP_PATCH_SIDE + SNAP_PATCH_R;
    seen[SNAP_PATCH_R * SNAP_PATCH_SIDE + SNAP_PATCH_R] = 1;
    while (head < tail) {
        unsigned idx = queue[head++];
        int rx = (int)(idx % SNAP_PATCH_SIDE), rz = (int)(idx / SNAP_PATCH_SIDE), dx, dz;
        if (!rx || !rz || rx == SNAP_PATCH_SIDE - 1 || rz == SNAP_PATCH_SIDE - 1) goto done;
        if (left + rx == 0) edges |= 1;
        if (left + rx == c->m.w - 1) edges |= 2;
        if (top + rz == 0) edges |= 4;
        if (top + rz == c->m.h - 1) edges |= 8;
        for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++) {
            unsigned next = (unsigned)(rz + dz) * SNAP_PATCH_SIDE + (unsigned)(rx + dx);
            int wx = left + rx + dx, wz = top + rz + dz;
            if (seen[next] || wx < 0 || wz < 0 || wx >= c->m.w || wz >= c->m.h) continue;
            seen[next] = 1;
            if (!c->q->ops->explored(c->q, wx, wz)) goto done;
            if (metal(c, wx, wz) <= c->m.threshold) { boundary = 1; continue; }
            if (tail >= SNAP_PATCH_CAP) goto done;
            queue[tail++] = (uint16_t)next;
            if (wz < az || (wz == az && wx < ax)) ax = wx, az = wz;
            if (wx < x0) x0 = wx;
            if (wx > x1) x1 = wx;
            if (wz < z0) z0 = wz;
            if (wz > z1) z1 = wz;
        }
    }
    /* The edge of the map alone cannot distinguish a small uniform metal map from a deposit. Require a
       discovered background cell as well. */
    closed = boundary && (edges & 3) != 3 && (edges & 12) != 12;
done:
    *cells = tail;
    out->ax = (int16_t)ax; out->az = (int16_t)az;
    out->x0 = (int16_t)x0; out->z0 = (int16_t)z0; out->x1 = (int16_t)x1; out->z1 = (int16_t)z1;
    out->cells = (uint16_t)tail;
    return closed;
}

/* The patch the footprint at the choice's cell starts: the flood from its first metal cell in scan order.
   The anchor is written as the flood finds it, closed or not, as same_plan's callers read it. */
static int distinct(const SNAP* c, SNAP_CHOICE* ch, int fx, int fz)
{
    SNAP_PATCH patch;
    unsigned cells;
    int x, z, sx = -1, sz = -1, closed;
    for (z = ch->z; z < ch->z + fz && sx < 0; z++) for (x = ch->x; x < ch->x + fx; x++)
        if (metal(c, x, z) > c->m.threshold) { sx = x; sz = z; break; }
    if (sx < 0) return 0;
    closed = flood(c, sx, sz, &patch, &cells);
    ch->anchor_x = patch.ax; ch->anchor_z = patch.az;
    return closed;
}

/* THE PLANS THE SITE WOULD COLLIDE WITH: a placed build of the seat's whose yard overlaps the candidate's,
   bit by bit as the site test reads yards -- a cell whose byte has bit 2 against one with bit 1 or 2 --
   except the plans a click without Shift replaces: those of the selected units it orders, a builder that can
   build it (and reclaim the unit it replaces) or a helper that would guard the first of them, unless their
   node carries flag 0x4, which survives a replace. */
static int plans_block(const SNAP* c, const SNAP_CHOICE* ch)
{
    const WORLDQ* q = c->q;
    const uint8_t* yard = c->scr->yard[0];
    WQ_DEF d;
    WQ_UNIT u;
    int fx, fz, primary = 0, i, k, n;
    int32_t pos[3];
    if (!def(c, ch->type, &d) || !footprint(&d, &fx, &fz) || !d.yard ||
        !q->ops->yard(q, ch->type, c->scr->yard[0], WQ_FOOT_MAX * WQ_FOOT_MAX)) return 1;
    tasim_snap_position(q, ch, pos);
    if (!c->in->shift) for (i = c->first; i <= c->last; i++) {
        if (q->ops->unit(q, i, &u) && selected(c, i) && can_build(c, &u, ch->type) &&
            (!ch->target || q->ops->can(q, i, WQ_CAN_RECLAIM, ch->target, pos) >= 0)) { primary = i; break; }
    }
    for (i = c->first; i <= c->last; i++) {
        int ordered;
        if (!q->ops->unit(q, i, &u)) continue;
        ordered = primary && selected(c, i) &&
            ((can_build(c, &u, ch->type) && (!ch->target || q->ops->can(q, i, WQ_CAN_RECLAIM, ch->target, pos) >= 0)) ||
             (can_assist(c, &u) && q->ops->can(q, i, WQ_CAN_GUARD, primary, pos) >= 0));
        n = q->ops->orders(q, i, c->scr->list, WQ_LIST_MAX);
        for (k = 0; k < n; k++) {
            const WQ_ORDER* o = &c->scr->list[k];
            const uint8_t* oy = c->scr->yard[1];
            WQ_DEF od;
            int ofx, ofz, ox, oz, x, z;
            if (!o->build) continue;
            if (ordered && !(o->flags & WQ_OF_KEEP)) continue;
            if (!def(c, o->type, &od) || (od.flags & WQ_DF_MOVER) || !footprint(&od, &ofx, &ofz) || !od.yard ||
                !q->ops->yard(q, o->type, c->scr->yard[1], WQ_FOOT_MAX * WQ_FOOT_MAX)) continue;
            ox = (int)(((int64_t)o->pos[0] - ofx * 524288 + 524288) >> 20);
            oz = (int)(((int64_t)o->pos[2] - ofz * 524288 + 524288) >> 20);
            for (z = ch->z > oz ? ch->z : oz; z < ch->z + fz && z < oz + ofz; z++)
                for (x = ch->x > ox ? ch->x : ox; x < ch->x + fx && x < ox + ofx; x++) {
                    unsigned a = yard[(z - ch->z) * fx + x - ch->x];
                    unsigned b = oy[(z - oz) * ofx + x - ox];
                    if (((a & 4u) && (b & 6u)) || ((b & 4u) && (a & 6u))) return 1;
                }
        }
    }
    return 0;
}

/* The one explored geothermal feature under the G cells (yard byte bit 7) of a footprint, by its anchor
   cell: 0 when none, or when it touches several -- one footprint over several distinct vents has no unique
   cancellation identity, so no reservation it means is guessed. A hop to an anchor past the cell, which no
   footprint tile of the engine's points to, is no feature. */
static int geo_anchor(const SNAP* c, int type, int x, int z, int* ax, int* az)
{
    const WORLDQ* q = c->q;
    const uint8_t* yard = c->scr->yard[0];
    WQ_DEF d;
    WQ_FEAT f;
    int fx, fz, dx, dz, found = 0;
    if (!def(c, type, &d) || !d.yard || !footprint(&d, &fx, &fz) || x < 0 || z < 0 || x + fx > c->m.w ||
        z + fz > c->m.h || !q->ops->yard(q, type, c->scr->yard[0], WQ_FOOT_MAX * WQ_FOOT_MAX)) return 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++) {
        int cx = x + dx, cz = z + dz;
        if (!(yard[dz * fx + dx] & 0x80u) || !q->ops->explored(q, cx, cz)) continue;
        if (!q->ops->feature(q, cx, cz, &f) || f.ax > cx || f.az > cz || !q->ops->explored(q, f.ax, f.az) ||
            !(f.flags & 0x20u)) continue;
        if (found && (*ax != f.ax || *az != f.az)) return 0;
        *ax = f.ax; *az = f.az; found = 1;
    }
    return found;
}

/* A queued build is the choice's plan when it is of the same kind and on the same vent, the same deposit
   (the same anchor), or for an upgrade within 16 world units of the same place on both axes. Its building
   type need not be the one offered now. */
static int same_plan(const SNAP* c, const SNAP_CHOICE* ch, const WQ_ORDER* node)
{
    WQ_DEF d;
    int x, z, fx, fz;
    if (!node->build || !def(c, node->type, &d) || d.kind != ch->kind || !footprint(&d, &fx, &fz)) return 0;
    x = ((node->pos[0] >> 16) - fx * 8) / 16;
    z = ((node->pos[2] >> 16) - fz * 8) / 16;
    if (ch->kind == WQ_KIND_GEO) {
        int ax, az, bx, bz;
        return geo_anchor(c, ch->type, ch->x, ch->z, &ax, &az) && geo_anchor(c, node->type, x, z, &bx, &bz) &&
            ax == bx && az == bz;
    } else if (ch->target) {
        int32_t pos[3];
        tasim_snap_position(c->q, ch, pos);
        return llabs((int64_t)node->pos[0] - pos[0]) <= 1048576 && llabs((int64_t)node->pos[2] - pos[2]) <= 1048576;
    } else {
        SNAP_CHOICE other;
        memset(&other, 0, sizeof other);
        other.x = x; other.z = z;
        return x >= 0 && z >= 0 && x + fx <= c->m.w && z + fz <= c->m.h && distinct(c, &other, fx, fz) &&
            other.anchor_x == ch->anchor_x && other.anchor_z == ch->anchor_z;
    }
}

/* The builder's first cancellable plan of the choice: an order the second list does not own (0x40000), and
   for an upgrade only a build that is not the head and follows the reclaim of the same unit at the same
   place, the reclaim the resolver would give it there. DISASSEMBLED 0x43A1F0: an unstarted, non-head build
   with no StopBuilding flag (WQ_OF_STOP) or owned object cannot call a handler, script or object destructor;
   exact removal sets 0x10000 for a non-head, skipping 0x489800 too. The build is removed with its reclaim.
   It reads `scr->list` and leaves it holding the unit's list. */
static int find_cancel(const SNAP* c, int slot, const SNAP_CHOICE* ch, CANCEL* out)
{
    const WORLDQ* q = c->q;
    WQ_UNIT u, t;
    WQ_DEF d;
    int k, n, target = ch->target && q->ops->unit(q, ch->target, &t);
    if (!q->ops->unit(q, slot, &u) || !u.type || !def(c, u.type, &d) || !(d.flags & WQ_DF_BUILDER)) return 0;
    n = q->ops->orders(q, slot, c->scr->list, WQ_LIST_MAX);
    for (k = 0; k < n; k++) {
        const WQ_ORDER* node = &c->scr->list[k];
        const WQ_ORDER* previous = k ? &c->scr->list[k - 1] : NULL;
        if (!same_plan(c, ch, node) || (node->flags & WQ_OF_SECOND)) continue;
        out->build = *node;
        out->has_reclaim = 0;
        if (ch->target) {
            if (!target || !previous || (previous->flags & WQ_OF_SECOND) || previous->target != ch->target ||
                q->ops->can(q, slot, WQ_CAN_RECLAIM, ch->target, node->pos) != previous->action ||
                memcmp(previous->pos, node->pos, sizeof node->pos)) continue;
            if (node->started || (node->flags & WQ_OF_STOP) || node->owned) continue;
            out->reclaim = *previous;
            out->has_reclaim = 1;
        }
        return 1;
    }
    return 0;
}

/* SNAP_QUEUED when a plan of the seat's is the choice's, with SNAP_CANCEL when a selected builder holding
   it can cancel it. Each unit's first matching order decides for that unit. */
static int queued(const SNAP* c, const SNAP_CHOICE* ch)
{
    const WORLDQ* q = c->q;
    WQ_UNIT u;
    int i, k, n, found = 0;
    for (i = c->first; i <= c->last; i++) {
        if (!q->ops->unit(q, i, &u)) continue;
        n = q->ops->orders(q, i, c->scr->list, WQ_LIST_MAX);
        for (k = 0; k < n; k++) {
            if (same_plan(c, ch, &c->scr->list[k])) {
                CANCEL cancel;
                found |= SNAP_QUEUED;
                /* find_cancel rereads this unit's list into the same buffer, and nothing reads it after */
                if (selected(c, i) && find_cancel(c, i, ch, &cancel)) return found | SNAP_CANCEL;
                break;
            }
        }
    }
    return found;
}

/* Every selected builder loses the plan find_cancel names, the build first and then its reclaim, each found
   again by its place (WQ_CMD_REMOVE); a removal of the build leaves the reclaim's place before it as it was.
   The count of builders that held one. */
static int unqueue(const SNAP* c, const SNAP_CHOICE* ch)
{
    const WORLDQ* q = c->q;
    WQ_UNIT u;
    int i, count = 0;
    for (i = c->first; i <= c->last; i++) {
        CANCEL cancel;
        WQ_CMD cmd;
        if (!q->ops->unit(q, i, &u) || !selected(c, i) || !find_cancel(c, i, ch, &cancel)) continue;
        memset(&cmd, 0, sizeof cmd);
        cmd.verb = WQ_CMD_REMOVE;
        cmd.unit = (uint16_t)i;
        cmd.target = (uint16_t)WQ_NONE;
        cmd.expect = cancel.build;
        q->ops->order(q, &cmd);
        if (cancel.has_reclaim) {
            cmd.expect = cancel.reclaim;
            q->ops->order(q, &cmd);
        }
        count++;
    }
    return count;
}

/* THE SNAP SCORER: candidates are centre cells within `radius` of the seed's cell; a footprint's origin is
   its centre minus half the footprint, rounded down. A mex scores its coverage, a geothermal plant 1. The
   highest score wins, and a tie goes to the centre nearest the raw cursor, the first in scan order on an
   exact tie. `legal` (Quick Build) skips a candidate the site test or a plan refuses unless it is the site
   of a queued plan; without it a mex ignores occupancy and a geothermal plant takes the nearest site that
   fits.
   THE DISTANCE IS EXACT. From the candidate centre (cx + 0.5 cells) to the raw point in 1/2^20 cells,
   T = cx * 2^20 + 2^19 - raw is an integer, and D = Tx^2 + Tz^2 orders the candidates exactly as the
   real-valued distance does: this is the exact order. It is an integer so that no build's floating-point
   unit can decide a tie: in double, T^2 needs up to 62 bits, which SSE2 rounds to 53 at once and x87 keeps
   in registers and rounds wherever a value is spilled. The candidates lie
   within `radius` cells of the seed, held to SNAP_RADIUS_MAX, and the entry refuses a seed more than
   SNAP_RADIUS_MAX + 1 cells from the raw point's cell on either axis (snapped's second search starts at the
   first one's chosen centre, within the first radius of it; tasim_snap_site takes any seed), so for any
   caller |T| < (2 x 64 + 2) x 2^20 < 2^28 and D < 2^57: a uint64 holds it exactly, which a double does
   not. */
static int site(const SNAP* c, int type, const int32_t seed[3], const int32_t raw[3], int radius, int legal,
                int ignore, SNAP_CHOICE* out)
{
    WQ_DEF d;
    int fx, fz, dx, dz, base_x = seed[0] >> 20, base_z = seed[2] >> 20;
    int best = 0, bx = 0, bz = 0, height = 0, k;
    uint64_t distance = UINT64_MAX;
    if (radius < 0 || radius > SNAP_RADIUS_MAX || abs(base_x - (raw[0] >> 20)) > SNAP_RADIUS_MAX + 1 ||
        abs(base_z - (raw[2] >> 20)) > SNAP_RADIUS_MAX + 1 || !def(c, type, &d) || !(k = d.kind) ||
        !footprint(&d, &fx, &fz))
        return 0;
    for (dz = -radius; dz <= radius; dz++) for (dx = -radius; dx <= radius; dx++) {
        int cx = base_x + dx, cz = base_z + dz, x = cx - fx / 2, z = cz - fz / 2;
        int h = 0, count;
        const int64_t tx = (int64_t)cx * 1048576 + 524288 - raw[0];
        const int64_t tz = (int64_t)cz * 1048576 + 524288 - raw[2];
        const uint64_t dist = (uint64_t)(tx * tx) + (uint64_t)(tz * tz);
        if (x < 0 || z < 0 || x + fx > c->m.w || z + fz > c->m.h || x * 16 + fx * 8 > 32767 || z * 16 + fz * 8 > 32767)
            continue;
        count = k == WQ_KIND_MEX ? c->q->ops->coverage(c->q, x, z, fx, fz) : 1;
        if (!count || count < best || (count == best && dist >= distance)) continue;
        if (legal) {
            SNAP_CHOICE candidate;
            int taken = !fits(c, type, x, z, ignore, &h);
            if (taken && !site_known(c, type, x, z)) continue;
            memset(&candidate, 0, sizeof candidate);
            candidate.type = type; candidate.kind = k; candidate.x = x; candidate.z = z;
            candidate.height = h; candidate.target = ignore;
            /* WHETHER A PLAN IS QUEUED HERE IS DECIDED BEFORE WHETHER ITS GROUND IS FREE. Ground the site
               test refuses -- a unit standing on the site or crossing it, a wreck -- or another plan's yard
               is this click's only as the site of a plan already queued, and then it is that plan's
               whatever stands on it: the click reaches the repeat and the Shift-cancel, never a native
               order in the plan's place. A free site with no plan in the way is decided as before. */
            if (taken || plans_block(c, &candidate)) {
                if (k == WQ_KIND_MEX && !ignore && !distinct(c, &candidate, fx, fz)) continue;
                if (!queued(c, &candidate)) continue;
                if (taken) h = c->q->ops->blocked_alt(c->q, type, x, z);
            }
        } else if (k == WQ_KIND_GEO && !fits(c, type, x, z, ignore, &h)) continue;
        best = count; distance = dist; bx = x; bz = z; height = h;
    }
    if (!best) return 0;
    out->type = type; out->kind = k; out->x = bx; out->z = bz; out->height = height;
    return 1;
}

static int snapped(const SNAP* c, int type, const int32_t raw[3], int radius, int legal, SNAP_CHOICE* out)
{
    SNAP_CHOICE ch;
    memset(&ch, 0, sizeof ch);
    if (!site(c, type, raw, raw, radius, legal, 0, &ch)) return 0;
    if (ch.kind == WQ_KIND_MEX) {
        SNAP_CHOICE stable;
        WQ_DEF d;
        int32_t next[3];
        def(c, type, &d);
        tasim_snap_position(c->q, &ch, next);
        memset(&stable, 0, sizeof stable);
        if (ch.x + d.fx / 2 != (raw[0] >> 20) || ch.z + d.fz / 2 != (raw[2] >> 20)) {
            /* A search boundary can expose only part of a deposit. Both modes require the chosen coverage
               target to remain best in the footprint-sized neighbourhood, using the raw mouse tie.
               Occupancy cannot turn a partial target into a complete one. */
            if (!site(c, type, next, raw, d.fx > d.fz ? d.fx : d.fz, 0, 0, &stable) || stable.x != ch.x ||
                stable.z != ch.z) return 0;
        }
    }
    *out = ch;
    return 1;
}

int tasim_snap_site_known(const WORLDQ* q, int type, int x, int z)
{
    SNAP c;
    snap_open(&c, q, NULL, NULL);
    return site_known(&c, type, x, z);
}

int tasim_snap_site(const WORLDQ* q, const SNAP_QUICK_IN* in, int type, const int32_t seed[3],
                    const int32_t raw[3], int radius, int legal, int ignore, SNAP_CHOICE* out, SNAP_SCRATCH* scr)
{
    SNAP c;
    if (legal && !in) return 0;
    snap_open(&c, q, in, scr);
    return site(&c, type, seed, raw, radius, legal, ignore, out);
}

int tasim_snap_snapped(const WORLDQ* q, const SNAP_QUICK_IN* in, int type, const int32_t raw[3], int radius,
                       int legal, SNAP_CHOICE* out, SNAP_SCRATCH* scr)
{
    SNAP c;
    if (legal && !in) return 0;
    snap_open(&c, q, in, scr);
    return snapped(&c, type, raw, radius, legal, out);
}

int tasim_snap_distinct(const WORLDQ* q, SNAP_CHOICE* ch, int fx, int fz, SNAP_SCRATCH* scr)
{
    SNAP c;
    snap_open(&c, q, NULL, scr);
    return distinct(&c, ch, fx, fz);
}

int tasim_snap_plans_block(const WORLDQ* q, const SNAP_CHOICE* ch, const SNAP_QUICK_IN* in, SNAP_SCRATCH* scr)
{
    SNAP c;
    if (!in) return 1;
    snap_open(&c, q, in, scr);
    return plans_block(&c, ch);
}

int tasim_snap_queued(const WORLDQ* q, const SNAP_CHOICE* ch, const SNAP_QUICK_IN* in, SNAP_SCRATCH* scr)
{
    SNAP c;
    if (!in) return 0;
    snap_open(&c, q, in, scr);
    return queued(&c, ch);
}

/* A cell starts a patch's scan when it is explored metal above the threshold and none of its W, NW, N and
   NE neighbours is: the first cell of its component that a row-major scan meets in its own run. The patch
   is reported only from the cell that is its anchor, the component's least (z, x), so each patch is
   reported once, from the row that holds its anchor. */
int tasim_snap_patches(const WORLDQ* q, int z0, int z1, SNAP_PATCH* out, int cap, uint32_t* flood_budget,
                       int* next_z, SNAP_SCRATCH* scr)
{
    SNAP c;
    int x, z, n = 0;
    snap_open(&c, q, NULL, scr);
    if (z0 < 0) z0 = 0;
    if (z1 > c.m.h) z1 = c.m.h;
    for (z = z0; z < z1; z++) {
        const int row = n;
        for (x = 0; x < c.m.w; x++) {
            SNAP_PATCH patch;
            unsigned cells;
            if (metal(&c, x, z) <= c.m.threshold || metal(&c, x - 1, z) > c.m.threshold ||
                metal(&c, x - 1, z - 1) > c.m.threshold || metal(&c, x, z - 1) > c.m.threshold ||
                metal(&c, x + 1, z - 1) > c.m.threshold) continue;
            if (*flood_budget < SNAP_PATCH_CAP || n >= cap) {
                *next_z = z;
                return row;
            }
            if (flood(&c, x, z, &patch, &cells) && patch.ax == x && patch.az == z) out[n++] = patch;
            *flood_budget -= cells;
        }
    }
    *next_z = z1 > z0 ? z1 : z0;
    return n;
}

/* QUICK BUILD'S CHOICE. Over a unit it offers only an upgrade: the unit must be a finished resource building
   of the seat's, and the replacement, centred on it, must have a strictly higher strength and fit once the
   old one is gone, or be queued there already. Over a reclaimable feature it offers nothing. Over ground,
   each type the selected builders' menus offer gets the legal snap and a mex a distinct deposit. Per kind
   the highest strength that is not blocked by a plan wins, the first in definition order on a tie; between
   a mex and a geothermal plant, the one whose centre is nearer the cursor. */
int tasim_snap_quick(const WORLDQ* q, const SNAP_QUICK_IN* in, SNAP_CHOICE* chosen, SNAP_SCRATCH* scr)
{
    SNAP c;
    WQ_UNIT target, u;
    WQ_DEF d;
    int i, t, n, ndefs, desired = 0;
    float old = 0;
    SNAP_CHOICE best[3];
    snap_open(&c, q, in, scr);
    memset(best, 0, sizeof best);
    memset(chosen, 0, sizeof *chosen);
    ndefs = q->ops->ndefs(q);
    if (ndefs > WQ_TYPES_MAX) ndefs = WQ_TYPES_MAX;
    if (in->target >= 0) {
        if (!q->ops->unit(q, in->target, &target) || !target.type || !target.finished ||
            !def(&c, target.type, &d) || !(desired = d.kind)) return 0;
        old = d.strength;
    } else if (in->feature_reclaimable) return 0;
    memset(scr->available, 0, (size_t)ndefs);
    for (i = 0; i < in->nbuilders; i++) {
        const int slot = in->builders[i];
        if (!q->ops->unit(q, slot, &u) || !u.type || !def(&c, u.type, &d) || !(d.flags & WQ_DF_BUILDER)) continue;
        if (in->target >= 0 && q->ops->can(q, slot, WQ_CAN_RECLAIM, in->target, in->raw) < 0) continue;
        n = q->ops->build_options(q, u.type, scr->opts, WQ_TYPES_MAX);
        while (n-- > 0) if (scr->opts[n] && scr->opts[n] < ndefs) scr->available[scr->opts[n]] = 1;
    }
    for (t = 1; t < ndefs; t++) if (scr->available[t]) {
        SNAP_CHOICE ch;
        int k, fx, fz;
        float value;
        memset(&ch, 0, sizeof ch);
        if (!def(&c, t, &d) || !(k = d.kind) || (desired && k != desired) || (value = d.strength) <= old) continue;
        if (best[k].type && value <= best[k].strength) continue;
        if (!footprint(&d, &fx, &fz)) continue;
        if (in->target >= 0) {
            ch.x = ((target.pos[0] >> 16) - fx * 8 + 8) / 16;
            ch.z = ((target.pos[2] >> 16) - fz * 8 + 8) / 16;
            ch.type = t; ch.kind = k; ch.target = in->target;
            /* the rule of site(): an upgrade already queued here is decided before the ground */
            if (!fits(&c, t, ch.x, ch.z, target.index, &ch.height)) {
                if (!site_known(&c, t, ch.x, ch.z) || !queued(&c, &ch)) continue;
                ch.height = q->ops->blocked_alt(q, t, ch.x, ch.z);
            }
        } else {
            if (!snapped(&c, t, in->raw, in->radius, 1, &ch)) continue;
            if (k == WQ_KIND_MEX && !distinct(&c, &ch, fx, fz)) continue;
        }
        ch.strength = value; ch.target = in->target >= 0 ? in->target : 0;
        ch.queued = queued(&c, &ch);
        if (!ch.queued && plans_block(&c, &ch)) continue;
        best[k] = ch;
    }
    *chosen = best[WQ_KIND_MEX].type ? best[WQ_KIND_MEX] : best[WQ_KIND_GEO];
    if (best[WQ_KIND_MEX].type && best[WQ_KIND_GEO].type) {
        int32_t pos[3], oldpos[3];
        int64_t dx, dz, d1, d2;
        tasim_snap_position(q, &best[WQ_KIND_GEO], pos);
        tasim_snap_position(q, &best[WQ_KIND_MEX], oldpos);
        dx = (pos[0] - (int64_t)in->raw[0]) >> 16; dz = (pos[2] - (int64_t)in->raw[2]) >> 16;
        d1 = dx * dx + dz * dz;
        dx = (oldpos[0] - (int64_t)in->raw[0]) >> 16; dz = (oldpos[2] - (int64_t)in->raw[2]) >> 16;
        d2 = dx * dx + dz * dz;
        if (d1 < d2) *chosen = best[WQ_KIND_GEO];
    }
    if (!chosen->type) return 0;
    chosen->queued = queued(&c, chosen);
    return 1;
}

/* the helper already guards one of the builders with this action */
static int existing_guard(const SNAP* c, int slot, int nprimary, int action)
{
    int n = c->q->ops->orders(c->q, slot, c->scr->list, WQ_LIST_MAX), k, j;
    for (k = 0; k < n; k++)
        for (j = 0; j < nprimary; j++)
            if (c->scr->list[k].action == action && c->scr->list[k].target == c->scr->primary[j]) return 1;
    return 0;
}

/* QUICK BUILD'S CLICK. A queued plan is cancelled by Shift where a selected builder can cancel it, and is
   left as it is otherwise. Else every selected builder whose menu offers the type (and that can reclaim the
   unit an upgrade replaces) is ordered: the reclaim of that unit first, then the build, through the
   new-order root -- appending with Shift or after the reclaim, replacing otherwise; the new-order root and
   not the Shift toggle, which would delete a different site within 16 world units. Each other selected
   helper guards one of them, shared out in turn, through the click root, unless with Shift it guards one
   already. Nothing is ordered when no builder qualifies or an action has no byte: the click is then the
   game's own. In a world whose lists have a bound (WQ_MAP.list_cap), a builder whose list cannot take all
   its new orders after it with Shift is given none of them, so an upgrade's reclaim never goes in without
   its build. */
int tasim_snap_commit(const WORLDQ* q, const SNAP_QUICK_IN* in, const SNAP_CHOICE* ch, SNAP_COMMIT_RES* res,
                      SNAP_SCRATCH* scr)
{
    SNAP c;
    WQ_UNIT u;
    WQ_DEF d;
    WQ_CMD cmd;
    int32_t pos[3];
    int i, j, np = 0, helper = 0;
    const int orders = ch->target ? 2 : 1;
    snap_open(&c, q, in, scr);
    memset(res, 0, sizeof *res);
    if (ch->queued) {
        if (in->shift && (ch->queued & SNAP_CANCEL)) {
            res->removed = unqueue(&c, ch);
            res->outcome = SNAP_UNQUEUED;
        } else res->outcome = SNAP_ALREADY;
        return res->outcome;
    }
    tasim_snap_position(q, ch, pos);
    for (i = 0; i < in->nbuilders; i++) {
        const int slot = in->builders[i];
        int act;
        if (!q->ops->unit(q, slot, &u) || !can_build(&c, &u, ch->type) ||
            (ch->target && q->ops->can(q, slot, WQ_CAN_RECLAIM, ch->target, pos) < 0)) continue;
        if (np >= SNAP_UNITS_MAX) return SNAP_NONE;
        def(&c, u.type, &d);
        act = q->ops->action(q, (d.flags & WQ_DF_VTOL) ? WQ_ACT_VTOL_MOBILEBUILD : WQ_ACT_MOBILEBUILD);
        if (act < 0) return SNAP_NONE;
        scr->primary[np] = (uint16_t)slot;
        scr->action[np] = (uint8_t)act;
        scr->reclaim[np] = (int16_t)(ch->target ? q->ops->can(q, slot, WQ_CAN_RECLAIM, ch->target, pos) : -1);
        np++;
    }
    if (!np) return SNAP_NONE;
    memset(&cmd, 0, sizeof cmd);
    cmd.verb = WQ_CMD_NEW;
    memcpy(cmd.pos, pos, sizeof pos);
    for (i = 0; i < np; i++) {
        cmd.unit = scr->primary[i];
        if (in->shift && c.m.list_cap &&
            q->ops->orders(q, cmd.unit, scr->list, WQ_LIST_MAX) + orders > c.m.list_cap) continue;
        if (ch->target) {
            cmd.action = (uint8_t)scr->reclaim[i];
            cmd.shift = (uint8_t)in->shift;
            cmd.target = (uint16_t)ch->target;
            cmd.type = 0;
            q->ops->order(q, &cmd);
        }
        cmd.action = scr->action[i];
        cmd.shift = (uint8_t)(in->shift || ch->target);
        cmd.target = (uint16_t)WQ_NONE;
        cmd.type = (uint16_t)ch->type;
        q->ops->order(q, &cmd);
    }
    cmd.verb = WQ_CMD_CLICK;
    cmd.type = 0;
    cmd.shift = (uint8_t)in->shift;
    for (i = 0; i < in->nbuilders; i++) {
        const int slot = in->builders[i];
        int main, guard;
        if (!q->ops->unit(q, slot, &u) || !can_assist(&c, &u)) continue;
        for (j = 0; j < np && slot != scr->primary[j]; j++) {}
        if (j < np) continue;
        main = scr->primary[helper++ % np];
        guard = q->ops->can(q, slot, WQ_CAN_GUARD, main, pos);
        if (guard < 0) continue;
        if (in->shift && existing_guard(&c, slot, np, guard)) continue;
        cmd.unit = (uint16_t)slot;
        cmd.action = (uint8_t)guard;
        cmd.target = (uint16_t)main;
        q->ops->order(q, &cmd);
        res->helpers++;
    }
    res->outcome = SNAP_ORDERED;
    res->built = np;
    return SNAP_ORDERED;
}
