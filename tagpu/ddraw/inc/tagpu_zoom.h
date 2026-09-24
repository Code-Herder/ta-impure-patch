#ifndef TAGPU_ZOOM_H
#define TAGPU_ZOOM_H
#include <windows.h>

/* THE RANGE THE LEVER CLAMPS TO, public because it is half of every "how much
   world can be on screen at once" question. The rect each pass gathers over is
   the viewport divided by the zoom, so the widest rect that can EVER be asked
   for is this viewport at TAGPU_ZOOM_MIN — an expression with the live screen
   in it and no resolution constant anywhere. tagpu_native.c bounds the rect
   with it and tagpu_terr.c sizes its staging for it, so both track whatever
   screen the player is on instead of a number someone picked. */
#define TAGPU_ZOOM_MIN  0.25f
#define TAGPU_ZOOM_MAX  8.0f

struct TAGPU_PACKET;
struct TAGPU_CMD;

/* tagpu_zoom — the view transform, and the ONE place that owns it.

   The native pass scales the world about the view centre; the engine's own
   screen->world arithmetic is 1:1 and knows nothing about it. So at any zoom
   z != 1 a click has to be translated before the engine sees it, or it lands on
   the wrong world point. The transform is a pure similarity about the centre of
   the world viewport:

       u = (s - c) / z + c        c = (vpL + vw/2, vpT + vh/2)

   where `s` is the REAL screen position of the pointer (what the player is
   looking at) and `u` is the unzoomed position to hand the engine so its 1:1
   maths lands on the world point actually under `s`.

   `u` REACHES THE ENGINE IN EXACTLY TWO PLACES, and neither is the cursor.
   Mouse messages carry it (tagpu_zoom_mouse_lparam below), and the mouse->world
   conversion at `0x498DA0` recomputes it for the record the engine dispatches
   (vpw_mouse_world() in tagpu_vpwide.c). `GetCursorPos` answers `s` — the truth
   — so the engine blits its own cursor sprite under the pointer and its
   screen-space readers of that poll (the edge scroll's equality on the
   outermost screen pixel) keep working at every zoom.

   THREE THREADS, ONE OWNER PER WORD.
   The render thread owns the LEVEL — it reads the two levers, tweens the
   wheel, works out the cursor anchor's eye delta and publishes the live view
   for the input path — and it writes NOTHING into engine memory: everything the zoom
   needs the engine to do travels as a COMMAND RECORD (tagpu_packet.h,
   TAGPU_CMD) that the game thread takes at the top of every in-play draw and
   applies there, on the thread that owns the camera, the viewport rect and
   the scroll rate (tagpu_zoom_apply, from tagpu_packet_pub.c's observer). The
   message thread reads the published view to convert input: five aligned
   32-bit loads, so a reader can at worst see one field from the previous
   frame — a sub-millisecond lag on a pointer, not a correctness problem.

   OUTSIDE THE WORLD VIEWPORT THE TRANSFORM IS THE IDENTITY. The side panel,
   minimap, top bar and chat are screen-space: the engine draws them at 1:1 at
   every zoom, so their clicks must arrive unmodified. The ARMOPT/exit/preferences
   stack may also cover the viewport while DrawGameScreen continues underneath;
   main+0x37EBE bit 0 says that stack owns input, and the whole transform becomes
   the identity until the stack closes. This is not a general modal flag:
   SHARE.GUI uses bit 6 and remains a known gap. The gate is always on `s`, where
   the player actually clicked. */

