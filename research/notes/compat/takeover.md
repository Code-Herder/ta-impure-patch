# The takeover (plan)

## Summary

The owner's direction (2026-09-26): mods run **as is, with their own exe and files**, and Impure
is the only thing that patches the engine. One rule for every setup — **stop TADR's code, keep
every byte the mod itself sets** — reached by one of two ways in, chosen by which DLL loads
first, plus a safety net that turns any route nobody foresaw into a clear refusal instead of a
crash like [Total Mayhem's](tadr-collision.md). The [suite](suite.md) is the gate: a landing
here is done when its setups meet their goal on Wine and Windows and their `today` entries are
deleted.

**Where it stands (2026-09-27).** `tdraw.dll` is kept out on the Patch Loader route and the
safety net is in (T1). **TADR's recorder is not: it still runs, and still patches the engine,
on every Patch Loader setup and beside the 2006 recorder** — it takes the exe's entry point,
which T1b's DirectPlay work does not touch (part 1). Both halves of the answer are the shape
the owner set out on 2026-09-27, and they are the same shape for part 2: **the exe file on disk
is the reference**, TADR's DLLs run none of their own code, and every launch compares the whole
exe image against that file and refuses to start when a changed byte leads into a TADR DLL.
A list of hook sites is not the answer — it is a guess about what TADR does, and the recorder
finding is what that guess costs.

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

1. **Impure loads first** — the retail exe, and every Patch Loader mod (Total Mayhem, ProTA,
   TA Zero). Impure's `DllMain` runs `hook_init` (`dllmain.c`), which points the `LoadLibrary`
   imports of every module in the game folder at the fork's `fake_LoadLibrary*`
   (`winapi_hooks.c`), the loader's `dplayx.dll` included, before the loader's own `DllMain`
   runs (`tagpu_takeover.c`, [§2.6d](../gpu-status.md#26d-keeping-tadr-out-tagpu_takeoverc-on-tagpu_takeoveroff)):
   the exe imports `DDRAW` first and `DPLAYX` eighth, and on Wine and on Windows (the suite,
   2026-09-26, Total Mayhem and ProTA) Impure's limits were installed before TADR's limit crack
   ran. Where the order were the other way, TADR would have patched before Impure started, and
   Impure's existing stock check refuses. When a module asks for a DLL in the
   game folder whose export table carries `DirectDrawCreate` and which is not Impure, Impure
   answers with its own module. TADR's `DllMain` — where it installs everything
   (`vendor/TADR/src/DDraw/ddraw.cpp`) — never runs; the loader's `GetProcAddress` finds
   Impure's `DirectDrawCreate`, and its `patch_call` at `0x47BFA2`/`0x4B55FB` points the exe's
   two calls at Impure directly. The test is the file's exports, not its name, because the
   mods rename it.

   **The recorder** cannot be kept from loading on this route: the loader's `dplayx.dll`
   *forwards* its DirectPlay exports to `tplayx.dll` (`vendor/Total-Annihilation-Patch-Loader/
   exports.def`), and Windows resolves a forwarder while it binds the exe's imports, before any
   `DllMain` and without a `LoadLibrary` anyone can answer. **It is not kept from running
   either, and that is this part's open hole.** T1b closed one of its two ways in: Impure's
   `DllMain`, after every import is bound, points every slot of an import descriptor that leads
   into a DirectPlay DLL of the game folder at its own forwarders, which load Windows'
   `dplayx.dll` by its full path on their first call — so the path that starts the recorder
   from inside a DirectPlay export (`vendor/TADR/src/Recorder/Dplayx_exports.pas`, `OnInit` →
   `OnInitialize(false)`: the log, Windows' DirectPlay, then the code injections) is never
   taken, and TotalA.exe reaches DirectPlay only through those three import slots ([the engine
   map](../exe-reverse-engineering.md), *Where other patchers meet ours*). The same holds for
   the 2006 recorder beside the retail exe, which *is* the game folder's `dplayx.dll`.

   The way in that stays open is the **entry point**. The recorder's `DllMain` splices a jump
   over `0x004E6FA0` and installs the same code injections from there, through
   `OnInitialize(true)` — no DirectPlay call, and no log, because the log is created only on
   the other path (`vendor/TADR/src/Recorder/InitCode_CoreExePatching.pas`, whose unit
   `initialization` runs in `DllMain`, and `InitCode.pas`; [the recorder's
   page](../deep-tadr.md)). MEASURED 2026-09-27, `retail+tadr1` on Wine, read out of the live
   process: **seven sites of the game's code hold a jump or call into the recorder** where the
   retail control holds stock bytes, and `0x004E6FA0` is stock again by then — the recorder
   restores it after running. The safety net does not fire on them: none is one of Impure's
   265 sites, and the recorder writes Impure's own value at the three unit-limit sites. Every
   recorder build the suite carries has this code in its source; only `retail+tadr1` has been
   read in memory.

   So on every Patch Loader setup, and beside the 2006 recorder, **TADR's recorder still runs
   and still patches the engine after Impure.** What closes it is the reference-image shape
   the owner set out (2026-09-27): the exe file on disk is the reference, TADR's DLLs run none
   of their own code, and every launch compares the whole exe image against the file and
   refuses to start when a changed byte leads into a TADR DLL — one rule with no list of hook
   sites in it, since a list is a guess about what TADR does and this finding is the proof.
   Not designed in code yet.
2. **The exe imports TADR** — the 3.9.02 exe (`TDRAW`), Escalation (`TAESC`). TADR's `DllMain`
   loads `ddraw.dll` first thing, so Impure's `DllMain` runs nested inside it, before TADR
   patches anything: Impure snapshots the exe's code, lets TADR's start-up finish, and at the
   exe's entry point puts back every code byte TADR wrote since the snapshot and points the
   exe's TADR import slots at its own exports. The snapshot and the exe file on disk are the
   same reference here, which is what makes the mod's own bytes safe: a mod's exe carries its
   changes in the file, so they are in the snapshot, and only what TADR wrote *after* it is put
   back. Data-section writes stay unless they land on an Impure site (resource paths are data).
   Not designed in detail yet: TADR's threads and window hooks started in its `DllMain`, and the
   entry point is contested — the recorder splices a jump over `0x004E6FA0` from its own
   `DllMain` (part 1), so whatever runs there has to survive sharing it.
3. **Sites a mod's exe changes belong to the mod.** Impure's fail-closed table compares with
   stock 3.1 today and refuses Escalation's exe at `0x40EAD6`. It will compare with the exe
   file on disk instead: a site the file itself changes is the mod's, and Impure leaves it —
   or refuses, where another of its patches depends on the stock bytes there.
4. **The safety net.** At the first `DirectDraw` call — after every DLL's start-up, before the
   first frame — Impure re-reads every site of its fail-closed table. A site someone else has
   rewritten stops the game with a box naming it, instead of letting it crash when a game
   loads. It sees start-up writes only: a hook TADR installs after `DirectDrawCreate` is
   outside it, which is why parts 1 and 2 keep TADR's code from running at all rather than
   cleaning up after it. And it sees only **its own** sites, which is the other half of why:
   the recorder's entry-point injections happen before this call and it does not fire on them,
   because they land elsewhere in the engine (part 1). Comparing the whole image against the
   exe file, rather than 265 sites against remembered bytes, is what the owner's reference-image
   rule asks for.

## Landings

| landing | parts | setups it moves to the goal |
|---|---|---|
| **T1** — landed 2026-09-26 | 4, then 1 (`tdraw.dll`) | none on its own terms: it kept `tdraw.dll` out of `loader+tadr-ota`, `loader+tadr-tazero`, `loader+tadr-mayhem`, `mayhem-11.3.0` and `prota-4.8`, but their recorder still ran, which the suite's goal did not check until T1b |
| **T1b** | 1 (the recorder's DirectPlay path) | none. It keeps the recorder out of the game's DirectPlay, which is what it was measured to do; it does not stop the recorder, which takes the entry point instead (part 1). The six setups it was written for still run TADR's recorder |
| **T1c** | 1 (the recorder itself) | `retail+tadr1`, `loader+tadr-ota`, `loader+tadr-tazero`, `loader+tadr-mayhem`, `mayhem-11.3.0`, `prota-4.8` — the reference-image rule of part 1: no TADR code runs in them, alone or in a network game, and a launch where any does refuses to start. Not designed in code yet |
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
