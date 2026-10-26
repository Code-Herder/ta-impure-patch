#ifndef TAGPU_WIKI_H
#define TAGPU_WIKI_H
/* tagpu_wiki -- the in-game wiki's game-thread half: its button and window opened and closed, the
   input it takes while open, the pause it holds in single player, and the record it hands the
   render thread. Design: research/notes/ingame-wiki-plan.md (*The design*, *Landing 2*); the
   specification is the mockup, tools/wiki/mockup/ (research/notes/ingame-wiki.md).

   GAME THREAD, every entry point but two: the shield's two input paths and the window procedure
   TA pumps on that thread, the packet's fill, the command record's take and the teardown wrap's
   entry. tagpu_wiki_holds_input and tagpu_wiki_frozen are the exceptions: any thread, the engine's
   mouse thread among them, reads them through the polls (winapi_hooks.c). */
#include <windows.h>
#include "tagpu_wikipk.h"

/* THE REAL PATH: a message at the top of TA's window procedure, from tagpu_shield_wndproc after
   its WM_TAGPU_* cases and before the sprocket and the replay HUD. 1 = taken: nothing of it
   reaches the engine. While the window is open every key, character, raw-input, move, button and
   wheel message is taken; while it is closed, a left press on the wiki button of the last frame
   drawn (the client point in lParam), and the release or the repeats of a press the wiki took. */
int  tagpu_wiki_message(UINT msg, WPARAM wp, LPARAM lp);

/* THE INJECTED PATH: a TAGPU_M_* event from the shield's deliver_mouse, after the device point is
   converted and before the engine's point is stored in g_ddraw.cursor: (cx, cy) the client point,
   cx < 0 when the injection named none, and (gx, gy) the engine's point the event carries. 1 =
   taken: the caller stores no point, sets no virtual button and sends no message -- except that a
   taken release still clears its virtual button. */
int  tagpu_wiki_mouse(int code, int cx, int cy, int gx, int gy);

/* An injected key, from deliver_key before its virtual state is set: 1 = taken, as above (a taken
   key-up still clears the virtual key). And an injected character: 1 = taken. */
int  tagpu_wiki_key(unsigned vk, int up);
int  tagpu_wiki_char(void);

/* The window lost the mouse or the keyboard: the controls stop showing pressed. A press the wiki
   took stays owed its release. */
void tagpu_wiki_cancel(void);

/* ANY THREAD. 1 while the wiki holds the input; then the polls answer a player who let go, and
   GetCursorPos the frozen point, which tagpu_wiki_frozen gives (stored before the capture word,
   so a reader that saw the word set reads the point of that open). */
LONG tagpu_wiki_holds_input(void);
void tagpu_wiki_frozen(LONG* x, LONG* y);

/* The packet's fill, game thread: 1 with the record while the wiki can be drawn -- in a level, on
   the Vulkan lane, while the replay HUD's pass stands (tagpu_hudstate_on) -- else 0, and an open
   window closes, so the wiki is never open where it cannot be seen or closed. */
int  tagpu_wiki_fill(TAGPU_PK_WIKI* w);

/* The command record's take: the hit map of the last frame the wiki was drawn on, copied, so a
   press between two takes reads this thread's own copy. NULL is no controls. */
void tagpu_wiki_take(const TAGPU_CMD_WIKI* c);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60: the window closes and the pause is
   forgotten, written by nothing -- the level it was taken in is ending. */
void tagpu_wiki_level_end(void);

#endif
