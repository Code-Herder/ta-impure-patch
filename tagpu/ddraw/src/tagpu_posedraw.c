/* tagpu_posedraw.c — the posed program (G16 steps 5 and 6).

   The pass that finally draws from step 4's bake. See tagpu_posedraw.h for the
   contract and research/notes/gpu-posing.md §4 for the design; what follows is
   what the code does rather than why the design is what it is.

   THE VERTEX SHADER IS THE PORT OF `emit_node`. Every expression below is
   transcribed from tagpu_native.c's emitter, which is itself transcribed from
   the engine's raster:

     piece transform   the pose matrix, rest -> model space, body turn folded in
     projection        sx = ax + x,  sy = ay + (-z - y/2)
     depth key         encBase + clamp((2y - z)/256, +-1.8)
     world             wx0 + sx-part, wz0 + sy-part   (what the fog samples)
     height            y                              (what the waterline clips)
     shade             the rest normal through the piece's rotation, flipped
                       toward SH_V, dotted with SH_L and quantised onto the
                       32-row SHD ramp

   THREE RANGES, ONE PROGRAM, `uRange` (G16 step 6). The bake has always laid
   the body, the slant and the wire down in one buffer, so a range is a
   different `first`/`count` on the same bind; what differs in the shader is
   small and explicit:

     BODY   (0)  the projection above, the depth key, the shade.
     SLANT  (1)  0x45A610's projection `(x + y/4, -z - y/4)` off the posed
                 vertex SNAPPED to whole units, the neutral SHD row, and its
                 own per-piece rule — `(P_FLAGS & 3) == 3`, visible AND
                 `cached` — which the body's all-zero matrix cannot express
                 because such a piece still draws in the body range.
     WIRE   (2)  the body projection, GL_LINES, one notch nearer (+0.15), and
                 the nanoframe's animated blue from a uniform.

   THE SNAP ROUNDS ONTO THE 16.16 GRID FIRST, and that is the whole reason the
   slant can be ported at all. `xi = v[0] >> 16` is an arithmetic FLOOR of the
   value the engine holds in 16.16; our float compose lands 1-2 LSB away from
   that value (gpu-posing.md §5), and a floor turns 2 LSB into a WHOLE screen
   unit — a shadow edge a pixel out — whenever a coordinate sits within
   2/65536 of an integer. Rounding to the grid before flooring puts us on the
   engine's own representation: it is EXACT against `recon_prim`, which rounds
   with the same `floor(x*65536 + 0.5)`, so Gate B isolates the port with
   nothing of §5's residual in it, and against the engine itself it leaves only
   the reconstruction's residual rather than multiplying it by 65536. The body
   and the wire are NOT rounded, deliberately: their projection is affine and
   continuous in the value, so the same 2 LSB moves a vertex 3e-5 px and can
   only flip a coverage sample.

   16.16 is exactly representable in float32 while |model unit| < 128 (128 x
   65536 = 2^23, where the float32 spacing is still 0.5 and `+ 0.5` is exact);
   the largest stock model is an order of magnitude inside that.

   THE ONE KNOWN INEXACTNESS is not here but in the bake: `emit_node` takes its
   degeneracy test on the ENGINE's posed vertices, rounded into 16.16 at every
   axis and every level of the tree, while the bake takes it on the rest
   vertices (tagpu_posebake.c, "THE ONE PLACE THIS IS NOT EXACT"). A face of any
   real area gives the same answer; a near-degenerate one can take the neutral
   SHD row where the CPU path takes a shaded one. Gate B is told to look for
   exactly that — a whole face one row off, rather than an edge flip.

   THE POSE LIVES IN A std140 UNIFORM BLOCK, which is what takes the piece cap
   out of the design (gpu-posing.md decision 7). The block is
   TAGPU_PBMAXPIECE (256) pieces of 3 rows plus two packed flag arrays — 14336
   bytes, inside the 16 KB GL 3.1 guarantees, with headroom rather than sitting
   exactly on the limit. The guarantee is not assumed: GL_MAX_UNIFORM_BLOCK_SIZE
   is read at build time and the pass refuses to arm below it. Since G16 step 8
   there is no CPU emitter to leave those units to, so the refusal is published
   instead (`tagpu_posedraw_live`) and `owndraw` stops skipping the engine's own
   unit rasterise — the engine draws them, rather than nothing drawing them.

   A HIDDEN PIECE ARRIVES AS AN ALL-ZERO MATRIX and collapses its triangles onto
   the model origin; a face the material stream has nothing for carries the skip
   flag and is pushed outside the clip volume. Neither changes the vertex count,
   which is what lets the two buffers be rebuilt independently.

   RENDER THREAD ONLY. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_model3do.h"
#include "tagpu_posebake.h"
#include "tagpu_posedraw.h"
#include "tagpu_native.h"
#include "tagpu_render3do.h"
#include "tagpu_classicpp.h"
#include "tagpu_shadow.h"

#define STR2(x) #x
#define STR(x)  STR2(x)

/* the block: 3 rows per piece, then two packed floats per piece — `shaded`
   for the body's SHD row and a visibility WORD (0 / 1 / 3) the slant and the
   wire read. Two arrays rather than bits of one float because the second costs
   1 KB (14336 against GL 3.1's guaranteed 16384) and a packed pair costs every
   reader of it a decode; the second is a word rather than two more arrays
   because its two values nest — a slant caster is always visible. */
