#ifndef TAGPU_TAKEOVER_H
#define TAGPU_TAKEOVER_H
/* tagpu_takeover -- keeping TADR's code out of the process on the routes where Impure starts
   first (research/notes/compat/takeover.md, part 1). Two ways TADR gets in, one answer each.

   ---- tdraw.dll: the LoadLibrary answer -------------------------------------------------

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

   RETURNS Impure's module with one more reference (as LoadLibrary would), or NULL when the
   request is not ours to answer -- the caller then loads normally. Any thread; no state.

   ---- the recorder: the exe's DirectPlay imports ----------------------------------------

   TADR's recorder (tplayx.dll; the 2006 build is a dplayx.dll of its own) cannot be kept
   from loading: the Patch Loader's dplayx.dll FORWARDS its DirectPlay exports to tplayx, and
   Windows resolves a forwarder while it binds the exe's imports, before any DllMain runs and
   without a LoadLibrary call anyone can hook. It can be kept from running: it does nothing
   until its first DirectPlay export is called -- that call writes its log, loads Windows'
   DirectPlay and installs its code injections (vendor/TADR/src/Recorder/Dplayx_exports.pas,
   OnInit -> InitCode.pas, OnInitialize) -- and the exe reaches DirectPlay only through its
   import table: TotalA.exe 3.1 imports DPLAYX.dll by ordinal 1, 2 and 4 and looks up no
   DirectPlay name anywhere (DISASSEMBLED: objdump -p, strings), and its first call is
   DirectPlayLobbyCreateA at start-up (MEASURED 2026-09-26: the recorder's log on every
   Patch Loader setup of the suite, single player included).

   tagpu_takeover_dplay_init, from DllMain -- after the loader has bound every import, before
   the exe's entry point: every import descriptor of the exe whose slots lead into a DLL in
   the game folder that exports DirectPlayCreate (and is not Impure) has ALL its slots
   pointed at Impure's forwarders, or none of them: a slot this cannot name leaves the
   descriptor as it is and says so. A forwarder loads Windows' own dplayx.dll by its full
   path on its first call -- from the game's code, outside the loader lock -- and passes every
   call on. An exe whose DirectPlay is Windows' own (the retail game alone) is not touched.

   `tagpu_takeover.off` in the game folder turns both off: the suite's harness setups that
   show the safety net (tagpu_patches.c, lim_verify) and the recorder check firing on what
   this keeps out. */

#include <windows.h>

/* `flags` is LoadLibraryEx's, 0 for LoadLibrary; `caller` is the return address, for the log. */
HMODULE tagpu_takeover_loadlibrary_a(const char* name, DWORD flags, void* caller);
HMODULE tagpu_takeover_loadlibrary_w(const wchar_t* name, DWORD flags, void* caller);

/* DllMain, DLL_PROCESS_ATTACH. Logs what it pointed where. */
void tagpu_takeover_dplay_init(void);

#endif
