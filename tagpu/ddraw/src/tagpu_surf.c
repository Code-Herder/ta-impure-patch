/* TA's own frame, snapshotted on the GAME thread for the render thread to diff
   against. The header carries the argument -- the ordering, the two-buffer
   ownership rule and what the in-play-only hook does not cover; this is the
   mechanism. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"                     /* g_ddraw.primary / g_ddraw.cs / g_ddraw.bpp */
#include "IDirectDrawSurface.h"     /* ->surface, ->pitch, ->width, ->height      */
#include "IDirectDrawPalette.h"     /* ->palette->data_rgb                        */
#include "tagpu_surf.h"
#include "tagpu_log.h"

/* THE LOG IS THE HOUSE PATTERN AND IT IS NOT SYNCHRONISED, deliberately. Every
   module in the DLL logs exactly this way, open-append-close with no lock, at
   55 call sites, and not one of them takes a lock; `tagpu_reclaim.c`'s is
   called from both threads too. This file logs from both: the game thread
   writes the re-read check and the heartbeat, the render thread the drop and
   the dump. The cost of that is an interleaved line in a diagnostic file. It is
   not a correctness surface -- no snapshot state is carried through it and
   nothing reads it back -- so locking it here alone would buy tidier logs in one
   file out of thirty-odd while making this module's logging unlike every other
   module's. If the log is ever made to matter, it is one lock in one place for
   all of them. */
static void slog(const char* s)
{
    tagpu_log(s);
}

/* ONE SNAPSHOT. Two of these exist and the ownership rule in the header says
   which thread may touch which: the game thread writes `s_snap[1 - s_hold]`,
   the render thread reads `s_snap[s_hold]`, and `have` therefore has exactly
   one writer (the game thread, into the buffer it owns). `drop` obeys the same
   rule and is what makes the level test exact: the game thread stamps each
   capture with the level it was taken under, so both the adopt and the serve
   compare two facts instead of consulting a flag somebody has to clear at the
   right moment. */
typedef struct {
    unsigned char* bytes;
    unsigned       cap;                 /* what `bytes` will hold, in bytes   */
    int            w, h;
    unsigned char  pal[1024];           /* R,G,B,255                          */
    unsigned       serial, palSerial;
    unsigned       stamp;
    /* THE LEVEL THIS CAPTURE BELONGS TO, as `s_dropReq` stood when the copy
       STARTED -- and the copy is thrown away entirely if the counter moved
       while it ran.

       Reading it AFTER the copy is NOT the safe side: it is what makes a
       straddling capture look CURRENT. The bytes are the dying level's, the
       stamp is the new level's, so the adopt matches (`s_dropSeen` has just
       advanced to the same value) and the serve matches (`s_dropReq` is that
       value), and the dead level's final frame is handed out as the new level's
       reference for the whole loading screen -- no in-play draw runs there to
       displace it. Reading it BEFORE is safe in every interleaving: a teardown
       landing before the load stamps the new level either way, and one landing
       after it is refused by both readers. */
    unsigned       drop;
    int            have;                /* this buffer holds a usable frame   */
} SNAP;

static SNAP s_snap[2];

/* THE THREE WORDS THE HAND-OVER IS. `s_hold` is written by the render thread
   only, and only while nothing is in flight; `s_req` by the render thread only;
   `s_ack` by the game thread only. The release/acquire pairs on `s_req` and
   `s_ack` are what publish everything else, `s_hold` included -- `volatile`
   orders nothing and is not used as though it did (tagpu_packet.c states the
   same reasoning where it picks __atomic over InterlockedExchange). */
static int      s_hold;                 /* the buffer the RENDER thread owns  */
static unsigned s_req;                  /* render -> game: bumped to ask      */
static unsigned s_ack;                  /* game -> render: the request served */

/* Render-thread only. */
static int      s_holdValid;            /* we have adopted at least one capture */
static unsigned s_dropSeen;             /* the level-end counter we have acted on */
static int      s_dx, s_dy, s_dw, s_dh; /* this frame's letterboxed rect      */
static unsigned s_asked, s_waited;

