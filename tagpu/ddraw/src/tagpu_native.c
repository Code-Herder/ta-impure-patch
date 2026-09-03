/* tagpu_native.c — G12b: the first truly NATIVE unit pass (Phase D).

   Chosen unit types leave the 8bpp composite path entirely: their composites
   are wiped to ColorKey (engine keeps pose/AABB/alloc/blit — of nothing) and
   the units are rendered here, in the present hook, as RGB into a
   game-resolution FBO composited over the frame:

     - geometry: the same engine-posed PrimitiveStruct walk as render3do,
       positioned in VIEWPORT coordinates with the live viewport rect;
     - materials: the shared 8bpp atlas + engine SHD shade rows, lifted to RGB
       through the live palette (256x1 RGBA texture, refreshed per frame —
       palette cycling stays correct);
     - occlusion: per-fragment test against the G12a scene-depth scaffold
       (painter's row keys; a tall feature in a nearer row hides the unit),
       plus a real GL depth buffer for self/inter-unit occlusion;
     - fog: per-fragment sample of the LOS counter map + MAPPED bits
       (32-px tiles, uploaded as R8 textures each frame), LosType-aware;
     - shadow: engine rules (shadows-cloak.md): the unit silhouette, 50%
       black, +5px x, at ground height, options-gated, drawn before the body
       -- but ONLY for units whose engine shadow came from the composite we
       wipe (mobile units). Units on the engine's 0x20000000 path (structures)
       and wrecks keep the engine's CACHED slant-projected shadow, which is
       built from the posed prims and survives the wipe; drawing ours too gave
       them two shadows. FBI noshadow/canhover/floater gates mirror the engine;
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
#include "tagpu_native.h"
#include "tagpu_render3do.h"
#include "tagpu_scaffold.h"
#include "tagpu_hires.h"
#include "tagpu_hires_draw.h"
#include "tagpu_fx.h"
#include "tagpu_sfx.h"
#include "tagpu_feat.h"
#include "tagpu_terr.h"
#include "tagpu_terrown.h"
#include "tagpu_mark.h"
#include "tagpu_markown.h"
#include "tagpu_glsl.h"
#include "tagpu_zoom.h"
#include "tagpu_overlay.h"   /* tagpu_overlay_target_fbo: the frame's default draw target */
#include "tagpu_vpwide.h"

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
#define U_YAW        0x66      /* u16 body yaw, 65536 = 360 deg             */
#define U_MODELID    0xA6      /* u16 index into MODEL_PTRS                 */
#define OFF_MODELPTRS 0x14377  /* Model3DONode* [] (model templates)        */
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
#define O3_NUMPARTS  0x00
#define O3_THISUNIT  0x0C
#define O3_PRIM0     0x22
#define PRIM_STRIDE  0x36
#define P_NODE       0x00
#define P_VBUF       0x22
#define P_FLAGS      0x28
#define N_VCOUNT     0x04
#define N_FCOUNT     0x08
#define N_SELPRIM    0x0C      /* selection primitive index, -1 = none      */
#define N_VERTS      0x24      /* raw model-space verts i32[3] 16.16        */
#define N_FACES      0x28
#define FACE_STRIDE  0x20
#define F_COLORTAB   0x00
#define F_VCOUNT     0x04
#define F_INDICES    0x0C

#define MAXNV  49152           /* vertices across all native units per frame */
/* Units (and wrecks) gathered per frame. This is NOT a soft limit: a unit past
   it is not merely undrawn, it is INVISIBLE — tagpu_overlay.c wipes the engine's
   composite for every unit `tagpu_native_owns_unit` accepts, whether or not this
   gather included it. So it has to stay ahead of the rect the gather fills from,
   and since G13d that rect grows with zoom-out (16x the area at the 0.25x
   floor). MAXNV is the real budget and truncates gracefully; this one must not
   be what runs out first. */
#define MAXU   2048
#define NVST   11              /* x,y,depthEnc, u,v, flat,ck, shadeRow, wx,wzp, vy */
/* depth keys: ground rows encode as (feat?3:1) + rel*4 (rel = row − r0, up
   to the sweep's row count plus the ±256 px gather slack); the engine draws
   projectiles/explosions after every ground row and before the airborne sweep
   (terrain-depth.md 3), so per frame: fxKey = just above the last possible
   row, airKey above that, and the VS divides by a scale above airKey — no
   absolute constant survives a taller viewport */
