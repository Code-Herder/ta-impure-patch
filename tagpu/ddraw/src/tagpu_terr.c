/* tagpu_terr.c — the terrain pass: the 32x32 map tiles, native.

   The engine's pass 0x483FA0 is a grid blit and nothing else
   (research/notes/terrain-depth.md 2, re-read from the bytes for G13b):

     tileX0 = eyeX/32 (toward zero);  fracX = eyeX - tileX0*32
     cols   = ceil((viewW + fracX)/32)            same for rows/viewH/fracY
     idx    = TILE_MAP[(tileY0+row)*rowStride + tileX0+col]   u16, *(main+0x1428B)
     gfx    = *(TILE_SET+4) + idx*0x400           8bpp, 32 wide, *(main+0x14283)
     screen = (vpL + col*32 - fracX, vpT + row*32 - fracY)

   rowStride = FeatureMapSizeX/2 (the map's width in 32-px cells). Colour plane
   only: no colour key (tiles are opaque), no shading, no depth, and NEITHER
   height NOR LOS enters it — cliffs, water and shadows are painted into the
   tile art by the map compiler. Three things follow, and this module leans on
   all three:

   * Terrain is the frame's implicit far plane, so it draws at a depth key
     BELOW every other band — under the flat-feature band (0.40), not merely
     under the row sweep.
   * Water does not animate, so a still atlas loses nothing. MEASURED
     2026-09-05, not assumed: camera pinned over open water on Anteer Strait
     and over Ring Atoll's lagoon, NOT ONE PIXEL of the viewport changed over
     10 s, nor over 30 s at +10 game speed, and NOT ONE BYTE of the 256-entry
     live palette changed across 24 samples. The minimap kept changing in the
     same frames, which is what says the capture was live. Earlier comments
     here claimed the engine cycles the palette for water; it does not.
   * Fog is a straight import of the shared rule (tagpu_glsl.h) — with one
     change that owning the bottom layer forces. Terrain must PAINT the fog's
     solid black in unexplored cells rather than discard: nothing is behind it
     any more except tagpu_terrown.c's key fill (TAGPU_GLSL_FOG_TERRAIN).

   The tile set is built by LoadMap and never changes after, so the atlas is
   built ONCE per map — a single R8 texture of 32x32 cells on a 34-texel pitch,
   64 per row (a GL_TEXTURE_2D_ARRAY is not viable: 5062 tiles on Two Continents
   against the usual 2048-layer cap). The spare texel on each side is a
   replicated edge guard, not padding — see CELL_PITCH. A map change is the TILE_SET pointer or its
   count moving. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_opt.h"
#include "tagpu_terr.h"
#include "tagpu_pal.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_glsl.h"
#include "tagpu_terrown.h"
#include "tagpu_native.h"
#include "tagpu_shadow.h"
#include "tagpu_zoom.h"

/* ---- engine layout (terrain-depth.md 1, byte-confirmed) ---- */
#define OFF_TILEMAP  0x1428B   /* u16 per 32-px cell, stride mapW16/2         */
#define OFF_TILESET  0x14283   /* -> {u32 count; u8* pixels}                  */
#define OFF_MAPW16   0x14233   /* map W/H in 16-px tiles                      */
#define OFF_MAPH16   0x14237
#define OFF_FEATMAP  0x14287   /* FeatureStruct grid, stride 0xD, one per     */
#define FT_STRIDE    0x0D      /* 16-px cell; the height byte at +4 is what   */
#define FT_HEIGHT    0x04      /* Classic++ lights the terrain from           */

#define TILE_PX      32
#define TILE_BYTES   0x400
#define ATLAS_COLS   64
/* Cells are laid out one texel apart, and that texel is a COPY of the cell's
   last row/column — it is not padding, it is a guard rail. A fragment centre
   that lands exactly on a quad's far edge interpolates u (or v) to exactly u1,
   and GL_NEAREST resolves that to floor(u1*W) = the FIRST texel of the next
   cell: at zoom 0.25 with ss=2 a tile is 16 FBO px, so a tile boundary lands
   on a pixel centre whenever the cell's game-space top edge is odd, and every
   such row sampled the unrelated tile 64 cells later in the atlas — the blue
   hairlines along tile edges. Duplicating the edge texel makes that sample the
   right colour instead. Nothing else changes: the quad still spans 32 texels,
   so sampling at 1:1 is bit-identical to the un-padded atlas. */
#define CELL_BORDER  1
#define CELL_PITCH   (TILE_PX + 2 * CELL_BORDER) /* 34                         */
#define ATLAS_W      (ATLAS_COLS * CELL_PITCH)   /* 2176                       */
#define MAX_TILES    65536                      /* the index is a u16          */
#define ICOMP        4                          /* col,row, atlas col,row      */
/* THE STAGING CEILING IS MEMORY, NOT A RESOLUTION. What one frame may need is
   reserved from the live viewport at the zoom floor (tagpu_terr_clamp_span),
   so it tracks the screen. MEASURED, from the `terr: staging` line: 1024x768
   reserves 10672 cells (83 KB), 2560x1440 54208 (423 KB), 3840x2160 124488
   (972 KB), 5120x2880 223568 (1746 KB); 7680x4320 works out at 507592
   (3966 KB). This is only the point past which a viewport is NOT BELIEVED —
   `vw`/`vh` are read out of engine memory, and a garbage pair must not be
   allowed to ask for an arbitrary allocation. 24 MB is a viewport of about
   14000 x 14000, which is past any screen and well short of a wild value.
   A cell costs FOUR SHORTS (s_inst); as six vertices of six floats, the same
   ceiling would be 432 MB. */
#define INST_MAX_BYTES  (24u * 1024u * 1024u)
#define INST_MAX_CELLS  ((int)(INST_MAX_BYTES / (ICOMP * sizeof(short))))
#define TERR_ENC     0.10f                      /* under every other band      */
#define DEFAULT_KEY  254                        /* see tagpu_terrown.c         */

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

typedef void (APIENTRY *PFN_DRAWARRAYSINST)(GLenum,GLint,GLsizei,GLsizei);
typedef void (APIENTRY *PFN_ATTRIBDIVISOR)(GLuint,GLuint);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM3F)(GLint,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_GETINTEGERV)(GLenum,GLint*);
static PFN_DRAWARRAYSINST x_glDrawArraysInstanced;
static PFN_ATTRIBDIVISOR  x_glVertexAttribDivisor;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_UNIFORM3F  x_glUniform3f;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_GETINTEGERV x_glGetIntegerv;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

/* ---- arming ---- */
static int s_armed = -1;
static int s_log = 0, s_passive = 0, s_over = 0, s_key = DEFAULT_KEY;
static unsigned s_armCheck = 0;

int tagpu_terr_key(void) { return s_key; }

int tagpu_terr_armed(unsigned frame_counter)
{
    int was;
    char buf[128];
    int n;
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    was = s_armed;
    s_armed = 0;
    n = tagpu_opt_read("tagpu_terr.on", buf, sizeof buf);
    if (n < 0) {
        tagpu_terrown_set_skip(0);
        if (was > 0) flog("terr: disarmed");
        return 0;
    }
    {
        int wasKey = s_key;
        s_log = 0; s_passive = 0; s_over = 0; s_key = DEFAULT_KEY;
        if (n > 0) {
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
                else if (!lstrcmpiA(p, "over")) s_over = 1;
                else if (!strncmp(p, "key=", 4)) {
                    int k = atoi(p + 4);
                    /* index 0 is the fog's own solid black — never a free key */
                    if (k > 0 && k < 256) s_key = k;
                }
                if (last) break;
                p = q + 1;
            }
        }
        /* a live key change would leave one frame filled with the old index
           while the composite inverts against the new one — hand the draw back
           for a frame instead */
        if (s_key != wasKey) tagpu_terrown_set_skip(0);
    }
    s_armed = 1;
    if (s_passive || s_over) tagpu_terrown_set_skip(0);
    if (was != 1) {
        char b[160];
        _snprintf(b, sizeof b, "terr: ARMED (log=%d passive=%d over=%d key=%d)",
                  s_log, s_passive, s_over, s_key);
        flog(b);
    }
    return 1;
}

