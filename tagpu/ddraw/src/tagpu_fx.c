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
#include "tagpu_opt.h"
#include "tagpu_fx.h"
#include "tagpu_pal.h"
#include "tagpu_fxown.h"
#include "tagpu_sfx.h"
#include "tagpu_glsl.h"
#include "tagpu_gaf.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_fogwide.h"
#include "tagpu_packet.h"



#define MODE_FLAT   TAGPU_FXMODE_FLAT
#define MODE_OPAQUE TAGPU_FXMODE_OPAQUE
#define MODE_ALPHA  TAGPU_FXMODE_ALPHA
#define MODE_FLASH  TAGPU_FXMODE_FLASH

#define MAXFXV   32768          /* vertices per bucket per frame             */
#define FXST     9              /* x,y,enc, u,v, c,mode, wx,wz               */
#define MAXMODEL 1024
#define ATLAS_DIM 2048
#define ATLAS_MAX 2048

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
    char buf[128];
    int n = tagpu_opt_read("tagpu_fx.on", buf, sizeof buf);
    s_log = 0; s_lines = s_models = s_sprites = s_expl = s_debris = 1; s_passive = 0;
    if (n < 0) {
        tagpu_fxown_set_skip(0);
        if (was > 0) flog("fx: disarmed");
        return;
    }
    if (n > 0) {
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
static GLint  s_uRestored;
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
    "uniform sampler2D uAtlasRGB;\n"         /* Classic++: the atlas's restored twin */
    "uniform int uRestored;\n"               /* 1 = sample it where its alpha says so */
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
    /* Classic++: the twin's colour where the lazy restore has painted it
       (alpha 1 -- tagpu_gaf.h), the index otherwise; the flash mode above
       keeps its index-driven light table. Effects hide in grey, so no RGB
       fog rule is needed here */
    "      vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n"
    "      rgb = t.a > 0.5 ? t.rgb : texelFetch(uPal, ivec2(ii, 0), 0).rgb;\n"
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
    glUniform1i(glGetUniformLocation(s_prog, "uAtlasRGB"), 6);
    s_uRestored = glGetUniformLocation(s_prog, "uRestored");
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
    s_atlas.prio = 2;                   /* restored after terrain and features */
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
    TAGPU_GAFGEOM gm;
    if (depth > 4) return;
    /* THE FRAME IS AN OPAQUE HANDLE HERE (landing 4a). It came out of the
       packet, resolved by the game thread against the sequence the engine's
       own record named; this file reads not one byte of it. Everything below
       goes through tagpu_gaf.c, which is the module whose class (session
       reader) carries the lifetime argument for GAF storage. */
    if (!tagpu_gaf_frame_geom(g, &gm)) return;
    if (gm.subn) {
        int k;
        for (k = 0; k < gm.subn; k++) {
            TAGPU_GAFGEOM sm;
            const unsigned char* sg = tagpu_gaf_subframe(g, k);
            if (!sg || !tagpu_gaf_frame_geom(sg, &sm)) continue;
            emit_sprite(sg, sx, sy,
                        (mode == MODE_OPAQUE && sm.subalp) ? MODE_ALPHA : mode,
                        wx, wz, depth + 1);
        }
        return;
    }
    int b = (mode == MODE_FLASH) ? B_FLASH : (s_under ? B_UNDER : B_SPRITES);
    if (mode == MODE_FLASH) s_cFlash++; else s_cSprites++;
    if (s_mute) return;
    const TAGPU_GAFENT* e = tagpu_gaf_atlas_get(&s_atlas, g);
    if (!e) { s_cAtlasFail++; return; }
    int w = gm.w, h = gm.h;
    float x0 = (float)(sx - gm.hotx);
    float y0 = (float)(sy - gm.hoty);
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
   under = the engine draws this layer before its projectile pass. The frame
   is the packet's, already resolved on the game thread (landing 4a). ---- */
int tagpu_fx_emit_frame(const unsigned char* g, int sx, int sy, int mode,
                        float wx, float wz, float enc, int under)
{
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

/* TAProgram capability bits (+0xF0): bit5 ALP alpha table built, bit7 LHT.
   Read by the publisher now; this is the frame's copy, latched at gather. */
static unsigned s_caps;
unsigned tagpu_fx_caps(void) { return s_caps; }

/* The node is a per-TYPE Model3DONode template the PUBLISHER resolved and
   range-checked on the game thread; this file only carries it to the native
   pass's emit_fx_model, which is the (fenced) file that walks it, under
   tagpu_reclaim's teardown fence — the same argument PK_PIECE.node stands on.
   Not one byte of it is read here, which is what took this file off the
   thread-split allow-list. */
static void emit_model(unsigned node, float ax, float ay, float wx, float wz,
                       short t0, short t1, short t2, int owner)
{
    if (!s_models || s_mute || s_nm >= MAXMODEL || !node) return;
    TAGPU_FXMODEL* m = &s_models_[s_nm++];
    m->node = (const char*)(size_t)node; m->ax = ax; m->ay = ay; m->wx = wx; m->wz = wz;
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
/* 512, not 256: the formatted message is ~290 bytes and _snprintf silently
   truncated it mid-word, dropping exactly the half that says what to do. */
static char s_fogAlarm[512];

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
    /* The LOG fires every trip (throttled); only the DIALOG is one-shot. The
       two checks below catch different faults — a moved buffer and dims that
       have come apart from it — and with a root cause still open the second,
       differing trip is the evidence most worth having. Gating both on one flag
       threw it away. */
    static DWORD tick;
    DWORD now = GetTickCount();
    if (now - tick > 1000) {
        tick = now;
        f = fopen("tagpu.log", "a");
        if (f) {
            fprintf(f, "FOGGUARD %s grid=%p cols=%d rows=%d org=(%d,%d) world=(%d,%d)\n",
                    why, (const void*)grid, cols, rows, orgX, orgY, wx, wzp);
            fclose(f);
        }
    }
    if (InterlockedCompareExchange(&s_fogAlarmed, 1, 0) != 0) return;
    _snprintf(s_fogAlarm, sizeof s_fogAlarm,
              "The fog-grid guard caught a bad read and dropped it.\n\n"
              "%s\ngrid=%p cols=%d rows=%d org=(%d,%d) world=(%d,%d)\n\n"
              "Without the guard this is the crash that freezes the renderer.\n"
              "The game is still running. Please tell Claude, and keep the log.",
              why, (const void*)grid, cols, rows, orgX, orgY, wx, wzp);
    s_fogAlarm[sizeof s_fogAlarm - 1] = 0;
    /* never on the render thread: a modal dialog there stops the frame loop and
       we would be diagnosing our own hang instead of the engine's grid */
    {
        HANDLE h = CreateThread(NULL, 0, fog_alarm_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
}

int tagpu_fog_at(const unsigned short* grid, int cols, int rows, int cells,
                 int orgX, int orgY, int wx, int wzp)
{
    if (!grid || cols <= 0 || rows <= 0) return 0;
    /* A plausible userland pointer, and nothing narrower: since tagpu_fogwide
       the grid is the ENGINE's buffer at zoom >= 1 and OUR OWN heap allocation
       at zoom < 1, and the process heap of a 0x400000 image can sit below the
       0x600000 this used to demand. Nothing is given up — both faults this
       guard has actually caught were a base of -9 and one of -318. */
    if ((size_t)grid <= 0x10000u || (size_t)grid >= 0x7FFF0000u) {
        fog_alarm("grid pointer is not a plausible allocation",
                  grid, cols, rows, orgX, orgY, wx, wzp);
        return 0;
    }
    /* and the dims, against the bound the PRODUCER publishes rather than a
       number typed here. This used to be a literal 512, chosen when it covered
       both producers and then left behind by both: the wide grid's own cap went
       to 1024 and the viewport bound to 16384, so a screen between 4057 and
       8153 px wide got a grid tagpu_fogwide built and this test refused — and a
       refusal here is `0`, which every caller reads as "nothing is hidden",
       so the whole screen's units, wrecks and effects drew through the black.
       tagpu_fogwide_dimcap() is a high-water mark, so it can only ever be too
       generous, which for a corruption guard is the right direction to err. */
    {
        int cap = tagpu_fogwide_dimcap();
        if (cols > cap || rows > cap) {
            fog_alarm("grid dims exceed the cap the producers publish",
                      grid, cols, rows, orgX, orgY, wx, wzp);
            return 0;
        }
    }
    /* AND THE CELL COUNT, which is the bound that actually matters. The largest
       index this function can form is cols*rows - 1, so the exposure is
       cols*rows*2 bytes past `grid` — and a per-DIMENSION cap has to be
       generous enough for the largest grid EITHER producer could legitimately
       hand over, which at 1920x1080 is 1024x1024 against a buffer of 245x148.
       That is a 2 MB window in front of a 72 KB allocation, and it is four
       times wider than the literal 512 this replaced. `cells` closes it: the
       caller knows what the buffer it is passing actually holds — the engine's
       own validated `cells` field for its grid, cols*rows for ours — so the
       bound becomes exact for both instead of shared and loose. A caller that
       does not know passes 0, which refuses. */
    if (cells <= 0 || (long)cols * rows > (long)cells) {
        fog_alarm("grid dims exceed the cells the buffer holds",
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

       THE SHADERS NO LONGER CLAMP THE SAME WAY, and this comment used to say
       theirs "is exact". Since G13s taFog clamps to `uFogDim - 1.0`, one whole
       cell short, because the last column of any grid never has its right
       corners written and interpolating toward them reads as NO FOG. The band
       `gx in [cols-1, cols)` here has that same hazard and is left alone
       deliberately: with the WIDE grid it is at least 320 px outside the view
       (FOGW_MARGIN plus the window's two spare columns) against a gather that
       reaches 256, so nothing can be sampled there; with the ENGINE's grid at
       zoom >= 1 an anchor 1..32 px past the viewport edge does land in it, and
       both answers available there — the interpolation's and the off-grid
       `return 0` a tighter bound would give — are the same "no fog", so
       tightening it would change nothing but the argument. */
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
    return tagpu_fog_at(v->fogGrid, v->fogCols, v->fogRows, v->fogCells,
                        v->fogOrgX, v->fogOrgY, wx, wzp) == 0;
}

/* the rect the engine's own explosion and debris passes cull against, out of
   the packet (`vp_addr`: L, T, R, B inclusive — the true rect, or the widened
   one at zoom < 1, exactly as the engine's field held it for this draw) */
static int in_vprect(const TAGPU_PACKET* pk, int sx, int sy)
{
    return sx >= pk->vp_addr[0] && sx <= pk->vp_addr[2] &&
           sy >= pk->vp_addr[1] && sy <= pk->vp_addr[3];
}

static unsigned s_rng = 0x12345u;
static int jitter5(void)            /* rand()*11/0x8000 - 5, visual only */
{
    s_rng = s_rng * 0x343FDu + 0x269EC3u;
    return (int)(((s_rng >> 16) & 0x7FFF) * 11 / 0x8000) - 5;
}

typedef struct { int hidden, fogged, laser, model, ball, sprite, flare, light, expl, debris, flash, other; } FXC;
static FXC s_c;

/* weapon fire, explosions, debris: the three tables the publisher gathered.
   NOTHING HERE READS ENGINE MEMORY (landing 4a). The projectile, explosion and
   debris arrays are walked on the game thread, inside DrawGameScreen, once per
   sim tick; every weapon field, every colour number and every GAF frame was
   resolved there. What is left in this file is the engine's DRAWING RULES —
   which rendertype makes which primitive, where the shadow blob goes, how the
   lightning bolt jitters — applied to values. */
static void gather_fx(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_PROJ* pj = tagpu_pk_proj(pk);
    const TAGPU_PK_EXPL* ex = tagpu_pk_expl(pk);
    const TAGPU_PK_DEBRIS* db = tagpu_pk_debris(pk);
    int alphaOn = (s_caps & 0x20) != 0, flashOn = (s_caps & 0x80) != 0;
    int eyeX = v->eyeX, eyeY = v->eyeY, vpL = v->vpL, vpT = v->vpT;
    unsigned i;
    char lb[200];

    s_mute = s_passive;                /* passive: count + log, emit nothing */
    /* we are drawing this frame: the engine may skip its own effects draw */
    if (!s_passive) tagpu_fxown_set_skip(1);
    tagpu_fxown_beat(v->frame_counter);

    /* ---- projectiles (the engine's 0x49BE60 rules) ---- */
    for (i = 0; i < pk->n_proj; i++) {
        const TAGPU_PK_PROJ* p = &pj[i];
        int X = p->pos[0], ALT = p->pos[1], Y = p->pos[2];
        int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
        int hzp = hy - (halt >> 1);
        int sx, sy;
        float ax, ay, wx, wz;
        if (!tagpu_fx_tile_visible(v, hx, hzp)) { s_c.fogged++; continue; }
        sx = hx - eyeX + vpL; sy = (hy - eyeY) - (halt >> 1) + vpT;
        ax = (float)X / 65536.0f - (float)eyeX + (float)vpL;
        ay = (float)Y / 65536.0f - (float)ALT / 65536.0f * 0.5f - (float)eyeY + (float)vpT;
        wx = (float)hx; wz = (float)hzp;
        if (s_log && i < 8 && (v->frame_counter % 60) == 0) {
            _snprintf(lb, sizeof lb,
                "fx: p%u rt=%d col=%d/%d pos=(%d,%d,%d) start=(%d,%d,%d) scr=(%d,%d) turn=(%d,%d,%d) node=%08x/%08x frame=%08x",
                i, p->rt, p->col, p->col2, hx, halt, hy,
                p->start[0] >> 16, p->start[1] >> 16, p->start[2] >> 16,
                sx, sy, p->turn[0], p->turn[1], p->turn[2],
                p->node, p->child, p->frame);
            flog(lb);
        }
        /* ground shadow blob (rendertypes 1,3,4,6): alpha blit of the shadow
           sequence's frame 0 at the projectile's ground point */
        if ((p->flags & TAGPU_PK_FX_SHADOW) && alphaOn && pk->shadow_frame)
            fx_sprite((const unsigned char*)(size_t)pk->shadow_frame,
                      sx, (p->shadow_y - eyeY) + vpT, MODE_ALPHA, wx, wz);
        switch (p->rt) {
        case 0: {
            s_c.laser++;
            int x0 = sx, y0 = sy;
            int x1 = (p->start[0] >> 16) - eyeX + vpL;
            int y1 = ((p->start[2] >> 16) - ((p->start[1] >> 16) >> 1)) - eyeY + vpT;
            if (!(p->flags & TAGPU_PK_FX_COL2)) emit_line(x0, y0, x1, y1, p->col, wx, wz);
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
                emit_line(sx2, sy2, ex2, ey2, p->col2, wx, wz);
                emit_line(ax0, ay0, bx, by, p->col, wx, wz);
            }
            break;
        }
        case 1: case 3: case 6:
            s_c.model++;
            emit_model(p->node, ax, ay, wx, wz, p->turn[0], p->turn[1], p->turn[2], p->owner);
            if (p->child)
                emit_model(p->child, ax, ay, wx, wz, p->cturn0, p->turn[1], p->turn[2], p->owner);
            break;
        case 2:
            s_c.ball++;              /* background refraction: engine-only */
            break;
        case 4:
            s_c.sprite++;
            if (p->frame)
                fx_sprite((const unsigned char*)(size_t)p->frame, sx, sy, MODE_OPAQUE, wx, wz);
            break;
        case 5:
            s_c.flare++;
            if (p->frame && alphaOn)
                fx_sprite((const unsigned char*)(size_t)p->frame, sx, sy, MODE_ALPHA, wx, wz);
            break;
        case 7: {
            s_c.light++;
            int dx = X - p->start[0];
            int dz = ALT - p->start[1];
            int dy = Y - p->start[2];
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
                    long long cx = p->start[0], cz = p->start[1], cy = p->start[2];
                    int px = (int)(cx >> 16), pz = (int)(cz >> 16), py = (int)(cy >> 16);
                    int k;
                    for (k = 0; k < nseg; k++) {
                        cx += stx; cz += stz; cy += sty;
                        int jx = (int)(cx >> 16) + jitter5();
                        int jz = (int)(cz >> 16) + jitter5();
                        int jy = (int)(cy >> 16) + jitter5();
                        emit_line(px - eyeX + vpL, (py - (pz >> 1)) - eyeY + vpT,
                                  jx - eyeX + vpL, (jy - (jz >> 1)) - eyeY + vpT, p->col, wx, wz);
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
    }

    /* ---- flying debris pieces (drawn by 0x4211D0) ---- */
    if (s_debris) {
        for (i = 0; i < pk->n_debris; i++) {
            const TAGPU_PK_DEBRIS* d = &db[i];
            int X = d->pos[0], ALT = d->pos[1], Y = d->pos[2];
            int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
            int sx = hx - eyeX + vpL, sy = (hy - eyeY) - (halt >> 1) + vpT;
            if (!in_vprect(pk, sx, sy)) continue;
            s_c.debris++;
            emit_model(d->node, (float)X / 65536.0f - (float)eyeX + (float)vpL,
                       (float)Y / 65536.0f - (float)ALT / 65536.0f * 0.5f - (float)eyeY + (float)vpT,
                       (float)hx, (float)(hy - (halt >> 1)), d->turn[0], d->turn[1], d->turn[2], 0);
        }
    }

    /* ---- explosions (0x420B00): the flash of every record, then the bodies ---- */
    if (s_expl && pk->n_expl) {
        int pass;
        s_c.expl = (int)pk->n_expl;
        for (pass = 0; pass < 2; pass++)
        for (i = 0; i < pk->n_expl; i++) {
            const TAGPU_PK_EXPL* e = &ex[i];
            int X = e->pos[0], ALT = e->pos[1], Y = e->pos[2];
            int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
            int sx = hx - eyeX + vpL, sy = (hy - eyeY) - (halt >> 1) + vpT;
            float wx, wz;
            if (!in_vprect(pk, sx, sy)) continue;
            wx = (float)hx; wz = (float)(hy - (halt >> 1));
            if (pass == 0) {
                if (flashOn && e->flash) {
                    fx_sprite((const unsigned char*)(size_t)e->flash, sx, sy, MODE_FLASH, wx, wz);
                    s_c.flash++;
                }
            } else {
                emit_model(e->node, (float)X / 65536.0f - (float)eyeX + (float)vpL,
                           (float)Y / 65536.0f - (float)ALT / 65536.0f * 0.5f - (float)eyeY + (float)vpT,
                           wx, wz, e->turn[0], e->turn[1], e->turn[2], 0);
                if (e->frame)
                    fx_sprite((const unsigned char*)(size_t)e->frame, sx, sy, MODE_OPAQUE, wx, wz);
            }
        }
    }

    /* ---- LHT flash colours from the packet's table + the live palette ---- */
    if (flashOn && (!s_lhtInit || v->frame_counter - s_lhtStamp >= 300)) {
        const unsigned char* lht = tagpu_pk_lht(pk);
        const unsigned char* pal = tagpu_pal_live();
        static unsigned char rgb[32 * 3];
        if (pal && lht) {
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
            "fx: proj=%u (laser=%d model=%d sprite=%d flare=%d light=%d ball=%d fogged=%d) expl=%u flash=%d debris=%u -> lines=%d sprites=%d flashq=%d models=%d atlas=%d%s",
            pk->n_proj, s_c.laser, s_c.model, s_c.sprite, s_c.flare, s_c.light, s_c.ball, s_c.fogged,
            pk->n_expl, s_c.flash, pk->n_debris, s_cLines, s_cSprites, s_cFlash, s_nm, s_atlas.n,
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
    /* the frame's capability bits, from the packet: bit5 the ALP alpha table
       is built, bit7 the LHT one. Read by the publisher, on the game thread. */
    s_caps = v->packet ? v->packet->fx_caps : 0u;
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
    /* Classic++: the lazy restore of this atlas (the palette the screen is
       SHOWN with -- tagpu_pal.h) */
    tagpu_gaf_atlas_restore(&s_atlas, tagpu_pal_live());
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
    x_glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, s_atlas.rgb);
    x_glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_uRestored, (s_atlas.rgb && tagpu_classicpp_assets()) ? 1 : 0);
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
