/* impai_maps.c -- the reach labels and the maps a seat's knowledge derives (impai.h). Engine-free: every input
   is an argument, so the maps cannot read state the recorder does not carry. */
#include <string.h>
#include "impai.h"
#include "tagpu_simreg.h"

/* The reach labels, the threat rows, the maps and their digests. RUNS: the reach labels and the threat rows
   are content every level init builds, live or puppet, and the overlay derives the maps on a puppet from
   the recorded knowledge, so a puppet's are the live game's; the brain and the military derive them too,
   on a live seat's turn only. */
SIM_FEATURE(impai_maps, SIM_PUPPET_RUNS, "impai-observe impai-observe-perm stock-impai-observe impai-eco impai-fight impai-naval stock-impai-naval");

/* ---- reach ---- */

size_t impai_reach_work(int w, int h)
{
    return w > 0 && h > 0 ? (size_t)w * (size_t)h * 2u * sizeof(uint32_t) : 0;
}

/* BOUND: every cell is queued once, when it is labelled, so the queue holds w x h and the work is O(w x h)
   per class. A label past IMPAI_REACH_MANY - 1 is stored as IMPAI_REACH_MANY: "some component" rather than a
   wrong one. */
int impai_reach_label(IMPAI_PASSFN fn, const void* ctx, int mclass, int w, int h, uint16_t* labels, void* work)
{
    uint32_t* cell = (uint32_t*)work;
    uint32_t* queue = cell + (size_t)w * (size_t)h;
    uint32_t next = 0;
    int x, z, bx, bz;
    if (w < 2 || h < 2 || !work) return 0;
    memset(cell, 0, (size_t)w * (size_t)h * sizeof *cell);
    for (z = 0; z < h; z++)
        for (x = 0; x < w; x++) {
            size_t head = 0, tail = 0;
            if (cell[(size_t)z * w + x] || !fn(ctx, mclass, x, z)) continue;
            next++;
            cell[(size_t)z * w + x] = next;
            queue[tail++] = (uint32_t)z * (uint32_t)w + (uint32_t)x;
            while (head < tail) {
                const uint32_t k = queue[head++];
                const int cx = (int)(k % (uint32_t)w), cz = (int)(k / (uint32_t)w);
                static const int DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
                int d;
                for (d = 0; d < 4; d++) {
                    const int nx = cx + DX[d], nz = cz + DZ[d];
                    size_t at;
                    if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                    at = (size_t)nz * w + nx;
                    if (cell[at] || !fn(ctx, mclass, nx, nz)) continue;
                    cell[at] = next;
                    queue[tail++] = (uint32_t)at;
                }
            }
        }
    for (bz = 0; bz < h / 2; bz++)
        for (bx = 0; bx < w / 2; bx++) {
            uint32_t l = 0;
            int k;
            for (k = 0; k < 4 && !l; k++) l = cell[(size_t)(2 * bz + k / 2) * w + 2 * bx + k % 2];
            labels[(size_t)bz * (w / 2) + bx] = (uint16_t)(l < IMPAI_REACH_MANY ? l : IMPAI_REACH_MANY);
        }
    return (int)next;
}

void impai_reach_top(IMPAI_TOPFN fn, const void* ctx, int w, int h, uint8_t* top)
{
    int bx, bz, k;
    for (bz = 0; bz < h / 2; bz++)
        for (bx = 0; bx < w / 2; bx++) {
            int most = 0;
            for (k = 0; k < 4; k++) {
                int v = fn(ctx, 2 * bx + k % 2, 2 * bz + k / 2);
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                if (v > most) most = v;
            }
            top[(size_t)bz * (w / 2) + bx] = (uint8_t)most;
        }
}

