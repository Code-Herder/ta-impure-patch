/* tagpu_terrown.c — own the engine's terrain draw (and the fog overlay with it).

   Every terrain pixel in the 8bpp frame comes from ONE pass:

     0x483FA0(OFFSCREEN* ctx)        stdcall, ret 4 @ 0x4843B3
     called once per frame at 0x468DB0, immediately before the (normally
     no-op) map debug overlay 0x418310.

   It is a grid blit and nothing else: 32x32 8bpp tiles from TILE_SET indexed
   by TILE_MAP, at vp + col*32 - (eye & 31). No height, no LOS, no depth, no
   colour key (research/notes/terrain-depth.md 2). tagpu_terr.c reproduces it.

   Prologue: 83 EC 48 | 8B 0D E8 1D 51 00 = `sub esp,0x48; mov ecx,[0x511DE8]`
   — nine position-independent bytes ending on an instruction boundary, so the
   stub resumes at 0x483FA9 and the four bytes past our jmp are NOPped.

   THE SKIP PATH IS NOT EMPTY. The terrain repaint is why the engine's
   offscreen never needs clearing; drop it and the overlays the engine still
   draws (health bars, nanoframe wireframes, the build cursor, chat, dialogs)
   land on last frame's garbage. So in place of the blit we fill the viewport
   rect with one palette index, the KEY. Every other index in that rect is then
   by construction something the engine drew after us, and the composite in
   tagpu_native.c refuses to cover exactly those pixels — an inverted
   composite, which is what owning a full-viewport layer costs.

   THE FOG OVERLAY GOES WITH IT:

     0x4848E0(OFFSCREEN* ctx)        stdcall, ret 4 @ 0x484B48, site 0x469D8E
     prologue 83 EC 2C | 53 | 55 = `sub esp,0x2C; push ebx; push ebp`

   Two reasons. Its grey band is a shade-LUT remap of pixels already in the
   frame, so it would rewrite our key fill into flat grey blobs and every one
   of them would read as "the engine drew something here". And we reproduce
   the overlay exactly, so drawing it twice is wrong anyway.

   But its first act is a LAZY REBUILD of the screen fog grid — the grid our
   own fog rule samples every frame:

     4848f2  test byte [esi+0x14281],bl   ; bl = 8
     4848f8  jne  0x484911
     4848fa  call 0x4843c0                ; rebuild the grid
     484904  or   word [eax+0x14281],bx   ; LosType |= 8

   so the skip path replicates those five lines and nothing else. Dropping
   them would freeze the grid and take our fog parity with it. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_terrown.h"
#include "tagpu_owndraw.h"
#include "tagpu_opt.h"
#include "tagpu_terr.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"
#include "tagpu_fogwide.h"
#include "tagpu_packet_pub.h"
#include "tagpu_log.h"

#define TERRAIN_VA   0x00483FA0u   /* stdcall(ctx), ret 4  */
#define FOG_VA       0x004848E0u   /* stdcall(ctx), ret 4  */
#define FOGGRID_BUILD_VA 0x004843C0u
#define TA_MAINPP    0x00511DE8u
#define OFF_EYEX      0x1431F
#define OFF_EYEY      0x14323
#define OFF_LOSTYPE  0x14281       /* u16; bit3 = grid is current              */
#define OFF_VP_L     0x37E27
#define OFF_VP_T     0x37E2B
#define OFF_VIEW_W   0x37E37
#define OFF_VIEW_H   0x37E3B
/* OFFSCREEN (terrain-depth.md 4, re-read from 0x4CBEF1 / 0x4C6B10):
   +0x08 pitch, +0x0C pixel base, inclusive clip rect +0x1C..+0x28 */
#define CTX_PITCH    2
#define CTX_BASE     3
#define CTX_CLIP_L   7
#define CTX_CLIP_T   8
#define CTX_CLIP_R   9
#define CTX_CLIP_B   10

static const unsigned char TERR_STOLEN[9] =
    { 0x83, 0xEC, 0x48, 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 };
