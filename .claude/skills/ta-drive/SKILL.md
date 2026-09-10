---
name: ta-drive
description: Launch and drive Total Annihilation from the CLI with tacli — isolated parallel instances, windowed on the real desktop, silent, no intro, scripted keyboard/mouse/camera, skirmish presets, and log/roster queries. Use when asked to run, start, play, test, or drive the game, or to run several game sessions in parallel.
---

# Driving TA from the CLI (tacli)

`tools/tacli` runs the game as **isolated instances**: each has its own gamedir, wine
prefix, wineserver, window, config and logs. Several agent sessions can each own one
instance without colliding, and the human keeps using the desktop meanwhile.

Reference: `research/notes/tacli-design.md` (design), `input-firewall.md` (how input
isolation works), `windowed-mode.md` (the display bugs this fixed and how they were
found), `cmdline-options.md` (every launch knob).
Capture and video work: the **ta-capture** skill.

## Rules of engagement

1. **One instance per session.** Name it after your task (`tacli launch fogtest`).
   Never drive an instance you did not launch — another agent may own it.
2. **Never use xdotool for keys or clicks.** All input goes through tacli, which
   writes the in-process token file the fork consumes. X injection is unreliable and
   has landed keystrokes in the user's unlock dialog (memory: `ta-input-injection-safety`).
   xdotool stays legitimate for *reading* window geometry and for capture. It is also
   pointless now: the shield (below) makes the game ignore hardware input anyway.
   **Never run `xrandr` against the live display**, and never move the human's pointer
   or activate windows to "test" something — that is their desktop.
3. **Silence is the default** and matters to the user: `NoDirectSound` plus six
   registry values plus dropping `music/`. Only pass `--sound` if asked.
4. **Clean up**: `tacli stop <name>` when done; `tacli rm <name>` when the instance
   has no further use. Leaving games running eats a GPU context each.
5. Rebuilt the fork? `tacli launch` re-copies `ddraw.dll` into the instance, so just
   relaunch. A running instance keeps its pinned copy (rebuilds cannot corrupt it).
   In a **worktree**, tacli pins the DLL your tree built (`<tree>/tagpu/ddraw/ddraw.dll`),
   falling back to the main checkout's — so build where you edit, or you will test the
   main checkout's binary and wonder why your change did nothing.
   **It picks that DLL relative to the `tools/tacli` you invoked, not your cwd**
   (`built_dll()`, `tools/tacli:77`), so run the worktree's own copy: `cd <tree> &&
   tools/tacli …`. Building in the worktree and then calling the *main* checkout's
   tacli silently launches the main checkout's binary, and the symptom is not an
   error — modules simply never appear in `tagpu.log` (no `terrown:` / `zoom:` /
   `vpwide:` ARMED line, no `terr:` grid line), which reads like a failed arm rather
   than a stale DLL. When a module you know is armed does not log, `md5sum` the
   instance's `gamedir/ddraw.dll` against your build before debugging anything else.
6. **When the human is going to play it, check the window is on their monitor**
   before handing it over — see *Where the window lands*. A game that is running
   perfectly but sits off-screen still answers every `tacli` command and shows
   them nothing, which is indistinguishable from a launch that failed.
7. **Arm every pass, unless the launch is a measurement.** "Launch the game" means
   the game with the work in it, not the 1997 renderer — see *The default arm set*.
   A bare `tacli launch` gives the stock engine with none of our passes, which is a
   control, not a demo. The exception is the A/B: an instance that exists to measure
   one pass arms only that pass.

## Where the window lands

Two separate things decide whether the human can see the game. Each has cost this
project a session.

**The display — theirs, not a virtual one.** `tacli` records it in `instance.json`
at create time: `TACLI_DISPLAY` first, then the inherited `DISPLAY`, then the live
sockets in `/tmp/.X11-unix` — skipping **virtual** X servers (Xvfb/Xephyr/Xnest) on
the first pass, because parallel agent sessions leave 3840x2160 Xvfb displays
running and a shell that inherits one launches the game where nobody can see it.
The human's session is the `Xorg` in `ps -eo args`; everything else is a stand-in
for a monitor. An explicit `TACLI_DISPLAY` still wins, so an agent that genuinely
wants a virtual display can ask for one. Read back the `display` field with
`tacli ls --json` before telling the human it is ready.

**The tile.** Instances are laid out in a grid so parallel windows do not stack.
`tile_for()` wraps within the screen and pulls the last row and column back inside
it — before 2026-09-02 it did neither, so the twelfth instance drew slot 10 and its
1920x1080 window was placed at y=11600, off every monitor. Two things follow:

- Slots are held by **running** instances only, so a stopped one frees its place;
  pass `--slot 0` to claim a cell explicitly.
- A window created off-screen is left **unmapped** by GNOME, and `xdotool
  windowmove` alone will not bring it back — it must be `xdotool windowmap`ped
  first. `xprop -id <wid> WM_STATE` reads `Withdrawn` when this is what happened,
  and the WM may resize the window on remap, so a relaunch is the clean fix.

**Which window is which: the title names the build and the instance.** Every `tacli
launch` writes `tagpu_title.txt` into the gamedir and the DLL appends it, so the title bar
reads

```
Total Annihilation - wt:worktree-gpu_render | tacli:play1
```

instead of the bare name every instance used to share. `wt:` is the **branch of the tree
tacli was run from** — the tree whose `ddraw.dll` it pinned, so it says which *build* you
are looking at, not just which checkout. `tacli:` is the **instance name**, the id every
other tacli command takes, so the title also tells you what to type to drive that window.

```bash
tools/tacli launch t1                      # Total Annihilation - wt:<branch> | tacli:t1
tools/tacli launch t1 --title "fog A/B"    # Total Annihilation - fog A/B
tools/tacli launch t1 --no-title           # stock title, no label
tools/tacli launch t1 --title auto         # back to the wt:/tacli: default
```

`--title` replaces the **whole** label, both fields included — it is the escape hatch for
an arbitrary title, not a way to edit one field. It is sticky per instance like every other
launch knob, and `--title auto` is the only way back out of a `--no-title`. A label that is
blank, or has nothing printable left in it, is the same as `--no-title`. `tacli ls --json` reports the exact string as
`window_title`, and that is what `tacli` searches for to find the client window — so
**take the window from `tacli ls`, never from an `xdotool search` you typed yourself**;
the title is no longer a constant you can hard-code.

Handing the game over is `tacli launch <name> --no-shield`, or `tacli shield <name>
off` on one that is already running: with the shield off their keyboard and mouse
reach the game and yours is no longer the only input.

## The loop

```bash
tools/tacli launch t1 --res 1024x768        # creates on first use, ~2s to window
tools/tacli ls                              # names, pids, windows
tools/tacli keys t1 space                   # menu accelerators
tools/tacli ui t1                           # what gadgets are on screen (preferred)
tools/tacli shot t1 -o /tmp/where.png       # engine surface: see where you are
tools/tacli order t1 --sel move pos 1988 1716   # orders: world coords, no mouse
tools/tacli click t1 320 240                # game coords; the UI, not orders
tools/tacli roster t1 --json                # units + camera eye
tools/tacli stop t1
```

## Ordering units: `tacli order`, not clicks

**`tacli order` is the way to command units. Reach for a click only when the thing
you are testing IS the mouse.** The verb names the order and the target outright and
goes to the engine's own order constructor through the scenario applier
(`ScriptAction_Type2Index` → `ORDERS_NewMainOrder2Unit`), so there is no screen in
it anywhere:

```bash
tools/tacli order t1 --sel move pos 1988 1716          # everything selected
tools/tacli order t1 --unit 2 --expect ARMCOM attack pos 3160 1200
tools/tacli order t1 --unit 2 --expect-target CORCOM attack unit 251
tools/tacli order t1 --unit 2 reclaim pos 1700 1500    # a wreck: name where it is
tools/tacli order t1 --unit 2 stop
```

Why this and not `click --right`:

- **World coordinates, not pixels.** Immune to zoom, resolution, the camera, the
  letterbox and the addressable ring — the four things that silently move a click.
  A target off-screen or under fog is the same as one in the middle of the view.
- **No selection dance and no camera work.** `--unit` orders one unit wherever it is;
  nothing has to be selected and nothing has to be on screen.
- **The order type is yours, not a guess.** A contextual click asks the engine to
  infer the order from whatever the cursor is over; here `attack` means attack.
- **No `Interface Type` in the question at all** — the left/right scheme simply does
  not enter into it.
- **Orders: `move attack defend`(=`guard`)` repair patrol reclaim capture load unload
  blast stop mobilebuild`.** There is no attack-move in TA; `attack pos <x> <y>` is
  the idiom, and the CLI says so if you try.

Two things it is **not**:

- **Not a validator.** `ScriptAction_Type2Index` is documented to return NULL for "this
  unit cannot take that order" and the fork reports that — but measured 2026-09-07 it
  refused nothing: `capture`, `mobilebuild`, `unload`, `blast`, `repair` and `load` were
  all accepted on a commander with a bare position and did nothing at all. A nonsense
  order is issued, not caught, so `1 issued` is not evidence the unit did anything —
  read the roster.
- **Not ownership-checked.** The applier hands the engine whatever unit you named, which
  is how a scenario orders units it spawned for another player. Ordering an enemy unit
  is accepted; whether its AI immediately overrides you was not measured.

**Naming the unit.** `--unit` takes the `engine_index` from `tacli roster`, which is
`UnitInGameIndex` — and the engine **recycles** it when a unit dies, which is why
`scenario-format.md` refuses it as a public identity. `--expect ARMCOM` is what makes it
safe: the fork checks the slot still holds that type, on the game thread, before it
orders, so a recycled index is an error and not an order to a stranger. **Pass `--expect`
whenever you pass `--unit`** — the roster hands you the type in the same row.
`--expect-target` is the same guard for a `unit` target. `--sel` sidesteps identity
entirely by asking the engine what the player has selected right now.

Failures are loud and the exit code is non-zero: `nothing alive in that slot`, `the slot
holds a different unit than expected (the index was recycled)`, `nothing is selected`.

**Drive menus by name, not by keystroke.** `tacli ui` reads the actual gadgets on
screen (see *Driving the UI* below), so the known-good path from a fresh launch is:

```bash
tools/tacli ui t1 click SINGLE            # each one auto-waits and reports the
tools/tacli ui t1 click Skirmish          # screen it landed on
tools/tacli ui t1 click Start
```

No shot between steps, and **no throwaway key** — `ui click` waits for the gadget to
exist before clicking, which is what the old dropped-first-key workaround was papering
over. (Blind `keys` sequences still drop their first key; that advice lives on only if
you drive with `keys`.) Then wait for the game proper:

```bash
tools/tacli wait t1 'alive=[1-9]' --timeout 150
```

That whole path — launch, the three clicks, the wait — is what `tacli scenario load` does
in one command, with the map and players from the file (see *Scenarios* below). Drive it
by hand when you want the menus themselves; use `load` when you want the game.

Skirmish settings come from the registry, no clicking: `--map "Two Continents"`,
`--player 2:2:1:1` (`N:controller[:side[:color[:metal[:energy]]]]`, controller 0=off
1=human 2=AI, side 0=ARM 1=CORE; an empty field leaves that key alone, so `0:1::3` sets
only a colour), `--los`, `--mapping`, `--unit-limit`. **Metal and energy are the starting
resources and set storage too** — the only way to set them at all, since in a running
game TA recomputes storage from owned units and clamps the level to it every tick. All of
it is sticky per instance: what a launch does not name, it inherits from the last one.
**The registry is the only way** — every TotalA.exe switch was traced in phase 1.2 and
none of them sets a game rule (`cmdline-options.md`). Raw switches go through
`--arg=-t --arg=120` (keep the `=`); `-r` and `-d` are refused.

**Two of those are not really registry settings.** `SKIRMISH.GUI`'s `Mapping` and
`LineOfSight` toggles come up at the stage their `.GUI` file gives them — `Unmapped`,
`Permanent` — whatever `SkirmishMapping` / `SkirmishLineOfSight` hold (measured
2026-09-02, and `SingleMapping` makes no difference either). The gadget decides the
game; the registry is only where TA saves the last one. So:

- **`scenario load` starts every game Mapped**, and says so (`map unmapped -> mapped`).
  `--mapping 0` opts out. Mapped is the default because a scenario places units by
  world coordinate all over the map, and on an unmapped one the human — and every
  screenshot — sees them through black.
- **`--los 0` turns the grey fog off.** `Mapping` reveals the *terrain*; the grey
  wash over ground nothing is currently looking at is the `LineOfSight` toggle
  (`Permanent|True|Circular`, stages 0-2). `--los 0` (Permanent) leaves everything
  already seen in full colour — measured 2026-09-02: the engine's `LosType` word at
  `*0x511DE8+0x14281` goes 14 → 12, and bit 1 is the one the terrain pass paints the
  grey mask from (`tagpu_native.c`, "fog is on is NOT LosType bit0"). Not the default,
  because a fog-free map is a play setting, not a test setting.
- Add `switches: {"radar": true}` (or `tacli switches <inst> radar=on`) to see enemy
  units as well as ground — that is TA's own `+radar` debug bit.
- Driving the menus by hand, set them before `Start`: `tacli ui t1 set Mapping 1`,
  `tacli ui t1 set LineOfSight 0`.

## Input details that cost time to learn

- Clicks need `Interface Type=1` (right-mouse orders) — tacli sets it, on **every** launch
  (`apply_prefix_settings`), so there is no instance anywhere that runs TA's own default.
  Select with `click`, and order with **`tacli order`** (above) rather than `click --right`,
  which is scheme-bound: at `Interface Type=0` a posted **right**-click orders nothing at all,
  because there the right button deselects and the left one orders. `click --right` still
  works at type 1 and is the way to test *that the mouse orders*; it is not the way to give a
  unit an order.
- **`Interface Type=1` is not neutral — it changes what the game does.** TA switches its
  *contextual* cursor off at type 1: with a unit selected, hovering ground gives `cursornormal`
  and hovering a wreck gives `cursorgrn`, where stock TA at type 0 gives `cursormove` and
  `cursorreclamate`. So an instance is **not a baseline for anything cursor- or
  interface-shaped**, and "stock TA does X" measured on a tacli instance may be measuring the
  registry. Since G13j the fork patches the cursor half back on at any interface type
  (`field-notes.md` patch 2); `tacli arm <i> curs.off` before launch restores the engine's own
  behaviour for an A/B. The explicit order buttons (Move/Attack/Patrol/Reclaim/Guard) were
  never affected either way.
- **At type 1 a plain `click` on the world DESELECTS** — that is the engine's own rule, and the
  reason `--right` is how you order. `click` selects only when something selectable is under the
  pointer. Between G13j and G13q the cursor patch broke that and a left click ordered as well,
  so a recipe written in that window may have been ordering where it meant to clear the
  selection; `field-notes.md` patch 2b is the fix. At type 0 the scheme is the mirror image —
  left orders, right deselects — which is why posted right-clicks order nothing there.
- **`tacli click X Y` is positionally correct on its own — you do not need to park the pointer
  first.** `inject_click` (`tagpu_input.c`) posts MOVE(x,y), the button down/up, then a MOVE back
  to the screen centre, and both halves of a click honour the target: measured 2026-09-07 with
  the pointer sitting at the centre beforehand, a bare `click --right` at (700,500) walked the
  unit to the world point under (700,500) and not to the one under the centre, and a bare `click`
  on a unit 377 px from the pointer selected it. *[This bullet first said the opposite. It came
  from one bad reading: the bare click HAD ordered, the commander just had not visibly moved 3 s
  later, and adding a `mouse:` park "confirmed" a rule that was never there.]*
- **What the trailing recentre does affect is anything aimed at the pointer afterwards.** After
  any `click`, the pointer is at the screen centre — so `wheel:` notches and the position-less
  `click` / `rclick` tokens land there unless you `pmove:X,Y` first. That is what `pmove` is for
  (`hover` in the `ui` layer).
- **Ctrl/Shift/Alt combos land** (since phase 1.1): `tacli keys t1 ctrl+d`,
  `shift+2`, `ctrl+shift+a`. The modifier is held 150 ms because TA *polls* it.
  If a combo does nothing, read the diagnostic the shield logs when the hold expires —
  `shield: vk=17 released after 150ms, polls=2 down=2`. `down>0` means the game saw
  your modifier and the binding is the problem, not the input path.
- **Injected modifiers need the shield ARMED, and that is not optional.** A polled
  modifier only reaches TA through `fake_GetAsyncKeyState`, and
  `tagpu_shield_key_state` opens with `if (!tagpu_shield_on()) return FALSE;` — so with
  `--no-shield` (or after `shield off`) the poll falls through to the **real keyboard**
  and your injected `down:shift` is invisible. Anything gated on a held modifier is
  therefore **untestable with the shield off**: the order markers are the case that
  bites, because the engine's own SHIFT hotkey (`0xF9` → `GetAsyncKeyState(VK_SHIFT)`) is
  what gates the whole marker block. Symptom: `order: arena=-1` for ever and no markers,
  with the injection reporting `sent: down:shift` perfectly happily. Hand the instance over
  *after* you have finished measuring, not before. The 150 ms hold diagnostic tells the two
  apart: `shield: vk=16 released after 166ms, polls=0 down=0` with **polls=0** means the
  game never asked, so nothing about your injection was the problem.
- Hold anything across frames with `down:<tok>` / `up:<tok>` — keys, or
  `lbutton`/`rbutton`/`mbutton`. Drag-select is `down:lbutton`, `mouse:x,y`,
  `up:lbutton`.
- `tacli eye X Y` pins the camera (writes both eye and scroll-target, else the engine
  fights back); `tacli eye <name> --release` frees it. Read the settled value from a
  `roster` call before doing coordinate maths.
- **`roster`'s `screen=` is the 1x projection, so at any zoom != 1 it is NOT where to
  click.** `tacli click` takes the position on screen; the roster reports the engine's own
  unzoomed one. At 0.25x a unit the roster puts at (512,384) is hovered at (560,382), and a
  click at the roster's figure silently selects nothing. Either drive at 1x, or find the
  unit by parking the pointer and reading `main+0x2CBA` (0 = nothing under it).
- **`roster` is empty for a second or two right after `scenario load`**, and a script that
  reads it immediately gets `units: []` and blames the load. It parses the newest `units:`
  block in `tagpu.log`, which the overlay writes every 30 presented frames, so its `eye` also
  lags a `tacli eye` by up to that long — read it in a retry loop, and re-read after moving the
  camera rather than assuming the first answer.
- **A moving unit invalidates `roster`'s `screen=` before your click lands.** A unit
  with a move order walks between the read and the injected click, and a selection
  click that misses is silent — the symptom is `native: … 0 sel` in the log and every
  subsequent order going nowhere. Re-read the roster immediately before clicking, or
  select first and order second. `grep -a "native: .* sel " tagpu.log` is the cheap
  confirmation that the selection actually took.
- **A right-click on water is rejected for a ground unit**, so it queues nothing and
  draws no order markers — which looks exactly like a broken marker pass. Sample the
  frame for grass before picking a waypoint (green-dominant, `g > b + 30`) rather than
  guessing an offset from the unit. (`tacli order … move pos` has the same constraint for
  a different reason: it is *issued* either way — see "not a validator" above — so check
  the roster moved, not the `1 issued`.)
- Game speed: `keys <name> plus` / `minus` (TA's own feature, up to +10, and negative
  below normal — invaluable for catching fast events or slowing them for capture).

## Driving the UI (menus, options, build panel)

`tacli ui` is a Playwright-style layer over TA's own gadget tree: **snapshot the screen,
then act on a gadget by name.** It reads the live gadget array from inside the process,
so it sees exactly what the engine sees — including whether a button is a toggle, what
stage it is on, and whether it is disabled. On demand only; nothing runs per frame.

```bash
tools/tacli ui t1                     # what is on screen right now
tools/tacli ui t1 --json              # every field (stable contract; the table is not)
tools/tacli ui t1 show Start          # one gadget in full
tools/tacli ui t1 click Skirmish      # auto-waits, then clicks the rect centre
tools/tacli ui t1 set LineOfSight 2   # declarative stage; check/uncheck are aliases
tools/tacli ui t1 set FXVOL 32        # a slider takes a value, same verb
tools/tacli ui t1 select MAPNAMES 'Anteer Strait'   # a row, by text or #index
tools/tacli ui t1 fill GAMENAME Test_2
tools/tacli ui t1 hover ARMSOLAR      # park the pointer on a gadget, no click
tools/tacli ui t1 press Start         # via the gadget's own quickkey
tools/tacli ui t1 wait --gui SKIRMISH
tools/tacli ui t1 click ARMLAB --page 2   # walk a paged build menu first
```

```
gui SELPROV.GUI  640x480   under: -
enter=SELECT  esc=PREVMENU
  #  type     name             click       state          key  grp
  1  button   PREVMENU         533,415     ok             p
  3  list     DPLAY            321,146     sel=0 n=4           50
  4  slider   SLIDER           586,149     inactive            50
  5  button   SELECT           533,293     ok             s
  7  button                    586,94      inactive            50
  9  button   SERVICE0         241,231     ok             w
