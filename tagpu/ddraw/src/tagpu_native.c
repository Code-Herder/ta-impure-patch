/* tagpu_native.c — G12b: the first truly NATIVE unit pass (Phase D).

   Chosen unit types leave the 8bpp composite path entirely: their composites
   are wiped to ColorKey (engine keeps pose/AABB/alloc/blit — of nothing) and
   the units are rendered here, in the present hook, as RGB into a
   game-resolution FBO composited over the frame:

     - geometry: the same engine-posed PrimitiveStruct walk as render3do,
       positioned in VIEWPORT coordinates with the live viewport rect;
     - materials: the shared 8bpp atlas + engine SHD shade rows, lifted to RGB
       through the live palette (256x1 RGBA texture, refreshed per frame — not
       because it cycles: it does NOT cycle in play, measured 2026-09-05, see
       tagpu_terr.c. Re-reading it is free and survives whatever does write it);
     - occlusion: per-fragment test against the G12a scene-depth scaffold
       (painter's row keys; a tall feature in a nearer row hides the unit),
       plus a real GL depth buffer for self/inter-unit occlusion;
     - fog: per-fragment sample of the LOS counter map + MAPPED bits
       (32-px tiles, uploaded as R8 textures each frame), LosType-aware;
     - shadow: engine rules (shadows-cloak.md): the unit silhouette, 50%
       black, +5px x, at ground height, options-gated, drawn before the body,
       and blended ONCE PER SILHOUETTE PIXEL through a stencil mask -- the
       engine blits one blackened copy of the composite, so a pixel the model
       covers twice is still darkened once (see the shadow loop). A mobile
       unit's is its body silhouette; a structure's is the engine's cached
       SLANT projection, which owndraw "all" stops the engine from blitting
       (G13k) and emit_slant draws from the live posed prims by the engine's
       own raster rules -- every face, flat, no waterline erase (G14j). A 3DO
       wreck keeps the engine's FShadow feature shadow. FBI
       noshadow/canhover/floater gates mirror the engine;
     - cloak (unit+0x10E bit2): true translucency (alpha 0.5); cloaked
       enemies are skipped entirely (engine parity);
     - waterline / digger (shadows-cloak.md §3, path-B units only): parts
       whose model height sits at or below sea level minus the unit's
       altitude are erased (enemy, no sonar) or tinted r/2,g/2,b/2+50 (own,
       sonar-seen); shadows are cut there; diggers lose everything below
       the origin. Per-vertex model height rides in the vertex stream.

   Armed by tagpu_native.on (first token = type, default armcom; the 3-name
   match as everywhere). Under-construction units (unit+0x104 nano > 0) stay
   on the composite path until complete — the nanoframe look is already
   verified there; ownership begins at completion.

   G12c close-out + G12d additions:
     - WRECKS (extra token "wrecks" in tagpu_native.on): 3D husks are found by
       walking the sweep-rect FeatureStruct tiles (flags bit0 -> wreck record
       *(main+0x1420B)+idx*0x30; only defs with FeatureMask bit0 CLEAR are 3D)
       and emitted at FEATURE depth (3+rel*4). The engine's own wreck draw
       (scratch fake-unit *(main+0x1420F) -> DrawUnit) is suppressed by the
       owndraw classifier when tagpu_native_wrecks_armed().
     - 2x SUPERSAMPLING (default on, killed by tagpu_ss.off): geometry renders
       into a 2x-game-res FBO, box-downsampled (LINEAR quad) into the 1x FBO,
       which composites NEAREST as before — game-res look, antialiased edges,
       visual parity with the composite path's SS.
     - SUB-PIXEL MOTION (default on, killed by tagpu_subpix.off): engine
       positions are integer shorts at sim rate; anchors interpolate between
       the last two sim samples per unit slot (render-side only), so walkers
       glide at present rate instead of stepping at sim rate.

   RESOLUTION RULE: the FBO is game_width x game_height (the game's requested
   mode, from the frame struct); every viewport quantity is a live engine
   read. Nothing here knows 640x480. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_opt.h"
#include "tagpu_native.h"
#include "tagpu_render3do.h"
#include "tagpu_scaffold.h"
#include "tagpu_hires.h"
#include "tagpu_hires_draw.h"
#include "tagpu_fx.h"
#include "tagpu_sfx.h"
#include "tagpu_fxown.h"
#include "tagpu_feat.h"
#include "tagpu_terr.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_terrown.h"
#include "tagpu_fogwide.h" /* the fog grid over the zoomed-out view, not just the 1x one */
#include "tagpu_mark.h"
#include "tagpu_markown.h"
#include "tagpu_order.h"
#include "tagpu_owndraw.h"   /* tagpu_owndraw_structshadow_ours: who draws a building's shadow */
#include "tagpu_reclaim.h"   /* the teardown fence this file's template reads stand behind */
#include "tagpu_posebake.h"
#include "tagpu_vk.h"        /* tagpu_vk_owns_present: is there a GL lane at all? */
#include "tagpu_posedraw.h"  /* G16 step 4: the per-type geometry bake and its caches */
#include "tagpu_lerp.h"      /* smooth-motion.md option A: the pose between two sim ticks */
#include "crc32.h"          /* the tagpu_posecrc.on gate oracle */
#include "tagpu_glsl.h"
#include "tagpu_zoom.h"
#include "tagpu_packet.h"  /* the view every pass draws from (landing 2) */
#include "tagpu_overlay.h"   /* tagpu_overlay_target_fbo: the frame's default draw target */
#include "tagpu_gui.h"       /* tagpu_gui_cursor_own: whose cursor is on screen (G17c) */
#include "tagpu_pal.h"       /* the palette the screen is SHOWN with, not main+0x143A7 */
#include "tagpu_vpwide.h"
#include "tagpu_hud.h"
#include "tagpu_shadow.h"    /* Classic++ cast shadows: the depth pass + read-back (G14i) */

/* ---- engine layout (all binary-verified in earlier phases) ---- */
#define TA_MAINPP    0x00511DE8u
#define OFF_BEGIN    0x14357
#define OFF_END      0x1435B
#define OFF_EYEX     0x1431F
#define OFF_EYEY     0x14323
#define OFF_VP_L     0x37E27
#define OFF_VP_T     0x37E2B
#define OFF_VIEW_W   0x37E37
#define OFF_VIEW_H   0x37E3B
#define OFF_GFXOPT   0x37F06   /* bit2 Shadow, bit3 TShadow                 */
#define OFF_LOSTYPE  0x14281   /* u16: b0 mapping, b1 true LOS              */
#define UNIT_STRIDE  0x118
#define U_STATE      0x110
#define U_XPOS       0x6C
#define U_ZPOS       0x70      /* altitude                                  */
#define U_YPOS       0x74      /* world Z (map depth) = the sort key source */
/* the shorts above are the HIGH WORDS of engine 16.16 fixed-point positions
   (ui-markers: DrawUnitSelectBoxRect reads +0x6A/6E/72) — the engine keeps
   sub-pixel fractions natively; read the full i32s for smooth motion */
#define U_XFIX       0x6A
#define U_ZFIX       0x6E      /* altitude, 16.16                           */
#define U_YFIX       0x72      /* map depth, 16.16                          */
#define U_ROT        0x64      /* u16[3] {bank, heading, pitch}, 65536=360  */
#define U_YAW        0x66      /* u16 body yaw, 65536 = 360 deg             */
#define U_MODELID    0xA6      /* u16 index into MODEL_PTRS                 */
#define OFF_MODELPTRS 0x14377  /* Model3DONode* [] (model templates)        */
#define OFF_UDEFCOUNT 0x1438F  /* u32 UNITINFOCount: the LENGTH of that     */
                               /* table as well as of the unit-def array —  */
                               /* 0x42DBCA loops i = 1 .. count-1 over      */
                               /* exactly these two (stride 4 and 0x249)    */
#define OFF_UIGATES  0x37F2F   /* bit2 = SelBoxes toggle (default on)       */
#define OFF_GUICOL   0x0DCB    /* GUI colour byte array: GetGuiPaletteColor  */
                               /* (composite-buffer.md) = *(u8*)(ta+0xDCB+i)*/
#define SELBOX_COLIDX 0x0A     /* the select box's GUI colour — an INDEX INTO */
                               /* that array, not a palette index: 0xA reads  */
                               /* 233 in stock TA, and using 10 raw drew the  */
                               /* box in a dark colour that went unnoticed    */
                               /* while the engine still painted its own      */
                               /* green one over it (G13d)                    */
#define U_OBJ3DO     0x9E
#define U_TYPE       0x92
#define U_OWNER      0xFF
#define U_NANO       0x104     /* float fraction REMAINING                  */
#define U_CARGO      0x8A      /* first unit carried/being built inside      */
#define U_CARGONEXT  0x8E      /* next in that chain                         */
#define ST_NOCARGO   0x20000u  /* the blit's own skip on a chain member      */
#define U_CLOAKF     0x10E     /* bit2 = actively cloaked                   */
#define UD_TYPEMASK  0x241     /* u32 FBI booleans: bit12 canhover, bit19   */
                               /* floater, bit25 noshadow (shadows-cloak §2)*/
#define ST_STRUCT    0x20000000u /* state bit: engine takes the cached-shadow */
                                 /* (nanoframe/structure) blit path          */
#define ST_SONAR     0x200u    /* state bit: submerged enemy shown tinted    */
#define UD_DIGGER    0x40000000u /* FBI mask bit30: clip below ground level  */
#define OFF_SEALEVEL 0x1427F   /* u8 water level, elevation units           */
#define OFF_LOCALPL  0x2A43    /* u8 the blit compares unit+0xFF against    */
/* The per-unit composite GAFFrame (Object3do+0x10) and its depth plane
   (GAFFrame+0x14) used to be read here; the packet carries the answer as
   TAGPU_PK_U_DEPTHPLANE since landing 3, and both offsets went unused with it.
   They are in research/notes/exe-reverse-engineering.md. */
#define OFF_FMAP     0x14287   /* FeatureStruct tile map, stride 0x0D       */
#define OFF_MAPW     0x14233   /* map W in 16-px tiles                      */
#define OFF_MAPH     0x14237   /* map H in 16-px tiles                      */
#define OFF_FDEFS    0x1426F   /* FeatureDef array, stride 0x100            */
#define OFF_WRECKS   0x1420B   /* wreck records, stride 0x30                */
#define FT_STRIDE    0x0D
#define FT_DEFIDX    0x08      /* u16; <0xFFFB = live feature anchor        */
#define FT_WIDX      0x0A      /* u16 wreck record index (when flags bit0)  */
#define FT_FLAGS     0x0C      /* u8; bit0 = wreckage present               */
#define FD_STRIDE    0x100
#define FD_MASK      0xFE      /* u8; bit0 set = animated GAF wreck (engine)*/
#define WR_STRIDE    0x30
#define WR_OBJ3DO    0x04
#define WR_XPOS      0x08      /* i32 16.16 world x   (>>16 = world units)   */
#define WR_ZPOS      0x0C      /* i32 16.16 altitude  (>>16 = world units)   */
#define WR_YPOS      0x10      /* i32 16.16 world z   (>>16 = map depth)     */
#include "tagpu_model3do.h"   /* O3_*, PRIM_*, P_*, N_*, F_*: the engine's model structures */

#define MAXNV  49152           /* vertices across all native units per frame */
/* Units (and wrecks) gathered per frame. This is NOT a soft limit: a unit past
   it is not merely undrawn, it is INVISIBLE — tagpu_overlay.c wipes the engine's
   composite for every unit `tagpu_native_owns_unit` accepts, whether or not this
   gather included it. So it has to stay ahead of the rect the gather fills from,
   and since G13d that rect grows with zoom-out (16x the area at the 0.25x
   floor). MAXNV is the real budget and truncates gracefully; this one must not
   be what runs out first. */
#define MAXU   2048
#define NVST   14              /* x,y,depthEnc, u,v, flat,ck, shadeRow, wx,wzp, vy,
                                  nx,ny,nz (Classic++: the posed face normal in
                                  map space, unit length, flat per face; level
                                  for lines and unshaded pieces) */
/* depth keys: ground rows encode as (feat?3:1) + rel*4 (rel = row − r0, up
   to the sweep's row count plus the ±256 px gather slack); the engine draws
   projectiles/explosions after every ground row and before the airborne sweep
   (terrain-depth.md 3), so per frame: fxKey = just above the last possible
   row, airKey above that, and the VS divides by a scale above airKey — no
   absolute constant survives a taller viewport */
#define ROW_SLACK 8               /* rows a gathered unit may sit past the sweep */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }
static void pose_dump(const TAGPU_PACKET* pk, const TAGPU_PK_UNIT* u,
                      const TAGPU_PK_PIECE* pc, int nparts);

/* ---- sub-pixel motion, and why the table is file-static ----------------
   The engine keeps 16.16 fixed-point unit positions (the roster shorts are
   just their high words) and steps them once per SIM tick, while we present
   far more often than that. Keeping the last two samples per unit SLOT and
   interpolating between them is what makes an owned unit's body slide rather
   than step.

   The table used to be a function-local static inside tagpu_native_frame,
   because the unit pass was its only reader. It is not any more: a marker
   drawn at native resolution — a crisp range circle centred on a walking
   unit — steps once per tick against a body that slides, and the step is
   plainly visible where the 1997 art's own blockiness hid it. So the read
   half is factored out and published as tagpu_native_unit_pos().

   ONLY THE UNIT PASS WRITES IT, and only for units it owns, so a unit the
   pass does not gather has no sample and every reader falls back to the raw
   16.16. Slot reuse (a dead unit's slot handed to a new one) is caught by the
   same distance snap the pass has always used, plus — for the accessor — a
   check that the stored sample still describes the unit being asked about. */
typedef struct { int x, z, y; int px, pz, py; unsigned tc, tp; } SPX;
static SPX      s_spx[8192];
static unsigned s_spxFrame;     /* the frame the pass last refreshed it in */

/* The read half. 1, and the three outputs filled, when slot `slot` carries a
   usable pair of samples for frame `fc`; 0 leaves them untouched, which is why
   every caller seeds them with the raw fixed-point position first. */
static int spx_sample(size_t slot, unsigned fc, float* fx, float* fz, float* fy)
{
    const SPX* e;
    unsigned dt, el;
    float dx, dz, dy, a;
    if (slot >= 8192) return 0;
    e = &s_spx[slot];
    if (e->tp == 0) return 0;
    dt = e->tc - e->tp;
    el = fc - e->tc;
    dx = (float)(e->x - e->px) / 65536.0f;
    dz = (float)(e->z - e->pz) / 65536.0f;
    dy = (float)(e->y - e->py) / 65536.0f;
    /* dt in [1,30]: a longer gap is a unit that stood still and started again,
       not a tick. el < dt: never extrapolate past the next expected sample.
       The distance bound is the slot-reuse snap — a teleport-sized "step" is a
       different unit in a recycled slot, and interpolating it would draw the
       marker somewhere between two unrelated places. */
    if (dt < 1 || dt > 30 || el >= dt ||
        dx * dx + dy * dy + dz * dz > 1024.0f) return 0;
    a = (float)el / (float)dt;
    *fx = (float)e->px / 65536.0f + dx * a;
    *fz = (float)e->pz / 65536.0f + dz * a;
    *fy = (float)e->py / 65536.0f + dy * a;
    return 1;
}

static void nlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* units skipped because their object pointer moved between gather and emit
   (a death landed inside the frame); per 300-frame window, on the native: line */

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_DEPTHFUNC)(GLenum);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_BLENDFUNC)(GLenum,GLenum);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM3F)(GLint,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM4F)(GLint,GLfloat,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_CLEARBUFFERFV)(GLenum,GLint,const GLfloat*);
typedef void (APIENTRY *PFN_DEPTHMASK)(GLboolean);
typedef void (APIENTRY *PFN_SCISSOR)(GLint,GLint,GLsizei,GLsizei);
typedef void (APIENTRY *PFN_LINEWIDTH)(GLfloat);
typedef void (APIENTRY *PFN_STENCILFUNC)(GLenum,GLint,GLuint);
typedef void (APIENTRY *PFN_STENCILOP)(GLenum,GLenum,GLenum);
typedef void (APIENTRY *PFN_COLORMASK)(GLboolean,GLboolean,GLboolean,GLboolean);
typedef void (APIENTRY *PFN_BLITFB)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_DEPTHFUNC  x_glDepthFunc;
static PFN_DISABLE    x_glDisable;
static PFN_BLENDFUNC  x_glBlendFunc;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_UNIFORM3F  x_glUniform3f;
static PFN_UNIFORM4F  x_glUniform4f;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_CLEARBUFFERFV x_glClearBufferfv;
static PFN_DEPTHMASK  x_glDepthMask;
static PFN_SCISSOR    x_glScissor;
static PFN_LINEWIDTH  x_glLineWidth;
static PFN_BLITFB     x_glBlitFramebuffer;
static PFN_STENCILFUNC x_glStencilFunc;
static PFN_STENCILOP   x_glStencilOp;
static PFN_COLORMASK   x_glColorMask;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

static int    s_state = 0;             /* 0=unloaded 1=ready 2=failed */
/* READ FROM THE GAME THREAD (tagpu_native_owns_unit, via tagpu_markown.c's
   mark_selbox), written here on the render thread — volatile so the publishing
   store below cannot be hoisted above the state it publishes. (It used to cite
   s_selComplete for the same reason; that one has no writer left, see its own
   comment.) */
static volatile int s_armed = -1;

/* MUST THE ENGINE'S CACHED SLANT BE SUPPRESSED THIS FRAME -- the
   structure-shadow gate's input, written at the bottom of the unit pass and
   read (and cleared) by the publisher at the top of the NEXT frame.

   IT WAS CALLED `s_ssPainter` AND ASKED "DID ANYTHING PAINT ONE", which is a
   different question and the fourth wrong one this gate has asked. Painting
   decides whether a SHADOW appears; it does not decide whether GARBAGE does,
   and inside a key-filled viewport the engine's cached slant is always
   garbage -- MEASURED 2026-09-18, 8779 px of opaque teal on four structures.
   So the first term is now the key-fill itself. Render thread only, one writer, one reader, same
   thread: `tagpu_native_frame` is the whole of both. Not volatile and not
   published anywhere else, because the value that crosses to the game thread is
   `tagpu_owndraw.c`'s `g_ssSkip`, which the publisher writes from this.

   It is deliberately the painters' OWN conditions, quoted where they live
   rather than restated up there -- that is the bug this variable exists to
   stop repeating. */
static int s_ssSuppress = 0;
static char   s_type[32] = "armcom";
static int    s_wrecks = 0;            /* "wrecks" token present            */
static int    s_ss     = 1;            /* 2x supersample (tagpu_ss.off)     */
static int    s_subpix = 1;            /* sub-pixel motion (tagpu_subpix.off)*/
static int    s_spxlog = 0;            /* anchor filmstrip (tagpu_spxlog.on) */
/* G16 step 8 removed the pose-race guard, the rest-equality detector, the
   reconstruction and `posewatch` along with the CPU emitters that were the
   only things that reached them, and with them the levers `tagpu_posefix.off`,
   `tagpu_posewatch.on` and `tagpu_poserecon.on`. Nothing reads `prim+0x22` any
   more, so the race they detected and worked around cannot happen — the pose
   comes off the FIELDS. gpu-status.md §2.9 keeps the history. */
static int    s_nano   = 1;            /* build-state look (tagpu_nano.off)  */
static GLuint s_prog, s_vao, s_vbo, s_fbo, s_colTex, s_depTex, s_palTex;
#define TAGPU_SS_MAX 4                 /* the most we will supersample by (G17b) */
static int    s_devres = 0;            /* the world at device res — OPT IN, tagpu_devres.on */
static TAGPU_WORLDTGT s_wt;            /* the world target, published per frame */
static int    s_wtHave = 0;
/* ---- `tagpu_selgeom.on` IS GONE, AND THE VULKAN LANE IS ITS `main` SETTING ----
   The lever existed because a GL line is one pixel wide IN THE BUFFER IT IS
   DRAWN INTO and the driver clamps an aliased line's width to 1 (measured:
   `glLineWidth(ss*3)` draws pixel-identically to `glLineWidth(ss)`), so at
   `ss > 1` the rect resolved to a half-lit smear; armed, each edge was emitted
   as two triangles with a width we chose. None of that survives the GL draw
   half [landing 11-3]: the Vulkan lane draws the rect as geometry always, and
   `tagpu_vk_world.h` records that `selAt1x` is not expressible there -- so the
   lane is permanently in what GL called `selgeom main`. A lever whose only
   reader is deleted does not become harmless by being left in: it becomes a
   file a session can arm and then measure nothing from. The widths it could
   set, and the 2026-09-11 coverage measurement behind them, are in
   ui-markers.md. */
static GLuint s_fogTex, s_fogLutTex, s_cprog, s_cvao, s_cvbo;
/* the 256-byte fog shade table as it was last uploaded to s_fogLutTex, for a
   backend that cannot read a GL texture (Phase G / G19e, tagpu_native.h) */
static unsigned char s_fogLutBytes[256];
static int s_fogLutHave;
/* whether this frame's world FBO pass is actually scissored to the viewport
   (Phase G / G19e, tagpu_native.h) */
static int s_scissorOn;
static GLuint s_fbo2, s_colTex2, s_depTex2, s_dprog;
static GLint  s_uGame, s_uShadow, s_uAlpha, s_uFog, s_uFogOrg, s_uFogDim,
              s_uScafOn, s_uScafP;
static GLint  s_uWaterT, s_uWaterMode, s_uDigT;
static GLint  s_uNanoOn, s_uNanoT, s_uNanoC;
static GLint  s_uLit, s_uLambert, s_uSun, s_uAmb, s_uNorm;  /* Classic++ lighting */
static GLint  s_uRestored;                          /* Classic++: the unit atlas's twin */
static GLint  s_uOffset, s_uSS, s_uZoom, s_uZoomC, s_uZoomF, s_uZoomCF, s_uDepthScale;
static GLint  s_uCKey = -1, s_uCSurfSz = -1, s_uCVp = -1;  /* composite: the key */
static GLint  s_uCCurs = -1;                               /* G17c: the cursor rect */
static float  s_zoom = 1.0f;
static TAGPU_PDVIEW s_pv;          /* the posed pass's view, filled once per
                                      frame and reused by the build-ghost pass */
static unsigned s_pvFrame = 0xFFFFFFFFu;   /* the frame that fill belongs to —
                                      never a real frame, so a ghost armed on
                                      the very first one cannot draw against a
                                      zeroed view */
static int    s_fboW = 0, s_fboH = 0, s_fboSS = 0;
static int    s_palInit = 0;
static int    s_fogCols = 0, s_fogRows = 0, s_fogOrgX = 0, s_fogOrgY = 0;
static int    s_fogCells = 0;         /* the ALLOCATION's cell count (= cols*rows) */
/* Frames drawn zoomed out, or from an unacknowledged eye, whose packet carried
   no WIDE fog grid — so the outer ring falls back to the engine's 1x grid and
   taFog's clamp. This is tagpu_fogwide's old `bare=` counter, moved here with
   the grid (landing 4b): it must read 0 unless `tagpu_fogwide.off` is armed. */
static unsigned s_fogBare = 0;
static const unsigned short* s_fogGrid = NULL;
static int    s_fogLut = 0;   /* grey remap uploaded this frame (logged) */
/* world origin of fog grid cell 0 on one axis: the builder's rounded eye>>5
   turned back into world px, i.e. 32*col0 + 16 (0x4843C0 head, 0x4848E0).
   `%` truncating toward zero is DELIBERATE, not a floor-mod bug: the builder
   computes col0 as ((eye + (sign & 31)) >> 5) - 1, which is C's truncating
   division, so a floor-based remainder would disagree with the engine for a
   negative eye (eye = -20: engine origin -16, floor would say -48). */
static GLint  s_uCast;                 /* the caster's three numbers (G14i) */
static TAGPU_SHADOWU s_shU;            /* the shadow read-back uniforms      */
static float  s_verts[MAXNV * NVST];
/* NOTHING WRITES THIS ON THE SURVIVING LANE, AND THAT IS A REAL GAP -- read it
   before believing any note that says the selection rect is ours.

   It was written once, at the bottom of the GL unit pass, meaning "every
   selection box this frame owed was actually emitted"; `tagpu_markown.c`'s
   mark_selbox reads it through `tagpu_native_selbox_complete()` and suppresses
   the ENGINE's own box draw only when it is 1. That store sat below the
   `!gl_draws` hand-over return, so on the Vulkan lane it never ran and this has
   been permanently 0 since landing 4b; landing 11-3 deleted the store with the
   rest of that pass, which changed nothing.

   THE CONSEQUENCE: on this lane we draw no selection rect at all and never
   suppress, so the engine draws every box itself -- at its UNZOOMED projection,
   which at zoom != 1 is the visible scatter the GL pass existed to avoid. No
   `tagpu_vk_*` file contains selection-rect code. This deviation is UNMEASURED.

   The variable and its accessor are kept rather than deleted because they are
   the seam a Vulkan selection rect would fill: write this from the pass that
   emits the boxes and the engine stands down exactly as it did on GL. */
static volatile int s_selComplete = 0;

