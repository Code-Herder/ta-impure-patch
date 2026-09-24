/* tagpu_gaf.c — GAF frame decoding and the shared shelf atlas.

   The effects, particle and feature passes share one decoder and one atlas
   implementation, each with its own storage and lifetime.

   RLE rows (TA's own): a u16 byte length, then codes — `b&1` skips `b>>1`
   texels, `b&2` repeats the next byte `(b>>2)+1` times, otherwise `(b>>2)+1`
   literals follow. Uncompressed frames are w*h top-down bytes. Either way a
   texel IS a palette index: the atlas is VK_FORMAT_R8_UNORM sampled
   VK_FILTER_NEAREST, so the shader gets the index back exactly and does its
   own colour-key discard and palette lookup. Read-only over the engine. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_log.h"
/* `tagpu_restoreglsl.h` below declares `TAGPU_RGLSL_FRAME` and
   `tagpu_rglsl_tileable`, both of which this file uses. */
#include "tagpu_gaf.h"
#include "tagpu_pal.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void glog(const char* s)
{
    tagpu_log(s);
}

/* one scratch plane for every atlas: decoding happens only inside
   tagpu_gaf_atlas_get, on the render thread, and the bytes are consumed by
   the paint before the call returns */
static unsigned char s_dec[TAGPU_GAF_DECMAX * TAGPU_GAF_DECMAX];
/* the frame re-emitted with its replicated border (up to PADMAX texels) on
   all four sides */
static unsigned char s_pad[(TAGPU_GAF_DECMAX + 2 * TAGPU_GAF_PADMAX) * (TAGPU_GAF_DECMAX + 2 * TAGPU_GAF_PADMAX)];

/* the cell's edge rounded up to the atlas's alignment */
static int cell_up(const TAGPU_GAFATLAS* a, int v)
{
    return (v + a->align - 1) / a->align * a->align;
}

/* ---- the published restore list (tagpu_gaf.h `rlist`) ------------------ */

/* The frame the restorer is asked for, from the entry that was just painted:
   the atlas is the source -- its RGBA base for a world pass, the R8 itself for
   the UI (tagpu_vk_restore.h) -- the twin the destination, same rect, the border
   -- and the cell's alignment slack past it -- painted as a copy of the edge,
   as atlas_paint painted them. 0 when this frame is below the model's floor
   and is never restored at all.
   ONE BUILDER ON PURPOSE: the whole claim of the published list is that its
   frames are exactly the frames this atlas painted, so they are built here
   rather than by a second piece of code that agrees today. */
static int restore_frame_of(const TAGPU_GAFATLAS* a, const TAGPU_GAFENT* e,
                            TAGPU_RGLSL_FRAME* f)
{
    if (a->restoreMinEdge > 0 && (e->w < a->restoreMinEdge || e->h < a->restoreMinEdge))
        return 0;
    f->ax = f->dx = e->x; f->ay = f->dy = e->y;
    f->w = e->w; f->h = e->h; f->wrap = e->wrap; f->border = a->pad; f->key = e->ck;
    f->padR = cell_up(a, e->w + 2 * a->pad) - (e->w + 2 * a->pad);
    f->padB = cell_up(a, e->h + 2 * a->pad) - (e->h + 2 * a->pad);
    return 1;
}

/* THE BOUND (tagpu_gaf.h): four times the entry ceiling. One append per paint,
   and an entry is painted once unless a repack reserves it again -- so this is
   room for the atlas to fill and be re-laid three times before the list has to
   restart, and it is a function of the atlas rather than a number. */
static int rlist_cap(const TAGPU_GAFATLAS* a)
{
    int c = a->max > 0 ? a->max * 4 : 1024;
    return c < 1024 ? 1024 : c;
}

/* ROOM FOR `need` FRAMES -- A PURE BOUNDS TEST, and that is the point of it
   rather than a simplification. The consumer holds `a->rlist`'s address as a
   raw pointer across the rest of the frame, so the address must not move
   while the list is live. The arm allocates `rlist_cap(a)` frames in one go,
   so this can only ever compare, and the address is fixed for the life of the
   atlas. THAT is what makes the feed safe to restore: not a check before the
   write, but an allocation that cannot move under a reader.

   The cost is stated rather than hidden: `rlist_cap` is `max * 4` frames at
   sizeof(TAGPU_RGLSL_FRAME) -- 360 448 bytes for the unit and effects atlases
   (max 2048) and 720 896 for the feature atlas (max 4096).

   0 means the caller has a list it must NOT write to: either the arm never
   ran, or `need` is past the bound. Both are the caller's to handle, and
   neither can leave the list in a half-grown state, because there is no
   growing. */
static int rlist_room(const TAGPU_GAFATLAS* a, int need)
{
    return a->rlist != NULL && need >= 0 && need <= a->rlistCap;
}

/* A new generation: the array stops being a continuation of what a consumer
   holds. `repaint` says the destination keeps what it has (the palette moved)
   rather than being blanked. */
static void rlist_reset(TAGPU_GAFATLAS* a, int repaint)
{
    if (!a->rlistWant) return;
    a->rlistN = 0;
    a->rlistRepaint = repaint;
    a->rlistGen++;
    /* AND THE BLANK IS COUNTED, not just flagged (tagpu_gaf.h `rlistBlanks`):
       a consumer sees only the LATEST generation, so a blanking reset followed
       by a repainting one in the same frame would hand it "keep what you have"
       over a destination it was told to clear. */
    if (!repaint) a->rlistBlanks++;
}

/* ...and re-seeded with every entry the atlas holds right now. This is both
   the arm path and the overflow recovery, which is why it is one function:
   "start from what is actually here" is the only state either can restart
   from, and having the recovery share the arm's code means the rare path is
   the one that has been exercised since the first frame of every session.
   The seed cannot reach the bound: entries are at most `max` and the bound is
   four times that. */
static void rlist_restart(TAGPU_GAFATLAS* a, int repaint)
{
    int i;
    rlist_reset(a, repaint);
    if (!a->rlistWant || !a->rlist) return;
    if (a->n > 0 && !rlist_room(a, a->n)) return;
    for (i = 0; i < a->n; i++) {
        if (!a->ents[i].ok) continue;
        if (a->rlistN >= a->rlistCap) break;      /* cannot happen; not assumed */
        if (restore_frame_of(a, &a->ents[i], &a->rlist[a->rlistN])) a->rlistN++;
    }
}

