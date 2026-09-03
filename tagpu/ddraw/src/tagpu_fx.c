/* tagpu_fx.c — the effects pass: weapon fire, explosions, debris.

   The engine draws these in two colour-only passes between the ground unit
   sweep and the airborne sweep (DrawGameScreen call sites 0x469B22 and
   0x469B2C; research/notes/effects.md has the decompiled rules):

     0x49BE60  projectiles  — walks the ProjectileStruct array (count
               main+0x141F3, base *(main+0x141F7), stride 0x6B). Per record,
               LOS-gated at the anchor tile, then by WeaponStruct.RenderType
               (+0x10C):
                 0 laser        DrawLine start->head, colour main+0xDCB[color],
                                two lines when color2 (+0x10E) != 0
                 1 model        shadow blob + 3DO root (+0x74) rotated by
                                (t0, t1-0x8000, t2-0x8000); the root's child
                                (thrust flame) while tick < +0x46, spinning by
                                +0x64 when WeaponTypeMask bit 21
                 2 ball         background-refraction sprite (main+0x1AB9B)
                                — NOT reproduced (counted only)
                 3 model        shadow blob + root, no rotation
                 4 sprite       shadow blob + anim set main+0x147BB[color]
                                (color 0..4), frame (tick-spawn) % n, opaque
                 5 flare        main+0x147F3, frame by remaining life, alpha
                 6 model        shadow blob + root rotated by the raw triple
                 7 lightning    two jagged polylines start->head, +-5 jitter
     0x420B00  explosions   — flying debris pieces (particle slots
               0x511DF0..0x511F80 -> +0x2C piece {node, turn@0x12, pos@0x16})
               then, over the ExplosionStruct array (count main+0x1491B,
               inline at main+0x1491F, stride 0x54; anchor must be inside the
               viewport rect): the LHT "flash" (anim state +0x10, table
               TAProgram+0xC8) for all, then per record the debris node (+0)
               rotated by +0x4C and the opaque sprite (anim state +0x04).

   Here: models are handed to the native pass (same face/atlas/palette path,
   unshaded like the engine's GAF_DrawTransformed); lines and sprites are
   rendered by this module into the native FBO right after the unit bodies,
   at a depth band above every ground row and below the airborne band. GAF
   frames (raw or TA-RLE, sub-frame lists) are decoded into a private atlas.
   ALP alpha = 50% blend; the LHT flash = additive, per-level colour derived
   from the live table. Armed by tagpu_fx.on; tokens: log, nolines, nomodels,
   nosprites, noexpl, nodebris, passive (gather + log, engine draws).
   The particle sfx pass (tagpu_sfx.c, armed by tagpu_sfx.on) rides the same
   buckets and program: layers 0..6 are emitted before the projectiles so the
   engine's order (smoke under weapon sprites) survives, layers 7..9 after
   the explosions; each sprite carries its own depth key. Read-only over sim. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_fx.h"
#include "tagpu_fxown.h"
#include "tagpu_sfx.h"
#include "tagpu_glsl.h"
#include "tagpu_gaf.h"

#define TA_MAINPP     0x00511DE8u
#define TAPROG_PP     0x0051FBD0u
#define OFF_TICK      0x38A47
#define OFF_NPROJ     0x141F3
#define OFF_PROJ      0x141F7
#define PROJ_STRIDE   0x6B
#define OFF_NEXPL     0x1491B
#define OFF_EXPL      0x1491F
#define EXPL_STRIDE   0x54
#define OFF_COLTAB    0x0DCB    /* u8[]: weapon colour number -> palette idx */
#define OFF_SHADOWSEQ 0x1480F   /* projectile ground-shadow blob sequence    */
#define OFF_SPRSEQ0   0x147BB   /* 5 sprite-weapon sequences (rendertype 4)  */
#define OFF_FLARESEQ  0x147F3   /* rendertype 5 sequence                     */
#define OFF_VPRECT    0x37E27   /* int l,t,r,b (frame px)                    */
#define PSYS_BEGIN    0x00511DF0u
#define PSYS_END      0x00511F80u
#define PROG_LHT      0xC8      /* u8[32*256] lighten table                  */
#define PROG_CAPS     0xF0      /* u16: bit5 ALP built, bit7 LHT built       */

#define P_WEAPON   0x00
#define P_X        0x04         /* i32 16.16 world x                         */
#define P_ALT      0x08         /* i32 16.16 altitude                        */
#define P_Y        0x0C         /* i32 16.16 map depth                       */
#define P_XS       0x10         /* start (tail) position, same layout        */
#define P_ALTS     0x14
#define P_YS       0x18
#define P_TURN     0x34         /* short[3] rotation triple                  */
#define P_SPAWN    0x42         /* int tick                                  */
#define P_DEATH    0x46         /* int tick                                  */
#define P_ATTACKER 0x52         /* UnitStruct*                               */
#define P_GROUNDH  0x5E         /* u16 terrain height under the projectile   */
#define P_HIDDEN   0x60         /* short; draw only when 0                   */
#define P_SPIN     0x64         /* short                                     */
#define W_NAME     0x00
#define W_MODEL    0x74         /* Model3DONode*                             */
#define W_LIFE     0xE6         /* u16                                       */
#define W_RT       0x10C        /* i8 RenderType                             */
#define W_COLOR    0x10D
#define W_COLOR2   0x10E
#define W_MASK     0x111        /* u32 WeaponTypeMask                        */
#define E_NODE     0x00
#define E_ST1      0x04         /* anim state: u16 frame @0, seq* @8         */
#define E_ST2      0x10
#define E_X        0x1C
#define E_ALT      0x20
#define E_Y        0x24
#define E_TURN     0x4C
#define D_NODE     0x00         /* debris piece (particle slot +0x2C)        */
#define D_TURN     0x12
#define D_X        0x16
#define D_ALT      0x1A
#define D_Y        0x1E
#define U_OWNER    0xFF

#define MODE_FLAT   TAGPU_FXMODE_FLAT
#define MODE_OPAQUE TAGPU_FXMODE_OPAQUE
#define MODE_ALPHA  TAGPU_FXMODE_ALPHA
#define MODE_FLASH  TAGPU_FXMODE_FLASH

#define MAXFXV   32768          /* vertices per bucket per frame             */
#define FXST     9              /* x,y,enc, u,v, c,mode, wx,wz               */
#define MAXMODEL 1024
#define ATLAS_DIM 2048
#define ATLAS_MAX 2048

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }
/* the TAProgram block (*0x51FBD0) lives in the exe's own data segment,
   below the heap floor ptr_ok assumes */
