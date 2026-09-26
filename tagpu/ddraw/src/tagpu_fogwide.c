/* tagpu_fogwide.c — the fog grid, over the window the ZOOM actually shows.
   See tagpu_fogwide.h for the problem, why the engine's own grid cannot be
   moved, and the hand-over discipline. This file is the replication of
   `0x4843C0` and the window arithmetic around it. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_fogwide.h"
#include "tagpu_zoom.h"
#include "tagpu_vpwide.h"
#include "tagpu_log.h"

/* the engine fields the builder reads, all in the TAdynmem block */
#define OFF_LOSTYPE  0x14281       /* u16; bit1 = true LOS, bit3 = grid current */
#define OFF_EYEX     0x1431F
#define OFF_EYEY     0x14323
#define OFF_PLOT_C   0x14233       /* i32 FeatureMapSizeX; LOS/MAPPED are half  */
#define OFF_PLOT_R   0x14237       /* i32 FeatureMapSizeY                       */
#define OFF_MAPPED   0x14273       /* u16* MAPPED bitmap, one bit per player    */
#define OFF_LOCALID  0x2A43        /* u8 — the LOCAL player, not the watched one */
#define OFF_PLAYERS  0x1B63        /* PlayerStruct[10], stride 0x14B            */
#define PLAYER_STRIDE 0x14B
#define PLAYER_LOS   0x7C          /* {u8* counters; i32 w; i32 h} inside it    */

/* THE BUFFER. One block, sized from the window the screen actually asks for
   (fogw_capacity below) rather than from a constant, grown and never shrunk.
   Nothing outside the game thread reads it — the publisher copies it into the
   packet on this thread, in the same draw the build ran in — so a grow frees
   the old block on the spot and there is no retire ring, no fence stamp and no
   lock.

   NOT A FIXED SQUARE: a 1024 square is 29x what 1920x1080 needs and 72x what
   1024x768 does, and still not enough past a 7680x4320 screen; and a size that
   a consumer re-types as its own constant can disagree with the producer's,
   which answers "nothing is hidden here" for every unit, wreck and effect past
   the smaller one (research/notes/fog-grid-sizing.md). Deriving the size is
   what removes that disagreement: the consumer's bound is the packet's own
   length (see the state block below).

   WHAT THE SIZE IS. The span of the window is fixed for a given viewport and
   zoom floor; only its PHASE moves with the eye, and the phase is worth exactly
   one cell (`col0` is `x0 - 16` floor-divided by 32, so the residue rides on
   the count). Sizing from this tick's `cols` would therefore reallocate every
   time the camera crossed a 32-world-pixel boundary — several times a second
   while scrolling. fogw_capacity sizes from the WORST residue instead, which
   takes the eye out of the expression; what is left moves only when the video
   mode does, which stock TA can only do at game entry (resolution.md §6) and
   this fork does across its game -> shell -> game cycles. */

