/* tagpu_posebake.c — the per-type geometry bake and its caches.

   The design and the reasoning are research/notes/gpu-posing.md §3 and §4;
   tagpu_posebake.h carries the contract. What is here is the walk that turns a
   `Model3DONode` template into two vertex streams, the two caches over them,
   the four things that invalidate an entry, and the lever.

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
   the bake can be per type at all. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_model3do.h"
#include "tagpu_posebake.h"
#include "tagpu_render3do.h"
#include "tagpu_packet.h"   /* the piece run the bake keys on */
#include "tagpu_vk.h"       /* tagpu_vk_armed(): whether to pay for the mirrors */
#include "tagpu_log.h"

/* The two cache sizes and their keeps are tagpu_posebake.h's, which says why.
   Entries are allocated as they are baked and kept until evicted, so a session
   pays for the types it has actually drawn: a stock model bakes to ~2000
   vertices, 64 KB of geometry and 40 KB of material stream with their mirrors,
   so a material table at its keep is ~43 MB -- a 10-player game's cost, not a
   1v1's -- and only a frame that draws more streams than the keep holds more. */
#define PB_MAXGEOM  TAGPU_PB_MAXGEOM
#define PB_MAXMAT   TAGPU_PB_MAXMAT
#define PB_MAXVERT 49152         /* vertices one model may bake to           */
#define PB_MAXNODEV 4096         /* vertices in one piece (emit_node's bound) */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void blog(const char* s)
{
    tagpu_log(s);
}

/* ---- the lever ---------------------------------------------------------
   The bake runs whether or not `tagpu_posebake.on` exists. The lever only
   carries the `log` token, which writes a line per baked model and per
   material stream. Re-read on the same cadence as the pass's other levers. */
static int s_log, s_polled;

