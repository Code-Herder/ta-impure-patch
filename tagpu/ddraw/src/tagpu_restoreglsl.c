/* tagpu_restoreglsl.c -- the Classic++ restorer's GL BACKEND: the device
   resources and the three draws, in the game's own GL context.
   research/notes/renderers.md 4c has the decisions; tagpu_restore_glsl.h has
   the shaders and the layout they assume.

   WHAT IS AND IS NOT IN THIS FILE SINCE THE SPLIT. The scheduler that decides
   WHEN to draw -- the job queues, batch formation, the pass sequencer, the
   cost model, the GPU-time budget, the weight file, the options and every
   counter -- is `tagpu_restore_core.c`, which names no rendering API. This
   file is asked for one specific draw at a time and makes it. The header of
   tagpu_restore_core.h says why that line was drawn where it is; the short
   version is that landing 11 of the Vulkan-only plan deletes `opengl_utils.h`
   while two of this module's exports are called from gather halves that
   survive, so the module had to come apart and this is the half that goes.

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
#include "tagpu_restore_core.h"
#include "tagpu_classicpp.h"
#include "tagpu_opt.h"   /* tagpu_rglsl.step, the measurement lever below */

#define LANE      "restoreglsl"
#define SLOT_COLS TAGPU_R_SLOTCOLS
#define SLOT_ROWS TAGPU_R_SLOTROWS
#define BATCH     TAGPU_R_BATCH
#define ACT_MAX   TAGPU_R_ACTMAX
#define MAX_JOBS  TAGPU_R_MAXJOBS
#define MAX_NK    TAGPU_R_MAXNK

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

static void rlog(const char* s) { tagpu_rcore_log(s); }

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
typedef void (APIENTRY *PFN_GETTEXPARAMIV)(GLenum,GLenum,GLint*);
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
/* the mip reduction's ONLY optional entry point, and it is optional because it
   is what makes the reduction give the sampler back exactly as it found it: a
   driver where this does not resolve gets glGenerateMipmap, not a twin left
   clamped to one level. It is deliberately NOT on the mandatory list below --
   nothing else here reads a texture parameter. */
static PFN_GETTEXPARAMIV x_glGetTexParameteriv;
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
    x_glGetTexParameteriv = (PFN_GETTEXPARAMIV)getgl("glGetTexParameteriv");
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
        if (missing) { char b[128]; _snprintf(b, sizeof b, LANE ": GL entry point %s is missing", missing); rlog(b); }
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

/* ---- a job's GL half: the caller's two atlases and our palette snapshot ---- */
struct TAGPU_RGLSL_JOB {
    TAGPU_RCORE* core;
    GLuint atlasTex, destTex, palTex;
    int    atlasW, atlasH, destW, destH;
};
static struct TAGPU_RGLSL_JOB s_gjob[MAX_JOBS];

/* ---- shared GL state: once per context, for every job ---- */
static int    s_glReady;               /* programs, tables, weights uploaded  */
static int    s_glFailed;              /* and they cannot be: no more tries   */
static int    s_actFailed;             /* float targets do not work here      */
static GLuint s_progFill, s_progConv, s_progOut;
static GLuint s_progMip;        /* the twin's own mip reduction (landing 7e) */
static GLint  s_uMipSrc = -1, s_uMipDim = -1;
static GLuint s_mipFBO;
/* A STAND-DOWN IS LOGGED ONCE PER CONTEXT, not once per process: the context is
   replaced on a device loss and on the startup GL reset, and a reduction that
   works on one context and refuses on the next would otherwise say nothing the
   second time. Both flags are cleared with every other piece of mip state. */
static int    s_mipSaid, s_mipPendSaid;
static GLint  s_uFill[7], s_uConv[6], s_uOut[7];
static GLuint s_ubo, s_rectTex, s_srcTex, s_keyTex, s_act[2], s_vao, s_outVAO, s_outVBO, s_fbo, s_destFBO;
static int    s_actSide, s_actLayers, s_attached;
static GLuint s_query[2];
/* the caller's state a slice disturbs */
static GLint     s_saveFbo, s_saveVp[4];
static GLboolean s_saveBlend, s_saveDepth, s_saveScissor, s_saveCull, s_saveDmask, s_saveCmask[4];

