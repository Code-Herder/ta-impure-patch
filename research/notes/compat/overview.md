# Impure beside mods, TADR and the Patch Loader

## Summary

Players do not install Impure into a clean retail folder. They install it into a TA Zero, Total
Mayhem, ProTA or Escalation folder that already carries a TADR `tdraw.dll`, a recorder
`tplayx.dll`, the Community Patch Loader as `dplayx.dll`, a modified `TotalA.exe`, or cnc-ddraw
under two names. This section is the guide to what happens then: the **load routes** that decide
which DLL runs first (below), [what TADR does beside Impure](tadr-collision.md), the
[outcome each setup has today](setups.md), how to [identify a player's setup](identify.md) from
what they send, and [the suite](suite.md) that tests all of it, `tools/compat/tacompat.py`, which
runs before every release (`CLAUDE.md` *Releases*; the `ta-compat-check` skill drives it).

The short version, measured on the reference setup with the DLL at `main`:

- **Retail exe alone**: Impure runs.
- **Retail exe + the 2006 recorder** (the game folder's own `dplayx.dll`): Impure runs and the
  recorder is never *called* — [the takeover](takeover.md) points the exe's three DirectPlay
  import slots at Windows' `dplayx.dll` — but it still **runs**, off the exe's entry point, and
  hooks seven places in the game's code.
- **Retail exe + Patch Loader + any TADR** (Total Mayhem 11.3.0, ProTA 4.8, the TA Zero
  player's files): Impure runs and `tdraw.dll` does not — the takeover answers the loader's
  request for it with Impure, and points the DirectPlay imports the loader forwards to the
  recorder at Windows' own, alone and in a two-player network game. **The recorder itself still
  runs**, off the entry point. Before it, a 2026 TADR refused
  to start ("TADR engine-limit error", the TA Zero report) and a pre-2026 one crashed the first
  skirmish load (Impure v0.2.3 with Mayhem and ProTA); both are [what TADR does beside
  Impure](tadr-collision.md), and the safety net now stops the game at start-up if anything
  like it gets through.
- **An exe that imports TADR** (the 3.9.02 exe, Escalation): Impure loads nested inside TADR's
  start-up and refuses at `0x40EAD6`, the one byte those exes change that Impure checks.
- **gammata's drop-in**: its `tdraw.dll` loads `ddraw_custom.dll`, so Impure never loads.

The **goal** for every setup is the same: Impure active, running the mod's own exe and files as
the player has them, with none of TADR's code run — `tdraw.dll` and the recorder alike. Reaching it is [the takeover](takeover.md)
(the owner's direction, 2026-09-26): Impure keeps TADR's code out of the process instead of
sharing the engine with it. Its first landing covers `tdraw.dll` on every setup where Impure
starts first. **The recorder half of the goal is not met anywhere yet**, and the suite cannot see
it from the game folder ([the suite](suite.md), *Whether TADR ran*); until the rest lands, every
setup records its behaviour today, and the `tadr_ran: false` rows mean no recorder log rather
than no TADR code.

## The load routes

Which DLL runs first decides everything, because each patcher checks (or does not check) the
bytes it finds.

| route | exe imports | who loads Impure | who runs first |
|---|---|---|---|
| retail | `DDRAW`, `DPLAYX` | the exe | Impure's `DllMain`, before the exe's entry point |
| retail + recorder | `DDRAW`, `DPLAYX` → the 2006 `Dplayx.dll` | the exe | Impure; the recorder is never called, and runs anyway off the exe's entry point |
| retail + Patch Loader | `DDRAW`, `DPLAYX` → the loader | the exe | Impure, then the loader's presets; its `tdraw.dll` is answered with Impure, its recorder runs off the entry point ([the takeover](takeover.md)) |
| 3.9.02 / Escalation | `TDRAW` / `TAESC` | `tdraw.dll`'s `DllMain`, `LoadLibrary("ddraw.dll")` | Impure, *inside* tdraw's start-up |
| gammata's drop-in | `TDRAW` | nobody: tdraw loads `ddraw_custom.dll` | cnc-ddraw |

**The Patch Loader route in detail** (`vendor/Total-Annihilation-Patch-Loader/dllmain.c`, and
[the Patch Loader's page](../deep-patch-loader.md)). The mod's `dplayx.dll` is in the game folder, so the retail exe's own
DirectPlay import loads it. Its `DllMain` checks for the retail exe, applies its preset list
without reading the bytes it replaces, sets `0x401064` = 1 (the handshake tdraw reads),
`LoadLibraryA("tdraw.dll")` (it shows an error box and exits if that fails), and points the
exe's two `DirectDrawCreate` calls, `0x47BFA2` and `0x4B55FB`, at the `DirectDrawCreate` of the
module that call returned. Its DirectPlay exports forward to `tplayx.dll`, TADR's recorder, so
the exe's DirectPlay import slots are bound into the recorder before any `DllMain` runs; Impure's
points them at Windows' `dplayx.dll`, so no game call reaches the recorder. **The recorder runs
all the same**, from the jump its `DllMain` splices over the exe's entry point `0x004E6FA0`
([the takeover](takeover.md), part 1) — measured on `retail+tadr1`, seven hooked sites in the
game's code.
Impure's `DllMain` has run before all of this: `tagpu_limits_install` (`dllmain.c`) compares its
whole fail-closed site table with the stock 3.1 bytes, finds them stock, and writes every site,
and the fork's `hook_init` points the loader's `LoadLibrary` imports at Impure's. So the
loader's request for `tdraw.dll` is answered with Impure itself, TADR's `DllMain` never runs,
and the two calls reach Impure's own export ([the takeover](takeover.md), part 1). At the first
DirectDraw call Impure reads every site again (part 4): with the takeover switched off, a
pre-2026 TADR's rewrites are what it finds, and the game stops there instead of crashing when
the first battle loads.

Mods on this route move the game's registry key through the loader's `RegistryPath=`:
Total Mayhem to `Software\TotalM`, ProTA to `Software\ProTA`. A fresh key has the stock default
map, which a mod may not ship.

## Gaps

- **The takeover covers the Patch Loader route only.** The 3.9.02 exe, Escalation and gammata's
  drop-in are still refused or never load Impure ([its landings](takeover.md#landings)).
- **TADR's recorder still runs on every setup that carries one**, off the exe's entry point,
  and patches the engine after Impure ([the takeover](takeover.md), part 1). What the suite
  checks from the game folder cannot see it, so no row's `tadr_ran: false` covers it yet.
- **Windows has no battle stage and no network stage**: its rows stop at the main menu.
- **The network stage is two players on one machine**, over Windows' DirectPlay on loopback;
  a game between two machines, and one with a player who still runs TADR, are not tested.
- **TA Zero is covered by its DLL layer only** (the player's files): its archives are not in
  the fixtures, so its setup cannot reach a TA Zero battle.
