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
#include "tagpu_feat.h"
#include "tagpu_terr.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_terrown.h"
#include "tagpu_mark.h"
#include "tagpu_markown.h"
#include "tagpu_order.h"
#include "tagpu_owndraw.h"   /* tagpu_owndraw_structshadow_ours: who draws a building's shadow */
#include "tagpu_reclaim.h"   /* tagpu_reclaim_level_gen: the model templates outlive units, not levels */
#include "tagpu_posebake.h"
#include "tagpu_posedraw.h"  /* G16 step 4: the per-type geometry bake and its caches */
#include "tagpu_glsl.h"
#include "tagpu_zoom.h"
#include "tagpu_overlay.h"   /* tagpu_overlay_target_fbo: the frame's default draw target */
#include "tagpu_pal.h"       /* the palette the screen is SHOWN with, not main+0x143A7 */
#include "tagpu_vpwide.h"
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
#define TAPROG_PP    0x0051FBD0u
#define PROG_FOGLUT  0xCC      /* u8[256] fog shade remap (0x4BFE10)        */
#define OFF_FOGGRID  0x1421F   /* -> {u16* buf, cols, rows, cells}: the      
                                  engine screen fog grid, 4-bit corner masks
                                  per 32-px view cell (terrain-depth.md 5)  */
#define OFF_LOSTYPE  0x14281   /* u16: b0 mapping, b1 true LOS              */
#define OFF_WATCHED  0x2A42    /* byte watched player id                    */
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
#define O3_COMPOSITE 0x10      /* GAFFrame* per-unit composite              */
#define GF_PTRDEPTH  0x14      /* u8* depth plane; NULL = path A (no clip)  */
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
static void pose_dump(const char* u, const char* o3);

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
static unsigned s_reread = 0;

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
static int    s_armed = -1;
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
static int    s_devresFailed = 0;      /* the driver refused the supersampled target: stay down */
static GLuint s_fogTex, s_fogLutTex, s_cprog, s_cvao, s_cvbo;
static GLuint s_fbo2, s_colTex2, s_depTex2, s_dprog;
static GLint  s_uGame, s_uShadow, s_uAlpha, s_uFog, s_uFogOrg, s_uFogDim,
              s_uScafOn, s_uScafP;
static GLint  s_uWaterT, s_uWaterMode, s_uDigT;
static GLint  s_uNanoOn, s_uNanoT, s_uNanoC;
static GLint  s_uLit, s_uLambert, s_uSun, s_uAmb, s_uNorm;  /* Classic++ lighting */
static GLint  s_uRestored;                          /* Classic++: the unit atlas's twin */
static GLint  s_uOffset, s_uSS, s_uZoom, s_uZoomC, s_uZoomF, s_uZoomCF, s_uDepthScale;
static GLint  s_uCKey = -1, s_uCSurfSz = -1, s_uCVp = -1;  /* composite: the key */
static float  s_zoom = 1.0f;
static int    s_fboW = 0, s_fboH = 0, s_fboSS = 0;
static int    s_palInit = 0;
static int    s_fogCols = 0, s_fogRows = 0, s_fogOrgX = 0, s_fogOrgY = 0;
static const unsigned short* s_fogGrid = NULL;
static int    s_fogLut = 0;   /* grey remap uploaded this frame (logged) */
static unsigned s_fillSeq = 0;   /* terrain key-fill sequence + stall counter */
static int      s_fillStall = 0;
/* world origin of fog grid cell 0 on one axis: the builder's rounded eye>>5
   turned back into world px, i.e. 32*col0 + 16 (0x4843C0 head, 0x4848E0).
   `%` truncating toward zero is DELIBERATE, not a floor-mod bug: the builder
   computes col0 as ((eye + (sign & 31)) >> 5) - 1, which is C's truncating
   division, so a floor-based remainder would disagree with the engine for a
   negative eye (eye = -20: engine origin -16, floor would say -48). */
static int fog_org(int eye) { int r = eye % 32; return eye + (r > 15 ? 16 : -16) - r; }
static GLint  s_uCast;                 /* the caster's three numbers (G14i) */
static TAGPU_SHADOWU s_shU;            /* the shadow read-back uniforms      */
static float  s_verts[MAXNV * NVST];
/* set by the frame, read by tagpu_markown.c on the game thread: 1 while every
   selection box this frame owed was actually emitted */
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
    "    if (int(texelFetch(uSurf, p, 0).r * 255.0 + 0.5) != uKey) discard;\n"
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