/* the backend, forward-declared so the scheduler below can be initialised
   before init_gl -- tagpu_rglsl_step runs every frame from tagpu_native.c,
   including long before any job exists, so `be` is never NULL */
static int  gl_ready(void);
static int  gl_act_ensure(int side);
static int  gl_act_free(void);
static int  gl_draw(const TAGPU_RDRAWREQ* r);
static void gl_slice_begin(int q);
static void gl_slice_end(int q);
static int  gl_timer_poll(int q, double* ns);
static void gl_timer_off(void);
static void gl_state_push(unsigned slice);
static void gl_state_pop(unsigned slice);
static int  gl_may_draw(void);

static const TAGPU_RBACKEND s_be = {
    LANE,
    gl_ready,
    gl_act_ensure,
    gl_act_free,
    gl_draw,
    gl_slice_begin,
    gl_slice_end,
    gl_timer_poll,
    gl_timer_off,
    gl_state_push,
    gl_state_pop,
    gl_may_draw
};

static TAGPU_RSCHED s_sched = { &s_be };

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
        _snprintf(b, sizeof b, LANE ": %s FAILED: %s", label, lg); rlog(b);
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
    if (!ok) { char b[128]; _snprintf(b, sizeof b, LANE ": %s link FAILED", label); rlog(b); glDeleteProgram(p); return 0; }
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

