/* tagpu_render3do.c — Phase B: real GPU geometry into TA's compositor.

   For one unit per frame we take the ENGINE-POSED vertex buffers
   (PrimitiveStruct+0x22, refreshed by TA's lazy repose because we do NOT
   suppress DrawUnit), triangulate the Model3DONode face lists, render them
   flat-coloured into an offscreen FBO with TA's own dimetric projection
   (sx = +x, sy = -z - y/2, hotspot-anchored), read the pixels back and write
   them straight into the GAFFrame colour plane at Object3do+0x10 that TA's
   blit (0x459200 -> CopyGafToContext) stamps onto the frame.

   Palette trick: faces carry TA palette indices, so the FBO renders the INDEX
   itself (in the red channel, flat-interpolated) — readback needs no
   nearest-match pass and is exact. Background clears to index 1 = ColorKey.
   The depth PLANE of the composite is left as the engine wrote it: it encodes
   elevation for the cargo z-merge only (ground blit never reads it) and our
   silhouette matches the engine's because we render the same posed verts with
   the same projection. GL self-occlusion uses the true dimetric view depth
   (2y - z), resolved per-pixel by the FBO depth test. */

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_model3do.h"   /* TAGPU_PBMAXPIECE: the piece-count bound */
#include "tagpu_render3do.h"
#include "tagpu_gaf.h"
#include "tagpu_r3dcache.h"
#include "tagpu_overlay.h"   /* tagpu_overlay_target_fbo: the frame's default draw target */

/* ---- engine layout (binary-verified; wiki unit-3do-bridge / composite-buffer) ---- */
#define O3_NUMPARTS   0x00     /* u16 piece count                          */
#define O3_PRIM0      0x22     /* inline PrimitiveStruct[], stride 0x36    */
#define PRIM_STRIDE   0x36
#define P_NODE        0x00     /* Model3DONode*                            */
#define P_VBUF        0x22     /* i32* posed verts, VertexCount*3, 16.16   */
#define P_FLAGS       0x28     /* bit0 = visible                           */
#define N_VCOUNT      0x04     /* Model3DONode.VertexCount                 */
#define N_FCOUNT      0x08     /* Model3DONode.FaceCount                   */
#define N_NAME        0x1C     /* Model3DONode.pNameStr                    */
#define N_FACES       0x28     /* Model3DONode.pFaceArray                  */
#define FACE_STRIDE   0x20     /* Model3DOFace                             */
#define F_COLORTAB    0x00     /* PaletteEntry resolved to a table pointer */
#define F_VCOUNT      0x04     /* vertex indices in this face              */
#define F_TEXNAME     0x08     /* char* GAF frame name, 0 = flat colour    */
#define F_INDICES     0x0C     /* u16* vertex indices                      */
#define GF_WIDTH      0x00     /* GAFFrame u16                             */
#define GF_HEIGHT     0x02
#define GF_HOTX       0x04     /* s16 model-origin pixel inside the sprite */
#define GF_HOTY       0x06
#define GF_PTRCOLOR   0x10     /* u8* colour plane, top-down, stride=W     */

#define FBO_DIM   640          /* stock composite is AABB-capped 600x600   */
#define MAXVERTS  24576        /* triangulated vertices per unit per frame */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void rlog(const char* s)
{
    FILE* fl = fopen("tagpu.log", "a");
    if (fl) { fprintf(fl, "%s\n", s); fclose(fl); }
}

/* GL 1.1 entries the fork does not expose — GetProcAddress(opengl32) works for
   these under wine (wglGetProcAddress does NOT; G1 lesson). */
typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_CLEARCOLOR)(GLfloat,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_DEPTHFUNC)(GLenum);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_READPIXELS)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM3F)(GLint,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_READBUFFER)(GLenum);
typedef void (APIENTRY *PFN_CLEARBUFFERFV)(GLenum,GLint,const GLfloat*);

static PFN_READBUFFER    x_glReadBuffer;
static PFN_CLEARBUFFERFV x_glClearBufferfv;
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_CLEARCOLOR x_glClearColor;
static PFN_DEPTHFUNC  x_glDepthFunc;
static PFN_DISABLE    x_glDisable;
static PFN_READPIXELS x_glReadPixels;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_UNIFORM3F  x_glUniform3f;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) p = (void*)GetProcAddress(gl, n);
    }
    return p;
}

static int    s_state = 0;         /* 0=unloaded 1=ready 2=failed */
void tagpu_r3d_glreset(void);
static GLuint s_prog, s_vao, s_vbo, s_fbo, s_colTex, s_depTex, s_dplTex;
static GLint  s_uWH, s_uShadeOn, s_uNanoOn, s_uNanoT, s_uNanoC;
static GLuint s_lutTex;
static int    s_lutBuilt = 0;
#define VSTRIDE 9                          /* x,y,depth, u,v, col,ck, elev, shade */
static float  s_verts[MAXVERTS * VSTRIDE];
static unsigned char s_pixels[FBO_DIM * FBO_DIM]; /* readback scratch    */
static unsigned char s_depth [FBO_DIM * FBO_DIM]; /* depth-plane scratch */
static unsigned char s_hi    [FBO_DIM * FBO_DIM]; /* 2x supersample colour   */
static unsigned char s_hidep [FBO_DIM * FBO_DIM]; /* 2x supersample depth    */

/* ---- 8bpp texture atlas: the unit textures' GAF frames as palette indices
   in an R8 texture, NEAREST-sampled => the FBO stays index-exact. Since G14g
   it is a TAGPU_GAFATLAS (tagpu_gaf.c), the same shelf atlas the feature and
   effects passes use, with the unit layout renderers.md 2.5 decided: every
   frame in a cell with a 4-texel replicated border, 4-aligned, so the
   Classic++ twin can be mipmapped to level 2 without one frame bleeding into
   the next (tagpu_gaf.h `pad`/`align`/`mip`). The entry's u0..v1 are still
   the frame's OWN texels, edge-mapped (TA maps quad corners to texture
   edges; a centre inset shifts every interior sample half a texel and flips
   ~50% of NEAREST lookups on noisy textures), so Classic's samples never
   reach the border and its pixels do not move. Recycled when full, at the
   start of a frame (tagpu_r3d_atlas_frame) or of a blit-path render, never
   between an emit and its draw. 2048^2 holds ~1,400 median cells (32x64
   frames become 40x72); the old 1024^2 with a 1-texel gap held 256 entries
   and drew a 257th flat. ---- */
#define ATLAS_DIM  2048
#define ATLAS_MAX  2048
#define ATLAS_PAD  4                      /* tools/tascene UNIT_PAD          */
#define ATLAS_MIP  2                      /* renderers.md 2.9: covers 0.25   */
static TAGPU_GAFENT  s_atlasEnts[ATLAS_MAX];
static TAGPU_GAFATLAS s_atlas;

