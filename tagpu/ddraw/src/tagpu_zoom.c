/* tagpu_zoom.c — the view transform shared by the render pass and the input
   path. See tagpu_zoom.h for the contract and why it is lock-free.

   TWO HALVES, ONE FILE. The RENDER half owns the level and the arithmetic —
   the levers, the wheel's tween, the cursor anchor's eye delta, the
   predicted eye every pass draws from — and writes NOTHING into engine
   memory: its whole output is the command record tagpu_zoom_frame_end()
   posts. The GAME half — the engine's own clamp and minimap-rect sites this
   module redirects, plus tagpu_zoom_apply() at the top of every in-play draw
   — is the only code here that touches the eye, the scroll target, the
   follow slots, the minimap box and its dirty bit, the fog flag and the
   scroll poll's step, and it does so on the thread that owns them. Every
   static is marked with the thread that owns it. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "dd.h"
#include "tagpu_opt.h"
#include "tagpu_addr.h"   /* tagpu_user_top: the range filters' upper end */
#include "tagpu_gui.h"
#include "tagpu_shield.h"
#include "tagpu_zoom.h"
#include "tagpu_menu.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"
#include "tagpu_input.h"
#include "tagpu_packet.h"
#include "tagpu_hudstate.h"
#include "tagpu_wikistate.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"
#include "tagpu_terr.h"       /* TAGPU_GATHER_SPAN_MAX */
#include "tagpu_recpov.h"     /* REC_POV_ZOOM_MIN: the follow lever's range */
#include "tagpu_icontype.h"   /* the icons' class table, for the pick */
#include "tagpu_icon_rule.h"  /* the icons' rule: whether a unit's model is hidden */
#include "tagpu_native.h"     /* tagpu_native_owns_unit: who draws a unit, for the pick */
#include "tagpu_fogwide.h"     /* the fog source this thread packs, for the pick */
#include "tagpu_icon.h"       /* tagpu_icon_cmd_take: whether a frame drew icons, for the record */
#include "tagpu_packet_pub.h" /* tagpu_packet_pub_level_gen: what the level caches key on */

/* Published view. Volatile because two threads touch it; each is one aligned
   32-bit slot, which x86 loads and stores atomically. */
static volatile float s_zoom = 1.0f;
static volatile LONG  s_vpL, s_vpT, s_vw, s_vh;
static volatile LONG  s_live;          /* a zoomed world is on screen right now */
static int            s_liveHeld;      /* render thread: frames that world stays live while the lane is rebuilt */
enum { LIVE_HELD_FRAMES = 600 };
static volatile LONG  s_fresh;         /* the pass published during this frame     */
static int            g_mmInstalled;   /* tagpu_zoom_init() redirected the minimap rect */

/* The top of the range every lever shares; the bottom is the floor
   (tagpu_zoom_floor), which follows the map. */
#define ZOOM_MAX  TAGPU_ZOOM_MAX

_Static_assert(REC_POV_ZOOM_MIN == (unsigned)(TAGPU_ZOOM_FLOOR_LO * 65536.0f),
               "a level the floor allows is a level a recording can carry");

static void zlog(const char* m);
static int  in_viewport(int x, int y, int L, int T, int W, int H);
static void anchor_step(int fromWheel, const TAGPU_PACKET* pk);
static int  mega_centre_shift(const TAGPU_PACKET* pk, float* dx, float* dy);
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
   [1/ZOOM_MAX, 1/floor] after each one, as the lab does.

   THE TWEEN. 250 ms of the performance counter, counted from the latest notch,
   from the level DRAWN when the notch is taken to the target, along
   g = 1 - (1 - f)^4 applied to the distance (Recoil's CamTransitionMode 0
   with CamTimeExponent 4, an ease-out). A notch during a tween starts a new
   one from the drawn level toward the previous target times its factor. The
   length is the same at every frame rate; at 60 Hz a quarter of a notch lands
   on the first frame and four fifths of it by 83 ms. Entering and leaving the
   mega map take no tween: they are a cut ("the mega map's cut", below).

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
    LONG mega;                     /* 1: bound to the mega map, 0: to the zoom */
    LONG held;                     /* a world button was down when it was taken */
    LONG tadr;                     /* leaving by this notch centres its point (`megazoom=`) */
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
/* THE MEGA MAP is a state of the wheel, not of the view: engaged means the
   target is the floor because a mega notch put it there, and `s_megaBack` is
   the target it left. Anything else that sets the level -- the file, a
   Follow, the mouse-world pin -- goes through wheel_rest and disengages it, so
   a stale "engaged" can never send a later notch to an old level.

   THE MEGA MAP'S CUT. Entering and leaving replace the view in one frame, and
   it is one frame because nothing in between is ever drawn. The
   new view's tables, eye and level all come from one command record: a cut
   posts its whole eye step and its level there, with the reach request the
   level needs (tagpu_zoom_frame_end), and the game thread takes the record in
   one apply. So `s_cutHold` keeps drawing the view of the last frame, at its
   level and eye, against packets that still describe it, until a packet
   acknowledges the whole step and the ladder no longer raises the level
   (predict); that frame draws the new view. Drawn any earlier, the reach bound
   would clamp the eye short and the ladder would hold the level at a rung, an
   in-between view for as many frames as the game thread takes.
   A BATCH TAKEN DURING A HOLD JOINS THE CUT (wheel_level): a tween started
   then would run from the held view, the level the cut is leaving.

   A MOUSE RECORD IS HANDLED THROUGH ONE VIEW. The door maps a button's point
   through a level (view), and the engine adds its own eye when it handles the
   record. The door, the ring's pop and the apply are all on the game thread:
   the main loop dispatches every queued message, then IdleTick pops and handles
   one record, then the frame's apply runs (engine/sim.md). So the pair a record
   meets can differ from the pair it was mapped with in only three ways, each
   closed by an ordering, none by a time:
   - the record is still in the ring when the cut applies: the cut's record
     waits at the game thread until the ring is empty (tagpu_zoom_cut_gate), and
     IdleTick pops one record each time it runs, so the wait ends;
   - a press arrives between the apply and the frame that shows it: from before
     the cut's record is posted until a frame of the new view has gone to present
     (tagpu_zoom_presented; a frame that did not present does not count),
     `s_cutOwed` stands and the door drops a press on the world with its release.
     The flag falls after `s_zoom` is written, so a press that reads it down reads
     the new level. What is left is the display's own latency, a frame handed to
     present and not yet scanned out, which no ordering here can see;
   - a button is already down on the world when the cut would start, whose
     release acts on the world (the left button's release decides the click or
     the box): a mega notch with a world button held is dropped. The notch says
     so itself, stamped in message order on this same thread, so a button let go
     and not yet dispatched still counts. A press after the notch is the two-flag
     case: each side writes its own flag and then reads the other's through a
     full fence (cut_may_start, tagpu_zoom_drop_mouse), so at least one sees the
     other.
   While `s_cutOwed` stands, what still reads the door (hover, a move) is mapped
   through the game thread's own level, which changes with its eye.

   THE RELEASE'S DEADLINE IS NOT A SAFETY ARGUMENT. The ladder and the reach
   bound still hold on the frame after it, whatever the game thread has done:
   CUT_HOLD_US only stops a game thread that has stalled from freezing the
   picture, and the view it releases is the bound's in-between one. */
static int           s_megaOn;
static float         s_megaBack = 1.0f;
static int           s_cutHold;
static volatile LONG s_cutOwed;                 /* written here, read at the door on the game thread */
static int           s_cutDrawn;                /* this frame draws the new view, acknowledged */
static int           s_cutShown;                /* ...and a frame of it has gone to present */
static unsigned      s_cutSeq;                  /* the cmd_seq the cut was posted in; 0 not yet */
static unsigned      s_cutGen;                  /* the cuts so far: a record names the latest */
static DWORD         s_cutUs;                   /* the latest cut's notch, on the counter */
static int           s_cumX, s_cumY;            /* the cumulative eye delta posted, world px */
/* the game thread's: a press on the world passed the door and its release has not */
static volatile LONG s_worldHeld[4];
#define CUT_HOLD_US  250000
/* THE REST OF A SPIN. A wheel sends many notches a gesture, and only the first
   is the cut: the ones behind it, same way round with no pause longer than
   this, are dropped, so a spin out of the mega map lands on the level it kept
   instead of zooming on past it. This is the feel of the gesture and nothing
   rests on it. */
#define SPIN_GAP_US  200000
static int           s_spinDir;                 /* +1, -1: the cut's way round; 0: none */
static LONG          s_spinUs;                  /* the spin's latest notch */

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

/* THE FLOOR, RENDER THREAD: the level the wheel cannot go below and the one
   the mega map engages at. Set once a frame, at the top of read_lever and
   before any pass runs, from the packet the frame is drawn from (the game
   thread's floor_compute); every consumer this frame reads this value, so the
   wheel, the levers, the reach bound and the native pass's gather cannot
   disagree about it. */
static float s_floor = TAGPU_ZOOM_FLOOR_HI;

