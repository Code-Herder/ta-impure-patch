#ifndef TAGPU_HUD_H
#define TAGPU_HUD_H
/* tagpu_hud.h — HUD scale: the in-game HUD magnified inside the player's own
   Screen Size, with the world viewport shrunk by exactly as much.
   Design: research/notes/gui-renderer.md §20; the geometry it rests on is
   research/notes/resolution.md §3.4a.

   TA's HUD is a hard 128 logical px side panel and two 32 px bars — immediates
   at 0x4981C9 and a family of absolute constants in the painters. HUD scale
   does NOT re-lay it out. Two halves, and they meet on one number:

     RESERVE  the engine's own viewport rect (the six ints at main+0x37E27) is
              written with left = 128s, top = 32s, bottom = H − 32s − 1, so
              every clamp the engine makes — edge scroll, the unit-under-pointer
              probe, drag selection, the SORT buffers LoadMap sizes — reasons
              about a world that really is that size.
     MAGNIFY  the composite samples the twin's HUD regions at s texels per
              device pixel instead of one, so the art the engine drew at 1x
              fills the space that was reserved for it.

   THE ONE NUMBER. Both halves derive their integers from tagpu_hud_geom(),
   which is a pure function of the screen dimensions and one percentage. They
   cannot disagree about where the HUD ends and the world begins, because
   neither of them owns that answer.

   THE SETTING is a percentage of stock, 0 meaning Auto. Auto is H/480 — the
   panel exactly fills the screen height, which §3.4a measured to be both the
   natural target and the hard ceiling (the panel block is a fixed 128x480 that
   does not stretch). It is exactly 1.0 at 640x480, TA's shipped mode, so an
   untouched install is unchanged. The store is the lever file `tagpu_hud.on`
   with a `scale=auto` / `scale=<percent>` token; the front-end Visuals row
   writes it and the game-entry observer reads it.

   WHAT MAKES A STALE VALUE SAFE. Exactly one word crosses threads: the
   percentage in force, latched at game entry (game thread) and read by the
   composite (render thread) and the pointer chain (message thread). It is a
   naturally aligned int, so a racing read sees one whole value or the other —
   and BOTH are valid, because every consumer re-resolves it against the screen
   dimensions IT is looking at, and tagpu_hud_geom clamps to that screen's own
   ceiling. So the geometry can never be bigger than the surface it is applied
   to, whatever the word held. In the shell, whose surface is atom-locked at
   640x480 whatever the player's Screen Size says, that ceiling is 1.0 and the
   whole module is the identity — which is why leaving a game needs no signal. */

/* Install the game-entry observer. DllMain, before the engine runs. */
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
/* Write it. -1 turns the pass off, 0 is Auto, otherwise a percentage. */
void tagpu_hud_store_pct(int pct);

/* 1 while HUD scale is in force on the surface the fork is presenting, with
   the geometry it is in force with. 0 — outputs untouched — in the shell,
   before the first game, and wherever the resolved scale is stock. */
int  tagpu_hud_live(int* panelW, int* barH, int* q8);

/* The point maps, between the engine's 1x HUD coordinates and the screen the
   player sees. Both are the identity outside a HUD region and at stock scale,
   so an unconditional call costs a compare. */
void tagpu_hud_to_screen(int* x, int* y);
void tagpu_hud_to_engine(int* x, int* y);

/* The viewport rect the game entry wrote, derived rather than remembered:
   tagpu_vpwide's four constants, which are 0x80 / 0x20 / 1 / 33 only at stock
   scale. `ta` is main. Always fills the outputs. */
void tagpu_hud_true_inset(const char* ta, int* L, int* T, int* rInset, int* bInset);

#endif