/* The atlas entry for a unit texture frame, uploading it on first sight.
   NULL for an unreadable frame, a compressed one (the engine's 3DO textures
   are raw planes; a compressed frame drew flat before G14g too, kept so
   Classic does not move -- whether the engine would texture it is not
   established) or a full atlas (recycled next frame). */
static const TAGPU_GAFENT* atlas_get(const char* g)
{
    const unsigned char* f = tagpu_gaf_frame_sane(g);
    if (!f || f[TAGPU_GF_COMP] != 0) return NULL;
    return tagpu_gaf_atlas_get(&s_atlas, f);
}

/* ---- per-face directional shading (G10): palette-aware shade LUT ----
   The composite stays 8bpp, so "lighting" = remapping each palette index to
   the palette's nearest entry to rgb*factor. 32 brightness rows, factor
   0.60 + 0.025*row => row 16 is EXACT 1.0 and forced to identity (shade off
   and shade-neutral are bit-identical to the unshaded renderer). Candidate
   indices 2..254 only — never emit reserved 0/1(ColorKey)/255. Built once
   from the live in-game palette; `tagpu_shade.off` in the game dir disables
   the remap per frame (A/B in one run). */
#define SH_ROWS    32
#define SH_NEUTRAL 16
static int s_shNeutral = SH_NEUTRAL;   /* LUT row that is identity/neutral      */
static int s_shDir     = 1;            /* +1 = higher row is brighter           */
static void shade_upload(const unsigned char* lut)
{
    glBindTexture(GL_TEXTURE_2D, s_lutTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, SH_ROWS, GL_RED, GL_UNSIGNED_BYTE, lut);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_lutBuilt = 1;
}
static void shade_build_lut(void)
{
    const char* base = *(const char* const*)0x511DE8;
    if (!ptr_ok(base) || IsBadReadPtr(base + 0x143A7, 256 * 4)) return;
    const unsigned char* pal = (const unsigned char*)(base + 0x143A7);
    static unsigned char lut[SH_ROWS * 256];
    int r, i, c;

    /* Prefer the ENGINE's own 32x256 shade table (PALETTE.SHD, built at init,
       live at *(TAProgramStruct+0xC4) — the table its Gouraud rasteriser
       0x459C70 uses; shadows-cloak.md/build-state.md). Calibrate rather than
       assume: neutral = the row with the most identity entries, direction from
       end-row luminance. Fall back to our computed LUT if unreadable. */
    /* TAProgramStruct is a STATIC (~0x51xxxx) — ptr_ok's heap range would
       reject it; range-check manually instead. */
    const char* prog = *(const char* const*)0x51FBD0;
    if ((size_t)prog > 0x401000u && (size_t)prog < 0x7FFF0000u &&
        !IsBadReadPtr(prog + 0xC4, 4)) {
        const unsigned char* shd = *(const unsigned char* const*)(prog + 0xC4);
        if (ptr_ok(shd) && !IsBadReadPtr(shd, SH_ROWS * 256)) {
            int bestr = -1, bestn = -1;
            for (r = 0; r < SH_ROWS; r++) {
                int n = 0;
                for (i = 0; i < 256; i++) n += (shd[r*256 + i] == i);
                if (n > bestn) { bestn = n; bestr = r; }
            }
            long lum0 = 0, lum31 = 0;
            for (i = 0; i < 256; i++) {
                const unsigned char* c0 = pal + (size_t)shd[0*256 + i]  * 4;
                const unsigned char* c1 = pal + (size_t)shd[31*256 + i] * 4;
                lum0  += c0[0] + c0[1] + c0[2];
                lum31 += c1[0] + c1[1] + c1[2];
            }
            s_shNeutral = bestr;
            s_shDir     = (lum31 >= lum0) ? 1 : -1;
            shade_upload(shd);
            { char b[96]; _snprintf(b, sizeof b,
                "r3d shade: engine SHD table (neutral=%d id=%d/256 dir=%d)",
                s_shNeutral, bestn, s_shDir); rlog(b); }
            return;
        }
    }
    for (r = 0; r < SH_ROWS; r++) {
        float f = 0.60f + 0.025f * r;
        for (i = 0; i < 256; i++) {
            if (r == SH_NEUTRAL || i <= 1 || i == 255) {
                lut[r * 256 + i] = (unsigned char)i;
                continue;
            }
            int tr = (int)(pal[i*4+0] * f + 0.5f); if (tr > 255) tr = 255;
            int tg = (int)(pal[i*4+1] * f + 0.5f); if (tg > 255) tg = 255;
            int tb = (int)(pal[i*4+2] * f + 0.5f); if (tb > 255) tb = 255;
            int best = i, bestd = 0x7FFFFFFF;
            for (c = 2; c <= 254; c++) {
                int dr = pal[c*4+0] - tr, dg = pal[c*4+1] - tg, db = pal[c*4+2] - tb;
                int d = dr*dr + dg*dg + db*db;
                if (d < bestd) { bestd = d; best = c; }
            }
            lut[r * 256 + i] = (unsigned char)best;
        }
    }
    s_shNeutral = SH_NEUTRAL; s_shDir = 1;
    shade_upload(lut);
    rlog("r3d shade: computed palette LUT (32 rows, row 16 identity)");
}

/* Model-space toward-camera axis: depth is 2y-z (larger = nearer), so the
   nearness gradient (0,2,-1)/sqrt5 points at the viewer. Faces that survive
   the depth test face the camera, so flipping each normal into the +V
   hemisphere yields the OUTWARD normal without trusting 3DO winding. */
static const float SH_V[3] = { 0.0f, 0.8944f, -0.4472f };
/* sun: high, from screen upper-left, slightly toward camera */
static const float SH_L[3] = { -0.35f, 0.80f, -0.49f };

/* ---- build-state (nanoframe) staging — engine formulas from build-state.md.
   p = Nanoframe*255 runs 255->0 over the build; two triangle-wave "blues"
   bounce over palette ramp 0xA0..0xAF; 5 stages pick a height threshold t and
   the (above, band, below) colours: -2 erase-to-ColorKey, -1 keep, else index. */
#define MAXLVERTS 8192                       /* wireframe line vertices */
static float s_lverts[MAXLVERTS * VSTRIDE];
static int nano_tri(int ph)
{
    return (ph & 0x10) ? 0xAF - (ph & 0xF) : 0xA0 + (ph & 0xF);
}
static void nano_stage(int p, float b1, float b2, float* t, float c[3])
{
    if (p >= 236)      { *t = (float)((p-235)*255/20);              c[0]=-2; c[1]=b1; c[2]=-2; }
    else if (p >= 201) { *t = (float)((p-200)*255/35);              c[0]=-2; c[1]=b1; c[2]=-2; }
    else if (p >= 116) { *t = (float)(((115-p)*255/85 - 1) & 0xFF); c[0]=-2; c[1]=b2; c[2]=b1; }
    else if (p >= 31)  { *t = (float)(((30-p)*255/85 - 1) & 0xFF);  c[0]=b1; c[1]=b2; c[2]=-1; }
    else               { *t = (float)(p*255/30);                    c[0]=-1; c[1]=b1; c[2]=-1; }
}

