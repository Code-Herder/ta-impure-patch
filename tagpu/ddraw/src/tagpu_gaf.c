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
/* `opengl_utils.h` was here until 11-5e-2b. This file named `glReadPixels`
   through it and nothing else; with the restored twin's read-back gone it
   names no GL type, constant or entry point at all. `tagpu_restoreglsl.h`
   below is NOT GL -- it declares `TAGPU_RGLSL_FRAME` and
   `tagpu_rglsl_tileable`, both of which this file still uses. */
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

/* THIS FILE NAMES NO GL ENTRY POINT AND MAKES NO GL CALL (11-5e-2b). It held
   the last one: `glReadPixels`, typedef'd `PFN_READPIXELS` and resolved by
   `getgl` -- `xwglGetProcAddress` first, then `GetModuleHandleA("opengl32.dll")`
   -- behind a `fetch_gl` that ran once. Its only consumer was
   `tagpu_gaf_atlas_mirror_rgb`, the restored twin's read-back, which went with
   the last lane that published its texels. That `xwglGetProcAddress` was this
   file's single remaining GL call site, so the file is GL-free now on the
   narrow AND the wide count (`tools/gl-sites.py`).
   [The mip-generation and float-parameter typedefs that stood here went in
   11-5e-2 with `twin_mips`, and the GL_TEXTURE_MAX_LEVEL /
   GL_TEXTURE_MAX_ANISOTROPY_EXT fallbacks and the TWIN_ANISO alias with the
   `glTexParameter` calls that were their only users.] */

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
   ONE BUILDER ON PURPOSE: the whole claim of the published list is that its
   frames are exactly the frames this atlas painted, so they are built here
   rather than by a second piece of code that agrees today. (Until 11-5e-2
   the second reader was the GL restorer's queue, which is what "shared" meant
   here; the list is now the only one, and the rule is why it stays one
   function.) */
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

/* THE DESTINATION HAS JUST BEEN ZEROED AND THE MIRROR HAS TO SAY SO.
   `job_clear_dest` empties the published list and drops the destination, and
   a CLEAR IS NOT A PAINT: the twin's generation does not move for it and the
   shelf gets SMALLER rather than larger -- so not one of the things
   `tagpu_gaf_atlas_mirror_rgb_step` keys on can see it, and a mirror left
   alone would hold the previous fill's colours over a destination that is now
   empty. The next entry re-laid into those rects then draws restored in one
   lane and indexed in the other until its repaint lands.
   This is the third time on this pass that a key which was not the CONTENT's
   key has been wrong, so the zeroing lives HERE, in one function beside the
   call it mirrors, rather than at each site.
   [The GL job this used to name went in 11-5e-2; the caller did not, which is
   why the rule still holds and only the mechanism was reworded.] */

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
    /* THE MIRROR THAT ALSO HAD TO BE ZEROED HERE IS GONE (11-5e-2b), and the
       list is the whole of this function now. Until 11-5e-2 the zeroing sat
       under `if (!a->job) return;` -- the GL restorer's job -- so a build with
       no GL restorer cleared the published list and left the read-back mirror
       holding the old layout's colours; 11-5e-2 unpinned it and 11-5e-2b
       removed the mirror itself. The rule the function was written to state is
       untouched: the rects this atlas hands out have just moved, so a
       consumer's own destination is wrong whatever any GL object is doing. */
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

/* ---- THE RESTORED TWIN'S MIRROR IS GONE (11-5e-2b) ----------------------
   `tagpu_gaf_atlas_mirror_rgb` armed it -- a `dim x dim` RGBA8 calloc and an
   FBO -- and `tagpu_gaf_atlas_mirror_rgb_step` filled it with `glReadPixels`
   off that FBO once per published frame, so a second backend could be handed
   the texels the GL restorer had painted. `oglu_load_dll` has no caller, so
   opengl32.dll is never in the process and the arm refused at its entry-point
   guard BEFORE its own calloc: `a->mirrorRgb` was NULL for the life of every
   process, and every consumer's `atlasRgb` with it. The four world lanes that
   read it moved to the published frame LIST in 11-5e-2b part 1; the UI, the
   fifth, had no list to move to and lost the route entirely (tagpu_gui.h).
   The `mirrorRgb*` fields went with these two functions. */