#define PD_ROWS   (TAGPU_PBMAXPIECE * 3)          /* 768 vec4 */
#define PD_FLAGV  (TAGPU_PBMAXPIECE / 4)          /*  64 vec4 */
#define PD_FLAGOFF (PD_ROWS * 16)                 /* bytes    */
#define PD_VISOFF  ((PD_ROWS + PD_FLAGV) * 16)    /* bytes    */
#define PD_BLOCK   ((PD_ROWS + PD_FLAGV * 2) * 16)  /* 14336  */

/* uRange */
#define PD_R_BODY  0
#define PD_R_SLANT 1
#define PD_R_WIRE  2

static void plog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int    s_state;          /* 0 untried, 1 ready, 2 refused */

/* ---- readiness ----------------------------------------------------------
   `tagpu_posedraw.on` WAS a measurement lever, kept while the CPU emitters
   were Gate B's oracle. G16 step 8 deleted them, so this pass is THE unit
   renderer and there is no lever: what is left is whether it can run at all,
   which `tagpu_posedraw_ready()` answers once per GL context.

   THAT ANSWER IS PUBLISHED, because `owndraw` needs it. Its detours skip the
   engine's own unit rasterisers, so if this pass cannot arm and the skip
   still happens, no units are drawn at all. `tagpu_owndraw_classify` therefore
   asks before it skips (gpu-posing.md §4, decision B) — and reads `s_state`
   from the GAME thread while only the render thread writes it. That is safe by
   DIRECTION rather than by timing: `s_state` is one aligned int, set to 1 only
   after the programs have linked and the buffers exist, and put back to 0 by
   `tagpu_posedraw_glreset()` BEFORE a new context is used. A stale "not ready"
   costs a double draw for a frame (the engine's 8bpp under our RGB); a stale
   "ready" is the unsafe direction and no write order produces it. */
int tagpu_posedraw_live(void) { return s_state == 1; }

/* 1 only once the pass has TRIED and failed — a driver this build cannot run
   on. Distinct from `!live`, which is also true for the frame or two before
   the render thread has built anything, and which is not a problem. */
int tagpu_posedraw_refused(void) { return s_state == 2; }

/* ---- GL ---------------------------------------------------------------- */
typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_UNIFORM3F)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_UNIFORM4F)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_BINDBUFBASE)(GLenum, GLuint, GLuint);
typedef GLuint (APIENTRY *PFN_GETUBIDX)(GLuint, const GLchar*);
typedef void (APIENTRY *PFN_UBBIND)(GLuint, GLuint, GLuint);
typedef void (APIENTRY *PFN_PROGLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
/* GL 1.1 core, and therefore NOT the global one: xwglGetProcAddress returns
   NULL for the 1.1 entry points under wine, so opengl_utils' `glGetIntegerv`
   is a null pointer and it guards its own use of it (render_ogl.c says so at
   the GL_NUM_EXTENSIONS read). Every module that wants it loads its own
   through a getgl() that falls back to opengl32.dll — calling the global one
   is a jump to address 0, which is what it did here. */
typedef void (APIENTRY *PFN_GETINTEGERV)(GLenum, GLint*);

static PFN_DRAWARRAYS   x_glDrawArrays;
static PFN_UNIFORM2F    x_glUniform2f;
static PFN_UNIFORM3F    x_glUniform3f;
static PFN_UNIFORM4F    x_glUniform4f;
static PFN_BINDBUFBASE  x_glBindBufferBase;
static PFN_GETUBIDX     x_glGetUniformBlockIndex;
static PFN_UBBIND       x_glUniformBlockBinding;
static PFN_PROGLOG      x_glGetProgramInfoLog;   /* not in opengl_utils.h */
static PFN_GETINTEGERV  x_glGetIntegerv;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}


static GLuint s_prog, s_dprog, s_ubo;

/* body program */
static GLint u_game, u_off, u_zoom, u_zoomC, u_depthScale, u_ss;
/* the fog macro carries its OWN zoom pair in the fragment stage
   (tagpu_glsl.h): the same two numbers, and the fog samples at the wrong
   place without them */
static GLint u_zoomF, u_zoomCF;
static GLint u_anchor, u_enc, u_shd, u_cast, u_alpha;
static GLint u_fog, u_fogOrg, u_fogDim, u_scafOn, u_scafP;
static GLint u_waterT, u_waterMode, u_digT, u_nanoOn, u_nanoT, u_nanoC;
static GLint u_lit, u_sun, u_amb, u_norm, u_shadow, u_restored, u_depthPass;
static GLint u_range, u_wire;
static TAGPU_SHADOWU s_shU;
/* depth program */
static GLint d_anchor, d_enc, d_cast, d_shadowMat, d_depthPass, d_range;
static GLint d_game, d_off, d_zoom, d_zoomC, d_depthScale;

static unsigned s_units, s_tris, s_overPiece;

/* units the pass actually drew this frame, for the caller to hold its own
   queued count against — a queued unit that is not drawn is a missing one */
unsigned tagpu_posedraw_drawn(void) { return s_units; }
static unsigned s_slantU, s_slantT, s_wireU, s_wireL;

