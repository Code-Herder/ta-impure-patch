# The compat suite (tacompat)

## Summary

`tools/compat/tacompat.py` builds every [setup](setups.md), starts it, watches every window the
game opens, reads what each party logs, and compares the outcome with the setup's goal and its
accepted behaviour today. It runs all setups at once on Wine, with a 200-against-200 skirmish
wherever the menu comes up and a two-player network game wherever Impure runs, and one at a time
on the Windows test box. Every run is also read for **any of TADR's code having run**: the goal
of a player's setup is Impure running with none of it. **Nothing may be
UNEXPECTED before a release.** The `ta-compat-check` skill has the whole loop and its traps.

## Running it

- `list` and `fetch`: fixtures with a URL download; the ones behind a browser check are fetched
  in a browser and imported with `fetch --import NAME=PATH`, which checks every member's md5.
  A mod as players have it is what TA Forever installs: its API
  (`api.taforever.com/data/featuredMod`, `installPackage`) lists each mod's packages in order on
  plain URLs, and TA Zero's and TA Twilight's setups layer one fixture per package in that order.
- `wine`: every setup at once. Each gets its own `tacli` instance (`compat-<setup>`), private
  copies of the registry hives, its own Xvfb display, and the overlay of its fixtures on the
  retail gamedir. The runner watches the display's windows for the whole run (a refusal is a
  box, and a box is the evidence), reads `tdrawlog.txt`, `tagpu.log`, `startup-failure.txt`,
  `ErrorLog.txt` and the Wine `+loaddll` trace, then fights the battle where the menu came up,
  and last — while the game is still alive — reads its code out of the process (below). Two
  checks hold the claim to what a player sees, not only to Impure being up. **The mod's own
  content**: at the menu, the unit types a setup's `content` names (ones only that mod defines)
  must be in the engine's table of loaded types (`tacli units`, a walk of the table in memory,
  not of any archive path). **The units are seen**: after the battle's scenario, the newest
  `units: alive=N` header in `tagpu.log` — the frame packet, what Impure draws — must hold nine
  in ten of the units the applier made. MEASURED 2026-09-27: Escalation made 401 and Impure saw
  0, which every earlier check had passed. Every game runs silent but with a sound device: Wine is kept off
  PulseAudio and its ALSA driver opens a null device (`asound-null.conf`), because TA:ESC with no
  device at all stops at "No sound driver is available for use."; no file of the setup changes.
- **The network stage** (`wine --mp SECONDS`, 30 by default, 0 for none): for each player's
  setup whose single-player run showed Impure running, a second instance of the same folder
  (`compat-<setup>-j`), both games on their own displays, hosted and joined through the game's
  own battle room over Windows' DirectPlay (the walk of `tools/mp_lobby.sh`), each side applying
  its half of a small fight (`scenarios/compat-mp-host.json`, `compat-mp-join.json`), then
  watched on both. Each game is queued as its single-player result comes in, and `--mp-jobs`
  (4 by default) play at once, **each on a DirectPlay port of its own**: the name server binds
  its port for the whole machine, so the suite patches a port from 47625 upward into both peers'
  prefixes (`tools/dpport.py`, the same patch `tacli launch --dplay-port` makes) and the two
  meet there and on no other game's. A holder of a game's port in one of the suite's own
  prefixes is stale and ended; any other is someone else's game, waited for five minutes and
  never touched, and then that game fails as not run. "Its own" is the set of
  prefixes **this run** created, not every `compat-` name: the instance names are fixed, so a
  session in another worktree runs prefixes named exactly like ours
  (`parallel-mp-runs-share-dplay-port`). A setup whose battle room this walk cannot reach a game
  in says so in `no_network_game`, in words and from a run, and plays none: the report's column
  reads `no mp`. No setup carries it today. Escalation's two did while its games started with **no
  units on either peer**; the units were there and the sim ran them, but every pass of the DLL
  filtered pointers at 2 GB and ESC's exe is large-address-aware, its heap above that
  ([exe-reverse-engineering](../exe-reverse-engineering.md), *A large-address-aware exe's heap
  is above 2 GB*). Both now play their network game (MEASURED 2026-09-27).
- `windows`: one setup at a time on the Windows test box, over SSH, with the game and a window
  watcher (`win-watch.ps1`) started as scheduled tasks in the logged-on session, because a
  process started over SSH cannot see the desktop's windows. Every setup runs from a work folder
  at a **path Windows has not started the exe from before**: the first start from a new path is
  the one Windows' compatibility engine hooks the exe's `DirectDrawCreate` import on, and a folder
  reused across setups tests only later starts — v0.3 refused every fresh Total Mayhem install
  while this runner, with one folder, passed ([the takeover](takeover.md), part 1).
