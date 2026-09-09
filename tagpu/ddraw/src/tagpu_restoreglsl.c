/* tagpu_restoreglsl.c — the Classic++ restorer as fragment passes, in the
   game's own GL context, sliced across frames. research/notes/renderers.md 4c
   has the decisions; tagpu_restore_glsl.h has the shaders and the layout they
   assume; tools/tascene-restore.js is this same driver in the browser lab,
   where the shaders were proven against the unditherer (max 1 level on
   0.0012 % of bytes, fp32; the feature atlas's far band exact).

   THE MODEL. unditherer/model.py: 3x3 conv 3->64 + ReLU, ten x (3x3 conv
   64->64 + BatchNorm + ReLU), 3x3 conv 64->3, out = in - net(in), RGB in
   [0,1]. BatchNorm is folded into the weight file (unditherer/weights.py),
   which lays every layer out as the conv pass indexes it: one std140 block of
   mat4 per output channel-tile k, bias first, then a mat4 per (tap, input
   tile). A conv draw binds NK consecutive k-blocks as one uniform range and
   writes NK output tiles through NK colour attachments (layers of a 2D-array
   texture, one per four channels); the activations ping-pong between two such
   arrays. NK=1 is the channel-tiled cost, NK=4 fetches each activation texel
   four times less -- the browser measured the layouts and the DLL takes the
   widest one the device's MAX_UNIFORM_BLOCK_SIZE allows.

   THE PADDING RULE, the thing that defines the pixels. The unditherer runs a
   tile whose opposite edges agree within 12 levels wrap-padded by 12 to 56x56
   and centre-cropped, and every other tile at 32x32 with zero padding at
   every layer. Here both are one mechanism: every slot has a valid rect and
   a tap outside it reads 0, at every layer (the rect test in the conv shader)
   -- because zero-padding only the INPUT and running unmasked is a different
   network: layer 2 would read layer 1's gutter, which is relu(bias), not 0.
   The FILL pass wrap-pads inside the rect; the OUT pass centre-crops.

   BATCHES, shared with the lab byte for byte (tascene-restore.js
   batchFrames): a frame's padded edge S is max(w, h) + 2 x depth if it
   wraps. A job's queue is taken in order; a batch holds frames of one SIZE
   CLASS (the ladder below) up to a square slot grid of min(8, ACT_MAX /
   class) per side -- then shrunk to the smallest square that holds what was
   taken, since the passes cost by the fragment -- and its slot pitch is its
   largest S. Every slot has its own rect, so a 30x25 tree and a 63x60 rock
   share a batch. Activations are
   sized to the largest cols x S seen, never above ACT_MAX (a 512-px frame
   restores alone in a 1x1 grid), and are freed after IDLE_FRAMES without work
   -- the queues live as long as the map, the 100 MB does not.

   SLICING. tagpu_native.c calls tagpu_rglsl_step() once per frame after the
   gathers and before the renders. Each call issues draws until an estimate
   of their GPU time reaches the budget (12 ms by default), the estimate being
   the previous slice's GL_TIME_ELAPSED query divided by the work it carried.
   Draws are the unit: a 64-tile batch of the full model is 1 (fill) +
   ceil(16/NK) x 11 (the conv layers with 16 output tiles) + 1 (the last
   layer has one) + 1 (out) = 47 of them at NK=4.
   Without ARB_timer_query (a 3.2 context is not promised it, though NVIDIA
   and Mesa both expose it) the slice is a fixed, conservative draw count.
   No second context, no worker thread: the GPU is the only engine, and a slow
   one restores slower without stalling anything.

   THE ATLAS IS THE TARGET. The OUT pass renders into the caller's RGBA8 atlas
   in the caller's own cell layout, replicated guard texels included, so there
   is no CPU copy of the result and no upload; the source is the R8 atlas the
   caller already built. The bytes never leave the GPU. Nothing is written to
   disk by this module (renderers.md 2.5b). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_restore_glsl.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"

#define SLOT_COLS    8
#define SLOT_ROWS    8
#define BATCH        (SLOT_COLS * SLOT_ROWS)       /* frames per batch, at most  */
#define ACT_MAX      512                           /* activation side cap, texels */
#define MAX_JOBS     6      /* terrain 0, features 1, effects 2, 3DO units 3, the UI 4 (G15e) */
#define MAX_NK       8
#define MAX_LAYERS   32
#define TILEABLE_THR 12.0
#define OPT_FILE     "tagpu_restoreglsl.on"
#define DEFAULT_BUDGET_MS 12.0
#define FIXED_DRAWS  6                             /* no timer: draws per frame */
#define IDLE_FRAMES  180                           /* activations freed after   */
#define QUEUE_LOG_FRAMES 300                       /* a queue's tally, at most  */

/* the size-class ladder: a batch holds one class; tascene-restore.js's twin */
static const int s_classes[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512 };
#define NCLASSES (int)(sizeof s_classes / sizeof s_classes[0])

#ifndef GL_MAX_UNIFORM_BLOCK_SIZE
#define GL_MAX_UNIFORM_BLOCK_SIZE 0x8A30
#endif
#ifndef GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#endif
#ifndef GL_MAX_COLOR_ATTACHMENTS
#define GL_MAX_COLOR_ATTACHMENTS 0x8CDF
#endif
#ifndef GL_MAX_DRAW_BUFFERS
#define GL_MAX_DRAW_BUFFERS 0x8824
#endif

static void rlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

