# Keeping TADR out

The takeover that stops TADR's code running beside Impure.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.6d Keeping TADR out (`tagpu_takeover.c`, on, `tagpu_takeover.off`)

No engine address: the fork's `LoadLibraryA/W/ExA/ExW` hooks (`winapi_hooks.c`), which
`hook_init` puts into the import table of every module in the game folder from Impure's
`DllMain`, ask `tagpu_takeover_loadlibrary_*` first. A request for a DLL file in the game folder
whose export table names `DirectDrawCreate`, that is not Impure's own file and not already
loaded, is answered with Impure's module (one more reference, as `LoadLibrary` gives), and the
log says `takeover: DPLAYX.dll asked for tdraw.dll … answered with Impure, so it does not start`.
On the Community Patch Loader route that request is the loader's `LoadLibraryA("tdraw.dll")`:
TADR installs everything in its `DllMain`, so none of it runs, and the loader's `patch_call` at
`0x47BFA2`/`0x4B55FB` points the exe's two `DirectDrawCreate` calls at Impure's export. The
loader's own presets stay. The file's exports are read from disk with every offset bounded by the
file's size, through a handle that refuses writers while it is open, so the size is the view's.
Only a plain load is answered — `LoadLibrary`, or `LoadLibraryEx` with no flag but
`LOAD_WITH_ALTERED_SEARCH_PATH`; a load that keeps the DLL from running or searches elsewhere
goes through untouched, and so does a bare name already loaded under that name.
`tagpu_takeover.off` in the game folder turns it off, which is how the suite shows the safety net
(§2.6b) catching what it keeps out. MEASURED 2026-09-26 on Wine and Windows: Total Mayhem 11.3.0,
ProTA 4.8 and the three Patch Loader setups start with no `tdrawlog.txt` (and on Windows no
`tdraw.dll` among the modules) and, on Wine, fight the 200v200 battle; the retail setups log no
takeover. The plan and the other routes: [the
takeover](../compat/takeover.md).

**TADR's modules never run.** Loading and running are two steps, and the second one is ours.
`tagpu_takeover_tadr_init` walks every mapped
image (`util_enumerate_modules`, which reads the address space rather than the loader's list, so a
mapped-but-uninitialised module is found), and for each one whose file is in the game folder, is
not Impure's and is not the exe, writes `mov eax,1; ret 0Ch` over its PE entry point if the file
is a TADR build. The loader's call into it then reports success and does nothing: no Delphi unit
initialization, no threads, no window hooks, and **not the jump the recorder's `DllMain` splices
over the exe's entry point `0x004E6FA0`** ([the engine map](../exe-reverse-engineering.md)). The log
says `takeover: DPLAYX.dll is TADR's recorder -- its entry point was made inert before the loader
called it, so none of its own code runs`. This is why the recorder cannot be kept out by answering
a `LoadLibrary`: the Patch Loader's `dplayx.dll` forwards its DirectPlay exports to `tplayx.dll`,
and Windows binds a forwarder before any `DllMain`, with no call anyone can answer.

A module is TADR's when its **file** carries `TADemo-MKChat`, the name TADR's builds give their
chat channel; `TA Demo Recorder`, in the recorders only, tells a recorder from a tdraw for the log.
MEASURED 2026-09-27 over the suite's 130 fixture files: in all 18 TADR modules and in nothing else
— not the Patch Loader's `dplayx.dll`, not Total Mayhem's or ProTA's own, not cnc-ddraw's
`ddraw_custom.dll`, not any mod's exe. Never the file's name, which the mods change.

