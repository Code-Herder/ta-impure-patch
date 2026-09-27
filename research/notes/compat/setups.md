# The setups and their outcomes

## Summary

Sixteen setups: fourteen game folders as a player has them — the retail 3.1 game, what a mod or
patch install puts next to it, and Impure's `ddraw.dll` — and two checks of the harness itself.

**Nine of the fourteen meet the goal in full, on both platforms: Impure runs, and none of TADR's
code runs.** The
retail exe alone, beside the 2006 recorder, beside a 2026 recorder, beside files nothing loads, all
three Patch Loader routes, and **Total Mayhem 11.3.0 and ProTA 4.8** as installed. "None of TADR's
code" is read out of the running game on every peer of every stage — the exe's own code against
`TotalA.exe` on disk — and not from the game folder, which a recorder that starts off the entry
point leaves untouched ([the suite](suite.md), *Whether TADR ran*; [the takeover](takeover.md)).
Each of the nine also fights a 200v200 skirmish and plays a two-player network game on Wine.

The other five are known gaps, each waiting on a takeover landing: four where the exe imports TADR
instead of Impure (`TDRAW`, `TAESC`) and Impure refuses at `0x0040EAD6`, and gammata's drop-in,
where Impure never loads at all. The 3.9.02 exe, which several of them run, is described at the end.

## The table

`tools/compat/setups.json` holds every setup; `fixtures.json` names every third-party file by md5
(nothing third-party is in the repository). Measured 2026-09-27, one full run a platform, **11
meeting the goal, 5 known gaps and 0 UNEXPECTED on each**: on Wine on the DLL that lands
(`ad2cd5d6ead9`) with the battle stage on (a skirmish on Two Continents and
`scenarios/200v200.json` for 60 s wherever the menu is reached) and the network stage on (two
players, 30 s, nine games), and on the Windows test box, start-up only, on `30d1ee954d1f` — the
same tree before `main`'s simulation work was merged in, which touches nothing the takeover does.
"*n* runs" is the number of changed runs the reference-image comparison found in the exe's one
executable section; every one of them is Impure's own work unless the row says otherwise. The
counts differ a little between platforms and between builds: the window the engine is given differs,
and every patch Impure adds adds runs.

