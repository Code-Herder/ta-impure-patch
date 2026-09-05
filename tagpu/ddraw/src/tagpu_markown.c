/* tagpu_markown.c — the fourteen call-site redirects, the prologue detour and
   the one capture window that is left. See tagpu_markown.h for what this owns,
   what became a re-draw, and why the pre-fog window is gone. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_markown.h"
#include "tagpu_order.h"
#include "tagpu_text.h"
#include "tagpu_terr.h"
#include "tagpu_native.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"

#define TA_MAINPP    0x00511DE8u

/* ---- the sites we redirect, and what they call ---- */
#define SITE_HOOK8_VA    0x00469BD7u   /* call 0x471F90(ctx, 8)  — before markers */
#define SITE_HOOK9_VA    0x00469D2Cu   /* call 0x471F90(ctx, 9)  — after them     */
#define SITE_TRANSP1_VA  0x00469EC5u   /* call 0x4BF8C0(ctx, r, c) — outer rect   */
#define SITE_TRANSP2_VA  0x00469F1Eu   /* call 0x4BF8C0(ctx, r, c) — inner rect   */
#define SITE_SELBOX1_VA  0x004699EBu   /* call 0x46A530(ctx, unit) — ground sweep  */
#define SITE_SELBOX2_VA  0x00469B8Au   /* call 0x46A530(ctx, unit) — air sweep     */
#define LEAF_SELBOX_VA   0x0046A530u   /* DrawUnitSelectBoxRect, stdcall, ret 8   */
#define PASS_SFX_VA      0x00471F90u   /* particle layer draw, stdcall, ret 8     */
#define PASS_TRANSP_VA   0x004BF8C0u   /* DrawTranspRectangle, stdcall, ret 0x0C  */
#define LEAF_BARS_VA     0x0046A430u   /* DrawHealthBars, stdcall, ret 0x10       */
/* The group digit: the only text the marker block draws, one character
   ('0' + unit+0xAC) at the unit's feet. stdcall(ctx, str, x, y, maxWidth),
   ret 0x14 — the same leaf the ShowRanges labels go through, but this is its
   only call site inside the block, so redirecting the SITE leaves the labels
   (which are reached from inside the order driver we already skip) alone. */
#define SITE_DIGIT_VA    0x00469CF9u
#define LEAF_TEXT_VA     0x004C14F0u   /* DrawTextCustomFont, stdcall, ret 0x14   */
/* The target-sprite drawer and its only two `E8` callers (`0x4394E0` delegates
   to it, `0x439B30` dispatches bit 3 to it); stdcall(ctx, view, node, pos,
   flag), ret 0x14. Both sites are redirected for the trace and nothing else
   now — the identity blend LUT they used to bracket went with the pre-fog
   capture window (tagpu_markown.h), because the sprite no longer has a buffer
   of ours to composite against. Its address is ALSO in `.rdata` 19 times, as
   the `+8` field of the 25-byte order-descriptor records — a field this build
   never reads, so the two redirects are the whole path today and would be
   bypassed silently if it were ever brought into use. [BINARY-VERIFIED] */
#define SITE_TSPRITE1_VA 0x00439516u
#define SITE_TSPRITE2_VA 0x00439C7Du
#define LEAF_TSPRITE_VA  0x00439740u

/* The order-marker driver and its sole call site, behind the SHIFT probe at
   `0x469BE1`. Redirecting the SITE rather than detouring the driver is what
   lets the snapshot run on the game thread with the driver's own arguments in
   hand — see tagpu_order.h for why the walk cannot happen on the present
   thread at all. */
#define SITE_ORDERS_VA   0x00469BFCu   /* call 0x48CC30(ctx, main+0x142F3)   */
#define PASS_ORDERS_VA   0x0048CC30u
/* The walker's three remaining leaf call sites. They are redirected only so
   `order.on=trace` can log the ENGINE's node list beside ours; with trace off
   each is a call straight through. (Bits 3 and 1's delegation to it already
   come through mark_tsprite.) */
