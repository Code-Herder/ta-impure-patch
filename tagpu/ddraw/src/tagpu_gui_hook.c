/* tagpu_gui_hook.c — the observers, the census and the publisher. Contract:
   inc/tagpu_gui.h. Design: research/notes/gui-renderer.md 3.5, 3.6, 10.

   NOTHING HERE CHANGES WHAT THE ENGINE DRAWS. Every detour is an observer
   (tagpu_detour_observe): the original runs unchanged, we read its arguments
   off the stack on the way in and, where we need its result, on the way out.

   THE PUBLISHER. While the layer is on (g_gui_draw, the render thread's
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
#include "../inc/tagpu_engine.h"
#include "../inc/dd.h"
#include "../inc/tagpu.h"
#include "tagpu_trigger.h"

#define TA_MAINPP     0x00511DE8u
#define OFF_GUI_TOP   0x531           /* GUIInfo.TheActive_GUIMEM               */
#define GM_CTRLS      0x04
#define P_SURFACE     0xBC            /* panel record: OFFSCREEN* surface       */
/* the panel's own rect, four SIGNED shorts, read by 0x4AB0B0 at 0x4AB0DF..
   0x4AB0FF (x, y, w, h -- it forms right/bottom as x+w-1, y+h-1) and again at
   0x4AB11E/0x4AB122 as the destination of the blit itself [DISASSEMBLED
   2026-09-21]. The in-game side panel reads [0, 128, 128, 352]. */
#define P_RECT_X      0x13
#define P_RECT_Y      0x15
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
/* `nostring`: text stays a box of captured pixels. The A/B for the arena
   saving, and the escape if the stamp ever disagrees with the engine's blit on
   some font.
   READ AT ATTACH, LIKE EVERY OTHER TOKEN THIS FILE OWNS — `read_tokens` runs
   once, from `tagpu_gui_init`, so `census`, `log`, `pgm`, `trace` and this one
   must be armed BEFORE the launch. Only the surf module's tokens (`strict`,
   `norestore`, `sharptest`, `nocursor`, `cursorscale=`) follow the file live,
   because only the DRAW can change mid-session; the publisher's shape cannot
   without leaving the twins holding ops of the other kind. Arming it on a
   running instance silently does nothing. */
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
/* ONE SPRITE DRAWN ONTO A SNAPSHOT SURFACE (see `snap_take`), kept so the
   surface can be rebuilt after a reset: the published sprite's fields, and its
   decoded plane COPIED OUT of `s_gafBuf` -- our own bytes, so a replay reads
   no engine memory and no scratch that has since been reused. */
#define OVL_MAX 8
typedef struct OVL {
    short l, t, r, b, dx, dy;
    unsigned short fw, fh;
    unsigned char ck;
    const void* frame;                /* the atlas key, a VALUE (see OP::frame) */
    unsigned key;                     /* OP::fkey                                */
    unsigned char* plane;             /* glen bytes, ours                        */
    unsigned glen;
} OVL;
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
    int isAsset;                      /* created with a "bitmaps\\....PCX" tag AND never yet
                                         named as an op's destination: a decoded asset, not
                                         a composition. `op_add` clears it; see PK_ASSET  */
    int assetSent;                    /* the consumer ACKED this surface's current token */
    unsigned assetTok;                /* the token of the offer in flight, 0 = none    */
    unsigned assetTries;              /* offers made; bounded by TAGPU_GUI_ASSET_TRIES  */
    unsigned char* snap;              /* the LOADER's bytes, taken by `snap_take` as
                                         the first op revoked an unsent asset claim;
                                         NULL = none. With `ovl` it is what the
                                         surface holds, built only from bytes the
                                         cut allows and ops we observed.          */
    OVL* ovl;                         /* the sprites drawn onto `snap` since, in order */
    int novl;
} SURF;
#define MAX_SURF 24
static SURF s_surf[MAX_SURF];
static int  s_nsurf = 0;
/* ---- THE HUD CHROME: the one producer the engine never re-offers ---------
   The side panel, top bar and bottom bar are painted by 0x467D70, called once
   per mode switch from 0x49842A and never again. Every other op producer can be
   asked to repeat itself -- `repaint_service` asks the gadget stage directly --
   but this one cannot: 0x467D70 clears its destination (0x4C6890) and PRESENTS
   A FRAME (0x4C63A0 at 0x467E41), so re-firing it is not a repaint. Issuing its
   blits ourselves is no better: their destination is *(main+0x37E1B), the
   engine's offscreen, which is the golden source.

   So we re-derive it instead. The recipe is small because the positions are
   constants: 0x467D70 adds 0x81 to the frame's own HotX and 0x4B7F90 lands the
   frame at (x - HotX, y - HotY), so the hotspot cancels and the three pieces
   land at fixed screen coordinates. Only ScreenH is live, and it is re-read
   every time rather than remembered, so a resolution change is followed with no
   re-capture.

   FIVE ENTRIES PER TABLE, AND THAT IS THE BOUND ON `side`. The three tables sit
   0x14 bytes apart (0x1481F, 0x14833, 0x14847), so each holds five dwords. The
   engine does not bounds-test the side byte it indexes them with -- it does
   `xor eax,eax; mov al,[rec+0x95]` and indexes -- so we do, because a side read
   out of a recycled record is DATA until it has been checked against the
   table's own extent. [RE 2026-09-21 from the retail binary.] */
#define CHROME_TOP     0x1481Fu       /* main+ ..., indexed by side*4 */
#define CHROME_BOTTOM  0x14833u
#define CHROME_PANEL   0x14847u
#define CHROME_SIDES   5              /* (0x14833 - 0x1481F) / 4 */
/* main+ : the resource block's own 33-byte LAST-DRAWN MEMO. `DrawGameScreen`
   runs every frame, but the METAL/ENERGY block at 0x468E40..0x4692C0 seeds a
   stack local from this memo, overwrites the live fields, `repz cmpsb`s the 33
   bytes against it at 0x468FD9 and SKIPS THE WHOLE BLOCK when they match
   (`je 0x469610` at 0x468FDB); only on a difference does it copy the fresh
   state back (0x468FEC) and draw. So at a fresh skirmish, with metal and energy
   both pinned at the storage cap, the bars and all six numbers are drawn once
   and then never again -- and a twin reset loses them for the rest of the level.
   [DISASSEMBLED 2026-09-21.]

   THREE references. `0x468E51` seeds the local and `0x468FC6`
   compares and updates, both inside this block; `0x4679A6` (in the function at
   `0x4679A0`) zeroes four DWORDs of it, at `+0x1D`, `+0x19`, `+0x0D` and
   `+0x01`. That third one does NOT touch byte 0, so byte 0 is ours alone to
   poison. It is a display memo, not sim state. */
#define HUD_MEMO       0x37E3Fu
#define CHROME_PLAYER  0x2A43u        /* main+ : the local player index      */
#define CHROME_PLRTBL  0x1B8Au        /* main+ : records, stride 331         */
#define CHROME_STRIDE  331
#define CHROME_RECSIDE 0x95u          /* record+ : the side byte             */
#define CHROME_XOFF    0x81           /* the top and bottom bars start here  */
#define CHROME_YOFF    0x20           /* the bottom bar sits ScreenH - 0x20  */
#define GFX_SCREEN_W   0xD4u          /* 0x4B6700 is `return *(*(0x51FBD0)+0xD4)` */
#define GFX_SCREEN_H   0xD8u          /* 0x4B6710 is `return *(*(0x51FBD0)+0xD8)` */
#define CHROME_TILEMAX 16             /* 3840 / 513 is 8; the bound, not the exit */
#define GAF_FETCH_VA   0x004B7F30u    /* frame = fetch(sequence, index), stdcall ret 8 */
typedef void* (__stdcall *gaf_fetch_fn)(void* seq, int idx);

static void chrome_emit(struct SURF* fs);     /* below the leaves include */
static void copy_record(struct SURF* s, const int* dst, const int* src, int x, int y);
                                              /* the copy leaf's body, same reason */
static int      s_chromePend = 0;             /* a reset owes the chrome a re-emit */
static int      s_panelPend = 0;              /* ... and owes the panel blit too   */
static int      s_hudPend = 0;                /* ... and owes the resource block   */
static unsigned s_hudPokes = 0, s_hudRefused = 0;
static int      s_hudPoked = 0;               /* poked for THIS debt, not ever   */
static unsigned char s_hudPoison = 0;
static unsigned s_panelEmits = 0, s_panelRefused = 0;
static unsigned s_frameBase = 0;              /* the flip surface publish last saw */
static unsigned s_chromeEmits = 0, s_chromeRefused = 0;

#define TAG_OFFSCREEN 0x005091D4u     /* the "OFFSCREEN" string every 0x4C69F0 of the main
                                         offscreen pushes: 0x490AD3, 0x491250, 0x491B23,
                                         0x4980CF, 0x498402                              */

static TAGPU_PUBOP* pub_op(int kind, unsigned surf);   /* below */
static void pub_commit(void);
static void ops_forget_base(unsigned base);
extern volatile int g_gui_draw;

/* the snapshot and its sprites, gone together: `ovl` means nothing without
   the bytes it was drawn over */
static void snap_free(SURF* s)
{
    int k;
    for (k = 0; k < s->novl; k++) free(s->ovl[k].plane);
    free(s->ovl); free(s->snap);
    s->ovl = NULL; s->novl = 0; s->snap = NULL;
}

/* forget a surface: its buffers, its recorded boxes, and the twin */
static void surf_drop(int i)
{
    if (s_surf[i].seeded && g_gui_draw) {
        TAGPU_PUBOP* o = pub_op(PK_FREE, s_surf[i].base);
        if (o) pub_commit();
    }
    free(s_surf[i].copy); free(s_surf[i].mask); free(s_surf[i].acc);
    snap_free(&s_surf[i]);
    ops_forget_base(s_surf[i].base);
    s_surf[i] = s_surf[--s_nsurf];
}

/* THE MAIN OFFSCREEN IS FREED TO THE HEAP, NOT THROUGH SurfaceFree: MEM_Free
   0x4D85A0 at 0x491AB8 (leaving a game) and 0x49838C (the game's mode switch),
   and the next 0x4C69F0("OFFSCREEN", w, h) may land on the same base (a
   same-base size change, surf_get) or on another (MEASURED 2026-09-07, both).
   Left standing, the old entry in the second case is a dead surface the census
   walks at every flip — an access violation at its base once the heap has
   returned the block — and one MAX_SURF slot leaked per cycle. The engine has
   exactly one main offscreen at a time, so a new one retires every other. Keyed
   on the base, NOT a SURF* — surf_drop swap-removes (s_surf[i] =
   s_surf[--s_nsurf]), so a pointer to the kept entry moves if it was the last
   slot; the base is stable. */
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
   is NOT safe: `MEM_Free` fires once per block, so a scan that races a
   swap-remove and misses (the moved entry lands in a slot the scan has already
   passed) queues nothing, the game thread never hears about it, and the entry
   stands for ever. A filter would be load-bearing, not an optimisation.

   THE DRAIN ONLY EVER READS A QUIESCENT RING. A producer claims a slot with
   `s_freeqN`, stores into it, and only THEN bumps `s_freeqIn`; the consumer
   reads `s_freeqIn` first and `s_freeqN` second, so any push still in flight
   makes the two disagree and it does not read the window at all. That is what
   makes the slots trustworthy: every index in [done, head) was stored by its
   own claimant, so no stale value from an earlier lap can be read as a live
   one. (The zero-on-take below is an assertion — a 0 means a bug — and is
   NOT the argument.)

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
                   and the next publish would find a box past the new surface
                   and call it an overflow (MEASURED 2026-09-07, one per switch) */
                ops_forget_base(base);
                s_surf[i].w = w; s_surf[i].h = h; s_surf[i].pitch = pitch;
                s_surf[i].copyValid = 0;
                s_surf[i].seeded = 0;          /* the twin is the old size: re-make it */
                s_surf[i].assetSent = 0; s_surf[i].assetTries = 0; s_surf[i].assetTok = 0;
                free(s_surf[i].copy); free(s_surf[i].mask); free(s_surf[i].acc);
                s_surf[i].copy = s_surf[i].mask = s_surf[i].acc = NULL;
                snap_free(&s_surf[i]);
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
       `SurfaceCreateNamed 0x4C69F0` asks MEM_Alloc for w*h+0x30 bytes and
       points the object's base field at block+0x30 (0x4C6A01..0x4C6A14), so the
       block MEM_Free will be handed is `base - 0x30` — and that holds however
       we reached the surface. The CONTEXT is not usable for this: `GetContext
       0x4C5E70` rep-movs a 12-dword copy into the caller's stack frame, so most
       blits hand us a copy whose address has nothing to do with the block.
       (MEASURED 2026-09-12: keying on the context refused 1235 draws in one
       game while registering the same surfaces through `after_alloc`, where the
       context IS the object.)

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
/* `OP_FOCUS` IS SPLIT OFF `OP_RECT`, because the two leaves are not the same
   operation. `0x4BF8C0` draws four inclusive edges through the
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
/* `OP_DIAG` IS SPLIT OFF `OP_LINE`, for `OP_FOCUS`'s reason one kind along: an
   AXIS-ALIGNED line's bounding box IS the line, one pixel thick, so it is a
   solid fill and ports exactly as `OP_BAR` does; a DIAGONAL's bounding box is
   the square the line crosses, and replaying it would paint the whole square.
   Keeping them as two kinds means `GUI kinds:` counts them apart, so the ratio
   is read rather than guessed. */
enum { OP_GAF = 1, OP_GAFA, OP_GAFB, OP_GAFD, OP_SCALE, OP_TEXT, OP_LINE, OP_BAR, OP_RECT, OP_FRAME, OP_FILL, OP_COPY,
       OP_FLIP, OP_FOCUS, OP_DIAG, OP_NKIND };
static const char* const OP_NAME[OP_NKIND] = { "?", "gaf", "gafa", "gafb", "gafd", "scale", "text", "line", "bar", "rect", "frame", "fill", "copy", "flip", "focus", "diag" };
typedef struct OP {
    unsigned base; short l, t, r, b; unsigned char kind;
    /* what the publisher needs beyond the box (gui-renderer.md 3.6) */
    unsigned char ck; unsigned short fw, fh;    /* sprite: key, frame size    */
    const void* frame; const void* pix;         /* sprite: identity -- KEYS,
                                                   never dereferenced again   */
    /* THE SPRITE IS RESOLVED AT OBSERVE TIME, NOT AT PUBLISH.
       `fkey` is `frame_key`'s content hash and `goff`/`glen` the decoded plane
       in `s_gafBuf`, both taken in `gaf_box` while the engine is inside its own
       blit of that frame. See `gaf_capture`. `publish` uses these and reads no
       engine asset memory at all, so `frame`/`pix` survive only as the atlas
       key the consumer matches on -- a VALUE compared against a table. */
    unsigned fkey;                              /* 0: not resolvable, use pixels */
    unsigned goff, glen;                        /* the decoded plane, 0 = none   */
    /* the three header bytes the `gui probe:` trace prints, taken with the rest
       in `gaf_box`. They are here so that `publish` needs NO pointer into
       engine art at all -- a debug path that dereferences what the release
       path does not is exactly the sort of thing that survives until it
       crashes someone. */
    unsigned char fcomp, fsub, fsubn;
    unsigned sgen;                              /* the seen table `glen` was decided against */
    short dx, dy;                               /* sprite: unclipped top-left */
    unsigned src; short sl, st;                 /* copy: source, its top-left */
    /* WHICH PHASE OF THE ENGINE'S DRAW THIS OP WAS OBSERVED IN, and it is the
       only field here that is about PROVENANCE rather than about the draw. 1
       means the op was recorded inside one of `DrawGameScreen`'s two world
       spans (see "THE WORLD PHASE" above `op_add`) and `publish` refuses it:
       world content may not cross into the UI replay whatever leaf it came
       through. The erase's own gate is NOT here: it is `s_winGameFlip`, a
       window flag, because an op record is not guaranteed to exist (see the
       `PK_CLEAR` emit in `publish`). */
    unsigned char world;
    /* text: the string is copied into a game-thread scratch AT OBSERVE
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
       THEM -- none of them goes through `0x4CCDEA`:
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
           `stos BYTE al` -- the low byte, a palette index, which is why
           `publish` reads `col` for `OP_RECT` too.
         - `OP_FOCUS` / `0x4BF7B0` writes through `0x4BEC70` and is a TINT: its
           argument is a shade level into `globals+0xC8`, not a colour.
       [exe-reverse-engineering.md has all four, disassembled.] */
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
       fill restricted to the clip rect. */
    unsigned char clipped;
    /* AND THE FONT IS RESOLVED AT OBSERVE TIME TOO, for the reason
       the sprite's plane is: `publish` runs up to CENSUS_MS after the draw and
       the font object is engine memory whose lifetime nothing here can state.
       `fid` is the slot id the consumer caches on (0: the font would not read,
       so this op is its box's bytes), `gboff`/`gblen` the glyph records in
       `s_glyBuf`, `frows`/`fyoff` the two header bytes the consumer needs, and
       `fgen` the `sent[]` generation those records were decided against.
       `frame` keeps the font's ADDRESS as an identity -- `op_same` tells two
       fonts apart with it and the probe prints it -- and nothing dereferences
       it. See `text_capture`. */
    unsigned fid;
    unsigned gboff; unsigned short gblen;
    unsigned char frows; signed char fyoff;
    unsigned fgen;
    /* WHICH OF THE FOUR EDGES OF ITS FOCUS RECTANGLE THIS IS (0..3), and it
       exists to be part of the op's IDENTITY.
       `0x4BF7B0` draws four edges of one box, and at `t == b` the top edge
       (l,t)-(r,t) and the bottom edge (l,b)-(r,b) carry byte-identical
       boxes -- the engine draws both, and since a tint READS its destination
       (`0x4CC8DF` stores `LUT[row*256 + dst]`) two of them are `LUT[LUT[x]]`
       and not a slower route to `LUT[x]`. Without this `dedup` would collapse them.
       Only `OP_FOCUS` sets or reads it. */
    unsigned char edge;
    /* OP_SCALE: THE SOURCE WINDOW the transformed draw takes out of its frame,
       in frame texels, half-open like the destination extent. The whole-frame
       case is (0, 0, GF_W, GF_H) and every other kind leaves it zeroed.
       `swin` is the same four values packed one byte each for the consumer's
       atlas key, or 0 for "the whole frame" -- see `tagpu_gui_int.h`. */
    unsigned short su, sv, sww, swh;
    unsigned       swin;
    unsigned char dup;                          /* an identical op follows: dropped */
} OP;
/* ---- THE UI FONTS, AS IDENTITIES AND BITS -------------------------------
   No note establishes a UI font's lifetime, and a probe is not a lifetime
   argument, so the render thread never dereferences a font object: the bits
   cross instead, on FIRST SIGHT of a (font, code) pair, here, on the game
   thread, inside the flip the observer recorded the draw in.

   THE BOUND IS THE FORMAT, as for the marker font's copy: the object is the
   `.fnt` file image — `u16 height; u16 yoff; u16 offset[256]; glyphs`
   (tools/guifont.py, decoded over the twenty stock faces) — so the offset table
   has 256 entries and an entry is a file offset into the block the loader read
   the file into.

   `f[3]` IS THE FIRST CODE, not the high byte of the y-offset word: `0x4CCF77`
   loads `[esi+3]` and `0x4CCFAA` does `sub ebx,first / jb`, which is the
   blitter's only lower bound on a character. It is 0 for every stock face,
   which is what makes `f[3] != 0` a usable "this is not that format" test — but
   it is a REFUSAL of an unusual font and not a statement about the format, and
   a font with a real non-zero `first` would be refused with it. Refused means
   the op falls back to its box's captured pixels, so the cost of being wrong
   here is a slower path and never a wrong glyph.

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
/* `volatile` because `tagpu_gui_font_stats` reads them from the RENDER thread
   for the heartbeat while only the game thread writes them -- the convention
   every other producer counter follows in `g_guiq`. Aligned 32-bit stores on
   x86 cannot tear, so what this buys is that the render thread sees a figure
   rather than a cached one. */
