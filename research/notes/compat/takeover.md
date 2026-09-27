# The takeover (plan)

## Summary

The owner's direction (2026-09-26): mods run **as is, with their own exe and files**, and Impure
is the only thing that patches the engine. One rule for every setup — **stop TADR's code, keep
every byte the mod itself sets** — reached by one of two ways in, chosen by which DLL loads
first, plus a safety net that turns any route nobody foresaw into a clear refusal instead of a
crash like [Total Mayhem's](tadr-collision.md). The [suite](suite.md) is the gate: a landing
here is done when its setups meet their goal on Wine and Windows and their `today` entries are
deleted.

**Where it stands (2026-09-27).** On every route where Impure loads first — the retail exe, the
2006 recorder beside it, and every Community Patch Loader mod — **none of TADR's code runs, and a
launch where any of it does refuses to start.** That is T1 (`tdraw.dll` and the safety net), T1b
(the exe's DirectPlay imports) and T1c (the entry point and the reference image) together; the
routes where TADR loads Impure are open, and one landing closes them: **1d and part 2 together**
(the decision of 2026-09-27, below).

The shape is the owner's, set out on 2026-09-27: **the exe file on disk is the reference**, TADR's
DLLs run none of their own code, and every launch compares the whole exe image against that file
and refuses to start when a changed byte leads into a TADR DLL. A list of hook sites is not the
answer — it is a guess about what TADR does, and the recorder finding is what that guess cost:
T1b was built on the premise that the recorder does nothing until its first DirectPlay export is
called, and the recorder takes the exe's entry point instead.

**The routes where TADR loads Impure are answered the same way as the other three** (the decision
of 2026-09-27): the rest of TADR's `DllMain` does not run, rather than running and having its
bytes put back afterwards. The measurements behind the choice are in 1d — Impure arrives before
TADR has written anything, so there is nothing to preserve; TADR's one thread does not exist yet,
so prevention stops it from ever starting; and nothing of TADR's is written later either, so
neither design would have needed to chase it.

## What stays, what goes

**Stays — the mod's content, whoever writes it** (the exe file, the Patch Loader, or a TADR
build):

- **Resource paths**: the registry key (`RegistryPath=`), the ini and GP3 names, the download
  folder, renamed data folders (`gamedatP`, `unitsM`, `WeaponM`, `guiM`, …).
- **The mod's multiplayer identity**: its version string and the battleroom version bytes
  `0x49E9C0`/`0x49E9C9`, which all players must match.
- **The mod's gameplay patches**: the AI fixes, target acquisition, teleport, reclaim rules,
  `+AI`/`+Control` levels, allied victory — the mod's rules. Where one lands on an Impure site
  the plan is that the mod's value wins (part 2 does it for the exe file's bytes). Today the
  safety net refuses a loader write that leaves other bytes than Impure's on one of its sites;
  it read every site after each of the suite's Patch Loader setups and found none. Measured today: Mayhem's loader writes the path budget at `0x40EAD6` with
  Impure's own value (66 650); Mayhem and ProTA write `B0 01` at `0x4266A5`, beside Impure's
  `EB` at `0x4266A7`, both removing the DirectX box; the 3.9.02 and Escalation exes carry their
  own path budget in the file.