#define ROW_SLACK 8               /* rows a gathered unit may sit past the sweep */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }
static void pose_dump(const char* u, const char* o3);

static void nlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_DEPTHFUNC)(GLenum);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_BLENDFUNC)(GLenum,GLenum);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM4F)(GLint,GLfloat,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_CLEARBUFFERFV)(GLenum,GLint,const GLfloat*);
typedef void (APIENTRY *PFN_DEPTHMASK)(GLboolean);
typedef void (APIENTRY *PFN_SCISSOR)(GLint,GLint,GLsizei,GLsizei);
typedef void (APIENTRY *PFN_LINEWIDTH)(GLfloat);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_DEPTHFUNC  x_glDepthFunc;
static PFN_DISABLE    x_glDisable;
static PFN_BLENDFUNC  x_glBlendFunc;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_UNIFORM4F  x_glUniform4f;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_CLEARBUFFERFV x_glClearBufferfv;
static PFN_DEPTHMASK  x_glDepthMask;
static PFN_SCISSOR    x_glScissor;
static PFN_LINEWIDTH  x_glLineWidth;

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
static GLuint s_prog, s_vao, s_vbo, s_fbo, s_colTex, s_depTex, s_palTex;
static GLuint s_fogTex, s_fogLutTex, s_cprog, s_cvao, s_cvbo;
static GLuint s_fbo2, s_colTex2, s_depTex2, s_dprog;
static GLint  s_uGame, s_uShadow, s_uAlpha, s_uFog, s_uFogOrg, s_uFogDim,
              s_uScafOn, s_uScafP;
static GLint  s_uWaterT, s_uWaterMode, s_uDigT;
static GLint  s_uOffset, s_uSS, s_uZoom, s_uZoomC, s_uZoomF, s_uZoomCF, s_uDepthScale;
static GLint  s_uCKey = -1, s_uCSurfSz = -1, s_uCVp = -1;  /* composite: the key */
static GLint  s_uCCur = -1, s_uCCurOff = -1;      /* ...and the cursor it moves */
/* Half-width of the box the cursor is moved in, game px. A TA cursor comes from
   a cursor_ary GAF frame and is a few tens of pixels; 64 is comfortably clear of
   the largest of them, and small enough that "the only engine pixel in here is
   the cursor" stays the safe assumption it is measured to be. */
#define CURSOR_PAD 64
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
    "uniform vec2 uGame;\n"                  /* game_width, game_height        */
    "uniform vec2 uOffset;\n"                /* shadow pass shift, px          */
    "uniform float uZoom;\n"                 /* G12d partial-zoom demo         */
    "uniform vec2 uZoomC;\n"                 /* zoom centre, game px           */
    "uniform float uDepthScale;\n"           /* > every key in use this frame  */
    "out vec2 vUV; flat out vec2 vFC; flat out float vShade; out vec2 vWorld;\n"
    "out float vEnc; out float vVY;\n"
    "void main(){\n"
    "  vec2 p = (aPos.xy + uOffset - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vFC = aFC; vShade = aShade; vWorld = aWorld; vEnc = aPos.z;\n"
    "  vVY = aVY;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec2 vFC; flat in float vShade; in vec2 vWorld;\n"
    "in float vEnc; in float vVY;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uLUT;\n"
    "uniform sampler2D uPal;\n"              /* 256x1 RGBA live palette        */
    TAGPU_GLSL_SCAF_UNIFORMS                  /* G12a scaffold, R8, viewport    */
    TAGPU_GLSL_FOG_UNIFORMS
    "uniform int uShadow;\n"
    "uniform float uAlpha;\n"
    "uniform float uWaterT;\n"              /* vy <= this is under water      */
    "uniform int uWaterMode;\n"             /* 1 erase (enemy), 2 tint (own)  */
    "uniform float uDigT;\n"                /* vy <= this is below ground     */
    TAGPU_GLSL_FOG_FN
    "void main(){\n"
    "  float idx;\n"
    "  if (vUV.x < 0.0) { idx = vFC.x; }\n"
    "  else {\n"
    "    idx = texture(uAtlas, vUV).r;\n"
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
    "  idx = texelFetch(uLUT, ivec2(int(idx*255.0+0.5), int(vShade*31.0+0.5)), 0).r;\n"
    "  int pi = int(idx*255.0+0.5);\n"
    TAGPU_GLSL_FOG_SHADE("pi")
    "  vec3 rgb = texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n"
    /* engine water table (prog+0xD0): r/2, g/2, b/2+0x32 */
    "  if (vVY <= uWaterT) {\n"
    "    if (uWaterMode == 1) discard;\n"
    "    rgb = rgb * 0.5 + vec3(0.0, 0.0, 50.0/255.0);\n"
    "  }\n"
    /* the FBO is PREMULTIPLIED: additive content (effects flashes) can then
       ride the same composite as (rgb, alpha 0) */
    "  frag = vec4(rgb * uAlpha, uAlpha);\n"
    "}\n";

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
/* MOVING THE MOUSE CURSOR (the other half of the zoom input fix, tagpu_zoom.h).
   The engine draws its cursor wherever it thinks the mouse is, which while the
   world is zoomed is the UNZOOMED position `u` we feed it, not where the pointer
   actually is. It has to be put back at `s`.

   It is done HERE rather than by capturing the draw the way G13d captured the
   markers, because the cursor is the one thing in the frame that does not go
   through DrawGameScreen's OFFSCREEN: `0x4C2870`/`0x4C2380` blit it with a NULL
   context, which makes the blit build its own offscreen over the primary
   surface, so swapping a pixel base cannot reach it. The composite is the one
   place that already owns this boundary — it is the code that decides, per
   pixel, whether the viewport shows ours or the engine's — and inside the
   viewport the engine's surface is 99.98 % key with NOTHING ON IT BUT THE CURSOR
   (terrain-depth.md 7.7, measured). So: paint the engine's own texels from the
   box around `u` at the box around `s`, and let our world cover the box at `u`.
   Both boxes are small, and the whole branch is off (uCur.z == 0) at zoom 1. */
