/* tagpu_wikiscene.c -- the in-game wiki's 3D stage as the world passes draw it. Contract:
   tagpu_wikiscene.h; the specification is tools/wiki/mockup/scene.js and the two gathers of
   tools/tascene-render.js, whose arithmetic this file follows line for line at the stage's camera
   (E = (0, 0), W = (gw, gh), the viewport's corner (0, 0), every height 0). */
#include <string.h>
#include "tagpu_wikiscene.h"
#include "tagpu_nanostage.h"

#define FXMODE_OPAQUE 1                 /* TAGPU_FXMODE_OPAQUE: tagpu_fx.h, not included: pure */
#define FXMODE_ALPHA  2

/* the gather's `ceil32` of a non-negative value */
static int ceil32(int v) { return (v + 31) / 32; }

/* THE DEPTH KEYS, tagpu_feat.c `map_key` as the lab's gatherFeatures computes them (the parity
   lane): a flat feature in the backdrop band 0.40 + 0.10 f, f its row-major place in the clamped
   sweep; a tall one above its row's units, 3 + 4 rel + 1.5 (col - c0) / cols. `rel` counts from
   the CLAMPED sweep's first row, as the lab does; tagpu_feat.c counts from the unit gather's
   unclamped row base, which differs wherever the eye is within 16 rows of the map's top -- as the
   stage's always is. The two agree on every order among features, which is all a stage without 3D
   units has to show; the units' keys (landing 6h) must take the same base as these. */
static float flat_key(int rel0, int col, int c0, int cols, double span)
{
    return (float)(0.40 + 0.10 * ((double)rel0 * cols + (col - c0)) / span);
}

static float tall_key(int rel0, int rows, int col, int c0, int cols)
{
    int rel = rel0 < 0 ? 0 : rel0 > rows + 8 ? rows + 8 : rel0;
    return (float)(3.0 + rel * 4.0 + 1.5 * ((double)(col - c0) / cols));
}

/* one frame's quads into a bucket, a compound frame's each sub-frame at the same anchor (tagpu_feat.c
   `emit_frame`): a sub-frame that asks for the alpha blit is drawn ALPHA under an opaque mode */
static int put(WSCENEQUAD* bucket, int* n, const WSCENEFRAME* fr, int nfr, int sx, int sy,
               float enc, int mode, int feat, int wax, int waz)
{
    int k;
    for (k = 0; k < nfr; k++) {
        WSCENEQUAD* q;
        if (*n >= WIKISCENE_QUADS) return 0;
        q = &bucket[(*n)++];
        q->x0 = (float)(sx - fr[k].hx);
        q->y0 = (float)(sy - fr[k].hy);
        q->x1 = q->x0 + (float)fr[k].w;
        q->y1 = q->y0 + (float)fr[k].h;
        q->enc = enc;
        q->mode = (uint8_t)(nfr > 1 && mode == FXMODE_OPAQUE && fr[k].subalp ? FXMODE_ALPHA : mode);
        q->sub = (uint8_t)k;
        q->feat = (int16_t)feat;
        q->wax = (float)wax;
        q->waz = (float)waz;
    }
    return 1;
}

