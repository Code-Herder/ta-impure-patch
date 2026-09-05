/* tagpu_restoreglsl.c — the Classic++ restorer as fragment passes, in the
   game's own GL context, sliced across frames. research/notes/renderers.md 4c
   has the decisions; tagpu_restore_glsl.h has the shaders and the layout they
   assume; tools/tascene-restore.js is this same driver in the browser lab,
   where the shaders were proven against the unditherer (max 1 level on
   0.0012 % of bytes, fp32).

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

   THE PADDING RULE, the thing that defines the pixels. tagpu_restore.c fed
   ONNX a tile whose opposite edges agree within 12 levels wrap-padded by 12 to
   56x56 and centre-cropped, and every other tile at 32x32 with zero padding
   at every layer. Here both are one mechanism: every slot has a valid rect and
   a tap outside it reads 0, at every layer (the rect test in the conv shader)
   -- because zero-padding only the INPUT and running unmasked is a different
   network: layer 2 would read layer 1's gutter, which is relu(bias), not 0.
   The FILL pass wrap-pads inside the rect; the OUT pass centre-crops.

   SLICING. tagpu_terr.c calls tagpu_rglsl_step() once per frame from its
   gather, before anything renders. Each call issues draws until an estimate
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
   in the terrain pass's own 34-pitch cell layout, replicated guard texel
   included, so there is no CPU copy of the result and no upload; the source
   is the R8 atlas the terrain pass already built. The tile bytes never leave
   the GPU. Nothing is written to disk by this module (renderers.md 2.5b). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_restore_glsl.h"
#include "tagpu_restoreglsl.h"

#define SLOT_COLS    8
#define SLOT_ROWS    8
#define BATCH        (SLOT_COLS * SLOT_ROWS)       /* frames per batch: 64      */
#define MAX_NK       8
#define MAX_LAYERS   32
#define TILEABLE_THR 12.0
#define OPT_FILE     "tagpu_restoreglsl.on"
#define DEFAULT_BUDGET_MS 12.0
#define FIXED_DRAWS  6                             /* no timer: draws per frame */

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
    if (!f) { _snprintf(b, sizeof b, "restoreglsl: no %s -- Classic++ terrain stays indexed", path); rlog(b); return 0; }
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

/* ---- options (OPT_FILE, read once per job) ---- */
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

/* ---- the job ---- */
typedef struct { int S, first, count; } Batch;        /* frames [first, first+count) */

static TAGPU_RGLSL_FRAME* s_frames;
static int    s_nframes, s_nwrap;
static Batch* s_batches;
static int    s_nbatches;
static int    s_state;                 /* 0 idle, 1 running, 2 done, -1 failed */
static GLuint s_atlasTex, s_destTex;
static int    s_atlasW, s_atlasH, s_destW, s_destH;

static GLuint s_progFill, s_progConv, s_progOut;
static GLint  s_uFill[5], s_uConv[6], s_uOut[6];
static GLuint s_ubo, s_rectTex, s_srcTex, s_palTex, s_act[2], s_vao, s_outVAO, s_outVBO, s_fbo, s_destFBO;
static int    s_nk, s_wmax, s_actSide, s_actLayers, s_attached;
static float  s_rect[BATCH * 4], s_src[BATCH * 4];
static float  s_outVerts[BATCH * 6 * 8];

/* progress */
static int    s_cur, s_pass, s_group, s_srcAct;
static int    s_draws, s_frameCount;
static double s_t0;
/* the budget */
static GLuint s_query[2];
static int    s_qHave[2], s_qFrame;
static double s_qUnits[2];
static double s_nsPerUnit;             /* 0 = unknown yet                     */
static int    s_qStall;                /* frames waited on one query result   */
static double s_gpuNs, s_gpuUnits, s_totalUnits;

