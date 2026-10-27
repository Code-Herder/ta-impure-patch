/* tagpu_wikiworld.c -- the producer of the in-game wiki's 3D stage, and the inert answers the
   wiki's instances of the world passes take for the game-only modules. Contract:
   tagpu_wikiworld.h; the instancing itself is tagpu_scene.h.

   THE PROTOTYPES ARE THE INSTANCES' OWN. TAGPU_SCENE_WIKI is defined here before the headers, so
   each header below declares its function under the name tagpu_scene.h maps it to -- exactly the
   declaration the wiki's instance calls through. A definition here that disagreed with it in a
   parameter or a return type would not compile, so the two cannot drift apart. The same holds for
   tagpu_scene_wiki.h, the seam's declarations of the instances' entry points: it is included
   after the pass headers, so each of its prototypes meets the mapped one here.

   THE HAND-OVER SOURCES ANSWER FROM THE PRODUCER (tagpu_wikiworld.h): the terrain, the features,
   the target; the units' hand-over is the wiki's posed program's own, which this file feeds
   through the instances' entry points. EVERY OTHER ANSWER IS "NOTHING", and each is one the instance already takes from the
   game's own module on a frame with nothing to give: no fog grid for the posed program, no shadow
   map and no scaffold (a null view, which the passes replace with their own 1x1 dummy; an
   UNDEFINED shadow format, on which the passes give the dummy their own depth format and keep a
   NEAREST compare sampler they never sample); no effects models; Classic++ off and the restorer
   down, so no job is ever made; the A/B and growth levers unarmed. */

#define TAGPU_SCENE_WIKI
#include "tagpu_scene.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu.h"
#include "tagpu_terr.h"
#include "tagpu_feat.h"
#include "tagpu_native.h"
#include "tagpu_vk_shadow.h"
#include "tagpu_vk_scaffold.h"
#include "tagpu_vk_fx.h"
#include "tagpu_vk_restore.h"
#include "tagpu_restore_guard.h"
#include "tagpu_classicpp.h"
#include "tagpu_vk.h"
#include "tagpu_packet.h"
#include "tagpu_vk_world.h"
#include "tagpu_vk_terr.h"
#include "tagpu_vk_feat.h"
#include "tagpu_vk_unit.h"
#include "tagpu_posebake.h"
#include "tagpu_posedraw.h"
#include "tagpu_render3do.h"
#include "tagpu_model3do.h"
#include "tagpu_scene_wiki.h"
#include "tagpu_wikiworld.h"
#include "tagpu_wikiscene.h"
#include "tagpu_wikistate.h"
#include "tagpu_wikiasset.h"
#include "tagpu_hudart.h"
#include "tagpu_gaf.h"
#include "tagpu_glsl.h"
#include "tagpu_pal.h"
#include "tagpu_opt.h"
#include "tagpu_settings.h"
#include "tagpu_log.h"

/* ---- the producer ------------------------------------------------------------------------- */

/* tagpu_terr.c's atlas layout, which the terrain pass's vertex stage is written for (`CELL_PITCH`
   34, `CELL_BORDER` 1, 64 tiles a row): each tile in a cell with a replicated one-texel border */
#define TILE_PX     32
#define CELL_BORDER 1
#define CELL_PITCH  (TILE_PX + 2 * CELL_BORDER)
#define ATLAS_COLS  64
#define ATLAS_W     (ATLAS_COLS * CELL_PITCH)
#define DEPTH_SCALE 512.0f              /* the mockup's pack `depth.scale` (tools/wiki/gfx.py)     */
#define TERR_ENC    0.10f               /* tagpu_terr.c TERR_ENC: under every other band           */
#define FATLAS_DIM  512                 /* the features' frames: a deposit and a vent are ~64 px   */
#define FATLAS_MAX  128
#define FVST        TAGPU_FEAT_VST

static double ms_since(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0;
}

/* the lever, tagpu_wiki3d.on, on the passes' 30-frame beat */
#define NOUNIT_MAX 8
static int      s_leverOff, s_noMap, s_noFeat, s_noUnits, s_leverSeen = -1;
static unsigned s_leverAt, s_leverSerial;
static char     s_noUnit[NOUNIT_MAX][WIKI_ID_MAX];
static int      s_nNoUnit;

static void lever(unsigned frame)
{
    char buf[256], said[64 + NOUNIT_MAX * (WIKI_ID_MAX + 8)], nu[NOUNIT_MAX][WIKI_ID_MAX];
    int n, off = 0, nomap = 0, nofeat = 0, nounits = 0, nn = 0, now, k;
    if (s_leverSeen >= 0 && frame - s_leverAt < 30) return;
    s_leverAt = frame;
    n = tagpu_opt_read("tagpu_wiki3d.on", buf, sizeof buf);
    if (n > 0) {
        char* p = buf;
        buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
        while (*p) {
            char* q;
            int last;
            while (*p && *p <= ' ') p++;
            q = p;
            while (*q && *q > ' ') q++;
            last = *q == 0;
            *q = 0;
            if (!lstrcmpiA(p, "off")) off = 1;
            else if (!lstrcmpiA(p, "nomap")) nomap = 1;
            else if (!lstrcmpiA(p, "nofeat")) nofeat = 1;
            else if (!lstrcmpiA(p, "nounits")) nounits = 1;
            else if (!strncmp(p, "nounit=", 7) && p[7] && nn < NOUNIT_MAX) {
                lstrcpynA(nu[nn], p + 7, WIKI_ID_MAX);
                nn++;
            }
            if (last) break;
            p = q + 1;
        }
    }
    /* the state as one number: the four switches, and the names folded in, so a changed name is
       a change */
    now = off | nomap << 1 | nofeat << 2 | nounits << 3 | nn << 4;
    for (k = 0; k < nn; k++) {
        const char* c;
        for (c = nu[k]; *c; c++) now = (int)(((unsigned)now * 31u + (unsigned char)*c) & 0x7FFFFFFFu);
    }
    if (now != s_leverSeen) {
        int o = snprintf(said, sizeof said, "%s%s%s%s", off ? " off" : "", nomap ? " nomap" : "",
                         nofeat ? " nofeat" : "", nounits ? " nounits" : "");
        for (k = 0; k < nn && o >= 0 && o < (int)sizeof said; k++)
            o += snprintf(said + o, sizeof said - (size_t)o, " nounit=%s", nu[k]);
        said[sizeof said - 1] = 0;
        tagpu_logf("wiki3d: tagpu_wiki3d.on:%s", now ? said : " the 3D stage on");
        s_leverSerial++;
    }
    s_leverSeen = now;
    s_leverOff = off;
    s_noMap = nomap;
    s_noFeat = nofeat;
    s_noUnits = nounits;
    memcpy(s_noUnit, nu, sizeof nu);
    s_nNoUnit = nn;
}

