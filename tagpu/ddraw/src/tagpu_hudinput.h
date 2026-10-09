#ifndef TAGPU_HUDINPUT_H
#define TAGPU_HUDINPUT_H
/* tagpu_hudinput -- the replay HUD's input and its commands: a click hit-tested against the
   controls of the last frame the HUD was drawn on, and acted on through the replay's own calls
   (inc/tagpu_replay.h). Design: research/notes/hud-in-game.md, *Input*.

   GAME THREAD, every entry point: the two mouse paths of the shield and the wndproc TA pumps on
   that thread, the packet's fill, the command record's take, the DrawGameScreen observer and the
   teardown wrap's entry. It holds the HUD's own state -- the button held, the folds, the scroll,
   the toast, the scale -- and hands it to the render thread in the packet; the render thread
   hands back the drawn frame's controls in the command record. Nothing here names engine memory:
   the one engine fact the gate needs, a menu over the world, is tagpu_menu_modal's. */
#include <windows.h>
#include "tagpu_packet.h"

/* The command record's take: the hit map of the last frame the HUD was drawn on, copied, so a
   click between two takes reads this thread's own copy. NULL -- no record the exchange vouches
   for -- is no controls. */
void tagpu_hudinput_take(const TAGPU_CMD_HUD* h);

/* A left or right button at the window's client point (cx, cy) -- cx < 0 when the point is not
   known, an injected click in the engine's own space -- from either of the shield's paths. 1 =
   consumed: the engine never sees it. A release is consumed exactly when its press was. A point
   the render-options panel or its sprocket owns is never the HUD's (tagpu_menu_owns_point). */
int  tagpu_hudinput_button(int cx, int cy, int down, int right);

/* WM_MOUSEWHEEL at the pointer: over the column it scrolls it, over any control it is taken. */
int  tagpu_hudinput_wheel(UINT msg, WPARAM wp);

/* The window lost the mouse: a drag ends where it is; its release, should it come, is still
   consumed. */
void tagpu_hudinput_cancel(void);

/* The packet's fill: the HUD's own state into `h`, and the timeline drag's seek, once a frame. */
void tagpu_hudinput_fill(TAGPU_PK_HUD* h);

/* The replay HUD's own scale, percent: its button's last choice, else the settings store's. */
unsigned tagpu_hudinput_scale(void);

/* Leave, for tagpu_menu.c's observer, which opens the question where the engine's screens are
   opened: 1 when the button asked for it since the last call; `opened` says whether it was.
   leave_shown, every frame the observer runs, whatever the HUD's state: whether the question is on
   the stack; once it is gone -- answered No -- the pause from before the click is put back. */
int  tagpu_hudinput_leave_due(void);
void tagpu_hudinput_leave_opened(int opened);
void tagpu_hudinput_leave_shown(int open);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60: the level's HUD state is forgotten. */
void tagpu_hudinput_level_end(void);

#endif