static const unsigned char FOG_STOLEN[5] =
    { 0x83, 0xEC, 0x2C, 0x53, 0x55 };

volatile unsigned char g_terrown_skip = 0;
/* WHAT THE TWO STUBS TEST, and it is not `g_terrown_skip`. The skip is the
   RENDER thread's request and changes whenever that thread decides; this is
   the GAME thread's decision, written only by `tagpu_terrown_latch`, which the
   packet publisher calls at the top of every in-play `DrawGameScreen` (after
   the viewport rect has been settled for the draw) and at the level end. It
   exists for `tagpu_vpwide`: the engine's terrain blit places its unclipped
   32x32 copies (`0x4CBEF1`) from the rect's L/T, so a rect widened for
   zoom < 1 -- a negative origin -- reaching that blit writes far outside the
   offscreen, which is what a new skirmish entered while the last one's zoom
   is still live, before our terrain pass has taken the ground back, does.
   Stubs reading the skip directly would leave the rect (game thread) and the
   skip (render thread) with no ordering at all.

   THE INVARIANT IS "the rect is wide => this is set", and it is kept by what
   the latch is handed rather than by what the request says: `request || the
   rect is still wide`, taken AFTER `tagpu_vpwide_apply` has had its say. So a
   rect that could not be restored (its own `true_rect_of` refusing) keeps the
   engine's blit away too. The price is that a draw may
   key-fill on a latch the render thread has already withdrawn -- the rest of
   the draw in flight, and the non-in-play callers of `DrawGameScreen`
   (`0x495C76`/`0x495E66` in the screenshot routine, `0x4962C2` the movie
   recorder), which never latch and keep the last in-play draw's value until
   the level end latches again. None of those inverts the composite:
   `tagpu_terrown_filled()` reads the request, which is already down. */
static volatile unsigned char g_terrown_own = 0;
static int g_installed = 0;
static volatile int g_filled = 0;      /* the fill has run under this skip */
static unsigned g_beat = 0, g_last = 0;