static const char* CFS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uSurf;\n"
    "uniform sampler2D uPal;\n"
    "uniform ivec2 uSurfSz;\n"
    "uniform vec4 uVp;\n"          /* the rect the key fill covers, game px */
    "uniform vec4 uCur;\n"         /* cursor box at u (x,y,w,h); w=0 = off   */
    "uniform vec2 uCurOff;\n"      /* s - u                                  */
    "uniform int uKey;\n"
    "bool inbox(vec2 p, vec4 b){\n"
    "  return p.x >= b.x && p.x < b.x + b.z && p.y >= b.y && p.y < b.y + b.w;\n"
    "}\n"
    "void main(){\n"
    "  vec4 c = texture(uTex, uv);\n"
    "  bool empty = c.a < 0.004 && max(max(c.r, c.g), c.b) < 0.004;\n"
    "  vec2 px = uv * vec2(uSurfSz);\n"
    /* Only inside the viewport is the engine's frame our key fill. Outside it
       the frame is UI we never touched, so the old rule stands there — and a
       UI pixel that happens to BE the key index can never be mistaken for it. */
    "  if (uKey >= 0 && px.x >= uVp.x && px.x < uVp.x + uVp.z &&\n"
    "                   px.y >= uVp.y && px.y < uVp.y + uVp.w) {\n"
    "    if (uCur.z > 0.0) {\n"
    /* where the pointer is: paint the cursor texel the engine put at u */
    "      vec2 sp = px - uCurOff;\n"
    /* ...and only from inside the viewport: `u` is in it but not 64 px clear
       of its edge, so the box can straddle the boundary and would otherwise
       stamp side-panel or top-bar texels into the world. `u` being in it is an
       invariant tagpu_vpwide would break — a widened rect puts the ring's `u`
       on the panel or off the surface — so while that module is live it takes
       the cursor over entirely and uCur.z is 0 here (tagpu_zoom_cursor_shift). */
    "      if (inbox(sp, uCur) && inbox(sp, uVp)) {\n"
    "        ivec2 q = clamp(ivec2(sp), ivec2(0), uSurfSz - 1);\n"
    "        int si = int(texelFetch(uSurf, q, 0).r * 255.0 + 0.5);\n"
    "        if (si != uKey) {\n"
    "          frag = vec4(texelFetch(uPal, ivec2(si, 0), 0).rgb, 1.0);\n"
    "          return;\n"
    "        }\n"
    "      }\n"
    /* where the engine put it: ours covers it, so do NOT fall through to the
       discard that would reveal the engine's frame and its stale cursor */
    "      if (inbox(px, uCur)) {\n"
    "        frag = empty ? vec4(0.0, 0.0, 0.0, 1.0) : c;\n"
    "        return;\n"
    "      }\n"
    "    }\n"
    "    ivec2 p = clamp(ivec2(px), ivec2(0), uSurfSz - 1);\n"
    "    if (int(texelFetch(uSurf, p, 0).r * 255.0 + 0.5) != uKey) discard;\n"
    /* the engine drew nothing here and neither did we: never let the raw key
       fill reach the screen — black is what the engine paints for "no world" */
    "    if (empty) { frag = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
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
    x_glUniform4f  = (PFN_UNIFORM4F) getgl("glUniform4f");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glClearBufferfv = (PFN_CLEARBUFFERFV)getgl("glClearBufferfv");
    x_glDepthMask  = (PFN_DEPTHMASK) getgl("glDepthMask");
    x_glScissor    = (PFN_SCISSOR)   getgl("glScissor");
    x_glLineWidth  = (PFN_LINEWIDTH) getgl("glLineWidth");
    if (!x_glDrawArrays || !x_glDepthFunc || !x_glDisable || !x_glBlendFunc ||
        !x_glUniform1f || !x_glUniform2f || !x_glUniform4f || !x_glActiveTexture ||
        !x_glClearBufferfv || !x_glDepthMask)
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
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "uLUT"),   1);
    glUniform1i(glGetUniformLocation(s_prog, "uPal"),   2);
    glUniform1i(glGetUniformLocation(s_prog, "uScaf"),  3);
    glUniform1i(glGetUniformLocation(s_prog, "uFogGrid"), 4);
    glUniform1i(glGetUniformLocation(s_prog, "uFogLUT"),  5);
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
    s_uCCur = glGetUniformLocation(s_cprog, "uCur");
    s_uCCurOff = glGetUniformLocation(s_cprog, "uCurOff");
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_colTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_depTex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLenum st2 = 0;
    if (ss > 1) {
        glBindTexture(GL_TEXTURE_2D, s_colTex2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * ss, h * ss, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, s_depTex2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w * ss, h * ss, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo2);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_colTex2, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_depTex2, 0);
        st2 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());
    s_fboW = w; s_fboH = h; s_fboSS = ss;
    { char b[96]; _snprintf(b, sizeof b, "native: FBO %dx%d ss=%d status=%x/%x",
                            w, h, ss, st, st2); nlog(b); }
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