/* The whole build-state decision for one unit, in the engine's own terms, so
   the two renderers that stage the scaffold (this one into a composite plane,
   the native pass into the GL frame) cannot drift apart on the formulas.

   Fills `t` (the height threshold, in composite depth-plane bytes), `c`
   (cAbove, cBand, cBelow: -2 erase, -1 keep the texture, else a palette index
   over 255) and `wire` (the second blue, the wireframe's colour). Returns 0 —
   touching nothing — when the unit is not a nanoframe, which is every unit
   whose `Nanoframe` (+0x104, the fraction of the build REMAINING) is 0.

   Deliberately does NOT test tagpu_nano.off: each caller reads that flag at
   its own cadence (the native pass once per arm poll; the composite path only
   after this function has said the unit is a nanoframe at all, so the stat
   costs nothing on the units that are not). build-state.md 0x458DD0 /
   0x458D30. */
int tagpu_r3d_nano_state(const char* unit, float* t, float c[3], float* wire)
{
    if (!ptr_ok(unit) || IsBadReadPtr(unit, 0x108)) return 0;
    float nano = *(const float*)(unit + 0x104);
    if (!(nano > 0.0f && nano <= 1.0f)) return 0;
    {
        const char* base = *(const char* const*)0x511DE8;
        unsigned tick = 0;
        if (ptr_ok(base) && !IsBadReadPtr(base + 0x38A47, 4))
            tick = *(const unsigned*)(base + 0x38A47);
        unsigned slot = *(const unsigned short*)(unit + 0xA8);
        int b1 = nano_tri((int)((slot ^ 5) + tick * 0x21 / 0x1E));
        int b2 = nano_tri((int)((slot ^ 9) + tick * 0x39 / 0x1E));
        int p  = (int)(nano * 255.0f);
        if (p > 255) p = 255; else if (p < 0) p = 0;
        *wire = (float)b2 / 255.0f;
        nano_stage(p, (float)b1 / 255.0f, *wire, t, c);
    }
    return 1;
}

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"   /* sprite px, sprite py, view depth  */
    "layout(location=1) in vec2 aUV;\n"    /* atlas UV; u<0 => flat-coloured    */
    "layout(location=2) in vec2 aCK;\n"    /* x: flat idx/255; y: tex ck/255    */
    "layout(location=3) in float aElev;\n"  /* model y: composite depth = y+0x32 */
    "layout(location=4) in float aShade;\n" /* LUT row / 31, constant per face   */
    "uniform vec2 uWH;\n"
    "out vec2 vUV; flat out vec2 vCK; out float vElev; flat out float vShade;\n"
    "void main(){\n"
    /* sprite row 0 (top) -> NDC y=-1 -> readback row 0: readback is top-down */
    "  gl_Position = vec4(aPos.x/uWH.x*2.0-1.0, aPos.y/uWH.y*2.0-1.0, aPos.z, 1.0);\n"
    "  vUV = aUV; vCK = aCK; vElev = aElev; vShade = aShade;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec2 vCK; in float vElev; flat in float vShade;\n"
    "layout(location=0) out vec4 frag;\n"
    "layout(location=1) out vec4 fragDep;\n"            /* composite depth plane */
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uLUT;\n"                         /* 256 x SH_ROWS shade LUT */
    "uniform int uShadeOn;\n"
    "uniform int uNanoOn;\n"                            /* build-state staging on */
    "uniform float uNanoT;\n"                           /* height threshold byte  */
    "uniform vec3 uNanoC;\n"                            /* cAbove, cBand, cBelow  */
    "void main(){\n"
    "  fragDep = vec4(clamp((vElev + 50.0)/255.0, 0.0, 1.0), 0.0, 0.0, 1.0);\n"
    "  float idx;\n"
    "  if (vUV.x < 0.0) { idx = vCK.x; }\n"
    "  else {\n"
    "    idx = texture(uAtlas, vUV).r;\n"               /* NEAREST: exact index */
    "    if (abs(idx - vCK.y) < 0.5/255.0) discard;\n"  /* texture colorkey hole */
    "  }\n"
    "  if (uShadeOn == 1)\n"
    "    idx = texelFetch(uLUT, ivec2(int(idx*255.0+0.5), int(vShade*31.0+0.5)), 0).r;\n"
    /* build-state recolour (engine 0x458D30 semantics, build-state.md):
       classify by height byte d=y+0x32 vs threshold t; colours -2=erase to
       ColorKey (depth still written = engine keeps full model heights),
       -1=keep, else palette index. uNanoC = (cAbove, cBand, cBelow). */
    "  if (uNanoOn == 1) {\n"
    "    float d = vElev + 50.0;\n"
    "    float c = (d < uNanoT - 4.0) ? uNanoC.z : (d < uNanoT) ? uNanoC.y : uNanoC.x;\n"
    "    if (c < -1.5)      idx = 1.0/255.0;\n"
    "    else if (c > -0.5) idx = c;\n"
    "  }\n"
    "  frag = vec4(idx, 0.0, 0.0, 1.0);\n"
    "}\n";

static GLuint mkshader(GLenum t, const char* src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log);
               rlog("render3do: shader compile FAILED:"); rlog(log); s_state = 2; }
    return s;
}