static void flog(const char* s)
{
    tagpu_log(s);
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

/* In place of the terrain blit: paint the viewport rect of the engine's
   offscreen with the key. Runs on the GAME thread, inside DrawGameScreen. */
static void __cdecl terr_fill(void* ctxv)
{
    const int* ctx = (const int*)ctxv;
    const char* ta = *(const char* const*)TA_MAINPP;
    unsigned char* base;
    int pitch, x0, y0, x1, y1, y, key;

    if (!ptr_ok(ctx) || !ptr_ok(ta)) return;
    pitch = ctx[CTX_PITCH];
    base  = (unsigned char*)(size_t)(unsigned)ctx[CTX_BASE];
    if (!ptr_ok(base) || pitch <= 0 || pitch > 16384) return;

    /* the TRUE 1x rect — tagpu_vpwide widens the engine's copy at zoom < 1 and
       a key fill taken from the wide one would erase the side panel */
    {
        int tw, th;
        tagpu_vpwide_true_rect(ta, &x0, &y0, &tw, &th);
        x1 = x0 + tw;
        y1 = y0 + th;
    }
    /* The engine's own blits are clipped to the context rect (inclusive), so
       intersecting with it can only shrink the fill to what the terrain pass
       would have covered — and that rect is the only bound this fill applies to
       the last row (the surface's own height at +0x04 is not compared). A
       context whose rect does not validate is therefore REFUSED, not filled on
       the viewport fields alone: failing here leaves g_filled clear, the
       composite does not invert, and our own opaque terrain covers the
       viewport. */
    {
        int cl = ctx[CTX_CLIP_L], ct = ctx[CTX_CLIP_T];
        int cr = ctx[CTX_CLIP_R], cb = ctx[CTX_CLIP_B];
        if (!(cl >= 0 && ct >= 0 && cr >= cl && cb >= ct && cr < pitch && cb < 8192))
            return;
        if (x0 < cl) x0 = cl;
        if (y0 < ct) y0 = ct;
        if (x1 > cr + 1) x1 = cr + 1;
        if (y1 > cb + 1) y1 = cb + 1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pitch) x1 = pitch;
    if (x1 <= x0 || y1 <= y0) return;

    key = tagpu_terr_key();
    for (y = y0; y < y1; y++)
        memset(base + (size_t)y * (size_t)pitch + x0, key, (size_t)(x1 - x0));
    /* only now: the composite inverts against this key, and a frame that did
       NOT get filled still holds a real terrain blit. Failing closed leaves our
       own FBO covering the viewport — the world is right and the engine's
       overlays are missing, which beats hiding the world. */
    g_filled = 1;
}

/* In place of the fog overlay: its lazy grid rebuild, and only that — plus the
   WIDE grid, which is ours. The engine's grid spans the 1x viewport and no
   more, so at zoom < 1 the outer ring of the view falls off its lattice
   entirely; tagpu_fogwide builds the same masks over a window the whole zoom
   range fits in. It runs here because here is where the maps it reads are the
   engine's own to read (tagpu_fogwide.h), and it needs to know whether the
   engine rebuilt on this tick — that is the same "the LOS state moved" signal. */
/* THE FOG SITE'S OWN LIVENESS, and it is a stamp rather than a flag. Our
   `terr_fogtick` runs only while `g_terrown_own` is set, and that falls on the
   next latch after the render thread drops its request on any path that hands
   terrain back — the lever removed, `passive`, `over`, a `key=` change, a
   bail-out, the 90-frame watchdog. When it does, this observer stops and the
   ENGINE calls `0x4848E0` itself, at the live eye, where we cannot see it.
   Both fog answers below then go stale and neither module can tell:
   `tagpu_fogwide`'s "valid" flag is only ever cleared from inside the tick
   that has stopped. So the tick stamps the publisher's in-play draw counter,
   and the publisher — which runs in the same draw's `after` — accepts the
   wide grid only when the stamp is this draw's. */
static unsigned s_fogDrawSeq;
static int      s_fogDrawSeen;

int tagpu_terrown_fog_site_live(void)
{
    return s_fogDrawSeen && s_fogDrawSeq == tagpu_packet_pub_draw_seq();
}

static void __cdecl terr_fogtick(void* ctxv)
{
    char* ta = *(char**)TA_MAINPP;
    unsigned short* los;
    int rebuilt = 0;
    (void)ctxv;
    if (!ptr_ok(ta)) return;
    los = (unsigned short*)(ta + OFF_LOSTYPE);
    /* we ran in this draw, rebuild or not: that is what the publisher tests */
    s_fogDrawSeq = tagpu_packet_pub_draw_seq();
    s_fogDrawSeen = 1;
    /* THE ENGINE'S OWN LAZY TEST, with nothing OR-ed into it. The
       screen fog grid is view-anchored, so an eye that moved must rebuild it,
       and every path that moves the eye says so by clearing LosType bit 3 —
       the engine's writers, and ours too:
       the cursor anchor, the camera range and the hold all move the eye from
       the GAME THREAD's command apply at the top of this same draw
       (tagpu_zoom_apply), which clears the bit exactly as `0x41CB6B` does,
       on the only thread that may (`0x484904` sets it with an unlocked
       read-modify-write, so a clear from any other thread could be lost).
       Nothing on the render thread writes the eye. */
    if (!(*(unsigned char*)los & 8)) {
        ((void (*)(void))(size_t)FOGGRID_BUILD_VA)();
        /* re-read: the builder reallocates nothing, but the engine reloads the
           TAdynmem pointer here and so do we */
        ta = *(char**)TA_MAINPP;
        if (!ptr_ok(ta)) return;
        /* set only here and at the engine's `0x484904`, each right after a
           build: the packet's publisher reads the grid's build record off
           this bit (tagpu_packet_pub.c, fog_rec_after) */
        *(unsigned short*)(ta + OFF_LOSTYPE) |= 8;
        rebuilt = 1;
    }
    tagpu_fogwide_tick(ta, rebuilt);
}

void tagpu_terrown_init(void)
{
    char b[192];
    int t = 0, g = 0;
    if (!tagpu_opt_on("tagpu_terrown.on")) return;
    if (memcmp((void*)TERRAIN_VA, TERR_STOLEN, sizeof TERR_STOLEN) != 0 ||
        memcmp((void*)FOG_VA, FOG_STOLEN, sizeof FOG_STOLEN) != 0) {
        flog("terrown: NOT armed — engine bytes differ at 0x483FA0 / 0x4848E0");
        return;
    }
    /* all-or-nothing: owning terrain without suppressing the fog overlay would
       let its shade remap rewrite the key fill, and suppressing the overlay
       without owning terrain would simply delete the fog */
    t = tagpu_detour_leaf_call(TERRAIN_VA, TERR_STOLEN, (int)sizeof TERR_STOLEN,
                               &g_terrown_own, 0x04, terr_fill);
    if (t)
        g = tagpu_detour_leaf_call(FOG_VA, FOG_STOLEN, (int)sizeof FOG_STOLEN,
                                   &g_terrown_own, 0x04, terr_fogtick);
    g_installed = (t && g);
    _snprintf(b, sizeof b,
        "terrown: %s terrain@0x483FA0=%d fog@0x4848E0=%d key=%d default "
        "(skip and key both follow tagpu_terr.on)",
        g_installed ? "ARMED" : "FAILED", t, g, tagpu_terr_key());
    flog(b);
}

void tagpu_terrown_set_skip(int on)
{
    unsigned char v = (unsigned char)(on && g_installed);
    if (v != g_terrown_skip) {
        /* THE STRUCTURE-SHADOW GATE GOES UP BEFORE THE SKIP, AND DOWN AFTER IT.
           Once the skip is visible the GAME thread key-fills the viewport, and
           from that moment anything the engine blits into its own surface --
           its cached slant shadow above all -- reaches the screen as opaque
           teal through the composite's inversion. Raising the gate first is an
           ordering (x86 does not reorder these two stores, and the game
           thread's unit blits come after its own terrain blit), so no draw can
           key-fill under a lowered gate. Lowering it after the skip is the
           benign direction: one frame of a missing shadow rather than one of
           teal. (The unit pass publishing `tagpu_terrown_filled()` a frame
           later instead gives up to two teal frames on every acquisition.) */
        if (v) tagpu_owndraw_set_structshadow_terr(1);
        /* the engine's surface still holds a real terrain blit at this instant;
           the composite must not invert until a filled frame has gone through */
        g_filled = 0;
        g_terrown_skip = v;
        if (!v) tagpu_owndraw_set_structshadow_terr(0);
        flog(v ? "terrown: engine terrain + fog overlay SKIPPED (ours live)"
               : "terrown: engine terrain + fog overlay restored");
    }
}

void tagpu_terrown_beat(unsigned int frame_counter) { g_beat = frame_counter; }

int tagpu_terrown_installed(void) { return g_installed; }

int tagpu_terrown_filled(void) { return g_terrown_skip && g_filled; }

int tagpu_terrown_request(void) { return g_terrown_skip != 0; }

void tagpu_terrown_latch(int own)
{
    g_terrown_own = (unsigned char)(own != 0);
}

void tagpu_terrown_flush(unsigned int frame_counter)
{
    if (!g_installed) return;
    /* if the native pass stops running (overlay off, a level teardown, a
       frame path that never reaches it) the engine's terrain comes back
       rather than the viewport going flat key-colour */
    if (g_terrown_skip && frame_counter - g_beat > 90) {
        flog("terrown: terrain pass silent for 90 frames");
        tagpu_terrown_set_skip(0);
    }
    if (frame_counter - g_last >= 300) {
        char b[64];
        g_last = frame_counter;
        _snprintf(b, sizeof b, "TERROWN skip=%u filled=%d",
                  (unsigned)g_terrown_skip, g_filled);
        flog(b);
    }
}
