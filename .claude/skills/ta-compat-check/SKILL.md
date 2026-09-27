---
name: ta-compat-check
description: Test a ddraw.dll against the setups players actually run — TADR, the Community Patch Loader, Total Mayhem, ProTA, TA Zero's files, Escalation, gammata's drop-in — on Wine (all at once) and on the Windows test box (one at a time), with tools/compat/tacompat.py. Use before pushing a release tag (required), after any change to the engine limits, a patch site, DllMain or the start-up checks, when a player reports a crash or error box with a mod or TADR installed, or when asked to test the DLL before publishing.
---

# Testing the DLL beside mods and TADR (tacompat)

`tools/compat/tacompat.py` builds each **setup** — the retail 3.1 game plus what a mod or patch
install puts next to it, plus the `ddraw.dll` under test — starts it, watches every window the
game opens, reads what each party logs, and compares the outcome with what the setup expects.
The routes, the outcomes and the engine facts behind them are in
the wiki's *Compatibility* section, `research/notes/compat/` (start at `overview.md`); read it before
changing a setup's expectation.

## The release rule

**Before a `v*` tag is pushed, the Wine suite runs on the release's DLL and nothing is
UNEXPECTED** (`CLAUDE.md` *Releases*). Run the Windows suite too when the owner allows the
SSH session; say in the hand-over which platforms ran. A known gap does not block a release;
an UNEXPECTED does, and so does a suite that could not run (exit 2).

## The loop

```bash
make -C tagpu/ddraw -j$(nproc)                       # the DLL under test (the default --dll)
tools/compat/tacompat.py list                        # setups, fixtures, what is missing
tools/compat/tacompat.py fetch                       # every fixture with a plain URL
tools/compat/tacompat.py wine                        # every setup at once, battles and network games
tools/compat/tacompat.py wine mayhem-11.3.0 prota-4.8   # a few, by name
tools/compat/tacompat.py wine --battle 0 --mp 0      # start-up only
tools/compat/tacompat.py wine mayhem-11.3.0 --mp 60  # a longer network game
tools/compat/tacompat.py wine --dll path/to/ddraw.dll   # a release zip's DLL
tools/compat/tacompat.py windows                     # the Windows box, one setup at a time
tools/compat/tacompat.py clean                       # remove the compat-* Wine instances
tools/compat/tacompat.py selftest                    # the hook decode, 11 cases, no game needed
```

