# The setups and their outcomes

## Summary

Thirteen setups, each a game folder as a player has it — the retail 3.1 game, what a mod or
patch install puts next to it, and Impure's `ddraw.dll` — and two checks of the harness. All
eight of the player's setups that start Impure meet the goal on Wine and on Windows: Impure
runs and **none of TADR's code does**, neither `tdraw.dll` nor the recorder — the retail exe
alone, beside the 2006 recorder, beside files nothing loads, and every Patch Loader setup,
**Total Mayhem 11.3.0 and ProTA 4.8** among them ([the takeover](takeover.md), T1 and T1b). On
Wine each also plays a two-player network game with no TADR code on either peer. The rest are
known gaps until the takeover's next landings: four refused by Impure on an exe that imports
TADR, and one where Impure never loads. The 3.9.02 exe, which several of them run, is described
at the end.

## The table

`tools/compat/setups.json` holds every setup; `fixtures.json` names every third-party file by
md5 (nothing third-party is in the repository). Measured 2026-09-27 on Wine on the reference
setup — the battle stage on (a skirmish on Two Continents and `scenarios/200v200.json` for 60 s
wherever the menu is reached) and the network stage on (two players, 30 s) — and on the Windows
test box, start-up only, with the DLL at the T1b commit:

| setup | what it is | Wine | Windows |
|---|---|---|---|
| `retail` | Impure alone on the retail exe (the control) | **meets goal**: battle 402 of 402 applied, network game | **meets goal** |
| `retail+tadr1` | + the 2006 recorder `Dplayx.dll` | **meets goal**: the recorder loads and is never called; network game | **meets goal** |
| `retail+tadr1-recorder` | the check of the recorder evidence: the same with the takeover off | the recorder runs, as it must | the same |
| `retail+tadr-files` | + a modern `tdraw.dll` and `tplayx.dll` nothing loads | **meets goal**, network game | not run |
| `loader+tadr-ota` | + Patch Loader v1.3.0.0, TADR `dev-dcff5dd` (OTA), its recorder | **meets goal**, network game | **meets goal** |
| `loader+tadr-tazero` | the TA Zero report: loader, TADR `tazero`, the 2022 recorder | **meets goal**, network game | **meets goal** |
| `loader+tadr-mayhem` | loader, TADR's current Mayhem build and recorder | **meets goal**, network game | **meets goal** |
| `392+tadr-dev` | the 3.9.02 exe, TADR `dev-dcff5dd` | `impure-refused` at `0x0040EAD6`; tdraw starts | not run |
| `392+tadr-2026.8.6` | the 3.9.02 exe, TADR v2026.8.6 | `impure-refused` at `0x0040EAD6`; tdraw starts | not run |
| `mayhem-11.3.0` | Total Mayhem 11.3.0 as installed | **meets goal**: battle fought, network game | **meets goal** |
| `prota-4.8` | ProTA 4.8 as installed | **meets goal**: battle fought, network game | **meets goal** |
| `mayhem-11.3.0-net` | the check of the safety net: Mayhem with the takeover off | refused by the net: 17 of 265 sites | the same |
| `gammata-ota` | gammata's OTA drop-in | `impure-not-loaded` (`ddraw_custom.dll` runs, and the recorder) | not run |
| `escalation` | Escalation GOLD 10.2.0 as installed | `impure-refused` at `0x0040EAD6`; tdraw starts | not run |
| `escalation+tadr-dev` | Escalation with TADR's current Escalation build | `impure-refused` at `0x0040EAD6`; tdraw starts | not run |

Before T1b the recorder ran in every setup that carries one (measured 2026-09-26 from its log),
at the game's first DirectPlay call, which comes at start-up; the goal of the time did not look
for it. The Windows column's "not run" rows are unchanged since the T1 runs of 2026-09-26, which
matched their Wine rows.

A full Wine run takes about 21 minutes on the reference setup: the single-player runs six at a
time, about seven minutes, and the eight network games one at a time, about 80 s each, queued
beside them. The Windows runs take about 45 s a setup, one at a time.

## The 3.9.02 exe

gammata's drop-in and the older community installs ship the "3.9.02" `TotalA.exe` (md5
`df08c41e…`). Against retail 3.1, 1 437 bytes differ and 1 398 of them are the version
resource. The engine changes are 12 code bytes at six sites and 23 data bytes:

| where | change | effect |
|---|---|---|
| imports | `DDRAW`, `WIN32`, `DPLAYX` → `TDRAW`, `TMUSI`, `TPLAYX` | TADR's DLLs load instead of the system ones |
| `0x40EAD6` | path budget 1333 → 66650 | the one site Impure checks and refuses |
| `0x406FB4` | branch → `jmp` | an AI commander keeps its orders when hit |
| `0x43E784` | branch to the next instruction | the reclaim cursor shows over any unit |
| `0x4266A5` | `test eax,eax` → `mov al,1` | no DirectX version box (Impure makes the same change at `0x4266A7`) |
| `0x4966E7`, `0x496776` | key table | two debug-mode keys remapped |
| `0x4FCC7C` | −1000.0 → −4.3×10¹² | `+atm` fills storage |
| `0x501FD8`, `0x501FE4` | level 4 → 2 | `+AI` and `+Control` become cheat-level commands (the two rows Total Mayhem's and ProTA's patch lists leave unnamed) |
| `0x5098A4` | `totala.ini` → `ta.ini` | the exe's options file, leaving `totala.ini` to TADR |

Every 3.9.02 feature — the unit limit, the weapon IDs, the megamap, the recorder — comes from
`tdraw.dll` and `tplayx.dll` at run time, not from the exe.

