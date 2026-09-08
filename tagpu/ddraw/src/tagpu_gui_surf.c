/* tagpu_gui_surf.c — the twins, the UI atlas, the replay and the layer draw
   (Phase E, G15b). Contract: inc/tagpu_gui.h; queue: tagpu_gui_int.h.
   Design: research/notes/gui-renderer.md 3.2-3.6.

   RENDER THREAD ONLY. Every engine surface the publisher has seeded gets a
   twin: an RG8 texture the surface's size (R = the palette index, G = the
   coverage: 1 where a replayed op wrote, 0 where the viewport's key fill
   erased) behind an FBO. The queue's ops are drained at every present, in
   order: SEED uploads the bytes, PIXELS uploads a box, SPRITE draws a quad
   from the UI atlas with the colour key discarded, COPY draws a quad sampling
   another twin, CLEAR clears a box to coverage 0, FREE drops the twin, RESET
   drops them all. Then the presented surface's twin is drawn over the frame,
   palette-resolved through the live palette, wherever its coverage is set —
   after the world's composite, so UI is above the world and the engine's own
   pixels remain the fallback beneath (the composite's key rule, unchanged).

   `strict` turns the fallback off for the harness: where the engine's surface
   holds a UI pixel (outside the viewport, or a non-key pixel inside it) and
   the twin holds nothing, magenta; the cursor's rect is exempt.

   The twin is 1:1 and NEAREST; its size is the surface's × k with k = 1
   (gui-renderer.md 6 — phase 2 raises k). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_gui.h"
#include "tagpu_gui_int.h"
#include "tagpu_gaf.h"
#include "tagpu_vpwide.h"
#include "tagpu_terr.h"
#include "tagpu_overlay.h"
#include "opengl_utils.h"
#include "dd.h"                         /* g_ddraw.primary->palette: the palette the engine's frame is PRESENTED with */
#include "IDirectDrawSurface.h"
#include "IDirectDrawPalette.h"

#define TA_MAINPP      0x00511DE8u
#define OFF_PALETTE    0x143A7          /* 256 x {R,G,B,pad}                   */
#define GFX_GLOBALS_PP 0x0051FBD0u
#define MOUSE_POS_X    0x1B6            /* the cursor's last drawn position    */
#define MOUSE_POS_Y    0x1BA
#define MOUSE_SPRITE   0x1B2            /* -> record: u16 w, u16 h, s16 hotspots */
#define POLL_MS        500
#define MAX_TWINS      32
#define ATLAS_DIM      2048
#define ATLAS_MAX      4096

extern volatile int g_gui_draw;         /* tagpu_gui_hook.c: the publisher's gate */

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_FBTEX2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFN_UNIFORM4F)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2IV)(GLint, GLsizei, const GLint*);
typedef void (APIENTRY *PFN_SCISSOR)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_BLENDFUNC)(GLenum, GLenum);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_CLEARCOLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_CLEAR)(GLbitfield);
static PFN_CLEARCOLOR x_glClearColor;
static PFN_CLEAR      x_glClear;
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_FBTEX2D    x_glFramebufferTexture2D;
static PFN_UNIFORM4F  x_glUniform4f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_UNIFORM2IV x_glUniform2iv;
static PFN_SCISSOR    x_glScissor;
static PFN_DISABLE    x_glDisable;
static PFN_BLENDFUNC  x_glBlendFunc;
static PFN_ACTIVETEX  x_glActiveTexture;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll"); if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}
static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}
static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

/* ------------------------------------------------------------------ state */
typedef struct TWIN {
    unsigned surf;                      /* the engine surface's pixel base     */
    int w, h;
    GLuint tex, fbo;
} TWIN;
static TWIN   s_twins[MAX_TWINS];
static int    s_ntwins = 0;
static unsigned s_presented = 0;        /* the last PK_FRAME's surface         */

static int    s_gl = 0;                 /* 0 none, 1 ready, 2 failed           */
static GLuint s_sprProg, s_cpyProg, s_layProg, s_vao, s_vbo, s_palTex;
static GLint  s_uSprSize, s_uSprCK;
static GLint  s_uCpySize, s_uCpyOff;
static GLint  s_uLaySize, s_uLayStrict, s_uLayKey, s_uLayVp, s_uLayCursor;
static TAGPU_GAFATLAS s_atlas;
static TAGPU_GAFENT   s_ents[ATLAS_MAX];
static unsigned char* s_rg;             /* interleave scratch, 2 bytes per texel */
static unsigned s_rgCap = 0;
static unsigned char s_palCopy[1024];

