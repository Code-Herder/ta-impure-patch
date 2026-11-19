/* tagpu_guardgeo.c -- the geometry of guarding a factory (tagpu_guardgeo.h). Pure arithmetic over the caller's
   rasters: the callers bound every input (a box at most GUARD_BOX_MAX a side, footprints 1..WQ_FOOT_MAX), so every
   index below fits int and every product int32. */
#include <math.h>
#include <string.h>
#include "tagpu_guardgeo.h"
#include "tagpu_simreg.h"

/* The seats and the lane a guarded plant keeps, which tagpu_guard.c orders its builders by. OWNER: a guard's
   handler runs only on its owner's machine (the order run's owner test, 0x48AEB5..0x48AECD), and a puppet's
   builders move as the recording says. */
SIM_FEATURE(guard_geometry, SIM_PUPPET_OWNER, "guard-seats stock-guard-seats");

void gg_exits(const uint8_t* yard, int fx, int fz, GgExits* out)
{
    int side;
    memset(out, 0, sizeof *out);
    for (side = 0; side < 4; side++) {
        const int along_x = side < 2, len = along_x ? fx : fz;
        int a = -1, e = -1, i;
        for (i = 0; i < len; i++) {
            const int x = along_x ? i : (side == 2 ? 0 : fx - 1), z = along_x ? (side == 0 ? 0 : fz - 1) : i;
            const uint8_t y = yard[z * fx + x];
            if ((y & 0x04) && !(y & 0x02)) {
                if (a < 0) a = i;
                e = i;
            }
        }
        if (a < 0) continue;
        out->edge[out->n] = (uint8_t)side;
        out->a[out->n] = (int16_t)a;
        out->e[out->n] = (int16_t)e;
        out->n++;
    }
}

void gg_sums(const uint8_t* blocked, int w, int h, int32_t* sums)
{
    const int W = w + 1;
    int x, z;
    memset(sums, 0, (size_t)W * sizeof *sums);
    for (z = 0; z < h; z++) {
        int32_t row = 0;
        sums[(z + 1) * W] = 0;
        for (x = 0; x < w; x++) {
            row += blocked[z * w + x] != 0;
            sums[(z + 1) * W + x + 1] = sums[z * W + x + 1] + row;
        }
    }
}

int32_t gg_count(const int32_t* sums, int w, int x, int z, int fx, int fz)
{
    const int W = w + 1;
    return sums[(z + fz) * W + x + fx] - sums[z * W + x + fx] - sums[(z + fz) * W + x] + sums[z * W + x];
}

void gg_square_fit(uint8_t* m, int bw, int bh, int ex, int ez)
{
    int i, j;
    for (j = 0; j < bh; j++) {
        uint8_t* row = m + (size_t)j * (size_t)bw;
        int shut = bw;
        for (i = bw - 1; i >= 0; i--) {
            if (!row[i]) shut = i;
            row[i] = (uint8_t)(shut > i + ex);
        }
    }
    for (i = 0; i < bw; i++) {
        int shut = bh;
        for (j = bh - 1; j >= 0; j--) {
            uint8_t* p = m + (size_t)j * (size_t)bw + (size_t)i;
            if (!*p) shut = j;
            *p = (uint8_t)(shut > j + ez);
        }
    }
}

int gg_overlaps(const GgRect* r, int x, int z, int fx, int fz)
{
    return r->x0 <= x + fx - 1 && x <= r->x1 && r->z0 <= z + fz - 1 && z <= r->z1;
}

static GgRect square_at(int x, int z, int s)
{
    GgRect r;
    r.x0 = (int16_t)x; r.z0 = (int16_t)z;
    r.x1 = (int16_t)(x + s - 1); r.z1 = (int16_t)(z + s - 1);
    return r;
}

/* the cells between r and the plant p along the axis that separates them most; below 0 they overlap */
static int clear_of(const GgRect* r, const GgRect* p)
{
    int g = r->x0 - p->x1 - 1, h;
    if ((h = p->x0 - r->x1 - 1) > g) g = h;
    if ((h = r->z0 - p->z1 - 1) > g) g = h;
    if ((h = p->z0 - r->z1 - 1) > g) g = h;
    return g;
}

