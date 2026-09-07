#ifndef TAGPU_GUI_H
#define TAGPU_GUI_H
/* tagpu_gui — the GL UI renderer, Phase E (research/notes/gui-renderer.md).

   THE MODULE, NOT A SURFACE. The engine's UI — the side panel, the top and
   bottom bars, the minimap, chat and dialogs in game, every screen of the
   shell — is mirrored into GL twins of the engine's own surfaces: observer
   detours on the pixel-writing leaves record what was drawn, the engine keeps
   drawing its own 8bpp surface (which stays the oracle and the fallback), and
   the render thread replays the ops into retained twins the composite draws.

   Family: tagpu_gui_hook.c (the observers, the brackets, the census),
   tagpu_gui_surf.c (twins, ring, seed), tagpu_gui_art.c (the UI GAF atlas and
   the uirestore policy), tagpu_gui_snap.c (the gadget-tree snapshot behind
   `tacli ui`, was tagpu_ui.c, contract inc/tagpu_ui.h). One trigger,
   gamedir/tagpu_gui.on; one seam into the composite, tagpu_gui_layer().

   G15a — THE CENSUS (this header's first life). No drawing. The observers
   record every op into a game-thread ring, and at every engine flip
   (FlipOffscreenToPrimary 0x4C63A0) the flipped surface is diffed against its
   previous copy; every recorded op's box is subtracted; what remains is a
   writer we have not named, counted and logged per screen. Tokens in
   tagpu_gui.on: `census` (run the diff), `log` (a line per 50 censuses and
   on any residual, window totals), `pgm` (tagpu_gui_census.trigger writes
   tagpu_gui_census.pgm: 0 = unchanged, 128 = explained, 255 = unexplained
   since the last dump), `trace` (the ops intersecting a residual, the first
   blits after a screen build, every surface allocation with its tag),
   `key=N` (the terrain key, 254). MEASURED 2026-09-07: 0 unexplained of
   3 710 035 changed pixels across the shell and in-game inventory.

   Arming: installed ONCE at DllMain and only if tagpu_gui.on exists then, per
   the arming rule (own-the-draw.md); every detour is byte-matched and the set
   is all-or-nothing. Every observer calls the original, so the engine's
   behaviour is byte-identical with the module armed. */
#include <windows.h>

void tagpu_gui_init(void);                          /* DllMain                */
void tagpu_gui_flush(unsigned int frame_counter);   /* render thread, per present */
int  tagpu_gui_installed(void);
#endif
