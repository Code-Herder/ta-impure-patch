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
/* The graphics globals block (`0x4B6220` is just `mov eax,ds:0x51FBD0; ret`)
   and, inside it, the pointer to the 64 KB blend LUT every alpha composite
   runs through: `out = tab[(src << 8) | dst]`, the inner loop at
   `0x4CBF99..0x4CBFAC` (source texels equal to the sprite's transparent index
   are skipped before the lookup). [BINARY-VERIFIED] */
#define GFX_GLOBALS_PP   0x0051FBD0u
#define GFX_ALPHATAB     0xC0
/* the target-sprite drawer and its only two callers (`0x4394E0` delegates to
   it, `0x439B30` dispatches bit 3 to it); stdcall(ctx, view, node, pos, flag),
   ret 0x14, no function-pointer table in the path [BINARY-VERIFIED] */
#define SITE_TSPRITE1_VA 0x00439516u
#define SITE_TSPRITE2_VA 0x00439C7Du
#define LEAF_TSPRITE_VA  0x00439740u

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

/* WHY THE ENGINE'S BLEND LUT IS REPLACED ACROSS ONE CALL.

   The order pass's target sprite — the pulsing star at a move/attack waypoint,
   drawn by `0x439740` through `AlphaCompsteBuf2OFFScreen 0x4B8500` — is
   ALPHA-COMPOSITED: it reads the destination pixel and looks the pair up in the
   LUT above. In stock TA that destination is the terrain, so the star comes out
   blended with the ground (olive over grass). In our frame the destination is
   the fill key, because terrown replaced the terrain blit with it — so the star
   came out blended with palette 254, a bright cyan, and read as washed out.

   markown.h used to argue this was harmless, on the grounds that the key is
   "exactly what they already read out of the engine's frame today". That is
   true of what the primitive READS and wrong about what it WRITES: the blend
   result is a function of the destination, so a key destination gives a keyed
   colour. Measured against stock: star olive vs ours teal, 14% of the sprite's
   bounding box on the cyan ramp against 1% after this.

   The fix is to make that one composite a copy: an identity LUT, every
   (src,dst) pair answering src, lands the sprite in our buffer as its own
   palette indices and the replay draws it opaque. That is a DELIBERATE
   departure from stock, which blends it; the sprite's true colours are what the
   capture now holds, so re-blending it against our own scene instead would be a
   shader change rather than another capture change.

   THE SWAP IS SCOPED TO THE SINGLE CALL, and that is the whole design, not a
   detail. `[globals+0xC0]` is not a bare pointer to borrow — it OWNS a 64 KB
   heap buffer with a lifetime: `0x4BA5C0` allocates it through TA's own
   allocator (`push 0x10000; call 0x4D83B0`), `0x4BA5F0` hands it to TA's free
   (`0x4D85A0`) from the graphics teardown, and `0x4BAAD0` (`rep movsd` of
   0x4000 dwords) and `0x4BA750` refill it wholesale when `palettes\PALETTE.ALP`
   is (re)loaded per game. [BINARY-VERIFIED] So a pointer of ours left in that
   slot across a frame boundary is not merely untidy: a table reload would write
   64 KB into OUR buffer — silently un-fixing the star while leaving the
   engine's real table stale for every other blend in the session — and a
   teardown would pass a block from the DLL's heap to TA's static-CRT free.

   An earlier revision installed it at hook 8 and restored it at hook 9, with a
   comment claiming that a global "is safe to restore from a LATER frame". It is
   not, and the abandonment path that comment pointed at (a `drawUnits == 0`
   frame skips hook 9 entirely) is exactly how the pointer would have escaped.
   Wrapping the two call sites of the drawer instead makes escape impossible:
   the swap begins and ends inside one function call that always returns, so no
   engine allocation, free or reload can ever observe it.

   Gated on our capture window actually being open, so passive mode and the
   engine's own frame are left with the engine's own blend. */
static unsigned char*  g_opaqueTab;      /* 64 KB, built once at init      */
static unsigned char** g_tabSlot;        /* non-NULL while ours is in      */
static unsigned char*  g_tabSaved;       /* the engine's own pointer       */

volatile unsigned char g_markown_skipBars = 0;

static int g_installed = 0;
static int g_capture = 0;                 /* follows tagpu_mark.on           */
static int g_selbox = 0;                  /* ours redraws the selection rect */
static unsigned g_beat = 0, g_last = 0;

/* One layer, double-buffered. The game thread fills one buffer while the GL
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

static void alpha_build(void)
{
    int v;
    g_opaqueTab = (unsigned char*)malloc(256 * 256);
    if (!g_opaqueTab) { flog("markown: opaque blend table alloc failed"); return; }
    /* row `src` is 256 copies of `src`, so any dst answers src */
    for (v = 0; v < 256; v++)
        memset(g_opaqueTab + v * 256, v, 256);
}

