#ifndef TAGPU_REPLAYS_H
#define TAGPU_REPLAYS_H
/* tagpu_replays -- the shell's Replays browser and the main menu's REPLAYS button, the demo
   recorder's front door. Design: research/notes/tadr-port/demo-recorder-s5-ui.md; the engine
   facts are in tagpu_replays.c, beside the code that rests on them.

   THE SHAPE. REPLAYS.GUI (REPLAYSE.GUI over Escalation's ground) is a real TA screen in impure-patch.ufo, pushed from the main menu the
   way the engine pushes its own Load Game screen, over a 640x480 ground of our own art. The main
   menu gains one button, copied from its SINGLE button in the parsed screen before the engine
   builds it, and a plate pasted into its freshly decoded backdrop; what else moves is the
   per-mod table's. Game thread only, all of it: every entry point is the engine calling us from
   its shell code.

   ARMING. tagpu_menu_init asks for the files while it builds the archive, and installs the hooks
   once the archive is written and its GUI redirect is in: a REPLAYS.GUI the engine cannot read
   faults GUI_Load, so the button is offered only when the screen it opens can load. */

#include <windows.h>
#include "tagpu_ufo.h"

/* The archive's entries for the screen: one ground per style that has art of its own, and one
   screen per layout those grounds use (REPLAYS.GUI, retail's, and Escalation's REPLAYSE.GUI), five
   at most. Writes at most `cap` entries into `f` and returns how many (0 when the art is missing, in
   which case nothing of the screen is armed). Their bytes live until tagpu_replays_files_free. */
int  tagpu_replays_files(TAGPU_UFO_FILE* f, int cap);
void tagpu_replays_files_free(void);

/* After the archive write. `ready` = the archive holds the files above and GUI_Load is redirected
   to our folder for the names tagpu_replays_redirects answers. */
void tagpu_replays_install(int ready);

/* Does GUI_Load read `name` out of our folder? Asked by tagpu_menu.c's redirect. */
int  tagpu_replays_redirects(const char* name);

/* The replay HUD's bookmarks (tagpu_hudmarks.h), in replays\\index.dat, on the game thread of a
   level that plays a recording. `file` is the name in replays\\; its line holds the bookmarks
   while its key is the file's root key (recr_root_key), which no playing, fork, deletion or
   compaction changes, and a line of another key, another recording's, has none. get copies at
   most `cap`, sorted, and returns how many. add refuses a T outside the recording, within 30
   GameTime of another or past 64, remove refuses a T that is not one; each writes the index
   whole, through a temporary file and a rename, and returns 1 only when that write succeeded. */
int  tagpu_replays_marks_get(const char* file, unsigned* out, int cap);
int  tagpu_replays_mark_add(const char* file, unsigned T);
int  tagpu_replays_mark_remove(const char* file, unsigned T);

/* On a recording-file worker (tagpu_recfile.c, the prune): marks `recording`, a handle with DELETE
   access that no other opener shares, for deletion unless replays\\index.dat gives `file` (its
   name in replays\\) a favourite or a bookmark on its line of the root key read through
   `recording`. The index is read off the disk under the lock its writer takes, so a mark saved
   before this call keeps the recording. 1 marked for deletion (it goes when the handle closes), 0
   kept, -1 nothing decided: the recording's key or the index could not be read, the index is not
   text, is of version 1 and not yet converted or of a version this build does not know, or the
   system has no SetFileInformationByHandle. The line is the browser's for that name (the name
   exactly, else in another case); one whose key is unknown counts by its name. */
int  tagpu_replays_unmarked_delete(const char* file, HANDLE recording);

/* Watch's own check refused the recording `file` of replays\\ with one of WATCH'S DATA REFUSALS
   (tagpu_replay.h), `code`, having compared the running values `a` and `b`: its index line is
   marked under the running archive fingerprint and build key, and the browser shows it locked
   until it opens under another of either (tagpu_replays.c, THE LOCK). On the game thread: at
   Watch's header check with the browser on top, or on the level, at its first step. */
void tagpu_replays_refused(const char* file, unsigned code, unsigned a, unsigned b);

/* FOR THE IN-GAME WIKI'S ASSET LOADER (tagpu_wikiasset.h), at the wiki's first open, on the game
   thread. archive_paths copies the engine's mount list into `out`, at most `max` archives' full
   paths (0x104 bytes each, as the engine keeps them) in mount order, and returns how many; one
   the mount left unterminated is "". asset_names copies the engine's `language` value and the
   asset folders' names as the running executable spells them. */
struct WIKI3DNAMES;
int  tagpu_replays_archive_paths(char (*out)[0x104], int max);
void tagpu_replays_asset_names(struct WIKI3DNAMES* out);

#endif