- **Verdicts.** `meets goal` (Impure active, none of TADR's code run); `known gap` (matches
  the setup's `today`, the behaviour accepted until the takeover reaches it); `UNEXPECTED`
  (neither). Exit 1 on any UNEXPECTED, with `--strict` on a known gap too; exit 2 when nothing
  could run.
- **Outcomes**: `impure-active` (menu reached, frame packets published), `impure-inactive`
  (loaded, drew nothing), `impure-not-loaded`, `impure-refused` (Impure's own box, exit 0xC1,
  `log\startup-failure.txt`), `tadr-refused` ("TADR engine-limit error"), `loader-refused`,
  `crash` (a crash report or crash box before the menu), `battle-crash` (after the menu, while
  the skirmish loads or fights), `other-box`, `exited`, `no-result`.
- **The battle stage** (Wine only, on by default, 60 s): every setup that reaches the main
  menu clicks through to a skirmish on Two Continents and applies `scenarios/200v200.json`.
  Collisions between two patchers show where the limits are *used*, not at start-up: Impure
  v0.2.3 beside Total Mayhem 11.3.0 reached the menu and crashed on the first skirmish load, which
  a start-up-only run passes.
- **The network stage** (Wine only, on by default, 30 s): every player's setup whose
  single-player run shows Impure running also plays a two-player game — a second instance of the
  same folder (`compat-<setup>-j`), hosted and joined through the game's own battle room over
  Windows' DirectPlay, each side applying its half of `scenarios/compat-mp-host.json` /
  `compat-mp-join.json`. One game at a time, queued beside the single-player runs: DirectPlay's
  port is the machine's. The report's column reads `mp ok`, `mp FAILED` or `-` (not played).
- **Whether TADR ran** (`tadr_ran`), on every peer, from two kinds of evidence:
  - **the game folder**: `tdrawlog.txt` at all (tdraw started), or `log\TA Demo Recorder Log
    -<date>.txt` at all — the recorder creates one only when it starts from inside a DirectPlay
    export, and a `DLL.DirectPlay…` line in it names which. **This half can never prove the
    negative**: the recorder's other way in is the jump its `DllMain` splices over the exe's entry
    point, and it writes no log at all.
  - **the running game's own code** (`exe_hooks`): the exe's executable sections read out of the
    live process and compared with `TotalA.exe` on disk, each run of changed bytes decoded for the
    address an **instruction** in it leads to — four bytes that merely hold an address are reported
    as held and never counted — and that address tested against every module loaded from the game
    folder.
    A target in a module whose file carries `TADemo-MKChat` is TADR's code having run. A target in
    any other DLL of the folder is the mod's own byte and is reported, never counted — the Patch
    Loader rewrites three of the exe's import thunks into calls to the mod's `win32.dll`.
  - **A goal of `tadr_ran: false` is not met by a read that did not happen.** No process to read,
    the exe mapped away from its `ImageBase`, `ptrace_scope` refusing: the row says so and reads
    UNEXPECTED, the same rule as *a run is only evidence if the watcher saw it*.
  - On Windows `win-watch.ps1` does the reading and judges nothing — it dumps the changed runs and
    the module table for the same decoder.
- **Results** go to `$TACOMPAT_CACHE/results/<stamp>-<platform>/` (default cache
  `~/.local/share/ta-compat`, outside every repository because results carry absolute
  paths): `summary.txt`, and per setup the tdrawlog, `tagpu.log`, `ErrorLog.txt`,
  `startup-failure.txt`, the Wine log and a picture of the first box.

## Fixtures

`tools/compat/fixtures.json` names every third-party file by md5; nothing third-party is in
the repository. A fixture with a `url` downloads with `fetch`. One marked `manual` sits behind
a browser check (Cloudflare on tauniverse.com): download it in Chrome through the
claude-in-chrome tools (the owner allows it for fixtures), then
`tacompat.py fetch --import NAME=~/Downloads/<file>` — the import checks every member's md5.

## Anti-virus

Impure rewrites the host exe's code in memory (265 sites, plus detour trampolines) and, since T1c,
eight bytes over a TADR DLL's PE entry point — a write into a foreign module's image, which is the
one new shape, and `PAGE_READWRITE` rather than `PAGE_EXECUTE_READWRITE` for that reason.
**Defender says nothing to any of it** (MEASURED 2026-09-27: all 16 setups on the box, Defender
with `RealTimeProtectionEnabled`, `AntivirusEnabled`, `AMServiceEnabled` and
`BehaviorMonitorEnabled` all true — **0** events in
`Microsoft-Windows-Windows Defender/Operational` over the whole run and 0 in
`Get-MpThreatDetection`). Read that log again after a Windows session that changes how the DLL
writes memory, and say in the hand-over what it said. `win-watch.ps1`'s
`OpenProcess`/`ReadProcessMemory` is harness code and is never shipped.

## Windows

The box and its key are in `$TACOMPAT_CACHE/windows.json` (`--ssh`, `--key`, `--game` set it
once). **Ask the owner before each Windows session**: it is their desktop, and the game
windows appear on its screen. The runner refuses while any `TotalA.exe` runs, copies the
player's folder once (read only), then per setup rebuilds a work folder, starts the game and a
window watcher (`tools/compat/win-watch.ps1`) as scheduled tasks in the logged-on session, and
stops the game before reading its logs. One setup at a time, by design. There is no battle
stage on Windows yet.

## Adding a setup

A player's report names files: identify each by image size and md5 against the fixtures and
the TADR release list (the note's *Identifying a player's setup*). Add missing files as a
fixture (`files`: name → md5), then a setup in `tools/compat/setups.json`:

- `add`: fixtures in order; `files` is a list, `"*"` for all, or `{"name in folder": "name in
  fixture"}`; later entries replace earlier ones.
- `goal`: `{"outcome": "impure-active", "tadr_ran": false}` for every player's setup.
- `today`: what it does now, when that is not the goal; `today_windows` overrides fields on
  Windows. Also `tdrawlog`, `failure`, `loaded`, `not_loaded` to pin the evidence.
- `registry_roots`: where a Patch Loader moved the game's registry key (`RegistryPath=`), so the
  battle's map lands there too.
- `levers`: empty files put in the game folder, a DLL switch. A setup with one checks a mechanism,
  not a player's folder, and plays no network game: `mayhem-11.3.0-net` turns the takeover off
  (`tagpu_takeover.off`) so the safety net has TADR to catch, and its goal is the net's refusal;
  `retail+tadr1-recorder` turns it off so the recorder runs and answers a DirectPlay call, and its
  goal is `tadr_ran: true` — the proof that both kinds of evidence can fire at all (measured
  2026-09-27: its log, and 24 sites of the game's code leading into the recorder on Wine, 6 on
  Windows, where a start-up-only run never reaches its DirectPlay path).

Run the new setup before writing its `today` — write down what it did, not what you expect.
**The commit that makes a setup meet its goal deletes its `today`.**

## Traps

- **Never run two `tacompat.py wine` at once**: the instance names are fixed (`compat-<setup>`).
- Instances are created **one at a time**: `tacli create` writes the registry hive every
  prefix shares. Each instance then gets private copies of the hives before anything runs.
- The DLL refuses a `tacli-state` folder without the `-xtacli-test` token (registry test
  mode), so the runner renames it; the mods' exes are not launched through `tacli launch`.
- A Patch Loader that moves the registry (`RegistryPath=TotalM`, `ProTA`) starts with a fresh
  key whose default map the mod may lack: a "Maps\… .TNT" box. `registry_roots` fixes it.
- The main window is told from a box by size (≥ 560x400); a mod's window title can be plain
  "Total Annihilation".
- Wine's message-box traces carry no text: classify by window title and by the logs.
- On Windows, SSH cannot see the desktop's windows (every title reads empty); the watcher
  must run as an interactive scheduled task. The game holds `log\tagpu.log` open until it is
  stopped.
- **A run is only evidence if the watcher saw it.** A Windows run whose watcher never wrote its
  `done` line is `no-result`, never a guess from the logs: a retail setup still "meets the
  goal" on frames drawn alone, so a dead watcher hides behind the controls. Read a setup's
  `watch.jsonl` when a result surprises you.
- The watcher runs under the **32-bit** PowerShell (`SysWOW64`): a 64-bit one lists only the
  exe among a 32-bit game's modules. Windows PowerShell 5 writes its UTF-8 files with a
  byte-order mark, which the runner strips when it reads them.
- **A demo file proves nothing about the recorder**: the 2006 one wrote none in a network game
  while it ran. Its log is the evidence — and only of the DirectPlay way in, never of the
  entry-point one, which leaves the game folder untouched. Reading the process is what covers that.
- **Where a DLL lands in memory differs between Wine and Windows, and only an instruction is
  evidence.** Windows mapped the 2006 recorder at `0x020C0000` and Mayhem's at `0x00910000`;
  Impure's own jump displacements are about `0x020F0000` and its own `mov esi,[esi+0x92]` begins
  `8B 96 92 00` = `0x0092968B`. Judged as addresses, those refused two installs over 35 of our own
  patch sites, on Windows only. Hence: a verdict rests on `E8`/`E9`/`FF 15`/`FF 25`/`push;ret`/
  `mov;jmp`, and a held value is counted and left alone, in the DLL and in `decode_run` alike.
  **A finding whose site is one of Impure's own patch addresses (the `enginefix`/`limits` lines in
  `tagpu.log`, give or take one byte for the opcode) is a decode bug, not TADR** — check that
  before believing a refusal.