/* ---- the shader --------------------------------------------------------- */
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"     /* rest position, model units    */
    "layout(location=1) in vec3 aNrm;\n"     /* rest normal of its own face   */
    "layout(location=2) in float aPiece;\n"
    "layout(location=3) in float aGF;\n"     /* TAGPU_PBF_SHADED              */
    "layout(location=4) in vec2 aUV;\n"
    "layout(location=5) in vec2 aFC;\n"      /* flat idx/255, tex ck/255      */
    "layout(location=6) in float aSkip;\n"
    /* 3 rows of a 4x3 per piece, then two per-piece words packed 4 to a vec4:
       uPieceFlag 1.0 = the piece is shaded (emit_geom_at's `pieceShaded`);
       uPieceVis  0 = not drawn, 1 = drawn, 3 = drawn AND the slant casts from
       it (`P_FLAGS` bit 0, and bit 1 as well). */
    "layout(std140) uniform Pose {\n"
    "  vec4 uRow[" STR(PD_ROWS) "];\n"
    "  vec4 uPieceFlag[" STR(PD_FLAGV) "];\n"
    "  vec4 uPieceVis[" STR(PD_FLAGV) "];\n"
    "};\n"
    "uniform vec2 uGame;\n"
    "uniform vec2 uOffset;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "uniform vec4 uAnchor;\n"                /* ax, ay, world x, world z      */
    "uniform float uEnc;\n"                  /* the row's depth key base      */
    "uniform vec2 uShd;\n"                   /* shNeutral, shDir              */
    "uniform vec3 uCast;\n"                  /* altitude, ground + throw, sv  */
    "uniform int uDepthPass;\n"
    "uniform mat4 uShadowMat;\n"
    "uniform int uRange;\n"                 /* 0 body, 1 slant, 2 wire       */
    "uniform float uWire;\n"                /* the nanoframe blue, idx/255   */
    "out vec2 vUV; flat out vec2 vFC; flat out float vShade; out vec2 vWorld;\n"
    "out float vEnc; out float vVY; flat out vec3 vNrm; out vec3 vShW;\n"
    /* tagpu_native.c's SH_V and SH_L, the engine's shading basis */
    "const vec3 SH_V = vec3(0.0, 0.8944, -0.4472);\n"
    "const vec3 SH_L = vec3(-0.35, 0.80, -0.49);\n"
    "void main(){\n"
    "  int pi = int(aPiece + 0.5);\n"
    "  int pb = pi * 3;\n"
    /* Three ways a vertex is not drawn, and all of them collapse the same way
       — every vertex of the primitive carries the same answer, so the whole
       primitive lands outside the same clip plane and nothing survives.

       aSkip: a face the engine's rasteriser paints nothing for. It is baked so
       that the geometry and the material buffers hold the same number of
       vertices (gpu-posing.md §4). Always 0 in the slant range, which flat
       fills every face it is given.

       uPieceVis >= 3, the SLANT's per-piece rule: `(P_FLAGS & 3) == 3`,
       visible AND `cached`, which a COB's dont-cache clears (a wind
       generator's mast). It cannot ride the all-zero matrix the way body
       visibility does, because such a piece still draws in the BODY range and
       needs its matrix there.

       uPieceVis >= 1, the WIRE's: `P_FLAGS & 1`, the same rule the body has —
       but the body expresses it as an all-zero matrix, which collapses a
       triangle to zero AREA, and a triangle of zero area is guaranteed to
       produce no fragments. A LINE of zero length is not: the rasterisation
       rules do not promise it away, and one bright pixel per hidden edge would
       land exactly on the unit's origin. So the wire is refused here instead
       of relying on that. */
    "  float pvis = uPieceVis[pi >> 2][pi & 3];\n"
    "  if (aSkip > 0.5 ||\n"
    "      (uRange == 1 && pvis < 2.5) ||\n"
    "      (uRange == 2 && pvis < 0.5)) {\n"
    "    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "    vUV = vec2(0.0); vFC = vec2(0.0); vShade = 0.0; vWorld = vec2(0.0);\n"
    "    vEnc = 0.0; vVY = 0.0; vNrm = vec3(0.0, 1.0, 0.0); vShW = vec3(0.0);\n"
    "    return;\n"
    "  }\n"
    "  vec4 rp = vec4(aPos, 1.0);\n"
    "  vec3 m = vec3(dot(uRow[pb], rp), dot(uRow[pb+1], rp), dot(uRow[pb+2], rp));\n"
    /* THE POSED VERTEX ONTO THE ENGINE'S OWN 16.16 GRID, before anything reads
       it. This is not an optimisation of the slant's snap: it is the vertex's
       REPRESENTATION. The engine holds every posed vertex as three 16.16
       integers, every CPU emitter reads them back as `v[i] / 65536.0f`, and
       `recon_prim` writes the reconstruction into the same form with the same
       `floor(x * 65536 + 0.5)`. A float compose that stops short of it sits up
       to half an LSB off a value that is exactly representable — see the file
       header for why the representation is exact here — so rounding is what
       makes the port reproduce the chain rather than approximate it. */
    "  m = floor(m * 65536.0 + 0.5) / 65536.0;\n"
    /* the engine's projection, exactly as emit_node bakes it on the CPU — and,
       for the slant range, 0x45A610's instead */
    "  float px, py;\n"
    "  if (uRange == 1) {\n"
    /* emit_slant_at's snap: `xi = v[0] >> 16`, `nzi = (-v[2]) >> 16`,
       `q = (v[1] >> 16) >> 2` — every one an arithmetic FLOOR and never a
       truncation toward zero. `m` is already the 16.16 value, so `floor` of it
       is the shift. */
    "    float xi = floor(m.x);\n"
    "    float yi = floor(m.y);\n"
    "    float nzi = floor(-m.z);\n"
    "    float q = floor(yi / 4.0);\n"
    "    px = xi + q; py = nzi - q;\n"
    "  } else {\n"
    "    px = m.x; py = -m.z - m.y * 0.5;\n"
    "  }\n"
    "  vec2 p0 = uAnchor.xy + vec2(px, py);\n"
    "  float md = clamp((2.0 * m.y - m.z) / 256.0, -1.8, 1.8);\n"
    /* the wire is emitted one notch NEARER than the surface it traces, so it
       wins against the solid part of the model: md reaches +-1.8 and the bias
       is 0.15, against the 2.0 half-gap between depth rows (emit_wire) */
    "  float enc = uEnc + md + (uRange == 2 ? 0.15 : 0.0);\n"
    /* the shade. The rest normal is unit length and the piece transform is a
       composition of rotations, so the posed normal is unit length too — but
       normalise anyway rather than rest the quantisation on that, since a
       length that drifted would move the whole face a row. The flip toward
       SH_V is emit_node's, and it is not baked because it depends on the POSED
       direction, which is what the piece matrix decides. */
    "  float shade = uShd.x / 31.0;\n"
    "  vec3 un = vec3(0.0, 1.0, 0.0);\n"
    "  bool pShaded = uPieceFlag[pi >> 2][pi & 3] > 0.5;\n"
    "  if (aGF >= 0.5 && pShaded) {\n"
    "    vec3 n = vec3(dot(uRow[pb].xyz, aNrm), dot(uRow[pb+1].xyz, aNrm),\n"
    "                  dot(uRow[pb+2].xyz, aNrm));\n"
    "    if (dot(n, SH_V) < 0.0) n = -n;\n"
    "    float nl = length(n);\n"
    "    if (nl > 1e-6) {\n"
    "      float I = dot(n, SH_L) / nl;\n"
    "      float rr = clamp(uShd.x + uShd.y * floor(I * 12.0 + 0.5), 0.0, 31.0);\n"
    "      shade = rr / 31.0;\n"
    /* Classic++ lights the fragment from the same outward normal the row is
       quantised from, carried in MAP space (3DO z points north, hence the
       flip) — emit_node's `un` */
    "      un = vec3(n.x / nl, n.y / nl, -n.z / nl);\n"
    "    }\n"
    "  }\n"
    "  vec2 p = (p0 + uOffset - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - enc/uDepthScale, 0.0, 1.0), 1.0);\n"
    /* the wire's colour is per UNIT (the nanoframe's animated blue) and the
       material stream is per type and owner, so it arrives as a uniform on the
       flat path rather than baked; the key stays -1, as emit_wire writes it */
    "  vUV = aUV; vFC = (uRange == 2) ? vec2(uWire, -1.0) : aFC;\n"
    "  vShade = shade;\n"
    "  vWorld = uAnchor.zw + vec2(px, py);\n"
    "  vEnc = enc; vVY = m.y; vNrm = un;\n"
    /* the vertex's SHADOW-SPACE point, the expression tagpu_shadow.c's own
       depth program evaluates, so a unit's fragments look their shadow up on
       their own caster */
    "  vShW = vec3(vWorld.x, uCast.y + uCast.z * m.y,\n"
    "              vWorld.y + (uCast.x + m.y) * 0.5);\n"
    "  if (uDepthPass == 1) gl_Position = uShadowMat * vec4(vShW, 1.0);\n"
    "}\n";

