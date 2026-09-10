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

/* The cap on a wide grid. ALLOCATED ONCE AND NEVER GROWN, and that is a
   lifetime argument, not a convenience: `tagpu_fogwide_get` hands the RENDER
   thread a pointer into `s_pub` while the game thread builds into `s_build`,
   so a buffer that could be reallocated under a zoom change would be a
   use-after-free with a window nobody would ever reproduce. The size is
   therefore chosen for the widest window worth covering, up front.

   IT USED TO BE DERIVED FROM TWO CONSTANTS THAT NO LONGER EXIST. `tagpu_native.c`
   refused a viewport over 4096 a side and clamped the effective span to 8192,
   which made 275 cells the widest window reachable and 320 a comfortable cap.
   Both were resolution limits and both are gone (the viewport bound is 16384
   and the span is the viewport at the zoom floor), so the window a real screen
   asks for is now: **485 cells at 3840x2160, 645 at 5120x2880, 965 at
   7680x4320** — every one of them past 320, which would have clamped the wide
   grid back to a 10240-px-wide core and left the rest of a 4K zoom-out smeared
   from the border cell again. 1024 covers all three and costs 2 MB a buffer,
   6 MB for the three, allocated only for a session that actually zooms out.

   THE RESIDUAL, stated so it stays true: past about 7680x4320 the window is
   clamped again. The clamp takes its trim off both ends (fogw_window below),
   so what is left is centred on the view and the smear returns only at the
   outer edge — the same degradation as before, moved out by a factor of three
   and a half. */
#define FOGW_MAXDIM  1024
#define FOGW_MARGIN  256           /* world px of slack on each side (below)   */

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
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

/* ---- the shared state --------------------------------------------------- */
/* Initialised at DLL ATTACH (tagpu_fogwide_init), before either thread that
   uses it exists. It was lazy — `if (!s_csInit) { InitializeCriticalSection(); 
   s_csInit = 1; }` on the game thread with the render thread testing the flag —
   and that is a publication race the compiler is allowed to lose: `s_csInit` is
   static and its address never escapes, so nothing stops the store being sunk
   ahead of the opaque call, and the render thread would then enter a section
   that was never initialised. Doing it at attach removes the ordering question
   rather than arguing about it. */
static CRITICAL_SECTION s_cs;
static volatile LONG    s_csInit;

/* Three buffers, three owners. `s_build` is the game thread's alone, `s_hold`
   the render thread's alone, `s_pub` the hand-over slot. A swap only ever
   exchanges two of the three pointers, and every swap is under s_cs, so the
   three stay pairwise distinct: the game thread can never be writing the buffer
   the render thread is reading. */
static unsigned short*  s_build;
static unsigned short*  s_pub;
static unsigned short*  s_hold;

static int              s_pubValid;                       /* s_pub holds a grid */
static int              s_pubCols, s_pubRows, s_pubOrgX, s_pubOrgY;
static unsigned         s_pubData;                        /* bumped per rebuild */

/* Liveness. The game thread bumps s_tick EVERY tick, rebuild or not, so the
   render thread can tell "nothing changed" from "the game thread stopped
   ticking" — terrain ownership disarmed, a menu, a level between maps.

   Measured in WALL TIME, not in render frames. The first cut of this compared
   the render thread's frame counter against the frame the tick last moved and
   gave up after two, which silently assumes the game loop runs at least as fast
   as the presenter. Uncapped (`--maxfps 0`) it does not: at 1920x1080 the
   render thread outran it several to one, every frame read "stale", and the
   wide grid was never used at all — the fog looked exactly as it does with the
   module off, with the lever making no difference either way. Wall time has no
   opinion about either rate. */
static volatile LONG    s_tick;
static volatile LONG    s_off;                            /* tagpu_fogwide.off */
#define FOGW_STALE_MS   500

/* render thread only */
static int              s_holdValid, s_holdCols, s_holdRows, s_holdOrgX, s_holdOrgY;
static unsigned         s_holdData;
static LONG             s_seenTick;
static DWORD            s_seenMs;

/* game thread only: the heartbeat, so the cost of a rebuild is a number and not
   an estimate. One line per 300 ticks, the cadence every other pass logs at. */
static unsigned         s_hbTicks, s_hbBuilds;
static double           s_hbUs, s_hbUsMax;