/* THE GROUND, built once a stage: the scene's cells and quads, the terrain atlas, the feature
   atlas and each quad's atlas entry, and the vertices. Rebuilt only here, at a change of stage,
   before this frame's hand-overs are published. */
static const WIKI3DSTAGE* s_stage;      /* the ground built, or NULL                              */
static int                s_groundOk;
static WSCENE             s_scene;
static WSCENEFEAT         s_sfeat[WIKISCENE_FEATS];
static const unsigned char* s_sframe[WIKISCENE_FEATS][2][WIKISCENE_SUBS];  /* [feat][shadow][sub]       */
static unsigned char*     s_tatlas;     /* ATLAS_W x s_tatlasH palette indices                    */
static int                s_tatlasH;
static unsigned           s_tatlasSerial;
static short              s_cells[WIKISCENE_TILES * TAGPU_TERR_ICOMP];
static TAGPU_GAFENT       s_fents[FATLAS_MAX];
static TAGPU_GAFATLAS     s_fatlas;
static float*             s_fv[2];      /* shadows, bodies: 6 vertices a quad, FVST floats each    */
static int                s_fvCap[2];

/* the frame a feature's sequence draws: its first, as the mockup's pack takes it (gfx.py) and the
   engine's feature pass draws a static feature (tagpu_feat.c `tagpu_gaf_seq_frame(seq, 0)`) */
static int frames_of(const void* seq, const unsigned char** fr, WSCENEFRAME* geo, uint8_t* n)
{
    const unsigned char* g = seq ? tagpu_gaf_seq_frame((const char*)seq, 0) : NULL;
    TAGPU_GAFGEOM gg;
    int k, subs;
    *n = 0;
    if (!seq) return 1;
    if (!g || !tagpu_gaf_frame_geom(g, &gg)) return 0;
    subs = gg.subn;
    if (subs > WIKISCENE_SUBS) return 0;
    for (k = 0; k < (subs ? subs : 1); k++) {
        const unsigned char* s = subs ? tagpu_gaf_subframe(g, k) : g;
        TAGPU_GAFGEOM sg;
        if (!s || !tagpu_gaf_frame_geom(s, &sg)) return 0;
        fr[k] = s;
        geo[k].w = (int16_t)sg.w; geo[k].h = (int16_t)sg.h;
        geo[k].hx = (int16_t)sg.hotx; geo[k].hy = (int16_t)sg.hoty;
        geo[k].ck = sg.ck; geo[k].subalp = sg.subalp;
    }
    *n = (uint8_t)(subs ? subs : 1);
    return 1;
}

static int fatlas_ready(void)
{
    if (s_fatlas.made) return 1;
    tagpu_gaf_atlas_lost(&s_fatlas);
    s_fatlas.dim = FATLAS_DIM;
    s_fatlas.max = FATLAS_MAX;
    s_fatlas.ents = s_fents;
    s_fatlas.tag = "wikifeat";
    /* a world atlas: the key travels as a plane beside the indices, as the game's feature atlas's */
    s_fatlas.keyPlane = 1;
    if (!tagpu_gaf_atlas_create(&s_fatlas)) return 0;
    return tagpu_gaf_atlas_mirror(&s_fatlas);
}

/* the vertices of one bucket, emit_frame's six corners a quad (tagpu_feat.c), with its edge nudge
   at zoom 1 and each corner's own world point -- here its screen point, the eye and the viewport's
   corner both being the map's -- for the fog lookup */
static int verts(int b, const WSCENEQUAD* q, int n)
{
    const float nu = TAGPU_EDGE_NUDGE_PX;
    int i;
    if (n * 6 > s_fvCap[b]) {
        float* p = (float*)realloc(s_fv[b], (size_t)n * 6 * FVST * sizeof(float));
        if (!p) return 0;
        s_fv[b] = p;
        s_fvCap[b] = n * 6;
    }
    for (i = 0; i < n; i++) {
        const TAGPU_GAFENT* e;
        const unsigned char* g;
        float* o = s_fv[b] + (size_t)i * 6 * FVST;
        float x0 = q[i].x0 - nu, x1 = q[i].x1 - nu, y0 = q[i].y0 - nu, y1 = q[i].y1 - nu, c;
        int k;
        const float cx[6] = { x0, x1, x0, x1, x1, x0 }, cy[6] = { y0, y0, y1, y0, y1, y1 };
        if (q[i].feat < 0 || q[i].feat >= WIKISCENE_FEATS || q[i].sub >= WIKISCENE_SUBS) return 0;
        g = s_sframe[q[i].feat][b == 0][q[i].sub];
        if (!g || !(e = tagpu_gaf_atlas_get(&s_fatlas, g))) return 0;
        c = (float)e->ck / 255.0f;
        for (k = 0; k < 6; k++) {
            const int right = cx[k] == x1, low = cy[k] == y1;
            float* v = o + k * FVST;
            v[0] = cx[k]; v[1] = cy[k]; v[2] = q[i].enc;
            v[3] = right ? e->u1 : e->u0; v[4] = low ? e->v1 : e->v0;
            v[5] = c; v[6] = (float)q[i].mode;
            v[7] = cx[k]; v[8] = cy[k]; v[9] = 1.0f;
        }
    }
    return 1;
}