/* Is this unit natively owned RIGHT NOW? (armed + type + complete) */
int tagpu_native_owns_unit(const char* u)
{
    if (s_armed != 1) return 0;
    const char* def = *(const char* const*)(u + U_TYPE);
    if (!ptr_ok(def)) return 0;
    if (!type_match(def)) return 0;
    float nano = *(const float*)(u + U_NANO);
    return nano <= 0.0f;
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
typedef struct { const char* node; float mn[3], mx[3]; } MAABB;
static MAABB s_aabb[64];
static int   s_naabb = 0;

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

static const MAABB* model_aabb(const char* root)
{
    int i;
    for (i = 0; i < s_naabb; i++)
        if (s_aabb[i].node == root) return &s_aabb[i];
    if (s_naabb >= 64) return NULL;
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
                }
            }
            for (t = 0; t < 3; t++) {
                float x = V[t][0], y = V[t][1], z = V[t][2];
                float* o = s_verts + nv * NVST;
                o[0] = ax + x;
                o[1] = ay + (-z - y * 0.5f);
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
                o[8] = wx0 + x;
                o[9] = wz0 + (-z - y * 0.5f);
                o[10] = y;
                nv++;
            }
        }
    }
    return nv;
}

static float s_P[MAXNODEV * 3];     /* one node's model-space vertices */