static void flog(const char* s)
{
    tagpu_log(s);
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

/* ---- what a build reads ------------------------------------------------- */
typedef struct {
    const unsigned char*  los;      /* LOS overlap counters, losW x losH       */
    int                   losW, losH;
    const unsigned short* mapped;   /* MAPPED bits, indexed (plotC*cy)/2 + cx  */
    int                   mappedCells;
    int                   plotC, plotR;
    unsigned              mask;     /* 1 << local player id                    */
    int                   trueLos;  /* LosType & 2                             */
} FOGW_SRC;

/* ---- the state, ALL OF IT THE GAME THREAD'S ------------------------------
   ONE buffer. The render thread holds no pointer of ours at all: the grid is
   built here, inside the draw, and the publisher COPIES it into the frame
   packet after the same draw, on this same thread. So a grow can free the old
   block on the spot, and there is no critical section, no retire ring and no
   liveness test — a frame either has a wide grid in its packet or it does not.

   The consumer's bound is the packet's own `len == cols*rows*2`, checked once
   at acquire (tagpu_packet.c), which is exact rather than generous; its
   refusal is a packet without a wide grid, which the native pass counts on its
   own line. */
static unsigned short*  s_grid;
static int              s_capCols, s_capRows;
static int              s_init;

static int              s_pubValid;                       /* s_grid holds a grid */
static int              s_pubCols, s_pubRows, s_pubOrgX, s_pubOrgY;
static unsigned         s_pubData;                        /* bumped per rebuild */

static volatile LONG    s_off;                            /* tagpu_fogwide.off */

/* game thread only: the heartbeat, so the cost of a rebuild is a number and not
   an estimate. Emitted on WALL TIME — see fogw_heartbeat for why not on ticks. */
static unsigned         s_hbTicks, s_hbBuilds;
static double           s_hbUs, s_hbUsMax;
static DWORD            s_hbMs;
#define FOGW_HB_MS      5000

/* game thread only: the window last built, and the source it was built from */
static int              s_lastCols, s_lastRows, s_lastCol0, s_lastRow0;
static const void*      s_lastLos;
static const void*      s_lastMapped;
static int              s_lastLosW, s_lastLosH, s_lastMask, s_lastTrue;
static int              s_said;                           /* one-shot diagnostics */
static int              s_saidFail;
/* The capacity request that malloc refused, and when. A different request asks
   at once; the same one asks again after FOGW_RETRY_MS, because a refusal that
   is never forgiven pins the grid smaller for the rest of the session — see
   fogw_alloc. */
static int              s_failCols, s_failRows;
static DWORD            s_failMs;
#define FOGW_RETRY_MS   5000

/* ---- the replication of 0x4843C0 ---------------------------------------- */

/* One dark map cell sets a DIFFERENT corner bit in each of the four grid
   entries around it (terrain-depth.md §5.2). `hi` picks the byte: 0 is the
   unexplored mask, 1 the out-of-LOS one. Every test is unsigned exactly as the
   engine's `jae` pairs are, so a negative index is rejected rather than wrapped
   — that is the bound on every write here. */
static void corner_or(unsigned char* g, int cols, int rows, int gx, int gy, int hi)
{
    if ((unsigned)gx < (unsigned)cols && (unsigned)gy < (unsigned)rows)
        g[((size_t)gy * cols + gx) * 2 + hi] |= 1;
    if ((unsigned)(gx - 1) < (unsigned)cols && (unsigned)gy < (unsigned)rows)
        g[((size_t)gy * cols + gx - 1) * 2 + hi] |= 2;
    if ((unsigned)gx < (unsigned)cols && (unsigned)(gy - 1) < (unsigned)rows)
        g[((size_t)(gy - 1) * cols + gx) * 2 + hi] |= 4;
    if ((unsigned)(gx - 1) < (unsigned)cols && (unsigned)(gy - 1) < (unsigned)rows)
        g[((size_t)(gy - 1) * cols + gx - 1) * 2 + hi] |= 8;
}

/* `0x4843C0` over an arbitrary window: grid entry (0,0) covers map cell
   (col0,row0) and the three cells right/below it, exactly as the engine's does
   for its own eye-derived col0/row0. `out` must hold cols*rows entries. */
static void fogw_build(const FOGW_SRC* s, unsigned short* out,
                       int cols, int rows, int col0, int row0)
{
    unsigned char* g = (unsigned char*)out;
    int cx, cy, i;

    memset(out, 0, (size_t)cols * rows * 2);

    for (cy = row0; cy < row0 + rows; cy++) {
        if ((unsigned)cy >= (unsigned)s->losH) continue;   /* engine: `jae` skip */
        for (cx = col0; cx < col0 + cols; cx++) {
            int gx, gy, idx;
            if ((unsigned)cx >= (unsigned)s->losW) continue;
            gx = cx - col0; gy = cy - row0;
            /* out of LOS, and only in true-LOS mode: the grey mask. The engine
               writes it only under LosType&2, which is why an inactive mode is
               an all-zero mask and costs nothing downstream. */
            if (s->trueLos && s->los[(size_t)cy * s->losW + cx] == 0)
                corner_or(g, cols, rows, gx, gy, 1);
            /* never explored: the black mask. The MAPPED map is u16 per tile
               with one bit per player, and the engine indexes it
               `(plotC*cy)/2 + cx` — i.e. its ROW stride is plotC BYTES, so it
               is plotC/2 tiles wide. The bound is its own allocation,
               plotC*plotR/2 bytes (`0x483CF6`). */
            idx = (s->plotC * cy) / 2 + cx;
            if ((unsigned)idx >= (unsigned)s->mappedCells) continue;
            if ((s->mapped[idx] & s->mask) == 0)
                corner_or(g, cols, rows, gx, gy, 0);
        }
    }

    /* The four map-border completions (`0x4846A1`..`0x4848CD`), in the engine's
       own order — they read bits an earlier block may have set, so the order is
       part of the result. Off the map there are no cells to darken the corners,
       so each edge copies the corner bits it does have outward.

       THE ROW EACH ONE FIXES IS DERIVED, not the engine's literal 0 / rows-2.
       The entry that straddles an edge is the one whose corners on one side are
       the map's outermost cells and on the other side are off it — for the top,
       `row0 + gy < 0 <= row0 + gy + 1`, so `gy = -row0 - 1`. The engine writes
       row 0, which is that row only while its window overshoots the map by at
       most one cell (`row0` 0 or -1, which an eye in the engine's own range
       `[0, extent - W]` guarantees, `extent` the scroll extent
       main+0x1422B); ours reaches as far as the zoom does, and
       the camera's centre range takes the engine's window as far as W/2 past
       the edge, where the literal index lands dozens of rows out in open
       water — which is why the native pass takes this grid whenever the eye
       is off the engine's own range (tagpu_zoom_wide_fog). The same reading
       gives `losH-1-row0` for the bottom, which is the engine's `rows-2`
       whenever its window overshoots by exactly the one cell. */
    {
        int gTop = -row0 - 1;
        int gBot = s->plotR / 2 - 1 - row0;
        int gLef = -col0 - 1;
        int gRig = s->plotC / 2 - 1 - col0;

        if (row0 < 0 && gTop >= 0 && gTop < rows)
            for (i = 0; i < cols; i++) {
                size_t k = ((size_t)gTop * cols + i) * 2;
                if (s->trueLos) {
                    if (g[k + 1] & 4) g[k + 1] |= 1;
                    if (g[k + 1] & 8) g[k + 1] |= 2;
                }
                if (g[k] & 4) g[k] |= 1;
                if (g[k] & 8) g[k] |= 2;
            }
        if (row0 + rows > s->plotR / 2 && gBot >= 0 && gBot < rows)
            for (i = 0; i < cols; i++) {
                size_t k = ((size_t)gBot * cols + i) * 2;
                if (s->trueLos) {
                    if (g[k + 1] & 1) g[k + 1] |= 4;
                    if (g[k + 1] & 2) g[k + 1] |= 8;
                }
                if (g[k] & 1) g[k] |= 4;
                if (g[k] & 2) g[k] |= 8;
            }
        if (col0 < 0 && gLef >= 0 && gLef < cols)
            for (i = 0; i < rows; i++) {
                size_t k = ((size_t)i * cols + gLef) * 2;
                if (s->trueLos) {
                    if (g[k + 1] & 8) g[k + 1] |= 4;
                    if (g[k + 1] & 2) g[k + 1] |= 1;
                }
                if (g[k] & 8) g[k] |= 4;
                if (g[k] & 2) g[k] |= 1;
            }
        if (col0 + cols > s->plotC / 2 && gRig >= 0 && gRig < cols)
            for (i = 0; i < rows; i++) {
                size_t k = ((size_t)i * cols + gRig) * 2;
                if (s->trueLos) {
                    if (g[k + 1] & 4) g[k + 1] |= 8;
                    if (g[k + 1] & 1) g[k + 1] |= 2;
                }
                if (g[k] & 4) g[k] |= 8;
                if (g[k] & 1) g[k] |= 2;
            }
    }
}

/* THE ONE DELIBERATE DEPARTURE from `0x4843C0`, and it is applied only to the
   entries that lie WHOLLY off the map — never to one the engine's own grid can
   contain, which is why fogw_check() compares the function above and not this
   one.

   Past the edge entry the completions just fixed, every entry is all-zero: no
   map cell reaches it, so nothing ever ORs a corner bit into it, and an
   all-zero entry means "no fog". The engine's grid meets that case only for
   an eye off its own range, where the native pass takes this one; ours spans
   the zoom, so at a shore there can be forty rows of "no fog" over open
   water. Terrain draws nothing there,
   so most of it is invisible; what is not is a sprite whose PROJECTED position
   (`y - alt/2`, the space the grid is built in) lands past the edge while its
   anchor is on the map. A tree on the high north shore does exactly that, and
   would draw in full colour above the shoreline with the whole map fogged
   behind it.
   Replicating the edge entry outward is what the engine's single off-map row
   already amounts to. */
static void fogw_edge_fill(const FOGW_SRC* s, unsigned short* out,
                           int cols, int rows, int col0, int row0)
{
    int gTop = -row0 - 1, gBot = s->plotR / 2 - 1 - row0;
    int gLef = -col0 - 1, gRig = s->plotC / 2 - 1 - col0;
    int i, j;

    /* rows first, so the columns then carry the corners outward with them */
    if (gTop > 0 && gTop < rows)
        for (i = 0; i < gTop; i++)
            memcpy(out + (size_t)i * cols, out + (size_t)gTop * cols,
                   (size_t)cols * 2);
    if (gBot >= 0 && gBot < rows - 1)
        for (i = gBot + 1; i < rows; i++)
            memcpy(out + (size_t)i * cols, out + (size_t)gBot * cols,
                   (size_t)cols * 2);
    if (gLef > 0 && gLef < cols)
        for (j = 0; j < rows; j++)
            for (i = 0; i < gLef; i++)
                out[(size_t)j * cols + i] = out[(size_t)j * cols + gLef];
    if (gRig >= 0 && gRig < cols - 1)
        for (j = 0; j < rows; j++)
            for (i = gRig + 1; i < cols; i++)
                out[(size_t)j * cols + i] = out[(size_t)j * cols + gRig];
}

/* ---- reading the sources ------------------------------------------------ */

/* Fill `s` from the engine, or return 0. Game thread, inside an in-play draw
   (tagpu_fogwide.h, WHERE IT RUNS, for why the maps are live there). Every
   field is range-checked, and every index the build takes is bounded by the
   dimensions collected here. */
static int fogw_source(char* ta, FOGW_SRC* s)
{
    const char* ps;
    int id;

    memset(s, 0, sizeof *s);
    if (!ptr_ok(ta)) return 0;

    /* ten players, so the bit always lands inside the u16 tile the engine
       masks with (`test ax,ax` after a 16-bit load) */
    id = *(const unsigned char*)(ta + OFF_LOCALID);
    if (id > 9) return 0;
    s->mask = 1u << id;

    ps = ta + OFF_PLAYERS + (size_t)id * PLAYER_STRIDE + PLAYER_LOS;
    s->los  = *(const unsigned char* const*)(ps);
    s->losW = *(const int*)(ps + 4);
    s->losH = *(const int*)(ps + 8);
    if (!ptr_ok(s->los) || s->losW <= 0 || s->losH <= 0 ||
        s->losW > 4096 || s->losH > 4096) return 0;

    s->plotC = *(const int*)(ta + OFF_PLOT_C);
    s->plotR = *(const int*)(ta + OFF_PLOT_R);
    if (s->plotC <= 0 || s->plotR <= 0 || s->plotC > 8192 || s->plotR > 8192)
        return 0;
    /* the engine's own loop bounds cx/cy by the LOS map and then indexes MAPPED
       with them, which only holds while the two agree on the map size */
    if (s->plotC / 2 != s->losW || s->plotR / 2 != s->losH) return 0;

    s->mapped = *(const unsigned short* const*)(ta + OFF_MAPPED);
    if (!ptr_ok(s->mapped)) return 0;
    s->mappedCells = (s->plotC * s->plotR) / 4;     /* u16 entries; see 0x483CF6 */
    if (s->mappedCells <= 0) return 0;

    s->trueLos = (*(const unsigned short*)(ta + OFF_LOSTYPE) & 2) != 0;
    return 1;
}

/* ---- the window --------------------------------------------------------- */

static int floor_div32(int v) { return v >= 0 ? v / 32 : -(((-v) + 31) / 32); }

/* The window the widest view can need, as the map-cell coordinates the builder
   indexes by. Returns 0 when the engine's viewport does not look sane.

   Sized for the zoom floor (TAGPU_ZOOM_MIN) and NOT for the level in force: the level the
   game thread can read is the one the render thread published on the previous
   frame, so a window cut to that level would be a frame behind every outward
   tween — and one frame of the tween can cross the whole range. Sizing for the
   whole range means no change of level can outrun the grid, at the cost of a
   window over 20x the 1x viewport's area while any zoom-out is live.

   WHAT IT COVERS. The window is the one the game thread publishes about this
   eye (tagpu_zoom_pub_window): the native pass's gather slab at the floor —
   the effective span centred on the 1x viewport's centre as the pass centres
   it, plus TAGPU_GATHER_MARGIN, the slack the pass accepts anchors in — grown
   by the lead, a quarter of the viewport on each side, and rounded outward to
   whole cells with the spare column and row the builder needs. At any level
   the pass's slab is no wider, so every eye within the lead of this build is
   one the render thread may draw from at every level. An eye further AHEAD —
   the cursor anchor's steps the game thread has not applied yet past the
   lead, or the scenario camera's jump after the fog site — is the render
   thread's to bound: the fog bound in tagpu_zoom.c clamps the eye it draws
   from until the slab lies inside the grid the packet carries, so no fog
   sample of any frame lands past this window. A gesture that only zooms in is never held by it: each
   notch's term is at most `(vw/2)` times its drop in distance and the eye and
   the distance are lerped along the same ease, so `|U| <= (vw/2)(1/z_built -
   1/z)` and the view stays a subset of the one before it; a gesture that only
   zooms out adds no displacement. What it holds is a reversal (a notch out
   that cuts a notch in whose displacement is still owed) that the game thread
   has not caught up with, for as long as that lasts. */
/* One axis of the window about `eye`: the world rect the game thread publishes
   about it (tagpu_zoom_pub_window, the function the unit and anchor windows
   take theirs from), put onto the engine's own lattice. No further clamp: a
   clamp would only make this grid narrower than the slab it exists to cover. */
static void fogw_axis(int eye, int v, int* c0, int* n)
{
    int lo, span;

    tagpu_zoom_pub_window(eye, v, &lo, &span);
    /* a grid corner sits at a map cell's centre, so entry 0 covers map cell
       c0 and the origin is 32*c0+16 */
    *c0 = floor_div32(lo - 16);
    /* +2, not +1: the builder fills entry gx from map cells c0+gx and
       c0+gx+1, so the LAST column and row of any window are short their
       right/bottom corners — which is why the engine's own border completion
       works on cols-2 and rows-2. One spare each way keeps the short ones
       outside anything the view can show. */
    *n = (lo + span - (32 * *c0 + 16) + 31) / 32 + 2;
}

static int fogw_window(char* ta, int vw, int vh,
                       int* col0, int* row0, int* cols, int* rows)
{
    /* vw/vh come from fogw_view, sampled once for this tick and shared with
       fogw_capacity, so the two cannot derive from different viewports. The
       one bound on the window is the allocated set below, and since it is
       SIZED from the same function (fogw_capacity) it is not a limit on the
       screen either. */
    fogw_axis(*(const int*)(ta + OFF_EYEX), vw, col0, cols);
    fogw_axis(*(const int*)(ta + OFF_EYEY), vh, row0, rows);
    if (*cols < 3 || *rows < 3) return 0;
    /* Clamp to what is ALLOCATED, and take the trim off both ends so the
       view's centre keeps the cover. fogw_capacity sized the set for this same
       viewport at the worst residue and fogw_alloc ran first, so in the normal
       case this is inert — it bites only when a malloc failed and we are still
       running on a smaller set. A zoomed-out slab can then be wider than the
       grid: the fog bound centres it and the native pass counts the frame
       (`out=`), which beats no wide grid at all. */
    if (*cols > s_capCols) { *col0 += (*cols - s_capCols) / 2; *cols = s_capCols; }
    if (*rows > s_capRows) { *row0 += (*rows - s_capRows) / 2; *rows = s_capRows; }
    return 1;
}

/* ---- the oracle: our replication against the engine's own grid ----------- */

/* The engine builder's own col0 for one axis (`0x48442D`..`0x484485`):
   trunc(eye/32), one less when the eye sits in the first half of its cell. */
static int eng_col0(int eye) { return eye / 32 - ((eye % 32) < 16 ? 1 : 0); }

/* One border completion's two rows (or columns) — the engine's literal index
   and fogw_build's derived one — marked for the oracle to leave out when they
   differ. Both are marked: that edge's completion is then defined differently
   by the two functions on each of them. */
static void edge_skip(unsigned char* skip, int n, int literal, int derived)
{
    if (literal == derived) return;
    if ((unsigned)literal < (unsigned)n) skip[literal] = 1;
    if ((unsigned)derived < (unsigned)n) skip[derived] = 1;
}

/* With tagpu_fogwide_check.on: build over the ENGINE's own window and compare
   with the grid the engine just built there, on every entry the two functions
   define the same way. A replication that is right is byte-identical there;
   anything else is a number in the log, not a guess.

   THE BORDER COMPLETIONS ARE COMPARED ONLY WHERE BOTH FUNCTIONS PUT THEM. The
   engine completes its literal row 0 / rows-2 / column 0 / cols-2; fogw_build
   completes the entry that straddles the map edge, `-row0-1` and so on (exe
   map, "The four border completions"). Those are the same line while the
   window overshoots the map by at most one cell — every eye in the engine's
   own range `[0, extent - W]` — and not otherwise: past it (`row0 <= -2`, or the
   far edges' equivalent, which the camera's centre range reaches) the
   engine's line lies wholly off the map and its completion is a no-op on an
   all-zero line, while ours completes the straddling line the engine leaves
   half-set. That difference is the derived index doing its job, so for each
   completion whose gate is up and whose two lines differ, both lines are left
   out — and nothing else can differ because of them: every entry depends only
   on its own four cells, and each completion copies bits within one entry.
   `skipped=` counts the entries left out; 0 for any eye in the engine's own
   range, where the comparison is the whole grid as before. */
static void fogw_check(char* ta, const FOGW_SRC* s)
{
    static unsigned short* scratch;
    static unsigned frames;
    static int armed = -1;
    static unsigned char skipR[256], skipC[256];
    const int* fg;
    const unsigned short* buf;
    int cols, rows, cells, i, n, bad = 0, nz = 0, cmp = 0, col0, row0;
    char b[192];

    if (armed < 0)
        armed = GetFileAttributesA("tagpu_fogwide_check.on") != INVALID_FILE_ATTRIBUTES;
    if (!armed) return;
    if ((frames++ % 120) != 0) return;

    fg = *(const int* const*)(ta + 0x1421F);
    if (!ptr_ok(fg)) return;
    buf = (const unsigned short*)(size_t)fg[0];
    cols = fg[1]; rows = fg[2]; cells = fg[3];
    if (!ptr_ok(buf) || cols <= 0 || rows <= 0 || cols > 256 || rows > 256 ||
        cells != (((cols * rows) + 7) & ~7)) return;   /* the allocation, 0x483C84 */
    if (!scratch) scratch = (unsigned short*)malloc((size_t)256 * 256 * 2);
    if (!scratch) return;

    col0 = eng_col0(*(const int*)(ta + OFF_EYEX));
    row0 = eng_col0(*(const int*)(ta + OFF_EYEY));
    fogw_build(s, scratch, cols, rows, col0, row0);

    memset(skipR, 0, (size_t)rows);
    memset(skipC, 0, (size_t)cols);
    if (row0 < 0)                  edge_skip(skipR, rows, 0, -row0 - 1);
    if (row0 + rows > s->plotR / 2) edge_skip(skipR, rows, rows - 2, s->plotR / 2 - 1 - row0);
    if (col0 < 0)                  edge_skip(skipC, cols, 0, -col0 - 1);
    if (col0 + cols > s->plotC / 2) edge_skip(skipC, cols, cols - 2, s->plotC / 2 - 1 - col0);

    /* `cols*rows`, NOT `cells`: the engine ROUNDS ITS ALLOCATION UP to a
       multiple of 8 and clears all of it, while fogw_build fills exactly the
       grid. Comparing the tail would put up to 7 entries of untouched
       `scratch` against the engine's zeroed ones — it cannot make a real
       difference vanish, but on a reused heap block it invents one. */
    n = cols * rows;
    for (i = 0; i < n; i++) {
        if (skipR[i / cols] || skipC[i % cols]) continue;
        cmp++;
        if (scratch[i] != buf[i]) bad++;
        if (buf[i]) nz++;
    }
    sprintf(b, "fogwide check: %dx%d compared=%d of cells=%d skipped=%d differ=%d "
               "engine-nonzero=%d truelos=%d",
            cols, rows, cmp, cells, n - cmp, bad, nz, s->trueLos);
    flog(b);
}

/* ---- growing the set, and giving the old one back ----------------------- */

/* ONE SAMPLE OF THE VIEWPORT PER TICK, taken here and passed to both
   fogw_capacity and fogw_window. tagpu_vpwide_true_rect is not guaranteed to
   answer the same twice: when its derived path is unavailable it falls back to
   the live OFF_VIEW_W/H fields (which nothing of ours writes). Two different
   answers would make the capacity and the window disagree, and then
   fogw_window's clamp — documented as inert — would be load bearing instead.
   One sample removes the question. */
static int fogw_view(char* ta, int* vw, int* vh)
{
    int vpL, vpT;
    tagpu_vpwide_true_rect(ta, &vpL, &vpT, vw, vh);
    return !(*vw < 64 || *vh < 64 || *vw > 16384 || *vh > 16384);
}

/* The size the CURRENT video mode asks for, independent of where the eye is.
   Same function as fogw_window's, evaluated at the worst residue: the count
   is `ceil((span + r)/32) + 2` for a residue r in [0,31], so the largest it
   can be is at r = 31. The span does not depend on the eye. */
static int fogw_capacity(int vw, int vh, int* capCols, int* capRows)
{
    int lo, spanX, spanY;

    tagpu_zoom_pub_window(0, vw, &lo, &spanX);
    tagpu_zoom_pub_window(0, vh, &lo, &spanY);
    *capCols = (spanX + 31 + 31) / 32 + 2;
    *capRows = (spanY + 31 + 31) / 32 + 2;
    return (*capCols >= 3 && *capRows >= 3);
}

/* Ensure the buffer covers capCols x capRows. Never shrinks: the window's span
   is fixed for a viewport and the zoom floor, so the only thing that moves it
   is a video-mode change, and growing once and keeping it costs nothing.

   THE OLD BLOCK IS FREED ON THE SPOT. The render thread holds no pointer into
   it — the publisher copies these bytes into the frame packet on this thread,
   in the same draw the build ran in — so the only reader of this block is the
   caller below and the copy after it, both here. */
static int fogw_alloc(int capCols, int capRows)
{
    size_t bytes;
    unsigned short* nb;

    if (s_grid && capCols <= s_capCols && capRows <= s_capRows) return 1;
    /* THE CLAMP COMES FIRST: the buffer never shrinks, so what is actually
       requested is the componentwise max of the ask and what we already have.
       Latching the pre-clamp ask and comparing the post-clamp one would never
       match when one dimension shrinks while the other grows — which is
       exactly the window-resize case the backoff exists for. */
    if (capCols < s_capCols) capCols = s_capCols;
    if (capRows < s_capRows) capRows = s_capRows;

    /* A REQUEST THAT COULD NOT BE SERVED IS NOT RETRIED EVERY TICK. This
       function is reached at the DRAW rate, not the sim rate — hundreds to
       thousands of times a second — so under memory pressure an un-latched
       retry is a malloc/free pair per call, for as long as the pressure lasts,
       on the thread this module is otherwise careful to keep at ~4 ms/s.

       IT MUST NOT STOP ASKING ALTOGETHER. Memory pressure is transient, and a latch cleared only by a *successful*
       grow pins the buffer at whatever size it had when the one refusal
       happened: the window is then clamped inside the view for the rest of the
       session, on-screen cells fall off the grid, and the fog gate answers 0 —
       "nothing is hidden here" — for every one of them. So the latch EXPIRES.
       The interval is a quality knob and not a safety argument: running on the
       smaller buffer is correct at any cadence, and this only decides how soon
       a transient failure is forgiven. */
    if (capCols == s_failCols && capRows == s_failRows &&
        GetTickCount() - s_failMs < FOGW_RETRY_MS)
        return s_grid != NULL;

    bytes = (size_t)capCols * capRows * 2;
    nb = (unsigned short*)malloc(bytes);
    if (!nb) {
        s_failCols = capCols; s_failRows = capRows; s_failMs = GetTickCount();
        if (!s_saidFail) {
            char fb[128];
            sprintf(fb, "fogwide: could not allocate %dx%d (%d KB) — staying "
                        "on %dx%d until the window changes",
                    capCols, capRows, (int)(bytes / 1024), s_capCols, s_capRows);
            flog(fb); s_saidFail = 1;
        }
        /* Keep whatever we already had: a smaller buffer still covers a clamped
           window (fogw_window), where no buffer at all covers nothing. */
        return s_grid != NULL;
    }

    free(s_grid);
    s_grid = nb;
    s_capCols = capCols; s_capRows = capRows;
    s_failCols = s_failRows = 0;
    s_pubValid = 0;                       /* the new block holds nothing yet */
    {
        char b[160];
        sprintf(b, "fogwide: grid %dx%d, %d KB (%d cells)%s",
                capCols, capRows, (int)(bytes / 1024), capCols * capRows,
                s_said ? " — grown" : "");
        flog(b); s_said = 1;
    }
    return 1;
}

static void fogw_heartbeat(int cols, int rows);
static void fogw_poll_off(void);

/* DllMain only. There is nothing to create; the entry point exists so the
   module has an explicit arming point and the tick has something to test. */
void tagpu_fogwide_init(void)
{
    s_init = 1;
}

void tagpu_fogwide_tick(char* ta, int rebuilt)
{
    FOGW_SRC s;
    int col0, row0, cols, rows, changed, capCols, capRows, vw, vh;

    if (!s_init) return;                    /* attach did not run */
    fogw_poll_off();

    /* EVERY bail-out withdraws the published grid, not just the first two: a
       tick that stops building must not leave the publisher copying a grid
       anchored at an eye that has moved on.

       THE LIVE ZOOM LEVEL IS NOT ONE OF THEM. A 1:1 view is spanned by the
       engine's own grid, but the level a gate here could read is published by
       the RENDER thread, and the render thread is the one that decides,
       mid-frame, to draw the first zoomed-out frame of a gesture. Gated on it,
       that frame asks for a wide grid the game thread has had no tick to
       build, falls back to the engine's, and the ring beyond the 1x viewport
       comes out with NO FOG at all — a full-brightness flash of terrain lasting
       exactly one frame, at the start of every zoom-out. (Measured 2026-09-10
       at the bottom-left corner of Town & Country, unmapped: one bare frame and
       one rebuild per gesture.) A producer that cannot see the future must not
       be gated on it: build every tick, and let the CONSUMER decide per frame
       which grid the frame it is drawing needs.

       WHAT THAT COSTS, and it is NOT only while the camera moves. `changed`
       below is `rebuilt || the window moved || …`, and `rebuilt` is the engine's
       own is-current bit, which every LOS stamp that touches the local player's
       maps clears (`0x481911`, `0x481D2D`, `0x481D73`, `0x482293` —
       terrain-depth.md §5.2). So the rate is the SIM TICK rate whenever
       anything at all is moving, units included: MEASURED 2026-09-10 at
       1920x1080 on `200v200`, 400 units fighting, camera still, zoom 1.0 —
       **760 rebuilds in 25.0 s = 30.4/s** (gamespeed 10's 30 Hz tick) at 145 us
       mean, so **~4.4 ms of game-thread time per second**, doubling at
       gamespeed 20. It is bounded by the tick rate rather than by the unit
       count, and it is paid by every session, including one that never zooms
       out — that is the price of the grid being ready before the frame that
       needs it, and the alternative is a frame drawn without one. */
    if (s_off ||
        !fogw_source(ta, &s) ||
        !fogw_view(ta, &vw, &vh) ||
        !fogw_capacity(vw, vh, &capCols, &capRows) ||
        !fogw_alloc(capCols, capRows) ||
        !fogw_window(ta, vw, vh, &col0, &row0, &cols, &rows)) {
        s_pubValid = 0;
        return;
    }
    fogw_heartbeat(cols, rows);

    fogw_check(ta, &s);

    /* Rebuild when the engine rebuilt (the LOS state moved), when the window
       moved, or when the maps themselves changed under us — a level change
       reallocates both and can leave the dimensions identical. Otherwise the
       published grid is still exactly what this tick would produce. */
    changed = rebuilt ||
              cols != s_lastCols || rows != s_lastRows ||
              col0 != s_lastCol0 || row0 != s_lastRow0 ||
              (const void*)s.los != s_lastLos || (const void*)s.mapped != s_lastMapped ||
              s.losW != s_lastLosW || s.losH != s_lastLosH ||
              (int)s.mask != s_lastMask || s.trueLos != s_lastTrue ||
              !s_pubValid;
    if (!changed) return;

    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        fogw_build(&s, s_grid, cols, rows, col0, row0);
        fogw_edge_fill(&s, s_grid, cols, rows, col0, row0);
        QueryPerformanceCounter(&b);
        if (f.QuadPart > 0) {
            double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
            s_hbUs += us;
            if (us > s_hbUsMax) s_hbUsMax = us;
        }
        s_hbBuilds++;
    }

    /* No swap and no section: the buffer just built IS the one the publisher
       copies out of, later in this same draw, on this same thread. */
    s_pubCols = cols; s_pubRows = rows;
    s_pubOrgX = 32 * col0 + 16; s_pubOrgY = 32 * row0 + 16;
    s_pubData++;
    s_pubValid = 1;

    s_lastCols = cols; s_lastRows = rows; s_lastCol0 = col0; s_lastRow0 = row0;
    s_lastLos = s.los; s_lastMapped = s.mapped;
    s_lastLosW = s.losW; s_lastLosH = s.losH;
    s_lastMask = (int)s.mask; s_lastTrue = s.trueLos;
}