/* material constants copied per frame from render3do's calibration */
static const float SH_V[3] = { 0.0f, 0.8944f, -0.4472f };
static const float SH_L[3] = { -0.35f, 0.80f, -0.49f };

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"     /* frame px, frame py, depth enc  */
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aFC;\n"      /* flat idx/255, tex ck/255       */
    "layout(location=3) in float aShade;\n"  /* LUT row / 31                   */
    "layout(location=4) in vec2 aWorld;\n"   /* world x, projected world z     */
    "layout(location=5) in float aVY;\n"     /* posed model height, elev units */
    "layout(location=6) in vec3 aNrm;\n"     /* posed face normal, map space   */
    "uniform vec2 uGame;\n"                  /* game_width, game_height        */
    "uniform vec2 uOffset;\n"                /* shadow pass shift, px          */
    "uniform float uZoom;\n"                 /* G12d partial-zoom demo         */
    "uniform vec2 uZoomC;\n"                 /* zoom centre, game px           */
    "uniform float uDepthScale;\n"           /* > every key in use this frame  */
    "uniform vec3 uCast;\n"                  /* altitude, ground + throw, sv   */
    "out vec2 vUV; flat out vec2 vFC; flat out float vShade; out vec2 vWorld;\n"
    "out float vEnc; out float vVY; flat out vec3 vNrm; out vec3 vShW;\n"
    "void main(){\n"
    "  vec2 p = (aPos.xy + uOffset - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vFC = aFC; vShade = aShade; vWorld = aWorld; vEnc = aPos.z;\n"
    "  vVY = aVY; vNrm = aNrm;\n"
    /* Classic++ shadows: the vertex's SHADOW-SPACE point, derived here rather
       than carried (renderers.md 2.11): real z = projected z + (altitude +
       height)/2, the height the ground plus the throw plus the model height
       scaled by the length rule. The same expression tagpu_shadow.c's depth
       program evaluates, so a unit's own shadow lookup lands on its own
       caster (self-shadowing, unit on unit). */
    "  vShW = vec3(aWorld.x, uCast.y + uCast.z * aVY, aWorld.y + (uCast.x + aVY) * 0.5);\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec2 vFC; flat in float vShade; in vec2 vWorld;\n"
    "in float vEnc; in float vVY; flat in vec3 vNrm; in vec3 vShW;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uLUT;\n"
    "uniform sampler2D uPal;\n"              /* 256x1 RGBA live palette        */
    "uniform sampler2D uAtlasRGB;\n"         /* Classic++: the atlas's restored twin, mipped */
    "uniform int uRestored;\n"               /* 1 = sample it where its alpha says so */
    TAGPU_GLSL_SCAF_UNIFORMS                  /* G12a scaffold, R8, viewport    */
    TAGPU_GLSL_FOG_UNIFORMS
    "uniform int uShadow;\n"
    "uniform float uAlpha;\n"
    "uniform float uWaterT;\n"              /* vy <= this is under water      */
    "uniform int uWaterMode;\n"             /* 1 erase (enemy), 2 tint (own)  */
    "uniform float uDigT;\n"                /* vy <= this is below ground     */
    "uniform int uNanoOn;\n"                /* build-state staging on         */
    "uniform float uNanoT;\n"               /* height threshold, depth bytes  */
    "uniform vec3 uNanoC;\n"                /* cAbove, cBand, cBelow          */
    TAGPU_GLSL_FOG_FN
    TAGPU_GLSL_LIGHT_UNIFORMS
    TAGPU_GLSL_SHADOW_UNIFORMS
    TAGPU_GLSL_LIGHT_FN
    "void main(){\n"
    "  float idx;\n"
    /* the shadow point's screen derivatives FIRST, while every fragment of
       the quad is still running -- the discards below end that (tagpu_glsl.h) */
    "  vec3 taSx = dFdx(vShW), taSy = dFdy(vShW);\n"
    /* Classic++: the twin is sampled HERE, before any discard, because it is
       mipmapped and its implicit derivatives are only defined while every
       fragment of the quad is still running (the R8 sample has no mips and
       never cared). Zero for a flat face, and zero when the switch is off. */
    "  vec4 t = vec4(0.0);\n"
    "  if (vUV.x < 0.0) { idx = vFC.x; }\n"
    "  else {\n"
    "    idx = texture(uAtlas, vUV).r;\n"
    "    if (uRestored == 1) t = texture(uAtlasRGB, vUV);\n"
    "    if (abs(idx - vFC.y) < 0.5/255.0) discard;\n"
    "  }\n"
    /* scaffold occlusion: nearer stamped rows hide this fragment (one copy
       of the rule, shared with the effects shader: tagpu_glsl.h) */
    TAGPU_GLSL_SCAF_TEST
    /* fog before the shadow return, or a hidden unit keeps its shadow */
    TAGPU_GLSL_FOG_DISCARD
    /* waterline / digger clipping (shadows-cloak.md §3 depth-bias rules):
       the engine erases or tints composite pixels whose depth (= vertex
       height + bias) is at or below the water line; shadows are always
       erased there. Our per-fragment height is the interpolated model y. */
    "  if (vVY <= uDigT) discard;\n"
    "  if (uShadow == 1) { if (vVY <= uWaterT) discard;\n"
    "                      frag = vec4(0.0, 0.0, 0.0, 0.5); return; }\n"
    /* Classic: the per-face shade row through the 32-row PALETTE.SHD LUT.
       Classic++ (uLit) skips it and lights the resolved colour per fragment
       below, from the face normal -- the same light with the 32-row
       quantisation taken out (renderers.md 1, Units row) */
    "  if (uLit == 0)\n"
    "    idx = texelFetch(uLUT, ivec2(int(idx*255.0+0.5), int(vShade*31.0+0.5)), 0).r;\n"
    /* build-state (nanoframe) recolour, engine 0x458D30 semantics
       (build-state.md): classify the fragment by its composite DEPTH byte —
       the model height plus 0x32 — against the threshold that sweeps with the
       build, then erase it, keep the texture, or paint one of the two animated
       blues. An ERASED fragment is DISCARDED, and that is a deliberate
       divergence from the engine, which keeps the whole model's heights in
       the depth plane whatever the fill has reached and tests the wireframe
       against them, so its back edges do not show. We cannot have both: the
       engine's depth plane is PER SPRITE, ours is the one shared GL depth
       buffer, so an erased fragment that writes depth is an invisible
       occluder for everything drawn after it -- and at p >= 201, the first
       fifth of every build, nano_stage erases all but a thin band, so that
       occluder is very nearly the whole model. It cost a factory its own far
       wall against the unit on its pad (the cargo is given the parent's
       encBase, so the two sort by md alone), and it culled the nanolathe
       spray, later-indexed units and hires bodies the same way. Losing the
       wireframe's hidden-line removal is the smaller of the two errors;
       getting it back needs per-sprite isolation (a stencil pass), which this
       landing did not do. */
    "  bool band = false;\n"
    "  if (uNanoOn == 1) {\n"
    "    float nd = vVY + 50.0;\n"
    "    float nc = (nd < uNanoT - 4.0) ? uNanoC.z\n"
    "             : (nd < uNanoT)       ? uNanoC.y : uNanoC.x;\n"
    "    if (nc < -1.5) discard;\n"
    "    if (nc > -0.5) { idx = nc; band = true; }\n"
    "  }\n"
    "  int pi = int(idx*255.0+0.5);\n"
    "  vec3 rgb;\n"
    /* Classic++: the restored texel where the lazy restore has painted it
       (alpha 1 -- tagpu_gaf.h; the twin sampled trilinear with its mips, the
       lab's LAB_UNIT_FS uUndither branch), the palette's colour for a flat
       face, a nanoframe band or a texel not yet restored, then the lab's
       lambert on either (renderers.md 2.11) and the grey band as the RGB rule
       (2.6). The hole stays the index test above: the twin's alpha is 0 at a
       keyed texel too, but the index compare is what keeps it out of the
       depth buffer. Divided by its alpha: a keyed texel is (0, 0, 0, 0), so a
       bilinear sample beside one is premultiplied by its coverage and would
       draw a dark ring where the lab's shader (which takes t.rgb as is) does.
       Classic: the index remap, as the engine does it. */
    "  if (uLit == 1) {\n"
    "    rgb = (t.a > 0.5 && !band) ? t.rgb / t.a\n"
    "        : texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n"
    "    rgb *= taLambert(uLambert == 1 ? vNrm : vec3(0.0, 1.0, 0.0), vShW, taSx, taSy);\n"
    TAGPU_GLSL_FOG_GREY_RGB("rgb")
    "  } else {\n"
    TAGPU_GLSL_FOG_SHADE("pi")
    "    rgb = texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n"
    "  }\n"
    /* engine water table (prog+0xD0): r/2, g/2, b/2+0x32 */
    "  if (vVY <= uWaterT) {\n"
    "    if (uWaterMode == 1) discard;\n"
    "    rgb = rgb * 0.5 + vec3(0.0, 0.0, 50.0/255.0);\n"
    "  }\n"
    /* the FBO is PREMULTIPLIED: additive content (effects flashes) can then
       ride the same composite as (rgb, alpha 0) */
    "  frag = vec4(rgb * uAlpha, uAlpha);\n"
    "}\n";

/* G16 step 5: the posed program is a TWIN of this one — its own vertex stage,
   this exact fragment stage. Handing over the source rather than letting
   tagpu_posedraw.c carry a copy is what keeps the two from drifting in the half
   of the pipeline step 5 does not replace. */
const char* tagpu_native_unit_fs(void) { return FS; }

/* composite: the game-res FBO over the frame, NEAREST (game-res look kept) */
static const char* CVS =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"
    "out vec2 uv;\n"
    "void main(){ uv = vec2(p.x, p.y);\n"
    "  gl_Position = vec4(p.x*2.0-1.0, 1.0-p.y*2.0, 0.0, 1.0); }\n";
/* THE COMPOSITE INVERTS ONCE WE OWN THE TERRAIN (G13b).
   Until then our passes only ever covered sprites, so the rule was "drop our
   empty pixels and let the engine's frame show". Terrain covers the whole
   viewport, so that rule would hide everything the engine still draws inside
   it — health bars, nanoframe wireframes, the build cursor, chat, dialogs.
   In place of its terrain blit, tagpu_terrown.c fills the viewport rect of the
   engine's offscreen with one palette index; every OTHER index there is by
   construction something the engine drew afterwards, so we discard OUR
   fragment at those pixels and its own already-drawn frame shows through.
   uKey < 0 keeps the pre-G13b behaviour exactly. texelFetch, not texture(),
   because the surface is an INDEX texture whose filter state belongs to
   cnc-ddraw and may be linear — interpolated palette indices are garbage. */
static const char* CFS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uSurf;\n"
    "uniform sampler2D uPal;\n"
    "uniform ivec2 uSurfSz;\n"
    "uniform vec4 uVp;\n"          /* the rect the key fill covers, game px */
    "uniform int uKey;\n"
    "uniform vec4 uCurs;\n"        /* G17c: the engine's cursor rect, game px */
    "void main(){\n"
    "  vec4 c = texture(uTex, uv);\n"
    "  bool empty = c.a < 0.004 && max(max(c.r, c.g), c.b) < 0.004;\n"
    "  vec2 px = uv * vec2(uSurfSz);\n"
    /* Only inside the viewport is the engine's frame our key fill. Outside it
       the frame is UI we never touched, so the old rule stands there — and a
       UI pixel that happens to BE the key index can never be mistaken for it. */
    "  if (uKey >= 0 && px.x >= uVp.x && px.x < uVp.x + uVp.z &&\n"
    "                   px.y >= uVp.y && px.y < uVp.y + uVp.w) {\n"
    "    ivec2 p = clamp(ivec2(px), ivec2(0), uSurfSz - 1);\n"
    /* G17c: THE CURSOR'S RECT COUNTS AS KEY. The engine blits its cursor into
       the back buffer inside the flip, after everything we observe, so those
       pixels are not the key and the discard above would let them through —
       under the cursor the GL UI renderer already draws its own into the sharp
       layer, and the screen would carry two. The rect is empty (w = h = 0)
       unless tagpu_gui_cursor_own says ours is being drawn this frame, so this
       is inert whenever the engine's cursor is the one on screen.
       The cost is the engine's own in-viewport pixels inside that rect for one
       frame — health bars, a nanoframe, chat — which its cursor had already
       covered in the frame we are compositing. */
    "    bool cur = px.x >= uCurs.x && px.x < uCurs.x + uCurs.z &&\n"
    "               px.y >= uCurs.y && px.y < uCurs.y + uCurs.w;\n"
    "    if (!cur && int(texelFetch(uSurf, p, 0).r * 255.0 + 0.5) != uKey) discard;\n"
    /* THE KEY FILL MUST NEVER REACH THE SCREEN, NOT EVEN A FRACTION OF IT.
       This pixel of the engine's frame is the raw key — index 254, a bright
       cyan — so `c` has to land on BLACK here, the colour the engine paints
       for "no world", rather than be blended over what is behind us.

       Emitting `c` and letting the blend do it only works when c.a is 1. Every
       partially covered pixel (the FBO is premultiplied and the 2x downsample
       gives fractional alpha along any edge terrain does not reach — the map
       boundary is a full-length one) would otherwise come out as
       `c.rgb + (1 - c.a) * key`: a cyan-tinted line at exactly the zoom levels
       where the world's edge lands off the pixel grid. Opaque `c.rgb` is that
       same composite against black, and it subsumes the empty case (c.rgb is
       then 0, which is the black this used to special-case). */
    "    frag = vec4(c.rgb, 1.0); return;\n"
    "  }\n"
    "  if (empty) discard;\n"
    "  frag = c;\n"
    "}\n";

/* downsample: 2x FBO -> 1x FBO, same orientation, LINEAR sampler at the 1x
   texel centres = exact 2:1 box filter; fractional edge alpha is the AA */
static const char* DVS =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"
    "out vec2 uv;\n"
    "void main(){ uv = p;\n"
    "  gl_Position = vec4(p.x*2.0-1.0, p.y*2.0-1.0, 0.0, 1.0); }\n";
static const char* DFS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag; uniform sampler2D uTex;\n"
    "void main(){ frag = texture(uTex, uv); }\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
               nlog("native: shader FAILED:"); nlog(lg); s_state = 2; }
    return sh;
}

static void tex2d(GLuint* t, GLenum filt)
{
    glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void init_gl(void)
{
    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glDepthFunc  = (PFN_DEPTHFUNC) getgl("glDepthFunc");
    x_glDisable    = (PFN_DISABLE)   getgl("glDisable");
    x_glBlendFunc  = (PFN_BLENDFUNC) getgl("glBlendFunc");
    x_glUniform1f  = (PFN_UNIFORM1F) getgl("glUniform1f");
    x_glUniform2f  = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glUniform3f  = (PFN_UNIFORM3F) getgl("glUniform3f");
    x_glUniform4f  = (PFN_UNIFORM4F) getgl("glUniform4f");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glClearBufferfv = (PFN_CLEARBUFFERFV)getgl("glClearBufferfv");
    x_glDepthMask  = (PFN_DEPTHMASK) getgl("glDepthMask");
    x_glScissor    = (PFN_SCISSOR)   getgl("glScissor");
    x_glLineWidth  = (PFN_LINEWIDTH) getgl("glLineWidth");
    x_glBlitFramebuffer = (PFN_BLITFB) getgl("glBlitFramebuffer");
    x_glStencilFunc = (PFN_STENCILFUNC)getgl("glStencilFunc");
    x_glStencilOp   = (PFN_STENCILOP)  getgl("glStencilOp");
    x_glColorMask   = (PFN_COLORMASK)  getgl("glColorMask");
    if (!x_glDrawArrays || !x_glDepthFunc || !x_glDisable || !x_glBlendFunc ||
        !x_glUniform1f || !x_glUniform2f || !x_glUniform4f || !x_glActiveTexture ||
        !x_glClearBufferfv || !x_glDepthMask ||
        !x_glStencilFunc || !x_glStencilOp || !x_glColorMask)
    { nlog("native: missing GL proc"); s_state = 2; return; }

    GLuint vs = mksh(GL_VERTEX_SHADER, VS), fs = mksh(GL_FRAGMENT_SHADER, FS);
    if (s_state == 2) return;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs); glLinkProgram(s_prog);
    GLint ok = 0; glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { nlog("native: link FAILED"); s_state = 2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uGame   = glGetUniformLocation(s_prog, "uGame");
    s_uOffset = glGetUniformLocation(s_prog, "uOffset");
    s_uShadow = glGetUniformLocation(s_prog, "uShadow");
    s_uWaterT    = glGetUniformLocation(s_prog, "uWaterT");
    s_uWaterMode = glGetUniformLocation(s_prog, "uWaterMode");
    s_uDigT      = glGetUniformLocation(s_prog, "uDigT");
    s_uNanoOn    = glGetUniformLocation(s_prog, "uNanoOn");
    s_uLit       = glGetUniformLocation(s_prog, "uLit");
    s_uLambert   = glGetUniformLocation(s_prog, "uLambert");
    s_uSun       = glGetUniformLocation(s_prog, "uSun");
    s_uAmb       = glGetUniformLocation(s_prog, "uAmb");
    s_uNorm      = glGetUniformLocation(s_prog, "uNorm");
    s_uNanoT     = glGetUniformLocation(s_prog, "uNanoT");
    s_uNanoC     = glGetUniformLocation(s_prog, "uNanoC");
    s_uAlpha  = glGetUniformLocation(s_prog, "uAlpha");
    s_uFog    = glGetUniformLocation(s_prog, "uFog");
    s_uFogOrg = glGetUniformLocation(s_prog, "uFogOrg");
    s_uFogDim = glGetUniformLocation(s_prog, "uFogDim");
    s_uScafOn = glGetUniformLocation(s_prog, "uScafOn");
    s_uScafP  = glGetUniformLocation(s_prog, "uScafP");
    s_uSS     = glGetUniformLocation(s_prog, "uSS");
    s_uZoom   = glGetUniformLocation(s_prog, "uZoom");
    s_uZoomC  = glGetUniformLocation(s_prog, "uZoomC");
    s_uZoomF  = glGetUniformLocation(s_prog, "uZoomF");
    s_uZoomCF = glGetUniformLocation(s_prog, "uZoomCF");
    s_uDepthScale = glGetUniformLocation(s_prog, "uDepthScale");
    s_uRestored   = glGetUniformLocation(s_prog, "uRestored");
    s_uCast       = glGetUniformLocation(s_prog, "uCast");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "uLUT"),   1);
    tagpu_shadow_locate(s_prog, &s_shU);     /* names the map's two units */
    glUniform1i(glGetUniformLocation(s_prog, "uPal"),   2);
    glUniform1i(glGetUniformLocation(s_prog, "uScaf"),  3);
    glUniform1i(glGetUniformLocation(s_prog, "uFogGrid"), 4);
    glUniform1i(glGetUniformLocation(s_prog, "uFogLUT"),  5);
    /* unit 8: the hires pass binds its own textures on 6 and 7 between the
       shadow and body draws (tagpu_hires_draw.c) and restores only unit 0 */
    glUniform1i(glGetUniformLocation(s_prog, "uAtlasRGB"), 8);
    glUseProgram(0);

    GLuint cvs = mksh(GL_VERTEX_SHADER, CVS), cfs = mksh(GL_FRAGMENT_SHADER, CFS);
    if (s_state == 2) return;
    s_cprog = glCreateProgram();
    glAttachShader(s_cprog, cvs); glAttachShader(s_cprog, cfs); glLinkProgram(s_cprog);
    glGetProgramiv(s_cprog, GL_LINK_STATUS, &ok);
    if (!ok) { nlog("native: cprog link FAILED"); s_state = 2; return; }
    glDeleteShader(cvs); glDeleteShader(cfs);
    glUseProgram(s_cprog);
    glUniform1i(glGetUniformLocation(s_cprog, "uTex"), 0);
    glUniform1i(glGetUniformLocation(s_cprog, "uSurf"), 1);
    glUniform1i(glGetUniformLocation(s_cprog, "uPal"),  2);
    s_uCKey = glGetUniformLocation(s_cprog, "uKey");
    s_uCSurfSz = glGetUniformLocation(s_cprog, "uSurfSz");
    s_uCVp = glGetUniformLocation(s_cprog, "uVp");
    s_uCCurs = glGetUniformLocation(s_cprog, "uCurs");
    /* a GLSL uniform defaults to 0, and uKey 0 is an ACTIVE key — the whole
       frame would invert against palette index 0. Default it off explicitly. */
    if (s_uCKey >= 0) glUniform1i(s_uCKey, -1);
    glUseProgram(0);

    GLuint dvs = mksh(GL_VERTEX_SHADER, DVS), dfs = mksh(GL_FRAGMENT_SHADER, DFS);
    if (s_state == 2) return;
    s_dprog = glCreateProgram();
    glAttachShader(s_dprog, dvs); glAttachShader(s_dprog, dfs); glLinkProgram(s_dprog);
    glGetProgramiv(s_dprog, GL_LINK_STATUS, &ok);
    if (!ok) { nlog("native: dprog link FAILED"); s_state = 2; return; }
    glDeleteShader(dvs); glDeleteShader(dfs);
    glUseProgram(s_dprog);
    glUniform1i(glGetUniformLocation(s_dprog, "uTex"), 0);
    glUseProgram(0);

    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo); glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, NVST * 4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, NVST * 4, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, NVST * 4, (void*)20);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, NVST * 4, (void*)28);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, NVST * 4, (void*)32);
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, NVST * 4, (void*)40);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, NVST * 4, (void*)44);
    glBindVertexArray(0);

    const float quad[] = { 0,0, 1,0, 0,1, 1,1 };
    glGenVertexArrays(1, &s_cvao); glBindVertexArray(s_cvao);
    glGenBuffers(1, &s_cvbo); glBindBuffer(GL_ARRAY_BUFFER, s_cvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
    glBindVertexArray(0);

    tex2d(&s_palTex, GL_NEAREST);
    tex2d(&s_fogTex, GL_NEAREST);
    tex2d(&s_fogLutTex, GL_NEAREST);
    tex2d(&s_colTex, GL_NEAREST);
    tex2d(&s_depTex, GL_NEAREST);
    tex2d(&s_colTex2, GL_LINEAR);          /* LINEAR = the 2:1 box filter */
    tex2d(&s_depTex2, GL_NEAREST);
    glGenFramebuffers(1, &s_fbo);
    glGenFramebuffers(1, &s_fbo2);
    s_state = 1;
    nlog("native: GL ready (G12b pass)");
}