static int emit_geom(const char* o3, int nv, float ax, float ay,
                     float wx0, float wz0, float encBase, int owner)
{
    int nparts = *(const unsigned short*)(o3 + O3_NUMPARTS);
    if (nparts <= 0 || nparts > 64) return nv;

    int anyShadeFlag = 0, p;
    for (p = 0; p < nparts; p++) {
        unsigned char fl = *(const unsigned char*)(o3 + O3_PRIM0 + p * PRIM_STRIDE + P_FLAGS);
        if ((fl & 1) && (fl & 4)) anyShadeFlag = 1;
    }
    for (p = 0; p < nparts; p++) {
        const char* pr = o3 + O3_PRIM0 + p * PRIM_STRIDE;
        unsigned char pflags = *(const unsigned char*)(pr + P_FLAGS);
        if (!(pflags & 1)) continue;
        int pieceShaded = anyShadeFlag ? ((pflags & 4) != 0) : 1;
        const char* nd = *(const char* const*)(pr + P_NODE);
        const int*  vb = *(const int* const*)(pr + P_VBUF);
        if (!ptr_ok(nd) || !ptr_ok(vb)) continue;
        int nvert = *(const int*)(nd + N_VCOUNT);
        if (nvert <= 0 || nvert > MAXNODEV || IsBadReadPtr(vb, (SIZE_T)nvert * 12)) continue;
        int i;
        for (i = 0; i < nvert * 3; i++) s_P[i] = (float)vb[i] / 65536.0f;
        nv = emit_node(nd, s_P, nvert, nv, ax, ay, wx0, wz0, encBase, owner,
                       pieceShaded, -1, 0);
    }
    return nv;
}

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