/* THE RESTORED TWIN IS THE WHOLE MIP CHAIN, not level 0 alone, and the reason
   is measurable. (The MIRROR this argued for is gone with the read-back in
   11-5e-2b; the three functions below stay, because the LAYOUT is still the
   contract between the producer's chain and the Vulkan restorer's dump --
   `tagpu_vk_restore.c` calls `_off` and `_chain`.) The restored twin is mipped
   and sampled
   LINEAR_MIPMAP_LINEAR to its top level, so a consumer holding only level 0
   draws a different picture wherever the art is minified. (The GL producer
   that made it that way, `tagpu_gaf_atlas_restore`, went in 11-5e-2 -- the
   MEASUREMENT below is a fact about mip chains and is unaffected, and the
   shape it argues for is what the Vulkan lane must reproduce.) On the unit
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

/* Arm the published restore list (tagpu_gaf.h). It used to ALSO stand the
   read-back down, which is what "and never both" meant; there is no read-back
   to stand down since 11-5e-2b, so this arms the only route there is.
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
    /* THE READ-BACK'S 16 MB WAS GIVEN BACK HERE, and there is no read-back to
       give back since 11-5e-2b. The mirror was documented as never freed --
       an atlas has no destructor and a re-arm should find it already correct
       -- and this was the one exception, because the list is not a second
       consumer of the mirror but its replacement. Now it is the only thing
       there ever was. */
    _snprintf(b, sizeof b, "%s: restorevk -- the restore is the other lane's to run, so"
              " no read-back and the frame list is published instead (%d entries seeded,"
              " %d-frame bound)", a->tag ? a->tag : "gaf", a->rlistN, rlist_cap(a));
    b[sizeof b - 1] = 0;
    glog(b);
    return 1;
}

/* GIVE BACK THE HEAP BUFFER THIS FUNCTION OWNS, for a caller that is about to
   lay the struct out again from zero. `mirror` (dim*dim) is the one it frees.
   `mirrorRgb`, the restored twin's mip chain, was the second until 11-5e-2b.

   IT IS NOT THE ONLY ALLOCATION IN A TAGPU_GAFATLAS, AND SAYING SO WOULD BE
   THE TRAP. `rlist` is malloc'd at the arm (`:526`) and grown by `rlist_room`,
   and this function does not touch it -- which is safe TODAY for a reason that
   is a property of the CALLER, not of this function: the only caller is
   `tagpu_gui_surf.c`'s `atlas_setup`, that is the GUI atlas, and the GUI never
   calls `tagpu_gaf_atlas_restore_vk`, so its `rlist` is NULL for the life of
   the process. `atlas_setup` then memsets the struct, so the day the UI is
   given a published list -- the gap 11-5e-2b part 2 names and 11-5e-2c owns --
   that memset drops a live pointer on every re-arm. WHOEVER ARMS A LIST FOR
   THE UI OWNS THIS: either free `rlist` here with its three fields, or give
   `atlas_setup` a path that does not zero over it. Written down rather than
   fixed in passing, because `rlist`'s lifetime is 11-5e-2c's subject and a
   half-fix here (free the buffer, leave `rlistWant`/`rlistFailed` latched)
   is the shape of bug this gate keeps finding. [11-5e-2b part 2's review, F4.]

   `mirror` is not freed by `_lost`, which keeps it
   deliberately so that a mirror is never stale for the frames between a
   context loss and the next create. A caller that re-arms by zeroing the
   struct would therefore drop the pointer and leak it; every writer already
   guards on the pointer and `_mirror` re-arms on demand, so handing it back
   here costs nothing the memset was not already costing functionally.
   [FROM THE 4b-3 LANDING REVIEW.] */
