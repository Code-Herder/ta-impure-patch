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
#include "tagpu_wikiframe.h"

#define TAGPU_WIKI_KEY     24u      /* a control's key, NUL-terminated                       */
/* THE HIT MAP: every control a frame of the wiki draws -- the page's WIKI_MAXHOT, a frame:game
   stage's HUD's HUD_MAXHOT and Leave's question's four (tagpu_wikistate.c holds the two bounds to
   these numbers at build time) */
#define TAGPU_CMD_WIKIHITS (160u + 128u + 4u)   /* TAGPU_CMD_WIKI is 10056 bytes (sizeof, the DLL's
                                                   compiler): tagpu_packet.c's assert holds the
                                                   command record to its reserve                  */
#define TAGPU_WIKI_FOLDS   32u      /* fold words: a bit for each of the tree's 1024 nodes the
                                       loader keeps (WIKI_MAXNODE)                           */
#define TAGPU_WIKI_NSCROLL 3u       /* the panels that scroll: tree, demos, text (WIKI_SCROLL_*) */
#define TAGPU_WIKI_MSG     128u     /* the stage's message: a simulation's refusal fits it       */

/* What the wiki shows this draw. Present in a packet exactly while the wiki can be drawn: in a
   level, on the Vulkan lane, while the replay HUD's pass stands (tagpu_hudstate_on) and the
   wiki's layout has not refused (tagpu_wikistate_refused).

   TWO LENGTHS. The area is TAGPU_PK_WIKI_HEAD bytes -- everything before `stage` -- while there is
   no stage to draw (the window closed, or no demo), and sizeof(TAGPU_PK_WIKI) with the stage, so
   the 158 KB view (80 KB of it the pose pool, 16 KB the nano spray's particles; MEASURED as
   sizeof on the DLL's compiler) and a frame:game entry's frame record after it (WIKIFRAMEREC,
   1680 bytes; the whole area 167704, the head 328, MEASURED as sizeof on the DLL's compiler) are
   carried only while they are shown. The reader takes
   `stage` and `frame` only from an area of the whole size (tagpu_pk_wiki_stage,
   tagpu_pk_wiki_frame). */
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
    uint8_t  config;            /* the Config sprocket the top bar shows from this layer: 0 none
                                   (the engine drew its own), 1 up, 2 down (tagpu_menu_bar_cog).
                                   Data: the render thread draws none for any other value     */
    uint8_t  about, drawer;     /* the theatre's About panel and its Entries drawer are open; at
                                   most one, and data: the render thread takes each as 0 or 1  */
    uint8_t  leave_held;        /* Try it on a replay: the answer of Leave's question held down,
                                   0 none, 1 Yes, 2 No; data: the render thread draws any other
                                   value as none                                             */
    /* THE CONTEXT THE TEXT IS FILLED FOR, by value (tagpu_wiki.c, *the context*): the render
       thread fills the page's titles, subtitle, paragraphs, demo titles and tree leaves with the
       words of each entry under it (tagpu_wikilay.h WIKIPAGE) -- the same pair the running demo
       plays and its caption, filled on the game thread, was filled with */
    uint8_t  iface;             /* the Interface Type, 0 or 1                                */
    int8_t   cast_side;         /* the layout side of the published WIKISIDE whose casting
                                   names the units; -1 none: the entry's own names. Data: the
                                   render thread bounds it against WIKI_SIDE_SIDES and the
                                   layout's sides, and the game thread names a side only after
                                   it read WIKISIDE_READY, which is final                    */
    char     held[TAGPU_WIKI_KEY];          /* the control held down; "" none                */
    uint8_t  try_world;         /* Try it on a live frame: the pointer (ptr_x, ptr_y) lies on
                                   the frame's world as the game thread mapped it, through the
                                   hit map it read (tagpu_wiki.c stage_world). The one fact both
                                   halves of THE ONE CURSOR read: the view keeps the armed
                                   building's ghost and the stage draws the simulation's cursor
                                   at the pointer only with it; data, taken as 0 or 1          */
    uint32_t fold[TAGPU_WIKI_FOLDS];        /* bit n: tree node n is folded                  */
    float    scroll[TAGPU_WIKI_NSCROLL];    /* each panel's offset, units, as asked: the layout
                                               holds it to the limit                         */
    char     msg[TAGPU_WIKI_MSG];   /* what the stage shows with no view: "no demo",
                                   "loading…", why the demo did not start                  */
    uint32_t shot;              /* the last capture the trigger tagpu_wiki.shot asked for, 0
                                   none: the first frame of this view whose 3D image is drawn
                                   writes it (tagpu_wikiworld.h)                             */
    uint32_t rep_serial;        /* the game thread's count of replays made and freed: the
                                   replay this packet shows, which the hit map drawn from it
                                   names (TAGPU_CMD_WIKI.rep_serial)                         */
    WIKISTAGEVIEW stage;        /* the demo's view by value: carried only with has_stage      */
    WIKIFRAMEREC  frame;        /* a frame:game entry's frame record (tagpu_wikiframe.h), `on` 0
                                   for any other entry; DATA, bounded by the render thread's
                                   tagpu_wikiframe_bound on its own copy before any read     */
} TAGPU_PK_WIKI;
#define TAGPU_PK_WIKI_HEAD ((uint32_t)offsetof(TAGPU_PK_WIKI, stage))

