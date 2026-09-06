/* tagpu_feat.c — the feature pass: trees, rocks, metal patches, splats and
   GAF wreckage, rendered natively with real depth.

   The engine keeps one 0xD-byte record per 16-px map tile (FeatureMap,
   *(main+0x14287)) and one 0x100-byte definition per feature type
   (*(main+0x1426F)). A tile whose FeatureDefIndex (+8) is below 0xFFFB is the
   ANCHOR of a feature; the other tiles of its footprint hold 0xFFFE and are
   never drawn. DrawGameScreen walks the sweep rect twice
   (research/notes/terrain-depth.md §3, decompiled again for G13a):

     flat pre-pass   every tile, before any unit: clears tile->flags bit2,
                     and for def Height (+0xFA) < 10 draws at once; taller
                     defs get flags |= 4 and are deferred
     row sweep       per 16-px row, after that row's units, every deferred
                     tile left to right

   and both call the one leaf

     0x46A610(ctx, tile, tileX, tileY)   stdcall, ret 0x10

   whose three bodies are reproduced here. Its projection (the +128/+32 are
   baked immediates, not a viewport read):

     sx = (tileX + 8)*16 + FootprintX*16/2 − eyeX
     sy = (tileY + 2)*16 + FootprintZ*16/2 − eyeY
          − (h00 + h01 + h10 + h11) >> 3        (2x2 anchor-corner heights)

   Bodies:
     1  tile flags bit0 set, def mask (+0xFE) bit0 CLEAR — 3D wreckage: the
        engine copies the wreck record into the scratch feature-unit
        (*(main+0x1420F)) and calls DrawUnit. Counted here, drawn by the
        native pass's wreck gather (native.on="… wrecks").
     2  flags bit0 set, mask bit0 set — animated GAF wreckage: shadow from
        the anim state at rec+0x10 (only when rec+0x2F bit2 and shadows are
        on), then the body from rec+4, both plain colour-keyed.
     3  otherwise a normal feature: shadow sequence *(def+0xB0) then body
        *(def+0xAC), frame 0 when static, or the anim states at def+0xD8 /
        def+0xCC when the def animates (mask bit1). Alpha blit instead of a
        plain copy per mask bit2 (body) / bit3 (shadow); the shadow is drawn
        only when the FShadow option (main+0x37F06 bit4) is set.

   The LOS gate (0x4658E0, two projected footprint corners) applies only to
   defs with +0xFF bit3, and only when the tile's "seen" nibble (flags >> 3)
   is not the local player's id — exactly the engine's condition.

   Everything above is READ-ONLY: bodies 2 and 3 of the leaf touch no engine
   state at all, and body 1's writes are into the draw-side scratch unit, so
   owning the leaf (tagpu_featown.c) changes nothing the sim reads. The
   animation frames are advanced by the tick, not by the draw
   (GAFGetCurrentFramePtrAddr is a pure read — see tagpu_gaf.c), so features
   keep animating while we own the draw.

   Depth: the whole point of this pass. Bodies write depth at the row key the
   painter's sweep implies — tall features at 3 + rel*4 (above the same row's
   units at 1 + rel*4, below the next row's), flat ones just under the
   particle layers the engine draws around the pre-pass — so units are
   occluded by trees through the depth buffer and the G12a scaffold is no
   longer needed. Shadows draw at a slightly lower key without depth writes:
   they are ground decals and must not occlude anything. */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_feat.h"
#include "tagpu_glsl.h"
#include "tagpu_featown.h"
#include "tagpu_gaf.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_native.h"

/* ---- engine layout (terrain-depth.md appendix, byte-confirmed) ---- */
#define OFF_FEATMAP  0x14287   /* FeatureStruct grid, stride 0xD              */
#define OFF_FEATDEF  0x1426F   /* FeatureDef array, stride 0x100              */
#define OFF_WRECKS   0x1420B   /* wreck records, stride 0x30                  */
#define OFF_MAPW16   0x14233   /* map W/H in 16-px tiles                      */
#define OFF_MAPH16   0x14237
#define OFF_SWEEPC   0x1424B   /* sweep cols = viewTilesX + 0xC               */
#define OFF_SWEEPR   0x1424F   /* sweep rows = viewTilesY + 0x20              */
#define OFF_LOCALPL  0x2A43    /* u8 local player id (the seen-nibble compare)*/
#define OFF_GFXOPT   0x37F06   /* bit4 = FShadow                              */
#define OFF_FCOUNT   0x14253   /* int NumFeatureDefs — anything past it is    */
                               /* junk (terrain-depth "Corrections")          */

