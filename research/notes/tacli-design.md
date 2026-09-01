# tacli — parallel instance launcher & driver: locked design (2026-09-01 interview)

*Outcome of the /grill-me interview for the "drive TA from a CLI" track. Goal: multiple
agentic sessions each driving their own TA instance on the real desktop, windowed, while
the human keeps using the PC. Companion reference: `cmdline-options.md` (every stock
launch knob), `resolution.md` (registry display mode), `runtime-injection.md`.*

## Locked decisions

| Branch | Decision |
|---|---|
| Display | **All instances windowed (cnc-ddraw `windowed=true fullscreen=false`) on the real display**, NVIDIA GL. Not Xvfb/Xephyr (software GL, unwatchable). Human interference solved properly by the phase-1.1 input firewall, not by hiding windows. |
| Windows | **1:1** (client area = game res), CLI auto-tiles via per-instance `posX/posY`. Default `--res 1024x768` (known-good; formulas verified), `--res WxH` free choice. |
| Isolation | `tagpu/instances/<id>/{gamedir,prefix}` (gitignored). Gamedir = fresh symlink mirror + instance-private files. Prefix = **`cp -al` hardlink clone** of the template `wineprefix/` (wine rewrites registry hives via temp+rename → template safe; TA writes go to gamedir). Separate prefix ⇒ separate wineserver ⇒ `wineserver -k` kills one instance only. |
| Sound | Default **off** via per-instance `totala.ini` `[Preferences] NoDirectSound=1` (official mechanism, see `cmdline-options.md`); `--sound` omits it. No wine audio-driver registry hacks. |
| Intro | Instance mirror **omits `Data/1.ZRB` + `Data/2.zrb` symlinks** — the `0x425ECF` find-file gate skips playback gracefully. No patch, no keystrokes. `3/4/5.zrb` stay linked. |
| Resolution | Registry `DisplaymodeWidth/Height` written into the instance prefix pre-launch (stock exe has no res switches). |
| Input | Existing in-process file protocol (`tagpu_keys.txt`/`tagpu_eye.txt`, WM_* posts) — already per-gamedir and display-independent. CLI wraps it. xdotool era stays dead. |
| TA `-d` switch | **Off-limits** (engine windowed mode likely bypasses the ddraw path our stack lives in). cnc-ddraw windowed is the one true mode. |
| CLI | `tools/tacli`, Python 3 stdlib-only. Full driving surface v1: `launch ls keys click eye shot glshot video log roster wait stop rm`, JSON output, instance ids, `--map`/AI/LOS/speed skirmish presets via registry. Paths anchored at the **main checkout** (worktree-safe), env-overridable. `ddraw.dll` **copied** (pinned) into each instance at launch — rebuilds never corrupt running games. |
| Skills | New **`ta-drive`** repo skill = agent-facing workflow (launch→drive→observe→stop, one instance per session). `ta-capture` slimmed to instance-aware capture; deprecated xdotool sections removed. |

## Phases

1. **Windowed-black fix first** (gates everything; prime suspect = stale-GL-context class
   already solved for mode switches — see decisions memory "THE BIG CATCH" entry), then
   tacli, then skills. DoD: windowed instance renders game + native pass, input tokens
   land, both shot triggers work, two instances side-by-side with the desktop usable.
2. **Phase 1.1 — input firewall** — **DONE 2026-09-01**, exactly as scoped: wndproc drops
   hardware kbd/mouse when armed; injector posts tagged `WM_APP` codes translated in
   wndproc; `GetCursorPos`/`GetKeyState`/`GetAsyncKeyState` answered from injected state.
   Human crosstalk measured away, and `ctrl+d` self-destructed a selected commander —
   the old "ctrl-d never lands" gap is closed. Full write-up: **`input-firewall.md`**.
   Shield is **on by default** (`tacli shield <name> off` hands the game to the human).
3. **Phase 1.2 (opportunistic)**: trace `-b` battleroom words (`truelos`/`permlos`/
   `mapping`/`fixedloc`/…, table in `cmdline-options.md`) for registry-free rule presets.

## Why (constraints that shaped it)

Human keeps the PC: no fullscreen grabs, no X-level injection, no focus stealing; the
X cursor is shared state (TA polls it — mouselock/cursor-parking sagas), so hardware-input
isolation must happen inside the game process, not at the display. Parallelism: every
per-instance collision (registry res/skirmish keys, trigger files, logs, wineserver,
mode list) is resolved by the instance dir + prefix clone. Watchability was a hard
requirement — the user wants to see agents play.
