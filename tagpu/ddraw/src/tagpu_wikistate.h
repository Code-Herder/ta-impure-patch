#ifndef TAGPU_WIKISTATE_H
#define TAGPU_WIKISTATE_H
/* tagpu_wikistate -- the in-game wiki's render-thread half: the packet's TAGPU_PK_WIKI, the frame's
   viewport and the engine's frame at HUD scale, laid out by tagpu_wikiframe.c for tagpu_vk_hud.c
   to draw in the same frame; and the drawn frame's controls handed back in the command record.

   RENDER THREAD, every entry point. It reads the packet's copy, the HUD scale's published word
   (tagpu_hud_live) and the same frame's replay HUD state (tagpu_hudstate_frame), and names no
   engine memory. */
#include "tagpu_hudlay.h"
#include "tagpu_packet.h"

struct TAGPU_FRAME;

/* Once a frame, from the overlay frame, after tagpu_hudstate_present: the wiki for this frame, or
   none (no record, its art not loaded, its layout refused). */
void tagpu_wikistate_present(const struct TAGPU_FRAME* f);

/* Render-thread frame `frame`'s wiki: 1 with its ops, the device pixels a wiki unit is and the game
   screen's top-left corner on the attachment; 0 when there is nothing to draw, or when `present`
   did not run in that frame. */
int  tagpu_wikistate_frame(unsigned frame, const HUDOPS** ops, float* px, int* ox, int* oy);

/* The Vulkan pass has built this frame's wiki ops and will draw them: their controls become the hit
   map. NULL: this frame draws no wiki, and the map is empty. After tagpu_wikistate_frame answered 1
   for the same frame, or with NULL on any frame. */
void tagpu_wikistate_drawn(const HUDOPS* ops);

/* The cursor goes back over the wiki layer this frame (tagpu_vk_gui_record_cursor): the layer drew,
   and either the window is closed -- the cursor is where the engine's own would be, which the
   button may cover -- or the record's pointer is in the picture. While the window is open with no
   pointer in the picture the UI layer's cursor stands at the frozen point, on the wiki button,
   which is not where the player's pointer is, so none is drawn. */
int  tagpu_wikistate_cursor(unsigned frame);

/* The command record's wiki part: the hit map of the last frame drawn. */
void tagpu_wikistate_cmd(TAGPU_CMD_WIKI* out);

#endif
