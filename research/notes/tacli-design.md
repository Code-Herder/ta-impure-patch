# tacli — parallel instance launcher & driver: locked design (2026-09-01 interview)

*Outcome of the /grill-me interview for the "drive TA from a CLI" track. Goal: multiple
agentic sessions each driving their own TA instance on the real desktop, windowed, while
the human keeps using the PC. Companion reference: `cmdline-options.md` (every stock
launch knob), `resolution.md` (registry display mode), `runtime-injection.md`.*

## Locked decisions

| Branch | Decision |
|---|---|
| Display | **All instances windowed (cnc-ddraw `windowed=true fullscreen=false`) on the real display**, NVIDIA GL. Not Xvfb/Xephyr (software GL, unwatchable). Human interference solved properly by the phase-1.1 input firewall, not by hiding windows. |
| Windows | **1:1** (client area = game res), CLI auto-tiles via the per-instance store's `window=` (`write_placement`). Default `--res 1024x768` (known-good; formulas verified), `--res WxH` free choice. **Every tile lands inside the screen** and slots are held by *running* instances only — corrected 2026-09-02, see below. |
| Isolation | `tagpu/instances/<id>/{gamedir,prefix}` (gitignored). Gamedir = fresh symlink mirror + instance-private files. Prefix = **`cp -al` hardlink clone** of the template `wineprefix/` (wine rewrites registry hives via temp+rename → template safe; TA writes go to gamedir). Separate prefix ⇒ separate wineserver ⇒ `wineserver -k` kills one instance only. |
| Sound | Default **off** via per-instance `totala.ini` `[Preferences] NoDirectSound=1` (official mechanism, see `cmdline-options.md`); `--sound` omits it. No wine audio-driver registry hacks. |
| Intro | Instance mirror **omits `Data/1.ZRB` + `Data/2.zrb` symlinks** — the `0x425ECF` find-file gate skips playback gracefully. No patch, no keystrokes. `3/4/5.zrb` stay linked. |
| Resolution | Registry `DisplaymodeWidth/Height` written into the instance prefix pre-launch (stock exe has no res switches). |
| Input | Existing in-process file protocol (`tagpu_keys.txt`/`tagpu_eye.txt`, WM_* posts) — already per-gamedir and display-independent. CLI wraps it. xdotool era stays dead. |
| TA `-d` switch | **Off-limits** (engine windowed mode likely bypasses the ddraw path our stack lives in). cnc-ddraw windowed is the one true mode. |
| CLI | `tools/tacli`, Python 3 stdlib-only. Full driving surface v1: `launch ls keys click eye shot glshot video log roster wait stop rm`, JSON output, instance ids, `--map`/AI/LOS/speed skirmish presets via registry. Paths anchored at the **main checkout** (worktree-safe), env-overridable. `ddraw.dll` **copied** (pinned) into each instance at launch — rebuilds never corrupt running games. |
| UI layer | **`tacli ui` — Playwright-CLI model over TA's own gadget tree** (`main+0x531 → ControlsAry`, stride `0x15B`). On-demand snapshot only (trigger → `tagpu_ui.json`), never periodic. Act **by gadget name** with strict mode (duplicate = error, not first-match); actuation is a **synthesized click** through the existing input path, never an engine call; Playwright-style **auto-wait + actionability** (exists · `active` · not `grayedout` · top GUI) and **post-action settle**; `UIChange_f` confirms the click landed. Verbs `click set check uncheck select hover fill press wait show`. Full RE backing: **`gui-gadgets.md`**. |
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

   Also corrected in the locked text above: the **Actuation** bullet's "rects are absolute
   game-space" is what the design assumed; the live run replaced it with the panel-relative
   rule, and the fork applies the transform before reporting.