void tagpu_native_frame(const TAGPU_FRAME* f)
{
    if (s_state == 2) return;
    if (s_armed < 0 || (f->frame_counter % 30) == 0) {
        int was = s_armed;
        s_armed = 0;
        HANDLE h = CreateFileA("tagpu_native.on", GENERIC_READ, FILE_SHARE_READ,
                               0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[64]; DWORD n = 0;
            if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
                buf[n] = 0;
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
            CloseHandle(h);
            s_armed = 1;
        }
        s_ss     = (GetFileAttributesA("tagpu_ss.off")     == INVALID_FILE_ATTRIBUTES);
        s_subpix = (GetFileAttributesA("tagpu_subpix.off") == INVALID_FILE_ATTRIBUTES);
        s_spxlog = (GetFileAttributesA("tagpu_spxlog.on")  != INVALID_FILE_ATTRIBUTES);
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
    if (!s_armed && !fxOn && !sfxOn && !featOn && !terrOn && !markOn) return;
    if (s_state == 0) init_gl();
    if (s_state != 1 || !tagpu_r3d_ensure()) return;

    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
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

    /* ---- live palette (cycles!) ---- */
    {
        const unsigned char* pal = (const unsigned char*)(ta + 0x143A7);
        static unsigned char rgba[256 * 4];
        int i;
        for (i = 0; i < 256; i++) {
            rgba[i*4+0] = pal[i*4+0]; rgba[i*4+1] = pal[i*4+1];
            rgba[i*4+2] = pal[i*4+2]; rgba[i*4+3] = 255;
        }
        x_glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_palTex);
        if (!s_palInit) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba); s_palInit = 1; }
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        x_glActiveTexture(GL_TEXTURE0);
    }

    /* ---- gather native-owned on-screen units ---- */
    typedef struct { const char* o3; const char* u; const void* hires;
                     float ax, ay, gy, wx0, wz0;
                     int rel, owner, cloaked, air, feat, sel, shadow; unsigned yaw;
                     float waterT, digT; int waterMode; } NU;
    static NU units[MAXU];
    /* sub-pixel motion: the engine keeps 16.16 fixed-point positions (the
       roster shorts are just their high words) — read the true fractions and
       interpolate between the last two sim samples per unit SLOT for
       present-rate smoothness (slot reuse is caught by the distance snap) */
    typedef struct { int x, z, y; int px, pz, py; unsigned tc, tp; } SPX;
    static SPX spx[8192];
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
                SPX* e = &spx[slot];
                if (e->tc == 0 || e->x != ix || e->z != iz || e->y != iy) {
                    e->px = e->x; e->pz = e->z; e->py = e->y; e->tp = e->tc;
                    e->x = ix; e->z = iz; e->y = iy; e->tc = f->frame_counter;
                }
                unsigned dt = e->tc - e->tp, el = f->frame_counter - e->tc;
                float dx = (float)(e->x - e->px) / 65536.0f;
                float dz = (float)(e->z - e->pz) / 65536.0f;
                float dy = (float)(e->y - e->py) / 65536.0f;
                if (e->tp != 0 && dt >= 1 && dt <= 30 && el < dt &&
                    dx * dx + dy * dy + dz * dz <= 1024.0f) {
                    float a = (float)el / (float)dt;
                    fx = (float)e->px / 65536.0f + dx * a;
                    fz = (float)e->pz / 65536.0f + dz * a;
                    fy = (float)e->py / 65536.0f + dy * a;
                }
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
        n2->o3 = o3; n2->u = u; n2->ax = ax; n2->ay = ay;
        n2->wx0 = fx; n2->wz0 = fy - fz * 0.5f;
        n2->yaw = *(const unsigned short*)(u + U_YAW);
        n2->hires = NULL;
        n2->shadow = 0;
        n2->waterT = -1e9f; n2->digT = -1e9f; n2->waterMode = 0;
        unsigned mask = 0;                    /* FBI booleans, read below */
        {
            const char* def = *(const char* const*)(u + U_TYPE);
            if (ptr_ok(def)) {
                /* mirror the engine's shadow branch in the blit 0x459200
                   (shadows-cloak.md §3): ST_STRUCT units get the cached
                   slant shadow (Object3do+0x14) from the engine itself;
                   the rest get a composite-derived silhouette the wipe
                   emptied -- that one is ours, under the engine's FBI gates */
                mask = *(const unsigned*)(def + UD_TYPEMASK);
                n2->shadow = !(st & ST_STRUCT) &&
                             !(mask & 0x02000000u) &&      /* noshadow          */
                             !(mask & 0x00081000u);        /* canhover|floater  */
                char nm[32]; int ci;
                for (ci = 0; ci < 31; ci++) {
                    char cch = def[0x20 + ci];   /* UnitName, e.g. ARMSOLAR */
                    if (cch >= 'A' && cch <= 'Z') cch = (char)(cch + 32);
                    nm[ci] = cch;
                    if (!cch) break;
                }
                nm[31] = 0;
                n2->hires = tagpu_hires_mesh(nm);
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
            }
            n2->gy = gy;
        }
        n2->rel = (wy >> 4) - r0;
        n2->owner = owner; n2->cloaked = cloaked;
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
                n2->wx0 = (float)rx; n2->wz0 = (float)(ry - rz / 2);
                n2->rel = (ry >> 4) - r0;
                n2->owner = 0; n2->cloaked = 0; n2->air = 0; n2->feat = 1; n2->sel = 0;
                n2->shadow = 0;     /* the engine's FShadow feature shadow stays */
                n2->waterT = -1e9f; n2->digT = -1e9f; n2->waterMode = 0;
                nwr++;
            }
        }
    }
    /* ---- effects gather (projectiles, explosions, debris, particles) ---- */
    TAGPU_FXVIEW fv;
    int nfx = 0, nfeat = 0, nterr = 0, nmark = 0;
    if (fxOn || sfxOn || featOn || terrOn || markOn) {
        fv.ta = ta; fv.eyeX = eyeX; fv.eyeY = eyeY;
        fv.vpL = vpL; fv.vpT = vpT; fv.vw = vw; fv.vh = vh; fv.scafOn = scafOn;
        fv.evpL = evpL; fv.evpT = evpT; fv.evw = evw; fv.evh = evh;
        fv.gw = gw; fv.gh = gh; fv.ss = s_ss ? 2 : 1; fv.fogMode = fogMode;
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
    int i;
    static TAGPU_HUNIT hunits[MAXU];
    int nhi = 0;
    for (i = 0; i < nu; i++) {
        firstv[i] = nv;
        /* airborne units draw in a second, un-rowed sweep above everything
           (terrain-depth 3.3) -> the air band above the effects band; wrecks
           sit at FEATURE depth (3+rel*4, terrain-depth 3.4) */
        float encBase = units[i].air ? airKey
                      : (units[i].feat ? 3.0f : 1.0f) + (float)units[i].rel * 4.0f;
        encb[i] = encBase;
        if (units[i].hires) {
            /* no vertices here: this unit is the other pass's, and leaving
               firstv[i] == firstv[i+1] makes its draws below empty */
            TAGPU_HUNIT* h = &hunits[nhi++];
            h->mesh = units[i].hires;
            h->ax = units[i].ax;   h->ay = units[i].ay;
            h->wx0 = units[i].wx0; h->wz0 = units[i].wz0;
            h->enc = encBase;
            h->shadowDy = (float)(units[i].gy - units[i].ay);
            h->yaw = units[i].yaw;
            h->alpha = units[i].cloaked ? 0.5f : 1.0f;
            h->fog = FOGW(i);
            h->waterT = units[i].waterT;
            h->digT = units[i].digT;
            h->waterMode = units[i].waterMode;
            h->shadow = units[i].shadow;
        } else {
            nv = emit_geom(units[i].o3, nv, units[i].ax, units[i].ay,
                           units[i].wx0, units[i].wz0, encBase, units[i].owner);
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
    if (nv == 0 && nfx == 0 && nfeat == 0 && nterr == 0 && !markOn && !terrOwned) return;

    /* ---- native selection rects (ui-markers: the ONLY marker interleaved
       with unit draws — the engine's is unreadable under our pixels, redraw
       it): flat model-XZ AABB rect at lowest model Y, rotated by body yaw,
       GUI colour 0xA, drawn as GL_LINES at just-under-the-unit depth ---- */
    int lineStart = nv, selDrawn = 0;
    if (nsel) {
        const char* mptrs = *(const char* const*)(ta + OFF_MODELPTRS);
        for (i = 0; i < nu && nv + 8 <= MAXNV; i++) {
            if (!units[i].sel || !units[i].u) continue;
            unsigned mid = *(const unsigned short*)(units[i].u + U_MODELID);
            if (!ptr_ok(mptrs)) break;
            const char* root = *(const char* const*)(mptrs + (size_t)mid * 4);
            const MAABB* a = model_aabb(root);
            if (!a) continue;
            float yawA = (float)*(const unsigned short*)(units[i].u + U_YAW)
                         * 6.2831853f / 65536.0f;
            float c = cosf(yawA), s2 = sinf(yawA);
            float y0 = a->mn[1];
            float cx[4] = { a->mn[0], a->mx[0], a->mx[0], a->mn[0] };
            float cz[4] = { a->mn[2], a->mn[2], a->mx[2], a->mx[2] };
            float px[4], py[4];
            int k;
            for (k = 0; k < 4; k++) {
                float rx2 = cx[k] * c + cz[k] * s2;
                float rz2 = -cx[k] * s2 + cz[k] * c;
                px[k] = units[i].ax + rx2;
                py[k] = units[i].ay + (-rz2 - y0 * 0.5f);
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

    /* ---- render into the (optionally 2x supersampled) game-res FBO ---- */
    int ss = s_ss ? 2 : 1;
    fbo_size(gw, gh, ss);
    glBindFramebuffer(GL_FRAMEBUFFER, ss > 1 ? s_fbo2 : s_fbo);
    glViewport(0, 0, gw * ss, gh * ss);
    { const GLfloat cl[4] = { 0, 0, 0, 0 }; x_glClearBufferfv(GL_COLOR, 0, cl); }
    glClear(GL_DEPTH_BUFFER_BIT);
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
    x_glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nv * NVST * 4, s_verts);

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

    /* selection rects first — the engine draws them under the unit sprite */
    if (nv > lineStart) {
        glUniform1i(s_uFog, fogMode & 1);
        glUniform1i(s_uShadow, 0);
        x_glUniform1f(s_uAlpha, 1.0f);
        x_glUniform2f(s_uOffset, 0.0f, 0.0f);
        x_glUniform1f(s_uWaterT, -1e9f);
        x_glUniform1f(s_uDigT, -1e9f);
        glUniform1i(s_uWaterMode, 0);
        if (x_glLineWidth) x_glLineWidth((GLfloat)ss);
        x_glDrawArrays(GL_LINES, lineStart, nv - lineStart);
    }

    /* shadow first (engine order), only when options allow; per unit so an
       aircraft's shadow lands at GROUND height (gy - ay shifts body->ground) */
    if ((gfx & 4) && (gfx & 8)) {
        glUniform1i(s_uShadow, 1);
        x_glUniform1f(s_uAlpha, 0.5f);
        x_glDepthMask(GL_FALSE);
        for (i = 0; i < nu; i++) {
            if (!units[i].shadow) continue;
            glUniform1i(s_uFog, FOGW(i));
            x_glUniform2f(s_uOffset, 5.0f, (float)(units[i].gy - units[i].ay));
            x_glUniform1f(s_uWaterT, units[i].waterT);
            x_glUniform1f(s_uDigT, units[i].digT);
            x_glDrawArrays(GL_TRIANGLES, firstv[i], firstv[i+1] - firstv[i]);
        }
        if (nhi) {
            tagpu_hires_draw(&hv, hunits, nhi, 1, f->frame_counter);
            HIRES_RESTORE();
        }
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
        x_glDrawArrays(GL_TRIANGLES, firstv[i], firstv[i+1] - firstv[i]);
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

    /* ---- box-downsample 2x -> 1x (quad covers every pixel; no clear) ---- */
    if (ss > 1) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, gw, gh);
        glUseProgram(s_dprog);
        glBindVertexArray(s_cvao);
        x_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_colTex2);
        x_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
        /* the cursor the engine drew at `u`, and where it belongs (see CFS) */
        if (s_uCCur >= 0) {
            int dx, dy, ux, uy;
            if (tagpu_zoom_cursor_shift(&dx, &dy, &ux, &uy)) {
                x_glUniform4f(s_uCCur, (float)(ux - CURSOR_PAD),
                                       (float)(uy - CURSOR_PAD),
                                       (float)(2 * CURSOR_PAD),
                                       (float)(2 * CURSOR_PAD));
                if (s_uCCurOff >= 0) x_glUniform2f(s_uCCurOff, (float)dx, (float)dy);
            } else {
                x_glUniform4f(s_uCCur, 0.0f, 0.0f, 0.0f, 0.0f);
            }
        }
        x_glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, s_palTex);
        x_glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)f->surface_tex);
    }
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_colTex);
    x_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    x_glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);

    static unsigned last = 0;
    if (f->frame_counter - last >= 300) {
        last = f->frame_counter;
        char b[160];
        _snprintf(b, sizeof b,
                  "native: %d unit(s) %d wreck(s) %d sel %d bar(s) %d verts fbo=%dx%d ss=%d subpix=%d scaf=%d fog=%d los=%u foglut=%d key=%d%s",
                  nu - nwr, nwr, nsel, nmark, nv, gw, gh, ss, s_subpix, scafOn, fogMode,
                  lostype, s_fogLut, keyOn, s_vtrunc ? " VERTEX-BUDGET-HIT" : "");
        nlog(b);
    }
    s_vtrunc = 0;
}

