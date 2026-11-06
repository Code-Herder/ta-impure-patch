#ifndef TAGPU_MODSUI_H
#define TAGPU_MODSUI_H
/* tagpu_modsui -- the Mods screen's state and input, at the main menu (research/notes/mod-manager-plan.md, *The screen*).
   The window is tagpu_modslay.c's and the render thread's: it draws the page published here with the wiki's kit and pass
   (tagpu_wikistate.c), and the wiki's layer hands back what it drew.

   TWO THREADS, ONE LOCK, AND NOTHING ELSE CROSSES. The page goes one way and the hit map the other, each copied under
   s_lock, which is held over a memcpy and no call. The game thread owns everything else: the tabs, the choices, the
   installs. Nothing here names engine memory, and no packet carries this: a shell packet holds a header alone.

   MODAL, BY THE WIKI'S CAPTURE (tagpu_wiki_capture): while open the engine's pointer stands where it was and its key polls read
   up, and the window's messages stop here before the engine sees them. A press taken owes its release; a release that was
   not taken is the engine's. */
#include <windows.h>
#include "tagpu_modslay.h"
#include "tagpu_wikipk.h"

void tagpu_modsui_init(void);           /* at attach, before any thread can ask */

/* Game thread, at the MODS button. `folder` names what this folder runs when it is what is running, "" else. */
void tagpu_modsui_open(const char* folder);
/* Any thread. */
int  tagpu_modsui_is_open(void);
/* Game thread, each flip: the installs' progress. */
void tagpu_modsui_tick(int in_game);

/* The window's messages and the injected path, the game thread: 1 taken, 0 the engine's. */
int  tagpu_modsui_message(UINT msg, WPARAM wp, LPARAM lp);
int  tagpu_modsui_mouse(int code, int cx, int cy);
int  tagpu_modsui_key(unsigned vk, int up);
void tagpu_modsui_cancel(void);

/* Render thread. The page when it has changed since *seen: 1 with `out` filled; 0 when it has not; -1 closed. */
int  tagpu_modsui_page(MODSPAGE* out, unsigned* seen);
/* What the last frame drew. */
void tagpu_modsui_drawn(const TAGPU_CMD_WIKI* map);

#endif