static const char* ground_build(const WIKI3DSTAGE* st)
{
    WSCENEGROUND g;
    int i, rows, h, maxdim = tagpu_vk_max_image_dim();
    unsigned char* atlas;
    memset(s_sframe, 0, sizeof s_sframe);
    if (st->nfeat < 0 || st->nfeat > WIKISCENE_FEATS) return "more features than the stage holds";
    if (st->ntile < 1 || st->ntile > 65536) return "no tile set";
    for (i = 0; i < st->nfeat; i++) {
        const WIKI3DFEAT* f = &st->feat[i];
        WSCENEFEAT* t = &s_sfeat[i];
        memset(t, 0, sizeof *t);
        t->cx = (int16_t)f->cx; t->cz = (int16_t)f->cz;
        t->fx = (int16_t)f->fx; t->fz = (int16_t)f->fz;
        t->height = (int16_t)f->height;
        t->flags = f->flags;
        /* the mockup's pack refuses the ground for one as well (tools/wiki/gfx.py), and the image is
           the mockup's: no stage description names one */
        if (f->model) return "a 3D feature, which the mockup's ground does not take either";
        if (!frames_of(f->seq, s_sframe[i][0], t->body, &t->nbody) ||
            !frames_of(f->seqshad, s_sframe[i][1], t->shadow, &t->nshadow))
            return "a feature's frame is not one the readers take";
    }
    g.w = st->w; g.h = st->h;
    g.ntile = st->ntile;
    g.tilemap = st->tilemap;
    g.nfeat = st->nfeat;
    g.feat = s_sfeat;
    if (!tagpu_wikiscene_ground(&g, &s_scene)) return "the ground is past a bound of the scene";
    if (s_scene.ncell * TAGPU_TERR_ICOMP > (int)(sizeof s_cells / sizeof s_cells[0])) return "too many cells";

    /* THE TERRAIN ATLAS, tagpu_terr.c `ensure_atlas`'s layout over the description's tile set */
    rows = (st->ntile + ATLAS_COLS - 1) / ATLAS_COLS;
    h = rows * CELL_PITCH;
    if (maxdim <= 0 || h > maxdim) return "the tile set is taller than the device's images";
    if (!(atlas = (unsigned char*)calloc((size_t)ATLAS_W * h, 1))) return "no memory for the terrain atlas";
    for (i = 0; i < st->ntile; i++) {
        const unsigned char* src = st->tiles + (size_t)i * TILE_PX * TILE_PX;
        unsigned char* cell = atlas + (size_t)(i / ATLAS_COLS) * CELL_PITCH * ATLAS_W + (size_t)(i % ATLAS_COLS) * CELL_PITCH;
        unsigned char* dst = cell + (size_t)CELL_BORDER * ATLAS_W + CELL_BORDER;
        int r;
        for (r = 0; r < TILE_PX; r++) {
            unsigned char* row = dst + (size_t)r * ATLAS_W;
            memcpy(row, src + (size_t)r * TILE_PX, TILE_PX);
            row[-1] = row[0];
            row[TILE_PX] = row[TILE_PX - 1];
        }
        memcpy(cell, dst - CELL_BORDER, CELL_PITCH);
        memcpy(cell + (size_t)(CELL_PITCH - 1) * ATLAS_W, dst + (size_t)(TILE_PX - 1) * ATLAS_W - CELL_BORDER, CELL_PITCH);
    }
    free(s_tatlas);
    s_tatlas = atlas;
    s_tatlasH = h;
    s_tatlasSerial++;
    for (i = 0; i < s_scene.ncell; i++) {
        short* o = s_cells + (size_t)i * TAGPU_TERR_ICOMP;
        o[0] = s_scene.cell[i].col; o[1] = s_scene.cell[i].row;
        o[2] = (short)(s_scene.cell[i].tile % ATLAS_COLS); o[3] = (short)(s_scene.cell[i].tile / ATLAS_COLS);
        o[4] = o[5] = o[6] = o[7] = 0;
    }

    /* THE FEATURE ATLAS: every frame of the ground painted now, on this thread, and the stage's
       quads' vertices built from their entries. The atlas has no allowance (`budget` 0): it holds
       a few frames, painted once a stage. */
    if (!fatlas_ready()) return "the feature atlas could not be laid out";
    tagpu_gaf_atlas_forget(&s_fatlas);
    tagpu_gaf_atlas_frame(&s_fatlas);
    if (!verts(0, s_scene.shadow, s_scene.nshadow) || !verts(1, s_scene.body, s_scene.nbody))
        return "a feature's frame did not fit the atlas";
    return NULL;
}

/* this frame's hand-overs and target */
static unsigned        s_frame;
static int             s_have, s_wantTerr, s_wantFeat, s_wantUnit, s_unitWhole, s_ground;
static int             s_terrHave, s_featHave, s_tgtHave;
static TAGPU_TERRHAND  s_terr;
static TAGPU_FEATHAND  s_feat;
static TAGPU_WORLDTGT  s_tgt;
static WSCENEFOG       s_fog;
/* the log's numbers */
static unsigned        s_saidAt, s_nFrames, s_nImage;
static double          s_msProd, s_msSeam, s_msMax;
static uint32_t        s_shotWant, s_shotDone;
static char            s_shotWhat[96];
static unsigned        s_nRec, s_nRefused, s_nUnitFrames;

/* ---- the units -------------------------------------------------------------------------------- */

/* THE ARENA'S ONE LEVEL: the arena is immutable and never freed, so what the wiki's bake keyed on
   it never goes stale. A constant, not the packet's: a level end in the game is no change to the
   stage, and the bake must not drop what it holds for one. */
#define WIKI_LEVEL_GEN  1u

/* the bytes of a Model3DONode the readers take (tagpu_model3do.h: its last field is N_CHILD) */
#define NODE_BYTES      (N_CHILD + 4)

/* Each of the view's types as the stage draws it, rebuilt when the entry, the arena or the lever
   changes: its record facts, the arena's unit, and its piece run -- the readers' walk of the
   model, every node of it inside the arena. */
typedef struct {
    const WIKI3DUNIT* u;                 /* NULL: a box                                          */
    int               np;
    const char*       node[TAGPU_PBMAXPIECE];
} TYPERUN;

