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
   bytes, with headroom rather than sitting exactly on any limit. The headroom
   is not assumed: the device's `maxUniformBufferRange` is read at bring-up and
   the pass refuses to arm below it. Since G16 step 8 there is no CPU emitter to
   leave those units to, so the refusal is published instead
   (`tagpu_posedraw_live`) and `owndraw` stops skipping the engine's own unit
   rasterise — the engine draws them, rather than nothing drawing them.

   A HIDDEN PIECE ARRIVES AS AN ALL-ZERO MATRIX and collapses its triangles onto
   the model origin; a face the material stream has nothing for carries the skip
   flag and is pushed outside the clip volume. Neither changes the vertex count,
   which is what lets the two buffers be rebuilt independently.

   RENDER THREAD ONLY. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tagpu_model3do.h"
#include "tagpu_posebake.h"
#include "tagpu_posedraw.h"
#include "tagpu_native.h"
#include "tagpu_render3do.h"
#include "tagpu_classicpp.h"
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

/* `uRange`'s three values (BODY 0, SLANT 1, WIRE 2) were #defined here for the
   draws that passed them. All three draws went with landing 11-5d, and the
   Vulkan consumer writes the number itself (tagpu_vk_unit.c's `b.i[40]`), so
   the macros had no use left and are not kept as documentation: the shader
   below is where `uRange` is defined and the only place it can be read. */

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
/* "THIS PASS WILL DRAW THE UNIT, SO THE ENGINE NEED NOT" -- and that is a
   promise to the GAME thread, which acts on it by skipping the engine's own
   rasterise and wiping its composite (tagpu_owndraw.c's `classify`, then
   `tagpu_r3dcache_wipe`). The comment there states the invariant this answer
   has to keep: a stale read may only be stale in the direction of NOT skipping,
   because "skipping when nothing will draw has no write order that produces
   it".

   ON THE VULKAN-ONLY LANE IT DOES. `ready()` arms there without a GL program,
   so `s_state == 1` alone would promise a draw this pass cannot guarantee: the
   twin stands down whenever a mirror is missing, the hand-over is short or the
   lane is not READY, and after the retry budget `render_vk.c` degrades to GDI
   while `tagpu_vk_owns_present()` stays latched -- so the game thread would go
   on skipping and wiping for the life of the process and every covered unit
   would be invisible.

   SO THE ANSWER IS NO, by construction rather than by checking whether the
   twin happened to draw. The engine keeps its own rasterise and its own
   composite; our twin draws the same units into the world target from the
   hand-over, which is independent of this.

   AND THE FALLBACK IT PROMISES IS CURRENTLY BROKEN ON THIS LANE -- not wrong
   as a design, BROKEN AS AN IMPLEMENTATION, which landing 11-5d measured and
   is the reason this comment changed. The 4b-2 text above rests on "the engine
   keeps its own rasterise", read as "so a unit our lane failed to draw is
   still on the screen in 8bpp". On `renderer=vulkan` today it is not.

   MEASURED 2026-09-19, `one-unit` on Two Continents, 1024x768 on a private
   display, one ARMCOM at screen (512,384), on BOTH lanes:

     * the engine rasterises every unit, every frame. `OWND target=all
       skipped=0 passed=55991` and climbing on the Vulkan lane; on gdi the
       detours are armed too (`owndraw: ARMED target="all"`) and this predicate
       is a literal `return 0`, so `classify` cannot skip on either.
     * `tacli shot` -- TA's own surface -- carries that commander in colour,
       with its drop shadow, in every configuration tried on both lanes.
     * `renderer=gdi`: THE PRESENTED FRAME HAS IT. The window capture and the
       engine-surface capture are the same picture. The fallback works.
     * `renderer=vulkan`: THE PRESENTED FRAME DOES NOT. With `native.on` off so
       nothing of ours draws a unit, the commander is absent from the window;
       with `terr.on` off as well, TA's own terrain DOES reach the frame (it
       comes up green) and the commander is still absent.

   SO THE LOSS IS OURS AND IT IS IN THE VULKAN COMPOSITE PATH. The gdi control
   is what settles that, and it was asked for by this landing's review after
   the first write-up concluded -- wrongly -- that the engine's rasterise was
   inherently invisible work. It is not: the same engine output reaches the
   player perfectly well one lane over.

   WHAT THAT MEANS FOR THIS PREDICATE. Returning 0 is not just safe, it is
   LOAD-BEARING, and more so than 4b-2 knew: while the composite drops TA's
   units, a `live()` of 1 would take away the engine's copy as well and a
   stood-down frame would have nothing at all on it. And once the composite
   bug is fixed the 4b-2 comfort comes back intact -- the engine really is the
   fallback, as it already is on gdi.

   THE MECHANISM IS NOT ESTABLISHED AND THIS LANDING DOES NOT GUESS. The lead,
   from reading rather than measuring: `tagpu_surf_take` (tagpu_surf.c:32,
   called from `tagpu_overlay_draw`) copies `g_ddraw.primary->surface` on the
   RENDER thread, while `tacli shot` reads the same object at the entry of the
   engine's flip on the GAME thread, where it is by construction the frame the
   previous flip presented. tagpu_surf.h's argument is explicitly a LIFETIME
   one -- `g_ddraw.cs` keeps the pointer live and `dds_Flip` swaps inside it --
   and says nothing about the buffer holding a FINISHED frame; TA writes those
   bytes without entering that section. If the snapshot lands mid-draw it gets
   what TA has drawn so far, which fits every observation above. Test it by
   dumping the snapshot's own bytes in the frame `tacli shot` fires and
   diffing. If it holds, the fix is an ORDERING -- snapshot where the shot
   does -- never a timing mitigation.

   WHICH IS WHY THIS IS `return 0` AND NOT A LANE TEST. The expression was
   `s_state == 1 && !tagpu_vk_owns_present()`, whose second term is pinned
   false, so the value has always been 0 here; writing it as a lane test made
   it read as a thing that would come back when the lanes did. It will not.
   `s_state` is deliberately not consulted: the pass arming has never been the
   question this answers.
   [FROM THE 4b-2 LANDING REVIEW, 2026-09-18; the redundancy half corrected by
   the vulkan-only plan's 11-5d, 2026-09-19.] */
