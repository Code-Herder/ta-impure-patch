/* tagpu_gui_surf.c — the twins, the UI atlas, the replay and the layer draw
   (Phase E). Contract: inc/tagpu_gui.h; queue: tagpu_gui_int.h.
   Design: research/notes/gui-renderer.md 3.2-3.6.

   RENDER THREAD ONLY. Every engine surface the publisher has seeded gets a
   twin. HERE a twin is a table entry (`TWIN`); its image is tagpu_vk_gui.c's,
   an R8G8 image the surface's size (R = the palette index, G = the coverage:
   1 where a replayed op wrote, 0 where none has or a CLEAR erased). The
   queue's ops are drained at every present, in order, and each one this file
   applies is recorded as a `TAGPU_GUIOP` in the frame's hand-over, which
   tagpu_vk_gui.c replays onto its images: SEED makes the twin empty (the
   engine's bytes do not cross), ASSET seeds it with the loader's bytes,
   SPRITE draws a quad from the UI atlas with the colour key discarded, STRING
   stamps TA's glyphs, COPY draws a quad sampling another twin, BAR/RECT/TINT
   fill, outline and remap a box, CLEAR clears a box to coverage 0, FREE drops
   the twin, RESET drops them all; PIXELS is dropped and counted. Then the
   presented surface's twin is
   drawn over the frame, palette-resolved through the live palette, wherever
   its coverage is set -- above the world passes, and discarded everywhere
   else, so the world shows through; nothing of the engine's frame is beneath
   it (LAY_FS).

   CLASSIC++. Beside the index twin every surface may carry a COLOUR
   twin -- VK_FORMAT_R8G8B8A8_UNORM, the same size, attachment 1 of the same
   framebuffer (tagpu_vk_gui.c `fb2`) -- into which a sprite op writes the UI
   atlas's RESTORED texel where the atlas has one (alpha 1) and zero elsewhere. A copy carries both channels, which is
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

   THE SEAM (gui-renderer.md 13.2-13.3). The twin stays 1:1 with the engine's
   surface — same ops, same seeds, same census. The composite is two layers,
   top down: the SHARP LAYER (one RGBA8 texture at the DEVICE resolution,
   drawn from live state at present time — its clients are the cursor and the
   minimap), then the mirror scaled by a SHARP-BILINEAR ramp one device pixel
   wide. Nothing of the engine's own frame is beneath them. k is device pixels
   per twin texel, read off the frame rather than configured, and it is 1.0 wherever the engine's screen IS the
   window. It is NOT always 1: `resizable` defaults TRUE (config.c) and
   `maintas` fits the viewport to the window (dd.c), so a player who drags the
   window off the game resolution is
   already at a fractional k and already gets this ramp. At k = 1 the ramp is
   exactly one source texel wide, so every output pixel samples a texel centre
   and the frame is what texelFetch gave, to within a rounding step far too
   small to cross an 8-bit value: that identity is the gate, not a hope. */

#include <windows.h>
#include "tagpu_gui_reset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_gui.h"
#include "tagpu_opt.h"
#include "tagpu_gui_int.h"
#include "tagpu_gaf.h"
#include "tagpu_text.h"                 /* the glyph atlas the string op stamps from (13.4) */
#include "tagpu_classicpp.h"
#include "tagpu_hud.h"
#include "tagpu_terr.h"
#include "tagpu_terrown.h"           /* is the world viewport carrying our key fill right now? */
#include "tagpu_pal.h"
#include "tagpu_surf.h"                    /* the one resolution of the presented palette */
#include "tagpu_vk.h"                 /* tagpu_vk_ab_arm: the A/B's arming */
#include "tagpu_log.h"
#include "dd.h"                         /* g_ddraw.cursor: the pointer the fork last saw (13.5) */
#include "mouse.h"                      /* mouse_last_client: the pointer at the DEVICE's resolution (13.5) */

/* The minimap's box, its three surfaces and the view box over them are packet
   fields; the addresses live in inc/tagpu_engine.h with the
   publisher. `+0x142F1 bit 1` is NOT among them and never was: it is
   DrawMinimap 0x466B00's DIRTY flag, which 0x466B16 clears in the same breath,
   so it reads 0 on almost every frame. */
#define POLL_MS        500
#define MAX_TWINS      32
#define TINT_LOST_MAX  32       /* dropped PK_PIXELS boxes remembered per twin */
#define ATLAS_DIM      2048
#define ATLAS_MAX      4096
#define UI_RESTORE_MIN 12       /* nothing under 12x12 is restored */
#define PAL_SETTLE     30       /* frames the palette must hold still before a re-arm */

extern volatile int g_gui_draw;         /* tagpu_gui_hook.c: the publisher's gate */


static void slog(const char* s)
{
    tagpu_log(s);
}

/* ------------------------------------------------------------------ state */
/* THE BOOKKEEPING the mirror op stream needs: which engine surface this twin
   stands for and how big it is, so `twin_find` can resolve a surface pointer
   to a slot and the ops can name it. */
typedef struct TWIN {
    unsigned surf;                      /* the engine surface's pixel base     */
    int w, h;
    /* THIS SURFACE HAS A COLOUR PLANE. Not an object name but the RECORD of a
       decision -- the op
       that first carried `TAGPU_GUICOL_DST` is what made the consumer's
       colour image, so the two stores hold colour for the same surfaces by
       construction rather than by two modules agreeing. Never cleared once
       set (a `PK_PIXELS` box invalidates the colour WITHIN the box, which is
       the consumer's `tw_col_drop`, not the plane's existence); it goes with
       the twin, and `s_colTwins` is the live count. */
    int col;
    /* THE BOXES THIS PASS DROPPED A `PK_PIXELS` FOR. Only the tint reads them,
       and only because the tint is the one op whose result depends on what
       the twin already holds. Cleared at the top of every `drain`, so they
       mean "in this batch", never "ever".

       PER PIXEL AND NOT PER SURFACE, MEASURED: SKIRMISH.GUI's side buttons,
       drawn through the ALP blit `0x4B8500` on every frame, fall to
       `PK_PIXELS` on the SAME surface as the focus rings and the modal dim, so
       a surface-wide flag declines 2.4 M tints for drops nowhere near them.
       What makes a tint unsafe is that the pixels it is about to READ were not
       repainted, so `tint_pieces` takes exactly those out of its box.

       Past `TINT_LOST_MAX` the extras are merged into the last slot. A union
       is a SUPERSET of the boxes it replaces, so subtracting it removes at
       least every dropped pixel: the overflow costs precision and cannot cost
       soundness, and the array needs no second state. */
    int nLost;
    int lost[TINT_LOST_MAX][4];
} TWIN;
static TWIN   s_twins[MAX_TWINS];
static int    s_ntwins = 0;
static unsigned s_presented = 0;        /* the last PK_FRAME's surface         */

static TAGPU_GAFATLAS s_atlas;
static TAGPU_GAFENT   s_ents[ATLAS_MAX];

static int    s_on = 0;
static DWORD  s_lastPoll = 0;
static unsigned s_drained = 0, s_sprites = 0, s_copies = 0, s_pixels = 0, s_seeds = 0, s_clears = 0, s_lostSprites = 0;
/* PK_PIXELS OPS THE DRAIN REFUSED TO CARRY -- the engine's own composed
   bytes, which may not reach the screen. `pixels=` beside it is pinned at 0
   and kept deliberately: two counters rather than one are how a reader
   sees that the traffic did not stop, the CARRYING did. */
static unsigned s_pixDropped = 0;
static unsigned s_assets = 0;          /* PK_ASSET ops carried into a twin     */
static unsigned s_movies = 0;          /* PK_MOVIE frames carried into a twin  */
static unsigned s_planes = 0;          /* PK_PLANE stamps carried into a twin  */
/* TINTS THE DRAIN DECLINED, BY REASON. `tintdrop`
   is no twin or no table -- the comment on the case below says that cannot
   happen, so a non-zero here is that comment being wrong and the whole point
   of spending four bytes on it. `tintstale` is the one that is EXPECTED to be
   non-zero on a screen whose paint falls back to `PK_PIXELS`: the tint was
   declined because the pixels under it were, which is the compounding guard
   and not a fault. Two counters and not one, because "the table never came"
   and "the paint under it went" are opposite bugs. `tintsplit` is the third:
   tints drawn over only the part of their box that was repainted
   (`tint_pieces`), so a non-zero `tintstale` now means a box wholly dropped
   or past the piece cap, never one sprite somewhere under it. */
static unsigned s_tintDrop = 0, s_tintStale = 0, s_tintSplit = 0;
/* SOLID RECTANGLES REPLAYED AS GEOMETRY, counted beside `pixels=` so the two
   can be read against each other: every one of these would otherwise be a
   `PK_PIXELS` box of arena bytes. */
static unsigned s_bars = 0;
/* HOLLOW RECTANGLES replayed as geometry, beside `bars=` for the same reason. */
static unsigned s_rects = 0;
/* FOCUS-TINT EDGES replayed as an operation, beside the other two. The third
   kind that would otherwise be `PK_PIXELS` on the shell's menus, and the
   largest: 1 523 px of a 1 613 px difference at 640x480, MAINMENU. */
static unsigned s_tints = 0;
/* THE HAND-OVER'S REMAP TABLE as `PK_SHADE` delivered it: the engine's
   lighten table `globals+0xC8` and the box shader's rows (`TAGPU_GUI_SHADE_ROWS`,
   layout in inc/tagpu_gui.h). A FILE-STATIC AND NOT A POINTER INTO THE QUEUE, for the reason the mirror
   exists at all: `drain` advances the arena tail per op, so the bytes are the
   game thread's again the moment this function returns, and the Vulkan lane
   does not run until two calls later. The hand-over carries THIS, by pointer,
   every frame -- so a mirror frame the consumer abandons cannot lose the
   table, which is what lets `PK_SHADE` need no acknowledgement of any kind.
   `s_shadeSerial` moves when the bytes do, and is the port's upload trigger. */
static unsigned char s_shade[TAGPU_GUI_SHADE_BYTES];
static int      s_shadeHave = 0;
static unsigned s_shadeSerial = 0;
/* Classic++ */
static int    s_norestore = 0;          /* `norestore` in the trigger: the A/B lever   */
/* WHETHER AN OP MAY SAY "RESTORED" THIS FRAME (gui-renderer.md 3.4). Written
   by `restore_step` below, which is the present's first statement again. Three
   things have to hold and each one is a fact rather than a hope:
     - Classic++ `assets=` is on, so there is a restorer running at all;
     - the UI atlas has armed its published frame list;
     - the CONSUMER says it has a restored image with at least one painted
       frame in it (`tagpu_gui_col_ready`), and the presented palette is still
       the one that image was painted against.
   The third is what keeps this from being a race with the lane that paints:
   an op that says restored to a consumer holding no restored atlas is drawn
   indexed there (`tagpu_vk_gui.c`'s replay), so saying it before the consumer
   has an atlas would fill colour twins with indexed art for the two seconds a
   first restore takes. It cannot say restored before the consumer has said it
   can. */
static int    s_colValid = 0;
/* THE CONSUMER'S ANSWER, one frame old by construction: `tagpu_vk_gui.c` calls
   `tagpu_gui_col_ready` from its own prepare, which runs LATER in the same
   iteration of the render loop than this module's present. Both are the render
   thread, so this is a plain static and not a handshake. One frame of lag in
   BOTH directions: on the way ON it costs nothing, and on the way OFF -- a
   restore job that blanks the consumer's atlas -- the consumer draws that
   frame's restored sprites indexed (its replay runs after the job starts,
   tagpu_vk_gui.c), then this answer drops and its return asks for the
   repaint. */
static int    s_colReady = 0;
/* ...AND HOW OFTEN THE RESTORE HAS GONE QUIET HAVING PAINTED SOMETHING NEW.
   Each one is a repaint's worth of art that was drawn too early to take the
   colour it should have. `s_colRepaints` is the BOUND on how many repaints one
   palette generation may ask for: the set of atlas entries a screen uses is
   finite so the loop converges by itself, and this is the guard for the day it
   does not rather than a rate limit -- a repaint that introduces a new entry is
   allowed, 32 in a row is a bug and says so once. */
static unsigned s_colSettled = 0, s_colSettledSeen = 0, s_colRepaints = 0;
/* the palette generation `s_colRepaints` was granted for: the budget below is
   per GENERATION, not per validity edge (see col_ask_repaint) */