static int bit_get(const uint8_t* b, int i) { return b[i >> 3] >> (i & 7) & 1; }
static void bit_set(uint8_t* b, int i) { b[i >> 3] = (uint8_t)(b[i >> 3] | 1u << (i & 7)); }

typedef struct LaneQ {
    const GgBox* box; const GgRect* plant; const uint8_t* fit; const int32_t* sums; const GgWork* wk;
    int s, n;
} LaneQ;

/* place (x, z) queued, reached from queue index `from` (-1 a start), when its square is in the box, not seen,
   clear of the plant, fitting the class and holding no blocked cell; the queue holds every place of the box at
   most once (the seen bits), so it cannot overflow */
static void lane_push(LaneQ* q, int x, int z, int from)
{
    const GgRect r = square_at(x, z, q->s);
    const int lx = x - q->box->x0, lz = z - q->box->z0, i = lz * q->box->w + lx;
    if (lx < 0 || lz < 0 || lx + q->s > q->box->w || lz + q->s > q->box->h) return;
    if (bit_get(q->wk->seen, i) || clear_of(&r, q->plant) < 0 || !q->fit[i]) return;
    if (gg_count(q->sums, q->box->w, lx, lz, q->s, q->s) > 0) return;
    bit_set(q->wk->seen, i);
    q->wk->qx[q->n] = (int16_t)x;
    q->wk->qz[q->n] = (int16_t)z;
    q->wk->qf[q->n] = from;
    q->n++;
}

/* r grown by m and clipped to the box, into out unless a rectangle already there holds it: 0 when out is full */
static int lane_add(const GgBox* box, GgRect r, int m, GgRect* out, int* nout, int cap)
{
    int k;
    r.x0 = (int16_t)(r.x0 - m > box->x0 ? r.x0 - m : box->x0);
    r.z0 = (int16_t)(r.z0 - m > box->z0 ? r.z0 - m : box->z0);
    r.x1 = (int16_t)(r.x1 + m < box->x0 + box->w - 1 ? r.x1 + m : box->x0 + box->w - 1);
    r.z1 = (int16_t)(r.z1 + m < box->z0 + box->h - 1 ? r.z1 + m : box->z0 + box->h - 1);
    for (k = 0; k < *nout; k++)
        if (out[k].x0 <= r.x0 && out[k].z0 <= r.z0 && out[k].x1 >= r.x1 && out[k].z1 >= r.z1) return 1;
    if (*nout >= cap) return 0;
    out[(*nout)++] = r;
    return 1;
}

/* The path ending at queue index `end`, walked back to its start, as rectangles (way_rects of impai_build.c): a
   square joins the rectangle before it while their union has at most twice the cells its squares can cover. The
   walk ends: each step goes to a lower queue index, and a start's is -1. */
static int lane_rects(const LaneQ* q, int end, int m, GgRect* out, int* nout, int cap)
{
    const int s = q->s;
    GgRect cur = square_at(q->wk->qx[end], q->wk->qz[end], s);
    int k, steps = 0;
    for (k = q->wk->qf[end]; ; k = q->wk->qf[k]) {
        if (k >= 0) {
            const GgRect r = square_at(q->wk->qx[k], q->wk->qz[k], s);
            GgRect u = cur;
            if (r.x0 < u.x0) u.x0 = r.x0;
            if (r.z0 < u.z0) u.z0 = r.z0;
            if (r.x1 > u.x1) u.x1 = r.x1;
            if (r.z1 > u.z1) u.z1 = r.z1;
            if ((u.x1 - u.x0 + 1) * (u.z1 - u.z0 + 1) <= 2 * (s * s + s * (steps + 1))) {
                cur = u;
                steps++;
                continue;
            }
        }
        if (!lane_add(q->box, cur, m, out, nout, cap)) return 0;
        if (k < 0) return 1;
        cur = square_at(q->wk->qx[k], q->wk->qz[k], s);
        steps = 0;
    }
}

static void lane_starts(LaneQ* q, const GgExits* L, int k)
{
    const int side = L->edge[k], s = q->s;
    const int pfx = q->plant->x1 - q->plant->x0 + 1, pfz = q->plant->z1 - q->plant->z0 + 1;
    const int last = L->e[k] - s + 1 > L->a[k] ? L->e[k] - s + 1 : L->a[k];
    int i;
    for (i = L->a[k]; i <= last; i++) {
        const int x = side < 2 ? q->plant->x0 + i : (side == 2 ? q->plant->x0 - s : q->plant->x0 + pfx);
        const int z = side < 2 ? (side == 0 ? q->plant->z0 - s : q->plant->z0 + pfz) : q->plant->z0 + i;
        lane_push(q, x, z, -1);
    }
}