static int    s_on = 0, s_strict = 0;
static DWORD  s_lastPoll = 0;
static unsigned s_drained = 0, s_sprites = 0, s_copies = 0, s_pixels = 0, s_seeds = 0, s_clears = 0, s_lostSprites = 0;
static int    s_skipToReset = 0;        /* after a GL context change: the queue's ops up to the producer's next
                                           RESET were published against twins and an atlas that died with the
                                           context — take their arena bytes, apply nothing (see drain) */
static unsigned s_skipped = 0;
static unsigned s_palChanges = 0;       /* presented-palette uploads (a fade is a run of them)             */
static int    s_palDiff = 0;            /* entries where the presented palette differs from main+0x143A7  */
static int    s_palDiffAt = -1;         /* the first such entry                                            */
static int    s_palSource = 0;          /* 1 = cnc-ddraw's palette object, 0 = the engine's table (no primary yet) */

/* ----------------------------------------------------------------- shaders */
/* a quad in surface pixels -> the twin's FBO (row 0 = surface row 0) */
static const char* QVS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a;\n"          /* x, y, u, v */
    "uniform vec2 uSize;\n"
    "out vec2 uv;\n"
    "void main(){ uv = a.zw;\n"
    "  gl_Position = vec4(a.x / uSize.x * 2.0 - 1.0, a.y / uSize.y * 2.0 - 1.0, 0.0, 1.0); }\n";
/* the sprite: the atlas index, the colour key discarded, coverage 1 */
static const char* SPR_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uAtlas; uniform int uCK;\n"
    "void main(){ float i = texture(uAtlas, uv).r;\n"
    "  if (int(i * 255.0 + 0.5) == uCK) discard;\n"
    "  frag = vec4(i, 1.0, 0.0, 0.0); }\n";
/* the copy: the source twin's index at (this pixel - offset), coverage 1 */
static const char* CPY_FS =
    "#version 330 core\n"
    "out vec4 frag;\n"
    "uniform sampler2D uSrc; uniform ivec2 uOff;\n"
    "void main(){ ivec2 p = ivec2(gl_FragCoord.xy) - uOff;\n"
    "  vec2 g = texelFetch(uSrc, p, 0).rg;\n"
    "  frag = vec4(g.r, 1.0, 0.0, 0.0); }\n";
/* the layer over the frame: uv.y = 0 at the top of the screen, like the
   native composite's CVS; the twin's row 0 is the surface's row 0 */
static const char* LAY_VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a;\n"
    "out vec2 uv;\n"
    "void main(){ uv = a.xy;\n"
    "  gl_Position = vec4(a.x*2.0-1.0, 1.0-a.y*2.0, 0.0, 1.0); }\n";
static const char* LAY_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uTwin; uniform sampler2D uPal; uniform sampler2D uSurf;\n"
    "uniform ivec2 uSize; uniform int uStrict; uniform int uKey; uniform vec4 uVp; uniform vec4 uCursor;\n"
    "void main(){\n"
    "  ivec2 p = clamp(ivec2(uv * vec2(uSize)), ivec2(0), uSize - 1);\n"
    "  vec2 f = vec2(p);\n"
    /* THE CURSOR IS THE ENGINE'S (gui-renderer.md 3.7): it is blitted onto the
       primary after everything we observe, so it exists only in the engine's
       frame. Its rect is left to that frame — the twin is not drawn there —
       and the same rect is exempt from `strict`. */
    "  bool cur = f.x >= uCursor.x && f.x < uCursor.x + uCursor.z && f.y >= uCursor.y && f.y < uCursor.y + uCursor.w;\n"
    "  if (cur) discard;\n"
    "  vec2 g = texelFetch(uTwin, p, 0).rg;\n"
    "  if (g.g > 0.5) { frag = vec4(texture(uPal, vec2((g.r * 255.0 + 0.5) / 256.0, 0.5)).rgb, 1.0); return; }\n"
    "  if (uStrict == 1) {\n"
    "    int e = int(texelFetch(uSurf, p, 0).r * 255.0 + 0.5);\n"
    "    bool inVp = f.x >= uVp.x && f.x < uVp.x + uVp.z && f.y >= uVp.y && f.y < uVp.y + uVp.w;\n"
    "    if (!inVp || e != uKey) { frag = vec4(1.0, 0.0, 1.0, 1.0); return; }\n"
    "  }\n"
    "  discard; }\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg); slog("gui: shader FAILED:"); slog(lg); s_gl = 2; }
    return sh;
}
static GLuint mkprog(const char* vs, const char* fs)
{
    GLuint v = mksh(GL_VERTEX_SHADER, vs), f = mksh(GL_FRAGMENT_SHADER, fs), p;
    GLint ok = 0;
    if (s_gl == 2) return 0;
    p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v); glDeleteShader(f);
    if (!ok) { slog("gui: program link FAILED"); s_gl = 2; return 0; }
    return p;
}

