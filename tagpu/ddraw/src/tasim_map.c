/* tasim_map.c -- the engine's feature grid, LoadMap's writes into it and the ground height (tasim_map.h). Each
   function's engine reading is at its declaration. */
#include <string.h>
#include "tasim_map.h"
#include "tasim_class.h"

/* the array sizes in a map's block, in its order: two u16 slots, four u8 bytes, the u16 word and back-reference, the
   u8 flags, the pool's u16 free list; each array starts on 4 bytes */
#define AL4(n) (((n) + 3u) & ~(size_t)3u)

static int refusal(int32_t w, int32_t h, int32_t pool_cap)
{
    if (w < TS_MAP_W_MIN || h < TS_MAP_H_MIN) return TS_MAP_SMALL;
    if ((uint64_t)(uint32_t)w * (uint32_t)h > TS_MAP_CELLS_MAX) return TS_MAP_LARGE;
    if (pool_cap < 0 || pool_cap > 0x7FFF) return TS_MAP_POOL;
    return TS_MAP_OK;
}

size_t ts_map_bytes(int32_t w, int32_t h, int32_t pool_cap)
{
    size_t n;
    if (refusal(w, h, pool_cap) != TS_MAP_OK) return 0;
    n = (size_t)w * (size_t)h;
    return 5 * AL4(2 * n) + 5 * AL4(n) + AL4(2 * (size_t)pool_cap);
}

int ts_map_init(TS_MAP* m, void* mem, size_t bytes, int32_t w, int32_t h, uint8_t sea, int32_t pool_cap,
                const TASIM_FEATUREDEF* fd, int32_t nfd)
{
    unsigned char* p = (unsigned char*)mem;
    size_t n;
    int32_t i;
    int r = refusal(w, h, pool_cap);
    if (r != TS_MAP_OK) return r;
    if (!mem || bytes < ts_map_bytes(w, h, pool_cap)) return TS_MAP_ROOM;
    memset(m, 0, sizeof *m);
    memset(mem, 0, ts_map_bytes(w, h, pool_cap));
    n = (size_t)w * (size_t)h;
    m->w = w;
    m->h = h;
    m->cells = (int32_t)n;
    m->sea = sea;
    m->slot_a = (uint16_t*)p; p += AL4(2 * n);
    m->slot_b = (uint16_t*)p; p += AL4(2 * n);
    m->fdef   = (uint16_t*)p; p += AL4(2 * n);
    m->back   = (uint16_t*)p; p += AL4(2 * n);
    m->pool_free = (uint16_t*)p; p += AL4(2 * (size_t)pool_cap);
    m->height = p; p += AL4(n);
    m->hmax   = p; p += AL4(n);
    m->hmin   = p; p += AL4(n);
    m->metal  = p; p += AL4(n);
    m->flags  = p; p += AL4(n);
    (void)p;
    for (i = 0; i < m->cells; i++) m->fdef[i] = (uint16_t)TS_F_NONE;
    m->fd = fd;
    m->nfd = fd ? nfd : 0;
    /* 0x421F5F..0x421F9B: next = i + 1, the free head 0, so record 0 is taken first */
    m->pool_cap = pool_cap;
    m->pool_top = pool_cap;
    for (i = 0; i < pool_cap; i++) m->pool_free[i] = (uint16_t)(pool_cap - 1 - i);
    return TS_MAP_OK;
}

/* 0x481550(x, z), the cell index or -1 off the grid (DISASSEMBLED 0x481550..0x481593, signed compares) */
static int32_t cell_at(const TS_MAP* m, int32_t x, int32_t z)
{
    if (x < 0 || x >= m->w || z < 0 || z >= m->h) return -1;
    return z * m->w + x;
}

int ts_map_destroy(TS_MAP* m, struct TS_CLASSMAPS* cm, int32_t idx, int force)
{
    const TASIM_FEATUREDEF* d;
    int32_t a = idx, dz, dx, fx, fz;
    uint16_t w;
    if (idx < 0 || idx >= m->cells) return 0;
    if (m->fdef[idx] == TS_F_FOOT) {
        /* 0x4246C6..0x4246E6: rows back by the byte +0x0A, columns by +0x0B */
        a = idx - ((int32_t)(m->back[idx] & 0xFF) * m->w + (int32_t)(m->back[idx] >> 8));
        if (a < 0) return 0;
    }
    w = m->fdef[a];
    if (w >= TS_F_MARK) return 0;
    if (w >= m->nfd) return 0;
    d = &m->fd[w];
    if (!force && (d->flags & 0x200)) return 0;
    if (m->flags[a] & 1) {
        /* 0x424761..0x42476F: the record goes back on the free list's head (0x4232F0) */
        if (m->back[a] < m->pool_cap && m->pool_top < m->pool_cap) m->pool_free[m->pool_top++] = m->back[a];
    }
    m->fdef[a] = (uint16_t)TS_F_NONE;
    m->flags[a] &= (uint8_t)~1u;
    fx = d->footx;
    fz = d->footz;
    for (dz = 0; dz < fz; dz++)
        for (dx = 0; dx < fx; dx++) {
            const int32_t c = a + dz * m->w + dx;
            if (c >= m->cells) continue;
            if (m->fdef[c] == TS_F_FOOT) {
                m->flags[c] &= (uint8_t)~1u;
                m->fdef[c] = (uint16_t)TS_F_NONE;
            }
        }
    if (cm) ts_class_remark_all(cm, a % m->w, a / m->w, fx, fz);
    return 1;
}