int tagpu_terr_on(void) { return s_armed > 0; }

/* ---- GL ---- */
static int    s_state = 0;             /* 0 unloaded, 1 ready, 2 failed       */
static GLuint s_prog, s_vao, s_vbo, s_qvbo, s_atlasTex;
static GLint  s_uGame, s_uFog, s_uFogOrg, s_uFogDim, s_uZoom, s_uZoomC,
              s_uDepthScale, s_uEnc, s_uOrigin, s_uTile0, s_uTexel;
static int    s_atlasH, s_atlasN;      /* atlas rows*CELL_PITCH, tiles held   */
static const void* s_setPtr;           /* the TILE_SET we built from          */
static int    s_setCount;
static int    s_maxTex;
/* Classic++ (tagpu_classicpp.on): the RESTORED copy of the atlas -- the same
   cells on the same pitch, true colour from the unditherer's model run as
   fragment passes by tagpu_restoreglsl.c straight into this texture -- so the
   one set of UVs serves both looks. Built once per map, a slice per frame,
   and the cells SHOW AS THEY LAND (renderers.md 4c Q6): the restorer clears
   the texture to alpha 0 when the job starts and its out pass writes alpha 1
   over every cell it paints, guard ring included, so the shader's alpha test
   is the per-cell flag -- no second texture, no upload, and a cell's samples
   are all-or-nothing because one quad paints its interior and its ring.
   Draws issued in the same frame are in order, so a cell whose out pass was
   issued by this frame's slice is restored in this frame's terrain draw. */
static GLuint s_rgbTex;
static int    s_rgbState = 0;      /* 0 none, 1 restoring, 2 complete, -1 failed */
static unsigned s_rgbPalSerial;    /* tagpu_pal serial s_rgbTex was restored through */
static TAGPU_RGLSL_JOB* s_job;     /* the restorer's job while state is 1     */
static GLint  s_uRestored;
static const unsigned char* s_setPix;   /* the current set's tile pixels     */
/* the last gathered rect, in 32-px cells: the GLSL job restores the tiles
   under it first (renderers.md 4c Q6), so it starts one frame after the atlas */
static int    s_rectTx0, s_rectTy0, s_rectCols, s_rectRows, s_rectValid;
/* Classic++ lighting: the engine's height grid as one R8 texel per 16-px
   cell, built with the atlas (once per map, from FeatureStruct+4 -- the byte
   tagpu_feat.c reads per anchor) so the fragment shader can take the lab's
   normal at any point of the map without a vertex stream growing: the
   heightfield normal is per fragment here, from the grid, where the lab
   computes it per vertex on 16-px sub-quads -- 4x the terrain vertices,
   29 MB a frame at the zoom floor, for the same field. Unit 5. */
static GLuint s_hTex;
static int    s_hW, s_hH;              /* 0 while there is no usable grid    */
static const void* s_hGrid;            /* the inputs the texture was built  */
static const void* s_hSet;             /* from, or last attempted from      */
static unsigned s_hFrame;              /* the frame of that attempt          */
static GLint  s_uHDim, s_uLit, s_uLambert, s_uSun, s_uAmb, s_uNorm;
static GLuint s_hVao, s_hVbo, s_hIbo;  /* the heightfield caster mesh (G14i)  */
static int    s_hMeshW, s_hMeshH;      /* the grid it was built from: a failed
                                          rebuild leaves the old mesh, and this
                                          is what keeps it undrawn (review) */
static TAGPU_SHADOWU s_shU;            /* the shadow read-back uniforms      */

/* THIS FRAME'S CELLS, one record each: the cell's column and row in the
   gather's own grid, then its tile's column and row in the atlas. Everything
   the six vertices used to carry is rebuilt from these four shorts by the
   vertex shader below, which is why a whole 4K view fits in a megabyte.
   GROWN FROM THE VIEWPORT (terr_reserve, through tagpu_terr_clamp_span) and
   never shrunk: a resolution change reserves once and every frame after it
   costs nothing. */
static short* s_inst;
static int    s_instCells;             /* what s_inst can hold                */
static int    s_ncell;                 /* what this frame put in it           */
/* what the shader needs to rebuild them: the screen point grid cell (0,0)'s
   top-left corner sits on, and the atlas's texel size. The map cell it is,
   the other half of the rebuild, is s_rectTx0/s_rectTy0. */
static float s_origX, s_origY, s_iw, s_ih;
/* The shader spells CELL_PITCH, CELL_BORDER and TILE_PX as literals — GLSL
   cannot see a C macro — so a change to any of the three must not silently
   leave the UVs a texel out. This is that change refusing to compile. */
typedef char terr_atlas_consts_unchanged[
    (CELL_PITCH == 34 && CELL_BORDER == 1 && TILE_PX == 32) ? 1 : -1];