static volatile unsigned s_gfontRecycles, s_gfontRefused, s_gfontGlyphs, s_gfontResends;
/* ...and a reader for them, because a counter the heartbeat does not print is a
   measurement nobody ever reads. */
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
   is cleared and the next string re-sends what it needs. */
/* THE GENERATION EVERY `sent[]` DECISION IS TAKEN AGAINST. `sent[]`
   says "the consumer already has this glyph", and TWO things clear it: the
   render thread throwing its glyph atlas away (below) and a publisher reseed
   skipping whole windows (`publish`). The capture runs at OBSERVE time, so
   a clear can land between the decision and the flip, and a block that carries
   no records BECAUSE they were already sent would then be a string with
   characters missing from it for the rest of the session -- the exact failure
   `gfont_check_gen` was added to prevent. Stamped into the op, compared at
   publish, and a mismatch publishes the box instead. Same shape, and the same
   reason, as the sprite path's `s_seenGen`.

   A SLOT RECYCLE NEEDS NO BUMP OF ITS OWN, though it does invalidate what is in
   flight: both tables are eight deep, so the ninth `(font, sig)` that makes US
   recycle is also the ninth ID the CONSUMER sees, and `tagpu_text.c`'s own
   `gfont_slot` answers that with `memset(s_gf)` + `memset(s_gatlas)` +
   `s_ggen++` -- every cell under every old id, gone. A recycle therefore
   RELIABLY CAUSES a consumer clear rather than leaving everything valid. What
   makes it safe anyway is the generation: that `s_ggen++` is what the poll
   above sees, one window later at worst, and it re-arms every `sent[]` through
   the same path a shelf overflow does. `gfont=` prints the recycles and the
   resends side by side, so the two moving together is a cross-check on this
   paragraph rather than a hope. */
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
   that actually went into the arena.

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
           runs inside the engine's blit rather than at the flip, and a
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
/* AND THE BATCH'S GLYPH RECORDS, for the same reason and dropped by
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
   scratch cannot take publishes its box's bytes instead -- slower, never
   wrong. */
#define GLY_SCRATCH (128u << 10)
static unsigned char s_glyBuf[GLY_SCRATCH];
static unsigned s_glyUsed;
/* `gaflost` and `gafhigh` live in g_guiq rather than here for the reason
   `gafstale` does: a counter the heartbeat does not print is a
   measurement nobody ever reads. */
static OP* s_lastOp = NULL;                     /* the op op_add just recorded */
#define MAX_OPS 65536
static OP       s_ops[MAX_OPS];
static int      s_nops = 0;
/* ops recorded against a base are dead once the object is gone or re-made: a
   new surface may be allocated over the same bytes before the next census, and
   neither the census nor the publisher may apply an old box to it.

   A COPY'S SOURCE IS A BASE TOO. An op whose destination is still alive and
   whose SOURCE was freed would keep naming the dead address -- and this
   module's own measurement is that the engine's next `0x4C69F0` lands on the
   freed block, so `publish`'s `surf_by_base(op->src)` would resolve a
   stranger. That is a wrong picture (the copy samples whatever now lives
   there) and potentially a wrong 300 KB `PK_ASSET` offer made in the dead
   surface's name. Clearing it makes the source unresolvable instead, and an
   unresolved source falls through to the box's own bytes, which is the honest
   answer. */
static void ops_forget_base(unsigned base)
{
    int k;
    for (k = 0; k < s_nops; k++) {
        if (s_ops[k].base == base) s_ops[k].base = 0;
        if (s_ops[k].src  == base) s_ops[k].src  = 0;
    }
}
static unsigned s_kindCount[OP_NKIND];
static unsigned s_kindTotal[OP_NKIND];          /* cumulative, for the heartbeat */
static unsigned s_nullCtx[OP_NKIND];        /* ops whose ctx was NULL/unknown */
/* THE AREA EACH KIND COVERS, beside the count, because the count cannot answer
   the question the rebuild asks. `publish` gives six kinds a SEMANTIC op --
   gaf -> PK_SPRITE, text -> PK_STRING, bar/line -> PK_BAR, rect -> PK_RECT,
   copy -> PK_COPY -- and sends every other kind as `PK_PIXELS`, a box of the
   engine's own composed bytes. Which kinds those are is a count question and
   is already answered; HOW MUCH SCREEN they own is not, and one `scale` op can
   be a whole map preview where forty `gaf` ops are forty 16x16 icons. Summed
   AFTER the surface clip, so it is the area actually written, and before the
   MAX_OPS drop, so a full ring does not silently shrink the figure.
   Duplicates are counted, exactly as `s_kindCount` counts them: the dedup is
   `publish`'s, and this measures what the engine DREW. */
static unsigned s_kindArea[OP_NKIND];
/* THE SEMANTIC HALF OF `OP_SCALE`, which is the one kind that is semantic for
   SOME of its ops and not others. A transformed draw crosses as a
   `PK_SPRITE` when its plane was captured -- an axis-aligned, keyable window of
   a single-plane frame -- and as nothing at all otherwise. Summing the whole
   kind into `raw` would report `raw=0.21 pct` for a shell screen whose
   residual is zero. Counted
   here, against the SAME box `s_kindArea` took, so the two are subtractable. */
static unsigned s_scaleSem;
static unsigned s_assetSends = 0;     /* PK_ASSET ops published                   */
static unsigned s_assetRevoked = 0;   /* surfaces that stopped being assets       */
static unsigned s_assetAcked = 0;     /* assets the consumer echoed back          */
static unsigned s_assetDrift = 0;     /* pixels that moved in an ACKED asset -- the
                                         residual the claim names, measured under
                                         `census.on` and expected to read 0        */
/* ---- THE FOCUS TINT'S THREE ----------------------------------------------
   `s_tints` is what crossed as `PK_TINT`; `s_focusRowBad` is a shade level
   outside the table's 32 rows, refused by `before_focus` (the engine passes it
   through unmasked and reads off the end -- see there); `s_tintNoTable` is a
   tint observed while `globals+0xC8` was not readable, which is the state in
   which `0x4BEC70` itself returns without drawing, so it is a match and not a
   loss. All three are expected to read 0 except `s_tints`. */
static unsigned s_tints = 0;
static unsigned s_focusRowBad = 0;
static unsigned s_tintNoTable = 0;

/* NEVER REISSUED, which is what makes the echo an identity. Skips 0 on wrap so
   that 0 always means "no offer in flight"; a wrap needs 2^32 offers. */
static unsigned s_assetTokNext = 1;
static unsigned asset_token(void)
{
    unsigned t = s_assetTokNext++;
    if (!s_assetTokNext) s_assetTokNext = 1;
    return t;
}

/* ---- THE WORLD PHASE: WHERE AN OP CAME FROM, NOT WHAT IT LOOKS LIKE ------

   Every op in this file is an OBSERVATION of an engine draw, and nothing about
   the leaf distinguishes a UI draw from a WORLD draw: the engine's feature pass
   `0x46A610` blits its trees through `0x4B7F90`/`0x4B8500`, the same two GAF
   blitters the side panel and the dialogs use, so without the phase the trees
   would publish as `PK_SPRITE` and replay over our own world. Stopping the
   engine drawing instead would hole the reference frame — the thing the clean
   cut exists to keep whole.

   A LIST OF GUILTY DRAWERS IS NOT A FIX, because the next one nobody has
   enumerated leaks exactly the same way. What is recorded here instead is the
   PHASE the engine was in when the op was taken: `DrawGameScreen 0x468CF0`
   paints its world in two straight-line spans, and an op recorded inside either
   of them is world content whatever leaf it came through. `publish` drops it.

   THE TWO SPANS, from an `objdump` of `0x468CF0..0x46A400` [BINARY-VERIFIED
   2026-09-22, the pristine build]. They are two and not one because the engine
   paints the METAL/ENERGY readout and the MINIMAP in between, and both of those
   are UI:

     0x468DB0  call 0x483FA0   terrain            <- span 1 opens
     0x468DBA  call 0x418310   map debug overlay
     0x468E16  call 0x4BE950   ) two lines at an EYE-RELATIVE position (the
     0x468E30  call 0x4BE950   ) `+0x80`/`+0x20` bias), gated on main+0x14280==2
     0x468E3A  call 0x4C69C0   clip reset         <- span 1 closes
     0x468E40..0x469610        the METAL/ENERGY block          UI
     0x46961F  call 0x466B00   DrawMinimap                     UI
     0x4696E7  call 0x4C13A0   the block's font/colour latch
     0x469849  call 0x471F90   particle layer 0   <- span 2 opens
     ...                       features, units, weapons, explosions, the
                               marker block, the fog overlay, the build cursor
     0x469F1E  call 0x4BF8C0   the band box, the last world-anchored draw
     0x469F36  call 0x435100   a mode query       <- span 2 closes
     0x469F40..0x46A3E0        the HUD extras tail: the F4/SPACE popup, chat,
                               the debug line, the clock, the retained GUI
                               blit, the profiler bars, the LIGHTBAR wipe   UI

   WHY THESE FOUR ADDRESSES AND NOT THE OBVIOUS ONES. `0x468DB0` as the single
   opener — "the first thing in the world draw" — does not work: the resource
   block and the minimap come AFTER the terrain blit, so one span from there to
   the tail would take the metal readout and the minimap off the screen. Both
   closers are reached on EVERY call of `DrawGameScreen`, which is what keeps
   the flag from leaking out of the function: `0x468E3A` is the join of the
   `main+0x14280 == 2` branch (`0x468DFF jne 0x468E35`), and `0x469F36` is the
   join of the whole build- cursor block (`0x469E0D je 0x469F30`, and the drawn
   path falls through `0x469F23` to the same label). Neither is inside a loop.

   OBSERVERS, NOT SUPPRESSIONS. Each site is a 5-byte `call rel32` repointed at
   a thunk that sets the flag and then calls the engine's own function with the
   engine's own arguments — `tagpu_markown.c`'s idiom, and the same reason:
   redirecting the SITE composes with whatever is detoured on the CALLEE, so
   `terrown`'s skip of `0x483FA0` still wins and `fxown`'s detour on `0x471F90`
   still runs. Nothing the engine does changes.

   IT FAILS OPEN, DELIBERATELY. A build whose bytes differ arms nothing, the
   flag stays 0, no op is ever dropped, and the leak is back. Failing the other
   way would take the HUD off the screen on an exe we do not recognise. */
static volatile unsigned char s_world = 0;   /* inside an engine world span      */
static int      s_phaseLive = 0;             /* the four redirects are installed */
static unsigned s_worldOps = 0;              /* ops stamped WORLD                */
static unsigned s_worldDropped = 0;          /* ... and refused by `publish`     */
static unsigned s_fillClipped = 0;           /* whole-surface fills cut round the
                                                viewport instead of covering it  */
static unsigned s_vpClears = 0;              /* viewport erases published        */
static unsigned char s_winGameFlip = 0;      /* this window held an in-play flip */

#define WP_TERRAIN_VA     0x00483FA0u        /* stdcall(ctx),    ret 4  */
#define WP_CLIPRESET_VA   0x004C69C0u        /* stdcall(ctx),    ret 4  */
#define WP_SFXLAYER_VA    0x00471F90u        /* stdcall(ctx, n), ret 8  */
#define WP_MODEQ_VA       0x00435100u        /* `mov eax,[ecx]; ret` — one
                                                argument, in ECX, no stack */
#define WP_SITE_HEAD_IN   0x00468DB0u
#define WP_SITE_HEAD_OUT  0x00468E3Au
#define WP_SITE_SWEEP_IN  0x00469849u
#define WP_SITE_SWEEP_OUT 0x00469F36u

/* THE FLAG IS SET BEFORE THE CALL AT EVERY SITE, so the callee is inside the
   phase the site names: terrain and the particle layer are WORLD, the clip
   reset and the mode query are UI. */
static void __stdcall wp_head_in(void* ctx)
{
    s_world = 1;
    ((void (__stdcall *)(void*))(size_t)WP_TERRAIN_VA)(ctx);
}
static void __stdcall wp_head_out(void* ctx)
{
    s_world = 0;
    ((void (__stdcall *)(void*))(size_t)WP_CLIPRESET_VA)(ctx);
}
static void __stdcall wp_sweep_in(void* ctx, int n)
{
    s_world = 1;
    ((void (__stdcall *)(void*, int))(size_t)WP_SFXLAYER_VA)(ctx, n);
}
/* `0x435100` takes its one argument in ECX and pops nothing, which is what
   `__fastcall` means to GCC on i386 for a single pointer argument. The engine
   reloads both ECX and EDX after the call (`0x469F40`/`0x469F46`) and needs
   only EAX from it, so a C thunk's ordinary clobbers are invisible here. */
static int __fastcall wp_sweep_out(void* obj)
{
    s_world = 0;
    return ((int (__fastcall *)(void*))(size_t)WP_MODEQ_VA)(obj);
}

/* an `E8 <rel32>` at `site` whose target is `expect`? The sites are in the
   exe's own `.text`, mapped by the loader before DllMain runs — DDRAW.dll is a
   static import of TotalA.exe — so there is nothing here to probe for. */
static int wp_site_is(unsigned site, unsigned expect)
{
    const unsigned char* p = (const unsigned char*)(size_t)site;
    return p[0] == 0xE8 && *(const unsigned*)(p + 1) == expect - (site + 5);
}

static int wp_redirect(unsigned site, void* target)
{
    unsigned char b[5];
    b[0] = 0xE8;
    *(unsigned*)(b + 1) = (unsigned)(size_t)target - (site + 5);
    return tagpu_detour_write(site, b, 5);
}

static int phase_install(void)
{
    int ok;
    if (GetFileAttributesA("tagpu_worldphase.off") != INVALID_FILE_ATTRIBUTES) {
        glog("gui: the world phase is OFF (tagpu_worldphase.off) — the engine's "
             "own world draws replay through the UI layer again");
        return 0;
    }
    /* all-or-nothing: every byte checked before any is written, so a patched or
       different exe arms none of the four rather than half of them */
    if (!wp_site_is(WP_SITE_HEAD_IN,   WP_TERRAIN_VA)   ||
        !wp_site_is(WP_SITE_HEAD_OUT,  WP_CLIPRESET_VA) ||
        !wp_site_is(WP_SITE_SWEEP_IN,  WP_SFXLAYER_VA)  ||
        !wp_site_is(WP_SITE_SWEEP_OUT, WP_MODEQ_VA)) {
        glog("gui: the world phase is NOT armed — engine bytes differ at one of "
             "0x468DB0/0x468E3A/0x469849/0x469F36; engine world draws will reach "
             "the UI replay");
        return 0;
    }
    /* THE WRITE ORDER IS PART OF THE FIX, and it is the opposite of the obvious
       one. The two sites that CLEAR the flag go in before the two that SET it,
       so the only state a half-written install can reach is a clear with no set:
       `s_world` never becomes 1 and the UI layer behaves exactly as it does
       unarmed. Writing the setters first fails CLOSED instead -- the first frame
       enters at 0x468DB0, sets the flag, never reaches the unwritten clear, and
       `publish` then refuses EVERY op for the rest of the session, taking the
       whole HUD with it. The byte check above is all-or-nothing; these four
       writes are not, because `tagpu_detour_write` can fail on its own. */
    ok  = wp_redirect(WP_SITE_HEAD_OUT,  (void*)wp_head_out);
    ok &= wp_redirect(WP_SITE_SWEEP_OUT, (void*)wp_sweep_out);
    ok &= wp_redirect(WP_SITE_HEAD_IN,   (void*)wp_head_in);
    ok &= wp_redirect(WP_SITE_SWEEP_IN,  (void*)wp_sweep_in);
    if (!ok) {
        /* put back whatever went in. A site is restored by pointing it at its
           ORIGINAL target: the bytes are DERIVED from the site and the target,
           never saved, so there is no copy to keep in sync and restoring a site
           that was never written is a no-op that writes the bytes already there.
           A restore that itself fails is covered by the stamp gate in `op_add`. */
        wp_redirect(WP_SITE_HEAD_IN,   (void*)(size_t)WP_TERRAIN_VA);
        wp_redirect(WP_SITE_SWEEP_IN,  (void*)(size_t)WP_SFXLAYER_VA);
        wp_redirect(WP_SITE_HEAD_OUT,  (void*)(size_t)WP_CLIPRESET_VA);
        wp_redirect(WP_SITE_SWEEP_OUT, (void*)(size_t)WP_MODEQ_VA);
        glog("gui: the world phase FAILED to install and was rolled back — the "
             "engine's own world draws replay through the UI layer again");
    }
    s_phaseLive = ok;
    return ok;
}

/* THE LOADER'S BYTES, TAKEN AS THE FIRST DRAW REVOKES AN ASSET CLAIM.

   A "bitmaps\\*.PCX" surface is an asset while nothing draws into it, and an
   asset's bytes may cross because the loader, not the 1997 rasteriser, made
   them. The post-game screen breaks that the ordinary way: `ENDMSN.GUI` loads
   `bitmaps\\outcome0.PCX` (the backdrop, the column labels, the frame) and then
   draws ONE GAF onto it -- the DEFEAT/VICTORY title, 101x29 at (272,15). That
   single op revoked the claim before the bytes had ever crossed, so the
   backdrop reached the twin as an empty seed plus the title, and after the
   reset the render-thread restart forces it came back as nothing at all: the
   engine's repaint copies this surface onto the frame every time and the copy
   had no seeded source. MEASURED 2026-09-23: 304 051 of 306 976 changed pixels
   on that surface unexplained by any op -- the PCX -- and 1 op, the title.

   THIS IS CALLED FROM `op_add`, WHICH EVERY LEAF CALLS FROM ITS `before_`
   OBSERVER -- before the engine writes. So what is copied is exactly the
   loader's bytes: not composed pixels, which is the one thing the clean cut
   forbids (the refused `wasAsset` repair above re-sent what was on the surface
   AFTER the draws). From here the surface is those bytes plus the sprites
   `publish` records in `ovl`; `pub_seed` sends both, so a reset rebuilds the
   surface exactly. Any draw that cannot be recorded as a replayable sprite
   drops the snapshot and the surface goes back to being unrecoverable, which
   is what it was before.

   THE RESIDUAL, as for any asset: an engine path that writes this surface
   without passing a leaf leaves the snapshot older than the surface, and
   nothing re-offers it. Wrong pixels, never a crash. */
