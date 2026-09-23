/* tagpu_posedraw.c — the posed program.

   The pass that draws from tagpu_posebake.c's bake. See tagpu_posedraw.h for the
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

   THREE RANGES, ONE PROGRAM, `uRange`. The bake lays the body, the slant and
   the wire down in one buffer, so a range is a
   different `first`/`count` on the same bind; what differs in the shader is
   small and explicit:

     BODY   (0)  the projection above, the depth key, the shade.
     SLANT  (1)  0x45A610's projection `(x + y/4, -z - y/4)` off the posed
                 vertex SNAPPED to whole units, the neutral SHD row, and its
                 own per-piece rule — `(P_FLAGS & 3) == 3`, visible AND
                 `cached` — which the body's all-zero matrix cannot express
                 because such a piece still draws in the body range.
     WIRE   (2)  the body projection, a LINE_LIST (tagpu_vk_unit.c's wire
                 pipeline), one notch nearer (+0.15), and
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

   THE POSE LIVES IN ONE STORAGE BUFFER PER FRAME, which is what takes the
   piece cap out of the design (gpu-posing.md decision 7). Every posed unit's
   rows are packed back to back, then every unit's `shaded` words, then every
   unit's visibility words (tagpu_posebake.h has the layout), and a unit reaches
   its own slice through three base indices in its uniform block. A unit
   therefore costs what its model has -- 2 016 bytes for stock's worst, 36
   pieces -- and the model ceiling, TAGPU_PBMAXPIECE (256), is a bound on one
   unit rather than a size every unit pays. The pass arms only on a device
   that can hold one unit at that ceiling (`tagpu_posedraw_ready`); the
   consumer checks each frame's packed size against the device's own limit.
   When the pass refuses to arm it says so (`tagpu_posedraw_refused`), and
   `owndraw` repeats the refusal once in its own words.

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
#include "tagpu_log.h"
#include "tagpu_packet.h"     /* the record count TAGPU_PD_MAXHAND must cover,
                                 and tagpu_grow_stress */
#include "tagpu_render3do.h"
#include "tagpu_classicpp.h"
#include "tagpu_vk.h"      /* tagpu_vk_armed(): whether to publish at all */
#include "tagpu_pal.h"
#include "tagpu_posebake.h"

#define STR2(x) #x
#define STR(x)  STR2(x)

/* per piece: 3 rows, then two packed floats -- `shaded` for the body's SHD
   row and a visibility WORD (0 / 1 / 3) the slant and the wire read. Two
   arrays rather than bits of one float because a packed pair costs every
   reader of it a decode; the second is a word rather than two more arrays
   because its two values nest -- a slant caster is always visible. The
   numbers are tagpu_posebake.h's, shared with the Vulkan pass that fills the
   buffer, so the two cannot describe it differently. */
#define PD_FLAGV   TAGPU_PD_FLAGV
#define PD_UNITMAX TAGPU_PD_UNITMAX

/* `uRange`'s three values (BODY 0, SLANT 1, WIRE 2) have no macro: the Vulkan
   consumer writes the number itself (tagpu_vk_unit.c's `b.i[40]`), and the
   shader below is where `uRange` is defined and the only place it can be read. */

static void plog(const char* s)
{
    tagpu_log(s);
}

/* ---- readiness ----------------------------------------------------------
   This pass is THE unit renderer and there is no lever: the one question is
   whether it can run at all, which `tagpu_posedraw_ready()` answers, and
   re-asks when the device changes.

   ONE HALF OF THAT ANSWER IS PUBLISHED TO THE GAME THREAD: whether the pass
   has tried and refused, which `tagpu_owndraw_classify` reads to log the
   refusal once. `s_state` is one aligned int that only the render thread
   writes, and the game thread's read decides nothing but whether a log line
   is written, so a stale value moves that line by a frame and cannot change
   a draw. */
static int    s_state;          /* 0 untried, 1 ready, 2 refused, 3 building */