void tagpu_gaf_atlas_free_buffers(TAGPU_GAFATLAS* a)
{
    if (!a) return;
    free(a->mirror);    a->mirror = NULL;
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
    /* `rgb` and `restoreFailed` outlived the GL restorer that set them: both
       are 0 for the life of the process since 11-5e-2 deleted the only writer
       (`tagpu_gaf_atlas_restore`). Cleared here anyway, because this function's
       contract is "the struct describes nothing that exists" and a field left
       alone on the strength of an argument made elsewhere is how the next
       landing gets a stale value. */
    a->rgb = 0; a->restoreFailed = 0;
    /* THE OTHER LANE'S DESTINATION DID NOT DIE -- ITS SOURCE DID. Nothing here
       is a Vulkan object, so a consumer's twin still holds the colours of an
       atlas whose every entry has just been dropped. The generation is what
       tells it to blank and start over; without this it would keep painting
       the old layout's rects for the rest of the session. */
    rlist_reset(a, 0);
    /* THE READ-BACK'S FBO AND ITS BYTES WENT IN 11-5e-2b. The FBO was
       forgotten here rather than deleted, exactly as `tex` is above -- deleting
       a name from a context that is gone either does nothing or destroys a
       live object of the NEW one handed the same number -- and the mirror's
       bytes were zeroed with the twin they mirrored. There is no twin, no FBO
       and no mirror now. */
    /* the entries went with the texture, so the wall the last fill hit says
       nothing about the next one */
    a->repackWall = 0;
}

