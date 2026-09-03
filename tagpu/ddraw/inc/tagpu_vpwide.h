#ifndef TAGPU_VPWIDE_H
#define TAGPU_VPWIDE_H

/* tagpu_vpwide — close the display-only ring at zoom < 1.

   THE PROBLEM. `tagpu_zoom` maps a screen pointer `s` to the unzoomed position
   `u` the engine's 1:1 arithmetic needs. At zoom < 1 the view shows more world
   than the engine's own viewport rect can NAME, so `u` for a pointer in the
   outer ring falls outside that rect and the button event has to be dropped:
   safe, but a unit you can plainly see cannot be selected or ordered. The
   captured marker layers stop at the same edge, because the engine's drawers
   clip to the same rect. Zoom-out was a viewing mode, not a play mode.

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
       the TRUE origin while honouring the WIDE clamp.

     CLIP: three sites in `DrawGameScreen` copy the rect into the offscreen
       surface's clip rect through `0x4C6B10`, which is a bare four-dword store
       with no clamping. A rect wider than the surface would license any engine
       drawer still running inside the viewport to write outside the
       allocation, so those three sites are redirected and clamped.

     W/H: the eye clamp `0x41C3C0` derives `maxEye = map - W` from them, and a
       negative maxEye makes it alternate between 0 and a negative eye every
       call. W and H are therefore never DRIVEN — only repaired, see below.

     THE CURSOR is NOT a reader of this rect, and that is the point: the engine
       draws its sprite wherever `GetCursorPos` reports, and the composite moves
       it back under the pointer from there — which only works while that
       position is inside the engine's own viewport, over the terrain key fill.
       Widening what the engine can NAME must therefore not widen what it draws
       ON, so `tagpu_zoom_to_engine_draw()` keeps the ring identity for that one
       poll while the messages carry the widened `u`. Measured before the split:
       at 0.5× with the pointer at screen (320,400) there was no cursor at the
       pointer and a ghost one on the build panel.

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
   true rect verified AND a zoomed-out view is live. */

/* Install the four call-site redirects and the wndproc lParam patch. DllMain
   only, byte-matched, all-or-nothing; a no-op unless tagpu_vpwide.on exists
   then. (The lParam patch: TA unpacks the mouse position with `AND 0xffff` /
   `SHR 0x10`, which is zero-extending, so a client x of -20 arrived as 65516
   and the event was lost — half the ring. It becomes `MOVSX`/`SAR`, which is
   Microsoft's own GET_X_LPARAM and identical for any position a real mouse can
   report.) */
void tagpu_vpwide_init(void);

/* Render thread, once a frame, from tagpu_zoom_frame_end(): widen the rect to
   the range the transform produces at `z`, or put the true rect back. */
void tagpu_vpwide_frame(float z);

/* The TRUE 1x viewport rect. EVERY pass that means "the viewport the engine's
   UI is built around" must call this instead of reading `main+0x37E27`,
   because while this module is live that field is deliberately wider — and a
   key fill or a composite key rect taken from the wide one would erase the
   side panel. Falls back to the field verbatim when nothing was ever widened,
   so it is exactly today's behaviour with the module disarmed. */
void tagpu_vpwide_true_rect(const char* ta, int* L, int* T, int* W, int* H);

/* The rect the engine can currently NAME, as L/T/W/H. Returns 0 — and leaves
   the outputs alone — when that is just the true viewport, which is the case
   at zoom >= 1, with the module disarmed, and at the menus. */
int  tagpu_vpwide_addressable(int* L, int* T, int* W, int* H);

#endif