/* ONE PAINTED ENTRY ONTO THE PUBLISHED LIST. Called from `atlas_paint`,
   which is the only place an entry's texels become correct, so "appended
   here" and "painted" are the same event and cannot drift apart.

   THE ADDRESS CANNOT MOVE UNDER A READER, which is what the bound is for:
   `rlist_room` is a comparison, the arm's `malloc` is the only allocation, and
   there is no `realloc` in this file at all. A consumer holding the pointer
   across the frame is holding a fixed address.

   THE ONE `free` is the atlas's own teardown, `tagpu_gaf_atlas_free_buffers`,
   which clears `rlist`, `rlistWant` and the counts together so no later
   append can reach a stale pointer.

   AND THE RESIDUAL, because it is a caller's property and not this file's.
   That `free` does NOT bound a consumer that captured the pointer before it
   ran. It is unreachable for an armed atlas today only because
   `tagpu_gaf_atlas_free_buffers` has exactly one call site --
   `tagpu_gui_surf.c`, the GUI atlas, which never arms a list (it is not one
   of `tagpu_gaf_atlas_restore_vk`'s three callers). That is an argument about
   the caller, not a property of this file. A second caller on an armed atlas
   re-opens the hazard.

   AND THE APPEND ITSELF CANNOT TEAR ONE. The frame is written at `[rlistN]`
   and only then is `rlistN` incremented, so a consumer that reads a stale
   count sees FEWER frames than exist and never a half-written one. Entries
   below the count are never rewritten by an append -- only `rlist_restart`
   rewrites in place, and the ordering that keeps it away from a reader is
   stated at the publication sites.

   OVERFLOW RESTARTS RATHER THAN DROPS. The bound is `max * 4`, so reaching it
   means the atlas was re-laid three times over without a reset -- a repack
   storm. `rlist_restart` is the designed recovery and says so at its
   definition: "start from what is actually here" is the only state either path
   can restart from, and it cannot itself overflow because the seed is at most
   `max` entries against a bound of four times that. */
static void rlist_add(TAGPU_GAFATLAS* a, const TAGPU_GAFENT* e)
{
    if (!a->rlistWant || !a->rlist) return;
    if (!rlist_room(a, a->rlistN + 1)) {
        char b[176];
        _snprintf(b, sizeof b, "%s: the published restore list reached its %d-frame"
                  " bound - restarting it from the %d entries the atlas holds now",
                  a->tag ? a->tag : "gaf", a->rlistCap, a->n);
        b[sizeof b - 1] = 0;
        glog(b);
        rlist_restart(a, 0);
        /* the seed is at most `a->n` <= `max` against a bound of `max * 4`, so
           there is room; the test is kept because "cannot happen" is not an
           argument for writing past an array. */
        if (!rlist_room(a, a->rlistN + 1)) return;
    }
    if (restore_frame_of(a, e, &a->rlist[a->rlistN])) a->rlistN++;
}

/* EVERY MIRROR WRITE GOES THROUGH HERE: the serial moves and the ring records
   the rect under the new serial, in one place, so the ring can never skip a
   serial -- which is what makes `tagpu_gaf_band_since`'s per-slot serial test
   a proof that it holds every write of an interval, stale slots from an
   earlier mirror included. */
static void mirror_wrote(TAGPU_GAFATLAS* a, int x0, int y0, int x1, int y1)
{
    TAGPU_GAFBAND* b;
    a->mirrorSerial++;
    b = &a->band[a->mirrorSerial % TAGPU_GAF_NBAND];
    b->serial = a->mirrorSerial;
    b->x0 = (unsigned short)x0; b->y0 = (unsigned short)y0;
    b->x1 = (unsigned short)x1; b->y1 = (unsigned short)y1;
}

/* the mirror's bytes: the index plane, and the key plane after it when the
   mirror was allocated with one -- `keym` is the record of that, so a clear
   covers exactly what the allocation holds whatever `keyPlane` says now */
static size_t mirror_bytes(const TAGPU_GAFATLAS* a)
{
    return (size_t)a->dim * a->dim * (a->keym ? 2u : 1u);
}

int tagpu_gaf_band_since(const TAGPU_GAFBAND* ring, unsigned now, unsigned since,
                         int dim, int rows, int rect[4])
{
    unsigned s;
    int x0 = dim, y0 = dim, x1 = 0, y1 = 0;
    rect[0] = rect[1] = rect[2] = rect[3] = 0;
    if (!ring || dim <= 0) return 0;
    if (now == since) return 1;
    /* unsigned, so a `since` AHEAD of `now` (a mirror re-armed from 0) is a
       huge gap and answers "the whole page" too */
    if (now - since > TAGPU_GAF_NBAND) return 0;
    for (s = since + 1; ; s++) {
        const TAGPU_GAFBAND* b = &ring[s % TAGPU_GAF_NBAND];
        if (b->serial != s) return 0;
        if (b->x0 < x0) x0 = b->x0;
        if (b->y0 < y0) y0 = b->y0;
        if (b->x1 > x1) x1 = b->x1;
        if (b->y1 > y1) y1 = b->y1;
        if (s == now) break;
    }
    if (rows > dim) rows = dim;
    if (x1 > dim) x1 = dim;
    if (y1 > rows) y1 = rows;
    if (x1 <= x0 || y1 <= y0) return 1;
    rect[0] = x0; rect[1] = y0; rect[2] = x1; rect[3] = y1;
    return 1;
}

/* the job's destination back to unpainted, and the published list with it */
static void job_clear_dest(TAGPU_GAFATLAS* a)
{
    /* THE PUBLISHED LIST GOES: the rects this atlas hands out have just moved
       (a recycle, a repack), so a consumer's own destination is wrong. The
       generation is what tells it, and dropping the list is what stops it
       painting the old layout over the new one. A CLEAR IS NOT A PAINT, so
       neither the twin's generation nor the shelf can carry one, and a
       consumer keyed on either would miss it. */
    rlist_reset(a, 0);
}

/* ORDER IS THE WHOLE FIX. A shelf packer wastes the difference between the
   cell that opened a shelf and every shorter cell that then sat on it, and
   the feature atlas is fed in map order -- a 320-tall tree, then a row of
   12-tall rocks under it. Fed tallest-first the same 229 frames span 58% of
   a 2048 square instead of 86%, which is the difference between fitting and
   latching `full`. This is a COUNTING SORT over the cell height: the bins
   below cover every height atlas_get and atlas_put admit (h <= DECMAX, plus
   two borders of at most PADMAX, rounded up to at most PADMAX of alignment),
   and s_ordNext threads the entries sharing a bin. No comparator, no qsort
   context, no allocation -- a repack runs on the render thread inside a
   frame. Render thread only, one atlas at a time: only tagpu_gaf_atlas_reset
   reaches it, and it never re-enters. */
#define ORD_MAX   4096          /* >= every atlas's `max` (feat and gui: 4096) */
#define ORD_HBINS (TAGPU_GAF_DECMAX + 4 * TAGPU_GAF_PADMAX + 1)
static int s_ordNext[ORD_MAX];
static int s_ordHead[ORD_HBINS];

