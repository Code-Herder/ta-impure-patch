# The setups and their outcomes

## Summary

Nineteen setups: sixteen game folders as a player has them — the retail 3.1 game, what a mod or
patch install puts next to it, and Impure's `ddraw.dll` — and three checks of the harness itself
(`retail+tadr1-recorder`, `mayhem-11.3.0-net`, `gui-stress`).

**Thirteen of the fourteen that both platforms have run meet the goal in full: Impure runs, and none of
TADR's code runs.** The retail exe alone, beside the 2006 recorder, beside a 2026 recorder, beside
files nothing loads, all three Patch Loader routes, **Total Mayhem 11.3.0 and ProTA 4.8** as
installed, and — since the T2 landing — the four routes whose exe imports TADR instead of
DirectDraw: the 3.9.02 exe with two builds of TADR, and **Escalation GOLD 10.2.0** with and without
its own. **TA Zero Alpha 5 and TA Twilight v2.0 Beta 98**, as TA Forever installs them, take the same
route and meet the goal on Wine as well (2026-09-28, below); Windows has not run them yet. "None of TADR's code" is read out of the running game on every peer of every stage — the
exe's own code against `TotalA.exe` on disk — and not from the game folder, which a recorder that
starts off the entry point leaves untouched ([the suite](suite.md), *Whether TADR ran*; [the
takeover](takeover.md)). Each of the thirteen fights a 200v200 skirmish, and eleven of them play a
two-player network game on Wine: every player's setup whose single-player run shows Impure running.

The one known gap left is gammata's drop-in, where Impure never loads at all: its `tdraw.dll` loads
`ddraw_custom.dll`, so Impure would have to be installed under that name (T3). The 3.9.02 exe,
which several setups run, is described at the end.

## The table

`tools/compat/setups.json` holds every setup; `fixtures.json` names every third-party file by md5
(nothing third-party is in the repository). Measured 2026-09-27, one full run a platform on the
same DLL — the one that lands, `70806711d2ca` — **15 meeting the goal, 1 known gap and 0 UNEXPECTED
on each**: on Wine with the battle stage on (a skirmish on Two Continents and
`scenarios/200v200.json` for 60 s wherever the menu is reached) and the network stage on (two
players, 30 s, **eleven games**), and on the Windows test box, start-up only.
"*n* runs" is the number of changed runs the reference-image comparison found in the exe's one
executable section; every one of them is Impure's own work unless the row says otherwise. The
counts differ a little between platforms and between builds: the window the engine is given differs,
and every patch Impure adds adds runs.