/* Game-thread only, except where the counters are read by the log line the
   game thread itself prints. */
static unsigned s_serial = 1;           /* monotonic across both buffers      */
static unsigned s_palSerial = 1;
static unsigned s_dropReq;              /* game -> render: the level ended    */
static unsigned s_captures, s_unchanged, s_refused, s_norequest;
/* CAPTURES THROWN AWAY BECAUSE A TEARDOWN LANDED WHILE THEY RAN. It is 0, and
   on today's binary nothing is expected to make it fire: this can only happen
   if a level teardown arrives on a thread that is not the one copying, and the
   teardown `0x491B60` was surveyed and measured on 2026-09-20 and is
   game-thread only -- its address is never taken, so its six call sites are all
   of them, and every teardown measured came in on the game thread
   (exe-reverse-engineering.md, "Who enters 0x491B60"; note that the identity
   rests on the measurement, because all six sites are dispatched through a
   function pointer and the call graph stops there). The test stays because
   it is one compare and it keeps this module correct without depending on that
   survey staying true; the counter stays because it is the thing that would
   say so if it ever stopped being. */
static unsigned s_straddle;
/* THE ORACLE, armed on the render thread and read on both. `s_reread` is the
   direct test of this module's invariant and `s_dumpArm` is how anyone looks
   at the golden source at all -- it has no consumer in the tree, so without
   this there is no way to see it.

   `s_reread` HAS TWO MODES, AND THE SECOND ONE EXISTS BECAUSE THE FIRST CANNOT
   MEASURE WHAT THIS MODULE CLAIMS. `tagpu_surfdump.on` is one shot, so a run of
   it yields one double-read. At the render-thread site's measured tear rate of
   1.35 % (223 of 16 500), twenty-four clean checks happen 72 % of the time --
   so "0 of 24" is exactly what a site tearing that badly would produce: a
   sample too small to see the thing it is looking for is not a measurement.

   So `tagpu_surfcheck.on` (mode 2) re-reads on EVERY capture for as long as the
   file is there, keeps the totals, and says nothing unless a check comes back
   non-zero.

   `__atomic` rather than `volatile` for the reason stated three lines above the
   hand-over's own words: `volatile` orders nothing, and a file that says so
   must not then make two exceptions for its own diagnostics. RELAXED is all any
   of them needs -- a missed or late arm is the whole consequence. */
#define RR_OFF   0u
#define RR_ONCE  1u
#define RR_EVERY 2u
static unsigned s_reread;               /* game: read the primary twice, compare */
static unsigned s_dumpArm;              /* render: write the next snapshot out    */
/* The continuous oracle's tally. Game thread writes, and the render thread
   reads them once when the lever is taken away -- atomics for that crossing. */
static unsigned s_rrChecks, s_rrTorn, s_rrWorst, s_rrBytes;
static unsigned s_usTotal, s_usMax, s_usN;
static int      s_saidRefused, s_saidFirst;
static LARGE_INTEGER s_freq;

#define SURF_LOG_EVERY 300u             /* captures between heartbeat lines   */

/* ---- the game thread ---------------------------------------------------- */

/* THE COPY ITSELF. Called with a request outstanding, so `s_hold` cannot move
   and `d` is ours alone. Returns 1 when `d` holds a usable frame.

   THE CRITICAL SECTION IS THE WEAKER OF TWO ARGUMENTS HERE AND IS TAKEN
   ANYWAY. On this thread the primary cannot be freed under us -- the game
   thread is the one that NULLs `g_ddraw.primary` inside the section and frees
   the object after leaving it -- and the bytes' writer is this thread, which is
   not running. The section costs nothing at 30 draws a second and keeps one
   rule for every reader of this object in the DLL. */
