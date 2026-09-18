/* tagpu_gui_hook.c — the observers, the census and the publisher (Phase E,
   G15a + G15b). Contract: inc/tagpu_gui.h. Design: research/notes/gui-renderer.md
   3.5, 3.6, 10.

   NOTHING HERE CHANGES WHAT THE ENGINE DRAWS. Every detour is an observer
   (tagpu_detour_observe): the original runs unchanged, we read its arguments
   off the stack on the way in and, where we need its result, on the way out.

   THE PUBLISHER (G15b). While the layer is on (g_gui_draw, the render thread's
   poll of tagpu_gui.on), the same ring the census reads is turned into queue
   ops for tagpu_gui_surf.c at the census cadence, inside the flip observer:
   a seed for every surface first seen, a sprite for a plain keyed GAF blit
   (its bytes decoded here on first sight), a twin-to-twin copy for 0x4C6B70
   from a twinned source, the viewport clear at the terrain key fill, and the
   box's bytes — read NOW, the frame complete — for everything else. See
   publish() and the dedup note above it.

   THE CENSUS. The engine's UI is retained: each .GUI screen owns a surface at
   panel+0xBC that the gadget handlers draw into, and the per-frame draw blits
   it into the frame (0x4AB0B0) only when dirty. So "which functions write UI
   pixels" cannot be answered by reading DrawGameScreen; it is answered by
   diffing. At every FlipOffscreenToPrimary (0x4C63A0, the engine's own
   "this frame is complete") the flipped surface is compared with its previous
   copy, the box of every op recorded since the last flip is subtracted, the
   world viewport is subtracted on a game frame (the terrain key fill is ours),
   and what is left is a writer we have not named — counted, boxed, and on
   demand written out as a PGM. The same diff runs on every surface an op has
   named (the GUI screens' own surfaces), so a handler that draws into a panel
   through a path we do not observe shows up there.

   THREADS. Everything in this file runs on the GAME thread, inside the
   engine's own calls; the render thread only reads a few counters in
   tagpu_gui_flush(). The op ring and the surface table are therefore plain
   statics with no locking, and an observer that fires on any other thread
   (tagpu_text.c rasterises through the glyph blitter on the render thread)
   records nothing: the game thread is the one the flip runs on. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_gui.h"
#include "tagpu_opt.h"
#include "tagpu_gui_int.h"
#include "tagpu_text.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"
#include "tagpu_gaf.h"
#include "tagpu_terrown.h"
#include "tagpu_reclaim.h"
#include "tagpu_packet_pub.h"

#define TA_MAINPP     0x00511DE8u
#define OFF_GUI_TOP   0x531           /* GUIInfo.TheActive_GUIMEM               */
#define GM_CTRLS      0x04
#define P_SURFACE     0xBC            /* panel record: OFFSCREEN* surface       */
#define P_TOTAL       0xB6

/* OFFSCREEN / drawing context head, shared by the surface object and the
   12-dword context the draw passes hand around (terrain-depth.md appendix) */
#define CTX_W      0
#define CTX_H      1
#define CTX_PITCH  2
#define CTX_BASE   3
#define CTX_CLIP_L 7                  /* +0x1C..+0x28, inclusive              */
#define CTX_CLIP_T 8
#define CTX_CLIP_R 9
#define CTX_CLIP_B 10

#define KEY_DEFAULT    254            /* tagpu_terr.c's composite key: the fill the
                                         terrain skip leaves in the viewport      */
#define FLIP_VA        0x004C63A0u    /* FlipOffscreenToPrimary                */
#define FLIP_RET_GAME  0x0046A3E0u    /* DrawGameScreen's call site returns here */
static const unsigned char FLIP_STOLEN[6] = { 0x81, 0xEC, 0xF4, 0x00, 0x00, 0x00 };

/* ------------------------------------------------------------------ state */

static int      s_installed = 0;
static int      s_census = 0, s_log = 0, s_pgm = 0, s_trace = 0;
/* `nostring` (G17d): text stays a box of captured pixels, as it was through
   the whole of phase 1. The A/B for the arena saving, and the escape if the
   stamp ever disagrees with the engine's blit on some font.
   READ AT ATTACH, LIKE EVERY OTHER TOKEN THIS FILE OWNS — `read_tokens` runs
   once, from `tagpu_gui_init`, so `census`, `log`, `pgm`, `trace` and this one
   must be armed BEFORE the launch. Only the surf module's tokens (`strict`,
   `norestore`, `sharptest`, `nocursor`, `cursorscale=`) follow the file live,
   because only the DRAW can change mid-session; the publisher's shape cannot
   without leaving the twins holding ops of the other kind. Arming it on a
   running instance silently does nothing, which cost one A/B to notice. */
static int      s_nostring = 0;
static int      s_key = KEY_DEFAULT;
static int      s_probeX = -1, s_probeY = -1;   /* trace: ops touching this pixel */
static DWORD    s_gameTid = 0;        /* the thread the flip runs on          */
static volatile unsigned s_flips = 0, s_opsTotal = 0, s_opsDropped = 0;
static volatile unsigned s_unexplTotal = 0, s_changedTotal = 0;
static unsigned s_lastLog = 0;
static unsigned s_winChanged = 0, s_winUnexpl = 0, s_winCensus = 0;   /* since the last line */
static int      s_winL = 0x7FFF, s_winT = 0x7FFF, s_winR = -1, s_winB = -1;

static void glog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}
static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

/* ---- the surfaces we have seen (game thread only) ---------------------- */
static void ops_forget_base(unsigned base);       /* below, with the ring */
typedef struct SURF {
    unsigned base;                    /* pixel base — the identity           */
    unsigned owner;                   /* the block MEM_Free 0x4D85A0 will be handed: the
                                         surface object itself, whose pixels are the same
                                         allocation at object+0x30 — see before_memfree */
    int w, h, pitch;
    unsigned char* copy;              /* the surface as of the last flip     */
    unsigned char* mask;              /* the last census: 0/128/255          */
    unsigned char* acc;               /* unexplained since the last PGM, OR-ed */
    int copyValid;
    unsigned seen;                    /* flips since first seen              */
    unsigned changed, explained, unexplained;   /* running totals            */
    int bl, bt, br, bb;               /* worst unexplained box last flip     */
    int seeded;                       /* a PK_SEED was published for it      */
    int lastCopyFrom;                 /* dedup(): position in the batch of the last
                                         COPY that read this surface, -1 if none  */
    int isOffscreen;                  /* created with the tag "OFFSCREEN" (0x5091D4): THE
                                         main offscreen, of which the engine has one at a
                                         time — see surf_drop_offscreens             */
} SURF;
#define MAX_SURF 24
static SURF s_surf[MAX_SURF];
static int  s_nsurf = 0;
#define TAG_OFFSCREEN 0x005091D4u     /* the "OFFSCREEN" string every 0x4C69F0 of the main
                                         offscreen pushes: 0x490AD3, 0x491250, 0x491B23,
                                         0x4980CF, 0x498402                              */

static TAGPU_PUBOP* pub_op(int kind, unsigned surf);   /* below */
static void pub_commit(void);
static void ops_forget_base(unsigned base);
extern volatile int g_gui_draw;

/* forget a surface: its buffers, its recorded boxes, and the twin */
static void surf_drop(int i)
{
    if (s_surf[i].seeded && g_gui_draw) {
        TAGPU_PUBOP* o = pub_op(PK_FREE, s_surf[i].base);
        if (o) pub_commit();
    }
    free(s_surf[i].copy); free(s_surf[i].mask); free(s_surf[i].acc);
    ops_forget_base(s_surf[i].base);
    s_surf[i] = s_surf[--s_nsurf];
}

/* THE MAIN OFFSCREEN IS FREED TO THE HEAP, NOT THROUGH SurfaceFree: MEM_Free
   0x4D85A0 at 0x491AB8 (leaving a game) and 0x49838C (the game's mode switch),
   and the next 0x4C69F0("OFFSCREEN", w, h) may land on the same base (a
   same-base size change, surf_get) or on another (MEASURED 2026-09-07, both).
   In the second case the old entry stayed: a dead 1024x768 the census walked
   at every flip — an access violation at its base once the heap had returned
   the block (the census run's crash on the first game -> shell switch) — and
   one MAX_SURF slot leaked per cycle. The engine has exactly one main
   offscreen at a time, so a new one retires every other. Keyed on the base,
   NOT a SURF* — surf_drop swap-removes (s_surf[i] = s_surf[--s_nsurf]), so a
   pointer to the kept entry moves if it was the last slot; the base is stable. */
static void surf_drop_offscreens(unsigned keepBase)
{
    int i;
    for (i = 0; i < s_nsurf; ) {
        if (s_surf[i].base != keepBase && s_surf[i].isOffscreen) surf_drop(i);
        else i++;
    }
}

/* ---- blocks freed on a thread that is not the game thread ---------------
   The table is the game thread's alone — `surf_drop` swap-removes and
   `surf_get` memsets the tail — so an observer that fires on another thread
   must not walk it: it would read a slot mid-move, mark the wrong entry and
   leave the right one standing, which is the crash this whole mechanism exists
   to stop. It leaves the block pointer here instead, and the game thread
   retires the entry at the top of the next flip, BEFORE the census or the
   publisher read any base. The engine's allocator is genuinely multi-threaded
   (`0x4D85B0` takes a critical section at `0x4D85C2`), so this is not a
   theoretical path even though no off-thread free of a recorded surface has
   yet been observed.

   EVERY OFF-THREAD FREE IS PUSHED, unfiltered, and that is deliberate. The
   obvious optimisation — scan the table off-thread and push only on a match —
   is NOT safe and was caught being unsafe: `MEM_Free` fires once per block, so
   a scan that races a swap-remove and misses (the moved entry lands in a slot
   the scan has already passed) queues nothing, the game thread never hears
   about it, and the entry stands for ever. A filter would have been
   load-bearing, not an optimisation.

   THE DRAIN ONLY EVER READS A QUIESCENT RING. A producer claims a slot with
   `s_freeqN`, stores into it, and only THEN bumps `s_freeqIn`; the consumer
   reads `s_freeqIn` first and `s_freeqN` second, so any push still in flight
   makes the two disagree and it does not read the window at all. That is what
   makes the slots trustworthy: every index in [done, head) was stored by its
   own claimant, so no stale value from an earlier lap can be read as a live
   one. (The third review found exactly that hole in the version before this:
   a flush skipped slots without clearing them, so a stale pointer survived a
   lap and was taken as real while the pointer that should have been there was
   written after the consumer had moved past. The zero-on-take below is kept as
   an assertion — a 0 now means a bug — and is NOT the argument.)

   EVERY WAY THIS RING CAN LOSE AN ENTRY ENDS IN THE SAME PLACE: retire the
   WHOLE table and let the surfaces re-register from the engine's own draws.
   The three ways are a push in flight, more than FREEQ claimed since the last
   drain, and a producer lapping the window while we walk it (the head is
   re-read against where the walk STARTED, which is the comparison that catches
   a claim at index >= from + FREEQ).

   WHAT IT COSTS, MEASURED 2026-09-12 over a load and four minutes of play:
   33 842 off-thread frees, EVERY ONE of them during the level load — the
   loader thread, created at 0x4982CA — and not one in the 236 000 flips of
   play that followed. Three flushes, all in the load. So in play this
   mechanism is inert, and its cost is three re-seeds while a map is loading.
   That is also why the ring exists rather than "any off-thread free flushes":
   at 33 842 frees spread over a load, an unconditional flush would re-seed
   every surface on most of the load's flips. */
#define FREEQ 256
static volatile LONG s_freeqN;                  /* claimed, ever — any thread   */
static volatile LONG s_freeqIn;                 /* STORED, ever — any thread    */
static LONG          s_freeqDone;               /* taken, ever — game thread    */
static volatile LONG s_freeq[FREEQ];            /* 0 = taken, or claimed-not-yet-stored */
static unsigned      s_freeqFlush;              /* times the table was flushed  */

static void surf_free_offthread(unsigned p)
{
    LONG n = InterlockedIncrement(&s_freeqN) - 1;   /* claim */
    InterlockedExchange(&s_freeq[n & (FREEQ - 1)], (LONG)p);
    InterlockedIncrement(&s_freeqIn);               /* and only now is it there */
}

static void surf_drain_freeq(void)              /* game thread only */
{
    /* STORED BEFORE CLAIMED, in that order: a push that completes between the
       two reads makes them disagree, which is the conservative answer. */
    LONG in   = InterlockedExchangeAdd(&s_freeqIn, 0);
    LONG head = InterlockedExchangeAdd(&s_freeqN, 0);
    LONG from = s_freeqDone;
    int flush = 0;

    if (head == from) return;
    /* The spans are taken in UNSIGNED arithmetic: these counters only grow, so
       one day they wrap, and a signed difference across that wrap is undefined
       where an unsigned one is exactly the distance we want. */
    if (in != head) flush = 1;                   /* a push is in flight */
    else if ((unsigned long)head - (unsigned long)from > FREEQ) flush = 1;
    else {
        LONG k;
        for (k = from; k != head; k++) {
            unsigned p = (unsigned)InterlockedExchange(&s_freeq[k & (FREEQ - 1)], 0);
            int i;
            if (!p) { flush = 1; break; }        /* claimed, not yet stored  */
            for (i = 0; i < s_nsurf; i++)
                if (s_surf[i].owner == p) { surf_drop(i); break; }
        }
        /* did a producer lap the window while we were walking it? */
        if (!flush && (unsigned long)InterlockedExchangeAdd(&s_freeqN, 0)
                      - (unsigned long)from > FREEQ) flush = 1;
    }
    if (flush) {
        s_freeqFlush++;
        while (s_nsurf) surf_drop(0);
    }
    s_freeqDone = head;
}

