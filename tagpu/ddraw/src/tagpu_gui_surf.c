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
#include "tagpu_hud.h"
#include "tagpu_terr.h"
#include "tagpu_terrown.h"           /* is the world viewport carrying our key fill right now? */
#include "tagpu_overlay.h"
#include "tagpu_pal.h"
#include "tagpu_surf.h"                    /* the one resolution of the presented palette */
#include "tagpu_vk.h"                 /* tagpu_vk_owns_present, tagpu_vk_ab_arm:
                                         which lane this is, and the A/B's arming */
/* `opengl_utils.h` was included here until [landing 11-4b]. This file now has no
   GL dependency at all -- no `gl*` call, no `GL*` type, no `GL_*` macro, no
   `oglu_*` or `xwgl*` -- which makes it the FIRST of the nineteen includers to
   leave that header. That matters for 11-5, whose plan is to delete
   `opengl_utils.{c,h}`: the other eighteen still use it (5 to 44 `gl*` calls
   each), so the header cannot go until their GL bring-up does. */
#include "dd.h"                         /* g_ddraw.cursor: the pointer the fork last saw (13.5) */
#include "ddsurface.h"                  /* G19f: g_ddraw.primary->surface/pitch, the composite's
                                           bottom layer as BYTES (dd.h only forward-declares it) */
#include "mouse.h"                      /* mouse_last_client: the pointer at the DEVICE's resolution (13.5) */

/* The minimap's box, its three surfaces and the view box over them were read
   here, on this thread, until the frame packet's landing 4c; every one of them
   is a packet field now and the addresses live in inc/tagpu_engine.h with the
   publisher. `+0x142F1 bit 1` is NOT among them and never was: it is
   DrawMinimap 0x466B00's DIRTY flag, which 0x466B16 clears in the same breath,
   so it reads 0 on almost every frame. */
#define POLL_MS        500
#define MAX_TWINS      32
#define ATLAS_DIM      2048
#define ATLAS_MAX      4096
#define UI_RESTORE_PRIO 4       /* terrain 0, features 1, effects 2, 3DO units 3 */
#define UI_RESTORE_MIN 12       /* G15-0's verdict: nothing under 12x12 is restored */
#define PAL_SETTLE     30       /* frames the palette must hold still before a re-arm */

extern volatile int g_gui_draw;         /* tagpu_gui_hook.c: the publisher's gate */


static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* ------------------------------------------------------------------ state */
/* THE GL IDS (`tex`, `fbo`, `rgb`) WENT WITH THE DRAW [landing 11-4b]. What is
   left is the BOOKKEEPING the mirror op stream needs: which engine surface this
   twin stands for and how big it is, so `twin_find` can resolve a surface
   pointer to a slot and the ops can name it. */
typedef struct TWIN {
    unsigned surf;                      /* the engine surface's pixel base     */
    int w, h;
} TWIN;
static TWIN   s_twins[MAX_TWINS];
static int    s_ntwins = 0;
static unsigned s_presented = 0;        /* the last PK_FRAME's surface         */

static TAGPU_GAFATLAS s_atlas;
static TAGPU_GAFENT   s_ents[ATLAS_MAX];

static int    s_on = 0, s_strict = 0;
static DWORD  s_lastPoll = 0;
static unsigned s_drained = 0, s_sprites = 0, s_copies = 0, s_pixels = 0, s_seeds = 0, s_clears = 0, s_lostSprites = 0;
/* PK_PIXELS OPS THE DRAIN REFUSED TO CARRY -- the engine's own composed
   bytes, which may not reach the screen. `pixels=` beside it is pinned at 0
   and kept deliberately: two counters that used to be one are how a reader
   sees that the traffic did not stop, the CARRYING did. */
static unsigned s_pixDropped = 0;
/* SOLID RECTANGLES REPLAYED AS GEOMETRY, counted beside `pixels=` so the two
   can be read against each other: every one of these used to be a `PK_PIXELS`
   box of arena bytes. [The vulkan-only plan, landing 8a.] */
static unsigned s_bars = 0;
/* HOLLOW RECTANGLES replayed as geometry, beside `bars=` for the same reason.
   [The vulkan-only plan, landing 8b.] */
static unsigned s_rects = 0;
static int    s_skipToReset = 0;        /* after a GL context change: the queue's ops up to the producer's next
                                           RESET were published against twins and an atlas that died with the
                                           context — take their arena bytes, apply nothing (see drain) */
static unsigned s_skipped = 0;
/* Classic++ (G15e) */
static int    s_norestore = 0;          /* `norestore` in the trigger: the A/B lever   */
/* PINNED AT 0 SINCE [landing 11-4b], AND KEPT DELIBERATELY. `restore_step`, which
   was the only thing that raised it, went with the draw half. That makes it the
   same shape as landing 11-4a's `s_state` -- with one difference that decides it:
   `s_state` was a READINESS GATE, so a future `if (s_state != 1) return;` would
   have silently stopped the pass publishing anything. This is a VALUE that is
   published (`s_mHand.colourTwins`) and logged, and 0 is the correct value --
   there are no colour twins on this lane. Removing it would change the hand-over's
   shape, which is a protocol change for `tagpu_vk_gui.c` rather than a deletion,
   so it is out of this landing's scope and named here instead of done quietly. */
static int    s_colValid = 0;           /* no colour twins on this lane; see above     */
static unsigned s_rearms = 0, s_colTwins = 0;
/* Phase 2's seam (G17a) */
static int    s_sharpW, s_sharpH;       /* its size, = the frame's viewport in window px               */
static int    s_sharpOn = 0;            /* it exists and may be sampled this frame                     */
/* G19f: ...AND WHETHER ANYTHING IS ACTUALLY IN IT. `s_sharpOn` says the layer
   exists and is bound, which it is on every frame once it has been made -- so
   it is the wrong question for a port that does not carry the layer's CONTENT.
   An EMPTY layer and a DISABLED one composite identically (it is taken only
   where its alpha says it has coverage, and a cleared one has none anywhere),
   so the Vulkan lane may draw a frame whose layer is empty and must refuse one
   whose layer is not. [FOUND 2026-09-16, the first run of the UI A/B: with
   `nocursor nominimap nostring` armed the pass stood down on every frame and
   the counters all read 0.] */
static int    s_sharpInk = 0;
static int    s_sharptest = 0;          /* the harness lever that proves the layer is wired            */
/* OUR CURSOR'S ANSWER FOR THIS PRESENT, latched here and taken once per frame
   by the render_ogl.c bracket (tagpu_gui.h has the whole argument). Set only
   at the tail of a successful sharp_cursor; cleared by the take, so a frame
   that never reaches this module answers 0 and the engine keeps its cursor. */
static int s_curInLayer = 0;            /* drawn into the sharp FBO                      */
static int s_curDrew = 0;               /* ...and that FBO was composited to the screen  */

int tagpu_gui_cursor_drew_take(void)
{
    int v = s_curDrew;
    s_curDrew = s_curInLayer = 0;
    return v;
}
static unsigned s_strings = 0;          /* string ops stamped                                          */
static unsigned s_glyphs = 0;           /* glyph quads drawn                                           */
static unsigned s_strMiss = 0;          /* glyphs the cache would not give (the engine drew them)      */
static unsigned s_strReseed = 0;        /* strings that stamped NOTHING and asked for a fresh seed     */
static unsigned s_strRepack = 0;        /* gathers restarted because the glyph atlas repacked under them */
/* G17e: the TNT's own 252-px minimap picture, uploaded once per map load */
static unsigned s_mmGenSeen;            /* the generation s_mmTex holds; 0 = nothing        */
/* THE BAKE HAPPENED, which is not the same as the GL texture existing: the
   resolve lands in `s_mmPicRgb` for the Vulkan twin whether or not there is a
   GL lane to upload it on. NOT cleared on a context loss -- the bytes survive
   one, and `s_mmGenSeen = 0` there already re-enters the block that re-creates
   the texture. [The vulkan-only plan, landing 4b-3.] */
static int      s_mmBaked;
static int      s_mmTW, s_mmTH;         /* its size in texels                               */
static int      s_mmbase = 0;           /* the minimap is ours (see the k rule in sharp_minimap) */
static int      s_mmforce = 0;          /* token `mmbase`: draw it at k = 1 too, for the harness */
static unsigned s_mmDrawn = 0;
static unsigned s_mmLogged;
static unsigned s_mmNoEng;              /* frames the engine's pair could not be read          */
static unsigned char* s_mmPicRgb;       /* the picture resolved through the presented palette  */
static unsigned s_mmPicCap;
/* G19f landing 3: the CONTENT serial of `s_mmPicRgb`, for the same reason the
   glyph atlas needed one -- a second backend cannot read the GL texture and
   must know when the bytes behind it moved. It moves on a map load and on a
   palette change, which are exactly the two things that re-resolve it. */
static unsigned s_mmPicSerial;
static unsigned s_mmPalSeen;            /* tagpu_pal_serial() when it was last resolved        */
static unsigned s_mmFogged;             /* engine texels where fogged != unfogged, this frame  */

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
static float  s_hudS = 1.0f;            /* HUD scale in force this frame (20); 1.0 is the feature off */

/* The GL entry-point block (13 `PFN_*` typedefs, 13 `x_gl*` pointers, `getgl`),
   `mkprog`, `quad` and the 30-odd `s_u*` uniform locations STOOD HERE and went
   with the draw [the vulkan-only plan, landing 11-4b]. Nothing in this file
   resolves a GL entry point or holds a GL object any more.

   ----------------------------------------------------------------- shaders */
/* a quad in surface pixels -> the twin's FBO (row 0 = surface row 0) */
/* THESE NINE ARE A BUILD INPUT, NOT CODE THIS FILE RUNS. With the GL half gone
   [landing 11-4b] nothing here references them -- but `tools/spirv-gen.py` reads
   them out of the PREPROCESSED translation unit and generates
   `inc/spirv/tagpu_gui_surf.spv.h`, which `tagpu_vk_gui.c` includes and draws
   with. This file holds more of them than any other: all seven `gui_*` programs
   of the PROGRAMS table are built from this pair-set. Deleting them fails the
   build loudly ("the manifest names tagpu_gui_surf::QVS and the source does not
   have it") rather than silently, but they must not be deleted at all. The
   pragma is local and paired; `__attribute__((unused))` also works, as a PREFIX
   only -- see the vulkan-only plan's 11-4a entry for the placement table. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
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
/* THE MINIMAP (13.6, G17e): our 252-px base, MASKED BY THE ENGINE'S OWN FOG.

   THE VISIBILITY DECISION STAYS THE ENGINE'S, which is 13.6's rule for the
   dots applied to the fog — and it has to be, because §13.6's stated fog
   source does not exist. The corner-mask grid the world passes hold is built
   around the eye and covers the VIEWPORT (29x23 cells against a 336x400 map),
   so it says nothing about the rest of the minimap; and the TNT picture is the
   whole map with nothing hidden, so a base drawn without fog would show the
   player terrain they have never explored.

   So the shader compares the engine's two 126-px surfaces: `+0x142DF`, the
   base with its fog shading, against `+0x142E3`, the same base without. Where
   they AGREE the engine is showing true terrain and our sharper copy of that
   same terrain is safe; where they differ the engine is hiding or shading
   something and its own pixel is used verbatim.

   AND EVERYTHING THE ENGINE DREW ON TOP COMES BACK THE SAME WAY. `+0x142DB`,
   the composite, is the fog base plus the unit dots, the radar coverage arcs
   and DrawPoint's points; it differs from `+0x142DF` exactly where one of
   those landed, so one comparison carries all three. That is what retired the
   dot replay this gate started with: the replay was measured pixel-exact
   against the engine, but it could only ever carry the DOTS — the arcs
   (0x4C0070) and the points (0x4BEE60) are not observed leaves (§7) and each
   would have needed its own rasteriser reproduced exactly. One mechanism that
   carries all three, from the engine's own pixels, beats two that do not.

   THE FOG TEST IS OVER A 3x3 NEIGHBOURHOOD, not one texel, and that is the
   whole safety argument. A shaded pixel can land on the same palette index it started
   from (the shade is a LUT into a dark-grey ramp, so a pixel already in that
   ramp maps to itself), and a single-texel test would then let four of OUR
   sub-texels through — sub-texels taken from the unfogged picture, which may be
   bright. Requiring the whole neighbourhood to agree costs a one-texel band of
   the engine's own resolution around every fog edge.

   WHAT THAT BUYS, EXACTLY [landing review, 2026-09-09]: no unit, arc or point
   can leak -- those come from the composite verbatim -- and terrain is bounded
   but not proven, because the test is at the engine's 126 px while the base it
   admits is the 252-px picture, so a fog-invariant neighbourhood can still
   carry a sub-texel the engine's own downsample never picked. Measured: 2
   differing pixels of 13 356 on a 99.6 % fogged map, both inside the engine's
   lit region. */
static const char* MM_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uPic; uniform sampler2D uEng; uniform sampler2D uPal;\n"
    "uniform ivec2 uEngSize;\n"
    "void main(){\n"
    "  ivec2 p = clamp(ivec2(uv * vec2(uEngSize)), ivec2(0), uEngSize - 1);\n"
    "  vec3 e = texelFetch(uEng, p, 0).rgb;\n"
    /* ANYTHING THE ENGINE DREW ON TOP OF ITS FOGGED BASE WINS, verbatim: the
       composite differs from the fog base exactly where a unit dot, a radar
       arc or a DrawPoint landed. That is one test for all three, and it is why
       there is no arc replay — 0x4C0070 and 0x4BEE60 are not observed leaves
       (§7) and would each need their own rasteriser reproduced pixel-exactly. */
    "  if (abs(e.b - e.r) > 0.5 / 255.0) {\n"
    "    frag = vec4(texture(uPal, vec2((e.b * 255.0 + 0.5) / 256.0, 0.5)).rgb, 1.0); return; }\n"
    "  bool clean = true;\n"
    "  for (int dy = -1; dy <= 1; ++dy)\n"
    "    for (int dx = -1; dx <= 1; ++dx) {\n"
    "      ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0), uEngSize - 1);\n"
    "      vec2 g = texelFetch(uEng, q, 0).rg;\n"
    "      if (abs(g.r - g.g) > 0.5 / 255.0) clean = false;\n"
    "    }\n"
    /* OUR BASE IS SAMPLED AS COLOUR, not as an index, and it has to be. At
       1 < k < 2 the box is SMALLER than the 252-px picture, so this is a
       downsample, and a downsample wants a filter — but interpolating palette
       INDICES is meaningless (13.3 says so about the mirror and it is just as
       true here). So the picture is uploaded already resolved through the
       presented palette and sampled RGB, with MIN linear and MAG nearest: the
       shrink is filtered, the blow-up at k > 2 stays crisp. */
    "  if (clean) { frag = vec4(texture(uPic, uv).rgb, 1.0); return; }\n"
    "  frag = vec4(texture(uPal, vec2((e.r * 255.0 + 0.5) / 256.0, 0.5)).rgb, 1.0); }\n";
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
       a cursor a Gamma step darker than the panel under it would show.
       AND IT IS THE ONLY PATH THIS PROGRAM TAKES: `uRestored` is passed 0 at
       every call (sharp_cursor carries the argument, gui-renderer.md 24.1).
       The twin branch above is kept because the uniform is shared with the
       sprite program, not because a cursor may take it again. */
    "  frag = vec4(texture(uPal, vec2((i * 255.0 + 0.5) / 256.0, 0.5)).rgb, 1.0); }\n";