static int atlas_repack(TAGPU_GAFATLAS* a);

const unsigned char* tagpu_gaf_frame_sane(const void* g0)
{
    const unsigned char* g = (const unsigned char*)g0;
    int w, h;
    if (!ptr_ok(g) || IsBadReadPtr(g, 0x18)) return NULL;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX) return NULL;
    return g;
}

/* GAF_SequenceIndex2Frame 0x4B7F30: seq+0x28 entry table, stride 8 */
const unsigned char* tagpu_gaf_seq_frame(const char* seq, int idx)
{
    int n;
    const char* tab;
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return NULL;
    n = *(const unsigned short*)(seq + TAGPU_SQ_N);
    if (idx < 0 || idx >= n || n > 4096) return NULL;
    tab = seq + TAGPU_SQ_TAB;
    if (IsBadReadPtr(tab, (SIZE_T)(idx + 1) * 8)) return NULL;
    return tagpu_gaf_frame_sane(*(const void* const*)(tab + idx * 8));
}

int tagpu_gaf_frame_geom(const void* g0, TAGPU_GAFGEOM* out)
{
    const unsigned char* g = tagpu_gaf_frame_sane(g0);
    if (!g || !out) return 0;
    out->w      = *(const unsigned short*)(g + TAGPU_GF_W);
    out->h      = *(const unsigned short*)(g + TAGPU_GF_H);
    out->hotx   = *(const short*)(g + TAGPU_GF_HOTX);
    out->hoty   = *(const short*)(g + TAGPU_GF_HOTY);
    out->subn   = *(const unsigned char*)(g + TAGPU_GF_SUBN);
    out->ck     = *(const unsigned char*)(g + TAGPU_GF_CK);
    out->subalp = *(const unsigned char*)(g + TAGPU_GF_SUBALP);
    return 1;
}

const unsigned char* tagpu_gaf_subframe(const void* g0, int k)
{
    const unsigned char* g = tagpu_gaf_frame_sane(g0);
    const unsigned char* const* arr;
    int sub;
    if (!g) return NULL;
    sub = *(const unsigned char*)(g + TAGPU_GF_SUBN);
    if (k < 0 || k >= sub) return NULL;
    arr = *(const unsigned char* const* const*)(g + TAGPU_GF_PIX);
    if (!ptr_ok(arr) || IsBadReadPtr(arr, (SIZE_T)sub * 4)) return NULL;
    return tagpu_gaf_frame_sane(arr[k]);
}

int tagpu_gaf_seq_nframes(const char* seq)
{
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return 0;
    return *(const unsigned short*)(seq + TAGPU_SQ_N);
}

/* GAFGetCurrentFramePtrAddr 0x4B7EE0: {u16 frame @0; ...; seq* @8} — a pure
   read, it does not advance the frame (the sim tick does) */
const unsigned char* tagpu_gaf_state_frame(const char* st)
{
    const char* seq;
    if (!ptr_ok(st) || IsBadReadPtr(st, 0x0C)) return NULL;
    seq = *(const char* const*)(st + TAGPU_AS_SEQ);
    return tagpu_gaf_seq_frame(seq, *(const unsigned short*)(st + TAGPU_AS_FRAME));
}

static int gaf_decode(const unsigned char* g, int w, int h, unsigned char* out, unsigned char* cov);
int tagpu_gaf_decode(const unsigned char* g, int w, int h, unsigned char* out)
{
    return gaf_decode(g, w, h, out, NULL);
}

int tagpu_gaf_decode_cov(const unsigned char* g, int w, int h, unsigned char* out, unsigned char* cov)
{
    return gaf_decode(g, w, h, out, cov);
}

static int gaf_decode(const unsigned char* g, int w, int h, unsigned char* out, unsigned char* cov)
{
    unsigned char ck, comp;
    const unsigned char* px;
    const unsigned char* p;
    int y;
    /* THE HEADER IS BOUNDED BEFORE IT IS READ: `ck`, `comp` and the pixel
       POINTER all come out of `g`, and only `px` is checked afterwards. Its
       own header promises "0 if unreadable", so the check belongs here rather
       than in each caller. No caller reaches it unsanitised today -- the UI
       publisher is behind `frame_key`, the menu and the order overlay come
       through `tagpu_gaf_seq_frame` -- so this is a BOUND that closes the
       contract, not the lifetime argument, which stays the caller's. */
    if (!tagpu_gaf_frame_sane(g)) return 0;
    ck = g[TAGPU_GF_CK]; comp = g[TAGPU_GF_COMP];
    px = *(const unsigned char* const*)(g + TAGPU_GF_PIX);
    if (!ptr_ok(px)) return 0;
    if (comp == 0) {
        if (IsBadReadPtr(px, (SIZE_T)w * h)) return 0;
        for (y = 0; y < h; y++) memcpy(out + (size_t)y * w, px + (size_t)y * w, (size_t)w);
        if (cov) memset(cov, 1, (size_t)w * h);   /* a raw frame writes every texel;
                                                    the KEY is its caller's test */
        return 1;
    }
    memset(out, ck, (size_t)w * h);
    if (cov) memset(cov, 0, (size_t)w * h);
    p = px;
    for (y = 0; y < h; y++) {
        int rowlen, x = 0;
        const unsigned char* q;
        unsigned char* row = out + (size_t)y * w;
        if (IsBadReadPtr(p, 2)) return 0;
        rowlen = *(const unsigned short*)p; p += 2;
        if (rowlen > 8192 || IsBadReadPtr(p, rowlen)) return 0;
        q = p;
        while (q < p + rowlen && x < w) {
            unsigned char b = *q++;
            if (b & 1) x += b >> 1;
            else if (b & 2) {
                int n = (b >> 2) + 1, i;
                unsigned char v;
                if (q >= p + rowlen) break;
                v = *q++;
                for (i = 0; i < n && x < w; i++) { if (cov) cov[(size_t)y * w + x] = 1; row[x++] = v; }
            } else {
                int n = (b >> 2) + 1, i;
                for (i = 0; i < n && q < p + rowlen; i++) {
                    unsigned char v = *q++;
                    if (x < w) { row[x] = v; if (cov) cov[(size_t)y * w + x] = 1; }
                    x++;
                }
            }
        }
        p += rowlen;
    }
    return 1;
}

/* Throw the atlas away: no entries, no shelves, every UV invalid. Shared by
   the recycle and by tagpu_gaf_atlas_forget so there is one drop, not two. */
