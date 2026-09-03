/* tagpu_hires.c — the REPLACEMENT-MESH SLOT (G12d, feeding G11): the glTF 2.0
   loader. `tagpu_hires_draw.c` is the renderer that draws what this produces.

   If gamedir/hires/<defname>.glb (or .gltf) exists — lowercase unit def name,
   e.g. armpw.glb — the native pass renders THAT model for the unit type
   instead of the engine's 3DO: engine anchor + body yaw, the frame's depth,
   fog and water rules, hot reload on mtime (~2 s iteration loop).

   WHY glTF, AND NOT THE OBJ SUBSET IT REPLACED. glTF is what `tools/ta3do`
   already writes when it exports a stock 3DO, so 3DO -> GLB -> edit -> back
   into the game is a ROUND TRIP rather than a conversion; it carries the piece
   TREE by name (what G11 needs to drive rigid pieces from live COB state); and
   it carries real materials — base colour and normal maps, metallic/roughness —
   which is why replacement meshes get their own render path instead of being
   squeezed through the engine's palette-index one.

   WHAT THIS PRODUCES, and it is shaped for the GPU, not for the file:
     - ONE static interleaved vertex buffer per model (position, normal, uv),
       triangles SORTED BY MATERIAL so each material is one contiguous draw.
       The buffer is uploaded once at load; a unit costs a handful of uniforms
       and one draw per material per frame, never a per-vertex CPU transform;
     - one HGroup per glTF material, carrying its two textures (base colour,
       normal), its factors and its doubleSided flag;
     - textures uploaded once, mipmapped, with the glTF sampler's own filters.
       PNG only — lodepng is already in the DLL and nothing else is.

   SUPPORTED: .glb and .gltf, buffers and images from the GLB BIN chunk, a
   `data:...;base64,` URI or a file beside the .gltf; the whole scene graph
   (node TRS or `matrix`, composed down the tree and BAKED — per-piece COB pose
   is G11's half, not this); primitive mode 4, indexed or not; POSITION,
   NORMAL, TEXCOORD_0.

   AXES. glTF is right-handed Y-up; the 3DO model space the frame is built in
   is the left-handed one, so Z is negated on positions AND normals, and
   triangle order reversed to match — the exact inverse of the conversion in
   `tools/ta3do`, which is what makes the round trip land back where it
   started. Both sides are in TA model units (game px): ta3do writes the 16.16
   fixed-point verts divided by 65536.

   NOT SUPPORTED, deliberately: per-piece COB pose (G11), skins, morph targets,
   animation, sparse accessors, texture wrap modes (UVs are clamped), KHR
   extensions, and every material channel past base colour + normal +
   metallic/roughness. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "lodepng.h"
#include "tagpu_hires.h"

#define MAXMESH   8        /* replacement types held at once                 */
#define MAXTRI    65536    /* triangles in one replacement model             */
#define MAXGRP    32       /* materials per model                            */
#define MAXIMG    32       /* distinct images per model                      */
#define MAXBUF    8        /* glTF buffers per model                         */
#define MAXTEX    2048     /* an image may not exceed this on a side         */
#define MAXFILE   (64u << 20)
#define HVSTRIDE  8        /* floats per vertex: px,py,pz, nx,ny,nz, u,v     */

typedef struct { int idx; unsigned char* px; int w, h; GLuint tex; } HImg;

typedef struct {
    int   albedo, normal;      /* index into HMesh.img, or -1                */
    float base[4];             /* baseColorFactor, LINEAR as glTF stores it  */
    float metal, rough;
    float cutoff;              /* < 0 = OPAQUE; MASK and BLEND both cut out   */
    int   doubleSided;
    int   magf, minf;          /* glTF sampler filters (0 = our defaults)    */
    int   first, count;        /* triangles, contiguous in the vertex buffer */
    int   mat;                 /* glTF material index, -1 = the default one  */
} HGroup;

typedef struct {
    char     name[32];
    char     path[128];
    FILETIME mtime;
    int      valid;
    int      announced;        /* the one "looked here, found this" log line  */
    int      ntri;
    float*   v;                /* 3 * ntri verts, HVSTRIDE floats each       */
    HImg     img[MAXIMG];
    int      nimg;
    HGroup   grp[MAXGRP];
    int      ngrp;
    GLuint   vao, vbo;
    int      uploaded;
} HMesh;

static HMesh s_mesh[MAXMESH];
static int   s_nmesh = 0;
static GLuint s_white = 0;     /* 1x1 opaque white, for untextured materials */

static void hlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}
/* ------------------------------------------------------------------ JSON --
   A jsmn-shaped tokeniser: one pass to count, one to fill, then navigation by
   PARENT LINKS (an object's direct children alternate key, value, key, ...).
   Numbers are parsed by hand rather than by strtod: the host process's
   LC_NUMERIC is not ours to assume, and a comma decimal point would turn every
   vertex into garbage silently. */

enum { JS_OBJ = 1, JS_ARR, JS_STR, JS_PRIM };

typedef struct { unsigned char type; int start, end, parent; } JTok;
typedef struct { const char* js; size_t len, pos; JTok* toks; int ntok, next, super; } JP;

static JTok* jalloc(JP* p)
{
    if (!p->toks) { p->next++; return NULL; }
    if (p->next >= p->ntok) return NULL;
    JTok* t = &p->toks[p->next++];
    t->start = t->end = -1; t->parent = -1;
    return t;
}

