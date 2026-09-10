/* tagpu_shadow.c -- Classic++ cast shadows: the lab's depth map along
   `shadowsun`, in the game (tagpu_shadow.h; renderers.md 2.7-2.9, 2.12;
   tascene-view.html shadowFrame / shadowPass / LAB_LIGHT).

   One depth texture, orthographic along the shadow sun, drawn once per frame
   from everything with geometry -- the frame's 3DO stream (units and wrecks,
   per unit so the caster's own length rule can scale it), the replacement
   meshes (tagpu_hires_draw.c), and the heightfield mesh the terrain module
   keeps (tagpu_terr.c) -- and read back by the terrain and unit fragment
   shaders through tagpu_glsl.h's taShadowAt: a blocker search from the
   receiver's own texel and 16 Poisson taps on the raw depths, a 16-tap
   Poisson PCF through a compare sampler, receiver-plane
   bias, the penumbra from the blocker distance. The two views of the one
   texture are sampler objects (GL 3.3 -- render_ogl.c asks for 3.3 core since
   this landing) on texture units 12 (compare + bilinear) and 13 (raw,
   nearest), the lab's units.

   THE GRID IS ANCHORED TO THE MAP (2.7). The lab rebuilds its light-space
   bounds from the eye every frame, so every scroll would move the texel
   lattice under the PCF kernel and every edge would crawl. Here the texel is
   a function of zoom only: the lab's density at 1x, by OCTAVES (2.7, refined
   2026-09-06 -- the zoom is continuous and eased, so a texel proportional to
   1/zoom would re-anchor the lattice every frame of an ease), the map origin
   sits on a texel corner, and the window over the lattice moves by whole
   texels. Below 1x the map doubles to 4096 before the texel doubles. The
   depth range is the map's full 0..255 plus a 256-unit caster allowance,
   constant per map; only its offset moves with the window, a translation
   both sides of every compare share.

   Not a byte of the engine is touched here: the module reads the view the
   native pass already built (TAGPU_FXVIEW) and the knobs tagpu_classicpp.c
   parsed. */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "opengl_utils.h"
#include "tagpu_shadow.h"
#include "tagpu_classicpp.h"
#include "tagpu_terr.h"

static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* entry points opengl_utils.h does not carry (fetched by name, as every
   native module does: opengl_utils.h's glGetIntegerv is NULL in this fork) */
typedef void (APIENTRY *PFN_GENSAMPLERS)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_DELSAMPLERS)(GLsizei, const GLuint*);
typedef void (APIENTRY *PFN_SAMPLERPARAMI)(GLuint, GLenum, GLint);
typedef void (APIENTRY *PFN_BINDSAMPLER)(GLuint, GLuint);
typedef void (APIENTRY *PFN_READBUFFER)(GLenum);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_DEPTHFUNC)(GLenum);
typedef void (APIENTRY *PFN_DEPTHMASK)(GLboolean);
typedef void (APIENTRY *PFN_UNIFORM3F)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_GETINTEGERV)(GLenum, GLint*);
static PFN_GENSAMPLERS   x_glGenSamplers;
static PFN_DELSAMPLERS   x_glDeleteSamplers;
static PFN_SAMPLERPARAMI x_glSamplerParameteri;
static PFN_BINDSAMPLER   x_glBindSampler;
static PFN_READBUFFER    x_glReadBuffer;
static PFN_ACTIVETEX     x_glActiveTexture;
static PFN_DISABLE       x_glDisable;
static PFN_DEPTHFUNC     x_glDepthFunc;
static PFN_DEPTHMASK     x_glDepthMask;
static PFN_UNIFORM3F     x_glUniform3f;
static PFN_GETINTEGERV   x_glGetIntegerv;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

#define UNIT_CMP 12            /* texture unit of the compare sampler   */
#define UNIT_RAW 13            /* ...and of the raw one (the lab's)     */
#define MARGIN   192.0f        /* the lab's caster margin, world units  */
#define Y_TOP    (255.0f + 256.0f)   /* the map's height byte plus the caster allowance */