/* 1 only once the pass has TRIED and failed — a driver this build cannot run
   on. `s_state` is also 0 for the frame or two before the render thread has
   built anything, and that is not a refusal. */
int tagpu_posedraw_refused(void) { return s_state == 2; }

/* ---- THE VULKAN LANE'S HAND-OVER -----------------------------------------
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

   EVERY WINDOW RECORDS. What is first-window about a frame is two separate
   things: the first window PUBLISHES THE VIEW (one publication a frame is
   right), and the A/B's capture is bracketed around a NON-GHOST window -- not
   around "the first", because with no posed unit on screen the ghost's window
   IS the first. */
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

/* the cast-shadow depth record. `s_depthOn` has no writer but the frame
   reset -- `pd_record` says what that leaves */
static int          s_depthOn, s_ncast;
static float        s_depthMat[16];

/* THE NANOFRAME PAIR IS STICKY: `pd_record` takes uNanoT and uNanoC only
   from a unit with `nanoOn`, so a unit without one carries whatever the last
   one that had it left here. Reset with the frame, so the first unit of a
   frame has zero behind it. */
static float        s_lastNanoT, s_lastNanoC[3];

/* THE FOG GRID'S COPY. `cells` is what the packet allocated for this grid,
   so the read is bounded by the allocation it reads from; the cap bounds OUR
   allocation, and the Vulkan pass re-checks it because a bound in one file is
   a bound only while both are read together. */
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
    /* the lever: grow to the exact size, so nearly every append moves the
       arena -- which the records survive because they hold offsets */
    if (tagpu_grow_stress()) want = need;
    q = realloc(*p, (size_t)want * elem);
    if (!q) {
        if (!s_saidRoom) {
            s_saidRoom = 1;
            plog("posedraw: the Vulkan hand-over's arena would not grow - nothing "
                 "is handed over while that is true");
        }
        return 0;
    }
    *p = q; *cap = want;
    return 1;
}

/* the consumer's report that it painted at least one structure slant this
   frame, and the producer's read-and-clear of it -- tagpu_posedraw.h states
   the ordering that makes the one-frame lag a property of the loop rather than
   of who happened to run */
static int s_slantDrew;
void tagpu_posedraw_slant_drew(void) { s_slantDrew = 1; }

/* NOT GATED ON THE ARM STATE: `tagpu_posedraw_ready` first arms the pass later
   in the same frame than the packet is checked, so a gate here would let the
   arming frame -- the one most likely to be truncated, at load -- through.
   The count is reset by `tagpu_posedraw_frame` and read only by this frame's
   publish. */
void tagpu_posedraw_uncarried(void) { s_other++; }

/* TAGPU_PD_MAXHAND is every record the producer can make (tagpu_posedraw.h),
   so a frame is never refused for its count while the packet's tables are the
   size they are -- and the tables cover the design point. */
typedef char pd_maxhand_covers[(TAGPU_PD_MAXHAND >= (int)(TAGPU_PK_MAX_UNITS +
    TAGPU_PK_MAX_WRECKS + 1u + TAGPU_PK_MAX_BUILDS)) ? 1 : -1];
typedef char pd_units_design[(TAGPU_PK_MAX_UNITS >= TAGPU_PK_DESIGN_SLOTS) ? 1 : -1];
int  tagpu_posedraw_slant_take(void) { int v = s_slantDrew; s_slantDrew = 0; return v; }