#define SITE_DBUILD_VA   0x00439BACu   /* call 0x438C00 — build site, bit 0  */
#define SITE_DDOTS_VA    0x00439BF2u   /* call 0x4394E0 — route dots, bit 1  */
#define SITE_DCIRC_VA    0x00439C37u   /* call 0x4399F0 — target circle, b2  */
#define SITE_DRANGE_VA   0x00439CBEu   /* call 0x4390A0 — range circles, b4  */
#define LEAF_DBUILD_VA   0x00438C00u
#define LEAF_DDOTS_VA    0x004394E0u
#define LEAF_DCIRC_VA    0x004399F0u
#define LEAF_DRANGE_VA   0x004390A0u

/* `83 EC 10 | 53 | 55` = sub esp,0x10; push ebx; push ebp — five
   position-independent bytes ending on an instruction boundary (0x46A435) */
static const unsigned char BARS_STOLEN[5] = { 0x83, 0xEC, 0x10, 0x53, 0x55 };

/* engine layout (ui-markers.md appendix, terrain-depth.md 4)
   OFFSCREEN: +0x08 pitch, +0x0C pixel base, inclusive clip rect +0x1C..+0x28 */
#define CTX_PITCH    2
#define CTX_BASE     3
#define CTX_CLIP_L   7
#define CTX_CLIP_T   8
#define CTX_CLIP_R   9
#define CTX_CLIP_B   10
#define CTX_FIELDS   11          /* how much of it we read                    */

volatile unsigned char g_markown_skipBars = 0;

static int g_installed = 0;
static int g_capture = 0;                 /* follows tagpu_mark.on           */
static int g_selbox = 0;                  /* ours redraws the selection rect */
static int g_cursor = 0;                  /* ours redraws the build cursor   */
static int g_orders = 0;                  /* ours redraws the order markers  */
static int g_digits = 0;                  /* ours redraws the group digit    */
static unsigned g_beat = 0, g_last = 0;

/* One layer, double-buffered — the post-fog build-cursor window, and only
   under `tagpu_mark.on=nocursor`. The game thread fills one buffer while the GL
   thread may still be uploading the other: a single buffer would let a present
   catch the key-fill half-done and show a frame with the top of every marker
   missing, which reads as flicker. Two buffers and a published pointer cost
   one more allocation and remove the whole class.

   The publication is therefore only ever REPLACED, never emptied and refilled.
   That distinction is the whole point and it is not academic: the engine runs
   this draw block far more often than we present — measured at ~83 blocks per
   presented frame on a live skirmish, because everything else in its frame is
   skipped and ours is the slow half — so anything the game thread leaves the
   published slot holding for the length of one capture is what roughly a tenth
   of all presents will read. Clearing at the start of a capture cost exactly
   that: 13 presents in 120 with SHIFT held showed no order markers at all.

   TWO buffers are enough only because the reader outruns them, and that was
   measured rather than assumed. The writer alternates slots, so the buffer the
   GL thread is uploading is reclaimed two publications later — about 0.4 ms at
   the rate above. Instrumented for the case that matters (the slot about to be
   key-filled is the one the reader still holds): 0 in ~50 000 publications at
   1024x768. It is the upload finishing inside two of the engine's blocks that
   keeps this true, so it is the thing to re-measure if the layer ever grows far
   faster than the block does. */
typedef struct {
    unsigned char* buf[2];
    unsigned char* retired[2];            /* the pair before the last growth */
    int   bytes;                          /* size of each buffer             */
    int   which;                          /* the one being written           */
    int   active;                         /* base is currently swapped out   */
    int*  ctx;                            /* whose base we swapped           */
    unsigned char* saved;                 /* what it held                    */
    int   opened;                         /* this frame's fill actually ran  */
    int   tried;                          /* ...and whether it was attempted */
    /* Published for the GL side, which reads it on the present thread. TWO
       slots and an index rather than one struct: the geometry and the pointer
       have to change together, and a single copy lets a reader pair a fresh
       `pix` with the previous resolution's rect. The writer fills the slot the
       index does NOT name, then moves the index. */
    TAGPU_MARKLAYER desc[2];
    volatile int    pub;                  /* published slot, -1 = nothing    */
    int             pend;                 /* the slot this window is filling */
} LAYER;

static LAYER g_L[TAGPU_MARK_NLAYER];

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

int tagpu_markown_installed(void) { return g_installed; }
int tagpu_markown_key(void) { return tagpu_terr_key(); }

int tagpu_markown_layer(int i, TAGPU_MARKLAYER* out)
{
    LAYER* L;
    int slot;
    if (i < 0 || i >= TAGPU_MARK_NLAYER || !out) return 0;
    L = &g_L[i];
    slot = L->pub;
    if (slot != 0 && slot != 1) return 0;
    *out = L->desc[slot];
    return out->pix && out->w > 0 && out->h > 0;
}