| setup | what it is | Wine | Windows |
|---|---|---|---|
| `retail` | Impure alone on the retail exe (the control) | **meets goal**: 443 runs, 0 into TADR; battle 402 of 402 applied; network game | **meets goal**: 485 runs, 0 into TADR |
| `retail+tadr1` | + the 2006 recorder `Dplayx.dll` | **meets goal**: its entry point inert, its three DirectPlay slots redirected, 443 runs and **0 into TADR**; network game | **meets goal**: the same, 438 runs, 0 into TADR |
| `retail+tadr-recorder-ota` | + a 2026 recorder as the exe's own DirectPlay | **meets goal**: 444 runs, 0 into TADR; network game | **meets goal**: 439 runs, 0 |
| `retail+tadr-files` | + a modern `tdraw.dll` and `tplayx.dll` nothing loads | **meets goal**: 443 runs, 0 into TADR; network game | **meets goal**: 441 runs, 0 |
| `loader+tadr-ota` | + Patch Loader v1.3.0.0, TADR `dev-dcff5dd` (OTA), its recorder | **meets goal**: 452 runs, 0 into TADR; network game | **meets goal**: 446 runs, 0 |
| `loader+tadr-tazero` | the TA Zero report: loader, TADR `tazero`, the 2022 recorder | **meets goal**: 451 runs, 0 into TADR; network game | **meets goal**: 446 runs, 0 |
| `loader+tadr-mayhem` | loader, TADR's current Mayhem build and recorder | **meets goal**: 452 runs, 0 into TADR; network game | **meets goal**: 448 runs, 0 |
| `mayhem-11.3.0` | Total Mayhem 11.3.0 as installed | **meets goal**: 576 runs, 0 into TADR and 3 into the mod's own `win32.dll`; battle fought; network game | **meets goal**: 575 runs, the same 3 |
| `prota-4.8` | ProTA 4.8 as installed | **meets goal**: 532 runs, 0 into TADR and 3 into the mod's own `win32.dll`; battle fought; network game | **meets goal**: 528 runs, the same 3 |
| `392+tadr-dev` | the 3.9.02 exe, TADR `dev-dcff5dd` | **meets goal**: `TDRAW`'s `DllMain` loaded Impure and the rest of it did not run, the recorder's entry point inert, the exe's one `TDRAW` import and its three DirectPlay slots redirected; 441 runs, **0 into TADR**; battle fought; network game | **meets goal**: the same, 439 runs, 0 into TADR |
| `392+tadr-2026.8.6` | the 3.9.02 exe, TADR v2026.8.6 | **meets goal**: the same, 441 runs and **0 into TADR**; battle fought; network game | **meets goal**: the same, 438 runs, 0 |
| `gammata-ota` | gammata's OTA drop-in | known gap: `impure-not-loaded` (`ddraw_custom.dll` is what its tdraw loads); 37 places of the game's code lead into TADR, and 4 of the exe's import slots — the other 15 surprising slots lead into `ddraw_custom.dll`, which is cnc-ddraw and not TADR | the same, 10 places |
| `escalation` | Escalation GOLD 10.2.0 as installed | **meets goal**: `TAESC`'s `DllMain` loaded Impure and the rest of it did not run, the recorder's entry point inert, the exe's one `TAESC` import redirected, and the exe file's own path budget at `0x0040EAD6` kept; 441 runs, **0 into TADR**; battle fought; network game played | **meets goal**: the same, 451 runs, 0 into TADR |
| `escalation+tadr-dev` | Escalation with TADR's current Escalation build | **meets goal**: the same, with TADR's Escalation build beside it; 442 runs and **0 into TADR**; battle fought; network game played | **meets goal**: the same, 443 runs, 0 |
| `tazero-alpha5` | TA Zero Alpha 5 as TA Forever installs it: its exe imports `ZDRAW` (TADR 2025.4.24) and `ZPLAYX` | **meets goal** (2026-09-28): `ZDRAW`'s `DllMain` loaded Impure and the rest of it did not run; the exe file's unit ceiling of 5000 replaced with 1500 ([the takeover](takeover.md), part 2); 442 runs, **0 into TADR**; GoK and Arm commanders; its own battle (`compat-battle-tazero`) fought; network game played, each peer holding both GoK commanders | not run |
| `twilight-2.0b98` | TA Twilight v2.0 Beta 98 as TA Forever installs it: the 3.9.02-shaped exe, TADR v2026.6.8 | **meets goal** (2026-09-28): the 3.9.02 route; 443 runs, **0 into TADR**; battle fought; network game played. Its `dsound.dll` proxy is Wine's built-in here | not run |
| `retail+tadr1-recorder` | **the check that the evidence can fire**: `retail+tadr1` with `tagpu_takeover.off` | the recorder runs, as it must: its log, **and 31 places** of the game leading into it — 28 sites of its code and the exe's 3 DirectPlay slots; battle fought | the same, **6 sites** — the six the entry point installs, the rest being the DirectPlay path's, which a start-up-only run never reaches |
| `mayhem-11.3.0-net` | **the check of the safety net**: Mayhem with `tagpu_takeover.off` | refused by the net: 17 of 265 sites rewritten; 12 places lead into TADR — the exe's two `DirectDrawCreate` calls, its 3 DirectPlay slots and the recorder's six sites | the same, 8 places |

**Measured 2026-09-28, Wine, the landing's DLL** (`51e20da2ac55`, 988 s): with the commander check
([the suite](suite.md)) and the two new setups, **14 meeting the goal, 1 known gap, 4 UNEXPECTED**,
none of them from the landing. Every battle started with a commander for every player (retail's
saved skirmish has three: ARMCOM and two CORCOM), and each peer of the 12 network games that
started held both players'. **Three network games never started** — `mayhem-11.3.0`,
`escalation`, `tazero-alpha5` — the joiner listing no session after twelve UPDATEs with the
host in its battle room on its own port; main's own build lost one the same way the night
before (0 of 13, 1 of 13, 3 of 15 in the three full runs since). The cause was the harness's
text entry, not the game: a joiner's ADDRESS field left reading `1` ([the suite](suite.md), the
network stage), fixed by reading every field back. Both Escalation setups also logged one UI-layer fresh start ("a copy names a source
twin this store never made"), as ten Escalation runs since 2026-09-27 have, main's build
included: the G21f/G21g work's open gap ([GPU status](../gpu-status.md)).

**Re-measured after the review, 2026-09-28, Wine** (`4cf8dd1875ab`, 313 s), on the three setups the
commander check's seat list changes the most — retail, `tazero-alpha5`, `twilight-2.0b98` — with
the network stage: **3 meeting the goal, 0 UNEXPECTED**. Retail's skirmish seats three
(ARMCOM, CORCOM, CORCOM, owners 0, 1 and 3), TA Zero's a GoK and an Arm commander, and every
network peer held both players' commanders — TA Zero's the first network game of that setup the
check has seen.

**What the two harness checks buy.** A goal of "no TADR code" is worth nothing from a check that
cannot see any, so one setup turns the takeover off and must see the recorder: its log *and* 31
patched places. The other turns it off on Total Mayhem, whose 2024 TADR writes Impure's own sites
without reading them, and must be stopped by the safety net before the skirmish that would
otherwise crash. Both are mechanisms, not players' folders, and neither plays a network game.

**A recorder that starts off the exe's entry point patches six sites** — `0x00417B9B`,
`0x0045130F`, `0x00480770`, `0x00490DF9`, `0x00496559`, `0x004965B3`, the same six in every setup
where one runs — and a build that also answers a DirectPlay call patches twenty-two more. The
recorder control shows both halves by platform: 6 on Windows, where the suite stops at the main
menu, and 28 sites plus the exe's 3 DirectPlay slots on Wine, where it fights a battle and plays a
network game. That is what the thirteen rows above have none of, and it is why the goal is judged
by reading the process: none of it leaves a trace in the game folder.

Some rows also report runs that **hold** an address inside one of those modules without an
instruction that goes there — none at all on Wine in this run, and on Windows 55 on
`escalation+tadr-dev`, 52 on `retail+tadr-recorder-ota`, 20 on `392+tadr-2026.8.6`, 17 on
`mayhem-11.3.0-net` and 5 on `392+tadr-dev`. A different set on each platform and each build,
because where a DLL lands is what decides which four bytes read as an address into it. Those are
coincidences: they are counted and never judged, and judging them refused two players' installs
before the rule was fixed ([the takeover](takeover.md), part 3).

The **import slots** read are every slot of the exe leading somewhere the exe file does not name:
the exe's own DirectPlay imports where a recorder answered them (3 on the recorder control, 3 on
`mayhem-11.3.0-net`), and gammata's 19, of which 4 lead into TADR. On the routes where the exe
imports `TDRAW`/`TAESC` that slot leads into **Impure** and so is not surprising at all, which is
the landing. They are read on Wine only; the Windows column's places are sites of code alone.