/* the depth twin writes no fragment at all — the native stream's own depth
   program (tagpu_shadow.c's VS_U with FS_NONE) discards nothing either, so a
   colour-keyed texel casts a shadow on both paths */
static const char* DFS =
    "#version 330 core\n"
    "void main(){}\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint s = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(s, 1, (const GLchar**)&src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 }, b[1100];
        glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        _snprintf(b, sizeof b, "posedraw: shader FAILED: %s", log);
        plog(b);
        s_state = 2;
    }
    return s;
}

static int link_block(GLuint prog)
{
    GLuint idx = x_glGetUniformBlockIndex(prog, "Pose");
    if (idx == GL_INVALID_INDEX) return 0;
    x_glUniformBlockBinding(prog, idx, 0);
    return 1;
}

int tagpu_posedraw_ready(void)
{
    GLuint vs, fs, dfs;
    GLint ok = 0, maxBlock = 0;
    const char* unitFS;
    char b[256];

    if (s_state) return s_state == 1;
    /* 3 = BUILDING, not 2 = refused. This blocks re-entry exactly as 2 did,
       but `tagpu_posedraw_refused()` stays false while we load entry points
       and query the block size — a window the GAME thread's owndraw classify
       runs through many times a frame, and in which a 2 here latched its
       one-shot "the posed unit program REFUSED to arm" on a perfectly healthy
       run. Every real refusal below sets 2 before returning. */
    s_state = 3;

    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glUniform2f  = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glUniform3f  = (PFN_UNIFORM3F) getgl("glUniform3f");
    x_glUniform4f  = (PFN_UNIFORM4F) getgl("glUniform4f");
    x_glBindBufferBase = (PFN_BINDBUFBASE)getgl("glBindBufferBase");
    x_glGetUniformBlockIndex = (PFN_GETUBIDX)getgl("glGetUniformBlockIndex");
    x_glUniformBlockBinding  = (PFN_UBBIND) getgl("glUniformBlockBinding");
    x_glGetProgramInfoLog    = (PFN_PROGLOG)getgl("glGetProgramInfoLog");
    x_glGetIntegerv          = (PFN_GETINTEGERV)getgl("glGetIntegerv");
    if (!x_glDrawArrays || !x_glUniform2f || !x_glUniform3f || !x_glUniform4f ||
        !x_glBindBufferBase || !x_glGetUniformBlockIndex ||
        !x_glUniformBlockBinding || !x_glGetIntegerv ||
        !glCreateShader || !glGenBuffers) {
        plog("posedraw: refused — the GL entry points this pass needs are missing");
        s_state = 2; return 0;
    }
    /* THE BOUND IS CHECKED, NOT ASSUMED. GL 3.1 guarantees 16 KB and we need
       PD_BLOCK (14336 since step 6's second per-piece word), but a driver that
       reports less would silently give every unit the wrong pose. Since step 8
       deleted the CPU emitters there is nothing to fall back TO: refusing here
       means owndraw stops skipping the engine's own unit rasterise, so the
       units are drawn by the engine at 8bpp rather than by us. */
    x_glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxBlock);
    if (maxBlock < PD_BLOCK) {
        _snprintf(b, sizeof b,
                  "posedraw: refused — GL_MAX_UNIFORM_BLOCK_SIZE is %d, the pose block needs %d",
                  (int)maxBlock, PD_BLOCK);
        plog(b);
        s_state = 2; return 0;
    }
    unitFS = tagpu_native_unit_fs();
    if (!unitFS) { plog("posedraw: refused — the native pass has no fragment shader to share");
                   s_state = 2; return 0; }

    s_state = 0;
    vs  = mksh(GL_VERTEX_SHADER, VS);
    fs  = mksh(GL_FRAGMENT_SHADER, unitFS);
    dfs = mksh(GL_FRAGMENT_SHADER, DFS);
    if (s_state == 2) return 0;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        if (x_glGetProgramInfoLog) x_glGetProgramInfoLog(s_prog, sizeof log - 1, NULL, log);
        _snprintf(b, sizeof b, "posedraw: link FAILED: %s", log);
        plog(b); s_state = 2; return 0;
    }
    s_dprog = glCreateProgram();
    glAttachShader(s_dprog, vs); glAttachShader(s_dprog, dfs);
    glLinkProgram(s_dprog);
    glGetProgramiv(s_dprog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        if (x_glGetProgramInfoLog) x_glGetProgramInfoLog(s_dprog, sizeof log - 1, NULL, log);
        _snprintf(b, sizeof b, "posedraw: depth link FAILED: %s", log);
        plog(b); s_state = 2; return 0;
    }
    if (!link_block(s_prog) || !link_block(s_dprog)) {
        plog("posedraw: refused — the Pose block did not survive linking");
        s_state = 2; return 0;
    }