/* ---- the shader ---------------------------------------------------------
   NEITHER OF THE TWO BELOW HAS A C REFERENCE, and neither is dead code.
   They are a BUILD INPUT: `tools/spirv-gen.py` reads them out of the
   PREPROCESSED translation unit under the manifest names tagpu_posedraw::VS
   and tagpu_posedraw::DFS, and generates the SPIR-V the two posed pipelines
   are built from -- `pose_unit` pairs this vertex stage with
   tagpu_native::FS, `pose_depth` with the DFS below (spirv-gen.py's manifest).
   Deleting either fails the build, and editing one edits the units the player
   sees. `tools/spirv-check.sh` re-extracts them through the preprocessor on
   every link and compares the hashes.
   The pragma below is paired and its `pop` is PROVED with a planted probe
   rather than read: a `pop` at column 0 inside a comment is text, not a
   directive. */
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
    /* The frame's poses, packed (tagpu_posebake.h). This unit's slice: 3 rows
       of a 4x3 per piece from uRowBase, and two per-piece words packed 4 to a
       vec4 from uFlagBase and uVisBase --
         flag 1.0 = the piece is shaded (emit_geom_at's `pieceShaded`);
         vis  0 = not drawn, 1 = drawn, 3 = drawn AND the slant casts from it
              (`P_FLAGS` bit 0, and bit 1 as well).
       readonly: a vertex stage that wrote it would need a device feature no
       pass enables. */
    "layout(std430) readonly buffer Pose {\n"
    "  vec4 uPose[];\n"
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
    /* this unit's three slices of `uPose`, in vec4: its first row, its first
       shaded word, its first visibility word */
    "uniform int uRowBase;\n"
    "uniform int uFlagBase;\n"
    "uniform int uVisBase;\n"
    "out vec2 vUV; flat out vec2 vFC; flat out float vShade; out vec2 vWorld;\n"
    "out float vEnc; out float vVY; flat out vec3 vNrm; out vec3 vShW;\n"
    /* tagpu_native.c's SH_V and SH_L, the engine's shading basis */
    "const vec3 SH_V = vec3(0.0, 0.8944, -0.4472);\n"
    "const vec3 SH_L = vec3(-0.35, 0.80, -0.49);\n"
    "void main(){\n"
    "  int pi = int(aPiece + 0.5);\n"
    /* EVERY INDEX INTO `uPose` IS CLAMPED TO THE BOUND RANGE, so no vertex can
       read past it whatever it is handed. That is the memory bound, not the
       correctness one: the consumer refuses a frame in which any unit's slice
       does not fit (tagpu_vk_unit.c), and the bake's piece index stays below
       the unit's pose count (`pb_walk` emits p < nparts, `unit_ok` requires
       npose >= nparts). On data that passes both, every clamp is a no-op. */
    "  int pl = uPose.length() - 1;\n"
    "  int pb = clamp(uRowBase + pi * 3, 0, pl - 2);\n"
    /* Three ways a vertex is not drawn, and all of them collapse the same way
       — every vertex of the primitive carries the same answer, so the whole
       primitive lands outside the same clip plane and nothing survives.

       aSkip: a face the engine's rasteriser paints nothing for. It is baked so
       that the geometry and the material buffers hold the same number of
       vertices (gpu-posing.md §4). Always 0 in the slant range, which flat
       fills every face it is given.

       vis >= 3, the SLANT's per-piece rule: `(P_FLAGS & 3) == 3`,
       visible AND `cached`, which a COB's dont-cache clears (a wind
       generator's mast). It cannot ride the all-zero matrix the way body
       visibility does, because such a piece still draws in the BODY range and
       needs its matrix there.

       vis >= 1, the WIRE's: `P_FLAGS & 1`, the same rule the body has —
       but the body expresses it as an all-zero matrix, which collapses a
       triangle to zero AREA, and a triangle of zero area is guaranteed to
       produce no fragments. A LINE of zero length is not: the rasterisation
       rules do not promise it away, and one bright pixel per hidden edge would
       land exactly on the unit's origin. So the wire is refused here instead
       of relying on that. */
    "  float pvis = uPose[clamp(uVisBase + (pi >> 2), 0, pl)][pi & 3];\n"
    "  if (aSkip > 0.5 ||\n"
    "      (uRange == 1 && pvis < 2.5) ||\n"
    "      (uRange == 2 && pvis < 0.5)) {\n"
    "    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "    vUV = vec2(0.0); vFC = vec2(0.0); vShade = 0.0; vWorld = vec2(0.0);\n"
    "    vEnc = 0.0; vVY = 0.0; vNrm = vec3(0.0, 1.0, 0.0); vShW = vec3(0.0);\n"
    "    return;\n"
    "  }\n"
    "  vec4 rp = vec4(aPos, 1.0);\n"
    "  vec3 m = vec3(dot(uPose[pb], rp), dot(uPose[pb+1], rp), dot(uPose[pb+2], rp));\n"
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
    "  bool pShaded = uPose[clamp(uFlagBase + (pi >> 2), 0, pl)][pi & 3] > 0.5;\n"
    "  if (aGF >= 0.5 && pShaded) {\n"
    "    vec3 n = vec3(dot(uPose[pb].xyz, aNrm), dot(uPose[pb+1].xyz, aNrm),\n"
    "                  dot(uPose[pb+2].xyz, aNrm));\n"
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
    /* the vertex's SHADOW-SPACE point, the expression tagpu_native.c's
       vertex stage writes to its own `vShW`, so a unit's fragments look their
       shadow up on their own caster */
    "  vShW = vec3(vWorld.x, uCast.y + uCast.z * m.y,\n"
    "              vWorld.y + (uCast.x + m.y) * 0.5);\n"
    "  if (uDepthPass == 1) gl_Position = uShadowMat * vec4(vShW, 1.0);\n"
    "}\n";

