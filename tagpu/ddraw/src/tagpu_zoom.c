/* tagpu_zoom.c — the view transform shared by the render pass and the input
   path. See tagpu_zoom.h for the contract and why it is lock-free.

   TWO HALVES, ONE FILE (since the frame packet's landing 2). The RENDER half
   owns the level and the arithmetic — the levers, the wheel's ease, the
   cursor anchor's eye delta, the predicted eye every pass draws from — and
   writes NOTHING into engine memory: its whole output is the command record
   tagpu_zoom_frame_end() posts. The GAME half — the engine's own clamp and
   minimap-rect sites this module redirects, plus tagpu_zoom_apply() at the
   top of every in-play draw — is the only code here that touches the eye,
   the scroll target, the follow slots, the minimap box, the fog flag and
   ScrollSpeed, and it does so on the thread that owns them. Every static is
   marked with the thread that owns it. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "dd.h"
#include "tagpu_opt.h"
#include "tagpu_zoom.h"
#include "tagpu_menu.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"
#include "tagpu_input.h"
#include "tagpu_packet.h"

/* Published view. Volatile because two threads touch it; each is one aligned
   32-bit slot, which x86 loads and stores atomically. */
static volatile float s_zoom = 1.0f;
static volatile LONG  s_vpL, s_vpT, s_vw, s_vh;
static volatile LONG  s_live;          /* a zoomed world is on screen right now */
static volatile LONG  s_fresh;         /* the pass published during this frame     */
static int            g_mmInstalled;   /* tagpu_zoom_init() patched the engine     */

/* The range BOTH levers share. The transform is fine outside it; these are the
   levels the rest of the stack has been checked at. */
/* the range itself is published in tagpu_zoom.h — the passes that size a
   gather from it need the same two numbers */
#define ZOOM_MIN  TAGPU_ZOOM_MIN
#define ZOOM_MAX  TAGPU_ZOOM_MAX

static void zlog(const char* m);
static int  in_viewport(int x, int y, int L, int T, int W, int H);
static void anchor_step(float zNow, int fromWheel, const TAGPU_PACKET* pk);
static void predict(const TAGPU_PACKET* pk);
static int  iround(float v);

/* ---- the wheel -------------------------------------------------------------

   The message thread only ever does one thing to the zoom: add this notch to
   s_wheelAccum. Everything else — folding those notches into a target, easing
   toward it, the clamp — happens on the render thread inside read_lever(), so
   the level still has exactly one owner and no float is ever shared across the
   two. It also batches for free: a flick that lands six notches inside one
   frame is one multiply, not six.

   The step is geometric because zoom is: a notch has to mean the same
   proportional change at 0.3x as at 6x, or the control is unusable at one end.
   1.1 per notch puts the full 0.25..8 range about 36 notches apart end to end,
   which is roughly two flicks of a real wheel and fine enough to stop where you
   meant to.

   And it EASES rather than jumping. The per-frame re-read in read_lever()
   exists precisely so a ramp is smooth (see its comment), and a bare notch is a
   9% jump of the whole world — harsh to look at, and worse at speed. A quarter
   of the remaining log-distance per frame settles in about six frames: fast
   enough to feel direct, slow enough that the eye tracks the world through it.
   Log-distance, not linear, so zooming in and out ease identically. */
#define WHEEL_STEP  1.1f      /* per notch, geometric              */
#define WHEEL_EASE  0.25f     /* of the remaining log-distance, per frame */
#define WHEEL_SNAP  0.0025f   /* log-distance at which the ease is done   */

#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

static volatile LONG s_wheelAccum;              /* raw delta, message thread */
/* WHERE THE NOTCH WAS AIMED, packed x,y as two shorts in ONE aligned 32-bit
   slot so the point cannot tear against itself — the pair is what the step
   below is about, and half of one frame's pointer with half of another's would
   be a point the player never aimed at. Message thread writes, render thread
   reads. The notches and the point are two publishes, so they CAN tear against
   each other; the bound is one frame of pointer travel times one notch of
   1/z, which is sub-pixel, and a lock on the input path buys nothing for it. */
static volatile LONG s_anchor;
static volatile LONG s_anchorSet;
static float         s_wheelTgt = 1.0f;         /* render thread only        */
static float         s_wheelCur = 1.0f;         /* render thread only        */
static LONG          s_wheelPend;               /* notches not yet logged    */

/* Pin the wheel to a level without an ease, and throw away any notches that
   arrived alongside. Used while the file lever is in force: the file wins, and
   when it goes away the wheel takes over from exactly where it left the view. */
static void wheel_pin(float z)
{
    LONG dropped = InterlockedExchange(&s_wheelAccum, 0);
    s_wheelTgt = s_wheelCur = z;
    s_wheelPend = 0;
    /* Say when the file is the reason the wheel did nothing. This is the only
       gate that swallows a notch silently — the other two report themselves
       from tagpu_zoom_wheel() — and it is the easiest to hit by accident, from
       a scenario that wrote tagpu_zoom.txt and never removed it. Throttled: the
       pin runs every frame the file exists. */
    if (dropped) {
        static DWORD tick;                       /* render thread only */
        DWORD now = GetTickCount();
        if (now - tick > 1000) {
            char b[96];
            tick = now;
            _snprintf(b, sizeof b,
                      "zoom: wheel %+d ignored - tagpu_zoom.txt is in force at %.3f",
                      (int)dropped, z);
            b[sizeof b - 1] = 0;
            zlog(b);
        }
    }
}

/* Fold in the notches since the last frame and take one step of the ease. */
static float wheel_level(void)
{
    LONG d = InterlockedExchange(&s_wheelAccum, 0);
    float lc, lt;

    if (d) {
        s_wheelTgt *= (float)pow(WHEEL_STEP, (double)d / WHEEL_DELTA);
        if (s_wheelTgt < ZOOM_MIN) s_wheelTgt = ZOOM_MIN;
        if (s_wheelTgt > ZOOM_MAX) s_wheelTgt = ZOOM_MAX;
        /* Land EXACTLY on 1.0 when the notches cancel. 1x is the identity the
           whole stack tests for by equality — the transform, the minimap rect
           and the scroll rate each short-circuit on `z == 1.0f` — so wheeling
           out and back has to restore it, not leave a 1e-7 residue that keeps
           all three live and makes 1x no longer byte-identical. The band is far
           narrower than one 10% notch, and the off-grid levels a clamp produces
           (0.25 * 1.1^n) miss it too, so nothing else can fall into it. */
        if (s_wheelTgt > 0.999f && s_wheelTgt < 1.001f) s_wheelTgt = 1.0f;
        s_wheelPend += d;
    }
    else if (s_wheelPend) {
        /* ONE line per gesture, at the end of it, not one per frame that
           carried notches. zlog is an fopen/fprintf/fclose and this runs on the
           render thread, so a sustained spin would otherwise open the log every
           frame for as long as it lasted — and unlike every other per-frame log
           in this stack (tagpu_spxlog.on and friends) there is no flag file to
           turn it off. Deferring to the settle costs nothing diagnostically: the
           total and the level it landed on are what the line was ever read for. */
        char b[64];
        _snprintf(b, sizeof b, "zoom: wheel %+d -> %.3f",
                  (int)s_wheelPend, s_wheelTgt);
        b[sizeof b - 1] = 0;
        s_wheelPend = 0;
        zlog(b);
    }
    if (s_wheelCur == s_wheelTgt) return s_wheelCur;

    lc = (float)log(s_wheelCur);
    lt = (float)log(s_wheelTgt);
    lc += (lt - lc) * WHEEL_EASE;
    /* Snap rather than approach forever: an ease that never arrives leaves the
       level a hair off the notch that was asked for, and recomputes two
       transcendentals every frame to stay there. */
    if (lt - lc < WHEEL_SNAP && lc - lt < WHEEL_SNAP) s_wheelCur = s_wheelTgt;
    else s_wheelCur = (float)exp(lc);
    return s_wheelCur;
}

/* this frame's level, and the eye the frame is drawn from (render thread) */
static float s_lever = 1.0f;
static int   s_predX, s_predY, s_havePred, s_unacked;