/* the idle release; 1 when something was actually let go */
static int gl_act_free(void)
{
    if (!s_act[0]) return 0;
    glDeleteTextures(2, s_act);
    s_act[0] = s_act[1] = 0; s_actSide = 0;
    return 1;
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
    gl_act_free();
    if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
    if (s_destFBO) glDeleteFramebuffers(1, &s_destFBO);
    if (s_vao) glDeleteVertexArrays(1, &s_vao);
    if (s_outVAO) glDeleteVertexArrays(1, &s_outVAO);
    if (s_query[0] && x_glDeleteQueries) x_glDeleteQueries(2, s_query);
    if (s_progMip) glDeleteProgram(s_progMip);
    if (s_mipFBO) glDeleteFramebuffers(1, &s_mipFBO);
    s_progMip = 0; s_mipFBO = 0; s_uMipSrc = s_uMipDim = -1;
    s_mipSaid = 0; s_mipPendSaid = 0;
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
    GLuint vsFS, vsOut, fsFill, fsConv, fsOut, fsMip;
    const TAGPU_RMODEL* w;
    const TAGPU_ROPT*   opt;
    size_t i;
    float* padded;
    size_t padTexels;

    if (s_glReady) return 1;
    if (s_glFailed) return 0;
    s_glFailed = 1;                      /* until the end of this function */
    if (!resolve_gl()) return 0;
    /* the options and the model, re-read per context as they always were */
    if (!tagpu_rcore_reload(LANE)) return 0;
    w = tagpu_rcore_model(); opt = tagpu_rcore_opt();

    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUBO);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDraw);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxAtt);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    if (maxAtt < maxDraw) maxDraw = maxAtt;
    if (align > 256) { _snprintf(b, sizeof b, LANE ": UNIFORM_BUFFER_OFFSET_ALIGNMENT %d > 256", align); rlog(b); return 0; }
    if (!tagpu_rcore_pick_nk(&s_sched, (int)maxUBO, (int)maxDraw)) return 0;

    _snprintf(prefix, sizeof prefix, "#version 330 core\n#define NK %d\n#define WMAX %d\n", s_sched.nk, s_sched.wmax);
    vsFS   = mksh(GL_VERTEX_SHADER, prefix, TAGPU_RESTORE_FS_VS, "fs.vert");
    fsMip  = mksh(GL_FRAGMENT_SHADER, prefix, TAGPU_RESTORE_MIP_FS, "mip.frag");
    fsFill = mksh(GL_FRAGMENT_SHADER, prefix, TAGPU_RESTORE_FILL_FS, "fill.frag");
    fsConv = mksh(GL_FRAGMENT_SHADER, prefix, TAGPU_RESTORE_CONV_FS, "conv.frag");
    vsOut  = mksh(GL_VERTEX_SHADER, prefix, TAGPU_RESTORE_OUT_VS, "out.vert");
    fsOut  = mksh(GL_FRAGMENT_SHADER, prefix, TAGPU_RESTORE_OUT_FS, "out.frag");
    s_progFill = mkprog(vsFS, fsFill, "fill");
    s_progConv = mkprog(vsFS, fsConv, "conv");
    s_progOut  = mkprog(vsOut, fsOut, "out");
    /* THE MIP REDUCTION IS NOT FATAL IF IT WILL NOT BUILD: `tagpu_rglsl_mips`
       answers 0 and its caller falls back to glGenerateMipmap, which is what
       shipped before landing 7e -- losing only the cross-driver identity of
       the levels, which is the whole point of having it. */
    s_progMip  = mkprog(vsFS, fsMip, "mip");
    if (s_progMip) {
        s_uMipSrc = glGetUniformLocation(s_progMip, "uSrc");
        s_uMipDim = glGetUniformLocation(s_progMip, "uSrcDim");
        glUseProgram(s_progMip);
        if (s_uMipSrc >= 0) glUniform1i(s_uMipSrc, 0);
        glUseProgram(0);
        if (s_uMipDim < 0) { glDeleteProgram(s_progMip); s_progMip = 0;
            rlog(LANE ": the mip reduction has no uSrcDim - falling back to glGenerateMipmap"); }
    }
    if (vsFS) glDeleteShader(vsFS);
    if (fsFill) glDeleteShader(fsFill);
    if (fsConv) glDeleteShader(fsConv);
    if (vsOut) glDeleteShader(vsOut);
    if (fsOut) glDeleteShader(fsOut);
    if (fsMip) glDeleteShader(fsMip);
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
    glUniform1i(s_uFill[4], 5); glUniform1i(s_uFill[6], w->depth);
    glUseProgram(s_progConv);
    glUniform1i(s_uConv[0], 4); glUniform1i(s_uConv[1], 2);
    x_glUniformBlockBinding(s_progConv, x_glGetUniformBlockIndex(s_progConv, "WBlock"), 0);
    glUseProgram(s_progOut);
    glUniform1i(s_uOut[0], 4); glUniform1i(s_uOut[1], 0); glUniform1i(s_uOut[2], 1); glUniform1i(s_uOut[3], 2);
    glUniform1i(s_uOut[4], 5);
    glUseProgram(0);

    /* the weights: bound WMAX mat4s at a time, so the tail is padded */
    padTexels = (size_t)w->ntex + (size_t)s_sched.wmax * 4;
    padded = (float*)calloc(padTexels * 4, sizeof(float));
    if (!padded) { rlog(LANE ": out of memory"); free_gl(); return 0; }
    memcpy(padded, w->body, (size_t)w->ntex * 16);
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
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof s_sched.verts, NULL, GL_STREAM_DRAW);
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
    s_sched.timer = s_query[0] ? 1 : 0;
    s_sched.qHave[0] = s_sched.qHave[1] = 0; s_sched.qFrame = 0;
    s_sched.nsPerUnit = 0.0; s_sched.qStall = 0;
    s_sched.qJob[0] = s_sched.qJob[1] = NULL;
    s_actSide = 0; s_attached = 0; s_sched.slice = 0; s_sched.idle = 0;
    _snprintf(b, sizeof b, LANE ": %dx%d %s, NK=%d (uniform block %d KB of %d, %d draw buffers), %s, budget %.1f ms/frame",
              w->depth, w->ch, opt->fp16 ? "fp16" : "fp32", s_sched.nk, s_sched.wmax * 64 >> 10, maxUBO >> 10, maxDraw,
              s_query[0] ? "GL_TIME_ELAPSED budget" : "no timer query: fixed slices", opt->budget);
    rlog(b);
    s_glReady = 1; s_glFailed = 0;
    return 1;
}

