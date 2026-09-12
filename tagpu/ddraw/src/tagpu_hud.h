#ifndef TAGPU_HUD_H
#define TAGPU_HUD_H
/* tagpu_hud.h — HUD scale: the in-game HUD magnified inside the player's own
   Screen Size, over a world the engine goes on drawing exactly as it always
   did. Design: research/notes/gui-renderer.md §22; the geometry it rests on is
   research/notes/resolution.md §3.4a; §22.5 is why the first build of this was
   withdrawn and what replaced it.

   TA's HUD is a hard 128 logical px side panel and two 32 px bars — immediates
   at 0x4981C9 and a family of absolute constants in the painters. HUD scale
   does NOT re-lay it out, and — this is the part that changed — it does not
   ask the engine for the space either. It is one transform, applied twice:

     MAGNIFY  the composite samples the twin's HUD regions at s texels per
              device pixel instead of one, so the art the engine drew at 1x
              covers 128s / 32s of the screen. The world under it is drawn and
              then hidden; nothing tells the engine that happened.
     POINT    the pointer is divided by s inside those same regions before the
              engine sees it, so every hit test the engine makes — all of them
              written against the 128 / 32 constants — keeps answering about
              the grid it was written for.

   THE BOUNDARY IS EXACT, not nearly: the panel's last screen column is
   128s − 1 and 128s / s is 128, so the screen point that first belongs to the
   world is the first point the map sends to engine column 128. The composite
   and the pointer map take that boundary from the same tagpu_hud_geom(), so
   they cannot disagree about it.

   WE WRITE FOUR OF THE SIX INTS OF THE ENGINE'S VIEWPORT RECT, AND NEVER L/T.
   That distinction is the whole design and it was learned the hard way.

   The FIRST build wrote all six — left = 128s, top = 32s — on the theory that
   the rect is the one origin every consumer projects about. It is not. The
   rect's L and T are the screen->world origin inside 0x498DA0 and nothing else;
   TA's world->screen projection is a +0x80/+0x20 pair of immediates baked at
   every site that uses it — unit picking, band select, build placement, the
   feature blits — so moving them tore the world in two by ((s−1)·128,
   (s−1)·32). Measured 2026-09-11 at 1024x768 Auto: the engine picked a unit
   76 px left and 19 px up from where it was drawn. §22.5 withdrew that build
   and wrote nothing at all, which cost the other half: the world under the HUD
   was rendered and covered, and the first world column the player could see was
   eye + (128s − 128) rather than eye, with no smaller eye to scroll to.

   §22.6 IS WHAT SHIPS: make the two rectangles the same one. `apply_rect`
   writes R, B, viewW and viewH — the four the engine derives from its screen
   size and nothing else derives a projection from — so the engine's own
   viewport becomes the window the player is actually looking at, while L and T
   keep the values the baked immediates assume. The world block is then
   translated by (128s − 128, 32s − 32) in exactly three places: the composite,
   the world layer's glViewport, and the pointer map's world branch. Because
   the rect is what the camera clamp and the map loader read, the eye now
   reaches mapW − viewW / mapH − viewH and the corner of the map is reachable.

   THE WRITE IS LATCHED AT GAME ENTRY, and that ordering is load-bearing rather
   than incidental: the observer sits on 0x4288D0 at site 0x49823D, which is the
   first call after the last of the engine's own six rect stores (0x498237), and
   the map loader thread is created afterwards at 0x4982CA and reads viewW/viewH
   at 0x483BBF/0x483FDF. So the scale a game runs at is fixed when that game
   starts; changing the row mid-game takes effect at the next one.

   THE SETTING is a percentage of stock, 0 meaning Auto. Auto is H/480 — the
   panel exactly fills the screen height, which §3.4a measured to be both the
   natural target and the hard ceiling (the panel block is a fixed 128x480 that
   does not stretch). It is exactly 1.0 at 640x480, TA's shipped mode, so an
   untouched install is unchanged. The store is the lever file `tagpu_hud.on`
   with a `scale=auto` / `scale=<percent>` token; the front-end Visuals row
   writes it, and — see the latch above — it takes effect at the next game.

   WHAT MAKES A STALE VALUE SAFE. Exactly one word crosses threads: the
   percentage in force, written by the row (message thread) and read by the
   composite (render thread) and the pointer chain (message thread). It is a
   naturally aligned int, so a racing read sees one whole value or the other —
   and BOTH are valid, because every consumer re-resolves it against the screen
   dimensions IT is looking at, and tagpu_hud_geom clamps to that screen's own
   ceiling. So the geometry can never be bigger than the surface it is applied
   to, whatever the word held. The worst a change mid-frame can do is leave the
   pointer map and the picture one frame apart, and neither of them is engine
   state. In the shell, whose surface is atom-locked at 640x480 whatever the
   player's Screen Size says, that ceiling is 1.0 and the whole module is the
   identity — which is why entering and leaving a game needs no signal. */

/* Seed the live scale from the lever. DllMain, before the engine runs. */
void tagpu_hud_init(void);

/* THE resolver. `pct` is the requested percentage of stock, or 0 for Auto.
   Outputs (any may be NULL) the scale in Q8 — 256 is stock — and the two
   integers it produces. Pure; clamped to [stock, this screen's ceiling], so a
   nonsense percentage and a nonsense screen both land on the identity. */
void tagpu_hud_geom(int screenW, int screenH, int pct,
                    int* q8, int* panelW, int* barH);

/* The largest percentage this screen can honour (>= 100). */
int  tagpu_hud_ceiling_pct(int screenW, int screenH);

/* The setting as stored, for the front-end row: the percentage, or 0 for Auto,
   or -1 when the pass is off altogether. Reads the lever, so front end only. */
int  tagpu_hud_stored_pct(void);
/* Write it, and put it in force. -1 turns the pass off, 0 is Auto, otherwise
   a percentage. */
void tagpu_hud_store_pct(int pct);

/* 1 while HUD scale is in force on the surface the fork is presenting, with
   the geometry it is in force with. 0 — outputs untouched — in the shell and
   wherever the resolved scale is stock. */
int  tagpu_hud_live(int* panelW, int* barH, int* q8);

/* The insets tagpu_vpwide.c builds the TRUE rect from: L/T are always the
   engine's own 0x80/0x20, the far edges come in by what the HUD covers. */
void tagpu_hud_true_inset(const char* ta, int* L, int* T, int* rInset, int* bInset);

/* The vector from a point in the ENGINE's surface to the same point on the
   screen, inside the WORLD region: (128s - 128, 32s - 32). 0 - and both
   outputs 0 - whenever the pass is inert. The composite subtracts it, the
   world layer's viewport adds it, and the pointer map's world branch
   subtracts it; those three are the whole of the translation. */
int  tagpu_hud_shift(int* dx, int* dy);

/* The point maps, between the engine's 1x HUD coordinates and the screen the
   player sees. Both are the identity outside a HUD region and at stock scale,
   so an unconditional call costs a compare. */
void tagpu_hud_to_screen(int* x, int* y);
void tagpu_hud_to_engine(int* x, int* y);

#endif