static int take_into(SNAP* d, const SNAP* prev, unsigned stamp, int* rrBad, int* rrShout)
{
    int ok = 0;

    *rrBad = -1;                        /* the re-read oracle did not run */
    *rrShout = 0;

    EnterCriticalSection(&g_ddraw.cs);
    /* THE BOUND COMES FROM THE OBJECT BEING READ. `g_ddraw.width/height` is the
       device MODE; the bytes and the pitch belong to the PRIMARY, which carries
       its own geometry, and between a mode change and the primary being
       recreated the two disagree -- so `h * pitch` off the mode can run past the
       primary's allocation. CLAUDE.md: a value is DATA until it has been
       validated as data. */
    if (g_ddraw.primary && g_ddraw.primary->surface && g_ddraw.primary->palette &&
        /* THE PRIMARY'S OWN DEPTH, for the same reason as its own geometry two
           lines down, and not `g_ddraw.bpp` -- the device MODE. `ddsurface.c`
           stamps the surface's `bpp` when it creates it and `dd.c` sets the
           mode's at SetDisplayMode, so between the two the pair disagrees, and
           an 8 from the mode over a 16bpp primary takes `w` bytes a row and
           calls them palette indices: a wrong picture, in bounds. */
        g_ddraw.primary->bpp == 8 &&
        g_ddraw.primary->width > 0 && g_ddraw.primary->height > 0) {
        int w = g_ddraw.primary->width, h = g_ddraw.primary->height;
        int pitch = g_ddraw.primary->pitch ? (int)g_ddraw.primary->pitch : w;
        unsigned need = (unsigned)w * (unsigned)h;
        if (w > TAGPU_SURF_MAXDIM || h > TAGPU_SURF_MAXDIM) {
            s_refused++;
        } else if (pitch >= w && need) {
            if (need > d->cap) {
                unsigned char* nb = (unsigned char*)realloc(d->bytes, need);
                if (nb) { d->bytes = nb; d->cap = need; }
            }
            if (d->cap >= need) {
                const unsigned char* src = (const unsigned char*)g_ddraw.primary->surface;
                const RGBQUAD* q = g_ddraw.primary->palette->data_rgb;
                unsigned char pal[1024];
                int y, i;
                /* CHANGED AGAINST THE PREVIOUS SNAPSHOT, not against whatever
                   this buffer held two captures ago. `prev` is the buffer the
                   render thread holds and nothing writes it while we read --
                   two readers is not a hazard. A geometry change is a change
                   whatever the rows say. */
                int diff = !prev->have || prev->w != w || prev->h != h;
                for (y = 0; y < h; y++) {
                    const unsigned char* sr = src + (size_t)y * pitch;
                    unsigned char* dr = d->bytes + (size_t)y * w;
                    /* ROW BY ROW, because the primary's pitch is not its width:
                       the copy takes the pitch OUT so that every consumer
                       uploads w*h with no row stride of its own to get wrong. */
                    if (!diff && memcmp(prev->bytes + (size_t)y * w, sr, (size_t)w) != 0)
                        diff = 1;
                    memcpy(dr, sr, (size_t)w);
                }
                /* THE INVARIANT, MEASURED RATHER THAN ASSERTED
                   (`tagpu_surfdump.on`). This module's whole claim is that at
                   this point nothing is writing these bytes -- the thread that
                   writes them is this one, and it is here. So a SECOND read of
                   the same rows must agree with the first, byte for byte,
                   however much the game is moving. It is read against what we
                   just copied, so it costs no buffer; a non-zero answer means
                   the claim is false and the site is wrong. On the render
                   thread, mid-frame, this is exactly the number that cannot be
                   bounded. */
                {
                  unsigned mode = __atomic_load_n(&s_reread, __ATOMIC_RELAXED);
                  if (mode != RR_OFF) {
                    unsigned bad = 0;
                    int yy;
                    for (yy = 0; yy < h; yy++) {
                        const unsigned char* sr2 = src + (size_t)yy * pitch;
                        const unsigned char* dr2 = d->bytes + (size_t)yy * w;
                        int x;
                        for (x = 0; x < w; x++) if (dr2[x] != sr2[x]) bad++;
                    }
                    /* THE LINE IS WRITTEN OUTSIDE THE SECTION, at the foot of
                       the caller: `slog` opens, writes and closes a file, and
                       this is the one place that would hold `g_ddraw.cs` over
                       something unbounded -- with the render thread's own use
                       of that section (tagpu_pal.c) waiting behind it.

                       THE COMPARE ITSELF IS STILL INSIDE, and in RR_EVERY that
                       is a `w*h` scalar pass holding `g_ddraw.cs` on every
                       capture -- about a millisecond at 1024x768, with the
                       render thread (ABOVE_NORMAL priority) able to block
                       behind it. It has to be inside: the second read is of
                       the PRIMARY, and the section is what keeps that pointer
                       live. This is why the continuous mode
                       is a probe you arm for a measurement and take away again,
                       and never a play default. */
                    if (mode == RR_ONCE) {
                        __atomic_store_n(&s_reread, RR_OFF, __ATOMIC_RELAXED);
                        *rrShout = 1;          /* a one-shot always says its answer */
                    } else {
                        /* CONTINUOUS: keep the tally, and speak only when the
                           invariant is violated. One line per capture would be
                           1 800 a minute and would itself perturb what it
                           measures. */
                        __atomic_fetch_add(&s_rrChecks, 1u, __ATOMIC_RELAXED);
                        if (bad) {
                            __atomic_fetch_add(&s_rrTorn, 1u, __ATOMIC_RELAXED);
                            __atomic_fetch_add(&s_rrBytes, bad, __ATOMIC_RELAXED);
                            if (bad > __atomic_load_n(&s_rrWorst, __ATOMIC_RELAXED))
                                __atomic_store_n(&s_rrWorst, bad, __ATOMIC_RELAXED);
                            *rrShout = 1;
                        }
                    }
                    *rrBad = (int)bad;
                  }
                }
                /* THE PALETTE, IN THIS SAME SECTION AND THIS SAME INSTANT. The
                   interleave stays inside the guard because RGBQUAD is
                   B,G,R,reserved: copying the raw bytes out and converting
                   afterwards swaps red and blue (tagpu_pal.c measured that as
                   paldiff 218 instead of 0). The fourth byte is ours, not the
                   engine's -- every consumer wants an opaque RGBA texel. */
                for (i = 0; i < 256; i++) {
                    pal[4*i+0] = q[i].rgbRed; pal[4*i+1] = q[i].rgbGreen;
                    pal[4*i+2] = q[i].rgbBlue; pal[4*i+3] = 255;
                }
                if (!prev->have || memcmp(prev->pal, pal, sizeof pal) != 0) s_palSerial++;
                memcpy(d->pal, pal, sizeof pal);
                if (diff) s_serial++; else s_unchanged++;
                d->w = w; d->h = h;
                d->serial = s_serial;
                d->palSerial = s_palSerial;
                d->stamp = stamp;
                ok = 1;
            }
        }
    }
    LeaveCriticalSection(&g_ddraw.cs);
    return ok;
}