DPLAY: *0 Internet TCP/IP Connection For DirectPlay · 1 IPX Connection For DirectPlay …
```

- **Everything auto-waits** (5 s, `--timeout`, `--no-wait`). A click waits for the gadget
  to appear and be actionable, then waits for a consequence and reports it —
  `screen SINGLE.GUI -> SKIRMISH.GUI`, `stage 0 -> 1`, or `engine confirmed` (the
  engine's own `UIChange_f`).
- **It refuses rather than guessing.** Absent, `inactive`, `grayed` or zero-sized gadgets
  error with the reason and list what *is* on screen. An ambiguous name is an error too —
  disambiguate with `button:NAME` or `#7`. TA uses **both** `active=0` and `grayed=1` for
  "you cannot have this", so both show up.
- **`#index` is the only way to reach an unnamed gadget**, and there are real ones: the
  arrows flanking a scrollbar are synthesized by the engine at load time, so they are in
  no `.GUI` file and have empty names. Take the `#` from the snapshot's first column —
  `ui t1 click "#7"` — and re-read it after any screen change, because it is a position
  in the current screen, not a handle.
- **`grp` appears only when `assoc` means something** — two or more toggles in one radio
  group, or a slider with its listbox and its arrow buttons. Same number = acting on one
  moves the others, and that is how an action reports its consequence.
- **`enter=` / `esc=`** are the screen's own Enter/Escape bindings, so backing out is
  `click PrevMenu`, never a guess.
- **In game, select a unit first.** With nothing selected the top GUI is `ARMMAIN2.GUI`
  (just KILLS/LOSSES/TOTALUNITS). Selecting a builder pushes its build page on top —
  `ARMCOM1.GUI`, gadgets named after the buildable units (`ARMSOLAR`, `ARMLAB`), plus the
  order buttons (`ARMMOVE`, `ARMSTOP`, `ARMATTACK`) and pagers `ARMPREV`/`ARMNEXT`.
- **The in-game menu is `Tab`, not `Esc`** (`Esc` does nothing in game). It opens
  `ARMOPT.GUI` — `SAVEGAME` / `LOADGAME` / `PREFS` / `MISSION` / `HELP` / `EXIT` / `OK`.
  Both SAVEGAME and LOADGAME land on **`LOADGAME.GUI`** (list `GAMES`, field `GAMENAME`,
  `LOAD` / `CANCEL` / `DELETE`); the `SAVEGAME.GUI` and `SAVELIST.GUI` files in the HPI
  are dead, no string in the exe names them. Quitting is `EXIT` → `MAINMENU` → `CHOICE1`.
- **A build button reports "delivered (no GUI-visible change)" — that is success.** It
  changes the cursor mode, not the GUI. Then place it with a world click:
  `tacli keys t1 mouse:X,Y`, then `tacli click t1 X Y` (**left**; right-click cancels).
  A red footprint box means the site is blocked — try clear ground.
- **`ui` is the gadget layer only.** The map, minimap and resource bars are not gadgets;
  they stay with `click` / `eye` / `roster`.
- **Lists read out and can be picked**: `ui select <list> <text|#index>`. A visible row
  is one click; an off-screen one is walked to with the arrow keys, which is the only
  mechanism in TA that moves a selection a known number of rows (the scrollbar arrows
  move one *pixel*, and there is no page-up/down and no mouse wheel). 99 maps takes about
  8 seconds — raise `--timeout` for a long list. `items unknown` means a *picture* list,
  whose entries are images with no text at all; that is different from `(empty)`.
  **Map and campaign selection are ordinary text lists**, so
  `Skirmish → SelectMap → select "<map>" → LOAD` works — which `--map` cannot do to an
  instance that is already running. `select` refuses a row the selection cannot rest on:
  a **separator** (TA marks these `&G` and the highlight slides off them) or one the
  screen has **disabled**. Both would otherwise look like a click that did nothing.
- **Sliders**: `ui set <name> <value>`, where the value is the number the engine acts on
  (`val=32/64` in the table), not a pixel offset. A slider that is a **scrollbar** — bound
  to a listbox by `assoc`, shown with the same `grp` — refuses, because the engine
  recomputes it from the list every frame; move the list with `select` instead.
- **`fill` types through WM_CHAR**, so mixed case round-trips. What a field *keeps* is
  TA's rule: the save-name field takes letters, digits, space and `_` and silently drops
  punctuation. `fill` reports the field's actual content, so read what it says. It
  refuses outright if your text is longer than the field's `maxchars`, rather than
  handing you a truncation and calling it a fill.