static int jparse(JP* p)
{
    for (p->pos = 0; p->pos < p->len; p->pos++) {
        char c = p->js[p->pos];
        if (c == '{' || c == '[') {
            JTok* t = jalloc(p);
            if (p->toks && !t) return -1;
            if (t) {
                t->type = (unsigned char)((c == '{') ? JS_OBJ : JS_ARR);
                t->start = (int)p->pos;
                t->parent = p->super;
            }
            p->super = p->next - 1;
        } else if (c == '}' || c == ']') {
            if (!p->toks) continue;
            int ty = (c == '}') ? JS_OBJ : JS_ARR, k;
            for (k = p->next - 1; k >= 0; k--) {
                if (p->toks[k].start != -1 && p->toks[k].end == -1) {
                    if (p->toks[k].type != ty) return -1;
                    p->toks[k].end = (int)p->pos + 1;
                    p->super = p->toks[k].parent;
                    break;
                }
            }
            if (k < 0) return -1;
        } else if (c == '"') {
            size_t start = ++p->pos;
            for (; p->pos < p->len; p->pos++) {
                char ch = p->js[p->pos];
                if (ch == '"') break;
                if (ch == '\\' && p->pos + 1 < p->len) p->pos++;
            }
            if (p->pos >= p->len) return -1;
            JTok* t = jalloc(p);
            if (p->toks && !t) return -1;
            if (t) { t->type = JS_STR; t->start = (int)start;
                     t->end = (int)p->pos; t->parent = p->super; }
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                   c == ':' || c == ',') {
            continue;
        } else {
            size_t start = p->pos;
            for (; p->pos < p->len; p->pos++) {
                char ch = p->js[p->pos];
                if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' ||
                    ch == '\t' || ch == '\r' || ch == '\n' || ch == ':') break;
            }
            JTok* t = jalloc(p);
            if (p->toks && !t) return -1;
            if (t) { t->type = JS_PRIM; t->start = (int)start;
                     t->end = (int)p->pos; t->parent = p->super; }
            p->pos--;
        }
    }
    if (p->toks) {
        int k;
        for (k = 0; k < p->next; k++)
            if (p->toks[k].end == -1) return -1;      /* unterminated container */
    }
    return p->next;
}

static double jnum(const char* s, const char* e)
{
    double sign = 1.0, v = 0.0;
    if (s < e && (*s == '-' || *s == '+')) { if (*s == '-') sign = -1.0; s++; }
    while (s < e && *s >= '0' && *s <= '9') v = v * 10.0 + (*s++ - '0');
    if (s < e && *s == '.') {
        double f = 0.1; s++;
        while (s < e && *s >= '0' && *s <= '9') { v += (*s++ - '0') * f; f *= 0.1; }
    }
    if (s < e && (*s == 'e' || *s == 'E')) {
        int es = 1, ev = 0; s++;
        if (s < e && (*s == '-' || *s == '+')) { if (*s == '-') es = -1; s++; }
        while (s < e && *s >= '0' && *s <= '9') ev = ev * 10 + (*s++ - '0');
        if (ev > 300) ev = 300;
        v *= pow(10.0, (double)(es * ev));
    }
    return sign * v;
}

/* ------------------------------------------------------------------ glTF -- */

typedef struct {
    const char* js;
    const JTok* t;
    int    n, root;
    const unsigned char* bin; size_t binlen;   /* the GLB BIN chunk, if any   */
    char   dir[160];                           /* "hires\", for sidecar files */
    unsigned char* own[MAXBUF];                /* buffers we allocated        */
    const unsigned char* buf[MAXBUF];
    size_t buflen[MAXBUF];
    int    bufdone[MAXBUF];
} GCtx;

static int jfield(const GCtx* g, int obj, const char* key)
{
    if (obj < 0 || g->t[obj].type != JS_OBJ) return -1;
    int k, c = 0, kl = (int)strlen(key), last = -1, end = g->t[obj].end;
    for (k = obj + 1; k < g->n && g->t[k].start < end; k++) {
        if (g->t[k].parent != obj) continue;
        if ((c & 1) == 0) last = k;
        else if (last >= 0 && g->t[last].end - g->t[last].start == kl &&
                 !memcmp(g->js + g->t[last].start, key, (size_t)kl))
            return k;
        c++;
    }
    return -1;
}

static int jelem(const GCtx* g, int arr, int i)
{
    if (arr < 0 || g->t[arr].type != JS_ARR || i < 0) return -1;
    int k, c = 0, end = g->t[arr].end;
    for (k = arr + 1; k < g->n && g->t[k].start < end; k++) {
        if (g->t[k].parent != arr) continue;
        if (c == i) return k;
        c++;
    }
    return -1;
}

static int jlen(const GCtx* g, int arr)
{
    if (arr < 0 || g->t[arr].type != JS_ARR) return 0;
    int k, c = 0, end = g->t[arr].end;
    for (k = arr + 1; k < g->n && g->t[k].start < end; k++)
        if (g->t[k].parent == arr) c++;
    return c;
}

static double jval(const GCtx* g, int tok, double def)
{
    if (tok < 0 || g->t[tok].type != JS_PRIM) return def;
    const char* s = g->js + g->t[tok].start;
    if (*s == 't') return 1.0;
    if (*s == 'f' || *s == 'n') return 0.0;
    return jnum(s, g->js + g->t[tok].end);
}

static int    gi(const GCtx* g, int obj, const char* k, int def)
{ int f = jfield(g, obj, k); return f < 0 ? def : (int)jval(g, f, (double)def); }
static double gf(const GCtx* g, int obj, const char* k, double def)
{ int f = jfield(g, obj, k); return f < 0 ? def : jval(g, f, def); }

/* n floats out of an array field; returns 1 if the field was there and long
   enough, leaving `out` untouched otherwise */
static int gvec(const GCtx* g, int obj, const char* k, float* out, int n)
{
    int a = jfield(g, obj, k), i;
    if (a < 0 || jlen(g, a) < n) return 0;
    for (i = 0; i < n; i++) out[i] = (float)jval(g, jelem(g, a, i), 0.0);
    return 1;
}