/* game thread only: the window last built, and the source it was built from */
static int              s_lastCols, s_lastRows, s_lastCol0, s_lastRow0;
static const void*      s_lastLos;
static const void*      s_lastMapped;
static int              s_lastLosW, s_lastLosH, s_lastMask, s_lastTrue;
static int              s_said;                           /* one-shot diagnostics */

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
       row 0 because its own grid never reaches more than one cell past the map
       (the eye clamp sees to that, so `row0` is only ever 0 or -1) and the two
       are then the same row; ours reaches as far as the zoom does, and the
       literal index would land dozens of rows out in open water. The same
       reading gives `losH-1-row0` for the bottom, which is the engine's
       `rows-2` whenever its window overshoots by exactly the one cell. */
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
   all-zero entry means "no fog". The engine never meets that case — its grid
   stops one cell past the map — but ours spans the zoom, so at a shore there
   can be forty rows of "no fog" over open water. Terrain draws nothing there,
   so most of it is invisible; what is not is a sprite whose PROJECTED position
   (`y - alt/2`, the space the grid is built in) lands past the edge while its
   anchor is on the map. A tree on the high north shore does exactly that, and
   drew in full colour above the shoreline with the whole map fogged behind it.
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

/* Fill `s` from the engine, or return 0. Game thread, at the fog overlay's own
   call site: the engine's builder reads these very allocations there, which is
   what makes them safe to read at all. Every field is range-checked, and every
   index the build takes is bounded by the dimensions collected here. */
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

   Sized from `tagpu_zoom_min()` and NOT from the level in force: the level the
   game thread can read is the one the render thread published on the previous
   frame, so a window cut to that level would be a frame behind every outward
   ease — and one ease step of a wheel flick is far more than the engine grid's
   own two cells of slack. Sizing for the whole range means no step of any lever
   can outrun the grid, at the cost of a window that is 16x the 1x viewport's
   area while any zoom-out is live. */
static int fogw_window(char* ta, int* col0, int* row0, int* cols, int* rows)
{
    int vpL, vpT, vw, vh, eyeX, eyeY, evw, evh, x0, y0, x1, y1;
    float zmin = tagpu_zoom_min();

    tagpu_vpwide_true_rect(ta, &vpL, &vpT, &vw, &vh);
    /* the same sanity bound the native pass applies to the same field — it was
       4096 in both, which refused a 5120x2880 screen the whole wide grid */
    if (vw < 64 || vh < 64 || vw > 16384 || vh > 16384) return 0;
    if (zmin < 0.05f || zmin > 1.0f) return 0;
    eyeX = *(const int*)(ta + OFF_EYEX);
    eyeY = *(const int*)(ta + OFF_EYEY);

    /* the same span the native pass gathers over (tagpu_native.c), at the
       widest level the levers reach */
    evw = (int)((float)vw / zmin) + 64;
    evh = (int)((float)vh / zmin) + 64;
    /* No span clamp: this expression IS what the native pass gathers over now.
       It used to be clamped to a fixed 8192 here because the native pass
       clamped there too, and once that went the clamp would only have made
       this grid narrower than the view it exists to cover. FOGW_MAXDIM is the
       one bound left, and it is the buffer, not the screen. */
    if (evw < vw) evw = vw;
    if (evh < vh) evh = vh;

    /* World rect, centred on the 1x viewport's centre exactly as the pass
       centres its own effective rect, plus FOGW_MARGIN: the eye can move
       between this build and the frames that read it (the render thread is not
       lock-stepped to the game loop), and the margin is what absorbs that
       rather than leaving a bare strip at the leading edge. */
    x0 = eyeX - (evw - vw) / 2 - FOGW_MARGIN;
    y0 = eyeY - (evh - vh) / 2 - FOGW_MARGIN;
    x1 = x0 + evw + 2 * FOGW_MARGIN;
    y1 = y0 + evh + 2 * FOGW_MARGIN;

    /* onto the engine's own lattice — a grid corner sits at a map cell's
       centre, so entry (0,0) covers map cell col0 and the origin is 32*col0+16 */
    *col0 = floor_div32(x0 - 16);
    *row0 = floor_div32(y0 - 16);
    /* +2, not +1: the builder fills entry gx from map cells col0+gx and
       col0+gx+1, so the LAST column and row of any window are short their
       right/bottom corners — which is why the engine's own border completion
       works on cols-2 and rows-2. One spare each way keeps the short ones
       outside anything the view can show. */
    *cols = (x1 - (32 * *col0 + 16) + 31) / 32 + 2;
    *rows = (y1 - (32 * *row0 + 16) + 31) / 32 + 2;
    if (*cols < 3 || *rows < 3) return 0;
    /* Clamp, and take the trim off both ends so the view's centre keeps the
       cover. Reachable only past a 7680x4320 screen now — see FOGW_MAXDIM. */
    if (*cols > FOGW_MAXDIM) { *col0 += (*cols - FOGW_MAXDIM) / 2; *cols = FOGW_MAXDIM; }
    if (*rows > FOGW_MAXDIM) { *row0 += (*rows - FOGW_MAXDIM) / 2; *rows = FOGW_MAXDIM; }
    return 1;
}