#define FT_STRIDE    0x0D
#define FT_HEIGHT    0x04      /* u8 tile height                              */
#define FT_DEFIDX    0x08      /* u16; < 0xFFFB = live feature anchor         */
#define FT_WIDX      0x0A      /* u16 wreck record index (when flags bit0)    */
#define FT_FLAGS     0x0C      /* u8; bit0 wreckage, bit2 deferred, bits3+ seen */
#define FD_STRIDE    0x100
#define FD_NAME      0x00      /* char Name[0x20] — INLINE, not a pointer      */
#define FD_FOOTX     0x94      /* i16 footprint in 16-px tiles                */
#define FD_FOOTZ     0x96
#define FD_BODYSEQ   0xAC      /* GAF sequence: static body                   */
#define FD_SHADSEQ   0xB0      /* GAF sequence: static shadow                 */
#define FD_BODYANIM  0xCC      /* anim state: animating body                  */
#define FD_SHADANIM  0xD8      /* anim state: animating shadow                */
#define FD_HEIGHT    0xFA      /* u8; >= 10 = tall (defers to the row sweep)  */
#define FD_MASK      0xFE      /* u8; b0 GAF wreck, b1 animates, b2 body alpha,*/
                               /*     b3 shadow alpha                          */
#define FD_MASKHI    0xFF      /* u8; bit3 = LOS-gated                         */
#define WR_STRIDE    0x30
#define WR_BODYANIM  0x04      /* anim state: GAF wreck body                  */
#define WR_SHADANIM  0x10      /* anim state: GAF wreck shadow                */
#define WR_FLAGS     0x2F      /* u8; bit2 = this wreck casts a shadow        */

#define MODE_OPAQUE TAGPU_FXMODE_OPAQUE
#define MODE_ALPHA  TAGPU_FXMODE_ALPHA

#define MAXBV_BODY   32768     /* vertices per bucket per frame (6 = one quad)*/
#define MAXBV_SHAD   16384
#define FVST         10        /* x,y,enc, u,v, ck,mode, wx,wz, lam           */
#define ATLAS_DIM    2048
#define ATLAS_MAX    4096      /* a map's feature frames: a body and a shadow */
                               /* per def, plus every frame of the animating  */
                               /* ones — a 200v200 battle scars the ground    */
                               /* with enough smudge/scar defs to pass 1024   */
#define MAXFOOT      16        /* junk-def guard: footprints beyond this are  */
                               /* garbage (terrain-depth "Corrections")       */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* bounded append (MSVCRT's _vsnprintf returns -1 on truncation) */
static void sappend(char* b, int cap, int* p, const char* fmt, ...)
{
    va_list ap; int n;
    if (*p >= cap - 1) return;
    va_start(ap, fmt);
    n = _vsnprintf(b + *p, (size_t)(cap - 1 - *p), fmt, ap);
    va_end(ap);
    if (n < 0 || n > cap - 1 - *p) *p = cap - 1; else *p += n;
    b[*p] = 0;
}

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_BLENDFUNC)(GLenum,GLenum);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_DEPTHMASK)(GLboolean);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_BLENDFUNC  x_glBlendFunc;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_DEPTHMASK  x_glDepthMask;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

/* ---- arming ---- */
static int s_armed = -1;
static int s_log = 0, s_passive = 0;
static int s_flat = 1, s_tall = 1, s_shadow = 1, s_wreck = 1;
static unsigned s_armCheck = 0;

int tagpu_feat_armed(unsigned frame_counter)
{
    int was;
    HANDLE h;
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    was = s_armed;
    s_armed = 0;
    h = CreateFileA("tagpu_feat.on", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        tagpu_featown_set_skip(0);
        if (was > 0) flog("feat: disarmed");
        return 0;
    }
    {
        char buf[128]; DWORD n = 0;
        s_log = 0; s_passive = 0;
        s_flat = s_tall = s_shadow = s_wreck = 1;
        if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
            char* p = buf;
            buf[n] = 0;
            while (*p) {
                char* q;
                int last;
                while (*p && *p <= ' ') p++;
                q = p;
                while (*q && *q > ' ') q++;
                last = (*q == 0);
                *q = 0;
                if (!lstrcmpiA(p, "log")) s_log = 1;
                else if (!lstrcmpiA(p, "passive")) s_passive = 1;
                else if (!lstrcmpiA(p, "noflat")) s_flat = 0;
                else if (!lstrcmpiA(p, "notall")) s_tall = 0;
                else if (!lstrcmpiA(p, "noshadow")) s_shadow = 0;
                else if (!lstrcmpiA(p, "nowreck")) s_wreck = 0;
                if (last) break;
                p = q + 1;
            }
        }
    }
    CloseHandle(h);
    s_armed = 1;
    if (s_passive) tagpu_featown_set_skip(0);
    if (was != 1) {
        char b[160];
        _snprintf(b, sizeof b,
            "feat: ARMED (flat=%d tall=%d shadow=%d wreck=%d log=%d passive=%d)",
            s_flat, s_tall, s_shadow, s_wreck, s_log, s_passive);
        flog(b);
    }
    return 1;
}

int tagpu_feat_on(void) { return s_armed > 0; }

/* ---- GL ---- */
static int    s_state = 0;             /* 0 unloaded, 1 ready, 2 failed       */
static GLuint s_prog, s_vao, s_vbo;
static GLint  s_uGame, s_uFog, s_uFogOrg, s_uFogDim, s_uZoom, s_uZoomC, s_uDepthScale,
              s_uRestored, s_uLit;
