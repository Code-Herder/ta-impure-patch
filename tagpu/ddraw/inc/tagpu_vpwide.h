#ifndef TAGPU_VPWIDE_H
#define TAGPU_VPWIDE_H

/* tagpu_vpwide — close the display-only ring at zoom < 1.

   THE PROBLEM. `tagpu_zoom` maps a screen pointer `s` to the unzoomed position
   `u` the engine's 1:1 arithmetic needs. At zoom < 1 the view shows more world
   than the engine's own viewport rect can NAME, so `u` for a pointer in the
   outer ring falls outside that rect and the button event has to be dropped:
   safe, but a unit you can plainly see cannot be selected or ordered. The
   captured marker layers stop at the same edge, because the engine's drawers
   clip to the same rect. Without this module, zoom-out is a viewing mode, not
   a play mode.

   THE FIX. Widen the engine's own rect to exactly the range `u` spans, so the
   region it can name is the region the zoom actually shows. `main+0x37E27` is
   six ints — L, T, R, B (a RECT) then W at `+0x37E37` and H at `+0x37E3B` —
   and its readers fall into four kinds that want different things:

     BOUNDS, with the screen->world origin HARDCODED at +0x80 / +0x20: the
       mouse routing test `0x469DF6`, `GetUnitAtMouse 0x48CD80` (whose first
       instruction is `IsPositionInRect(vpRect, mouse)`), the HotUnits cull
       `0x48BAE0`, the projectile and debris visibility tests. These are the
       readers the widening is FOR, and they need nothing else from us.

     ORIGIN: `0x498DA0` alone computes `world = eye + clamp(pos, L, R) - L`, so
       L and T are the origin there, not a bound. Measured: moving L 128->0 and
       T 32->0 shifts the map cell under the cursor by exactly (+8, +2) cells.
       Its one call site is redirected below and the conversion redone against
       the TRUE origin while honouring the WIDE clamp. That same redirect also
       carries the ZOOM's own repair of the mouse point, which is why it is
       armed by `tagpu_zoom.on` as well — see THE CURSOR below.

     CLIP: three sites in `DrawGameScreen` copy the rect into the offscreen
       surface's clip rect through `0x4C6B10`, which is a bare four-dword store
       with no clamping. Those three sites are redirected and clamped TWICE
       over, because there are two different bounds to respect. A rect wider
       than the surface would license any engine drawer still running inside
       the viewport to write outside the allocation. And a rect wider than the
       TRUE VIEWPORT — which is the whole point of this module — would license
       one to write on the side panel and the strips, where nothing ever
       repaints: our key fill covers the true viewport only, so a mark left
       there stands for the rest of the session. That second clamp is stock
       TA's own bound at these sites (unwidened, the rect they are handed IS
       the true one), so it can never remove anything the engine would have
       drawn on screen.

     W/H: the eye clamp `0x41C3C0` derives `maxEye = map - W` from them, and a
       negative maxEye makes it alternate between 0 and a negative eye every
       call. W and H are therefore never DRIVEN — only repaired, see below.

     THE CURSOR is NOT a reader of this rect. The engine draws its sprite wherever `GetCursorPos` reports, and that hook
       reports the TRUE pointer at every zoom, so the sprite lands under the
       pointer with nothing to move it afterwards and the question of what the
       engine may draw ON does not arise. What it costs is that the record the
       poll leaves at `[obj+0x196]`, and through the dispatch at `main+0x2C76`,
       is then in SCREEN space where the arithmetic above is 1:1 — so the
       `0x498DA0` stub recomputes `u` there and writes it back. That repair is
       what the zoom needs whether or not anything is widened, which is why
       `tagpu_zoom.on` arms the redirect on its own.

   The one engine WRITE to the rect, `0x49821D`, mostly is not a fight: it lives
   in the game-screen enter callback `0x497F40` and recomputes all six from the
   screen dimensions, so a re-entry RESTORES the true rect rather than
   compounding on a widened one, and the per-frame apply picks it up again.
   The exception is W and H: that callback computes `W = R - L + 1` by RE-READING
   L (`0x4981C9` writes it, `0x498214` reads it back), so a store of ours landing
   in that window would leave W hundreds of pixels wide and the eye clamp
   oscillating. The true rect is therefore derived from the SCREEN dimensions at
   `+0x37E1F`/`+0x37E23` — fields we never write — and W/H are checked against
   that every frame and put back when they disagree.

   MP-safety is the `ScrollSpeed` argument unchanged: the viewport rect is
   camera state that no other machine ever sees.

   Armed by `tagpu_vpwide.on` at DLL attach, like every other code-patching
   pass. Nothing is written to the rect unless the patches installed AND the
   true rect verified AND a zoomed-out view is live. `tagpu_zoom.on` arms the
   `0x498DA0` redirect ALONE — the mouse-point repair, no rect ever widened —
   because the zoom transform goes live from its own lever with no arm file at
   all, and once the engine is told the truth about the pointer that repair is
   what keeps its world point right.

   WRITTEN ON THE GAME THREAD: the render thread posts the zoom level (tagpu_zoom_frame_end) and the game thread
   derives the addressable rect from it at the top of every in-play draw
   (tagpu_vpwide_apply, from the packet publisher's `before`), so the store
   and 0x497F40's own writes are on one thread and cannot interleave — so W/H
   is counted rather than repaired, and the counter says it stays 0. Every
   reader of the rect that is not the engine's own runs on that thread too. */