#define PU(v, n) v = glGetUniformLocation(s_prog, n)
    PU(u_game, "uGame");        PU(u_off, "uOffset");
    PU(u_zoom, "uZoom");        PU(u_zoomC, "uZoomC");
    PU(u_depthScale, "uDepthScale"); PU(u_ss, "uSS");
    PU(u_zoomF, "uZoomF");      PU(u_zoomCF, "uZoomCF");
    PU(u_anchor, "uAnchor");    PU(u_enc, "uEnc");
    PU(u_shd, "uShd");          PU(u_cast, "uCast");
    PU(u_alpha, "uAlpha");      PU(u_fog, "uFog");
    PU(u_fogOrg, "uFogOrg");    PU(u_fogDim, "uFogDim");
    PU(u_scafOn, "uScafOn");    PU(u_scafP, "uScafP");
    PU(u_waterT, "uWaterT");    PU(u_waterMode, "uWaterMode");
    PU(u_digT, "uDigT");        PU(u_nanoOn, "uNanoOn");
    PU(u_nanoT, "uNanoT");      PU(u_nanoC, "uNanoC");
    PU(u_lit, "uLit");          PU(u_sun, "uSun");
    PU(u_amb, "uAmb");          PU(u_norm, "uNorm");
    PU(u_shadow, "uShadow");    PU(u_restored, "uRestored");
    PU(u_depthPass, "uDepthPass");
    PU(u_range, "uRange");      PU(u_wire, "uWire");
#undef PU
    /* the samplers name the same units the native pass binds its textures on,
       so this pass never re-binds them: it draws between that pass's own binds */
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "uLUT"),   1);
    glUniform1i(glGetUniformLocation(s_prog, "uPal"),   2);
    glUniform1i(glGetUniformLocation(s_prog, "uScaf"),  3);
    glUniform1i(glGetUniformLocation(s_prog, "uFogGrid"), 4);
    glUniform1i(glGetUniformLocation(s_prog, "uFogLUT"),  5);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlasRGB"), 8);
    tagpu_shadow_locate(s_prog, &s_shU);         /* names the map's two units */
    glUniform1i(u_depthPass, 0);
    glUniform1i(u_range, PD_R_BODY);
    glUseProgram(0);

    d_game = glGetUniformLocation(s_dprog, "uGame");
    d_off  = glGetUniformLocation(s_dprog, "uOffset");
    d_zoom = glGetUniformLocation(s_dprog, "uZoom");
    d_zoomC = glGetUniformLocation(s_dprog, "uZoomC");
    d_depthScale = glGetUniformLocation(s_dprog, "uDepthScale");
    d_anchor = glGetUniformLocation(s_dprog, "uAnchor");
    d_enc    = glGetUniformLocation(s_dprog, "uEnc");
    d_cast   = glGetUniformLocation(s_dprog, "uCast");
    d_shadowMat = glGetUniformLocation(s_dprog, "uShadowMat");
    d_depthPass = glGetUniformLocation(s_dprog, "uDepthPass");
    d_range     = glGetUniformLocation(s_dprog, "uRange");

    glGenBuffers(1, &s_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
    glBufferData(GL_UNIFORM_BUFFER, PD_BLOCK, NULL, GL_STREAM_DRAW);
    x_glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    _snprintf(b, sizeof b,
              "posedraw: armed — the posed program and its depth twin, pose block %d bytes "
              "(%d pieces, driver max %d)",
              PD_BLOCK, TAGPU_PBMAXPIECE, (int)maxBlock);
    plog(b);
    s_state = 1;
    return 1;
}