int impai_ring_at(int r, int i, int* dx, int* dz)
{
    /* a side slot's offset as k and r times these: (-k,-r) (k,-r) (-r,-k) (r,-k) (-r,k) (r,k) (-k,r) (k,r) */
    static const struct { int8_t kx, rx, kz, rz; } SIDE[8] = {
        { -1, 0, 0, -1 }, { 1, 0, 0, -1 }, { 0, -1, -1, 0 }, { 0, 1, -1, 0 },
        { 0, -1, 1, 0 },  { 0, 1, 1, 0 },  { -1, 0, 0, 1 },  { 1, 0, 0, 1 } };
    static const int8_t AXIS[4][2] = { { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 } };
    static const int8_t CORNER[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    int j;
    if (r < 0 || i < 0) return 0;
    if (r == 0) {
        *dx = *dz = 0;
        return i == 0;
    }
    if (i < 4) {
        *dx = AXIS[i][0] * r;
        *dz = AXIS[i][1] * r;
        return 1;
    }
    j = i - 4;
    if (j < 8 * (r - 1)) {
        const int k = 1 + j / 8;
        *dx = SIDE[j % 8].kx * k + SIDE[j % 8].rx * r;
        *dz = SIDE[j % 8].kz * k + SIDE[j % 8].rz * r;
        return 1;
    }
    j -= 8 * (r - 1);
    if (j >= 4) return 0;
    *dx = CORNER[j][0] * r;
    *dz = CORNER[j][1] * r;
    return 1;
}

/* the class's labels, NULL when the level has none for it */
static const uint16_t* class_labels(const IMPAI_REACH* r, unsigned c)
{
    if (!r || !r->labels || c >= WQ_MCLASSES || !(r->present >> c & 1u)) return NULL;
    return r->labels + r->at[c];
}

int impai_reach_at(const IMPAI_REACH* r, unsigned c, int bx, int bz)
{
    const uint16_t* l = class_labels(r, c);
    if (!l) return -1;
    if (bx < 0 || bz < 0 || bx >= r->bw || bz >= r->bh) return 0;
    return l[(size_t)bz * r->bw + bx];
}

/* WHY THE FIRST CELL: the class map holds a footprint at the cell it starts at, so the cell a unit stands on as its
   class sees it is its first cell. A unit's centre lies up to half its footprint past it, in another block whenever
   the centre is within that of a block's edge, and that block can have no passable cell for the class at all -- a
   feature's footprint beside it, folded in with its ring. MEASURED on ProTA's bar-nota-pincushion-ac-2 at 57d39d71:
   the commander stopped at (3354, 5856), its first cell (209, 365) in block (104, 182), its centre in block
   (104, 183), whose four cells a feature anchored at (204, 367) covers or rings [INFERRED: no passable cell, from
   the feature grid]; every economy rule that reads the builder's label turned it down for the 37 139 ticks to the
   game's end, 89 % of its idle decisions. The centre's block stands in when the first cell's has no label: a unit
   the map changed under.
   THE LABEL BESIDE: where neither block carries a label -- the labels are made once, at the level init, so a unit
   standing where a feature stood then, since reclaimed or destroyed, stands in a hole of them --, the one label the
   eight blocks round the first cell's block carry stands in; 0 when they carry none, or two. MEASURED on Trout
   Farm's south start (bar-hota-troutfarm-aa-1 at ecf62769, a diagnostic build logging each offer of the sea plant;
   the level's class maps from tools/tasim/classcheck.py's inputs): the level's Truck08 feature is anchored at cell
   (223, 434), 5 x 3 cells, and class 2's map holds no entry for cells 222..227 x 433..436, its footprint and the
   class's ring. The commander stood at (3584..3599, 6960..6975) px from GameTime 52 300 to the cap, the truck gone
   [INFERRED: reclaimed], its first cell's block (111 or 112, 217) and its centre's (112, 217) with no label
   [INFERRED: the level init's labels, from the core's class map]; the passable ring round the hole is one component
   with an entry 11 px from the ARMSY site. Each of the shipyard's offers to it from 52 207 to the cap was refused by
   THE BUILDER'S REACH (impai_build.c), the seat built no sea army against the stock AI's last base on the lake, and
   the game reached the cap ahead, 78 622 against 56 805. RESIDUAL: a block carries its first passable cell's
   label, so the one label beside can be another component than the cells round the unit -- a ring block holding
   two, its first cell of the other --; a hole wider than the blocks round the first cell's block, all eight of
   them unlabelled, reads 0; and a unit between two components reaches nothing. BOUND: nine more label reads. */
int impai_reach_unit(const IMPAI_REACH* r, unsigned c, int32_t x, int32_t z, int fx, int fz)
{
    const int32_t cx = (int32_t)(((int64_t)x - ((int64_t)fx * 0x80000) + 0x80000) >> 20);
    const int32_t cz = (int32_t)(((int64_t)z - ((int64_t)fz * 0x80000) + 0x80000) >> 20);
    int l = impai_reach_at(r, c, cx >> 1, cz >> 1), k, got = 0;
    if (l || (l = impai_reach_at(r, c, x >> 21, z >> 21)) != 0) return l;
    for (k = 0; k < 9; k++) {
        const int m = impai_reach_at(r, c, (cx >> 1) + k % 3 - 1, (cz >> 1) + k / 3 - 1);
        if (m <= 0 || m == got) continue;
        if (got) return 0;
        got = m;
    }
    return got;
}

int impai_reach_near(const IMPAI_REACH* r, unsigned c, uint16_t label, int bx, int bz, int radius, int* ox, int* oz)
{
    const uint16_t* l = class_labels(r, c);
    int ring, i, dx, dz;
    if (!l) return 0;
    for (ring = 0; ring <= radius; ring++)
        for (i = 0; impai_ring_at(ring, i, &dx, &dz); i++) {
            const int x = bx + dx, z = bz + dz;
            if (x < 0 || z < 0 || x >= r->bw || z >= r->bh || l[(size_t)z * r->bw + x] != label) continue;
            *ox = x;
            *oz = z;
            return 1;
        }
    return 0;
}

static int sea_army(const IMPAI_TINFO* t) { return t->klass >= IMPAI_K_SRAIDER && t->klass <= IMPAI_K_SAA; }

static int land_army(const IMPAI_TINFO* t)
{
    return t->klass <= IMPAI_K_LSCOUT && (t->flags & IMPAI_TF_MOBILE) &&
           !(t->flags & IMPAI_TF_VTOL);
}

void impai_reach_summary(IMPAI_REACH* r, const IMPAI_TINFO* ti, unsigned nti)
{
    uint32_t sea_n[WQ_MCLASSES], sea_classes = 0;
    unsigned t, c;
    size_t b, blocks;
    memset(sea_n, 0, sizeof sea_n);
    memset(r->land_w, 0, sizeof r->land_w);
    r->land_total = 0;
    r->sea_blocks = 0;
    r->sea_ref = 0xFF;
    for (t = 1; ti && t < nti; t++) {
        const unsigned m = ti[t].mclass;
        if (!class_labels(r, m)) continue;
        if (land_army(&ti[t]) && r->land_w[m] < 0xFFFF) {
            r->land_w[m]++;
            r->land_total++;
        }
        if (sea_army(&ti[t])) {
            sea_n[m]++;
            sea_classes |= 1u << m;
        }
    }
    for (c = 0; c < WQ_MCLASSES; c++)
        if (sea_n[c] && (r->sea_ref == 0xFF || sea_n[c] > sea_n[r->sea_ref])) r->sea_ref = (uint8_t)c;
    blocks = (size_t)r->bw * r->bh;
    for (b = 0; sea_classes && b < blocks; b++)
        for (c = 0; c < WQ_MCLASSES; c++)
            if ((sea_classes >> c & 1u) && r->labels[r->at[c] + b]) {
                r->sea_blocks++;
                break;
            }
}

int impai_mostly_water(const IMPAI_REACH* r)
{
    return 2u * (uint64_t)r->sea_blocks > (uint64_t)r->bw * r->bh;
}

int impai_land_link(const IMPAI_REACH* r, int ax, int az, int bx, int bz)
{
    uint64_t linked = 0;
    unsigned c;
    if (!r || !r->labels || !r->land_total || ax < 0 || az < 0 || bx < 0 || bz < 0 || ax >= r->bw || az >= r->bh ||
        bx >= r->bw || bz >= r->bh)
        return 1;
    for (c = 0; c < WQ_MCLASSES; c++) {
        const uint16_t* l = r->land_w[c] ? class_labels(r, c) : NULL;
        uint16_t la;
        if (!l) continue;
        la = l[(size_t)az * r->bw + ax];
        if (la && la == l[(size_t)bz * r->bw + bx]) linked += r->land_w[c];
    }
    return 2u * linked >= r->land_total;
}

int impai_sea_labels(const IMPAI_REACH* r, int bx, int bz, int radius, uint16_t* out, int cap)
{
    const uint16_t* l = r && r->sea_ref < WQ_MCLASSES ? class_labels(r, r->sea_ref) : NULL;
    int ring, i, dx, dz, n = 0, k;
    if (!l) return 0;
    for (ring = 0; ring <= radius && n < cap; ring++)
        for (i = 0; n < cap && impai_ring_at(ring, i, &dx, &dz); i++) {
            const int x = bx + dx, z = bz + dz;
            uint16_t v;
            if (x < 0 || z < 0 || x >= r->bw || z >= r->bh || !(v = l[(size_t)z * r->bw + x])) continue;
            for (k = 0; k < n && out[k] != v; k++) {}
            if (k == n) out[n++] = v;
        }
    return n;
}

/* ---- the threat table ---- */

/* a value rounded once to an integer field: half up, held to 0..max */
static uint32_t round_to(double v, uint32_t max)
{
    if (!(v > 0.0)) return 0;
    if (v >= (double)max) return max;
    return (uint32_t)(v + 0.5);
}

/* THE GROUND REACH: what the type can hit on the ground by the engine's own targeting. The target finder 0x40B7B0
   keeps a best inside the slot's bad-target category (+0x231) and one outside, and returns the inside one when
   there is no other (0x40B9D8..0x40BA2D, 0x40BA4D..0x40BA7D, DISASSEMBLED 2026-11-14), so a NOTAIR slot shoots
   ground units when no aircraft is a candidate; a toairweapon never does: the finder's can-engage call 0x49ABB0
   (0x40B914) refuses it a target whose +0x110 & 3 is not 2 (0x49AD07..0x49AD1C). MEASURED 2026-11-13, Comet
   Catcher: ARMSAM, NOTAIR, killed 26 of the 27 ImpAI constructors a human's line took. The role classifier's
   dps_ground leaves those slots out: a role says what a type is for, this row what it hits; the fight
   predictor's hurts() has the same rule (impai_fight.c).
   Over the type's first min(nweapons, TASIM_ROLE_WEAPONS) slots, every regular one (tasim_roles.c's `regular`:
   no commandfire, stockpile or interceptor) whose weapon lacks toairweapon, its slot's NOTAIR notwithstanding:
   hit_ground the sum of tasim_roles.c's per-slot `dps` of the [DAMAGE] default, x 30 x 16, rounded once and
   held to 0..0xFFFF; hit_range the largest such range, held the same. A slot that is not anti-air is in the
   classifier's dps_ground by the same formula, in the same order, so for a type with no anti-air slot the two
   rows agree exactly (tests/impai_brain_test.c, check_threat_reach). */
static int reach_slot(const TASIM_ROLE_WEAPON* w)
{
    return !(w->flags & (TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | TASIM_WF_INTERCEPTOR | TASIM_WF_TOAIR));
}

static void ground_reach(const TASIM_ROLE_IN* u, IMPAI_THREAT* t)
{
    const int n = u->nweapons < TASIM_ROLE_WEAPONS ? u->nweapons : TASIM_ROLE_WEAPONS;
    double dps = 0.0;
    int32_t range = 0;
    int k;
    for (k = 0; k < n; k++) {
        const TASIM_ROLE_WEAPON* w = &u->w[k];
        if (!reach_slot(w)) continue;
        if (w->reload_ticks > 0.0f)
            dps += (double)w->damage_default * (double)(w->burst > 1 ? w->burst : 1) / (double)w->reload_ticks;
        if (w->range > range) range = w->range;
    }
    t->hit_ground = (uint16_t)round_to(dps * 30.0 * 16.0, 0xFFFFu);
    t->hit_range = (uint16_t)(range > 0xFFFF ? 0xFFFF : range);
}

void impai_threat_from_roles(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* out, int n, IMPAI_THREAT* t)
{
    int i;
    for (i = 0; i < n; i++) {
        memset(&t[i], 0, sizeof t[i]);
        t[i].dps_ground = (uint16_t)round_to(out[i].dps_ground * 30.0 * 16.0, 0xFFFFu);
        t[i].dps_air = (uint16_t)round_to(out[i].dps_air * 30.0 * 16.0, 0xFFFFu);
        t[i].dps_water = (uint16_t)round_to(out[i].dps_water * 30.0 * 16.0, 0xFFFFu);
        t[i].range = (uint16_t)(out[i].range_ground <= 0 ? 0 : out[i].range_ground > 0xFFFF ? 0xFFFF : out[i].range_ground);
        t[i].value = round_to(out[i].value, 0xFFFFFFFFu);
        t[i].layer = (uint8_t)out[i].layer;
        t[i].flags = (uint8_t)out[i].flags;
        ground_reach(&in[i], &t[i]);
    }
}

/* ---- the maps ---- */

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* Every cell whose centre lies within r px of (cx, cz) gains d, row by row through the difference array: a
   row's span from the disc's half-width there, exact in integers. */
static void stamp_disc(const IMPAI_MAPS* m, int cx, int cz, int r, uint32_t d)
{
    const int64_t r2 = (int64_t)r * r;
    int gz;
    for (gz = 0; gz < m->gh; gz++) {
        const int64_t dz = (int64_t)gz * IMPAI_CELL_PX + IMPAI_CELL_PX / 2 - cz;
        int half, x0, x1;
        if (dz * dz > r2) continue;
        half = (int)isqrt64((uint64_t)(r2 - dz * dz));
        x0 = floor_div(cx - half - IMPAI_CELL_PX / 2 + IMPAI_CELL_PX - 1, IMPAI_CELL_PX);
        x1 = floor_div(cx + half - IMPAI_CELL_PX / 2, IMPAI_CELL_PX);
        if (x0 < 0) x0 = 0;
        if (x1 >= m->gw) x1 = m->gw - 1;
        if (x0 > x1) continue;
        m->diff[gz * (IMPAI_GRID_MAX + 1) + x0] += (int32_t)d;
        m->diff[gz * (IMPAI_GRID_MAX + 1) + x1 + 1] -= (int32_t)d;
    }
}

static void diff_into(const IMPAI_MAPS* m, uint32_t* out)
{
    int gx, gz;
    for (gz = 0; gz < m->gh; gz++) {
        int64_t run = 0;
        for (gx = 0; gx < m->gw; gx++) {
            run += m->diff[gz * (IMPAI_GRID_MAX + 1) + gx];
            out[gz * m->gw + gx] = run <= 0 ? 0 : run >= 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)run;
        }
    }
}