static int    s_state = 0;     /* 0 unloaded 1 ready 2 failed          */
static GLuint s_tex, s_fbo, s_cmp, s_raw, s_progU, s_progH;
static GLint  s_uMatU, s_uCast, s_uMatH;
static int    s_res;           /* the texture's current edge           */
static int    s_maxTex = 4096;
static int    s_live;          /* the map holds this frame's casters   */
static float  s_mat[16];
static float  s_texel, s_depth;
static int    s_rows0, s_rows1;/* heightfield rows under the window    */
static float  s_cot;           /* the shadow sun: horizontal / vertical */
static unsigned s_logged;      /* frames logged (once per change)      */
static int    s_lastRes, s_lastK;
static float  s_lastZoom = -1.0f;

/* ---- the depth program for the native stream: attributes 4 (world x,
   PROJECTED z) and 5 (posed height) of tagpu_native.c's VAO, and the
   caster's three numbers. The world point is the one the unit shader's
   vShW derives -- the same expression, so a unit is consistent with itself
   (renderers.md 2.11): real z = projected z + (altitude + height)/2, and the
   shadow's height is the ground plus the throw plus the scaled model height. */
static const char* VS_U =
    "#version 330 core\n"
    "layout(location=4) in vec2 aWorld;\n"
    "layout(location=5) in float aVY;\n"
    "uniform mat4 uShadowMat;\n"
    "uniform vec3 uCast;\n"                  /* altitude, ground + throw, sv */
    "void main(){\n"
    "  vec3 W = vec3(aWorld.x, uCast.y + uCast.z * aVY,\n"
    "                aWorld.y + (uCast.x + aVY) * 0.5);\n"
    "  gl_Position = uShadowMat * vec4(W, 1.0);\n"
    "}\n";
/* the heightfield mesh: one world point per vertex (tagpu_terr.c) */
static const char* VS_H =
    "#version 330 core\n"
    "layout(location=0) in vec3 aW;\n"
    "uniform mat4 uShadowMat;\n"
    "void main(){ gl_Position = uShadowMat * vec4(aW, 1.0); }\n";
static const char* FS_NONE =
    "#version 330 core\n"
    "void main(){}\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
               slog("shadow: shader FAILED:"); slog(lg); s_state = 2; }
    return sh;
}

static GLuint mkprog(const char* vs, const char* fs, GLint* uMat)
{
    GLuint v = mksh(GL_VERTEX_SHADER, vs), f = mksh(GL_FRAGMENT_SHADER, fs), p;
    GLint ok = 0;
    if (s_state == 2) return 0;
    p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v); glDeleteShader(f);
    if (!ok) { slog("shadow: link FAILED"); s_state = 2; return 0; }
    *uMat = glGetUniformLocation(p, "uShadowMat");
    return p;
}