- `selftest`: the hook decode against every shape it claims to decode and every byte pattern it
  must not judge on, with no game and no fixtures — eleven cases, each built from something that
  was measured: the Patch Loader's own thunk rewrite, a plain immediate, the two coincidences that
  refused a launch on the Windows box, and a run holding a call into the mod's own DLL *and* a jump
  into TADR, where the TADR one has to win. It is the only check of `decode_run` that does not need
  a running game, and both platforms' verdicts rest on it.

A run is **meets goal**, **known gap** (matches `today`) or **UNEXPECTED**; the exit status is
1 on any UNEXPECTED. The rule: no UNEXPECTED before a release.

**Every player gets a commander, in both games.** Before any scenario touches the map, the
skirmish reads the engine's own unit array (`tacli units`, the catalogue's `commanders` and
`unit_owners`, `tagpu_cat.c`): every living unit whose type has the FBI `commander` bit
(`UnitDef+0x245` bit `0x40000`, [engine map](../exe-reverse-engineering.md)), and every owner
with a living unit. At a game's start a player's units are its start units, so every owner must
hold a commander, and there must be two at least. The network game checks the same on **each
peer**, so a peer that never received the other player's commander fails too. It is not read from
the roster: that is what Impure draws, and the other player's commander starts under the fog.
The battle scenario clears the map (`clear_existing`), which is why the check comes first.
MEASURED 2026-09-28: retail's saved skirmish has three players (ARMCOM, and two CORCOM for the
AIs); TA Zero's starts a GoK and an Arm commander; each network peer holds both.

**A mod without the stock unit types names its own scenarios.** The battle (`200v200`) and the
network game's two halves (`compat-mp-host`, `compat-mp-join`) place stock types, which every mod
the suite runs keeps except TA Zero, whose factions are redesigned from scratch (no `ARMPW`, no
`ARMCOM`). A setup's `scenarios` names its own for the three roles — `tazero-alpha5` uses
`compat-battle-tazero` and `compat-mp-host-tazero`/`-join-tazero`, the same fights in TA Zero's
types. The walk knows the game screen by its in-game panel, `<side>MAIN.GUI`, for any side: TA
Zero's third is GoK, and a hard-coded `ARMMAIN|CORMAIN` never saw a GoK player's game start.

**The UI layer is part of every verdict.** A run whose Vulkan UI layer gave up following the game
(it then composites nothing: a black screen) or asked for a fresh start past the start-up is
UNEXPECTED, read from the layer's own log lines. Every other check here reads state and gadgets, so
before this a black screen passed: every Escalation run gave up on 2026-09-27 and no row said so.
The `gui-stress` setup drives the race on purpose, a fresh start every 120 presents while colour is
valid, and passes only if the layer never asks for one of its own and its fallback is seen to run
([GPU status](../gpu-status.md), *Two colour disagreements are neither*).

## Whether TADR ran

`tadr_ran` in a setup's goal is judged two ways, on every peer of every stage: from what TADR
leaves in the game folder, and — since 2026-09-27 — from **reading the running game's own code**.
The second is what the first cannot give.

**From the game folder.**

- **`tdraw.dll`** writes `tdrawlog.txt` from its `DllMain`: the file at all means it started
  (the result says too whether it installed its engine patches). **On the routes where TADR's own
  `DllMain` is what loads Impure** — the 3.9.02 exe, Escalation, TA Zero, TA Twilight — it has
  written that first line before Impure exists, so the file cannot be absent there however
  completely the takeover stops the rest. Those setups carry `tadr_started` in their goal, which
  allows that one line and nothing else — `<n> --- Process Attached`, followed by `.  config=<name>`
  in the builds from 2026 and by nothing in TA Zero's 2025 build: an engine patch installed, a recorder log, or any site of the game's code leading
  into TADR still fails them.
- **The recorder** (`tplayx.dll`, or the 2006 `dplayx.dll`) writes `log\TA Demo Recorder Log
  -<date>.txt` **only when it starts from inside one of its DirectPlay exports**. Any such log
  written during the run means it ran; the absence of one does **not** mean it did not, because
  its other way in — the jump it splices over the exe's entry point — writes no log at all
  ([the takeover](takeover.md), part 1). A demo file is no evidence either way: the 2006
  recorder wrote none in a network game while it ran.