float tagpu_zoom_read_lever(const TAGPU_PACKET* pk)
{
    /* NO ZOOM WITHOUT THE MOUSE-WORLD REPAIR, and this is the gate that makes
       that structural rather than merely documented.

       `fake_GetCursorPos` answers the engine the TRUE pointer at every zoom, so
       the only thing that still hands its 1:1 screen->world arithmetic the
       unzoomed `u` is tagpu_vpwide's redirect of `0x498DA0`. Without that
       redirect a zoomed world would name the world point under the SCREEN
       position: hover, the cursor-shape choice, build placement, the routing
       test at `0x469DE1` and `GetUnitAtMouse` all wrong, silently, while clicks
       (which carry `u` in the message) still land correctly — half broken and
       hard to see.

       BOTH LEVERS ARE UNGATED BY ANY ARM FILE. `tagpu_zoom.on` installs the
       minimap/ScrollSpeed/camera-range patches and nothing else; the file and
       the wheel are read here, and the native pass publishes the view, with no
       arm file consulted at all. So a build with `native.on` and `terr.on` but
       no `zoom.on` could be wheeled to 0.5x and reach exactly that state. Pin
       to 1.0 instead, and say so once. */
    if (!tagpu_vpwide_mouse_world_live()) {
        static LONG said;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            zlog("zoom: PINNED AT 1.0 — the 0x498DA0 mouse->world repair is not "
                 "installed, so a zoomed world would name the point under the "
                 "SCREEN position. Arm tagpu_zoom.on (or tagpu_vpwide.on).");
        InterlockedExchange(&s_wheelAccum, 0);
        s_wheelTgt = s_wheelCur = 1.0f;
        s_wheelPend = 0;
        s_zoom = 1.0f;
        anchor_step(1.0f, 0, pk);
        predict(pk);
        s_lever = 1.0f;
        return 1.0f;
    }

    /* Re-read EVERY frame: this is a continuous control, so a 30-frame poll
       would quantise a ramp to 2 Hz and make a smooth renderer look like a
       staircase on video. On a bad parse the LAST GOOD value is kept rather
       than snapping back to 1.0 — a writer driving a ramp at 60 Hz can be
       caught mid-write, and a one-frame jump to unzoomed reads as a flicker.
       Only the file's ABSENCE hands the level to the wheel. */
    HANDLE zh = CreateFileA("tagpu_zoom.txt", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    int    fromWheel = (zh == INVALID_HANDLE_VALUE);
    if (fromWheel) {
        s_zoom = wheel_level();
    } else {
        char zb[32]; DWORD zn = 0;
        if (ReadFile(zh, zb, sizeof zb - 1, &zn, 0) && zn > 0) {
            float z;
            zb[zn] = 0;
            z = (float)atof(zb);
            if (z >= ZOOM_MIN && z <= ZOOM_MAX) s_zoom = z;
        }
        CloseHandle(zh);
        /* Pin to `s_zoom` rather than to the value just parsed: on a torn read
           that is the LAST GOOD level, which is the one actually in force and
           therefore the one the wheel must inherit when the file goes away. */
        wheel_pin(s_zoom);
    }
    /* The eye step that holds the point under the cursor, worked out HERE:
       this is the call the driver makes at the top of its frame, before any
       pass reads the eye, so the zoom and the eye it is drawn with change
       together. Only the wheel anchors — the file lever has no gesture behind
       it and every zoom fixture drives it. */
    anchor_step(s_zoom, fromWheel, pk);
    predict(pk);
    s_lever = s_zoom;
    return s_zoom;
}

float tagpu_zoom_lever(void)
{
    return s_lever;
}

int tagpu_zoom_predicted_eye(int* eyeX, int* eyeY)
{
    if (!s_havePred) return 0;
    *eyeX = s_predX; *eyeY = s_predY;
    return 1;
}

int tagpu_zoom_unacked(void)
{
    return s_unacked;
}

int tagpu_zoom_wheel(UINT msg, WPARAM wparam, LPARAM lparam)
{
    static DWORD s_offTick;       /* message thread only */
    static int   s_off;
    static DWORD s_gripeTick;
    DWORD now;
    int x, y, delta;

    if (msg != WM_MOUSEWHEEL) return 0;

    /* The escape hatch, polled rather than cached at attach so it can be
       flipped on a running instance. Throttled because a stat per notch on the
       message thread is pointless; a quarter second is well under the time it
       takes to reach for the wheel after touching the file. */
    /* Unsigned tick arithmetic, so a wrap is a non-event; a zero stamp is a
       tick count 250 ms after boot, which no running game is ever in. */
    now = GetTickCount();
    if (now - s_offTick > 250) {
        s_offTick = now;
        s_off = (GetFileAttributesA("tagpu_wheel.off") != INVALID_FILE_ATTRIBUTES);
    }
    if (s_off) return 0;

    /* `s_live` is "our zoomed world is on screen": no publish, no wheel, so the
       menus and a game not yet loaded are untouchable however hard it is spun.
       The gate is the TRUE viewport — the screen region the world is drawn in,
       which does not move with the zoom — so at zoom < 1 the wheel is live over
       the whole world including the ring, and dead over the screen-space UI. */
    x = (int)(short)LOWORD(lparam);
    y = (int)(short)HIWORD(lparam);
    if (!s_live ||
        !in_viewport(x, y, (int)s_vpL, (int)s_vpT, (int)s_vw, (int)s_vh)) {
        /* Say so, once a second: a notch that does nothing is otherwise
           indistinguishable from a wheel that is not wired up at all, and the
           two reasons want different fixes. */
        if (now - s_gripeTick > 1000) {
            s_gripeTick = now;
            zlog(s_live ? "zoom: wheel ignored — pointer is off the world viewport"
                        : "zoom: wheel ignored — no zoomed world on screen");
        }
        return 0;
    }

    delta = (int)(short)HIWORD(wparam);
    if (!delta) return 0;
    /* AIM FIRST, THEN THE NOTCH. The point is published before the delta so a
       render thread that sees the notches has, by then, a point at least as
       new as they are — the tear can only be a point NEWER than its notches,
       which is the harmless direction (it anchors where the pointer is now).
       The gate above has already proved this point is inside the true
       viewport, which is what makes it a legal anchor. */
    InterlockedExchange(&s_anchor,
                        (LONG)(((unsigned)(unsigned short)y << 16) |
                               (unsigned)(unsigned short)x));
    InterlockedExchange(&s_anchorSet, 1);
    InterlockedExchangeAdd(&s_wheelAccum, (LONG)delta);
    return 1;
}

void tagpu_zoom_wheel_locked_out(void)
{
    static DWORD tick;                       /* message thread only */
    DWORD now = GetTickCount();
    if (now - tick > 1000) {
        tick = now;
        zlog("zoom: wheel ignored - the window has no mouse lock yet "
             "(click in it once)");
    }
}

void tagpu_zoom_publish_view(int vpL, int vpT, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    s_vpL = vpL; s_vpT = vpT; s_vw = vw; s_vh = vh;
    s_live = 1;
    s_fresh = 1;
}

/* ---- scroll rate -----------------------------------------------------------

   `main+0x1434D` is the engine's scroll speed (u8): the registry value
   "ScrollSpeed" under HKCU\Software\Cavedog Entertainment\Total Annihilation,
   loaded at 0x42FB62 and defaulted to 0x20 when the value is missing. It is a
   distance in WORLD pixels per scroll tick, so at zoom z the same pointer dwell
   at the screen edge moves the view z times less far across the SCREEN, and
   scrolling at 0.25x feels glued. Scale it by 1/z and the screen moves at the
   same rate at every zoom.

   Written on the GAME THREAD, from the apply at the top of every in-play draw
   (before the scroll poll of the NEXT frame reads it), and safe to write: the
   eye is not simulated — it is a per-player camera preference that no other
   machine ever sees — so this cannot perturb the sim or an MP session.

   The base is re-read from the engine whenever the value is not the one we last
   wrote, so changing the preference in the options screen is picked up on the
   next draw instead of being overwritten by a stale cache.

   AND THE FIELD IS PERSISTED, which is why `zoom_save_scroll` below exists.
   `0x430F00` (save-all-preferences, called from a dozen places — e.g. `0x416D03`
   sets the scroll speed and `0x416D09` immediately saves) writes this live byte
   to the registry at `0x430FAE`. Left alone, a zoomed session would persist the
   SCALED number as the player's preference, and because the next launch loads it
   as the new base it would COMPOUND across launches (32 → 64 → 128 …). So the
   one call site that persists it is redirected to save the player's own value.
   The options screen still displays the scaled number while zoomed, which is at
   least the rate scrolling is actually running at. */
#define TA_MAINPP        0x00511DE8u
#define OFF_SCROLLSPEED  0x1434D

/* game thread only: the level the last command carried, and whether it is
   live — what every game-thread reader in this file uses in place of the
   render thread's s_zoom/s_live, so no float crosses the threads */
static float s_gLevel = 1.0f;
static int   s_gLive, s_gEyeOff;
static unsigned char s_scrollBase, s_scrollWrote;   /* game thread only */

static float game_level(void)
{
    return s_gLive ? s_gLevel : 1.0f;
}

static void apply_scroll_rate(char* ta)
{
    unsigned char* p = (unsigned char*)(ta + OFF_SCROLLSPEED);
    float z;
    int v;

    if (*p != s_scrollWrote) s_scrollBase = *p;   /* the engine or the player */
    if (!s_scrollBase) return;

    z = game_level();
    /* Without the save guard installed (tagpu_zoom.on absent) the scaled value
       would reach the player's registry, so leave the field alone entirely. */
    v = (g_mmInstalled && z > 0.05f && z != 1.0f)
        ? (int)((float)s_scrollBase / z + 0.5f) : (int)s_scrollBase;
    if (v < 1) v = 1;
    if (v > 255) v = 255;
    *p = s_scrollWrote = (unsigned char)v;
}

/* ---- the camera's range ----------------------------------------------------

   `0x41C3C0` clamps the eye to `[0, map - W]`, W being the 1x viewport size:
   the range that puts the VIEWPORT's own edges exactly on the map's. At zoom z
   the view is still centred on `eye + W/2` but is only W/z wide, so those
   bounds stop the visible window `W/2 - W/(2z)` short of the map on every side.
   Zoomed in, the edges and corners of the map cannot be reached at all, and the
   camera reads as though it were being pushed back off them.

   THE RANGE THE ZOOM ACTUALLY NEEDS is the engine's own, widened by exactly
   that shortfall:

       d   = (W/2)(1 - 1/z)            0 at 1x, and W/2 in the limit
       eye in [-d, (map - W) + d]

   which is the same arithmetic the transform uses about the same centre, so the
   two cannot disagree at the edges: the world at the viewport's left edge is
   `eye + d`, which is 0 exactly when `eye = -d`. At z <= 1, d is 0 and the range
   is the engine's own, byte for byte.

   Everything that reads the EYE follows for free, because the eye is the
   engine's camera: the minimap's view box, the minimap click jump, the mouse
   and edge scroll, the HotUnits cull and our own passes need nothing new.

   WHAT IS NOT COVERED, and it is the SCROLL TARGET `main+0x14327`/`+0x1432B`
   that draws the line. Every path that sets the eye and copies it into the
   target afterwards (`0x41C574` SetCamera, the path whose clamp call is `0x41CDE1`, the scroll `0x41D037`)
   reaches the widened range through us. But three sites compute that target and
   clamp it INLINE against `[0, map - W]` without going through this function at
   all — `0x41C4C0` (the smooth SetCamera), `0x41C7F7` (the smooth centre-on) and
   `0x41CAF7` (the per-frame camera FOLLOW, which recomputes the target from the
   tracked unit every frame). The stepper `0x41CA10` then eases the eye to that
   target and our clamp, being wider, leaves it there — so those paths still stop
   `d` short of a map edge. Nothing fights and nothing churns (the eye arrives at
   a target that is inside our range and both stop), it is simply the old
   behaviour on the paths the detour does not sit on. The follow's clamp is the
   one of the three that has been closed (zoom_follow_clamp below).

   MECHANISM: a `leaf_call` detour on the clamp itself, on a flag raised only
   while a zoomed-IN world is live. With it clear the engine's own function runs
   verbatim — including the two minimap-rect redirects INSIDE it — so 1x and
   zoom-out are untouched. With it set our replacement does the whole job: the
   widened clamp, then the same minimap rect the engine computes last, through
   the same wrapper.

   AND THE ONE THING AN OFF-MAP EYE BREAKS. `0x498DA0` hands a pointer OUTSIDE
   the viewport the world point `eye + clamp(pos, L, R) - L`, which on the side
   panel is `eye` itself and under the bottom bar is `eye + H - 1`: on the map
   for every eye the ENGINE can produce, off it for ours. The chain from there
   is not defensive — `GetGridPosPLOT` returns NULL outside the map and
   `GetGridPosFeature` dereferences whatever it is handed, the crash vpwide's own
   stub carries a clamp for — so the one call site that starts it is redirected
   and the world point clamped to the map. A no-op at 1x, where the engine's own
   bounds cannot produce a world point off the map, and needed on every frame
   the eye is ours, so it is not gated on the zoom.

   A pointer INSIDE the viewport needs none of this: at z > 1 the transform maps
   the whole viewport into `[L + d, R - d]`, so the world it names spans
   `[eye + d, eye + W - 1 - d]` — which is `[0, map - 1]` at either extreme of
   the range above, and inside it everywhere else. */

#define SAVE_SETTING_VA  0x004B6A50u   /* stdcall(section,name,dword), ret 0xC */
#define SITE_SAVESCROLL  0x00430FAEu   /* the one site that persists ScrollSpeed */
#define MINIMAP_RECT_VA  0x00466B70u   /* stdcall(RECT*), ret 4              */
#define SITE_MMRECT1_VA  0x0041C426u   /* call 0x466B70 — eye clamped at top */
#define SITE_MMRECT2_VA  0x0041C442u   /* call 0x466B70 — ...and at bottom   */
#define OFF_MM_X         0x142E7       /* i16 minimap rect on screen         */
#define OFF_MM_Y         0x142E9
#define OFF_MM_W         0x142EB
#define OFF_MM_H         0x142ED
#define EYECLAMP_VA      0x0041C3C0u   /* stdcall(void), ret 0: clamp + mm rect */
/* The per-frame stepper `0x41CA10` clamps the scroll target INLINE, and the
   block is `0x41CAF7`..`0x41CB43` — reached only on a frame that is following
   something, because `0x41CA8F` jumps straight to `0x41CB4A` when nothing is.
   Nothing jumps INTO it, so the whole block is ours to replace. */
#define FOLLOWCLAMP_VA   0x0041CAF7u   /* mov esi,[eax+0x14327] — the clamp's top */
#define FOLLOWCLAMP_END  0x0041CB44u   /* the instruction past the block          */
#define VA_GETTPOS       0x00484B50u   /* GetTPosition(x,y,out), stdcall, ret 0xC */
#define SITE_GETTPOS     0x00498EF9u   /* its call site inside 0x498DA0           */
#define OFF_EYEX         0x1431F
#define OFF_EYEY         0x14323
#define OFF_MAP_W        0x1422B       /* map size in world px                    */
#define OFF_MAP_H        0x1422F
#define OFF_SCRTX        0x14327       /* MapXScrollingTo — the eye eases to here */
#define OFF_SCRTY        0x1432B
#define OFF_MM_RECT      0x142CB       /* the RECT 0x466B70 fills                 */
/* The three slots the camera FOLLOW lives in — see release_follow(). */
#define OFF_FOLLOW_OBJ   0x142F7       /* followed object, position at +0x4       */
#define OFF_FOLLOW_UNIT  0x142F3       /* followed unit, position at +0x6A        */
#define OFF_FOLLOW_HOLD  0x1434B       /* u16 frame countdown on main+0x1433F     */
#define OFF_LOSTYPE      0x14281       /* bit 3: the screen fog grid is current   */
#define OFF_VIEW_W       0x37E37       /* the viewport FIELDS, which is what the  */
#define OFF_VIEW_H       0x37E3B       /* stepper's own clamp reads               */

/* `mov eax, ds:0x511DE8` — the whole first instruction, so the five stolen
   bytes end on an instruction boundary (0x41C3C5 is `push esi`). */
static const unsigned char EYE_STOLEN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };

/* `mov esi,[eax+0x14327]` — six bytes, and they are never executed: the stub
   jumps past the whole clamp block to FOLLOWCLAMP_END. */
static const unsigned char FOLLOW_STOLEN[6] = { 0x8B, 0xB0, 0x27, 0x43, 0x01, 0x00 };

typedef void (__stdcall *PFN_GETTPOS)(int x, int y, int* out);

static volatile unsigned char g_eyeWide;  /* the clamp's stub reads this one; the game thread writes it */
static int  g_eyeInstalled;               /* the detour and the guard both went in */
static int  s_eyeOff;                     /* tagpu_zoomedge.off, polled on the render thread */

static void zlog(const char* m)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
}

static int ta_ok(const char* ta)
{
    return (size_t)ta > 0x600000u && (size_t)ta < 0x7FFF0000u;
}

/* Round toward the nearest pixel, negatives included: (int) truncates toward
   zero, which would bias every point left of the centre by half a pixel and
   make a zoomed-out click land one pixel off on one side of the screen only. */
static int iround(float v)
{
    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* ---- the minimap's view rectangle ------------------------------------------

   `0x466B70(RECT*)` is a pure computation with two call sites, both in the eye
   clamp `0x41C3C0`:

       out->left   = mmX + mmW * eyeX / mapW          mm* = main+0x142E7..0x142ED
       out->top    = mmY + mmH * eyeY / mapH          map* = main+0x1422B/0x1422F
       out->right  = left + mmW * viewCellsW * 16 / mapW - 1   (main+0x1423B)
       out->bottom = top  + mmH * viewCellsH * 16 / mapH - 1   (main+0x1423F)

   Its size therefore comes from the 1x view, which is exactly what is no longer
   true. Rather than reproduce any of that, let the engine fill the rect and
   scale the RESULT about its own centre by 1/z: at 0.5x the box on the minimap
   doubles, which is what the player is actually looking at. Clamped to the
   minimap so a wildly zoomed-out view cannot draw a box off the edge of it.
   GAME THREAD: from the engine's own clamp through the two redirects, and from
   the apply below; the level it scales by is the one the last command carried. */
static void __stdcall zoom_minimap_rect(int* r)
{
    const char* ta;
    float zm = game_level();
    float cx, cy, hw, hh;
    int mx, my, mw, mh;

    ((void (__stdcall *)(int*))MINIMAP_RECT_VA)(r);
    if (!r || zm <= 0.05f || zm == 1.0f) return;

    cx = (float)(r[0] + r[2]) * 0.5f;
    cy = (float)(r[1] + r[3]) * 0.5f;
    hw = (float)(r[2] - r[0]) * 0.5f / zm;
    hh = (float)(r[3] - r[1]) * 0.5f / zm;
    r[0] = iround(cx - hw); r[2] = iround(cx + hw);
    r[1] = iround(cy - hh); r[3] = iround(cy + hh);

    ta = *(const char* const*)TA_MAINPP;
    if (!ta_ok(ta)) return;
    mx = *(const short*)(ta + OFF_MM_X); my = *(const short*)(ta + OFF_MM_Y);
    mw = *(const short*)(ta + OFF_MM_W); mh = *(const short*)(ta + OFF_MM_H);
    if (mw <= 0 || mh <= 0) return;
    if (r[0] < mx) r[0] = mx;
    if (r[1] < my) r[1] = my;
    if (r[2] > mx + mw - 1) r[2] = mx + mw - 1;
    if (r[3] > my + mh - 1) r[3] = my + mh - 1;
}

/* the level the camera range is computed at on the game thread: the engine's
   own whenever the escape hatch is thrown */
static float eye_level(void)
{
    return s_gEyeOff ? 1.0f : game_level();
}

/* The camera range at `z`: the engine's own bounds, widened by d. Returns 0 —
   and leaves the outputs alone — when the engine state is not sane enough to
   compute one. GAME THREAD. The render thread computes the same range from
   the packet (range_pk below): the packet's `vp` IS tagpu_vpwide_true_rect's
   answer and its `map_pxw/h` the same two fields, so the two agree by
   construction and a step the render thread pre-clamps is accepted here. */
static int zoom_eye_range(const char* ta, float z,
                          int* loX, int* hiX, int* loY, int* hiY)
{
    int L, T, W, H, mapW, mapH, dx = 0, dy = 0;

    if (!ta_ok(ta)) return 0;
    /* the TRUE viewport, never the field: vpwide owns that one at zoom < 1 */
    tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
    if (W <= 0 || H <= 0) return 0;
    mapW = *(const int*)(ta + OFF_MAP_W);
    mapH = *(const int*)(ta + OFF_MAP_H);
    if (mapW <= 0 || mapH <= 0) return 0;

    if (z > 1.0f) {
        dx = iround((float)W * 0.5f * (1.0f - 1.0f / z));
        dy = iround((float)H * 0.5f * (1.0f - 1.0f / z));
    }
    /* HUD SCALE NEEDS NOTHING HERE (gui-renderer.md 22.6). It briefly did --
       the range was widened by what the magnified HUD covered -- but the
       viewport the engine clamps about IS the visible window now, so the
       engine's own range is already the right one and a second correction
       would be a double one. */
    *loX = -dx; *hiX = mapW - W + dx;
    *loY = -dy; *hiY = mapH - H + dy;
    /* A map smaller than the viewport inverts the ENGINE's range too — it then
       alternates between 0 and a negative bound on every call. Ours holds
       still, which is the most that can be said for either. */
    if (*hiX < *loX) *hiX = *loX;
    if (*hiY < *loY) *hiY = *loY;
    return 1;
}

/* The same range from the packet's copy of the same fields — the render
   thread's, for the cursor anchor's pre-clamp and the predicted eye. Must
   stay the same arithmetic as zoom_eye_range. */
static int range_pk(const TAGPU_PACKET* p, float z,
                    int* loX, int* hiX, int* loY, int* hiY)
{
    int W = p->vp[2], H = p->vp[3], mapW = p->map_pxw, mapH = p->map_pxh, dx = 0, dy = 0;
    if (W <= 0 || H <= 0 || mapW <= 0 || mapH <= 0) return 0;
    if (z > 1.0f) {
        dx = iround((float)W * 0.5f * (1.0f - 1.0f / z));
        dy = iround((float)H * 0.5f * (1.0f - 1.0f / z));
    }
    *loX = -dx; *hiX = mapW - W + dx;
    *loY = -dy; *hiY = mapH - H + dy;
    if (*hiX < *loX) *hiX = *loX;
    if (*hiY < *loY) *hiY = *loY;
    return 1;
}

/* Clamp one x/y pair into the range where it is stored. 1 when it moved. */
static int clamp_pair(int* px, int* py, int loX, int hiX, int loY, int hiY)
{
    int moved = 0;

    if      (*px < loX) { *px = loX; moved = 1; }
    else if (*px > hiX) { *px = hiX; moved = 1; }
    if      (*py < loY) { *py = loY; moved = 1; }
    else if (*py > hiY) { *py = hiY; moved = 1; }
    return moved;
}

/* The replacement clamp. GAME THREAD, and only while g_eyeWide is set. `arg` is
   the first stack slot of a function that takes no arguments — ignored.

   THE SCROLL TARGET IS DELIBERATELY NOT TOUCHED HERE, unlike in the apply.
   `main+0x14327`/`+0x1432B` is where the camera is heading, and three of this
   function's callers are inside the per-frame stepper `0x41CA10`, which eases
   the eye halfway toward it and calls us afterwards: writing the target there
   would make it the eye every frame and the camera would never arrive. Every
   caller that MEANT to move the camera copies the clamped eye into the target
   itself, right after we return (`0x41C5A4`, `0x41CDE6`, `0x41D05E`), so the
   pair stays consistent without our help. */
static void __cdecl zoom_eye_clamp(void* arg)
{
    char* ta = *(char**)TA_MAINPP;
    int loX, hiX, loY, hiY;

    (void)arg;
    if (!ta_ok(ta)) return;
    if (zoom_eye_range(ta, eye_level(), &loX, &hiX, &loY, &hiY))
        clamp_pair((int*)(ta + OFF_EYEX), (int*)(ta + OFF_EYEY),
                   loX, hiX, loY, hiY);
    /* the engine's own last act, and the only place this rect is recomputed */
    zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
}

/* THE FOLLOW'S OWN CLAMP, MADE ZOOM-AWARE — the third of the three inline ones
   this module's range never reached, and the first to be closed.

   The per-frame stepper `0x41CA10` recomputes the scroll target from whatever
   is being followed (`want = unit - view/2`, `0x41CA95`..`0x41CAD2`) and then
   clamps it INLINE to `[0, map - view]` without going anywhere near
   `0x41C3C0`. That bound is about the UNZOOMED viewport, so at zoom z the
   camera's centre cannot come closer than `W/2` to a map edge while the window
   the player is looking at is only `W/z` wide. Ctrl+C on a commander inside
   that band therefore takes the follow, aims the camera, and stops short --
   with the unit off screen entirely once `W/2 - W/(2z)` exceeds the distance.
   Reported from play 2026-09-12: "when zoomed in to the maximum possible zoom
   level, ctrl-c does not manage to focus on the commander". Measured there, at
   1920x1080 with the HUD at 225% (view 1632x936) on a 4064x3968 map, z = 8,
   commander at world (3808,3600): the follow asked for (2992,3132), the
   engine's clamp cut it to (2432,3032), and the visible window was world
   x [3145,3349] y [3440,3558] -- 459 px short.

   Correcting it afterwards is not open to us: the stepper eases the eye toward
   the target and calls `0x41C3C0` only after, so a target widened from our eye
   clamp would be overwritten before it was ever used. So the block is replaced
   instead of chased, which is also why it is the WHOLE block -- the fog bit it
   clears at `0x41CB3B` is part of it.

   AT z <= 1 THIS IS THE ENGINE'S OWN ARITHMETIC, to the byte, read from the
   same two viewport FIELDS the block read (`vpwide` owns those below 1x and the
   stepper's `want` is computed from them, so ours must clamp against them too;
   above 1x the field and the true rect are the same rect). The widened range is
   consulted only where it differs, which is z > 1 and nowhere else. */
static void __cdecl zoom_follow_clamp(void)
{
    char* ta = *(char**)TA_MAINPP;
    int loX, hiX, loY, hiY;
    float z;

    if (!ta_ok(ta)) return;

    loX = loY = 0;
    hiX = *(const int*)(ta + OFF_MAP_W) - *(const int*)(ta + OFF_VIEW_W);
    hiY = *(const int*)(ta + OFF_MAP_H) - *(const int*)(ta + OFF_VIEW_H);

    z = eye_level();
    if (z > 1.0f) {
        int zloX, zhiX, zloY, zhiY;
        /* on a frame whose engine state is not sane enough for a range, the
           engine's own bound above stands rather than nothing being clamped */
        if (zoom_eye_range(ta, z, &zloX, &zhiX, &zloY, &zhiY)) {
            loX = zloX; hiX = zhiX; loY = zloY; hiY = zhiY;
        }
    }
    /* The engine does not guard this, because its own `lo` is a literal 0 and
       its `else if` leaves a target below an inverted `hi` at 0 anyway; ours
       has a computed `lo`, so the pair is ordered before it is used. */
    if (hiX < loX) hiX = loX;
    if (hiY < loY) hiY = loY;

    clamp_pair((int*)(ta + OFF_SCRTX), (int*)(ta + OFF_SCRTY), loX, hiX, loY, hiY);

    /* `0x41CB3B`, the block's last act: bit 3 of main+0x14281 is the screen fog
       grid's is-current flag and the target it was built for has just moved.
       This is the GAME thread -- the only one that may touch that word at all
       (exe map, "who may clear main+0x14281 bit 3"). */
    *(unsigned short*)(ta + OFF_LOSTYPE) &= (unsigned short)~8u;
}

/* pushfd ; pushad ; call zoom_follow_clamp ; popad ; popfd ; jmp past the block.
   The flags are saved as well as the registers because this lands in the MIDDLE
   of a function rather than on a prologue. */
static unsigned char* build_follow_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x9C;                                                /* pushfd  */
    *p++ = 0x60;                                                /* pushad  */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)&zoom_follow_clamp); p += 4;
    *p++ = 0x61;                                                /* popad   */
    *p++ = 0x9D;                                                /* popfd   */
    *p++ = 0xE9; tagpu_detour_rel(p, FOLLOWCLAMP_END); p += 4;  /* jmp     */
    return s;
}