/* THE ONE LINE THAT SAYS WHETHER THE GOLDEN SOURCE IS LIVE, and what it costs
   the game thread. `us` is the whole copy -- the row loop, the comparison
   against the previous snapshot and the palette -- measured rather than argued
   about, because that work runs on the GAME thread and TA is lockstep.

   `asked`/`waited` ARE THE RENDER THREAD'S COUNTERS, read from this one. They
   are aligned dwords written by a single thread, so a stale value is the worst
   a racy read can get -- the standing they have in `tagpu_packet.c`'s
   heartbeat, for the same reason. What they answer: `asked` against `captured`
   is how many render frames ran per engine draw, and `waited` is how often a
   sync found a capture still in flight and kept what it held. `norequest` is
   the other side of it -- in-play draws on which this cost an acquire load and
   a compare. */
static void heartbeat(int w, int h)
{
    char b[320], rr[96];
    unsigned n = __atomic_load_n(&s_rrChecks, __ATOMIC_RELAXED);
    rr[0] = 0;
    /* THE CONTINUOUS ORACLE'S RUNNING TALLY, and only while it is running. This
       is the number the claim rests on, so it belongs where anyone watching the
       log will see it accumulate rather than only at the end. */
    if (n) {
        _snprintf(rr, sizeof rr, " | re-read %u check(s) %u torn, %u byte(s), worst %u",
                  n, __atomic_load_n(&s_rrTorn, __ATOMIC_RELAXED),
                  __atomic_load_n(&s_rrBytes, __ATOMIC_RELAXED),
                  __atomic_load_n(&s_rrWorst, __ATOMIC_RELAXED));
        rr[sizeof rr - 1] = 0;
    }
    _snprintf(b, sizeof b,
              "surf: golden source %dx%d on the GAME thread -- captured=%u unchanged=%u "
              "refused=%u straddle=%u norequest=%u serial=%u/%u | render asked=%u waited=%u | "
              "us avg=%u max=%u%s",
              w, h, s_captures, s_unchanged, s_refused, s_straddle, s_norequest,
              s_serial, s_palSerial,
              __atomic_load_n(&s_asked, __ATOMIC_RELAXED),
              __atomic_load_n(&s_waited, __ATOMIC_RELAXED),
              s_usN ? s_usTotal / s_usN : 0u, s_usMax, rr);
    b[sizeof b - 1] = 0;
    slog(b);
    s_usTotal = 0; s_usN = 0; s_usMax = 0;
}