static int init_gl(void)
{
    if (s_gl) return s_gl == 1;
    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glFramebufferTexture2D = (PFN_FBTEX2D)getgl("glFramebufferTexture2D");
    x_glUniform4f  = (PFN_UNIFORM4F)getgl("glUniform4f");
    x_glUniform2f  = (PFN_UNIFORM2F)getgl("glUniform2f");
    x_glUniform2iv = (PFN_UNIFORM2IV)getgl("glUniform2iv");
    x_glScissor    = (PFN_SCISSOR)getgl("glScissor");
    x_glDisable    = (PFN_DISABLE)getgl("glDisable");
    x_glBlendFunc  = (PFN_BLENDFUNC)getgl("glBlendFunc");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glClearColor = (PFN_CLEARCOLOR)getgl("glClearColor");
    x_glClear      = (PFN_CLEAR)getgl("glClear");
    if (!x_glDrawArrays || !x_glFramebufferTexture2D || !x_glUniform4f || !x_glUniform2f ||
        !x_glUniform2iv || !x_glScissor || !x_glDisable || !x_glBlendFunc || !x_glActiveTexture ||
        !x_glClearColor || !x_glClear) {
        slog("gui: GL entry points missing"); s_gl = 2; return 0;
    }
    s_sprProg = mkprog(QVS, SPR_FS);
    s_cpyProg = mkprog(QVS, CPY_FS);
    s_layProg = mkprog(LAY_VS, LAY_FS);
    if (s_gl == 2) return 0;
    glUseProgram(s_sprProg);
    glUniform1i(glGetUniformLocation(s_sprProg, "uAtlas"), 0);
    s_uSprSize = glGetUniformLocation(s_sprProg, "uSize");
    s_uSprCK   = glGetUniformLocation(s_sprProg, "uCK");
    glUseProgram(s_cpyProg);
    glUniform1i(glGetUniformLocation(s_cpyProg, "uSrc"), 0);
    s_uCpySize = glGetUniformLocation(s_cpyProg, "uSize");
    s_uCpyOff  = glGetUniformLocation(s_cpyProg, "uOff");
    glUseProgram(s_layProg);
    glUniform1i(glGetUniformLocation(s_layProg, "uTwin"), 0);
    glUniform1i(glGetUniformLocation(s_layProg, "uPal"),  1);
    glUniform1i(glGetUniformLocation(s_layProg, "uSurf"), 2);
    s_uLaySize   = glGetUniformLocation(s_layProg, "uSize");
    s_uLayStrict = glGetUniformLocation(s_layProg, "uStrict");
    s_uLayKey    = glGetUniformLocation(s_layProg, "uKey");
    s_uLayVp     = glGetUniformLocation(s_layProg, "uVp");
    s_uLayCursor = glGetUniformLocation(s_layProg, "uCursor");
    glUseProgram(0);
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float) * 256, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glGenTextures(1, &s_palTex);
    glBindTexture(GL_TEXTURE_2D, s_palTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    memset(s_palCopy, 0xFF, sizeof s_palCopy);
    memset(&s_atlas, 0, sizeof s_atlas);
    s_atlas.ents = s_ents; s_atlas.max = ATLAS_MAX; s_atlas.dim = ATLAS_DIM; s_atlas.tag = "gui";
    s_atlas.pad = 0; s_atlas.align = 0; s_atlas.mip = 0;      /* 1:1, NEAREST, the 1-texel border */
    if (!tagpu_gaf_atlas_create(&s_atlas)) { slog("gui: atlas FAILED"); s_gl = 2; return 0; }
    s_gl = 1;
    slog("gui: GL ready (twins RG8, atlas 2048x2048, layer over the composite)");
    return 1;
}