| setup | what it is | Wine | Windows |
|---|---|---|---|
| `retail` | Impure alone on the retail exe (the control) | **meets goal**: 441 runs, 0 into TADR; battle 402 of 402 applied; network game | **meets goal**: 433 runs, 0 into TADR |
| `retail+tadr1` | + the 2006 recorder `Dplayx.dll` | **meets goal**: its entry point inert, its three DirectPlay slots redirected, 443 runs and **0 into TADR**; network game | **meets goal**: the same, 434 runs, 0 into TADR |
| `retail+tadr-recorder-ota` | + a 2026 recorder as the exe's own DirectPlay | **meets goal**: 444 runs, 0 into TADR; network game | **meets goal**: 431 runs, 0 |
| `retail+tadr-files` | + a modern `tdraw.dll` and `tplayx.dll` nothing loads | **meets goal**: 443 runs, 0 into TADR; network game | **meets goal**: 434 runs, 0 |
| `loader+tadr-ota` | + Patch Loader v1.3.0.0, TADR `dev-dcff5dd` (OTA), its recorder | **meets goal**: 452 runs, 0 into TADR; network game | **meets goal**: 441 runs, 0 |
| `loader+tadr-tazero` | the TA Zero report: loader, TADR `tazero`, the 2022 recorder | **meets goal**: 451 runs, 0 into TADR; network game | **meets goal**: 440 runs, 0 |
| `loader+tadr-mayhem` | loader, TADR's current Mayhem build and recorder | **meets goal**: 452 runs, 0 into TADR; network game | **meets goal**: 439 runs, 0 |
| `mayhem-11.3.0` | Total Mayhem 11.3.0 as installed | **meets goal**: 576 runs, 0 into TADR and 3 into the mod's own `win32.dll`; battle fought; network game | **meets goal**: 566 runs, the same 3 |
| `prota-4.8` | ProTA 4.8 as installed | **meets goal**: 530 runs, 0 into TADR and 3 into the mod's own `win32.dll`; battle fought; network game | **meets goal**: 517 runs, the same 3 |
| `392+tadr-dev` | the 3.9.02 exe, TADR `dev-dcff5dd` | known gap: `impure-refused` at `0x0040EAD6`; tdraw starts — 15 places lead into TADR, one of them an import slot of the exe | the same, 13 places |
| `392+tadr-2026.8.6` | the 3.9.02 exe, TADR v2026.8.6 | known gap: the same refusal; 9 places lead into TADR, one an import slot | the same, 8 places |
| `gammata-ota` | gammata's OTA drop-in | known gap: `impure-not-loaded` (`ddraw_custom.dll` is what its tdraw loads); 37 places lead into TADR, 4 of them import slots | the same, 10 places |
| `escalation` | Escalation GOLD 10.2.0 as installed | known gap: `impure-refused` at `0x0040EAD6`; 10 places lead into `TAESC.dll` and `eplayx.dll`, one an import slot | the same, 8 places |
| `escalation+tadr-dev` | Escalation with TADR's current Escalation build | known gap: the same refusal; 20 places lead into `TAESC.dll` and `eplayx.dll`, one an import slot | the same, 18 places |
| `retail+tadr1-recorder` | **the check that the evidence can fire**: `retail+tadr1` with `tagpu_takeover.off` | the recorder runs, as it must: its log, **and 31 places** of the game leading into it — 28 sites of its code and the exe's 3 DirectPlay slots; battle fought | the same, **6 sites** — the six the entry point installs, the rest being the DirectPlay path's, which a start-up-only run never reaches |
| `mayhem-11.3.0-net` | **the check of the safety net**: Mayhem with `tagpu_takeover.off` | refused by the net: 17 of 265 sites rewritten; 12 places lead into TADR — the exe's two `DirectDrawCreate` calls, its 3 DirectPlay slots and the recorder's six sites | the same, 8 places |

**What the two harness checks buy.** A goal of "no TADR code" is worth nothing from a check that
cannot see any, so one setup turns the takeover off and must see the recorder: its log *and* 24
patched sites. The other turns it off on Total Mayhem, whose 2024 TADR writes Impure's own sites
without reading them, and must be stopped by the safety net before the skirmish that would
otherwise crash. Both are mechanisms, not players' folders, and neither plays a network game.

**A recorder that starts off the exe's entry point patches six sites** — `0x00417B9B`,
`0x0045130F`, `0x00480770`, `0x00490DF9`, `0x00496559`, `0x004965B3`, the same six in every setup
where one runs — and a build that also answers a DirectPlay call patches eighteen more. The
recorder control shows both halves by platform: 6 on Windows, where the suite stops at the main
menu, and 24 on Wine, where it fights a battle and plays a network game. That is what the nine
rows above have none of, and it is why the goal is judged by reading the process: none of it
leaves a trace in the game folder.

Some rows also report runs that **hold** an address inside one of those modules without an
instruction that goes there — 18 on `392+tadr-dev` and 19 on `escalation+tadr-dev` on Wine, and on
Windows a different set entirely, 43 on `loader+tadr-tazero` and 11 on `retail+tadr-files`. Those
are coincidences: they are counted and never judged, and judging them refused two players' installs
before the rule was fixed ([the takeover](takeover.md), part 4).

The **import slots** in those counts are the exe's own DirectPlay imports where a recorder answered
them (3 on the recorder control, 3 on `mayhem-11.3.0-net`) and the `TDRAW`/`TAESC` import on the
routes where the exe loads TADR instead of Impure (1, and 4 on gammata's). They are read on Wine
only; the Windows column's places are sites of code alone.

A full Wine run takes about 24 minutes of runs on the reference setup — the single-player six at a
time, the nine network games one at a time, about 80 s each, queued beside them — plus about 16
minutes to build the sixteen instances first, since each is created fresh. The Windows runs take
about 45 s a setup, one at a time; there is no battle or network stage there yet.

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
