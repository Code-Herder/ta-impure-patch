#ifndef TAGPU_WIKISTATE_H
#define TAGPU_WIKISTATE_H
/* tagpu_wikistate -- the in-game wiki's render-thread half: the packet's TAGPU_PK_WIKI, the frame's
   viewport and the engine's frame at HUD scale, laid out by tagpu_wikilay.c (the button, the
   window and the page) and tagpu_wikistage.c (the stage, from the record's copy of the demo's
   view) for tagpu_vk_hud.c to draw in the same frame; and the drawn frame's controls, scrolls and
   scrollbars handed back in the command record.

   RENDER THREAD, every entry point but tagpu_wikistate_refused. It reads the packet's copy, the
   published art (tagpu_hudart.h), the HUD scale's published word (tagpu_hud_live) and the same
   frame's replay HUD state (tagpu_hudstate_frame), and names no engine memory. */
#include "tagpu_huddraw.h"
#include "tagpu_packet.h"

struct TAGPU_FRAME;

/* Once a frame, from the overlay frame, after tagpu_hudstate_present: the wiki for this frame, or
   none (no record, its art not loaded, its layout refused). */
void tagpu_wikistate_present(const struct TAGPU_FRAME* f);

/* Render-thread frame `frame`'s wiki: 1 with its three op lists -- the page, then the stage's ops
   under its 3D image, then those over it (tagpu_wikistage_keep; an empty `over` and every op of
   the stage under when the frame draws the whitebox) -- the stage lying inside the page's bezel
   where no later op of the page reaches, so drawing it after the whole page paints as the mockup
   does; empty stage lists while the window is closed -- the device pixels a wiki unit is and the
   game screen's top-left corner on the attachment; 0 when there is nothing to draw, or when
   `present` did not run in that frame. The lists point into this module's buffers, which hold
   until the next `present`. */
int  tagpu_wikistate_frame(unsigned frame, HUDOPLIST* page, HUDOPLIST* stage, HUDOPLIST* over, float* px,
                           int* ox, int* oy);

/* THE STAGE THIS FRAME DRAWS, for the 3D stage's producer (tagpu_wikiworld.h): 1 when frame
   `frame` draws a demo's view -- the record's copy, bounded (`*v`, which holds until the next
   `present`), the entry's and the demo's indices, the record's shot number, the stage's rect in
   units, the device pixels a unit is and the screen's corner on the attachment; 0 otherwise. */
int  tagpu_wikistate_scene(unsigned frame, const WIKISTAGEVIEW** v, int* entry, int* demo, uint32_t* shot,
                           HUDRECT* stage, float* px, int* ox, int* oy);

/* WHICH PICTURE OF THE STAGE frame `frame` DRAWS, decided once by the seam before the HUD's pass
   builds it: `image` 1 when the stage's 3D image is composited whole this frame -- its ops are
   then split under and over it by tagpu_wikistage_keep, with `ground` 1 (the image carries the
   ground) and `resolved` the unit types the image draws (NULL: none) -- and 0 for the whitebox,
   every op under. Once a frame; a second call for the same frame changes nothing. */
void tagpu_wikistate_image(unsigned frame, int image, int ground, const uint32_t* resolved);

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

/* ANY THREAD: 1 once the layout has refused, for the rest of the session -- no frame draws the
   wiki again, so tagpu_wiki_fill closes the window and the button is gone. The kit's icons and
   style are checked at the load (tagpu_wikilay_check), and the build refuses nothing else but a
   missing argument or its own memory, so that memory is what is left to refuse. */
int  tagpu_wikistate_refused(void);

/* The command record's wiki part: what the last frame drawn answered. */
void tagpu_wikistate_cmd(TAGPU_CMD_WIKI* out);

#endif