static const WIKI3D* s_tA;
static int           s_tEntry = -1;
static unsigned      s_tLever = ~0u;
static WSCENETYPE    s_types[WIKI_MAX_DEFS];
static TYPERUN       s_run[WIKI_MAX_DEFS];
static uint32_t      s_resolved[(WIKI_MAX_DEFS + 31) / 32];
static WSCENEMODEL   s_rec[WIKISCENE_MODELS];

/* THE BOUND THE WIKI'S BAKE RESTS ON: node `p`'s bytes lie inside the arena. The arena is one
   block, published whole and never freed, and its loader wrote every pointer in it to point inside
   it (tagpu_wikiasset.h); this checks that a run handed to the bake names only that block, on the
   pointer's value, before the call. */
static int in_arena(const WIKI3D* a, const char* p)
{
    const char* base = (const char*)a;
    return p >= base && a->bytes >= NODE_BYTES && (size_t)(p - base) <= (size_t)a->bytes - NODE_BYTES;
}

static int unit_levered(const char* name)
{
    int k;
    for (k = 0; k < s_nNoUnit; k++)
        if (!strcmp(s_noUnit[k], name)) return 1;
    return 0;
}

static void types_build(const WIKI3D* a, const WIKISTAGEVIEW* v, int entry, const char* id)
{
    char in3d[512], boxes[512];
    int t, j, k, n3 = 0, nb = 0;
    in3d[0] = boxes[0] = 0;
    memset(s_types, 0, sizeof s_types);
    memset(s_resolved, 0, sizeof s_resolved);
    for (t = 0; t < v->ntype && t < WIKI_MAX_DEFS; t++) {
        TYPERUN* r = &s_run[t];
        const WIKI3DUNIT* u = NULL;
        r->u = NULL;
        r->np = 0;
        for (j = 0; j < a->nunit; j++)
            if (!strcmp(a->unit[j].name, v->type[t])) { u = &a->unit[j]; break; }
        if (!u || !u->ok || !u->model || unit_levered(v->type[t])) {
            nb += snprintf(boxes + nb, nb < (int)sizeof boxes ? sizeof boxes - (size_t)nb : 0, " %s", v->type[t]);
            if (nb > (int)sizeof boxes) nb = (int)sizeof boxes;
            continue;
        }
        r->np = tagpu_model_walk(u->model, r->node, TAGPU_PBMAXPIECE);
        for (k = 0; k < r->np; k++)
            if (!in_arena(a, r->node[k])) break;
        if (r->np <= 0 || k < r->np) {
            tagpu_logf("wiki3d: %s: the model's walk %s - drawn as its box", v->type[t],
                       r->np <= 0 ? "is refused" : "leaves the arena");
            r->np = 0;
            nb += snprintf(boxes + nb, nb < (int)sizeof boxes ? sizeof boxes - (size_t)nb : 0, " %s", v->type[t]);
            if (nb > (int)sizeof boxes) nb = (int)sizeof boxes;
            continue;
        }
        n3 += snprintf(in3d + n3, n3 < (int)sizeof in3d ? sizeof in3d - (size_t)n3 : 0, " %s", v->type[t]);
        if (n3 > (int)sizeof in3d) n3 = (int)sizeof in3d;
        r->u = u;
        s_types[t].ok = 1;
        s_types[t].mobile = u->mobile;
        s_types[t].slant = u->slant;
        s_types[t].shadow = u->shadow;
        s_resolved[t >> 5] |= 1u << (t & 31);
    }
    in3d[sizeof in3d - 1] = boxes[sizeof boxes - 1] = 0;
    tagpu_logf("wiki3d: %s: the types in 3D:%s; the boxes:%s", id, in3d[0] ? in3d : " none",
               boxes[0] ? boxes : " none");
    s_tA = a;
    s_tEntry = entry;
    s_tLever = s_leverSerial;
}

/* the engine's option word as the last packet carried it: bit 2 Shadow, bit 3 TShadow (the
   silhouette), bit 5 Shading (tagpu_packet.h `gfx_opt`). The wiki can be open with no packet; it
   keeps the last one's. */
static unsigned s_gfx;

/* One record's posed draw: the type's run baked, the rest pose turned onto its facing, and the
   record's fields as tagpu_native.c hands a unit or a ghost to the posed program. 0 when the
   bake refused it this frame. */
