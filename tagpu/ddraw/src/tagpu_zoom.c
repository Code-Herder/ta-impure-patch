/* tagpu_zoom.c — the view transform shared by the render pass and the input
   path. See tagpu_zoom.h for the contract and why it is lock-free.

   TWO HALVES, ONE FILE. The RENDER half owns the level and the arithmetic —
   the levers, the wheel's tween, the cursor anchor's eye delta, the
   predicted eye every pass draws from — and writes NOTHING into engine
   memory: its whole output is the command record tagpu_zoom_frame_end()
   posts. The GAME half — the engine's own clamp and minimap-rect sites this
   module redirects, plus tagpu_zoom_apply() at the top of every in-play draw
   — is the only code here that touches the eye, the scroll target, the
   follow slots, the minimap box and its dirty bit, the fog flag and
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
#include "tagpu_log.h"

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
static void anchor_step(int fromWheel, const TAGPU_PACKET* pk);
static float predict(const TAGPU_PACKET* pk, float z, int mayHold);
static int  iround(float v);
static int  clampi(int v, int lo, int hi);

/* ---- the wheel -------------------------------------------------------------

   BAR's wheel (Beyond All Reason on the Recoil engine; bar-camera-port.md
   §1.2 has the sources). A notch scales the camera's DISTANCE, which for TA's
   fixed oblique view is iz = 1/z, and the drawn camera tweens to the new
   target over a fixed time.

   THE NOTCH. A wheel message of delta d is n = d / WHEEL_DELTA notches, and it
   multiplies the target distance by max(0.1, 1 - 0.14 n): Recoil's
   `1 + move * 0.007` with BAR's ScrollWheelSpeed of -20. One notch in is
   x0.86 on the distance and x1.163 on the level; one out is x1.14 and x0.877,
   so in-then-out lands on 1.020, not on 1. The factor is taken per MESSAGE,
   as Recoil takes it per event, and the target distance is clamped to
   [1/ZOOM_MAX, 1/ZOOM_MIN] after each one, as the lab does.

   THE TWEEN. 250 ms of the performance counter, counted from the latest notch,
   from the level DRAWN when the notch is taken to the target, along
   g = 1 - (1 - f)^4 applied to the distance (Recoil's CamTransitionMode 0
   with CamTimeExponent 4, an ease-out). A notch during a tween starts a new
   one from the drawn level toward the previous target times its factor. The
   length is the same at every frame rate; at 60 Hz a quarter of a notch lands
   on the first frame and four fifths of it by 83 ms.

   THE VIEW CENTRE TWEENS WITH THE SAME g, AND EACH NOTCH IN AIMS IT FROM ITS
   OWN POINT. Recoil (and the lab, tascene-view.html) keeps a target pose —
   centre and distance — and lerps the drawn pose to it. A notch in at screen
   point a moves the target centre by (a - c)(iz_prev - iz_new), c the
   viewport centre and iz_prev/iz_new the target distance before and after
   that notch, which is what holds the world point under a at the target; a
   notch out leaves the target centre where it is. The eye here is not a
   target but a sum of deltas (anchor_step), so the same thing is kept as R,
   the displacement the tween still owes:

       at a notch      R = R (1 - g_posted) + (in ? (a - c)(iz_prev - iz_new) : 0)
                       and the tween restarts, g_posted = 0
       every frame     post R (g - g_posted), g_posted = g

   The first line is the lab's `c1 += ...` seen from the drawn centre: what is
   left of the old tween's displacement, plus this notch's. Summed over a
   gesture the posts telescope to the sum of the notches' own terms, so where
   the eye ends depends only on the notches' points and targets, never on the
   frame timing — the same final pose the lab computes, away from the clamp.

   ONE OWNER, AND EVERY NOTCH KEEPS ITS OWN POINT. The message thread only
   queues: one slot per message, holding its delta, the point it was aimed at
   and the counter time, in a single-producer single-consumer ring (below).
   The targets, the tween, R and the clamp are the render thread's, inside
   read_lever(), so the level has exactly one owner and no float crosses the
   threads. */
#define NOTCH_PER_UNIT   0.14      /* per WHEEL_DELTA: Recoil's 0.007 x 20    */
#define NOTCH_FLOOR      0.1       /* the smallest factor one message applies */
#define TWEEN_US         250000.0  /* Recoil's wheel transition, 0.25 s       */

#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

/* THE NOTCH RING. The producer is tagpu_zoom_wheel(), which runs only inside
   the window procedure — the fork's wndproc and the shield's delivery, both on
   the window's owner thread, which is the only thread Windows runs a window
   procedure on — and calls nothing that pumps messages, so it is one thread
   and never re-entered. The consumer is the render thread (read_lever and the
   two pins). THE ORDERING IS THE WHOLE SAFETY ARGUMENT: the producer writes a
   slot and only then publishes `head` with an interlocked store (a full
   fence), and never writes a slot while `head - tail == NOTCHQ`; the consumer
   reads `head`, then the slots below it, and only then publishes `tail` with
   an interlocked store. So a slot is read only after it is complete and
   rewritten only after it has been read. The indices are free-running and
   compared by unsigned difference, which a wrap does not disturb (NOTCHQ
   divides 2^32). A full ring drops the notch and says so: 256 messages inside
   one render frame is a stall, and by then the target has hit the end of the
   range many times over. */
#define NOTCHQ 256
typedef struct {
    LONG delta;                    /* the message's raw wheel delta            */
    LONG xy;                       /* where it was aimed: x, y as two shorts   */
    LONG us;                       /* qpc_us() when it was taken               */
} NOTCH;
static NOTCH         s_nq[NOTCHQ];
static volatile LONG s_nqHead;                  /* written by the message thread only */
static volatile LONG s_nqTail;                  /* written by the render thread only  */

/* render thread only: the target and the drawn level, and the tween between */
static float         s_lever = 1.0f;            /* the level the frame is drawn at   */
static float         s_wheelTgt = 1.0f;
static float         s_wheelCur = 1.0f;
static float         s_izFrom = 1.0f, s_izTo = 1.0f;
static DWORD         s_tweenUs;                 /* the counter time it counts from   */
static int           s_tweening;
static double        s_tweenG = 1.0;            /* this frame's g; 1 when nothing tweens */
static int           s_batch;                   /* notches were taken this frame     */
static float         s_shiftX, s_shiftY;        /* ...and their own terms of R       */
static float         s_remX, s_remY;            /* R: the displacement still owed    */
static double        s_gPosted = 1.0;           /* the g already posted of R         */
static LONG          s_wheelPend;               /* notches not yet logged            */
static float         s_landMs = -1.0f;          /* the last tween's length, for the log */

/* The performance counter in microseconds, modulo 2^32: both threads convert
   the same counter the same way, so a signed 32-bit difference of two stamps
   is exact for anything under 35 minutes. Split so the multiply cannot
   overflow at any uptime. */
static DWORD qpc_us(void)
{
    LARGE_INTEGER q, f;
    if (!QueryPerformanceCounter(&q) || !QueryPerformanceFrequency(&f) || f.QuadPart <= 0)
        return GetTickCount() * 1000u;
    return (DWORD)((q.QuadPart / f.QuadPart) * 1000000LL +
                   (q.QuadPart % f.QuadPart) * 1000000LL / f.QuadPart);
}

/* One wheel message's factor on the distance. */
static double notch_factor(LONG delta)
{
    double k = 1.0 - NOTCH_PER_UNIT * (double)delta / WHEEL_DELTA;
    return k < NOTCH_FLOOR ? NOTCH_FLOOR : k;
}

/* Render thread: empty the ring without taking anything from it. The raw
   delta thrown away, for the caller's log line. */
static LONG wheel_discard(void)
{
    LONG h = s_nqHead, t = s_nqTail, d = 0;
    MemoryBarrier();                            /* head, then the slots under it */
    for (; t != h; t++) d += s_nq[(ULONG)t % NOTCHQ].delta;
    InterlockedExchange(&s_nqTail, t);          /* the slots are read: release them */
    return d;
}

/* Render thread: the wheel at rest at level z — no tween, nothing owed, the
   ring emptied. Returns the raw delta thrown away. */
static LONG wheel_reset(float z)
{
    LONG dropped = wheel_discard();
    s_wheelTgt = s_wheelCur = z;
    s_tweening = 0;
    s_tweenG = 1.0;
    s_batch = 0;
    s_remX = s_remY = 0.0f;
    s_gPosted = 1.0;
    s_wheelPend = 0;
    return dropped;
}

/* Pin the wheel to a level without a tween, and throw away any notches that
   arrived alongside. Used while the file lever is in force: the file wins, and
   when it goes away the wheel takes over from exactly where it left the view. */
