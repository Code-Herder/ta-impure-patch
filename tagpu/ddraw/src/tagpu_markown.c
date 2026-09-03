/* tagpu_markown.c — the four call-site redirects and the capture buffers.
   See tagpu_markown.h for what this owns and why it is a pointer swap. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_markown.h"
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
#define HOTKEY_VA        0x004C1B80u   /* KeyboardHotkeySampler(id), ret 4        */

/* `83 EC 10 | 53 | 55` = sub esp,0x10; push ebx; push ebp — five
   position-independent bytes ending on an instruction boundary (0x46A435) */
static const unsigned char BARS_STOLEN[5] = { 0x83, 0xEC, 0x10, 0x53, 0x55 };

/* engine layout (ui-markers.md appendix, terrain-depth.md 4) */
#define OFF_VP_L     0x37E27
#define OFF_VP_T     0x37E2B
#define OFF_VIEW_W   0x37E37
#define OFF_VIEW_H   0x37E3B
#define OFF_GAMEOPT  0x37F06     /* bit0 = the registry option "damagebars"   */
#define OFF_UNITS    0x14357     /* unit array base, stride 0x118             */
#define OFF_UNITEND  0x1435B     /* one past its last slot                    */
#define OFF_HOTIDS   0x1435F     /* u16* HotUnits ids                         */
#define OFF_HOTCNT   0x14367     /* their count                               */
#define OFF_WATCHED  0x2A42      /* u8 watched player id                      */
#define UNIT_STRIDE  0x118
#define U_SQUAD      0xAC        /* group digit, 0 = none                     */
#define U_OWNER      0xFF        /* u8 player id                              */
/* OFFSCREEN: +0x08 pitch, +0x0C pixel base, inclusive clip rect +0x1C..+0x28 */
#define CTX_PITCH    2
#define CTX_BASE     3
#define CTX_CLIP_L   7
#define CTX_CLIP_T   8
#define CTX_CLIP_R   9
#define CTX_CLIP_B   10
#define CTX_FIELDS   11          /* how much of it we read                    */

#define SHIFT_HOTKEY 0xF9        /* the id the engine samples for the markers */

volatile unsigned char g_markown_skipBars = 0;

static int g_installed = 0;
static int g_capture = 0;                 /* follows tagpu_mark.on           */
static int g_selbox = 0;                  /* ours redraws the selection rect */
static unsigned g_beat = 0, g_last = 0;

/* One layer, double-buffered. The game thread fills one buffer while the GL
   thread may still be uploading the other: a single buffer would let a present
   catch the key-fill half-done and show a frame with the top of every marker
   missing, which reads as flicker. Two buffers and a published pointer cost
   one more allocation and remove the whole class. */
