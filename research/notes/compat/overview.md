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

- **Retail exe, nothing else, or the 2006 recorder**: Impure runs. These are the only setups
  that meet the goal.
- **Retail exe + Patch Loader + a 2026 TADR** (the TA Zero player's report): TADR refuses at its
  first DirectDraw call with "TADR engine-limit error", because Impure's limits are already in
  the bytes it validates.
- **Retail exe + Patch Loader + a pre-2026 TADR** (Total Mayhem 11.3.0 and ProTA 4.8, both the
  current releases): nobody validates, both patch the engine, and **the game crashes on the
  first skirmish load** — TADR's limit crack rewrites twelve of Impure's unit-type sites and
  corrupts them. The menu works, so the player meets it in their first game.
- **An exe that imports TADR** (the 3.9.02 exe, Escalation): Impure loads nested inside TADR's
  start-up and refuses at `0x40EAD6`, the one byte those exes change that Impure checks.
- **gammata's drop-in**: its `tdraw.dll` loads `ddraw_custom.dll`, so Impure never loads.

The **goal** for every setup is the same: Impure active, running the mod's own exe and files as
the player has them, with TADR not patching the engine beside it. Reaching it is the *takeover*
(the owner's direction, 2026-09-26, not built): Impure keeps TADR's engine patches out of the
process instead of sharing the engine with them. Until then each setup records its behaviour
today as a **known gap**.

## The load routes

Which DLL runs first decides everything, because each patcher checks (or does not check) the
bytes it finds.

| route | exe imports | who loads Impure | who runs first |
|---|---|---|---|
| retail | `DDRAW`, `DPLAYX` | the exe | Impure's `DllMain`, before the exe's entry point |
| retail + recorder | `DDRAW`, `DPLAYX` → the 2006 `Dplayx.dll` | the exe | Impure; the recorder loads as the exe's DirectPlay |
| retail + Patch Loader | `DDRAW`, `DPLAYX` → the loader | the exe | Impure, then the loader's presets, then `tdraw.dll` |
| 3.9.02 / Escalation | `TDRAW` / `TAESC` | `tdraw.dll`'s `DllMain`, `LoadLibrary("ddraw.dll")` | Impure, *inside* tdraw's start-up |
| gammata's drop-in | `TDRAW` | nobody: tdraw loads `ddraw_custom.dll` | cnc-ddraw |

**The Patch Loader route in detail** (`vendor/Total-Annihilation-Patch-Loader/dllmain.c`, and
[the Patch Loader's page](../deep-patch-loader.md)). The mod's `dplayx.dll` is in the game folder, so the retail exe's own
DirectPlay import loads it. Its `DllMain` checks for the retail exe, applies its preset list
without reading the bytes it replaces, sets `0x401064` = 1 (the handshake tdraw reads),
`LoadLibraryA("tdraw.dll")` (it shows an error box and exits if that fails), and points the
exe's two `DirectDrawCreate` calls, `0x47BFA2` and `0x4B55FB`, at tdraw's export. Its DirectPlay
exports forward to `tplayx.dll`, TADR's recorder. Impure's `DllMain` has run before all of this:
`tagpu_limits_install` (`dllmain.c`) compares its whole fail-closed site table with the stock
3.1 bytes, finds them stock, and writes every site. Nothing reads those sites again afterwards —
which is the gap the pre-2026 TADR falls through.

Mods on this route move the game's registry key through the loader's `RegistryPath=`:
Total Mayhem to `Software\TotalM`, ProTA to `Software\ProTA`. A fresh key has the stock default
map, which a mod may not ship.

## Gaps

- **The takeover is not built.** No setup but the retail ones meets the goal.
- **Windows has no battle stage**, so it cannot see the Mayhem crash; its Mayhem row stops at
  the menu with TADR installed.
- **Multiplayer is not in the suite.** An earlier two-peer run with the 2006 recorder wrote no
  `.tad` demo and is not a pass.
- **TA Zero is covered by its DLL layer only** (the player's files): its archives are not in
  the fixtures, so its setup cannot reach a TA Zero battle.
