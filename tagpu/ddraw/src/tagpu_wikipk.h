#ifndef TAGPU_WIKIPK_H
#define TAGPU_WIKIPK_H
/* tagpu_wikipk -- the in-game wiki's two records between the threads (research/notes/
   ingame-wiki-plan.md, *Data and threads*):

     TAGPU_PK_WIKI    game thread -> render thread, in the frame packet: what the wiki shows this
                      draw, filled by tagpu_wiki_fill (tagpu_wiki.c) inside the packet's fill.
     TAGPU_CMD_WIKI   render thread -> game thread, in the command record: the controls of the
                      last frame the wiki was drawn on, and what that frame's layout answered,
                      taken by tagpu_wiki_take.

   VALUES ONLY. Neither record holds a pointer, so neither thread can reach the other's memory
   through it, and a record copied out of its slot is whole on its own. The stage is a view of the
   wiki's simulation copied by value (tagpu_wikistage_copy): the simulation lives on the game
   thread alone, and the render thread draws from the copy.

   THE PACKET'S WIKI AREA IS ITS LAST (tagpu_packet_pub.c, after the effects models): the record
   can grow without moving or cutting any table before it. Every array below carries its bound
   here, and every count is checked against it where the record is read.

   No test group names this file as a source. Three simulation files include it through
   tagpu_packet.h (tagpu_patches.c, tagpu_weapons.c, tagpu_datakeys.c), and none reads a type of
   it: what the wiki draws and takes changes nothing they read, and nothing the recorder carries;
   but a change here runs their test groups (ingame-wiki-plan.md, landing 2). */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_wikistage.h"

#define TAGPU_WIKI_KEY     24u      /* a control's key, NUL-terminated                       */
#define TAGPU_CMD_WIKIHITS 160u     /* the hit map: every control a frame of the wiki draws   */
#define TAGPU_WIKI_FOLDS   32u      /* fold words: a bit for each of the tree's 1024 nodes the
                                       loader keeps (WIKI_MAXNODE)                           */
#define TAGPU_WIKI_NSCROLL 3u       /* the panels that scroll: tree, demos, text (WIKI_SCROLL_*) */

/* What the wiki shows this draw. Present in a packet exactly while the wiki can be drawn: in a
   level, on the Vulkan lane, while the replay HUD's pass stands (tagpu_hudstate_on) and the
   wiki's layout has not refused (tagpu_wikistate_refused).

   TWO LENGTHS. The area is TAGPU_PK_WIKI_HEAD bytes -- everything before `stage` -- while there is
   no stage to draw (the window closed, or no demo), and sizeof(TAGPU_PK_WIKI) with the stage, so
   the 60 KB view is carried only while it is shown. The reader takes `stage` only from an area of
   the whole size (tagpu_pk_wiki_stage). */
typedef struct TAGPU_PK_WIKI {
    uint8_t  open;              /* the window is open: the game takes no input              */
    uint8_t  paused;            /* the wiki holds single player's pause                      */
    int16_t  ptr_x, ptr_y;      /* the pointer's client point, as the last move noted it
                                   (mouse_last_client); -1 for none or outside the picture  */
    /* THE PAGE, as tagpu_wikilay.h WIKIPAGE holds it; every index is bounded against the data
       by the layout, which draws an entry or a demo it does not have as none */
    int16_t  entry;             /* WIKIDATA.entry's index; -1 none                           */
    int16_t  demo;              /* that entry's demo; -1 none                                */
    uint8_t  playing;           /* the demo plays: the play button shows pause               */
    uint8_t  done;              /* the demo has ended and holds its last frame               */
    uint8_t  speed;             /* 1 or 2                                                    */
    uint8_t  try_it;            /* Try it yourself has the stage                             */
    int16_t  mod;               /* MOD_* (tagpu_mod.h): the mod a lock is read for           */
    uint8_t  has_stage;         /* `stage` is carried                                        */
    uint8_t  pad;
    char     held[TAGPU_WIKI_KEY];          /* the control held down; "" none                */
    uint32_t fold[TAGPU_WIKI_FOLDS];        /* bit n: tree node n is folded                  */
    float    scroll[TAGPU_WIKI_NSCROLL];    /* each panel's offset, units, as asked: the layout
                                               holds it to the limit                         */
    char     msg[32];           /* what the stage shows with no view: "no demo", a failure   */
    WIKISTAGEVIEW stage;        /* the demo's view by value: last, carried only with has_stage */
} TAGPU_PK_WIKI;
#define TAGPU_PK_WIKI_HEAD ((uint32_t)offsetof(TAGPU_PK_WIKI, stage))

/* THE WIKI'S HIT MAP: the controls of the last frame the wiki was drawn on, topmost first, in
   the window's client pixels, each key as tagpu_wikilay.c makes it. A LEVEL: a frame that did not
   draw the wiki posts n = 0 and drawn = 0. */
typedef struct TAGPU_CMD_WIKIHIT {
    int16_t  x0, y0, x1, y1;    /* [x0, x1) x [y0, y1)                                       */
    char     key[TAGPU_WIKI_KEY];
} TAGPU_CMD_WIKIHIT;

/* A panel's scrollbar as the frame drew it, in client pixels: the track, the thumb's top and
   length on it, and the scroll a pixel of thumb travel is worth. No bar: h = 0. */
typedef struct TAGPU_CMD_WIKIBAR {
    int16_t  x0, y0, x1, y1;    /* the track: [x0, x1) x [y0, y1)                            */
    int16_t  thumb_y, thumb_h;
    float    per_px;            /* units of scroll a client pixel of thumb travel             */
} TAGPU_CMD_WIKIBAR;

typedef struct TAGPU_CMD_WIKI {
    uint32_t n;                 /* <= TAGPU_CMD_WIKIHITS                                      */
    uint8_t  drawn;             /* the frame this map is drew the wiki layer                  */
    uint8_t  open;              /* that frame drew the window, not the button alone           */
    int16_t  entry;             /* the entry that frame drew, TAGPU_PK_WIKI.entry's value      */
    float    u;                 /* the client pixels a wiki unit was on that frame            */
    float    scroll[TAGPU_WIKI_NSCROLL];     /* each panel's scroll as drawn, units           */
    float    scroll_max[TAGPU_WIKI_NSCROLL]; /* and its limit                                 */
    TAGPU_CMD_WIKIBAR bar[TAGPU_WIKI_NSCROLL];
    TAGPU_CMD_WIKIHIT hit[TAGPU_CMD_WIKIHITS];
} TAGPU_CMD_WIKI;

#endif
