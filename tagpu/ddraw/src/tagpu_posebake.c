/* tagpu_posebake.c — the per-type geometry bake and its caches (G16 step 4).

   The design and the reasoning are research/notes/gpu-posing.md §3 and §4;
   tagpu_posebake.h carries the contract. What is here is the walk that turns a
   `Model3DONode` template into two GL vertex buffers, the two caches over them,
   the four things that invalidate an entry, and the lever that checks the bake
   against the CPU emitters it is going to replace.

   THE WALK IS THE POINT. `emit_node`, `emit_slant_at` and `emit_wire` each walk
   the same tree with slightly different rules, and the bake has to reproduce
   all three EXACTLY — the same faces, the same fan, the same corner-to-UV
   mapping, the same skips — or the GPU path draws a different model from the
   one it is being compared against. So there is ONE walk here, `pb_walk`, and
   both bakes and the predictor drive it. A rule that lives in one place cannot
   drift between the geometry buffer and the material stream, and the two
   therefore always agree about how many vertices there are, which is what makes
   them independently rebuildable (gpu-posing.md §4).

   WHAT THE WALK DOES NOT DECIDE is anything per UNIT. Piece visibility, the
   `cached` bit the slant tests, the body turn, the nano state, the team colour
   of a *pixel* — none of that is a property of the template, so none of it is
   baked. A hidden piece arrives at the shader as an all-zero matrix and
   collapses onto the model origin; that is the whole mechanism, and it is why
   the bake can be per type at all.

   NOTHING DRAWS FROM THESE BUFFERS YET. The posed program is step 5. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_model3do.h"
#include "tagpu_posebake.h"
#include "tagpu_render3do.h"
#include "tagpu_reclaim.h"

/* A 69-unit inventory of 67 distinct types filled a 64-entry table and started
   evicting, so both are set clear of a busy screen rather than at it. A stock
   model bakes to ~2000 vertices: 128 geometry entries is about 8 MB of static
   VBO and 256 material streams about 10 MB, against the 2.75 MB of vertices
   this pass re-uploads EVERY FRAME today. */
#define PB_MAXGEOM  128          /* types cached at once                     */
#define PB_MAXMAT   256          /* (type, owner) streams cached at once     */
#define PB_MAXVERT 49152         /* vertices one model may bake to           */
#define PB_MAXNODEV 4096         /* vertices in one piece (emit_node's bound) */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void blog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* ---- the lever ---------------------------------------------------------
   `tagpu_posebake.on` bakes for every unit the native pass draws and reports
   in the `native:` line. `check` adds the cross-check against the emitters
   (below), which costs a second walk per unit per frame and is a measurement,
   not a play setting. Re-read on the same cadence as the pass's other levers. */
static int s_armed, s_log, s_polled;