/* ---- the oracle: our replication against the engine's own grid ----------- */

/* The engine builder's own col0 for one axis (`0x48442D`..`0x484485`):
   trunc(eye/32), one less when the eye sits in the first half of its cell. */
static int eng_col0(int eye) { return eye / 32 - ((eye % 32) < 16 ? 1 : 0); }

/* With tagpu_fogwide_check.on: build over the ENGINE's own window and compare
   with the grid the engine just built there. A replication that is right is
   byte-identical; anything else is a number in the log, not a guess. */
static void fogw_check(char* ta, const FOGW_SRC* s)
{
    static unsigned short* scratch;
    static unsigned frames;
    static int armed = -1;
    const int* fg;
    const unsigned short* buf;
    int cols, rows, cells, i, n, bad = 0, nz = 0;
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

    fogw_build(s, scratch, cols, rows,
               eng_col0(*(const int*)(ta + OFF_EYEX)),
               eng_col0(*(const int*)(ta + OFF_EYEY)));
    /* `cols*rows`, NOT `cells`: the engine ROUNDS ITS ALLOCATION UP to a
       multiple of 8 and clears all of it, while fogw_build fills exactly the
       grid. Comparing the tail put up to 7 entries of untouched `scratch`
       against the engine's zeroed ones — it cannot make a real difference
       vanish, but on a reused heap block it invents one, on the instrument
       this whole landing rests on. */
    n = cols * rows;
    for (i = 0; i < n; i++) {
        if (scratch[i] != buf[i]) bad++;
        if (buf[i]) nz++;
    }
    sprintf(b, "fogwide check: %dx%d compared=%d of cells=%d differ=%d "
               "engine-nonzero=%d truelos=%d",
            cols, rows, n, cells, bad, nz, s->trueLos);
    flog(b);
}

/* ---- the tick ----------------------------------------------------------- */

static int fogw_alloc(void)
{
    size_t bytes = (size_t)FOGW_MAXDIM * FOGW_MAXDIM * 2;
    if (s_build && s_pub && s_hold) return 1;
    if (!s_build) s_build = (unsigned short*)malloc(bytes);
    if (!s_pub)   s_pub   = (unsigned short*)malloc(bytes);
    if (!s_hold)  s_hold  = (unsigned short*)malloc(bytes);
    if (s_build && s_pub && s_hold) {
        if (!s_said) {
            char b[128];
            sprintf(b, "fogwide: %d KB for the wide grid (3 x %dx%d cells)",
                    (int)(bytes * 3 / 1024), FOGW_MAXDIM, FOGW_MAXDIM);
            flog(b); s_said = 1;
        }
        return 1;
    }
    return 0;
}

static void fogw_heartbeat(int cols, int rows);
static void fogw_poll_off(void);

void tagpu_fogwide_init(void)
{
    if (s_csInit) return;
    InitializeCriticalSection(&s_cs);
    s_csInit = 1;
}