/* the depth stage writes no fragment at all and discards nothing, so a
   colour-keyed texel casts a shadow */
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
           a smaller device may not hold one unit's pose the larger one did. The
           accessor caches per device, so this is one compare in the steady
           state. */
        if (s_state == 1) {
            lim = tagpu_vk_max_storage_range();
            if (lim > 0 && lim < PD_UNITMAX) {
                _snprintf(b, sizeof b,
                          "posedraw: the device changed and its maxStorageBufferRange is "
                          "%d, one unit's pose needs %d - standing down", lim, PD_UNITMAX);
                b[sizeof b - 1] = 0;
                plog(b);
                s_state = 2;
            }
        }
        return s_state == 1;
    }
    /* 3 = BUILDING, not 2 = refused. This blocks re-entry exactly as 2
       would, but `tagpu_posedraw_refused()` stays false while we load entry
       points and query the block size — a window the GAME thread's owndraw
       classify runs through many times a frame, in which a 2 here would latch
       its one-shot "the posed unit program REFUSED to arm" on a perfectly
       healthy run. Every real refusal below sets 2 before returning. */
    s_state = 3;

    /* THIS PASS ARMS WITHOUT A PROGRAM OF ITS OWN. It never rasterises
       anything: it fills the hand-over whose poses tagpu_vk_unit.c binds, and
       the SPIR-V for that draw is compiled from `VS`/`DFS` above by
       tools/spirv-gen.py.
       What the callers need from this function is a yes, because the per-unit
       gather is gated on it (`if (!pdReady) continue;`) and the gather is what
       feeds the hand-over.

       THE ONE DEVICE QUESTION IS WHETHER IT CAN BIND ONE UNIT AT THE PIECE
       CEILING (`PD_UNITMAX`, 14 336 bytes). The spec guarantees every device
       at least 128 MB of `maxStorageBufferRange`, so this cannot refuse a
       conformant device; it is asked because the number is the device's, not
       ours. The FRAME's size is the consumer's check, against the same limit,
       on every frame. A 0 here means no device YET, not a device that cannot:
       `s_state` goes back to 0 so the next frame asks again, rather than
       latching a refusal during the lane's ~200 ms bring-up. */
    lim = tagpu_vk_max_storage_range();
    if (lim <= 0) { s_state = 0; return 0; }
    if (lim < PD_UNITMAX) {
        _snprintf(b, sizeof b,
                  "posedraw: refused — the device's maxStorageBufferRange is %d, "
                  "one unit's pose needs %d", lim, PD_UNITMAX);
        b[sizeof b - 1] = 0;
        plog(b);
        s_state = 2;
        return 0;
    }
    s_state = 1;
    _snprintf(b, sizeof b,
              "posedraw: armed — no rasteriser of its own, one unit's pose "
              "at most %d bytes (%d pieces), device storage limit %d",
              PD_UNITMAX, TAGPU_PBMAXPIECE, lim);
    b[sizeof b - 1] = 0;
    plog(b);
    return 1;
}

