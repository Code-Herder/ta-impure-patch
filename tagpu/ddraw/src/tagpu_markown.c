/* tagpu_markown.c — the fourteen call-site redirects, the prologue detour and
   the one capture window that is left. See tagpu_markown.h for what this owns,
   what became a re-draw, and why the pre-fog window is gone. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_markown.h"
#include "tagpu_opt.h"
#include "tagpu_order.h"
#include "tagpu_text.h"
#include "tagpu_packet_pub.h"
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
   flag), ret 0x14. Both sites are redirected for the trace and nothing else:
   the sprite has no buffer of ours to composite against (tagpu_markown.h).
   Its address is ALSO in `.rdata` 19 times, as
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
static int g_selbox = 0;                  /* ours redraws the selection rect */
static int g_cursor = 0;                  /* ours redraws the build cursor   */
static int g_orders = 0;                  /* ours redraws the order markers  */
static int g_digits = 0;                  /* ours redraws the group digit    */
static unsigned g_beat = 0, g_last = 0;

/* NOTHING HERE CAPTURES THE ENGINE'S MARKER PIXELS. Everything the marker
   pass draws is re-derived from engine STATE as our own geometry. A capture
   would DIVERT the engine's draw: bytes captured into a scratch of ours never
   reach the engine's own surface, so the reference frame `tagpu_surf.c` keeps
   would miss exactly the markers the capture took.

   The *ownership* levers below are a different act: they stop the engine
   drawing a marker at all, so ours can stand in its place. */

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}


int tagpu_markown_installed(void) { return g_installed; }
int tagpu_markown_key(void) { return tagpu_terr_key(); }


/* ---- the redirected call sites --------------------------------------- */

/* hook 8: the layer-8 particle draw runs FIRST, into the engine's own frame
   (those particles belong to tagpu_sfx, not to us). What remains here is
   bookkeeping the order arena needs to know a marker block has STARTED, plus
   the font latch the text pass needs taken at this instant. */
static void __stdcall mark_hook8(void* ctx, int n)
{
    ((void (__stdcall *)(void*, int))PASS_SFX_VA)(ctx, n);

    /* The engine's own font and text colour, as of the start of the block that
       draws the group digit and the ShowRanges labels. Taken here rather than
       read from the present thread because `SetFont 0x4C1420` runs many times a
       frame and nothing between this hook and `0x469CF9` calls it
       (tagpu_text.h). The font is COPIED here, glyph by glyph, and travels in
       the packet as bytes: the present thread never dereferences it
       (tagpu_packet_pub.c). */
    tagpu_packet_pub_font_snapshot();

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
   ends` over ~50 000 blocks of live play agreed. */
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
    /* OURS DRAWS BOTH OF THIS BLOCK'S PRIMITIVES, so the engine's pair is not
       drawn at all — capturing them could never carry them into the outer ring.
       The engine clips these rects to the OFFSCREEN, which is screen-sized,
       while the position it projects them at is the addressable coordinate
       `vpwide` widened to; at zoom < 1 the two disagree and the whole rect is
       clipped away long before our buffer sees it. Skipping is also what stops
       an engine-drawn rect standing in our frame as a ghost at the unzoomed
       position, exactly as for the selection rect above. */
    if (g_cursor) return;
    ((void (__stdcall *)(void*, void*, int))PASS_TRANSP_VA)(ctx, rect, colour);
}

/* The selection rectangle is the one marker the engine draws INSIDE the unit
   sweeps, per unit and immediately before that unit's own sprite — so it cannot
   be bracketed with the block above, and the native pass re-draws it (it would
   otherwise be buried under our pixels). Left alone, the ENGINE also draws its
   own into the frame underneath, where at 1x it lands on exactly the same
   pixels and is invisible, and at any other zoom becomes a ghost box at the
   unzoomed position.

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

   The star alpha-composites through `table[(src<<8)|dst]`, only ever against
   the ENGINE's own frame — which is what stock does — so there is nothing to
   correct. Under `passive` and `trace`, where the engine draws its own markers
   into its own surface and we composite that surface, the star is stock's
   blend against a key-filled viewport: teal instead of olive, and a debug
   mode's business rather than the shipped frame's. */
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

    if (!tagpu_opt_on("tagpu_markown.on")) return;

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
    flog(v ? "markown: engine build cursor/band box SKIPPED (ours live)"
           : "markown: engine build cursor/band box restored");
}


void tagpu_markown_beat(unsigned int frame_counter) { g_beat = frame_counter; }

void tagpu_markown_flush(unsigned int frame_counter)
{
    if (!g_installed) return;
    /* if the native pass stops running (overlay off, a level teardown, a frame
       path that never reaches it) the engine's markers come back rather than the
       health bars and order lines simply vanishing */
    if ((g_markown_skipBars || g_selbox || g_cursor || g_orders ||
         g_digits) &&
        frame_counter - g_beat > 90) {
        flog("markown: marker pass silent for 90 frames");
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
                  "MARKOWN bars-skipped=%u selbox=%d cursor=%d "
                  "orders=%d digits=%d",
                  (unsigned)g_markown_skipBars, g_selbox, g_cursor,
                  g_orders, g_digits);
        flog(b);
    }
}