float tagpu_zoom_floor(void)
{
    return s_floor;
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

static void cut_end(void)
{
    s_cutHold = 0;
    s_cutDrawn = 0;
    s_cutShown = 0;
    s_cutSeq = 0;
    InterlockedExchange(&s_cutOwed, 0);
}

/* Some press on the world has passed the door without its release. */
static int world_button_held(void)
{
    int i;
    for (i = 0; i < 4; i++) if (s_worldHeld[i]) return 1;
    return 0;
}

/* Render thread, before a cut starts: this side of "a mouse record is handled
   through one view". The notch's own `held` is the message order's answer for
   what came before it; the bits, read after raising the flag, are the answer for
   a press after it, whose door raises its bit and then reads the flag. */
static int cut_may_start(const NOTCH* e)
{
    LONG was = s_cutOwed;
    InterlockedExchange(&s_cutOwed, 1);
    if (!e->held && !world_button_held()) return 1;
    InterlockedExchange(&s_cutOwed, was);
    return 0;
}

/* GAME THREAD, once a draw: a release lost outside the window must not refuse
   every cut after it. Windows updates the async state as it queues the message
   [INFERRED], so all buttons up at one apply puts every release in the queue, and
   the pump runs before the next apply: a bit that outlives that was never going to
   be cleared. Clearing here and not at the notch keeps the decision in message
   order. Under the shield the async state is not these buttons, so nothing clears
   here: a press that passed the door before the shield armed keeps its bit until
   that slot's next release, which only a test launch can reach. */
static void world_held_stale(void)
{
    static int upBefore;
    int i, up;

    if (!world_button_held() || tagpu_shield_on()) { upBefore = 0; return; }
    up = !((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
            GetAsyncKeyState(VK_MBUTTON) | GetAsyncKeyState(VK_XBUTTON1) |
            GetAsyncKeyState(VK_XBUTTON2)) & 0x8000);
    if (up && upBefore)
        for (i = 0; i < 4; i++) InterlockedExchange(&s_worldHeld[i], 0);
    upBefore = up;
}

/* The packet has applied the cut's record and all of its eye step. */
static int cut_acked(const TAGPU_PACKET* pk)
{
    return s_cutSeq && (int)(s_cutSeq - pk->cmd_ack_seq) <= 0 &&
           s_cumX == pk->cmd_ack_dx && s_cumY == pk->cmd_ack_dy;
}

/* Render thread: the wheel at rest at level z — no tween, nothing owed — with
   the ring left as it is. */
static void wheel_rest(float z)
{
    s_wheelTgt = s_wheelCur = z;
    s_tweening = 0;
    s_tweenG = 1.0;
    s_batch = 0;
    s_remX = s_remY = 0.0f;
    s_gPosted = 1.0;
    s_wheelPend = 0;
    s_megaOn = 0;
    s_spinDir = 0;
    /* the hold, not the door: the cut's record may still be on its way to the
       game thread, and the end of read_lever shuts the door on its ack */
    s_cutHold = 0;
}

/* Render thread: the wheel at rest at level z, the ring emptied. Returns the
   raw delta thrown away. */
static LONG wheel_reset(float z)
{
    LONG dropped = wheel_discard();
    wheel_rest(z);
    return dropped;
}

/* Render thread: 1 when a notch is queued. `head` is the message thread's
   interlocked store, so a notch it published is seen here or on a later
   frame, never lost: nothing in this file takes it but wheel_level and the
   two discards. */
static int notches_queued(void)
{
    return s_nqHead != s_nqTail;
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

/* Start a tween toward the target, counted from `at`: from the level last
   DRAWN, which is the tween's own unless the reach bound or the reach ladder
   held it for that frame (predict, reach_ladder). */
static void tween_start(DWORD at)
{
    s_izFrom = 1.0f / s_lever;
    s_izTo   = 1.0f / s_wheelTgt;
    s_tweenUs = at;
    s_tweening = 1;
    s_landMs = -1.0f;
}

/* THE FLOOR MOVED (a level load, a resize, a map's extent). Engaged means the
   target is the floor, so the target moves with it, along a tween (it is not
   the player's gesture) or at once during a cut's hold; otherwise a target the new floor leaves below it rests at the
   floor. Render thread, from read_lever before the wheel is read. */
static void floor_moved(void)
{
    if (s_megaOn) {
        s_wheelTgt = s_floor;
        if (s_cutHold) s_wheelCur = s_wheelTgt;
        else tween_start(qpc_us());
    } else if (s_wheelTgt < s_floor) {
        wheel_rest(s_floor);
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
   by construction, which is the bound the reach bound's "a gesture that only
   zooms in is never held" rests on. For every notch the producer took against the same
   viewport the clamp is the identity. */
static float wheel_level(const TAGPU_PACKET* pk)
{
    LONG  h = s_nqHead, t = s_nqTail, d = 0, lastUs = 0;
    DWORD now;
    int   vL = (int)s_vpL, vT = (int)s_vpT, vW = (int)s_vw, vH = (int)s_vh;
    float cx, cy;
    int   n = 0, cut = 0;

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
        double iz;
        int    centre = 0, whole = 0, way = e->delta > 0 ? 1 : -1;
        float  z;
        if (way == s_spinDir && (LONG)((DWORD)e->us - (DWORD)s_spinUs) < SPIN_GAP_US) {
            s_spinUs = e->us;
            continue;
        }
        s_spinDir = 0;
        /* THE MEGA MAP. A mega notch back engages it: the target becomes the
           floor and the view centre moves onto the map's centre, so the whole
           map is on screen from wherever it was engaged, and the target it
           left is kept. While it is engaged ANY notch forward leaves it --
           TADR's gesture -- back to the kept target, holding the world point
           under the pointer as any notch in does, or under `megazoom=tadr`
           moving the view centre onto it.
           A mega notch that has nothing to do (back while engaged, forward
           while not) is not a notch at all: it starts no tween and restarts
           none. A zoom notch back while engaged is already at the floor and
           leaves it engaged. */
        if (s_megaOn && e->delta > 0) {
            if (!cut_may_start(e)) continue;
            iz = 1.0 / (double)s_megaBack;
            s_megaOn = 0;
            centre = e->tadr != 0;
            cut = 1;
            s_spinDir = way;
            s_spinUs = e->us;
        } else if (e->mega) {
            /* engaging needs a level to leave: at the floor already it
               would keep the floor as the level to return to, so the eye would
               move to the map's centre and the next notch forward would bring no
               level back */
            if (e->delta < 0 && !s_megaOn && s_wheelTgt > s_floor) {
                if (!cut_may_start(e)) continue;
                s_megaBack = s_wheelTgt;
                s_megaOn = 1;
                iz = 1.0 / (double)s_floor;
                whole = 1;
                cut = 1;
                s_spinDir = way;
                s_spinUs = e->us;
            } else {
                continue;
            }
        } else {
            iz = izPrev * notch_factor(e->delta);
        }
        if (iz < 1.0 / ZOOM_MAX) iz = 1.0 / ZOOM_MAX;
        if (iz > 1.0 / s_floor) iz = 1.0 / s_floor;
        z = (float)(1.0 / iz);
        /* Land EXACTLY on 1.0 when the notches bring it there. 1x is the
           identity the whole stack tests for by equality — the transform, the
           minimap rect and the scroll step each short-circuit on `z == 1.0f` —
           so a target within a thousandth of it is taken as it. BAR's notches
           do not cancel (in-then-out is 1.020), so this is reached only by a
           sequence that happens to land inside the band. */
        if (z > 0.999f && z < 1.001f) z = 1.0f;
        s_wheelTgt = z;
        iz = 1.0 / (double)z;
        /* a notch IN holds the point it was aimed at, clamped into this
           frame's viewport (above), and leaving the mega map is one; TADR's
           leaving moves the view centre ONTO that point instead: (a - c) * izPrev
           is its world offset from the centre at the level being left */
        if ((centre || iz < izPrev) && vW > 0 && vH > 0) {
            int   qx = (int)(short)(e->xy & 0xFFFF);
            int   qy = (int)(short)((e->xy >> 16) & 0xFFFF);
            float ax = (float)clampi(qx, vL, vL + vW - 1);
            float ay = (float)clampi(qy, vT, vT + vH - 1);
            float k  = (float)(centre ? izPrev : izPrev - iz);
            s_shiftX += (ax - cx) * k;
            s_shiftY += (ay - cy) * k;
        }
        if (whole) {
            float dx, dy;
            if (mega_centre_shift(pk, &dx, &dy)) {
                s_shiftX += dx;
                s_shiftY += dy;
            }
        }
        d += e->delta;
        lastUs = e->us;
        n++;
    }
    InterlockedExchange(&s_nqTail, t);          /* the slots are read: release them */
    /* a spin ends by the frame, so the stamps compared above stay near each other */
    if (s_spinDir && (LONG)(now - (DWORD)s_spinUs) >= SPIN_GAP_US) s_spinDir = 0;

    if (n) {
        LONG back;
        s_batch  = 1;
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
        /* A tween even when the level does not move (a notch at the end of
           the range): R may still owe the displacement of the tween it cut,
           and it is paid out along this one rather than in one frame. */
        tween_start(back > 0 ? now - (DWORD)back : now);
        /* the target at once, and with it g = 1 below: anchor_step posts the
           whole of R in this frame's record, so the cut's eye and level reach
           the game thread together */
        if (cut || s_cutHold) {
            s_wheelCur = s_wheelTgt;
            s_tweening = 0;
            if (cut) {
                s_cutUs = back > 0 ? now - (DWORD)back : now;
                s_cutGen++;
                s_cutSeq = 0;
                s_cutDrawn = 0;
                s_cutShown = 0;
            }
            s_cutHold = 1;
        }
        s_wheelPend += d;
    } else if (s_wheelPend && !s_tweening && !s_cutHold) {
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

/* ---- the follow lever -------------------------------------------------------

   THE REPLAY'S FOLLOW AND JUMP REACH THE RENDER THREAD ONLY BY VALUE. The
   game thread writes the camera for a follow epoch in its apply and publishes
   the epoch and its level in the packet of that draw (follow_epoch,
   follow_level); this thread reads those two numbers and nothing else, and
   answers with two of its own in every command record (follow_broken,
   follow_seen). An epoch is the replay's counter, unique in the process and
   never reused, and every test of one is an EQUALITY against the epoch the
   reader holds in force — so a packet or a record from before an epoch began,
   or from one that has ended, matches nothing and changes nothing, however
   late either thread takes it. That is the whole ordering argument: no word
   of either side is read by the other, and no stale value can be mistaken
   for a current one.

   In read_lever the follow lever stands between the file and the wheel. A
   packet whose epoch is nonzero, not the one this thread broke, and whose
   level is in the lever's range puts the follow in force, at its level or
   at this viewer's floor where the recorded level is below it:
     no notch queued: the wheel is pinned at rest to the follow level and no
       anchor is applied; when the frame is drawn at that level the epoch is
       `seen`, which is what ends a Jump on the game thread;
     a notch queued: the epoch is `broken` — the apply stops writing the
       camera — and the wheel starts from the follow level and takes the notch
       as it takes any, its anchor included.
   Either way the wheel is left at the follow level when the epoch ends, so
   ending a Follow moves nothing on the screen. Render thread only. */
static unsigned s_followBroken;            /* the epoch a notch ended            */
static unsigned s_followSeen;              /* the latest epoch drawn at its level */
static unsigned s_followFree;              /* this frame's follow_free ("the reach ladder") */

#define FOLLOW_Q16_MIN REC_POV_ZOOM_MIN
#define FOLLOW_Q16_MAX ((unsigned)(ZOOM_MAX * 65536.0f))

/* The follow epoch this packet puts in force, or 0. */
static unsigned follow_in_force(const TAGPU_PACKET* pk)
{
    if (!pk || !pk->in_game || !pk->follow_epoch || pk->follow_epoch == s_followBroken ||
        pk->follow_level < FOLLOW_Q16_MIN || pk->follow_level > FOLLOW_Q16_MAX)
        return 0;
    return pk->follow_epoch;
}

/* the eye this frame is drawn from (render thread) */
static int   s_predX, s_predY, s_havePred;
/* the reach bound's cost and the witness of frames drawn at the last level, by
   the monotone rule or a cut's hold (tagpu_zoom_reach_held) */
static unsigned s_reachHeld, s_reachPaused, s_reachBack;
static int      s_reachHeldMax;

/* ---- the reach ladder --------------------------------------------------------

   THE TABLES' REACH FOLLOWS THE LEVEL, and this is what makes that safe. The
   game thread builds the packet's anchor table and the models' cull over a
   window about its eye (tagpu_packet_pub.c, table_reach): the slab at the
   packet's `reach_level` grown by the lead. A floor-sized window would be the
   whole map at every level on a large map, so the window is taken at the
   level this thread asks for instead, as a rung of the ladder
   (tagpu_zoom_rung): every record carries `reach_zoom`, the rung of the
   lowest level this view may draw before its next record is applied -- the
   level drawn, the wheel's target and the lever's own, whichever is lowest.

   THE INVARIANT: every packet the game thread can still publish carries
   tables taken at a rung of a request this thread posted -- the current
   packet's `reach_level`, or one of the requests still in flight -- and the
   frame is drawn at no level below any of them. So the slab at the drawn
   level lies inside the reach of every packet that can arrive (the gather
   span falls as the level rises), the reach bound's interval is not empty for
   want of a level, and a newer packet can never ask for a HIGHER level than
   the one last drawn: a zoom-out waits at a rung for the game thread instead
   of popping back. The request includes the wheel's target, so a notch out
   asks for its target's rung on the frame it arrives; the wait is the game
   thread's lag, at a rung crossing only (`reachheld=` in the native
   heartbeat).

   IN FLIGHT means posted and not yet acknowledged: a request leaves the ring
   when a packet's `cmd_ack_seq` reaches its seq, from which on every packet
   carries the reach of that record or a later one. A full ring merges its two
   oldest entries into one with the later seq and the larger rung, which only
   holds the level higher for longer. A new epoch does NOT empty the ring: the
   game thread applies a record's reach whatever its epoch (the epoch gates
   only the eye delta), so a request of the old level still decides the reach
   of a packet of the new one.

   A FOLLOW'S PACKETS ARE THEIR OWN REQUEST. Its level and camera are written on
   the game thread, so that thread takes the reach too: the lower of the record's
   rung and the rung of the level it writes (tagpu_zoom_apply). A packet of a
   Follow therefore always covers its own level, and a recorded cut draws whole
   on the packet that brings it, with nothing asked first. A rung above the level
   is dropped from the ring on such a frame, because it can no longer raise a
   packet's reach: the game thread keeps the Follow's rung as a cap after the
   Follow ends, until a record comes from a frame that holds a packet past that
   Follow and is not following (follow_free). Its ring holds its own rung and every
   rung after it, and the rungs dropped are older, so a Follow's end pops nothing.
   What makes each frame SAFE is simpler: zmin always includes this packet's own
   reach, and a frame draws from one packet. Render thread only. */
#define REACH_INFLIGHT 8
static struct { unsigned seq; float rung; } s_inflight[REACH_INFLIGHT];
static int      s_nInflight;
static float    s_reach = TAGPU_ZOOM_FLOOR_HI;   /* this frame's packet's reach_level */
static float    s_asked = 1.0f;                  /* the level the levers named this frame */
static float    s_zmin = TAGPU_ZOOM_FLOOR_HI;    /* ...and the lowest it may be drawn at  */
static unsigned s_reachLevelHeld;

float tagpu_zoom_rung(float z, float floor)
{
    static const float ladder[] = { 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f };
    float r = ladder[sizeof ladder / sizeof ladder[0] - 1];
    int i;
    for (i = 0; i < (int)(sizeof ladder / sizeof ladder[0]); i++)
        if (z >= ladder[i]) { r = ladder[i]; break; }
    if (r < floor) r = floor;
    if (r > TAGPU_ZOOM_FLOOR_HI) r = TAGPU_ZOOM_FLOOR_HI;
    return r;
}

/* The level this frame may be drawn at, at least: the floor, the packet's
   reach level and every rung still in flight ("A Follow's packets are their own
   request"). */
static float reach_ladder(const TAGPU_PACKET* pk, float z, int following)
{
    float zmin = s_floor > s_reach ? s_floor : s_reach;
    int i, n = 0;

    for (i = 0; i < s_nInflight; i++) {
        if (pk && (int)(s_inflight[i].seq - pk->cmd_ack_seq) <= 0) continue;
        s_inflight[n++] = s_inflight[i];
        /* a rung above a Follow's level leaves the ring ("the reach ladder") */
        if (following && s_inflight[i].rung > z) { n--; continue; }
        if (s_inflight[i].rung > zmin) zmin = s_inflight[i].rung;
    }
    s_nInflight = n;
    s_zmin = zmin;
    if (z < zmin) { z = zmin; s_reachLevelHeld++; }
    return z;
}

/* A request posted as `seq`, into the ring (see "the reach ladder"). */
static void reach_posted(unsigned seq, float rung)
{
    if (s_nInflight == REACH_INFLIGHT) {
        int i;
        if (s_inflight[0].rung > s_inflight[1].rung) s_inflight[1].rung = s_inflight[0].rung;
        for (i = 1; i < REACH_INFLIGHT; i++) s_inflight[i - 1] = s_inflight[i];
        s_nInflight--;
    }
    s_inflight[s_nInflight].seq = seq;
    s_inflight[s_nInflight].rung = rung;
    s_nInflight++;
}

float tagpu_zoom_read_lever(const TAGPU_PACKET* pk)
{
    unsigned fe;
    float    fz;
    int      following = 0;

    /* THE FLOOR AND THE REACH LEVEL, once a frame and first: every consumer
       below reads them. A packet with no sane floor (the level end's) is the
       fixed one; no packet keeps the last. */
    {
        float fl = (pk && pk->zoom_floor >= TAGPU_ZOOM_FLOOR_LO &&
                    pk->zoom_floor <= TAGPU_ZOOM_FLOOR_HI)
                   ? pk->zoom_floor : (pk ? TAGPU_ZOOM_FLOOR_HI : s_floor);
        if (fl != s_floor) {
            s_floor = fl;
            floor_moved();
        }
        s_reach = (pk && pk->reach_level >= TAGPU_ZOOM_FLOOR_LO &&
                   pk->reach_level <= TAGPU_ZOOM_FLOOR_HI) ? pk->reach_level : s_floor;
    }
    fe = follow_in_force(pk);                   /* see "the follow lever" */
    /* a recorded level below this viewer's floor is drawn at the floor, and
       is still the follow's level for `seen` */
    fz = fe ? (float)pk->follow_level / 65536.0f : 1.0f;
    if (fe && fz < s_floor) fz = s_floor;

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

       THE LEVERS ARE UNGATED BY ANY ARM FILE. `tagpu_zoom.on` installs the
       minimap/scroll-step/camera-range patches and nothing else; the file, the
       follow and the wheel are read here, and the native pass publishes the view, with no
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
        cut_end();
        s_zoom = 1.0f;
        s_asked = 1.0f;
        anchor_step(0, pk);
        predict(pk, 1.0f, 0);
        s_lever = 1.0f;
        /* the level is pinned and the follow's camera drawn: a Jump has
           nothing more to wait for */
        if (fe) s_followSeen = fe;
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
    if (fromWheel && fe && !notches_queued()) {
        /* THE FOLLOW LEVER, pinned: no notch, so no gesture and no anchor */
        wheel_rest(fz);
        z = fz;
        following = 1;
        fromWheel = 0;
    } else if (fromWheel) {
        if (fe) {
            /* a notch for the epoch in force ends it, and the wheel takes
               the notch from the follow level */
            s_followBroken = fe;
            wheel_rest(fz);
            s_lever = fz;
        }
        z = wheel_level(pk);
    } else {
        char zb[32]; DWORD zn = 0;
        if (ReadFile(zh, zb, sizeof zb - 1, &zn, 0) && zn > 0) {
            float lv;
            zb[zn] = 0;
            lv = (float)atof(zb);
            /* the file says a level; the floor of this map and screen is what
               can be drawn */
            if (lv >= TAGPU_ZOOM_FLOOR_LO && lv <= ZOOM_MAX) z = lv < s_floor ? s_floor : lv;
        }
        CloseHandle(zh);
        /* On a torn read `z` is still `s_zoom`, the LAST GOOD level, which is
           the one actually in force and therefore the one the wheel must
           inherit when the file goes away. */
        wheel_pin(z);
        /* the file outranks the follow's level, so the follow's camera is
           drawn at the file's: a Jump has nothing more to wait for */
        if (fe) s_followSeen = fe;
    }
    /* The eye step that holds the point under the cursor, worked out HERE:
       this is the call the driver makes at the top of its frame, before any
       pass reads the eye, so the zoom and the eye it is drawn with change
       together. Only the wheel anchors — the file lever has no gesture behind
       it and every zoom fixture drives it. */
    anchor_step(fromWheel, pk);
    /* the reach ladder and the reach bound may hold the level for this frame
       (reach_ladder, predict), so the published level is written once, after
       both */
    s_asked = z;
    z = reach_ladder(pk, z, following);
    s_followFree = (pk && !following) ? pk->follow_last : 0u;
    z = predict(pk, z, 1);
    /* seen only when the frame is drawn AT the follow level: the reach ladder
       or the reach bound may draw it at another (reach_ladder, predict) */
    if (following && z == fz) s_followSeen = fe;
    s_zoom = z;
    s_lever = z;
    /* after the store ("a mouse record is handled through one view"). A deadline
       release keeps the door shut: its eye may not have been applied. */
    if (s_cutOwed && !s_cutHold && pk) {
        if (s_cutShown) cut_end();
        else if (cut_acked(pk)) s_cutDrawn = 1;
    }
    return z;
}

void tagpu_zoom_render_gone(void)
{
    cut_end();
    s_liveHeld = 0;
    s_live = 0;
    /* the apply goes on running on the last record (render_gdi.c posts none), so
       without this the scroll step, the minimap box and the pick stay at the last
       level */
    tagpu_zoom_frame_end();
}

void tagpu_zoom_presented(void)
{
    s_liveHeld = 0;
    if (s_cutDrawn) s_cutShown = 1;
}

/* A LANE BEING REBUILT IS NOT A WORLD THAT LEFT THE SCREEN. Its frames draw nothing zoomed, and a
   record saying so hands the engine its own camera clamp back, which moves an eye out in the mirror's
   margin onto the map (MEASURED 2026-11-06: eye x -206 to 0 across a rebuild, and held there while
   the game is paused). The gather goes on publishing the view with the lane down, so a published view
   says nothing about the lane: the world stays live, and the ground ours (tagpu_terrown_set_skip),
   from any rebuild's start until a frame reaches the screen. LIVE_HELD_FRAMES bounds it, counted on
   every frame, for a lane that neither presents nor fails and for a level that ends under the rebuild,
   and a lane given up on ends it at once: `tagpu_zoom_render_gone`. */
int tagpu_zoom_live_held(void) { return s_liveHeld > 0; }

void tagpu_zoom_lane_retrying(void)
{
    s_liveHeld = s_live ? LIVE_HELD_FRAMES : 0;
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

int tagpu_zoom_gather_span(int v, float z, float floor)
{
    float f;
    int span;
    if (!(z == z)) return v;
    /* a level below the floor is drawn at the floor's span, never at the 1x
       one: that would cull a visible ring */
    if (z < floor) z = floor;
    if (z >= 1.0f) return v;
    /* THE ABSOLUTE CAP, compared in float before the conversion: it bounds the
       value whatever floor the caller handed in, a zero or a negative one
       included */
    f = (float)v / z + 64.0f;
    if (!(f <= (float)TAGPU_GATHER_SPAN_MAX)) span = TAGPU_GATHER_SPAN_MAX;
    else span = f > (float)v ? (int)f : v;
    return span;
}

void tagpu_zoom_pub_window(int eye, int v, float level, int* lo, int* span)
{
    int s = tagpu_zoom_gather_span(v, level, level);
    int pad = TAGPU_GATHER_MARGIN + (v > 0 ? v / TAGPU_LEAD_DIV : 0);

    *lo = eye - (s - v) / 2 - pad;
    *span = s + 2 * pad;
}

void tagpu_zoom_reach_held(unsigned* frames, int* maxPx, unsigned* paused, unsigned* back,
                           unsigned* levelHeld)
{
    *frames = s_reachHeld;
    *maxPx = s_reachHeldMax;
    *paused = s_reachPaused;
    *back = s_reachBack;
    *levelHeld = s_reachLevelHeld;
}

static int options_gui_owns_input(void);

/* Message thread: is this wheel message the mega map's? Ctrl picks the one
   the plain wheel does not do, and `wheel=` in impure.cfg says which that is:
   `zoom` (the default) zooms on the plain wheel, `megamap` is TADR's binding.
   Both doors carry the modifier keys in wparam's low word (wndproc.c's
   hardware message, tagpu_shield.c's injected one). */
static LONG wheel_is_mega(WPARAM wparam)
{
    int legacy = 0, ctrl = (LOWORD(wparam) & MK_CONTROL) != 0;
    if (!tagpu_settings_get(TS_WHEEL, &legacy)) legacy = 0;
    return (LONG)((ctrl ^ (legacy == 1)) & 1);
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
    /* THE IN-GAME MENUS OWN IT, as they own every button (see
       `options_gui_owns_input`, the same test at the click door): ARMOPT and
       the screens under it sit over the world, and a notch there is the
       menu's -- the list wheel's, when a list is under the pointer
       (tagpu_gui.h) -- not a zoom of the world behind it. */
    if (options_gui_owns_input()) {
        if (now - s_gripeTick > 1000) {
            s_gripeTick = now;
            zlog("zoom: wheel ignored — an in-game menu owns input");
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
        e->mega  = wheel_is_mega(wparam);
        e->held  = world_button_held();
        {
            int centred = 0;
            e->tadr = tagpu_settings_get(TS_MEGAZOOM, &centred) && centred == 1;
        }
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

/* ---- the scroll step ---------------------------------------------------------

   THE SCROLL POLL `0x41CE90` (one call, `0x496976`, on the GAME THREAD) steps
   the eye by `ScrollSpeed main+0x1434D (u8) x main+0x38A3F` (the 30-Hz ticks
   since the scheduler's last call), capped at 0x80, and returns before any key
   or edge test when the step is 0 (`0x41CE99..0x41CEBA`, DISASSEMBLED). One
   register, EBX, carries that step to edge scroll and the arrow keys alike, in
   all four directions (`0x41CFA1`/`0x41CFCC`/`0x41CFFB`/`0x41D025`). The step
   is in WORLD pixels, so at zoom z the picture moves z times as far across the
   SCREEN: the step is divided by the level IN THE REGISTER, AFTER THE CAP
   (`zoom_scroll_step`, through the stub at `0x41CEAA`). For z > 0,
   min(a, b)/z = min(a/z, b/z), so that equals scaling both the byte and the
   cap, and nothing saturates: the screen scrolls at the 1x rate at every level.

   THE BYTE HAS ONE CLASS OF WRITER, THE ENGINE. Nothing in this DLL writes
   `main+0x1434D`; every engine access of it is listed in
   research/notes/engine/input.md (its `main+0x1434D` row, from a raw scan of
   the displacement), so every engine reader sees the player's own value, and
   the zoom exists only in EBX, from the call at `0x41CEAB` to the poll's
   `pop ebx` at `0x41D0B3`.

   THE STEP IS BOUNDED. A positive engine step becomes one in
   [1, 0x80 / TAGPU_ZOOM_FLOOR_LO] = [1, 8192] world px; 0, a negative step and
   every step at z == 1.0f are the engine's own. The eye it moves is clamped at
   `0x41D054` (`0x41C3C0`: zoom_eye_clamp when the camera range is installed)
   before anything reads it.

   THE EYE IS NOT SIMULATED: it is a per-player camera preference that no
   other machine ever sees, and the step is not simulation state, so this
   cannot perturb the sim, a replay or an MP session. */
#define SCROLL_STEP_CAP  0x80          /* the poll's own cap, 0x41CEAA..0x41CEB3 */
#define TA_MAINPP        0x00511DE8u
#define OFF_GUI_FLAGS    0x37EBE   /* bit 0: ARMOPT/exit/preferences owns input */

/* game thread only: the level the last command carried, clamped to the
   floor this thread computed, and whether it is live — what every game-thread
   reader in this file uses in place of the render thread's s_zoom/s_live, so
   no float crosses the threads — and whether the centre range is in force
   (camera_centre() below) */
static float s_gLevel = 1.0f;
static int   s_gLive, s_gCentre;
/* game thread only: the floor (floor_compute) and the level the packet's
   tables' reach is taken at (the record's reach rung, clamped to the floor) */
static float s_gFloor = TAGPU_ZOOM_FLOOR_HI;
static float s_gReach = TAGPU_ZOOM_FLOOR_HI;
/* game thread only: the last record of this level's epoch drew the
   strategic icons (icon_hit); 0 from the level end until one does */
static int   s_gIcons;
/* game thread only: the level the minimap's view box main+0x142CB was last scaled by; every
   write of it goes through zoom_minimap_rect once tagpu_zoom_init installed the redirects
   (g_mmInstalled), and without them the engine's clamp writes the 1x box, so it is not kept */
static float s_rectLevel = 1.0f;

static float game_level(void)
{
    return s_gLive ? s_gLevel : 1.0f;
}

/* GAME THREAD, called from the scroll poll `0x41CE90` through the stub at
   `0x41CEAA` (scroll_step_install). `step` is EBX before the cap -- the byte
   times the elapsed ticks -- and the return value is EBX from `0x41CEB8` on.
   The engine's signed cap is reproduced first, so at z == 1.0f the step is the
   engine's bit for bit. The level is this thread's own (game_level: the last
   apply's, at or above TAGPU_ZOOM_FLOOR_LO; the guard below also turns a NaN
   back into the engine's step), so the result is at most 0x80 / (1/64) = 8192,
   and it is at least 1 rather than 0 because 0 makes the poll return at
   `0x41CEBA` and a held key would do nothing. */
static int __cdecl zoom_scroll_step(int step)
{
    float z;
    int v;

    if (step > SCROLL_STEP_CAP) step = SCROLL_STEP_CAP;     /* the replaced cmp/jle/mov, signed */
    if (step <= 0) return step;                              /* 0 skips the poll at 0x41CEBA */
    z = game_level();
    if (z == 1.0f || !(z >= TAGPU_ZOOM_FLOOR_LO)) return step;
    v = iround((float)step / z);
    return v < 1 ? 1 : v;          /* <= SCROLL_STEP_CAP / TAGPU_ZOOM_FLOOR_LO = 8192 */
}

/* ---- the camera's range ----------------------------------------------------

   THE CENTRE CLAMP (BAR's: Recoil's SpringController clamps the ground point
   at the view centre to the map, and nothing else). The view is centred on
   `eye + W/2` at every zoom, W being the TRUE viewport, so the range that keeps
   that centre on the map is

       eye in [-W/2, extent - W/2]         the same at every zoom

   and the map's edge can reach the middle of the screen, zoomed in or out.
   The engine's own `0x41C3C0` clamps to `[0, extent - W]` instead, the range
   that keeps the VIEWPORT's edges on the extent at 1x.

   THE MAP IS THE SCROLL EXTENT, main+0x1422B/0x1422F: the map's pixel size
   main+0x14223/0x14227 less 32 px wide and 128 px tall, written at the level
   load (`0x4833B8..0x4833E0`: `sub 0x20`, `sub 0x80`) and rewritten after that
   only by the debug-level `Edge` command [DISASSEMBLED]. MEASURED 2026-09-23 on
   Two Continents: PLOT 672 x 800, main+0x14223/0x14227 10752 x 12800, extent
   10720 x 12672. Stock never shows a pixel past it, and the strip between it
   and the tile map is art no player saw; the terrain and feature passes fold
   the mirror at the same line (tagpu_fx.h `tagpu_edge_area`). The packet carries the same words (`map_pxw/h`, and the
   PLOT grid's `map_w16/h16` that bound it, camera_range), so the two threads
   compute the range from the same words and agree by construction.

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
#define OFF_EXTENT_H     0x1422F       /* ...and less 128 px: where the map ends  */
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
    return (size_t)ta > 0x600000u && (size_t)ta <= tagpu_user_top();
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

   THE BOX CANNOT INVERT, BY CONSTRUCTION. The engine places it by the EXTENT
   from the viewport's top-left, and the centre range lets that top-left reach
   W/2 (H/2) past the engine's own `extent - W` — so near the far edges the
   box runs up to half its 1x size past the minimap, its centre onto the
   minimap's edge, and at a high zoom the whole shrunken box lies past: an
   edge clamped on one side only then leaves top below bottom, a box the GUI
   renderer drops and the engine's own drawer would put on the panel below the
   minimap. So the centre is clamped into the
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
    s_rectLevel = zm;

    cx = (float)(r[0] + r[2]) * 0.5f;
    cy = (float)(r[1] + r[3]) * 0.5f;
    hw = (float)(r[2] - r[0]) * 0.5f;
    hh = (float)(r[3] - r[1]) * 0.5f;
    if (zm >= TAGPU_ZOOM_FLOOR_LO && zm != 1.0f) { hw /= zm; hh /= zm; }
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
   `[-W/2, extent - W/2]` when `centre`, else the engine's own
   `[0, extent - W]`. Returns 0 — and leaves the outputs alone — when the
   inputs are not sane enough to compute it. The centre range cannot invert
   (its width is the extent's); the engine's inverts on a map smaller than the
   viewport, where the engine alternates between 0 and a negative bound on
   every call, and this one holds at 0.
   THE CENTRE RANGE IS BOUNDED BY THE TILE MAP AS WELL (`mapW`, `mapH`, the
   PLOT grid x 16): the debug-level `Edge` command writes the extent as the
   map's pixel size less an argument it never clamps (`0x416730`), so a
   negative one names an extent past the map, and the far end the eye's
   readers were audited for is the map's (exe map, "Who reads the eye"). */
static int camera_range(int W, int H, int extW, int extH, int mapW, int mapH,
                        int centre, int* loX, int* hiX, int* loY, int* hiY)
{
    if (W <= 0 || H <= 0 || extW <= 0 || extH <= 0) return 0;
    if (centre) {
        if (mapW <= 0 || mapH <= 0) return 0;
        *loX = -(W / 2); *hiX = (extW < mapW ? extW : mapW) - W / 2;
        *loY = -(H / 2); *hiY = (extH < mapH ? extH : mapH) - H / 2;
    } else {
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

/* THE MAP'S CENTRE AS AN EYE: the middle of the centre range, which puts the
   view centre on the middle of `[0, min(extent, tile map)]` on each axis --
   the rectangle floor_compute's `fit` spans, so at the floor the whole of it
   is on screen. From the packet's copies of floor_compute's own inputs (the
   true viewport, the extent, the PLOT grid; range_pk), and the centre range
   whatever the packet's `cam_centre` says: the eye is the one the mega map
   lands at, at the floor, where the rect is wide and the apply holds the
   camera to that range. 0, outputs untouched, with no range to take it from. */
static int mega_eye(const TAGPU_PACKET* p, float* ex, float* ey)
{
    int loX, hiX, loY, hiY;

    if (!camera_range(p->vp[2], p->vp[3], p->map_pxw, p->map_pxh,
                      plot_px(p->map_w16), plot_px(p->map_h16), 1, &loX, &hiX, &loY, &hiY))
        return 0;
    *ex = (float)(loX + hiX) * 0.5f;
    *ey = (float)(loY + hiY) * 0.5f;
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

/* The replacement for `0x41C3C0`. GAME THREAD. `arg` is the first stack slot
   of a function that takes no arguments — ignored.

   THE SCROLL TARGET IS DELIBERATELY NOT TOUCHED HERE, unlike in the apply.
   `main+0x14327`/`+0x1432B` is where the camera is heading, and three of this
   function's callers are inside the per-frame stepper `0x41CA10`, which eases
   the eye halfway toward it and calls us afterwards: writing the target there
   would make it the eye every frame and the camera would never arrive. The
   other nine callers each MEANT to move the camera, and each copies the
   clamped eye into the target itself right after we return (e.g. `0x41C5A4`,
   `0x41CDE6`, `0x41D05E`), so the pair stays consistent without our help. */
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
   holds exactly. The replay's Follow and Jump release it the same way, on
   their epoch's first write (tagpu_zoom_follow): a Ctrl+C made before them
   would otherwise have the stepper move the target to its unit, which ends
   the Follow on its next frame. `who` names the taker for the log. */
static void release_follow(char* ta, const char* who)
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
            char b[96];
            tick = now;
            _snprintf(b, sizeof b, "zoom: %s took the camera - the unit follow is released", who);
            b[sizeof b - 1] = 0;
            zlog(b);
        }
    }
}

/* ---- the floor: the game thread's -------------------------------------------

   THE FLOOR IS THE LEVEL AT WHICH THE WHOLE MAP FITS THE SCREEN, bounded by
   what a click can carry. Two terms, each a property of the level:

     fit      min(W / spanX, H / spanY) over the centre range
              (camera_range, centre = 1) -- the range the camera moves in,
              which is the scroll extent bounded by the tile map, so the
              same arithmetic the camera and the mirror fold end at. At it
              the view spans the whole extent. It is the lower end of the
              search, not a predicate.
     lparam   the widened rect at the level has every edge in [-32768, 32767]
              (tagpu_vpwide_rect_at): the button lParam carries the engine
              point as two int16s (tagpu_vpwide.c, "THE RECT IS THE
              TRANSFORM'S RANGE"). vpwide's refusal calls the same function,
              so the floor and the refusal are one predicate.

   The engine's unit hit test narrows to int16 as well, and is not a term:
   its one call is guarded at any level (tagpu_zoom_hit_guard).

   The search runs on Q16 levels q / 65536 from ceil(max(fit, 1/64) x 65536)
   up to 0.25: the lowest when it is `ok`, else the smallest `ok` one by
   bisection (the rect only grows as the level falls, so `ok` is monotone),
   else 0.25. So the floor is a multiple of 1/65536, and every float compare
   against it, and its Q16 form in a recording, is exact. It FAILS CLOSED to
   0.25 where the viewport or the map cannot be read. Recomputed only when
   one of its inputs moved (the true rect, the extent, the PLOT) or the
   level did (the publisher's serial, which moves at every level end, even
   one whose teardown skipped tagpu_zoom_level_end), and logged
   then: `zoom: floor` names the term that bound it. Game thread, from the
   apply at the top of every in-play draw, before the level is set. */
static int s_floorKey[9], s_floorKeyOk;    /* game thread only */

static int floor_ok(int L, int T, int W, int H, unsigned q)
{
    int aL, aT, aR, aB;
    return tagpu_vpwide_rect_at(L, T, W, H, (float)q / 65536.0f, &aL, &aT, &aR, &aB);
}

static void floor_compute(const char* ta)
{
    int L, T, W, H, extW, extH, mapW, mapH, loX, hiX, loY, hiY, key[9];
    unsigned q = 16384u;
    float fit = 0.0f;
    const char* by;

    tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
    extW = *(const int*)(ta + OFF_EXTENT_W);
    extH = *(const int*)(ta + OFF_EXTENT_H);
    mapW = plot_px(*(const int*)(ta + OFF_PLOT_C));
    mapH = plot_px(*(const int*)(ta + OFF_PLOT_R));
    key[0] = L; key[1] = T; key[2] = W; key[3] = H;
    key[4] = extW; key[5] = extH; key[6] = mapW; key[7] = mapH;
    key[8] = (int)tagpu_packet_pub_level_gen();
    if (s_floorKeyOk && !memcmp(key, s_floorKey, sizeof key)) return;
    memcpy(s_floorKey, key, sizeof key);
    s_floorKeyOk = 1;

    if (W < 64 || W > 16384 || H < 64 || H > 16384 ||
        !camera_range(W, H, extW, extH, mapW, mapH, 1, &loX, &hiX, &loY, &hiY) ||
        hiX <= loX || hiY <= loY) {
        by = "no viewport or map to fit";
    } else {
        double lo, fx = (double)W / (double)(hiX - loX), fy = (double)H / (double)(hiY - loY);
        unsigned qlo;
        fit = (float)(fx < fy ? fx : fy);
        lo = fit > TAGPU_ZOOM_FLOOR_LO ? (double)fit : (double)TAGPU_ZOOM_FLOOR_LO;
        qlo = lo >= 0.25 ? 16384u : (unsigned)ceil(lo * 65536.0);
        if (qlo >= 16384u) {
            by = "0.25";
        } else if (floor_ok(L, T, W, H, qlo)) {
            q = qlo;
            by = fit > TAGPU_ZOOM_FLOOR_LO ? "fit" : "1/64";
        } else if (!floor_ok(L, T, W, H, 16384u)) {
            by = "0.25 (the rect is not int16 even there)";
        } else {
            unsigned bad = qlo;
            while (q - bad > 1u) {
                unsigned mid = bad + (q - bad) / 2u;
                if (floor_ok(L, T, W, H, mid)) q = mid; else bad = mid;
            }
            by = "lparam";
        }
    }
    s_gFloor = (float)q / 65536.0f;
    {
        char b[200];
        _snprintf(b, sizeof b, "zoom: floor %.4f (fit %.4f) on %dx%d, extent %dx%d, map %dx%d - bound by %s",
                  (double)s_gFloor, (double)fit, W, H, extW, extH, mapW, mapH, by);
        b[sizeof b - 1] = 0;
        zlog(b);
    }
}

float tagpu_zoom_game_floor(void)
{
    return s_gFloor;
}

/* ---- the hit test's guard ----------------------------------------------------

   THE ENGINE'S UNIT HIT TEST NARROWS TO INT16, AND NOTHING THE ENGINE KEEPS
   BOUNDS WHAT IT NARROWS. `0x48C6A0(unit, point)` [DISASSEMBLED] takes the
   model's root box from `0x4CB650(MODEL_PTRS[unit+0xA6], &min, &max, 0)`
   (stdcall, `ret 0x10`; flag 0: the root piece's vertices unioned with the
   origin, 16.16), builds four corners of it at min.y (`0x48C6DA..0x48C725`),
   rotates each by the unit's turn with `0x4B6CC0` (three planar rotations,
   `fsincos` and `fistp` in `0x4B7173`: the norm is kept to well under a
   world px), and forms, in 32 bits, `x - (eyeX << 16) + c.x`,
   `alt + c.y` and `z - (eyeY << 16) - c.z` (`0x48C73C..0x48C7A3`), keeping
   bits 16..31 of each (`sar 0x10`, `movsx`). x is `[unit+0x6A]`, the
   altitude `[unit+0x6E]`, z `[unit+0x72]`, all 16.16, and the eye
   main+0x1431F/+0x14323 in px. No engine code bounds the position
   [DISASSEMBLED]: the air course sets an aircraft's target altitude to
   cruisealt plus a byte with no clamp (`0x49074F`), off the map the vertical
   step is skipped so a climb only decays (`0x43D4B8`), no function clamps x
   and z for aircraft, carried units (`0x48A9F0`), the `0x2C` network record's
   raw positions (`0x48B679`) or Teleport (`0x406AA0`). So no margin taken at a
   level load can bound the narrowing.

   THE GUARD IS ON THE ONE CALL. `call 0x48C6A0` at `0x48CE15`, inside
   GetUnitAtMouse `0x48CD80`, is the only reference to `0x48C6A0` in the
   disassembly, and its address as a dword (`A0 C6 48 00`) occurs nowhere in
   the image, so no table or pushed address reaches it. tagpu_vpwide.c
   redirects that call here with the widened rect (the rect is what lets a
   far unit into HotUnits). The engine's test runs only when

     1. the model id `WORD [unit+0xA6]` is not 0 and is below UNITINFOCount
        main+0x1438F, and MODEL_PTRS main+0x14377 and its entry pass the
        pointer filter -- the bound tagpu_native.c's model_root uses. Id 0
        is no model: the load's fill loop starts at 1 (`mov esi,1` at
        `0x42D69E`, `cmp [UNITINFOCount],esi` at `0x42D6B6`) over a block
        `0x4D83B0` does not clear, so entry 0 holds whatever the heap left
        there, and GetUnitAtMouse itself skips id 0 before the call
        (`0x48CE05`) [DISASSEMBLED];
     2. R, the ceiling of the largest norm over the eight corners of that
        root box, in px, plus 2, is at most 8191;
     3. in 64-bit arithmetic, with px the 16.16 value shifted down 16
        (floored, as the engine's `sar`), |x - eyeX| + R, |z - eyeY| + R and
        |alt| + R are each at most 32767;

   and the answer is otherwise "not hit".

   WHY THAT IS A BOUND BY CONSTRUCTION. Two's-complement arithmetic is exact
   modulo 2^32, so each kept value is the true one whenever the TRUE sum lies
   in int32, whatever the intermediates did (`eyeX << 16` itself wraps for
   |eyeX| >= 32768); its high half is then floor(sum / 65536), which lies in
   [px - eye - R, px - eye + R] for a rotated corner of norm at most R, and
   condition 3 puts that inside int16 on all three axes -- so the true sum is
   in int32 and the kept value is exact. `0x4C1320` [DISASSEMBLED] then
   compares, per edge, `(xj - xi)(py - yi)` with `(yj - yi)(px - xi)` as two
   separate signed 32-bit products. The point is the dispatched mouse point,
   which GetUnitAtMouse has already found inside the rect at main+0x37E27
   (`0x4B6720`), whose edges are int16 (tagpu_vpwide_rect_at); a corner is
   x in [-32768 + 0x80, 32767 + 0x80] and y = z - alt/2 + 0x20 within about
   +-49 200. So a point-to-corner difference is under 65 700 in x and
   82 000 in y, and an edge of the rotated box is at most 2R + 1 in x and
   3R + 3 in y; with R <= 8191 each product is under 1.7 x 10^9 < 2^31. A
   unit the guard turns away is one whose engine answer could have wrapped,
   and "not hit" is the only answer that names no wrong unit.

   WHAT IT COSTS. At 1x every HotUnit is on a screen at most 16 384 px wide,
   far inside the limit. Below 1x a unit is unpickable (no hover, no click)
   only while it is more than about 32 767 - R px from the eye or flying
   above about that -- a unit straddling the far edge of a rect the floor
   bound by lparam (super-ultrawide), or an aircraft tens of thousands of px
   up. Counted: `zoom: hit guard` in the log, at most once a second.

   R IS CACHED PER MODEL ID, KEYED ON THE LEVEL: the table is built at the
   first in-play apply of a level, while the 0x48CE15 redirect is in, and is
   stamped with the publisher's level serial (tagpu_packet_pub_level_gen),
   which moves at every level end whichever thread noticed it -- including a
   teardown seen off the game thread, which returns before
   tagpu_zoom_level_end runs. The guard reads the table only while its stamp
   is the current serial. MODEL_PTRS is written at the level load (`0x42D6AA`)
   and nulled only by the teardown (`0x42DCD8`), so the models a table of the
   current level was measured from outlive it; any other call -- no table,
   or one of an older level -- measures on the spot with the same `0x4CB650`
   call, correct and only slower. GetUnitAtMouse runs on the game thread, the
   table's thread.

   A UNIT DRAWN AS AN ICON IS ANSWERED BY ITS ICON, before any of the above
   (icon_hit): the strategic icons' rule (inc/tagpu_icon_rule.h) at the
   game thread's applied level says its model is not drawn, so "hit" is the
   point inside 0.45 of the icon's edge, in world px, about the anchor
   `0x48C6A0` projects -- `((x - (eyeX << 16)) >> 16) + 0x80`,
   `((z - (eyeY << 16)) >> 16) - ((alt >> 16) >> 1) + 0x20`, without its
   corner offsets and computed in 64 bits, without its int16 narrowing. Such
   a unit never reaches `0x48C6A0`, so the narrowing the conditions above
   guard cannot wrap for it. A unit carried by one whose model is hidden is
   never hit (BAR gives cargo no icon of its own); every other unit -- a
   model, a nanoframe, a dead one, a type the class table does not cover, one
   the native pass does not draw -- goes to the guarded engine test as before. HotUnits is the cull and is
   untouched (`0x485596`): an icon changes what a hit test
   answers, not which units are asked. Counted: `zoom: icon pick`, at most
   once a second. */
#define VA_HITTEST       0x0048C6A0u   /* stdcall(unit, point), ret 8           */
#define VA_MODELBOX      0x004CB650u   /* stdcall(model, min*, max*, flag), ret 0x10 */
#define OFF_WATCHED      0x2A42        /* u8 the watched player's id            */
#define OFF_MODELPTRS    0x14377       /* Model3DONode* [UNITINFOCount]         */
#define OFF_UDEFCOUNT    0x1438F       /* u32 UNITINFOCount                     */
#define U_X              0x6A          /* 16.16 world x                         */
#define U_ALT            0x6E          /* 16.16 altitude                        */
#define U_Z              0x72          /* 16.16 world z                         */
#define U_MODELID        0xA6          /* u16 index into MODEL_PTRS, and the
                                          type's UnitDef row                     */
#define U_CARRIER        0x86          /* UnitStruct* that carries it, 0 = none */
#define U_OWNER          0xFF          /* u8 player id                          */
#define U_CLOAKF         0x10E         /* u8, bit 2 = actively cloaked          */
#define U_NANO           0x104         /* float, the build fraction remaining   */
#define U_HEALTH         0x108         /* s16                                   */
#define OFF_UNITBEG      0x14357       /* UnitStruct* the unit array starts at  */
#define OFF_UNITSLOTS    0x14351       /* u16: its slot count, 10 x MaxUnits + 1 */
#define OFF_HOTUNITS     0x1435F       /* u16*: HotUnits, the ids GetUnitAtMouse walks */
#define OFF_HOTCOUNT     0x14367       /* its count */
#define UNIT_STRIDE      0x118
#define OFF_SCREENW      0x37E1F       /* i32 the engine's screen, W and H      */
#define OFF_SCREENH      0x37E23
#define HIT_R_MAX        8191u
#define HIT_R_TOO_BIG    0xFFFFu       /* a model whose R exceeds HIT_R_MAX     */
#define HIT_MODELS       0x10000u      /* every u16 model id                    */

typedef int  (__stdcall *PFN_HITTEST)(const char* unit, const int* point);
typedef void (__stdcall *PFN_MODELBOX)(const void* model, int* mn, int* mx, int flag);

/* game thread only: R per model id, 0 = not measured, of the level whose
   serial + 1 is s_hitGen (0: no table) */
static unsigned short s_hitR[HIT_MODELS];
static unsigned s_hitGen;
static unsigned s_hitRefused;

static int model_ptr_ok(const void* p)
{
    return (size_t)p > 0x10000u && (size_t)p <= tagpu_user_top();
}

/* R of model `m` in px, HIT_R_TOO_BIG above HIT_R_MAX, 0 when there is no
   model to measure. `m` is in [1, UNITINFOCount) (the caller's bound). */
static unsigned hit_measure(const char* ta, unsigned m)
{
    const void* const* ptrs = *(const void* const* const*)(ta + OFF_MODELPTRS);
    int mn[3], mx[3], k;
    double r2 = 0.0, r;

    if (!model_ptr_ok(ptrs) || !model_ptr_ok(ptrs[m])) return 0;
    ((PFN_MODELBOX)VA_MODELBOX)(ptrs[m], mn, mx, 0);
    /* the largest norm over the eight corners: each axis at its larger
       magnitude */
    for (k = 0; k < 3; k++) {
        double a = fabs((double)mn[k]), b = fabs((double)mx[k]);
        double c = a > b ? a : b;
        r2 += c * c;
    }
    r = ceil(sqrt(r2) / 65536.0) + 2.0;
    return r > (double)HIT_R_MAX ? HIT_R_TOO_BIG : (unsigned)r;
}

/* UNITINFOCount when `m` is below it and it is a count a u16 id can index;
   0 otherwise */
static unsigned hit_model_count(const char* ta)
{
    unsigned n = *(const unsigned*)(ta + OFF_UDEFCOUNT);
    return n <= HIT_MODELS ? n : 0u;
}

/* The table of the level whose serial is `gen`, from its first in-play
   apply. Entry 0 stays 0: id 0 is no model. */
static void hit_table_build(const char* ta, unsigned gen)
{
    unsigned n = hit_model_count(ta), m;
    memset(s_hitR, 0, sizeof s_hitR);
    for (m = 1; m < n; m++) s_hitR[m] = (unsigned short)hit_measure(ta, m);
    s_hitGen = gen + 1u;
}

/* Condition 3: the 16.16 value's px (floored, the engine's `sar`) less `eye`,
   plus R, inside int16. */
static int hit_axis_fits(int v1616, long long eye, unsigned r)
{
    long long d = (long long)(v1616 >> 16) - eye;
    return (d < 0 ? -d : d) + (long long)r <= 32767;
}

/* game thread only: the icon picks answered, for the log */
static unsigned s_iconHits, s_iconMisses;

/* Whether `unit`'s model is hidden under its icon at level `z`: 1, with the
   icon's edge in *edge; 0 when it is drawn as a model; -1 when the rule does
   not apply -- a row past the class table, a nanoframe, a unit with no health.
   The row is unit+0xA6, bounded by the table's count, which is the level's
   UNITINFOCount. The icon is the one the picture draws (tagpu_icon_type): a
   mod set's picture, from the art this thread published, or the class's
   shape. */
static int icon_hides(const TAGPU_PK_UTYPE* tab, unsigned n, const char* unit,
                      const TAGPU_ICON_VIEW* v, float z, int* edge)
{
    const unsigned row = *(const unsigned short*)(unit + U_MODELID);
    const TAGPU_ICONRECT* pic;
    int hide, size;
    if (row >= n || tab[row].cls >= TAGPU_ICLS_N) return -1;
    if (*(const float*)(unit + U_NANO) > 0.0f || *(const short*)(unit + U_HEALTH) <= 0) return -1;
    pic = tagpu_icon_type(v, tagpu_icontype_art(), tab[row].cls, tab[row].icon, &size, edge);
    tagpu_icon_eval(v, z, size, *edge, tagpu_icon_cover_r(pic, tab[row].cls, tab[row].r_px), &hide);
    return hide;
}

/* The pick of a unit drawn as an icon: 1 hit, 0 not hit, -1 not an icon (ask
   the engine). The invariant is above `VA_HITTEST`. Every value comes from the
   unit's own record, its carriers', the other listed units', the main block
   and the fog source this thread packs, read on the game thread that owns them inside the engine's own call (`0x48CE15` in
   `0x48CD80`); the row is bounded by the table's count and every carrier by
   the unit array -- a stride-aligned record below the engine's slot count.
   THE PICK FOLLOWS THE PICTURE'S CARGO RULE (TAGPU_ICON_CARGO_HOPS,
   tagpu_icon.c's suppression): it walks up the carrier pointers +0x86 as far
   as the render thread follows the cargo lists down, through carriers the
   unit pass draws only, and a carrier whose model is hidden makes the unit
   part of its picture -- not hit, whatever the unit is, a nanoframe on a
   factory's pad included, since nothing draws it. Otherwise the unit's own
   rule decides, as the picture draws it: cargo of a carrier that shows its
   model is an icon of its own when the rule says so. The two walks read the
   same links from either end [INFERRED: a unit's +0x86 names the unit whose
   cargo list +0x8A/+0x8E holds it, the engine keeping the two together]. The
   pick follows the game thread's applied level and the picture the frame's
   level; the two are equal at rest and differ only while a zoom tween moves --
   a property, not a timing argument: no memory is read across threads. */
/* WHAT THE PICTURE REFUSES IS NOT THERE TO PICK. HotUnits lists a foreign unit
   when any of four box points is in sight (0x465AC0, from 0x48BC56), where the
   gather asks only the anchor's tile, so the list holds units with no icon, or
   only a radar dot, which is never pickable. These are the gather's own gates
   (tagpu_native.c): a foreign unit with no source or cloaked, any unit on
   unexplored ground, a foreign unit out of sight. The source is the one this
   thread packs for the packet, so it can be one packet ahead of the frame the
   player saw, which costs at most one click at a fog edge. */
static int icon_refused(const char* ta, const char* unit)
{
    const int foreign = *(const unsigned char*)(unit + U_OWNER) != *(const unsigned char*)(ta + OFF_WATCHED);
    const short wx = (short)(*(const int*)(unit + U_X) >> 16);
    const short wz = (short)(*(const int*)(unit + U_ALT) >> 16);
    const short wy = (short)(*(const int*)(unit + U_Z) >> 16);
    TAGPU_FOGSRC f;
    int fog;
    if (!tagpu_fogwide_source(&f)) return foreign;
    if (foreign && (*(const unsigned char*)(unit + U_CLOAKF) & 4)) return 1;
    fog = tagpu_fog_at(&f, wx, wy - wz / 2);
    return (fog & 1) || (foreign && (fog & 2));
}

static int icon_circle(const char* ta, const TAGPU_PK_UTYPE* tab, unsigned n,
                       const TAGPU_ICON_VIEW* v, float z, const char* unit, const int* point,
                       long long* dist2)
{
    const char* carrier;
    int edge = 0, h, hop;
    long long ax, ay, dx, dy;
    double r;

    carrier = *(const char* const*)(unit + U_CARRIER);
    for (hop = 0; carrier && hop < TAGPU_ICON_CARGO_HOPS; hop++) {
        const char* beg = *(const char* const*)(ta + OFF_UNITBEG);
        const unsigned slots = *(const unsigned short*)(ta + OFF_UNITSLOTS);
        size_t d;
        int ce;
        if (!model_ptr_ok(beg) || carrier < beg) return -1;
        d = (size_t)(carrier - beg);
        if (d % UNIT_STRIDE || d / UNIT_STRIDE >= slots) return -1;
        /* a carrier the engine draws hides nothing, and the picture's
           suppression stops there too */
        if (!tagpu_native_owns_unit(carrier)) break;
        if (icon_hides(tab, n, carrier, v, z, &ce) == 1) return 0;
        carrier = *(const char* const*)(carrier + U_CARRIER);
    }
    h = icon_hides(tab, n, unit, v, z, &edge);
    if (h <= 0) return -1;
    ax = (((long long)*(const int*)(unit + U_X) - ((long long)*(const int*)(ta + OFF_EYEX) << 16)) >> 16) + 0x80;
    ay = (((long long)*(const int*)(unit + U_Z) - ((long long)*(const int*)(ta + OFF_EYEY) << 16)) >> 16)
         - ((long long)(*(const int*)(unit + U_ALT) >> 16) >> 1) + 0x20;
    dx = (long long)point[0] - ax;
    dy = (long long)point[1] - ay;
    r = 0.45 * (double)edge / (double)z;
    *dist2 = dx * dx + dy * dy;
    return (double)*dist2 <= r * r && !icon_refused(ta, unit);
}

/* THE ICON THE PICK ANSWERS FOR. The engine keeps, among its hits, the smallest
   of a key from each unit's +0x92 record (0x48CE1E..0x48CE75), which says
   nothing about the pointer, so where two icons overlap the pointer on one's
   centre could name the other. So among the hidden-model icons under the
   pointer only one is a hit: the nearest centre, an equal distance going to
   the lower record.

   ONE WALK A PICK. The winner is found at the pick's first hit and kept for
   the rest of its pass. The engine's loop (0x48CDF1..0x48CE8E) has no exit: it
   walks HotUnits (main+0x1435F, count +0x14367) from index 0 and calls this
   for every id past the zero model id it skips (0x48CE05), and 0x48C6A0 has no
   other caller, so the first such id starts a pass and clears the winner. That
   list is 10 * MaxUnits entries and the slot count one more, so a count at the
   slot count is not the list's and the hit stands alone. */
static const char* s_pickWinner;       /* game thread only */

static const char* first_listed(const unsigned short* hot, unsigned count, const char* beg, unsigned slots)
{
    unsigned i;
    for (i = 0; i < count; i++) {
        const unsigned id = hot[i];
        if (id < slots && *(const unsigned short*)(beg + (size_t)id * UNIT_STRIDE + U_MODELID))
            return beg + (size_t)id * UNIT_STRIDE;
    }
    return NULL;
}

/* THE ICONS ARE THOSE THE PICTURE DREW. The render thread draws them only
   while the unit pass and the marker pass both draw (tagpu_native.c's call of
   tagpu_icon_frame), and the command record that ends each render frame
   carries that frame's answer (`icons`, a level like `zoom`; 0 from a frame
   that left before the native pass). The apply copies the latest record at
   the top of each in-play draw, and GetUnitAtMouse runs on this thread from
   the in-game handlers (0x499278, 0x491D18), so a click is answered with the
   answer of the frame whose record the last apply took: the frame the player
   saw, or the one before it at a switch -- the same one-record lag the level
   has -- and never an icon no frame drew. Only a record of this level's epoch
   counts, and the level end clears the answer, so a record posted before the
   render thread saw the level end cannot answer for the next level. No such
   record yet is the engine's test. */
static int icon_hit(const char* ta, const char* unit, const int* point)
{
    unsigned n, i;
    const TAGPU_PK_UTYPE* tab = tagpu_icontype_table(&n);
    const float z = game_level();
    const unsigned short* hot = *(const unsigned short* const*)(ta + OFF_HOTUNITS);
    const unsigned count = *(const unsigned*)(ta + OFF_HOTCOUNT);
    const char* beg = *(const char* const*)(ta + OFF_UNITBEG);
    const unsigned slots = *(const unsigned short*)(ta + OFF_UNITSLOTS);
    const int listed = model_ptr_ok(hot) && model_ptr_ok(beg) && count < slots;
    TAGPU_ICON_VIEW v;
    int scale, dist, fade, hit;
    long long nearest = 0;

    if (listed && unit == first_listed(hot, count, beg, slots)) s_pickWinner = NULL;
    /* a unit the native pass does not draw keeps the engine's sprite and no
       icon -- the gather's own first test, asked here on this thread as the
       packet's fill asks it */
    if (!s_gIcons || !tab || !tagpu_native_owns_unit(unit)) return -1;
    tagpu_settings_icons(&scale, &dist, &fade);
    tagpu_icon_view(&v, *(const int*)(ta + OFF_SCREENW), *(const int*)(ta + OFF_SCREENH),
                    scale, dist, fade);
    hit = icon_circle(ta, tab, n, &v, z, unit, point, &nearest);
    if (hit != 1 || !listed) return hit;
    if (!s_pickWinner) {
        s_pickWinner = unit;
        for (i = 0; i < count; i++) {
            const unsigned id = hot[i];
            const char* other = beg + (size_t)id * UNIT_STRIDE;
            long long theirs = 0;
            if (id >= slots || other == unit || !*(const unsigned short*)(other + U_MODELID)) continue;
            if (!tagpu_native_owns_unit(other)) continue;
            if (icon_circle(ta, tab, n, &v, z, other, point, &theirs) == 1 &&
                (theirs < nearest || (theirs == nearest && other < s_pickWinner))) {
                s_pickWinner = other;
                nearest = theirs;
            }
        }
    }
    return unit == s_pickWinner;
}

unsigned tagpu_zoom_hit_radius(const char* ta, unsigned m)
{
    /* model id 0 is the slot the engine never writes (see hit_table_build) */
    if (m == 0 || m >= hit_model_count(ta)) return 0;
    if (s_hitGen == tagpu_packet_pub_level_gen() + 1u && s_hitR[m]) return s_hitR[m];
    return hit_measure(ta, m);
}

int __stdcall tagpu_zoom_hit_guard(const char* unit, const int* point)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    unsigned m = 0, r = 0, n;

    if (ta_ok(ta) && model_ptr_ok(unit)) {
        const int icon = icon_hit(ta, unit, point);
        if (icon >= 0) {
            static DWORD tick;               /* game thread only */
            DWORD now = GetTickCount();
            if (icon) s_iconHits++; else s_iconMisses++;
            if (now - tick > 1000) {
                char b[120];
                tick = now;
                _snprintf(b, sizeof b, "zoom: icon pick - %u hit(s), %u miss(es) so far", s_iconHits,
                          s_iconMisses);
                b[sizeof b - 1] = 0;
                zlog(b);
            }
            return icon;
        }
        m = *(const unsigned short*)(unit + U_MODELID);
        n = hit_model_count(ta);
        if (m != 0 && m < n) {
            r = s_hitGen == tagpu_packet_pub_level_gen() + 1u ? s_hitR[m] : 0u;
            if (!r) r = hit_measure(ta, m);
        }
        if (r && r <= HIT_R_MAX &&
            hit_axis_fits(*(const int*)(unit + U_X), *(const int*)(ta + OFF_EYEX), r) &&
            hit_axis_fits(*(const int*)(unit + U_Z), *(const int*)(ta + OFF_EYEY), r) &&
            hit_axis_fits(*(const int*)(unit + U_ALT), 0, r))
            return ((PFN_HITTEST)VA_HITTEST)(unit, point);
    }
    s_hitRefused++;
    {
        static DWORD tick;                   /* game thread only */
        DWORD now = GetTickCount();
        if (now - tick > 1000) {
            char b[200];
            tick = now;
            _snprintf(b, sizeof b,
                      "zoom: hit guard - %u unit hit test(s) answered \"not hit\" so far, whose int16 "
                      "narrowing could wrap (the last: model %u, R %u)",
                      s_hitRefused, m, r);
            b[sizeof b - 1] = 0;
            zlog(b);
        }
    }
    return 0;
}

float tagpu_zoom_applied_level(void)
{
    return s_gLevel;
}

float tagpu_zoom_reach_level(void)
{
    return s_gReach;
}

/* the game thread's: the latest cut applied, and one applied since the recorded
   camera last said so (tagpu_zoom_cut_latched) */
static unsigned s_gCutGen;
static int      s_gCutLatch;
static float    s_gFolCap;                 /* a Follow's rung; 0 none ("the reach ladder") */
static unsigned s_gFolLast;                /* the Follow whose rung it is */

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

/* ---- the replay's Follow and Jump, the game thread's half -------------------

   THE MECHANISM (demo-recorder-perspective-streams.md, "Follow's mechanism"):
   the replay's frame visit hands the eye of the recorded seat's camera, fitted
   to this screen, to tagpu_zoom_follow before the draw call 0x4969CD; the
   apply at the draw's top writes it, with the scroll target, after every
   engine camera writer of the frame (the stepper and the scroll poll precede
   the draw call; nothing inside DrawGameScreen stores the eye); and the
   packet that draw publishes carries the epoch and its level to the render
   thread ("the follow lever").

   FOLLOW ENDS ON AN OBSERVED FACT, NOT A GUESS. Every camera move a player
   makes writes the scroll target — the nine eye writers copy the eye into it,
   the centre-ons store it, the stepper's follow source writes it (the exe
   map's camera module) — and between two applies nothing else does: the eye
   clamp 0x41C3C0 touches only the eye, the stepper eases only the eye unless
   it follows a unit, an object or a remembered position, and the epoch's
   first write released all three (release_follow). So a target at the apply's entry that is not where the
   last write left it, after its clamp, IS a camera move of someone else's,
   and the epoch ends there. The render thread ends one by value: a record
   whose follow_broken is the epoch in force (a wheel notch), or for a Jump
   one whose follow_seen is (its level drawn). Epochs are the replay's, unique
   in the process, compared only for equality with the one in force, so no
   record or packet from before an epoch began, or after it ended, can act on
   it. All of it is game-thread state, read and written by this thread
   alone. */
static struct {
    unsigned epoch;            /* in force, 0 none                                   */
    unsigned last;             /* the latest epoch ever handed in: none at or below
                                  it starts again                                    */
    int      once;             /* a Jump                                             */
    int      x, y;             /* the eye the next write puts the camera at          */
    unsigned level;            /* its level x 65536, for the packet                  */
    int      wrote;            /* the epoch's first write is done ...                */
    int      tx, ty;           /* ... and left the target here, clamped              */
    unsigned ended;            /* the latest epoch to end ...                        */
    int      why;              /* ... and why (TAGPU_FOLLOW_*)                        */
} s_fol;

static void follow_end(int why)
{
    s_fol.ended = s_fol.epoch;
    s_fol.why = why;
    s_fol.epoch = 0;
}

/* The epoch's end tests, at the entry of a write: the target moved under it,
   or the render thread's record ended it. `c` may be NULL (no record yet, or
   no apply at all). */
static void follow_check(const char* ta, const TAGPU_CMD* c)
{
    const int* scr = (const int*)(ta + OFF_SCRTX);
    if (!s_fol.epoch || !s_fol.wrote) return;
    if (scr[0] != s_fol.tx || scr[1] != s_fol.ty) follow_end(TAGPU_FOLLOW_MOVED);
    else if (c && c->follow_broken == s_fol.epoch) follow_end(TAGPU_FOLLOW_ZOOMED);
    else if (c && s_fol.once && c->follow_seen == s_fol.epoch) follow_end(TAGPU_FOLLOW_DRAWN);
}

/* GAME THREAD, ahead of the apply. The record this draw's camera and rect take:
   this one, or while it names a cut not yet applied and a mouse record waits in
   the ring, the last one taken ("a mouse record is handled through one view"). A
   record's fields are absolute, so taking the last again changes nothing, and the
   cut's own cmd_seq is unacknowledged until it goes through. */
const TAGPU_CMD* tagpu_zoom_cut_gate(const TAGPU_CMD* c)
{
    static TAGPU_CMD last;
    static int       have;
    int mouse = 0, key = 0;

    if (!c) return c;
    if (c->cut_gen != s_gCutGen) {
        if (have && tagpu_gui_input_rings(&mouse, &key) && mouse > 0) return &last;
        s_gCutGen = c->cut_gen;
        if (c->epoch == s_epoch) s_gCutLatch = 1;
    }
    if (!have || last.cmd_seq != c->cmd_seq) { last = *c; have = 1; }
    return c;
}

int tagpu_zoom_cut_latched(void)
{
    return s_gCutLatch;
}

void tagpu_zoom_cut_clear(void)
{
    s_gCutLatch = 0;
}

void tagpu_zoom_apply(char* ta, const TAGPU_CMD* c, int terr)
{
    int* eye = (int*)(ta + OFF_EYEX);
    int* scr = (int*)(ta + OFF_SCRTX);
    int  loX, hiX, loY, hiY, moved = 0, applied = 0;

    /* THE LEVEL FIRST: every game-thread reader in this file — the minimap
       rect's scale, the scroll step — uses the level the last record carried,
       never the render thread's own float. A frame that drew nothing zoomed
       posts live = 0, which is 1.0 here: the engine's own rect and rate come
       back at the next draw. */
    s_applyLive = 1;
    world_held_stale();
    floor_compute(ta);
    {
        unsigned gen = tagpu_packet_pub_level_gen();
        if (tagpu_vpwide_mouse_world_live() && s_hitGen != gen + 1u) hit_table_build(ta, gen);
    }
    s_gLevel  = c ? c->zoom : 1.0f;
    s_gLive   = c ? (int)c->live : 0;
    /* THE LEVEL AT OR ABOVE THIS THREAD'S FLOOR: the render thread's floor is
       a frame old, and every game-thread consumer of the level -- the rect,
       the minimap box, the scroll step, the recorder -- reads one at or above
       the floor computed here. The record passed cmd_valid, so the level is
       finite and at least TAGPU_ZOOM_FLOOR_LO. */
    if (s_gLevel < s_gFloor) s_gLevel = s_gFloor;
    /* the tables' reach: the record's rung, never below the floor, and the
       floor (the widest) with no record -- the safe default */
    s_gReach = c ? c->reach_zoom : s_gFloor;
    /* A FOLLOW'S PACKETS ARE THEIR OWN REQUEST ("the reach ladder") */
    if (s_fol.epoch) {
        s_gFolCap = tagpu_zoom_rung((float)s_fol.level / 65536.0f, s_gFloor);
        s_gFolLast = s_fol.epoch;
    } else if (c && c->follow_free == s_gFolLast) {
        s_gFolCap = 0.0f;
        s_gFolLast = 0;
    }
    if (s_gFolCap > 0.0f && s_gFolCap < s_gReach) s_gReach = s_gFolCap;
    /* the icons answer the pick only from a record of THIS level's epoch: a
       record the render thread posted before it saw the level end is the old
       level's frame, and the level end cleared the answer, so no click is
       answered by circles no frame of this level drew */
    s_gIcons = (c && c->epoch == s_epoch) ? (int)c->icons : 0;
    if (s_gReach < s_gFloor) s_gReach = s_gFloor;
    if (s_gReach > TAGPU_ZOOM_FLOOR_HI) s_gReach = TAGPU_ZOOM_FLOOR_HI;
    /* THEN THE RANGE: centred only on a draw whose ground is ours. `terr` is
       the request the publisher latches for this same draw right after this
       call (`terr || rect still wide`), so the centre range is in force only
       where the engine's terrain pass is skipped — see "the camera's range".
       And only when it can be computed: a draw whose engine state is not sane
       enough for it takes the engine's own range and publishes `centre = 0`,
       so the eye is inside `[0, extent - W]` whoever draws the ground. */
    s_gCentre = g_eyeInstalled && terr;
    if (!clamp_range(ta, &loX, &hiX, &loY, &hiY)) s_gCentre = 0;

    /* THE REPLAY'S FOLLOW, BEFORE ANYTHING HERE WRITES THE CAMERA: the target
       at entry is what the frame's engine writers left, so this is the test
       of "someone else moved it". A scenario camera handed over for this
       draw is such a move too. An epoch's first write releases the engine's
       own follow, so an earlier Ctrl+C cannot have the stepper move the
       target and end the epoch on its next frame. */
    if (s_fol.epoch) {
        follow_check(ta, c);
        if (s_fol.epoch && s_placeOn) follow_end(TAGPU_FOLLOW_MOVED);
        if (s_fol.epoch && !s_fol.wrote) release_follow(ta, "the replay's follow");
    }

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
            if (c->drop_follow) release_follow(ta, "cursor anchor");
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
       draw that moves the eye is the draw that rebuilds the engine's fog grid
       and the packet this draw publishes carries the eye it was drawn at
       (tagpu_zoom_place_eye). */
    if (s_placeOn) {
        s_placeOn = 0;
        eye[0] = s_placeX; eye[1] = s_placeY;
        scr[0] = s_placeX; scr[1] = s_placeY;
        moved = applied = 1;
    }

    /* THE REPLAY'S FOLLOW WRITES THE CAMERA, every draw while its epoch
       stands, the eye and the target together and clamped into the range as
       the hold is; the hold, when on, wins. Written only when it differs, so
       a still camera costs no fog rebuild. */
    if (s_fol.epoch && !(c && c->hold_on)) {
        int x = s_fol.x, y = s_fol.y;
        clamp_pair(&x, &y, loX, hiX, loY, hiY);
        if (eye[0] != x || eye[1] != y) { eye[0] = x; eye[1] = y; moved = 1; }
        if (scr[0] != x || scr[1] != y) { scr[0] = x; scr[1] = y; }
        applied = 1;
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
    /* the target this apply leaves, which the next one tests the epoch on */
    if (s_fol.epoch) {
        s_fol.tx = scr[0]; s_fol.ty = scr[1];
        s_fol.wrote = 1;
    }

    /* A CAMERA WE MOVED OWES THE ENGINE WHAT ITS OWN WRITERS DO. Nine of the
       eleven (exe map, `main+0x142F1` bit 1) store the eye, set bit 1 of
       main+0x142F1 (`orb $2`, e.g. `0x41C598`, `0x41D04D`), call `0x41C3C0`
       — which only clamps the eye and recomputes the minimap's view box
       through `0x466B70`, touching neither flag nor the target — then copy
       the eye into the target and clear bit 3 of main+0x14281. The stepper's
       two (`0x41CB5F`, `0x41CBCF`) set bit 1 and clear bit 3, then step the
       eye toward the target, which they never write. So: the view box
       recomputed, `0x41C3C0` being the only place the engine ever fills it;
       bit 1 set, the dirty flag DrawMinimap `0x466B00` alone tests
       (`0x466B06..0x466B16`) — display state, like the word's other bits; the
       minimap's radar rebuild `0x466DC0` also sets it on every sim tick for
       the viewed record main+0x2A43 (`0x46742F`), so this write decides something only on
       a frame no tick ran; and bit 3 cleared,
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
    } else if (g_mmInstalled && game_level() != s_rectLevel) {
        /* THE BOX IS A FUNCTION OF THE EYE AND THE LEVEL, so a level that changed under a still
           eye owes it too: nothing else rescales it until the eye moves (MEASURED 2026-10-07: a
           paused replay kept the 1x box through wheel steps to 0.456 and 0.25). Only the box
           and its redraw bit: the level changed, the camera did not. */
        zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
        *(unsigned char*)(ta + OFF_MM_DIRTY) |= 2u;
    }
}

void tagpu_zoom_level_end(char* ta)
{
    s_gLevel = 1.0f; s_gLive = 0;
    /* the floor fails closed until the next level's first apply computes its
       own */
    s_gFloor = s_gReach = TAGPU_ZOOM_FLOOR_HI;
    s_floorKeyOk = 0;
    /* no frame of the next level has drawn an icon yet */
    s_gIcons = 0;
    /* the hit guard's radii are this level's models': the serial has moved,
       so the table is no longer read, and the next level's first apply
       builds its own */
    /* a new epoch: nothing owed to the old level survives into the next */
    s_epoch++;
    s_appliedSeq = 0; s_appliedDx = 0; s_appliedDy = 0;
    s_gCutLatch = 0;
    s_gFolCap = 0.0f;
    s_gFolLast = 0;
    s_placeOn = 0;                        /* a placement is the old level's */
    s_applyLive = 0;
    if (s_fol.epoch) follow_end(TAGPU_FOLLOW_LEVEL);
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

/* The write where no apply runs (tagpu_zoom_follow): the same end tests with
   no record, the release on the epoch's first write, the pair clamped into
   the range in force, and, when the camera moved, what place_eye's own write
   owes the engine. Nothing draws a level here, so a Jump ends with its
   write. */
static int follow_direct(char* ta)
{
    int* eye = (int*)(ta + OFF_EYEX);
    int* scr = (int*)(ta + OFF_SCRTX);
    int  loX, hiX, loY, hiY, x = s_fol.x, y = s_fol.y, moved = 0;

    follow_check(ta, NULL);
    if (!s_fol.epoch) return 0;
    if (!s_fol.wrote) release_follow(ta, "the replay's follow");
    clamp_range(ta, &loX, &hiX, &loY, &hiY);
    clamp_pair(&x, &y, loX, hiX, loY, hiY);
    if (eye[0] != x || eye[1] != y || scr[0] != x || scr[1] != y) {
        eye[0] = x; eye[1] = y;
        scr[0] = x; scr[1] = y;
        zoom_minimap_rect((int*)(ta + OFF_MM_RECT));
        *(unsigned char*)(ta + OFF_MM_DIRTY) |= 2u;
        *(unsigned short*)(ta + OFF_LOSTYPE) &= (unsigned short)~8u;
        moved = 1;
    }
    s_fol.tx = x; s_fol.ty = y;
    s_fol.wrote = 1;
    if (s_fol.once) follow_end(TAGPU_FOLLOW_DRAWN);
    return moved;
}

int tagpu_zoom_follow(char* ta, int eyeX, int eyeY, unsigned levelQ16, unsigned E, int once)
{
    if (!E || !ta_ok(ta)) return 0;
    if (E != s_fol.epoch) {
        if (E <= s_fol.last) return 0;         /* ended, or older than one that did */
        if (s_fol.epoch) follow_end(TAGPU_FOLLOW_STOPPED);
        s_fol.epoch = s_fol.last = E;
        s_fol.once = once;
        s_fol.wrote = 0;
    }
    s_fol.x = eyeX; s_fol.y = eyeY;
    s_fol.level = levelQ16;
    return s_applyLive ? 0 : follow_direct(ta);
}

void tagpu_zoom_follow_stop(unsigned E)
{
    if (E && E == s_fol.epoch) follow_end(TAGPU_FOLLOW_STOPPED);
}

int tagpu_zoom_follow_state(unsigned E)
{
    if (!E) return TAGPU_FOLLOW_UNKNOWN;
    if (E == s_fol.epoch) return TAGPU_FOLLOW_ACTIVE;
    return E == s_fol.ended ? s_fol.why : TAGPU_FOLLOW_UNKNOWN;
}

/* Published only once the apply has written for the epoch, so a packet that
   names it carries the eye it wrote (or the hold's, which wins). */
void tagpu_zoom_follow_pub(unsigned* epoch, unsigned* levelQ16, unsigned* last)
{
    int on = s_fol.epoch && s_fol.wrote;
    *epoch = on ? s_fol.epoch : 0u;
    *levelQ16 = on ? s_fol.level : 0u;
    *last = s_gFolLast;
}

void tagpu_zoom_applied(unsigned* seq, int* cum_dx, int* cum_dy, float* level,
                        unsigned* epoch, unsigned* centre)
{
    *seq = s_appliedSeq; *cum_dx = s_appliedDx; *cum_dy = s_appliedDy;
    *level = game_level(); *epoch = s_epoch; *centre = s_gCentre ? 1u : 0u;
}

/* ---- the end of the render frame: the command record ----------------------- */

/* the cursor anchor's state, render thread only (below) */
static int s_ackX, s_ackY;                 /* ...and what the last packet acknowledged  */
static unsigned s_epochSeen;               /* the cmd_epoch of the last packet seen     */

void tagpu_zoom_frame_end(void)
{
    TAGPU_CMD rec;
    if (s_liveHeld > 0) s_liveHeld--;
    else if (!s_fresh) s_live = 0;
    s_fresh = 0;
    /* THE RECORD: this frame's level and whether a zoomed world is on screen
       (the game thread derives the addressable rect, the minimap box and the
       scroll step from the pair), the anchor's cumulative delta, whether any
       of it is still unacknowledged (the follow release rides that, so a
       record the game thread never took loses nothing: the next one carries
       the same request), and the camera hold. Posted on EVERY path out of the
       overlay frame, so a frame that drew nothing zoomed hands the engine its
       own rect and rate back at the next in-play draw. */
    memset(&rec, 0, sizeof rec);
    /* a cut's level goes with its eye step, while this thread still draws the
       old view ("the mega map's cut") */
    rec.zoom        = s_cutHold ? s_wheelCur : s_zoom;
    rec.cut_gen     = s_cutGen;
    rec.live        = s_live ? 1u : 0u;
    rec.epoch       = s_epochSeen;
    rec.cum_dx      = s_cumX;
    rec.cum_dy      = s_cumY;
    rec.drop_follow = (s_cumX != s_ackX || s_cumY != s_ackY) ? 1u : 0u;
    rec.follow_broken = s_followBroken;
    rec.follow_seen   = s_followSeen;
    rec.follow_free   = s_followFree;
    s_followFree      = 0;
    /* whether this frame drew the icons: 0 on every path that left before
       the native pass decided them, as on a frame that decided none */
    rec.icons       = tagpu_icon_cmd_take() ? 1u : 0u;
    /* THE REACH REQUEST: the rung of the lowest level this view may draw
       before its next record is applied -- the level drawn, the wheel's
       target (so a notch out asks for its target on the frame it arrives)
       and the level the levers named -- held in the ring once posted ("the
       reach ladder") */
    {
        float r = s_zoom;
        if (s_wheelTgt < r) r = s_wheelTgt;
        if (s_asked < r) r = s_asked;
        rec.reach_zoom = tagpu_zoom_rung(r, s_floor);
    }
    tagpu_input_cmd(&rec);
    tagpu_hudstate_cmd(&rec.hud);
    tagpu_gui_sweep_cmd(&rec.sweep);
    tagpu_wikistate_cmd(&rec.wiki);
    if (tagpu_cmd_post(&rec)) {
        reach_posted(tagpu_cmd_posted_seq(), rec.reach_zoom);
        if (s_cutOwed && !s_cutSeq) s_cutSeq = tagpu_cmd_posted_seq();
    }
}

float tagpu_zoom_level(void)
{
    return s_live ? s_zoom : 1.0f;
}

/* Snapshot the published view. Returns 0 when there is nothing to do —
   the caller then leaves the point alone. */
static int view(float* z, float* cx, float* cy, int* L, int* T, int* W, int* H)
{
    LONG  owed = s_cutOwed;
    float zz;
    MemoryBarrier();                            /* the flag, then the level it says to read */
    zz = !s_live ? 1.0f : !owed ? s_zoom : s_gLive ? s_gLevel : 1.0f;
    if (!(zz >= TAGPU_ZOOM_FLOOR_LO) || zz == 1.0f) return 0;
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

   The executable mapping and TA_MAINPP live for the process, and ta_ok guards
   the level pointer before the byte read, so this answers in the shell as well. */
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
        InterlockedExchange(&s_worldHeld[slot], 0);
        return drop;
    }

    x = (int)(short)LOWORD(lparam);
    y = (int)(short)HIWORD(lparam);
    /* a point the render-options UI owns is never in the ring: it is screen
       furniture, not world, and dropping its press would swallow the click */
    if (tagpu_menu_owns_point(x, y)) { s_dropped[slot] = 0; return 0; }
    /* A POINT ON THE WORLD DURING A CUT HAS NO RIGHT ANSWER ("a mouse record is
       handled through one view"); the panels are mapped 1:1 and are not touched.
       Not view(), which answers 0 at level 1, a level a cut starts from. */
    {
        int world = !options_gui_owns_input() &&
                    in_viewport(x, y, (int)s_vpL, (int)s_vpT, (int)s_vw, (int)s_vh);
        if (world) {
            InterlockedExchange(&s_worldHeld[slot], 1);
            if (s_live && s_cutOwed) {
                InterlockedExchange(&s_worldHeld[slot], 0);
                s_dropped[slot] = 1;
                return 1;
            }
        }
    }
    /* the return value is "was it transformed", which is 0 in the ring — `ring`
       is the flag to read, and it is the only one that matters here */
    to_engine(&x, &y, &ring);
    if (ring) InterlockedExchange(&s_worldHeld[slot], 0);
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
   every rect derived from the viewport (vpwide's addressable rect, the tables'
   reach, the ring test) currently assumes away.

   THE FOG. The game thread's apply invalidates the screen fog grid on the
   draw it steps the eye (bit 3 of main+0x14281, cleared on the thread that
   owns it), so the engine's grid is rebuilt for the commanded eye inside that
   same draw whether the fog draw is the engine's or terrown's replica. Our
   passes do not read that grid: they expand the whole map's fog source the
   packet carries (tagpu_fogsrc.h), which no eye can outrun, so the frame drawn
   from the PREDICTED eye before that packet arrives has its fog wherever it
   looks. What that frame needs bounded is its gather, and the reach bound
   (predict) clamps the drawn eye into the rect the packet's tables cover.

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
    /* NOTHING OWED — the common case, and every zoom-out that cut no zoom-in
       and engaged no mega map.
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

/* THE DISPLACEMENT ENGAGING THE MEGA MAP OWES: from the eye the wheel's
   gesture lands at as of this notch to the map's centre (mega_eye). The eye
   it lands at is this frame's (the packet's plus what is posted and not yet
   acknowledged, as anchor_step steps from), plus the carry, what the tween
   this batch cuts still owes, and the terms this frame's earlier notches have
   already added. Paid through R like any notch's term, so the cut posts it with
   the level and anchor_step clamps it into the range per axis. 0 with no packet
   or no range: the level still falls, about the view centre. Render thread,
   from wheel_level. */
static int mega_centre_shift(const TAGPU_PACKET* pk, float* dx, float* dy)
{
    float tx, ty, owed = (float)(1.0 - s_gPosted);

    if (!pk || !pk->in_game || !mega_eye(pk, &tx, &ty)) return 0;
    *dx = tx - ((float)(pk->eye[0] + (s_cumX - pk->cmd_ack_dx)) + s_residX + s_remX * owed + s_shiftX);
    *dy = ty - ((float)(pk->eye[1] + (s_cumY - pk->cmd_ack_dy)) + s_residY + s_remY * owed + s_shiftY);
    return 1;
}

/* The eye interval that puts `[e + dL, e + dR]` inside `[lo, hi]`, and `*e`
   clamped into it; returns 1. An empty interval centres the span on
   `[lo, hi]` and returns 0. */
static int reach_fit(int* e, int dL, int dR, int lo, int hi)
{
    int a = lo - dL, b = hi - dR;
    if (a > b) { *e = a + (b - a) / 2; return 0; }
    *e = clampi(*e, a, b);
    return 1;
}

/* THE REACH BOUND ON THE DRAWN EYE. The invariant: every unit, wreck, ghost and
   feature anchor this frame gathers lies inside the rect the packet's tables
   were built over -- its `reach_lo/hi` (tagpu_packet_pub.c, table_reach), the
   window tagpu_zoom_pub_window publishes about the packet's eye. A unit inside
   it carries its pieces, the anchor table holds its rows and the effects
   models were culled against it, so a frame that gathers inside it finds every
   table covering its slab. It rests on a bound and not on the game thread
   keeping up: the slab comes from this frame's level and viewport, the reach
   from the packet's own numbers, and the eye is clamped until one lies inside
   the other, for any gesture, reversal or lag.

   THE SLAB, per axis (vh for y): `[e + (vw - S)/2 - M, e + (vw - S)/2 + S + M]`,
   S = tagpu_zoom_gather_span(vw, z, floor) and M = TAGPU_GATHER_MARGIN — the native
   pass's own test, whose S is this one (the native pass only raises it to
   the viewport, which it already is or exceeds); the slab contains the
   view. At every level, the 1x one included.

   WHAT IT HOLDS. The reach is the slab at the packet's reach level about its
   eye, grown by the lead on each side, and the frame is drawn at no level
   below the reach level (the reach ladder), so S never exceeds the reach's
   slab and the interval holds every eye within the lead of the packet's, at
   every level: at z >= 1 the slab is the 1x one and the reach at least the
   0.25 one, and the clamp could bind only on a step of 1.75 viewports.
   The clamp is the identity for every gesture that only zooms in (each notch's
   eye term is at most `(vw/2)` times its drop in distance, and the eye and the
   distance are lerped along the same ease, so the view stays a subset of the
   one before it) -- except a leaving of `megazoom=tadr`, whose eye term is
   `(a - c)` times the whole distance it leaves from (up to half a viewport times
   1/floor), so its new view is not inside the old one and only a cut past its
   deadline can be held here ("the mega map's cut") -- and for any displacement
   the game thread has not applied
   that is within the lead; what it holds back is a larger one, and the drawn
   eye waits at the edge of the reach for as many frames as that takes, instead
   of drawing past it. While a frame is held, a click is still mapped by the
   game thread through the eye it applies, up to the hold from where it is
   drawn. The interval is empty only for a packet whose reach was not taken for
   its own viewport; the domain is then centred on the reach, `*fit` is 0, and
   the native pass counts any frame whose slab is not inside the reach (`out=`)
   as the bound's witness. */
static void reach_bound(const TAGPU_PACKET* pk, float z, int* ex, int* ey, int* fit)
{
    int vw = pk->vp[2], vh = pk->vp[3], x = *ex, y = *ey;
    int sw, sh, dLx, dRx, dLy, dRy;

    *fit = 1;
    if (vw <= 0 || vh <= 0) return;
    sw = tagpu_zoom_gather_span(vw, z, s_floor); sh = tagpu_zoom_gather_span(vh, z, s_floor);
    dLx = (vw - sw) / 2 - TAGPU_GATHER_MARGIN; dRx = dLx + sw + 2 * TAGPU_GATHER_MARGIN;
    dLy = (vh - sh) / 2 - TAGPU_GATHER_MARGIN; dRy = dLy + sh + 2 * TAGPU_GATHER_MARGIN;
    *fit = reach_fit(&x, dLx, dRx, pk->reach_lo[0], pk->reach_hi[0]) &
           reach_fit(&y, dLy, dRy, pk->reach_lo[1], pk->reach_hi[1]);
    *ex = x; *ey = y;
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
   the reach bound; during a cut's hold, the last frame's, by the same bound.
   `z` is the level the levers ask for; the level to draw at
   is returned.

   THE DRAWN VIEW NEVER MOVES AGAINST THE GESTURE, on either axis. The bound's
   upper end `reach_hi - (vw + S)/2 - M` falls as the level falls (S grows), so
   by itself it walks a held eye backward frame by frame during a zoom-out that
   cuts a zoom-in whose displacement the game thread has not applied, and snaps
   it forward when a packet lands — a sawtooth, one tooth per packet
   (gpu-status §2.3 has the numbers). When the bound would step the eye away
   from where the gesture puts it, the frame is drawn at the LAST frame's level
   instead, so the zoom pauses with the eye; the eye then moves only toward the
   gesture. The eye is clamped for that level by the same bound, so the reach
   invariant is untouched: the monotone rule only picks which level the bound
   is evaluated at.
   Why the last level is enough: for one packet its interval at that level is
   the one that last frame's eye was clamped into, so that eye is still in
   it, and every eye between it and `u` is too. A NEWER packet's interval at a
   level is non-decreasing in the eye it was published about (the reach is
   that eye's slab at the reach level grown by the lead, its size fixed by the
   viewport and the packet's reach_level, which is at most the level last
   drawn: the reach ladder), so
   it still holds last frame's eye unless the packet's own eye moved against
   the gesture — the game thread applying a step of another gesture, or its own
   camera writers. Only then is the step taken anyway, and counted (`back=` in
   the native heartbeat): the reach invariant is the one that cannot give.
   THE RULE NEEDS AN INTERVAL. Where the one at this level is empty, the bound
   centres the domain on the reach, and a centred eye is not a step the gesture
   asked for in either direction: pausing on it would hold the last level for
   as long as that lasted. So the rule is applied only when the bound fitted
   the eye into a non-empty interval, and the retry is taken only when it did
   too; a centred frame is drawn as the bound left it and counted by the native
   pass (`out=`). The retry draws at the last frame's level only while that
   level is at or above the reach ladder's for this frame (the floor, this
   packet's reach level, every rung in flight): below it the slab could leave
   the reach of this packet or of one still to come. */
static float predict(const TAGPU_PACKET* pk, float z, int mayHold)
{
    int ux, uy, ex, ey, loX, hiX, loY, hiY, fit;
    s_havePred = 0;
    if (!pk) { s_haveDrawn = 0; cut_end(); return z; }
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
        cut_end();
    }
    if (!pk->in_game) { s_haveDrawn = 0; cut_end(); return z; }
    s_ackX = pk->cmd_ack_dx; s_ackY = pk->cmd_ack_dy;
    ux = s_cumX - s_ackX; uy = s_cumY - s_ackY;
    ex = pk->eye[0] + ux; ey = pk->eye[1] + uy;
    if (range_pk(pk, &loX, &hiX, &loY, &hiY))
        clamp_pair(&ex, &ey, loX, hiX, loY, hiY);
    ux = ex; uy = ey;
    /* THE MEGA MAP'S CUT (above s_megaOn): the last frame's view until the
       packet can draw the new one whole. The hold draws no level below the
       ladder's, and goes through the bound like any other frame. */
    if (s_cutHold) {
        LONG el = (LONG)(qpc_us() - s_cutUs);
        if ((z == s_asked && cut_acked(pk)) ||
            el >= CUT_HOLD_US || !s_haveDrawn || s_drawnZ < s_zmin) {
            s_cutHold = 0;
            s_landMs = el > 0 ? (float)el / 1000.0f : 0.0f;
        } else {
            ex = s_drawnX; ey = s_drawnY; z = s_drawnZ;
            reach_bound(pk, z, &ex, &ey, &fit);
            s_reachPaused++;
            s_predX = ex; s_predY = ey; s_havePred = 1;
            s_drawnX = ex; s_drawnY = ey;
            return z;
        }
    }
    /* LAST, so nothing moves the eye after the bound is established */
    reach_bound(pk, z, &ex, &ey, &fit);
    if (mayHold && fit && s_haveDrawn &&
        (against(ex, ux, s_drawnX) || against(ey, uy, s_drawnY))) {
        int hx = ux, hy = uy, hfit;
        reach_bound(pk, s_drawnZ, &hx, &hy, &hfit);
        if (hfit && s_drawnZ >= s_zmin &&
            !against(hx, ux, s_drawnX) && !against(hy, uy, s_drawnY)) {
            ex = hx; ey = hy; z = s_drawnZ;
            s_reachPaused++;
        } else {
            s_reachBack++;
        }
    }
    if (ex != ux || ey != uy) {
        int d = abs(ex - ux) > abs(ey - uy) ? abs(ex - ux) : abs(ey - uy);
        s_reachHeld++;
        if (d > s_reachHeldMax) s_reachHeldMax = d;
    }
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

#define SCROLL_POLL_VA   0x0041CE90u   /* the scroll poll, one call 0x496976          */
#define SCROLL_CAP_VA    0x0041CEAAu   /* its cap: cmp ebx,0x80 / push edi / jle / mov */
#define SCROLL_CAP_LEN   14            /* ... up to 0x41CEB8, test ebx,ebx            */

/* 0x41CE90..0x41CEBF, DISASSEMBLED: main into EAX, the byte main+0x1434D times
   the elapsed ticks main+0x38A3F into EBX, the signed cap at 0x80, the zero
   test -- what zoom_scroll_step's argument rests on. */
static const unsigned char SCROLL_POLL_HEAD[48] = {
    0xA1, 0xE8, 0x1D, 0x51, 0x00, 0x83, 0xEC, 0x24, 0x53, 0x33, 0xDB, 0x8A,
    0x98, 0x4D, 0x43, 0x01, 0x00, 0x55, 0x0F, 0xAF, 0x98, 0x3F, 0x8A, 0x03,
    0x00, 0x56, 0x81, 0xFB, 0x80, 0x00, 0x00, 0x00, 0x57, 0x7E, 0x05, 0xBB,
    0x80, 0x00, 0x00, 0x00, 0x85, 0xDB, 0x0F, 0x84, 0xF0, 0x01, 0x00, 0x00 };

/* THE SCROLL POLL'S CAP becomes `push edi; call stub; nop x8`, and the stub
   hands EBX to zoom_scroll_step and puts the result back in EBX.

   IN PLACE BEFORE IT CAN RUN: this runs from DllMain's process attach, before
   the exe's entry point and so before any thread that could execute the poll;
   the stub is complete before the site points at it; and the site is matched
   over all 48 bytes first, which establishes EAX = main and EBX = byte x ticks.
   No branch lands inside `0x41CEAB..0x41CEB7` (DISASSEMBLED: every rel8, rel32
   and jcc32 in .text; the `jle` targets `0x41CEB8`), and `push edi` runs at
   the stock depth, so the stack at `0x41CEB8` is the engine's.

   EVERY REGISTER BUT EBX IS KEPT: `[esp+0x10]` is pushad's EBX slot (EDI, ESI,
   EBP, ESP, EBX, EDX, ECX, EAX from esp+0), and `push ebx; call; add esp,4`
   leaves esp back at that frame. The flags are not kept and need not be:
   `test ebx,ebx` at `0x41CEB8` sets every flag the `je` after it reads.

   THE SITE'S rel32 IS COMPUTED BY HAND, against the address after the call
   (`0x41CEB0`): tagpu_detour_rel encodes against the buffer it writes into,
   which is right for the stub and wrong for `site[]`.

   ITS OWN BYTE MATCH, outside the zoom's all-or-nothing list: nothing else
   rests on it and it rests on nothing else, so an exe whose poll differs keeps
   the zoom and scrolls at the engine's step, with no byte of ours involved. */
static int scroll_step_install(void)
{
    unsigned char site[SCROLL_CAP_LEN];
    unsigned char* s;
    unsigned char* p;

    if (!bytes_are(SCROLL_POLL_VA, SCROLL_POLL_HEAD, (int)sizeof SCROLL_POLL_HEAD)) return 0;
    if (!(s = p = tagpu_detour_stub())) return 0;
    *p++ = 0x60;                                              /* pushad                 */
    *p++ = 0x53;                                              /* push ebx               */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)zoom_scroll_step); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                    /* add esp,4              */
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x10;       /* mov [esp+0x10],eax     */
    *p++ = 0x61;                                              /* popad                  */
    *p++ = 0xC3;                                              /* ret                    */
    site[0] = 0x57;                                           /* push edi               */
    site[1] = 0xE8;
    *(unsigned int*)(site + 2) = (unsigned int)(size_t)s - (SCROLL_CAP_VA + 6u);
    memset(site + 6, 0x90, SCROLL_CAP_LEN - 6);
    if (!tagpu_detour_write(SCROLL_CAP_VA, site, SCROLL_CAP_LEN)) {
        tagpu_detour_stub_free(s);
        return 0;
    }
    return 1;
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
        !site_is(SITE_GETTPOS, VA_GETTPOS) ||
        !site_is(SITE_DEBUGOVL, DEBUGOVL_VA) ||
        !bytes_are(EYECLAMP_VA, EYE_STOLEN, (int)sizeof EYE_STOLEN) ||
        !bytes_are(CENTRE_BLOCK, EYE_STOLEN, (int)sizeof EYE_STOLEN) ||
        !bytes_are(CENTOBJ_BLOCK, EYE_STOLEN, (int)sizeof EYE_STOLEN) ||
        !bytes_are(FOLLOWCLAMP_VA, FOLLOW_STOLEN, (int)sizeof FOLLOW_STOLEN)) {
        zlog("zoom: NOT armed — engine bytes differ at one of "
             "0x41C426/0x41C442/0x498EF9/0x468DBA/0x41C3C0/"
             "0x41C808/0x41C93B/0x41CAF7");
        return;
    }
    zlog(scroll_step_install()
         ? "zoom: scroll step armed at 0x41CEAA -- the scroll poll's step is divided by the "
           "level; ScrollSpeed main+0x1434D is never written"
         : "zoom: scroll step NOT armed -- the scroll poll 0x41CE90..0x41CEBF differs from the "
           "retail bytes, or no stub; edge and arrow scroll keep the engine's step at every zoom");
    ok  = redirect(SITE_MMRECT1_VA, (void*)zoom_minimap_rect);
    ok &= redirect(SITE_MMRECT2_VA, (void*)zoom_minimap_rect);
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
        zlog("zoom: PARTIAL — minimap and the scroll step only, the camera "
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
    zlog(built ? "zoom: ARMED (minimap rect 0x466B70 x2, "
                 "camera centre range 0x41C3C0 + target clamps 0x41C808/"
                 "0x41C93B/0x41CAF7 + world guard 0x498EF9 + debug overlay guard "
                 "0x468DBA); the eye, the target and the follow are "
                 "written on the game thread from the frame packet's command apply"
               : "zoom: ARMED without every target clamp (0x41C808/"
                 "0x41C93B/0x41CAF7) — a centring or a follow near a map edge "
                 "stops at the 1x range");
}