static int name_ieq(const char* a, const char* b)
{
    int i;
    for (i = 0; i < 31 && a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return a[i] == 0 || a[i] <= ' ';
}

static int type_match(const char* def)
{
    if (name_ieq("all", s_type)) return 1;
    return name_ieq(def + 0x00, s_type) || name_ieq(def + 0x20, s_type) ||
           name_ieq(def + 0x80, s_type);
}

/* Is this unit natively owned RIGHT NOW? (armed + type)

   UNDER-CONSTRUCTION UNITS ARE OURS TOO, and that is not a detail: everything
   the engine still draws inside the viewport lands at the UNZOOMED projection,
   because the composite scales OUR fragments and passes its frame through 1:1.
   A nanoframe left on the composite path therefore sits at its 1x pixels while
   the world moves under it — it slides away from the factory or the commander
   building it the moment the zoom is not 1 (the bug this pass now closes), and
   with owndraw "all" in force it is not even the engine's own look any more:
   the rasterise is skipped, so 0x458DD0 recolours an empty composite and only
   its wireframe survives. */
/* Is this unit's ModelId one model_root() will resolve? The bound is
   UNITINFOCount, the count 0x42DBCA itself loops to. Factored out so
   `tagpu_native_owns_unit` and `model_root` cannot drift apart about it —
   they are two halves of one answer — and side-effect free, because
   model_root's BADMODELID counter is a render-thread diagnostic and
   owns_unit is called from the game thread. */
static int model_id_ok(const char* ta, const char* u, unsigned* out_mid)
{
    unsigned mid = 0, n;
    int ok = 0;
    if (ptr_ok(ta) && ptr_ok(u)) {
        mid = *(const unsigned short*)(u + U_MODELID);
        n   = *(const unsigned*)(ta + OFF_UDEFCOUNT);
        ok  = (mid != 0 && n != 0 && n <= 0x10000u && mid < n);
    }
    if (out_mid) *out_mid = mid;
    return ok;
}

int tagpu_native_owns_unit(const char* u)
{
    if (s_armed != 1) return 0;
    const char* def = *(const char* const*)(u + U_TYPE);
    if (!ptr_ok(def)) return 0;
    if (!type_match(def)) return 0;
    /* A UNIT WHOSE MODEL WE CANNOT RESOLVE IS NOT OURS — ALL of it stays the
       engine's. This predicate is the one place that decision is made: the
       gather skips what it refuses, tagpu_overlay.c leaves the engine's
       composite unwiped, tagpu_mark.c leaves the bar on the engine's anchor,
       and tagpu_markown.c leaves the engine's own selection rect alone. Get it
       wrong in one direction and a unit is drawn twice; wrong in the other and
       it is INVISIBLE, or — the case this line was added for — it keeps its
       sprite and silently loses its selection box for ever.

       0x46A530 has NO ModelId test: its only early-out is the SelBoxes flag at
       0x46A544, and it indexes MODEL_PTRS[ModelId] at 0x46A56B and bounds it
       through 0x4CB650 unconditionally [BINARY-VERIFIED 2026-09-10]. So the
       engine DOES draw a box for a unit we cannot bound, and suppressing it
       while our own loop skips the unit leaves that unit unmarked every frame.
       (This corrects the claim, made here and in ui-markers.md on 2026-09-09,
       that a unit with no model is "owed nothing" — it is owed the engine's.) */
    {
        const char* ta = *(const char* const*)TA_MAINPP;
        if (!model_id_ok(ta, u, NULL)) return 0;
    }
    /* ...but a unit UNDER CONSTRUCTION only while we can actually take the
       whole of it over. Claiming one means the engine's blit-time build-state
       effect (0x458DD0) must be detoured away and we must stage the look
       ourselves; if either half is missing the engine stamps its recolour and
       wireframe at the unzoomed 1x projection and the two fight. Failing back
       to "the engine owns nanoframes" is the pre-G13l behaviour: the scaffold
       drifts at zoom != 1, which is a known bug, where a half-armed state is
       an unknown one. Costs one float read per candidate and only when the
       detour is absent or the tagpu_nano.off lever is set. */
    {
        extern int tagpu_owndraw_buildfx_armed(void);
        if ((!s_nano || !tagpu_owndraw_buildfx_armed()) &&
            !IsBadReadPtr(u, 0x108) &&
            *(const float*)(u + U_NANO) > 0.0f) return 0;
    }
    return 1;
}

int tagpu_native_owns_obj(unsigned int obj3do)
{
    if (s_armed != 1) return 0;
    if (!ptr_ok((const void*)(size_t)obj3do)) return 0;
    const char* u = *(const char* const*)((size_t)obj3do + O3_THISUNIT);
    if (!ptr_ok(u)) return 0;
    int own = tagpu_native_owns_unit(u);
    static int diag = 0;
    if (own && diag < 6) {
        diag++;
        const char* def = *(const char* const*)(u + U_TYPE);
        char b[160];
        _snprintf(b, sizeof b, "native OWNS: u=%p def=%p \"%.12s\"/\"%.12s\"/\"%.12s\" type=%s",
                  (const void*)u, (const void*)def,
                  ptr_ok(def) ? def + 0x00 : "?", ptr_ok(def) ? def + 0x20 : "?",
                  ptr_ok(def) ? def + 0x80 : "?", s_type);
        nlog(b);
    }
    return own;
}

/* append one unit's triangles; returns new vertex count */
/* ---- whole-3DO-tree model-space AABB (engine FUN_004CB650 equivalent) ----
   walked over the raw Model3DONode template (verts 16.16, child offsets
   accumulate); cached per root node. Feeds the native selection rect. */
/* THE UNIT'S MODEL TEMPLATE, BOUNDS-CHECKED — and the bound is the whole
   safety argument, deliberately, because a readability PROBE would not be one.

   `main+0x14377` is the array of Model3DONode* the engine indexes with the
   unit record's ModelId, and `UNITINFOCount` at `main+0x1438F` is its LENGTH,
   not merely a related number. [BINARY-VERIFIED 2026-09-09]: the count is set
   first and the table allocated as `count * 4` bytes at 0x42D68A/0x42D693; the
   load loop then runs `i = 1 .. count-1` and writes EVERY slot (0x42D7A2), so
   no slot in range is left holding the allocator's garbage ONCE THE LOAD HAS
   FINISHED -- 0x4D83B0 does not zero, and the count at main+0x1438F and the
   table pointer at 0x42D6AA are both live before that loop runs, so a pass
   reading during the load itself can see an uninitialised slot. That window is
   not closed by this bound; it is empty for a different reason, which is that
   no unit record exists to name a ModelId until the load is over; and the
   loop 0x42DBCA runs the same range, freeing each slot and nulling it
   (0x42DC15) before freeing the table and nulling the pointer (0x42DCB6 /
   0x42DCD8). The unit defs at `main+0x1439B` run alongside at stride 0x249,
   which is why one count serves both. Slot 0 is the "no model" entry the
   engine neither fills nor frees, and this file's dead-unit test already reads
   ModelId == 0 that way.

   So `1 <= mid < count` is not a heuristic: it is the exact set of slots the
   engine writes, and every one of them holds NULL or a template of the level
   that is loaded.

   NOTHING BOUNDS THE u16 IN THE UNIT RECORD, and it is read from a Mode B
   object (thread-safe-destruction.md §2): the unit array is a fixed array the
   engine recycles in place and never returns to the heap, so a slot that died
   between this frame's gather and this read is mapped but holds another unit's
   values — or, mid-rewrite, a torn one. That contract says a stale read is at
   worst a wrong frame, and it holds only if every value taken from such an
   object is validated AS DATA before it is used. This one was not: an
   unbounded index into a bounded table addresses arbitrary memory, and the
   pointer read from there passes a range test about as often as not, so the
   caller then walks bytes that are not a model at all. That is the read behind
   the 2026-09-08 access violation in aabb_walk.

   Bounded, the worst case is back inside the Mode B contract by construction
   rather than by luck: a stale index costs one frame of another type's AABB
   and can never fault. A live template has an internally consistent tree,
   which is why aabb_walk below needs no per-node probing — see the note there.

   THE ONE THING THIS RESTS ON that it does not itself establish: the templates
   must still be alive while we read them. They are freed by 0x42DB90 in the
   level-teardown cascade, and the render thread is held out of that window by
   tagpu_reclaim's teardown wrap — whose wait has a 1 s timeout, after which
   the cascade proceeds with a render pass still running. That hole is
   pre-existing, is recorded in thread-safe-destruction.md §6a as an open item,
   and is the reason this function is not the last word on the subject. */
static unsigned s_badModelId;          /* refused here, reported with the frame */

/* THE MODEL ID ARRIVES BOUNDED (frame packet exchange, landing 3). The
   publisher drops a unit's `model_id` unless it is inside UNITINFOCount, so
   what reaches here is already a slot the engine writes; the count is carried
   beside it and re-checked, because this is the one place an index becomes an
   address. The table itself, and the templates in it, are freed by the level
   teardown (0x42DBCA/0x42DCB6) and held out of that window by tagpu_reclaim's
   teardown wrap — the fence, unchanged by this landing. */
static const char* model_root(const TAGPU_PACKET* pk, unsigned mid)
{
    /* THE TABLE'S BASE IS READ LIVE, EVERY CALL, and that is the point: the
       level teardown frees it at 0x42DCCB and NULLS main+0x14377 at 0x42DCD8,
       so the null is what refuses this walk once the cascade has run. A copy
       taken at publish time and held for a frame reads straight past it
       (landing review, 2026-09-12). The bound on `mid` is the packet's — the
       publisher dropped anything outside UNITINFOCount — and the LIFETIME is
       tagpu_reclaim's teardown wrap, unchanged. */
    const char* ta = *(const char* const*)TA_MAINPP;
    const char* mptrs;
    unsigned n = pk->udef_count;
    if (!ptr_ok(ta)) return NULL;
    mptrs = *(const char* const*)(ta + OFF_MODELPTRS);
    if (!(mid && n && n <= 0x10000u && mid < n)) {
        if (mid) s_badModelId++;       /* 0 is "no model", not a refusal */
        return NULL;
    }
    if (!ptr_ok(mptrs)) return NULL;
    return *(const char* const*)(mptrs + (size_t)mid * 4);   /* in bounds */
}

/* THE TWO MODEL-AABB CACHES WENT WITH THE GL DRAW HALVES [the vulkan-only
   plan, landing 11-3]. `s_aabb` (the whole-tree bounds the shadow's height
   rule read) and `s_sbox` (the select box's own bounds), their counters, the
   `MAABB` type and the `aabb_walk` that filled them had exactly one reader
   each -- `model_aabb` and `selbox_aabb` -- and both of those were called only
   from the GL unit pass this landing deleted. Landing 11-2 had already made
   that pass unreachable (`gl_draws` is false at every surviving call site), so
   nothing observable goes with them.

   What they knew is NOT lost with the code: the select box is the ROOT PIECE's
   own vertices unioned with the origin rather than the whole tree -- a ~11 px
   difference on a Stumpy -- because `DrawUnitSelectBoxRect` passes 0 as
   0x4CB650's descend flag. That, the three-vertex threshold and the seeding of
   both bounds with {0,0,0} are written up against the disassembly in
   exe-reverse-engineering.md, which is where they belong. The Vulkan shadow
   pass does not ask for either bound: it draws the posed casters through
   tagpu_vk_hires/tagpu_vk_unit and counts them against the hand-over. */



/* Replacement meshes do NOT come through this vertex stream. A glTF model has
   smooth normals, normal maps and true-colour materials, none of which this
   program's palette-index shader can carry, so it renders in its own pass
   (tagpu_hires_draw.c) which shares this frame's FBO, depth keys, scaffold,
   fog and shadow rules. Units holding one are gathered below and simply
   contribute no vertices here. */

/* faces of one Model3DONode whose vertices are already model-space floats
   P[nvert*3] — the engine-posed vbuf for units, rotated raw verts for
   effects models. skipFace: index the engine never draws (-1 = none);
   quadOnly: textured faces need exactly 4 verts (GAF_DrawTransformed is a
   quad rasteriser — the generic 3DO draw 0x46BAE0 skips the rest). */
/* G16 step 8: the two DEGRADATIONS, counted. There is no fallback renderer any
   more (gpu-posing.md §4, "the refusal ledger"), so neither of these drops a
   unit — but both are things stock content never does, and a silent
   degradation is exactly what a gate must not allow. Both ride the `native:`
   line beside `posed=`, and are only printed when they have caught something.

     rest=   units past the frame's pose arena, drawn AT REST for that frame
             (right geometry, material, position, fog, shadow and depth; only
             the animation frozen) from one shared identity block
     unpl=   PIECES the pose walk could not place — a node that did not read or
             a parent link that never resolved — left at rest inside a unit
             that is otherwise posed, as hires_pose has always done */
/*   nobake= units the gather could not get a bake for, which since step 8
             means they DRAW NOTHING — the one honest drop in the ledger. It
             has to be counted whether or not tagpu_posebake.on is armed,
             because without a count an undrawable model is a unit that is
             simply missing from the screen with nothing in the log. */
/* The block the arena-full degradation hands out: TAGPU_PBMAXPIECE identity
   matrices, every piece visible, shaded and casting. Filled ONCE and never
   written again, which is what makes it safe to hand the same pointer to
   every unit that takes it in a frame — and what makes the degradation
   allocation-free, so it cannot itself run out and need a degradation.
   `pvis` is 3 (visible AND cached) because that is what the overwhelming
   majority of pieces read, and because the only range that consults it for
   anything but visibility is the structure slant, whose casters do not
   animate in the first place. */
static float         s_poseRestPose[TAGPU_PBMAXPIECE * 12];
static unsigned char s_poseRestShaded[TAGPU_PBMAXPIECE];
static unsigned char s_poseRestVis[TAGPU_PBMAXPIECE];
static void pose_rest_block_init(void)
{
    static int filled = 0;              /* render thread only, like the pass */
    int i;
    if (filled) return;
    for (i = 0; i < TAGPU_PBMAXPIECE; i++) {
        float* m = s_poseRestPose + (size_t)i * 12;
        m[0] = m[5] = m[10] = 1.0f;
        s_poseRestShaded[i] = 1;
        s_poseRestVis[i] = 3;
    }
    filled = 1;
}
static int   s_castLogged = 0;      /* the first casters' numbers, once per session */
#define MAXNODEV 4096               /* verts of one node staged for emission */

static int emit_node(const char* nd, const float* P, int nvert, int nv,
                     float ax, float ay, float wx0, float wz0, float encBase,
                     int owner, int pieceShaded, int skipFace, int quadOnly)
{
    int shNeutral = tagpu_r3d_shade_neutral(), shDir = tagpu_r3d_shade_dir();
    int nface = *(const int*)(nd + N_FCOUNT);
    const char* faces = *(const char* const*)(nd + N_FACES);
    if (nvert <= 0 || nface <= 0 || nface > 512 || !ptr_ok(faces)) return nv;
    if (IsBadReadPtr(faces, (SIZE_T)nface * FACE_STRIDE)) return nv;

    int j;
    for (j = 0; j < nface; j++) {
        if (j == skipFace) continue;
        const char* fa = faces + j * FACE_STRIDE;
        int fvc = *(const int*)(fa + F_VCOUNT);
        const unsigned short* idx = *(const unsigned short* const*)(fa + F_INDICES);
        if (fvc < 3 || fvc > 32 || !ptr_ok(idx)) continue;
        if (IsBadReadPtr(idx, (SIZE_T)fvc * 2)) continue;

        /* material — shared helpers from render3do */
        float uv[4], ckf = -1.0f, colv = -1.0f;
        int hasTex = 0;
        {
            const char* tg = tagpu_r3d_face_texframe(fa, owner);
            if (tg && tagpu_r3d_atlas_uv(tg, uv, &ckf)) hasTex = 1;
            if (!hasTex) {
                int fc = tagpu_r3d_face_colour(fa);
                if (fc < 0) continue;
                colv = (float)fc / 255.0f;
            }
        }
        if (quadOnly && hasTex && fvc != 4) continue;

        int k;
        for (k = 1; k + 1 < fvc; k++) {
            unsigned short tri[3]; int slot[3];
            tri[0] = idx[0]; slot[0] = 0;
            tri[1] = idx[k]; slot[1] = k;
            tri[2] = idx[k+1]; slot[2] = k+1;
            if (tri[0] >= nvert || tri[1] >= nvert || tri[2] >= nvert) continue;
            if (nv + 3 > MAXNV) return nv;   /* the budget. s_vtrunc counted this
                                                for a log line landing 11-3 deleted */
            float V[3][3]; int t;
            for (t = 0; t < 3; t++) {
                const float* v = P + tri[t] * 3;
                V[t][0] = v[0]; V[t][1] = v[1]; V[t][2] = v[2];
            }
            float shade = (float)shNeutral / 31.0f;
            /* Classic++ lights the fragment from the same outward normal the
               shade row is quantised from, carried in MAP space (x east, y up,
               z south -- 3DO z points north, hence the flip; tascene-view.html
               buildUnits does the same). Level where the engine draws the
               piece unshaded (no shade flag, a degenerate face), so the
               lambert is exactly 1.0 there, as the neutral row is the
               identity. */
            float un[3] = { 0.0f, 1.0f, 0.0f };
            if (pieceShaded) {
                float e1x = V[1][0]-V[0][0], e1y = V[1][1]-V[0][1], e1z = V[1][2]-V[0][2];
                float e2x = V[2][0]-V[0][0], e2y = V[2][1]-V[0][1], e2z = V[2][2]-V[0][2];
                float nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
                if (nx*SH_V[0] + ny*SH_V[1] + nz*SH_V[2] < 0.0f) { nx=-nx; ny=-ny; nz=-nz; }
                float nl = sqrtf(nx*nx + ny*ny + nz*nz);
                if (nl > 1e-6f) {
                    float I = (nx*SH_L[0] + ny*SH_L[1] + nz*SH_L[2]) / nl;
                    int rr = shNeutral + shDir * (int)floorf(I * 12.0f + 0.5f);
                    if (rr < 0) rr = 0; else if (rr > 31) rr = 31;
                    shade = (float)rr / 31.0f;
                    un[0] = nx / nl; un[1] = ny / nl; un[2] = -nz / nl;
                }
            }
            for (t = 0; t < 3; t++) {
                float x = V[t][0], y = V[t][1], z = V[t][2];
                float* o = s_verts + nv * NVST;
                float px = x;
                float py = -z - y * 0.5f;
                o[0] = ax + px;
                o[1] = ay + py;
                /* depth enc: row base +- intra-model view depth (2y-z),
                   squeezed into the +-2 gap between row keys */
                float md = (2.0f * y - z) / 256.0f;
                if (md > 1.8f) md = 1.8f; if (md < -1.8f) md = -1.8f;
                o[2] = encBase + md;
                if (hasTex) {
                    int c = fvc <= 4 ? slot[t] : slot[t] * 4 / fvc;
                    o[3] = (c == 1 || c == 2) ? uv[2] : uv[0];
                    o[4] = (c >= 2)           ? uv[3] : uv[1];
                    o[5] = 0.0f; o[6] = ckf;
                } else {
                    o[3] = -1.0f; o[4] = -1.0f;
                    o[5] = colv;  o[6] = -1.0f;
                }
                o[7] = shade;
                o[8] = wx0 + px;
                o[9] = wz0 + py;
                o[10] = y;
                o[11] = un[0]; o[12] = un[1]; o[13] = un[2];
                nv++;
            }
        }
    }
    return nv;
}

static float s_P[MAXNODEV * 3];     /* one node's model-space vertices */


/* effects models (projectiles, debris): the raw node rotated by the engine
   triple exactly like 0x4B6CC0 — Rz(t0) on (x,y), then Rx(t2) on (y,z),
   then Ry(t1) on (x,z); drawn unshaded (GAF_DrawTransformed copies texels);
   the face at index 0 is skipped when the node has a selection primitive
   (0x46BAE0's rule); textured faces must be quads */
static void rot2(float c, float s, float* a, float* b)
{
    float na = *a * c - *b * s;
    *b = *a * s + *b * c;
    *a = na;
}

static int emit_fx_model(const TAGPU_FXMODEL* m, int nv, float fxKey)
{
    const char* nd = m->node;
    if (!ptr_ok(nd) || IsBadReadPtr(nd, 0x40)) return nv;
    int nvert = *(const int*)(nd + N_VCOUNT);
    const int* vb = *(const int* const*)(nd + N_VERTS);
    if (nvert <= 0 || nvert > MAXNODEV || !ptr_ok(vb) || IsBadReadPtr(vb, (SIZE_T)nvert * 12)) return nv;
    const float K = 6.2831853f / 65536.0f;
    float c0 = cosf((float)m->turn[0] * K), s0 = sinf((float)m->turn[0] * K);
    float c1 = cosf((float)m->turn[1] * K), s1 = sinf((float)m->turn[1] * K);
    float c2 = cosf((float)m->turn[2] * K), s2 = sinf((float)m->turn[2] * K);
    int i;
    for (i = 0; i < nvert; i++) {
        float x = (float)vb[i*3+0] / 65536.0f;
        float y = (float)vb[i*3+1] / 65536.0f;
        float z = (float)vb[i*3+2] / 65536.0f;
        if (m->turn[0]) rot2(c0, s0, &x, &y);
        if (m->turn[2]) rot2(c2, s2, &y, &z);
        if (m->turn[1]) rot2(c1, s1, &x, &z);
        s_P[i*3+0] = x; s_P[i*3+1] = y; s_P[i*3+2] = z;
    }
    int selprim = *(const int*)(nd + N_SELPRIM);
    return emit_node(nd, s_P, nvert, nv, m->ax, m->ay, m->wx, m->wz, fxKey,
                     m->owner, 0, selprim != -1 ? 0 : -1, 1);
}

/* ------------------------------------------------------- replacement pose --
   One unit's COB pose, in the form tagpu_hires_draw.c consumes: per glTF
   piece a 4x3 (3 rows of 4) that carries a REST vertex of that piece to where
   the unit's script is holding it this frame.

   The engine keeps the pose as explicit fields rather than only as posed
   vertices, so nothing here has to recover a transform from geometry. Per
   piece: the node's rest offset from its parent (N_OFF -- the same field
   aabb_walk sums), a MOVE delta and a TURN triple in the PrimitiveStruct.
   Accumulated down the tree that is P = parent * T(off + move) * R(turn), and
   the REST transform is the same walk with move and turn zero, which collapses
   to a translation by the accumulated offset. The matrix we want is therefore
   P * T(-restOffset), and it is the identity for a piece the script has not
   touched -- so a model whose pieces never move renders exactly as before.

   The conventions were measured against the engine's own posed vertex buffer
   (P_VBUF), which is the ground truth this cannot argue with, and the residual
   came out at 2e-5 model units over every piece of a walking Peewee:

     - turn[0] rotates about X, turn[1] about Y, turn[2] about Z, each the
       positive-angle rot2 above at 65536 = 360 degrees. NOT the index order
       the effects models use (emit_fx_model reads a different struct);
     - the ORDER is Z, then X, then Y -- read out of the engine, not guessed
       (tacob landing 4). UNITS_PieceOffset 0x43DEF0 composes through 0x4B6CC0,
       which rotates the (x,y) pair by the +0x14 word, then (y,z) by +0x10, then
       (x,z) by +0x12. An earlier note here said the sample could not settle it;
       it could not, every piece in it turning about one axis only -- but the
       disassembly can, and it picked the order this pass already used;
     - `pos` is a MOVE delta in the parent's frame, added to the rest offset
       BEFORE the rotation -- also read rather than inferred: 0x43DF2A..0x43DF55
       adds PrimitiveStruct+0x04/+0x08/+0x0C to the node's +0x10/+0x14/+0x18 and
       only then walks up the chain. It still reads zero in every sample here.
       `tools/tacob pose-check --all` is the standing check on both: it rebuilds
       the cobtrace fixtures' posed vertices from these rules and diffs them
       against P_VBUF, and its residual equals this pass's own err= per dump.

   research/notes/model-import.md carries the derivation and the numbers. */

#define HPOSE_MAX  49152        /* floats: ~340 posed Peewees in a frame     */
static float s_hpose[HPOSE_MAX];
static int   s_hposeN;

/* o = a * b, both 4x3 row-major (rows of {m0,m1,m2,t}) */
static void m43_mul(const float* a, const float* b, float* o)
{
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++)
            o[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] + a[r*4+2]*b[2*4+c];
        o[r*4+3] = a[r*4+0]*b[0*4+3] + a[r*4+1]*b[1*4+3] +
                   a[r*4+2]*b[2*4+3] + a[r*4+3];
    }
}

/* T(d) * R(turn), as a 4x3. R is built by rotating the basis vectors through
   the very same rot2 sequence a vertex would take, so the matrix cannot
   disagree with the convention above. */
static void piece_local(const unsigned short* turn, const float* d, float* o)
{
    const float K = 6.2831853f / 65536.0f;
    float c0 = cosf((float)turn[0] * K), s0 = sinf((float)turn[0] * K);
    float c1 = cosf((float)turn[1] * K), s1 = sinf((float)turn[1] * K);
    float c2 = cosf((float)turn[2] * K), s2 = sinf((float)turn[2] * K);
    int j;
    for (j = 0; j < 3; j++) {
        float x = j == 0 ? 1.0f : 0.0f;
        float y = j == 1 ? 1.0f : 0.0f;
        float z = j == 2 ? 1.0f : 0.0f;
        if (turn[2]) rot2(c2, s2, &x, &y);      /* about Z */
        if (turn[0]) rot2(c0, s0, &y, &z);      /* about X */
        if (turn[1]) rot2(c1, s1, &x, &z);      /* about Y */
        o[0*4+j] = x; o[1*4+j] = y; o[2*4+j] = z;
    }
    o[0*4+3] = d[0]; o[1*4+3] = d[1]; o[2*4+3] = d[2];
}

/* glTF piece -> engine primitive, resolved by name once per unit TYPE: the
   Model3DONode tree is shared by every unit of a type, so the node pointer of
   primitive 0 identifies the template the mapping was resolved against, and
   the mesh's reload generation identifies the piece list on the other side. */
typedef struct {
    const void* mesh;
    unsigned gen;
    const void* nd0;
    int   n;                                  /* glTF pieces mapped         */
    short e[TAGPU_HMAXPIECE];                 /* engine primitive, -1 = none */
} HPMAP;
static HPMAP s_pmap[8];
static int   s_npmap;

/* `a` is ours (a glTF piece name, at most 31 chars); `b` is the engine's, and
   the caller can only establish that ONE byte of it is readable. This compare
   reads as many as `a` is long, so a name lying in the last bytes of a page
   would fault the render thread on the next one — every other engine-string
   read in this file is bounded the same way. It resolves once per unit TYPE,
   so probing each byte costs nothing worth measuring. */
