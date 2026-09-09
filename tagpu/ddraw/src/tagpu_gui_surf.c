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
   rather than configured, and it is 1.0 wherever the engine's screen IS the
   window; giving the ENGINE window / k is G17b's. It is NOT always 1 today:
   `resizable` defaults TRUE (config.c) and `maintas` fits the viewport to the
   window (dd.c), so a player who drags the window off the game resolution is
   already at a fractional k and already gets this ramp. At k = 1 the ramp is
   exactly one source texel wide, so every output pixel samples a texel centre
   and the frame is what texelFetch gave, to within a rounding step far too
   small to cross an 8-bit value: that identity is the gate, not a hope. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_gui.h"
#include "tagpu_opt.h"
#include "tagpu_gui_int.h"
#include "tagpu_gaf.h"
#include "tagpu_text.h"                 /* the glyph atlas the string op stamps from (13.4) */
#include "tagpu_classicpp.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_vpwide.h"
#include "tagpu_terr.h"
#include "tagpu_overlay.h"
#include "opengl_utils.h"
#include "dd.h"                         /* g_ddraw.primary->palette: the palette the engine's frame is PRESENTED with */
#include "mouse.h"                      /* mouse_last_client: the pointer at the DEVICE's resolution (13.5) */
#include "IDirectDrawSurface.h"
#include "IDirectDrawPalette.h"

#define TA_MAINPP      0x00511DE8u
#define OFF_PALETTE    0x143A7          /* 256 x {R,G,B,pad}                   */
#define GFX_GLOBALS_PP 0x0051FBD0u
#define MOUSE_POS_X    0x1B6            /* the cursor's last drawn position    */
#define MOUSE_POS_Y    0x1BA
#define MOUSE_SPRITE   0x1B2            /* -> record: u16 w, u16 h, s16 hotspots */
/* the minimap's box on the engine's screen, filled by BuildMinimapSurface
   0x466780 (engine map, "The minimap, located"); +0x142F1 bit 1 is what
   DrawMinimap 0x466B00 itself is gated on */
#define MM_OFFX        0x142E7
#define MM_OFFY        0x142E9
#define MM_W           0x142EB
#define MM_H           0x142ED
#define MM_FLAGS       0x142F1
#define MM_COMPOSITE   0x142DB          /* the fog+dots composite; non-NULL = built */
#define MM_VIEWRECT    0x142CB          /* the view box, 4 ints, SCREEN px, edges inclusive */
#define MM_VIEWCOL     0xDD9            /* its palette index (0x466B50 reads this byte)     */
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
static GLint  s_uLayCursOurs;
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
static int    s_sharpFailed = 0;        /* the target could not be made: stay off rather than retry     */
static GLuint s_sharpProg;              /* QVS + SHARP_FS: a client's flat-coloured quad in the layer   */
static GLint  s_uSharpProgSize, s_uSharpProgCol;
static GLuint s_cursProg;               /* QVS + CURS_FS: the cursor's frame out of the UI atlas        */
static GLint  s_uCursSize, s_uCursCK, s_uCursRestored;
static GLuint s_strProg;                /* QVS + STR_FS: a string op's glyphs into a twin (G17d)        */
static GLint  s_uStrSize, s_uStrFg, s_uStrBg, s_uStrTr;
static unsigned s_strings = 0;          /* string ops stamped                                          */
static unsigned s_glyphs = 0;           /* glyph quads drawn                                           */
static unsigned s_strMiss = 0;          /* glyphs the cache would not give (the engine drew them)      */
static unsigned s_strReseed = 0;        /* strings that stamped NOTHING and asked for a fresh seed     */
/* G17e: the TNT's own 252-px minimap picture, uploaded once per map load */
static GLuint   s_mmTex;
static unsigned s_mmGenSeen;            /* the generation s_mmTex holds; 0 = nothing        */
static int      s_mmTW, s_mmTH;         /* its size in texels                               */
static int      s_mmbase = 0;           /* token `mmbase`: draw it, harness only for now    */
static int      s_mmdots = 0;           /* token `mmdots`: the dots ALONE, over the engine's */
static unsigned s_mmDrawn = 0;
static unsigned s_mmSprites = 0, s_mmOther = 0;   /* ops published against the minimap composite */
/* THE ENGINE'S DOTS, ACCUMULATED FROM ITS OWN OPS (13.6). Which units appear on
   the minimap is fog- and LOS-dependent sim logic, so re-deriving it does not
   fail by rendering badly — it fails by showing enemy positions the player is
   not entitled to. The dots therefore stay the engine's: they are 0x4B7F90
   blits, which are observed leaves, so they arrive as sprite ops carrying their
   own destination, and replaying those is exact and free.
   `0x466DC0` rebuilds the composite by copying the base in FIRST and then
   drawing the dots, so a full-surface op against the composite is where a new
   rebuild starts and the list resets. */
#define MM_MAXDOTS 512
typedef struct { const void* frame; const void* pix; unsigned short fw, fh; unsigned char ck; short sl, st; } MMDOT;
static MMDOT    s_mmDots[MM_MAXDOTS];
static int      s_mmNDots;
static int      s_mmDotsFull;
static unsigned s_mmDotLost;            /* a dot whose art the atlas would not hold */
static unsigned s_mmLogged;

/* The composite's PIXEL BASE, which is what an op names — `+0x142DB` is the
   OFFSCREEN and its base is field 3 (w, h, pitch, base; CTX_BASE in the hook). */
static unsigned mm_base(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const int* off;
    if (!ptr_ok(ta)) return 0;
    off = *(const int* const*)(ta + MM_COMPOSITE);
    if (!ptr_ok(off)) return 0;
    return (unsigned)off[3];
}

/* THE CURSOR (gui-renderer.md 13.5, G17c). Decided ONCE per frame, in
   tagpu_gui_cursor_frame, because the world composite reads the decision
   before this module's present runs: tagpu_overlay.c calls the native pass
   first, and the two must not disagree about whether the engine's own cursor
   is being erased. Everything here is render-thread state. */