**The invariant**: an entry point is written only while it is one the loader has not called yet.
The loader maps the whole import graph, then initialises it as a post-order walk in
import-directory order, and Impure imports nothing from the game folder, so a module's place in
that walk is the place of the exe's import descriptor that names it. `to_descriptor_of` gives the
first such index — a descriptor names a module by its *name* (the module the loader bound it to,
`GetModuleHandleA`) or by any *slot* leading into it (how a forwarder such as the Patch Loader's
`dplayx.dll` → `tplayx.dll` is seen) — and a module is made inert only when that descriptor comes
after the descriptor of the module Impure is running inside, **and** the loader's own
initialisation-order list (`PEB_LDR_DATA.InInitializationOrderModuleList`, read under the loader
lock) does not hold it yet. The name is what a first launch needs: Windows' compatibility engine
re-points the exe's `DirectDrawCreate` slot at `apphelp.dll` on the first start from a new path
(MEASURED 2026-09-27, Windows 10 19041), and by slot alone Impure then lost its own descriptor and
the recorder ran — [the takeover note](../compat/takeover.md) has the measurement.
On the retail routes that module is Impure's own, the exe's `DDRAW`, and every TADR module
in the folder is named later or not named by the exe at all. Where the comparison cannot be made or
fails, the module is left running and the log says which and why; the image comparison below is
what answers for that launch. Per module rather than once for the exe, because on the 3.9.02 routes
Impure runs inside `TDRAW` and the recorder is named after it: judged by one flag for the whole
pass, the recorder stayed running there (MEASURED 2026-09-27, seven of its sites in the engine).

**Where the exe imports TADR, TADR's `DllMain` is what loads Impure, and the rest of it does not
run** (`tagpu_takeover_nested_init`, pass 1d). The 3.9.02 and Escalation exes import
`TDRAW`/`TAESC` and no `DDRAW` at all (DISASSEMBLED: `objdump -p`), and that `DllMain` loads
`ddraw.dll` itself, so Impure's own `DllMain` runs nested inside it with the whole rest of TADR's
still to come. Stopping that rest is **prevention, not repair**: nothing of TADR's has been
installed at that moment, so nothing has to be undone, and no build of TADR has to be understood.
Impure reads its own thread's stack, from its frame up to the stack's top (`NT_TIB.StackBase`
from the TIB, a `VirtualQuery` region only if that cannot be had), and finds two things in it:

- **the slot holding the return address of the `LoadLibrary` that is loading us** — a slot whose
  value lies inside a module of the game folder whose *file* carries TADR's marker, six bytes
  before which that module's own bytes decode as a call (`FF 15` through a pointer, `E8` rel32,
  `FF /2` with a register and a displacement, `FF D0`..`FF D7`, `FF 10`..`FF 17`), and whose next
  slot up — `LoadLibrary` being `stdcall`, its argument sits directly above its return address —
  points at a string naming Impure's own file, read bounded, ANSI or wide, compared as a basename;
- **that module's `DllMain` frame** above it: the **outermost** slot holding a return address into
  the same module whose next two slots are the module's own base and `DLL_PROCESS_ATTACH`, which
  is what the loader pushes. Outermost, because a helper the module calls with the same two values
  in the same places would otherwise be unwound to instead, and `DllMain` is the last of that
  module's frames on the stack; the entry point above it is called from ntdll, so its return
  address is not in the module and it never matches. A caller that pops the arguments itself
  (`83 C4`, `81 C4` or `8D 64 24`, which would make `DllMain` `cdecl`) refuses the pass, since the
  stub returns with `ret 0Ch`.

**The exit is read out of the function, not guessed at.** The call that entered the frame names it
— `E8` outright, `FF 15` through a pointer inside the module, anything else refuses — and its first
instructions are decoded: `push ebp; mov ebp,esp` confirms the frame pointer the stub unwinds to,
and every `push` of `ebx`, `esi` or `edi` after it is recorded with the stack address it pushed to,
following the stack adjustments in between. That matters because a stdcall callee owns those three
registers for its caller: a return that skips its `pop`s hands the caller the callee's working
values, silently, and only on a build whose caller keeps something live in one of them.