static void atlas_drop(TAGPU_GAFATLAS* a, const char* why)
{
    char b[128];
    a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    a->gen++;                   /* every UV in the atlas has just moved */
    memset(a->hash, 0, sizeof a->hash);
    /* the twin's rects are about to be re-used by other frames: back to
       unpainted, and whatever was queued is dropped (it re-queues on its miss) */
    job_clear_dest(a);
    _snprintf(b, sizeof b, "%s: atlas reset (%s) — frames re-decode on demand, generation %u",
              a->tag ? a->tag : "gaf", why, a->gen);
    glog(b);
}

void tagpu_gaf_atlas_reset(TAGPU_GAFATLAS* a)
{
    int wasFull = a->full;
    /* Re-lay what is here instead of throwing it away. Only for an
       atlas that asked (`repack`), only when it actually filled -- a restart
       that is not "full" is the UI atlas's PK_RESET, which wants the entries
       gone -- and only while a re-lay can still gain
       something. atlas_repack owns the log line and leaves the atlas usable. */
    if (wasFull && a->repack) {
        if (a->repackWall) return;      /* held: nothing a rebuild can improve */
        if (atlas_repack(a)) return;
    }
    /* the sprite atlases reset when full; the UI atlas also on a PK_RESET
       (tagpu_gui_surf.c twins_reset), which is not "full" */
    atlas_drop(a, wasFull ? "full" : "restart");
}

/* The atlas no longer describes anything the caller wants: drop the entries
   AND the repack decisions taken over them. This is the ONLY way back from
   the wall, and the feature pass owes it one call when the map changes --
   `repack` deliberately pins what it holds, and what it holds is only ever
   right for one map. Without it a session that walled on one map would carry
   that map's frames into the next and refuse every frame of it for the rest
   of the session. It goes straight to the drop: a repack here would re-lay
   the very entries we have just been told are meaningless. */
void tagpu_gaf_atlas_forget(TAGPU_GAFATLAS* a)
{
    a->repackWall = 0;
    atlas_drop(a, "subject replaced");
}

/* The CPU mirror (tagpu_gaf.h). Correct from the instant it exists because
   every entry that has already been painted is put back into the state a
   repack leaves one in -- the rect assigned, nothing uploaded -- so the next
   atlas_get repaints it where it sits, into the mirror. */
int tagpu_gaf_atlas_mirror(TAGPU_GAFATLAS* a)
{
    char b[160];
    int i, again = 0;
    if (!a || a->dim <= 0) return 0;
    if (a->mirror) return 1;
    /* calloc: index 0 in the index plane and 0 -- keyed -- in the key plane,
       so a texel no paint has reached is a hole in the base atlas rather than
       black */
    a->mirror = (unsigned char*)calloc((size_t)a->dim * a->dim, a->keyPlane ? 2u : 1u);
    if (!a->mirror) {
        _snprintf(b, sizeof b, "%s: no memory for a %d KB atlas mirror — the Vulkan"
                  " edition of this pass stays down", a->tag ? a->tag : "gaf",
                  (a->dim * a->dim * (a->keyPlane ? 2 : 1)) >> 10);
        b[sizeof b - 1] = 0;
        glog(b);
        return 0;
    }
    a->keym = a->keyPlane ? a->mirror + (size_t)a->dim * a->dim : NULL;
    for (i = 0; i < a->n; i++)
        if (a->ents[i].ok) { a->ents[i].ok = 0; a->ents[i].resv = 1; again++; }
    /* THE SERIAL CARRIES ON ACROSS A MIRROR'S LIVES, and the new one opens
       with a whole-page write: a consumer still holding a copy of an earlier
       mirror then finds that write in its interval (or a gap the ring cannot
       cover) and re-sends everything. Restarting at 0 would let a stale copy's
       serial match a new one's, and the ring's slots from the earlier life
       would answer for writes they never saw. */
    mirror_wrote(a, 0, 0, a->dim, a->dim);
    _snprintf(b, sizeof b, "%s: atlas mirror armed, %d KB%s — %d painted frame(s)"
              " re-decode on their next use so the mirror holds them too",
              a->tag ? a->tag : "gaf", (int)(mirror_bytes(a) >> 10),
              a->keym ? " with its key plane" : "", again);
    b[sizeof b - 1] = 0;
    glog(b);
    return 1;
}

/* THE RESTORED TWIN IS THE WHOLE MIP CHAIN, not level 0 alone, and the reason
   is measurable. The three functions below are the LAYOUT contract between
   the producer's chain and the Vulkan restorer's dump -- `tagpu_vk_restore.c`
   calls `_off` and `_chain`. The restored twin is mipped and sampled
   VK_FILTER_LINEAR with VK_SAMPLER_MIPMAP_MODE_LINEAR to its top level
   (tagpu_vk_unit.c), so a consumer holding only level 0 draws a different
   picture wherever the art is minified. On the unit
   atlas a 32-texel cell lands on a ~23 px sprite at 1024x768 -- LOD around 0.5,
   which is a blend of levels 0 and 1 -- so "wherever it is minified" is
   ordinary play. MEASURED with level 0 alone: 2 126 of 2 132 unit pixels
   differing, worst channel 155, and the figure barely moved when only the
   MAGNIFICATION filter was matched. [gate 3a, 2026-09-16.]

   LAYOUT: level L is `dim >> L` square, RGBA8, at the offset the levels before
   it occupy. Both are computed here so that producer and consumer cannot
   disagree about it. */
size_t tagpu_gaf_mip_bytes(int dim, int level)
{
    int d = dim >> level;
    if (d < 1) d = 1;
    return (size_t)d * (size_t)d * 4;
}

size_t tagpu_gaf_mip_off(int dim, int level)
{
    size_t off = 0;
    int i;
    for (i = 0; i < level; i++) off += tagpu_gaf_mip_bytes(dim, i);
    return off;
}

size_t tagpu_gaf_mip_chain(int dim, int mip)
{
    return tagpu_gaf_mip_off(dim, mip) + tagpu_gaf_mip_bytes(dim, mip);
}

/* Arm the published restore list (tagpu_gaf.h). Polled on the owner's arm
   beat, so the lever is allowed to appear mid-session: a latch that is only
   ever tested at start-up reads as "off" for a session the owner turned it on
   during. [tagpu_terr.c carries the same
   poll for the terrain atlas, whose restore is a fixed list rather than a
   queue and so needs no cursor.] */