static void wheel_pin(float z)
{
    LONG dropped = wheel_reset(z);
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

/* Take the notches since the last frame, in order, and draw the tween at this
   frame's time. Leaves s_batch/s_shiftX/s_shiftY for anchor_step.

   THE ANCHOR IS BOUNDED HERE, NOT BY THE PRODUCER. The message thread took a
   notch only inside the viewport it saw — four plain stores it can read
   mixed, and a HUD-scale or video-mode change can land between the notch and
   this frame. So each anchor is clamped into the viewport this frame is drawn
   with (the packet's `vp`, the TRUE 1x rect; the last published one when
   there is no packet), and the centre `c` is that viewport's: |a - c| <= W/2
   by construction, which is the bound fogw_window's "a zoom-in is always
   covered" rests on. For every notch the producer took against the same
   viewport the clamp is the identity. */
static float wheel_level(const TAGPU_PACKET* pk)
{
    LONG  h = s_nqHead, t = s_nqTail, d = 0, lastUs = 0;
    DWORD now;
    int   vL = (int)s_vpL, vT = (int)s_vpT, vW = (int)s_vw, vH = (int)s_vh;
    float cx, cy;
    int   n = 0;

    if (pk && pk->vp[2] > 0 && pk->vp[3] > 0) {
        vL = pk->vp[0]; vT = pk->vp[1]; vW = pk->vp[2]; vH = pk->vp[3];
    }
    cx = (float)vL + (float)vW * 0.5f;
    cy = (float)vT + (float)vH * 0.5f;
    now = qpc_us();

    MemoryBarrier();                            /* head, then the slots under it */
    s_batch = 0;
    s_shiftX = s_shiftY = 0.0f;
    for (; t != h; t++) {
        const NOTCH* e = &s_nq[(ULONG)t % NOTCHQ];
        double izPrev = 1.0 / (double)s_wheelTgt;
        double iz = izPrev * notch_factor(e->delta);
        float  z;
        if (iz < 1.0 / ZOOM_MAX) iz = 1.0 / ZOOM_MAX;
        if (iz > 1.0 / ZOOM_MIN) iz = 1.0 / ZOOM_MIN;
        z = (float)(1.0 / iz);
        /* Land EXACTLY on 1.0 when the notches bring it there. 1x is the
           identity the whole stack tests for by equality — the transform, the
           minimap rect and the scroll rate each short-circuit on `z == 1.0f` —
           so a target within a thousandth of it is taken as it. BAR's notches
           do not cancel (in-then-out is 1.020), so this is reached only by a
           sequence that happens to land inside the band. */
        if (z > 0.999f && z < 1.001f) z = 1.0f;
        s_wheelTgt = z;
        iz = 1.0 / (double)z;
        /* a notch IN holds the point it was aimed at, clamped into this
           frame's viewport (above) */
        if (iz < izPrev && vW > 0 && vH > 0) {
            int   qx = (int)(short)(e->xy & 0xFFFF);
            int   qy = (int)(short)((e->xy >> 16) & 0xFFFF);
            float ax = (float)clampi(qx, vL, vL + vW - 1);
            float ay = (float)clampi(qy, vT, vT + vH - 1);
            s_shiftX += (ax - cx) * (float)(izPrev - iz);
            s_shiftY += (ay - cy) * (float)(izPrev - iz);
        }
        d += e->delta;
        lastUs = e->us;
        n++;
    }
    InterlockedExchange(&s_nqTail, t);          /* the slots are read: release them */

    if (n) {
        LONG back;
        s_batch  = 1;
        /* the level last DRAWN, which is the tween's own unless the fog bound
           held it for that frame (predict) */
        s_izFrom = 1.0f / s_lever;
        s_izTo   = 1.0f / s_wheelTgt;
        /* The tween counts from the latest notch's own stamp, not from this
           frame. The head is read BEFORE the clock, so every stamp taken is
           at or before `now`; a negative difference — a stamp from another
           core's counter a hair ahead — starts the tween now. A stamp is not
           bounded to a frame old: frames that skip read_lever (overlay.off,
           the reclaim teardown) leave notches queued for as long as they
           last, and an old enough stamp lands the tween on this frame, R
           posted whole in one step (anchor_step) — a camera jump of at most
           the gesture's own displacement, clamped like any other step. */
        back = (LONG)(now - (DWORD)lastUs);
        s_tweenUs = back > 0 ? now - (DWORD)back : now;
        /* A tween even when the level does not move (a notch at the end of
           the range): R may still owe the displacement of the tween it cut,
           and it is paid out along this one rather than in one frame. */
        s_tweening = 1;
        s_landMs = -1.0f;
        s_wheelPend += d;
    } else if (s_wheelPend && !s_tweening) {
        /* ONE line per gesture, when it lands, not one per frame that carried
           notches. zlog is a file write and this runs on the render thread, so
           a sustained spin would otherwise write a line every frame for as
           long as it lasted. The landing time is measured from the LAST notch
           to the first frame drawn at the target. */
        char b[112];
        if (s_landMs >= 0.0f)
            _snprintf(b, sizeof b, "zoom: wheel %+d -> %.3f, landed %.1f ms after the last notch",
                      (int)s_wheelPend, s_wheelTgt, s_landMs);
        else
            _snprintf(b, sizeof b, "zoom: wheel %+d -> %.3f",
                      (int)s_wheelPend, s_wheelTgt);
        b[sizeof b - 1] = 0;
        s_wheelPend = 0;
        zlog(b);
    }

    s_tweenG = 1.0;
    if (s_tweening) {
        LONG   el = (LONG)(now - s_tweenUs);
        double f  = el < -1000000 ? 1.0 : el < 0 ? 0.0 : (double)el / TWEEN_US;
        if (f >= 1.0) {
            /* the target itself, not the lerp's last value: an exact 1.0
               stays exact */
            s_wheelCur = s_wheelTgt;
            s_tweening = 0;
            s_landMs = el > 0 ? (float)el / 1000.0f : 0.0f;
        } else {
            double r = 1.0 - f, g = 1.0 - r * r * r * r;
            double iz = (double)s_izFrom + ((double)s_izTo - (double)s_izFrom) * g;
            s_wheelCur = (float)(1.0 / iz);
            s_tweenG = g;
        }
    }
    return s_wheelCur;
}

/* the eye this frame is drawn from (render thread) */
static int   s_predX, s_predY, s_havePred, s_fogWide;
/* the fog bound's cost and the monotone rule's witness (tagpu_zoom_fog_held) */
static unsigned s_fogHeld, s_fogPaused, s_fogBack;
static int      s_fogHeldMax;

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
        wheel_reset(1.0f);
        s_zoom = 1.0f;
        anchor_step(0, pk);
        predict(pk, 1.0f, 0);
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
    float  z = s_zoom;
    if (fromWheel) {
        z = wheel_level(pk);
    } else {
        char zb[32]; DWORD zn = 0;
        if (ReadFile(zh, zb, sizeof zb - 1, &zn, 0) && zn > 0) {
            float fz;
            zb[zn] = 0;
            fz = (float)atof(zb);
            if (fz >= ZOOM_MIN && fz <= ZOOM_MAX) z = fz;
        }
        CloseHandle(zh);
        /* On a torn read `z` is still `s_zoom`, the LAST GOOD level, which is
           the one actually in force and therefore the one the wheel must
           inherit when the file goes away. */
        wheel_pin(z);
    }
    /* The eye step that holds the point under the cursor, worked out HERE:
       this is the call the driver makes at the top of its frame, before any
       pass reads the eye, so the zoom and the eye it is drawn with change
       together. Only the wheel anchors — the file lever has no gesture behind
       it and every zoom fixture drives it. */
    anchor_step(fromWheel, pk);
    /* the fog bound may hold the level for this frame (predict), so the
       published level is written once, after it */
    z = predict(pk, z, 1);
    s_zoom = z;
    s_lever = z;
    return z;
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

/* RENDER THREAD. Which fog grid THIS frame samples, decided by predict() with
   every term this frame's: the level read_lever settles on (the one the native
   pass draws with), not tagpu_zoom_level(), whose `live` is raised by this
   frame's own publish_view after the grid is chosen — the first in-play frame
   after the shell, with the wheel level kept below 1, would otherwise draw
   zoomed out on the engine's grid. */
int tagpu_zoom_wide_fog(void)
{
    return s_fogWide;
}

int tagpu_zoom_gather_span(int v, float z)
{
    int span = v;
    if (z > 0.05f && z < 1.0f) {
        /* the floor's span is the cap: a level that slipped below the floor
           cannot ask for an unbounded rect */
        int cap = (int)((float)v / TAGPU_ZOOM_MIN) + 64;
        span = (int)((float)v / z) + 64;
        if (span > cap) span = cap;
    }
    return span < v ? v : span;
}

void tagpu_zoom_pub_window(int eye, int v, int* lo, int* span)
{
    int s = tagpu_zoom_gather_span(v, ZOOM_MIN);
    int pad = TAGPU_GATHER_MARGIN + (v > 0 ? v / TAGPU_LEAD_DIV : 0);

    *lo = eye - (s - v) / 2 - pad;
    *span = s + 2 * pad;
}

void tagpu_zoom_fog_held(unsigned* frames, int* maxPx, unsigned* paused, unsigned* back)
{
    *frames = s_fogHeld;
    *maxPx = s_fogHeldMax;
    *paused = s_fogPaused;
    *back = s_fogBack;
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
    /* ONE SLOT PER MESSAGE, its point with it: the gate above has already
       proved this point is inside the true viewport, which is what makes it a
       legal anchor. The slot is written first and published after — see "the
       notch ring" for the ordering. */
    {
        LONG   h = s_nqHead;                    /* this thread's own index */
        NOTCH* e;
        if ((ULONG)(h - s_nqTail) >= NOTCHQ) {
            if (now - s_gripeTick > 1000) {
                s_gripeTick = now;
                zlog("zoom: wheel notch dropped - the ring is full (the render thread is not taking them)");
            }
            return 1;
        }
        e = &s_nq[(ULONG)h % NOTCHQ];
        e->delta = (LONG)delta;
        e->xy    = (LONG)(((unsigned)(unsigned short)y << 16) | (unsigned)(unsigned short)x);
        e->us    = (LONG)qpc_us();
        InterlockedExchange(&s_nqHead, h + 1);  /* publish: the slot is complete */
    }
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
#define OFF_GUI_FLAGS    0x37EBE   /* bit 0: ARMOPT/exit/preferences owns input */

/* game thread only: the level the last command carried, and whether it is
   live — what every game-thread reader in this file uses in place of the
   render thread's s_zoom/s_live, so no float crosses the threads — and
   whether the centre range is in force (camera_centre() below) */
static float s_gLevel = 1.0f;
static int   s_gLive, s_gCentre;
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

   THE CENTRE CLAMP (BAR's: Recoil's SpringController clamps the ground point
   at the view centre to the map, and nothing else). The view is centred on
   `eye + W/2` at every zoom, W being the TRUE viewport, so the range that keeps
   that centre on the map is

       eye in [-W/2, map - W/2]            the same at every zoom

   and the map's edge can reach the middle of the screen, zoomed in or out.
   The engine's own `0x41C3C0` clamps to `[0, extent - W]` instead, the range
   that keeps the VIEWPORT's edges on the scroll extent's at 1x.

   TWO SIZES, AND ONLY ONE IS THE MAP. `map` is the map's own size, the PLOT
   grid main+0x14233/0x14237 (16-px cells) x 16 — the tile map's extent, the
   one the lab and the terrain pass draw. `extent` is the SCROLL EXTENT
   main+0x1422B/0x1422F, the map less 32 px wide and 128 px tall, written at
   the level load from the map's pixel size main+0x14223/0x14227
   (`0x4833B8..0x4833E0`: `sub 0x20`, `sub 0x80`) and rewritten after that only
   by the debug-level `Edge` command [DISASSEMBLED]. MEASURED 2026-09-23 on Two
   Continents: PLOT 672 x 800, main+0x14223/0x14227 10752 x 12800, extent
   10720 x 12672. The view centre stops at the MAP's edge, as the lab's does;
   the engine's range and the pointer's guards stay on the extent, the bound
   they are built on (zoom_tpos_guard). The PLOT grid and not
   main+0x14223 is read for the map because the packet carries it
   (`map_w16/h16`): the two threads then compute the range from the same
   words, and agree by construction.

   THE CENTRE RANGE IS IN FORCE ONLY WHILE THE GROUND IS OURS, and that is the
   bound the whole range rests on. The engine's terrain pass `0x483FA0` indexes
   the tile map from the eye with no bounds check at either end (`0x48409B`,
   exe map "Two per-cell loops that differ"), so an eye off the engine's range
   under an engine terrain draw reads before the tile array or past its end.
   What follows bounds that EYE and nothing more: over a view larger than the
   map the pass's window runs past the tile map from any eye (the engine's
   own range is empty there), in stock and on both cameras, and the window
   check at 0x484057 bounds it, drawing the part past the map black
   (tagpu_patches.c; exe map, "Engine defects we patch").
   terrown takes that function away on every draw whose terrain latch is up
   (`g_terrown_own`, tagpu_terrown.c), and the latch is set by the packet
   publisher right after this module's apply, from the same request the apply
   was handed (`terr || rect still wide`). So:

     the apply runs at the top of every in-play draw, before the draw's first
     read of the eye; it takes the centre range only when the request is up,
     which makes the latch up for that draw; and it clamps the eye and the
     target into the range in force on every draw (every draw that applied a
     delta or a hold, when our eye clamp is not installed and the engine's
     own clamp keeps the rest). A draw whose ground is the engine's therefore
     starts from an eye in `[0, extent - W]`.

     between in-play draws, every engine camera writer ends in `0x41C3C0` or
     in one of the target clamps below, and all of them use the range the
     last apply chose, which the latch still matches: nothing else writes
     the latch, and the level end drops both together (and walks the eye
     home).

     WHAT THAT COSTS, BY DESIGN: whenever our terrain pass hands the ground
     back — its 90-frame watchdog, a `key=` change, the passive or over
     modes, a gather that bails or draws no cell, the pass disarmed — the
     next apply takes the engine's range and walks an eye that was past
     `[0, extent - W]` inward, by up to W/2 (H/2), on that draw. And it stays
     there when the ground comes back: the range is a clamp, not a memory,
     so a view that had the map's edge at its centre shows it W/2 world px
     nearer its own edge (on it, at 1x) until the player scrolls back. The level
     end does the same.

     EVERY CLAMP FAILS CLOSED. Where the range in force cannot be computed
     (a main pointer outside the sanity window, a zero viewport or map), each
     one clamps to the engine's own `[0, extent - W]` from the engine's own words
     instead (engine_range), and the apply publishes `centre = 0` for that
     draw — so no path leaves the eye or the target unclamped. The pointer's
     guard (zoom_tpos_guard) fails closed on the map's own size where the
     extent is not positive, and passes the point through only where there
     is no map, or no sane main, to clamp against. The screenshot tiler's
     `DrawGameScreen(1, 0)` loop `0x495C76` and the movie recorder never
     apply or latch, so they draw on exactly that pair.

   Every other engine reader of the eye is bounded on its own for any eye in
   the centre range at any zoom, with vpwide's widened rect below 1x — the
   audit is the exe map's "Who reads the eye" table. The one without a lower
   bound is the map debug overlay `0x418310`, which has its own guard here
   (zoom_debug_overlay).

   THE SCROLL TARGET `main+0x14327`/`+0x1432B` HAS THREE REACHABLE INLINE
   CLAMPS, all to `[0, extent - W]`, that never reach `0x41C3C0`: the smooth arms
   of the centre-on `0x41C7C0` (block `0x41C808`) and of the centre-on-object
   `0x41C8E0` that centre-on-unit calls (block `0x41C93B`), and the per-frame
   camera follow in the stepper (block `0x41CAF7`). The stepper eases the eye
   to the target, so a target cut short there stops the camera short: a unit
   centred near an edge would not be centred. Each block is replaced, not
   chased — its first instruction becomes a jump to a stub that clamps the
   target into the range in force and resumes at the block's own tail — so
   every target the engine computes lands on the same range the eye does.
   SetCamera `0x41C4C0` has a fourth of the same shape and needs nothing: no
   caller reaches it (CENTRE_BLOCK).

   MECHANISM for the eye: a `leaf_call` detour on `0x41C3C0`, on a flag that is
   always up once installed — the replacement does the whole job: the clamp,
   then the minimap rect the engine computes last, through the same wrapper.

   AND THE POINTER. `0x498DA0` hands a pointer OUTSIDE the viewport the world
   point `eye + clamp(pos, L, R) - L`, which on the side panel is `eye` itself
   and under the bottom bar is `eye + H - 1`, and a pointer over the void past
   the map edge names a point off the map as well. The chain from there is not
   defensive — `GetGridPosPLOT` returns NULL outside the map and
   `GetGridPosFeature 0x421E60` is handed it, which the always-installed
   guard there (tagpu_patches.c) answers with "no feature" — so the one call
   site that starts it is redirected and the world point clamped to the
   SCROLL EXTENT (zoom_tpos_guard), which keeps the hovered cell real. */

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
/* THE TWO SMOOTH CENTRING BLOCKS [DISASSEMBLED 2026-09-23]. Each smooth arm
   stores the new target and then clamps it inline; each block starts with
   `mov eax, ds:0x511DE8` (5 bytes), and the stub resumes at the function's
   tail — `mov eax, ds:0x511DE8` / `and word [eax+0x14281], 0xFFF7` / pops /
   `ret` — so the fog bit is cleared and the frame unwound by the engine's own
   bytes. The block's own exits do the same by other routes: a target clamped
   at the top returns early (`0x41C871`, `0x41C9A5`) after the same clear and
   the same pops, and the rest reach the tail. So the stub's one exit has the
   side effects of every exit it replaces.
   NO BRANCH LANDS IN A REPLACED BYTE (every rel8 and rel32 in .text was
   checked). Branches do land inside each block's address range: the
   non-smooth arm's `je` (`0x41C7F1`, `0x41C924`) targets `0x41C87A` /
   `0x41C9AE`, past the five bytes the jump takes, and that arm runs intact
   through its own `0x41C3C0` call to the same tail.
   SETCAMERA `0x41C4C0` HAS A THIRD SMOOTH ARM, `0x41C4EC`, AND IT IS LEFT
   ALONE BECAUSE IT IS UNREACHABLE: its four callers (`0x495C68`, `0x495E11`,
   `0x497060`, `0x4978C9`) all push smooth = 0 (`push 0` at `0x495C5E`,
   `0x495DCB`, `0x49703E`, `0x4978C5`), and `0x41C4C0` occurs nowhere in the
   image as a 32-bit value, so no pointer to it exists and `0x41C4C7`'s
   `je 0x41C574` is always taken. */
#define CENTRE_BLOCK     0x0041C808u   /* centre-on(x, y, smooth) 0x41C7C0        */
#define CENTRE_TAIL      0x0041C8BFu   /* ... pop esi; ret 0xC                    */
#define CENTOBJ_BLOCK    0x0041C93Bu   /* centre-on(object, smooth) 0x41C8E0      */
#define CENTOBJ_TAIL     0x0041C9F3u   /* ... pop edi; pop esi; ret 8             */
/* THE MAP DEBUG OVERLAY: stdcall(offscreen), ret 4, one call site. Its cell
   window starts at eye/16 with an upper bound only (`0x4183B9`, `0x4183D0`), so
   a negative eye reads before the feature grid and two others. */
#define DEBUGOVL_VA      0x00418310u
#define SITE_DEBUGOVL    0x00468DBAu   /* call 0x418310, inside DrawGameScreen    */
#define VA_GETTPOS       0x00484B50u   /* GetTPosition(x,y,out), stdcall, ret 0xC */
#define SITE_GETTPOS     0x00498EF9u   /* its call site inside 0x498DA0           */
#define OFF_EYEX         0x1431F
#define OFF_EYEY         0x14323
#define OFF_EXTENT_W     0x1422B       /* the SCROLL EXTENT: the map less 32 px   */
#define OFF_EXTENT_H     0x1422F       /* ...and less 128 px ("two sizes")        */
#define OFF_MAPPX_W      0x14223       /* the map's own pixel size, which         */
#define OFF_MAPPX_H      0x14227       /* GetTPosition clamps to (0x484B67)       */
#define OFF_PLOT_C       0x14233       /* the map in 16-px cells: the map is 16x  */
#define OFF_PLOT_R       0x14237
#define OFF_FIELD_W      0x37E37       /* the true viewport size: the W and H of  */
#define OFF_FIELD_H      0x37E3B       /* 0x41C3C0's [0, extent - W]; vpwide never writes them */
#define OFF_SCRTX        0x14327       /* MapXScrollingTo — the eye eases to here */
#define OFF_SCRTY        0x1432B
#define OFF_MM_RECT      0x142CB       /* the RECT 0x466B70 fills                 */
#define OFF_MM_DIRTY     0x142F1       /* bit 1: DrawMinimap 0x466B00 redraws     */
/* The three slots the camera FOLLOW lives in — see release_follow(). */
#define OFF_FOLLOW_OBJ   0x142F7       /* followed object, position at +0x4       */
#define OFF_FOLLOW_UNIT  0x142F3       /* followed unit, position at +0x6A        */
#define OFF_FOLLOW_HOLD  0x1434B       /* u16 frame countdown on main+0x1433F     */
#define OFF_LOSTYPE      0x14281       /* bit 3: the screen fog grid is current   */

/* `mov eax, ds:0x511DE8` — the whole first instruction, so the five stolen
   bytes end on an instruction boundary (0x41C3C5 is `push esi`). The three
   centring blocks start with the same instruction. */
static const unsigned char EYE_STOLEN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };

/* `mov esi,[eax+0x14327]` — six bytes, and they are never executed: the stub
   jumps past the whole clamp block to FOLLOWCLAMP_END. */
static const unsigned char FOLLOW_STOLEN[6] = { 0x8B, 0xB0, 0x27, 0x43, 0x01, 0x00 };

typedef void (__stdcall *PFN_GETTPOS)(int x, int y, int* out);

/* the eye clamp's leaf_call flag: up for good once installed, because the
   replacement is the camera's range at every zoom */
static volatile unsigned char g_clampOurs = 1;
static int  g_eyeInstalled;               /* the clamp and both guards went in */

static void zlog(const char* m)
{
    tagpu_log(m);
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

       out->left   = mmX + mmW * eyeX / extW          mm*  = main+0x142E7..0x142ED
       out->top    = mmY + mmH * eyeY / extH          ext* = the scroll extent,
       out->right  = left + mmW * viewCellsW * 16 / extW - 1   main+0x1422B/0x1422F
       out->bottom = top  + mmH * viewCellsH * 16 / extH - 1   (main+0x1423B/0x1423F)

   Its size therefore comes from the 1x view, which is exactly what is no longer
   true. Rather than reproduce any of that, let the engine fill the rect and
   scale the RESULT about its own centre by 1/z: at 0.5x the box on the minimap
   doubles, which is what the player is actually looking at.

   THE BOX CANNOT INVERT, BY CONSTRUCTION. The engine places it by the EXTENT,
   and the centre range lets the view centre reach the MAP's edge, 32 px right
   of and 128 px below the extent's ("two sizes") — so near the far edges the
   box's centre lies past the minimap, and at a high zoom the whole shrunken box
   does: an edge clamped on one side only then leaves top below bottom (at z 6
   on Two Continents), a box the GUI renderer drops and the engine's own drawer
   would put on the panel below the minimap. So the centre is clamped into the
   minimap FIRST, then each edge on BOTH sides. `hw`, `hh` >= 0, so the two
   edges leave `iround` in order, and a clamp to one interval is monotone, so
   they stay in order: left <= right and top <= bottom on every path. For an
   eye in the engine's own range at 1x every step is the identity (for a box
   at least a minimap pixel wide and tall; the float halves are exact).
   GAME THREAD: from the engine's own clamp through the two redirects, and from
   the apply below; the level it scales by is the one the last command carried. */
