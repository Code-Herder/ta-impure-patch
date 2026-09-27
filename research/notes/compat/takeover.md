# The takeover (plan)

## Summary

The owner's direction (2026-09-26): mods run **as is, with their own exe and files**, and Impure
is the only thing that patches the engine. One rule for every setup — **stop TADR's code, keep
every byte the mod itself sets** — reached by one of two ways in, chosen by which DLL loads
first, plus a safety net that turns any route nobody foresaw into a clear refusal instead of a
crash like [Total Mayhem's](tadr-collision.md). The [suite](suite.md) is the gate: a landing
here is done when its setups meet their goal on Wine and Windows and their `today` entries are
deleted.

## What stays, what goes

**Stays — the mod's content, whoever writes it** (the exe file, the Patch Loader, or a TADR
build):

- **Resource paths**: the registry key (`RegistryPath=`), the ini and GP3 names, the download
  folder, renamed data folders (`gamedatP`, `unitsM`, `WeaponM`, `guiM`, …).
- **The mod's multiplayer identity**: its version string and the battleroom version bytes
  `0x49E9C0`/`0x49E9C9`, which all players must match.
- **The mod's gameplay patches**: the AI fixes, target acquisition, teleport, reclaim rules,
  `+AI`/`+Control` levels, allied victory — the mod's rules. Where one lands on an Impure site
  the mod's value wins. Measured today: Mayhem's loader writes the path budget at `0x40EAD6` with
  Impure's own value (66 650); Mayhem and ProTA write `B0 01` at `0x4266A5`, beside Impure's
  `EB` at `0x4266A7`, both removing the DirectX box; the 3.9.02 and Escalation exes carry their
  own path budget in the file.
- **Values poked into spare bytes for TADR to read** (Mayhem's click-snap radii at `0x101F0A`):
  harmless, nothing reads them without TADR.

Impure already follows the moved paths: what it reads of the game's settings it reads from
engine memory, not from a registry path. Only tacli's test-mode registry store assumes the
stock key (`tagpu_regstore.c`), which is why the suite writes a mod's key with `wine reg`.

**Goes — TADR's code**: `tdraw.dll` and its renamed builds (`mdraw`, `zdraw`, `TAESC`), with
everything they install: the limit crack, EngineLimits, the bug fixes, the megamap, the chat and
income overlays, the anticheat hashing. What a mod's content may still need from them is a
per-mod check (below).

## The four parts

1. **Impure loads first** — the retail exe, and every Patch Loader mod (Total Mayhem, ProTA,
   TA Zero). Impure's `DllMain` runs `hook_init` (`dllmain.c`), which points the `LoadLibrary`
   imports of every module in the game folder at the fork's `fake_LoadLibrary*`
   (`winapi_hooks.c`), the loader's `dplayx.dll` included, before the loader's own `DllMain`
   runs: the exe imports `DDRAW` first and `DPLAYX` eighth, and on Wine and on Windows (the suite,
   2026-09-26, Total Mayhem and ProTA) Impure's limits were installed before TADR's limit crack
   ran. Where the order were the other way, TADR would have patched before Impure started, and
   Impure's existing stock check refuses. When a module asks for a DLL in the
   game folder whose export table carries `DirectDrawCreate` and which is not Impure, Impure
   answers with its own module. TADR's `DllMain` — where it installs everything
   (`vendor/TADR/src/DDraw/ddraw.cpp`) — never runs; the loader's `GetProcAddress` finds
   Impure's `DirectDrawCreate`, and its `patch_call` at `0x47BFA2`/`0x4B55FB` points the exe's
   two calls at Impure directly. The test is the file's exports, not its name, because the
   mods rename it.
2. **The exe imports TADR** — the 3.9.02 exe (`TDRAW`), Escalation (`TAESC`). TADR's `DllMain`
   loads `ddraw.dll` first thing, so Impure's `DllMain` runs nested inside it, before TADR
   patches anything: Impure snapshots the exe's code, lets TADR's start-up finish, and at the
   exe's entry point puts back every code byte TADR wrote since the snapshot and points the
   exe's TADR import slots at its own exports. Data-section writes stay unless they land on an
   Impure site (resource paths are data). Not designed in detail yet: TADR's threads and window
   hooks started in its `DllMain`.
3. **Sites a mod's exe changes belong to the mod.** Impure's fail-closed table compares with
   stock 3.1 today and refuses Escalation's exe at `0x40EAD6`. It will compare with the exe
   file on disk instead: a site the file itself changes is the mod's, and Impure leaves it —
   or refuses, where another of its patches depends on the stock bytes there.
4. **The safety net.** At the first `DirectDraw` call — after every DLL's start-up, before the
   first frame — Impure re-reads every site of its fail-closed table. A site someone else has
   rewritten stops the game with a box naming it, instead of letting it crash when a game
   loads. It sees start-up writes only: a patch made later (the recorder at its first
   DirectPlay call, a hook TADR installs after `DirectDrawCreate`) is outside it, which is why
   parts 1 and 2 keep TADR's code from running at all rather than cleaning up after it.

## Landings

| landing | parts | setups it moves to the goal |
|---|---|---|
| **T1** | 4, then 1 | `loader+tadr-ota`, `loader+tadr-tazero`, `loader+tadr-mayhem`, `mayhem-11.3.0`, `prota-4.8` |
| **T2** | 3 | none alone; Escalation's exe stops being refused at `0x40EAD6` |
| **T3** | 2 | `392+tadr-dev`, `392+tadr-2026.8.6`, `escalation`, `escalation+tadr-dev` |
| **T4** | distribution | `gammata-ota`: its tdraw loads only `ddraw_custom.dll`, so Impure is installed under that name too |

## Open

- **The recorder** (`tplayx.dll`, loaded by the Patch Loader's forwarders and by the 3.9.02
  exe's import): demos, and TA Forever's replays. Keeping it out means pointing the exe's
  DirectPlay imports at the system's `dplayx.dll` and neutering its entry point. The owner's
  decision; until then the safety net is what guards against its start-up writes.
- **What each mod's content takes from TADR**: its data keys in unit and weapon files (some
  already ported, [TADR port C](../tadr-port/data-keys.md)), its UI features. A survey per mod
  before its setup's `today` is deleted.
- **Multiplayer with players who still run TADR** is not a goal: the mods' battlerooms check the
  version bytes, not the DLLs, so such a game may start and then disagree.