static void init_gl(void)
{
    x_glGenSamplers       = (PFN_GENSAMPLERS)  getgl("glGenSamplers");
    x_glDeleteSamplers    = (PFN_DELSAMPLERS)  getgl("glDeleteSamplers");
    x_glSamplerParameteri = (PFN_SAMPLERPARAMI)getgl("glSamplerParameteri");
    x_glBindSampler       = (PFN_BINDSAMPLER)  getgl("glBindSampler");
    x_glReadBuffer        = (PFN_READBUFFER)   getgl("glReadBuffer");
    x_glActiveTexture     = (PFN_ACTIVETEX)    getgl("glActiveTexture");
    x_glDisable           = (PFN_DISABLE)      getgl("glDisable");
    x_glDepthFunc         = (PFN_DEPTHFUNC)    getgl("glDepthFunc");
    x_glDepthMask         = (PFN_DEPTHMASK)    getgl("glDepthMask");
    x_glUniform3f         = (PFN_UNIFORM3F)    getgl("glUniform3f");
    x_glGetIntegerv       = (PFN_GETINTEGERV)  getgl("glGetIntegerv");
    if (!x_glGenSamplers || !x_glDeleteSamplers || !x_glSamplerParameteri ||
        !x_glBindSampler) {
        slog("shadow: sampler objects unavailable (GL 3.3) -- Classic++ shadows off");
        s_state = 2; return;
    }
    if (!x_glReadBuffer || !x_glActiveTexture || !x_glDisable || !x_glDepthFunc ||
        !x_glDepthMask || !x_glUniform3f) {
        slog("shadow: missing GL proc -- Classic++ shadows off"); s_state = 2; return;
    }
    s_progU = mkprog(VS_U, FS_NONE, &s_uMatU);
    s_progH = mkprog(VS_H, FS_NONE, &s_uMatH);
    if (s_state == 2) return;
    s_uCast = glGetUniformLocation(s_progU, "uCast");

    /* the map: one depth texture; the raw view NEAREST on the texture itself,
       the compare view through a sampler object, which is what lets one
       texture serve both sampler types (the lab's shCmp / shRaw) */
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_res = 0;                          /* allocated on first use, at the frame's size */
    x_glGenSamplers(1, &s_cmp);
    x_glSamplerParameteri(s_cmp, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    x_glSamplerParameteri(s_cmp, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    x_glSamplerParameteri(s_cmp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    x_glSamplerParameteri(s_cmp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    x_glSamplerParameteri(s_cmp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    x_glSamplerParameteri(s_cmp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    x_glGenSamplers(1, &s_raw);
    x_glSamplerParameteri(s_raw, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    x_glSamplerParameteri(s_raw, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    x_glSamplerParameteri(s_raw, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    x_glSamplerParameteri(s_raw, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s_fbo);
    if (x_glGetIntegerv) {
        GLint m = 0;
        x_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m);
        if (m >= 256) s_maxTex = m;
    }
    {
        char b[160];
        const char* v = (const char*)glGetString(GL_VERSION);
        _snprintf(b, sizeof b, "shadow: GL ready (GL_VERSION %s, max texture %d)",
                  v ? v : "?", s_maxTex);
        slog(b);
    }
    s_state = 1;
}

/* (re)allocate the map at `res` and attach it: depth only, no colour buffer
   drawn or read, which is what makes the FBO complete without one */
static int ensure_tex(int res)
{
    GLenum st;
    if (s_res == res) return 1;
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, res, res, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_tex, 0);
    { GLenum none = GL_NONE; glDrawBuffers(1, &none); }
    x_glReadBuffer(GL_NONE);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char b[128];
        _snprintf(b, sizeof b, "shadow: depth FBO incomplete (0x%x) at %d -- Classic++ shadows off",
                  (unsigned)st, res);
        slog(b);
        s_state = 2;
        return 0;
    }
    s_res = res;
    return 1;
}

/* ---- the light-space frame (the lab's shadowFrame, anchored) ------------ */
static void basis(const float* lz, float* lx, float* ly)
{
    float l;
    lx[0] = lz[2]; lx[1] = 0.0f; lx[2] = -lz[0];        /* up x sun, normalised */
    l = sqrtf(lx[0] * lx[0] + lx[2] * lx[2]);
    if (l < 1e-4f) { lx[0] = 1.0f; lx[2] = 0.0f; }
    else { lx[0] /= l; lx[2] /= l; }
    ly[0] = lz[1] * lx[2] - lz[2] * lx[1];
    ly[1] = lz[2] * lx[0] - lz[0] * lx[2];
    ly[2] = lz[0] * lx[1] - lz[1] * lx[0];
}

/* the light-space extent of a world box over its eight corners:
   e = u0, u1, v0, v1, d0, d1; depth increases away from the light */
static void extent(const float* lx, const float* ly, const float* lz,
                   const float* bx, const float* by, const float* bz, float* e)
{
    int i;
    e[0] = e[2] = e[4] = 1e30f; e[1] = e[3] = e[5] = -1e30f;
    for (i = 0; i < 8; i++) {
        float X = bx[i & 1], Y = by[(i >> 1) & 1], Z = bz[(i >> 2) & 1];
        float u = X * lx[0] + Y * lx[1] + Z * lx[2];
        float v = X * ly[0] + Y * ly[1] + Z * ly[2];
        float d = -(X * lz[0] + Y * lz[1] + Z * lz[2]);
        if (u < e[0]) e[0] = u;
        if (u > e[1]) e[1] = u;
        if (v < e[2]) e[2] = v;
        if (v > e[3]) e[3] = v;
        if (d < e[4]) e[4] = d;
        if (d > e[5]) e[5] = d;
    }
}

/* column-major row r of the matrix: ndc = 2 (a . W - lo) / s - 1 */
static void mrow(float* m, int r, const float* a, float s, float lo)
{
    m[r] = 2.0f * a[0] / s; m[4 + r] = 2.0f * a[1] / s; m[8 + r] = 2.0f * a[2] / s;
    m[12 + r] = -1.0f - 2.0f * lo / s;
}

static void frame(const TAGPU_FXVIEW* v, const TAGPU_LIGHT* L)
{
    const float* lz = L->shadowSun;
    float lx[3], ly[3], nlz[3], e1[6], e[6], bx[2], bz[2];
    const float by[2] = { 0.0f, Y_TOP };
    /* the zoomed viewport's world window: frame px and world px are 1:1, the
       eye is the viewport's top-left in world (the gathers' own arithmetic) */
    float X0 = (float)(v->eyeX + (v->evpL - v->vpL));
    float Z0 = (float)(v->eyeY + (v->evpT - v->vpT));
    float need, base, texel, span, u0, v0;
    int res, k, i;

    basis(lz, lx, ly);
    /* the base texel: the lab's density -- the 1x window at this viewport,
       with the lab's margins, over shadowres. A function of the viewport size
       and the shadow sun only, so it is the same every frame */
    bx[0] = (float)v->eyeX - MARGIN; bx[1] = (float)(v->eyeX + v->vw) + MARGIN;
    bz[0] = (float)v->eyeY - MARGIN; bz[1] = (float)(v->eyeY + v->vh) + 128.0f + MARGIN;
    extent(lx, ly, lz, bx, by, bz, e1);
    base = (e1[1] - e1[0] > e1[3] - e1[2] ? e1[1] - e1[0] : e1[3] - e1[2]) / (float)L->shadowres;
    if (base < 1e-3f) base = 1e-3f;
    /* this frame's window */
    bx[0] = X0 - MARGIN; bx[1] = X0 + (float)v->evw + MARGIN;
    bz[0] = Z0 - MARGIN; bz[1] = Z0 + (float)v->evh + 128.0f + MARGIN;
    extent(lx, ly, lz, bx, by, bz, e);
    need = e[1] - e[0] > e[3] - e[2] ? e[1] - e[0] : e[3] - e[2];
    /* the octave: the smallest k whose map covers the window; below 1x the
       map doubles before the texel does (renderers.md 2.7) */
    res = L->shadowres;
    k = (int)ceilf(logf(need / ((float)res * base)) / logf(2.0f) - 1e-4f);
    if (k > 0 && res * 2 <= 4096 && res * 2 <= s_maxTex) { res *= 2; k -= 1; }
    while (res > s_maxTex) { res /= 2; k += 1; }
    if (k < -4) k = -4;
    if (k > 8) k = 8;
    texel = base * ldexpf(1.0f, k);
    span = (float)res * texel;
    /* the map centred on the window, then its origin snapped to the lattice
       (the lattice is the multiples of the texel from the map origin, which
       is light-space (0, 0) since the world origin projects there) */
    u0 = e[0] - (span - (e[1] - e[0])) * 0.5f;
    v0 = e[2] - (span - (e[3] - e[2])) * 0.5f;
    u0 = floorf(u0 / texel) * texel;
    v0 = floorf(v0 / texel) * texel;
    for (i = 0; i < 16; i++) s_mat[i] = 0.0f;
    nlz[0] = -lz[0]; nlz[1] = -lz[1]; nlz[2] = -lz[2];
    mrow(s_mat, 0, lx, span, u0);
    mrow(s_mat, 1, ly, span, v0);
    mrow(s_mat, 2, nlz, e[5] - e[4], e[4]);
    s_mat[15] = 1.0f;
    s_texel = texel; s_depth = e[5] - e[4];
    s_rows0 = (int)floorf(bz[0] / 16.0f);
    s_rows1 = (int)ceilf(bz[1] / 16.0f);
    s_cot = sqrtf(lz[0] * lz[0] + lz[2] * lz[2]) / (lz[1] > 1e-3f ? lz[1] : 1e-3f);
    if (res != s_lastRes || k != s_lastK || v->zoom != s_lastZoom) {
        if (s_logged < 40) {
            char b[200];
            _snprintf(b, sizeof b, "shadow: frame zoom=%.3f res=%d k=%d texel=%.3f span=%.0f "
                      "depth=%.0f window=(%.0f,%.0f %dx%d) rows=%d..%d",
                      v->zoom, res, k, texel, span, s_depth, X0, Z0, v->evw, v->evh,
                      s_rows0, s_rows1);
            slog(b);
            s_logged++;
        }
        s_lastRes = res; s_lastK = k; s_lastZoom = v->zoom;
    }
    ensure_tex(res);
}

/* ---- the frame protocol ------------------------------------------------ */
/* The gate reads the MASTER ARM, not assets/light (G18a): the depth map is the
   shadow dimension's, and `shadows=` is that dimension's own key. This map is
   the SOFT value alone (G18b) -- at `shadows=2` the frame draws Classic's own
   silhouette and slant instead (tagpu_native.c), and the two are never both on.

   `amb >= 1.0f` is no longer `sun=off` (G18b routed that through `light=`); it
   is now only an explicit `amb=1`, and there it is an honest early out rather
   than a policy: taLambert multiplies the shadow term by (1 - amb), so at amb 1
   the pass would cost a depth render and change no pixel. */
int tagpu_shadow_begin(const TAGPU_FXVIEW* v, int engineShadowBit)
{
    const TAGPU_LIGHT* L = tagpu_classicpp_light();
    s_live = 0;
    if (!tagpu_classicpp_on() || L->shadows != TAGPU_SHADOWS_SOFT ||
        !engineShadowBit || L->amb >= 1.0f) return 0;
    if (s_state == 0) init_gl();
    if (s_state != 1) return 0;
    frame(v, L);
    if (s_state != 1) return 0;                 /* the FBO refused */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_res, s_res);
    x_glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    x_glDepthFunc(GL_LESS);
    x_glDepthMask(GL_TRUE);
    x_glDisable(GL_BLEND);
    x_glDisable(GL_CULL_FACE);
    x_glDisable(GL_STENCIL_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(s_progU);
    glUniformMatrix4fv(s_uMatU, 1, GL_FALSE, s_mat);
    s_live = 1;
    return 1;
}

void tagpu_shadow_unit(float alt, float gndThrow, float sv)
{
    x_glUniform3f(s_uCast, alt, gndThrow, sv);
}

void tagpu_shadow_hills(void)
{
    const TAGPU_LIGHT* L = tagpu_classicpp_light();
    if (!s_live || !L->terrainshadow) return;
    glUseProgram(s_progH);
    glUniformMatrix4fv(s_uMatH, 1, GL_FALSE, s_mat);
    tagpu_terr_hills_draw(s_rows0, s_rows1);
}

/* tagpu_shadowdump.on: write the map once as a 16-bit PGM (near = small),
   the lab's `debug=shadow` for the game -- how a caster's silhouette in
   light space is checked without guessing (tagpu_shadow.pgm, then the
   trigger is removed) */
static void dump_map(void)
{
    static unsigned s_poll;
    float* buf;
    unsigned short* out;
    FILE* f;
    size_t n = (size_t)s_res * (size_t)s_res, i;
    if (((s_poll++) % 60) != 0) return;
    if (GetFileAttributesA("tagpu_shadowdump.on") == INVALID_FILE_ATTRIBUTES) return;
    buf = (float*)malloc(n * sizeof(float));
    out = (unsigned short*)malloc(n * 2);
    if (!buf || !out) { free(buf); free(out); return; }
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    for (i = 0; i < n; i++) {
        float v = buf[i] * 65535.0f;
        unsigned short u = (unsigned short)(v < 0.0f ? 0.0f : v > 65535.0f ? 65535.0f : v);
        out[i] = (unsigned short)((u >> 8) | (u << 8));      /* PGM is big-endian */
    }
    f = fopen("tagpu_shadow.pgm", "wb");
    if (f) {
        fprintf(f, "P5\n%d %d\n65535\n", s_res, s_res);
        fwrite(out, 2, n, f);
        fclose(f);
        {
            char b[200];
            _snprintf(b, sizeof b, "shadow: dumped %dx%d map (texel %.3f depth %.0f) to tagpu_shadow.pgm; matrix"
                      " %.5f %.5f %.5f %.3f / %.5f %.5f %.5f %.3f / %.5f %.5f %.5f %.3f",
                      s_res, s_res, s_texel, s_depth,
                      s_mat[0], s_mat[4], s_mat[8], s_mat[12], s_mat[1], s_mat[5], s_mat[9], s_mat[13],
                      s_mat[2], s_mat[6], s_mat[10], s_mat[14]);
            slog(b);
        }
    }
    free(buf); free(out);
    DeleteFileA("tagpu_shadowdump.on");
}

void tagpu_shadow_end(void)
{
    if (!s_live) return;
    dump_map();
    /* the map on the lab's two units for every read-back this frame; the
       sampler bindings are unit state and outlive the frame */
    x_glActiveTexture(GL_TEXTURE0 + UNIT_CMP);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    x_glBindSampler(UNIT_CMP, s_cmp);
    x_glActiveTexture(GL_TEXTURE0 + UNIT_RAW);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    x_glBindSampler(UNIT_RAW, s_raw);
    x_glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

const float* tagpu_shadow_mat(void) { return s_mat; }
int tagpu_shadow_live(void) { return s_live; }

/* the lab's buildUnits: sv = (a + b h) / (h cot el) on the model height,
   svAir the same over the altitude plus the height; `len` throws the
   caster by agl * svAir and scales it by svAir, anything else keeps the
   true altitude and the ground rule's scale */
void tagpu_shadow_caster(float top, float agl, float* throw_, float* sv)
{
    const TAGPU_LIGHT* L = tagpu_classicpp_light();
    float s = 1.0f, sa;
    float cot = s_cot;
    if (cot <= 1e-3f) {                 /* not framed yet: the sun straight up */
        const float* lz = L->shadowSun;
        cot = sqrtf(lz[0] * lz[0] + lz[2] * lz[2]) / (lz[1] > 1e-3f ? lz[1] : 1e-3f);
    }
    if (L->shadowlenOn && top > 0.0f && cot > 1e-3f)
        s = (L->shadowlen[0] + L->shadowlen[1] * top) / (top * cot);
    sa = s;
    if (agl > 0.0f && L->shadowlenOn && cot > 1e-3f)
        sa = (L->shadowlen[0] + L->shadowlen[1] * (agl + top)) / ((agl + top) * cot);
    if (agl > 0.0f && L->airshadow == TAGPU_AIRSHADOW_LEN) { *throw_ = agl * sa; *sv = sa; }
    else { *throw_ = agl; *sv = s; }
}

/* ---- the read-back uniforms of a consumer program ---------------------- */
void tagpu_shadow_locate(unsigned prog, TAGPU_SHADOWU* u)
{
    u->on = glGetUniformLocation(prog, "uShadowOn");
    u->sun = glGetUniformLocation(prog, "uShadowSun");
    u->mat = glGetUniformLocation(prog, "uShadowMat");
    u->scale = glGetUniformLocation(prog, "uShScale");
    u->penumbra = glGetUniformLocation(prog, "uPenumbra");
    u->shade = glGetUniformLocation(prog, "uShade");
    /* the sampler units are fixed for the program's life: named even when
       no map is ever drawn, or GL refuses the draw (tagpu_shadow.h) */
    glUniform1i(glGetUniformLocation(prog, "uShadowCmp"), UNIT_CMP);
    glUniform1i(glGetUniformLocation(prog, "uShadowRaw"), UNIT_RAW);
    if (u->on >= 0) glUniform1i(u->on, 0);
}

void tagpu_shadow_apply(const TAGPU_SHADOWU* u)
{
    const TAGPU_LIGHT* L;
    if (u->on < 0) return;
    glUniform1i(u->on, s_live ? 1 : 0);
    if (!s_live) return;
    L = tagpu_classicpp_light();
    glUniformMatrix4fv(u->mat, 1, GL_FALSE, s_mat);
    x_glUniform3f(u->sun, L->shadowSun[0], L->shadowSun[1], L->shadowSun[2]);
    x_glUniform3f(u->scale, s_texel, s_depth, 1.0f / (float)s_res);
    glUniform1f(u->penumbra, L->penumbra);
    glUniform1f(u->shade, L->shade);
}

void tagpu_shadow_glreset(void)
{
    s_state = 0;
    s_tex = s_fbo = s_cmp = s_raw = s_progU = s_progH = 0;
    s_res = 0;
    s_live = 0;
    s_lastRes = 0; s_lastK = 0; s_lastZoom = -1.0f;
}