int tagpu_gaf_atlas_restore_vk(TAGPU_GAFATLAS* a)
{
    char b[192];
    if (!a || a->dim <= 0 || a->max <= 0 || !a->ents) return 0;
    if (a->rlistWant) return 1;
    if (a->rlistFailed) return 0;
    /* THE KNOB IS `assets=`, NOT A SECOND LEVER. `tagpu_classicpp_assets()`
       is the master arm AND the `assets=` key of `tagpu_classicpp.cfg`, which
       is what the render-options screen's `Undithered assets` row writes, so
       one question decides whether the art is restored and one row moves it.
       Polled, not latched at attach: this function is called on its owner's
       arm beat, so `assets=0 -> 1` arms within that beat. The latch above is a
       LIFETIME (one `malloc`, one address the consumer holds for the frame),
       not the draw switch; `assets=1 -> 0` is answered by the publishers'
       `restored` flag and by the scheduler's `may_draw`, which pause the job
       and put `uRestored` back to 0 without freeing anything. */
    if (!tagpu_classicpp_assets()) return 0;
    /* THE WHOLE BOUND, IN ONE ALLOCATION, ONCE. Growing the list would make
       the feed unsafe: `realloc` moves the buffer, and the consumer holds its
       address as a raw pointer for the rest of the frame. Taking the bound
       up front means the address never moves while the list is live, which is
       a LIFETIME the code enforces rather than an ordering stated three files
       away. `rlist_room` is a comparison from here on.
       Sized by `rlist_cap` -- `max * 4`, so the atlas can fill and be re-laid
       three times before the list has to restart. */
    a->rlistCap = rlist_cap(a);
    a->rlist = (TAGPU_RGLSL_FRAME*)malloc((size_t)a->rlistCap * sizeof *a->rlist);
    if (!a->rlist) {
        a->rlistFailed = 1;
        a->rlistCap = 0;
        _snprintf(b, sizeof b, "%s: no memory for a published restore list - the other"
                  " lane keeps reading this one's restored texels back",
                  a->tag ? a->tag : "gaf");
        b[sizeof b - 1] = 0;
        glog(b);
        return 0;
    }
    a->rlistWant = 1;
    /* SEEDED WITH WHAT IS HERE NOW, which is what makes it correct from the
       instant it exists: whatever this atlas already holds, a consumer
       starting at index 0 restores the same rectangles for itself. */
    rlist_restart(a, 0);
    _snprintf(b, sizeof b, "%s: restorevk -- the restore is the other lane's to run, so"
              " no read-back and the frame list is published instead (%d entries seeded,"
              " %d-frame bound)", a->tag ? a->tag : "gaf", a->rlistN, rlist_cap(a));
    b[sizeof b - 1] = 0;
    glog(b);
    return 1;
}

/* tagpu_gaf.h. `rlist_restart(a, 1)` and nothing else: the frames are the same
   rectangles, and the only thing that changed is the palette the restorer
   reads them through. */
void tagpu_gaf_atlas_restore_repalette(TAGPU_GAFATLAS* a)
{
    if (!a || !a->rlistWant || !a->rlist) return;
    rlist_restart(a, 1);
}

/* GIVE BACK EVERY HEAP BUFFER AN ATLAS OWNS, for a caller that is about to lay
   the struct out again from zero. That is two: `mirror` (dim*dim) and `rlist`
   (the published restore list).

   `rlist` IS FREED HERE because the one caller, `tagpu_gui_surf.c`'s
   `atlas_setup`, memsets the struct afterwards: the day the UI is given a
   list, that memset would otherwise drop a live pointer on every re-arm.

   THE BOUND IS WHAT MAKES THIS WHOLE. `rlist` has exactly ONE writer -- the
   arm's single `malloc` -- and no `realloc` and no other `free` anywhere in
   this file, so "give it back and clear the state that described it" is a
   complete statement rather than one end of a lifetime nobody owns. The
   latches go with the buffer: an atlas with `rlistWant` set and `rlist` NULL
   is precisely the half-state to avoid.

   `mirror` is not freed by `_lost`, which keeps it
   deliberately so that a mirror is never stale for the frames between a
   `_lost` and the next create. A caller that re-arms by zeroing the
   struct would therefore drop the pointer and leak it; every writer already
   guards on the pointer and `_mirror` re-arms on demand, so handing it back
   here costs nothing the memset was not already costing functionally. */
void tagpu_gaf_atlas_free_buffers(TAGPU_GAFATLAS* a)
{
    if (!a) return;
    free(a->mirror);    a->mirror = NULL; a->keym = NULL;
    /* AND THE STATE THAT DESCRIBED IT, not just the pointer: `rlistWant` is
       what every writer tests before touching the list, so leaving it set over
       a NULL buffer is the half-state this function exists to avoid. The
       caller memsets after us today, which would cover it -- that is exactly
       why it must not be relied on here. */
    free(a->rlist);     a->rlist = NULL;
    a->rlistN = a->rlistCap = 0;
    a->rlistWant = 0; a->rlistFailed = 0;
}

void tagpu_gaf_atlas_lost(TAGPU_GAFATLAS* a)
{
    /* `made` goes: the layout describes nothing any more, so the next create
       must lay it out again. */
    a->made = 0;
    a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    a->gen++;                   /* every UV in the atlas has just moved */
    /* and so is everything it held, so the mirror of it says nothing. (The
       re-create zeroes it again; doing it here as well means a mirror is never
       stale for the frames between a loss and the next create.) */
    if (a->mirror) { memset(a->mirror, 0, mirror_bytes(a)); mirror_wrote(a, 0, 0, a->dim, a->dim); }
    memset(a->hash, 0, sizeof a->hash);
    /* THE CONSUMER'S DESTINATION DID NOT DIE -- ITS SOURCE DID. Nothing here
       is a Vulkan object, so a consumer's twin still holds the colours of an
       atlas whose every entry has just been dropped. The generation is what
       tells it to blank and start over; without this it would keep painting
       the old layout's rects for the rest of the session. */
    rlist_reset(a, 0);
    /* the entries went, so the wall the last fill hit says nothing about the
       next one */
    a->repackWall = 0;
}

/* frame headers are heap pointers: mix the high bits down so the low-order
   allocator alignment does not cluster every key into one bucket */
static unsigned gaf_hash(const void* p)
{
    unsigned h = (unsigned)(size_t)p;
    h ^= h >> 15; h *= 0x2545F491u; h ^= h >> 13;
    return h & (TAGPU_GAF_HASH - 1);
}