A full Wine run takes about 10 minutes on the reference setup (572 s, 2026-09-28, 19 setups and 15
network games): about 50 s to make the instances, eight at a time, then the single-player runs six
at a time with the network games queued beside them, six at a time. The six battles are what hold
the machine at its limits — a load of 28 on 32 cores, and GPU memory near its 12 GB, which each
game's restorer sizes its atlas against (half of what is free) at any count. The Windows runs take
about 45 s a setup, one at a time; there is no battle or network stage there yet.

## Total Mayhem's maps with no terrain

**What a player sees.** A fresh Total Mayhem 11.3.0 on the Steam install shows a box naming
`Maps\A Plethora of Ponds.TNT` when the SKIRMISH screen opens, and the game goes no further.
MEASURED 2026-09-27 on Windows 10, with Impure: the box's text was read from the live window.

**Why.** `mayhem.gp3` carries 105 maps, and every one of them is only an `.ota` (the map's
settings: starting positions, resources, description) with no `.tnt` (the terrain). The terrain
always comes from the player's install. Matched by name against every archive of the Steam install
(275 maps with a terrain): 96 of Mayhem's `.ota` have one — 53 in `ccmaps.ccx`, 37 in `totala2.hpi`,
4 in `btmaps.ccx`, 1 each in `ccmiss.ccx` and `floggen.ufo` — and **9 have none anywhere**: A
Plethora of Ponds, Abysmal Lake, Ancient Issaquah, Cloudious Prime, Long Lakes, Luschie, Luschious,
Thundurlok Rok and Tropical Paradise. [INFERRED] They are downloadable maps from after the game's
release that the Steam install does not carry.

The engine lists a map by its `.ota` alone, so those 9 are in the list. Mayhem's Patch Loader moves
the registry to `Software\TotalM`, which starts with no `SkirmishMap`, and with none the engine
saves the list's first entry — alphabetically "A Plethora of Ponds" — and opens the SKIRMISH screen
on it (`0x430B82`, [the engine map](../exe-reverse-engineering.md), *The saved skirmish map*). Picking
any of the other 8 from the map list does the same.

**Not established yet:** whether stock Mayhem (its own `ddraw.dll`, no Impure) shows the same box
from the same key. The folder to answer it was built on Windows on 2026-09-27 and not yet run to the
SKIRMISH screen. The suite does not see any of this: its battle stage writes `Two Continents` into
the setup's `registry_roots` before the game starts, and the Windows runner has no battle stage.

**The fix to build: leave a map out of the list when its terrain is missing.** When the list builder
(`0x434AB0`, which enumerates `Maps\*.ota`) adds a map, it would first check that `Maps\<name>.TNT`
opens through the game's own archive layer, and skip the map if it does not. This fixes both ways
the box is reached, because a map that is not listed can be neither the saved default nor a pick
from the list. Checking only the saved name when the SKIRMISH screen opens would fix the first and
leave the other 8 maps as boxes. This is a property of the player's install, not of Mayhem, so it
applies to every route and every mod whose `.ota` reference a map that isn't installed. What is not
mapped yet: the entry the builder appends per `.ota`, the archive layer's open call to test with,
and whether the multiplayer map list is built by the same function (`0x434AB0`'s other modes,
`MULTI MAPS` at `0x504A24`).

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