static SURF* surf_get(unsigned base, int w, int h, int pitch, unsigned owner)
{
    int i;
    if (!base || w <= 0 || h <= 0 || pitch <= 0 || w > 4096 || h > 4096 || pitch > 8192) return NULL;
    for (i = 0; i < s_nsurf; i++)
        if (s_surf[i].base == base) {
            s_surf[i].owner = owner;       /* re-made over the same bytes: the new block */
            if (s_surf[i].w != w || s_surf[i].h != h || s_surf[i].pitch != pitch) {
                /* the object was re-allocated over the same bytes: start over.
                   The ops already recorded against the base carry the OLD
                   size's boxes, and nothing else drops them: the game's own
                   OFFSCREEN is freed by 0x4D85A0 directly (0x491AB8 on the way
                   back to the shell, 0x49838C at the game's mode switch), never
                   through SurfaceFree 0x4C6AC0, so before_free never sees it —
                   the next publish then found a box past the new surface and
                   called it an overflow (MEASURED 2026-09-07, one per switch) */
                ops_forget_base(base);
                s_surf[i].w = w; s_surf[i].h = h; s_surf[i].pitch = pitch;
                s_surf[i].copyValid = 0;
                s_surf[i].seeded = 0;          /* the twin is the old size: re-make it */
                free(s_surf[i].copy); free(s_surf[i].mask); free(s_surf[i].acc);
                s_surf[i].copy = s_surf[i].mask = s_surf[i].acc = NULL;
            }
            return &s_surf[i];
        }
    if (s_nsurf >= MAX_SURF) return NULL;
    memset(&s_surf[s_nsurf], 0, sizeof(SURF));
    s_surf[s_nsurf].base = base; s_surf[s_nsurf].w = w; s_surf[s_nsurf].h = h;
    s_surf[s_nsurf].pitch = pitch; s_surf[s_nsurf].owner = owner;
    s_surf[s_nsurf].bl = s_surf[s_nsurf].bt = 0x7FFF; s_surf[s_nsurf].br = s_surf[s_nsurf].bb = -1;
    return &s_surf[s_nsurf++];
}

/* a drawing context's destination as a surface */
static SURF* surf_of_ctx(const int* ctx)
{
    unsigned base;
    if (!ptr_ok(ctx)) return NULL;
    base = (unsigned)ctx[CTX_BASE];
    /* THE OWNER IS DERIVED FROM THE BASE, NOT FROM THE CONTEXT POINTER.
       `SurfaceCreateNamed 0x4C69F0` asks MEM_Alloc for w*h+0x30 bytes and points
       the object's base field at block+0x30 (0x4C6A01..0x4C6A14), so the block
       MEM_Free will be handed is `base - 0x30` — and that holds however we
       reached the surface. The CONTEXT is not usable for this: `GetContext
       0x4C5E70` rep-movs a 12-dword copy into the caller's stack frame, so most
       blits hand us a copy whose address has nothing to do with the block.
       (MEASURED 2026-09-12, and it is why the first cut of this rule was wrong:
       keying on the context refused 1235 draws in one game while registering
       the same surfaces through `after_alloc`, where the context IS the object.)

       WHAT THIS DOES NOT COVER, stated rather than assumed: `SurfaceAttach
       0x4C6A60` — one caller, `0x4B5897` — lays the same header over the locked
       DirectDraw primary, memory the engine did not allocate and will not
       MEM_Free. Such a surface would have no destructor here. None was ever
       recorded (every base this module has seen is an `0x4C69F0` object;
       measured over a full session in game and in the shell), and its pixels
       belong to the fork, which frees them only in the surface's own Release. */
    return surf_get(base, ctx[CTX_W], ctx[CTX_H], ctx[CTX_PITCH], base - 0x30);
}

/* ---- the ops recorded since the last flip ------------------------------ */
/* `OP_FOCUS` IS SPLIT OFF `OP_RECT` BY LANDING 8b, and the split is the whole
   reason 8b can port anything: the two leaves that were both `OP_RECT` are not
   the same operation. `0x4BF8C0` draws four inclusive edges through the
   STORE-ONLY Bresenham `0x4CC7AB` -- a hollow rectangle of one palette index,
   which ports as geometry. `0x4BF7B0` draws FOUR edges of ONE box through
   `0x4BEC70` -- the two groups of four calls at `0x4BF7F3`..`0x4BF839` and
   `0x4BF86A`..`0x4BF8A4` are MUTUALLY EXCLUSIVE ARMS, selected by
   `test edi,edi / jne 0x4BF854` on the ctx argument at `0x4BF7BD`, the first
   acquiring a context through `0x4C5E70` and returning at `0x4BF851`. Whose
   writer `0x4CC8DF` READS THE DESTINATION and remaps it through
   `globals+0xC8` -- the same shade table `0x4BF4D0` uses. Publishing that as a
   colour and a box would paint a solid rectangle where the engine tinted what
   was under it. It keeps the `PK_PIXELS` path until something can carry a
   read-modify-write across the seam.
   [DISASSEMBLED 2026-09-18; exe-reverse-engineering.md has both.] */
/* `OP_DIAG` IS SPLIT OFF `OP_LINE` BY LANDING 8c, for `OP_FOCUS`'s reason one
   kind along: an AXIS-ALIGNED line's bounding box IS the line, one pixel thick,
   so it is a solid fill and ports exactly as `OP_BAR` does; a DIAGONAL's
   bounding box is the square the line crosses, and replaying it would paint the
   whole square. Keeping them as two kinds means `GUI kinds:` counts them apart,
   so the ratio is read rather than guessed. */
enum { OP_GAF = 1, OP_GAFA, OP_GAFB, OP_GAFD, OP_SCALE, OP_TEXT, OP_LINE, OP_BAR, OP_RECT, OP_FRAME, OP_FILL, OP_COPY,
       OP_FLIP, OP_FOCUS, OP_DIAG, OP_NKIND };
static const char* const OP_NAME[OP_NKIND] = { "?", "gaf", "gafa", "gafb", "gafd", "scale", "text", "line", "bar", "rect", "frame", "fill", "copy", "flip", "focus", "diag" };
typedef struct OP {
    unsigned base; short l, t, r, b; unsigned char kind;
    /* what the publisher needs beyond the box (gui-renderer.md 3.6) */
    unsigned char ck; unsigned short fw, fh;    /* sprite: key, frame size    */
    const void* frame; const void* pix;         /* sprite: identity -- KEYS,
                                                   never dereferenced again   */
    /* THE SPRITE IS RESOLVED AT OBSERVE TIME, NOT AT PUBLISH (G19f-7).
       `fkey` is `frame_key`'s content hash and `goff`/`glen` the decoded plane
       in `s_gafBuf`, both taken in `gaf_box` while the engine is inside its own
       blit of that frame. See `gaf_capture`. `publish` uses these and reads no
       engine asset memory at all, so `frame`/`pix` survive only as the atlas
       key the consumer matches on -- a VALUE compared against a table. */
    unsigned fkey;                              /* 0: not resolvable, use pixels */
    unsigned goff, glen;                        /* the decoded plane, 0 = none   */
    /* the three header bytes the `gui probe:` trace prints, taken with the rest
       in `gaf_box`. They are here so that `publish` needs NO pointer into
       engine art at all -- the probe was the last read left, and a debug path
       that dereferences what the release path no longer does is exactly the
       sort of thing that survives until it crashes someone.
       [FOUND 2026-09-16 by BOTH landing reviewers, independently.] */
    unsigned char fcomp, fsub, fsubn;
    unsigned sgen;                              /* the seen table `glen` was decided against */
    short dx, dy;                               /* sprite: unclipped top-left */
    unsigned src; short sl, st;                 /* copy: source, its top-left */
    unsigned seq;                               /* flip: terrown's fill seq   */
    /* text (G17d): the string is copied into a game-thread scratch AT OBSERVE
       TIME, not read again at publish. The argument routinely points at a
       caller's stack temp, which is gone by the flip — the same reason a
       sprite's pixels are copied rather than pointed at (gui-renderer.md 3.5).
       `frame` carries the font object and `dx`/`dy` the x/y it was given. */
    unsigned soff; unsigned short slen;
    unsigned char fg, bg, tr;                   /* text: 0x4CCF60's three colours */
    /* THE THIRD ARGUMENT of the four `rect_box` leaves, WHICH IS A PALETTE
       INDEX FOR `OP_BAR` AND IS NOT ONE FOR THE OTHER THREE.

       `DrawBar 0x4BF6F0` is the only one of the four that fills: it writes
       through `0x4CCDEA`, which takes the LOW BYTE of its colour and nothing
       wider (the 4-aligned path builds a DWORD of four copies for
       `rep stos DWORD`, the unaligned one `rep stos BYTE al`). One byte is
       therefore the engine's own width for `OP_BAR`, not a narrowing of ours.

       THE OTHER THREE MEAN SOMETHING ELSE AND THIS FIELD DOES NOT DESCRIBE
       THEM [CORRECTED 2026-09-18 by landing 8a's review; the first version of
       this comment claimed all four went through `0x4CCDEA`, and disassembly
       of the pristine build says none of the other three does]:
         - `OP_FRAME` / `0x4BF4D0` is a SHADE, not a fill. The argument is a
           SIGNED level clamped to [-0x20, +0x1F] that selects one of 32 rows
           of a 256-byte remap table -- `globals+0xC4` for negative, `+0xC8`
           for positive -- and every pixel already in the box is read and
           written back through that row. Truncating it to a byte is
           meaningless, which is why `publish` never reads `col` for it.
           `0x4AA912`'s `0x4BF4D0(panel+0xBC, rect, -0x18)` is darken level 24,
           NOT palette index 232.
         - `OP_RECT` / `0x4BF8C0` writes four edges through the store-only
           Bresenham `0x4CC7AB`, whose colour is `[ebp+0x1C]` stored
           `stos BYTE al` -- the low byte, a palette index. ESTABLISHED by
           landing 8b, which is why `publish` reads `col` for `OP_RECT` too.
         - `OP_FOCUS` / `0x4BF7B0` writes through `0x4BEC70` and is a TINT: its
           argument is a shade level into `globals+0xC8`, not a colour.
       [exe-reverse-engineering.md has all four, disassembled.
       The vulkan-only plan, landing 8a.] */
    unsigned char col;
    /* DID `clip_ctx` MOVE AN EDGE? Only `OP_RECT` reads it, and it is the
       difference between a hollow figure we may describe and one we may not.
       `rect_box` clamps the WHOLE BOX to the context's clip rect; the engine
       does not -- `0x4BF8C0` clips each of its four edges SEPARATELY through
       `0x4BEA20`, which draws nothing at all for an edge wholly outside. So a
       rect crossing its clip rect is an OPEN figure on screen, and replaying
       the clamped box would CLOSE it, with edges painted along the clip
       boundary the engine never drew. When this is set the op keeps the
       `PK_PIXELS` path, whose bytes come from the surface and are therefore
       exact whatever the engine did -- a fallback by construction, not a
       heuristic. `OP_BAR` does not need it: a clipped solid fill is the same
       fill restricted to the clip rect. [FOUND BY LANDING 8b'S REVIEW.] */
    unsigned char clipped;
    /* AND THE FONT IS RESOLVED AT OBSERVE TIME TOO (G19f-8), for the reason
       the sprite's plane is: `publish` runs up to CENSUS_MS after the draw and
       the font object is engine memory whose lifetime nothing here can state.
       `fid` is the slot id the consumer caches on (0: the font would not read,
       so this op is its box's bytes), `gboff`/`gblen` the glyph records in
       `s_glyBuf`, `frows`/`fyoff` the two header bytes the consumer needs, and
       `fgen` the `sent[]` generation those records were decided against.
       `frame` keeps the font's ADDRESS as an identity -- `op_same` tells two
       fonts apart with it and the probe prints it -- and after this landing
       nothing dereferences it. See `text_capture`. */
    unsigned fid;
    unsigned gboff; unsigned short gblen;
    unsigned char frows; signed char fyoff;
    unsigned fgen;
    unsigned char dup;                          /* an identical op follows: dropped */
} OP;
/* ---- THE UI FONTS, AS IDENTITIES AND BITS (landing 4c) -------------------
   The string op used to carry the engine's FONT OBJECT and the render thread
   dereferenced it — the header, the offset table and every glyph's packed rows
   — up to a queue backlog later, behind IsBadReadPtr, with no note anywhere
   establishing a UI font's lifetime. A probe is not a lifetime argument, so the
   bits cross instead: on FIRST SIGHT of a (font, code) pair, here, on the game
   thread, inside the flip the observer recorded the draw in.

   THE BOUND IS THE FORMAT, as the marker font's copy already had it (landing
   1): the object is the `.fnt` file image — `u16 height; u16 yoff; u16
   offset[256]; glyphs` (tools/guifont.py, decoded over the twenty stock faces)
   — so the offset table has 256 entries and an entry is a file offset into the
   block the loader read the file into.

   `f[3]` IS THE FIRST CODE, not "the high byte of the y-offset word" as this
   comment said until landing 4c's review corrected it: `0x4CCF77` loads
   `[esi+3]` and `0x4CCFAA` does `sub ebx,first / jb`, which is the blitter's
   only lower bound on a character. It is 0 for every stock face, which is what
   makes `f[3] != 0` a usable "this is not that format" test — but it is a
   REFUSAL of an unusual font and not a statement about the format, and a font
   with a real non-zero `first` would be refused with it. Refused means the op
   falls back to its box's captured pixels, which is what every text draw was
   before G17d, so the cost of being wrong here is a slower path and never a
   wrong glyph.

   A SLOT IS KEYED ON THE POINTER AND THE SIGNATURE. An allocator that hands the
   same address to a different font would otherwise serve the old font's glyphs;
   a changed signature takes a new slot and a new id, and the consumer's cache
   keys on the id, so nothing of the old font survives into the new one. */
#define GF_SLOTS    8
#define GF_LO       0x20
#define GF_HI       0xFF
#define GF_NCODE    (GF_HI - GF_LO + 1)
#define F_ROWS_L    0            /* font[0], the rows the blitter writes */
typedef struct {
    const unsigned char* font;
    unsigned char sig[3];              /* rows, yoff, first                    */
    unsigned      id;                  /* what the op and the cache key on     */
    unsigned char sent[GF_NCODE];      /* this code's bits have been published */
} GFONT;
static GFONT    s_gfont[GF_SLOTS];
static int      s_ngfont;
static unsigned s_gfontNext = 1;
/* `volatile` because `tagpu_gui_font_stats` reads them from the RENDER thread for the
   heartbeat while only the game thread writes them -- the convention every other
   producer counter follows in `g_guiq`. Aligned 32-bit stores on x86 cannot tear, so
   what this buys is that the render thread sees a figure rather than a cached one.
   [FOUND 2026-09-16 by the landing review: the reader is what G19f-8 added.] */
static volatile unsigned s_gfontRecycles, s_gfontRefused, s_gfontGlyphs, s_gfontResends;
/* ...and a reader for them, because a counter the heartbeat does not print is a
   measurement nobody ever reads -- which these four were from landing 4c until
   G19f-8 made them the way to see this mechanism work. */
void tagpu_gui_font_stats(unsigned* glyphs, unsigned* resends, unsigned* refused, unsigned* recycles)
{
    *glyphs = s_gfontGlyphs; *resends = s_gfontResends;
    *refused = s_gfontRefused; *recycles = s_gfontRecycles;
}
static unsigned s_gfontGenSeen;

/* THE CONSUMER'S ATLAS CAN THROW EVERY CELL AWAY, and this is what notices.
   `tagpu_text.c`'s glyph atlas resets on a shelf overflow and on a ninth font;
   our `sent[]` would otherwise still say those glyphs are published, and they
   would be missing from every string for the rest of the session — the string
   drawn with the characters dropped and the rest closed up. The generation is
   the render thread's, read here, monotone; when it moves every slot's `sent[]`
   is cleared and the next string re-sends what it needs [found by the landing
   review, twice]. */