static int gstreq(const GCtx* g, int tok, const char* s)
{
    int l = (int)strlen(s);
    return tok >= 0 && g->t[tok].type == JS_STR &&
           g->t[tok].end - g->t[tok].start == l &&
           !memcmp(g->js + g->t[tok].start, s, (size_t)l);
}

static int groot(const GCtx* g, const char* name) { return jfield(g, g->root, name); }

/* ------------------------------------------------------------ file + b64 -- */

static unsigned char* slurp(const char* path, size_t* out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    long n;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        (unsigned long)n > MAXFILE) { fclose(f); return NULL; }
    rewind(f);
    unsigned char* b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    *out = got;
    return b;
}

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static unsigned char* b64dec(const char* s, size_t len, size_t* out)
{
    unsigned char* o = malloc(len / 4 * 3 + 4);
    if (!o) return NULL;
    size_t n = 0;
    int acc = 0, bits = 0, i;
    for (i = 0; (size_t)i < len; i++) {
        int v = b64v(s[i]);
        if (v < 0) continue;                     /* '=' and whitespace */
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; o[n++] = (unsigned char)((acc >> bits) & 0xFF); }
    }
    *out = n;
    return o;
}

/* A glTF uri, percent-decoded, joined to the document's directory. Refused if
   it tries to leave that directory or name a drive: these files are dropped in
   by hand, and a slot that reads arbitrary paths is a slot that surprises. */
static int uri_path(const GCtx* g, const char* uri, size_t len, char* out, size_t cap)
{
    char rel[128];
    size_t n = 0, i;
    for (i = 0; i < len && n + 1 < sizeof rel; i++) {
        char c = uri[i];
        if (c == '%' && i + 2 < len) {
            char h[3]; h[0] = uri[i+1]; h[1] = uri[i+2]; h[2] = 0;
            c = (char)strtol(h, NULL, 16); i += 2;
        }
        if (c == '/') c = '\\';
        rel[n++] = c;
    }
    rel[n] = 0;
    if (!n || rel[0] == '\\' || strstr(rel, "..") || strchr(rel, ':')) return 0;
    if (strlen(g->dir) + n + 1 > cap) return 0;
    _snprintf(out, cap, "%s%s", g->dir, rel);
    out[cap - 1] = 0;
    return 1;
}

/* ------------------------------------------------------- buffers/accessors */

static int gbuf(GCtx* g, int idx, const unsigned char** p, size_t* len)
{
    if (idx < 0 || idx >= MAXBUF) return 0;
    if (!g->bufdone[idx]) {
        g->bufdone[idx] = 1;
        int b = jelem(g, groot(g, "buffers"), idx);
        int u = jfield(g, b, "uri");
        if (u < 0) {                                   /* the GLB BIN chunk */
            g->buf[idx] = g->bin; g->buflen[idx] = g->binlen;
        } else {
            const char* s = g->js + g->t[u].start;
            size_t l = (size_t)(g->t[u].end - g->t[u].start);
            const char* c = NULL;
            if (l > 5 && !memcmp(s, "data:", 5)) {
                size_t i;
                for (i = 0; i + 7 < l; i++)
                    if (!memcmp(s + i, ";base64,", 8)) { c = s + i + 8; break; }
                if (c) g->own[idx] = b64dec(c, l - (size_t)(c - s), &g->buflen[idx]);
            } else {
                char path[192];
                if (uri_path(g, s, l, path, sizeof path))
                    g->own[idx] = slurp(path, &g->buflen[idx]);
            }
            g->buf[idx] = g->own[idx];
            if (!g->buf[idx]) g->buflen[idx] = 0;
        }
    }
    *p = g->buf[idx]; *len = g->buflen[idx];
    return *p != NULL;
}

typedef struct { const unsigned char* p; size_t stride; int count, comp, ncomp, norm; } GAcc;