static void snap_take(SURF* s)
{
    const unsigned char* cur = (const unsigned char*)(size_t)s->base;
    int y;
    if (s->snap || !ptr_ok(cur) || s->w <= 0 || s->h <= 0 || s->pitch < s->w) return;
    s->snap = (unsigned char*)malloc((size_t)s->w * s->h);
    if (!s->snap) return;
    for (y = 0; y < s->h; y++)
        memcpy(s->snap + (size_t)y * s->w, cur + (size_t)y * s->pitch, (size_t)s->w);
    s->novl = 0;
}

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
    /* THE INVARIANT BEHIND `PK_ASSET`, AND IT IS ENFORCED HERE BECAUSE THIS IS
       WHERE A DESTINATION IS NAMED AND ITS BOX IS KNOWN TO COVER A PIXEL. An
       asset surface is one the loader filled and nothing draws into; the moment
       an op covers a pixel of it that is no longer true, so it stops being one
       for the rest of its life. `assetSent` is deliberately NOT cleared: bytes
       that already crossed are not un-sent, and the ops now arriving will paint
       over them, which is exactly the right outcome.

       AFTER THE CLIP, NOT BEFORE IT. Revoking on an op that clipped to nothing
       would revoke a claim about bytes the engine never touched, and it costs
       the picture: the surface is no longer an asset, `publish` seeds only
       DESTINATIONS it has an op for, and a surface that is only ever a copy
       SOURCE therefore has no path left to any content at all -- the copy falls
       through to `PK_PIXELS`, which the drain drops, and the screen draws
       black. Past this line every remaining
       path either records the op or drops it for want of room, and both mean
       pixels are being written, so the claim is exactly as strong as it was.

       IT IS PERMANENT, AND THE RECOVERIES AROUND IT ARE PER-EPISODE, so a
       revocation that lands on a surface whose bytes have already crossed says
       nothing now and goes black at the NEXT RESEED: the reseed clears
       `seeded`, the offer branch is refused on `isAsset`, no `PK_COPY` is
       emitted, and `PK_PIXELS` is dropped. **Restoring the claim at the reseed
       (a `wasAsset` bit) is REFUSED, because it would breach the clean cut**: a
       surface something drew into holds COMPOSED pixels, and composed pixels
       are the one thing that may not cross. There is no by-design repair that
       respects the cut -- the honest outcome for a copy whose source is
       composed is that we do not carry it -- so what is added instead is that
       the case cannot happen QUIETLY. It fires at most once per surface,
       because this is the line that clears the flag. */
    if (s->isAsset) {
        if (s->assetSent || s->assetTok) {
            char rb[160];
            _snprintf(rb, sizeof rb,
                      "gui asset: REVOKED a surface whose bytes already crossed — base %08X %dx%d, "
                      "kind %s box=(%d,%d)-(%d,%d); it will go black at the next reseed",
                      s->base, s->w, s->h, OP_NAME[kind], l, t, r, b);
            rb[sizeof rb - 1] = 0;
            glog(rb);
        }
        /* ...UNLESS ITS BYTES CAN STILL CROSS AS THE LOADER LEFT THEM. See
           `snap_take`: an unsent claim is not given up, it is turned into the
           loader's bytes plus the ops drawn over them. */
        if (!s->assetSent && !s->assetTok) snap_take(s);
        s->isAsset = 0; s_assetRevoked++;
    }
    s_kindArea[kind] += (unsigned)(r - l + 1) * (unsigned)(b - t + 1);
    /* a draw we cannot record is one the snapshot cannot be rebuilt past */
    if (s_nops >= MAX_OPS) { s_opsDropped++; if (s->snap) snap_free(s); return; }
    o = &s_ops[s_nops++];
    memset(o, 0, sizeof *o);
    o->base = s->base; o->l = (short)l; o->t = (short)t; o->r = (short)r; o->b = (short)b;
    o->kind = (unsigned char)kind;
    /* THE PROVENANCE, TAKEN HERE BECAUSE THIS IS THE ONLY PLACE AN OP IS BORN.
       Stamping at the observer would mean seventeen places to keep in step, and
       `chrome_emit` — which is not inside any engine call — would need its own
       rule. It does not: it runs at the flip, past `0x469F36`, so the flag it
       reads is 0 by the same ordering that gives every other op its answer. */
    /* THE GATE THAT MAKES THE WHOLE PHASE FAIL OPEN. `s_phaseLive` is set only
       when all four redirects went in; until then -- and for ever, if the
       install failed and even its rollback failed -- every op is stamped UI and
       `publish` refuses nothing. A latched `s_world` cannot cost a pixel,
       because nothing reads it. */
    o->world = (unsigned char)(s_phaseLive ? s_world : 0);
    if (o->world) s_worldOps++;
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
   thread reads only. It counts wherever the flip OBSERVER installed, which is
   not the same thing as the layer being armed. Nothing reads it but the
   packet dump. */
unsigned tagpu_gui_flips(void) { return s_flips; }

/* ---- the publisher (game thread -> tagpu_gui_surf.c) ------------------- */

TAGPU_GUIQ g_guiq;                    /* the queue; storage below              */
static TAGPU_PUBOP    s_qops[TAGPU_GUI_QCAP];
static unsigned char* s_arena;
/* `g_gui_draw` IS THE CONSUMER SAYING IT IS THERE: 0 means nothing drains the
   queue, so `publish()` returns on its first line, `g_guiq` receives no op, the
   arena is never written, and `gaf_capture` / `text_capture` are reached only
   through the census's half of their `(s_census || g_gui_draw)` gate. The
   writer is `tagpu_gui_surf.c` (`g_gui_draw = on`).

   WHAT THE LEVER BUYS WITHOUT A CONSUMER: the 17 leaves record ops into
   `s_ops`, and the census diffs each surface against its own copy and reports
   what no op explains. That is the harness mode, which is
   why `census` is a token of its own.

   THE TRANSPORT IS ALSO THE ONLY RECORD IN THE TREE of how UI art is decoded at
   the engine's OWN blit -- `gaf_capture`'s comment argues that is the only
   moment the art is alive by the engine's ordering rather than by our hope,
   which is the thing a native UI pass has to get right first. */
volatile int g_gui_draw = 0;          /* 0 until the consumer arms: see above */
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
   scratch was full", and one counter for both would read the former as
   failures. [MEASURED 2026-09-16: 3923 of them, 15 resets, and `gaflost` 0.] */
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

/* THE SNAPSHOT AS THE SEED: the loader's bytes as a `PK_ASSET` -- the one kind
   whose payload crosses, and these bytes meet its guarantee because they were
   taken before any draw (see `snap_take`) -- then every sprite drawn onto them
   since, each carrying its own plane so the consumer's atlas does not have to
   still hold it. Token 0: nothing waits for an ack, because the ack exists to
   retire an OFFER that is re-made at every copy, and this is a seed, made
   again only when a reset clears `seeded`. */
static int pub_seed_snap(SURF* s)
{
    TAGPU_PUBOP* o = pub_op(PK_ASSET, s->base);
    unsigned char* dst;
    int k;
    if (!o) return 0;
    o->w = s->w; o->h = s->h; o->pitch = s->w;
    o->l = 0; o->t = 0; o->r = (short)(s->w - 1); o->b = (short)(s->h - 1);
    dst = pub_bytes(o, (unsigned)s->w * (unsigned)s->h);
    if (!dst) return 0;
    memcpy(dst, s->snap, (size_t)s->w * s->h);
    pub_commit();
    for (k = 0; k < s->novl; k++) {
        const OVL* v = &s->ovl[k];
        o = pub_op(PK_SPRITE, s->base); if (!o) return 0;
        o->l = v->l; o->t = v->t; o->r = v->r; o->b = v->b;
        o->sl = v->dx; o->st = v->dy; o->fw = v->fw; o->fh = v->fh; o->ck = v->ck;
        o->frame = v->frame; o->pix = (const void*)(size_t)v->key;
        dst = pub_bytes(o, v->glen);
        if (!dst) return 0;
        memcpy(dst, v->plane, v->glen);
        pub_commit();
        seen_frame(v->frame, (const void*)(size_t)v->key, 1);
    }
    s->seeded = 1;
    return 1;
}

/* a sprite `publish` just sent onto a snapshot surface, remembered with a copy
   of its plane; past OVL_MAX the surface is no longer one we can rebuild */
/* the held sprite of this frame, or NULL: the plane a redraw with no fresh
   capture can be replayed from (`gaf_capture` captures nothing for a frame the
   seen table already holds) */
static const OVL* ovl_plane_of(const SURF* s, const OP* op)
{
    int k;
    for (k = 0; k < s->novl; k++)
        if (s->ovl[k].frame == op->frame && s->ovl[k].key == op->fkey &&
            s->ovl[k].fw == op->fw && s->ovl[k].fh == op->fh) return &s->ovl[k];
    return NULL;
}

static void ovl_add(SURF* s, const OP* op)
{
    OVL* v;
    const OVL* held;
    int k;
    if (!s->snap) return;
    /* THE SAME SPRITE AT THE SAME PLACE IS THE SAME PIXELS: a redraw replaces
       nothing, so it takes no entry. Without this every repaint that redraws
       a title would spend one, and eight would drop the snapshot. */
    for (k = 0; k < s->novl; k++) {
        v = &s->ovl[k];
        if (v->frame == op->frame && v->key == op->fkey && v->fw == op->fw && v->fh == op->fh &&
            v->dx == op->dx && v->dy == op->dy && v->l == op->l && v->t == op->t &&
            v->r == op->r && v->b == op->b && v->ck == op->ck) {
            /* ...but it is the NEWEST draw now, and order is what a replay
               keeps: move it to the end so whatever it covers stays under it */
            OVL keep = *v;
            memmove(v, v + 1, (size_t)(s->novl - k - 1) * sizeof(OVL));
            s->ovl[s->novl - 1] = keep;
            return;
        }
    }
    held = ovl_plane_of(s, op);
    if (!s->ovl) s->ovl = (OVL*)calloc(OVL_MAX, sizeof(OVL));
    if (!s->ovl || s->novl >= OVL_MAX || (!op->glen && !held)) { snap_free(s); return; }
    v = &s->ovl[s->novl];
    v->glen = op->glen ? op->glen : held->glen;
    v->plane = (unsigned char*)malloc(v->glen);
    if (!v->plane) { snap_free(s); return; }
    memcpy(v->plane, op->glen ? s_gafBuf + op->goff : held->plane, v->glen);
    v->l = op->l; v->t = op->t; v->r = op->r; v->b = op->b;
    v->dx = op->dx; v->dy = op->dy; v->fw = op->fw; v->fh = op->fh; v->ck = op->ck;
    v->frame = op->frame; v->key = op->fkey;
    s->novl++;
}

static int pub_seed(SURF* s)
{
    TAGPU_PUBOP* o;
    if (s->snap) return pub_seed_snap(s);
    o = pub_op(PK_SEED, s->base);
    if (!o) return 0;
    o->w = s->w; o->h = s->h; o->pitch = s->pitch;
    o->l = 0; o->t = 0; o->r = (short)(s->w - 1); o->b = (short)(s->h - 1);
    if (!pub_surface_bytes(s, 0, 0, s->w - 1, s->h - 1, o)) return 0;
    pub_commit();
    s->seeded = 1;
    return 1;
}

/* ---- THE LIGHTEN TABLE, `globals+0xC8` ----------------------------------
   What a focus tint remaps through: 32 rows of 256 bytes, built by the engine
   at init. `tagpu_packet_pub.c` latches the same table for the world's flash
   blit (`lht_snapshot`) and the shade table beside it the same way; this is
   the GUI queue's own copy, because the op stream is a different channel from
   the frame packet and a pointer between them would be a lifetime nobody has
   established.

   LATCHED ON THE POINTER, exactly as those two are: the note records no
   rebuild, but "no note establishes it" is the font's lesson, so a different
   pointer is a different table and is re-copied. The SIZE is the format's
   bound -- `TAGPU_GUI_SHADE_BYTES` carries that argument.

   THE READABILITY TEST IS `0x4BEC70`'S OWN. It fetches `[globals+0xC8]` and
   returns 0 without drawing when it is NULL (`0x4BEC7B`), so a null table is
   the state in which the engine draws no tint either -- publishing nothing is
   a match, not a gap. `ptr_ok` on top of that is a value filter on a pointer
   we are about to read 8 KB through and is NOT the safety argument; the
   argument is that this is an init-time allocation the engine holds for the
   process and hands to its own rasteriser on the same thread we are on. */
/* THE ADDRESSES COME FROM `tagpu_engine.h` (`TA_GFX_PP`, `PROG_LHT`,
   `PROG_CAPS`), not from private copies here. This file is a `publisher` in
   `thread-split.allow`, so it may include that header, and a second spelling
   of `0x51FBD0` in a file that already reaches it twice is how two copies of
   one fact drift apart. */
static unsigned char s_lht[TAGPU_GUI_SHADE_BYTES];
static int      s_lhtOk = 0;
static int      s_lhtSent = 0;        /* crossed since the last reset          */
static unsigned s_lhtCopies = 0;

/* 1 = the consumer has the table (or will, ahead of this batch's first tint),
   0 = there is none to publish, -1 = the queue is full and `publish` must
   return. Three states rather than two because "no table" and "no room" call
   for opposite things and one of them is silent. */
