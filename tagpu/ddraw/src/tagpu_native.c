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
#include "tagpu_fx.h"

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
#define OFF_MAPPED   0x14273   /* u16 per 32-px tile, bit p = explored      */
#define OFF_LOSTYPE  0x14281   /* u16: b0 mapping, b1 true LOS              */
#define OFF_WATCHED  0x2A42    /* byte watched player id                    */
#define OFF_PLAYERS  0x1B63    /* PlayerStruct[10], stride 0x14B            */
#define PL_STRIDE    0x14B
#define PL_LOSMAP    0x7C      /* u8* LOS counter map                       */
#define PL_LOSW      0x80
#define PL_LOSH      0x84
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
#define SELBOX_COLIDX 0x0A     /* GUI colour: bright green palette index    */
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
static GLuint s_losTex, s_mapTex, s_cprog, s_cvao, s_cvbo;
static GLuint s_fbo2, s_colTex2, s_depTex2, s_dprog;
static GLint  s_uGame, s_uShadow, s_uAlpha, s_uFog, s_uScafOn, s_uScafP;
static GLint  s_uWaterT, s_uWaterMode, s_uDigT;
static GLint  s_uOffset, s_uSS, s_uZoom, s_uZoomC, s_uZoomF, s_uZoomCF, s_uDepthScale;
static float  s_zoom = 1.0f;
static int    s_fboW = 0, s_fboH = 0, s_fboSS = 0;
static int    s_palInit = 0;
static int    s_losW = 0, s_losH = 0;
static float  s_verts[MAXNV * NVST];
static unsigned char s_losBuf[512 * 512], s_mapBuf[512 * 512];

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
    "uniform sampler2D uScaf;\n"             /* G12a scaffold, R8, viewport    */
    "uniform sampler2D uLos;\n"              /* LOS counter bytes, 32px tiles  */
    "uniform sampler2D uMap;\n"              /* explored bits, 32px tiles      */
    "uniform int uShadow;\n"
    "uniform float uAlpha;\n"
    "uniform int uFog;\n"                    /* bit0 mapping on, bit1 true LOS */
    "uniform int uScafOn;\n"
    "uniform vec4 uScafP;\n"                 /* vpL, vpT, vw, vh (frame px)    */
    "uniform float uSS;\n"                   /* supersample factor (1 or 2)    */
    "uniform float uZoomF;\n"
    "uniform vec2 uZoomCF;\n"
    "uniform float uWaterT;\n"              /* vy <= this is under water      */
    "uniform int uWaterMode;\n"             /* 1 erase (enemy), 2 tint (own)  */
    "uniform float uDigT;\n"                /* vy <= this is below ground     */
    "void main(){\n"
    "  float idx;\n"
    "  if (vUV.x < 0.0) { idx = vFC.x; }\n"
    "  else {\n"
    "    idx = texture(uAtlas, vUV).r;\n"
    "    if (abs(idx - vFC.y) < 0.5/255.0) discard;\n"
    "  }\n"
    /* scaffold occlusion: nearer stamped rows hide this fragment */
    "  if (uScafOn == 1) {\n"
    /* VS maps game py 0 -> NDC -1 -> FBO window y 0, so gl_FragCoord.xy/uSS IS
       the game-frame pixel; scaffold texture row 0 = viewport top (top-down). */
    "    vec2 fc = gl_FragCoord.xy / uSS;\n"
    "    fc = (fc - uZoomCF) / uZoomF + uZoomCF;\n"
    "    vec2 uv = vec2((fc.x - uScafP.x) / uScafP.z,\n"
    "                   (fc.y - uScafP.y) / uScafP.w);\n"
    "    if (uv.x >= 0.0 && uv.x < 1.0 && uv.y >= 0.0 && uv.y < 1.0) {\n"
    "      float s = texture(uScaf, uv).r * 255.0;\n"
    "      if (s > vEnc + 0.5) discard;\n"
    "    }\n"
    "  }\n"
    /* waterline / digger clipping (shadows-cloak.md §3 depth-bias rules):
       the engine erases or tints composite pixels whose depth (= vertex
       height + bias) is at or below the water line; shadows are always
       erased there. Our per-fragment height is the interpolated model y. */
    "  if (vVY <= uDigT) discard;\n"
    "  if (uShadow == 1) { if (vVY <= uWaterT) discard;\n"
    "                      frag = vec4(0.0, 0.0, 0.0, 0.5); return; }\n"
    "  idx = texelFetch(uLUT, ivec2(int(idx*255.0+0.5), int(vShade*31.0+0.5)), 0).r;\n"
    "  vec3 rgb = texelFetch(uPal, ivec2(int(idx*255.0+0.5), 0), 0).rgb;\n"
    /* engine water table (prog+0xD0): r/2, g/2, b/2+0x32 */
    "  if (vVY <= uWaterT) {\n"
    "    if (uWaterMode == 1) discard;\n"
    "    rgb = rgb * 0.5 + vec3(0.0, 0.0, 50.0/255.0);\n"
    "  }\n"
    /* fog: 32-px LOS tiles, engine rules (terrain-depth.md 5.3) */
    "  if ((uFog & 1) == 1) {\n"
    "    ivec2 t = ivec2(int(vWorld.x) >> 5, int(vWorld.y) >> 5);\n"
    "    float ex = texelFetch(uMap, t, 0).r;\n"
    "    if (ex < 0.5/255.0) discard;\n"            /* unexplored: engine draws nothing */
    "    if ((uFog & 2) == 2) {\n"
    "      float lit = texelFetch(uLos, t, 0).r;\n"
    "      if (lit < 0.5/255.0) rgb *= 0.55;\n"     /* explored, out of LOS  */
    "    }\n"
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
static const char* CFS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag; uniform sampler2D uTex;\n"
    "void main(){ vec4 c = texture(uTex, uv);\n"
    "  if (c.a < 0.004 && max(max(c.r, c.g), c.b) < 0.004) discard; frag = c; }\n";

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
    glUniform1i(glGetUniformLocation(s_prog, "uLos"),   4);
    glUniform1i(glGetUniformLocation(s_prog, "uMap"),   5);
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
    tex2d(&s_losTex, GL_NEAREST);
    tex2d(&s_mapTex, GL_NEAREST);
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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