static int unit_one(const WSCENEMODEL* m, const TYPERUN* r)
{
    static TAGPU_PK_PIECE s_pc[TAGPU_PBMAXPIECE];
    static float s_pose[TAGPU_PBMAXPIECE * 12];
    static unsigned char s_shaded[TAGPU_PBMAXPIECE];
    static unsigned char s_pvis[TAGPU_PBMAXPIECE];
    const WIKI3DUNIT* u = r->u;
    const TAGPU_PBGEOM* bg;
    const TAGPU_PBMAT* bm;
    TAGPU_PDUNIT q;
    float rot[12];
    int i, k;
    if (!u || r->np <= 0) return 0;
    for (i = 0; i < r->np; i++) {
        memset(&s_pc[i], 0, sizeof s_pc[i]);
        s_pc[i].flags = 1;
        s_pc[i].node = (uint32_t)(size_t)r->node[i];
    }
    /* THE TEMPLATE WALK'S KEY (`ghost` 1), the one tagpu_native.c `ghost_one` takes for the same
       run: every run this bake is handed is the readers' walk of a model, so the units and the
       ghosts of a type share one geometry entry. The owner keys the material: its team faces'
       logo frame. */
    if (!tagpu_posebake_unit(s_pc, r->np, m->owner, 1, &bg, &bm) || bg->nparts != r->np ||
        bg->count[TAGPU_PB_BODY] <= 0)
        return 0;
    /* THE REST POSE TURNED ONTO THE FACING: ghost_one's arithmetic with the record's yaw (cs, sn)
       about the up axis as the body turn -- the rotation tools/tascene-render.js `emitUnits`
       applies, and at the default facing ghost_one's diag(-1, 1, -1) -- one rotation about the
       root's rest point, every piece at rest. A piece Create() hides takes an all-zero matrix and
       visibility 0; one it marks dont-cache draws but casts no slant (visibility 1, not 3:
       tagpu_native.c `posed_pose`'s words). */
    memset(rot, 0, sizeof rot);
    rot[0] = m->cs;  rot[2] = m->sn;
    rot[5] = 1.0f;
    rot[8] = -m->sn; rot[10] = m->cs;
    for (i = 0; i < bg->nparts; i++) {
        float* o = s_pose + (size_t)i * 12;
        const float* r0 = bg->restOff[0];
        float rel[3];
        for (k = 0; k < 3; k++) rel[k] = bg->restOff[i][k] - r0[k];
        for (k = 0; k < 3; k++) {
            o[k*4+0] = rot[k*4+0];
            o[k*4+1] = rot[k*4+1];
            o[k*4+2] = rot[k*4+2];
            o[k*4+3] = rot[k*4+0]*rel[0] + rot[k*4+1]*rel[1] + rot[k*4+2]*rel[2] + r0[k];
        }
        s_shaded[i] = 1;
        s_pvis[i] = 3;
        if (u->hides > 0 && (u->hidden[i >> 5] >> (i & 31) & 1u)) {
            memset(o, 0, 12 * sizeof *o);
            s_pvis[i] = 0;
        } else if (u->uncaches > 0 && (u->uncached[i >> 5] >> (i & 31) & 1u)) {
            s_pvis[i] = 1;
        }
    }
    memset(&q, 0, sizeof q);
    q.geom = bg; q.mat = bm;
    q.pose = s_pose; q.shaded = s_shaded; q.pvis = s_pvis;
    q.npose = bg->nparts;
    /* THE STAGE'S CAMERA: the eye and the viewport's corner at the map's, so the anchor is the
       record's world point; the stage is flat, so the projected z is z and the shadow's ground
       line is the anchor's own (shOffY 0) */
    q.ax = m->x; q.ay = m->z;
    q.wx0 = m->x; q.wz0 = m->z;
    q.enc = m->enc;
    q.alpha = m->alpha;
    q.ghost = m->ghost;
    q.fog = 0;                                /* units take no fog on the stage (section 7)   */
    q.waterT = -1e9f; q.digT = -1e9f;
    q.nanoOn = m->nanoOn;
    if (m->nanoOn) {
        q.nanoT = m->nanoT;
        q.nanoC[0] = m->nanoC[0]; q.nanoC[1] = m->nanoC[1]; q.nanoC[2] = m->nanoC[2];
        q.nanoWire = m->nanoWire;
    }
    q.cast[2] = 1.0f;
    q.castSkip = 1;                           /* there is no cast-shadow map on the stage      */
    /* THE ENGINE'S TWO HARD SHADOWS under its option word, tagpu_native.c's test with Classic's
       `hard`: a structure's slant under Shadow, a mobile unit's silhouette under Shadow and
       TShadow. The record carries the type's and the nanoframe's refusals already. */
    q.shKind = TAGPU_PDSH_NONE;
    if ((s_gfx & 4u) && m->shadow == WSCENE_SH_SLANT) q.shKind = TAGPU_PDSH_SLANT;
    else if ((s_gfx & 4u) && (s_gfx & 8u) && m->shadow == WSCENE_SH_SIL) q.shKind = TAGPU_PDSH_SIL;
    /* the engine's lit rasteriser for a structure under Shading, and for a ghost -- a preview of
       one -- under the same option (ghost_one) */
    q.lit = (s_gfx & 0x20u) && (m->ghost || !u->mobile);
    tagpu_posedraw_unit(&q);
    return 1;
}

/* THE UNITS' FRAME, every call in tagpu_native.c's order: the atlas's level and recycle before
   the bake latches its generation, the bake's and the posed program's frame, the atlas's
   allowance frame and the shade calibration, then the two windows -- the units', the ghosts'.
   Returns 1 when every record was handed over, 0 when the bake refused one (its texels not yet
   painted): the seam then does not draw this frame's hand-over (tagpu_wikiworld_wants). */
static int units_frame(const TAGPU_FRAME* f, int nrec, int nunit, int gw, int gh, int ss)
{
    TAGPU_PDVIEW pv;
    int i, refused = 0;
    tagpu_r3d_atlas_level(WIKI_LEVEL_GEN);
    tagpu_r3d_atlas_recycle();
    tagpu_posebake_frame(f->frame_counter, WIKI_LEVEL_GEN);
    tagpu_posedraw_frame(f->frame_counter);
    if (!tagpu_r3d_ensure()) return 0;
    tagpu_r3d_atlas_frame();
    tagpu_r3d_shade_want(f->packet ? tagpu_pk_shd(f->packet) : NULL);
    if (!nrec) return 1;
    if (!tagpu_posedraw_ready()) return 0;
    memset(&pv, 0, sizeof pv);
    pv.game[0] = (float)gw; pv.game[1] = (float)gh;
    pv.zoom = 1.0f;
    pv.zoomC[0] = (float)gw * 0.5f; pv.zoomC[1] = (float)gh * 0.5f;
    pv.depthScale = DEPTH_SCALE;
    pv.ss = (float)ss;
    pv.scafP[2] = (float)gw; pv.scafP[3] = (float)gh;
    /* CLASSIC'S LIGHT: taLambert of the level normal is exactly 1 (amb 1, norm 1), as level
       ground is in the game's Classic preset (tagpu_glsl.h) */
    pv.sun[1] = 1.0f;
    pv.amb = 1.0f;
    pv.norm = 1.0f;
    pv.shNeutral = tagpu_r3d_shade_neutral();
    if (nunit) {
        tagpu_posedraw_begin(&pv);
        for (i = 0; i < nunit; i++) refused += !unit_one(&s_rec[i], &s_run[s_rec[i].type]);
        tagpu_posedraw_end();
    }
    if (nrec > nunit) {
        tagpu_posedraw_begin_ghost(&pv);
        for (i = nunit; i < nrec; i++) refused += !unit_one(&s_rec[i], &s_run[s_rec[i].type]);
        tagpu_posedraw_end();
    }
    s_nRefused += refused != 0;
    return refused == 0;
}

static void fog_fields(float* org, float* cols, float* rows)
{
    org[0] = s_fog.orgx; org[1] = s_fog.orgz;
    *cols = (float)s_fog.cols;
    *rows = (float)s_fog.rows;
}

