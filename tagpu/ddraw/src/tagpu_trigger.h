#ifndef TAGPU_TRIGGER_H
#define TAGPU_TRIGGER_H

/* THE ON-DEMAND TRIGGER FAMILY, GATHERED BEHIND ONE CALL [the vulkan-only
   plan, landing 10c].

   These are the `.trigger`-file observers -- peek, the weapon dump, the GUI
   snapshot `tacli ui` reads, the unit and feature catalogues, and the scenario
   applier's DETECTION half. They are not rendering. They read engine memory
   and write a file when a trigger appears, and they must run at the MENUS as
   well as in a game, because that is where half of `tacli` works.

   They used to be called one by one from `tagpu_overlay_draw`, which is
   reached only from render_ogl.c and render_vk.c. `render_gdi.c` contains no
   `tagpu_` call at all, so on that lane none of them ran: `tacli ui` returned
   "no UI snapshot appeared" and the shell could not be driven past the main
   menu. Landing 10b's whole claim is that `renderer=gdi` is this project's
   stock reference, and a reference nothing can observe is not much of one.

   Now they hang off the engine's own flip (`0x4C63A0`, observed by
   tagpu_gui_hook.c), which every renderer reaches, on the GAME thread.

   WHAT THE CALLER OWES: a TAGPU_FRAME whose geometry fields are real. They are
   all `g_ddraw` members the render thread's packet copies anyway. `packet` may
   be NULL -- every one of these null-checks it -- but note that
   `tagpu_input_frame` is deliberately NOT in this set for exactly that reason;
   see the note at its call site. */

struct TAGPU_FRAME;

void tagpu_triggers_frame(const struct TAGPU_FRAME* f);

#endif
