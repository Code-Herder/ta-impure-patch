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

## Why (constraints that shaped it)

Human keeps the PC: no fullscreen grabs, no X-level injection, no focus stealing; the
X cursor is shared state (TA polls it — mouselock/cursor-parking sagas), so hardware-input
isolation must happen inside the game process, not at the display. Parallelism: every
per-instance collision (registry res/skirmish keys, trigger files, logs, wineserver,
mode list) is resolved by the instance dir + prefix clone. Watchability was a hard
requirement — the user wants to see agents play.