static void lever_read(void)
{
    char v[64];
    DWORD n;
    HANDLE h;
    s_armed = s_log = 0;
    if (GetFileAttributesA("tagpu_posebake.on") == INVALID_FILE_ATTRIBUTES) return;
    s_armed = 1;
    h = CreateFileA("tagpu_posebake.on", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    n = 0;
    if (ReadFile(h, v, sizeof v - 1, &n, NULL)) {
        v[n] = 0;
        if (strstr(v, "log"))   s_log = 1;
    }
    CloseHandle(h);
}

int tagpu_posebake_armed(void)    { return s_armed; }

/* THE `check` TOKEN IS GONE, with G16 step 8. Its two quantities were
   `emit_geom`'s body vertex count and `pose_accum_body`'s accumulated rest
   offsets, checked against the bake for the same unit on the same frame. The
   emitter that produced the first no longer exists, and the second was only
   reachable from the same call site, so what is left would be an oracle
   comparing the bake against nothing. Gates B and D (gpu-posing.md §4 step 6)
   are the record that the bake and the emitter agreed while both existed —
   0 differing pixels on eleven of twelve scenes.

   A CONSEQUENCE WORTH NAMING: `posed_pose` deliberately duplicates
   `pose_accum_body`'s arithmetic, and that duplication was load-bearing only
   because `check` compared the two. It is not any more, so the two can be
   reconciled — but that is its own decision with its own risk, not a free
   tidy, and it is not step 8's. */

/* ---- the caches --------------------------------------------------------- */
static TAGPU_PBGEOM s_geom[PB_MAXGEOM];
static int          s_ngeom;
static TAGPU_PBMAT  s_mat[PB_MAXMAT];
static unsigned char* s_matSkip[PB_MAXMAT];   /* per vertex, for the predictor */
static int          s_nmat;
static unsigned     s_glGen;                  /* bumped by tagpu_posebake_glreset */
static unsigned     s_frame;
/* THE FRAME'S generations, latched by tagpu_posebake_frame and used by every
   lookup in it. Re-reading them per unit was a real hazard: the teardown pre
   hook gives up after a timeout and lets the cascade free the templates while a
   render pass is still running, so the generation can move MID-FRAME — and a
   lookup that saw the new one would re-bake from templates 0x42DB90 has just
   freed and stamp the new generation on the result, which is the stale-template
   bug the generation exists to prevent. cache_gen_check latches once per frame
   for exactly this reason; this now matches it. */
static unsigned     s_lvlGen, s_atlasGen;
static int          s_anomTotal, s_oddTotal, s_collapsed, s_refused, s_baked, s_matBaked;

/* the bake's scratch: one model at a time, render thread only */
static float s_scratchG[PB_MAXVERT * TAGPU_PB_GEOMST];
static float s_scratchM[PB_MAXVERT * TAGPU_PB_MATST];
static unsigned char s_scratchSkip[PB_MAXVERT];

/* ---- the one walk ------------------------------------------------------- */
/* Emitted in range order; within a range, piece order, then face order, then
   the fan (or the edge loop). `vi` holds 3 vertex indices for a triangle and 2
   for a line; `slot` holds the fan slots the UV mapping needs (0, k, k+1). */
typedef void (*PB_EMIT)(void* ctx, int range, int p, const char* nd,
                        const int* rv, int nvert, const char* fa, int fvc,
                        const unsigned short* vi, const int* slot, int n);

typedef struct { int badNode, oddFace; } PBWALKSTAT;

static int pb_walk(const char* const* nd, int nparts, int range,
                   PB_EMIT emit, void* ctx, PBWALKSTAT* st)
{
    int p, produced = 0;
    for (p = 0; p < nparts; p++) {
        const char* n = nd[p];
        int nvert, nface, j, j0;
        const char* faces;
        const int* rv;
        if (!ptr_ok(n) || IsBadReadPtr(n, N_CHILD + 4)) { if (st) st->badNode++; continue; }
        nvert = *(const int*)(n + N_VCOUNT);
        nface = *(const int*)(n + N_FCOUNT);
        faces = *(const char* const*)(n + N_FACES);
        rv    = *(const int* const*)(n + N_VERTS);
        /* the emitters' own bounds, so a node they would skip is skipped here */
        if (nvert <= 0 || nvert > PB_MAXNODEV) continue;
        if (!ptr_ok(rv) || IsBadReadPtr(rv, (SIZE_T)nvert * 12)) { if (st) st->badNode++; continue; }
        if (nface <= 0 || nface > 512 || !ptr_ok(faces)) continue;
        if (IsBadReadPtr(faces, (SIZE_T)nface * FACE_STRIDE)) { if (st) st->badNode++; continue; }
        /* the slant raster skips face 0 when the node carries a selection
           primitive (0x45A610's rule); the body raster does not — `emit_geom_at`
           passes skipFace = -1 (emit_node's `skipFace` is the effects
           renderer's, not a unit's) */
        j0 = (range == TAGPU_PB_SLANT && *(const int*)(n + N_SELPRIM) != -1) ? 1 : 0;
        for (j = j0; j < nface; j++) {
            const char* fa = faces + j * FACE_STRIDE;
            int fvc = *(const int*)(fa + F_VCOUNT);
            const unsigned short* idx = *(const unsigned short* const*)(fa + F_INDICES);
            /* the emitters' own bound. A face outside it is CONTENT, not
               corruption — 14 of 67 stock models on the pose inventory carry
               one — so it is counted and not called an anomaly. */
            if (fvc < 3 || fvc > 32 || !ptr_ok(idx)) { if (st) st->oddFace++; continue; }
            if (IsBadReadPtr(idx, (SIZE_T)fvc * 2)) { if (st) st->oddFace++; continue; }
            if (range == TAGPU_PB_WIRE) {
                int e;
                for (e = 0; e < fvc; e++) {
                    unsigned short vi[2]; int slot[2];
                    vi[0] = idx[e]; vi[1] = idx[(e + 1) % fvc];
                    slot[0] = e; slot[1] = (e + 1) % fvc;
                    if (vi[0] >= nvert || vi[1] >= nvert) continue;
                    if (emit) emit(ctx, range, p, n, rv, nvert, fa, fvc, vi, slot, 2);
                    produced += 2;
                }
            } else {
                int k;
                for (k = 1; k + 1 < fvc; k++) {
                    unsigned short vi[3]; int slot[3];
                    vi[0] = idx[0];   slot[0] = 0;
                    vi[1] = idx[k];   slot[1] = k;
                    vi[2] = idx[k+1]; slot[2] = k + 1;
                    if (vi[0] >= nvert || vi[1] >= nvert || vi[2] >= nvert) continue;
                    if (emit) emit(ctx, range, p, n, rv, nvert, fa, fvc, vi, slot, 3);
                    produced += 3;
                }
            }
        }
    }
    return produced;
}

/* ---- the geometry bake -------------------------------------------------- */
typedef struct { int nv; int over; TAGPU_PBGEOM* g; } PBGEOMCTX;

static void geom_emit(void* vctx, int range, int p, const char* nd,
                      const int* rv, int nvert, const char* fa, int fvc,
                      const unsigned short* vi, const int* slot, int n)
{
    PBGEOMCTX* c = (PBGEOMCTX*)vctx;
    float V[3][3], nx = 0.0f, ny = 1.0f, nz = 0.0f;
    int shaded = 0, t, r;
    (void)nd; (void)nvert; (void)fa; (void)fvc; (void)slot;
    if (c->over) return;
    if (c->nv + n > PB_MAXVERT) { c->over = 1; return; }
    for (t = 0; t < n; t++)
        for (r = 0; r < 3; r++)
            V[t][r] = (float)rv[vi[t] * 3 + r] / 65536.0f;
    /* THE REST NORMAL, and why it may be baked at all: every face of a 3DO
       belongs to ONE piece, and a piece's transform is built by rotating basis
       vectors (`piece_local`) and composing those, so it is rigid. A rigid
       transform carries the rest normal to the posed normal and preserves its
       length — so the degeneracy test `emit_node` applies to the posed normal
       gives the same answer here, and the shader has only to transform, flip
       toward SH_V and normalise. The flip is NOT baked: it depends on the posed
       direction, which is what the piece matrix decides. */
    /* THE ONE PLACE THIS IS NOT EXACT. `emit_node` computes its `nl` from the
       ENGINE's posed vertices, which the compose has rounded into 16.16 at every
       axis and every level of the tree (`fistp`, 0x4B7173); the bake computes it
       from the rest vertices. For a face of any real area the two agree, but a
       NEAR-DEGENERATE one can land on the other side of `nl > 1e-6` there and
       take the neutral row where we take a shaded one, or the reverse. That is a
       whole face one SHD row off, which is what Gate B is told to look for. */
    if (range == TAGPU_PB_BODY) {
        float e1x = V[1][0]-V[0][0], e1y = V[1][1]-V[0][1], e1z = V[1][2]-V[0][2];
        float e2x = V[2][0]-V[0][0], e2y = V[2][1]-V[0][1], e2z = V[2][2]-V[0][2];
        float ax = e1y*e2z - e1z*e2y, ay = e1z*e2x - e1x*e2z, az = e1x*e2y - e1y*e2x;
        float nl = sqrtf(ax*ax + ay*ay + az*az);
        if (nl > 1e-6f) { nx = ax/nl; ny = ay/nl; nz = az/nl; shaded = 1; }
        else            { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
    }
    /* the slant and the wire are drawn on the neutral SHD row with a level
       normal, exactly as emit_slant_at and emit_wire write them */
    for (t = 0; t < n; t++) {
        float* o = s_scratchG + (size_t)(c->nv + t) * TAGPU_PB_GEOMST;
        o[0] = V[t][0]; o[1] = V[t][1]; o[2] = V[t][2];
        o[3] = nx; o[4] = ny; o[5] = nz;
        o[6] = (float)p;
        o[7] = shaded ? (float)TAGPU_PBF_SHADED : 0.0f;
    }
    /* the body range's rest AABB per piece — step 5's replacement for the
       `s_emitTop` emit_node kept while it wrote the posed vertices */
    if (range == TAGPU_PB_BODY && c->g && p >= 0 && p < TAGPU_PBMAXPIECE) {
        TAGPU_PBGEOM* g = c->g;
        for (t = 0; t < n; t++) {
            /* `pbody` is the SEEDED flag and must not be set until all three
               axes have been seeded from this first vertex. Setting it inside
               the r loop seeded x only: y and z then compared against the
               zeroed struct, so every piece's box was unioned with the origin
               plane and `tagpu_posedraw_top` could only read too tall — a
               wreck's shadow thrown too far, worst where the pose has
               M[5] < 0. */
            for (r = 0; r < 3; r++) {
                if (!g->pbody[p] || V[t][r] < g->pmn[p][r]) g->pmn[p][r] = V[t][r];
                if (!g->pbody[p] || V[t][r] > g->pmx[p][r]) g->pmx[p][r] = V[t][r];
            }
            g->pbody[p] = 1;
        }
    }
    c->nv += n;
}

/* the parent links and the accumulated rest offsets, off the template — the
   same walk `pose_accum_body` does per unit per frame, done once per type */
static void bake_topology(TAGPU_PBGEOM* g, const char* const* nd, int nparts)
{
    int i, pass, left;
    for (i = 0; i < nparts; i++) { g->parent[i] = -1; g->done[i] = 0; }
    for (i = 0; i < nparts; i++) {
        const char* ch = *(const char* const*)(nd[i] + N_CHILD);
        int sib, k;
        for (sib = 0; sib < nparts && ptr_ok(ch); sib++) {
            for (k = 0; k < nparts; k++)
                if (nd[k] == ch) { if (g->parent[k] < 0) g->parent[k] = (short)i; break; }
            if (IsBadReadPtr(ch, N_SIB + 4)) break;
            ch = *(const char* const*)(ch + N_SIB);
        }
    }
    left = nparts;
    for (pass = 0; pass < nparts && left > 0; pass++) {
        for (i = 0; i < nparts; i++) {
            const int* off;
            int k;
            if (g->done[i] || (g->parent[i] >= 0 && !g->done[g->parent[i]])) continue;
            off = (const int*)(nd[i] + N_OFF);
            for (k = 0; k < 3; k++)
                g->restOff[i][k] = (g->parent[i] < 0 ? 0.0f : g->restOff[g->parent[i]][k])
                                 + (float)off[k] / 65536.0f;
            g->done[i] = 1;
            left--;
        }
    }
    /* a piece whose parent link never resolved is the piece tree's problem, not
       the renderer's: it is left at rest (gpu-posing.md §3, "degrade inside the
       unit") and counted as an anomaly so the model says so once */
    for (i = 0; i < nparts; i++) if (!g->done[i]) g->orphan++;
}

static void mat_drop(int i)
{
    if (s_mat[i].vao) glDeleteVertexArrays(1, &s_mat[i].vao);
    if (s_mat[i].vbo) glDeleteBuffers(1, &s_mat[i].vbo);
    if (s_matSkip[i]) { free(s_matSkip[i]); s_matSkip[i] = NULL; }
    memset(&s_mat[i], 0, sizeof s_mat[i]);
}

/* A material stream is only meaningful against the geometry it was walked
   beside, so dropping a geometry entry drops every stream that names it —
   otherwise the slot is re-baked for another type and a surviving stream would
   match a `geom ==` test against a model it has never seen. */
static int s_dropCascade;      /* material streams dropped WITH their geometry */

static void geom_drop(TAGPU_PBGEOM* g)
{
    int i;
    for (i = 0; i < s_nmat; i++)
        if (s_mat[i].geom == g) { mat_drop(i); s_dropCascade++; }
    if (g->vbo) glDeleteBuffers(1, &g->vbo);
    memset(g, 0, sizeof *g);
}

/* evict the entry least recently asked for; GL deletion is render-thread only,
   and every caller of this is on it */
static int geom_slot(void)
{
    int i, worst = 0;
    if (s_ngeom < PB_MAXGEOM) return s_ngeom++;
    for (i = 1; i < PB_MAXGEOM; i++)
        if (s_geom[i].lastFrame < s_geom[worst].lastFrame) worst = i;
    geom_drop(&s_geom[worst]);
    return worst;
}

static int mat_slot(void)
{
    int i, worst = 0;
    if (s_nmat < PB_MAXMAT) return s_nmat++;
    for (i = 1; i < PB_MAXMAT; i++)
        if (s_mat[i].lastFrame < s_mat[worst].lastFrame) worst = i;
    mat_drop(worst);
    return worst;
}

static TAGPU_PBGEOM* geom_bake(const char* const* nd, int nparts, unsigned lvl)
{
    PBGEOMCTX c; PBWALKSTAT st;
    TAGPU_PBGEOM* g;
    int r, slot;
    char b[192];
    c.nv = 0; c.over = 0; st.badNode = 0; st.oddFace = 0;
    slot = geom_slot();
    g = &s_geom[slot];
    memset(g, 0, sizeof *g);
    g->root = nd[0]; g->levelGen = lvl; g->glGen = s_glGen; g->nparts = nparts;
    c.g = g;                    /* the per-piece rest AABB accumulates here */
    for (r = 0; r < TAGPU_PB_NRANGE; r++) {
        g->first[r] = c.nv;
        pb_walk(nd, nparts, r, geom_emit, &c, r == 0 ? &st : NULL);
        g->count[r] = c.nv - g->first[r];
    }
    g->badNode = st.badNode; g->oddFace = st.oddFace;
    bake_topology(g, nd, nparts);
    if (c.over) {
        _snprintf(b, sizeof b,
                  "posebake: REFUSED root=%p — %d pieces bake past the %d-vertex bound",
                  (const void*)nd[0], nparts, PB_MAXVERT);
        blog(b);
        /* KEEP THE ENTRY, marked. Dropping it left `root == NULL`, which the
           lookup never matches — so the next frame missed, took a fresh slot
           and walked the whole model again, once per frame for the life of the
           level, inflating `refused=` with it. Remembering the refusal costs
           one flag and makes the count mean "models refused", not "frames".
           The entry is dropped by the ordinary generation checks like any
           other, so a level or context change re-tries. */
        g->refused = 1;
        g->nvert = 0;
        s_refused++;
        return NULL;
    }
    g->nvert = c.nv;
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)c.nv * TAGPU_PB_GEOMST * sizeof(float),
                 s_scratchG, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_baked++;
    s_anomTotal += g->badNode + g->orphan;
    s_oddTotal  += g->oddFace;
    if (s_log) {
        _snprintf(b, sizeof b,
                  "posebake: root=%p %d piece(s) -> %d vert (body %d, slant %d, wire %d) odd-faces=%d",
                  (const void*)nd[0], nparts, g->nvert,
                  g->count[TAGPU_PB_BODY], g->count[TAGPU_PB_SLANT], g->count[TAGPU_PB_WIRE],
                  g->oddFace);
        blog(b);
    }
    /* ...but these two ARE anomalies, and they get a line whether or not the
       lever asked for logging: nothing in stock content produces either */
    if (g->badNode || g->orphan) {
        _snprintf(b, sizeof b,
                  "posebake: root=%p ANOMALY — %d piece(s) whose node or vertex array does not "
                  "read, %d whose parent link never resolved (left at rest)",
                  (const void*)nd[0], g->badNode, g->orphan);
        blog(b);
    }
    return g;
}

/* ---- the material stream ------------------------------------------------ */
typedef struct { int nv; int owner; int nskip; int anom; int over; } PBMATCTX;

static void mat_emit(void* vctx, int range, int p, const char* nd,
                     const int* rv, int nvert, const char* fa, int fvc,
                     const unsigned short* vi, const int* slot, int n)
{
    PBMATCTX* c = (PBMATCTX*)vctx;
    float uv[4], ckf = -1.0f, colv = 0.0f;
    int hasTex = 0, skip = 0, t;
    (void)p; (void)nd; (void)rv; (void)nvert; (void)vi;
    /* the geometry walk bounded itself and the two are supposed to agree, but
       the check that says so runs after this loop — so bound it here as well
       rather than trusting the invariant with the scratch buffer */
    if (c->over || c->nv + n > PB_MAXVERT) { c->over = 1; return; }
    /* The slant and the wire take no material from the face at all: the slant
       raster 0x4C1000 flat-fills every face it is given, and the wire is drawn
       in the nanoframe's animated blue, a per-unit uniform. What the WIRE still
       needs from the material is the emitter's own test — a face the engine
       paints nothing for gets no outline either — so the lookup runs for it and
       only its skip flag is kept. */
    if (range != TAGPU_PB_SLANT) {
        const char* tg = tagpu_r3d_face_texframe(fa, c->owner);
        if (tg && tagpu_r3d_atlas_uv(tg, uv, &ckf)) hasTex = 1;
        if (!hasTex) {
            int fc = tagpu_r3d_face_colour(fa);
            /* neither a texture the atlas has nor a flat colour: the engine's
               rasteriser paints nothing for this face and neither do we. Today
               `emit_node` drops it and the vertex count changes with the atlas;
               here it is baked and collapsed, so the two buffers stay the same
               length (gpu-posing.md §4). */
            if (fc < 0) {
                skip = 1;
                c->nskip += n;
                /* counted in the body range only: the same face comes back
                   once per fan triangle and once per wire edge, and an anomaly
                   is a fact about the FACE */
                if (range == TAGPU_PB_BODY && slot[0] == 0 && slot[1] == 1) c->anom++;
            }
            else        colv = (float)fc / 255.0f;
        }
    }
    for (t = 0; t < n; t++) {
        float* o = s_scratchM + (size_t)(c->nv + t) * TAGPU_PB_MATST;
        if (range == TAGPU_PB_BODY && hasTex) {
            /* the fan's corner onto the quad's UVs, emit_node's own mapping */
            int cc = fvc <= 4 ? slot[t] : slot[t] * 4 / fvc;
            o[0] = (cc == 1 || cc == 2) ? uv[2] : uv[0];
            o[1] = (cc >= 2)            ? uv[3] : uv[1];
            o[2] = 0.0f; o[3] = ckf;
        } else {
            o[0] = -1.0f; o[1] = -1.0f;          /* the flat path              */
            o[2] = (range == TAGPU_PB_BODY) ? colv : 0.0f;
            o[3] = -1.0f;
        }
        o[4] = skip ? 1.0f : 0.0f;
        s_scratchSkip[c->nv + t] = (unsigned char)skip;
    }
    c->nv += n;
}

static TAGPU_PBMAT* mat_bake(const TAGPU_PBGEOM* g, const char* const* nd,
                             int owner, unsigned atlasGen, unsigned lvl)
{
    PBMATCTX c;
    TAGPU_PBMAT* m;
    int r, slot;
    char b[192];
    c.nv = 0; c.owner = owner; c.nskip = 0; c.anom = 0; c.over = 0;
    for (r = 0; r < TAGPU_PB_NRANGE; r++)
        pb_walk(nd, g->nparts, r, mat_emit, &c, NULL);
    /* THE INVARIANT the split rests on. If this ever fires, the two walks have
       drifted apart and the material stream would be read against the wrong
       vertices — refuse rather than upload a buffer that lies. */
    if (c.over || c.nv != g->nvert) {
        _snprintf(b, sizeof b,
                  "posebake: REFUSED material root=%p owner=%d — %d vertices against the "
                  "geometry's %d; the two walks disagree",
                  (const void*)g->root, owner, c.nv, g->nvert);
        blog(b);
        s_refused++;
        return NULL;
    }
    slot = mat_slot();
    m = &s_mat[slot];
    memset(m, 0, sizeof *m);
    m->geom = g; m->root = g->root; m->owner = owner; m->atlasGen = atlasGen;
    m->levelGen = lvl; m->glGen = s_glGen;
    m->nvert = c.nv; m->nskip = c.nskip; m->noMaterial = c.anom;
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)c.nv * TAGPU_PB_MATST * sizeof(float),
                 s_scratchM, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    /* THE POSED PASS'S VAO (G16 step 5). The two buffers are bound together
       once, here, rather than re-pointed per unit per frame: a draw is then
       one bind and one glDrawArrays. Locations 0-3 come off the geometry (the
       type's), 4-6 off this stream — the same split the two buffers have, so
       either can be re-baked without touching the other's pointers. */
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glEnableVertexAttribArray(0);   /* rest position                        */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, TAGPU_PB_GEOMST * 4, (void*)0);
    glEnableVertexAttribArray(1);   /* rest normal of the vertex's face     */
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, TAGPU_PB_GEOMST * 4, (void*)12);
    glEnableVertexAttribArray(2);   /* piece index                          */
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, TAGPU_PB_GEOMST * 4, (void*)24);
    glEnableVertexAttribArray(3);   /* TAGPU_PBF_* flags                    */
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, TAGPU_PB_GEOMST * 4, (void*)28);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glEnableVertexAttribArray(4);   /* uv                                   */
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, TAGPU_PB_MATST * 4, (void*)0);
    glEnableVertexAttribArray(5);   /* flat colour, colour key              */
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, TAGPU_PB_MATST * 4, (void*)8);
    glEnableVertexAttribArray(6);   /* skip                                 */
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, TAGPU_PB_MATST * 4, (void*)16);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_matSkip[slot] = (unsigned char*)malloc((size_t)c.nv ? (size_t)c.nv : 1);
    if (s_matSkip[slot]) memcpy(s_matSkip[slot], s_scratchSkip, (size_t)c.nv);
    s_matBaked++;
    s_collapsed += m->noMaterial;
    /* A FACE WITH NO MATERIAL IS ORDINARY, and measuring said so: all 67 models
       of the pose inventory have some (the footprint quad the slant raster
       fills and the body raster does not, among others), 2.7% of every baked
       vertex. The engine's own rasteriser skips them too. So this is a count,
       not a complaint, and it is logged only under `log`. */
    if (s_log) {
        _snprintf(b, sizeof b,
                  "posebake: material root=%p owner=%d atlas gen %u -> %d vert, "
                  "%d face(s) with no material collapsing %d vert",
                  (const void*)g->root, owner, atlasGen, m->nvert, m->noMaterial, m->nskip);
        blog(b);
    }
    return m;
}