int tagpu_rglsl_tileable(const unsigned char* px, int w, int h, const unsigned char* pal)
{
    double lr = 0.0, tb = 0.0;
    int i, c;
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

static void free_gl(void)
{
    if (s_progFill) glDeleteProgram(s_progFill);
    if (s_progConv) glDeleteProgram(s_progConv);
    if (s_progOut)  glDeleteProgram(s_progOut);
    if (s_ubo) glDeleteBuffers(1, &s_ubo);
    if (s_outVBO) glDeleteBuffers(1, &s_outVBO);
    if (s_rectTex) glDeleteTextures(1, &s_rectTex);
    if (s_srcTex) glDeleteTextures(1, &s_srcTex);
    if (s_palTex) glDeleteTextures(1, &s_palTex);
    if (s_act[0]) glDeleteTextures(2, s_act);
    if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
    if (s_destFBO) glDeleteFramebuffers(1, &s_destFBO);
    if (s_vao) glDeleteVertexArrays(1, &s_vao);
    if (s_outVAO) glDeleteVertexArrays(1, &s_outVAO);
    if (s_query[0] && x_glDeleteQueries) x_glDeleteQueries(2, s_query);
    s_progFill = s_progConv = s_progOut = 0;
    s_ubo = s_outVBO = s_rectTex = s_srcTex = s_palTex = s_fbo = s_destFBO = s_vao = s_outVAO = 0;
    s_act[0] = s_act[1] = 0; s_query[0] = s_query[1] = 0;
    s_actSide = 0;
}

static void free_job(void)
{
    free(s_frames); s_frames = NULL; s_nframes = 0;
    free(s_batches); s_batches = NULL; s_nbatches = 0;
}

/* programs, tables, weights, queries: once per job (the model or NK may change) */
static int init_gl(const unsigned char* pal)
{
    char prefix[96], b[256];
    GLint maxUBO = 0, maxDraw = 0, maxAtt = 0, align = 0;
    GLuint vsFS, vsOut, fsFill, fsConv, fsOut;
    int kbytes, nk;
    size_t i;
    unsigned char palRGBA[256 * 4];
    float* padded;
    size_t padTexels;

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
    if (!s_progFill || !s_progConv || !s_progOut) return 0;
    {
        static const char* fillN[5] = { "uAtlas", "uPal", "uRect", "uSrc", "uSlot" };
        static const char* convN[6] = { "uAct", "uRect", "uJin", "uKStride", "uSlot", "uRelu" };
        static const char* outN[6]  = { "uAct", "uAtlas", "uPal", "uRect", "uSlot", "uDst" };
        for (i = 0; i < 5; i++) s_uFill[i] = glGetUniformLocation(s_progFill, fillN[i]);
        for (i = 0; i < 6; i++) s_uConv[i] = glGetUniformLocation(s_progConv, convN[i]);
        for (i = 0; i < 6; i++) s_uOut[i]  = glGetUniformLocation(s_progOut, outN[i]);
    }
    /* sampler units, fixed: 0 atlas, 1 palette, 2 rect, 3 src, 4 activations */
    glUseProgram(s_progFill);
    glUniform1i(s_uFill[0], 0); glUniform1i(s_uFill[1], 1); glUniform1i(s_uFill[2], 2); glUniform1i(s_uFill[3], 3);
    glUseProgram(s_progConv);
    glUniform1i(s_uConv[0], 4); glUniform1i(s_uConv[1], 2);
    x_glUniformBlockBinding(s_progConv, x_glGetUniformBlockIndex(s_progConv, "WBlock"), 0);
    glUseProgram(s_progOut);
    glUniform1i(s_uOut[0], 4); glUniform1i(s_uOut[1], 0); glUniform1i(s_uOut[2], 1); glUniform1i(s_uOut[3], 2);
    glUseProgram(0);

    /* the weights: bound WMAX mat4s at a time, so the tail is padded */
    padTexels = (size_t)s_w.ntex + (size_t)s_wmax * 4;
    padded = (float*)calloc(padTexels * 4, sizeof(float));
    if (!padded) { rlog("restoreglsl: out of memory"); return 0; }
    memcpy(padded, s_w.body, (size_t)s_w.ntex * 16);
    glGenBuffers(1, &s_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(padTexels * 16), padded, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    free(padded);

    slot_tex(&s_rectTex); slot_tex(&s_srcTex);
    /* the palette snapshot: R,G,B,pad -> RGBA8 */
    for (i = 0; i < 256; i++) { palRGBA[i * 4] = pal[i * 4]; palRGBA[i * 4 + 1] = pal[i * 4 + 1]; palRGBA[i * 4 + 2] = pal[i * 4 + 2]; palRGBA[i * 4 + 3] = 255; }
    glGenTextures(1, &s_palTex);
    glBindTexture(GL_TEXTURE_2D, s_palTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palRGBA);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
    _snprintf(b, sizeof b, "restoreglsl: %dx%d %s, NK=%d (uniform block %d KB of %d, %d draw buffers), %s, budget %.1f ms/frame",
              s_w.depth, s_w.ch, s_opt.fp16 ? "fp16" : "fp32", s_nk, s_wmax * 64 >> 10, maxUBO >> 10, maxDraw,
              s_query[0] ? "GL_TIME_ELAPSED budget" : "no timer query: fixed slices", s_opt.budget);
    rlog(b);
    return 1;
}

static int ensure_act(int side)
{
    int l;
    GLenum fmt = s_opt.fp16 ? GL_RGBA16F : GL_RGBA32F;
    GLenum st;
    if (s_act[0] && s_actSide >= side) return 1;
    if (s_act[0]) glDeleteTextures(2, s_act);
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
    /* the float target, checked once: the contingency of renderers.md 4c */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    x_glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_act[0], 0, 0);
    { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
    s_attached = 1;
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char b[128];
        _snprintf(b, sizeof b, "restoreglsl: %s array layer is not a complete render target (0x%x)", s_opt.fp16 ? "RGBA16F" : "RGBA32F", st);
        rlog(b);
        return 0;
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

int tagpu_rglsl_begin(const TAGPU_RGLSL_FRAME* frames, int count,
                      unsigned int atlasTex, int atlasW, int atlasH,
                      const unsigned char* pal,
                      unsigned int destTex, int destW, int destH)
{
    char b[256];
    int i, maxS = 0, pending[2][BATCH], npend[2] = { 0, 0 }, sizes[2] = { 0, 0 }, nclass = 0;
    int pad;

    GLint fbo0 = 0;

    tagpu_rglsl_abort();
    if (!frames || count <= 0 || !atlasTex || !destTex || !pal) return 0;
    if (!resolve_gl()) { s_state = -1; return 0; }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo0);
    read_options();
    if (!load_weights(s_opt.tiny ? "tiny" : "full")) { s_state = -1; return 0; }
    if (!init_gl(pal)) { free_gl(); s_state = -1; return 0; }
    pad = s_w.depth;

    s_frames = (TAGPU_RGLSL_FRAME*)malloc((size_t)count * sizeof *s_frames);
    s_batches = (Batch*)malloc(((size_t)count / BATCH + 4) * sizeof *s_batches);
    if (!s_frames || !s_batches) { rlog("restoreglsl: out of memory"); free_job(); free_gl(); s_state = -1; return 0; }
    /* Batches keep the caller's order (visibility first) as far as the two
       padded-size classes allow: a class's batch is emitted when it fills. */
    s_nframes = 0; s_nbatches = 0; s_nwrap = 0;
    for (i = 0; i < count; i++) {
        const TAGPU_RGLSL_FRAME* f = &frames[i];
        int p = f->wrap ? pad : 0, pw = f->w + 2 * p, ph = f->h + 2 * p, S = pw > ph ? pw : ph, c;
        if (f->w <= 0 || f->h <= 0) continue;
        for (c = 0; c < nclass && sizes[c] != S; c++) ;
        if (c == nclass) {
            if (nclass == 2) { rlog("restoreglsl: more than two padded sizes in one job"); free_job(); free_gl(); s_state = -1; return 0; }
            sizes[nclass++] = S;
        }
        pending[c][npend[c]++] = i;
        if (npend[c] == BATCH) {
            int k;
            s_batches[s_nbatches].S = S; s_batches[s_nbatches].first = s_nframes; s_batches[s_nbatches].count = BATCH;
            for (k = 0; k < BATCH; k++) s_frames[s_nframes++] = frames[pending[c][k]];
            s_nbatches++; npend[c] = 0;
        }
        if (S > maxS) maxS = S;
        if (f->wrap) s_nwrap++;
    }
    for (i = 0; i < nclass; i++) if (npend[i]) {
        int k;
        s_batches[s_nbatches].S = sizes[i]; s_batches[s_nbatches].first = s_nframes; s_batches[s_nbatches].count = npend[i];
        for (k = 0; k < npend[i]; k++) s_frames[s_nframes++] = frames[pending[i][k]];
        s_nbatches++;
    }
    if (!s_nbatches) { free_job(); free_gl(); return 0; }
    s_atlasTex = atlasTex; s_atlasW = atlasW; s_atlasH = atlasH;
    s_destTex = destTex; s_destW = destW; s_destH = destH;
    if (!ensure_act(SLOT_COLS * maxS)) { glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo0); free_job(); free_gl(); s_state = -1; return 0; }
    /* the destination starts as zeros, so a dump diffs deterministically */
    glBindFramebuffer(GL_FRAMEBUFFER, s_destFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_destTex, 0);
    { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        rlog("restoreglsl: the destination atlas is not a complete render target");
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo0); free_job(); free_gl(); s_state = -1; return 0;
    }
    {
        GLboolean scissor = x_glIsEnabled(GL_SCISSOR_TEST), cmask[4];
        GLfloat cc[4] = { 0.f, 0.f, 0.f, 0.f };
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

    s_cur = 0; s_pass = 0; s_group = 0; s_srcAct = 0;
    s_draws = 0; s_frameCount = 0; s_gpuNs = 0.0; s_gpuUnits = 0.0; s_totalUnits = 0.0;
    s_t0 = now_ms();
    s_state = 1;
    _snprintf(b, sizeof b, "restoreglsl: job started: %d frames (%d wrap-padded) in %d batches, slots %d/%d px, activations %d MB",
              s_nframes, s_nwrap, s_nbatches, sizes[0], nclass > 1 ? sizes[1] : 0,
              (int)(2u * (unsigned)s_actSide * (unsigned)s_actSide * (unsigned)s_actLayers * (s_opt.fp16 ? 8u : 16u) >> 20));
    rlog(b);
    return 1;
}

/* one draw; returns its cost in work units (texels x input tiles x NK) */
static double issue_draw(void)
{
    const Batch* bt = &s_batches[s_cur];
    int S = bt->S, TW = SLOT_COLS * S, TH = SLOT_ROWS * S;
    double units;

    if (s_pass == 0) {
        int s;
        memset(s_rect, 0, sizeof s_rect); memset(s_src, 0, sizeof s_src);
        for (s = 0; s < bt->count; s++) {
            const TAGPU_RGLSL_FRAME* f = &s_frames[bt->first + s];
            int p = f->wrap ? s_w.depth : 0;
            s_rect[s * 4 + 2] = (float)(f->w + 2 * p); s_rect[s * 4 + 3] = (float)(f->h + 2 * p);
            s_src[s * 4] = (float)f->ax; s_src[s * 4 + 1] = (float)f->ay;
            s_src[s * 4 + 2] = (float)f->w; s_src[s * 4 + 3] = (float)f->h;
        }
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_rectTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, GL_RGBA, GL_FLOAT, s_rect);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, s_srcTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, GL_RGBA, GL_FLOAT, s_src);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, TW, TH);
        attach(s_act[0], 0, 1);
        glUseProgram(s_progFill);
        glUniform1i(s_uFill[4], S);
        glBindVertexArray(s_vao);
        x_glDrawArrays(GL_TRIANGLES, 0, 3);
        s_pass = 1; s_group = 0; s_srcAct = 0;
        units = (double)TW * TH;
    } else if (s_pass <= s_w.depth) {
        const Layer* L = &s_w.layer[s_pass - 1];
        int n = (int)L->kout - s_group; if (n > s_nk) n = s_nk;
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, TW, TH);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, s_act[s_srcAct]);
        glUseProgram(s_progConv);
        glUniform1i(s_uConv[2], (int)L->jin);
        glUniform1i(s_uConv[3], (int)(L->kstride / 4));
        glUniform1i(s_uConv[4], S);
        glUniform1i(s_uConv[5], s_pass < s_w.depth ? 1 : 0);
        attach(s_act[1 - s_srcAct], s_group, n);
        x_glBindBufferRange(GL_UNIFORM_BUFFER, 0, s_ubo,
                            (GLintptr)((size_t)(L->offset + (unsigned)s_group * L->kstride) * 16),
                            (GLsizeiptr)((size_t)s_wmax * 64));
        glBindVertexArray(s_vao);
        x_glDrawArrays(GL_TRIANGLES, 0, 3);
        units = (double)TW * TH * L->jin * s_nk;
        s_group += s_nk;
        if (s_group >= (int)L->kout) { s_group = 0; s_srcAct = 1 - s_srcAct; s_pass++; }
    } else {
        int s, nv = 0;
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, s_act[s_srcAct]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        attach(0, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_destFBO);
        glViewport(0, 0, s_destW, s_destH);
        for (s = 0; s < bt->count; s++) {
            const TAGPU_RGLSL_FRAME* f = &s_frames[bt->first + s];
            float x0 = (float)(f->dx - f->border), y0 = (float)(f->dy - f->border);
            float x1 = (float)(f->dx + f->w + f->border), y1 = (float)(f->dy + f->h + f->border);
            float sc = (float)(s % SLOT_COLS), sr = (float)(s / SLOT_COLS);
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
        glUniform1i(s_uOut[4], S);
        x_glUniform2f(s_uOut[5], (float)s_destW, (float)s_destH);
        glBindVertexArray(s_outVAO);
        glBindBuffer(GL_ARRAY_BUFFER, s_outVBO);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nv * 32), s_outVerts, GL_STREAM_DRAW);
        x_glDrawArrays(GL_TRIANGLES, 0, nv);
        units = (double)bt->count * (S + 2) * (S + 2) * s_w.layer[s_w.depth - 1].jin;
        if (s_opt.log) {
            char b[128];
            _snprintf(b, sizeof b, "restoreglsl: batch %d/%d (S%d, %d frames) issued at frame %d", s_cur + 1, s_nbatches, S, bt->count, s_frameCount);
            rlog(b);
        }
        s_pass = 0; s_cur++;
    }
    s_draws++;
    return units;
}