void tagpu_surf_capture(unsigned stamp)
{
    unsigned req = __atomic_load_n(&s_req, __ATOMIC_ACQUIRE);
    unsigned dropBefore;
    LARGE_INTEGER t0, t1;
    int hold, ok, rrBad, rrShout, dw, dh;
    SNAP* d;

    /* NOTHING HAS BEEN ASKED FOR. This is the gate that bounds what this costs
       the game thread: one acquire load and a compare on every in-play draw
       that the render thread has not yet come back for. */
    if (req == s_ack) { s_norequest++; return; }

    /* `s_hold` cannot move while a request is outstanding -- the render thread
       changes it only when `req == ack` -- so this read needs no more than to
       see the value that was published with the request. */
    hold = __atomic_load_n(&s_hold, __ATOMIC_RELAXED);
    d = &s_snap[1 - hold];

    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    /* THE LEVEL, BEFORE THE COPY (see SNAP.drop). */
    dropBefore = __atomic_load_n(&s_dropReq, __ATOMIC_ACQUIRE);
    QueryPerformanceCounter(&t0);
    ok = take_into(d, &s_snap[hold], stamp, &rrBad, &rrShout);
    QueryPerformanceCounter(&t1);
    if (s_freq.QuadPart && t1.QuadPart >= t0.QuadPart) {
        unsigned us = (unsigned)(((t1.QuadPart - t0.QuadPart) * 1000000LL) / s_freq.QuadPart);
        s_usTotal += us; s_usN++;
        if (us > s_usMax) s_usMax = us;
    }
    d->drop = dropBefore;
    /* AND A COPY THAT STRADDLED A TEARDOWN IS NOT PUBLISHED AT ALL. Stamping
       with the level it started in is already safe -- both readers refuse it
       once they have acted on the drop -- but discarding it outright makes the
       stronger statement true by construction: every snapshot the render thread
       can adopt lies entirely within one level. A refusal is still an answer,
       which is the path this takes. */
    d->have = ok && (__atomic_load_n(&s_dropReq, __ATOMIC_ACQUIRE) == dropBefore);
    if (d->have) s_captures++;
    else if (ok) s_straddle++;         /* copied fine; the level moved under it */
    /* READ BEFORE THE PUBLISH, USED AFTER IT. The moment `s_ack` is stored the
       render thread may adopt `d`, and then it is no longer ours to read -- the
       rule this file states at the top. Both uses below are logging. */
    dw = d->w; dh = d->h;

    /* THE PUBLISH. Everything above -- the bytes, the palette, the serials, the
       geometry and `have` -- becomes visible to the render thread with this
       store and not before it. */
    __atomic_store_n(&s_ack, req, __ATOMIC_RELEASE);

    if (rrShout) {
        char b[208];
        _snprintf(b, sizeof b,
                  "surf: re-read check at draw %u: %d byte(s) of %d differ between two "
                  "reads of the primary at this site (0 = nothing else is writing it)",
                  stamp, rrBad, dw * dh);
        b[sizeof b - 1] = 0;
        slog(b);
    }
    if (ok && !s_saidFirst) {
        char b[192];
        s_saidFirst = 1;
        _snprintf(b, sizeof b,
                  "surf: the golden source is captured on the GAME thread, post-flip, "
                  "at in-play draw %u -- %dx%d (the shell has none; tacli shot reads it there)",
                  stamp, d->w, d->h);
        b[sizeof b - 1] = 0;
        slog(b);
    }
    if (s_refused && !s_saidRefused) {
        char b[176];
        s_saidRefused = 1;
        /* WHAT A REFUSAL ACTUALLY DOES: the consumer declines to adopt and goes
           on serving the snapshot it already holds, so the reference FREEZES at
           the last good frame rather than going absent. `stamp` is what says
           how old it is. */
        _snprintf(b, sizeof b, "surf: TA's primary is outside what this module carries "
                  "(max %d) - the golden source stops advancing and holds its last "
                  "good frame; `stamp` says which draw that was", TAGPU_SURF_MAXDIM);
        b[sizeof b - 1] = 0;
        slog(b);
    }
    /* GATED ON `ok`, not merely on the counter. `s_captures` only moves on a
       successful capture, so testing the modulus on every ATTEMPT would
       re-print the line -- and zero the timing accumulators -- once per in-play
       draw for as long as captures were being refused, naming a geometry
       `take_into` never set. */
    if (d->have && s_captures % SURF_LOG_EVERY == 0) heartbeat(dw, dh);
}

