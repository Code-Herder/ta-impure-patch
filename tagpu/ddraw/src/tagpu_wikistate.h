#ifndef TAGPU_WIKISTATE_H
#define TAGPU_WIKISTATE_H
/* tagpu_wikistate -- the in-game wiki's render-thread half: the packet's TAGPU_PK_WIKI, the frame's
   viewport and the engine's frame at HUD scale, laid out by tagpu_wikilay.c (the button, the
   window and the page) and tagpu_wikistage.c (the stage, from the record's copy of the demo's
   view) for tagpu_vk_hud.c to draw in the same frame; and the drawn frame's controls, scrolls and
   scrollbars handed back in the command record.

   RENDER THREAD, every entry point. It reads the packet's copy, the published art (tagpu_hudart.h),
   the HUD scale's published word (tagpu_hud_live) and the same frame's replay HUD state
   (tagpu_hudstate_frame), and names no engine memory. */
#include "tagpu_huddraw.h"
#include "tagpu_packet.h"

struct TAGPU_FRAME;

/* Once a frame, from the overlay frame, after tagpu_hudstate_present: the wiki for this frame, or
   none (no record, its art not loaded, its layout refused). */
void tagpu_wikistate_present(const struct TAGPU_FRAME* f);

/* Render-thread frame `frame`'s wiki: 1 with its two op lists -- the page, then the stage, which
   lies inside the page's bezel where no later op of the page reaches, so drawing it after the
   whole page paints as the mockup does; an empty stage list while the window is closed -- the
   device pixels a wiki unit is and the game screen's top-left corner on the attachment; 0 when
   there is nothing to draw, or when `present` did not run in that frame. The lists point into
   this module's buffers, which hold until the next `present`. */
int  tagpu_wikistate_frame(unsigned frame, HUDOPLIST* page, HUDOPLIST* stage, float* px, int* ox, int* oy);

/* The Vulkan pass has built this frame's wiki ops and will draw them (`drew` 1): the page's
   controls become the hit map, and its scrolls and scrollbars the command record's. 0: this frame
   draws no wiki, and the map is empty. After tagpu_wikistate_frame answered 1 for the same frame,
   or with 0 on any frame. */
void tagpu_wikistate_drawn(int drew);

/* The cursor goes back over the wiki layer this frame (tagpu_vk_gui_record_cursor): the layer drew,
   and either the window is closed -- the cursor is where the engine's own would be, which the
   button may cover -- or the record's pointer is in the picture. While the window is open with no
   pointer in the picture the UI layer's cursor stands at the frozen point, on the wiki button,
   which is not where the player's pointer is, so none is drawn. */
int  tagpu_wikistate_cursor(unsigned frame);

/* The command record's wiki part: what the last frame drawn answered. */
void tagpu_wikistate_cmd(TAGPU_CMD_WIKI* out);

#endif