static int name_eq(const char* a, const char* b)
{
    for (; *a; a++, b++) {
        char ca = *a, cb;
        if (IsBadReadPtr(b, 1)) return 0;
        cb = *b;
        if (!cb) return 0;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return !IsBadReadPtr(b, 1) && *b == 0;
}

static const HPMAP* pmap_for(const void* mesh, const char* const* nd, int nparts)
{
    int i, g, np = tagpu_hires_npiece(mesh);
    unsigned gen = tagpu_hires_gen(mesh);
    HPMAP* m;
    if (np > TAGPU_HMAXPIECE) np = TAGPU_HMAXPIECE;
    for (i = 0; i < s_npmap; i++)
        if (s_pmap[i].mesh == mesh && s_pmap[i].gen == gen &&
            s_pmap[i].nd0 == nd[0]) return &s_pmap[i];
    /* the table only ever holds one entry per replacement type, and there can
       be at most MAXMESH of those; a full table means a type was reloaded
       against a new template, so start over rather than stop mapping */
    if (s_npmap >= (int)(sizeof s_pmap / sizeof s_pmap[0])) s_npmap = 0;
    m = &s_pmap[s_npmap++];
    m->mesh = mesh; m->gen = gen; m->nd0 = nd[0]; m->n = np;
    char b[256];
    int nb = 0, bl;
    bl = _snprintf(b, sizeof b, "hires: pose unbound:");
    for (g = 0; g < np; g++) {
        const char* want = tagpu_hires_piece(mesh, g);
        m->e[g] = -1;
        if (want && want[0])
            for (i = 0; i < nparts; i++) {
                const char* have = *(const char* const*)(nd[i] + N_NAME);
                if (!ptr_ok(have) || IsBadReadPtr(have, 1)) continue;
                if (name_eq(want, have)) { m->e[g] = (short)i; break; }
            }
        if (m->e[g] >= 0) { nb++; continue; }
        /* a node the unit's 3DO has no piece for can never move: say which,
           because a renamed node is silent otherwise — it just stops posing */
        if (bl > 0 && bl < (int)sizeof b - 34)
            bl += _snprintf(b + bl, sizeof b - bl, " %.30s",
                            (want && want[0]) ? want : "<unnamed>");
    }
    {
        char l[128];
        _snprintf(l, sizeof l, "hires: pose bound %d of %d piece%s to the unit's 3DO",
                  nb, np, np == 1 ? "" : "s");
        nlog(l);
        if (nb < np) nlog(b);
    }
    return m;
}

/* ---- the cache keyed on a MODEL TEMPLATE, and the level it belongs to
   `s_pmap` (a replacement mesh's glTF piece -> engine primitive map) is keyed
   on a raw `Model3DONode*`. (It was one of three until landing 11-3 took the
   two model-AABB caches with the GL draw halves; the reasoning below was
   written for all three and holds unchanged for the one that is left.) That
   tree is
   shared by every unit of a type, so it rightly outlives any unit — but it does
   NOT outlive the LEVEL, and it is not freed through `FreeObjectState`, so
   `tagpu_reclaim`'s deferral does not cover it. Until this check existed
   nothing dropped these entries at all: a second level whose allocator handed
   the same address to a different model was served the first level's answer,
   for the rest of the process. That is not a fault -- it is a wrong shadow
   height, a wrong select box and a mis-bound replacement pose, silently.

   The cure is THE PACKET'S level generation, which the publisher advances at
   every level end -- it used to be tagpu_reclaim's own counter, and that moves
   only when reclaim is armed, so with `reclaim.off` these caches never dropped
   at all (landing review, 2026-09-12). The publisher's bump is in the same
   place reclaim's is, the teardown `0x491B60`'s POST hook when reclaim is the
   provider, which matters: a bump in the pre hook is observed by a pass that is already
   past `tagpu_overlay.c`'s teardown gate and still running (the pre hook is
   waiting for exactly that pass), and that pass would drop these caches and
   refill them from templates about to be freed, stamping the new generation on
   stale entries. Checked once per frame rather than per lookup: every one of
   these caches is consulted only from tagpu_native_frame's own call tree.

   It holds no GL objects, so dropping it is resetting one count; the entries
   rebuild on the next frame that asks. */
static unsigned s_cacheGen;              /* the level s_pmap describes           */
static unsigned s_lastLevelGen;          /* the last packet's, carried over a frame with none */

static void cache_gen_check(unsigned g)
{
    if (g == s_cacheGen) return;
    if (s_npmap) {
        char b[160];
        _snprintf(b, sizeof b,
                  "native: level %u -> %u, dropping the template cache: pmap=%d",
                  s_cacheGen, g, s_npmap);
        nlog(b);
    }
    s_cacheGen = g;
    s_npmap = 0;
}

/* Everything one unit's pose needs, accumulated down the piece tree. Shared
   with pose_dump, which checks it against the engine's own posed vertices. */
/* THE PIECE COUNT IS NOT CAPPED AT 64 ANY MORE (gpu-posing.md decision 7):
   64 was this array's size and nothing in the engine bounds a model's pieces.
   TAGPU_PBMAXPIECE and why it is 256 are in tagpu_model3do.h.
   The consequence HERE is that one HPOSE is ~17 kB, so they are held STATIC
   rather than on the stack — pose_dump and hires_pose both used to take one as
   a local. Both are render-thread only: they are reached only from
   tagpu_native_frame. */
typedef struct {
    const char* nd[TAGPU_PBMAXPIECE];  /* the TYPE's template nodes, from the run  */
    float acc[TAGPU_PBMAXPIECE][12];   /* rest vertex of that piece -> model space */
    float rest[TAGPU_PBMAXPIECE][3];   /* accumulated rest offset                  */
    unsigned char done[TAGPU_PBMAXPIECE]; /* 0 = tree link broken, piece at rest    */
} HPOSE;

/* `bt` non-NULL folds the body turn into the BASE piece's own turn, exactly
   where the compose adds it (0x45B0DB, only on the top-level call) -- the
   reconstruction needs it because P_VBUF holds the body-rotated pose, while
   hires_pose and pose_dump want model space and pass NULL. */
/* `pc` is this unit's PK_PIECE run out of the frame packet and `nparts` its
   length; `basePiece` the index the body turn folds into (0xFFFF = none). The
   per-piece POSE fields are the packet's copy, taken on the game thread; the
   per-piece NODE is the type's template, which the level teardown frees and
   tagpu_reclaim's fence covers, so it is still dereferenced here. */
static int pose_accum_body(const TAGPU_PK_PIECE* pc, int nparts, unsigned basePiece,
                           HPOSE* h, const unsigned short* bt)
{
    static short parent[TAGPU_PBMAXPIECE];   /* render thread only, as HPOSE is */
    const char** nd = h->nd;
    int i, g, left, pass;
    if (nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    if (!bt) basePiece = 0xFFFFu;
    for (i = 0; i < nparts; i++) {
        nd[i] = (const char*)(size_t)pc[i].node;
        if (!ptr_ok(nd[i]) || IsBadReadPtr(nd[i], N_CHILD + 4)) return 0;
        parent[i] = -1;
        h->done[i] = 0;
    }
    /* parent links out of the node tree: a node's children are its `child`
       and everything down that child's sibling chain */
    for (i = 0; i < nparts; i++) {
        const char* ch = *(const char* const*)(nd[i] + N_CHILD);
        int sib;
        for (sib = 0; sib < nparts && ptr_ok(ch); sib++) {
            for (g = 0; g < nparts; g++)
                if (nd[g] == ch) { if (parent[g] < 0) parent[g] = (short)i; break; }
            if (IsBadReadPtr(ch, N_SIB + 4)) break;
            ch = *(const char* const*)(ch + N_SIB);
        }
    }
    /* accumulate parents before children; a piece whose parent link is broken
       stays undone and falls back to its rest place */
    left = nparts;
    for (pass = 0; pass < nparts && left > 0; pass++) {
        for (i = 0; i < nparts; i++) {
            if (h->done[i] || (parent[i] >= 0 && !h->done[parent[i]])) continue;
            {
                const int* off = (const int*)(nd[i] + N_OFF);
                const int* mv  = pc[i].pos;
                const unsigned short* tn = pc[i].turn;
                unsigned short bturn[3];
                float d[3], loc[12];
                int k;
                for (k = 0; k < 3; k++)
                    d[k] = (float)off[k] / 65536.0f + (float)mv[k] / 65536.0f;
                if ((unsigned)i == basePiece) {
                    for (k = 0; k < 3; k++)
                        bturn[k] = (unsigned short)(tn[k] + bt[k]);
                    tn = bturn;
                }
                piece_local(tn, d, loc);
                if (parent[i] < 0) {
                    memcpy(h->acc[i], loc, sizeof loc);
                    for (k = 0; k < 3; k++)
                        h->rest[i][k] = (float)off[k] / 65536.0f;
                } else {
                    m43_mul(h->acc[parent[i]], loc, h->acc[i]);
                    for (k = 0; k < 3; k++)
                        h->rest[i][k] = h->rest[parent[i]][k] + (float)off[k] / 65536.0f;
                }
            }
            h->done[i] = 1;
            left--;
        }
    }
    return nparts;
}



/* ---- G16 step 5: one unit's pose, off the type's CACHED topology --------
   `pose_accum_body` rebuilds the parent links by scanning the node list for
   every sibling of every node. That is fine on today's rare trip frames and
   not fine at 200 units a frame, so gpu-posing.md §4 requires the walk to be
   cached per type — step 4 put `parent[]` and `restOff[]` in the bake entry,
   and this is the caller that finally consumes them instead of walking again.

   THE ARITHMETIC IS pose_accum_body's, DELIBERATELY UNCHANGED: same order of
   operations, same `piece_local`, same body-turn fold into the base piece at
   0x45B0DB's place. Only the parent array's SOURCE moves. That matters because
   until step 8 the duplication is the oracle `tagpu_posebake.on=check` compares
   against — the two must still agree exactly.

   G16 step 8: THERE IS NOWHERE TO FALL BACK TO, so this degrades instead of
   refusing (gpu-posing.md §3, "degrade inside the unit, never drop it", and
   §4's refusal ledger). A piece whose parent link never resolves takes the
   IDENTITY — it draws at its rest position while the rest of the unit poses,
   which is exactly what `hires_pose` has always done for an undone piece. The
   only thing left that returns 0 is the object not being readable at all,
   which is a LIFETIME question rather than a per-unit refusal: the gather has
   already re-read `o3` against `unit+0x9E`, and `tagpu_reclaim` defers
   `FreeObjectState 0x45AAA0` and the model-template frees until the render
   thread has completed the pass. `ptr_ok` below is a cheap filter on a VALUE
   and is not the safety argument; the deferral is.

   The piece count is NOT re-checked against the bake's: `tagpu_posebake_unit`
   matched the cache entry on `nparts` one call earlier, so equality holds at
   every call site, and `g->nparts` is used as the loop bound it always was. */
/* GATE ORACLE -- tagpu_posecrc.on, and the only thing that watches the
   matrices this pass hands the GPU. `tagpu_posedump.on` dumps the ENGINE's
   fields and `tools/tacob pose-check` diffs tacob's own reconstruction of
   them; neither ever looked at posed_pose's output.

   IT IS CONTENT-ADDRESSED, NOT TICK-ADDRESSED, AND THAT IS THE WHOLE POINT.
   Joining two runs on the sim tick would need the two to be tick-for-tick
   deterministic, which they are not: the walk fixture's move order is issued
   over the wire and lands on whatever tick it lands on, so run B's unit is
   several ticks out of phase with run A's and every CRC differs for a reason
   that has nothing to do with the code. Joining on the INPUT needs no
   determinism at all -- `in` is a CRC of every byte posed_pose reads, `out` a
   CRC of every byte it writes, and posed_pose is a pure function of the
   former. So for every `in` that appears in both logs the `out` must match,
   whatever tick each run saw it on, and a log that disagrees with ITSELF on
   one `in` says the function is not pure.

   smooth-motion.md gate 2 is exactly that join, with tagpu_lerp.on absent on
   both sides. Off by default; the two extra passes cost nothing when it is. */
static int s_poseCrcOn = 0;                 /* polled on the 30-frame cadence */
static unsigned long s_poseCrcIn = 0, s_poseCrcOut = 0;
static unsigned s_poseCrcRaced = 0;         /* samples the sim moved under */

/* Every byte posed_pose reads, in the order it reads them: the body turn, and
   per piece the rest offset, the two COB triples, the flag byte, the parent
   link, and whether the node resolved and whether it is the base piece.
   Deliberately the LIVE fields and not the blended ones -- `in` has to name
   the ENGINE state, so a lever-on run and a lever-off run that saw the same
   simulation join on the same key. */
static unsigned long pose_crc_in(unsigned basePiece, const TAGPU_PBGEOM* g,
                                 const TAGPU_PK_PIECE* pc, const char* const* nd,
                                 const unsigned short* bt, int nparts)
{
    unsigned long c = Crc32_ComputeBuf(0, bt, 3 * sizeof *bt);
    int i;
    for (i = 0; i < nparts; i++) {
        unsigned char fl = pc[i].flags;
        short par = g->parent[i];
        unsigned char link = (unsigned char)((nd[i] ? 1 : 0) |
                                             ((unsigned)i == basePiece ? 2 : 0));
        if (nd[i]) c = Crc32_ComputeBuf(c, nd[i] + N_OFF, 12);
        c = Crc32_ComputeBuf(c, pc[i].pos, 12);
        c = Crc32_ComputeBuf(c, pc[i].turn, 6);
        c = Crc32_ComputeBuf(c, &fl, 1);
        c = Crc32_ComputeBuf(c, &par, sizeof par);
        c = Crc32_ComputeBuf(c, &link, 1);
    }
    return c;
}

static int posed_pose(const TAGPU_PK_UNIT* pu, const TAGPU_PK_PIECE* pc,
                      const unsigned short* bturnSrc, unsigned basePiece,
                      const TAGPU_PBGEOM* g,
                      float* out, unsigned char* shaded, unsigned char* pvis)
{
    static float acc[TAGPU_PBMAXPIECE][12];      /* render thread only */
    static unsigned char done[TAGPU_PBMAXPIECE];
    static const char* nd[TAGPU_PBMAXPIECE];
    const unsigned short* bturn;
    unsigned short bt[3];
    int nparts, i, pass, left, anyShadeFlag = 0;
    /* smooth-motion.md option A: NULL unless tagpu_lerp.on is armed AND this
       unit has two adjacent sim ticks of history. NULL is the blend weight of
       1.0 that invariant 2 requires the degradation to be, and it is spelled
       as the caller reading the live fields exactly as it always has. */
    const int* lpos = NULL;
    const unsigned short* lturn = NULL;

    if (!pc) return 0;
    /* the bake's count, not a fresh read of the unit's: tagpu_posebake_unit
       keyed the entry on it, so the two agree by construction */
    nparts = g->nparts;
    if (nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    bturn = bturnSrc;
    bt[0] = bturn[2];                       /* +0x1C = unit+0x68, about X */
    bt[1] = bturn[1];                       /* +0x1A = unit+0x66, about Y */
    bt[2] = bturn[0];                       /* +0x18 = unit+0x64, about Z */
    for (i = 0; i < nparts; i++) {
        unsigned char fl;
        nd[i] = (const char*)(size_t)pc[i].node;
        /* a node that does not read leaves THAT PIECE at rest rather than
           dropping the unit: `done[i]` stays 0 and the identity is written
           for it below, the same degradation an unresolved parent takes.
           The template's lifetime is tagpu_reclaim's deferral plus the level
           generation the bake is keyed by; this is a value filter. */
        if (!ptr_ok(nd[i])) { nd[i] = NULL; }
        done[i] = 0;
        fl = pc[i].flags;
        if ((fl & 1) && (fl & 4)) anyShadeFlag = 1;
    }
    /* Before anything is composed: one call per unit per frame. It pairs this
       unit against the PREVIOUS packet by its stable id and blends the two
       piece runs; nothing engine-side is read or written -- invariant 1. */
    if (!pu || !tagpu_lerp_unit(pu, pc, nparts, &lpos, &lturn)) { lpos = NULL; lturn = NULL; }
    if (s_poseCrcOn) s_poseCrcIn = pose_crc_in(basePiece, g, pc, nd, bt, nparts);
    /* parents before children, exactly as pose_accum_body orders them; the
       links themselves come off the bake */
    left = nparts;
    for (pass = 0; pass < nparts && left > 0; pass++) {
        for (i = 0; i < nparts; i++) {
            short par = g->parent[i];
            const int* off;
            const int* mv;
            const unsigned short* tn;
            unsigned short bturn2[3];
            float d[3], loc[12];
            int k;
            if (done[i] || !nd[i] || (par >= 0 && !done[par])) continue;
            off = (const int*)(nd[i] + N_OFF);
            mv  = pc[i].pos;
            tn  = pc[i].turn;
            /* THE WHOLE OF OPTION A IS THESE TWO LINES. Everything below --
               the rest offset, the body-turn fold, piece_local, the parent
               multiply -- is the arithmetic it always was, on a blended pair
               of triples instead of the live ones. */
            if (lpos) { mv = lpos + i * 3; tn = lturn + i * 3; }
            for (k = 0; k < 3; k++)
                d[k] = (float)off[k] / 65536.0f + (float)mv[k] / 65536.0f;
            if ((unsigned)i == basePiece) {
                for (k = 0; k < 3; k++)
                    bturn2[k] = (unsigned short)(tn[k] + bt[k]);
                tn = bturn2;
            }
            piece_local(tn, d, loc);
            if (par < 0) memcpy(acc[i], loc, sizeof loc);
            else         m43_mul(acc[par], loc, acc[i]);
            done[i] = 1;
            left--;
        }
    }
    /* A PIECE THE WALK COULD NOT PLACE STAYS AT REST — the identity, which
       carries its rest vertices to where the model holds them unposed. That is
       `hires_pose`'s answer for the same condition and §3's rule: the unit
       draws, with one piece unanimated, rather than not drawing. Nothing in
       stock content reaches it (Gate A measured `norecon` 0 over 82 types),
       so it is also counted, once per frame, on the `native:` line. */
    for (i = 0; i < nparts; i++) {
        if (!done[i]) {
            memset(acc[i], 0, sizeof acc[i]);
            acc[i][0] = acc[i][5] = acc[i][10] = 1.0f;
        }
    }
    /* out: the piece's matrix, or all zeros for a piece this unit is not
       showing — which collapses its triangles onto the model origin, the same
       vertices `emit_geom_at`'s `if (!(pflags & 1)) continue` never emitted */
    for (i = 0; i < nparts; i++) {
        unsigned char fl = pc[i].flags;
        if (fl & 1) memcpy(out + (size_t)i * 12, acc[i], 12 * sizeof(float));
        else        memset(out + (size_t)i * 12, 0, 12 * sizeof(float));
        shaded[i] = (unsigned char)(anyShadeFlag ? ((fl & 4) != 0) : 1);
        /* G16 step 6: the visibility word the slant and the wire ranges read.
           The SLANT casts from a piece only when bit0 AND bit1 are set —
           visible, and `cached`, which a COB's dont-cache clears
           (emit_slant_at's `(pflags & 3) != 3`); a separate flag rather than a
           zeroed matrix because such a piece still draws in the body range and
           needs its matrix there. The WIRE wants plain visibility, bit0, which
           is emit_wire's own test. Written as 0/1/3 rather than `fl & 3` so
           that a piece marked cached but NOT visible reads as hidden. */
        pvis[i] = (unsigned char)(!(fl & 1) ? 0 : ((fl & 2) ? 3 : 1));
    }
    if (s_poseCrcOn) {
        /* every byte it wrote */
        unsigned long c = Crc32_ComputeBuf(0, out, (size_t)nparts * 12 * sizeof *out);
        c = Crc32_ComputeBuf(c, shaded, (size_t)nparts);
        c = Crc32_ComputeBuf(c, pvis, (size_t)nparts);
        s_poseCrcOut = c;
        /* AND THE INPUT AGAIN. This is THE GATE LANDING 3 IS MEASURED BY.
           Until the packet, the COB scripts ran on the GAME thread while this
           one posed, so the fields could move between the hash above and the
           loop that read them -- gpu-posing.md section 2's residual, one tick
           of one piece, measured at 1 sample in 1498 on the walk fixture. The
           pose now comes out of a packet the game thread finished writing
           before it handed the slot over, and the exchange forbids a fill that
           overlaps a consumer frame, so `raced=` must read EXACTLY 0: this is
           no longer a rate to accept but a race that cannot happen, and a
           non-zero count means the exchange itself is broken. */
        if (pose_crc_in(basePiece, g, pc, nd, bt, nparts) != s_poseCrcIn) {
            s_poseCrcRaced++;
            s_poseCrcIn = 0;                /* 0 = "do not join on this one" */
        }
    }
    return nparts;
}

/* Fill out[npiece*12] for `mesh` from the unit's Object3do. 0 = leave it all
   at rest (the caller then uploads the identity).

   THE BODY TURN IS FOLDED IN HERE, ALL THREE WORDS, from the Object3do's
   CACHED copy — and both halves of that sentence were wrong until 2026-09-09.
   This pass used to pose in model space and let the shader rotate the result
   by the heading alone (`uYawEnc`), which is not what the engine does twice
   over:

     - the engine folds the whole triple at Object3do+0x18/+0x1A/+0x1C into the
       BASE piece's own turn (0x45B0DB) before composing, so bank and pitch are
       part of the pose and not an outer rotation. Applying the heading alone
       draws a replacement mesh upright on ground that tilts every other unit —
       right on the flat, wrong on every slope, and a Stumpy on the fixture
       hillside reads 17.4 deg of bank and -22.1 of pitch;
     - and the words to fold are the CACHED ones, not the live unit+0x64. On a
       ground unit the two agree; on a bomber they were measured 157 degrees of
       heading apart (cached (0,16128,3) against live (0,44767,65508)), and the
       geometry the engine draws follows the cached copy.

   This is the same additive fold recon_begin uses, and that path is measured
   at exactly 0 residual against the engine's own P_VBUF on all eight COB
   classes (`tools/tacob pose-check --all`) — so the fold is the engine's
   arithmetic, and the outer rotation it replaces was the approximation.
   The caller sends 0 for the shader's yaw whenever this returns 1: the pose
   already carries the rotation, and turning it again would double it.
   model-import.md, "The body turn is all three words". */
static int hires_pose(const TAGPU_PK_PIECE* pc, int nparts, const unsigned short* bturn,
                      unsigned basePiece, const void* mesh, float* out, int npiece)
{
    static HPOSE h;                        /* 17 kB at TAGPU_PBMAXPIECE: not a local */
    unsigned short bt[3];
    int g;
    if (!pc || nparts <= 0) return 0;
    bt[0] = bturn[2];                      /* +0x1C = unit+0x68, about X */
    bt[1] = bturn[1];                      /* +0x1A = unit+0x66, about Y */
    bt[2] = bturn[0];                      /* +0x18 = unit+0x64, about Z */
    if (!pose_accum_body(pc, nparts, basePiece, &h, bt)) return 0;
    {
        const HPMAP* pm = pmap_for(mesh, h.nd, nparts);
        for (g = 0; g < npiece; g++) {
            float* o = out + g * 12;
            /* the map was resolved against a node template, and `nparts`
               comes off the unit rather than the template — so bound the index
               by THIS unit's part count rather than trusting the two agree */
            int e = (g < pm->n) ? pm->e[g] : -1;
            int r;
            if (e >= nparts) e = -1;
            if (e < 0 || !h.done[e]) {                  /* rest pose */
                memset(o, 0, 12 * sizeof(float));
                o[0] = o[5] = o[10] = 1.0f;
                continue;
            }
            if (!(pc[e].flags & 1)) {
                memset(o, 0, 12 * sizeof(float));       /* COB HIDE */
                continue;
            }
            /* P * T(-restOffset) */
            for (r = 0; r < 3; r++) {
                o[r*4+0] = h.acc[e][r*4+0];
                o[r*4+1] = h.acc[e][r*4+1];
                o[r*4+2] = h.acc[e][r*4+2];
                o[r*4+3] = h.acc[e][r*4+3] - (h.acc[e][r*4+0]*h.rest[e][0] +
                                              h.acc[e][r*4+1]*h.rest[e][1] +
                                              h.acc[e][r*4+2]*h.rest[e][2]);
            }
        }
    }
    return 1;
}

/* ---- the build ghost -----------------------------------------------------
   The translucent preview of a building under the placement cursor and of
   every queued build the order pass is showing a site rect for: the model's
   OWN colours at the ghost's alpha, through the posed program, as extra body
   draws. The squares themselves are not ours: they stay exactly as
   tagpu_mark.c and tagpu_order.c draw them, and they alone carry the
   green/blocked distinction — the ghost reads no colour at all.

   ALL THE DATA IS THE PACKET'S. The cursor's unit type, corners and mode
   bytes arrive in the header; the queue arrives as the PK_BUILD table the
   publisher copies out of tagpu_order.c's game-thread snapshot — the same
   snapshot the squares are drawn from and under the same lever gate, so the
   two can only differ by the copy's age: the squares read the arena as it
   stands at present time and the ghost reads the copy the previous draw made
   of it, one presented frame at most. The one engine read here is the
   per-type MODEL template through model_root, the same fenced read the whole
   unit pass stands on (thread-split.allow: `fenced`).

   IT DRAWS WHAT THE UNIT PASS SET UP, so it has two prerequisites, both now
   checked rather than assumed: the posed program must be live and the view
   it draws in (s_pv) must be THIS frame's — the unit pass fills it, so a
   frame that early-returned must not leave the ghost drawing against last
   frame's eye. And because it draws after tagpu_fx_render, which rebinds the
   texture units the posed program samples, it re-binds them itself (see
   ghost_bind_textures).

   The pose is a rest pose — per-piece translation by the bake's restOff,
   which is posed_pose's own output for a unit holding every piece at rest —
   built into render-thread scratch, so a ghost costs one draw call, a bake
   lookup and no pose arena.

   Armed by tagpu_ghost.on (tokens: `alpha=<f>`, default 0.40), and only
   useful with tagpu_native.on — the pass says so at runtime when armed
   without it. */
static int   s_ghostOn = 0;
static float s_ghostAlpha = 0.40f;
static unsigned s_ghostCheck = 0;
static int   s_ghostLogged = -1;     /* the armed state the log last named */
static unsigned s_ghostCurs = 0, s_ghostQueue = 0, s_ghostDrawn = 0;
static unsigned s_ghostNoBake = 0, s_ghostTrunc = 0;
static int   s_ghostNoDraw = 0;      /* the missing prerequisite was logged */

/* THE BUILD GHOST'S STANDING REQUEST FOR THE PACKET'S BUILDS TABLE. Written on
   the render thread, read on the game thread inside fill_frame; one writer, no
   ordering owed — a frame either side of a change costs one frame of an unused
   or an empty table and nothing else. This is tagpu_fxown's `want` pattern and
   exists for the same reason: the table costs the GAME thread a walk of the
   order pass's arena and a copy of up to TAGPU_PK_MAX_BUILDS * 16 bytes into
   the packet, every frame, and nothing but this pass ever reads it. Until
   2026-09-14 fill_builds was called unconditionally, so every session paid for
   it whether or not a ghost could draw — including every session before the
   ghost became a play default, where the answer was always zero.

   The 90-frame watchdog is fxown's too: a pass that has stopped asking stops
   being charged, so a render thread that dies or a lever turned off between
   polls cannot leave the publisher paying for ever. */
static volatile unsigned char g_wantBuilds;
static unsigned g_beatWantBuilds;
void tagpu_native_set_want_builds(int want, unsigned int frame_counter)
{
    g_wantBuilds = (unsigned char)(want != 0);
    if (want) g_beatWantBuilds = frame_counter;
}

/* THE WATCHDOG HAS TO LIVE OUTSIDE THE SETTER, which is the whole point of it
   and which the first version of this got wrong: it decayed inside
   set_want_builds, in an `else` arm reached only when `want == 0` — by which
   point the line above had already stored 0, so the branch could never change
   anything, and the case it was written for (the render thread STOPS CALLING)
   was the one case it could not see. tagpu_fxown does it properly and this now
   copies that: the decay runs from tagpu_overlay.c's unconditional flush run,
   which sits in front of the `tagpu_overlay.off` early-return, so a render
   thread that is STILL RUNNING but has stopped polling — a driver that refused
   (`s_state == 2` returns ahead of the 30-frame poll), a context loss,
   `tagpu_overlay.off` — stops charging the publisher for a table nothing will
   read. What it does NOT cover, because it cannot: a render thread that has
   EXITED stops calling this flush too, so the flag keeps its last value. That
   is the same hole fxown's watchdog has and is not worth a second mechanism —
   with no render thread there is no renderer, and an unused table copy is the
   least of it. (An earlier draft of this comment claimed the exit case was
   covered. It never was.) */
void tagpu_native_flush_want(unsigned int frame_counter)
{
    if (g_wantBuilds && (unsigned)(frame_counter - g_beatWantBuilds) > 90)
        g_wantBuilds = 0;
}
int tagpu_native_want_builds(void) { return g_wantBuilds != 0; }

static int ghost_armed(unsigned frame_counter)
{
    char buf[64];
    int n;
    if (frame_counter - s_ghostCheck < 30) return s_ghostOn;
    s_ghostCheck = frame_counter;
    n = tagpu_opt_read("tagpu_ghost.on", buf, sizeof buf);
    if (n < 0) {
        if (s_ghostLogged != 0) { nlog("ghost: off"); s_ghostLogged = 0; }
        s_ghostOn = 0;
        return 0;
    }
    s_ghostOn = 1;
    s_ghostAlpha = 0.40f;
    if (n > 0) {
        char* p = buf;
        while (p < buf + n) {
            while (*p && *p <= ' ') p++;
            char* q = p;
            while (*q && *q > ' ') q++;
            int last = (*q == 0);
            *q = 0;
            if (!_strnicmp(p, "alpha=", 6)) {
                float v = (float)atof(p + 6);
                if (v >= 0.05f && v <= 1.0f) s_ghostAlpha = v;
            }
            if (last) break;
            p = q + 1;
        }
    }
    /* THE PREREQUISITE IS ASKED BEFORE ANYTHING IS LOGGED, and that ORDER is
       the fix. The ghost draws through the unit pass — its view and its
       program — so with tagpu_native.on off it can never draw a pixel. Since
       2026-09-14 the ghost IS a play default and carries `needs
       tagpu_native.on` in tagpu_opt.c's table like every other dependent
       lever, so the table's own resolution withholds the default when the unit
       pass is off. This test stays because `needs` governs the DEFAULT, not the
       lever: a hand-written `tagpu_ghost.on` file arms the pass whatever the
       table says, and that is the case this refusal is for.

       Until 2026-09-14 the ARMED line was logged FIRST and this test came
       after, so with the unit pass off each 30-frame poll wrote both lines —
       ARMED set the state to 1, the refusal then found it != 2 and set it to
       2, and the next poll found 2 != 1 and started again. Two file opens
       every 30 frames for the life of the session, and a log that claimed the
       pass was armed 15 lines before saying it was not. */
    if (!tagpu_opt_on("tagpu_native.on")) {
        if (s_ghostLogged != 2) {
            nlog("ghost: off — needs tagpu_native.on (it draws through the unit pass)");
            s_ghostLogged = 2;
        }
        s_ghostOn = 0;
        return 0;
    }
    if (s_ghostLogged != 1) {
        char b[96];
        _snprintf(b, sizeof b, "ghost: ARMED alpha=%.2f", s_ghostAlpha);
        nlog(b);
        s_ghostLogged = 1;
    }
    return 1;
}

/* the model's piece list, parents first, as the packet would carry it for a
   live unit: one TAGPU_PK_PIECE per template node, at rest (pos and turn 0,
   visible). The tree is the per-type template PK_PIECE.node already
   dereferences for units — the fenced read, nothing more.

   A MODEL THE WALK CANNOT HOLD IS REFUSED, NOT HALVED. Both caps — the piece
   count and the stack of pending siblings — used to drop what did not fit and
   return the rest, which bakes and draws cleanly as half a building with
   `nobake=0` reading healthy; the live unit path publishes zero pieces for the
   same condition and counts the refusal, so this one does too.

   NO PER-NODE READABILITY PROBE, for aabb_walk's reason and by its argument —
   see the comment there, which this walk is the second instance of. Until the
   2026-09-14 review this one probed every node and every sibling with
   IsBadReadPtr, which was wrong twice over: it is the shape of check CLAUDE.md
   rules out (its answer can go stale between the check and the read), and it
   was ALSO a second way to halve a model, defeating the paragraph above — a
   failed node probe `continue`d, dropping that node AND its whole subtree, and
   a failed sibling probe ended the sibling loop, dropping that sibling and
   every one after it, both without setting `over`, so the caller read
   `nobake=0 trunc=0` over half a building. What the walk rests on instead:
   `root` came through model_root behind the caller's `mid < udef_count` bound,
   so it is a slot of the engine's own model table and the tree hanging off a
   live template is internally consistent (the engine's own walk at 0x4CB650
   probes nothing either); the two caps are DATA bounds that stop a malformed
   model running away; and the lifetime is the LEVEL, the render thread held
   out of the teardown cascade by tagpu_reclaim's wrap. The `ptr_ok` in the
   sibling loop is a range test on a VALUE — the end-of-list test, not the
   safety argument. The residual is the one aabb_walk already names: the pre
   hook's 1 s timeout (thread-safe-destruction.md §6a), pre-existing and not
   something this walk can close.

   EVERY PIECE IS MARKED VISIBLE, which is the one place the ghost is not the
   finished building: piece visibility is the COB script's (the live path takes
   it from the unit's primitives), and no script runs for a preview. A
   building whose script hides a piece at rest — doors, alternate geometry —
   therefore shows it in the ghost. Every stock 3DO's full tree is what most
   previews want, and the reviewed trigger for a stock building was unproven;
   it is a known deviation, stated in gpu-status §2.23, not a hidden one. */
static int ghost_pieces(const char* root, TAGPU_PK_PIECE* out, int max)
{
    const char* stack[TAGPU_PBMAXPIECE];
    int sp = 0, n = 0, over = 0;
    if (!ptr_ok(root)) return 0;
    stack[sp++] = root;
    while (sp > 0) {
        const char* nd = stack[--sp];
        const char* ch;
        if (n >= max) { over = 1; break; }
        out[n].pos[0] = 0; out[n].pos[1] = 0; out[n].pos[2] = 0;
        out[n].turn[0] = 0; out[n].turn[1] = 0; out[n].turn[2] = 0;
        out[n].flags = 1;                     /* visible; bit1 unused here    */
        out[n].node = (uint32_t)(size_t)nd;
        n++;
        for (ch = *(const char* const*)(nd + N_CHILD);
             ptr_ok(ch);
             ch = *(const char* const*)(ch + N_SIB)) {
            if (sp >= TAGPU_PBMAXPIECE) { over = 1; break; }
            stack[sp++] = ch;
        }
        if (over) break;
    }
    if (over) { s_ghostTrunc++; return 0; }
    return n;
}

/* one ghost: the type's bake, a rest pose, one posed body draw.
   `fx` and `fz` are world px — x, and z; `ay0` is the anchor's world-space
   screen y, `fz - half the altitude`, and it is COMPUTED BY THE CALLER with
   the same arithmetic as the square this ghost sits on. That is deliberate and
   it is not the unit pass's: the squares halve the altitude with the engine's
   own truncating `>> 1`, the unit pass halves it in float (its verified
   convention for a placed unit), and the two differ by up to half a world
   pixel — half a screen pixel at 1x and a visible few at the closest zoom
   (the 2026-09-12 review measured it). The ghost is the square's twin while
   the player is placing, so it takes the square's arithmetic; the difference
   from where the building will actually stand is that same sub-pixel. */
static int ghost_one(const TAGPU_PACKET* pk, unsigned mid,
                      float fx, float fz, float ay0,
                      const TAGPU_PDVIEW* pv, int eyeX, int eyeY, int vpL,
                      int vpT, int r0)
{
    static TAGPU_PK_PIECE s_pc[TAGPU_PBMAXPIECE];
    static float s_pose[TAGPU_PBMAXPIECE * 12];
    static unsigned char s_shaded[TAGPU_PBMAXPIECE];
    static unsigned char s_pvis[TAGPU_PBMAXPIECE];
    const char* root;
    const TAGPU_PBGEOM* bg;
    const TAGPU_PBMAT* bm;
    TAGPU_PDUNIT q;
    int np, i;

    if (mid >= pk->udef_count) return 0;      /* the bound every model id takes */
    root = model_root(pk, mid);
    if (!root) { s_ghostNoBake++; return 0; }
    np = ghost_pieces(root, s_pc, TAGPU_PBMAXPIECE);
    if (np <= 0) { s_ghostNoBake++; return 0; }
    /* owner = the human whose cursor this is: the material's team-coloured
       frames, so the ghost shows the model exactly as the player's built
       unit will look. ghost=1 keys the bake APART from the units' entries:
       this run walks the template tree in ITS order, which is not the prim
       order a live unit's packet run carries, so sharing a slot would pose a
       placed building's parts with the wrong pieces' matrices (the
       2026-09-12 leak). */
    if (!tagpu_posebake_unit(s_pc, np, pk->local_player, 1, &bg, &bm) ||
        bg->nparts <= 0 || bg->count[TAGPU_PB_BODY] <= 0) { s_ghostNoBake++; return 0; }
    for (i = 0; i < bg->nparts; i++) {
        float* o = s_pose + (size_t)i * 12;
        memset(o, 0, 12 * sizeof(float));
        o[0] = o[5] = o[10] = 1.0f;
        o[3] = bg->restOff[i][0];
        o[7] = bg->restOff[i][1];
        o[11] = bg->restOff[i][2];
        s_shaded[i] = 1;
        s_pvis[i] = 1;
    }
    memset(&q, 0, sizeof q);
    q.geom = bg; q.mat = bm;
    q.pose = s_pose; q.shaded = s_shaded; q.pvis = s_pvis;
    q.npose = bg->nparts;
    /* the engine's dimetric anchor: screen y = world z - altitude/2, already
       halved by the caller into `ay0`. (The gather's own locals name the two
       the other way round — its `fz` IS the altitude — which is the trap this
       comment exists to stop.) */
    q.ax = fx - (float)eyeX + (float)vpL;
    q.ay = ay0 - (float)eyeY + (float)vpT;
    q.wx0 = fx; q.wz0 = ay0;
    /* a ground unit's depth row at the ghost's own row — the ghost is a
       preview of exactly that, and the row term keeps it in the plane's legal
       band. Drawn LAST in the pass, so it covers what it sits on. */
    q.enc = 1.0f + (float)(((int)fz >> 4) - r0) * 4.0f;
    q.alpha = s_ghostAlpha;
    q.fog = 0;                                /* never fog-dimmed, like the square */
    q.waterT = -1e9f; q.digT = -1e9f;         /* no waterline, no digger clip    */
    q.waterMode = 0;
    q.nanoOn = 0;
    q.cast[0] = 0.0f; q.cast[1] = 0.0f; q.cast[2] = 1.0f;
    q.ghost = 1;                              /* not a unit: the stats skip it   */
    /* AND IT IS NOT IN THE DEPTH MAP, WHICH THIS FIELD NOW SAYS. The depth loop
       runs earlier in the frame and over the real units only; a ghost is drawn
       here, after it, with depth writes off. `castSkip` was left 0 by the memset
       above, which was harmless for as long as `pd_record` refused ghosts
       outright -- but it computes `casts = (depthOn && !castSkip)`, so the
       moment ghosts are carried (landing 6) every one of them would be handed
       over as a CASTER the GL map has no silhouette for, and
       `tagpu_vk_unit_casters` feeds the shadow census that is compared against
       the GL side's own count. An extra caster there is a wrong shadow map, not
       a missing one. [Landing 6, 2026-09-17.] */
    q.castSkip = 1;
    tagpu_posedraw_unit(&q);
    return 1;
}

/* the pass itself, called once per frame from tagpu_native_frame after the
   effects: collect the cursor ghost and the queue ghosts and draw them.

   THE SAMPLERS ARE THE POSED PROGRAM'S, WHICH NEVER RE-BINDS THEM — it draws
   inside the unit pass's own binds (tagpu_posedraw.c says so where it sets
   them). This pass runs after tagpu_fx_render, which rebinds units 0/1/2/4/5/6
   to its own atlas, the palette and the fog grid for its own program, so
   without rebinding the ghost samples fx textures with the model's UVs
   whenever any effect is on screen — the frame the first in-game check never
   had. Bind every unit the posed shader names, from the same sources the unit
   pass binds (that is the invariant; "nothing else happens to touch it" is
   not). */
static int ghost_offscreen(float ax, float ay, int evpL, int evpT, int evw, int evh);

/* THE BUILD GHOST'S RECORD HALF, LIFTED ABOVE THE LANE SEAM [the vulkan-only
   plan, landing 11-3]. This was `ghost_pass`, and it was called from the unit
   pass's GL half -- BELOW `if (!gl_draws) { ...hand over...; return; }` -- so
   although `tagpu_vk_unit_record_ghosts` has existed since landing 6 and
   `tagpu_posedraw.c` carries `ghost` through the hand-over, nothing ever
   recorded a ghost on the Vulkan lane and the build preview has never appeared
   there. That was invisible while `auto` meant OpenGL; landing 11-2 made it
   the default, which is what turned a latent gap into a missing feature.

   WHAT CHANGED COMING UP: three GL statements and one GL question.
   `ghost_bind_textures` and the `glDepthMask(FALSE/TRUE)` bracket were the GL
   draw's own state -- the Vulkan consumer takes depth-off with a second
   pipeline instead (`tagpu_vk_unit.c`'s ghost stage), so there is nothing to
   replace them with. And the prerequisite `tagpu_posedraw_live()` is
   `s_state == 1 && !tagpu_vk_owns_present()`, i.e. "the GL posed program is
   live", which is false on the only lane that now exists; recording needs no
   program, so what is left of that guard is the half that is still true --
   the view must belong to THIS frame.

   Everything else is untouched, because none of it was ever GL: `ghost_pieces`
   walks the 3DO tree, `ghost_offscreen` is a cull in screen space, and
   `ghost_one` fills a TAGPU_PDUNIT and hands it to `tagpu_posedraw_unit`,
   which is the same entry point the units above use. */
static void ghost_record(const TAGPU_PACKET* pk, unsigned frame_counter,
                       int eyeX, int eyeY, int vpL, int vpT, int r0,
                       int evpL, int evpT, int evw, int evh)
{
    const TAGPU_PK_BUILD* bs;
    unsigned nb, k;
    int haveCursor = 0;
    float cfx = 0.0f, cfz = 0.0f, cay = 0.0f;

    if (!s_ghostOn || !pk || !pk->in_game) return;
    /* THE DRAW'S TWO PREREQUISITES, checked rather than assumed. Both are the
       unit pass's and both are established in the same frame: the view this
       pass draws through (s_pv, filled only when that pass really ran) and the
       posed program itself — without the check a frame that early-returned
       would draw against last frame's eye, and a refused program would keep
       baking GL geometry that nothing can draw, ageing the live units out of
       the bake cache for nothing (the review's findings). `ghost_armed` says
       the lever-level half of this out loud, once. */
    if (s_pvFrame != frame_counter) {
        if (!s_ghostNoDraw) {
            /* one arm only since landing 11-3: the other read "the posed
               program is not live on this driver", which was the
               `tagpu_posedraw_live()` prerequisite. That predicate is false on
               this lane by definition, so keeping it as a reason would report a
               driver fault for the normal case. */
            nlog("ghost: armed, but the unit pass is not drawing this frame — nothing to draw in");
            s_ghostNoDraw = 1;
        }
        return;
    }
    s_ghostNoDraw = 0;
    /* THE HEARTBEAT GOES HERE, IN FRONT OF THE "nothing to draw" RETURN below.
       It used to sit at the tail, so it printed only on a frame that was both a
       multiple of 300 AND had a ghost on screen — which made `nobake` and
       `trunc`, the two counters gpu-status §2.23 says must stay 0, almost never
       observable, and never at all in the sessions where something had gone
       wrong and no ghost drew. The counters are cumulative, so printing them on
       a frame that drew nothing is exactly as meaningful. */
    if ((frame_counter % 300) == 0) {
        char b[160];
        _snprintf(b, sizeof b,
                  "ghost: curs=%u queue=%u drawn=%u nobake=%u trunc=%u alpha=%.2f",
                  s_ghostCurs, s_ghostQueue, s_ghostDrawn, s_ghostNoBake,
                  s_ghostTrunc, s_ghostAlpha);
        nlog(b);
    }
    /* the cursor: the square's own gate — mode 14, and either the band bit or
       the mouse inside the rect the engine can NAME (gather_cursor's test) —
       plus a build type the udef bound accepts.

       AND THE SQUARE MUST BE OURS AT ALL. This ghost is the twin of the build
       placement square, which lives in the marker pass's cursor layer, so it
       arms on that layer's own answer — `tagpu_mark_cursor_ours()`, the one
       gather_cursor asks — exactly as the QUEUE ghost below arms on the order
       pass's (tagpu_order_copy_builds returns 0 when that pass is disarmed).
       Until 2026-09-14 this half asked nothing: with `tagpu_mark` off, or
       `mark.on=nocursor`, the translucent building followed the pointer while
       our square did not, and the engine's own square is drawn at its UNZOOMED
       1x projection, so at any zoom != 1 the building and its square were in
       different places. Harmless while the ghost was opt-in; the ghost became
       a play default the same day, which is what made it reachable. */
    if (tagpu_mark_cursor_ours() &&
        pk->cursor_mode == 0x0E && pk->build_unit_id != 0 &&
        pk->build_unit_id < pk->udef_count &&
        (pk->region_flags & 8 ||
         (pk->mouse[0] >= pk->vp_addr[0] && pk->mouse[0] <= pk->vp_addr[2] &&
          pk->mouse[1] >= pk->vp_addr[1] && pk->mouse[1] <= pk->vp_addr[3]))) {
        const int* br = pk->build_rect;   /* the corners, world px: x, altitude, z */
        /* THE SQUARE'S OWN SANITY BOUND, and its own halving. gather_cursor
           projects the two corners and drops the rect when any of them lands
           past ±0x100000; a ghost averaged from a rect the square refuses
           would draw where the square does not. The same projection here also
           bounds the midpoint adds below, so a sentinel rect cannot overflow
           them. And the altitude is halved the way the square halves it —
           `>> 1` per corner, engine truncation, not the unit pass's float —
           so the ghost sits ON its square rather than up to half a world
           pixel above it. */
        float l = (float)br[0] - (float)eyeX + (float)vpL;
        float r = (float)br[3] - (float)eyeX + (float)vpL;
        float t = (float)(br[2] - (br[1] >> 1)) - (float)eyeY + (float)vpT;
        float b = (float)(br[5] - (br[4] >> 1)) - (float)eyeY + (float)vpT;
        if (l >= -1048576.0f && l <= 1048576.0f &&
            r >= -1048576.0f && r <= 1048576.0f &&
            t >= -1048576.0f && t <= 1048576.0f &&
            b >= -1048576.0f && b <= 1048576.0f) {
            cfx = (float)(br[0] + br[3]) * 0.5f;
            cfz = (float)(br[2] + br[5]) * 0.5f;
            /* the square's centre, in the WORLD-space half of the anchor (the
               part that has not had the eye subtracted yet): the mean of its
               two edges, each with the engine's own truncating halving */
            cay = cfz - (float)((br[1] >> 1) + (br[4] >> 1)) * 0.5f;
            haveCursor = !ghost_offscreen(cfx - (float)eyeX + (float)vpL,
                                          cay - (float)eyeY + (float)vpT,
                                          evpL, evpT, evw, evh);
        }
    }
    bs = tagpu_pk_builds(pk);
    nb = pk->n_builds;
    if (!haveCursor && (!bs || !nb)) return;

    /* THE GHOST'S OWN WINDOW, and it says so: the A/B's capture must never be
       bracketed around it (tagpu_posedraw.h), and this window is not always the
       second -- with no posed unit on screen the unit window above is skipped
       and this is the first. */
    tagpu_posedraw_begin_ghost(&s_pv);
    /* THE GHOSTS BLEND WITH EACH OTHER. Depth writes off for the pass: the
       normal case is the cursor ghost standing on a queued ghost's own site,
       and with the mask on the second draw failed GL_LESS against the first's
       depth and the overlap simply vanished. The TEST stays on, so terrain and
       units still occlude a ghost in front of them; only ghost-against-ghost
       needed the mask. Restored after, because that is the state the marker
       pass and the composite expect. */
    if (haveCursor) {
        s_ghostCurs++;
        s_ghostDrawn += ghost_one(pk, pk->build_unit_id, cfx, cfz, cay,
                  &s_pv, eyeX, eyeY, vpL, vpT, r0);
    }
    for (k = 0; k < nb; k++) {
        float fx = (float)bs[k].pos[0] / 65536.0f;
        float fz = (float)bs[k].pos[2] / 65536.0f;
        /* the same truncating halving draw_build projects with: it halves the
           whole world px `(foot + pos) >> 16`, and a footprint offset is a
           whole world px, so truncating the position alone lands on the same
           y. (The ghost draws at the site's centre either way — the offset
           cancels between the footprint's two corners.) */
        float alt = (float)((int)bs[k].pos[1] >> 16);
        float ay = fz - alt * 0.5f;
        if (ghost_offscreen(fx - (float)eyeX + (float)vpL, ay - (float)eyeY + (float)vpT,
                            evpL, evpT, evw, evh)) continue;
        s_ghostQueue++;
        s_ghostDrawn += ghost_one(pk, bs[k].type, fx, fz, ay, &s_pv, eyeX, eyeY, vpL, vpT, r0);
    }
    tagpu_posedraw_end();
}

/* `ghost_bind_textures` stood here: it bound the atlas, the LUT and the
   restored twin for the GL ghost draw. The Vulkan consumer binds its own
   descriptors, so the lift left it with no caller [landing 11-3]. */


/* is this anchor outside the rect the zoom can show? The unit gather's own
   test, margin included: it must never drop a ghost the pass would have drawn,
   so it is the widest view the lever allows, not the current viewport. */
static int ghost_offscreen(float ax, float ay, int evpL, int evpT, int evw, int evh)
{
    return ax < (float)(evpL - 256) || ax > (float)(evpL + evw + 256) ||
           ay < (float)(evpT - 256) || ay > (float)(evpT + evh + 256);
}


/* Lower the structure-shadow gate on the way out of a frame this pass will not
   draw, stamping the heartbeat as it goes. See the one place it is RAISED,
   below the viewport bound. */
#define SSHADOW_NONE() tagpu_owndraw_set_structshadow(0, f->frame_counter)

void tagpu_native_frame(const TAGPU_FRAME* f)
{
    /* before the early-out and before any gather: a level that ended while this
       pass was disarmed still invalidates the template caches, and the check is
       one aligned load when nothing has changed */
    /* the level generation is THE PACKET'S: it is the only one that moves when
       tagpu_reclaim is not armed. A frame with no packet keeps the one it had,
       which is right — nothing has told us a level ended. */
    if (f->packet) s_lastLevelGen = f->packet->level_gen;
    cache_gen_check(s_lastLevelGen);
    /* and the UNIT ATLAS, which keys on frame ADDRESSES the next level's loader
       may reuse and until G19f-7 reset only when full or on a context loss.
       Here rather than in tagpu_r3d_atlas_frame below because
       tagpu_posebake_frame latches tagpu_r3d_atlas_gen() on the next line. */
    tagpu_r3d_atlas_level(s_lastLevelGen);
    /* the geometry bake's caches take the same three generations one frame
       later than they are bumped, for the same reason and on the same thread —
       and it holds GL objects, so its drop has to be here, on the render
       thread, and not wherever the generation moved */
    tagpu_posebake_frame(f->frame_counter, s_lastLevelGen);
    pose_rest_block_init();      /* the degradation's block, once per session */
    tagpu_hires_frame(f->frame_counter);      /* gate 3b: same beat, same reason */
    tagpu_mark_frame(f->frame_counter);       /* landing 5: and again */
    tagpu_posedraw_frame(f->frame_counter);   /* this frame's counters, and the
                                              Vulkan hand-over's frame stamp */
    /* smooth-motion.md option A. BEFORE the gather, because posed_pose
       blends through it: it latches THIS FRAME'S PAIR — the packet and the
       previous one, which the exchange guarantees are two distinct sim ticks —
       and the wall-clock weight between their two tick stamps. Off by default
       and a no-op past its own early-out when tagpu_lerp.on is absent. */
    tagpu_lerp_frame(f->frame_counter, f->packet, f->packet_prev);
    if ((f->frame_counter % 30) == 0)
        s_poseCrcOn = GetFileAttributesA("tagpu_posecrc.on") != INVALID_FILE_ATTRIBUTES;
    /* WHETHER THIS PASS DRAWS, or only gathers and hands over. Under
       `renderer=vulkan` there is no GL context in the process: the arm poll, the
       view, the fog and palette COPIES, the four gathers and the vertex
       emission are the pass and run either way, and the composite -- the world
       FBO, the shadow map, the key/fill inversion and the resolve -- stands
       down. `s_state` is the GL program's state and is only asked where GL
       draws. [The vulkan-only plan, landing 4b-2.] */
    const int gl_draws = !tagpu_vk_owns_present();
    if (gl_draws && s_state == 2) {
        /* THE GL PROGRAM HAS REFUSED, so this pass paints nothing at all --
           and that includes the structures' slant shadows the blit's two
           branches are detoured for. Publish that BEFORE returning: the engine
           then keeps drawing its own, which is the only correct picture when
           we draw none. [The vulkan-only plan, landing 10b: until it, the
           branches were flipped for the process at DllMain and this path left
           every building without a shadow.] */
        tagpu_owndraw_set_structshadow(0, f->frame_counter);
        return;
    }
    if (s_armed < 0 || (f->frame_counter % 30) == 0) {
        int was = s_armed;
        /* NEVER PUBLISH "DISARMED" WHILE RE-READING. `s_armed` used to be zeroed
           here and set back at the end of the block, with a FILE READ in
           between -- and `tagpu_native_owns_unit()` opens `if (s_armed != 1)
           return 0;` and is called from the GAME thread by tagpu_markown.c's
           mark_selbox. So for the length of that read, twice a second, every
           selected unit read as "not ours", markown stopped suppressing, and the
           engine drew its own selection rects into the key-filled viewport --
           where the GUI mirror published the boxes and painted them cyan over
           the world (tagpu_gui_surf.c tap(), which now refuses the key).
           [MEASURED 2026-09-09: the hit frames landed on a strict 30-frame
           lattice -- 30/60/90/120/150/180/240 apart, the +1s being 60 fps
           capture against a 59.8 fps game -- and 256 units with no combat at all
           reproduced it at a HIGHER rate (0.70 %) than a 500 v 500 fight
           (0.42 %), which is what ruled out the explosion and vertex-budget
           theories. SELHANDBACK was 0 in every run.]
           The state is computed into locals and published in ONE store, and
           `s_armed` is volatile so that store cannot be hoisted above the state
           it publishes. That closes the periodic window, which is the one the
           shipped configuration hits twice a second.

           RESIDUAL, stated rather than papered over: `s_type` is written only
           when it has actually changed, and the pass is disarmed across that
           write -- which NARROWS a window rather than removing one, since
           nothing waits for the game thread to observe the disarm. It is
           reachable only while a human is editing the arm file, never in a
           steady configuration. Closing it properly wants the type published
           by index into a double buffer, which is its own piece of work. */
        char buf[64];
        char type[32];
        int wrecks = s_wrecks, armed = 0;
        lstrcpyA(type, s_type);
        int n = tagpu_opt_read("tagpu_native.on", buf, sizeof buf);
        if (n >= 0) {
            if (n > 0) {
                int i = 0; while (buf[i] && buf[i] > ' ') i++;
                wrecks = 0;
                if (i > 0 && i < 32) {
                    buf[i] = 0; lstrcpyA(type, buf);
                    /* extra tokens: "wrecks" arms the native husk pass */
                    char* p = buf + i + 1;
                    while (p < buf + n) {
                        while (*p && *p <= ' ') p++;
                        char* q = p;
                        while (*q && *q > ' ') q++;
                        int last = (*q == 0);
                        *q = 0;
                        if (!lstrcmpiA(p, "wrecks")) wrecks = 1;
                        if (last) break;
                        p = q + 1;
                    }
                }
            }
            armed = 1;
        }
        /* the type is what owns_unit reads AFTER the gate, so a change to it is
           the one case that has to disarm across the write */
        if (lstrcmpA(type, s_type) != 0) {
            s_armed = 0;
            lstrcpyA(s_type, type);
        }
        s_wrecks = wrecks;
        s_armed  = armed;                    /* the one store a reader can see */
        s_ss     = (GetFileAttributesA("tagpu_ss.off")     == INVALID_FILE_ATTRIBUTES);
        /* THE DEVICE-RESOLUTION WORLD IS OPT-IN, and the reason is the selection
           rects. The comment by the rect draw records the measurement: the
           driver clamps an aliased GL line to one pixel, so a line in an
           ss-times buffer is one SUPERSAMPLE wide and resolves to a half-lit
           smear — which is why the GL half deferred them into the 1x FBO after
           the box-downsample instead. Under devres there is no such downsample,
           so the rects would reach the screen thinner and dimmer than the
           engine's (about 0.75 of a device pixel at k = 1.5) while everything
           else got sharper. Two landing reviewers found this independently.
           `tagpu_devres.on` is how it was measured. The rect-as-geometry lever
           that answered it went with the GL draw half [landing 11-3] -- AND SO
           DID THE RECT: this lane draws no selection rect at all and the engine
           draws the boxes itself (see `s_selComplete`). The thin-rect problem
           described above is therefore not the one this lane has. ui-markers.md
           keeps the 2026-09-11 coverage numbers as the record of the GL path.

           `s_devresFailed` WENT WITH `fbo_size`, which was its only writer, so
           the "the driver refused the supersampled target, stay down" latch no
           longer exists here. It is not replaced by a local one because the
           refusal moved to the lane that can see the target: tagpu_vk.c names
           it in the log and captures nothing. */
        s_devres = (GetFileAttributesA("tagpu_devres.on") != INVALID_FILE_ATTRIBUTES);
        s_subpix = (GetFileAttributesA("tagpu_subpix.off") == INVALID_FILE_ATTRIBUTES);
        s_spxlog = (GetFileAttributesA("tagpu_spxlog.on")  != INVALID_FILE_ATTRIBUTES);
        s_nano   = (GetFileAttributesA("tagpu_nano.off")   == INVALID_FILE_ATTRIBUTES);
        /* tagpu_ghost.on, its own 30f poll — and the answer is PUBLISHED, because
           the packet's builds table is walked and copied by the game thread and
           is worth nothing to anyone but this pass. */
        tagpu_native_set_want_builds(ghost_armed(f->frame_counter), f->frame_counter);
        if (s_armed != was && was >= 0) {
            char b[96]; _snprintf(b, sizeof b, "native: %s (type=%s wrecks=%d ss=%d subpix=%d)",
                                  s_armed ? "ARMED" : "disarmed", s_type,
                                  s_wrecks, s_ss, s_subpix);
            nlog(b);
        }
    }
    /* View zoom — the level tagpu_zoom_read_lever() settled on at the top of
       THIS overlay frame (tagpu_overlay.c calls it once, before the scaffold,
       so every pass draws from the same level and the same predicted eye). It
       is re-read every frame there: a continuous control, so a 30-frame poll
       would quantise any ramp to 2 Hz and make a perfectly smooth renderer
       look like a staircase on video. The lever and the transform built on it
       live in tagpu_zoom.c, because the input path needs exactly the same
       numbers and the two must never disagree. */
    s_zoom = tagpu_zoom_lever();

    /* the effects pass (tagpu_fx.on) rides this frame: it needs the view,
       fog and palette set up here and draws into this FBO */
    int fxOn = tagpu_fx_armed(f->frame_counter);
    int sfxOn = tagpu_sfx_armed(f->frame_counter);
    /* the publisher fills the packet's effect tables only while a pass is
       asking for them: the walk costs it four engine arrays (landing 4a).
       Raised every frame, PASSIVE OR NOT — a passive pass still counts what it
       would have drawn — and dropped by fxown's watchdog after 90 silent
       frames, the same one the two skip bytes stand on. */
    tagpu_fxown_set_want(fxOn, sfxOn, f->frame_counter);
    int featOn = tagpu_feat_armed(f->frame_counter);
    int terrOn = tagpu_terr_armed(f->frame_counter);
    int markOn = tagpu_mark_armed(f->frame_counter);
    /* polled unconditionally, not behind markOn: tagpu_order.c hands the
       engine's driver back from its own disarm path, and it can only do that
       if it is still being asked */
    tagpu_order_armed(f->frame_counter);
    if (!s_armed && !fxOn && !sfxOn && !featOn && !terrOn && !markOn) { SSHADOW_NONE(); return; }
    /* BOTH OF THESE ARE GL AND ONLY GL. `init_gl` builds this pass's programs
       and FBOs; `tagpu_r3d_ensure` builds the 3DO atlas and LUT textures ("init
       on demand (GL context current)", where it is defined). Asking either on
       the vulkan-only lane would refuse the whole pass, gathers included, on the
       one lane the hand-overs are for. */
    if (gl_draws) {
        if (s_state == 0) init_gl();
        if (s_state != 1) { SSHADOW_NONE(); return; }
    }
    /* THE 3DO ATLAS AND THE SHADE LUT ARE THE PASS'S, not the backend's, so
       this is asked on both lanes -- the unit pass needs the atlas laid out and
       the LUT mirrored whichever rasteriser draws it, and since tagpu_gaf.h's
       change the atlas exists without a GL name. On the GL lane it is asked at
       exactly the point it always was, right after `init_gl`. */
    if (!tagpu_r3d_ensure()) { SSHADOW_NONE(); return; }

    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) { SSHADOW_NONE(); return; }
    /* The palette THE SCREEN IS SHOWN WITH, not the engine's own table: the
       engine gamma-scales every palette on the way to DirectDraw and never
       scales main+0x143A7, so a pass reading that table draws the world at
       the wrong brightness at any Gamma but the default (tagpu_pal.h). One
       resolve serves the atlas restore here and uPal below. */
    const unsigned char* pal = tagpu_pal_live();
    /* No frame is drawn with an unspecified palette. Unreachable in practice —
       ptr_ok(ta) above is what the engine-table fallback needs — but s_palTex
       is only given storage by the upload below, and a shader sampling a
       storageless texture is the all-black failure mode the fog LUT records. */
    if (!pal) { SSHADOW_NONE(); return; }
    /* the unit atlas's frame: recycle if full, arm and step its Classic++
       restore -- before any face asks it for a UV */
    tagpu_r3d_atlas_frame(pal);
    /* AND THE SHADE LUT, on the same beat and for the same reason: it is the
       pass's, the hand-over carries its mirror, and until this line it was only
       ever built by the GL composite asking for its texture name. */
    /* `tagpu_pk_shd` DEREFERENCES ITS ARGUMENT -- it reads `p->shd_len` with no
       null test of its own -- and a frame with no packet is ordinary (the shell
       before the first publish). NULL is a legitimate argument to `_want`: it
       builds the LUT from our own computed ramp, exactly as `_texref` does on
       the GL lane when the engine's table has not arrived. */
    tagpu_r3d_lut_want(f->packet ? tagpu_pk_shd(f->packet) : NULL);
    /* THE UNIT ARRAY IS NOT READ HERE ANY MORE, AND THAT IS WHAT LANDING 3 IS
       (cross-thread-engine-reads.md §5 row 2). This pass used to load `begin`
       and `end` as an unsynchronised pair: the level teardown 0x485980 frees
       the array and nulls `begin` (0x485A27) inside the cascade tagpu_reclaim
       fences, so between games the pair was refused — but the NEXT level's
       load, 0x4854A0 from 0x4918D4, runs with this thread live, stores `begin`
       at 0x485525, memsets the whole array, makes two more allocations and only
       then stores `end` at 0x4855D6, the ONLY store to main+0x1435B in the
       binary, so `end` is never nulled. For the length of that memset the pair
       was (new begin, last game's end) and the walk could run past the new
       allocation. No read-side gate could close that, and alignment played no
       part in it. The fix is the ordering the publisher stands on: every unit
       in the packet was copied on the GAME thread, during an in-play draw, at a
       point where the loader thread has finished — and the walk there runs to
       the engine's own SLOT COUNT, so it needs no pair at all.

       THE VIEW COMES FROM THE PACKET (landing 2): the
       TRUE 1x rect — not the field, which tagpu_vpwide widens at zoom < 1 —
       as the game thread published it, and the eye this frame is drawn from:
       the packet's eye plus the cursor anchor's deltas the game thread has not
       applied yet (tagpu_zoom_predicted_eye). No packet, or an out-of-game
       one, is no world to draw. The composite key rect (uVp), the zoom's
       published view and the effective gather below all mean this rect. */
    const TAGPU_PACKET* pk = f->packet;
    if (!pk || !pk->in_game) { SSHADOW_NONE(); return; }
    int vpL = pk->vp[0], vpT = pk->vp[1], vw = pk->vp[2], vh = pk->vp[3];
    int eyeX, eyeY;
    if (!tagpu_zoom_predicted_eye(&eyeX, &eyeY)) { SSHADOW_NONE(); return; }
    /* A SANITY BOUND ON ENGINE DATA, NOT A SUPPORTED-RESOLUTION LIMIT. The
       viewport is read out of engine memory and everything below sizes itself
       from it, so a garbage pair must not be believed — but nothing here
       assumes a number: the FBO is the game's own size, the gather rect is the
       viewport over the zoom, and the terrain staging is reserved from the
       viewport. 16384 is the largest 2D texture common hardware will hold,
       which is the real ceiling on the FBO the frame is drawn into; it used to
       read 4096, and a 5K or 8K desktop was refused the whole native pass. */
    if (vw < 64 || vh < 64 || vw > 16384 || vh > 16384) { SSHADOW_NONE(); return; }
/* The macro captures `f` from its expansion site, so it is confined to the one
   function that has an `f` to capture. This is its last use. */
#undef SSHADOW_NONE

    /* THE STRUCTURE-SHADOW GATE, AND THIS IS THE ONLY PLACE IT IS RAISED [the
       vulkan-only plan, landing 10b]. The blit's two branches are detoured
       rather than flipped now, and this is the word they read: set, the engine
       draws no cached slant shadow and every structure's is ours.

       IT SITS AFTER EVERY EARLY RETURN ABOVE ON PURPOSE, and the first version
       of this landing had it before them. The flag's two stale directions are
       not symmetric -- stale 0 is a double shadow for a frame, stale 1 is NO
       shadow -- so it may only be raised at a point the pass has committed to
       drawing. Published from the top of the function it stayed raised through
       every `return` here: a frame with no packet (the whole shell), a GL
       program still building, an atlas that would not allocate. Each of those
       now lowers it on the way out, which is the `SSHADOW_NONE()` above them,
       and this is the one line that raises it.

       ONE WRITE PER FRAME, so there is no window: a shell frame writes 0 and
       only 0, a drawing frame writes 1 and only 1, and the value changes only
       when the pass's own state does.

       IT IS OBSERVED, NOT PREDICTED, and that is the whole of the second
       re-review of 10b. The question is "did anything actually paint a
       structure's slant", and the honest answer is a report from the painters
       rather than a guess assembled up here from their levers. Two rounds were
       spent guessing it wrong:

         * `s_armed == 1` alone asked "is the pass armed", which is not the
           same question at all;
         * `tagpu_classicpp_on() || tagpu_posedraw_live()` asked it as an
           either/or, and the two painters are not alternatives. BOTH of them
           come out of the `pdu[]` array, which is filled only when
           `tagpu_posedraw_ready()` -- so on a driver where the posed program
           will not link (`s_state == 2`, a session latch) `npd` is 0, nothing
           casts into the cast-shadow map AND the slant range is empty, and the
           gate stood raised over an empty frame for the session with Classic++
           on, which is the shipped default. `tagpu_posedraw_ready()` is a
           PRECONDITION OF BOTH, never an alternative to one.

       `s_ssSuppress` (below, next to the painters) carries the answer, and the
       read here CLEARS it, so every frame must earn the gate again. That is
       what makes the stale directions bounded by construction rather than by
       argument: any path that does not reach the painters -- the nine returns
       above, the hand-over return at `:3788`, a lane switch, a refused map --
       leaves 0 behind and the next frame lowers the gate. `gl_draws` is ANDed
       in because it is read fresh from `tagpu_vk_owns_present()` this frame
       while `s_ssSuppress` is last frame's: without it a GL -> Vulkan switch
       would carry one raised frame across the seam.

       THE COST IS ONE FRAME, IN BOTH DIRECTIONS, and it is the same shape the
       cursor hand-over already uses (`tagpu_cursown_publish` /
       `tagpu_gui_cursor_drew_take`): when a painter starts, the engine and we
       both draw for one frame; when it stops, one frame has no structure
       shadow and then the gate falls. Neither can persist, which is the
       property the two guessed versions did not have. */
    {
        const int suppress = s_ssSuppress;
        s_ssSuppress = 0;                 /* this frame must earn it again */
        tagpu_owndraw_set_structshadow(s_armed == 1 && gl_draws && suppress,
                                       f->frame_counter);
    }
    int gw = f->game_width  > 0 ? f->game_width  : vpL + vw;
    int gh = f->game_height > 0 ? f->game_height : vpT + vh;

    /* ---- the viewport the zoom actually shows (see TAGPU_FXVIEW.evpL) ----
       Every gather below sizes itself from this rather than the engine's own
       viewport, so zooming out reaches further into the map instead of leaving
       the frame edge bare. At z >= 1 it IS the engine's viewport, bit for bit. */
    int evpL = vpL, evpT = vpT, evw = vw, evh = vh;
    int maxeff_w = (int)((float)vw / TAGPU_ZOOM_MIN) + 64;
    int maxeff_h = (int)((float)vh / TAGPU_ZOOM_MIN) + 64;
    if (s_zoom > 0.05f && s_zoom < 1.0f) {
        evw = (int)((float)vw / s_zoom) + 64;      /* +64: partial cells at the edge */
        evh = (int)((float)vh / s_zoom) + 64;
        /* THE BOUND IS THE ZOOM FLOOR, AND IT HAS THE SCREEN IN IT. The lever
           clamps to TAGPU_ZOOM_MIN, so this is the same expression at its
           extreme and it can never shorten a view the player can actually
           reach — it is here so that a zoom that somehow slipped below the
           floor cannot ask for an unbounded rect, which is a property of the
           value and not of the resolution. The fixed 8192 it replaced was
           already below what a 3840x2160 viewport asks for at 0.25x (14912),
           so the rect lost a third of its width here before the cell budget
           cut it again. */
        if (evw > maxeff_w) evw = maxeff_w;
        if (evh > maxeff_h) evh = maxeff_h;
    }
    /* Reserve the terrain staging for THIS VIEWPORT at the zoom floor and trim
       the rect to what could be reserved. Unconditional, at every zoom: the
       reservation is what the gather draws out of, so a 1x frame needs it made
       too, and making it from the viewport rather than from this frame's rect
       is what keeps a zoom-out from allocating mid-gesture. It trims nothing
       on any screen whose viewport was believed — see tagpu_terr.h. */
    tagpu_terr_clamp_span(vw, vh, &evw, &evh);
    /* the effective rect is only ever WIDER than the engine's — a trim that
       took it below the viewport would cull content that is plainly on screen.
       Then centre it: at z >= 1 the deltas are zero and this is the engine's
       own viewport, bit for bit. */
    if (evw < vw) evw = vw;
    if (evh < vh) evh = vh;
    evpL = vpL + (vw - evw) / 2;
    evpT = vpT + (vh - evh) / 2;

    int scafR0 = 0, scafRows = 0;
    int scafOn = tagpu_scaffold_frameinfo(f->frame_counter, &scafR0, &scafRows);
    int r0 = scafOn ? scafR0 : ((eyeY + (evpT - vpT)) >> 4) - 16;
    int rows = scafRows > 0 ? scafRows : (evh >> 4) + 32;
    /* this frame's depth bands (see ROW_SLACK) */
    float fxKey = 3.0f + (float)(rows + ROW_SLACK) * 4.0f + 4.0f;
    float airKey = fxKey + 12.0f;
    float depthScale = airKey + 8.0f;

    unsigned gfx = pk->gfx_opt;
    unsigned lostype = pk->los_type;
    int watched = pk->watched;

    /* ---- fog upload: the engine's own screen fog grid ----
       NOT the LOS/MAPPED source maps. The engine's overlay (0x4848E0) is
       driven entirely by the grid 0x4843C0 builds, and reading the source
       maps per fragment does not reproduce it: measured on Two Continents,
       MAPPED reports explored across a whole band the engine paints solid
       black. The grid is by construction what the engine drew, it is tiny
       (30x24 here) and it carries the black/grey split in its two bytes.
       Its cells are laid out exactly as an RG8 texture — low byte = the
       unexplored corner mask, high byte = the out-of-LOS one — so the engine
       buffer uploads with no conversion. */
    int fogMode = 0;
    s_fogGrid = NULL; s_fogCells = 0; s_fogLut = 0;
    {
        /* BOTH GRIDS COME OUT OF THE PACKET (landing 4b). This was a read of
           the engine's descriptor `*(main+0x1421F)` and of the buffer behind
           it, on this thread, while the game thread's own fog site was
           rewriting both — the read `tagpu_fog_at`'s guard was added for after
           a hard fault off a base of -9 (2026-09-03). The publisher copies the
           bytes now, with the relation between the descriptor's three numbers
           checked there, and the acquire proves the copy's length is exactly
           `cols * rows * 2`, so every index this frame can form is inside the
           bytes it was handed. The wide grid arrives the same way and
           tagpu_fogwide's three buffers, its critical section and its retire
           ring went with the hand-over they existed for.

           WHICH GRID THIS FRAME USES IS UNCHANGED, and it is this thread's
           decision because it is the one that knows what it is about to draw:
           the engine's own at zoom >= 1, where it spans the frame by
           construction (its origin is the eye rounded to a half cell and its
           count is viewW/32 + 2, so the last column starts at least a pixel
           past the viewport's right edge — same for the last row); the wide one
           below 1, where it cannot, and the ring beyond it would otherwise get
           taFog's clamp smearing the border cell.

           ...OR while the eye this frame is drawn from is AHEAD of the
           packet's — a cursor-anchored step the game thread has not applied yet
           (tagpu_zoom_unacked). The engine's grid is anchored at the packet's
           eye with only its two spare columns of slack, and the frame that
           eases up THROUGH 1.0 starts at z = 0.5 exactly at the lowest (one
           ease step of 0.25 in log space is z1 = z0^0.75 * ztgt^0.25, which
           reaches 1.0 at z0 = 8^(-1/3)), where the eye steps by up to vw/2 in
           that single frame — 896 px at the 1792-px viewport of a 1920x1080
           screen, against the 32 px those two columns are worth. The wide grid
           spans it with room over: sizing at the zoom FLOOR covers any anchored
           step at any level with the margin untouched. */
        const unsigned short* buf = tagpu_pk_fog(pk);
        int cols = pk->fog_cols, rows = pk->fog_rows;
        int orgX = pk->fog_org[0], orgY = pk->fog_org[1];
        int bufCells = cols * rows;
        {
            const unsigned short* wb = tagpu_pk_fogw(pk);
            if (wb && (tagpu_zoom_level() < 1.0f || tagpu_zoom_unacked())) {
                buf = wb; cols = pk->fogw_cols; rows = pk->fogw_rows;
                orgX = pk->fogw_org[0]; orgY = pk->fogw_org[1];
                bufCells = cols * rows;
            } else if (!wb && (tagpu_zoom_level() < 1.0f || tagpu_zoom_unacked())) {
                /* a zoomed frame drawn over the engine's 1x grid: the outer ring
                   falls back to taFog's clamp. This is what `bare=` counted on
                   tagpu_fogwide's own heartbeat before the grid rode in the
                   packet; it must read 0 outside a deliberate `fogwide.off`. */
                s_fogBare++;
            }
        }
        if (buf && cols > 0 && rows > 0) {
            /* THE UPLOAD IS GL; WHAT FOLLOWS IT IS THE PASS. The four statics
               below are what `tagpu_terr_render`'s hand-over carries to the
               Vulkan twin (`fogGrid`/`fogGridCols`/`fogGridRows` there), so the
               gate is around the texture calls and not around the block.
               The size test reads `s_fogCols`/`s_fogRows` BEFORE they are
               updated, which is why it is inside the gate with them. */
            if (gl_draws) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                x_glActiveTexture(GL_TEXTURE4);
                glBindTexture(GL_TEXTURE_2D, s_fogTex);
                if (cols != s_fogCols || rows != s_fogRows)
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, cols, rows, 0,
                                 GL_RG, GL_UNSIGNED_BYTE, buf);
                else
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cols, rows,
                                    GL_RG, GL_UNSIGNED_BYTE, buf);
                x_glActiveTexture(GL_TEXTURE0);
            }
            s_fogGrid = buf; s_fogCols = cols; s_fogRows = rows;
            s_fogCells = bufCells;
            s_fogOrgX = orgX;
            s_fogOrgY = orgY;
            /* "fog is on" is NOT LosType bit0 — that bit is only the MAPPING
               option. The overlay runs every frame and what it paints is
               decided entirely by the grid bytes: the builder writes the grey
               mask only when LosType&2 and the black mask only where MAPPED is
               clear, so an inactive mode is already an all-zero grid and costs
               nothing to sample. Gating on bit0 dropped the whole rule under
               true-LOS-without-mapping (LosType=14, a reachable skirmish
               setting) — and since G13b suppresses the engine's overlay, that
               would delete the grey band outright instead of merely disagreeing
               with it. */
            fogMode = 1 | (lostype & 2);
            /* the grey band's darken is a palette remap, not a scale: 0x4BFE10
               rewrites every pixel p as shade[p] (256 bytes, everything folded
               into the dark-grey ramp), copied into the packet by the game
               thread. Cheap enough to re-upload each frame. */
            {
                const unsigned char* t = tagpu_pk_fogshade(pk);
                static unsigned char ident[256];
                s_fogLut = t ? 1 : 0;
                if (!t) {
                    /* identity, so an absent table degrades to "grey is not
                       darkened" — never to "every grey pixel is palette index
                       0", which is solid black over the whole band */
                    int i;
                    for (i = 0; i < 256; i++) ident[i] = (unsigned char)i;
                    t = ident;
                }
                /* 256 bytes: always re-spec, so no init flag can survive a
                   context reset and leave the texture storageless (which has
                   the same all-black failure mode) */
                if (gl_draws) {
                    x_glActiveTexture(GL_TEXTURE5);
                    glBindTexture(GL_TEXTURE_2D, s_fogLutTex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0,
                                 GL_RED, GL_UNSIGNED_BYTE, t);
                    x_glActiveTexture(GL_TEXTURE0);
                }
                /* ...and the same 256 bytes kept where a second backend can
                   reach them (Phase G / G19e). The identity fallback above is
                   part of the pass's input, not a detail of the GL upload, so
                   what is published is what was uploaded and never a second
                   construction of it -- and on the vulkan-only lane, where
                   nothing was uploaded, it is still the one construction. */
                memcpy(s_fogLutBytes, t, 256);
                s_fogLutHave = 1;
            }
        }
    }

    /* ---- the live palette (re-read per frame; it does NOT cycle -- terr.c),
       and this is the ONLY palette texture the world has: terrain, features,
       effects, markers and the replacement meshes are all handed s_palTex. ---- */
    /* GL THROUGHOUT, unlike the two above: the bytes a second backend needs are
       `tagpu_pal_live()`'s, which `tagpu_pal_frame` fills from this frame's
       packet before any pass runs, and each Vulkan twin uploads them itself. */
    if (gl_draws) {
        x_glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_palTex);
        if (!s_palInit) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pal); s_palInit = 1; }
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, pal);
        x_glActiveTexture(GL_TEXTURE0);
    }

    /* ---- gather native-owned on-screen units ---- */
    /* ONE GATHERED DRAWABLE. `pu`/`pw` point into THIS FRAME'S PACKET — our
       own memory for the length of the frame, which is what retires the `dead`
       re-read this record used to carry (the engine's Object3do pointer was
       compared against the unit record again before every draw, because it
       could be freed between the gather and the draw). `pc` is the entry's
       piece run in the same packet. */
    typedef struct { const TAGPU_PK_UNIT* pu; const TAGPU_PK_WRECK* pw;
                     const TAGPU_PK_PIECE* pc; int nparts; unsigned basePiece;
                     const unsigned short* bturn;
                     const void* hires;
                     float ax, ay, gy, wx0, wz0, wy, gnd;
                     int rel, owner, cloaked, air, feat, sel, shadow, slant; unsigned yaw;
                     float waterT, digT; int waterMode;
                     int nanoOn; float nanoT, nanoC[3], nanoWire; } NU;
    static NU units[MAXU];
    /* sub-pixel motion: see the SPX block at the top of this file for the
       table, the read half and why both are file-static now */
    s_spxFrame = f->frame_counter;
    int nu = 0, nv = 0, nwr = 0, nsel = 0;
    unsigned uiGates = pk->ui_gates;
    const TAGPU_PK_UNIT* pkUnits = tagpu_pk_units(pk);
    if (s_armed) {                 /* units are gathered only by the unit pass */
    for (unsigned pui = 0; pui < pk->n_units && nu < MAXU; pui++) {
        const TAGPU_PK_UNIT* pu = &pkUnits[pui];
        unsigned st = pu->state;
        const TAGPU_PK_PIECE* pc;
        if (!(pu->flags & TAGPU_PK_U_NATIVE)) continue;
        pc = tagpu_pk_pieces(pk, pu->piece_off, pu->piece_n);
        if (!pc) continue;         /* outside the widest rect: no pose to draw */
        short wx = (short)(pu->pos[0] >> 16), wz = (short)(pu->pos[1] >> 16), wy = (short)(pu->pos[2] >> 16);
        int ix = pu->pos[0], iz = pu->pos[1], iy = pu->pos[2];
        float fx = (float)ix / 65536.0f, fz = (float)iz / 65536.0f, fy = (float)iy / 65536.0f;
        if (s_subpix) {
            unsigned slot = pu->slot;
            if (slot < 8192) {
                SPX* e = &s_spx[slot];
                if (e->tc == 0 || e->x != ix || e->z != iz || e->y != iy) {
                    e->px = e->x; e->pz = e->z; e->py = e->y; e->tp = e->tc;
                    e->x = ix; e->z = iz; e->y = iy; e->tc = f->frame_counter;
                }
                spx_sample(slot, f->frame_counter, &fx, &fz, &fy);
            }
        }
        float ax = fx - (float)eyeX + (float)vpL;
        float ay = fy - fz * 0.5f - (float)eyeY + (float)vpT;
        if (ax < evpL - 256 || ax > evpL + evw + 256 ||
            ay < evpT - 256 || ay > evpT + evh + 256)
            continue;
        int owner = pu->owner;
        int cloaked = (pu->cloak & 4) != 0;
        if (cloaked && owner != watched) continue;    /* enemies never see cloak */
        /* fog gate at the anchor tile: engine draws nothing there */
        if (fogMode & 1) {
            int fog = tagpu_fog_at(s_fogGrid, s_fogCols, s_fogRows, s_fogCells,
                                   s_fogOrgX, s_fogOrgY, wx, wy - wz / 2);
            if (fog & 1) continue;                    /* unexplored: black    */
            /* grey shows terrain, never units — the engine draws no unit it
               cannot currently see. Our own stay: they are what makes LOS. */
            if ((fog & 2) && owner != watched) continue;
        }
        /* sub-pixel evidence: with tagpu_spxlog.on present, log the emitted
           anchor of every SELECTED unit each present frame (60 Hz numeric
           filmstrip: subpix on = smooth fractions between sim steps) */
        if ((st & 0x10) && s_spxlog) {
            char b[96];
            _snprintf(b, sizeof b, "spx: f=%u fix=(%d,%d) a=(%.3f,%.3f)",
                      f->frame_counter, ix, iy, ax, ay);
            nlog(b);
        }
        if (nu == 0) pose_dump(pk, pu, pc, pu->piece_n);
        NU* n2 = &units[nu++];
        n2->pu = pu; n2->pw = NULL; n2->pc = pc; n2->nparts = pu->piece_n;
        n2->basePiece = pu->base_piece; n2->bturn = pu->bturn;
        n2->ax = ax; n2->ay = ay;
        n2->wx0 = fx; n2->wz0 = fy - fz * 0.5f;
        n2->wy = fz; n2->gnd = fz;          /* the ground under it, refined below */
        n2->yaw = pu->rot[1];
        n2->hires = NULL;
        n2->shadow = 0; n2->slant = 0;
        /* build state: the engine's own staging for a unit under construction
           (build-state.md), shared with the composite path so the two cannot
           drift. The blit-time effect that would otherwise draw this at the 1x
           projection is skipped for every unit this pass owns — the third
           owndraw detour, on 0x458DD0. */
        n2->nanoOn = s_nano &&
                     tagpu_r3d_nano_state(pu->nano, pu->id, pk->tick,
                                          &n2->nanoT, n2->nanoC, &n2->nanoWire);
        n2->waterT = -1e9f; n2->digT = -1e9f; n2->waterMode = 0;
        unsigned mask = 0;                    /* FBI booleans, from the packet */
        {
            if (pu->type_row != 0xFFFFu) {
                /* mirror the engine's shadow branch in the blit 0x459200
                   (shadows-cloak.md §3). ST_STRUCT units take the CACHED
                   SLANT SHADOW branch (Object3do+0x14): while owndraw "all"
                   has redirected that branch (tagpu_owndraw_structshadow_ours)
                   the engine draws nothing for them and the slant shadow is
                   ours, under that branch's own gates -- noshadow, and the
                   model-0-under-water skip at 0x4592D5; canhover/floater are
                   NOT tested there. The rest get a composite-derived
                   silhouette the wipe emptied -- ours, under the engine's
                   FBI gates. Without the redirect a structure keeps the
                   engine's cached shadow, exactly as before. */
                mask = pu->def_mask;
                /* a digger never reaches the cached branch: path A tests the
                   structure bit first and then sends a digger to the
                   COMPLETED branch (0x4592C8), path B tests digger before the
                   structure bit (0x4594D0) and clips a silhouette inline */
                n2->slant = (st & ST_STRUCT) != 0 && !(mask & UD_DIGGER);
                if (n2->slant) {
                    n2->shadow = tagpu_owndraw_structshadow_ours() &&
                                 !(mask & 0x02000000u);    /* noshadow          */
                    if (n2->shadow && pu->model_id == 0 &&
                        fz < (float)pk->sea_level)
                        n2->shadow = 0;
                } else {
                    n2->shadow = !(mask & 0x02000000u) &&  /* noshadow          */
                                 !(mask & 0x00081000u);    /* canhover|floater  */
                }
                /* the UnitName, lowercased by the publisher (UnitDef+0x20) */
                char nm[sizeof pu->name + 1];
                memcpy(nm, pu->name, sizeof pu->name);
                nm[sizeof pu->name] = 0;
                n2->hires = tagpu_hires_mesh(nm, f->frame_counter);
                /* A replacement unit contributes NO vertices to this pass, so
                   routing it there while the other pass cannot draw makes it
                   invisible rather than stock — and the other pass latches off
                   for the session when its program fails to build. Ask first;
                   the file is still looked up and logged, so the log says the
                   glTF was found AND why the unit is rendering as a 3DO. */
                if (n2->hires && !tagpu_hires_draw_ready()) {
                    static int said = 0;
                    n2->hires = NULL;
                    if (!said) {
                        said = 1;
                        nlog("hires: the replacement pass failed to build (see "
                             "'hires draw:' above) - replacement units fall "
                             "back to the engine's own 3DO");
                    }
                }
            }
        }
        /* waterline + digger clipping, PATH B only (composite has a depth
           plane -- the engine splits on frame+0x14 at 0x45927E; stationary
           path-A buildings are never clipped). Threshold is in vertex-height
           units: depth = vy + 0x32 <= (sea - alt) + 0x32  <=>  vy <= sea - alt.
           Enemy without the sonar bit: erased; own / sonar-seen: tinted.
           Digger: everything below the unit origin is erased. */
        {
            if (pu->flags & TAGPU_PK_U_DEPTHPLANE) {
                float sub = (float)pk->sea_level - fz;
                if (sub > 0.0f) {
                    int local = pk->local_player;
                    n2->waterT = sub;
                    n2->waterMode = (owner != local && !(st & ST_SONAR)) ? 1 : 2;
                }
                if (mask & UD_DIGGER) n2->digT = 0.0f;
            }
        }
        /* A UNIT UNDER CONSTRUCTION CASTS NO SHADOW — measured against the
           engine on one solar at one spot with only the build state varying
           (2026-09-03, build-state.md 7): over the pixels the completed unit
           darkens by half, the lobe reads 1.00 of bare terrain at 25 % built,
           0.82 at 75 %, 0.87 at 89 %, 0.70 at 95 % and 0.48 complete. Dropping
           it for the whole build matches every reading to 89 % and leaves the
           last few per cent short of a shadow the engine part-draws — the
           conservative way round, because the alternative is what this line
           was written for: the scaffold erases most of the model early on, and
           without it our slant projection shows through the hole as a black
           silhouette where the engine shows grass. */
        if (n2->nanoOn) n2->shadow = 0;
        n2->air = ((st & 3) != 1);
        n2->feat = 0;
        n2->sel = ((st & 0x10) && (uiGates & 4));
        if (n2->sel) nsel++;
        /* shadow sits at GROUND height under the unit (engine: GetPosHeight);
           terrain height byte = PLOT_MEMORY tile +0x04 (16-px grid) */
        {
            float gy = ay;
            if (pu->flags & TAGPU_PK_U_GROUND) {
                int th = pu->ground_h;
                gy = fy - (float)th * 0.5f - (float)eyeY + (float)vpT;
                n2->gnd = (float)th;
            }
            n2->gy = gy;
        }
        n2->rel = (wy >> 4) - r0;
        n2->owner = owner; n2->cloaked = cloaked;
    }

    /* A UNIT BEING BUILT INSIDE A FACTORY IS PART OF THE FACTORY'S SPRITE.
       The engine does not sort it against the factory at all: the blit's cargo
       loop (0x459646..0x4596DD) rebuilds the cargo composite, scaffolds it and
       Z-MERGES it into the factory's own scratch through 0x4B90A0, per pixel,
       by the two height planes offset by the position delta. Sorted as a
       separate sprite it lands on ITS OWN tile row instead -- measured on an
       ARM lab at world y 1072 building a Hammer at 1068, one 16-unit row
       apart, so four whole depth keys behind the lab, which then covered it at
       every pixel it filled (reported from play: "the unit is being built
       UNDER the lab"). Giving the cargo the parent's row and band leaves the
       two models to sort against each other by md, our intra-model view depth.
       That is an APPROXIMATION of the merge, not a port of it: 0x4B90A0
       compares dstDepth against srcDepth + HIWORD(dy) -- the engine's depth
       plane is a HEIGHT, biased by the world height delta between the two
       origins -- while md = (2y - z)/256 is model-local and carries neither
       that bias nor the positional term. The two agree while parent and cargo
       sit at the same height, which is every factory pad; a cargo whose origin
       is offset in height sorts here as though it were level with its parent.
       The engine's own chain skip
       (0x459657, state & 0x20000) is mirrored so a member it does not draw
       does not get moved either. */
    for (int a = 0; a < nu; a++) {
        const TAGPU_PK_UNIT* parent = units[a].pu;
        int ci, guard = 0;
        if (!parent) continue;
        /* the chain, as PACKET indices the publisher resolved: every hop is a
           bounded index into the units table. The engine walk's own 64-hop
           guard is KEPT and is what terminates: a chain that loops back on
           itself anywhere — not only to its head — costs 64 iterations and
           stops, where a head test alone would not. */
        for (ci = parent->cargo_first; ci >= 0 && guard < 64; guard++) {
            const TAGPU_PK_UNIT* c = &pkUnits[ci];
            if (!(c->state & ST_NOCARGO)) {
                for (int b = 0; b < nu; b++)
                    if (units[b].pu == c) {
                        units[b].rel = units[a].rel;
                        units[b].air = units[a].air;
                        break;
                    }
            }
            ci = c->cargo_next;
        }
    }
    }

    /* ---- gather 3D wrecks from the sweep-rect tiles (G12c close-out) ----
       anchor tile: flags bit0 + live def with FeatureMask bit0 CLEAR ->
       wreck record holds the husk's Object3do + world pos; drawn at FEATURE
       depth (3+rel*4). The engine's own scratch-fake-unit draw is suppressed
       by the owndraw classifier while this pass is armed. */
    if (s_armed && s_wrecks) {
        int mapW = pk->map_w16, mapH = pk->map_h16;
        const TAGPU_PK_WRECK* pkWrecks = tagpu_pk_wrecks(pk);
        if (mapW > 0 && mapH > 0 && mapW <= 4096 && mapH <= 4096) {
            /* THE HUSKS COME OUT OF THE PACKET. The publisher walked the
               feature grid for anchors whose def is a 3D wreck and copied the
               record's pose, so this pass no longer follows tile -> def ->
               record -> Object3do on the render thread. The rect is unchanged:
               both axes off the ZOOM's rect, like the row range and the cull
               below — leaving the columns on the engine's viewport stopped
               wrecks at the unzoomed left and right edges while terrain,
               features and units carried on past them. */
            int tx0 = ((eyeX + (evpL - vpL)) >> 4) - 8;
            int tx1 = tx0 + (evw >> 4) + 16;
            int ty0 = r0, ty1 = r0 + rows;
            unsigned wi;
            if (tx0 < 0) tx0 = 0;
            if (ty0 < 0) ty0 = 0;
            if (tx1 > mapW) tx1 = mapW;
            if (ty1 > mapH) ty1 = mapH;
            for (wi = 0; wi < pk->n_wrecks && nu < MAXU; wi++) {
                const TAGPU_PK_WRECK* pw = &pkWrecks[wi];
                const TAGPU_PK_PIECE* wpc;
                /* wreck record positions are 16.16 fixed-point (the engine
                   copies them straight into the scratch unit's +0x6A/6E/72
                   16.16 pos fields, then projects from the high words) — shift
                   to whole world units, exactly what the projection below and
                   the fog/rel maths expect. */
                int rx = pw->pos[0] >> 16;
                int rz = pw->pos[1] >> 16;
                int ry = pw->pos[2] >> 16;
                /* BY THE ANCHOR TILE, as the grid walk this replaced was: a
                   record's own position is near its tile but not inside it, so
                   filtering on the position would take a different set at the
                   rect's edge */
                if (pw->col < tx0 || pw->col >= tx1) continue;
                if (pw->row < ty0 || pw->row >= ty1) continue;
                wpc = tagpu_pk_pieces(pk, pw->piece_off, pw->piece_n);
                if (!wpc) continue;
                float ax = (float)(rx - eyeX + vpL);
                float ay = (float)(ry - rz / 2 - eyeY + vpT);
                if (ax < evpL - 256 || ax > evpL + evw + 256 ||
                    ay < evpT - 256 || ay > evpT + evh + 256) continue;
                /* wreckage is remembered furniture: hidden only where the
                   map is unexplored, visible (darkened) in grey */
                if ((fogMode & 1) &&
                    (tagpu_fog_at(s_fogGrid, s_fogCols, s_fogRows, s_fogCells,
                                  s_fogOrgX, s_fogOrgY, rx, ry - rz / 2) & 1))
                    continue;
                NU* n2 = &units[nu++];
                n2->pu = NULL; n2->pw = pw; n2->pc = wpc; n2->nparts = pw->piece_n;
                n2->basePiece = pw->base_piece; n2->bturn = pw->bturn;
                n2->ax = ax; n2->ay = ay; n2->gy = ay;
                n2->wx0 = (float)rx; n2->wz0 = (float)(ry - rz / 2);
                n2->wy = (float)rz; n2->gnd = (float)rz;
                n2->rel = (ry >> 4) - r0;
                n2->owner = 0; n2->cloaked = 0; n2->air = 0; n2->feat = 1; n2->sel = 0;
                n2->shadow = 0;     /* the engine's FShadow feature shadow stays */
                n2->slant = 0;
                /* units[] is static and only nu resets per frame, so a field
                   left unwritten here is last frame's. A wreck landing on an
                   index that held a replacement unit would inherit its HMesh*:
                   the build loop takes the hires branch, emits no native
                   vertices (the wreck itself vanishes) and draws that unit's
                   model at the wreck's anchor, at a stale yaw, posed against
                   the wreck's own Object3do. */
                n2->hires = NULL; n2->yaw = 0;
                n2->waterT = -1e9f; n2->digT = -1e9f; n2->waterMode = 0;
                /* ...and the nanoframe group, for the same reason: a wreck on
                   an index that held a unit under construction inherited
                   nanoOn = 1 and was staged and wire-drawn as one (landing
                   review, 2026-09-12). Present on main since G16 too. */
                n2->nanoOn = 0; n2->nanoT = 0.0f; n2->nanoWire = 0.0f;
                n2->nanoC[0] = n2->nanoC[1] = n2->nanoC[2] = 0.0f;
                nwr++;
            }
        }
    }
    /* THE WORLD AT THE DEVICE'S RESOLUTION (G17b, gui-renderer.md 13.1).
       Until now the supersampled buffer was always box-resolved back down to
       the GAME resolution and the composite then stretched that into the
       letterboxed viewport -- so at k > 1 the extra samples were thrown away
       and the world reached the screen at the engine's resolution, upscaled.
       When the viewport is wider than the engine's screen, pick `ss` to reach
       the device resolution instead and composite from the supersampled buffer
       directly, skipping the resolve.

       DECIDED HERE, ABOVE `fv`, AND NOT LOWER DOWN. Every pass that draws into
       this frame has to agree about how many samples a game pixel is: the fx
       and marker passes take it from `fv.ss` and the hires pass from `hv.ss`,
       and `TAGPU_GLSL_SCAF_TEST` divides gl_FragCoord by it to find the game
       pixel. The first cut of this raised `ss` AFTER `fv.ss` was set, so at
       ceil(k) > 2 they disagreed and the scaffold test addressed a texel 1.5x
       out (found by the landing review; invisible at k = 1.5, where ceil(k) is
       2 and the two happen to match).

       AT k = 1 NONE OF THIS APPLIES -- `devres` stays 0, `ss` stays 2, the
       resolve runs and the composite reads the same texture it always did, so
       every measurement taken at 1:1 (the parity md5 included) is untouched.
       Past TAGPU_SS_MAX the source would fall BELOW the destination and the
       composite would blur it, so devres is refused there rather than capped
       into a stretch. */
    int ss = s_ss ? 2 : 1;
    int devres = 0;
    if (s_ss && s_devres && f->vp_w > gw && gw > 0) {
        int need = (f->vp_w + gw - 1) / gw;          /* ceil(vp_w / gw) = ceil(k) */
        if (need <= TAGPU_SS_MAX) {
            if (need > ss) ss = need;
            devres = 1;
        }
    }
    /* ...AND PUBLISHED HERE, ONE LINE BELOW THE DECISION, so that the Vulkan
       backend sizes its offscreen world target from the SAME number rather than
       recomputing `s_ss ? 2 : 1` on its own side. The paragraph above is the
       whole argument for why that matters and it applies verbatim to a second
       backend: the first cut of `devres` raised `ss` after `fv.ss` was set and
       the scaffold test addressed a texel 1.5x out. Two lanes disagreeing about
       it would be the same bug with a wider blast radius.

       ABOVE THE `!gl_draws` RETURN, so this runs on both lanes -- every value
       here is arithmetic on the frame packet and the levers, and nothing in it
       touches GL, which is what makes that placement legal.

       THE RECT IS THE FRAME'S OWN VIEWPORT AND CARRIES NO HUD SHIFT. The GL
       composite shifts this draw by `tagpu_hud_shift` scaled into frame pixels
       (see the composite, far below); the Vulkan composite does not, so the
       shift is deliberately NOT folded in here -- publishing a shifted rect
       that only one consumer applies is how two lanes drift. `tagpu_hud.on` is
       not a play default and the gap is stated in the plan.
       [The vulkan-only plan, landing 4c-2.] */
    {
        TAGPU_WORLDTGT w;
        w.gw = gw; w.gh = gh; w.ss = ss; w.devres = devres;
        w.vx = f->vp_x; w.vy = f->vp_y; w.vw = f->vp_w; w.vh = f->vp_h;
        w.frame = f->frame_counter;
        s_wt = w; s_wtHave = 1;
    }
    /* the rect's own path, snapshotted beside ss for the same reason: the
       gather decides how many vertices an edge is worth and the draw decides
       what primitive to read them as, and the two must not disagree inside one
       frame. `selW` is a width in GAME pixels whatever the trigger said, which
       is the one unit both the 1x buffer and an ss buffer can be expressed in. */

    /* ---- effects gather (projectiles, explosions, debris, particles) ---- */
    TAGPU_FXVIEW fv;
    int nfx = 0, nfeat = 0, nterr = 0;
    unsigned rglslSeen = 0;      /* the restorer's call count before the gathers */
    /* THE VIEW IS FILLED WHATEVER IS ARMED, and only the GATHERS are gated.
       It used to be filled inside the `if` below — but `tagpu_shadow_begin`
       is handed this same struct further down, on the Classic++ shadow path,
       which is gated on NONE of these five passes. With all five disarmed the
       shadow module was therefore reading UNINITIALISED STACK: its `zoom` came
       out 0.000 and its light window ran to millions of texels, and the
       hand-over it publishes was stamped with a garbage frame, so the Vulkan
       lane's shadow pass silently found nothing every frame.
       It cannot happen under the play defaults, where `terr.on` is always on;
       it happens in exactly the configuration a single pass is MEASURED in.
       Forty stores on a path that already walks every unit is not worth
       gating. [FOUND 2026-09-15 taking the unit pass's A/B.] */
    {
        fv.eyeX = eyeX; fv.eyeY = eyeY;
        fv.packet = f->packet;
        fv.vpL = vpL; fv.vpT = vpT; fv.vw = vw; fv.vh = vh; fv.scafOn = scafOn;
        fv.evpL = evpL; fv.evpT = evpT; fv.evw = evw; fv.evh = evh;
        fv.gw = gw; fv.gh = gh; fv.ss = ss; fv.fogMode = fogMode;
        fv.zoom = s_zoom;
        fv.zoomCx = (float)vpL + (float)vw * 0.5f;
        fv.zoomCy = (float)vpT + (float)vh * 0.5f;
        fv.encSprite = fxKey + 3.0f;      /* nearer than fx models (fxKey ± 1.8) */
        fv.depthScale = depthScale;
        /* particle layer n -> depth key, from the ten 0x471F90 call sites
           (terrain-depth.md 3): 0..4 before any unit row (under everything
           the row sweep and the scaffold stamp), 5/6 after the row sweep and
           before the projectiles (above the last row key fxKey-2.2, below
           the fx models fxKey-1.8), 7 after the explosions (above the fx
           sprites at fxKey+3), 8 after the airborne sweep, 9 before the fog */
        {
            /* layers 0..2 are drawn before the flat-feature pre-pass and 3..4
               after it, so the flat band (0.40..0.50, tagpu_feat.c) sits
               between them */
            int L;
            for (L = 0; L <= 2; L++) fv.encLayer[L] = 0.30f;
            fv.encLayer[3] = fv.encLayer[4] = 0.60f;
            fv.encLayer[5] = fv.encLayer[6] = fxKey - 2.0f;
            fv.encLayer[7] = fxKey + 5.0f;
            fv.encLayer[8] = airKey + 3.0f;
            fv.encLayer[9] = airKey + 5.0f;
        }
        fv.fogGrid = fogMode ? s_fogGrid : NULL;
        fv.fogCols = s_fogCols; fv.fogRows = s_fogRows;
        fv.fogCells = s_fogCells;
        fv.fogOrgX = s_fogOrgX; fv.fogOrgY = s_fogOrgY;
        fv.fogTex = s_fogTex; fv.fogLut = s_fogLutTex;
        fv.r0 = r0; fv.rows = rows;
        fv.frame_counter = f->frame_counter;
    }
    rglslSeen = tagpu_rglsl_calls();
    if (fxOn || sfxOn || featOn || terrOn || markOn) {
        /* terrain first (the frame's far plane), then features: they own the
           depth the units are tested against */
        if (terrOn) nterr = tagpu_terr_gather(&fv);
        if (featOn) nfeat = tagpu_feat_gather(&fv);
        if (fxOn || sfxOn) nfx = tagpu_fx_gather(&fv);
        if (markOn) tagpu_mark_gather(&fv);   /* the count fed a deleted GL draw;
                                                 the CALL fills the hand-over */
        /* Classic++: one slice of the restorer, after the gathers (so the
           frames they missed this frame are queued) and before the renders
           (so what it paints is sampled this frame) */
        tagpu_rglsl_step();
    }
    /* ...AND THE SAME SLICE WITH NO PASS ARMED, when the measurement lever asks
       for it. `tagpu_rglsl.step` exists because the A/B needs the restorer
       painting while exactly ONE Vulkan pass draws, and the block above ties
       the step to arming a pass -- which used to be free only because `mark`
       had no Vulkan twin. The call-count compare is `tagpu_gui_surf.c`'s, so a
       frame the block above already stepped is not stepped twice and the slice
       budget is spent once either way. */
    if (tagpu_rglsl_step_forced(f->frame_counter) && tagpu_rglsl_calls() == rglslSeen)
        tagpu_rglsl_step();

    /* NEVER return early while we own the terrain: the engine's frame is a
       flat key fill inside the viewport, and only the composite below turns it
       back into a picture. tagpu_terr_gather hands the draw back on any bail,
       so nterr == 0 usually means the engine is painting terrain again — but a
       hard bail (no atlas, bad map pointer) can leave a key-filled frame with
       nothing of ours to cover it, and that frame still has to be composited. */
    int terrOwned = tagpu_terrown_filled();
    if (nu == 0 && nfx == 0 && nfeat == 0 && nterr == 0 && !markOn && !terrOwned) return;

    /* THE POSE VIEW IS THE GATHER'S, so it is built here and not inside the
       composite where it used to sit. Every number in it comes from a local
       assigned well above this line -- the viewport, `gw`/`gh`, `s_zoom`,
       `depthScale`, `ss`, `scafOn`, the fog origin and the Classic++ light --
       and none of it is GL. It is stored in `s_pv`, which already existed for
       the build ghost's own dependency check, and BOTH lanes now read it from
       there: this one to hand the unit pass over, the composite below to draw
       it. Copying the twenty lines into the vulkan-only exit instead would
       have been a second construction to drift from the first, which is the
       failure this landing keeps finding elsewhere.
       [The vulkan-only plan, landing 4b-2.] */
    {
        TAGPU_PDVIEW pv;
        const TAGPU_LIGHT* L = tagpu_classicpp_light();
        memset(&pv, 0, sizeof pv);
        pv.game[0] = (float)gw; pv.game[1] = (float)gh;
        pv.zoom = s_zoom;
        pv.zoomC[0] = (float)vpL + (float)vw * 0.5f;
        pv.zoomC[1] = (float)vpT + (float)vh * 0.5f;
        pv.depthScale = depthScale;
        pv.ss = (float)ss;
        pv.scafOn = scafOn ? 1 : 0;
        pv.scafP[0] = (float)vpL; pv.scafP[1] = (float)vpT;
        pv.scafP[2] = (float)vw;  pv.scafP[3] = (float)vh;
        pv.fogOrg[0] = (float)s_fogOrgX; pv.fogOrg[1] = (float)s_fogOrgY;
        pv.fogDim[0] = (float)s_fogCols; pv.fogDim[1] = (float)s_fogRows;
        pv.lit = tagpu_classicpp_on() ? 1 : 0;
        pv.sun[0] = L->unitSun[0]; pv.sun[1] = L->unitSun[1]; pv.sun[2] = L->unitSun[2];
        pv.amb = L->amb;
        pv.norm = 1.0f / L->unitLevel;
        pv.shNeutral = tagpu_r3d_shade_neutral();
        pv.shDir = tagpu_r3d_shade_dir();
        s_pv = pv;
        /* the build ghost's dependency, made checkable: its pass may only draw
           in a frame this ran (see ghost_pass) */
        s_pvFrame = f->frame_counter;
    }


    /* uFog bit1 = hide in grey rather than darken. Units the watched player
       cannot see are not drawn at all; wreckage is furniture and stays, and
       our own units make the LOS they stand in. */