/* ---- GL entry points this module needs beyond opengl_utils ---- */
typedef void (APIENTRY *PFN_TEXIMAGE3D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
typedef void (APIENTRY *PFN_FBTEXLAYER)(GLenum,GLenum,GLuint,GLint,GLint);
typedef void (APIENTRY *PFN_BINDBUFRANGE)(GLenum,GLuint,GLuint,GLintptr,GLsizeiptr);
typedef GLuint (APIENTRY *PFN_GETBLOCKIDX)(GLuint,const GLchar*);
typedef void (APIENTRY *PFN_BLOCKBINDING)(GLuint,GLuint,GLuint);
typedef void (APIENTRY *PFN_GENQUERIES)(GLsizei,GLuint*);
typedef void (APIENTRY *PFN_DELQUERIES)(GLsizei,const GLuint*);
typedef void (APIENTRY *PFN_BEGINQUERY)(GLenum,GLuint);
typedef void (APIENTRY *PFN_ENDQUERY)(GLenum);
typedef void (APIENTRY *PFN_QUERYUI64)(GLuint,GLenum,GLuint64*);
typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_CAP)(GLenum);
typedef GLboolean (APIENTRY *PFN_ISENABLED)(GLenum);
typedef void (APIENTRY *PFN_COLORMASK)(GLboolean,GLboolean,GLboolean,GLboolean);
typedef void (APIENTRY *PFN_DEPTHMASK)(GLboolean);
typedef void (APIENTRY *PFN_GETBOOLEANV)(GLenum,GLboolean*);
typedef void (APIENTRY *PFN_CLEARCOLOR)(GLfloat,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_GETFLOATV)(GLenum,GLfloat*);
typedef void (APIENTRY *PFN_GETINTEGERV)(GLenum,GLint*);
static PFN_TEXIMAGE3D   x_glTexImage3D;
static PFN_FBTEXLAYER   x_glFramebufferTextureLayer;
static PFN_BINDBUFRANGE x_glBindBufferRange;
static PFN_GETBLOCKIDX  x_glGetUniformBlockIndex;
static PFN_BLOCKBINDING x_glUniformBlockBinding;
static PFN_GENQUERIES   x_glGenQueries;
static PFN_DELQUERIES   x_glDeleteQueries;
static PFN_BEGINQUERY   x_glBeginQuery;
static PFN_ENDQUERY     x_glEndQuery;
static PFN_QUERYUI64    x_glGetQueryObjectui64v;
static PFN_DRAWARRAYS   x_glDrawArrays;
static PFN_UNIFORM2F    x_glUniform2f;
static PFN_CAP          x_glDisable, x_glEnable;
static PFN_ISENABLED    x_glIsEnabled;
static PFN_COLORMASK    x_glColorMask;
static PFN_DEPTHMASK    x_glDepthMask;
static PFN_GETBOOLEANV  x_glGetBooleanv;
static PFN_CLEARCOLOR   x_glClearColor;
static PFN_GETFLOATV    x_glGetFloatv;
static PFN_GETINTEGERV  x_glGetIntegerv;
/* opengl_utils.h declares glGetIntegerv but this build path does not resolve
   it (tagpu_terr.c fetches its own too), so this module resolves both */
#define glGetFloatv x_glGetFloatv
#define glGetIntegerv x_glGetIntegerv

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

static int resolve_gl(void)
{
    static int done = 0;
    if (done) return done > 0;
    x_glTexImage3D = (PFN_TEXIMAGE3D)getgl("glTexImage3D");
    x_glFramebufferTextureLayer = (PFN_FBTEXLAYER)getgl("glFramebufferTextureLayer");
    x_glBindBufferRange = (PFN_BINDBUFRANGE)getgl("glBindBufferRange");
    x_glGetUniformBlockIndex = (PFN_GETBLOCKIDX)getgl("glGetUniformBlockIndex");
    x_glUniformBlockBinding = (PFN_BLOCKBINDING)getgl("glUniformBlockBinding");
    x_glGenQueries = (PFN_GENQUERIES)getgl("glGenQueries");
    x_glDeleteQueries = (PFN_DELQUERIES)getgl("glDeleteQueries");
    x_glBeginQuery = (PFN_BEGINQUERY)getgl("glBeginQuery");
    x_glEndQuery = (PFN_ENDQUERY)getgl("glEndQuery");
    x_glGetQueryObjectui64v = (PFN_QUERYUI64)getgl("glGetQueryObjectui64v");
    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glUniform2f = (PFN_UNIFORM2F)getgl("glUniform2f");
    x_glDisable = (PFN_CAP)getgl("glDisable");
    x_glEnable = (PFN_CAP)getgl("glEnable");
    x_glIsEnabled = (PFN_ISENABLED)getgl("glIsEnabled");
    x_glColorMask = (PFN_COLORMASK)getgl("glColorMask");
    x_glDepthMask = (PFN_DEPTHMASK)getgl("glDepthMask");
    x_glGetBooleanv = (PFN_GETBOOLEANV)getgl("glGetBooleanv");
    x_glClearColor = (PFN_CLEARCOLOR)getgl("glClearColor");
    x_glGetFloatv = (PFN_GETFLOATV)getgl("glGetFloatv");
    x_glGetIntegerv = (PFN_GETINTEGERV)getgl("glGetIntegerv");
    {
        const char* missing =
            !x_glTexImage3D ? "glTexImage3D" : !x_glFramebufferTextureLayer ? "glFramebufferTextureLayer" :
            !x_glBindBufferRange ? "glBindBufferRange" : !x_glGetUniformBlockIndex ? "glGetUniformBlockIndex" :
            !x_glUniformBlockBinding ? "glUniformBlockBinding" : !x_glDrawArrays ? "glDrawArrays" :
            !x_glUniform2f ? "glUniform2f" : !x_glDisable ? "glDisable" : !x_glEnable ? "glEnable" :
            !x_glIsEnabled ? "glIsEnabled" : !x_glColorMask ? "glColorMask" : !x_glDepthMask ? "glDepthMask" :
            !x_glGetBooleanv ? "glGetBooleanv" : !x_glClearColor ? "glClearColor" : !x_glGetFloatv ? "glGetFloatv" :
            !glGetIntegerv ? "glGetIntegerv" : !glGenBuffers ? "glGenBuffers" : !glBindBuffer ? "glBindBuffer" :
            !glBufferData ? "glBufferData" : NULL;
        done = missing ? -1 : 1;
        if (missing) { char b[128]; _snprintf(b, sizeof b, "restoreglsl: GL entry point %s is missing", missing); rlog(b); }
    }
    return done > 0;
}

static int has_timer_query(void)
{
    GLint n = 0, i;
    const char* v = (const char*)glGetString(GL_VERSION);
    if (v && ((v[0] == '3' && v[2] >= '3') || v[0] >= '4')) return 1;
    if (!glGetStringi) return 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && (!strcmp(e, "GL_ARB_timer_query") || !strcmp(e, "GL_EXT_timer_query"))) return 1;
    }
    return 0;
}

/* ---- the weight file (unditherer/weights.py) ---- */
typedef struct { unsigned offset, jin, kout, kstride; } Layer;   /* in vec4 texels */
static struct {
    int    depth, ch, ntex, kmax;      /* kmax: mat4s in the widest k-block */
    Layer  layer[MAX_LAYERS];
    float* body;
    char   name[16];
} s_w;