/* One line per FOGW_HB_MS of WALL TIME while a grid is live, called at the top
   of the tick so a run that stops rebuilding still reports.

   ON WALL TIME, NOT ON A TICK COUNT. A tick here is a `DrawGameScreen` call, not a presented frame, and the game loop turns
   that over as fast as the scene allows while the presenter caps only the flip:
   MEASURED 2026-09-10 at 1920x1080 at a 60 fps cap, both instances presenting
   58-60 fps, **330 ticks a second** on `crowd-static` (256 units, Two
   Continents) and **3200-4900** on a sparse Town & Country skirmish. So a
   block of 300 ticks is anywhere from a tenth of a second to a second — a
   count per block is a RATIO that reads like a rate, and no two runs compare —
   and a line per 300 ticks is 11-16 log writes a second on the game thread,
   in a session that never zooms out. Five seconds of wall time is one
   line per five seconds whatever the scene is doing, and the line carries the
   rate rather than leaving it to be reconstructed. */
static void fogw_heartbeat(int cols, int rows)
{
    char b[288];
    DWORD now = GetTickCount();
    double secs;

    s_hbTicks++;
    if (!s_hbMs) { s_hbMs = now; return; }     /* the first tick opens the window */
    if (now - s_hbMs < FOGW_HB_MS) return;
    secs = (double)(now - s_hbMs) / 1000.0;
    /* No hand-over counters: there is one buffer, nothing is retired, and
       whether a frame found a wide grid in its packet is the native pass's
       count, on its own line. */
    sprintf(b, "fogwide: %dx%d cells=%d cap=%dx%d rebuilds=%u in %.1fs = %.1f/s "
               "ticks=%u build=%.0f/%.0f us (mean/max)",
            cols, rows, cols * rows, s_capCols, s_capRows, s_hbBuilds, secs,
            secs > 0.0 ? (double)s_hbBuilds / secs : 0.0, s_hbTicks,
            s_hbBuilds ? s_hbUs / s_hbBuilds : 0.0, s_hbUsMax);
    flog(b);
    s_hbMs = now;
    s_hbTicks = s_hbBuilds = 0; s_hbUs = s_hbUsMax = 0.0;
}