/* ---- the capture itself ---------------------------------------------- */

static void layer_clear(LAYER* L);

/* Point the context's pixel base at our scratch and, on the first call of a
   frame, key-fill the viewport rect of it first. Returns 0 (and leaves the
   engine's frame alone) for any context we cannot validate — a refused capture
   just means the engine keeps drawing this block into its own frame, which is
   the pre-G13d behaviour and always safe. */
static int layer_begin(LAYER* L, int* ctx, int fresh)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    unsigned char* buf;
    int pitch, cl, ct, cr, cb, x0, y0, x1, y1, need, y;

    if (L->active) return 1;                        /* already ours          */
    if (!ptr_ok(ctx) || IsBadReadPtr(ctx, CTX_FIELDS * 4) || !ptr_ok(ta)) return 0;
    pitch = ctx[CTX_PITCH];
    if (pitch <= 0 || pitch > 16384) return 0;
    if (!ptr_ok((void*)(size_t)(unsigned)ctx[CTX_BASE])) return 0;
    cl = ctx[CTX_CLIP_L]; ct = ctx[CTX_CLIP_T];
    cr = ctx[CTX_CLIP_R]; cb = ctx[CTX_CLIP_B];
    /* the same validation terrown's fill does: the OFFSCREEN carries no buffer
       HEIGHT, so its inclusive clip rect is the only bound we have — and it is
       what sizes our scratch, since every blit below is clipped to it */
    if (!(cl >= 0 && ct >= 0 && cr >= cl && cb >= ct && cr < pitch && cb < 8192))
        return 0;
    need = pitch * (cb + 1);

    if (!fresh) {
        /* the second half of a double-outlined rect: keep the buffer, the fill
           and the geometry the first call set up */
        if (!L->opened || pitch != L->desc[L->pend].pitch || need > L->bytes)
            return 0;
        L->ctx = ctx;
        L->saved = (unsigned char*)(size_t)(unsigned)ctx[CTX_BASE];
        ctx[CTX_BASE] = (int)(size_t)L->buf[L->which];
        L->active = 1;
        return 1;
    }

    /* The ADDRESSABLE rect, not the true one: at zoom < 1 tagpu_vpwide widens
       what the engine can name, its own drawers then reach past the 1x edge,
       and the replay maps every captured pixel back through the zoom anyway
       (layer_quad projects from the TRUE vpL/eye, so a capture at an engine
       position outside the 1x viewport lands where it belongs on screen).
       Capturing only the 1x rect would throw the ring's markers away again.
       The clip intersection below is what keeps it inside our scratch. */
    {
        int w, h;
        if (!tagpu_vpwide_addressable(&x0, &y0, &w, &h))
            tagpu_vpwide_true_rect(ta, &x0, &y0, &w, &h);
        x1 = x0 + w;
        y1 = y0 + h;
    }
    if (x0 < cl) x0 = cl;
    if (y0 < ct) y0 = ct;
    if (x1 > cr + 1) x1 = cr + 1;
    if (y1 > cb + 1) y1 = cb + 1;
    if (x1 <= x0 || y1 <= y0) return 0;

    if (need > L->bytes) {
        /* NOT realloc. It moves the block, and the GL thread may be part-way
           through a glTexSubImage2D out of it with no handshake to wait on —
           dropping the publication first would only narrow that window, not
           close it, because the reader has already copied the pointer out.
           Allocate a new pair and RETIRE the old one for a generation instead:
           `need` only grows when the engine's surface pitch does, i.e. on a
           resolution change, so this holds at most one spare pair and frees it
           the next time round, by which point no reader can still be in it. */
        unsigned char* a = (unsigned char*)malloc((size_t)need);
        unsigned char* b = a ? (unsigned char*)malloc((size_t)need) : NULL;
        if (!a || !b) {
            free(a); free(b);
            flog("markown: capture buffer alloc failed");
            return 0;                     /* buffers untouched; retry next call */
        }
        free(L->retired[0]); free(L->retired[1]);
        L->retired[0] = L->buf[0]; L->retired[1] = L->buf[1];
        L->buf[0] = a; L->buf[1] = b;
        L->bytes = need;
        /* both descriptors now name pixels in the retired pair */
        layer_clear(L);
    }

    /* The slot the GL side is NOT reading, chosen BEFORE the fill and
       REMEMBERED for layer_end: deriving it again there would pick the wrong
       one if layer_clear ran on the other thread in between. Buffer index and
       descriptor slot are the same number by construction, so this KEY FILL can
       never land in the buffer the published descriptor names, and one index
       cannot drift from the other. (The non-fresh path above is the deliberate
       exception — see mark_transp.) */
    L->pend = (L->pub == 0) ? 1 : 0;
    L->which = L->pend;
    buf = L->buf[L->which];

    /* Only the viewport rect is filled, and only the viewport rect is ever
       uploaded: a bar near the edge can spill past it under the context's own
       clip, but in the engine the side panel is blitted over that spill a few
       hundred instructions later, so not drawing it is what parity means. */
    {
        int k = tagpu_markown_key();
        for (y = y0; y < y1; y++)
            memset(buf + (size_t)y * pitch + x0, k, (size_t)(x1 - x0));
    }

    L->ctx = ctx;
    L->saved = (unsigned char*)(size_t)(unsigned)ctx[CTX_BASE];
    ctx[CTX_BASE] = (int)(size_t)buf;
    {
        TAGPU_MARKLAYER* d = &L->desc[L->pend];
        d->pix = NULL; d->pitch = pitch;
        d->x = x0; d->y = y0; d->w = x1 - x0; d->h = y1 - y0;
    }
    L->active = 1;
    L->opened = 1;
    return 1;
}

