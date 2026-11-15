#ifndef TAGPU_SHARETICK_H
#define TAGPU_SHARETICK_H
/* The player's share levels, set on the game's own resource bar
   (research/notes/modules/share.md). Everything here runs on the GAME thread: the draw from the
   post-GUI observer tagpu_menu.c owns, the rest from the window procedure (tagpu_shield.c). */

/* Draws a tick on each resource bar, and the levels under the bar while the pointer is on it;
   follows a drag. Inside DrawGameScreen, after its GUI draw and before the flip. */
void tagpu_sharetick_draw(void);

/* 1 when (gx, gy), in the engine's own coordinates, is on a bar's handle strip. */
int tagpu_sharetick_owns(int gx, int gy);

/* A left press or release at (gx, gy). 1 when it is ours: a press on a strip, and the release
   that ends that press. */
int tagpu_sharetick_click(int gx, int gy, int down);

/* 1 while a press the tick took is held. */
int tagpu_sharetick_dragging(void);

/* The window lost the mouse: a drag ends with the level it has. */
void tagpu_sharetick_cancel(void);

#endif