static int prog_ok(const void* p)
{ return (size_t)p > 0x400000u && (size_t)p < 0x7FFF0000u && !IsBadReadPtr(p, 0x100); }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_BLENDFUNC)(GLenum,GLenum);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_DEPTHMASK)(GLboolean);
typedef void (APIENTRY *PFN_LINEWIDTH)(GLfloat);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_BLENDFUNC  x_glBlendFunc;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_DEPTHMASK  x_glDepthMask;
static PFN_LINEWIDTH  x_glLineWidth;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

/* ---- arming ---- */
static int  s_armed = -1;
static int  s_log = 0, s_lines = 1, s_models = 1, s_sprites = 1, s_expl = 1, s_debris = 1;
static int  s_passive = 0;             /* gather + log only; engine keeps drawing */
static unsigned s_armCheck = 0;

static void read_arm(unsigned frame_counter)
{
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return;
    s_armCheck = frame_counter;
    int was = s_armed;
    s_armed = 0;
    HANDLE h = CreateFileA("tagpu_fx.on", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    s_log = 0; s_lines = s_models = s_sprites = s_expl = s_debris = 1; s_passive = 0;
    if (h == INVALID_HANDLE_VALUE) {
        tagpu_fxown_set_skip(0);
        if (was > 0) flog("fx: disarmed");
        return;
    }
    char buf[128]; DWORD n = 0;
    if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
        buf[n] = 0;
        char* p = buf;
        while (*p) {
            while (*p && *p <= ' ') p++;
            char* q = p;
            while (*q && *q > ' ') q++;
            int last = (*q == 0);
            *q = 0;
            if (!lstrcmpiA(p, "log")) s_log = 1;
            else if (!lstrcmpiA(p, "nolines")) s_lines = 0;
            else if (!lstrcmpiA(p, "nomodels")) s_models = 0;
            else if (!lstrcmpiA(p, "nosprites")) s_sprites = 0;
            else if (!lstrcmpiA(p, "noexpl")) s_expl = 0;
            else if (!lstrcmpiA(p, "nodebris")) s_debris = 0;
            else if (!lstrcmpiA(p, "passive")) s_passive = 1;
            if (last) break;
            p = q + 1;
        }
    }
    CloseHandle(h);
    s_armed = 1;
    /* the engine skip is armed by a successful gather (below), never by the
       file alone; passive turns it off here */
    if (s_passive) tagpu_fxown_set_skip(0);
    if (was != 1) {
        char b[128];
        _snprintf(b, sizeof b, "fx: ARMED (lines=%d models=%d sprites=%d expl=%d debris=%d log=%d passive=%d)",
                  s_lines, s_models, s_sprites, s_expl, s_debris, s_log, s_passive);
        flog(b);
    }
}

int tagpu_fx_armed(unsigned frame_counter)
{
    read_arm(frame_counter);
    return s_armed > 0;
}

/* ---- GL ---- */
static int    s_state = 0;              /* 0 unloaded, 1 ready, 2 failed     */
static GLuint s_prog, s_vao, s_vbo, s_lhtTex;
static GLint  s_uGame, s_uFog, s_uFogOrg, s_uFogDim, s_uZoom, s_uZoomC, s_uDepthScale;
static GLint  s_uScafOn, s_uScafP, s_uSS, s_uZoomF, s_uZoomCF;
/* four buckets, drawn in this order: the particle layers the engine draws
   BEFORE its projectile pass (0..6: wake foam, feature smoke, trail puffs,
   nanolathe), lines, flashes (additive), sprites (weapon sprites, explosions,
   then particle layers 7..9) — so lasers and flashes sit over trail smoke and
   explosion sprites over their flash like the engine; no per-run segment
   table, so nothing is ever dropped for alternating too often */
enum { B_UNDER = 0, B_LINES = 1, B_FLASH = 2, B_SPRITES = 3, NBUCKET = 4 };
static float  s_verts[NBUCKET][MAXFXV * FXST];
static int    s_nv[NBUCKET];
static float  s_encCur = 403.0f;       /* depth key of what is being emitted  */
static int    s_under = 0;             /* emit sprites/dots into B_UNDER      */
static int    s_mute = 0;              /* passive: count, emit nothing        */
static TAGPU_FXMODEL s_models_[MAXMODEL];
static int    s_nm = 0;

/* the shared shelf atlas (tagpu_gaf.c). Its entries are keyed on the frame
   header AND its pixel pointer/dims: effect sequences (the flash) are freed
   when their explosion ends and the address is reused for other frames */
static TAGPU_GAFENT   s_atlasEnts[ATLAS_MAX];
static TAGPU_GAFATLAS s_atlas;
static int s_lhtInit = 0;
static unsigned s_lhtStamp = 0;

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aCM;\n"       /* colour-or-ck /255, mode      */
    "layout(location=3) in vec2 aWorld;\n"
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"                  /* same view transform as units */
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"            /* same depth encoding as units */
    "out vec2 vUV; flat out vec2 vCM; out vec2 vWorld; out float vEnc;\n"
    "void main(){\n"
    "  vec2 p = (aPos.xy - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vCM = aCM; vWorld = aWorld; vEnc = aPos.z;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec2 vCM; in vec2 vWorld; in float vEnc;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uPal;\n"
    "uniform sampler2D uLht;\n"              /* 32x1 RGB additive per level  */
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_SCAF_UNIFORMS
    TAGPU_GLSL_FOG_FN
    "void main(){\n"
    "  int mode = int(vCM.y + 0.5);\n"
    "  vec3 rgb; float a = 1.0;\n"
    /* scaffold occlusion, the unit shader's rule: only the B_UNDER draw
       (particle layers below every row key) turns uScafOn on */
    TAGPU_GLSL_SCAF_TEST
    /* effects are transient: the engine's own passes are LOS-gated, so they
       vanish in grey rather than darkening (uFog bit1 is set for this pass) */
    TAGPU_GLSL_FOG_DISCARD
    "  if (mode == 0) {\n"
    "    rgb = texelFetch(uPal, ivec2(int(vCM.x*255.0+0.5), 0), 0).rgb;\n"
    "  } else {\n"
    "    float idx = texture(uAtlas, vUV).r;\n"
    "    if (abs(idx - vCM.x) < 0.5/255.0) discard;\n"
    "    int ii = int(idx*255.0+0.5);\n"
    "    if (mode == 3) {\n"
    "      int lv = clamp(ii - 79, 0, 31);\n"
    "      rgb = texelFetch(uLht, ivec2(lv, 0), 0).rgb;\n"
    "    } else {\n"
    "      rgb = texelFetch(uPal, ivec2(ii, 0), 0).rgb;\n"
    "      if (mode == 2) a = 0.5;\n"
    "    }\n"
    "  }\n"
    /* premultiplied FBO: flashes are pure additive light (alpha 0) */
    "  if (mode == 3) frag = vec4(rgb, 0.0); else frag = vec4(rgb * a, a);\n"
    "}\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
               flog("fx: shader FAILED:"); flog(lg); s_state = 2; }
    return sh;
}