int tagpu_gaf_atlas_create(TAGPU_GAFATLAS* a)
{
    if (a->made) return 1;
    if (a->dim <= 0 || a->max <= 0 || !a->ents) return 0;
    /* THERE IS NO TEXTURE HERE; THE LAYOUT IS THE ATLAS. The shelf packer, the
       entry table and the CPU mirror below are what an atlas is in this build,
       and the consuming Vulkan pass uploads the mirror. `a->made` is what says
       the layout exists. */
    /* A FRESH ATLAS IS A FRESH MIRROR, AND THE MEMSET IS WHAT MAKES IT INDEX 0
       -- the assumption the border comment in `atlas_paint` rests on. A mirror
       that kept the previous atlas's texels would be a copy of something that
       no longer exists. atlas_drop is deliberately NOT here: it leaves the
       texels alone and lets re-inserted entries overwrite them, and the mirror
       follows exactly because it follows the paints. */
    if (a->mirror) { memset(a->mirror, 0, mirror_bytes(a)); mirror_wrote(a, 0, 0, a->dim, a->dim); }
    a->made = 1;
    a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    memset(a->hash, 0, sizeof a->hash);
    /* the layout parameters: unset is the sprite atlases' one-texel border */
    if (a->pad < 1) a->pad = 1;
    if (a->pad > TAGPU_GAF_PADMAX) a->pad = TAGPU_GAF_PADMAX;
    if (a->align < 1) a->align = 1;
    if (a->mip < 0) a->mip = 0;
    return 1;
}

/* THE REPACK. Re-lay every entry the atlas holds, tallest cell first, and
   leave each one RESERVED: the rect is assigned, nothing is uploaded, and
   the next atlas_get for that frame paints it in place (atlas_insert's probe
   below). The texels are not moved -- an entry records the frame's address
   and its size, never the decoded bytes -- so the pixels come back the way
   they arrived the first time, by RLE decode on demand. That is the same
   work one recycle does, done ONCE when the page fills instead
   of once per frame for as long as it stays full.

   It reads `w` and `h` out of an entry and nothing else: both were bounded to
   1..TAGPU_GAF_DECMAX by atlas_get/atlas_put before the entry existed, so the
   geometry below cannot address outside the atlas whatever the entry's `frame`
   pointer has since become. No pointer stored in an entry is dereferenced
   here. (A frame freed by the engine and re-allocated at the same address
   with the same `pix` and the same size would draw its old art -- that is the
   pre-existing hazard the (frame, pix, w, h) key already carries, and this
   widens the window it lives in: the feature atlas is meant to last as long
   as the map, but nothing tells it when the map changes. features.md 5.)

   Returns 1 when the atlas was re-laid (the caller must not go on to drop
   it), 0 when it was left exactly as it was found. */
static int atlas_repack(TAGPU_GAFATLAS* a)
{
    const int p = a->pad;
    const int before = a->n;
    char b[192];
    int i, hb, x = 0, y = 0, sh = 0, kept = 0, w;
    int wanted = 0, wanted_skip = 0;

    /* `made` (tagpu_gaf_atlas_create): a repack re-lays the ENTRIES and the
       paints that follow it feed the CPU mirror. */
    if (before <= 0 || before > ORD_MAX || !a->made || !a->ents) return 0;

    /* Only what is still being ASKED FOR is re-laid. An entry nothing has
       touched since the last repack is not part of the set the atlas is short
       of room for -- most sharply after a map change, when every one of the
       previous map's frames is dead weight -- so it is dropped here and
       re-decodes on demand if it is ever wanted again. This is what keeps the
       atlas from accumulating across a session, and it is why the wall below
       can be read as "the LIVE set does not fit" rather than "the set we
       happen to be holding does not fit". */
    for (i = 0; i < ORD_HBINS; i++) s_ordHead[i] = -1;
    for (i = 0; i < before; i++) {
        const int ch = cell_up(a, (int)a->ents[i].h + 2 * p);
        if (ch < 1 || ch >= ORD_HBINS) return 0;   /* h was bounded; belt and braces */
        if (!a->ents[i].hit) { wanted_skip++; continue; }
        s_ordNext[i] = s_ordHead[ch];
        s_ordHead[ch] = i;
        wanted++;
    }
    /* Nothing was touched at all -- the caller filled the atlas inside a
       single frame without a lookup landing, which the hit marking makes
       impossible in practice. Re-lay everything rather than evict everything. */
    if (wanted == 0) {
        for (i = 0; i < ORD_HBINS; i++) s_ordHead[i] = -1;
        for (i = 0; i < before; i++) {
            const int ch = cell_up(a, (int)a->ents[i].h + 2 * p);
            s_ordNext[i] = s_ordHead[ch]; s_ordHead[ch] = i;
        }
        wanted = before; wanted_skip = 0;
    }

    /* Pass 1 -- the shelf geometry of atlas_insert, fed tallest bin first.
       Because the tallest cell on a shelf is always the one that opened it,
       `sh` is set once per shelf and never grown under a later cell, which is
       exactly the waste arrival order pays. The new rect goes straight into
       the entry: its old one stops meaning anything either way. */
    for (i = 0; i < before; i++) a->ents[i].resv = 0;   /* the untouched drop out here */
    for (hb = ORD_HBINS - 1; hb >= 1; hb--) {
        for (i = s_ordHead[hb]; i >= 0; i = s_ordNext[i]) {
            TAGPU_GAFENT* e = &a->ents[i];
            const int cw = cell_up(a, (int)e->w + 2 * p);
            if (cw > a->dim || hb > a->dim) continue;
            if (x + cw > a->dim) { y += sh; x = 0; sh = 0; }
            if (y + hb > a->dim) continue;      /* shorter bins may still fit */
            e->x = (unsigned short)(x + p);
            e->y = (unsigned short)(y + p);
            e->resv = 1;
            x += cw;
            if (hb > sh) sh = hb;
            kept++;
        }
    }

    /* An empty atlas that is also `full` could never take another frame, and
       the tallest-first layout has to be at least as good as the arrival
       order that got these entries in, so this cannot happen -- but the
       state it would leave is unrecoverable, so hand the caller back to the
       plain recycle rather than commit it. The rects written above belong to
       entries the recycle is about to make unreachable. */
    if (kept <= 0) return 0;

    /* Pass 2 -- compact the survivors down so `ents` stays dense (`n` is
       where atlas_insert puts the next one) and rebuild the hash over their
       new indices. `w <= i` throughout, so the copy never runs ahead of the
       read. */
    memset(a->hash, 0, sizeof a->hash);
    for (i = 0, w = 0; i < before; i++) {
        TAGPU_GAFENT* src = &a->ents[i];
        TAGPU_GAFENT* dst;
        int slot;
        if (!src->resv) continue;
        dst = &a->ents[w];
        if (dst != src) *dst = *src;
        dst->u0 = (float)dst->x / (float)a->dim;
        dst->v0 = (float)dst->y / (float)a->dim;
        dst->u1 = (float)(dst->x + dst->w) / (float)a->dim;
        dst->v1 = (float)(dst->y + dst->h) / (float)a->dim;
        dst->ok = 0;                    /* reserved until its next get paints it */
        dst->hit = 0;                   /* a fresh interval to prove it is still wanted */
        for (slot = (int)gaf_hash(dst->frame); a->hash[slot];
             slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) { }
        a->hash[slot] = ++w;
    }
    a->n = w;
    a->shelfX = x; a->shelfY = y; a->shelfH = sh;
    a->full = 0;
    a->gen++;                           /* every UV in the atlas has just moved */
    a->repacks++;
    /* the twin's rects moved with them: back to unpainted (a new list
       generation, which the consumer blanks its twin on), and whatever was
       queued is dropped -- it re-queues as each reserved entry is painted.
       Unlike the recycle this happens once, which is what lets the twin
       converge at all while zoomed out. */
    job_clear_dest(a);

    /* The branch that says a second page is the only thing left.
       `wanted` is the set that was still being asked for; if the tallest-first
       layout could not hold all of it, no re-sort will, and repacking again
       would only re-decode the same frames to reach the same wall. Hold what
       we have rather than drop it for a rebuild that would place fewer.
       Note this compares against `wanted`, never against the previous
       repack's count: evicting the untouched legitimately lowers that count,
       and reading a drop as failure would latch the wall on a healthy atlas
       the first time a map went quiet. */
    if (kept < wanted) {
        a->repackWall = 1;
        a->full = 1;
        _snprintf(b, sizeof b,
                  "%s: atlas repack wall — %d of %d live frames fit one %d square tallest-first"
                  " (%d%% spanned); holding this layout, only a second page adds room",
                  a->tag ? a->tag : "gaf", kept, wanted, a->dim,
                  a->dim ? (y + sh) * 100 / a->dim : 0);
    } else {
        _snprintf(b, sizeof b,
                  "%s: atlas repacked — %d frames re-laid tallest-first (%d dropped unasked-for),"
                  " %d%% of the %d square, generation %u (repack %u)",
                  a->tag ? a->tag : "gaf", kept, wanted_skip,
                  a->dim ? (y + sh) * 100 / a->dim : 0, a->dim, a->gen, a->repacks);
    }
    glog(b);
    return 1;
}