static void fbo_size(int w, int h, int ss)
{
    if (w == s_fboW && h == s_fboH && ss == s_fboSS) return;
    glBindTexture(GL_TEXTURE_2D, s_colTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, s_depTex);
    /* DEPTH24_STENCIL8, not DEPTH_COMPONENT24: the shadow pass needs a stencil
       to blend each silhouette exactly once (see the shadow loop). The depth
       texture is an attachment only -- nothing samples it -- so the packed
       format costs nothing but the byte. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_colTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_depTex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLenum st2 = 0;
    if (ss > 1) {
        glBindTexture(GL_TEXTURE_2D, s_colTex2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * ss, h * ss, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, s_depTex2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w * ss, h * ss, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo2);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_colTex2, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_depTex2, 0);
        st2 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());
    s_fboW = w; s_fboH = h; s_fboSS = ss;
    { char b[96]; _snprintf(b, sizeof b, "native: FBO %dx%d ss=%d status=%x/%x",
                            w, h, ss, st, st2); nlog(b); }
    /* An incomplete SUPERSAMPLED target is survivable only while something
       still resolves it: `devres` composites straight out of it, so an FBO the
       driver refused (a 4x buffer is 4096x3072 here) would put a black world on
       the screen for the rest of the session. The size is cached either way, so
       this is not retried per frame; devres simply stands down and the ordinary
       resolve path — which composites the 1x target — carries the frame.
       (The landing review's point: nothing else notices a failed allocation.) */
    if (ss > 1 && st2 != GL_FRAMEBUFFER_COMPLETE && !s_devresFailed) {
        s_devresFailed = 1;            /* a LATCH: the poll below re-reads the trigger every 500 ms
                                          and would otherwise switch it straight back on */
        s_devres = 0;
        nlog("native: supersampled FBO incomplete — devres off, the resolve path carries the frame");
    }
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
int tagpu_native_owns_unit(const char* u)
{
    if (s_armed != 1) return 0;
    const char* def = *(const char* const*)(u + U_TYPE);
    if (!ptr_ok(def)) return 0;
    if (!type_match(def)) return 0;
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

static const char* model_root(const char* ta, const char* u)
{
    unsigned mid, n;
    const char* mptrs;
    if (!ptr_ok(ta) || !ptr_ok(u)) return NULL;
    mid = *(const unsigned short*)(u + U_MODELID);
    n   = *(const unsigned*)(ta + OFF_UDEFCOUNT);
    if (mid == 0 || n == 0 || n > 0x10000u || mid >= n) {
        if (mid) s_badModelId++;       /* 0 is "no model", not a refusal */
        return NULL;
    }
    mptrs = *(const char* const*)(ta + OFF_MODELPTRS);
    if (!ptr_ok(mptrs)) return NULL;
    return *(const char* const*)(mptrs + (size_t)mid * 4);   /* in bounds */
}

typedef struct { const char* node; float mn[3], mx[3]; } MAABB;
static MAABB s_aabb[256];              /* every caster asks, once per model (G14i) */
static int   s_naabb = 0;

/* NO PER-NODE READABILITY PROBE HERE, ON PURPOSE. An IsBadReadPtr before each
   dereference would be a check whose answer can go stale between the check and
   the read — it would make a fault rarer without making it impossible, which
   is the shape of fix this project does not take (CLAUDE.md, "Fixes must be
   safe by construction"). The walk is safe because of what it is handed: a
   root that came through model_root, so it is a slot of the engine's own model
   table, and the tree hanging off a live template is internally consistent —
   the engine's own equivalent walk (0x4CB650) probes nothing either.

   The two loop bounds below are data bounds, not memory probes: they stop a
   malformed model from running away, and they hold whatever the file contains.

   The lifetime the argument rests on is the LEVEL: 0x42DB90 frees every
   template in the teardown cascade, and the render thread is held out of that
   window by tagpu_reclaim's teardown wrap (thread-safe-destruction.md §6a).
   That hold has one timing-dependent hole left — the pre hook's 1 s timeout,
   after which the cascade frees the templates with a render pass still
   running. It is pre-existing and is recorded there as an open item; it is not
   something this walk can close on its own. */
static void aabb_walk(const char* nd, float ox, float oy, float oz,
                      float* mn, float* mx, int depth)
{
    int sib = 0;
    if (!ptr_ok(nd) || depth > 24) return;
    for (; ptr_ok(nd) && sib < 64; sib++,
         nd = *(const char* const*)(nd + 0x2C)) {                 /* sibling */
        float px = ox + (float)*(const int*)(nd + 0x10) / 65536.0f;
        float py = oy + (float)*(const int*)(nd + 0x14) / 65536.0f;
        float pz = oz + (float)*(const int*)(nd + 0x18) / 65536.0f;
        int nvert = *(const int*)(nd + 0x04);
        const int* vb = *(const int* const*)(nd + 0x24);
        if (nvert > 0 && nvert <= 1024 && ptr_ok(vb) &&
            !IsBadReadPtr(vb, (SIZE_T)nvert * 12)) {
            int i;
            for (i = 0; i < nvert; i++) {
                float x = px + (float)vb[i*3+0] / 65536.0f;
                float y = py + (float)vb[i*3+1] / 65536.0f;
                float z = pz + (float)vb[i*3+2] / 65536.0f;
                if (x < mn[0]) mn[0] = x; if (x > mx[0]) mx[0] = x;
                if (y < mn[1]) mn[1] = y; if (y > mx[1]) mx[1] = y;
                if (z < mn[2]) mn[2] = z; if (z > mx[2]) mx[2] = z;
            }
        }
        const char* ch = *(const char* const*)(nd + 0x30);        /* child */
        if (ptr_ok(ch)) aabb_walk(ch, px, py, pz, mn, mx, depth + 1);
        if (depth == 0) break;    /* root has no meaningful siblings */
    }
}

/* The select box's bounds are NOT this whole-tree walk, and the difference is
   ~11 px on a Stumpy. `DrawUnitSelectBoxRect` asks `0x4CB650(model,&min,&max,0)`
   and that routine:

     - seeds BOTH min and max with {0,0,0} (`0x4CB65D`..`0x4CB675`), so the model
       origin is always inside the box;
     - accumulates only nodes with THREE OR MORE vertices (`0x4CB6D9`
       `cmp $2,eax; jle`) — the same threshold that decides a piece is drawable;
     - and descends into the child (`node+0x30`) and the sibling (`node+0x2C`)
       only when its flag argument is non-zero (`0x4CB780` `test ebp,ebp; je`).
       The select box passes **0** (`push $0` @`0x46A55A`), so the walk stops at
       the root: the rect is the ROOT PIECE's own vertices, offset by its own
       `+0x10/14/18`, unioned with the origin — never the turret, the barrel or
       anything else hanging off it.

   Kept apart from model_aabb() rather than folded into it: that one IS the whole
   tree, which is what the shadow pass's model height wants (`mx[1]`, measured
   against the lab), and the two must not drift into each other. */
static MAABB s_sbox[256];
static int   s_nsbox = 0;

static const MAABB* selbox_aabb(const char* nd)
{
    int i;
    for (i = 0; i < s_nsbox; i++)
        if (s_sbox[i].node == nd) return &s_sbox[i];
    if (s_nsbox >= 256 || !ptr_ok(nd) || IsBadReadPtr(nd, 0x40)) return NULL;
    MAABB* a = &s_sbox[s_nsbox];
    a->node = nd;
    a->mn[0] = a->mn[1] = a->mn[2] = 0.0f;      /* the engine's {0,0,0} seed */
    a->mx[0] = a->mx[1] = a->mx[2] = 0.0f;
    {
        int nvert = *(const int*)(nd + N_VCOUNT);
        const int* vb = *(const int* const*)(nd + N_VERTS);
        /* Fewer than three vertices is the ENGINE'S OWN answer (0x4CB6D9) and
           caches as the bare origin seed. An unreadable vertex array is not an
           answer at all, and caching one would be permanent: the entry is keyed
           by the node pointer and never re-tried, so that model would carry a
           zero-size rect for the life of the process — and worse, silently,
           because `selDrawn` would still count it and `s_selComplete` would
           stay 1, leaving markown suppressing the engine's box over nothing.
           Refuse instead: the caller skips the unit, the completeness flag goes
           false, and the whole set goes back to the engine for that frame. */
        if (nvert > 2) {
            const int* of = (const int*)(nd + N_OFF);
            int k, r;
            if (nvert > 4096 || !ptr_ok(vb) ||
                IsBadReadPtr(vb, (SIZE_T)nvert * 12)) return NULL;
            for (k = 0; k < nvert; k++)
                for (r = 0; r < 3; r++) {
                    float v = (float)(of[r] + vb[k*3+r]) / 65536.0f;
                    if (v < a->mn[r]) a->mn[r] = v;
                    if (v > a->mx[r]) a->mx[r] = v;
                }
        }
    }
    s_nsbox++;
    return a;
}

static const MAABB* model_aabb(const char* root)
{
    int i;
    for (i = 0; i < s_naabb; i++)
        if (s_aabb[i].node == root) return &s_aabb[i];
    if (s_naabb >= 256) return NULL;
    MAABB* a = &s_aabb[s_naabb];
    a->node = root;
    a->mn[0] = a->mn[1] = a->mn[2] = 1e9f;
    a->mx[0] = a->mx[1] = a->mx[2] = -1e9f;
    aabb_walk(root, 0, 0, 0, a->mn, a->mx, 0);
    if (a->mn[0] > a->mx[0]) return NULL;      /* nothing valid found */
    s_naabb++;
    return a;
}

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
static int s_vtrunc = 0;            /* vertex budget hit this frame (logged) */
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
static unsigned s_poseAtRest = 0;
static unsigned s_poseUnplaced = 0;
/*   nobake= units the gather could not get a bake for, which since step 8
             means they DRAW NOTHING — the one honest drop in the ledger. It
             has to be counted whether or not tagpu_posebake.on is armed,
             because without a count an undrawable model is a unit that is
             simply missing from the screen with nothing in the log. */
static unsigned s_poseNoBake = 0;
/*   q=      units the gather QUEUED against what the pass actually drew. Since
             step 8 a queued unit the draw drops is a unit missing from the
             screen, so the two numbers have to be visible together; printed
             only when they disagree. */
static unsigned s_poseQueued = 0;
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
static float s_castLogX = -1.0f;    /* ...one line per caster position seen */
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
            if (nv + 3 > MAXNV) { s_vtrunc = 1; return nv; }
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

/* ---- the three caches keyed on a MODEL TEMPLATE, and the level they belong to
   `s_aabb` (the whole-tree AABB the shadow's height rule reads), `s_sbox` (the
   select box's own bounds) and `s_pmap` (a replacement mesh's glTF piece ->
   engine primitive map) are all keyed on a raw `Model3DONode*`. That tree is
   shared by every unit of a type, so it rightly outlives any unit — but it does
   NOT outlive the LEVEL, and it is not freed through `FreeObjectState`, so
   `tagpu_reclaim`'s deferral does not cover it. Until this check existed
   nothing dropped these entries at all: a second level whose allocator handed
   the same address to a different model was served the first level's answer,
   for the rest of the process. That is not a fault -- it is a wrong shadow
   height, a wrong select box and a mis-bound replacement pose, silently.

   The cure is the level generation `tagpu_reclaim` bumps for the teardown
   `0x491B60` -- in its POST hook, after the cascade has freed the templates,
   which matters: a bump in the pre hook is observed by a pass that is already
   past `tagpu_overlay.c`'s teardown gate and still running (the pre hook is
   waiting for exactly that pass), and that pass would drop these caches and
   refill them from templates about to be freed, stamping the new generation on
   stale entries. Checked once per frame rather than per lookup: every one of
   these caches is consulted only from tagpu_native_frame's own call tree.

   They hold no GL objects, so dropping them is resetting three counts; the
   entries rebuild on the next frame that asks. */
static unsigned s_cacheGen;              /* the level s_aabb/s_sbox/s_pmap describe */

static void cache_gen_check(void)
{
    unsigned g = tagpu_reclaim_level_gen();
    if (g == s_cacheGen) return;
    if (s_naabb || s_nsbox || s_npmap) {
        char b[160];
        _snprintf(b, sizeof b,
                  "native: level %u -> %u, dropping the template caches: aabb=%d selbox=%d pmap=%d",
                  s_cacheGen, g, s_naabb, s_nsbox, s_npmap);
        nlog(b);
    }
    s_cacheGen = g;
    s_naabb = s_nsbox = s_npmap = 0;
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
    const char* nd[TAGPU_PBMAXPIECE];
    const char* pr[TAGPU_PBMAXPIECE];
    float acc[TAGPU_PBMAXPIECE][12];   /* rest vertex of that piece -> model space */
    float rest[TAGPU_PBMAXPIECE][3];   /* accumulated rest offset                  */
    unsigned char done[TAGPU_PBMAXPIECE]; /* 0 = tree link broken, piece at rest    */
} HPOSE;

/* `bt` non-NULL folds the body turn into the BASE piece's own turn, exactly
   where the compose adds it (0x45B0DB, only on the top-level call) -- the
   reconstruction needs it because P_VBUF holds the body-rotated pose, while
   hires_pose and pose_dump want model space and pass NULL. */
static int pose_accum_body(const char* o3, HPOSE* h, const unsigned short* bt)
{
    static short parent[TAGPU_PBMAXPIECE];   /* render thread only, as HPOSE is */
    const char* basePrim = bt ? *(const char* const*)(o3 + O3_BASEPRIM) : NULL;
    int nparts = *(const unsigned short*)(o3 + O3_NUMPARTS);
    const char** nd = h->nd;
    const char** pr = h->pr;
    int i, g, left, pass;
    if (nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    for (i = 0; i < nparts; i++) {
        pr[i] = o3 + O3_PRIM0 + i * PRIM_STRIDE;
        nd[i] = *(const char* const*)(pr[i] + P_NODE);
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
                const int* mv  = (const int*)(pr[i] + P_POS);
                const unsigned short* tn = (const unsigned short*)(pr[i] + P_TURN);
                unsigned short bturn[3];
                float d[3], loc[12];
                int k;
                for (k = 0; k < 3; k++)
                    d[k] = (float)off[k] / 65536.0f + (float)mv[k] / 65536.0f;
                if (basePrim && pr[i] == basePrim) {
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
static int posed_pose(const char* o3, const TAGPU_PBGEOM* g,
                      float* out, unsigned char* shaded, unsigned char* pvis)
{
    static float acc[TAGPU_PBMAXPIECE][12];      /* render thread only */
    static unsigned char done[TAGPU_PBMAXPIECE];
    static const char* pr[TAGPU_PBMAXPIECE];
    static const char* nd[TAGPU_PBMAXPIECE];
    const unsigned short* bturn;
    unsigned short bt[3];
    const char* basePrim;
    int nparts, i, pass, left, anyShadeFlag = 0;

    if (!ptr_ok(o3)) return 0;
    /* the bake's count, not a fresh read of the unit's: tagpu_posebake_unit
       keyed the entry on it, so the two agree by construction and a second
       read could only disagree by catching a recycled slot */
    nparts = g->nparts;
    if (nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    bturn = (const unsigned short*)(o3 + O3_BTURN);
    bt[0] = bturn[2];                       /* +0x1C = unit+0x68, about X */
    bt[1] = bturn[1];                       /* +0x1A = unit+0x66, about Y */
    bt[2] = bturn[0];                       /* +0x18 = unit+0x64, about Z */
    basePrim = *(const char* const*)(o3 + O3_BASEPRIM);
    for (i = 0; i < nparts; i++) {
        unsigned char fl;
        pr[i] = o3 + O3_PRIM0 + i * PRIM_STRIDE;
        nd[i] = *(const char* const*)(pr[i] + P_NODE);
        /* a node that does not read leaves THAT PIECE at rest rather than
           dropping the unit: `done[i]` stays 0 and the identity is written
           for it below, the same degradation an unresolved parent takes.
           The template's lifetime is tagpu_reclaim's deferral plus the level
           generation the bake is keyed by; this is a value filter. */
        if (!ptr_ok(nd[i])) { nd[i] = NULL; }
        done[i] = 0;
        fl = *(const unsigned char*)(pr[i] + P_FLAGS);
        if ((fl & 1) && (fl & 4)) anyShadeFlag = 1;
    }
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
            mv  = (const int*)(pr[i] + P_POS);
            tn  = (const unsigned short*)(pr[i] + P_TURN);
            for (k = 0; k < 3; k++)
                d[k] = (float)off[k] / 65536.0f + (float)mv[k] / 65536.0f;
            if (basePrim && pr[i] == basePrim) {
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
            s_poseUnplaced++;
        }
    }
    /* out: the piece's matrix, or all zeros for a piece this unit is not
       showing — which collapses its triangles onto the model origin, the same
       vertices `emit_geom_at`'s `if (!(pflags & 1)) continue` never emitted */
    for (i = 0; i < nparts; i++) {
        unsigned char fl = *(const unsigned char*)(pr[i] + P_FLAGS);
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
static int hires_pose(const char* o3, const void* mesh, float* out, int npiece)
{
    static HPOSE h;                        /* 17 kB at TAGPU_PBMAXPIECE: not a local */
    unsigned short bt[3];
    int g, nparts;
    if (!ptr_ok(o3) || IsBadReadPtr(o3, O3_PRIM0)) return 0;
    {
        const unsigned short* bturn = (const unsigned short*)(o3 + O3_BTURN);
        bt[0] = bturn[2];                  /* +0x1C = unit+0x68, about X */
        bt[1] = bturn[1];                  /* +0x1A = unit+0x66, about Y */
        bt[2] = bturn[0];                  /* +0x18 = unit+0x64, about Z */
    }
    nparts = pose_accum_body(o3, &h, bt);
    if (!nparts) return 0;
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
            if (!(*(const unsigned char*)(h.pr[e] + P_FLAGS) & 1)) {
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

void tagpu_native_frame(const TAGPU_FRAME* f)
{
    /* before the early-out and before any gather: a level that ended while this
       pass was disarmed still invalidates the template caches, and the check is
       one aligned load when nothing has changed */
    cache_gen_check();
    /* the geometry bake's caches take the same three generations one frame
       later than they are bumped, for the same reason and on the same thread —
       and it holds GL objects, so its drop has to be here, on the render
       thread, and not wherever the generation moved */
    tagpu_posebake_frame(f->frame_counter);
    pose_rest_block_init();      /* the degradation's block, once per session */
    tagpu_posedraw_frame();      /* this frame's counters */
    if (s_state == 2) return;
    if (s_armed < 0 || (f->frame_counter % 30) == 0) {
        int was = s_armed;
        s_armed = 0;
        char buf[64];
        int n = tagpu_opt_read("tagpu_native.on", buf, sizeof buf);
        if (n >= 0) {
            if (n > 0) {
                int i = 0; while (buf[i] && buf[i] > ' ') i++;
                s_wrecks = 0;
                if (i > 0 && i < 32) {
                    buf[i] = 0; lstrcpyA(s_type, buf);
                    /* extra tokens: "wrecks" arms the native husk pass */
                    char* p = buf + i + 1;
                    while (p < buf + n) {
                        while (*p && *p <= ' ') p++;
                        char* q = p;
                        while (*q && *q > ' ') q++;
                        int last = (*q == 0);
                        *q = 0;
                        if (!lstrcmpiA(p, "wrecks")) s_wrecks = 1;
                        if (last) break;
                        p = q + 1;
                    }
                }
            }
            s_armed = 1;
        }
        s_ss     = (GetFileAttributesA("tagpu_ss.off")     == INVALID_FILE_ATTRIBUTES);
        /* THE DEVICE-RESOLUTION WORLD IS OPT-IN, and the reason is the selection
           rects. The comment by the rect draw records the measurement: the
           driver clamps an aliased GL line to one pixel, so a line in an
           ss-times buffer is one SUPERSAMPLE wide and resolves to a half-lit
           smear — which is exactly why `selAt1x` draws them into the 1x FBO
           after the box-downsample instead. Under devres there is no such
           downsample, so the rects would reach the screen thinner and dimmer
           than the engine's (about 0.75 of a device pixel at k = 1.5) while
           everything else got sharper. Two landing reviewers found this
           independently. `tagpu_devres.on` is how it was measured; making it
           the default waits on drawing the rects as real geometry with a
           width, which is its own piece of work. */
        s_devres = !s_devresFailed &&
                   (GetFileAttributesA("tagpu_devres.on") != INVALID_FILE_ATTRIBUTES);
        s_subpix = (GetFileAttributesA("tagpu_subpix.off") == INVALID_FILE_ATTRIBUTES);
        s_spxlog = (GetFileAttributesA("tagpu_spxlog.on")  != INVALID_FILE_ATTRIBUTES);
        s_nano   = (GetFileAttributesA("tagpu_nano.off")   == INVALID_FILE_ATTRIBUTES);
        if (s_armed != was && was >= 0) {
            char b[96]; _snprintf(b, sizeof b, "native: %s (type=%s wrecks=%d ss=%d subpix=%d)",
                                  s_armed ? "ARMED" : "disarmed", s_type,
                                  s_wrecks, s_ss, s_subpix);
            nlog(b);
        }
    }
    /* View zoom — re-read EVERY frame, unlike the arm state above. It is a
       continuous control, so a 30-frame poll would quantise any ramp to 2 Hz and
       make a perfectly smooth renderer look like a staircase on video. The lever
       and the transform built on it live in tagpu_zoom.c, because the input path
       needs exactly the same numbers and the two must never disagree. */
    s_zoom = tagpu_zoom_read_lever();

    /* the effects pass (tagpu_fx.on) rides this frame: it needs the view,
       fog and palette set up here and draws into this FBO */
    int fxOn = tagpu_fx_armed(f->frame_counter);
    int sfxOn = tagpu_sfx_armed(f->frame_counter);
    int featOn = tagpu_feat_armed(f->frame_counter);
    int terrOn = tagpu_terr_armed(f->frame_counter);
    int markOn = tagpu_mark_armed(f->frame_counter);
    /* polled unconditionally, not behind markOn: tagpu_order.c hands the
       engine's driver back from its own disarm path, and it can only do that
       if it is still being asked */
    tagpu_order_armed(f->frame_counter);
    if (!s_armed && !fxOn && !sfxOn && !featOn && !terrOn && !markOn) return;
    if (s_state == 0) init_gl();
    if (s_state != 1 || !tagpu_r3d_ensure()) return;

    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
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
    if (!pal) return;
    /* the unit atlas's frame: recycle if full, arm and step its Classic++
       restore -- before any face asks it for a UV */
    tagpu_r3d_atlas_frame(pal);
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    if (!ptr_ok(beg) || !ptr_ok(end) || end <= beg) return;
    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000) return;

    /* the TRUE 1x rect, not the field: while tagpu_vpwide is live the engine's
       copy is deliberately wider, and the composite key rect (uVp), the zoom's
       published view and the effective gather below all mean the real one */
    int vpL, vpT, vw, vh;
    tagpu_vpwide_true_rect(ta, &vpL, &vpT, &vw, &vh);
    int eyeX = *(int*)(ta + OFF_EYEX), eyeY = *(int*)(ta + OFF_EYEY);
    if (vw < 64 || vh < 64 || vw > 4096 || vh > 4096) return;
    int gw = f->game_width  > 0 ? f->game_width  : vpL + vw;
    int gh = f->game_height > 0 ? f->game_height : vpT + vh;

    /* ---- the viewport the zoom actually shows (see TAGPU_FXVIEW.evpL) ----
       Every gather below sizes itself from this rather than the engine's own
       viewport, so zooming out reaches further into the map instead of leaving
       the frame edge bare. At z >= 1 it IS the engine's viewport, bit for bit. */
    int evpL = vpL, evpT = vpT, evw = vw, evh = vh;
    if (s_zoom > 0.05f && s_zoom < 1.0f) {
        evw = (int)((float)vw / s_zoom) + 64;      /* +64: partial cells at the edge */
        evh = (int)((float)vh / s_zoom) + 64;
        if (evw > 8192) evw = 8192;
        if (evh > 8192) evh = 8192;
        /* and never ask the terrain pass for more cells than it can draw: it
           would bail, and a bail hands the whole draw back for a frame */
        tagpu_terr_clamp_span(&evw, &evh);
        /* the effective rect is only ever WIDER than the engine's — a clamp that
           took it below the viewport would cull content that is plainly on
           screen. Unreachable at any real resolution, stated so it stays true. */
        if (evw < vw) evw = vw;
        if (evh < vh) evh = vh;
        evpL = vpL + (vw - evw) / 2;
        evpT = vpT + (vh - evh) / 2;
    }

    int scafR0 = 0, scafRows = 0;
    int scafOn = tagpu_scaffold_frameinfo(f->frame_counter, &scafR0, &scafRows);
    int r0 = scafOn ? scafR0 : ((eyeY + (evpT - vpT)) >> 4) - 16;
    int rows = scafRows > 0 ? scafRows : (evh >> 4) + 32;
    /* this frame's depth bands (see ROW_SLACK) */
    float fxKey = 3.0f + (float)(rows + ROW_SLACK) * 4.0f + 4.0f;
    float airKey = fxKey + 12.0f;
    float depthScale = airKey + 8.0f;

    unsigned gfx = *(unsigned short*)(ta + OFF_GFXOPT);
    unsigned lostype = *(unsigned short*)(ta + OFF_LOSTYPE);
    int watched = *(unsigned char*)(ta + OFF_WATCHED);

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
    s_fogGrid = NULL; s_fogLut = 0;
    {
        const int* fg = *(const int* const*)(ta + OFF_FOGGRID);
        if (ptr_ok(fg) && !IsBadReadPtr(fg, 16)) {
            const unsigned short* buf = (const unsigned short*)(size_t)fg[0];
            int cols = fg[1], rows = fg[2], cells = fg[3];
            if (ptr_ok(buf) && cols > 0 && rows > 0 && cols <= 256 && rows <= 256 &&
                cells == cols * rows && !IsBadReadPtr(buf, (SIZE_T)cells * 2)) {
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
                s_fogGrid = buf; s_fogCols = cols; s_fogRows = rows;
                /* the overlay puts cell (0,0) at screen vp + (+-16 - eye%32);
                   in world terms 32*col0 + 16, col0 being the builder's
                   half-cell-rounded eye>>5. The grid lattice is offset half a
                   cell from the map cells: a corner IS a map cell's centre. */
                s_fogOrgX = fog_org(eyeX);
                s_fogOrgY = fog_org(eyeY);
                /* "fog is on" is NOT LosType bit0 — that bit is only the
                   MAPPING option. The overlay runs every frame and what it
                   paints is decided entirely by the grid bytes: the builder
                   writes the grey mask only when LosType&2 and the black mask
                   only where MAPPED is clear, so an inactive mode is already an
                   all-zero grid and costs nothing to sample. Gating on bit0
                   dropped the whole rule under true-LOS-without-mapping
                   (LosType=14, a reachable skirmish setting: LineOfSight cycle
                   stage 1 with the mapping option off) — and since G13b
                   suppresses the engine's overlay, that would delete the grey
                   band outright instead of merely disagreeing with it. */
                fogMode = 1 | (lostype & 2);
                /* the grey band's darken is a palette remap, not a scale:
                   0x4BFE10 rewrites every pixel p as shadeLUT[p] through
                   *(TAProgram+0xCC) (256 bytes, everything folded into the
                   dark-grey ramp). Cheap enough to re-upload each frame. */
                {
                    const char* prog = *(const char* const*)TAPROG_PP;
                    const unsigned char* t = NULL;
                    static unsigned char ident[256];
                    if ((size_t)prog > 0x400000u && !IsBadReadPtr(prog, 0x100)) {
                        const unsigned char* p = 
                            *(const unsigned char* const*)(prog + PROG_FOGLUT);
                        if (ptr_ok(p) && !IsBadReadPtr(p, 256)) t = p;
                    }
                    s_fogLut = t ? 1 : 0;
                    if (!t) {
                        /* identity, so an unreadable table degrades to "grey is
                           not darkened" — never to "every grey pixel is palette
                           index 0", which is solid black over the whole band */
                        int i;
                        for (i = 0; i < 256; i++) ident[i] = (unsigned char)i;
                        t = ident;
                    }
                    /* 256 bytes: always re-spec, so no init flag can survive a
                       context reset and leave the texture storageless (which
                       has the same all-black failure mode) */
                    x_glActiveTexture(GL_TEXTURE5);
                    glBindTexture(GL_TEXTURE_2D, s_fogLutTex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0,
                                 GL_RED, GL_UNSIGNED_BYTE, t);
                    x_glActiveTexture(GL_TEXTURE0);
                }
            }
        }
    }

    /* ---- the live palette (re-read per frame; it does NOT cycle -- terr.c),
       and this is the ONLY palette texture the world has: terrain, features,
       effects, markers and the replacement meshes are all handed s_palTex. ---- */
    {
        x_glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_palTex);
        if (!s_palInit) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pal); s_palInit = 1; }
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, pal);
        x_glActiveTexture(GL_TEXTURE0);
    }

    /* ---- gather native-owned on-screen units ---- */
    typedef struct { const char* o3; const char* u; const void* hires;
                     const char* rec;       /* the wreck record (feat), else NULL */
                     int dead;              /* object pointer moved since the gather */
                     float ax, ay, gy, wx0, wz0, wy, gnd;
                     int rel, owner, cloaked, air, feat, sel, shadow, slant; unsigned yaw;
                     float waterT, digT; int waterMode;
                     int nanoOn; float nanoT, nanoC[3], nanoWire; } NU;
    static NU units[MAXU];
    /* sub-pixel motion: see the SPX block at the top of this file for the
       table, the read half and why both are file-static now */
    s_spxFrame = f->frame_counter;
    int nu = 0, nv = 0, nwr = 0, nsel = 0;
    unsigned uiGates = *(unsigned char*)(ta + OFF_UIGATES);
    if (s_armed) {                 /* units are gathered only by the unit pass */
    for (char* u = beg + UNIT_STRIDE; u < end && nu < MAXU; u += UNIT_STRIDE) {
        unsigned st = *(unsigned*)(u + U_STATE);
        if (!(st & 0x10000000u) || (st & 0x4000u)) continue;
        if (!tagpu_native_owns_unit(u)) continue;
        char* o3 = *(char**)(u + U_OBJ3DO);
        if (!ptr_ok(o3)) continue;
        short wx = *(short*)(u + U_XPOS), wz = *(short*)(u + U_ZPOS), wy = *(short*)(u + U_YPOS);
        int ix = *(int*)(u + U_XFIX), iz = *(int*)(u + U_ZFIX), iy = *(int*)(u + U_YFIX);
        float fx = (float)ix / 65536.0f, fz = (float)iz / 65536.0f, fy = (float)iy / 65536.0f;
        if (s_subpix) {
            size_t slot = (size_t)(u - beg) / UNIT_STRIDE;
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
        int owner = *(unsigned char*)(u + U_OWNER);
        int cloaked = (*(unsigned char*)(u + U_CLOAKF) & 4) != 0;
        if (cloaked && owner != watched) continue;    /* enemies never see cloak */
        /* fog gate at the anchor tile: engine draws nothing there */
        if (fogMode & 1) {
            int fog = tagpu_fog_at(s_fogGrid, s_fogCols, s_fogRows,
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
        if (nu == 0) pose_dump(u, o3);
        NU* n2 = &units[nu++];
        n2->o3 = o3; n2->u = u; n2->ax = ax; n2->ay = ay; n2->rec = NULL; n2->dead = 0;
        n2->wx0 = fx; n2->wz0 = fy - fz * 0.5f;
        n2->wy = fz; n2->gnd = fz;          /* the ground under it, refined below */
        n2->yaw = *(const unsigned short*)(u + U_YAW);
        n2->hires = NULL;
        n2->shadow = 0; n2->slant = 0;
        /* build state: the engine's own staging for a unit under construction
           (build-state.md), shared with the composite path so the two cannot
           drift. The blit-time effect that would otherwise draw this at the 1x
           projection is skipped for every unit this pass owns — the third
           owndraw detour, on 0x458DD0. */
        n2->nanoOn = s_nano &&
                     tagpu_r3d_nano_state(u, &n2->nanoT, n2->nanoC, &n2->nanoWire);
        n2->waterT = -1e9f; n2->digT = -1e9f; n2->waterMode = 0;
        unsigned mask = 0;                    /* FBI booleans, read below */
        {
            const char* def = *(const char* const*)(u + U_TYPE);
            if (ptr_ok(def)) {
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
                mask = *(const unsigned*)(def + UD_TYPEMASK);
                /* a digger never reaches the cached branch: path A tests the
                   structure bit first and then sends a digger to the
                   COMPLETED branch (0x4592C8), path B tests digger before the
                   structure bit (0x4594D0) and clips a silhouette inline */
                n2->slant = (st & ST_STRUCT) != 0 && !(mask & UD_DIGGER);
                if (n2->slant) {
                    n2->shadow = tagpu_owndraw_structshadow_ours() &&
                                 !(mask & 0x02000000u);    /* noshadow          */
                    if (n2->shadow &&
                        *(const unsigned short*)(u + U_MODELID) == 0 &&
                        fz < (float)*(const unsigned char*)(ta + OFF_SEALEVEL))
                        n2->shadow = 0;
                } else {
                    n2->shadow = !(mask & 0x02000000u) &&  /* noshadow          */
                                 !(mask & 0x00081000u);    /* canhover|floater  */
                }
                char nm[32]; int ci;
                for (ci = 0; ci < 31; ci++) {
                    char cch = def[0x20 + ci];   /* UnitName, e.g. ARMSOLAR */
                    if (cch >= 'A' && cch <= 'Z') cch = (char)(cch + 32);
                    nm[ci] = cch;
                    if (!cch) break;
                }
                nm[31] = 0;
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
            const char* fr = *(const char* const*)(o3 + O3_COMPOSITE);
            if (ptr_ok(fr) && *(const unsigned*)(fr + GF_PTRDEPTH) != 0) {
                float sub = (float)*(const unsigned char*)(ta + OFF_SEALEVEL) - fz;
                if (sub > 0.0f) {
                    int local = *(const unsigned char*)(ta + OFF_LOCALPL);
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
            const char* fmap = *(const char* const*)(ta + OFF_FMAP);
            int mapW = *(const int*)(ta + OFF_MAPW), mapH = *(const int*)(ta + OFF_MAPH);
            int tx = wx >> 4, tyy = wy >> 4;
            if (ptr_ok(fmap) && tx >= 0 && tyy >= 0 && tx < mapW && tyy < mapH) {
                int th = *(const unsigned char*)(fmap + ((size_t)tyy * mapW + tx) * FT_STRIDE + 0x04);
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
        const char* pu = units[a].u;
        if (!ptr_ok(pu)) continue;
        const char* c = *(const char* const*)(pu + U_CARGO);
        for (int guard = 0; ptr_ok(c) && guard < 64; guard++,
             c = *(const char* const*)(c + U_CARGONEXT)) {
            if (*(const unsigned*)(c + U_STATE) & ST_NOCARGO) continue;
            for (int b = 0; b < nu; b++)
                if (units[b].u == c) {
                    units[b].rel = units[a].rel;
                    units[b].air = units[a].air;
                    break;
                }
        }
    }
    }

    /* ---- gather 3D wrecks from the sweep-rect tiles (G12c close-out) ----
       anchor tile: flags bit0 + live def with FeatureMask bit0 CLEAR ->
       wreck record holds the husk's Object3do + world pos; drawn at FEATURE
       depth (3+rel*4). The engine's own scratch-fake-unit draw is suppressed
       by the owndraw classifier while this pass is armed. */
    if (s_armed && s_wrecks) {
        const char* fmap = *(const char* const*)(ta + OFF_FMAP);
        const char* defs = *(const char* const*)(ta + OFF_FDEFS);
        const char* recs = *(const char* const*)(ta + OFF_WRECKS);
        int mapW = *(const int*)(ta + OFF_MAPW), mapH = *(const int*)(ta + OFF_MAPH);
        if (ptr_ok(fmap) && ptr_ok(defs) && ptr_ok(recs) &&
            mapW > 0 && mapH > 0 && mapW <= 4096 && mapH <= 4096) {
            /* both axes off the ZOOM's rect, like the row range and the cull
               below — leaving the columns on the engine's viewport stopped
               wrecks at the unzoomed left and right edges while terrain,
               features and units carried on past them */
            int tx0 = ((eyeX + (evpL - vpL)) >> 4) - 8;
            int tx1 = tx0 + (evw >> 4) + 16;
            int ty0 = r0, ty1 = r0 + rows;
            if (tx0 < 0) tx0 = 0;
            if (ty0 < 0) ty0 = 0;
            if (tx1 > mapW) tx1 = mapW;
            if (ty1 > mapH) ty1 = mapH;
            for (int ty = ty0; ty < ty1 && nu < MAXU; ty++)
            for (int tx = tx0; tx < tx1 && nu < MAXU; tx++) {
                const char* t = fmap + ((size_t)ty * mapW + tx) * FT_STRIDE;
                if (!(*(const unsigned char*)(t + FT_FLAGS) & 1)) continue;
                unsigned defIdx = *(const unsigned short*)(t + FT_DEFIDX);
                if (defIdx >= 0xFFFB) continue;               /* not an anchor */
                const char* def = defs + (size_t)defIdx * FD_STRIDE;
                if (*(const unsigned char*)(def + FD_MASK) & 1) continue; /* GAF wreck */
                unsigned widx = *(const unsigned short*)(t + FT_WIDX);
                const char* rec = recs + (size_t)widx * WR_STRIDE;
                if (IsBadReadPtr(rec, WR_STRIDE)) continue;
                const char* o3 = *(const char* const*)(rec + WR_OBJ3DO);
                if (!ptr_ok(o3)) continue;
                /* wreck record positions are 16.16 fixed-point (the engine
                   copies them straight into the scratch unit's +0x6A/6E/72
                   16.16 pos fields, then projects from the high words) — shift
                   to whole world units, exactly what the projection below and
                   the fog/rel maths expect. Reading them raw put every wreck
                   ~1700<<16 px off-screen, so all wrecks were culled (nu=0). */
                int rx = *(const int*)(rec + WR_XPOS) >> 16;
                int rz = *(const int*)(rec + WR_ZPOS) >> 16;
                int ry = *(const int*)(rec + WR_YPOS) >> 16;
                float ax = (float)(rx - eyeX + vpL);
                float ay = (float)(ry - rz / 2 - eyeY + vpT);
                if (ax < evpL - 256 || ax > evpL + evw + 256 ||
                    ay < evpT - 256 || ay > evpT + evh + 256) continue;
                /* wreckage is remembered furniture: hidden only where the
                   map is unexplored, visible (darkened) in grey */
                if ((fogMode & 1) &&
                    (tagpu_fog_at(s_fogGrid, s_fogCols, s_fogRows,
                                  s_fogOrgX, s_fogOrgY, rx, ry - rz / 2) & 1))
                    continue;
                NU* n2 = &units[nu++];
                n2->o3 = o3; n2->u = NULL; n2->ax = ax; n2->ay = ay; n2->gy = ay;
                n2->rec = rec; n2->dead = 0;
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

    /* ---- effects gather (projectiles, explosions, debris, particles) ---- */
    TAGPU_FXVIEW fv;
    int nfx = 0, nfeat = 0, nterr = 0, nmark = 0;
    if (fxOn || sfxOn || featOn || terrOn || markOn) {
        fv.ta = ta; fv.eyeX = eyeX; fv.eyeY = eyeY;
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
        fv.fogOrgX = s_fogOrgX; fv.fogOrgY = s_fogOrgY;
        fv.fogTex = s_fogTex; fv.fogLut = s_fogLutTex;
        fv.r0 = r0; fv.rows = rows;
        fv.frame_counter = f->frame_counter;
        /* terrain first (the frame's far plane), then features: they own the
           depth the units are tested against */
        if (terrOn) nterr = tagpu_terr_gather(&fv);
        if (featOn) nfeat = tagpu_feat_gather(&fv);
        if (fxOn || sfxOn) nfx = tagpu_fx_gather(&fv);
        if (markOn) nmark = tagpu_mark_gather(&fv);
        /* Classic++: one slice of the restorer, after the gathers (so the
           frames they missed this frame are queued) and before the renders
           (so what it paints is sampled this frame) */
        tagpu_rglsl_step();
    }
    /* NEVER return early while we own the terrain: the engine's frame is a
       flat key fill inside the viewport, and only the composite below turns it
       back into a picture. tagpu_terr_gather hands the draw back on any bail,
       so nterr == 0 usually means the engine is painting terrain again — but a
       hard bail (no atlas, bad map pointer) can leave a key-filled frame with
       nothing of ours to cover it, and that frame still has to be composited. */
    int terrOwned = tagpu_terrown_filled();
    if (nu == 0 && nfx == 0 && nfeat == 0 && nterr == 0 && !markOn && !terrOwned) return;

    /* uFog bit1 = hide in grey rather than darken. Units the watched player
       cannot see are not drawn at all; wreckage is furniture and stays, and
       our own units make the LOS they stand in. */
#define FOGW(i) ((fogMode & 1) | \
                 ((units[i].feat || units[i].owner == watched) ? 0 : 2))

    /* ---- build geometry (body); shadow reuses it with an offset ---- */
    static int firstv[MAXU + 1];
    static float encb[MAXU];
    static float topv[MAXU];            /* the posed model top (the length rule's h) */
    static int hidx[MAXU];              /* the unit's hunits index, or -1 */
    int i;
    static TAGPU_HUNIT hunits[MAXU];
    int nhi = 0;
    /* G16 step 5: the units the POSED program drew, and their poses. `pdix[i]`
       is the unit's entry or -1; a posed unit contributes no vertices to the
       shared stream, so its firstv range is empty and every CPU draw over it
       is a no-op — the same way a replacement mesh's is. */
    static TAGPU_PDUNIT pdu[MAXU];
    static int pdix[MAXU];
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
    s_hposeN = 0;
    for (i = 0; i < nu; i++) {
        firstv[i] = nv;
        /* BEFORE any of the skips below, not in the branch that fills it: the
           array is static, so a unit that takes an early `continue` — dead, or
           a replacement mesh — would otherwise be read against another unit's
           index from an earlier frame. */
        pdix[i] = -1;
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
        encb[i] = encBase;                    /* before the dead check: encb is
                                                 static, and every later loop
                                                 indexes it by i */
        {
            const char* now = units[i].o3;
            if (units[i].feat) { if (units[i].rec) now = *(const char* const*)(units[i].rec + WR_OBJ3DO); }
            else if (units[i].u) now = *(const char* const*)(units[i].u + U_OBJ3DO);
            if (now != units[i].o3) {
                units[i].dead = 1; s_reread++;
                hidx[i] = -1; topv[i] = 0.0f;   /* static: the depth pass reads them too */
                continue;
            }
        }
        if (units[i].hires) {
            /* no vertices here: this unit is the other pass's, and leaving
               firstv[i] == firstv[i+1] makes its draws below empty */
            TAGPU_HUNIT* h = &hunits[nhi++];
            hidx[i] = nhi - 1; topv[i] = 0.0f;
            h->mesh = units[i].hires;
            /* the COB pose, into a frame arena; past the arena a unit still
               draws, at rest, rather than dropping out of the scene */
            h->pose = NULL; h->npose = 0;
            {
                int np = tagpu_hires_npiece(units[i].hires);
                if (np > TAGPU_HMAXPIECE) np = TAGPU_HMAXPIECE;
                if (np > 0 && s_hposeN + np * 12 <= HPOSE_MAX) {
                    float* dst = s_hpose + s_hposeN;
                    if (hires_pose(units[i].o3, units[i].hires, dst, np)) {
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
            h->yaw = h->pose ? 0
                   : (unsigned short)*(const unsigned short*)(units[i].o3 + O3_BTURN + 2);
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
            hidx[i] = -1;
            topv[i] = 0.0f;
            if (!pdReady) continue;
            if (!tagpu_posebake_unit(units[i].o3, units[i].owner, &bg, &bm) ||
                bg->nparts <= 0 || bg->count[TAGPU_PB_BODY] <= 0) {
                s_poseNoBake++;
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
                    posed_pose(units[i].o3, bg, pdPose + pdPoseN,
                               pdShaded + pdShadedN, pdPvis + pdShadedN)) {
                    q->pose   = pdPose + pdPoseN;
                    q->shaded = pdShaded + pdShadedN;
                    q->pvis   = pdPvis + pdShadedN;     /* same stride and slot */
                    pdPoseN += np * 12;
                    pdShadedN += np;
                } else {
                    q->pose   = s_poseRestPose;
                    q->shaded = s_poseRestShaded;
                    q->pvis   = s_poseRestVis;
                    s_poseAtRest++;
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
                pdix[i] = npd++;
                /* the model top no longer falls out of the vertices. Only a
                   WRECK reads it: a unit with a record prefers model_aabb */
                topv[i] = tagpu_posedraw_top(q);
            }
        }
    }
    firstv[nu] = nv;
    /* effects models (missiles, shells, debris) through the same path */
    int fxFirst = nv;
    if (nfx) {
        int k, nm = tagpu_fx_nmodels();
        for (k = 0; k < nm; k++) nv = emit_fx_model(tagpu_fx_model(k), nv, fxKey);
    }
    int fxLast = nv;
    /* nhi belongs in this test: a replacement unit contributes no vertices to
       this pass (it is the other one's), so a frame holding nothing but those
       would bail here and draw them nowhere. SINCE G16 STEP 8 npd belongs in
       it for exactly the same reason, and more urgently: an ordinary unit
       contributes no vertices either now, so without this a frame of nothing
       but units — every unit in the game, on a map with our terrain off —
       would return here having drawn none of them. */
    if (nv == 0 && nhi == 0 && npd == 0 && nfx == 0 && nfeat == 0 && nterr == 0 &&
        !markOn && !terrOwned) return;

    /* ---- native selection rects (ui-markers: the ONLY marker interleaved
       with unit draws — the engine's is unreadable under our pixels, redraw
       it): flat model-XZ AABB rect at lowest model Y, rotated by body yaw,
       GUI colour 0xA, drawn as GL_LINES at just-under-the-unit depth ---- */
    int lineStart = nv, selDrawn = 0;
    if (nsel) {
        for (i = 0; i < nu && nv + 8 <= MAXNV; i++) {
            if (units[i].dead || !units[i].sel || !units[i].u) continue;
            const char* root = model_root(ta, units[i].u);
            const MAABB* a = root ? selbox_aabb(root) : NULL;
            if (!a) continue;
            /* the engine hands all THREE of the unit's angles to 0x4B6CC0
               (bank, heading, pitch at u+0x64), so the corners take the same
               triple an effects model does: Rz(bank) on (x,y), Rx(pitch) on
               (y,z), Ry(heading) on (x,z) — emit_fx_model's order, rot2's
               sense (x' = x c - z s). Yaw alone is right on the flat and
               several pixels out on a slope, where one tank was measured at
               17.4 deg of bank and -22.1 of pitch and the next along at -30.7
               of pitch; the TRANSPOSED yaw,
               which this loop used until 2026-09-08, is a rotation by
               -heading, so the rect turned against the unit it marks. */
            const unsigned short* rot =
                (const unsigned short*)(units[i].u + U_ROT);
            const float K = 6.2831853f / 65536.0f;
            float c0 = cosf((float)rot[0] * K), s0 = sinf((float)rot[0] * K);
            float c1 = cosf((float)rot[1] * K), s1 = sinf((float)rot[1] * K);
            float c2 = cosf((float)rot[2] * K), s2 = sinf((float)rot[2] * K);
            float y0 = a->mn[1];
            float cx[4] = { a->mn[0], a->mx[0], a->mx[0], a->mn[0] };
            float cz[4] = { a->mn[2], a->mn[2], a->mx[2], a->mx[2] };
            float px[4], py[4];
            int k;
            for (k = 0; k < 4; k++) {
                float x = cx[k], y = y0, z = cz[k];
                if (rot[0]) rot2(c0, s0, &x, &y);
                if (rot[2]) rot2(c2, s2, &y, &z);
                if (rot[1]) rot2(c1, s1, &x, &z);
                /* The engine's own projection for this rect (0x467A50), term
                   by term, because it truncates each one SEPARATELY and only
                   then halves the height:

                     sx = ((rot.x + pos.x) >> 16) + 0x80
                     sy = ((pos.z - rot.z) >> 16)
                        - (((rot.y + pos.y) >> 16) >> 1) + 0x20

                   `>>` is arithmetic, so both are floors, and `sar 1` floors
                   the ALREADY floored height — folding them into one float
                   expression lands a pixel out on some edges (measured: 46 of
                   ~110 box pixels differed from the engine's before this).
                   `rot.y` is the corner's own y, which bank and pitch move.
                   The anchor carries the eye and the altitude already:
                   ax = wx - eyeX + 128, ay = wz - alt/2 - eyeY + 32. */
                {
                    float alt = units[i].wy;
                    float zt  = (units[i].ay - (float)vpT + alt * 0.5f) - z;
                    float yt  = floorf(floorf(y + alt) * 0.5f);
                    px[k] = floorf(units[i].ax - (float)vpL + x) + (float)vpL + 0.5f;
                    py[k] = floorf(zt) - yt + (float)vpT + 0.5f;
                }
                /* At 1x the truncation above has already put the corner on a
                   device pixel, which is what keeps the line fully coloured
                   rather than smeared across two rows — the engine's own
                   corners are integers for the same reason. Away from 1x the
                   shader scales about the zoom centre and lands between
                   pixels, so snap there too: forward through the zoom, floor,
                   and back. (Snapping a marker to the pixel grid is what the
                   glyph atlas does, gpu-status 2.2.) */
                if (s_zoom > 0.0f && s_zoom != 1.0f) {
                    float zcx0 = (float)vpL + (float)vw * 0.5f;
                    float zcy0 = (float)vpT + (float)vh * 0.5f;
                    float sx = (px[k] - zcx0) * s_zoom + zcx0;
                    float sy = (py[k] - zcy0) * s_zoom + zcy0;
                    sx = floorf(sx) + 0.5f;
                    sy = floorf(sy) + 0.5f;
                    px[k] = (sx - zcx0) / s_zoom + zcx0;
                    py[k] = (sy - zcy0) / s_zoom + zcy0;
                }
            }
            float enc = encb[i] - 0.5f;
            selDrawn++;
            for (k = 0; k < 4; k++) {
                int k2 = (k + 1) & 3, t2;
                for (t2 = 0; t2 < 2; t2++) {
                    float* o = s_verts + nv * NVST;
                    o[0] = t2 ? px[k2] : px[k];
                    o[1] = t2 ? py[k2] : py[k];
                    o[2] = enc;
                    o[3] = -1.0f; o[4] = -1.0f;                 /* flat path  */
                    o[5] = (float)*(unsigned char*)(ta + OFF_GUICOL + SELBOX_COLIDX)
                           / 255.0f;
                    o[6] = -1.0f;
                    o[7] = (float)tagpu_r3d_shade_neutral() / 31.0f;
                    o[8] = units[i].wx0; o[9] = units[i].wz0;
                    o[10] = 1e9f;                               /* never clipped */
                    o[11] = 0.0f; o[12] = 1.0f; o[13] = 0.0f;   /* level: lit 1.0 */
                    nv++;
                }
            }
        }
    }
    /* Whether we owed a box we could not draw. `markown` suppresses the
       engine's selection rect per unit on `tagpu_native_owns_unit` alone, but
       this loop can still come up short — the gather cap, the vertex budget, an
       unresolvable model AABB — and a suppressed box we then failed to draw
       leaves a selected unit unmarked. So say so, and let markown hand the
       WHOLE set back for a frame: at 1x the engine's boxes land on the same
       pixels and nothing shows, at any other zoom a one-frame ghost is a much
       smaller lie than a missing marker. Self-correcting either way. */
    s_selComplete = (selDrawn == nsel && nu < MAXU);
    int lineEnd = nv;

    /* nanoframe wireframes: a second line range per unit, empty for everyone
       not under construction. Before the shadows in the vertex budget — at the
       top of a build the scaffold is erased down to this skeleton, so losing it
       loses the unit, while losing a shadow loses a shadow. */
    int nwire = 0, npdWire = 0;
    for (i = 0; i < nu; i++) {
        if (units[i].dead || !units[i].nanoOn || pdix[i] < 0) continue;
        {
            const TAGPU_PBGEOM* pg = (const TAGPU_PBGEOM*)pdu[pdix[i]].geom;
            if (pg && pg->count[TAGPU_PB_WIRE] > 0) { npdWire++; nwire++; }
        }
    }

    /* structure shadows: the slant projection is a second vertex range per
       unit (the body range cannot be re-offset into it); empty for everyone
       else, so the shadow pass below indexes it uniformly. Emitted LAST so
       that under the vertex budget effects and selection rects win over a
       building's shadow, the least visible thing to lose. */
    /* Classic++ at `shadows=1` draws neither Classic sub-pass (renderers.md
       2.12): no slant range here, and no silhouette below, except an aircraft's
       under airshadow=drop -- the one thing that lane borrows from Classic.
       `shadows=2` (HARD, G18b) draws the pair under the switch instead, and the
       depth pass then refuses (tagpu_shadow_begin wants SHADOWS_SOFT outright);
       `shadows=0` draws neither, the aircraft included.

       The MASTER ARM, not assets/light (G18a): which half of Classic++ is on
       says nothing about who owns the shadows -- that dimension has `shadows=`.
       Classic itself is untouched by the key: with the switch off the engine's
       own option bits rule, exactly as before.

       G16 step 8 deleted the CPU emitters, so this loop builds no vertices any
       more: it counts the units whose bake carries a slant range, and the pass
       below draws that range out of the posed program. */
    const TAGPU_LIGHT* cppL = tagpu_classicpp_light();
    int cpp  = tagpu_classicpp_on();
    int hard = cpp && cppL->shadows == TAGPU_SHADOWS_HARD;
    int airDrop = cppL->airshadow == TAGPU_AIRSHADOW_DROP &&
                  cppL->shadows != TAGPU_SHADOWS_OFF;
    int nslant = 0, npdSlant = 0;
    for (i = 0; i < nu; i++) {
        if (units[i].dead || (cpp && !hard) || !units[i].slant || !units[i].shadow ||
            units[i].hires || pdix[i] < 0) continue;
        npdSlant++; nslant++;
    }

    /* the replacement pass gets the same frame it would have drawn into here:
       same FBO, same projection and depth scale, same scaffold, same fog grid,
       and the palette + SHD textures already bound above on units 1..5 */
    TAGPU_HVIEW hv;
    if (nhi) {
        memset(&hv, 0, sizeof hv);
        hv.game[0] = (float)gw; hv.game[1] = (float)gh;
        hv.zoom = s_zoom;
        hv.zoomC[0] = (float)vpL + (float)vw * 0.5f;
        hv.zoomC[1] = (float)vpT + (float)vh * 0.5f;
        hv.depthScale = depthScale;
        hv.ss = (float)ss;
        hv.scafOn = scafOn;
        hv.scafTex = scafOn ? tagpu_scaffold_texref() : 0;
        hv.scafP[0] = (float)vpL; hv.scafP[1] = (float)vpT;
        hv.scafP[2] = (float)vw;  hv.scafP[3] = (float)vh;
        hv.fogOrg[0] = (float)s_fogOrgX; hv.fogOrg[1] = (float)s_fogOrgY;
        hv.fogDim[0] = (float)s_fogCols; hv.fogDim[1] = (float)s_fogRows;
        hv.palTex = s_palTex; hv.lutTex = tagpu_r3d_lut_texref();
        hv.fogTex = s_fogTex; hv.fogLutTex = s_fogLutTex;
        hv.shNeutral = tagpu_r3d_shade_neutral();
        hv.shDir = tagpu_r3d_shade_dir();
    }

    /* ---- Classic++ shadows: the depth pass, before the frame FBO (G14i) ----
       The stream is complete, so it is uploaded here -- every pass after this
       reads the same buffer -- and, when the map is on, drawn once more along
       the shadow sun into tagpu_shadow.c's depth texture: per unit, so the
       caster's own length rule can scale it (renderers.md 2.2, 2.12). The
       casters are the lab's: every unit and wreck, cloaked or not (a cloaked
       enemy never reached this buffer); a nanoframe casts nothing (2.11); an
       aircraft under airshadow=drop keeps the Classic silhouette instead;
       effects models are not casters. Then the replacement meshes, then the
       heightfield rows under the window. The caster numbers are kept for the
       body draw, whose fragments look their own shadow up at the same point. */
    static float castv[MAXU][3];
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nv * NVST * 4, s_verts);
    for (i = 0; i < nu; i++) { castv[i][0] = 0.0f; castv[i][1] = 0.0f; castv[i][2] = 1.0f; }
    if (cpp && tagpu_shadow_begin(&fv, (gfx & 4) != 0)) {
        for (i = 0; i < nu; i++) {
            float agl, throw_, sv, top = 0.0f, amn = 0.0f;
            int skip;
            /* its object moved since the gather (the body loop above):
               nothing in its record is its own any more, and its hires
               index would be last frame's slot -- another unit's entry */
            if (units[i].dead) continue;
            /* the model height at rest, the lab's meshTop: the whole-tree
               AABB for a unit (constant per type, so an animating piece does
               not make its shadow breathe); a wreck has no unit record and
               takes its posed top, which never moves */
            if (units[i].u) {
                const char* root = model_root(ta, units[i].u);
                if (root) {
                    const MAABB* a = model_aabb(root);
                    if (a) { top = a->mx[1]; amn = a->mn[1]; }
                }
            }
            if (top <= 0.0f) top = topv[i];
            /* a ground unit SITS ON THE HEIGHT BYTE, as the lab's does: the
               engine's own y is the interpolated ground under it, a few units
               off the byte the receiver is drawn from, and a caster floating
               that much above its receiver throws a shadow detached by
               agl * cot el -- only an airborne unit has an altitude here */
            agl = units[i].air ? units[i].wy - units[i].gnd : 0.0f;
            if (agl < 0.0f) agl = 0.0f;
            tagpu_shadow_caster(top, agl, &throw_, &sv);
            castv[i][0] = units[i].wy; castv[i][1] = units[i].gnd + throw_; castv[i][2] = sv;
            if (s_castLogged < 16 && units[i].u && units[i].wx0 != s_castLogX) {
                s_castLogX = units[i].wx0;
                char b[160];
                _snprintf(b, sizeof b, "shadow: caster model=%u top=%.1f (aabb y %.1f..%.1f) wy=%.1f gnd=%.1f agl=%.1f throw=%.1f sv=%.3f air=%d",
                          (unsigned)*(const unsigned short*)(units[i].u + U_MODELID),
                          top, amn, top, units[i].wy, units[i].gnd, agl, throw_, sv, units[i].air);
                nlog(b);
                s_castLogged++;
            }
            skip = units[i].nanoOn || (units[i].air && airDrop);
            /* a posed unit casts from the posed program's depth twin, after
               this loop — the caster numbers are recorded here because this is
               where they are computed */
            if (pdix[i] >= 0) {
                pdu[pdix[i]].cast[0] = castv[i][0];
                pdu[pdix[i]].cast[1] = castv[i][1];
                pdu[pdix[i]].cast[2] = castv[i][2];
                pdu[pdix[i]].castSkip = skip;
                continue;
            }
            if (units[i].hires) {
                if (hidx[i] >= 0) {
                    TAGPU_HUNIT* h = &hunits[hidx[i]];
                    h->cast[0] = castv[i][0]; h->cast[1] = castv[i][1]; h->cast[2] = castv[i][2];
                    h->castSkip = skip;
                }
                continue;
            }
            if (skip || firstv[i + 1] == firstv[i]) continue;
            tagpu_shadow_unit(castv[i][0], castv[i][1], castv[i][2]);
            x_glDrawArrays(GL_TRIANGLES, firstv[i], firstv[i + 1] - firstv[i]);
        }
        if (npd) {
            int k;
            tagpu_posedraw_depth_begin(tagpu_shadow_mat());
            for (k = 0; k < npd; k++)
                if (!pdu[k].castSkip) tagpu_posedraw_depth_unit(&pdu[k]);
        }
        if (nhi) tagpu_hires_depth(&hv, hunits, nhi, tagpu_shadow_mat());
        tagpu_shadow_hills();
        tagpu_shadow_end();
    }

    /* ---- render into the (optionally 2x supersampled) game-res FBO ---- */
    fbo_size(gw, gh, ss);
    glBindFramebuffer(GL_FRAMEBUFFER, ss > 1 ? s_fbo2 : s_fbo);
    glViewport(0, 0, gw * ss, gh * ss);
    { const GLfloat cl[4] = { 0, 0, 0, 0 }; x_glClearBufferfv(GL_COLOR, 0, cl); }
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    x_glDepthFunc(GL_LESS);
    /* engine clips unit blits to the viewport rect — so do we (in this FBO
       window y == game frame py, so the rect maps directly) */
    if (x_glScissor) {
        glEnable(GL_SCISSOR_TEST);
        x_glScissor(vpL * ss, vpT * ss, vw * ss, vh * ss);
    }

    /* terrain is the frame's implicit far plane: it draws under everything,
       writes depth at a key below every other band, and (since it is now the
       bottom layer) paints the fog's solid black itself. Own program. */
    if (nterr) tagpu_terr_render(&fv, s_palTex);

    /* features (trees, rocks, splats, GAF wrecks) draw next and write real
       depth, so every unit body below is occluded by them through the depth
       buffer — this is what the G12a scaffold was standing in for. Its own
       program; the unit program and VAO are (re)bound right after. */
    if (nfeat) {
        glEnable(GL_BLEND);
        tagpu_feat_render(&fv, s_palTex);
    }

    glUseProgram(s_prog);
    x_glUniform2f(s_uGame, (float)gw, (float)gh);
    x_glUniform1f(s_uSS, (float)ss);
    {
        float zcx = (float)vpL + (float)vw * 0.5f;
        float zcy = (float)vpT + (float)vh * 0.5f;
        x_glUniform1f(s_uZoom, s_zoom);
        x_glUniform2f(s_uZoomC, zcx, zcy);
        x_glUniform1f(s_uZoomF, s_zoom);
        x_glUniform2f(s_uZoomCF, zcx, zcy);
    }
    x_glUniform2f(s_uFogOrg, (float)s_fogOrgX, (float)s_fogOrgY);
    x_glUniform2f(s_uFogDim, (float)s_fogCols, (float)s_fogRows);
    x_glUniform1f(s_uDepthScale, depthScale);
    glUniform1i(s_uScafOn, scafOn ? 1 : 0);
    x_glUniform4f(s_uScafP, (float)vpL, (float)vpT, (float)vw, (float)vh);
    /* Classic++ lighting: the units' sun (tagpu_classicpp.c), once a frame */
    {
        const TAGPU_LIGHT* L = tagpu_classicpp_light();
        glUniform1i(s_uLit, tagpu_classicpp_on() ? 1 : 0);
        glUniform1i(s_uLambert, tagpu_classicpp_lit() ? 1 : 0);
        x_glUniform3f(s_uSun, L->unitSun[0], L->unitSun[1], L->unitSun[2]);
        x_glUniform1f(s_uAmb, L->amb);
        x_glUniform1f(s_uNorm, 1.0f / L->unitLevel);
    }
    tagpu_shadow_apply(&s_shU);            /* this frame's map, or uShadowOn 0 */
    x_glUniform3f(s_uCast, 0.0f, 0.0f, 1.0f);   /* lines, wires, effects: no caster */
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tagpu_r3d_atlas_texref());
    x_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tagpu_r3d_lut_texref());
    x_glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_palTex);
    x_glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, scafOn ? tagpu_scaffold_texref() : 0);
    x_glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, s_fogTex);
    x_glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, s_fogLutTex);
    x_glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, tagpu_r3d_atlas_rgbref());
    x_glActiveTexture(GL_TEXTURE0);
    /* Classic++: the twin exists and the switch is on; a job may still be
       running, and the shader's alpha test is what says a texel is ready */
    glUniform1i(s_uRestored, (tagpu_r3d_atlas_rgbref() && tagpu_classicpp_assets()) ? 1 : 0);
    /* the stream was uploaded before the depth pass; the terrain and feature
       renders bound their own VAOs, so ours is put back */
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

/* the replacement pass runs its own program; put ours back for the draws that
   follow it, and leave texture unit 0 selected the way the rest expects */
#define HIRES_RESTORE() do { \
        glUseProgram(s_prog); \
        glBindVertexArray(s_vao); \
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo); \
        x_glActiveTexture(GL_TEXTURE0); \
    } while (0)

    glEnable(GL_BLEND);
    x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);      /* premultiplied */

    /* Selection rects first — the engine draws them under the unit sprite.

       ...but when this pass is SUPERSAMPLED they are not drawn here at all.
       The engine's rect is four Bresenham lines (0x4BE950): one fully coloured
       pixel per major-axis step. A GL line in an ss-times buffer is one
       SUPERSAMPLE wide — the driver clamps aliased line width to 1, measured:
       `glLineWidth(ss*3)` draws pixel-identically to `glLineWidth(ss)` — so it
       resolves to a half-lit smear, about half the engine's colour. Drawn
       instead into the 1x FBO right after the box-downsample, where a GL line
       IS the engine's rule, one whole pixel per step. It still needs the
       world's depth to sit under its own unit, so the ss depth buffer is
       blitted down with it. `selAt1x` is 0 without the blit entry point or
       without supersampling, and then this draws it here as before. */
    /* the 1x selection-rect path draws OVER the resolved frame, so it exists
       only when there is one: under `devres` the rects are drawn in the main
       pass at ss, which is what the ss == 1 path has always done */
    int selAt1x = (!devres && ss > 1 && x_glBlitFramebuffer != NULL);
    glUniform1i(s_uNanoOn, 0);
    if (lineEnd > lineStart && !selAt1x) {
        glUniform1i(s_uFog, fogMode & 1);
        glUniform1i(s_uShadow, 0);
        x_glUniform1f(s_uAlpha, 1.0f);
        x_glUniform2f(s_uOffset, 0.0f, 0.0f);
        x_glUniform1f(s_uWaterT, -1e9f);
        x_glUniform1f(s_uDigT, -1e9f);
        glUniform1i(s_uWaterMode, 0);
        if (x_glLineWidth) x_glLineWidth((GLfloat)ss);
        x_glDrawArrays(GL_LINES, lineStart, lineEnd - lineStart);
    }

    /* shadow first (engine order), only when options allow; per unit so an
       aircraft's shadow lands at GROUND height (gy - ay shifts body->ground).
       Option bits as the engine tests them: the silhouette needs Shadow AND
       TShadow, a structure's slant shadow only Shadow. Both sit 5 px right
       of the body (the blit's sx+0x85 against the body's sx+0x80). */
    if (gfx & 4) {
        glUniform1i(s_uShadow, 1);
        x_glUniform1f(s_uAlpha, 0.5f);
        x_glDepthMask(GL_FALSE);
        /* ONE 50% BLEND PER SILHOUETTE PIXEL, not one per surface the ray
           crosses. The engine blackens a copy of the unit's COMPOSITE and
           blits that ONCE (shadows-cloak.md 3), so a pixel the model covers
           twice is still darkened once. We re-use the body's 3-D geometry, so
           without a mask the blend compounds -- and from above an aircraft is
           a two-sided shell over its whole area, which is where this showed
           up. Measured 2026-09-04, engine vs ours on the same fixture and
           camera: the engine's shadow is a single sharp mode at 0.44-0.52 of
           the bare ground, ours was BIMODAL at 0.25 (two surfaces) and 0.50
           (one), with a 0.125 tail for three; four airframes all read 0.252-
           0.255 against the engine's 0.487. Ground units had it too, milder.
           Two draws per unit fix it: mark the silhouette into the stencil with
           colour writes off, then blend where the mark is with the op that
           ZEROES it, so the next fragment on that pixel fails EQUAL 1. Both
           draws see the same depth buffer (depth writes are off), so they
           cover exactly the same fragments and no mark is left behind. The
           mark is per unit and cleared by that unit's own second draw, so two
           DIFFERENT units' shadows still stack, exactly as the engine's two
           separate blits do. */
        glEnable(GL_STENCIL_TEST);
        /* G16 step 8: no unit's vertices are in the shared stream any more,
           so both shadow sub-passes are the posed program's. The silhouette
           first (the body range, for everything that is not a structure),
           then the structure slant out of the bake's own SLANT range. The
           shared-stream loop that used to precede these drew nothing but
           empty ranges once step 6 posed the slant, and is gone with the
           emitters that filled it. */
        if (npd) {
            tagpu_posedraw_shadow_begin();
            for (i = 0; i < nu; i++) {
                if (pdix[i] < 0 || units[i].slant) continue;
                if (cpp && !hard && !(units[i].air && airDrop)) continue;
                if (!units[i].shadow) continue;
                if (!(gfx & 8)) continue;
                tagpu_posedraw_shadow_set(&pdu[pdix[i]], 5.0f,
                                          (float)(units[i].gy - units[i].ay),
                                          units[i].waterT, units[i].digT);
                x_glStencilFunc(GL_ALWAYS, 1, 0xFF);
                x_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                x_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                tagpu_posedraw_redraw(&pdu[pdix[i]]);
                x_glStencilFunc(GL_EQUAL, 1, 0xFF);
                x_glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                tagpu_posedraw_redraw(&pdu[pdix[i]]);
            }
            HIRES_RESTORE();
        }
        x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        /* G16 step 6: a posed STRUCTURE's slant, out of the same bake's slant
           range — the same stencil dance, the same 5 px offset and the same
           ground shift as the loop above, which drew an empty `sfirst` range
           for these units. The waterline and digger thresholds are the pass's
           own (-1e9, the structure branch's rule) and are not passed in. */
        if (npdSlant) {
            tagpu_posedraw_slant_begin();
            for (i = 0; i < nu; i++) {
                if (pdix[i] < 0 || !units[i].slant) continue;
                if (cpp && !hard && !(units[i].air && airDrop)) continue;
                if (!units[i].shadow) continue;
                tagpu_posedraw_slant_set(&pdu[pdix[i]], 5.0f,
                                         (float)(units[i].gy - units[i].ay));
                x_glStencilFunc(GL_ALWAYS, 1, 0xFF);
                x_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                x_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                tagpu_posedraw_slant_redraw(&pdu[pdix[i]]);
                x_glStencilFunc(GL_EQUAL, 1, 0xFF);
                x_glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                tagpu_posedraw_slant_redraw(&pdu[pdix[i]]);
            }
            HIRES_RESTORE();
        }
        x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        /* the stencil stays ON for the hires shadow: a replacement mesh needs
           the same one-blend-per-pixel mask and does it per unit itself */
        if (nhi) {
            /* under Classic++ at `shadows=1` only an aircraft under `drop` keeps
               its silhouette; at `shadows=2` every replacement mesh does */
            static TAGPU_HUNIT hsil[MAXU];
            int ns = 0, k;
            for (k = 0; k < nhi; k++)
                if (!cpp || hard || (hunits[k].air && airDrop)) hsil[ns++] = hunits[k];
            if (ns) tagpu_hires_draw(&hv, hsil, ns, 1, f->frame_counter);
            HIRES_RESTORE();
        }
        x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        x_glDisable(GL_STENCIL_TEST);
        x_glDepthMask(GL_TRUE);
    }
    /* bodies */
    glUniform1i(s_uShadow, 0);
    x_glUniform2f(s_uOffset, 0.0f, 0.0f);
    for (i = 0; i < nu; i++) {
        glUniform1i(s_uFog, FOGW(i));
        x_glUniform1f(s_uAlpha, units[i].cloaked ? 0.5f : 1.0f);
        x_glUniform1f(s_uWaterT, units[i].waterT);
        x_glUniform1f(s_uDigT, units[i].digT);
        glUniform1i(s_uWaterMode, units[i].waterMode);
        glUniform1i(s_uNanoOn, units[i].nanoOn);
        x_glUniform3f(s_uCast, castv[i][0], castv[i][1], castv[i][2]);
        if (units[i].nanoOn) {
            x_glUniform1f(s_uNanoT, units[i].nanoT);
            x_glUniform3f(s_uNanoC, units[i].nanoC[0], units[i].nanoC[1],
                                    units[i].nanoC[2]);
        }
        x_glDrawArrays(GL_TRIANGLES, firstv[i], firstv[i+1] - firstv[i]);
    }
    /* G16 step 5: the posed bodies. They are drawn as a block after the CPU
       ones rather than interleaved by index — with the lever on essentially
       every unit takes this path, so there is nothing to interleave with, and
       the depth keys sort the two against each other anyway. What the order
       does reach is the blend of a CLOAKED unit (alpha 0.5) against another
       unit at the same key, which is why this is a measurement lever and not
       a play setting until Gate B has run. */
    s_poseQueued = (unsigned)npd;
    if (npd) {
        TAGPU_PDVIEW pv;
        const TAGPU_LIGHT* L = tagpu_classicpp_light();
        int k;
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
        tagpu_posedraw_begin(&pv);
        for (k = 0; k < npd; k++) tagpu_posedraw_unit(&pdu[k]);
        tagpu_posedraw_end();
        HIRES_RESTORE();
    }
    glUniform1i(s_uNanoOn, 0);      /* the wireframe carries its own colour */
    x_glUniform3f(s_uCast, 0.0f, 0.0f, 1.0f);
    /* G16 step 6, and since step 8 the only wire path: the outlines come out
       of the bake's WIRE range. Same line width, same one-notch-nearer depth
       (the shader's own +0.15), and the animated blue as a uniform rather
       than a per-vertex colour. */
    if (npdWire) {
        if (x_glLineWidth) x_glLineWidth((GLfloat)ss);
        tagpu_posedraw_wire_begin();
        for (i = 0; i < nu; i++) {
            if (pdix[i] < 0 || units[i].dead || !units[i].nanoOn) continue;
            tagpu_posedraw_wire_unit(&pdu[pdix[i]], units[i].nanoWire);
        }
        tagpu_posedraw_end();
        HIRES_RESTORE();
    }
    if (nhi) {
        tagpu_hires_draw(&hv, hunits, nhi, 0, f->frame_counter);
        HIRES_RESTORE();
    }
#undef HIRES_RESTORE
    /* effects: models in the unit pipeline, then lines/sprites (own program;
       depth test still on so aircraft cover them, depth writes off) */
    if (fxLast > fxFirst) {
        glUniform1i(s_uFog, (fogMode & 1) | 2);
        x_glUniform1f(s_uAlpha, 1.0f);
        x_glUniform1f(s_uWaterT, -1e9f);
        x_glUniform1f(s_uDigT, -1e9f);
        glUniform1i(s_uWaterMode, 0);
        x_glDrawArrays(GL_TRIANGLES, fxFirst, fxLast - fxFirst);
    }
    if (nfx) tagpu_fx_render(&fv, s_palTex, scafOn ? tagpu_scaffold_texref() : 0);
#undef FOGW
    x_glDisable(GL_BLEND);
    x_glDisable(GL_DEPTH_TEST);
    /* UI markers last and over everything — health bars, order lines, the
       build cursor: in the engine they are painted after every world sprite,
       and here they need no depth test and no blend (every fragment they keep
       is opaque, and the key texels discard). The scissor stays on: a zoomed-in
       marker layer reaches past the viewport and must be cut at its edge. */
    if (markOn) tagpu_mark_render(&fv, s_palTex);
    if (x_glScissor) x_glDisable(GL_SCISSOR_TEST);

    /* ---- box-downsample ss -> 1x (quad covers every pixel; no clear) ----
       Skipped under `devres`: the composite reads the supersampled buffer. */
    if (ss > 1 && !devres) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, gw, gh);
        glUseProgram(s_dprog);
        glBindVertexArray(s_cvao);
        x_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_colTex2);
        x_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        /* ---- the selection rects, at 1x, over the resolved frame ---- */
        if (selAt1x && lineEnd > lineStart) {
            /* the world's depth, downsampled by point sampling (NEAREST is the
               only filter a depth blit may use), so the rect is still occluded
               by its own unit and by anything nearer — the engine draws it
               inside the row sweep, not over the frame */
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo2);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
            x_glBlitFramebuffer(0, 0, gw * ss, gh * ss, 0, 0, gw, gh,
                                GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
            glViewport(0, 0, gw, gh);
            glUseProgram(s_prog);
            glBindVertexArray(s_vao);
            glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
            /* every texture unit this program reads, put back: the feature,
               effects and marker passes in between bind their own — the marker
               pass alone takes 1, 2 and 3 (`tagpu_mark.c`, palette/fog/fogLut)
               — and with the shade LUT (unit 1) and the palette (unit 2)
               pointing at someone else's texture the rect draws BLACK
               (measured). Unit 3 is the scaffold, which the flat path reaches
               through TAGPU_GLSL_SCAF_TEST whenever `tagpu_scaffold.on` is
               armed: left as the marker pass had it, that test samples the fog
               LUT and discards rect fragments at random. */
            x_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tagpu_r3d_atlas_texref());
            x_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, tagpu_r3d_lut_texref());
            x_glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, s_palTex);
            x_glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, scafOn ? tagpu_scaffold_texref() : 0);
            x_glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, s_fogTex);
            x_glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, s_fogLutTex);
            x_glActiveTexture(GL_TEXTURE8);
            glBindTexture(GL_TEXTURE_2D, tagpu_r3d_atlas_rgbref());
            x_glActiveTexture(GL_TEXTURE0);
            glEnable(GL_DEPTH_TEST);
            x_glDepthFunc(GL_LESS);
            if (x_glDepthMask) x_glDepthMask(GL_FALSE);
            if (x_glScissor) { glEnable(GL_SCISSOR_TEST); x_glScissor(vpL, vpT, vw, vh); }
            glEnable(GL_BLEND);
            x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glUniform1i(s_uFog, fogMode & 1);
            glUniform1i(s_uShadow, 0);
            glUniform1i(s_uNanoOn, 0);
            glUniform1i(s_uWaterMode, 0);
            x_glUniform1f(s_uAlpha, 1.0f);
            x_glUniform2f(s_uOffset, 0.0f, 0.0f);
            x_glUniform1f(s_uWaterT, -1e9f);
            x_glUniform1f(s_uDigT, -1e9f);
            /* ...and the same test scales gl_FragCoord by uSS. These
               fragments are already 1x, so it is 1 here, not ss. The next
               frame sets it back with the rest of the pass's uniforms. */
            x_glUniform1f(s_uSS, 1.0f);
            if (x_glLineWidth) x_glLineWidth(1.0f);
            x_glDrawArrays(GL_LINES, lineStart, lineEnd - lineStart);
            x_glDisable(GL_DEPTH_TEST);
            if (x_glDepthMask) x_glDepthMask(GL_TRUE);
            if (x_glScissor) x_glDisable(GL_SCISSOR_TEST);
            x_glDisable(GL_BLEND);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());

    /* ---- composite over the frame (restore the letterbox viewport) ---- */
    int keyOn = -1;
    glViewport(f->vp_x, f->vp_y, f->vp_w, f->vp_h);
    glUseProgram(s_cprog);
    glBindVertexArray(s_cvao);
    glEnable(GL_BLEND);
    x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);      /* premultiplied */
    /* the inverted composite, and only once the engine has actually run a
       key-filled frame — on the frame the skip is first set its surface still
       carries a real terrain blit, and inverting on that would hide the world */
    /* ...and only while the fill is actually still running. If the game thread
       starts drawing a screen that never reaches 0x483FA0, its surface stops
       carrying our key and inverting against it would black that screen out;
       a stalled sequence simply stops the inversion and leaves our own FBO
       covering the viewport, which is always safe and self-corrects. */
    {
        unsigned seq = tagpu_terrown_fill_seq();
        if (seq != s_fillSeq) { s_fillSeq = seq; s_fillStall = 0; }
        else if (s_fillStall < 1000) s_fillStall++;
    }
    keyOn = (f->surface_tex && terrOwned && s_fillStall < 30) ? tagpu_terr_key() : -1;
    /* `keyOn >= 0` is precisely "the world you are looking at is OURS, drawn at
       our zoom" — the terrain is ours and the inverted composite is in force. It
       is the only honest moment to tell the input path to start unzooming: with,
       say, only the marker pass armed the screen still shows the engine's 1x
       world, and bending clicks against it would be the bug this fixes. */
    if (keyOn >= 0) tagpu_zoom_publish_view(vpL, vpT, vw, vh);
    if (s_uCKey >= 0) glUniform1i(s_uCKey, keyOn);
    if (keyOn >= 0) {
        GLint sz[2]; sz[0] = gw; sz[1] = gh;
        if (s_uCSurfSz >= 0) glUniform2iv(s_uCSurfSz, 1, sz);
        if (s_uCVp >= 0) x_glUniform4f(s_uCVp, (float)vpL, (float)vpT,
                                       (float)vw, (float)vh);
        x_glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_palTex);
        x_glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)f->surface_tex);
    }
    x_glActiveTexture(GL_TEXTURE0);
    /* G17b: under `devres` the world was never resolved down, so the composite
       reads the supersampled buffer (GL_LINEAR, i.e. a downsample to the
       viewport) instead of a game-res texture stretched up to it. */
        glBindTexture(GL_TEXTURE_2D, devres ? s_colTex2 : s_colTex);
    x_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    x_glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);

    static unsigned last = 0;
    if (f->frame_counter - last >= 300) {
        last = f->frame_counter;
        char b[768], bake[64];
        /* writes nothing at all unless tagpu_posebake.on is there */
        tagpu_posebake_stats(bake, sizeof bake);
        /* EVERY APPEND HERE IS BOUNDED, AND EVERY BUFFER IS TERMINATED BY HAND.
           This was `posed[96]` extended by two `lstrcatA`s with no
           remaining-space check. `tagpu_posedraw_stats` alone reaches 56 chars
           with all three ranges live, the degradation field adds up to 49 and
           `q=` 11 — and at the 10k-unit budget step 8 sizes for (§4's table)
           the counters are accumulated over the whole 300-frame window, so
           seven digits are reachable and the write runs off this frame.
           `_snprintf` also does NOT NUL-terminate what it truncates, so a
           truncated `posed` would have sent `lstrcatA` scanning past the end;
           the terminator is forced after every call rather than assumed. */
        char posed[192];
        int pn;
        tagpu_posedraw_stats(posed, sizeof posed);
        posed[sizeof posed - 1] = '\0';
        pn = lstrlenA(posed);
        /* G16 step 8's two DEGRADATIONS, printed only when they have caught
           something — in a healthy game neither ever does, and a field that
           reads 0 forever trains the eye to skip it. Both mean the unit still
           drew; see gpu-posing.md §4's refusal ledger. */
        if (posed[0] && (s_poseAtRest || s_poseUnplaced || s_poseNoBake)) {
            _snprintf(posed + pn, sizeof posed - pn, " rest=%u unpl=%u nobake=%u",
                      s_poseAtRest, s_poseUnplaced, s_poseNoBake);
            posed[sizeof posed - 1] = '\0';
            pn = lstrlenA(posed);
        }
        if (posed[0] && s_poseQueued != tagpu_posedraw_drawn()) {
            _snprintf(posed + pn, sizeof posed - pn, " q=%u", s_poseQueued);
            posed[sizeof posed - 1] = '\0';
        }
        /* ModelIds refused by model_root's bound — a unit slot recycled under
           the frame, or a torn read of one. Reported only when it has caught
           something, because in a healthy game it never does. */
        char badmodel[48];
        badmodel[0] = 0;
        if (s_badModelId)
            _snprintf(badmodel, sizeof badmodel, " BADMODELID=%u", s_badModelId);
        _snprintf(b, sizeof b,
                  "native: %d unit(s) %d wreck(s) %d sel %d bar(s) %d slant %d nano %d verts fbo=%dx%d ss=%d devres=%d subpix=%d scaf=%d fog=%d los=%u foglut=%d key=%d reread=%u%s%s%s%s",
                  nu - nwr, nwr, nsel, nmark, nslant, nwire, nv, gw, gh, ss, devres, s_subpix, scafOn, fogMode,
                  lostype, s_fogLut, keyOn, s_reread,
                  bake, posed, badmodel, s_vtrunc ? " VERTEX-BUDGET-HIT" : "");
        b[sizeof b - 1] = '\0';        /* _snprintf does not terminate a truncation */
        nlog(b);
        s_reread = 0;
        s_poseAtRest = 0; s_poseUnplaced = 0; s_poseNoBake = 0;
    }
    s_vtrunc = 0;
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
static void pose_dump(const char* u, const char* o3)
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
    const unsigned short* bturn = (const unsigned short*)(o3 + O3_BTURN);
    unsigned short bt[3];
    int nparts;
    char b[288];
    bt[0] = bturn[2];                       /* +0x1C = unit+0x68, about X */
    bt[1] = bturn[1];                       /* +0x1A = unit+0x66, about Y */
    bt[2] = bturn[0];                       /* +0x18 = unit+0x64, about Z */
    nparts = pose_accum_body(o3, &h, bt);
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
              "posedump: tick=%d idx=%d unit=%p o3=%p nparts=%d yaw=%u "
              "body=(%u,%u,%u) live=(%u,%u,%u)",
              *(const int*)(*(const char* const*)TA_MAINPP + 0x38A47),
              (int)*(const short*)(u + 0xA8), u, o3,
              (int)*(const unsigned short*)(o3 + O3_NUMPARTS),
              (unsigned)*(const unsigned short*)(u + U_YAW),
              (unsigned)bt[0], (unsigned)bt[1], (unsigned)bt[2],
              (unsigned)*(const unsigned short*)(u + 0x68),
              (unsigned)*(const unsigned short*)(u + U_YAW),
              (unsigned)*(const unsigned short*)(u + 0x64));
    nlog(b);
    if (!nparts) { nlog("posedump: pose_accum_body refused this unit"); return; }
    int p;
    for (p = 0; p < nparts && p < 32; p++) {
        const char* pr = h.pr[p];
        const char* nd = h.nd[p];
        const int*  vb = *(const int* const*)(pr + P_VBUF);
        const int*  of = (const int*)(nd + N_OFF);
        const int*  mv = (const int*)(pr + P_POS);
        const unsigned short* tn = (const unsigned short*)(pr + P_TURN);
        const char* nm = *(const char* const*)(nd + N_NAME);
        const int*  nv = *(const int* const*)(nd + N_VERTS);
        int cnt = *(const int*)(nd + N_VCOUNT);
        float worst = -1.0f;
        if (h.done[p] && ptr_ok(nv) && ptr_ok(vb) && cnt > 0 && cnt <= MAXNODEV &&
            !IsBadReadPtr(nv, (SIZE_T)cnt * 12) && !IsBadReadPtr(vb, (SIZE_T)cnt * 12)) {
            int k, r;
            worst = 0.0f;
            for (k = 0; k < cnt; k++) {
                float v[3], g[3];
                for (r = 0; r < 3; r++) v[r] = (float)nv[k*3+r] / 65536.0f;
                for (r = 0; r < 3; r++) {
                    const float* m = h.acc[p] + r * 4;
                    g[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3];
                }
                for (r = 0; r < 3; r++) {
                    float d = g[r] - (float)vb[k*3+r] / 65536.0f;
                    if (d < 0.0f) d = -d;
                    if (d > worst) worst = d;
                }
            }
        }
        _snprintf(b, sizeof b,
                  "posedump: p%d %s%s off=(%d,%d,%d) move=(%d,%d,%d) "
                  "turn=(%u,%u,%u) err=%.2f",
                  p, ptr_ok(nm) ? nm : "?",
                  (*(const unsigned char*)(pr + P_FLAGS) & 1) ? "" : " HIDDEN",
                  of[0], of[1], of[2], mv[0], mv[1], mv[2],
                  (unsigned)tn[0], (unsigned)tn[1], (unsigned)tn[2], worst);
        nlog(b);
        if (ptr_ok(nv) && ptr_ok(vb) && cnt > 0) {
            int k, kmax = cnt < 3 ? cnt : 3;
            for (k = 0; k < kmax; k++) {
                _snprintf(b, sizeof b,
                    "posedump:   v%d node=(%d,%d,%d) vbuf=(%d,%d,%d)", k,
                    nv[k*3], nv[k*3+1], nv[k*3+2], vb[k*3], vb[k*3+1], vb[k*3+2]);
                nlog(b);
            }
        }
    }
}

