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
#include "tagpu_abshot.h"  /* the GL half of the Phase G A/B */
#include "tagpu_vk.h"      /* tagpu_vk_armed(): whether to publish at all */
#include "tagpu_pal.h"
#include "tagpu_posebake.h"

#define STR2(x) #x
#define STR(x)  STR2(x)

/* the block: 3 rows per piece, then two packed floats per piece — `shaded`
   for the body's SHD row and a visibility WORD (0 / 1 / 3) the slant and the
   wire read. Two arrays rather than bits of one float because the second costs
   1 KB (14336 against GL 3.1's guaranteed 16384) and a packed pair costs every
   reader of it a decode; the second is a word rather than two more arrays
   because its two values nest — a slant caster is always visible. */
/* the numbers themselves are tagpu_posebake.h's, shared with the Vulkan
   edition of this pass so that the two cannot describe the block differently */
#define PD_ROWS    TAGPU_PD_ROWS
#define PD_FLAGV   TAGPU_PD_FLAGV
#define PD_FLAGOFF TAGPU_PD_FLAGOFF
#define PD_VISOFF  TAGPU_PD_VISOFF
#define PD_BLOCK   TAGPU_PD_BLOCK

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

/* ---- THE VULKAN LANE'S HAND-OVER (Phase G / G19e, the SIXTH world pass) ----
   The contract is tagpu_posedraw.h's; this is the state behind it.

   NOTHING IS PUBLISHED ON A SHIPPED FRAME. `s_mirrorWant` is the latch the
   frame beat sets when the Vulkan lane is armed, and it is also what makes
   tagpu_posebake.c keep the two streams at all. While it is 0 the arenas are
   never allocated, no record is written, and the cost on the path every player
   runs is the one branch at the top of `pd_record`. */
static int          s_mirrorWant;
static unsigned     s_frame;
static TAGPU_PDHAND s_pub;
static int          s_pubHave;

/* WINDOWS, because `_begin`/`_end` is not once a frame. The build ghost draws
   through the very same entry points (tagpu_posedraw.h's `ghost` field says
   why) and opens a pair of its own after the units'.

   SINCE LANDING 6 EVERY WINDOW RECORDS, and this comment said the opposite:
   that the first window publishes and everything a later one draws is one more
   thing the Vulkan lane has no copy of. That was true, and it is what stood the
   unit pass down for the whole of any building placement. What is left of the
   first-window rule is narrower and is two separate things: the first window
   PUBLISHES THE VIEW (one publication a frame is right), and the A/B's capture
   is bracketed around a NON-GHOST window -- not around "the first", because
   with no posed unit on screen the ghost's window IS the first. */
static int          s_win;          /* windows opened this frame              */
static int          s_recording;    /* inside a recording window              */
static unsigned     s_nghost;       /* ghosts recorded this frame             */
static int          s_abTaking;     /* THIS window opened the capture         */
static int          s_saidNoPub, s_saidFrame;
static int          s_abClaim;      /* it reached the disk; frame-scoped      */
static int          s_other;        /* draws the hand-over carries no copy of */

static TAGPU_PDUREC* s_rec;       static unsigned s_recCap, s_nrec;
static float*        s_rowArena;  static unsigned s_rowCap, s_nrow;
static float*        s_flagArena; static unsigned s_flagCap, s_nflag;
static float*        s_visArena;  static unsigned s_visCap;
static int           s_saidCap, s_saidRoom;
static int           s_polled;      /* the 30-frame lever beat has run once   */

/* the depth twin, which runs EARLIER in the frame than the bodies */
static int          s_depthOn, s_ncast;
static float        s_depthMat[16];

/* THE NANOFRAME PAIR IS STICKY ON THE GL SIDE and is carried the same way
   here: the twin writes uNanoT and uNanoC only on a unit with `nanoOn`, so a
   unit without one is drawn against whatever the last one that had it left in
   the program. Reset with the frame, because a freshly linked program holds
   zero and the first unit of a session has nothing behind it. */
static float        s_lastNanoT, s_lastNanoC[3];

/* THE FOG GRID'S COPY. `cells * 2` is the size the GL upload itself was given
   for this grid, so the read is bounded by the bound the GL lane already
   trusts; the cap bounds the ALLOCATION, and the Vulkan pass re-checks it
   because a bound in one file is a bound only while both are read together. */
#define PD_FOG_MAXDIM 1024
static unsigned short* s_fogCopy;
static int             s_fogCopyCells;

