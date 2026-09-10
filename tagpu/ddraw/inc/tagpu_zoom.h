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

   Two threads share this: the render thread publishes the live view once a
   frame (it is the one that reads the zoom lever and the engine's viewport),
   the message thread reads it to convert input. Publishing is five aligned
   32-bit stores and reading is five aligned loads, so a reader can at worst see
   one field from the previous frame — a sub-millisecond lag on a pointer, not a
   correctness problem, and not worth a lock on the input path.

   OUTSIDE THE WORLD VIEWPORT THE TRANSFORM IS THE IDENTITY. The side panel,
   minimap, top bar, chat and every dialog are screen-space: they are drawn by
   the engine at 1:1 at any zoom, which is the whole point of the key-fill, and
   a click on them must arrive unmodified. The gate is always on `s`, where the
   player actually clicked. */

/* Install the engine patches the zoom needs. DllMain only, byte-matched,
   all-or-nothing, armed by tagpu_zoom.on, and every one of them inert at zoom 1.

     THE MINIMAP'S VIEW RECTANGLE (0x466B70) is computed from the eye and the
       view size in map cells, so at any zoom != 1 it draws the 1x region and
       lies about what is on screen. The rectangle stays ENGINE-DRAWN — the
       minimap is screen-space and correct at 1:1 — we only rescale the rect it
       was going to draw.

     THE SCROLLSPEED SAVE (0x430FAE) keeps the rate we scale (see frame_end)
       out of the player's registry, where it would compound across launches.

     THE CAMERA'S RANGE (0x41C3C0, the eye clamp) is widened to the range the
       zoom actually shows: the engine's own bounds hold the VIEWPORT's edges on
       the map's, which at zoom > 1 stops the visible window short of every map
       edge and reads as the camera being pushed back off them. A leaf_call
       detour on a flag that is raised only while a zoomed-IN world is live, so
       1x and zoom-out run the engine's own function verbatim — including the
       two minimap-rect redirects inside it. `tagpu_zoomedge.off` in the gamedir
       disables it live and walks the eye back onto the 1x range.

     THE WORLD POINT UNDER THE MOUSE (0x498EF9, the GetTPosition call inside
       0x498DA0) is clamped to the map, because for a pointer OUTSIDE the
       viewport that point is the eye itself — on the map for every eye the
       engine can produce, off it for the widened range above, and the chain
       from there dereferences a NULL plot. Not gated on the zoom: it is a
       no-op whenever the eye is the engine's own. */
void  tagpu_zoom_init(void);

/* Re-read the level. Render thread only; called once a frame. Returns the level
   in force, from the two levers in priority order:

     tagpu_zoom.txt, when present — the file's value when it parses in
       0.25..8.0, the last good value on a torn read. Scripted drivers (tacli,
       the scenarios) own this one, and while it is there it WINS.
     the wheel otherwise — the level the player has wheeled to, eased toward
       its target one step per call, and 1.0 until they turn the wheel.

   While the file is in force the wheel is pinned to it, so DELETING the file
   leaves the view exactly where it was and hands the wheel control from there,
   rather than snapping back to 1.0 or to some level wheeled at long ago. */
float tagpu_zoom_read_lever(void);

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

/* End of the overlay's frame. A frame in which nothing published — the passes
   disarmed, the menu, a game not yet loaded — takes the view back down to 1:1,
   so a zoom lever left lying in the gamedir cannot bend menu clicks. It is also
   where the engine's scroll rate is kept in step with the zoom (see
   apply_scroll_rate() in tagpu_zoom.c) — the view has to move at the same rate
   across the SCREEN at every zoom, or scrolling at 0.25x feels glued — and
   where the camera's own range is kept in step (see apply_eye_range(): the
   engine clamps the eye only when IT moves the camera, so an eye left past the
   1x bound by a zoom-out has to be walked home from here). */
void  tagpu_zoom_frame_end(void);

/* The level in force (1.0 until the first publish — menus are never zoomed). */
float tagpu_zoom_level(void);

/* The camera range in force: the engine's own eye bounds, widened to the range
   the zoom actually shows (see the camera-range block in tagpu_zoom.c), or the
   engine's own verbatim whenever that patch is not the one clamping. Returns 0
   — and leaves every output alone — when there is no sane engine state to
   compute one from.

   ANYTHING THAT POSITIONS THE CAMERA ITSELF MUST CLAMP WITH THIS, not with
   [0, map - view]: those are the bounds that hold the 1x VIEWPORT's edges on
   the map's, and using them at zoom > 1 pulls the view back off every map edge,
   which is the bug the camera range exists to fix. */
int   tagpu_zoom_eye_range(int* loX, int* hiX, int* loY, int* hiY);

/* s -> u. Returns 1 if the point was transformed, 0 if it was left alone
   (zoom 1, no view published yet, or a screen-space position outside the world
   viewport). Both pointers are updated in place.

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
   from — and rewriting it is what used to throw the sprite across the frame
   (see carries_point() in tagpu_zoom.c).

   The rewrite belongs at the door and not at the many places that write
   g_ddraw.cursor: cnc-ddraw's PeekMessage rewriter and its wndproc both
   normalise the same event, so a transform applied at a write site would be
   applied twice. CallWindowProcA on g_ddraw.wndproc is the single door into the
   engine, and there are exactly three of them (wndproc.c x2, the shield's
   to_game). */
LPARAM tagpu_zoom_mouse_lparam(UINT msg, LPARAM lparam);

#endif