#define FOGW(i) ((fogMode & 1) | \
                 ((units[i].feat || units[i].owner == watched) ? 0 : 2))

    /* ---- build geometry (body); shadow reuses it with an offset ---- */




    int i;
    static TAGPU_HUNIT hunits[MAXU];
    int nhi = 0;
    /* G16 step 5: the units the POSED program drew, and their poses. `pdix[i]`
       is the unit's entry or -1; a posed unit contributes no vertices to the
       shared stream, so its firstv range is empty and every CPU draw over it
       is a no-op — the same way a replacement mesh's is. */
    static TAGPU_PDUNIT pdu[MAXU];

    /* THE FRAME'S POSE ARENA, sized from MAXU rather than a magic unit count
       (gpu-posing.md §4, "the budget"). The bound that matters is total PIECES
       on screen, not units x the per-model maximum: 2048 units at stock's
       worst model (36 pieces, ARMSCORP/CORSCORP) is 73 728, and a 256-piece
       model is 7x anything stock ships. PD_ARENA slots is 4.7 MB of pose and
       96 KB each of the two per-piece bytes; sizing for MAXU * TAGPU_PBMAXPIECE
       instead would be 25 MB to make an unreachable case impossible, which is
       what the rest-block degradation is for. Past it a unit draws at rest —
       it never drops out of the scene. */