/* ---- the pose words ------------------------------------------------------
   The two packed per-piece words, one float a piece, zero-filled out to the
   vec4 the storage buffer holds them in. This is the one place those bytes
   are made: the hand-over appends them to its `flags` and `vis` arrays and the
   Vulkan pass copies those arrays whole into the buffer's two word sections.

   IT IS PURE: it fills two caller-provided arrays and returns a count,
   holding nothing between calls. Returns the piece count it wrote, clamped,
   or 0 for a unit with no pose. */
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
   same sources the view carries. Everything not set below is left at the
   memset's zero -- see tagpu_posedraw.h on uLambert, which is the one where
   that matters. */
static void pd_view_publish(const TAGPU_PDVIEW* v)
{
    memset(&s_pub, 0, sizeof s_pub);
    s_pub.frame = s_frame;
    s_pub.gw = v->game[0]; s_pub.gh = v->game[1];
    s_pub.zoom = v->zoom;
    s_pub.zoomCx = v->zoomC[0]; s_pub.zoomCy = v->zoomC[1];
    s_pub.depthScale = v->depthScale;
    s_pub.shd[0] = (float)v->shNeutral; s_pub.shd[1] = (float)v->shDir;

    /* `restored` IS SET BELOW, AFTER THE ARM: it asks whether the published
       list is armed, and asking that before the arm on the same beat would
       cost the first frame of a session for no reason. */
    s_pub.scafOn = v->scafOn ? 1 : 0;
    s_pub.scafP[0] = v->scafP[0]; s_pub.scafP[1] = v->scafP[1];
    s_pub.scafP[2] = v->scafP[2]; s_pub.scafP[3] = v->scafP[3];
    s_pub.ss = v->ss;
    s_pub.fogOrgX = v->fogOrg[0]; s_pub.fogOrgY = v->fogOrg[1];
    s_pub.fogCols = v->fogDim[0]; s_pub.fogRows = v->fogDim[1];
    s_pub.lit = v->lit ? 1 : 0;
    s_pub.lambert = 0;                 /* never set -- see the header */
    s_pub.sun[0] = v->sun[0]; s_pub.sun[1] = v->sun[1]; s_pub.sun[2] = v->sun[2];
    s_pub.amb = v->amb; s_pub.norm = v->norm;

    /* NO CAST SHADOWS. This function opens with `memset(&s_pub, 0, sizeof
       s_pub)` and NOTHING in this file writes `shadowMat`, `shadowSun`,
       `shScale`, `penumbra` or `shade`, so the whole cast-shadow block is
       zero, and `shadowOn` is the constant 0. Reviving cast shadows means
       writing a producer; see `tagpu_vk_shadow.h` on TAGPU_SHADOWHAND. */
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
       idempotent, so asking every publish costs one branch once it is there.
       The mirror is ASKED for here and READ in `tagpu_posedraw_handover`,
       beside the restore list -- see there for why the two reads are one. */
    tagpu_r3d_atlas_mirror_want();
    /* AND THE RESTORED ATLAS, AS THE REQUEST AND NOTHING ELSE. No texels are
       read back here: the Vulkan pass restores from the list itself.
       `atlasRgbAniso` is the restored atlas's SAMPLER ratio rather than a fact
       about the mirror, and the list accessor carries it.

       THE ARM IS A CALL ON THIS BEAT, and must stay one. `_want` arms the
       list; without it the unit atlas has NO list armed and the Vulkan
       restorer nothing to restore from. Silently, because a lane with no list stands
       down rather than complains. */
    tagpu_r3d_atlas_restore_want();
    /* AND NOW THE FLAG, because the arm above is what it asks about. */
    /* `_assets()`, NOT `_on()`. `tagpu_feat.c`, `tagpu_fx.c` and `tagpu_terr.c`
       all ask `tagpu_classicpp_assets()` -- `s_on && s_assets` -- and
       `tagpu_classicpp_on()` is `s_on` alone. `assets=` is live cfg the
       render-options menu writes back, so asking `_on()` here would, when
       assets are turned off mid-session, revert terrain, features and effects
       to the palette path while units kept sampling the restored atlas: mixed
       art, silently, until restart. */
    s_pub.restored = (tagpu_r3d_atlas_restore_armed() && tagpu_classicpp_assets()) ? 1 : 0;
    /* THE ARM IS TAKEN HERE AND THE LIST IS NOT. Arming is an ASK and belongs
       on this beat; capturing the list is a READ of a buffer this frame is
       still painting into, and it is in `tagpu_posedraw_handover`. The reason
       is `ghost_record`: it runs after this function, inside the same
       `tagpu_native_frame`, and reaches `atlas_paint` through
       `tagpu_r3d_atlas_uv` -> `atlas_get` on a build ghost whose texture is
       not yet atlased. That appends to the very list a capture here would
       publish, so a capture here would be a snapshot taken before the frame
       has finished writing it. See the handover for the ordering. */
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

       THE BUILD GHOST IS RECORDED, NOT COUNTED. `ghost.on` is a play default,
       so counting it would leave `otherDraws` non-zero for as long as a
       building placement is open, and the Vulkan unit pass would draw NOTHING
       -- not the ghost, not the units. A ghost rides this same entry point
       with the same uniforms and differs in exactly two things: `alpha` (0.40,
       which `r->alpha` carries) and depth writes, which the consumer takes
       with a second pipeline. The other two counted cases -- an arena that
       would not grow, a unit with no pieces -- are refusals. */
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
    /* THE HARD SHADOW'S RANGE, RESOLVED HERE AND NOWHERE ELSE. The silhouette
       is the BODY range shifted (the engine blackens the composite it has just
       built and blits it again, 0x45A470 then 0x459200); the structure slant is
       the bake's own SLANT range, whose per-piece `cached` rule the vertex
       shader applies off the visibility word. A ghost casts nothing -- it is a
       preview of a building that is not there, and the engine draws no shadow
       for a placement cursor. */
    r->shKind = u->ghost ? TAGPU_PDSH_NONE : u->shKind;
    r->shOffY = u->shOffY;
    if (r->shKind == TAGPU_PDSH_SLANT) {
        r->shFirst = g->first[TAGPU_PB_SLANT];
        r->shCount = g->count[TAGPU_PB_SLANT];
    } else if (r->shKind == TAGPU_PDSH_SIL) {
        r->shFirst = r->first;
        r->shCount = r->count;
    }
    if (r->shCount <= 0) r->shKind = TAGPU_PDSH_NONE;
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
    /* STICKY -- see `s_lastNanoT` above. The two are only written on a unit
       that has `nanoOn`, so a unit without one carries the last such unit's
       values. */
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
    /* THE NANOFRAME WIRE: every face of every visible piece as a closed
       outline, in the second oscillator's colour (build-state.md 3, the
       engine's 0x458FA0). The bake's WIRE range, drawn by the consumer after
       the bodies. A ghost is never a
       nanoframe and never carries one. */
    if (u->nanoOn && !u->ghost && g->count[TAGPU_PB_WIRE] > 0) {
        r->wireFirst = g->first[TAGPU_PB_WIRE];
        r->wireCount = g->count[TAGPU_PB_WIRE];
        r->wire = u->nanoWire;
    }
    /* `s_depthOn` HAS NO WRITER but the frame reset, so `casts` is 0 for every
       record and `s_ncast` never leaves 0. The consumer's chain and what it
       costs are in tagpu_posedraw.h. */
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
    /* THE PUBLISH WINDOW. EVERY window of a frame records -- the build ghost
       draws in a SECOND one (ghost_pass opens its own, after the wire), and counting that window against the hand-over
       instead would stand the Vulkan unit pass down for the whole of any
       building placement.

       THE FIRST WINDOW OF A FRAME PUBLISHES THE VIEW, whichever window that is.
       THE A/B IS BRACKETED AROUND A NON-GHOST WINDOW AND NOTHING ELSE, and that
       is NOT the same test: with no posed unit on screen `tagpu_native.c` skips
       the unit window and the ghost's is the FIRST, so bracketing "the first"
       would black the frame around a pass whose draws the Vulkan lane makes in
       a different stage entirely. They are two tests because they disagree on
       exactly that frame. */
    {
        int first = (s_win++ == 0);
        s_recording = s_mirrorWant;
        if (first && s_recording) {
            pd_view_publish(v);
        }
        /* THE A/B ARMING, and that is the whole of it: the claim is taken
           when this window closes, in `tagpu_posedraw_end`. */
        if (!ghostWindow && s_recording && s_ab && !s_abDone)
            s_abTaking = 1;
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
    /* the integrity this line is for: the material entry names THIS
       geometry and agrees with it about the vertex count */
    if (m->geom != g || m->nvert != g->nvert) return NULL;
    if (u->npose < g->nparts) return NULL;
    if (g->nparts > TAGPU_PBMAXPIECE) return NULL;
    if (g->count[range] <= 0) return NULL;
    *mo = m;
    return g;
}

void tagpu_posedraw_unit(const TAGPU_PDUNIT* u)
{
    const TAGPU_PBMAT* m;
    const TAGPU_PBGEOM* g = unit_ok(u, &m, TAGPU_PB_BODY);
    if (!g) return;
    pd_record(u, g, m);
}

/* ---- closing the window -------------------------------------------------- */
/* CLOSES THE RECORDING WINDOW AND PUBLISHES IT. */
void tagpu_posedraw_end(void)
{
    if (s_state != 1) return;
    if (!s_recording) return;
    s_recording = 0;

    /* ONLY THE WINDOW THAT OPENED THE BRACKET CLOSES IT: a frame can have
       more than one window, so `s_abDone` alone is not enough. */
    if (s_abTaking) {
        s_abTaking = 0;
        s_abDone = 1;
        /* THE A/B CLAIM, of the Vulkan capture alone. `tagpu_vk_ab_arm` unlinks
           the target `_vk.ppm` at the instant the claim latches, which is what
           makes the file on the disk this arming's rather than an earlier
           run's; diff it against a capture from another BUILD. */
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
       slant and the build ghost all draw LATER in this
       frame than this window closes, and a count frozen now would miss exactly
       the draws the refusal exists to catch. It is read at the moment the
       hand-over is taken, which is later in this same iteration of
       render_vk.c's loop than every draw in the frame. */
    /* A GHOST FRAME IS AN ORDINARY FRAME FOR THIS INSTRUMENT, so the claim is
       not suppressed on one. What the lever claims is the VULKAN capture
       alone, diffed file-to-file against another BUILD, and both sides of that
       comparison carry whatever the frame had. Suppressing it would cost a
       capture per session: the unit window latches `s_abDone` and
       `tagpu_vk_ab_arm` unlinks the target, then the ghost window would
       republish `ab = 0`, and `s_abDone` only clears when the lever file is
       deleted -- so the one-shot would be spent and nothing written. */
    /* `s_abClaim` IS FRAME-SCOPED AND THIS LINE IS IDEMPOTENT: a SECOND
       window's `_end` re-runs it, so a claim consumed here would be dropped
       WHETHER OR NOT a ghost had been recorded. A frame whose queued build
       sites are all off screen opens the ghost window -- `ghost_pass` opens it
       on the site COUNT, before the per-site cull -- and records nothing, so
       the pair would be silently lost, with `s_abDone` latched so the one-shot
       never retried and the instrument reading as a port failure for the rest
       of the session. */
    s_pub.ab = s_abClaim;
    s_pubHave = 1;
}

/* ---- the Vulkan lane's hand-over ---------------------------------------- */
int tagpu_posedraw_handover(TAGPU_PDHAND* out, unsigned now)
{
    /* WHICH HALF REFUSED, ONCE. The consumer can say "no hand-over" but only
       this side knows whether there was nothing to give or it was stamped for
       another frame, and those have different causes -- the first is the pass
       standing down, the second is a counter disagreeing across the seam. One
       latch, cleared on the first success. */
    if (!s_pubHave || !out) {
        /* A WINDOW THAT NEVER OPENED IS THE ORDINARY CASE and must not be
           reported: the shell has no posed units, so `s_win` is 0 there on
           every frame and a latch spent on it hides the state that matters.
           What is worth a line is a window that OPENED and published nothing,
           which is a pass standing down mid-frame. */
        /* PERIODIC, AND IT REPORTS THE STATE RATHER THAN AN OPINION ABOUT IT.
           A one-shot latch would be spent on the shell's ordinary "no posed
           units" case and say nothing for the rest of the run. These four
           values are the whole
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
       `render_vk.c`: `tagpu_overlay_draw`, then `tagpu_vk_frame`, in one
       iteration on one thread. */
    s_pub.otherDraws = s_other;
    /* AND SO IS THE RESTORE LIST, FOR THE SAME REASON AND A SHARPER ONE.
       `tagpu_native.c`'s `ghost_record` runs after the FIRST posedraw window
       of the frame, in the same `tagpu_native_frame`, and can paint the unit
       atlas through `tagpu_r3d_atlas_uv` -> `atlas_get`. `atlas_paint` feeds
       the published list, so a capture at `pd_begin` would hand the consumer
       a count taken before the frame finished appending to it.

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
       timing argument. */
    /* THE INDEXED MIRROR IS READ ON THE SAME BEAT AS THE LIST, AND FOR THE
       SAME REASON. `atlas_paint` (tagpu_gaf.c) writes a cell's bytes into the
       mirror, bumps `mirrorSerial` and appends the cell to the list in one
       call, so a serial and a list read here together agree by construction:
       every listed frame's texels are in the mirror the serial names, and the
       consumer uploads that serial before it queues the frame. Read in
       `pd_view_publish`, BEFORE `ghost_record`, a build ghost that atlased a
       new unit's texture would publish the frame with the previous serial,
       the consumer would see nothing to upload, and the restorer would paint
       the cell from the texels the device still held there: palette index 0,
       opaque black, for the rest of the generation. */
    s_pub.atlas = tagpu_r3d_atlas_mirror(&s_pub.atlasDim, &s_pub.atlasRows,
                                         &s_pub.atlasSerial);
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
               one path only, and a reader here cannot see what zeroed it. */
            s_pub.atlasRgbAniso  = 0.0f;
            /* AND THE FLAG CANNOT OUTLIVE THE LIST IT PROMISES. `restored`
               is published ~440 lines above from
               `tagpu_r3d_atlas_restore_armed()`, one term; `restoreFrames`
               comes from `tagpu_r3d_atlas_restore_list()`, three. They agree
               only because `rlistWant` and `rlist` are set and cleared
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

/* ---- frame and reset ---------------------------------------------------- */
void tagpu_posedraw_frame(unsigned frame_counter)
{
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
        /* ASKED OF THE CONSUMER, NOT OF THE LEVER. `tagpu_vk_armed()` is true
        whenever `tagpu_vk.on` exists, even where the lever arms nothing, and
        these latches are one-way, so once asked the memory is held for the
        process's life -- a mirror paid for with no consumer at all.
        `tagpu_vk_owns_present()` is exactly "a Vulkan pass will run in this
        process", which is the question. */
        if (!s_mirrorWant && tagpu_vk_owns_present()) s_mirrorWant = 1;
        s_ab = GetFileAttributesA(PD_ABFILE) != INVALID_FILE_ATTRIBUTES;
        if (!s_ab) s_abDone = 0;
    }
}