static void r3d_init(void)
{
    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glClearColor = (PFN_CLEARCOLOR)getgl("glClearColor");
    x_glDepthFunc  = (PFN_DEPTHFUNC) getgl("glDepthFunc");
    x_glDisable    = (PFN_DISABLE)   getgl("glDisable");
    x_glReadPixels = (PFN_READPIXELS)getgl("glReadPixels");
    x_glUniform1f  = (PFN_UNIFORM1F) getgl("glUniform1f");
    x_glUniform2f  = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glUniform3f  = (PFN_UNIFORM3F) getgl("glUniform3f");
    x_glReadBuffer = (PFN_READBUFFER)getgl("glReadBuffer");
    x_glClearBufferfv = (PFN_CLEARBUFFERFV)getgl("glClearBufferfv");
    if (!x_glReadBuffer || !x_glClearBufferfv) { rlog("render3do: no ReadBuffer/ClearBufferfv"); s_state = 2; return; }
    if (!x_glDrawArrays || !x_glClearColor || !x_glDepthFunc || !x_glDisable ||
        !x_glReadPixels || !x_glUniform2f || !x_glUniform1f || !x_glUniform3f ||
        !glGenFramebuffers || !glBindFramebuffer || !glFramebufferTexture2D ||
        !glCheckFramebufferStatus) {
        rlog("render3do: missing GL entry points"); s_state = 2; return;
    }

    GLuint vs = mkshader(GL_VERTEX_SHADER, VS), fs = mkshader(GL_FRAGMENT_SHADER, FS);
    if (s_state == 2) return;
    s_prog = glCreateProgram(); glAttachShader(s_prog, vs); glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    GLint ok = 0; glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { rlog("render3do: program link FAILED"); s_state = 2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uWH = glGetUniformLocation(s_prog, "uWH");

    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo); glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VSTRIDE * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, VSTRIDE * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, VSTRIDE * sizeof(float),
                          (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, VSTRIDE * sizeof(float),
                          (void*)(7 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, VSTRIDE * sizeof(float),
                          (void*)(8 * sizeof(float)));
    glBindVertexArray(0);

    /* 8bpp index atlas (R8, NEAREST both ways: sampled texel == palette
       index), created now so the unit program never samples texture 0 */
    tagpu_gaf_atlas_lost(&s_atlas);
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "unit";
    s_atlas.pad = ATLAS_PAD; s_atlas.align = ATLAS_PAD; s_atlas.mip = ATLAS_MIP;
    s_atlas.prio = 3;                 /* restored after terrain, features, effects */
    if (!tagpu_gaf_atlas_create(&s_atlas)) { rlog("render3do: atlas texture FAILED"); s_state = 2; return; }

    /* shade LUT texture (built lazily from the live palette on first use) */
    glGenTextures(1, &s_lutTex);
    glBindTexture(GL_TEXTURE_2D, s_lutTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, SH_ROWS, 0,
                 GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "uLUT"), 1);
    s_uShadeOn = glGetUniformLocation(s_prog, "uShadeOn");
    s_uNanoOn  = glGetUniformLocation(s_prog, "uNanoOn");
    s_uNanoT   = glGetUniformLocation(s_prog, "uNanoT");
    s_uNanoC   = glGetUniformLocation(s_prog, "uNanoC");
    glUseProgram(0);

    /* one fixed-size FBO; per-frame we render/read only the sprite's WxH corner */
    glGenTextures(1, &s_colTex);
    glBindTexture(GL_TEXTURE_2D, s_colTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FBO_DIM, FBO_DIM, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &s_depTex);
    glBindTexture(GL_TEXTURE_2D, s_depTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, FBO_DIM, FBO_DIM, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &s_dplTex);          /* composite depth plane target (R8) */
    glBindTexture(GL_TEXTURE_2D, s_dplTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FBO_DIM, FBO_DIM, 0,
                 GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_colTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, s_dplTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, s_depTex, 0);
    { GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
      glDrawBuffers(2, bufs); }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char b[64]; _snprintf(b, sizeof b, "render3do: FBO incomplete 0x%x", st);
        rlog(b); s_state = 2; return;
    }
    s_state = 1;
    rlog("render3do: ready (FBO 640x640, index-in-red palettiser-free path)");
}

/* Resolve a face's flat colour to a TA palette index. pColorTable is the
   on-disk PaletteEntry "resolved to a palette/color-table pointer" (tamem.h);
   the r3d_diag dump below tells us the real shape — until then: a small value
   IS the index, a valid pointer's first byte is our best candidate, else a
   neutral grey. Textured faces get the same treatment for now (flat-shaded
   milestone; GAF sampling is the next step). */
/* Validate a candidate GAFFrame* (same 0x18-byte header as the composite) and
   sample a representative palette index from its 8bpp colour plane: median-ish
   probe of 5 texels, skipping the frame's ColorKey. Returns -1 if the
   candidate is not a sane uncompressed GAFFrame. A fully-transparent texture
   (e.g. the 'ground' footprint quad) returns its ColorKey so the face renders
   transparent, exactly as the engine's textured rasteriser leaves it. */
static int gaf_sample(unsigned p)
{
    if (!ptr_ok((void*)(size_t)p) || IsBadReadPtr((void*)(size_t)p, 0x18)) return -1;
    const char* g = (const char*)(size_t)p;
    int w = *(const unsigned short*)(g + 0x00);
    int h = *(const unsigned short*)(g + 0x02);
    unsigned char ck   = *(const unsigned char*)(g + 0x08);
    unsigned char comp = *(const unsigned char*)(g + 0x09);
    const unsigned char* px = *(const unsigned char* const*)(g + 0x10);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || comp != 0) return -1;
    if (!ptr_ok(px) || IsBadReadPtr(px, (SIZE_T)w * h)) return -1;
    int sx[5] = { w/2, w/4, 3*w/4, w/2,   w/2 };
    int sy[5] = { h/2, h/2, h/2,   h/4, 3*h/4 };
    int i;
    for (i = 0; i < 5; i++) {
        unsigned char c = px[sy[i] * w + sx[i]];
        if (c != ck) return c;
    }
    return ck;   /* fully transparent texture */
}

/* Returns the face's palette index, or -1 for "draw nothing": a face with no
   flat colour AND no resolved texture (e.g. the 'ground' footprint quad) is
   skipped by the engine's rasteriser too — painting it was the green-slab bug. */
static int face_colour(const char* fa)
{
    /* pColorTable is NOT always a live pointer: live armcom faces carry the
       deterministic non-pointer value 0x01E11F00 there, which passes a naive
       range check and faults on deref (crashed the first Phase B run). Only
       trust a small value as a literal palette index; never dereference it.
       Textured faces (pColorTable==0): the loader leaves resolved texture
       pointers in the TexState words — live dumps show +0x10 for plain
       textures (ruparm) and +0x18 for the team-colour path (torso) — and the
       target is the ubiquitous GAFFrame header, so validate + sample both. */
    unsigned ct = *(const unsigned*)(fa + F_COLORTAB);
    int s = (ct > 1 && ct < 255) ? (int)ct : -1;
    if (s < 0) s = gaf_sample(*(const unsigned*)(fa + 0x10));
    if (s < 0) s = gaf_sample(*(const unsigned*)(fa + 0x18));
    if (s < 0) {
        unsigned ap = *(const unsigned*)(fa + 0x18);
        if (ptr_ok((void*)(size_t)ap) && !IsBadReadPtr((void*)(size_t)ap, 0x2C)) {
            const char* an = (const char*)(size_t)ap;
            int nfr = *(const unsigned short*)(an + 0x00);
            const unsigned* tab = *(const unsigned* const*)(an + 0x28);
            /* the table is an INLINE GAFFrame[] (stride 0x18), not ptrs */
            if (nfr > 0 && nfr <= 64)
                s = gaf_sample((unsigned)(size_t)tab);
        }
    }
    return s;
}

/* Resolve a face's texture GAFFrame for rendering: plain textures live at
   +0x10 (inline GAFFrame[]); the team-colour path at +0x18 is a GAF anim
   entry whose +0x28 is an inline frame table — pick frame[owner] there
   (clamped), pending confirmation of the exact player->frame mapping.
   Returns NULL for untextured faces. */
