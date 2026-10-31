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

/* A FRAME'S WIKI, in paint order (tagpu_vk_hud.c draws them so): the page's base layer, the
   stage's ops under its 3D image, the image, those over it, a frame:game stage's HUD -- its base
   layer, then its top layer, in the replay HUD's own art -- then the frame's TOP range (Leave's
   question, the viewer's cursor), and last the page's top layer: About and the drawer, the only
   HOPF_TOP ops of the page (tagpu_wikilay.c theatre), which lie over the stage and all of it. The
   stage lies inside the page's bezel where no later op of the page's base layer reaches. */
typedef struct {
    HUDOPLIST     page, stage, over, top;   /* the wiki's kit: page, stage under, stage over, TOP */
    const HUDOPS* hud;                      /* the stage HUD, NULL none: the HUD's art (tagpu_hudart_get) */
    float         px;                       /* the wiki's unit and the game screen's corner        */
    int           ox, oy;
    float         hpx;                      /* the stage HUD's unit and its frame's corner          */
    int           hox, hoy;
    int           open;                     /* the window is drawn open                           */
} WIKILISTS;

/* Render-thread frame `frame`'s wiki into `out`: 1 with its lists -- empty stage lists and no HUD
   while the window is closed, an empty `over` and every op of the stage under when the frame draws
   the whitebox (tagpu_wikistage_keep) -- each in device pixels from the attachment's corner; 0 when
   there is nothing to draw, or when `present` did not run in that frame. The lists point into this
   module's buffers, which hold until the next `present`. */
int  tagpu_wikistate_frame(unsigned frame, WIKILISTS* out);

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

/* The Vulkan pass has built this frame's wiki ops and will draw them (`drew` 1): the controls drawn
   become the hit map -- the page's, a frame:game stage HUD's and Leave's question's, topmost first
   (tagpu_wikistate.c, THE HIT ORDER) -- and the page's scrolls and scrollbars and the stage HUD's
   playhead run the command record's; `cursor` 1
   when the stage's cursor is among what it draws -- its op (the list's `watch`) was given
   instances and none of them is hidden. 0: this frame draws no wiki, and the map is empty. After
   tagpu_wikistate_frame answered 1 for the same frame, or with 0 on any frame. */
void tagpu_wikistate_drawn(int drew, int cursor);

/* The cursor goes back over the wiki layer this frame (tagpu_vk_gui_record_cursor): the layer drew,
   and either the window is closed -- the cursor is where the engine's own would be, which the
   button may cover -- or the record's pointer is in the picture and the stage did not draw its
   cursor in its place: in Try it yourself the stage draws the simulation's cursor only while the
   pointer is over it, and tagpu_wikistate_drawn says whether that frame's pass drew it, so the
   screen shows exactly one. While the window is open with no pointer in the picture the UI
   layer's cursor stands at the frozen point, on the wiki button, which is not where the player's
   pointer is, so none is drawn. */
int  tagpu_wikistate_cursor(unsigned frame);

/* ANY THREAD: 1 once the layout has refused, for the rest of the session -- no frame draws the
   wiki again, so tagpu_wiki_fill closes the window and the button is gone. The kit's icons and
   style are checked at the load (tagpu_wikilay_check), and the build refuses nothing else but a
   missing argument or its own memory, so that memory is what is left to refuse. */
int  tagpu_wikistate_refused(void);

/* RENDER THREAD: the wiki cannot be drawn for the rest of the session for a reason outside its
   layout -- the HUD's pass has no room for its kit (tagpu_vk_hud.c). Said once; then
   tagpu_wikistate_refused answers 1 as for the layout's own refusal. */
void tagpu_wikistate_refuse(const char* why);

/* THE GAME'S CURSORS' SPRITE SHEET the stage's HOPF_SPRITE ops draw from (tagpu_hudlay.h):
   HUD_SPRITE_CELLS x HUD_SPRITE_CELL by HUD_SPRITE_CELL texels, R8G8B8A8 premultiplied, and in
   `*gen` its generation, which moves whenever its texels do; 0 while there is none (the stage then
   draws no sprite op). The texels hold until the next `present`. */
const unsigned char* tagpu_wikistate_sprites(unsigned* gen);

/* THE STAGE'S COMPOSED SIDEBAR COLUMN the stage's HOP_SIDE op draws from (tagpu_wikiside.h):
   HUD_SIDE_W x HUD_SIDE_H texels, R8G8B8A8 premultiplied, the column's rows from the top and
   transparent below them, and in `*gen` its generation, which moves whenever its texels do; 0
   while none has been composed (the stage then has no HOP_SIDE op). The texels hold until the
   next `present`: RENDER THREAD ONLY, as the composition is. */
const unsigned char* tagpu_wikistate_side(unsigned* gen);

/* THE FRAME'S BARS the stage's HOP_BAR ops draw from (tagpu_wikibar.h): HUD_BAR_W x HUD_BAR_H texels,
   R8G8B8A8, the top bar's rows then the bottom bar's, and in `*gen` its generation, which moves
   whenever its texels do; 0 while none has been composed (the stage then draws the whitebox
   strips). The texels hold until the next `present`: RENDER THREAD ONLY, as the composition is. */
const unsigned char* tagpu_wikistate_bar(unsigned* gen);

/* The command record's wiki part: what the last frame drawn answered. */
void tagpu_wikistate_cmd(TAGPU_CMD_WIKI* out);

#endif