/* Install the engine patches the zoom needs. DllMain only, byte-matched
   before anything is written, armed by tagpu_zoom.on.

     THE MINIMAP'S VIEW RECTANGLE (0x466B70) is computed from the eye and the
       view size in map cells, so at any zoom != 1 it draws the 1x region and
       lies about what is on screen. The rectangle stays ENGINE-DRAWN — the
       minimap is screen-space and correct at 1:1 — we only rescale the rect it
       was going to draw, and clamp it to the minimap.

     THE SCROLLSPEED SAVE (0x430FAE) keeps the rate we scale (see the apply)
       out of the player's registry, where it would compound across launches.

     THE CAMERA'S RANGE (0x41C3C0, the eye clamp) is BAR's centre clamp: the
       ground point at the view centre stays on the map at every zoom, so the
       eye ranges over [-W/2, map - W/2] — `map` the map's own size, the PLOT
       grid x 16 — instead of the engine's [0, extent - W], `extent` the scroll
       extent main+0x1422B, the map less 32 and 128 px.
       In force only on draws whose ground our terrain pass owns; the engine's
       own range otherwise (tagpu_zoom.c, "the camera's range").

     THE SCROLL TARGET'S THREE REACHABLE INLINE CLAMPS (0x41C808, 0x41C93B —
       the smooth arms of the two centre-ons — and the follow's 0x41CAF7) take
       the same range, so a centring or a follow reaches it. SetCamera's smooth
       arm has no caller that reaches it and is left alone.

     THE WORLD POINT UNDER THE MOUSE (0x498EF9, the GetTPosition call inside
       0x498DA0) is clamped to the map, because a pointer over the void past
       the map edge, or outside the viewport, names a point off it once the
       eye is, and the chain from there dereferences a NULL plot.

     THE MAP DEBUG OVERLAY (0x468DBA, the call of 0x418310) runs only for an
       eye in the engine's own range: its cell window has no lower bound. */
void  tagpu_zoom_init(void);

/* ---- render thread: the level, once a frame ---------------------------- */

/* Re-read the level. Render thread, ONCE per overlay frame, from the driver
   (tagpu_overlay.c) BEFORE any pass reads the eye — the scaffold included, so
   every pass draws the same frame from the same eye. Returns the level in
   force, from the two levers in priority order:

     tagpu_zoom.txt, when present — the file's value when it parses in
       0.25..8.0, the last good value on a torn read. Scripted drivers (tacli,
       the scenarios) own this one, and while it is there it WINS.
     the wheel otherwise — the level the player has wheeled to, drawn along
       its 250 ms tween at this call's time, and 1.0 until they turn the wheel.

   While the file is in force the wheel is pinned to it, so DELETING the file
   leaves the view exactly where it was and hands the wheel control from there,
   rather than snapping back to 1.0 or to some level wheeled at long ago.

   `pk` is this frame's packet (or NULL): the cursor anchor works out its eye
   delta against the packet's eye and the camera range the packet's map and
   viewport describe, and the PREDICTED eye for this frame — the packet's eye
   plus every delta the game thread has not acknowledged yet — is what
   tagpu_zoom_predicted_eye() answers until the next call. */
float tagpu_zoom_read_lever(const struct TAGPU_PACKET* pk);

/* The level tagpu_zoom_read_lever() settled on this frame (the file's or the
   wheel's), whether or not a zoomed world is on screen. */
float tagpu_zoom_lever(void);

/* The eye every pass draws this frame from: the packet's eye plus the cursor
   anchor's deltas the game thread has not acknowledged yet, clamped to the
   camera range in force. Exact once the packet catches up (the next in-play
   draw applies the same deltas), so a wheel notch is drawn on the frame it
   happens and does not wobble. Returns 0 — and leaves the outputs alone —
   when there is no in-game packet: then there is no world to draw. */
int   tagpu_zoom_predicted_eye(int* eyeX, int* eyeY);

/* 1 when this frame must take the WIDE fog grid (tagpu_fogwide), built every
   tick from the live eye with a margin around it, over the engine's own: the
   level is below 1, where the engine's grid cannot span the view; or the
   predicted eye is ahead of the packet's, and the engine's grid spans the
   packet's; or the packet's eye is off the engine's own range, where the
   engine places its border completions off the map. Render thread. */
int   tagpu_zoom_wide_fog(void);