/* Upload one entry's texels into the rect it has already been assigned, and
   finish it: the replicated border, the UVs, the tileability verdict and the
   restore queue. Shared by a first insertion and by the paint of an entry a
   repack reserved -- which is why it takes the rect from the entry rather
   than from the shelf cursor. */
static void atlas_paint(TAGPU_GAFATLAS* a, TAGPU_GAFENT* e, unsigned char ck,
                        const unsigned char* pixels)
{
    const int p = a->pad, w = e->w, h = e->h;
    const int cw = cell_up(a, w + 2 * p), ch = cell_up(a, h + 2 * p);
    const int x = e->x, y = e->y;
    int i;

    /* THE PAINT IS THE ATLAS, AND THE MIRROR IS THE PAINT: the mirror write
       below is the only destination the art has. Nothing here is gated on a
       backend, and nothing may become so: a cell that is packed but not
       mirrored is a hole the Vulkan pass samples as index 0. */
    /* Re-emit the frame with its outermost row and column repeated all
       round, `pad` deep. The border is what any sampler that reaches past
       the frame must land on: under VK_FILTER_NEAREST that is the fragment
       whose centre falls exactly on the quad's far edge (its u interpolates
       to exactly u1, and floor(u1*dim) is one texel past the frame) — left
       unwritten that texel is whatever a fresh mirror holds, which
       tagpu_gaf_atlas_create zeroes, i.e. index 0, a real palette entry (black) rather than the frame's colour
       key, which is the black hairline down the right of every tree at
       zoom 0.25. Under a filtered sampler it is every edge fragment, which
       is why the border is on all four sides and not just two; under a mipmapped one it is the
       whole 4-texel ring (tagpu_gaf.h `pad`). The cell's slack past the
       border, where the alignment rounds up (0..align-1 texels on the
       right and bottom), is filled with the same edge: at level 2 the
       far-edge sample of a frame whose width is 3 mod 4 takes a quarter
       of its weight from the level-2 texel that covers the slack, so
       unwritten slack would darken that column by a sixteenth. The whole
       cell is mirrored, and the frame published for the Vulkan restorer
       covers the same rect, so its OUT pass paints the slack the
       same way. */
    {
        const int pw = cw, pr = cw - p - w, pb = ch - p - h;   /* right/bottom: p + slack */
        int k;
        for (i = 0; i < h; i++) {
            unsigned char* row = s_pad + (size_t)(i + p) * pw + p;
            memcpy(row, pixels + (size_t)i * w, (size_t)w);
            for (k = 1; k <= p; k++) row[-k] = row[0];
            for (k = 0; k < pr; k++) row[w + k] = row[w - 1];
        }
        for (k = 1; k <= p; k++)
            memcpy(s_pad + (size_t)(p - k) * pw, s_pad + (size_t)p * pw, (size_t)pw);
        for (k = 0; k < pb; k++)
            memcpy(s_pad + (size_t)(p + h + k) * pw, s_pad + (size_t)(p + h - 1) * pw, (size_t)pw);
        /* THE CPU MIRROR TAKES THE BYTES THE PADDING LOOPS ABOVE JUST BUILT,
           OUT OF `s_pad` ITSELF. Not a second evaluation of the art: the very
           rows those loops wrote, so there is nothing for a second pass over
           the source to drift from. It is the only copy, and DELETING IT
           DELETES THE SPRITE. The rect is the cell's, border and alignment slack included,
           exactly as above -- and it is inside the atlas by construction,
           because the shelf packer refused the cell otherwise. */
        if (a->mirror) {
            const int x0 = x - p, y0 = y - p;
            for (k = 0; k < ch; k++)
                memcpy(a->mirror + (size_t)(y0 + k) * a->dim + x0,
                       s_pad + (size_t)k * pw, (size_t)cw);
            /* THE KEY PLANE FROM THE SAME ROWS, against the key this entry is
               painted with -- the one every sprite drawn from the cell keys
               on, border and slack included, since those replicate the edge */
            if (a->keym) {
                for (k = 0; k < ch; k++) {
                    const unsigned char* src = s_pad + (size_t)k * pw;
                    unsigned char* km = a->keym + (size_t)(y0 + k) * a->dim + x0;
                    for (i = 0; i < cw; i++) km[i] = src[i] == ck ? 0 : 255;
                }
            }
            mirror_wrote(a, x0, y0, x0 + cw, y0 + ch);
        }
    }

    e->u0 = (float)x / (float)a->dim;         e->v0 = (float)y / (float)a->dim;
    e->u1 = (float)(x + w) / (float)a->dim;   e->v1 = (float)(y + h) / (float)a->dim;
    e->ck = ck;
    /* decided here, while the pixels are still at hand: the tileability the
       restorer wrap-pads by (a key on an edge says no) */
    /* The tileability test asks about the ART, so it reads the engine's own
       table and not the palette the screen is shown with: a gamma-scaled
       palette stretches every colour distance by the same factor and moves
       tiles across the threshold (measured 2026-09-09 on the terrain's 5062:
       177 tiles wrap-padded at factor 1.5, 400 at 1.0). */
    {
        const unsigned char* art = tagpu_pal_engine();
        e->wrap = art ? (char)tagpu_rglsl_tileable(pixels, w, h, art, e->ck) : 0;
    }
    e->ok = 1; e->resv = 0;
    /* AND ONTO THE PUBLISHED LIST, WHICH IS THE VULKAN RESTORER'S ONLY FEED. This
       is the one place an entry's texels become correct, so the append
       belongs here and nowhere else: "painted" and "published" are then the
       same event and cannot drift apart.

       APPENDING DURING A PAINT MAKES `a->rlist` MUTABLE while a consumer holds
       its address as a raw pointer. That is safe by construction rather than
       by a guard, and there are two halves to it because the hazard has two:

         - THE BOUND kills the moving address. The arm allocates `rlist_cap(a)`
           frames in one `malloc`, `rlist_room` is a comparison, and there is no
           `realloc` anywhere in this file, so the address is fixed for as long
           as the atlas holds it. The one `free` is the teardown in
           `tagpu_gaf_atlas_free_buffers`, and `rlist_add`'s own header says
           what it does and does not bound.
         - THE ORDERING keeps `rlist_restart`'s in-place rewrite away from a
           reader. The unit atlas's list is taken in `tagpu_posedraw_handover`,
           which `render_vk.c`'s loop reaches through `tagpu_vk_frame`, after
           `tagpu_overlay_draw` has finished every paint of the frame. Taken at
           the FIRST posedraw window instead, it would be read while
           `tagpu_native.c`'s `ghost_record` can still paint through
           `tagpu_r3d_atlas_uv` -> `atlas_get`.

       AND IT IS AN ORDERING RATHER THAN A RACE, so no lock belongs here. There
       is no second thread: every paint of an armed atlas is render-thread --
       the unit atlas through `tagpu_native.c`'s `emit_node` and
       `tagpu_posebake.c`'s `mat_emit`, both inside `tagpu_native_frame`,
       itself called only from `tagpu_overlay.c`'s `tagpu_overlay_draw` -- and
       so is every consumer. A fence would fix nothing while reading as though
       it had. */
    rlist_add(a, e);
}