The return address in that first slot is then replaced with a stub Impure generates from those
values: `mov eax,1`; each recorded register loaded back from the frame by absolute address; `fs:0`
put back to the first exception record that is **not** below the frame — the value the frame's own
epilogue would leave, whether or not it registered one itself, since a record below `ebp` belongs
to this frame or a deeper one and every one of those is gone by the time the stub returns; `mov
esp,<that ebp>`; `pop ebp`; `ret 0Ch`. TADR's `DllMain` therefore returns success to the loader the
moment `LoadLibrary` hands it Impure, with every register its caller is owed. The stub's page is
allocated `PAGE_READWRITE` and then made `PAGE_EXECUTE_READ` — never writable and executable at
once — and nothing inside TADR's own image is written.

**The loader has one call left**: `DLL_PROCESS_DETACH` at process exit, which that module's own
code would answer by freeing what it never set up. Its entry point is made inert for it, the way
every other TADR module's is — the loader has already entered it, so nothing is taken from a call
in flight, and the only call that can still reach it is the detach. Its TLS callbacks are untouched
either way: the loader ran them before the entry point, so its TLS index exists. Every value is verified before anything is
changed, and each one that is missing leaves the pass undone with its reason in the log (`takeover:
the start-up that loaded Impure was not stopped -- …`). MEASURED 2026-09-27: it fires on `TDRAW.dll`
(TADR `dev-dcff5dd` and v2026.8.6) and on Escalation's `TAESC.dll`, and the log then reads
`takeover: TDRAW.dll loaded Impure from its own DllMain, and the rest of that DllMain does not run`.
The fail-safe is the image comparison at the first DirectDraw call: a launch where this pass did
nothing and TADR then hooked the engine is refused, not played.

Once that `DllMain` is stopped, the exe's import from it leads into a module that will never
initialise, so `nest_take_imports` points the slots of the descriptors naming it at Impure's own
exports of the same names — all of a descriptor's slots or none of them, as the DirectPlay pass
does, and only where Impure exports every name the exe asks for. One slot on both routes,
`DirectDrawCreate`. What a 3.9.02 player loses with it is what TADR's tdraw drew; the exe's own
raised limits are in the exe.

**The exe's DirectPlay imports**, which this makes load-bearing: an uninitialised Delphi DLL must
never be called. From `DllMain` too, after every import is bound and before the exe's entry point,
`tagpu_takeover_dplay_init` reads the exe's import directory (every RVA bounded by the
image): a descriptor whose first slot leads into a DLL of the game folder that exports
`DirectPlayCreate` and is not Impure has every one of its slots pointed at Impure's forwarders
— `DirectPlayCreate`, `DirectPlayEnumerate`/`A`/`W`, `DirectPlayLobbyCreateA`/`W`, by
Microsoft's ordinals or by name — or, if one slot is anything else, none of them. On the retail
exe that is `DPLAYX.dll`'s three slots (`0x4FC034`..`0x4FC03C`, the [engine
map](../exe-reverse-engineering.md)); the page is made writable for the write and put back. A
forwarder loads Windows' `dplayx.dll` by its full path on its first call — from the game's code,
outside the loader lock — publishes it with a compare-exchange and passes the call on. If it
cannot load, E_FAIL, which each of the exe's four DirectPlay call sites survives: the enum at
`0x4CA435` discards the result, the lobby at `0x4CA4D7` tests it and has zeroed its out-pointer
first, and the creates at `0x4CA667` and `0x4CA922` test it and return (DISASSEMBLED). The log
says `takeover: DPLAYX.dll's 3 DirectPlay imports led into tplayx.dll, a DirectPlay DLL in the
game folder -- pointed at Windows' own, so it does not start`, then, at the first call, `the
game's DirectPlay is Windows' dplayx.dll`. An exe whose DirectPlay is Windows' own is not
touched. The same lever turns it off. It also closes the recorder's *other* way in on its own —
the path that writes the recorder's log and installs its injections from inside an export.