void tagpu_wikiworld_frame(const TAGPU_FRAME* f)
{
    const WIKISTAGEVIEW* v;
    const WIKI3D* a;
    const WIKIART* art;
    HUDRECT stage;
    LARGE_INTEGER t0;
    float u, org[2];
    int ox, oy, entry, demo, i, x0, y0, x1, y1, ss;
    uint32_t shot;
    const WIKI3DSTAGE* st = NULL;
    int gw, gh, nrec, nunit;

    s_have = s_terrHave = s_featHave = s_tgtHave = 0;
    QueryPerformanceCounter(&t0);
    lever(f->frame_counter);
    if (s_leverOff) return;
    if (!tagpu_wikistate_scene(f->frame_counter, &v, &entry, &demo, &shot, &stage, &u, &ox, &oy)) return;
    if (!(a = tagpu_wiki3d_get()) || !(art = tagpu_wikiart_get()) || !art->data) return;
    if (entry < 0 || entry >= art->data->nentry) return;
    for (i = 0; i < a->nstage; i++)
        if (!strcmp(a->stage[i].entry, art->data->entry[entry].id)) { st = &a->stage[i]; break; }
    if (v->w <= 0 || v->h <= 0 || v->w > WIKISCENE_MAP || v->h > WIKISCENE_MAP) return;
    if (f->packet) s_gfx = f->packet->gfx_opt;
    /* THE GROUND FALLS BACK WHOLE (the plan's section 3): a stage the loader could not resolve, or
       whose cells are not this view's map, is the whitebox's, and the units are drawn over it */
    if (!st || !st->ok || st->w != v->w || st->h != v->h) st = NULL;
    if (st && st != s_stage) {
        const char* why = ground_build(st);
        s_stage = st;
        s_groundOk = why == NULL;
        if (why)
            tagpu_logf("wiki3d: stage %s: the ground falls back to the whitebox - %s", st->entry, why);
        else
            tagpu_logf("wiki3d: stage %s: the ground in 3D - %d tiles, %d cells, %d feature quad(s) and %d "
                       "shadow(s), a %dx%d terrain atlas, %d feature frame(s)", st->entry, st->ntile,
                       s_scene.ncell, s_scene.nbody, s_scene.nshadow, ATLAS_W, s_tatlasH, s_fatlas.n);
    }
    s_ground = st && s_groundOk;

    /* THE MODEL RECORDS, of the types the arena resolved for this entry */
    if (a != s_tA || entry != s_tEntry || s_leverSerial != s_tLever) types_build(a, v, entry, art->data->entry[entry].id);
    nrec = tagpu_wikiscene_models(v, s_types, v->ntype < WIKI_MAX_DEFS ? v->ntype : WIKI_MAX_DEFS,
                                  s_rec, &nunit);
    if (!s_ground && !nrec) return;

    /* ONE ss FOR THE FRAME: the target is sized by it and every pass's hand-over carries it */
    ss = tagpu_settings_ss();
    gw = v->w * 16;
    gh = v->h * 16;
    s_wantTerr = s_ground;
    s_wantFeat = s_ground && s_scene.nshadow + s_scene.nbody > 0;
    s_wantUnit = nrec > 0;
    s_unitWhole = units_frame(f, nrec, nunit, gw, gh, ss) && !s_noUnits;
    s_nRec += (unsigned)nrec;
    s_nUnitFrames++;
    if (s_ground) {
        tagpu_wikiscene_fog(v->fog, v->w, v->h, &s_fog);
        tagpu_gaf_atlas_frame(&s_fatlas);
    }

    /* THE TERRAIN, every cell of the map at zoom 1: tagpu_terr.c `terr_publish`'s fields at the
       stage's camera -- grid cell (0, 0) is the map's, its corner the target's, no restore, no
       relief, no shadow map, the engine's fog grid when a cell is fogged */
    if (s_wantTerr && !s_noMap) {
        memset(&s_terr, 0, sizeof s_terr);
        s_terr.cells = s_cells; s_terr.ncell = s_scene.ncell;
        s_terr.gw = (float)s_scene.gw; s_terr.gh = (float)s_scene.gh;
        s_terr.zoom = 1.0f;
        s_terr.zoomCx = (float)s_scene.gw * 0.5f; s_terr.zoomCy = (float)s_scene.gh * 0.5f;
        s_terr.depthScale = DEPTH_SCALE;
        s_terr.enc = TERR_ENC;
        s_terr.texelW = 1.0f / (float)ATLAS_W; s_terr.texelH = 1.0f / (float)s_tatlasH;
        s_terr.fog = s_fog.on;
        fog_fields(org, &s_terr.fogCols, &s_terr.fogRows);
        s_terr.fogOrgX = org[0]; s_terr.fogOrgY = org[1];
        s_terr.norm = 1.0f;
        s_terr.mapPxW = (float)((v->w / 2) * 32); s_terr.mapPxH = (float)((v->h / 2) * 32);
        s_terr.atlas = s_tatlas; s_terr.atlasW = ATLAS_W; s_terr.atlasH = s_tatlasH;
        s_terr.atlasSerial = s_tatlasSerial;
        s_terr.pal = tagpu_pal_engine(); s_terr.palSerial = tagpu_pal_engine_serial();
        if (s_fog.on) {
            s_terr.fogGrid = s_fog.grid;
            s_terr.fogGridCols = s_fog.cols; s_terr.fogGridRows = s_fog.rows;
        }
        s_terr.vpL = 0; s_terr.vpT = 0; s_terr.vw = s_scene.gw; s_terr.vh = s_scene.gh;
        s_terr.scissorOn = 1;
        s_terr.ss = ss;
        s_terr.frame = f->frame_counter;
        s_terrHave = s_terr.pal != NULL;
    }
    /* THE FEATURES, both buckets as the ground's build left them, under the same fog */
    if (s_wantFeat && !s_noFeat) {
        memset(&s_feat, 0, sizeof s_feat);
        s_feat.shadow = s_fv[0]; s_feat.nShadow = s_scene.nshadow * 6;
        s_feat.body = s_fv[1]; s_feat.nBody = s_scene.nbody * 6;
        s_feat.gw = (float)s_scene.gw; s_feat.gh = (float)s_scene.gh;
        s_feat.zoom = 1.0f;
        s_feat.zoomCx = (float)s_scene.gw * 0.5f; s_feat.zoomCy = (float)s_scene.gh * 0.5f;
        s_feat.depthScale = DEPTH_SCALE;
        s_feat.fog = s_fog.on;
        fog_fields(org, &s_feat.fogCols, &s_feat.fogRows);
        s_feat.fogOrgX = org[0]; s_feat.fogOrgY = org[1];
        s_feat.mapPxW = (float)((v->w / 2) * 32); s_feat.mapPxH = (float)((v->h / 2) * 32);
        s_feat.nudgeW = TAGPU_EDGE_NUDGE_PX;
        tagpu_wikiworld_feat_atlas_hand(&s_feat);
        if (s_fog.on) {
            s_feat.fogGrid = s_fog.grid;
            s_feat.fogGridCols = s_fog.cols; s_feat.fogGridRows = s_fog.rows;
        }
        s_feat.vpL = 0; s_feat.vpT = 0; s_feat.vw = s_scene.gw; s_feat.vh = s_scene.gh;
        s_feat.scissorOn = 1;
        s_feat.ss = ss;
        s_feat.frame = f->frame_counter;
        s_featHave = s_feat.atlas && s_feat.pal;
    }
    /* THE TARGET: the map at the Supersampling setting's ss, composited into the image's rect --
       the fit the stage's ops are built with (tagpu_wikistage_image), on the attachment */
    tagpu_wikistage_image(v->w, v->h, stage, u, &x0, &y0, &x1, &y1);
    s_tgt.gw = gw; s_tgt.gh = gh;
    s_tgt.ss = ss;
    s_tgt.devres = 0;
    s_tgt.vx = ox + x0; s_tgt.vy = oy + y0;
    s_tgt.vw = x1 - x0; s_tgt.vh = y1 - y0;
    s_tgt.frame = f->frame_counter;
    s_tgtHave = s_tgt.vw > 0 && s_tgt.vh > 0;

    if (shot != s_shotWant) {
        s_shotWant = shot;
        snprintf(s_shotWhat, sizeof s_shotWhat, "%s demo %d at tick %d", art->data->entry[entry].id, demo + 1, (int)v->tick);
        s_shotWhat[sizeof s_shotWhat - 1] = 0;
    }
    s_frame = f->frame_counter;
    s_have = s_tgtHave;
    s_msProd += ms_since(t0);
}