static TAGPU_GAFENT   s_atlasEnts[ATLAS_MAX];
static TAGPU_GAFATLAS s_atlas;

enum { B_SHADOW = 0, B_BODY = 1, NBUCKET = 2 };
static float s_vShadow[MAXBV_SHAD * FVST];
static float s_vBody[MAXBV_BODY * FVST];
static float* const s_verts[NBUCKET] = { s_vShadow, s_vBody };
static const int    s_vcap[NBUCKET]  = { MAXBV_SHAD, MAXBV_BODY };
static int   s_nv[NBUCKET];

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aCM;\n"       /* colour key /255, mode        */
    "layout(location=3) in vec2 aWorld;\n"
    "layout(location=4) in float aLam;\n"   /* Classic++: the ground's lambert */
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "out vec2 vUV; flat out vec2 vCM; out vec2 vWorld; flat out float vLam;\n"
    "void main(){\n"
    "  vec2 p = (aPos.xy - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vCM = aCM; vWorld = aWorld; vLam = aLam;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec2 vCM; in vec2 vWorld; flat in float vLam;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uPal;\n"
    "uniform sampler2D uAtlasRGB;\n"   /* Classic++: the atlas's restored twin */
    "uniform int uRestored;\n"         /* 1 = sample it where its alpha says so */
    "uniform int uLit;\n"              /* 1 = Classic++: lit, RGB fog rule     */
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_FOG_FN
    "void main(){\n"
    /* features are terrain furniture: the engine draws them under the fog
       overlay, so they stay visible in grey and are merely shade-remapped */
    TAGPU_GLSL_FOG_DISCARD
    "  float idx = texture(uAtlas, vUV).r;\n"
    /* colour-keyed: the key texel is a hole, and discarding keeps it out of
       the depth buffer too — a tree occludes only where it has pixels */
    "  if (abs(idx - vCM.x) < 0.5/255.0) discard;\n"
    "  float a = (int(vCM.y + 0.5) == 2) ? 0.5 : 1.0;\n"
    /* Classic++ (uLit): the twin's colour where the lazy restore has painted
       it (alpha 1 -- tagpu_gaf.h), the palette's for a frame not yet
       restored, times the GROUND's lambert at the anchor (a billboard has no
       normal of its own; the lab's lambertAt -- a tree on a shaded slope sits
       in the shade rather than on top of it), then the grey band as the RGB
       rule (renderers.md 2.6). The hole stays the index test above. */
    "  if (uLit == 1) {\n"
    "    vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n"
    "    vec3 c = t.a > 0.5 ? t.rgb : texelFetch(uPal, ivec2(int(idx*255.0+0.5), 0), 0).rgb;\n"
    "    c *= vLam;\n"
    TAGPU_GLSL_FOG_GREY_RGB("c")
    "    frag = vec4(c * a, a); return;\n"
    "  }\n"
    "  int pi = int(idx*255.0+0.5);\n"
    TAGPU_GLSL_FOG_SHADE("pi")
    "  vec3 rgb = texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n"
    "  frag = vec4(rgb * a, a);\n"           /* premultiplied, like the FBO */
    "}\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
               flog("feat: shader FAILED:"); flog(lg); s_state = 2; }
    return sh;
}

static void init_gl(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glBlendFunc  = (PFN_BLENDFUNC) getgl("glBlendFunc");
    x_glUniform1f  = (PFN_UNIFORM1F) getgl("glUniform1f");
    x_glUniform2f  = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glDepthMask  = (PFN_DEPTHMASK) getgl("glDepthMask");
    if (!x_glDrawArrays || !x_glBlendFunc || !x_glUniform1f || !x_glUniform2f ||
        !x_glActiveTexture || !x_glDepthMask) {
        flog("feat: missing GL proc"); s_state = 2; return;
    }
    vs = mksh(GL_VERTEX_SHADER, VS); fs = mksh(GL_FRAGMENT_SHADER, FS);
    if (s_state == 2) return;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs); glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { flog("feat: link FAILED"); s_state = 2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uGame = glGetUniformLocation(s_prog, "uGame");
    s_uFog  = glGetUniformLocation(s_prog, "uFog");
    s_uFogOrg = glGetUniformLocation(s_prog, "uFogOrg");
    s_uFogDim = glGetUniformLocation(s_prog, "uFogDim");
    s_uZoom = glGetUniformLocation(s_prog, "uZoom");
    s_uZoomC = glGetUniformLocation(s_prog, "uZoomC");
    s_uDepthScale = glGetUniformLocation(s_prog, "uDepthScale");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "uPal"),   1);
    glUniform1i(glGetUniformLocation(s_prog, "uFogGrid"), 2);
    glUniform1i(glGetUniformLocation(s_prog, "uFogLUT"),  3);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlasRGB"), 4);
    s_uRestored = glGetUniformLocation(s_prog, "uRestored");
    s_uLit = glGetUniformLocation(s_prog, "uLit");
    glUseProgram(0);

    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo); glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof s_vShadow + (GLsizeiptr)sizeof s_vBody,
                 NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, FVST * 4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, FVST * 4, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, FVST * 4, (void*)20);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, FVST * 4, (void*)28);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, FVST * 4, (void*)36);
    glBindVertexArray(0);

    tagpu_gaf_atlas_lost(&s_atlas);          /* its texture is made on first use */
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "feat";
    s_atlas.prio = 1;                        /* restored after the terrain, before effects */
    tagpu_gaf_atlas_create(&s_atlas);   /* never bind texture 0 to uAtlas */
    s_state = 1;
    flog("feat: GL ready");
}