#define PD_ARENA (MAXU * 48)                /* pieces buffered per frame */
    static float pdPose[PD_ARENA * 12];
    static unsigned char pdShaded[PD_ARENA];
    static unsigned char pdPvis[PD_ARENA];
    int npd = 0, pdPoseN = 0, pdShadedN = 0;
    const int pdPoseMax = (int)(sizeof pdPose / sizeof pdPose[0]);
    const int pdShadedMax = (int)(sizeof pdShaded);
    int pdReady = tagpu_posedraw_ready();
    /* tagpu_posecrc.on: one line per unit per SIM TICK. Per FRAME would be two
       identical lines per tick at 60 fps and sixteen on a fast machine, which
       buries the transitions the join is looking for. */
    static unsigned crcTick = 0xFFFFFFFFu;
    unsigned crcNow = 0;
    int crcLog = 0;
    if (s_poseCrcOn) {
        crcNow = pk->tick;
        if (crcNow != crcTick) { crcTick = crcNow; crcLog = 1; }
    }
    s_hposeN = 0;
    for (i = 0; i < nu; i++) {
        /* BEFORE any of the skips below, not in the branch that fills it: the
           array is static, so a unit that takes an early `continue` — dead, or
           a replacement mesh — would otherwise be read against another unit's
           index from an earlier frame. */
        /* The object pointer was captured at gather time. If the engine has
           since nulled or replaced it (unit death, wreck destroyed), the old
           object is on tagpu_reclaim's queue and still readable — but drawing
           a dead unit's last pose is pointless, so skip it. On an exe where
           reclaim could not arm this is the narrow-window guard on its own:
           the null lands the instruction after the free returns, so a block
           that could fault has already changed here
           (thread-safe-destruction.md). Wrecks re-read their record. */
        /* airborne units draw in a second, un-rowed sweep above everything
           (terrain-depth 3.3) -> the air band above the effects band; wrecks
           sit at FEATURE depth (3+rel*4, terrain-depth 3.4) */
        float encBase = units[i].air ? airKey
                      : (units[i].feat ? 3.0f : 1.0f) + (float)units[i].rel * 4.0f;
        /* THE RE-READ IS GONE (frame packet exchange, landing 3). This used to
           compare the unit's `+0x9E` again against the Object3do gathered a
           moment earlier, because the engine could free it between the two —
           the narrow-window guard tagpu_reclaim's deferral sits behind. There
           is nothing left to re-read: the pose, the composite rect and every
           field below are a COPY the game thread made, in our own memory,
           valid for the whole frame. `s_reread` therefore stays 0 for good
           and the counter is kept only so a non-zero one would be loud. */
        if (units[i].hires) {
            /* no vertices here: this unit is the other pass's, and leaving
               firstv[i] == firstv[i+1] makes its draws below empty */
            TAGPU_HUNIT* h = &hunits[nhi++];
            h->mesh = units[i].hires;
            /* the COB pose, into a frame arena; past the arena a unit still
               draws, at rest, rather than dropping out of the scene */
            h->pose = NULL; h->npose = 0;
            {
                int np = tagpu_hires_npiece(units[i].hires);
                if (np > TAGPU_HMAXPIECE) np = TAGPU_HMAXPIECE;
                if (np > 0 && s_hposeN + np * 12 <= HPOSE_MAX) {
                    float* dst = s_hpose + s_hposeN;
                    if (hires_pose(units[i].pc, units[i].nparts, units[i].bturn,
                                   units[i].basePiece, units[i].hires, dst, np)) {
                        h->pose = dst; h->npose = np;
                        s_hposeN += np * 12;
                    }
                }
            }
            h->ax = units[i].ax;   h->ay = units[i].ay;
            h->wx0 = units[i].wx0; h->wz0 = units[i].wz0;
            h->enc = encBase;
            h->shadowDy = (float)(units[i].gy - units[i].ay);
            /* the pose carries the body turn now (hires_pose), so the
               shader must not turn it again; only the rest-pose fallback,
               which has no pose to carry anything, still needs an outer
               rotation — and it takes the heading from the CACHED triple the
               drawn geometry follows, not the live unit word */
            h->yaw = h->pose ? 0 : units[i].bturn[1];
            h->alpha = units[i].cloaked ? 0.5f : 1.0f;
            h->fog = FOGW(i);
            h->waterT = units[i].waterT;
            h->digT = units[i].digT;
            h->waterMode = units[i].waterMode;
            /* the silhouette needs both option bits, the slant only "Shadow"
               (the engine's cached branch never tests TShadow) */
            h->slant = units[i].slant;
            h->shadow = units[i].shadow && (units[i].slant || (gfx & 8));
            h->air = units[i].air;
            h->cast[0] = h->cast[1] = 0.0f; h->cast[2] = 1.0f; h->castSkip = 1;
        } else {
            /* G16 step 8: THE POSED PROGRAM IS THE PATH. This unit is drawn
               out of its type's baked buffers with its pose in a uniform
               block, and no vertices are built for it here at all — the CPU
               emitters that used to do it are gone, and with them the pose
               race's original window, because nothing below reads `prim+0x22`.

               There is nowhere to fall back to, so every condition that used
               to refuse now DEGRADES inside the unit (gpu-posing.md §4, "the
               refusal ledger"):

                 - the frame's pose arena full  -> the unit draws AT REST, from
                   one shared identity block that costs no arena, so the
                   degradation cannot itself fail;
                 - a piece the walk cannot place -> that PIECE at rest, inside
                   a unit that is otherwise posed (`posed_pose`);
                 - the type will not bake at all -> nothing is drawn for it,
                   which is a model past TAGPU_PBMAXPIECE pieces or PB_MAXVERT
                   vertices — 7x and 85x what the largest stock model asks for,
                   logged once per model by the bake, and the one honest drop.

               `npd < MAXU` is not tested: the gather loop above stops at
               `nu < MAXU` and every unit here takes exactly one slot, so
               `npd <= nu <= MAXU` holds by construction. */
            const TAGPU_PBGEOM* bg; const TAGPU_PBMAT* bm;
            if (!pdReady) continue;
            if (!tagpu_posebake_unit(units[i].pc, units[i].nparts, units[i].owner, 0, &bg, &bm) ||
                bg->nparts <= 0 || bg->count[TAGPU_PB_BODY] <= 0) {
                continue;
            }
            {
                TAGPU_PDUNIT* q = &pdu[npd];
                int np = bg->nparts;
                /* THE DEGRADATION, and it is chosen before anything is
                   written: past the arena the unit takes the shared rest
                   block instead of a slice of it. `s_poseRestBlock` is
                   TAGPU_PBMAXPIECE identities, filled once, never written
                   again — so this path allocates nothing and the unit draws
                   with its animation frozen for the frame rather than not
                   drawing. */
                int fits = pdPoseN + np * 12 <= pdPoseMax &&
                           pdShadedN + np <= pdShadedMax;
                memset(q, 0, sizeof *q);
                q->geom = bg; q->mat = bm;
                if (fits &&
                    posed_pose(units[i].pu, units[i].pc, units[i].bturn, units[i].basePiece,
                               bg, pdPose + pdPoseN,
                               pdShaded + pdShadedN, pdPvis + pdShadedN)) {
                    q->pose   = pdPose + pdPoseN;
                    q->shaded = pdShaded + pdShadedN;
                    q->pvis   = pdPvis + pdShadedN;     /* same stride and slot */
                    pdPoseN += np * 12;
                    pdShadedN += np;
                    if (crcLog && s_poseCrcIn) {
                        char cb[128];
                        _snprintf(cb, sizeof cb,
                                  "posecrc: tick=%u o3=%08X np=%d in=%08lx out=%08lx raced=%u",
                                  crcNow, units[i].pu ? units[i].pu->o3_key
                                                      : (units[i].pw ? units[i].pw->o3_key : 0u), np,
                                  s_poseCrcIn, s_poseCrcOut, s_poseCrcRaced);
                        cb[sizeof cb - 1] = '\0';
                        nlog(cb);
                    }
                } else {
                    q->pose   = s_poseRestPose;
                    q->shaded = s_poseRestShaded;
                    q->pvis   = s_poseRestVis;
                }
                q->npose = np;
                q->ax = units[i].ax;   q->ay = units[i].ay;
                q->wx0 = units[i].wx0; q->wz0 = units[i].wz0;
                q->enc = encBase;
                q->alpha = units[i].cloaked ? 0.5f : 1.0f;
                q->fog = FOGW(i);
                q->waterT = units[i].waterT;
                q->digT = units[i].digT;
                q->waterMode = units[i].waterMode;
                q->nanoOn = units[i].nanoOn;
                q->nanoT = units[i].nanoT;
                q->nanoC[0] = units[i].nanoC[0];
                q->nanoC[1] = units[i].nanoC[1];
                q->nanoC[2] = units[i].nanoC[2];
                q->cast[0] = 0.0f; q->cast[1] = 0.0f; q->cast[2] = 1.0f;
                npd++;   /* the count is live: the hand-over and the bail read it */
                /* the model top no longer falls out of the vertices. Only a
                   WRECK reads it: a unit with a record prefers model_aabb */
            }
        }
    }
    /* effects models (missiles, shells, debris) through the same path */
    if (nfx) {
        int k, nm = tagpu_fx_nmodels();
        for (k = 0; k < nm; k++) nv = emit_fx_model(tagpu_fx_model(k), nv, fxKey);
    }
    /* nhi belongs in this test: a replacement unit contributes no vertices to
       this pass (it is the other one's), so a frame holding nothing but those
       would bail here and draw them nowhere. SINCE G16 STEP 8 npd belongs in
       it for exactly the same reason, and more urgently: an ordinary unit
       contributes no vertices either now, so without this a frame of nothing
       but units — every unit in the game, on a map with our terrain off —
       would return here having drawn none of them. */
    if (nv == 0 && nhi == 0 && npd == 0 && nfx == 0 && nfeat == 0 && nterr == 0 &&
        !markOn && !terrOwned) return;

    /* MOVED BELOW THE GATHER, not left at the top of the composite [4b-2]. The
       per-unit array `pdu[]` and the vertex emission above are the UNIT pass's
       gather, and they are pure CPU -- audited with the corrected regex: there
       is not one GL call between the four world gathers and the early return
       just above. Exiting before them left the unit pass no gather to hand
       over; gating its 150 GL calls one by one would have been the wrong answer
       to that, because its entry points are all called from the composite
       BELOW, which this lane never reaches. They are already unreachable here.
       What is missing is the RECORD, not a gate. */
    /* THE VULKAN-ONLY LANE'S FRAME ENDS HERE, and it ends here rather than
       threading a gate through the composite because it is a DIFFERENT frame.
       Everything below this line is the GL world composite -- 103 GL calls in
       eighteen runs: the unit geometry upload, the cast-shadow depth map, the
       `ss x` FBO, the shadow and slant redraws, the nano wire pass, the key/fill
       inversion against the engine's own surface and the resolve to the
       drawable. None of it has a counterpart on this lane yet; the `ss` target
       and TA's surface are 4c, and until then each ported pass draws itself into
       the swapchain from its own hand-over.

       SO THE HONEST SHAPE IS TWO EXITS, not one body with gates in it. The GL
       path below is left byte for byte as it was -- which is what the phase's
       standing rule asks ("a Phase G landing that moves a GL pixel has failed")
       -- and this lane calls the passes that have been taught to hand over
       without drawing, in the composite's own order, and stops.

       THE PREDICATE IS THE COMPOSITE'S OWN, deliberately: `if (nterr)` is what
       gates `tagpu_terr_render` below, so the two sites agree about when a pass
       runs by using the same test rather than by being read together. Each
       `*_render` also refuses on its own count, so this is belt and braces.

       WHAT IS NOT DONE HERE, stated rather than left to be discovered: the
       300-frame stats line at the end of the composite (posebake, posedraw and
       the fill sequence) does not run on this lane, and neither does
       `tagpu_zoom_publish_view` -- but that one could not run anyway, because
       it is gated on `keyOn >= 0` and `keyOn` needs `f->surface_tex`, which is
       0 until 4c gives this backend TA's surface. The input path therefore
       stays 1:1 here, which is correct while nothing zoomed reaches the screen.
       [The vulkan-only plan, landing 4b-2.] */
    if (!gl_draws) {
        /* WHETHER TERRAIN IS CLIPPED TO THE VIEWPORT IS THE PASS'S DECISION AND
           NOT GL'S, and this line is where that gets said on this lane. The
           engine clips unit blits to the viewport rect and this pass matches it;
           the hand-over carries that as `scissorOn` and the Vulkan twin's
           `terr_scissor` honours it ("NO CLIP WHERE THE GL LANE HAS NONE").
           Below, `s_scissorOn` is set from whether `x_glScissor` RESOLVED --
           which is a fact about one context's entry points, and the reason the
           GL path has a fallback at all. With no context every `x_gl*` is NULL,
           so leaving the flag to that told the twin "no clip" and it drew
           terrain over the whole frame instead of the viewport.
           MEASURED before the fix: 33 088 px of 786 432 (4.21%) differing
           against the two-lane capture, every one of them black in the GL half
           and LOS-grey here, and all of them outside the 896x704 viewport this
           fixture logs as `zoomvp=`. Coverage, not colour. [Landing 4b-2.] */
        s_scissorOn = 1;
        /* IN THE COMPOSITE'S OWN ORDER, and with the composite's own predicates:
           terrain is the world's bottom layer and the features follow it, which
           is the order below and the order tagpu_vk.c records the passes in.
           `glEnable(GL_BLEND)` wraps the feature render down there; over here the
           twin owns its own blend state, which is why there is nothing to set. */
        if (nterr) tagpu_terr_render(&fv, 0);
        if (nfeat) tagpu_feat_render(&fv, 0);
        /* the effects are the LAST of the world, after the features and the
           units -- the order below, and the order tagpu_vk.c records in. The
           scaffold texture is 0 here: the effects twin refuses any frame whose
           twin had the scaffold live, so that is the value it wants. */
        if (nfx) tagpu_fx_render(&fv, 0, 0);
        /* THE MARKERS ARE THE FRAME'S TOP LAYER, above the world and below the
           UI -- where the composite draws them, and where tagpu_vk.c records
           them. `markOn` rather than a vertex count, because this pass's own
           heartbeat has to run even on a frame with no markers: without it the
           watchdog reads a dead pass and hands the draw back to the engine. */
        if (markOn) tagpu_mark_render(&fv);
        /* AND THE UNIT PASS, last of the world's five. Its three body-path entry
           points gate their own GL; the shadow, slant, wire and depth ones are
           the composite's and are not called here at all. `begin` opens the
           recording window and arms the A/B, `unit` records each pose, `end`
           publishes -- which is the whole of the hand-over. */
        /* THIS LANE'S OWN HEARTBEAT, the composite's 300-frame stats line
           being below the exit and so unreachable here. It reports what was
           handed OVER rather than what was drawn, which is the only thing
           this lane decides. */
        if ((f->frame_counter % 300) == 0) {
            char hb[176];
            _snprintf(hb, sizeof hb,
                      "native: vulkan lane handed over frame %u: terr=%d feat=%d "
                      "fx=%d mark=%d units=%d posed=%d", f->frame_counter,
                      nterr, nfeat, nfx, markOn ? 1 : 0, nu, npd);
            hb[sizeof hb - 1] = 0;
            nlog(hb);
        }
        if (npd) {
            int k;
            tagpu_posedraw_begin(&s_pv);
            for (k = 0; k < npd; k++) tagpu_posedraw_unit(&pdu[k]);
            tagpu_posedraw_end();
        }
        /* AND THE BUILD GHOST, in its own window after the units' [landing
           11-3]. It is AFTER on purpose and not merely by habit: ghosts blend
           with each other and with what they sit on, so they are recorded last
           exactly as the GL pass drew them last. `_begin`/`_end` is a window
           rather than a frame (tagpu_posedraw.h), and with no posed unit on
           screen this is the first and only one. */
        ghost_record(pk, f->frame_counter, eyeX, eyeY, vpL, vpT, r0,
                     evpL, evpT, evw, evh);
        return;
    }

    /* THE GL DRAW HALF OF THE UNIT PASS STOOD HERE -- 1 054 lines, deleted by
       the vulkan-only plan's landing 11-3. Everything above this point is the
       GATHER half and still runs on the Vulkan lane; the hand-over and its
       `return` are the last thing in the function now.

       IT WAS ALREADY UNREACHABLE WHEN IT WAS DELETED, and that is the whole
       safety argument rather than a hope about coverage. Every draw below was
       reached only by falling past `if (!gl_draws)`, `gl_draws` is
       `!tagpu_vk_owns_present()`, this pass runs only from
       `tagpu_overlay_draw`, and landing 11-2 removed that function's only
       other caller (`render_ogl.c:1632`). The one that survives,
       `render_vk.c:232`, runs after `tagpu_vk_own_present()` has set
       `s_ownWin` at the top of the render thread -- so `gl_draws` is false at
       every call that exists, and the process creates no GL context on any
       path for it to have drawn into anyway.

       WHAT WENT WITH IT: the selection rects, the slant and silhouette shadow
       draws, the wire and ghost passes, the depth pre-pass, and the per-frame
       diagnostic log line -- plus the five file-local statics whose ONLY
       reader was that line (`s_reread`, `s_vtrunc`, `s_poseAtRest`,
       `s_poseUnplaced`, `s_poseNoBake`). Those were counted in the gather half
       and reset only down here, so on the Vulkan lane they had already been
       accumulating without a reader since gate 4; deleting the tail is what
       made that visible. Their increments went with them. */
}

