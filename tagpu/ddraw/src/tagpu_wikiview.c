/* tagpu_wikiview.c -- the tech tree's unit view, the render thread's half. Contract:
   tagpu_wikiview.h.

   THE WIKI'S INSTANCES: TAGPU_SCENE_WIKI is defined before the headers (tagpu_wikiworld.c's
   paragraph), so the bake, the atlas and the posed program named here are the wiki's -- the ones
   the stage draws from -- and nothing of the game's world is read or written. */

#define TAGPU_SCENE_WIKI
#include "tagpu_scene.h"

#include <windows.h>
#include <math.h>
#include <string.h>
#include "tagpu.h"
#include "tagpu_packet.h"
#include "tagpu_posebake.h"
#include "tagpu_render3do.h"
#include "tagpu_model3do.h"
#include "tagpu_pal.h"
#include "tagpu_log.h"
#include "tagpu_wikiasset.h"
#include "tagpu_wikiside.h"
#include "tagpu_wikitech.h"
#include "tagpu_wikiworld.h"
#include "tagpu_wikiview.h"

/* the bytes of a Model3DONode the readers take (tagpu_model3do.h: its last field is N_CHILD) */
#define NODE_BYTES (N_CHILD + 4)

_Static_assert(TAGPU_WV_MAXVERT >= 49152, "the stream holds every body range the bake allows (PB_MAXVERT)");

/* THE VIEW'S SHADERS, a build input like tagpu_fps.c's: tools/spirv-gen.py reads them out of this
   translation unit and tagpu_vk_wikiview.c draws with the SPIR-V. unitview.js's program, in the
   house's conventions: the atlas is the expanded RGBA one the unit pass samples (a key texel's
   alpha 0, so the key's discard is the alpha's), a flat face's colour rides in the vertex, and
   the pass flips Y with a negative-height viewport -- so a derivative along Y runs the other way
   from WebGL's, and the face's normal, toward the eye as the mockup's is, is cross(dFdy, dFdx).
   The light is the mockup's, in the held axes the vertices are in. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec3 aCol;\n"
    "layout(location=3) in vec2 aQH;\n"
    "layout(location=4) in vec2 aQAB;\n"
    "layout(location=5) in vec4 aQR;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV; flat out vec3 vCol; out vec3 vP; out vec2 vQH; flat out vec2 vQAB; flat out vec4 vQR;\n"
    "void main(){\n"
    "  vUV = aUV; vCol = aCol; vP = aPos; vQH = aQH; vQAB = aQAB; vQR = aQR;\n"
    "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "}\n";

/* taQuadST is tagpu_native.c's, letter for letter: a textured quad mapped per pixel through the
   inverse bilinear map, as the unit pass maps it */
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec3 vCol; in vec3 vP; in vec2 vQH; flat in vec2 vQAB; flat in vec4 vQR;\n"
    "uniform sampler2D uAtlas;\n"
    "out vec4 frag;\n"
    "vec2 taQuadST(vec2 h, vec2 ab){\n"
    "  vec2 g = ab - 1.0;\n"
    "  float k1 = 1.0 + h.x * g.y - h.y * g.x;\n"
    "  float D = max(k1 * k1 + 4.0 * g.x * h.y, 0.0);\n"
    "  float qt = 2.0 * h.y / max(k1 + sqrt(D), 1e-6);\n"
    "  return vec2(h.x / max(1.0 + g.x * qt, 1e-6), qt);\n"
    "}\n"
    "void main(){\n"
    "  vec3 base = vCol;\n"
    "  if (vUV.x >= 0.0) {\n"
    "    vec2 uv = vQR.x >= 0.0 ? vQR.xy + (vQR.zw - vQR.xy) * clamp(taQuadST(vQH, vQAB), 0.0, 1.0) : vUV;\n"
    "    vec4 b = texture(uAtlas, uv);\n"
    "    if (b.a < 0.5) discard;\n"
    "    base = b.rgb;\n"
    "  }\n"
    "  vec3 n = normalize(cross(dFdy(vP), dFdx(vP)));\n"
    "  float l = 0.45 + 0.65 * max(dot(n, vec3(-0.48, 0.8, 0.36)), 0.0);\n"
    "  frag = vec4(base * l, 1.0);\n"
    "}\n";
#pragma GCC diagnostic pop

static float         s_vert[TAGPU_WV_MAXVERT * TAGPU_WV_STRIDE];   /* render thread only */
static int           s_nvert;
static int           s_have, s_note = TAGPU_TV_NOTE_LOADING;
static unsigned      s_frame;
static float         s_c[3], s_dist, s_yaw, s_pitch, s_zoom;
static const unsigned char* s_pal;
static unsigned      s_palSerial;
/* the fit, kept for the body and the bake it was taken from */
static int           s_fitBody = -1;
static unsigned      s_fitSerial;
static float         s_fitC[3], s_fitDist;
static int           s_saidArena = -1;  /* the body whose walk left the arena, said once */

static float         s_pose[TAGPU_PBMAXPIECE * 12];
static unsigned char s_shaded[TAGPU_PBMAXPIECE], s_pvis[TAGPU_PBMAXPIECE];