/* ---- the render thread's side ------------------------------------------- */

/* Polled by the PRODUCER, on the game thread, at the top of the tick — not by
   a consumer. Polled by the consumer, the lever works only while something is
   consuming the grid: on any frame the native pass never reaches the fog
   block, `s_off` is not refreshed and the game thread goes on rebuilding a
   ~36k-cell grid nobody reads, with `tagpu_fogwide.off` sitting in the gamedir
   doing nothing. The producer is the one that must obey it, and it is also the thread that can
   then publish "invalid" and take the consumer down with it in one step. The
   engine's own loop does file I/O throughout, and tagpu_opt.c's readers poll
   from this thread on the same cadence. */
static void fogw_poll_off(void)
{
    static DWORD tick;                       /* game thread only */
    static int   first = 1;
    DWORD now = GetTickCount();
    if (first || now - tick > 250) {
        first = 0;
        tick = now;
        s_off = (GetFileAttributesA("tagpu_fogwide.off") != INVALID_FILE_ATTRIBUTES);
    }
}

/* THE GAME THREAD'S HAND-OFF. The packet's publisher calls this from the same DrawGameScreen the tick above
   ran in, on this thread, and COPIES the bytes into the frame packet. There is
   no other reader: the render thread takes the copy out of its packet and this
   pointer never leaves the game thread.

   0 means this frame's packet carries no wide grid — the module is off, attach
   did not run, the tick has not built one yet, or a bail-out withdrew it — and
   the consumer then uses the engine's own grid. */
int tagpu_fogwide_current(const unsigned short** buf,
                          int* cols, int* rows, int* orgX, int* orgY)
{
    if (s_off || !s_init || !s_pubValid || !s_grid) return 0;
    *buf = s_grid; *cols = s_pubCols; *rows = s_pubRows;
    *orgX = s_pubOrgX; *orgY = s_pubOrgY;
    return 1;
}