/* The pose oracle, armed by tagpu_posedump.on (self-deleting): a one-shot
   numeric dump of the engine's per-piece pose for the first owned unit — its
   rest offset, MOVE delta and TURN triple, its raw node verts against the
   engine's own posed vbuf, and `err=`, the largest disagreement in model units
   between those posed verts and what pose_accum_body() reconstructs from the
   fields. That last number is the whole point: it is how the transform
   convention was solved in the first place (research/notes/model-import.md),
   and it is how a unit whose script does something no sample covered — a piece
   MOVEd, two turn axes at once — says so, instead of just rendering slightly
   wrong. err should read 0.00. */
/* THE ENGINE'S POSED VERTEX BUFFER IS NO LONGER READ HERE (frame packet
   exchange, landing 3). `err=` compared our reconstruction against
   PrimitiveStruct+0x22 — the buffer the engine rewrites in place, inside the
   Object3do that FreeObjectState can take away mid-frame — and it was the last
   per-unit engine read left in this file. What the dump is FOR survives whole:
   `tools/tacob pose-check` diffs tacob's own reconstruction against the FIELDS
   printed below (the rest offset, the COB move and turn, the visibility bit),
   and `tagpu_posecrc.on` watches the matrices this pass hands the GPU. The
   `err=` column is a gap this landing leaves open, not one it closes:
   recovering it wants the posed buffer copied into the packet under the
   trigger, which is a table nothing else would use. */
