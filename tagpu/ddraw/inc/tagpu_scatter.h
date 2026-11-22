#ifndef TAGPU_SCATTER_H
#define TAGPU_SCATTER_H
/* tagpu_scatter -- clearing a build site: a builder's idle units standing on the site it has come to build on are
   moved off it (research/notes/build-scatter-plan.md; research/notes/modules/build-scatter.md). */

/* At DllMain: MobileBuild's blocked-site branch 0x403CD0, its VTOL twin 0x41400D and the yard test's occupant
   compare 0x47D554, each byte-matched. Without the first nothing is installed. */
void tagpu_scatter_init(void);
/* The level init, on the LOADER thread at the PLAYERS call (tagpu_replay.c): every row cleared, the level's
   widest yard read. */
void tagpu_scatter_level_init(void);
/* Once a simulation step, on the GAME thread in the players phase (tagpu_allysight.c, players_phase), after the
   step's unit tick: no order handler is on the stack. Checks every row, plans the sites refused in this step and
   orders their blockers off. */
void tagpu_scatter_tick(void);

/* THE CURSOR'S SITE TEST: stock's 0x47D2E0 (def, cell, ignore, player record), and, when it refuses a site that
   only units build scatter would move for that player stand in the way of, yes, with *moves 1. Every other
   refusal is stock's. GAME THREAD. */
unsigned tagpu_scatter_site_test(const void* def, unsigned cell, unsigned ignore, void* player, int* moves);
/* The build cursor's own last answer, for the frame packet's square: set by the cursor's call alone
   (tagpu_patches.c, build_site_test), read by the publisher on the same thread. */
void tagpu_scatter_cursor_set(int moves);
int  tagpu_scatter_cursor_moves(void);

/* THE SITES, for the factory guard's planner (tagpu_guard.c): a site one of `owner`'s builders has been refused
   on within the last SITE_TTL steps of `now` is ground a structure is about to stand on. _covers: the footprint
   fx x fz at cell (x, z) overlaps one. _mark: every such site's cells in the w x h box at (x0, z0) set to 1 in
   `t`, and with them every place one of `owner`'s units has been sent to and holds the move for still (a row). GAME THREAD; they read this file's rows and nothing of the engine. */
int  tagpu_scatter_covers(unsigned owner, int now, int x, int z, int fx, int fz);
void tagpu_scatter_mark(unsigned owner, int now, int x0, int z0, int w, int h, unsigned char* t);

#endif