static int load_weights(const char* model)
{
    char path[MAX_PATH], b[300];
    FILE* f;
    unsigned char hdr[16];
    unsigned depth, ch, ntex, l;
    size_t want;

    if (s_w.body && !strcmp(s_w.name, model)) return 1;
    free(s_w.body); s_w.body = NULL;
    /* beside TotalA.exe, where tacli links it */
    path[0] = 0;
    if (GetModuleFileNameA(NULL, path, sizeof path)) {
        char* p = strrchr(path, '\\');
        if (p && (size_t)(p - path) + 24 < sizeof path) _snprintf(p + 1, 24, "%s.w32.bin", model);
        else path[0] = 0;
    }
    if (!path[0]) _snprintf(path, sizeof path, "%s.w32.bin", model);
    f = fopen(path, "rb");
    if (!f) { _snprintf(b, sizeof b, "restoreglsl: no %s -- Classic++ stays indexed", path); rlog(b); return 0; }
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "TAW1", 4)) { rlog("restoreglsl: weight file is not TAW1"); fclose(f); return 0; }
    memcpy(&depth, hdr + 4, 4); memcpy(&ch, hdr + 8, 4); memcpy(&ntex, hdr + 12, 4);
    if (depth < 2 || depth > MAX_LAYERS || ch < 4 || ch > 256 || (ch & 3) || ntex == 0 || ntex > (1u << 24)) {
        rlog("restoreglsl: weight header out of range"); fclose(f); return 0;
    }
    s_w.depth = (int)depth; s_w.ch = (int)ch; s_w.ntex = (int)ntex; s_w.kmax = 0;
    for (l = 0; l < depth; l++) {
        if (fread(&s_w.layer[l], 1, 16, f) != 16) { rlog("restoreglsl: weight header truncated"); fclose(f); return 0; }
        {
            const Layer* L = &s_w.layer[l];
            /* every product in 64 bits: a corrupt header must not wrap its way past the bound */
            unsigned long long end = (unsigned long long)L->offset + (unsigned long long)L->kout * L->kstride;
            if (L->jin == 0 || L->kout == 0 || L->kstride == 0 || (L->kstride & 15) ||
                L->kstride > ntex || L->kout > ntex || end > ntex || L->jin > 64) {
                rlog("restoreglsl: weight layer table inconsistent"); fclose(f); return 0;
            }
        }
        if ((int)(s_w.layer[l].kstride / 4) > s_w.kmax) s_w.kmax = (int)(s_w.layer[l].kstride / 4);
    }
    want = (size_t)ntex * 16;
    s_w.body = (float*)malloc(want);
    if (!s_w.body || fread(s_w.body, 1, want, f) != want) {
        rlog("restoreglsl: weight body truncated"); free(s_w.body); s_w.body = NULL; fclose(f); return 0;
    }
    fclose(f);
    strncpy(s_w.name, model, sizeof s_w.name - 1); s_w.name[sizeof s_w.name - 1] = 0;
    _snprintf(b, sizeof b, "restoreglsl: %s: %dx%d, %d vec4 (%u KB)", path, s_w.depth, s_w.ch, s_w.ntex, (unsigned)(want >> 10));
    rlog(b);
    return 1;
}

/* ---- options (OPT_FILE, read once per GL context) ---- */
static struct { int tiny, fp16, nk, log; double budget; } s_opt;

static void read_options(void)
{
    HANDLE h;
    char buf[256]; DWORD n = 0;
    s_opt.tiny = 0; s_opt.fp16 = 0; s_opt.nk = 0; s_opt.log = 0; s_opt.budget = DEFAULT_BUDGET_MS;
    h = CreateFileA(OPT_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
        char* p = buf;
        buf[n] = 0;
        while (*p) {
            char* q; int last;
            while (*p && *p <= ' ') p++;
            q = p;
            while (*q && *q > ' ') q++;
            last = (*q == 0);
            *q = 0;
            if (!lstrcmpiA(p, "tiny")) s_opt.tiny = 1;
            else if (!lstrcmpiA(p, "fp16")) s_opt.fp16 = 1;
            else if (!lstrcmpiA(p, "log")) s_opt.log = 1;
            else if (!strncmp(p, "nk=", 3)) { int k = atoi(p + 3); if (k == 1 || k == 2 || k == 4 || k == 8) s_opt.nk = k; }
            else if (!strncmp(p, "budget=", 7)) { double v = atof(p + 7); if (v >= 0.5 && v <= 100.0) s_opt.budget = v; }
            if (last) break;
            p = q + 1;
        }
    }
    CloseHandle(h);
}

/* ---- the jobs ---- */
typedef struct { TAGPU_RGLSL_FRAME f; short S, cls; } QF;   /* a queued frame with its padded edge */

struct TAGPU_RGLSL_JOB {
    int    used, prio, oneshot, failed;
    char   tag[12];
    GLuint atlasTex, destTex, palTex;
    int    atlasW, atlasH, destW, destH;
    QF*    q; int qn, qcap;                  /* queued, not yet in a batch     */
    /* the batch in flight */
    QF     bf[BATCH]; int bn, bS, bcols, inflight;
    int    pass, group, srcAct;
    /* the run: from the first frame queued while idle to the queue draining */
    int    running, rframes, rwrap, rbatches, rdraws, rslices;
    unsigned sliceMark;                      /* s_slice + 1 of the last slice it drew in */
    unsigned rcall0;                         /* s_calls when the run began: frames = s_calls - rcall0 */
    double rt0, rgpuNs, rgpuUnits, runits;   /* runits: work units issued this run */
    /* lifetime tallies for the queues' log line */
    int    tframes, tbatches, tdraws; double tgpuNs; unsigned lastTallySlice;
};
static struct TAGPU_RGLSL_JOB s_jobs[MAX_JOBS];

/* ---- shared GL state: once per context, for every job ---- */
static int    s_glReady;               /* programs, tables, weights uploaded  */
static int    s_glFailed;              /* and they cannot be: no more tries   */
static int    s_actFailed;             /* float targets do not work here      */
static GLuint s_progFill, s_progConv, s_progOut;
static GLint  s_uFill[7], s_uConv[6], s_uOut[7];
static GLuint s_ubo, s_rectTex, s_srcTex, s_keyTex, s_act[2], s_vao, s_outVAO, s_outVBO, s_fbo, s_destFBO;
static int    s_nk, s_wmax, s_actSide, s_actLayers, s_attached;
static float  s_rect[BATCH * 4], s_src[BATCH * 4], s_key[BATCH * 4];
static float  s_outVerts[BATCH * 6 * 8];
static unsigned s_slice;               /* slices issued in this context       */
static unsigned s_calls;               /* frames the driver has stepped, ready or not */
static int    s_idle;                  /* consecutive slices with nothing to do */
/* the budget */
static GLuint s_query[2];
static int    s_qHave[2], s_qFrame;
static double s_qUnits[2];
static double s_nsPerUnit;             /* 0 = unknown yet                     */
static int    s_qStall;                /* frames waited on one query result   */
static struct TAGPU_RGLSL_JOB* s_qJob[2];   /* the job each query timed       */