/* the insertion shared by atlas_get (which decodes into s_dec first) and
   atlas_put (which is handed the bytes): `pixels` holds w*h indices */
static const TAGPU_GAFENT* atlas_insert(TAGPU_GAFATLAS* a, const void* g, const void* pix,
                                        int w, int h, unsigned win,
                                        unsigned char ck, const unsigned char* pixels)
{
    int slot;
    TAGPU_GAFENT* e;
    for (slot = (int)gaf_hash(g); a->hash[slot]; slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) {
        TAGPU_GAFENT* c = &a->ents[a->hash[slot] - 1];
        if (c->frame == g && c->pix == pix && c->w == w && c->h == h && c->win == win) {
            c->hit = 1;                 /* still wanted: survives the next repack */
            if (c->ok) return c;
            /* a repack reserved this rect and the caller is holding exactly
               the pixels it wants: paint it where it already sits */
            if (c->resv) { atlas_paint(a, c, ck, pixels); return c; }
            return NULL;
        }
    }
    /* every frame carries its OWN border of `pad` texels, so the shelf
       advances by the whole cell (w+2p / h+2p, rounded up to the alignment)
       rather than sharing one gutter between two neighbours */
    {
        const int p = a->pad, cw = cell_up(a, w + 2 * p), ch = cell_up(a, h + 2 * p);
        if (a->full || ch > a->dim) return NULL;
        if (a->n >= a->max) { a->full = 1; return NULL; }
        if (a->shelfX + cw > a->dim) { a->shelfY += a->shelfH; a->shelfX = 0; a->shelfH = 0; }
        if (a->shelfY + ch > a->dim) { a->full = 1; return NULL; }
        e = &a->ents[a->n];
        e->frame = g; e->pix = pix; e->win = win;
        e->w = (unsigned short)w; e->h = (unsigned short)h; e->ok = 0; e->resv = 0;
        e->hit = 1;                     /* asked for by definition: it is being inserted */
        e->x = (unsigned short)(a->shelfX + p);       /* inside the border */
        e->y = (unsigned short)(a->shelfY + p);
        a->hash[slot] = ++a->n;
        a->shelfX += cw;
        if (ch > a->shelfH) a->shelfH = ch;
    }
    atlas_paint(a, e, ck, pixels);
    return e;
}

const TAGPU_GAFENT* tagpu_gaf_atlas_get(TAGPU_GAFATLAS* a, const unsigned char* g)
{
    int w, h;
    const void* pix;
    const TAGPU_GAFENT* hit;
    if (!tagpu_gaf_atlas_create(a)) return NULL;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    /* tagpu_gaf_frame_sane already promises this of every caller's frame; the
       decode into s_dec and the guard-rail copy below both index off it */
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX) return NULL;
    pix = *(const void* const*)(g + TAGPU_GF_PIX);
    hit = tagpu_gaf_atlas_find(a, g, pix, w, h, 0u);   /* atlas_get is always the whole frame */
    /* the fast path is the one that runs hundreds of times a frame, so this is
       where an entry proves it is still part of the working set */
    if (hit) { ((TAGPU_GAFENT*)hit)->hit = 1; return hit; }
    /* a frame whose pixels are momentarily unreadable must stay retryable:
       claiming the slot here would cache the failure for the atlas's whole
       life, and the feature atlas is meant to live as long as the map */
    if (!tagpu_gaf_decode(g, w, h, s_dec)) return NULL;
    return atlas_insert(a, g, pix, w, h, 0u, g[TAGPU_GF_CK], s_dec);
}

const TAGPU_GAFENT* tagpu_gaf_atlas_put(TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                        int w, int h, unsigned win,
                                        unsigned char ck, const unsigned char* pixels)
{
    if (!tagpu_gaf_atlas_create(a)) return NULL;
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX || !pixels) return NULL;
    return atlas_insert(a, frame, pix, w, h, win, ck, pixels);
}

const TAGPU_GAFENT* tagpu_gaf_atlas_find(const TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                         int w, int h, unsigned win)
{
    int slot;
    if (!a->ents || !a->n) return NULL;
    for (slot = (int)gaf_hash(frame); a->hash[slot]; slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) {
        const TAGPU_GAFENT* c = &a->ents[a->hash[slot] - 1];
        if (c->frame == frame && c->pix == pix && c->w == w && c->h == h && c->win == win)
            return c->ok ? c : NULL;
    }
    return NULL;
}