/* ---- the four invalidations --------------------------------------------- */
/* level teardown (node pointers are recycled by the next level), GL context
   loss (every id dies), an atlas recycle (every UV moves) and the owner (part
   of the material key). The first three are checked here, once per frame,
   because every lookup below is on the render thread inside the native pass. */
void tagpu_posebake_frame(unsigned frame_counter)
{
    unsigned lvl = tagpu_reclaim_level_gen();
    unsigned agen = tagpu_r3d_atlas_gen();
    int i, dg = 0, dm = 0;
    s_dropCascade = 0;
    s_frame = frame_counter;
    s_lvlGen = lvl; s_atlasGen = agen;
    /* the same 30-frame cadence tagpu_native.on is read on: this is a file
       probe, and one per frame is a syscall nobody asked for */
    if ((frame_counter % 30) == 0 || !s_polled) { lever_read(); s_polled = 1; }
    for (i = 0; i < s_ngeom; i++)
        if (s_geom[i].root && (s_geom[i].levelGen != lvl || s_geom[i].glGen != s_glGen)) {
            geom_drop(&s_geom[i]); dg++;
        }
    for (i = 0; i < s_nmat; i++)
        if (s_mat[i].geom && (s_mat[i].levelGen != lvl || s_mat[i].glGen != s_glGen ||
                              s_mat[i].atlasGen != agen || !s_mat[i].geom->root)) {
            mat_drop(i); dm++;
        }
    if (dg || dm || s_dropCascade) {
        char b[192];
        /* the cascade is reported separately or the line reads "0 material" on
           a GL reset that dropped every stream there was: a material goes with
           the geometry it was walked beside, before this loop ever sees it */
        _snprintf(b, sizeof b,
                  "posebake: dropped %d geometry (taking %d material with them) and %d material "
                  "in its own right — level %u, GL %u, atlas %u",
                  dg, s_dropCascade, dm, lvl, s_glGen, agen);
        blog(b);
    }
}