/* A mouse message on its way into the engine, offered to the wheel first.
   Returns 1 when the wheel took it — the caller must then NOT pass it on.

   The engine has no use for it either way: its window procedure dispatches only
   0x200..0x206 through the jump table at 0x4B5E3B, so WM_MOUSEWHEEL falls
   straight to a bare DefWindowProcA. Nothing is being taken away from anyone.

   Only WM_MOUSEWHEEL is taken, only while a zoomed world is actually on screen
   (so the menus can never be wheeled), only while the pointer is over the world
   viewport (the side panel, the minimap and every dialog keep their wheel for
   whatever wants it later), and not at all when tagpu_wheel.off exists.

   `lparam` must be the GAME-space point, which is the space the engine's own
   window procedure is handed and the space tagpu_zoom_publish_view() reports
   the viewport in — so the gate compares like with like. A hardware wheel
   arrives in SCREEN space and cnc-ddraw has already walked it the whole way by
   the time either door is reached: ScreenToClient, then the letterbox offset
   (mouse.x_adjust) and the unscale (mouse.unscale_x), then a clamp to
   g_ddraw.width/height. Do not pass a raw client point: wherever adjmouse
   scaling is in force, client and game space differ.

   Message thread. It only accumulates the notches; the level itself moves on
   the render thread in tagpu_zoom_read_lever(), which is what keeps one owner
   for the number. */
int   tagpu_zoom_wheel(UINT msg, WPARAM wparam, LPARAM lparam);

/* A wheel arrived but cnc-ddraw's own mouse-lock gate is about to swallow it
   (windowed, not yet clicked in, devmode off), so it can never reach
   tagpu_zoom_wheel(). Says so in the log, throttled — without this that case is
   the one refusal with no explanation, which is exactly what the other two
   gripes exist to prevent. Message thread. */
void  tagpu_zoom_wheel_locked_out(void);

/* The native pass drew this world-viewport rect this frame. Render thread; it
   is what says "the world on screen IS zoomed", so the transform is live only
   between a publish and the frame_end that finds none. */
void  tagpu_zoom_publish_view(int vpL, int vpT, int vw, int vh);

/* End of the overlay's frame, on EVERY path out of it. A frame in which
   nothing published — the passes disarmed, the menu, a game not yet loaded —
   takes the view back down to 1:1, so a zoom lever left lying in the gamedir
   cannot bend menu clicks. And it POSTS THIS FRAME'S COMMAND RECORD: the level
   and whether it is live (the game thread derives the addressable rect, the
   minimap box and the scroll rate from them), the anchor's cumulative delta,
   the camera hold (tagpu_input.c) and the follow release. A frame that drew
   nothing zoomed therefore hands the engine its own rect and rate back at the
   next in-play draw. */
void  tagpu_zoom_frame_end(void);

/* The level in force (1.0 until the first publish — menus are never zoomed).
   Render thread and the message thread. */
float tagpu_zoom_level(void);

/* The widest view the levers can produce: the LOWEST level either of them will
   settle at, which is the same clamp tagpu_zoom_read_lever() applies to both.
   Anything that has to size a buffer for "however far out this view can go"
   asks here rather than pinning the number itself — tagpu_fogwide.c does,
   because the level it can read is always one frame old and one frame of the
   tween can cross the whole range. */
float tagpu_zoom_min(void);

/* ---- game thread: the apply ------------------------------------------- */

/* GAME THREAD, at the top of every in-play draw, from the frame packet
   publisher's `before`: apply the latest command record `c` (NULL until the
   first post) to the words this module is responsible for, on the thread
   that owns them. The apply runs after whichever of the frame callback's own
   camera writers ran this frame (the stepper 0x41CA10 and the scroll poll
   0x41CE90 are both called before the draw call at 0x4969CD, and both can
   be skipped — the stepper when the sim is paused, both under an in-game
   GUI screen) and before the draw's first read of the eye at 0x468DD9; no
   store to the eye exists inside DrawGameScreen, so nothing moves the camera
   between the apply and the read. In order: the level it carries becomes
   the level every game-thread reader here uses (the minimap rect's scale,
   the scroll rate); the camera range is chosen — the centre range when
   `terr`, the render thread's terrain request that the publisher latches
   for this same draw right after, else the engine's own; a NEW record of the
   current epoch has its eye delta applied — the follow released first when
   the record asks, then the eye and its scroll target stepped together; the
   hold, when on, is clamped into the range and written when it differs; the
   range is applied to the eye and the target every draw, which is what walks
   the eye into the engine's own range before a draw whose ground is the
   engine's; a camera that moved gets the minimap's view box recomputed and
   the screen fog grid invalidated — bit 3 of main+0x14281 cleared, exactly as
   every engine eye writer clears it, which is safe HERE and nowhere else; and
   ScrollSpeed is driven at base/z. `ta` is validated by the caller. */
