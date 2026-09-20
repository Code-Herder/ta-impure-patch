#ifndef TAGPU_GUI_H
#define TAGPU_GUI_H
/* tagpu_gui -- the engine's UI, OBSERVED (research/notes/gui-renderer.md).

   WHAT IT IS NOW. Observer detours on the engine's pixel-writing leaves record
   what the UI draws -- which sprite, which string, at which coordinates, into
   which surface -- while the engine goes on drawing its own 8bpp surface
   exactly as it always did. That record is the engine's UI stated
   SEMANTICALLY, and it is what a native UI pass will be built from.

   IT NO LONGER DRAWS ANYTHING. Until the clean cut this module also owned the
   other half: `tagpu_gui_surf.c` replayed those ops into retained twins and
   composited them -- with our own device-resolution sharp layer, through one
   quad -- over the frame. That was the engine's pixels reaching the screen, so
   the cut deleted the file, the composite, the twins, the sharp layer and the
   GLSL behind them. There is no lever to bring it back and no UI on screen: no
   side panel, no bars, no minimap, no cursor, no dialogs. That is the cut's
   cost, and the UI returns as a pass of ours rather than as a replay of the
   engine's.

   WHY THE CAPTURE SURVIVED IT. Two reasons, and the first is not about the UI
   at all: `tagpu_gui_hook.c`'s `before_flip` is the only host of
   `tagpu_triggers_frame`, so every `tacli` verb in the fork is dispatched from
   inside it -- deleting the file would take the whole harness with it. The
   second is that capture is not compositing. `tagpu_surf_take`'s reference
   frame is kept for exactly the same reason.

   Family: tagpu_gui_hook.c (the observers, the census, the publisher),
   tagpu_gui_snap.c (the gadget-tree snapshot behind `tacli ui`, was
   tagpu_ui.c, contract inc/tagpu_ui.h). One trigger, gamedir/tagpu_gui.on;
   the detours install at DllMain when it exists then.

   Tokens in tagpu_gui.on: `census` (the G15a diff; costs a 1024x768 compare
   per 5 ms), `log`, `pgm`, `trace`, `key=N` (census diagnostics,
   tagpu_gui_hook.c). The tokens that named the draw -- `strict`, `norestore`,
   `sharptest`, `nocursor`, `mmbase`, `nominimap`, `cursorscale=` -- went with
   it.

   THREADS. The observers and the publisher run on the game thread inside the
   engine's own calls. Nothing consumes the queue, so it is drained where it is
   filled (tagpu_gui_int.h).

   Every observer calls the original, so the engine's behaviour is
   byte-identical with the module armed. */#include <windows.h>
#include "tagpu.h"

void tagpu_gui_init(void);                          /* DllMain                */
void tagpu_gui_flush(unsigned int frame_counter);   /* render thread: the heartbeat line */
int  tagpu_gui_installed(void);
unsigned tagpu_gui_flips(void);                     /* the publisher's flip count, game thread */

/* THE CURSOR DECISION AND ITS TWO READERS ARE GONE. G17c latched, once a
   frame, whether OUR cursor was being drawn, so that the composite and the
   world pass could erase the engine's from the same rect and not ship two.
   With nothing of ours on screen there is nothing to erase for: the engine
   draws its own cursor into its own surface, `tagpu_cursown_publish(0)` in
   render_vk.c keeps it that way unconditionally, and the reference frame is
   complete because of it. What the player sees is no cursor at all, which is
   the cut's cost rather than a defect to work around.

   WHAT THAT WORK ESTABLISHED IS WORTH KEEPING and is written down in
   gui-renderer.md 13.5, 17 and 23 -- above all the bound the rect never had:
   the engine's cursors PULSE (the move cursor cycles 27x27 to 35x35, one pixel
   per side per step) and the rect a frame holds can be one animation step
   behind the sprite the engine blits next. A native cursor pass needs the
   cursor table's animation extent, not a margin. */
struct TAGPU_PACKET;

/* WHETHER THE PACKET CARRIES THE THREE MINIMAP SURFACES (landing 4c). The
   sharp minimap raises it from its own frame — once per present, whenever it
   would draw — and `tagpu_gui_flush`'s watchdog drops it after 90 silent
   frames, the same shape tagpu_fxown uses for the effect tables. It costs the
   publisher an ew x eh x 3 interleave per publish when set, and at k = 1 the
   sharp minimap is deliberately the engine's own, so an unarmed frame must not
   pay for it. Written on the render thread, read on the game thread; one
   writer, no ordering owed — a frame either side of the change costs one frame
   of the engine's own minimap. */
void tagpu_gui_set_want_minimap(int on, unsigned int frame_counter);
int  tagpu_gui_want_minimap(void);
/* THE LEVEL'S MINIMAP PICTURE IS ACKNOWLEDGED, NOT ASSUMED (landing 4c, and its
   review). The publisher puts it in a packet and the mailbox is latest-wins:
   a packet the render thread never takes is a counted statistic, not an error,
   so "it went into one packet" is not "the consumer has it". The render half
   raises this to `level_gen + 1` the moment it has copied that level's picture,
   and the publisher keeps sending until it does — which is normally one extra
   packet and never more than the frames it takes the consumer to run once.
   0 = nothing held. Written on the render thread, read on the game thread; one
   writer, monotone within a level. */
void     tagpu_gui_set_minimap_have(unsigned level_gen_plus_1);
unsigned tagpu_gui_minimap_have(void);

/* THE VULKAN LANE'S HAND-OVER STOOD HERE -- `TAGPU_GUIHAND`, the op stream a
   present had applied, copied while it was live so the Vulkan twin store could
   replay it. Its one consumer was the composite, and it went with it. */#endif