/* ---- the pose upload ---------------------------------------------------- */
/* One unit's pose into the block: the rows contiguous from 0, the packed piece
   flags at their own offset. Only the bytes this model uses are written. */
static void upload_pose(const TAGPU_PDUNIT* u)
{
    static float flags[PD_FLAGV * 4], vis[PD_FLAGV * 4];
    int np = u->npose, i, nf;
    if (np > TAGPU_PBMAXPIECE) np = TAGPU_PBMAXPIECE;
    if (np <= 0) return;
    nf = (np + 3) / 4;
    memset(flags, 0, (size_t)nf * 4 * sizeof(float));
    memset(vis,   0, (size_t)nf * 4 * sizeof(float));
    for (i = 0; i < np; i++) {
        flags[i] = (u->shaded && u->shaded[i]) ? 1.0f : 0.0f;
        vis[i]   = u->pvis ? (float)u->pvis[i] : 1.0f;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0,
                    (GLsizeiptr)np * 3 * 16, u->pose);
    glBufferSubData(GL_UNIFORM_BUFFER, PD_FLAGOFF,
                    (GLsizeiptr)nf * 16, flags);
    /* uploaded for every range, not only the two that read it: the block is
       one buffer and one unit's draws (body, then its silhouette, then its
       slant) share whatever the last upload left in it */
    glBufferSubData(GL_UNIFORM_BUFFER, PD_VISOFF,
                    (GLsizeiptr)nf * 16, vis);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

/* ---- bodies ------------------------------------------------------------- */
void tagpu_posedraw_begin(const TAGPU_PDVIEW* v)
{
    if (s_state != 1) return;
    glUseProgram(s_prog);
    x_glUniform2f(u_game, v->game[0], v->game[1]);
    x_glUniform2f(u_off, 0.0f, 0.0f);
    glUniform1f(u_zoom, v->zoom);
    x_glUniform2f(u_zoomC, v->zoomC[0], v->zoomC[1]);
    if (u_zoomF >= 0)  glUniform1f(u_zoomF, v->zoom);
    if (u_zoomCF >= 0) x_glUniform2f(u_zoomCF, v->zoomC[0], v->zoomC[1]);
    glUniform1f(u_depthScale, v->depthScale);
    if (u_ss >= 0) glUniform1f(u_ss, v->ss);
    x_glUniform2f(u_fogOrg, v->fogOrg[0], v->fogOrg[1]);
    x_glUniform2f(u_fogDim, v->fogDim[0], v->fogDim[1]);
    glUniform1i(u_scafOn, v->scafOn ? 1 : 0);
    x_glUniform4f(u_scafP, v->scafP[0], v->scafP[1], v->scafP[2], v->scafP[3]);
    glUniform1i(u_lit, v->lit ? 1 : 0);
    x_glUniform3f(u_sun, v->sun[0], v->sun[1], v->sun[2]);
    glUniform1f(u_amb, v->amb);
    glUniform1f(u_norm, v->norm);
    glUniform1i(u_shadow, 0);
    glUniform1i(u_depthPass, 0);
    glUniform1i(u_range, PD_R_BODY);
    glUniform1i(u_restored,
                (tagpu_r3d_atlas_rgbref() && tagpu_classicpp_on()) ? 1 : 0);
    x_glUniform2f(u_shd, (float)v->shNeutral, (float)v->shDir);
    tagpu_shadow_apply(&s_shU);
    x_glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
}

/* the geometry, the material stream and the pose all have to be present and
   agree about the piece count before anything is drawn: a mismatch would index
   the block past the pose we uploaded. `range` is the one about to be drawn —
   a model can have body triangles and no wire edges, or the reverse. */
static const TAGPU_PBGEOM* unit_ok(const TAGPU_PDUNIT* u, const TAGPU_PBMAT** mo,
                                   int range)
{
    const TAGPU_PBGEOM* g = (const TAGPU_PBGEOM*)u->geom;
    const TAGPU_PBMAT*  m = (const TAGPU_PBMAT*)u->mat;
    if (s_state != 1 || !g || !m || !u->pose) return NULL;
    if (m->geom != g || !m->vao || m->nvert != g->nvert) return NULL;
    if (u->npose < g->nparts) return NULL;
    if (g->nparts > TAGPU_PBMAXPIECE) { s_overPiece++; return NULL; }
    if (g->count[range] <= 0) return NULL;
    *mo = m;
    return g;
}

void tagpu_posedraw_unit(const TAGPU_PDUNIT* u)
{
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_BODY);
    if (!g) return;
    upload_pose(u);
    x_glUniform4f(u_anchor, u->ax, u->ay, u->wx0, u->wz0);
    glUniform1f(u_enc, u->enc);
    glUniform1i(u_fog, u->fog);
    glUniform1f(u_alpha, u->alpha);
    glUniform1f(u_waterT, u->waterT);
    glUniform1f(u_digT, u->digT);
    glUniform1i(u_waterMode, u->waterMode);
    glUniform1i(u_nanoOn, u->nanoOn);
    if (u->nanoOn) {
        glUniform1f(u_nanoT, u->nanoT);
        x_glUniform3f(u_nanoC, u->nanoC[0], u->nanoC[1], u->nanoC[2]);
    }
    x_glUniform3f(u_cast, u->cast[0], u->cast[1], u->cast[2]);
    glBindVertexArray(m->vao);
    x_glDrawArrays(GL_TRIANGLES, g->first[TAGPU_PB_BODY], g->count[TAGPU_PB_BODY]);
    s_units++;
    s_tris += (unsigned)g->count[TAGPU_PB_BODY] / 3;
}