/* the ping-pong arrays, grown to `side` (never past ACT_MAX), between batches only */
static int gl_act_ensure(int side)
{
    const TAGPU_RMODEL* w = tagpu_rcore_model();
    const TAGPU_ROPT*   opt = tagpu_rcore_opt();
    GLenum fmt = opt->fp16 ? GL_RGBA16F : GL_RGBA32F;
    GLenum st;
    char b[128];
    int l;
    if (s_actFailed) return 0;
    if (s_act[0] && s_actSide >= side) return 1;
    gl_act_free();
    s_actLayers = w->ch / 4;
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
        _snprintf(b, sizeof b, LANE ": %s array layer is not a complete render target (0x%x)", opt->fp16 ? "RGBA16F" : "RGBA32F", st);
        rlog(b);
        s_actFailed = 1;
        gl_act_free();
        return 0;
    }
    if (opt->log) {
        _snprintf(b, sizeof b, LANE ": activations %dx%d x %d layers (%d MB)", side, side, s_actLayers,
                  (int)(2u * (unsigned)side * (unsigned)side * (unsigned)s_actLayers * (opt->fp16 ? 8u : 16u) >> 20));
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

/* Bind the destination as a render target, check it is complete, and clear it
   to alpha 0 -- the restorer's "not painted here yet" mark -- unless the
   caller is REPAINTING one it already filled (a palette change: every rect is
   queued again, and clearing first would blank the world for the length of
   the job instead of recolouring it in place). */
static int prepare_dest(struct TAGPU_RGLSL_JOB* g, int clear)
{
    GLint fbo0 = 0;
    GLboolean scissor, cmask[4];
    GLfloat cc[4] = { 0.f, 0.f, 0.f, 0.f };
    int ok = 1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_destFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->destTex, 0);
    { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        char b[128];
        _snprintf(b, sizeof b, LANE ": %s: the destination atlas is not a complete render target",
                  g->core ? g->core->tag : "job");
        rlog(b);
        ok = 0;
    } else if (clear) {
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

/* ---- the three draws ---- */
static void upload_slot(GLuint tex, int unit, const float* v)
{
    glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, GL_RGBA, GL_FLOAT, v);
}

static int gl_draw(const TAGPU_RDRAWREQ* r)
{
    struct TAGPU_RGLSL_JOB* g = (struct TAGPU_RGLSL_JOB*)r->job->owner;

    if (r->kind == TAGPU_RDRAW_FILL) {
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g->atlasTex);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g->palTex);
        upload_slot(s_rectTex, 2, r->rect);
        upload_slot(s_srcTex, 3, r->src);
        upload_slot(s_keyTex, 5, r->key);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, r->TW, r->TH);
        attach(s_act[0], 0, 1);
        glUseProgram(s_progFill);
        glUniform1i(s_uFill[5], r->S);
        glBindVertexArray(s_vao);
        x_glDrawArrays(GL_TRIANGLES, 0, 3);
        return 1;
    }
    if (r->kind == TAGPU_RDRAW_CONV) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, r->TW, r->TH);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_rectTex);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, s_act[r->srcAct]);
        glUseProgram(s_progConv);
        glUniform1i(s_uConv[2], (int)r->L->jin);
        glUniform1i(s_uConv[3], (int)(r->L->kstride / 4));
        glUniform1i(s_uConv[4], r->S);
        glUniform1i(s_uConv[5], r->relu);
        attach(s_act[1 - r->srcAct], r->group, r->n);
        x_glBindBufferRange(GL_UNIFORM_BUFFER, 0, s_ubo,
                            (GLintptr)((size_t)(r->L->offset + (unsigned)r->group * r->L->kstride) * 16),
                            (GLsizeiptr)((size_t)s_sched.wmax * 64));
        glBindVertexArray(s_vao);
        x_glDrawArrays(GL_TRIANGLES, 0, 3);
        return 1;
    }
    /* OUT: straight into the caller's atlas, in the caller's cell layout */
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g->atlasTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g->palTex);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_rectTex);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, s_keyTex);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, s_act[r->srcAct]);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    attach(0, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_destFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->destTex, 0);
    { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
    glViewport(0, 0, g->destW, g->destH);
    glUseProgram(s_progOut);
    glUniform1i(s_uOut[5], r->S);
    x_glUniform2f(s_uOut[6], (float)g->destW, (float)g->destH);
    glBindVertexArray(s_outVAO);
    glBindBuffer(GL_ARRAY_BUFFER, s_outVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)r->nv * 32), r->verts, GL_STREAM_DRAW);
    x_glDrawArrays(GL_TRIANGLES, 0, r->nv);
    return 1;
}