/* Give the engine its frame back and publish what was drawn. `publish` is 0
   for a window we opened and then decided held nothing. */
static void layer_end(LAYER* L, int publish)
{
    int slot;
    if (!L->active) return;
    L->ctx[CTX_BASE] = (int)(size_t)L->saved;
    L->active = 0;
    L->ctx = NULL; L->saved = NULL;
    slot = L->pend;
    L->desc[slot].pix = publish
        ? L->buf[L->which] + (size_t)L->desc[slot].y * L->desc[slot].pitch
                           + L->desc[slot].x
        : NULL;
    L->pub = slot;                    /* moved last: the descriptor is whole */
}

/* Publish "nothing here" without touching the context, and WITHOUT claiming a
   slot — a flip here could hand layer_end the descriptor of an older frame.
   One volatile int, safe from either thread; layer_end is not, which is why
   nothing but the game thread calls it.

   Call it only for a frame that has DECIDED it has nothing — never to open a
   capture with. A capture that is about to publish leaves the last publication
   standing until it has a whole new one to put in its place; that is what the
   second buffer is for. */
static void layer_clear(LAYER* L) { L->pub = -1; }

/* ---- the redirected call sites --------------------------------------- */

/* hook 8: the layer-8 particle draw runs FIRST, into the engine's own frame
   (those particles belong to tagpu_sfx, not to us). What remains here is
   bookkeeping for the two things that need to know a marker block has STARTED
   — the post-fog window's "was there anything last time", and the order
   arena's — plus the font latch the text pass needs taken at this instant. */
static void __stdcall mark_hook8(void* ctx, int n)
{
    LAYER* P = &g_L[TAGPU_MARK_POSTFOG];

    ((void (__stdcall *)(void*, int))PASS_SFX_VA)(ctx, n);

    /* The engine's own font and text colour, as of the start of the block that
       draws the group digit and the ShowRanges labels. Taken here rather than
       read from the present thread because `SetFont 0x4C1420` runs many times a
       frame and nothing between this hook and `0x469CF9` calls it
       (tagpu_text.h). */
    tagpu_text_snapshot();

    /* The post-fog window is per-call and every one of its calls is still ahead
       of us in this frame, so this is where its frame starts. Its "nothing to
       show" is only knowable in arrears — no DrawTranspRectangle came — so it
       is decided here, about the frame that just ended, rather than by clearing
       on the way in and hoping a call arrives before the next present. The cost
       is a one-block ghost — the block in which the drag ends still carries the
       last rect — against a hole that was most of a frame wide. */
    if (!P->opened) layer_clear(P);
    P->tried = 0;
    P->opened = 0;

    /* and the order arena's own block: the snapshot only runs when the SHIFT
       gate at `0x469BE1` opens, so "no snapshot in this block" is the only
       signal that the markers should come off the screen */
    tagpu_order_block_begin();
}