void tagpu_feat_glreset(void)
{
    s_state = 0;
    tagpu_gaf_atlas_lost(&s_atlas);
}

/* ---- emission ---- */
static float s_encCur = 0.0f;
static int   s_bucketCur = B_BODY;
static int   s_mute = 0;                /* passive: count, emit nothing        */
static int   s_cBody, s_cShadow, s_cAtlasFail, s_cOverflow;
static int   s_ownable = 0;            /* the wreck pass is up: we may own it  */

static void put_vert(int b, float x, float y, float u, float v, float c, int mode,
                     float wx, float wz, float lam)
{
    float* o = s_verts[b] + (size_t)s_nv[b] * FVST;
    o[0] = x; o[1] = y; o[2] = s_encCur; o[3] = u; o[4] = v;
    o[5] = c; o[6] = (float)mode; o[7] = wx; o[8] = wz; o[9] = lam;
    s_nv[b]++;
}

/* One GAF frame at its hotspot, clipped away when fully off the viewport.
   (wax, waz) is the feature's WORLD anchor — the fog rule is a per-fragment
   lookup in the explored/LOS maps, so every vertex carries the world position
   of its own corner (anchor + its offset from the projected anchor), which
   also makes a tree straddling the fog edge fade across it like the engine's
   per-cell overlay rather than all at once. */
static void emit_frame(const TAGPU_FXVIEW* v, const unsigned char* g, int sx, int sy,
                       int wax, int waz, int mode, float lam, int depth)
{
    int b = s_bucketCur, w, h;
    float x0, y0, x1, y1, c;
    const TAGPU_GAFENT* e;
    if (depth > 4) return;                       /* sub-frame recursion guard */
    g = tagpu_gaf_frame_sane(g);
    if (!g) return;
    {
        int sub = g[TAGPU_GF_SUBN];
        if (sub) {                               /* a sub-frame list, like sprites */
            const unsigned char* const* arr =
                *(const unsigned char* const* const*)(g + TAGPU_GF_PIX);
            int k;
            if (!ptr_ok(arr) || IsBadReadPtr(arr, (SIZE_T)sub * 4)) return;
            for (k = 0; k < sub; k++) {
                const unsigned char* sg = tagpu_gaf_frame_sane(arr[k]);
                int m = mode;
                if (!sg) continue;
                if (mode == MODE_OPAQUE && sg[TAGPU_GF_SUBALP]) m = MODE_ALPHA;
                emit_frame(v, sg, sx, sy, wax, waz, m, lam, depth + 1);
            }
            return;
        }
    }
    if (b == B_BODY) s_cBody++; else s_cShadow++;
    if (s_mute) return;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    x0 = (float)(sx - *(const short*)(g + TAGPU_GF_HOTX));
    y0 = (float)(sy - *(const short*)(g + TAGPU_GF_HOTY));
    x1 = x0 + (float)w; y1 = y0 + (float)h;
    if (x1 < (float)v->evpL || x0 > (float)(v->evpL + v->evw) ||
        y1 < (float)v->evpT || y0 > (float)(v->evpT + v->evh)) return;
    if (s_nv[b] + 6 > s_vcap[b]) { s_cOverflow++; return; }
    e = tagpu_gaf_atlas_get(&s_atlas, g);
    if (!e) { s_cAtlasFail++; return; }
    c = (float)e->ck / 255.0f;
    {   /* world position of each corner: the anchor plus the corner's offset
           from the projected anchor (screen px and world px are 1:1 here) */
        float wx0 = (float)wax + (x0 - (float)sx), wx1 = (float)wax + (x1 - (float)sx);
        float wz0 = (float)waz + (y0 - (float)sy), wz1 = (float)waz + (y1 - (float)sy);
        put_vert(b, x0, y0, e->u0, e->v0, c, mode, wx0, wz0, lam);
        put_vert(b, x1, y0, e->u1, e->v0, c, mode, wx1, wz0, lam);
        put_vert(b, x0, y1, e->u0, e->v1, c, mode, wx0, wz1, lam);
        put_vert(b, x1, y0, e->u1, e->v0, c, mode, wx1, wz0, lam);
        put_vert(b, x1, y1, e->u1, e->v1, c, mode, wx1, wz1, lam);
        put_vert(b, x0, y1, e->u0, e->v1, c, mode, wx0, wz1, lam);
    }
}

/* ---- the LOS gate, 0x4658E0: two projected footprint corners. The per-tile
   test is the same one the effects pass runs per projectile, so it is shared
   rather than copied (both are the engine's LosType rule). ---- */