static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void __stdcall zoom_minimap_rect(int* r)
{
    const char* ta;
    float zm = game_level();
    float cx, cy, hw, hh;
    int mx, my, mw, mh;

    ((void (__stdcall *)(int*))MINIMAP_RECT_VA)(r);
    if (!r) return;

    cx = (float)(r[0] + r[2]) * 0.5f;
    cy = (float)(r[1] + r[3]) * 0.5f;
    hw = (float)(r[2] - r[0]) * 0.5f;
    hh = (float)(r[3] - r[1]) * 0.5f;
    if (zm > 0.05f && zm != 1.0f) { hw /= zm; hh /= zm; }
    if (hw < 0.0f) hw = 0.0f;           /* a 1x box under one minimap pixel */
    if (hh < 0.0f) hh = 0.0f;

    ta = *(const char* const*)TA_MAINPP;
    if (ta_ok(ta)) {
        mx = *(const short*)(ta + OFF_MM_X); my = *(const short*)(ta + OFF_MM_Y);
        mw = *(const short*)(ta + OFF_MM_W); mh = *(const short*)(ta + OFF_MM_H);
    } else {
        mw = mh = 0;
    }
    if (mw <= 0 || mh <= 0) {           /* no minimap to hold it: scaled only */
        r[0] = iround(cx - hw); r[2] = iround(cx + hw);
        r[1] = iround(cy - hh); r[3] = iround(cy + hh);
        return;
    }
    if (cx < (float)mx) cx = (float)mx;
    else if (cx > (float)(mx + mw - 1)) cx = (float)(mx + mw - 1);
    if (cy < (float)my) cy = (float)my;
    else if (cy > (float)(my + mh - 1)) cy = (float)(my + mh - 1);
    r[0] = clampi(iround(cx - hw), mx, mx + mw - 1);
    r[2] = clampi(iround(cx + hw), mx, mx + mw - 1);
    r[1] = clampi(iround(cy - hh), my, my + mh - 1);
    r[3] = clampi(iround(cy + hh), my, my + mh - 1);
}

/* THE CAMERA'S RANGE, the one arithmetic both threads use: the centre range
   `[-W/2, map - W/2]` when `centre`, else the engine's own `[0, extent - W]`
   ("two sizes"). Returns 0 — and leaves the outputs alone — when the inputs
   the chosen range needs are not sane enough to compute it. The centre range
   cannot invert (its width is the map's); the engine's inverts on a map
   smaller than the viewport, where the engine alternates between 0 and a
   negative bound on every call, and this one holds at 0. */
static int camera_range(int W, int H, int extW, int extH, int mapW, int mapH,
                        int centre, int* loX, int* hiX, int* loY, int* hiY)
{
    if (W <= 0 || H <= 0) return 0;
    if (centre) {
        if (mapW <= 0 || mapH <= 0) return 0;
        *loX = -(W / 2); *hiX = mapW - W / 2;
        *loY = -(H / 2); *hiY = mapH - H / 2;
    } else {
        if (extW <= 0 || extH <= 0) return 0;
        *loX = 0; *hiX = extW - W;
        *loY = 0; *hiY = extH - H;
        if (*hiX < 0) *hiX = 0;
        if (*hiY < 0) *hiY = 0;
    }
    return 1;
}

/* The map in world px from its size in 16-px PLOT cells; 0 (no centre range)
   for a count no map has, so the multiply cannot overflow. */
static int plot_px(int cells)
{
    return cells > 0 && cells < 65536 ? cells * 16 : 0;
}

/* The range in force on the GAME thread: the TRUE viewport (never the field:
   vpwide owns that one at zoom < 1) and the map, centred when the last apply
   said the ground is ours. HUD SCALE NEEDS NOTHING HERE (gui-renderer.md
   22.6): the viewport the engine clamps about IS the visible window. */
static int zoom_eye_range(const char* ta, int* loX, int* hiX, int* loY, int* hiY)
{
    int L, T, W, H;

    if (!ta_ok(ta)) return 0;
    tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
    return camera_range(W, H, *(const int*)(ta + OFF_EXTENT_W), *(const int*)(ta + OFF_EXTENT_H),
                        plot_px(*(const int*)(ta + OFF_PLOT_C)),
                        plot_px(*(const int*)(ta + OFF_PLOT_R)),
                        s_gCentre, loX, hiX, loY, hiY);
}

/* THE ENGINE'S OWN RANGE, FROM THE ENGINE'S OWN WORDS — what every clamp here
   falls back to when the range above cannot be computed, so that a clamp FAILS
   CLOSED. `0x41C3C0` clamps to `[0, extent - W]` from main+0x1422B/0x1422F and
   main+0x37E37/0x37E3B with no test of any of them [DISASSEMBLED]; this reads
   the same four words and holds the top at 0 where the engine's inverts. It
   always answers, and its range is inside the centre one, so falling back can
   only pull the eye in. It needs nothing of `ta` but that it is not NULL: the
   engine's own bytes that every caller replaces read main unconditionally. */
static void engine_range(const char* ta, int* loX, int* hiX, int* loY, int* hiY)
{
    int hx = *(const int*)(ta + OFF_EXTENT_W) - *(const int*)(ta + OFF_FIELD_W);
    int hy = *(const int*)(ta + OFF_EXTENT_H) - *(const int*)(ta + OFF_FIELD_H);

    *loX = 0; *hiX = hx > 0 ? hx : 0;
    *loY = 0; *hiY = hy > 0 ? hy : 0;
}

/* The range every game-thread clamp uses: the one in force, else the engine's.
   Returns whether it was the one in force. */
static int clamp_range(const char* ta, int* loX, int* hiX, int* loY, int* hiY)
{
    if (zoom_eye_range(ta, loX, hiX, loY, hiY)) return 1;
    engine_range(ta, loX, hiX, loY, hiY);
    return 0;
}

/* The same range from the packet's copies of the same inputs — the render
   thread's, for the cursor anchor's pre-clamp and the predicted eye. The
   packet's `vp` IS tagpu_vpwide_true_rect's answer, its `map_pxw/h` the
   scroll extent's two words, its `map_w16/h16` the PLOT grid's and its
   `cam_centre` the apply's own choice for the draw it was published from, so
   the two agree by construction and a step the render thread pre-clamps is
   accepted there. */
static int range_pk(const TAGPU_PACKET* p, int* loX, int* hiX, int* loY, int* hiY)
{
    return camera_range(p->vp[2], p->vp[3], p->map_pxw, p->map_pxh,
                        plot_px(p->map_w16), plot_px(p->map_h16),
                        p->cam_centre != 0, loX, hiX, loY, hiY);
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

/* The replacement for `0x41C3C0`. GAME THREAD. `arg` is the first stack slot
   of a function that takes no arguments — ignored.

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
    if (!ta) return;
    clamp_range(ta, &loX, &hiX, &loY, &hiY);
    clamp_pair((int*)(ta + OFF_EYEX), (int*)(ta + OFF_EYEY), loX, hiX, loY, hiY);
    /* the engine's own last act, and the only place this rect is recomputed */
    zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
}

/* The scroll target into the range in force — what each of the inline target
   clamps does in place of the engine's `[0, extent - W]`. GAME THREAD. On a frame
   whose engine state is not sane enough for the range in force, into the
   engine's own (clamp_range): never left unclamped. */
static void clamp_target(char* ta)
{
    int loX, hiX, loY, hiY;
    clamp_range(ta, &loX, &hiX, &loY, &hiY);
    clamp_pair((int*)(ta + OFF_SCRTX), (int*)(ta + OFF_SCRTY), loX, hiX, loY, hiY);
}

/* THE FOLLOW'S CLAMP. The per-frame stepper `0x41CA10` recomputes the scroll
   target from whatever is being followed (`want = unit - view/2`,
   `0x41CA95`..`0x41CAD2`) and then clamps it INLINE to `[0, extent - view]`.
   MEASURED 2026-09-12 at z = 8 (1920x1080, HUD 225%,
   view 1632x936, a 4064x3968 map, commander at world (3808,3600)): the follow
   asked for (2992,3132), the engine's clamp cut it to (2432,3032), and the
   visible window was 459 px short of the commander.

   Correcting it afterwards is not open to us: the stepper eases the eye toward
   the target and calls `0x41C3C0` only after, so a target corrected from our eye
   clamp would be overwritten before it was ever used. So the block is replaced
   instead of chased, which is also why it is the WHOLE block — the fog bit it
   clears at `0x41CB3B` is part of it, and cleared here. */
static void __cdecl zoom_follow_clamp(void)
{
    char* ta = *(char**)TA_MAINPP;

    if (!ta) return;                     /* the block's own bytes would fault */
    clamp_target(ta);
    /* `0x41CB3B`, the block's last act, done on every pass exactly as the
       block did: bit 3 of main+0x14281 is the screen fog grid's is-current
       flag and the target it was built for has just moved. This is the GAME
       thread -- the only one that may touch that word at all (exe map, "who
       may clear main+0x14281 bit 3"). */
    *(unsigned short*)(ta + OFF_LOSTYPE) &= (unsigned short)~8u;
}

/* The two smooth centring blocks: the target they just stored, clamped. The
   fog bit is the tail's (see CENTRE_BLOCK). */
static void __cdecl zoom_centring_clamp(void)
{
    char* ta = *(char**)TA_MAINPP;
    if (ta) clamp_target(ta);
}

/* pushfd ; pushad ; call fn ; popad ; popfd ; jmp resume. The flags are saved
   as well as the registers because this lands in the MIDDLE of a function
   rather than on a prologue. */
static unsigned char* build_block_stub(void (__cdecl *fn)(void), unsigned resume)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x9C;                                                /* pushfd  */
    *p++ = 0x60;                                                /* pushad  */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)fn); p += 4;
    *p++ = 0x61;                                                /* popad   */
    *p++ = 0x9D;                                                /* popfd   */
    *p++ = 0xE9; tagpu_detour_rel(p, resume); p += 4;           /* jmp     */
    return s;
}

/* The map debug overlay, called only for an eye stock TA can produce.
   `0x418310` starts its cell window at eye/16 and bounds only the far end
   (`0x4183B9`, `0x4183D0`), so an eye below 0 reads before the feature grid
   `main+0x14287` and two other grids [DISASSEMBLED 2026-09-23]. It draws
   nothing unless the debug view `main+0x14280` or the Contour command
   `0x511DD0` is on. With the eye inside `[0, extent - W]` it runs exactly as
   stock; outside it the overlay is skipped on EVERY draw, for as long as the
   eye stays there — which the centre range allows whenever the view is
   within W/2 (H/2) of a map edge — so a developer view shows no lines at all
   along the map's edges, and the game loses nothing. GAME THREAD, inside
   DrawGameScreen, where nothing writes the eye. */
static void __stdcall zoom_debug_overlay(void* ctx)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    int L, T, W, H, loX, hiX, loY, hiY, ex, ey;

    if (!ta_ok(ta)) return;
    tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
    if (!camera_range(W, H, *(const int*)(ta + OFF_EXTENT_W), *(const int*)(ta + OFF_EXTENT_H),
                      0, 0, 0, &loX, &hiX, &loY, &hiY))
        return;
    ex = *(const int*)(ta + OFF_EYEX);
    ey = *(const int*)(ta + OFF_EYEY);
    if (ex < loX || ex > hiX || ey < loY || ey > hiY) return;
    ((void (__stdcall *)(void*))DEBUGOVL_VA)(ctx);
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
   passes `cmp eax, ebp` and is then read through. The eye and the scroll
   target are written from this thread too, for the same reason plus one
   more: a torn COORDINATE would be bounded by `clamp_pair()`, but a lost
   update discards a camera move rather than delaying it.

   Called from the apply, on the draw that applies a NEW delta from a gesture
   that asked for the camera — the same ordering the engine's own scroll poll
   has: released, then the eye and the target stepped, in one thread, with no
   stepper between the two. The stepper is called from the frame callback
   before the draw call (0x495599 inside 0x495490, from 0x49680C/0x49693E;
   it is skipped when the sim is paused or an in-game GUI screen is up) and
   does not run again until the next frame callback; nothing inside
   DrawGameScreen stores the eye (the scenario camera is written by the apply
   itself, and releases nothing). A gesture that moves the eye by NOTHING —
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
   follow, move the eye and the target together, clamp, then the minimap box,
   the minimap's dirty bit and the fog flag. Nothing here waits, and nothing
   here can be torn: every word is written by this thread alone. */