/* hook 9: the end of the marker block, and the only thing left that needs it
   is the order arena. Every in-game frame reaches this hook — the one branch
   that does not is drawUnits == 0, which only TA's movie recorder passes
   (`0x469C03 je 0x469D38` lands PAST it; the recorder is `0x4962C2`, function
   `0x495E88`, `xor ebx,ebx` at `0x495EA1`) [BINARY-VERIFIED] — and `opens ==
   ends` over ~50 000 blocks of live play said so while the capture still used
   it. */
static void __stdcall mark_hook9(void* ctx, int n)
{
    tagpu_order_block_end();
    ((void (__stdcall *)(void*, int))PASS_SFX_VA)(ctx, n);
}

/* the build-cursor footprint and the drag band box: two consecutive calls that
   make one double-outlined rect, so the key fill happens on the first of them
   and both land in the same buffer. */
static void __stdcall mark_transp(void* ctx, void* rect, int colour)
{
    LAYER* L = &g_L[TAGPU_MARK_POSTFOG];
    int opened = 0;
    /* OURS DRAWS BOTH OF THIS BLOCK'S PRIMITIVES, so the engine's pair is not
       drawn at all — capturing them could never carry them into the outer ring.
       The engine clips these rects to the OFFSCREEN, which is screen-sized,
       while the position it projects them at is the addressable coordinate
       `vpwide` widened to; at zoom < 1 the two disagree and the whole rect is
       clipped away long before our buffer sees it. Skipping is also what stops
       an engine-drawn rect standing in our frame as a ghost at the unzoomed
       position, exactly as for the selection rect above. */
    if (g_cursor) return;
    if (g_capture) {
        /* `tried` and `opened` are separate on purpose: if the FIRST of the two
           calls is refused (a clip rect that will not validate, an allocation
           that failed) the second must not then take the fresh path, flip the
           buffer and re-key-fill — that would drop the outer rect and publish
           only the inner one. A refused first call means this frame's cursor
           stays the engine's, whole. */
        /* The second call continues into the buffer the first one PUBLISHED,
           so a present landing between them uploads the outer rect without the
           inner: one frame of single-outlined cursor. Publishing once, after
           both, would need layer_end split into "give the context back" and
           "swap the descriptor in" — worth it if the cursor is ever seen to
           thin out, not before. It is strictly smaller than what it replaced,
           which showed NO cursor from hook 9 until the first of these calls. */
        int fresh = !L->tried;
        L->tried = 1;
        opened = layer_begin(L, (int*)ctx, fresh);
    }
    ((void (__stdcall *)(void*, void*, int))PASS_TRANSP_VA)(ctx, rect, colour);
    if (opened) layer_end(L, 1);
}

/* The selection rectangle is the one marker the engine draws INSIDE the unit
   sweeps, per unit and immediately before that unit's own sprite — so it cannot
   be bracketed with the block above, and the native pass has always re-drawn it
   (it would otherwise be buried under our pixels). What was left over is that
   the ENGINE still drew its own into the frame underneath, where at 1x it lands
   on exactly the same pixels and is invisible, and at any other zoom becomes a
   ghost box at the unzoomed position.

   The test is per unit rather than a flat flag: the native pass draws a box for
   exactly the units it owns (`native.on`'s type filter), so anything it does not
   own must keep the engine's. And it is ALSO gated on that pass having actually
   managed to draw every box it owed last frame — it can come up short on the
   gather cap, the vertex budget or an unresolvable model AABB, and a suppressed
   box nobody drew leaves a selected unit unmarked. */
static void __stdcall mark_selbox(void* ctx, void* unit)
{
    if (g_selbox && tagpu_native_selbox_complete() &&
        tagpu_native_owns_unit((const char*)unit)) return;
    ((void (__stdcall *)(void*, void*))LEAF_SELBOX_VA)(ctx, unit);
}

/* The waypoint star. The trace tap for capability bit 3; its two call sites get
   separate stubs only so the diff can tell bit 3 from bit 1's unconditional
   delegation to the same drawer (logged as bit 5).

   This used to bracket the call with an identity blend LUT, because the star
   alpha-composites through `table[(src<<8)|dst]` and, captured into a buffer of
   ours, read our fill key as its destination and came out teal instead of
   olive. With the pre-fog capture gone the star only ever composites against
   the ENGINE's own frame again — which is what stock does — so there is nothing
   left to correct, and the 64 KB table and its scoped pointer swap went with
   it. Under `passive` and `trace`, where the engine draws its own markers into
   its own surface and we composite that surface, the star is stock's blend
   against a key-filled viewport: teal, exactly as it was before G13o, and a
   debug mode's business rather than the shipped frame's. */