static unsigned s_colRepaintsGen = (unsigned)-1;
/* THE PICTURE STORE'S HALF (tagpu_vk_gui.c): a decoded asset and a
   transformed stamp cross as bytes rather than as atlas sprites, and the
   consumer restores them in a store of its own.
   `s_picArm` is Classic++'s assets being armed at all -- NOT `s_colValid`,
   because an asset is seeded ONCE, at the flip that first copies it, and that
   is usually before the UI atlas has restored anything: gated on validity,
   the main menu's backdrop would never be restored. A colour plane on a twin
   while colour is not valid is harmless -- the composite reads it only under
   `s_colValid` (`colourTwins`).
   `s_picSettled` is the consumer's settle count, and each new one asks the
   engine for a repaint (`tagpu_gui_pic_settled`). */
static int      s_picArm = 0;
static unsigned s_picSettled = 0, s_picSettledSeen = 0;
#define COL_REPAINT_MAX 32
/* the picture store's own repaint budget, per palette generation as the
   atlas's is (see pic_ask_repaint) */
#define PIC_REPAINT_MAX 1024
static unsigned s_picRepaints = 0, s_picRepaintsGen = (unsigned)-1;
/* THE PALETTE THE RESTORED ART IS RIGHT FOR, and the settle counter of 3.4.
   `s_colPalSeen` distinguishes "never armed" from "armed against serial 0". */
static unsigned s_colPalSerial = 0, s_colPalLast = 0;
static int      s_colPalSeen = 0, s_colPalStill = 0;
static unsigned s_rearms = 0, s_colTwins = 0;
/* OPS THAT ACTUALLY CARRIED `TAGPU_GUICOL_ON` since the last heartbeat, and it
   is the field that says whether restored art is reaching the screen rather
   than merely being painted. `colvalid=1` says this side would say yes;
   `colops=0` beside it says nothing ASKED -- which is the difference between
   the restorer being off and the ops that would sample it never running. */
static unsigned s_colOps = 0;
/* Phase 2's seam */
static int    s_sharpW, s_sharpH;       /* its size, = the frame's viewport in window px               */
static int    s_sharpOn = 0;            /* it exists and may be sampled this frame                     */
/* ...AND WHETHER ANYTHING IS ACTUALLY IN IT. `s_sharpOn` says the layer
   exists this frame, which it does on every frame that reaches `sharp_begin`'s
   tail -- so it is the wrong question for a consumer that needs the layer's
   CONTENT. An EMPTY layer and a DISABLED one composite identically (it is
   taken only where its alpha says it has coverage, and a cleared one has none
   anywhere); this is what the hand-over's `sharpOn` carries, and
   tagpu_vk_gui.c refuses a frame that says coverage and brings no quad for
   it. */
static int    s_sharpInk = 0;
static int    s_sharptest = 0;          /* the harness lever that proves the layer is wired            */
/* `reseedstress=N`: the harness lever that asks for a fresh start every N
   presents, through the very call the consumer uses when it falls behind
   (`tagpu_gui_mirror_reseed`, this thread). A fresh start re-lays the UI atlas,
   which starts a blanking restore while the frame in flight still says colour,
   and re-creates every twin without colour while colour is valid: the two
   disagreements that, answered with another fresh start, looped until the
   consumer composited nothing (tagpu_vk_gui.c `behind_ex`). Under it the
   consumer must ask for NONE of its own -- tools/compat's `gui-stress` setup is
   the check. Never a player's. */
static unsigned s_stress = 0, s_stressN = 0, s_stressFired = 0;
static unsigned s_strings = 0;          /* string ops stamped                                          */
static unsigned s_glyphs = 0;           /* glyph quads drawn                                           */
static unsigned s_strMiss = 0;          /* glyphs the cache would not give (the engine drew them)      */
static unsigned s_strReseed = 0;        /* strings that stamped NOTHING and asked for a fresh seed     */
static unsigned s_strRepack = 0;        /* gathers restarted because the glyph atlas repacked under them */
/* the TNT's own 252-px minimap picture, uploaded once per map load */
static unsigned s_mmGenSeen;            /* the generation s_mmPicRgb holds; 0 = nothing     */
/* THE BAKE HAPPENED: the resolve is in `s_mmPicRgb`, which `mir_finish` hands
   over as `mmPic` for tagpu_vk_gui.c to upload. Never cleared once set; the
   generation and palette keys beside it are what invalidate a bake. */
static int      s_mmBaked;
static int      s_mmTW, s_mmTH;         /* its size in texels                               */
static int      s_mmbase = 0;           /* the minimap is ours (see the k rule in sharp_minimap) */
static int      s_mmforce = 0;          /* token `mmbase`: draw it at k = 1 too, for the harness */
static unsigned s_mmDrawn = 0;
static unsigned s_mmLogged;
static unsigned s_mmNoEng;              /* frames the engine's pair could not be read          */
static unsigned char* s_mmPicRgb;       /* the picture resolved through the presented palette  */
static unsigned s_mmPicCap;
/* The CONTENT serial of `s_mmPicRgb`, for the same reason the glyph atlas has
   one -- the consumer must know when the bytes behind it moved. It moves on a
   map load and on a palette change, which are exactly the two things that
   re-resolve it. */
static unsigned s_mmPicSerial;
static unsigned s_mmPalSeen;            /* tagpu_pal_serial() when it was last resolved        */
static unsigned s_mmFogged;             /* engine texels where fogged != unfogged, this frame  */

/* THE CURSOR (gui-renderer.md 13.5). Decided ONCE per frame, in
   tagpu_gui_cursor_frame, from that frame's packet; sharp_cursor draws from
   the decision. Everything here is render-thread state. */
static int    s_nocursor = 0;           /* token: keep phase 1's engine cursor          */
static float  s_cursorScale = 1.0f;     /* token cursorscale=, device px per frame px   */
static const unsigned char* s_curFrame; /* the GAF frame the engine is blitting          */
static int    s_curW, s_curH;           /* its size, frame px                            */
static int    s_curHX, s_curHY;         /* its hotspot, frame px, may be negative        */
static unsigned char s_curCK;           /* its colour key                                */
static int    s_curOwn = 0;             /* ours is drawn this frame                      */
static unsigned s_curDrawn = 0;         /* frames ours was drawn                         */
static unsigned s_curWarm = 0;          /* frames spent atlasing a shape we had not seen */
static int    s_curDev = 0;             /* the last draw used the true client point      */
static float  s_k = 1.0f;               /* device px per twin texel: 13.1's k, and the ramp's width    */
static float  s_hudS = 1.0f;            /* HUD scale in force this frame (20); 1.0 is the feature off */

/* ----------------------------------------------------------------- shaders */
/* a quad in surface pixels -> the twin's framebuffer (row 0 = surface row 0) */
/* THESE TEN ARE A BUILD INPUT, NOT CODE THIS FILE RUNS. Nothing here
   references them -- but `tools/spirv-gen.py` reads them out of the
   PREPROCESSED translation unit and generates `inc/spirv/tagpu_gui_surf.spv.h`,
   which `tagpu_vk_gui.c` includes and draws with. This file holds more of them
   than any other: all eight `gui_*` programs of the PROGRAMS table are built
   from this pair-set (ten strings, eight programs). Deleting them fails the
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
/* THE STRING OP (13.4): TA's own glyphs out of the coverage atlas,
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
   loses its colour. */
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
/* THE MINIMAP (13.6): our 252-px base, MASKED BY THE ENGINE'S OWN FOG.

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
   those landed, so one comparison carries all three. So there is no dot
   replay: a replay can be pixel-exact against the engine, but it can only
   ever carry the DOTS — the arcs (0x4C0070) and the points (0x4BEE60) are not
   observed leaves (§7) and each would need its own rasteriser reproduced
   exactly. One mechanism that carries all three, from the engine's own
   pixels, beats two that do not.

   THE FOG TEST IS OVER A 3x3 NEIGHBOURHOOD, not one texel, and that is the
   whole safety argument. A shaded pixel can land on the same palette index it started
   from (the shade is a LUT into a dark-grey ramp, so a pixel already in that
   ramp maps to itself), and a single-texel test would then let four of OUR
   sub-texels through — sub-texels taken from the unfogged picture, which may be
   bright. Requiring the whole neighbourhood to agree costs a one-texel band of
   the engine's own resolution around every fog edge.

   WHAT THAT BUYS, EXACTLY: no unit, arc or point
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
/* THE FOCUS TINT: the destination's own index, remapped through
   one row of the engine's lighten table.

   `uSrc` IS A SNAPSHOT OF THE TWIN, NOT THE TWIN. `0x4CC8DF` does
   `dst = LUT[row*256 + dst]`, a read-modify-write of the very texels this
   draw writes, and sampling an attachment a draw is writing is undefined in
   Vulkan -- the same rule that makes a self-`PK_COPY` a refusal one file
   over. So the caller copies the box out first and this samples the copy, at
   the SAME COORDINATES, which is why there is no offset uniform: `gl_FragCoord`
   indexes both.

   COVERAGE IS CARRIED THROUGH UNCHANGED, and that is the difference between a
   tint and every other op here. A bar covers, a clear uncovers, a sprite
   covers what it does not key out; a tint says nothing about coverage at all,
   because the engine has no such concept and this op only remaps a byte. So
   green comes from the snapshot rather than from a constant.

   THE RESTORED COLOUR IS SHADED BY BLENDING, NOT READ. The table's lower
   half holds, beside each entry's remap, that entry's map of colour -- rgb an
   offset, a a scale (tagpu_vk_gui.c `tint_table`) -- and the Classic++ pipeline
   blends `src + dst * srcAlpha` with the destination's alpha kept. So a texel
   with restored colour becomes that colour shaded as the engine shaded its
   index, and one without stays without. `oIdx.a` is 0, which is what makes
   the same blend a plain write of the index. Reading the colour plane instead
   would need a snapshot of it as well, for the reason above.

   THE ROW IS AN INDEX INTO THE `TAGPU_GUI_SHADE_ROWS`-ROW TABLE AND IS BOUNDED
   BEFORE THE DRAW (`before_focus` and `before_frame` produce only rows inside
   it, and the Vulkan pass refuses a row past it), so there is no clamp here --
   a clamp would turn a producer bug into a wrong picture instead of a loud
   one. */
static const char* TINT_FS =
    "#version 330 core\n"
    "layout(location=0) out vec4 oIdx;\n"
    "layout(location=1) out vec4 oCol;\n"
    "uniform sampler2D uSrc; uniform sampler2D uShade;\n"
    "uniform int uRow;\n"
    "void main(){ ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec2 g = texelFetch(uSrc, p, 0).rg;\n"
    "  int i = int(g.r * 255.0 + 0.5);\n"
    "  oIdx = vec4(texelFetch(uShade, ivec2(i, uRow), 0).r, g.g, 0.0, 0.0);\n"
    "  oCol = texelFetch(uShade, ivec2(i, uRow + textureSize(uShade, 0).y / 2), 0); }\n";
/* the layer over the frame: uv.y = 0 at the top of the screen; the twin's
   row 0 is the surface's row 0 */
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
   one draws upside down (MEASURED 2026-09-09 with `sharptest`). */
static const char* SHARP_FS =
    "#version 330 core\n"
    "out vec4 frag; uniform vec4 uCol;\n"
    "void main(){ frag = uCol; }\n";