/* ---- releasing the camera follow -------------------------------------------

   A CAMERA THE PLAYER IS DRIVING IS NOT FOLLOWING ANYTHING, AND THAT IS THE
   ENGINE'S OWN RULE, not a preference of ours. The per-frame stepper `0x41CA10`
   recomputes the scroll target from whatever the camera is following, EVERY
   frame, and clamps it inline — so a delta we add to the eye is eased straight
   back out and the zoom reads as though it were pinned to the unit. The engine
   has the same problem with its own edge and hotkey scroll and solves it by
   releasing the follow: `0x41D091..0x41D0AA`, the tail of the scroll poll, runs
   exactly the three stores below — and only on a frame where the eye actually
   changed (`0x41D035` skips the whole tail when it did not). Those three stores
   are the body of `0x41C390`, the engine's own release, which `0x4174FD` (the
   camera-track toggle) and `0x48D709` (centre-on-unit) call verbatim.

   THREE SLOTS, because the stepper takes the first of three that is set
   [MEASURED 2026-09-10, objdump of the pristine build, `0x41CA1A..0x41CA8D`]:

     main+0x1434B  u16  a frame COUNTDOWN; while non-zero the camera follows the
                        remembered position at main+0x1433F. `0x499E50` fills
                        both when a followed object is destroyed.
     main+0x142F7  ptr  the followed object; its position is at +0x4.
     main+0x142F3  ptr  the followed unit; position +0x6A, and it is dropped by
                        the stepper itself once `[unit+0x110] & 0x10000000`
                        goes away. `Ctrl+C` (0x41C310) and the next/previous
                        unit keys (0x41C2E0) are what set it.

   THE STORES ARE THE GAME THREAD'S, AND THAT IS THE WHOLE SAFETY ARGUMENT.
   `main` IS NOT ALIGNED, and not by accident: the allocator at `0x41D920`
   takes `pad = (GetTickCount() % 1000) * 7`, `malloc(0x3924D + pad)` and
   publishes `base + pad` (`0x41D9D5`), so the struct's alignment is drawn
   afresh at every launch and then fixed for the session. `main+0x142F7` is
   4-aligned in **25%** of launches and straddles a 64-byte cache line in
   **4.7%** of them [MEASURED 2026-09-10, over all 1000 tick residues], and an
   x86 access that crosses a line is not atomic. A cross-thread store of ZERO
   would therefore be torn in about one launch in twenty — and torn into a
   slot the stepper DEREFERENCES, at `0x41CA58` and `0x41CA95`: half a pointer
   passes `cmp eax, ebp` and is then read through. Since landing 2 the eye and
   the scroll target are written from this thread too, for the same reason
   plus one more: a torn COORDINATE was bounded by `clamp_pair()`, but a lost
   update discarded a camera move rather than delaying it.

   Called from the apply, on the draw that applies a NEW delta from a gesture
   that asked for the camera — the same ordering the engine's own scroll poll
   has: released, then the eye and the target stepped, in one thread, with no
   stepper between the two. The stepper is called from the frame callback
   before the draw call (0x495599 inside 0x495490, from 0x49680C/0x49693E;
   it is skipped when the sim is paused or an in-game GUI screen is up) and
   does not run again until the next frame callback; nothing inside
   DrawGameScreen stores the eye. A gesture that moves the eye by NOTHING —
   the pointer on the viewport centre — releases nothing, so the A/B control
   holds exactly. */