static int compsz(int c)
{
    switch (c) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

static int gacc(GCtx* g, int idx, GAcc* a)
{
    int ac = jelem(g, groot(g, "accessors"), idx);
    if (ac < 0) return 0;
    int ty = jfield(g, ac, "type");
    a->ncomp = gstreq(g, ty, "SCALAR") ? 1 : gstreq(g, ty, "VEC2") ? 2
             : gstreq(g, ty, "VEC3")   ? 3 : gstreq(g, ty, "VEC4") ? 4 : 0;
    a->comp  = gi(g, ac, "componentType", 0);
    a->count = gi(g, ac, "count", 0);
    a->norm  = gi(g, ac, "normalized", 0);
    int esz  = compsz(a->comp) * a->ncomp;
    if (!a->ncomp || !esz || a->count <= 0 || a->count > (1 << 22)) return 0;

    int bvi = gi(g, ac, "bufferView", -1);
    if (bvi < 0) return 0;                    /* sparse-only: unsupported */
    int bv = jelem(g, groot(g, "bufferViews"), bvi);
    if (bv < 0) return 0;
    const unsigned char* base; size_t blen;
    if (!gbuf(g, gi(g, bv, "buffer", 0), &base, &blen)) return 0;
    size_t bo = (size_t)gi(g, bv, "byteOffset", 0);
    size_t bl = (size_t)gi(g, bv, "byteLength", 0);
    size_t bs = (size_t)gi(g, bv, "byteStride", 0);
    size_t ao = (size_t)gi(g, ac, "byteOffset", 0);
    if (!bl || bo > blen || bl > blen - bo) return 0;
    a->stride = bs ? bs : (size_t)esz;
    if (a->stride < (size_t)esz) return 0;
    if (ao > bl || (size_t)(a->count - 1) * a->stride + (size_t)esz > bl - ao) return 0;
    a->p = base + bo + ao;
    return 1;
}

static void gread(const GAcc* a, int i, float* out, int n)
{
    const unsigned char* p = a->p + (size_t)i * a->stride;
    int k;
    for (k = 0; k < n; k++) {
        float v = 0.0f;
        if (k < a->ncomp) {
            switch (a->comp) {
            case 5126: memcpy(&v, p + k * 4, 4); break;
            case 5121: v = (float)p[k];             if (a->norm) v /= 255.0f;   break;
            case 5123: { unsigned short s; memcpy(&s, p + k * 2, 2);
                         v = (float)s;              if (a->norm) v /= 65535.0f; } break;
            case 5120: { signed char s = (signed char)p[k];
                         v = (float)s;              if (a->norm) v = v / 127.0f; } break;
            case 5122: { short s; memcpy(&s, p + k * 2, 2);
                         v = (float)s;              if (a->norm) v = v / 32767.0f; } break;
            case 5125: { unsigned int s; memcpy(&s, p + k * 4, 4); v = (float)s; } break;
            default: break;
            }
        }
        out[k] = v;
    }
}

static unsigned gidx(const GAcc* a, int i)
{
    const unsigned char* p = a->p + (size_t)i * a->stride;
    unsigned short s16; unsigned int s32;
    switch (a->comp) {
    case 5121: return p[0];
    case 5123: memcpy(&s16, p, 2); return s16;
    case 5125: memcpy(&s32, p, 4); return s32;
    default: return 0xFFFFFFFFu;
    }
}

/* ------------------------------------------------------------- matrices --
   glTF matrices are column-major: m[col*4 + row]. */

static void mat_id(float* m)
{ memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1.0f; }

static void mat_mul(const float* a, const float* b, float* o)
{
    int c, r;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] +
                       a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
}

static void mat_pt(const float* m, const float* v, float* o)
{
    int r;
    for (r = 0; r < 3; r++)
        o[r] = m[0*4+r]*v[0] + m[1*4+r]*v[1] + m[2*4+r]*v[2] + m[3*4+r];
}

static void node_local(const GCtx* g, int node, float* m)
{
    float raw[16];
    if (gvec(g, node, "matrix", raw, 16)) { memcpy(m, raw, sizeof raw); return; }
    float t[3] = {0,0,0}, r[4] = {0,0,0,1}, s[3] = {1,1,1};
    gvec(g, node, "translation", t, 3);
    gvec(g, node, "rotation", r, 4);
    gvec(g, node, "scale", s, 3);
    float x = r[0], y = r[1], z = r[2], w = r[3];
    float xx = x*x, yy = y*y, zz = z*z, xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;
    m[0]  = (1-2*(yy+zz))*s[0]; m[1]  = (2*(xy+wz))*s[0];   m[2]  = (2*(xz-wy))*s[0];   m[3]  = 0;
    m[4]  = (2*(xy-wz))*s[1];   m[5]  = (1-2*(xx+zz))*s[1]; m[6]  = (2*(yz+wx))*s[1];   m[7]  = 0;
    m[8]  = (2*(xz+wy))*s[2];   m[9]  = (2*(yz-wx))*s[2];   m[10] = (1-2*(xx+yy))*s[2]; m[11] = 0;
    m[12] = t[0];               m[13] = t[1];               m[14] = t[2];               m[15] = 1;
}

/* ------------------------------------------------------------ materials -- */

typedef struct {
    GCtx*  g;
    HMesh* m;
    float  (*p)[9];            /* per tri: 3 verts x xyz, TA model space     */
    float  (*n)[9];            /* per tri: 3 verts x normal, TA model space  */
    float  (*t)[6];            /* per tri: 3 verts x uv                      */
    unsigned char* gid;        /* per tri: which HGroup it belongs to        */
    int    ntri, trunc;
} Build;

/* decode images[idx] once per model; returns its HMesh.img slot, or -1 */
static int img_slot(Build* b, int idx)
{
    GCtx* g = b->g;
    HMesh* m = b->m;
    int i;
    if (idx < 0) return -1;
    for (i = 0; i < m->nimg; i++) if (m->img[i].idx == idx) return i;
    if (m->nimg >= MAXIMG) return -1;

    int im = jelem(g, groot(g, "images"), idx);
    if (im < 0) return -1;
    const unsigned char* data = NULL;
    size_t len = 0;
    unsigned char* tmp = NULL;
    int bvi = gi(g, im, "bufferView", -1);
    if (bvi >= 0) {
        int bv = jelem(g, groot(g, "bufferViews"), bvi);
        const unsigned char* base; size_t blen;
        if (bv < 0 || !gbuf(g, gi(g, bv, "buffer", 0), &base, &blen)) return -1;
        size_t bo = (size_t)gi(g, bv, "byteOffset", 0);
        size_t bl = (size_t)gi(g, bv, "byteLength", 0);
        if (!bl || bo > blen || bl > blen - bo) return -1;
        data = base + bo; len = bl;
    } else {
        int u = jfield(g, im, "uri");
        if (u < 0) return -1;
        const char* s = g->js + g->t[u].start;
        size_t l = (size_t)(g->t[u].end - g->t[u].start);
        if (l > 5 && !memcmp(s, "data:", 5)) {
            const char* c = NULL; size_t k;
            for (k = 0; k + 7 < l; k++)
                if (!memcmp(s + k, ";base64,", 8)) { c = s + k + 8; break; }
            if (!c) return -1;
            tmp = b64dec(c, l - (size_t)(c - s), &len);
        } else {
            char path[192];
            if (!uri_path(g, s, l, path, sizeof path)) return -1;
            tmp = slurp(path, &len);
        }
        if (!tmp) return -1;
        data = tmp;
    }
    unsigned uw = 0, uh = 0;
    unsigned char* out = NULL;
    unsigned err = lodepng_decode32(&out, &uw, &uh, data, len);
    free(tmp);
    if (err || !out) { free(out); return -1; }
    if (!uw || !uh || uw > MAXTEX || uh > MAXTEX) { free(out); return -1; }
    HImg* s2 = &m->img[m->nimg];
    s2->idx = idx; s2->px = out; s2->w = (int)uw; s2->h = (int)uh; s2->tex = 0;
    return m->nimg++;
}