/* the cell of a 32-px block's centre */
static int cell_of(int b) { return (b * 32 + 16) / IMPAI_CELL_PX; }

int impai_maps_build(const IMPAI_VIEW* v, IMPAI_MAPS* m, uint16_t radar_as)
{
    const int seat = v->seat;
    int i, layer, gx, gz;
    m->gw = (v->w * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX;
    m->gh = (v->h * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX;
    m->threat_built = v->threat != NULL;
    if (m->gw < 1 || m->gh < 1 || m->gw > IMPAI_GRID_MAX || m->gh > IMPAI_GRID_MAX || seat < 0 || seat >= WQ_SEATS)
        return 0;
    for (layer = 0; layer < IMPAI_THREATS; layer++) memset(m->threat[layer], 0, (size_t)m->gw * m->gh * sizeof(uint32_t));
    memset(m->influence, 0, (size_t)m->gw * m->gh * sizeof(int32_t));
    memset(m->presence, 0, (size_t)m->gw * m->gh * sizeof(uint16_t));
    /* presence: enemy rows the seat holds by its radar alone, or as ghosts */
    for (i = 1; i < v->nrows; i++) {
        const IMPAI_SEEN* r = &v->rows[i].seat[seat];
        const int enemy = r->owner == 0xFF || (r->owner != seat && !(v->allies >> r->owner & 1u));
        if (!r->how || !enemy || (r->how & IMPAI_NOW)) continue;
        gx = cell_of(r->bx); gz = cell_of(r->bz);
        if (gx < m->gw && gz < m->gh && m->presence[gz * m->gw + gx] < 0xFFFF) m->presence[gz * m->gw + gx]++;
    }
    if (!v->threat) return 1;
    /* threat, one layer at a time: each identified enemy row's type stamps its dps over its range, on the ground
       layer its ground reach (impai_threat_from_roles, THE GROUND REACH), and there each contact never identified
       stamps radar_as's -- the two tests exclude each other (ident), so no row stamps twice */
    for (layer = 0; layer < IMPAI_THREATS; layer++) {
        memset(m->diff, 0, (size_t)(IMPAI_GRID_MAX + 1) * IMPAI_GRID_MAX * sizeof(int32_t));
        for (i = 1; i < v->nrows; i++) {
            const IMPAI_SEEN* r = &v->rows[i].seat[seat];
            const IMPAI_THREAT* t;
            uint32_t d;
            if (layer == IMPAI_THREAT_GROUND && radar_as < (unsigned)v->ntypes &&
                impai_row_unidentified_now(r, (unsigned)v->ntypes)) {
                t = &v->threat[radar_as];
                if (t->hit_ground) stamp_disc(m, r->bx * 32 + 16, r->bz * 32 + 16, t->hit_range, t->hit_ground);
                continue;
            }
            if (!r->how || r->owner == 0xFF || r->owner == seat || (v->allies >> r->owner & 1u) ||
                r->type == WQ_NONE || r->type >= v->ntypes)
                continue;
            t = &v->threat[r->type];
            if (layer == IMPAI_THREAT_GROUND) {
                if (t->hit_ground) stamp_disc(m, r->bx * 32 + 16, r->bz * 32 + 16, t->hit_range, t->hit_ground);
                continue;
            }
            d = layer == IMPAI_THREAT_AIR ? t->dps_air : t->dps_water;
            if (d) stamp_disc(m, r->bx * 32 + 16, r->bz * 32 + 16, t->range, d);
        }
        diff_into(m, m->threat[layer]);
    }
    /* influence: own value minus identified enemies' value per cell, held to int32 in the scratch, then each
       cell's 3 x 3 box summed in 64 bits and held again. BOUND: nine reads a cell, gw x gh cells. */
    {
        memset(m->diff, 0, (size_t)(IMPAI_GRID_MAX + 1) * IMPAI_GRID_MAX * sizeof(int32_t));
        for (i = 1; i < v->nrows; i++) {
            const IMPAI_SEEN* r = &v->rows[i].seat[seat];
            int64_t val;
            if (!r->how || r->owner == 0xFF || r->type == WQ_NONE || r->type >= v->ntypes) continue;
            if (r->owner != seat && (v->allies >> r->owner & 1u)) continue;
            gx = cell_of(r->bx); gz = cell_of(r->bz);
            if (gx >= m->gw || gz >= m->gh) continue;
            val = (int64_t)v->threat[r->type].value;
            if (r->owner != seat) val = -val;
            val += m->diff[gz * (IMPAI_GRID_MAX + 1) + gx];
            m->diff[gz * (IMPAI_GRID_MAX + 1) + gx] = (int32_t)(val > INT32_MAX ? INT32_MAX : val < INT32_MIN ? INT32_MIN : val);
        }
        for (gz = 0; gz < m->gh; gz++)
            for (gx = 0; gx < m->gw; gx++) {
                int64_t sum = 0;
                int dz, dx;
                for (dz = -1; dz <= 1; dz++)
                    for (dx = -1; dx <= 1; dx++)
                        if (gz + dz >= 0 && gz + dz < m->gh && gx + dx >= 0 && gx + dx < m->gw)
                            sum += m->diff[(gz + dz) * (IMPAI_GRID_MAX + 1) + gx + dx];
                m->influence[gz * m->gw + gx] = (int32_t)(sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : sum);
            }
    }
    return 1;
}

/* ---- the route ---- */

int64_t impai_cell_of_px(int64_t v) { return v >= 0 ? v / IMPAI_CELL_PX : -((-v + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX); }

static int64_t coord_held(int v) { return v < 0 ? 0 : v > 32767 ? 32767 : v; }

/* the ground threat of 256-px cell (gx, gz); 0 off the grid */
static uint32_t ground_at(const IMPAI_MAPS* m, int64_t gx, int64_t gz)
{
    return gx >= 0 && gz >= 0 && gx < m->gw && gz < m->gh ? m->threat[IMPAI_THREAT_GROUND][gz * m->gw + gx] : 0;
}

/* THE ROUTE: the straight segment from (x0, z0) px to the point (x1, z1), sampled at n = max(|dx|, |dz|) / 32 + 1
   steps, i = 0..n, the sample x0 + dx x i / n in 64-bit integers, each read at its 256-px cell (impai_cell_of_px)
   on the ground threat layer, a cell off the grid reading 0. A sample in the start's own cell, or in the cell just
   read, is skipped, so each cell the line crosses past the start's is read once. 1 when a cell read is above 0,
   the point's own cell included (impai_build.c, route_threatened). THE OWN CELL is left out so a builder standing
   in threat can still be sent out of it.
   BOUND: the point is held to 0..32 767 px, the coordinate limit, on each axis first -- a job's cell is recorded
   state, an int16 that can lie far past the map -- and the start is a 16.16 position's integer part, so |dx| and
   |dz| are below 2^16 and a route is at most 2 049 samples, 1 025 within the coordinate limit, each one map read. A
   point on any map already lies within the limit, so the hold changes no answer for a point a decision chose.
   RESIDUAL: the engine paths around obstacles and may bend into a threat the line misses; and a ghost row keeps
   its threat at its last block until the seat sees that block again (research/notes/impai/brain.md, As built), so
   a route stays closed until a unit of the seat looks. */
int impai_route_threat(const IMPAI_MAPS* m, int64_t x0, int64_t z0, int x1, int z1)
{
    const int64_t dx = coord_held(x1) - x0, dz = coord_held(z1) - z0;
    const int64_t ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
    const int64_t n = (ax > az ? ax : az) / 32 + 1, ox = impai_cell_of_px(x0), oz = impai_cell_of_px(z0);
    int64_t i, lx = ox, lz = oz;
    for (i = 0; i <= n; i++) {
        const int64_t gx = impai_cell_of_px(x0 + dx * i / n), gz = impai_cell_of_px(z0 + dz * i / n);
        if ((gx == ox && gz == oz) || (gx == lx && gz == lz)) continue;
        lx = gx;
        lz = gz;
        if (ground_at(m, gx, gz) > 0) return 1;
    }
    return 0;
}

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

uint64_t impai_known_digest(const IMPAI_VIEW* v)
{
    uint64_t h = 0xCBF29CE484222325ull;
    int i;
    for (i = 0; i < v->nrows; i++) h = fnv(h, &v->rows[i].seat[v->seat], sizeof(IMPAI_SEEN));
    return fnv(h, v->clusters, (size_t)v->nclusters * sizeof *v->clusters);
}

uint64_t impai_maps_digest(const IMPAI_MAPS* m)
{
    const size_t n = (size_t)m->gw * (size_t)m->gh;
    uint64_t h = 0xCBF29CE484222325ull;
    int layer;
    h = fnv(h, &m->gw, sizeof m->gw);
    h = fnv(h, &m->gh, sizeof m->gh);
    for (layer = 0; layer < IMPAI_THREATS; layer++) h = fnv(h, m->threat[layer], n * sizeof(uint32_t));
    h = fnv(h, m->influence, n * sizeof(int32_t));
    return fnv(h, m->presence, n * sizeof(uint16_t));
}