static void tsprite(int bit, void* ctx, void* view, void* node,
                    void* pos, int flag)
{
    tagpu_order_trace_drawer(bit, node, (const int*)pos, flag);
    ((void (__stdcall *)(void*, void*, void*, void*, int))LEAF_TSPRITE_VA)
        (ctx, view, node, pos, flag);
}

static void __stdcall mark_tsprite(void* ctx, void* view, void* node,
                                   void* pos, int flag)
{
    tsprite(3, ctx, view, node, pos, flag);
}

static void __stdcall mark_tsprite_dot(void* ctx, void* view, void* node,
                                       void* pos, int flag)
{
    tsprite(5, ctx, view, node, pos, flag);
}

/* THE ORDER MARKERS. The stub runs the snapshot on the game thread with the
   driver's own arguments, and skips the engine's driver entirely when that
   snapshot is complete and ours is the one drawing. `passive` and `trace`
   both make the snapshot return 0, so the engine draws its own beside ours.

   Skipping rather than capturing is the whole point: the engine's drawers clip
   to the OFFSCREEN's own width and height, and the offscreen is screen-sized
   while `vpwide` lets the projection reach far outside it, so at zoom < 1 a
   captured marker layer stops dead at the surface bound and the outer ring is
   bare (tagpu_order.h). */
static void __stdcall mark_orders(void* ctx, void* view)
{
    /* The snapshot runs FIRST and unconditionally — `passive` and `trace` both
       need it to publish (trace logs our node list beside the engine's) and
       both make it return 0, which is how the engine keeps the draw. `g_orders`
       is the separate question of whether the present thread has actually taken
       the markers over yet; until it has, the engine draws them. */
    if (tagpu_order_snapshot(ctx, view) && g_orders) return;
    ((void (__stdcall *)(void*, void*))PASS_ORDERS_VA)(ctx, view);
}

/* The three remaining leaf call sites inside the walker. They exist for the
   trace and nothing else; with `order.on=trace` off, each is one compare and a
   call through. */
static void __stdcall mark_dbuild(void* ctx, void* view, void* node,
                                  void* pos, int flag)
{
    tagpu_order_trace_drawer(0, node, (const int*)pos, flag);
    ((void (__stdcall *)(void*, void*, void*, void*, int))LEAF_DBUILD_VA)
        (ctx, view, node, pos, flag);
}

static void __stdcall mark_ddots(void* ctx, void* view, void* node,
                                 void* pos, int flag)
{
    tagpu_order_trace_drawer(1, node, (const int*)pos, flag);
    ((void (__stdcall *)(void*, void*, void*, void*, int))LEAF_DDOTS_VA)
        (ctx, view, node, pos, flag);
}

static void __stdcall mark_dcirc(void* ctx, void* view, void* node,
                                 void* pos, int flag)
{
    tagpu_order_trace_drawer(2, node, (const int*)pos, flag);
    ((void (__stdcall *)(void*, void*, void*, void*, int))LEAF_DCIRC_VA)
        (ctx, view, node, pos, flag);
}

static void __stdcall mark_drange(void* ctx, void* view, void* node,
                                  void* pos, int flag)
{
    tagpu_order_trace_drawer(4, node, (const int*)pos, flag);
    ((void (__stdcall *)(void*, void*, void*, void*, int))LEAF_DRANGE_VA)
        (ctx, view, node, pos, flag);
}

/* THE GROUP DIGIT. Skipped outright once tagpu_mark.c draws it, for the reason
   every other skip here exists: `0x4C14F0` rejects the string whole unless its
   measured box is fully inside the context's clip rect (`0x4B6750` at
   `0x4C1697` is a containment test, not an intersection), and that rect is the
   screen-sized offscreen's — so at zoom < 1 a digit out in the ring is not
   clipped, it is dropped. [BINARY-VERIFIED] */