int tagpu_wikiscene_ground(const WSCENEGROUND* g, WSCENE* out)
{
    int16_t* anchor = out->anchor;      /* cell -> feature, -1 none                              */
    int tw, th, cols, rows, r, c, i, r0, c0, nRows, nCols;
    double span;

    out->refused = 1;
    out->ncell = out->nshadow = out->nbody = 0;
    out->gw = out->gh = 0;
    if (!g || g->w < 2 || g->h < 2 || g->w > WIKISCENE_MAP || g->h > WIKISCENE_MAP || !g->tilemap ||
        g->ntile < 1 || g->nfeat < 0 || g->nfeat > WIKISCENE_FEATS || (g->nfeat && !g->feat))
        return 0;
    out->gw = g->w * 16;
    out->gh = g->h * 16;

    /* THE TERRAIN: classicTerrain().gather over E = (0, 0), W = (gw, gh) -- every cell of the tile
       map, row by row, its tile's index bounded by the set (a junk index is not drawn there; here
       it refuses the ground, which may not be drawn in part) */
    tw = g->w / 2;
    th = g->h / 2;
    cols = ceil32(out->gw);
    rows = ceil32(out->gh);
    for (r = 0; r < rows; r++) {
        if (r >= th) continue;
        for (c = 0; c < cols; c++) {
            unsigned idx;
            if (c >= tw) continue;
            idx = g->tilemap[r * tw + c];
            if (idx >= (unsigned)g->ntile) return 0;
            out->cell[out->ncell].col = (int16_t)c;
            out->cell[out->ncell].row = (int16_t)r;
            out->cell[out->ncell].tile = (uint16_t)idx;
            out->ncell++;
        }
    }

    /* THE FEATURES: gatherFeatures' sweep over the same rect. Its rect is the engine's
       (DrawGameScreen): 16 rows above the eye's and the view's rows + 32, 10 columns left and the
       view's columns + 12, clamped to the map less its last row and column, which the engine's
       sweep never reaches -- nothing anchored there is drawn. An anchor cell takes the last
       feature the description places on it, as the lab's anchor table does. The ground is deeper
       than a map that has not grown to it (tagpu_wiki_map_rows), so a feature whose footprint
       ends past the map's last row is not on this map, as gfx.js's Stage leaves it out. */
    for (i = 0; i < g->w * g->h; i++) anchor[i] = -1;
    for (i = 0; i < g->nfeat; i++) {
        const WSCENEFEAT* f = &g->feat[i];
        if (f->cx < 0 || f->cz < 0 || f->cx >= g->w) return 0;
        if (f->nbody > WIKISCENE_SUBS || f->nshadow > WIKISCENE_SUBS || f->fx < 0 || f->fz < 0) return 0;
        if (f->cz + f->fz > g->h) continue;
        anchor[f->cz * g->w + f->cx] = (int16_t)i;
    }
    r0 = (0 >> 4) - 16;
    nRows = (out->gh >> 4) + 32;
    c0 = (0 >> 4) - 10;
    nCols = (out->gw >> 4) + 12;
    if (r0 < 0) { nRows += r0; r0 = 0; }
    if (r0 + nRows > g->h - 1) nRows = g->h - r0 - 1;
    if (c0 < 0) { nCols += c0; c0 = 0; }
    if (c0 + nCols > g->w - 1) nCols = g->w - c0 - 1;
    span = (double)nRows * nCols;
    for (r = r0; r < r0 + nRows; r++)
        for (c = c0; c < c0 + nCols; c++) {
            const int k = anchor[r * g->w + c];
            const WSCENEFEAT* f;
            int hx, hz, wax, waz, flat;
            float enc;
            if (k < 0) continue;
            f = &g->feat[k];
            /* the engine's projection, screen y = world z - height/2, at height 0; the viewport's
               corner and the eye are both (0, 0), so the anchor is its own screen point */
            hx = (f->fx * 16) / 2;
            hz = (f->fz * 16) / 2;
            wax = c * 16 + hx;
            waz = r * 16 + hz;
            flat = f->height < 10;
            enc = flat ? flat_key(r - r0, c, c0, nCols, span) : tall_key(r - r0, nRows, c, c0, nCols);
            if (f->nshadow &&
                !put(out->shadow, &out->nshadow, f->shadow, f->nshadow, wax, waz,
                     enc - (flat ? 0.03f : 0.30f), (f->flags & 8) ? FXMODE_ALPHA : FXMODE_OPAQUE, k, wax, waz))
                return 0;
            if (f->nbody &&
                !put(out->body, &out->nbody, f->body, f->nbody, wax, waz, enc,
                     (f->flags & 4) ? FXMODE_ALPHA : FXMODE_OPAQUE, k, wax, waz))
                return 0;
        }
    out->refused = 0;
    return 1;
}

void tagpu_wikiscene_fog(const uint8_t* fog, int w, int h, WSCENEFOG* out)
{
    int bw, bh, bx, bz, i, any = 0;
    out->on = 0;
    out->cols = out->rows = 0;
    out->orgx = out->orgz = 16.0f;
    if (!fog || w < 2 || h < 2 || w > WIKISCENE_MAP || h > WIKISCENE_MAP) return;
    for (i = 0; i < w * h; i++)
        if (fog[i]) { any = 1; break; }
    if (!any) return;
    bw = w >> 1;
    bh = h >> 1;
    /* the corners of block (bx, bz) are the centres of it and of its right, lower and lower-right
       neighbours; the far column and row replicate their own block (tagpu_glsl.h, the clamp stops
       one cell short of the grid). 1 unexplored, 2 grey: the view's 2 is black, its 1 grey. */
    for (bz = 0; bz < bh; bz++)
        for (bx = 0; bx < bw; bx++) {
            int j, b0 = 0, b1 = 0;
            /* tl, tr, bl, br: bit j of each mask */
            for (j = 0; j < 4; j++) {
                int x = bx + (j & 1), z = bz + (j >> 1), f;
                if (x > bw - 1) x = bw - 1;
                if (z > bh - 1) z = bh - 1;
                f = fog[z * 2 * w + x * 2];
                if (f == 2) b0 |= 1 << j;
                else if (f) b1 |= 1 << j;
            }
            out->grid[bz * bw + bx] = (uint16_t)(b0 | b1 << 8);
        }
    out->cols = bw;
    out->rows = bh;
    out->on = 1;
}

/* ---- the model records ------------------------------------------------------------------------ */

#define GHOST_ALPHA 0.40f               /* tagpu_native.c `s_ghostAlpha`'s default (ghost_armed)  */

/* THE DEPTH ROW'S KEY, emitUnits' `unit0 + rel * rowStep` (tools/tascene-render.js) at the stage's
   camera: the row the anchor's z falls in, truncated as ToInt32 truncates, clamped to the map, and
   counted from row 0 -- the base the features' tall keys take above (`tall_key`), so a unit and a
   tall feature of one row sort as the game's passes sort them */