/* textures[ti] -> (image slot, sampler filters) */
static int tex_slot(Build* b, int ti, int* magf, int* minf)
{
    GCtx* g = b->g;
    int tx = jelem(g, groot(g, "textures"), ti);
    if (tx < 0) return -1;
    int sm = jelem(g, groot(g, "samplers"), gi(g, tx, "sampler", -1));
    if (sm >= 0) {
        if (magf) *magf = gi(g, sm, "magFilter", 0);
        if (minf) *minf = gi(g, sm, "minFilter", 0);
    }
    return img_slot(b, gi(g, tx, "source", -1));
}

/* one HGroup per glTF material — the unit of a draw call */
static int group_for(Build* b, int matIdx)
{
    GCtx* g = b->g;
    HMesh* m = b->m;
    int i;
    for (i = 0; i < m->ngrp; i++) if (m->grp[i].mat == matIdx) return i;
    if (m->ngrp >= MAXGRP) return -1;
    HGroup* h = &m->grp[m->ngrp];
    memset(h, 0, sizeof *h);
    h->mat = matIdx;
    h->albedo = h->normal = -1;
    h->base[0] = h->base[1] = h->base[2] = h->base[3] = 1.0f;
    h->metal = 1.0f; h->rough = 1.0f;      /* the glTF defaults */
    h->cutoff = -1.0f;
    int mt = jelem(g, groot(g, "materials"), matIdx);
    if (mt >= 0) {
        int pbr = jfield(g, mt, "pbrMetallicRoughness");
        gvec(g, pbr, "baseColorFactor", h->base, 4);
        h->metal = (float)gf(g, pbr, "metallicFactor", 1.0);
        h->rough = (float)gf(g, pbr, "roughnessFactor", 1.0);
        h->doubleSided = gi(g, mt, "doubleSided", 0);
        /* MASK cuts out at its own threshold. BLEND would need the draws
           sorted back to front, which this pass does not do, so it cuts out
           at half too — a hard edge beats a wrong one drawn out of order. */
        int am = jfield(g, mt, "alphaMode");
        if (gstreq(g, am, "MASK"))       h->cutoff = (float)gf(g, mt, "alphaCutoff", 0.5);
        else if (gstreq(g, am, "BLEND")) h->cutoff = 0.5f;
        int bct = gi(g, jfield(g, pbr, "baseColorTexture"), "index", -1);
        if (bct >= 0) h->albedo = tex_slot(b, bct, &h->magf, &h->minf);
        int nt = gi(g, jfield(g, mt, "normalTexture"), "index", -1);
        if (nt >= 0) h->normal = tex_slot(b, nt, NULL, NULL);
    }
    return m->ngrp++;
}

/* -------------------------------------------------------------- normals -- */

static void cross3(const float* a, const float* b, float* o)
{
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}

/* inverse-transpose of a 4x4's upper 3x3, so a node that rotates or scales
   non-uniformly still hands us normals that point out of the surface */
static int nrm_mat(const float* m, float* o)
{
    const float *c0 = m, *c1 = m + 4, *c2 = m + 8;
    float a[3], b[3], c[3];
    int i;
    cross3(c1, c2, a); cross3(c2, c0, b); cross3(c0, c1, c);
    float det = c0[0]*a[0] + c0[1]*a[1] + c0[2]*a[2];
    if (fabsf(det) < 1e-12f) return 0;
    float inv = 1.0f / det;
    for (i = 0; i < 3; i++) { o[0*3+i] = a[i]*inv; o[1*3+i] = b[i]*inv; o[2*3+i] = c[i]*inv; }
    return 1;
}

static void nrm_apply(const float* o, const float* v, float* out)
{
    int r;
    for (r = 0; r < 3; r++) out[r] = o[0*3+r]*v[0] + o[1*3+r]*v[1] + o[2*3+r]*v[2];
}

static void nrm_unit(float* v)
{
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    else { v[0] = 0.0f; v[1] = 1.0f; v[2] = 0.0f; }
}

/* ------------------------------------------------------------ mesh walk -- */