void tagpu_surf_level_end(void)
{
    /* THE REFERENCE MUST NOT OUTLIVE ITS LEVEL. A snapshot of the last frame of
       one game, held through a shell and diffed against the next one, is a
       wrong answer that looks like a right one. The counter is all this side
       does: the render thread acts on it at its next sync, which keeps `have`
       to one writer. */
    __atomic_fetch_add(&s_dropReq, 1u, __ATOMIC_RELEASE);
}

/* ---- the render thread -------------------------------------------------- */

/* THE GOLDEN SOURCE AS A PICTURE. It is an 8-bit index image with a palette,
   and nothing in the tree samples it -- so without this there is no way to look
   at the thing this whole module exists to keep. P6 PPM, resolved through the
   snapshot's OWN palette (the one captured in the same instant as the indices,
   which is the point of carrying it here), written from the render thread out
   of the buffer it holds. One shot, under a self-deleting lever, so nothing
   about the steady state changes. */
static void dump_ppm(const SNAP* s)
{
    FILE* f = fopen("tagpu_surf.ppm", "wb");
    unsigned char* row;
    int y, x;
    if (!f) return;
    row = (unsigned char*)malloc((size_t)s->w * 3);
    if (!row) { fclose(f); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        const unsigned char* sr = s->bytes + (size_t)y * s->w;
        for (x = 0; x < s->w; x++) {
            const unsigned char* e = s->pal + 4 * (unsigned)sr[x];
            row[3*x+0] = e[0]; row[3*x+1] = e[1]; row[3*x+2] = e[2];
        }
        fwrite(row, 1, (size_t)s->w * 3, f);
    }
    free(row);
    fclose(f);
    {
        char b[192];
        _snprintf(b, sizeof b,
                  "surf: golden source written to tagpu_surf.ppm -- %dx%d, serial=%u "
                  "pal=%u, from in-play draw %u",
                  s->w, s->h, s->serial, s->palSerial, s->stamp);
        b[sizeof b - 1] = 0;
        slog(b);
    }
}