- **A refusal naming a DLL of the game folder may be the mod's own byte, not TADR's.** Impure
  refuses only for a target inside a module carrying TADR's marker; a `takeover:` log line ending
  `(not TADR: the mod's own)` is working as intended. If a legitimate mod DLL ever *does* refuse,
  the marker test is what to look at, never a widening of the rule.
- **Xvfb killed with SIGKILL leaves `/tmp/.X<n>-lock` behind**, and `free_display` counts a lock as
  a display in use: a few hundred runs used to exhaust `:180`–`:399` and the next run died with
  "no free X display number". `stop_xvfb` terminates and removes the lock. If a run dies that way
  again, look for locks in that range with no live Xvfb before blaming anything else.
- **The joiner's session list fills when SELGAME opens and on UPDATE, never by itself**: a host
  still busy with its map load is missing from it. The runner presses UPDATE while JOIN is grey,
  as a player would (Total Mayhem needed it one run in three).
- `DPlayHelpWndClass` is DirectPlay's own 1x1 helper window, opened with a session: not a box.
- **A host frozen at CREATE NEW GAME** (the click on Next never returns, no frame after it) was
  seen twice on 2026-09-27 while two `TotalA.exe` of another session were stuck on the machine —
  `<defunct>` in `ps`, with live threads — and not in the eight games after they were killed.
  The cause is not established; look for such a process before blaming the change (the port
  check sees only listeners), and kill one only with the owner's leave.

## Maintaining this skill

State the present; replace rather than annotate; the story of a run belongs in the note or
the commit. A new setup or outcome is documented in the note and here in the same commit.