static const char* face_texframe(const char* fa, int owner)
{
    unsigned tp = *(const unsigned*)(fa + 0x10);
    if (ptr_ok((void*)(size_t)tp) && !IsBadReadPtr((void*)(size_t)tp, 0x18)) {
        const char* g = (const char*)(size_t)tp;
        int w = *(const unsigned short*)(g + 0x00);
        int h = *(const unsigned short*)(g + 0x02);
        if (w > 0 && h > 0 && w <= 512 && h <= 512 &&
            *(const unsigned char*)(g + 0x09) == 0) return g;
    }
    unsigned ap = *(const unsigned*)(fa + 0x18);
    if (ptr_ok((void*)(size_t)ap) && !IsBadReadPtr((void*)(size_t)ap, 0x2C)) {
        const char* an = (const char*)(size_t)ap;
        int nfr = *(const unsigned short*)(an + 0x00);
        const char* tab = *(const char* const*)(an + 0x28);
        if (nfr > 0 && nfr <= 64 && ptr_ok(tab)) {
            int k = owner < 0 ? 0 : (owner >= nfr ? nfr - 1 : owner);
            return tab + k * 0x18;                /* inline GAFFrame[] */
        }
    }
    return NULL;
}

/* One-shot dump: raw 0x20-byte face records + the raw 0x40-byte node header,
   so the true in-memory layout / pColorTable semantics can be pinned from the
   log. All reads stay inside the face/node records themselves (already
   validated); nothing pointed TO by a face is dereferenced here. */
static void hexline(const char* tag, const unsigned char* p, int n)
{
    char b[300]; int i, o;
    o = _snprintf(b, sizeof b, "%s", tag);
    for (i = 0; i < n && o < (int)sizeof b - 4; i++)
        o += _snprintf(b + o, sizeof b - o, " %02x", p[i]);
    rlog(b);
}

static void r3d_diag(const char* o3, int nparts)
{
    static int done = 0;
    if (done) return;
    done = 1;
    char b[300];
    int p, j;
    /* one line per piece: which colour did each piece's first face resolve to */
    for (p = 0; p < nparts; p++) {
        const char* pr = o3 + O3_PRIM0 + p * PRIM_STRIDE;
        const char* nd = *(const char* const*)(pr + P_NODE);
        if (!ptr_ok(nd)) continue;
        const char* nm = *(const char* const*)(nd + N_NAME);
        const char* faces = *(const char* const*)(nd + N_FACES);
        int fc = *(const int*)(nd + N_FCOUNT);
        _snprintf(b, sizeof b, "r3d DIAG summary piece %2d '%.12s' vis=%d faces=%d face0col=%d",
                  p, ptr_ok(nm) ? nm : "?",
                  *(const unsigned char*)(pr + P_FLAGS) & 1, fc,
                  (fc > 0 && ptr_ok(faces)) ? (int)face_colour(faces) : -1);
        rlog(b);
    }
    for (p = 0; p < nparts; p++) {
        const char* pr = o3 + O3_PRIM0 + p * PRIM_STRIDE;
        const char* nd = *(const char* const*)(pr + P_NODE);
        if (!ptr_ok(nd)) continue;
        const char* nm = *(const char* const*)(nd + N_NAME);
        int fc = *(const int*)(nd + N_FCOUNT);
        _snprintf(b, sizeof b, "r3d DIAG piece %d '%.10s' node=%p faces=%d verts=%d facearr=%p vertarr=%p",
                  p, ptr_ok(nm) ? nm : "?", (const void*)nd, fc,
                  *(const int*)(nd + N_VCOUNT),
                  *(const void* const*)(nd + N_FACES),
                  *(const void* const*)(nd + 0x24));
        rlog(b);
        hexline("r3d DIAG  node raw:", (const unsigned char*)nd, 0x40);
        const char* faces = *(const char* const*)(nd + N_FACES);
        if (!ptr_ok(faces)) continue;
        for (j = 0; j < fc && j < 2; j++) {
            const char* fa = faces + j * FACE_STRIDE;
            _snprintf(b, sizeof b, "r3d DIAG  face %d raw:", j);
            hexline(b, (const unsigned char*)fa, FACE_STRIDE);
            _snprintf(b, sizeof b, "r3d DIAG   gaf@10=%d gaf@18=%d chosen=%d",
                      gaf_sample(*(const unsigned*)(fa + 0x10)),
                      gaf_sample(*(const unsigned*)(fa + 0x18)),
                      (int)face_colour(fa));
            rlog(b);
            /* peek at what the TexState words point to (layout hunt) */
            {
                int t;
                for (t = 0x10; t <= 0x18; t += 8) {
                    unsigned tp = *(const unsigned*)(fa + t);
                    if (ptr_ok((void*)(size_t)tp) &&
                        !IsBadReadPtr((void*)(size_t)tp, 0x30)) {
                        _snprintf(b, sizeof b, "r3d DIAG   @%02x->%08x:", t, tp);
                        hexline(b, (const unsigned char*)(size_t)tp, 0x30);
                        unsigned t2 = *(const unsigned*)((const char*)(size_t)tp + 0x28);
                        if (ptr_ok((void*)(size_t)t2) &&
                            !IsBadReadPtr((void*)(size_t)t2, 0x30)) {
                            _snprintf(b, sizeof b, "r3d DIAG    +28->%08x:", t2);
                            hexline(b, (const unsigned char*)(size_t)t2, 0x30);
                        }
                    }
                }
            }
        }
    }
}

/* Save an 8bpp plane as binary PGM (for the engine-vs-ours diff harness). */
static void save_pgm(const char* path, const unsigned char* px, int w, int h)
{
    FILE* fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P5\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h, fp);
    fclose(fp);
}

/* Diff harness: `tagpu_diff.trigger` forces one engine rebuild of the target's
   composite (pose-dirty at Object3do+0x08 — a render-side flag the engine sets
   itself constantly; sim untouched), snapshots the engine's fresh colour plane
   next frame, then dumps ours from the same pose: tagpu_eng.pgm / tagpu_ours.pgm. */
static int s_diff_state = 0;   /* 0 idle; 1 = engine rebuild pending */