/* ONE QUAD PER VISIBLE CELL, AND THE CELL IS AN INSTANCE. `aCorner` is the
   unit quad's six corners in the engine's own vertex order — one static buffer
   uploaded at init and never touched again — and `aCell` is this frame's four
   shorts for the cell, per instance.

   The three values the per-vertex stream used to carry are rebuilt here, and
   rebuilt EXACTLY: every term is an integer far below 2^24 — a grid column is
   at most 2052 (the 16384-px viewport tagpu_native.c will believe, at the
   0.25x zoom floor, over 32), a map cell at most 2047 (`mapW16 <= 4096`, and
   the tile map's stride is half that), an atlas column 63 and an atlas row
   1023 — so each product and sum is exact in float and aPos, aUV and aWorld
   are bit for bit the floats the CPU used to write. Those same bounds are
   what puts every field of aCell inside a signed short. The atlas texel size arrives as the same
   `uTexel` float the CPU used to multiply by, so the UVs are the same product
   of the same two operands. What changes is only the cost — four shorts a
   cell against six vertices of six floats — and that is what lets one frame's
   budget cover a 3840x2160 view at the zoom floor. */
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aCorner;\n"   /* per vertex: 0/1 x 0/1        */
    "layout(location=1) in vec4 aCell;\n"     /* per instance: col,row,cx,cy  */
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "uniform float uEnc;\n"
    "uniform vec2 uOrigin;\n"   /* screen px of grid cell (0,0)'s corner      */
    "uniform vec2 uTile0;\n"    /* the map cell grid cell (0,0) IS            */
    "uniform vec2 uTexel;\n"    /* 1/ATLAS_W, 1/atlas height                  */
    "out vec2 vUV; out vec2 vWorld;\n"
    "void main(){\n"
    /* CELL_PITCH 34, CELL_BORDER 1, TILE_PX 32 — held to those values by
       terr_atlas_consts_unchanged in tagpu_terr.c */
    "  vec2 g = aCell.xy + aCorner;\n"
    "  vec2 aPos = uOrigin + g * 32.0;\n"
    "  vec2 aWorld = (uTile0 + g) * 32.0;\n"
    "  vec2 aUV = (aCell.zw * 34.0 + 1.0 + aCorner * 32.0) * uTexel;\n"
    "  vec2 p = (aPos - uZoomC) * uZoom + uZoomC - vec2(" TAGPU_EDGE_NUDGE ");\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - uEnc/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vWorld = aWorld;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; in vec2 vWorld;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uPal;\n"
    "uniform sampler2D uAtlasRGB;\n"   /* Classic++: the restored atlas    */
    "uniform int uRestored;\n"         /* 1 = a restore is running or done:
                                          sample it where its alpha says so */
    "uniform sampler2D uHeight;\n"     /* Classic++: R8 height per 16-px cell */
    "uniform vec2 uHDim;\n"            /* its size: mapW16, mapH16; 0 = none */
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_FOG_FN
    TAGPU_GLSL_LIGHT_UNIFORMS
    TAGPU_GLSL_SHADOW_UNIFORMS
    TAGPU_GLSL_LIGHT_FN
    /* the lab's normalAt (tascene-view.html): at a grid point, central
       differences of the height over 32 world units, coordinates clamped to
       the map; elevation and x/z are the same world units. The height is the
       engine's byte, so `* 255.0` gives it back exactly. */
    "float taH(ivec2 p){\n"
    "  p = clamp(p, ivec2(0), ivec2(uHDim) - 1);\n"
    "  return texelFetch(uHeight, p, 0).r * 255.0;\n"
    "}\n"
    "vec3 taGridN(ivec2 p){\n"
    "  float dx = (taH(p + ivec2(1, 0)) - taH(p - ivec2(1, 0))) * (1.0/32.0);\n"
    "  float dz = (taH(p + ivec2(0, 1)) - taH(p - ivec2(0, 1))) * (1.0/32.0);\n"
    "  return normalize(vec3(-dx, 1.0, -dz));\n"
    "}\n"
    /* the lab's terrain normal AT THIS FRAGMENT: the four grid-point normals
       of the 16-px cell it is in, interpolated exactly as the lab's two
       triangles per cell interpolate them (the diagonal runs (1,0)-(0,1)),
       so the field is the one the owner looked at, not a bilinear cousin */
    "vec3 taTerrN(vec2 w){\n"
    "  vec2 g = w * (1.0/16.0);\n"
    "  vec2 f = floor(g), t = g - f;\n"
    "  ivec2 c = ivec2(f);\n"
    "  vec3 n00 = taGridN(c), n10 = taGridN(c + ivec2(1, 0));\n"
    "  vec3 n01 = taGridN(c + ivec2(0, 1)), n11 = taGridN(c + ivec2(1, 1));\n"
    "  return t.x + t.y <= 1.0\n"
    "    ? n00 + t.x * (n10 - n00) + t.y * (n01 - n00)\n"
    "    : n11 + (1.0 - t.x) * (n01 - n11) + (1.0 - t.y) * (n10 - n11);\n"
    "}\n"
    /* the world point THIS FRAGMENT depicts -- (x, h, z + h/2), the lab's
       terrain vertex (tascene-view.html buildTerrainLab), h the height at
       the fragment over the same two triangles -- and its screen derivatives
       for the shadow's receiver-plane bias. The derivatives are ANALYTIC:
       the height is reconstructed here from the grid, so a dFdx() of it
       would jump at every cell edge (the lab interpolates the point as a
       varying, whose derivative is exact per triangle); the gradient of the
       triangle the fragment is in, times the derivative of vWorld, which IS
       a varying, is the same clean number. */
    "vec3 taTerrW(vec2 w, out vec3 dWdx, out vec3 dWdy){\n"
    "  vec2 g = w * (1.0/16.0);\n"
    "  vec2 f = floor(g), t = g - f;\n"
    "  ivec2 c = ivec2(f);\n"
    "  float h00 = taH(c), h10 = taH(c + ivec2(1, 0));\n"
    "  float h01 = taH(c + ivec2(0, 1)), h11 = taH(c + ivec2(1, 1));\n"
    "  float h, gx, gz;\n"
    "  if (t.x + t.y <= 1.0) {\n"
    "    h = h00 + t.x * (h10 - h00) + t.y * (h01 - h00);\n"
    "    gx = (h10 - h00) * (1.0/16.0); gz = (h01 - h00) * (1.0/16.0);\n"
    "  } else {\n"
    "    h = h11 + (1.0 - t.x) * (h01 - h11) + (1.0 - t.y) * (h10 - h11);\n"
    "    gx = (h11 - h01) * (1.0/16.0); gz = (h11 - h10) * (1.0/16.0);\n"
    "  }\n"
    "  vec2 sx = dFdx(w), sy = dFdy(w);\n"
    "  float hx = gx * sx.x + gz * sx.y, hy = gx * sy.x + gz * sy.y;\n"
    "  dWdx = vec3(sx.x, hx, sx.y + 0.5 * hx);\n"
    "  dWdy = vec3(sy.x, hy, sy.y + 0.5 * hy);\n"
    "  return vec3(w.x, h, w.y + 0.5 * h);\n"
    "}\n"
    "void main(){\n"
    /* the depicted world point and its derivatives FIRST, while the flow is
       still uniform (tagpu_glsl.h, the shadow half) */
    "  vec3 taWx = vec3(0.0), taWy = vec3(0.0);\n"
    "  vec3 taW = uHDim.x > 0.5 ? taTerrW(vWorld, taWx, taWy) : vec3(vWorld.x, 0.0, vWorld.y);\n"
    /* terrain is the bottom layer: it paints the fog's black instead of
       discarding, and it darkens (never hides) in grey — the engine's rule */
    TAGPU_GLSL_FOG_TERRAIN
    /* Classic++ (uLit): the restored colour where the reveal has painted it
       -- alpha is the restorer's own "painted" mark (see s_rgbTex), a cell it
       has not reached yet is alpha 0 -- and the palette's colour elsewhere,
       so the reveal goes lit-indexed to lit-restored; lit by the lab's rule
       from the heightfield normal; then the grey band as the RGB rule
       (renderers.md 2.6) rather than the index LUT. Nothing below this
       branch runs under Classic++, nothing in it runs under Classic. */
    "  if (uLit == 1) {\n"
    "    vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n"
    "    vec3 c = t.a > 0.5 ? t.rgb\n"
    "           : texelFetch(uPal, ivec2(int(texture(uAtlas, vUV).r * 255.0 + 0.5), 0), 0).rgb;\n"
    "    if (uHDim.x > 0.5) c *= taLambert(uLambert == 1 ? taTerrN(vWorld) : vec3(0.0, 1.0, 0.0),\n"
    "                                     taW, taWx, taWy);\n"
    TAGPU_GLSL_FOG_GREY_RGB("c")
    "    frag = vec4(c, 1.0); return;\n"
    "  }\n"
    /* NEAREST on an R8 atlas: the texel IS the palette index, and the quad
       spans exactly the tile's 32 texels, so no colour key and no filtering to
       get wrong. A fragment landing exactly on the far edge reads the cell's
       replicated guard texel rather than the next cell (CELL_PITCH). */
    "  int pi = int(texture(uAtlas, vUV).r * 255.0 + 0.5);\n"
    TAGPU_GLSL_FOG_SHADE("pi")
    "  frag = vec4(texelFetch(uPal, ivec2(pi, 0), 0).rgb, 1.0);\n"
    "}\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
               flog("terr: shader FAILED:"); flog(lg); s_state = 2; }
    return sh;
}