**From the running process** (`exe_hooks`). The exe's executable sections are read out of the live
game and compared with `TotalA.exe` on disk; every run of changed bytes is decoded for the address
it leads to (`decode_run`: `E8`/`E9` rel32, `FF 15`/`FF 25` through a pointer, `push imm32; ret`,
`mov eax,imm32; jmp eax`), and that address is tested against every module loaded from the game
folder. **A verdict rests on an instruction that transfers control, never on four bytes that
merely hold an address** -- those are reported as held and not counted, because the bytes are as
likely to be the middle of an instruction or the displacement of a jump ([the takeover](takeover.md),
part 3, where both halves of that are measured). A target in a module whose file carries
`TADemo-MKChat` is TADR's code having run; a target in any other DLL of the folder is a byte the
mod itself sets and is reported, never counted — the Patch Loader rewrites three of the exe's
import thunks into direct calls to the mod's `win32.dll`. One finding a run, and **a TADR target
wins the run**: the bytes before a changed run can be a stock `FF 15` through an import slot that
leads into the mod's own `WIN32.dll`, and taking that one would hide a hook behind it.

**The exe's import slots are read too, on Wine.** The layout comes from the exe file (`pe_imports`:
each descriptor's first slot and how many), the bound values from the process, and a slot leading
into a TADR module counts exactly as a site of the code does — it is the other way a call leaves
the image, and the code that reaches it is stock, so the comparison of the code cannot see it. A
slot bound into the very DLL its descriptor names is **not a change at all** and is left out: the
retail exe's own imports of `WIN32.dll` and `smackw32.DLL` are sixteen such slots on every setup,
and reporting them would bury the one that matters. The exception is a descriptor whose own DLL is
TADR's, which is precisely the recorder installed as the game folder's `dplayx.dll`. The Windows
watcher does not read the slots at all; a row whose slots were not read says so.

The decode is deliberately the same set of shapes `tagpu_takeover.c` decodes in-process, down to
the five bytes of lookback before a changed run, so the suite and the DLL answer the same question
and may be compared. One divergence is left and is deliberate: the DLL reads an `FF 15`/`FF 25`
pointer only where the read is safe by construction — inside the exe's image or inside a
game-folder module — while the suite, reading from outside, reads any address.

**A goal of `tadr_ran: false` is not met by a comparison that did not happen.** When the read
fails — no process to read, the exe mapped away from its `ImageBase`, `ptrace_scope` refusing — or
when no read was attempted at all,
the row says so and reads UNEXPECTED. It is not counted as TADR having run: it is counted as
unproven, which is the same rule as *a run is only evidence if the watcher saw it*.

- **On Wine** the read is `/proc/<pid>/mem` of the game. Only descendants of the pid the runner
  started are looked at (`wine TotalA.exe` maps the PE in that process or in a child of it, and a
  `ptrace_scope` of 1 lets a process read its own descendants and nothing else, so another
  session's game can never be read). A module's extent is the `SizeOfImage` in its PE header read
  out of the process, because the maps show a PE image's header page alone. A module of the game
  folder is found by **where its file really is**, not by the folder's path being a prefix: tacli
  links the retail install's own files into an instance instead of copying them.
- **On Windows** `win-watch.ps1` does the reading — `ReadProcessMemory` from the 32-bit
  PowerShell, since a 64-bit one lists only the exe among a 32-bit game's modules — and **judges
  nothing**: it emits the changed runs with a little context each side, and the module table with
  each module's extent and whether its file carries the marker, for the same `decode_run` to
  decode. Its one gap is the indirect call form, which needs a read at an arbitrary address the
  watcher does not make. It has run over all sixteen setups (2026-09-27), and the rule above is
  what it bought: Wine and Windows map a DLL to different addresses, so a coincidence that fires
  on one machine fires on nothing else, and only a run on the other platform shows it.

`retail+tadr1-recorder` is the check of both kinds of evidence at once: the 2006 recorder with the
takeover switched off (`tagpu_takeover.off`), whose goal is `tadr_ran: true`. MEASURED 2026-09-27:
its log appears *and* the process read finds **24 sites** of the game's code leading into the
recorder's module on Wine, where a battle and a network game let its DirectPlay path run as well,
and the 6 the entry point alone installs on the Windows box, which stops at the main menu — against
0 on every setup where the takeover is on.

## Adding a setup

Every third-party file is a fixture in `tools/compat/fixtures.json`, named by md5; nothing
third-party is in the repository. A setup in `tools/compat/setups.json` lists the fixtures it
overlays on the retail folder, its `goal`, and, while it falls short, its `today` — written from
a run, not from expectation. The commit that makes a setup meet its goal deletes its `today`.
A Patch Loader that moves the registry (`RegistryPath=`) needs the setup's `registry_roots`, or
the battle stage opens on a map the mod does not ship. `levers` are empty files the setup puts in
the game folder — a DLL switch. A setup with one checks a mechanism rather than a player's
folder, and `content` names three unit types only the mod defines (see above): every setup that
ships a mod's own content carries one. `mayhem-11.3.0-net` switches the takeover off (`tagpu_takeover.off`) so that the safety
net has something to catch, and its goal is the net's refusal. `no_network_game` holds a setup out
of the network stage with its reason, measured (above); none does today.
