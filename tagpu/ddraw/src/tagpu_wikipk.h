#ifndef TAGPU_WIKIPK_H
#define TAGPU_WIKIPK_H
/* tagpu_wikipk -- the in-game wiki's two records between the threads (research/notes/
   ingame-wiki-plan.md, *Data and threads*):

     TAGPU_PK_WIKI    game thread -> render thread, in the frame packet: what the wiki shows this
                      draw, filled by tagpu_wiki_fill (tagpu_wiki.c) inside the packet's fill.
     TAGPU_CMD_WIKI   render thread -> game thread, in the command record: the controls of the
                      last frame the wiki was drawn on, taken by tagpu_wiki_take.

   VALUES ONLY. Neither record holds a pointer, so neither thread can reach the other's memory
   through it, and a record copied out of its slot is whole on its own.

   THE PACKET'S WIKI AREA IS ITS LAST (tagpu_packet_pub.c, after the effects models): the record
   can grow without moving or cutting any table before it. Every array below carries its bound
   here, and every count is checked against it where the record is read.

   No test group names this file as a source. Three simulation files include it through
   tagpu_packet.h (tagpu_patches.c, tagpu_weapons.c, tagpu_datakeys.c), and none reads a type of
   it: what the wiki draws and takes changes nothing they read, and nothing the recorder carries. */
#include <stdint.h>

#define TAGPU_WIKI_KEY    24u       /* a control's key, NUL-terminated                       */
#define TAGPU_CMD_WIKIHITS 160u     /* the hit map: every control a frame of the wiki draws   */

/* What the wiki shows this draw. Present in a packet exactly while the wiki can be drawn: in a
   level, on the Vulkan lane, while the replay HUD's pass stands (tagpu_hudstate_on) and the
   wiki's layout has not refused its kit (tagpu_wikistate_refused). */
typedef struct TAGPU_PK_WIKI {
    uint8_t  open;              /* the window is open: the game takes no input              */
    uint8_t  btn_down;          /* the wiki button's press was taken and its release is owed */
    uint8_t  x_down;            /* the close button's, likewise                              */
    uint8_t  paused;            /* the wiki holds single player's pause                      */
    int16_t  ptr_x, ptr_y;      /* the pointer's client point, as the last move noted it
                                   (mouse_last_client); -1 for none or outside the picture  */
} TAGPU_PK_WIKI;

/* THE WIKI'S HIT MAP: the controls of the last frame the wiki was drawn on, topmost first, in
   the window's client pixels, each key as tagpu_wikiframe.c (and later the page's layout) makes
   it. A LEVEL: a frame that did not draw the wiki posts n = 0 and drawn = 0. */
typedef struct TAGPU_CMD_WIKIHIT {
    int16_t  x0, y0, x1, y1;    /* [x0, x1) x [y0, y1)                                       */
    char     key[TAGPU_WIKI_KEY];
} TAGPU_CMD_WIKIHIT;
typedef struct TAGPU_CMD_WIKI {
    uint32_t n;                 /* <= TAGPU_CMD_WIKIHITS                                      */
    uint8_t  drawn;             /* the frame this map is drew the wiki layer                  */
    uint8_t  open;              /* that frame drew the window, not the button alone           */
    uint8_t  pad[2];
    TAGPU_CMD_WIKIHIT hit[TAGPU_CMD_WIKIHITS];
} TAGPU_CMD_WIKI;

#endif