static void lever_read(void)
{
    char v[64];
    DWORD n;
    HANDLE h;
    s_log = 0;
    if (GetFileAttributesA("tagpu_posebake.on") == INVALID_FILE_ATTRIBUTES) return;
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

/* THERE IS NO `check` TOKEN: there is no CPU emitter left to compare the bake
   against. Gates B and D (gpu-posing.md §4 step 6) are the record that the
   bake and the emitter agreed — 0 differing pixels on eleven of twelve scenes.

   `posed_pose` duplicates `pose_accum_body`'s arithmetic, and nothing compares
   the two, so they can be reconciled — but that is its own decision with its
   own risk, not a free tidy. */

/* ---- the caches --------------------------------------------------------- */
static TAGPU_PBGEOM s_geom[PB_MAXGEOM];
static int          s_ngeom;                  /* slots ever used: the high-water mark */
static TAGPU_PBMAT  s_mat[PB_MAXMAT];
static unsigned char* s_matSkip[PB_MAXMAT];   /* per vertex, for the predictor */
/* THE LOOKUP IS A HASH, NOT A SCAN: a unit asks for its two entries every
   frame, and a scan of the table is O(units x entries), which the old table
   sizes kept affordable and the frame bound does not. Chains hold slot + 1, so
   0 ends one and a zeroed table is empty. A geometry entry hashes on its root
   and ghost flag, a material stream on its geometry's slot and its owner; the
   rest of each key is compared in the chain. */
#define PB_HBITS 16
#define PB_HSIZE (1u << PB_HBITS)
typedef char pb_chain_fits[(PB_MAXGEOM < 0xFFFF && PB_MAXMAT < 0xFFFF) ? 1 : -1];
static unsigned short s_geomHead[PB_HSIZE], s_geomNext[PB_MAXGEOM];
static unsigned short s_matHead[PB_HSIZE], s_matNext[PB_MAXMAT];
/* the free slots below each high-water mark, the live counts the keeps are
   tested on, and how many streams name each geometry entry (its drop scans
   the material table only when that is not 0) */
static unsigned short s_geomFree[PB_MAXGEOM], s_matFree[PB_MAXMAT];
static int          s_ngeomFree, s_nmatFree, s_geomLive, s_matLive;
static int          s_geomSaid, s_matSaid;    /* the peaks already logged */
static unsigned short s_geomNmat[PB_MAXGEOM];
/* ---- THE VULKAN LANE'S MIRRORS (Phase G, the unit pass) ----
   The bake's own output, copied out of the walk's scratch in the same call,
   once the latch below has armed -- tagpu_posebake.h says why, and
   tagpu_terr.c's `s_mirrorWant` is the pattern. Indexed by the same slot as
   the entry, freed by the same drop. */
static float*       s_geomMirror[PB_MAXGEOM];
static float*       s_matMirror[PB_MAXMAT];
static int          s_mirrorWant;             /* the Vulkan lane asked for them */
/* ONE COUNTER FOR BOTH TABLES, so that no geometry serial is ever a material
   serial and a ref handed to the wrong accessor cannot match. It starts at 1:
   0 is "never baked", which is what a zeroed entry reads. */
static unsigned     s_serial = 1;
static int          s_nmat;                   /* slots ever used: the high-water mark */
/* THE BAKE'S OWN FRAME, advanced by every tagpu_posebake_frame, so "stamped
   this frame" means "asked for since the last latch" however the render
   thread's counter moves; nothing but the eviction reads it */
static unsigned     s_frame;
/* THE FRAME'S generations, latched by tagpu_posebake_frame and used by every
   lookup in it. Re-reading them per unit would be a real hazard: the teardown pre
   hook gives up after a timeout and lets the cascade free the templates while a
   render pass is still running, so the generation can move MID-FRAME — and a
   lookup that saw the new one would re-bake from templates 0x42DB90 has just
   freed and stamp the new generation on the result, which is the stale-template
   bug the generation exists to prevent. cache_gen_check latches once per frame
   for exactly this reason, and this matches it. */
static unsigned     s_lvlGen, s_atlasGen;

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
typedef struct { int nv; int over; } PBGEOMCTX;

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

static unsigned pb_hash(unsigned a, unsigned b)
{
    return ((a ^ (b * 0x9E3779B9u)) * 0x85EBCA6Bu) >> (32 - PB_HBITS);
}

static unsigned geom_bucket(const TAGPU_PBGEOM* g)
{
    return pb_hash((unsigned)(size_t)g->root, (unsigned)g->ghost);
}

static unsigned mat_bucket(const TAGPU_PBMAT* m)
{
    return pb_hash((unsigned)(m->geom - s_geom), (unsigned)m->owner);
}

static void chain_unlink(unsigned short* head, unsigned short* next, unsigned b, int k)
{
    unsigned short* at = &head[b];
    while (*at && *at != (unsigned short)(k + 1)) at = &next[*at - 1];
    if (*at) *at = next[k];
    next[k] = 0;
}

static void mat_drop(int i)
{
    TAGPU_PBMAT* m = &s_mat[i];
    if (!m->geom) return;
    chain_unlink(s_matHead, s_matNext, mat_bucket(m), i);
    s_geomNmat[m->geom - s_geom]--;
    if (s_matSkip[i]) { free(s_matSkip[i]); s_matSkip[i] = NULL; }
    if (s_matMirror[i]) { free(s_matMirror[i]); s_matMirror[i] = NULL; }
    memset(m, 0, sizeof *m);
    s_matFree[s_nmatFree++] = (unsigned short)i;
    s_matLive--;
}

/* A material stream is only meaningful against the geometry it was walked
   beside, so dropping a geometry entry drops every stream that names it —
   otherwise the slot is re-baked for another type and a surviving stream would
   match a `geom ==` test against a model it has never seen. */
static int s_dropCascade;      /* material streams dropped WITH their geometry */

static void geom_drop(TAGPU_PBGEOM* g)
{
    int i, k = (int)(g - s_geom);
    if (!g->root) return;
    for (i = 0; i < s_nmat && s_geomNmat[k]; i++)
        if (s_mat[i].geom == g) { mat_drop(i); s_dropCascade++; }
    chain_unlink(s_geomHead, s_geomNext, geom_bucket(g), k);
    /* the mirror goes with the entry, and the serial in it is what stops a
       hand-over published before this from reading the next model's bytes */
    if (s_geomMirror[k]) { free(s_geomMirror[k]); s_geomMirror[k] = NULL; }
    free(g->restOff);                         /* the topology block */
    memset(g, 0, sizeof *g);
    s_geomFree[s_ngeomFree++] = (unsigned short)k;
    s_geomLive--;
}

/* THE SLOT A NEW ENTRY TAKES (tagpu_posebake.h says why). At the keep, the
   least recently used entry this frame has not asked for is evicted first;
   then a free slot, and a new one above the high-water mark only when there is
   none. Every entry this frame asks for is stamped with s_frame, a frame asks
   for at most TAGPU_PB_FRAMEMAX, and the table holds that many: so a table
   with every slot live has an unstamped entry to evict, a table with a slot
   not live has a free one, and the -1 is unreachable. The drop frees mirrors
   the Vulkan unit pass reads on the render thread, and every caller is on it. */
static int geom_slot(void)
{
    int i, worst = -1;
    if (s_geomLive >= TAGPU_PB_KEEPGEOM) {
        for (i = 0; i < s_ngeom; i++)
            if (s_geom[i].root && s_geom[i].lastFrame != s_frame &&
                (worst < 0 || s_geom[i].lastFrame < s_geom[worst].lastFrame)) worst = i;
        if (worst >= 0) geom_drop(&s_geom[worst]);
    }
    if (s_ngeomFree) return s_geomFree[--s_ngeomFree];
    if (s_ngeom < PB_MAXGEOM) return s_ngeom++;
    return -1;
}

static int mat_slot(void)
{
    int i, worst = -1;
    if (s_matLive >= TAGPU_PB_KEEPMAT) {
        for (i = 0; i < s_nmat; i++)
            if (s_mat[i].geom && s_mat[i].lastFrame != s_frame &&
                (worst < 0 || s_mat[i].lastFrame < s_mat[worst].lastFrame)) worst = i;
        if (worst >= 0) mat_drop(worst);
    }
    if (s_nmatFree) return s_matFree[--s_nmatFree];
    if (s_nmat < PB_MAXMAT) return s_nmat++;
    return -1;
}

/* the -1 above, which the bound makes unreachable, said once if it ever is */
static int s_saidFull;

static void say_full(const char* table)
{
    char b[160];
    if (s_saidFull) return;
    s_saidFull = 1;
    _snprintf(b, sizeof b, "posebake: the %s cache is full of entries this frame asked for "
              "(%u a frame is the bound) - the unit is not drawn", table,
              (unsigned)TAGPU_PB_FRAMEMAX);
    b[sizeof b - 1] = 0;
    blog(b);
}

static TAGPU_PBGEOM* geom_bake(const char* const* nd, int nparts, unsigned lvl,
                               int ghost)
{
    PBGEOMCTX c; PBWALKSTAT st;
    TAGPU_PBGEOM* g;
    unsigned char* topo;
    int r, slot;
    char b[192];
    c.nv = 0; c.over = 0; st.badNode = 0; st.oddFace = 0;
    slot = geom_slot();
    if (slot < 0) { say_full("geometry"); return NULL; }
    g = &s_geom[slot];
    memset(g, 0, sizeof *g);
    /* the topology, nparts entries of each array in one block: restOff first
       for its alignment, then parent, then done */
    topo = (unsigned char*)malloc((size_t)nparts * (sizeof(float[3]) + sizeof(short) + 1));
    if (!topo) {
        s_geomFree[s_ngeomFree++] = (unsigned short)slot;
        return NULL;
    }
    g->restOff = (float (*)[3])(void*)topo;
    g->parent = (short*)(void*)(topo + (size_t)nparts * sizeof(float[3]));
    g->done = (unsigned char*)(g->parent + nparts);
    g->root = nd[0]; g->levelGen = lvl; g->nparts = nparts;
    g->ghost = ghost;
    {
        const unsigned b = geom_bucket(g);
        s_geomNext[slot] = s_geomHead[b];
        s_geomHead[b] = (unsigned short)(slot + 1);
    }
    s_geomLive++;
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
        /* KEEP THE ENTRY, marked. Dropping it would leave `root == NULL`, which
           the lookup never matches — so the next frame would miss, take a fresh
           slot and walk the whole model again, once per frame for the life of
           the level, and log this line with it. Remembering the refusal costs
           one flag. The entry is dropped by the ordinary generation checks like any
           other, so a level change re-tries. */
        g->refused = 1;
        g->nvert = 0;
        return NULL;
    }
    g->nvert = c.nv;
    g->serial = s_serial++;
    /* THE BAKE IS THE PASS, AND THE MIRROR IS WHERE IT LANDS. `c` above is the
       walk's own scratch; the mirror takes it verbatim, in the same call, so
       there is no second evaluation of the bake to drift from the first. It is
       the only destination the bake has: the Vulkan unit pass reads it through
       `tagpu_posebake_geom_mirror`. A refused malloc leaves the slot NULL,
       which the accessor reports as "no mirror" and the Vulkan pass stands
       down on, visibly. */
    if (s_mirrorWant && c.nv > 0) {
        size_t nb = (size_t)c.nv * TAGPU_PB_GEOMST * sizeof(float);
        s_geomMirror[slot] = (float*)malloc(nb);
        if (s_geomMirror[slot]) memcpy(s_geomMirror[slot], s_scratchG, nb);
    }
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
typedef struct { int nv; int owner; int nskip; int anom; int over; int defer; } PBMATCTX;

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
        const int got = tg ? tagpu_r3d_atlas_uv(tg, uv, &ckf) : 0;
        if (got > 0) hasTex = 1;
        /* DEFERRED past the allowance: the bake is refused below, so this face
           is never baked flat for want of a texel that arrives next frame */
        if (got < 0) c->defer = 1;
        if (!hasTex) {
            int fc = tagpu_r3d_face_colour(fa);
            /* neither a texture the atlas has nor a flat colour: the engine's
               rasteriser paints nothing for this face and neither do we. It is
               baked and collapsed, so the two buffers stay the same length
               (gpu-posing.md §4). */
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
    c.nv = 0; c.owner = owner; c.nskip = 0; c.anom = 0; c.over = 0; c.defer = 0;
    for (r = 0; r < TAGPU_PB_NRANGE; r++)
        pb_walk(nd, g->nparts, r, mat_emit, &c, NULL);
    /* A FACE WHOSE TEXTURE WAITS FOR THE ALLOWANCE (tagpu_r3d_atlas_uv's -1):
       no material this frame, so the unit is not drawn, and the next frame
       bakes it again with the faces that painted here already in the atlas
       and the rest first in line (tagpu_gaf.h `budget`). Not an anomaly, so
       no line: the atlas logs the deferral. */
    if (c.defer) return NULL;
    /* THE INVARIANT the split rests on. If this ever fires, the two walks have
       drifted apart and the material stream would be read against the wrong
       vertices — refuse rather than publish a stream that lies. */
    if (c.over || c.nv != g->nvert) {
        _snprintf(b, sizeof b,
                  "posebake: REFUSED material root=%p owner=%d — %d vertices against the "
                  "geometry's %d; the two walks disagree",
                  (const void*)g->root, owner, c.nv, g->nvert);
        blog(b);
        return NULL;
    }
    slot = mat_slot();
    if (slot < 0) { say_full("material"); return NULL; }
    m = &s_mat[slot];
    memset(m, 0, sizeof *m);
    m->geom = g; m->root = g->root; m->owner = owner; m->atlasGen = atlasGen;
    m->levelGen = lvl;
    {
        const unsigned b = mat_bucket(m);
        s_matNext[slot] = s_matHead[b];
        s_matHead[b] = (unsigned short)(slot + 1);
    }
    s_geomNmat[g - s_geom]++;
    s_matLive++;
    m->nvert = c.nv; m->nskip = c.nskip;
    m->serial = s_serial++;
    if (s_mirrorWant && c.nv > 0) {
        size_t nb = (size_t)c.nv * TAGPU_PB_MATST * sizeof(float);
        s_matMirror[slot] = (float*)malloc(nb);
        if (s_matMirror[slot]) memcpy(s_matMirror[slot], s_scratchM, nb);
    }
    /* WHAT THE VERTEX LAYOUT IS, recorded because the consumer has to
       reproduce it and nothing else in this file states it. Locations 0-3 come
       off the geometry stream (the type's), 4-6 off this material stream -- the
       same split the two mirrors have, so either can be re-baked without
       touching the other. tagpu_vk_unit.c builds its own binding from the two
       mirrors. */
    s_matSkip[slot] = (unsigned char*)malloc((size_t)c.nv ? (size_t)c.nv : 1);
    if (s_matSkip[slot]) memcpy(s_matSkip[slot], s_scratchSkip, (size_t)c.nv);
    /* A FACE WITH NO MATERIAL IS ORDINARY, and measuring said so: all 67 models
       of the pose inventory have some (the footprint quad the slant raster
       fills and the body raster does not, among others), 2.7% of every baked
       vertex. The engine's own rasteriser skips them too. So this is a count,
       not a complaint, and it is logged only under `log`. */
    if (s_log) {
        _snprintf(b, sizeof b,
                  "posebake: material root=%p owner=%d atlas gen %u -> %d vert, "
                  "%d face(s) with no material collapsing %d vert",
                  (const void*)g->root, owner, atlasGen, m->nvert, c.anom, m->nskip);
        blog(b);
    }
    return m;
}

/* ---- the four invalidations --------------------------------------------- */
/* level teardown (node pointers are recycled by the next level), the mirror
   latch's first arming (below), an atlas recycle (every UV moves) and the
   owner (part of the material key). The first three are checked here, once per
   frame, because every lookup below is on the render thread inside the native
   pass. */
void tagpu_posebake_frame(unsigned frame_counter, unsigned level_gen)
{
    unsigned lvl = level_gen;
    unsigned agen = tagpu_r3d_atlas_gen();
    int i, dg = 0, dm = 0;
    /* the frame that just ended held more than a keep: said at each new peak,
       so a session that never needs more says nothing */
    if ((s_geomLive > TAGPU_PB_KEEPGEOM && s_geomLive > s_geomSaid) ||
        (s_matLive > TAGPU_PB_KEEPMAT && s_matLive > s_matSaid)) {
        char b[192];
        if (s_geomLive > s_geomSaid) s_geomSaid = s_geomLive;
        if (s_matLive > s_matSaid) s_matSaid = s_matLive;
        _snprintf(b, sizeof b, "posebake: a frame held %d geometry entries and %d material "
                  "streams (keep %d and %d, bound %u each)", s_geomLive, s_matLive,
                  TAGPU_PB_KEEPGEOM, TAGPU_PB_KEEPMAT, (unsigned)TAGPU_PB_FRAMEMAX);
        b[sizeof b - 1] = 0;
        blog(b);
    }
    s_dropCascade = 0;
    s_frame++;
    s_lvlGen = lvl; s_atlasGen = agen;
    /* the same 30-frame cadence tagpu_native.on is read on: this is a file
       probe, and one per frame is a syscall nobody asked for */
    if ((frame_counter % 30) == 0 || !s_polled) {
        lever_read(); s_polled = 1;
        /* THE MIRROR LATCH, on the first poll and once. A Vulkan pass runs in
           every process (`tagpu_vk_owns_present()` is always true), so it arms
           unconditionally, and it only ever goes 0 -> 1.

           EVERY ENTRY IS DROPPED ON THE TRANSITION, and that is the point
           rather than a cost: an entry baked before the lane came up has no
           mirror and would never grow one -- the bake is what takes it, and
           nothing re-bakes an entry that is still valid. Dropping them makes
           the next frame re-bake each type as it is asked for, with a mirror,
           instead of leaving the Vulkan pass permanently standing down on
           models that happen to have been on screen first. It costs one
           re-bake of what is visible, once. */
        if (!s_mirrorWant) {
            s_mirrorWant = 1;
            for (i = 0; i < s_nmat; i++)
                if (s_mat[i].geom) { mat_drop(i); dm++; }
            for (i = 0; i < s_ngeom; i++)
                if (s_geom[i].root) { geom_drop(&s_geom[i]); dg++; }
        }
    }
    /* THE STREAMS FIRST: a stream carries its geometry's level, so a level
       change drops every stream here and the geometry drops below find none
       left to scan the material table for */
    for (i = 0; i < s_nmat; i++)
        if (s_mat[i].geom && (s_mat[i].levelGen != lvl ||
                              s_mat[i].atlasGen != agen || !s_mat[i].geom->root)) {
            mat_drop(i); dm++;
        }
    for (i = 0; i < s_ngeom; i++)
        if (s_geom[i].root && s_geom[i].levelGen != lvl) {
            geom_drop(&s_geom[i]); dg++;
        }
    if (dg || dm || s_dropCascade) {
        char b[192];
        /* the cascade is reported separately or the line reads "0 material" on
           a level change that dropped every stream there was: a material goes
           with the geometry it was walked beside, before this loop ever sees
           it */
        _snprintf(b, sizeof b,
                  "posebake: dropped %d geometry (taking %d material with them) and %d material "
                  "in its own right — level %u, atlas %u",
                  dg, s_dropCascade, dm, lvl, agen);
        blog(b);
    }
}

/* ---- the Vulkan lane's mirrors ------------------------------------------
   The contract, and the whole lifetime argument, is in tagpu_posebake.h. Both
   accessors do the same three things: bound the pointer against the array it
   must be in, check that it lands ON an entry rather than inside one, and only
   then compare the serial. A pointer from an evicted and re-baked slot fails
   the third; a pointer from anywhere else fails the first two. */
static int in_table(const void* p, const void* base, size_t stride, int n)
{
    size_t off;
    if ((const char*)p < (const char*)base) return 0;
    off = (size_t)((const char*)p - (const char*)base);
    if (off % stride) return 0;
    return (int)(off / stride) < n;
}

const float* tagpu_posebake_geom_mirror(const TAGPU_PBGEOM* g, unsigned serial,
                                        int* nvert)
{
    int k;
    if (nvert) *nvert = 0;
    if (!g || !serial) return NULL;
    if (!in_table(g, s_geom, sizeof s_geom[0], PB_MAXGEOM)) return NULL;
    k = (int)(g - s_geom);
    if (g->serial != serial || !s_geomMirror[k] || g->nvert <= 0) return NULL;
    if (nvert) *nvert = g->nvert;
    return s_geomMirror[k];
}

const float* tagpu_posebake_mat_mirror(const TAGPU_PBMAT* m, unsigned serial,
                                       int* nvert)
{
    int k;
    if (nvert) *nvert = 0;
    if (!m || !serial) return NULL;
    if (!in_table(m, s_mat, sizeof s_mat[0], PB_MAXMAT)) return NULL;
    k = (int)(m - s_mat);
    if (m->serial != serial || !s_matMirror[k] || m->nvert <= 0) return NULL;
    if (nvert) *nvert = m->nvert;
    return s_matMirror[k];
}

/* ---- the lookup --------------------------------------------------------- */
int tagpu_posebake_unit(const TAGPU_PK_PIECE* pc, int nparts, int owner,
                        int ghost,
                        const TAGPU_PBGEOM** geomOut, const TAGPU_PBMAT** matOut)
{
    const char* nd[TAGPU_PBMAXPIECE];
    int i;
    unsigned lvl = s_lvlGen, agen = s_atlasGen;   /* the frame's, not a fresh read */
    TAGPU_PBGEOM* g = NULL;
    TAGPU_PBMAT* m = NULL;

    if (geomOut) *geomOut = NULL;
    if (matOut)  *matOut  = NULL;
    /* THE PIECE LIST COMES FROM THE FRAME PACKET. Each entry's
       `node` is the TYPE's Model3DONode — a per-level template the teardown
       cascade frees, which is what this module's fence argument has always
       been about — while a unit's Object3do is per UNIT and could be freed
       under this thread mid-frame. The publisher walked
       the prims on the game thread; nothing here dereferences a unit. */
    if (!pc || nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    for (i = 0; i < nparts; i++) {
        nd[i] = (const char*)(size_t)pc[i].node;
        if (!ptr_ok(nd[i]) || IsBadReadPtr(nd[i], N_CHILD + 4)) return 0;
    }
    /* THE KEY IS THE TEMPLATE, NOT THE UNIT — and `ghost` apart. Primitive 0's
       node identifies the tree every unit of the type shares (the same
       identity `pmap_for` uses), and the level generation says the tree is
       still the one it was baked from — a template is freed by the teardown
       cascade, not by any unit's destructor, so nothing else would notice its
       address being handed out again (thread-safe-destruction.md §6a). The
       ghost flag splits the entry in two: the ghost's synthesized run walks
       the tree in its own order, so its geometry stream's piece indices and
       `parent[]` do not line up with a live unit's prim-ordered run, and a
       shared entry would pose a building's parts with the wrong pieces'
       matrices. */
    {
        unsigned k = s_geomHead[pb_hash((unsigned)(size_t)nd[0], (unsigned)ghost)];
        for (; k; k = s_geomNext[k - 1]) {
            TAGPU_PBGEOM* e = &s_geom[k - 1];
            if (e->root == nd[0] && e->levelGen == lvl && e->nparts == nparts &&
                e->ghost == ghost) { g = e; break; }
        }
    }
    if (g && g->refused) { g->lastFrame = s_frame; return 0; }
    if (!g) {
        if (!tagpu_r3d_ready()) return 0;
        g = geom_bake(nd, nparts, lvl, ghost);
        if (!g) return 0;
    }
    g->lastFrame = s_frame;

    {
        unsigned k = s_matHead[pb_hash((unsigned)(g - s_geom), (unsigned)owner)];
        for (; k; k = s_matNext[k - 1]) {
            TAGPU_PBMAT* e = &s_mat[k - 1];
            if (e->geom == g && e->root == nd[0] && e->owner == owner &&
                e->atlasGen == agen) { m = e; break; }
        }
    }
    if (!m) {
        m = mat_bake(g, nd, owner, agen, lvl);
        if (!m) return 0;
    }
    m->lastFrame = s_frame;
    if (geomOut) *geomOut = g;
    if (matOut)  *matOut  = m;
    return 1;
}