void  tagpu_zoom_apply(char* ta, const struct TAGPU_CMD* c, int terr);

/* GAME THREAD, from the level teardown (the packet publisher's level end):
   the level's camera state handed back — the engine's own range back in
   force with the eye and the target walked into it, ScrollSpeed restored to
   the player's own value — before the shell draws; nothing applies a
   command again until the next level's first in-play draw. The command
   EPOCH is bumped here and the applied delta reset, so a record posted for
   the old level (a notch in its last frames) carries no delta into the new
   one; the render thread resets its own sum when a packet shows the new epoch. */
void  tagpu_zoom_level_end(char* ta);

/* GAME THREAD: what the apply has done so far, for the packet's
   acknowledgement fields — the last record's cmd_seq, the cumulative delta
   applied, the level in force, the epoch, and whether the centre range is in
   force (the render thread computes the same range from it). */
void  tagpu_zoom_applied(unsigned* seq, int* cum_dx, int* cum_dy, float* level,
                         unsigned* epoch, unsigned* centre);

/* ---- the input path ---------------------------------------------------- */

/* s -> u. Returns 1 if the point was transformed, 0 if it was left alone
   (zoom 1, no view published yet, the options/exit GUI stack owns input, or a
   screen-space position outside the world viewport). Both pointers are updated
   in place.

   THE ADDRESSABLE RING. The engine can only name screen positions inside its
   own viewport — anything outside it it routes to the screen-space UI instead
   (measured: a click whose position lands outside does nothing at all). At
   zoom < 1 the view shows more world than the 1x viewport has room to name, so
   `u` for a pointer in that outer ring falls outside it. There the transform
   returns the pointer UNCHANGED and reports the ring, and the button event is
   dropped — see to_engine() in tagpu_zoom.c for why identity beats clamping.

   The ring is the same boundary the captured marker layers stop at
   (ui-markers.md 6.1): at zoom < 1 everything outside the 1x viewport is
   DISPLAY-ONLY. One gap, and one fix would close both — giving the engine a
   wider addressable rect, or shifting its eye for the duration of a click. */
int   tagpu_zoom_to_engine(int* x, int* y);

/* 1 when this mouse message must not reach the engine at all: a BUTTON event in
   the display-only ring above. Take the screen-space lParam, before the
   rewrite. Dropping is the honest answer — the click has no world point the
   engine can name, and the clamped one is somewhere the player did not click. */
int   tagpu_zoom_drop_mouse(UINT msg, LPARAM lparam);

/* Rewrite a BUTTON message's lParam on its way into the engine's own window
   procedure, leaving every other message — a move above all — untouched. A
   button is the one event whose position cannot be recovered later: it is
   queued on the engine's event ring and dispatched whenever the game loop gets
   to it, so the transform is applied here, at the press. A move is not queued —
   it lands in `[obj+0x196]`, which is also where the cursor sprite is drawn
   from — and rewriting it would throw the sprite across the frame (see
   carries_point() in tagpu_zoom.c).

   The rewrite belongs at the door and not at the many places that write
   g_ddraw.cursor: cnc-ddraw's PeekMessage rewriter and its wndproc both
   normalise the same event, so a transform applied at a write site would be
   applied twice. CallWindowProcA on g_ddraw.wndproc is the single door into the
   engine, and there are exactly three of them (wndproc.c x2, the shield's
   to_game). */
LPARAM tagpu_zoom_mouse_lparam(UINT msg, LPARAM lparam);

#endif
