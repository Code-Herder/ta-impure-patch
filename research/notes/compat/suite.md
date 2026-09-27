# The compat suite (tacompat)

## Summary

`tools/compat/tacompat.py` builds every [setup](setups.md), starts it, watches every window the
game opens, reads what each party logs, and compares the outcome with the setup's goal and its
accepted behaviour today. It runs all setups at once on Wine, with a 200-against-200 skirmish
wherever the menu comes up and a two-player network game wherever Impure runs, and one at a time
on the Windows test box. Every run is also read for **any of TADR's code having run**: the goal
of a player's setup is Impure running with none of it. **Nothing may be
UNEXPECTED before a release.** The `ta-compat-check` skill has the whole loop and its traps.

## Running it

- `list` and `fetch`: fixtures with a URL download; the ones behind a browser check are fetched
  in a browser and imported with `fetch --import NAME=PATH`, which checks every member's md5.
- `wine`: every setup at once. Each gets its own `tacli` instance (`compat-<setup>`), private
  copies of the registry hives, its own Xvfb display, and the overlay of its fixtures on the
  retail gamedir. The runner watches the display's windows for the whole run (a refusal is a
  box, and a box is the evidence), reads `tdrawlog.txt`, `tagpu.log`, `startup-failure.txt`,
  `ErrorLog.txt` and the Wine `+loaddll` trace, then fights the battle where the menu came up.
- **The network stage** (`wine --mp SECONDS`, 30 by default, 0 for none): for each player's
  setup whose single-player run showed Impure running, a second instance of the same folder
  (`compat-<setup>-j`), both games on their own displays, hosted and joined through the game's
  own battle room over Windows' DirectPlay (the walk of `tools/mp_lobby.sh`), each side applying
  its half of a small fight (`scenarios/compat-mp-host.json`, `compat-mp-join.json`), then
  watched on both. One network game at a time, queued as the single-player results come in:
  DirectPlay's name server owns UDP 47624 for the whole machine. A holder of that port in one
  of the suite's own prefixes is stale and ended; any other is someone else's game, waited for
  five minutes and never touched, and then the stage fails as not run.
- `windows`: one setup at a time on the Windows test box, over SSH, with the game and a window
  watcher (`win-watch.ps1`) started as scheduled tasks in the logged-on session, because a
  process started over SSH cannot see the desktop's windows.

A run is **meets goal**, **known gap** (matches `today`) or **UNEXPECTED**; the exit status is
1 on any UNEXPECTED. The rule: no UNEXPECTED before a release.

## Whether TADR ran

`tadr_ran` in a setup's goal is judged from what each part of TADR leaves in the game folder,
on every peer of every stage:

- **`tdraw.dll`** writes `tdrawlog.txt` from its `DllMain`: the file at all means it started
  (the result says too whether it installed its engine patches).
- **The recorder** (`tplayx.dll`, or the 2006 `dplayx.dll`) writes `log\TA Demo Recorder Log
  -<date>.txt`, and a line `DLL.DirectPlay…` in it only from inside one of its DirectPlay
  exports, which is where it starts. A log without such a line is a recorder that loaded and
  never ran — which is what the takeover leaves, since the loader's forwarders load it anyway.
  A demo file is no evidence either way: the 2006 recorder wrote none in a network game while
  it ran.

`retail+tadr1-recorder` is the check of that evidence: the 2006 recorder with the takeover
switched off, whose goal is `tadr_ran: true`. Measured 2026-09-26, before the recorder's part of
the takeover existed, every setup carrying a recorder had run it — the 2006 one, the three
Patch Loader setups, Total Mayhem, ProTA and gammata's — at the game's first DirectPlay call, in
single player.


## Adding a setup

Every third-party file is a fixture in `tools/compat/fixtures.json`, named by md5; nothing
third-party is in the repository. A setup in `tools/compat/setups.json` lists the fixtures it
overlays on the retail folder, its `goal`, and, while it falls short, its `today` — written from
a run, not from expectation. The commit that makes a setup meet its goal deletes its `today`.
A Patch Loader that moves the registry (`RegistryPath=`) needs the setup's `registry_roots`, or
the battle stage opens on a map the mod does not ship. `levers` are empty files the setup puts in
the game folder — a DLL switch. A setup with one checks a mechanism rather than a player's
folder: `mayhem-11.3.0-net` switches the takeover off (`tagpu_takeover.off`) so that the safety
net has something to catch, and its goal is the net's refusal.