5. **Phase 1.3c — the deferrals closed** — **DONE 2026-09-01**, same day, second live
   pass. Everything listed as "still open" above is now resolved and verified on a running
   instance. Evidence: `gui-gadgets.md` §2.4–2.6, §7, §7.1, §9.

   - **Listbox items ship.** There was never a node structure to reverse: `0x4B6AF0` walks
     a **flat blob** at `+0xC2` counting `\0`/`\n`, with the count in `+0xC0`. Items are
     rendered in the snapshot and `ui select <list> <item|#index>` clicks the row.
     The **picture** flavour (`attribs & 0xA0`, entries at `+0xC6`) carries no text at
     all, so it stays `items: null` — and an *empty* text list is now `[]`, which is a
     different answer and is reported differently. Verified on `SELPROV`'s four
     DirectPlay providers and on `LOADGAME`'s save list, empty and non-empty.
   - **Slider values ship.** The value is `knobpos`, an `i16` at **`+0x140`** — *neither*
     of the two offsets the design guessed: `+0x136` is `range` and `+0x142` is
     `knobsize`. Rendered as `value=N/range`. A slider `set` verb was **not** added: TA's
     sliders are scrollbars bound to a listbox by `assoc`, so the thing an agent wants to
     move is the list, and `ui select` moves it.
   - **`fill` works** — first execution of shipped-but-untested code, on `LOADGAME.GUI`'s
     `GAMENAME`. One change was needed: typing through key tokens capitalised the whole
     batch, because the shield **holds** a modifier for 150 ms (TA polls it) and the
     `shift+c` in "Claude" was still down for "laude". `fill` now types through the
     `char:` WM_CHAR token, which carries no modifier state; `Test_2` round-trips exactly.
     What a field then *keeps* is TA's rule, not ours — `GAMENAME` drops `- . , ! # @` —
     and `fill` reports the field's actual content rather than claiming success.
   - **`help` (`+0x33`): the offset was right and the engine wipes it.**
     `GUI_ParseCommonFields` reads the TDF value into the field and then memsets it
     (`0x4AD4A9`) before a translation round-trip. Some screens write it at runtime and
     those read back — `SKIRMISH`'s `StartLocation`/`CommanderDeath` carry real sentences
     live. So `help` stays in the contract; it is simply empty most of the time.
   - **`grayedout` (`+0x13C`) is real** and fires. Only *builders'* build pages declare it
     (never the commander's, which is why the first pass never saw it): selecting a built
     `ARMLAB` shows `IGPATCH`, `ARMATTACK`, `ARMDEFEND`, `ARMBLAST` as `grayed`, with
     `active=1`. Both branches of the actionability check are needed; neither is dead.
   - **`assoc` ruling** (Q9's open sub-point). Rendering it on every row was rejected:
     every gadget has one. It is shown **only where the engine acts on it** — 2+ toggles
     sharing a group (`0x4A6A40`'s radio reset) or a slider bound to a listbox
     (`0x4A3EF0`). On `SELPROV` that is exactly the `DPLAY`/`SLIDER`/arrows quartet; on
     `SKIRMISH` nothing qualifies and no column appears.
   - **`hover` added**, closing the verb that went missing between the interview and the
     grammar. It is a move with no click and no pointer parking (a new `pmove:` token —
     `mouse:` falls back to `SendInput` when the shield is down, which would drag the
     human's real pointer) and it reports label text that appeared. No tooltip surfaced
     over the build panel, which is consistent with `help` being empty everywhere there.
   - **Two input bugs fixed on the way**: `tagpu_keys.txt` was read into a 256-byte buffer
     and then deleted, so a batch longer than that lost its tail silently — the buffer is
     now 1 KB and `tacli` splits long batches and waits for each to be consumed. And the
     token log built its line with `_snprintf`, whose `-1` on truncation walked the write
     cursor backwards out of the buffer.

   - **`tools/test_tacli.py`** — the first tests `tacli` has had. Stdlib `unittest`,
     no game and no wine: every function that turns a snapshot dict into a decision
     (which gadget a selector names, where a listbox row is, what to type, what the
     table says) runs against synthetic snapshots. `python3 tools/test_tacli.py`.
     It earned itself immediately by catching a live bug: `_ui_keyname` carried a
     `VK_F1..VK_F12` branch, but `quickkey` is **ASCII** — `0x70` is `'p'`
     (`ARMPATROL`'s accelerator), not F1 — so the branch was unreachable for every
     real value and wrong for the one value that could reach it.

6. **Phase 1.3d — every gadget type is now driveable** — **DONE 2026-09-01**. Three
   parallel RE passes closed the last capability gaps; evidence in `gui-gadgets.md`
   §2.4.1, §2.6.1 and the picture-list part of §2.4.

   - **Slider `set` shipped.** `ui set FXVOL 32` takes the *value* the engine acts on,
     not the pixel offset: `value = knobpos*thick/(range-1)` truncating, inverted with
     the engine's own ceil so every value round-trips. Actuation is the gesture
     `Slider_HandleMouse 0x4A4170` implements — press on the knob, move, release, 1 px
     per unit — spread over four input batches, because the press frame only captures
     and a gesture posted in one batch leaves the engine looking at a released button.
     Verified live on `SOUNDS.GUI` `FXVOL` at 32 / 64 / 0 / 48 / 7, with the engine's
     SFX-volume global at `main+0x37F0C` following each time. A slider bound to a
     listbox by `assoc` is a scrollbar and `set` refuses it, pointing at `select`.
   - **`select` scrolls.** Reaching an off-screen row is the arrow keys and nothing
     else: the scroll arrows move the knob one *pixel* (usually no rows), the track
     click likewise, and TA has neither page-up/down nor a mouse wheel. `select` clicks
     the nearest visible row for focus, then steps, polling the selection rather than
     counting presses — a rapid batch is partly dropped, and a `&G` separator swallows
     a press without moving. 96 rows in ~8 s on `SELMAP`'s 99-map list.
   - **Map selection is no longer bypassed.** `SELMAP.GUI`'s `MAPNAMES` turned out to
     be a *text* list, as are `NEWGAME.GUI`'s `Campaign`/`Missions` and `SELGAME`'s ten
     columns — only `RESTRICT2`'s `PICLIST` and `LOGOSEL`'s `LOGOS` are picture lists in
     this binary, and their entries are GAF frame headers with no text in them at all.
     `MAINMENU -> Skirmish -> SelectMap -> select "Anteer Strait" -> LOAD` drives the
     map from the CLI, which `--map` cannot do to a running instance.
   - **Two silent-failure modes now refuse instead.** A `&G` row is a separator the
     selection slides off, and `SetListText`'s fifth argument is a per-item enable array
     at `+0xD6` (`RESTRICT2` uses it). Both are reported, and `select` names them rather
     than clicking at something that will not take.
   - **Deliberately not added**: a `dblclick` verb. The engine does detect double
     clicks (`0x4AB570`), but every list screen in the stock set pairs its list with an
     explicit button (`LOAD` / `CANCEL` / `DELETE`), so `select` + `click` covers it
     without a second actuation path — the same reasoning that kept `fill` off
     `GUIGADGET_SetText`.
   - **Still unexercised live**: the per-item flag array. `RESTRICT2.GUI` is the only
     screen that sets it and it hangs off the multiplayer lobby, which the `SELPROV`
     crash blocks. It is read and reported; it has never been seen non-null.
   - **Settle got sharper**: a consequence in the clicked gadget's own `assoc` group
     counts, so a scroll-arrow click reports the slider it moved instead of "no
     GUI-visible change". Unnamed gadgets are labelled `#7` rather than an empty string.

   **Route notes that cost time and should not be re-derived:** the in-game menu is
   **`Tab`** (not `Esc`, which does nothing in game), reaching `ARMOPT.GUI`; `SAVEGAME.GUI`
   / `SAVELIST.GUI` / `OPTION.GUI` / `CMENU.GUI` ship in the HPI but are named by no string
   in the exe — the live save/load screen is `LOADGAME.GUI`. And on `SELPROV`, **only the
   `SELECT` button crashes**; moving the list selection is safe, so the provider list can
   be read without risking the instance.

7. **Phase 1.4 — JSON scenarios (`tacli scenario`)** — **SCOPED 2026-09-01** by
   `/grill-me`; **phases A (the compiler), B (the catalogues), C (the applier) and D
   (`load`) all built the same day**, E still to come. A strict JSON file describes a
   *situation* — 200 units
   fighting, a wreck of a chosen type, the camera already on it — and one command launches
   an instance, drives `ui click SINGLE/Skirmish/Start`, waits for the game and spawns it by
   calling the engine's own `UNITS_CreateUnit` / `SpawnFeatureOnMap` / `Order2Unit`.
   Nothing is addressed by numeric id: units, features, orders and engine switches are all
   **name-keyed and resolved against the live game**, so a mod that swaps what an index means
   cannot corrupt a scenario — the failure mode TA's binary `.sav` is built out of.
   The CLI compiles and validates; the DLL scans a private line format and re-checks
   everything, creating **nothing** unless every entity resolves. Full design, engine recipe,
   schema and phases: **`scenario-format.md`**.

   Live today, with no game and no wine: `scenario list` / `validate <name>` /
   `expand <name> [--wire]`, and `scenarios/200v200.json`. The compiler is the component
   that decides what reaches the engine, so it is the one with exhaustive offline tests —
   116 of `test_tacli.py`'s 181.

   **From nothing: `scenario load <inst> <name>`.** One command launches with the file's
   own `setup` (map, resolution, players, `unit_limit` via `totala.ini`), drives
   `SINGLE → Skirmish → Start` by gadget name, waits for a world, reads back the map the
   engine *actually* loaded — TA falls back silently on one it does not have — and applies.
   A stopped instance to a live 200v200 with the camera on the collision point takes 6.4
   seconds. It needed no fork change: every step was already a verb, which is the argument
   for having built them as verbs.

   Against a running **game**: `scenario apply <inst> <name>` spawns the situation by
   calling the engine's own creation functions from a detour at `0x4969D2`, inside TA's
   main loop and outside the renderer, and reports per entity what was requested and where
   it actually landed. `tacli switches <inst> shootall=on` sets the `SoftwareDebugMode`
   bits live on any instance, game or menu. The fork side is `tagpu_scenario.c`. Verified
   the day it was written: 402 entities and 400 orders in one visit, every position exact,
   no spawn hitch — and it corrected four design claims, including the player-slot
   numbering the schema had wrong (`scenario-format.md`, *What the live runs corrected*).
   `switches` then settled what `shootall` (`0x400`) actually does, which was community
   lore in every source: idle units engage an enemy *building* in range only with the bit
   set. Same game, same six Peewees, one bit, 45 seconds each way.

   With a running instance: `tacli units` / `features` / `maps` / `catalogue` ask the
   game what exists and cache it per instance, and `scenario validate --instance <name>`
   checks every name against it. The fork side is `tagpu_cat.c`, one more on-demand
   trigger in the `tagpu_ui` mould. It paid for itself immediately: the design note's own
   example named `ARMCOM_DEAD`, and stock TA has no commander corpse at all.

## Window placement — the invisible-game failure [FIXED 2026-09-02]

Handing a game to the human to play turned up two placement bugs that between
them make a perfectly healthy instance show nothing at all.

1. **Slots were reserved by every instance that had ever been created**, not by
   the running ones. The twelfth instance drew slot 10.
2. **`tile_for()` never wrapped.** `cols` came from the screen width, `row =
   index // cols` grew without limit, and a 1920x1080 window on a 3840x2160
   screen fits one per row — so slot 10 was placed at **y = 11600**, below every
   monitor. cnc-ddraw honours `posX/posY`, so the game created its window there.

The symptom is nasty because nothing looks broken: the game runs, renders,
accepts injected input, drives its menus and answers `tacli weapons`/`roster`
normally. GNOME simply leaves a window that is outside every monitor **unmapped**
(`xprop -id <wid> WM_STATE` → `Withdrawn`), and `xdotool windowmove` on an
unmapped window changes its geometry without showing it — it needs `windowmap`,
and the WM may resize it on the way back in.

Fixed by wrapping and clamping the grid (`index %= cols * rows`, then pull the
last row/column inside the screen), reserving slots against *running* instances
only, taking the grid's screen size from the display instead of a hard-coded
3840x2160, and re-tiling on launch when a recorded tile no longer fits. Covered
by `WindowTiling` in `tools/test_tacli.py`.

The same session found the display-choice hazard next door: `default_display()`
took the inherited `DISPLAY` whenever it answered, and parallel agent sessions
leave 3840x2160 **Xvfb** servers running, so a shell that inherited one would put
the game on a display with no monitor behind it. It now skips virtual X servers
(Xvfb/Xephyr/Xnest, identified from `ps`) unless `TACLI_DISPLAY` names one
explicitly.

## Remote instances: a test folder on a Windows machine (G21c)

A remote instance is a game folder on another machine, driven by the same verbs as a local
one. It works because every channel between `tacli` and the DLL is a file in the game folder
(the key and eye files, the lever files, the trigger/result pairs, the `.ab` captures, `log\`),
and the DLL on Windows reads and writes the same files. The plan is
[Hardware portability](hardware-portability.html) §2 decision 5; the code is `tools/taremote.py`
(the link and every PowerShell statement) and the remote section of `tools/tacli` (the verbs).

### Making one

```
tacli remote add <name> --ssh <user>@<host> [--key <file>] --from <player folder> [--to <test folder>]
```

- **The player's folder is read once and never written.** `remote add` copies it with
  `robocopy` into the test folder (default `<user profile>\tacli\<name>`), then checks the copy
  holds as many files and bytes as the source.
- **The two folders are compared as the file system names them, before anything is written**
  (`Remote.resolve_folders`, then `check_folders` on what it returns). `GetFullPath` drops `.`,
  `..` and a trailing dot. Each existing component is replaced by the long name its directory
  listing gives (`Directory.GetDirectories(parent, component)` answers an 8.3 short name with
  the entry's long name). A junction or symbolic link anywhere in either path is refused, and
  so are two drive letters on one volume (a `subst` alias). A string compare alone let
  `C:\Games\TA.\test`, a short name or a junction land inside the player's folder.
  `check_folders` also refuses statically a component with a trailing dot or space, or a wildcard.
- **It refuses** when a `TotalA.exe` is running anywhere on the machine (the player's folder may
  be in use), when the player's folder has no `TotalA.exe` or is itself a test folder, when the
  test folder exists, and when its drive lacks the room. It also refuses when the user logged on
  at the console (`Win32_ComputerSystem.UserName`) is not the SSH login's user. TA reads
  `HKCU`, so the key the launch wrapper saves must be the hive the game will use.
- **The metadata and the marker come first.** `instance.json` is written with `state: adding`,
  then the test folder is created with its marker `tacli-test-folder.txt` (first line
  `instance=<name>`), and only then does `robocopy` run. A copy that fails half-way leaves an
  instance `tacli rm` removes; `launch` refuses one still `adding`.
- **The metadata is the only local trace**: `tagpu/instances/<name>/instance.json` (gitignored)
  holds `type: remote` and `remote: {ssh, key, player, folder, task, console_user}`. The same
  directory holds the log mirror (`remote-log/`), fetched captures (`ab/`) and a copy of the
  last registry export (`registry-before.reg`). Commands print the test folder's paths in their
  messages (a crash report, a remote error); tracked content never carries them.
- **The test folder carries tacli's own files**: the marker, and `tacli-state\` (the
  copied-file list, the launch wrapper and its status file). `rm` deletes the folder only
  when the marker names the instance being removed.
- A remote instance whose metadata no longer parses is skipped, with a warning, by the loops
  over every instance (`ls`, the slot and window layout of a local launch), so it cannot block a
  local launch. A verb aimed at it refuses.

### Routing

`_route_remote` runs before every verb. A verb aimed at a remote instance runs its remote form,
or it refuses before a statement is sent or a local file is touched. The table is keyed by the
verb's **handler function**, not by `args.cmd`, because a subcommand's own positional can carry
that name: `order`'s order is `cmd`, so `tacli order r1 stop` parses with `args.cmd == "stop"`.

| verb | on a remote instance |
|---|---|
| `launch`, `stop`, `rm` | their remote forms (`cmd_remote_*`) |
| `arm`, `keys`, `ui`, `eye`, `shield`, `crash`, `scenario load`, `ab` | the local code, over a `RemotePath` |
| `log` | the local code, over a local mirror of the current run |
| `remote add`, `remote restore` | remote-only verbs |
| everything else | refused: "`<verb>` does not reach a remote instance" |

Before every routed verb except `launch` (which refuses instead), `_route_remote` prints a
warning when TA's registry key on that machine still holds a test launch's values (below).

`Instance` reads its metadata on construction. For a remote instance `gamedir` is a
`taremote.RemotePath`, which offers the `pathlib.Path` methods the file channels use
(`exists`, `stat`, `read_text`, `write_text`, `unlink`, `rename`, `glob`, `mkdir`). It is
**not** `os.PathLike`, so `open()`, `shutil` or `subprocess` raise `TypeError` on it rather
than act on a local file of the same name. Every `RemotePath` is built from components checked
by `_component`, so none can name anything outside the test folder. `Instance.pid()` is the
`TotalA.exe` whose path lies in the test folder; `window()` is None.

### The link, and the one-statement rule

One PowerShell per tacli command: `ssh -T -o BatchMode=yes … <user>@<host> powershell
-NoProfile -NonInteractive -Command -`, fed statements on stdin and kept open (`Session`). One
session serves every `Instance` object naming the same link (`session_for`), and `close_all`
ends each with a blank line at exit. `-o IdentitiesOnly=yes` goes with `--key`, so no other
key of the agent is offered. A restarted session reads from a fresh queue, so the old reader's
end-of-stream never reaches it. **[MEASURED 2026-09-25, the Windows test setup]:** 0.7–0.9 s to
open the session, 25–80 ms per statement after that, 9.5 MB/s reading a file.

PowerShell reading stdin runs line by line, and **a statement that spans lines is skipped
silently**. So `ps_script` is the one function that makes a script, and it is unit-tested:

- `ps_check_statement` refuses anything that could continue onto the next line: control
  characters, an unclosed bracket or single-quoted string, a trailing `|` or `,`, a backtick,
  a here-string. It also refuses what tacli never needs: double-quoted strings (they expand `$`
  and backticks), `#` comments and non-ASCII.
- **Statement i runs only if statement i−1 ran and succeeded**, by a sequence counter: line i
  is guarded by `$__seq -eq i` and sets `$__seq = i+1` only after its statement returned. A
  line PowerShell skipped ran nothing, so it never advanced the counter, and **no line after it
  runs**: a `Remove-Item` after a skipped marker check, or a `Move-Item` after a skipped backup,
  cannot run unchecked. A failure sets the counter to −1.
- Each line resets `$ErrorActionPreference` to `Stop` first, prints `DONE <batch> <i>` after its
  statement, and on a failure prints `ERR <batch> <i> <type> <message>`. `Session.run` fails on
  an `ERR` and on a statement with no `DONE`. What the static check cannot see, such as
  `if ($true)` without its block (a parse error on stderr and no output at all **[MEASURED]**),
  fails the run instead of returning nothing.
- **The reported exception is the innermost one.** PowerShell hands a catch block a .NET
  method's exception wrapped in `MethodInvocationException`, so a missing file would read as
  that and not as `FileNotFoundException`, which the callers key on.
- The markers carry a random per-session token and their batch number, so a late line of an
  earlier batch is never read as the current one.
- Error messages, and every path or name the far side sends back (processes, the profile
  folder, the console user, file names), travel in base64. The session's stdout stays in the
  console's code page even after `[Console]::OutputEncoding` is set to UTF-8: the Windows test
  setup's French accents came out as replacement characters **[MEASURED]**, and a non-ASCII path
  would then match nothing.
- `ps_str` gives any string as one line of PowerShell: printable ASCII as a single-quoted
  literal (quotes doubled; `$`, backticks and `"` stay plain), anything else, a newline
  included, as a base64 literal decoded on the far side.
- Native commands (`reg.exe`, `robocopy.exe`) run with stderr discarded under
  `$ErrorActionPreference = 'Continue'` and are judged by `$LASTEXITCODE`, never by what they
  print, which is in the machine's language.

### Writing into the test folder

- **Every write replaces the file whole**: written as `<name>.tacli-tmp`, then moved over the
  target, so the DLL never reads half of one. The move is retried when a reader holds the target.
- **Key batches are never appended.** The DLL reads `tagpu_keys.txt` whole and deletes it; an
  append could land between the read and the delete and be lost with the file. A remote batch
  is written under a temporary name and **moved into place only when the name is free**
  (`RemotePath.create_new`), and `.NET`'s `File.Move` never overwrites. So the DLL only ever
  sees a complete batch, and tacli, the only writer of the name, waits for each to be taken.
- **Nothing of the player's copy is replaced or deleted without its original beside it**
  (`RemotePath._guard`, which runs before every write, delete and rename, on both ends of a
  rename):
  - The three files a launch replaces (`ddraw.dll`, `impure.cfg`, `totala.ini`) have their
    original recorded by `remote add`: a `.tacli-original` copy, hash-checked, or an empty
    `.tacli-absent` when the player's folder has none. Nothing touches one while that record is
    missing.
  - Every other copied file is listed in `tacli-state\copied.txt` with the SHA-256 it had when
    `remote add` copied it. Its first replace, delete or rename makes the `.tacli-original` from
    whichever of the test folder's copy and the player's file still has that hash, and refuses
    when neither does. "tacli has not written it yet" does not make a file original: the game
    rewrites some copied files itself (`tagpu_vk.gpus` at every start).
- **A DLL upload is proved**: `RemotePath.upload` compares the far side's MD5 with the local one.

### Launch, stop, and the registry

**TA's key (`HKCU\Software\Cavedog Entertainment\Total Annihilation`) is written only by a
launch wrapper on the remote machine, inside the game's own lifetime.** tacli never writes it:
there is no snapshot of "is the game still running" on this side for a restore to race.

`launch` on a remote instance, in order:

1. Refuses a test folder still `adding`. Refuses while a pending registry record exists, or
   while any `\tacli\` task is Running or Queued (a wrapper still owns the key). Refuses when
   any `TotalA.exe` that is not the test folder's runs: it may be the player's game. An
   instance already running reports so, as locally.
2. Uploads this tree's `ddraw.dll` (the one a local launch pins), unless `--keep-dll`.
3. Writes the harness files as a local launch does: `tagpu_shield.on` (unless `--no-shield`),
   `tagpu_nowarp.on`, `tagpu_defaults.off` (unless `--defaults`), the title label, `totala.ini`
   (silence, `--unit-limit`), the store's `resolution=` for `--res` and `maxfps=` for
   `--maxfps`. It rotates `ErrorLog.txt`, clears stale triggers and the key file, and auto-arms
   the `*own` halves.
4. Writes the wrapper, `tacli-state\launch.ps1`, generated by `ps_script` in two batches whose
   markers are appended to `tacli-state\launch.status` (the `sink`: nobody reads its stdout).
   Then it registers the task `\tacli\<name>` and starts it. The task has an interactive
   principal for the console user, the test folder as working directory, `-ExecutionTimeLimit`
   zero and `IgnoreNew`, and its action is `powershell.exe -NoProfile -NonInteractive
   -ExecutionPolicy Bypass -WindowStyle Hidden -File <wrapper>`. **`-File`, not
   `-EncodedCommand`**: the wrapper is longer than a command line allows. Not stdin either,
   because nothing stays attached to the task to feed it. A process started from the SSH
   session would run where nobody can see it.
5. Waits for the test folder's `TotalA.exe` **and** a `log\tagpu.log` whose `run/part` header
   differs from the one before the start: the DLL has attached and begun this run. A folder
   with no `log\` yet reads as "no header" (the innermost `FileNotFoundException` or
   `DirectoryNotFoundException`), not as an error. A crash report, or an `ERR` in the wrapper's
   first batch, fails the wait with its message.
6. Fetches the wrapper's export into `tagpu/instances/<name>/registry-before.reg`, checked
   against the SHA-256 in the record. It says that a TA started meanwhile by anyone reads the
   test values.

**The wrapper**, batch 1:

1. Refuses beside any running `TotalA.exe`.
2. Takes the pending record `%LOCALAPPDATA%\tacli\registry-pending.txt` with
   `File.Open(…, CreateNew)`, which is the lock: a second wrapper fails there and touches
   nothing.
3. Exports the key with `reg export` to `registry-before.reg` beside the record, and appends
   `key=present <SHA-256>` (or `key=absent`) to the record.
4. Writes the launch's values: `Interface Type` 1, `PlayMovie` 0, the six sound values unless
   `--sound`, the display mode for `--res`, `--map`, `--player`, `--los`, `--mapping`.
5. Starts `TotalA.exe` from the test folder and waits for it to exit.

Batch 2 runs whatever happened in batch 1, and only if the wrapper took the record:

1. Waits until no `TotalA.exe` runs at all.
2. Checks the export against its recorded hash, deletes the key and imports the export (or
   deletes a key that was absent before).
3. Exports again, compares the hashes, and **only then** removes the record.

The record and the export are **one per user of the machine**, in `%LOCALAPPDATA%\tacli`,
beside no test folder. The key is in `HKCU`, which every test folder of that user shares. A
record per instance let a second launch export a first one's test values as "the original",
which the next restore then put back.

**A restore that never ran** has one cause: the wrapper was ended (a restart, the task ended
by hand), since it removes its record before it exits and its task is Running while it lives.
So a record with no `\tacli\` task Running is reported loudly by every remote command, and
`tacli remote restore <name>` (any remote instance on that machine) restores it. That verb
refuses, in the same batch as the restore, while any `TotalA.exe` runs (a restore under a
running game deletes a key that game reads, and its own writes at exit overwrite the restore)
and while any `\tacli\` task is Running or Queued. When the export on the remote machine is gone
or changed, the local copy is put back first, and only if its SHA-256 is the one the record names.

**The residual, inherent in one `HKCU` key**: a TA started by anyone, the player included,
while a test game runs reads the test values. The wrapper does not restore under it: it waits
for every `TotalA.exe` to exit, so that player's changes to the key during the test are
replaced by the original at the restore. A TA started inside the restore's own second reads a
half-restored key. `launch` says so.

Flags that shape a wine instance on the local desktop (`--window`, `--display`, `--slot`,
`--dplay`, `--intro`, `--free-dplay-port`, `--shipped`, `--no-restore-pointer`) are refused.

`stop` stops only the test folder's `TotalA.exe` (`Stop-Process`) and waits for it to go. It
then waits up to 30 s for the wrapper and reports its restore from `launch.status`: "restored …,
by the launch wrapper". If the wrapper waits for another `TotalA.exe`, `stop` says which pid
and exits 0. If its restore failed ("TA'S REGISTRY KEY WAS NOT RESTORED", with the reason),
`stop` exits 1. `rm` stops the same way and keeps the instance while its restore is not done or
its record is pending (its local export copy is `remote restore`'s fallback). Otherwise `rm`
removes the task and deletes the test folder: the marker must name the instance, the folder must
still resolve to the path `remote add` recorded, and nothing inside it may be a junction or link.
The delete is `Directory.Delete`, not `Remove-Item -Recurse`.

### The shield on a remote desktop

It is the same file, with the same meaning. `tagpu_shield.on` in the test folder makes the DLL
drop the Windows desktop's hardware keyboard and mouse, so whoever sits at that machine cannot
perturb a test. The injected input still arrives, and `shield <name> off` hands the game to
them. `tagpu_nowarp.on` keeps the game off their pointer. **[MEASURED 2026-09-25]:** the log
carries `shield: ARMED (hardware input blocked)`. An injected `ctrl+a` was polled by the game
(`shield: vk=17 released after 172ms, polls=4 down=4`) and selected the units.

### Logs and captures

- **`log` and every log wait read a local mirror** (`taremote.sync_logs` into
  `tagpu/instances/<name>/remote-log/log/`) that talog reads like any gamedir. A file is known
  by its `run/part` header, as talog knows it. After a rotation, the bytes of the file that was
  `tagpu.log` are reused under its new name, so only what was appended since the last sync
  crosses the link. A file whose name, header and length are unchanged is neither fetched nor
  rewritten. Only the current run is mirrored, and only names of the log-file form
  (`<stream>.log`, `<stream>.<n>.log`) become local paths. Each read takes a file's header and
  its new bytes through **one open handle**: a rename moves the name, not the handle's file, so
  both come from the same file. A header that no longer matches, or a file gone since the
  listing, starts the sync again (four tries).
- **`tacli ab <name> <pass>`** (local or remote) removes the target `.ppm`, creates
  `tagpu_<pass>.ab`, waits for `vk: shot: wrote tagpu_<pass>_vk.ppm` or a refusal line, and
  removes the lever. A refusal is any of the lines `tagpu_vk.c` logs when an arming captured
  nothing, matched without regard to case ("nothing captured", "the A/B capture was lost to",
  "this arming is REFUSED", "nothing unlinked"). A remote capture is fetched to
  `tagpu/instances/<name>/ab/`; a local one stays in the gamedir unless `-o` names a place.
  **The file is this arming's by construction**: the target is removed before the lever
  exists, `tagpu_vk_ab_arm` removes it again when the claim latches, and only a `wrote` line
  logged after the call's own log mark counts.
  **The settle only affects whether a capture happens at all**. A pass clears its latch
  (`s_abDone`) only when its own poll sees the lever absent, every 30 frames (`tagpu_gui_surf.c`:
  every `POLL_MS` = 500 ms). No line logs that. So a lever removed less than `--settle` seconds
  ago (default 2 s, enough at 15 fps and up) is kept gone for the rest of that time before it is
  created again. A leftover counts, and so does the previous `ab` of the same pass, whose
  removal time is `ab_cleared` in `instance.json`. Without that, a second `ab` straight after a
  first re-created the lever before any poll and timed out [MEASURED 2026-09-25, a local
  instance on a private Xvfb: `ab gui` twice in a row; the second now succeeds after a 2 s
  settle].

### What the live gate measured [MEASURED 2026-09-25, the Windows test setup, main `8cbecb6`'s DLL]

These runs used the first design, in which tacli exported and restored the key itself. The
launch wrapper replaced it after the landing review (above); the wrapper's own live check is
listed under *Not covered live*.

- `remote add` copied 94 files (1051 MB) in 7 s.
- `scenario load marker-mix --res 1920x1080` went from a stopped instance to 5 units applied,
  the camera pinned, in 13 s: launch, `SINGLE → Skirmish → Mapped → Start`, the live wait, the
  loaded map read back (`Maps\Two Continents.TNT`).
- `ui` read `ARMMAIN2.GUI 1920x1080`, `eye` held the camera, `keys` delivered `ctrl+a`. The
  census read `gui=1` and no world pass: that card stands the world passes down (G21a).
- `ab g21c gui` wrote and fetched a 1920x1080 capture of the UI layer (the panel, the resource
  bars, the minimap and the cursor over a black world) in 3 s. It was the capture that works on
  that card today.
- `crash` read no report, and `stop` restored the key.
- **TA's registry key**: the 22 882-byte export was byte-identical before the launch, after
  `stop`, and after a stop missed on purpose (the game killed outside tacli, then `tacli log`).
  A mid-test export differed where the launch writes: `Interface Type`, the display mode,
  `SkirmishMap` and six sound values. So `reg export` of the key after `reg import` of its own
  export reproduces it byte for byte, which the wrapper's verification relies on.
- **The player's folder**: every file's SHA-256, size and write time were identical before
  and after the whole run, `rm` included.

**Not covered live:** the launch wrapper as a whole (its task action, `-ExecutionPolicy
Bypass`, its restore, the pending record), `remote restore`, the resolved-folder checks and
the SHA-256 backup tier. All are unit-tested against a model of PowerShell that runs the
generated lines. The Windows test setup was offline when they were built. Also not covered
live: `ui` verbs beyond the snapshot and the clicks of the `scenario load` path; a non-ASCII
path (carried in base64 and unit-tested, not run); `rm --force` on a running game. Two tacli
commands driving one remote instance at once are not supported: the key-file protocol assumes
one writer. `rm` leaves the empty `<user profile>\tacli` folder, `%LOCALAPPDATA%\tacli` and the
Task Scheduler folder `\tacli\` behind.

## Why (constraints that shaped it)

Human keeps the PC: no fullscreen grabs, no X-level injection, no focus stealing; the
X cursor is shared state (TA polls it — mouselock/cursor-parking sagas), so hardware-input
isolation must happen inside the game process, not at the display. Parallelism: every
per-instance collision (registry res/skirmish keys, trigger files, logs, wineserver,
mode list) is resolved by the instance dir + prefix clone. Watchability was a hard
requirement — the user wants to see agents play.