**The exe image against the exe file** (`tagpu_takeover_verify_image`), at the first DirectDraw
call, beside §2.6b's net and before the first frame. The exe file is read from disk and every
executable section of it compared with the same bytes in memory; each run of changed bytes is
decoded for what a hook has to be — `E8`/`E9` rel32, `FF 15`/`FF 25` through a pointer inside the
image, `push imm32; ret`, `mov eax,imm32; jmp eax`, and any absolute address the run holds — and a
target inside a TADR module of the game folder refuses the launch through `tagpu_refuse`, naming
the site, the bytes, the file's bytes and the module. **Only an instruction that transfers control
names a target**: four bytes that merely hold such an address are counted in the log and never
refuse, because they are as likely to be the middle of an instruction or the displacement of a
jump, and where a DLL lands decides which of them look like an address — judged as addresses they
refused `retail+tadr1` over 32 of our own sites and Total Mayhem over 3 (MEASURED on Windows
2026-09-27, TADR having run nothing on either; Wine maps both DLLs elsewhere and showed neither).
**No list of sites is involved**: a mod's own
changes are in its exe file, so they are not changed bytes; Impure's patches and the loader's lead
into Impure's module or into stubs Impure allocated, which are in no module image. Only TADR
refuses, and a measurement is why — the Patch Loader rewrites `0x4E4708`, `0x4E71A0` and
`0x4EADF2` into direct calls to the mod's own `win32.dll`, which is the mod's byte and stays; both
kinds are logged. The same pass reads **every import slot of the exe** too, the other way a call can
leave the image: the code reaching a slot is stock and the file holds no bound address, so what
makes a slot wrong is where it leads, and a slot leading into a TADR module refuses — which is also
the answer to a descriptor the DirectPlay pass could not name every slot of, since the recorder is
inert by then. It does not see a function pointer written into a data section other than an import
slot, a target computed at run time, a TADR build with neither marker, or anything written after
this call, which is what the passes above are for; an exe loaded away from its own `ImageBase` has
its code left uncompared and says so.

MEASURED 2026-09-27, one full suite run on Wine and one on the Windows test box on the same DLL,
read out of the live process by the suite as well as in-process — **15 of 16 setups meet the goal,
1 known gap, 0 UNEXPECTED on each platform**: `retail` 443 changed runs in `.text` and nothing
leading out of the exe; `retail+tadr1`, `retail+tadr-recorder-ota`, `retail+tadr-files`, the three
Patch Loader setups, Total Mayhem 11.3.0, ProTA 4.8, **both 3.9.02 routes and both Escalation
routes** all **0 sites leading into TADR**, alone, in the battle and — for eleven of them — in the
two-player network game, on every peer, where the same read on the T1b DLL found 6 hooks into the
2006 recorder. With `tagpu_takeover.off` that recorder installs 6 sites by start-up and 28 plus the
exe's 3 DirectPlay slots once a battle and a network game let its DirectPlay path run, which is the
proof the check can see what the game folder cannot show ([the setups](../compat/setups.md)). The one
gap is gammata's drop-in, where Impure never loads.

Three things the landing review changed here, each a hole the measurements could not have shown:
the exe's **import slots are judged even when the code comparison could not be made** (an exe held
open for writing, or loaded away from its `ImageBase`), since a slot does not depend on the file;
a changed run is scanned for a **TADR** target before any other module's, because the bytes before
it can be a stock `FF 15` through a slot into the mod's own `WIN32.dll` and stopping there hid a
hook behind it; and the lookback is **five** bytes, not four, since `FF 15`/`FF 25` carry their
operand at offsets 2 to 5. Pass 3 also asks a DLL's **file** whether it exports `DirectPlayCreate`
rather than asking the loaded module: `GetProcAddress` on a forwarded export loads and initialises
the target, and every Patch Loader's `dplayx.dll` forwards all nine of its exports to `tplayx`.