void tagpu_surf_sync(const TAGPU_FRAME* f)
{
    unsigned ack, drop;

    /* THE ORACLE, POLLED HERE because this is the render thread's one call a
       frame -- 60 file stats a second, not the flip's 5 000 (the lesson
       `tagpu_gui_hook.c`'s trigger family records).

       `tagpu_surfdump.on` is the ONE SHOT, self-deleting, and arms both halves:
       the game thread's re-read check on its next capture, and this thread's
       dump of whatever that capture publishes -- one picture and one answer,
       which is what you want when you are looking at the reference.

       `tagpu_surfcheck.on` is the CONTINUOUS probe and is NOT deleted: it runs
       the re-read on every capture for as long as the file is there and says
       nothing unless a check is non-zero. Taking the file away prints the
       tally. It exists because a one-shot cannot measure a rare race -- see
       `s_reread`'s own comment. */
    if (GetFileAttributesA("tagpu_surfdump.on") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("tagpu_surfdump.on");
        __atomic_store_n(&s_reread, RR_ONCE, __ATOMIC_RELAXED);
        __atomic_store_n(&s_dumpArm, 1u, __ATOMIC_RELAXED);
    } else if (GetFileAttributesA("tagpu_surfcheck.on") != INVALID_FILE_ATTRIBUTES) {
        if (__atomic_load_n(&s_reread, __ATOMIC_RELAXED) != RR_EVERY) {
            __atomic_store_n(&s_reread, RR_EVERY, __ATOMIC_RELAXED);
            slog("surf: the continuous re-read check is ARMED (tagpu_surfcheck.on) -- "
                 "every capture reads the primary twice; silence means 0 torn. "
                 "Remove the file for the tally.");
        }
    } else if (__atomic_load_n(&s_reread, __ATOMIC_RELAXED) == RR_EVERY) {
        char b[224];
        unsigned n = __atomic_load_n(&s_rrChecks, __ATOMIC_RELAXED);
        __atomic_store_n(&s_reread, RR_OFF, __ATOMIC_RELAXED);
        _snprintf(b, sizeof b,
                  "surf: the continuous re-read check is DISARMED -- %u check(s), "
                  "%u torn, %u byte(s) in all, worst %u",
                  n, __atomic_load_n(&s_rrTorn, __ATOMIC_RELAXED),
                  __atomic_load_n(&s_rrBytes, __ATOMIC_RELAXED),
                  __atomic_load_n(&s_rrWorst, __ATOMIC_RELAXED));
        b[sizeof b - 1] = 0;
        slog(b);
    }

    /* WHERE IT GOES IS THIS FRAME'S, and it is OURS rather than the engine's:
       the letterboxed viewport moves with a mode change and belongs to the
       window, not to the picture the engine rasterised. */
    if (f) { s_dx = f->vp_x; s_dy = f->vp_y; s_dw = f->vp_w; s_dh = f->vp_h; }

    /* THIS BRANCH IS ONLY THE LOG, AND THAT IS THE POINT. It cannot be where
       the drop takes effect, because it is unreachable on exactly the frames
       that need it: `tagpu_overlay_draw` returns above this line under
       `tagpu_overlay.off` and, the one that matters, while
       `tagpu_reclaim_teardown_active()` -- which is a LEVEL TEARDOWN, the very
       event the drop exists for. The consumer does not share those returns:
       `tagpu_vk_surf_prepare` is called from `tagpu_vk.c`'s frame record and
       runs regardless. So the test lives on the snapshot and is made in
       `tagpu_surf_frame`, where every reader passes; this is the line that
       SAYS so, because a reference that stops existing and one that goes stale
       look the same from outside. */
    drop = __atomic_load_n(&s_dropReq, __ATOMIC_ACQUIRE);
    if (drop != s_dropSeen) {
        /* GATED ONLY ON HOLDING SOMETHING. Gating it also on the held snapshot
           carrying the generation we are leaving would be silently false
           whenever we hold a frame from an older one -- and this line is the
           only thing that makes a drop observable from outside. */
        if (s_holdValid && s_snap[s_hold].have)
            slog("surf: the level ended - the golden source is dropped; "
                 "there is none again until the next in-play draw");
        s_dropSeen = drop;
    }

    ack = __atomic_load_n(&s_ack, __ATOMIC_ACQUIRE);
    if (ack != s_req) {
        /* STILL IN FLIGHT. We keep what we hold and ask for nothing: issuing a
           second request would let the game thread write the buffer we are
           reading, which is the one thing the rule forbids. The render thread
           runs faster than the engine's in-play draw, so this is the common
           case and not a fault. */
        __atomic_fetch_add(&s_waited, 1u, __ATOMIC_RELAXED);
        return;
    }

    /* The answer landed in the buffer we do not hold. Take it if it carried a
       frame OF THIS LEVEL; keep nothing if it did not (a refusal is still an
       answer).

       THE LEVEL TEST IS NOT THE `s_holdValid` FLAG ABOVE, AND MUST NOT BE.
       Clearing the flag for a drop and then falling through to here would
       re-adopt the dead level's final frame in the same call -- and that is the
       COMMON path, not a corner: the teardown usually lands with no capture in
       flight, so `ack == s_req` and this line is reached, and
       `tagpu_surf_frame` would serve the previous level's picture through the
       shell and into the next level, one line after the log said it had been
       dropped. So the level is a property of the SNAPSHOT rather than of the
       consumer's flag: a capture carries the `s_dropReq` it was taken under,
       and only a capture taken under the drop we have acted on may be adopted.
       Exact in both directions -- it also does not throw away a good capture
       from the NEW level, which a request-number barrier would. */
    if (s_snap[1 - s_hold].have && s_snap[1 - s_hold].drop == s_dropSeen) {
        /* `__atomic` on both, because this file states that rule about these
           very words and must not then except its own code from it. Neither is
           a race today -- the req/ack protocol makes `s_hold` stable whenever
           the game thread reads it, and `s_dumpArm` is written and read on this
           thread -- but a rule the code does not follow is how the next edit
           gets it wrong. */
        __atomic_store_n(&s_hold, 1 - s_hold, __ATOMIC_RELAXED);
        s_holdValid = 1;
        if (__atomic_load_n(&s_dumpArm, __ATOMIC_RELAXED)) {
            __atomic_store_n(&s_dumpArm, 0u, __ATOMIC_RELAXED);
            dump_ppm(&s_snap[s_hold]);
        }
    }

    /* ASK FOR THE NEXT, with `s_hold` already settled: the RELEASE store is what
       makes the new value of `s_hold` visible to the game thread before it can
       see the request that names it. */
    __atomic_store_n(&s_req, s_req + 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_asked, 1u, __ATOMIC_RELAXED);
}

