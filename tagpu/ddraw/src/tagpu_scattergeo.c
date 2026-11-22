#include "tagpu_simreg.h"
SIM_FEATURE(build_scatter_geo, SIM_PUPPET_OWNER, "build-scatter stock-build-scatter");
/* tagpu_scattergeo.c -- see tagpu_scattergeo.h. Engine-free: no engine memory, no Win32, no allocation. */
#include <string.h>
#include "tagpu_scattergeo.h"

int sg_overlap(int x, int z, int fx, int fz, int u, int v, int gx, int gz)
{
    return x < u + gx && u < x + fx && z < v + gz && v < z + fz;
}

/* the footprint at box-relative place (px, pz) lies in the box */
static int in_box(const GgBox* box, int px, int pz, int fx, int fz)
{
    return px >= 0 && pz >= 0 && px + fx <= box->w && pz + fz <= box->h;
}

/* no cell of the footprint holds a unit other than self */
static int unoccupied(const GgBox* box, const uint16_t* occ, int px, int pz, int fx, int fz, unsigned self)
{
    int i, j;
    for (j = 0; j < fz; j++)
        for (i = 0; i < fx; i++) {
            const unsigned u = occ[(pz + j) * box->w + px + i];
            if (u && u != self) return 0;
        }
    return 1;
}

int sg_way_off(const GgBox* box, int x, int z, int fx, int fz, unsigned self, const uint8_t* pass,
               const int32_t* wsums, const int32_t* tsums, const uint16_t* occ, const SgWork* wk, int* ox, int* oz)
{
    static const int DX[4] = { 0, -1, 1, 0 }, DZ[4] = { -1, 0, 0, 1 };
    const int sx = x - box->x0, sz = z - box->z0;
    int head = 0, tail = 0, k;
    if (fx < 1 || fz < 1 || box->w < 1 || box->h < 1 || !in_box(box, sx, sz, fx, fz)) return 0;
    memset(wk->seen, 0, (size_t)box->w * (size_t)box->h);
    wk->seen[sz * box->w + sx] = 1;
    wk->qx[tail] = (int16_t)sx; wk->qz[tail] = (int16_t)sz; tail++;
    while (head < tail) {
        const int cx = wk->qx[head], cz = wk->qz[head];
        head++;
        for (k = 0; k < 4; k++) {
            const int nx = cx + DX[k], nz = cz + DZ[k];
            if (!in_box(box, nx, nz, fx, fz) || wk->seen[nz * box->w + nx]) continue;
            wk->seen[nz * box->w + nx] = 1;
            if (!pass[nz * box->w + nx] || gg_count(wsums, box->w, nx, nz, fx, fz)) continue;
            if (!gg_count(tsums, box->w, nx, nz, fx, fz) && unoccupied(box, occ, nx, nz, fx, fz, self)) {
                *ox = box->x0 + nx; *oz = box->z0 + nz;
                return 1;
            }
            wk->qx[tail] = (int16_t)nx; wk->qz[tail] = (int16_t)nz; tail++;
        }
    }
    return 0;
}
