# The compat suite (tacompat)

## Summary

`tools/compat/tacompat.py` builds every [setup](setups.md), starts it, watches every window the
game opens, reads what each party logs, and compares the outcome with the setup's goal and its
accepted behaviour today. It runs all setups at once on Wine, with a 200-against-200 skirmish
wherever the menu comes up, and one at a time on the Windows test box. **Nothing may be
UNEXPECTED before a release.** The `ta-compat-check` skill has the whole loop and its traps.

## Running it

- `list` and `fetch`: fixtures with a URL download; the ones behind a browser check are fetched
  in a browser and imported with `fetch --import NAME=PATH`, which checks every member's md5.
- `wine`: every setup at once. Each gets its own `tacli` instance (`compat-<setup>`), private
  copies of the registry hives, its own Xvfb display, and the overlay of its fixtures on the
  retail gamedir. The runner watches the display's windows for the whole run (a refusal is a
  box, and a box is the evidence), reads `tdrawlog.txt`, `tagpu.log`, `startup-failure.txt`,
  `ErrorLog.txt` and the Wine `+loaddll` trace, then fights the battle where the menu came up.
- `windows`: one setup at a time on the Windows test box, over SSH, with the game and a window
  watcher (`win-watch.ps1`) started as scheduled tasks in the logged-on session, because a
  process started over SSH cannot see the desktop's windows.

A run is **meets goal**, **known gap** (matches `today`) or **UNEXPECTED**; the exit status is
1 on any UNEXPECTED. The rule: no UNEXPECTED before a release.


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