static void init_gl(void)
{
    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glBlendFunc  = (PFN_BLENDFUNC) getgl("glBlendFunc");
    x_glUniform1f  = (PFN_UNIFORM1F) getgl("glUniform1f");
    x_glUniform2f  = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glDepthMask  = (PFN_DEPTHMASK) getgl("glDepthMask");
    x_glLineWidth  = (PFN_LINEWIDTH) getgl("glLineWidth");
    if (!x_glDrawArrays || !x_glBlendFunc || !x_glUniform1f || !x_glUniform2f ||
        !x_glActiveTexture || !x_glDepthMask) { flog("fx: missing GL proc"); s_state = 2; return; }

    GLuint vs = mksh(GL_VERTEX_SHADER, VS), fs = mksh(GL_FRAGMENT_SHADER, FS);
    if (s_state == 2) return;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs); glLinkProgram(s_prog);
    GLint ok = 0; glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { flog("fx: link FAILED"); s_state = 2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uGame = glGetUniformLocation(s_prog, "uGame");
    s_uFog  = glGetUniformLocation(s_prog, "uFog");
    s_uFogOrg = glGetUniformLocation(s_prog, "uFogOrg");
    s_uFogDim = glGetUniformLocation(s_prog, "uFogDim");
    s_uZoom = glGetUniformLocation(s_prog, "uZoom");
    s_uZoomC = glGetUniformLocation(s_prog, "uZoomC");
    s_uDepthScale = glGetUniformLocation(s_prog, "uDepthScale");
    s_uScafOn = glGetUniformLocation(s_prog, "uScafOn");
    s_uScafP  = glGetUniformLocation(s_prog, "uScafP");
    s_uSS     = glGetUniformLocation(s_prog, "uSS");
    s_uZoomF  = glGetUniformLocation(s_prog, "uZoomF");
    s_uZoomCF = glGetUniformLocation(s_prog, "uZoomCF");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "uPal"),   1);
    glUniform1i(glGetUniformLocation(s_prog, "uFogGrid"), 2);
    glUniform1i(glGetUniformLocation(s_prog, "uFogLUT"),  3);   /* unused here:
        effects hide in grey rather than shading, but binding it keeps a future
        FOG_SHADE in this pass off unit 0 (the atlas) */
    glUniform1i(glGetUniformLocation(s_prog, "uLht"),   4);
    glUniform1i(glGetUniformLocation(s_prog, "uScaf"),  5);
    glUseProgram(0);

    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo); glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, FXST * 4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, FXST * 4, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, FXST * 4, (void*)20);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, FXST * 4, (void*)28);
    glBindVertexArray(0);

    tagpu_gaf_atlas_lost(&s_atlas);      /* its texture is made on first use */
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "fx";
    tagpu_gaf_atlas_create(&s_atlas);   /* never bind texture 0 to uAtlas */
    glGenTextures(1, &s_lhtTex);
    glBindTexture(GL_TEXTURE_2D, s_lhtTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_lhtInit = 0;
    s_state = 1;
    flog("fx: GL ready");
}

void tagpu_fx_glreset(void)
{
    s_state = 0; s_lhtInit = 0;
    tagpu_gaf_atlas_lost(&s_atlas);
}

/* ---- emission ---- */
static void put_vert(int b, float x, float y, float u, float v, float c, int mode, float wx, float wz)
{
    float* o = s_verts[b] + (size_t)s_nv[b] * FXST;
    o[0] = x; o[1] = y; o[2] = s_encCur; o[3] = u; o[4] = v;
    o[5] = c; o[6] = (float)mode; o[7] = wx; o[8] = wz;
    s_nv[b]++;
}

static int s_cLines = 0, s_cSprites = 0, s_cFlash = 0, s_cAtlasFail = 0;
static int s_cOverflow = 0, s_cQuads = 0;
static int s_traceN = 0;              /* emission trace lines left (sfx log) */
void tagpu_fx_trace(int n) { s_traceN = n; }

static void put_quad(int b, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, float c, int mode,
                     float wx, float wz)
{
    if (s_nv[b] + 6 > MAXFXV) { s_cOverflow++; return; }
    put_vert(b, x0, y0, u0, v0, c, mode, wx, wz);
    put_vert(b, x1, y0, u1, v0, c, mode, wx, wz);
    put_vert(b, x0, y1, u0, v1, c, mode, wx, wz);
    put_vert(b, x1, y0, u1, v0, c, mode, wx, wz);
    put_vert(b, x1, y1, u1, v1, c, mode, wx, wz);
    put_vert(b, x0, y1, u0, v1, c, mode, wx, wz);
    s_cQuads++;
}

static void emit_line(int x0, int y0, int x1, int y1, int colidx, float wx, float wz)
{
    if (!s_lines) return;
    s_cLines++;
    if (s_mute) return;
    if (s_nv[B_LINES] + 2 > MAXFXV) { s_cOverflow++; return; }
    float c = (float)colidx / 255.0f;
    /* pixel centres: the engine's Bresenham paints the cells at both ends */
    put_vert(B_LINES, (float)x0 + 0.5f, (float)y0 + 0.5f, -1, -1, c, MODE_FLAT, wx, wz);
    put_vert(B_LINES, (float)x1 + 0.5f, (float)y1 + 0.5f, -1, -1, c, MODE_FLAT, wx, wz);
}

/* the fx pass's own `nosprites` token lives at ITS call sites (fx_sprite),
   not here: the particle pass emits through this path too */