/* G12d replacement-mesh emitter: flat-colour tris through the same shade/
   palette pipeline, rotated by body yaw, anchored like the 3DO would be */
static int emit_hires(const void* mesh, int nv, float ax, float ay,
                      float wx0, float wz0, float encBase, unsigned yaw)
{
    int nt = tagpu_hires_ntri(mesh), i, k;
    int shNeutral = tagpu_r3d_shade_neutral(), shDir = tagpu_r3d_shade_dir();
    float ang = (float)yaw * 6.2831853f / 65536.0f;
    float c = cosf(ang), sn = sinf(ang);
    for (i = 0; i < nt; i++) {
        if (nv + 3 > MAXNV) return nv;
        const float* t = tagpu_hires_tri(mesh, i);
        float colv = (float)tagpu_hires_col(mesh, i) / 255.0f;
        float P[3][3];
        for (k = 0; k < 3; k++) {
            float x = t[k*3+0], y = t[k*3+1], z = t[k*3+2];
            P[k][0] = x * c + z * sn;
            P[k][1] = y;
            P[k][2] = -x * sn + z * c;
        }
        float shade = (float)shNeutral / 31.0f;
        {
            float e1x = P[1][0]-P[0][0], e1y = P[1][1]-P[0][1], e1z = P[1][2]-P[0][2];
            float e2x = P[2][0]-P[0][0], e2y = P[2][1]-P[0][1], e2z = P[2][2]-P[0][2];
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
        for (k = 0; k < 3; k++) {
            float x = P[k][0], y = P[k][1], z = P[k][2];
            float* o = s_verts + nv * NVST;
            o[0] = ax + x;
            o[1] = ay + (-z - y * 0.5f);
            float md = (2.0f * y - z) / 256.0f;
            if (md > 1.8f) md = 1.8f; if (md < -1.8f) md = -1.8f;
            o[2] = encBase + md;
            o[3] = -1.0f; o[4] = -1.0f;
            o[5] = colv;  o[6] = -1.0f;
            o[7] = shade;
            o[8] = wx0 + x;
            o[9] = wz0 + (-z - y * 0.5f);
            o[10] = y;
            nv++;
        }
    }
    return nv;
}

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
        {
            s_zoom = 1.0f;
            HANDLE zh = CreateFileA("tagpu_zoom.txt", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
            if (zh != INVALID_HANDLE_VALUE) {
                char zb[32]; DWORD zn = 0;
                if (ReadFile(zh, zb, sizeof zb - 1, &zn, 0) && zn > 0) {
                    zb[zn] = 0;
                    float z = (float)atof(zb);
                    if (z >= 0.25f && z <= 8.0f) s_zoom = z;
                }
                CloseHandle(zh);
            }
        }
        if (s_armed != was && was >= 0) {
            char b[96]; _snprintf(b, sizeof b, "native: %s (type=%s wrecks=%d ss=%d subpix=%d)",
                                  s_armed ? "ARMED" : "disarmed", s_type,
                                  s_wrecks, s_ss, s_subpix);
            nlog(b);
        }
    }
    /* the effects pass (tagpu_fx.on) rides this frame: it needs the view,
       fog and palette set up here and draws into this FBO */
    int fxOn = tagpu_fx_armed(f->frame_counter);
    if (!s_armed && !fxOn) return;
    if (s_state == 0) init_gl();
    if (s_state != 1 || !tagpu_r3d_ensure()) return;

    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    if (!ptr_ok(beg) || !ptr_ok(end) || end <= beg) return;
    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000) return;

    int vpL = *(int*)(ta + OFF_VP_L), vpT = *(int*)(ta + OFF_VP_T);
    int vw  = *(int*)(ta + OFF_VIEW_W), vh = *(int*)(ta + OFF_VIEW_H);
    int eyeX = *(int*)(ta + OFF_EYEX), eyeY = *(int*)(ta + OFF_EYEY);
    if (vw < 64 || vh < 64 || vw > 4096 || vh > 4096) return;
    int gw = f->game_width  > 0 ? f->game_width  : vpL + vw;
    int gh = f->game_height > 0 ? f->game_height : vpT + vh;

    int scafR0 = 0, scafRows = 0;
    int scafOn = tagpu_scaffold_frameinfo(f->frame_counter, &scafR0, &scafRows);
    int r0 = scafOn ? scafR0 : (eyeY >> 4) - 16;
    int rows = scafRows > 0 ? scafRows : (vh >> 4) + 32;
    /* this frame's depth bands (see ROW_SLACK) */
    float fxKey = 3.0f + (float)(rows + ROW_SLACK) * 4.0f + 4.0f;
    float airKey = fxKey + 12.0f;
    float depthScale = airKey + 8.0f;

    unsigned gfx = *(unsigned short*)(ta + OFF_GFXOPT);
    unsigned lostype = *(unsigned short*)(ta + OFF_LOSTYPE);
    int watched = *(unsigned char*)(ta + OFF_WATCHED);

    /* ---- LOS/MAPPED upload (32-px tiles) ---- */
    int fogMode = 0;
    {
        const char* pl = ta + OFF_PLAYERS + (size_t)watched * PL_STRIDE;
        const unsigned char* los = *(const unsigned char* const*)(pl + PL_LOSMAP);
        int lw = *(const int*)(pl + PL_LOSW), lh = *(const int*)(pl + PL_LOSH);
        const unsigned short* mapd = *(const unsigned short* const*)(ta + OFF_MAPPED);
        if (ptr_ok(los) && ptr_ok(mapd) && lw > 0 && lh > 0 && lw <= 512 && lh <= 512 &&
            !IsBadReadPtr(los, (SIZE_T)lw * lh) &&
            !IsBadReadPtr(mapd, (SIZE_T)lw * lh * 2)) {
            int i, n = lw * lh;
            unsigned short wbit = (unsigned short)(1u << watched);
            for (i = 0; i < n; i++) {
                s_losBuf[i] = los[i] ? 255 : 0;
                s_mapBuf[i] = (mapd[i] & wbit) ? 255 : 0;
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            x_glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, s_losTex);
            if (lw != s_losW || lh != s_losH)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, lw, lh, 0, GL_RED, GL_UNSIGNED_BYTE, s_losBuf);
            else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lw, lh, GL_RED, GL_UNSIGNED_BYTE, s_losBuf);
            x_glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, s_mapTex);
            if (lw != s_losW || lh != s_losH)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, lw, lh, 0, GL_RED, GL_UNSIGNED_BYTE, s_mapBuf);
            else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lw, lh, GL_RED, GL_UNSIGNED_BYTE, s_mapBuf);
            s_losW = lw; s_losH = lh;
            x_glActiveTexture(GL_TEXTURE0);
            fogMode = (lostype & 3);
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
    static NU units[512];
    /* sub-pixel motion: the engine keeps 16.16 fixed-point positions (the
       roster shorts are just their high words) — read the true fractions and
       interpolate between the last two sim samples per unit SLOT for
       present-rate smoothness (slot reuse is caught by the distance snap) */
    typedef struct { int x, z, y; int px, pz, py; unsigned tc, tp; } SPX;
    static SPX spx[8192];
    int nu = 0, nv = 0, nwr = 0, nsel = 0;
    unsigned uiGates = *(unsigned char*)(ta + OFF_UIGATES);
    if (s_armed) {                 /* units are gathered only by the unit pass */
    for (char* u = beg + UNIT_STRIDE; u < end && nu < 512; u += UNIT_STRIDE) {
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
        if (ax < vpL - 256 || ax > vpL + vw + 256 || ay < vpT - 256 || ay > vpT + vh + 256)
            continue;
        int owner = *(unsigned char*)(u + U_OWNER);
        int cloaked = (*(unsigned char*)(u + U_CLOAKF) & 4) != 0;
        if (cloaked && owner != watched) continue;    /* enemies never see cloak */
        /* fog gate at the anchor tile: engine draws nothing there */
        if (fogMode & 1) {
            int tx = (wx >> 5), ty = ((wy - wz / 2) >> 5);
            if (tx >= 0 && ty >= 0 && tx < s_losW && ty < s_losH) {
                if (!s_mapBuf[ty * s_losW + tx]) continue;
                if ((fogMode & 2) && owner != watched && !s_losBuf[ty * s_losW + tx]) continue;
            }
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
            int tx0 = (eyeX >> 4) - 8, tx1 = (eyeX >> 4) + (vw >> 4) + 8;
            int ty0 = r0, ty1 = r0 + rows;
            if (tx0 < 0) tx0 = 0;
            if (ty0 < 0) ty0 = 0;
            if (tx1 > mapW) tx1 = mapW;
            if (ty1 > mapH) ty1 = mapH;
            for (int ty = ty0; ty < ty1 && nu < 512; ty++)
            for (int tx = tx0; tx < tx1 && nu < 512; tx++) {
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
                if (ax < vpL - 256 || ax > vpL + vw + 256 ||
                    ay < vpT - 256 || ay > vpT + vh + 256) continue;
                if (fogMode & 1) {          /* unexplored: engine draws nothing */
                    int fx2 = rx >> 5, fy2 = (ry - rz / 2) >> 5;
                    if (fx2 >= 0 && fy2 >= 0 && fx2 < s_losW && fy2 < s_losH &&
                        !s_mapBuf[fy2 * s_losW + fx2]) continue;
                }
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
    /* ---- effects gather (projectiles, explosions, debris) ---- */
    TAGPU_FXVIEW fv;
    int nfx = 0;
    if (fxOn) {
        fv.ta = ta; fv.eyeX = eyeX; fv.eyeY = eyeY;
        fv.vpL = vpL; fv.vpT = vpT;
        fv.gw = gw; fv.gh = gh; fv.ss = s_ss ? 2 : 1; fv.fogMode = fogMode;
        fv.zoom = s_zoom;
        fv.zoomCx = (float)vpL + (float)vw * 0.5f;
        fv.zoomCy = (float)vpT + (float)vh * 0.5f;
        fv.encSprite = fxKey + 3.0f;      /* nearer than fx models (fxKey ± 1.8) */
        fv.depthScale = depthScale;
        fv.los = fogMode ? s_losBuf : NULL; fv.mapd = fogMode ? s_mapBuf : NULL;
        fv.losW = s_losW; fv.losH = s_losH;
        fv.frame_counter = f->frame_counter;
        nfx = tagpu_fx_gather(&fv);
    }
    if (nu == 0 && nfx == 0) return;

    /* ---- build geometry (body); shadow reuses it with an offset ---- */
    static int firstv[513];
    static float encb[512];
    int i;
    for (i = 0; i < nu; i++) {
        firstv[i] = nv;
        /* airborne units draw in a second, un-rowed sweep above everything
           (terrain-depth 3.3) -> the air band above the effects band; wrecks
           sit at FEATURE depth (3+rel*4, terrain-depth 3.4) */
        float encBase = units[i].air ? airKey
                      : (units[i].feat ? 3.0f : 1.0f) + (float)units[i].rel * 4.0f;
        encb[i] = encBase;
        if (units[i].hires)
            nv = emit_hires(units[i].hires, nv, units[i].ax, units[i].ay,
                            units[i].wx0, units[i].wz0, encBase, units[i].yaw);
        else
            nv = emit_geom(units[i].o3, nv, units[i].ax, units[i].ay,
                           units[i].wx0, units[i].wz0, encBase, units[i].owner);
    }
    firstv[nu] = nv;
    /* effects models (missiles, shells, debris) through the same path */
    int fxFirst = nv;
    if (nfx) {
        int k, nm = tagpu_fx_nmodels();
        for (k = 0; k < nm; k++) nv = emit_fx_model(tagpu_fx_model(k), nv, fxKey);
    }
    int fxLast = nv;
    if (nv == 0 && nfx == 0) return;

    /* ---- native selection rects (ui-markers: the ONLY marker interleaved
       with unit draws — the engine's is unreadable under our pixels, redraw
       it): flat model-XZ AABB rect at lowest model Y, rotated by body yaw,
       GUI colour 0xA, drawn as GL_LINES at just-under-the-unit depth ---- */
    int lineStart = nv;
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
            for (k = 0; k < 4; k++) {
                int k2 = (k + 1) & 3, t2;
                for (t2 = 0; t2 < 2; t2++) {
                    float* o = s_verts + nv * NVST;
                    o[0] = t2 ? px[k2] : px[k];
                    o[1] = t2 ? py[k2] : py[k];
                    o[2] = enc;
                    o[3] = -1.0f; o[4] = -1.0f;                 /* flat path  */
                    o[5] = (float)SELBOX_COLIDX / 255.0f; o[6] = -1.0f;
                    o[7] = (float)tagpu_r3d_shade_neutral() / 31.0f;
                    o[8] = units[i].wx0; o[9] = units[i].wz0;
                    o[10] = 1e9f;                               /* never clipped */
                    nv++;
                }
            }
        }
    }

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
    glUniform1i(s_uFog, fogMode);
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
    glBindTexture(GL_TEXTURE_2D, s_losTex);
    x_glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, s_mapTex);
    x_glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nv * NVST * 4, s_verts);

    glEnable(GL_BLEND);
    x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);      /* premultiplied */

    /* selection rects first — the engine draws them under the unit sprite */
    if (nv > lineStart) {
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
            x_glUniform2f(s_uOffset, 5.0f, (float)(units[i].gy - units[i].ay));
            x_glUniform1f(s_uWaterT, units[i].waterT);
            x_glUniform1f(s_uDigT, units[i].digT);
            x_glDrawArrays(GL_TRIANGLES, firstv[i], firstv[i+1] - firstv[i]);
        }
        x_glDepthMask(GL_TRUE);
    }
    /* bodies */
    glUniform1i(s_uShadow, 0);
    x_glUniform2f(s_uOffset, 0.0f, 0.0f);
    for (i = 0; i < nu; i++) {
        x_glUniform1f(s_uAlpha, units[i].cloaked ? 0.5f : 1.0f);
        x_glUniform1f(s_uWaterT, units[i].waterT);
        x_glUniform1f(s_uDigT, units[i].digT);
        glUniform1i(s_uWaterMode, units[i].waterMode);
        x_glDrawArrays(GL_TRIANGLES, firstv[i], firstv[i+1] - firstv[i]);
    }
    /* effects: models in the unit pipeline, then lines/sprites (own program;
       depth test still on so aircraft cover them, depth writes off) */
    if (fxLast > fxFirst) {
        x_glUniform1f(s_uAlpha, 1.0f);
        x_glUniform1f(s_uWaterT, -1e9f);
        x_glUniform1f(s_uDigT, -1e9f);
        glUniform1i(s_uWaterMode, 0);
        x_glDrawArrays(GL_TRIANGLES, fxFirst, fxLast - fxFirst);
    }
    if (nfx) tagpu_fx_render(&fv, s_palTex, s_losTex, s_mapTex);
    x_glDisable(GL_BLEND);
    x_glDisable(GL_DEPTH_TEST);
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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* ---- composite over the frame (restore the letterbox viewport) ---- */
    glViewport(f->vp_x, f->vp_y, f->vp_w, f->vp_h);
    glUseProgram(s_cprog);
    glBindVertexArray(s_cvao);
    glEnable(GL_BLEND);
    x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);      /* premultiplied */
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
                  "native: %d unit(s) %d wreck(s) %d sel %d verts fbo=%dx%d ss=%d subpix=%d scaf=%d fog=%d%s",
                  nu - nwr, nwr, nsel, nv, gw, gh, ss, s_subpix, scafOn, fogMode,
                  s_vtrunc ? " VERTEX-BUDGET-HIT" : "");
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
    s_losW = s_losH = 0;
    tagpu_fx_glreset();
}

int tagpu_native_wrecks_armed(void)
{
    return s_armed > 0 && s_state != 2 && s_wrecks;
}