static int feat_visible(const TAGPU_FXVIEW* v, int col, int row, int fx, int fz, int th)
{
    int hh = th >> 1;
    if (tagpu_fx_tile_visible(v, col * 16, row * 16 - hh)) return 1;
    return tagpu_fx_tile_visible(v, col * 16 + fx * 16, row * 16 + fz * 16 - hh);
}

/* ---- per-frame counters ---- */
typedef struct {
    int anchors, flat, tall, gafwreck, wreck3d, losSkip, junk, animated, shadows;
} FEATC;
static FEATC s_c;
static int s_logged;
static int s_lit;                       /* Classic++ this frame: anchors take the ground's light */

/* Classic++: the ground's lambert at an anchor -- the lab's lambertAt(col,
   row): central differences of the height over 32 world units at the anchor
   cell, coordinates clamped to the map, normal (-dx, 1, -dz), then the one
   lighting rule (tagpu_classicpp.c). Every corner of every quad the anchor
   emits takes this one value, as the lab's do. */
static float ground_lambert(const char* fmap, int col, int row, unsigned mapW, unsigned mapH)
{
#define HAT(c, r) ((int)*(const unsigned char*)(fmap + ((size_t)(r) * mapW + (size_t)(c)) * FT_STRIDE + FT_HEIGHT))
    int cl = col > 0 ? col - 1 : 0, cr = (unsigned)(col + 1) < mapW ? col + 1 : (int)mapW - 1;
    int ru = row > 0 ? row - 1 : 0, rd = (unsigned)(row + 1) < mapH ? row + 1 : (int)mapH - 1;
    float dx = (float)(HAT(cr, row) - HAT(cl, row)) / 32.0f;
    float dz = (float)(HAT(col, rd) - HAT(col, ru)) / 32.0f;
    float inv = 1.0f / sqrtf(dx * dx + 1.0f + dz * dz);
    float n[3];
    n[0] = -dx * inv; n[1] = inv; n[2] = -dz * inv;
    return tagpu_classicpp_ground(n);
#undef HAT
}

/* FeatureDef.Name is an inline char[0x20] (tagpu_cat.c reads the same field
   for `tacli features`), so it needs no dereference — only a NUL somewhere */
static const char* def_name(const char* def)
{
    int i;
    for (i = 0; i < 0x20; i++)
        if (def[FD_NAME + i] == 0) return i ? def + FD_NAME : "?";
    return "?";
}