/* ---- the slice bracket ---- */
static int gl_ready(void) { return s_glReady; }

static void gl_slice_begin(int q) { x_glBeginQuery(GL_TIME_ELAPSED, s_query[q]); }
static void gl_slice_end(int q)   { (void)q; x_glEndQuery(GL_TIME_ELAPSED); }

static int gl_timer_poll(int q, double* ns)
{
    GLuint64 avail = 0, t = 0;
    x_glGetQueryObjectui64v(s_query[q], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) return 0;
    x_glGetQueryObjectui64v(s_query[q], GL_QUERY_RESULT, &t);
    *ns = (double)t;
    return 1;
}

static void gl_timer_off(void)
{
    if (s_query[0]) x_glDeleteQueries(2, s_query);
    s_query[0] = s_query[1] = 0;
}

static void gl_state_push(unsigned slice)
{
    char b[200];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_saveFbo);
    glGetIntegerv(GL_VIEWPORT, s_saveVp);
    s_saveBlend = x_glIsEnabled(GL_BLEND); s_saveDepth = x_glIsEnabled(GL_DEPTH_TEST);
    s_saveScissor = x_glIsEnabled(GL_SCISSOR_TEST); s_saveCull = x_glIsEnabled(GL_CULL_FACE);
    x_glGetBooleanv(GL_DEPTH_WRITEMASK, &s_saveDmask); x_glGetBooleanv(GL_COLOR_WRITEMASK, s_saveCmask);
    x_glDisable(GL_BLEND); x_glDisable(GL_DEPTH_TEST); x_glDisable(GL_SCISSOR_TEST); x_glDisable(GL_CULL_FACE);
    x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (slice < 2) {
        /* the error flag is process-wide and nobody drains it per frame: an
           error reported after a slice is ours only if none was pending before */
        int pending = 0;
        while (glGetError() != GL_NO_ERROR && pending < 16) pending++;
        if (pending) { _snprintf(b, sizeof b, LANE ": %d GL error(s) were pending before slice %u (not ours)", pending, slice + 1); rlog(b); }
    }
}

static void gl_state_pop(unsigned slice)
{
    if (slice <= 2) {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) { char b[128]; _snprintf(b, sizeof b, LANE ": GL error 0x%x after slice %u", e, slice); rlog(b); }
    }
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s_saveFbo);
    glViewport(s_saveVp[0], s_saveVp[1], s_saveVp[2], s_saveVp[3]);
    if (s_saveBlend) x_glEnable(GL_BLEND);
    if (s_saveDepth) x_glEnable(GL_DEPTH_TEST);
    if (s_saveScissor) x_glEnable(GL_SCISSOR_TEST);
    if (s_saveCull) x_glEnable(GL_CULL_FACE);
    x_glDepthMask(s_saveDmask); x_glColorMask(s_saveCmask[0], s_saveCmask[1], s_saveCmask[2], s_saveCmask[3]);
}

static int gl_may_draw(void) { return tagpu_classicpp_assets(); }

/* ---- the public API: unchanged, so no caller moved with this split ---- */

static TAGPU_RGLSL_JOB* job_new_x(const char* tag, int prio, int oneshot,
                                  unsigned int atlasTex, int atlasW, int atlasH,
                                  const unsigned char* pal,
                                  unsigned int destTex, int destW, int destH, int clear)
{
    TAGPU_RCORE* c;
    struct TAGPU_RGLSL_JOB* g;
    unsigned char palRGBA[256 * 4];
    int i;
    char b[200];
    if (!atlasTex || !destTex || !pal || atlasW <= 0 || atlasH <= 0 || destW <= 0 || destH <= 0) return NULL;
    if (!init_gl()) return NULL;
    c = tagpu_rcore_job_new(&s_sched, tag, prio, oneshot, NULL);
    if (!c) return NULL;
    g = &s_gjob[(int)(c - s_sched.jobs)];
    memset(g, 0, sizeof *g);
    c->owner = g; g->core = c;
    g->atlasTex = atlasTex; g->atlasW = atlasW; g->atlasH = atlasH;
    g->destTex = destTex; g->destW = destW; g->destH = destH;
    /* the palette snapshot: R,G,B,pad -> RGBA8 */
    for (i = 0; i < 256; i++) { palRGBA[i * 4] = pal[i * 4]; palRGBA[i * 4 + 1] = pal[i * 4 + 1]; palRGBA[i * 4 + 2] = pal[i * 4 + 2]; palRGBA[i * 4 + 3] = 255; }
    glGenTextures(1, &g->palTex);
    glBindTexture(GL_TEXTURE_2D, g->palTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palRGBA);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!prepare_dest(g, clear)) {
        glDeleteTextures(1, &g->palTex);
        tagpu_rcore_job_free(&s_sched, c);
        return NULL;
    }
    if (!oneshot) {
        _snprintf(b, sizeof b, LANE ": %s: lazy restore armed (%dx%d twin of the %dx%d atlas)", c->tag, destW, destH, atlasW, atlasH);
        rlog(b);
    }
    return g;
}

