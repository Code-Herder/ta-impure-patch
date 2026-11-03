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
   game's own module on a frame with nothing to give: no shadow
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
#include "tagpu_vk_fog.h"
#include "tagpu_vk_terr.h"
#include "tagpu_vk_feat.h"
#include "tagpu_vk_unit.h"
#include "tagpu_posebake.h"
#include "tagpu_posedraw.h"
#include "tagpu_render3do.h"
#include "tagpu_model3do.h"
#include "tagpu_scene_wiki.h"
#include "tagpu_posemath.h"
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
    int n, off = 0, nomap = 0, nofeat = 0, nounits = 0, nn = 0, now, k, changed;
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
    /* a change is the switches or the names themselves, compared in full */
    now = off | nomap << 1 | nofeat << 2 | nounits << 3 | nn << 4;
    changed = now != s_leverSeen;
    for (k = 0; k < nn && !changed; k++) changed = strcmp(nu[k], s_noUnit[k]) != 0;
    if (changed) {
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
static int                s_stageRows;      /* the rows of the map s_stage's ground was built for */
static WSCENE             s_scene;
static WSCENEFEAT         s_sfeat[WIKISCENE_FEATS];
static const unsigned char* s_sframe[WIKISCENE_FEATS][2][WIKISCENE_SUBS];  /* [feat][shadow][sub]       */
/* the ground's 3D features, drawn through the unit pass (wrecks_build): each one's cell and
   footprint, and its arena feature, in the description's order */
static WSCENEWRECK        s_wreck[WIKISCENE_WRECKS];
static const WIKI3DFEAT*  s_wfeat[WIKISCENE_WRECKS];
static int                s_nwreck;
static unsigned char*     s_tatlas;     /* ATLAS_W x s_tatlasH palette indices                    */
static int                s_tatlasH;
static unsigned           s_tatlasSerial;
/* THE STAGE'S TILE MAP, the terrain draw's: every 32-px cell's tile index (the scene's cells,
   WSCENECELL), `s_tmW` x `s_tmH`, and the serial the Vulkan lane re-uploads it on */
static unsigned short     s_tmap[WIKISCENE_TILES];
static int                s_tmW, s_tmH;
static unsigned           s_tmSerial;
static TAGPU_GAFENT       s_fents[FATLAS_MAX];
static TAGPU_GAFATLAS     s_fatlas;
static float*             s_fv[2];      /* shadows, bodies: 6 vertices a quad, FVST floats each    */
static int                s_fvCap[2];
/* the vertices of the features still standing, this frame's, when the view has reclaimed some:
   sized with s_fv by the ground's build, so a frame never allocates */
static float*             s_fl[2];
static int                s_flCap[2];
static uint8_t            s_dead[WIKISCENE_FEATS];

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

/* the ground of a map `map_rows` tall, the top of the stage's (tagpu_wikiscene_ground) */
static const char* ground_build(const WIKI3DSTAGE* st, int map_rows)
{
    WSCENEGROUND g;
    int i, rows, h, n2d = 0, maxdim = tagpu_vk_max_image_dim();
    unsigned char* atlas;
    memset(s_sframe, 0, sizeof s_sframe);
    s_nwreck = 0;
    if (st->nfeat < 0 || st->nfeat > WIKISCENE_FEATS) return "more features than the stage holds";
    if (st->ntile < 1 || st->ntile > 65536) return "no tile set";
    for (i = 0; i < st->nfeat; i++) {
        const WIKI3DFEAT* f = &st->feat[i];
        WSCENEFEAT* t = &s_sfeat[n2d];
        /* A 3D FEATURE (its section names a model) is no part of the ground: the unit pass draws it
           (tagpu_wikiscene_models), as tools/wiki/gfx.py leaves it out of the pack's ground and
           lists it in features3d */
        if (f->model) {
            WSCENEWRECK* w = &s_wreck[s_nwreck];
            if (s_nwreck >= WIKISCENE_WRECKS) return "more 3D features than the stage holds";
            w->cx = (int16_t)f->cx; w->cz = (int16_t)f->cz;
            w->fx = (int16_t)f->fx; w->fz = (int16_t)f->fz;
            s_wfeat[s_nwreck++] = f;
            continue;
        }
        memset(t, 0, sizeof *t);
        t->cx = (int16_t)f->cx; t->cz = (int16_t)f->cz;
        t->fx = (int16_t)f->fx; t->fz = (int16_t)f->fz;
        t->height = (int16_t)f->height;
        t->flags = f->flags;
        if (!frames_of(f->seq, s_sframe[n2d][0], t->body, &t->nbody) ||
            !frames_of(f->seqshad, s_sframe[n2d][1], t->shadow, &t->nshadow))
            return "a feature's frame is not one the readers take";
        n2d++;
    }
    g.w = st->w; g.h = map_rows;
    g.ntile = st->ntile;
    g.tilemap = st->tilemap;
    g.nfeat = n2d;
    g.feat = s_sfeat;
    if (!tagpu_wikiscene_ground(&g, &s_scene)) return "the ground is past a bound of the scene";
    /* the scene's cells are every cell of the map's tile grid, row by row (tagpu_wikiscene_ground) */
    if (s_scene.ncell != (st->w / 2) * (map_rows / 2) || s_scene.ncell > WIKISCENE_TILES)
        return "the ground's cells are not its tile grid";

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
    s_tmW = st->w / 2;
    s_tmH = map_rows / 2;
    for (i = 0; i < s_scene.ncell; i++) {
        const WSCENECELL* c = &s_scene.cell[i];
        if (c->col < 0 || c->col >= s_tmW || c->row < 0 || c->row >= s_tmH)
            return "a ground cell is outside its tile grid";
        s_tmap[(size_t)c->row * s_tmW + c->col] = c->tile;
    }
    s_tmSerial++;

    /* THE FEATURE ATLAS: every frame of the ground painted now, on this thread, and the stage's
       quads' vertices built from their entries. The atlas has no allowance (`budget` 0): it holds
       a few frames, painted once a stage. */
    if (!fatlas_ready()) return "the feature atlas could not be laid out";
    tagpu_gaf_atlas_forget(&s_fatlas);
    tagpu_gaf_atlas_frame(&s_fatlas);
    if (!verts(0, s_scene.shadow, s_scene.nshadow) || !verts(1, s_scene.body, s_scene.nbody))
        return "a feature's frame did not fit the atlas";
    for (i = 0; i < 2; i++) {
        const int n = (i ? s_scene.nbody : s_scene.nshadow) * 6;
        if (n > s_flCap[i]) {
            float* p = (float*)realloc(s_fl[i], (size_t)n * FVST * sizeof(float));
            if (!p) return "no memory for the features' vertices";
            s_fl[i] = p;
            s_flCap[i] = n;
        }
    }
    return NULL;
}

/* THE FEATURES STANDING: each bucket's quads whose feature is not dead, their vertices copied in
   order into s_fl, which the ground's build sized for every quad; *n their vertices */
static const float* live_verts(int b, int* n)
{
    const WSCENEQUAD* q = b ? s_scene.body : s_scene.shadow;
    const int nq = b ? s_scene.nbody : s_scene.nshadow;
    int i, m = 0;
    for (i = 0; i < nq; i++) {
        if (q[i].feat >= 0 && q[i].feat < WIKISCENE_FEATS && s_dead[q[i].feat]) continue;
        memcpy(s_fl[b] + (size_t)m * 6 * FVST, s_fv[b] + (size_t)i * 6 * FVST, 6 * FVST * sizeof(float));
        m++;
    }
    *n = m * 6;
    return s_fl[b];
}

/* this frame's hand-overs and target */
static unsigned        s_frame;
static int             s_have, s_wantTerr, s_wantFeat, s_wantUnit, s_unitWhole, s_ground;
static int             s_terrHave, s_featHave, s_tgtHave;
static TAGPU_TERRHAND  s_terr;
static TAGPU_FEATHAND  s_feat;
static TAGPU_WORLDTGT  s_tgt;
/* THE STAGE'S FOG SOURCE (tagpu_wikiworld_fogsrc): `s_fog` the bits the seam's fog image holds,
   kept while the next frame's (`s_fogNew`) are the same so the serial moves only when they change,
   and `s_fogHave` this frame's answer */
static WSCENEFOG       s_fog, s_fogNew;
static TAGPU_FOGSRC    s_fogSrc;
static unsigned        s_fogSerial;
static int             s_fogHave;
static uint32_t        s_shotWant, s_shotDone;
static char            s_shotWhat[96];

/* THE LOG'S NUMBERS, each over a window of the stage's drawn frames: the periodic line's, 300 frames
   of the frame counter, and the count's (THE COUNT, below). The producer leaves its frame's part in
   s_fr*, which tagpu_wikiworld_drawn adds to both windows when that frame is drawn. */
typedef struct {
    unsigned frames, image, rec, unitFrames, refused;
    double   prod, seam, max;
} WINSTAT;
static WINSTAT         s_per, s_cnt;
static unsigned        s_saidAt;
static unsigned        s_frRec, s_frUnit, s_frRefused;
static double          s_frProd;
/* THE STAGE'S WORK, each count only ever growing: the grounds built (ground_build) and the type
   records built (types_build); the bake's entries baked and dropped are its own
   (tagpu_posebake_work). A stage that is up does none of it frame to frame. */
enum { WORK_GROUND, WORK_TYPES, WORK_BAKED, WORK_DROPPED, WORK_N };
static unsigned        s_nGround, s_nTypes;

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
/* the view's type names the records were built for: the casting's (tagpu_wikisim.h), which a fill
   on another side or Interface Type changes under the same entry */
static int           s_tNtype = -1;
static char          s_tNames[WIKI_MAX_DEFS][WIKISTAGE_LABEL];
static WSCENETYPE    s_types[WIKI_MAX_DEFS];
static TYPERUN       s_run[WIKI_MAX_DEFS];
static uint32_t      s_resolved[(WIKI_MAX_DEFS + 31) / 32];
static WSCENEMODEL   s_rec[WIKISCENE_MODELS];

/* Node `p`'s bytes lie inside the arena's used extent, [base, base + bytes). What makes the bake read
   the arena only is the loader's construction: it wrote every pointer in the arena, each to point
   inside it, and copies no pointer-sized word of a file that a reader follows (tagpu_wikiasset.h).
   The walk that made the run already followed those pointers; this re-checks the run's nodes, on
   their values, before the bake is handed them, so a loader that broke its rule makes a box. */
static int in_arena(const WIKI3D* a, const char* p)
{
    const uint32_t v = (uint32_t)(size_t)p;
    return v >= a->base && a->bytes >= NODE_BYTES && v - a->base <= a->bytes - NODE_BYTES;
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
    s_nTypes++;
    s_tA = a;
    s_tEntry = entry;
    s_tLever = s_leverSerial;
    s_tNtype = v->ntype < WIKI_MAX_DEFS ? v->ntype : WIKI_MAX_DEFS;
    memcpy(s_tNames, v->type, sizeof s_tNames);
}

/* THE 3D FEATURES' RUNS, for the ground just built: each model's walk, every node inside the
   arena, as types_build makes a type's. A model the walk refuses or that leaves the arena refuses
   the ground whole, as gfx.py falls the mockup's ground back for a model it cannot resolve. */
static TYPERUN s_wrun[WIKISCENE_WRECKS];
static const char* wrecks_build(const WIKI3D* a)
{
    int i, k;
    for (i = 0; i < s_nwreck; i++) {
        TYPERUN* r = &s_wrun[i];
        r->u = NULL;
        r->np = tagpu_model_walk(s_wfeat[i]->model, r->node, TAGPU_PBMAXPIECE);
        for (k = 0; k < r->np; k++)
            if (!in_arena(a, r->node[k])) break;
        if (r->np <= 0 || k < r->np) {
            r->np = 0;
            return "a 3D feature's model walk is refused or leaves the arena";
        }
    }
    return NULL;
}

/* the engine's option word as the last packet carried it: bit 2 Shadow, bit 3 TShadow (the
   silhouette), bit 5 Shading (tagpu_packet.h `gfx_opt`). The wiki can be open with no packet; it
   keeps the last one's. */
static unsigned s_gfx;

/* THE SCRIPT'S POSE of a unit, `pose` its posed model (the view's WIKISTAGEPIECEs, `npose` of them
   in the posed order), in the stage's piece order: tagpu_native.c `posed_pose`'s composition --
   P = parent * T(rest offset from the parent + MOVE) * R(TURN) down the tree, the root's
   translation unturned and its rotation folded under the facing -- with the stage's facing
   matrix `rot` about the root's rest point r0 standing for the body turn, so a script that poses
   nothing draws the rest pose. A piece the walk cannot place, or that the posed order does not
   hold, keeps the rest pose turned onto the facing. Each piece's matrix, shade and visibility word
   in posed_pose's words: hidden (flag bit 0 clear) all zeros and 0, visible and cached 3, visible
   dont-cache 1; shaded by bit 2 once any visible piece carries it. */
static void pose_compose(const WIKI3DUNIT* u, const TAGPU_PBGEOM* bg, const WIKISTAGEPIECE* pose,
                         int npose, const float* rot, float* out, unsigned char* shaded,
                         unsigned char* pvis)
{
    static float acc[TAGPU_PBMAXPIECE][12];   /* render thread only */
    static unsigned char done[TAGPU_PBMAXPIECE];
    static const unsigned short rest_turn[3] = { 0, 0, 0 };
    const float* r0 = bg->restOff[0];
    int i, k, pass, left = bg->nparts, any_shade = 0;
    for (i = 0; i < bg->nparts; i++) {
        const int j = u->posed_of[i];
        done[i] = 0;
        if (j >= 0 && j < npose && (pose[j].flags & 1) && (pose[j].flags & 4)) any_shade = 1;
    }
    for (pass = 0; pass < bg->nparts && left > 0; pass++)
        for (i = 0; i < bg->nparts; i++) {
            const short par = bg->parent[i];
            const int j = u->posed_of[i];
            const WIKISTAGEPIECE* p = j >= 0 && j < npose ? &pose[j] : NULL;
            float d[3], loc[12];
            if (done[i] || (par >= 0 && (par >= bg->nparts || !done[par]))) continue;
            for (k = 0; k < 3; k++)
                d[k] = (par >= 0 ? bg->restOff[i][k] - bg->restOff[par][k] : 0.0f) +
                       (p ? (float)p->pos[k] / 65536.0f : 0.0f);
            piece_local(p ? p->turn : rest_turn, d, loc);
            if (par >= 0) m43_mul(acc[par], loc, acc[i]);
            else {
                /* the root: its rest point turned onto the facing about r0, its MOVE unturned
                   (posed_pose's T(off + move) outside the body turn) */
                float face[12], rel[3];
                for (k = 0; k < 3; k++) rel[k] = bg->restOff[i][k] - r0[k];
                for (k = 0; k < 3; k++) {
                    face[k*4+0] = rot[k*4+0];
                    face[k*4+1] = rot[k*4+1];
                    face[k*4+2] = rot[k*4+2];
                    face[k*4+3] = rot[k*4+0]*rel[0] + rot[k*4+1]*rel[1] + rot[k*4+2]*rel[2] + r0[k] + d[k];
                }
                m43_mul(face, loc, acc[i]);
                for (k = 0; k < 3; k++) acc[i][k*4+3] = face[k*4+3];
            }
            done[i] = 1;
            left--;
        }
    for (i = 0; i < bg->nparts; i++) {
        const int j = u->posed_of[i];
        /* a node the posed order does not hold keeps the rest path's visibility too */
        const unsigned rest_fl = u->hides > 0 && (u->hidden[i >> 5] >> (i & 31) & 1u) ? 0u
                               : u->uncaches > 0 && (u->uncached[i >> 5] >> (i & 31) & 1u) ? 5u : 7u;
        const unsigned fl = j >= 0 && j < npose ? pose[j].flags : rest_fl;
        float* o = out + (size_t)i * 12;
        if (!done[i]) {
            float rel[3];
            for (k = 0; k < 3; k++) rel[k] = bg->restOff[i][k] - r0[k];
            for (k = 0; k < 3; k++) {
                acc[i][k*4+0] = rot[k*4+0];
                acc[i][k*4+1] = rot[k*4+1];
                acc[i][k*4+2] = rot[k*4+2];
                acc[i][k*4+3] = rot[k*4+0]*rel[0] + rot[k*4+1]*rel[1] + rot[k*4+2]*rel[2] + r0[k];
            }
        }
        if (fl & 1) memcpy(o, acc[i], 12 * sizeof *o);
        else        memset(o, 0, 12 * sizeof *o);
        shaded[i] = (unsigned char)(any_shade ? (fl & 4) != 0 : 1);
        pvis[i] = (unsigned char)(!(fl & 1) ? 0 : (fl & 2) ? 3 : 1);
    }
}

/* the posed model the view carries for record `m`'s unit, *n its primitives; NULL for a ghost or a
   unit drawn at rest. DATA from the packet: its slot, offset and count are bounded by the view's
   own counts, and the count must be the type's primitive count and its run the walk the posed
   order was matched against (WIKI3DUNIT posed_of). */
static const WIKISTAGEPIECE* pose_of(const WIKISTAGEVIEW* v, const WSCENEMODEL* m, const WIKI3DUNIT* u,
                                     const TYPERUN* r, int* n)
{
    const WIKISTAGEUNIT* vu;
    *n = 0;
    if (!v || m->ghost || m->slot < 0 || m->slot >= v->nunit || m->slot >= WIKISTAGE_UNITS) return NULL;
    vu = &v->unit[m->slot];
    if (!vu->pose_n || vu->pose_n != u->pieces || u->nwalk != r->np || v->npose > WIKISTAGE_POSE ||
        vu->pose_at > v->npose || vu->pose_n > v->npose - vu->pose_at)
        return NULL;
    *n = vu->pose_n;
    return &v->pose[vu->pose_at];
}

/* One record's posed draw: the type's run baked, its script's pose -- or the rest pose -- turned
   onto its facing, and the record's fields as tagpu_native.c hands a unit or a ghost to the posed
   program. 0 when the bake refused it this frame. */
static int unit_one(const WSCENEMODEL* m, const TYPERUN* r, const WIKISTAGEVIEW* v)
{
    static TAGPU_PK_PIECE s_pc[TAGPU_PBMAXPIECE];
    static float s_pose[TAGPU_PBMAXPIECE * 12];
    static unsigned char s_shaded[TAGPU_PBMAXPIECE];
    static unsigned char s_pvis[TAGPU_PBMAXPIECE];
    const WIKI3DUNIT* u = r->u;
    const TAGPU_PBGEOM* bg;
    const TAGPU_PBMAT* bm;
    const WIKISTAGEPIECE* pose;
    TAGPU_PDUNIT q;
    float rot[12];
    int i, k, npose;
    /* a wreck's run has no unit: no script pose, nothing hidden, never lit (its record says so) */
    if ((!u && !m->wreck) || r->np <= 0) return 0;
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
    pose = u ? pose_of(v, m, u, r, &npose) : NULL;
    if (pose) pose_compose(u, bg, pose, npose, rot, s_pose, s_shaded, s_pvis);
    else for (i = 0; i < bg->nparts; i++) {
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
        if (u && u->hides > 0 && (u->hidden[i >> 5] >> (i & 31) & 1u)) {
            memset(o, 0, 12 * sizeof *o);
            s_pvis[i] = 0;
        } else if (u && u->uncaches > 0 && (u->uncached[i >> 5] >> (i & 31) & 1u)) {
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
       one -- under the same option (ghost_one); a wreck on the neutral row, tagpu_native.c's husk */
    q.lit = !m->wreck && (s_gfx & 0x20u) && (m->ghost || !u->mobile);
    tagpu_posedraw_unit(&q);
    return 1;
}

/* THE UNITS' FRAME, every call in tagpu_native.c's order: the atlas's level and recycle before
   the bake latches its generation, the bake's and the posed program's frame, the atlas's
   allowance frame and the shade calibration, then the two windows -- the units', the ghosts'.
   Returns 1 when every record was handed over, 0 when the bake refused one (its texels not yet
   painted): the seam then does not draw this frame's hand-over (tagpu_wikiworld_wants). */
static int units_frame(const TAGPU_FRAME* f, const WIKISTAGEVIEW* v, int nrec, int nunit, int gw, int gh, int ss)
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
        for (i = 0; i < nunit; i++)
            refused += !unit_one(&s_rec[i], s_rec[i].wreck ? &s_wrun[s_rec[i].type] : &s_run[s_rec[i].type], v);
        tagpu_posedraw_end();
    }
    if (nrec > nunit) {
        tagpu_posedraw_begin_ghost(&pv);
        for (i = nunit; i < nrec; i++) refused += !unit_one(&s_rec[i], &s_run[s_rec[i].type], v);
        tagpu_posedraw_end();
    }
    s_frRefused = refused != 0;
    return refused == 0;
}

static void win_add(WINSTAT* w, int image, double ms)
{
    w->frames++;
    w->image += image != 0;
    w->rec += s_frRec;
    w->unitFrames += s_frUnit;
    w->refused += s_frRefused;
    w->prod += s_frProd;
    w->seam += ms;
    if (ms > w->max) w->max = ms;
}

static void work_now(unsigned* k)
{
    k[WORK_GROUND] = s_nGround;
    k[WORK_TYPES] = s_nTypes;
    tagpu_posebake_work(&k[WORK_BAKED], &k[WORK_DROPPED]);
}

/* THE COUNT, tagpu_wiki3d.count: the stage's next N drawn frames counted on their own and said in
   one line, the test harness's measure of the stage in place of the periodic line's 300.

   THE WINDOW OPENS ON THE FIRST DRAWN FRAME AFTER THE ASK THAT IS NOT BUSY: no ground, type record,
   bake or drop since the drawn frame before it, and no record the bake refused. So what the window
   says is the stage at rest, whatever an earlier change (a lever, a shot, an entry) left in flight,
   and a stage that works on every frame never opens one: the ask then goes unanswered, which is the
   failure. Inside the window the work is counted, not filtered: an up stage does none of it.

   The ask is read on the lever's beat, right after the lever, so a count asked once the lever's line
   is out counts frames under that lever. Render thread only, as everything here; the milliseconds
   are said, never the window's condition. */
#define COUNT_ASK  "tagpu_wiki3d.count"
#define COUNT_MAX  3000u
static unsigned s_cntAsk, s_cntN, s_cntFrom;
static int      s_cntState;                     /* 0 none, 1 asked: waiting for a frame at rest, 2 counting */
static unsigned s_cntBase[WORK_N], s_workSeen[WORK_N];

static void count_ask(unsigned frame)
{
    char buf[32];
    unsigned n = 0;
    if (frame != s_leverAt) return;             /* the lever's beat: lever() read it this frame */
    if (tagpu_opt_read(COUNT_ASK, buf, sizeof buf) <= 0) return;
    DeleteFileA(COUNT_ASK);
    if (sscanf(buf, "%u", &n) != 1 || n < 1 || n > COUNT_MAX) {
        tagpu_logf("wiki3d: count: `%s` is not a frame count from 1 to %u - nothing asked", buf, COUNT_MAX);
        return;
    }
    s_cntAsk++;
    s_cntN = n;
    s_cntState = 1;
    tagpu_logf("wiki3d: count %u asked: %u frame(s) of the stage, from the first at rest", s_cntAsk, n);
}

static void count_frame(unsigned frame, int image, double ms, int busy, const unsigned* k)
{
    if (s_cntState == 1 && !busy) {
        s_cntState = 2;
        s_cntFrom = frame;
        memset(&s_cnt, 0, sizeof s_cnt);
        memcpy(s_cntBase, k, sizeof s_cntBase);
    }
    if (s_cntState != 2) return;
    win_add(&s_cnt, image, ms);
    if (s_cnt.frames < s_cntN) return;
    tagpu_logf("wiki3d: count %u: %u of %u frame(s) drew the image, frames %u..%u; the work: %u ground(s), %u type "
               "record set(s), %u bake(s), %u drop(s), %u frame(s) with a record the bake refused; %.1f model record(s) "
               "a frame; render thread: the producer %.3f ms and the passes %.3f ms a frame on average, the passes "
               "at most %.3f ms", s_cntAsk, s_cnt.image, s_cnt.frames, s_cntFrom, frame,
               k[WORK_GROUND] - s_cntBase[WORK_GROUND], k[WORK_TYPES] - s_cntBase[WORK_TYPES],
               k[WORK_BAKED] - s_cntBase[WORK_BAKED], k[WORK_DROPPED] - s_cntBase[WORK_DROPPED], s_cnt.refused,
               s_cnt.unitFrames ? (double)s_cnt.rec / s_cnt.unitFrames : 0.0,
               s_cnt.prod / s_cnt.frames, s_cnt.seam / s_cnt.frames, s_cnt.max);
    s_cntState = 0;
}

void tagpu_wikiworld_frame(const TAGPU_FRAME* f)
{
    const WIKISTAGEVIEW* v;
    const WIKI3D* a;
    const WIKIART* art;
    HUDRECT stage;
    LARGE_INTEGER t0;
    float u;
    int ox, oy, entry, demo, r[4], clip[4], ss;
    uint32_t shot;
    const WIKI3DSTAGE* st = NULL;
    int gw, gh, nrec, nunit, nShadow = 0, nBody = 0;
    const float *shadow = NULL, *body = NULL;

    s_have = s_terrHave = s_featHave = s_tgtHave = s_fogHave = 0;
    s_frRec = s_frUnit = s_frRefused = 0;
    QueryPerformanceCounter(&t0);
    lever(f->frame_counter);
    count_ask(f->frame_counter);
    if (s_leverOff) return;
    if (!tagpu_wikistate_scene(f->frame_counter, &v, &entry, &demo, &shot, &stage, &u, &ox, &oy)) return;
    if (!(a = tagpu_wiki3d_get()) || !(art = tagpu_wikiart_get()) || !art->data) return;
    if (entry < 0 || entry >= art->data->nentry) return;
    st = tagpu_wiki3d_stage(a, &art->data->entry[entry], demo);
    if (v->w <= 0 || v->h <= 0 || v->w > WIKISCENE_MAP || v->h > WIKISCENE_MAP) return;
    if (f->packet) s_gfx = f->packet->gfx_opt;
    /* THE GROUND FALLS BACK WHOLE (the plan's section 3): a stage the loader could not resolve, or
       whose ground is not this view's map's columns or not as deep as its rows, is the whitebox's,
       and the units are drawn over it. A map takes the top v->h rows of its stage's ground. */
    if (!st || !st->ok || st->w != v->w || v->h > st->h || (v->h & 1)) st = NULL;
    /* a device with no image size yet (bring-up, a re-pick) builds nothing and latches nothing:
       the stage is built on the first frame that has one, and again when the map's rows change */
    if (st && (st != s_stage || v->h != s_stageRows) && tagpu_vk_max_image_dim() > 0) {
        const char* why;
        s_nGround++;
        why = ground_build(st, v->h);
        if (!why) why = wrecks_build(a);
        s_stage = st;
        s_stageRows = v->h;
        s_groundOk = why == NULL;
        if (why)
            tagpu_logf("wiki3d: stage %s.%s: the ground falls back to the whitebox - %s", st->entry, st->scene, why);
        else
            tagpu_logf("wiki3d: stage %s.%s: the ground in 3D - %d tiles, %d cells, %d feature quad(s) and %d "
                       "shadow(s), a %dx%d terrain atlas, %d feature frame(s)", st->entry, st->scene, st->ntile,
                       s_scene.ncell, s_scene.nbody, s_scene.nshadow, ATLAS_W, s_tatlasH, s_fatlas.n);
    }
    s_ground = st && st == s_stage && v->h == s_stageRows && s_groundOk;

    /* THE MODEL RECORDS, of the types the arena resolved for this entry */
    if (a != s_tA || entry != s_tEntry || s_leverSerial != s_tLever ||
        s_tNtype != (v->ntype < WIKI_MAX_DEFS ? v->ntype : WIKI_MAX_DEFS) ||
        memcmp(s_tNames, v->type, sizeof s_tNames))
        types_build(a, v, entry, art->data->entry[entry].id);
    nrec = tagpu_wikiscene_models(v, s_types, v->ntype < WIKI_MAX_DEFS ? v->ntype : WIKI_MAX_DEFS,
                                  s_ground ? s_wreck : NULL, s_ground ? s_nwreck : 0, s_rec, &nunit);
    if (!s_ground && !nrec) return;

    /* ONE ss FOR THE FRAME: the target is sized by it and every pass's hand-over carries it */
    ss = tagpu_settings_ss();
    gw = v->w * 16;
    gh = v->h * 16;
    s_wantTerr = s_ground;
    /* a feature the view says was reclaimed is drawn no more (gfx.js drops its anchor); features are
       wanted only while a quad is left, since the pass refuses an empty hand-over and a wanted pass
       that draws nothing drops the whole image */
    if (s_ground && v->ngone && tagpu_wikiscene_dead(&s_scene, v->gone, v->ngone, s_dead)) {
        shadow = live_verts(0, &nShadow);
        body = live_verts(1, &nBody);
    } else if (s_ground) {
        shadow = s_fv[0]; nShadow = s_scene.nshadow * 6;
        body = s_fv[1]; nBody = s_scene.nbody * 6;
    }
    s_wantFeat = s_ground && nShadow + nBody > 0;
    s_wantUnit = nrec > 0;
    s_unitWhole = units_frame(f, v, nrec, nunit, gw, gh, ss) && !s_noUnits;
    s_frRec = (unsigned)nrec;
    s_frUnit = 1;
    if (s_ground) {
        tagpu_wikiscene_fog(v->fog, v->w, v->h, &s_fogNew);
        if (s_fogNew.on) {
            if (!s_fog.on || s_fogNew.w != s_fog.w || s_fogNew.h != s_fog.h ||
                memcmp(s_fogNew.words, s_fog.words,
                       (size_t)s_fogNew.stride * (size_t)s_fogNew.h * sizeof s_fog.words[0])) {
                s_fog = s_fogNew;
                s_fogSerial++;
            }
            s_fogSrc.words = s_fog.words;
            s_fogSrc.w = s_fog.w; s_fogSrc.h = s_fog.h; s_fogSrc.stride = s_fog.stride;
            s_fogSrc.serial = s_fogSerial;
            s_fogHave = tagpu_fog_src_ok(&s_fogSrc);
        }
        tagpu_gaf_atlas_frame(&s_fatlas);
    }

    /* THE TERRAIN, every cell of the map at zoom 1: tagpu_terr.c `terr_publish`'s fields at the
       stage's camera -- the grid is the whole tile map and grid cell (0, 0) is its first, its
       corner the target's, the area the map's, no mirror, no keys, no restore, no relief, no
       shadow map, fog when a cell is fogged (the source, tagpu_wikiworld_fogsrc) */
    if (s_wantTerr && !s_noMap) {
        memset(&s_terr, 0, sizeof s_terr);
        s_terr.cols = s_tmW; s_terr.rows = s_tmH;
        s_terr.areaW = s_tmW; s_terr.areaH = s_tmH;
        s_terr.atlasN = s_stage ? s_stage->ntile : 0;
        s_terr.tileMap = s_tmap; s_terr.mapW = s_tmW; s_terr.mapH = s_tmH;
        s_terr.mapSerial = s_tmSerial;
        s_terr.gw = (float)s_scene.gw; s_terr.gh = (float)s_scene.gh;
        s_terr.zoom = 1.0f;
        s_terr.zoomCx = (float)s_scene.gw * 0.5f; s_terr.zoomCy = (float)s_scene.gh * 0.5f;
        s_terr.depthScale = DEPTH_SCALE;
        s_terr.enc = TERR_ENC;
        s_terr.texelW = 1.0f / (float)ATLAS_W; s_terr.texelH = 1.0f / (float)s_tatlasH;
        s_terr.fog = s_fogHave;
        s_terr.norm = 1.0f;
        s_terr.mapPxW = (float)((v->w / 2) * 32); s_terr.mapPxH = (float)((v->h / 2) * 32);
        s_terr.atlas = s_tatlas; s_terr.atlasW = ATLAS_W; s_terr.atlasH = s_tatlasH;
        s_terr.atlasSerial = s_tatlasSerial;
        s_terr.pal = tagpu_pal_engine(); s_terr.palSerial = tagpu_pal_engine_serial();
        s_terr.vpL = 0; s_terr.vpT = 0; s_terr.vw = s_scene.gw; s_terr.vh = s_scene.gh;
        s_terr.scissorOn = 1;
        s_terr.ss = ss;
        s_terr.frame = f->frame_counter;
        s_terrHave = s_terr.pal != NULL;
    }
    /* THE FEATURES, both buckets as the ground's build left them, under the same fog */
    if (s_wantFeat && !s_noFeat) {
        memset(&s_feat, 0, sizeof s_feat);
        s_feat.shadow = shadow; s_feat.nShadow = nShadow;
        s_feat.body = body; s_feat.nBody = nBody;
        s_feat.gw = (float)s_scene.gw; s_feat.gh = (float)s_scene.gh;
        s_feat.zoom = 1.0f;
        s_feat.zoomCx = (float)s_scene.gw * 0.5f; s_feat.zoomCy = (float)s_scene.gh * 0.5f;
        s_feat.depthScale = DEPTH_SCALE;
        s_feat.fog = s_fogHave;
        s_feat.mapPxW = (float)((v->w / 2) * 32); s_feat.mapPxH = (float)((v->h / 2) * 32);
        s_feat.nudgeW = TAGPU_EDGE_NUDGE_PX;
        tagpu_wikiworld_feat_atlas_hand(&s_feat);
        s_feat.vpL = 0; s_feat.vpT = 0; s_feat.vw = s_scene.gw; s_feat.vh = s_scene.gh;
        s_feat.scissorOn = 1;
        s_feat.ss = ss;
        s_feat.frame = f->frame_counter;
        s_featHave = s_feat.atlas && s_feat.pal;
    }
    /* THE TARGET: the map at the Supersampling setting's ss, composited into the image's rect --
       the fit the stage's ops are built with (tagpu_wikistage_image), on the attachment. A frame
       clips it to its viewport, the rect the stage's world ops clip to: a players frame's map is
       larger than its viewport and stands at the eye; any other frame's lies inside it */
    tagpu_wikistage_image(v, stage, u, r, clip);
    s_tgt.gw = gw; s_tgt.gh = gh;
    s_tgt.ss = ss;
    s_tgt.devres = 0;
    s_tgt.vx = ox + r[0]; s_tgt.vy = oy + r[1];
    s_tgt.vw = r[2] - r[0]; s_tgt.vh = r[3] - r[1];
    s_tgt.clip = v->frame;
    s_tgt.cx = ox + clip[0]; s_tgt.cy = oy + clip[1];
    s_tgt.cw = clip[2] - clip[0]; s_tgt.ch = clip[3] - clip[1];
    s_tgt.frame = f->frame_counter;
    s_tgtHave = s_tgt.vw > 0 && s_tgt.vh > 0;

    if (shot != s_shotWant) {
        s_shotWant = shot;
        snprintf(s_shotWhat, sizeof s_shotWhat, "%s demo %d at tick %d", art->data->entry[entry].id, demo + 1, (int)v->tick);
        s_shotWhat[sizeof s_shotWhat - 1] = 0;
    }
    s_frame = f->frame_counter;
    s_have = s_tgtHave;
    s_frProd = ms_since(t0);
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
    unsigned k[WORK_N];
    int busy;
    if (!s_have || s_frame != frame) return;
    /* a frame is busy when the stage did work since the last drawn frame -- in this frame's producer,
       or in one whose frame was not drawn -- or the bake refused one of its records */
    work_now(k);
    busy = memcmp(k, s_workSeen, sizeof k) != 0 || s_frRefused;
    memcpy(s_workSeen, k, sizeof k);
    win_add(&s_per, image, ms);
    count_frame(frame, image, ms, busy, k);
    if (frame - s_saidAt >= 300) {
        const WINSTAT* w = &s_per;
        tagpu_logf("wiki3d: frame %u: %s; %u of %u frame(s) drew the image; %.1f model record(s) a frame, "
                   "%u frame(s) with one the bake refused; render thread: the producer %.3f ms and the "
                   "passes %.3f ms a frame on average, the passes at most %.3f ms",
                   frame, image ? "the image" : "the whitebox (a pass the image needs did not prepare)",
                   w->image, w->frames, w->unitFrames ? (double)w->rec / w->unitFrames : 0.0, w->refused,
                   w->prod / w->frames, w->seam / w->frames, w->max);
        s_saidAt = frame;
        memset(&s_per, 0, sizeof s_per);
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

/* the stage's fog source this frame (tagpu_wikiworld.h): units take no fog on the stage (the plan's
   section 7), the ground and its features do */
const TAGPU_FOGSRC* tagpu_wikiworld_fogsrc(void)
{
    return (s_have && s_fogHave) ? &s_fogSrc : NULL;
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