int tagpu_rglsl_tileable(const unsigned char* px, int w, int h, const unsigned char* pal, int key)
{
    double lr = 0.0, tb = 0.0;
    int i, c;
    if (key >= 0) {
        for (i = 0; i < h; i++) if (px[i * w] == key || px[i * w + w - 1] == key) return 0;
        for (i = 0; i < w; i++) if (px[i] == key || px[(h - 1) * w + i] == key) return 0;
    }
    for (i = 0; i < h; i++) {
        const unsigned char* a = pal + px[i * w] * 4;
        const unsigned char* b = pal + px[i * w + w - 1] * 4;
        for (c = 0; c < 3; c++) lr += abs((int)a[c] - (int)b[c]);
    }
    for (i = 0; i < w; i++) {
        const unsigned char* u = pal + px[i] * 4;
        const unsigned char* d = pal + px[(h - 1) * w + i] * 4;
        for (c = 0; c < 3; c++) tb += abs((int)u[c] - (int)d[c]);
    }
    return lr / (3.0 * h) < TILEABLE_THR && tb / (3.0 * w) < TILEABLE_THR;
}

static GLuint mksh(GLenum type, const char* prefix, const char* body, const char* label)
{
    const char* src[2] = { prefix, body };
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 2, src, NULL); glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char lg[600], b[700];
        glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
        _snprintf(b, sizeof b, "restoreglsl: %s FAILED: %s", label, lg); rlog(b);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint mkprog(GLuint vs, GLuint fs, const char* label)
{
    GLuint p; GLint ok = 0;
    if (!vs || !fs) return 0;
    p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char b[128]; _snprintf(b, sizeof b, "restoreglsl: %s link FAILED", label); rlog(b); glDeleteProgram(p); return 0; }
    return p;
}

static void slot_tex(GLuint* t)
{
    glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, SLOT_COLS, SLOT_ROWS, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static void free_act(void)
{
    if (s_act[0]) glDeleteTextures(2, s_act);
    s_act[0] = s_act[1] = 0; s_actSide = 0;
}

static void free_gl(void)
{
    if (s_progFill) glDeleteProgram(s_progFill);
    if (s_progConv) glDeleteProgram(s_progConv);
    if (s_progOut)  glDeleteProgram(s_progOut);
    if (s_ubo) glDeleteBuffers(1, &s_ubo);
    if (s_outVBO) glDeleteBuffers(1, &s_outVBO);
    if (s_rectTex) glDeleteTextures(1, &s_rectTex);
    if (s_srcTex) glDeleteTextures(1, &s_srcTex);
    if (s_keyTex) glDeleteTextures(1, &s_keyTex);
    free_act();
    if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
    if (s_destFBO) glDeleteFramebuffers(1, &s_destFBO);
    if (s_vao) glDeleteVertexArrays(1, &s_vao);
    if (s_outVAO) glDeleteVertexArrays(1, &s_outVAO);
    if (s_query[0] && x_glDeleteQueries) x_glDeleteQueries(2, s_query);
    s_progFill = s_progConv = s_progOut = 0;
    s_ubo = s_outVBO = s_rectTex = s_srcTex = s_keyTex = s_fbo = s_destFBO = s_vao = s_outVAO = 0;
    s_query[0] = s_query[1] = 0;
    s_glReady = 0;
}

/* programs, tables, weights, queries: once per context */
static int init_gl(void)
{
    char prefix[96], b[256];
    GLint maxUBO = 0, maxDraw = 0, maxAtt = 0, align = 0;
    GLuint vsFS, vsOut, fsFill, fsConv, fsOut;
    int kbytes, nk;
    size_t i;
    float* padded;
    size_t padTexels;

    if (s_glReady) return 1;
    if (s_glFailed) return 0;
    s_glFailed = 1;                      /* until the end of this function */
    if (!resolve_gl()) return 0;
    read_options();
    if (!load_weights(s_opt.tiny ? "tiny" : "full")) return 0;

    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUBO);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDraw);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxAtt);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    if (maxAtt < maxDraw) maxDraw = maxAtt;
    if (align > 256) { _snprintf(b, sizeof b, "restoreglsl: UNIFORM_BUFFER_OFFSET_ALIGNMENT %d > 256", align); rlog(b); return 0; }
    kbytes = s_w.kmax * 64;
    nk = maxUBO / kbytes;
    if (nk > maxDraw) nk = maxDraw;
    nk = nk >= 8 ? 8 : nk >= 4 ? 4 : nk >= 2 ? 2 : nk >= 1 ? 1 : 0;
    if (!nk) { _snprintf(b, sizeof b, "restoreglsl: MAX_UNIFORM_BLOCK_SIZE %d < one k-block (%d)", maxUBO, kbytes); rlog(b); return 0; }
    if (s_opt.nk && s_opt.nk <= nk) nk = s_opt.nk;
    s_nk = nk; s_wmax = nk * s_w.kmax;

    _snprintf(prefix, sizeof prefix, "#version 330 core\n#define NK %d\n#define WMAX %d\n", s_nk, s_wmax);
    vsFS   = mksh(GL_VERTEX_SHADER, prefix, TAGPU_RESTORE_FS_VS, "fs.vert");
    fsFill = mksh(GL_FRAGMENT_SHADER, prefix, TAGPU_RESTORE_FILL_FS, "fill.frag");
    fsConv = mksh(GL_FRAGMENT_SHADER, prefix, TAGPU_RESTORE_CONV_FS, "conv.frag");
    vsOut  = mksh(GL_VERTEX_SHADER, prefix, TAGPU_RESTORE_OUT_VS, "out.vert");
    fsOut  = mksh(GL_FRAGMENT_SHADER, prefix, TAGPU_RESTORE_OUT_FS, "out.frag");
    s_progFill = mkprog(vsFS, fsFill, "fill");
    s_progConv = mkprog(vsFS, fsConv, "conv");
    s_progOut  = mkprog(vsOut, fsOut, "out");
    if (vsFS) glDeleteShader(vsFS);
    if (fsFill) glDeleteShader(fsFill);
    if (fsConv) glDeleteShader(fsConv);
    if (vsOut) glDeleteShader(vsOut);
    if (fsOut) glDeleteShader(fsOut);
    if (!s_progFill || !s_progConv || !s_progOut) { free_gl(); return 0; }
    {
        static const char* fillN[7] = { "uAtlas", "uPal", "uRect", "uSrc", "uKey", "uSlot", "uKeyR" };
        static const char* convN[6] = { "uAct", "uRect", "uJin", "uKStride", "uSlot", "uRelu" };
        static const char* outN[7]  = { "uAct", "uAtlas", "uPal", "uRect", "uKey", "uSlot", "uDst" };
        for (i = 0; i < 7; i++) s_uFill[i] = glGetUniformLocation(s_progFill, fillN[i]);
        for (i = 0; i < 6; i++) s_uConv[i] = glGetUniformLocation(s_progConv, convN[i]);
        for (i = 0; i < 7; i++) s_uOut[i]  = glGetUniformLocation(s_progOut, outN[i]);
    }
    /* sampler units, fixed: 0 atlas, 1 palette, 2 rect, 3 src, 4 activations, 5 key */
    glUseProgram(s_progFill);
    glUniform1i(s_uFill[0], 0); glUniform1i(s_uFill[1], 1); glUniform1i(s_uFill[2], 2); glUniform1i(s_uFill[3], 3);
    glUniform1i(s_uFill[4], 5); glUniform1i(s_uFill[6], s_w.depth);
    glUseProgram(s_progConv);
    glUniform1i(s_uConv[0], 4); glUniform1i(s_uConv[1], 2);
    x_glUniformBlockBinding(s_progConv, x_glGetUniformBlockIndex(s_progConv, "WBlock"), 0);
    glUseProgram(s_progOut);
    glUniform1i(s_uOut[0], 4); glUniform1i(s_uOut[1], 0); glUniform1i(s_uOut[2], 1); glUniform1i(s_uOut[3], 2);
    glUniform1i(s_uOut[4], 5);
    glUseProgram(0);

    /* the weights: bound WMAX mat4s at a time, so the tail is padded */
    padTexels = (size_t)s_w.ntex + (size_t)s_wmax * 4;
    padded = (float*)calloc(padTexels * 4, sizeof(float));
    if (!padded) { rlog("restoreglsl: out of memory"); free_gl(); return 0; }
    memcpy(padded, s_w.body, (size_t)s_w.ntex * 16);
    glGenBuffers(1, &s_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(padTexels * 16), padded, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    free(padded);

    slot_tex(&s_rectTex); slot_tex(&s_srcTex); slot_tex(&s_keyTex);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenVertexArrays(1, &s_vao);
    glGenVertexArrays(1, &s_outVAO); glBindVertexArray(s_outVAO);
    glGenBuffers(1, &s_outVBO); glBindBuffer(GL_ARRAY_BUFFER, s_outVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof s_outVerts, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (void*)8);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
    glBindVertexArray(0);
    glGenFramebuffers(1, &s_fbo);
    glGenFramebuffers(1, &s_destFBO);
    /* GL_TIME_ELAPSED is ARB_timer_query, core only in 3.3: on this 3.2
       context it is an extension, and a glBeginQuery on an unsupported target
       would raise INVALID_ENUM and never signal -- so ask first */
    if (x_glGenQueries && x_glGetQueryObjectui64v && x_glBeginQuery && x_glEndQuery && has_timer_query())
        x_glGenQueries(2, s_query);
    s_qHave[0] = s_qHave[1] = 0; s_qFrame = 0; s_nsPerUnit = 0.0; s_qStall = 0;
    s_qJob[0] = s_qJob[1] = NULL;
    s_actSide = 0; s_attached = 0; s_slice = 0; s_idle = 0;
    _snprintf(b, sizeof b, "restoreglsl: %dx%d %s, NK=%d (uniform block %d KB of %d, %d draw buffers), %s, budget %.1f ms/frame",
              s_w.depth, s_w.ch, s_opt.fp16 ? "fp16" : "fp32", s_nk, s_wmax * 64 >> 10, maxUBO >> 10, maxDraw,
              s_query[0] ? "GL_TIME_ELAPSED budget" : "no timer query: fixed slices", s_opt.budget);
    rlog(b);
    s_glReady = 1; s_glFailed = 0;
    return 1;
}

