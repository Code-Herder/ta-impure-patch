#ifndef TAGPU_TOPBAR_H
#define TAGPU_TOPBAR_H
/* What Impure draws on the game's own top bar besides the share tick: the repaint every drawing
   there needs, and the weather report (research/notes/modules/topbar.md). Everything here runs on
   the GAME thread, from the post-GUI observer tagpu_menu.c owns, inside DrawGameScreen. */

/* Once per DrawGameScreen, before anything is drawn on the top bar: notes whether the resource
   block repainted the bar after a repaint asked for with tagpu_topbar_repaint. */
void tagpu_topbar_frame(void);

/* 1 when this DrawGameScreen's resource block repainted the whole top bar after a repaint was
   asked: nothing Impure drew there before this frame is on it any more. 0 says nothing either way:
   the block also repaints by itself whenever a value it shows changes. */
int tagpu_topbar_fresh(void);

/* Asks the next DrawGameScreen to repaint the whole top bar, tiles and readouts: what was drawn
   on it can then be drawn again somewhere else, or not at all. Asking again while one is pending
   writes nothing. */
void tagpu_topbar_repaint(void);

/* The weather report: the wind and tide one generator of the mod makes, and the game time, in the
   viewed side's font right of the resource bars, as TADR draws it. */
void tagpu_topbar_draw(void);

#endif
