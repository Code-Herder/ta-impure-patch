#ifndef TAGPU_DOCK_H
#define TAGPU_DOCK_H
/* tagpu_dock -- the watching dock: the bar along the bottom of the world on a level that plays a
   recording, with the transport (rewind, play/pause, forward), the speeds, a bookmark, Leave, the
   time readouts and the timeline strip with its markers. Design:
   research/notes/tadr-port/demo-recorder-s5-ui.md, *The watching dock*; the engine facts sit
   beside the code in tagpu_dock.c.

   THE SHAPE. DOCK.GUI is a real TA screen in impure-patch.ufo, pushed over ARMMAIN2 through
   tagpu_menu.c's one screen slot, which it shares with RENDER.GUI (RENDER wins while it is
   wanted). The engine hit-tests and dispatches its buttons and draws its labels; the DLL owns the
   strip, drawn at the post-GUI observer 0x46A308 and hit-tested on the shield's two input paths.
   The markers come from a worker thread that reads the recording through its own reader.

   GAME THREAD, every entry point but the worker, which reads only the recording file. */

#include "tagpu_ufo.h"

#define TAGPU_DOCK_SCREEN "DOCK.GUI"

/* attach, while tagpu_menu_init builds the archive: impure/dock.gui and anims/dock.gaf. Writes at
   most `cap` entries and returns how many, 0 when the art is missing (logged). Their bytes live
   until tagpu_dock_files_free. */
int  tagpu_dock_files(TAGPU_UFO_FILE* f, int cap);
void tagpu_dock_files_free(void);

/* After the archive write: `ready` = the archive holds the files above, GUI_Load is redirected to
   our folder for DOCK.GUI, and the frame tick and the post-GUI observer are in. */
void tagpu_dock_install(int ready);

/* Does GUI_Load read `name` out of our folder? Asked by tagpu_menu.c's redirect. */
int  tagpu_dock_redirects(const char* name);

/* 1 when the level plays a recording and the dock fits between the side panel and the screen's
   right edge. */
int  tagpu_dock_wanted(void);

/* The slot's open, around GUI_Load(DOCK.GUI) and its stage 1: `place` writes the rects into the
   parsed records before stage 1 sizes the panel's surface from them, `built` repaints the ground
   frame stage 1 loaded and forgets what the setters were last given. */
void tagpu_dock_place(char* ctrls);
void tagpu_dock_built(void* gi, void* gm);

/* every frame while the dock holds the slot, from the DrawGameScreen observer; `on_top` = the
   dock is the top screen, the only one the engine's setters reach by name */
void tagpu_dock_tick(void* gi, int on_top);

/* the screen's OnCommand: __stdcall void(GUIInfo*), the actuated index in gi+0x60 */
void __stdcall tagpu_dock_oncommand(void* gi);

/* the slot let the dock go: popped, or popped behind the slot's back */
void tagpu_dock_closed(void);

/* Does the dock's engine rect hold this engine-space point? 0 when the dock is not open. */
int  tagpu_dock_owns(int gx, int gy);

/* The strip's buttons, in engine space, from both of the shield's input paths. 1 = consumed; a
   release is consumed exactly when its press was. The left button drags the playhead and seeks
   at the release; the right button removes the bookmark under it. */
int  tagpu_dock_click(int gx, int gy, int down, int on_top);
int  tagpu_dock_rclick(int gx, int gy, int down, int on_top);

/* The window lost the mouse (focus, activation or capture): a drag in progress ends with no
   seek. */
void tagpu_dock_cancel_press(void);

/* the post-GUI observer 0x46A308: the strip's fill, markers and playhead over the dock's panel;
   `top` the top screen, `gm` the dock's */
void tagpu_dock_draw(void* top, void* gm);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60: the worker is stopped and joined, and the
   level's markers and bookmarks forgotten. */
void tagpu_dock_level_end(void);

#endif
