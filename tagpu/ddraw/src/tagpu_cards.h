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
/* A player's colour as a palette index: the middle pixel of its logo's frame in the engine's
   colour sequence, which is what a card's header chip shows; 59 when the sequence or the frame
   does not validate. Game thread. */
unsigned tagpu_cards_logo_pal(unsigned logo);
#endif