static const char* LAY_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    /* `uSurf` STOOD IN THIS LIST AND IS GONE WITH THE THREE BRANCHES THAT READ
       IT -- the stale-mirror guard, the `uStrict` harness and the cursor-rect
       discard. All three existed because TA's own composed frame was the layer
       UNDERNEATH this one: the guard asked "has the engine painted something
       here the publisher never saw", `strict` diffed us against it, and the
       cursor rect was left unpainted so the engine's own blit could show
       through. Nothing of the engine's reaches the screen any more, so all
       three questions are about a layer that does not exist, and the sampler
       they shared is not declared rather than bound to a dummy: the rule is
       about SAMPLING, and a pipeline that cannot name the image cannot break
       it. [THE CLEAN CUT; the UI rebuild keeps the cut and restores the draw.]
       `uCursor` went with them -- it only ever bounded those tests. */
    "uniform sampler2D uTwin; uniform sampler2D uPal;\n"
    "uniform sampler2D uTwinCol; uniform sampler2D uSharp;\n"
    "uniform int uColOn; uniform int uSharpOn;\n"
    "uniform ivec2 uSize; uniform ivec2 uSharpSize; uniform vec2 uScale;\n"
    "uniform vec4 uVp;\n"
    "uniform int uVpKey;\n"
    /* HUD SCALE (gui-renderer.md 22, and 22.5 for why nothing is "reserved"):
       x = the panel's width ON SCREEN, y = the bars' height on screen, z = 1/s,
       w = s. w == 1 is the whole feature off and every line below is then the
       identity, which is the s = 1 gate. */
    "uniform vec4 uHud;\n"
    /* ONE TAP OF THE MIRROR, premultiplied by its coverage. rgb is the
       restored colour where this texel has one and the live palette
       everywhere else (the per-texel rule of 3.4, unchanged); a is coverage,
       so an uncovered texel contributes NOTHING to a blend instead of
       dragging index 0 in from a box the key fill erased. */
    "vec4 tap(ivec2 p){\n"
    "  p = clamp(p, ivec2(0), uSize - 1);\n"
    "  vec2 g = texelFetch(uTwin, p, 0).rg;\n"
    "  if (g.g <= 0.5) return vec4(0.0);\n"
    /* THE KEY IS NOT UI, AND INSIDE THE VIEWPORT IT IS ALL WE EVER MIRROR.
       The publisher hands us a box's bytes verbatim (pub_surface_bytes) and
       twin_upload stamps coverage 255 on every one of them, key included -- so
       any engine drawer still running inside the world viewport publishes a
       rectangle of tagpu_terrown's fill, and without this rule the layer
       resolves index 254 through the palette and paints BRIGHT CYAN, opaque,
       over the world composite. [MEASURED 2026-09-09: the engine's selection
       rect is drawn as four DrawLine 0x4BE950 calls, each recorded as its own
       axis-aligned BOUNDING BOX, so one rotated rect published four boxes whose
       union is a ~44 px cyan square with a hole -- 15 frames in 3600 of
       ordinary play, and 12 of 12 with the engine's rects forced on.]
       The composite makes the same judgement one layer down (tagpu_native.c
       CFS, "THE KEY FILL MUST NEVER REACH THE SCREEN"); this is that rule for
       the layer above it. Coverage 0 is exactly the right answer: it is what
       "no UI here" already means to every reader of the twin, so the ramp
       blends it as absence rather than dragging cyan into its neighbours.
       uVpKey is -1 whenever the viewport is NOT ours (tagpu_terrown_filled),
       so with the terrain pass off -- where the engine's own art fills the
       viewport and 254 would be a real colour -- the rule is inert. */
    "  if (uVpKey >= 0 && int(g.r * 255.0 + 0.5) == uVpKey &&\n"
    "      float(p.x) >= uVp.x && float(p.x) < uVp.x + uVp.z &&\n"
    "      float(p.y) >= uVp.y && float(p.y) < uVp.y + uVp.w) return vec4(0.0);\n"
    "  if (uColOn != 0) { vec4 c = texelFetch(uTwinCol, p, 0);\n"
    "    if (c.a > 0.5) return vec4(c.rgb, 1.0); }\n"
    "  return vec4(texture(uPal, vec2((g.r * 255.0 + 0.5) / 256.0, 0.5)).rgb, 1.0); }\n"
    "void main(){\n"
    /* d IS THE DESTINATION POINT, in engine pixels, and stays that. The HUD
       map below moves only where the MIRROR IS SAMPLED; the cursor rect and
       `strict` both ask about the engine's own frame, which is at the
       destination — the engine blits its cursor and holds its UI pixels at
       screen positions, not at the twin positions we magnify from. */
    "  vec2 d = uv * vec2(uSize);\n"
    /* THE HUD'S REGION MAP RUNS FIRST, and `p`/`f` below are the SOURCE texel
       it chose, not the dest fragment. Everything downstream of here indexes
       the engine's own surface -- `uCursor` is "the engine's own rect, GAME
       px", `uVp` is the engine's viewport rect, and uTwin/uSurf are the twin
       and the primary -- so all of them have to be asked about the texel the
       colour actually came from. At s == 1 `sd` IS `d` and this is the
       identity, which is why the s = 1 gate and main's stale-mirror guard are
       untouched by it.

       This was wrong for one build (22.5): the map sat BELOW the cursor test
       and the guard, so in a magnified region the guard compared the twin at
       the dest texel -- out in the world, where our key fill had erased it --
       found index 0 against a non-zero primary, and discarded the HUD it was
       about to draw. The symptom was a HUD that measured 128/32 on screen
       with `s=4.500` in the heartbeat. */
    "  vec2 sd = d; float ramp = 1.0;\n"
    "  if (uHud.w > 1.0) {\n"
    "    float H = float(uSize.y);\n"
    "    if (d.x < uHud.x || d.y < uHud.y) { sd = d * uHud.z; ramp = uHud.w; }\n"
    "    else if (d.y >= H - uHud.y) {\n"
    "      sd = vec2(d.x * uHud.z, H - (H - d.y) * uHud.z); ramp = uHud.w; }\n"
    /* THE WORLD IS TRANSLATED, NOT MAGNIFIED (22.6). The engine's viewport is
       now the visible window itself -- it draws the world into [128, R] x
       [32, B] of its own surface -- and this puts that block where the player
       sees it. uHud.xy carry the panel width and bar height on screen, so the
       vector is (uHud.x - 128, uHud.y - 32) and the sampling subtracts it. */
    "    else sd = d - vec2(uHud.x - 128.0, uHud.y - 32.0);\n"
    "  }\n"
    "  ivec2 p = clamp(ivec2(sd), ivec2(0), uSize - 1);\n"
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
    /* THE CURSOR'S RECT WAS DISCARDED HERE AND IS NOT ANY MORE, and the
       difference is a hole rather than a nicety. In phase 1 the cursor was the
       engine's: blitted onto the primary after everything we observe, so it
       existed only in the engine's frame, and this shader left its rect unpainted
       so that frame could show through from underneath. There is no underneath
       now -- discarding here would punch the twin's own background out and leave
       the lane's clear colour in a cursor-shaped rect. So the twin always draws,
       and the cursor is the sharp layer's above it (G17c), which is the only
       cursor there is. */
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
    /* THE HUD'S REGION MAP (gui-renderer.md 22). Three regions, and they are
       the ones the engine RESERVED — tagpu_hud wrote the viewport rect from
       the same two integers this uniform carries, so the space the world was
       kept out of and the space the HUD art is blown up into cannot disagree.
       The panel owns its full column height (its art is a 128x480 block that
       does not stretch, resolution.md 3.4a) and magnifies about its top-left;
       the top bar about its top-left; the bottom bar about its BOTTOM-left,
       which is where the engine anchors it (screenH - 0x20). The world region
       is the identity and has no coverage in the twin anyway — the viewport's
       key fill erased it, and that fill covers exactly this rect.
       `ramp` widens the sharp-bilinear ramp with the magnification: it is
       "one DEVICE pixel", and one device pixel is s source texels fewer here. */
    "  vec2 tc = sd - 0.5;\n"
    "  vec2 b  = floor(tc);\n"
    "  vec2 w  = clamp((tc - b - 0.5) * (uScale * ramp) + 0.5, 0.0, 1.0);\n"
    "  ivec2 ib = ivec2(b);\n"
    "  vec4 c = mix(mix(tap(ib),                tap(ib + ivec2(1, 0)), w.x),\n"
    "               mix(tap(ib + ivec2(0, 1)), tap(ib + ivec2(1, 1)), w.x), w.y);\n"
    /* THE STALE-MIRROR GUARD. The twin holds what the PUBLISHER saw; uSurf holds
       what the engine actually has on the primary. The cursor rect above is one
       instance of a general rule - the engine paints by paths we never observe -
       and the intro Smacker is another: it writes the primary directly, so no op
       ever reaches the queue, the twin keeps the black it was seeded with at
       coverage 255, and the layer paints that stale black over a playing movie.

       The test is deliberately NARROW: only where the mirror says index 0 and
       the engine says otherwise. Index 0 does NOT mean "never published" --
       twin_upload stamps (index, 255) from the engine's own bytes, so it also
       means "black when we last saw it", and the two are indistinguishable.
       The guard does not need to tell them apart: the publisher only ever
       OBSERVES, so the twin can lag the engine's surface but never lead it, and
       where the two disagree the engine's is the newer. A wider test (any index
       mismatch) also unblanks the movie, but
       the twin's index and its restored colour are separate channels, so it
       discards restored texels whose index legitimately differs and drops that
       art back to the engine's dithered original. Measured on the tab row: the
       wide rule visibly de-restores it, this one leaves the panel bit-identical
       to an unguarded build at matched interaction history. */
    /* `p` and not `ib`: p = floor(tc + 0.5) is the NEAREST twin texel to this
       fragment, which is the corner the bilinear blend above weights most (>= 0.5
       per axis), and 13.3's sharpening ramp drives that weight toward 1 as k grows.
       So the guard tests the texel `c` is made of. The uStrict branch below reads
       the engine's surface at `p` for the same reason. */
    /* THE STALE-MIRROR GUARD AND THE `strict` HARNESS BOTH STOOD HERE.

       The guard discarded where the twin read index 0 and the engine's primary
       did not -- "the engine has painted by a path we never observed, let its
       own pixels through". Two things retire it. It could only ever let the
       ENGINE's frame through, and that frame is not underneath us. And its
       premise was the SEED: a twin was adopted whole from the engine's bytes
       and could therefore hold a stale copy of a region nobody republishes --
       the intro Smacker writing the primary directly was the case it was
       written for. A twin that is never seeded starts EMPTY, coverage 0, and
       an unobserved region is a region we simply do not paint. The hole the
       guard patched is closed at the source instead of being tested for.

       `strict` painted magenta where the engine's surface held a UI pixel and
       the twin held nothing -- the `uiwalk` oracle. It diffed us against the
       engine's composed frame, which is the comparison the cut removed: with
       nothing under us a miss is VISIBLE, which is the whole point of cutting
       rather than levering. The golden source is still captured and still
       reachable (`tagpu_vk_surf_engine_view`) for a comparison that wants one;
       it is not this shader's business. `uKey` went with `strict`, its only
       reader. */
    "  if (c.a > 0.5) { frag = vec4(c.rgb / c.a, 1.0); return; }\n"
    "  discard; }\n";
#pragma GCC diagnostic pop

/* THE UI ATLAS IS NOT A GL OBJECT, and `init_gl` arming it at its tail is the
   last place in this module where something the Vulkan twin needs was reachable
   only through a GL bring-up. It is the CPU table every sprite op resolves
   against -- with no atlas every sprite is `lost` and the surface re-seeds, and
   the first vulkan-only run of landing 4b-3 measured exactly that: sprites=0,
   atlas=0/0, lost=816878. `tagpu_gaf_atlas_create` has keyed on `made` rather
   than on a GL name since 4b-2, so it is safe to ask for on either lane.
   Idempotent: the `made` test comes before the memset that would clear it, and
   the memset's other casualty -- the one heap buffer `free_buffers` owns -- is
   handed back rather than dropped. It was two until 11-5e-2b part 2 took
   `mirrorRgb`; see that function's own comment for the one it does NOT own.
   [The vulkan-only plan, landing 4b-3.] */
static int atlas_setup(void)
{
    if (s_atlas.made) return 1;
    /* the memset drops `mirror`, which `_lost` keeps across a context loss --
       so hand it back first or each loss leaks it. (It was `mirror` AND
       `mirrorRgb` until 11-5e-2b part 2. `rlist` was a third allocation this
       call did NOT free until 11-5e-2c, which made it free that too -- so this
       hands back BOTH heap buffers now and clears `rlistWant`/`rlistFailed`
       with them. The fact that still matters here is the other one: the GUI
       atlas never arms a list, so `rlist` is NULL on this path either way, and
       this is the only call site -- `tagpu_gaf_atlas_restore_vk` has three
       callers and none is in this file. `tagpu_gaf_atlas_free_buffers` and
       `rlist_add` carry the argument and name the residual.)
       [FROM THE 4b-3 LANDING REVIEW; the rlist half corrected by 11-5e-2c's] */
    tagpu_gaf_atlas_free_buffers(&s_atlas);
    memset(&s_atlas, 0, sizeof s_atlas);
    s_atlas.ents = s_ents; s_atlas.max = ATLAS_MAX; s_atlas.dim = ATLAS_DIM; s_atlas.tag = "gui";
    s_atlas.pad = 0; s_atlas.align = 0; s_atlas.mip = 0;      /* 1:1, NEAREST, the 1-texel border */
    s_atlas.prio = UI_RESTORE_PRIO;
    s_atlas.restoreMinEdge = UI_RESTORE_MIN;
    return tagpu_gaf_atlas_create(&s_atlas) ? 1 : 0;
}

/* WHETHER THIS FRAME CARRIES TA's OWN SURFACE, which is what the mirror's
   `eng` copy, the strict guard and the layer's guard all really ask. On the GL
   lane the fork has uploaded it and `f->surface_tex` names that upload; on a
   lane with no GL there is no texture to name, but the BYTES are where they
   always were -- `g_ddraw.primary->surface`, which `mir_finish` copies out
   under g_ddraw.cs and validates against the primary's own geometry. Keying on
   the texture left the Vulkan twin with no engine frame and it composited
   nothing at all on the vulkan-only lane, saying so every time: "the hand-over
   carries no copy of the engine's own frame". [Vulkan-only plan, 4b-3.] */
static int have_engine_frame(const TAGPU_FRAME* f)
{
    /* The `|| tagpu_vk_owns_present()` term was the whole point of 4b-3's fix and
       it is now the whole function: this file is reached only through
       `tagpu_overlay_draw`, which runs only from `render_vk.c` after the present
       latch is set [landing 11-4b]. `f->surface_tex` is kept as the first term
       because it is the honest question -- a caller that DOES carry an engine
       frame answers yes for the original reason -- but nothing can make this
       return 0 today. */
    (void)f;
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
    /* the three `glDelete*` calls STOOD HERE and freed this twin's texture, its
       FBO and its colour attachment [landing 11-4b]. All three ids are 0 on this
       lane -- nothing creates them since the draw half went -- so the guards
       never fired; they are gone rather than left as GL calls one restored
       `if` away from running without a context. The RECORD below is the whole
       function now: the twin's slot is recycled so `twin_find` stops resolving
       this surface, which is what the mirror's FREE op pairs with. */
    *t = s_twins[--s_ntwins];
}

