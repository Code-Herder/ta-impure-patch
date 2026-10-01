#ifndef TAGPU_REPLAYS_H
#define TAGPU_REPLAYS_H
/* tagpu_replays -- the shell's Replays browser and the main menu's REPLAYS button, the demo
   recorder's front door. Design: research/notes/tadr-port/demo-recorder-s5-ui.md; the engine
   facts are in tagpu_replays.c, beside the code that rests on them.

   THE SHAPE. REPLAYS.GUI is a real TA screen in impure-patch.ufo, pushed from the main menu the
   way the engine pushes its own Load Game screen, over a 640x480 ground of our own art. The main
   menu gains one button, copied from its SINGLE button in the parsed screen before the engine
   builds it, and a plate pasted into its freshly decoded backdrop; what else moves is the
   per-mod table's. Game thread only, all of it: every entry point is the engine calling us from
   its shell code.

   ARMING. tagpu_menu_init asks for the files while it builds the archive, and installs the hooks
   once the archive is written and its GUI redirect is in: a REPLAYS.GUI the engine cannot read
   faults GUI_Load, so the button is offered only when the screen it opens can load. */

#include "tagpu_ufo.h"

/* The archive's entries for the screen: REPLAYS.GUI and one ground per style that has art of its
   own. Writes at most `cap` entries into `f` and returns how many (0 when the art is missing, in
   which case nothing of the screen is armed). Their bytes live until tagpu_replays_files_free. */
int  tagpu_replays_files(TAGPU_UFO_FILE* f, int cap);
void tagpu_replays_files_free(void);

/* After the archive write. `ready` = the archive holds the files above and GUI_Load is redirected
   to our folder for the names tagpu_replays_redirects answers. */
void tagpu_replays_install(int ready);

/* Does GUI_Load read `name` out of our folder? Asked by tagpu_menu.c's redirect. */
int  tagpu_replays_redirects(const char* name);

#endif
