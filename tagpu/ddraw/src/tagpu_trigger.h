#ifndef TAGPU_TRIGGER_H
#define TAGPU_TRIGGER_H

/* THE ON-DEMAND TRIGGER FAMILY, GATHERED BEHIND ONE CALL [the vulkan-only
   plan, landing 10c].

   These are the `.trigger`-file observers -- peek, the weapon dump, the GUI
   snapshot `tacli ui` reads, the unit and feature catalogues, the scenario
   applier's DETECTION half, and since landing 11-2a the engine-surface
   screenshot behind `tacli shot`. They are not rendering. They read engine
   memory and write a file when a trigger appears, and they must run at the
   MENUS as well as in a game, because that is where half of `tacli` works.

   THE SCREENSHOT IS THE ONE MEMBER THAT ONLY ARMS. The others answer in place;
   it cannot, because the family runs at the flip's ENTRY and the engine copies
   back buffer -> primary INSIDE that call, so the primary still holds the
   previous frame here. It sets a flag and `dds_Unlock`'s primary branch takes
   the picture -- see screenshot.h. It is also the one member with no counter
   of its own: it costs one GetFileAttributes per pass of this family.

   They used to be called one by one from `tagpu_overlay_draw`, which is
   reached only from render_ogl.c and render_vk.c. `render_gdi.c` contains no
   `tagpu_` call at all, so on that lane none of them ran: `tacli ui` returned
   "no UI snapshot appeared" and the shell could not be driven past the main
   menu. Landing 10b's whole claim is that `renderer=gdi` is this project's
   stock reference, and a reference nothing can observe is not much of one.

   Now they hang off the engine's own flip (`0x4C63A0`, observed by
   tagpu_gui_hook.c), which every renderer reaches, on the GAME thread.

   AND THE HOST INSTALLS UNCONDITIONALLY, WHICH IT DID NOT AT FIRST. The flip
   is reached on every lane, but for one commit the OBSERVER of it was not
   installed on every lane: `tagpu_gui_init` sat behind `tagpu_gui.on`, which
   `tagpu_defaults.off` — written by a bare `tacli launch` — takes away. So
   this family, and with it every `tacli` verb and all of the input injection,
   went silently dead on an ordinary instance, on EVERY renderer. The landing
   review of 10c-2 caught it. `tagpu_gui_init` now installs the observer
   whenever the flip's bytes match and gates only the UI layer on the trigger.
   Two preconditions remain -- the byte match, and tagpu_detour_observe itself
   succeeding (it can refuse on its allocation, on the chain rule, or on the
   VirtualProtect in tagpu_detour_land) -- and each has its own log line saying
   `no tacli verb can answer`.

   WHAT THE CALLER OWES: a TAGPU_FRAME whose geometry fields are real. They are
   all `g_ddraw` members the render thread's packet copies anyway, and they are
   read from the thread that WRITES them, so they cannot tear -- the render
   thread's old read was the cross-thread one.

   `packet` stays NULL, and none of these six reads it at all; do not take that
   as a null-check they perform, because they do not. That is the line the family
   is drawn along, and landing 10c-2 SPLIT `tagpu_input.c` on it rather than
   excluding the file: its token half (`tagpu_input_frame` -- keys, clicks, the
   shield's expiries) reads no packet and joined the set, while the half that
   dereferences `f->packet` through `do_eye` stayed on the render thread as
   `tagpu_input_eye_frame`. Each half keeps its own poll counter, so each runs at
   the cadence of the clock it is handed.

   AND THE CALLER OWES A CLOCK, NOT A FLIP. These throttle themselves on
   `frame_counter % 5` (`% 15` for peek), written against the PRESENT rate of ~60/s; the engine's
   flip runs at thousands per second in the shell. The caller must gate on
   elapsed time and hand over a counter that advances with it. */

struct TAGPU_FRAME;

void tagpu_triggers_frame(const struct TAGPU_FRAME* f);

#endif
