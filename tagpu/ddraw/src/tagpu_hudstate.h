#ifndef TAGPU_HUDSTATE_H
#define TAGPU_HUDSTATE_H
/* tagpu_hudstate -- the replay HUD's state, from the game thread to the render thread
   (research/notes/hud-in-game.md). Two halves, one per thread, and the frame packet between them:

     GAME THREAD     tagpu_hudstate_fill: the replay's clock, its players and the timeline's
                     markers, taken through the replay's and the dock's own calls into the
                     packet's TAGPU_PK_HUD (tagpu_packet.h), by the packet's publisher.
     RENDER THREAD   tagpu_hudstate_present: that record, the frame's viewport and the engine's
                     frame at HUD scale, as the HUDSTATE tagpu_hudlay.c lays out, for
                     tagpu_vk_hud.c to draw in the same frame. */
#include "tagpu_hudlay.h"
#include "tagpu_packet.h"

struct TAGPU_FRAME;

/* `tagpu_replayhud.on`: the replay HUD is drawn in place of the watching dock and the player
   cards. Read once, the first time anything asks; `scale=N` in the file (75, 100, 125, 150) is
   the HUD's scale, ahead of the settings store's, as tagpu_hud.on carries the game's. Game
   thread. */
int  tagpu_hudstate_on(void);

/* The packet's HUD record for this draw, GameTime `now`: 1 when a recording plays and the HUD is
   on, 0 (and `h` untouched) otherwise. Game thread, from the packet's fill. */
int  tagpu_hudstate_fill(TAGPU_PK_HUD* h, unsigned now);

/* Once a frame, from the overlay frame, after the palette's: the HUD for this frame, or none. */
void tagpu_hudstate_present(const struct TAGPU_FRAME* f);

/* This frame's HUD: 1 with the state, the device pixels a HUD unit is, and the game screen's
   top-left corner on the attachment; 0 when there is nothing to draw. Render thread, after
   `present`, in the same frame. */
int  tagpu_hudstate_frame(const HUDSTATE** st, float* px, int* ox, int* oy);

#endif