/* the ping-pong arrays, grown to `side` (never past ACT_MAX), between batches only */
static int ensure_act(int side)
{
    int l;
    GLenum fmt = s_opt.fp16 ? GL_RGBA16F : GL_RGBA32F;
    GLenum st;
    char b[128];
    if (s_actFailed) return 0;
    if (s_act[0] && s_actSide >= side) return 1;
    free_act();
    s_actLayers = s_w.ch / 4;
    glGenTextures(2, s_act);
    for (l = 0; l < 2; l++) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, s_act[l]);
        x_glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, (GLint)fmt, side, side, s_actLayers, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    s_actSide = side;
    /* the float target, checked once per allocation: the contingency of renderers.md 4c */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    x_glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_act[0], 0, 0);
    { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
    s_attached = 1;
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        _snprintf(b, sizeof b, "restoreglsl: %s array layer is not a complete render target (0x%x)", s_opt.fp16 ? "RGBA16F" : "RGBA32F", st);
        rlog(b);
        s_actFailed = 1;
        free_act();
        return 0;
    }
    if (s_opt.log) {
        _snprintf(b, sizeof b, "restoreglsl: activations %dx%d x %d layers (%d MB)", side, side, s_actLayers,
                  (int)(2u * (unsigned)side * (unsigned)side * (unsigned)s_actLayers * (s_opt.fp16 ? 8u : 16u) >> 20));
        rlog(b);
    }
    return 1;
}

static void attach(GLuint tex, int first, int count)
{
    GLenum bufs[MAX_NK];
    int n;
    for (n = 0; n < count; n++) {
        x_glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + n, tex, 0, first + n);
        bufs[n] = GL_COLOR_ATTACHMENT0 + n;
    }
    for (n = count; n < s_attached; n++) {
        x_glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + n, 0, 0, 0);
        bufs[n] = GL_NONE;
    }
    if (count > s_attached) s_attached = count;
    glDrawBuffers(s_attached, bufs);
    s_attached = count;
}

/* the destination to zeros: unpainted everywhere, and a dump diffs deterministically */
static int clear_dest(struct TAGPU_RGLSL_JOB* j)
{
    GLint fbo0 = 0;
    GLboolean scissor, cmask[4];
    GLfloat cc[4] = { 0.f, 0.f, 0.f, 0.f };
    int ok = 1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_destFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j->destTex, 0);
    { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        char b[128];
        _snprintf(b, sizeof b, "restoreglsl: %s: the destination atlas is not a complete render target", j->tag);
        rlog(b);
        ok = 0;
    } else {
        scissor = x_glIsEnabled(GL_SCISSOR_TEST);
        x_glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        x_glClearColor(0.f, 0.f, 0.f, 0.f);
        x_glDisable(GL_SCISSOR_TEST);
        x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT);
        x_glClearColor(cc[0], cc[1], cc[2], cc[3]);
        if (scissor) x_glEnable(GL_SCISSOR_TEST);
        x_glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo0);
    return ok;
}

static void job_drop_work(struct TAGPU_RGLSL_JOB* j)
{
    j->qn = 0; j->bn = 0; j->inflight = 0; j->pass = 0; j->group = 0; j->srcAct = 0;
    j->running = 0;
}

