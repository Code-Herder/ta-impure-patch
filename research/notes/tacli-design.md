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
| UI layer | **`tacli ui` — Playwright-CLI model over TA's own gadget tree** (`main+0x531 → ControlsAry`, stride `0x15B`). On-demand snapshot only (trigger → `tagpu_ui.json`), never periodic. Act **by gadget name** with strict mode (duplicate = error, not first-match); actuation is a **synthesized click** through the existing input path, never an engine call; Playwright-style **auto-wait + actionability** (exists · `active` · not `grayedout` · top GUI) and **post-action settle**; `UIChange_f` confirms the click landed. Verbs `click set fill press wait show`. Full RE backing: **`gui-gadgets.md`**. |
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
3. **Phase 1.2 — command-line trace** — **DONE 2026-09-01**. Every switch the binary
   parses is now traced to the global it writes and A/B-confirmed on a live instance;
   full table, evidence and reproduction in **`cmdline-options.md`**.
   Outcome: **no registry-free rule presets exist.** `-b` is broken in the stock
   binary (its handler forgets the `add edi,2` every other handler does, so nothing
   ever matches) and ten of its eleven words have empty bodies regardless. Skirmish
   rules stay on the registry — that decision is now measured, not assumed.
   The real rule surface is `online.dll` + `-c`, and it is network-only; noted as a
   lead for agent-vs-agent multiplayer, not taken.
   Shipped alongside: **`tacli peek`** (in-process memory reads via
   `tagpu_peek.trigger`, `tagpu/ddraw/inc/tagpu_peek.h`) and
   **`tacli launch --arg=<switch>`**, which refuses `-r` and `-d`.

4. **Phase 1.3 — text-driven UI (`tacli ui`)** — **DONE 2026-09-01**, scoped by
   `/grill-me` and shipped the same session. RE backing: **`gui-gadgets.md`**. Drives
   menus, options and the in-game build panel from text instead of screenshot-and-guess,
   on the Playwright **CLI** model — snapshot the current screen, then act on a named
   gadget. `tagpu/ddraw/src/tagpu_ui.c` (walk) + `tacli ui` (verbs).

   Locked shape:
   - **Source**: in-process walk of the live gadget array, not `.GUI` file parsing and not
     OCR. One chain covers shell menus, dialogs and the in-game panel.
   - **Snapshot**: top GUI's gadgets in full + a breadcrumb of the screens beneath (only the
     top GUI is interactive, so listing covered gadgets would advertise dead affordances).
     Surface size in the header — the front end is 640×480 whatever `--res` says.
   - **Selectors**: TA's own `name[16]`, `type:name` to disambiguate, strict mode on
     duplicates. No minted refs: every action re-resolves from a fresh snapshot, so the
     stale-handle bug class does not exist.
   - **Actuation**: synthesized click at the rect centre through `tagpu_keys.txt` — one
     input path, rule 2 of the skill intact. Rects are absolute game-space, so
     `(xpos+width/2, ypos+height/2)` needs no scaling.
   - **Waiting**: auto-wait before the action, settle after it, 5 s default (Playwright's own
     `expect` default), `--timeout` / `--no-wait`, plus a standalone `wait`.
   - **Toggles**: declarative `set <name> <stage>` bounded by `stages`; `check`/`uncheck` are
     aliases. `assoc` shown as a group tag so radio side-effects are legible.
   - **`fill`**: click-to-focus + clear + type, *not* `GUIGADGET_SetText` — same observable
     contract, no second actuation path.
   - **Build pages**: `click` is strict (page 2 lives in an unloaded `.GUI`), `--page N` is the
     explicit opt-in; snapshot reports `page n/m`. `ui` is the gadget layer only — the world
     view stays with `click`/`eye`/`roster`.
   - **Deferred (phase C)**: listbox items (`0x4B6AF0` node layout) and slider position
     (`+0x136` vs `+0x142`). Rendered `items=?` / `value=?` — *not implemented*, never *empty*.

   **Outcome — both DoDs met.** Snapshots verified on MAINMENU/SINGLE/SKIRMISH (640×480)
   and the in-game panel (1024×768); `MAINMENU→SINGLE→Skirmish→Start→ARMSOLAR→placed`
   driven entirely by gadget name, **no screenshot between steps and no throwaway key** —
   auto-wait did retire the dropped-first-key workaround for `ui` (blind `keys` sequences
   still drop theirs, and `SKILL.md` says so). `crdefault`/`escdefault`/`defaultfocus`,
   `stages`, `status_curnt` and `text` all confirmed live; 2-stage toggles and 3-stage
   cycles both render and `set` correctly.

   **Two corrections the live run forced**, both worth remembering:
   - **Gadget coordinates are panel-relative, not absolute.** Shell menus are full-screen
     panels at `(0,0)` so the distinction is invisible, but the in-game build panel sits
     at `(0,128)` and every raw rect must have that added. Read as absolute, `ARMSOLAR`'s
     click lands inside the minimap and silently does nothing. The static reading missed
     this; **only the pixel cross-check caught it** — which is exactly why that DoD item
     existed.
   - **`quickkey` is a `u8` ASCII accelerator**, not the `i16` the corpus declares.

   Still open (phase C, unchanged): listbox items and slider values. `help` (`+0x33`) read
   empty on every screen tested and `grayedout` never non-zero — TA expresses at least
   some unavailability through `active` instead (`SINGLE.GUI`'s `AnyMsn`).

## Why (constraints that shaped it)

Human keeps the PC: no fullscreen grabs, no X-level injection, no focus stealing; the
X cursor is shared state (TA polls it — mouselock/cursor-parking sagas), so hardware-input
isolation must happen inside the game process, not at the display. Parallelism: every
per-instance collision (registry res/skirmish keys, trigger files, logs, wineserver,
mode list) is resolved by the instance dir + prefix clone. Watchability was a hard
requirement — the user wants to see agents play.