static void emit_sprite(const unsigned char* g, int sx, int sy, int mode, float wx, float wz, int depth)
{
    if (depth > 4) return;
    g = tagpu_gaf_frame_sane(g);
    if (!g) return;
    int sub = g[TAGPU_GF_SUBN];
    if (sub) {
        const unsigned char* const* arr = *(const unsigned char* const* const*)(g + TAGPU_GF_PIX);
        if (!ptr_ok(arr) || IsBadReadPtr(arr, (SIZE_T)sub * 4)) return;
        int k;
        for (k = 0; k < sub; k++) {
            const unsigned char* sg = tagpu_gaf_frame_sane(arr[k]);
            if (!sg) continue;
            int m = mode;
            if (mode == MODE_OPAQUE && sg[TAGPU_GF_SUBALP]) m = MODE_ALPHA;
            emit_sprite(sg, sx, sy, m, wx, wz, depth + 1);
        }
        return;
    }
    int b = (mode == MODE_FLASH) ? B_FLASH : (s_under ? B_UNDER : B_SPRITES);
    if (mode == MODE_FLASH) s_cFlash++; else s_cSprites++;
    if (s_mute) return;
    const TAGPU_GAFENT* e = tagpu_gaf_atlas_get(&s_atlas, g);
    if (!e) { s_cAtlasFail++; return; }
    int w = *(const unsigned short*)(g + TAGPU_GF_W), h = *(const unsigned short*)(g + TAGPU_GF_H);
    float x0 = (float)(sx - *(const short*)(g + TAGPU_GF_HOTX));
    float y0 = (float)(sy - *(const short*)(g + TAGPU_GF_HOTY));
    float x1 = x0 + (float)w, y1 = y0 + (float)h;
    float c = (float)e->ck / 255.0f;
    if (s_traceN > 0) {
        char tb[160]; s_traceN--;
        _snprintf(tb, sizeof tb, "fx: emit b=%d mode=%d at=(%.0f,%.0f) %dx%d enc=%.1f ck=%u uv=(%.3f,%.3f) nv=%d",
                  b, mode, x0, y0, w, h, s_encCur, (unsigned)e->ck, e->u0, e->v0, s_nv[b]);
        flog(tb);
    }
    put_quad(b, x0, y0, x1, y1, e->u0, e->v0, e->u1, e->v1, c, mode, wx, wz);
}

/* the fx pass's sprites honour its own nosprites token */
static void fx_sprite(const unsigned char* g, int sx, int sy, int mode, float wx, float wz)
{
    if (s_sprites) emit_sprite(g, sx, sy, mode, wx, wz, 0);
}

/* ---- the particle pass emits through the same buckets, at its own key;
   under = the engine draws this layer before its projectile pass ---- */
int tagpu_fx_emit_seq_frame(const char* seq, int frame, int sx, int sy, int mode,
                            float wx, float wz, float enc, int under)
{
    const unsigned char* g = tagpu_gaf_seq_frame(seq, frame);
    if (!g) return 0;
    float keep = s_encCur; int keepU = s_under, before = s_cQuads;
    s_encCur = enc; s_under = under;
    emit_sprite(g, sx, sy, mode, wx, wz, 0);
    s_encCur = keep; s_under = keepU;
    return s_cQuads > before;
}

/* DrawBar 0x4BF6F0 of the rect (x, y, x+1, y+1): a 2x2 flat dot */
int tagpu_fx_emit_dot(int x, int y, int colidx, float wx, float wz, float enc, int under)
{
    if (s_mute) return 0;
    float keep = s_encCur; int before = s_cQuads;
    float x0 = (float)x, y0 = (float)y;
    s_encCur = enc;
    put_quad(under ? B_UNDER : B_SPRITES, x0, y0, x0 + 2.0f, y0 + 2.0f,
             -1, -1, -1, -1, (float)colidx / 255.0f, MODE_FLAT, wx, wz);
    s_encCur = keep;
    return s_cQuads > before;
}

void tagpu_fx_set_mute(int on) { s_mute = on; }

/* TAProgram capability bits (+0xF0): bit5 ALP alpha table built, bit7 LHT */
unsigned tagpu_fx_caps(void)
{
    const char* prog = *(const char* const*)TAPROG_PP;
    return prog_ok(prog) ? *(const unsigned short*)(prog + PROG_CAPS) : 0u;
}

static void emit_model(const char* node, float ax, float ay, float wx, float wz,
                       short t0, short t1, short t2, int owner)
{
    if (!s_models || s_mute || s_nm >= MAXMODEL) return;
    if (!ptr_ok(node) || IsBadReadPtr(node, 0x40)) return;
    TAGPU_FXMODEL* m = &s_models_[s_nm++];
    m->node = node; m->ax = ax; m->ay = ay; m->wx = wx; m->wz = wz;
    m->turn[0] = t0; m->turn[1] = t1; m->turn[2] = t2; m->owner = owner;
}

/* the shaders' fog rule (tagpu_glsl.h) on the CPU — bilinear coverage over the
   grid's four corner bits, thresholded at 0.5 */
static float fog_cov(unsigned m, float fx, float fy)
{
    float tl = (float)( m       & 1u), tr = (float)((m >> 1) & 1u);
    float bl = (float)((m >> 2) & 1u), br = (float)((m >> 3) & 1u);
    float top = tl + (tr - tl) * fx, bot = bl + (br - bl) * fx;
    return top + (bot - top) * fy;
}

/* ---- the fog-grid guard -----------------------------------------------------

   `grid` is the ENGINE's own screen fog buffer, read raw out of its struct once
   a frame (tagpu_native.c) and indexed here, later, on the render thread. The
   only test either caller ever made was `!grid`, which a NON-NULL garbage value
   walks straight through — and on 2026-09-03 one did: a hard read fault at
   `tagpu_fog_at+0x10c` off a base of -9 (and, in an earlier instance, -318),
   from tagpu_sfx_gather, which killed the render thread and left the process up
   and the game frozen. Reproduced with the file zoom lever and NO wheel input,
   so it is not the wheel; what it wants is a zoomed-out view, live effects and
   the camera moving, which is when the engine rebuilds this buffer under us.

   THE ROOT CAUSE IS NOT FOUND YET. This turns the crash into a dropped fog
   sample so the session survives to be examined, and — because a log line in a
   98 MB file is invisible while you are playing — says so on screen, ONCE, from
   a thread of its own so the renderer never blocks on the dialog. */

static volatile LONG s_fogAlarmed;
static char s_fogAlarm[256];