int tagpu_wikiworld_wants(unsigned frame, WIKIWORLDWANT* w)
{
    if (!s_have || s_frame != frame) return 0;
    w->terr = s_wantTerr;
    w->feat = s_wantFeat;
    w->unit = s_wantUnit;
    w->unitWhole = s_unitWhole;
    w->ground = s_ground;
    w->resolved = s_resolved;
    return 1;
}

void tagpu_wikiworld_drawn(unsigned frame, int image, double ms)
{
    if (!s_have || s_frame != frame) return;
    s_nFrames++;
    s_nImage += image != 0;
    s_msSeam += ms;
    if (ms > s_msMax) s_msMax = ms;
    if (frame - s_saidAt >= 300) {
        tagpu_logf("wiki3d: frame %u: %s; %u of %u frame(s) drew the image; %.1f model record(s) a frame, "
                   "%u frame(s) with one the bake refused; render thread: the producer %.3f ms and the "
                   "passes %.3f ms a frame on average, the passes at most %.3f ms",
                   frame, image ? "the image" : "the whitebox (a pass the image needs did not prepare)",
                   s_nImage, s_nFrames, s_nUnitFrames ? (double)s_nRec / s_nUnitFrames : 0.0, s_nRefused,
                   s_msProd / s_nFrames, s_msSeam / s_nFrames, s_msMax);
        s_saidAt = frame;
        s_nFrames = s_nImage = 0;
        s_nRec = s_nRefused = s_nUnitFrames = 0;
        s_msProd = s_msSeam = s_msMax = 0;
    }
}

int tagpu_wikiworld_shot_due(unsigned frame)
{
    if (!s_have || s_frame != frame || !s_shotWant || s_shotWant == s_shotDone) return 0;
    DeleteFileA(WIKIWORLD_SHOT);
    return 1;
}

void tagpu_wikiworld_shot_armed(unsigned frame, unsigned w, unsigned h)
{
    s_shotDone = s_shotWant;
    tagpu_logf("wiki3d: shot %u: %s, the %ux%u target at ss %d, frame %u - written to %s", (unsigned)s_shotDone,
               s_shotWhat, w, h, s_tgt.ss, frame, WIKIWORLD_SHOT);
}

/* ---- the hand-over sources ---------------------------------------------------------------- */

/* EACH ONCE, AND ONLY ON ITS OWN FRAME: the rule tagpu_terr_handover states, and the reason is the
   same -- the pointers name this module's buffers as of the frame that published them */
int tagpu_wikiworld_terr_handover(TAGPU_TERRHAND* out, unsigned now)
{
    if (!s_terrHave || !out) return 0;
    s_terrHave = 0;
    if (s_terr.frame != now) return 0;
    *out = s_terr;
    return 1;
}

int tagpu_wikiworld_feat_handover(TAGPU_FEATHAND* out, unsigned now)
{
    if (!s_featHave || !out) return 0;
    s_featHave = 0;
    if (s_feat.frame != now) return 0;
    *out = s_feat;
    return 1;
}

/* the atlas protocol of tagpu_feat.h over the wiki's own atlas */
int tagpu_wikiworld_feat_atlas_hand(TAGPU_FEATHAND* h)
{
    TAGPU_GAFVIEW av;
    int ok = s_fatlas.made && tagpu_gaf_atlas_view(&s_fatlas, &av);
    if (!ok) return 0;
    h->atlas = av.idx; h->atlasDim = av.dim;
    h->atlasKey = av.key; h->atlasDirty = av.dirty;
    h->atlasRows = av.rows;
    h->atlasSerial = av.serial; h->atlasWhole = av.whole;
    h->atlasMoves = av.moves; h->atlasMoveN = av.moveN;
    h->atlasMoveSerial = av.moveSerial; h->atlasMovePrev = av.movePrev;
    h->pal = tagpu_pal_engine(); h->palSerial = tagpu_pal_engine_serial();
    return 1;
}