int tagpu_posedraw_live(void) { return 0; }

/* 1 only once the pass has TRIED and failed — a driver this build cannot run
   on. Distinct from `!live`, which is also true for the frame or two before
   the render thread has built anything, and which is not a problem. */
int tagpu_posedraw_refused(void) { return s_state == 2; }

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
static int          s_abTaking;     /* THIS window opened the capture         */
static int          s_saidFrame;
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
static int          s_ab, s_abDone;

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
/* `s_slantU/s_slantT/s_wireU/s_wireL` counted the slant and wire draws and
   went with them in landing 11-5d: their only increments were inside
   `_slant_redraw` and `_wire_unit`, so keeping them would have left
   `tagpu_posedraw_stats` with two branches that can never be taken. */

/* ---- the shader ---------------------------------------------------------
   NEITHER OF THE TWO BELOW HAS A C REFERENCE LEFT, and neither is dead code.
   They are a BUILD INPUT: `tools/spirv-gen.py` reads them out of the
   PREPROCESSED translation unit under the manifest names tagpu_posedraw::VS
   and tagpu_posedraw::DFS, and generates the SPIR-V the two posed pipelines
   are built from -- `pose_unit` pairs this vertex stage with
   tagpu_native::FS, `pose_depth` with the DFS below (spirv-gen.py:186-187).
   Deleting either fails the build, and editing one edits the units the player
   sees. `tools/spirv-check.sh` re-extracts them through the preprocessor on
   every link and compares the hashes.
   The pragma below is paired and its `pop` is PROVED with a planted probe
   rather than read -- landing 11-4b put one at column 0 inside a comment,
   where it is text and not a directive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
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
#pragma GCC diagnostic pop

int tagpu_posedraw_ready(void)
{
    int lim;
    char b[256];

    if (s_state) {
        /* RE-ASKED, NOT LATCHED, where the bound belongs to a device that can
           change under us: the GPU picker tears the lane down and re-picks, and
           a smaller device may not hold the pose block the larger one did. The
           accessor caches per device, so this is one compare in the steady
           state. [FROM THE 4b-2 LANDING REVIEW.] */
        if (s_state == 1) {
            lim = tagpu_vk_max_uniform_range();
            if (lim > 0 && lim < PD_BLOCK) {
                _snprintf(b, sizeof b,
                          "posedraw: the device changed and its maxUniformBufferRange is "
                          "%d, the pose block needs %d - standing down", lim, PD_BLOCK);
                b[sizeof b - 1] = 0;
                plog(b);
                s_state = 2;
            }
        }
        return s_state == 1;
    }
    /* 3 = BUILDING, not 2 = refused. This blocks re-entry exactly as 2 did,
       but `tagpu_posedraw_refused()` stays false while we load entry points
       and query the block size — a window the GAME thread's owndraw classify
       runs through many times a frame, and in which a 2 here latched its
       one-shot "the posed unit program REFUSED to arm" on a perfectly healthy
       run. Every real refusal below sets 2 before returning. */
    s_state = 3;

    /* THIS PASS ARMS WITHOUT A PROGRAM OF ITS OWN. It never rasterises
       anything: it fills the pose block that `tagpu_vk_pose.c` binds, and the
       SPIR-V for that draw is compiled from `VS`/`DFS` above at build time.
       What the callers need from this function is a yes, because the per-unit
       gather is gated on it (`if (!pdReady) continue;`) and the gather is what
       feeds the hand-over.

       THE ONE REAL QUESTION IS WHETHER THE DEVICE CAN HOLD THE BLOCK. The pose
       block is a compile-time size (`PD_BLOCK`), so the whole of bring-up is
       one `maxUniformBufferRange` compare. A 0 there means no device YET, not a
       device that cannot: `s_state` goes back to 0 so the next frame asks
       again, rather than latching a refusal during the lane's ~200 ms
       bring-up. [The vulkan-only plan, 4b-2 and 11-5d.] */
    lim = tagpu_vk_max_uniform_range();
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
              "posedraw: armed — no rasteriser of its own, pose block "
              "%d bytes (%d pieces), device limit %d", PD_BLOCK, TAGPU_PBMAXPIECE, lim);
    b[sizeof b - 1] = 0;
    plog(b);
    return 1;
}