static int    s_nocursor = 0;           /* token: keep phase 1's engine cursor          */
static float  s_cursorScale = 1.0f;     /* token cursorscale=, device px per frame px   */
static const unsigned char* s_curFrame; /* the GAF frame the engine is blitting          */
static int    s_curW, s_curH;           /* its size, frame px                            */
static int    s_curHX, s_curHY;         /* its hotspot, frame px, may be negative        */
static unsigned char s_curCK;           /* its colour key                                */
static float  s_curEng[4];              /* the engine's own rect, GAME px: what to erase */
static int    s_curOwn = 0;             /* ours is drawn this frame                      */
static unsigned s_curDrawn = 0;         /* frames ours was drawn                         */
static unsigned s_curWarm = 0;          /* frames spent atlasing a shape we had not seen */
static int    s_curDev = 0;             /* the last draw used the true client point      */
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
/* THE STRING OP (13.4, G17d): TA's own glyphs out of the coverage atlas,
   stamped one cell at a time in the palette indices the engine's blitter was
   given. `0x4CCF60` picks fg where the glyph's bit is set and bg where it is
   clear, and stores only when the chosen colour differs from `transparent` —
   an 8-bit compare (`cmp al,ah` at 0x4CCFE2), which is why the three arrive as
   bytes. Reproduced here exactly, so at k = 1 the stamp is the blit.
   oCol is 0 wherever we write: text is a flat palette index and never has
   restored colour of its own, and 0 is what tells the layer to resolve those
   texels through the palette. Between the glyphs nothing is written at all, so
   restored art under a transparent-background string SURVIVES — which is the
   whole difference from publishing the box's bytes, where the whole rectangle
   lost its colour. */
static const char* STR_FS =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "layout(location=0) out vec4 oIdx;\n"
    "layout(location=1) out vec4 oCol;\n"
    "uniform sampler2D uGlyph;\n"
    "uniform int uFg; uniform int uBg; uniform int uTr;\n"
    "void main(){ float c = texture(uGlyph, uv).r;\n"
    "  int col = (c > 0.5) ? uFg : uBg;\n"
    "  if (col == uTr) discard;\n"
    "  oIdx = vec4(float(col) / 255.0, 1.0, 0.0, 0.0);\n"
    "  oCol = vec4(0.0); }\n";
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
/* A CLIENT'S FILL IN THE SHARP LAYER: a constant colour over a quad given in
   sharp-layer pixels, y DOWN from the viewport's top row. It pairs with QVS,
   the TWINS' vertex mapping, unchanged and deliberately: both textures are
   read back with a texelFetch whose row 0 is the top of the thing they mirror,
   and NDC y = -1 is attachment row 0, so `y / size * 2 - 1` is the one mapping
   both want. There is no flip anywhere in this module, and a client that adds
   one draws upside down (MEASURED 2026-09-09: this shader had one for exactly
   as long as it took `sharptest` to draw a quad through it). */
static const char* SHARP_FS =
    "#version 330 core\n"
    "out vec4 frag; uniform vec4 uCol;\n"
    "void main(){ frag = uCol; }\n";
/* THE CURSOR IN THE SHARP LAYER (13.5, G17c). The same UI atlas the sprite
   ops draw from, so a cursor frame is restored by the same lazy job at the
   same priority and needs no pool slot of its own -- and the same colour-key
   discard, so the layer's alpha is exactly the frame's coverage and the
   composite's `sh.a > 0.5` test does the rest. It pairs with QVS, in
   sharp-layer pixels with y DOWN from the viewport's top row; there is no
   flip in this module and a client that adds one draws at the foot of the
   screen (the comment above SHARP_FS is the measurement). */
static const char* CURS_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uAtlas; uniform sampler2D uAtlasRGB; uniform sampler2D uPal;\n"
    "uniform int uCK; uniform int uRestored;\n"
    "void main(){ float i = texture(uAtlas, uv).r;\n"
    "  if (int(i * 255.0 + 0.5) == uCK) discard;\n"
    "  if (uRestored != 0) { vec4 t = texture(uAtlasRGB, uv);\n"
    "    if (t.a > 0.5) { frag = vec4(t.rgb, 1.0); return; } }\n"
    /* the PRESENTED palette, the one the layer resolves the mirror through --
       not main+0x143A7. The cursor sits on top of both halves of the frame and
       a cursor a Gamma step darker than the panel under it would show. */
    "  frag = vec4(texture(uPal, vec2((i * 255.0 + 0.5) / 256.0, 0.5)).rgb, 1.0); }\n";