int tagpu_render3do(const TAGPU_FRAME* f, const char* unit, const char* obj3do,
                    char* gafframe)
{
    if (s_state == 0) r3d_init();
    if (s_state != 1) return 0;
    /* a full atlas recycles here, before this unit's faces are emitted: every
       emit-to-draw of the blit path is inside this call, and the native pass
       gathers and draws inside its own frame call, so neither holds a UV
       across it */
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);

    int W = *(unsigned short*)(gafframe + GF_WIDTH);
    int H = *(unsigned short*)(gafframe + GF_HEIGHT);
    if (W <= 0 || H <= 0 || W > FBO_DIM || H > FBO_DIM) return 0;
    float hotX = (float)*(short*)(gafframe + GF_HOTX);
    float hotY = (float)*(short*)(gafframe + GF_HOTY);
    unsigned char* col = *(unsigned char**)(gafframe + GF_PTRCOLOR);
    if (!ptr_ok(col)) return 0;

    int nparts = *(unsigned short*)(obj3do + O3_NUMPARTS);
    if (nparts <= 0 || nparts > TAGPU_PBMAXPIECE) return 0;
    int owner = ptr_ok(unit) ? *(const unsigned char*)(unit + 0xFF) : 0;
    /* 2x supersampled edges: render at 2x, majority-downsample (palette-safe).
       Falls back to 1x when the doubled sprite would not fit the FBO. */
    int ss = (W * 2 <= FBO_DIM && H * 2 <= FBO_DIM &&
              GetFileAttributesA("tagpu_ss.off") == INVALID_FILE_ATTRIBUTES) ? 2 : 1;

    /* build-state: fraction REMAINING at unit+0x104; staged scaffold in-shader
       + wireframe line pass (the engine's blit-time effect does not fire on
       our written pixels — live-verified — so we own the look). */
    float nanoT = 0.0f, nanoC[3] = { -1.0f, -1.0f, -1.0f }, blue2f = 0.0f;
    int   nlv = 0;
    /* nano_state FIRST: it early-outs on Nanoframe == 0, which is nearly every
       unit, and the lever is a disk stat. Testing the file first ran that stat
       once per unit per frame for the whole roster writeback_paint walks. */
    int   nanoOn = tagpu_r3d_nano_state(unit, &nanoT, nanoC, &blue2f) &&
                   GetFileAttributesA("tagpu_nano.off") == INVALID_FILE_ATTRIBUTES;
    r3d_diag(obj3do, nparts);

    if (s_diff_state == 0 &&
        GetFileAttributesA("tagpu_diff.trigger") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("tagpu_diff.trigger");
        *(int*)((char*)obj3do + 0x08) = 1;   /* force engine repose        */
        *(int*)((char*)obj3do + 0x04) = 0;   /* zero TimeVisible stamp: forces
                                                the sprite-cache REBUILD too */
        s_diff_state = 1;
        rlog("r3d diff: armed — skipping one paint so the engine rebuilds");
        return 1;                            /* leave the plane to the engine */
    }
    int dump_pair = (s_diff_state == 1);
    if (dump_pair) {
        s_diff_state = 0;
        save_pgm("tagpu_eng.pgm", col, W, H);  /* engine's own fresh render */
        {
            const unsigned char* dep = *(const unsigned char* const*)(gafframe + 0x14);
            if (ptr_ok(dep) && !IsBadReadPtr(dep, (SIZE_T)W * H))
                save_pgm("tagpu_engdep.pgm", dep, W, H);
        }
        rlog("r3d diff: engine colour+depth planes saved");
    }

    /* ---- build the triangle list from the engine-posed pieces ---- */
    int nv = 0, ntri = 0, p;
    /* self-calibrating shade-flag respect: many COB scripts never touch the
       shade opcodes, leaving every piece unflagged — exempting those units
       would erase G10 entirely. Only respect bit2 when the script uses it. */
    int anyShadeFlag = 0, nShaded = 0;
    for (p = 0; p < nparts; p++) {
        unsigned char fl = *(const unsigned char*)(obj3do + O3_PRIM0 + p * PRIM_STRIDE + P_FLAGS);
        if ((fl & 1) && (fl & 4)) { anyShadeFlag = 1; nShaded++; }
    }
    int visPieces = 0;
    for (p = 0; p < nparts; p++) {
        const char* pr = obj3do + O3_PRIM0 + p * PRIM_STRIDE;
        unsigned char pflags = *(const unsigned char*)(pr + P_FLAGS);
        if (!(pflags & 1)) continue;                                 /* COB-hidden */
        visPieces++;
        /* COB shade flag (bit2, set by 0x480DF0): the engine's Gouraud path
           lights only flagged pieces and pins the rest to neutral row 0xF —
           this is how glow/emissive faces stay lit (build-state.md §5). */
        int pieceShaded = anyShadeFlag ? ((pflags & 4) != 0) : 1;
        const char* nd  = *(const char* const*)(pr + P_NODE);
        const int*  vb  = *(const int* const*)(pr + P_VBUF);
        if (!ptr_ok(nd) || !ptr_ok(vb)) continue;
        int nvert = *(const int*)(nd + N_VCOUNT);
        int nface = *(const int*)(nd + N_FCOUNT);
        const char* faces = *(const char* const*)(nd + N_FACES);
        if (nvert <= 0 || nface <= 0 || nface > 512 || !ptr_ok(faces)) continue;
        /* layout not fully pinned yet — probe, don't fault (cheap: per piece) */
        if (IsBadReadPtr(faces, (SIZE_T)nface * FACE_STRIDE) ||
            IsBadReadPtr(vb, (SIZE_T)nvert * 12)) continue;

        int j;
        for (j = 0; j < nface; j++) {
            const char* fa = faces + j * FACE_STRIDE;
            int fvc = *(const int*)(fa + F_VCOUNT);
            const unsigned short* idx = *(const unsigned short* const*)(fa + F_INDICES);
            if (fvc < 3 || fvc > 32 || !ptr_ok(idx)) continue;   /* points/lines */
            if (IsBadReadPtr(idx, (SIZE_T)fvc * 2)) continue;
            /* material: real texture (atlas) or flat palette colour or skip */
            const char* texg = face_texframe(fa, owner);
            const TAGPU_GAFENT* ae = texg ? atlas_get(texg) : NULL;
            float colv = 0.0f;
            if (!ae) {
                int fcol = face_colour(fa);
                if (fcol < 0) continue;                /* engine draws nothing */
                colv = (float)fcol / 255.0f;
            }

            /* nanoframe wireframe: every drawable face's closed outline in
               blue2, depth-tested LEQUAL over the fill (engine 0x458FA0) */
            if (nanoOn) {
                int e;
                for (e = 0; e < fvc && nlv + 2 <= MAXLVERTS; e++) {
                    unsigned short pa = idx[e], pb = idx[(e + 1) % fvc];
                    if (pa >= nvert || pb >= nvert) continue;
                    unsigned short two[2]; two[0] = pa; two[1] = pb;
                    int q;
                    for (q = 0; q < 2; q++) {
                        const int* v = vb + two[q] * 3;
                        float x = (float)v[0] / 65536.0f;
                        float y = (float)v[1] / 65536.0f;
                        float z = (float)v[2] / 65536.0f;
                        float* o = s_lverts + nlv * VSTRIDE;
                        o[0] = (x + hotX) * ss;
                        o[1] = ((-z - y * 0.5f) + hotY) * ss;
                        o[2] = (2.0f * y - z) * (-1.0f / 2048.0f);
                        o[3] = -1.0f; o[4] = -1.0f;    /* flat colour path */
                        o[5] = blue2f; o[6] = -1.0f;
                        o[7] = y;
                        o[8] = (float)s_shNeutral / 31.0f;
                        nlv++;
                    }
                }
            }

            int k;
            for (k = 1; k + 1 < fvc; k++) {                      /* fan from 0 */
                unsigned short tri[3]; int slot[3];
                tri[0] = idx[0];     slot[0] = 0;
                tri[1] = idx[k];     slot[1] = k;
                tri[2] = idx[k + 1]; slot[2] = k + 1;
                if (tri[0] >= nvert || tri[1] >= nvert || tri[2] >= nvert) continue;
                if (nv + 3 > MAXVERTS) break;
                int t;
                float P[3][3];
                for (t = 0; t < 3; t++) {
                    const int* v = vb + tri[t] * 3;
                    P[t][0] = (float)v[0] / 65536.0f;
                    P[t][1] = (float)v[1] / 65536.0f;            /* up          */
                    P[t][2] = (float)v[2] / 65536.0f;            /* map-forward */
                }
                /* face normal -> quantised shade row (degenerate = neutral) */
                float shade = (float)s_shNeutral / 31.0f;
                if (pieceShaded) {
                    float e1x = P[1][0]-P[0][0], e1y = P[1][1]-P[0][1], e1z = P[1][2]-P[0][2];
                    float e2x = P[2][0]-P[0][0], e2y = P[2][1]-P[0][1], e2z = P[2][2]-P[0][2];
                    float nx = e1y*e2z - e1z*e2y;
                    float ny = e1z*e2x - e1x*e2z;
                    float nz = e1x*e2y - e1y*e2x;
                    if (nx*SH_V[0] + ny*SH_V[1] + nz*SH_V[2] < 0.0f)
                        { nx = -nx; ny = -ny; nz = -nz; }        /* outward     */
                    float nl = sqrtf(nx*nx + ny*ny + nz*nz);
                    if (nl > 1e-6f) {
                        float I  = (nx*SH_L[0] + ny*SH_L[1] + nz*SH_L[2]) / nl;
                        /* +/-12 rows around neutral == the old 1.0+0.30*I map */
                        int   rr = s_shNeutral +
                                   s_shDir * (int)floorf(I * 12.0f + 0.5f);
                        if (rr < 0) rr = 0; else if (rr >= SH_ROWS) rr = SH_ROWS - 1;
                        shade = (float)rr / 31.0f;
                    }
                }
                for (t = 0; t < 3; t++) {
                    float x = P[t][0], y = P[t][1], z = P[t][2];
                    float* o = s_verts + nv * VSTRIDE;
                    o[0] = (x + hotX) * ss;                      /* sprite px   */
                    o[1] = ((-z - y * 0.5f) + hotY) * ss;        /* sprite py   */
                    o[2] = (2.0f * y - z) * (-1.0f / 2048.0f);   /* view depth, nearer = smaller */
                    if (ae) {
                        /* quad corners 0..3 -> (u0,v0)(u1,v0)(u1,v1)(u0,v1);
                           n-gons collapse onto the quad corners */
                        int c = fvc <= 4 ? slot[t] : slot[t] * 4 / fvc;
                        o[3] = (c == 1 || c == 2) ? ae->u1 : ae->u0;
                        o[4] = (c >= 2)           ? ae->v1 : ae->v0;
                        o[5] = 0.0f;
                        o[6] = (float)ae->ck / 255.0f;
                    } else {
                        o[3] = -1.0f; o[4] = -1.0f;              /* flat marker */
                        o[5] = colv;  o[6] = -1.0f;
                    }
                    o[7] = y;      /* model elevation -> composite depth y+0x32 */
                    o[8] = shade;
                    nv++;
                }
                ntri++;
            }
        }
    }
    if (nv == 0) return 0;

    /* ---- FBO pass ---- */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, W * ss, H * ss);
    { const GLfloat ck[4] = { 1.0f / 255.0f, 0, 0, 1 };  /* index 1 = ColorKey */
      const GLfloat fz[4] = { 0, 0, 0, 1 };              /* depth 0 = far      */
      x_glClearBufferfv(GL_COLOR, 0, ck);
      x_glClearBufferfv(GL_COLOR, 1, fz); }
    glEnable(GL_DEPTH_TEST);
    x_glDepthFunc(GL_LESS);
    glClear(GL_DEPTH_BUFFER_BIT);

    if (!s_lutBuilt) shade_build_lut();
    glUseProgram(s_prog);
    x_glUniform2f(s_uWH, (float)(W * ss), (float)(H * ss));
    glUniform1i(s_uShadeOn,
                (s_lutBuilt &&
                 GetFileAttributesA("tagpu_shade.off") == INVALID_FILE_ATTRIBUTES) ? 1 : 0);
    glUniform1i(s_uNanoOn, nanoOn ? 1 : 0);
    x_glUniform1f(s_uNanoT, nanoT);
    x_glUniform3f(s_uNanoC, nanoC[0], nanoC[1], nanoC[2]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_lutTex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_atlas.tex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_verts, NULL, GL_STREAM_DRAW);  /* orphan */
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nv * VSTRIDE * sizeof(float), s_verts);
    x_glDrawArrays(GL_TRIANGLES, 0, nv);

    if (nanoOn && nlv > 0) {
        if (nv + nlv > MAXVERTS) nlv = MAXVERTS - nv;      /* shared VBO cap */
        if (nlv > 0) {
            glUniform1i(s_uNanoOn, 0);          /* wireframe skips staging   */
            x_glDepthFunc(GL_LEQUAL);           /* engine: ties pass (edges
                                                   draw over their own face) */
            glBufferSubData(GL_ARRAY_BUFFER,
                            (GLsizeiptr)nv * VSTRIDE * sizeof(float),
                            (GLsizeiptr)nlv * VSTRIDE * sizeof(float), s_lverts);
            x_glDrawArrays(GL_LINES, nv, nlv);
            x_glDepthFunc(GL_LESS);
        }
    }

    /* partial-render detector: a repose on the game thread can race our read
       and drop pieces/faces for a frame — the suspected flicker source. Track
       each unit's high-water counts and log dips (rate-limited). */
    {
        typedef struct { const void* k; short mp, mt10; } HW;
        static HW hw[256];
        unsigned hidx = ((unsigned)(size_t)obj3do >> 4) & 255;
        HW* e2 = &hw[hidx];
        if (e2->k != (const void*)obj3do) { e2->k = obj3do; e2->mp = 0; e2->mt10 = 0; }
        short t10 = (short)(ntri / 10);
        static unsigned lastDip = 0;
        if ((visPieces < e2->mp || t10 < e2->mt10 - 1) &&
            f->frame_counter - lastDip >= 30) {
            lastDip = f->frame_counter;
            char b[128];
            _snprintf(b, sizeof b, "r3d DIP: o3=%p pieces=%d/%d tris=%d/%d0 fr=%u",
                      (const void*)obj3do, visPieces, e2->mp, ntri, e2->mt10, f->frame_counter);
            rlog(b);
        }
        if (visPieces > e2->mp) e2->mp = (short)visPieces;
        if (t10 > e2->mt10) e2->mt10 = t10;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    x_glReadBuffer(GL_COLOR_ATTACHMENT0);
    x_glReadPixels(0, 0, W * ss, H * ss, GL_RED, GL_UNSIGNED_BYTE, ss == 2 ? s_hi : s_pixels);
    x_glReadBuffer(GL_COLOR_ATTACHMENT1);
    x_glReadPixels(0, 0, W * ss, H * ss, GL_RED, GL_UNSIGNED_BYTE, ss == 2 ? s_hidep : s_depth);
    if (ss == 2) {
        /* palette-aware majority downsample: a final pixel is opaque iff >=2
           of its 4 subsamples are (keeps silhouette coverage close to 1x);
           colour = most frequent opaque index, depth = nearest opaque. */
        int oy, ox;
        for (oy = 0; oy < H; oy++) {
            const unsigned char* r0 = s_hi    + (size_t)(oy*2)     * (W*2);
            const unsigned char* r1 = s_hi    + (size_t)(oy*2 + 1) * (W*2);
            const unsigned char* d0 = s_hidep + (size_t)(oy*2)     * (W*2);
            const unsigned char* d1 = s_hidep + (size_t)(oy*2 + 1) * (W*2);
            for (ox = 0; ox < W; ox++) {
                unsigned char c[4], d[4];
                c[0]=r0[ox*2]; c[1]=r0[ox*2+1]; c[2]=r1[ox*2]; c[3]=r1[ox*2+1];
                d[0]=d0[ox*2]; d[1]=d0[ox*2+1]; d[2]=d1[ox*2]; d[3]=d1[ox*2+1];
                int i, j, nop = 0;
                for (i = 0; i < 4; i++) nop += (c[i] != 1);
                if (nop < 2) { s_pixels[oy*W+ox] = 1; s_depth[oy*W+ox] = 0; continue; }
                int best = -1, bestn = 0; unsigned char dep = 0;
                for (i = 0; i < 4; i++) {
                    if (c[i] == 1) continue;
                    if (d[i] > dep) dep = d[i];
                    int n = 0;
                    for (j = 0; j < 4; j++) n += (c[j] == c[i]);
                    if (n > bestn) { bestn = n; best = c[i]; }
                }
                s_pixels[oy*W+ox] = (unsigned char)best;
                s_depth [oy*W+ox] = dep;
            }
        }
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    x_glDisable(GL_DEPTH_TEST);
    x_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);            /* GL default back */
    glBindFramebuffer(GL_FRAMEBUFFER, tagpu_overlay_target_fbo());

    if (dump_pair) {
        save_pgm("tagpu_ours.pgm", s_pixels, W, H);
        unsigned hsum = 2166136261u; int hi;
        for (hi = 0; hi < W * H; hi++) { hsum ^= s_pixels[hi]; hsum *= 16777619u; }
        char hb[96]; _snprintf(hb, sizeof hb, "r3d diff: our plane saved, fnv=%08x", hsum);
        rlog(hb);
        /* also dump the atlas so texture uploads/packing can be inspected */
        unsigned char* ab = (unsigned char*)malloc((size_t)s_atlas.dim * s_atlas.dim);
        if (ab) {
            glBindTexture(GL_TEXTURE_2D, s_atlas.tex);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, ab);
            glBindTexture(GL_TEXTURE_2D, 0);
            save_pgm("tagpu_atlas.pgm", ab, s_atlas.dim, s_atlas.dim);
            free(ab);
        }
    }

    /* ---- write both planes (top-down == our readback row order) ---- */
    int y;
    for (y = 0; y < H; y++)
        memcpy(col + (size_t)y * W, s_pixels + (size_t)y * W, (size_t)W);
    {
        unsigned char* dep = *(unsigned char**)(gafframe + 0x14);
        if (ptr_ok(dep) && !IsBadReadPtr(dep, (SIZE_T)W * H))
            for (y = 0; y < H; y++)
                memcpy(dep + (size_t)y * W, s_depth + (size_t)y * W, (size_t)W);
    }

    /* flicker fix: cache this render so the owndraw stub can repaint a
       freshly reallocated composite synchronously (engine rebuilds movers'
       boxes; without this the rebuild frame blits an empty plane). */
    tagpu_r3dcache_store(obj3do, s_pixels, s_depth, W, H, (int)hotX, (int)hotY);

    static unsigned last = 0;
    if (f->frame_counter - last >= 60) {
        last = f->frame_counter;
        char b[128];
        _snprintf(b, sizeof b, "render3do: %d tris (%d verts) -> %dx%d hotspot=(%d,%d) ss=%d shfl=%d/%d",
                  ntri, nv, W, H, (int)hotX, (int)hotY, ss, nShaded, nparts);
        rlog(b);
    }
    return 1;
}