/* ---- the pose words ------------------------------------------------------
   THE UPLOAD THIS SECTION WAS NAMED FOR WENT WITH LANDING 11-5d; what is left
   is the conversion it shared with the hand-over.

   The two packed per-piece words, one float a piece, zero-filled out to the
   vec4 the block stores them in. It was FACTORED OUT so that the hand-over
   carried exactly the bytes the upload wrote rather than a second conversion
   of the same two arrays, and with the upload gone that is now the only
   description left: the Vulkan pass writes `nf * 16` bytes at PD_FLAGOFF and
   PD_VISOFF, and this is where those bytes are made.

   IT IS PURE, and the landing relied on that to delete the upload: it fills
   two caller-provided arrays and returns a count, holding nothing between
   calls, so the upload's call and `pd_record`'s call were the same answer
   computed twice rather than two halves of one sequence. Returns the piece
   count it wrote, clamped, or 0 for a unit with no pose. */
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

/* ---- the Vulkan lane's record ------------------------------------------- */
/* The two uniform blocks as `_begin` is about to leave them, taken from the
   same sources rather than read back out of the program. Everything the twin
   does NOT set is left at the zero a freshly linked program holds, which is
   what the twin is actually drawing with -- see tagpu_posedraw.h on uLambert,
   which is the one where that matters. */
