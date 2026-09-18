/* tagpu_gaf.c — GAF frame decoding and the shared GL shelf atlas.

   Lifted out of tagpu_fx.c when the feature pass (G13a) needed the same two
   pieces; the effects, particle and feature passes now share one decoder and
   one atlas implementation, each with its own storage and lifetime.

   RLE rows (TA's own): a u16 byte length, then codes — `b&1` skips `b>>1`
   texels, `b&2` repeats the next byte `(b>>2)+1` times, otherwise `(b>>2)+1`
   literals follow. Uncompressed frames are w*h top-down bytes. Either way a
   texel IS a palette index: the atlas is GL_R8 sampled NEAREST, so the shader
   gets the index back exactly and does its own colour-key discard and palette
   lookup. Read-only over the engine. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_gaf.h"
#include "tagpu_pal.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_vk.h"      /* tagpu_vk_owns_present: is there a GL lane at all? */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void glog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* GL 3.0's mip generation and GL 1.1's float texture parameter are not in
   opengl_utils.h; fetched once, the way every tagpu module fetches what the
   fork does not export (wglGetProcAddress first, then opengl32 itself) */
typedef void (APIENTRY* PFN_GENERATEMIPMAP)(GLenum);
typedef void (APIENTRY* PFN_TEXPARAMETERF)(GLenum, GLenum, GLfloat);
/* ...and GL 1.0's read-back, which opengl_utils.h does not export either
   (tagpu_abshot.c and tagpu_overlay.c both fetch it the same way) */
typedef void (APIENTRY* PFN_READPIXELS)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
static PFN_GENERATEMIPMAP x_glGenerateMipmap;
static PFN_TEXPARAMETERF  x_glTexParameterf;
static PFN_READPIXELS     x_glReadPixels;
static int s_glFetched;
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
/* the lab's default (tascene-view.html aniso); in the header because a
   second backend has to apply the SAME ratio or draw different art */
#define TWIN_ANISO TAGPU_GAF_TWIN_ANISO

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) p = (void*)GetProcAddress(gl, n);
    }
    return p;
}

static void fetch_gl(void)
{
    if (s_glFetched) return;
    s_glFetched = 1;
    x_glGenerateMipmap = (PFN_GENERATEMIPMAP)getgl("glGenerateMipmap");
    x_glTexParameterf  = (PFN_TEXPARAMETERF)getgl("glTexParameterf");
    x_glReadPixels     = (PFN_READPIXELS)getgl("glReadPixels");
}

/* one scratch plane for every atlas: decoding happens only inside
   tagpu_gaf_atlas_get, on the render thread, and the bytes are consumed by
   the upload before the call returns */
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
   the R8 atlas is the source, the twin the destination, same rect, the border
   -- and the cell's alignment slack past it -- painted as a copy of the edge,
   as the R8 upload painted them. 0 when this frame is below the model's floor
   and is never restored at all.
   SHARED BY THE QUEUE AND THE LIST ON PURPOSE: the whole claim of the list is
   that it is the frames the GL lane was given, so the two must be built by
   one piece of code rather than by two that agree today. */
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

/* ROOM FOR `need` FRAMES, up to the bound and never past it. One function
   because the alternative is the bug this had in its first draft: the seed
   loop and the append each grew (or failed to grow) on their own, so an atlas
   holding more entries than the current allocation lost the tail of its own
   list -- silently, which on the other lane is cells that stay indexed for
   ever with nothing in the log. 0 when the memory was refused OR when `need`
   is past the bound, and the caller then has a list it must not write to. */