static void init_gl(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    /* Instanced drawing is GL 3.1 and the divisor GL 3.3, both core in any
       context that can compile the `#version 330 core` shaders below — so a
       device missing them cannot run this pass at all, and the same refusal
       the missing-proc test already applies is the right answer. */
    x_glDrawArraysInstanced = (PFN_DRAWARRAYSINST)getgl("glDrawArraysInstanced");
    x_glVertexAttribDivisor = (PFN_ATTRIBDIVISOR)getgl("glVertexAttribDivisor");
    x_glUniform1f  = (PFN_UNIFORM1F) getgl("glUniform1f");
    x_glUniform2f  = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glUniform3f  = (PFN_UNIFORM3F) getgl("glUniform3f");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glGetIntegerv = (PFN_GETINTEGERV)getgl("glGetIntegerv");
    if (!x_glUniform1f || !x_glUniform2f || !x_glUniform3f ||
        !x_glActiveTexture || !x_glDrawArraysInstanced || !x_glVertexAttribDivisor) {
        flog("terr: missing GL proc"); s_state = 2; return;
    }
    vs = mksh(GL_VERTEX_SHADER, VS); fs = mksh(GL_FRAGMENT_SHADER, FS);
    if (s_state == 2) return;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs); glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { flog("terr: link FAILED"); s_state = 2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uGame = glGetUniformLocation(s_prog, "uGame");
    s_uFog  = glGetUniformLocation(s_prog, "uFog");
    s_uFogOrg = glGetUniformLocation(s_prog, "uFogOrg");
    s_uFogDim = glGetUniformLocation(s_prog, "uFogDim");
    s_uZoom = glGetUniformLocation(s_prog, "uZoom");
    s_uZoomC = glGetUniformLocation(s_prog, "uZoomC");
    s_uDepthScale = glGetUniformLocation(s_prog, "uDepthScale");
    s_uEnc = glGetUniformLocation(s_prog, "uEnc");
    s_uOrigin = glGetUniformLocation(s_prog, "uOrigin");
    s_uTile0 = glGetUniformLocation(s_prog, "uTile0");
    s_uTexel = glGetUniformLocation(s_prog, "uTexel");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "uPal"),   1);
    glUniform1i(glGetUniformLocation(s_prog, "uFogGrid"), 2);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlasRGB"), 4);
    s_uRestored = glGetUniformLocation(s_prog, "uRestored");
    glUniform1i(glGetUniformLocation(s_prog, "uFogLUT"),  3);
    glUniform1i(glGetUniformLocation(s_prog, "uHeight"),  5);
    s_uHDim = glGetUniformLocation(s_prog, "uHDim");
    s_uLit  = glGetUniformLocation(s_prog, "uLit");
    s_uLambert = glGetUniformLocation(s_prog, "uLambert");
    s_uSun  = glGetUniformLocation(s_prog, "uSun");
    s_uAmb  = glGetUniformLocation(s_prog, "uAmb");
    s_uNorm = glGetUniformLocation(s_prog, "uNorm");
    tagpu_shadow_locate(s_prog, &s_shU);
    glUseProgram(0);

    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    /* the unit quad, in the engine's own vertex order: two triangles whose
       shared edge runs (1,0)-(0,1), exactly the six corners the per-vertex
       gather used to write out per cell */
    {
        static const GLfloat corners[12] = { 0,0, 1,0, 0,1, 1,0, 1,1, 0,1 };
        glGenBuffers(1, &s_qvbo); glBindBuffer(GL_ARRAY_BUFFER, s_qvbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof corners, corners,
                     GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
    }
    glGenBuffers(1, &s_vbo); glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* no storage yet: the per-frame upload in tagpu_terr_render re-specifies it
       at this frame's size, and the attribute below only records the binding */
    glEnableVertexAttribArray(1);
    /* GL_SHORT, UNNORMALISED: every field is a small integer, so the fixed
       conversion to float is exact and the shader gets the same numbers the
       gather wrote. Normalising here would divide them all by 32767. */
    glVertexAttribPointer(1, 4, GL_SHORT, GL_FALSE, ICOMP * 2, (void*)0);
    x_glVertexAttribDivisor(1, 1);
    glBindVertexArray(0);

    s_maxTex = 0;
    if (x_glGetIntegerv) {
        GLint m = 0;
        x_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m);
        s_maxTex = (int)m;
    }
    /* Only a FAILED query gets the fallback. 4096 is the conservative floor GL
       3.3 hardware always beats — 120 atlas rows on the 34-texel pitch, 7680
       tiles. This used to read `< ATLAS_W`, which was harmless while ATLAS_W
       was 2048 and a device answering exactly 2048 passed; at 2176 that same
       honest answer would be overwritten with 4096 and we would then hand
       glTexImage2D a width the driver rejects, leaving the atlas storageless —
       and with the composite inverted that is a BLACK viewport, not a missing
       texture. So a device that genuinely cannot hold the atlas is refused
       here instead, once, and the engine keeps its own terrain pass. */
    if (s_maxTex <= 0) s_maxTex = 4096;
    if (s_maxTex < ATLAS_W) {
        char b[128];
        _snprintf(b, sizeof b, "terr: GL_MAX_TEXTURE_SIZE %d < atlas width %d —"
                               " terrain stays the engine's", s_maxTex, ATLAS_W);
        flog(b);
        s_state = 2;
        return;
    }

    /* s_atlasTex = 0 is what forces the rebuild (ensure_atlas tests it first);
       the set identity must SURVIVE, or ensure_atlas cannot tell "same set, new
       context" from "new map" and throws the restore away -- see glreset */
    s_atlasTex = 0;
    s_state = 1;
    flog("terr: GL ready");
}

void tagpu_terr_glreset(void)
{
    s_state = 0;
    s_atlasTex = 0;                     /* the id died with the context */
    s_rgbTex = 0;
    s_hTex = 0;                         /* the id died; ensure_height rebuilds */
    s_hVao = s_hVbo = s_hIbo = 0;       /* ...and the caster mesh with it      */
    s_hMeshW = s_hMeshH = 0;
    s_hW = s_hH = 0; s_hGrid = NULL; s_hFrame = 0;
    s_rectValid = 0;
    /* The set identity (s_setPtr/s_setCount/s_setPix) is LEFT ALONE: zeroing
       s_atlasTex is what forces the atlas rebuild, and the identity's job is to
       tell a NEW MAP from the same set (ensure_atlas aborts a running restore
       on a new map). The restore itself does not survive a reset: its result
       lived only in s_rgbTex, which died with the context, so it is run again
       (two seconds, on the GPU) rather than kept as a 23 MB copy. Until
       2026-09-05 the ONNX path kept its CPU result here and re-uploaded it.
       The job is already gone: tagpu_native_glreset resets the restorer first. */
    s_job = NULL;
    s_rgbState = 0;
}

/* ---- the height grid: one R8 texel per 16-px cell, once per map ----
   Keyed on ITS OWN inputs -- the grid pointer, the dims, and the tile set the
   atlas was built from (LoadMap could hand a same-sized map the same
   allocation, and the set's identity is what this module already trusts to
   say "new map") -- and re-checked every frame by ensure_height, because the
   set can be ready a frame before the grid is. A build that fails leaves
   s_hW 0, which is what the render gates the lambert on (never s_hTex: a
   previous map's texture is still a live id), and is retried every 60 frames
   until it succeeds or the inputs change. Without a grid Classic++ terrain
   draws UNLIT -- the restored colour and the grey rule stay, only the
   lambert is skipped (uHDim 0 in the shader). */
