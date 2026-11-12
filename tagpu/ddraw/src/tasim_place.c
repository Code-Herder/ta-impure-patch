/* tasim_place.c -- the placement gesture's rules over WORLDQ (tasim_place.h). research/notes/modules/placement.md
   states them; tagpu_place.c holds the gesture, its hooks and its colours. */
#include <stdint.h>
#include <string.h>
#include "tasim_place.h"

/* The order position of a site: 0x419670's (0x419704..0x419717), wrapping as its `shl` does. */
static int32_t site_x(int fx, const PLACE_SITE* s) { return (int32_t)((uint32_t)(2 * s->cx + fx) << 19); }
static int32_t site_z(int fz, const PLACE_SITE* s) { return (int32_t)((uint32_t)(2 * s->cz + fz) << 19); }

/* The footprint's first cell for a 16.16 centre: 0x4197D0's rounding, an arithmetic shift as its `sar`
   (0x419817..0x41983D). */
static int origin(int32_t centre, int foot)
{
    return (int)((uint32_t)centre - ((uint32_t)foot << 19) + 0x80000u) >> 20;
}

static int footprint(const WORLDQ* q, int type, int* fx, int* fz)
{
    WQ_DEF d;
    if (!q->ops->def(q, type, &d)) return 0;
    *fx = d.fx; *fz = d.fz;
    return *fx > 0 && *fz > 0 && *fx <= WQ_FOOT_MAX && *fz <= WQ_FOOT_MAX;
}

/* THE BUILDER SET is 0x419670's -- the seat's selected units (+0x110 bit 4) of a builder type (+0x241 bit
   6), each with the build action of its kind, VTOL_MOBILEBUILD for a type with +0x241 bit 11
   (0x419755..0x41979F) -- narrowed to units that are alive, not excluded and the seat's, which 0x419670 does
   not test, and to a build action the order table has. The build menu is not consulted: 0x419670 does not
   consult it either. */
unsigned tasim_place_builders(const WORLDQ* q, const PLACE_IN* in, PLACE_SCRATCH* scr)
{
    int i;
    scr->nb = 0;
    for (i = 0; i < in->nselected && scr->nb < PLACE_BUILDERS_MAX; i++) {
        WQ_UNIT u;
        WQ_DEF d;
        int act;
        if (!q->ops->unit(q, in->selected[i], &u) || !(u.state & 0x10u) || !u.type || !q->ops->def(q, u.type, &d) ||
            !(d.flags & WQ_DF_BUILDER)) continue;
        if ((act = q->ops->action(q, (d.flags & WQ_DF_VTOL) ? WQ_ACT_VTOL_MOBILEBUILD : WQ_ACT_MOBILEBUILD)) <= 0)
            continue;
        scr->slot[scr->nb] = in->selected[i];
        scr->action[scr->nb] = (uint8_t)act;
        scr->nb++;
    }
    return scr->nb;
}

unsigned tasim_place_budget(unsigned n, unsigned builders)
{
    unsigned most = PLACE_ORDER_BUDGET / (builders ? builders : 1);
    return n < most ? n : most;
}

int tasim_place_snapshot(const WORLDQ* q, PLACE_SCRATCH* scr)
{
    unsigned b;
    scr->nplan = 0;
    for (b = 0; b < scr->nb; b++) {
        int k, n = q->ops->orders(q, scr->slot[b], scr->list, WQ_LIST_MAX);
        for (k = 0; k < n; k++) {
            const WQ_ORDER* o = &scr->list[k];
            const int match = o->action == scr->action[b];
            PLACE_PLAN* p;
            if (!match && !o->build) continue;
            if (scr->nplan >= PLACE_PLANS) return scr->planned = 0;
            scr->order[scr->nplan].x = o->pos[0];
            scr->order[scr->nplan].z = o->pos[2];
            scr->order[scr->nplan].match = (uint8_t)match;
            scr->by[scr->nplan] = PLACE_NO_SITE;
            p = &scr->plan[scr->nplan++];
            p->type = o->build ? o->type : 0;
            p->b = (uint16_t)b;
            p->place = o->place;
            p->build = o->build;
            p->flags = o->flags;
        }
    }
    return scr->planned = 1;
}

/* The plans of builder `b` are plan[*first .. return): the snapshot keeps them contiguous and in builder
   order, so a caller walking b upwards passes the previous end as `from`. */
static unsigned plans_of(const PLACE_SCRATCH* scr, unsigned b, unsigned from, unsigned* first)
{
    unsigned end;
    while (from < scr->nplan && scr->plan[from].b < b) from++;
    for (end = from; end < scr->nplan && scr->plan[end].b == b; end++) {}
    *first = from;
    return end;
}