static const char* LAY_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uTwin; uniform sampler2D uPal; uniform sampler2D uSurf;\n"
    "uniform sampler2D uTwinCol; uniform sampler2D uSharp;\n"
    "uniform int uColOn; uniform int uSharpOn;\n"
    "uniform ivec2 uSize; uniform ivec2 uSharpSize; uniform vec2 uScale;\n"
    "uniform int uStrict; uniform int uKey; uniform vec4 uVp; uniform vec4 uCursor;\n"
    "uniform int uCursOurs;\n"
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
    /* THE SHARP LAYER (gui-renderer.md 13.2), TOP of the composite: device
       resolution, drawn from live state at present time, row 0 the viewport's
       TOP row. Alpha is its coverage.
       IT IS TESTED FIRST, AHEAD OF THE CURSOR RECT, and that order is the
       whole of G17c's half of this shader. G17a had the rect discard above it
       — harmless while the layer was empty, and fatal the moment the cursor
       moved in, because the one place a cursor is ever drawn is the one place
       the shader had already given up on. */
    "  if (uSharpOn != 0) {\n"
    "    ivec2 sp = clamp(ivec2(uv * vec2(uSharpSize)), ivec2(0), uSharpSize - 1);\n"
    "    vec4 sh = texelFetch(uSharp, sp, 0);\n"
    "    if (sh.a > 0.5) { frag = vec4(sh.rgb, 1.0); return; }\n"
    "  }\n"
    /* THE CURSOR'S RECT. In phase 1 (and with `nocursor`) the cursor is the
       engine's: it is blitted onto the primary after everything we observe, so
       it exists only in the engine's frame, and its rect is left to that frame
       — the twin is not drawn there. uCursOurs says the sharp layer carried a
       cursor instead, and then the twin DOES draw here, showing the background
       the engine's blit covered; ours is already above it.
       Either way the rect stays exempt from `strict`, because either way the
       engine's surface holds cursor pixels the twin has never seen. */
    "  bool cur = f.x >= uCursor.x && f.x < uCursor.x + uCursor.z && f.y >= uCursor.y && f.y < uCursor.y + uCursor.w;\n"
    "  if (cur && uCursOurs == 0) discard;\n"
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
    "  if (uStrict == 1 && !cur) {\n"
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
    s_sharpProg = mkprog(QVS, SHARP_FS);
    s_cursProg  = mkprog(QVS, CURS_FS);
    s_strProg   = mkprog(QVS, STR_FS);
    if (s_gl == 2) return 0;
    glUseProgram(s_strProg);
    glUniform1i(glGetUniformLocation(s_strProg, "uGlyph"), 0);
    s_uStrSize = glGetUniformLocation(s_strProg, "uSize");
    s_uStrFg   = glGetUniformLocation(s_strProg, "uFg");
    s_uStrBg   = glGetUniformLocation(s_strProg, "uBg");
    s_uStrTr   = glGetUniformLocation(s_strProg, "uTr");
    s_uSharpProgSize = glGetUniformLocation(s_sharpProg, "uSize");
    s_uSharpProgCol  = glGetUniformLocation(s_sharpProg, "uCol");
    glUseProgram(s_cursProg);
    glUniform1i(glGetUniformLocation(s_cursProg, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_cursProg, "uAtlasRGB"), 1);
    glUniform1i(glGetUniformLocation(s_cursProg, "uPal"), 2);
    s_uCursSize = glGetUniformLocation(s_cursProg, "uSize");
    s_uCursCK   = glGetUniformLocation(s_cursProg, "uCK");
    s_uCursRestored = glGetUniformLocation(s_cursProg, "uRestored");
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
    s_uLayCursOurs  = glGetUniformLocation(s_layProg, "uCursOurs");
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

/* A STRING OP INTO ITS TWIN, glyph by glyph (13.4, G17d).

   NOT into the sharp layer. 13.2 leaves that open ("string ops IF they are
   rendered late") and 13.4 closes it: a 1x glyph carries 1x information however
   it is drawn, so a device-resolution layer would buy nothing and would cost
   the one thing that matters — text scaling WITH the UI it belongs to. At k = 3
   a 1x-device string beside a 3x panel is unreadable. The gains 13.4 claims are
   all properties of stamping into the twin: the glyph's edge no longer drags in
   the art it was blitted onto, restored colour survives between the letters,
   and the arena carries the string instead of the rectangle.

   THE ENGINE ADVANCES BY THE GLYPH'S OWN WIDTH BYTE AND NOTHING ELSE — no
   kerning, no pair table (`0x4CCFF7`..`0x4CCFFD` adds `cl`, the width, to the
   row-start pointer). So a run of per-glyph quads at those offsets is the same
   arithmetic the blitter does, not an approximation of it. */