/* Install the call-site redirects and the wndproc lParam patch. DllMain only,
   byte-matched; the widening half is all-or-nothing and needs
   `tagpu_vpwide.on`, while the `0x498DA0` redirect goes in for either arm file.
   (The lParam patch: TA unpacks the mouse position with `AND 0xffff` /
   `SHR 0x10`, which is zero-extending, so a client x of -20 arrives as 65516
   and the event is lost — half the ring. The patch makes it `MOVSX`/`SAR`,
   which is Microsoft's own GET_X_LPARAM and identical for any position a real
   mouse can report.) */
void tagpu_vpwide_init(void);

/* GAME THREAD, at the top of every in-play draw, from the frame packet
   publisher's `before` with the latest command record (NULL until the first
   post): widen the rect to the range the transform produces at the level the
   record carries — while a zoomed-out world is live — or put the true rect
   back. Verifies the rect once against what 0x497F40 builds, on a draw it
   does not own, and COUNTS (never repairs) a W/H that disagrees with the
   screen dimensions: on one thread that cannot happen, and the counter is
   the proof it does not.
   `terr_ours` is the render thread's terrain request as read for THIS draw
   (`tagpu_terrown_request`), and the rect widens only when it is set: the
   engine's own terrain blit places unclipped tile copies from the rect's L/T,
   so a widened origin under an engine terrain draw writes outside the
   offscreen. The caller then latches the stubs with
   `terr_ours || tagpu_vpwide_wide()`. */
struct TAGPU_CMD;
void tagpu_vpwide_apply(char* ta, const struct TAGPU_CMD* c, int terr_ours);

/* GAME THREAD, from the level teardown: the true rect back before the shell
   draws — no in-play draw will apply a record until the next level. */
void tagpu_vpwide_level_end(char* ta);

/* GAME THREAD: 1 while the rect holds our widened values -- which a failed
   `restore` can leave in place, so the terrain latch asks this rather than
   assuming the apply succeeded. */
int tagpu_vpwide_wide(void);

/* For the heartbeat: in-play draws the apply ran on, and the draws on which
   W/H disagreed with the screen-derived size (must stay 0). */
void tagpu_vpwide_counters(unsigned* applies, unsigned* wh_mismatch);

/* The TRUE 1x viewport rect. GAME THREAD ONLY — every
   render-thread pass takes it from the frame packet's `vp` field, which the
   publisher fills from this call. Every game-thread reader that means "the
   viewport the engine's UI is built around" must call this instead of reading
   `main+0x37E27`, because while this module is live that field is
   deliberately wider — and a key fill or a capture rect taken from the wide
   one would erase the side panel. Falls back to the field verbatim when
   nothing was ever widened, so with the module disarmed it is the field
   itself. */
void tagpu_vpwide_true_rect(const char* ta, int* L, int* T, int* W, int* H);

/* Is the `0x498DA0` mouse->world repair INSTALLED? The zoom transform may not go
   live without it: `fake_GetCursorPos` answers the true pointer, so the engine's
   1:1 screen->world arithmetic is only handed the unzoomed `u` because this
   redirect puts it back (see "the mouse point" in tagpu_vpwide.c). Nothing here
   is armed by default, and the zoom's own levers — `tagpu_zoom.txt` and the
   wheel — are gated by no arm file at all, so tagpu_zoom_read_lever() asks this
   before it will leave 1.0. */
int  tagpu_vpwide_mouse_world_live(void);

/* The rect the engine can currently NAME, as L/T/W/H. Returns 0 — and leaves
   the outputs alone — when that is just the true viewport, which is the case
   at zoom >= 1, with the module disarmed, and at the menus. */
int  tagpu_vpwide_addressable(int* L, int* T, int* W, int* H);

#endif