/* THE GENERATION EVERY `sent[]` DECISION IS TAKEN AGAINST (G19f-8). `sent[]`
   says "the consumer already has this glyph", and TWO things clear it: the
   render thread throwing its glyph atlas away (below) and a publisher reseed
   skipping whole windows (`publish`). Until this landing both were safe by
   POSITION -- the decision was taken inside `publish`, after either clear had
   already happened in the same call. The capture now runs at OBSERVE time, so
   a clear can land between the decision and the flip, and a block that carries
   no records BECAUSE they were already sent would then be a string with
   characters missing from it for the rest of the session -- the exact failure
   `gfont_check_gen` was added to prevent. Stamped into the op, compared at
   publish, and a mismatch publishes the box instead. Same shape, and the same
   reason, as the sprite path's `s_seenGen`.

   A SLOT RECYCLE NEEDS NO BUMP OF ITS OWN, AND THE REASON IS NOT THE ONE THIS
   COMMENT GAVE. It said "nothing in flight is invalidated", which is backwards:
   both tables are eight deep, so the ninth `(font, sig)` that makes US recycle
   is also the ninth ID the CONSUMER sees, and `tagpu_text.c`'s own `gfont_slot`
   answers that with `memset(s_gf)` + `memset(s_gatlas)` + `s_ggen++` -- every
   cell under every old id, gone. A recycle therefore RELIABLY CAUSES a consumer
   clear rather than leaving everything valid. What makes it safe anyway is the
   generation: that `s_ggen++` is what the poll above sees, one window later at
   worst, and it re-arms every `sent[]` through the same path a shelf overflow
   does. `gfont=` prints the recycles and the resends side by side, so the two
   moving together is a cross-check on this paragraph rather than a hope.
   [CORRECTED 2026-09-16 by the cross-thread reviewer.] */
static unsigned s_sentGen = 1;
static void gfont_sent_clear(void)
{
    int i;
    for (i = 0; i < s_ngfont; i++) memset(s_gfont[i].sent, 0, sizeof s_gfont[i].sent);
    s_sentGen++;
}
static void gfont_check_gen(void)
{
    unsigned g = tagpu_text_glyph_gen();
    if (g == s_gfontGenSeen) return;
    s_gfontGenSeen = g;
    gfont_sent_clear();
    s_gfontResends++;
}

static GFONT* gfont_slot(const unsigned char* f)
{
    int i;
    GFONT* g;
    gfont_check_gen();
    /* the format test, not a probe: +3 is the high byte of the y-offset word
       and is 0 for every font of this format */
    if (f[3] != 0 || f[0] == 0) { s_gfontRefused++; return NULL; }
    for (i = 0; i < s_ngfont; i++)
        if (s_gfont[i].font == f && s_gfont[i].sig[0] == f[0] &&
            s_gfont[i].sig[1] == f[2] && s_gfont[i].sig[2] == f[3]) return &s_gfont[i];
    if (s_ngfont < GF_SLOTS) g = &s_gfont[s_ngfont++];
    else {
        /* every slot taken: start over rather than evict one font into an id
           another font's glyphs are cached under. The consumer keys on the id,
           which is never reused, so the old cells simply stop being asked for. */
        memset(s_gfont, 0, sizeof s_gfont);
        s_ngfont = 1; g = &s_gfont[0];
        s_gfontRecycles++;
    }
    memset(g, 0, sizeof *g);
    g->font = f; g->sig[0] = f[0]; g->sig[1] = f[2]; g->sig[2] = f[3];
    g->id = s_gfontNext++;
    return g;
}

/* one glyph's width and packed rows, or NULL when this font has no such code —
   exactly the two skips the blitter makes: 0x4CCFAA below `first` (f[3]) and
   0x4CCFB9 on a zero table entry. A ZERO WIDTH is refused here as it is in the
   render half's rasteriser: the engine's do-while at 0x4CCFCA/0x4CCFE9 wraps
   the counter to 255 and smears 256 columns of ink, and no stock font has one. */
static const unsigned char* gfont_glyph(const unsigned char* f, int code, int* w)
{
    int first = f[3], off;
    if (code < first) return NULL;
    off = *(const unsigned short*)(f + 4 + 2 * (code - first));
    if (!off) return NULL;
    *w = f[off];
    if (*w <= 0) return NULL;
    return f + off + 1;
}

/* ---- THE GLYPH RECORDS, WRITTEN WHERE THE ENGINE IS ABOUT TO READ THEM ----
   One walk of the string, writing `[code][w][nb:u16][bits][pad to 4]` for every
   code this font has not sent yet and this block does not already carry. It
   marks NOTHING: an op that never reaches the queue must not take its glyphs
   with it, so `glyph_block_mark` does the marking at publish, from the bytes
   that actually went into the arena. (Two publish-time walks, `glyph_block_size`
   then `glyph_block_fill`, did this before G19f-8; sizing first is pointless
   once the destination is a scratch we can bound per record.)

   OUR READ SET IS A SUBSET OF THE ENGINE'S, BYTE FOR BYTE. `0x4CCF60` has no
   clip and no destination bound: it reads `font[0]`, `font[2]`, `font[3]`, the
   `u16` table entry for every code of the string it walks, and each glyph's
   `rows*w` bits, then writes `sum(widths) * font[0]` pixels wherever the caller
   said (exe-reverse-engineering.md, "`0x4CCF60` -- the blitter, which takes its
   destination directly"). We walk the same string with the blitter's own two
   skips -- below `first` at `0x4CCFAA`, a zero table entry at `0x4CCFB9` -- and
   read the same bytes for a SUBSET of its codes. So this is not the GAF path's
   extent residual repeated: there the engine reads a clipped sub-rect while
   `tagpu_gaf_decode` reads all `w*h`, and here there is nothing we touch that
   the engine does not touch itself, on the next instruction.

   Returns the bytes written, or `GLY_REFUSED` when the scratch could not take
   the block WHOLE -- never a partial one, since half a record published as a
   full one is a glyph made of someone else's bits. */
#define GLY_REFUSED 0xFFFFFFFFu
static unsigned glyph_block_capture(GFONT* g, const unsigned char* f,
                                    const unsigned char* str, int n,
                                    unsigned char* dst, unsigned room)
{
    unsigned at = 0;
    int i, rows = f[F_ROWS_L], any = 0;
    unsigned char seen[GF_NCODE];
    for (i = 0; i < n; i++) {
        int c = str[i], w;
        const unsigned char* bits;
        unsigned nb, pad;
        if (c < GF_LO || c > GF_HI) continue;
        if (g->sent[c - GF_LO]) continue;
        /* `seen` is cleared on the FIRST unsent code and not before: this walk
           now runs inside the engine's blit rather than at the flip, and a
           settled screen -- every code of every label already sent -- must cost
           the walk and nothing else. */
        if (!any) { memset(seen, 0, sizeof seen); any = 1; }
        if (seen[c - GF_LO]) continue;
        bits = gfont_glyph(f, c, &w);
        if (!bits) continue;
        seen[c - GF_LO] = 1;
        nb = ((unsigned)(rows * w) + 7u) / 8u;
        pad = (nb + 3u) / 4u * 4u;
        if (4u + pad > room - at) return GLY_REFUSED;
        dst[at + 0] = (unsigned char)c;
        dst[at + 1] = (unsigned char)w;
        dst[at + 2] = (unsigned char)(nb & 0xFFu);
        dst[at + 3] = (unsigned char)(nb >> 8);
        memcpy(dst + at + 4, bits, nb);
        if (pad > nb) memset(dst + at + 4 + nb, 0, pad - nb);
        at += 4u + pad;
    }
    return at;
}

/* A GLYPH IS SENT ONCE IT IS IN THE ARENA, AND NOT BEFORE. Walks the block that
   was just copied, marks each code against the font's slot, and returns the
   record COUNT -- which is what `gcount` has to be for the consumer to find the
   string that follows them (`tagpu_text_glyph_block_bytes`). It reads no font:
   the walk is over our own bytes, in our own format.

   A slot RECYCLED between the capture and here is simply not found, and the
   block still publishes: the consumer keys on the id, ids are never reused, so
   those cells land under an id nothing will ask for again and the next window
   re-sends what it needs. The length test in the loop can never fire on a block
   this file wrote and is there so that it cannot walk off one that it did. */
static unsigned glyph_block_mark(unsigned fid, const unsigned char* blk, unsigned len)
{
    GFONT* g = NULL;
    unsigned at = 0, k = 0;
    int i;
    for (i = 0; i < s_ngfont; i++) if (s_gfont[i].id == fid) { g = &s_gfont[i]; break; }
    while (at + 4u <= len) {
        unsigned nb  = (unsigned)blk[at + 2] | ((unsigned)blk[at + 3] << 8);
        unsigned pad = (nb + 3u) / 4u * 4u;
        int c = blk[at];
        if (at + 4u + pad > len) break;
        if (g && c >= GF_LO && c <= GF_HI) g->sent[c - GF_LO] = 1;
        at += 4u + pad;
        k++;
        s_gfontGlyphs++;
    }
    return k;
}

/* The batch's strings. Reset with s_nops, and bounded the same way: a census is
   ~5 ms of drawing, in which the whole UI redraws a few hundred short labels. */
#define STR_SCRATCH (64u << 10)
static unsigned char s_strBuf[STR_SCRATCH];
static unsigned s_strUsed;
static unsigned s_strLost;                      /* strings the scratch could not take */

/* THE BATCH'S SPRITE PLANES, for the same reason and reset the same way.
   Sized for one census window's FIRST SIGHTS, not for the screen: `seen_frame`
   means a frame is decoded once and then never again, so what has to fit is the
   art a single screen build introduces. The shell's whole inventory is 19 atlas
   entries. A full 640x480 GAF would be 307 200 bytes on its own, so the bound
   is generous rather than tight -- and being wrong is a slower path and never a
   wrong picture: a frame the scratch cannot take publishes its box's bytes,
   exactly as an undecodable one always has. `s_gafLost` counts those. */
#define GAF_SCRATCH (2u << 20)
static unsigned char s_gafBuf[GAF_SCRATCH];
static unsigned s_gafUsed;
/* AND THE BATCH'S GLYPH RECORDS (G19f-8), for the same reason and dropped by
   the same reset. A SETTLED SESSION WRITES NOTHING HERE: `sent[]` means a
   (font, code) pair is captured once and never again, so what has to fit is the
   glyphs a screen's first window introduces -- about 16 bytes a code for a
   12-row face, a few thousand for a whole screen of labels. 128 KB is generous
   rather than tight, and `glyhigh` is what says so rather than this comment.

   THERE IS NO CROSS-OP DEDUP INSIDE A WINDOW, ON PURPOSE. Two ops needing the
   same unsent code each carry it: `sent[]` is not marked until publish, and
   either op may be the one that never gets there (a dedup drop, a queue
   overflow, an untwinned surface). The sprite path can share a decode through
   `s_gcap` because its consumer keys on the frame and one copy serves every op;
   a glyph record is only ever read out of the op that carries it. What the
   scratch cannot take publishes its box's bytes instead, which is what a text
   op did before G17d -- slower, never wrong. */
#define GLY_SCRATCH (128u << 10)
static unsigned char s_glyBuf[GLY_SCRATCH];
static unsigned s_glyUsed;
/* `gaflost` and `gafhigh` live in g_guiq rather than here for the reason
   `gafstale` was moved there: a counter the heartbeat does not print is a
   measurement nobody ever reads. */
static OP* s_lastOp = NULL;                     /* the op op_add just recorded */
#define MAX_OPS 65536
static OP       s_ops[MAX_OPS];
static int      s_nops = 0;
/* ops recorded against a base are dead once the object is gone or re-made: a
   new surface may be allocated over the same bytes before the next census, and
   neither the census nor the publisher may apply an old box to it */
static void ops_forget_base(unsigned base)
{
    int k;
    for (k = 0; k < s_nops; k++) if (s_ops[k].base == base) s_ops[k].base = 0;
}
static unsigned s_kindCount[OP_NKIND];
static unsigned s_kindTotal[OP_NKIND];          /* cumulative, for the heartbeat */
static unsigned s_nullCtx[OP_NKIND];        /* ops whose ctx was NULL/unknown */

static void op_add(int kind, SURF* s, int l, int t, int r, int b)
{
    OP* o;
    /* EVERY early return below must leave s_lastOp NULL: the callers (gaf_box,
       before_copy) decorate "the op just recorded" with the frame identity or
       the copy's source, and a blit that recorded nothing — a fully clipped
       glyph, a NULL surface — must not write those into the PREVIOUS op.
       (MEASURED 2026-09-07: the last glyph of ARMOPT's "Exit" label lost its
       frame to the fully clipped blit that followed it — 67 px at 1024×768.) */
    s_lastOp = NULL;
    s_kindCount[kind]++;
    s_kindTotal[kind]++;
    s_opsTotal++;
    if (!s) { s_nullCtx[kind]++; return; }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > s->w - 1) r = s->w - 1;
    if (b > s->h - 1) b = s->h - 1;
    if (l > r || t > b) return;
    if (s_nops >= MAX_OPS) { s_opsDropped++; return; }
    o = &s_ops[s_nops++];
    memset(o, 0, sizeof *o);
    o->base = s->base; o->l = (short)l; o->t = (short)t; o->r = (short)r; o->b = (short)b;
    o->kind = (unsigned char)kind;
    s_lastOp = o;
}

/* clip a box to the context's clip rect (inclusive) */
static void clip_ctx(const int* ctx, int* l, int* t, int* r, int* b)
{
    if (*l < ctx[CTX_CLIP_L]) *l = ctx[CTX_CLIP_L];
    if (*t < ctx[CTX_CLIP_T]) *t = ctx[CTX_CLIP_T];
    if (*r > ctx[CTX_CLIP_R]) *r = ctx[CTX_CLIP_R];
    if (*b > ctx[CTX_CLIP_B]) *b = ctx[CTX_CLIP_B];
}

static int on_game_thread(void)
{
    return s_gameTid != 0 && GetCurrentThreadId() == s_gameTid;
}

/* The flip counter, for the frame packet to echo (tagpu_packet_pub.c): the
   twin travels on this module's queue and the packet on the exchange, at
   different cadences, and the heartbeat counts the skew between them. Game
   thread reads only; 0 for ever when the layer is not armed. */
unsigned tagpu_gui_flips(void) { return s_flips; }

/* ---- the publisher (game thread -> tagpu_gui_surf.c) ------------------- */