static int rlist_room(TAGPU_GAFATLAS* a, int need)
{
    int cap = rlist_cap(a), want;
    TAGPU_RGLSL_FRAME* g;
    if (!a->rlist || need > cap) return 0;
    if (need <= a->rlistCap) return 1;
    want = a->rlistCap > 0 ? a->rlistCap : 256;
    while (want < need) want *= 2;
    if (want > cap) want = cap;
    g = (TAGPU_RGLSL_FRAME*)realloc(a->rlist, (size_t)want * sizeof *g);
    if (!g) {
        char b[176];
        _snprintf(b, sizeof b, "%s: no memory to grow the published restore list to %d"
                  " frames - the other lane's restore stands down",
                  a->tag ? a->tag : "gaf", want);
        b[sizeof b - 1] = 0;
        glog(b);
        /* THE LIST GOES RATHER THAN LOSING A FRAME. A list with a hole in it
           leaves cells indexed on the other lane for ever and nothing says so;
           dropping it hands the consumer a generation change and no request,
           and the consumer then STANDS THE PASS DOWN -- not "goes back to the
           read-back", which it cannot do: the arm latches are one-way and the
           mirror is already freed. The buffer itself is kept by a failed
           `realloc` and is freed here because nothing will ask for it again. */
        free(a->rlist);
        a->rlist = NULL; a->rlistN = 0; a->rlistCap = 0;
        a->rlistWant = 0; a->rlistFailed = 1;
        a->rlistGen++;
        return 0;
    }
    a->rlist = g;
    a->rlistCap = want;
    return 1;
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
       over a destination this lane has cleared. */
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

static void rlist_add(TAGPU_GAFATLAS* a, const TAGPU_RGLSL_FRAME* f)
{
    if (!a->rlistWant || !a->rlist) return;
    /* THE BOUND IS REACHED BY RESTARTING, NOT BY GROWING PAST IT. The list is
       fed for the atlas's life, so any ceiling is a number that can be
       exceeded; what cannot be exceeded is "the entries that are here", and
       that is what the restart leaves behind. Said once per restart, because
       it is a real event a consumer sees as a blank and a repaint. */
    if (a->rlistN >= rlist_cap(a)) {
        char b[192];
        _snprintf(b, sizeof b, "%s: the published restore list reached its %d-frame"
                  " bound - restarting it from the %d entries in the atlas, so the"
                  " other lane blanks its twin and repaints",
                  a->tag ? a->tag : "gaf", rlist_cap(a), a->n);
        b[sizeof b - 1] = 0;
        glog(b);
        rlist_restart(a, 0);
        /* AND THE FRAME THAT TRIGGERED THE RESTART IS ALREADY IN IT: this is
           called from `atlas_paint`, which sets `e->ok` before it enqueues, so
           the re-seed above included it. Appending it again would be harmless
           (the same rect restored twice) but it would also be the one place
           where the published list is not the list the GL lane was given,
           which is the whole claim `restore_frame_of` exists to keep.
           [FROM THE LANDING-7d REVIEW.] */
        return;
    }
    if (!rlist_room(a, a->rlistN + 1)) return;
    a->rlist[a->rlistN++] = *f;
}

/* Rebuild the twin's mip levels 1..mip from level 0 -- after every batch the
   restorer painted, after a recycle cleared level 0 (the restorer clears
   only that level: tagpu_restoreglsl.c clear_dest), and once when the twin
   is made, so it is never sampled incomplete (an incomplete texture reads
   as opaque black, which the shader would take for a restored texel). */
/* THE RESTORED TWIN HAS JUST BEEN ZEROED AND THE MIRROR HAS TO SAY SO.
   `tagpu_rglsl_job_clear` clears the destination atlas to alpha 0 as well as
   dropping the queue, and a CLEAR IS NOT A PAINT: `tagpu_rglsl_job_painted`
   does not move for it, the twin's generation does not move, and the shelf
   gets SMALLER rather than larger -- so not one of the three things
   `tagpu_gaf_atlas_mirror_rgb_step` keys on can see it, and a mirror left
   alone would hold the previous fill's colours over a texture that is now
   empty. The next entry re-laid into those rects then draws restored in one
   lane and indexed in the other until its repaint lands.
   This is the third time on this pass that a key which was not the CONTENT's
   key has been wrong, so the zeroing lives HERE, in one function beside the
   call it mirrors, rather than at each site. */
static void twin_mips(TAGPU_GAFATLAS* a);   /* below; job_clear_dest wants it */

static void rgb_mirror_zeroed(TAGPU_GAFATLAS* a)
{
    if (!a->mirrorRgb) return;
    /* AND IT RUNS WHETHER OR NOT A LANE IS CURRENTLY ASKING, which is 16 MB on
       a recycle for nobody. That is not waste to be optimised away: the mirror
       is deliberately never freed (tagpu_gaf.h), so a lane that re-arms later
       finds it already correct -- and it can only do that if the zeroing
       happened when the twin was zeroed, not when someone next looked. */
    memset(a->mirrorRgb, 0, tagpu_gaf_mip_chain(a->mirrorRgbDim, a->mirrorRgbMip));
    /* the ROWS are kept: they are the high-water mark of what a consumer has
       been handed, and it has to be handed the zeros over exactly those */
    a->mirrorRgbSerial++;
}

/* the job's destination back to unpainted, and every mirror of it with it */
static void job_clear_dest(TAGPU_GAFATLAS* a)
{
    /* THE PUBLISHED LIST GOES WHATEVER THE GL JOB IS, and that is deliberately
       outside the early return below: the rects this atlas hands out have just
       moved (a recycle, a repack), so a consumer's own destination is as wrong
       as this twin is, whether or not a GL job exists to clear. The generation
       is what tells it, and dropping the list is what stops it painting the
       old layout over the new one. */
    rlist_reset(a, 0);
    if (!a->job) return;
    tagpu_rglsl_job_clear(a->job);
    twin_mips(a);
    rgb_mirror_zeroed(a);
}

/* OUR OWN REDUCTION FIRST, glGenerateMipmap ONLY AS THE FALLBACK. The twin is
   sampled GL_LINEAR_MIPMAP_LINEAR, so its levels 1.. are part of the picture a
   second backend has to reproduce -- and gpu-status 2.45 measured that this
   driver's glGenerateMipmap is an unweighted 2x2 box average with a rounding
   rule no candidate matched exactly, every candidate landing within +/-1 per
   RGB channel. A per-driver +/-1 cannot be pinned down by a note, so both lanes
   do the reduction themselves (`tagpu_rglsl_mips`) and the levels are identical
   by construction. The fallback is still here because it is what shipped before
   landing 7e: it loses the cross-driver identity of the levels, not the levels.
   [The Vulkan-only plan's landing 7e.] */
static void twin_mips(TAGPU_GAFATLAS* a)
{
    if (!a->mip || !a->rgb) return;
    if (!tagpu_rglsl_mips(a->rgb, a->dim, a->mip)) {
        if (!x_glGenerateMipmap) return;
        glBindTexture(GL_TEXTURE_2D, a->rgb);
        x_glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    a->mippedN = tagpu_rglsl_job_painted(a->job);
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

const char* tagpu_gaf_seq_name(const char* seq)
{
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return "?";
    return seq + TAGPU_SQ_NAME;
}

int tagpu_gaf_decode(const unsigned char* g, int w, int h, unsigned char* out)
{
    unsigned char ck, comp;
    const unsigned char* px;
    const unsigned char* p;
    int y;
    /* THE HEADER IS BOUNDED BEFORE IT IS READ, and it was not: this function
       took `ck`, `comp` and the pixel POINTER out of `g` on its first three
       lines with only `px` checked afterwards, which is the same one-line
       defect `frame_key` had and which cost a crash to find. Its own header
       promises "0 if unreadable", so the check belongs here rather than in each
       caller. No caller reaches it unsanitised today -- the UI publisher is
       behind `frame_key`, the menu and the order overlay come through
       `tagpu_gaf_seq_frame` -- so this is a BOUND that closes the contract, not
       the lifetime argument, which stays the caller's.
       [FOUND 2026-09-16, the landing-5 review.] */
    if (!tagpu_gaf_frame_sane(g)) return 0;
    ck = g[TAGPU_GF_CK]; comp = g[TAGPU_GF_COMP];
    px = *(const unsigned char* const*)(g + TAGPU_GF_PIX);
    if (!ptr_ok(px)) return 0;
    if (comp == 0) {
        if (IsBadReadPtr(px, (SIZE_T)w * h)) return 0;
        for (y = 0; y < h; y++) memcpy(out + (size_t)y * w, px + (size_t)y * w, (size_t)w);
        return 1;
    }
    memset(out, ck, (size_t)w * h);
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
                for (i = 0; i < n && x < w; i++) row[x++] = v;
            } else {
                int n = (b >> 2) + 1, i;
                for (i = 0; i < n && q < p + rowlen; i++) {
                    unsigned char v = *q++;
                    if (x < w) row[x] = v;
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
    /* PART 2: re-lay what is here instead of throwing it away. Only for an
       atlas that asked (`repack`), only when it actually filled -- a restart
       that is not "full" is the UI atlas re-arming or a context change, and
       both want the entries gone -- and only while a re-lay can still gain
       something. atlas_repack owns the log line and leaves the atlas usable. */
    if (wasFull && a->repack) {
        if (a->repackWall) return;      /* held: nothing a rebuild can improve */
        if (atlas_repack(a)) return;
    }
    /* the sprite atlases reset when full; the UI atlas also on a re-arm or a
       GL context change (tagpu_gui_surf.c twins_reset), which is not "full" */
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
   atlas_get repaints it where it sits, into GL and the mirror together. */
int tagpu_gaf_atlas_mirror(TAGPU_GAFATLAS* a)
{
    char b[160];
    int i, again = 0;
    if (!a || a->dim <= 0) return 0;
    if (a->mirror) return 1;
    a->mirror = (unsigned char*)calloc((size_t)a->dim * a->dim, 1);
    if (!a->mirror) {
        _snprintf(b, sizeof b, "%s: no memory for a %d KB atlas mirror — the Vulkan"
                  " edition of this pass stays down", a->tag ? a->tag : "gaf",
                  (a->dim * a->dim) >> 10);
        b[sizeof b - 1] = 0;
        glog(b);
        return 0;
    }
    for (i = 0; i < a->n; i++)
        if (a->ents[i].ok) { a->ents[i].ok = 0; a->ents[i].resv = 1; again++; }
    a->mirrorSerial = 0;
    _snprintf(b, sizeof b, "%s: atlas mirror armed, %d KB — %d painted frame(s)"
              " re-decode on their next use so the mirror holds them too",
              a->tag ? a->tag : "gaf", (a->dim * a->dim) >> 10, again);
    b[sizeof b - 1] = 0;
    glog(b);
    return 1;
}

/* ---- the RESTORED twin's mirror (tagpu_gaf.h, G19f landing 4) ---- */

/* The rows the shelf has actually used. Cells are laid in shelves from row 0
   up, so nothing is painted at or below `shelfY + shelfH` and reading further
   would be reading memory no entry can ever name -- the same bound
   tagpu_gui_surf.c already publishes as `atlasRows` for the indexed mirror. */
static int rgb_rows(const TAGPU_GAFATLAS* a)
{
    int rows = a->shelfY + a->shelfH;
    if (rows < 1) rows = 1;
    if (rows > a->dim) rows = a->dim;
    return rows;
}

int tagpu_gaf_atlas_mirror_rgb(TAGPU_GAFATLAS* a)
{
    char b[160];
    if (!a || a->dim <= 0) return 0;
    if (a->mirrorRgb) return 1;
    if (a->mirrorRgbFailed) return 0;
    /* THE LIST HAS TAKEN OVER: the restore is the other lane's to run, so
       there is nothing here to read back and a 16 MB buffer would be armed for
       a consumer that no longer looks at it.
       AND THE READ-BACK IS NOT A FALLBACK IF THE LIST LATER DIES, which this
       comment claimed until the landing-7d review: both of the owning pass's
       arm latches are ONE-WAY (`s_rlistAsked` and `s_mirrorRgbAsked` are only
       ever set), and arming the list freed the mirror, so after the
       out-of-memory drop below there is neither. The consumer stands down
       instead -- `restore_want` clears its "this twin is a picture" flag when
       the request disappears under a live job -- which is a stand-down rather
       than a lane drawing a frozen twin against a GL lane that is still
       restoring. Said here because this is where the fallback was promised. */
    if (a->rlistWant) return 0;
    fetch_gl();
    if (!x_glReadPixels || !glGenFramebuffers || !glBindFramebuffer ||
        !glFramebufferTexture2D || !glCheckFramebufferStatus) {
        a->mirrorRgbFailed = 1;
        _snprintf(b, sizeof b, "%s: no glReadPixels/FBO entry points - the restored"
                  " twin cannot be mirrored and the Vulkan edition stays indexed",
                  a->tag ? a->tag : "gaf");
        b[sizeof b - 1] = 0;
        glog(b);
        return 0;
    }
    a->mirrorRgbDim = a->dim; a->mirrorRgbMip = a->mip;
    a->mirrorRgb = (unsigned char*)calloc(tagpu_gaf_mip_chain(a->dim, a->mip), 1);
    if (!a->mirrorRgb) {
        a->mirrorRgbFailed = 1;
        _snprintf(b, sizeof b, "%s: no memory for a %d KB restored-twin mirror - the"
                  " Vulkan edition of this pass stays indexed",
                  a->tag ? a->tag : "gaf",
                  (unsigned)(tagpu_gaf_mip_chain(a->dim, a->mip) >> 10));
        b[sizeof b - 1] = 0;
        glog(b);
        return 0;
    }
    /* NOTHING IS MARKED FOR REPAINT HERE, and that is the difference from the
       indexed mirror's arming (tagpu_gaf.h): calloc's alpha 0 IS the restorer's
       own "not painted here yet", so a consumer reading this mirror before the
       first step gets the answer an unpainted cell would give it -- indexed
       art -- rather than a wrong one. The first step reads the whole used
       region back and it is level from there. */
    a->mirrorRgbRows = 0;
    a->mirrorRgbSerial = 0;
    a->mirroredPainted = 0;
    a->mirroredRgbGen = 0;
    _snprintf(b, sizeof b, "%s: restored-twin mirror armed, %u KB (%d mip level(s))"
              " - read back when the restorer paints and not otherwise",
              a->tag ? a->tag : "gaf",
              (unsigned)(tagpu_gaf_mip_chain(a->dim, a->mip) >> 10), a->mip + 1);
    b[sizeof b - 1] = 0;
    glog(b);
    return 1;
}

/* THE RESTORED TWIN'S MIRROR IS THE WHOLE MIP CHAIN, not level 0 alone, and
   the reason is measurable: `tagpu_gaf_atlas_restore` gives a mipped twin
   GL_LINEAR_MIPMAP_LINEAR to GL_TEXTURE_MAX_LEVEL, so a consumer holding only
   level 0 draws a different picture wherever the art is minified. On the unit
   atlas a 32-texel cell lands on a ~23 px sprite at 1024x768 -- LOD around 0.5,
   which is a blend of levels 0 and 1 -- so "wherever it is minified" is
   ordinary play. MEASURED before this existed: 2 126 of 2 132 unit pixels
   differing, worst channel 155, and the figure barely moved when only the
   MAGNIFICATION filter was matched. [gate 3a, 2026-09-16.]

   THE LEVELS ARE READ BACK, NOT RE-DERIVED. A blit chain on the Vulkan side
   would be this fork guessing at glGenerateMipmap's reduction, and the guess
   would be a per-driver difference that no note could pin down. Reading GL's
   own levels makes the two byte-identical by construction, which is the same
   rule the whole seam runs on: hand over the bytes, never the derivation.

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

/* THE READ-BACK, FOR ANY TEXTURE (the Vulkan-only plan's gate 2).
   `tagpu_gaf_atlas_mirror_rgb_step` below CALLS THIS -- it is the atlas's own
   use of exactly this and does not repeat it; terrain's restored twin is a GL
   texture built by tagpu_terr.c and painted by the restorer, so it needs the
   same read-back without the atlas around it. The two bodies were the same
   thirty-five lines twice over until the gate-2 landing review said so, which
   is a second place for the pack alignment, the saved binding or the dropped
   attachment to be got wrong.

   IT LIVES HERE RATHER THAN IN A NEW FILE because this is where the entry
   points are already resolved -- `glReadPixels` is not in the fork's own
   globals and has to be fetched (see `fetch_gl`), and a second file resolving
   it again is a second place to get wrong. Nothing about the atlas is touched.

   ROW 0 IS MEMORY ROW 0, NOT THE SCREEN'S. glReadPixels is described bottom-up
   because the default framebuffer's y = 0 is the bottom of the screen; the
   attachment here is a TEXTURE, whose y = 0 is the row glTexSubImage2D and
   vkCmdCopyBufferToImage both write first. So this fills `dst` in the order a
   second backend uploads it, with no flip.

   `level` is the mip level to attach -- 0 for an unmipped texture, and the
   whole chain read one call at a time for a mipped one.

   `fbo` is the caller's, created here on first use and owned by the caller:
   one FBO per client, made once, never per frame. Returns 1 when `dst` holds
   `rows` rows of RGBA8 and 0 when it holds nothing new.

   `status` (optional) is how a caller tells a PERMANENT refusal from a frame
   that simply had nothing: it is the `glCheckFramebufferStatus` value when one
   was taken, and 0 when this got no further than the entry points or the FBO
   name. An incomplete framebuffer will be incomplete again next frame -- it is
   a property of the texture, not of the moment -- so both callers latch on it
   and stop asking, and neither could do that from the return value alone. */
int tagpu_gl_rgba_readback(unsigned tex, int level, int w, int rows,
                           unsigned char* dst, unsigned* fbo, unsigned* status)
{
    GLint fbo0 = 0, pack = 4;
    GLenum st;
    int ok = 0;
    if (status) *status = 0;
    if (!tex || !dst || !fbo || w <= 0 || rows <= 0) return 0;
    fetch_gl();
    if (!x_glReadPixels || !glGenFramebuffers || !glBindFramebuffer ||
        !glFramebufferTexture2D || !glCheckFramebufferStatus) return 0;
    if (!*fbo) {
        glGenFramebuffers(1, fbo);
        if (!*fbo) return 0;
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo0);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status) *status = (unsigned)st;
    if (st == GL_FRAMEBUFFER_COMPLETE) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        x_glReadPixels(0, 0, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        ok = 1;
    }
    /* THE ATTACHMENT IS DROPPED WHATEVER HAPPENED: leaving someone else's
       texture attached to our FBO would keep it alive past a delete and make
       the next status check answer about the wrong image. */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo0);
    return ok;
}

void tagpu_gaf_atlas_mirror_rgb_step(TAGPU_GAFATLAS* a)
{
    unsigned st = 0;
    int rows, painted;
    if (!a || !a->mirrorRgb) return;
    /* AND THE BUFFER STILL HAS TO BE THIS ATLAS'S SHAPE. It is never freed, so
       a `mip` that moved under it (the demote on a GL with no glGenerateMipmap,
       against the owner's re-init on a context reset) would have this read-back
       write levels the allocation has no room for. Refused for the frame and
       NOT latched: the demote is re-applied at the top of the next restore, so
       the window closes on its own and a latch would cost the mirror for ever
       over a frame. [The gate-3a verification pass named the pair; the chain is
       this landing's, so this hazard is too.] */
    if (a->dim != a->mirrorRgbDim || a->mip != a->mirrorRgbMip) return;
    if (!a->rgb || !a->job) {
        /* the twin is gone (a re-arm, or a context loss before the re-create).
           Say so rather than leaving the last twin's colours standing: a
           consumer that kept them would restore art the GL lane no longer
           does. The `rgbGen` bump on the re-create brings the next step in. */
        if (a->mirrorRgbRows) {
            memset(a->mirrorRgb, 0, tagpu_gaf_mip_chain(a->mirrorRgbDim, a->mirrorRgbMip));
            a->mirrorRgbRows = 0;
            a->mirrorRgbSerial++;
        }
        a->mirroredPainted = 0;
        a->mirroredRgbGen = 0;
        return;
    }
    painted = tagpu_rglsl_job_painted(a->job);
    rows = rgb_rows(a);
    /* THE CONTENT KEY IS THE PAINT COUNT, `rgbGen` FOR ITS DISCONTINUITY, AND
       THE ROW BOUND FOR THE SHELF GROWING UNDER IT. Every one of the three is
       a thing that changes what a consumer would read and nothing else is:
       keying on the paint count alone misses the re-arm (it restarts at 0) and
       keying on it plus the generation misses a shelf that grew without a
       paint landing yet -- which leaves rows in the mirror that were never
       read. [This is landing 2's lesson, which cost that landing two rounds: a
       serial that is not the CONTENT's serial uploads once and then misses
       everything after it.] */
    /* AND THE MIP GENERATION IS PART OF THE KEY, which is the fourth thing
       this content key has had to learn. `twin_mips` runs at the TOP of a frame
       out of `tagpu_gaf_atlas_restore`, `tagpu_rglsl_step` paints in the
       MIDDLE of it, and this read-back runs at the END -- so the read-back that
       first sees a new paint count sees level 0 freshly painted and levels 1+
       as they were BEFORE it, and then latches `mirroredPainted` and never
       looks again. The mirror's level 0 was right and its levels 1+ were one
       batch stale, for good.
       MEASURED: 581 of 1 528 unit pixels differing at 640x480 with every filter
       setting already matched, 422 of them by more than 8 levels, and the
       Vulkan side showing flat greys where the GL side had colour -- the
       signature of sampling a mip that was built from different texels.
       [FOUND 2026-09-16, gate 3a, by looking at WHERE the residual was rather
       than trying one more filter.] */
    if (painted == a->mirroredPainted && a->rgbGen == a->mirroredRgbGen &&
        a->mippedN == a->mirroredMippedN && rows <= a->mirrorRgbRows)
        return;
    /* THE READ-BACK ITSELF IS `tagpu_gl_rgba_readback` ABOVE -- the FBO, the
       pack alignment, the saved binding, the dropped attachment and the fact
       that row 0 is memory row 0 and not the screen's all live there, once. */
    if (tagpu_gl_rgba_readback(a->rgb, 0, a->dim, rows, a->mirrorRgb,
                               &a->mirrorRgbFbo, &st)) {
        /* AND EVERY OTHER LEVEL, WHOLE. Only level 0 is worth bounding by the
           shelf: level 1 of a 2048 twin is 4 MB and level 2 is 1 MB, the
           arithmetic to bound them would have to round the shelf cursor down
           per level, and a level read short is a level whose tail keeps the
           previous twin's colours. They are also written by glGenerateMipmap
           in one go, so there is no partial state to track.
           A LEVEL THAT FAILS COSTS THE WHOLE MIRROR, and the first draft of
           this comment said the opposite -- that the chain is only as deep as
           the levels that came back and a consumer builds the shallower image.
           It cannot: GL still filters this twin to its own MAX_LEVEL, so a
           shallower chain on the other side is a different picture wherever
           the art is minified. `mirrorRgbMips` still records how deep the read
           got, and `tagpu_render3do.c`'s accessor turns anything short of
           `mip` into NO MIRROR. [Corrected by the gate-3a re-review.] */
        int L;
        a->mirrorRgbMips = 0;
        for (L = 1; L <= a->mip; L++) {
            int d = a->dim >> L;
            if (d < 1) d = 1;
            if (!tagpu_gl_rgba_readback(a->rgb, L, d, d,
                                        a->mirrorRgb + tagpu_gaf_mip_off(a->dim, L),
                                        &a->mirrorRgbFbo, &st))
                break;
            a->mirrorRgbMips = L;
        }
        if (rows > a->mirrorRgbRows) a->mirrorRgbRows = rows;
        a->mirroredPainted = painted;
        a->mirroredRgbGen = a->rgbGen;
        a->mirroredMippedN = a->mippedN;
        a->mirrorRgbSerial++;
        return;
    }
    /* NO STATUS MEANS NOTHING TO LATCH: the entry points were not resolved or
       the FBO name could not be made, and neither says anything about this
       texture. An INCOMPLETE framebuffer does, and it will say it again every
       frame, so it is answered once and for good. */
    if (!st || st == GL_FRAMEBUFFER_COMPLETE) return;
    {
        char b[160];
        _snprintf(b, sizeof b, "%s: restored-twin read-back FBO incomplete (%x) - the"
                  " Vulkan edition of this pass stays indexed",
                  a->tag ? a->tag : "gaf", st);
        b[sizeof b - 1] = 0;
        glog(b);
        free(a->mirrorRgb);
        a->mirrorRgb = NULL;
        a->mirrorRgbRows = 0;
        a->mirrorRgbFailed = 1;
        a->mirrorRgbSerial++;
        /* and the FBO with it: the latch means nothing will ever ask again, so
           holding a name for the process's life buys nothing. Deleted while its
           context is still current, which is what separates this from
           `tagpu_gaf_atlas_lost` -- there the context is gone and a delete
           would either do nothing or destroy a live object of the NEW one.
           The helper has already dropped the attachment and put the previous
           binding back, so this is a name with nothing attached to it. */
        glDeleteFramebuffers(1, &a->mirrorRgbFbo);
        a->mirrorRgbFbo = 0;
    }
}

/* Arm the published restore list and stand the read-back down (tagpu_gaf.h).
   Polled on the owner's arm beat, so the lever is allowed to appear
   mid-session -- which is the case that has already been got wrong once on
   this plan: a latch that is only ever tested at start-up reads as "off" for
   a session the owner turned it on during. [tagpu_terr.c carries the same
   poll for the terrain atlas, whose restore is a fixed list rather than a
   queue and so needs no cursor.] */
int tagpu_gaf_atlas_restore_vk(TAGPU_GAFATLAS* a)
{
    char b[192];
    if (!a || a->dim <= 0 || a->max <= 0 || !a->ents) return 0;
    if (a->rlistWant) return 1;
    if (a->rlistFailed) return 0;
    if (GetFileAttributesA("tagpu_restorevk.on") == INVALID_FILE_ATTRIBUTES) return 0;
    /* A FIRST ALLOCATION ONLY -- `rlist_room` is what sizes it from here, and
       the seed below asks it for however many entries the atlas holds. */
    a->rlistCap = 256;
    if (a->rlistCap > rlist_cap(a)) a->rlistCap = rlist_cap(a);
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
       instant it exists: whatever this lane has already restored, a consumer
       starting at index 0 restores the same rectangles for itself. */
    rlist_restart(a, 0);
    /* AND THE READ-BACK'S 16 MB GOES BACK. The mirror is documented as never
       freed -- because an atlas has no destructor and a re-arm should find it
       already correct -- and this is the one exception, with its own reason:
       the list is not a second consumer of the mirror, it is the mirror's
       replacement, and nothing will read it again while the list is armed.
       Every write to it is already guarded on the pointer (`mirror_rgb_step`
       returns at the top when it is NULL), so freeing it here is not a new
       lifetime to reason about. */
    if (a->mirrorRgb) {
        free(a->mirrorRgb);
        a->mirrorRgb = NULL;
        a->mirrorRgbRows = 0;
        a->mirroredPainted = 0;
        a->mirroredRgbGen = 0;
        a->mirroredMippedN = 0;
        a->mirrorRgbSerial++;       /* what a consumer holds is no longer fed  */
    }
    _snprintf(b, sizeof b, "%s: restorevk -- the restore is the other lane's to run, so"
              " no read-back and the frame list is published instead (%d entries seeded,"
              " %d-frame bound)", a->tag ? a->tag : "gaf", a->rlistN, rlist_cap(a));
    b[sizeof b - 1] = 0;
    glog(b);
    return 1;
}

/* GIVE BACK THE TWO HEAP BUFFERS AN ATLAS OWNS, for a caller that is about to
   lay the struct out again from zero. `mirror` (dim*dim) and `mirrorRgb` (the
   mip chain) are the only allocations in a TAGPU_GAFATLAS, and neither is
   freed by `_lost` -- it keeps them deliberately, so that a mirror is never
   stale for the frames between a context loss and the next create. A caller
   that re-arms by zeroing the struct therefore drops both pointers and leaks
   them; every writer already guards on the pointer, and `_mirror`/`_mirror_rgb`
   re-arm on demand, so handing them back here costs nothing that the memset
   was not already costing functionally.
   [FROM THE 4b-3 LANDING REVIEW.] */
void tagpu_gaf_atlas_free_buffers(TAGPU_GAFATLAS* a)
{
    if (!a) return;
    free(a->mirror);    a->mirror = NULL;
    free(a->mirrorRgb); a->mirrorRgb = NULL;
}

void tagpu_gaf_atlas_lost(TAGPU_GAFATLAS* a)
{
    /* `made` goes with the name: the layout described a texture that no longer
       exists, so the next create must lay it out again. */
    a->tex = 0; a->made = 0;
    a->n = 0; a->shelfX = a->shelfY = a->shelfH = 0; a->full = 0;
    a->gen++;                   /* ...and again: the texture itself is gone */
    /* and so is everything it held, so the mirror of it says nothing. (The
       re-create zeroes it again; doing it here as well means a mirror is never
       stale for the frames between a loss and the next create.) */
    if (a->mirror) { memset(a->mirror, 0, (size_t)a->dim * a->dim); a->mirrorSerial++; }
    memset(a->hash, 0, sizeof a->hash);
    /* the twin and the job died with the context (tagpu_rglsl_glreset has
       already forgotten the job: it runs first); re-armed on the next frame */
    a->rgb = 0; a->job = NULL; a->restoreFailed = 0; a->mippedN = 0;
    /* THE OTHER LANE'S DESTINATION DID NOT DIE -- ITS SOURCE DID. Nothing here
       is a Vulkan object, so a consumer's twin still holds the colours of an
       atlas whose every entry has just been dropped. The generation is what
       tells it to blank and start over; without this it would keep painting
       the old layout's rects for the rest of the session. */
    rlist_reset(a, 0);
    a->mirroredMippedN = 0;
    /* THE FBO DIED WITH THE CONTEXT TOO -- forgotten, never deleted, exactly
       as `tex` is above: deleting a name from a context that is gone either
       does nothing or destroys a live object of the NEW one that has been
       handed the same number. The mirror's bytes survive, and are zeroed with
       the twin they mirror because that twin no longer exists. */
    a->mirrorRgbFbo = 0;
    a->mirroredPainted = 0; a->mirroredRgbGen = 0;
    if (a->mirrorRgb) {
        memset(a->mirrorRgb, 0, tagpu_gaf_mip_chain(a->mirrorRgbDim, a->mirrorRgbMip));
        a->mirrorRgbRows = 0;
        a->mirrorRgbSerial++;
    }
    /* the entries went with the texture, so the wall the last fill hit says
       nothing about the next one */
    a->repackWall = 0;
}

/* one frame onto the restore queue -- and onto the published list, which is
   the same frame and must stay so: `restore_frame_of` is shared. */
static void restore_enqueue(TAGPU_GAFATLAS* a, const TAGPU_GAFENT* e)
{
    TAGPU_RGLSL_FRAME f;
    if (!restore_frame_of(a, e, &f)) return;
    tagpu_rglsl_job_add(a->job, &f, 1);
    rlist_add(a, &f);
}

/* tagpu_restoredump.on: the twin as the shader samples it, once per fill of
   the atlas, as raw bytes -- the restorer's only disk write, and only under
   the trigger (the terrain pass writes its own). Three files per atlas:
   tagpu_restore_<tag>.r8 (the source, dim x dim), .rgba (the twin, dim x dim
   x 4) and .idx (a line per entry: x y w h key wrap), so `tascene featdiff`
   can find each frame in both and hold the twin to the lab's bar. */
static void dump_if_armed(TAGPU_GAFATLAS* a)
{
    static unsigned s_check;
    char name[64], b[160];
    unsigned char* buf;
    FILE* f;
    int i;
    if (!a->job || a->n == 0 || a->n == a->dumpedN) return;
    if (!tagpu_rglsl_job_idle(a->job)) return;
    if (++s_check % 60) return;                    /* one attribute read a second */
    if (GetFileAttributesA("tagpu_restoredump.on") == INVALID_FILE_ATTRIBUTES) return;
    buf = (unsigned char*)malloc((size_t)a->dim * a->dim * 4);
    if (!buf) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    _snprintf(name, sizeof name, "tagpu_restore_%s.r8", a->tag);
    glBindTexture(GL_TEXTURE_2D, a->tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buf);
    f = fopen(name, "wb");
    if (f) { fwrite(buf, 1, (size_t)a->dim * a->dim, f); fclose(f); }
    _snprintf(name, sizeof name, "tagpu_restore_%s.rgba", a->tag);
    glBindTexture(GL_TEXTURE_2D, a->rgb);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    f = fopen(name, "wb");
    if (f) { fwrite(buf, 1, (size_t)a->dim * a->dim * 4, f); fclose(f); }
    free(buf);
    /* ...AND EVERY MIP LEVEL OF A MIPPED TWIN, as one file, level 0 first and
       each level `dim >> L` square (the layout `tagpu_gaf_mip_off` describes).
       Only the unit atlas is mipped, and it is the last consumer this lane has
       to wire: a Vulkan restore paints level 0 and the twin is sampled
       GL_LINEAR_MIPMAP_LINEAR, so the levels have to come from somewhere, and
       the choice is between reading GL's back (what the mirror does today) and
       reducing them here. That choice is a question about what
       glGenerateMipmap ACTUALLY DID, and this file is the only place that can
       answer it -- hence the dump. [landing 7e's measurement.] */
    if (a->mip > 0 && a->rgb) {
        size_t chain = tagpu_gaf_mip_chain(a->dim, a->mip);
        unsigned char* mbuf = (unsigned char*)malloc(chain);
        if (mbuf) {
            int L, got = 0;
            glBindTexture(GL_TEXTURE_2D, a->rgb);
            for (L = 0; L <= a->mip; L++) {
                int d = a->dim >> L;
                if (d < 1) d = 1;
                glGetTexImage(GL_TEXTURE_2D, L, GL_RGBA, GL_UNSIGNED_BYTE,
                              mbuf + tagpu_gaf_mip_off(a->dim, L));
                got = L;
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            _snprintf(name, sizeof name, "tagpu_restore_%s.mips", a->tag);
            f = fopen(name, "wb");
            if (f) { fwrite(mbuf, 1, chain, f); fclose(f); }
            free(mbuf);
            _snprintf(b, sizeof b, "%s: the twin's mip chain dumped to tagpu_restore_%s.mips"
                      " (%d levels of %d, %u KB)", a->tag, a->tag, got + 1, a->dim,
                      (unsigned)(chain >> 10));
            b[sizeof b - 1] = 0;
            glog(b);
        }
    }
    _snprintf(name, sizeof name, "tagpu_restore_%s.idx", a->tag);
    f = fopen(name, "w");
    if (f) {
        for (i = 0; i < a->n; i++) if (a->ents[i].ok)
            fprintf(f, "%d %d %d %d %d %d\n", a->ents[i].x, a->ents[i].y, a->ents[i].w, a->ents[i].h,
                    a->ents[i].ck, a->ents[i].wrap);
        fclose(f);
    }
    a->dumpedN = a->n;
    _snprintf(b, sizeof b, "%s: restored twin dumped to tagpu_restore_%s.{r8,rgba,idx} (%dx%d, %d entries)%s",
              a->tag, a->tag, a->dim, a->dim, a->n, f ? "" : " -- WRITE FAILED");
    glog(b);
}

void tagpu_gaf_atlas_restore(TAGPU_GAFATLAS* a, const unsigned char* pal)
{
    int i;
    a->pal = pal;
    if (a->job) {
        /* The palette moved under the twin — the Gamma slider, or `+gamma N`
           in chat: every texel in it was restored through the old one and is
           now the wrong brightness beside the engine's own pixels. Re-point
           the job and queue every entry again, WITHOUT clearing, so the atlas
           recolours cell by cell instead of vanishing for the length of the
           repaint. Gated on the job being idle, which bounds this to one
           repaint of this atlas in flight however often the palette moves. */
        if (pal && a->palSerial != tagpu_pal_serial() && tagpu_rglsl_job_idle(a->job)) {
            char b[128];
            a->palSerial = tagpu_pal_serial();
            tagpu_rglsl_job_repalette(a->job, pal);
            /* AND THE PUBLISHED LIST IS A REPAINT GENERATION, opened before
               the loop below so that the loop's own `restore_enqueue` fills
               it. A consumer rebuilds its job with `repaint` set and keeps
               what its destination holds, exactly as this lane does. */
            rlist_reset(a, 1);
            for (i = 0; i < a->n; i++) if (a->ents[i].ok) restore_enqueue(a, &a->ents[i]);
            _snprintf(b, sizeof b, "%s: palette changed (serial=%u): %d entries queued for repaint",
                      a->tag, a->palSerial, a->n);
            glog(b);
        }
        /* a mipped twin: its levels follow level 0 one frame behind the
           batch that painted it (the OUT draw is issued after this call,
           in tagpu_rglsl_step; the next frame's call sees the count move) */
        if (a->mip && tagpu_rglsl_job_painted(a->job) != a->mippedN) twin_mips(a);
        dump_if_armed(a);
        return;
    }
    if (a->restoreFailed || !a->tex || !pal) return;
    if (!tagpu_classicpp_assets()) return;
    fetch_gl();
    if (a->mip && (!x_glGenerateMipmap || !x_glTexParameterf)) {
        char b[128];
        _snprintf(b, sizeof b, "%s: no glGenerateMipmap/glTexParameterf: twin left unmipped, NEAREST", a->tag);
        glog(b);
        a->mip = 0;
    }
    if (!a->rgb) {
        GLuint t = 0;
        glGenTextures(1, &t);
        if (!t) { a->restoreFailed = 1; return; }
        glBindTexture(GL_TEXTURE_2D, t);
        if (a->mip) {
            /* renderers.md 1: trilinear to level `mip`, anisotropic where the
               extension answers -- a driver without it raises INVALID_ENUM
               on the parameter and is otherwise unaffected, so try it and
               read the error flag, drained first because it is process-wide */
            char b[160];
            int pending = 0;
            GLenum err;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, a->mip);
            while (glGetError() != GL_NO_ERROR && pending < 16) pending++;
            {
                /* `aniso=` (tagpu_classicpp.h): 4 in play, 1 when the Vulkan
                   A/B is being taken, because the two APIs place anisotropic
                   samples differently and that is the one difference the port
                   cannot close. BOTH LANES READ THE SAME KNOB. */
                float want = tagpu_classicpp_light()->aniso;
                if (want < 1.0f) want = TWIN_ANISO;
                x_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, want);
                err = glGetError();
                a->rgbAniso = (err == GL_NO_ERROR && want > 1.0f) ? want : 0.0f;
            }
            /* RECORDED, NOT JUST LOGGED, above. A second backend has to apply
               the same ratio, and both "the extension answered" and "the knob
               said 1" are facts it cannot work out for itself. */
            _snprintf(b, sizeof b, "%s: restored twin %dx%d, trilinear to mip level %d, %s%s",
                      a->tag, a->dim, a->dim, a->mip,
                      a->rgbAniso > 1.0f ? "anisotropic" : "no anisotropic filtering",
                      err == GL_NO_ERROR ? "" : " (extension absent)");
            if (a->rgbAniso > 1.0f) {
                char r[16];
                _snprintf(r, sizeof r, " %.0fx", (double)a->rgbAniso);
                r[sizeof r - 1] = 0;
                strncat(b, r, sizeof b - strlen(b) - 1);
            }
            glog(b);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            a->rgbAniso = 0.0f;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* EVERY LEVEL IS ALLOCATED HERE, not left to glGenerateMipmap to
           create. `twin_mips` reduces the chain itself as of landing 7e-1, and
           a reduction draws INTO level L through a framebuffer -- which a level
           with no storage makes incomplete, so the first chain of every twin
           would silently fall back to the driver's reduction and a twin painted
           once would keep it for good.

           THE LEVELS ARE UNDEFINED UNTIL THE `twin_mips` BELOW, and what makes
           that safe is an invariant rather than the shortness of the window --
           which is what the first version of this comment argued, wrongly, and
           it was six statements including a call:

             * nothing between here and there SAMPLES the twin. The only thing
               that touches it is `tagpu_rglsl_job_new`, which renders into
               level 0 to clear it;
             * the one path that abandons the twin DELETES it (`a->rgb = 0`
               below), so no twin with an undefined chain is ever published;
             * and `twin_mips` always writes the chain when `a->mip` is set,
               because the branch above demotes `a->mip` to 0 whenever
               `glGenerateMipmap` did not resolve -- so the fallback is
               guaranteed to be there when it is needed. */
        {
            int L;
            for (L = 0; L <= a->mip; L++) {
                int d = a->dim >> L;
                if (d < 1) d = 1;
                glTexImage2D(GL_TEXTURE_2D, L, GL_RGBA8, d, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            }
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        a->rgb = t;
        /* the content key's discontinuity: a fresh twin is alpha 0 everywhere
           and the job that fills it counts from 0 again (tagpu_gaf.h) */
        a->rgbGen++;
    }
    a->job = tagpu_rglsl_job_new(a->tag ? a->tag : "gaf", a->prio, 0,
                                 a->tex, a->dim, a->dim, pal, a->rgb, a->dim, a->dim);
    if (!a->job) {
        /* the reason is in tagpu.log. The twin goes with the job: the passes
           gate their restored branch on `rgb`, and a twin nothing has cleared
           is 16 MB of whatever the driver left there, alpha included */
        glDeleteTextures(1, &a->rgb);
        a->rgb = 0;
        a->restoreFailed = 1;
        return;
    }
    a->palSerial = tagpu_pal_serial();
    /* the job cleared level 0 to alpha 0: the mip levels must say the same
       before anything samples them -- and since landing 7e-1 they are ALLOCATED
       but undefined until this runs, rather than absent until it runs, so this
       is the call that makes the twin samplable at all rather than merely
       consistent. It cannot fail to write them: `a->mip` is 0 unless
       `glGenerateMipmap` resolved, so either our reduction runs or that does.
       A twin whose chain this left undefined is one a trilinear fetch reads
       garbage from; before, it was one GL reported incomplete. */
    twin_mips(a);
    /* what is already in the atlas was uploaded before the switch: queue it,
       in upload order, so nothing stays indexed for want of a miss */
    rlist_reset(a, 0);          /* a new twin here is a new one over there    */
    for (i = 0; i < a->n; i++) if (a->ents[i].ok) restore_enqueue(a, &a->ents[i]);
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
    GLuint t = 0;
    if (a->made) return 1;
    if (a->dim <= 0 || a->max <= 0 || !a->ents) return 0;
    /* THE GL NAME IS OPTIONAL; THE LAYOUT IS NOT. On a lane with no GL there is
       no texture to create and none is needed -- the shelf packer, the entry
       table and the CPU mirror below are the atlas, and the twin uploads the
       mirror. `if (a->tex) return 1` used to stand where `a->made` does, and
       `glGenTextures` leaving `t` at 0 with no context turned this into a
       refusal that read downstream as `atlas=0` and no sprite texels at all.
       [The vulkan-only plan, landing 4b-2.] */
    if (!tagpu_vk_owns_present()) {
        glGenTextures(1, &t);
        if (!t) return 0;
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, a->dim, a->dim, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    /* A FRESH TEXTURE IS A FRESH MIRROR. The storage above is unwritten (index
       0, the assumption the border comment below rests on) and a mirror that
       kept the previous texture's texels would be a copy of something that no
       longer exists. atlas_drop is deliberately NOT here: it leaves the texels
       alone and lets re-inserted entries overwrite them, and the mirror follows
       exactly because it follows the paints. */
    if (a->mirror) { memset(a->mirror, 0, (size_t)a->dim * a->dim); a->mirrorSerial++; }
    a->tex = t;
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
   below). We cannot move the texels ourselves -- an entry records the
   frame's address and its size, never the decoded bytes, and GL 3.3 core has
   no glCopyImageSubData to shuffle them with -- so the pixels come back the
   way they arrived the first time, by RLE decode on demand. That is the same
   work one of today's recycles does, done ONCE when the page fills instead
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

    /* `made`, not `tex`: a repack re-lays the ENTRIES and the paints that follow
       it feed the CPU mirror, both of which a lane with no GL name still needs. */
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
    /* the twin's rects moved with them: back to unpainted (job_clear clears
       level 0), and whatever was queued is dropped -- it re-queues as each
       reserved entry is painted. Unlike the recycle this happens once, which
       is what lets the twin converge at all while zoomed out. */
    job_clear_dest(a);

    /* PART 3, the branch that says a second page is the only thing left.
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

    /* THE UPLOAD IS GL; THE PAINT IS THE ATLAS. Every GL call in this function
       is gated and the CPU mirror writes are not: the mirror takes the same
       bytes from the same buffer (see below), so on a lane with no GL name the
       art still reaches a second backend. [The vulkan-only plan, landing 4b-2.] */
    const int gl_draws = !tagpu_vk_owns_present();
    if (gl_draws) {
        glBindTexture(GL_TEXTURE_2D, a->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    /* Re-emit the frame with its outermost row and column repeated all
       round, `pad` deep. The border is what any sampler that reaches past
       the frame must land on: under GL_NEAREST that is the fragment whose
       centre falls exactly on the quad's far edge (its u interpolates to
       exactly u1, and floor(u1*dim) is one texel past the frame) — left
       unwritten that texel is whatever glTexImage2D(NULL) leaves, i.e.
       index 0, a real palette entry (black) rather than the frame's colour
       key, which is the black hairline down the right of every tree at
       zoom 0.25. Under a filtered sampler it is every edge fragment, which
       is why the border is on all four sides and not just the two the
       shelf packer used to leave spare; under a mipmapped one it is the
       whole 4-texel ring (tagpu_gaf.h `pad`). The cell's slack past the
       border, where the alignment rounds up (0..align-1 texels on the
       right and bottom), is filled with the same edge: at level 2 the
       far-edge sample of a frame whose width is 3 mod 4 takes a quarter
       of its weight from the level-2 texel that covers the slack, so
       unwritten slack would darken that column by a sixteenth. The whole
       cell is uploaded, and restore_enqueue has the OUT pass paint the
       twin's slack the same way. */
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
        if (gl_draws)
            glTexSubImage2D(GL_TEXTURE_2D, 0, x - p, y - p, cw, ch,
                            GL_RED, GL_UNSIGNED_BYTE, s_pad);
        /* THE CPU MIRROR TAKES THE SAME BYTES, FROM THE SAME BUFFER, IN THE
           SAME CALL (Phase G / G19e). Not a second copy of the art: the very
           rows the line above hands GL, so a backend that uploads the mirror
           and a backend that samples `tex` cannot disagree about a texel. The
           rect is the cell's, border and alignment slack included, exactly as
           above -- and it is inside the atlas by construction, because the
           shelf packer refused the cell otherwise. */
        if (a->mirror) {
            const int x0 = x - p, y0 = y - p;
            for (k = 0; k < ch; k++)
                memcpy(a->mirror + (size_t)(y0 + k) * a->dim + x0,
                       s_pad + (size_t)k * pw, (size_t)cw);
            a->mirrorSerial++;
        }
    }
    if (gl_draws) glBindTexture(GL_TEXTURE_2D, 0);

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
    if (a->job) restore_enqueue(a, e);
}

/* the insertion shared by atlas_get (which decodes into s_dec first) and
   atlas_put (which is handed the bytes): `pixels` holds w*h indices */
static const TAGPU_GAFENT* atlas_insert(TAGPU_GAFATLAS* a, const void* g, const void* pix,
                                        int w, int h, unsigned char ck, const unsigned char* pixels)
{
    int slot;
    TAGPU_GAFENT* e;
    for (slot = (int)gaf_hash(g); a->hash[slot]; slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) {
        TAGPU_GAFENT* c = &a->ents[a->hash[slot] - 1];
        if (c->frame == g && c->pix == pix && c->w == w && c->h == h) {
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
        e->frame = g; e->pix = pix;
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
    hit = tagpu_gaf_atlas_find(a, g, pix, w, h);
    /* the fast path is the one that runs hundreds of times a frame, so this is
       where an entry proves it is still part of the working set */
    if (hit) { ((TAGPU_GAFENT*)hit)->hit = 1; return hit; }
    /* a frame whose pixels are momentarily unreadable must stay retryable:
       claiming the slot here would cache the failure for the atlas's whole
       life, and the feature atlas is meant to live as long as the map */
    if (!tagpu_gaf_decode(g, w, h, s_dec)) return NULL;
    return atlas_insert(a, g, pix, w, h, g[TAGPU_GF_CK], s_dec);
}

const TAGPU_GAFENT* tagpu_gaf_atlas_put(TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                        int w, int h, unsigned char ck, const unsigned char* pixels)
{
    if (!tagpu_gaf_atlas_create(a)) return NULL;
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX || !pixels) return NULL;
    return atlas_insert(a, frame, pix, w, h, ck, pixels);
}

const TAGPU_GAFENT* tagpu_gaf_atlas_find(const TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                         int w, int h)
{
    int slot;
    if (!a->ents || !a->n) return NULL;
    for (slot = (int)gaf_hash(frame); a->hash[slot]; slot = (slot + 1) & (TAGPU_GAF_HASH - 1)) {
        const TAGPU_GAFENT* c = &a->ents[a->hash[slot] - 1];
        if (c->frame == frame && c->pix == pix && c->w == w && c->h == h)
            return c->ok ? c : NULL;
    }
    return NULL;
}
