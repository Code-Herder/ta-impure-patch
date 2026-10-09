#ifndef TAGPU_HUDMARKS_H
#define TAGPU_HUDMARKS_H
/* tagpu_hudmarks -- the replay HUD's timeline markers: each player's commander deaths and defeat,
   read off the recording by a worker thread with its own reader, and the bookmarks the player
   keeps for a recording in replays\index.dat (tagpu_replays.h); and the stats window's samples,
   which the same worker takes on its way.

   GAME THREAD, every entry point. The worker reads the recording file and nothing of the
   engine's; nothing here names engine memory. */

/* The level's markers and bookmarks, for the HUD's packet fill: up to `cap` of them into
   `T`/`kind`/`seat` -- kind 0 a commander's death, 1 a defeat, 2 a bookmark, whose seat is 0xFF --
   the deaths and defeats as far as the worker has walked the timeline that plays. Starts that
   worker on the first call that finds the playback live, and again, after joining the last, when
   the playback has moved to another timeline. */
int  tagpu_hudmarks_get(unsigned* T, unsigned char* kind, unsigned char* seat, int cap);

/* The stats window's samples of measure `key` (layout.json window.measures `k`) for the recorded
   seats `seat[0 .. nseat)`: seat i's sample k into v[i * cap + k], as many as the walker has
   published, at most `cap`. Returns the count -- 0 for a key it does not sample -- with the
   GameTime between two samples in `period` and the first's in `base`. Call after
   tagpu_hudmarks_get, which starts and stops the walker. */
int  tagpu_hudmarks_stats(const char* key, const unsigned char* seat, int nseat, float* v, int cap,
                          unsigned* period, unsigned* base);

/* The bookmark button: removes the bookmark nearest `now` within `within` GameTime of it, else adds
   one at `now`. 1 added, 0 removed, -1 neither -- the recording was not opened from replays\, the
   list is full, or index.dat refused the write -- with the GameTime it acted at in `at`. */
int  tagpu_hudmarks_bookmark(unsigned now, unsigned within, unsigned* at);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60: the worker is stopped and joined, and the
   level's markers and bookmarks forgotten. */
void tagpu_hudmarks_level_end(void);

#endif