TAGPU_GUIQ g_guiq;                    /* the queue; storage below              */
static TAGPU_PUBOP    s_qops[TAGPU_GUI_QCAP];
static unsigned char* s_arena;
volatile int g_gui_draw = 0;          /* set by the render thread's trigger poll */
static int   s_pubOverflow = 0;
static unsigned s_pubOps = 0, s_pubBytes = 0;

/* sprite frames whose bytes were already published (open addressing) */
#define SEEN_N 8192
static const void* s_seenF[SEEN_N];
static const void* s_seenP[SEEN_N];
/* THE SEEN TABLE'S GENERATION, so a stale decision can be told from a failure.
   `gaf_capture` skips the decode when the table already has the frame -- the
   consumer has those bytes, so nobody needs them again. A RESET then clears the
   table (the consumer threw its atlas away), and the ops already recorded in
   that window carry a decision taken against the old table: they believed the
   bytes were sent, and after the clear they are not. Those ops publish their
   box instead, which costs nothing at all because a reset re-seeds every
   surface whole in the same publish -- but it is a different event from "the
   scratch was full", and one counter for both would have read as 3923 failures
   on the first session that measured it. [MEASURED 2026-09-16: 3923 of them,
   15 resets, and `gaflost` 0.] */
static unsigned s_seenGen = 1;
static unsigned hash_ptr(const void* a, const void* b)
{
    unsigned x = (unsigned)(size_t)a * 2654435761u ^ ((unsigned)(size_t)b >> 3) * 40503u;
    return (x ^ (x >> 15)) & (SEEN_N - 1);
}
static int seen_frame(const void* f, const void* p, int add)
{
    unsigned i = hash_ptr(f, p), n;
    for (n = 0; n < SEEN_N; n++, i = (i + 1) & (SEEN_N - 1)) {
        if (!s_seenF[i]) { if (add) { s_seenF[i] = f; s_seenP[i] = p; } return 0; }
        if (s_seenF[i] == f && s_seenP[i] == p) return 1;
    }
    return 1;                          /* full: claim seen, the consumer copes */
}

static TAGPU_PUBOP* pub_op(int kind, unsigned surf)
{
    unsigned head = g_guiq.qHead, tail = g_guiq.qTail;
    TAGPU_PUBOP* o;
    if (head - tail >= TAGPU_GUI_QCAP - 1) { s_pubOverflow = 1; g_guiq.why = TAGPU_GUI_WHY_QUEUE; return NULL; }
    o = &s_qops[head & (TAGPU_GUI_QCAP - 1)];
    memset(o, 0, sizeof *o);
    o->kind = (unsigned char)kind; o->surf = surf; o->flip = s_flips;
    return o;
}
static void pub_commit(void) { MemoryBarrier(); g_guiq.qHead++; s_pubOps++; }

/* bytes for an op: the arena slot, or NULL when there is no room */
static unsigned char* pub_bytes(TAGPU_PUBOP* o, unsigned len)
{
    unsigned at;
    if (!tagpu_guiq_arena_room(&g_guiq, len, &at)) { s_pubOverflow = 1; g_guiq.why = TAGPU_GUI_WHY_ARENA; return NULL; }
    o->aoff = at; o->alen = len;
    g_guiq.aHead = at + len;
    s_pubBytes += len;
    return s_arena + at;
}

/* a surface's bytes, rows packed: the seed, or a box */
static int pub_surface_bytes(SURF* s, int l, int t, int r, int b, TAGPU_PUBOP* o)
{
    const unsigned char* cur = (const unsigned char*)(size_t)s->base;
    unsigned w = (unsigned)(r - l + 1), hh = (unsigned)(b - t + 1);
    unsigned char* dst;
    int y;
    if (!ptr_ok(cur) || l < 0 || t < 0 || r >= s->w || b >= s->h || l > r || t > b) {
        /* a box recorded against a surface that has since changed size (or a
           base we cannot read): the batch stops here and the next publish
           starts fresh — never a silent drop that leaves a twin stale */
        s_pubOverflow = 1; g_guiq.why = TAGPU_GUI_WHY_BOX;
        return 0;
    }
    dst = pub_bytes(o, w * hh);
    if (!dst) return 0;
    for (y = 0; y < (int)hh; y++)
        memcpy(dst + (size_t)y * w, cur + (size_t)(t + y) * s->pitch + l, w);
    return 1;
}

static int pub_seed(SURF* s)
{
    TAGPU_PUBOP* o = pub_op(PK_SEED, s->base);
    if (!o) return 0;
    o->w = s->w; o->h = s->h; o->pitch = s->pitch;
    o->l = 0; o->t = 0; o->r = (short)(s->w - 1); o->b = (short)(s->h - 1);
    if (!pub_surface_bytes(s, 0, 0, s->w - 1, s->h - 1, o)) return 0;
    pub_commit();
    s->seeded = 1;
    return 1;
}

/* THE SPRITE IDENTITY. The atlas and the seen table key a frame on its
   header and pixel-plane addresses, and the shell frees a popped screen's
   art and hands the same addresses to the next screen's: the same key would
   then name different pixels. So the key also carries a hash of the plane's
   first bytes (the row lengths and data of the first rows, up to 64 bytes),
   read here at publish time under the same guard the first-sight decode
   uses. NULL = the plane cannot be read now: the caller publishes the box's
   bytes instead, as it does when the decode fails. */
static const void* frame_key(const unsigned char* fr, const void* pix, int w, int h)
{
    const unsigned char* px = (const unsigned char*)pix;
    unsigned hh = 2166136261u, i, n = 0;
    /* `fr` IS CHECKED, AND IT WAS NOT. This function guarded the PIXEL pointer
       and then dereferenced the FRAME HEADER on the next line, where every
       other caller of the GAF resolvers in this tree -- tagpu_fx.c,
       tagpu_feat.c, tagpu_render3do.c, tagpu_gui_surf.c -- puts the header
       through `tagpu_gaf_frame_sane` first. It is a BOUND on a value and it is
       NOT the reason this read is safe: that is the caller's ordering against
       the level teardown (see `publish`). This is the parity the module already
       had everywhere else, kept so a header that is merely garbage rather than
       unmapped is refused rather than hashed. */
    if (!tagpu_gaf_frame_sane(fr)) return NULL;
    if (!ptr_ok(px)) return NULL;
    if (fr[0x09] == 0) {                             /* raw: w*h bytes exist */
        n = (unsigned)w * (unsigned)h; if (n > 64) n = 64;
        if (IsBadReadPtr(px, n)) return NULL;
        for (i = 0; i < n; i++) hh = (hh ^ px[i]) * 16777619u;
    } else {                                         /* RLE: [len][data] per row */
        const unsigned char* q = px;
        int row;
        for (row = 0; row < h && n < 64; row++) {
            unsigned len, k;
            if (IsBadReadPtr(q, 2)) return NULL;
            len = *(const unsigned short*)q;
            if (len > 8192 || IsBadReadPtr(q, 2 + len)) return NULL;
            for (k = 0; k < 2 + len && n < 64; k++, n++) hh = (hh ^ q[k]) * 16777619u;
            q += 2 + len;
        }
    }
    hh ^= (unsigned)(size_t)pix * 2654435761u;
    hh ^= (unsigned)(unsigned short)*(const short*)(fr + 0x04) << 16;     /* the hotspot */
    hh ^= (unsigned)(unsigned short)*(const short*)(fr + 0x06);
    return (const void*)(size_t)(hh ? hh : 1u);
}

/* ---- THE SPRITE, RESOLVED WHERE IT IS PROVABLY ALIVE (G19f-7) ------------
   `publish` used to take the identity hash AND the decoded plane out of engine
   memory, up to CENSUS_MS after the blit that recorded the op. Between those
   two moments the engine is free to release the art, and it does: the level
   teardown's cascade frees the per-level GAF banks, and `GUI_Pop 0x4A9660`
   frees a popped screen's from 39 call sites with no flag and no generation at
   all. Landing 5 closed the teardown with an ordering against the level
   generation and left the pop open, covered only by a BOUND -- which this
   module says twice over is not a safety argument.

   THE ORDERING IS ON THE ENGINE'S TIMELINE, AND IT COVERS LIFETIME ONLY.
   State it precisely, because the first version of this comment did not. This
   runs from `gaf_box`, a `before_` detour on the blit leaf, so:

       our read  <  the engine's blit  <  the engine's free

   The free routes that opened this window -- the teardown cascade and
   `GUI_Pop 0x4A9660` -- all run AFTER the blit they follow, and the caller
   holds the art alive across the call it is currently making. Our read
   precedes that call. That is the ordering, and it needs no flag, no
   generation and no `MEM_Free` block size. It covers all 39 pop sites, the
   cascade, and any free route nobody has found yet, because it does not
   enumerate them: it is earlier than all of them.

   **It is NOT the claim that "the engine would fault if this were dead".**
   The detour runs BEFORE the engine's read, so if the memory were dead WE
   would fault first; that phrasing was a counterfactual dressed as a proof and
   the landing review was right to say so.

   **AND IT DOES NOT BOUND THE EXTENT.** The engine reads the CLIPPED sub-rect;
   `tagpu_gaf_decode` reads all `w*h`, or every RLE row. So a header whose `w`/
   `h` exceed the plane the loader actually allocated is not covered by anything
   above -- only by `tagpu_gaf_frame_sane`, which is a SHAPE test (w,h <= 512),
   and the decoder's own `IsBadReadPtr`, which this file says everywhere is not
   a safety argument. That residual is unchanged by this landing: the same
   decode read the same bytes at publish before it, over memory that might also
   have been freed. Moving it strictly removes the lifetime half and leaves the
   extent half exactly where it was. Named, not fixed: bounding it needs the
   plane's allocated length, and the plane is not a block start, so `MEM_Size`
   cannot answer it either. [Extent residual raised by the landing review.]

   After this, `publish` dereferences no engine asset memory on this path at
   all -- the `gui probe:` trace included, which was the last one left.
   `frame`/`pix` survive in the op as the consumer's atlas KEY, a value compared
   against a table and never followed.

   IT ALSO FIXES A WRONG-ART CASE THE GENERATION GATE COULD NOT SEE. The key is
   a hash of the plane's first bytes precisely because the shell hands a freed
   screen's addresses to the next screen's art. Taken at publish time, that hash
   was read from whatever the address held THEN -- so art freed and replaced
   inside one census window hashed the NEW content under the OLD op, and the
   consumer matched a key that named pixels the op never drew. Taken here, the
   hash is of the bytes the engine is about to blit, which is the only content
   the op ever meant.

   The plane is decoded only on FIRST SIGHT, exactly as `publish` did it -- a
   settled session decodes nothing -- and `s_gcap` keeps one window from
   decoding the same new frame once per gadget that shares it. The scratch is a
   bound: what it cannot take publishes its box's bytes instead, which is what
   an undecodable frame has always done. Slower, never wrong.
   [BUILT 2026-09-16, after the landing-5 re-review named the pop window and
   said the by-design fix wanted a block size. It wanted a different hook.] */
#define GCAP_N 256
static struct { const void* fr; unsigned key, off, len; } s_gcap[GCAP_N];  /* this window's decodes */

/* THE WINDOW'S SCRATCHES, DROPPED WITH ITS OPS -- all of them, here, because
   an op's `soff`/`goff`/`gboff` are offsets into them and an op that outlived
   its bytes would publish whatever now sits at that offset. `s_gcap` MUST go
   too: its entries are offsets into `s_gafBuf`. The three call sites set
   `s_nops = 0` and call this; the string scratch used to be reset beside
   `s_nops` at each of them instead, which is one more place for the next
   scratch to be forgotten. */
static void ops_window_reset(void)
{
    s_nops = 0;
    s_strUsed = 0;
    s_glyUsed = 0;
    s_gafUsed = 0;
    memset(s_gcap, 0, sizeof s_gcap);
}

static void gaf_capture(OP* o, const unsigned char* fr)
{
    const void* key = frame_key(fr, o->pix, o->fw, o->fh);
    unsigned n, i;
    unsigned char* dst;
    o->fkey = (unsigned)(size_t)key;
    o->goff = o->glen = 0;
    o->sgen = s_seenGen;
    if (!key) return;                       /* unreadable now: publish the box */
    if (seen_frame(fr, key, 0)) return;      /* the consumer already has it     */
    /* the same new frame twice in one window -- 39 gadgets sharing one button
       face -- reuses the first decode. Direct-mapped, and matched on ALL THREE
       of the frame pointer, the key and the length. The first version matched
       the key and the length only, and the comment over it claimed the length
       test made the reuse "safe whatever the hash does" -- which was true of
       the SIZE and of nothing else: two distinct frames of equal `fw*fh`
       colliding in `frame_key` inside one window would have had the second op
       handed the first's plane and published under its own address key, and the
       consumer would have atlased the wrong pixels. Comparing `fr` costs one
       load and makes the reuse exact, so a collision can only ever cost a
       second decode. (Matching the pointer is sound HERE and only here:
       `s_gcap` lives for one census window and is cleared with it, so an
       address cannot be recycled inside its lifetime -- which is exactly why
       the ATLAS, which lives for a session, may not match on one.)
       `frame_key` never returns 0, so 0 is a free empty marker.
       [FOUND 2026-09-16 by the landing review.] */
    n = (unsigned)o->fw * (unsigned)o->fh;
    i = o->fkey & (GCAP_N - 1);
    if (s_gcap[i].fr == (const void*)fr && s_gcap[i].key == o->fkey && s_gcap[i].len == n) {
        o->goff = s_gcap[i].off; o->glen = s_gcap[i].len; return;
    }
    if (!n || n > GAF_SCRATCH - s_gafUsed) { g_guiq.gaflost++; return; }
    dst = s_gafBuf + s_gafUsed;
    /* NOT `gaflost`: that one means "the scratch could not take it", which is a
       statement about the bound, and folding an undecodable frame into it would
       make a sizing figure move for a reason that has nothing to do with size.
       An unreadable or malformed frame is the decoder's own refusal and has
       always fallen through to the box's bytes. [FOUND 2026-09-16, the landing
       review, which noticed the declaration and the two call sites disagreed.] */
    if (!tagpu_gaf_decode(fr, o->fw, o->fh, dst)) { g_guiq.gafbaddec++; return; }
    o->goff = s_gafUsed; o->glen = n;
    s_gcap[i].fr = (const void*)fr; s_gcap[i].key = o->fkey; s_gcap[i].off = o->goff; s_gcap[i].len = n;
    s_gafUsed += n;
    if (s_gafUsed > g_guiq.gafhigh) g_guiq.gafhigh = s_gafUsed;
}