static void emit_mesh(Build* b, int meshIdx, const float* m)
{
    GCtx* g = b->g;
    float nm[9];
    int haveNM = nrm_mat(m, nm);
    int me = jelem(g, groot(g, "meshes"), meshIdx);
    int prims = jfield(g, me, "primitives");
    int np = jlen(g, prims), pi;
    for (pi = 0; pi < np; pi++) {
        int pr = jelem(g, prims, pi);
        if (gi(g, pr, "mode", 4) != 4) continue;
        int at = jfield(g, pr, "attributes");
        GAcc pos, nrm, uv, idx;
        int pa = gi(g, at, "POSITION", -1);
        if (pa < 0 || !gacc(g, pa, &pos) || pos.ncomp < 3) continue;
        int na = gi(g, at, "NORMAL", -1);
        int hasN = na >= 0 && gacc(g, na, &nrm) && nrm.ncomp >= 3 &&
                   nrm.count >= pos.count && haveNM;
        int ua = gi(g, at, "TEXCOORD_0", -1);
        int hasUV = ua >= 0 && gacc(g, ua, &uv) && uv.ncomp >= 2 &&
                    uv.count >= pos.count;
        int ia = gi(g, pr, "indices", -1);
        int hasI = ia >= 0 && gacc(g, ia, &idx) && idx.ncomp == 1 && compsz(idx.comp);
        int nverts = hasI ? idx.count : pos.count;
        int grp = group_for(b, gi(g, pr, "material", -1));
        if (grp < 0) continue;
        int k;
        for (k = 0; k + 2 < nverts; k += 3) {
            if (b->ntri >= MAXTRI) { b->trunc = 1; return; }
            /* winding reversed BECAUSE Z is negated: mirroring flips the sense
               of a triangle, and reversing it back leaves the geometric normal
               agreeing with the shading normal we mirror the same way */
            static const int order[3] = { 0, 2, 1 };
            float p[3][3], n[3][3], t[3][2];
            int c, bad = 0;
            for (c = 0; c < 3; c++) {
                unsigned vi = hasI ? gidx(&idx, k + order[c])
                                   : (unsigned)(k + order[c]);
                if (vi >= (unsigned)pos.count) { bad = 1; break; }
                float v[3];
                gread(&pos, (int)vi, v, 3);
                mat_pt(m, v, p[c]);
                p[c][2] = -p[c][2];                 /* glTF -> 3DO model space */
                if (hasN) {
                    float s[3];
                    gread(&nrm, (int)vi, s, 3);
                    nrm_apply(nm, s, n[c]);
                    n[c][2] = -n[c][2];
                    nrm_unit(n[c]);
                }
                if (hasUV && vi < (unsigned)uv.count) gread(&uv, (int)vi, t[c], 2);
                else { t[c][0] = 0.0f; t[c][1] = 0.0f; }
            }
            if (bad) continue;
            if (!hasN) {                            /* flat: the face's own */
                float e1[3], e2[3], fn[3];
                for (c = 0; c < 3; c++) {
                    e1[c] = p[1][c] - p[0][c];
                    e2[c] = p[2][c] - p[0][c];
                }
                cross3(e1, e2, fn);
                nrm_unit(fn);
                for (c = 0; c < 3; c++) memcpy(n[c], fn, sizeof fn);
            }
            float* op = b->p[b->ntri];
            float* on = b->n[b->ntri];
            float* ot = b->t[b->ntri];
            for (c = 0; c < 3; c++) {
                op[c*3+0] = p[c][0]; op[c*3+1] = p[c][1]; op[c*3+2] = p[c][2];
                on[c*3+0] = n[c][0]; on[c*3+1] = n[c][1]; on[c*3+2] = n[c][2];
                ot[c*2+0] = t[c][0]; ot[c*2+1] = t[c][1];
            }
            b->gid[b->ntri] = (unsigned char)grp;
            b->m->grp[grp].count++;
            b->ntri++;
        }
    }
}

static void walk_node(Build* b, int nodeIdx, const float* parent, int depth)
{
    GCtx* g = b->g;
    if (depth > 32) return;
    int nd = jelem(g, groot(g, "nodes"), nodeIdx);
    if (nd < 0) return;
    float loc[16], m[16];
    node_local(g, nd, loc);
    mat_mul(parent, loc, m);
    int mi = gi(g, nd, "mesh", -1);
    if (mi >= 0) emit_mesh(b, mi, m);
    int ch = jfield(g, nd, "children");
    int nc = jlen(g, ch), k;
    for (k = 0; k < nc; k++)
        walk_node(b, (int)jval(g, jelem(g, ch, k), -1.0), m, depth + 1);
}

/* --------------------------------------------------------------- loader -- */

static void mesh_free(HMesh* m)
{
    int i;
    free(m->v);
    m->v = NULL;
    for (i = 0; i < m->nimg; i++) free(m->img[i].px);
    m->ntri = 0; m->ngrp = 0; m->nimg = 0; m->uploaded = 0;
    memset(m->img, 0, sizeof m->img);
    memset(m->grp, 0, sizeof m->grp);
}

/* GL objects survive a reload only to be deleted here, on a thread that has a
   context; tagpu_hires_glreset forgets them instead, the context being gone */
static void mesh_gl_free(HMesh* m)
{
    int i;
    for (i = 0; i < MAXIMG; i++)
        if (m->img[i].tex) { glDeleteTextures(1, &m->img[i].tex); m->img[i].tex = 0; }
    if (m->vbo) { glDeleteBuffers(1, &m->vbo); m->vbo = 0; }
    if (m->vao) { glDeleteVertexArrays(1, &m->vao); m->vao = 0; }
}

static void ctx_free(GCtx* g)
{
    int i;
    for (i = 0; i < MAXBUF; i++) free(g->own[i]);
}