/* one anchor: the three bodies of 0x46A610 */
static void draw_feature(const TAGPU_FXVIEW* v, const char* ta, const char* tile,
                         const char* def, int col, int row, int flat,
                         float encBody, int shadowsOn)
{
    int fx = *(const short*)(def + FD_FOOTX);
    int fz = *(const short*)(def + FD_FOOTZ);
    unsigned mapW = (unsigned)*(const int*)(ta + OFF_MAPW16);
    unsigned mapH = (unsigned)*(const int*)(ta + OFF_MAPH16);
    unsigned flags = *(const unsigned char*)(tile + FT_FLAGS);
    unsigned mask  = *(const unsigned char*)(def + FD_MASK);
    int h00, h01, h10, h11, sx, sy, wax, waz;
    float lam;
    const unsigned char* g;
    /* junk defs hold wild footprints; they only move our quad, but an
       unclamped one once hung the render thread (terrain-depth Corrections) */
    if (fx < 0 || fx > MAXFOOT) { fx = 1; s_c.junk++; }
    if (fz < 0 || fz > MAXFOOT) { fz = 1; s_c.junk++; }

    h00 = *(const unsigned char*)(tile + FT_HEIGHT);
    h01 = ((unsigned)(col + 1) < mapW) ? *(const unsigned char*)(tile + FT_STRIDE + FT_HEIGHT) : h00;
    if ((unsigned)(row + 1) < mapH) {
        const char* t2 = tile + (size_t)mapW * FT_STRIDE;
        h10 = *(const unsigned char*)(t2 + FT_HEIGHT);
        h11 = ((unsigned)(col + 1) < mapW) ? *(const unsigned char*)(t2 + FT_STRIDE + FT_HEIGHT) : h10;
    } else { h10 = h00; h11 = h01; }

    /* the engine's projection, +128/+32 baked in as (col+8)*16 / (row+2)*16 */
    wax = col * 16 + (fx * 16) / 2;
    waz = row * 16 + (fz * 16) / 2 - ((h00 + h01 + h10 + h11) >> 3);
    sx = wax + 128 - v->eyeX;
    sy = waz + 32 - v->eyeY;
    lam = s_lit ? ground_lambert(*(const char* const*)(ta + OFF_FEATMAP), col, row, mapW, mapH)
                : 1.0f;

    if (flags & 1) {                                  /* wreckage on this tile */
        const char* recs = *(const char* const*)(ta + OFF_WRECKS);
        const char* rec;
        if (!(mask & 1)) {                            /* body 1: 3D wreck      */
            s_c.wreck3d++;                            /* the native wreck pass */
            return;                                   /* draws it as a unit    */
        }
        s_c.gafwreck++;
        if (!s_wreck) return;
        if (!ptr_ok(recs)) return;
        rec = recs + (size_t)*(const unsigned short*)(tile + FT_WIDX) * WR_STRIDE;
        if (IsBadReadPtr(rec, WR_STRIDE)) return;
        if (shadowsOn && s_shadow && (*(const unsigned char*)(rec + WR_FLAGS) & 4)) {
            g = tagpu_gaf_state_frame(rec + WR_SHADANIM);
            if (g) {
                s_bucketCur = B_SHADOW; s_encCur = encBody - 0.3f;
                emit_frame(v, g, sx, sy, wax, waz, MODE_OPAQUE, lam, 0);
                s_c.shadows++;
            }
        }
        g = tagpu_gaf_state_frame(rec + WR_BODYANIM);
        if (g) {
            s_bucketCur = B_BODY; s_encCur = encBody;
            emit_frame(v, g, sx, sy, wax, waz, MODE_OPAQUE, lam, 0);
        }
        return;
    }

    /* body 3: a normal feature — shadow first, then the body */
    {
        const char* shadSeq = *(const char* const*)(def + FD_SHADSEQ);
        const char* bodySeq = *(const char* const*)(def + FD_BODYSEQ);
        int animating = (mask & 2) != 0;
        if (animating) s_c.animated++;
        if (shadSeq && shadowsOn && s_shadow) {
            g = animating ? tagpu_gaf_state_frame(def + FD_SHADANIM)
                          : tagpu_gaf_seq_frame(shadSeq, 0);
            if (g) {
                s_bucketCur = B_SHADOW;
                s_encCur = encBody - (flat ? 0.03f : 0.3f);
                emit_frame(v, g, sx, sy, wax, waz, (mask & 8) ? MODE_ALPHA : MODE_OPAQUE, lam, 0);
                s_c.shadows++;
            }
        }
        /* the engine gates BOTH body paths on the static sequence pointer */
        if (!bodySeq) return;
        g = animating ? tagpu_gaf_state_frame(def + FD_BODYANIM)
                      : tagpu_gaf_seq_frame(bodySeq, 0);
        if (!g) return;
        s_bucketCur = B_BODY; s_encCur = encBody;
        emit_frame(v, g, sx, sy, wax, waz, (mask & 4) ? MODE_ALPHA : MODE_OPAQUE, lam, 0);
        if (s_log && s_logged < 8 &&
            sx >= v->vpL && sx < v->vpL + v->vw && sy >= v->vpT && sy < v->vpT + v->vh) {
            char b[256];
            s_logged++;
            _snprintf(b, sizeof b,
                "feat: def=%u \"%.24s\" h=%u foot=%dx%d mask=%02X/%02X %s frame=%ux%u hot=(%d,%d) tile=(%d,%d) at=(%d,%d) enc=%.2f",
                (unsigned)*(const unsigned short*)(tile + FT_DEFIDX), def_name(def),
                (unsigned)*(const unsigned char*)(def + FD_HEIGHT), fx, fz,
                mask, *(const unsigned char*)(def + FD_MASKHI),
                flat ? "flat" : "tall",
                (unsigned)*(const unsigned short*)(g + TAGPU_GF_W),
                (unsigned)*(const unsigned short*)(g + TAGPU_GF_H),
                *(const short*)(g + TAGPU_GF_HOTX), *(const short*)(g + TAGPU_GF_HOTY),
                col, row, sx, sy, encBody);
            flog(b);
        }
    }
}

/* Every exit from the gather that draws nothing must also hand the draw back:
   the skip byte is latched from the previous frame, so returning early with it
   set would leave the engine's leaf detoured and the map bare of features until
   the 90-frame watchdog in tagpu_featown_flush notices. */
static int feat_bail(void)
{
    tagpu_featown_set_skip(0);
    return 0;
}