static void build_hills(const unsigned char* buf, int w, int h);
static void build_height(const char* ta, unsigned frame)
{
    const char* grid = *(const char* const*)(ta + OFF_FEATMAP);
    int w = *(const int*)(ta + OFF_MAPW16), h = *(const int*)(ta + OFF_MAPH16);
    unsigned char* buf;
    int r, c;
    char b[160];
    s_hW = s_hH = 0;
    s_hGrid = grid; s_hSet = s_setPtr; s_hFrame = frame;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || w > s_maxTex || h > s_maxTex) {
        flog("terr: height grid dims out of range -- Classic++ terrain draws unlit");
        return;
    }
    if (!ptr_ok(grid) || IsBadReadPtr(grid, (SIZE_T)w * (SIZE_T)h * FT_STRIDE)) {
        flog("terr: height grid unreadable -- Classic++ terrain draws unlit (retried)");
        return;
    }
    buf = (unsigned char*)malloc((size_t)w * (size_t)h);
    if (!buf) return;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            buf[(size_t)r * w + c] =
                *(const unsigned char*)(grid + ((size_t)r * w + c) * FT_STRIDE + FT_HEIGHT);
    if (!s_hTex) {
        glGenTextures(1, &s_hTex);
        glBindTexture(GL_TEXTURE_2D, s_hTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else glBindTexture(GL_TEXTURE_2D, s_hTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    build_hills(buf, w, h);          /* TODO: unconditional -- 19 MB even when
                                        terrainshadow=0, which is the default.
                                        See build_hills' header. */
    free(buf);
    s_hW = w; s_hH = h;
    _snprintf(b, sizeof b, "terr: height grid %dx%d uploaded from %p (Classic++ lighting)",
              w, h, (const void*)grid);
    flog(b);
}

/* ---- the heightfield as a caster (G14i, renderers.md 2.8, 2.12) ----
   One vertex per grid point at the world point the lab's terrain vertex
   depicts -- (c*16, h, r*16 + h/2) -- and two triangles per cell on the
   diagonal taTerrN interpolates across ((1,0)-(0,1)), indices ordered by
   cell row so the rows under the light window are one contiguous range.
   Static: the map's heights never change. Two Continents: 537,600 vertices
   (6.4 MB), 3.2 M indices (12.9 MB), once per map.

   ==== TODO (IMPORTANT), 2026-09-09: 19 MB of VRAM per map for a pass that is
   OFF BY DEFAULT and normally never draws. ====
   `terrainshadow` now defaults to 0 (tagpu_classicpp.c shadow_defaults --
   the ground self-shadowed itself, renderers.md 2.7b), and the only caller of
   tagpu_terr_hills_draw is gated on it (tagpu_shadow.c:398). This function is
   NOT gated: build_height calls it unconditionally, so every map pays 6.4 MB
   of vertices and 12.9 MB of indices that nothing reads.

   Do NOT fix it by gating the build on the flag. The flag is live -- the cfg
   is re-read while the game runs (read_cfg, and the render-options screen
   triggers it) -- so `terrainshadow=1` mid-session must still produce a mesh,
   and that is the fixture the eventual shadow fix gets measured in.

   Build it LAZILY instead, on the first tagpu_terr_hills_draw after the grid
   changed. The obstacle is that `buf` is freed at the end of build_height, so
   the lazy path needs the bytes: either keep that w*h byte buffer alive (0.5 MB
   on Two Continents, 3 % of what it replaces) or re-read the engine grid at
   OFF_FEATMAP with the same ptr_ok/IsBadReadPtr guard build_height uses. Keep
   the existing s_hMeshW/s_hMeshH == s_hW/s_hH check in the draw so a failed
   rebuild still refuses rather than indexing past the old mesh. */
static void build_hills(const unsigned char* buf, int w, int h)
{
    size_t nv = (size_t)w * (size_t)h, ni = (size_t)(w - 1) * (size_t)(h - 1) * 6, k = 0;
    float* vb; unsigned* ib;
    int r, c;
    char b[160];
    if (w < 2 || h < 2) return;
    vb = (float*)malloc(nv * 3 * sizeof(float));
    ib = (unsigned*)malloc(ni * sizeof(unsigned));
    if (!vb || !ib) { free(vb); free(ib); flog("terr: hills: out of memory"); return; }
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            float hh = (float)buf[(size_t)r * w + c];
            float* o = vb + ((size_t)r * w + c) * 3;
            o[0] = (float)(c * 16); o[1] = hh; o[2] = (float)(r * 16) + hh * 0.5f;
        }
    for (r = 0; r < h - 1; r++)
        for (c = 0; c < w - 1; c++) {
            unsigned i00 = (unsigned)(r * w + c), i10 = i00 + 1u;
            unsigned i01 = i00 + (unsigned)w, i11 = i01 + 1u;
            ib[k++] = i00; ib[k++] = i10; ib[k++] = i01;
            ib[k++] = i11; ib[k++] = i01; ib[k++] = i10;
        }
    if (!s_hVao) {
        glGenVertexArrays(1, &s_hVao);
        glGenBuffers(1, &s_hVbo);
        glGenBuffers(1, &s_hIbo);
    }
    glBindVertexArray(s_hVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_hVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * 3 * sizeof(float)), vb, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void*)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_hIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(ni * sizeof(unsigned)), ib, GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    free(vb); free(ib);
    s_hMeshW = w; s_hMeshH = h;
    _snprintf(b, sizeof b, "terr: hills mesh %dx%d grid points, %u cells (Classic++ shadows)",
              w, h, (unsigned)((w - 1) * (h - 1)));
    flog(b);
}

int tagpu_terr_hills_draw(int r0, int r1)
{
    int cells = s_hMeshW - 1, rows = s_hMeshH - 1;
    /* only the mesh built from THIS grid: after a map change whose rebuild
       failed (too small, out of memory) the old mesh is still bound and
       the new grid's size would index past it */
    if (!s_hVao || s_hMeshW < 2 || s_hMeshH < 2) return 0;
    if (s_hMeshW != s_hW || s_hMeshH != s_hH) return 0;
    if (r0 < 0) r0 = 0;
    if (r1 > rows - 1) r1 = rows - 1;
    if (r1 < r0) return 0;
    glBindVertexArray(s_hVao);
    glDrawElements(GL_TRIANGLES, (GLsizei)((r1 - r0 + 1) * cells * 6), GL_UNSIGNED_INT,
                   (const void*)(size_t)((size_t)r0 * (size_t)cells * 6u * 4u));
    glBindVertexArray(0);
    return 1;
}

/* once per frame after the atlas is known: (re)build when the inputs moved,
   or when the last attempt failed and 60 frames have passed */
static void ensure_height(const char* ta, unsigned frame)
{
    const char* grid = *(const char* const*)(ta + OFF_FEATMAP);
    int w = *(const int*)(ta + OFF_MAPW16), h = *(const int*)(ta + OFF_MAPH16);
    int same = s_hGrid == (const void*)grid && s_hSet == s_setPtr;
    if (s_hW > 0 && same && s_hW == w && s_hH == h) return;
    if (s_hW == 0 && same && s_hFrame != 0 && frame - s_hFrame < 60) return;
    build_height(ta, frame);
}