/* ---- exports for the native pass (G12b, tagpu_native.c): share the atlas,
   shade LUT and calibration so both paths draw identical materials ---- */
GLuint tagpu_r3d_atlas_texref(void) { return s_atlas.tex; }
GLuint tagpu_r3d_atlas_rgbref(void) { return s_atlas.rgb; }
unsigned tagpu_r3d_atlas_gen(void)  { return s_atlas.gen; }
void tagpu_r3d_atlas_frame(const unsigned char* pal)
{
    if (s_state != 1) return;
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
    tagpu_gaf_atlas_restore(&s_atlas, pal);
}
GLuint tagpu_r3d_lut_texref(void)
{
    if (!s_lutBuilt && s_state == 1) shade_build_lut();
    return s_lutBuilt ? s_lutTex : 0;
}
int tagpu_r3d_shade_neutral(void) { return s_shNeutral; }
int tagpu_r3d_shade_dir(void)     { return s_shDir; }
int tagpu_r3d_atlas_uv(const char* g, float uv[4], float* ck)
{
    if (s_state != 1) return 0;
    const TAGPU_GAFENT* e = atlas_get(g);
    if (!e) return 0;
    uv[0] = e->u0; uv[1] = e->v0; uv[2] = e->u1; uv[3] = e->v1;
    *ck = (float)e->ck / 255.0f;
    return 1;
}
int tagpu_r3d_ready(void) { return s_state == 1; }
int tagpu_r3d_ensure(void)             /* init on demand (GL context current) */
{
    if (s_state == 0) r3d_init();
    return s_state == 1;
}
const char* tagpu_r3d_face_texframe(const char* fa, int owner) { return face_texframe(fa, owner); }
int tagpu_r3d_face_colour(const char* fa) { return face_colour(fa); }

void tagpu_r3d_glreset(void)
{
    /* fresh GL context: the new atlas/LUT textures are EMPTY — the CPU-side
       caches must forget what was uploaded or everything samples black. The
       atlas's twin and job died with the context too (tagpu_native_glreset
       has already run tagpu_rglsl_glreset: tagpu_overlay.c orders them) */
    s_state = 0;
    s_lutBuilt = 0;
    tagpu_gaf_atlas_lost(&s_atlas);
}