int tagpu_surf_frame(TAGPU_SURFFRAME* out)
{
    const SNAP* s = &s_snap[s_hold];
    if (!out || !s_holdValid || !s->have) return 0;
    /* THE LEVEL TEST, HERE AND NOT IN THE PRODUCER. The
       snapshot carries the level it was taken under, so this compares two
       facts rather than consulting a flag somebody had to remember to clear --
       and it is on the path EVERY reader takes, which the producer's half is
       not (see `tagpu_surf_sync`). Reading `s_dropReq` rather than `s_dropSeen`
       is what makes it independent of whether the producer ran at all: a
       teardown that suppresses `tagpu_overlay_draw` still ends the reference on
       the very next frame. Both READERS of `s_dropReq` on this side -- this one
       and the adopt in `tagpu_surf_sync` -- are the render thread (this
       function has exactly one caller, `tagpu_vk_surf_prepare`), so the load
       needs no more than ACQUIRE against the game thread's increment. */
    if (s->drop != __atomic_load_n(&s_dropReq, __ATOMIC_ACQUIRE)) return 0;
    if (s_dw < 1 || s_dh < 1) return 0;     /* nowhere to put it */
    out->bytes = s->bytes;
    out->w = s->w; out->h = s->h;
    out->pal = s->pal;
    out->serial = s->serial;
    out->palSerial = s->palSerial;
    out->stamp = s->stamp;
    out->dx = s_dx; out->dy = s_dy; out->dw = s_dw; out->dh = s_dh;
    return 1;
}