static void pose_dump(const TAGPU_PACKET* pk, const TAGPU_PK_UNIT* u,
                      const TAGPU_PK_PIECE* pc, int nparts)
{
    if (GetFileAttributesA("tagpu_posedump.on") == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA("tagpu_posedump.on");
    static HPOSE h;                        /* see HPOSE: not a 17 kB local */
    /* THE BODY TURN IS ALL THREE WORDS, AND IT IS THE CACHED COPY. The compose
       folds `o3+0x18/+0x1A/+0x1C` into the base piece's own turn at 0x45B0DB
       -- +0x18 onto the Z word, +0x1A (the heading) onto Y, +0x1C onto X -- so
       a reconstruction that applies the heading alone is short a bank and a
       pitch, and on ordinary ground the terrain's tilt puts tens of degrees
       there (three parked ARMSTUMPs read -22.1, -30.7 and +1.8 degrees of
       +0x68). This dump used to rotate model space by `unit+0x66` and report
       what was left; that omission, not any staleness in the vertex buffer, is
       the whole of the residual the eight fixtures recorded -- recovered from
       the fixtures' own base pieces 2026-09-08, every class to exactly 0.
       `bt` is therefore built exactly as recon_begin builds it. `err=` is now
       built from the SAME RECONSTRUCTION recon_err uses -- not the same
       number: recon_err skips a piece whose visible bit is clear and compares
       recon_prim's 16.16-ROUNDED output, while this reports every piece,
       ` HIDDEN` ones included, differenced in float. */
    const unsigned short* bturn = u->bturn;
    unsigned short bt[3];
    int ok;
    char b[288];
    bt[0] = bturn[2];                       /* +0x1C = unit+0x68, about X */
    bt[1] = bturn[1];                       /* +0x1A = unit+0x66, about Y */
    bt[2] = bturn[0];                       /* +0x18 = unit+0x64, about Z */
    ok = pose_accum_body(pc, nparts, u->base_piece, &h, bt);
    /* tick and in-game index first, so the line joins tagpu_cobtrace.log's
       (tick, unit) columns; the tick is read here on the render thread, so
       it names the sim tick this pass sampled, which may be the one before
       the pose's last update or the one after */
    /* `body` and `live` are printed in AXIS order (x, y, z) -- the order a
       piece's own turn triple is indexed in, so `tacob pose-check` passes
       `body` through unchanged. `body` is the cached copy the compose actually
       folds; `live` is the unit's own `+0x68/+0x66/+0x64` beside it, because
       the two are NOT the same on an aircraft: the fixtures' recovered heading
       matched `yaw` to within 2 units on the kbot, tank, building, ship and
       sub and was 1.4, 91.1 and 148.0 degrees away from it on the fighter,
       gunship and bomber. Which of the two diverges, and why, is not
       established -- this line is what will say. `yaw=` is kept, and is the
       live heading, so a fixture written before this change still parses. */
    _snprintf(b, sizeof b,
              "posedump: tick=%d idx=%d o3=%08X nparts=%d yaw=%u "
              "body=(%u,%u,%u) live=(%u,%u,%u)",
              (int)pk->tick, (int)u->id, u->o3_key, nparts,
              (unsigned)u->rot[1],
              (unsigned)bt[0], (unsigned)bt[1], (unsigned)bt[2],
              (unsigned)u->rot[2], (unsigned)u->rot[1], (unsigned)u->rot[0]);
    nlog(b);
    if (!ok) { nlog("posedump: pose_accum_body refused this unit"); return; }
    int p;
    for (p = 0; p < nparts && p < 32; p++) {
        const char* nd = h.nd[p];
        const int*  of = (const int*)(nd + N_OFF);
        const int*  mv = pc[p].pos;
        const unsigned short* tn = pc[p].turn;
        const char* nm = *(const char* const*)(nd + N_NAME);
        _snprintf(b, sizeof b,
                  "posedump: p%d %s%s off=(%d,%d,%d) move=(%d,%d,%d) "
                  "turn=(%u,%u,%u)",
                  p, ptr_ok(nm) ? nm : "?",
                  (pc[p].flags & 1) ? "" : " HIDDEN",
                  of[0], of[1], of[2], mv[0], mv[1], mv[2],
                  (unsigned)tn[0], (unsigned)tn[1], (unsigned)tn[2]);
        nlog(b);
    }
}

/* game-thread readable flag: the owndraw classifier suppresses the engine's
   scratch-fake-unit wreck rasterise only while the native husk pass is armed */
/* the fork restarts its render thread (NEW GL context) on every display-mode
   change — all our GL ids die; re-init from scratch on the next frame */
void tagpu_native_glreset(void)
{
    s_state = 0; s_fboW = s_fboH = s_fboSS = 0; s_palInit = 0;
    s_fogCols = s_fogRows = 0; s_fogCells = 0; s_fogGrid = NULL; s_fogLut = 0;
    tagpu_rglsl_glreset();      /* first: the passes below forget their jobs */
    tagpu_fx_glreset();
    tagpu_feat_glreset();
    tagpu_terr_glreset();
    tagpu_shadow_glreset();
    s_castLogged = 0;
    tagpu_mark_glreset();
    tagpu_hires_draw_glreset();
    tagpu_posebake_glreset();
    tagpu_posedraw_glreset();
}

int tagpu_native_wrecks_armed(void)
{
    return s_armed > 0 && s_state != 2 && s_wrecks;
}

/* THE FOG GRID AS THE NATIVE PASS UPLOADED IT (Phase G / G19e, the unit pass).
   The very buffer glTexImage2D/glTexSubImage2D above was given, with the
   dimensions it was given and the CELL COUNT the packet's own allocation
   holds -- the bound a copy of it has to be made against. That count is
   `cols * rows` at every one of its assignments today (`bufCells`, and the
   wide grid re-derives it from its own pair); it is published separately
   because it is the ALLOCATION's number and the dimensions are the IMAGE's,
   and a caller bounding a memcpy wants the former. NULL on a frame with no
   grid. Render thread only, and valid for the frame that uploaded it: the
   pointer is into a frame packet. */
const unsigned short* tagpu_native_foggrid(int* cols, int* rows, int* cells)
{
    if (cols) *cols = s_fogCols;
    if (rows) *rows = s_fogRows;
    if (cells) *cells = s_fogCells;
    return s_fogGrid;
}

const unsigned char* tagpu_native_foglut(void)
{
    return s_fogLutHave ? s_fogLutBytes : NULL;
}

int tagpu_native_worldtgt(TAGPU_WORLDTGT* out)
{
    if (!s_wtHave || !out) return 0;
    if (s_wt.gw < 1 || s_wt.gh < 1 || s_wt.ss < 1) return 0;
    *out = s_wt;
    return 1;
}

int tagpu_native_scissor_on(void)
{
    return s_scissorOn;
}

int tagpu_native_selbox_complete(void) { return s_selComplete; }

/* One unit's world position for a pass that draws something ANCHORED to it —
   the interpolated sample when this frame's unit gather produced one, the raw
   16.16 otherwise. Outputs are world units: x = world x, y = ALTITUDE,
   z = map depth, i.e. the same triple the engine keeps at unit+0x6A and the
   same order its order-list walker copies into `pos`.

   Honours tagpu_subpix.off, because it goes through the same table the lever
   controls: with sub-pixel off nothing is ever written, `tp` stays 0 and every
   call falls through to the raw read.

   THE SLOT CHECK IS NOT OPTIONAL. `s_spx` is indexed by array slot, and a slot
   outlives the unit that filled it: a sample belonging to a dead unit would be
   handed to whatever the engine put in its place. `spx_sample`'s distance snap
   catches the large jump, and the equality test below catches the rest by
   refusing any sample that does not still describe THIS unit's current
   position. 0 means "no position" — callers must not use the outputs. */
int tagpu_native_unit_pos(const TAGPU_PK_UNIT* u, float* x, float* y, float* z)
{
    int ix, iz, iy;
    float fx, fz, fy;

    if (!u || !x || !y || !z) return 0;
    ix = u->pos[0];
    iz = u->pos[1];
    iy = u->pos[2];
    fx = (float)ix / 65536.0f;
    fz = (float)iz / 65536.0f;
    fy = (float)iy / 65536.0f;

    /* THE SLOT CHECK IS NOT OPTIONAL, and the packet does not retire it. The
       table is indexed by the engine's array slot and a slot outlives the unit
       that filled it, so a sample belonging to a dead unit would be handed to
       whatever took its place: the equality test refuses any sample that does
       not still describe THIS unit's current 16.16 position, and spx_sample's
       own distance snap catches the large jump. What the packet does retire is
       the bounds check the pointer form needed — a slot is now a number the
       publisher took out of its own walk, not an address to validate. */
    {
        unsigned slot = u->slot;
        if (slot < 8192 && s_spx[slot].x == ix && s_spx[slot].z == iz &&
            s_spx[slot].y == iy)
            spx_sample(slot, s_spxFrame, &fx, &fz, &fy);
    }
    *x = fx; *y = fz; *z = fy;
    return 1;
}
