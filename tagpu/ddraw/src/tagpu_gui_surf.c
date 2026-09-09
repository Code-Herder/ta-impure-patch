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

   CLASSIC++ (G15e). Beside the index twin every surface may carry a COLOUR
   twin -- GL_RGBA8, the same size, COLOR_ATTACHMENT1 on the same FBO -- into
   which a sprite op writes the UI atlas's RESTORED texel where the atlas has
   one (alpha 1) and zero elsewhere. A copy carries both channels, which is
   what makes restored art survive the panel's `panel+0xBC` -> frame blit
   (gui-renderer.md 13.2: colour has to be per surface, because art is drawn
   into a retained surface long before it has a screen position). A seed or a
   pixel op invalidates the colour of its box -- those carry indices only --
   so the layer falls back to the palette there. The layer takes colour where
   alpha is 1 and the palette elsewhere, which is per texel, so a partly
   restored surface is never half-wrong.

   THE PALETTE-VALIDITY RULE (gui-renderer.md 3.4). The restore job SNAPSHOTS
   the palette into a texture when it is created, so its colours are only
   right while that palette is still the one we present with. Every frame the
   two are compared: while they differ the colour twins are ignored and the
   frame is indexed -- a fade shows dithered art, never wrong art -- and once
   the palette has been still for PAL_SETTLE frames the atlas is re-armed
   against the new one and every colour twin is invalidated so the art comes
   back restored as it is redrawn.

   What restores and what does not: colour reaches a twin only through a
   sprite op, so anything SEEDED (a surface adopted whole from the engine's
   bytes) stays indexed until the engine redraws it. The shell redraws every
   gadget on every flip; the in-game panel repaints on a mode switch or when
   something marks it dirty.

   THE SEAM (G17a, gui-renderer.md 13.2-13.3). The twin stays 1:1 with the
   engine's surface — same ops, same seeds, same census, so phase 1's `strict`
   walk goes on meaning what it meant. What changed is how it reaches the
   screen. The composite is now three layers, top down: the SHARP LAYER (one
   RGBA8 texture at the DEVICE resolution, drawn from live state at present
   time — empty until the cursor and the string op fill it), then the mirror
   scaled by a SHARP-BILINEAR ramp one device pixel wide, then the engine's own
   frame as the fallback. k is device pixels per twin texel, read off the frame
   rather than configured, and it is 1.0 wherever the engine's screen is the
   window — which is every path phase 1 has; giving the engine window / k is
   G17b's. At k = 1 the ramp is exactly one source texel wide, so every output
   pixel samples a texel centre and the frame is what texelFetch gave: that
   identity is the gate, not a hope. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_gui.h"
#include "tagpu_opt.h"
#include "tagpu_gui_int.h"
#include "tagpu_gaf.h"
#include "tagpu_classicpp.h"
#include "tagpu_restoreglsl.h"
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
#define UI_RESTORE_PRIO 4       /* terrain 0, features 1, effects 2, 3DO units 3 */
#define UI_RESTORE_MIN 12       /* G15-0's verdict: nothing under 12x12 is restored */
#define PAL_SETTLE     30       /* frames the palette must hold still before a re-arm */

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
typedef void (APIENTRY *PFN_CLEARBUFFERFV)(GLenum, GLint, const GLfloat*);
typedef void (APIENTRY *PFN_DRAWBUFFERS)(GLsizei, const GLenum*);
static PFN_CLEARBUFFERFV x_glClearBufferfv;
static PFN_DRAWBUFFERS   x_glDrawBuffers;
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
    GLuint rgb;                         /* Classic++: the colour twin, or 0    */
} TWIN;
static TWIN   s_twins[MAX_TWINS];
static int    s_ntwins = 0;
static unsigned s_presented = 0;        /* the last PK_FRAME's surface         */