int gg_lane(const GgBox* box, const GgRect* plant, const GgExits* L, int start, int s, int open, int margin,
            const uint8_t* fit, const int32_t* sums, const GgWork* wk, GgRect* out, int* nout, int cap)
{
    static const int DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
    LaneQ q;
    int k, head;
    q.box = box; q.plant = plant; q.fit = fit; q.sums = sums; q.wk = wk; q.s = s; q.n = 0;
    memset(wk->seen, 0, (size_t)box->w * box->h / 8 + 1);
    if (start >= 0 && start < L->n) lane_starts(&q, L, start);
    for (k = 0; k < L->n; k++)
        if (k != start) lane_starts(&q, L, k);
    for (head = 0; head < q.n; head++) {
        const GgRect r = square_at(wk->qx[head], wk->qz[head], s);
        if (clear_of(&r, plant) >= open) return lane_rects(&q, head, margin, out, nout, cap);
        for (k = 0; k < 4; k++) lane_push(&q, wk->qx[head] + DX[k], wk->qz[head] + DZ[k], head);
    }
    return 0;
}

int gg_rin(int fx, int fz)
{
    const int v = (int)(16.0 * sqrt((double)(fx * fx + fz + fz)));
    return v / 2;
}

int gg_goal_place(int32_t c, int bf)
{
    /* 16.16 world px; (bf << 19) is half the footprint in 16.16 px and 0x80000 half a cell */
    const int32_t v = c - (int32_t)((uint32_t)bf << 19) + 0x80000;
    return v >= 0 ? v >> 20 : -(int)((-(int64_t)v + 0xFFFFF) >> 20);
}

/* the footprint at box-relative place (x, z) lies in the box */
static int in_box(const GgBox* box, int x, int z, int fx, int fz)
{
    return x >= 0 && z >= 0 && x + fx <= box->w && z + fz <= box->h;
}

/* THE WAY OUT FOR A SEAT. A breadth-first search over the places a footprint fx x fz fits on `pass` and covers
   no `taken` cell but those of its own place (ox, oz), from that place, to one whose footprint touches the box's
   edge. The sums are of `taken` less the own footprint, made once a search; each place enters the queue at most
   once (the seen bits). Box-relative throughout. */
static int way_out_rel(const GgBox* box, int ox, int oz, int fx, int fz, const uint8_t* pass, const uint8_t* taken,
                       const GgWork* wk)
{
    static const int DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
    const int w = box->w, h = box->h, W = w + 1;
    int x, z, n = 0, head;
    for (z = 0; z < h; z++) {
        int32_t row = 0;
        if (!z) memset(wk->sums, 0, (size_t)W * sizeof *wk->sums);
        wk->sums[(z + 1) * W] = 0;
        for (x = 0; x < w; x++) {
            const int own = x >= ox && x < ox + fx && z >= oz && z < oz + fz;
            row += taken[z * w + x] != 0 && !own;
            wk->sums[(z + 1) * W + x + 1] = wk->sums[z * W + x + 1] + row;
        }
    }
    memset(wk->seen, 0, (size_t)w * h / 8 + 1);
    bit_set(wk->seen, oz * w + ox);
    wk->qx[n] = (int16_t)ox; wk->qz[n] = (int16_t)oz; n++;
    for (head = 0; head < n; head++) {
        const int px = wk->qx[head], pz = wk->qz[head];
        int k;
        if (px == 0 || pz == 0 || px + fx == w || pz + fz == h) return 1;
        for (k = 0; k < 4; k++) {
            const int nx = px + DX[k], nz = pz + DZ[k], i = nz * w + nx;
            if (!in_box(box, nx, nz, fx, fz) || bit_get(wk->seen, i) || !pass[i]) continue;
            if (gg_count(wk->sums, w, nx, nz, fx, fz) > 0) continue;
            bit_set(wk->seen, i);
            wk->qx[n] = (int16_t)nx; wk->qz[n] = (int16_t)nz; n++;
        }
    }
    return 0;
}

