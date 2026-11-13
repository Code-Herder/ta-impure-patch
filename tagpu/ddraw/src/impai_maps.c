/* impai_maps.c -- the reach labels and the maps a seat's knowledge derives (impai.h). Engine-free: every input
   is an argument, so the maps cannot read state the recorder does not carry. */
#include <string.h>
#include "impai.h"

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

/* ---- the threat table ---- */

/* a value rounded once to an integer field: half up, held to 0..max */
static uint32_t round_to(double v, uint32_t max)
{
    if (!(v > 0.0)) return 0;
    if (v >= (double)max) return max;
    return (uint32_t)(v + 0.5);
}

void impai_threat_from_roles(const TASIM_ROLE_OUT* out, int n, IMPAI_THREAT* t)
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

int impai_maps_build(const IMPAI_VIEW* v, IMPAI_MAPS* m)
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
    /* threat, one layer at a time: each identified enemy row's type stamps its dps over its range */
    for (layer = 0; layer < IMPAI_THREATS; layer++) {
        memset(m->diff, 0, (size_t)(IMPAI_GRID_MAX + 1) * IMPAI_GRID_MAX * sizeof(int32_t));
        for (i = 1; i < v->nrows; i++) {
            const IMPAI_SEEN* r = &v->rows[i].seat[seat];
            const IMPAI_THREAT* t;
            uint32_t d;
            if (!r->how || r->owner == 0xFF || r->owner == seat || (v->allies >> r->owner & 1u) ||
                r->type == WQ_NONE || r->type >= v->ntypes)
                continue;
            t = &v->threat[r->type];
            d = layer == IMPAI_THREAT_GROUND ? t->dps_ground : layer == IMPAI_THREAT_AIR ? t->dps_air : t->dps_water;
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