int tasim_place_queued_at(const WORLDQ* q, int px, int pz, int* tx, int* tz, int* tw, int* th, PLACE_SCRATCH* scr)
{
    unsigned b;
    for (b = 0; b < scr->nb; b++) {
        int k, n = q->ops->orders(q, scr->slot[b], scr->list, WQ_LIST_MAX);
        for (k = 0; k < n; k++) {
            const WQ_ORDER* o = &scr->list[k];
            int ox, oz;
            if (!o->build || !footprint(q, o->type, tw, th)) continue;
            ox = origin(o->pos[0], *tw);
            oz = origin(o->pos[2], *th);
            if (px >= ox && px < ox + *tw && pz >= oz && pz < oz + *th) {
                *tx = ox;
                *tz = oz;
                return 1;
            }
        }
    }
    return 0;
}

/* A site a stock Shift-click would toggle off instead of placing: some builder has an order of its own
   build action within 16 world units of the site's order position (0x43AFC0's first match; it compares no
   build type). */
static int toggles(const PLACE_SCRATCH* scr, int fx, int fz, const PLACE_SITE* s)
{
    const int32_t x = site_x(fx, s), z = site_z(fz, s);
    unsigned i;
    for (i = 0; i < scr->nplan; i++)
        if (scr->order[i].match && place_within(x, z, scr->order[i].x, scr->order[i].z)) return 1;
    return 0;
}

/* THE TAKE-OFF RULE, the one place a site's action is decided: the preview's states and the commit's
   removals and appends are both read from what this leaves -- each plan's `by` and each site's state -- so
   for the same queues the preview shows what the release does.
   A ring takes nothing off: a site a stock Shift-click would toggle takes one, and the caller leaves it out.
   Every other shape is TA's Shift-click per site, judged against each builder's queue as it stood before
   the gesture: place_take_offs_one (tagpu_placegeo.h) per builder, over its run of the snapshot, with the
   red sites refused. A site that takes an order off from some builder is PLACE_SITE_TAKE. */
static void take_offs(PLACE_SCRATCH* scr, int fx, int fz, PLACE_SITE* s, unsigned n, int ring)
{
    PlaceSitePos pos[PLACE_MAX_SITES];
    uint8_t took[PLACE_MAX_SITES];
    unsigned i, p, first, end, b;
    for (p = 0; p < scr->nplan; p++) scr->by[p] = PLACE_NO_SITE;
    if (ring) {
        for (i = 0; i < n; i++)
            if (s[i].state != PLACE_SITE_RED && toggles(scr, fx, fz, &s[i])) s[i].state = PLACE_SITE_TAKE;
        return;
    }
    /* a site turns only from free to take below, never to or from red, so the refused set holds for every
       builder */
    for (i = 0; i < n; i++) {
        pos[i].x = site_x(fx, &s[i]);
        pos[i].z = site_z(fz, &s[i]);
        pos[i].refused = s[i].state == PLACE_SITE_RED;
    }
    for (b = end = 0; b < scr->nb; b++) {
        end = plans_of(scr, b, end, &first);
        place_take_offs_one(scr->order + first, end - first, pos, n, scr->by + first, took);
        for (i = 0; i < n; i++)
            if (took[i]) s[i].state = PLACE_SITE_TAKE;
    }
}

/* Each site judged by the instance's site test, as a single click there would be (the game opens its view
   with the build cursor's own test), at the altitude the build cursor gives it: the test's for a free site,
   blocked_alt for a refused one (0x4198EC..0x419909). Then the take-off rule. */
unsigned tasim_place_classify(const WORLDQ* q, const PLACE_IN* in, PLACE_SITE* sites, unsigned n, PLACE_SCRATCH* scr)
{
    unsigned i, red = 0;
    int fx, fz;
    if (n > PLACE_MAX_SITES) n = PLACE_MAX_SITES;
    if (!footprint(q, in->type, &fx, &fz)) fx = fz = 0;
    for (i = 0; i < n; i++) {
        int alt = 0, ok = fx && q->ops->site_ok(q, in->type, sites[i].cx, sites[i].cz, 0, &alt);
        sites[i].alt = ok ? alt : q->ops->blocked_alt(q, in->type, sites[i].cx, sites[i].cz);
        sites[i].state = ok && scr->planned ? PLACE_SITE_FREE : PLACE_SITE_RED;
        red += sites[i].state == PLACE_SITE_RED;
    }
    take_offs(scr, fx, fz, sites, n, in->ring);
    return red;
}

/* PHASE 1, per builder: the orders the take-off rule gave a site (plans [first, end) of builder `b`) are
   taken off, and each of those sites adds nothing for this builder. An order carrying WQ_OF_SECOND belongs on the
   second list: it is taken but not removed, as 0x43AFC0 would look for it there and find nothing
   (0x43B034..0x43B058). THE ORDERS GO IN DESCENDING PLACE: a removal moves none of the places still to come,
   so each order is found again at the place the snapshot copied it from, and the head goes last. The
   instance removes an order only while its place still holds the copied action, position and flags; a
   refusal counts in `*stale` (the queue changed inside this commit), and the order stays queued. */