static unsigned s_appliedSeq;              /* game thread only */
static int      s_appliedDx, s_appliedDy;
static unsigned s_epoch;                   /* bumped at every level end (game thread) */
/* the scenario camera's eye, owed to the next apply (tagpu_zoom_place_eye),
   and whether the apply runs at all (game thread only) */
static int      s_placeOn, s_placeX, s_placeY, s_applyLive;

void tagpu_zoom_apply(char* ta, const TAGPU_CMD* c, int terr)
{
    int* eye = (int*)(ta + OFF_EYEX);
    int* scr = (int*)(ta + OFF_SCRTX);
    int  loX, hiX, loY, hiY, moved = 0, applied = 0;

    /* THE LEVEL FIRST: every game-thread reader in this file — the minimap
       rect's scale, the scroll rate — uses the level the last record carried,
       never the render thread's own float. A frame that drew nothing zoomed
       posts live = 0, which is 1.0 here: the engine's own rect and rate come
       back at the next draw. */
    s_applyLive = 1;
    s_gLevel  = c ? c->zoom : 1.0f;
    s_gLive   = c ? (int)c->live : 0;
    /* THEN THE RANGE: centred only on a draw whose ground is ours. `terr` is
       the request the publisher latches for this same draw right after this
       call (`terr || rect still wide`), so the centre range is in force only
       where the engine's terrain pass is skipped — see "the camera's range".
       And only when it can be computed: a draw whose engine state is not sane
       enough for it takes the engine's own range and publishes `centre = 0`,
       so the eye is inside `[0, extent - W]` whoever draws the ground. */
    s_gCentre = g_eyeInstalled && terr;
    if (!clamp_range(ta, &loX, &hiX, &loY, &hiY)) s_gCentre = 0;

    /* A NEW RECORD: its delta is consumed exactly once — the difference
       between its cumulative sum and what was applied so far — and the
       follow is released on the same draw when the gesture asked. The
       render thread pre-clamped the step against the range of the draw it
       read, which is not a bound on this one: the engine's scroll poll and
       stepper run between the two, and the range itself can change (see the
       clamp below). The packet's eye reconciles the prediction. */
    /* A record of an OLDER epoch — posted before the render thread saw the
       level end — carries the old level's sum and applies nothing; the render
       thread's first record after seeing the new epoch starts from zero, as
       the applied sum did at the level end. */
    if (c && c->cmd_seq != s_appliedSeq && c->epoch == s_epoch) {
        int dx = c->cum_dx - s_appliedDx, dy = c->cum_dy - s_appliedDy;
        if (dx || dy) {
            if (c->drop_follow) release_follow(ta);
            eye[0] += dx; eye[1] += dy;
            /* the target moves with the eye, always: the two disagreeing is
               what the per-frame stepper reads as "a camera move is in
               flight", and it would drag the eye back and rebuild the fog
               grid every frame for as long as the disagreement lasted */
            scr[0] += dx; scr[1] += dy;
            moved = applied = 1;
        }
        s_appliedSeq = c->cmd_seq; s_appliedDx = c->cum_dx; s_appliedDy = c->cum_dy;
    }

    /* THE SCENARIO CAMERA, once: here and not where the scenario runs, so the
       draw that moves the eye is the draw that builds its fog grids — both
       grids in the packet this draw publishes are built at the eye it
       carries (tagpu_zoom_place_eye). */
    if (s_placeOn) {
        s_placeOn = 0;
        eye[0] = s_placeX; eye[1] = s_placeY;
        scr[0] = s_placeX; scr[1] = s_placeY;
        moved = applied = 1;
    }

    /* THE HOLD, a level: the camera is where tagpu_eye.txt says, every draw,
       clamped into the camera's range. Written only when it differs, so the
       invalidation below is paid only when the camera actually moved. */
    if (c && c->hold_on) {
        int x = c->hold_x, y = c->hold_y;
        clamp_pair(&x, &y, loX, hiX, loY, hiY);
        if (eye[0] != x || eye[1] != y) { eye[0] = x; eye[1] = y; moved = 1; }
        if (scr[0] != x || scr[1] != y) { scr[0] = x; scr[1] = y; }
        applied = 1;
    }

    /* THE RANGE IN FORCE, and this is the half of the bound that holds when
       the ground goes back to the engine: an eye the centre range allowed is
       walked into `[0, extent - W]` here, before the draw that would hand it to
       `0x483FA0`, which reads the map's grids from eye/16 with no bound of
       its own (its window over a view larger than the map is the
       engine-defect patch's to bound, "the camera's range").

       ON EVERY DRAW WHERE OUR CLAMP IS THE ENGINE'S (g_eyeInstalled), and on
       EVERY DRAW THAT APPLIED A DELTA OR A HOLD, WHATEVER IS INSTALLED. The
       second half is not covered by the first: the wheel and its cursor
       anchor need only the mouse->world repair, which `vpwide.on` installs
       without `zoom.on`, and on such a build the engine's own clamp has
       already run for this frame when the delta lands — an edge scroll that
       left the eye at `extent - H` plus a 20-px anchor step would reach
       `0x483FA0` unclamped. Here the range is the engine's (s_gCentre is 0
       without the eye clamp), so this is the engine's clamp, applied once
       more after the only writer that runs after it.

       It writes only when the eye or the target is actually outside the
       range: on the draw the ground changes hands (the centre range gives
       way to the engine's), after a delta the engine's scroll poll or
       stepper moved the eye under, and at no other time. The target is
       clamped rather than assigned the eye, which keeps a camera move that
       is genuinely in flight. */
    if (g_eyeInstalled || applied) {
        moved |= clamp_pair(eye, eye + 1, loX, hiX, loY, hiY);
        moved |= clamp_pair(scr, scr + 1, loX, hiX, loY, hiY);
    }

    /* A CAMERA WE MOVED OWES THE ENGINE WHAT ITS OWN WRITERS DO. Each of the
       eleven (exe map, `main+0x142F1` bit 1) stores the eye, sets bit 1 of
       main+0x142F1 (`orb $2`, e.g. `0x41C598`, `0x41D04D`), calls `0x41C3C0`
       — which only clamps the eye and recomputes the minimap's view box
       through `0x466B70`, touching neither flag nor the target — then copies
       the eye into the target and clears bit 3 of main+0x14281. So: the view
       box recomputed, `0x41C3C0` being the only place the engine ever fills
       it; bit 1 set, DrawMinimap `0x466B00`'s dirty flag, without which the
       engine's minimap is not redrawn and its box stays where it was until
       something else sets it; and bit 3 cleared,
       the screen fog grid's is-current flag, since that grid is
       view-anchored and rebuilt lazily off it. Both flags are written HERE,
       on the game thread, the only one that may: `0x484904` sets bit 3 and
       `0x466B16` clears bit 1 with UNLOCKED read-modify-writes on this thread,
       so a write from any other could be swallowed whole. The engine's own
       fog draw (or terrown's replica of its lazy rebuild) then rebuilds the
       grid inside THIS draw, for the commanded eye, and this draw's
       DrawMinimap redraws the box. */
    if (moved) {
        zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
        *(unsigned char*)(ta + OFF_MM_DIRTY) |= 2u;
        *(unsigned short*)(ta + OFF_LOSTYPE) &= (unsigned short)~8u;
    }

    /* and the scroll rate, every draw: base/z, or the base when nothing is
       zoomed — before the next frame's scroll poll reads it */
    apply_scroll_rate(ta);
}

void tagpu_zoom_level_end(char* ta)
{
    s_gLevel = 1.0f; s_gLive = 0;
    /* a new epoch: nothing owed to the old level survives into the next */
    s_epoch++;
    s_appliedSeq = 0; s_appliedDx = 0; s_appliedDy = 0;
    s_placeOn = 0;                        /* a placement is the old level's */
    s_applyLive = 0;
    /* THE GROUND GOES BACK TO THE ENGINE HERE (the publisher latches it down
       right after this call), so the eye and the target are walked into its
       own range first: nothing between this teardown and the next level's
       first apply may find an eye off `[0, extent - W]`. */
    s_gCentre = 0;
    if (!ta) return;
    if (g_eyeInstalled) {
        int loX, hiX, loY, hiY;
        clamp_range(ta, &loX, &hiX, &loY, &hiY);
        clamp_pair((int*)(ta + OFF_EYEX), (int*)(ta + OFF_EYEY), loX, hiX, loY, hiY);
        clamp_pair((int*)(ta + OFF_SCRTX), (int*)(ta + OFF_SCRTY), loX, hiX, loY, hiY);
    }
    if (ta_ok(ta)) apply_scroll_rate(ta);                     /* the player's own value, for the options screen */
}

/* THE SCENARIO CAMERA (tagpu_scenario.c's place_camera). GAME THREAD, from
   the flip observer, which runs at any of the flip's 44 call sites behind a
   16-ms clock gate — mid-draw inside DrawGameScreen (`0x46A3DB`, after its
   fog site) or outside it (`0x467E41`, the HUD panel painter's, and 42 more)
   — so the eye is NOT written here. It is clamped into the range in force
   (the last apply's, the same the terrain latch holds; the engine's own where
   it cannot be computed) and handed to the next apply, which writes it at the
   top of the next in-play draw, before that draw's first read of the eye and
   before its fog site builds either grid: the packet that draw publishes
   carries the new eye with both grids built at it, and the apply owes the
   engine what every eye writer does (the minimap box, its dirty bit, the fog
   flag). The apply clamps once more with its own range. Where the apply does
   not run at all (the packet publisher count-only: nothing of ours draws the
   world, so no grid can disagree with the eye) the camera is written here,
   the same way. The eye the apply will write goes to outX, outY. Returns 0
   and writes nothing when main is not sane. */
int tagpu_zoom_place_eye(char* ta, int x, int y, int* outX, int* outY)
{
    int loX, hiX, loY, hiY;

    if (!ta_ok(ta)) return 0;
    clamp_range(ta, &loX, &hiX, &loY, &hiY);
    clamp_pair(&x, &y, loX, hiX, loY, hiY);
    if (s_applyLive) {
        s_placeX = x; s_placeY = y; s_placeOn = 1;
    } else {
        *(volatile int*)(ta + OFF_EYEX)  = x;
        *(volatile int*)(ta + OFF_EYEY)  = y;
        *(volatile int*)(ta + OFF_SCRTX) = x;
        *(volatile int*)(ta + OFF_SCRTY) = y;
        zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
        *(unsigned char*)(ta + OFF_MM_DIRTY) |= 2u;
        *(unsigned short*)(ta + OFF_LOSTYPE) &= (unsigned short)~8u;
    }
    if (outX) *outX = x;
    if (outY) *outY = y;
    return 1;
}

void tagpu_zoom_applied(unsigned* seq, int* cum_dx, int* cum_dy, float* level,
                        unsigned* epoch, unsigned* centre)
{
    *seq = s_appliedSeq; *cum_dx = s_appliedDx; *cum_dy = s_appliedDy;
    *level = game_level(); *epoch = s_epoch; *centre = s_gCentre ? 1u : 0u;
}