/* ---- the Classic silhouette shadow -------------------------------------- */
void tagpu_posedraw_shadow_begin(void)
{
    if (s_state != 1) return;
    glUseProgram(s_prog);
    glUniform1i(u_shadow, 1);
    glUniform1i(u_depthPass, 0);
    glUniform1i(u_range, PD_R_BODY);
    x_glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
}

void tagpu_posedraw_shadow_set(const TAGPU_PDUNIT* u, float offX, float offY,
                               float waterT, float digT)
{
    const TAGPU_PBMAT* m;
    if (!unit_ok(u, &m, TAGPU_PB_BODY)) return;
    upload_pose(u);
    x_glUniform4f(u_anchor, u->ax, u->ay, u->wx0, u->wz0);
    glUniform1f(u_enc, u->enc);
    glUniform1i(u_fog, u->fog);
    x_glUniform2f(u_off, offX, offY);
    glUniform1f(u_waterT, waterT);
    glUniform1f(u_digT, digT);
    x_glUniform3f(u_cast, u->cast[0], u->cast[1], u->cast[2]);
    glBindVertexArray(m->vao);
}

/* the draw alone, against whatever `_shadow_set` left bound: the stencil pass
   needs the identical geometry twice and must not re-upload between them */
void tagpu_posedraw_redraw(const TAGPU_PDUNIT* u)
{
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_BODY);
    if (!g) return;
    x_glDrawArrays(GL_TRIANGLES, g->first[TAGPU_PB_BODY], g->count[TAGPU_PB_BODY]);
}

void tagpu_posedraw_end(void)
{
    if (s_state != 1) return;
    glBindVertexArray(0);
}

/* ---- the structure-shadow slant (G16 step 6) ---------------------------- */
/* `emit_slant`'s range. The uniforms it does NOT set are as deliberate as the
   ones it does: uAlpha and uWaterMode are never read on this path, because the
   fragment shader's `uShadow == 1` branch returns before either. */
void tagpu_posedraw_slant_begin(void)
{
    if (s_state != 1) return;
    glUseProgram(s_prog);
    glUniform1i(u_shadow, 1);
    glUniform1i(u_depthPass, 0);
    glUniform1i(u_nanoOn, 0);
    glUniform1i(u_range, PD_R_SLANT);
    x_glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
}

void tagpu_posedraw_slant_set(const TAGPU_PDUNIT* u, float offX, float offY)
{
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_SLANT);
    if (!g) return;
    upload_pose(u);
    x_glUniform4f(u_anchor, u->ax, u->ay, u->wx0, u->wz0);
    glUniform1f(u_enc, u->enc);
    glUniform1i(u_fog, u->fog);
    x_glUniform2f(u_off, offX, offY);
    /* THE STRUCTURE BRANCH BLITS ITS CACHED SPRITE AS BUILT (0x459319,
       0x4595E9 straight after 0x45A790): the waterline erase 0x4BA1B0 belongs
       to the COMPLETED branch and the digger's inline branch only, so a
       building on the shore keeps the whole slant. Getting this wrong is what
       erased the Kbot lab's shadow below the waterline until G14j, so it is
       pinned here rather than passed in. */
    glUniform1f(u_waterT, -1e9f);
    glUniform1f(u_digT,   -1e9f);
    x_glUniform3f(u_cast, u->cast[0], u->cast[1], u->cast[2]);
    glBindVertexArray(m->vao);
    /* counted here rather than in _slant_redraw: the stencil dance draws the
       same geometry twice and the count is of casters, not of draws */
    s_slantU++;
    s_slantT += (unsigned)g->count[TAGPU_PB_SLANT] / 3;
}

void tagpu_posedraw_slant_redraw(const TAGPU_PDUNIT* u)
{
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_SLANT);
    if (!g) return;
    x_glDrawArrays(GL_TRIANGLES, g->first[TAGPU_PB_SLANT], g->count[TAGPU_PB_SLANT]);
}

/* ---- the nanoframe wireframe (G16 step 6) ------------------------------- */
void tagpu_posedraw_wire_begin(void)
{
    if (s_state != 1) return;
    glUseProgram(s_prog);
    glUniform1i(u_shadow, 0);
    glUniform1i(u_depthPass, 0);
    /* the wireframe carries its own colour and must not be re-classified by
       the build-state recolour it is drawn beside (the CPU path clears the
       same uniform before its own line draws) */
    glUniform1i(u_nanoOn, 0);
    glUniform1i(u_waterMode, 0);
    glUniform1f(u_alpha, 1.0f);
    x_glUniform2f(u_off, 0.0f, 0.0f);
    x_glUniform3f(u_cast, 0.0f, 0.0f, 1.0f);
    glUniform1i(u_range, PD_R_WIRE);
    x_glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
}

