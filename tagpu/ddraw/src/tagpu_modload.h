#ifndef TAGPU_MODLOAD_H
#define TAGPU_MODLOAD_H
#include <stddef.h>
#include <stdint.h>
/* tagpu_modload -- a mod chosen by `mod=` in impure.cfg, run on the player's own exe from mods\<id>\
   (research/notes/mod-manager-plan.md). With no mod= none of this does anything, unless the player has switched on a mutator of the
   folder's own game: then only those mount.

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
   code runs: a box naming the reason, mod= cleared so the next start is the folder's own, and exit. The one
   exception keeps mod=: a mod missing while another copy of the game holds the install lock. */

void tagpu_modload_begin(void);
void tagpu_modload_end(void);
/* First DirectDraw call: refuses if the entry stage never ran, which another DLL's jump on the entry point would cause. */
void tagpu_modload_check(void);

/* The exe file as the mod's drop-in folder would hold it, for what compares the image with its file (the limit table, the
   takeover's check): a mod's table in it for an exe mod, stock for a loader mod. NULL with no mod. Lives for the process. */
const unsigned char* tagpu_modload_reference_file(size_t* n);

/* GAME THREAD. What the exe's half of the fingerprint names: the image a catalog mod's table makes when this is that mod, managed or its drop-in
   folder, with its id in `id`, and otherwise a hash of the exe and any Patch Loader beside it, with `id` empty. */
void tagpu_modload_exe_identity(uint8_t out[32], char* id, size_t nid);

/* The running mod's id, NULL with none. Written once at attach. */
const char* tagpu_modload_id(void);
/* Whether a managed mod closes this path's loose step: the folders the engine reads game data from. Under
   no mod, never. */
int tagpu_modload_loose_closed(const char* path);
/* The mutators this start mounted, "name version, name version", "" none: settled by the first mount, at WinMain's start-up. */
const char* tagpu_modload_mutators(void);
/* `fn` with each file a mutator took out of a mounted archive of that file name: what the engine's directory no longer holds. Settled likewise. */
void tagpu_modload_each_gone(void (*fn)(void* ctx, const char* archive, const char* path), void* ctx);

/* For the Mods screen. The name of the archive no mod runs without. Whether this folder's exe is one the pack knows: mods
   run on it. The first call is the game thread's and loads the pack when no mod did. */
const char* tagpu_modload_essential_archive(void);
int tagpu_modload_exe_known(void);
/* Whether the file is an original archive of the game, by its hash: 1, 0, -1 unreadable. Off the game thread, and only after
   tagpu_modload_exe_known has returned, which orders the pack before it. */
int tagpu_modload_archive_is_original(const char* path);

/* 1 when this process is the game: an exe named TotalA.exe, or one holding the engine's sites in its file. Any other exe that
   loads a ddraw.dll from this folder is a tool, and mods\ is none of its business. Reads the exe unless the name answers. */
int tagpu_modload_is_game(void);

#endif
