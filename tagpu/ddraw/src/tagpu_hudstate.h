#ifndef TAGPU_HUDSTATE_H
#define TAGPU_HUDSTATE_H
/* tagpu_hudstate -- the HUD's state, from the game thread to the render thread
   (research/notes/hud-in-game.md): a replay's controls, or a live skirmish's rewind. Two halves,
   one per thread, and the frame packet between them:

     GAME THREAD     tagpu_hudstate_fill: the replay's clock, its players, the timeline's
                     markers and the HUD's own state -- or, live, the rewind's state -- taken
                     through the replay's calls, the markers' walker and the HUD's input into the
                     packet's TAGPU_PK_HUD (tagpu_packet.h), by the packet's publisher.
     RENDER THREAD   tagpu_hudstate_present: that record, the frame's viewport and the engine's
                     frame at HUD scale, as the HUDSTATE tagpu_hudlay.c lays out, for
                     tagpu_vk_hud.c to draw in the same frame; and the drawn frame's controls,
                     handed back in the command record (tagpu_hudstate_cmd). */
#include "tagpu_hudlay.h"
#include "tagpu_packet.h"

struct TAGPU_FRAME;

/* The HUD draws: on the Vulkan lane, until the pass refuses (`tagpu_hudstate_refuse`). On GDI a
   replay has no on-screen controls and a live game no rewind button. Game thread. */
int  tagpu_hudstate_on(void);

/* The HUD cannot draw for the rest of the session -- its art did not load, its pass could not
   be built or the device failed it, its layout did not build: `tagpu_hudstate_on` answers 0 from
   now on. Render thread; published with a full barrier. */
void tagpu_hudstate_refuse(const char* why);

/* What the HUD shows for the level in play, the fill's own test, while it is on: the live
   rewind's controls while a rewind owns the level or one could open (a live, recorded,
   single-player skirmish), else the replay's while a recording plays. A rewind into the past plays
   the recording as puppets (tagpu_replay_live is true there), so the rewind is asked first. Game
   thread. */
enum { HUD_MODE_NONE, HUD_MODE_REPLAY, HUD_MODE_LIVE };
int  tagpu_hudstate_mode(void);

/* The packet's HUD record for this draw, GameTime `now`: 1 when the HUD shows something
   (tagpu_hudstate_mode), its `mode` saying which, 0 (and `h` untouched) otherwise. Game thread,
   from the packet's fill. */
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

/* Render-thread frame `frame`'s Config panel on the attachment, while the live HUD draws and the
   panel is open: 1 with its rect, 0 otherwise. Render thread. */
int  tagpu_hudstate_panel(unsigned frame, int* x, int* y, int* w, int* h);

/* The Vulkan pass has built this frame's ops and will draw them: their controls become the hit
   map. NULL: this frame draws no HUD, and the map is empty. Render thread, after
   `tagpu_hudstate_frame` answered 1 for the same frame. */
void tagpu_hudstate_drawn(const HUDOPS* ops);

/* The command record's HUD part: the hit map of the last frame drawn. Render thread. */
void tagpu_hudstate_cmd(TAGPU_CMD_HUD* out);

#endif