TAGPU_RGLSL_JOB* tagpu_rglsl_job_new(const char* tag, int prio, int oneshot,
                                     unsigned int atlasTex, int atlasW, int atlasH,
                                     const unsigned char* pal,
                                     unsigned int destTex, int destW, int destH)
{
    struct TAGPU_RGLSL_JOB* j = NULL;
    unsigned char palRGBA[256 * 4];
    int i;
    char b[160];
    if (!atlasTex || !destTex || !pal || atlasW <= 0 || atlasH <= 0 || destW <= 0 || destH <= 0) return NULL;
    for (i = 0; i < MAX_JOBS; i++) if (!s_jobs[i].used) { j = &s_jobs[i]; break; }
    if (!j) { rlog("restoreglsl: no free job slot"); return NULL; }
    if (!init_gl()) return NULL;
    memset(j, 0, sizeof *j);
    j->used = 1; j->prio = prio; j->oneshot = oneshot;
    strncpy(j->tag, tag ? tag : "job", sizeof j->tag - 1);
    j->atlasTex = atlasTex; j->atlasW = atlasW; j->atlasH = atlasH;
    j->destTex = destTex; j->destW = destW; j->destH = destH;
    /* the palette snapshot: R,G,B,pad -> RGBA8 */
    for (i = 0; i < 256; i++) { palRGBA[i * 4] = pal[i * 4]; palRGBA[i * 4 + 1] = pal[i * 4 + 1]; palRGBA[i * 4 + 2] = pal[i * 4 + 2]; palRGBA[i * 4 + 3] = 255; }
    glGenTextures(1, &j->palTex);
    glBindTexture(GL_TEXTURE_2D, j->palTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palRGBA);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!clear_dest(j)) { glDeleteTextures(1, &j->palTex); j->used = 0; return NULL; }
    j->lastTallySlice = s_slice;
    if (!oneshot) {
        _snprintf(b, sizeof b, "restoreglsl: %s: lazy restore armed (%dx%d twin of the %dx%d atlas)", j->tag, destW, destH, atlasW, atlasH);
        rlog(b);
    }
    return j;
}

int tagpu_rglsl_job_add(TAGPU_RGLSL_JOB* j, const TAGPU_RGLSL_FRAME* frames, int count)
{
    int i, added = 0;
    if (!j || !j->used || j->failed || !frames || count <= 0) return 0;
    if (j->qn + count > j->qcap) {
        int cap = j->qcap ? j->qcap : 256;
        QF* nq;
        while (cap < j->qn + count) cap *= 2;
        nq = (QF*)realloc(j->q, (size_t)cap * sizeof *nq);
        if (!nq) { rlog("restoreglsl: out of memory"); return 0; }
        j->q = nq; j->qcap = cap;
    }
    for (i = 0; i < count; i++) {
        QF* p = &j->q[j->qn];
        int S, c;
        if (frames[i].w <= 0 || frames[i].h <= 0) continue;
        p->f = frames[i];
        S = (p->f.w > p->f.h ? p->f.w : p->f.h) + (p->f.wrap ? 2 * s_w.depth : 0);
        if (S > ACT_MAX && p->f.wrap) { p->f.wrap = 0; S = p->f.w > p->f.h ? p->f.w : p->f.h; }
        if (S > ACT_MAX) {
            char b[128];
            _snprintf(b, sizeof b, "restoreglsl: %s: %dx%d frame exceeds the %d-texel slot, left indexed", j->tag, p->f.w, p->f.h, ACT_MAX);
            rlog(b);
            continue;
        }
        for (c = 0; c < NCLASSES && s_classes[c] < S; c++) ;
        p->S = (short)S; p->cls = (short)c;
        j->qn++; added++;
    }
    if (added && !j->running) {
        j->running = 1; j->rframes = 0; j->rwrap = 0; j->rbatches = 0; j->rdraws = 0; j->rslices = 0;
        j->rt0 = now_ms(); j->rgpuNs = 0.0; j->rgpuUnits = 0.0; j->runits = 0.0;
        j->rcall0 = s_calls;
        if (j->oneshot) {
            char b[160];
            int w = 0;
            for (i = 0; i < j->qn; i++) w += j->q[i].f.wrap ? 1 : 0;
            _snprintf(b, sizeof b, "restoreglsl: %s: job started: %d frames (%d wrap-padded), model %dx%d",
                      j->tag, j->qn, w, s_w.depth, s_w.ch);
            rlog(b);
        }
    }
    return added;
}

void tagpu_rglsl_job_clear(TAGPU_RGLSL_JOB* j)
{
    if (!j || !j->used) return;
    job_drop_work(j);
    if (!j->failed) clear_dest(j);
}

int tagpu_rglsl_job_idle(const TAGPU_RGLSL_JOB* j)
{
    return !j || !j->used || (j->qn == 0 && !j->inflight);
}

int tagpu_rglsl_job_failed(const TAGPU_RGLSL_JOB* j)
{
    return j && j->used && j->failed;
}

int tagpu_rglsl_job_painted(const TAGPU_RGLSL_JOB* j)
{
    return (j && j->used) ? j->tframes : 0;
}

void tagpu_rglsl_job_free(TAGPU_RGLSL_JOB* j)
{
    int i, any = 0;
    if (!j || !j->used) return;
    if (j->palTex) glDeleteTextures(1, &j->palTex);
    free(j->q);
    /* a query still in flight must not credit its time to whoever takes
       this slot next */
    for (i = 0; i < 2; i++) if (s_qJob[i] == j) s_qJob[i] = NULL;
    memset(j, 0, sizeof *j);
    for (i = 0; i < MAX_JOBS; i++) any |= s_jobs[i].used;
    /* the last job takes the scratch with it; the programs stay for the next map */
    if (!any) free_act();
}

/* take the next batch off the queue: the head's size class, up to the grid
   that class allows, in queue order; the rest close up behind */
static int form_batch(struct TAGPU_RGLSL_JOB* j)
{
    int cls, cols, cap, i, k = 0, S = 0;
    if (j->qn == 0) return 0;
    cls = j->q[0].cls;
    cols = ACT_MAX / s_classes[cls]; if (cols > SLOT_COLS) cols = SLOT_COLS;
    cap = cols * cols;
    j->bn = 0;
    for (i = 0; i < j->qn; i++) {
        if (j->q[i].cls == cls && j->bn < cap) {
            j->bf[j->bn++] = j->q[i];
            if (j->q[i].S > S) S = j->q[i].S;
        } else j->q[k++] = j->q[i];
    }
    j->qn = k;
    /* the grid is the smallest square that holds the batch: the passes cost
       by the fragment, and a queue's two-frame batch must not pay for 64 */
    for (cols = 1; cols * cols < j->bn; cols++) ;
    j->bS = S; j->bcols = cols;
    j->pass = 0; j->group = 0; j->srcAct = 0;
    if (!ensure_act(cols * S)) { j->failed = 1; job_drop_work(j); return 0; }
    j->inflight = 1;
    return 1;
}

/* one draw for the job's in-flight batch; returns its cost in work units
   (texels x input tiles x NK) */