typedef struct {
    unsigned char* buf[2];
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
        unsigned char* a = (unsigned char*)realloc(L->buf[0], (size_t)need);
        unsigned char* b;
        if (a) L->buf[0] = a;
        b = a ? (unsigned char*)realloc(L->buf[1], (size_t)need) : NULL;
        if (b) L->buf[1] = b;
        if (!a || !b) { flog("markown: capture buffer alloc failed"); return 0; }
        L->bytes = need;
    }
    L->which ^= 1;
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
    {   /* into the slot the GL side is NOT reading, REMEMBERED for layer_end:
           deriving it again there would pick the wrong one if layer_clear ran
           on the other thread in between */
        TAGPU_MARKLAYER* d;
        L->pend = (L->pub == 0) ? 1 : 0;
        d = &L->desc[L->pend];
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
   nothing but the game thread calls it. */
static void layer_clear(LAYER* L) { L->pub = -1; }

/* Is there anything for capture window A to catch? Order markers only draw
   while the engine's own SHIFT hotkey is held (`0x469BE1`), and group digits
   only for a watched unit carrying a squad tag with `damagebars` on — health
   bars themselves are ours now and never reach this block. When neither can
   fire, the window is not opened at all: no fill, no upload, nothing.

   The hotkey is sampled through the engine's own function so a rebind, or a
   different key on a different build, cannot make us disagree with it. Asking
   twice is harmless, and that is checked rather than assumed: ALL NINE of the
   image's `GetAsyncKeyState` call sites are the `0x4C1BA1..0x4C1D56` stubs and
   every one of them does `and al,0xfe`, so nothing in the engine ever reads the
   "pressed since last call" bit — which is the only thing an extra poll could
   consume (our shield's fake clears it on read, tagpu_shield.c). */
static int prefog_wanted(const char* ta)
{
    const unsigned short* ids;
    const char *units, *end;
    int n, i, watched;

    if (((int (__stdcall *)(int))HOTKEY_VA)(SHIFT_HOTKEY)) return 1;
    if (!(*(const unsigned char*)(ta + OFF_GAMEOPT) & 1)) return 0;
    ids   = *(const unsigned short* const*)(ta + OFF_HOTIDS);
    units = *(const char* const*)(ta + OFF_UNITS);
    end   = *(const char* const*)(ta + OFF_UNITEND);
    n     = *(const int*)(ta + OFF_HOTCNT);
    watched = *(const unsigned char*)(ta + OFF_WATCHED);
    if (!ptr_ok(ids) || !ptr_ok(units) || !ptr_ok(end) || end <= units) return 0;
    if (n <= 0 || n > 20000) return 0;
    for (i = 0; i < n; i++) {
        /* Bound the index against the array. The engine's own loop does not —
           it trusts HotUnits because it built it — but this runs at hook 8,
           BEFORE the drawUnits gate the engine's loop sits behind, so it can
           see frames the engine's never walks. */
        const char* u = units + (size_t)ids[i] * UNIT_STRIDE;
        if (u + UNIT_STRIDE > end) continue;
        if (*(const unsigned char*)(u + U_OWNER) != (unsigned)watched) continue;
        if (*(const unsigned char*)(u + U_SQUAD)) return 1;
    }
    return 0;
}

/* ---- the four redirected call sites ---------------------------------- */

/* hook 8: the layer-8 particle draw runs FIRST, into the engine's own frame
   (those particles belong to tagpu_sfx, not to us), and the marker window
   opens behind it. */
static void __stdcall mark_hook8(void* ctx, int n)
{
    const char* ta;
    ((void (__stdcall *)(void*, int))PASS_SFX_VA)(ctx, n);
    layer_clear(&g_L[TAGPU_MARK_PREFOG]);
    if (!g_capture) return;
    ta = *(const char* const*)TA_MAINPP;
    if (!ptr_ok(ta) || !prefog_wanted(ta)) return;
    layer_begin(&g_L[TAGPU_MARK_PREFOG], (int*)ctx, 1);
}

/* hook 9: close the window before the layer-9 particles, which the engine
   draws after the markers and which are not ours to move. */
static void __stdcall mark_hook9(void* ctx, int n)
{
    layer_end(&g_L[TAGPU_MARK_PREFOG], 1);
    /* the post-fog window is per-call, so this is where its frame starts */
    g_L[TAGPU_MARK_POSTFOG].tried = 0;
    g_L[TAGPU_MARK_POSTFOG].opened = 0;
    layer_clear(&g_L[TAGPU_MARK_POSTFOG]);
    ((void (__stdcall *)(void*, int))PASS_SFX_VA)(ctx, n);
}

/* the build-cursor footprint and the drag band box: two consecutive calls that
   make one double-outlined rect, so the key fill happens on the first of them
   and both land in the same buffer. */
static void __stdcall mark_transp(void* ctx, void* rect, int colour)
{
    LAYER* L = &g_L[TAGPU_MARK_POSTFOG];
    int opened = 0;
    if (g_capture) {
        /* `tried` and `opened` are separate on purpose: if the FIRST of the two
           calls is refused (a clip rect that will not validate, an allocation
           that failed) the second must not then take the fresh path, flip the
           buffer and re-key-fill — that would drop the outer rect and publish
           only the inner one. A refused first call means this frame's cursor
           stays the engine's, whole. */
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
        memcmp((void*)LEAF_BARS_VA, BARS_STOLEN, 5) != 0) {
        flog("markown: NOT armed — engine bytes differ at one of "
             "0x4699EB/0x469B8A/0x469BD7/0x469D2C/0x469EC5/0x469F1E/0x46A430");
        return;
    }

    ok  = redirect(SITE_HOOK8_VA,   (void*)mark_hook8);
    ok &= redirect(SITE_HOOK9_VA,   (void*)mark_hook9);
    ok &= redirect(SITE_TRANSP1_VA, (void*)mark_transp);
    ok &= redirect(SITE_TRANSP2_VA, (void*)mark_transp);
    ok &= redirect(SITE_SELBOX1_VA, (void*)mark_selbox);
    ok &= redirect(SITE_SELBOX2_VA, (void*)mark_selbox);
    ok &= tagpu_detour_leaf(LEAF_BARS_VA, BARS_STOLEN, 5,
                            &g_markown_skipBars, 0x10);
    g_installed = ok;
    _snprintf(b, sizeof b,
        "markown: %s (hook8/hook9/transp x2/selbox x2 redirected, bars@0x46A430 "
        "detoured; all follow tagpu_mark.on)", ok ? "ARMED" : "PARTIAL — see above");
    flog(b);
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
        layer_clear(&g_L[TAGPU_MARK_PREFOG]);
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
    if ((g_capture || g_markown_skipBars || g_selbox) &&
        frame_counter - g_beat > 90) {
        flog("markown: marker pass silent for 90 frames");
        tagpu_markown_set_capture(0);
        tagpu_markown_set_bars(0);
        tagpu_markown_set_selbox(0);
    }
    if (frame_counter - g_last >= 300) {
        char b[96];
        g_last = frame_counter;
        _snprintf(b, sizeof b, "MARKOWN capture=%d bars-skipped=%u selbox=%d",
                  g_capture, (unsigned)g_markown_skipBars, g_selbox);
        flog(b);
    }
}