#ifdef GUARDGEO_TEST
int gg_way_out(const GgBox* box, int x, int z, int fx, int fz, const uint8_t* pass, const uint8_t* taken,
               const GgWork* wk)
{
    const int ox = x - box->x0, oz = z - box->z0;
    if (!in_box(box, ox, oz, fx, fz)) return 0;
    return way_out_rel(box, ox, oz, fx, fz, pass, taken, wk);
}
#endif

/* a footprint at box-relative (x, z) that lies in the box, fits pass and covers no taken cell */
static int place_free(const GgBox* box, int x, int z, int fx, int fz, const uint8_t* pass, const uint8_t* taken)
{
    int i, j;
    if (!in_box(box, x, z, fx, fz) || !pass[z * box->w + x]) return 0;
    for (j = 0; j < fz; j++)
        for (i = 0; i < fx; i++)
            if (taken[(z + j) * box->w + x + i]) return 0;
    return 1;
}

static void mark(uint8_t* t, int w, int x, int z, int fx, int fz, uint8_t v)
{
    int i, j;
    for (j = 0; j < fz; j++)
        for (i = 0; i < fx; i++) t[(z + j) * w + x + i] = v;
}

int gg_seat(const GgBox* box, int fx, int fz, const uint8_t* pass, const uint8_t* taken, int gx, int gz,
            int r2min, int r2max, const GgSeated* seated, int nseated, int budget, const GgWork* wk,
            uint8_t* taken2, int* sx, int* sz)
{
    const int w = box->w, h = box->h;
    int tried = 0, slack;
    if (r2min > r2max) return 0;
    /* the ranking, walked without a sort: slack 0 (the band's outer edge) first, each slack's places in z then x
       order. For one d2 a row z holds at most the two places x = gx -+ r with r * r = d2 - dz * dz, so the walk
       is O(band width x box height) */
    for (slack = 0; slack <= r2max - r2min; slack++) {
        const int d2 = r2max - slack;
        int z;
        for (z = 0; z + fz <= h; z++) {
            const int dz = z + box->z0 - gz, rem = d2 - dz * dz;
            int r, side;
            if (rem < 0) continue;
            r = (int)sqrt((double)rem);
            while (r * r > rem) r--;
            while ((r + 1) * (r + 1) <= rem) r++;
            if (r * r != rem) continue;
            for (side = 0; side < (r ? 2 : 1); side++) {
                const int x = gx + (side ? r : -r) - box->x0;
                int k, ok = 1;
                if (x < 0 || x + fx > w || !place_free(box, x, z, fx, fz, pass, taken)) continue;
                if (tried++ >= budget) return 0;
                if (!way_out_rel(box, x, z, fx, fz, pass, taken, wk)) continue;
                memcpy(taken2, taken, (size_t)w * h);
                mark(taken2, w, x, z, fx, fz, 1);
                for (k = 0; k < nseated && ok; k++)
                    ok = way_out_rel(box, seated[k].x - box->x0, seated[k].z - box->z0, seated[k].fx, seated[k].fz,
                                     seated[k].pass, taken2, wk);
                if (!ok) continue;
                *sx = x + box->x0;
                *sz = z + box->z0;
                return 1;
            }
        }
    }
    return 0;
}

int gg_wait_spot(const GgBox* box, int x, int z, int fx, int fz, const uint8_t* pass, const uint8_t* taken,
                 int rmax, int* wx, int* wz)
{
    const int ox = x - box->x0, oz = z - box->z0;
    int r;
    if (place_free(box, ox, oz, fx, fz, pass, taken)) {
        *wx = x; *wz = z;
        return 1;
    }
    for (r = 1; r <= rmax; r++) {
        int i;
        for (i = 0; i < 8 * r; i++) {
            int px, pz;
            if (i < 2 * r)      { px = ox - r + i;           pz = oz - r; }
            else if (i < 4 * r) { px = ox + r;               pz = oz - r + (i - 2 * r); }
            else if (i < 6 * r) { px = ox + r - (i - 4 * r); pz = oz + r; }
            else                { px = ox - r;               pz = oz + r - (i - 6 * r); }
            if (place_free(box, px, pz, fx, fz, pass, taken)) {
                *wx = px + box->x0; *wz = pz + box->z0;
                return 1;
            }
        }
    }
    return 0;
}