TAGPU_RGLSL_JOB* tagpu_rglsl_job_new(const char* tag, int prio, int oneshot,
                                     unsigned int atlasTex, int atlasW, int atlasH,
                                     const unsigned char* pal,
                                     unsigned int destTex, int destW, int destH)
{
    return job_new_x(tag, prio, oneshot, atlasTex, atlasW, atlasH, pal, destTex, destW, destH, 1);
}

TAGPU_RGLSL_JOB* tagpu_rglsl_job_repaint(const char* tag, int prio, int oneshot,
                                         unsigned int atlasTex, int atlasW, int atlasH,
                                         const unsigned char* pal,
                                         unsigned int destTex, int destW, int destH)
{
    return job_new_x(tag, prio, oneshot, atlasTex, atlasW, atlasH, pal, destTex, destW, destH, 0);
}

void tagpu_rglsl_job_repalette(TAGPU_RGLSL_JOB* j, const unsigned char* pal)
{
    unsigned char palRGBA[256 * 4];
    int i;
    if (!j || !j->core || !j->core->used || !j->palTex || !pal) return;
    for (i = 0; i < 256; i++) { palRGBA[i * 4] = pal[i * 4]; palRGBA[i * 4 + 1] = pal[i * 4 + 1]; palRGBA[i * 4 + 2] = pal[i * 4 + 2]; palRGBA[i * 4 + 3] = 255; }
    glBindTexture(GL_TEXTURE_2D, j->palTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, palRGBA);
    glBindTexture(GL_TEXTURE_2D, 0);
}

int tagpu_rglsl_job_add(TAGPU_RGLSL_JOB* j, const TAGPU_RGLSL_FRAME* frames, int count)
{
    if (!j || !j->core) return 0;
    return tagpu_rcore_job_add(&s_sched, j->core, frames, count);
}

void tagpu_rglsl_job_clear(TAGPU_RGLSL_JOB* j)
{
    if (!j || !j->core || !j->core->used) return;
    tagpu_rcore_job_drop(j->core);
    if (!j->core->failed) prepare_dest(j, 1);
}

int tagpu_rglsl_job_idle(const TAGPU_RGLSL_JOB* j)
{
    return !j || !j->core || !j->core->used || (j->core->qn == 0 && !j->core->inflight);
}

int tagpu_rglsl_job_failed(const TAGPU_RGLSL_JOB* j)
{
    return j && j->core && j->core->used && j->core->failed;
}

int tagpu_rglsl_job_painted(const TAGPU_RGLSL_JOB* j)
{
    return (j && j->core && j->core->used) ? j->core->tframes : 0;
}

void tagpu_rglsl_job_free(TAGPU_RGLSL_JOB* j)
{
    if (!j || !j->core || !j->core->used) return;
    if (j->palTex) glDeleteTextures(1, &j->palTex);
    tagpu_rcore_job_free(&s_sched, j->core);
    memset(j, 0, sizeof *j);
}

void tagpu_rglsl_step(void) { tagpu_rcore_step(&s_sched); }

unsigned tagpu_rglsl_calls(void) { return s_sched.calls; }