static void release_follow(char* ta)
{
    int*   obj  = (int*)  (ta + OFF_FOLLOW_OBJ);
    int*   unit = (int*)  (ta + OFF_FOLLOW_UNIT);
    short* hold = (short*)(ta + OFF_FOLLOW_HOLD);

    if (!*obj && !*unit && !*hold) return;   /* nothing follows: the usual case */
    *hold = 0;
    *unit = 0;
    *obj  = 0;
    /* One line per follow actually taken, throttled like every other per-frame
       line in this module: a site that re-establishes a follow each tick
       (0x499E50 refills the countdown when a followed object dies) would
       otherwise open the log once per draw on the game thread. */
    {
        static DWORD tick;                   /* game thread only */
        DWORD now = GetTickCount();
        if (now - tick > 1000) {
            tick = now;
            zlog("zoom: cursor anchor took the camera - the unit follow is released");
        }
    }
}

/* ---- the apply: the game thread's half of every command ---------------------

   GAME THREAD, at the top of every in-play draw (tagpu_packet_pub.c's
   `before`), with the latest command record — NULL until the render thread's
   first post. This runs after whichever of the frame callback's own camera
   writers ran this frame (the stepper and the scroll poll both precede the
   draw call at 0x4969CD, and both can be skipped: the stepper when paused,
   both under an in-game GUI screen) and before the draw's first read of the
   eye at 0x468DD9 — no store to the eye exists inside DrawGameScreen. The
   order below is the order the engine's own camera writers keep: release the
   follow, move the eye and the target together, clamp, then the minimap box
   and the fog flag. Nothing here waits, and nothing here can be torn: every
   word is written by this thread alone. */
static unsigned s_appliedSeq;              /* game thread only */
static int      s_appliedDx, s_appliedDy;
static unsigned s_epoch;                   /* bumped at every level end (game thread) */

void tagpu_zoom_apply(char* ta, const TAGPU_CMD* c)
{
    int* eye = (int*)(ta + OFF_EYEX);
    int* scr = (int*)(ta + OFF_SCRTX);
    int  loX = 0, hiX = 0, loY = 0, hiY = 0, haveRange, moved = 0, clampIt = 0;
    float zr;

    /* THE LEVEL FIRST: every game-thread reader in this file — the clamp's
       flag, the minimap rect's scale, the scroll rate — uses the level the
       last record carried, never the render thread's own float. A frame that
       drew nothing zoomed posts live = 0, which is 1.0 here: the engine's own
       range, rect and rate come back at the next draw. */
    s_gLevel  = c ? c->zoom : 1.0f;
    s_gLive   = c ? (int)c->live : 0;
    s_gEyeOff = c ? (int)c->eyeoff : 0;
    zr = g_eyeInstalled ? eye_level() : 1.0f;
    g_eyeWide = (unsigned char)(g_eyeInstalled && zr > 1.0f);

    haveRange = zoom_eye_range(ta, zr, &loX, &hiX, &loY, &hiY);

    /* A NEW RECORD: its delta is consumed exactly once — the difference
       between its cumulative sum and what was applied so far — and the
       follow is released on the same draw when the gesture asked. The
       render thread pre-clamped the step against the same range from the
       packet's copy of the same fields, so the clamp below fires only when
       the engine moved the eye between the two, and then the packet's eye
       reconciles the prediction. */
    /* A record of an OLDER epoch — posted before the render thread saw the
       level end — carries the old level's sum and applies nothing; the render
       thread's first record after seeing the new epoch starts from zero, as
       the applied sum did at the level end (landing review). */
    if (c && c->cmd_seq != s_appliedSeq && c->epoch == s_epoch) {
        int dx = c->cum_dx - s_appliedDx, dy = c->cum_dy - s_appliedDy;
        if (dx || dy) {
            if (c->drop_follow) release_follow(ta);
            eye[0] += dx; eye[1] += dy;
            /* the target moves with the eye, always: the two disagreeing is
               what the per-frame stepper reads as "a camera move is in
               flight", and it would drag the eye back and rebuild the fog
               grid every frame for as long as the disagreement lasted (G13g) */
            scr[0] += dx; scr[1] += dy;
            moved = 1; clampIt = 1;
        }
        s_appliedSeq = c->cmd_seq; s_appliedDx = c->cum_dx; s_appliedDy = c->cum_dy;
    }

    /* THE HOLD, a level: the camera is where tagpu_eye.txt says, every draw,
       clamped into the camera's range — not into [0, map - view], which at
       zoom > 1 would pull a scripted camera back off every map edge (the
       same bug the engine's own clamp had). Written only when it differs, so
       the invalidation below is paid only when the camera actually moved. */
    if (c && c->hold_on && haveRange) {
        int x = c->hold_x, y = c->hold_y;
        clamp_pair(&x, &y, loX, hiX, loY, hiY);
        if (eye[0] != x || eye[1] != y) { eye[0] = x; eye[1] = y; moved = 1; }
        if (scr[0] != x || scr[1] != y) { scr[0] = x; scr[1] = y; }
    }

    /* THE EYE HAS TO COME HOME WHEN THE ZOOM DOES. The engine's clamp runs
       only when IT moves the camera, so an eye parked at -d at 4x would sit
       there — showing the void past the map edge — from the moment the player
       wheels back out until the next time they scroll. So the range in force
       is re-applied to the eye AND the scroll target on every draw a zoomed
       world is live; it writes only when one of them is actually outside it,
       which is during a zoom-out at a map edge and at no other time. The
       target is clamped rather than assigned the eye, which keeps a camera
       move that is genuinely in flight: such a target is inside [0, map - W]
       already, so it is inside ours too and is not touched. */
    if (g_eyeInstalled && s_gLive) clampIt = 1;
    if (clampIt && haveRange) {
        moved |= clamp_pair(eye, eye + 1, loX, hiX, loY, hiY);
        moved |= clamp_pair(scr, scr + 1, loX, hiX, loY, hiY);
    }

    /* A CAMERA WE MOVED OWES THE ENGINE THE SAME TWO THINGS ITS OWN WRITERS DO
       (exe note: eleven sites, each one immediately before its 0x41C3C0 call):
       the minimap's view box recomputed — `0x41C3C0` is the only place the
       engine ever fills it, so the box would otherwise stay where it was until
       the next engine camera move — and the screen fog grid invalidated,
       because it is view-anchored and rebuilt lazily off bit 3 of
       main+0x14281. That bit is cleared HERE, on the game thread, exactly as
       `0x41CB6B`, `0x41CB3B`, `0x41C567` and `0x41CE0D` clear it, which is the
       only thread that may: `0x484904` sets it with an UNLOCKED
       read-modify-write, so a clear from any other thread could be swallowed
       whole and leave the fog built for where the camera used to be. The
       engine's own fog draw (or terrown's replica of its lazy rebuild) then
       rebuilds it inside THIS draw, for the commanded eye. */
    if (moved) {
        zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
        *(unsigned short*)(ta + OFF_LOSTYPE) &= (unsigned short)~8u;
    }

    /* and the scroll rate, every draw: base/z, or the base when nothing is
       zoomed — before the next frame's scroll poll reads it */
    apply_scroll_rate(ta);
}

void tagpu_zoom_level_end(char* ta)
{
    s_gLevel = 1.0f; s_gLive = 0; s_gEyeOff = 0;
    g_eyeWide = 0;
    /* a new epoch: nothing owed to the old level survives into the next */
    s_epoch++;
    s_appliedSeq = 0; s_appliedDx = 0; s_appliedDy = 0;
    if (ta_ok(ta)) apply_scroll_rate(ta);      /* the player's own value, for the options screen */
}

void tagpu_zoom_applied(unsigned* seq, int* cum_dx, int* cum_dy, float* level, unsigned* epoch)
{
    *seq = s_appliedSeq; *cum_dx = s_appliedDx; *cum_dy = s_appliedDy;
    *level = game_level(); *epoch = s_epoch;
}