/* ---- THE FONT THIS OP NAMES CANNOT GO AWAY, BECAUSE NOTHING NAMES IT LATER
   (G19f-8, the same move as G19f-7 made for the sprite).

   `publish` used to take the font slot, the offset table and every unsent
   glyph's packed rows out of the object at the flip -- `gfont_slot`,
   `glyph_block_size` and `glyph_block_fill`, up to CENSUS_MS after the draw
   that recorded the op. What stood in for a lifetime there was a level
   generation and `ptr_ok`, and neither is one: the generation says the level
   has not ENDED, which is not the same as the font still being mapped, and a
   range test on a value is a filter and never an ordering. The note said so and
   left the window open rather than let the line above it look closed.

   THE ORDERING, ON THE ENGINE'S OWN TIMELINE. This runs from the detour at the
   head of `0x4CCF60`, the glyph blitter, with the engine's own `font` and `str`
   arguments in hand. The engine is committed to reading `font[0]`, `font[2]`,
   `font[3]`, the table entry for every code of that string and each glyph's
   bits before it returns, so:

       our read  <  the engine's read  <  any free of the font

   holds by the engine's sequencing rather than by our hope. It is NOT "the
   engine would fault if this were dead" -- the detour runs FIRST, so we would
   fault first; that phrasing was a counterfactual dressed as a proof when the
   G19f-7 review found it on the sprite path, and it is no better here. What
   makes the read safe is that the engine has already decided to make it.

   AND THE EXTENT HALF IS CLOSED TOO, which it is not on the sprite path: we
   read a SUBSET of the bytes the blitter reads, code for code (see
   `glyph_block_capture`). The GAF path's residual -- the engine blits a clipped
   sub-rect while we decode all `w*h` -- has no counterpart here, because
   `0x4CCF60` has no clip at all.

   AFTER THIS, `publish` DEREFERENCES NO PER-LEVEL ENGINE ASSET ON ANY PATH --
   no GAF bank and no font object, the two whose lifetimes nothing here can
   state. It is NOT "no engine memory at all", which would be an overclaim: it
   still reads the graphics globals through `TA_MAINPP` for the true viewport
   rect, and `pub_surface_bytes` still reads the surface's own pixels. Both have
   stated lifetimes -- the globals are process-lifetime and the surfaces are the
   FORK's, freed in their own Release (see `surf_of_ctx`) -- which is exactly
   what a GAF bank and a font do not have. `frame` survives in a text op as the
   font's address, an identity `op_same` compares and the probe prints, never
   followed. `op->lgen` and `strstale` are gone with the gate they existed for,
   and `tagpu_packet_pub_level_tracked` and `tagpu_reclaim_level_closing` have
   no caller left in the tree.

   NOT COVERED. A font whose header lies -- a table entry pointing outside the
   loaded file image, a width byte that runs the bits past its end -- is refused
   only by `f[3] != 0`, `f[0] == 0` and `gfont_glyph`'s zero tests, exactly as
   before; the engine would read the same wrong bytes one instruction later, so
   this landing neither adds nor removes that. `ptr_ok(font)` in `before_text`
   is a BOUND on the value and is not the safety argument; the ordering is. */
static void text_capture(OP* o, const unsigned char* f, const unsigned char* str, int n)
{
    GFONT* g;
    unsigned room, at, len;
    o->fid = 0; o->gboff = 0; o->gblen = 0;
    g = gfont_slot(f);                          /* also polls the consumer's gen */
    if (!g) return;                             /* not that format: the box's bytes */
    at   = s_glyUsed;
    room = GLY_SCRATCH - s_glyUsed;
    if (room > 0xFFFFu) room = 0xFFFFu;         /* so `gblen` cannot be truncated */
    len = glyph_block_capture(g, f, str, n, s_glyBuf + at, room);
    if (len == GLY_REFUSED) { g_guiq.glylost++; return; }
    /* every field the publisher needs, decided here and now: the slot's id, the
       two header bytes the consumer stamps quads with, and the generation the
       "already sent" half of the block was decided against */
    o->fid   = g->id;
    o->frows = f[F_ROWS_L];
    o->fyoff = (signed char)f[2];
    o->fgen  = s_sentGen;
    o->gboff = at;
    o->gblen = (unsigned short)len;
    s_glyUsed += len;
    if (s_glyUsed > g_guiq.glyhigh) g_guiq.glyhigh = s_glyUsed;
}

static SURF* surf_by_base(unsigned base)
{
    int i;
    for (i = 0; i < s_nsurf; i++) if (s_surf[i].base == base) return &s_surf[i];
    return NULL;
}

/* everything the census ring holds, in order, becomes published ops; the
   surface's bytes are read NOW (inside the flip, the frame complete), which
   makes a pixel op the final state of its box and the twin converge on the
   engine's surface whatever the order of the ops that wrote it */
/* THE SHELL REDRAWS EVERY GADGET ON EVERY FLIP (MEASURED 2026-09-07: ~41 ops
   per flip at ~12 000 flips a second on MAINMENU, the same with the layer on
   or off), and every one of those redraws is identical. A batch therefore
   keeps only the LAST of any run of identical ops: the final state of the
   surface is the same, because an op's replay is idempotent and the last
   occurrence is the one whose position in the order matters. The one reader
   in the op set is the COPY: dropping an earlier duplicate is safe unless a
   copy that READ its surface lies between the two with none after the
   survivor — then the replay would run the copy before the write it read.
   (A per-surface epoch bumped by every copy was tried first and defeated the
   whole dedup in the shell, whose panel is copied to the frame on every one
   of its ~12 000 flips a second: a reseed storm, 2 749 resets in one walk.) */
/* open addressing over the batch: 2x the ring's capacity keeps the load
   under a half, and a probe that runs long stops and calls the op distinct
   (a stray duplicate costs one idempotent replay, not a stall on the game
   thread inside the flip) */
#define DUP_TAB (MAX_OPS * 2)
#define DUP_PROBE_MAX 64
static int s_dupTab[DUP_TAB];
static unsigned op_hash(const OP* o)
{
    unsigned h = (unsigned)o->kind * 0x9E3779B1u;
    h ^= o->base * 0x85EBCA6Bu; h ^= (unsigned)(unsigned short)o->l * 0xC2B2AE35u; h ^= (unsigned)(unsigned short)o->t * 0x27D4EB2Fu;
    h ^= (unsigned)(unsigned short)o->r * 0x165667B1u; h ^= (unsigned)(unsigned short)o->b * 0xD3A2646Cu;
    h ^= (unsigned)(size_t)o->frame * 0xFD7046C5u; h ^= (unsigned)(size_t)o->pix * 0xB55A4F09u;
    h ^= o->src * 0x2C1B3C6Du; h ^= (unsigned)(unsigned short)o->sl * 0x297A2D39u; h ^= (unsigned)(unsigned short)o->st * 0x4F6B9E23u;
    h ^= h >> 16;
    return h & (DUP_TAB - 1);
}
static int op_same(const OP* a, const OP* b)
{
    if (!(a->kind == b->kind && a->base == b->base && a->l == b->l && a->t == b->t && a->r == b->r && a->b == b->b &&
          a->frame == b->frame && a->pix == b->pix && a->src == b->src && a->sl == b->sl && a->st == b->st))
        return 0;
    /* G17d: TWO STRINGS IN ONE BOX ARE NOT THE SAME OP. Until the string op
       existed a text draw published its box's bytes, read at publish time, so
       collapsing two draws over the same rectangle was exactly right — the
       later read carried both. A string op carries the string, so dropping the
       earlier one would drop whatever ink of it the later one does not cover. */
    if (a->kind == OP_TEXT)
        return a->slen == b->slen && a->dx == b->dx && a->dy == b->dy &&
               a->fg == b->fg && a->bg == b->bg && a->tr == b->tr &&
               /* G19f-8: and the SLOT, not just the address the prefix above
                  compared. A font reloaded at its old address takes a new slot
                  and a new id, and the consumer caches on the id -- so two ops
                  whose only difference is which of them the consumer has the
                  glyphs for are not the same op. */
               a->fid == b->fid &&
               (a->slen == 0 || !memcmp(s_strBuf + a->soff, s_strBuf + b->soff, a->slen));
    return 1;
}
static void dedup(void)
{
    int i;
    memset(s_dupTab, 0, sizeof s_dupTab);
    /* where the last copy that reads each surface sits in this batch */
    for (i = 0; i < s_nsurf; i++) s_surf[i].lastCopyFrom = -1;
    for (i = 0; i < s_nops; i++)
        if (s_ops[i].kind == OP_COPY) { SURF* src = surf_by_base(s_ops[i].src); if (src) src->lastCopyFrom = i; }
    for (i = 0; i < s_nops; i++) {
        OP* o = &s_ops[i];
        unsigned slot, n;
        o->dup = 0;
        if (o->kind == OP_FLIP) continue;
        slot = op_hash(o);
        for (n = 0; n < DUP_PROBE_MAX; n++, slot = (slot + 1) & (DUP_TAB - 1)) {
            if (!s_dupTab[slot]) { s_dupTab[slot] = i + 1; break; }
            if (op_same(&s_ops[s_dupTab[slot] - 1], o)) {
                int j = s_dupTab[slot] - 1;
                SURF* d = surf_by_base(o->base);
                int lc = d ? d->lastCopyFrom : -1;
                if (lc < j || lc > i) s_ops[j].dup = 1;      /* no copy read the surface between, or one follows */
                s_dupTab[slot] = i + 1;
                break;
            }
        }
    }
}

static const char* const WHY_NAME[TAGPU_GUI_WHY_N] =
    { "?", "arm", "gl-context", "queue-full", "arena-full", "box-outside-surface", "lost-sprite", "atlas-full", "untwinned-copy", "stall-over", "string-empty", "level-changed" };

/* THE CONSUMER CAN DIE, OR CRAWL. cnc-ddraw stops its render thread inside
   every SetDisplayMode and starts a new one with a new GL context (dd.c);
   between the two nothing drains the queue, and on the way out of a game the
   old thread presents only every few hundred ms while the game thread is in
   the exit path — and the game thread keeps flipping and this keeps
   publishing: a batch every 5 ms carrying ~150 KB of pixel bytes in game
   (the box bytes of every non-sprite op, re-read at each cadence), so the
   16 MB arena is half a second of backlog. A queue nobody reads fills; the
   overflow policy then resets and re-seeds into the full arena at every
   publish (MEASURED 2026-09-07: 24 `arena-full` resets in the 120 ms after
   the exit click, 3 643 ops queued, the tail still creeping — so a time rule
   alone never fired). So two rules say the consumer is behind: the tail has
   not moved for TAGPU_GUI_STALL_MS with work queued (dead), or the backlog
   is past half the arena or a quarter of the ring (crawling). Either way the
   batch is dropped — nothing is queued, no counter but `stalls` moves — until
   the consumer has caught up (the queue empty, or the backlog under the
   low-water marks), when one reseed brings the twins back from the surfaces
   as they are then. Returns 1 to drop. */
static unsigned arena_used(void)
{
    unsigned head = g_guiq.aHead, tail = g_guiq.aTail;     /* offsets, wrapping */
    return head >= tail ? head - tail : TAGPU_GUI_ASIZE - (tail - head);
}
static int consumer_stalled(void)
{
    static unsigned s_tailSeen = 0;
    static LARGE_INTEGER s_tailQpc, s_fq;
    static int s_stalled = 0;
    unsigned tail = g_guiq.qTail, head = g_guiq.qHead, queued = head - tail, used = arena_used();
    LARGE_INTEGER now;
    if (!s_fq.QuadPart) QueryPerformanceFrequency(&s_fq);
    QueryPerformanceCounter(&now);
    if (s_stalled) {
        if (head == tail || (queued < TAGPU_GUI_QCAP / 16 && used < TAGPU_GUI_ASIZE / 8)) {
            s_stalled = 0; s_tailSeen = tail; s_tailQpc = now;
            g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_STALL;
            return 0;
        }
        return 1;
    }
    if (queued >= TAGPU_GUI_QCAP / 4 || used >= TAGPU_GUI_ASIZE / 2) { s_stalled = 1; g_guiq.stalls++; return 1; }
    if (tail != s_tailSeen || head == tail) { s_tailSeen = tail; s_tailQpc = now; return 0; }
    if ((now.QuadPart - s_tailQpc.QuadPart) * 1000 < (LONGLONG)TAGPU_GUI_STALL_MS * s_fq.QuadPart) return 0;
    s_stalled = 1; g_guiq.stalls++;
    return 1;
}