/* THE CURSOR IN THE SHARP LAYER (13.5). The same UI atlas the sprite
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
    /* NO SAMPLER OF THE ENGINE'S OWN FRAME IS DECLARED HERE: nothing of the
       engine's reaches the screen, and a pipeline that cannot name the image
       cannot sample it. */
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
       tagpu_vk_gui.c's upload of them (the SEED/PIXELS case) stamps coverage
       255 on every one, key included -- so
       any engine drawer still running inside the world viewport publishes a
       rectangle of tagpu_terrown's fill, and without this rule the layer
       resolves index 254 through the palette and paints BRIGHT CYAN, opaque,
       over the world composite. [MEASURED 2026-09-09: the engine's selection
       rect is drawn as four DrawLine 0x4BE950 calls, each recorded as its own
       axis-aligned BOUNDING BOX, so one rotated rect published four boxes whose
       union is a ~44 px cyan square with a hole -- 15 frames in 3600 of
       ordinary play, and 12 of 12 with the engine's rects forced on.]
       The key fill must never reach the screen, from this layer or any
       other. Coverage 0 is exactly the right answer: it is what
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
       map below moves only where the MIRROR IS SAMPLED. */
    "  vec2 d = uv * vec2(uSize);\n"
    /* THE HUD'S REGION MAP RUNS FIRST, and `p`/`f` below are the SOURCE texel
       it chose, not the dest fragment. Everything downstream of here indexes
       the engine's own surface -- `uVp` is the engine's viewport rect and
       uTwin is the twin -- so both have to be asked about the texel the
       colour actually came from. At s == 1 `sd` IS `d` and this is the
       identity, which is why the s = 1 gate is untouched by it. */
    "  vec2 sd = d; float ramp = 1.0;\n"
    "  if (uHud.w > 1.0) {\n"
    "    float H = float(uSize.y);\n"
    "    if (d.x < uHud.x || d.y < uHud.y) { sd = d * uHud.z; ramp = uHud.w; }\n"
    "    else if (d.y >= H - uHud.y) {\n"
    "      sd = vec2(d.x * uHud.z, H - (H - d.y) * uHud.z); ramp = uHud.w; }\n"
    /* THE WORLD IS TRANSLATED, NOT MAGNIFIED (22.6). The engine's viewport is
       the visible window itself -- it draws the world into [128, R] x
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
       IT IS TESTED FIRST, ahead of anything that could discard: the one place
       a cursor is ever drawn must never be a place the shader has already
       given up on. */
    "  if (uSharpOn != 0) {\n"
    "    ivec2 sp = clamp(ivec2(uv * vec2(uSharpSize)), ivec2(0), uSharpSize - 1);\n"
    "    vec4 sh = texelFetch(uSharp, sp, 0);\n"
    "    if (sh.a > 0.5) { frag = vec4(sh.rgb, 1.0); return; }\n"
    "  }\n"
    /* THE CURSOR'S RECT IS NOT DISCARDED HERE, and the difference is a hole
       rather than a nicety: nothing of the engine's frame is underneath, so
       discarding here would punch the twin's own background out and leave the
       lane's clear colour in a cursor-shaped rect. So the twin always draws,
       and the cursor is the sharp layer's above it, which is the only cursor
       there is. */
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
    /* NO STALE-MIRROR GUARD: the engine's composed frame is not underneath
       us, so there is nothing here to compare the twin against. A twin that
       is never seeded starts EMPTY, coverage 0, so a region the publisher
       never observed is a region we simply do not paint -- the
       hole is closed at the source rather than tested for. The golden source
       is still reachable (`tagpu_vk_surf_engine_view`) for a comparison that
       wants one; it is not this shader's business. */
    "  if (c.a > 0.5) { frag = vec4(c.rgb / c.a, 1.0); return; }\n"
    "  discard; }\n";
#pragma GCC diagnostic pop

/* THE UI ATLAS IS A CPU TABLE, NOT AN IMAGE. It is what every sprite op
   resolves against -- with no atlas every sprite is `lost` and the surface
   re-seeds (measured: sprites=0, atlas=0/0, lost=816878) -- and
   `tagpu_gaf_atlas_create` keys on `made`, not on any image existing; the
   texels reach tagpu_vk_gui.c as the atlas's CPU mirror. Idempotent: the `made` test comes before the
   memset that would clear it, and the memset's other casualty -- the one heap
   buffer `free_buffers` owns -- is handed back rather than dropped; see that
   function's own comment for the one it does NOT own. */
static int atlas_setup(void)
{
    if (s_atlas.made) return 1;
    /* the memset drops `mirror` and `rlist`, which `_lost` keeps -- so this
       hands back every heap buffer first, and clears `rlistWant`/`rlistFailed`
       with them, or the memset leaks them. The GUI
       atlas never arms a list, so `rlist` is NULL on this path either way, and
       this is the only call site -- `tagpu_gaf_atlas_restore_vk` has three
       callers and none is in this file. `tagpu_gaf_atlas_free_buffers` and
       `rlist_add` carry the argument and name the residual. */
    tagpu_gaf_atlas_free_buffers(&s_atlas);
    memset(&s_atlas, 0, sizeof s_atlas);
    s_atlas.ents = s_ents; s_atlas.max = ATLAS_MAX; s_atlas.dim = ATLAS_DIM; s_atlas.tag = "gui";
    s_atlas.pad = 0; s_atlas.align = 0; s_atlas.mip = 0;      /* 1:1, NEAREST, the 1-texel border */
    s_atlas.restoreMinEdge = UI_RESTORE_MIN;
    return tagpu_gaf_atlas_create(&s_atlas) ? 1 : 0;
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
    /* THE RECORD is the whole function: the twin's slot is recycled so
       `twin_find` stops resolving this surface, which is what the mirror's FREE
       op pairs with. */
    if (t->col && s_colTwins) s_colTwins--;
    *t = s_twins[--s_ntwins];
}

/* The colour twin is tagpu_vk_gui.c's, made by the first op that carries
   `TAGPU_GUICOL_DST`: RGBA8 at attachment 1 of the twin's framebuffer, so one
   draw writes the index and the colour together and they can never disagree
   about what a texel holds. Cleared to alpha 0 — nothing is restored until an
   op says so. */
static TWIN* twin_make(unsigned surf, int w, int h)
{
    TWIN* t = twin_find(surf);
    if (t && (t->w != w || t->h != h)) { twin_drop(t); t = NULL; }
    if (t) return t;
    if (s_ntwins >= MAX_TWINS) twin_drop(&s_twins[0]);        /* the oldest goes */
    t = &s_twins[s_ntwins++];
    memset(t, 0, sizeof *t);
    t->surf = surf; t->w = w; t->h = h;
    /* THE ENTRY IS THE TABLE, AND THE IMAGES ARE THE CONSUMER'S. A twin
       exists when the table says so -- every `if (t)` that gates a `mir_op`
       asks this -- and tagpu_vk_gui.c makes its images from the SEED this
       entry's op carries. */
    return t;
}

/* The return is this op's TAGPU_GUIOP::col -- what this function decided
   about colour, for the mirror to carry rather than decide again: `DST` asks
   the consumer for a colour plane, `ON` says the texels are restored, and the
   two are recorded as separate facts. */
static unsigned char twin_sprite(TWIN* t, const TAGPU_GAFENT* e, const TAGPU_PUBOP* o)
{
    /* THE DECISION IS ONE FLAG. `s_colValid` carries all three terms (see its
       declaration), so there is one input and it moves.

       `e` IS NOT TESTED HERE AND THE FLOOR IS NOT LOST. An entry below
       `UI_RESTORE_MIN` is simply never put in the published list, so its texels
       in the restored image stay alpha 0 and `SPR_FS` takes the palette for
       them -- per texel, which is stricter than per sprite and needs no second
       copy of the rule. */
    unsigned char col = 0;
    (void)e; (void)o;
    s_sprites++;
    if (s_colValid) {
        if (!t->col) { t->col = 1; s_colTwins++; }
        col = TAGPU_GUICOL_DST | TAGPU_GUICOL_ON;
        s_colOps++;
    }
    /* AND NO `DST` WITHOUT `ON`. The bit means "make the colour plane", and a
       twin that already has one needs nothing made; an indexed sprite over a
       twin that HAS colour still writes `oCol = vec4(0)` through the
       two-attachment pipeline, which the consumer selects from its own
       `colImg` rather than from this bit -- so the colour under that sprite is
       correctly dropped without this op asking for anything. */
    return col;
}

/* A STRING OP INTO ITS TWIN, glyph by glyph (13.4).

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
/* The Vulkan mirror's record of a string, defined with the rest of the
   mirror below -- this is drawn above it, and records what it resolved. */
static void mir_string(const TAGPU_PUBOP* o, const short cell[][4], int n,
                       int x0, int top, unsigned char col);
static void mir_atlas_seen(void);       /* with the mirror below              */

static void twin_string(TWIN* t, const TAGPU_PUBOP* o)
{
    /* THE BLOCK IS GLYPH RECORDS THEN THE STRING. `feed` installs
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

    if (!o->alen) goto reseed;
    /* PASS ONE: rasterise every glyph this string needs, so the atlas texture
       is uploaded ONCE for the string rather than once per new glyph.

       AND THE ATLAS CAN REPACK UNDER US WHILE WE DO IT: a glyph that runs the
       shelves out clears every cell and
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
       `tagpu_text_glyph_have` is how it is asked. */
    if (!tagpu_text_glyph_have()) goto reseed;
    tagpu_text_glyph_dims(&aw, &ah);
    if (aw <= 0 || ah <= 0) goto reseed;

    /* the blitter's destination is base + (y - (s8)font[2]) * pitch + x, so the
       string's first pixel row is at y - yoff and NOT at the y it was given */
    x = (int)o->sl;
    top = (int)o->st - yoff;
    for (i = 0; i < n; i++) {
        int gw = cell[i][2];
        /* The loop walks the glyphs because `x` is the running pen position
           `mir_string` publishes as the string's placement and `s_glyphs` is
           the counter the log reports. */
        x += gw;
        s_glyphs++;
    }
    s_strings++;
    /* NO COLOUR BIT, AND THE ERASE STILL HAPPENS. A string never RESTORES
       anything -- `STR_FS` writes `oCol = vec4(0.0)`, so it erases the colour
       under every glyph it stamps and leaves it standing between them -- and
       the consumer runs that write whenever the destination has a colour plane,
       which it picks from its own `colImg`. So this op asks for nothing and
       gets the erase for free; asking for `DST` would only make a colour plane
       on a twin that text is the first thing to touch. */
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
    unsigned char col = 0;
    (void)o;
    s_copies++;
    /* THE SOURCE'S COLOUR IS THE WHOLE QUESTION, not `s_colValid`: a copy
       carries what is already in the source twin, and that art was restored
       (or not) when it was drawn. Gating this on the live validity would blank
       the destination's colour for the duration of a fade and leave it blank
       afterwards, because nothing re-copies. */
    if (src && src->col) {
        if (!t->col) { t->col = 1; s_colTwins++; }
        col = TAGPU_GUICOL_DST | TAGPU_GUICOL_ON;
        s_colOps++;
    }
    return col;
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

   THE VALIDITY RULE, gui-renderer.md 3.4. A restore job snapshots the palette
   into a texture of its own, so the restored twin's colours are a function of
   the palette that was live when the job was made (`tagpu_vk_restore_job_new`
   snapshots it). While that is still the palette the frame is PRESENTED with,
   colour is used; while it is
   not, every colour twin is ignored and the frame is indexed — dithered art
   for the duration of a fade, never wrong art. Once the palette has held
   still for PAL_SETTLE frames the job is rebuilt against the new one and
   every colour twin is invalidated, so the art comes back restored as the
   engine redraws it. */

/* tagpu_gui.h: the consuming lane's own answer, taken on its prepare. */
void tagpu_gui_col_ready(int have, unsigned settled)
{
    s_colReady = have ? 1 : 0;
    s_colSettled = settled;
}

void tagpu_gui_pic_settled(unsigned settled)
{
    s_picSettled = settled;
}

/* Once per present, BEFORE the drain, because the sprite ops it replays ask
   whether colour is valid and the answer has to be one frame's answer.

   WHAT THIS DOES NOT DO: it does not start, step or own a restore. The list
   is armed on the atlas and the painting is the Vulkan pass's, exactly as the
   terrain, feature and effects atlases work -- so what is left here is the
   one decision this side owns, which is whether an op may claim its texels
   are restored. */
/* THE ART HAS TO BE REDRAWN TO GAIN COLOUR, AND THIS IS WHAT ASKS FOR IT.
   Colour reaches a twin only through the op that DRAWS the art -- a sprite
   writes the restored texel beside the index, a copy carries both -- so a
   surface painted before the restore landed keeps its indexed pixels for as
   long as nothing repaints it. In the shell that is invisible (every gadget is
   redrawn on every flip); IN GAME IT IS THE WHOLE SIDEBAR, which the engine
   draws once when the selection changes and then leaves alone. MEASURED
   2026-09-22 on `pose-inventory` at 1024x768: with the panel left standing,
   `assets=0` and `assets=1` were 0 differing pixels of 82 944 over
   (0,120)-(128,768) and 273 colours either way; deselecting and reselecting the
   commander -- one repaint, nothing else -- took the same region to 23 662
   colours.

   `g_guiq.colarm` IS THE ASK, and it asks for the ENGINE's repaint alone --
   `tagpu_gui_hook.c`'s `repaint_arm` shadows it and the repaint redraws every
   gadget through the leaves, as sprites, which is what carries colour.

   IT IS THE ONE COUNTER OF THAT PAIR THAT CROSSES A THREAD: `resets` is
   raised and shadowed by the SAME thread, the game thread, while this one is
   raised HERE, on the render thread, and shadowed on the game thread. What
   makes that safe is not the `resets` analogy but its own shape: one writer, a
   single aligned `volatile unsigned`, monotone and never reset, consumed as an
   inequality against the shadow rather than as a count. The reader can
   therefore only be one repaint late, never wrong, and a late repaint is the
   direction that costs nothing -- the art stays indexed one screen longer.
   IT IS NOT A RESEED, and that is not a preference: a reseed resets the twin
   store and the UI atlas, which re-arms the restore list, which clears the
   consumer's restored image, which clears this very flag -- so raising a
   reseed HERE closes a loop. Measured 2026-09-22: the validity flag
   oscillated, the atlas re-armed every few frames and the layer composited
   nothing at all, which the harness shows as a magenta frame.

   ONCE PER EDGE, never per frame. It fires on 0 -> 1 only, so a session pays
   for one repaint at the arm and one more per palette re-arm. */
/* ONE REPAINT OUT OF A BUDGET THAT IS PER PALETTE GENERATION, and that is
   where the bound lives. Reset on every 0 -> 1 validity edge, it would bound
   the ordinary settle loop (a repaint adds atlas ENTRIES, the list is appended
   to, `rlistGen` does not move) and NOT the other one: a repaint that makes
   the atlas RESEED bumps `rlistGen`, which frees the job, which drops
   `s_arHave`, which drops validity to 0 and back to 1 -- refilling the budget
   on the way and asking again. So the budget is keyed to `s_rearms`: a
   validity edge inside one generation spends from the same 32, and only a
   genuine palette re-arm grants a fresh one. Returns 1 when it actually
   asked. */
static int col_ask_repaint(void)
{
    if (s_rearms != s_colRepaintsGen) {
        s_colRepaintsGen = s_rearms;
        s_colRepaints = 0;
    }
    if (s_colRepaints >= COL_REPAINT_MAX) {
        if (s_colRepaints == COL_REPAINT_MAX) {
            s_colRepaints++;
            slog("gui: the restored UI atlas has settled 32 times in one palette "
                 "generation and the art is still drawing ahead of it - no further "
                 "repaints are asked for, and whatever is on screen now keeps the "
                 "indices it has");
        }
        return 0;
    }
    s_colRepaints++;
    g_guiq.colarm++;
    return 1;
}

/* ONE REPAINT PER PICTURE SETTLE, OUT OF A BUDGET OF ITS OWN PER PALETTE
   GENERATION. The store's settles converge by themselves -- a repaint that
   draws only pictures the store holds finishes nothing new, and a picture it
   evicted recently and stores again is not counted (tagpu_vk_gui.c
   `ps_recent`) -- and this is the bound for the screen that gets past that
   filter: at most PIC_REPAINT_MAX repaints a generation, whatever the store
   does. Its own budget and not the atlas's, because a walk through the map
   list asks once per pick and would spend the repaint an in-game sidebar
   needs later; 1024 is ten walks through all 99 maps. Spent, a picture
   restored afterwards takes colour at the engine's own next redraw. Returns
   1 when it actually asked. */
static int pic_ask_repaint(void)
{
    if (s_rearms != s_picRepaintsGen) {
        s_picRepaintsGen = s_rearms;
        s_picRepaints = 0;
    }
    if (s_picRepaints >= PIC_REPAINT_MAX) {
        if (s_picRepaints == PIC_REPAINT_MAX) {
            s_picRepaints++;
            slog("gui: the picture store has asked for 1024 repaints in one palette "
                 "generation - no further ones are asked for, and a picture restored from "
                 "now on takes colour at the engine's own next redraw");
        }
        return 0;
    }
    s_picRepaints++;
    g_guiq.colarm++;
    return 1;
}

static void col_valid_edge(int on)
{
    static int was = 0;
    if (on != was) {
        int pic;
        was = on;
        if (!on) return;
        pic = s_picSettled != s_picSettledSeen;
        s_colSettledSeen = s_colSettled;
        s_picSettledSeen = s_picSettled;
        if (col_ask_repaint())
            slog("gui: Classic++ colour is valid - asking the engine for a repaint, because "
                 "art already on a surface keeps the indices it was drawn with");
        /* a picture settle it absorbs keeps its own ask, from its own budget
           (below): the atlas's refusing the edge must not refuse the picture */
        else if (pic)
            pic_ask_repaint();
        return;
    }
    if (!on) return;
    /* THE PICTURE STORE ASKS FOR ITS OWN, from `pic_ask_repaint`'s budget and
       not the atlas's. A settle there is a picture new to the store finished
       (tagpu_vk_gui.c `pic_step`). */
    if (s_picSettled != s_picSettledSeen) {
        s_picSettledSeen = s_picSettled;
        pic_ask_repaint();
    }
    /* AND AGAIN EVERY TIME THE RESTORE SETTLES HAVING PAINTED MORE. The sprites
       drawn by the last repaint may have put entries in the atlas that had no
       restored texels yet, and those draws took alpha 0; one more repaint draws
       them against the atlas as it now is. Finite by construction -- a repaint
       that adds no entry adds no frame, and no frame is no settle -- and
       bounded anyway. */
    if (s_colSettled == s_colSettledSeen) return;
    s_colSettledSeen = s_colSettled;
    col_ask_repaint();
}

static void restore_step(void)
{
    unsigned live;

    /* `norestore` is the A/B lever: the UI layer with the art it would have
       had before Classic++, while the world goes on restoring. */
    s_picArm = 0;
    if (s_norestore || !tagpu_classicpp_assets()) { s_colValid = 0; col_valid_edge(0); return; }
    /* ARMING IS POLLED, NOT LATCHED AT START-UP. `tagpu_gaf_atlas_restore_vk`
       is idempotent and answers 1 on every call after the first, so this is a
       compare once the list exists -- and `assets=0 -> 1` from the
       render-options row arms it on the next present. */
    if (!tagpu_gaf_atlas_restore_vk(&s_atlas)) { s_colValid = 0; col_valid_edge(0); return; }
    /* the list the consumer keys its picture store on is published from here */
    s_picArm = 1;
    /* AND THE CONSUMER HAS TO HAVE SOMETHING TO SAMPLE. A sprite that says
       restored to a consumer with no restored image is drawn indexed into its
       colour twin, so the first two seconds of every level would put indexed
       art where the restored art belongs. */
    if (!s_colReady) { s_colValid = 0; col_valid_edge(0); return; }

    live = tagpu_pal_serial();
    if (!s_colPalSeen) {
        s_colPalSeen = 1;
        s_colPalSerial = live; s_colPalLast = live; s_colPalStill = 0;
        s_colValid = 1; col_valid_edge(1);
        return;
    }
    if (live == s_colPalSerial) {            /* the art is right for the screen */
        s_colPalLast = live; s_colPalStill = 0;
        s_colValid = 1; col_valid_edge(1);
        return;
    }
    /* THE PALETTE MOVED OUT FROM UNDER THE RESTORED TEXELS. Dithered art for
       the duration, never wrong art: the layer falls back to the palette for
       every texel while this is 0. */
    s_colValid = 0; col_valid_edge(0);
    if (live != s_colPalLast) { s_colPalLast = live; s_colPalStill = 0; return; }
    if (++s_colPalStill < PAL_SETTLE) return;
    /* ...AND IT HAS SETTLED. Re-arm against the new one: the list is restarted
       as a REPAINT (the rectangles have not moved, only the palette they are
       read through), and `s_rearms` tells the consumer to invalidate every
       colour plane it holds -- so nothing composites stale colour while the
       repaint walks the atlas, and the art comes back as the engine redraws
       each gadget. */
    s_colPalStill = 0;
    s_colPalSerial = live;
    s_rearms++;
    tagpu_gaf_atlas_restore_repalette(&s_atlas);
    slog("gui: the presented palette settled - the UI atlas is restored against it "
         "again and every colour plane was invalidated");
}

/* ------------------------------------------------------------------ drain */
/* ============================================================================
   THE MIRROR of this present's op stream, which tagpu_vk_gui.c replays.
   tagpu_gui.h carries the contract and the reason it is a COPY rather than a
   pointer into the queue; here is the machinery.

   RECORDED WHERE EACH OP IS APPLIED, not where it is read. The hand-over says
   what the render half DID -- a sprite whose atlas entry could not be resolved
   is applied to nothing here and must draw nothing there, and recording it at
   the top of the switch would publish an op this file declined. So every
   `mir_*` call below sits where the op is applied to the twin table.

   A FRAME IS ALL OR NOTHING. If the arena or the op array will not grow, the
   frame's record is abandoned and published as `lost`: a truncated op stream
   applied to a twin store is a DIFFERENT PICTURE, not a smaller one.
   `s_mirLost` counts it. */

static int       s_mirWant = 0;        /* the Vulkan pass asked for one       */
static TagpuGuiReset s_mirReset;
/* THE ASSET TOKEN THIS RECORD CARRIES, held back until the record is actually
   handed over. `mir_op` succeeding is not delivery: a later op in the same
   drain can exhaust MIR_OPS_MAX/MIR_ARENA_MAX, or a glyph repack or an atlas
   move can invalidate the whole frame, and `mir_finish` then publishes `lost`
   and throws it away. Acking there would tell the producer its 300 KB had
   landed when it had not. */
static unsigned  s_mirAssetTok = 0;
/* ONE SLOT, NOT ONE PER ASSET: a second `PK_ASSET` in the same drain overwrites
   the first, so only the last is acked. It converges rather than stalling --
   the acked surface stops offering, so the next present carries one asset and
   acks it -- at one extra offer per surface per round, and `TAGPU_GUI_ASSET_TRIES`
   still bounds it. One backdrop per screen is the only case that exists today;
   an array is the fix if a screen ever loads two. */
/* ...AND THE TOKEN THE PUBLISHED RECORD CARRIES, which is a different thing
   again. `mir_finish` publishing a record is NOT the consumer taking it: it
   only sets `s_mHave`, and the taking is `tagpu_gui_handover`, which the
   Vulkan lane calls from `tagpu_vk_gui_prepare` LATER IN THE SAME LOOP
   ITERATION -- after an acquire that can return VK_ERROR_OUT_OF_DATE_KHR and
   bail (`tagpu_vk.c`, the swapchain rebuild). The next `mir_begin` then drops
   the untaken record. Acking at `mir_finish` would therefore tell the producer
   its 300 KB had landed on a frame where the record was thrown away, and since
   nothing but an ack retires an offer, the backdrop would go black FOR THE
   REST OF THE EPISODE -- on a path a player reaches by dragging the window,
   with `asset=n/n/0/0` reading as success. */
static unsigned  s_handAssetTok = 0;
static int       s_mirRec  = 0;        /* ...and this frame is being recorded */
static unsigned  s_mirLost = 0;        /* frames abandoned for want of room   */
static int       s_mOther = 0;         /* ops the mirror does not carry       */
/* THE A/B LEVER. `tagpu_gui.ab` latches a claim for one frame; the seam
   captures the Vulkan image on that frame and `tagpu_vk_ab_arm` has already
   unlinked the target, so the file on the disk is this arming's. Diff it
   against a capture from another BUILD. */
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
/* THE SAME QUESTION ABOUT THE UI ATLAS. A sprite's record carries the atlas
   rect this file RESOLVED, and the cursor quad carries one too -- both valid
   only for the generation they were read in. `tagpu_gaf_atlas_put` runs INSIDE
   the drain, so a sprite that fills the atlas recycles it (`atlas_drop`,
   `a->gen++`) in the middle of the very present whose earlier sprites are
   already recorded, and `sharp_cursor` can do it again afterwards.
   `s_mAtAny` is CLEARED BY A RECORDED RESET rather than only by `mir_begin`,
   and that is not tidiness: `twins_reset` recycles the atlas, so EVERY reset
   moves the generation -- a guard that did not forget the ops before one would
   lose every reseed present and answer the reseed with another. */
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
    if (tagpu_gui_reset_begin(&s_mirReset)) {
        g_guiq.why = TAGPU_GUI_WHY_ARM;
        g_guiq.reseed = 1;
    }
    s_mNOps = 0; s_mALen = 0; s_mOther = 0; s_mLayer = 0;
    s_mStrAny = 0; s_mStrGen = 0;
    s_mAtAny = 0; s_mAtGen = 0;
    s_mNSDraw = 0;
    s_abFrame = 0;          /* the claim never outlives the frame that made it */
    s_mirRec = s_mirWant;
    s_mirAssetTok = 0;
    /* THE PREVIOUS RECORD'S TOKEN DIES WITH THE PREVIOUS RECORD. If it was
       never taken, this is what stops a later hand-over acking bytes that were
       thrown away; if this frame carries no asset, it is what stops a stale
       token riding out on an unrelated record. Clearing it HERE rather than
       conditionally in `mir_finish` is what makes both true without a case
       analysis -- including `mir_finish`'s `lost` arm, which returns early. */
    s_handAssetTok = 0;
    s_mHave = 0;
}

/* room for one more op, or abandon the frame */
static TAGPU_GUIOP* mir_op(void)
{
    if (!s_mirRec || !tagpu_gui_reset_ready(&s_mirReset)) return NULL;
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
       a repack of ITS OWN string and is correct for itself -- it resolved its
       cells against the glyph atlas as it stood. This record does not draw until
       the drain is over, against the atlas as it stands THEN, so a repack caused by
       a LATER string in the same present leaves these cells naming cleared
       texels. `mir_finish` compares and loses the frame. */
    if (!s_mStrAny) { s_mStrAny = 1; s_mStrGen = tagpu_text_glyph_gen(); }
    if (!mir_bytes(cell, m->alen, &m->aoff)) s_mNOps--;
}

/* A SHARP-LAYER QUAD, RECORDED WHERE IT IS DRAWN AND WITH WHAT IT RESOLVED.
   The layer's three clients draw in a fixed order and this preserves it, which
   matters: the cursor is drawn LAST and so covers the minimap where they
   overlap (the engine draws its own inside the flip, 0x4C67C0), and the view
   box after its base for the same reason the engine draws them that way
   (0x466B44 then 0x466B5E).

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

/* THE PART OF A TINT'S BOX THIS PASS DID REPAINT: the box minus every box
   dropped in this batch, as disjoint pieces. Inclusive on every edge, which
   is the convention `mir_box` carries and the one the Vulkan lane reads back
   (`extent = x1 - x0 + 1`).

   THE HAZARD IS PER PIXEL, SO THE REFUSAL IS PER PIXEL. A pixel outside every
   dropped box holds what the engine held before its tint, so tinting it is
   exact; a pixel inside one does not, and is left where the drop left it,
   untinted. Declining the whole box instead was sound but lost the modal dim
   `0x4AA969` lays over a 640x480 panel whenever one sprite anywhere on it was
   dropped -- SKIRMISH's side buttons, drawn every frame through the ALP blit
   `0x4B8500` and sitting under the dialog that dims them.

   Each subtraction splits a piece into at most four around the overlap (the
   bands above and below it, then the two sides), so pieces never overlap and
   no pixel is tinted twice. Past `TINT_PIECES` the whole box is declined,
   which is the old answer and never compounds. Returns the piece count; 0 is
   a box wholly under dropped ones. */
#define TINT_PIECES 64
static int tint_pieces(const TWIN* t, const TAGPU_PUBOP* o, int out[TINT_PIECES][4])
{
    int n = 1, k;
    out[0][0] = o->l; out[0][1] = o->t; out[0][2] = o->r; out[0][3] = o->b;
    for (k = 0; k < t->nLost && n > 0; k++) {
        const int* L = t->lost[k];
        int i, m = n;
        for (i = 0; i < m; ) {
            int l = out[i][0], tp = out[i][1], r = out[i][2], b = out[i][3];
            int il = l > L[0] ? l : L[0], it = tp > L[1] ? tp : L[1];
            int ir = r < L[2] ? r : L[2], ib = b < L[3] ? b : L[3];
            int add[4][4], na = 0, j;
            if (il > ir || it > ib) { i++; continue; }
            if (it > tp) { add[na][0] = l;      add[na][1] = tp;     add[na][2] = r;      add[na][3] = it - 1; na++; }
            if (ib < b)  { add[na][0] = l;      add[na][1] = ib + 1; add[na][2] = r;      add[na][3] = b;      na++; }
            if (il > l)  { add[na][0] = l;      add[na][1] = it;     add[na][2] = il - 1; add[na][3] = ib;     na++; }
            if (ir < r)  { add[na][0] = ir + 1; add[na][1] = it;     add[na][2] = r;      add[na][3] = ib;     na++; }
            /* the piece goes; the last unvisited one takes its slot, the
               already-split tail moves down behind it */
            out[i][0] = out[m - 1][0]; out[i][1] = out[m - 1][1];
            out[i][2] = out[m - 1][2]; out[i][3] = out[m - 1][3];
            out[m - 1][0] = out[n - 1][0]; out[m - 1][1] = out[n - 1][1];
            out[m - 1][2] = out[n - 1][2]; out[m - 1][3] = out[n - 1][3];
            m--; n--;
            if (n + na > TINT_PIECES) return -1;
            for (j = 0; j < na; j++) {
                out[n][0] = add[j][0]; out[n][1] = add[j][1];
                out[n][2] = add[j][2]; out[n][3] = add[j][3]; n++;
            }
        }
    }
    return n;
}

static void drain(void)
{
    unsigned tail = g_guiq.qTail, head = g_guiq.qHead;
    int budget = 20000;                    /* ops per present: a burst is many flips */
    int ti;
    for (ti = 0; ti < s_ntwins; ti++) s_twins[ti].nLost = 0;
    while (tail != head && budget-- > 0) {
        const TAGPU_PUBOP* o = &g_guiq.ops[tail & (TAGPU_GUI_QCAP - 1)];
        TWIN* t;
        unsigned char col = 0;          /* TAGPU_GUIOP::col                   */
        /* THE GLYPH BLOCK GOES IN BEFORE THE SWITCH, NOT INSIDE IT: the
           records are the op's own payload, the producer has already marked
           the pairs sent, and installing them is independent of everything the
           switch decides. */
        if (o->kind == PK_STRING && o->alen)
            tagpu_text_glyph_feed(o->font_id, o->font_rows, o->font_yoff,
                                  g_guiq.arena + o->aoff, o->gcount, o->alen);
        switch (o->kind) {
        case PK_FRAME:  s_presented = o->surf; break;
        case PK_RESET:  twins_reset();
            tagpu_gui_reset_record(&s_mirReset);
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
               A surface's contents as the engine has them are not a
               description of a draw but a COPY of what the 1997 rasteriser
               composed. Carried, they would put those bytes in a twin and the
               twin on the screen, which is precisely the engine pixel the cut
               removed -- so the producer sends none (`pub_seed`), and a seed
               that did carry some would still have them dropped here. The
               geometry crosses and the payload does not:
               `m->alen = 0`, and the consumer's own SEED case already answers
               that exactly right -- `tw_make` then `tw_fresh`, which clears to
               coverage 0, then `if (!o->alen) break`.

               WHAT THAT COSTS, STATED: a surface we have not watched being
               drawn is EMPTY rather than adopted, so it shows nothing until the
               engine redraws it through a leaf we observe. That is the hole the
               forced repaint exists to fill -- `GUI_StageUpdateDraw(gi, 0x40)`
               in tagpu_gui_hook.c, which asks the engine to re-emit the top
               screen as ops. It is also why the twin can be empty and must
               never be stale: the hole is closed at the source rather than
               patched by reading the engine's frame. */
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
        case PK_ASSET: {
            /* THE ONE OP WHOSE PAYLOAD CROSSES WHERE A SEED'S DOES NOT, and
               the mirror carries it as a SEED **with** its bytes -- which is
               precisely the shape `TAGPU_GUIOP_SEED` already has on the other
               side (`o->alen` optional, validated as `w * h`), so the Vulkan
               lane needs no case of its own and no new validation. The
               producer's guarantee is what earns this: the surface was filled
               by the loader and `op_add` has never seen an op name it as a
               destination. See PK_ASSET in tagpu_gui_int.h. */
            unsigned off = 0;
            t = twin_make(o->surf, o->w, o->h);
            if (t && o->alen && o->alen == (unsigned)o->w * (unsigned)o->h &&
                mir_bytes(g_guiq.arena + o->aoff, o->alen, &off)) {
                TAGPU_GUIOP* m = mir_op();
                if (m) {
                    m->kind = TAGPU_GUIOP_SEED; mir_box(m, o);
                    m->w = o->w; m->h = o->h;
                    m->aoff = off; m->alen = o->alen;
                    /* A PICTURE FOR THE CONSUMER'S STORE: the loader's bytes,
                       which nothing has drawn over, restored and coloured
                       there. Its colour plane makes every copy out of it carry
                       colour (`twin_copy`). */
                    if (s_picArm) {
                        if (!t->col) { t->col = 1; s_colTwins++; }
                        m->col = TAGPU_GUICOL_DST | TAGPU_GUICOL_ON;
                        s_colOps++;
                    }
                    /* REMEMBERED, NOT YET ACKED -- `mir_finish` publishes it
                       if and only if this record is handed over. */
                    /* ONLY A REAL OFFER'S TOKEN: a snapshot seed carries 0
                       (tagpu_gui_hook.c `pub_seed_snap`) and must not wipe an
                       offer drained earlier in this present, which would then
                       never be acked. */
                    if (o->assetTok) s_mirAssetTok = o->assetTok;
                    s_assets++;
                }
            }
            break; }
        case PK_MOVIE:
        case PK_PLANE: {
            /* A SMACKER FRAME, or a transformed GAF stamp's indices, carried as
               a box of bytes -- `TAGPU_GUIOP_PIXELS` with its payload, which the
               Vulkan lane validates and uploads as it stands. What makes these
               bytes legal to carry, and exact, is the producer's: see PK_MOVIE
               and PK_PLANE in tagpu_gui_int.h. The box is bounded HERE against
               the twin it lands in, whatever the producer clipped it to. */
            unsigned off = 0;
            int bw = o->r - o->l + 1, bh = o->b - o->t + 1;
            t = twin_find(o->surf);
            if (t && bw > 0 && bh > 0 && o->l >= 0 && o->t >= 0 && o->r < t->w && o->b < t->h &&
                o->alen == (unsigned)bw * (unsigned)bh &&
                mir_bytes(g_guiq.arena + o->aoff, o->alen, &off)) {
                TAGPU_GUIOP* m = mir_op();
                if (m) { m->kind = TAGPU_GUIOP_PIXELS; mir_box(m, o); m->aoff = off; m->alen = o->alen;
                         if (o->kind == PK_MOVIE) s_movies++; else s_planes++;
                         /* A STAMP IS A PICTURE THE CONSUMER RESTORES, under
                            the same rule as a sprite (`twin_sprite`): only
                            while colour is valid. One drawn before that is
                            drawn again -- in the shell at the next flip, in
                            game by the repaint the validity edge asks for. A
                            movie frame is not art to restore. */
                         if (o->kind == PK_PLANE && s_colValid) {
                             if (!t->col) { t->col = 1; s_colTwins++; }
                             m->col = TAGPU_GUICOL_DST | TAGPU_GUICOL_ON;
                             s_colOps++;
                         } }
            }
            break; }
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
                        `s_pixels`/`s_seeds` already are: this is the only place
                        in this file a CLEAR is applied, so a counter anywhere
                        else would print `clears=0` while CLEAR ops are mirrored
                        every frame. */
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
               is what the bytes hold -- so it is not a faithful record either.

               The fix is at the PRODUCER, not here: every kind that falls
               through to `as_pixels` in tagpu_gui_hook.c needs a semantic op of
               its own, as `PK_BAR` and `PK_RECT` have. `s_pixDropped` is the
               work list's size -- by cause on the producer's `GUI pixels:`
               line, and by area rather than count on the `gui area:` census
               line, which is what says which kind to do first. Until then those regions simply do not draw, visibly,
               which is the whole reason for cutting rather than levering. */
            t = twin_find(o->surf);
            /* THE TINT'S DESTINATION JUST STOPPED MATCHING THE ENGINE'S, and
               the tint is the only op that cares.
               Every other kind overwrites its box, so a dropped neighbour
               costs one wrong region until the next repaint paints it again.
               A tint READS the box, so applying one over a box we did not
               repaint folds this frame's error into next frame's input:
               `LUT[LUT[x]]`, then `LUT^3[x]`, with nothing short of a
               `PK_RESET` to unwind it. Leaving those pixels untinted instead
               leaves them at their last CONSISTENT state, which is the same
               thing the dropped pixels themselves leave there. */
            if (t) {
                if (t->nLost < TINT_LOST_MAX) {
                    int* L = t->lost[t->nLost++];
                    L[0] = o->l; L[1] = o->t; L[2] = o->r; L[3] = o->b;
                } else {                      /* merge: a union is a superset */
                    int* L = t->lost[TINT_LOST_MAX - 1];
                    if (o->l < L[0]) L[0] = o->l;
                    if (o->t < L[1]) L[1] = o->t;
                    if (o->r > L[2]) L[2] = o->r;
                    if (o->b > L[3]) L[3] = o->b;
                }
                if (o->alen) s_pixDropped++;
            }
            break;
        case PK_BAR:
            /* A SOLID RECTANGLE, filled with one palette index and fully
               covered. No arena bytes to copy and none to run out of, which is
               the whole reason it is not `PK_PIXELS`.
               TWO ENGINE LEAVES REACH THIS, and most of the traffic is the
               second: `DrawBar 0x4BF6F0` (writer `0x4CCDEA`) and an
               AXIS-ALIGNED `DrawLine 0x4BE950` (writer `0x4CC7AB`), whose
               bounding box is the line, one pixel thick. Both writers take the
               low byte of their colour and nothing wider, which is why one
               packet describes both. */
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
               is `OP_FOCUS`, which crosses as `PK_TINT` below. */
            t = twin_find(o->surf);
            if (t) {
                TAGPU_GUIOP* m;
                m = mir_op();
                if (m) { m->kind = TAGPU_GUIOP_RECT; mir_box(m, o); m->fg = o->fg; }
                s_rects++;
            }
            break;
        case PK_SHADE:
            /* THE TABLE, AND IT IS NOT A PICTURE. Its rows are the engine's
               two palette-derived remaps, `globals+0xC8` and PALETTE.SHD
               `globals+0xC4`, which the engine fills at init -- the same
               category as the palette itself, which has always crossed -- so
               it is on the allowed side of the clean cut. Nothing composed it.

               VALIDATED AGAINST THE FORMAT rather than trusted: the producer
               and this file agree on `TAGPU_GUI_SHADE_ROWS` x 256 through one
               constant, and an
               `alen` that is not exactly that is a queue this build did not
               write. Refusing it leaves `s_shadeHave` where it was, which the
               tint case below then reads as "no table". */
            if (o->alen == TAGPU_GUI_SHADE_BYTES) {
                memcpy(s_shade, g_guiq.arena + o->aoff, TAGPU_GUI_SHADE_BYTES);
                s_shadeHave = 1; s_shadeSerial++;
            }
            break;
        case PK_TINT:
            /* A BOX REMAPPED THROUGH ROW `fg` -- one edge of a focus
               rectangle, or the box shader's whole box (a list's selected row,
               the dim under a modal screen). No arena bytes: what the op
               carries is the operation, and the consumer applies it to its own
               twin.

               THE TABLE IS A PRECONDITION AND NOT A FALLBACK. The producer
               publishes `PK_SHADE` ahead of the first tint of a batch and the
               queue is FIFO, so `s_shadeHave` is set by the time this runs --
               unless the table could not be read at all, in which case the
               producer published no tint either (`0x4BEC70` refuses on the
               same condition). A tint here without one is therefore a bug in
               this build, and the answer is to draw nothing rather than to
               index a table of zeros. */
            t = twin_find(o->surf);
            if (!t || !s_shadeHave) { s_tintDrop++; break; }
            /* SEE `PK_PIXELS` ABOVE: only over the part of its box this
               batch did repaint (`tint_pieces`). A tint is the only op here
               that reads its own destination, so it is the only one that may
               not run against pixels we know are wrong. */
            {
                int pc[TINT_PIECES][4];
                int n = tint_pieces(t, o, pc), k;
                if (n <= 0) { s_tintStale++; break; }
                if (n != 1 || pc[0][0] != o->l || pc[0][1] != o->t ||
                    pc[0][2] != o->r || pc[0][3] != o->b) s_tintSplit++;
                for (k = 0; k < n; k++) {
                    TAGPU_GUIOP* m = mir_op();
                    if (!m) break;
                    m->kind = TAGPU_GUIOP_TINT; mir_box(m, o); m->fg = o->fg;
                    m->l = pc[k][0]; m->t = pc[k][1]; m->r = pc[k][2]; m->b = pc[k][3];
                }
                s_tints++;
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
                  /* THE RECT THIS FILE JUST RESOLVED, carried rather than
                     looked up again on the other side: a second lookup could
                     answer differently after a repack. */
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
/* THE UI's RENDER HALF READS THE PACKET, never the engine: the cursor's
   sprite record, the minimap's box, its surfaces and its view box are all
   packet fields.

   THE POINTER IS NEVER KEPT ACROSS A CALL, let alone across a frame: it is
   handed down from the driver's own record every time. A static holding it
   would be exactly the cached packet pointer `tagpu_packet.poison` exists to
   catch, and the two entry points here (the cursor decision and the layer,
   both after the world pass) are separate calls with separate chances to be
   skipped. NULL is "no packet this frame" — a shell frame or a load — and
   every consumer below declines. */

/* ------------------------------------------------------------- the cursor */
/* ONE DECISION PER FRAME (13.5). tagpu_overlay.c calls this after
   tagpu_native_frame and before tagpu_gui_present, and the decision is read
   ONCE, here, from this frame's packet: sharp_cursor, the one reader, draws
   from it.

   The engine's own cursor is drawn inside the flip, where the observer leaves
   drop every op (tagpu_gui_hook.c), so it never reaches a twin, and the
   composite has none of the engine's frame beneath it: nothing here erases
   it, and the sharp layer's is the only cursor the composite carries.

   OWNERSHIP IS LATCHED ON THE ATLAS. A shape we have not uploaded yet is not
   owned: the sharp pass atlases it this frame and the NEXT frame draws it, so
   a new shape costs one frame on which the sharp layer holds no cursor. */
void tagpu_gui_cursor_frame(const TAGPU_PACKET* pk)
{
    const unsigned char* fr;
    const void* pix;
    s_curOwn = 0;
    s_curFrame = NULL;
    /* `s_atlas.made`: ownership is latched on the ATLAS (see above), which is
       a CPU table and exists whether or not any image does. */
    if (!s_on || s_nocursor || !s_atlas.made) return;
    if (!pk || !pk->cur_rec) return;
    /* the sprite record IS a GAF frame header -- size, hotspot, colour key and
       a pixel pointer at +0x10 -- and it comes out of the cursor TABLE, loaded
       once and never rewritten, which is why it may cross as a key at all. The
       publisher sends the record alone; the size and the hotspot are read
       here, off the frame `tagpu_gaf_frame_sane` has vetted. */
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

/* ------------------------------------------------------------ sharp layer */
/* THE SCREEN-SPACE HALF OF 13.2's SHARP LAYER. One RGBA8 image (tagpu_vk_gui.c)
   the size of the frame's viewport in WINDOW pixels — everything of ours at the device's
   resolution (13.1) — cleared at every present and composited above the 1x
   mirror wherever its alpha says it has coverage.

   ROW 0 IS THE VIEWPORT'S TOP ROW, and it is the LAYER SHADER that makes it
   so: it samples this image with the same top-down `uv` it indexes the twin
   with. Attachment row 0 is where NDC y = -1 lands, so a client's geometry
   needs no flip, and a client uses QVS, the very mapping the twins use.
   `sharptest` checks it: its square belongs at the top-left. [MEASURED
   2026-09-09 on the OpenGL layer this replaced: a quad through a shader that
   adds a flip comes out at the foot of the screen.] There is no flip.

   Its clients are the cursor (13.5) and the minimap (13.6). `sharptest` is
   what makes an empty layer testable — without it the gate cannot tell a
   wired layer from a dead one. */
/* THE CURSOR INTO THE SHARP LAYER, at 1x DEVICE size whatever k is (13.5).
   That is the convention every scaled desktop UI follows and it is always
   crisp; `cursorscale=` is the escape for a 3x UI at 4K, where TA's cursors
   carry gameplay meaning.

   ITS POSITION IS THE TRUE POINTER where one is known. The engine only ever
   learns a point on its own logical grid, so its cursor can only sit on
   multiples of k device pixels; mouse_last_client hands back the client point
   the message carried, which is where the pointer actually is, and that is
   also what puts ours AHEAD of the engine's last-drawn position. With no
   client point (an injected click, or before
   the first message) it falls back to the engine's own position at the centre
   of its logical pixel, which is exactly where the engine draws.

   Called from sharp_begin, which records into a layer the consumer clears
   at every present. It runs on the warm-up frame too, one frame BEFORE it may draw, because atlasing the
   shape here is what lets the next tagpu_gui_cursor_frame own it. */
static void sharp_cursor(const TAGPU_FRAME* f)
{
    const TAGPU_GAFENT* e;
    float kx, ky;
    int cx = 0, cy = 0, dx, dy, x0, y0, w, h;
    /* THIS IS A CLIENT OF THE SHARP LAYER: it resolves the cursor's rect from
       a mouse position read on THIS thread at THIS instant and records it with
       `mir_sdraw` for tagpu_vk_gui.c, which is the only copy of that answer
       anyone gets. */
    if (!s_curFrame || !s_sharpW || !s_sharpH) return;
    e = tagpu_gaf_atlas_get(&s_atlas, s_curFrame);
    if (!e) {
        /* Unreachable in practice -- tagpu_gui_cursor_frame only owns a frame
           tagpu_gaf_atlas_find already answered for, and atlas_get consults
           the same index first. If it ever does happen, the frame is
           disowned rather than drawn from an entry the atlas no longer has. */
        s_curOwn = 0;
        return;
    }
    if (!s_curOwn) return;               /* the warm-up frame: atlased above, drawn from the next */
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
       frame. [MEASURED 2026-09-13: the Move and reclaim cursors showed it on
       first use, for a few seconds.] The artifact is the sprite's own
       silhouette drawn in the
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
       a sprite's -- AND ON TODAY'S CODE THIS CANNOT FIRE: `sharp_cursor`'s own
       `tagpu_gaf_atlas_get` cannot recycle the atlas. `atlas_insert` answers
       exhaustion with `a->full = 1` and NULL and never touches `gen`, and the
       three sites that do bump it -- `atlas_drop` (through
       `tagpu_gaf_atlas_reset`/`_forget`), `atlas_repack` (the gui atlas never
       asks for one) and `tagpu_gaf_atlas_lost` -- all run inside the drain or
       outside the present entirely. So nothing moves the generation between
       here and `mir_finish`.
       It stays because the RECT IS RESOLVED THE SAME WAY a sprite's is and the
       next client drawn after the drain would need it; it is marked as a claim
       about this quad, not as a guard that does work today. */
    mir_atlas_seen();
    mir_sdraw(TAGPU_GUISK_CURSOR, (float)x0, (float)y0,
              (float)(x0 + w), (float)(y0 + h),
              e->u0, e->v0, e->u1, e->v1, NULL, (int)e->ck);
    s_curDrawn++;
}

/* THE MINIMAP'S BASE AT ITS NATIVE SIZE (13.6).

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
/* THE LEVEL'S MINIMAP PICTURE, kept on this side. It arrives in the level's
   FIRST in-play packet and in no other — carrying 63 KB in every packet of a
   900-a-second stream to serve one bake would be absurd — so the consumer
   copies it out of that packet and keys the copy on the level generation.
   That copy is also what a palette change re-bakes from, which is why it has
   to outlive the packet that brought it.

   The publisher decodes it on the game thread, on the first in-play draw of
   the level -- not on the LOADER thread, which "never publish outside the
   in-play gate" forbids -- and the engine's own ordering makes that the
   earliest moment every per-map pointer is final (the in-play handler is
   installed only after the loader sets bit 1 of main+0x38D75). */
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
           makes the pair self-healing for every reason a copy can be absent. */
        tagpu_gui_set_minimap_have(0);
        return 0;
    }
    *pix = s_picCopy; *w = s_picW; *h = s_picH; *gen = s_picGen;
    return 1;
}

static void sharp_minimap(const TAGPU_FRAME* f)
{
    /* THIS IS A CLIENT OF THE SHARP LAYER. It produces two things for
       tagpu_vk_gui.c: the `mir_sdraw` records, and the CPU BAKE of the minimap
       picture through the presented palette -- `s_mmPicRgb`, which
       `mir_finish` hands over as `mmPic` and the consumer uploads into an image
       of its own. */
    const TAGPU_PACKET* pk = f->packet;
    const unsigned char* pic = NULL;
    unsigned gen = 0;
    int pw = 0, ph = 0, mx, my, mw, mh;
    float kx, ky;

    if ((!s_mmbase && !s_mmforce) || !pk) return;
    /* NOT `+0x142F1 & 2`, which is what DrawMinimap 0x466B00 tests: that is a
       DIRTY flag and 0x466B16 CLEARS it in the same breath, so it reads 0 on
       almost every frame [MEASURED 2026-09-09: gated on it, this draws nothing
       at all, ever]. The engine can afford a dirty flag
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
    /* THERE IS NO k = 1 GATE HERE. At k = 1 the box is 106x126 DEVICE pixels,
       so drawing it from a 252x252 source throws three quarters of the picture
       away and lands on a nearest downsample where the engine used its own
       stretch: no sharper, and 7232 px away from the oracle every phase-1
       measurement is taken against. But no pixel of the presented frame comes
       from the engine, so declining to draw leaves a BLACK BOX, and a nearest
       downsample that is 7232 px from the oracle beats a hole that is the whole
       126x106 of it.

       MEASURED 2026-09-21, 1024x768 skirmish at k = 1.000: the minimap region
       carried 1 flat colour with a k = 1 gate in and 1097 with `mmbase` forcing
       it past, against the reference's 68 — the unit blips and the viewport box
       drew either way, because those are ops of their own; only the terrain
       picture underneath them was missing. Band-by-band the whole side panel
       then met or beat the reference, and it was the only element short.

       THE COST IS PAID AT EVERY k: ~13 KB of interleave per publish on the game
       thread, in every session. `nominimap` is the opt-out. `mmbase` is a
       harness lever and changes nothing at k = 1. */
    (void)s_mmforce;
    /* THE STANDING REQUEST. The publisher interleaves the three minimap
       surfaces and carries the level's picture only while this is up. Dropped
       by the module's own watchdog after 90 silent frames. The cost is paid at
       every k (see above); `nominimap` is the opt-out. */
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
    /* the question is whether THIS palette's resolve of THIS map is in
       `s_mmPicRgb`. The box is NOT part of the key, though the crop below is
       derived from it: a box that changed within one level and one palette
       would leave a bake of the old size standing. */
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
           190 KB re-upload each — against the 126-px engine pair `mir_finish`
           copies on every frame the minimap is drawn */
        /* THE PICTURE IS PADDED AND THE BOX IS NOT, and drawn whole the right
           of the minimap is a flat blue band. The level's minimap is
           a GAF frame at `main+0x1426B` whose header is a SQUARE 252x252, but
           the map only fills an aspect-correct region inside it: on a 336x400
           map the data runs to column 212 and columns 213..251 are one uniform
           colour [MEASURED 2026-09-21 — the last column that is not internally
           constant is 212, and rows run to 251]. Drawing the whole frame with
           0..1 UVs therefore squeezes 252 columns of which 40 are padding into
           the 106-px box: the map lands in 89 columns and the padding stretches
           across the remaining 17. Golden fills all 106.

           THE CROP GOES IN THE BAKE, NOT IN THE UVs. `MM_FS` uses the SAME `uv`
           for `uPic` and for `uEng`, and the engine's fogged/unfogged pair is
           exactly box-sized and needs the full 0..1 — so narrowing the UVs would
           have fixed the picture and broken the mask with it. Uploading only the
           valid sub-rect keeps 0..1 true for both, needs no new uniform, touches
           no shader, and makes the upload smaller.

           THE VALID REGION IS WHAT THE ENGINE READS, and `0x4B95A0` is not a
           stretch: it is a fixed 2:1 BOX
           DOWNSAMPLE. Destination extent comes from `WORD[arg2+0]`/`[arg2+2]`,
           and for each destination pixel it reads a 2x2 source box -- source
           offset `2*(row*sw + col)` (`0x4B95F6 lea eax,[edi+ebx*2]`), that
           pixel and its right neighbour, then the same pair one source row
           down, blended through the 64K LUT at `[arg1+0xC0]`. So the engine
           reads exactly `[0, 2*bw) x [0, 2*bh)` and crops the padding by never
           looking at it, and the bound we want is that same rule rather than an
           aspect fit. The two agree here only because `ph == 2*bh` (252 = 2*126)
           -- 252 x 106/126 is also 212 -- which is a coincidence of this map's
           box, not a property. */
        {
            int bw = pk->mm_box[2], bh = pk->mm_box[3];
            int vw = pw, vh = ph, y, x;
            unsigned n;
            if (bw > 0 && bh > 0) {
                if (bw < pw / 2) vw = 2 * bw;        /* exactly what 0x4B95A0 reads */
                if (bh < ph / 2) vh = 2 * bh;
                if (vw < 1) vw = 1; else if (vw > pw) vw = pw;
                if (vh < 1) vh = 1; else if (vh > ph) vh = ph;
            }
            n = (unsigned)vw * (unsigned)vh;
            if (n * 3 > s_mmPicCap) {
                free(s_mmPicRgb); s_mmPicCap = n * 3 + 4096;
                s_mmPicRgb = (unsigned char*)malloc(s_mmPicCap);
                if (!s_mmPicRgb) { s_mmPicCap = 0; return; }
            }
            for (y = 0; y < vh; y++) {
                const unsigned char* srow = pic + (size_t)y * (unsigned)pw;
                unsigned char* drow = s_mmPicRgb + (size_t)y * (unsigned)vw * 3;
                for (x = 0; x < vw; x++) {
                    const unsigned char* e = pal + 4 * (unsigned)srow[x];
                    drow[3 * x] = e[0]; drow[3 * x + 1] = e[1]; drow[3 * x + 2] = e[2];
                }
            }
            s_mmPicSerial++;
            s_mmTW = vw; s_mmTH = vh;
        }
        s_mmGenSeen = gen; s_mmPalSeen = tagpu_pal_serial();
        s_mmBaked = 1;
    }
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
    /* THIS FUNCTION IS THE SHARP LAYER'S FRAME. It decides
       `s_sharpOn`/`s_sharpInk` -- which draw_layer and mir_finish hand to
       tagpu_vk_gui.c -- and it runs the two clients whose `mir_sdraw` records
       ARE the layer. The image and its clear are the consumer's. */
    int w = f->vp_w, h = f->vp_h;
    s_sharpOn = 0;
    /* ...AND THE INK WITH IT, on every call. Left standing, a session that once
       had coverage and then took the early return below would publish
       `sharpOn = 1` with no quads for ever -- which tagpu_vk_gui.c reads as
       "coverage I was given no quad for" and composites nothing on every such
       frame. Re-decided from the two clients' own counters at the tail. */
    s_sharpInk = 0;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    /* the layer's SPACE is the frame's viewport, and that is true with no
       target to hold it: the clients clamp to it and record device-pixel
       rects, and `mir_finish` publishes it as sharpW/sharpH. */
    s_sharpW = w; s_sharpH = h;
    if (s_sharptest) {
        /* THE HARNESS LEVER, never for a player: a 64x64 opaque green square
           at the viewport's TOP-LEFT and a one-DEVICE-pixel white column at
           device x = 100. Between them they prove the five things the gate
           cannot otherwise see — the layer exists at the device resolution, it
           composites ABOVE the mirror, alpha is what gates it, row 0 is the
           top, and QVS puts a client's GEOMETRY the right way up. It is
           drawn as quads rather than scissored clears precisely because
           geometry is what the layer's clients use, and a scissor box would
           prove the convention for the one client kind that never needs it. */
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
        /* THE CURSOR LAST, because that is where the engine puts it: its own
           draw is inside the flip (0x4C67C0), after every surface the frame
           composed. Recorded before the minimap, the minimap's quad covered it
           and the pointer vanished whenever it crossed the minimap.
           [FOUND 2026-09-23, the owner's report.] */
        sharp_minimap(f);               /* 13.6's base, behind `mmbase` while it is alone */
        sharp_cursor(f);                /* 13.5's cursor: the layer's first real client */
        s_sharpInk = (s_sharptest || s_curDrawn != c0 || s_mmDrawn != m0) ? 1 : 0;
    }
    /* a future client recording here must not assume draw_layer ran: it can
       return early */
    s_sharpOn = 1;
}

static void draw_layer(const TAGPU_FRAME* f)
{
    TWIN* t = s_presented ? twin_find(s_presented) : NULL;
    float ky;
    int L = 0, T = 0, W = 0, H = 0, key;
    int hudPw = 0, hudBh = 0, hudQ8 = 256;
    if (!t || t->w != f->game_width || t->h != f->game_height) return;
    /* THE PALETTE IS NOT TAKEN HERE. It crosses on the hand-over, read by
       `mir_finish` (`tagpu_pal_live`/`tagpu_pal_serial`) -- which runs AFTER
       the drain, while restore_step decided s_colValid against the serial
       BEFORE it. A drain can be thousands of ops long and the game thread may
       set a new palette during it, so the two can differ for one frame; see
       gui-renderer.md 3.4 for why they must not. */
    /* THE TRUE VIEWPORT, from this frame's packet — the rect the game thread
       published, the same one the world composite keyed on this frame. On a
       frame with no in-game packet the LAST one seen stands: the level's tail
       (the out-of-game packet, a refused packet) can still carry the key fill
       in the engine's surface while terrown's fill flag is up, and an empty
       rect there would show the key colour raw for that frame; between levels
       the old level's rect is the right one and the next level's first packet
       replaces it. */
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

       DERIVED ONCE, HERE, AND BEFORE THE RECORD TEST BELOW. It is a property
       of the FRAME, and two things want it: the hand-over's `hud[]` and the
       300-frame heartbeat's `s=`. Derived inside the `s_mirRec` block, `s_hudS`
       would stay unwritten on a frame that records nothing, and the heartbeat
       would print a scale the HUD does not have. It is also the one
       derivation, rather than a second thing that can drift from the composite
       it is supposed to describe. */
    if (!tagpu_hud_live(&hudPw, &hudBh, &hudQ8)) { hudPw = hudBh = 0; hudQ8 = 256; }
    s_hudS = (float)hudQ8 / 256.0f;
    /* THE HAND-OVER BELOW IS THE WHOLE OUTPUT of this function: `s_mHand`
       describes the composite tagpu_vk_gui.c is to run, derived from this
       frame's own locals -- skipping it would leave the consumer with no UI
       at all. */
    /* The composite's uniforms, for tagpu_vk_gui.c. Taken HERE rather than
       recomputed in the publish: every one of them is a local this function
       derived, and a second derivation is a second thing that can drift. */
    if (s_mirRec) {
        s_mHand.presented = s_presented;
        s_mHand.surfW = t->w; s_mHand.surfH = t->h;
        s_mHand.vpKey = tagpu_terrown_filled() ? key : -1;
        s_mHand.vpL = (float)L; s_mHand.vpT = (float)T;
        s_mHand.vpW = (float)W; s_mHand.vpH = (float)H;
        s_mHand.scaleX = s_k; s_mHand.scaleY = ky;
        s_mHand.sharpOn = s_sharpInk ? 1 : 0;   /* COVERAGE, not existence */
        /* WHETHER THE PRESENTED TWIN'S COLOUR IMAGE MAY BE COMPOSITED: colour
           valid this frame (`s_colValid`, see its declaration) AND this twin
           given a colour plane (`t->col`) -- the header's definition, and the
           consumer's check that its own twin has one (`pres->colImg`). The two
           records are made by the same op, `TAGPU_GUICOL_DST`, and dropped by
           the same RESET, so they agree whenever every record reaches the
           consumer; a record it never takes is the exception (`mir_begin`
           drops it, gpu-status.md *Two colour disagreements are neither*).
           Validity alone was
           not that: a surface presented before any sprite gave it colour --
           every surface right after a reseed -- has no colour plane on either
           side, and the consumer stood the frame down and asked for another
           reseed. MEASURED 2026-09-27 on the Wine suite: that reason was most
           of the fresh starts, and all 8 of Escalation's before it gave up. */
        s_mHand.colourTwins = (s_colValid && t->col) ? 1 : 0;
        s_mHand.colValid = s_colValid;
        s_mHand.vpX = f->vp_x; s_mHand.vpY = f->vp_y;
        s_mHand.vpW_gl = f->vp_w; s_mHand.vpH_gl = f->vp_h;
        /* the same four numbers derived above, not a second reading of
           tagpu_hud_live() */
        s_mHand.hud[0] = (float)hudPw; s_mHand.hud[1] = (float)hudBh;
        s_mHand.hud[2] = 256.0f / (float)hudQ8; s_mHand.hud[3] = s_hudS;
        s_mLayer = 1;
    }
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
    /* the lever is its own file, polled on this cadence like every other pass's */
    s_ab = GetFileAttributesA(AB_FILE) != INVALID_FILE_ATTRIBUTES;
    if (!s_ab) s_abDone = 0;
    /* `norestore`: the layer without Classic++ art, so the two halves can be
       A/B'd live without turning the world's restorer off too */
    s_norestore = on && strstr(buf, "norestore") != NULL;
    /* `sharptest`: 13.2's sharp layer filled with a known pattern. The
       harness's mode, never a player's -- an empty layer proves nothing. */
    s_sharptest = on && strstr(buf, "sharptest") != NULL;
    /* `mmbase`: the TNT's 252-px picture drawn into the sharp layer over
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
        const char* q = on ? strstr(buf, "reseedstress=") : NULL;
        long n = q ? atol(q + 13) : 0;
        /* bounded below: a fresh start every present never lets one finish */
        s_stress = (n >= 10 && n <= 1000000) ? (unsigned)n : 0;
    }
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
    /* the atlas alone: the sprite table this module resolves against, and
       whose CPU mirror the hand-over carries */
    if (!atlas_setup()) return;
    if (s_stress && ++s_stressN >= s_stress) {
        s_stressN = 0;
        s_stressFired++;
        tagpu_gui_mirror_reseed();
        {
            char b[96];
            _snprintf(b, sizeof b, "gui: reseedstress: fresh start %u asked by the harness",
                      s_stressFired);
            b[sizeof b - 1] = 0;
            slog(b);
        }
    }
    /* No palette is uploaded here: it crosses to the consuming lane as bytes on
       the hand-over. `restore_step` runs before the drain, so the sprites it
       replays ask one frame's question about whether their texels are
       restored. What it decides is only that; the painting is the Vulkan
       pass's. */
    restore_step();
    /* NO RESTORER IS STEPPED HERE: the Vulkan restorer is stepped from
       `tagpu_vk.c` inside the frame's command buffer and needs nothing from
       here. WHOEVER STEPS A RESTORER HERE steps it BEFORE THE DRAIN, so that
       what it paints this frame is what the drain's sprites sample. The shell,
       and the game with the world passes disarmed, is the case that makes it
       necessary -- the native pass returns early with no unit array, so
       nothing else would drain this atlas's queue and every sprite would read
       alpha 0 from an unpainted twin, the UI staying indexed for ever and
       silently. The same ordering governs the indexed mirror, which is written
       by the paint. */
    mir_begin();            /* the Vulkan mirror records this drain */
    drain();
    /* AFTER the drain and before the layer: the sharp layer's quads for this
       frame are recorded (and, under `sharptest`, the pattern) before
       draw_layer decides what the composite carries. */
    sharp_begin(f);
    {
        /* ONE LEVER, ONE FRAME, AND THE FLAG TRAVELS WITH THE DATA (the
           roadmap's A/B shape). The Vulkan lane captures the frame this flag
           arrived on rather than whichever one its own poll landed on --
           `mir_finish` hands it over with the ops. */
        int taking = s_ab && !s_abDone;
        /* THE CAPTURE IS ARMED BY ITS OWN UNLINK. `tagpu_vk_ab_arm` is the
           shape the world passes use: it unlinks the target and grants the
           claim only when the file is gone, so a stale capture is never paired
           with a fresh one. */
        draw_layer(f);
        if (taking) {
            s_abDone = 1;
            /* THE A/B CLAIM. `tagpu_vk_ab_arm` unlinks the target `_vk.ppm` at
               the instant the claim latches, which is what makes the file on
               the disk this arming's; diff it against a capture from another
               BUILD.

               THE CLAIM IS THIS FRAME'S OR NOBODY'S. Set here and cleared only
               inside `mir_finish`, a frame that published no record would carry
               the flag forward and the capture would land on a DIFFERENT frame
               -- the one thing the "one lever, one frame" rule exists to
               prevent. */
            s_abFrame = tagpu_vk_ab_arm("gui");
        }
    }
    mir_finish(f);          /* close and publish the frame's record */
    if (f->frame_counter - last >= 300) {
        /* THE BUFFER: 457 literal characters and 78 conversions, worst case
           1313 with the terminator, against 1344 bytes -- 31 bytes of headroom,
           which is THREE `%u`s (counted 2026-09-26, by script over the format
           string). Count it again when you add one, the same way: %u -> 10,
           %d -> 11, %08X -> 8, each float -> 24 (a bound on the values these
           carry, not on a double). A count by eye has been wrong here before,
           and truncation here is silent and takes the TAIL, where `fps=` lives;
           _snprintf does not NUL-terminate what it truncates -- hence the
           explicit terminator below. */
        char b[1344];
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
        _snprintf(b, sizeof b, "gui: twins=%d presented=%08X drained=%u seeds=%u sprites=%u copies=%u pixels=%u pixdrop=%u assets=%u movie=%u planes=%u bars=%u rects=%u tints=%u/%u/%u/%u/%u clears=%u atlas=%d/%d lost=%u resets=%u overflows=%u gafnoplane=%u gafreseed=%u gafscratch=%u/%u/%u strrearm=%u glyscratch=%u/%u gfont=%u/%u/%u/%u stalls=%u palchg=%u paldiff=%d@%d palsrc=%d cpp=%d assets=%d light=%d col=%u/%d colvalid=%d rearms=%u rgb=%d/%d colops=%u prescol=%d k=%.3f s=%.3f sharp=%dx%d curs=%d,%dx%d,dev=%d,sc=%.2f,drawn=%u,warm=%u str=%u/%u,miss=%u,reseed=%u,repack=%u,glyphs=%u/%u,fonts=%d arena=%u mirlost=%u mm=%u,fog=%u/%u,noeng=%u fps=%.1f",
                  s_ntwins, s_presented, s_drained, s_seeds, s_sprites, s_copies, s_pixels, s_pixDropped, s_assets, s_movies, s_planes, s_bars, s_rects, s_tints, s_shadeSerial, s_tintDrop, s_tintStale, s_tintSplit, s_clears,
                  s_atlas.n, s_atlas.max, s_lostSprites, g_guiq.resets, g_guiq.overflows, g_guiq.gafnoplane, g_guiq.gafreseed, g_guiq.gafhigh, g_guiq.gaflost, g_guiq.gafbaddec, g_guiq.strrearm, g_guiq.glyhigh, g_guiq.glylost, pGlyphs, pResends, pRefused, pRecycles, g_guiq.stalls,
                  tagpu_pal_changes(), palDiff, palDiffAt, tagpu_pal_presented(),
                  tagpu_classicpp_on() ? 1 : 0, tagpu_classicpp_assets() ? 1 : 0,
                  tagpu_classicpp_lit() ? 1 : 0,
                  s_colTwins, s_ntwins, s_colValid, s_rearms,
                  s_atlas.rlistWant ? s_atlas.rlistN : 0, s_atlas.n,
                  s_colOps, twin_find(s_presented) && twin_find(s_presented)->col ? 1 : 0,
                  s_k, s_hudS, s_sharpW, s_sharpH,
                  s_curOwn, s_curW, s_curH, s_curDev, s_cursorScale, s_curDrawn, s_curWarm,
                  s_strings, s_glyphs, s_strMiss, s_strReseed, s_strRepack, gCached, gDrops, gFonts,
                  /* the arena head is MONOTONIC, so the delta between two of these
                     lines is the bytes the producer wrote in 300 frames — which is
                     what `nostring` is A/B'd on (13.4: ~40 bytes where a text op
                     carried ~968) and what §7's cadence note is about */
                  g_guiq.aHead, s_mirLost, s_mmDrawn,
                  /* THE PAIR'S SIZE AS READ from the packet, which is where
                     `mir_finish` gets it too */
                  s_mmFogged,
                  f->packet ? (unsigned)(f->packet->mm_w * f->packet->mm_h) : 0u,
                  s_mmNoEng,
                  fps);
        b[sizeof b - 1] = '\0';
        slog(b);
    }
}

/* The frame's record is closed and published. Everything the port
   needs that is NOT an op goes in here -- the atlas texels, the palette, the
   engine's own frame -- each of them a copy or a mirror this module owns,
   because the Vulkan lane does not run until after `tagpu_packet_frame_end`. */
static void mir_finish(const TAGPU_FRAME* f)
{
    /* PUBLISHED EVEN WHEN THE COMPOSITE DID NOT DRAW. `draw_layer` returns
       early when the presented surface has no twin or its size has moved --
       and on those frames the drain still APPLIED this frame's ops to the twin
       table. Withholding the record leaves the Vulkan store behind by exactly
       those ops, for the session, which is the same hole the consumer is built
       to close.
       `presented = 0` is how the consumer is told the composite state is not
       valid: it replays and does not draw. */
    /* AN ABANDONED FRAME IS PUBLISHED, NOT WITHHELD: the same hole as the
       paragraph above, at the other end. `s_mirRec` goes to 0 when
       the op array or the arena refuses to grow; withholding the record then is
       byte-identical, to the consumer, to "the lane is not armed" -- so it
       believes it is level while it is a frame of ops behind, for the session.
       A repack between a recorded string and here is the same statement about
       the same frame. Both publish `lost`, which the consumer answers with the
       behind state. */
    if (!s_mirWant || !tagpu_gui_reset_ready(&s_mirReset)) { s_mHave = 0; return; }
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
    /* THE TOKEN MOVES TO THE RECORD; IT IS NOT ECHOED HERE. Past `!s_mirWant`,
       past `!s_mirRec` and past both atlas-generation tests, this record is
       good -- but "good" is not "delivered", and the producer must only stop
       re-offering for bytes the consumer actually holds. The echo is published
       in `tagpu_gui_handover`, where taking the record is what the call means.
       Assigned unconditionally so a frame with no asset clears it. */
    s_handAssetTok = s_mirAssetTok; s_mirAssetTok = 0;
    if (!s_mLayer) {
        s_mHand.presented = 0; s_mHand.surfW = s_mHand.surfH = 0;
        s_mHand.sharpOn = 0;
        s_mHand.colourTwins = 0;
        s_mHand.colValid = 0;
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

    /* THE RESTORED UI ATLAS IS PUBLISHED AS THE FRAME LIST, the WORK rather
       than the picture: the consuming lane paints a restored image of its own
       from these rectangles. `rlist` is one `malloc` with no `realloc` and no
       second free (tagpu_gaf.c), so its address is fixed for the life of the
       atlas and may be aliased here; a consumer copies the frames it takes
       inside the same call. The three counters beside it are the cursor's --
       see tagpu_gui.h. GATED ON `rlistN`, so "a list exists but is empty"
       cannot reach a consumer as a generation with nothing in it.
       A ROW COUNT PUBLISHED BESIDE A PICTURE must cover the cells
       `tagpu_gaf_atlas_put` adds inside the DRAIN, after any read-back: rows
       past a stale count put 166 827 px of undefined memory on the screen at
       1080p and 0 px at 1024x768 on the same build. */
    if (s_atlas.rlistWant && s_atlas.rlist && s_atlas.rlistN > 0) {
        s_mHand.restoreFrames  = s_atlas.rlist;
        s_mHand.restoreN       = s_atlas.rlistN;
        s_mHand.restoreGen     = s_atlas.rlistGen;
        s_mHand.restoreRepaint = s_atlas.rlistRepaint;
        s_mHand.restoreBlanks  = s_atlas.rlistBlanks;
    } else {
        s_mHand.restoreFrames = NULL; s_mHand.restoreN = 0;
        s_mHand.restoreGen = 0; s_mHand.restoreRepaint = 0;
        s_mHand.restoreBlanks = 0;
    }
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
    /* THE ENGINE'S PAIR IS COPIED, NOT ALIASED: it is packet memory, and the
       packet's rule is stricter than the hand-over's, which is the FRAME. It
       is given back inside `tagpu_packet_frame_end`, which `render_vk.c`
       calls BEFORE `tagpu_vk_frame` in the same iteration -- so a consumer of
       an alias would read it after its owner has released it, and
       `tagpu_packet.poison` fires at that give-back, which means the lever
       built to catch exactly this could not see it. An alias would be safe
       only by the rotation, which is not a bound. It is why `mmPic`, the
       atlas and the palette are all copies. 47 KB a frame, and only when
       an MM quad was recorded.
       THE DIMENSIONS COME FROM THE SAME READ as the bytes, so the two cannot
       disagree. */
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
                   `alen` were taken further up, so a grow here would leave the
                   ops' own base pointer stale and every `aoff` in the record
                   pointing into freed heap. */
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

    /* THE LIGHTEN TABLE, BY POINTER AND EVERY FRAME. It aliases
       this file's own static, not the queue's arena, so it outlives the drain
       that filled it -- and carrying it on every hand-over rather than as a
       one-shot is what makes `PK_SHADE` need no acknowledgement: a frame the
       consumer abandons loses the RECORD, never the table. NULL until one has
       arrived, which is a state no tint op can be published into. */
    s_mHand.shade = s_shadeHave ? s_shade : NULL;
    s_mHand.shadeSerial = s_shadeSerial;

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
    if (on && !s_mirWant) {
        /* The drain can already own twins when its consumer first arms.
           Publish no partial history: the first delivered op must be a
           producer RESET, followed by that epoch's seeds and draws. */
        tagpu_gui_reset_arm(&s_mirReset);
        g_guiq.why = TAGPU_GUI_WHY_ARM;
        g_guiq.reseed = 1;
    }
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
           edit -- finds it already correct. */
    }
    s_mirWant = on ? 1 : 0;
    /* THE PRODUCER'S ASSET THROTTLE, PUBLISHED FROM THE ONE PLACE THAT DECIDES
       IT. A `PK_ASSET` is ~300 KB of game-thread memcpy and is worth nothing
       while this lane is not recording, so the producer reads this before it
       composes one. It is a THROTTLE and not the handshake: correctness is the
       token echoed in `mir_finish`, and a stale read here costs one frame of
       delay or one unacked offer, never a lost asset. */
    g_guiq.mirArmed = s_mirWant ? 1u : 0u;
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
    tagpu_gui_reset_deliver(&s_mirReset, out->lost);
    /* THE ASSET ECHO, AND THIS IS THE ONE PLACE IT IS TRUE BY DEFINITION. The
       producer reads `assetAck` to decide an offer is done with; publishing it
       in the same statement sequence that hands the record out makes "acked"
       mean "the consumer has these bytes" rather than "a record carrying them
       was made available and may yet be dropped". An ordering, not a window:
       there is no path from here that can fail to deliver what `*out` already
       carries. */
    if (s_handAssetTok) { g_guiq.assetAck = s_handAssetTok; s_handAssetTok = 0; }
    return 1;
}