static void alpha_opaque_on(void)
{
    unsigned char** slot;
    char* g;
    if (g_tabSlot || !g_opaqueTab) return;         /* already in, or no table */
    g = *(char**)GFX_GLOBALS_PP;
    if (!ptr_ok(g)) return;
    slot = (unsigned char**)(g + GFX_ALPHATAB);
    if (IsBadWritePtr(slot, 4) || !ptr_ok(*slot)) return;  /* not built yet */
    g_tabSaved = *slot;
    *slot = g_opaqueTab;
    g_tabSlot = slot;
}

static void alpha_opaque_off(void)
{
    if (!g_tabSlot) return;
    /* only put ours back if ours is still what is there: if the engine has
       re-pointed the slot in between, writing the old pointer over it would
       leak the new buffer and hand the engine a dangling one */
    if (*g_tabSlot == g_opaqueTab) *g_tabSlot = g_tabSaved;
    g_tabSlot = NULL;
    g_tabSaved = NULL;
}

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
    LAYER* L = &g_L[TAGPU_MARK_PREFOG];
    LAYER* P = &g_L[TAGPU_MARK_POSTFOG];
    const char* ta;
    ((void (__stdcall *)(void*, int))PASS_SFX_VA)(ctx, n);

    /* A window still open here belongs to a frame that never reached hook 9,
       and there is exactly one way to get that: `0x469C03 je 0x469D38` leaves
       the block early when drawUnits is 0 and lands PAST the hook. Three of
       DrawGameScreen's four callers pass drawUnits=1 (two as a literal,
       `0x4969CD` through an ebx its function sets to 1 at `0x4967CF`); the
       fourth, `0x4962C2`, is TA's own movie recorder (`"%s\\MOVIE%03i"`,
       function at `0x495E88`, `xor ebx,ebx` at `0x495EA1`) and passes 0.
       [BINARY-VERIFIED]

       Abandon it rather than close it: `saved` belongs to a stack frame that
       has since returned, so writing it back would scribble on whatever lives
       at that address now. The engine's own frame lost the tail of that one
       draw either way — nothing here can give it back — but the next frame
       starts clean instead of corrupting a stack. */
    if (L->active) { L->active = 0; L->ctx = NULL; L->saved = NULL; }

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

    /* The engine's markers land in our buffer or in its own frame; there is no
       third option, so every path that does not open a window has to give the
       last capture up. Opening one does not: it fills the OTHER buffer and
       hook 9 swaps it in whole. */
    ta = *(const char* const*)TA_MAINPP;
    if (!g_capture || !ptr_ok(ta) || !prefog_wanted(ta) ||
        !layer_begin(L, (int*)ctx, 1))
        layer_clear(L);
}

/* hook 9: close the window before the layer-9 particles, which the engine
   draws after the markers and which are not ours to move. */
static void __stdcall mark_hook9(void* ctx, int n)
{
    /* Every in-game frame reaches this hook — the one branch that does not is
       drawUnits == 0, which only the movie recorder passes (see hook 8) — and
       `opens == ends` over ~50 000 blocks of live play says so. */
    layer_end(&g_L[TAGPU_MARK_PREFOG], 1);
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

/* The waypoint star, and the only place the blend LUT is touched. Bracketing
   the call rather than the frame is what keeps our pointer out of a slot the
   engine owns (see the note above). */
static void __stdcall mark_tsprite(void* ctx, void* view, void* node,
                                   void* pos, int flag)
{
    int mine = g_L[TAGPU_MARK_PREFOG].active;
    if (mine) alpha_opaque_on();
    ((void (__stdcall *)(void*, void*, void*, void*, int))LEAF_TSPRITE_VA)
        (ctx, view, node, pos, flag);
    if (mine) alpha_opaque_off();
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
        memcmp((void*)LEAF_BARS_VA, BARS_STOLEN, 5) != 0) {
        flog("markown: NOT armed — engine bytes differ at one of "
             "0x4699EB/0x469B8A/0x469BD7/0x469D2C/0x469EC5/0x469F1E/0x46A430/"
             "0x439516/0x439C7D");
        return;
    }

    alpha_build();

    ok  = redirect(SITE_HOOK8_VA,   (void*)mark_hook8);
    ok &= redirect(SITE_HOOK9_VA,   (void*)mark_hook9);
    ok &= redirect(SITE_TRANSP1_VA, (void*)mark_transp);
    ok &= redirect(SITE_TRANSP2_VA, (void*)mark_transp);
    ok &= redirect(SITE_SELBOX1_VA, (void*)mark_selbox);
    ok &= redirect(SITE_SELBOX2_VA, (void*)mark_selbox);
    ok &= redirect(SITE_TSPRITE1_VA, (void*)mark_tsprite);
    ok &= redirect(SITE_TSPRITE2_VA, (void*)mark_tsprite);
    ok &= tagpu_detour_leaf(LEAF_BARS_VA, BARS_STOLEN, 5,
                            &g_markown_skipBars, 0x10);
    g_installed = ok;
    _snprintf(b, sizeof b,
        "markown: %s (hook8/hook9/transp x2/selbox x2/tsprite x2 redirected, "
        "bars@0x46A430 detoured; all follow tagpu_mark.on)",
        ok ? "ARMED" : "PARTIAL — see above");
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