static double issue_draw(struct TAGPU_RGLSL_JOB* j)
{
    int S = j->bS, cols = j->bcols, TW = cols * S, TH = cols * S;
    double units;

    if (j->pass == 0) {
        int s;
        memset(s_rect, 0, sizeof s_rect); memset(s_src, 0, sizeof s_src);
        for (s = 0; s < BATCH; s++) { s_key[s * 4] = -1.f; s_key[s * 4 + 1] = s_key[s * 4 + 2] = s_key[s * 4 + 3] = 0.f; }
        for (s = 0; s < j->bn; s++) {
            const TAGPU_RGLSL_FRAME* f = &j->bf[s].f;
            int p = f->wrap ? s_w.depth : 0;
            int t = (s / cols) * SLOT_COLS + (s % cols);       /* the 8x8 table's texel */
            s_rect[t * 4 + 2] = (float)(f->w + 2 * p); s_rect[t * 4 + 3] = (float)(f->h + 2 * p);
            s_src[t * 4] = (float)f->ax; s_src[t * 4 + 1] = (float)f->ay;
            s_src[t * 4 + 2] = (float)f->w; s_src[t * 4 + 3] = (float)f->h;
            s_key[t * 4] = (float)f->key;
        }
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, j->atlasTex);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, j->palTex);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_rectTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, GL_RGBA, GL_FLOAT, s_rect);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, s_srcTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, GL_RGBA, GL_FLOAT, s_src);
        glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, s_keyTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, GL_RGBA, GL_FLOAT, s_key);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, TW, TH);
        attach(s_act[0], 0, 1);
        glUseProgram(s_progFill);
        glUniform1i(s_uFill[5], S);
        glBindVertexArray(s_vao);
        x_glDrawArrays(GL_TRIANGLES, 0, 3);
        j->pass = 1; j->group = 0; j->srcAct = 0;
        units = (double)TW * TH;
    } else if (j->pass <= s_w.depth) {
        const Layer* L = &s_w.layer[j->pass - 1];
        int n = (int)L->kout - j->group; if (n > s_nk) n = s_nk;
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, TW, TH);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_rectTex);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, s_act[j->srcAct]);
        glUseProgram(s_progConv);
        glUniform1i(s_uConv[2], (int)L->jin);
        glUniform1i(s_uConv[3], (int)(L->kstride / 4));
        glUniform1i(s_uConv[4], S);
        glUniform1i(s_uConv[5], j->pass < s_w.depth ? 1 : 0);
        attach(s_act[1 - j->srcAct], j->group, n);
        x_glBindBufferRange(GL_UNIFORM_BUFFER, 0, s_ubo,
                            (GLintptr)((size_t)(L->offset + (unsigned)j->group * L->kstride) * 16),
                            (GLsizeiptr)((size_t)s_wmax * 64));
        glBindVertexArray(s_vao);
        x_glDrawArrays(GL_TRIANGLES, 0, 3);
        units = (double)TW * TH * L->jin * s_nk;
        j->group += s_nk;
        if (j->group >= (int)L->kout) { j->group = 0; j->srcAct = 1 - j->srcAct; j->pass++; }
    } else {
        int s, nv = 0;
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, j->atlasTex);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, j->palTex);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_rectTex);
        glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, s_keyTex);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, s_act[j->srcAct]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        attach(0, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_destFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, j->destTex, 0);
        { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
        glViewport(0, 0, j->destW, j->destH);
        for (s = 0; s < j->bn; s++) {
            const TAGPU_RGLSL_FRAME* f = &j->bf[s].f;
            float x0 = (float)(f->dx - f->border), y0 = (float)(f->dy - f->border);
            float x1 = (float)(f->dx + f->w + f->border + f->padR), y1 = (float)(f->dy + f->h + f->border + f->padB);
            float sc = (float)(s % cols), sr = (float)(s / cols);
            float xs[6] = { x0, x1, x0, x1, x1, x0 }, ys[6] = { y0, y0, y1, y0, y1, y1 };
            int k;
            for (k = 0; k < 6; k++) {
                float* o = s_outVerts + (size_t)nv * 8;
                o[0] = xs[k]; o[1] = ys[k]; o[2] = (float)f->dx; o[3] = (float)f->dy;
                o[4] = sc; o[5] = sr; o[6] = (float)f->w; o[7] = (float)f->h;
                nv++;
            }
        }
        glUseProgram(s_progOut);
        glUniform1i(s_uOut[5], S);
        x_glUniform2f(s_uOut[6], (float)j->destW, (float)j->destH);
        glBindVertexArray(s_outVAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_outVBO);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nv * 32), s_outVerts, GL_STREAM_DRAW);
        x_glDrawArrays(GL_TRIANGLES, 0, nv);
        units = (double)j->bn * (S + 2) * (S + 2) * s_w.layer[s_w.depth - 1].jin;
        j->rbatches++; j->tbatches++;
        for (s = 0; s < j->bn; s++) { j->rframes++; j->tframes++; if (j->bf[s].f.wrap) j->rwrap++; }
        if (s_opt.log) {
            char b[160];
            _snprintf(b, sizeof b, "restoreglsl: %s: batch %d (S%d, %dx%d slots, %d frames) issued at slice %u, %d queued",
                      j->tag, j->rbatches, S, cols, cols, j->bn, s_slice, j->qn);
            rlog(b);
        }
        j->inflight = 0; j->bn = 0; j->pass = 0;
    }
    j->rdraws++; j->tdraws++;
    return units;
}

/* the job whose batch is in flight, else the lowest prio with a queue */
static struct TAGPU_RGLSL_JOB* pick_job(void)
{
    struct TAGPU_RGLSL_JOB* best = NULL;
    int i;
    for (i = 0; i < MAX_JOBS; i++) if (s_jobs[i].used && !s_jobs[i].failed && s_jobs[i].inflight) return &s_jobs[i];
    for (i = 0; i < MAX_JOBS; i++) {
        struct TAGPU_RGLSL_JOB* j = &s_jobs[i];
        if (!j->used || j->failed || j->qn == 0) continue;
        if (!best || j->prio < best->prio) best = j;
    }
    return best;
}

/* a run drained: the terrain's "done" line, or a queue's tally */
static void job_drained(struct TAGPU_RGLSL_JOB* j)
{
    char b[360];
    double wall = now_ms() - j->rt0;
    unsigned frames = s_calls - j->rcall0 + 1;     /* this frame included */
    j->running = 0;
    if (j->oneshot) {
        /* fps = frames the game drew while the run lasted (every step call,
           drawn in or not) over the wall; the GPU figure is the slices this
           job started, so its share says how much of the work it timed */
        _snprintf(b, sizeof b, "restoreglsl: %s: done: %d frames (%d wrap-padded) in %d batches, %d draws in %d of %u frames = %.0f ms wall since begin (%.1f fps while restoring); GPU %.0f ms measured over %.0f%% of the work; %dx%d %s NK=%d budget %.0f ms",
                  j->tag, j->rframes, j->rwrap, j->rbatches, j->rdraws, j->rslices, frames, wall,
                  wall > 0.0 ? 1000.0 * frames / wall : 0.0, j->rgpuNs / 1e6,
                  j->runits > 0.0 ? 100.0 * j->rgpuUnits / j->runits : 0.0,
                  s_w.depth, s_w.ch, s_opt.fp16 ? "fp16" : "fp32", s_nk, s_opt.budget);
        rlog(b);
    } else if (s_opt.log || s_slice - j->lastTallySlice >= QUEUE_LOG_FRAMES) {
        j->lastTallySlice = s_slice;
        _snprintf(b, sizeof b, "restoreglsl: %s: queue drained: %d frames in %d batches this run, %u frames from the first queued to the last painted (%.0f ms, %d slices drawn in); %d frames, %d batches, %d draws so far",
                  j->tag, j->rframes, j->rbatches, frames, wall, j->rslices, j->tframes, j->tbatches, j->tdraws);
        rlog(b);
    }
}

