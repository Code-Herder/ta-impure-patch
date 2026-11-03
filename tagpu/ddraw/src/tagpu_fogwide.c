/* tagpu_fogwide.c -- the fog of war's source bits for the whole map, packed on the game thread.
   See tagpu_fogwide.h for where it runs and why the engine's own grid cannot serve a zoomed-out
   view, and inc/tagpu_fogsrc.h for the format and the expansion every reader applies. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_fogwide.h"
#include "tagpu_fogsrc.h"
#include "tagpu_addr.h"   /* tagpu_user_top: the range filters' upper end */
#include "tagpu_log.h"

/* the engine fields the pack reads, all in the TAdynmem block */
#define OFF_LOSTYPE  0x14281       /* u16; bit1 = true LOS, bit3 = grid current */
#define OFF_EYEX     0x1431F
#define OFF_EYEY     0x14323
#define OFF_PLOT_C   0x14233       /* i32 FeatureMapSizeX; LOS/MAPPED are half  */
#define OFF_PLOT_R   0x14237       /* i32 FeatureMapSizeY                       */
#define OFF_MAPPED   0x14273       /* u16* MAPPED bitmap, one bit per player    */
#define OFF_LOCALID  0x2A43        /* u8 — the LOCAL player, not the watched one */
#define OFF_PLAYERS  0x1B63        /* the player records, stride 0x14B          */
#define PLAYER_RECS  11            /* the engine constructs eleven (0x41D97E..0x41D997); the
                                      eleventh, record 10, is viewed only on a puppet level,
                                      whose viewer it seats (tagpu_replay.c)    */
#define PLAYER_STRIDE 0x14B
#define PLAYER_LOS   0x7C          /* {u8* counters; i32 w; i32 h} inside it    */
#define FOG_DESC     0x1421F       /* -> {u16* buf, i32 cols, i32 rows, i32 cells}: the engine's
                                      screen grid, read by the oracle alone          */

static void flog(const char* s)
{
    tagpu_log(s);
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p <= tagpu_user_top(); }

/* ---- what a pack reads -------------------------------------------------- */
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
   ONE buffer of packed bits, `stride * losH` words, grown and never shrunk; at most
   TAGPU_FOG_MAXDIM / 16 * TAGPU_FOG_MAXDIM words, 1 MiB. The render thread holds no pointer of
   ours: the publisher COPIES the words into the frame packet after the same draw, on this same
   thread, so a grow frees the old block on the spot and there is no critical section, no retire
   ring and no liveness test -- a frame either has the source in its packet or it does not. The
   consumer's bound is the packet's own `off + stride * h * 4 <= used_bytes`, checked at acquire
   (tagpu_packet.c). */
static uint32_t*        s_bits;
static size_t           s_bitsCap;                        /* words                 */
static int              s_init;

static int              s_pubValid;                       /* s_bits holds a source */
static int              s_pubW, s_pubH, s_pubStride;
static unsigned         s_pubData;                        /* bumped per pack       */

static volatile LONG    s_off;                            /* tagpu_fogwide.off */

/* game thread only: the heartbeat, so the cost of a pack is a number and not an estimate.
   Emitted on WALL TIME — see fogw_heartbeat for why not on ticks. */
static unsigned         s_hbTicks, s_hbPacks;
static double           s_hbUs, s_hbUsMax;
static DWORD            s_hbMs;
#define FOGW_HB_MS      5000

/* game thread only: the source last packed */
static const void*      s_lastLos;
static const void*      s_lastMapped;
static int              s_lastLosW, s_lastLosH, s_lastMask, s_lastTrue;
static int              s_saidFail;

/* ---- the pack ------------------------------------------------------------- */