static int    s_gl = 0;                 /* 0 none, 1 ready, 2 failed           */
static GLuint s_sprProg, s_cpyProg, s_layProg, s_vao, s_vbo, s_palTex;
static GLint  s_uSprSize, s_uSprCK, s_uSprRestored;
static GLint  s_uCpySize, s_uCpyOff, s_uCpyHasCol;
static GLint  s_uLaySize, s_uLayStrict, s_uLayKey, s_uLayVp, s_uLayCursor, s_uLayColOn;
static GLint  s_uLayScale, s_uLaySharpSize, s_uLaySharpOn;
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
/* Classic++ (G15e) */
static int    s_norestore = 0;          /* `norestore` in the trigger: the A/B lever   */
static int    s_colValid = 0;           /* the colour twins may be sampled this frame  */
static unsigned char s_restorePal[1024];/* the palette the atlas's restore snapshotted */
static unsigned s_palSeen = 0;          /* s_palChanges when the settle count last moved */
static int    s_palSettle = 0;
static unsigned s_rearms = 0, s_colTwins = 0;
static unsigned s_rglslSeen = 0;        /* tagpu_rglsl_calls() at the last present */
/* Phase 2's seam (G17a) */
static GLuint s_sharpTex, s_sharpFbo;   /* the sharp layer: device res, RGBA8, row 0 the viewport's TOP */
static int    s_sharpW, s_sharpH;       /* its size, = the frame's viewport in window px               */
static int    s_sharpOn = 0;            /* it exists and may be sampled this frame                     */
static int    s_sharptest = 0;          /* the harness lever that proves the layer is wired            */
static float  s_k = 1.0f;               /* device px per twin texel: 13.1's k, and the ramp's width    */

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
    "in vec2 uv;\n"
    "layout(location=0) out vec4 oIdx;\n"
    "layout(location=1) out vec4 oCol;\n"
    "uniform sampler2D uAtlas; uniform sampler2D uAtlasRGB;\n"
    "uniform int uCK; uniform int uRestored;\n"
    "void main(){ float i = texture(uAtlas, uv).r;\n"
    "  if (int(i * 255.0 + 0.5) == uCK) discard;\n"
    "  oIdx = vec4(i, 1.0, 0.0, 0.0);\n"
    /* the restored twin is alpha 0 where the frame is keyed and where its
       restore has not landed yet, so alpha IS 'this texel has colour'. The
       UI atlas is pad 0 / mip 0, so alpha is 0 or 1 and t.rgb needs no
       un-premultiply (the unit atlas's mipped twin is the one that does). */
    "  oCol = vec4(0.0);\n"
    "  if (uRestored != 0) { vec4 t = texture(uAtlasRGB, uv);\n"
    "    if (t.a > 0.5) oCol = vec4(t.rgb, 1.0); } }\n";
