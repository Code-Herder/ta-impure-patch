#ifndef TAGPU_CARDS_H
#define TAGPU_CARDS_H
#include <windows.h>
/* Game-thread companion to DOCK.GUI. The post-GUI layer shares the dock's paired-click rule;
   an engine modal hides it. No pointers to its mutable model cross into a frame packet. */
void tagpu_cards_open(int q8, void* gm);
void tagpu_cards_close(void);
void tagpu_cards_tick(int on_top);
void tagpu_cards_draw(void);
int tagpu_cards_owns(int x, int y);
int tagpu_cards_click(int x, int y, int down, int right, int on_top);
int tagpu_cards_wheel(UINT msg, WPARAM wp, LPARAM lp);
void tagpu_cards_cancel(void);
/* The palette index of the engine's colour `logo` (PlayerInfo+0x96), 59 when it does not
   resolve: the cards' chips and the replay's overlays. Game thread. */
unsigned tagpu_cards_colour(unsigned logo);
/* The dock's Overlays menu, drawn over the panel's bottom right. It opens only while the cards
   are shown (available) and closes with them; the dock toggles it from its button and closes it
   on a press anywhere but the menu and that button. */
int  tagpu_cards_menu_available(void);
int  tagpu_cards_menu_open(void);
void tagpu_cards_menu_toggle(void);
void tagpu_cards_menu_close(void);
int  tagpu_cards_menu_owns(int x, int y);
#endif