/* ------------------------------------------------------------------ twins */
static TWIN* twin_find(unsigned surf)
{
    int i;
    for (i = 0; i < s_ntwins; i++) if (s_twins[i].surf == surf) return &s_twins[i];
    return NULL;
}
static void twin_drop(TWIN* t)
{
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->tex) glDeleteTextures(1, &t->tex);
    *t = s_twins[--s_ntwins];
}
static TWIN* twin_make(unsigned surf, int w, int h)
{
    TWIN* t = twin_find(surf);
    GLenum st;
    if (t && (t->w != w || t->h != h)) { twin_drop(t); t = NULL; }
    if (t) return t;
    if (s_ntwins >= MAX_TWINS) twin_drop(&s_twins[0]);        /* the oldest goes */
    t = &s_twins[s_ntwins++];
    memset(t, 0, sizeof *t);
    t->surf = surf; t->w = w; t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    x_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    /* a fresh twin covers nothing until its seed lands */
    x_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    x_glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char b[120]; _snprintf(b, sizeof b, "gui: twin %08X %dx%d FBO incomplete (%x)", surf, w, h, (unsigned)st); slog(b);
    }
    return t;
}

/* rows of indices -> (index, 255) pairs -> the twin's box */
static void twin_upload(TWIN* t, int l, int tp, int w, int h, const unsigned char* idx)
{
    unsigned n = (unsigned)w * (unsigned)h, i;
    if (l < 0 || tp < 0 || l + w > t->w || tp + h > t->h || w <= 0 || h <= 0) return;
    if (n * 2 > s_rgCap) {
        free(s_rg); s_rgCap = n * 2 + 65536; s_rg = (unsigned char*)malloc(s_rgCap);
        if (!s_rg) { s_rgCap = 0; return; }
    }
    for (i = 0; i < n; i++) { s_rg[2 * i] = idx[i]; s_rg[2 * i + 1] = 255; }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, l, tp, w, h, GL_RG, GL_UNSIGNED_BYTE, s_rg);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void quad(float* v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    float q[24] = { x0,y0,u0,v0,  x1,y0,u1,v0,  x0,y1,u0,v1,   x0,y1,u0,v1,  x1,y0,u1,v0,  x1,y1,u1,v1 };
    memcpy(v, q, sizeof q);
}

static void twin_sprite(TWIN* t, const TAGPU_GAFENT* e, const TAGPU_PUBOP* o)
{
    float v[24];
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glEnable(GL_SCISSOR_TEST);
    x_glScissor(o->l, o->t, o->r - o->l + 1, o->b - o->t + 1);
    glUseProgram(s_sprProg);
    x_glUniform2f(s_uSprSize, (float)t->w, (float)t->h);
    glUniform1i(s_uSprCK, (int)o->ck);
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_atlas.tex);
    quad(v, (float)o->sl, (float)o->st, (float)(o->sl + o->fw), (float)(o->st + o->fh), e->u0, e->v0, e->u1, e->v1);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    x_glDrawArrays(GL_TRIANGLES, 0, 6);
    x_glDisable(GL_SCISSOR_TEST);
    s_sprites++;
}

static void twin_copy(TWIN* t, const TWIN* src, const TAGPU_PUBOP* o)
{
    float v[24];
    GLint off[2];
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glEnable(GL_SCISSOR_TEST);
    x_glScissor(o->l, o->t, o->r - o->l + 1, o->b - o->t + 1);
    glUseProgram(s_cpyProg);
    x_glUniform2f(s_uCpySize, (float)t->w, (float)t->h);
    off[0] = o->l - o->sl; off[1] = o->t - o->st;      /* dst pixel - src pixel */
    x_glUniform2iv(s_uCpyOff, 1, off);
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src->tex);
    quad(v, (float)o->l, (float)o->t, (float)(o->r + 1), (float)(o->b + 1), 0, 0, 0, 0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    x_glDrawArrays(GL_TRIANGLES, 0, 6);
    x_glDisable(GL_SCISSOR_TEST);
    s_copies++;
}