/* `tagpu_rglsl.step` -- STEP THE RESTORER WITHOUT ARMING A PASS, and it exists
   for the A/B rather than for play.

   Why it has to exist at all. `tagpu_rglsl_step` runs from `tagpu_native.c`
   only inside `if (fxOn || sfxOn || featOn || terrOn || markOn)`, so measuring
   restored art has always meant arming one of those five -- and the capture
   requires exactly ONE pass drawing into the Vulkan frame, so the only usable
   choice was the one of the five with no Vulkan pass of its own. That was
   `mark`, until the landing that ported it. With all five drawing, an A/B of
   restored art would be unmeasurable: arm nothing and the twin stays alpha 0
   and both lanes agree about a branch neither took (which is how gate 3a's
   first figures came out wrong), arm anything and the lane refuses the capture
   with *"2 passes drew into it"*.

   So the lever steps the restorer and arms NOTHING. It is not a play knob and
   should not become one: the restorer's own slice budget still bounds it, but
   a session that steps it with no pass asking for a twin is doing work for
   nobody. [The Vulkan-only plan's landing 5, which is what broke the old
   recipe and therefore owed this.] */
int tagpu_rglsl_step_forced(unsigned frame_counter)
{
    static int      s_force = -1;
    static unsigned s_forceCheck;
    char b[64];
    if (s_force >= 0 && frame_counter - s_forceCheck < 30) return s_force > 0;
    s_forceCheck = frame_counter;
    {
        int was = s_force;
        s_force = tagpu_opt_read("tagpu_rglsl.step", b, sizeof b) >= 0;
        if (s_force != was)
            rlog(s_force ? LANE ": tagpu_rglsl.step - the restorer is stepped with no pass armed (a measurement lever, not a play one)"
                         : LANE ": tagpu_rglsl.step gone - back to the pass arming");
    }
    return s_force > 0;
}

/* THE TWIN'S OWN MIP CHAIN -- levels 1..mip of `tex` from the level above
   each, as the exact integer 2x2 box average, so that the levels this lane
   sees and the ones a second backend paints are the SAME BYTES rather than
   two drivers' idea of glGenerateMipmap (gpu-status 2.45, and the shader's own
   header comment for why +/-1 was not good enough). 1 when the whole chain was
   reduced; 0 when it was not, and then the caller must fall back -- a twin
   whose levels 1.. were left as they were is a twin that filters to stale
   colours, which is the one outcome worse than a per-driver +/-1.

   IT IS NOT A SLICE and it is not budgeted like one: the whole chain of a 2048
   twin is 1024x1024 + 512x512 fragments, once per batch of frames painted, and
   it has to be complete before the frame that samples it. It borrows the
   slice's state bracket rather than repeating it, so that a piece of state
   added to gl_state_push is covered here too; the sentinel slice is what tells
   that bracket this is not a numbered slice and owes no error bookkeeping.

   THE FEEDBACK LOOP IS CLOSED BY THE SAMPLER, not by a copy: level L-1 is read
   while level L is the colour attachment, both of them levels of one texture,
   which is legal exactly because base = max = L-1 excludes the level being
   written from everything the sampler can reach. A second attachment or a
   staging copy would buy nothing and cost the chain. */
/* EVERY WAY THIS STANDS DOWN SAYS SO, AND SAYS WHICH ONE. Four of the six used
   to be silent -- no restorer program, no `glGetTexParameteriv`, an odd level, no
   framebuffer -- and the skill told an operator to grep `mip reduction` to find
   out. A measurement instrument whose "it did not run" reads exactly like "it
   ran" is worse than no instrument: the chain check then reports the driver's
   +/-1 and the operator reads it as a bug in the shader. [Landing 7e-1's review.] */
static int mip_stand_down(const char* why)
{
    if (!s_mipSaid) {
        char b[220];
        s_mipSaid = 1;
        _snprintf(b, sizeof b, LANE ": the mip reduction stood down (%s) - glGenerateMipmap builds the levels instead, inside the +/-1 per RGB channel gpu-status 2.45 measured", why);
        rlog(b);
    }
    return 0;
}