int tagpu_wikiworld_feat_atlas_owed(void)
{
    return s_fatlas.made && tagpu_gaf_atlas_owed(&s_fatlas);
}

void tagpu_wikiworld_feat_atlas_ack(unsigned serial, int keep)
{
    if (s_fatlas.made) tagpu_gaf_atlas_ack(&s_fatlas, serial, keep);
}

int tagpu_wikiworld_worldtgt(TAGPU_WORLDTGT* out)
{
    if (!s_tgtHave || !out) return 0;
    *out = s_tgt;
    return 1;
}

/* the fog grid the posed program would ask for (tagpu_posedraw.c): units take no fog on the stage
   (the plan's section 7), and the stage draws none */
const unsigned short* tagpu_wikiworld_foggrid(int* cols, int* rows, int* cells)
{
    if (cols)  *cols = 0;
    if (rows)  *rows = 0;
    if (cells) *cells = 0;
    return NULL;
}

/* the target's scissor is on: the stage's passes clip to the whole map, as the game's do to its
   viewport */
int tagpu_wikiworld_scissor_on(void)
{
    return 1;
}

/* ---- the game-only modules ---------------------------------------------------------------- */

VkFormat tagpu_wikiworld_shadow_format(const TAGPU_VKPASS* d, int* linearOk)
{
    (void)d;
    if (linearOk) *linearOk = 0;
    return VK_FORMAT_UNDEFINED;
}

int tagpu_wikiworld_shadow_ready(unsigned frame)
{
    (void)frame;
    return 0;
}

VkImageView tagpu_wikiworld_shadow_view(unsigned frame, uint32_t slot)
{
    (void)frame; (void)slot;
    return VK_NULL_HANDLE;
}

VkImageView tagpu_wikiworld_scaffold_view(unsigned frame, uint32_t slot)
{
    (void)frame; (void)slot;
    return VK_NULL_HANDLE;
}

int tagpu_wikiworld_fx_models_ok(unsigned frame)
{
    (void)frame;
    return 0;
}

int tagpu_wikiworld_classicpp_assets(void)
{
    return 0;
}

/* Only `aniso` is read, to build the restored twin's sampler, which the stage never samples:
   1 is the knob's "off". */
static const TAGPU_LIGHT s_classicLight = { .aniso = 1.0f };

const TAGPU_LIGHT* tagpu_wikiworld_classicpp_light(void)
{
    return &s_classicLight;
}

int tagpu_wikiworld_rguard_fault(const char* token)
{
    (void)token;
    return 0;
}

int tagpu_wikiworld_ab_arm(const char* tag)
{
    (void)tag;
    return 0;
}

int tagpu_wikiworld_grow_stress(void)
{
    return 0;
}

/* The restorer is down: `up` and `fits` refuse, so no instance makes a job. The job calls answer
   for a job that does not exist -- idle, failed, nothing painted -- and touch no argument. */

int tagpu_wikiworld_restore_up(const TAGPU_VKPASS* d)
{
    (void)d;
    return 0;
}

unsigned tagpu_wikiworld_restore_epoch(void)
{
    return 0;
}

int tagpu_wikiworld_restore_fits(const TAGPU_VKPASS* d, unsigned long long bytes, char* why,
                                 int whyLen)
{
    (void)d; (void)bytes;
    if (why && whyLen > 0) lstrcpynA(why, "the wiki's stage has no restorer", whyLen);
    return 0;
}

TAGPU_VKRJOB* tagpu_wikiworld_restore_job_new(const TAGPU_VKPASS* d, const char* tag,
                                              int prio, int oneshot, int model, int repaint,
                                              VkImage srcImg, VkImageView srcView,
                                              int srcW, int srcH, int srcBase,
                                              const unsigned char* pal,
                                              VkImage dstImg, VkImageView dstView,
                                              int dstW, int dstH)
{
    (void)d; (void)tag; (void)prio; (void)oneshot; (void)model; (void)repaint;
    (void)srcImg; (void)srcView; (void)srcW; (void)srcH; (void)srcBase; (void)pal;
    (void)dstImg; (void)dstView; (void)dstW; (void)dstH;
    return NULL;
}

int tagpu_wikiworld_restore_job_chain(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j, int mips, int dim,
                                      const VkImageView* attach, const VkImageView* sample)
{
    (void)d; (void)j; (void)mips; (void)dim; (void)attach; (void)sample;
    return 0;
}

int tagpu_wikiworld_restore_job_add(TAGPU_VKRJOB* j, const TAGPU_RGLSL_FRAME* frames, int count)
{
    (void)j; (void)frames; (void)count;
    return 0;
}

int tagpu_wikiworld_restore_job_add_nbhd(TAGPU_VKRJOB* j, const TAGPU_RNBFRAME* frames, int count)
{
    (void)j; (void)frames; (void)count;
    return 0;
}

void tagpu_wikiworld_restore_job_budget(TAGPU_VKRJOB* j, int prio, double capMs)
{
    (void)j; (void)prio; (void)capMs;
}

int tagpu_wikiworld_restore_job_queued(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 0;
}

void tagpu_wikiworld_restore_job_feeding(TAGPU_VKRJOB* j, int more)
{
    (void)j; (void)more;
}

int tagpu_wikiworld_restore_job_idle(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 1;
}

int tagpu_wikiworld_restore_job_failed(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 1;
}

int tagpu_wikiworld_restore_job_painted(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 0;
}

int tagpu_wikiworld_restore_job_remap(TAGPU_VKRJOB* j, int (*map)(void* ctx, TAGPU_RGLSL_FRAME* f),
                                      void* ctx, int* kept, int* requeued, int* dropped)
{
    (void)j; (void)map; (void)ctx;
    if (kept)     *kept = 0;
    if (requeued) *requeued = 0;
    if (dropped)  *dropped = 0;
    return 0;
}

int tagpu_wikiworld_restore_job_dst_live(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 0;
}

void tagpu_wikiworld_restore_job_free(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j)
{
    (void)d; (void)j;
}
