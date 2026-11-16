#ifndef TAGPU_BOARD_H
#define TAGPU_BOARD_H
/* tagpu_board.c -- the drawing board (research/notes/modules/board.md): while the draw key is
   held, a left drag draws a line on the ground, a middle click places a marker, a double click
   opens a text box for a marker's text, a drag on a marker moves it, a right drag erases and a
   right double click on a marker deletes it. Drawn on Vulkan only, in a level in play, never on a
   puppet seat; in a network game every peer receives every player's operations, and each player
   sees their own marks, their mutual allies' and, as a watcher, everyone's.

   Every function here runs on the GAME thread: the keys and buttons from the window procedure
   (tagpu_shield.c), the pointer's world point from the in-play mouse handler (tagpu_route.c),
   the fill from the packet's publisher. The render thread sees the board only as the packet's
   copied tables (PK_BOARD, PK_BDSEG, PK_BDMARK). */
#include "tagpu_packet.h"

/* DLL attach, after tagpu_route_init: arms when the router's right-press stub is installed */
void tagpu_board_init(void);
/* the level's teardown (tagpu_packet_pub.c): every mark, the gesture and the text box cleared */
void tagpu_board_level_end(void);

/* A key the window procedure delivers: 0 not the board's; 1 taken; 2 taken, and its character
   goes to tagpu_board_char rather than to the engine (the text box is open). */
int tagpu_board_key(unsigned vk, int up);
/* A character the window procedure delivers: 1 when the board took it -- every character while
   the draw key is held or the text box is open -- and the engine must not see it. */
int tagpu_board_char(unsigned ch);
/* A mouse button message (WM_[LRM]BUTTON{DOWN,UP,DBLCLK}) the window procedure delivers: 1 when
   the board took it, and the engine must not see it. */
int tagpu_board_mouse(unsigned msg);
/* the window lost the keyboard or the mouse: the key, the gesture and the text box end */
void tagpu_board_cancel(void);

/* The in-play handler's record at the right-press branch (tagpu_route.c): the world point and
   the region bits are this record's. Never consumes it. */
void tagpu_board_record(const int* rec, unsigned msg);

/* A RECMP_MARK body another peer sent, renumbered to this peer's seats (tagpu_recorder.c, in
   delivery order): its operations applied as its seat's when this player sees that seat
   (tagpu_share_board_seen), each under the rights it carries cut to what this player sees; a seat
   this player does not see is counted and not stored. 0, and nothing applied, for a body
   tagpu_boardwire_check refuses. Game thread. */
int tagpu_board_apply(const void* body, unsigned bytes);
/* a body the channel dropped before tagpu_board_apply: counted in the level's `refused=` */
void tagpu_board_refused(void);

/* The player's operations not yet sent, as one RECMP_MARK body of at most `cap` bytes, taken out
   of the queue: its bytes, 0 when none is queued. Game thread. */
unsigned tagpu_board_outbox(unsigned char* body, unsigned cap);
/* the queue emptied: a game with no channel to send it on, or one that has failed */
void tagpu_board_outbox_drop(void);

/* The packet's tables, from the publisher. 1 with `h` filled in a level in play; `*ns` segments
   and `*nm` markers written, at most the caps, `*cut` set when more would have been. */
int tagpu_board_fill(TAGPU_PK_BOARD* h, TAGPU_PK_BDSEG* segs, unsigned maxs, unsigned* ns,
                     TAGPU_PK_BDMARK* marks, unsigned maxm, unsigned* nm, int* cut);

#endif