/* node `p` lies inside the arena's used extent: tagpu_wikiworld.c `in_arena`'s test, on the walk's
   values, before the bake is handed them */
static int in_arena(const WIKI3D* a, const char* p)
{
    const uint32_t v = (uint32_t)(size_t)p;
    return v >= a->base && a->bytes >= NODE_BYTES && v - a->base <= a->bytes - NODE_BYTES;
}

static void apply(const float* m, const float* p, float* o)
{
    int k;
    for (k = 0; k < 3; k++) o[k] = m[k*4+0]*p[0] + m[k*4+1]*p[1] + m[k*4+2]*p[2] + m[k*4+3];
}

/* THE FIT, unitview.js's: the centre of the rest pose's bounds and its largest extent * 1.9 + 8,
   over the vertices the rest pose draws -- the body's, of the pieces it shows, less the faces the
   bake collapses, which the mockup's pack does not hold either (a collapsed face can lie far from
   the model: ARMSOLAR's). 0 when it draws none. */
static int fit(const WIKI3DUNIT* u, const TAGPU_PBGEOM* bg, const float* g, const float* m, float* c, float* dist)
{
    static const float I[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, q[3], ext = 0;
    const int v0 = bg->first[TAGPU_PB_BODY], n = bg->count[TAGPU_PB_BODY];
    int v, k, any = 0;
    tagpu_wikiworld_pose_compose(u, bg, NULL, 0, I, s_pose, s_shaded, s_pvis);
    for (v = v0; v < v0 + n; v++) {
        const float* gv = g + (size_t)v * TAGPU_PB_GEOMST;
        const int p = (int)(gv[6] + 0.5f);
        if (p < 0 || p >= bg->nparts || !s_pvis[p] || m[(size_t)v * TAGPU_PB_MATST + 4] > 0.5f) continue;
        apply(s_pose + (size_t)p * 12, gv, q);
        for (k = 0; k < 3; k++) { if (q[k] < lo[k]) lo[k] = q[k]; if (q[k] > hi[k]) hi[k] = q[k]; }
        any = 1;
    }
    if (!any) return 0;
    for (k = 0; k < 3; k++) {
        c[k] = (lo[k] + hi[k]) * 0.5f;
        if (hi[k] - lo[k] > ext) ext = hi[k] - lo[k];
    }
    *dist = ext * 1.9f + 8.0f;
    return 1;
}

/* the record's camera, DATA: a value that is not finite reads as the view's default, the pitch is
   held to the drag's limit and the zoom to what a wheel can reach in reason */
static float finite_or(float v, float d) { return isfinite(v) ? v : d; }

void tagpu_wikiview_frame(const TAGPU_FRAME* f)
{
    static const char* node[TAGPU_PBMAXPIECE];
    static TAGPU_PK_PIECE pc[TAGPU_PBMAXPIECE];
    static const float I[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
    const TAGPU_PK_WIKI* w;
    const WIKI3D* a = NULL;
    const WIKI3DUNIT* u;
    const TAGPU_PBGEOM* bg;
    const TAGPU_PBMAT* bm;
    const float *g, *m;
    int ng = 0, nm = 0, np, i, v, k, body, npose;

    s_have = 0;
    s_nvert = 0;
    s_frame = f->frame_counter;
    s_note = TAGPU_TV_NOTE_LOADING;
    if (!f->packet || !f->packet->in_game || !(w = tagpu_pk_wiki(f->packet)) || !w->open || w->tv_body < 0)
        return;
    s_note = w->tv_note < TAGPU_TV_NOTES ? w->tv_note : TAGPU_TV_NOTE_LOADING;
    if (s_note != TAGPU_TV_NOTE_HINT && s_note != TAGPU_TV_NOTE_REST) return;
    s_note = TAGPU_TV_NOTE_LOADING;
    if (tagpu_wikitech3d_get(&a) != WIKISIDE_READY || !a) return;
    body = w->tv_body;
    if (body >= a->nunit) { s_note = TAGPU_TV_NOTE_NOMODEL; return; }
    u = &a->unit[body];
    if (!u->ok || !u->model) { s_note = TAGPU_TV_NOTE_NOMODEL; return; }
    if (!(s_pal = tagpu_pal_engine())) return;
    s_palSerial = tagpu_pal_engine_serial();
    if (!tagpu_wikiworld_units_begin(f)) return;

    /* THE RUN: the readers' walk of the model, every node inside the arena (the stage's rule) */
    np = tagpu_model_walk(u->model, node, TAGPU_PBMAXPIECE);
    for (i = 0; i < np; i++)
        if (!in_arena(a, node[i])) break;
    if (np <= 0 || i < np) {
        if (s_saidArena != body) {
            s_saidArena = body;
            tagpu_logf("wiki: the tech tree's view of %s: the model's walk %s - nothing drawn", u->name,
                       np <= 0 ? "is refused" : "leaves the arena");
        }
        s_note = TAGPU_TV_NOTE_NOMODEL;
        return;
    }
    for (i = 0; i < np; i++) {
        memset(&pc[i], 0, sizeof pc[i]);
        pc[i].flags = 1;
        pc[i].node = (uint32_t)(size_t)node[i];
    }
    /* the template walk's key (`ghost` 1), as the stage bakes a type; owner 0's team faces */
    if (!tagpu_posebake_unit(pc, np, 0, 1, &bg, &bm) || bg->nparts != np || bg->count[TAGPU_PB_BODY] <= 0)
        return;
    g = tagpu_posebake_geom_mirror(bg, bg->serial, &ng);
    m = tagpu_posebake_mat_mirror(bm, bm->serial, &nm);
    if (!g || !m || ng != bg->nvert || nm != bg->nvert || bg->first[TAGPU_PB_BODY] < 0 ||
        bg->count[TAGPU_PB_BODY] > ng - bg->first[TAGPU_PB_BODY] || bg->count[TAGPU_PB_BODY] > TAGPU_WV_MAXVERT)
        return;

    if (body != s_fitBody || bg->serial != s_fitSerial) {
        s_fitBody = body;
        s_fitSerial = bg->serial;
        if (!fit(u, bg, g, m, s_fitC, &s_fitDist)) { s_fitC[0] = s_fitC[1] = s_fitC[2] = 0; s_fitDist = 60; }
    }

    /* THE POSE: the record's, when it is this model's posed order (the stage's test, pose_of), else
       the rest pose */
    npose = w->tv_npose;
    if (!npose || npose != u->pieces || u->nwalk != np || npose > WIKISTAGE_PIECES) npose = 0;
    tagpu_wikiworld_pose_compose(u, bg, npose ? w->tv_pose : NULL, npose, I, s_pose, s_shaded, s_pvis);

    /* THE STREAM: the body's triangles of the pieces the pose shows, less what the bake collapses */
    {
        const int v0 = bg->first[TAGPU_PB_BODY], v1 = v0 + bg->count[TAGPU_PB_BODY] / 3 * 3;
        float* o = s_vert;
        for (v = v0; v < v1; v += 3) {
            const float* gv = g + (size_t)v * TAGPU_PB_GEOMST;
            const float* mv = m + (size_t)v * TAGPU_PB_MATST;
            const int p = (int)(gv[6] + 0.5f);
            if (p < 0 || p >= bg->nparts || !s_pvis[p] || mv[4] > 0.5f) continue;
            for (k = 0; k < 3; k++) {
                const float* gk = gv + (size_t)k * TAGPU_PB_GEOMST;
                const float* mk = mv + (size_t)k * TAGPU_PB_MATST;
                apply(s_pose + (size_t)p * 12, gk, o);
                o[3] = mk[0]; o[4] = mk[1];
                if (mk[0] < 0.0f) {
                    int idx = (int)(mk[2] * 255.0f + 0.5f);
                    if (idx < 0) idx = 0;
                    if (idx > 255) idx = 255;
                    o[5] = s_pal[idx * 4 + 0] / 255.0f;
                    o[6] = s_pal[idx * 4 + 1] / 255.0f;
                    o[7] = s_pal[idx * 4 + 2] / 255.0f;
                } else
                    o[5] = o[6] = o[7] = 0.0f;
                memcpy(o + 8, mk + 5, 8 * sizeof *o);
                o += TAGPU_WV_STRIDE;
            }
        }
        s_nvert = (int)((o - s_vert) / TAGPU_WV_STRIDE);
    }
    s_note = s_nvert ? w->tv_note : TAGPU_TV_NOTE_EMPTY;
    memcpy(s_c, s_fitC, sizeof s_c);
    s_dist = s_fitDist;
    s_yaw = finite_or(w->tv_cam[0], 0.7f);
    s_yaw = fmodf(s_yaw, 6.2831853f);
    s_pitch = finite_or(w->tv_cam[1], 0.45f);
    if (s_pitch < -1.4f) s_pitch = -1.4f;
    if (s_pitch > 1.4f) s_pitch = 1.4f;
    s_zoom = finite_or(w->tv_cam[2], 1.0f);
    if (s_zoom < 1.0f / 64) s_zoom = 1.0f / 64;
    if (s_zoom > 64.0f) s_zoom = 64.0f;
    s_have = s_nvert > 0;
}

int tagpu_wikiview_note(unsigned frame)
{
    return frame == s_frame ? s_note : TAGPU_TV_NOTE_LOADING;
}

int tagpu_wikiview_hand(unsigned frame, TAGPU_WIKIVIEWHAND* h)
{
    memset(h, 0, sizeof *h);
    if (!s_have || frame != s_frame) return 0;
    if (!tagpu_r3d_atlas_view(&h->atlas) || !h->atlas.idx) return 0;
    h->vert = s_vert;
    h->nvert = s_nvert;
    memcpy(h->c, s_c, sizeof h->c);
    h->dist = s_dist;
    h->yaw = s_yaw;
    h->pitch = s_pitch;
    h->zoom = s_zoom;
    h->pal = s_pal;
    h->palSerial = s_palSerial;
    return 1;
}

void tagpu_wikiview_atlas_ack(unsigned serial, int keep)
{
    tagpu_r3d_atlas_ack(serial, keep);
}