static int pub_shade(void)
{
    const char* g;
    const unsigned char* t;
    TAGPU_PUBOP* o;
    unsigned char* dst;
    g = *(const char* const*)TA_GFX_PP;
    if (!ptr_ok(g)) return 0;
    /* THE ENGINE'S OWN PRECONDITION, AND IT IS A BOUND AND NOT A PROBE.
       `PROG_CAPS` bit 7 is the graphics globals'
       "the lighten table is there" flag, and it is the engine's own gate on
       this exact buffer: the in-place setter `0x4BAB30` tests `[globals+0xF0]`
       bit 7 and returns without writing when it is clear, exactly as
       `0x4BAB00` does for the darken table on bit 6 and `0x4BAAD0` for the
       alpha table on bit 5. Its siblings' shape is the proof of what the bit
       means. Without this test an allocation that exists but has not been
       filled yet -- `0x4BA660` allocates the 8192 bytes and returns 1 without
       touching the caps word, so the two are separate events -- is copied as
       though it were a table. `tagpu_packet_pub.c`'s `lht_snapshot` has the
       same test. `0x4BEC70` itself does NOT test the bit -- it null-
       checks and draws -- so the engine will happily draw through an unbuilt
       table and we deliberately will not. */
    if (!(*(const unsigned short*)(g + PROG_CAPS) & 0x80u)) return 0;
    t = *(const unsigned char* const*)(g + PROG_LHT);
    if (!ptr_ok(t)) return 0;
    /* KEYED ON THE CONTENT, NOT ON THE POINTER.
       `0x4BAB30` is reached from `0x42E2AB`, which loads PALETTE.LHT into a
       heap buffer, `rep movsd`s 0x800 dwords of it straight into
       `[globals+0xC8]` and frees the source at `0x42E2B1` -- an IN-PLACE
       rewrite that leaves the pointer exactly where it was. A latch keyed on
       the pointer cannot see it, and `lhtcopies=1` would have read as proof
       that nothing changed. Comparing the bytes we are about to rely on is
       the same 8 KB read either way and needs no argument about how many
       times that path can run. The read is ordered against the rewrite by
       being on the same thread: `0x42E2AB` and this observer are both game
       thread, so the engine cannot be mid-`rep movsd` while we are here. */
    if (!s_lhtOk || memcmp(s_lht, t, sizeof s_lht) != 0) {
        memcpy(s_lht, t, sizeof s_lht);
        s_lhtOk = 1; s_lhtCopies++;
        s_lhtSent = 0;                 /* a different table is a different fact */
    }
    if (s_lhtSent) return 1;
    o = pub_op(PK_SHADE, 0);
    if (!o) return -1;
    o->w = (int)TAGPU_GUI_SHADE_ROWS; o->h = 256;
    dst = pub_bytes(o, (unsigned)sizeof s_lht);
    if (!dst) return -1;
    memcpy(dst, s_lht, sizeof s_lht);
    pub_commit();
    /* AFTER THE COMMIT, NEVER BEFORE IT. Marking it sent on the way in would
       claim a table that a full arena threw away -- the same shape of mistake
       as acking an asset at publish rather than at hand-over. */
    s_lhtSent = 1;
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
    /* `fr` IS CHECKED, as every other caller of the GAF resolvers in this
       tree -- tagpu_fx.c, tagpu_feat.c, tagpu_render3do.c, tagpu_gui_surf.c --
       puts the header through `tagpu_gaf_frame_sane` first. It is a BOUND on a
       value and it is NOT the reason this read is safe: that is the caller's
       ordering against the level teardown (see `publish`). It is here so a
       header that is merely garbage rather than unmapped is refused rather
       than hashed. */
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

/* ---- THE SPRITE, RESOLVED WHERE IT IS PROVABLY ALIVE --------------------
   Taking the identity hash and the decoded plane out of engine memory at
   `publish`, up to CENSUS_MS after the blit that recorded the op, would read
   art the engine is free to release in between, and it does: the level
   teardown's cascade frees the per-level GAF banks, and `GUI_Pop 0x4A9660`
   frees a popped screen's from 39 call sites with no flag and no generation at
   all. A level-generation ordering cannot cover the pop, and a BOUND is not a
   safety argument.

   THE ORDERING IS ON THE ENGINE'S TIMELINE, AND IT COVERS LIFETIME ONLY. This
   runs from `gaf_box`, a `before_` detour on the blit leaf, so:

       our read  <  the engine's blit  <  the engine's free

   The free routes -- the teardown cascade and
   `GUI_Pop 0x4A9660` -- all run AFTER the blit they follow, and the caller
   holds the art alive across the call it is currently making. Our read
   precedes that call. That is the ordering, and it needs no flag, no
   generation and no `MEM_Free` block size. It covers all 39 pop sites, the
   cascade, and any free route nobody has found yet, because it does not
   enumerate them: it is earlier than all of them.

   **It is NOT the claim that "the engine would fault if this were dead".**
   The detour runs BEFORE the engine's read, so if the memory were dead WE
   would fault first; that phrasing is a counterfactual dressed as a proof.

   **AND IT DOES NOT BOUND THE EXTENT.** The engine reads the CLIPPED sub-rect;
   `tagpu_gaf_decode` reads all `w*h`, or every RLE row. So a header whose `w`/
   `h` exceed the plane the loader actually allocated is not covered by anything
   above -- only by `tagpu_gaf_frame_sane`, which is a SHAPE test (w,h <= 512),
   and the decoder's own `IsBadReadPtr`, which this file says everywhere is not
   a safety argument. Named, not fixed: bounding it needs the plane's
   allocated length, and the plane is not a block start, so `MEM_Size` cannot
   answer it either.

   `publish` dereferences no engine asset memory on this path at all -- the
   `gui probe:` trace included.
   `frame`/`pix` survive in the op as the consumer's atlas KEY, a value compared
   against a table and never followed.

   IT ALSO CLOSES A WRONG-ART CASE. The key is a hash of the plane's first
   bytes precisely because the shell hands a freed screen's addresses to the
   next screen's art. Taken at publish time, that hash would be read from
   whatever the address held THEN -- so art freed and replaced inside one
   census window would hash the NEW content under the OLD op, and the consumer
   would match a key that named pixels the op never drew. Taken here, the
   hash is of the bytes the engine is about to blit, which is the only content
   the op ever meant.

   The plane is decoded only on FIRST SIGHT -- a settled session decodes
   nothing -- and `s_gcap` keeps one window from
   decoding the same new frame once per gadget that shares it. The scratch is a
   bound: what it cannot take publishes its box's bytes instead, which is what
   an undecodable frame has always done. Slower, never wrong. */
#define GCAP_N 256
static struct { const void* fr; unsigned key, off, len; } s_gcap[GCAP_N];  /* this window's decodes */

/* THE WINDOW'S SCRATCHES, DROPPED WITH ITS OPS -- all of them, here, because
   an op's `soff`/`goff`/`gboff` are offsets into them and an op that outlived
   its bytes would publish whatever now sits at that offset. `s_gcap` MUST go
   too: its entries are offsets into `s_gafBuf`. The three call sites set
   `s_nops = 0` and call this, so the next scratch has one place to be reset. */
/* HOW FAR `publish` GOT THROUGH THIS WINDOW: every op below this index was
   handled (published, or deliberately skipped), every op from it on was not.
   0 when `publish` never ran or returned before its loop. */
static int s_pubReached = 0;
static SURF* surf_by_base(unsigned base);
static void ops_window_reset(void)
{
    /* A SNAPSHOT IS ITS SURFACE ONLY WHILE EVERY DRAW ON IT REACHED `ovl`. An
       op recorded into this window and never published -- the window thrown
       away on a stall, `publish` returning on a full queue or arena, or not
       running at all -- drew pixels the rebuild would not replay, so the
       snapshot goes with the window. By construction rather than by the
       stall being rare: `consumer_stalled` fires on exactly the way out of a
       game, which is when the post-game screen builds. */
    int k;
    for (k = s_pubReached; k < s_nops; k++) {
        SURF* sv = surf_by_base(s_ops[k].base);
        if (sv && sv->snap) snap_free(sv);
    }
    s_pubReached = 0;
    s_nops = 0;
    s_winGameFlip = 0;
    s_strUsed = 0;
    s_glyUsed = 0;
    s_gafUsed = 0;
    memset(s_gcap, 0, sizeof s_gcap);
}

/* `force`: decode even when the seen table says the consumer has the frame.
   A snapshot surface (see `snap_take`) needs every sprite drawn onto it WITH
   its plane, because `ovl` must be able to replay it after a reset has emptied
   the consumer's atlas -- and without one the precheck in `publish` drops the
   snapshot, which puts the post-game backdrop back to black whenever its title
   frame has already crossed from another surface. */
static void gaf_capture(OP* o, const unsigned char* fr, int force)
{
    const void* key = frame_key(fr, o->pix, o->fw, o->fh);
    unsigned n, i;
    unsigned char* dst;
    o->fkey = (unsigned)(size_t)key;
    o->goff = o->glen = 0;
    o->sgen = s_seenGen;
    if (!key) return;                       /* unreadable now: publish the box */
    if (!force && seen_frame(fr, key, 0)) return;   /* the consumer already has it */
    /* the same new frame twice in one window -- 39 gadgets sharing one button
       face -- reuses the first decode. Direct-mapped, and matched on ALL THREE
       of the frame pointer, the key and the length: on the key and the length
       alone, two distinct frames of equal `fw*fh` colliding in `frame_key`
       inside one window would hand the second op the first's plane, published
       under its own address key, and the consumer would atlas the wrong
       pixels. Comparing `fr` costs one
       load and makes the reuse exact, so a collision can only ever cost a
       second decode. (Matching the pointer is sound HERE and only here:
       `s_gcap` lives for one census window and is cleared with it, so an
       address cannot be recycled inside its lifetime -- which is exactly why
       the ATLAS, which lives for a session, may not match on one.)
       `frame_key` never returns 0, so 0 is a free empty marker. */
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
       always fallen through to the box's bytes. */
    if (!tagpu_gaf_decode(fr, o->fw, o->fh, dst)) { g_guiq.gafbaddec++; return; }
    o->goff = s_gafUsed; o->glen = n;
    s_gcap[i].fr = (const void*)fr; s_gcap[i].key = o->fkey; s_gcap[i].off = o->goff; s_gcap[i].len = n;
    s_gafUsed += n;
    if (s_gafUsed > g_guiq.gafhigh) g_guiq.gafhigh = s_gafUsed;
}

/* THE BLENDED BLIT'S RLE ARM, CARRIED AS THE SPRITE IT DRAWS. `0x4B8310` hands
   an RLE frame to `0x4CC3D0` with `[globals+0xC8] + row*256` (row = its fifth
   argument), and that loop writes `row_base[src]` for every texel the frame
   draws and leaves every skip alone (`0x4CC4AD`..`0x4CC4B7` and the three arms
   like it). It never reads the destination -- so what it puts on screen is a
   fixed picture, the frame remapped through one row of the lighten table, and
   an ordinary keyed sprite of THAT picture is exact. It is what the post-game
   screen's player names are: 6x8..6x10 font glyphs, row 15, measured
   2026-09-23. The RAW arm (`0x4CBF2C`, `dst = T[src*256 + dst]`) does read its
   destination and is not this; `o->fkey` stays 0 for it and the op keeps the
   box path, as do sub-frame stacks (`0x4B8500`).

   THE KEY IS CHOSEN FROM THE ROW, NOT FROM THE PLANE: a value the row cannot
   produce from ANY source byte, so no drawn texel can be mistaken for it, and
   a later sight that skips the decode (the consumer already has it) still
   knows it. The frame's own key when the row cannot produce that, else the
   lowest free value; a row that reaches all 256 values cannot be keyed and
   publishes nothing.

   THE IDENTITY folds the row's BYTES into `frame_key`'s hash, not its number:
   the engine rewrites this table in place (`0x42E2AB`, see `pub_shade`), and
   a different row content is a different picture. Read here, on the game
   thread inside the engine's own call that reads the same bytes -- the ordering
   `gaf_capture` argues, and the table is an init-time allocation besides.
   `row` is BOUNDED here and not by the engine: `0x4B84AB` shifts it unmasked,
   and the table is 32 rows (`TAGPU_GUI_SHADE_ROWS`). */
static unsigned char s_gafbCov[TAGPU_GAF_DECMAX * TAGPU_GAF_DECMAX];
static void gafb_capture(OP* o, const unsigned char* fr, unsigned row, int force)
{
    const char* g;
    const unsigned char* t;
    unsigned char img[256];
    unsigned hh = 2166136261u, n, i, k;
    unsigned char* dst;
    const void* fk;
    o->fkey = 0; o->goff = o->glen = 0; o->sgen = s_seenGen;
    if (o->fcomp == 0 || o->fsub != 0) return;
    if (!o->fw || !o->fh || o->fw > TAGPU_GAF_DECMAX || o->fh > TAGPU_GAF_DECMAX) return;
    if (row >= TAGPU_GUI_SHADE_ROWS) return;
    g = *(const char* const*)TA_GFX_PP;
    if (!ptr_ok(g)) return;
    t = *(const unsigned char* const*)(g + PROG_LHT);
    if (!ptr_ok(t)) return;
    t += row * 256u;
    memset(img, 0, sizeof img);
    for (i = 0; i < 256; i++) { img[t[i]] = 1; hh = (hh ^ t[i]) * 16777619u; }
    if (img[o->ck]) {
        for (k = 0; k < 256 && img[k]; k++) ;
        if (k == 256) return;
        o->ck = (unsigned char)k;
    }
    fk = frame_key(fr, o->pix, o->fw, o->fh);
    if (!fk) return;
    hh ^= (unsigned)(size_t)fk * 2654435761u;
    hh ^= row * 0x9E3779B9u;
    o->fkey = hh ? hh : 1u;
    if (!force && seen_frame(fr, (const void*)(size_t)o->fkey, 0)) return;   /* see `gaf_capture` */
    n = (unsigned)o->fw * (unsigned)o->fh;
    i = o->fkey & (GCAP_N - 1);
    if (s_gcap[i].fr == (const void*)fr && s_gcap[i].key == o->fkey && s_gcap[i].len == n) {
        o->goff = s_gcap[i].off; o->glen = s_gcap[i].len; return;
    }
    if (n > GAF_SCRATCH - s_gafUsed) { g_guiq.gaflost++; return; }
    dst = s_gafBuf + s_gafUsed;
    if (!tagpu_gaf_decode_cov(fr, o->fw, o->fh, dst, s_gafbCov)) { g_guiq.gafbaddec++; return; }
    for (k = 0; k < n; k++) dst[k] = s_gafbCov[k] ? t[dst[k]] : o->ck;
    o->goff = s_gafUsed; o->glen = n;
    s_gcap[i].fr = (const void*)fr; s_gcap[i].key = o->fkey; s_gcap[i].off = o->goff; s_gcap[i].len = n;
    s_gafUsed += n;
    if (s_gafUsed > g_guiq.gafhigh) g_guiq.gafhigh = s_gafUsed;
}

/* A TRANSFORMED GAF FRAME, RESAMPLED TO ITS DESTINATION HERE rather than in the
   renderer. `GAF_DrawTransformed 0x4C7580` maps a frame onto a parallelogram, and
   the one the in-game HUD uses is the player's colour badge: a 32x32 frame onto
   `(132,5)-(152,25)`, measured 2026-09-21 as a SINGLE call whose FOUR vertices
   are origin, +u, +u+v and +v -- `xy=(132,5)(152,5)(152,25)(132,25)` with
   `uv=(0,0)(32,0)(32,32)(0,32)` -- so it is an axis-aligned uniform downscale and
   nothing more. [The loop at `0x4C763D..0x4C7679` walks FOUR at stride 8, and
   the badge is the WHOLE-FRAME case of a uv quad that is in general a window.]

   WHY NOT IN THE RENDERER. The Vulkan lane draws a sprite over
   `(sl,st)-(sl+fw,st+fh)`, the FRAME's size, not the op's box, and it does that
   deliberately: a clipped sprite still needs its whole quad. Teaching it a second
   destination size would touch the render thread for one 21x21 badge. Resampling
   here gives the existing sprite path a frame that is already the right size, and
   the atlas keys on `(frame, pix, fw, fh)` plus the source WINDOW, so the scaled
   variant is a different entry from any 1:1 use of the same art.

   THE WINDOW IS PART OF THE KEY (`swin`, 0 meaning "the whole frame"): two
   windows of ONE frame resampled to one destination size are the same `(frame,
   pix, fw, fh)` with different texels -- and `atlas_find` runs before
   `atlas_put`, so without it the second would silently wear the first one's
   pixels.

   NEAREST, AND SAID PLAINLY: this reproduces the engine's affine map by sampling
   `src[(y*sh)/dh][(x*sw)/dw]`, which is the same rule its rasteriser steps but not
   provably the same rounding on every texel. The badge is measured against the
   golden source rather than assumed; whatever that number is, it is in the note.

   THE PLANE IS ALWAYS CARRIED. `gaf_capture` dedups through `seen_frame`, which is
   keyed on the frame POINTER -- so a 1:1 sight of the same art would answer "the
   consumer already has it" while the atlas held no scaled entry, and the sprite
   would be lost and ask for a reseed, every window, for ever. 441 bytes against a
   16 MB arena is the cheaper side of that trade by a wide margin. */
static void scale_capture(OP* o, const unsigned char* fr, int dw, int dh)
{
    /* the frame header's own w/h; `GF_W`/`GF_H` live in the leaves include,
       which is below this point in the translation unit */
    int sw = (int)*(const unsigned short*)(fr + 0x00);
    int sh = (int)*(const unsigned short*)(fr + 0x02);
    /* the window inside that frame; `before_scale` bounded it against the
       header above and stamped it on the op, so these are already facts */
    int su = (int)o->su, sv = (int)o->sv;
    int ww = (int)o->sww, wh = (int)o->swh;
    unsigned n = (unsigned)dw * (unsigned)dh;
    unsigned char *dst, *tmp;
    int x, y;
    o->goff = o->glen = 0;
    o->sgen = s_seenGen;
    o->fkey = (unsigned)(size_t)frame_key(fr, o->pix, sw, sh);
    if (!o->fkey) return;                        /* unreadable now: publish nothing */
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (sw > TAGPU_GAF_DECMAX || sh > TAGPU_GAF_DECMAX) return;
    /* RE-TESTED HERE, against the header THIS function read. `before_scale`
       bounded the window against a header it read at observe time; re-reading
       it means the two reads could in principle disagree, and the decode and
       the walk below both index off this one. Cheaper to re-test than to
       argue that they cannot. */
    if (su < 0 || sv < 0 || ww <= 0 || wh <= 0 || su + ww > sw || sv + wh > sh) return;
    /* the native decode goes BEYOND the output in the same scratch and is
       transient -- `s_gafUsed` only ever advances over the resampled plane */
    if (n + (unsigned)sw * (unsigned)sh > GAF_SCRATCH - s_gafUsed) { g_guiq.gaflost++; return; }
    dst = s_gafBuf + s_gafUsed;
    tmp = dst + n;
    if (!tagpu_gaf_decode(fr, sw, sh, tmp)) { g_guiq.gafbaddec++; return; }
    /* A 16.16 ACCUMULATOR, NOT `(x * sw) / dw`, and the difference is measurable.
       The two agree almost everywhere and disagree exactly where the truncated
       step lands a hair under an integer: with sw=32, dw=20 the step is
       `32<<16 / 20` = 104857, so x=5 accumulates 524285 and shifts to 7, where
       the division gives 524288/65536 = 8. Sixteen of the badge's 400 pixels
       took the brighter neighbour in the engine's frame and the darker one in
       ours, and those sixteen are that off-by-one: every unambiguous column the
       two images could be matched on (0, 9, 13, 14, 16, 17, 18) agrees with BOTH
       rules, so the accumulator is what distinguishes them. This is what a 1997
       affine rasteriser steps, and reproducing the step is what makes the badge
       exact rather than nearly right. [MEASURED 2026-09-21.]

       THE SHIFT IS BOUNDED ANYWAY. A truncated step cannot walk off the frame,
       but `sw`/`sh` come from a frame header, which is engine DATA: the clamp is
       what makes that a fact rather than an argument about the arithmetic. */
    {
        /* THE WINDOW IS THE SOURCE, AND IT IS WHAT THE STEP DIVIDES. `sw`/`sh`
           above are the FRAME's dimensions, which is what the decode fills and
           what a row is indexed by; the resample walks only the rectangle the
           uv corners named, starting at its origin. For the whole-frame case --
           the in-game badge -- (su, sv) is (0, 0) and (sww, swh) is (sw, sh),
           so the window changes nothing and the badge's measured texels hold.
           */
        unsigned stepx = ((unsigned)ww << 16) / (unsigned)dw;
        unsigned stepy = ((unsigned)wh << 16) / (unsigned)dh;
        unsigned accy = (unsigned)sv << 16;
        unsigned xlim = (unsigned)(su + ww) - 1u;
        unsigned ylim = (unsigned)(sv + wh) - 1u;
        for (y = 0; y < dh; y++, accy += stepy) {
            unsigned sy = accy >> 16, accx = (unsigned)su << 16;
            const unsigned char* srow;
            unsigned char* drow = dst + (size_t)y * (unsigned)dw;
            if (sy > ylim) sy = ylim;
            srow = tmp + (size_t)sy * (unsigned)sw;
            for (x = 0; x < dw; x++, accx += stepx) {
                unsigned sx = accx >> 16;
                if (sx > xlim) sx = xlim;
                drow[x] = srow[sx];
            }
        }
    }
    o->goff = s_gafUsed; o->glen = n;
    s_gafUsed += n;
}


/* ---- THE FONT THIS OP NAMES CANNOT GO AWAY, BECAUSE NOTHING NAMES IT LATER
   (the same move `gaf_capture` makes for the sprite).

   Taking the font slot, the offset table and every unsent glyph's packed rows
   out of the object at the flip -- up to CENSUS_MS after the draw that
   recorded the op -- would need a lifetime, and neither a level generation nor
   `ptr_ok` is one: the generation says the level has not ENDED, which is not
   the same as the font still being mapped, and a range test on a value is a
   filter and never an ordering.

   THE ORDERING, ON THE ENGINE'S OWN TIMELINE. This runs from the detour at the
   head of `0x4CCF60`, the glyph blitter, with the engine's own `font` and `str`
   arguments in hand. The engine is committed to reading `font[0]`, `font[2]`,
   `font[3]`, the table entry for every code of that string and each glyph's
   bits before it returns, so:

       our read  <  the engine's read  <  any free of the font

   holds by the engine's sequencing rather than by our hope. It is NOT "the
   engine would fault if this were dead" -- the detour runs FIRST, so we would
   fault first; that phrasing is a counterfactual dressed as a proof. What
   makes the read safe is that the engine has already decided to make it.

   AND THE EXTENT HALF IS CLOSED TOO, which it is not on the sprite path: we
   read a SUBSET of the bytes the blitter reads, code for code (see
   `glyph_block_capture`). The GAF path's residual -- the engine blits a clipped
   sub-rect while we decode all `w*h` -- has no counterpart here, because
   `0x4CCF60` has no clip at all.

   `publish` DEREFERENCES NO PER-LEVEL ENGINE ASSET ON ANY PATH --
   no GAF bank and no font object, the two whose lifetimes nothing here can
   state. It is NOT "no engine memory at all", which would be an overclaim: it
   still reads the graphics globals through `TA_MAINPP` for the true viewport
   rect, and `pub_surface_bytes` still reads the surface's own pixels. Both have
   stated lifetimes -- the globals are process-lifetime and the surfaces are the
   FORK's, freed in their own Release (see `surf_of_ctx`) -- which is exactly
   what a GAF bank and a font do not have. `frame` survives in a text op as the
   font's address, an identity `op_same` compares and the probe prints, never
   followed.

   NOT COVERED. A font whose header lies -- a table entry pointing outside the
   loaded file image, a width byte that runs the bits past its end -- is refused
   only by `f[3] != 0`, `f[0] == 0` and `gfont_glyph`'s zero tests; the engine
   would read the same wrong bytes one instruction later. `ptr_ok(font)` in
   `before_text` is a BOUND on the value and is not the safety argument; the
   ordering is. */
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
   (A per-surface epoch bumped by every copy defeats the whole dedup in the
   shell, whose panel is copied to the frame on every one of its ~12 000 flips
   a second: MEASURED as a reseed storm, 2 749 resets in one walk.) */
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
    h ^= (unsigned)o->world * 0x9E3779B9u;   /* `op_same` compares it, so the hash
                                                must separate it too: otherwise a WORLD
                                                op and a geometrically identical UI one
                                                collide, lengthen the probe chain, and a
                                                chain that reaches DUP_PROBE_MAX leaves
                                                both out of the table */
    h ^= h >> 16;
    return h & (DUP_TAB - 1);
}
static int op_same(const OP* a, const OP* b)
{
    /* A WORLD OP AND A UI OP ARE NEVER THE SAME OP, whatever their boxes say.
       `dedup` keeps the LATER of two duplicates and marks the earlier, so
       without this a UI draw that happened to match a later world draw — same
       kind, same surface, same box, same frame — would be collapsed into an op
       `publish` then refuses, and the UI draw would simply vanish. The odds are
       small and the failure is silent -- `OP::edge` exists for the same
       reason: an identity that leaves out what actually distinguishes two
       draws. */
    if (a->world != b->world) return 0;
    if (!(a->kind == b->kind && a->base == b->base && a->l == b->l && a->t == b->t && a->r == b->r && a->b == b->b &&
          a->frame == b->frame && a->pix == b->pix && a->src == b->src && a->sl == b->sl && a->st == b->st))
        return 0;
    /* TWO STRINGS IN ONE BOX ARE NOT THE SAME OP. A string op carries the
       string, not the box's bytes read at publish, so dropping the earlier one
       would drop whatever ink of it the later one does not cover. */
    /* A TINT COLLAPSES ACROSS FLIPS AND NEVER WITHIN ONE CALL. Across flips the
       collapse is right for the same reason it is right for every other kind:
       the engine REPAINTS the gadget before it tints it again, so the last
       flip's paint-then-tint is the whole of what the batch leaves on screen.
       What must not collapse is two edges of the SAME call, which have no
       repaint between them -- so the edge ordinal is part of the identity.
       `col` is here too: it is the shade ROW, and two rings at different levels
       over one box are two different pictures. (Measured both ways: exempting
       `OP_FOCUS` from `dedup` outright put SINGLE.GUI at 99.75 %, over-tinting
       a gadget whose repaint collapsed while its tints did not.) */
    if (a->kind == OP_FOCUS)
        return a->edge == b->edge && a->col == b->col;
    /* TWO WINDOWS OF ONE FRAME ARE NOT ONE OP. The
       prefix above compares the frame and the plane, which are equal for every
       window of a frame, so without this a windowed stamp and a whole-frame one
       over the same box collapse. Collapsing is right for an op that OVERWRITES
       its box and these mostly do -- but a colour-keyed sprite leaves its
       transparent texels alone, so the survivor does not write what both would
       have written. Same shape as the `edge` field above: make the identity
       finer rather than exempt the kind (the tint above measures why). */
    if (a->kind == OP_SCALE)
        return a->swin == b->swin;
    if (a->kind == OP_TEXT)
        return a->slen == b->slen && a->dx == b->dx && a->dy == b->dy &&
               a->fg == b->fg && a->bg == b->bg && a->tr == b->tr &&
               /* And the SLOT, not just the address the prefix above
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
    { "?", "arm", "queue-full", "arena-full", "box-outside-surface", "lost-sprite", "atlas-full", "untwinned-copy", "stall-over", "string-empty", "level-changed" };

/* THE CONSUMER CAN DIE, OR CRAWL. cnc-ddraw stops its render thread inside
   every SetDisplayMode and starts a new one (dd.c);
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
    /* the panel re-emit runs at the flip's RETURN, where the flip surface is no
       longer an argument to anything. Kept as the BASE, a value, and resolved
       through `surf_by_base` at the point of use, so a surface freed in between
       is a failed lookup rather than a dangling SURF*. */
    if (flipSurf) s_frameBase = flipSurf;
    int i;
    SURF* fs;
    int vl = 0, vt = 0, vr = -1, vb = -1;
    s_pubReached = 0;
    if (!g_gui_draw) return;
    if (consumer_stalled()) return;
    dedup();
    /* ---- THE LEVEL BOUNDARY, FOR THE CONSUMER'S ATLAS.

       `tagpu_gui_surf.c`'s UI atlas matches entries on `(o->frame, o->pix,
       fw, fh)` -- the frame's ADDRESS and its content hash -- and its only
       resets are `twins_reset` and the atlas filling. NEITHER of those is a
       level boundary. The engine frees a level's GAF banks
       and the next level's loader may hand a new frame an old one's address;
       `frame_key` hashes only the plane's first 64 bytes plus the hotspot, so
       UI art whose first RLE row is one transparent run can collide by
       CONSTRUCTION rather than by 2^-32 luck, and then `atlas_find` hits the
       old entry and the twin draws the previous level's texels.

       A REFUSAL OF OPS AT THE BOUNDARY WOULD NOT COVER THIS: entries already
       sitting in the consumer's atlas from the previous level survive any
       refusal. The fix is a drop.

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
        /* `assetSent` DIES WITH THE TWIN IT DESCRIBES. It is not a fact about
           this side -- it records that the CONSUMER holds the asset's bytes --
           so a reseed, which is the consumer saying it threw its twins away,
           un-sends it exactly as it un-seeds everything else. Missing this, the
           backdrop comes back black after the first reset and stays that way,
           because the surface re-seeds EMPTY (a `PK_SEED`'s payload is dropped
           by design) while `assetSent` still claims the bytes landed. The
           first-sight tables two lines below are re-armed for the same
           reason. */
        for (i = 0; i < s_nsurf; i++) {
            s_surf[i].seeded = 0;
            s_surf[i].assetSent = 0; s_surf[i].assetTries = 0; s_surf[i].assetTok = 0;
        }
        /* `assetTok` GOES WITH THEM, and clearing it is what retires every echo
           still in flight: the next offer issues a NEW token, and a token that
           was never issued for it cannot satisfy it. Clearing `g_guiq.assetAck`
           here instead would make the producer a second writer of the
           consumer's word AND still lose the race -- an in-flight `PK_ASSET`
           from the previous episode could be drained after the clear and before
           the read, re-marking the debt paid with nothing across. Retiring the
           token is the same statement with no window in it. */
        memset(s_seenF, 0, sizeof s_seenF); memset(s_seenP, 0, sizeof s_seenP);
        s_seenGen++;
        /* AND THE GLYPHS. A reseed is the consumer saying it threw state away,
           and the ops in flight when it did are skipped whole — so every
           first-sight glyph record in them is lost while our `sent[]` still
           says it was published. This is the same re-arm the sprite and pixel
           tables above get. */
        gfont_sent_clear();
        /* AND THE LIGHTEN TABLE. Same rule as the glyphs and the asset: a
           reseed is the consumer saying it threw state away, and whatever was
           in flight when it did was skipped whole -- so a `PK_SHADE` that has
           not been drained yet is lost while `s_lhtSent` still claims it
           landed, and every tint afterwards would index a table the consumer
           does not have. Cheap to redo: 8 KB once per reset. */
        s_lhtSent = 0;
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
        /* THE CHROME IS OWED, NOT EMITTED HERE. A reset means the consumer threw
           every twin away, and the chrome is the one thing the engine will not
           redraw for us. It is emitted at the TOP of the next window rather than
           into this one, because that is what puts it UNDER the gadgets: the
           window is empty at that point, so the three ops take slots 0..2, and
           the repaint runs later still (after_flip) and lands behind
           them. Ordering by position in the op array, not by a rule. */
        s_chromePend = 1;
        s_panelPend = 1;
        s_hudPend   = 1;
        s_hudPoked  = 0;   /* a NEW debt: the last episode's poke says nothing */
    }
    /* THE VIEWPORT RECT, AND ONLY WHILE A LEVEL IS OPEN. `main+0x37E27` keeps
       the last level's rect after the shell comes back, so an ungated read
       hands a perfectly plausible rectangle to two consumers that would then
       act on it off a level: the frame's erase below, and the chrome fill's cut
       in the `OP_FILL` branch — which would punch a hole in the MAIN MENU's
       backdrop fill. `vr < 0` is the one answer that means "there is no
       viewport", and both test it. */
    if (tagpu_packet_pub_level_open()) {
        const char* ta = *(const char* const*)TA_MAINPP;
        int L, T, W, H;
        tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
        if (W > 0 && H > 0) { vl = L; vt = T; vr = L + W - 1; vb = T + H - 1; }
    }
    fs = surf_by_base(flipSurf);
    if (fs && !fs->seeded && !pub_seed(fs)) return;
    /* ---- THE VIEWPORT'S ERASE -------------------------------------------
       What the viewport region holds BEFORE this window's draws, which is why
       it is emitted HERE and not at the flip marker: the marker is pushed at
       the TOP of `before_flip`, i.e. AFTER the frame's ops, so an erase emitted
       there lands on top of every engine draw the frame put inside the viewport
       and wipes it. `dedup` makes that total rather than intermittent — it
       keeps the LAST of two identical ops, so the one surviving copy of a
       per-frame draw sits immediately before the marker. MEASURED 2026-09-22:
       with the erase at the marker, the `PAUSED` banner is in the golden source
       and absent from the screen (one grab, against a baseline DLL built from
       `main` as the control -- not a per-frame census).

       At the ~60 flips a second an in-play frame runs at, a 5 ms window holds
       exactly one flip; the shell, at its far higher flip rate, can hold
       several, and the flag below is raised by any of them.

       THE GATE IS ARM STATE, NOT A COUNTER, and it is a WINDOW FLAG rather
       than a field of the flip op. `s_winGameFlip` is `ret == FLIP_RET_GAME`
       AND `tagpu_packet_pub_level_open()`, both taken on the game thread at the
       flip and both static across the frame; it is raised by any flip in the
       window and cleared by `ops_window_reset`, so a window is a game window
       when any flip in it said so. It is deliberately NOT carried on the
       `OP_FLIP` record: that record is only pushed `if (s && s_nops < MAX_OPS)`,
       so on a saturated window -- exactly the window whose frame drew the most
       -- the erase would be the thing that went missing, and last frame's
       viewport would survive under this frame's UI. The flag costs one byte and
       has no such hole. It is deliberately NOT a value
       the world pass publishes — that is a render-thread counter read here a
       frame late, and on every 0->1 acquisition edge (level entry, recovery
       from a `terr_bail`) the gate would still be down and the frame wrong.
       `tagpu_owndraw.c` ~:210-225 is the precedent for that exact hazard. */
    if (vr >= 0 && fs && s_winGameFlip) {
        TAGPU_PUBOP* c = pub_op(PK_CLEAR, fs->base);
        if (!c) return;
        c->l = (short)vl; c->t = (short)vt; c->r = (short)vr; c->b = (short)vb;
        pub_commit();
        s_vpClears++;
    }
    for (i = 0; i < s_nops && !s_pubOverflow; i++) {
        s_pubReached = i;
        OP* op = &s_ops[i];
        SURF* s;
        TAGPU_PUBOP* o;
        if (op->kind == OP_FLIP) {
            s = surf_by_base(op->base);
            if (s && !s->seeded && !pub_seed(s)) return;
            o = pub_op(PK_FRAME, op->base); if (!o) return; pub_commit();
            continue;
        }
        if (op->dup) continue;
        s = surf_by_base(op->base);
        if (!s) continue;
        if (s_probeX >= 0 && s->base == flipSurf && op->l <= s_probeX && s_probeX <= op->r && op->t <= s_probeY && s_probeY <= op->b) {
            char b[300];
            /* THE HEADER BYTES CAME WITH THE OP: reading the engine's frame
               header here would dereference engine art on the freed-bank window,
               and a debug path that dereferences what the release path does not
               is how a fixed crash comes back. `fcomp`/`fsub`/`fsubn` are
               stamped in `gaf_box`; a non-GAF op prints 0 for them, which is
               honest, where reading a FONT object as a GAF header would not
               be. */
            _snprintf(b, sizeof b, "gui probe: %s box=(%d,%d)-(%d,%d) at (%d,%d) frame=%08X %ux%u ck=%u comp=%u sub=%u/%u src=%08X (%d,%d)",
                      OP_NAME[op->kind], op->l, op->t, op->r, op->b, op->dx, op->dy, (unsigned)(size_t)op->frame,
                      (unsigned)op->fw, (unsigned)op->fh, (unsigned)op->ck,
                      (unsigned)op->fcomp, (unsigned)op->fsub, (unsigned)op->fsubn,
                      op->src, op->sl, op->st);
            glog(b);
        }
        /* WORLD CONTENT DOES NOT CROSS. The op was recorded inside one of
           `DrawGameScreen`'s world spans (see "THE WORLD PHASE" above
           `op_add`), so whatever leaf carried it, it is the engine painting the
           world and this channel replays the UI.

           DROPPED HERE, and the position is chosen twice over. Not in `op_add`:
           the census explains changed pixels from the op array, and an op that
           is never recorded reads there as an UNEXPLAINED change — an
           instrument lying about a draw we chose not to carry. And not before
           the probe above: `gui probe:` answers "what did the engine draw at
           this pixel", which world draws are part of. It IS before `pub_seed`,
           so a surface no UI op ever names is never seeded for one. */
        /* A SNAPSHOT SURFACE TAKES ONLY WHAT IT CAN REPLAY: a 1:1 sprite whose
           plane is in hand, which is exactly the set the sprite branch below
           publishes as `PK_SPRITE` and `ovl_add` records. Anything else --
           and a world op, which is dropped rather than published -- is a draw
           the rebuild would not contain, so the snapshot goes first, before
           `pub_seed` could send it. */
        if (s->snap && !((op->kind == OP_GAF || op->kind == OP_GAFB) && !op->world && op->frame && op->fw && op->fh &&
                         op->fw <= TAGPU_GAF_DECMAX && op->fh <= TAGPU_GAF_DECMAX &&
                         op->fkey && (op->glen || ovl_plane_of(s, op))))
            snap_free(s);
        if (op->world) { s_worldDropped++; continue; }
        if (!s->seeded && !pub_seed(s)) return;
        /* a plain keyed blit of a frame the atlas can hold is a sprite; a frame
           past the decoder's edge (TAGPU_GAF_DECMAX, the shell's 640-wide title
           art) is its box's bytes like everything else */
        /* `OP_GAFB` rides here once `gafb_capture` has keyed it: its plane is
           the remapped picture and `fkey`/`ck` are that picture's, so from this
           point on it IS a keyed sprite. Uncaptured (`fkey` 0) it takes the
           `as_pixels` exit below, as it always did. */
        if ((op->kind == OP_GAF || op->kind == OP_GAFB) && op->frame && op->fw && op->fh &&
            op->fw <= TAGPU_GAF_DECMAX && op->fh <= TAGPU_GAF_DECMAX) {
            const void* key;
            /* ---- THE ASSET THIS OP NAMES CANNOT GO AWAY, BECAUSE NOTHING
               HERE NAMES IT.

               `frame`/`pix` point into a per-LEVEL GAF bank, and two engine
               routes free it within CENSUS_MS of the blit that recorded the op:
               the level teardown's cascade, and `GUI_Pop 0x4A9660`, which frees
               a popped screen's art from 39 call sites with no flag and no
               generation.

               BOTH READS ARE IN `gaf_box`, where the engine is inside its own
               blit of the same frame and the art is alive by the engine's
               ordering rather than by ours -- see `gaf_capture`. So this path
               dereferences NO engine asset memory: `op->fkey` is a number,
               `op->goff`/`glen` index our own scratch, and `op->frame`/`op->pix`
               survive only as the consumer's atlas key, a value compared
               against a table and never followed.

               NO LEVEL-GENERATION GATE, deliberately. It could not cover the
               pop: `0x460647 call 0x4a9660` runs three instructions after the
               teardown returns, so the generation has already moved and those
               ops pass the test. With no read left to guard it would guard
               nothing and cost something: MEASURED at one level end, such a
               gate refused 215 ops, each falling back to its box's bytes and
               losing its sprite identity.

               [The crash this prevents, MEASURED 2026-09-16: quitting a
               skirmish to the main menu at 1920x1080 took an access violation
               in `frame_key` reading the frame header's `TAGPU_GF_COMP` byte
               out of a bank the cascade had just freed (279 blocks), and the
               process then spun.] */
            int have;
            const OVL* held = NULL;
            key = (const void*)(size_t)op->fkey;
            if (!key) goto as_pixels;           /* it was not readable when drawn */
            have = seen_frame(op->frame, key, 0);
            /* a snapshot surface's redraw with no fresh plane sends the one it
               holds -- the precheck above admitted it on exactly that */
            if (!have && !op->glen && s->snap) held = ovl_plane_of(s, op);
            /* A FIRST SIGHT WITH NO PLANE IN HAND PUBLISHES ITS BOX, and it is
               decided BEFORE the op is opened so a half-filled sprite can never
               be committed. Two ways to get here and they mean opposite things:
               a RESET cleared the seen table after this op decided it needed no
               plane, or the scratch was full when the blit was seen -- the
               latter being the only real failure, and it should read 0. The
               reset case is CHEAP rather than free: it costs this op a
               PK_PIXELS box in the arena and one window without its atlas
               identity, in a publish that is already seeding every surface
               whole, and it self-heals on the next window. */
            if (!have && !op->glen && !held) {
                if (op->sgen != s_seenGen) g_guiq.gafreseed++;
                else                       g_guiq.gafnoplane++;
                goto as_pixels;
            }
            o = pub_op(PK_SPRITE, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->sl = op->dx; o->st = op->dy; o->fw = op->fw; o->fh = op->fh; o->ck = op->ck;
            o->frame = op->frame; o->pix = key;
            if (!have) {
                unsigned n = op->glen ? op->glen : held->glen;
                unsigned char* dst = pub_bytes(o, n);
                if (!dst) return;
                memcpy(dst, op->glen ? s_gafBuf + op->goff : held->plane, n);
                seen_frame(op->frame, key, 1);
            }
            pub_commit();
            if (s->snap) ovl_add(s, op);
            continue;
        }
        /* A text draw whose string we captured is a STRING op — TA's own
           glyphs, stamped by the render thread from the coverage atlas, instead
           of ~968 arena bytes of a box that has already blended with whatever
           art it was drawn onto. A text op with no string (the scratch was
           full, or the font would not read) falls through to its box's
           bytes. */
        if (op->kind == OP_TEXT && op->slen && op->fid && !s_nostring) {
            unsigned char* dst;
            const unsigned char* str = s_strBuf + op->soff;
            /* NO FONT IS READ HERE. The slot, the two header
               bytes and every unsent glyph's rows were taken in `text_capture`,
               inside the engine's own call to the glyph blitter; this branch
               copies our own bytes out of our own scratch. `op->fid` being set
               IS the statement that the capture succeeded.

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
               clears every `sent[]` when it has moved. Called once on entry, the
               test below would be a comparison against the top of `publish`, and
               the render thread can drop its atlas in the middle of the loop;
               polled here it is one statement before the decision it guards. */
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
        /* A SOLID RECTANGLE IS A COLOUR AND A BOX. `DrawBar 0x4BF6F0` fills
           its rect with one palette index through `0x4CCDEA`, so publishing the
           box's BYTES -- which is what `as_pixels` below does, read out of the
           surface at the FLIP -- would be both larger than the op and later
           than it: anything drawn over the box in between is what those bytes
           hold. `op->col` was taken while the
           engine was inside the call. */
        if (op->kind == OP_BAR) {
            o = pub_op(PK_BAR, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->fg = op->col;
            pub_commit();
            continue;
        }
        /* A WHOLE-SURFACE FILL IS A BAR THE SIZE OF THE SURFACE. `0x4C6890`
           writes one palette index over every pixel, which is `PK_BAR`'s exact
           shape -- same packet, same `twin_fill`, coverage 1. Sent as
           `as_pixels` it would be dropped by the drain: the engine's clear of
           the offscreen would never reach the twin, and every pixel the engine
           left at index 0 would present as the lane's magenta clear. The
           viewport's own erase still uncovers the world after this, because the
           FLIP op that emits it is recorded after these and publishes later in
           the same window. */
        if (op->kind == OP_FILL) {
            /* AND IT IS CUT ROUND THE VIEWPORT RATHER THAN UNDONE AFTERWARDS.
               The fill that matters here is the HUD chrome's: `0x467D70` calls
               `0x4C6890(offscreen, 0)` at its head before blitting its three
               pieces, so the black it lays down spans the whole surface — the
               viewport included — although every pixel of it that MEANS
               anything is outside. The frame's erase already reopens the
               viewport, but it is emitted at the FLIP op, so a chrome re-emit
               landing after it in the same window (`chrome_emit` runs at the
               flip's return, and pays a debt a reset left) blackens the world
               until the next frame's erase. Subtracting the rect is the answer
               that has no ordering in it at all.

               UP TO FOUR BANDS, and a fill wholly inside the viewport emits
               none — which is right: it would be erased whole. The engine's own
               clip is not involved; `op_add` has already clamped the box to the
               surface. `s->base == flipSurf` keeps this off the GUI screens'
               own surfaces, whose fills have nothing to do with the viewport. */
            int bl = op->l, bt = op->t, br = op->r, bb = op->b;
            if (vr >= 0 && s->base == flipSurf &&
                bl <= vr && br >= vl && bt <= vb && bb >= vt) {
                int band[4][4], nb = 0, k;
                int it = bt > vt ? bt : vt, ib = bb < vb ? bb : vb;
                if (bt < vt) { band[nb][0] = bl;     band[nb][1] = bt;     band[nb][2] = br; band[nb][3] = vt - 1; nb++; }
                if (bb > vb) { band[nb][0] = bl;     band[nb][1] = vb + 1; band[nb][2] = br; band[nb][3] = bb;     nb++; }
                if (bl < vl) { band[nb][0] = bl;     band[nb][1] = it;     band[nb][2] = vl - 1; band[nb][3] = ib; nb++; }
                if (br > vr) { band[nb][0] = vr + 1; band[nb][1] = it;     band[nb][2] = br; band[nb][3] = ib;     nb++; }
                for (k = 0; k < nb; k++) {
                    o = pub_op(PK_BAR, s->base); if (!o) return;
                    o->l = (short)band[k][0]; o->t = (short)band[k][1];
                    o->r = (short)band[k][2]; o->b = (short)band[k][3];
                    o->fg = op->col;
                    pub_commit();
                }
                s_fillClipped++;
                continue;
            }
            o = pub_op(PK_BAR, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->fg = op->col;
            pub_commit();
            continue;
        }
        /* A HOLLOW RECTANGLE IS FOUR EDGES AND A COLOUR.
           `DrawTranspRectangle 0x4BF8C0` is named for its
           hollow centre and NOT for translucency: its four edges go through
           the store-only Bresenham `0x4CC7AB`, which reads nothing of the
           destination and writes `stos BYTE al` from the low byte of its
           colour argument. So the op is a box, a palette index, and the fact
           that the MIDDLE IS UNTOUCHED -- which publishing the box's bytes
           would get wrong twice over: it copies the interior the op never
           wrote, and it copies it at the flip.
           `0x4BF7B0` is NOT here: it is `OP_FOCUS`, and it tints.
           A CLIPPED rect falls through to `as_pixels` instead -- see
           `OP::clipped` for why, which is the `&& !op->clipped` below. */
        if (op->kind == OP_RECT && !op->clipped) {
            o = pub_op(PK_RECT, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->fg = op->col;
            pub_commit();
            continue;
        }
        /* AN AXIS-ALIGNED LINE IS A SOLID FILL ONE PIXEL THICK.
           `before_line` has already decided
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
        /* A TINT IS A BOX, A ROW, AND -- ONCE -- THE TABLE. `0x4BF7B0`'s edges
           go through `0x4BEC70` to `0x4CC8DF`, which READS the destination byte
           and writes back `LUT[row*256 + byte]`: there is no colour in the op
           at all, which is why it cannot be a `PK_BAR` or a `PK_RECT`. What
           crosses is the OPERATION; the consumer applies it to its own twin and
           no engine pixel is involved.

           `before_focus` has already split the rectangle into its four edges
           and clipped each as the engine does, so this is one edge and the box
           is one pixel thick. The ORDER of the four matters at the corners --
           see `PK_TINT` in tagpu_gui_int.h -- and it is the array's order,
           which is the engine's.

           THE TABLE GOES FIRST AND IN THE SAME BATCH, which is the whole
           delivery argument: the queue is FIFO, so a tint the consumer takes
           has the table it indexes, and a batch that runs out of room loses
           both together. */
        if (op->kind == OP_FOCUS) {
            int st = pub_shade();
            if (st < 0) return;
            if (st == 0) { s_tintNoTable++; continue; }
            o = pub_op(PK_TINT, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->fg = op->col;                      /* the ROW, not an index */
            pub_commit();
            s_tints++;
            continue;
        }
        /* THE TRANSFORMED FRAME, already resampled to its box by `scale_capture`.
           It carries its plane every time on purpose (see there), so there is no
           `seen_frame` arm here and no first-sight case to get wrong. Without
           this branch `OP_SCALE` falls to `as_pixels` -> `PK_PIXELS`, which the
           drain drops: the player's colour badge, and every other transformed
           GAF draw, would render NOTHING at all. */
        if (op->kind == OP_SCALE && op->glen && op->fw && op->fh) {
            unsigned char* dst;
            /* THE CONTENT HASH, NOT THE PLANE POINTER -- the same key the 1:1
               branch above publishes. `pix` is in the atlas key precisely
               because "freed sequences get their address reused" (tagpu_gaf.h),
               and the raw address would take that defence out of force for
               every scaled sprite: the shell frees a popped screen's art, the
               allocator hands a DIFFERENT frame the same header and plane
               addresses, the same window at the same destination size is drawn,
               `atlas_find` hits and `atlas_put` never runs -- the old texels
               for the rest of the session. */
            const void* skey = (const void*)(size_t)op->fkey;
            if (!skey) goto as_pixels;       /* unreadable when drawn: box it */
            o = pub_op(PK_SPRITE, s->base); if (!o) return;
            o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
            o->sl = op->dx; o->st = op->dy; o->fw = op->fw; o->fh = op->fh; o->ck = op->ck;
            o->frame = op->frame; o->pix = skey;
            /* the atlas's extra key: 0 for a whole-frame stamp, the packed
               window for a sub-rectangle. Without it two windows of one frame
               at one destination size are the same entry. */
            o->swin = op->swin;
            dst = pub_bytes(o, op->glen);
            if (!dst) return;
            memcpy(dst, s_gafBuf + op->goff, op->glen);
            pub_commit();
            continue;
        }
    as_pixels:
        if (op->kind == OP_COPY) {
            SURF* src;
            /* AN IDENTITY SELF-COPY IS NOT AN OP. The engine blits a surface
               onto itself at the same origin -- the post-game DEFEAT/VICTORY
               screen (ENDMSN.GUI) does it over the whole 640x480 frame on every
               present -- and that writes each pixel with its own value, index
               and colour alike. The consumer refuses ANY self-copy, because one
               with a different origin reads and writes one image in one draw;
               sent this one, it would fall behind on every frame of that screen
               and leave the window black until the next screen. Not publishing it
               is exact: no twin byte differs from what the copy would leave. */
            if (op->src == s->base && op->sl == op->l && op->st == op->t) continue;
            src = surf_by_base(op->src);
            /* THE ASSET'S BYTES, ONCE, AND AHEAD OF THE COPY THAT NEEDS THEM.
               Not at `after_alloc`, where the surface is still blank and the
               loader has not run; not as a `PK_SEED`, whose payload the drain
               drops by design. Here the source has been filled and is about to
               be read by a copy whose twin is otherwise empty -- which is
               exactly the black backdrop this closes.

               THE LIFETIME ARGUMENT IS `before_memfree`'s, and it is cited here
               rather than left implied: `0x4D85A0` is the sole caller of the
               allocator's free, the observer at its entry is this module's
               destructor for the object, a game-thread free drops the entry
               inline, and an off-thread free goes through the ring
               `surf_drain_freeq()` empties at the top of every `before_flip` --
               before the census or the publisher reads a single base. That
               covers an asset SOURCE exactly as it covers a seed's destination:
               same object class, same `owner = base - 0x30`. The `ptr_ok` in
               `pub_surface_bytes` is a value filter and is NOT this argument.

               "IMMUTABLE" IS TOO STRONG, so it is not claimed. What `op_add`
               enforces is that no op recorded THROUGH THE 17 LEAVES, with their
               gates open, has named this surface as a destination -- the loader
               that fills it is itself an unhooked write path, which is the whole
               premise. An engine path that re-filled a claimed surface without
               passing a leaf would leave the twin holding the older bytes, and
               nothing would re-offer. Wrong picture, never a crash; stated as
               the residual rather than papered over. Ordering is the queue's
               own: this commits before the `PK_COPY` below, so the consumer has
               the source before it is asked to sample it. */
            /* ACKED, NOT MERELY SENT -- and acked by the TOKEN THIS SURFACE'S
               OFFER CARRIED, not by its address. `assetSent` is set by the
               consumer's echo and never by the act of publishing (the lesson the
               panel debt taught), and the token is what makes the echo mean this
               offer rather than some earlier surface that happened to occupy the
               same block. `assetTok == 0` is "no offer in flight" and must never
               match the initial `assetAck` of 0. */
            if (src && src->isAsset && !src->assetSent && src->assetTok &&
                g_guiq.assetAck == src->assetTok) { src->assetSent = 1; s_assetAcked++; }
            /* AND ONLY WHILE SOMETHING IS LISTENING. `mirArmed` is the
               consumer's own arm state, not a timer: an unarmed lane drops the
               payload in `mir_bytes` without a word, so composing the offer at
               all is pure game-thread memcpy. Reading it stale is harmless in
               both directions -- a missed present re-offers on the next one, a
               spurious one goes unacked -- because nothing but the echo retires
               an offer. (A try count would be a ~1.2 s timeout standing in for a
               state, and 240 x 300 KB is ~72 MB.) */
            if (src && src->isAsset && !src->assetSent && src->w > 0 && src->h > 0 &&
                g_guiq.mirArmed &&
                src->assetTries < TAGPU_GUI_ASSET_TRIES) {
                /* THE TOKEN IS STAMPED ON THE OP AND ADOPTED BY THE SURFACE
                   ONLY ONCE THE OFFER IS COMMITTED. Writing `src->assetTok`
                   first would retire the previous token on the arena-full path
                   -- `pub_surface_bytes` returns without committing -- so an
                   echo already on its way for the offer that DID go out could
                   no longer match it, costing a re-offer for no reason. A local
                   until `pub_commit`, and `assetTok` then describes only offers
                   that actually left. */
                unsigned tok = asset_token();
                TAGPU_PUBOP* a = pub_op(PK_ASSET, src->base);
                if (!a) return;
                a->assetTok = tok;
                a->w = src->w; a->h = src->h; a->pitch = src->pitch;
                a->l = 0; a->t = 0;
                a->r = (short)(src->w - 1); a->b = (short)(src->h - 1);
                if (!pub_surface_bytes(src, 0, 0, src->w - 1, src->h - 1, a)) return;
                pub_commit();
                src->assetTok = tok;
                src->assetTries++;
                src->seeded = 1;          /* the copy below may now name it */
                s_assetSends++;
            }
            /* A SNAPSHOT SOURCE IS SEEDED ON DEMAND, which is the whole point of
               it: after a reset nothing draws into the post-game backdrop again,
               but the engine's repaint copies it onto the frame every time, and
               this is the one place that learns it is needed. */
            if (src && !src->seeded && src->snap && !pub_seed(src)) return;
            if (src && src->seeded) {
                o = pub_op(PK_COPY, s->base); if (!o) return;
                o->src = op->src; o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
                o->sl = op->sl; o->st = op->st;
                pub_commit();
                continue;
            }
            /* A THROTTLED ASSET PUBLISHES NOTHING, NOT A PACKET THE DRAIN WILL
               DROP -- without this the `mirArmed` throttle saves no bytes at
               all. Skipping the offer leaves `seeded` at 0, so this copy would
               fall through to the `PK_PIXELS` below, which copies THE
               DESTINATION'S WHOLE BOX -- for the shell backdrop the same 307 200
               bytes, into the same arena, for a drain that drops every
               `PK_PIXELS`. Same cost, different packet kind.

               EMITTING NOTHING IS INDISTINGUISHABLE IN THE PICTURE: a dropped
               packet leaves the destination twin holding what it already had,
               and so does no packet. Only the memcpy differs. The surface is
               re-offered as soon as `mirArmed` reads 1, because nothing here
               retires an offer. */
            if (src && src->isAsset && !src->seeded) continue;
        }
        /* everything else — and a copy from a source we do not twin — is its
           box's bytes as they stand now */
        o = pub_op(PK_PIXELS, s->base); if (!o) return;
        o->l = op->l; o->t = op->t; o->r = op->r; o->b = op->b;
        if (!pub_surface_bytes(s, op->l, op->t, op->r, op->b, o)) return;
        pub_commit();
    }
    /* the loop also stops on an overflow an op before `i` raised */
    s_pubReached = i;
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

/* 1 once leaves_install() has been reached, i.e. the op machinery and the
   surface table have their detours. NOT s_installed, which a partial install
   leaves 0 while detours are live. */
static int s_opsLive = 0;
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
    char b[512];        /* holds the `gui area:` line: 288 of `ar` plus ~150 */
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

    /* THE ON-DEMAND TRIGGERS, ON THE GAME THREAD AND ON EVERY LANE.
       `renderer=gdi` makes no `tagpu_` call at all, so hung off a renderer's
       present not one of them would run there and no `tacli` verb could see
       the game. Here they run wherever the engine flips, which is every lane.

       IT SITS ABOVE THIS FUNCTION'S THREE EARLY RETURNS on purpose: those are
       about the GUI census having nothing to do, which says nothing about
       whether someone dropped a `.trigger` file.

       AND IT IS GATED ON A CLOCK, NOT ON THE FLIP. `s_flips` is not a frame
       counter -- CENSUS_MS above says the shell flips ~5000 times
       a second, and the op census measured ~12 000/s on MAINMENU. The family
       was written against the PRESENT (60/s): its `% 5` throttles and
       `SCN_ARM_FRAMES 600` are all in that unit. Handing it `s_flips` would
       raise its file-stat rate about a hundredfold, on the game thread, inside
       the engine's flip, under Wine -- and turn the scenario applier's
       ten-second arm watchdog into about a tenth of a second.

       TRIG_MS is the bound, and it is a clock rather than a rate assumption
       about a renderer: the family sees ~60 frames a second on EVERY lane, so
       `% 5` is ~83 ms and the arm watchdog is ~9.6 s. One
       QueryPerformanceCounter per flip is orders of magnitude cheaper than
       1.27 GetFileAttributes calls per flip.

       IT FAILS CLOSED, DELIBERATELY. If QueryPerformanceFrequency ever refuses,
       `s_trigFreq` stays 0 and the block never runs -- every trigger verb goes
       quiet on every lane. The CENSUS_MS gate below fails OPEN in the same
       case. Closed is the right way round here: open would be the ~12 000 file
       stats a second this gate exists to remove, on the game thread, inside the
       engine's flip. QPF cannot fail on Win2000+ or Wine, so neither branch is
       reachable; the asymmetry is written down because it is deliberate. */
#define TRIG_MS 16
    {
        static LARGE_INTEGER s_trigQpc, s_trigFreq;
        static unsigned      s_trigFrames = 0;
        LARGE_INTEGER tnow;   /* not `now`: the census below has its own */
        if (!s_trigFreq.QuadPart) QueryPerformanceFrequency(&s_trigFreq);
        QueryPerformanceCounter(&tnow);
        if (s_trigFreq.QuadPart &&
            (!s_trigQpc.QuadPart ||
             (tnow.QuadPart - s_trigQpc.QuadPart) * 1000 >=
                 (LONGLONG)TRIG_MS * s_trigFreq.QuadPart)) {
            TAGPU_FRAME gf;
            s_trigQpc = tnow;
            memset(&gf, 0, sizeof gf);
            gf.struct_size   = sizeof gf;
            gf.abi           = TAGPU_ABI;
            gf.game_width    = g_ddraw.width;
            gf.game_height   = g_ddraw.height;
            gf.vp_x          = g_ddraw.render.viewport.x;
            gf.vp_y          = g_ddraw.render.viewport.y;
            gf.vp_w          = g_ddraw.render.viewport.width;
            gf.vp_h          = g_ddraw.render.viewport.height;
            gf.win_width     = g_ddraw.render.width;
            gf.win_height    = g_ddraw.render.height;
            gf.hwnd          = g_ddraw.hwnd;
            /* copied for completeness; none of the five reads it, and a DC is
               not meaningful off the thread that made it */
            gf.hdc           = g_ddraw.render.hdc;
            gf.frame_counter = s_trigFrames++;
            gf.bpp           = g_ddraw.bpp;
            tagpu_triggers_frame(&gf);
        }
    }
#undef TRIG_MS

    /* AND NOTHING BELOW THIS LINE RUNS WITHOUT THE LEAVES. Everything from here
       on is the UI layer's op machinery, and all of it is paired with a leaf
       detour:

         - `surf_of_ctx` REGISTERS a surface in s_surf, and the only thing that
           RETIRES one is `before_memfree`, leaf #15. Installing the registrar
           without its destructor is an invariant the leaves' own header states
           ("rides the same table so it takes the same all-or-nothing byte
           match") and splitting it silently is how a table grows stale.
         - `ops_window_reset()` on the early return below memsets 4 KB on EVERY
           flip, and the shell flips thousands of times a second. A bare
           instance would pay that for nothing at all, since with no leaves
           there is never an op to reset.

           [The ARMED configuration pays it too, in two supported
           configurations, because the reset sits on the `!s_census &&
           !g_gui_draw` exit and `g_gui_draw` is 0 whenever the layer is armed
           but not DRAWING: `gui.on=off`, which is the documented live A/B, and
           any armed instance on `renderer=gdi`, where `tagpu_gui_present` --
           the only writer of that word -- is never called because render_gdi.c
           makes no `tagpu_` call. Armed+gdi matters because gdi is meant to be
           the stock lane, and it is open.]
         - the return hijack exists for `s_inFlip`, which only the leaf sites
           read, and for `before_alloc_push`, which is a leaf.
         - `surf_of_ctx` dereferences four dwords behind `ptr_ok` ALONE, and
           CLAUDE.md is explicit that ptr_ok is a value filter and never a
           safety argument. What underwrote that read was the 17-site byte
           match; the flip's own prologue is six generic bytes (`sub esp,0xF4`)
           and cannot carry it.
         - and with `gui.on=census` on a build whose leaves do not match,
           s_census is 1 while no leaf records an op, so every changed pixel
           counts as unexplained and the census writes tagpu.log at up to
           200 Hz for the life of the process.

       `s_opsLive` is set where the leaves are actually installed, not from
       `s_installed`: a PARTIAL leaf install leaves s_installed 0 with detours
       live, and those detours push ops that would then never be reset. */
    if (!s_opsLive) return 0;

    src = flip_source(entry_esp);
    s = surf_of_ctx(src);
    /* the marker: this flip's surface, and whether the frame that follows it is
       an in-play one and therefore owes the viewport its erase. Both terms are
       known on this thread and neither is a per-frame value the OTHER thread
       published: `ret` is this call's own return address and `level_open` moves
       once per level. */
    if (isGame && tagpu_packet_pub_level_open()) s_winGameFlip = 1;
    if (s && s_nops < MAX_OPS) {
        OP* o = &s_ops[s_nops++];
        memset(o, 0, sizeof *o);
        o->kind = OP_FLIP; o->base = s->base;
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
    if (!s_census) { publish(s ? s->base : 0); ops_window_reset(); chrome_emit(s); return hijack; }
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
            /* DID THE ASSET CLAIM ACTUALLY HOLD? The claim `PK_ASSET` rests on
               is that nothing writes an asset surface after the loader filled
               it, and `op_add` can only enforce the half that comes through the
               17 leaves -- an engine path that re-filled one without passing a
               leaf would leave the twin holding older bytes and nothing would
               re-offer. This MEASURES that residual, for free, because the
               census already diffs every tracked surface against its own
               shadow. A surface still claiming `isAsset` whose bytes crossed
               (`assetSent`) and then moved is precisely the hole. DIAGNOSTIC,
               NOT A GUARD: the census runs only under `census.on`, so this says
               whether the residual is real, it does not close it. */
            if (s_surf[i].isAsset && s_surf[i].assetSent && c2) s_assetDrift += c2;
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
    /* THE CENSUS ABOVE CAN MOVE THIS SURFACE. `surf_drop` swap-removes
       (`s_surf[i] = s_surf[--s_nsurf]`, the rule stated at the top of this
       file), so dropping any earlier slot copies the LAST slot's contents down
       and leaves a `SURF*` that pointed at the last slot one past `s_nsurf`.
       The loop skips the flip surface itself, so it is always still in the
       table -- only its address can have changed, which is exactly what
       `surf_by_base` is for and why `s_frameBase` exists. The swapped-out bytes
       survive, so a stale pointer would still read the right values -- but the
       pointer comparison `&s_surf[i] != s` below would be wrong, and a later
       insert into that slot would alias a different surface. */
    if (s) s = surf_by_base(s->base);
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
            /* THE SAME GUARD as the `gui area:` block below: `_snprintf`'s -1
               is tested rather than added to `n`, the buffer is sized past the
               255-byte worst case, and it is always terminated. */
            char ops[288]; int k, n = 0, w; unsigned nullc = 0;
            for (k = 1; k < OP_NKIND; k++) {
                nullc += s_nullCtx[k];
                if (!s_kindCount[k]) continue;
                w = _snprintf(ops + n, sizeof ops - (size_t)n, "%s%s %u", n ? " " : "", OP_NAME[k], s_kindCount[k]);
                if (w < 0) break;
                n += w;
                if (n >= (int)sizeof ops) { n = (int)sizeof ops - 1; break; }
            }
            ops[n] = 0;
            if (!n) _snprintf(ops, sizeof ops, "none");
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
        /* ---- AND THE SAME WINDOW BY AREA, WHICH IS THE REBUILD'S QUESTION.
           Its own line rather than more fields on the one above: that line is
           read by eye and by `uiwalk`, and it is already 300 characters.
           `raw=` is the part of the drawn area `publish` can only send as a box
           of the engine's composed bytes -- the kinds with no semantic op --
           so it is the area that has NO source but an observation of the
           engine, and the area a UI pass of ours cannot reproduce until each
           of those kinds is given one. It is a SUM OVER OPS and not a covered
           area: ops overlap, so it can exceed the surface and `pct` can exceed
           100. That is the honest quantity -- de-duplicating it would need a
           coverage mask per kind, which is the census's `pgm` job and not a
           per-op tally's. Read it as weight, not as a footprint. */
        if (s_log) {
            /* THE SAME GUARD `repaint_service` CARRIES. mingw's `_snprintf`
               returns -1 on truncation rather than the length it wanted, so a
               bare `n += _snprintf(...)` makes `n` negative and
               `sizeof ar - (size_t)n` wrap to a size that writes BEFORE the
               buffer. `ar` is sized past the worst case of fifteen kinds x
               (five-char name + space + ten digits + separator) = 255, and is
               terminated even when no kind had area and the loop never ran at
               all. `full` stops the TEXT while the totals keep accruing, so
               `sem`/`raw`/`pct` stay exact either way. */
            char ar[288]; int k, n = 0, w, full = 0;
            unsigned raw = 0, sem = 0, tot;
            for (k = 1; k < OP_NKIND; k++) {
                if (!s_kindArea[k]) continue;
                /* `OP_FOCUS` IS IN THIS SET because `PK_TINT` is its semantic
                   op, and it is what moves the shell's figure: counted as raw
                   it is the whole of `raw` on MAINMENU (`raw=1156164` against
                   a `focus` of exactly that). `OP_FILL` is in it because it
                   crosses as a `PK_BAR` (the whole-surface fill branch).
                   `OP_SCALE` is NOT here and that is deliberate: it publishes a
                   `PK_SPRITE` only when its resampled plane was captured, and a
                   kind that is sometimes semantic cannot be summed as though it
                   always were. It stays in `raw`, which over-reports it -- the
                   direction an honest gap should err in. `OP_GAFB` stays out
                   for the same reason: only its RLE arm is a sprite. */
                if (k == OP_GAF || k == OP_TEXT || k == OP_BAR ||
                    k == OP_RECT || k == OP_LINE || k == OP_COPY ||
                    k == OP_FOCUS || k == OP_FILL) sem += s_kindArea[k];
                /* SPLIT, NOT PROMOTED. `OP_SCALE` is not a kind that can be
                   summed as though it were always semantic -- a rotated or
                   sheared stamp, a sub-frame stack, a window too large to key
                   and a clipped draw all publish nothing. The captured ones are
                   the common case, so the two halves are counted apart instead
                   of the whole being charged to `raw`. `s_scaleSem` can never
                   exceed `s_kindArea` because both take the same box from the
                   same op. */
                else if (k == OP_SCALE) {
                    /* the min is a BOUND and not an expectation: both sides take
                       the op's own clamped box, so they are equal or `s_scaleSem`
                       is the smaller. It is here so that a future edit which
                       breaks that cannot make `raw` underflow into a huge
                       unsigned and read as a catastrophe. */
                    unsigned semArea = s_scaleSem <= s_kindArea[k] ? s_scaleSem : s_kindArea[k];
                    sem += semArea;
                    raw += s_kindArea[k] - semArea;
                }
                else if (k != OP_FLIP)                            raw += s_kindArea[k];
                if (full) continue;
                w = _snprintf(ar + n, sizeof ar - (size_t)n, "%s%s %u",
                              n ? " " : "", OP_NAME[k], s_kindArea[k]);
                if (w < 0) { full = 1; continue; }
                n += w;
                if (n >= (int)sizeof ar) { n = (int)sizeof ar - 1; full = 1; }
            }
            ar[n] = 0;
            if (!n) _snprintf(ar, sizeof ar, "none");
            tot = sem + raw;
            _snprintf(b, sizeof b,
                "gui area: %s %s %dx%d surf=%u area=%u[%s] semantic=%u raw=%u pct=%u.%02u",
                isGame ? "GAME" : "shell", top_screen_name(), s->w, s->h,
                (unsigned)s->w * (unsigned)s->h, tot, ar, sem, raw,
                tot ? (unsigned)((unsigned long long)raw * 100u / tot) : 0u,
                tot ? (unsigned)((unsigned long long)raw * 10000u / tot % 100u) : 0u);
            b[sizeof b - 1] = 0;
            glog(b);
        }
    }
    publish(s ? s->base : 0);
    ops_window_reset();
    chrome_emit(s);
    memset(s_kindCount, 0, sizeof s_kindCount);
    memset(s_kindArea, 0, sizeof s_kindArea);
    s_scaleSem = 0;
    memset(s_nullCtx, 0, sizeof s_nullCtx);
    s_builds = 0; s_buildFlags = 0;
    return hijack;
}

/* ---- the engine redraws, instead of us seeding its bytes ----------------
   A surface whose contents we did not watch arrive can only be published as
   `PK_SEED` -- its raw bytes -- because nothing here knows how they got
   there, and a reseed (a level boundary, an arena overflow) re-publishes
   every one of them. `GUI_StageUpdateDraw 0x4A81E0(gi, 0x40)` is the engine's
   own redraw of the top screen, and every draw it makes runs through the
   leaves, so the panel's chrome arrives as ops that the twin can hold in
   palette space -- OVER the seed, not instead of it: `publish` still seeds
   a surface on first touch and the repaint replays on top.

   WHY THE FLAG IS EXACTLY 0x40 AND NOTHING ELSE [DISASSEMBLED 2026-09-18]:
   `0x4A82F0` computes `eax = flags & 1` -- the BUILD bit -- and `0x4A82F7`
   `je 0x4A90D1`, which is past BOTH allocations (`0x4A907C`, `0x4A90B5`).
   The frees: `0x4C6AC0` at `0x4A9537` and
   `0x4A9549`, and the RAW free `0x4D85A0` at `0x4A9575` and `0x4A95A7`,
   which walk the gadget array freeing each record's own pointers. All four
   are inside one block gated by `0x4A950A test bl,0x2 / je 0x4A95C2`, and
   `ebx` is reloaded from the flags argument at `0x4A9434`/`0x4A94C0`/
   `0x4A94C9`, so `bl` is the flags byte on every path into it.

   So the claim this rests on is the NARROW one, which is also the one it
   needs: under `flags == 0x40` the function ITSELF executes no allocation
   and no free, and therefore changes no lifetime of the panel surfaces.
   Verified over the whole body (`0x4A81E0..0x4A95F2`): 32 distinct direct
   call targets, NO indirect calls, and the only indirect jumps are the two
   in-body switch tables (`0x4A95F4`, `0x4A962C`). NOT verified, and
   deliberately not claimed: that nothing it CALLS allocates -- `0x4BBC40`
   and `0x4BBE50` are among its callees and both reach `0x4D85A0`. A
   callee's own transient scratch is the engine's business; the panel
   surfaces are ours. A `0x1` call twice without a teardown is what would
   leak.

   WHAT A 0x40 REDRAW ACTUALLY DOES, IN ORDER [DISASSEMBLED 2026-09-18], because
   "every pixel arrives as an op" is NOT the whole truth: `0x4A90F4` reads
   `gi->TheActive_GUIMEM->[0x24]` and, when it is set, repaints the WHOLE panel
   surface from it with `0x4C6B70(panel+0xBC, that, 0, 0)` -- a
   surface-to-surface copy, not a description of a draw. When it is NULL the
   fallback at `0x4A911D` is `0x4B0230(gi, 0, panel+0xC4)`, the picture handler,
   which is a bitmap too. Only THEN does the gadget loop at `0x4A9135` run. So
   the chrome arrives as ops and the WALLPAPER arrives as a copy, which is a win
   only while that copy's source is a surface we twin.

   WHERE IT IS CALLED FROM, AND WHY THAT IS AN ORDERING AND NOT A HOPE: at
   the flip's RETURN, on the game thread, with `s_inFlip` already cleared --
   the leaves drop every op while `s_inFlip` is set (the cursor is drawn
   inside the flip and nothing in there is UI), so a repaint issued anywhere
   inside the flip would draw and publish nothing. `s_repainting` makes the
   call non-reentrant by construction rather than by an argument about what
   the engine's gadget handlers do not do.

   WHAT THE REDRAW WRITES BESIDES PIXELS, AND WHY IT DOES NOT RUN AWAY
   [MEASURED 2026-09-18]. On the `0x40`
   path `0x4A943B` reads the focus index at `GUIMEM+0x20`; if it is not -1
   and `gi+0xA2` is set, `0x4A947B` calls `0x4A16F0(gi, idx, 8)`, which
   writes `gi+0xCCA = 1` unconditionally at `0x4A1708` and stamps gadget
   `+0x1F` (0 on every type-3 gadget at `0x4A1722`, `0x1E` on the target at
   `0x4A1758`). `gi+0xCCA` is the GUI dirty flag: the pump `0x4A9FD0` tests
   it at `0x4AA0AF`, clears it at `0x4AA0BB`, and issues ANOTHER
   `0x4A81E0(gi, GUIMEM->flags | 0x40)` at `0x4AA0CD`. So each repaint can
   provoke a further engine redraw.

   `builds=` CANNOT ANSWER IT AT THIS SAMPLE SIZE. Three boots per arm,
   `gui.on=census`, `renderer=vulkan`: shipped gave means of **1.0, 72.0, 69.1**
   redraws per census window and `norepaint` gave **28.1, 21.8, 62.1** --
   overlapping, dominated by something other than the repaint (how long a boot
   lingers on which screen), and no direction. So the amplification question is
   **NOT settled by this measurement** and no number here should be read as
   settling it.

   What IS established, in both arms and every boot: `buildFlags` is **0xC0** in
   every census window -- the engine is already issuing `0x40` redraws
   continuously on its own, at three call sites of its own (`0x41A8DA`,
   `0x4931B4`, `0x4A96BF`) -- and our repaint fires on a RESEED EDGE, 2-4 times
   a boot, not per frame. Every measured boot in both arms ended `dropped=0
   overflows=0 stalls=0` with a complete frame at 0 magenta. That is "no runaway
   was observed", which is weaker than "it does not amplify" and is what the
   evidence supports.

   AND THE DESTINATION IS CHECKED BEFORE THE CALL, not after: the redraw's
   first act writes INTO the surface `panel+0xBC` points at (the pointer
   itself is written by the BUILD, at `0x4A9092`), and `0x4C6B70` with a
   NULL destination
   builds its own context over the PRIMARY surface (terrain-depth.md) -- a
   redraw of an unbuilt screen would paint the wallpaper straight onto the
   screen. `repaint_service` refuses unless `TheActive_GUIMEM`, its
   `ControlsAry` and `panel+0xBC` are all present.                        */
#define OFF_GUIINFO   0x519           /* GUIInfo, inline in main (gui-gadgets.md 1.2)  */
#define GI_ACTIVE     0x18            /* GUIInfo.TheActive_GUIMEM                      */
/* the same field as OFF_GUI_TOP at the top of this file, reached the other way
   round: 0x519 + 0x18 == 0x531. Checked here so a correction to one spelling
   cannot silently leave the other behind. */
typedef char gui_top_spelling_agrees[(OFF_GUIINFO + GI_ACTIVE == OFF_GUI_TOP) ? 1 : -1];
#define STAGE_VA      0x004A81E0u     /* GUI_StageUpdateDraw(gi, flags) stdcall ret 8  */
#define STAGE_REDRAW  0x40            /* the redraw bit; 0x1 builds, 0x2 tears down    */
typedef void (__stdcall *gui_stage_fn)(void* gi, int flags);

static int      s_repaint = 1;        /* off with the `norepaint` token            */
static int      s_repaintPend = 0;    /* game thread only, from here down         */
static int      s_repainting = 0;
static unsigned s_repaints = 0, s_repaintSkips = 0, s_repaintOps = 0;
static int      s_repaintCounted = 0;  /* this pending episode's refusal is counted */
static int      s_drawShadow = 0;     /* our own last-seen value of g_gui_draw    */
static unsigned s_resetShadow = 0;    /* ... and of g_guiq.resets                */
static unsigned s_colarmShadow = 0;   /* ... and of g_guiq.colarm                */

/* WHEN A REPAINT IS WORTH ISSUING. A level boundary is a SUBSET of the right
   trigger, not a trigger of its own. A surface is seeded whenever `publish`
   finds `!s->seeded`, and the only thing that clears that flag for every
   surface at once is a RESEED; `publish`'s own level check is one of the four
   things that asks for one (the others are the consumer's stall-over, a lost
   sprite and an arena overflow). So the edge to watch is `g_guiq.resets`,
   which publish bumps on this same thread -- no new cross-thread agreement,
   and it covers the level case for free.

   [MEASURED 2026-09-18:] the packet's level generation advances in
   `tagpu_packet_pub_level_end`, i.e. when a level is TORN DOWN, so a
   shell-to-game transition never moves it; shadowing it fires exactly once per
   session -- at the arm -- and never on entering a game. It does not need to:
   entering a game BUILDS the in-game screen, and a build draws every gadget
   through the leaves already.

   `g_gui_draw` is read once into a local and compared with a shadow this
   thread owns, so the render thread flipping it mid-check cannot be seen
   twice differently. */
static void repaint_arm(void)
{
    int draw = g_gui_draw;
    unsigned resets = g_guiq.resets;
    /* AND THE THIRD EDGE: Classic++ colour became valid on the render half.
       Same shape as the two above and the same one-word read, because it is
       the same question -- something happened that makes what is already on
       the screen wrong, and only the engine can redraw it. See `colarm` in
       tagpu_gui_int.h for why this is not a reseed. */
    unsigned colarm = g_guiq.colarm;
    if (draw && !s_drawShadow) { s_repaintPend = 1; s_repaintCounted = 0; }
    if (resets != s_resetShadow) { s_repaintPend = 1; s_repaintCounted = 0; }
    if (colarm != s_colarmShadow) { s_repaintPend = 1; s_repaintCounted = 0; }
    s_drawShadow = draw;
    s_resetShadow = resets;
    s_colarmShadow = colarm;
}

/* ONE PER EPISODE, NOT ONE PER FLIP. A guard failure leaves `s_repaintPend`
   set -- the retry is the point -- so counting every refusal would count
   FLIPS: in the shell, ~5000 a second while a screen is between push and build
   (`repaints=3/4813992`, unreadable). */
static void repaint_refused(void)
{
    if (s_repaintCounted) return;
    s_repaintCounted = 1;
    s_repaintSkips++;
}

/* THE PANEL SURFACE REACHES THE FRAME ONLY WHILE THE ENGINE THINKS IT IS DIRTY,
   and after a reset it never does again. `0x4AB0B0` blits `panel+0xBC` to the
   frame at the panel rect and CLEARS `Active_b (+0x14)` in the same breath
   (`0x4AB111` and `0x4AB13F`), so there is exactly one blit per dirty mark; the
   other way in is the overlap test at `0x4AB136`, and the in-game side panel at
   `[0,128,128,352]` does not overlap the viewport `{128,32,W-1,H-33}`, so that
   never fires either. The ARM/CORE emblem lives in that surface and arrives
   exactly once per level -- `copies=1` -- so without a re-emit it is missing
   from every frame after the first reset. [MEASURED 2026-09-21: the probe at
   (64,285) saw one `copy box=(0,128)-(127,479)` and nothing afterwards.]

   WE DO NOT MARK THE ENGINE'S FLAG. Setting `Active_b` would be a write to engine
   state that races the engine's own use of it -- it clears the flag itself, two
   instructions after reading it -- and would buy nothing we cannot get by
   re-emitting the blit as an op, which writes nothing at all.

   WHY THE SOURCE TWIN IS FILLED, BY ORDERING. The redraw issued immediately above
   repaints the whole panel surface: in game `GUIMEM+0x24` is NULL, so `0x4A911D`
   takes the picture handler `0x4B0230(gi, 0, panel+0xC4)`, which draws one
   128x352 GAF into `panel+0xBC` [MEASURED 2026-09-21: `repaint #4 -- 1 op(s)
   [gaf 1]`, base `0BB00090` = the panel object's own `+0x0C`, box (0,0)-(127,351)].
   Those ops and this copy go into the SAME window, this one strictly after them,
   and the consumer replays a window in array order. So the copy reads a twin the
   ops just above it built -- position in `s_ops`, not a claim about timing.

   AND THE GUARD IS NOT AN OPTIMISATION. `twin_copy`'s caller asks for a RESEED
   when the source twin is missing (`g_guiq.why = WHY_COPY`), and a reseed is a
   reset, and a reset owes another of these copies: emitting one blind is a loop,
   not a wasted op. So we emit only when the redraw actually put an op on the
   panel surface's own base -- a checked fact about this window. When it did not,
   the debt is kept and the next redraw gets the chance; nothing is emitted. */
static void panel_emit(const char* ctrls, int n0)
{
    const int* psurf;
    SURF* fs;
    unsigned pbase;
    int i, filled = 0;

    if (!s_panelPend) return;
    if (!tagpu_reclaim_level_tracked() || tagpu_reclaim_level_closing()) {
        s_panelPend = 0; s_panelRefused++; return;
    }
    if (!tagpu_packet_pub_level_open()) return;      /* not yet in play: keep the debt */
    fs = surf_by_base(s_frameBase);
    if (!fs) return;
    psurf = (const int*)(size_t)*(const void* const*)(ctrls + P_SURFACE);
    if (!ptr_ok(psurf)) { s_panelPend = 0; s_panelRefused++; return; }
    pbase = (unsigned)psurf[CTX_BASE];
    if (!pbase) { s_panelPend = 0; s_panelRefused++; return; }
    for (i = n0; i < s_nops; i++)
        if (s_ops[i].base == pbase) { filled = 1; break; }
    if (!filled) return;                             /* keep the debt */
    surf_of_ctx(psurf);                              /* the source is a surface too */
    copy_record(fs, NULL, psurf,
                *(const short*)(ctrls + P_RECT_X), *(const short*)(ctrls + P_RECT_Y));
    /* THE DEBT IS PAID BY A RECORD, NOT BY A CALL. `op_add` drops silently at
       `s_nops >= MAX_OPS` and leaves `s_lastOp` NULL, which `copy_record`
       already tests before filling in the source -- so on a full ring clearing
       the debt would clear it for a copy that was never recorded, and the
       emblem would stay missing until the next reset. The debt is kept
       instead; the next repaint re-offers it. */
    if (!s_lastOp) return;
    s_panelPend = 0;
    s_panelEmits++;
}

/* MAKE THE ENGINE REDRAW ITS OWN RESOURCE BLOCK, rather than drawing one.
   Everything in that block already publishes through leaves we have -- the bar
   fill is `0x4BF6F0` twice, the six numbers are `0x4C14F0` -> `0x4CCF60` three
   times -- so the only thing missing after a reset is a REASON for the engine to
   run it again. `HUD_MEMO` is that reason, and one byte of it is enough.

   WHY BYTE 0 AND NOT THE WHOLE MEMO. The block seeds its stack local FROM the
   memo and then overwrites the live fields, so a byte it does not overwrite
   compares equal however we poison it (local and memo carry the same poison) and
   would buy nothing. Byte 0 is overwritten, at `0x468E7F`, with a value read
   fresh out of the player record -- and, unlike bytes 1..4 which `flds
   0x79(%esp)` reads at `0x468E7B` as the number's animation state, it is never
   read back from the memo. So poisoning byte 0 changes the COMPARISON and
   nothing the engine displays: no value is fabricated, and the redraw recomputes
   every field from the player record regardless. Poisoning the animated bytes
   would have made the numbers converge from a value we invented.

   BOUNDED, AND NOT BY LUCK. A poison equal to the byte the block would compute
   compares equal and skips, so the debt is kept and the poison is COMPLEMENTED
   on the retry: the fresh byte cannot equal both x and ~x, so the second attempt
   must differ. At most two pokes per reset, and the debt clears as soon as the
   memo stops reading back as our poison -- which is the engine having written
   its own fresh state over it, i.e. having drawn.

   ORDERING, NOT TIMING. This runs at the flip's RETURN, on the game thread --
   which is INSIDE `DrawGameScreen`, after its resource block has already run
   for this frame. Nothing is mid-read, and the next frame's `repz cmpsb` is the
   first thing to look at what we wrote. */
static void hud_invalidate(void)
{
    char* ta;
    volatile unsigned char* memo;
    if (!s_hudPend) return;
    if (!tagpu_reclaim_level_tracked() || tagpu_reclaim_level_closing()) {
        s_hudPend = 0; s_hudRefused++; return;
    }
    if (!tagpu_packet_pub_level_open()) return;      /* not yet in play: keep the debt */
    ta = *(char* const*)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    memo = (volatile unsigned char*)(ta + HUD_MEMO);
    /* PER EPISODE, NOT EVER. Whether ANY poke has landed (`s_hudPokes`) is a
       different question: once one has, the memo has long since been
       overwritten by the engine's own fresh state, so the test would pass on
       every LATER reset and clear each new debt without poking at all
       (MEASURED: three resets, `hud=1/0`, bars still black). `s_hudPoked` is
       cleared with every new debt, so the test only ever asks about the poke
       this episode made. */
    if (s_hudPoked) {
        /* ONE POKE PER DEBT, BY CONSTRUCTION -- not one per flip. `after_flip`
           is on the flip FUNCTION `0x4C63A0`, which has 44 call sites; it is
           NOT only the flip inline in `DrawGameScreen` at `0x46A3DB`, and even
           that one is conditional (`je 0x46A3E0`). The chrome painter
           `0x467D70` presents at `0x467E41` and returns, so presents do happen
           with no resource block between them. There, "the memo still holds our
           poison" does not mean "the engine has not redrawn yet" -- it means
           nothing has looked, and complementing and re-poking would run once
           per flip for as long as that lasted. The poison is already in place:
           keep the debt and write nothing more. */
        if (*memo != s_hudPoison) { s_hudPend = 0; s_hudPoked = 0; }
        return;
    }
    s_hudPoison = (unsigned char)(s_hudPoison ^ 0xFFu);
    *memo = s_hudPoison;
    s_hudPoked = 1;
    s_hudPokes++;
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
    /* THE LIFETIME GATE, as `panel_emit`, `hud_invalidate` and `chrome_emit`
       have it -- and this is the only one of the four that CALLS INTO the
       engine. The four `ptr_ok` tests below are
       range tests on VALUES and are not a lifetime argument (this project's own
       rule); what keeps the redraw off a level that is going away is asking the
       reclaimer.

       `level_closing()` ALONE, and NOT the other three's
       `!level_tracked() || level_closing()`. Those two are level-scoped -- the
       resource block and the side panel exist only in a game -- but a repaint
       is exactly as necessary in the SHELL, where `s_levelTracked` is 0 by
       definition. Copying their gate here would refuse every menu repaint in
       the game's whole front end. `level_closing()` is
       `s_levelTracked && s_teardown`, so it is false in the shell and true only
       while a tracked level is actually going away, which is the one window
       this call must not be in. */
    if (tagpu_reclaim_level_closing()) { repaint_refused(); return; }
    /* ptr_ok here is a range test on a VALUE, as everywhere else in this file;
       what makes the call safe is the three-link check below plus the flag */
    ta = *(const char* const*)TA_MAINPP;
    if (!ptr_ok(ta)) { repaint_refused(); return; }
    gi = (char*)(size_t)ta + OFF_GUIINFO;
    gm = *(const char* const*)(gi + GI_ACTIVE);
    if (!ptr_ok(gm)) { repaint_refused(); return; }     /* 0x4A81EA's own early return */
    ctrls = *(const char* const*)(gm + GM_CTRLS);
    if (!ptr_ok(ctrls)) { repaint_refused(); return; }  /* ebp at 0x4A8202 */
    if (!ptr_ok(*(const void* const*)(ctrls + P_SURFACE))) { repaint_refused(); return; }
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
           offset whatever the platform does, and `n` is clamped below because a
           return of exactly the space left is a fit that msvcrt also leaves
           unterminated — the one case "cannot go negative" does not cover.

           TWO NUMBERS, TWO QUANTITIES. `s_repaintOps` is `s_nops - n0`: ops
           RECORDED. The breakdown is the `s_kindTotal` delta, and `op_add` bumps
           that BEFORE its `!s`, fully-clipped and `MAX_OPS` early returns — so it
           is ops ATTEMPTED. They agree on every screen measured so far; where
           they do not, the line says so rather than letting the reader assume. */
        char b[448], kinds[288];
        int n = 0, w;
        unsigned att = 0;
        for (k = 1; k < OP_NKIND; k++)
            if (s_kindTotal[k] != before[k]) {
                att += s_kindTotal[k] - before[k];
                w = _snprintf(kinds + n, sizeof kinds - (size_t)n, "%s%s %u",
                              n ? " " : "", OP_NAME[k], s_kindTotal[k] - before[k]);
                if (w < 0) break;                 /* out of room: keep what fits */
                n += w;
                if (n >= (int)sizeof kinds) { n = (int)sizeof kinds - 1; break; }
            }
        kinds[n] = 0;
        if (!n) _snprintf(kinds, sizeof kinds, "none");
        if (att == s_repaintOps)
            _snprintf(b, sizeof b, "gui: repaint #%u -- 0x4A81E0(gi, 0x40) on the top screen: %u op(s) [%s] (skips=%u)",
                      s_repaints, s_repaintOps, kinds, s_repaintSkips);
        else
            _snprintf(b, sizeof b, "gui: repaint #%u -- 0x4A81E0(gi, 0x40) on the top screen: %u of %u op(s) recorded [%s] (skips=%u)",
                      s_repaints, s_repaintOps, att, kinds, s_repaintSkips);
        b[sizeof b - 1] = 0;
        glog(b);
    }
    /* after the log block on purpose: `s_repaintOps` and the kind breakdown are
       what the REDRAW drew, and this op is ours. */
    panel_emit(ctrls, n0);
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
    hud_invalidate();
    return s_retDepth > 0 ? s_retStack[--s_retDepth] : NULL;
}

/* ---- the leaf observers -------------------------------------------------
   Each reads the engine's arguments off entry_esp: [0] return address, [1]
   first stack argument, ... Conventions and boxes per leaf are in
   gui-renderer.md's appendix and the engine map; the byte strings are the
   prologues stolen, byte-matched at install. */

#include "tagpu_gui_leaves.h"

/* ---- the chrome re-emit --------------------------------------------------
   Owed by a reset (see `publish`), paid at the top of the next op window.

   TWO GATES, NEITHER OF THEM OURS, and between them they replace every
   lifetime argument this function would otherwise need:

     - `tagpu_packet_pub_level_open()` -- a level is actually on screen. True
       only after the engine's first in-play draw, and the engine installs that
       draw's call site (0x498342) BEFORE it paints the chrome (0x49842A), both
       inside 0x497CE0. So this cannot be true before the art exists. It is also
       what keeps us out of the shell and off the loading screen.

     - `tagpu_reclaim_level_closing()` -- the teardown is in flight. Raised in
       the PRE hook, before the first per-level asset is freed, and lowered after
       the generation moves. The sequence tables are NEVER NULLed while the banks
       behind them are freed at teardown, so reading them during the cascade
       would hand us a dangling pointer; this refuses instead. Its companion
       `tagpu_reclaim_level_tracked()` says whether that ordering exists at all,
       and 0 there is a reason to refuse, never to proceed -- so a build with the
       teardown wrap missing draws no chrome rather than an unsafe one.

   WHY THE GATES AND NOT A REACHABILITY ARGUMENT. `publish` and the teardown both
   run on the game thread and 0x491B60 reaches no flip, so in principle they
   cannot interleave -- but that closure is 610 functions and contains 362
   indirect call sites, which is not something a static read can close. The
   gates are a bound instead, and bounds do not care what a function pointer
   does.

   NOT-YET-OPEN IS A RETRY, NOT A REFUSAL. A reset can land between the paint and
   the first in-play draw; clearing the debt there would lose the chrome for the
   whole level. The debt is kept until it is paid or the level closes. */
static void chrome_emit(struct SURF* fs)
{
    /* THE TWO BARS TILE, THE PANEL DOES NOT. The bar frames are 513 px wide and
       the engine lays them end to end from 0x81 until it runs out of screen --
       measured, not assumed: the probe caught the second one at
       `gaf box=(642,736)-(1023,767)`, and 0x81 + 513 is exactly 642, with the
       right edge clipped by the surface rather than by a narrower asset.
       Drawing each bar once would leave everything past 642 black, which at
       1024 wide is 382 px of every bar and at 1920 would be most of it. */
    /* `ext` is the sequence a bar CONTINUES with past its first tile, and for
       the top bar that is the BOTTOM bar's art, not its own. Established by
       measurement rather than from the engine's code: in the golden source the
       top bar's x642..1023 is 97.1% identical to the bottom bar's tiles and
       only 9.8% identical to its own left half, which carries METAL and ENERGY.
       Repeating its own frame would draw a second METAL / ENERGY panel at
       x=642, and its sequence holds one frame so there is no plain variant
       inside it. NOT DERIVED FROM THE PRODUCER: the engine function that lays
       these continuations is still unidentified -- it is neither 0x467D70
       (three blits, no loop) nor either site in 0x46A860 (both redraw the same
       first tile). This reproduces what that function's output looks like. */
    static const struct { unsigned tbl; unsigned ext; int x; int bottom; int tile; } PIECE[3] = {
        { CHROME_TOP,    CHROME_BOTTOM, CHROME_XOFF, 0, 1 },
        { CHROME_BOTTOM, CHROME_BOTTOM, CHROME_XOFF, 1, 1 },
        { CHROME_PANEL,  0,             0,           0, 0 },
    };
    const char* ta;
    const char* gfx;
    const char* rec;
    unsigned side;
    int h, w, i, made = 0;

    if (!s_chromePend || !fs) return;
    if (!tagpu_reclaim_level_tracked() || tagpu_reclaim_level_closing()) {
        s_chromePend = 0; s_chromeRefused++; return;
    }
    if (!tagpu_packet_pub_level_open()) return;        /* not yet in play: keep the debt */

    ta = *(const char* const*)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    gfx = *(const char* const*)TA_GFX_PP;
    if (!ptr_ok(gfx)) return;
    h = *(const int*)(gfx + GFX_SCREEN_H);
    w = *(const int*)(gfx + GFX_SCREEN_W);
    if (h <= CHROME_YOFF || w <= CHROME_XOFF) { s_chromePend = 0; s_chromeRefused++; return; }

    rec = *(const char* const*)(ta + CHROME_PLRTBL +
                                (unsigned)*(const unsigned char*)(ta + CHROME_PLAYER) * CHROME_STRIDE);
    if (!ptr_ok(rec)) return;
    side = *(const unsigned char*)(rec + CHROME_RECSIDE);
    if (side >= CHROME_SIDES) { s_chromePend = 0; s_chromeRefused++; return; }

    s_chromePend = 0;
    /* THE FILL FIRST, because that is the order 0x467D70 itself uses: it calls
       0x4C6890(offscreen, 0) at its head and only then blits the three pieces.
       Reproducing it is what puts black where the engine leaves black -- the
       column below the 129x480 panel above all, which is 36 896 px of index 0 in
       the golden source and the lane's magenta without this fill. */
    op_add(OP_FILL, fs, 0, 0, fs->w - 1, fs->h - 1);
    if (s_lastOp) s_lastOp->col = 0;
    for (i = 0; i < 3; i++) {
        void* seq = *(void* const*)(ta + PIECE[i].tbl + side * 4);
        const unsigned char* fr;
        int x, y, step, n;
        if (!ptr_ok(seq)) continue;
        fr = (const unsigned char*)((gaf_fetch_fn)GAF_FETCH_VA)(seq, 0);
        if (!ptr_ok(fr)) continue;
        /* the engine's own arithmetic: it passes hotspot + offset and the blit
           subtracts the hotspot again, so these land at (x, y) exactly */
        y = (PIECE[i].bottom ? h - CHROME_YOFF : 0) + GF_HY(fr);
        step = PIECE[i].tile ? (int)GF_W(fr) : 0;
        /* BOUNDED BY COUNT, NOT ONLY BY THE EDGE. `step` comes from a frame
           header, which is engine DATA: a zero or negative width would spin this
           loop for ever and a one-pixel one would emit thousands of ops. The
           count is the bound; the screen edge is the ordinary exit. */
        for (n = 0, x = PIECE[i].x; n < CHROME_TILEMAX; n++) {
            if (n == 1 && PIECE[i].ext && PIECE[i].ext != PIECE[i].tbl) {
                void* es = *(void* const*)(ta + PIECE[i].ext + side * 4);
                const unsigned char* ef = ptr_ok(es)
                    ? (const unsigned char*)((gaf_fetch_fn)GAF_FETCH_VA)(es, 0) : NULL;
                if (ptr_ok(ef)) { fr = ef; step = (int)GF_W(fr); y = GF_HY(fr); }
            }
            gaf_record(NULL, fs, fr, x + GF_HX(fr), y, OP_GAF);
            made++;
            if (step <= 0) break;
            x += step;
            if (x >= w) break;
        }
    }
    if (made) s_chromeEmits++;
    if (s_log && made) {
        char b[160];
        _snprintf(b, sizeof b, "gui chrome: re-emitted %d op(s) for side %u at %dx%d (reset #%u)",
                  made, side, w, h, g_guiq.resets);
        b[sizeof b - 1] = 0;
        glog(b);
    }
}

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

/* TWO INSTALLS, NOT ONE, AND ONLY THE SECOND IS THE UI LAYER'S.

   The flip observer is this file's hook, but it is not this file's FEATURE:
   `before_flip` is the only host of `tagpu_triggers_frame`, which is every
   `tacli` verb's way into the process and the one the input injection
   `tacli keys` and `tacli click` go through. Behind `read_tokens()` with
   everything else, `tagpu_gui.on` being absent would take the tooling channel
   down with the UI layer.

   THAT IS THE DEFAULT FOR A BARE INSTANCE. `tagpu_opt_read` answers -1 when the
   file is absent AND no default applies, and no default applies while
   `tagpu_defaults.off` exists — which `tacli launch` writes unless it is given
   `--defaults`. Gated on it, `tacli launch <i>` followed by `tacli click <i> …`
   would write the token file, print `sent:`, and nothing would read it, ON
   EVERY RENDERER; and `tacli` also arms `tagpu_shield.on` by default, so
   hardware input would be blocked too: drivable by nothing.

   So the observer installs whenever its bytes match, and `tagpu_gui.on` gates
   the CAPTURE alone. Two further consequences, both wanted:
     - a build whose LEAF prologues differ does not cost us the flip as well;
       the tooling survives where only the capture cannot install.
     - `tagpu_gui.off` and a bare launch still take the capture, the census and
       the leaves away — they do not take `tacli`'s ability to drive the
       instance with them.

   EVERY EXIT LOGS, AND THERE ARE FIVE. Only the first two mean the instance
   cannot be driven, and both of those say so in the words `no tacli verb can
   answer` — that phrase is the diagnostic, not the absence of a line. */
void tagpu_gui_init(void)
{
    char b[256];        /* 221 worst case: 126 literal characters, the longest
                           `%s` (`FAILED`, 6), 8 `%d` at eleven digits and the
                           NUL. All eight are small flags, but `_snprintf` does not
                           NUL-terminate on truncation and `glog` reads `%s`,
                           so one byte short is an out-of-bounds READ, not a cut. */
    int n, ok, want, phase = 0;

    want = read_tokens();

    /* DllMain runs on the process's first thread, which is the thread TA's
       game loop and every flip run on; taking it here rather than at the
       first flip means the splash screen's draws (before flip 1) are recorded */
    s_gameTid = GetCurrentThreadId();

    if (!tagpu_detour_bytes_ok(FLIP_VA, FLIP_STOLEN, sizeof FLIP_STOLEN)) {
        glog("gui: NOT armed — engine bytes differ at the flip 0x4C63A0; the "
             "trigger family has no host on this build and no tacli verb can answer");
        return;
    }
    ok = tagpu_detour_observe(FLIP_VA, FLIP_STOLEN, sizeof FLIP_STOLEN, before_flip, after_flip);
    if (!ok) {
        glog("gui: NOT armed — the flip observer refused to install; the trigger "
             "family has no host and no tacli verb can answer");
        return;
    }
    if (!want) {
        glog("gui: trigger host only (tagpu_gui.on is not on) — the flip 0x4C63A0 "
             "is observed, so tacli's triggers and input injection work; the op "
             "capture, the census and the 17 leaves are OFF");
        return;
    }

    /* the op capture proper: all-or-nothing over the leaves */
    if (!leaves_match()) {
        glog("gui: op capture NOT armed — engine bytes differ at a watched leaf "
             "(the flip observer is installed, so tacli still works)");
        return;
    }
    g_guiq.ops = s_qops;
    s_arena = (unsigned char*)VirtualAlloc(NULL, TAGPU_GUI_ASIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_guiq.arena = s_arena;
    if (!s_arena) {
        glog("gui: op capture NOT armed — no arena (the flip observer is installed, "
             "so tacli still works)");
        return;
    }
    s_opsLive = 1;            /* before the install: a partial one still pushes ops */
    n = leaves_install();
    s_installed = n == LEAF_COUNT;
    /* AND THE PROVENANCE BRACKET, WITH THE LEAVES AND NOT BEFORE THEM: the four
       redirects exist to stamp ops, and without the leaves there is no op to
       stamp. It arms independently — a refusal there leaves the op capture
       working and the engine's world draws crossing, and the log line says
       so. */
    phase = phase_install();
    _snprintf(b, sizeof b, "gui: %s flip@0x4C63A0=%d leaves=%d/%d worldphase=%d census=%d log=%d pgm=%d key=%d (the op stream feeds the UI pass; no engine pixel is carried)",
              s_installed ? "ARMED" : "FAILED", ok, n, LEAF_COUNT, phase, s_census, s_log, s_pgm, s_key);
    b[sizeof b - 1] = 0;      /* _snprintf does not terminate what it truncates */
    glog(b);
}

/* "Did the 17 leaves install": `tagpu_gui_surf.c` asks this before drawing its
   layer. */
int tagpu_gui_installed(void) { return s_installed; }

/* THE MINIMAP HANDSHAKE. `tagpu_gui_surf.c`'s sharp minimap layer raises
   `g_wantMm` once per present and publishes `g_mmHave` from its own frame, so
   `tagpu_packet_pub.c` interleaves the three 126-px surfaces and the level's
   picture, and `want_minimap_watchdog` releases the want when the layer stops
   asking. */
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
    /* mingw's `_snprintf` does not NUL-terminate on truncation, and `glog`
       hands the result to `fprintf("%s")`, so an undersized buffer is an
       out-of-bounds READ, not a tidy cut. */
    char b[600];        /* 571 worst case, COUNTED OUT OF THE FORMAT STRING rather
                           than adjusted by eye: 207 literal characters, 33 `%u` at
                           ten digits, and THREE `%d` at eleven (`world=…/%d`,
                           `surfaces=%d`, `draw=%d`) plus the NUL. A conversion is
                           sized by its TYPE, not by the values you expect in it:
                           `s_phaseLive` only ever holds 0 or 1 and still counts
                           eleven. The `asset=` and `tint=` groups are FOUR fields
                           each. */
    want_minimap_watchdog(frame_counter);
    if (!s_installed) return;
    if (frame_counter - last >= 600) {
        last = frame_counter;
        /* SIZED FOR THE COUNTERS, NOT FOR THE LINE YOU LAST SAW. Thirty-three
           `%u`s at ten digits and three `%d` at eleven, plus 207 literals, is
           571 bytes, under the 600 of `b`. The observed line is ~300; the gap is
           entirely how long the session has run. COUNT IT AGAIN when you add a
           group. */
        _snprintf(b, sizeof b, "GUI flips=%u ops=%u dropped=%u world=%u/%u/%d fillcut=%u vpclear=%u changed=%u unexplained=%u surfaces=%d published=%u bytes=%u queue=%u resets=%u overflows=%u stalls=%u draw=%d flush=%u repaints=%u/%u rops=%u chrome=%u/%u panel=%u/%u hud=%u/%u asset=%u/%u/%u/%u tint=%u/%u/%u/%u",
                  s_flips, s_opsTotal, s_opsDropped,
                  s_worldOps, s_worldDropped, s_phaseLive, s_fillClipped, s_vpClears,
                  s_changedTotal, s_unexplTotal, s_nsurf,
                  s_pubOps, s_pubBytes, g_guiq.qHead - g_guiq.qTail, g_guiq.resets, g_guiq.overflows, g_guiq.stalls, g_gui_draw, s_freeqFlush, s_repaints, s_repaintSkips, s_repaintOps,
                  s_chromeEmits, s_chromeRefused, s_panelEmits, s_panelRefused, s_hudPokes, s_hudRefused,
                  s_assetSends, s_assetAcked, s_assetRevoked, s_assetDrift,
                  s_tints, s_lhtCopies, s_focusRowBad, s_tintNoTable);
        glog(b);
        {
            /* THE SAME GUARD AS THE OTHER THREE IN THIS FILE: mingw's
               `_snprintf` returns -1 on truncation, so a bare `n +=` makes `n`
               negative and the size argument wrap. `ops` is also read by `%s`,
               so it is terminated even when no kind has fired. */
            int k, n = 0, w;
            char ops[300];
            for (k = 1; k < OP_NKIND; k++) {
                if (!s_kindTotal[k]) continue;
                w = _snprintf(ops + n, sizeof ops - (size_t)n, "%s%s %u", n ? " " : "", OP_NAME[k], s_kindTotal[k]);
                if (w < 0) break;
                n += w;
                if (n >= (int)sizeof ops) { n = (int)sizeof ops - 1; break; }
            }
            ops[n] = 0;
            if (!n) _snprintf(ops, sizeof ops, "none");
            _snprintf(b, sizeof b, "GUI kinds: %s", ops);
            b[sizeof b - 1] = 0;
            glog(b);
        }
    }
}

/* ---- the TNT's minimap picture, for the render thread -------------------
   Returns 1 and fills the outputs when a picture has been snapshotted since
   the last map load. `gen` changes exactly once per load, so a consumer that
   caches anything derived from these bytes drops it when the generation moves.
   The bytes are stable for the life of that generation: one writer, one write,
   and it happens inside the map loader before any frame of that map presents. */