static int load_gltf(HMesh* m, const char* path)
{
    size_t flen = 0;
    unsigned char* file = slurp(path, &flen);
    char b[192];
    if (!file) return 0;

    char* json = NULL;
    size_t jsonlen = 0;
    GCtx g;
    JTok* toks = NULL;
    Build bd;
    int i, ntok = 0;
    memset(&g, 0, sizeof g);
    memset(&bd, 0, sizeof bd);

    if (flen >= 12 && !memcmp(file, "glTF", 4)) {
        unsigned total;
        memcpy(&total, file + 8, 4);
        if (total > flen) total = (unsigned)flen;
        size_t o = 12;
        while (o + 8 <= total) {
            unsigned clen, ctype;
            memcpy(&clen, file + o, 4);
            memcpy(&ctype, file + o + 4, 4);
            o += 8;
            if (clen > total - o) break;
            if (ctype == 0x4E4F534Au && !json) {
                json = malloc(clen + 1);
                if (!json) { free(file); return 0; }
                memcpy(json, file + o, clen);
                json[clen] = 0;
                jsonlen = clen;
            } else if (ctype == 0x004E4942u && !g.bin) {
                g.bin = file + o; g.binlen = clen;
            }
            o += clen + ((4 - (clen & 3)) & 3);
        }
        if (!json) { hlog("hires: GLB has no JSON chunk"); free(file); return 0; }
    } else {
        json = (char*)file;
        jsonlen = flen;
    }

    {   /* two-pass tokenise: count, allocate exactly, fill */
        JP p;
        memset(&p, 0, sizeof p);
        p.js = json; p.len = jsonlen; p.super = -1;
        ntok = jparse(&p);
        if (ntok <= 0) {
            _snprintf(b, sizeof b, "hires: %s is not valid JSON", path); hlog(b);
            goto fail;
        }
        toks = malloc((size_t)ntok * sizeof *toks);
        if (!toks) goto fail;
        memset(&p, 0, sizeof p);
        p.js = json; p.len = jsonlen; p.super = -1; p.toks = toks; p.ntok = ntok;
        if (jparse(&p) != ntok || toks[0].type != JS_OBJ) {
            _snprintf(b, sizeof b, "hires: %s JSON did not re-parse", path); hlog(b);
            goto fail;
        }
    }

    g.js = json; g.t = toks; g.n = ntok; g.root = 0;
    {   /* the document's directory, for .gltf sidecar buffers and images */
        const char* slash = strrchr(path, '\\');
        size_t dl = slash ? (size_t)(slash - path) + 1 : 0;
        if (dl >= sizeof g.dir) dl = 0;
        memcpy(g.dir, path, dl);
        g.dir[dl] = 0;
    }

    mesh_free(m);
    bd.g = &g;
    bd.m = m;
    bd.p   = malloc(sizeof(float) * 9 * MAXTRI);
    bd.n   = malloc(sizeof(float) * 9 * MAXTRI);
    bd.t   = malloc(sizeof(float) * 6 * MAXTRI);
    bd.gid = malloc(MAXTRI);
    if (!bd.p || !bd.n || !bd.t || !bd.gid) goto fail;

    {
        float I[16];
        mat_id(I);
        int sc = jelem(&g, groot(&g, "scenes"), gi(&g, g.root, "scene", 0));
        if (sc >= 0) {
            int roots = jfield(&g, sc, "nodes");
            int nr = jlen(&g, roots), k;
            for (k = 0; k < nr; k++)
                walk_node(&bd, (int)jval(&g, jelem(&g, roots, k), -1.0), I, 0);
        } else {                       /* no scene: every node is its own root */
            int nn = jlen(&g, groot(&g, "nodes")), k;
            for (k = 0; k < nn; k++) walk_node(&bd, k, I, 0);
        }
    }

    if (bd.ntri == 0) {
        _snprintf(b, sizeof b, "hires: %s has no drawable triangles", path);
        hlog(b);
        goto fail;
    }

    /* sort triangles into their groups, so a material is one contiguous draw */
    {
        int fill[MAXGRP], run = 0;
        for (i = 0; i < m->ngrp; i++) { m->grp[i].first = run; fill[i] = run; run += m->grp[i].count; }
        m->v = malloc(sizeof(float) * HVSTRIDE * 3 * (size_t)bd.ntri);
        if (!m->v) goto fail;
        for (i = 0; i < bd.ntri; i++) {
            int gidx2 = bd.gid[i], dst = fill[gidx2]++, c;
            float* o = m->v + (size_t)dst * 3 * HVSTRIDE;
            for (c = 0; c < 3; c++, o += HVSTRIDE) {
                o[0] = bd.p[i][c*3+0]; o[1] = bd.p[i][c*3+1]; o[2] = bd.p[i][c*3+2];
                o[3] = bd.n[i][c*3+0]; o[4] = bd.n[i][c*3+1]; o[5] = bd.n[i][c*3+2];
                o[6] = bd.t[i][c*2+0]; o[7] = bd.t[i][c*2+1];
            }
        }
        m->ntri = bd.ntri;
    }

    _snprintf(b, sizeof b, "hires: %s loaded, %d tris, %d material%s, %d image%s%s",
              path, m->ntri, m->ngrp, m->ngrp == 1 ? "" : "s",
              m->nimg, m->nimg == 1 ? "" : "s",
              bd.trunc ? " (TRUNCATED at the triangle cap)" : "");
    hlog(b);

    free(bd.p); free(bd.n); free(bd.t); free(bd.gid);
    ctx_free(&g);
    free(toks);
    if (json != (char*)file) free(json);
    free(file);
    return 1;

fail:
    free(bd.p); free(bd.n); free(bd.t); free(bd.gid);
    mesh_free(m);
    ctx_free(&g);
    free(toks);
    if (json != (char*)file) free(json);
    free(file);
    return 0;
}

/* ------------------------------------------------------------------ GL -- */

typedef void (APIENTRY *PFN_GENMIPMAP)(GLenum);
static PFN_GENMIPMAP x_glGenerateMipmap;
static int s_glprobed = 0;

static void gl_probe(void)
{
    if (s_glprobed) return;
    s_glprobed = 1;
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress("glGenerateMipmap") : NULL;
    if (!p) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) p = (void*)GetProcAddress(gl, "glGenerateMipmap");
    }
    x_glGenerateMipmap = (PFN_GENMIPMAP)p;
}