- **Click a field before you fill it.** An unfocused field turns your text into
  *quickkeys*: the first character actuates whatever button owns that letter on the
  screen (on `SELGAME`, `J` is `JOINGAME`'s) and the rest is dropped. The symptom is a
  fill that "kept only the first character", or a screen that jumped somewhere. So
  `ui <inst> click NICKNAME` then `ui <inst> fill NICKNAME …`. Filling a field that
  already holds the value you want is also worth skipping — `fill` clears first, and
  clearing is the slow half.
- **`hover` rarely shows you anything.** It parks the pointer and reports label text
  that appeared, but no tooltip surfaced over the build panel in testing — consistent
  with `help` being empty there. Use it to set up a hover state, not to read one.
- **`help` is empty on almost every gadget** — the engine parses the `.GUI` tooltip and
  then zeroes the field. A few screens write it at runtime (SKIRMISH's toggles do), and
  those read back. Empty `help` is normal, not a broken read.

Changing `tacli ui`? `python3 tools/test_tacli.py` covers the selector, row-geometry,
token and rendering logic offline — no game needed, and it runs in a hundredth of a
second. Everything else about the layer needs a real instance.

## What exists in this game (catalogues)

Ask the running game what it has; the answers cache in the instance and feed
`scenario --instance`.

```bash
tools/tacli units    t1 --limit 0    # 279 unit types in stock TA: ARMPW, ARMCOM, CORAK…
tools/tacli features t1 --json       # 570 features — this is where wreck names live
tools/tacli maps     t1              # 99 map names, off SELMAP's own list
tools/tacli catalogue t1             # what this instance has cached
```

- **Read them in a game, not at the menu.** At the shell TA has parsed only enough of
  each FBI to name it: descriptions and footprints are empty, and the table is *re-indexed*
  when a game loads (ARMCOM is 162 at the menu, 34 in a skirmish). `units` tells you when
  it cached a menu-time read. Names are trustworthy either way; nothing else is.
- **`maps` is the opposite** — the list is a listbox on `SELMAP`, so it only works from
  the shell. It walks `SINGLE → Skirmish → SelectMap`, reads, and backs out to where it
  started.
- **Wreck names are lowercase** (`armlab_dead`, `corak_dead`) while unit names are
  uppercase (`ARMPW`). Scenarios may write either; the game's spelling is what gets used.
  **The Commander has no corpse** — there is no `armcom_dead` in stock TA.

## Scenarios (JSON situations)

A scenario is a JSON *situation* — 200 units fighting, a wreck of a chosen type, the
camera already on it. **`tacli scenario load` goes from nothing to that situation in one
command**, calling the engine's own creation functions to build it. Everything is
name-keyed and resolved against the live game, so nothing is ever addressed by a numeric
id.

```bash
tools/tacli scenario load     t1 200v200        # launch → menus → live → spawned → camera
tools/tacli scenario list                       # what is in scenarios/
tools/tacli scenario validate 200v200 --instance t1   # schema + this game's catalogue
tools/tacli scenario expand   200v200 --wire    # the flat list, and the file the fork reads
tools/tacli scenario apply    t1 200v200        # spawn it into a game already running
tools/tacli switches t1 shootall=on noshake=on  # the SoftwareDebugMode bits, live
```

- **`shootall` (on by default) means idle units engage enemy *buildings* in range**, not
  just mobile units — measured, not lore: six Peewees on `hold` ignored an enemy Solar
  Collector for 45 s with the bit off and left a wreck within 45 s with it on, without
  moving. Turn it **off** when you want units to ignore structures. `noshake` kills screen
  shake, which matters for frame comparison (**ta-capture**).

- **`load` is the whole trip; `apply` is the mutation.** `load` launches with the file's
  own `setup` (map, resolution, players, unit limit), clicks `SINGLE → Skirmish → Start`,
  waits for a world, checks the map TA *actually* loaded against the one asked for, and
  then applies. It is a clean start: on an instance that is already running it refuses
  unless you pass `--restart`. `apply` stacks a situation onto whatever is on screen and
  ignores `setup`'s launch half entirely, because none of it can be changed in a running
  game. Explicit flags (`--map`, `--res`, `--unit-limit`) win over the file, so one
  scenario re-runs elsewhere without being edited.
- **`apply` needs a running game, not the menus.** It runs from a detour inside TA's main
  loop, which only turns over in game; at `MAINMENU` or on the mission-end screen it waits
  600 frames and then tells you so.
- **It validates three times and creates nothing if anything fails**: strict schema,
  the instance's cached catalogue (`tacli units` / `features`), and a resolve pass inside
  the game. `on_error: "skip"` opts into best effort.
- **Read the result.** `apply --json` reports, per entity, the engine index, what was
  requested and where it actually landed (the third component is the terrain snap), plus
  the switches, what the clear removed, and the camera. It is keyed by your own handles.
- **`clear_existing` defaults true** and removes the skirmish's starting commanders
  silently. A scenario that leaves a player with **no units at all** is a defeat: TA goes
  to `ENDMSN.GUI` and nothing further applies. Give both sides something.
- **A slot with `controller: "off"` must own nothing.** An off player never gets a unit
  table, so the first unit created for it faults inside `UNITS_AllocateUnit` on a null
  base — a hard crash during load with nothing in `tagpu.log` to explain it. Validation
  now refuses this before launch and names the slot, so the failure mode is a message
  rather than a dead game; the fix is `"ai"` (an idle AI with one unit is the usual way
  to keep a side alive without giving it anything to do).
- `roster` reports `idx=` — `UnitInGameIndex`, the same number `apply` returns, so the two
  views line up. It is recycled on death, so it names a unit only while it lives. Indices
  are handed out in **per-player blocks of the unit limit**, so with `unit_limit: 500`
  player 0's units are `1..500` and player 1's start at `501` — a free read of whether the
  limit took.
- Ship reusable situations in `scenarios/`; one-offs can be any path. Do not hand-write
  the wire file — it changes whenever the DLL does.

Design, engine recipe and what the live runs corrected: `research/notes/scenario-format.md`.

## Observing

- `tacli shot` — TA's own 8bpp surface. Engine truth, and the only view that shows
  engine UI (menus, placement boxes). **Native GPU-rendered units are invisible here.**
- `tacli glshot` — the GL framebuffer: what is actually presented, including our
  passes. Use this to judge our renderer.
- `tacli crash <name>` — the last crash TA recorded, **symbolised**. TA installs
  its own exception handler and writes `ErrorLog.txt` the instant it faults, so a
  crash is visible in milliseconds; `launch`, `wait` and `scenario apply/load` all
  poll for it and **fail fast with the fault address** instead of sitting out their
  timeout and then blaming the loading screen. Read the address, not just the
  message: `hires CRASHED in TotalA.exe at UNITS_SetHotKeyGroup+0x8d | C0000005 |
  Access violation: Illegal read, data address 0x0000001C` named the cause
  (below) in one line. Symbols come from `tools/ta_symbols.txt`, so the name is
  the nearest preceding one — treat it as a neighbourhood, not a signature.
  **`ErrorLog.txt` is shared between instances** (the gamedir symlinks it), which
  is why the report matches the crashing exe's path against the instance before
  claiming the crash is yours.
- **A game whose sim tick stops while every thread sleeps, with no `ErrorLog.txt`, is not
  paused — it may be a wild jump.** `tacli peek <i> '*0x511DE8+0x38A47:4'` twice, four
  seconds apart, is the test (the sim tick; 30 per second); `tacli shot` failing and the
  periodic `native:`/`reclaim:` lines stopping say the render thread went with it, and every
  `TotalA.exe` thread reading `anon_pipe_read` in `/proc/<pid>/task/*/wchan` is a wineserver
  wait, not a spin. Measured 2026-09-07: a call-site redirect whose rel32 was computed against
  the wrong address froze the tank fixture at tick 244 every run, deterministically, and
  looked exactly like a hang. ptrace is off on the reference setup, so there is no backtrace to be
  had — bisect the change instead (the cobtrace module's `-alloc -run -ret -kill -rand`
  tokens exist for that).
- **Cursor and hover state, without a screenshot**: `main+0x2CBE` is the cursor index the
  engine currently has installed, `+0x2CBA` the unit under the pointer (0 = none), `+0x2CBC`
  the feature under it (`0xFFFF` = none), `+0x2CC3` the current order byte (1 = contextual,
  2 Move, 3 Attack, 7 Guard, 8 Repair, 9 Patrol, 12 Reclaim, 13 Capture, 14 build placement)
  and `+0x2CC6` the mouse-region flags (bit0 minimap, bit1 world viewport, bit2 either). The
  indices that matter: 1 attack, 5 defend, 7 patrol, 11 reclamate, 14 move, 15 select, 17 red,
  18 grn, 19 normal — full table in `exe-reverse-engineering.md` §"The cursor chain". Park the
  pointer with `keys <i> mouse:X,Y`, then peek; that is the whole measurement, and it beats
  reading sprites out of a capture.
- `tacli peek <name> '*0x511DE8+0x2C76:4'` — read game memory from inside the
  process (deref with `*`, `+hex` offsets, `:1|2|4|s<N>|x<N>`). The cheap way to
  answer "did that actually change anything?" without a debugger. Grammar:
  `tagpu/ddraw/inc/tagpu_peek.h`; worked example: `cmdline-options.md` §A/B.
  Handy camera reads: eye `+0x1431F`/`+0x14323`, its scroll target `+0x14327`/`+0x1432B`,
  view size `+0x37E37`/`+0x37E3B`, screen size `+0x37E1F`/`+0x37E23`, map px
  `+0x1422B`/`+0x1422F`, mouse `+0x2C76`/`+0x2C7A` (two DWORDs — **y is at +0x2C7A**, not
  `+0x2C78`, which is the high half of x). Camera-module map: `exe-reverse-engineering.md`.
- `tacli log <name> -g <regex>` / `tacli wait <name> <regex>` — the fork logs
  `units:`/`native:`/`OWND` lines; `roster` parses the newest unit block (id, type,
  owner, world + screen coords — `owner` equal to the `me=` in `units:` is yours).
  That is how you find a unit to steer the camera to and click.
- Video and frame-by-frame flicker analysis: **ta-capture** skill (60fps or you will
  alias one-present dropouts). Grab the window rect from `tacli ls --json`.

## Extra weapons (the sim-changing module)

`tagpu_weapons.c` lifts "three weapons per unit" to `Weapon4..N` (capacity 16).
It is **off unless armed before launch** and stock units keep running the untouched
engine code either way (research/notes/extra-weapons.md, "Implementation").

```bash
tools/tacli launch w1; tools/tacli stop w1          # create the instance dir
tools/tacli arm w1 weapons.on                        # must exist at DLL attach
tools/extra_weapons_fixture.py                       # builds scenarios/content/wpn-test.ufo (gitignored)
ln -s $PWD/scenarios/content/wpn-test.ufo tagpu/instances/w1/gamedir/   # the test units
rm -f tagpu/instances/w1/catalogue.json              # cached type list is now stale
tools/tacli scenario load w1 wpn-llt10 --restart     # two ten-laser towers vs solars
tools/tacli weapons w1                               # every slot of every unit + counters
tools/tacli log w1 -g "weapons: (loader|VIOL|MISM)"
```

- `tacli weapons <inst> [idx…]` is the oracle: per slot the state byte, weapon,
  target, reload, heading, pitch, stock, aim result and COB thread, plus `armed`,
  the C-path hit counters (`hits:`) and projectile launches per slot (`fires by
  slot:`). It works **unarmed** too — that instance is your control. With stock
  content and the module armed, every counter but `loader`/`stock_splice` must
  read 0 and `mismatch`/`violation` must be 0; that is the regression check.
- It also prints `type CRCs:` — each unit type's `CRC_weapons` and `CRC_all`, the
  unit-sync values. Two instances that agree about a type print the same pair;
  armed and unarmed differ for exactly the types carrying `Weapon4+`. In a
  multiplayer game TA **disables** a type the peers disagree about (it vanishes
  from `tacli units` on both sides) and starts anyway, silently.
- **Content goes in a `.ufo`, never as loose files** (the engine finds a loose
  `units/*.fbi` and then drops the type). `tools/hpipack.py` writes/reads the
  archive, `tools/cobclone.py` gives a COB per-weapon script copies, and
  `tools/extra_weapons_fixture.py` rebuilds the shipped test pack from the game.
  After adding or removing archives, delete the instance's `catalogue.json` or
  `scenario load` refuses the new type at validation.
- `tacli log -g` takes a Python regex: alternate with `(a|b)`, not `a\|b`.

## The COB script trace (tacob's oracle)

`tagpu_cobtrace.on` at DLL attach makes the fork log every COB thread the engine starts,
refuses, returns, kills or draws a random number for — one tab-separated line each, stamped
with the sim tick — to `gamedir/tagpu_cobtrace.log`. The line contract and what the first
traces taught: `research/notes/tacob-design.md` §"The trace contract"; the engine seam:
`exe-reverse-engineering.md` §"The COB engine". Reads only; arm it like `weapons.on`, before
the launch, and read the file straight from the gamedir (`tacli ls --json` names it):

```bash
tools/tacli arm c1 cobtrace.on=ARMPW native.on=all   # the value is a type filter; native.on
                                                     # because the pose oracle lives in that pass
tools/tacli scenario load c1 cob-kbot --restart
sleep 8; tools/tacli arm c1 posedump.on              # one pose dump, header `posedump: tick= idx=`
grep -a 'cobtrace:' tagpu/instances/c1/gamedir/tagpu.log      # ARMED … filter=,ARMPW,
cut -f1-8 tagpu/instances/c1/gamedir/tagpu_cobtrace.log | head
```

- **`tools/cobtrace_fixtures.py`** runs the nine class scenarios (`scenarios/cob-*.json`) this
  way and keeps `cobtrace.log`, `posedump.txt` and tacli's `apply.json` per class under
  `research/notes/evidence/cobtrace/`. It parks the camera on the traced unit (`tacli eye`)
  before dropping `posedump.on`: the pose oracle dumps the first unit the native pass draws,
  and an aircraft or a ship has left the spawn view by the time it has done anything.
- **Sight radius before weapon range.** A Stumpy 250 units from an AK never aimed: neither
  could see the other. Put the target inside the shooter's `SightDistance`, not just its range.
- **A Hawk is air-to-air**; ordered at a ground unit it flies over it and does nothing. Give it
  a patrolling enemy aircraft.
- The file is truncated at every launch and flushed per line, so `tacli stop` loses nothing.

## The input firewall (on by default)

**"Human controllable" / "let me play it" / "hand it to me" means SHIELD OFF.** That is the
whole content of the request: an instance launched with the shield on answers every `tacli`
command and ignores the human's keyboard and mouse entirely, which reads to them as a game
that is running but broken. So `--no-shield` at launch (or `tacli shield <i> off` on a running
one), and check `tacli ls` says `OPEN` before saying it is theirs. Also check the window is on
one of their monitors (*Where the window lands*) — the two together are what "controllable"
means.

While armed, the game ignores the real keyboard and mouse completely and sees only what
tacli injects — the human can click and type across your window without perturbing your
test, and your instance does not need focus.

```bash
tools/tacli shield t1            # report state
tools/tacli shield t1 off        # hand the game to the human (live, <=15 frames)
tools/tacli launch t1 --no-shield
```

Turn it **off** when you want the user to play or drive the instance by hand; leave it on
otherwise. Details and evidence: `research/notes/input-firewall.md`.

## Arming the GPU passes

```bash
tools/tacli arm t1 native.on=all owndraw.on writeback.on=armcom scaffold.on
tools/tacli arm t1 native.on=off           # clear one
```

Any `tagpu_<x>` trigger file works; value goes into the file (e.g. `all`, `armcom`).

### The default arm set

**Unless the launch is a measurement, arm all of it.** Most of these install engine
detours at DLL attach and cannot be armed afterwards, so this runs *before* the launch
that matters — and `launch` auto-arms each pass's `*own.on` patch half for you:

```bash
tools/tacli arm <i> 'native.on=all wrecks' terr.on feat.on fx.on sfx.on \
                    mark.on order.on zoom.on vpwide.on gui.on
tools/tacli launch <i> --no-shield --res 1920x1080
```

Units and wrecks, terrain, features, weapon effects, particles, world-space markers and
the shift-held order overlay,
zoom (wheel live, camera range widened), the wide viewport that makes zoomed-out
clicks land, and **the GL UI layer** (`gui.on`, since G15b — the panel, bars, dialogs and the
shell drawn by us at 1:1, the engine's surface the fallback; *The GL UI layer* below). `launch` then prints `auto-armed owndraw.on=all / fxown.on / featown.on /
terrown.on / markown.on`, and `tagpu.log` carries one `ARMED` line per pass — read them,
because a missing one is the whole pass silently absent.

**The shipped DLL arms all of this by itself** (`tagpu_opt.c`, gpu-status §2.8: with no arm
file the play set is on, Classic++ and the extra weapons included). An instance opts out:
`launch` and `scenario load` write `tagpu_defaults.off` into the gamedir unless given
`--defaults` (sticky per instance; `--no-defaults` back), so everything above still holds
here and a bare launch is still the stock control. `--defaults` is the player's
configuration and the one to test a release with. Under it a pass is turned off with
`tacli arm <i> <pass>.off` — removing an `.on` that was never there changes nothing — and
`native.off` takes `owndraw` with it, `zoom.off` takes `vpwide`. `tagpu.log`'s first `opt:`
line says which way the instance went.

**The pose-race levers** (`tagpu_native.c`, gpu-status §2.9) are measuring tools, not play
settings; all three are off unless the file is there. Their fixture is
**`scenarios/pose-inventory.json`** — 69 units in four clusters, one camera stop each, covering
all eight pose classes and the two extremes of stock geometry (`ARMSCORP`/`CORSCORP`, 36 pieces;
`CORGANT`, 574 verts / 304 faces) — with **`pose-inventory-sea.json`** for the naval classes on
Anteer Strait. Everything is owned by player 0 so nothing fires and the poses are the only thing
moving. The stops are `tacli eye <i> 1716 806` (ground), `2716 806` (structures), `3616 806` (the
extremes) and `2216 1606` (the air lane).

| file | what it does |
|---|---|
| `tagpu_posebake.on` | G16 step 4's per-type geometry bake (gpu-status §2.10). **Draws nothing** — it bakes, caches and reports. `log` gives a line per model and per material stream. The `native:` line grows `bake=<types>/<streams> anom= odd= nomat= refused=`. *(Its `check` token is gone with step 8: it held the bake to `emit_geom`'s vertex count, and that emitter no longer exists.)* |

⚠ **`[2026-09-09]` The other three pose levers are GONE, and so is `tagpu_posedraw.on`.** G16 step 8
deleted the CPU emitters, and with them the pose-race guard, the rest-equality detector, the
reconstruction and `posewatch` — the only things `tagpu_posefix.off`, `tagpu_posewatch.on` and
`tagpu_poserecon.on` ever reached. **The posed program is not a lever any more, it is the unit
renderer**: there is nothing to A/B it against in one build, and creating those files does nothing.
The `native:` line lost `posefix=`, `guard=`, `rest=`, `norecon=` and `errmax=` with them.

**What to read instead.** `posed=<units>/<tris>` — with ` slant=<units>/<tris>` and
` wire=<units>/<lines>` when the scene has them — is the pass, and **`verts=` should read 0 for
units**: anything else means something was built on the CPU, which now only the selection lines and
the effects models do. Four more fields appear **only when they have caught something**, and in a
healthy game none of them ever does:

| field | meaning |
|---|---|
| `rest=` | units past the frame's pose arena, drawn **at rest** for that frame (right geometry, material, position, fog, shadow; only the animation frozen). ⚠ It looks exactly like the old pose-race artifact — that is why it is counted |
| `unpl=` | *pieces* the pose walk could not place, left at rest inside a unit that is otherwise posed |
| `nobake=` | units whose type would not bake, which **draw nothing** — the one honest drop. Over 256 pieces or 49152 vertices; stock's worst is 36 and 574 |
| `q=` | units the gather queued against units the pass drew, printed only when they disagree. A queued unit the draw dropped is a unit missing from the screen |

**`posed=` reading lower than the unit count is usually the HIRES pass, not a miss.** A gamedir with
`hires/<name>.glb` in it draws those units through the replacement-mesh renderer instead, and they
are counted in `unit(s)` but not in `posed=`. Check `ls <gamedir>/hires/` before chasing it — this
cost a diagnosis on 2026-09-09.

**If units are missing on the OLD path, that is `MAXNV`, not a bug in yours.** Before step 8 every
unit's geometry went through one shared 49152-vertex stream, so past it units got an empty range and
drew nothing — health bar, no model — and the line says `VERTEX-BUDGET-HIT`. `scenarios/crowd-static.json`
(256 units, one owner, no orders, so it is the same picture between runs and can be diffed) shows it
at 0.5x zoom. The posed path has no shared budget to exhaust.

**A/B-ing any live lever: wait for a FRESH `native:` line before the second shot.** That line is
written every 300 frames — five seconds at 60 fps — so a lever flipped and shot three seconds later
is read against the *previous* setting's counters, and the diff comes out 0 for the wrong reason.
Count the `native: [0-9]` lines, flip, wait until the count has moved by two, and confirm the
numbers actually changed before believing a pixel diff (2026-09-09, a 200-unit A/B that read
`0 differing pixels` because both shots were the same path).

**A cross-BUILD A/B needs a zero noise floor first, and most fixtures do not have one.** Swapping
`ddraw.dll` means relaunching, and a relaunch re-runs the sim: an animating piece is at a different
angle, so `shadow-struct` diffs **9616 px** against *itself* and `shadow-lab` 10466. Establish the
floor by running the SAME dll twice before believing any number (ta-capture rule 7). What works:
pause at a fixed sim tick (poll `*0x511DE8+0x38A47:4`, then `keys <i> tab`), diff the world viewport
only (the minimap and resource bar move on their own), and pick a fixture with nothing animating —
`one-unit` and a few static structures both measure **0**. A battle cannot be paired at all: two
loads of `200v200` diverge to different survivors.

**A frame-time A/B needs `--maxfps 0`, and without it it measures nothing.** `write_ddraw_ini`
rewrites the cap into the instance's `ddraw.ini` whenever `--maxfps`, `--res` or `--window` is given, or the tile is off-screen — **not on a bare `tacli launch <inst>`, which writes the file at all** (corrected 2026-09-09; the docstring used to claim every launch path). Pass `--maxfps` explicitly to be sure of the value. The DLL reads
it at attach, so an edit made by hand first is overwritten — which is why the cap is a **sticky
launch knob** (since 2026-09-09) rather than something to edit:

```bash
tools/tacli launch t1 --maxfps 0                       # 0 is UNLIMITED; sticky, like --res
tools/tacli scenario load t1 200v200 --restart --maxfps 0
grep maxfps <gamedir>/ddraw.ini                        # confirm it survived the launch
```

**`maxfps=0` is the unlimited setting** — `fpsl_init` maps a NEGATIVE value onto the display
refresh (60) and only `0` falls through every branch with `tick_length` left at 0, so the value to
make survive the launch is `0`, not `-1`. **Two paths that both hold 60 (or 58.5) fps have not been
compared, they have both hit the cap** — that is exactly how G16's posed program read "no
difference" until 2026-09-09, when uncapping it showed 184 fps against 313.

Two things to do on top of uncapping, both learned taking that measurement:

- **Pause the sim first** (`tacli keys <i> tab`, then peek the tick twice to confirm). On a
  fighting scenario units die under the measurement, so the second half of an A/B draws a smaller
  scene than the first. Paused, the renderer keeps working and the unit count is fixed.
- **Read the losing path's `verts=`.** If it says `VERTEX-BUDGET-HIT` it is *truncating* geometry,
  so the comparison flatters it — it is drawing less and still costing more.

The meter needs no code: the overlay writes a `units:` line every 30 presented frames, so
`30 × (lines gained) / (seconds elapsed)` is the frame rate.

**Slice `tagpu.log` by BYTE OFFSET when a run has to be attributed to a camera stop** — take
`stat -c %s` before and after and `tail -c +N` — rather than grepping the whole file. Any
per-unit-per-frame logging makes it grow fast (`nlog` opens and closes the file per line), and a
`tacli log` grep can also hand you the *previous* game's lines after a reload.

**Classic++ is a separate switch; its restorer runs as GLSL in the game's own context.**
`tacli arm <i> classicpp.on` turns on the restored true-colour terrain, features, effects and (since G14g) unit textures;
it is *polled* twice a second, so it flips live for an A/B, and arming it mid-play restores
everything already on screen (the terrain radiating from the screen centre, the sprites within
a slice or two after it). The restore itself is `tagpu_restoreglsl.c` — fragment passes sliced
at 12 ms of GPU time per frame, the terrain's tiles nearest the screen centre first, then the
feature and effects atlases' queues — and needs nothing but the weight files
`full.w32.bin`/`tiny.w32.bin`, which `launch` links beside `TotalA.exe` from
`unditherer/models/` (the tree tacli runs from first). Read the result in `tagpu.log`:
`restoreglsl: 12x64 fp32, NK=4 ...` then `restoreglsl: terr: job started: N frames ...` and
finally `restoreglsl: terr: done: ... in S of F frames = W ms wall since begin (X fps while
restoring); GPU G ms` — **that `fps` is the frame rate the game held during the restore**, every
frame over wall time (S is the frames the terrain drew in; the rest went to a sprite batch or a
query wait), and the one to quote. Two Continents: 2.37 s at 59.4 fps with the sprite queues
live (2.1 s without); the biggest stock map (Lava & Two Hills, 11,561 tiles) 4.6 s, its viewport
complete in 1.9 s. The sprites' lines are `restoreglsl: feat: lazy restore armed ...` /
`restoreglsl: fx: ...` and a `queue drained: N frames in B batches this run, F frames from the
first queued to the last painted` tally (under `log`, or at most one per 300 frames). The first
job of every launch is abandoned by the startup GL reset and restarted; the `done` line is the
second job's. Cells and sprites show as they land, so a shot taken during the restore is a
mixed frame — wait for the `done` line before a parity capture.

Knobs go in **`tagpu_restoreglsl.on`**, read **once per GL context** (since G14e the restorer's
programs are shared by every job), so arm them before the launch you are measuring — the
startup GL reset re-reads them once, a map change does not: `tacli arm <i> 'restoreglsl.on=log tiny'` — `tiny`
(the 6×24 model), `fp16`, `nk=N` (output tiles per conv draw), `budget=MS` (GPU ms per frame,
default 12), `log` (a line per batch). **`tagpu_restoredump.on`** makes the DLL write the finished
terrain atlas once to `gamedir/tagpu_restore.rgba`, and each lazy atlas's restored twin once its
queue drains — `tagpu_restore_feat.{r8,rgba,idx}`, `tagpu_restore_fx.{...}`, and since G14g
`tagpu_restore_unit.{...}` (re-written when the atlas has grown; the `.idx` lists every entry) —
the only files the GLSL restorer ever writes. The unit atlas's lines are `unit: restored twin
2048x2048, trilinear to mip level 2, 4x anisotropic` (or `no anisotropic filtering (extension
absent)`) and `restoreglsl: unit: lazy restore armed …`; its queue's `queue drained` tally obeys
the same once-per-300-slices rule as the sprites', so a small burst that drains right after the
terrain logs nothing — `restoreglsl.on=log` for its per-batch lines, or the dump line's entry
count.

**Classic++ lighting knobs go in `tagpu_classicpp.cfg`** (G14f), and unlike the restorer's
they are **live**: the file is re-read on the switch's own twice-a-second poll whenever its
write time or size changes. `tacli arm <i> 'classicpp.cfg=sun=off'` writes it (tokens
`sun=AZ,EL` | `sun=off`, `unitsun=AZ,EL`, `amb=A`, several separated by spaces in one quoted
value), `tacli arm <i> classicpp.cfg=off` removes it (= the lab's defaults `324.5,53.1` /
`215.5,53.1` / `0.35`). The DLL answers every read with one line, `classicpp: light sun=…
unitsun=… amb=… level=…/… (tagpu_classicpp.cfg)` — or `(no cfg: defaults)` — so
`tacli log <i> -g 'classicpp: light' | tail -1` says what the frame is lit by; allow ~1.5 s
after arming before a shot.

**The switch has two halves (G18a): `assets=0|1` and `light=0|1`**, both default 1, both in
the same cfg and live on the same poll, and they answer on their own line ahead of the light
one — `classicpp: assets=1 light=1 (tagpu_classicpp.cfg)`. `assets=` is the restored
atlases (`uRestored`, the terrain restore step, every lazy GAF twin, and the G15e UI twin);
`light=` is the lambert (`uLambert`, and the ground lambert baked into a feature anchor).
`tagpu_classicpp.on` stays the master arm — absent, both are off whatever the cfg says — and
the **shadow** dimension is still on that master arm and its own `shadows=` key, not on these.

- `assets=1 light=1` is the switch as it always was, to the pixel.
- **`light=0` is not `sun=off`.** Both give a flat frame, but `sun=off` sets `amb=1.0`, which
  puts the depth-map shadows out (`tagpu_shadow.c:359` refuses at `amb >= 1.0`); `light=0`
  leaves them. Measured on `tascene-parity`: `light=0 shadows=0` is byte-identical to
  `sun=off`, and `light=0` alone differs from it by exactly the 1800 shadow pixels.
- **`light=0` leaves a unit UNSHADED, not Classic-shaded.** The engine's per-face
  `PALETTE.SHD` shade row is the *Classic* branch's (`uLit == 0`), which the master arm
  selects, so under Classic++ the choice is the lambert or nothing.
- **To see `assets=` on the UI you must first make the UI draw real art.** Entering a game
  seeds the panel with HUD icons only, all under the 12-px restore floor, so flipping
  `assets` changes 0 px there. Press Tab for `ARMOPT` (or select a builder) first: then the
  flip moves 37 415 of that rect's 45 056 px.
- The `gui: twins=` heartbeat carries `cpp=<master> assets=<n> light=<n>`, and `colvalid`
  drops to 0 while `assets=0`. **The shadow keys** (G14i) ride the same file — `shadows=0|1|2`,
`shadowsun=AZ,EL`, `penumbra=K`, `shadowlen=A,B|off`, `shade=S`, `terrainshadow=0|1`
(**default 0** since 2026-09-09 — the ground self-shadows, renderers.md 2.7b; set it to 1 only
to study that),
`shadowres=N`, `airshadow=len|physical|drop`, the lab's defaults — and answer on a second
line, `classicpp: shadows=1(soft) shadowsun=225.0,40.0 …`; the map also needs the engine's own
Shadows option on.

**`shadows=` is three-way since G18b: `0` none, `1` SOFT (the map-anchored depth map, the
default), `2` HARD (Classic's own 5-px silhouette and cached slant, drawn under the
switch).** The two are never both on, so `shadows=2` is how you get Classic's shadow look
with Classic++ art. An out-of-range value logs `bad token` and leaves the default standing.
Only the soft map reads the other seven keys. `shadows=0` also drops the silhouette an
aircraft keeps under `airshadow=drop`.

**`sun=off` is now exactly `light=0`** (G18b) and no longer touches `amb` or `shadows=`. It
used to force `amb=1.0` and silently clear the shadow key — so the log answered `shadows=0`
while the cfg said 1, and it did not put it back. The picture is unchanged (`sun=off
shadows=0` is 0 px from the old `sun=off`); what is new is that **`sun=off shadows=1` keeps
the depth map**. `tagpu_shadow.c`'s `amb >= 1.0f` refusal now only fires on an explicit
`amb=1`, where the shadow term would be multiplied by `(1 - amb) = 0` anyway. `tagpu.log` says `shadow: GL ready (GL_VERSION 3.3.0 …)` once per context,
`shadow: frame zoom=… res=… k=… texel=…` whenever the lattice changes (zoom), and one
`shadow: caster model=… top=… gnd=… sv=…` line per caster position seen (16 at most) — the
numbers the length rule used. **`tacli arm <i> shadowdump.on`** writes the map once as
`tagpu_shadow.pgm` and removes itself (`shadow: dumped …` carries the matrix). **A shadow A/B
against the lab needs the pointer parked off the units**: `scenario load`'s `center_on` leaves
it ON the anchor, the engine draws its crosshair there, and those pixels are identical in an
on/off pair, so `tacli keys <i> mouse:200,700` first. **The parked cursor is still an
animating sprite**, so it differs between two *launches* even when nothing else does —
exclude its rect (~26×36 around where you parked it) from any cross-build diff, or you will
chase a constant ~110 px that is not yours.

**On any fixture with an animating unit, pause the sim before shooting.** `shadow-lab`'s mex
spinners, the wind generator's blades and a commander's idle put 3000–4000 px of noise
between two shots one second apart, and the phase differs per launch, so cross-launch
diffing is meaningless there. `tacli keys <i> tab` opens `ARMOPT`, which pauses the sim and
sits in the side-panel rect, leaving the world viewport untouched — the noise floor goes to
**0 px**. Take every state of the A/B inside that one paused run. **The engine's own Shadows toggle is the
parity oracle for a Classic shadow**: `tacli keys <i> tab`, `ui <i> click PREFS`, `ui <i> click
VISUALS`, `ui <i> set BSHADOWS 0` (or `1`), `ui <i> click PREV`, `ui <i> click OK`. It clears and
sets BOTH option bits (`main+0x37F06` reads `0x3F` on, `0x23` off) — **read the word back with
`tacli peek <i> '*0x511DE8+0x37F06:2'` before the shot**: one run of those same clicks on a stock
instance reported `stage 0` and changed nothing, and its on/off pair differed only by the drill
arms. A shadow's pixels are then `glshot` on minus `glshot` off, counted in 200×140 around each
roster screen position (the G13k / G14j numbers). `terr: height grid WxH uploaded …` is the terrain's height
texture, once per map; without it (an unreadable grid, logged and retried every 60 frames)
Classic++ terrain draws unlit — still restored, still the RGB grey rule, no lambert.
**The level-ground test** is a sun on/off pair of `glshot`s with `feat.on=passive` (so the
engine draws the sprites, identical in both): every pixel whose 16-px cell has zero gradient
at all four corners must be byte-identical — 0 of 162,828 on the parity fixture — while the
sloped ones move. `sun=off` also draws the units without their LUT row (the lab's meaning of
"no sun"), so it is not a Classic frame: compare it to a Classic++ shot of the previous DLL,
not to Classic. **Since G18b the level-ground rule holds under a shadow too** — a level
cell's own normal is the one `light=0`/`sun=off` substitutes, so a shadowed level cell is
identical in the lit and the flat lane (300 026 level pixels, 558 of them shadowed, on the
parity fixture). The old `sun=off` put the shadows out, so the pair could not show it. **`assets=0 light=0` is the one Classic++ state that IS a Classic frame** —
with `shadows=0` too it lands within 594 px of a `classicpp.on`-removed shot on
`tascene-parity`, all of them on one unit (the LUT row above).
`tools/tascene restorediff <pack> <the .rgba>` holds the terrain to a pack built with `--undither`
of the same map (max 1 level on < 0.01 % of bytes is the bar; Two Continents measures 0.0012 %),
and `tools/tascene featdiff <pack> <gamedir>/tagpu_restore_feat` the feature twin (the far band
exact, the near band reported; 24 of 24 frames matched on the parity scenario);
`tools/tascene unitdiff <pack> <gamedir>/tagpu_restore_unit` holds the unit twin to the pack's
`units/atlas.rgba.bin` the same way, with the ring check over the whole 4-texel border (25 of 25
on the parity scenario, far band max 1 level on 2 bytes, the ring exact). The effects
twin has no pack reference: judge it by eye (`fx-mix`).
The ONNX Runtime path — `onnxruntime.dll`, DirectML, the vkd3d-proton `d3d12` pair, `tagpu_cache/`,
`tagpu_restorecpu.on`, `tagpu_restoreonnx.on` — was deleted on 2026-09-05; a `restore:` line in
`tagpu.log` means an old DLL; a `vkd3d-proton ... from` line at launch means a stale `tools/tacli`.

**Health bars are a registry value, not a trigger**, and `tacli` does not set it, so
the mark pass draws no bars until you do (`tagpu_mark.c:333` gates on `main+0x37F06`
bit0):

```bash
WINEPREFIX=<inst>/prefix wine reg add \
  "HKCU\Software\Cavedog Entertainment\Total Annihilation" \
  /v damagebars /t REG_DWORD /d 1 /f
```

**`<pass>.off` does nothing while `<pass>.on` exists.** The precedence is the one
`tagpu_opt.c` documents — an `.on` wins, tokens and all; an `.off` only turns off a pass that
was on *by default*. So on an instance where you armed `classicpp.on` by hand, `arm <i>
classicpp.off` is inert and the shot you take after it is still Classic++. Remove the arm
instead: `tacli arm <i> classicpp.on=off`.

**Deliberately NOT in the set**, so that "everything" stays a decision and not a sweep:

- `scaffold.on` — superseded by `feat.on` (features write real depth now) and its debug
  overlay tints every tall feature purple.
- `writeback.on` — the older per-type sprite composite, targeted at one unit name;
  `native.on=all` + `owndraw.on` is the path that replaced it.
- `weapons.on` — sim-changing, and inert without `.ufo` content built for it. Arm it
  for the extra-weapons work, not for a play session.
- The `.off` flags (`curs.off`, `wheel.off`, `zoomedge.off`, `ss.off`, `shade.off`,
  `subpix.off`, `nano.off`, `r3dcache.off`, `overlay.off`) — these **disable** features.
  Arming everything means leaving all of them absent.
- **`reclaim.off` disables a crash fix, not a feature** (G14h): `tagpu_reclaim` defers the
  engine's model-object frees so the render thread cannot read a freed unit or wreck — the
  `200v200` fault at ~95 s — and since 2026-09-09 the per-LEVEL model templates too, so a
  level teardown cannot pull a model tree out from under a render pass. It is on by default with
  no arm file; `tacli arm <i> reclaim.off` is the A/B lever back to the racing build. Read
  `reclaim: ARMED …` at launch — it names `model templates@0x42DC01/0x42DCB6 -> deferred` when
  that half armed — and the `reclaim: def=… drn=… ovf=0 … tmpl=<queued>/<freed by the
  epoch>/<leaked> …` line every 300 frames. **`ovf` and the third `tmpl` field must stay 0**; the
  second is normally 0 too, because the usual path releases templates at the teardown rather than
  through the epoch. A level change logs `reclaim: level teardown: flushed N …` and then
  `reclaim: teardown post: freed N block(s) …` (~279 on stock content, one per unit type plus the
  table), and `native: level N -> N+1, dropping the template caches: aabb= selbox= pmap=`.
  **Quitting a level to the shell and starting another one in the same process works** — measured
  2026-09-09 over two cycles under the play defaults; the `tab` → `ui click EXIT` →
  `ui click MAINMENU` → `ui click CHOICE1` route is how you do it, and it is the only way to
  exercise the level generation at all.
- The instrumentation triggers (`suppress.on`, `tracer.on`, `gldbg.on`, `posedump.on`,
  `cobtrace.on`, `spxlog.on`, `fpsosd.on`) — debugging, not features.
- `hires.on` only carries the hires renderer's *tweaks* (`anchor=`, sun, ambient,
  normal maps). What turns hires models on is a `gamedir/hires/<unit>.glb` existing.

**Two things this set changes about how you observe.** `terr.on` makes `tacli shot`
inside the viewport ~99.9 % one flat palette index (that is the ownership proof, not a
bug) — judge the picture with `glshot`. And order markers only draw while SHIFT is held,
which needs the **shield on**, so measure the markers before handing the instance over.

**A stale `owndraw.on` is worse than none.** Its detours skip the engine's unit rasterisers,
so with `native.on` cleared but `owndraw.on` still armed the engine draws **no units at all**
(only health bars) — an engine-side A/B then measures nothing, silently. `launch` now drops it
when native is off and says so; if you ever see `OWND ... repaint=0 miss=<everything>` with no
`native:` lines, that is the shape of it.

**owndraw must exist at launch, not after.** The code-patching passes — `owndraw`,
`suppress`, `tracer` — install their engine-code detours once at DLL load and only if
their trigger is present then; there is no per-frame re-arm (patching live engine bytes
off the frame loop is racy). So `arm owndraw.on` *after* launch does nothing, and the
native pass then double-draws (engine 8bpp under our RGB, near-invisible). The GL/behaviour
triggers (`native.on`, `writeback.on`, the `.off` toggles) do re-read every frame and are
fine to change live. To save the footgun, `launch`/`scenario load` **auto-arm `owndraw.on`
to match `native.on`** when native is set — it prints `auto-armed owndraw.on=…`. So the
normal flow is: `arm native.on=all wrecks`, then `scenario load … --restart`. Verify with
the log line `owndraw: ARMED … opaque@0x459830=OK` and `OWND … skipped>0`. With target `all`
that line also reads `structshadow@0x4592C6+0x45952C=OURS`: the engine's cached building
shadow is skipped and the native pass draws it (`native: … N slant`). A solid **teal**
silhouette on or beside a building is that shadow leaking through the composite — it means
this build predates G13k, or owndraw is armed for a single type.

**The effects pass works the same way.** `fx.on` (weapon fire, explosions, debris — tokens
`log`, `passive`, `nolines`, `nomodels`, `nosprites`, `noexpl`, `nodebris`) needs the
`fxown.on` code patches, which tacli auto-arms at launch when `fx.on` exists (`auto-armed
fxown.on`). The engine skip then *follows `fx.on` live*: `arm fx.on=off` restores the
engine's effects within 30 frames, `arm fx.on="log passive"` keeps gathering and logging
(`fx: proj=… expl=…` every 60 frames) while the engine draws — the same-fight A/B lever.
Verify with `fxown: ARMED site.proj=1 site.expl=1 …` and `FXOWN skip=1`. Fixtures:
`scenarios/fx-mix.json`, `fx-lasers.json`, `fx-rockets.json`.

**Features (trees, rocks, metal patches, splats, wreckage) are `feat.on`** — tokens `log`,
`passive`, `noflat`, `notall`, `noshadow`, `nowreck` — on their own patch, `featown.on`,
which tacli auto-arms at launch when `feat.on` exists. Same live A/B lever: `arm feat.on="log
passive"` = the engine draws while we count (`feat: rect=68x76 anchors=231 flat=38 tall=193
... -> body=231 shadow=193` every 60 frames), `arm feat.on=log` = ours. Two things to know:
it **only takes the draw while `native.on` carries `wrecks`** (3D wreckage is drawn through
`DrawUnit` from inside the same leaf, so without the native wreck pass owning the leaf would
delete every husk — the log line says so when it refuses), and it makes **`scaffold.on`
unnecessary**: features now write real depth, so leave the scaffold disarmed (its debug
overlay tints every tall feature purple and ruins captures). Verify `featown: ARMED
feature@0x46A610=1` and `FEATOWN skip=1`. Fixture: `scenarios/feat-forest.json` (Two
Continents: units parked two tile rows behind a tree row, a lab wreck, a walking commander).

**Terrain is `terr.on`** — tokens `log`, `passive`, `over`, `key=N` — on its own patch,
`terrown.on`, which tacli auto-arms at launch when `terr.on` exists. It has a **three-way**
A/B lever rather than two: `passive` = the engine draws, we emit nothing; `over` = ours drawn
on top of the engine's own terrain, which is the pixel-parity test (diff `shot` against
`glshot` — a terrain-only band must differ by **zero** pixels); default = ours, with the
engine's terrain pass *and its fog overlay* skipped. Verify `terrown: ARMED
terrain@0x483FA0=1 fog@0x4848E0=1 key=254` and `TERROWN skip=1 filled=1`.

Three things about this one are unlike the other passes:

- **The engine's frame inside the viewport becomes a flat fill of one palette index** (the
  key, 254 by default) in place of the terrain blit, and the composite then shows the engine's
  frame **only** where it is *not* the key — that is how health bars, nanoframe wireframes,
  the build cursor and chat still reach the screen. So `tacli shot` inside the viewport is
  supposed to be ~99.9 % one flat colour while owned; that is the ownership proof, not a bug.
  `key=N` moves it if a mod's UI ever uses 254.
- **It owns the fog overlay too**, so `terr.on` off/`passive` restores *both*. If the grey
  band ever disappears, check `native: … fog=N los=N` in the log before suspecting the shader:
  fog is on whenever the grid uploaded, and `los` is the engine's raw `LosType`.
- **Without `terrown.on` it refuses to draw at all**, and says so:
  `terr: … (NOTHING EMITTED: terrown.on must exist at DLL attach — arm it before launch,
  not after)`. Our terrain is opaque and covers the whole viewport, so drawing it with no
  key to invert against would hide every engine overlay and still *look* right. Arming
  `terr.on` after launch therefore does nothing visible — relaunch.

`key=N` is re-read with the rest of the tokens, so dropping the token restores 254, and
changing it live hands the draw back for one frame so the fill and the composite can never
disagree about which index they mean.

**The world-space UI markers are `mark.on`** — tokens `log`, `passive`, `nobars`,
`nocapture`, `noselbox`, `nocursor`, `nodigits` — on its own patch, `markown.on`, which tacli
auto-arms at launch when `mark.on` exists. It covers health bars, group digits,
order/waypoint/build-queue markers, range circles and their `ShowRanges` labels, the
build-cursor footprint and the drag band box, and it stops the engine drawing its own copy of
the selection rect underneath ours. **Nothing world-anchored is captured any more** (G13p):
every one of them is re-drawn from engine state, because a capture cannot reach any of them at
zoom < 1 — the bar loop walks HotUnits, culled to the *unzoomed* viewport; the line drawers
clip to the offscreen's own width and height; and `DrawTextCustomFont` does not clip a string
at all, it **rejects** it whole when the box does not fit. The one capture window left is the
post-fog build cursor, and only `nocursor` opens it. Verify
`markown: ARMED (hook8/hook9/transp x2/selbox x2/tsprite x2/orders/digit/drawers x4 redirected …)`,
`MARKOWN capture=1 bars-skipped=1 selbox=1 cursor=1 orders=1 digits=1`, and
`mark: bars=N cursor=N ordtri=N ordline=N text=N(lab=N) atlas=N/N …` (`log`).

**The order markers are their own pass, `order.on`** (G13o) — tokens `log`, `passive`,
`trace`, `nobuild`, `nodots`, `nocircle`, `nosprite`, `noranges`, `nolabels` — riding the same
`markown.on` patch set. It walks the order lists **on the game thread** at the engine's own
driver call site and draws them at native resolution, so a queued build site or a waypoint out
in the zoomed-out ring is drawn where the capture reached nothing at all. It needs `mark.on`:
the geometry lands in that pass's buckets, and a disarmed `mark.on` hands the markers straight
back. Verify `order: ARMED (…)`, `markown: engine order markers SKIPPED (ours live)` and
`order: arena=N recs=N drawn=N lines=N dots=N …` (`log`). **`arena=-1` is normal** — it means
no snapshot ran in the last block, i.e. SHIFT is not held.

Three things to know:

- **Health bars need the `damagebars` registry option**, which is *off* when the value is
  missing — that is the engine's own gate (`main+0x37F06` bit0) and we honour it. Set it
  before launch under `HKCU\Software\Cavedog Entertainment\Total Annihilation`. **The group
  digit needs it too**: `0x469C97` skips the whole unit when the bit is clear, so with
  `damagebars` off a squad-tagged unit shows no digit in stock TA either. Assign a tag with
  `tacli keys <i> ctrl+1` on a selected unit.
- **Order markers only draw while SHIFT is HELD** — the engine samples its own hotkey
  `0xF9`, which this build resolves to `GetAsyncKeyState(VK_SHIFT)` (jump table at
  `0x4C1C48`, verified). `tacli keys <i> down:shift` … `up:shift` around a shot. We call
  the engine's sampler rather than reading the key ourselves, so a different keymap cannot
  make us disagree with it.
- **`passive` is the A/B lever** and hands *everything* back — bars, capture, the selection
  rect and the build cursor — so the engine draws the lot while we still gather and count.
  `nocursor` alone hands back just the build cursor and band box, which is the A/B for those:
  at zoom < 1 the engine's own are **clipped away** wherever the placement's engine coordinate
  leaves the screen-sized offscreen (at 0.467× on 1024×768, everything outside screen
  `[307,785]×[205,563]`), so `nocursor` is a way to see the bug, not a baseline for the look.
- **`order.on=passive` is the order pass's own A/B** — the engine draws its markers again,
  captured, which is the way to *see* the bound: at 0.25× on 1024×768 the engine reaches only
  screen x ∈ (432,688), y ∈ (288,480), so a site queued outside that band draws nothing.
  **`order.on=trace` runs BOTH sides in one pass** and logs both node lists
  (`order TRACE own:` / `order TRACE eng:`), which is the correctness gate — a pixel diff is
  unavailable, because native resolution means the frames deliberately differ. Diff them per
  block: the own-phase always precedes the eng-phase.
- **`ShowRanges` is a typed cheat, not a switch.** `tacli switches` only reaches
  SoftwareDebugMode; this one lives at `main+0x391BF`. Open the chat with `return`, type it
  with `char:` tokens and press `return` again:
  `tacli keys <i> return`, `tacli keys <i> char:+ char:s char:h char:o char:w char:r char:a
  char:n char:g char:e char:s`, `tacli keys <i> return`, then confirm with
  `tacli peek <i> '*0x511DE8+0x391BF:4'`. With it on, a selected unit draws nine def-range
  circles and three weapon ones, each labelled.
- **Text is closed too, as of G13p.** The group digit and the `ShowRanges` labels are ours —
  TA's own glyphs, rasterised through `0x4CCF60` into an atlas of ours and drawn at a constant
  SCREEN size. `mark.on=nodigits` hands the digit back for an A/B (at 1× the two are
  pixel-identical; at 0.25× in the ring the engine's is absent), and `order.on=nolabels` hands
  back the labels.

**Zoom has two levers, and the file wins.**

**`tagpu_zoom.txt` in the gamedir** — a bare float 0.25–8.0, re-read every frame; write it
**atomically** (temp + rename) or the DLL reads a torn value. This is the scripted lever, so
every scenario and every `tacli` recipe still drives zoom exactly as before.

**The mouse wheel** is what the player uses, and what the level falls back to whenever the
file is *absent*: one notch is ×1.1 geometric, clamped to the same 0.25–8.0, eased over
about six frames. It is live **only while our zoomed world is actually on screen and the
pointer is over the world viewport** — the menus, the side panel and the minimap keep their
wheel, and the log says which gate refused (`zoom: wheel ignored — no zoomed world on
screen` / `— pointer is off the world viewport`). Every accepted turn logs
`zoom: wheel +720 -> 1.000`, once per gesture rather than per notch.
`tacli arm <i> wheel.off` disables it live and **`tacli arm <i> wheel.off=off`
puts it back** — the re-enable is the flag's own removal, not a `wheel.on`,
which would only leave a stray file and a dead wheel.

```bash
tools/tacli wheel <i> -6 --at 576 384    # six notches out, pointed at the world first
tools/tacli wheel <i> 6  --at 576 384    # and back — this lands on EXACTLY 1.0
tools/tacli keys  <i> pmove:576,384 wheel:-6      # the same thing as raw tokens
```

- **Aim it.** `--at` is a `pmove:` first, and without it the notches land wherever the
  injected pointer was left, and if that is the side panel or a menu the notches do
  nothing at all.
- **Deleting `tagpu_zoom.txt` hands over, it does not reset.** While the file is there the
  wheel is pinned to it, so removing it leaves the view exactly where the file had it and
  the wheel continues from there. Wheel notches sent while the file is present are dropped.
- **Wheeling out and back lands on exactly 1×** — the round trip is snapped to `1.0f`, so
  the identity path really is the identity. The exception is a round trip that hit the
  0.25 or 8.0 **clamp**: the grid re-anchors there, so −15/+15 comes back at 1.044, not 1.
  Re-anchor with the file (write `1.0`, then delete it) when you need exactly 1× back.

`tacli arm <i> zoom.on` (at launch, like every other code-patching pass) installs the engine
patches either lever needs — the minimap view rectangle, the guard that keeps our
`ScrollSpeed` scaling out of the player's registry, and **the camera's range** — and logs
`zoom: ARMED (… camera range 0x41C3C0 + world guard 0x498EF9)`. Everything else needs no arm
at all, and every patch is inert at 1×.

**The camera's range is what lets a zoomed-in view reach the map edge.** TA clamps the eye to
the range that puts the *1× viewport's* edges on the map's, which at zoom > 1 stops the visible
window `W/2 − W/(2z)` short of every edge — 224 px at 2× on 1024×768 — so the map's edges and
corners could not be reached and the camera read as if it were being pushed back off them.
Armed, the range widens by exactly that, so `eyeX` goes **negative** at the left edge and past
`map − W` at the right; zoom back out and the eye walks home on its own within a frame.
`tacli eye` clamps with the same range, so a scripted camera reaches the edges too.
`tacli arm <i> zoomedge.off` disables just this and puts the eye back on the 1× range;
`zoomedge.off=off` removes the file again.

**To measure a camera bound, jump with the minimap and peek the eye.** Arrow keys do not scroll
(TA's scroll hotkeys are its own ids `0xF4`/`0xF5`/`0xF6`/`0xF7`, not VK arrows), but a *held*
left button on the minimap does jump the camera, and lands exactly on `world − (W/2, H/2)`
before the clamp:

```bash
tools/tacli keys edge1 mouse:10,0 down:lbutton   # top-left of the minimap click rect
tools/tacli peek edge1 '*0x511DE8+0x1431F:4' '*0x511DE8+0x14323:4'
tools/tacli keys edge1 up:lbutton
```

`pclick:` alone does **not** work here — the camera jump wants the button held across a frame.

**Edge scroll DOES fire under injected input — you have to land on the exact edge pixel.**
TA's mouse trigger is an *equality* on the outermost pixel, not a band: `x == 0`, `y == 0`,
`x == screenW − 1`, `y == screenH − 1`. Measured at 1× on 1024×768: `mouse:1023,400` scrolls
right, `mouse:1020,400` does nothing at all. (Earlier notes here claimed edge scroll was dead
under injection; that was a probe 3 px short of the edge.)

```bash
tools/tacli keys <i> mouse:1023,400     # scroll right;  x=0 left, y=0 up, y=767 down
```

**All four edges fire at every zoom since G13m.** They did not before: the engine takes the
mouse position from the record the `GetCursorPos` polls fill, which we used to answer with the
*unzoomed* position. Three of the four screen edges lie outside the viewport rect (`L=128`,
`T=32`, `B=screenH−33`) so they passed through untransformed, but the screen's right column *is*
the viewport's right column, so it contracted toward the centre — at 2× a pointer at `x=1023`
reached the engine as 800 and `x == 1023` was unsatisfiable. The poll answers the true pointer
now (`gpu-status.md` §2.3d). Measured at 1920×1080 on all four edges at 1×, 0.25× and 2×.

Three things to know when driving zoomed:

- **`tacli click` takes the position ON SCREEN**, the same as your eyes — the transform
  is applied on the far side of `g_ddraw.cursor`, so the injected path and the human's
  mouse cannot disagree.
- **The cursor sprite is the engine's own and sits under the pointer at every zoom**
  (G13m). If you are hunting a cursor artefact, `main+0x2C76`/`+0x2C7A` is the *unzoomed*
  point the engine is naming, not where the sprite is; the sprite is at the pointer.
- **At zoom < 1 the outer ring needs `vpwide.on`, or it is display-only.** The engine can
  only name screen positions inside its own 1× viewport, so without that arm the world the
  zoom-out reveals beyond it has no address: a click there is **dropped** (the selection is
  left alone rather than being moved to whatever sat at the 1× position). At 0.5× the
  addressable region is then the central half of the frame in each axis. Zoom ≥ 1 has no
  such limit either way.
- **In-game dialogs drawn inside the viewport take bent clicks at any zoom ≠ 1.** The
  transform's gate is geometric — inside the world viewport rect or not — so `ARMOPT`,
  `EXITMENU` and `YESORNO`, which the engine draws over the middle of the world, are
  treated as world clicks and unzoomed. Measured at 0.386×: `ui click MAINMENU` at its
  own (577,336) does nothing and its **pre-image** (576,365) hits it. Pre-existing (it is
  the same with the file lever), harmless (the keyboard is unaffected — `ui press` uses
  the gadget's quickkey — and wheeling back to 1× restores clicking), and not the same
  gap as the ring. `tagpu_zoom.h` promises dialogs arrive unmodified; it cannot see them.

**`tacli arm <i> vpwide.on`** (at launch) closes that: it widens the rect the engine
addresses to exactly what the zoom shows, so a ring click selects and orders normally.
Verify with `vpwide: ARMED (…)` and `vpwide: true viewport rect verified (128,32 896x704)`;
you will also see `vpwide: viewport rect restored to 1x` whenever the zoom goes back to 1.
It writes engine state (`main+0x37E27..0x37E33`, camera state only) and patches four more
sites, so it is **off by default** — arm it when you are testing zoomed play, leave it off
when you want the pre-G13f baseline. Two things it does not change: the captured **order
markers** still stop at the engine's screen-sized offscreen (further out than before, not
to the frame edge), and edge scroll behaves the same armed or not (see the zoom section: it
does fire under injection, on the exact edge pixel).

Since G13m `zoom.on` **on its own** also installs one of vpwide's redirects — the `0x498DA0`
mouse→world repair, which the zoom now depends on. You will see
`vpwide: mouse->world repair only (0x498DA0) — the viewport rect is never widened` in the log
where you used to see no `vpwide:` line at all. Nothing is written to the viewport rect in that
mode; the ring is still display-only. `vpwide.on` upgrades the same line to the full
`vpwide: ARMED (…)`.

**And with NEITHER armed there is now no zoom at all.** The lever used to work with no arm file,
which since G13m would mean a zoomed world whose *hover* named the point under the screen
position (clicks were fine, which made it worse). The level is pinned at 1.0 instead and says so
once: `zoom: PINNED AT 1.0 — the 0x498DA0 mouse->world repair is not installed`. If the wheel and
`tagpu_zoom.txt` both appear dead, that log line is why — arm `zoom.on`.

**Particles (smoke, fire, wakes, nanolathe) are `sfx.on`** — tokens `log`, `passive`,
`nosmoke`, `nofire`, `nowake`, `nonano` — on the same `fxown.on` patch set (tacli auto-arms
it when either `fx.on` or `sfx.on` exists), with its own live skip: `arm sfx.on="log
passive"` = engine draws while we count (`sfx: layers L2=44(wake:44) L6=26(nano:26) …`
every 60 frames), `arm sfx.on=log` = ours. Verify `fxown: ARMED … sfx@0x471F90=1` and
`FXOWN … sfx=1`; run it with `native.on=all` or the low layers (wake foam) composite over
engine-drawn hulls. Fixture: `scenarios/sfx-strait.json` (Anteer Strait: damaged structures
smoke, boats wake, a nanoframe to finish); lessons that cost an hour: a scripted `repair`
order does not make a builder nanolathe — select it, `ui click ARMORDERS`, `ui click
ARMREPAIR`, then `keys mouse:X,Y` + `click X Y` on the frame; the spray stops the moment
metal hits zero, and clearing the starting commander drops the storage to what the
remaining units provide (the fixture adds storage units); boats only path along the
water and head for the map's far end, so put the camera on their route; `ctrl+d` on a
selected structure gives burning debris (an extractor's explosion emits fire, a
storage's does not); `tacli log` returns a tail of the file, so count lines in the raw
`gamedir/tagpu.log` with `grep -a -c`.

## Things that will bite you

- **A launch or wait that "timed out" has usually crashed instead.** Check
  `tacli crash <name>` before theorising about loading screens — the commands do it
  for you now, but a hand-rolled poll will not.
- `pkill -f TotalA.exe` kills your own shell (the pattern matches the wrapper).
  Use `pkill -x` / `pgrep -x`, or just `tacli stop`. The same trap applies to `pkill -f` on **any**
  script name you are running from — `pkill -f mysweep.sh` inside a shell whose command line
  contains that string takes the shell with it.
- **`pgrep -x TotalA.exe | head -1` can name a ZOMBIE.** A game that has exited stays `Z` until
  its parent reaps it, and several runs leave several behind, so a liveness check built on the
  first pid reports DEAD while the game is running perfectly — which reads as a crash and sends
  you looking for one. Check `ps -o stat=` on every pid, not the first, or ask `tacli ls`.
- **`ErrorLog.txt` is shared between instances and is not cleared at launch**, so `tacli crash`
  can hand you a fault from hours ago. Delete it before a run you intend to attribute, and treat
  its timestamp as part of the evidence.
- `tagpu.log` contains binary bytes: always `grep -a` (tacli's `log`/`wait` handle it).
- Two X windows share each instance's title (frame + client), and the user's
  browser/Discord windows match the *substring* — tacli matches exact title + pid. The
  title now carries a per-tree label (above), so an instance launched before that existed
  still answers to the bare `Total Annihilation`, and tacli searches for both.
- The launch briefly warps the pointer to a screen origin (a wine-side quirk, not TA);
  tacli restores it and reports `pointer_restored`. Do not "fix" this with xdotool.
- The DEBUG build writes `cnc-ddraw-TotalA-*.log` at **~100 MB/minute** and rotates
  through three files, so a run older than a couple of minutes has already overwritten
  the trace you wanted. Delete them after every run; do not plan to read them later.
- If the user reports monitors blanking, check the Xorg output-probe count
  (`grep -c '(DFP-3): connected' ~/.local/share/xorg/Xorg.1.log`) before theorising —
  flat count rules out the wine XRandR cause. Two different bugs have caused this;
  both are written up in `windowed-mode.md`.
- Instances are cheap in disk (hardlinked prefix, symlinked gamedir) but each running
  game is a real GPU client — a handful at a time, not dozens.
- **`SELPROV`'s `SELECT` kills the game on the *non*-TCP/IP rows** —
  `Access Violation ... at 0023:00000000` in `ErrorLog.txt`, reproducible with plain
  `tacli keys` and nothing to do with `ui`. IPX was the row that did it.
  ***Internet TCP/IP Connection For DirectPlay* selects cleanly** and goes to `TCP.GUI`.
  **Select it by name, never by row number**: with wine's builtin DirectPlay it is row
  0, with native DirectPlay (below) the list is four rows in a different order and it is
  row 3. Reading the provider list and moving its selection are always safe.
## Multiplayer: two instances in one game

Works since 2026-09-02, over loopback, on the stock wine 9.0 these instances use.
What used to block it was wine's builtin DirectPlay, which implements the client half
only and cannot create a session at all (`DPWSCB_Open`: "session creation is not yet
supported", true through wine `master`); Microsoft's own DirectPlay in front of it
fixes that. `tools/dpinstall.sh` installs it into a prefix and `tools/dptest/` proves
a prefix can host before you go blaming the game.

```bash
tools/tacli launch h1 --dplay --free-dplay-port     # the host
tools/tacli launch j1 --dplay                       # the joiner
tools/mp_lobby.sh h1 j1 'Two Continents'            # menus -> battle room -> live
```

- `--dplay` installs native DirectPlay into that instance's prefix and appends the
  overrides to `ddraw=n,b`. It is **sticky per instance**, so a single-player instance
  keeps wine's builtin and nothing about it changes.
- `--free-dplay-port` kills a stale `dplaysvr.exe`. DirectPlay's name server outlives
  the game that started it and owns UDP 47624 **machine-wide**, so a leftover one makes
  the next host fail `Open(DPOPEN_CREATE) = DPERR_GENERIC` — which looks exactly like a
  broken prefix and is not. Put it on the **hosting** launch only: doing it while a peer
  is hosting takes that game down too.
- `MP_NO_START=1 tools/mp_lobby.sh …` stops in the battle room instead of starting, for
  when you want to read or change the lobby.
- After running `dptest` against an instance's prefix, **let it settle** before
  launching TA there. Starting the game into a prefix whose wineserver is still shutting
  down produced a launch with no process and no `ErrorLog.txt`; the relaunch was fine.

Three lobby facts that are not guessable, all encoded in `mp_lobby.sh`:

- **`START` ungreys only when every player is ready — the host included.** Each client
  lists *itself* as row 0, so the host's own toggle is `READY0` on its own screen and
  the joiner's is `READY0` on theirs. `PLAYER0`/`READY0`/`PLAYER1`… are created at
  runtime and sit past the end of the default `ui` snapshot; reach them with
  `ui <inst> show READY1`.
- **Lobby state syncs**, so set the map on the host and read it back on the joiner
  (`ui <join> show MAPNAME`) as a cheap proof the link is live.
- **`scenario apply` on the host replicates its units to the joiner** through TA's own
  create packet — which is what makes a scripted two-instance test possible. Apply on
  one peer only; both peers then see the units.

Do not `pkill -x dplaysvr.exe` by hand while another agent's game is hosting.

## The GL UI layer (Phase E — `tagpu_gui.on`, `tacli gui`)

Since G15b the UI — the in-game panel, build pages, bars, option screens, chat, the popups
and the whole shell — is drawn by our GL layer from the engine's own draw calls, replayed into
twins of its surfaces (`research/notes/gui-renderer.md` §10). The engine still draws its
surface, which stays the fallback beneath; with the trigger absent the DLL is byte-identical
to main's (parity md5 measured equal, §10). **`gui.on` is part of the default arm set** now.

```bash
tools/tacli gui <i> on            # arm BEFORE launch (the detours install at DLL attach); the draw follows the file live
tools/tacli gui <i> off           # keep the detours, stop the draw — the live A/B, 500 ms poll
tools/tacli gui <i> strict        # the harness's mode: fallback off, a miss painted magenta (never for a player)
tools/tacli gui <i> remove        # un-arm entirely at the next launch
tools/tacli arm <i> gui.on=norestore   # G15e: the layer WITHOUT Classic++ art — the UI-only A/B
tools/tacli arm <i> 'gui.on=sharptest log'   # G17a: the sharp layer filled with a known pattern
tools/tacli gui <i>               # report
tools/tacli log <i> -g 'gui: twins='   # heartbeat per 300 frames: twins= seeds= sprites= pixels= atlas= resets= overflows= k= sharp= fps=
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --res 1024x768 --layer --out /tmp/uiwalk
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --side core --layer --game-only --out /tmp/uiwalk-core
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --layer --cycles 3 --out /tmp/uiwalk-cycles   # G15d: three game->shell->game cycles
```

- `uiwalk.py --layer` arms `strict`, walks the shell and a game by gadget name, and at every
  stop takes the engine's surface and our GL frame and counts differing pixels (outside the
  world viewport in game, and inside it where the engine drew a non-key pixel; the cursor rect
  excluded) and magenta holes; `report.md` has one row per stop with the heartbeat's
  fps/resets/overflows. The bar is **0, 0 and 0 on every stop** — except `MAINMENU`, whose
  ~185 differing pixels are its sparkle animation between the two shots, single scattered
  pixels in the sky. Run it with the venv's python (numpy + PIL).
- **The in-game walk is side-aware and reaches the HUD extras (G15c).** `--side core` runs the
  CORE parity fixture (`scenarios/tascene-parity-core.json`) and walks `CORMAIN2`/`CORCOM1`/`2`
  with the `COR*` pagers; the in-game menu is `ARMOPT.GUI` on both sides. After the screens the
  walk types `+clock` and `+bps` in chat, holds SPACE over the commander (the Kills/Losses box,
  F4's twin), opens the menu, PREFS and F4 at 1x and then zooms them to 0.5x and 2x by writing
  `tagpu_zoom.txt` (so no click is bent), walks the commander for the minimap's dot, and
  releases the eye and edge-scrolls for the view box. `--screens-only` stops after the G15b
  inventory. Inside the viewport the walk compares only where the engine's surface is not the
  terrain key (index 254 in the 8-bit PNG `tacli shot` writes) — that is what `strict` calls a
  UI pixel there — and reports `vpdiff/vpui` per stop; the cursor rect (`*0x51FBD0+0x1B6/+0x1BA`,
  size from the record at `+0x1B2`), padded 8 px because the sprite animates between the two
  shots, is excluded everywhere. The clock stop is taken with the menu open: `ARMOPT` pauses
  the sim and the seconds with it (the game clock is the tick `main+0x38A47` ÷ 30).
- **`ARMOPT` is not over the viewport** — its record is `xpos=0 ypos=128 128×352`, the side
  panel's rect; it replaces the build panel and pauses the game (`PAUSED` in the middle of
  the world). The screens over the world are `PREFS`, `VISUALRT`, the F4/SPACE box and the chat.
- **The live zoom is readable from `mark.on=log`**: its line every 120 frames carries `zoom=`;
  the file lever itself logs nothing. The walk arms `mark.on=log` for that and records the
  level per stop.
- Read `gui: ARMED flip@0x4C63A0=1 leaves=16/16` at launch, then `gui: layer ON` and
  `gui: GL ready`. **`resets=` is 2 per launch (the arm, the shell→game switch) and +1 per
  context switch after that** — each one is logged with its reason (`gui: reset #n: stall-over
  …`; the other reasons are `arm`, `gl-context`, `queue-full`, `arena-full`,
  `box-outside-surface`, `lost-sprite`, `atlas-full`, `untwinned-copy`) when `log` is in the
  trigger, so a count that grows without a switch has a name. `overflows=` and `lost=` must stay
  0; `stalls=` is 1 per context switch (the publisher dropping batches while the render thread
  is dead or crawling — G15d), `skipped=` the stale ops the render thread stepped over after a
  context change. `palchg=` counts palette uploads (a fade is a run of them; a switch costs a
  few) and `paldiff=n@i` the entries where the presented palette differs from `main+0x143A7`
  and the first of them: **whatever the live `Gamma` makes it, not a property of an instance**
  (below) — 235 at factor 1.125 and 0 at 1.0, in the shell and in game alike; 255 after
  `+gamma 15`, 0 after `+gamma 10`. In the
  shell exactly one entry, index 9, differs *beyond* the gamma scale.
- **The presented palette is not `main+0x143A7`**: the engine scales every palette it sets by
  the Gamma factor on the way to DirectDraw (`SetGamma 0x4BA590`) and never scales its table.
  The scale **truncates**. **The factor has two formulas**: an option screen applies
  `0.5 + Gamma/24` (1.0 at the engine's own default of 12), while **`+gamma N` typed in chat sets
  it to `N/10` outright** (`+gamma 15` → 1.5, `+gamma 10` → 1.0) with no cheat bit — the lever
  that makes the two palettes differ in a skirmish. **`Gamma` is one shared, mutable value, not a
  property of the template**: `wineprefix/user.reg` and all 58 instance prefixes are the same
  inode and wine rewrites it in place at launch, so it is whatever TA last stored — 15 (factor
  1.125, `paldiff=235`) and 12 (factor 1.0, `paldiff=0`) have both been read on the same day
  (MEASURED 2026-09-09). **Read it, never assume it.** **Since 2026-09-09 everything we draw
  follows the presented palette**, the world included (`tagpu_pal.c`, gpu-status §2.3f);
  `tagpu.log`'s `pal: presented palette changed (… gamma=…)` line is where to read the live
  factor, and `tacli peek <i> '*0x511DE8+0x37F08:4'` gives the *option*, which after a `+gamma`
  no longer implies the factor.
- **Type a chat line slowly, and check it before you send it.** `keys return`, the `char:`
  tokens, `keys return` back to back drops the opening `return` often enough to matter: the run
  then types the cheat into the game as hotkeys and nothing happens, silently. A second of sleep
  between the three, and a `glshot` of the bottom strip to read the chat field back, is the
  difference between measuring and guessing (2026-09-09).
- **The cycles (G15d): `--cycles N`** runs, after the in-game stops, N times game → shell → game
  in the same process: `park`, Tab, `EXIT`, `MAINMENU`, `CHOICE1` (the return: the game freed,
  640×480 restored, a new GL context), `ui wait --gui MAINMENU`, the whole shell inventory
  again, `SINGLE`, `Skirmish`, `ui set Mapping 1`, then **the loading screen held under
  `strict`** (`Walk.stop_loading`: `Start`, then a bracketed surface/GL/surface triple every
  second until `units: alive=` appears *after* the click — read the log by byte offset, a
  `tacli log` grep sees the previous game's lines — the row is the worst sample and the count),
  `scenario apply` of the fixture (works on a running game; the skirmish's own commanders are
  cleared), the side's screens, `+gamma 15` and `+gamma 10`. `EXITMENU` and `YESORNO` sit over
  the middle of the world, so the cycle runs at zoom 1. A full run — shell, 32 game stops, three
  cycles — is ~110 stops and ~30 minutes: `setsid nohup … & disown` and poll the log; three
  instances at once are fine for parity, not for the fps column.
- **A frame rate for any DLL, the module's own heartbeat aside**: the overlay logs a `units:`
  line every 30 presented frames, so timing their arrival in `tagpu.log` from outside is an
  fps meter that needs no code — `30 × intervals / elapsed` (the G15b measurement used exactly
  that against main's DLL).
- **Classic++ UI (G15e, 2026-09-08).** With `classicpp.on` the UI art is restored too: every
  surface carries a colour twin beside its index twin and the layer picks per texel. Read
  `gui: Classic++ UI armed …` and the heartbeat's `cpp=1 col=<made>/<live> colvalid=1 rgb=<tex>`;
  `restoreglsl: gui: lazy restore armed` says the job was created. **`gui.on=norestore` is the
  A/B** — the same DLL, the layer up, Classic++ art off — which is how the 37 435-of-45 056-px
  panel-rect difference was measured. Two things will waste your time otherwise:
  - **On entering a game the panel is INDEXED, and that is expected.** Colour reaches a twin only
    through a sprite op, and the panel is *seeded* at the mode switch, so the atlas holds only
    the small HUD icons (25 entries, none over 10×12, all under the 12-px restore floor). Open
    `ARMOPT` with Tab, or select a builder, and real UI art is drawn — the atlas goes to 44 and
    the panel restores. A `norestore` A/B taken before that differs by **0 px** and means nothing.
  - **`uiwalk.py --layer` is NOT a valid regression with Classic++ on.** It diffs our frame
    against the engine's **indexed** surface, so every restored pixel counts as a difference. Run
    the walk with `classicpp` off (or `gui.on=norestore`). **The restored half's own check is
    `--restore`, below.**
  - The palette rule is testable with G15d's own lever: `+gamma 15` in chat → the heartbeat shows
    `colvalid=0` for a moment, then `rearms=` +1 and `colvalid=1` again. It does **not** move
    `paldiff` off 0 — `paldiff` already reads **235** on a stock instance (next bullet).
  - **`paldiff=235` is the ORDINARY reading here, not a `+gamma` one** [MEASURED 2026-09-09].
    **`paldiff` is whatever the live `Gamma` makes it, and `Gamma` is one value shared by the
    template prefix and all 58 instances** (the same inode — `clone_prefix` is `cp -al` — rewritten
    in place by wine at every launch). At 15 the factor is `0.5 + 15/24 = 1.125`, truncated (the
    presented palette reproduces exactly as `min(255, (int)(e × 1.125))`), and 235 of 256 entries
    differ from `main+0x143A7` in the shell and in game alike; only index 9, in the shell, differs
    *beyond* that scale. At 12 the factor is 1.0 and `paldiff` reads **0**. Both were read on
    2026-09-09, hours apart, on the same DLLs. **So read `paldiff=` from the heartbeat before
    trusting any measurement that depends on it** — do not assume 235, and do not "fix" the
    registry value, which would move every measurement taken against that prefix. Two standing
    consequences: **the world passes are dark by the factor whenever it is not 1.0** (they read
    `+0x143A7`; at 1.125, 14 896 of a 17 049-px Classic viewport sample are exact `palette.pal`
    colours against 244 presented ones), and **anything comparing a restored twin to an offline
    restore must use the presented palette**, never the archives' `palette.pal`.

- **The composite is three layers since G17a (2026-09-09)**, and the heartbeat says so with
  **`k=` and `sharp=`**: `k` is device pixels per twin texel (`vp_w / twin_w`, read off the frame)
  and `sharp=WxH` the sharp layer's size (the *viewport*, not the client). **`k` is not always
  1**: `resizable` defaults TRUE and `maintas` fits the viewport to the client, so any window
  dragged off the game resolution is fractional, and the second game→shell return at 1920×1080
  gives `k=2.000 sharp=1280x960` (a 1280×984 client over the 640×480 shell). **At integer `k` the
  ramp is exactly nearest**, so `k = 2` proves the seam holds and proves nothing about the blend
  — that needs a fractional `k`, which is G17b's.
  - **The mirror is unchanged and so is the `strict` walk**: the 4-tap ramp is bit-identical at
    `k = 1` by construction, so `uiwalk.py --layer` is still the regression it was (0/0/0 at every
    stop but `MAINMENU`'s sparkle). If it ever stops being, the ramp is what to suspect first.
  - **`gui.on=sharptest` is the lever that proves the sharp layer exists.** It is empty until
    G17c/G17d, so nothing else can tell a wired layer from dead code. It paints a 64×64 opaque
    green square at the viewport's **top-left** and a **one-device-pixel** white column at device
    x = 100, both drawn as **geometry** (the client kind G17c and G17d will be); check the
    square's bbox is `(0,0)-(63,63)` in a `glshot`. **There is no y flip in the sharp layer** — a
    client uses `QVS`, the twins' own vertex mapping, and one that adds a flip draws upside down.
    Harness only, like `strict` — never hand a player an instance with it armed.

### Driving and measuring at k != 1 (phase 2, G17b)

```bash
tools/tacli launch <i> --res 1280x720 --window 1920x1080   # engine 1280x720 in a 1920x1080 client => k = 1.5
tools/tacli click <i> 1056 764 --device                    # CLIENT-AREA pixels, converted by the engine's own path
tools/tacli ui <i> click SINGLE --device                   # aim where the gadget is DRAWN
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --res 1024x768 --window 1536x1152 --layer --device --cycles 3
```

- **`--window WxH` is the whole trick, and it needs no engine patch.** cnc-ddraw takes
  `ddraw.ini`'s `width`/`height` as the client and maxes them against the game mode
  (`dd.c`), so the engine keeps its own screen and the fork letterboxes it: `k = window / res`.
  It is sticky in the instance's meta, like `--res`. The **shell is atom-locked at 640x480**, so a
  1536x1152 window puts the shell at k = 2.4 and the game at k = 1.5 in the same run.
- **`--device` is the only click that tests the pointer path.** Every other injected event is
  delivered in the engine's own coordinates (`tagpu_shield.c` `deliver_mouse`), so it never
  touches `mouse.unscale_*` and would pass at any k, right or wrong. `--device` posts
  client-area pixels and lets `mouse_client_to_game` — the same function a hardware click takes
  — work back to a logical pixel.
- **`uiwalk` runs a hit check at every stop whether or not you pass `--device`** (it costs a
  snapshot, no clicking): every gadget aimed where the renderer draws it, put through the fork's
  own inverse, checked back inside its own rect. Read `MISS=` and `drift=` on the per-stop line
  and the four new `report.md` columns. **Worst drift is 1 logical pixel** — the renderer scales
  by `vp/surface` and the input unscales by `(surface-1)/(vp-1)` — so it is a hit test, not a
  pixel test. Zero-area rects are counted as `degenerate`, not misses.
- **Pixel parity reads `-1` at k != 1 and that is correct**: `frame_parity` refuses a stop whose
  GL frame and surface are not 1:1. The hit columns are the measurement there.
- **Do not walk cycles at 1280x720.** Leaving a game at that mode crashes in the level teardown —
  at k = 1 too, so it is the mode and not the scaling ([resolution](resolution.html) §3.1d).
  `--res 1024x768 --window 1536x1152` is k = 1.5 on a mode with clean teardowns on record.
- **The world can be drawn at the device's resolution since G17b, and it is OPT-IN**: arm
  `tagpu_devres.on` and `ss` follows `ceil(k)` with the box-resolve to game resolution skipped,
  so the composite downsamples rather than nearest-stretching. The native log line carries
  `devres=` beside `ss=`. It is not the default because a selection rect drawn in an `ss` buffer
  is one *supersample* wide (the driver clamps aliased line width to 1), which under `devres`
  reaches the screen thinner and dimmer than the engine's — that wants the rects drawn as real
  geometry first.

### The Q2 diff — is the restored UI right? (G15e)

```bash
tools/uiwalk.py --inst <i> --restore --out /tmp/uirestore      # arms gui.on=log + classicpp.on + restoredump.on
tools/tascene uidiff /tmp/uirestore/shell-tagpu_restore_gui    # the shell's art
tools/tascene uidiff /tmp/uirestore/game-tagpu_restore_gui --sheet /tmp/near.png
```

- `--restore` is the third walk mode: no census, no `strict`, no shots. It just drives every
  screen so the UI atlas fills, then copies the DLL's dumped twin out. **The dump is taken once
  per phase** — the atlas does not survive the shell → game switch (`gui: atlas reset`), so a
  single copy at the end would carry the HUD and nothing else — and each phase's **presented
  palette** is saved beside it as `<phase>-tagpu_restore_gui.pal`, read out of a `tacli shot`'s
  8-bit PNG. `uidiff` picks that up automatically.
- `tascene uidiff` restores each dumped cell **offline** with the same model and holds the twin
  to it under `featdiff`'s two-band bar. There is no UI pack and no sequence-name registry, so
  the reference is the dump's own cells: coverage is total and `unmatched` should read 0. Entries
  under the 12-px restore floor were never queued and are counted, not diffed (they are most of
  them: 90 of 149 in game).
- **It restores each cell with the tileability flag the dump carries** (`wrap` in the `.idx`) and
  reports any whose flag the live palette would not produce. That is not pedantry: `e->wrap` is
  decided once, when the frame is first atlased, so a frame first seen while the palette is still
  uniform keeps a wrong flag for the session — one such entry came out 10 levels off over half its
  texels under `--wrap auto` and byte-identical under `--wrap yes`.
- The bar: far band **max 1 level and under 0.01 %** of bytes; the near band (within the model's
  reach of a keyed texel) is *reported*, not barred. Measured 2026-09-09: 0.0007–0.0017 % far, and
  a near band of mean 0.435 / max 8 — tighter than the feature twin's 0.41 / max 23.
- The census below still works and is still the regression for "a writer we do not observe".

### The UI census (G15a)

```bash
tools/tacli gui <i> census                            # = 'gui.on=census log pgm trace', at launch
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --res 1024x768 --out /tmp/uiwalk   # the inventory walk + report
tools/tacli arm <i> gui_census.trigger              # the accumulated residual mask -> gamedir/tagpu_gui_census.pgm
tools/tacli log <i> -g 'gui census:'                # per-window lines: changed=, unexplained=, box=, ops=[…]
```

- Read `gui: ARMED flip@0x4C63A0=1 leaves=N/N` first; `NOT armed — engine bytes differ` means a
  site is owned by a module that installed after it (the observer chains onto `fxown`'s
  `0x4B7F90` stub, so the default arm set is fine).
- `changed`/`unexplained` on a `gui census:` line are the **window's totals since the previous
  line**, not one census; a residual > 256 px logs at once with the ops that intersect it
  (`trace`). Surfaces other than the presented one are reported only when they have a residual
  — the PCX backgrounds and the `SAVEMOUSE` buffers always do (the loader and cursor code write
  them directly), which is expected.
- `uiwalk.py` drives the shell and a game by gadget name and writes `report.md` with one row
  per stop; it needs no shots to work, but takes the engine surface at every stop.
- **`tacli shot` works in game again** since 2026-09-07: the window title's `wt:… | tacli:…`
  label put `:` and `|` into the PNG filename, which is why the surface shot silently never
  appeared in game while the shell's bare title was fine (`screenshot.c` now sanitises it).