/* ---- the atlas: built once per map, never per frame ---- */
static int ensure_atlas(const char* ta)
{
    const int* set = *(const int* const*)(ta + OFF_TILESET);
    const unsigned char* pix;
    unsigned char* buf;
    int count, rows, h, i;
    char b[192];

    if (!ptr_ok(set) || IsBadReadPtr((void*)set, 8)) return 0;
    count = set[0];
    pix = (const unsigned char*)(size_t)(unsigned)set[1];
    if (count <= 0 || count > MAX_TILES) return 0;
    /* the identity test FIRST: the probe below walks megabytes of tile art and
       this runs every frame */
    if (s_atlasTex && s_setPtr == (const void*)set && s_setCount == count) return 1;
    if (!ptr_ok(pix) || IsBadReadPtr((void*)pix, (SIZE_T)count * TILE_BYTES)) return 0;

    rows = (count + ATLAS_COLS - 1) / ATLAS_COLS;
    h = rows * CELL_PITCH;
    if (h > s_maxTex) {                 /* keep what fits; the rest draw black */
        rows = s_maxTex / CELL_PITCH;
        h = rows * CELL_PITCH;
        _snprintf(b, sizeof b, "terr: tile set %d exceeds the atlas (%dx%d max) — %d kept",
                  count, ATLAS_W, s_maxTex, rows * ATLAS_COLS);
        flog(b);
    }
    buf = (unsigned char*)calloc((size_t)ATLAS_W * (size_t)h, 1);
    if (!buf) { flog("terr: atlas alloc failed"); return 0; }
    for (i = 0; i < count && i < rows * ATLAS_COLS; i++) {
        const unsigned char* src = pix + (size_t)i * TILE_BYTES;
        unsigned char* cell = buf + (size_t)(i / ATLAS_COLS) * CELL_PITCH * ATLAS_W
                                  + (size_t)(i % ATLAS_COLS) * CELL_PITCH;
        unsigned char* dst = cell + (size_t)CELL_BORDER * ATLAS_W + CELL_BORDER;
        int r;
        for (r = 0; r < TILE_PX; r++) {
            unsigned char* row = dst + (size_t)r * ATLAS_W;
            memcpy(row, src + (size_t)r * TILE_PX, TILE_PX);
            row[-1] = row[0];                       /* left  border column */
            row[TILE_PX] = row[TILE_PX - 1];        /* right border column */
        }
        memcpy(cell, dst - CELL_BORDER, CELL_PITCH);                    /* top    */
        memcpy(cell + (size_t)(CELL_PITCH - 1) * ATLAS_W,               /* bottom */
               dst + (size_t)(TILE_PX - 1) * ATLAS_W - CELL_BORDER, CELL_PITCH);
    }
    if (!s_atlasTex) {
        glGenTextures(1, &s_atlasTex);
        glBindTexture(GL_TEXTURE_2D, s_atlasTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* always re-spec rather than sub-image: no init flag can then survive a
       context reset and leave the texture storageless, which reads as 0 in
       every sample (the failure that cost G13c an hour) */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, h, 0, GL_RED, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(buf);

    s_atlasH = h;
    s_atlasN = count < rows * ATLAS_COLS ? count : rows * ATLAS_COLS;
    /* a different set is a new map: drop a restore still running on the old
       one and start over. (A GL reset reaches here with the SAME set, after
       glreset has already reset the restore, so both calls are no-ops then.) */
    if (s_setPtr != (const void*)set || s_setCount != count || s_setPix != pix) {
        if (s_job) { tagpu_rglsl_job_free(s_job); s_job = NULL; }
        s_rgbState = 0;
    }
    s_setPtr = (const void*)set; s_setCount = count; s_setPix = pix;
    _snprintf(b, sizeof b, "terr: atlas built %dx%d for %d tiles (set=%p pix=%p, %d KB)",
              ATLAS_W, h, count, (void*)set, (void*)pix, (count * TILE_BYTES) >> 10);
    flog(b);
    return 1;
}

/* ---- Classic++: the GLSL restorer, straight into s_rgbTex ----
   Every tile of the set as a frame, visible ones first, and since the cells
   show as they land (s_rgbTex) the order is what the player watches: a tile's
   rank is the Chebyshev distance in cells from the CENTRE of the last gathered
   rect to the nearest map cell that uses it, so the reveal radiates from the
   middle of the screen, reaches the viewport's edge at rank ~half its span and
   carries on outward across the map at the same pace. (Ranking the whole rect
   0, as the one-flip version did, restored the visible cells in tile-index
   order -- a scatter.) The set holds every tile the map references and nothing
   else, so no tile is left unranked, but an unreferenced one would simply go
   last. */
static int* restore_order(const char* ta, int count)
{
    const unsigned short* tmap = *(const unsigned short* const*)(ta + OFF_TILEMAP);
    int mapW16 = *(const int*)(ta + OFF_MAPW16), mapH16 = *(const int*)(ta + OFF_MAPH16);
    int stride = mapW16 / 2, mrows = mapH16 / 2, my, mx, i, maxRank = 0, *rank, *order, *bucket, *next;
    int cx = s_rectTx0 + s_rectCols / 2, cy = s_rectTy0 + s_rectRows / 2;
    if (!ptr_ok(tmap) || stride <= 0 || mrows <= 0 || stride > 2048 || mrows > 2048) return NULL;
    rank = (int*)malloc((size_t)count * sizeof *rank);
    order = (int*)malloc((size_t)count * sizeof *order);
    if (!rank || !order) { free(rank); free(order); return NULL; }
    for (i = 0; i < count; i++) rank[i] = 8192;
    for (my = 0; my < mrows; my++) {
        int dy = my < cy ? cy - my : my - cy;
        for (mx = 0; mx < stride; mx++) {
            int dx = mx < cx ? cx - mx : mx - cx;
            int d = dx > dy ? dx : dy, idx = tmap[(size_t)my * stride + mx];
            if (idx < count && d < rank[idx]) rank[idx] = d;
        }
    }
    for (i = 0; i < count; i++) if (rank[i] > maxRank) maxRank = rank[i];
    /* counting sort by rank, stable in tile order */
    bucket = (int*)calloc((size_t)maxRank + 2, sizeof *bucket);
    next = (int*)calloc((size_t)maxRank + 2, sizeof *next);
    if (!bucket || !next) { free(bucket); free(next); free(rank); free(order); return NULL; }
    for (i = 0; i < count; i++) bucket[rank[i] + 1]++;
    for (i = 1; i <= maxRank + 1; i++) bucket[i] += bucket[i - 1];
    for (i = 0; i < count; i++) order[bucket[rank[i]] + next[rank[i]]++] = i;
    free(bucket); free(next); free(rank);
    return order;
}

/* `repaint`: the atlas is already restored and only the palette moved, so the
   destination keeps what it holds and every tile is queued again over it —
   the terrain recolours centre-out instead of blanking for the 2+ seconds the
   job takes. */
static int glsl_begin(const char* ta, int repaint)
{
    const unsigned char* pal = tagpu_pal_live();
    /* the ART's palette for the tileability test, the SCREEN's for the restore
       itself -- tagpu_pal.h, and tagpu_gaf.c's atlas_insert has the numbers */
    const unsigned char* art = tagpu_pal_engine();
    int rows = s_atlasH / CELL_PITCH, n = s_atlasN, i, ok;
    int* order;
    TAGPU_RGLSL_FRAME* frames;
    (void)rows;
    order = restore_order(ta, n);
    frames = (TAGPU_RGLSL_FRAME*)malloc((size_t)n * sizeof *frames);
    if (!order || !frames) { free(order); free(frames); flog("terr: restore order alloc failed"); return 0; }
    for (i = 0; i < n; i++) {
        int t = order[i];
        TAGPU_RGLSL_FRAME* f = &frames[i];
        f->ax = f->dx = (t % ATLAS_COLS) * CELL_PITCH + CELL_BORDER;
        f->ay = f->dy = (t / ATLAS_COLS) * CELL_PITCH + CELL_BORDER;
        f->w = f->h = TILE_PX; f->border = CELL_BORDER; f->key = -1;   /* tiles are opaque */
        f->padR = f->padB = 0;                                          /* no alignment slack */
        f->wrap = art ? tagpu_rglsl_tileable(s_setPix + (size_t)t * TILE_BYTES, TILE_PX, TILE_PX, art, -1) : 0;
    }
    free(order);
    /* the destination, re-specified per map: the same layout as s_atlasTex */
    if (!s_rgbTex) {
        glGenTextures(1, &s_rgbTex);
        glBindTexture(GL_TEXTURE_2D, s_rgbTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else glBindTexture(GL_TEXTURE_2D, s_rgbTex);
    if (!repaint)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_W, s_atlasH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    /* a one-shot job at the head of the queue: the terrain restores before
       any GAF atlas, and its done line is the restore's measurement */
    s_job = repaint
        ? tagpu_rglsl_job_repaint("terr", 0, 1, s_atlasTex, ATLAS_W, s_atlasH, pal, s_rgbTex, ATLAS_W, s_atlasH)
        : tagpu_rglsl_job_new    ("terr", 0, 1, s_atlasTex, ATLAS_W, s_atlasH, pal, s_rgbTex, ATLAS_W, s_atlasH);
    s_rgbPalSerial = tagpu_pal_serial();
    ok = s_job && tagpu_rglsl_job_add(s_job, frames, n) > 0;
    if (!ok && s_job) { tagpu_rglsl_job_free(s_job); s_job = NULL; }
    free(frames);
    return ok;
}

/* tagpu_restoredump.on: the finished atlas, as the shader samples it, once, as
   raw RGBA -- the restorer's only disk write, and only under the trigger.
   `tascene restorediff` holds it to the pack's atlas (renderers.md 4c Q8). */
static void dump_if_armed(void)
{
    unsigned char* buf;
    FILE* f;
    char b[160];
    size_t n = (size_t)ATLAS_W * (size_t)s_atlasH * 4;
    if (GetFileAttributesA("tagpu_restoredump.on") == INVALID_FILE_ATTRIBUTES) return;
    buf = (unsigned char*)malloc(n);
    if (!buf) return;
    glBindTexture(GL_TEXTURE_2D, s_rgbTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    f = fopen("tagpu_restore.rgba", "wb");
    if (f) { fwrite(buf, 1, n, f); fclose(f); }
    free(buf);
    _snprintf(b, sizeof b, "terr: restored atlas dumped to tagpu_restore.rgba (%dx%d RGBA, %d tiles)%s",
              ATLAS_W, s_atlasH, s_atlasN, f ? "" : " -- WRITE FAILED");
    flog(b);
}

/* Once per frame after the atlas is known: start the restore when the switch
   is on and none exists for this set, then watch the job the frame driver
   slices (tagpu_native.c calls tagpu_rglsl_step after the gathers) until it
   drains; the cells already painted are sampled from the first slice on. The
   switch going off mid-restore pauses the job (its scratch stays allocated)
   and leaves the texture in place, unsampled, restored as far as it got. */
static void restore_step(const char* ta)
{
    if (!tagpu_classicpp_assets() || !s_atlasTex || !s_setPix) return;
    if (s_rgbState == 0) {
        if (!s_rectValid) return;          /* the order wants a viewport: next frame */
        if (!tagpu_pal_live()) return;     /* ...and a palette: next frame  */
        if (!glsl_begin(ta, 0)) { s_rgbState = -1; flog("terr: GLSL restore could not start; Classic++ terrain stays indexed"); return; }
        s_rgbState = 1;
        return;
    }
    if (s_rgbState == 1) {
        if (tagpu_rglsl_job_failed(s_job)) {
            s_rgbState = -1; flog("terr: GLSL restore failed; Classic++ terrain stays indexed");
            tagpu_rglsl_job_free(s_job); s_job = NULL;
            return;
        }
        if (tagpu_rglsl_job_idle(s_job)) {
            char b[128];
            s_rgbState = 2;
            tagpu_rglsl_job_free(s_job); s_job = NULL;     /* the texture is ours */
            _snprintf(b, sizeof b, "terr: restored atlas complete (GLSL) %dx%d for %d tiles", ATLAS_W, s_atlasH, s_atlasN);
            flog(b);
            dump_if_armed();
        }
        return;
    }
    /* Complete, and then the palette moved under it (the Gamma option, or
       `+gamma N`): the tiles hold the brightness the old palette gave them
       while the engine's own pixels beside them moved. Queue them all again
       over the texture that is there — one repaint at a time, because this
       only runs from state 2. */
    if (s_rgbState == 2 && s_rgbPalSerial != tagpu_pal_serial()) {
        char b[128];
        if (!s_rectValid) return;          /* restore_order wants a viewport: next frame */
        if (!glsl_begin(ta, 1)) {
            s_rgbPalSerial = tagpu_pal_serial();   /* do not retry every frame */
            flog("terr: palette changed but the repaint could not start; the atlas keeps the old colours");
            return;
        }
        s_rgbState = 1;
        _snprintf(b, sizeof b, "terr: palette changed (serial=%u): %d tiles queued for repaint",
                  s_rgbPalSerial, s_atlasN);
        flog(b);
    }
}

/* The cells a rect of `w` x `h` game px can cost the gather. The +2 is the
   gather's own worst case: ceil32 of the size plus a fractional eye offset
   costs one extra column and one extra row. */
static int span_cells(int w, int h)
{
    long cols = (long)w / 32 + 2, rows = (long)h / 32 + 2;
    long n = cols * rows;                    /* < 2^31 for any believed w,h */
    return n > 0 ? (int)n : 0;
}

/* Grow the staging to hold `cells`, never shrink. Render thread, and in
   practice once per resolution: the reservation is made for the widest rect
   the VIEWPORT can produce, not for this frame's rect, so zooming never
   allocates. A refusal — the memory guard, or a failed realloc — leaves the
   old buffer intact and is not an error here: the caller trims the rect to
   what is held, which costs a black margin and not a dropped frame. */
static void terr_reserve(int cells)
{
    short* p;
    size_t bytes;
    if (cells <= s_instCells) return;
    if (cells > INST_MAX_CELLS) cells = INST_MAX_CELLS;
    if (cells <= s_instCells) return;
    bytes = (size_t)cells * ICOMP * sizeof(short);
    p = (short*)realloc(s_inst, bytes);
    if (!p) {
        char b[128];
        _snprintf(b, sizeof b, "terr: could not reserve %d cells (%u KB) —"
                               " the view is trimmed to the %d held",
                  cells, (unsigned)(bytes / 1024), s_instCells);
        flog(b);
        return;
    }
    {
        char b[128];
        _snprintf(b, sizeof b, "terr: staging %d -> %d cells (%u KB)",
                  s_instCells, cells, (unsigned)(bytes / 1024));
        flog(b);
    }
    s_inst = p; s_instCells = cells;
}

/* The one budget every pass has to agree on — see tagpu_terr.h for the
   contract. `tagpu_terr_gather` bails when the rect it is asked for needs more
   cells than the staging holds, and a bail HANDS THE DRAW BACK: one frame of
   the engine's own terrain under an inverted composite, which reads as a
   flash. Bailing is the one behaviour that looks like a bug, so the rect is
   trimmed to what can actually be drawn BEFORE any pass sizes itself from it
   (tagpu_native.c), leaving terrain, units, wrecks and features one rect.

   The reservation is made from the VIEWPORT at the zoom floor, so on any
   screen whose viewport is believed the trim below finds nothing to do and no
   view is ever drawn short. It runs anyway, because the reservation can be
   refused and the gather's bail must never be what a player sees.

   Shrink is proportional and iterated rather than solved: the rect keeps its
   aspect, so the margin is even on all four sides, and no square root is needed
   for a loop that converges in three passes at any sane viewport. */
void tagpu_terr_clamp_span(int vw, int vh, int* w, int* h)
{
    int guard = 64;
    if (*w < 32) *w = 32;
    if (*h < 32) *h = 32;
    /* the widest rect this viewport can ever ask for: itself at the zoom floor.
       vw/vh are the true 1x viewport, already range-checked by the caller. */
    if (vw > 0 && vh > 0)
        terr_reserve(span_cells((int)((float)vw / TAGPU_ZOOM_MIN) + 64,
                                (int)((float)vh / TAGPU_ZOOM_MIN) + 64));
    while (guard-- > 0) {
        if (span_cells(*w, *h) <= s_instCells) return;
        *w -= *w / 32 + 1;
        *h -= *h / 32 + 1;
    }
}

/* the engine's own arithmetic: `cdq; and edx,0x1f; add; sar 5` is a division
   toward zero, not a floor — reproduce it, do not "fix" it */
static int div32_trunc(int v) { return (v + (v < 0 ? 31 : 0)) >> 5; }
static int ceil32(int v)      { int q = div32_trunc(v); return (v - q * 32) ? q + 1 : q; }

/* one visible cell: where it is in this frame's grid, and where its tile is in
   the atlas. The shader turns the pair into the six vertices. */
static void put_cell(int col, int row, int cx, int cy)
{
    short* o = s_inst + (size_t)s_ncell * ICOMP;
    o[0] = (short)col; o[1] = (short)row; o[2] = (short)cx; o[3] = (short)cy;
    s_ncell++;
}

/* Every exit that draws nothing must hand the draw back: the skip byte is
   latched from the previous frame, so returning early with it set would leave
   the viewport flat key-colour until the 90-frame watchdog notices. */
static int terr_bail(void)
{
    tagpu_terrown_set_skip(0);
    return 0;
}

int tagpu_terr_gather(const TAGPU_FXVIEW* v)
{
    const char* ta = v->ta;
    const unsigned short* tmap;
    int mapW16, mapH16, stride, mrows;
    int tx0, ty0, fx, fy, cols, rows, r, c;
    int skipped = 0, junk = 0, own, wasFilled, emit;
    int eyeX, eyeY, vpL, vpT, evw, evh;
    float iw, ih;

    if (s_armed != 1) return terr_bail();
    if (s_state == 0) init_gl();
    if (s_state != 1) return terr_bail();
    if (!ensure_atlas(ta)) return terr_bail();
    ensure_height(ta, v->frame_counter);
    restore_step(ta);

    s_ncell = 0;
    /* Read BEFORE touching the skip: it says whether the engine frame we are
       about to composite over is the key fill rather than a terrain blit. */
    wasFilled = tagpu_terrown_filled();
    /* Emitting without owning the draw is only ever right as a deliberate A/B
       (`over`) or for the one frame after we hand the draw back — our terrain
       is opaque and covers the whole viewport, so any other time it would hide
       the health bars, wireframes, build cursor and chat that the composite's
       key test exists to let through. Without the patch installed there IS no
       key, so the pass counts and says so rather than blanking the overlays. */
    own = !s_passive && !s_over && tagpu_terrown_installed();
    emit = own || s_over || wasFilled;

    tmap = *(const unsigned short* const*)(ta + OFF_TILEMAP);
    mapW16 = *(const int*)(ta + OFF_MAPW16);
    mapH16 = *(const int*)(ta + OFF_MAPH16);
    if (!ptr_ok(tmap)) return terr_bail();
    if (mapW16 <= 0 || mapH16 <= 0 || mapW16 > 4096 || mapH16 > 4096) return terr_bail();
    stride = mapW16 / 2;                       /* TILE_MAP width in 32-px cells */
    mrows  = mapH16 / 2;
    if (stride <= 0 || mrows <= 0) return terr_bail();

    /* Run the engine's own algorithm over the ZOOM's viewport, not the
       engine's. Screen and world differ by a pure translation, so widening the
       rect is the same as moving the eye to its top-left corner and asking for
       more columns — the quads still land at unzoomed game coordinates, which is
       what the vertex shader's scale-about-the-centre expects. At zoom >= 1
       these are the engine's own numbers and the arithmetic is untouched. */
    vpL = v->evpL; vpT = v->evpT; evw = v->evw; evh = v->evh;
    eyeX = v->eyeX + (vpL - v->vpL);
    eyeY = v->eyeY + (vpT - v->vpT);
    tx0 = div32_trunc(eyeX); fx = eyeX - tx0 * 32;
    ty0 = div32_trunc(eyeY); fy = eyeY - ty0 * 32;
    cols = ceil32(evw + fx);
    rows = ceil32(evh + fy);
    if (cols <= 0 || rows <= 0) return terr_bail();
    /* the staging holds what tagpu_terr_clamp_span reserved for this viewport,
       and it trimmed the rect to fit — so this is the guard, not the policy */
    if (!s_inst || (long)cols * rows > s_instCells) return terr_bail();
    s_rectTx0 = tx0; s_rectTy0 = ty0; s_rectCols = cols; s_rectRows = rows; s_rectValid = 1;

    iw = 1.0f / (float)ATLAS_W;
    ih = 1.0f / (float)s_atlasH;
    /* what the vertex shader rebuilds the quads from: the screen point grid
       cell (0,0) starts at, and the atlas texel size. `vpL - fx` is the same
       integer `vpL + c * 32 - fx` was built on, so the positions are the same
       floats. The map cell is s_rectTx0/s_rectTy0, set just above. */
    s_origX = (float)(vpL - fx); s_origY = (float)(vpT - fy);
    s_iw = iw; s_ih = ih;
    for (r = 0; r < rows && emit; r++) {
        int my = ty0 + r;
        if (my < 0 || my >= mrows) { skipped += cols; continue; }
        for (c = 0; c < cols; c++) {
            int mx = tx0 + c;
            int idx;
            if (mx < 0 || mx >= stride) { skipped++; continue; }
            idx = tmap[(size_t)my * stride + mx];
            if (idx >= s_atlasN) { junk++; continue; }
            /* the quad still spans exactly TILE_PX texels, and its far edge
               lands ON the guard column, which is a copy of the last real one.
               Screen and world differ by a pure translation here, so the cell's
               world rect is exactly (mx*32, my*32)..+32 — the space the
               engine's fog grid is built in — and the shader reaches it as
               uTile0 + (col,row), which is (tx0+c, ty0+r) = (mx, my). */
            put_cell(c, r, idx % ATLAS_COLS, idx / ATLAS_COLS);
        }
    }

    /* we drew this frame: the engine's terrain pass may be skipped. Handing it
       back here clears `filled`, so the composite stops inverting on the SAME
       frame the quads above were emitted for — which is why the hand-back never
       shows a frame of bare key fill. */
    tagpu_terrown_set_skip(own && s_ncell > 0);
    tagpu_terrown_beat(v->frame_counter);

    {
        static unsigned last = 0;
        if (v->frame_counter - last >= 60) {
            char b[240];
            last = v->frame_counter;
            _snprintf(b, sizeof b,
                "terr: grid=%dx%d tile0=(%d,%d) frac=(%d,%d) map=%dx%d cells=%d"
                " zoomvp=%dx%d off-map=%d junk=%d atlas=%dx%d/%d%s%s",
                cols, rows, tx0, ty0, fx, fy, stride, mrows, s_ncell, evw, evh,
                skipped, junk, ATLAS_W, s_atlasH, s_setCount,
                s_over ? " (over: engine still drawing)"
                       : (s_passive ? " (passive: engine still drawing)" : ""),
                tagpu_terrown_installed() ? ""
                    : " (NOTHING EMITTED: terrown.on must exist at DLL attach —"
                      " arm it before launch, not after)");
            flog(b);
        }
    }
    if (s_log && s_ncell) {
        static unsigned lastl = 0;
        if (v->frame_counter - lastl >= 120) {
            char b[200];
            int lx = tx0 < 0 ? 0 : (tx0 >= stride ? stride - 1 : tx0);
            int ly = ty0 < 0 ? 0 : (ty0 >= mrows ? mrows - 1 : ty0);
            lastl = v->frame_counter;
            _snprintf(b, sizeof b,
                "terr: cell(0,0) idx=%u at=(%d,%d) world=(%d,%d) vp=(%d,%d %dx%d) key=%d",
                (unsigned)tmap[(size_t)ly * stride + lx],
                v->vpL - fx, v->vpT - fy, tx0 * 32, ty0 * 32,
                v->vpL, v->vpT, v->vw, v->vh, s_key);
            flog(b);
        }
    }
    return s_ncell;
}

void tagpu_terr_render(const TAGPU_FXVIEW* v, unsigned int palTex)
{
    if (s_state != 1 || s_ncell == 0) return;
    glUseProgram(s_prog);
    x_glUniform2f(s_uGame, (float)v->gw, (float)v->gh);
    glUniform1i(s_uFog, v->fogMode & 1);   /* terrain darkens in grey, never hides */
    if (s_uFogOrg >= 0) x_glUniform2f(s_uFogOrg, (float)v->fogOrgX, (float)v->fogOrgY);
    if (s_uFogDim >= 0) x_glUniform2f(s_uFogDim, (float)v->fogCols, (float)v->fogRows);
    x_glUniform1f(s_uZoom, v->zoom > 0.0f ? v->zoom : 1.0f);
    x_glUniform2f(s_uZoomC, v->zoomCx, v->zoomCy);
    x_glUniform1f(s_uDepthScale, v->depthScale > 1.0f ? v->depthScale : 512.0f);
    x_glUniform1f(s_uEnc, TERR_ENC);
    /* the three the vertex shader rebuilds each cell's quad from */
    x_glUniform2f(s_uOrigin, s_origX, s_origY);
    x_glUniform2f(s_uTile0, (float)s_rectTx0, (float)s_rectTy0);
    x_glUniform2f(s_uTexel, s_iw, s_ih);
    x_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, palTex);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, v->fogTex);
    x_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, v->fogLut);
    x_glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, s_rgbTex);
    x_glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, s_hTex);
    x_glActiveTexture(GL_TEXTURE0);
    /* running OR complete: while the job runs the alpha test in the shader
       reveals each cell as its out pass lands (and stays indexed elsewhere);
       a failed or absent job never samples the texture */
    glUniform1i(s_uRestored, ((s_rgbState == 1 || s_rgbState == 2) && tagpu_classicpp_assets()) ? 1 : 0);
    tagpu_shadow_apply(&s_shU);            /* this frame's map, or uShadowOn 0 */
    /* the lighting: the terrain's sun. uLit is the MASTER ARM (the Classic++
       colour path, which `assets=`/`light=` only subdivide) and uLambert the
       `light=` half; uHDim is 0 while there is no usable grid, and the shader
       then skips the lambert rather than sample a dead or stale texture */
    {
        const TAGPU_LIGHT* L = tagpu_classicpp_light();
        glUniform1i(s_uLit, tagpu_classicpp_on() ? 1 : 0);
        glUniform1i(s_uLambert, tagpu_classicpp_lit() ? 1 : 0);
        x_glUniform3f(s_uSun, L->sun[0], L->sun[1], L->sun[2]);
        x_glUniform1f(s_uAmb, L->amb);
        x_glUniform1f(s_uNorm, 1.0f / L->level);
        x_glUniform2f(s_uHDim, (float)s_hW, (float)s_hH);
    }
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* orphan and upload in one call, sized to what this frame USES. Even the
       whole array is only 2 MB now, but a 1x view needs ~2.5k cells of it and
       re-specifying the rest every frame would churn driver memory for nothing.
       (GL_ARRAY_BUFFER's binding is not VAO state, so binding it to re-specify
       the storage leaves the attribute's own buffer binding alone.) */
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)s_ncell * ICOMP * 2, s_inst,
                 GL_STREAM_DRAW);
    /* opaque, and the far plane of the frame: depth writes ON, no blending
       needed (the FBO is premultiplied and terrain's alpha is 1 everywhere) */
    x_glDrawArraysInstanced(GL_TRIANGLES, 0, 6, s_ncell);
}