void tagpu_rglsl_step(void)
{
    GLint fbo = 0, vp[4] = { 0, 0, 0, 0 };
    GLboolean blend, depth, scissor, cull, dmask, cmask[4];
    double allowed, spent = 0.0;
    int q, ndraw = 0, i;
    struct TAGPU_RGLSL_JOB* j;
    char b[300];

    s_calls++;
    if (!s_glReady) return;
    /* last slice's GPU time -> the cost estimate; the query before that is
       read while the newest still runs, so nothing here ever waits */
    if (s_query[0]) {
        q = (s_qFrame + 1) & 1;
        if (s_qHave[q]) {
            GLuint64 avail = 0, ns = 0;
            x_glGetQueryObjectui64v(s_query[q], GL_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) {
                s_qStall = 0;
                x_glGetQueryObjectui64v(s_query[q], GL_QUERY_RESULT, &ns);
                s_qHave[q] = 0;
                if (s_qUnits[q] > 0.0) {
                    double est = (double)ns / s_qUnits[q];
                    s_nsPerUnit = s_nsPerUnit > 0.0 ? 0.5 * (s_nsPerUnit + est) : est;
                    if (s_qJob[q] && s_qJob[q]->used) {
                        s_qJob[q]->rgpuNs += (double)ns; s_qJob[q]->rgpuUnits += s_qUnits[q];
                        s_qJob[q]->tgpuNs += (double)ns;
                    }
                }
            } else if (++s_qStall < 300) {
                return;                        /* the GPU is a frame behind: add nothing */
            } else {
                /* a result that never comes is a driver that does not really
                   time queries: fixed slices from here rather than wait forever */
                rlog("restoreglsl: timer query never completed; fixed slices from here");
                x_glDeleteQueries(2, s_query); s_query[0] = s_query[1] = 0; s_qHave[0] = s_qHave[1] = 0;
            }
        }
    }
    if (!tagpu_classicpp_on()) return;           /* paused: the queues keep filling */
    j = pick_job();
    if (!j) {
        /* nothing to do: after a while the scratch goes (the queues stay) */
        if (s_act[0] && ++s_idle >= IDLE_FRAMES) {
            for (i = 0; i < MAX_JOBS; i++) if (s_jobs[i].used && s_jobs[i].inflight) return;
            free_act();
            if (s_opt.log) rlog("restoreglsl: idle: activations freed");
        }
        return;
    }
    s_idle = 0;
    allowed = (s_query[0] && s_nsPerUnit > 0.0) ? s_opt.budget * 1e6 / s_nsPerUnit : 0.0;

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    blend = x_glIsEnabled(GL_BLEND); depth = x_glIsEnabled(GL_DEPTH_TEST);
    scissor = x_glIsEnabled(GL_SCISSOR_TEST); cull = x_glIsEnabled(GL_CULL_FACE);
    x_glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask); x_glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    x_glDisable(GL_BLEND); x_glDisable(GL_DEPTH_TEST); x_glDisable(GL_SCISSOR_TEST); x_glDisable(GL_CULL_FACE);
    x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (s_slice < 2) {
        /* the error flag is process-wide and nobody drains it per frame: an
           error reported after a slice is ours only if none was pending before */
        int pending = 0;
        while (glGetError() != GL_NO_ERROR && pending < 16) pending++;
        if (pending) { _snprintf(b, sizeof b, "restoreglsl: %d GL error(s) were pending before slice %u (not ours)", pending, s_slice + 1); rlog(b); }
    }
    q = s_qFrame & 1;
    if (s_query[0]) { x_glBeginQuery(GL_TIME_ELAPSED, s_query[q]); s_qJob[q] = j; }
    do {
        /* every batch boundary re-picks, so a higher-priority job that
           gained work takes the budget as soon as the batch in flight lands */
        if (!j->inflight) {
            j = pick_job();
            if (!j) break;
            if (!j->inflight && !form_batch(j)) continue;      /* it just failed: another */
        }
        if (j->sliceMark != s_slice + 1) { j->sliceMark = s_slice + 1; j->rslices++; }
        {
            double u = issue_draw(j);
            spent += u; j->runits += u;
        }
        ndraw++;
        if (!j->inflight && j->qn == 0 && j->running) job_drained(j);
        if (s_query[0]) { if (s_nsPerUnit <= 0.0 && ndraw >= 2) break; if (spent >= allowed) break; }
        else if (ndraw >= FIXED_DRAWS) break;
    } while (1);
    if (s_query[0]) { x_glEndQuery(GL_TIME_ELAPSED); s_qUnits[q] = spent; s_qHave[q] = 1; s_qFrame++; }
    s_slice++;
    if (s_slice <= 2) {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) { _snprintf(b, sizeof b, "restoreglsl: GL error 0x%x after slice %u", e, s_slice); rlog(b); }
    }

    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (blend) x_glEnable(GL_BLEND);
    if (depth) x_glEnable(GL_DEPTH_TEST);
    if (scissor) x_glEnable(GL_SCISSOR_TEST);
    if (cull) x_glEnable(GL_CULL_FACE);
    x_glDepthMask(dmask); x_glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
}

void tagpu_rglsl_glreset(void)
{
    int i;
    /* every id died with the context: forget them without deleting */
    s_progFill = s_progConv = s_progOut = 0;
    s_ubo = s_outVBO = s_rectTex = s_srcTex = s_keyTex = s_fbo = s_destFBO = s_vao = s_outVAO = 0;
    s_act[0] = s_act[1] = 0; s_query[0] = s_query[1] = 0; s_actSide = 0;
    s_qHave[0] = s_qHave[1] = 0; s_qJob[0] = s_qJob[1] = NULL;
    s_glReady = 0; s_glFailed = 0; s_actFailed = 0;
    for (i = 0; i < MAX_JOBS; i++) {
        free(s_jobs[i].q);
        memset(&s_jobs[i], 0, sizeof s_jobs[i]);
    }
}