/* the A/B lever, the same shape as every other ported pass's */
#define PD_ABFILE "tagpu_posedraw.ab"
#define PD_ABOUT  "tagpu_posedraw_gl.ppm"
static int          s_ab, s_abDone, s_abFrame;
static TAGPU_ABSHOT s_shot;

/* Grow one of the three arenas. A failure is not an error: the frame simply
   hands nothing over and the Vulkan pass draws nothing, which is a pass that
   stood down rather than one that drew a different picture. */
static int arena_room(void** p, unsigned* cap, unsigned need, size_t elem)
{
    void* q;
    unsigned want;
    if (need <= *cap) return 1;
    want = *cap ? *cap * 2 : 1024;
    while (want < need) want *= 2;
    q = realloc(*p, (size_t)want * elem);
    if (!q) {
        if (!s_saidRoom) {
            s_saidRoom = 1;
            plog("posedraw: the Vulkan hand-over's arena would not grow - nothing "
                 "is handed over while that is true (the GL lane is unaffected)");
        }
        return 0;
    }
    *p = q; *cap = want;
    return 1;
}

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

    /* THE VULKAN-ONLY LANE ARMS WITHOUT A PROGRAM, because on that lane this
       pass never draws: every `tagpu_posedraw_*` entry point below is called
       from tagpu_native.c's GL composite, which that lane exits before. What it
       DOES need is for this function to answer yes, because the per-unit gather
       is gated on it (`if (!pdReady) continue;`) and the gather is what feeds
       the hand-over.

       THE ONE REAL QUESTION IS THE SAME ONE, ASKED OF THE OTHER DEVICE. The
       pose block is a compile-time size (`PD_BLOCK`); both lanes only ask
       whether the device can hold it, so the GL path's
       GL_MAX_UNIFORM_BLOCK_SIZE check becomes `maxUniformBufferRange` here.
       A 0 there means no device yet, not a device that cannot: `s_state` goes
       back to 0 so the next frame asks again, rather than latching a refusal
       during the lane's ~200 ms bring-up. [The vulkan-only plan, 4b-2.] */
    if (tagpu_vk_owns_present()) {
        int lim = tagpu_vk_max_uniform_range();
        if (lim <= 0) { s_state = 0; return 0; }
        if (lim < PD_BLOCK) {
            _snprintf(b, sizeof b,
                      "posedraw: refused — the device's maxUniformBufferRange is %d, "
                      "the pose block needs %d", lim, PD_BLOCK);
            b[sizeof b - 1] = 0;
            plog(b);
            s_state = 2;
            return 0;
        }
        s_state = 1;
        _snprintf(b, sizeof b,
                  "posedraw: armed for the Vulkan lane — no GL program, pose block "
                  "%d bytes (%d pieces), device limit %d", PD_BLOCK, TAGPU_PBMAXPIECE, lim);
        b[sizeof b - 1] = 0;
        plog(b);
        return 1;
    }

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
/* The two packed per-piece words, one float a piece, zero-filled out to the
   vec4 the block stores them in. FACTORED OUT so that the hand-over carries
   the bytes this upload writes rather than a second conversion of the same two
   arrays -- the Vulkan pass writes `nf * 16` bytes at PD_FLAGOFF and PD_VISOFF
   exactly as the two glBufferSubData below do. Returns the piece count it
   wrote, clamped, or 0 for a unit with no pose. */
static int pose_words(const TAGPU_PDUNIT* u, float* flags, float* vis)
{
    int np = u->npose, i, nf;
    if (np > TAGPU_PBMAXPIECE) np = TAGPU_PBMAXPIECE;
    if (np <= 0) return 0;
    nf = (np + 3) / 4;
    memset(flags, 0, (size_t)nf * 4 * sizeof(float));
    memset(vis,   0, (size_t)nf * 4 * sizeof(float));
    for (i = 0; i < np; i++) {
        flags[i] = (u->shaded && u->shaded[i]) ? 1.0f : 0.0f;
        vis[i]   = u->pvis ? (float)u->pvis[i] : 1.0f;
    }
    return np;
}

