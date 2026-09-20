/* TA's own frame, snapshotted on the GAME thread for the render thread to diff
   against. The header carries the argument -- the ordering, the two-buffer
   ownership rule and what the in-play-only hook does not cover; this is the
   mechanism.
   [The vulkan-only plan, landing 4c-1; THE CLEAN CUT; moved to the game thread
   2026-09-20.] */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"                     /* g_ddraw.primary / g_ddraw.cs / g_ddraw.bpp */
#include "IDirectDrawSurface.h"     /* ->surface, ->pitch, ->width, ->height      */
#include "IDirectDrawPalette.h"     /* ->palette->data_rgb                        */
#include "tagpu_surf.h"

static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* ONE SNAPSHOT. Two of these exist and the ownership rule in the header says
   which thread may touch which: the game thread writes `s_snap[1 - s_hold]`,
   the render thread reads `s_snap[s_hold]`, and `have` therefore has exactly
   one writer (the game thread, into the buffer it owns) -- which is why the
   level-end drop is a render-thread flag and not a store into here. */
typedef struct {
    unsigned char* bytes;
    unsigned       cap;                 /* what `bytes` will hold, in bytes   */
    int            w, h;
    unsigned char  pal[1024];           /* R,G,B,255                          */
    unsigned       serial, palSerial;
    unsigned       stamp;
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
static int      s_holdValid;            /* what we hold belongs to this level */
static unsigned s_dropSeen;             /* the level-end counter we have acted on */
static int      s_dx, s_dy, s_dw, s_dh; /* this frame's letterboxed rect      */
static unsigned s_asked, s_waited;

/* Game-thread only, except where the counters are read by the log line the
   game thread itself prints. */
static unsigned s_serial = 1;           /* monotonic across both buffers      */
static unsigned s_palSerial = 1;
static unsigned s_dropReq;              /* game -> render: the level ended    */
static unsigned s_captures, s_unchanged, s_refused, s_norequest;
/* THE ONE-SHOT ORACLE (`tagpu_surfdump.on`, self-deleting), armed on the render
   thread and read on both. `s_reread` is the direct test of THIS landing's
   invariant and `s_dumpArm` is how anyone looks at the golden source at all --
   it has no consumer in the tree, so without this there is no way to see it. */
static volatile int s_reread;           /* game: read the primary twice, compare */
static volatile int s_dumpArm;          /* render: write the next snapshot out    */
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
static int take_into(SNAP* d, const SNAP* prev, unsigned stamp)
{
    int ok = 0;

    EnterCriticalSection(&g_ddraw.cs);
    /* THE BOUND COMES FROM THE OBJECT BEING READ. `g_ddraw.width/height` is the
       device MODE; the bytes and the pitch belong to the PRIMARY, which carries
       its own geometry, and between a mode change and the primary being
       recreated the two disagree -- so `h * pitch` off the mode can run past the
       primary's allocation. CLAUDE.md: a value is DATA until it has been
       validated as data. */
    if (g_ddraw.primary && g_ddraw.primary->surface && g_ddraw.primary->palette &&
        g_ddraw.bpp == 8 &&
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
                   (`tagpu_surfdump.on`). This landing's whole claim is that at
                   this point nothing is writing these bytes -- the thread that
                   writes them is this one, and it is here. So a SECOND read of
                   the same rows must agree with the first, byte for byte,
                   however much the game is moving. It is read against what we
                   just copied, so it costs no buffer; a non-zero answer means
                   the claim is false and the site is wrong. At the old site --
                   the render thread, mid-frame -- this is exactly the number
                   that could not be bounded. */
                if (s_reread) {
                    unsigned bad = 0;
                    int yy;
                    for (yy = 0; yy < h; yy++) {
                        const unsigned char* sr2 = src + (size_t)yy * pitch;
                        const unsigned char* dr2 = d->bytes + (size_t)yy * w;
                        int x;
                        for (x = 0; x < w; x++) if (dr2[x] != sr2[x]) bad++;
                    }
                    {
                        char lb[192];
                        s_reread = 0;
                        _snprintf(lb, sizeof lb,
                                  "surf: re-read check at draw %u: %u byte(s) of %u differ "
                                  "between two reads of the primary at this site (0 = nothing "
                                  "else is writing it)", stamp, bad, need);
                        lb[sizeof lb - 1] = 0;
                        slog(lb);
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
   the thread it was moved onto. `us` is the whole copy -- the row loop, the
   comparison against the previous snapshot and the palette -- measured rather
   than argued about, because this landing put that work on the GAME thread and
   TA is lockstep.

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
    char b[256];
    _snprintf(b, sizeof b,
              "surf: golden source %dx%d on the GAME thread -- captured=%u unchanged=%u "
              "refused=%u norequest=%u serial=%u/%u | render asked=%u waited=%u | "
              "us avg=%u max=%u",
              w, h, s_captures, s_unchanged, s_refused, s_norequest,
              s_serial, s_palSerial,
              __atomic_load_n(&s_asked, __ATOMIC_RELAXED),
              __atomic_load_n(&s_waited, __ATOMIC_RELAXED),
              s_usN ? s_usTotal / s_usN : 0u, s_usMax);
    b[sizeof b - 1] = 0;
    slog(b);
    s_usTotal = 0; s_usN = 0; s_usMax = 0;
}

void tagpu_surf_capture(unsigned stamp)
{
    unsigned req = __atomic_load_n(&s_req, __ATOMIC_ACQUIRE);
    LARGE_INTEGER t0, t1;
    int hold, ok;
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
    QueryPerformanceCounter(&t0);
    ok = take_into(d, &s_snap[hold], stamp);
    QueryPerformanceCounter(&t1);
    if (s_freq.QuadPart && t1.QuadPart >= t0.QuadPart) {
        unsigned us = (unsigned)(((t1.QuadPart - t0.QuadPart) * 1000000LL) / s_freq.QuadPart);
        s_usTotal += us; s_usN++;
        if (us > s_usMax) s_usMax = us;
    }
    d->have = ok;
    if (ok) s_captures++;

    /* THE PUBLISH. Everything above -- the bytes, the palette, the serials, the
       geometry and `have` -- becomes visible to the render thread with this
       store and not before it. */
    __atomic_store_n(&s_ack, req, __ATOMIC_RELEASE);

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
        _snprintf(b, sizeof b, "surf: TA's primary is outside what this module carries "
                  "(max %d) - there is no golden source this level", TAGPU_SURF_MAXDIM);
        b[sizeof b - 1] = 0;
        slog(b);
    }
    if (s_captures && s_captures % SURF_LOG_EVERY == 0) heartbeat(d->w, d->h);
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

    /* THE ONE-SHOT ORACLE. Polled here because this is the render thread's one
       call a frame -- 60 file stats a second, not the flip's 5 000 (the lesson
       `tagpu_gui_hook.c`'s trigger family records). It arms BOTH halves: the
       game thread's re-read check on its next capture, and this thread's dump
       of whatever that capture publishes. */
    if (GetFileAttributesA("tagpu_surfdump.on") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("tagpu_surfdump.on");
        s_reread = 1;
        s_dumpArm = 1;
    }

    /* WHERE IT GOES IS THIS FRAME'S, and it is OURS rather than the engine's:
       the letterboxed viewport moves with a mode change and belongs to the
       window, not to the picture the engine rasterised. */
    if (f) { s_dx = f->vp_x; s_dy = f->vp_y; s_dw = f->vp_w; s_dh = f->vp_h; }

    drop = __atomic_load_n(&s_dropReq, __ATOMIC_ACQUIRE);
    if (drop != s_dropSeen) {
        s_dropSeen = drop;
        /* SAID OUT LOUD, because a reference that quietly stops existing and one
           that quietly goes stale look the same from outside. */
        if (s_holdValid) slog("surf: the level ended - the golden source is dropped; "
                              "there is none again until the next in-play draw");
        s_holdValid = 0;
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
       frame; keep the previous one if it did not (a refusal is still an
       answer). */
    if (s_snap[1 - s_hold].have) {
        s_hold = 1 - s_hold; s_holdValid = 1;
        if (s_dumpArm) { s_dumpArm = 0; dump_ppm(&s_snap[s_hold]); }
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