/* ---- the end of the render frame: the command record ----------------------- */

/* The escape hatch, polled rather than cached at attach so it can be flipped on
   a running instance. Thrown, the level the range is computed at reads as 1.0,
   which walks the eye back onto the 1x range rather than merely freezing it
   where it stood. The poll is the RENDER thread's, once a frame and throttled
   on top of that; the result rides the command record to the game thread. */
static void eye_poll_off(void)
{
    static DWORD tick;                       /* render thread only */
    DWORD now = GetTickCount();
    if (now - tick > 250) {
        tick = now;
        s_eyeOff = (GetFileAttributesA("tagpu_zoomedge.off") != INVALID_FILE_ATTRIBUTES);
    }
}

/* the cursor anchor's state, render thread only (below) */
static int s_cumX, s_cumY;                 /* the cumulative eye delta posted, world px */
static int s_ackX, s_ackY;                 /* ...and what the last packet acknowledged  */
static unsigned s_epochSeen;               /* the cmd_epoch of the last packet seen     */

void tagpu_zoom_frame_end(void)
{
    TAGPU_CMD rec;
    if (!s_fresh) s_live = 0;
    s_fresh = 0;
    eye_poll_off();
    /* THE RECORD: this frame's level and whether a zoomed world is on screen
       (the game thread derives the camera range, the addressable rect and the
       scroll rate from the pair), the anchor's cumulative delta, whether any
       of it is still unacknowledged (the follow release rides that, so a
       record the game thread never took loses nothing: the next one carries
       the same request), and the camera hold. Posted on EVERY path out of the
       overlay frame, so a frame that drew nothing zoomed hands the engine its
       own range, rect and rate back at the next in-play draw. */
    memset(&rec, 0, sizeof rec);
    rec.zoom        = s_zoom;
    rec.live        = s_live ? 1u : 0u;
    rec.eyeoff      = s_eyeOff ? 1u : 0u;
    rec.epoch       = s_epochSeen;
    rec.cum_dx      = s_cumX;
    rec.cum_dy      = s_cumY;
    rec.drop_follow = (s_cumX != s_ackX || s_cumY != s_ackY) ? 1u : 0u;
    tagpu_input_cmd(&rec);
    tagpu_cmd_post(&rec);
}

float tagpu_zoom_level(void)
{
    return s_live ? s_zoom : 1.0f;
}

float tagpu_zoom_min(void)
{
    return ZOOM_MIN;
}

/* Snapshot the published view. Returns 0 when there is nothing to do —
   the caller then leaves the point alone. */
static int view(float* z, float* cx, float* cy, int* L, int* T, int* W, int* H)
{
    float zz = s_live ? s_zoom : 1.0f;
    if (zz <= 0.05f || zz == 1.0f) return 0;
    *L = (int)s_vpL; *T = (int)s_vpT; *W = (int)s_vw; *H = (int)s_vh;
    if (*W <= 0 || *H <= 0) return 0;
    *z = zz;
    *cx = (float)*L + (float)*W * 0.5f;
    *cy = (float)*T + (float)*H * 0.5f;
    return 1;
}

static int in_viewport(int x, int y, int L, int T, int W, int H)
{
    return x >= L && y >= T && x < L + W && y < T + H;
}

/* The whole transform. `ring` (may be NULL) reports that `u` fell outside the
   rect the engine can NAME — the display-only ring, where it has no screen
   position for the world under the pointer. tagpu_vpwide closes that ring while
   it is armed, so this is the answer for zoom < 1 without it.

   There the transform gives up and returns the pointer UNCHANGED rather than
   clamping it to the viewport edge. Clamping still names a world point the
   player did not click, and it names one that MOVES with the pointer along the
   edge, so a hover reads as though the cursor were sliding along the viewport
   border. Identity at least leaves the 1x world point under the pointer, which
   is what every screen-space behaviour already expects; and the one thing that
   would then be wrong, a CLICK on that 1x point, is what
   tagpu_zoom_drop_mouse() throws away.

   Nothing about the CURSOR is decided here any more. The engine is told the
   truth by fake_GetCursorPos and draws its sprite under the pointer; this
   transform reaches it only through a button message and through the
   mouse->world repair in tagpu_vpwide.c. */
static int to_engine(int* x, int* y, int* ring)
{
    float z, cx, cy; int L, T, W, H, ux, uy;
    if (ring) *ring = 0;
    if (!x || !y || !view(&z, &cx, &cy, &L, &T, &W, &H)) return 0;
    if (!in_viewport(*x, *y, L, T, W, H)) return 0;   /* screen-space: 1:1 */
    ux = iround(((float)*x - cx) / z + cx);
    uy = iround(((float)*y - cy) / z + cy);
    /* The ring is whatever the engine cannot NAME, so it is measured against
       the addressable rect — which tagpu_vpwide has widened to exactly this
       transform's range when it is armed, and which is the true viewport when
       it is not. The gate above stays on the TRUE rect: it decides "did the
       player click on the world or on the screen-space UI", and that boundary
       does not move. */
    {
        int aL, aT, aW, aH;
        if (tagpu_vpwide_addressable(&aL, &aT, &aW, &aH)) {
            L = aL; T = aT; W = aW; H = aH;
        }
    }
    if (!in_viewport(ux, uy, L, T, W, H)) {
        if (ring) *ring = 1;
        return 0;
    }
    *x = ux; *y = uy;
    return 1;
}

int tagpu_zoom_to_engine(int* x, int* y)
{
    return to_engine(x, y, NULL);
}

/* WHICH MESSAGES MAY CARRY `u` INTO THE ENGINE — the BUTTON events, and only
   them. A button message is pushed on the engine's own event ring
   (`0x4B5EB2` / `0x4B5EFE` -> `0x4C2E30`), which is a queue: the position it
   carries is the position the press was MADE at, and it is the one thing in
   this stack that a later pointer sample cannot reconstruct. So the transform
   is applied there, at the door, and the ring keeps it.

   A MOVE MUST NOT BE REWRITTEN, and that is the whole reason the sprite used to
   jump. `0x4B5E51` does not queue: it copies its record into `[obj+0x196]`
   (`0x4C2360`), which is both the dispatch's fallback record AND the position
   the engine draws its cursor from on the path that does not poll — `0x4C67C0`,
   called twice out of the surface present machinery.
   A rewritten move therefore puts `u` where the sprite is read from, and
   whichever of the message and the next GetCursorPos poll ran last decides
   where the cursor appears — measured at 1920x1080 / 0.25x: the sprite tracked
   `u` across the frame at 4x the pointer's speed and off the left edge, exactly
   the artefact this was supposed to remove. The move's own position is not
   needed as `u` by anything: the world point is recomputed from the pointer in
   vpw_mouse_world() (tagpu_vpwide.c), which is where it is actually used.

   WM_MOUSEWHEEL is not in the list either — the engine's jump table covers only
   0x200..0x206 and drops the wheel to DefWindowProcA — and tagpu_zoom_wheel()
   is handed the point separately. */
static int carries_point(UINT msg)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:  case WM_LBUTTONUP:    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:  case WM_RBUTTONUP:    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:  case WM_MBUTTONUP:    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:  case WM_XBUTTONUP:    case WM_XBUTTONDBLCLK:
        return 1;
    }
    return 0;
}

/* The subset of carries_point() that ACTS on the world rather than just moving
   the pointer. A move or a hit-test in the ring is harmless; a button press
   there is not. Returns the button's slot (so a press and its release share
   one), -1 for every other message, and sets `down`. */
static int button_slot(UINT msg, int* down)
{
    *down = 1;
    switch (msg)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return 3;
    }
    *down = 0;
    switch (msg)
    {
    case WM_LBUTTONUP: return 0;
    case WM_RBUTTONUP: return 1;
    case WM_MBUTTONUP: return 2;
    case WM_XBUTTONUP: return 3;
    }
    return -1;
}

/* Which buttons went down in the ring and were therefore never delivered. */
static unsigned char s_dropped[4];

int tagpu_zoom_drop_mouse(UINT msg, LPARAM lparam)
{
    int x, y, ring = 0, down = 0, slot = button_slot(msg, &down);

    if (slot < 0) return 0;

    /* A PRESS AND ITS RELEASE ARE DROPPED TOGETHER OR NOT AT ALL. Deciding a
       release on its own position is wrong in both directions: a drag begun on
       an addressable point and released in the ring would have its release
       swallowed and leave the engine holding the button for the rest of the
       game, and a release delivered for a press that was dropped is a button-up
       the engine never saw go down. So the press decides and the release
       follows it. */
    if (!down) {
        int drop = s_dropped[slot];
        s_dropped[slot] = 0;
        return drop;
    }

    x = (int)(short)LOWORD(lparam);
    y = (int)(short)HIWORD(lparam);
    /* a point the render-options UI owns is never in the ring: it is screen
       furniture, not world, and dropping its press would swallow the click */
    if (tagpu_menu_owns_point(x, y)) { s_dropped[slot] = 0; return 0; }
    /* the return value is "was it transformed", which is 0 in the ring — `ring`
       is the flag to read, and it is the only one that matters here */
    to_engine(&x, &y, &ring);
    s_dropped[slot] = (unsigned char)ring;
    return ring;
}

LPARAM tagpu_zoom_mouse_lparam(UINT msg, LPARAM lparam)
{
    if (!carries_point(msg)) return lparam;
    int x = (int)(short)LOWORD(lparam);
    int y = (int)(short)HIWORD(lparam);
    /* THE RENDER-OPTIONS PANEL IS OVER THE WORLD, so the geometric gate below
       would treat every click on it as a world click and unzoom it -- at 3.1x
       a row click landed hundreds of pixels away and no row worked at all
       (found in play 2026-09-09). The engine hit-tests a GUI screen in screen
       space, so a point the screen owns must reach it untouched. This module
       cannot see GUI screens; tagpu_menu answers for its own. */
    if (tagpu_menu_owns_point(x, y)) return lparam;
    if (!tagpu_zoom_to_engine(&x, &y)) return lparam;
    return MAKELPARAM((short)x, (short)y);
}