/* the copy: the source twin's index at (this pixel - offset), coverage 1 */
static const char* CPY_FS =
    "#version 330 core\n"
    "layout(location=0) out vec4 oIdx;\n"
    "layout(location=1) out vec4 oCol;\n"
    "uniform sampler2D uSrc; uniform sampler2D uSrcCol;\n"
    "uniform ivec2 uOff; uniform int uSrcHasCol;\n"
    "void main(){ ivec2 p = ivec2(gl_FragCoord.xy) - uOff;\n"
    "  vec2 g = texelFetch(uSrc, p, 0).rg;\n"
    "  oIdx = vec4(g.r, 1.0, 0.0, 0.0);\n"
    /* a source with no colour twin contributes none: writing 0 INVALIDATES
       the destination's colour there, which is what a copy from an indexed
       surface means */
    "  oCol = uSrcHasCol != 0 ? texelFetch(uSrcCol, p, 0) : vec4(0.0); }\n";
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
    "uniform sampler2D uTwinCol; uniform sampler2D uSharp;\n"
    "uniform int uColOn; uniform int uSharpOn;\n"
    "uniform ivec2 uSize; uniform ivec2 uSharpSize; uniform vec2 uScale;\n"
    "uniform int uStrict; uniform int uKey; uniform vec4 uVp; uniform vec4 uCursor;\n"
    /* ONE TAP OF THE MIRROR, premultiplied by its coverage. rgb is the
       restored colour where this texel has one and the live palette
       everywhere else (the per-texel rule of 3.4, unchanged); a is coverage,
       so an uncovered texel contributes NOTHING to a blend instead of
       dragging index 0 in from a box the key fill erased. */
    "vec4 tap(ivec2 p){\n"
    "  p = clamp(p, ivec2(0), uSize - 1);\n"
    "  vec2 g = texelFetch(uTwin, p, 0).rg;\n"
    "  if (g.g <= 0.5) return vec4(0.0);\n"
    "  if (uColOn != 0) { vec4 c = texelFetch(uTwinCol, p, 0);\n"
    "    if (c.a > 0.5) return vec4(c.rgb, 1.0); }\n"
    "  return vec4(texture(uPal, vec2((g.r * 255.0 + 0.5) / 256.0, 0.5)).rgb, 1.0); }\n"
    "void main(){\n"
    "  ivec2 p = clamp(ivec2(uv * vec2(uSize)), ivec2(0), uSize - 1);\n"
    "  vec2 f = vec2(p);\n"
    /* THE CURSOR IS THE ENGINE'S (gui-renderer.md 3.7): it is blitted onto the
       primary after everything we observe, so it exists only in the engine's
       frame. Its rect is left to that frame — the twin is not drawn there —
       and the same rect is exempt from `strict`. */
    "  bool cur = f.x >= uCursor.x && f.x < uCursor.x + uCursor.z && f.y >= uCursor.y && f.y < uCursor.y + uCursor.w;\n"
    "  if (cur) discard;\n"
    /* THE SHARP LAYER (gui-renderer.md 13.2), top of the composite: device
       resolution, drawn from live state at present time, row 0 the viewport's
       TOP row. Alpha is its coverage; G17a leaves it empty, so this branch is
       taken by nothing until the cursor (G17c) and the string op (G17d) fill
       it, and `sharptest` is what proves it is wired at all. */
    "  if (uSharpOn != 0) {\n"
    "    ivec2 sp = clamp(ivec2(uv * vec2(uSharpSize)), ivec2(0), uSharpSize - 1);\n"
    "    vec4 sh = texelFetch(uSharp, sp, 0);\n"
    "    if (sh.a > 0.5) { frag = vec4(sh.rgb, 1.0); return; }\n"
    "  }\n"
    /* THE 1x MIRROR, SCALED BY THE SHARP-BILINEAR RAMP (gui-renderer.md 13.3).
       A 4-tap whose weight ramps across ONE DEVICE PIXEL — flat inside a
       texel, steep across its boundary — so a 1-px bevel does not stagger
       between one and two device pixels at a fractional k the way nearest
       does. It runs AFTER the palette lookup because interpolating indices is
       meaningless, and coverage is thresholded at 0.5 exactly as the fog
       grid's corner bits are.
       AT uScale = 1 IT MUST BE THE IDENTITY, and that is the gate: tc lands on
       an integer, so w is 0 or 1 and the blend is a single tap, resolved
       through the palette and divided by its own coverage of 1. */
    "  vec2 tc = uv * vec2(uSize) - 0.5;\n"
    "  vec2 b  = floor(tc);\n"
    "  vec2 w  = clamp((tc - b - 0.5) * uScale + 0.5, 0.0, 1.0);\n"
    "  ivec2 ib = ivec2(b);\n"
    "  vec4 c = mix(mix(tap(ib),                tap(ib + ivec2(1, 0)), w.x),\n"
    "               mix(tap(ib + ivec2(0, 1)), tap(ib + ivec2(1, 1)), w.x), w.y);\n"
    "  if (c.a > 0.5) { frag = vec4(c.rgb / c.a, 1.0); return; }\n"
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
    x_glClearBufferfv = (PFN_CLEARBUFFERFV)getgl("glClearBufferfv");
    x_glDrawBuffers   = (PFN_DRAWBUFFERS)getgl("glDrawBuffers");
    if (!x_glDrawArrays || !x_glFramebufferTexture2D || !x_glUniform4f || !x_glUniform2f ||
        !x_glUniform2iv || !x_glScissor || !x_glDisable || !x_glBlendFunc || !x_glActiveTexture ||
        !x_glClearColor || !x_glClear) {
        slog("gui: GL entry points missing"); s_gl = 2; return 0;
    }
    /* the colour twins need these two; without them the module still runs,
       indexed, exactly as it did before G15e */
    if (!x_glClearBufferfv || !x_glDrawBuffers)
        slog("gui: no glClearBufferfv/glDrawBuffers — Classic++ UI unavailable, the layer stays indexed");
    s_sprProg = mkprog(QVS, SPR_FS);
    s_cpyProg = mkprog(QVS, CPY_FS);
    s_layProg = mkprog(LAY_VS, LAY_FS);
    if (s_gl == 2) return 0;
    glUseProgram(s_sprProg);
    glUniform1i(glGetUniformLocation(s_sprProg, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_sprProg, "uAtlasRGB"), 1);
    s_uSprSize = glGetUniformLocation(s_sprProg, "uSize");
    s_uSprCK   = glGetUniformLocation(s_sprProg, "uCK");
    s_uSprRestored = glGetUniformLocation(s_sprProg, "uRestored");
    glUseProgram(s_cpyProg);
    glUniform1i(glGetUniformLocation(s_cpyProg, "uSrc"), 0);
    glUniform1i(glGetUniformLocation(s_cpyProg, "uSrcCol"), 1);
    s_uCpySize = glGetUniformLocation(s_cpyProg, "uSize");
    s_uCpyOff  = glGetUniformLocation(s_cpyProg, "uOff");
    s_uCpyHasCol = glGetUniformLocation(s_cpyProg, "uSrcHasCol");
    glUseProgram(s_layProg);
    glUniform1i(glGetUniformLocation(s_layProg, "uTwin"), 0);
    glUniform1i(glGetUniformLocation(s_layProg, "uPal"),  1);
    glUniform1i(glGetUniformLocation(s_layProg, "uSurf"), 2);
    glUniform1i(glGetUniformLocation(s_layProg, "uTwinCol"), 3);
    glUniform1i(glGetUniformLocation(s_layProg, "uSharp"),   4);
    s_uLaySize   = glGetUniformLocation(s_layProg, "uSize");
    s_uLayStrict = glGetUniformLocation(s_layProg, "uStrict");
    s_uLayKey    = glGetUniformLocation(s_layProg, "uKey");
    s_uLayVp     = glGetUniformLocation(s_layProg, "uVp");
    s_uLayCursor = glGetUniformLocation(s_layProg, "uCursor");
    s_uLayColOn  = glGetUniformLocation(s_layProg, "uColOn");
    s_uLayScale     = glGetUniformLocation(s_layProg, "uScale");
    s_uLaySharpSize = glGetUniformLocation(s_layProg, "uSharpSize");
    s_uLaySharpOn   = glGetUniformLocation(s_layProg, "uSharpOn");
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
    s_atlas.prio = UI_RESTORE_PRIO;
    s_atlas.restoreMinEdge = UI_RESTORE_MIN;
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
    if (t->rgb) glDeleteTextures(1, &t->rgb);
    *t = s_twins[--s_ntwins];
}