static void __stdcall mark_digit(void* ctx, void* str, int x, int y, int maxw)
{
    if (g_digits) return;
    ((void (__stdcall *)(void*, void*, int, int, int))LEAF_TEXT_VA)
        (ctx, str, x, y, maxw);
}

/* ---- install ---------------------------------------------------------- */

/* an `E8 <rel32>` at `site` whose target is `expect`? */
static int site_is(unsigned int site, unsigned int expect)
{
    const unsigned char* p = (const unsigned char*)(size_t)site;
    if (IsBadReadPtr((void*)p, 5)) return 0;
    return p[0] == 0xE8 &&
           *(const unsigned int*)(p + 1) == expect - (site + 5);
}

static int redirect(unsigned int site, void* target)
{
    unsigned char b[5];
    b[0] = 0xE8;
    *(unsigned int*)(b + 1) = (unsigned int)(size_t)target - (site + 5);
    return tagpu_detour_write(site, b, 5);
}

void tagpu_markown_init(void)
{
    char b[192];
    int ok;

    if (GetFileAttributesA("tagpu_markown.on") == INVALID_FILE_ATTRIBUTES) return;

    /* all-or-nothing: every byte is checked before any of them is written, so a
       patched or different exe arms nothing rather than half of it */
    if (!site_is(SITE_HOOK8_VA,   PASS_SFX_VA)    ||
        !site_is(SITE_HOOK9_VA,   PASS_SFX_VA)    ||
        !site_is(SITE_TRANSP1_VA, PASS_TRANSP_VA) ||
        !site_is(SITE_TRANSP2_VA, PASS_TRANSP_VA) ||
        !site_is(SITE_SELBOX1_VA, LEAF_SELBOX_VA)  ||
        !site_is(SITE_SELBOX2_VA, LEAF_SELBOX_VA)  ||
        !site_is(SITE_TSPRITE1_VA, LEAF_TSPRITE_VA) ||
        !site_is(SITE_TSPRITE2_VA, LEAF_TSPRITE_VA) ||
        !site_is(SITE_ORDERS_VA,  PASS_ORDERS_VA)  ||
        !site_is(SITE_DIGIT_VA,   LEAF_TEXT_VA)    ||
        !site_is(SITE_DBUILD_VA,  LEAF_DBUILD_VA)  ||
        !site_is(SITE_DDOTS_VA,   LEAF_DDOTS_VA)   ||
        !site_is(SITE_DCIRC_VA,   LEAF_DCIRC_VA)   ||
        !site_is(SITE_DRANGE_VA,  LEAF_DRANGE_VA)  ||
        memcmp((void*)LEAF_BARS_VA, BARS_STOLEN, 5) != 0) {
        flog("markown: NOT armed — engine bytes differ at one of "
             "0x4699EB/0x469B8A/0x469BD7/0x469BFC/0x469D2C/0x469EC5/0x469F1E/"
             "0x46A430/0x469CF9/0x439516/0x439BAC/0x439BF2/0x439C37/0x439C7D/"
             "0x439CBE");
        return;
    }

    ok  = redirect(SITE_HOOK8_VA,   (void*)mark_hook8);
    ok &= redirect(SITE_HOOK9_VA,   (void*)mark_hook9);
    ok &= redirect(SITE_TRANSP1_VA, (void*)mark_transp);
    ok &= redirect(SITE_TRANSP2_VA, (void*)mark_transp);
    ok &= redirect(SITE_SELBOX1_VA, (void*)mark_selbox);
    ok &= redirect(SITE_SELBOX2_VA, (void*)mark_selbox);
    ok &= redirect(SITE_TSPRITE1_VA, (void*)mark_tsprite_dot);
    ok &= redirect(SITE_TSPRITE2_VA, (void*)mark_tsprite);
    ok &= redirect(SITE_ORDERS_VA,  (void*)mark_orders);
    ok &= redirect(SITE_DIGIT_VA,   (void*)mark_digit);
    ok &= redirect(SITE_DBUILD_VA,  (void*)mark_dbuild);
    ok &= redirect(SITE_DDOTS_VA,   (void*)mark_ddots);
    ok &= redirect(SITE_DCIRC_VA,   (void*)mark_dcirc);
    ok &= redirect(SITE_DRANGE_VA,  (void*)mark_drange);
    ok &= tagpu_detour_leaf(LEAF_BARS_VA, BARS_STOLEN, 5,
                            &g_markown_skipBars, 0x10);
    g_installed = ok;
    _snprintf(b, sizeof b,
        "markown: %s (hook8/hook9/transp x2/selbox x2/tsprite x2/orders/digit/"
        "drawers x4 redirected, bars@0x46A430 detoured; all follow "
        "tagpu_mark.on and tagpu_order.on)",
        ok ? "ARMED" : "PARTIAL — see above");
    flog(b);
}