/* ---- zoom to the cursor ----------------------------------------------------

   The wheel holds the world point under the POINTER still, instead of the one
   at the centre of the screen. The transform cannot do it: it is a similarity
   about the viewport centre and nothing in it is free. What is free is the
   engine's eye, because the world on screen is

       W(s) = eye + vw/2 + (s - c) / z

   so holding W(s) fixed across a change in z is one subtraction:

       d = (a - c) * (1/z_prev - 1/z_now)          a = where the notch was aimed

   applied to the eye. THE DELTA IS EXACT, not an approximation of one: it is
   the difference of two exact solutions, so the constants (the anchored world
   point, vw/2) cancel and never appear.

   WHY A DELTA AND NOT A SOLVED POSITION. The absolute form — keep the anchored
   world point W* and set the eye from it every frame — is algebraically the
   same thing, but it ASSERTS the eye on every frame of the ease and so
   overwrites any other camera source for as long as a gesture lasts. The delta
   composes with them instead: an edge scroll, an arrow key or a camera move
   already in flight is preserved, because we add to whatever the eye is rather
   than declaring what it should be. And when z is not moving the delta is
   exactly zero, so at steady state — which is almost every frame — this
   function posts NOTHING new and there is no interference to reason about.

   HOW IT REACHES THE EYE (landing 2). The render thread never writes the eye.
   The step is added to a CUMULATIVE sum that rides the command record; the
   game thread applies the difference from what it applied last, at the top of
   its next in-play draw, releasing the follow first when the gesture asked —
   consumed exactly once, accumulated if the game thread is slow, and applied
   after whichever of the engine's own camera writers ran that frame and
   BEFORE the draw reads the eye (nothing inside DrawGameScreen stores it), so
   the frame, its fog rebuild and its minimap box all see the commanded camera. Meanwhile this frame is drawn from the PREDICTED eye: the
   packet's eye plus every delta not yet acknowledged (predict below), so the
   picture moves on the frame of the notch and the next packet, carrying the
   same delta applied, replaces the prediction with the truth — no wobble. The
   step is pre-clamped here against the same camera range the game thread
   applies, computed from the packet's copy of the same two fields, so what is
   posted is what will be accepted; the one case the game thread refuses more —
   the engine scrolled the eye to the edge between the two — is reconciled by
   the packet like any other engine camera move.

   THREE PROPERTIES FALL OUT OF THE DELTA FORM, and the tests lean on all three.
   With the pointer at the viewport centre `a - c` is zero, so the eye never
   moves and the behaviour is what it was before this existed, bit for bit —
   that is the A/B control, and it is why this needs no lever. The steps
   TELESCOPE, so the total displacement over a gesture is
   `(a - c)(1/z_start - 1/z_end)` however many frames the ease took and whatever
   the frame timing was — an exact oracle on `main+0x1431F` with `tacli peek`.
   And in-then-out with a still pointer returns the eye exactly where it was.

   THE ROUNDING RESIDUAL IS CARRIED; A REFUSED DELTA IS NOT. The eye is an
   integer in world px, so `d` is split into an integer part and a remainder
   that is kept for the next frame — that is what makes the telescoping exact
   rather than drifting a pixel per frame. But a delta the camera RANGE refused
   at a map edge is thrown away, and the residual with it: banking it would
   grow without bound (a hard zoom-in at a corner banks hundreds of world px)
   and then spend itself as a sideways lurch on the first notch of the way out.
   The two leftovers are different things and only one of them is a debt.

   WHAT IT COSTS AT THE EDGE OF THE MAP: the point cannot be held, because the
   eye cannot go where holding it would need. The anchor drifts toward the
   centre, which is what every map application does, and there is no fix that
   is not "refuse to zoom".

   THE RESIDUAL, STATED: the eye is an integer in world px, so at zoom z one
   unit of it is z screen px, and the anchor can sit up to z/2 px from the
   pointer while a gesture is in flight — 0.5 px at 1x, 4 px at 8x. Holding it
   exactly would mean giving the transform an off-centre scale centre, which
   every rect derived from the viewport (vpwide's addressable rect, fogwide's
   window, the ring test) currently assumes away.

   THE FOG. The game thread's apply invalidates the screen fog grid on the
   draw it steps the eye (bit 3 of main+0x14281, cleared on the thread that
   owns it), so the engine's grid is rebuilt for the commanded eye inside that
   same draw whether the fog draw is the engine's or terrown's replica. What
   remains is the frame drawn from the PREDICTED eye before that packet
   arrives: its grid, read from engine memory, spans the packet's eye and the
   engine grid's slack collapses to 32 px at z -> 1 (exe note), so a frame
   whose predicted eye is ahead of the packet's takes the WIDE grid
   (tagpu_zoom_unacked, tagpu_native.c) — built every tick from the live eye
   with a margin that covers any one anchored step. The old gate on terrown
   owning the fog draw is gone with the old handshake: the request is no
   longer ours to route, the invalidation is the engine's own mechanism.

   A FOLLOWED CAMERA IS RELEASED RATHER THAN FOUGHT. The stepper recomputes the
   scroll target from the followed unit every frame (`0x41CAF7`) and clamps it
   inline, so a delta added to the eye is eased straight back out: the zoom
   would read as pinned to the unit. It is not what following means — the
   engine's own edge scroll releases the follow the moment it moves the eye,
   and so does the draw that applies a step here (release_follow, on the game
   thread, in the same apply). A gesture that moves the eye by NOTHING — the
   pointer on the viewport centre — releases nothing, so the A/B control still
   holds exactly.

   AND WHAT IS STILL NOT COVERED. `0x41C4C0` (the smooth SetCamera) and
   `0x41C7F7` (the smooth centre-on) also compute the scroll target and clamp it
   INLINE against `[0, map - W]` without going through our clamp, so a target we
   stepped can be recomputed there without our delta and the stepper eases the
   eye back. Neither is a standing state the way a follow is — each is one
   camera move in flight, and the zoom composes with the next one — so nothing
   fights and nothing churns. */

/* The level the eye was last stepped at, and the sub-world-pixel carry. Render
   thread only: the same thread that owns the level itself. */
static float s_zStep = 1.0f;
static float s_residX, s_residY;

/* THE CARRY'S INVARIANT, kept at every exit that does not step the eye. A
   frame that moves the eye leaves at most half a world pixel behind, by
   construction, so at rest `|resid| <= 0.5` and anything above it is
   displacement that was owed and never taken — which nothing else would ever
   spend (every later frame returns at `zNow == s_zStep`) until the next notch
   anywhere on the map discharged it in one frame as a silent camera jump
   [landing review, 2026-09-10]. */
static void drop_claim(void)
{
    if (s_residX > 0.5f || s_residX < -0.5f) s_residX = 0.0f;
    if (s_residY > 0.5f || s_residY < -0.5f) s_residY = 0.0f;
}

/* 1 while the eye may be stepped: a zoomed world is actually on screen, and
   nothing else is driving the camera. Throttled log lines, because a control
   that silently does nothing is the one failure this module's other gripes
   exist to prevent. */
static int anchor_allowed(void)
{
    static DWORD tick;                       /* render thread only */
    const char* why = 0;

    if (!s_live)                     return 0;   /* menus: nothing to say */
    if (tagpu_input_eye_held())      why = "zoom: cursor anchor off - tagpu_eye.txt holds the camera";
    if (!why) return 1;
    {
        DWORD now = GetTickCount();
        if (now - tick > 1000) { tick = now; zlog(why); }
    }
    return 0;
}

/* the level the render thread's range is computed at: the same rule the game
   thread applies (eye_level with the widened clamp installed, else 1.0) */
static float pred_level(void)
{
    return g_eyeInstalled ? (s_eyeOff ? 1.0f : tagpu_zoom_level()) : 1.0f;
}

/* Render thread, once a frame, from read_lever() and BEFORE any pass reads the
   eye. `fromWheel` is false while the file lever is in force: it changes z with
   no gesture behind it and every zoom fixture drives it, so it must not move
   the camera. */
