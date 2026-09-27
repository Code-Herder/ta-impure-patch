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

- **Retail exe, nothing else, or the 2006 recorder**: Impure runs.
- **Retail exe + Patch Loader + any TADR** (Total Mayhem 11.3.0, ProTA 4.8, the TA Zero
  player's files): Impure runs and TADR never starts — [the takeover](takeover.md)'s first
  part answers the loader's request for `tdraw.dll` with Impure. Before it, a 2026 TADR refused
  to start ("TADR engine-limit error", the TA Zero report) and a pre-2026 one crashed the first
  skirmish load (Impure v0.2.3 with Mayhem and ProTA); both are [what TADR does beside
  Impure](tadr-collision.md), and the safety net now stops the game at start-up if anything
  like it gets through.
- **An exe that imports TADR** (the 3.9.02 exe, Escalation): Impure loads nested inside TADR's
  start-up and refuses at `0x40EAD6`, the one byte those exes change that Impure checks.
- **gammata's drop-in**: its `tdraw.dll` loads `ddraw_custom.dll`, so Impure never loads.

The **goal** for every setup is the same: Impure active, running the mod's own exe and files as
the player has them, with TADR not patching the engine beside it. Reaching it is [the takeover](takeover.md)
(the owner's direction, 2026-09-26): Impure keeps TADR's code out of the process instead of
sharing the engine with it. Its first landing covers every setup where Impure starts first;
until the others land, the rest record their behaviour today as a **known gap**.

## The load routes

Which DLL runs first decides everything, because each patcher checks (or does not check) the
bytes it finds.

| route | exe imports | who loads Impure | who runs first |
|---|---|---|---|
| retail | `DDRAW`, `DPLAYX` | the exe | Impure's `DllMain`, before the exe's entry point |
| retail + recorder | `DDRAW`, `DPLAYX` → the 2006 `Dplayx.dll` | the exe | Impure; the recorder loads as the exe's DirectPlay |
| retail + Patch Loader | `DDRAW`, `DPLAYX` → the loader | the exe | Impure, then the loader's presets; its `tdraw.dll` is answered with Impure ([the takeover](takeover.md)) |
| 3.9.02 / Escalation | `TDRAW` / `TAESC` | `tdraw.dll`'s `DllMain`, `LoadLibrary("ddraw.dll")` | Impure, *inside* tdraw's start-up |
| gammata's drop-in | `TDRAW` | nobody: tdraw loads `ddraw_custom.dll` | cnc-ddraw |

**The Patch Loader route in detail** (`vendor/Total-Annihilation-Patch-Loader/dllmain.c`, and
[the Patch Loader's page](../deep-patch-loader.md)). The mod's `dplayx.dll` is in the game folder, so the retail exe's own
DirectPlay import loads it. Its `DllMain` checks for the retail exe, applies its preset list
without reading the bytes it replaces, sets `0x401064` = 1 (the handshake tdraw reads),
`LoadLibraryA("tdraw.dll")` (it shows an error box and exits if that fails), and points the
exe's two `DirectDrawCreate` calls, `0x47BFA2` and `0x4B55FB`, at the `DirectDrawCreate` of the
module that call returned. Its DirectPlay exports forward to `tplayx.dll`, TADR's recorder.
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
- **Windows has no battle stage**: its rows stop at the main menu.
- **Multiplayer is not in the suite.** An earlier two-peer run with the 2006 recorder wrote no
  `.tad` demo and is not a pass.
- **TA Zero is covered by its DLL layer only** (the player's files): its archives are not in
  the fixtures, so its setup cannot reach a TA Zero battle.
