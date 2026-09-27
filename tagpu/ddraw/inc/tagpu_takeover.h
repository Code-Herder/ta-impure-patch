#ifndef TAGPU_TAKEOVER_H
#define TAGPU_TAKEOVER_H
/* tagpu_takeover -- keeping TADR's code out of the process on the routes where Impure starts
   first (research/notes/compat/takeover.md, part 1).

   On the Community Patch Loader route the retail exe imports DDRAW (Impure) before DPLAYX
   (the loader), so Impure's DllMain has pointed the loader's LoadLibrary imports at the fork's
   fake_LoadLibrary* (hook_init, hook=3) before the loader's own DllMain asks for
   "tdraw.dll". TADR installs everything it patches in its DllMain, so answering that request
   with Impure keeps all of it out; the loader's GetProcAddress then finds Impure's
   DirectDrawCreate, and its patch_call points the exe's two calls (0x47BFA2, 0x4B55FB) at it.

   WHAT IS ANSWERED FOR: a DLL file in the game folder -- the folder of TotalA.exe -- whose
   export table names DirectDrawCreate, that is not Impure's own file and is not already in
   the process. The test is the file's exports, never its name: the mods rename TADR (mdraw,
   zdraw, TAESC). A system DLL is never answered for, nor a load with LOAD_LIBRARY_AS_DATAFILE
   or AS_IMAGE_RESOURCE (the caller reads it, it does not run it).

   `tagpu_takeover.off` in the game folder turns it off: the suite's harness setup that shows
   the safety net (tagpu_patches.c, lim_verify) refusing what this would have kept out.

   RETURNS Impure's module with one more reference (as LoadLibrary would), or NULL when the
   request is not ours to answer -- the caller then loads normally. Any thread; no state. */

#include <windows.h>

/* declared only for _WIN32_WINNT >= 0x0600; the values are the loader's own */
#ifndef LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
#define LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE 0x00000040
#endif
#ifndef LOAD_LIBRARY_AS_IMAGE_RESOURCE
#define LOAD_LIBRARY_AS_IMAGE_RESOURCE 0x00000020
#endif

HMODULE tagpu_takeover_loadlibrary_a(const char* name, void* caller);
HMODULE tagpu_takeover_loadlibrary_w(const wchar_t* name, void* caller);

#endif