void tagpu_posebake_glreset(void)
{
    /* The context is gone, so the ids are already invalid and must NOT be
       deleted against the new one — forget them. The next frame's generation
       check drops the entries. */
    int i;
    for (i = 0; i < s_ngeom; i++) s_geom[i].vbo = 0;
    for (i = 0; i < s_nmat; i++)  { s_mat[i].vbo = 0; s_mat[i].vao = 0; }
    s_glGen++;
}

/* ---- the lookup --------------------------------------------------------- */
int tagpu_posebake_unit(const char* o3, int owner,
                        const TAGPU_PBGEOM** geomOut, const TAGPU_PBMAT** matOut)
{
    const char* nd[TAGPU_PBMAXPIECE];
    int nparts, i;
    unsigned lvl = s_lvlGen, agen = s_atlasGen;   /* the frame's, not a fresh read */
    TAGPU_PBGEOM* g = NULL;
    TAGPU_PBMAT* m = NULL;

    if (geomOut) *geomOut = NULL;
    if (matOut)  *matOut  = NULL;
    if (!ptr_ok(o3) || IsBadReadPtr(o3, O3_PRIM0)) return 0;
    nparts = *(const unsigned short*)(o3 + O3_NUMPARTS);
    if (nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    for (i = 0; i < nparts; i++) {
        const char* pr = o3 + O3_PRIM0 + i * PRIM_STRIDE;
        nd[i] = *(const char* const*)(pr + P_NODE);
        if (!ptr_ok(nd[i]) || IsBadReadPtr(nd[i], N_CHILD + 4)) return 0;
    }
    /* THE KEY IS THE TEMPLATE, NOT THE UNIT. Primitive 0's node identifies the
       tree every unit of the type shares (the same identity `pmap_for` uses),
       and the level generation says the tree is still the one it was baked
       from — a template is freed by the teardown cascade, not by any unit's
       destructor, so nothing else would notice its address being handed out
       again (thread-safe-destruction.md §6a). */
    for (i = 0; i < s_ngeom; i++)
        if (s_geom[i].root == nd[0] && s_geom[i].levelGen == lvl &&
            s_geom[i].glGen == s_glGen && s_geom[i].nparts == nparts) { g = &s_geom[i]; break; }
    if (g && g->refused) { g->lastFrame = s_frame; return 0; }
    if (!g) {
        if (!tagpu_r3d_ready()) return 0;
        g = geom_bake(nd, nparts, lvl);
        if (!g) return 0;
    }
    g->lastFrame = s_frame;

    for (i = 0; i < s_nmat; i++)
        if (s_mat[i].geom == g && s_mat[i].root == nd[0] && s_mat[i].owner == owner &&
            s_mat[i].atlasGen == agen && s_mat[i].glGen == s_glGen) { m = &s_mat[i]; break; }
    if (!m) {
        m = mat_bake(g, nd, owner, agen, lvl);
        if (!m) return 0;
    }
    m->lastFrame = s_frame;
    if (geomOut) *geomOut = g;
    if (matOut)  *matOut  = m;
    return 1;
}

int tagpu_posebake_stats(char* out, int n)
{
    if (!s_armed || n <= 0) { if (out && n > 0) out[0] = 0; return 0; }
    _snprintf(out, n, " bake=%d/%d anom=%d odd=%d nomat=%d refused=%d",
              s_ngeom, s_nmat, s_anomTotal, s_oddTotal, s_collapsed, s_refused);
    out[n - 1] = 0;             /* _snprintf does not terminate on truncation */
    return (int)strlen(out);
}
