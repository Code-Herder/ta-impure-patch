#ifndef TAGPU_TRIGGER_H
#define TAGPU_TRIGGER_H

/* THE ON-DEMAND TRIGGER FAMILY, GATHERED BEHIND ONE CALL.

   These are the `.trigger`-file observers -- peek, the weapon dump, the GUI
   snapshot `tacli ui` reads, the unit and feature catalogues, the scenario
   applier's DETECTION half, and the engine-surface screenshot behind `tacli
   shot`. They are not rendering. They read engine memory and write a file when
   a trigger appears, and they must run at the MENUS as well as in a game,
   because that is where half of `tacli` works.

   THE SCREENSHOT IS THE ONE MEMBER THAT ANSWERS A PASS LATE, on purpose. The
   others answer in place; it cannot, because the family runs at the flip's
   ENTRY and the engine copies back buffer -> primary INSIDE that call, so the
   primary here holds the frame the previous flip presented. So it services a
   pending arm BEFORE it polls for a new one: a trigger seen on one pass is
   answered on the next, with a frame presented in between. See screenshot.h
   for the two hosts that were tried for the capture and why both were worse.
   It is also the one member with no counter of its own: it costs one
   GetFileAttributes per pass of this family.

   They hang off the engine's own flip (`0x4C63A0`, observed by
   tagpu_gui_hook.c), which every renderer reaches, on the GAME thread -- not
   off a renderer's present: `render_gdi.c` contains no `tagpu_` call at all,
   and the GDI backend is this project's stock reference, so hung off a present
   none of them would run there, `tacli ui` would return "no UI snapshot
   appeared" and the shell could not be driven past the main menu.

   AND THE HOST INSTALLS UNCONDITIONALLY. `tagpu_gui_init` installs the observer
   whenever the flip's bytes match and gates only the UI layer on
   `tagpu_gui.on`, which `tagpu_defaults.off` — written by a bare `tacli launch`
   — takes away; gating the observer as well would take this family, and with it
   every `tacli` verb and all of the input injection, silently dead on an
   ordinary instance, on EVERY renderer. Two preconditions remain -- the byte
   match, and tagpu_detour_observe itself succeeding (it can refuse on its
   allocation, on the chain rule, or on the VirtualProtect in tagpu_detour_land)
   -- and each has its own log line saying `no tacli verb can answer`.

   WHAT THE CALLER OWES: a TAGPU_FRAME whose geometry fields are real. They are
   all `g_ddraw` members the render thread's packet copies anyway, and they are
   read from the thread that WRITES them, so they cannot tear.

   `packet` stays NULL, and none of these six reads it at all; do not take that
   as a null-check they perform, because they do not. That is the line the
   family is drawn along, and `tagpu_input.c` is SPLIT on it: its token half
   (`tagpu_input_frame` -- keys, clicks, the shield's expiries) reads no packet
   and belongs to the set, while the half that dereferences `f->packet` through
   `do_eye` stays on the render thread as `tagpu_input_eye_frame`. Each half
   keeps its own poll counter, so each runs at the cadence of the clock it is
   handed.

   AND THE CALLER OWES A CLOCK, NOT A FLIP. These throttle themselves on
   `frame_counter % 5` (`% 15` for peek), written against the PRESENT rate of ~60/s; the engine's
   flip runs at thousands per second in the shell. The caller must gate on
   elapsed time and hand over a counter that advances with it. */

struct TAGPU_FRAME;

void tagpu_triggers_frame(const struct TAGPU_FRAME* f);

#endif
