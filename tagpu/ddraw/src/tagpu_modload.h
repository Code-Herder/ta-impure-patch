#ifndef TAGPU_MODLOAD_H
#define TAGPU_MODLOAD_H
/* tagpu_modload -- a mod chosen by `mod=` in impure.cfg, run on the player's own exe from mods\<id>\
   (research/notes/mod-manager-plan.md). With no mod= none of this does anything.

   THE ORDER IS THE LOAD ORDER, AND THE RESULT IS THE DROP-IN FOLDER'S. A mod whose exe differs on disk
   has its bytes in place when Impure patches, and a Patch Loader writes its list after Impure's
   DllMain, so:

   - tagpu_modload_begin, from DllMain before Impure's first patch: identifies the exe on disk and any
     loader beside it, builds stock + the mod's XOR from that file, checks the image hash, and writes
     the image the mod's kind sees at this point: the target for an exe mod, stock for a loader mod.
   - tagpu_modload_end, last in DllMain: a loader mod's bytes, this module's detours, a snapshot of every
     section, and a jump on the exe's entry point.
   - the entry point, after every DllMain: whatever another DLL wrote since is put back from the
     snapshot, import slots included, then the jump is gone and the exe starts. That is what makes a
     folder's Patch Loader, a mod's win32.dll or a hook on an import slot harmless whatever they write:
     nothing is listed, and displacements into their DLLs need no table.

   ANYTHING THAT DOES NOT FIT refuses at the entry point, outside the loader lock and before any game
   code runs: a box naming the reason, mod= cleared so the next start is the folder's own, and exit. */

void tagpu_modload_begin(void);
void tagpu_modload_end(void);
/* First DirectDraw call: refuses if the entry stage never ran, which another DLL's jump on the entry point would cause. */
void tagpu_modload_check(void);

/* The exe file as the mod's drop-in folder would hold it, for what compares the image with its file (the limit table, the
   takeover's check): a mod's table in it for an exe mod, stock for a loader mod. NULL with no mod. Lives for the process. */
const unsigned char* tagpu_modload_reference_file(size_t* n);

/* The running mod's id, NULL with none. Written once at attach. */
const char* tagpu_modload_id(void);
/* Whether a managed mod closes this path's loose step: the folders the engine reads game data from. Under
   no mod, never. */
int tagpu_modload_loose_closed(const char* path);

/* 1 when this process is the game: an exe named TotalA.exe, or one holding the engine's sites in its file. Any other exe that
   loads a ddraw.dll from this folder is a tool, and mods\\ is none of its business. Reads the exe unless the name answers. */
int tagpu_modload_is_game(void);

#endif