int ts_map_spawn(TS_MAP* m, struct TS_CLASSMAPS* cm, int32_t idx, uint32_t def, int32_t owner)
{
    const TASIM_FEATUREDEF* d;
    int32_t col, row, fx, fz, dz, dx;
    if (idx < 0 || idx >= m->cells || def == TS_F_NONE) return 0;
    if (def == TS_F_VOID) {
        m->fdef[idx] = (uint16_t)TS_F_VOID;            /* 0x423C76..0x423C81, and nothing else */
        return 0;
    }
    if (def >= (uint32_t)m->nfd) {
        m->past_defs++;
        return 0;
    }
    d = &m->fd[def];
    col = idx % m->w;
    row = idx / m->w;
    fx = d->footx;
    fz = d->footz;
    if (col + fx > m->w || row + fz > m->h) return 0;   /* 0x423CEC, 0x423D22: equal is accepted */
    for (dz = 0; dz < fz; dz++)
        for (dx = 0; dx < fx; dx++) {
            const int32_t c = idx + dz * m->w + dx;
            if (m->fdef[c] != TS_F_NONE && !ts_map_destroy(m, cm, c, 0)) return 0;
        }
    if (!(d->flags & 1)) {
        /* a 3DO: the pool's head, or nothing (0x423DB4..0x423DE5) */
        if (m->pool_top <= 0) return 0;
        m->back[idx] = m->pool_free[--m->pool_top];
        m->flags[idx] |= 1;
    } else {
        m->back[idx] = 0;
        m->flags[idx] &= (uint8_t)~1u;
    }
    m->fdef[idx] = (uint16_t)def;
    m->flags[idx] = (uint8_t)((m->flags[idx] & 0x87) | ((owner & 0xF) << 3));
    for (dz = 0; dz < fz; dz++)
        for (dx = 0; dx < fx; dx++) {
            const int32_t c = idx + dz * m->w + dx;
            if (!dz && !dx) continue;
            m->fdef[c] = (uint16_t)TS_F_FOOT;
            m->back[c] = (uint16_t)((dz & 0xFF) | ((dx & 0xFF) << 8));   /* bytes: a footprint past 255 wraps */
            m->flags[c] &= (uint8_t)~1u;
        }
    if (cm) ts_class_remark_all(cm, col, row, fx, fz);
    return 1;
}