static void pd_view_publish(const TAGPU_PDVIEW* v)
{
    memset(&s_pub, 0, sizeof s_pub);
    s_pub.frame = s_frame;
    s_pub.gw = v->game[0]; s_pub.gh = v->game[1];
    s_pub.zoom = v->zoom;
    s_pub.zoomCx = v->zoomC[0]; s_pub.zoomCy = v->zoomC[1];
    s_pub.depthScale = v->depthScale;
    s_pub.shd[0] = (float)v->shNeutral; s_pub.shd[1] = (float)v->shDir;

    /* `restored` IS SET BELOW, AFTER THE ARM (11-5e-2c). It used to sit here
       and read `tagpu_r3d_atlas_rgbref()`, a GL texture name whose only writer
       in the tree assigns 0 -- so it published 0 on every frame of every
       process since 11-5e-2, and every consumer of the restored twin stood
       down. It now asks whether the published list is armed, and asking that
       before the arm on the same beat would cost the first frame of a session
       for no reason. */
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

    /* THE CAST-SHADOW READ-BACK BLOCK. It used to mirror `tagpu_shadow_apply`,
       which wrote uShadowOn and RETURNED when no map was live, so the rest of
       the block stayed at the program's zero. That function is gone with the
       rest of the GL lane (landing 11 D2); what is left is the zero. */
    /* 0, AND THAT IS WHAT IT ALREADY WAS. This read `tagpu_shadow_live()` until
       landing 11 D2 deleted the GL lane's `tagpu_shadow.c`. That function
       returned `s_live`, whose only assignment to 1 sat inside
       `tagpu_shadow_begin`, which had no caller -- so this published 0 on every
       frame and the `if` below it never ran. The other shadow fields are
       published as zero and now demonstrably so: this function opens with
       `memset(&s_pub, 0, sizeof s_pub)` and, since this edit, NOTHING in this
       file writes `shadowMat`, `shadowSun`, `shScale`, `penumbra` or `shade` at
       all -- which is a stronger statement than the old code could make, and
       one a grep checks. Writing the constant changes nothing and stops the
       file claiming to ask a question. Reviving cast shadows means writing a
       producer; see `tagpu_vk_shadow.h` on TAGPU_SHADOWHAND. */
    s_pub.shadowOn = 0;

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
    /* AND THE RESTORED TWIN, AS THE REQUEST AND NOTHING ELSE (landing 7e-2,
       and 11-5e-2b). A read-back stood beside this and published the twin's
       TEXELS: `tagpu_r3d_atlas_mirror_rgb_want` / `_step` armed and drove a
       `glReadPixels` off an FBO here, on the render thread with the context
       current, and the accessor handed the bytes over. There is no GL context
       to read back from -- `oglu_load_dll` has no caller, so opengl32.dll is
       never in the process -- and the accessor had returned NULL on every
       published frame of every process for as long as that has been true.
       `atlasRgbAniso` is the one thing that survived it, because it is the
       twin's SAMPLER ratio rather than a fact about the mirror, and the list
       accessor carries it.

       THE ARM IS STILL A CALL ON THIS BEAT. `_want` chose between the list and
       the read-back, and now arms only the list -- so dropping it along with
       the read-back would leave the unit atlas with NO list armed and the other
       lane with nothing to restore from. Silently, because a lane with no list
       stands down rather than complains. */
    tagpu_r3d_atlas_restore_want();
    /* AND NOW THE FLAG, because the arm above is what it asks about. */
    /* `_assets()`, NOT `_on()` (11-5e-2c review). `tagpu_feat.c`, `tagpu_fx.c`
       and `tagpu_terr.c` all ask `tagpu_classicpp_assets()` -- `s_on && s_assets`
       -- and this asked `tagpu_classicpp_on()`, which is `s_on` alone. `assets=`
       is live cfg the render-options menu writes back, so turning assets off
       mid-session reverted terrain, features and effects to the palette path
       while units kept sampling the restored twin: mixed art, silently, until
       restart. Unreachable while this was pinned 0; reachable the moment it
       was not. */
    s_pub.restored = (tagpu_r3d_atlas_restore_armed() && tagpu_classicpp_assets()) ? 1 : 0;
    /* THE ARM IS TAKEN HERE AND THE LIST IS NOT (11-5e-2c). Arming is an ASK
       and belongs on this beat; capturing the list is a READ of a buffer this
       frame is still painting into, and it has moved to
       `tagpu_posedraw_handover`. The reason is `ghost_record`: it runs after
       this function, inside the same `tagpu_native_frame`, and reaches
       `atlas_paint` through `tagpu_r3d_atlas_uv` -> `atlas_get` on a build
       ghost whose texture is not yet atlased. With the feed restored that
       appends to the very list a capture here would have published, so a
       capture here is a snapshot taken before the frame has finished writing
       it. See the handover for the ordering that replaces it. */
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
    /* THIS REPRODUCED WHICH CASTERS WERE IN THE MAP, back when a depth loop
       drew them earlier in the same frame over the same array with the same
       `unit_ok` gate. There is no such loop: `s_depthOn` lost its only writer
       when landing 11-5d deleted `tagpu_posedraw_depth_begin`, so `casts` is
       0 for every record and `s_ncast` never leaves 0. The consumer's chain
       and what it costs are in tagpu_posedraw.h's tombstone. */
    r->ghost = u->ghost ? 1 : 0;
    r->casts = (s_depthOn && !u->castSkip) ? 1 : 0;
    if (r->casts) s_ncast++;
}

