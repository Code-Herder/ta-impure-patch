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
routes where TADR loads Impure are still open (parts 2 and 3, landings T2 to T4).

The shape is the owner's, set out on 2026-09-27: **the exe file on disk is the reference**, TADR's
DLLs run none of their own code, and every launch compares the whole exe image against that file
and refuses to start when a changed byte leads into a TADR DLL. A list of hook sites is not the
answer — it is a guess about what TADR does, and the recorder finding is what that guess cost:
T1b was built on the premise that the recorder does nothing until its first DirectPlay export is
called, and the recorder takes the exe's entry point instead.

## What stays, what goes

**Stays — the mod's content, whoever writes it** (the exe file, the Patch Loader, or a TADR
build):

- **Resource paths**: the registry key (`RegistryPath=`), the ini and GP3 names, the download
  folder, renamed data folders (`gamedatP`, `unitsM`, `WeaponM`, `guiM`, …).
- **The mod's multiplayer identity**: its version string and the battleroom version bytes
  `0x49E9C0`/`0x49E9C9`, which all players must match.
- **The mod's gameplay patches**: the AI fixes, target acquisition, teleport, reclaim rules,
  `+AI`/`+Control` levels, allied victory — the mod's rules. Where one lands on an Impure site
  the plan is that the mod's value wins (part 3 does it for the exe file's bytes). Today the
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

## The four parts

### Part 1 — Impure loads first

The retail exe, and every Patch Loader mod (Total Mayhem, ProTA, TA Zero). Three passes keep
TADR's code from running, one for each way it gets in, and part 4 below is the check that
decides. All four are in `tagpu_takeover.c`, whose header carries the contract;
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
in the log, and part 4 is what answers for such a launch. That is also why part 2 is a separate
design rather than this pass applied twice. Every slot of a descriptor is looked at, not its first,
so a descriptor one unbindable slot would have hidden still counts.

**What the invariant does not cover**, and the log would be wrong about: a game-folder module that
is in the exe's import table not at all — pulled in as a dependency of an earlier descriptor's
module, or by a forwarded export. Such a module is already initialised when this runs, and its
entry point is made inert anyway; only part 4 answers for it. No setup of the suite has one
(MEASURED 2026-09-27, `objdump -p` over every fixture's exe and DLLs). Nor does it cover a **PE TLS
callback**, which the loader calls whatever the entry point says: both `tdraw.dll` builds of the
fixtures carry a TLS directory (Mayhem RVA `0x6E240`, ProTA `0x81F00`) whose callback array begins
with NULL, so nothing runs today, and a build that ever carries one is named in the log instead of
passing for silent.

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

**What it measures to.** One full suite run a platform on this DLL, 2026-09-27, read out of the
live process: **11 setups of 16 meet the goal, 5 are known gaps, none is UNEXPECTED, on Wine and on
Windows alike.** `retail` 431 changed runs in `.text` and nothing leading out of the exe;
`retail+tadr1`, `retail+tadr-recorder-ota`, `retail+tadr-files`, the three Patch Loader setups,
`mayhem-11.3.0` and `prota-4.8` **0 sites leading into TADR** — alone, in a 200v200 battle and in a
two-player network game, on every peer — where the same read on the T1b DLL found 6 hooks into the
2006 recorder's module and the game folder showed nothing at all. Mayhem and ProTA report 3 sites
each into the mod's own `win32.dll`, which stay. With `tagpu_takeover.off` the 2006 recorder
installs 6 by start-up and **24** once a battle and a network game let its DirectPlay path run,
which is the proof the check can see what the game folder cannot show.

### Part 2 — the exe imports TADR

The 3.9.02 exe (`TDRAW`), Escalation (`TAESC`). TADR's `DllMain` loads `ddraw.dll` first thing,
so Impure's `DllMain` runs nested inside it, before TADR patches anything: Impure snapshots the
exe's code, lets TADR's start-up finish, and at the exe's entry point puts back every code byte
TADR wrote since the snapshot and points the exe's TADR import slots at its own exports. The
snapshot and the exe file on disk are the same reference here, which is what makes the mod's own
bytes safe: a mod's exe carries its changes in the file, so they are in the snapshot, and only
what TADR wrote *after* it is put back. Data-section writes stay unless they land on an Impure
site (resource paths are data). Not designed in detail yet: TADR's threads and window hooks
started in its `DllMain`, and the entry point is contested — part 1b makes a TADR module inert
before the loader calls it, so nothing of the recorder's is at `0x004E6FA0` on the routes that
pass covers, but on this one TADR is already running when Impure arrives.

### Part 3 — sites a mod's exe changes belong to the mod

Impure's fail-closed table compares with stock 3.1 today and refuses Escalation's exe at
`0x40EAD6`. It will compare with the exe file on disk instead: a site the file itself changes is
the mod's, and Impure leaves it — or refuses, where another of its patches depends on the stock
bytes there.

### Part 4 — the reference image, and the safety net behind it

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
| **T1** — landed 2026-09-26 | 4 (the safety net), then 1a | none on its own terms: it kept `tdraw.dll` out of `loader+tadr-ota`, `loader+tadr-tazero`, `loader+tadr-mayhem`, `mayhem-11.3.0` and `prota-4.8`, but their recorder still ran, which the suite's goal did not check until T1b |
| **T1b** | 1c | none on its own: it keeps the recorder out of the game's DirectPlay, which is what it was measured to do, and does not stop the recorder, which takes the entry point instead. It is what makes T1c safe, since a recorder that cannot initialise must never be called |
| **T1c** — landed 2026-09-27 | 1b, 4 | `retail+tadr1`, `retail+tadr-files`, `loader+tadr-ota`, `loader+tadr-tazero`, `loader+tadr-mayhem`, `mayhem-11.3.0`, `prota-4.8`: no TADR code runs in them, alone or in a network game, read out of the running process on every peer, and a launch where any does refuses to start |
| **T2** | 3 | none alone; Escalation's exe stops being refused at `0x40EAD6` |
| **T3** | 2 | `392+tadr-dev`, `392+tadr-2026.8.6`, `escalation`, `escalation+tadr-dev` |
| **T4** | distribution | `gammata-ota`: its tdraw loads only `ddraw_custom.dll`, so Impure is installed under that name too |

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
- **What the reference image cannot see** (part 4): a hook installed by writing a function pointer
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