static DWORD WINAPI fog_alarm_thread(LPVOID p)
{
    (void)p;
    MessageBoxA(NULL, s_fogAlarm, "tagpu: fog grid guard tripped",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    return 0;
}

static void fog_alarm(const char* why, const unsigned short* grid, int cols,
                      int rows, int orgX, int orgY, int wx, int wzp)
{
    FILE* f;
    if (InterlockedCompareExchange(&s_fogAlarmed, 1, 0) != 0) return;
    _snprintf(s_fogAlarm, sizeof s_fogAlarm,
              "The fog-grid guard caught a bad read and dropped it.\n\n"
              "%s\ngrid=%p cols=%d rows=%d org=(%d,%d) world=(%d,%d)\n\n"
              "Without the guard this is the crash that freezes the renderer.\n"
              "The game is still running. Please tell Claude, and keep the log.",
              why, (const void*)grid, cols, rows, orgX, orgY, wx, wzp);
    s_fogAlarm[sizeof s_fogAlarm - 1] = 0;
    f = fopen("tagpu.log", "a");
    if (f) {
        fprintf(f, "FOGGUARD %s grid=%p cols=%d rows=%d org=(%d,%d) world=(%d,%d)\n",
                why, (const void*)grid, cols, rows, orgX, orgY, wx, wzp);
        fclose(f);
    }
    /* never on the render thread: a modal dialog there stops the frame loop and
       we would be diagnosing our own hang instead of the engine's grid */
    {
        HANDLE h = CreateThread(NULL, 0, fog_alarm_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
}

int tagpu_fog_at(const unsigned short* grid, int cols, int rows,
                 int orgX, int orgY, int wx, int wzp)
{
    if (!grid || cols <= 0 || rows <= 0) return 0;
    /* the same bounds tagpu_native.c validates the pointer with when it reads it
       out of the engine struct — if it no longer holds, the buffer moved */
    if ((size_t)grid <= 0x600000u || (size_t)grid >= 0x7FFF0000u) {
        fog_alarm("grid pointer is not in engine address space",
                  grid, cols, rows, orgX, orgY, wx, wzp);
        return 0;
    }
    /* and the dims: the producer refuses anything over 256 a side, so a larger
       one here means cols/rows and the buffer have come apart */
    if (cols > 256 || rows > 256) {
        fog_alarm("grid dims exceed the 256 the producer accepts",
                  grid, cols, rows, orgX, orgY, wx, wzp);
        return 0;
    }
    float gx = (float)(wx  - orgX) * (1.0f / 32.0f);
    float gy = (float)(wzp - orgY) * (1.0f / 32.0f);
    /* The grid only spans the VIEW, while the gather accepts anchors up to
       256 px outside it. Off-grid means "off-screen", not "off-map", so this
       reports no fog and leaves the caller's own viewport cull to decide —
       clamping to the border cell instead would cull anything whose anchor
       sits past the edge over dark ground, popping sprites in as you scroll.
       The shaders clamp, which is exact: an on-screen fragment is in range. */
    if (gx < 0.0f || gy < 0.0f ||
        gx >= (float)cols || gy >= (float)rows) return 0;
    int cx = (int)gx, cy = (int)gy;
    unsigned e = grid[cy * cols + cx];
    int r = 0;
    if (fog_cov(e        & 0xFu, gx - (float)cx, gy - (float)cy) >= 0.5f) r |= 1;
    if (fog_cov((e >> 8) & 0xFu, gx - (float)cx, gy - (float)cy) >= 0.5f) r |= 2;
    return r;
}

/* engine LOS gate for a projectile anchor tile (0x49BE60 head); the particle
   leaves (tagpu_sfx.c) and the feature gate 0x4658E0 (tagpu_feat.c) run the
   same test. BOTH fog bands hide: the engine draws no effect it cannot
   currently see, and 0x4658E0 likewise tests the LOS counter when LosType&2
   and MAPPED otherwise — which is exactly the pairing the grid encodes, since
   the builder only writes the grey mask in true-LOS mode. */
int tagpu_fx_tile_visible(const TAGPU_FXVIEW* v, int wx, int wzp)
{
    if (!(v->fogMode & 1) || !v->fogGrid) return 1;
    return tagpu_fog_at(v->fogGrid, v->fogCols, v->fogRows,
                        v->fogOrgX, v->fogOrgY, wx, wzp) == 0;
}

static int in_vprect(const char* ta, int sx, int sy)
{
    const int* r = (const int*)(ta + OFF_VPRECT);
    return sx >= r[0] && sx <= r[2] && sy >= r[1] && sy <= r[3];
}

static unsigned s_rng = 0x12345u;
static int jitter5(void)            /* rand()*11/0x8000 - 5, visual only */
{
    s_rng = s_rng * 0x343FDu + 0x269EC3u;
    return (int)(((s_rng >> 16) & 0x7FFF) * 11 / 0x8000) - 5;
}

typedef struct { int hidden, fogged, laser, model, ball, sprite, flare, light, expl, debris, flash, other; } FXC;
static FXC s_c;

/* weapon fire, explosions, debris: the two engine passes, gathered */
static void gather_fx(const TAGPU_FXVIEW* v)
{
    s_mute = s_passive;                /* passive: count + log, emit nothing */
    /* we are drawing this frame: the engine may skip its own effects draw */
    if (!s_passive) tagpu_fxown_set_skip(1);
    tagpu_fxown_beat(v->frame_counter);

    const char* ta = v->ta;
    int tick = *(const int*)(ta + OFF_TICK);
    const unsigned char* coltab = (const unsigned char*)(ta + OFF_COLTAB);
    unsigned caps = tagpu_fx_caps();
    int alphaOn = (caps & 0x20) != 0, flashOn = (caps & 0x80) != 0;
    int eyeX = v->eyeX, eyeY = v->eyeY, vpL = v->vpL, vpT = v->vpT;
    char lb[200];

    /* ---- projectiles (0x49BE60) ---- */
    int np = *(const int*)(ta + OFF_NPROJ);
    const char* pbase = *(const char* const*)(ta + OFF_PROJ);
    if (np > 0 && np <= 8192 && ptr_ok(pbase) && !IsBadReadPtr(pbase, (SIZE_T)np * PROJ_STRIDE)) {
        const unsigned char* shadowFrame = tagpu_gaf_seq_frame(*(const char* const*)(ta + OFF_SHADOWSEQ), 0);
        int i;
        for (i = 0; i < np; i++) {
            const char* p = pbase + (size_t)i * PROJ_STRIDE;
            if (*(const short*)(p + P_HIDDEN) != 0) { s_c.hidden++; continue; }
            int X = *(const int*)(p + P_X), ALT = *(const int*)(p + P_ALT), Y = *(const int*)(p + P_Y);
            int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
            int hzp = hy - (halt >> 1);
            if (!tagpu_fx_tile_visible(v, hx, hzp)) { s_c.fogged++; continue; }
            const char* w = *(const char* const*)(p + P_WEAPON);
            if (!ptr_ok(w) || IsBadReadPtr(w, 0x115)) { s_c.other++; continue; }
            int rt = *(const signed char*)(w + W_RT);
            int color = *(const unsigned char*)(w + W_COLOR);
            int sx = hx - eyeX + vpL, sy = (hy - eyeY) - (halt >> 1) + vpT;
            float ax = (float)X / 65536.0f - (float)eyeX + (float)vpL;
            float ay = (float)Y / 65536.0f - (float)ALT / 65536.0f * 0.5f - (float)eyeY + (float)vpT;
            float wx = (float)hx, wz = (float)hzp;
            int owner = 0;
            {
                const char* au = *(const char* const*)(p + P_ATTACKER);
                if (ptr_ok(au) && !IsBadReadPtr(au, 0x118)) owner = *(const unsigned char*)(au + U_OWNER);
            }
            if (s_log && i < 8 && (v->frame_counter % 60) == 0) {
                _snprintf(lb, sizeof lb,
                    "fx: p%d \"%.20s\" rt=%d col=%d/%d pos=(%d,%d,%d) start=(%d,%d,%d) scr=(%d,%d) turn=(%d,%d,%d) spawn=%d death=%d tick=%d gh=%u",
                    i, w + W_NAME, rt, color, *(const unsigned char*)(w + W_COLOR2), hx, halt, hy,
                    *(const int*)(p + P_XS) >> 16, *(const int*)(p + P_ALTS) >> 16, *(const int*)(p + P_YS) >> 16,
                    sx, sy, *(const short*)(p + P_TURN), *(const short*)(p + P_TURN + 2), *(const short*)(p + P_TURN + 4),
                    *(const int*)(p + P_SPAWN), *(const int*)(p + P_DEATH), tick,
                    (unsigned)*(const unsigned short*)(p + P_GROUNDH));
                flog(lb);
            }
            /* ground shadow blob (rendertypes 1,3,4,6): alpha blit of the
               shadow sequence's frame 0 at the projectile's ground point */
            #define SHADOW_BLOB() do { if (alphaOn && shadowFrame) { \
                int gh = *(const unsigned short*)(p + P_GROUNDH); \
                fx_sprite(shadowFrame, sx, (hy - eyeY) - (gh >> 1) + vpT, MODE_ALPHA, wx, wz); } } while (0)
            const short* tr = (const short*)(p + P_TURN);
            switch (rt) {
            case 0: {
                s_c.laser++;
                int c1 = coltab[color];
                int x0 = sx, y0 = sy;
                int x1 = (*(const int*)(p + P_XS) >> 16) - eyeX + vpL;
                int y1 = ((*(const int*)(p + P_YS) >> 16) - ((*(const int*)(p + P_ALTS) >> 16) >> 1)) - eyeY + vpT;
                int color2 = *(const unsigned char*)(w + W_COLOR2);
                if (color2 == 0) emit_line(x0, y0, x1, y1, c1, wx, wz);
                else {
                    /* engine: a second line one pixel beside the first, in
                       colour2, drawn first (0x49BE60 case 0) */
                    int ax0 = x0, ay0 = y0, bx = x1, by = y1;
                    int sx2, sy2, ex2, ey2;
                    if (abs(y0 - y1) < abs(x0 - x1)) {
                        if (x1 < x0) { ax0 = x1; bx = x0; ay0 = y1; by = y0; }
                        sx2 = ax0; sy2 = ay0 - 1; ex2 = bx; ey2 = by - 1;
                    } else {
                        if (y1 < y0) { ax0 = x1; bx = x0; ay0 = y1; by = y0; }
                        sx2 = ax0 - 1; sy2 = ay0; ex2 = bx + 1; ey2 = by;
                        /* the engine's variant shifts start x-1 / end x+1 */
                    }
                    emit_line(sx2, sy2, ex2, ey2, coltab[color2], wx, wz);
                    emit_line(ax0, ay0, bx, by, c1, wx, wz);
                }
                break;
            }
            case 1: case 3: case 6: {
                s_c.model++;
                SHADOW_BLOB();
                const char* node = *(const char* const*)(w + W_MODEL);
                short t0 = 0, t1 = 0, t2 = 0;
                if (rt == 1) { t0 = tr[0]; t1 = (short)(tr[1] - 0x8000); t2 = (short)(tr[2] - 0x8000); }
                else if (rt == 6) { t0 = tr[0]; t1 = tr[1]; t2 = tr[2]; }
                emit_model(node, ax, ay, wx, wz, t0, t1, t2, owner);
                if (rt == 1 && ptr_ok(node) && !IsBadReadPtr(node, 0x40)) {
                    const char* child = *(const char* const*)(node + 0x30);
                    if (ptr_ok(child) && tick < *(const int*)(p + P_DEATH)) {
                        unsigned mask = *(const unsigned*)(w + W_MASK);
                        short c0 = (mask & (1u << 21)) ? *(const short*)(p + P_SPIN) : t0;
                        emit_model(child, ax, ay, wx, wz, c0, t1, t2, owner);
                    }
                }
                break;
            }
            case 2:
                s_c.ball++;              /* background refraction: engine-only */
                break;
            case 4: {
                s_c.sprite++;
                if (color == 0xFF) break;
                SHADOW_BLOB();
                if (color > 4) break;
                const char* seq = *(const char* const*)(ta + OFF_SPRSEQ0 + color * 4);
                int n = tagpu_gaf_seq_nframes(seq);
                if (n > 0) {
                    int idx = (tick - *(const int*)(p + P_SPAWN)) % n;
                    if (idx < 0) idx += n;
                    fx_sprite(tagpu_gaf_seq_frame(seq, idx), sx, sy, MODE_OPAQUE, wx, wz);
                }
                break;
            }
            case 5: {
                s_c.flare++;
                const char* seq = *(const char* const*)(ta + OFF_FLARESEQ);
                int n = tagpu_gaf_seq_nframes(seq);
                int life = *(const unsigned short*)(w + W_LIFE);
                if (n > 0 && life > 0 && alphaOn) {
                    int idx = n - ((*(const int*)(p + P_DEATH) - tick) * n) / life;
                    if (idx >= 0 && idx < n)
                        fx_sprite(tagpu_gaf_seq_frame(seq, idx), sx, sy, MODE_ALPHA, wx, wz);
                }
                break;
            }
            case 7: {
                s_c.light++;
                int c1 = coltab[color];
                int dx = X - *(const int*)(p + P_XS);
                int dz = ALT - *(const int*)(p + P_ALTS);
                int dy = Y - *(const int*)(p + P_YS);
                double len = sqrt((double)dx * dx + (double)dz * dz + (double)dy * dy);
                long long len16 = (long long)len;                    /* __ftol */
                long long n16 = (len16 << 16) / 0x50000;              /* steps of 5 */
                int nseg = (int)(n16 >> 16);
                if (n16 != 0 && nseg > 0 && nseg < 512) {
                    long long stx = ((long long)dx << 16) / n16;
                    long long stz = ((long long)dz << 16) / n16;
                    long long sty = ((long long)dy << 16) / n16;
                    int pass;
                    for (pass = 0; pass < 2; pass++) {
                        long long cx = *(const int*)(p + P_XS), cz = *(const int*)(p + P_ALTS), cy = *(const int*)(p + P_YS);
                        int px = (int)(cx >> 16), pz = (int)(cz >> 16), py = (int)(cy >> 16);
                        int k;
                        for (k = 0; k < nseg; k++) {
                            cx += stx; cz += stz; cy += sty;
                            int jx = (int)(cx >> 16) + jitter5();
                            int jz = (int)(cz >> 16) + jitter5();
                            int jy = (int)(cy >> 16) + jitter5();
                            emit_line(px - eyeX + vpL, (py - (pz >> 1)) - eyeY + vpT,
                                      jx - eyeX + vpL, (jy - (jz >> 1)) - eyeY + vpT, c1, wx, wz);
                            px = jx; pz = jz; py = jy;
                        }
                    }
                }
                break;
            }
            default:
                s_c.other++;
                break;
            }
            #undef SHADOW_BLOB
        }
    }

    /* ---- flying debris pieces (particle slots, drawn by 0x4211D0) ---- */
    if (s_debris) {
        unsigned a;
        for (a = PSYS_BEGIN; a < PSYS_END; a += 4) {
            const char* sys = *(const char* const*)(size_t)a;
            if (!ptr_ok(sys) || IsBadReadPtr(sys, 0x30)) continue;
            const char* pc = *(const char* const*)(sys + 0x2C);
            if (!ptr_ok(pc) || IsBadReadPtr(pc, 0x30)) continue;
            const char* node = *(const char* const*)(pc + D_NODE);
            int X = *(const int*)(pc + D_X), ALT = *(const int*)(pc + D_ALT), Y = *(const int*)(pc + D_Y);
            int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
            int sx = hx - eyeX + vpL, sy = (hy - eyeY) - (halt >> 1) + vpT;
            if (!in_vprect(ta, sx, sy)) continue;
            s_c.debris++;
            const short* tr = (const short*)(pc + D_TURN);
            emit_model(node, (float)X / 65536.0f - (float)eyeX + (float)vpL,
                       (float)Y / 65536.0f - (float)ALT / 65536.0f * 0.5f - (float)eyeY + (float)vpT,
                       (float)hx, (float)(hy - (halt >> 1)), tr[0], tr[1], tr[2], 0);
        }
    }

    /* ---- explosions (0x420B00) ---- */
    int ne = *(const int*)(ta + OFF_NEXPL);
    if (s_expl && ne > 0 && ne <= 300) {
        const char* ebase = ta + OFF_EXPL;
        int i, pass;
        s_c.expl = ne;
        for (pass = 0; pass < 2; pass++)
        for (i = 0; i < ne; i++) {
            const char* e = ebase + (size_t)i * EXPL_STRIDE;
            int X = *(const int*)(e + E_X), ALT = *(const int*)(e + E_ALT), Y = *(const int*)(e + E_Y);
            int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
            int sx = hx - eyeX + vpL, sy = (hy - eyeY) - (halt >> 1) + vpT;
            if (!in_vprect(ta, sx, sy)) continue;
            float wx = (float)hx, wz = (float)(hy - (halt >> 1));
            if (pass == 0) {
                if (flashOn && *(const unsigned* )(e + E_ST2 + TAGPU_AS_SEQ) != 0) {
                    const unsigned char* g = tagpu_gaf_state_frame(e + E_ST2);
                    if (g) { fx_sprite(g, sx, sy, MODE_FLASH, wx, wz); s_c.flash++; }
                }
            } else {
                const char* node = *(const char* const*)(e + E_NODE);
                if (ptr_ok(node)) {
                    const short* tr = (const short*)(e + E_TURN);
                    emit_model(node, (float)X / 65536.0f - (float)eyeX + (float)vpL,
                               (float)Y / 65536.0f - (float)ALT / 65536.0f * 0.5f - (float)eyeY + (float)vpT,
                               wx, wz, tr[0], tr[1], tr[2], 0);
                }
                if (*(const unsigned*)(e + E_ST1 + TAGPU_AS_SEQ) != 0) {
                    const unsigned char* g = tagpu_gaf_state_frame(e + E_ST1);
                    if (g) fx_sprite(g, sx, sy, MODE_OPAQUE, wx, wz);
                    if (s_log && i < 4 && (v->frame_counter % 60) == 0) {
                        const char* seq = *(const char* const*)(e + E_ST1 + TAGPU_AS_SEQ);
                        const char* seq2 = *(const char* const*)(e + E_ST2 + TAGPU_AS_SEQ);
                        _snprintf(lb, sizeof lb,
                            "fx: e%d seq=\"%.24s\" f=%u/%d flash=\"%.24s\" f=%u node=%p pos=(%d,%d,%d) scr=(%d,%d)",
                            i, ptr_ok(seq) ? seq + TAGPU_SQ_NAME : "?", (unsigned)*(const unsigned short*)(e + E_ST1),
                            tagpu_gaf_seq_nframes(seq), ptr_ok(seq2) ? seq2 + TAGPU_SQ_NAME : "-",
                            (unsigned)*(const unsigned short*)(e + E_ST2), (const void*)node, hx, halt, hy, sx, sy);
                        flog(lb);
                    }
                }
            }
        }
    }

    /* ---- LHT flash colours from the live table + palette (32 levels) ---- */
    if (flashOn && (!s_lhtInit || v->frame_counter - s_lhtStamp >= 300)) {
        const char* prog = *(const char* const*)TAPROG_PP;
        const unsigned char* lht = prog_ok(prog) ? *(const unsigned char* const*)(prog + PROG_LHT) : NULL;
        const unsigned char* pal = (const unsigned char*)(ta + 0x143A7);
        static unsigned char rgb[32 * 3];
        if (ptr_ok(lht) && !IsBadReadPtr(lht, 0x2000)) {
            int L;
            for (L = 0; L < 32; L++) {
                long sr = 0, sg = 0, sb = 0; int d;
                for (d = 0; d < 256; d++) {
                    int m = lht[L * 256 + d];
                    sr += (int)pal[m*4+0] - pal[d*4+0];
                    sg += (int)pal[m*4+1] - pal[d*4+1];
                    sb += (int)pal[m*4+2] - pal[d*4+2];
                }
                sr /= 256; sg /= 256; sb /= 256;
                rgb[L*3+0] = (unsigned char)(sr < 0 ? 0 : sr > 255 ? 255 : sr);
                rgb[L*3+1] = (unsigned char)(sg < 0 ? 0 : sg > 255 ? 255 : sg);
                rgb[L*3+2] = (unsigned char)(sb < 0 ? 0 : sb > 255 ? 255 : sb);
            }
            glBindTexture(GL_TEXTURE_2D, s_lhtTex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 32, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
            glBindTexture(GL_TEXTURE_2D, 0);
            s_lhtInit = 1; s_lhtStamp = v->frame_counter;
        }
    }

    static unsigned last = 0;
    if (v->frame_counter - last >= 60) {
        last = v->frame_counter;
        _snprintf(lb, sizeof lb,
            "fx: proj=%d (laser=%d model=%d sprite=%d flare=%d light=%d ball=%d hidden=%d fogged=%d) expl=%d flash=%d debris=%d -> lines=%d sprites=%d flashq=%d models=%d atlas=%d%s",
            np, s_c.laser, s_c.model, s_c.sprite, s_c.flare, s_c.light, s_c.ball, s_c.hidden, s_c.fogged,
            ne, s_c.flash, s_c.debris, s_cLines, s_cSprites, s_cFlash, s_nm, s_atlas.n,
            s_passive ? " (passive)" : "");
        flog(lb);
    }
    s_mute = 0;
}

/* one frame: the particle layers the engine draws first, the two effects
   passes, then the particle layers it draws after them */
int tagpu_fx_gather(const TAGPU_FXVIEW* v)
{
    int fxOn = (s_armed == 1), sfxOn = tagpu_sfx_on();
    if (!fxOn && !sfxOn) return 0;
    if (s_state == 0) init_gl();
    if (s_state != 1) {
        static int said = 0;
        if (sfxOn && !said) { said = 1; flog("fx: GL not ready — the particle pass (sfx) is idle too"); }
        return 0;
    }
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
    memset(s_nv, 0, sizeof s_nv); s_nm = 0;
    s_cLines = s_cSprites = s_cFlash = s_cAtlasFail = s_cOverflow = s_cQuads = 0;
    memset(&s_c, 0, sizeof s_c);
    s_encCur = v->encSprite; s_under = 0; s_mute = 0;
    if (sfxOn) tagpu_sfx_gather(v, 0, 6);
    if (fxOn) gather_fx(v);
    if (sfxOn) { tagpu_sfx_gather(v, 7, 9); tagpu_sfx_frame_done(v); }
    if (s_cOverflow || s_cAtlasFail) {
        static unsigned last = 0;
        if (v->frame_counter - last >= 60) {
            char b[160];
            last = v->frame_counter;
            _snprintf(b, sizeof b, "fx: DROPPED this frame: bucket-full=%d atlas-fail=%d (under=%d lines=%d flash=%d sprites=%d verts)",
                      s_cOverflow, s_cAtlasFail, s_nv[B_UNDER], s_nv[B_LINES], s_nv[B_FLASH], s_nv[B_SPRITES]);
            flog(b);
        }
    }
    return s_nv[0] + s_nv[1] + s_nv[2] + s_nv[3] + s_nm;
}

int tagpu_fx_nmodels(void) { return s_nm; }
const TAGPU_FXMODEL* tagpu_fx_model(int i) { return (i >= 0 && i < s_nm) ? &s_models_[i] : NULL; }

void tagpu_fx_render(const TAGPU_FXVIEW* v, unsigned int palTex,
                     unsigned int scafTex)
{
    int total = s_nv[0] + s_nv[1] + s_nv[2] + s_nv[3];
    if (s_state != 1 || total == 0) return;
    glUseProgram(s_prog);
    x_glUniform2f(s_uGame, (float)v->gw, (float)v->gh);
    glUniform1i(s_uFog, (v->fogMode & 1) | 2);   /* effects hide in grey */
    if (s_uFogOrg >= 0) x_glUniform2f(s_uFogOrg, (float)v->fogOrgX, (float)v->fogOrgY);
    if (s_uFogDim >= 0) x_glUniform2f(s_uFogDim, (float)v->fogCols, (float)v->fogRows);
    int scaf = (v->scafOn && scafTex) ? 1 : 0;
    if (s_uScafP >= 0) glUniform4f(s_uScafP, (float)v->vpL, (float)v->vpT, (float)v->vw, (float)v->vh);
    x_glUniform1f(s_uSS, (float)(v->ss > 0 ? v->ss : 1));
    x_glUniform1f(s_uZoomF, v->zoom > 0.0f ? v->zoom : 1.0f);
    x_glUniform2f(s_uZoomCF, v->zoomCx, v->zoomCy);
    x_glUniform1f(s_uZoom, v->zoom > 0.0f ? v->zoom : 1.0f);
    x_glUniform2f(s_uZoomC, v->zoomCx, v->zoomCy);
    x_glUniform1f(s_uDepthScale, v->depthScale > 1.0f ? v->depthScale : 512.0f);
    x_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_atlas.tex);
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, palTex);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, v->fogTex);
    x_glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, s_lhtTex);
    x_glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, scafTex);
    x_glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);
    {
        int b, first = 0;
        for (b = 0; b < NBUCKET; b++) {
            if (s_nv[b])
                glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)first * FXST * 4,
                                (GLsizeiptr)s_nv[b] * FXST * 4, s_verts[b]);
            first += s_nv[b];
        }
    }
    glEnable(GL_BLEND);
    x_glDepthMask(GL_FALSE);
    if (x_glLineWidth) x_glLineWidth((GLfloat)v->ss);
    x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    /* only the under-layers can sit behind a stamped feature row: the
       scaffold fetch is paid by that draw alone */
    int first = 0;
    glUniform1i(s_uScafOn, scaf);
    if (s_nv[B_UNDER]) x_glDrawArrays(GL_TRIANGLES, first, s_nv[B_UNDER]);
    first += s_nv[B_UNDER];
    glUniform1i(s_uScafOn, 0);
    if (s_nv[B_LINES]) x_glDrawArrays(GL_LINES, first, s_nv[B_LINES]);
    first += s_nv[B_LINES];
    if (s_nv[B_FLASH]) {
        x_glBlendFunc(GL_ONE, GL_ONE);
        x_glDrawArrays(GL_TRIANGLES, first, s_nv[B_FLASH]);
        x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    first += s_nv[B_FLASH];
    if (s_nv[B_SPRITES]) x_glDrawArrays(GL_TRIANGLES, first, s_nv[B_SPRITES]);
    x_glDepthMask(GL_TRUE);
}