static void twin_clear(TWIN* t, const TAGPU_PUBOP* o)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glEnable(GL_SCISSOR_TEST);
    x_glScissor(o->l, o->t, o->r - o->l + 1, o->b - o->t + 1);
    x_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    x_glClear(GL_COLOR_BUFFER_BIT);
    x_glDisable(GL_SCISSOR_TEST);
    s_clears++;
}

static void twins_reset(void)
{
    while (s_ntwins) twin_drop(&s_twins[0]);
    s_presented = 0;
    tagpu_gaf_atlas_reset(&s_atlas);
}

/* ------------------------------------------------------------------ drain */
static void drain(void)
{
    unsigned tail = g_guiq.qTail, head = g_guiq.qHead;
    int budget = 20000;                    /* ops per present: a burst is many flips */
    while (tail != head && budget-- > 0) {
        const TAGPU_PUBOP* o = &g_guiq.ops[tail & (TAGPU_GUI_QCAP - 1)];
        TWIN* t;
        if (s_skipToReset && o->kind != PK_RESET) {
            /* published before the producer learned the context was gone:
               every twin and atlas entry it names is dead, and applying it
               would only count its sprites as lost (MEASURED 2026-09-07: 705
               per game -> shell switch). The producer's RESET follows at its
               next publish, since glreset raised `reseed`. */
            s_skipped++;
            goto next;
        }
        switch (o->kind) {
        case PK_FRAME:  s_presented = o->surf; break;
        case PK_RESET:  twins_reset(); s_skipToReset = 0; break;
        case PK_SEED:
            t = twin_make(o->surf, o->w, o->h);
            if (t && o->alen) twin_upload(t, 0, 0, o->w, o->h, g_guiq.arena + o->aoff);
            s_seeds++;
            break;
        case PK_FREE:
            t = twin_find(o->surf); if (t) twin_drop(t);
            break;
        case PK_CLEAR:
            t = twin_find(o->surf); if (t) twin_clear(t, o);
            break;
        case PK_PIXELS:
            t = twin_find(o->surf);
            if (t && o->alen) { twin_upload(t, o->l, o->t, o->r - o->l + 1, o->b - o->t + 1, g_guiq.arena + o->aoff); s_pixels++; }
            break;
        case PK_SPRITE: {
            const TAGPU_GAFENT* e;
            t = twin_find(o->surf);
            if (!t) break;
            e = tagpu_gaf_atlas_find(&s_atlas, o->frame, o->pix, o->fw, o->fh);
            if (!e && o->alen) e = tagpu_gaf_atlas_put(&s_atlas, o->frame, o->pix, o->fw, o->fh, o->ck, g_guiq.arena + o->aoff);
            if (!e) {
                /* the atlas is full, or the frame's bytes never arrived (an
                   earlier reset lost them): a fresh start — the seeds carry
                   the pixels the sprite would have drawn */
                g_guiq.why = s_atlas.full ? TAGPU_GUI_WHY_ATLAS : TAGPU_GUI_WHY_LOST;
                if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
                g_guiq.reseed = 1;
                if (s_lostSprites < 8) {
                    char b[200];
                    _snprintf(b, sizeof b, "gui: lost sprite #%u: frame %08X %ux%u bytes=%u atlas=%d/%d%s twins=%d flip=%u",
                              s_lostSprites + 1, (unsigned)(size_t)o->frame, (unsigned)o->fw, (unsigned)o->fh, o->alen,
                              s_atlas.n, s_atlas.max, s_atlas.full ? " FULL" : "", s_ntwins, o->flip);
                    slog(b);
                }
                s_lostSprites++;
                break;
            }
            twin_sprite(t, e, o);
            break; }
        case PK_COPY: {
            TWIN* src = twin_find(o->src);
            t = twin_find(o->surf);
            if (t && src) twin_copy(t, src, o);
            else if (t) { g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_COPY; }
            break; }
        default: break;
        }
    next:
        if (o->alen) g_guiq.aTail = o->aoff + o->alen;
        tail++;
        s_drained++;
    }
    MemoryBarrier();
    g_guiq.qTail = tail;
}

