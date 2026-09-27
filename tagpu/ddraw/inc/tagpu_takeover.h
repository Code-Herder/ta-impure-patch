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
   the process, asked for by a plain load. The test is the file's exports, never its name: the
   mods rename TADR (mdraw, zdraw, TAESC). A plain load is LoadLibrary, or LoadLibraryEx with
   no flag but LOAD_WITH_ALTERED_SEARCH_PATH: every other flag either keeps the DLL from
   running (AS_DATAFILE, AS_IMAGE_RESOURCE, DONT_RESOLVE_DLL_REFERENCES) or moves the search
   away from the game folder (LOAD_LIBRARY_SEARCH_*), and such a load goes through untouched.
   A bare name already loaded under that name is left to LoadLibrary, which returns that
   module. The one load this can answer differently from Windows is a bare name that is also
   a KnownDLL, where Windows would take the system copy over the game folder's; no KnownDLL
   exports DirectDrawCreate, and no TADR build is named like one.

   `tagpu_takeover.off` in the game folder turns it off: the suite's harness setup that shows
   the safety net (tagpu_patches.c, lim_verify) refusing what this would have kept out.

   RETURNS Impure's module with one more reference (as LoadLibrary would), or NULL when the
   request is not ours to answer -- the caller then loads normally. Any thread; no state. */

#include <windows.h>

/* `flags` is LoadLibraryEx's, 0 for LoadLibrary; `caller` is the return address, for the log. */
HMODULE tagpu_takeover_loadlibrary_a(const char* name, DWORD flags, void* caller);
HMODULE tagpu_takeover_loadlibrary_w(const wchar_t* name, DWORD flags, void* caller);

#endif
