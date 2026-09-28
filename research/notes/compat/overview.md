# Impure beside mods, TADR and the Patch Loader

## Summary

Players do not install Impure into a clean retail folder. They install it into a TA Zero, TA Twilight,
Total Mayhem, ProTA or Escalation folder that already carries a TADR `tdraw.dll`, a recorder
`tplayx.dll`, the Community Patch Loader as `dplayx.dll`, a modified `TotalA.exe`, or cnc-ddraw
under two names. This section is the guide to what happens then: the **load routes** that decide
which DLL runs first (below), [what TADR does beside Impure](tadr-collision.md), the
[outcome each setup has today](setups.md), how to [identify a player's setup](identify.md) from
what they send, and [the suite](suite.md) that tests all of it, `tools/compat/tacompat.py`, which
runs before every release (`CLAUDE.md` *Releases*; the `ta-compat-check` skill drives it).

The short version, measured on the reference setup with the DLL at `main`:

- **Retail exe alone**: Impure runs.
- **Retail exe + the 2006 recorder** (the game folder's own `dplayx.dll`): Impure runs, the
  recorder is never *called* — [the takeover](takeover.md) points the exe's three DirectPlay
  import slots at Windows' `dplayx.dll` — and it never **runs** either: its entry point is made
  inert before the loader calls it, so the jump it would splice over the exe's entry point is
  never written. Read out of the running process, 0 of the game's code leads into it.
- **Retail exe + Patch Loader + any TADR** (Total Mayhem 11.3.0, ProTA 4.8, the TA Zero
  player's files): Impure runs and **no TADR code does** — the takeover answers the loader's
  request for `tdraw.dll` with Impure, makes the recorder's entry point inert, and points the
  DirectPlay imports the loader forwards to it at Windows' own; alone and in a two-player network
  game, on every peer. Before it, a 2026 TADR refused to start ("TADR engine-limit error", the TA
  Zero report) and a pre-2026 one crashed the first skirmish load (Impure v0.2.3 with Mayhem and
  ProTA); both are [what TADR does beside Impure](tadr-collision.md), and Impure now stops the
  game at start-up if anything like it gets through — the safety net on its own sites, and the
  whole exe image against the exe file on disk.
- **An exe that imports TADR** (the 3.9.02 exe, Escalation, TA Zero's `ZDRAW`, TA Twilight's
  3.9.02-shaped exe): Impure loads nested inside TADR's
  start-up and **stops the rest of it** — the `DllMain` that loaded Impure returns without
  patching anything, the recorder's entry point is made inert before the loader calls it, and the
  exe's import from the tdraw leads into Impure's own export, and the recorder's DirectPlay slots
  into Impure's forwarders to Windows' own `dplayx.dll`. TADR's `DllMain` has written one line to
  its own log by the time Impure exists, and that is the whole of what runs
  ([the takeover](takeover.md), 1d).
- **gammata's drop-in**: its `tdraw.dll` loads `ddraw_custom.dll`, so Impure never loads.

The **goal** for every setup is the same: Impure active, running the mod's own exe and files as
the player has them, with none of TADR's code run — `tdraw.dll` and the recorder alike. Reaching it
is [the takeover](takeover.md) (the owner's direction, 2026-09-26): Impure keeps TADR's code out of
the process instead of sharing the engine with it, and the exe file on disk is the reference every
launch is compared against. **Every route where Impure loads at all meets that goal as of
2026-09-27**, and the suite proves it by reading the running game's code rather than its log files
([the suite](suite.md), *Whether TADR ran*). On the routes where TADR's own `DllMain` is what loads
Impure it has written its first log line before Impure exists, and that one line is the whole of
what runs. Only gammata's drop-in is left, where Impure never loads.

## The load routes

Which DLL runs first decides everything, because each patcher checks (or does not check) the
bytes it finds.

| route | exe imports | who loads Impure | who runs first |
|---|---|---|---|
| retail | `DDRAW`, `DPLAYX` | the exe | Impure's `DllMain`, before the exe's entry point |
| retail + recorder | `DDRAW`, `DPLAYX` → the 2006 `Dplayx.dll` | the exe | Impure; the recorder's entry point is made inert before the loader calls it, and it is never called |
| retail + Patch Loader | `DDRAW`, `DPLAYX` → the loader | the exe | Impure, then the loader's presets; its `tdraw.dll` is answered with Impure, its recorder is made inert and never called ([the takeover](takeover.md)) |
| 3.9.02 / Escalation / TA Zero / TA Twilight | `TDRAW` / `TAESC` / `ZDRAW` | `tdraw.dll`'s `DllMain`, `LoadLibrary("ddraw.dll")` | Impure, *inside* tdraw's start-up, and the rest of that start-up does not run; the recorder's entry point is made inert before the loader calls it ([the takeover](takeover.md), 1d) |
| gammata's drop-in | `TDRAW` | nobody: tdraw loads `ddraw_custom.dll` | cnc-ddraw |

**The Patch Loader route in detail** (`vendor/Total-Annihilation-Patch-Loader/dllmain.c`, and
[the Patch Loader's page](../deep-patch-loader.md)). The mod's `dplayx.dll` is in the game folder, so the retail exe's own
DirectPlay import loads it. Its `DllMain` checks for the retail exe, applies its preset list
without reading the bytes it replaces, sets `0x401064` = 1 (the handshake tdraw reads),
`LoadLibraryA("tdraw.dll")` (it shows an error box and exits if that fails), and points the
exe's two `DirectDrawCreate` calls, `0x47BFA2` and `0x4B55FB`, at the `DirectDrawCreate` of the
module that call returned. Its DirectPlay exports forward to `tplayx.dll`, TADR's recorder, so
the exe's DirectPlay import slots are bound into the recorder before any `DllMain` runs; Impure's
points them at Windows' `dplayx.dll`, so no game call reaches the recorder. The recorder's own
way in — the jump its `DllMain` splices over the exe's entry point `0x004E6FA0` — is closed by
making its entry point inert before the loader calls it ([the takeover](takeover.md), part 1).
What that way in costs, MEASURED 2026-09-27 by reading the running game: a recorder that starts
off the entry point alone patches **six** sites of the engine, the same six in every setup where
one runs — `0x00417B9B`, `0x0045130F`, `0x00480770`, `0x00490DF9`, `0x00496559`, `0x004965B3` — and
the 2006 build with `tagpu_takeover.off`, which answers a DirectPlay call as well, patches
**twenty-four**. With the takeover on, none of them is there.

The loader also rewrites three of the exe's import thunks into direct calls to the mod's own
`win32.dll` (`0x004E4708`, `0x004E71A0`, `0x004EADF2`): the mod's bytes, which stay, and which
the reference-image check reports without refusing.
Impure's `DllMain` has run before all of this: `tagpu_limits_install` (`dllmain.c`) compares its
whole fail-closed site table with the stock 3.1 bytes, finds them stock, and writes every site,
and the fork's `hook_init` points the loader's `LoadLibrary` imports at Impure's. So the
loader's request for `tdraw.dll` is answered with Impure itself, TADR's `DllMain` never runs,
and the two calls reach Impure's own export ([the takeover](takeover.md), part 1). At the first
DirectDraw call Impure reads every site again (part 3): with the takeover switched off, a
pre-2026 TADR's rewrites are what it finds, and the game stops there instead of crashing when
the first battle loads.

Mods on this route move the game's registry key through the loader's `RegistryPath=`:
Total Mayhem to `Software\TotalM`, ProTA to `Software\ProTA`. A fresh key has the stock default
map, which a mod may not ship.

## Gaps

- **gammata's drop-in never loads Impure at all**: its `tdraw.dll` loads `ddraw_custom.dll`, so
  Impure would have to be installed under that name ([its landings](takeover.md#landings)).
- **Windows has no battle stage and no network stage**: its rows stop at the main menu, which is
  why a recorder there shows only the six sites its entry point installs. The reading of the game's
  code runs on both platforms; the exe's import slots are read on Wine only ([the suite](suite.md)).
- **The network stage is two players on one machine**, over Windows' DirectPlay on loopback;
  a game between two machines, and one with a player who still runs TADR, are not tested.
- **TA Twilight's `dsound.dll` proxy is not loaded on Wine**, which prefers its built-in
  `dsound` to a copy in the game folder; it loads beside Impure only on Windows, where the
  new setups have not run yet.
- **TA Zero's map pack is not in the fixtures** (TA Forever installs one): the battle is fought on the
  retail Two Continents.