static void twin_string(TWIN* t, const TAGPU_PUBOP* o)
{
    const char* str = (const char*)(g_guiq.arena + o->aoff);
    const unsigned char* font = (const unsigned char*)o->frame;
    short cell[256][4];                 /* ax, ay, w, h per drawn glyph        */
    int n = 0, i, x, top, yoff = 0, aw = 0, ah = 0, restored;
    GLuint gtex;
    float v[24];

    if (!s_strProg || !o->alen) goto reseed;
    /* PASS ONE: rasterise every glyph this string needs, so the atlas texture
       is uploaded ONCE for the string rather than once per new glyph. */
    for (i = 0; i < 256 && str[i] && str[i] != '\n'; i++) {
        int ax, ay, gw, gh;
        if (!tagpu_text_glyph(font, (unsigned char)str[i], &ax, &ay, &gw, &gh, &yoff)) {
            /* the engine skips a code below `first` and a zero table entry and
               advances for neither, so a refusal here is only a divergence when
               the cache refused something the engine would have drawn */
            s_strMiss++;
            continue;
        }
        if (n < 256) {
            cell[n][0] = (short)ax; cell[n][1] = (short)ay;
            cell[n][2] = (short)gw; cell[n][3] = (short)gh; n++;
        }
    }
    if (!n) goto reseed;
    x_glActiveTexture(GL_TEXTURE0);
    gtex = tagpu_text_glyph_tex();
    if (!gtex) goto reseed;
    tagpu_text_glyph_dims(&aw, &ah);
    if (aw <= 0 || ah <= 0) goto reseed;

    restored = s_colValid && s_atlas.rgb != 0;
    if (restored) twin_colour(t);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    x_glDisable(GL_BLEND);
    x_glDisable(GL_DEPTH_TEST);
    x_glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_strProg);
    x_glUniform2f(s_uStrSize, (float)t->w, (float)t->h);
    glUniform1i(s_uStrFg, (int)o->fg);
    glUniform1i(s_uStrBg, (int)o->bg);
    glUniform1i(s_uStrTr, (int)o->tr);
    glBindTexture(GL_TEXTURE_2D, gtex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* the blitter's destination is base + (y - (s8)font[2]) * pitch + x, so the
       string's first pixel row is at y - yoff and NOT at the y it was given */
    x = (int)o->sl;
    top = (int)o->st - yoff;
    for (i = 0; i < n; i++) {
        int gw = cell[i][2], gh = cell[i][3];
        quad(v, (float)x, (float)top, (float)(x + gw), (float)(top + gh),
             (float)cell[i][0] / (float)aw,            (float)cell[i][1] / (float)ah,
             (float)(cell[i][0] + gw) / (float)aw,     (float)(cell[i][1] + gh) / (float)ah);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
        x_glDrawArrays(GL_TRIANGLES, 0, 6);
        x += gw;
        s_glyphs++;
    }
    s_strings++;
    return;

reseed:
    /* WE PUBLISHED A STRING AND DREW NOTHING, so the twin is missing text the
       engine's surface has. A box of pixels would have been drawn whatever
       happened; a string can fail on a font we cannot read, so the fallback is
       to re-seed the surface from the engine's own — expensive, and it should
       never happen. Counted and logged rather than silent. */
    if (s_strReseed < 8) {
        char b[180];
        _snprintf(b, sizeof b, "gui: string op stamped nothing (font %08X, %u bytes, surface %08X) — re-seeding",
                  (unsigned)(size_t)o->frame, o->alen, o->surf);
        b[sizeof b - 1] = '\0';
        slog(b);
    }
    s_strReseed++;
    g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_STRING;
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
        /* G17e: what the engine publishes against the minimap composite, which
           is the question the dot replay turns on — the unit dots are 0x4B7F90
           blits and therefore observed leaves, so they should arrive here as
           sprite ops with their own positions rather than only inside the base
           copy's bytes (gui-renderer.md §7). */
        if (o->surf && o->surf == mm_base()) {
            if (o->kind == PK_SPRITE) {
                /* THE COMPOSITE HAS NO TWIN — it is never presented, so nothing
                   ever seeds it, and drain's own PK_SPRITE case returns before
                   it atlases anything (MEASURED 2026-09-09: `twins=2`, and the
                   first dot replay drew nothing because every atlas_find missed).
                   The frame has to be taken HERE, from the op's own first-sight
                   bytes, or the dots exist as positions with no art. */
                const TAGPU_GAFENT* e =
                    tagpu_gaf_atlas_find(&s_atlas, o->frame, o->pix, o->fw, o->fh);
                if (!e && o->alen)
                    e = tagpu_gaf_atlas_put(&s_atlas, o->frame, o->pix, o->fw, o->fh,
                                            o->ck, g_guiq.arena + o->aoff);
                s_mmSprites++;
                if (!e) s_mmDotLost++;
                else if (s_mmNDots < MM_MAXDOTS) {
                    MMDOT* d = &s_mmDots[s_mmNDots++];
                    d->frame = o->frame; d->pix = o->pix;
                    d->fw = o->fw; d->fh = o->fh; d->ck = o->ck;
                    d->sl = o->sl; d->st = o->st;
                } else s_mmDotsFull = 1;
            } else {
                s_mmOther++;
                /* the rebuild's base copy: everything after it is this frame's
                   dots. Any op that covers the whole composite serves, since
                   nothing else redraws all of it. */
                if (o->l <= 0 && o->t <= 0) { s_mmNDots = 0; s_mmDotsFull = 0; }
            }
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
        case PK_STRING:
            t = twin_find(o->surf);
            if (t) twin_string(t, o);
            break;
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
   exactly that much at any other setting.
   WHICH SETTING WE ARE AT IS NOT A PROPERTY OF THE TEMPLATE PREFIX, and the
   G15e note that said it was is withdrawn (MEASURED 2026-09-09, gui-renderer.md
   15): `wineprefix/user.reg` and all 58 instance prefixes are ONE inode — the
   clone is `cp -al` — and wine rewrites it in place at every launch, so there
   is a single Gamma shared by every instance and by the template, and it is
   whatever TA last stored. It read 15 (factor 1.125, `paldiff=235`) for the
   instances launched earlier that day and 12 (factor 1.0, `paldiff=0`) for the
   ones launched after, on this DLL and on main's alike. This layer is right at
   either value; the world passes are wrong by the factor whenever it is not
   12, and how it comes to be one or the other is open. */
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

/* ------------------------------------------------------------- the cursor */
/* ONE DECISION PER FRAME, TAKEN BEFORE THE WORLD PASS (13.5, G17c).
   tagpu_overlay.c calls this, then tagpu_native_frame, then tagpu_gui_present.
   That order is forced: erasing the engine's cursor takes BOTH modules — over
   the panel this module's twin covers it once the layer stops discarding, but
   over the world the composite discards our fragment wherever the engine's
   surface is not the terrain key, and a cursor pixel is not the key, so the
   engine's cursor survives underneath. The composite therefore has to know
   the rect, and it runs first. Reading the globals twice would let the two
   halves disagree by a mouse move and leave a sliver of the engine's cursor
   standing, so the state is read ONCE, here, and both halves use it.

   OWNERSHIP IS LATCHED ON THE ATLAS. A shape we have not uploaded yet is not
   owned: the sharp pass atlases it this frame and the NEXT frame draws it,
   which costs one frame of the engine's own cursor per new shape and never a
   frame with no cursor at all. Getting that backwards is the one failure this
   gate can ship invisibly -- the erase is unconditional, the draw is not. */
void tagpu_gui_cursor_frame(void)
{
    const char* g;
    const unsigned char* fr;
    const void* pix;
    s_curOwn = 0;
    s_curFrame = NULL;
    /* the engine's rect first and unconditionally: draw_layer's discard (and
       `strict`'s exemption) needs it on every path, including the ones below
       that decline to own the cursor */
    cursor_rect(s_curEng);
    if (!s_on || s_nocursor || s_gl != 1 || s_sharpFailed) return;
    g = *(const char* const*)GFX_GLOBALS_PP;
    if (!ptr_ok(g)) return;
    /* the sprite record IS a GAF frame header -- size, hotspot, colour key and
       a pixel pointer at +0x10 -- which is what makes the cursor reachable
       with no observer change at all: the blits themselves never become ops
       (everything drawn inside the flip is excluded) but the frame behind them
       is readable from here, on the render thread, every present. */
    fr = tagpu_gaf_frame_sane(*(const void* const*)(g + MOUSE_SPRITE));
    if (!fr) return;
    s_curW  = *(const unsigned short*)(fr + TAGPU_GF_W);
    s_curH  = *(const unsigned short*)(fr + TAGPU_GF_H);
    s_curHX = *(const short*)(fr + TAGPU_GF_HOTX);
    s_curHY = *(const short*)(fr + TAGPU_GF_HOTY);
    s_curCK = fr[TAGPU_GF_CK];
    s_curFrame = fr;
    pix = *(const void* const*)(fr + TAGPU_GF_PIX);
    if (tagpu_gaf_atlas_find(&s_atlas, fr, pix, s_curW, s_curH)) s_curOwn = 1;
    else s_curWarm++;
}

int tagpu_gui_cursor_own(float* r)
{
    if (!s_curOwn) return 0;
    if (r) { r[0] = s_curEng[0]; r[1] = s_curEng[1]; r[2] = s_curEng[2]; r[3] = s_curEng[3]; }
    return 1;
}

/* ------------------------------------------------------------ sharp layer */
/* THE SCREEN-SPACE HALF OF 13.2's SHARP LAYER. One RGBA8 texture the size of
   the frame's viewport in WINDOW pixels — everything of ours at the device's
   resolution (13.1) — cleared at every present and composited above the 1x
   mirror wherever its alpha says it has coverage.

   ROW 0 IS THE VIEWPORT'S TOP ROW, and it is the LAYER SHADER that makes it
   so: it samples this texture with the same top-down `uv` it indexes the twin
   with. Nothing about an FBO flips anything -- glScissor's y is measured from
   the framebuffer origin here exactly as it is on the window, and that origin
   is attachment row 0, which is also where NDC y = -1 lands. So a scissor box
   and a client's geometry agree without help, and a client uses QVS, the very
   mapping the twins use.
   `sharptest` earned its lines twice over here (MEASURED 2026-09-09). Its
   first square was scissored at `h - 64`, on the assumption that a scissor is
   bottom-up like the window's, and came out at the foot of the screen. The
   landing review then said the explanation was wrong in a way that would
   mirror G17c's cursor -- correctly -- and the shader written to fix it added
   a flip, which put the quad at the foot of the screen again. There is no
   flip. The lever caught both.

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

/* THE CURSOR INTO THE SHARP LAYER, at 1x DEVICE size whatever k is (13.5).
   That is the convention every scaled desktop UI follows and it is always
   crisp; `cursorscale=` is the escape for a 3x UI at 4K, where TA's cursors
   carry gameplay meaning.

   ITS POSITION IS THE TRUE POINTER where one is known. The engine only ever
   learns a point on its own logical grid, so its cursor can only sit on
   multiples of k device pixels; mouse_last_client hands back the client point
   the message carried, which is where the pointer actually is, and that is
   also what puts ours AHEAD of the engine's last-drawn position -- what G13m
   spent a gate achieving. With no client point (an injected click, or before
   the first message) it falls back to the engine's own position at the centre
   of its logical pixel, which is exactly where the engine draws.

   Called from sharp_begin with the layer's FBO bound and cleared. It runs on
   the warm-up frame too, one frame BEFORE it may draw, because atlasing the
   shape here is what lets the next tagpu_gui_cursor_frame own it. */
static void sharp_cursor(const TAGPU_FRAME* f)
{
    const TAGPU_GAFENT* e;
    float v[24], kx, ky;
    int cx = 0, cy = 0, dx, dy, x0, y0, w, h, restored;
    if (!s_curFrame || !s_cursProg || !s_sharpW || !s_sharpH) return;
    e = tagpu_gaf_atlas_get(&s_atlas, s_curFrame);
    if (!e) {
        /* Unreachable in practice -- tagpu_gui_cursor_frame only owns a frame
           tagpu_gaf_atlas_find already answered for, and atlas_get consults
           the same index first. Kept because ownership was published to the
           world composite a whole module ago: if it ever does happen, one
           frame with no cursor over the world beats a stale claim. */
        s_curOwn = 0;
        return;
    }
    if (!s_curOwn) return;               /* the warm-up frame: the engine's is on screen */
    kx = (f->game_width  > 0) ? (float)f->vp_w / (float)f->game_width  : 1.0f;
    ky = (f->game_height > 0) ? (float)f->vp_h / (float)f->game_height : 1.0f;
    if (mouse_last_client(&cx, &cy)) {
        dx = cx - f->vp_x; dy = cy - f->vp_y; s_curDev = 1;
    } else {
        int gx = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
        int gy = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
        dx = (int)(((float)gx + 0.5f) * kx);
        dy = (int)(((float)gy + 0.5f) * ky);
        s_curDev = 0;
    }
    /* clamped to the layer, which is the letterboxed viewport: the same edge
       clamp wndproc applies to the engine's own copy of the point */
    if (dx < 0) dx = 0; else if (dx > s_sharpW - 1) dx = s_sharpW - 1;
    if (dy < 0) dy = 0; else if (dy > s_sharpH - 1) dy = s_sharpH - 1;
    w = (int)((float)s_curW * s_cursorScale + 0.5f);
    h = (int)((float)s_curH * s_cursorScale + 0.5f);
    if (w <= 0 || h <= 0) return;
    /* the hotspot is SIGNED and routinely negative (the build cursors hang
       below and right of the point), so this is a subtraction that may move
       the frame's origin either way */
    x0 = dx - (int)((float)s_curHX * s_cursorScale);
    y0 = dy - (int)((float)s_curHY * s_cursorScale);
    restored = s_colValid && s_atlas.rgb != 0;
    x_glDisable(GL_BLEND);
    x_glDisable(GL_DEPTH_TEST);
    glUseProgram(s_cursProg);
    x_glUniform2f(s_uCursSize, (float)s_sharpW, (float)s_sharpH);
    glUniform1i(s_uCursCK, (int)e->ck);
    glUniform1i(s_uCursRestored, restored ? 1 : 0);
    /* units 1 and 2 must hold real textures even when the branch is off */
    x_glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_palTex);
    x_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_atlas.rgb ? s_atlas.rgb : s_palTex);
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_atlas.tex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    quad(v, (float)x0, (float)y0, (float)(x0 + w), (float)(y0 + h), e->u0, e->v0, e->u1, e->v1);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    x_glDrawArrays(GL_TRIANGLES, 0, 6);
    s_curDrawn++;
}

