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

   WE DO NOT WRITE THE ENGINE'S VIEWPORT RECT, and the reason is worth keeping.
   The first build did: left = 128s, top = 32s, on the theory that the rect is
   the one origin every consumer projects about. It is not. The rect's L and T
   are the screen->world origin inside 0x498DA0 and nothing else; TA's
   world->screen projection is a +0x80/+0x20 pair of immediates baked at every
   site that uses it — unit picking, band select, build placement, the feature
   blits — so moving the rect tore the world in two by ((s−1)·128, (s−1)·32).
   Measured 2026-09-11 at 1024x768 Auto: the engine picked a unit 76 px left
   and 19 px up from where it was drawn. Nothing here writes engine memory now,
   which is also why the setting no longer has to wait for a game to start.

   THE COST OF NOT RESERVING, and it is bigger than it first looked: the world
   under the HUD is rendered and covered (≈20% of the fill at s = 4.5), and the
   first world column the player can see is eye + (128s − 128) rather than eye.
   MEASURED 2026-09-12 at 4K Auto: the engine clamps eyeX to 0, so on a map with
   a western start the player's own commander (world x = 400, engine screen
   x = 528) sits behind the 576-px panel and there is no smaller eye to scroll
   to. Moving that clamp is the rest of this feature, not a follow-up, and until
   it moves Auto at 4K is not a defensible default. See gui-renderer.md §22.5.

   THE SETTING is a percentage of stock, 0 meaning Auto. Auto is H/480 — the
   panel exactly fills the screen height, which §3.4a measured to be both the
   natural target and the hard ceiling (the panel block is a fixed 128x480 that
   does not stretch). It is exactly 1.0 at 640x480, TA's shipped mode, so an
   untouched install is unchanged. The store is the lever file `tagpu_hud.on`
   with a `scale=auto` / `scale=<percent>` token; the front-end Visuals row
   writes it, and writing it takes effect on the next frame.

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

/* The point maps, between the engine's 1x HUD coordinates and the screen the
   player sees. Both are the identity outside a HUD region and at stock scale,
   so an unconditional call costs a compare. */
void tagpu_hud_to_screen(int* x, int* y);
void tagpu_hud_to_engine(int* x, int* y);

#endif