int tagpu_feat_gather(const TAGPU_FXVIEW* v)
{
    const char* ta = v->ta;
    const char* fmap;
    const char* fdefs;
    int mapW, mapH, nCols, nRows, r0, c0, row, col;
    int localPl, shadowsOn, nDefs;
    float flatSpan;
    if (s_armed != 1) return feat_bail();
    if (s_state == 0) init_gl();
    if (s_state != 1) return feat_bail();
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
    /* Classic++: the lazy restore of this atlas, armed once the switch is on
       (main+0x143A7 is the live palette, the same one the native pass uploads) */
    tagpu_gaf_atlas_restore(&s_atlas, (const unsigned char*)(ta + 0x143A7));

    memset(s_nv, 0, sizeof s_nv);
    memset(&s_c, 0, sizeof s_c);
    s_cBody = s_cShadow = s_cAtlasFail = s_cOverflow = 0;
    s_logged = 0;
    /* Emit nothing unless we will actually own the draw. `passive` is the
       explicit A/B lever; the wrecks requirement is the implicit one — without
       it the engine keeps drawing every feature and ours would land on top as a
       double draw, with the occlusion this pass exists for silently inert. */
    s_ownable = tagpu_native_wrecks_armed();
    s_mute = s_passive || !s_ownable;
    s_lit = tagpu_classicpp_on();

    fmap  = *(const char* const*)(ta + OFF_FEATMAP);
    fdefs = *(const char* const*)(ta + OFF_FEATDEF);
    mapW = *(const int*)(ta + OFF_MAPW16);
    mapH = *(const int*)(ta + OFF_MAPH16);
    nCols = *(const int*)(ta + OFF_SWEEPC);
    nRows = *(const int*)(ta + OFF_SWEEPR);
    if (!ptr_ok(fmap) || !ptr_ok(fdefs)) return feat_bail();
    if (mapW <= 0 || mapH <= 0 || mapW > 4096 || mapH > 4096) return feat_bail();
    if (nCols <= 0 || nRows <= 0 || nCols > 1024 || nRows > 1024) return feat_bail();

    /* The engine's own sweep rect and its edge clamps (DrawGameScreen), run
       over the ZOOM's viewport rather than the engine's (TAGPU_FXVIEW.evpL):
       screen and world differ by a pure translation, so reaching further is
       "move the eye to the wider rect's top-left and ask for more tiles". The
       quads still land at unzoomed game coordinates — the vertex shader does
       the scaling. At zoom >= 1 the deltas are zero and this IS the engine's
       rect, tile for tile. */
    r0 = ((v->eyeY + (v->evpT - v->vpT)) >> 4) - 16;
    nRows += (v->evh - v->vh) >> 4;
    /* the nCols/nRows <= 1024 sanity check above is on the ENGINE's numbers;
       re-state a ceiling on ours, since we just added to them. The effective
       rect is already trimmed to the terrain budget, and the map clamps below
       bound this further — this is the guard keeping its meaning, not a new
       policy. */
    if (nRows > 4096) nRows = 4096;
    if (r0 < 0) { nRows += r0; r0 = 0; }
    if (r0 + nRows > mapH - 1) nRows = mapH - r0 - 1;
    c0 = ((v->eyeX + (v->evpL - v->vpL)) >> 4) - 10;
    nCols += (v->evw - v->vw) >> 4;
    if (nCols > 4096) nCols = 4096;
    if (c0 < 0) { nCols += c0; c0 = 0; }
    if (c0 + nCols > mapW - 1) nCols = mapW - c0 - 1;
    if (nRows <= 0 || nCols <= 0) return feat_bail();

    nDefs = *(const int*)(ta + OFF_FCOUNT);
    if (nDefs < 0 || nDefs > 4096) nDefs = 0;         /* no guard we can trust */
    localPl = *(const unsigned char*)(ta + OFF_LOCALPL);
    shadowsOn = (*(const unsigned short*)(ta + OFF_GFXOPT) & 0x10) != 0;
    flatSpan = (float)nRows * (float)nCols;

    for (row = r0; row < r0 + nRows; row++) {
        for (col = c0; col < c0 + nCols; col++) {
            const char* tile = fmap + ((size_t)row * mapW + col) * FT_STRIDE;
            unsigned idx = *(const unsigned short*)(tile + FT_DEFIDX);
            const char* def;
            int flat;
            float enc;
            if (idx >= 0xFFFB) continue;              /* no anchor on this tile */
            /* tiles can name defs past the map's real ones, whose 0x100-byte
               records hold garbage — wild footprints and invalid sequence
               pointers (terrain-depth.md "Corrections") */
            if (nDefs && (int)idx >= nDefs) { s_c.junk++; continue; }
            def = fdefs + (size_t)idx * FD_STRIDE;
            if (IsBadReadPtr(def, FD_STRIDE)) { s_c.junk++; continue; }
            s_c.anchors++;
            flat = *(const unsigned char*)(def + FD_HEIGHT) < 10;
            if (flat) s_c.flat++; else s_c.tall++;
            if (flat ? !s_flat : !s_tall) continue;
            /* the engine's gate: only defs flagged +0xFF bit3 are LOS-tested,
               and a tile already seen by the local player skips even that */
            if ((*(const unsigned char*)(def + FD_MASKHI) & 8) &&
                ((*(const unsigned char*)(tile + FT_FLAGS) >> 3) & 0xF) != (unsigned)localPl) {
                int fx = *(const short*)(def + FD_FOOTX);
                int fz = *(const short*)(def + FD_FOOTZ);
                if (fx < 0 || fx > MAXFOOT) fx = 1;
                if (fz < 0 || fz > MAXFOOT) fz = 1;
                if (!feat_visible(v, col, row, fx, fz,
                                  *(const unsigned char*)(tile + FT_HEIGHT))) {
                    s_c.losSkip++;
                    continue;
                }
            }
            if (flat) {
                /* the backdrop band: above the particle layers the engine
                   draws before the pre-pass (0..2) and below those after it
                   (3..4); the scan fraction keeps the engine's paint order
                   when two flat features overlap */
                float f = ((float)(row - r0) * (float)nCols + (float)(col - c0)) / flatSpan;
                enc = 0.40f + 0.10f * f;
            } else {
                /* the row key the painter's sweep implies: above this row's
                   units (1 + rel*4), below the next row's; the column
                   fraction keeps left-to-right order inside the row.
                   The engine's edge clamps keep rel inside [0, sweepRows),
                   the same range the unit gather uses; clamping to the band
                   the frame's keys were sized for (rows + the gather's row
                   slack) means no arithmetic here can ever push a feature
                   into the effects band above it. */
                int rel = row - v->r0;
                if (rel < 0) rel = 0;
                if (rel > v->rows + 8) rel = v->rows + 8;
                enc = 3.0f + (float)rel * 4.0f
                      + 1.5f * ((float)(col - c0) / (float)nCols);
            }
            draw_feature(v, ta, tile, def, col, row, flat, enc, shadowsOn);
        }
    }

    /* we drew this frame: the engine's feature leaf may be skipped. Body 1
       (3D wreckage) goes through DrawUnit, which only the native pass's wreck
       gather replaces — without it, owning the leaf would lose every husk. */
    if (!s_passive && s_ownable) tagpu_featown_set_skip(1);
    else tagpu_featown_set_skip(0);
    tagpu_featown_beat(v->frame_counter);

    {
        static unsigned last = 0;
        if (v->frame_counter - last >= 60) {
            char b[420]; int p = 0;
            last = v->frame_counter;
            sappend(b, sizeof b,
                    &p, "feat: rect=%dx%d anchors=%d flat=%d tall=%d gafwreck=%d 3dwreck=%d",
                    nCols, nRows, s_c.anchors, s_c.flat, s_c.tall, s_c.gafwreck, s_c.wreck3d);
            sappend(b, sizeof b, &p, " defs=%d", nDefs);
            sappend(b, sizeof b, &p, " anim=%d los-skip=%d junk=%d -> body=%d shadow=%d atlas=%d",
                    s_c.animated, s_c.losSkip, s_c.junk, s_cBody, s_cShadow, s_atlas.n);
            if (s_cOverflow || s_cAtlasFail)
                sappend(b, sizeof b, &p, " DROPPED(full=%d atlas-fail=%d)",
                        s_cOverflow, s_cAtlasFail);
            if (s_passive) sappend(b, sizeof b, &p, " (passive: nothing emitted)");
            else if (!s_ownable)
                sappend(b, sizeof b, &p,
                        " (nothing emitted: native.on needs \"wrecks\" before we can own the leaf)");
            flog(b);
        }
    }
    return s_nv[B_SHADOW] + s_nv[B_BODY];
}