void tagpu_fogwide_tick(char* ta, int rebuilt)
{
    FOGW_SRC s;
    int col0, row0, cols, rows, changed;

    if (!s_csInit) return;                  /* attach did not run, or refused */
    InterlockedIncrement(&s_tick);          /* "the game thread is still here" */
    fogw_poll_off();

    /* EVERY bail-out withdraws the published grid, not just the first two.
       `s_tick` has already advanced by the time any of them is reached, so the
       render thread's liveness rule cannot catch a producer that is still
       ticking but has stopped building — it would go on painting a grid
       anchored at an eye that has moved on. Nothing to do while the view is
       1:1 either: the engine's own grid spans it, and leaving it in place keeps
       the fog at 1x bit for bit what it has always been. */
    if (s_off || tagpu_zoom_level() >= 1.0f ||
        !fogw_source(ta, &s) ||
        !fogw_window(ta, &col0, &row0, &cols, &rows) ||
        !fogw_alloc()) {
        if (s_pubValid) {
            EnterCriticalSection(&s_cs);
            s_pubValid = 0;
            LeaveCriticalSection(&s_cs);
        }
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
        fogw_build(&s, s_build, cols, rows, col0, row0);
        fogw_edge_fill(&s, s_build, cols, rows, col0, row0);
        QueryPerformanceCounter(&b);
        if (f.QuadPart > 0) {
            double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
            s_hbUs += us;
            if (us > s_hbUsMax) s_hbUsMax = us;
        }
        s_hbBuilds++;
    }

    EnterCriticalSection(&s_cs);
    {
        unsigned short* t = s_pub; s_pub = s_build; s_build = t;
        s_pubCols = cols; s_pubRows = rows;
        s_pubOrgX = 32 * col0 + 16; s_pubOrgY = 32 * row0 + 16;
        s_pubData++;
        s_pubValid = 1;
    }
    LeaveCriticalSection(&s_cs);

    s_lastCols = cols; s_lastRows = rows; s_lastCol0 = col0; s_lastRow0 = row0;
    s_lastLos = s.los; s_lastMapped = s.mapped;
    s_lastLosW = s.losW; s_lastLosH = s.losH;
    s_lastMask = (int)s.mask; s_lastTrue = s.trueLos;
}

/* One line per 300 ticks while a wide grid is live, called at the top of the
   tick so a run that stops rebuilding still reports. `rebuilds` is out of the
   ticks in the block — it reads 300/300 while the camera is moving, because
   every scroll invalidates the engine's grid and ours with it. */
static void fogw_heartbeat(int cols, int rows)
{
    char b[160];
    if (++s_hbTicks < 300) return;
    sprintf(b, "fogwide: %dx%d cells=%d rebuilds=%u/%u build=%.0f/%.0f us (mean/max)",
            cols, rows, cols * rows, s_hbBuilds, s_hbTicks,
            s_hbBuilds ? s_hbUs / s_hbBuilds : 0.0, s_hbUsMax);
    flog(b);
    s_hbTicks = s_hbBuilds = 0; s_hbUs = s_hbUsMax = 0.0;
}

/* ---- the render thread's side ------------------------------------------- */

/* Polled by the PRODUCER, on the game thread, at the top of the tick — not by
   the render thread inside tagpu_fogwide_get(). It was the other way round, and
   then the lever only worked while something was consuming the grid: on any
   frame the native pass never reached the fog block, `s_off` was never
   refreshed and the game thread went on rebuilding a ~36k-cell grid nobody
   read, with `tagpu_fogwide.off` sitting in the gamedir doing nothing. The
   producer is the one that must obey it, and it is also the thread that can
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

int tagpu_fogwide_get(const unsigned short** buf,
                      int* cols, int* rows, int* orgX, int* orgY)
{
    LONG tick;
    DWORD now;

    if (s_off || !s_csInit) return 0;

    /* Liveness: the game thread bumps s_tick every tick whether or not it
       rebuilds, so a tick that stops advancing means it is no longer running —
       terrain ownership disarmed, a menu — and the caller must go back to the
       engine's own grid rather than keep painting a grid anchored at an eye
       that has moved on. Half a second is the bound on how long a grid may
       outlive its tick; a game loop stalled longer than that is a frozen game.

       A grid that is merely one game tick behind the presenter is NOT stale and
       must not be refused: the engine's own grid is rebuilt on that same thread
       and is exactly as old. */
    tick = s_tick;
    now = GetTickCount();
    if (tick != s_seenTick) { s_seenTick = tick; s_seenMs = now; }
    else if (now - s_seenMs > FOGW_STALE_MS) { s_holdValid = 0; return 0; }

    EnterCriticalSection(&s_cs);
    if (!s_pubValid) {
        s_holdValid = 0;
    } else if (s_pubData != s_holdData) {
        unsigned short* t = s_hold; s_hold = s_pub; s_pub = t;
        s_holdCols = s_pubCols; s_holdRows = s_pubRows;
        s_holdOrgX = s_pubOrgX; s_holdOrgY = s_pubOrgY;
        s_holdData = s_pubData;
        s_holdValid = 1;
    } else {
        /* the held buffer already IS this version — the stall that cleared the
           flag did not take the grid with it */
        s_holdValid = 1;
    }
    LeaveCriticalSection(&s_cs);

    if (!s_holdValid || !s_hold) return 0;
    *buf = s_hold; *cols = s_holdCols; *rows = s_holdRows;
    *orgX = s_holdOrgX; *orgY = s_holdOrgY;
    return 1;
}
