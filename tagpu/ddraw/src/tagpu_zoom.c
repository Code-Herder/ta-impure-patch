/* tagpu_zoom.c — the view transform shared by the render pass and the input
   path. See tagpu_zoom.h for the contract and why it is lock-free. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "dd.h"
#include "tagpu_zoom.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"

/* Published view. Volatile because two threads touch it; each is one aligned
   32-bit slot, which x86 loads and stores atomically. */
static volatile float s_zoom = 1.0f;
static volatile LONG  s_vpL, s_vpT, s_vw, s_vh;
static volatile LONG  s_live;          /* a zoomed world is on screen right now */
static volatile LONG  s_fresh;         /* the pass published during this frame     */
static int            g_mmInstalled;   /* tagpu_zoom_init() patched the engine     */
static void zlog(const char* m);   /* defined with the minimap patch below */

/* The range BOTH levers share. The transform is fine outside it; these are the
   levels the rest of the stack has been checked at. */
#define ZOOM_MIN  0.25f
#define ZOOM_MAX  8.0f

static void zlog(const char* m);
static int  in_viewport(int x, int y, int L, int T, int W, int H);
static void apply_eye_range(void);

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

float tagpu_zoom_read_lever(void)
{
    /* Re-read EVERY frame: this is a continuous control, so a 30-frame poll
       would quantise a ramp to 2 Hz and make a smooth renderer look like a
       staircase on video. On a bad parse the LAST GOOD value is kept rather
       than snapping back to 1.0 — a writer driving a ramp at 60 Hz can be
       caught mid-write, and a one-frame jump to unzoomed reads as a flicker.
       Only the file's ABSENCE hands the level to the wheel. */
    HANDLE zh = CreateFileA("tagpu_zoom.txt", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (zh == INVALID_HANDLE_VALUE) {
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
    return s_zoom;
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

   This is the one engine field the zoom writes, and it is safe to write: the
   eye is not simulated — it is a per-player camera preference that no other
   machine ever sees — so this cannot perturb the sim or an MP session.

   The base is re-read from the engine whenever the value is not the one we last
   wrote, so changing the preference in the options screen is picked up on the
   next frame instead of being overwritten by a stale cache.

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

static unsigned char s_scrollBase, s_scrollWrote;

static void apply_scroll_rate(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    unsigned char* p;
    float z;
    int v;

    if ((size_t)ta <= 0x600000u || (size_t)ta >= 0x7FFF0000u) return;
    p = (unsigned char*)(ta + OFF_SCROLLSPEED);
    if (*p != s_scrollWrote) s_scrollBase = *p;   /* the engine or the player */
    if (!s_scrollBase) return;

    z = tagpu_zoom_level();
    /* Without the save guard installed (tagpu_zoom.on absent) the scaled value
       would reach the player's registry, so leave the field alone entirely. */
    v = (g_mmInstalled && z > 0.05f && z != 1.0f)
        ? (int)((float)s_scrollBase / z + 0.5f) : (int)s_scrollBase;
    if (v < 1) v = 1;
    if (v > 255) v = 255;
    *p = s_scrollWrote = (unsigned char)v;
}

void tagpu_zoom_frame_end(void)
{
    if (!s_fresh) s_live = 0;
    s_fresh = 0;
    /* after s_live settles, so a frame that drew nothing zoomed puts the
       engine's own rate back */
    apply_scroll_rate();
    /* and the same for the addressable rect: a frame that published nothing
       must hand the engine its own viewport back before the menus see it */
    tagpu_vpwide_frame(tagpu_zoom_level());
    /* and for the camera range — last, because it is the only one of the three
       that can still MOVE the view this frame */
    apply_eye_range();
}

float tagpu_zoom_level(void)
{
    return s_live ? s_zoom : 1.0f;
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

/* Round toward the nearest pixel, negatives included: (int) truncates toward
   zero, which would bias every point left of the centre by half a pixel and
   make a zoomed-out click land one pixel off on one side of the screen only. */
static int iround(float v)
{
    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
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
   the engine draws its cursor from on the paths that do not poll (`0x4C2380`).
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
    if (!tagpu_zoom_to_engine(&x, &y)) return lparam;
    return MAKELPARAM((short)x, (short)y);
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
   minimap so a wildly zoomed-out view cannot draw a box off the edge of it. */

#define SAVE_SETTING_VA  0x004B6A50u   /* stdcall(section,name,dword), ret 0xC */
#define SITE_SAVESCROLL  0x00430FAEu   /* the one site that persists ScrollSpeed */
#define MINIMAP_RECT_VA  0x00466B70u   /* stdcall(RECT*), ret 4              */
#define SITE_MMRECT1_VA  0x0041C426u   /* call 0x466B70 — eye clamped at top */
#define SITE_MMRECT2_VA  0x0041C442u   /* call 0x466B70 — ...and at bottom   */
#define OFF_MM_X         0x142E7       /* i16 minimap rect on screen         */
#define OFF_MM_Y         0x142E9
#define OFF_MM_W         0x142EB
#define OFF_MM_H         0x142ED

static void zlog(const char* m)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
}

static void __stdcall zoom_minimap_rect(int* r)
{
    const char* ta;
    float zm = tagpu_zoom_level();
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
    if ((size_t)ta <= 0x600000u || (size_t)ta >= 0x7FFF0000u) return;
    mx = *(const short*)(ta + OFF_MM_X); my = *(const short*)(ta + OFF_MM_Y);
    mw = *(const short*)(ta + OFF_MM_W); mh = *(const short*)(ta + OFF_MM_H);
    if (mw <= 0 || mh <= 0) return;
    if (r[0] < mx) r[0] = mx;
    if (r[1] < my) r[1] = my;
    if (r[2] > mx + mw - 1) r[2] = mx + mw - 1;
    if (r[3] > my + mh - 1) r[3] = my + mh - 1;
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
   target afterwards (`0x41C574` SetCamera, `0x41CDB0`, the scroll `0x41D037`)
   reaches the widened range through us. But three sites compute that target and
   clamp it INLINE against `[0, map - W]` without going through this function at
   all — `0x41C4C0` (the smooth SetCamera), `0x41C7F7` (the smooth centre-on) and
   `0x41CAF7` (the per-frame camera FOLLOW, which recomputes the target from the
   tracked unit every frame). The stepper `0x41CA30` then eases the eye to that
   target and our clamp, being wider, leaves it there — so those paths still stop
   `d` short of a map edge. Nothing fights and nothing churns (the eye arrives at
   a target that is inside our range and both stop), it is simply the old
   behaviour on the paths the detour does not sit on. Closing them means widening
   three inline clamps in the middle of the camera module, which is a much bigger
   patch than this one and has not been done.

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

#define EYECLAMP_VA      0x0041C3C0u   /* stdcall(void), ret 0: clamp + mm rect */
#define VA_GETTPOS       0x00484B50u   /* GetTPosition(x,y,out), stdcall, ret 0xC */
#define SITE_GETTPOS     0x00498EF9u   /* its call site inside 0x498DA0           */
#define OFF_EYEX         0x1431F
#define OFF_EYEY         0x14323
#define OFF_MAP_W        0x1422B       /* map size in world px                    */
#define OFF_MAP_H        0x1422F
#define OFF_SCRTX        0x14327       /* MapXScrollingTo — the eye eases to here */
#define OFF_SCRTY        0x1432B
#define OFF_MM_RECT      0x142CB       /* the RECT 0x466B70 fills                 */

/* `mov eax, ds:0x511DE8` — the whole first instruction, so the five stolen
   bytes end on an instruction boundary (0x41C3C5 is `push esi`). */
static const unsigned char EYE_STOLEN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };

typedef void (__stdcall *PFN_GETTPOS)(int x, int y, int* out);

static volatile unsigned char g_eyeWide;  /* the game thread reads this one      */
static int  g_eyeInstalled;               /* the detour and the guard both went in */
static int  s_eyeOff;                     /* tagpu_zoomedge.off, polled          */

static int ta_ok(const char* ta)
{
    return (size_t)ta > 0x600000u && (size_t)ta < 0x7FFF0000u;
}

/* The escape hatch, polled rather than cached at attach so it can be flipped on
   a running instance. Thrown, the level below reads as 1.0, which walks the eye
   back onto the 1x range rather than merely freezing it where it stood.

   The poll is the RENDER thread's, from apply_eye_range() once a frame and
   throttled on top of that: the level has one owner in this module, and a
   synchronous file stat has no business on the game thread — which reads the
   result here and does nothing else with it. */
static void eye_poll_off(void)
{
    static DWORD tick;                       /* render thread only */
    DWORD now = GetTickCount();
    if (now - tick > 250) {
        tick = now;
        s_eyeOff = (GetFileAttributesA("tagpu_zoomedge.off") != INVALID_FILE_ATTRIBUTES);
    }
}

static float eye_level(void)
{
    return s_eyeOff ? 1.0f : tagpu_zoom_level();
}

/* The camera range at `z`: the engine's own bounds, widened by d. Returns 0 —
   and leaves the outputs alone — when the engine state is not sane enough to
   compute one. */
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
    *loX = -dx; *hiX = mapW - W + dx;
    *loY = -dy; *hiY = mapH - H + dy;
    /* A map smaller than the viewport inverts the ENGINE's range too — it then
       alternates between 0 and a negative bound on every call. Ours holds
       still, which is the most that can be said for either. */
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

   THE SCROLL TARGET IS DELIBERATELY NOT TOUCHED HERE, unlike in
   apply_eye_range(). `main+0x14327`/`+0x1432B` is where the camera is heading,
   and three of this function's callers are inside the per-frame stepper
   `0x41CA30`, which eases the eye halfway toward it and calls us afterwards:
   writing the target there would make it the eye every frame and the camera
   would never arrive. Every caller that MEANT to move the camera copies the
   clamped eye into the target itself, right after we return (`0x41C5A4`,
   `0x41CDE6`, `0x41D05E`), so the pair stays consistent without our help. */
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

/* THE EYE HAS TO COME HOME WHEN THE ZOOM DOES. The clamp above runs only when
   the engine moves the camera, so an eye parked at -d at 4x would sit there —
   showing the void past the map edge — from the moment the player wheels back
   out until the next time they scroll. This is the same clamp applied from the
   render thread once a frame; it writes only when the eye is actually outside
   the range in force, which is during a zoom-out at a map edge and at no other
   time. Same standing as the ScrollSpeed write above: local camera state that
   no other machine ever sees.

   AND IT MUST MOVE THE SCROLL TARGET WITH IT — `main+0x14327`/`+0x1432B`, the
   pair the engine eases the eye toward. Unlike the clamp above, this correction
   has no caller to copy the eye into the target afterwards, and the per-frame
   stepper `0x41CA30` acts on any disagreement between the two: at `0x41CB5F` it
   sets the camera-moved bit and at `0x41CB6B` it CLEARS `main+0x14281` bit 3,
   the fog grid's own is-current flag, then halves the distance and hands the
   result to the (no longer widened) engine clamp, which puts it straight back.
   A zoom-out from a map edge would therefore leave the eye at 0 and the target
   at -d for as long as the player did not scroll — a permanent per-frame fog
   grid rebuild on exactly the path 97e518f had to guard against a crash.

   Clamping the target into the range rather than assigning the eye to it is
   what keeps a camera move that is genuinely in flight: such a target is inside
   [0, map - W] already, so it is inside ours too and is not touched at all. */
static void apply_eye_range(void)
{
    char* ta;
    int loX, hiX, loY, hiY, moved;
    float z;

    if (!g_eyeInstalled) return;
    eye_poll_off();
    z = eye_level();
    g_eyeWide = (unsigned char)(z > 1.0f);   /* 1.0 in the menus: s_live is 0 */
    if (!s_live) return;                     /* nothing to correct, and no game */
    ta = *(char**)TA_MAINPP;
    if (!zoom_eye_range(ta, z, &loX, &hiX, &loY, &hiY)) return;
    moved  = clamp_pair((int*)(ta + OFF_EYEX),  (int*)(ta + OFF_EYEY),
                        loX, hiX, loY, hiY);
    moved |= clamp_pair((int*)(ta + OFF_SCRTX), (int*)(ta + OFF_SCRTY),
                        loX, hiX, loY, hiY);
    /* Recomputed here because `0x41C3C0` is the only place the engine ever
       fills this rect, so a corrected eye would otherwise leave the minimap's
       box where it was until the next camera move. It is a write to
       `main+0x142CB` from the RENDER thread — the same one-frame-tear standing
       as the published view this module already accepts, and it happens only on
       the frames the correction fires. */
    if (moved) zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
}

int tagpu_zoom_eye_range(int* loX, int* hiX, int* loY, int* hiY)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    /* the ENGINE's own bounds unless our clamp is the one actually in force —
       an eye hold that reached past them with the detour absent would only be
       hauled back by the engine on its next scroll */
    return zoom_eye_range(ta, g_eyeInstalled ? eye_level() : 1.0f,
                          loX, hiX, loY, hiY);
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

/* Keep our scaling out of the player's registry — see the block above. */
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

    if (GetFileAttributesA("tagpu_zoom.on") == INVALID_FILE_ATTRIBUTES) return;

    /* Every byte is checked before any of them is written, so a patched or
       different exe arms nothing rather than half of it. */
    if (!site_is(SITE_MMRECT1_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_MMRECT2_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_SAVESCROLL, SAVE_SETTING_VA) ||
        !site_is(SITE_GETTPOS, VA_GETTPOS) ||
        !bytes_are(EYECLAMP_VA, EYE_STOLEN, (int)sizeof EYE_STOLEN)) {
        zlog("zoom: NOT armed — engine bytes differ at one of "
             "0x41C426/0x41C442/0x430FAE/0x498EF9/0x41C3C0");
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
        zlog(eye ? "zoom: ARMED (minimap rect 0x466B70 x2, ScrollSpeed save "
                   "0x430FAE, camera range 0x41C3C0 + world guard 0x498EF9)"
                 : "zoom: PARTIAL — minimap and ScrollSpeed only, the camera "
                   "range did NOT install");
    } else {
        zlog("zoom: PARTIAL — see above");
    }
}
