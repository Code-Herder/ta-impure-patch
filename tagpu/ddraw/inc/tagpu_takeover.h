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

   It can be kept from RUNNING, because loading and running are two steps.
   tagpu_takeover_tadr_init, from DllMain, writes `mov eax,1; ret 0Ch` over the PE entry point
   of every module of the game folder that is a TADR build, so the loader's call into it
   reports success and does nothing: no unit initialization, no threads, no window hooks, and
   not the jump the recorder's DllMain splices over the exe's entry point (0x004E6FA0,
   research/notes/deep-tadr.md). Its exports stay bound, which is why pass 3 is not optional:
   an uninitialised Delphi DLL must never be called.

   THE INVARIANT THIS RESTS ON is that no module of the game folder has been initialised yet,
   so no entry point written here is one the loader has already called or is calling. The
   loader maps the whole import graph, then initialises it as a post-order walk in
   import-directory order; Impure imports nothing from the game folder; so the condition is
   exactly "the FIRST import descriptor of the exe that leads into the game folder is
   Impure's", and every other game-folder module is in a later descriptor's subtree.
   to_first_local_is_ours tests that against the exe in front of it, over every slot of a
   descriptor rather than its first, and the pass does nothing when it fails -- it is not an
   assumption about the retail exe. WHAT IT DOES NOT COVER: a game-folder module that is not in
   the exe's import table at all, and a PE TLS callback, which the loader calls whatever the entry
   point holds. Both are named where they are found (to_tls_callbacks logs one) and answered by
   pass 4; no setup of the suite has either (MEASURED 2026-09-27). The 3.9.02 and Escalation
   exes import TDRAW / TAESC and no DDRAW at all (DISASSEMBLED: objdump -p), so there TADR's
   DllMain is what loads Impure and is running while this would write: the pass is skipped and
   says so, and pass 4 is what answers for such a launch. MEASURED on Wine and on Windows (the
   suite, 2026-09-26): on the retail and Patch Loader exes, which import DDRAW first, Impure's
   limits were installed before TADR's limit crack ran.

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
   the exe's entry point, and therefore before the exe can make a DirectPlay call: every
   import descriptor of the exe whose slots lead into a DLL in the game folder that exports
   DirectPlayCreate (and is not Impure) has ALL its slots pointed at Impure's forwarders, or
   none of them: a slot this cannot name leaves the descriptor as it is and says so. THE EXPORT
   IS ASKED OF THE FILE, never of the loaded module: GetProcAddress on a forwarded export makes
   the loader load and initialise the target, and every Patch Loader's dplayx.dll forwards all
   nine of its exports to tplayx, so asking here -- from DllMain, under the loader lock -- would
   start the recorder out of the loader's order, on the routes where pass 2 was skipped. Impure's
   own module is PINNED before the first such write, because the slots then hold addresses of
   its code for the life of the process. A forwarder loads Windows' own dplayx.dll by its full
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
   image (FF 15, FF 25), `push imm32; ret`, `mov eax,imm32; jmp eax`. A target inside a module of
   the game folder that is not Impure's refuses the launch, naming the site, the bytes and the
   module.

   ONLY AN INSTRUCTION THAT TRANSFERS CONTROL NAMES A TARGET. Four bytes of changed code that
   merely HOLD an address inside such a module are counted (`maybe`, one line in the log) and
   never refuse, because the bytes are as likely to be the middle of an instruction or the
   displacement of a jump, and which of them look like an address depends on where the loader put
   a DLL that day. MEASURED on Windows, the suite 2026-09-27, both halves: `8B 96 92 00` -- the
   middle of a `mov esi,[esi+0x92]` of ours -- reads as 0x0092968B with Total Mayhem's recorder
   mapped at 0x00910000, and a rel32 displacement of ours to a stub above the image reads as
   0x020F-something with the recorder beside the retail exe mapped at 0x020C0000; judged as
   addresses they refused those two installs over 3 and 32 of Impure's own patch sites, TADR
   having run nothing at all. Every TADR hook ever measured here -- 24 on the 2006 recorder, 6 on
   the entry-point route, 13 to 18 of a tdraw's -- is found by its instruction.

   The same pass reads EVERY IMPORT SLOT of the exe, which is the other way a call can leave the
   image: the code that reaches a slot is stock, so the comparison never looks at it, and the
   file holds no bound address to compare with -- what makes a slot wrong is where it leads. A
   slot leading into a TADR module refuses too, and that is also the answer to pass 3's own
   failure mode: a descriptor it could not name every slot of still holds the recorder's
   addresses, and the recorder is inert by then, so the first call into it would run
   uninitialised Delphi code. THE SLOTS ARE JUDGED EVEN WHEN THE CODE COULD NOT BE COMPARED --
   an exe another program holds open for writing, or one loaded away from its ImageBase -- since
   a slot does not depend on the file.

   ONE FINDING A CHANGED RUN, AND A TADR ONE WINS IT: the bytes before a run can be a stock FF 15
   through an import slot into the mod's own WIN32.dll, which the retail exe imports, and stopping
   there would hide a TADR hook in the same run. The opcode is looked for from FIVE bytes before
   the first changed byte, since FF 15 / FF 25 carry their operand at offsets 2 to 5.

   WHY THIS EXEMPTS THE MOD AND NOT TADR, with no list of sites in it: a mod's own changes to
   the engine are in its exe FILE, so they are not changed bytes at all; Impure's patches and
   the Patch Loader's lead into Impure's module or into stubs Impure allocated, which are in no
   module image; TADR's lead into TADR. WHAT IT DOES NOT SEE: a hook installed by writing a
   function pointer into a data section other than an import slot (the engine's globals differ
   from the file everywhere by the time this runs, so comparing them would be noise), a hook
   whose target is computed at run time, a TADR build carrying neither marker string, and
   anything written after this call -- which is why passes 1 to 3 keep TADR's code from running
   at all rather than cleaning up after it. An exe loaded away from its own ImageBase has its
   code left uncompared, and says so in the log; its import slots are still read. */

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