static void anchor_step(float zNow, int fromWheel, const TAGPU_PACKET* pk)
{
    LONG  a;
    float ax, ay, cx, cy, k;
    int   nx, ny, loX, hiX, loY, hiY, px, py, qx, qy, moved;

    /* Whatever happens below, the level the NEXT step measures from is this
       one. A frame that declined to move the eye must not leave its change
       banked for a later frame to apply in one jump. */
    if (zNow == s_zStep) {                       /* the common case: no gesture */
        drop_claim();                            /* no gesture, no debt */
        return;
    }
    {
        float zWas = s_zStep;
        s_zStep = zNow;

        if (!fromWheel || !s_anchorSet || !anchor_allowed() ||
            zWas <= 0.05f || zNow <= 0.05f) { s_residX = s_residY = 0.0f; return; }
        if ((int)s_vw <= 0 || (int)s_vh <= 0) { s_residX = s_residY = 0.0f; return; }
        /* no in-game packet: no eye to step from, and no world being drawn */
        if (!pk || !pk->in_game) { s_residX = s_residY = 0.0f; return; }

        a  = s_anchor;
        ax = (float)(int)(short)(a & 0xFFFF);
        ay = (float)(int)(short)((a >> 16) & 0xFFFF);
        cx = (float)(int)s_vpL + (float)(int)s_vw * 0.5f;
        cy = (float)(int)s_vpT + (float)(int)s_vh * 0.5f;

        k  = 1.0f / zWas - 1.0f / zNow;
        s_residX += (ax - cx) * k;
        s_residY += (ay - cy) * k;
    }
    nx = iround(s_residX);
    ny = iround(s_residY);
    /* Sub-pixel: carried, not lost. This is NOT what keeps the centred-pointer
       case honest — that is the `ax == cx && ay == cy` test below, because
       `nx` is the accumulated residual and a centred gesture can still find a
       whole pixel sitting in it. */
    if (!nx && !ny) { drop_claim(); return; }

    /* THE RANGE FIRST, AND NOTHING IS POSTED WITHOUT ONE — from the packet's
       copy of the map size and the true viewport, at the same level the game
       thread's clamp will use. A frame whose packet carries no sane range (a
       level change with the view still live) posts nothing and keeps nothing:
       an unclamped step would otherwise wait in the sum for the next sane
       frame and land as one jump. */
    if (!range_pk(pk, pred_level(), &loX, &hiX, &loY, &hiY)) { drop_claim(); return; }
    /* A GESTURE THAT ASKS FOR NO DISPLACEMENT NEVER TAKES THE CAMERA, and
       this is the test that makes the A/B control structural instead of
       probable. `nx`/`ny` are the ACCUMULATED residual, so the pointer being
       on the viewport centre is not on its own enough: `iround` rounds half
       away from zero, and a debit leaves the remainder in the CLOSED interval
       [-0.5, +0.5], whose -0.5 endpoint is the common one — there `nx` is -1
       for ever with nothing feeding it, and a centred wheel would step a pixel
       and release the follow. Asking what THIS gesture is owed closes it by
       construction rather than by how often the float lands on a half.
       [landing review, 2026-09-10] */
    if (ax == cx && ay == cy) return;

    /* the step, taken against the eye this frame is drawn from — the packet's
       plus what is already posted and unacknowledged — and pre-clamped into
       the range; refused at a map edge, the residual is dropped rather than
       banked against the way back out */
    px = pk->eye[0] + (s_cumX - pk->cmd_ack_dx);
    py = pk->eye[1] + (s_cumY - pk->cmd_ack_dy);
    qx = px + nx; qy = py + ny;
    moved = clamp_pair(&qx, &qy, loX, hiX, loY, hiY);
    s_residX -= (float)nx;
    s_residY -= (float)ny;
    if (moved) s_residX = s_residY = 0.0f;
    s_cumX += qx - px;
    s_cumY += qy - py;
}

/* The eye this frame is drawn from: the packet's, plus the deltas the game
   thread has not applied yet, clamped into the range in force. */
static void predict(const TAGPU_PACKET* pk)
{
    int ux, uy, ex, ey, loX, hiX, loY, hiY;
    s_havePred = 0; s_unacked = 0;
    if (!pk) return;
    /* A NEW EPOCH — the level ended on the game thread, which reset what it
       had applied to zero: the sum posted from here on starts from zero too,
       and whatever was owed to the old level (a notch in its last frames, a
       carry) dies with it. The level-end packet carries the new epoch, and so
       does every packet of the next level, so this is seen at the latest on
       the new level's first frame — before any record of it could carry a
       delta the game thread would accept (landing review). */
    if (pk->cmd_epoch != s_epochSeen) {
        s_epochSeen = pk->cmd_epoch;
        s_cumX = s_cumY = 0; s_ackX = s_ackY = 0;
        s_residX = s_residY = 0.0f;
    }
    if (!pk->in_game) return;
    s_ackX = pk->cmd_ack_dx; s_ackY = pk->cmd_ack_dy;
    ux = s_cumX - s_ackX; uy = s_cumY - s_ackY;
    ex = pk->eye[0] + ux; ey = pk->eye[1] + uy;
    if (range_pk(pk, pred_level(), &loX, &hiX, &loY, &hiY))
        clamp_pair(&ex, &ey, loX, hiX, loY, hiY);
    s_predX = ex; s_predY = ey; s_havePred = 1;
    /* "ahead of the packet" is the DRAWN eye differing from the packet's,
       whatever moved it — an unacknowledged delta, or the range walking the
       eye home before the game thread has (a zoom-out at a map edge): either
       way the engine's grid spans the packet's eye and this frame must take
       the wide one (landing review) */
    s_unacked = (ex != pk->eye[0] || ey != pk->eye[1]);
}

/* GetTPosition on the world point under the mouse, clamped to the map — see the
   block above for why an off-map eye makes that necessary and why it is a no-op
   without one. */
static void __stdcall zoom_tpos_guard(int x, int y, int* out)
{
    const char* ta = *(const char* const*)TA_MAINPP;

    if (ta_ok(ta)) {
        int mw = *(const int*)(ta + OFF_MAP_W);
        int mh = *(const int*)(ta + OFF_MAP_H);
        if (mw > 0 && mh > 0) {
            if (x < 0) x = 0; else if (x > mw - 1) x = mw - 1;
            if (y < 0) y = 0; else if (y > mh - 1) y = mh - 1;
        }
    }
    ((PFN_GETTPOS)VA_GETTPOS)(x, y, out);
}

/* Keep our scaling out of the player's registry — see the block above. Game
   thread, like the apply that writes the byte it guards. */
static void __stdcall zoom_save_scroll(void* section, const char* name, int value)
{
    if (s_scrollBase && (unsigned char)value == s_scrollWrote)
        value = (int)s_scrollBase;
    ((void (__stdcall *)(void*, const char*, int))SAVE_SETTING_VA)(section, name, value);
}

/* an `E8 <rel32>` at `site` whose target is `expect`? */
static int site_is(unsigned int site, unsigned int expect)
{
    const unsigned char* p = (const unsigned char*)(size_t)site;
    if (IsBadReadPtr((void*)p, 5)) return 0;
    return p[0] == 0xE8 &&
           *(const unsigned int*)(p + 1) == expect - (site + 5);
}

static int bytes_are(unsigned int va, const unsigned char* want, int n)
{
    const void* p = (const void*)(size_t)va;
    return !IsBadReadPtr(p, (UINT_PTR)n) && memcmp(p, want, (size_t)n) == 0;
}

static int redirect(unsigned int site, void* target)
{
    unsigned char b[5];
    b[0] = 0xE8;
    *(unsigned int*)(b + 1) = (unsigned int)(size_t)target - (site + 5);
    return tagpu_detour_write(site, b, 5);
}

void tagpu_zoom_init(void)
{
    int ok;

    if (!tagpu_opt_on("tagpu_zoom.on")) return;

    /* Every byte is checked before any of them is written, so a patched or
       different exe arms nothing rather than half of it. */
    if (!site_is(SITE_MMRECT1_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_MMRECT2_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_SAVESCROLL, SAVE_SETTING_VA) ||
        !site_is(SITE_GETTPOS, VA_GETTPOS) ||
        !bytes_are(EYECLAMP_VA, EYE_STOLEN, (int)sizeof EYE_STOLEN) ||
        !bytes_are(FOLLOWCLAMP_VA, FOLLOW_STOLEN, (int)sizeof FOLLOW_STOLEN)) {
        zlog("zoom: NOT armed — engine bytes differ at one of "
             "0x41C426/0x41C442/0x430FAE/0x498EF9/0x41C3C0/0x41CAF7");
        return;
    }
    ok  = redirect(SITE_MMRECT1_VA, (void*)zoom_minimap_rect);
    ok &= redirect(SITE_MMRECT2_VA, (void*)zoom_minimap_rect);
    ok &= redirect(SITE_SAVESCROLL, (void*)zoom_save_scroll);
    g_mmInstalled = ok;
    /* The camera range needs its own guard in place before it may widen a
       thing, so the two arm together or not at all — and only on top of a
       minimap wrapper that went in, because our replacement clamp calls it.
       `&&`, not `&=`: the detour must not be LANDED at all when the guard did
       not take, rather than landed and left inert by a flag that happens never
       to be raised. */
    if (ok) {
        int eye = redirect(SITE_GETTPOS, (void*)zoom_tpos_guard) &&
                  tagpu_detour_leaf_call(EYECLAMP_VA, EYE_STOLEN,
                                         (int)sizeof EYE_STOLEN,
                                         &g_eyeWide, 0, zoom_eye_clamp);
        g_eyeInstalled = eye;
        /* THE FOLLOW'S CLAMP ONLY ON TOP OF THE EYE'S, and built before it is
           landed so a failed allocation arms nothing: its whole job is to agree
           with the range `zoom_eye_clamp` enforces, and widening the target
           where the eye is still clamped to `[0, map - view]` would be a camera
           that asks for a place it is then dragged out of every frame. */
        if (eye) {
            unsigned char* sf = build_follow_stub();
            int fol = sf && tagpu_detour_land(FOLLOWCLAMP_VA, sf,
                                              (int)sizeof FOLLOW_STOLEN);
            zlog(fol ? "zoom: ARMED (minimap rect 0x466B70 x2, ScrollSpeed save "
                       "0x430FAE, camera range 0x41C3C0 + follow clamp 0x41CAF7 "
                       "+ world guard 0x498EF9); the eye, the target, the follow "
                       "and ScrollSpeed are written on the game thread from the "
                       "frame packet's command apply"
                     : "zoom: ARMED without the follow clamp — 0x41CAF7 did NOT "
                       "install, so a followed unit still stops W/2 from a map "
                       "edge at zoom > 1");
        } else {
            zlog("zoom: PARTIAL — minimap and ScrollSpeed only, the camera "
                 "range did NOT install");
        }
    } else {
        zlog("zoom: PARTIAL — see above");
    }
}