/* THE MINIMAP'S BASE AT ITS NATIVE SIZE (13.6, G17e).

   The engine fits the TNT's `TED_GENERATED_PIC` into a 126-px box and throws
   half of what it has away; the picture is 252x252. Drawing it at its own size
   into the device-res layer is a free 2x with no new data path and no colour
   drift — and the layer is the ONLY place it can live, because the engine's
   minimap reaches the frame as a copy of the 126-px composite `+0x142DB`, so a
   twin can never hold more than 126 px there.

   BEHIND `mmbase`, AND THAT IS NOT A DEFAULT WAITING TO HAPPEN. On its own
   this covers the engine's minimap with a base that has no fog, no unit dots,
   no radar arcs and no view box — every one of which is still the engine's
   until the rest of G17e lands. Shipping it on would hide information the
   player is entitled to, which is worse than a soft picture. */
static void sharp_minimap(const TAGPU_FRAME* f)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const unsigned char* pic = NULL;
    unsigned gen = 0;
    int pw = 0, ph = 0, mx, my, mw, mh;
    float kx, ky, v[24];

    if ((!s_mmbase && !s_mmdots) || !s_cursProg || !ptr_ok(ta)) return;
    /* NOT `+0x142F1 & 2`, which is what DrawMinimap 0x466B00 tests: that is a
       DIRTY flag and 0x466B16 CLEARS it in the same breath, so it reads 0 on
       almost every frame [MEASURED 2026-09-09 — the first build of this gated
       on it and drew nothing at all, ever]. The engine can afford a dirty flag
       because its copy lands in the game offscreen and stays there until
       something overdraws it; the sharp layer is cleared at every present, so
       ours has to be redrawn every frame. The honest gate is that the minimap
       surfaces exist at all, which is what being in a game with one means. */
    if (!ptr_ok(*(const void* const*)(ta + MM_COMPOSITE))) return;
    if (s_mmbase) {
        if (!tagpu_gui_minimap_pic(&pic, &pw, &ph, &gen)) return;
        if (pw <= 0 || ph <= 0) return;
    }
    if (s_mmbase && (gen != s_mmGenSeen || !s_mmTex)) {
        if (!s_mmTex) glGenTextures(1, &s_mmTex);
        if (!s_mmTex) return;
        glBindTexture(GL_TEXTURE_2D, s_mmTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, pw, ph, 0, GL_RED, GL_UNSIGNED_BYTE, pic);
        glBindTexture(GL_TEXTURE_2D, 0);
        s_mmGenSeen = gen; s_mmTW = pw; s_mmTH = ph;
    }
    /* the box the engine fitted it into, in ITS screen pixels, read live: it is
       0x0 at BuildMinimapSurface's entry, since that call is what computes it */
    mx = *(const short*)(ta + MM_OFFX); my = *(const short*)(ta + MM_OFFY);
    mw = *(const short*)(ta + MM_W);    mh = *(const short*)(ta + MM_H);
    if (mw <= 0 || mh <= 0) return;
    kx = (f->game_width  > 0) ? (float)f->vp_w / (float)f->game_width  : 1.0f;
    ky = (f->game_height > 0) ? (float)f->vp_h / (float)f->game_height : 1.0f;
    x_glDisable(GL_BLEND);
    x_glDisable(GL_DEPTH_TEST);
    glUseProgram(s_cursProg);
    x_glUniform2f(s_uCursSize, (float)s_sharpW, (float)s_sharpH);
    glUniform1i(s_uCursCK, -1);          /* no colour key: every texel of the picture draws */
    glUniform1i(s_uCursRestored, 0);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_palTex);
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, s_palTex);
    x_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_mmTex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* THE WHOLE PICTURE INTO THE WHOLE BOX, and that is the engine's own
       mapping rather than a guess: 0x466845 builds a context for the box-sized
       surface and 0x46685F hands the picture straight to the stretch
       `0x4B95A0`, so the 252x252 square is squashed into an aspect-correct box
       (106x126 on a 336x400 map). Full 0..1 UVs reproduce exactly that. */
    if (s_mmbase) {
        quad(v, (float)mx * kx, (float)my * ky,
                (float)(mx + mw) * kx, (float)(my + mh) * ky, 0.0f, 0.0f, 1.0f, 1.0f);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
        x_glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    /* THE ENGINE'S DOTS, ON TOP, AT THE SAME SCALE IT DRAWS THEM. A dot's
       position is in composite pixels and the composite IS the box, so it maps
       to the screen by the box's own offset and to the device by k — the same
       transform the base just took. Scaled by k rather than kept at 1x device
       like the cursor: a dot is 2-3 px of engine art whose SIZE carries meaning
       against the map under it, and the engine's own is blown up by k anyway. */
    if (s_mmNDots > 0) {
        int i;
        /* the first few, named: a dot list that is full of positions the box
           does not contain is the failure this replay can have, and it is
           invisible from a screenshot */
        if (s_mmLogged < 4) {
            char b[190];
            s_mmLogged++;
            _snprintf(b, sizeof b, "gui: minimap dots=%d box=(%d,%d) %dx%d first=(%d,%d) %ux%u ck=%u",
                      s_mmNDots, mx, my, mw, mh, (int)s_mmDots[0].sl, (int)s_mmDots[0].st,
                      (unsigned)s_mmDots[0].fw, (unsigned)s_mmDots[0].fh, (unsigned)s_mmDots[0].ck);
            b[sizeof b - 1] = '\0';
            slog(b);
        }
        for (i = 0; i < s_mmNDots; i++) {
            const MMDOT* d = &s_mmDots[i];
            const TAGPU_GAFENT* e = tagpu_gaf_atlas_find(&s_atlas, d->frame, d->pix, d->fw, d->fh);
            if (!e) continue;             /* never atlased: the frame is the base copy's problem */
            glUniform1i(s_uCursCK, (int)e->ck);
            x_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_atlas.tex);
            quad(v, (float)(mx + d->sl) * kx, (float)(my + d->st) * ky,
                    (float)(mx + d->sl + d->fw) * kx, (float)(my + d->st + d->fh) * ky,
                    e->u0, e->v0, e->u1, e->v1);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
            x_glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }
    /* THE VIEW BOX LAST, because that is where the engine puts it: DrawMinimap
       copies the composite at 0x466B44 and only then draws the box at
       0x466B5E. Getting that order wrong is not theoretical — with the dots
       replayed over the engine's own frame and no box of ours, the only four
       pixels in the whole picture that differed were the box's, overwritten by
       a dot [MEASURED 2026-09-09]. Its rect is `main+0x142CB`, four ints in
       SCREEN pixels with inclusive edges (it is drawn into the game offscreen,
       not the composite), and tagpu_zoom.c already keeps it honest at zoom. */
    if (s_mmbase) {
        const int* vr = (const int*)(ta + MM_VIEWRECT);
        int ci = (int)*(const unsigned char*)(ta + MM_VIEWCOL);
        float r = s_palCopy[4 * ci] / 255.0f, g2 = s_palCopy[4 * ci + 1] / 255.0f,
              b2 = s_palCopy[4 * ci + 2] / 255.0f;
        int L = vr[0], T = vr[1], R = vr[2], B = vr[3];
        if (s_mmLogged < 5) {
            char lb[190];
            s_mmLogged = 5;
            /* the FIRST frame's values, which is the point — it says the rect
               and the colour were read at all. The rect moves with the camera
               and the first frame after a load is not where it settles, so do
               not read a stale box out of this line. */
            _snprintf(lb, sizeof lb, "gui: minimap view box, first frame: (%d,%d)-(%d,%d) idx=%d rgb=%.0f,%.0f,%.0f k=%.2f,%.2f",
                      L, T, R, B, ci, r * 255.0f, g2 * 255.0f, b2 * 255.0f, kx, ky);
            lb[sizeof lb - 1] = '\0';
            slog(lb);
        }
        if (R >= L && B >= T) {
            int e;
            glUseProgram(s_sharpProg);
            x_glUniform2f(s_uSharpProgSize, (float)s_sharpW, (float)s_sharpH);
            x_glUniform4f(s_uSharpProgCol, r, g2, b2, 1.0f);
            glBindVertexArray(s_vao);
            glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
            /* four one-GAME-pixel edges, so the box keeps the weight the engine
               gives it rather than thinning to a device pixel as k grows */
            for (e = 0; e < 4; e++) {
                float x0, y0, x1, y1;
                if (e == 0)      { x0 = (float)L;     y0 = (float)T;     x1 = (float)(R + 1); y1 = (float)(T + 1); }
                else if (e == 1) { x0 = (float)L;     y0 = (float)B;     x1 = (float)(R + 1); y1 = (float)(B + 1); }
                else if (e == 2) { x0 = (float)L;     y0 = (float)T;     x1 = (float)(L + 1); y1 = (float)(B + 1); }
                else             { x0 = (float)R;     y0 = (float)T;     x1 = (float)(R + 1); y1 = (float)(B + 1); }
                quad(v, x0 * kx, y0 * ky, x1 * kx, y1 * ky, 0, 0, 0, 0);
                glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
                x_glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
    }
    s_mmDrawn++;
}

static void sharp_begin(const TAGPU_FRAME* f)
{
    int w = f->vp_w, h = f->vp_h;
    s_sharpOn = 0;
    /* A FAILURE LATCHES, like init_gl's s_gl = 2. sharp_drop() zeroes the ids,
       so without this every present would re-enter the allocation below --
       generating, sizing and deleting a vp_w x vp_h texture and appending a log
       line 60 times a second for the rest of the session, which buries the
       heartbeat. The layer is additive, so staying off costs sharpness only. */
    if (s_sharpFailed) return;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    if (s_sharpTex && (s_sharpW != w || s_sharpH != h)) sharp_drop();
    if (!s_sharpTex) {
        GLenum st;
        glGenTextures(1, &s_sharpTex);
        glGenFramebuffers(1, &s_sharpFbo);
        if (!s_sharpTex || !s_sharpFbo) {
            slog("gui: sharp layer — no texture/framebuffer id, the mirror alone");
            sharp_drop(); s_sharpFailed = 1; return;
        }
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
            s_sharpFailed = 1;
            return;
        }
        s_sharpW = w; s_sharpH = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_sharpFbo);
    glViewport(0, 0, w, h);
    x_glDisable(GL_SCISSOR_TEST);
    x_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    x_glClear(GL_COLOR_BUFFER_BIT);
    if (s_sharptest && s_sharpProg) {
        /* THE HARNESS LEVER, never for a player, and the only thing in G17a
           that puts a texel in this layer: a 64x64 opaque green square at the
           viewport's TOP-LEFT and a one-DEVICE-pixel white column at device
           x = 100. Between them they prove the five things the gate cannot
           otherwise see — the layer exists at the device resolution, it
           composites ABOVE the mirror, alpha is what gates it, row 0 is the
           top, and SHARP_VS puts a client's GEOMETRY the right way up. It is
           drawn as quads rather than scissored clears precisely because
           geometry is what G17c and G17d will use, and a scissor box would
           have proved the convention for the one client kind that never
           needs it. */
        float v[24];
        x_glDisable(GL_BLEND);
        x_glDisable(GL_DEPTH_TEST);
        glUseProgram(s_sharpProg);
        x_glUniform2f(s_uSharpProgSize, (float)w, (float)h);
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        x_glUniform4f(s_uSharpProgCol, 0.0f, 1.0f, 0.0f, 1.0f);
        quad(v, 0.0f, 0.0f, 64.0f, 64.0f, 0, 0, 0, 0);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
        x_glDrawArrays(GL_TRIANGLES, 0, 6);
        x_glUniform4f(s_uSharpProgCol, 1.0f, 1.0f, 1.0f, 1.0f);
        quad(v, 100.0f, 0.0f, 101.0f, (float)h, 0, 0, 0, 0);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
        x_glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    sharp_cursor(f);                    /* 13.5's cursor: the layer's first real client */
    sharp_minimap(f);                   /* 13.6's base, behind `mmbase` while it is alone */
    /* THE CLEAR COLOUR IS MODULE-WIDE STATE AND WE OWN IT AT (0,0,0,0).
       render_ogl.c repaints the letterbox bars with a bare glClear on EVERY
       frame whose viewport is offset (`if (viewport.x || viewport.y)`), and
       tagpu_overlay_capture_begin does the same for a glshot — neither sets a
       colour of its own, so whatever we leave here is what they paint. The
       clear above already ends at (0,0,0,0) on the normal path; this is the
       statement of the invariant, not a second setter.
       `sharp_begin` also leaves the VIEWPORT at (0,0,w,h) with the scissor
       test off and the default framebuffer bound. That is safe only because
       tagpu_gui_present rebinds the target FBO and the frame's viewport after
       draw_layer -- which can return early -- so a future client drawing here
       must not assume draw_layer ran. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_sharpOn = 1;
}

static void draw_layer(const TAGPU_FRAME* f)
{
    TWIN* t = s_presented ? twin_find(s_presented) : NULL;
    float v[24], ky;
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
    /* THE RECT tagpu_gui_cursor_frame READ, not a second read of the globals:
       the world composite was given that one before the native pass ran, and
       a cursor that moved in between would leave the two halves erasing
       different rectangles. */
    x_glUniform4f(s_uLayCursor, s_curEng[0], s_curEng[1], s_curEng[2], s_curEng[3]);
    glUniform1i(s_uLayCursOurs, s_curOwn ? 1 : 0);
    /* k, and with it the ramp's width (13.3): device pixels per twin texel.
       The twin is the engine's surface 1:1, so this is exactly 13.1's k — 1.0
       for as long as the engine's screen IS the window. That is NOT the same
       as "always 1 in phase 1": any window the player drags off the game
       resolution lands here fractional, and so does the 640x480 shell left in
       a bigger client. Below 1 the fork is scaling the engine DOWN into a
       smaller window; the ramp is held at plain bilinear there rather than
       widened past a texel. */
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
    /* `mmbase` (G17e): the TNT's 252-px picture drawn into the sharp layer over
       the engine's 126-px minimap. Harness only while it is the base ALONE —
       no fog, no dots, no arcs, no view box. */
    s_mmbase = on && strstr(buf, "mmbase") != NULL;
    /* `mmdots`: the dot replay ALONE, over the engine's own minimap. It is the
       oracle for the replay and the reason it exists — if our dots land where
       the engine's do, in the colours the engine used, the frame does not
       change at all, and any difference is exactly our error. The base cannot
       be tested that way (it is a different resample by construction), so it
       gets its own token. */
    s_mmdots = on && strstr(buf, "mmdots") != NULL;
    /* `nocursor`: phase 1's cursor, the engine's own, kept as the A/B against
       ours — and the escape if the sprite record ever stops being a GAF frame
       header on some build. `cursorscale=N` (13.5) sizes ours in DEVICE
       pixels; 1 is the default and the convention, and it is clamped rather
       than trusted because a 0 or a wild value would put a garbage quad in
       the layer on every frame. */
    s_nocursor = on && strstr(buf, "nocursor") != NULL;
    {
        const char* q = on ? strstr(buf, "cursorscale=") : NULL;
        double sc = q ? atof(q + 12) : 1.0;
        if (!(sc >= 0.25) || sc > 8.0) sc = 1.0;
        s_cursorScale = (float)sc;
    }
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
        /* 302 bytes of literal + 44 conversions: the worst case is ~700, and
           _snprintf does not NUL-terminate what it truncates */
        char b[768];
        static LARGE_INTEGER t0, fq;
        LARGE_INTEGER t1;
        double fps = 0.0;
        unsigned gCached = 0, gDrops = 0; int gFonts = 0;
        tagpu_text_glyph_stats(&gCached, &gDrops, &gFonts);
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t1);
        if (t0.QuadPart) fps = (double)(f->frame_counter - last) * (double)fq.QuadPart / (double)(t1.QuadPart - t0.QuadPart);
        t0 = t1;
        last = f->frame_counter;
        _snprintf(b, sizeof b, "gui: twins=%d presented=%08X drained=%u seeds=%u sprites=%u copies=%u pixels=%u clears=%u atlas=%d/%d lost=%u strict=%d resets=%u overflows=%u stalls=%u skipped=%u palchg=%u paldiff=%d@%d palsrc=%d cpp=%d col=%u/%d colvalid=%d rearms=%u rgb=%u k=%.3f sharp=%dx%d curs=%d,%dx%d,dev=%d,sc=%.2f,drawn=%u,warm=%u str=%u/%u,miss=%u,reseed=%u,glyphs=%u/%u,fonts=%d arena=%u mm=%u/%u/%u,dots=%d%s,lost=%u fps=%.1f",
                  s_ntwins, s_presented, s_drained, s_seeds, s_sprites, s_copies, s_pixels, s_clears,
                  s_atlas.n, s_atlas.max, s_lostSprites, s_strict, g_guiq.resets, g_guiq.overflows, g_guiq.stalls,
                  s_skipped, s_palChanges, s_palDiff, s_palDiffAt, s_palSource,
                  tagpu_classicpp_on() ? 1 : 0, s_colTwins, s_ntwins, s_colValid, s_rearms, s_atlas.rgb,
                  s_k, s_sharpW, s_sharpH,
                  s_curOwn, s_curW, s_curH, s_curDev, s_cursorScale, s_curDrawn, s_curWarm,
                  s_strings, s_glyphs, s_strMiss, s_strReseed, gCached, gDrops, gFonts,
                  /* the arena head is MONOTONIC, so the delta between two of these
                     lines is the bytes the producer wrote in 300 frames — which is
                     what `nostring` is A/B'd on (13.4: ~40 bytes where a text op
                     carried ~968) and what §7's cadence note is about */
                  g_guiq.aHead, s_mmDrawn, s_mmSprites, s_mmOther,
                  s_mmNDots, s_mmDotsFull ? "+" : "", s_mmDotLost, fps);
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
    s_sharpProg = 0; s_sharpFailed = 0;      /* a new context deserves a fresh try */
    s_cursProg = 0; s_curOwn = 0; s_curFrame = NULL;   /* and the cursor is nobody's until it is re-atlased */
    s_strProg = 0;
    s_mmTex = 0; s_mmGenSeen = 0;       /* the picture's texture died; the BYTES are the hook's */
    tagpu_text_glreset();               /* the glyph atlas's texture id died too; its CELLS are CPU-side */
    tagpu_gaf_atlas_lost(&s_atlas);
    memset(s_palCopy, 0xFF, sizeof s_palCopy);        /* the palette texture died too: re-upload */
    s_skipToReset = 1;
    g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_GLCTX;
}

int tagpu_gui_drawing(void) { return s_on; }
