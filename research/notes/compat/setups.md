# The setups and their outcomes

## Summary

Thirteen setups, each a game folder as a player has it — the retail 3.1 game, what a mod or
patch install puts next to it, and Impure's `ddraw.dll` — and one check of the safety net. Nine
meet the goal on Wine and on Windows: the retail exe alone or beside files nothing loads, and
every Patch Loader setup, **Total Mayhem 11.3.0 and ProTA 4.8** among them, since [the
takeover](takeover.md)'s first landing keeps their TADR from starting. The rest are known gaps
until its next landings: four refused by Impure on an exe that imports TADR and one where Impure
never loads. The 3.9.02 exe, which several of them run, is described at the end.

## The table

`tools/compat/setups.json` holds every setup; `fixtures.json` names every third-party file by
md5 (nothing third-party is in the repository). Measured on Wine on the reference setup and on the Windows test box, with
the DLL at `main`, the battle stage on (a skirmish on Two Continents and
`scenarios/200v200.json` for 60 s wherever the menu is reached):

| setup | what it is | Wine | Windows |
|---|---|---|---|
| `retail` | Impure alone on the retail exe (the control) | **meets goal**: menu, battle 402 of 402 applied | **meets goal** |
| `retail+tadr1` | + the 2006 recorder `Dplayx.dll` | **meets goal** | **meets goal**: the recorder loads |
| `retail+tadr-files` | + a modern `tdraw.dll` and `tplayx.dll` nothing loads | **meets goal** | **meets goal** |
| `loader+tadr-ota` | + Patch Loader v1.3.0.0, TADR `dev-dcff5dd` (OTA), its recorder | **meets goal**: TADR does not start | **meets goal** |
| `loader+tadr-tazero` | the TA Zero report: loader, TADR `tazero`, the 2022 recorder | **meets goal** | **meets goal** |
| `loader+tadr-mayhem` | loader, TADR's current Mayhem build and recorder | **meets goal** | **meets goal** |
| `392+tadr-dev` | the 3.9.02 exe, TADR `dev-dcff5dd` | `impure-refused` at `0x0040EAD6` | `impure-refused` |
| `392+tadr-2026.8.6` | the 3.9.02 exe, TADR v2026.8.6 | `impure-refused` at `0x0040EAD6` | `impure-refused` |
| `mayhem-11.3.0` | Total Mayhem 11.3.0 as installed | **meets goal**: battle fought | **meets goal** |
| `prota-4.8` | ProTA 4.8 as installed | **meets goal**: battle fought | **meets goal** |
| `mayhem-11.3.0-net` | the check of the safety net: Mayhem with the takeover off | refused by the net: 17 of 265 sites | the same |
| `gammata-ota` | gammata's OTA drop-in | `impure-not-loaded` (`ddraw_custom.dll` runs) | `impure-not-loaded` |
| `escalation` | Escalation GOLD 10.2.0 as installed | `impure-refused` at `0x0040EAD6` | `impure-refused` |
| `escalation+tadr-dev` | Escalation with TADR's current Escalation build | `impure-refused` at `0x0040EAD6` | `impure-refused` |

A full Wine run takes about seven minutes on the reference setup, six setups at a time; the
Windows column is the Windows test box, one setup at a time, start-up only, about ten minutes
(2026-09-26, the DLL at `main`). On both platforms Impure's limits were installed before
TADR's limit crack ran on the Patch Loader route (`limits: installed 241 sites` in
`tagpu.log`, then "Install Limit Crack" in `tdrawlog.txt`): Impure's `DllMain` runs before the
loader's.

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