/* ---- the end of the render frame: the command record ----------------------- */

/* the cursor anchor's state, render thread only (below) */
static int s_cumX, s_cumY;                 /* the cumulative eye delta posted, world px */
static int s_ackX, s_ackY;                 /* ...and what the last packet acknowledged  */
static unsigned s_epochSeen;               /* the cmd_epoch of the last packet seen     */

void tagpu_zoom_frame_end(void)
{
    TAGPU_CMD rec;
    if (!s_fresh) s_live = 0;
    s_fresh = 0;
    /* THE RECORD: this frame's level and whether a zoomed world is on screen
       (the game thread derives the addressable rect, the minimap box and the
       scroll rate from the pair), the anchor's cumulative delta, whether any
       of it is still unacknowledged (the follow release rides that, so a
       record the game thread never took loses nothing: the next one carries
       the same request), and the camera hold. Posted on EVERY path out of the
       overlay frame, so a frame that drew nothing zoomed hands the engine its
       own rect and rate back at the next in-play draw. */
    memset(&rec, 0, sizeof rec);
    rec.zoom        = s_zoom;
    rec.live        = s_live ? 1u : 0u;
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

/* F2/Tab push ARMOPT and then set main+0x37EBE bit 0; the in-game preferences
   path sets the same bit. It stays set through EXITMENU, YESORNO and the
   preferences screens, and the measured Resume/close paths clear it. The
   executable has more clear sites, but their full screen mapping is not
   established. This bit is NOT a general modal flag: SHARE.GUI uses bit 6.
   DrawGameScreen continues underneath those screens, so s_live quite rightly
   remains set even though the GUI now owns every button event. Read the
   engine state here, at the one shared s -> u door, so hardware messages,
   injected clicks and the mouse->world repair all make the same decision.

   The executable mapping and TA_MAINPP live for the process. `view()` has
   already proved that an in-game world was published; ta_ok still guards the
   level pointer before the byte read. */
static int options_gui_owns_input(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    return ta_ok(ta) && (*(const unsigned char*)(ta + OFF_GUI_FLAGS) & 1u) != 0;
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

   Nothing about the CURSOR is decided here. The engine is told the
   truth by fake_GetCursorPos and draws its sprite under the pointer; this
   transform reaches it only through a button message and through the
   mouse->world repair in tagpu_vpwide.c. */
static int to_engine(int* x, int* y, int* ring)
{
    float z, cx, cy; int L, T, W, H, ux, uy;
    if (ring) *ring = 0;
    if (!x || !y || !view(&z, &cx, &cy, &L, &T, &W, &H)) return 0;
    if (options_gui_owns_input()) return 0;             /* GUI space: 1:1 */
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

   A MOVE MUST NOT BE REWRITTEN. `0x4B5E51` does not queue: it copies its
   record into `[obj+0x196]` (`0x4C2360`), which is both the dispatch's
   fallback record AND the position the engine draws its cursor from on the
   path that does not poll — `0x4C67C0`, called twice out of the surface
   present machinery. A rewritten move therefore puts `u` where the sprite is
   read from, and whichever of the message and the next GetCursorPos poll ran
   last decides where the cursor appears — measured at 1920x1080 / 0.25x: the
   sprite tracks `u` across the frame at 4x the pointer's speed and off the
   left edge. The move's own position is not needed as `u` by anything: the
   world point is recomputed from the pointer in vpw_mouse_world()
   (tagpu_vpwide.c), which is where it is actually used.

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
    /* THE RENDER-OPTIONS PANEL IS OVER THE WORLD. Built-in GUI screens are
       covered by the engine ownership gate in to_engine(); this DLL-owned panel
       has no such engine bit, so tagpu_menu answers for its own screen-space
       rectangle. */
    if (tagpu_menu_owns_point(x, y)) return lparam;
    if (!tagpu_zoom_to_engine(&x, &y)) return lparam;
    return MAKELPARAM((short)x, (short)y);
}

/* ---- zoom to the cursor ----------------------------------------------------

   Zooming IN holds the world point under the POINTER still, instead of the
   one at the centre of the screen; zooming OUT pulls straight back and holds
   the centre (BAR's CamSpringZoomOutFromMousePos off). The transform cannot
   hold the pointer's point: it is a similarity about the viewport centre and
   nothing in it is free. What is free is the engine's eye, because the world
   on screen is

       W(s) = eye + vw/2 + (s - c) / z

   so holding W(s) fixed across a change in z is one subtraction:

       d = (a - c) * (1/z_prev - 1/z_now)          a = where the notch was aimed

   applied to the eye. THE DELTA IS EXACT, not an approximation of one: it is
   the difference of two exact solutions, so the constants (the anchored world
   point, vw/2) cancel and never appear. It is taken per NOTCH, on the notch's
   own point and on the TARGET distances before and after it, and paid out
   along the tween's ease as R ("the wheel") — so two notches aimed at two
   points each hold their own, and a notch that cuts a tween cannot re-aim
   the displacement the cut tween still owed.

   WHY A DELTA AND NOT A SOLVED POSITION. The absolute form — keep the anchored
   world point W* and set the eye from it every frame — is algebraically the
   same thing, but it ASSERTS the eye on every frame of the tween and so
   overwrites any other camera source for as long as a gesture lasts. The delta
   composes with them instead: an edge scroll, an arrow key or a camera move
   already in flight is preserved, because we add to whatever the eye is rather
   than declaring what it should be. And when z is not moving the delta is
   exactly zero, so at steady state — which is almost every frame — this
   function posts NOTHING new and there is no interference to reason about.

   HOW IT REACHES THE EYE. The render thread never writes the eye.
   The step is added to a CUMULATIVE sum that rides the command record; the
   game thread applies the difference from what it applied last, at the top of
   its next in-play draw, releasing the follow first when the gesture asked —
   consumed exactly once, accumulated if the game thread is slow, and applied
   after whichever of the engine's own camera writers ran that frame and
   BEFORE the draw reads the eye (nothing inside DrawGameScreen stores it), so
   the frame, its fog rebuild and its minimap box all see the commanded
   camera. Meanwhile this frame is drawn from the PREDICTED eye: the
   packet's eye plus every delta not yet acknowledged (predict below), so the
   picture moves on the frame of the notch and the next packet, carrying the
   same delta applied, replaces the prediction with the truth — no wobble. The
   step is pre-clamped here against the same camera range the game thread
   applies, computed from the packet's copy of the same two fields, so what is
   posted is what will be accepted; the one case the game thread refuses more —
   the engine scrolled the eye to the edge between the two — is clamped there
   and reconciled by the packet like any other engine camera move, and the
   pre-clamp never posts a pull-back of its own for it (step_axis).

   TWO PROPERTIES FALL OUT OF THE DELTA FORM, and the tests lean on both.
   With the pointer at the viewport centre `a - c` is zero, so R stays exactly
   zero, the eye never moves and the behaviour is that of a centre-anchored
   zoom, bit for bit — that is the A/B control, and it is why this needs no
   lever. The posts TELESCOPE, so the total displacement over a gesture is
   the sum over its notches in of `(a_i - c)(iz_before_i - iz_after_i)` on the
   targets, however many frames the tweens took and whatever the frame timing
   was — an exact oracle on `main+0x1431F` with `tacli peek`, and the same
   final pose the lab reaches. A notch out adds nothing of its own: one that
   cuts a zoom-in's tween lets the zoom-in's displacement finish, and
   in-then-out leaves the eye where the zoom-in put it.

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
   arrives, and its grids were built about the packet's eye: the fog bound
   (predict) takes the engine's grid only where it spans the view and
   otherwise the wide one, clamping the drawn eye into it. Nothing here
   depends on terrown owning the fog draw: the invalidation is the engine's
   own mechanism.

   A FOLLOWED CAMERA IS RELEASED RATHER THAN FOUGHT. The stepper recomputes the
   scroll target from the followed unit every frame (`0x41CAF7`) and clamps it
   inline, so a delta added to the eye is eased straight back out: the zoom
   would read as pinned to the unit. It is not what following means — the
   engine's own edge scroll releases the follow the moment it moves the eye,
   and so does the draw that applies a step here (release_follow, on the game
   thread, in the same apply). A gesture that moves the eye by NOTHING — the
   pointer on the viewport centre — releases nothing, so the A/B control still
   holds exactly.

   A SMOOTH CENTRING ISSUED AFTER A STEP WINS. The two centre-ons' smooth
   arms (`0x41C7C0`, `0x41C8E0`) set the scroll
   target once, from their own point and without our delta, and the stepper
   eases the eye to it. A step applied while one is in flight moves the eye
   and the target together, so the centring's destination moves with it.
   Neither is a standing state the way a follow is — each is one camera move,
   and the zoom composes with the next one — so nothing fights and nothing
   churns. */

/* The sub-world-pixel carry. Render thread only: the same thread that owns
   the level itself. */
static float s_residX, s_residY;

/* THE CARRY'S INVARIANT, kept at every exit that does not step the eye. A
   frame that moves the eye leaves at most half a world pixel behind, by
   construction, so at rest `|resid| <= 0.5` and anything above it is
   displacement that was owed and never taken — which nothing else would ever
   spend (every later frame at rest returns before stepping) until the next
   notch anywhere on the map discharged it in one frame as a silent camera
   jump. */
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

/* One axis of the anchor's step from the predicted position `p`, pre-clamped
   into [lo, hi]; `*cut` says the clamp took some of it.
   NEVER A PULL-BACK. A prediction already past the range carries a delta the
   game thread is about to clamp — the engine's scroll poll moved the eye
   under it, which is exactly the race the apply's clamp exists for — and
   posting the difference as well would take the overshoot off twice, so the
   eye would stop a step short of the edge. MEASURED 2026-09-23 (vpwide.on
   without zoom.on, notches in below the centre while the pointer
   edge-scrolled to the bottom stop): a +10 the apply clamped at the stop was
   followed on the next draw by a posted -10, applied to an eye already at
   the stop. So the bounds widen to include `p`: a step never moves the eye
   further out than the range, and never back in on its own account. */
static int step_axis(int p, int n, int lo, int hi, int* cut)
{
    int q   = p + n;
    int top = hi > p ? hi : p;
    int bot = lo < p ? lo : p;

    if      (q > top) { q = top; *cut = 1; }
    else if (q < bot) { q = bot; *cut = 1; }
    return q;
}

/* Render thread, once a frame, from read_lever() and BEFORE any pass reads the
   eye, after wheel_level() has taken this frame's notches. `fromWheel` is false
   while the file lever is in force: it changes z with no gesture behind it and
   every zoom fixture drives it, so it must not move the camera (the pin has
   already zeroed R). */
static void anchor_step(int fromWheel, const TAGPU_PACKET* pk)
{
    double g = s_tweenG, dg;
    int    wantX, wantY, nx, ny, loX, hiX, loY, hiY, px, py, qx, qy, cutX = 0, cutY = 0;

    /* A NOTCH: what the tween it cut had not posted yet, plus the notches'
       own terms ("the wheel"). The new tween starts at g = 0. */
    if (s_batch) {
        s_remX = s_remX * (float)(1.0 - s_gPosted) + s_shiftX;
        s_remY = s_remY * (float)(1.0 - s_gPosted) + s_shiftY;
        s_gPosted = 0.0;
    }
    /* NOTHING OWED — the common case, and every zoom-out that cut no zoom-in.
       Exactly zero, not "small": a notch aimed at the viewport centre adds
       0 * k, so the centred gesture never reaches the lines below, which is
       what makes the A/B control structural. */
    if (s_remX == 0.0f && s_remY == 0.0f) {
        s_gPosted = g;
        drop_claim();
        return;
    }
    /* No eye to step, or a camera something else is driving: the gesture's
       displacement is dropped, not banked for a later frame to land as a jump. */
    if (!fromWheel || !anchor_allowed() || (int)s_vw <= 0 || (int)s_vh <= 0 ||
        !pk || !pk->in_game) {
        s_remX = s_remY = 0.0f;
        s_residX = s_residY = 0.0f;
        s_gPosted = g;
        return;
    }

    /* this frame's share of R, into the carry */
    dg = g - s_gPosted;
    s_gPosted = g;
    s_residX += s_remX * (float)dg;
    s_residY += s_remY * (float)dg;
    /* AN AXIS THE GESTURE OWES NOTHING ON IS NEVER STEPPED. `resid` carries
       the last gesture's rounding, and a carry at exactly -0.5 rounds to -1
       under iround with nothing feeding it: without this, a notch whose
       point is level with the centre would still step the eye a pixel along
       that axis (and release the follow). */
    wantX = s_remX != 0.0f;
    wantY = s_remY != 0.0f;
    if (g >= 1.0) s_remX = s_remY = 0.0f;           /* the tween has landed: all posted */
    nx = wantX ? iround(s_residX) : 0;
    ny = wantY ? iround(s_residY) : 0;
    if (!nx && !ny) return;                         /* sub-pixel: carried, not lost */

    /* THE RANGE FIRST, AND NOTHING IS POSTED WITHOUT ONE — from the packet's
       copy of the map size, the true viewport and the range the game thread
       chose for that draw. A frame whose packet carries no sane range (a
       level change with the view still live) posts nothing and keeps no more
       than the carry: an unclamped step would otherwise wait in the sum for
       the next sane frame and land as one jump. */
    if (!range_pk(pk, &loX, &hiX, &loY, &hiY)) { drop_claim(); return; }

    /* the step, taken against the eye this frame is drawn from — the packet's
       plus what is already posted and unacknowledged — and pre-clamped into
       the range PER AXIS (step_axis). An axis the clamp cuts drops what the
       gesture still owes on it, residual included, rather than banking it
       against the way back out; the other axis carries on. */
    px = pk->eye[0] + (s_cumX - pk->cmd_ack_dx);
    py = pk->eye[1] + (s_cumY - pk->cmd_ack_dy);
    qx = step_axis(px, nx, loX, hiX, &cutX);
    qy = step_axis(py, ny, loY, hiY, &cutY);
    s_residX -= (float)nx;
    s_residY -= (float)ny;
    if (cutX) { s_remX = 0.0f; s_residX = 0.0f; }
    if (cutY) { s_remY = 0.0f; s_residY = 0.0f; }
    s_cumX += qx - px;
    s_cumY += qy - py;
}

/* The eye interval that puts `[e + dL, e + dR]` inside `[lo, hi]`, and `*e`
   clamped into it; returns 1. An empty interval centres the span on
   `[lo, hi]` and returns 0. */
static int fog_fit(int* e, int dL, int dR, int lo, int hi)
{
    int a = lo - dL, b = hi - dR;
    if (a > b) { *e = a + (b - a) / 2; return 0; }
    *e = clampi(*e, a, b);
    return 1;
}

/* 1 when the engine's grid in this packet reaches more than one cell past the
   map on some side. Its builder writes the four border completions on the
   grid's literal first and last-but-one rows and columns, and those are the
   entries straddling the map edge only while its window overshoots the map by
   at most one cell (exe map, "The four border completions": left and top
   need `col0 >= -1`, right and bottom `col0 + cols <= PLOT_C/2 + 1`). Past
   that the completions land off the map and a fogged map edge fades to lit
   across its last half cell; the wide grid derives the straddling index
   instead. Judged on the grid's OWN window — the origin `32 col0 + 16` and the
   size the packet carries with it — and never on the packet's eye, which is
   not the eye the grid was built at whenever something moved the camera
   between the two. The map's size is the PLOT grid's (`map_w16/h16`, 16-px
   cells), halved into the fog lattice's 32-px cells — not the scroll extent
   `map_pxw/h`, which is 32 px narrower and 128 px shorter and would refuse
   the grid the engine builds at the bottom of its own range. */
static int engine_grid_misplaced(const TAGPU_PACKET* pk)
{
    /* `org - 16` is a multiple of 32 on either sign, so the division is exact */
    int c0 = (pk->fog_org[0] - 16) / 32, r0 = (pk->fog_org[1] - 16) / 32;
    int w = pk->map_w16 / 2, h = pk->map_h16 / 2;       /* PLOT_C/2, PLOT_R/2 */
    return c0 < -1 || c0 + pk->fog_cols > w + 1 ||
           r0 < -1 || r0 + pk->fog_rows > h + 1;
}

/* THE FOG BOUND ON THE DRAWN EYE. The invariant: every fog sample this frame
   takes from a grid lies inside that grid's fully written cells — the sample
   of every drawn pixel (taFog, at the world point under it), and on the wide
   grid every CPU sample (tagpu_fog_at): the unit and wreck gathers test the
   point they sample against the slab below, and the feature, effect and
   particle gate moves a point in the grid's unwritten last column or row
   onto its written edge and answers every other point where it lies
   (tagpu_fx_tile_visible). It rests on a bound and not on the game thread
   keeping up: the domain comes from this frame's level and viewport, the
   grid's span from the packet's own numbers, and the eye is clamped until one
   lies inside the other, for any gesture, reversal or lag.

   THE DOMAIN, per axis (vh for y). The drawn pixels: every world pass is
   scissored to the viewport, so a fragment's world point lies in the view
   `e + vw/2 +- (vw/2)/z`, inside the 1x rect `[e, e + vw]` at z >= 1. The
   slab: `[e + (vw - S)/2 - M, e + (vw - S)/2 + S + M]`, S =
   tagpu_zoom_gather_span(vw, z) and M = TAGPU_GATHER_MARGIN — the native
   pass's own test, whose S is this one or shorter (its terrain reservation
   only trims), which only narrows the slab about the same centre; it
   contains the view.
   THE SPAN, the same for both grids: `[org, org + 32 (cols - 1)]`. The last
   column of any grid is short its right corners (the builder fills entry gx
   from cells gx and gx + 1), so a point past that edge would read corners
   nobody wrote.

   THE CHOICE. The engine's grid when the level is at least 1, its window is
   the engine's own construction (engine_grid_misplaced) and the 1x rect
   about the drawn eye lies in its span — true for the eye it was built at
   wherever the view is a multiple of 32 (30 x 24 cells for the 896 x 704
   view at 1024x768: `view/32 + 2`, MEASURED 2026-09-24), false for an anchor
   step it has not seen, and false at the build eye itself for a view that is
   not (1016 rows at 1080p reach into the short last row). Otherwise the wide
   grid, with the eye clamped so the slab lies in its span. That interval
   holds every eye within the lead of the one the grid was built about, at
   every level, since fogw_window cuts the slab at the floor about that eye
   grown by the lead (tagpu_zoom_pub_window) and S never exceeds the floor's,
   so it is empty only for a grid built for another viewport or trimmed by a
   failed allocation. The clamp is the identity for every gesture that only
   zooms in (fogw_window's argument), and for any displacement the game
   thread has not applied that is within the lead; what it holds back is a
   larger one, and the drawn eye waits at the edge of what the grid covers
   for as many frames as that takes, instead of drawing past it. The
   prediction runs one posted step ahead of the game thread by design, and
   the lead is sized to absorb that step (gpu-status §2.3e has the
   numbers). The clamp reads the grid's own origin and size, never the lead:
   a grid a failed allocation trimmed is bounded by what it holds. While a
   frame is held, a click is still mapped by the game
   thread through the eye it applies, up to the hold from where it is drawn.
   With no wide grid in the packet (`fogwide.off`, a failed build) a frame
   whose engine grid qualifies but for the 1x rect is held into that grid; any
   other such frame — below 1x, or on a misplaced engine grid — is drawn BARE,
   unclamped over the engine's grid, and the native pass counts it. An empty
   interval centres the domain on the grid. The native pass also counts any
   frame whose domain is not inside its grid, as the bound's witness.
   Returns 1 when the frame is to sample the wide grid; `*fit` is 0 when an
   interval it clamped into was empty and the domain was centred instead. */
static int fog_bound(const TAGPU_PACKET* pk, float z, int* ex, int* ey, int* fit)
{
    int vw = pk->vp[2], vh = pk->vp[3], x = *ex, y = *ey;
    int engine = z >= 1.0f && pk->fog_cols > 0 && pk->fog_rows > 0 &&
                 !engine_grid_misplaced(pk);

    *fit = 1;
    if (vw <= 0 || vh <= 0) return 1;
    if (engine) {
        int ox = pk->fog_org[0], oy = pk->fog_org[1];
        int hx = ox + 32 * (pk->fog_cols - 1), hy = oy + 32 * (pk->fog_rows - 1);
        if (x >= ox && x + vw <= hx && y >= oy && y + vh <= hy) return 0;
        if (!tagpu_pk_fogw(pk)) {
            *fit = fog_fit(&x, 0, vw, ox, hx) & fog_fit(&y, 0, vh, oy, hy);
            *ex = x; *ey = y;
            return 0;
        }
    }
    if (tagpu_pk_fogw(pk)) {
        int sw = tagpu_zoom_gather_span(vw, z), sh = tagpu_zoom_gather_span(vh, z);
        int dLx = (vw - sw) / 2 - TAGPU_GATHER_MARGIN, dRx = dLx + sw + 2 * TAGPU_GATHER_MARGIN;
        int dLy = (vh - sh) / 2 - TAGPU_GATHER_MARGIN, dRy = dLy + sh + 2 * TAGPU_GATHER_MARGIN;
        *fit = fog_fit(&x, dLx, dRx, pk->fogw_org[0], pk->fogw_org[0] + 32 * (pk->fogw_cols - 1)) &
               fog_fit(&y, dLy, dRy, pk->fogw_org[1], pk->fogw_org[1] + 32 * (pk->fogw_rows - 1));
        *ex = x; *ey = y;
    }
    return 1;
}

/* 1 when the drawn eye `d` steps away from `prev` against the gesture, which
   puts this frame's eye at `u`: toward the gesture is toward `u`. */
static int against(int d, int u, int prev)
{
    return (u >= prev && d < prev) || (u <= prev && d > prev);
}

/* the eye and level the LAST frame was drawn with, for the monotone rule
   (render thread) */
static int   s_drawnX, s_drawnY, s_haveDrawn;
static float s_drawnZ;

/* The eye this frame is drawn from: the packet's, plus the deltas the game
   thread has not applied yet, clamped into the range in force and then by
   the fog bound. `z` is the level the levers ask for; the level to draw at is
   returned.

   THE DRAWN VIEW NEVER MOVES AGAINST THE GESTURE, on either axis. The bound's
   upper end `org + 32 (cols - 1) - (vw + S)/2 - M` falls as the level falls
   (S grows), so by itself it walks a held eye backward frame by frame during
   a zoom-out that cuts a zoom-in whose displacement the game thread has not
   applied, and snaps it forward when a packet lands — a sawtooth, one tooth
   per packet (gpu-status §2.3 has the numbers). When the bound would step
   the eye away from where the gesture puts it, the frame is drawn at the
   LAST frame's level instead, so the zoom pauses with the eye; the eye then
   moves only toward the gesture. The grid is chosen and the eye clamped for
   that level by the same bound, so the fog invariant is untouched: the
   monotone rule only picks which level the bound is evaluated at.
   Why the last level is enough: for one packet its interval at that level is
   the one that last frame's eye was clamped into, so that eye is still in
   it, and every eye between it and `u` is too. A NEWER packet's wide
   interval at a level is non-decreasing in the eye it was built about (the
   window is that eye's floor slab snapped down to the 32-px lattice, its
   size fixed by the viewport), so it still holds last frame's eye unless the
   grid's own eye moved against the gesture — the game thread applying a
   step of another gesture, or its own camera writers — or the grid was
   trimmed by a failed allocation. Only then is the step taken anyway, and
   counted (`back=` in the native heartbeat): the fog invariant is the one
   that cannot give.
   THE RULE NEEDS AN INTERVAL. Where the one at this level is empty — a grid
   trimmed narrower than the slab — the bound centres the domain on the grid,
   and a centred eye is not a step the gesture asked for in either direction:
   pausing on it would hold the last level for as long as the trim lasted. So
   the rule is applied only when the bound fitted the eye into a non-empty
   interval, and the retry is taken only when it did too; a centred frame is
   drawn as the bound left it and counted by the native pass (`out=`). */
static float predict(const TAGPU_PACKET* pk, float z, int mayHold)
{
    int ux, uy, ex, ey, loX, hiX, loY, hiY, wide, fit;
    s_havePred = 0; s_fogWide = 0;
    if (!pk) { s_haveDrawn = 0; return z; }
    /* A NEW EPOCH — the level ended on the game thread, which reset what it
       had applied to zero: the sum posted from here on starts from zero too,
       and whatever was owed to the old level (a notch in its last frames, a
       carry) dies with it. The level-end packet carries the new epoch, and so
       does every packet of the next level, so this is seen at the latest on
       the new level's first frame — before any record of it could carry a
       delta the game thread would accept. */
    if (pk->cmd_epoch != s_epochSeen) {
        s_epochSeen = pk->cmd_epoch;
        s_cumX = s_cumY = 0; s_ackX = s_ackY = 0;
        s_residX = s_residY = 0.0f;
        s_remX = s_remY = 0.0f;
        s_haveDrawn = 0;
    }
    if (!pk->in_game) { s_haveDrawn = 0; return z; }
    s_ackX = pk->cmd_ack_dx; s_ackY = pk->cmd_ack_dy;
    ux = s_cumX - s_ackX; uy = s_cumY - s_ackY;
    ex = pk->eye[0] + ux; ey = pk->eye[1] + uy;
    if (range_pk(pk, &loX, &hiX, &loY, &hiY))
        clamp_pair(&ex, &ey, loX, hiX, loY, hiY);
    ux = ex; uy = ey;
    /* LAST, so nothing moves the eye after the bound is established */
    wide = fog_bound(pk, z, &ex, &ey, &fit);
    if (mayHold && fit && s_haveDrawn &&
        (against(ex, ux, s_drawnX) || against(ey, uy, s_drawnY))) {
        int hx = ux, hy = uy, hfit, hw = fog_bound(pk, s_drawnZ, &hx, &hy, &hfit);
        if (hfit && !against(hx, ux, s_drawnX) && !against(hy, uy, s_drawnY)) {
            ex = hx; ey = hy; wide = hw; z = s_drawnZ;
            s_fogPaused++;
        } else {
            s_fogBack++;
        }
    }
    if (ex != ux || ey != uy) {
        int d = abs(ex - ux) > abs(ey - uy) ? abs(ex - ux) : abs(ey - uy);
        s_fogHeld++;
        if (d > s_fogHeldMax) s_fogHeldMax = d;
    }
    s_fogWide = wide;
    s_predX = ex; s_predY = ey; s_havePred = 1;
    s_drawnX = ex; s_drawnY = ey; s_drawnZ = z; s_haveDrawn = 1;
    return z;
}

/* GetTPosition on the world point under the mouse, clamped to the SCROLL
   EXTENT — see "the camera's range" for why the centre range makes that
   necessary. The extent and not the map, and that is the safety argument:
   GetTPosition's answer lies below `(y & ~15) + 144` (a 128-px search down
   from `y & ~15`, 0x484B94..0x484B9B, plus the interpolation's one cell),
   and with `y <= extent - 1 = mapH - 129` that is below
   `(mapH - 144) + 144 = mapH`, on the map — the extent's 128-px bottom margin
   is exactly its search window (exe map, "Engine defects we patch"). A clamp
   to the map's own size would leave 128 px in which it answers off the map.
   Since the always-installed guard on 0x421E60 (tagpu_patches.c) a NULL plot
   answers "no feature" instead of faulting, so this clamp keeps the hovered
   cell and feature real; it is no longer what keeps the game alive.

   IT FAILS CLOSED WHEREVER THERE IS A MAP TO CLAMP AGAINST. The bound is the
   extent; where a size of the extent is not positive, the map's own pixel
   size main+0x14223/0x14227 less the level load's margins (32, 128) stands in
   for it; and `y` is held to `mapH - 129` as well — the search window itself,
   which the extent equals at the level load and which the debug `Edge`
   command can loosen. The point passes through untouched only when there is
   nothing to clamp against: main not sane (GetTPosition reads main at
   0x484B61 unconditionally, as every engine caller of it does) or no map size
   positive (no map loaded; GetTPosition clamps to the map's size itself).
   A no-op for every eye in the engine's own range, which cannot name a point
   past the extent. */
static int tpos_bound(int extent, int map, int margin)
{
    if (extent > 0) return extent - 1;
    return map > margin ? map - margin - 1 : -1;       /* -1: no bound */
}

static void __stdcall zoom_tpos_guard(int x, int y, int* out)
{
    const char* ta = *(const char* const*)TA_MAINPP;

    if (ta_ok(ta)) {
        int mw = *(const int*)(ta + OFF_MAPPX_W);
        int mh = *(const int*)(ta + OFF_MAPPX_H);
        int hx = tpos_bound(*(const int*)(ta + OFF_EXTENT_W), mw, 32);
        int hy = tpos_bound(*(const int*)(ta + OFF_EXTENT_H), mh, 128);
        if (mh > 128 && (hy < 0 || hy > mh - 129)) hy = mh - 129;
        if (hx >= 0) x = clampi(x, 0, hx);
        if (hy >= 0) y = clampi(y, 0, hy);
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
    static const unsigned blocks[2][2] = {
        { CENTRE_BLOCK,  CENTRE_TAIL  },
        { CENTOBJ_BLOCK, CENTOBJ_TAIL },
    };
    unsigned char* stubs[3];
    int ok, i, built;

    if (!tagpu_opt_on("tagpu_zoom.on")) return;

    /* Every byte is checked before any of them is written, so a patched or
       different exe arms nothing rather than half of it. */
    if (!site_is(SITE_MMRECT1_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_MMRECT2_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_SAVESCROLL, SAVE_SETTING_VA) ||
        !site_is(SITE_GETTPOS, VA_GETTPOS) ||
        !site_is(SITE_DEBUGOVL, DEBUGOVL_VA) ||
        !bytes_are(EYECLAMP_VA, EYE_STOLEN, (int)sizeof EYE_STOLEN) ||
        !bytes_are(CENTRE_BLOCK, EYE_STOLEN, (int)sizeof EYE_STOLEN) ||
        !bytes_are(CENTOBJ_BLOCK, EYE_STOLEN, (int)sizeof EYE_STOLEN) ||
        !bytes_are(FOLLOWCLAMP_VA, FOLLOW_STOLEN, (int)sizeof FOLLOW_STOLEN)) {
        zlog("zoom: NOT armed — engine bytes differ at one of "
             "0x41C426/0x41C442/0x430FAE/0x498EF9/0x468DBA/0x41C3C0/"
             "0x41C808/0x41C93B/0x41CAF7");
        return;
    }
    ok  = redirect(SITE_MMRECT1_VA, (void*)zoom_minimap_rect);
    ok &= redirect(SITE_MMRECT2_VA, (void*)zoom_minimap_rect);
    ok &= redirect(SITE_SAVESCROLL, (void*)zoom_save_scroll);
    g_mmInstalled = ok;
    if (!ok) { zlog("zoom: PARTIAL — see above"); return; }

    /* The three target clamps are BUILT before anything is landed, so a failed
       allocation lands none of them. */
    stubs[0] = build_block_stub(zoom_follow_clamp, FOLLOWCLAMP_END);
    for (i = 0; i < 2; i++) stubs[1 + i] = build_block_stub(zoom_centring_clamp, blocks[i][1]);
    built = stubs[0] && stubs[1] && stubs[2];

    /* THE CAMERA RANGE ONLY ON TOP OF ITS TWO GUARDS, and only on top of a
       minimap wrapper that went in, because the replacement clamp calls it:
       the world point under the mouse and the debug overlay are the two
       readers an off-map eye needs bounded here. `&&`, not `&=`: the clamp
       must not be LANDED at all when a guard did not take. */
    g_eyeInstalled = redirect(SITE_GETTPOS, (void*)zoom_tpos_guard) &&
                     redirect(SITE_DEBUGOVL, (void*)zoom_debug_overlay) &&
                     tagpu_detour_leaf_call(EYECLAMP_VA, EYE_STOLEN,
                                            (int)sizeof EYE_STOLEN,
                                            &g_clampOurs, 0, zoom_eye_clamp);
    if (!g_eyeInstalled) {
        zlog("zoom: PARTIAL — minimap and ScrollSpeed only, the camera "
             "range did NOT install");
        return;
    }
    /* THE TARGET CLAMPS ONLY ON TOP OF THE EYE'S: their whole job is to agree
       with the range `zoom_eye_clamp` enforces. Without them a centring or a
       follow stops at the engine's own range, which is inside the centre
       range, so the camera is short of the edge and nothing else. */
    if (built) {
        built = tagpu_detour_land(FOLLOWCLAMP_VA, stubs[0], (int)sizeof FOLLOW_STOLEN);
        for (i = 0; i < 2; i++)
            built &= tagpu_detour_land(blocks[i][0], stubs[1 + i], (int)sizeof EYE_STOLEN);
    }
    zlog(built ? "zoom: ARMED (minimap rect 0x466B70 x2, ScrollSpeed save 0x430FAE, "
                 "camera centre range 0x41C3C0 + target clamps 0x41C808/"
                 "0x41C93B/0x41CAF7 + world guard 0x498EF9 + debug overlay guard "
                 "0x468DBA); the eye, the target, the follow and ScrollSpeed are "
                 "written on the game thread from the frame packet's command apply"
               : "zoom: ARMED without every target clamp (0x41C808/"
                 "0x41C93B/0x41CAF7) — a centring or a follow near a map edge "
                 "stops at the 1x range");
}
