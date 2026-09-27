#ifndef TAGPU_TAKEOVER_H
#define TAGPU_TAKEOVER_H
/* tagpu_takeover -- TADR's DLLs run none of their own code, and no launch goes ahead with a
   byte of the game's code leading into one (research/notes/compat/takeover.md, part 1).

   THE EXE FILE ON DISK IS THE REFERENCE. Three passes keep TADR's code from running -- one
   for each way it gets in -- and a fourth compares the whole image against that file and
   refuses the launch when any of them missed. The first three are a list of the ways in and
   so a guess about what TADR does; the fourth is not, which is why it is the one that decides.
   `tagpu_takeover.off` in the game folder turns all four off, for the suite's harness setups
   that need TADR running to show the safety net and the recorder check firing.

   ---- 1. tdraw.dll asked for by name: the LoadLibrary answer ------------------------------

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

   ---- 2. a TADR module already mapped: the inert entry point ------------------------------

   The recorder cannot be kept from loading. On the Patch Loader route the loader's dplayx.dll
   FORWARDS its DirectPlay exports to tplayx.dll, and Windows resolves a forwarder while it
   binds the exe's imports -- before any DllMain runs, with no LoadLibrary call anyone can
   answer. Beside the retail exe the 2006 recorder IS the game folder's dplayx.dll, imported
   by the exe itself.

   It can be kept from RUNNING, because loading and running are two steps. When Impure's
   DllMain runs, every module the exe's imports pull in is mapped and bound and NONE of their
   DllMains has been called yet -- the loader maps the whole graph, then initialises it in
   dependency order, and the exe imports DDRAW first (DISASSEMBLED: objdump -p; MEASURED on
   Wine and on Windows, the suite 2026-09-26, Impure's limits installed before TADR's limit
   crack ran). tagpu_takeover_tadr_init, from DllMain, writes `mov eax,1; ret 0Ch` over the
   PE entry point of every module of the game folder that is a TADR build, so the loader's
   call into it reports success and does nothing: no unit initialization, no threads, no
   window hooks, and not the jump the recorder's DllMain splices over the exe's entry point
   (0x004E6FA0, research/notes/deep-tadr.md). Its exports stay bound, which is why pass 3 is
   not optional: an uninitialised Delphi DLL must never be called.

   WHICH MODULES ARE TADR: the ones whose FILE carries "TADemo-MKChat", the name TADR's own
   builds give their chat channel. MEASURED 2026-09-27 over every file of the suite's
   fixtures (130 files, 12 installs): it is in all 18 TADR modules -- every tdraw.dll and its
   renamed copies (TAESC.dll), every recorder (tplayx, eplayx, zplayx, the 2006 Dplayx.dll)
   -- and in nothing else, the Patch Loader's own dplayx.dll, Total Mayhem's and ProTA's
   dplayx.dll, cnc-ddraw's ddraw_custom.dll, the audio DLLs and every mod's TotalA.exe
   included. "TA Demo Recorder", in the 9 recorders only, tells the two apart for the log.
   Never the file's name: the mods rename everything.

   ---- 3. the exe's DirectPlay imports ----------------------------------------------------

   tagpu_takeover_dplay_init, from DllMain -- after the loader has bound every import, before
   the exe's entry point: every import descriptor of the exe whose slots lead into a DLL in
   the game folder that exports DirectPlayCreate (and is not Impure) has ALL its slots
   pointed at Impure's forwarders, or none of them: a slot this cannot name leaves the
   descriptor as it is and says so. A forwarder loads Windows' own dplayx.dll by its full
   path on its first call -- from the game's code, outside the loader lock -- and passes every
   call on. An exe whose DirectPlay is Windows' own (the retail game alone) is not touched.
   TotalA.exe 3.1 imports DPLAYX.dll by ordinal 1, 2 and 4 and looks up no DirectPlay name
   anywhere (DISASSEMBLED: objdump -p, strings), so those three slots are every way the exe
   has of reaching DirectPlay.

   This is what keeps a game the recorder can no longer initialise from calling into it, and
   it also closes the recorder's second way in on its own: the path that writes the
   recorder's log, loads Windows' DirectPlay and installs its code injections from inside an
   export (vendor/TADR/src/Recorder/Dplayx_exports.pas, OnInit -> InitCode.pas,
   OnInitialize(false)). Windows' own service provider still reads gdwDPlaySPRefCount from
   whatever module is loaded under the name dplayx.dll, which on the loader route is the
   loader's and forwards to the recorder: that is one counter in a statically initialised data
   section, read and written by DirectPlay alone, and no code of the recorder's runs for it.

   ---- 4. the whole image against the file: the check that decides ------------------------

   tagpu_takeover_verify_image, from the first DirectDraw call -- after every DllMain and the
   exe's entry point, before the first frame, beside the limits' own safety net
   (tagpu_patches.c, lim_verify). It reads the exe file from disk, compares every executable
   section of it with the same bytes in memory, and for each run of changed bytes decodes what
   a hook has to be: a rel32 call or jump (E8, E9), a call or jump through a pointer inside the
   image (FF 15, FF 25), `push imm32; ret`, `mov eax,imm32; jmp eax`, and any absolute address
   the run itself holds. A target inside a module of the game folder that is not Impure's
   refuses the launch, naming the site, the bytes and the module.

   WHY THIS EXEMPTS THE MOD AND NOT TADR, with no list of sites in it: a mod's own changes to
   the engine are in its exe FILE, so they are not changed bytes at all; Impure's patches and
   the Patch Loader's lead into Impure's module or into stubs Impure allocated, which are in no
   module image; TADR's lead into TADR. WHAT IT DOES NOT SEE: a write to a data section (the
   engine's globals differ from the file everywhere by the time this runs), a hook whose target
   is computed at run time, and anything written after this call -- which is why passes 1 to 3
   keep TADR's code from running at all rather than cleaning up after it. An exe loaded away
   from its own ImageBase is not compared, and says so in the log. */

#include <windows.h>

/* `flags` is LoadLibraryEx's, 0 for LoadLibrary; `caller` is the return address, for the log. */
HMODULE tagpu_takeover_loadlibrary_a(const char* name, DWORD flags, void* caller);
HMODULE tagpu_takeover_loadlibrary_w(const wchar_t* name, DWORD flags, void* caller);

/* DllMain, DLL_PROCESS_ATTACH, in this order and before anything of ours patches the exe.
   Both log what they did. */
void tagpu_takeover_tadr_init(void);
void tagpu_takeover_dplay_init(void);

/* The first DirectDraw call. Logs what it compared; refuses the launch (tagpu_refuse.h) and
   does not return when a byte of the game's code leads into another module of the folder. */
void tagpu_takeover_verify_image(void);

#endif