/* THE WIKI'S HIT MAP: the controls of the last frame the wiki was drawn on, topmost first, in
   the window's client pixels, each key as the module that drew it makes it -- tagpu_wikilay.c the
   page's, tagpu_hudlay.c a frame:game stage's HUD's, tagpu_wikiframe.c Leave's question's -- and
   `src` (WIKIHIT_*, tagpu_wikiframe.h) the one. The same key can be two controls (the page's
   `play` and the stage HUD's), so a key is read only with its source. A LEVEL: a frame that did not
   draw the wiki posts n = 0 and drawn = 0. */
typedef struct TAGPU_CMD_WIKIHIT {
    int16_t  x0, y0, x1, y1;    /* [x0, x1) x [y0, y1)                                       */
    char     key[TAGPU_WIKI_KEY];
    uint8_t  src;               /* WIKIHIT_PAGE, _HUD or _DIALOG; data: anything else is the
                                   page's                                                    */
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
    uint8_t  view;              /* that frame's stage drew a view, not a message               */
    int16_t  side_h;            /* that view's sidebar rows, 0 without a sidebar: the stage's
                                   fit is the drawn frame's, whatever the simulation runs now */
    uint8_t  frame;             /* that view was a whole game frame (WIKISTAGEVIEW.frame): the
                                   stage holds no world point a click could name            */
    int16_t  hud_x0, hud_x1;    /* the stage HUD's playhead run as drawn, client pixels; equal:
                                   no track was drawn                                        */
    int32_t  hud_end;           /* the game tick the run's end stood for (the record's end)   */
    int32_t  hud_first, hud_G;  /* a live strip's drawn span, game ticks: the run stood for
                                   [first, max(G, first)] (the record's); 0, 0 on any other
                                   frame. DATA: it turns a pixel into a tick only, and the
                                   replay clamps the tick to its own span (tagpu_wiki.c
                                   track_tick)                                               */
    uint32_t rep_serial;        /* TAGPU_PK_WIKI.rep_serial of the packet that frame drew: the
                                   replay its stage HUD, its run and its end are of; data,
                                   compared for equality only                                */
    uint8_t  nlane;             /* the lanes that frame's stage HUD drew, their keys' lane:<k>
                                   order: tagpu_wikiframe_lanes of the bounded record the HUD
                                   was built from (tagpu_hudstate.c fills the game's lane_id
                                   the same way); 0 on any other frame                       */
    int32_t  lane_id[WIKIFRAME_FORKS];  /* the fork each lane showed. DATA: a lane's control
                                   takes a place only below nlane, and the game thread acts on
                                   an id only while it names a live fork of the replay
                                   rep_serial names (tagpu_wiki.c drawn_lanes)               */
    float    u;                 /* the client pixels a wiki unit was on that frame            */
    float    scroll[TAGPU_WIKI_NSCROLL];     /* each panel's scroll as drawn, units           */
    float    scroll_max[TAGPU_WIKI_NSCROLL]; /* and its limit                                 */
    TAGPU_CMD_WIKIBAR bar[TAGPU_WIKI_NSCROLL];
    TAGPU_CMD_WIKIHIT hit[TAGPU_CMD_WIKIHITS];
} TAGPU_CMD_WIKI;

#endif