- **Values poked into spare bytes for TADR to read** (Mayhem's click-snap radii at `0x101F0A`):
  harmless, nothing reads them without TADR.

Impure already follows the moved paths: what it reads of the game's settings it reads from
engine memory, not from a registry path. Only tacli's test-mode registry store assumes the
stock key (`tagpu_regstore.c`), which is why the suite writes a mod's key with `wine reg`.

**Goes — TADR's code, all of it** (the owner, 2026-09-26: "We do not load TADR, we will
replace it"): `tdraw.dll` and its renamed builds (`mdraw`, `zdraw`, `TAESC`), with everything
they install — the limit crack, EngineLimits, the bug fixes, the megamap, the chat and income
overlays, the anticheat hashing — and the recorder, `tplayx.dll` or the 2006 `dplayx.dll`, with
its demos, TA Forever's replays, its commands and its code injections. Until Impure has its own,
a game played with Impure records no demo. What a mod's content may still need from them is a
per-mod check (below).

## The three parts

### Part 1 — none of TADR's code runs

**Four passes, one for each way TADR's code gets in**, and part 3 below is the check that
decides. Three of them are for the routes where Impure loads first — the retail exe, and every
Patch Loader mod (Total Mayhem, ProTA, TA Zero); the fourth, 1d, is for the routes where TADR
loads Impure. All are in `tagpu_takeover.c`, whose header carries the contract;
`tagpu_takeover.off` in the game folder turns all four off, which is how the suite's harness
setups have TADR to catch.

**1a. `tdraw.dll` asked for by name.** Impure's `DllMain` runs `hook_init` (`dllmain.c`), which
points the `LoadLibrary` imports of every module in the game folder at the fork's
`fake_LoadLibrary*` (`winapi_hooks.c`), the loader's `dplayx.dll` included, before the loader's
own `DllMain` runs ([§2.6d](../gpu-status.md#26d-keeping-tadr-out-tagpu_takeoverc-on-tagpu_takeoveroff)):
the exe imports `DDRAW` first and `DPLAYX` eighth, and on Wine and on Windows (the suite,
2026-09-26, Total Mayhem and ProTA) Impure's limits were installed before TADR's limit crack
ran. When a module asks for a DLL in the game folder whose export table carries
`DirectDrawCreate` and which is not Impure, Impure answers with its own module. TADR's
`DllMain` — where it installs everything (`vendor/TADR/src/DDraw/ddraw.cpp`) — never runs; the
loader's `GetProcAddress` finds Impure's `DirectDrawCreate`, and its `patch_call` at
`0x47BFA2`/`0x4B55FB` points the exe's two calls at Impure directly. The test is the file's
exports, not its name, because the mods rename it.

**1b. The recorder cannot be kept from loading, so it is kept from running.** The loader's
`dplayx.dll` *forwards* its DirectPlay exports to `tplayx.dll`
(`vendor/Total-Annihilation-Patch-Loader/exports.def`), and Windows resolves a forwarder while
it binds the exe's imports, before any `DllMain` and without a `LoadLibrary` anyone can answer;
beside the retail exe the 2006 recorder *is* the game folder's `dplayx.dll`, imported by the
exe itself. Loading and running are two steps, and the second one is ours: Impure writes
`mov eax,1; ret 0Ch` over the PE entry point of every module of the game folder that is a TADR
build, and the
loader's call into it reports success and does nothing: no Delphi unit initialization, no
threads, no window hooks, and **not the jump the recorder's `DllMain` splices over the exe's
entry point `0x004E6FA0`** (`vendor/TADR/src/Recorder/InitCode_CoreExePatching.pas`, whose unit
`initialization` runs in `DllMain`, and `InitCode.pas`; [the recorder's page](../deep-tadr.md)).
The same write covers a `tdraw.dll` that reached the process by a path 1a does not see.

**The invariant it rests on** is that no module of the game folder has been initialised yet, so no
entry point written is one the loader has already called or is calling. The loader maps the whole
import graph, then initialises it as a post-order walk in import-directory order; Impure imports
nothing from the game folder; so the condition is exactly *the first import descriptor of the exe
that leads into the game folder is Impure's*, and every other game-folder module is in a later
descriptor's subtree. Impure tests that against the exe in front of it and the pass does nothing
when it fails — it is not an assumption about the retail exe. The 3.9.02 and Escalation exes import
`TDRAW` / `TAESC` and **no `DDRAW` at all** (DISASSEMBLED: `objdump -p`), so on those routes TADR's
`DllMain` is what loads Impure and is running while this would write: the pass is skipped, says so
in the log, and 1d below is what answers for such a launch. Every slot of a descriptor is looked at, not its first,
so a descriptor one unbindable slot would have hidden still counts.

**What the invariant does not cover**, and the log would be wrong about: a game-folder module that
is in the exe's import table not at all — pulled in as a dependency of an earlier descriptor's
module, or by a forwarded export. Such a module is already initialised when this runs, and its
entry point is made inert anyway; only part 3 answers for it. No setup of the suite has one
(MEASURED 2026-09-27, `objdump -p` over every fixture's exe and DLLs). Nor does it cover a **PE TLS
callback**, which the loader calls whatever the entry point says. MEASURED 2026-09-27 over the
fixtures' 18 TADR modules: **no recorder carries a TLS directory at all** (`tplayx`, `eplayx`,
`zplayx`, the 2006 `Dplayx.dll` — nine of nine), Total Mayhem's and ProTA's `tdraw.dll` carry one
whose callback array begins with NULL, gammata's carries none, and the **six remaining
`tdraw`/`TAESC` builds carry two callbacks each** — the C runtime's dynamic-TLS initialisers
[INFERRED from the `/GS` cookie and the `fs:0x2c` indexing], one gated on `DLL_THREAD_ATTACH` and
one on `DLL_THREAD_DETACH` / `DLL_PROCESS_DETACH`, so neither does anything at
`DLL_PROCESS_ATTACH`.

Nothing runs today because the two sets are **disjoint**: every module this pass makes inert is a
recorder, and every build whose callback array is live is a `tdraw`/`TAESC` loaded only where the
exe imports TADR — the routes where the pass does not run at all. **They have to stay disjoint,
and today only the log says so.** An inert entry point means the module's CRT start-up never runs,
so its TLS index is never allocated, and the `DLL_THREAD_ATTACH` callback would then index another
module's TLS block and walk what it found there, on every thread the game creates. A module whose
callback array is live is one to refuse, not one to make inert.

A module is TADR's when its **file** carries `TADemo-MKChat`, the name TADR's builds give their
chat channel. MEASURED 2026-09-27 over every file of the suite's fixtures — 130 files, 12
installs — it is in all 18 TADR modules (every `tdraw.dll` and its renamed copies, Escalation's
`TAESC.dll` included, and every recorder: `tplayx`, `eplayx`, `zplayx`, the 2006 `Dplayx.dll`)
and in **nothing else**: not the Patch Loader's `dplayx.dll`, not Total Mayhem's or ProTA's own
`dplayx.dll`, not cnc-ddraw's `ddraw_custom.dll`, not the audio DLLs, not any mod's
`TotalA.exe`. `TA Demo Recorder`, in the 9 recorders only, tells the two apart for the log.
Never the file's name: the mods rename everything.

**1c. The exe's DirectPlay imports** — which is now load-bearing rather than belt-and-braces,
because an uninitialised Delphi DLL must never be called. After the loader has bound every
import, every import descriptor of the exe whose slots lead into a DLL of the game folder that
exports `DirectPlayCreate` (and is not Impure) — **asked of the DLL's file, never of the loaded
module**, because `GetProcAddress` on a forwarded export makes the loader load and initialise the
target, and every Patch Loader's `dplayx.dll` forwards all nine of its exports to `tplayx`: asking
from inside `DllMain`, under the loader lock, would start the recorder out of the loader's own
order, on the very routes where 1b did not make it inert. Every one of those files exports the
*name* `DirectPlayCreate` (MEASURED 2026-09-27: the loader's, the 2006 recorder's, Mayhem's and
ProTA's), so the file answers the same question — has **all** its slots pointed at Impure's own
forwarders, or none of them: a slot Impure cannot name leaves the descriptor alone and says so
in the log. A forwarder loads Windows' `dplayx.dll` by its full path on its first call, from
the game's code and outside the loader lock, and passes every call on. TotalA.exe 3.1 imports
`DPLAYX.dll` by ordinal 1, 2 and 4 and looks up no DirectPlay name anywhere (DISASSEMBLED:
`objdump -p`, `strings`), so those three slots are every way the exe has of reaching
DirectPlay ([the engine map](../exe-reverse-engineering.md), *Where other patchers meet ours*).
This also closes the recorder's *other* way in on its own — the path that writes the recorder's
log, loads Windows' DirectPlay and installs its code injections from inside an export
(`vendor/TADR/src/Recorder/Dplayx_exports.pas`, `OnInit` → `OnInitialize(false)`). Windows'
own service provider still reads `gdwDPlaySPRefCount` from whichever module is loaded as
`dplayx.dll`, which on the loader route forwards to the recorder: that is one counter in a
statically initialised data section, written by DirectPlay alone, and no recorder code runs
for it.

**What it measures to.** One full suite run a platform on the DLL this lands, 2026-09-27, read out
of the live process: **15 setups of 16 meet the goal, 1 is a known gap, none is UNEXPECTED, on Wine
and on Windows alike.** `retail` 443 changed runs in `.text` and nothing leading out of the exe;
`retail+tadr1`, `retail+tadr-recorder-ota`, `retail+tadr-files`, the three Patch Loader setups,
`mayhem-11.3.0`, `prota-4.8`, both 3.9.02 routes and both Escalation routes **0 sites leading into
TADR** — alone, in a 200v200 battle and, for eleven of them, in a two-player network game, on every
peer — where the same read on the T1b DLL found 6 hooks into the 2006 recorder's module and the
game folder showed nothing at all. Mayhem and ProTA report 3 sites each into the mod's own
`win32.dll`, which stay. With `tagpu_takeover.off` the 2006 recorder installs 6 by start-up and
**28 sites plus the exe's 3 DirectPlay slots** once a battle and a network game let its DirectPlay
path run, which is the proof the check can see what the game folder cannot show. The gap is
gammata's drop-in (T3), where Impure never loads at all.

**1d. The exe imports TADR, so TADR's `DllMain` is what loads Impure.** The 3.9.02 exe imports
`TDRAW`, Escalation's imports `TAESC`, and neither imports `DDRAW` at all (DISASSEMBLED: `objdump
-p`), so the loader never loads Impure from the exe: TADR's `DllMain` does, with
`LoadLibraryA("ddraw.dll")`, and Impure's `DllMain` runs nested inside one that has barely started.
This is the fourth way in, and it is answered like the other three — **the rest of that `DllMain`
does not run** — rather than by cleaning up after it.

Everything TADR does before that call is four things, the same code in `tadr-dev-ota`'s
`tdraw.dll` and in Escalation's `TAESC.dll` (DISASSEMBLED): test the reason for
`DLL_PROCESS_ATTACH`, write `Process Attached.  config=<name>` to its own log,
`LoadLibraryA("dplayx.dll")`, then `LoadLibraryA("ddraw.dll")` — keeping the handle in a global of
its own. So TADR has patched nothing when Impure arrives. MEASURED on all four setups, Wine,
2026-09-27:

| | at Impure's `DllMain` | at the first `DirectDraw` call |
|---|---|---|
| the exe's code against its file | **0 changed runs** — byte-identical; the only finding is the `TDRAW`/`TAESC` import slot the loader bound | 407 to 436 changed runs, **15** leading into TADR (`392+tadr-dev`), **10** (`escalation`) |
| fail-closed sites differing from stock 3.1 | **1 of 284** — the exe **file's** own path budget (66650 in the 3.9.02 exe, 1114 in Escalation's), not a write of TADR's | the same one |
| threads | **one**: the exe's main thread, started at `0x004E6FA0` | **three**: that one, the exe's own `0x004E7850`, and **one thread of TADR's** |
| the game window's procedure | — | the window's and the class's are both the exe's own `0x004B5CC0`: **no TADR subclass** |

That the limits install sees **one** difference and not the seventeen of `mayhem-11.3.0-net`,
where the same family of `tdraw` runs its limit crack, is what says TADR's patching happens after
it has loaded Impure and not before.

**TADR writes nothing after its own start-up either.** The same comparison run 27 times a launch —
in Impure's `DllMain`, at the first `DirectDraw` call, then every two seconds to `t+50s` — gives
the **identical set** of places leading into TADR every time, compared as a set and not by size.
The changed runs do grow after the first `DirectDraw` call (407 to 436, 371 to 407): those are
Impure's own detours arming at first use, and TADR's count does not move. What that does not cover
is a **network game**, where the recorder's other way in installs eighteen more sites; the battle
stage did not load in that measurement, since getting past the refusal leaves the engine with
TADR's writes over Impure's fix sites.

**Why prevention and not repair.** Nothing has to be put back, TADR's one thread never starts, and
there is no window procedure to restore — none of which is true of a design that lets the start-up
finish and then reverts its bytes at the exe's entry point. The measurements above are what make
the choice safe rather than hopeful: Impure arrives before TADR has written anything, so there is
nothing to preserve, and the C runtime of TADR's module has already run, since the PE entry point
is the CRT's start-up and it calls `DllMain` after itself.

**How the moment is identified.** The return address into TADR is on the stack. MEASURED on all
four setups, the stack carries exactly **three** frames into the TADR module, at the same three
offsets on each, and the innermost is the `call` that loaded Impure. The instruction is `call esi`,
so it names no target — but `LoadLibraryA` is `stdcall`, so its argument is still one word above
that return address, and three conditions together settle it: a return address inside a
game-folder module whose **file** carries TADR's marker, a `call` in front of it, and an argument
naming Impure's own file. A coincidence does not satisfy all three.

**How the rest of that `DllMain` is skipped.** The saved return address of that `LoadLibrary`
call is replaced with a stub that returns TRUE out of TADR's `DllMain`: `mov eax,1`, `fs:0` put
back, `mov esp,<ebp>`, `pop ebp`, `ret 0Ch` — the frame's own values baked in as immediates,
valid for the one return the stub serves. **Nothing of TADR's code is written**; the only write
outside Impure is one dword of this thread's own stack, and the exe's one import slot below.

Its own `DllMain` would have been the tidier resume point, and it is not usable: the branch it
takes for any reason but `DLL_PROCESS_ATTACH` is **not** a path that returns doing nothing.
DISASSEMBLED, both builds: `cmp eax,1 / jne A`, and A is `test eax,eax / jne B` — the fall-through
at A is the **`DLL_PROCESS_DETACH` cleanup**, a chain of five calls that would free what was never
set up, and only B is the epilogue. Resuming at A would have run the detach path; finding B means
matching a second branch in a third-party binary. The stub needs neither.

What the stub has to get right, and how each part is established rather than assumed: `ebp` is the
frame word below DllMain's own return address, and the saved `ebp` it points at must itself be a
stack address further up. The SEH registration is **found**, by walking `fs:0` for the innermost
record that lies inside the frame and whose `Next` lies outside it — not assumed to be at
`ebp-0xC`, which is only where the two builds read put it. `ret 0Ch` is the stdcall `DllMain` the
two arguments on the stack have just confirmed, and a caller that popped them itself (`add esp,
imm8` at its return address) is refused instead, since then the stub would move its stack.

**The fail-safe is what makes that acceptable.** If any condition fails to verify — the return
address, the argument, the branch — **nothing is written and the launch refuses exactly as it does
today** (part 3). No build can end up worse than it already is, and the suite is what says which
builds are covered.

**The recorder is still made inert on these routes, and that is why they meet their goal rather
than merely starting.** Stopping the `DllMain` that loaded Impure does nothing about the recorder,
which the exe imports in a descriptor of its own: `TPLAYX`/`EPLAYX` is a separate module whose
`DllMain` the loader has not reached yet, so 1b applies to it. That is what the pass's precondition
became — not "the first game-folder descriptor is Impure's", but **"every descriptor naming this
module comes after the descriptor of the module whose `DllMain` we are inside"**, which is Impure's
own on the routes where the exe imports `DDRAW` and TADR's here. Without it these launches ran with
the recorder's six entry-point sites in the engine, which is what the first measured attempt showed
(2026-09-27: 7 places into `TPLAYX` with 1d alone, 0 with 1b generalised).

**The one slot left behind.** After this, `tdraw`'s exports must never be called, and the exe's
imports from it are small enough to name: the 3.9.02 exe imports **one** export from `TDRAW`,
`DirectDrawCreate` (ordinal 5), which is the export Impure *is*; **three** ordinals (1, 2, 4) from
`TPLAYX`, the same three DirectPlay ordinals retail imports from `DPLAYX`, which **1c already
redirects on these routes** (measured: 18 places into TADR become 15); and nine winmm names
(`waveOutGetVolume`, `auxSetVolume`, `PlaySoundA`, `mciSendStringA`, …) from `TMUSI`, which carries
no TADR marker, is a winmm wrapper of that distribution, and is left alone with its own start-up.
Escalation is the same shape (`TAESC`, `EPLAYX`, `EMUSI`). So 1d redirects one import slot and
reuses 1c for the other three.

**What a player on these routes gains and loses.** Every 3.9.02 feature — the unit limit, the
weapon IDs, the megamap, the recorder — comes from `tdraw.dll` and `tplayx.dll` at run time, not
from the exe, so stopping TADR's code takes them away and Impure's own raised limits
([TADR port A](../tadr-port/limits-evidence.md)) stand in their place. The exe's own bytes stay, which
is the rule. The recorder is not replaced: that is its own project ([Open](#open)).

**Still open in 1d**: TADR's `DllMain` read in two builds of nine, the other seven answered by the
fail-safe rather than by a measurement; the name the `LoadLibrary` call passes has to be a string
**inside a loaded module's image** for the argument to be followed at all — a name built on the
stack or on the heap is not, and the pass then does nothing (every build measured passes a
literal); and a **32-bit stack layout** throughout — the frame walk, the `ret 0Ch` and the SEH
record are all x86, which is what the engine is.

### Part 2 — sites a mod's exe file changes belong to the mod

**The reference for Impure's fail-closed table is the exe file on disk**, not the stock 3.1 bytes
it is built against. Both comparisons are made, in `DllMain`, before anything is written:

- **memory differs from the file** — something that ran before Impure rewrote that site, which is
  what this refuses over, as it always did;
- **the file differs from stock 3.1** — the mod's own change, since a mod ships its engine changes
  in its exe. That is refused too, because Impure's value there belongs to a fix whose argument
  rests on what the stock bytes do — **except** at a site marked `lim_file_ok`, where the value
  stands alone: nothing is sized or indexed by it, no stub reads it, no other site's argument
  depends on it. There the mod's bytes are kept and Impure writes nothing.

**One site is marked**: `0x0040EAD6`, the pathfinder's search budget. The 3.9.02 exe sets 66650,
which is what Impure writes anyway; Escalation's sets **1114**, below stock's 1333 and deliberately
so, and it is kept. Adding a site to that list is a claim about the site, made in a review, and
never a way past a refusal.

Comparing with the baked stock bytes alone could not tell those two cases apart, and so refused
every mod's own exe at its first changed site — which is what stopped these four setups from
starting at all. When the file cannot be read, or the exe is loaded away from the base its file
asks for, there is no reference: the table falls back to the stock bytes, says so in the log, and a
mod's own exe refuses there as before.

### Part 3 — the reference image, and the safety net behind it

At the first `DirectDraw` call — after every DLL's start-up and the exe's entry point, before the
first frame — Impure makes two comparisons, and either stops the game with a box naming what it
found rather than letting it crash when a battle loads.

The **reference image** (`tagpu_takeover_verify_image`) reads the exe file from disk and
compares every executable section of it with the same bytes in memory. Each run of changed
bytes is decoded for what a hook has to be — a rel32 call or jump (`E8`, `E9`), a call or jump
through a pointer inside the image (`FF 15`, `FF 25`), `push imm32; ret`, `mov eax,imm32; jmp
eax` — and a target inside a module of the game folder that carries TADR's marker refuses the
launch, naming the site, the bytes, the file's bytes and the module.

**Only an instruction that transfers control names a target.** Four bytes of changed code that
merely *hold* an address inside one of those modules are counted in the log and never refuse. The
bytes are as likely to be the middle of an instruction or the displacement of a jump, and which of
them look like an address depends on where the loader put a DLL that day — so a check that judged
them would refuse a different, arbitrary set of installs on every machine. MEASURED on Windows
2026-09-27, both halves of it, each on a setup where TADR had run *nothing* (entry point inert,
DirectPlay slots redirected, and the log saying so):

- `8B 96 92 00`, the middle of a `mov esi,[esi+0x92]` of Impure's, reads as `0x0092968B`, and
  Total Mayhem 11.3.0's recorder was mapped at `0x00910000` — 3 of Impure's own patch sites
  refused that install.
- a rel32 displacement of Impure's to a stub above the image is about `0x020F0000`, and the 2006
  recorder beside the retail exe was mapped at `0x020C0000` — 32 of our own sites refused
  `retail+tadr1`.

Wine maps both DLLs elsewhere and showed neither, which is what the Windows half of the suite is
for. Nothing is lost by not judging a held value: every TADR hook measured on any setup — 24 on
the 2006 recorder, 6 on the entry-point route, 13 to 18 of a `tdraw`'s — is found by its
instruction. `tacompat.py selftest` carries all three coincidences as cases that must not fire.

How much noise that rule silences is measurable, because the suite still counts what it will not
judge: over one Windows run of the sixteen setups, the changed runs held **43** such addresses on
`loader+tadr-tazero`, 19 on `escalation+tadr-dev`, 18 on `392+tadr-dev`, 11 on `retail+tadr-files`
and 0 on most of the rest — a different set on every setup and every platform, because it follows
where the loader put a DLL. Judged, each of those was a refused launch.

A decoded instruction is *narrower* evidence than a held value, not proof: nothing here disassembles,
so a stock byte that happens to be `E8` or `E9` inside a changed run is decoded as though it were an
opcode, and its target could in principle land inside a module of the folder. It is a far smaller
surface than a held value — the target has to fall inside one of a handful of image ranges rather
than merely resemble an address — and it has never fired: 0 in some 8 000 changed runs over 32
launches on the two platforms. If one ever does, the site will be one of Impure's own patch
addresses, which is how to tell it from TADR.

**Why this needs no list of sites, and exempts the mod without one.** A mod's own changes to
the engine are in its exe *file*, so they are not changed bytes at all. Impure's patches and
the Patch Loader's lead into Impure's module or into stubs Impure allocated, which are in no
module image. TADR's lead into TADR. **Only TADR refuses**, and a measurement is why: the
Patch Loader rewrites three of the exe's import thunks into direct calls to the mod's own
`win32.dll` — `0x004E4708`, `0x004E71A0` and `0x004EADF2`, `FF 15 00 C1 4F 00` becoming
`E8 rel32; nop` (MEASURED 2026-09-27, Total Mayhem 11.3.0 and ProTA 4.8) — and that is a byte
the mod itself sets. Both kinds go to the log; only TADR's count.

The same pass reads **every import slot of the exe**, which is the other way a call can leave the
image: the code that reaches a slot is stock, so the comparison never looks at it, and the file
holds no bound address to compare with — what makes a slot wrong is where it leads. A slot leading
into a TADR module refuses too, and that is also the answer to part 1c's own failure mode: a
descriptor it could not name every slot of still holds the recorder's addresses, and the recorder
is inert by then, so the exe's first DirectPlay call would run uninitialised Delphi code.
**The slots are judged even when the code comparison could not be made at all** — an exe someone
else holds open for writing, or one loaded away from its own `ImageBase` — because a slot does not
depend on the file. Discarding the slot verdict with the file verdict would have let exactly the
launch above proceed.

**One finding a changed run, and a TADR one wins it.** The bytes before a run can be a stock
`FF 15` through an import slot that leads into the mod's own `WIN32.dll`, which the retail exe
imports: taking that finding and stopping would hide a TADR hook in the same run behind a byte that
is allowed to be there. The opcode is looked for from **five** bytes before the first changed byte,
not four, because `FF 15`/`FF 25` carry their operand at offsets 2 to 5 — a repointed slot address
whose last byte alone differs begins five bytes back.

**What it does not see**: a hook installed by writing a function pointer into a data section other
than an import slot (the engine's globals differ from the file everywhere by the time this runs, so
comparing them would be noise); a hook whose target is computed at run time; a TADR build carrying
neither marker; and anything written *after* this call — which is why part 1 keeps TADR's code from
running at all rather than cleaning up after it. An exe loaded away from its own `ImageBase` has its
code left uncompared, and says so in the log; its import slots are still read.

The **safety net** (`tagpu_patches.c`, `lim_verify`) re-reads every site of Impure's
fail-closed table. It is the older and narrower of the two: it sees only *its own* sites, which
is why it never fired on the recorder's injections, and only start-up writes. It stays because
it names exactly which of Impure's fixes was overwritten, which the image check cannot.
Both refuse through `tagpu_refuse` — the report, `log\startup-failure.txt`, the box and
`ExitProcess(0xC1)` in one place.

## Landings

| landing | parts | setups it moves to the goal |
|---|---|---|
| **T1** — landed 2026-09-26 | 3 (the safety net), then 1a | none on its own terms: it kept `tdraw.dll` out of `loader+tadr-ota`, `loader+tadr-tazero`, `loader+tadr-mayhem`, `mayhem-11.3.0` and `prota-4.8`, but their recorder still ran, which the suite's goal did not check until T1b |
| **T1b** | 1c | none on its own: it keeps the recorder out of the game's DirectPlay, which is what it was measured to do, and does not stop the recorder, which takes the entry point instead. It is what makes T1c safe, since a recorder that cannot initialise must never be called |
| **T1c** — landed 2026-09-27 | 1b, 3 | `retail+tadr1`, `retail+tadr-files`, `loader+tadr-ota`, `loader+tadr-tazero`, `loader+tadr-mayhem`, `mayhem-11.3.0`, `prota-4.8`: no TADR code runs in them, alone or in a network game, read out of the running process on every peer, and a launch where any does refuses to start |
| **T2** — landed 2026-09-27 | 1d **and** part 2, in ONE landing | `392+tadr-dev`, `392+tadr-2026.8.6`, `escalation`, `escalation+tadr-dev`: Impure runs and none of TADR's code does, read out of the running process — 441 to 442 changed runs and **0 into TADR** on Wine, 438 to 451 and 0 on Windows. All four fight the 200v200 battle; the two 3.9.02 routes also play a two-player network game, and Escalation's play none, its battle room starting a game with no units on either peer ([the suite](suite.md)) |
| **T3** | distribution | `gammata-ota`: its tdraw loads only `ddraw_custom.dll`, so Impure is installed under that name too |

**T2 was one landing and not two** because part 2 alone changes nothing a player sees: stop
refusing the exe file's own path budget and these four launches still refuse, over the 15 of 159
fail-closed sites TADR rewrites, one step later. The landing's claim is the one that matters —
those four setups meet their goal, read out of the running process — so both parts were in it,
with one review and one suite run a platform.

## Open

- **Replacing the recorder**: demos, TA Forever's replays, its in-game commands — Impure's own,
  a project of its own ([the recorder's shipped features](../tadr-merge-exploration.md#the-shipped-features-by-port-group)).
- **What each mod's content takes from TADR**: its data keys in unit and weapon files (some
  already ported, [TADR port C](../tadr-port/data-keys.md)), its UI features. A setup that meets
  its goal starts Impure with TADR out and fights the suite's battle; it does not show that the
  mod's content lost nothing. That is a survey per mod, not yet made for Total Mayhem, ProTA or
  TA Zero.
- **Multiplayer with players who still run TADR** is not a goal: the mods' battlerooms check the
  version bytes, not the DLLs, so such a game may start and then disagree.
- **What the reference image cannot see** (part 3): a hook installed by writing a function pointer
  into a data section other than an import slot, one whose target is computed at run time, a TADR
  build carrying neither marker string, and anything written after the first `DirectDraw` call. The
  first three are why part 1 stops TADR's code rather than trusting the check; the fourth is what
  part 1 is for.
- **Anti-virus says nothing to any of it.** The comparisons are inert — file reads and reads of our
  own memory — and the writes into the host exe's code are what Impure has always done. New in kind
  is the eight bytes written over **another DLL's** PE entry point, which is a shape
  behaviour-based engines watch; it is made with `PAGE_READWRITE` rather than
  `PAGE_EXECUTE_READWRITE`, since nothing runs that module's code while the bytes go in, so the
  page is never writable and executable at once. MEASURED on the Windows test box 2026-09-27, three
  full runs of all sixteen setups, Defender armed (`RealTimeProtectionEnabled`, `AntivirusEnabled`,
  `AMServiceEnabled`, `BehaviorMonitorEnabled` all true; platform 4.18.26080.4, signatures
  1.459.421.0): **no detection at all** — 0 events of ids 1006–1009, 1015 or 1116–1119 in
  `Microsoft-Windows-Windows Defender/Operational`, and nothing in `Get-MpThreatDetection`. What the
  log does hold over the same hours is the hourly health report (1150/1151) and six id 2050
  submissions, each of an `amsistream-…`: **AMSI script content — the harness's own PowerShell over
  SSH, uploaded for analysis.** Not one event names `TotalA.exe`, `ddraw.dll` or any TADR DLL. Two
  things follow: the writes into the engine and into a TADR DLL's entry point drew nothing, and our
  own automation scripts are what gets sampled, so anything secret has no business in one. That is
  one machine with one engine and one signature set, so it is evidence and not a guarantee. The
  suite's `OpenProcess`/`ReadProcessMemory` is the most anti-virus-shaped code in the change and
  ships to nobody — it is in `win-watch.ps1`, on our own machine.
  **Counting the right ids matters**: id 5007 is Defender's own configuration change and id 2050 a
  sample upload; a first pass that counted both read 65 "detections" that were nothing.