static void publish(unsigned flipSurf)
{
    int i;
    SURF* fs;
    int vl = 0, vt = 0, vr = -1, vb = -1;
    if (!g_gui_draw) return;
    if (consumer_stalled()) return;
    dedup();
    /* ---- THE LEVEL BOUNDARY, FOR THE CONSUMER'S ATLAS (G19f-7, found by the
       landing review -- BOTH reviewers, independently, the eleventh such pair
       on this lane).

       `tagpu_gui_surf.c`'s UI atlas matches entries on `(o->frame, o->pix,
       fw, fh)` -- the frame's ADDRESS and its content hash -- and its only
       resets are `twins_reset`, the atlas filling, and a GL context loss.
       NONE of those is a level boundary. The engine frees a level's GAF banks
       and the next level's loader may hand a new frame an old one's address;
       `frame_key` hashes only the plane's first 64 bytes plus the hotspot, so
       UI art whose first RLE row is one transparent run can collide by
       CONSTRUCTION rather than by 2^-32 luck, and then `atlas_find` hits the
       old entry and the twin draws the previous level's texels.

       THE GATE THIS LANDING REMOVED WAS NEVER THE COVER FOR THIS, which is
       worth saying plainly because it would be the obvious thing to assume:
       `op->lgen != level_gen` refused ops RECORDED before a boundary and
       RESOLVED after one -- a ~5 ms window -- and did nothing at all about
       entries already sitting in the consumer's atlas from the previous
       level. Those survived it. So this is a hole the old code had too, and
       the fix is a drop rather than a refusal.

       A reseed is the whole of it: `PK_RESET` makes the consumer call
       `twins_reset`, which calls `tagpu_gaf_atlas_reset` on that atlas, and
       the UI atlas never asks for a repack so that goes straight to
       `atlas_drop` -- every entry, every hash. Same shape as
       `tagpu_r3d_atlas_level` for the unit atlas, one level down the stack.
       The first level of a session drops nothing: `s_pubLevelGen` starts 0
       and no generation encodes to that. */
    {
        static unsigned s_pubLevelGen;          /* 0 = no level seen yet */
        unsigned g = tagpu_packet_pub_level_gen() + 1u;
        if (g != s_pubLevelGen) {
            if (s_pubLevelGen) { g_guiq.reseed = 1; g_guiq.why = TAGPU_GUI_WHY_LEVEL; }
            s_pubLevelGen = g;
        }
    }
    if (g_guiq.reseed || s_pubOverflow) {
        TAGPU_PUBOP* o;
        unsigned why = g_guiq.why;
        for (i = 0; i < s_nsurf; i++) s_surf[i].seeded = 0;
        memset(s_seenF, 0, sizeof s_seenF); memset(s_seenP, 0, sizeof s_seenP);
        s_seenGen++;
        /* AND THE GLYPHS. A reseed is the consumer saying it threw state away,
           and the ops in flight when it did are skipped whole — so every
           first-sight glyph record in them is lost while our `sent[]` still
           says it was published. This is the same re-arm the sprite and pixel
           tables above get, and it was missing [found by the review of the
           review's fixes]. */
        gfont_sent_clear();
        /* an overflow drops the queue's tail too: what the consumer has not
           taken is stale against the fresh seeds */
        if (s_pubOverflow) g_guiq.overflows++;
        g_guiq.resets++;
        g_guiq.reseed = 0; s_pubOverflow = 0; g_guiq.why = 0;
        if (s_log) {
            char b[200];
            _snprintf(b, sizeof b, "gui: reset #%u: %s (queued=%u arena=%u/%u surfaces=%d)", g_guiq.resets,
                      why < TAGPU_GUI_WHY_N ? WHY_NAME[why] : "?", g_guiq.qHead - g_guiq.qTail,
                      arena_used(), TAGPU_GUI_ASIZE, s_nsurf);
            glog(b);
        }
        o = pub_op(PK_RESET, 0);
        if (!o) return;
        pub_commit();
    }
    {
        const char* ta = *(const char* const*)TA_MAINPP;
        int L, T, W, H;
        tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
        if (W > 0 && H > 0) { vl = L; vt = T; vr = L + W - 1; vb = T + H - 1; }
    }
    fs = surf_by_base(flipSurf);
    if (fs && !fs->seeded && !pub_seed(fs)) return;
    for (i = 0; i < s_nops && !s_pubOverflow; i++) {
        OP* op = &s_ops[i];
        SURF* s;
        TAGPU_PUBOP* o;
        if (op->kind == OP_FLIP) {
            unsigned nextSeq = (i + 1 < s_nops) ? 0 : tagpu_terrown_fill_seq();
            int k;
            for (k = i + 1; k < s_nops; k++) if (s_ops[k].kind == OP_FLIP) { nextSeq = s_ops[k].seq; break; }
            if (k >= s_nops) nextSeq = tagpu_terrown_fill_seq();
            s = surf_by_base(op->base);
            if (s && !s->seeded && !pub_seed(s)) return;
            o = pub_op(PK_FRAME, op->base); if (!o) return; pub_commit();
            /* the frame that follows this flip began with the terrain skip's
               key fill: that is the viewport's erase, mirrored as a clear */
            if (nextSeq != op->seq && vr >= 0 && s) {
                o = pub_op(PK_CLEAR, op->base); if (!o) return;
                o->l = (short)vl; o->t = (short)vt; o->r = (short)vr; o->b = (short)vb;
                pub_commit();
            }
            continue;
        }
        if (op->dup) continue;
        s = surf_by_base(op->base);
        if (!s) continue;
        if (s_probeX >= 0 && s->base == flipSurf && op->l <= s_probeX && s_probeX <= op->r && op->t <= s_probeY && s_probeY <= op->b) {
            char b[300];
            /* THE HEADER BYTES CAME WITH THE OP. This read the engine's frame
               header here until G19f-7 -- the last dereference of engine art
               left in `publish`, on the same freed-bank window the rest of this
               function stopped taking, and behind `frame_sane`'s probe alone.
               A debug path that dereferences what the release path no longer
               does is how a fixed crash comes back. `fcomp`/`fsub`/`fsubn` are
               stamped in `gaf_box`; a non-GAF op prints 0 for them, which is
               honest, where reading a FONT object as a GAF header was not.
               [FOUND 2026-09-16 by BOTH landing reviewers, independently.] */
            _snprintf(b, sizeof b, "gui probe: %s box=(%d,%d)-(%d,%d) at (%d,%d) frame=%08X %ux%u ck=%u comp=%u sub=%u/%u src=%08X (%d,%d)",
                      OP_NAME[op->kind], op->l, op->t, op->r, op->b, op->dx, op->dy, (unsigned)(size_t)op->frame,
                      (unsigned)op->fw, (unsigned)op->fh, (unsigned)op->ck,
                      (unsigned)op->fcomp, (unsigned)op->fsub, (unsigned)op->fsubn,
                      op->src, op->sl, op->st);
            glog(b);
        }
        if (!s->seeded && !pub_seed(s)) return;
        /* a plain keyed blit of a frame the atlas can hold is a sprite; a frame
           past the decoder's edge (TAGPU_GAF_DECMAX, the shell's 640-wide title
           art) is its box's bytes like everything else */
        if (op->kind == OP_GAF && op->frame && op->fw && op->fh &&
            op->fw <= TAGPU_GAF_DECMAX && op->fh <= TAGPU_GAF_DECMAX) {
            const void* key;
            /* ---- THE ASSET THIS OP NAMES CANNOT GO AWAY, BECAUSE NOTHING
               HERE NAMES IT ANY MORE.

               `frame`/`pix` point into a per-LEVEL GAF bank, and this function
               used to take both the identity hash and the decoded plane out of
               that bank -- up to CENSUS_MS after the blit that recorded the op.
               Two engine routes free it inside that window: the level
               teardown's cascade, and `GUI_Pop 0x4A9660`, which frees a popped
               screen's art from 39 call sites with no flag and no generation.

               G19f-7 MOVED BOTH READS INTO `gaf_box`, where the engine is
               inside its own blit of the same frame and the art is alive by the
               engine's ordering rather than by ours -- see `gaf_capture`. So
               this path now dereferences NO engine asset memory: `op->fkey` is
               a number, `op->goff`/`glen` index our own scratch, and
               `op->frame`/`op->pix` survive only as the consumer's atlas key, a
               value compared against a table and never followed.

               WHAT STOOD HERE BEFORE, AND WHY IT IS GONE. Landing 5 put a
               level-generation gate here -- refuse while a teardown is in
               flight, refuse an op whose observed generation is stale -- and it
               was a correct ordering for the reads it guarded. It could not
               cover the pop: `0x460647 call 0x4a9660` runs three instructions
               after the teardown returns, so the generation has already moved
               and those ops pass the test. With the reads gone the gate had
               nothing left to protect, and it was not free: it refused 215 ops
               at a single measured level end, each falling back to its box's
               bytes and losing its sprite identity. A gate that guards nothing
               and costs something is removed, not kept as belt.

               [The crash that bought all this, MEASURED 2026-09-16: quitting a
               skirmish to the main menu at 1920x1080 took an access violation
               in `frame_key` reading the frame header's `TAGPU_GF_COMP` byte
               out of a bank the cascade had just freed (279 blocks), and the
               process then spun. Reproduced with the Vulkan lane OFF, so it is
               the GL publisher's own and nothing to do with the port. The
               landing-5 re-review then named the pop window this closes and
               said the by-design fix wanted `MEM_Free`'s block size, which the
               observer is not handed. It wanted a different hook instead.] */
            int have;
            key = (const void*)(size_t)op->fkey;
            if (!key) goto as_pixels;           /* it was not readable when drawn */
            have = seen_frame(op->frame, key, 0);
            /* A FIRST SIGHT WITH NO PLANE IN HAND PUBLISHES ITS BOX, and it is
               decided BEFORE the op is opened so a half-filled sprite can never
               be committed. Two ways to get here and they mean opposite things:
               a RESET cleared the seen table after this op decided it needed no
               plane, or the scratch was full when the blit was seen -- the
               latter being the only real failure, and it should read 0. The
               reset case is CHEAP rather than free, which is the honest word:
               it costs this op a PK_PIXELS box in the arena and one window
               without its atlas identity, in a publish that is already seeding
               every surface whole, and it self-heals on the next window.
               ["free" corrected by the landing review.] */
            if (!have && !op->glen) {
                if (op->sgen != s_seenGen) g_guiq.gafreseed++;
                else                       g_guiq.gafnoplane++;
                goto as_pixels;
            }
            o = pub_op(PK_SPRITE, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->sl = op->dx; o->st = op->dy; o->fw = op->fw; o->fh = op->fh; o->ck = op->ck;
            o->frame = op->frame; o->pix = key;
            if (!have) {
                unsigned char* dst = pub_bytes(o, op->glen);
                if (!dst) return;
                memcpy(dst, s_gafBuf + op->goff, op->glen);
                seen_frame(op->frame, key, 1);
            }
            pub_commit();
            continue;
        }
        /* G17d: a text draw whose string we captured is a STRING op — TA's own
           glyphs, stamped by the render thread from the coverage atlas, instead
           of ~968 arena bytes of a box that has already blended with whatever
           art it was drawn onto. A text op with no string (the scratch was
           full, or the font would not read) falls through to its box's bytes,
           which is exactly what it was before this gate. */
        if (op->kind == OP_TEXT && op->slen && op->fid && !s_nostring) {
            unsigned char* dst;
            const unsigned char* str = s_strBuf + op->soff;
            /* NO FONT IS READ HERE ANY MORE (G19f-8). The slot, the two header
               bytes and every unsent glyph's rows were taken in `text_capture`,
               inside the engine's own call to the glyph blitter; this branch
               copies our own bytes out of our own scratch. `op->fid` being set
               IS the statement that the capture succeeded, so the old
               `op->frame` test, the level generation and `ptr_ok` are all gone
               with the reads they were standing in for.

               ONE THING CAN STILL HAVE CHANGED: the `sent[]` table. The block
               omits the codes this font had already published, and both the
               render thread's atlas reset and a publisher reseed clear that
               table -- so a block decided against an older generation is a
               string whose missing glyphs nobody holds. Publishing the box
               instead costs this op its stamped glyphs for one window, in a
               publish that is re-seeding surfaces whole anyway, and the next
               window re-captures everything. Exactly the sprite path's
               `gafreseed`, and counted apart for the same reason. */
            /* POLLED HERE, PER OP, AND NOT ONCE AT THE TOP OF THIS FUNCTION.
               `gfont_check_gen` reads the RENDER thread's glyph generation and
               clears every `sent[]` when it has moved. The first version of this
               landing called it once on entry and the comment claimed that made
               the test below "a comparison against NOW" -- it made it a
               comparison against the top of `publish`, and the render thread can
               drop its atlas in the middle of the loop. That WIDENED a window
               the code already had: before G19f-8 the poll was inside
               `gfont_slot`, one statement before the decision it guards, which
               is what this restores.
               [FOUND 2026-09-16 by the cross-thread reviewer, with the
               interleaving spelled out.] */
            gfont_check_gen();
            if (op->fgen != s_sentGen) { g_guiq.strrearm++; goto as_pixels; }
            o = pub_op(PK_STRING, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->sl = op->dx; o->st = op->dy;
            o->frame = NULL;
            o->font_id = op->fid;
            o->font_rows = op->frows;
            o->font_yoff = op->fyoff;
            o->fg = op->fg; o->bg = op->bg; o->tr = op->tr;
            dst = pub_bytes(o, (unsigned)op->gblen + (unsigned)op->slen + 1u);
            if (!dst) return;
            memcpy(dst, s_glyBuf + op->gboff, op->gblen);
            memcpy(dst + op->gblen, str, (size_t)op->slen);
            dst[op->gblen + op->slen] = 0;
            /* AND ONLY NOW ARE THEY SENT. The arena slot is taken and the bytes
               are in it, so nothing between here and `pub_commit` can lose
               them; marking in the capture would have marked glyphs that a
               queue overflow, an untwinned surface or a dedup drop threw away.
               The count comes back from the same walk, which is what the
               consumer needs to find the string behind the records. */
            o->gcount = (unsigned short)glyph_block_mark(op->fid, s_glyBuf + op->gboff, op->gblen);
            pub_commit();
            continue;
        }
        /* A SOLID RECTANGLE IS A COLOUR AND A BOX. [The vulkan-only plan,
           landing 8a.] `DrawBar 0x4BF6F0` fills its rect with one palette index
           through `0x4CCDEA`, so publishing the box's BYTES -- which is what
           `as_pixels` below does, read out of the surface at the FLIP -- was
           both larger than the op and later than it: anything drawn over the
           box in between is what those bytes held. `op->col` was taken while the
           engine was inside the call. */
        if (op->kind == OP_BAR) {
            o = pub_op(PK_BAR, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->fg = op->col;
            pub_commit();
            continue;
        }
        /* A HOLLOW RECTANGLE IS FOUR EDGES AND A COLOUR. [The vulkan-only
           plan, landing 8b.] `DrawTranspRectangle 0x4BF8C0` is named for its
           hollow centre and NOT for translucency: its four edges go through
           the store-only Bresenham `0x4CC7AB`, which reads nothing of the
           destination and writes `stos BYTE al` from the low byte of its
           colour argument. So the op is a box, a palette index, and the fact
           that the MIDDLE IS UNTOUCHED -- which is what publishing the box's
           bytes got wrong twice over: it copied the interior the op never
           wrote, and it copied it at the flip.
           `0x4BF7B0` is NOT here: it is `OP_FOCUS` now, and it tints.
           A CLIPPED rect falls through to `as_pixels` instead -- see
           `OP::clipped` for why, which is the `&& !op->clipped` below. */
        if (op->kind == OP_RECT && !op->clipped) {
            o = pub_op(PK_RECT, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->fg = op->col;
            pub_commit();
            continue;
        }
        /* AN AXIS-ALIGNED LINE IS A SOLID FILL ONE PIXEL THICK. [The
           vulkan-only plan, landing 8c.] `before_line` has already decided
           axis-aligned from the ENDPOINTS, so the box here is the line itself
           and `PK_BAR` describes it exactly -- same packet, same twin fill,
           same `vkCmdClearAttachments`, and NO new op kind for either consumer
           to miss. That last part is deliberate: a kind missing from one
           enumeration is this file's characteristic silent bug, and an
           axis-aligned line and a bar are the same draw.
           `OP_DIAG` is NOT here and keeps `as_pixels`: its box is the square
           the line crosses, not the line. */
        if (op->kind == OP_LINE && !op->clipped) {
            o = pub_op(PK_BAR, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->fg = op->col;
            pub_commit();
            continue;
        }
    as_pixels:
        if (op->kind == OP_COPY) {
            SURF* src = surf_by_base(op->src);
            if (src && src->seeded) {
                o = pub_op(PK_COPY, s->base); if (!o) return;
                o->src = op->src; o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
                o->sl = op->sl; o->st = op->st;
                pub_commit();
                continue;
            }
        }
        /* everything else — and a copy from a source we do not twin — is its
           box's bytes as they stand now */
        o = pub_op(PK_PIXELS, s->base); if (!o) return;
        o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
        if (!pub_surface_bytes(s, op->l, op->t, op->r, op->b, o)) return;
        pub_commit();
    }
}

/* ---- the census, at the flip ------------------------------------------- */

static const char* top_screen_name(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const char* top;
    const char* ctrls;
    if (!ptr_ok(ta)) return "-";
    top = *(const char* const*)(ta + OFF_GUI_TOP);
    if (!ptr_ok(top)) return "(no gui)";
    ctrls = *(const char* const*)(top + GM_CTRLS);   /* ControlsAry[0] = the panel */
    if (!ptr_ok(ctrls)) return "(no panel)";
    return ctrls + 0x02;                            /* its name[16] is the screen's */
}

static void census_surface(SURF* s, int isGame, int subtractVp, int vl, int vt, int vr, int vb,
                           unsigned* outChanged, unsigned* outUnexpl)
{
    const unsigned char* cur = (const unsigned char*)(size_t)s->base;
    unsigned changed = 0, unexpl = 0;
    int y, x, i;
    *outChanged = *outUnexpl = 0;
    if (!ptr_ok(cur)) return;
    /* a surface the engine freed behind the observer's back (MEM_Free, not
       SurfaceFree) may be unmapped by now: two page probes, first and last
       row, before a whole-surface read. -1 = gone, the caller drops it. */
    if (IsBadReadPtr(cur, 1) || IsBadReadPtr(cur + (size_t)(s->h - 1) * s->pitch, (size_t)s->w)) { *outUnexpl = (unsigned)-1; return; }
    if (!s->copy) s->copy = (unsigned char*)malloc((size_t)s->w * s->h);
    if (!s->mask) s->mask = (unsigned char*)malloc((size_t)s->w * s->h);
    if (!s->copy || !s->mask) return;
    if (!s->copyValid) {
        for (y = 0; y < s->h; y++) memcpy(s->copy + (size_t)y * s->w, cur + (size_t)y * s->pitch, (size_t)s->w);
        memset(s->mask, 0, (size_t)s->w * s->h);
        s->copyValid = 1;
        return;
    }
    /* 1. what changed since the last flip */
    for (y = 0; y < s->h; y++) {
        const unsigned char* a = cur + (size_t)y * s->pitch;
        unsigned char* c = s->copy + (size_t)y * s->w;
        unsigned char* m = s->mask + (size_t)y * s->w;
        if (memcmp(a, c, (size_t)s->w) == 0) { memset(m, 0, (size_t)s->w); continue; }
        for (x = 0; x < s->w; x++) {
            if (a[x] != c[x]) { m[x] = 255; changed++; c[x] = a[x]; }
            else m[x] = 0;
        }
    }
    if (!changed) return;
    /* 2. subtract every op that named this surface */
    for (i = 0; i < s_nops; i++) {
        const OP* o = &s_ops[i];
        int r, b;
        if (o->base != s->base || o->kind == OP_FLIP) continue;
        /* the box was clamped to the surface's size WHEN RECORDED; the surface
           may have been re-made smaller since (before_free zeroes the ops of a
           freed base, but a same-base re-allocation of a different size goes
           through surf_get) — never index the mask past it */
        r = o->r < s->w - 1 ? o->r : s->w - 1;
        b = o->b < s->h - 1 ? o->b : s->h - 1;
        for (y = o->t; y <= b; y++) {
            unsigned char* m = s->mask + (size_t)y * s->w;
            for (x = o->l; x <= r; x++) if (m[x] == 255) m[x] = 128;
        }
    }
    /* 3. on a game frame the world viewport is ours: the terrain skip fills it
       with the KEY every frame, so a changed pixel that is now the key is that
       erase. A changed pixel that is NOT the key inside the viewport is UI the
       engine drew over the world (a dialog, chat) and stays in the census. */
    if (isGame && subtractVp) {
        for (y = vt; y <= vb && y < s->h; y++) {
            unsigned char* m = s->mask + (size_t)y * s->w;
            const unsigned char* a = cur + (size_t)y * s->pitch;
            for (x = vl; x <= vr && x < s->w; x++) if (m[x] == 255 && a[x] == (unsigned char)s_key) m[x] = 128;
        }
    }
    /* 4. what is left */
    s->bl = s->bt = 0x7FFF; s->br = s->bb = -1;
    for (y = 0; y < s->h; y++) {
        const unsigned char* m = s->mask + (size_t)y * s->w;
        for (x = 0; x < s->w; x++) if (m[x] == 255) {
            unexpl++;
            if (x < s->bl) s->bl = x;
            if (x > s->br) s->br = x;
            if (y < s->bt) s->bt = y;
            if (y > s->bb) s->bb = y;
        }
    }
    s->changed += changed; s->unexplained += unexpl; s->explained += changed - unexpl;
    if (unexpl) {
        if (!s->acc) { s->acc = (unsigned char*)malloc((size_t)s->w * s->h); if (s->acc) memset(s->acc, 0, (size_t)s->w * s->h); }
        if (s->acc) for (i = 0; i < s->w * s->h; i++) if (s->mask[i] == 255) s->acc[i] = 255;
    }
    *outChanged = changed; *outUnexpl = unexpl;
}

/* the last census's mask (0/128) with every unexplained pixel since the last
   dump at 255; dumping clears the accumulation */
static void write_pgm(SURF* s)
{
    FILE* f;
    int y, x;
    if (!s || !s->mask) return;
    f = fopen("tagpu_gui_census.pgm", "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        unsigned char row[4096];
        const unsigned char* m = s->mask + (size_t)y * s->w;
        const unsigned char* a = s->acc ? s->acc + (size_t)y * s->w : NULL;
        for (x = 0; x < s->w; x++) row[x] = (a && a[x]) ? 255 : (m[x] == 255 ? 255 : m[x]);
        fwrite(row, 1, (size_t)s->w, f);
    }
    fclose(f);
    if (s->acc) memset(s->acc, 0, (size_t)s->w * s->h);
}

/* the flipped surface (tagpu_gui_leaves.h: the graphics globals' back buffer) */
static const int* flip_source(void* entry_esp);
static unsigned s_builds, s_buildFlags;     /* GUI_StageUpdateDraw calls since the last census */

#define CENSUS_MS 5      /* the shell flips ~5000x a second (measured 2026-09-07):
                            the diff runs at most this often, ops accumulate between */
#define LOG_EVERY 50     /* `log`: one line per this many censuses, or any unexplained */

static volatile int s_inFlip = 0;      /* between the flip's entry and its return */
static void* s_retStack[32];           /* hijacked returns, LIFO (alloc, flip)    */
static int   s_retDepth = 0;

static int __cdecl before_flip(void* entry_esp)
{
    unsigned ret = ((unsigned*)entry_esp)[0];
    const int* src;
    SURF* s;
    int isGame = (ret == FLIP_RET_GAME);
    unsigned changed = 0, unexpl = 0;
    char b[320];
    static LARGE_INTEGER s_lastQpc, s_freq;
    static unsigned s_censuses = 0;
    LARGE_INTEGER now;
    int hijack = 0;
    if (!s_gameTid) s_gameTid = GetCurrentThreadId();
    else if (!on_game_thread()) return 0;
    /* FIRST, before the census or the publisher read a single base: retire every
       surface another thread's MEM_Free left for us (surf_drain_freeq) */
    surf_drain_freeq();
    s_flips++;
    if (s_flips == 1) {
        _snprintf(b, sizeof b, "gui: first flip on thread %u (init saw %u)", (unsigned)GetCurrentThreadId(), (unsigned)s_gameTid);
        glog(b);
    }
    src = flip_source(entry_esp);
    s = surf_of_ctx(src);
    /* the marker: this flip's surface and the fill sequence as of now */
    if (s && s_nops < MAX_OPS) {
        OP* o = &s_ops[s_nops++];
        memset(o, 0, sizeof *o);
        o->kind = OP_FLIP; o->base = s->base; o->seq = tagpu_terrown_fill_seq();
    }
    /* the cursor is drawn into the back buffer INSIDE the flip and its
       background restored before it returns: nothing in between is UI */
    if (s_retDepth < 32) {
        s_retStack[s_retDepth++] = (void*)(size_t)ret;
        s_inFlip = 1;
        hijack = 1;
    }
    if (!s_census && !g_gui_draw) { ops_window_reset(); return hijack; }
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&now);
    if (s_lastQpc.QuadPart && (now.QuadPart - s_lastQpc.QuadPart) * 1000 < (LONGLONG)CENSUS_MS * s_freq.QuadPart)
        return hijack;                              /* too soon: keep accumulating ops */
    s_lastQpc = now;
    s_censuses++;
    if (!s_census) { publish(s ? s->base : 0); ops_window_reset(); return hijack; }
    if (s) {
        int vl = 0, vt = 0, vr = -1, vb = -1, sub = 0;
        if (isGame) {
            const char* ta = *(const char* const*)TA_MAINPP;
            int L, T, W, H;
            tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
            if (W > 0 && H > 0) { vl = L; vt = T; vr = L + W - 1; vb = T + H - 1; sub = 1; }
        }
        s->seen++;
        census_surface(s, isGame, sub, vl, vt, vr, vb, &changed, &unexpl);
        if (unexpl == (unsigned)-1) { changed = unexpl = 0; }      /* the flip's own surface cannot be gone; ignore */
        s_changedTotal += changed; s_unexplTotal += unexpl;
        s_winChanged += changed; s_winUnexpl += unexpl; s_winCensus++;
        if (unexpl) {
            if (s->bl < s_winL) s_winL = s->bl;
            if (s->bt < s_winT) s_winT = s->bt;
            if (s->br > s_winR) s_winR = s->br;
            if (s->bb > s_winB) s_winB = s->bb;
        }
        if (s_pgm && GetFileAttributesA("tagpu_gui_census.trigger") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("tagpu_gui_census.trigger");
            write_pgm(s);
            _snprintf(b, sizeof b, "gui census: pgm written for surface %08X %dx%d (screen %s)",
                      s->base, s->w, s->h, top_screen_name());
            glog(b);
        }
    }
    /* every other surface an op named this frame: the GUI screens' own */
    {
        int i;
        for (i = 0; i < s_nsurf; i++) {
            unsigned c2, u2;
            if (s && s_surf[i].base == s->base) continue;
            census_surface(&s_surf[i], 0, 0, 0, 0, 0, 0, &c2, &u2);
            if (u2 == (unsigned)-1) {
                if (s_log) {
                    _snprintf(b, sizeof b, "gui census: surface %08X %dx%d is unmapped — freed behind the observer, dropped", s_surf[i].base, s_surf[i].w, s_surf[i].h);
                    glog(b);
                }
                surf_drop(i); i--;
                continue;
            }
            if (u2 && s_log && s_surf[i].h > 1) {
                int k, onThis = 0, shown = 0;
                for (k = 0; k < s_nops; k++) if (s_ops[k].base == s_surf[i].base) onThis++;
                _snprintf(b, sizeof b, "gui census: %s surface %08X %dx%d changed=%u unexplained=%u box=(%d,%d)-(%d,%d) ops_on_it=%d of %d",
                          top_screen_name(), s_surf[i].base, s_surf[i].w, s_surf[i].h, c2, u2,
                          s_surf[i].bl, s_surf[i].bt, s_surf[i].br, s_surf[i].bb, onThis, s_nops);
                glog(b);
                if (s_trace) {
                    /* the distinct destinations this window's ops named */
                    unsigned bases[16]; unsigned counts[16]; int nb = 0;
                    for (k = 0; k < s_nops; k++) {
                        int j;
                        for (j = 0; j < nb; j++) if (bases[j] == s_ops[k].base) { counts[j]++; break; }
                        if (j == nb && nb < 16) { bases[nb] = s_ops[k].base; counts[nb] = 1; nb++; }
                    }
                    for (k = 0; k < nb && shown < 16; k++, shown++) {
                        _snprintf(b, sizeof b, "gui trace: window ops on base %08X: %u", bases[k], counts[k]);
                        glog(b);
                    }
                }
            }
        }
    }
    if (s && s_trace && unexpl > 256) {
        /* the ops that touched the residual's box, up to 96 — the rest of the
           frame's ops are noise for placing it */
        int i, n = 0;
        for (i = 0; i < s_nops && n < 96; i++) {
            const OP* o = &s_ops[i];
            if (o->base != s->base) continue;
            if (o->r < s->bl || o->l > s->br || o->b < s->bt || o->t > s->bb) continue;
            _snprintf(b, sizeof b, "gui trace: %s %s op %s (%d,%d)-(%d,%d)", isGame ? "GAME" : "shell",
                      top_screen_name(), OP_NAME[o->kind], o->l, o->t, o->r, o->b);
            glog(b); n++;
        }
        for (i = 0; i < s_nsurf; i++) if (&s_surf[i] != s) {
            _snprintf(b, sizeof b, "gui trace: surface %08X %dx%d pitch %d", s_surf[i].base, s_surf[i].w, s_surf[i].h, s_surf[i].pitch);
            glog(b);
        }
    }
    if (s && (unexpl > 256 || s_censuses - s_lastLog >= (unsigned)(s_log ? LOG_EVERY : LOG_EVERY * 20))) {
        s_lastLog = s_censuses;
        {
            char ops[200]; int k, n = 0; unsigned nullc = 0;
            for (k = 1; k < OP_NKIND; k++) {
                nullc += s_nullCtx[k];
                if (s_kindCount[k]) n += _snprintf(ops + n, sizeof ops - (size_t)n, "%s%s %u", n ? " " : "", OP_NAME[k], s_kindCount[k]);
            }
            /* the numbers are the WINDOW's — every census since the previous line */
            _snprintf(b, sizeof b,
                "gui census: %s %s %08X %dx%d flip=%u n=%u win=%u changed=%u unexplained=%u box=(%d,%d)-(%d,%d) ops=%d[%s] "
                "builds=%u/%X nosurf=%u dropped=%u",
                isGame ? "GAME" : "shell", top_screen_name(), s->base, s->w, s->h, s_flips, s_censuses, s_winCensus,
                s_winChanged, s_winUnexpl,
                s_winUnexpl ? s_winL : 0, s_winUnexpl ? s_winT : 0, s_winUnexpl ? s_winR : 0, s_winUnexpl ? s_winB : 0,
                s_nops, ops, s_builds, s_buildFlags, nullc, s_opsDropped);
            glog(b);
            s_winChanged = s_winUnexpl = s_winCensus = 0;
            s_winL = s_winT = 0x7FFF; s_winR = s_winB = -1;
        }
    }
    publish(s ? s->base : 0);
    ops_window_reset();
    memset(s_kindCount, 0, sizeof s_kindCount);
    memset(s_nullCtx, 0, sizeof s_nullCtx);
    s_builds = 0; s_buildFlags = 0;
    return hijack;
}

/* ---- landing 9: the engine redraws, instead of us seeding its bytes -----
   A surface whose contents we did not watch arrive can only be published as
   `PK_SEED` -- its raw bytes -- because nothing here knows how they got
   there, and a reseed (a level boundary, an arena overflow) re-publishes
   every one of them. `GUI_StageUpdateDraw 0x4A81E0(gi, 0x40)` is the engine's
   own redraw of the top screen, and every draw it makes runs through the
   leaves, so the panel's chrome arrives as ops that the twin can hold in
   palette space instead.

   WHY THE FLAG IS EXACTLY 0x40 AND NOTHING ELSE [DISASSEMBLED 2026-09-18]:
   `0x4A82F0` computes `eax = flags & 1` -- the BUILD bit -- and `0x4A82F7`
   `je 0x4A90D1`, which is past BOTH allocations (`0x4A907C`, `0x4A90B5`);
   the two frees (`0x4A9537`, `0x4A9549`) are gated on the `0x2` teardown
   bit. Those four are the only allocator and free calls in the whole
   function, so a 0x40 call allocates nothing, frees nothing, and changes no
   lifetime. A `0x1` call twice without a teardown is what would leak.

   WHAT A 0x40 REDRAW ACTUALLY DOES, IN ORDER [DISASSEMBLED 2026-09-18], because
   "every pixel arrives as an op" is NOT the whole truth and the plan said it
   was: `0x4A90F4` reads `gi->TheActive_GUIMEM->[0x24]` and, when it is set,
   repaints the WHOLE panel surface from it with `0x4C6B70(panel+0xBC, that,
   0, 0)` -- a surface-to-surface copy, not a description of a draw. When it
   is NULL the fallback at `0x4A911D` is `0x4B0230(gi, 0, panel+0xC4)`, the
   picture handler, which is a bitmap too. Only THEN does the gadget loop at
   `0x4A9135` run. So the chrome arrives as ops and the WALLPAPER arrives as a
   copy, which is a win only while that copy's source is a surface we twin.

   WHERE IT IS CALLED FROM, AND WHY THAT IS AN ORDERING AND NOT A HOPE: at
   the flip's RETURN, on the game thread, with `s_inFlip` already cleared --
   the leaves drop every op while `s_inFlip` is set (the cursor is drawn
   inside the flip and nothing in there is UI), so a repaint issued anywhere
   inside the flip would draw and publish nothing. `s_repainting` makes the
   call non-reentrant by construction rather than by an argument about what
   the engine's gadget handlers do not do.

   AND THE DESTINATION IS CHECKED BEFORE THE CALL, not after: the redraw's
   first act writes `panel+0xBC`, and `0x4C6B70` with a NULL destination
   builds its own context over the PRIMARY surface (terrain-depth.md) -- a
   redraw of an unbuilt screen would paint the wallpaper straight onto the
   screen. `repaint_service` refuses unless `TheActive_GUIMEM`, its
   `ControlsAry` and `panel+0xBC` are all present.                        */
#define OFF_GUIINFO   0x519           /* GUIInfo, inline in main (gui-gadgets.md 1.2)  */
#define GI_ACTIVE     0x18            /* GUIInfo.TheActive_GUIMEM                      */
#define STAGE_VA      0x004A81E0u     /* GUI_StageUpdateDraw(gi, flags) stdcall ret 8  */
#define STAGE_REDRAW  0x40            /* the redraw bit; 0x1 builds, 0x2 tears down    */
typedef void (__stdcall *gui_stage_fn)(void* gi, int flags);

static int      s_repaint = 1;        /* off with the `norepaint` token            */
static int      s_repaintPend = 0;    /* game thread only, from here down         */
static int      s_repainting = 0;
static unsigned s_repaints = 0, s_repaintSkips = 0, s_repaintOps = 0;
static int      s_drawShadow = 0;     /* our own last-seen value of g_gui_draw    */
static unsigned s_resetShadow = 0;    /* ... and of g_guiq.resets                */

/* WHEN A REPAINT IS WORTH ISSUING. The plan named two moments -- the layer
   arming, and a level boundary -- and the second one is a SUBSET of the right
   trigger, not a trigger of its own. A surface is seeded whenever `publish`
   finds `!s->seeded`, and the only thing that clears that flag for every
   surface at once is a RESEED; `publish`'s own level check is one of the four
   things that asks for one (the others are the consumer's stall-over, a lost
   sprite and an arena overflow). So the edge to watch is `g_guiq.resets`,
   which publish bumps on this same thread -- no new cross-thread agreement,
   and it covers the level case for free.

   [MEASURED 2026-09-18, and this is why the trigger moved:] the packet's level
   generation advances in `tagpu_packet_pub_level_end`, i.e. when a level is
   TORN DOWN, so a shell-to-game transition never moves it. Shadowing it fired
   exactly once per session -- at the arm -- and never on entering a game. It
   did not need to: entering a game BUILDS the in-game screen, and a build
   draws every gadget through the leaves already.

   `g_gui_draw` is read once into a local and compared with a shadow this
   thread owns, so the render thread flipping it mid-check cannot be seen
   twice differently. */
static void repaint_arm(void)
{
    int draw = g_gui_draw;
    unsigned resets = g_guiq.resets;
    if (draw && !s_drawShadow) s_repaintPend = 1;     /* the layer just armed      */
    if (resets != s_resetShadow) s_repaintPend = 1;   /* the twins were thrown away */
    s_drawShadow = draw;
    s_resetShadow = resets;
}

static void repaint_service(void)
{
    const char* ta;
    char* gi;
    const char* gm;
    const char* ctrls;
    int n0, k;
    unsigned before[OP_NKIND];
    if (!s_repaint || !s_repaintPend || s_repainting || !g_gui_draw) return;
    /* ptr_ok here is a range test on a VALUE, as everywhere else in this file;
       what makes the call safe is the three-link check below plus the flag */
    ta = *(const char* const*)TA_MAINPP;
    if (!ptr_ok(ta)) { s_repaintSkips++; return; }
    gi = (char*)(size_t)ta + OFF_GUIINFO;
    gm = *(const char* const*)(gi + GI_ACTIVE);
    if (!ptr_ok(gm)) { s_repaintSkips++; return; }      /* 0x4A81EA's own early return */
    ctrls = *(const char* const*)(gm + GM_CTRLS);
    if (!ptr_ok(ctrls)) { s_repaintSkips++; return; }   /* ebp at 0x4A8202 */
    if (!ptr_ok(*(const void* const*)(ctrls + P_SURFACE))) { s_repaintSkips++; return; }
    s_repaintPend = 0;
    s_repainting = 1;
    n0 = s_nops;
    memcpy(before, s_kindTotal, sizeof before);
    ((gui_stage_fn)(size_t)STAGE_VA)(gi, STAGE_REDRAW);
    s_repainting = 0;
    s_repaints++;
    /* the ops the redraw itself produced, by kind: the whole point of the call,
       and the only number that says whether a screen came back as DRAWS -- the
       sprite and glyph ops the atlas and the font path can hold -- or only as
       the flat bytes a seed would have carried anyway */
    s_repaintOps = (unsigned)(s_nops - n0);
    if (s_log) {
        /* SIZED FOR THE WORST CASE AND THE ACCUMULATOR CANNOT GO NEGATIVE.
           `OP_NKIND - 1` kinds, each at most "scale " (6) plus ten digits plus a
           separator, is 255 — hence 288. And mingw's `_snprintf` returns −1 on
           truncation rather than the length it wanted, so `n += _snprintf(...)`
           would make `n` negative and `sizeof kinds - (size_t)n` wrap to a size
           that writes BEFORE the buffer. Checking the return keeps `n` a real
           offset whatever the platform does. */
        char b[448], kinds[288];
        int n = 0, w;
        for (k = 1; k < OP_NKIND; k++)
            if (s_kindTotal[k] != before[k]) {
                w = _snprintf(kinds + n, sizeof kinds - (size_t)n, "%s%s %u",
                              n ? " " : "", OP_NAME[k], s_kindTotal[k] - before[k]);
                if (w < 0) break;                 /* out of room: keep what fits */
                n += w;
            }
        kinds[n] = 0;
        if (!n) _snprintf(kinds, sizeof kinds, "none");
        _snprintf(b, sizeof b, "gui: repaint #%u -- 0x4A81E0(gi, 0x40) on the top screen: %u op(s) [%s] (skips=%u)",
                  s_repaints, s_repaintOps, kinds, s_repaintSkips);
        b[sizeof b - 1] = 0;
        glog(b);
    }
}

static void* __cdecl after_flip(unsigned int* regs)
{
    (void)regs;
    s_inFlip = 0;
    /* the repaint runs with the flip's hijacked return still on s_retStack:
       depth is unchanged across it, and a nested alloc (a 0x40 makes none)
       would push and pop above it */
    repaint_arm();
    repaint_service();
    return s_retDepth > 0 ? s_retStack[--s_retDepth] : NULL;
}

/* ---- the leaf observers -------------------------------------------------
   Each reads the engine's arguments off entry_esp: [0] return address, [1]
   first stack argument, ... Conventions and boxes per leaf are in
   gui-renderer.md's appendix and the engine map; the byte strings are the
   prologues stolen, byte-matched at install. */

#include "tagpu_gui_leaves.h"

/* ---- install ----------------------------------------------------------- */

static int read_tokens(void)
{
    char buf[256];
    if (tagpu_opt_read("tagpu_gui.on", buf, sizeof buf) < 0) return 0;
    s_census = strstr(buf, "census") != NULL;
    s_log    = strstr(buf, "log") != NULL;
    s_pgm    = strstr(buf, "pgm") != NULL;
    s_trace  = strstr(buf, "trace") != NULL;
    s_nostring = strstr(buf, "nostring") != NULL;
    s_repaint  = strstr(buf, "norepaint") == NULL;   /* opt-OUT: the redraw is the shipped path */
    { const char* k = strstr(buf, "key="); if (k) s_key = atoi(k + 4) & 255; }
    { const char* k = strstr(buf, "probe="); if (k) sscanf(k + 6, "%d,%d", &s_probeX, &s_probeY); }
    return 1;
}

void tagpu_gui_init(void)
{
    char b[200];
    int n, ok;
    if (!read_tokens()) return;
    /* all-or-nothing: every site must carry the bytes we expect */
    if (!tagpu_detour_bytes_ok(FLIP_VA, FLIP_STOLEN, sizeof FLIP_STOLEN) || !leaves_match()) {
        glog("gui: NOT armed — engine bytes differ at a watched site");
        return;
    }
    /* DllMain runs on the process's first thread, which is the thread TA's
       game loop and every flip run on; taking it here rather than at the
       first flip means the splash screen's draws (before flip 1) are recorded */
    s_gameTid = GetCurrentThreadId();
    g_guiq.ops = s_qops;
    s_arena = (unsigned char*)VirtualAlloc(NULL, TAGPU_GUI_ASIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_guiq.arena = s_arena;
    if (!s_arena) { glog("gui: NOT armed — no arena"); return; }
    ok = tagpu_detour_observe(FLIP_VA, FLIP_STOLEN, sizeof FLIP_STOLEN, before_flip, after_flip);
    n = leaves_install();
    s_installed = ok && n == LEAF_COUNT;
    _snprintf(b, sizeof b, "gui: %s flip@0x4C63A0=%d leaves=%d/%d census=%d log=%d pgm=%d key=%d (Phase E: observers on the game thread; the layer follows the trigger, tagpu_gui_surf.c)",
              s_installed ? "ARMED" : "FAILED", ok, n, LEAF_COUNT, s_census, s_log, s_pgm, s_key);
    glog(b);
}

int tagpu_gui_installed(void) { return s_installed; }

static volatile unsigned char g_wantMm;
static unsigned g_wantMmBeat;
void tagpu_gui_set_want_minimap(int on, unsigned int frame_counter)
{
    g_wantMm = (unsigned char)(on != 0);
    if (on) g_wantMmBeat = frame_counter;
}
int tagpu_gui_want_minimap(void) { return g_wantMm != 0; }
static volatile unsigned g_mmHave;
void     tagpu_gui_set_minimap_have(unsigned v) { g_mmHave = v; }
unsigned tagpu_gui_minimap_have(void) { return g_mmHave; }
/* called from the module's own flush, on the render thread */
static void want_minimap_watchdog(unsigned int frame_counter)
{
    if (g_wantMm && frame_counter - g_wantMmBeat > 90) {
        g_wantMm = 0;
        glog("gui: the sharp minimap stopped asking — the packet's surfaces go idle");
    }
}

void tagpu_gui_flush(unsigned int frame_counter)
{
    static unsigned last = 0;
    /* 352 AND NOT 200, MEASURED RATHER THAN GUESSED. The heartbeat's format is
       137 literal characters plus 15 `%u` and 2 `%d`; at ten and eleven digits
       that is 309, and this buffer held 200. It was already over before landing
       9 added three fields (263), and the line observed in a live session is
       ~180 with `bytes=` at nine digits — so the margin was one order of
       magnitude of one counter. mingw's `_snprintf` does not NUL-terminate on
       truncation, and `glog` hands the result to `fprintf("%s")`, so the
       failure would have been an out-of-bounds READ, not a tidy cut. */
    char b[352];
    want_minimap_watchdog(frame_counter);
    if (!s_installed) return;
    if (frame_counter - last >= 600) {
        last = frame_counter;
        _snprintf(b, sizeof b, "GUI flips=%u ops=%u dropped=%u changed=%u unexplained=%u surfaces=%d published=%u bytes=%u queue=%u resets=%u overflows=%u stalls=%u draw=%d flush=%u repaints=%u/%u ops=%u",
                  s_flips, s_opsTotal, s_opsDropped, s_changedTotal, s_unexplTotal, s_nsurf,
                  s_pubOps, s_pubBytes, g_guiq.qHead - g_guiq.qTail, g_guiq.resets, g_guiq.overflows, g_guiq.stalls, g_gui_draw, s_freeqFlush, s_repaints, s_repaintSkips, s_repaintOps);
        glog(b);
        {
            int k, n = 0;
            char ops[300];
            for (k = 1; k < OP_NKIND; k++)
                if (s_kindTotal[k]) n += _snprintf(ops + n, sizeof ops - (size_t)n, "%s%s %u", n ? " " : "", OP_NAME[k], s_kindTotal[k]);
            _snprintf(b, sizeof b, "GUI kinds: %s", ops);
            glog(b);
        }
    }
}

/* ---- G17e: the TNT's minimap picture, for the render thread --------------
   Returns 1 and fills the outputs when a picture has been snapshotted since
   the last map load. `gen` changes exactly once per load, so a consumer that
   caches anything derived from these bytes drops it when the generation moves.
   The bytes are stable for the life of that generation: one writer, one write,
   and it happens inside the map loader before any frame of that map presents. */
