#ifndef TAGPU_GUI_H
#define TAGPU_GUI_H
/* tagpu_gui — the GL UI renderer, Phase E (research/notes/gui-renderer.md).

   THE MODULE, NOT A SURFACE. The engine's UI — the side panel, the top and
   bottom bars, the minimap, chat and dialogs in game, every screen of the
   shell — is mirrored into GL twins of the engine's own surfaces: observer
   detours on the pixel-writing leaves record what was drawn, the engine keeps
   drawing its own 8bpp surface (which stays the oracle and the fallback), and
   the render thread replays the ops into retained twins that are drawn over
   the frame after the world's composite.

   Family: tagpu_gui_hook.c (the observers, the census, the publisher),
   tagpu_gui_surf.c (the twins, the UI atlas, the replay, the layer draw),
   tagpu_gui_snap.c (the gadget-tree snapshot behind `tacli ui`, was
   tagpu_ui.c, contract inc/tagpu_ui.h). One trigger, gamedir/tagpu_gui.on;
   the detours install at DllMain when it exists then; the DRAW follows the
   file live (polled twice a second): delete it and the frame is today's,
   recreate it and every twin re-seeds from the engine's surfaces at the next
   flip. Off also stops publishing, so an idle module is a few ifs.

   Tokens in tagpu_gui.on: `census` (the G15a diff; costs a 1024x768 compare
   per 5 ms), `strict` (the fallback off: a UI pixel the engine drew that we
   have not is painted magenta, the cursor's rect exempt — the harness's mode),
   `norestore` (G15e: the layer without Classic++ art — the UI-only A/B),
   `sharptest` (G17a: 13.2's sharp layer filled with a known pattern, so an
   empty layer is still testable — the harness's mode too), `log`, `pgm`,
   `trace`, `key=N` (census diagnostics, tagpu_gui_hook.c).

   THREADS. The observers and the publisher run on the game thread inside the
   engine's own calls; the twins, the atlas and the draw run on the render
   thread inside the present. They meet only in the queue (tagpu_gui_int.h).

   Every observer calls the original, so the engine's behaviour is
   byte-identical with the module armed. G15b draws the INDEX twin: Classic,
   1:1, palette-resolved at present; the colour twin is G15e. */
#include <windows.h>
#include "tagpu.h"

void tagpu_gui_init(void);                          /* DllMain                */
/* render thread, per present, GL current: poll the trigger, drain the queue
   into the twins, then draw the presented surface's twin over the frame
   (after the world's composite, so UI is above the world) */
void tagpu_gui_present(const TAGPU_FRAME* f);
void tagpu_gui_flush(unsigned int frame_counter);   /* render thread: the heartbeat line */
void tagpu_gui_glreset(void);                       /* the GL context changed */
int  tagpu_gui_installed(void);
int  tagpu_gui_drawing(void);                       /* the trigger says draw  */

/* THE CURSOR, G17c (gui-renderer.md 13.5). Erasing the engine's own cursor
   takes two modules, so the decision is taken once and read by both:
   tagpu_overlay.c calls tagpu_gui_cursor_frame FIRST, then the world pass,
   then tagpu_gui_present. Over the panel this module's twin covers the
   engine's cursor once the layer stops discarding its rect; over the world it
   does not, because the composite drops our fragment wherever the engine's
   surface is not the terrain key and a cursor pixel is not the key — so
   tagpu_native.c asks tagpu_gui_cursor_own for the rect and treats it as key.
   A version that only drew ours would ship two cursors over the world.
   tagpu_gui_cursor_own returns 1 when ours is being drawn this frame and
   fills `r` with the engine's rect in GAME pixels (x, y, w, h). */
void tagpu_gui_cursor_frame(void);
int  tagpu_gui_cursor_own(float* r);

/* G17e: the TNT's own minimap picture (`main+0x1426B`, TED_GENERATED_PIC),
   decoded on the game thread inside `BuildMinimapSurface 0x466780` — the one
   place it is alive, since the loader frees it before the map's first frame.
   252x252 or 252x256, against the 126-px box the engine fits it into, so
   drawing it at its native size is a free 2x with no new data path
   (gui-renderer.md 13.6). `gen` moves once per map load. */
int  tagpu_gui_minimap_pic(const unsigned char** pix, int* w, int* h, unsigned* gen);
#endif