/* tagpu_restoredump.on: the twin as the shader samples it, once per fill of
   the atlas, as raw bytes -- the restorer's only disk write, and only under
   the trigger (the terrain pass writes its own). Three files per atlas:
   tagpu_restore_<tag>.r8 (the source, dim x dim), .rgba (the twin, dim x dim
   x 4) and .idx (a line per entry: x y w h key wrap), so `tascene featdiff`
   can find each frame in both and hold the twin to the lab's bar. */
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
       and the Vulkan twin uploads the mirror. `a->tex` is set to 0 rather than
       left alone so that a re-created atlas cannot carry a stale name, and it
       is 0 for the life of the process: the `glGenTextures` that used to fill
       it went in landing 11-4c. `a->made`, not `a->tex`, is what says the
       layout exists -- keying it on the name once produced a refusal that read
       downstream as `atlas=0` and no sprite texels at all.
       [The vulkan-only plan, landings 4b-2 and 11-4c.] */
    /* A FRESH ATLAS IS A FRESH MIRROR, AND THE MEMSET IS WHAT MAKES IT INDEX 0
       -- the assumption the border comment in `atlas_paint` rests on. A mirror
       that kept the previous atlas's texels would be a copy of something that
       no longer exists. atlas_drop is deliberately NOT here: it leaves the
       texels alone and lets re-inserted entries overwrite them, and the mirror
       follows exactly because it follows the paints. */
    if (a->mirror) { memset(a->mirror, 0, (size_t)a->dim * a->dim); a->mirrorSerial++; }
    a->tex = 0;
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

    /* THE PAINT IS THE ATLAS, AND THE MIRROR IS THE PAINT. This function used
       to upload each cell to a GL texture beside the mirror write below; the
       upload went in landing 11-4c and the mirror write is now the only
       destination the art has. Nothing here is gated on a backend, and nothing
       may become so: a cell that is packed but not mirrored is a hole the
       Vulkan twin samples as index 0.
       [The vulkan-only plan, landings 4b-2 and 11-4c.] */
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
       cell is uploaded, and the frame published for the other lane's
       restore covers the same rect, so its OUT pass paints the slack the
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
           OUT OF `s_pad` ITSELF (Phase G / G19e). Not a second evaluation of
           the art: the very rows those loops wrote, so there is nothing for a
           second pass over the source to drift from. It was written beside a
           GL upload of the same buffer, which is why it is phrased as a mirror;
           since landing 11-4c it is the only copy, and DELETING IT DELETES THE
           SPRITE. The rect is the cell's, border and alignment slack included,
           exactly as above -- and it is inside the atlas by construction,
           because the shelf packer refused the cell otherwise. */
        if (a->mirror) {
            const int x0 = x - p, y0 = y - p;
            for (k = 0; k < ch; k++)
                memcpy(a->mirror + (size_t)(y0 + k) * a->dim + x0,
                       s_pad + (size_t)k * pw, (size_t)cw);
            a->mirrorSerial++;
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
    /* THE PUBLISHED LIST IS NOT FED HERE, AND THAT IS A KNOWN DEFECT RATHER
       THAN A DESIGN. This line read `if (a->job) restore_enqueue(a, e);` --
       the GL restorer's job -- and `a->job` has been NULL for the life of the
       process since landing 11-4c took the `glGenTextures` that filled
       `a->tex` out of `tagpu_gaf_atlas_create`: the predicate that read as
       "the GL restorer is running" had already become "the GL restorer can
       never run". So the other lane's list is SEEDED ONCE by
       `tagpu_gaf_atlas_restore_vk` and never fed again, and `job_clear_dest`
       empties it on any recycle or repack: after the first reset it stays
       empty and every frame inserted afterwards stays indexed on the consumer
       for the rest of the session. Measured 2026-09-19 with a probe on
       `a->rlistWant`: with `tagpu_restorevk.on` armed, feat and fx both report
       `restoring the atlas HERE - 0 of 0 frames` and zero queue drains.

       THE ONE-LINE FIX IS NOT SAFE AND WAS TAKEN BACK OUT. Enqueueing here
       makes `a->rlist` mutable during a paint, and `tagpu_posedraw.c`'s
       `pd_view_publish` captures the RAW POINTER on the FIRST posedraw window
       of the frame, while `tagpu_native.c`'s `ghost_record` runs AFTER it and
       reaches this function through `tagpu_r3d_atlas_uv` -> `atlas_get` on a
       build ghost whose texture is not yet atlased. `rlist_room` would then
       `realloc` -- or, out of memory, `free` -- the buffer the render thread
       is about to read. The feat and fx atlases escape only because their
       publication happens to be the last write of their frame; the unit atlas
       does not, and nothing enforces that ordering.

       WHAT THE DELETION LEAVES IS STRONGER THAN WHAT IT FOUND. With the feed
       gone, `rlist_room` is reachable only from `rlist_restart`, and
       `rlist_restart` only from `tagpu_gaf_atlas_restore_vk`, which latches on
       `a->rlistWant` and runs once per atlas per session -- before anything
       has published a non-NULL pointer. So `a->rlist` is assigned exactly
       once and is neither moved nor freed for the life of the atlas: a
       lifetime the code enforces, where before it rested on `a->job` being
       NULL for a reason stated three files away.

       RESTORING THE FEED THEREFORE COSTS A BOUND FIRST, and that is a landing
       with its own measurement and its own review, not a line here:
         - the BOUND: allocate `rlist_cap(a)` frames once in the arm and make
           `rlist_room` a pure bounds test, so the address stays constant with
           the list live. At `sizeof(TAGPU_RGLSL_FRAME)` that is 0.36 MB for
           the unit atlas and 0.72 MB for feat, against a read-back mirror
           this file already declines to free at 16 MB.
         - and an ORDERING for the restart, which rewrites the array in place
           and so is not covered by the bound: take the unit list in
           `tagpu_posedraw_handover`, after every paint of the frame, rather
           than at `pd_begin`.
       [The vulkan-only plan, 11-5e-2's review; the feed is 11-5e-2c.] */
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
