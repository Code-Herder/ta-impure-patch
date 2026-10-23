#ifndef TAGPU_HUDSTATE_H
#define TAGPU_HUDSTATE_H
/* tagpu_hudstate -- the replay HUD's state, from the game thread to the render thread
   (research/notes/hud-in-game.md). Two halves, one per thread, and the frame packet between them:

     GAME THREAD     tagpu_hudstate_fill: the replay's clock, its players, the timeline's
                     markers and the HUD's own state, taken through the replay's calls, the
                     markers' walker and the HUD's input into the packet's TAGPU_PK_HUD
                     (tagpu_packet.h), by the packet's publisher.
     RENDER THREAD   tagpu_hudstate_present: that record, the frame's viewport and the engine's
                     frame at HUD scale, as the HUDSTATE tagpu_hudlay.c lays out, for
                     tagpu_vk_hud.c to draw in the same frame; and the drawn frame's controls,
                     handed back in the command record (tagpu_hudstate_cmd). */
#include "tagpu_hudlay.h"
#include "tagpu_packet.h"

struct TAGPU_FRAME;

/* The replay HUD draws: on the Vulkan lane, until the pass refuses (`tagpu_hudstate_refuse`).
   On GDI a replay has no on-screen controls. Game thread. */
int  tagpu_hudstate_on(void);

/* The HUD cannot draw for the rest of the session -- its art did not load, its pass could not
   be built or the device failed it, its layout did not build: `tagpu_hudstate_on` answers 0 from
   now on. Render thread; published with a full barrier. */
void tagpu_hudstate_refuse(const char* why);

/* The HUD draws for the level in play: it is on and a recording plays, the fill's own test. Game
   thread. */
int  tagpu_hudstate_drawing(void);

/* The packet's HUD record for this draw, GameTime `now`: 1 when a recording plays and the HUD is
   on, 0 (and `h` untouched) otherwise. Game thread, from the packet's fill. */
int  tagpu_hudstate_fill(TAGPU_PK_HUD* h, unsigned now);

/* The packet's stats area for the record `h` just filled: 1 while its window is open, with the
   samples of its measure for its seats, 0 (and `s` untouched) otherwise. Game thread, from the
   packet's fill. */
int  tagpu_hudstate_fill_stats(TAGPU_PK_HUDSTAT* s, const TAGPU_PK_HUD* h);

/* Once a frame, from the overlay frame, after the palette's: the HUD for this frame, or none. */
void tagpu_hudstate_present(const struct TAGPU_FRAME* f);

/* Render-thread frame `frame`'s HUD: 1 with the state, the device pixels a HUD unit is, and the
   game screen's top-left corner on the attachment; 0 when there is nothing to draw, or when
   `present` did not run in that frame. Render thread. */
int  tagpu_hudstate_frame(unsigned frame, const HUDSTATE** st, float* px, int* ox, int* oy);

/* The Vulkan pass has built this frame's ops and will draw them: their controls become the hit
   map. NULL: this frame draws no HUD, and the map is empty. Render thread, after
   `tagpu_hudstate_frame` answered 1 for the same frame. */
void tagpu_hudstate_drawn(const HUDOPS* ops);

/* A recording's name -- the engine's 8-bit text, at most `n` bytes -- as the UTF-8 the HUD's faces
   draw, into `dst` of `cap` bytes (tagpu_hudstate.c, THE NAMES IN WHAT THE FACES CARRY). Pure: either
   thread. */
void tagpu_hudstate_utf8(char* dst, size_t cap, const char* src, size_t n);

/* The command record's HUD part: the hit map of the last frame drawn. Render thread. */
void tagpu_hudstate_cmd(TAGPU_CMD_HUD* out);

#endif