/* G11 groundwork: one-shot numeric dump of the engine's per-piece pose data
   for the first owned unit — posed pos/turn PLUS raw node verts vs posed vbuf
   verts, enough to solve the exact transform convention offline (order/signs
   of Rz,Rx,Ry at 65536=360°). Armed by tagpu_posedump.on (self-deleting). */
static void pose_dump(const char* u, const char* o3)
{
    if (GetFileAttributesA("tagpu_posedump.on") == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA("tagpu_posedump.on");
    int nparts = *(const unsigned short*)(o3 + O3_NUMPARTS);
    char b[256];
    _snprintf(b, sizeof b, "posedump: unit=%p o3=%p nparts=%d yaw=%u", u, o3, nparts,
              (unsigned)*(const unsigned short*)(u + U_YAW));
    nlog(b);
    int p;
    for (p = 0; p < nparts && p < 20; p++) {
        const char* pr = o3 + O3_PRIM0 + p * PRIM_STRIDE;
        const char* nd = *(const char* const*)(pr + P_NODE);
        const int*  vb = *(const int* const*)(pr + P_VBUF);
        if (!ptr_ok(nd)) continue;
        const int* pp = (const int*)(pr + 0x04);          /* posed pos 16.16 */
        const unsigned short* pt = (const unsigned short*)(pr + 0x10); /* posed turn */
        const char* nm = *(const char* const*)(nd + 0x1C);
        _snprintf(b, sizeof b, "posedump: p%d %s pos=(%d,%d,%d) turn=(%u,%u,%u)",
                  p, ptr_ok(nm) ? nm : "?", pp[0], pp[1], pp[2],
                  (unsigned)pt[0], (unsigned)pt[1], (unsigned)pt[2]);
        nlog(b);
        const int* nv = *(const int* const*)(nd + 0x24);
        int cnt = *(const int*)(nd + 0x04);
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
    tagpu_fx_glreset();
    tagpu_feat_glreset();
    tagpu_terr_glreset();
    tagpu_mark_glreset();
    tagpu_hires_draw_glreset();
}

int tagpu_native_wrecks_armed(void)
{
    return s_armed > 0 && s_state != 2 && s_wrecks;
}

int tagpu_native_selbox_complete(void) { return s_selComplete; }