static unsigned remove_taken(const WORLDQ* q, PLACE_SCRATCH* scr, unsigned b, unsigned first, unsigned end,
                             uint8_t* toggled, unsigned* stale)
{
    unsigned p, removed = 0;
    for (p = first; p < end; p++)
        if (scr->by[p] != PLACE_NO_SITE) toggled[scr->by[p]] = 1;
    for (p = end; p-- > first;) {
        const PLACE_PLAN* plan = &scr->plan[p];
        WQ_CMD cmd;
        if (scr->by[p] == PLACE_NO_SITE || (plan->flags & WQ_OF_SECOND)) continue;
        memset(&cmd, 0, sizeof cmd);
        cmd.verb = WQ_CMD_REMOVE;
        cmd.unit = scr->slot[b];
        cmd.target = (uint16_t)WQ_NONE;
        cmd.expect.place = plan->place;
        cmd.expect.action = scr->action[b];
        cmd.expect.pos[0] = scr->order[p].x;
        cmd.expect.pos[2] = scr->order[p].z;
        cmd.expect.flags = plan->flags;
        if (q->ops->order(q, &cmd) == WQ_OK) removed++;
        else ++*stale;
    }
    return removed;
}

/* PHASE 2, per builder: the sites of `order` that are neither red nor toggled for it are appended through
   the new-order root with Shift (append, never replace), at the position and altitude 0x419670 gives,
   while its list holds fewer than PLACE_LIST_CAP orders -- counted once before, then one more per append;
   0x43ADC0 only ever drops head nodes on the way (WQ_OF_DROP), so the count never runs short. `*capped` is set
   when the cap stops it. A refused order (the builder no longer the seat's) ends the builder's appends. */
static unsigned append_sites(const WORLDQ* q, const PLACE_IN* in, PLACE_SCRATCH* scr, unsigned b, int fx, int fz,
                             const PLACE_SITE* s, const uint16_t* order, unsigned count, const uint8_t* toggled,
                             int* capped)
{
    WQ_CMD cmd;
    unsigned i, issued = 0;
    int length = q->ops->orders(q, scr->slot[b], scr->list, WQ_LIST_MAX);
    if (length < 0) return 0;
    memset(&cmd, 0, sizeof cmd);
    cmd.verb = WQ_CMD_NEW;
    cmd.action = scr->action[b];
    cmd.shift = 1;
    cmd.unit = scr->slot[b];
    cmd.target = (uint16_t)WQ_NONE;
    cmd.type = (uint16_t)in->type;
    for (i = 0; i < count; i++) {
        const PLACE_SITE* t = &s[order[i]];
        if (t->state == PLACE_SITE_RED || toggled[order[i]]) continue;
        if (length >= PLACE_LIST_CAP) { *capped = 1; break; }
        cmd.pos[0] = site_x(fx, t);
        cmd.pos[1] = (int32_t)((uint32_t)t->alt << 16);
        cmd.pos[2] = site_z(fz, t);
        if (q->ops->order(q, &cmd) != WQ_OK) break;
        issued++;
        length++;
    }
    return issued;
}

/* The commit, TA's semantics: the gesture is the stock Shift-clicks of its sites, judged against each
   builder's queue as it stood before the gesture. Two phases rather than N literal clicks, because
   0x43AFC0's inclusive 16 units would let a site toggle off the order the previous site of the same gesture
   added (a line of 1x1 sites would alternate add and remove); here a site only ever removes an order queued
   before the gesture. A ring removes nothing: its toggling sites are left out. The sites are judged afresh
   -- the preview is never trusted -- by the same classify and take-off rule the preview's states come
   from. */
void tasim_place_commit(const WORLDQ* q, const PLACE_IN* in, PLACE_SITE* sites, unsigned n, const uint16_t* order,
                        PLACE_COMMIT_RES* res, PLACE_SCRATCH* scr)
{
    uint8_t toggled[PLACE_MAX_SITES];
    unsigned b, i, first, end;
    int fx, fz;
    memset(res, 0, sizeof *res);
    if (n > PLACE_MAX_SITES) n = PLACE_MAX_SITES;
    if (!footprint(q, in->type, &fx, &fz)) return;
    tasim_place_builders(q, in, scr);
    res->planned = tasim_place_snapshot(q, scr);
    res->red = tasim_place_classify(q, in, sites, n, scr);
    for (b = end = 0; b < scr->nb; b++) {
        int capped = 0;
        memset(toggled, 0, sizeof toggled);
        end = plans_of(scr, b, end, &first);
        if (in->ring) {
            for (i = 0; i < n; i++) toggled[i] = sites[i].state == PLACE_SITE_TAKE;
        } else res->removed += remove_taken(q, scr, b, first, end, toggled, &res->stale);
        res->issued += append_sites(q, in, scr, b, fx, fz, sites, order, n, toggled, &capped);
        res->capped_builders += (unsigned)capped;
    }
}