/* The two bits of every cell of the map (tagpu_fogsrc.h, THE FORMAT), the tests `0x4843C0`
   makes per map cell: out of LOS only in true-LOS mode -- the engine writes the grey mask only
   under LosType&2, so outside it the bit is never set -- and never explored where the local
   player's MAPPED bit is clear. MAPPED is u16 a tile with one bit a player and the engine indexes
   it `(plotC*cy)/2 + cx`: its ROW stride is plotC BYTES, so it is plotC/2 tiles wide. The bound
   is its own allocation, plotC*plotR/2 bytes (`0x483CF6`); an index past it is not dark, as the
   engine's own loop skips it. Every LOS index is inside `losW x losH`, the map's dimensions
   fogw_source collected. */
static void fogw_pack(const FOGW_SRC* s, uint32_t* bits, int stride)
{
    int cx, cy;
    for (cy = 0; cy < s->losH; cy++) {
        const unsigned char* los = s->los + (size_t)cy * s->losW;
        uint32_t* row = bits + (size_t)cy * stride;
        int base = (s->plotC * cy) / 2;
        uint32_t word = 0;
        for (cx = 0; cx < s->losW; cx++) {
            unsigned idx = (unsigned)(base + cx), v = 0;
            if (idx < (unsigned)s->mappedCells && (s->mapped[idx] & s->mask) == 0)
                v = TAGPU_FOG_UNEXPLORED;
            if (s->trueLos && los[cx] == 0) v |= TAGPU_FOG_OUTOFLOS;
            word |= (uint32_t)v << (2 * (cx & 15));
            if ((cx & 15) == 15) { row[cx >> 4] = word; word = 0; }
        }
        if (s->losW & 15) row[s->losW >> 4] = word;
    }
}

/* ---- reading the sources ------------------------------------------------ */

/* Fill `s` from the engine, or return 0. Game thread, inside an in-play draw
   (tagpu_fogwide.h, WHERE IT RUNS, for why the maps are live there). Every
   field is range-checked, and every index the pack takes is bounded by the
   dimensions collected here. */
