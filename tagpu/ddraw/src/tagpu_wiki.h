#ifndef TAGPU_WIKI_H
#define TAGPU_WIKI_H
/* tagpu_wiki -- the in-game wiki's game-thread half: its button and window opened and closed, the
   input it takes while open, the pause it holds in single player, and the record it hands the
   render thread. Design: research/notes/ingame-wiki-plan.md (*The design*, *Landing 2*); the
   specification is the mockup, tools/wiki/mockup/ (research/notes/ingame-wiki.md).

   GAME THREAD, every entry point but four: the shield's two input paths and the window procedure
   TA pumps on that thread, the packet's fill, the command record's take and the teardown wrap's
   entry. tagpu_wiki_holds_input, tagpu_wiki_frozen and tagpu_wiki_cursor_store are any thread's,
   the engine's mouse thread among them, through the polls (winapi_hooks.c); tagpu_wiki_init is
   DllMain's. */
#include <windows.h>
#include "tagpu_wikipk.h"

/* THE REAL PATH: a message at the top of TA's window procedure, from tagpu_shield_wndproc after
   its WM_TAGPU_* cases and before the sprocket and the replay HUD. 1 = taken: nothing of it
   reaches the engine. While the window is open every key, character, raw-input, move, button and
   wheel message is taken; while it is closed, a left press on the wiki button of the last frame
   drawn (the client point in lParam), any press on a control of a last frame that still drew the
   window open, and the release or the repeats of a press the wiki took.
   TAGPU_WIKI_FROZEN: not taken, but handed to the engine at the frozen point by the caller. */
int  tagpu_wiki_message(UINT msg, WPARAM wp, LPARAM lp);

/* The answer of tagpu_wiki_message and tagpu_wiki_mouse for the release of a button the engine
   holds down, made while the window is open: the caller hands it to the engine at the frozen point
   (tagpu_wiki_frozen), with no client point, as an injected release there is handed. */
#define TAGPU_WIKI_FROZEN 2

/* Every message handed to TA's window procedure, at both its doors (the tail of cnc-ddraw's
   window procedure and the shield's to_game): the buttons and keys the engine holds down, which
   the wiki never keeps a release of. */
void tagpu_wiki_engine_got(UINT msg, WPARAM wp);

/* THE INJECTED PATH: a TAGPU_M_* event from the shield's deliver_mouse, after the device point is
   converted and before the engine's point is stored in g_ddraw.cursor: (cx, cy) the client point,
   cx < 0 when the injection named none, and (gx, gy) the engine's point the event carries. 1 =
   taken: the caller stores no point, sets no virtual button and sends no message -- except that a
   taken release still clears its virtual button. TAGPU_WIKI_FROZEN: deliver it at the frozen
   point instead. */
int  tagpu_wiki_mouse(int code, int cx, int cy, int gx, int gy);

/* An injected key, from deliver_key before its virtual state is set: 1 = taken, as above (a taken
   key-up still clears the virtual key). And an injected character: 1 = taken. */
int  tagpu_wiki_key(unsigned vk, int up);
int  tagpu_wiki_char(void);

/* The window lost the mouse or the keyboard: the controls stop showing pressed. A press the wiki
   took stays owed its release. */
void tagpu_wiki_cancel(void);

/* ANY THREAD. 1 while the wiki holds the input; then the polls answer a player who let go, and
   GetCursorPos the frozen point, which tagpu_wiki_frozen gives as one pair, under the cursor lock
   the open and the close store under. */
LONG tagpu_wiki_holds_input(void);
void tagpu_wiki_frozen(LONG* x, LONG* y);

/* ANY THREAD: GetCursorPos's store of the live engine point (x, y) in g_ddraw.cursor, tested
   against the capture word under the cursor lock, so the poll and the open are ordered: while the
   wiki holds the input nothing is stored and the frozen point is the answer. `outside`: the
   pointer is not over the picture, and the UI layer's point is forgotten with the store. The
   answer goes to (*ax, *ay); 1 when the wiki held the input. */
int  tagpu_wiki_cursor_store(int x, int y, int outside, LONG* ax, LONG* ay);

/* DllMain's attach, before any DirectDraw object exists: the cursor lock. */
void tagpu_wiki_init(void);

/* The packet's fill, game thread: 1 with the record while the wiki can be drawn -- in a level, on
   the Vulkan lane, while the replay HUD's pass stands (tagpu_hudstate_on) and the wiki's layout
   has not refused (tagpu_wikistate_refused) -- else 0, and an open window closes, so the wiki is
   never open where it cannot be seen or closed. `*len` is the record's length to carry:
   TAGPU_PK_WIKI_HEAD, or the whole record with the stage. While the window is open the fill steps
   the demo (at most 8 ticks) and copies its view into the record. It answers the trigger
   tagpu_wiki.check. `again`: this fill redoes one made for the same publish, which the slot's
   growth discarded. The first fill of a publish closes a window the last published packet carried
   no record of (tagpu_wiki_carried), so the wiki is never open past one frame that cannot show it.
   Every fill that returns 1 is followed by one tagpu_wiki_carried. */
int  tagpu_wiki_fill(TAGPU_PK_WIKI* w, uint32_t* len, int again);

/* The whole record did not fit the packet: `w` made a head without the stage -- has_stage 0, and
   a message on the stage instead -- and the head's length returned, for the caller to place. */
uint32_t tagpu_wiki_head_only(TAGPU_PK_WIKI* w);

/* After the fill's record is placed: `carried` the bytes of it the packet holds, 0 for none. It
   answers the trigger tagpu_wiki.dump with the record as placed. */
void tagpu_wiki_carried(const TAGPU_PK_WIKI* w, uint32_t carried);

/* The command record's take: the hit map, the scrolls and the scrollbars of the last frame the
   wiki was drawn on, copied and bounded as data, so a press between two takes reads this thread's
   own copy. NULL is no controls. */
void tagpu_wiki_take(const TAGPU_CMD_WIKI* c);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60: the window closes and the pause is
   forgotten, written by nothing -- the level it was taken in is ending. */
void tagpu_wiki_level_end(void);

#endif