/* ---- bodies ------------------------------------------------------------- */
static void pd_begin(const TAGPU_PDVIEW* v, int ghostWindow);

void tagpu_posedraw_begin(const TAGPU_PDVIEW* v)       { pd_begin(v, 0); }
void tagpu_posedraw_begin_ghost(const TAGPU_PDVIEW* v) { pd_begin(v, 1); }

static void pd_begin(const TAGPU_PDVIEW* v, int ghostWindow)
{
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
        }
        /* THE ARMING, and since landing 4d-2 that is the whole of it -- there
           is no GL half to black the frame for. */
        if (!ghostWindow && s_recording && s_ab && !s_abDone)
            s_abTaking = 1;
    }
    /* THE PROGRAM AND ITS UNIFORMS STOOD HERE, deleted by landing 11-3.
       Everything above -- the recording window (`s_recording = s_mirrorWant`)
       and the A/B's arming -- is the pass, and is what makes a hand-over
       happen at all. */
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
    /* `m->vao` IS A GL NAME, NOT A VALIDITY TEST, and asking it here rejected
       every unit on the vulkan-only lane -- `mat_bake` creates no vertex array
       there, so the hand-over came out `nunit=0` and the twin stood down with
       nothing to say. The integrity the line is for is the other two terms:
       the material entry names THIS geometry and agrees with it about the
       vertex count. Where GL draws, a missing array is still a refusal,
       because the draw below binds it.

       TENTH INSTANCE IN THIS LANDING of a pass keyed on a GL handle rather
       than on what the handle stands for -- after the GAF atlas's `tex`, the
       marker layer's `s_tex[i]`, the readout's `textTex`, the terrain atlas's
       bound and the rest. It is the shape this landing is made of.
       [The vulkan-only plan, landing 4b-2.] */
    if (m->geom != g || m->nvert != g->nvert) return NULL;
    /* AND THE `!m->vao` HALF IS GONE TOO [landing 11-5d]. 4b-2 left it as
       `!tagpu_vk_owns_present() && !m->vao` so that a GL draw still refused a
       unit whose vertex array was missing; there is no GL draw and the first
       term is pinned false, so the line could never reject and was itself the
       shape this landing removes -- a lane test in a file whose own rule says
       gate on the thing, not on the backend that built it. Value-identical to
       delete: the condition was unreachable. */
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
    /* THE PER-UNIT UNIFORMS AND THE DRAW STOOD HERE, deleted by landing 11-3;
       the uniform-block upload that fed them went with landing 11-5d. What is
       left is the record, and the Vulkan twin draws from it. The upload was
       inert to remove because it shared no state with the record: it called
       `pose_words` for itself, and `pd_record` calls it again for the arenas.
       `pose_words` is pure -- it fills two caller-provided arrays and returns a
       clamped count -- so the second call is the same answer, not a
       continuation of the first. */
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