int tagpu_rglsl_step(void)
{
    GLint fbo = 0, vp[4] = { 0, 0, 0, 0 };
    GLboolean blend, depth, scissor, cull, dmask, cmask[4];
    double allowed, spent = 0.0;
    int q, ndraw = 0;
    char b[300];

    if (s_state != 1) return s_state == 2 ? 1 : (s_state < 0 ? -1 : 0);
    if (!s_atlasTex || !s_destTex) return 0;

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
                    s_gpuNs += (double)ns; s_gpuUnits += s_qUnits[q];
                }
            } else if (++s_qStall < 300) {
                return 0;                      /* the GPU is a frame behind: add nothing */
            } else {
                /* a result that never comes is a driver that does not really
                   time queries: fixed slices from here rather than wait forever */
                rlog("restoreglsl: timer query never completed; fixed slices from here");
                x_glDeleteQueries(2, s_query); s_query[0] = s_query[1] = 0; s_qHave[0] = s_qHave[1] = 0;
            }
        }
    }
    allowed = (s_query[0] && s_nsPerUnit > 0.0) ? s_opt.budget * 1e6 / s_nsPerUnit : 0.0;

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    blend = x_glIsEnabled(GL_BLEND); depth = x_glIsEnabled(GL_DEPTH_TEST);
    scissor = x_glIsEnabled(GL_SCISSOR_TEST); cull = x_glIsEnabled(GL_CULL_FACE);
    x_glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask); x_glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    x_glDisable(GL_BLEND); x_glDisable(GL_DEPTH_TEST); x_glDisable(GL_SCISSOR_TEST); x_glDisable(GL_CULL_FACE);
    x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, s_palTex);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_rectTex);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, s_srcTex);

    if (s_frameCount < 2) {
        /* the error flag is process-wide and nobody drains it per frame: an
           error reported after a slice is ours only if none was pending before */
        int pending = 0;
        while (glGetError() != GL_NO_ERROR && pending < 16) pending++;
        if (pending) { _snprintf(b, sizeof b, "restoreglsl: %d GL error(s) were pending before slice %d (not ours)", pending, s_frameCount + 1); rlog(b); }
    }
    q = s_qFrame & 1;
    if (s_query[0]) x_glBeginQuery(GL_TIME_ELAPSED, s_query[q]);
    do {
        spent += issue_draw();
        ndraw++;
        if (s_cur >= s_nbatches) break;
        if (s_query[0]) { if (s_nsPerUnit <= 0.0 && ndraw >= 2) break; if (spent >= allowed) break; }
        else if (ndraw >= FIXED_DRAWS) break;
    } while (1);
    if (s_query[0]) { x_glEndQuery(GL_TIME_ELAPSED); s_qUnits[q] = spent; s_qHave[q] = 1; s_qFrame++; }
    s_totalUnits += spent;
    s_frameCount++;
    if (s_frameCount <= 2) {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) { _snprintf(b, sizeof b, "restoreglsl: GL error 0x%x after slice %d (batch %d pass %d)", e, s_frameCount, s_cur, s_pass); rlog(b); }
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

    if (s_cur >= s_nbatches) {
        double wall = now_ms() - s_t0;
        double gpuEst = s_nsPerUnit > 0.0 ? s_totalUnits * s_nsPerUnit / 1e6 : 0.0;
        s_state = 2;
        _snprintf(b, sizeof b, "restoreglsl: done: %d frames (%d wrap-padded) in %d batches, %d draws over %d frames = %.0f ms wall since begin (%.1f fps while restoring); GPU %.0f ms measured over %.0f%% of the work; %dx%d %s NK=%d budget %.0f ms",
                  s_nframes, s_nwrap, s_nbatches, s_draws, s_frameCount, wall,
                  wall > 0.0 ? 1000.0 * s_frameCount / wall : 0.0, s_gpuNs / 1e6,
                  s_totalUnits > 0.0 ? 100.0 * s_gpuUnits / s_totalUnits : 0.0,
                  s_w.depth, s_w.ch, s_opt.fp16 ? "fp16" : "fp32", s_nk, s_opt.budget);
        (void)gpuEst;
        rlog(b);
        /* the scratch goes now; the destination is the caller's */
        free_job(); free_gl();
        return 1;
    }
    return 0;
}

void tagpu_rglsl_abort(void)
{
    if (s_state == 1) rlog("restoreglsl: job abandoned");
    free_job();
    free_gl();
    s_state = 0;
}

void tagpu_rglsl_glreset(void)
{
    /* every id died with the context: forget them without deleting */
    s_progFill = s_progConv = s_progOut = 0;
    s_ubo = s_outVBO = s_rectTex = s_srcTex = s_palTex = s_fbo = s_destFBO = s_vao = s_outVAO = 0;
    s_act[0] = s_act[1] = 0; s_query[0] = s_query[1] = 0; s_actSide = 0;
    free_job();
    s_state = 0;
}