void tagpu_posedraw_wire_unit(const TAGPU_PDUNIT* u, float wire)
{
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_WIRE);
    if (!g) return;
    upload_pose(u);
    x_glUniform4f(u_anchor, u->ax, u->ay, u->wx0, u->wz0);
    glUniform1f(u_enc, u->enc);
    glUniform1i(u_fog, u->fog);
    glUniform1f(u_wire, wire);
    glUniform1f(u_waterT, u->waterT);
    glUniform1f(u_digT, u->digT);
    glBindVertexArray(m->vao);
    x_glDrawArrays(GL_LINES, g->first[TAGPU_PB_WIRE], g->count[TAGPU_PB_WIRE]);
    s_wireU++;
    s_wireL += (unsigned)g->count[TAGPU_PB_WIRE] / 2;
}

/* ---- the shadow-depth twin ---------------------------------------------- */
void tagpu_posedraw_depth_begin(const float* shadowMat)
{
    if (s_state != 1 || !shadowMat) return;
    glUseProgram(s_dprog);
    glUniformMatrix4fv(d_shadowMat, 1, GL_FALSE, shadowMat);
    glUniform1i(d_depthPass, 1);
    glUniform1i(d_range, PD_R_BODY);
    /* the depth pass takes gl_Position from uShadowMat alone, but the vertex
       shader is the body's, so the projection uniforms it also evaluates must
       hold something finite — a zero uGame would make the discarded branch NaN
       on a strict driver */
    x_glUniform2f(d_game, 1.0f, 1.0f);
    x_glUniform2f(d_off, 0.0f, 0.0f);
    glUniform1f(d_zoom, 1.0f);
    x_glUniform2f(d_zoomC, 0.0f, 0.0f);
    glUniform1f(d_depthScale, 1.0f);
    x_glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
}

void tagpu_posedraw_depth_unit(const TAGPU_PDUNIT* u)
{
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_BODY);
    if (!g) return;
    upload_pose(u);
    x_glUniform4f(d_anchor, u->ax, u->ay, u->wx0, u->wz0);
    glUniform1f(d_enc, u->enc);
    x_glUniform3f(d_cast, u->cast[0], u->cast[1], u->cast[2]);
    glBindVertexArray(m->vao);
    x_glDrawArrays(GL_TRIANGLES, g->first[TAGPU_PB_BODY], g->count[TAGPU_PB_BODY]);
}

/* ---- the model top ------------------------------------------------------ */
/* `s_emitTop` was the highest posed y emit_node saw while it wrote the
   vertices; nothing writes them here, so it comes off each piece's baked rest
   AABB through that piece's pose matrix (gpu-posing.md §4). The two documented
   deviations are on the AABB itself — see tagpu_posebake.h. */
float tagpu_posedraw_top(const TAGPU_PDUNIT* u)
{
    const TAGPU_PBGEOM* g = (const TAGPU_PBGEOM*)u->geom;
    float top = -1e9f;
    int p, c, np;
    if (!g || !u->pose) return 0.0f;
    np = g->nparts;
    if (np > u->npose) np = u->npose;
    if (np > TAGPU_PBMAXPIECE) np = TAGPU_PBMAXPIECE;
    for (p = 0; p < np; p++) {
        const float* M = u->pose + (size_t)p * 12;
        /* an all-zero matrix is a piece the unit is not showing: emit_node
           never reached its vertices either */
        if (!g->pbody[p]) continue;
        if (M[0] == 0.0f && M[1] == 0.0f && M[2] == 0.0f && M[3] == 0.0f &&
            M[4] == 0.0f && M[5] == 0.0f && M[6] == 0.0f && M[7] == 0.0f &&
            M[8] == 0.0f && M[9] == 0.0f && M[10] == 0.0f && M[11] == 0.0f)
            continue;
        for (c = 0; c < 8; c++) {
            float x = (c & 1) ? g->pmx[p][0] : g->pmn[p][0];
            float y = (c & 2) ? g->pmx[p][1] : g->pmn[p][1];
            float z = (c & 4) ? g->pmx[p][2] : g->pmn[p][2];
            float wy = M[4] * x + M[5] * y + M[6] * z + M[7];
            if (wy > top) top = wy;
        }
    }
    return top > 0.0f ? top : 0.0f;
}

/* ---- frame, reset, stats ------------------------------------------------ */
void tagpu_posedraw_frame(void)
{
    s_units = s_tris = 0;
    s_slantU = s_slantT = s_wireU = s_wireL = 0;
}

void tagpu_posedraw_glreset(void)
{
    /* the context is gone: the ids are already invalid and must not be deleted
       against the new one. The next ready() rebuilds. */
    s_prog = s_dprog = s_ubo = 0;
    s_state = 0;
}

int tagpu_posedraw_stats(char* out, int n)
{
    int k;
    if (s_state != 1 || n <= 0) { if (out && n > 0) out[0] = 0; return 0; }
    k = _snprintf(out, n, " posed=%u/%utri", s_units, s_tris);
    if (k < 0 || k >= n) return k;
    /* the two step-6 ranges, and only when a scene actually has them: a screen
       with no structure casting and nothing under construction should not carry
       two zeroes that read as a pass that ran and found nothing */
    if (s_slantU)
        k += _snprintf(out + k, n - k, " slant=%u/%utri", s_slantU, s_slantT);
    if (k < 0 || k >= n) return k;
    if (s_wireU)
        k += _snprintf(out + k, n - k, " wire=%u/%uln", s_wireU, s_wireL);
    if (k < 0 || k >= n) return k;
    if (s_overPiece)
        k += _snprintf(out + k, n - k, " OVER-PIECE");
    return k;
}