static int fogw_source(char* ta, FOGW_SRC* s)
{
    const char* ps;
    int id;

    memset(s, 0, sizeof *s);
    if (!ptr_ok(ta)) return 0;

    /* one of the eleven records, so the bit always lands inside the u16
       tile the engine masks with (`test ax,ax` after a 16-bit load). Record
       10's counters exist whenever it is viewed: a puppet level names it only
       between giving it its buffers and the teardown (tagpu_replay.c,
       viewer_frame's LIFETIME), and the counters pointer is range-checked
       below as every record's is */
    id = *(const unsigned char*)(ta + OFF_LOCALID);
    if (id >= PLAYER_RECS) return 0;
    s->mask = 1u << id;

    ps = ta + OFF_PLAYERS + (size_t)id * PLAYER_STRIDE + PLAYER_LOS;
    s->los  = *(const unsigned char* const*)(ps);
    s->losW = *(const int*)(ps + 4);
    s->losH = *(const int*)(ps + 8);
    /* TAGPU_FOG_MAXDIM is the packet's reserve and the terrain's own bound: a map past it is
       one the Vulkan lane draws no world for */
    if (!ptr_ok(s->los) || s->losW <= 0 || s->losH <= 0 ||
        s->losW > TAGPU_FOG_MAXDIM || s->losH > TAGPU_FOG_MAXDIM) return 0;

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

/* The published source as the expansion reads it; the caller has checked s_pubValid. */
static void fogw_view(TAGPU_FOGSRC* f)
{
    f->words = s_bits; f->w = s_pubW; f->h = s_pubH; f->stride = s_pubStride;
    f->serial = s_pubData;
}

/* ---- the oracle: our bits against the engine's own grid ------------------ */

/* The engine builder's own col0 for one axis (`0x48442D`..`0x484485`):
   trunc(eye/32), one less when the eye sits in the first half of its cell. */
static int eng_col0(int eye) { return eye / 32 - ((eye % 32) < 16 ? 1 : 0); }

/* One border completion's two rows (or columns) — the engine's literal index
   and the window build's derived one — marked for the oracle to leave out when
   they differ. Both are marked: that edge's completion is then defined
   differently by the two functions on each of them. */
static void edge_skip(unsigned char* skip, int n, int literal, int derived)
{
    if (literal == derived) return;
    if ((unsigned)literal < (unsigned)n) skip[literal] = 1;
    if ((unsigned)derived < (unsigned)n) skip[derived] = 1;
}

/* With tagpu_fogwide_check.on, every 120th tick, two comparisons on one line:

   `differ=`: the window build over the ENGINE's own window, from OUR packed bits
   (tagpu_fog_build_window), against the grid the engine just built there, on every
   entry the two functions define the same way. Zero proves the pack and the window
   replication.

   `mirror-differ=`: the whole-map expansion (tagpu_fog_entry, the C mirror of the
   shaders' taFogEntry and the expansion tagpu_fog_at answers from) against that same
   window build, on the entries the build fully defines -- all but its last row and
   column, which are short their far corners -- less the skipped lines, and only those
   that touch the map (an entry wholly off it is all zero in a window build, while the
   expansion repeats the straddling entry there). Zero proves the expansion every reader
   applies.

   THE BORDER COMPLETIONS ARE COMPARED ONLY WHERE BOTH FUNCTIONS PUT THEM. The
   engine completes its literal row 0 / rows-2 / column 0 / cols-2; the window build
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
   range, where the comparison is the whole grid. */
static void fogw_check(char* ta)
{
    static uint16_t* scratch;
    static unsigned frames;
    static int armed = -1;
    static unsigned char skipR[256], skipC[256];
    TAGPU_FOGSRC f;
    const int* fg;
    const unsigned short* buf;
    int cols, rows, cells, i, n, bad = 0, nz = 0, cmp = 0, col0, row0, mcmp = 0, mbad = 0;
    char b[224];

    if (armed < 0)
        armed = GetFileAttributesA("tagpu_fogwide_check.on") != INVALID_FILE_ATTRIBUTES;
    if (!armed) return;
    if ((frames++ % 120) != 0) return;
    if (!s_pubValid) return;

    fg = *(const int* const*)(ta + FOG_DESC);
    if (!ptr_ok(fg)) return;
    buf = (const unsigned short*)(size_t)fg[0];
    cols = fg[1]; rows = fg[2]; cells = fg[3];
    if (!ptr_ok(buf) || cols <= 0 || rows <= 0 || cols > 256 || rows > 256 ||
        cells != (((cols * rows) + 7) & ~7)) return;   /* the allocation, 0x483C84 */
    if (!scratch) scratch = (uint16_t*)malloc((size_t)256 * 256 * 2);
    if (!scratch) return;

    fogw_view(&f);
    col0 = eng_col0(*(const int*)(ta + OFF_EYEX));
    row0 = eng_col0(*(const int*)(ta + OFF_EYEY));
    tagpu_fog_build_window(&f, scratch, cols, rows, col0, row0);

    memset(skipR, 0, (size_t)rows);
    memset(skipC, 0, (size_t)cols);
    if (row0 < 0)                edge_skip(skipR, rows, 0, -row0 - 1);
    if (row0 + rows > f.h)       edge_skip(skipR, rows, rows - 2, f.h - 1 - row0);
    if (col0 < 0)                edge_skip(skipC, cols, 0, -col0 - 1);
    if (col0 + cols > f.w)       edge_skip(skipC, cols, cols - 2, f.w - 1 - col0);

    /* `cols*rows`, NOT `cells`: the engine ROUNDS ITS ALLOCATION UP to a
       multiple of 8 and clears all of it, while the window build fills exactly
       the grid. Comparing the tail would put up to 7 entries of untouched
       `scratch` against the engine's zeroed ones — it cannot make a real
       difference vanish, but on a reused heap block it invents one. */
    n = cols * rows;
    for (i = 0; i < n; i++) {
        int gx = i % cols, gy = i / cols, cx = col0 + gx, cy = row0 + gy;
        if (skipR[gy] || skipC[gx]) continue;
        cmp++;
        if (scratch[i] != buf[i]) bad++;
        if (buf[i]) nz++;
        if (gx < cols - 1 && gy < rows - 1 &&
            cx >= -1 && cx <= f.w - 1 && cy >= -1 && cy <= f.h - 1) {
            unsigned e = tagpu_fog_entry(&f, cx, cy);
            mcmp++;
            if (e != ((scratch[i] & 0xFu) | ((scratch[i] >> 8) & 0xFu) << 4)) mbad++;
        }
    }
    sprintf(b, "fogwide check: %dx%d compared=%d of cells=%d skipped=%d differ=%d "
               "engine-nonzero=%d truelos=%d mirror-compared=%d mirror-differ=%d",
            cols, rows, cmp, cells, n - cmp, bad, nz,
            (*(const unsigned short*)(ta + OFF_LOSTYPE) & 2) != 0, mcmp, mbad);
    flog(b);
}

/* ---- growing the buffer --------------------------------------------------- */

/* Room for `words`. Never shrinks: the size is the map's, so it moves only with a level, and
   growing once and keeping it costs nothing. THE OLD BLOCK IS FREED ON THE SPOT: the render
   thread holds no pointer into it — the publisher copies these words into the frame packet on
   this thread, in the same draw the pack ran in. A refusal publishes nothing until a later tick's
   request is served; at most 1 MiB is ever asked for. */
static int fogw_room(size_t words)
{
    uint32_t* nb;
    if (s_bits && words <= s_bitsCap) return 1;
    nb = (uint32_t*)malloc(words * sizeof *nb);
    if (!nb) {
        if (!s_saidFail) {
            char fb[128];
            sprintf(fb, "fogwide: could not allocate %u KB for the fog source -- no fog is "
                        "published until it can be", (unsigned)(words * 4 / 1024));
            flog(fb); s_saidFail = 1;
        }
        return 0;
    }
    free(s_bits);
    s_bits = nb;
    s_bitsCap = words;
    s_saidFail = 0;
    return 1;
}

static void fogw_heartbeat(void);
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
    int changed, stride;

    if (!s_init) return;                    /* attach did not run */
    fogw_poll_off();

    /* EVERY bail-out withdraws the published source: a tick that stops
       packing must not leave the publisher copying bits an LOS state that
       keeps moving has left behind. */
    if (s_off || !fogw_source(ta, &s)) {
        s_pubValid = 0;
        return;
    }
    fogw_heartbeat();

    /* Pack when the engine rebuilt (the LOS state moved), or when the maps themselves changed
       under us — a level change reallocates both and can leave the dimensions identical.
       Otherwise the published bits are still exactly what this tick would pack.

       WHAT THAT COSTS. `rebuilt` is the engine's own is-current bit, which every LOS stamp that
       touches the local player's maps clears (`0x481911`, `0x481D2D`, `0x481D73`, `0x482293` —
       terrain-depth.md §5.2), and every scroll as well; so the rate is the SIM TICK rate
       whenever anything at all is moving, ~30/s at gamespeed 10, and it is paid by every session
       on the Vulkan lane, including one that never zooms out. The heartbeat's `pack=` says what
       one pack costs. */
    changed = rebuilt ||
              (const void*)s.los != s_lastLos || (const void*)s.mapped != s_lastMapped ||
              s.losW != s_lastLosW || s.losH != s_lastLosH ||
              (int)s.mask != s_lastMask || s.trueLos != s_lastTrue ||
              !s_pubValid;
    if (changed) {
        LARGE_INTEGER f, a, b;
        stride = tagpu_fog_stride(s.losW);
        if (!fogw_room((size_t)stride * (size_t)s.losH)) {
            s_pubValid = 0;
            return;
        }
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        fogw_pack(&s, s_bits, stride);
        QueryPerformanceCounter(&b);
        if (f.QuadPart > 0) {
            double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
            s_hbUs += us;
            if (us > s_hbUsMax) s_hbUsMax = us;
        }
        s_hbPacks++;

        /* No swap and no section: the buffer just packed IS the one the publisher
           copies out of, later in this same draw, on this same thread. */
        s_pubW = s.losW; s_pubH = s.losH; s_pubStride = stride;
        s_pubData++;
        s_pubValid = 1;

        s_lastLos = s.los; s_lastMapped = s.mapped;
        s_lastLosW = s.losW; s_lastLosH = s.losH;
        s_lastMask = (int)s.mask; s_lastTrue = s.trueLos;
    }

    fogw_check(ta);
}