static GLuint upload_img(HImg* im, int magf, int minf)
{
    if (im->tex) return im->tex;
    glGenTextures(1, &im->tex);
    glBindTexture(GL_TEXTURE_2D, im->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im->w, im->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, im->px);
    int mips = 0;
    if (x_glGenerateMipmap) { x_glGenerateMipmap(GL_TEXTURE_2D); mips = 1; }
    /* A replacement unit is a 1024px texture on a ~40px sprite: without mips
       every frame resamples different texels and the surface crawls. Honour
       the glTF sampler's own filters when it named them, but never select a
       mipmapped minification we could not build. */
    int mn = minf ? minf : GL_LINEAR_MIPMAP_LINEAR;
    if (!mips && (mn == GL_NEAREST_MIPMAP_NEAREST || mn == GL_LINEAR_MIPMAP_NEAREST ||
                  mn == GL_NEAREST_MIPMAP_LINEAR  || mn == GL_LINEAR_MIPMAP_LINEAR))
        mn = (mn == GL_NEAREST_MIPMAP_NEAREST || mn == GL_NEAREST_MIPMAP_LINEAR)
             ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mn);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf ? magf : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    free(im->px);
    im->px = NULL;
    return im->tex;
}

static GLuint white_tex(void)
{
    if (!s_white) {
        const unsigned char px[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &s_white);
        glBindTexture(GL_TEXTURE_2D, s_white);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return s_white;
}

/* ------------------------------------------------------------------ API -- */

const void* tagpu_hires_mesh(const char* defname)
{
    int i;
    HMesh* m = NULL;
    for (i = 0; i < s_nmesh; i++)
        if (!lstrcmpiA(s_mesh[i].name, defname)) { m = &s_mesh[i]; break; }
    if (!m) {
        if (s_nmesh >= MAXMESH) return NULL;
        m = &s_mesh[s_nmesh];
        memset(m, 0, sizeof *m);
        lstrcpynA(m->name, defname, sizeof m->name);
        s_nmesh++;
    }
    char glb[128], gltf[128];
    _snprintf(glb,  sizeof glb,  "hires\\%s.glb",  m->name);
    _snprintf(gltf, sizeof gltf, "hires\\%s.gltf", m->name);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    const char* use = NULL;
    if (GetFileAttributesExA(glb, GetFileExInfoStandard, &fad)) use = glb;
    else if (GetFileAttributesExA(gltf, GetFileExInfoStandard, &fad)) use = gltf;
    if (!m->announced) {
        /* say once, per unit type, where the slot looked. Silence here used to
           be indistinguishable from a load that failed, and the two want very
           different fixes. */
        char b[224];
        m->announced = 1;
        _snprintf(b, sizeof b, "hires: %s -> %s", m->name, use ? use : "no replacement (looked for hires\\<name>.glb / .gltf)");
        hlog(b);
    }
    if (!use) {
        /* the file went away: forget it, so putting it back reloads even if
           its write time is older than the one we last saw */
        if (m->valid) { mesh_gl_free(m); mesh_free(m); }
        m->valid = 0;
        memset(&m->mtime, 0, sizeof m->mtime);
        m->path[0] = 0;
        return NULL;
    }
    if (strcmp(m->path, use) != 0 ||
        CompareFileTime(&fad.ftLastWriteTime, &m->mtime) != 0) {
        mesh_gl_free(m);
        m->mtime = fad.ftLastWriteTime;
        lstrcpynA(m->path, use, sizeof m->path);
        m->valid = load_gltf(m, use);
    }
    return m->valid ? (const void*)m : NULL;
}

int tagpu_hires_ngroup(const void* mesh) { return ((const HMesh*)mesh)->ngrp; }

int tagpu_hires_group(const void* mesh, int i, TAGPU_HGROUP* out)
{
    const HMesh* m = (const HMesh*)mesh;
    if (i < 0 || i >= m->ngrp || !m->uploaded) return 0;
    const HGroup* h = &m->grp[i];
    out->albedo = h->albedo >= 0 ? m->img[h->albedo].tex : s_white;
    out->normal = h->normal >= 0 ? m->img[h->normal].tex : 0;
    memcpy(out->base, h->base, sizeof out->base);
    out->metal = h->metal;
    out->rough = h->rough;
    out->cutoff = h->cutoff;
    out->doubleSided = h->doubleSided;
    out->first = h->first * 3;
    out->count = h->count * 3;
    return out->count > 0;
}

unsigned int tagpu_hires_vao(const void* mesh)
{
    HMesh* m = (HMesh*)mesh;
    int i;
    if (m->uploaded) return m->vao;
    if (!m->v || m->ntri <= 0) return 0;
    gl_probe();
    white_tex();
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)m->ntri * 3 * HVSTRIDE * sizeof(float)),
                 m->v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, HVSTRIDE * 4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, HVSTRIDE * 4, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, HVSTRIDE * 4, (void*)24);
    glBindVertexArray(0);
    for (i = 0; i < m->ngrp; i++) {
        HGroup* h = &m->grp[i];
        if (h->albedo >= 0) upload_img(&m->img[h->albedo], h->magf, h->minf);
        if (h->normal >= 0) upload_img(&m->img[h->normal], 0, 0);
    }
    /* the vertex data is the GPU's now, and the pixels went with the textures */
    free(m->v);
    m->v = NULL;
    m->uploaded = 1;
    return m->vao;
}

void tagpu_hires_glreset(void)
{
    int i, k;
    for (i = 0; i < s_nmesh; i++) {
        HMesh* m = &s_mesh[i];
        for (k = 0; k < MAXIMG; k++) m->img[k].tex = 0;
        m->vao = m->vbo = 0;
        /* the buffers those names stood for are gone with the context, and the
           CPU copies were freed at upload — reload from the file instead */
        mesh_free(m);
        m->valid = 0;
        memset(&m->mtime, 0, sizeof m->mtime);
        m->path[0] = 0;
    }
    s_white = 0;
    s_glprobed = 0;
}