/* The colour twin, made by the first op that has colour to put in it: RGBA8
   at COLOR_ATTACHMENT1 of the same FBO, so one MRT draw writes the index and
   the colour together and they can never disagree about what a texel holds.
   Cleared to alpha 0 — nothing is restored until an op says so. */
static void twin_colour(TWIN* t)
{
    static const GLenum two[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    static const GLfloat zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    GLenum st;
    if (t->rgb || !x_glDrawBuffers || !x_glClearBufferfv || !t->fbo) return;
    glGenTextures(1, &t->rgb);
    if (!t->rgb) return;
    glBindTexture(GL_TEXTURE_2D, t->rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    x_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t->rgb, 0);
    x_glDrawBuffers(2, two);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        /* back to indexed rather than into an incomplete FBO, which would
           drop the INDEX draws too and blank the surface */
        char b[140];
        _snprintf(b, sizeof b, "gui: colour twin %08X %dx%d FBO incomplete (%x) — indexed", t->surf, t->w, t->h, (unsigned)st);
        slog(b);
        x_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        x_glDrawBuffers(1, two);
        glDeleteTextures(1, &t->rgb);
        t->rgb = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    x_glClearBufferfv(GL_COLOR, 1, zero);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_colTwins++;
}

/* Indices arrived for this box and they say nothing about colour: drop the
   colour there so the layer falls back to the palette. */
static void twin_col_drop(TWIN* t, int l, int tp, int w, int h)
{
    static const GLfloat zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (!t->rgb || !x_glClearBufferfv || w <= 0 || h <= 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glEnable(GL_SCISSOR_TEST);
    x_glScissor(l, tp, w, h);
    x_glClearBufferfv(GL_COLOR, 1, zero);
    x_glDisable(GL_SCISSOR_TEST);
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
    twin_col_drop(t, l, tp, w, h);
}

static void quad(float* v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    float q[24] = { x0,y0,u0,v0,  x1,y0,u1,v0,  x0,y1,u0,v1,   x0,y1,u0,v1,  x1,y0,u1,v0,  x1,y1,u1,v1 };
    memcpy(v, q, sizeof q);
}

static void twin_sprite(TWIN* t, const TAGPU_GAFENT* e, const TAGPU_PUBOP* o)
{
    float v[24];
    int restored = s_colValid && s_atlas.rgb != 0;
    if (restored) twin_colour(t);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glEnable(GL_SCISSOR_TEST);
    x_glScissor(o->l, o->t, o->r - o->l + 1, o->b - o->t + 1);
    glUseProgram(s_sprProg);
    x_glUniform2f(s_uSprSize, (float)t->w, (float)t->h);
    glUniform1i(s_uSprCK, (int)o->ck);
    glUniform1i(s_uSprRestored, (restored && t->rgb) ? 1 : 0);
    /* unit 1 must hold a real texture even when the branch is off */
    x_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_atlas.rgb ? s_atlas.rgb : s_palTex);
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
    /* THE COPY IS WHY COLOUR IS PER SURFACE (gui-renderer.md 13.2): the panel
       is painted into panel+0xBC and only later blitted to the frame, so
       restored art reaches the screen through here or not at all. A source
       with no colour twin writes zero, which invalidates the destination's
       colour over the box — a copy from indexed art means indexed art. */
    if (src->rgb) twin_colour(t);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    glEnable(GL_SCISSOR_TEST);
    x_glScissor(o->l, o->t, o->r - o->l + 1, o->b - o->t + 1);
    glUseProgram(s_cpyProg);
    x_glUniform2f(s_uCpySize, (float)t->w, (float)t->h);
    off[0] = o->l - o->sl; off[1] = o->t - o->st;      /* dst pixel - src pixel */
    x_glUniform2iv(s_uCpyOff, 1, off);
    glUniform1i(s_uCpyHasCol, (src->rgb && t->rgb) ? 1 : 0);
    x_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, src->rgb ? src->rgb : s_palTex);
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

/* ---------------------------------------------------------- Classic++ arm */
/* Once per present, BEFORE the drain (the sprite ops it replays ask whether
   colour is valid) and after upload_palette (this compares against it).

   THE VALIDITY RULE, gui-renderer.md 3.4. tagpu_rglsl_job_new snapshots the
   palette into a texture of its own, so the restored twin's colours are a
   function of the palette that was live when the job was made. While that is
   still the palette the frame is PRESENTED with, colour is used; while it is
   not, every colour twin is ignored and the frame is indexed — dithered art
   for the duration of a fade, never wrong art. Once the palette has held
   still for PAL_SETTLE frames the job is rebuilt against the new one and
   every colour twin is invalidated, so the art comes back restored as the
   engine redraws it. */
static void restore_step(void)
{
    int i;
    if (s_norestore || !tagpu_classicpp_on() || !x_glDrawBuffers || !x_glClearBufferfv) {
        s_colValid = 0;
        return;
    }
    if (!s_atlas.rgb) {                         /* first arm, and after a context loss */
        memcpy(s_restorePal, s_palCopy, sizeof s_restorePal);
        tagpu_gaf_atlas_restore(&s_atlas, s_restorePal);
        s_palSeen = s_palChanges; s_palSettle = 0;
        s_colValid = s_atlas.rgb != 0;
        if (s_colValid)
            slog("gui: Classic++ UI armed — the UI atlas's restored twin at priority 4, nothing under 12x12");
        return;
    }
    tagpu_gaf_atlas_restore(&s_atlas, s_restorePal);   /* the per-frame call: a no-op once armed */
    if (memcmp(s_restorePal, s_palCopy, sizeof s_restorePal) == 0) { s_colValid = 1; s_palSettle = 0; return; }
    s_colValid = 0;
    if (s_palChanges != s_palSeen) { s_palSeen = s_palChanges; s_palSettle = 0; return; }
    if (++s_palSettle < PAL_SETTLE) return;
    {
        char b[220];
        int ncol = 0;
        if (s_atlas.job) { tagpu_rglsl_job_free(s_atlas.job); s_atlas.job = NULL; }
        if (s_atlas.rgb) { glDeleteTextures(1, &s_atlas.rgb); s_atlas.rgb = 0; }
        s_atlas.restoreFailed = 0;
        memcpy(s_restorePal, s_palCopy, sizeof s_restorePal);
        tagpu_gaf_atlas_restore(&s_atlas, s_restorePal);
        for (i = 0; i < s_ntwins; i++)
            if (s_twins[i].rgb) { twin_col_drop(&s_twins[i], 0, 0, s_twins[i].w, s_twins[i].h); ncol++; }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        s_rearms++; s_palSettle = 0;
        _snprintf(b, sizeof b, "gui: the presented palette moved — restored twin re-armed (#%u), %d of %d twin(s) had colour and were invalidated",
                  s_rearms, ncol, s_ntwins);
        slog(b);
    }
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
   engine's own default of 12), applied only on the way to SetEntries, never to
   +0x143A7 (read 2026-09-07, engine map "The palette the screen is presented
   with"). The engine's own pixels beneath the twin are drawn by cnc-ddraw through the
   palette its SetEntries received, so that is the palette the twin resolves
   through: the primary's palette object in this DLL. The engine's table is
   the fallback until a primary exists, and the number of entries where the
   two disagree is measured at every upload (`paldiff=` in the heartbeat):
   0 at Gamma 12, and the world passes, which read +0x143A7, are wrong by
   exactly that much at any other setting — WHICH IS EVERY SETTING WE MEASURE
   AT. The template wine prefix carries Gamma 15, so the factor is 1.125,
   paldiff reads 235 in the shell and in game alike, and the world is drawn
   ~11 % darker than the engine presents its own pixels (MEASURED 2026-09-09,
   gui-renderer.md 14). This layer is right either way; the world passes are
   the ones still to decide. */
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
       freed one. The present itself runs outside the section. The interleave
       reads data_rgb by member (RGBQUAD is B,G,R,reserved — reading the raw
       bytes as R,G,B swaps red and blue, paldiff 218 not 0), so it must stay
       inside the guard; the 2026-09-07 review's "copy the 1024 bytes out and
       convert outside the lock" was declined because the byte copy loses the
       member order and the saving is below the meter. */
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

/* ------------------------------------------------------------ sharp layer */
/* THE SCREEN-SPACE HALF OF 13.2's SHARP LAYER. One RGBA8 texture the size of
   the frame's viewport in WINDOW pixels — everything of ours at the device's
   resolution (13.1) — cleared at every present and composited above the 1x
   mirror wherever its alpha says it has coverage.

   ROW 0 IS THE VIEWPORT'S TOP ROW, and nothing flips to make that true: the
   layer shader indexes this texture with the same top-down `uv` it indexes the
   twin with, and a scissor box on an FBO addresses the attachment's rows
   directly, so scissor y IS the texture row IS the distance down from the top
   of the viewport. A client draws in screen coordinates and never converts.
   (MEASURED 2026-09-09: this is what `sharptest` was for. Its first square was
   scissored at `h - 64` on the assumption that a scissor is bottom-up like the
   window's, and it came out at the bottom of the screen.)

   Empty in G17a by design: its clients are the cursor (13.5, G17c) and the
   string op (13.4, G17d). `sharptest` is what makes an empty layer testable —
   without it the gate cannot tell a wired layer from a dead one. */
static void sharp_drop(void)
{
    if (s_sharpFbo) glDeleteFramebuffers(1, &s_sharpFbo);
    if (s_sharpTex) glDeleteTextures(1, &s_sharpTex);
    s_sharpFbo = s_sharpTex = 0;
    s_sharpW = s_sharpH = 0;
    s_sharpOn = 0;
}

static void sharp_begin(const TAGPU_FRAME* f)
{
    int w = f->vp_w, h = f->vp_h;
    s_sharpOn = 0;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    if (s_sharpTex && (s_sharpW != w || s_sharpH != h)) sharp_drop();
    if (!s_sharpTex) {
        GLenum st;
        glGenTextures(1, &s_sharpTex);
        glGenFramebuffers(1, &s_sharpFbo);
        if (!s_sharpTex || !s_sharpFbo) { sharp_drop(); return; }
        glBindTexture(GL_TEXTURE_2D, s_sharpTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_sharpFbo);
        x_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_sharpTex, 0);
        st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            /* the mirror alone is a complete picture — the layer is additive
               (13.2), so a target we cannot make costs sharpness, never a hole */
            char b[140];
            _snprintf(b, sizeof b, "gui: sharp layer %dx%d FBO incomplete (%x) — the mirror alone", w, h, (unsigned)st);
            b[sizeof b - 1] = '\0';
            slog(b);
            sharp_drop();
            return;
        }
        s_sharpW = w; s_sharpH = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_sharpFbo);
    glViewport(0, 0, w, h);
    x_glDisable(GL_SCISSOR_TEST);
    x_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    x_glClear(GL_COLOR_BUFFER_BIT);
    if (s_sharptest) {
        /* THE HARNESS LEVER, never for a player, and the only thing in G17a
           that puts a texel in this layer: a 64x64 opaque green square at the
           viewport's TOP-LEFT (rows 0..63, which is scissor y 0..63 — see
           above) and a one-DEVICE-pixel white column at device x = 100.
           Between them they prove the four things the gate cannot otherwise
           see — the layer exists at the device resolution, it composites
           ABOVE the mirror, alpha is what gates it, and row 0 is the top. */
        glEnable(GL_SCISSOR_TEST);
        x_glScissor(0, 0, 64, 64);
        x_glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        x_glClear(GL_COLOR_BUFFER_BIT);
        x_glScissor(100, 0, 1, h);
        x_glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        x_glClear(GL_COLOR_BUFFER_BIT);
        x_glDisable(GL_SCISSOR_TEST);
        /* the clear colour is global state and tagpu_overlay_capture_begin
           clears the frame's target without setting one of its own — leaving
           white here would tint every letterbox bar under the lever */
        x_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_sharpOn = 1;
}

static void draw_layer(const TAGPU_FRAME* f)
{
    TWIN* t = s_presented ? twin_find(s_presented) : NULL;
    float v[24], cur[4], ky;
    GLint sz[2], sh[2];
    const char* ta = *(const char* const*)TA_MAINPP;
    int L = 0, T = 0, W = 0, H = 0, key;
    if (!t || t->w != f->game_width || t->h != f->game_height) return;
    /* the palette was uploaded in tagpu_gui_present, BEFORE restore_step
       decided s_colValid. Uploading it again here -- after a drain that can be
       thousands of ops long, during which the game thread may have set a new
       one -- would show restored texels resolved through the palette their
       restore snapshotted beside indexed texels resolved through a NEWER one,
       which is the "wrong art" 3.4 exists to prevent. One upload per frame,
       and it is the one s_colValid was decided against. */
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
    /* k, and with it the ramp's width (13.3): device pixels per twin texel.
       The twin is the engine's surface 1:1, so this is exactly 13.1's k —
       1.0 for as long as the engine's screen IS the window, which is every
       path in phase 1 and the whole of G17a. Below 1 the fork is scaling the
       engine DOWN into a smaller window; the ramp is held at plain bilinear
       there rather than widened past a texel. */
    s_k = (t->w > 0 && f->vp_w > 0) ? (float)f->vp_w / (float)t->w : 1.0f;
    if (s_k < 1.0f) s_k = 1.0f;
    ky = (t->h > 0 && f->vp_h > 0) ? (float)f->vp_h / (float)t->h : 1.0f;
    if (ky < 1.0f) ky = 1.0f;
    x_glUniform2f(s_uLayScale, s_k, ky);
    /* the sharp layer, above everything, at the device resolution */
    glUniform1i(s_uLaySharpOn, s_sharpOn ? 1 : 0);
    sh[0] = s_sharpW; sh[1] = s_sharpH;
    x_glUniform2iv(s_uLaySharpSize, 1, sh);
    x_glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, s_sharpOn ? s_sharpTex : s_palTex);
    /* Classic++: the presented surface's colour twin, and whether it may be
       read at all this frame (the palette-validity rule, restore_step) */
    glUniform1i(s_uLayColOn, (s_colValid && t->rgb) ? 1 : 0);
    x_glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, t->rgb ? t->rgb : s_palTex);
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
    x_glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, 0);
    x_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
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
    int n;
    int on;
    if (s_lastPoll && t - s_lastPoll < POLL_MS) return;
    s_lastPoll = t;
    n = tagpu_opt_read("tagpu_gui.on", buf, sizeof buf);
    on = n >= 0;
    /* `off` inside the file also turns the draw off, so the detours can stay
       installed (they need the file at attach) while the layer is A/B'd */
    if (on && strstr(buf, "off")) on = 0;
    s_strict = on && strstr(buf, "strict") != NULL;
    /* `norestore`: the layer without Classic++ art, so the two halves can be
       A/B'd live without turning the world's restorer off too */
    s_norestore = on && strstr(buf, "norestore") != NULL;
    /* `sharptest`: 13.2's sharp layer filled with a known pattern. The
       harness's mode like `strict`, never a player's — G17a's layer is empty
       otherwise and an empty layer proves nothing. */
    s_sharptest = on && strstr(buf, "sharptest") != NULL;
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
    upload_palette();       /* before restore_step, which compares against it */
    restore_step();         /* before the drain: its sprite ops ask whether colour is valid */
    /* STEP THE RESTORER WHEN NOTHING ELSE DID. tagpu_rglsl_step's only other
       caller is the native pass, which returns early with no unit array — so
       in the shell, and in game with the world passes disarmed, it never runs
       and the UI atlas's queue is never drained: every sprite would then read
       alpha 0 from an unpainted twin and the UI would stay indexed for ever,
       silently. Comparing the restorer's call count across presents says
       whether the native pass stepped it this frame; when it did, we do
       nothing, so the budget is sliced once either way. Before the drain, so
       what it paints this frame is what the drain's sprites sample. */
    if (s_atlas.job) {
        unsigned n = tagpu_rglsl_calls();
        if (n == s_rglslSeen) tagpu_rglsl_step();
        s_rglslSeen = tagpu_rglsl_calls();
    }
    drain();
    /* AFTER the drain, which binds twin FBOs and leaves one bound, and before
       the layer that samples it: the sharp layer is cleared for this frame
       (and, under `sharptest`, filled) while nothing of the composite has
       been written yet. */
    sharp_begin(f);
    draw_layer(f);
    unbind_all();
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());
    glViewport(f->vp_x, f->vp_y, f->vp_w, f->vp_h);
    if (f->frame_counter - last >= 300) {
        /* 205 bytes of literal + 30 conversions: the worst case is ~519, and
           _snprintf does not NUL-terminate what it truncates */
        char b[768];
        static LARGE_INTEGER t0, fq;
        LARGE_INTEGER t1;
        double fps = 0.0;
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t1);
        if (t0.QuadPart) fps = (double)(f->frame_counter - last) * (double)fq.QuadPart / (double)(t1.QuadPart - t0.QuadPart);
        t0 = t1;
        last = f->frame_counter;
        _snprintf(b, sizeof b, "gui: twins=%d presented=%08X drained=%u seeds=%u sprites=%u copies=%u pixels=%u clears=%u atlas=%d/%d lost=%u strict=%d resets=%u overflows=%u stalls=%u skipped=%u palchg=%u paldiff=%d@%d palsrc=%d cpp=%d col=%u/%d colvalid=%d rearms=%u rgb=%u k=%.3f sharp=%dx%d fps=%.1f",
                  s_ntwins, s_presented, s_drained, s_seeds, s_sprites, s_copies, s_pixels, s_clears,
                  s_atlas.n, s_atlas.max, s_lostSprites, s_strict, g_guiq.resets, g_guiq.overflows, g_guiq.stalls,
                  s_skipped, s_palChanges, s_palDiff, s_palDiffAt, s_palSource,
                  tagpu_classicpp_on() ? 1 : 0, s_colTwins, s_ntwins, s_colValid, s_rearms, s_atlas.rgb,
                  s_k, s_sharpW, s_sharpH, fps);
        b[sizeof b - 1] = '\0';
        slog(b);
    }
}

void tagpu_gui_glreset(void)
{
    /* the context is gone: forget every id, start over from fresh seeds —
       and take nothing from the queue until the producer's RESET arrives */
    s_ntwins = 0; s_presented = 0;
    s_gl = 0; s_sprProg = s_cpyProg = s_layProg = s_vao = s_vbo = s_palTex = 0;
    s_sharpTex = s_sharpFbo = 0; s_sharpW = s_sharpH = 0; s_sharpOn = 0;   /* the sharp layer died with it */
    tagpu_gaf_atlas_lost(&s_atlas);
    memset(s_palCopy, 0xFF, sizeof s_palCopy);        /* the palette texture died too: re-upload */
    s_skipToReset = 1;
    g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_GLCTX;
}

int tagpu_gui_drawing(void) { return s_on; }