/* The colour twin, made by the first op that has colour to put in it: RGBA8
   at COLOR_ATTACHMENT1 of the same FBO, so one MRT draw writes the index and
   the colour together and they can never disagree about what a texel holds.
   Cleared to alpha 0 — nothing is restored until an op says so. */
/* Indices arrived for the box and they say nothing about colour: drop the
   colour there so the layer falls back to the palette. */
static TWIN* twin_make(unsigned surf, int w, int h)
{
    TWIN* t = twin_find(surf);
    if (t && (t->w != w || t->h != h)) { twin_drop(t); t = NULL; }
    if (t) return t;
    if (s_ntwins >= MAX_TWINS) twin_drop(&s_twins[0]);        /* the oldest goes */
    t = &s_twins[s_ntwins++];
    memset(t, 0, sizeof *t);
    t->surf = surf; t->w = w; t->h = h;
    /* THE TEXTURE AND ITS FBO ARE GL; THE ENTRY IS THE TABLE. A twin
       exists when the table says so, and the Vulkan twin keeps images of
       its own -- so on a lane with no GL the entry is made, `tex`/`fbo`
       stay 0, and every `if (t)` that gates a `mir_op` still passes. This
       is the shape landing 4b-2 found twelve times, in the module that
       has the most of it.
       [The vulkan-only plan, landing 4b-3.] */
    return t;
}

/* rows of indices -> (index, 255) pairs -> the twin's box */
/* G19f landing 4: the return is this op's TAGPU_GUIOP::col -- what this
   function decided about colour, for the mirror to carry rather than decide
   again. `twin_colour` can refuse (no MRT entry points, an incomplete FBO), so
   "restored" and "the destination has a colour twin" are two different facts
   and both are recorded. */