/* ---- closing the window --------------------------------------------------
   THE BANNER HERE READ "the Classic silhouette shadow" until landing 11-5d
   deleted that section out from under it. */
/* CLOSES THE RECORDING WINDOW AND PUBLISHES IT. The comment that stood here
   described `_redraw`, the silhouette's second half, which went with landing
   11-5d -- it had drifted one function away from what it documented. */
void tagpu_posedraw_end(void)
{
    if (s_state != 1) return;
    /* the one GL call this function made -- glBindVertexArray(0) -- went with
       the draw halves [landing 11-3]. The publish below IS the pass. */
    if (!s_recording) return;
    s_recording = 0;

    /* ONLY THE WINDOW THAT OPENED THE BRACKET CLOSES IT. `s_abDone` alone was
       the test until landing 6's review, and it stopped being enough the moment
       a frame could have more than one window. */
    if (s_abTaking) {
        s_abTaking = 0;
        s_abDone = 1;
        /* THE A/B CLAIM, which is all that is left of it. Until landing 4d-2
           this pass also captured a GL half (`tagpu_abshot.c`) and, where the GL
           lane drew, claimed the Vulkan one only if that half had reached the
           disk -- route D gave the two lanes a window each. Route D went in
           4d-1 and the GL half had nothing to pair with. `tagpu_vk_ab_arm`
           unlinks the target `_vk.ppm` at the instant the claim latches, which
           is what makes the file on the disk this arming's rather than an
           earlier run's; diff it against a capture from another BUILD. */
        s_abClaim = tagpu_vk_ab_arm("posedraw");
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
    /* THE GHOST SUPPRESSION IS GONE, BECAUSE THE PAIR IT PROTECTED IS [landing
       11-3]. It read `s_nghost ? 0 : ...` because the A/B was a SAME-RUN pair:
       the GL half was blacked and read back around the FIRST window while the
       ghost drew in a SECOND, so the GL capture could not contain a ghost that
       the Vulkan frame did, and a frame with one was not a valid oracle. The GL
       capture half went in landing 4d-2; what the lever claims now is the
       VULKAN capture alone, diffed file-to-file against another BUILD, and both
       sides of that comparison carry whatever the frame had. A ghost frame is
       therefore an ordinary frame for this instrument.

       KEEPING THE TERM WOULD HAVE COST A CAPTURE PER SESSION once landing 11-3
       made the ghost record on this lane: the unit window latches `s_abDone` and
       `tagpu_vk_ab_arm` unlinks the target, then the ghost window republishes
       `ab = 0`, and `s_abDone` only clears when the lever file is deleted -- so
       the one-shot is spent and nothing is written. Found by this landing's
       review. */
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
    s_pub.ab = s_abClaim;
    s_pubHave = 1;
}

/* ---- the structure-shadow slant (G16 step 6) ---------------------------- */
/* `emit_slant`'s range. The uniforms it does NOT set are as deliberate as the
   ones it does: uAlpha and uWaterMode are never read on this path, because the
   fragment shader's `uShadow == 1` branch returns before either. */
/* ---- the nanoframe wireframe (G16 step 6) ------------------------------- */
/* ---- the shadow-depth twin ---------------------------------------------- */
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
        /* PERIODIC, AND IT REPORTS THE STATE RATHER THAN AN OPINION ABOUT IT.
           The latched version of this line was spent on the shell's ordinary
           "no posed units" case and then said nothing for the rest of the run,
           which is the third time in this landing a one-shot latch has hidden
           the thing it was added to show. These four values are the whole
           decision: `mirrorWant` is whether a second backend asked for the
           record at all, `win` whether a window opened this frame,
           `recording` whether that window took it, and `pubHave` whether the
           frame ended with something to give. */
        if ((now % 300u) == 0u) {
            char b[192];
            _snprintf(b, sizeof b, "posedraw: nothing to hand over for frame %u - "
                      "mirrorWant=%d win=%d recording=%d pubHave=%d other=%d",
                      now, s_mirrorWant, s_win, s_recording, s_pubHave, s_other);
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
    if (s_pubHave && s_pub.frame == now) s_saidFrame = 0;
    /* NOT THIS FRAME'S, SO NOT ALIVE. `units`, `rows`, `flags` and `vis` are
       arrays this file REALLOCATES the moment a frame needs more room than the
       last did, and the records name bake entries tagpu_posebake.c evicts. The
       stale hand-over is cleared as well, so the next frame starts honest. */
    if (s_pub.frame != now) { s_pubHave = 0; return 0; }
    /* THE COUNT IS TAKEN NOW, not when the window closed -- `_end` says why.
       Every draw of this frame is behind us at this point, because the whole
       native pass runs earlier in this iteration of the render loop than the
       `tagpu_vk_frame` that calls this. On the Vulkan lane that is
       `render_vk.c`: `tagpu_overlay_draw` at `:232`, then `tagpu_vk_frame` at
       `:268`, in one iteration on one thread. (This said `render_ogl.c` until
       11-5e-2c; the property is the loop's shape and both loops have it, but
       the file named was the one that no longer runs.) */
    s_pub.otherDraws = s_other;
    /* AND SO IS THE RESTORE LIST, FOR THE SAME REASON AND A SHARPER ONE
       (11-5e-2c). It was taken in `pd_view_publish`, on the FIRST posedraw
       window of the frame -- and `tagpu_native.c`'s `ghost_record` runs after
       that, in the same `tagpu_native_frame`, and can paint the unit atlas
       through `tagpu_r3d_atlas_uv` -> `atlas_get`. Now that `atlas_paint`
       feeds the published list again, a capture at `pd_begin` would hand the
       consumer a count taken before the frame finished appending to it.

       WHAT MAKES HERE CORRECT IS THE LOOP, NOT A LOCK. `tagpu_overlay_draw`
       has returned by the time `tagpu_vk_frame` calls us, so every paint of
       this frame -- the native pass's and the bake's alike -- is behind this
       line. `rlist_restart`, the only thing that rewrites list entries in
       place rather than appending past them, therefore cannot run between this
       capture and the consumer's read of it.

       The pointer itself is safe by the BOUND rather than by this ordering:
       the arm allocates the whole `rlist_cap(a)` in one go and nothing
       reallocs or frees it, so the address is fixed for the atlas's life. Two
       different guarantees for two different hazards, and neither is a
       timing argument. [The vulkan-only plan, 11-5e-2c.] */
    {
        float aniso = 0.0f;
        const TAGPU_RGLSL_FRAME* fr =
            tagpu_r3d_atlas_restore_list(&s_pub.restoreDim, &s_pub.restoreN,
                                         &s_pub.restoreGen, &s_pub.restoreRepaint,
                                         &s_pub.restoreBlanks, &s_pub.restoreMips,
                                         &aniso);
        if (fr) {
            s_pub.atlasRgbAniso  = aniso;
            s_pub.restoreFrames  = fr;
        } else {
            s_pub.restoreFrames  = NULL;
            s_pub.restoreN = 0; s_pub.restoreGen = 0;
            s_pub.restoreRepaint = 0; s_pub.restoreBlanks = 0;
            s_pub.restoreDim = 0; s_pub.restoreMips = 0;
            /* WRITTEN ON BOTH PATHS, not left to the memset in the publisher.
               It is the only field of this block that would otherwise be set on
               one path only, and a reader here cannot see what zeroed it.
               [The 11-5e-2b cross-thread review.] */
            s_pub.atlasRgbAniso  = 0.0f;
            /* AND THE FLAG CANNOT OUTLIVE THE LIST IT PROMISES (11-5e-2c
               review, L1). `restored` is published ~440 lines above from
               `tagpu_r3d_atlas_restore_armed()`, one term; `restoreFrames`
               comes from `tagpu_r3d_atlas_restore_list()`, three. They agree
               today only because `rlistWant` and `rlist` are set and cleared
               together and `dim` is never zeroed after `r3d_init` -- which is
               an argument about three other places. Clearing it here makes
               "flag set, list absent" unrepresentable in the hand-over instead
               of merely unreached. */
            s_pub.restored = 0;
        }
    }
    *out = s_pub;
    s_pubHave = 0;
    return 1;
}

/* ---- frame, reset, stats ------------------------------------------------ */
void tagpu_posedraw_frame(unsigned frame_counter)
{
    s_units = s_tris = 0;
    /* THE HAND-OVER'S FRAME, and everything that is per-frame about it. The
       previous frame's publish is dropped here rather than left standing: the
       stamp would refuse it anyway, and clearing it is what makes that a
       belt-and-braces check instead of the only one. */
    s_frame = frame_counter;
    s_pubHave = 0;
    s_win = 0; s_recording = 0; s_other = 0;
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
        /* ASKED OF THE CONSUMER, NOT OF THE LEVER. [FROM THE 4d-1 LANDING REVIEW.]
        This used to test `tagpu_vk_armed()`, which is true whenever `tagpu_vk.on`
        exists -- and these latches are one-way, so once asked the memory is held
        for the process's life. Until 4d-1 that was right: `tagpu_vk.on` under
        `renderer=openglcore` brought up route D, which consumed the mirror. Route
        D is gone, so on that path the lever now arms nothing and the mirror would
        be paid for with no consumer at all. `tagpu_vk_owns_present()` is exactly
        "a Vulkan pass will run in this process", which is the question. */
        if (!s_mirrorWant && tagpu_vk_owns_present()) s_mirrorWant = 1;
        s_ab = GetFileAttributesA(PD_ABFILE) != INVALID_FILE_ATTRIBUTES;
        if (!s_ab) s_abDone = 0;
    }
}

void tagpu_posedraw_glreset(void)
{
    /* THREE GL OBJECT NAMES WERE FORGOTTEN HERE, and they went with landing
       11-5d along with the program they named. What is left is the arm state,
       and it still belongs here: the fork restarts its render thread on every
       display-mode change, and the device the next `ready()` asks about its
       `maxUniformBufferRange` may not be the device this one answered for. */
    s_state = 0;
}

int tagpu_posedraw_stats(char* out, int n)
{
    int k;
    if (s_state != 1 || n <= 0) { if (out && n > 0) out[0] = 0; return 0; }
    k = _snprintf(out, n, " posed=%u/%utri", s_units, s_tris);
    if (k < 0 || k >= n) return k;
    /* ` slant=` AND ` wire=` STOOD HERE and went with their counters in landing
       11-5d. NOTE THAT NOTHING CALLS THIS FUNCTION EITHER, and has not since
       landing 11-3 took `tagpu_native.c`'s GL composite: the only tree-wide
       references are its declaration and this definition, so ` posed=` has not
       been printed for several landings. `ta-drive` was still telling sessions
       to read all three out of the log; corrected 2026-09-19. */
    if (s_overPiece)
        k += _snprintf(out + k, n - k, " OVER-PIECE");
    return k;
}