/* game-thread readable flag: the owndraw classifier suppresses the engine's
   scratch-fake-unit wreck rasterise only while the native husk pass is armed */
/* the fork restarts its render thread (NEW GL context) on every display-mode
   change — all our GL ids die; re-init from scratch on the next frame */
void tagpu_native_glreset(void)
{
    s_state = 0; s_fboW = s_fboH = s_fboSS = 0; s_palInit = 0;
    s_fogCols = s_fogRows = 0; s_fogGrid = NULL; s_fogLut = 0;
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
int tagpu_native_unit_pos(const char* u, float* x, float* y, float* z)
{
    const char* ta;
    const char* beg;
    const char* end;
    size_t off;
    int ix, iz, iy;
    float fx, fz, fy;

    if (!u || !x || !y || !z) return 0;
    if (!ptr_ok(u)) return 0;
    ta = *(const char* const*)TA_MAINPP;
    if (!ptr_ok(ta)) return 0;
    beg = *(const char* const*)(ta + OFF_BEGIN);
    end = *(const char* const*)(ta + OFF_END);
    /* BOTH ends, and the stride. Callers today pre-filter, but this is a
       published accessor and its contract is "one unit's position" — a pointer
       past the array's end, or one landing mid-slot, is not that. */
    if (!ptr_ok(beg) || !ptr_ok(end) || u < beg || u >= end) return 0;
    if ((size_t)(u - beg) % UNIT_STRIDE) return 0;

    ix = *(const int*)(u + U_XFIX);
    iz = *(const int*)(u + U_ZFIX);
    iy = *(const int*)(u + U_YFIX);
    fx = (float)ix / 65536.0f;
    fz = (float)iz / 65536.0f;
    fy = (float)iy / 65536.0f;

    off = (size_t)(u - beg);
    {
        size_t slot = off / UNIT_STRIDE;
        if (slot < 8192 && s_spx[slot].x == ix && s_spx[slot].z == iz &&
            s_spx[slot].y == iy)
            spx_sample(slot, s_spxFrame, &fx, &fz, &fy);
    }
    *x = fx; *y = fz; *z = fy;
    return 1;
}