static float row_key(float z, int h)
{
    int row = (int)z >> 4;
    if (row > h - 1) row = h - 1;
    if (row < 0) row = 0;
    return 1.0f + (float)row * 4.0f;
}

/* one record of type `t` for owner `o` at (x, z) facing (hx, hz); 0 when the type is a box or the
   owner none of the eight. THE YAW FROM THE VECTOR, with no angle (the plan's section 6): the
   model's +z turned onto (hx, hz) is the rotation (cs, sn) = (-hz, hx); at (0, 1) that is
   diag(-1, 1, -1), tagpu_native.c `ghost_one`'s body rotation for the default heading 0x8000. A
   building takes that heading whatever its facing says (0x485A40's centre). */
static int model(WSCENEMODEL* m, const WSCENETYPE* types, int ntype, int t, int o, float x, float z,
                 float hx, float hz, int h)
{
    const WSCENETYPE* ty;
    if (t < 0 || t >= ntype || !types[t].ok || o < 0 || o >= 8) return 0;
    ty = &types[t];
    memset(m, 0, sizeof *m);
    if (!ty->mobile) { hx = 0.0f; hz = 1.0f; }
    m->type = (uint8_t)t;
    m->owner = (uint8_t)o;
    m->slot = -1;
    m->x = x;
    m->z = z;
    m->cs = -hz;
    m->sn = hx;
    m->enc = row_key(z, h);
    m->alpha = 1.0f;
    m->shadow = (uint8_t)(!ty->shadow ? WSCENE_SH_NONE : ty->slant ? WSCENE_SH_SLANT : WSCENE_SH_SIL);
    return 1;
}

int tagpu_wikiscene_models(const WIKISTAGEVIEW* v, const WSCENETYPE* types, int ntype,
                           WSCENEMODEL* out, int* nunit)
{
    int n = 0, i, j;
    *nunit = 0;
    if (!v || v->w <= 0 || v->h <= 0 || !types) return 0;
    for (i = 0; i < v->nunit && i < WIKISTAGE_UNITS; i++) {
        const WIKISTAGEUNIT* u = &v->unit[i];
        WSCENEMODEL* m = &out[n];
        const float built = u->built < 0.0f ? 0.0f : u->built > 1.0f ? 1.0f : u->built;
        if (!model(m, types, ntype, u->type, u->owner, u->x, u->z, u->hx, u->hz, v->h)) continue;
        m->slot = (int16_t)i;
        /* A UNIT UNDER CONSTRUCTION is a nanoframe, staged from the fraction of the build
           remaining at the simulation's tick, and casts no shadow (tagpu_native.c, build-state.md
           7). A RECLAIM IS NOT ONE: the engine's ReclaimUnit 0x404730 advances it by a kind-5 hit
           0x489BB0 (0x404981) on the hit points, and neither it nor the hit's receiver 0x489CE0
           writes the fraction +0x104 (DISASSEMBLED), so the unit draws finished to the end. */
        if (built < 1.0f)
            m->nanoOn = (uint8_t)tagpu_nanostage(1.0f - built, (unsigned)i, (unsigned)v->tick,
                                                 &m->nanoT, m->nanoC, &m->nanoWire);
        if (m->nanoOn) m->shadow = WSCENE_SH_NONE;
        n++;
    }
    *nunit = n;
    /* THE GHOSTS, in ghost_record's order; never a nanoframe, never a shadow, untinted */
    if (v->has_ghost &&
        model(&out[n], types, ntype, v->ghost.type, 0, v->ghost.x, v->ghost.z, 0.0f, 1.0f, v->h))
        n++;
    for (i = 0; i < v->nsite && i < WIKISTAGE_SITES; i++)
        if (model(&out[n], types, ntype, v->site[i].type, 0, v->site[i].x, v->site[i].z, 0.0f, 1.0f, v->h))
            n++;
    /* a queued site with a unit of its type standing on its exact point is the started one, and
       the nanoframe itself is drawn there: the engine knows it from the order's own target
       (`started`); the view carries no such link, and the simulation places a new unit on its
       site's point, so the match is exact */
    for (i = 0; i < v->nqueued && i < WIKISTAGE_QUEUED; i++) {
        const WIKISTAGESITE* q = &v->queued[i];
        int standing = 0;
        for (j = 0; j < v->nunit && j < WIKISTAGE_UNITS && !standing; j++)
            standing = v->unit[j].type == q->type && v->unit[j].x == q->x && v->unit[j].z == q->z;
        if (!standing && model(&out[n], types, ntype, q->type, q->owner, q->x, q->z, 0.0f, 1.0f, v->h))
            n++;
    }
    for (i = *nunit; i < n; i++) {
        out[i].ghost = 1;
        out[i].alpha = GHOST_ALPHA;
        out[i].shadow = WSCENE_SH_NONE;
    }
    return n;
}