void tagpu_markown_set_orders(int ours)
{
    int v = ours && g_installed;
    if (v == g_orders) return;
    g_orders = v;
    flog(v ? "markown: engine order markers SKIPPED (ours live)"
           : "markown: engine order markers restored");
}

void tagpu_markown_set_digits(int ours)
{
    int v = ours && g_installed;
    if (v == g_digits) return;
    g_digits = v;
    flog(v ? "markown: engine group digits SKIPPED (ours live)"
           : "markown: engine group digits restored");
}

void tagpu_markown_set_bars(int ours)
{
    unsigned char v = (unsigned char)(ours && g_installed);
    if (v == g_markown_skipBars) return;
    g_markown_skipBars = v;
    flog(v ? "markown: engine health bars SKIPPED (ours live)"
           : "markown: engine health bars restored");
}

void tagpu_markown_set_selbox(int ours)
{
    int v = ours && g_installed;
    if (v == g_selbox) return;
    g_selbox = v;
    flog(v ? "markown: engine selection rects SKIPPED for owned units"
           : "markown: engine selection rects restored");
}

void tagpu_markown_set_cursor(int ours)
{
    int v = ours && g_installed;
    if (v == g_cursor) return;
    g_cursor = v;
    /* the post-fog window can never open again while this is set, so its last
       publication would stand behind ours forever; give it up here. One
       volatile store, which is why this is safe from the present thread. */
    if (v) layer_clear(&g_L[TAGPU_MARK_POSTFOG]);
    flog(v ? "markown: engine build cursor/band box SKIPPED (ours live)"
           : "markown: engine build cursor/band box restored");
}

void tagpu_markown_set_capture(int on)
{
    int v = on && g_installed;
    if (v == g_capture) return;
    g_capture = v;
    if (!v) {
        /* Publish "nothing" and stop opening new windows — but do NOT close an
           open one from here. This runs on the PRESENT thread (the arm poll and
           the watchdog both reach it) while `layer_begin` runs on the game
           thread inside DrawGameScreen, and an open window's `ctx` points into
           that thread's live stack frame. Racing `layer_end` against the game
           thread's own could write a half-cleared `saved` — a NULL pixel base —
           straight back into the engine's draw context. It is not needed
           either: every window the game thread opens, it closes, at hook 9 or
           at the end of the same DrawTranspRectangle call. */
        layer_clear(&g_L[TAGPU_MARK_POSTFOG]);
        g_L[TAGPU_MARK_POSTFOG].tried = 0;
        g_L[TAGPU_MARK_POSTFOG].opened = 0;
    }
    flog(v ? "markown: engine UI markers CAPTURED (ours live)"
           : "markown: engine UI markers restored");
}

void tagpu_markown_beat(unsigned int frame_counter) { g_beat = frame_counter; }

void tagpu_markown_flush(unsigned int frame_counter)
{
    if (!g_installed) return;
    /* if the native pass stops running (overlay off, GL failure, a frame path
       that never reaches it) the engine's markers come back rather than the
       health bars and order lines simply vanishing */
    if ((g_capture || g_markown_skipBars || g_selbox || g_cursor || g_orders ||
         g_digits) &&
        frame_counter - g_beat > 90) {
        flog("markown: marker pass silent for 90 frames");
        tagpu_markown_set_capture(0);
        tagpu_markown_set_bars(0);
        tagpu_markown_set_selbox(0);
        tagpu_markown_set_cursor(0);
        tagpu_markown_set_orders(0);
        tagpu_markown_set_digits(0);
    }
    if (frame_counter - g_last >= 300) {
        char b[128];
        g_last = frame_counter;
        _snprintf(b, sizeof b,
                  "MARKOWN capture=%d bars-skipped=%u selbox=%d cursor=%d "
                  "orders=%d digits=%d",
                  g_capture, (unsigned)g_markown_skipBars, g_selbox, g_cursor,
                  g_orders, g_digits);
        flog(b);
    }
}