/* One line per FOGW_HB_MS of WALL TIME while a source is live, called at the top
   of the tick so a run that stops packing still reports.

   ON WALL TIME, NOT ON A TICK COUNT. A tick here is a `DrawGameScreen` call, not a presented frame, and the game loop turns
   that over as fast as the scene allows while the presenter caps only the flip:
   MEASURED 2026-09-10 at 1920x1080 at a 60 fps cap, both instances presenting
   58-60 fps, **330 ticks a second** on `crowd-static` (256 units, Two
   Continents) and **3200-4900** on a sparse Town & Country skirmish. So a
   block of 300 ticks is anywhere from a tenth of a second to a second — a
   count per block is a RATIO that reads like a rate, and no two runs compare —
   and a line per 300 ticks is 11-16 log writes a second on the game thread.
   Five seconds of wall time is one line per five seconds whatever the scene is
   doing, and the line carries the rate rather than leaving it to be
   reconstructed. */
static void fogw_heartbeat(void)
{
    char b[256];
    DWORD now = GetTickCount();
    double secs;

    s_hbTicks++;
    if (!s_hbMs) { s_hbMs = now; return; }     /* the first tick opens the window */
    if (now - s_hbMs < FOGW_HB_MS) return;
    secs = (double)(now - s_hbMs) / 1000.0;
    sprintf(b, "fogwide: %dx%d source, %u KB, packs=%u in %.1fs = %.1f/s ticks=%u "
               "pack=%.0f/%.0f us (mean/max)",
            s_pubValid ? s_pubW : 0, s_pubValid ? s_pubH : 0,
            s_pubValid ? (unsigned)((size_t)s_pubStride * s_pubH * 4 / 1024) : 0u,
            s_hbPacks, secs, secs > 0.0 ? (double)s_hbPacks / secs : 0.0, s_hbTicks,
            s_hbPacks ? s_hbUs / s_hbPacks : 0.0, s_hbUsMax);
    flog(b);
    s_hbMs = now;
    s_hbTicks = s_hbPacks = 0; s_hbUs = s_hbUsMax = 0.0;
}

/* Polled by the PRODUCER, on the game thread, at the top of the tick — not by
   a consumer. Polled by the consumer, the lever works only while something is
   consuming the source: on any frame the native pass never reaches the fog
   block, `s_off` is not refreshed and the game thread goes on packing bits
   nobody reads, with `tagpu_fogwide.off` sitting in the gamedir doing nothing.
   The producer is the one that must obey it, and it is also the thread that can
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

/* THE GAME THREAD'S HAND-OFF. The packet's publisher calls this from the same
   DrawGameScreen the tick above ran in, on this thread, and COPIES the words
   into the frame packet. There is no other reader: the render thread takes the
   copy out of its packet and this pointer never leaves the game thread. */
int tagpu_fogwide_source(TAGPU_FOGSRC* out)
{
    if (s_off || !s_init || !s_pubValid || !s_bits) return 0;
    fogw_view(out);
    return 1;
}