static void upload_pose(const TAGPU_PDUNIT* u)
{
    /* THE BLOCK UPLOAD IS GL; `pose_words` is the pass. `pd_record` re-derives
       the same words from the same function for the hand-over, so on a lane
       with no GL there is nothing to upload and nothing is lost.
       [The vulkan-only plan, landing 4b-2.] */
    static float flags[PD_FLAGV * 4], vis[PD_FLAGV * 4];
    int np = pose_words(u, flags, vis), nf;
    if (np <= 0) return;
    if (tagpu_vk_owns_present()) return;
    nf = (np + 3) / 4;
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

/* ---- the Vulkan lane's record ------------------------------------------- */
/* The two uniform blocks as `_begin` is about to leave them, taken from the
   same sources rather than read back out of the program. Everything the twin
   does NOT set is left at the zero a freshly linked program holds, which is
   what the twin is actually drawing with -- see tagpu_posedraw.h on uLambert,
   which is the one where that matters. */
static void pd_view_publish(const TAGPU_PDVIEW* v)
{
    const TAGPU_LIGHT* L = tagpu_classicpp_light();
    memset(&s_pub, 0, sizeof s_pub);
    s_pub.frame = s_frame;
    s_pub.gw = v->game[0]; s_pub.gh = v->game[1];
    s_pub.zoom = v->zoom;
    s_pub.zoomCx = v->zoomC[0]; s_pub.zoomCy = v->zoomC[1];
    s_pub.depthScale = v->depthScale;
    s_pub.shd[0] = (float)v->shNeutral; s_pub.shd[1] = (float)v->shDir;

    s_pub.restored = (tagpu_r3d_atlas_rgbref() && tagpu_classicpp_on()) ? 1 : 0;
    s_pub.scafOn = v->scafOn ? 1 : 0;
    s_pub.scafP[0] = v->scafP[0]; s_pub.scafP[1] = v->scafP[1];
    s_pub.scafP[2] = v->scafP[2]; s_pub.scafP[3] = v->scafP[3];
    s_pub.ss = v->ss;
    s_pub.fogOrgX = v->fogOrg[0]; s_pub.fogOrgY = v->fogOrg[1];
    s_pub.fogCols = v->fogDim[0]; s_pub.fogRows = v->fogDim[1];
    s_pub.lit = v->lit ? 1 : 0;
    s_pub.lambert = 0;                 /* the twin never sets it -- see the header */
    s_pub.sun[0] = v->sun[0]; s_pub.sun[1] = v->sun[1]; s_pub.sun[2] = v->sun[2];
    s_pub.amb = v->amb; s_pub.norm = v->norm;

    /* THE CAST-SHADOW READ-BACK BLOCK, from the same places tagpu_shadow_apply
       reads them: it writes uShadowOn and RETURNS when no map is live, so on
       such a frame the rest stays at the program's zero and is published zero. */
    s_pub.shadowOn = tagpu_shadow_live() ? 1 : 0;
    if (s_pub.shadowOn) {
        memcpy(s_pub.shadowMat, tagpu_shadow_mat(), sizeof s_pub.shadowMat);
        s_pub.shadowSun[0] = L->shadowSun[0];
        s_pub.shadowSun[1] = L->shadowSun[1];
        s_pub.shadowSun[2] = L->shadowSun[2];
        tagpu_shadow_scale(s_pub.shScale);
        s_pub.penumbra = L->penumbra;
        s_pub.shade = L->shade;
    }

    /* the viewport and the clip, which the scaffold rect already carries as
       four floats -- published as the integers the scissor was set from */
    s_pub.vpL = (int)v->scafP[0]; s_pub.vpT = (int)v->scafP[1];
    s_pub.vw  = (int)v->scafP[2]; s_pub.vh  = (int)v->scafP[3];
    s_pub.scissorOn = tagpu_native_scissor_on();
    s_pub.ss_i = (int)(v->ss > 0.0f ? v->ss : 1.0f);

    /* THE TEXELS. The two mirrors are asked for HERE rather than on the arm
       beat, because `tagpu_r3d_atlas_mirror_want` needs the atlas to have its
       dimensions and that is not true of the first frames of a session; it is
       idempotent, so asking every publish costs one branch once it is there. */
    tagpu_r3d_atlas_mirror_want();
    s_pub.atlas = tagpu_r3d_atlas_mirror(&s_pub.atlasDim, &s_pub.atlasRows,
                                         &s_pub.atlasSerial);
    /* AND THE RESTORED TWIN, on the same beat and for the same reason -- it too
       needs the atlas to have its dimensions, and asking is idempotent. The
       STEP is here rather than in the arm beat because it is a glReadPixels off
       an FBO and this is the render thread with the context current; it is a
       no-op until the mirror is armed and again once the restorer has stopped
       painting. The accessor returns NULL until a step has covered rows, so
       nothing is published that a consumer could not upload. */
    tagpu_r3d_atlas_mirror_rgb_want();
    tagpu_r3d_atlas_mirror_rgb_step();
    s_pub.atlasRgb = tagpu_r3d_atlas_mirror_rgb(NULL, &s_pub.atlasRgbRows,
                                                &s_pub.atlasRgbMips,
                                                &s_pub.atlasRgbAniso,
                                                &s_pub.atlasRgbSerial);
    /* ...OR THE REQUEST INSTEAD OF THE PICTURE, and never both (landing 7e-2).
       Written as an either/or here as well as guaranteed by the arm freeing the
       mirror: the arm is a poll that can land on any frame, and "mutually
       exclusive by construction" was already wrong once on this plan for
       exactly that reason. `atlasRgbAniso` is published on BOTH paths -- it is
       the twin's sampler ratio, not the mirror's -- so it is read from the list
       accessor here rather than left at whatever the mirror call zeroed it to. */
    {
        float aniso = 0.0f;
        const TAGPU_RGLSL_FRAME* fr =
            tagpu_r3d_atlas_restore_list(&s_pub.restoreDim, &s_pub.restoreN,
                                         &s_pub.restoreGen, &s_pub.restoreRepaint,
                                         &s_pub.restoreBlanks, &s_pub.restoreMips,
                                         &aniso);
        if (fr) {
            s_pub.atlasRgb       = NULL;
            s_pub.atlasRgbRows   = 0;
            s_pub.atlasRgbMips   = 0;
            s_pub.atlasRgbSerial = 0;
            s_pub.atlasRgbAniso  = aniso;
            s_pub.restoreFrames  = fr;
        } else {
            s_pub.restoreFrames  = NULL;
            s_pub.restoreN = 0; s_pub.restoreGen = 0;
            s_pub.restoreRepaint = 0; s_pub.restoreBlanks = 0;
            s_pub.restoreDim = 0; s_pub.restoreMips = 0;
        }
    }
    s_pub.lut = tagpu_r3d_lut_mirror(&s_pub.lutW, &s_pub.lutH, &s_pub.lutSerial);
    s_pub.pal = tagpu_pal_live(); s_pub.palSerial = tagpu_pal_serial();
    s_pub.fogLut = tagpu_native_foglut();
    {   /* THE GRID IS COPIED. It points into a frame packet the game thread
           reuses, and `cells` -- not cols*rows -- is what the packet actually
           allocated, so it is the bound the copy is made against. */
        int cols = 0, rows = 0, cells = 0;
        const unsigned short* g = tagpu_native_foggrid(&cols, &rows, &cells);
        int want = cols * rows;
        if (g && cols > 0 && rows > 0 && want <= cells &&
            cols <= PD_FOG_MAXDIM && rows <= PD_FOG_MAXDIM) {
            if (want > s_fogCopyCells) {
                unsigned short* n =
                    (unsigned short*)realloc(s_fogCopy, (size_t)want * 2);
                if (n) { s_fogCopy = n; s_fogCopyCells = want; }
            }
            if (s_fogCopy && want <= s_fogCopyCells) {
                memcpy(s_fogCopy, g, (size_t)want * 2);
                s_pub.fogGrid = s_fogCopy;
                s_pub.fogGridCols = cols; s_pub.fogGridRows = rows;
            }
        }
    }

    s_pub.depthOn = s_depthOn;
    if (s_depthOn) memcpy(s_pub.castMat, s_depthMat, sizeof s_pub.castMat);

    s_nrec = 0; s_nrow = 0; s_nflag = 0; s_ncast = 0;
}


/* One drawn unit, appended to this frame's hand-over. Called from
   `tagpu_posedraw_unit` AFTER its own `unit_ok` has passed, so everything here
   is known good: the two bake entries agree about the vertex count, the pose
   carries at least `nparts` pieces, and the body range is non-empty.

   THE MIRRORS ARE NOT READ HERE, only named. tagpu_posebake.c owns them and
   evicts them, so the pass that reads them asks that module at the instant of
   the read, with the serial recorded here -- see tagpu_posebake.h. What this
   function stores is an identity, not a pointer it has dereferenced. */
static void pd_record(const TAGPU_PDUNIT* u, const TAGPU_PBGEOM* g,
                      const TAGPU_PBMAT* m)
{
    static float flags[PD_FLAGV * 4], vis[PD_FLAGV * 4];
    TAGPU_PDUREC* r;
    int np, nf;

    /* EVERY DRAW THIS FRAME THAT IS NOT RECORDED IS COUNTED INSTEAD, and that
       is the refusal the design rests on: a frame the Vulkan lane draws with
       one unit missing is a different frame, not a slightly worse one.

       THE BUILD GHOST IS RECORDED SINCE LANDING 6 and no longer counted here.
       It was excluded from the start, and the cost of that was measured before
       the port: `ghost.on` is a play default, so for as long as a building
       placement was open `otherDraws` was non-zero and the Vulkan unit pass
       drew NOTHING -- not the ghost, not the units. A ghost rides this same
       entry point with the same uniforms and differs in exactly two things:
       `alpha` (0.40, which `r->alpha` already carried) and depth writes, which
       the consumer takes with a second pipeline. The other two counted cases --
       an arena that would not grow, a unit with no pieces -- stay refusals. */
    if (!s_recording) { s_other++; return; }
    if (s_nrec >= (unsigned)TAGPU_PD_MAXHAND) {
        if (!s_saidCap) {
            s_saidCap = 1;
            plog("posedraw: more posed units on screen than the Vulkan hand-over "
                 "carries - the Vulkan edition stands down on such a frame "
                 "(TAGPU_PD_MAXHAND)");
        }
        s_other++;
        return;
    }
    np = pose_words(u, flags, vis);
    if (np <= 0) { s_other++; return; }
    nf = (np + 3) / 4;

    /* The four arenas grow together or the unit is not recorded at all -- a
       record written against an arena that would not grow is the one shape
       this must never take. */
    if (!arena_room((void**)&s_rec, &s_recCap, s_nrec + 1, sizeof s_rec[0]) ||
        !arena_room((void**)&s_rowArena, &s_rowCap, s_nrow + (unsigned)np * 12,
                    sizeof(float)) ||
        !arena_room((void**)&s_flagArena, &s_flagCap, s_nflag + (unsigned)nf * 4,
                    sizeof(float)) ||
        !arena_room((void**)&s_visArena, &s_visCap, s_flagCap, sizeof(float)))
        { s_other++; return; }

    r = &s_rec[s_nrec++];
    memset(r, 0, sizeof *r);
    r->geom = g; r->mat = m;
    r->geomSerial = g->serial; r->matSerial = m->serial;
    r->nvert = g->nvert;
    r->first = g->first[TAGPU_PB_BODY];
    r->count = g->count[TAGPU_PB_BODY];
    r->npose = np;
    r->rowOff = s_nrow / 4;                 /* in vec4, as the header says    */
    /* `arena_room` rounds up to a power of two, so the vis arena can end a
       call larger than the flag one asked for; both are indexed by `flagOff`
       and the one that matters is that neither is ever shorter. */
    r->flagOff = s_nflag;
    memcpy(s_rowArena + s_nrow, u->pose, (size_t)np * 12 * sizeof(float));
    s_nrow += (unsigned)np * 12;
    memcpy(s_flagArena + s_nflag, flags, (size_t)nf * 4 * sizeof(float));
    memcpy(s_visArena  + s_nflag, vis,   (size_t)nf * 4 * sizeof(float));
    s_nflag += (unsigned)nf * 4;

    r->anchor[0] = u->ax; r->anchor[1] = u->ay;
    r->anchor[2] = u->wx0; r->anchor[3] = u->wz0;
    r->enc = u->enc;
    r->cast[0] = u->cast[0]; r->cast[1] = u->cast[1]; r->cast[2] = u->cast[2];
    r->alpha = u->alpha;
    r->waterT = u->waterT; r->digT = u->digT;
    r->fog = u->fog; r->waterMode = u->waterMode; r->nanoOn = u->nanoOn;
    /* STICKY, as the twin's program is -- tagpu_posedraw.h says why. The two
       are only written on a unit that has `nanoOn`, so a unit without one
       carries the last value the GL program was given. */
    if (u->nanoOn) {
        s_lastNanoT = u->nanoT;
        s_lastNanoC[0] = u->nanoC[0];
        s_lastNanoC[1] = u->nanoC[1];
        s_lastNanoC[2] = u->nanoC[2];
    }
    r->nanoT = s_lastNanoT;
    r->nanoC[0] = s_lastNanoC[0];
    r->nanoC[1] = s_lastNanoC[1];
    r->nanoC[2] = s_lastNanoC[2];
    /* THE DEPTH LOOP DREW EXACTLY THESE UNITS WITH `castSkip` CLEAR, earlier in
       the same frame and over the same array with the same `unit_ok` gate, so
       this reproduces which casters are in the map rather than guessing at it. */
    r->ghost = u->ghost ? 1 : 0;
    if (r->ghost) s_nghost++;
    r->casts = (s_depthOn && !u->castSkip) ? 1 : 0;
    if (r->casts) s_ncast++;
}

/* ---- bodies ------------------------------------------------------------- */
static void pd_begin(const TAGPU_PDVIEW* v, int ghostWindow);

void tagpu_posedraw_begin(const TAGPU_PDVIEW* v)       { pd_begin(v, 0); }
void tagpu_posedraw_begin_ghost(const TAGPU_PDVIEW* v) { pd_begin(v, 1); }

static void pd_begin(const TAGPU_PDVIEW* v, int ghostWindow)
{
    const int gl_draws = !tagpu_vk_owns_present();
    if (s_state != 1) return;
    /* THE PUBLISH WINDOW. EVERY window of a frame records, since landing 6 --
       the build ghost draws in a SECOND one (ghost_pass opens its own, after
       the wire and the replacement meshes), and until landing 6 that window
       recorded nothing and everything in it was counted against the hand-over
       instead. That count is what stood the Vulkan unit pass down for the whole
       of any building placement.

       THE FIRST WINDOW OF A FRAME PUBLISHES THE VIEW, whichever window that is.
       THE A/B IS BRACKETED AROUND A NON-GHOST WINDOW AND NOTHING ELSE, and that
       is NOT the same test: with no posed unit on screen `tagpu_native.c` skips
       the unit window and the ghost's is the FIRST, so bracketing "the first"
       would black the frame around a pass whose draws the Vulkan lane makes in
       a different stage entirely. The two were one `s_win == 0` test until
       landing 6's review found they disagree on exactly that frame. */
    {
        int first = (s_win++ == 0);
        s_recording = s_mirrorWant;
        if (first && s_recording) {
            pd_view_publish(v);
        /* THE GL HALF OF THE PHASE G A/B (tagpu_abshot.h): black the frame
           immediately before this pass draws, read it back immediately after.
           DEPTH too, because these draws test it -- without the depth clear the
           GL half would be tested against the terrain and the features this
           frame already put down while the Vulkan half starts from a cleared
           attachment, and every fragment the two disagree about would read as
           a port failure. SCISSOR because the native pass clips these draws to
           the world viewport and measuring them unclipped measures a pass the
           player never sees. */
        }
        if (!ghostWindow && s_recording && s_ab && !s_abDone) {
            s_abTaking = 1;
            /* `tagpu_abshot_begin` is GL too, though it is not spelled `gl*`:
               it clears the frame and saves the state it moves. On the
               vulkan-only lane there is no GL half to take. */
            if (gl_draws)
                tagpu_abshot_begin(&s_shot, TAGPU_ABSHOT_DEPTH | TAGPU_ABSHOT_SCISSOR |
                                            TAGPU_ABSHOT_TOPDOWN);
        }
    }
    /* THE PROGRAM AND ITS UNIFORMS ARE GL. Everything above -- the
       recording window (`s_recording = s_mirrorWant`) and the A/B's arming --
       is the pass, and is what makes a hand-over happen at all.
       [The vulkan-only plan, landing 4b-2.] */
    if (gl_draws) {
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
    const int gl_draws = !tagpu_vk_owns_present();
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_BODY);
    if (!g) return;
    upload_pose(u);
    /* THE UNIFORMS AND THE DRAW ARE GL; `upload_pose` above and
       `pd_record` below are the pass, and the twin draws from the record.
       [The vulkan-only plan, landing 4b-2.] */
    if (gl_draws) {
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
    }
    pd_record(u, g, m);
    /* A GHOST IS NOT A UNIT. It rides this same entry point on purpose — that
       is the whole of its draw — but the two counters below feed the `posed=N`
       stats and the queued-vs-drawn heartbeat, which reads their inequality as
       "units queued but not drawn". Counting ghosts there fires that alarm
       every frame one draws and inflates the unit and triangle totals, so the
       record says which it is and only the units are counted. The ghost pass
       has its own `drawn=` counter for what it drew. */
    if (!u->ghost) {
        s_units++;
        s_tris += (unsigned)g->count[TAGPU_PB_BODY] / 3;
    }
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
    const int gl_draws = !tagpu_vk_owns_present();
    if (s_state != 1) return;
    /* THE PUBLISH IS THE PASS; this one call is the draw's.
       [The vulkan-only plan, landing 4b-2.] */
    if (gl_draws) {
        glBindVertexArray(0);
    }
    if (!s_recording) return;
    s_recording = 0;

    /* THE GL HALF IS READ BACK IMMEDIATELY, before anything later in the frame
       draws -- the wire, the replacement meshes and the effects are all still
       to come. `tagpu_abshot_end` returns 1 only when the capture reached the
       disk, and the Vulkan half may be claimed on nothing else: a stale
       _gl.ppm from an earlier run would otherwise be diffed against a fresh
       Vulkan capture of a different frame. */
    /* ONLY THE WINDOW THAT OPENED THE BRACKET CLOSES IT. `s_abDone` alone was
       the test until landing 6's review, and it stopped being enough the moment
       a frame could have more than one window. */
    if (s_abTaking) {
        s_abTaking = 0;
        s_abDone = 1;
        /* AND ON THE TARGET HAVING BEEN UNLINKED, armed on both lanes -- and
           WHERE THERE IS NO GL HALF THE INTENT IS THE CLAIM, as in every other
           ported pass: `tagpu_abshot_end` is not merely refused there, it is
           never called. tagpu_abshot.h has the whole argument. */
        if (gl_draws) {
            int wrote = tagpu_abshot_end(&s_shot, PD_ABOUT, "posedraw");
            int fresh = tagpu_vk_ab_arm("posedraw");
            s_abClaim = wrote && fresh;
        } else {
            s_abClaim = tagpu_vk_ab_arm("posedraw");
        }
    }

    /* PUBLISHED LAST, with the counts this window ended with. A frame that
       recorded no unit still publishes: `otherDraws` may be non-zero, and the
       Vulkan lane has to see that rather than see nothing and draw its own
       idea of an empty frame. */
    s_pub.units = s_rec;   s_pub.nunit = s_nrec;
    s_pub.rows  = s_rowArena;  s_pub.nrow  = s_nrow / 4;
    s_pub.flags = s_flagArena; s_pub.vis   = s_visArena;
    s_pub.nflag = s_nflag;
    s_pub.ncast = s_ncast;
    /* `otherDraws` IS NOT SET HERE, and that is deliberate: the wire, the
       replacement meshes, the slant and the build ghost all draw LATER in this
       frame than this window closes, and a count frozen now would miss exactly
       the draws the refusal exists to catch. It is read at the moment the
       hand-over is taken, which is later in this same iteration of
       render_ogl.c's loop than every GL draw in the frame. */
    /* AND A FRAME THAT DREW A BUILD GHOST CANNOT CLAIM THE PAIR. The GL half is
       blacked and read back around the FIRST window, a few lines above, and the
       ghost draws in a SECOND one -- so the GL capture cannot contain a ghost
       while the Vulkan frame, which is the whole presented image, can. Landing 6
       carries the ghost, so on exactly the frames that have one the A/B stops
       being a valid oracle, and it says so by NOT CLAIMING rather than by
       reporting a difference that is the instrument's own. Landing 6's oracle
       is the two-window comparison instead (gpu-status §2.40), which needs
       neither a bracket nor a single drawing pass. */
    /* `s_abClaim` IS FRAME-SCOPED AND THIS LINE IS IDEMPOTENT, which the first
       version was not: it read `s_pub.ab = s_nghost ? 0 : s_abFrame` and then
       zeroed `s_abFrame`, so a SECOND window's `_end` re-ran it with the value
       already consumed and dropped the claim WHETHER OR NOT a ghost had been
       recorded. A frame whose queued build sites are all off screen opens the
       ghost window -- `ghost_pass` opens it on the site COUNT, before the
       per-site cull -- and records nothing, so the pair was silently lost, with
       `s_abDone` latched so the one-shot never retried and the instrument read
       as a port failure for the rest of the session.
       [Landing 6's review, 2026-09-17.] */
    s_pub.ab = s_nghost ? 0 : s_abClaim;
    s_pubHave = 1;
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
    /* THE FRAME'S DEPTH TWIN RAN, and with this matrix. Recorded rather than
       inferred, because `casts` on a hand-over record means "this unit is IN
       the map the twin drew" -- which is false on any frame where
       tagpu_shadow_begin refused and this block never ran, even though every
       unit's `castSkip` still reads the same. It is set here rather than in
       `_depth_unit` so that a frame whose casters were all skipped still says
       the map was drawn. */
    s_depthOn = 1;
    memcpy(s_depthMat, shadowMat, sizeof s_depthMat);
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

/* ---- the Vulkan lane's hand-over ---------------------------------------- */
int tagpu_posedraw_handover(TAGPU_PDHAND* out, unsigned now)
{
    /* WHICH HALF REFUSED, ONCE. The consumer can say "no hand-over" but only
       this side knows whether there was nothing to give or it was stamped for
       another frame, and those have different causes -- the first is the pass
       standing down, the second is a counter disagreeing across the seam. One
       latch, cleared on the first success. [The vulkan-only plan, 4b-2.] */
    if (!s_pubHave || !out) {
        /* A WINDOW THAT NEVER OPENED IS THE ORDINARY CASE and must not be
           reported: the shell has no posed units, so `s_win` is 0 there on
           every frame and a latch spent on it hides the state that matters.
           What is worth a line is a window that OPENED and published nothing,
           which is a pass standing down mid-frame. */
        if (s_win > 0 && !s_saidNoPub) {
            char b[160];
            s_saidNoPub = 1;
            _snprintf(b, sizeof b, "posedraw: %d window(s) opened for frame %u and "
                      "nothing was published - the pass stood down inside the frame",
                      s_win, now);
            b[sizeof b - 1] = 0;
            plog(b);
        }
        return 0;
    }
    if (s_pub.frame != now && !s_saidFrame) {
        char b[160];
        s_saidFrame = 1;
        _snprintf(b, sizeof b, "posedraw: published frame %u, asked for %u - the "
                  "two counters disagree across the seam", s_pub.frame, now);
        b[sizeof b - 1] = 0;
        plog(b);
    }
    if (s_pubHave && s_pub.frame == now) { s_saidNoPub = 0; s_saidFrame = 0; }
    /* NOT THIS FRAME'S, SO NOT ALIVE. `units`, `rows`, `flags` and `vis` are
       arrays this file REALLOCATES the moment a frame needs more room than the
       last did, and the records name bake entries tagpu_posebake.c evicts. The
       stale hand-over is cleared as well, so the next frame starts honest. */
    if (s_pub.frame != now) { s_pubHave = 0; s_abFrame = 0; return 0; }
    /* THE COUNT IS TAKEN NOW, not when the window closed -- `_end` says why.
       Every GL draw of this frame is behind us at this point, because the
       whole native pass runs earlier in this iteration of render_ogl.c's loop
       than the tagpu_vk_frame that calls this. */
    s_pub.otherDraws = s_other;
    *out = s_pub;
    s_pubHave = 0; s_abFrame = 0;
    return 1;
}

/* ---- frame, reset, stats ------------------------------------------------ */
void tagpu_posedraw_frame(unsigned frame_counter)
{
    s_units = s_tris = 0;
    s_slantU = s_slantT = s_wireU = s_wireL = 0;
    /* THE HAND-OVER'S FRAME, and everything that is per-frame about it. The
       previous frame's publish is dropped here rather than left standing: the
       stamp would refuse it anyway, and clearing it is what makes that a
       belt-and-braces check instead of the only one. */
    s_frame = frame_counter;
    s_pubHave = 0;
    s_win = 0; s_recording = 0; s_other = 0; s_nghost = 0;
    s_abTaking = 0; s_abClaim = 0;
    s_depthOn = 0; s_ncast = 0;
    s_lastNanoT = 0.0f;
    s_lastNanoC[0] = s_lastNanoC[1] = s_lastNanoC[2] = 0.0f;
    /* The mirror latch and the A/B lever, on the same 30-frame beat every other
       lever in this stack is read on -- these are file probes, and one a frame
       is a syscall nobody asked for. The latch only ever goes 0 -> 1: what it
       turns on is tagpu_posebake.c keeping its two streams, and that module
       re-bakes the inventory once when it does. */
    if ((frame_counter % 30) == 0 || !s_polled) {
        s_polled = 1;
        if (!s_mirrorWant && tagpu_vk_armed()) s_mirrorWant = 1;
        s_ab = GetFileAttributesA(PD_ABFILE) != INVALID_FILE_ATTRIBUTES;
        if (!s_ab) s_abDone = 0;
    }
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