void tagpu_feat_render(const TAGPU_FXVIEW* v, unsigned int palTex)
{
    int total = s_nv[B_SHADOW] + s_nv[B_BODY];
    if (s_state != 1 || total == 0) return;
    glUseProgram(s_prog);
    x_glUniform2f(s_uGame, (float)v->gw, (float)v->gh);
    glUniform1i(s_uFog, v->fogMode & 1);        /* features darken in grey */
    if (s_uFogOrg >= 0) x_glUniform2f(s_uFogOrg, (float)v->fogOrgX, (float)v->fogOrgY);
    if (s_uFogDim >= 0) x_glUniform2f(s_uFogDim, (float)v->fogCols, (float)v->fogRows);
    x_glUniform1f(s_uZoom, v->zoom > 0.0f ? v->zoom : 1.0f);
    x_glUniform2f(s_uZoomC, v->zoomCx, v->zoomCy);
    x_glUniform1f(s_uDepthScale, v->depthScale > 1.0f ? v->depthScale : 512.0f);
    x_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_atlas.tex);
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, palTex);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, v->fogTex);
    x_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, v->fogLut);
    x_glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, s_atlas.rgb);
    x_glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_uRestored, (s_atlas.rgb && tagpu_classicpp_on()) ? 1 : 0);
    glUniform1i(s_uLit, s_lit ? 1 : 0);    /* the lambert the gather baked in */
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof s_vShadow + (GLsizeiptr)sizeof s_vBody,
                 NULL, GL_STREAM_DRAW);
    if (s_nv[B_SHADOW])
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)s_nv[B_SHADOW] * FVST * 4, s_vShadow);
    if (s_nv[B_BODY])
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)s_nv[B_SHADOW] * FVST * 4,
                        (GLsizeiptr)s_nv[B_BODY] * FVST * 4, s_vBody);
    glEnable(GL_BLEND);
    x_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);      /* premultiplied FBO */
    /* shadows are ground decals: they test depth but never write it, so a
       feature's own body is not fighting its shadow and nothing is occluded
       by a shadow that the engine would have drawn under it */
    if (s_nv[B_SHADOW]) {
        x_glDepthMask(GL_FALSE);
        x_glDrawArrays(GL_TRIANGLES, 0, s_nv[B_SHADOW]);
        x_glDepthMask(GL_TRUE);
    }
    /* bodies write depth — this is what occludes units behind trees */
    if (s_nv[B_BODY])
        x_glDrawArrays(GL_TRIANGLES, s_nv[B_SHADOW], s_nv[B_BODY]);
}