int tagpu_rglsl_mips(unsigned tex, int dim, int mip)
{
    GLint base = 0, maxl = 0, minf = 0, magf = 0;
    GLenum err;
    int L, ok = 1, bad = 0, pending = 0;

    if (!tex || dim <= 0 || mip <= 0) return 0;      /* nothing asked for */
    if (!s_glReady)  return mip_stand_down("the restorer is not up");
    if (!s_progMip)  return mip_stand_down("the reduction program did not build");
    /* the one optional entry point, and it is what lets the four borrowed
       texture parameters be given back exactly as they were found */
    if (!x_glGetTexParameteriv) return mip_stand_down("glGetTexParameteriv is missing");
    /* an odd level would need GL's weighted three-tap, not a 2x2 average, so
       the chain is refused whole rather than reduced wrongly for part of it */
    for (L = 1; L <= mip; L++)
        if ((dim >> L) < 1 || ((dim >> (L - 1)) & 1))
            return mip_stand_down("a level of this chain is odd");
    if (!s_mipFBO) {
        glGenFramebuffers(1, &s_mipFBO);
        if (!s_mipFBO) return mip_stand_down("no framebuffer");
    }

    /* the sentinel slice: gl_state_push/pop keep the whole list of state a draw
       here disturbs, and this is not a numbered slice, so it owes none of their
       PER-SLICE error bookkeeping -- but it owes the bookkeeping itself, because
       this runs at the TOP of the frame and the slice runs later in the same
       one, so a pending error another pass raised is consumed HERE. Reporting it
       is the whole value of that drain: without this the slice bracket's
       "N GL error(s) were pending (not ours)" -- the only place in the build
       that reports an error nobody owns -- silently stopped firing on every
       frame the unit twin was reduced. [Landing 7e-1's review.] */
    gl_state_push(~0u);
    while (glGetError() != GL_NO_ERROR && pending < 16) pending++;
    if (pending && !s_mipPendSaid) {
        char pb[160];
        s_mipPendSaid = 1;
        _snprintf(pb, sizeof pb, LANE ": %d GL error(s) were pending before the mip reduction (not ours)", pending);
        rlog(pb);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    x_glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
    x_glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxl);
    x_glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
    x_glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUseProgram(s_progMip);
    glBindVertexArray(s_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s_mipFBO);
    for (L = 1; L <= mip && ok; L++) {
        int src = dim >> (L - 1), dst = dim >> L;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, L - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, L - 1);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, L);
        { GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; glDrawBuffers(1, bufs); }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { ok = 0; bad = L; break; }
        glViewport(0, 0, dst, dst);
        glUniform1i(s_uMipDim, src);
        x_glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    /* the sampler goes back to what the consumer set it to -- this function
       borrowed four parameters of somebody else's texture and owes all four */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, base);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
    glBindTexture(GL_TEXTURE_2D, 0);
    err = glGetError();
    gl_state_pop(~0u);
    /* AN ERROR ANYWHERE IN THE CHAIN FAILS THE WHOLE CHAIN, and the caller then
       regenerates all of it: glGenerateMipmap rebuilds every level from level 0,
       so the fallback leaves one reduction's levels rather than a mixture of
       two. That is the only reason a partial failure needs no unwinding. */
    if (err != GL_NO_ERROR) { ok = 0; if (!bad) bad = mip; }
    if (!ok && !s_mipSaid) {
        char b[200];
        s_mipSaid = 1;      /* the same once-per-CONTEXT flag the refusals use */
        _snprintf(b, sizeof b, LANE ": the mip reduction failed at level %d of %d (GL 0x%x) - glGenerateMipmap builds the levels instead", bad, mip, (unsigned)err);
        rlog(b);
    }
    return ok;
}

void tagpu_rglsl_glreset(void)
{
    /* every id died with the context: forget them without deleting */
    s_progMip = 0; s_mipFBO = 0; s_uMipSrc = s_uMipDim = -1;
    s_mipSaid = 0; s_mipPendSaid = 0;
    s_progFill = s_progConv = s_progOut = 0;
    s_ubo = s_outVBO = s_rectTex = s_srcTex = s_keyTex = s_fbo = s_destFBO = s_vao = s_outVAO = 0;
    s_act[0] = s_act[1] = 0; s_query[0] = s_query[1] = 0; s_actSide = 0;
    s_glReady = 0; s_glFailed = 0; s_actFailed = 0;
    memset(s_gjob, 0, sizeof s_gjob);
    tagpu_rcore_lost(&s_sched);
}