/* ------------------------------------------------------------------ layer */
/* THE PALETTE THE ENGINE'S FRAME IS SHOWN WITH is not main+0x143A7. Every
   palette the engine sets goes through 0x4BA200(entries, first, count), which
   keeps the entries in the graphics globals (+0x214) and hands DirectDraw
   min(255, entry x gamma) with gamma = *(float*)(globals+0x614) — the Gamma
   option (SetGamma 0x4BA590 [CORPUS], gamma = 0.5 + Gamma/24, 1.0 at the
   default 12), applied only on the way to SetEntries, never to +0x143A7 (read
   2026-09-07, engine map "The palette the screen is presented with"). The
   engine's own pixels beneath the twin are drawn by cnc-ddraw through the
   palette its SetEntries received, so that is the palette the twin resolves
   through: the primary's palette object in this DLL. The engine's table is
   the fallback until a primary exists, and the number of entries where the
   two disagree is measured at every upload (`paldiff=` in the heartbeat):
   0 at Gamma 12, and the world passes, which read +0x143A7, are wrong by
   exactly that much at any other setting. */
static void upload_palette(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const unsigned char* engine = NULL;
    unsigned char rgba[1024];
    int i;
    int havePresented = 0;
    if (ptr_ok(ta) && ptr_ok(ta + OFF_PALETTE)) engine = (const unsigned char*)(ta + OFF_PALETTE);
    /* under the fork's lock: the game thread NULLs g_ddraw.primary inside it
       when the primary's last reference goes (IDirectDrawSurface__Release),
       and frees the object only after leaving it — so a pointer read and
       dereferenced inside the section is a live object or NULL, never a
       freed one. The present itself runs outside the section. */
    EnterCriticalSection(&g_ddraw.cs);
    if (g_ddraw.primary && g_ddraw.primary->palette) {
        const RGBQUAD* q = g_ddraw.primary->palette->data_rgb;
        for (i = 0; i < 256; i++) { rgba[4*i] = q[i].rgbRed; rgba[4*i+1] = q[i].rgbGreen; rgba[4*i+2] = q[i].rgbBlue; rgba[4*i+3] = 255; }
        havePresented = 1;
    }
    LeaveCriticalSection(&g_ddraw.cs);
    if (havePresented) {
        s_palSource = 1;
    } else if (engine) {
        memcpy(rgba, engine, 1024);
        s_palSource = 0;
    } else return;
    if (memcmp(rgba, s_palCopy, 1024) == 0) return;
    memcpy(s_palCopy, rgba, 1024);
    s_palChanges++;
    s_palDiff = 0; s_palDiffAt = -1;
    if (engine)
        for (i = 0; i < 256; i++)
            if (rgba[4*i] != engine[4*i] || rgba[4*i+1] != engine[4*i+1] || rgba[4*i+2] != engine[4*i+2]) {
                if (s_palDiffAt < 0) s_palDiffAt = i;
                s_palDiff++;
            }
    glBindTexture(GL_TEXTURE_2D, s_palTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, s_palCopy);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void cursor_rect(float* r)
{
    const char* g = *(const char* const*)GFX_GLOBALS_PP;
    const unsigned short* rec;
    r[0] = r[1] = -1.0f; r[2] = r[3] = 0.0f;
    if (!ptr_ok(g)) return;
    rec = *(const unsigned short* const*)(g + MOUSE_SPRITE);
    r[0] = (float)*(const int*)(g + MOUSE_POS_X);
    r[1] = (float)*(const int*)(g + MOUSE_POS_Y);
    if (ptr_ok(rec)) { r[2] = (float)rec[0]; r[3] = (float)rec[1]; }
    else { r[2] = 64.0f; r[3] = 64.0f; }
}

static void draw_layer(const TAGPU_FRAME* f)
{
    TWIN* t = s_presented ? twin_find(s_presented) : NULL;
    float v[24], cur[4];
    GLint sz[2];
    const char* ta = *(const char* const*)TA_MAINPP;
    int L = 0, T = 0, W = 0, H = 0, key;
    if (!t || t->w != f->game_width || t->h != f->game_height) return;
    upload_palette();
    tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
    key = tagpu_terr_key();
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());
    glViewport(f->vp_x, f->vp_y, f->vp_w, f->vp_h);
    x_glDisable(GL_BLEND);
    x_glDisable(GL_DEPTH_TEST);
    glUseProgram(s_layProg);
    sz[0] = t->w; sz[1] = t->h;
    x_glUniform2iv(s_uLaySize, 1, sz);
    glUniform1i(s_uLayStrict, s_strict && f->surface_tex ? 1 : 0);
    glUniform1i(s_uLayKey, key);
    x_glUniform4f(s_uLayVp, (float)L, (float)T, (float)W, (float)H);
    cursor_rect(cur);
    x_glUniform4f(s_uLayCursor, cur[0], cur[1], cur[2], cur[3]);
    x_glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (GLuint)f->surface_tex);
    x_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_palTex);
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    quad(v, 0, 0, 1, 1, 0, 0, 0, 0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    x_glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* leave nothing of ours bound: the drain binds twin FBOs, the atlas, the
   copy source and our VAO/program, and draw_layer may not have run */
static void unbind_all(void)
{
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    x_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

/* ---------------------------------------------------------------- present */
static void poll(void)
{
    DWORD t = GetTickCount();
    char buf[256];
    DWORD n = 0;
    HANDLE h;
    int on;
    if (s_lastPoll && t - s_lastPoll < POLL_MS) return;
    s_lastPoll = t;
    h = CreateFileA("tagpu_gui.on", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    on = h != INVALID_HANDLE_VALUE;
    if (on) { if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0; CloseHandle(h); buf[n] = 0; }
    else buf[0] = 0;
    /* `off` inside the file also turns the draw off, so the detours can stay
       installed (they need the file at attach) while the layer is A/B'd */
    if (on && strstr(buf, "off")) on = 0;
    s_strict = on && strstr(buf, "strict") != NULL;
    if (on != s_on) {
        s_on = on;
        g_gui_draw = on;
        if (on) { g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_ARM; }   /* the twins start from the surfaces as they are */
        slog(on ? "gui: layer ON (twins re-seed at the next flip)" : "gui: layer OFF (the frame is the engine's)");
    }
}

void tagpu_gui_present(const TAGPU_FRAME* f)
{
    static unsigned last = 0;
    if (!tagpu_gui_installed() || !f) return;
    poll();
    if (!s_on) {
        /* off: nothing is published, but drain whatever was */
        g_guiq.qTail = g_guiq.qHead; g_guiq.aTail = g_guiq.aHead;
        return;
    }
    if (!init_gl()) return;
    drain();
    draw_layer(f);
    unbind_all();
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());
    glViewport(f->vp_x, f->vp_y, f->vp_w, f->vp_h);
    if (f->frame_counter - last >= 300) {
        char b[260];
        static LARGE_INTEGER t0, fq;
        LARGE_INTEGER t1;
        double fps = 0.0;
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t1);
        if (t0.QuadPart) fps = (double)(f->frame_counter - last) * (double)fq.QuadPart / (double)(t1.QuadPart - t0.QuadPart);
        t0 = t1;
        last = f->frame_counter;
        _snprintf(b, sizeof b, "gui: twins=%d presented=%08X drained=%u seeds=%u sprites=%u copies=%u pixels=%u clears=%u atlas=%d/%d lost=%u strict=%d resets=%u overflows=%u stalls=%u skipped=%u palchg=%u paldiff=%d@%d palsrc=%d fps=%.1f",
                  s_ntwins, s_presented, s_drained, s_seeds, s_sprites, s_copies, s_pixels, s_clears,
                  s_atlas.n, s_atlas.max, s_lostSprites, s_strict, g_guiq.resets, g_guiq.overflows, g_guiq.stalls,
                  s_skipped, s_palChanges, s_palDiff, s_palDiffAt, s_palSource, fps);
        slog(b);
    }
}

void tagpu_gui_glreset(void)
{
    /* the context is gone: forget every id, start over from fresh seeds —
       and take nothing from the queue until the producer's RESET arrives */
    s_ntwins = 0; s_presented = 0;
    s_gl = 0; s_sprProg = s_cpyProg = s_layProg = s_vao = s_vbo = s_palTex = 0;
    tagpu_gaf_atlas_lost(&s_atlas);
    memset(s_palCopy, 0xFF, sizeof s_palCopy);        /* the palette texture died too: re-upload */
    s_skipToReset = 1;
    g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_GLCTX;
}

int tagpu_gui_drawing(void) { return s_on; }