static unsigned char twin_sprite(TWIN* t, const TAGPU_GAFENT* e, const TAGPU_PUBOP* o)
{
    /* ALWAYS 0, AND THAT IS 4b-3's ANSWER MADE EXPLICIT [landing 11-4b]. The two
       bits were `t->rgb ? DST : 0` and `(restored && t->rgb) ? ON : 0`, over
       `restored = s_colValid && s_atlas.rgb != 0`. Every term is a GL object name
       or a latch only the GL bring-up could raise, and all of them went with the
       draw -- so the expression could no longer evaluate to anything but 0, while
       still reading like a decision. 4b-3 already said what this lane records:
       "the ops this lane records say indexed, which is what its twin will draw."
       Computing that from three flags nothing can set would be the `s_state` trap
       of landing 11-4a at record scale -- the value is right, and the next reader
       would believe the inputs still moved. */
    (void)t; (void)e; (void)o;
    s_sprites++;
    return 0;
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
/* G19f: the Vulkan mirror's record of a string, defined with the rest of the
   mirror below -- this is drawn above it, and records what it resolved. */
static void mir_string(const TAGPU_PUBOP* o, const short cell[][4], int n,
                       int x0, int top, unsigned char col);
static void mir_atlas_seen(void);       /* with the mirror below              */

static void twin_string(TWIN* t, const TAGPU_PUBOP* o)
{
    /* THE BLOCK IS GLYPH RECORDS THEN THE STRING (landing 4c). `feed` installs
       the glyph records the producer sent with this op — first sight of a
       (font, code) pair — are installed by the caller, before this runs;
       nothing below has a font address to dereference, and `o->frame` is NULL
       for a string now. */
    const char* str;
    unsigned goff;
    /* the glyph records at the head of the block were installed by the drain
       loop, before this was called and whether or not it was called at all */
    goff = tagpu_text_glyph_block_bytes(g_guiq.arena + o->aoff, o->gcount, o->alen);
    str = (const char*)(g_guiq.arena + o->aoff + goff);
    short cell[256][4];                 /* ax, ay, w, h per drawn glyph        */
    int n = 0, i, x, top, yoff = 0, aw = 0, ah = 0;
    int attempt, miss = 0;
    unsigned gen0;

    /* THE QUESTION IS WHETHER THERE IS A STRING TO STAMP, and on the GL lane
       also whether the program that stamps it came up. A GL program name is 0
       on a lane that creates none, and re-seeding the whole surface for every
       string because of that is the shape landing 4b-2 found twelve times.
       [The vulkan-only plan, landing 4b-3.] */
    if (!o->alen) goto reseed;
    /* PASS ONE: rasterise every glyph this string needs, so the atlas texture
       is uploaded ONCE for the string rather than once per new glyph.

       AND THE ATLAS CAN REPACK UNDER US WHILE WE DO IT [landing review,
       2026-09-09]: a glyph that runs the shelves out clears every cell and
       starts over, so the ax/ay already in `cell` would name texels that were
       just wiped and re-used, and the string's leading characters would sample
       0 -- invisible where `bg == tr`, solid boxes where it is not. The
       generation says whether that happened. One retry is enough by
       construction, the atlas being empty at that point and one string's
       distinct glyphs fitting in it; a second move means something is wrong
       with the font, and the box's own bytes are the honest fallback. */
    for (attempt = 0; attempt < 2; attempt++) {
        gen0 = tagpu_text_glyph_gen();
        n = 0; miss = 0;
        for (i = 0; i < 256 && str[i] && str[i] != '\n'; i++) {
            int ax, ay, gw, gh;
            if (!tagpu_text_glyph_id(o->font_id, (unsigned char)str[i], &ax, &ay, &gw, &gh, &yoff)) {
                /* the engine skips a code below `first` and a zero table entry
                   and advances for neither, so a refusal here is only a
                   divergence when the cache refused something the engine would
                   have drawn */
                miss++;
                continue;
            }
            if (n < 256) {
                cell[n][0] = (short)ax; cell[n][1] = (short)ay;
                cell[n][2] = (short)gw; cell[n][3] = (short)gh; n++;
            }
        }
        if (tagpu_text_glyph_gen() == gen0) break;
        s_strRepack++;
    }
    if (tagpu_text_glyph_gen() != gen0) goto reseed;
    s_strMiss += (unsigned)miss;
    if (!n) goto reseed;
    /* THE QUESTION IS WHETHER THE GLYPH CACHE HAS RASTERISED ANYTHING, and
       `tagpu_text_glyph_have` is how it is asked. It was written in 4b-3 as
       the context-free twin of `tagpu_text_glyph_tex`'s own first line, back
       when the answer arrived wrapped in a GL texture name and asking for the
       name on a lane that makes none would have sent every string down the
       re-seed path. That function is deleted (11-5e-1) and the plain question
       is all there is now.
       [The vulkan-only plan, landing 4b-3.] */
    if (!tagpu_text_glyph_have()) goto reseed;
    tagpu_text_glyph_dims(&aw, &ah);
    if (aw <= 0 || ah <= 0) goto reseed;

    /* the blitter's destination is base + (y - (s8)font[2]) * pitch + x, so the
       string's first pixel row is at y - yoff and NOT at the y it was given */
    x = (int)o->sl;
    top = (int)o->st - yoff;
    for (i = 0; i < n; i++) {
        int gw = cell[i][2];
        /* the per-glyph `quad(v, ...)` STOOD HERE and filled a vertex buffer for
           the GL upload two lines below it; both went together [landing 11-4b].
           The loop still walks the glyphs, because `x` is the running pen
           position `mir_string` publishes as the string's placement and
           `s_glyphs` is the counter the log reports. */
        x += gw;
        s_glyphs++;
    }
    s_strings++;
    /* 0 for the reason `twin_sprite` gives: `t->rgb` is a GL texture name and
       nothing creates one here any more [landing 11-4b]. */
    mir_string(o, cell, n, (int)o->sl, top, 0);
    return;

reseed:
    /* WE PUBLISHED A STRING AND DREW NOTHING, so the twin is missing text the
       engine's surface has. A box of pixels would have been drawn whatever
       happened; a string can fail on a font we cannot read, so the fallback is
       to re-seed the surface from the engine's own — expensive, and it should
       never happen. Counted and logged rather than silent. */
    if (s_strReseed < 8) {
        char b[180];
        _snprintf(b, sizeof b, "gui: string op stamped nothing (font id %u, %u bytes, surface %08X) — re-seeding",
                  o->font_id, o->alen, o->surf);
        b[sizeof b - 1] = '\0';
        slog(b);
    }
    s_strReseed++;
    g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_STRING;
}

static unsigned char twin_copy(TWIN* t, const TWIN* src, const TAGPU_PUBOP* o)
{
    /* THE COPY IS WHY COLOUR IS PER SURFACE (gui-renderer.md 13.2): the panel
       is painted into panel+0xBC and only later blitted to the frame, so
       restored art reaches the screen through here or not at all. A source
       with no colour twin writes zero, which invalidates the destination's
       colour over the box — a copy from indexed art means indexed art. */
    /* 0, for `twin_sprite`'s reason: both terms were GL texture names and the
       twin performs the same copy from the same op. [landing 11-4b] */
    s_copies++;
    return 0;
}

/* THE BOX, ONE PALETTE INDEX, FULLY COVERED. `twin_clear`'s shape with two
   differences: the index goes in the RED channel (the twin is RG -- index and
   coverage, see `twin_upload`) and GREEN is 1, because this op COVERS what it
   fills where a clear uncovers it. [The vulkan-only plan, landing 8a.] */
/* FOUR INCLUSIVE EDGES, ONE PALETTE INDEX, INTERIOR UNTOUCHED. `twin_fill`'s
   shape four times over, one scissor per edge, because that is exactly what
   `0x4BF8C0` does -- and doing it as one fill plus a smaller clear would be
   wrong, not merely slower: the interior is whatever was already in the twin
   and the op never wrote it. [The vulkan-only plan, landing 8b.] */
static void twins_reset(void)
{
    while (s_ntwins) twin_drop(&s_twins[0]);
    s_presented = 0;
    tagpu_gaf_atlas_reset(&s_atlas);
}

/* ---------------------------------------------------------- Classic++ arm */
/* Once per present, BEFORE the drain (the sprite ops it replays ask whether
   colour is valid) and after upload_palette (this compares against it).

   THE VALIDITY RULE, gui-renderer.md 3.4. A restore job snapshots the palette
   into a texture of its own, so the restored twin's colours are a function of
   the palette that was live when the job was made. (Until 11-5e-2 this named
   `tagpu_rglsl_job_new`, the GL backend's constructor; the rule is the
   restorer's rather than that backend's, and `tagpu_vk_restore_job_new`
   snapshots the same way.) While that is
   still the palette the frame is PRESENTED with, colour is used; while it is
   not, every colour twin is ignored and the frame is indexed — dithered art
   for the duration of a fade, never wrong art. Once the palette has held
   still for PAL_SETTLE frames the job is rebuilt against the new one and
   every colour twin is invalidated, so the art comes back restored as the
   engine redraws it. */
/* ------------------------------------------------------------------ drain */
/* ==================================================================== G19f ==
   THE VULKAN LANE'S MIRROR of this present's op stream. tagpu_gui.h carries
   the contract and the reason it is a COPY rather than a pointer into the
   queue; here is the machinery.

   RECORDED WHERE EACH OP IS APPLIED, not where it is read. The hand-over says
   what the render half DID -- a sprite whose atlas entry could not be resolved
   drew nothing here and must draw nothing there, and recording it at the top of
   the switch would publish an op the GL lane skipped. So every `mir_*` call
   below sits beside the GL call it mirrors.

   A FRAME IS ALL OR NOTHING. If the arena or the op array will not grow, the
   frame's record is abandoned and nothing is published: a truncated op stream
   applied to a twin store is a DIFFERENT PICTURE, not a smaller one, and the
   A/B would report it as a rasteriser difference. `s_mirLost` counts it. */

static int       s_mirWant = 0;        /* the Vulkan pass asked for one       */
static int       s_mirRec  = 0;        /* ...and this frame is being recorded */
static unsigned  s_mirLost = 0;        /* frames abandoned for want of room   */
static int       s_mOther = 0;         /* ops this landing does not carry     */
/* G19f: THE A/B LEVER. `tagpu_gui.ab` latches a claim for one frame; the seam
   captures the Vulkan image on that frame and `tagpu_vk_ab_arm` has already
   unlinked the target, so the file on the disk is this arming's. Diff it against
   a capture from another BUILD. (Until landing 4d-2 this lever also made the
   pass draw its composite over a black frame and read the viewport back, so the
   two lanes could be diffed against each other; route D went in 4d-1 and that
   half went with it.) */
#define AB_FILE  "tagpu_gui.ab"
static int       s_ab, s_abDone, s_abFrame;
static int       s_mLayer = 0;         /* draw_layer actually composited      */

static TAGPU_GUIOP* s_mOps;
static unsigned  s_mNOps, s_mCapOps;
static unsigned char* s_mArena;
static unsigned  s_mALen, s_mACap;

static TAGPU_GUIHAND s_mHand;
static int       s_mHave = 0;          /* a hand-over stands                  */
static int       s_mNSDraw;            /* the sharp layer's quads this frame   */
static TAGPU_GUISDRAW s_mSDraw[TAGPU_GUI_SDRAW_MAX];
static int       s_mStrAny;            /* a string op was recorded this frame  */
static unsigned  s_mStrGen;            /* ...at this glyph-atlas generation     */
/* THE SAME QUESTION ABOUT THE UI ATLAS, and it has been open since landing 1.
   A sprite's record carries the atlas rect this lane RESOLVED, and the cursor
   quad carries one too -- both valid only for the generation they were read in.
   `tagpu_gaf_atlas_put` runs INSIDE the drain, so a sprite that fills the atlas
   recycles it (`atlas_drop`, `a->gen++`) in the middle of the very present
   whose earlier sprites are already recorded, and `sharp_cursor` can do it
   again afterwards. Landing 2 built exactly this guard for the glyph atlas and
   the sprite atlas was left without one.
   `s_mAtAny` is CLEARED BY A RECORDED RESET rather than only by `mir_begin`,
   and that is not tidiness: `twins_reset` recycles the atlas, so EVERY reset
   moves the generation -- a guard that did not forget the ops before one would
   lose every reseed present and answer the reseed with another.
   [FOUND 2026-09-16, the landing-4 review.] */
static int       s_mAtAny;             /* a resolved atlas rect is in the record */
static unsigned  s_mAtGen;             /* ...at this UI-atlas generation         */
static int       s_mTaken = 0;         /* ...and has been taken               */
static unsigned  s_mFrame = 0;

#define MIR_OPS_MAX   (1u << 16)       /* the queue's own ceiling             */
#define MIR_ARENA_MAX (24u << 20)      /* the queue's 16 MB, plus the slack a
                                          seed burst adds when every surface
                                          reseeds in one present              */

static void mir_finish(const TAGPU_FRAME* f);   /* defined below draw_layer,
   which fills the composite half of the record it publishes */

static void mir_begin(void)
{
    s_mNOps = 0; s_mALen = 0; s_mOther = 0; s_mLayer = 0;
    s_mStrAny = 0; s_mStrGen = 0;
    s_mAtAny = 0; s_mAtGen = 0;
    s_mNSDraw = 0;
    s_abFrame = 0;          /* the claim never outlives the frame that made it */
    s_mirRec = s_mirWant;
    s_mHave = 0;
}

/* room for one more op, or abandon the frame */
static TAGPU_GUIOP* mir_op(void)
{
    if (!s_mirRec) return NULL;
    if (s_mNOps >= s_mCapOps) {
        unsigned want = s_mCapOps ? s_mCapOps * 2 : 1024;
        TAGPU_GUIOP* n;
        if (want > MIR_OPS_MAX) { s_mirRec = 0; s_mirLost++; return NULL; }
        n = (TAGPU_GUIOP*)realloc(s_mOps, want * sizeof *n);
        if (!n) { s_mirRec = 0; s_mirLost++; return NULL; }
        s_mOps = n; s_mCapOps = want;
    }
    memset(&s_mOps[s_mNOps], 0, sizeof s_mOps[0]);
    return &s_mOps[s_mNOps++];
}

/* A RESOLVED UI-ATLAS RECT HAS GONE INTO THE RECORD at the generation it is
   valid for. `mir_finish` compares and loses the frame if the atlas moved
   under it before the present ended. */
static void mir_atlas_seen(void)
{
    if (!s_mirRec || s_mAtAny) return;
    s_mAtAny = 1; s_mAtGen = s_atlas.gen;
}

/* copy `n` bytes into our arena and answer the offset, or abandon the frame */
static int mir_bytes(const void* src, unsigned n, unsigned* off)
{
    if (!s_mirRec) return 0;
    if (!n) { *off = 0; return 1; }
    /* EVERY PAYLOAD STARTS 4-BYTE ALIGNED. A string's cells are read on the
       other side as `short`, and the blocks before it are `w * h` bytes of
       pixels, so an unaligned `aoff` is reachable -- defined nowhere in C and
       tolerated by x86 only until something vectorises the read. Padding is the
       whole fix and it costs at most three bytes an op. */
    s_mALen = (s_mALen + 3u) & ~3u;
    if (s_mALen > s_mACap) { s_mirRec = 0; s_mirLost++; return 0; }
    if (s_mALen + n > s_mACap) {
        unsigned want = s_mACap ? s_mACap * 2 : (1u << 20);
        unsigned char* nb;
        while (want < s_mALen + n) want *= 2;
        if (want > MIR_ARENA_MAX) { s_mirRec = 0; s_mirLost++; return 0; }
        nb = (unsigned char*)realloc(s_mArena, want);
        if (!nb) { s_mirRec = 0; s_mirLost++; return 0; }
        s_mArena = nb; s_mACap = want;
    }
    memcpy(s_mArena + s_mALen, src, n);
    *off = s_mALen;
    s_mALen += n;
    return 1;
}

/* A STRING, RECORDED WHERE IT IS DRAWN AND WITH WHAT IT RESOLVED. Called from
   the tail of `twin_string`, after the draw succeeded -- a string that stamped
   nothing re-seeds instead, and must not be published as though it had. */
static void mir_string(const TAGPU_PUBOP* o, const short cell[][4], int n,
                       int x0, int top, unsigned char col)
{
    TAGPU_GUIOP* m;
    if (!s_mirRec || n < 1) return;
    m = mir_op();
    if (!m) return;
    m->kind = TAGPU_GUIOP_STRING;

    m->surf = o->surf;
    /* ONLY `TAGPU_GUICOL_DST` IS EVER SET HERE, and that matters: `STR_FS`
       writes `oCol = vec4(0.0)` and has no restored branch at all, so a string
       on a colour twin ERASES colour under the glyphs it stamps and leaves it
       standing between them (which is the whole point of stamping glyphs
       rather than publishing the box). What the consumer needs is therefore
       only whether that second attachment exists. */
    m->col = col;
    m->fg = o->fg; m->bg = o->bg; m->tr = o->tr;
    m->sl = (short)x0; m->st = (short)top;
    m->nglyph = (unsigned short)n;
    m->alen = (unsigned)n * 8;           /* four shorts a glyph */
    /* THE GENERATION THESE CELLS WERE RESOLVED AT. `twin_string` retries around
       a repack of ITS OWN string and is correct for itself -- it drew into the
       GL twin from the texture as it stood. This record does not draw until the
       drain is over, against the atlas as it stands THEN, so a repack caused by
       a LATER string in the same present leaves these cells naming cleared
       texels. `mir_finish` compares and loses the frame.
       [FOUND 2026-09-16, the landing-2 review.] */
    if (!s_mStrAny) { s_mStrAny = 1; s_mStrGen = tagpu_text_glyph_gen(); }
    if (!mir_bytes(cell, m->alen, &m->aoff)) s_mNOps--;
}

/* A SHARP-LAYER QUAD, RECORDED WHERE IT IS DRAWN AND WITH WHAT IT RESOLVED.
   The layer's three clients draw in a fixed order and this preserves it, which
   matters: the minimap is drawn AFTER the cursor and so covers it where they
   overlap, and its view box after its base for the same reason the engine draws
   them that way (0x466B44 then 0x466B5E).

   THE LIST OVERFLOWING IS NOT A `behind`. Unlike an op, a sharp-layer quad
   mutates no state that persists into the next frame -- the layer is cleared to
   (0,0,0,0) at every present -- so a frame that produced more quads than this
   carries is one frame that cannot be composited, not a store that has fallen
   out of step. It says so by counting, and the consumer refuses the frame. */
static void mir_sdraw(int kind, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1,
                      const float col[4], int ck)
{
    TAGPU_GUISDRAW* q;
    if (!s_mirRec) return;
    if (s_mNSDraw >= TAGPU_GUI_SDRAW_MAX) { s_mNSDraw = TAGPU_GUI_SDRAW_MAX + 1; return; }
    q = &s_mSDraw[s_mNSDraw++];
    q->kind = kind;
    q->dst[0] = x0; q->dst[1] = y0; q->dst[2] = x1; q->dst[3] = y1;
    q->uv[0] = u0; q->uv[1] = v0; q->uv[2] = u1; q->uv[3] = v1;
    if (col) { q->col[0] = col[0]; q->col[1] = col[1];
               q->col[2] = col[2]; q->col[3] = col[3]; }
    else     { q->col[0] = q->col[1] = q->col[2] = q->col[3] = 0.0f; }
    q->ck = ck;
}

/* the box ops all carry the same rectangle */
static void mir_box(TAGPU_GUIOP* m, const TAGPU_PUBOP* o)
{
    m->surf = o->surf; m->src = o->src;
    m->l = o->l; m->t = o->t; m->r = o->r; m->b = o->b;
    m->sl = o->sl; m->st = o->st;
}

static void drain(void)
{
    unsigned tail = g_guiq.qTail, head = g_guiq.qHead;
    int budget = 20000;                    /* ops per present: a burst is many flips */
    while (tail != head && budget-- > 0) {
        const TAGPU_PUBOP* o = &g_guiq.ops[tail & (TAGPU_GUI_QCAP - 1)];
        TWIN* t;
        unsigned char col = 0;          /* G19f landing 4: TAGPU_GUIOP::col   */
        /* THE GLYPH BLOCK GOES IN BEFORE THE SKIP, NOT INSIDE THE SWITCH.
           `s_skipToReset` jumps the whole switch for every op queued before a
           GL context change (measured at 705 of them), so installing the block
           inside it would let those ops take their first-sight glyph records
           with them — and the producer has already marked the pairs sent. The
           records are the op's own payload and installing them is independent
           of everything the switch decides [found by the review of the
           review's fixes]. The producer's reseed handling clears `sent[]` as
           well, which is the second line. */
        if (o->kind == PK_STRING && o->alen)
            tagpu_text_glyph_feed(o->font_id, o->font_rows, o->font_yoff,
                                  g_guiq.arena + o->aoff, o->gcount, o->alen);
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
        case PK_RESET:  twins_reset(); s_skipToReset = 0;
            { TAGPU_GUIOP* m = mir_op(); if (m) m->kind = TAGPU_GUIOP_RESET; }
            /* AND EVERY RESOLVED ATLAS RECT BEFORE THIS POINT STOPS MATTERING.
               The consumer drops its whole store on this op, so a sprite
               recorded ahead of it drew into a twin that no longer exists --
               and `twins_reset` has just recycled the atlas, so without this
               line the generation check below would lose every present that
               carries a reset, which is every present that answers a reseed. */
            s_mAtAny = 0; s_mAtGen = 0;
            break;
        case PK_SEED:
            /* THE TWIN IS MADE EMPTY, AND THE ENGINE'S BYTES ARE NOT CARRIED.
               `PK_SEED` publishes a surface's whole contents as the engine has
               them -- the one op in the stream whose payload is not a
               description of a draw but a COPY of what the 1997 rasteriser
               composed. Passing it on would put those bytes in a twin and the
               twin on the screen, which is precisely the engine pixel the cut
               removed. So the geometry crosses and the payload does not:
               `m->alen = 0`, and the consumer's own SEED case already answers
               that exactly right -- `tw_make` then `tw_fresh`, which clears to
               coverage 0, then `if (!o->alen) break`.

               WHAT THAT COSTS, STATED: a surface we have not watched being
               drawn is EMPTY rather than adopted, so it shows nothing until the
               engine redraws it through a leaf we observe. That is the hole the
               forced repaint exists to fill -- `GUI_StageUpdateDraw(gi, 0x40)`
               in tagpu_gui_hook.c, landing 9, which asks the engine to re-emit
               the top screen as ops. It is also why the twin can be empty and
               must never be stale: the stale-mirror guard `LAY_FS` used to
               carry was patching exactly this, by reading the engine's frame,
               and the hole is closed at the source now instead. */
            t = twin_make(o->surf, o->w, o->h);
            if (t) {
                TAGPU_GUIOP* m = mir_op();
                if (m) {
                    m->kind = TAGPU_GUIOP_SEED; mir_box(m, o);
                    m->w = o->w; m->h = o->h; m->alen = 0;
                }
            }
            s_seeds++;
            break;
        case PK_FREE:
            t = twin_find(o->surf);
            if (t) { TAGPU_GUIOP* m; twin_drop(t);
                     m = mir_op(); if (m) { m->kind = TAGPU_GUIOP_FREE; m->surf = o->surf; } }
            break;
        case PK_CLEAR:
            t = twin_find(o->surf);
            if (t) { TAGPU_GUIOP* m;
                     m = mir_op(); if (m) { m->kind = TAGPU_GUIOP_CLEAR; mir_box(m, o); }
                     /* COUNTED HERE, beside the mirror op, as `s_bars`/`s_rects`/
                        `s_pixels`/`s_seeds` already are. It used to be counted inside
                        `twin_clear`, below that function's lane gate -- so when the
                        draw half went [landing 11-4b] the heartbeat kept printing
                        `clears=0` while CLEAR ops were being mirrored every frame.
                        An instrument that is pinned at 0 beside live traffic sends
                        the next session to the wrong function. */
                     s_clears++; }
            break;
        case PK_PIXELS:
            /* DROPPED, AND COUNTED SO THE HOLE IS A NUMBER. `PK_PIXELS` is the
               publisher's fallback: an op whose kind it could not describe
               semantically, sent as a box of the engine's own composed bytes
               read off the surface AT THE FLIP. Two things are wrong with it
               and only one is the cut's. It is an engine pixel, so it cannot
               reach the screen. And it is LATER than the draw it stands for --
               whatever was painted over that box between the blit and the flip
               is what the bytes hold -- so it was never a faithful record even
               when it was allowed.

               The fix is at the PRODUCER, not here: every kind that falls
               through to `as_pixels` in tagpu_gui_hook.c needs a semantic op of
               its own, exactly as `PK_BAR` and `PK_RECT` were given one by
               landings 8a and 8b. `s_pixDropped` is the work list's size --
               measured by area rather than count on the `gui area:` census
               line, which is what says which kind to do first. Until then those
               regions simply do not draw, visibly, which is the whole reason
               for cutting rather than levering. */
            t = twin_find(o->surf);
            if (t && o->alen) s_pixDropped++;
            break;
        case PK_BAR:
            /* A SOLID RECTANGLE, filled with one palette index and fully
               covered. No arena bytes to copy and none to run out of, which is
               the whole reason it is not `PK_PIXELS` any more.
               TWO ENGINE LEAVES REACH THIS, and since landing 8c most of the
               traffic is the second: `DrawBar 0x4BF6F0` (writer `0x4CCDEA`) and
               an AXIS-ALIGNED `DrawLine 0x4BE950` (writer `0x4CC7AB`), whose
               bounding box is the line, one pixel thick. Both writers take the
               low byte of their colour and nothing wider, which is why one
               packet describes both. [8a, then 8c.] */
            t = twin_find(o->surf);
            if (t) {
                TAGPU_GUIOP* m;
                m = mir_op();
                if (m) { m->kind = TAGPU_GUIOP_BAR; mir_box(m, o); m->fg = o->fg; }
                s_bars++;
            }
            break;
        case PK_RECT:
            /* FOUR EDGES, no arena bytes. `0x4BF8C0` only; `0x4BF7B0` tints and
               is `OP_FOCUS`, which still publishes pixels.
               [The vulkan-only plan, landing 8b.] */
            t = twin_find(o->surf);
            if (t) {
                TAGPU_GUIOP* m;
                m = mir_op();
                if (m) { m->kind = TAGPU_GUIOP_RECT; mir_box(m, o); m->fg = o->fg; }
                s_rects++;
            }
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
            col = twin_sprite(t, e, o);
            { TAGPU_GUIOP* m = mir_op();
              if (m) {
                  m->kind = TAGPU_GUIOP_SPRITE; mir_box(m, o);
                  m->ck = o->ck; m->fw = o->fw; m->fh = o->fh; m->col = col;
                  /* THE RECT THE GL LANE JUST RESOLVED, carried rather than
                     looked up again on the other side: a second lookup could
                     answer differently after a repack and the A/B would be
                     comparing two atlases. */
                  m->u0 = e->u0; m->v0 = e->v0; m->u1 = e->u1; m->v1 = e->v1;
                  mir_atlas_seen();
              } }
            break; }
        case PK_STRING:
            t = twin_find(o->surf);
            /* the glyphs went in above, before the skip gate and whether or
               not this surface has a twin: the producer marks a (font, code)
               pair sent the moment it commits the op and never sends it again */
            /* twin_string records the mirror op itself, from what it
               resolved -- see mir_string */
            if (t) twin_string(t, o);
            break;
        case PK_COPY: {
            TWIN* src = twin_find(o->src);
            t = twin_find(o->surf);
            if (t && src) {
                TAGPU_GUIOP* m;
                col = twin_copy(t, src, o);
                m = mir_op(); if (m) { m->kind = TAGPU_GUIOP_COPY; mir_box(m, o); m->col = col; }
            }
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
/* THE PALETTE THE ENGINE'S FRAME IS SHOWN WITH is not main+0x143A7 —
   tagpu_pal.h has the engine facts and does the resolving, for the world's
   passes and this one alike. Here it is only uploaded, and only when it has
   actually moved: the serial says so. */
/* THE GL UI's RENDER HALF READS THE PACKET (landing 4c). It took the cursor's
   position and sprite out of the graphics globals, and the minimap's box, its
   surfaces and its view box out of the TAdynmem block, on THIS thread, every
   present — the four engine reads the plan had not scheduled. They are all
   packet fields now.

   THE POINTER IS NEVER KEPT ACROSS A CALL, let alone across a frame: it is
   handed down from the driver's own record every time. A static holding it
   would be exactly the cached packet pointer `tagpu_packet.poison` exists to
   catch, and the two entry points here (the cursor decision before the world
   pass, the layer after it) are separate calls with separate chances to be
   skipped. NULL is "no packet this frame" — a shell frame or a load — and
   every consumer below declines. */
static void cursor_rect(const TAGPU_PACKET* pk, float* r)
{
    /* THE FOUR OUTCOMES THE LIVE READ HAD, reproduced exactly: no rect at all
       (the globals unreadable — the publisher writes (-1,-1,0,0) itself); the
       position with a 64x64 fallback (globals readable, sprite record not);
       the position with the record's own size; and the position with a ZERO
       size, which a readable record of zero extent gives and which the first
       draft of this collapsed into "no rect".

       The gate is a flag, not the size: the level-end packet zeroes the whole
       header, and (0,0) is a real cursor position. It was `in_game` alone
       until landing 6 — which is exactly why the shell had no cursor: the
       shell's packet must be in_game=0 (every world pass reads that field to
       decide whether to draw at all), so the field could not distinguish "a
       shell frame that carries a cursor" from "a level-end packet that
       carries none". `cursor_live` is that distinction, and it is only ever
       set by the two publishers that read the cursor: the in-play fill and
       the shell's own channel. An in-play packet implies it. */
    r[0] = r[1] = -1.0f; r[2] = r[3] = 0.0f;
    if (!pk) return;
    if (!pk->in_game && !pk->cursor_live) return;
    r[0] = (float)pk->cur_pos[0];
    r[1] = (float)pk->cur_pos[1];
    r[2] = (float)pk->cur_w;
    r[3] = (float)pk->cur_h;
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
void tagpu_gui_cursor_frame(const TAGPU_PACKET* pk)
{
    const unsigned char* fr;
    const void* pix;
    s_curOwn = 0;
    s_curFrame = NULL;
    /* the engine's rect first and unconditionally: draw_layer's discard (and
       `strict`'s exemption) needs it on every path, including the ones below
       that decline to own the cursor */
    cursor_rect(pk, s_curEng);
    /* `s_atlas.made`, NOT `s_gl == 1`: the sentence two paragraphs up says
       ownership is latched on the ATLAS, and `s_gl` is the GL bring-up's latch
       -- a lane that brings no GL up left it at 0 and the cursor was never
       claimed at all (measured: curs=0,0x0,drawn=0 on the first vulkan-only
       run of landing 4b-3). On the GL lane the two are the same fact: the
       atlas is armed at the tail of `init_gl` and only on the path that sets
       `s_gl = 1`. [The vulkan-only plan, landing 4b-3.] */
    if (!s_on || s_nocursor || !s_atlas.made) return;
    if (!pk || !pk->cur_rec) return;
    /* the sprite record IS a GAF frame header -- size, hotspot, colour key and
       a pixel pointer at +0x10 -- and it comes out of the cursor TABLE, loaded
       once and never rewritten, which is why it may cross as a key at all. The
       publisher read the position and the size beside it; tagpu_gaf.c is the
       only file that dereferences the frame (landing 4c). */
    fr = tagpu_gaf_frame_sane((const void*)(size_t)pk->cur_rec);
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
    float kx, ky;
    int cx = 0, cy = 0, dx, dy, x0, y0, w, h;
    /* THIS IS A CLIENT OF THE SHARP LAYER, NOT A PURE GL FUNCTION: it resolves
       the cursor's rect from a mouse position read on THIS thread at THIS
       instant and records it with `mir_sdraw` for the Vulkan twin, which is
       the only copy of that answer anyone gets. The quad is GL; the rect, the
       record and `s_curInLayer` are the lane's. [Vulkan-only plan, 4b-3.] */
    if (!s_curFrame || !s_sharpW || !s_sharpH) return;
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
        /* HUD SCALE (20): this is the ENGINE's point, and over a HUD region
           the pointer chain divided it by s on the way in. Put it back before
           it becomes device pixels, or our cursor sits at a fraction of where
           the player is pointing. The client-point branch above needs none of
           this -- it never left device space. */
        tagpu_hud_to_screen(&gx, &gy);
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
    /* THE CURSOR IS RESOLVED THROUGH THE PRESENTED PALETTE AND NEVER THROUGH
       THE RESTORED TWIN, whatever `s_colValid` says about the rest of the
       frame. [MEASURED 2026-09-13, from the owner's report: "the Move cursor,
       the reclaim cursor exhibit the same clear artifact as the mouse cursor
       did ... when we first use the move cursor for the first time ... for a
       few seconds".] The artifact is the sprite's own silhouette drawn in the
       terrain key's cyan, and the A/B that names its source is `norestore` in
       tagpu_gui.on (tagpu_gui_surf.c, restore_step): with the twin off the
       same trigger measures clean, so it is this branch -- uAtlasRGB -- that
       carries it. A newly inserted entry's cell in the twin is painted by the
       lazy Classic++ job some frames after the insertion, and until that
       first paint lands the sample returns whatever the cell held: 321 texels
       of exactly (0,255,255) over the whole star, on 2 of 8 consecutive
       presents of a first use, gone within seconds. The palette path gives
       the engine's own colours back, and it is the rule the layer above
       resolves every other engine pixel with -- so the one sprite the player
       is always looking at stops depending on that job at all. What is NOT
       closed here: the same window exists for any other sprite whose cell is
       new, which no cursor-local change can reach (gui-renderer.md 24). */
    /* THE RESOLVED RECT, and this is the one that could not be re-derived:
       `dx`/`dy` above came from `mouse_last_client()` on THIS thread at THIS
       instant, and the Vulkan lane runs later in the same iteration. */
    /* THE CURSOR'S QUAD CARRIES A RESOLVED ATLAS RECT TOO, so it is marked like
       a sprite's -- AND ON TODAY'S CODE THIS CANNOT FIRE, which the first
       version of this comment got wrong by claiming `sharp_cursor`'s own
       `tagpu_gaf_atlas_get` could recycle the atlas. It cannot: `atlas_insert`
       answers exhaustion with `a->full = 1` and NULL and never touches `gen`,
       and the three sites that do bump it -- `atlas_drop` (through
       `tagpu_gaf_atlas_reset`/`_forget`), `atlas_repack` (the gui atlas never
       asks for one) and `tagpu_gaf_atlas_lost` -- all run inside the drain or
       outside the present entirely. So nothing moves the generation between
       here and `mir_finish`.
       It stays because the RECT IS RESOLVED THE SAME WAY a sprite's is and the
       next client drawn after the drain would need it; it is marked as a claim
       about this quad, not as a guard that does work today.
       [The false half was FOUND 2026-09-16 by the re-review.] */
    mir_atlas_seen();
    mir_sdraw(TAGPU_GUISK_CURSOR, (float)x0, (float)y0,
              (float)(x0 + w), (float)(y0 + h),
              e->u0, e->v0, e->u1, e->v1, NULL, (int)e->ck);
    s_curDrawn++;
    /* OUR CURSOR IS IN THE SHARP FBO FROM HERE — which is NOT the same as
       being on screen, and the difference is what decides whether the engine
       may draw its own. Only draw_layer samples this layer, and draw_layer
       returns early when there is no presented twin or its size disagrees with
       the frame's (a mode change, a context reset, before the first PK_FRAME
       op drains). On those frames our cursor sits in an FBO nobody reads, so
       telling tagpu_cursown the engine may stand down would leave NO cursor at
       all. The layer's own comment already warns that a client here must not
       assume draw_layer ran; this is that warning obeyed. The screen answer is
       set at the tail of draw_layer instead. [FROM REVIEW 2026-09-14.] */
    s_curInLayer = 1;
}

/* THE MINIMAP'S BASE AT ITS NATIVE SIZE (13.6, G17e).

   The engine fits the TNT's `TED_GENERATED_PIC` into a 126-px box and throws
   half of what it has away; the picture is 252x252. Drawing it at its own size
   into the device-res layer is a free 2x with no new data path and no colour
   drift — and the layer is the ONLY place it can live, because the engine's
   minimap reaches the frame as a copy of the 126-px composite `+0x142DB`, so a
   twin can never hold more than 126 px there.

   ON AT k > 1, THE ENGINE'S AT k = 1 — the rule is stated where it is applied,
   below. `nominimap` turns it off; `mmbase` forces it on at k = 1 too, which is
   how the k = 1 comparison against the engine is taken at all.

   THE PICTURE IS THE WHOLE MAP WITH NOTHING HIDDEN, so a base drawn without
   fog would not merely look wrong — it would show the player terrain they have
   never explored. That is a cheat, and in multiplayer it is the same class of
   cheat 13.6 refuses for the dots, which is why the mask below is not optional.

   13.6 says the fog "comes from the corner-mask grid we already hold as an
   RG8 texture for every world pass". IT CANNOT [MEASURED 2026-09-09]: that
   grid is built around the EYE and covers the viewport — 29x23 cells against
   a 336x400 map on Two Continents — so it has nothing to say about the rest
   of the map. The engine's own minimap fog is a different pass over different
   data: `0x466C20` shades `+0x142E3` into `+0x142DF` per player, reading the
   player id at `main+0x2A43`. Re-deriving that is the thing 13.6 forbids for
   the dots, so the mask below asks the engine instead. */
/* THE LEVEL'S MINIMAP PICTURE, kept on this side (landing 4c). It arrives in
   the level's FIRST in-play packet and in no other — carrying 63 KB in every
   packet of a 900-a-second stream to serve one bake would be absurd — so the
   consumer copies it out of that packet and keys the copy on the level
   generation. That copy is also what a GL re-init re-bakes from, which is why
   it has to outlive the packet that brought it either way.

   It used to be decoded by an observer on the LOADER thread and read from here
   through a generation and a barrier: correct on its own, and exactly what
   "never publish outside the in-play gate" forbids. The publisher decodes it
   now, on the game thread, on the first in-play draw of the level — which the
   engine's own ordering makes the earliest moment every per-map pointer is
   final (the in-play handler is installed only after the loader sets bit 1 of
   main+0x38D75). */
static unsigned char* s_picCopy;
static int      s_picW, s_picH;
static unsigned s_picGen;               /* the packet level_gen it came from, +1 */

static int minimap_pic(const TAGPU_PACKET* pk, const unsigned char** pix,
                       int* w, int* h, unsigned* gen)
{
    const unsigned char* p = tagpu_pk_minimap_pic(pk);
    if (p && pk->mmpic_w > 0 && pk->mmpic_h > 0 && s_picGen != pk->level_gen + 1u) {
        unsigned n = (unsigned)pk->mmpic_w * (unsigned)pk->mmpic_h;
        unsigned char* nb = (unsigned char*)realloc(s_picCopy, n);
        if (nb) {
            memcpy(nb, p, n);
            s_picCopy = nb; s_picW = pk->mmpic_w; s_picH = pk->mmpic_h;
            s_picGen = pk->level_gen + 1u;
            /* the acknowledgement: the publisher stops sending it now */
            tagpu_gui_set_minimap_have(s_picGen);
        }
    }
    if (!s_picCopy || s_picGen != pk->level_gen + 1u) {
        /* WITHDRAW THE ACKNOWLEDGEMENT. It says "stop sending", and the one
           path that raises it without a copy is the `mmbase nominimap` pair
           below; dropping `nominimap` mid-level would then leave this level
           with no picture at all and nothing to recover it. Clearing it here
           makes the pair self-healing for every reason a copy can be absent
           [found by the review of the review's fixes]. */
        tagpu_gui_set_minimap_have(0);
        return 0;
    }
    *pix = s_picCopy; *w = s_picW; *h = s_picH; *gen = s_picGen;
    return 1;
}

static void sharp_minimap(const TAGPU_FRAME* f)
{
    /* THIS IS A CLIENT OF THE SHARP LAYER, NOT A PURE GL FUNCTION. Two things
       here are the Vulkan twin's and not GL's: the `mir_sdraw` records, and
       the CPU BAKE of the minimap picture through the presented palette --
       `s_mmPicRgb`, which `mir_finish` hands over as `mmPic` and
       tagpu_vk_gui.c uploads into an image of its own. Baking it only when a
       GL texture exists would hand the twin a picture it never gets.
       [The vulkan-only plan, landing 4b-3.] */
    const TAGPU_PACKET* pk = f->packet;
    const unsigned char* pic = NULL;
    unsigned gen = 0;
    int pw = 0, ph = 0, mx, my, mw, mh;
    float kx, ky;

    if ((!s_mmbase && !s_mmforce) || !pk) return;
    /* NOT `+0x142F1 & 2`, which is what DrawMinimap 0x466B00 tests: that is a
       DIRTY flag and 0x466B16 CLEARS it in the same breath, so it reads 0 on
       almost every frame [MEASURED 2026-09-09 — the first build of this gated
       on it and drew nothing at all, ever]. The engine can afford a dirty flag
       because its copy lands in the game offscreen and stays there until
       something overdraws it; the sharp layer is cleared at every present, so
       ours has to be redrawn every frame. The honest gate is that the minimap
       surfaces exist at all, which is what being in a game with one means. */
    if (!pk->mm_live) return;
    /* the box the engine fitted it into, in ITS screen pixels, out of the
       packet: it is 0x0 at BuildMinimapSurface's entry, since that call is what
       computes it, so the publisher reads it after the draw like everything
       else in the header */
    mx = pk->mm_box[0]; my = pk->mm_box[1];
    mw = pk->mm_box[2]; mh = pk->mm_box[3];
    if (mw <= 0 || mh <= 0) return;
    /* HUD SCALE (20): the engine fitted the box into its 1x panel and the
       composite magnifies that panel, so the sharper copy has to land on the
       magnified box, not the box the engine measured. The corner goes through
       the same map the pointer does and the size through the same s, so the
       three agree by construction rather than by two rounding rules meeting. */
    {
        int hq8 = 256;
        tagpu_hud_live(NULL, NULL, &hq8);
        tagpu_hud_to_screen(&mx, &my);
        mw = mw * hq8 / 256;
        mh = mh * hq8 / 256;
        if (mw <= 0 || mh <= 0) return;
    }
    kx = (f->game_width  > 0) ? (float)f->vp_w / (float)f->game_width  : 1.0f;
    ky = (f->game_height > 0) ? (float)f->vp_h / (float)f->game_height : 1.0f;
    /* AT k = 1 THE ENGINE'S MINIMAP STANDS, and that is not timidity — it is
       where the arithmetic says the win is. The box is 106x126 DEVICE pixels
       there, so drawing it from a 252x252 source throws three quarters of the
       picture away and lands on a nearest downsample where the engine used its
       own stretch: no sharper, and 7232 px away from the oracle every phase-1
       measurement is taken against. The extra resolution only starts paying at
       k > 1, where the engine blows its 126-px picture up and we do not.
       `mmbase` forces it on anyway, which is how the k = 1 comparison above was
       taken at all. */
    if (kx <= 1.001f && ky <= 1.001f && !s_mmforce) return;
    /* THE STANDING REQUEST, AND IT IS RAISED HERE RATHER THAN AT THE TOP. The
       publisher interleaves the three minimap surfaces and carries the level's
       picture only while this is up, and at k = 1 (the gate just above) the
       sharp minimap is deliberately the engine's own — so a request raised
       before the gate would have the game thread pay ~13 KB of interleave per
       publish, for ever, in every session that never resizes its window. Past
       the gate the first frame raises it and the next one draws, which costs
       two frames of the engine's own minimap after a resize and nothing at all
       before one. Dropped by the module's own watchdog after 90 silent
       frames. */
    tagpu_gui_set_want_minimap(1, f->frame_counter);
    if (s_mmbase) {
        if (!minimap_pic(pk, &pic, &pw, &ph, &gen)) return;
        if (pw <= 0 || ph <= 0) return;
    } else {
        /* `nominimap` with `mmbase`: the surfaces are wanted and the picture is
           not. Acknowledge it anyway — the acknowledgement means "stop sending
           it", not "I have used it", and without this the publisher would put
           63 KB in every packet of the level for a consumer that never reads
           it. The combination is a harness one and this is two lines. */
        tagpu_gui_set_minimap_have(pk->level_gen + 1u);
    }
    /* `s_mmBaked`, NOT `s_mmTex`: the question is whether THIS palette's
       resolve of THIS map is in `s_mmPicRgb`, and the GL texture is one of
       the two places that answer is put. [Vulkan-only plan, 4b-3.] */
    if (s_mmbase && (gen != s_mmGenSeen || tagpu_pal_serial() != s_mmPalSeen || !s_mmBaked)) {
        /* THE PALETTE THE SCREEN IS SHOWN WITH, not main+0x143A7: tagpu_pal.h
           owns that resolution for the whole DLL, and its serial above is the
           key this bake is invalidated on. NULL only before any palette is
           readable, and then there is nothing to bake — retry next frame. */
        const unsigned char* pal = tagpu_pal_live();
        if (!pal) return;
        /* resolved HERE, once per map load and once per palette change, rather
           than per fragment: the sampler has to see colour for the filter above
           to mean anything, and a fade is a run of palette changes that costs a
           190 KB re-upload each — against 13 356 texels of engine surface this
           module already uploads every single frame */
        {
            unsigned n = (unsigned)pw * (unsigned)ph, i;
            if (n * 3 > s_mmPicCap) {
                free(s_mmPicRgb); s_mmPicCap = n * 3 + 4096;
                s_mmPicRgb = (unsigned char*)malloc(s_mmPicCap);
                if (!s_mmPicRgb) { s_mmPicCap = 0; return; }
            }
            for (i = 0; i < n; i++) {
                const unsigned char* e = pal + 4 * (unsigned)pic[i];
                s_mmPicRgb[3 * i] = e[0]; s_mmPicRgb[3 * i + 1] = e[1]; s_mmPicRgb[3 * i + 2] = e[2];
            }
            s_mmPicSerial++;
        }
        s_mmGenSeen = gen; s_mmPalSeen = tagpu_pal_serial(); s_mmTW = pw; s_mmTH = ph;
        s_mmBaked = 1;
    }
    /* the program, its uniforms and every texture unit are set by whichever
       block below actually draws -- `s_mmProg` for the picture, `s_sharpProg`
       for the box. Only the array bindings are shared. */
    /* THE WHOLE PICTURE INTO THE WHOLE BOX, and that is the engine's own
       mapping rather than a guess: 0x466845 builds a context for the box-sized
       surface and 0x46685F hands the picture straight to the stretch
       `0x4B95A0`, so the 252x252 square is squashed into an aspect-correct box
       (106x126 on a 336x400 map). Full 0..1 UVs reproduce exactly that. */
    if (s_mmbase) {
        /* THE ENGINE'S OWN PAIR, this frame: fogged base in R, unfogged in G.
           Both are 8bpp OFFSCREENs (w, h, pitch, base as four ints), read here
           on the render thread while the game thread repaints their PIXELS —
           that part has the same standing as the fork's own surface upload,
           and the worst it costs is one frame's fog against another's. The
           descriptors are another matter: they carry a base pointer and a
           pitch, so a stale or torn one is a wild read, not a stale picture.
           What makes them readable is their lifetime — the minimap build
           (0x4669B0, from the level load at 0x4919C3) stores the three
           pointers once, and 0x466AA0 frees and nulls them inside the
           teardown cascade tagpu_reclaim fences — and the cross-check below
           is the DATA bound on top of it (cross-thread-engine-reads.md §4).
           13 KB. */
        const unsigned char* rg = tagpu_pk_minimap(pk);
        int ew = pk->mm_w, eh = pk->mm_h;
        /* NOT A FAULT ON THE FIRST FRAME AFTER ARMING: the request above is
           what makes the publisher copy them, so the frame that raises it finds
           nothing and the next one has them. It IS a fault if it keeps
           climbing — the publisher refused the descriptors, and its own
           `gui: … refused=` counter says so. */
        if (!rg) { s_mmNoEng++; return; }
        /* `mir_finish` re-reads this pair from the packet for the twin, so the
           GL texture below is the GL lane's copy and nothing else depends on
           it. [Vulkan-only plan, 4b-3.] */
        /* how much of the map the engine is hiding right now, in its own
           texels — the number that says whether a run had any fog to mask at
           all. A fully-mapped skirmish reads 0 and proves nothing about the
           mask; a fogged one is the only fixture that tests it. */
        {
            int d = 0, n = ew * eh, i;
            for (i = 0; i < n; i++) if (rg[3 * i] != rg[3 * i + 1]) d++;
            s_mmFogged = (unsigned)d;
        }
        mir_sdraw(TAGPU_GUISK_MM, (float)mx * kx, (float)my * ky,
                  (float)(mx + mw) * kx, (float)(my + mh) * ky,
                  0.0f, 0.0f, 1.0f, 1.0f, NULL, 0);
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
        /* the same resolution the base was baked through, for the same reason */
        const unsigned char* pal = tagpu_pal_live();
        const int* vr = pk->mm_view;
        int ci = (int)pk->mm_viewcol;
        float r = pal ? pal[4 * ci] / 255.0f : 0.0f,
              g2 = pal ? pal[4 * ci + 1] / 255.0f : 0.0f,
              b2 = pal ? pal[4 * ci + 2] / 255.0f : 0.0f;
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
        if (pal && R >= L && B >= T) {
            int e;
            /* four one-GAME-pixel edges, so the box keeps the weight the engine
               gives it rather than thinning to a device pixel as k grows */
            for (e = 0; e < 4; e++) {
                float x0, y0, x1, y1;
                if (e == 0)      { x0 = (float)L;     y0 = (float)T;     x1 = (float)(R + 1); y1 = (float)(T + 1); }
                else if (e == 1) { x0 = (float)L;     y0 = (float)B;     x1 = (float)(R + 1); y1 = (float)(B + 1); }
                else if (e == 2) { x0 = (float)L;     y0 = (float)T;     x1 = (float)(L + 1); y1 = (float)(B + 1); }
                else             { x0 = (float)R;     y0 = (float)T;     x1 = (float)(R + 1); y1 = (float)(B + 1); }
                {   /* the colour is already through the presented palette */
                    float bc[4]; bc[0] = r; bc[1] = g2; bc[2] = b2; bc[3] = 1.0f;
                    mir_sdraw(TAGPU_GUISK_FLAT, x0 * kx, y0 * ky, x1 * kx, y1 * ky,
                              0, 0, 0, 0, bc, 0);
                }
            }
        }
    }
    /* only when something was actually drawn: with `mmbase` set and
       `nominimap` also set, both blocks above are skipped and the heartbeat
       would otherwise report a minimap this module never put on screen. */
    if (s_mmbase) s_mmDrawn++;
}

static void sharp_begin(const TAGPU_FRAME* f)
{
    /* THIS FUNCTION IS THE SHARP LAYER'S FRAME, NOT ITS GL TARGET. It decides
       `s_sharpOn`/`s_sharpInk` -- which draw_layer hands the Vulkan twin -- and
       it runs the two clients whose `mir_sdraw` records ARE the twin's copy of
       the layer. Only the FBO and the clear are GL. [Vulkan-only plan, 4b-3.] */
    int w = f->vp_w, h = f->vp_h;
    s_sharpOn = 0;
    /* ...AND THE INK WITH IT. `s_sharpOn` was reset on every call and this was
       not, so a session that once had coverage and then took one of the early
       returns below (an FBO re-create failing latches `s_sharpFailed`)
       published `sharpOn = 1` with no quads for ever -- which the Vulkan pass
       reads as "the GL lane has coverage I did not produce" and stops
       compositing for the session. Re-decided from the two clients' own
       counters at the tail. [FOUND 2026-09-16, the landing-3 review.] */
    s_sharpInk = 0;
    /* A FAILURE LATCHED here, in `init_gl`'s `s_gl = 2` shape; `sharp_drop()`
       so without this every present would re-enter the allocation below --
       generating, sizing and deleting a vp_w x vp_h texture and appending a log
       line 60 times a second for the rest of the session, which buries the
       heartbeat. The layer is additive, so staying off costs sharpness only. */
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    /* the layer's SPACE is the frame's viewport, and that is true with no
       target to hold it: the clients clamp to it and record device-pixel
       rects, and `mir_finish` publishes it as sharpW/sharpH. */
    s_sharpW = w; s_sharpH = h;
    if (s_sharptest) {
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
        {
            static const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
            static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            mir_sdraw(TAGPU_GUISK_FLAT, 0.0f, 0.0f, 64.0f, 64.0f, 0, 0, 0, 0, green, 0);
            mir_sdraw(TAGPU_GUISK_FLAT, 100.0f, 0.0f, 101.0f, (float)h, 0, 0, 0, 0, white, 0);
        }
    }
    /* the two clients, and whether either of them put anything there. Their
       own counters answer it, so neither function grows a flag of its own. */
    {
        unsigned c0 = s_curDrawn, m0 = s_mmDrawn;
        sharp_cursor(f);                /* 13.5's cursor: the layer's first real client */
        sharp_minimap(f);               /* 13.6's base, behind `mmbase` while it is alone */
        s_sharpInk = (s_sharptest || s_curDrawn != c0 || s_mmDrawn != m0) ? 1 : 0;
    }
    /* THE CLEAR COLOUR IS MODULE-WIDE STATE AND WE OWN IT AT (0,0,0,0).
       BOTH PAINTERS THIS PROTECTED ARE GONE and the invariant is kept anyway:
       render_ogl.c repainted the letterbox bars with a bare glClear on every
       frame whose viewport was offset (`if (viewport.x || viewport.y)`), and
       the glshot capture did the same — neither set a colour of its own, so
       whatever was left here is what they painted. render_ogl.c went in 11-2
       and the capture in 11-5e-1, so nothing reads this state today. It stays
       stated because the property is about THIS module's exit condition, not
       about who consumed it: any future painter inherits whatever we leave.
       The clear above already ends at (0,0,0,0) on the normal path; this is
       the statement of the invariant, not a second setter.
       `sharp_begin` also leaves the VIEWPORT at (0,0,w,h) with the scissor
       test off and the default framebuffer bound. That is safe only because
       tagpu_gui_present rebinds the target FBO and the frame's viewport after
       draw_layer -- which can return early -- so a future client drawing here
       must not assume draw_layer ran. */
    s_sharpOn = 1;
}

static void draw_layer(const TAGPU_FRAME* f)
{
    TWIN* t = s_presented ? twin_find(s_presented) : NULL;
    float ky;
    int L = 0, T = 0, W = 0, H = 0, key;
    int hudPw = 0, hudBh = 0, hudQ8 = 256;
    if (!t || t->w != f->game_width || t->h != f->game_height) return;
    /* the palette was uploaded in tagpu_gui_present, BEFORE restore_step
       decided s_colValid. Uploading it again here -- after a drain that can be
       thousands of ops long, during which the game thread may have set a new
       one -- would show restored texels resolved through the palette their
       restore snapshotted beside indexed texels resolved through a NEWER one,
       which is the "wrong art" 3.4 exists to prevent. One upload per frame,
       and it is the one s_colValid was decided against. */
    /* THE TRUE VIEWPORT, from this frame's packet (landing 2) — the rect the
       game thread published, the same one the world composite keyed on this
       frame. On a frame with no in-game packet the LAST one seen stands: the
       level's tail (the out-of-game packet, a refused packet) can still carry
       the key fill in the engine's surface while terrown's fill flag is up,
       and an empty rect there would show the key colour raw for that frame
       (landing review); between levels the old level's rect is the right one
       and the next level's first packet replaces it. */
    {
        static int s_lastVp[4];
        if (f->packet && f->packet->in_game) {
            s_lastVp[0] = f->packet->vp[0]; s_lastVp[1] = f->packet->vp[1];
            s_lastVp[2] = f->packet->vp[2]; s_lastVp[3] = f->packet->vp[3];
        }
        L = s_lastVp[0]; T = s_lastVp[1]; W = s_lastVp[2]; H = s_lastVp[3];
    }
    key = tagpu_terr_key();
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
    /* HUD scale (20): the two reserved integers and s, resolved against the
       surface being presented -- so the shell, whose surface is 640x480
       whatever the Screen Size row says, resolves to stock and the map is the
       identity there without a signal of its own.

       DERIVED ONCE, HERE, AND OUTSIDE THE GL BLOCK. It is a property of the
       FRAME, and two things want it: the layer's uniform and the hand-over's
       `hud[]`. Leaving it inside the draw left `s_hudS` unwritten on a lane
       that does not draw, so the 300-frame heartbeat printed `s=1.000` for the
       life of the process however the HUD was scaled -- the same defect this
       landing fixed one screen away in `fog=`, and the same rule: an
       instrument that is only true on one lane sends the next session to the
       wrong function. It also ends the second derivation the mirror block used
       to make, which this function's own comment calls "a second thing that
       can drift from the draw it is supposed to describe".
       [FROM THE 4b-3 LANDING REVIEW.] */
    if (!tagpu_hud_live(&hudPw, &hudBh, &hudQ8)) { hudPw = hudBh = 0; hudQ8 = 256; }
    s_hudS = (float)hudQ8 / 256.0f;
    /* THE COMPOSITE IS GL; THE HAND-OVER BELOW IS THE VULKAN TWIN'S, and it
       is the whole reason this function may not simply return on a lane with
       no GL. `s_mHand` describes the composite the twin is to run, derived
       from this frame's own locals -- skipping it would leave the twin with
       no UI at all. Landing 4b-2 met this shape in tagpu_posedraw_live();
       this is the same one, in the function that hands the UI over.
       [The vulkan-only plan, landing 4b-3.] */
    /* THE COMPOSITE HAS RUN, so anything the sharp layer carried is now on the
       frame the player sees. This — not the draw into the layer — is what lets
       the engine's own cursor blit stand down (tagpu_cursown.h). Every early
       return above leaves it 0 and the engine keeps its cursor.

       ON ROUTE E THIS FLAG IS WEAKER THAN IT IS ON THE GL LANE, AND THE
       CONSUMER IS WHERE THAT IS MADE GOOD NOW. There is no composite here to
       have run: `s_mirRec` says the RECORD survived to this point, which is not
       the same as the twin having drawn it, and `tagpu_vk_gui_prepare` and
       `tagpu_vk_gui_record` between them can still refuse the frame afterwards.

       SO THIS IS NO LONGER THE WHOLE ANSWER, and nothing here has to change.
       Gate 4's last landing moved `tagpu_cursown_publish` to AFTER
       `tagpu_vk_frame` in render_vk.c and made it
       `cur_drew && tagpu_vk_ui_composited()` -- this flag is the first half and
       the seam's own "the composite reached the command buffer" is the second.
       The frames where the two disagree are counted, as `held=` in this
       function's own heartbeat. [The gap was named by the 4b-3 landing review
       and closed by gate 4's last item; gpu-status.md §2.57.] */
    if (s_sharpOn && s_curInLayer && s_mirRec) s_curDrew = 1;

    /* G19f: the uniforms this composite just ran with, for the Vulkan mirror.
       Taken HERE rather than recomputed in the publish: every one of them is a
       local this function derived, and a second derivation is a second thing
       that can drift from the draw it is supposed to describe. */
    if (s_mirRec) {
        s_mHand.presented = s_presented;
        s_mHand.surfW = t->w; s_mHand.surfH = t->h;
        s_mHand.strict = (s_strict && have_engine_frame(f)) ? 1 : 0;
        s_mHand.key = key;
        s_mHand.vpKey = tagpu_terrown_filled() ? key : -1;
        s_mHand.vpL = (float)L; s_mHand.vpT = (float)T;
        s_mHand.vpW = (float)W; s_mHand.vpH = (float)H;
        s_mHand.curEng[0] = s_curEng[0]; s_mHand.curEng[1] = s_curEng[1];
        s_mHand.curEng[2] = s_curEng[2]; s_mHand.curEng[3] = s_curEng[3];
        s_mHand.curOurs = s_curOwn ? 1 : 0;
        s_mHand.guard = have_engine_frame(f) ? 1 : 0;
        s_mHand.scaleX = s_k; s_mHand.scaleY = ky;
        s_mHand.sharpOn = s_sharpInk ? 1 : 0;   /* COVERAGE, not existence */
        /* `t->rgb` was the second term until [landing 11-4b] took the GL ids off
           TWIN; `s_colValid` alone says the same thing and is pinned at 0 (see its
           declaration), so this publishes 0 -- correctly: this lane has no colour
           twins for `tagpu_vk_gui.c` to sample. */
        s_mHand.colourTwins = s_colValid ? 1 : 0;
        s_mHand.vpX = f->vp_x; s_mHand.vpY = f->vp_y;
        s_mHand.vpW_gl = f->vp_w; s_mHand.vpH_gl = f->vp_h;
        /* the same four numbers the uniform above ran with, not a second
           reading of tagpu_hud_live() */
        s_mHand.hud[0] = (float)hudPw; s_mHand.hud[1] = (float)hudBh;
        s_mHand.hud[2] = 256.0f / (float)hudQ8; s_mHand.hud[3] = s_hudS;
        s_mLayer = 1;
    }
}

/* leave nothing of ours bound: the drain binds twin FBOs, the atlas, the
   copy source and our VAO/program, and draw_layer may not have run */
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
    /* the lever is its own file, polled on this cadence like every other pass's */
    s_ab = GetFileAttributesA(AB_FILE) != INVALID_FILE_ATTRIBUTES;
    if (!s_ab) s_abDone = 0;
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
    s_mmforce = on && strstr(buf, "mmbase") != NULL;
    s_mmbase = on && !(strstr(buf, "nominimap") != NULL);
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
    /* NOTHING IS CLEARED HERE. `s_curDrew` is cleared by the TAKE, once per
       frame, from the render_ogl.c bracket — so a present that returns early
       below, and a frame that never calls this function at all, both answer 0
       through exactly the same path. Clearing on entry and setting on the draw
       looks equivalent and is not: the engine draws its cursor from inside the
       flip, on another thread, while this present runs, so a window that reads
       0 while we composite lets one engine draw per frame through — the bug
       wearing the counters of a fix (measured 2026-09-13 at 60/s). */
    if (!tagpu_gui_installed() || !f) return;
    poll();
    if (!s_on) {
        /* off: nothing is published, but drain whatever was */
        g_guiq.qTail = g_guiq.qHead; g_guiq.aTail = g_guiq.aHead;
        return;
    }
    /* THE GL LANE'S OBJECTS, and its gate: 68 GL calls that make the programs,
       the VAO and the palette texture this module draws with. A lane with no
       GL context makes none of them and must not be stopped by their absence
       -- everything below this line either gates itself or is the Vulkan
       twin's. [The vulkan-only plan, landing 4b-3.] */
    /* the atlas alone: the sprite table this module resolves against, which
       is the Vulkan twin's as much as the GL lane's */
    if (!atlas_setup()) return;
    /* `upload_palette()` and `restore_step()` STOOD HERE, the present's first two
       statements [the vulkan-only plan, landing 11-4b]. Both opened with
       `if (tagpu_vk_owns_present()) return;` -- landing 4b-3 had already found them
       PURE GL, feeding the twin nothing. `restore_step` leaving `s_colValid` at 0 is
       not a loss: that is what the record's `restored` term reads, and "indexed" is
       the honest answer for a lane with no restored GL twin to sample. */
    /* THE STEP THAT STOOD HERE WAS THE GL RESTORER'S and it went in 11-5e-2
       with the backend it stepped. It was already unreachable: the block was
       gated on `s_atlas.job`, which is the GL job, and no atlas has had one
       since landing 11-4c stopped filling `a->tex`. The Vulkan restorer is
       stepped from `tagpu_vk.c` inside the frame's command buffer and needs
       nothing from here.

       WHAT IT WAS FOR IS STILL THE RULE FOR WHOEVER STEPS A RESTORER HERE:
       step it BEFORE THE DRAIN, so that what it paints this frame is what the
       drain's sprites sample. The shell, and the game with the world passes
       disarmed, is the case that made it necessary -- the native pass returns
       early with no unit array, so nothing else would drain this atlas's
       queue and every sprite would read alpha 0 from an unpainted twin, the
       UI staying indexed for ever and silently. */
    /* G19f landing 4 MIRRORED THE RESTORED TWIN HERE, between the step that
       painted it and the drain whose sprites sample it, so that the bytes a
       second backend was handed were the bytes this lane's own draws read on
       the same frame rather than a frame behind. The mirror was
       `tagpu_gaf_atlas_mirror_rgb` + `_step`, which is `glReadPixels` off an
       FBO; `oglu_load_dll` has no caller, so it never produced a row and the
       hand-over's `atlasRgb` was NULL on every frame. Removed in 11-5e-2b
       along with the fields it fed. The ORDERING argument above is unaffected
       and still governs the indexed mirror, which is written by the paint. */
    mir_begin();            /* G19f: the Vulkan mirror records this drain */
    drain();
    /* AFTER the drain, which binds twin FBOs and leaves one bound, and before
       the layer that samples it: the sharp layer is cleared for this frame
       (and, under `sharptest`, filled) while nothing of the composite has
       been written yet. */
    sharp_begin(f);
    {
        /* ONE LEVER, ONE FRAME, AND THE FLAG TRAVELS WITH THE DATA (the
           roadmap's A/B shape). The Vulkan lane captures the frame this flag
           arrived on rather than whichever one its own poll landed on --
           `mir_finish` hands it over with the ops. */
        int taking = s_ab && !s_abDone;
        /* THE VULKAN HALF IS ARMED BY ITS OWN UNLINK. The claim below used to
           be `wrote ? 1 : 0` -- true only when a GL capture had reached the
           disk -- and on a lane that draws no GL there is no capture to write,
           so the Vulkan half could never fire and this pass could not be
           measured at all. `tagpu_vk_ab_arm` is the shape the five world passes
           took in 4b-1: it unlinks the target and grants the claim only when the
           file is gone, so a stale capture is never paired with a fresh one.
           [The vulkan-only plan, landing 4b-3; the GL half it used to need as
           well went in 4d-2, with route D.] */
        draw_layer(f);
        if (taking) {
            s_abDone = 1;
            /* THE A/B CLAIM, which is all that is left of it. Until landing 4d-2
               this pass also captured a GL half (`tagpu_abshot.c`) and, where the
               GL lane drew, claimed the Vulkan one only if that half had reached
               the disk. Route D went in 4d-1 and the GL half had nothing to pair
               with. `tagpu_vk_ab_arm` unlinks the target `_vk.ppm` at the instant
               the claim latches, which is what makes the file on the disk this
               arming's; diff it against a capture from another BUILD.

               THE CLAIM IS STILL THIS FRAME'S OR NOBODY'S. It used to be set here
               and cleared only inside `mir_finish`, so a frame that published no
               record carried the flag forward and the two lanes captured
               DIFFERENT frames -- the one thing the "one lever, one frame" rule
               exists to prevent, and why several captures in the first measuring
               session would not pair. [FOUND 2026-09-16, the landing review.] */
            s_abFrame = tagpu_vk_ab_arm("gui");
        }
    }
    mir_finish(f);          /* G19f: close and publish the frame's record */
    /* sharp_begin left the default framebuffer bound and the viewport at the
       layer's size; this puts the frame's target and viewport back for
       whatever draws after us. Both are GL state and there is none to restore
       on a lane with no context. [The vulkan-only plan, landing 4b-3.] */
    if (f->frame_counter - last >= 300) {
        /* 411 literal chars + 69 conversions: the worst case is ~1120, so the
           buffer is 1152 and is now close enough that the next group needs a
           bigger one. It grew with the merge (the cursor, string and minimap
           counters joined `assets=`/`light=`), again for the GAF scratch
           counters, and again for the glyph scratch's and the producer's font
           slots, and _snprintf does not NUL-terminate what it truncates --
           hence the explicit terminator below. The figures in this comment were
           318/52 and stale by two merges when the landing review counted them;
           they are measured from the format string rather than remembered.
           RE-MEASURED 2026-09-18 for landing 8b's `rects=`, by counting the
           format string itself rather than by eye: 430 literal characters and
           72 conversions, worst case 1181 with the terminator. 1152 would have
           truncated -- and truncation here is silent and takes the TAIL, where
           `fps=` lives. [The 1152 was 9 bytes of headroom when 8a's review
           counted it; one more counter spent all of it.] */
        char b[1280];
        int palDiffAt, palDiff = tagpu_pal_diff(&palDiffAt);
        static LARGE_INTEGER t0, fq;
        LARGE_INTEGER t1;
        double fps = 0.0;
        unsigned gCached = 0, gDrops = 0; int gFonts = 0;
        unsigned pGlyphs = 0, pResends = 0, pRefused = 0, pRecycles = 0;
        tagpu_text_glyph_stats(&gCached, &gDrops, &gFonts);
        tagpu_gui_font_stats(&pGlyphs, &pResends, &pRefused, &pRecycles);
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t1);
        if (t0.QuadPart) fps = (double)(f->frame_counter - last) * (double)fq.QuadPart / (double)(t1.QuadPart - t0.QuadPart);
        t0 = t1;
        last = f->frame_counter;
        _snprintf(b, sizeof b, "gui: twins=%d presented=%08X drained=%u seeds=%u sprites=%u copies=%u pixels=%u pixdrop=%u bars=%u rects=%u clears=%u atlas=%d/%d lost=%u strict=%d resets=%u overflows=%u gafnoplane=%u gafreseed=%u gafscratch=%u/%u/%u strrearm=%u glyscratch=%u/%u gfont=%u/%u/%u/%u stalls=%u skipped=%u palchg=%u paldiff=%d@%d palsrc=%d cpp=%d assets=%d light=%d col=%u/%d colvalid=%d rearms=%u rgb=%u k=%.3f s=%.3f sharp=%dx%d curs=%d,%dx%d,dev=%d,sc=%.2f,drawn=%u,warm=%u str=%u/%u,miss=%u,reseed=%u,repack=%u,glyphs=%u/%u,fonts=%d arena=%u mirlost=%u mm=%u,fog=%u/%u,noeng=%u fps=%.1f",
                  s_ntwins, s_presented, s_drained, s_seeds, s_sprites, s_copies, s_pixels, s_pixDropped, s_bars, s_rects, s_clears,
                  s_atlas.n, s_atlas.max, s_lostSprites, s_strict, g_guiq.resets, g_guiq.overflows, g_guiq.gafnoplane, g_guiq.gafreseed, g_guiq.gafhigh, g_guiq.gaflost, g_guiq.gafbaddec, g_guiq.strrearm, g_guiq.glyhigh, g_guiq.glylost, pGlyphs, pResends, pRefused, pRecycles, g_guiq.stalls,
                  s_skipped, tagpu_pal_changes(), palDiff, palDiffAt, tagpu_pal_presented(),
                  tagpu_classicpp_on() ? 1 : 0, tagpu_classicpp_assets() ? 1 : 0,
                  tagpu_classicpp_lit() ? 1 : 0,
                  s_colTwins, s_ntwins, s_colValid, s_rearms, s_atlas.rgb,
                  s_k, s_hudS, s_sharpW, s_sharpH,
                  s_curOwn, s_curW, s_curH, s_curDev, s_cursorScale, s_curDrawn, s_curWarm,
                  s_strings, s_glyphs, s_strMiss, s_strReseed, s_strRepack, gCached, gDrops, gFonts,
                  /* the arena head is MONOTONIC, so the delta between two of these
                     lines is the bytes the producer wrote in 300 frames — which is
                     what `nostring` is A/B'd on (13.4: ~40 bytes where a text op
                     carried ~968) and what §7's cadence note is about */
                  g_guiq.aHead, s_mirLost, s_mmDrawn,
                  /* THE PAIR'S SIZE AS READ, not as uploaded. It was
                     `s_mmEngW * s_mmEngH` -- the GL texture's -- which reads 0
                     on a lane that makes no texture, so the line said "no
                     engine pair" about a pair this module had just counted
                     13227 fogged texels in. The packet is where both lanes get
                     it. [The vulkan-only plan, landing 4b-3.] */
                  s_mmFogged,
                  f->packet ? (unsigned)(f->packet->mm_w * f->packet->mm_h) : 0u,
                  s_mmNoEng,
                  /* `cursown=<armed>/<of>,<skip>,held=` STOOD HERE and is gone
                     with the module. It reported which of the engine's four
                     cursor-blit call sites were redirected and whether the blit
                     was being skipped right now -- a question about the
                     COMPOSITE, where the engine's frame was the bottom layer
                     and its cursor would have shown through beneath ours.
                     Nothing the engine blits reaches the screen now, so there
                     is nothing to suppress: suppressing it would only hole the
                     golden source. [The clean cut; the UI rebuild does not
                     bring it back.] */
                  fps);
        b[sizeof b - 1] = '\0';
        slog(b);
    }
}

/* G19f: the frame's record is closed and published. Everything the port
   needs that is NOT an op goes in here -- the atlas texels, the palette, the
   engine's own frame -- each of them a copy or a mirror this module owns,
   because the Vulkan lane does not run until after `tagpu_packet_frame_end`
   and cannot read a GL texture at all. */
static void mir_finish(const TAGPU_FRAME* f)
{
    /* PUBLISHED EVEN WHEN THE COMPOSITE DID NOT DRAW. `draw_layer` returns
       early when the presented surface has no twin or its size has moved --
       and on those frames the drain still APPLIED this frame's ops to the GL
       twins. Withholding the record leaves the Vulkan store behind by exactly
       those ops, for the session, which is the same hole the consumer was
       restructured to close and which was still open here.
       `presented = 0` is how the consumer is told the composite state is not
       valid: it replays and does not draw. [FOUND 2026-09-16, the landing
       review.] */
    /* AN ABANDONED FRAME IS PUBLISHED, NOT WITHHELD, and this is the same hole
       the paragraph above closed at the other end. `s_mirRec` goes to 0 when
       the op array or the arena refuses to grow; withholding the record then is
       byte-identical, to the consumer, to "the lane is not armed" -- so it
       believes it is level while it is a frame of ops behind, for the session.
       A repack between a recorded string and here is the same statement about
       the same frame. Both publish `lost`, which the consumer answers with the
       behind state. [FOUND 2026-09-16, verifying the landing-2 review.] */
    if (!s_mirWant) { s_mHave = 0; return; }
    /* THE TWO ATLASES THE RECORD CARRIES RESOLVED RECTS INTO, checked the same
       way and for the same reason: a rect is valid only for the generation it
       was read in, and both of these can move in the middle of a present --
       the glyph atlas when a string repacks it, the UI atlas when a sprite
       fills it or `twins_reset` recycles it. */
    if (!s_mirRec || (s_mStrAny && s_mStrGen != tagpu_text_glyph_gen()) ||
        (s_mAtAny && s_mAtGen != s_atlas.gen)) {
        memset(&s_mHand, 0, sizeof s_mHand);
        s_mHand.frame = f->frame_counter;
        s_mHand.lost = 1;
        s_mHave = 1; s_mTaken = 0; s_mFrame = f->frame_counter;
        s_mirLost++;
        return;
    }
    s_mHand.lost = 0;
    if (!s_mLayer) {
        s_mHand.presented = 0; s_mHand.surfW = s_mHand.surfH = 0;
        s_mHand.strict = 0; s_mHand.guard = 0; s_mHand.sharpOn = 0;
        s_mHand.colourTwins = 0;
    }

    s_mHand.frame = f->frame_counter;
    s_mHand.ops = s_mOps; s_mHand.nops = s_mNOps;
    s_mHand.arena = s_mArena; s_mHand.alen = s_mALen;
    s_mHand.otherOps = s_mOther;
    s_mHand.ab = s_abFrame;

    /* THE UI ATLAS, as bytes. Asked for once; `tagpu_gaf_atlas_mirror` makes
       it correct from the instant it exists by marking every painted entry for
       repaint, so there is no window where a sprite samples texels that were
       never written (tagpu_gaf.h). */
    if (tagpu_gaf_atlas_mirror(&s_atlas) && s_atlas.mirror) {
        int rows = s_atlas.shelfY + s_atlas.shelfH;
        if (rows < 1) rows = 1;
        if (rows > s_atlas.dim) rows = s_atlas.dim;
        s_mHand.atlas = s_atlas.mirror;
        s_mHand.atlasDim = s_atlas.dim;
        s_mHand.atlasRows = rows;
        s_mHand.atlasSerial = s_atlas.mirrorSerial;
    } else {
        s_mHand.atlas = NULL; s_mHand.atlasDim = 0;
        s_mHand.atlasRows = 0; s_mHand.atlasSerial = 0;
    }

    /* THE RESTORED UI ATLAS (landing 4). Read back where the paint happens,
       above -- nothing is read here.
       `atlasRgbRows` IS WHAT THE MIRROR HOLDS, and the first version of this
       comment made two claims about it that are false. It is NOT true that
       "rows past the shelf name no entry, so uploading them can change no texel
       any op samples": `tagpu_gaf_atlas_put` runs inside the DRAIN, after the
       read-back, so a cell lands above `atlasRgbRows` on the very next frame --
       which is how 166 827 px of undefined memory reached the screen at 1080p
       and 0 px at 1024x768 on the same build. And the read-back could shrink
       what it had filled -- `rgb_mirror_zeroed` kept the row count and zeroed
       the content, a context loss dropped the count to 0, and the consumer
       cleared its own image on a shrink for exactly that reason. Both of those
       went with the read-back in 11-5e-2b part 2, along with the consumer's
       shrink clear; the paragraph is kept because the 1080p fault above is why
       a row count is published at all, and the next lane to publish one will
       want it.
       IT IS NOT GATED ON `colValid`. Withholding it on an invalid frame would
       make the image come and go under the consumer for a reason that has
       nothing to do with the image, and nothing would be gained: whether a
       texel of it is ever SAMPLED is `TAGPU_GUICOL_ON`, carried per op, and no
       op carries it while `s_colValid` is 0 -- nor at all, since `twin_sprite`
       and `twin_copy` both return 0 unconditionally.
       THE PUBLICATION ITSELF WENT IN 11-5e-2b. `atlasRgb`/`atlasRgbRows`/
       `atlasRgbSerial` were filled from `s_atlas.mirrorRgb` here; that mirror
       is `glReadPixels` and is never armed, so all three were NULL/0 on every
       frame. Nothing replaces them: this lane arms no restore list, so there
       is no route to Classic++ colour in the UI until one is built. */
    s_mHand.colRearm = s_rearms;

    /* ---- THE SHARP LAYER. `sharpOn` above already says whether anything has
       COVERAGE; this is what produced it. The list is copied BY VALUE, so its
       lifetime is the struct's and not the arena's.
       A list that OVERFLOWED is published as a refusal (`nsdraw` past the cap),
       not as a truncation: the layer is cleared every present, so one frame the
       consumer cannot composite is one frame, never a store out of step. */
    s_mHand.sharpW = s_sharpOn ? s_sharpW : 0;
    s_mHand.sharpH = s_sharpOn ? s_sharpH : 0;
    s_mHand.nsdraw = s_mNSDraw;
    if (s_mNSDraw > 0 && s_mNSDraw <= TAGPU_GUI_SDRAW_MAX)
        memcpy(s_mHand.sdraw, s_mSDraw, (size_t)s_mNSDraw * sizeof s_mSDraw[0]);
    s_mHand.mmPic = s_mmPicRgb; s_mHand.mmPicW = s_mmTW; s_mHand.mmPicH = s_mmTH;
    s_mHand.mmPicGen = s_mmPicSerial;
    /* THE ENGINE'S PAIR IS COPIED, NOT ALIASED, and the comment that stood here
       -- "packet memory, valid for this frame, which is the rule the whole
       hand-over already lives under" -- WAS WRONG about which rule applies.
       The hand-over's rule is the FRAME; the packet's is stricter than that.
       It is given back inside `tagpu_packet_frame_end`, which `render_ogl.c`
       calls BEFORE `tagpu_vk_frame` in the same iteration -- so the consumer
       reads it after its owner has released it, and `tagpu_packet.poison` fires
       at that give-back, which means the lever built to catch exactly this
       cannot see it. Safe today only by the rotation, which is not a bound.
       It is why `eng`, `mmPic`, the atlas and the palette are all copies
       already. 47 KB a frame, and only when an MM quad was recorded.
       THE DIMENSIONS COME FROM THE SAME READ as the bytes, so the two cannot
       disagree -- `s_mmEngW/H` are the GL texture's and persist across a frame
       that produced no pair at all. [FOUND 2026-09-16, the landing-3 review.] */
    s_mHand.mmEng = NULL; s_mHand.mmEngW = 0; s_mHand.mmEngH = 0;
    if (s_mNSDraw > 0 && s_mNSDraw <= TAGPU_GUI_SDRAW_MAX && f->packet) {
        const unsigned char* eng = tagpu_pk_minimap(f->packet);
        int ew = f->packet->mm_w, eh = f->packet->mm_h;
        int want = 0, k;
        for (k = 0; k < s_mNSDraw; k++)
            if (s_mSDraw[k].kind == TAGPU_GUISK_MM) { want = 1; break; }
        if (want && eng && ew > 0 && eh > 0) {
            unsigned off = 0;
            if (mir_bytes(eng, (unsigned)ew * (unsigned)eh * 3u, &off)) {
                /* AND THE ARENA IS RE-PUBLISHED, because this is the only
                   `mir_bytes` in `mir_finish` and it can REALLOC: `arena` and
                   `alen` were taken further up, so a grow here left the ops'
                   own base pointer stale and every `aoff` in the record
                   pointing into freed heap. Latent until this copy existed. */
                s_mHand.arena = s_mArena; s_mHand.alen = s_mALen;
                s_mHand.mmEng = s_mArena + off;
                s_mHand.mmEngW = ew; s_mHand.mmEngH = eh;
            }
        }
    }

    s_mHand.glyphs = tagpu_text_glyph_atlas(&s_mHand.glyphW, &s_mHand.glyphH);
    s_mHand.glyphSerial = tagpu_text_glyph_serial();

    s_mHand.pal = tagpu_pal_live();
    s_mHand.palSerial = tagpu_pal_serial();

    /* THE ENGINE'S OWN FRAME. The composite's bottom layer and its
       stale-mirror guard both sample it, as `f->surface_tex` -- a GL texture,
       which is exactly what a second backend cannot have. The bytes behind it
       are the fork's primary, and they are read HERE under `g_ddraw.cs` for
       the lifetime reason tagpu_pal.c gives for the palette: the game thread
       NULLs `g_ddraw.primary` inside that section and frees the object only
       after leaving it, so a pointer read AND dereferenced inside it is a live
       object or NULL, never a freed one. Copied rather than aliased, because
       the Vulkan lane runs two calls later. */
    s_mHand.eng = NULL; s_mHand.engW = s_mHand.engH = s_mHand.engPitch = 0;
    if (have_engine_frame(f)) {
        /* TAKEN BY tagpu_surf.c, NOT COPIED AGAIN HERE. This block used to do
           its own critical section and its own row loop; the snapshot moved out
           when the Vulkan lane needed the same bytes as its BOTTOM layer,
           because a base layer that exists only while the UI pass runs is the
           coupling landing 4c-1 exists to undo. The lifetime is still a copy
           rather than an alias of anything short-lived: the buffer is
           tagpu_surf.c's, written once per frame at the top of
           `tagpu_overlay_draw`, so it is stable for the whole window in which
           this hand-over is valid -- which is a stronger statement than the
           per-frame realloc it replaces, not a weaker one, because there is now
           exactly one writer at exactly one point in the frame.
           [The vulkan-only plan, landing 4c-1.] */
        TAGPU_SURFFRAME sf;
        if (tagpu_surf_frame(&sf)) {
            s_mHand.eng = sf.bytes;
            s_mHand.engW = sf.w; s_mHand.engH = sf.h; s_mHand.engPitch = sf.w;
        }
    }
    /* the guard has nothing to compare against without those bytes */
    if (!s_mHand.eng) { s_mHand.guard = 0; s_mHand.strict = 0; }
    /* AND SINCE LANDING 10 THAT IS ALL `eng` IS: A FLAG. The Vulkan lane used
       to memcpy these bytes into its own R8 image every frame -- a second copy
       of what `tagpu_vk_surf.c` had already uploaded from the same
       `tagpu_surf_frame`, 786 432 of them at 1024x768 -- and now borrows that
       image instead (`tagpu_vk_surf_engine_view`). Nothing downstream
       dereferences this pointer any more, so the lifetime paragraph above is
       about a read that no longer happens; it is kept because the pointer is
       still what says whether a frame exists at all, which is what the two
       lines above turn into `guard` and `strict`. */

    s_mFrame = f->frame_counter;
    s_mHave = 1; s_mTaken = 0;
}

void tagpu_gui_mirror_reseed(void)
{
    /* the same flag, the same reason enum: a consumer that has fallen behind
       is exactly what TAGPU_GUI_WHY_STALL names, and the producer's handling
       of it (a RESET and every surface seeded again) is what both stores need */
    g_guiq.why = TAGPU_GUI_WHY_STALL;
    g_guiq.reseed = 1;
}

void tagpu_gui_mirror_want(int on)
{
    if (!on && s_mirWant) {
        /* nothing is kept once nothing asks for it: this is the largest thing
           the module owns after the twins themselves */
        free(s_mOps);   s_mOps = NULL;   s_mCapOps = 0;
        free(s_mArena); s_mArena = NULL; s_mACap = 0;
        s_mNOps = s_mALen = 0; s_mHave = 0;
        /* THE ATLAS MIRROR IS NOT FREED HERE, and that is the atlas's rule
           rather than an oversight (tagpu_gaf.h): it is owned by a static atlas
           with no destructor and costs a re-decode of every frame on screen to
           re-arm. A lane that disarms and re-arms -- which is every `gui.on`
           edit -- finds it already correct. (There were TWO until 11-5e-2b
           part 2: the restored one would have been read back from scratch, and
           there is no read-back any more.) */
    }
    s_mirWant = on ? 1 : 0;
}

int tagpu_gui_handover(TAGPU_GUIHAND* out, unsigned now)
{
    if (!out || !s_mHave || s_mTaken) return 0;
    /* THE FRAME, and it is the safety refusal rather than a tidiness one: every
       pointer in the struct aliases a buffer this module reallocs on the next
       present, so a hand-over that outlived its frame can name memory that has
       moved. Same rule, same reason, as tagpu_feat.h's. */
    if (s_mFrame != now) return 0;
    *out = s_mHand;
    s_mTaken = 1;
    return 1;
}

/* NOTHING CALLS THIS SINCE 11-5e-1 -- its one call site was tagpu_overlay.c's
   GL-context-change watch, which could never fire. WORTH A SECOND LOOK WHEN
   THE GL OBJECTS GO, because not everything below is GL teardown: the reseed
   request, `TAGPU_GUI_WHY_GLCTX` and `s_skipToReset` are "forget everything
   derived from this device", and with `tagpu_native_glreset`'s fog-grid drop
   they are the only such path in the build -- with nothing on the Vulkan side
   wired to them. [The 11-5e-1 review's observation, for 11-5e-2.] */
void tagpu_gui_glreset(void)
{
    /* the context is gone: forget every id, start over from fresh seeds —
       and take nothing from the queue until the producer's RESET arrives */
    /* EIGHTEEN GL OBJECT NAMES WERE ZEROED HERE and went with the draw half
       [landing 11-4b]: the six programs, the VAO/VBO, the palette texture, the
       sharp layer's texture and FBO, the minimap's pair, `s_gl`, `s_palUpValid`
       and `s_sharpFailed`. Nothing creates any of them, so every one was a
       write-only static -- invisible to `-Wall`, which does not warn for a
       file-scope static that is only assigned. What is left is the state that
       genuinely dies with a context: the twin table, the sharp layer's SIZE
       (its space is the viewport, which a new context re-establishes), the
       cursor's ownership and the minimap's generation. */
    s_ntwins = 0; s_presented = 0;
    s_sharpW = s_sharpH = 0; s_sharpOn = 0;            /* the sharp layer died with it */
    s_curOwn = 0; s_curFrame = NULL;   /* and the cursor is nobody's until it is re-atlased */
    s_mmGenSeen = 0;                    /* the picture's texture died; the BYTES are the hook's */
    /* the text module used to be forwarded to here, for the glyph atlas's
       texture id. It owns no GPU object since 11-5e-1 -- its cells and its
       atlas bytes are CPU-side and a context change never touched them, so
       there was never anything for it to lose but the id. */
    tagpu_gaf_atlas_lost(&s_atlas);
    s_skipToReset = 1;
    g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_GLCTX;
}

int tagpu_gui_drawing(void) { return s_on; }