void ts_map_extremes(TS_MAP* m, int32_t x, int32_t z, int32_t w, int32_t h)
{
    const int32_t x0 = (int16_t)x < 0 ? 0 : (int16_t)x, z0 = (int16_t)z < 0 ? 0 : (int16_t)z;
    int32_t x1 = (int16_t)x + (int16_t)w, z1 = (int16_t)z + (int16_t)h, cx, cz;
    if (x1 >= m->w) x1 = m->w - 1;
    if (z1 >= m->h) z1 = m->h - 1;
    for (cz = z0; cz < z1; cz++)
        for (cx = x0; cx < x1; cx++) {
            const int32_t i = cz * m->w + cx;
            uint8_t lo = m->height[i], hi = lo, v;
            if (cx < m->w - 1) {
                v = m->height[i + 1];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            if (cz < m->h - 1) {
                v = m->height[i + m->w];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                if (cx < m->w - 1) {
                    v = m->height[i + m->w + 1];
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
            }
            m->hmin[i] = lo;
            m->hmax[i] = hi;
        }
}

static void mask_cell(TS_MAP* m, int32_t i)
{
    if (m->fdef[i] == TS_F_NONE || m->fdef[i] == TS_F_FOOT) m->fdef[i] = (uint16_t)TS_F_BORDER;
}

void ts_map_mask(TS_MAP* m, int lava)
{
    int32_t x, z, py;
    m->ext_w = (m->w << 4) - 0x20;
    m->ext_h = (m->h << 4) - 0x80;
    for (z = 0; z < m->h; z++) {
        mask_cell(m, z * m->w + m->w - 2);
        mask_cell(m, z * m->w + m->w - 1);
    }
    /* passes 2 and 3 stay on the grid by TS_MAP_H_MIN (tasim_map.h); the row tests are the bound too */
    for (x = 0; x < m->w; x++)
        for (z = 0, py = 0; z < m->h && py - (m->height[z * m->w + x] >> 1) < 0; z++, py += 16)
            mask_cell(m, z * m->w + x);
    for (x = 0; x < m->w; x++)
        for (z = m->h - 1, py = z * 16; z > 0 && py - (m->height[z * m->w + x] >> 1) > m->ext_h; z--, py -= 16)
            mask_cell(m, (z - 1) * m->w + x);
    if (lava)
        for (z = 0; z < m->cells; z++)
            if (m->hmin[z] <= m->sea) mask_cell(m, z);
}

void ts_map_metal(TS_MAP* m)
{
    int32_t i, dz, dx;
    for (i = 0; i < m->cells; i++) {
        const uint16_t w = m->fdef[i];
        const TASIM_FEATUREDEF* d;
        if (w >= TS_F_MARK || w >= m->nfd) continue;   /* the engine reads its table at any word below 0xFFFB */
        d = &m->fd[w];
        if (d->metal == 0.0f || !(d->flags & 0x200)) continue;
        for (dz = 0; dz < d->footz; dz++)
            for (dx = 0; dx < d->footx; dx++) {
                const int32_t c = cell_at(m, i % m->w + dx, i / m->w + dz);
                if (c >= 0) m->metal[c] = (uint8_t)(int32_t)d->metal;   /* _ftol 0x4E43A0: toward zero */
            }
    }
}

int ts_map_load(TS_MAP* m, const TS_MAPLOAD* in)
{
    const int v2 = in->version == 0x2000;
    const uint8_t bg = (in->surface_metal < 0 || !v2) ? 0 : (uint8_t)in->surface_metal;
    int32_t i, k;
    if (!in->attr || (in->version != 0x2000 && in->version != 0x1020)) return -1;
    for (i = 0; i < m->cells; i++) {
        m->slot_a[i] = m->slot_b[i] = 0;
        m->fdef[i] = (uint16_t)TS_F_NONE;
        m->flags[i] &= 0xFC;
        m->metal[i] = bg;
    }
    for (i = 0; i < m->cells; i++) {
        const uint8_t* r = in->attr + (size_t)i * (v2 ? 4 : 8);
        m->height[i] = r[0];
        m->flags[i] = (uint8_t)((m->flags[i] & 0xD7) | 0x50);
        if (v2) {
            if ((uint32_t)(r[1] | r[2] << 8) == TS_F_VOID) ts_map_spawn(m, NULL, i, TS_F_VOID, TS_OWNER_NONE);
        } else {
            m->metal[i] = r[6];
        }
    }
    for (i = 0; i < m->cells; i++) {
        const uint8_t* r = in->attr + (size_t)i * (v2 ? 4 : 8);
        const uint32_t d = v2 ? (uint32_t)(r[1] | r[2] << 8) : r[2];
        if (d < (v2 ? 0xFFFBu : 0xFCu)) ts_map_spawn(m, NULL, i, d, TS_OWNER_NONE);
    }
    if (v2)
        for (k = 0; k < in->nschema; k++) {
            /* 0x423160: a GAF anchor at (x, z), a 3DO one centred there, footx / 2 truncated (cdq; sub; sar 1) */
            const TS_MAPFEAT* s = &in->schema[k];
            int32_t x = s->x, z = s->z, c;
            if (s->def < 0 || s->def >= m->nfd) continue;
            if (!(m->fd[s->def].flags & 1)) {
                x -= m->fd[s->def].footx / 2;
                z -= m->fd[s->def].footz / 2;
            }
            /* 0x481550 answers NULL off the grid and the spawn then reads its footprint through NULL: the bound */
            if ((c = cell_at(m, x, z)) >= 0) ts_map_spawn(m, NULL, c, (uint32_t)s->def, TS_OWNER_NONE);
        }
    ts_map_extremes(m, 0, 0, m->w, m->h);
    ts_map_mask(m, in->lava);
    ts_map_metal(m);
    return 0;
}

int32_t ts_map_ground(const TS_MAP* m, int32_t x16, int32_t z16)
{
    const int32_t x = (int16_t)(uint16_t)((uint32_t)x16 >> 16), z = (int16_t)(uint16_t)((uint32_t)z16 >> 16);
    const int32_t cx = x >> 4, cz = z >> 4, fx = x & 15, fz = z & 15;
    int32_t i, h00, h10, h01, h11, top, bot;
    if (cx < 0 || cx + 1 >= m->w || cz < 0 || cz + 1 >= m->h) return -1;
    i = cz * m->w + cx;
    h00 = m->height[i];
    h10 = m->height[i + 1];
    h01 = m->height[i + m->w];
    h11 = m->height[i + m->w + 1];
    top = h00 + (h10 - h00) * fx / 16;                 /* C's division truncates toward zero, as the sar does */
    bot = h01 + (h11 - h01) * fx / 16;
    return top + (bot - top) * fz / 16;
}

void ts_map_cell(const TS_MAP* m, int32_t idx, uint8_t out[13])
{
    out[0] = (uint8_t)m->slot_a[idx];
    out[1] = (uint8_t)(m->slot_a[idx] >> 8);
    out[2] = (uint8_t)m->slot_b[idx];
    out[3] = (uint8_t)(m->slot_b[idx] >> 8);
    out[4] = m->height[idx];
    out[5] = m->hmax[idx];
    out[6] = m->hmin[idx];
    out[7] = m->metal[idx];
    out[8] = (uint8_t)m->fdef[idx];
    out[9] = (uint8_t)(m->fdef[idx] >> 8);
    out[10] = (uint8_t)m->back[idx];
    out[11] = (uint8_t)(m->back[idx] >> 8);
    out[12] = m->flags[idx];
}
