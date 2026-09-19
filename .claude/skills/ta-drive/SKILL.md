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
   That compares a **copy against the file on disk now**, which is what it is for. It does
   **not** work against an md5 written down earlier: the link is not byte-reproducible, and
   two builds of the identical tree differ in three bytes (the PE timestamp, its copy in the
   debug directory, and the checksum — measured 2026-09-15). To answer "is this the binary
   those figures were measured on", check the *tree*, not the hash.
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

**Leaving a game for the main menu is four steps, and none of them is Esc.** The button
that opens the exit menu sits on `ARMOPT.GUI`, which **Tab** raises over the world — not
on the side panel, and not on the top bar (its `MOREBAR` row is collapsed, and at any HUD
scale above 100 % the right of that bar is off-screen anyway). Esc in game does nothing
at all: its code lands on the in-game dispatcher's default case. So:

```bash
tools/tacli keys t1 tab tab          # TWICE: a keys invocation drops its first token
tools/tacli ui   t1 click EXIT       # ARMOPT.GUI -> EXITMENU.GUI
tools/tacli ui   t1 click MAINMENU   # -> YESORNO.GUI, "Surrender this battle…?"
tools/tacli keys t1 y y              # CHOICE1 — the click on it is unreliable, the key is not
tools/tacli ui   t1                  # confirm: MAINMENU.GUI 640x480
```

The shell runs at **640×480** whatever the game ran at, so `tacli ui` reporting
`MAINMENU.GUI 640x480` is how you know the level really tore down. `EXITMENU.GUI` also
carries `RESTART`, `EXITGAME` (quit to the desktop) and `CANCEL`.

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

**The game speed is SHARED STATE, and it is not reset by a launch.** `gamespeed` lives in
`HKCU\Software\Cavedog Entertainment\Total Annihilation`, i.e. in `user.reg` — which the
template prefix and all 58 instance prefixes hold as **one inode** (`clone_prefix` is
`cp -al`; 101 links, measured 2026-09-09), exactly like `Gamma`. A running instance keeps
its own copy and writes it back at exit, so one session pressing `+` silently leaves every
later launch of every instance at that speed. **It was found at 20 — double speed — on
2026-09-09** and set back to 10.

```bash
WINEPREFIX=<inst>/prefix wine reg query \
  "HKCU\Software\Cavedog Entertainment\Total Annihilation" /v gamespeed    # 0xa = TA normal
```

Read it before any measurement whose answer is a rate, a duration or a distance-per-second.
`gamespeed` multiplies the **tick** rate (`main+0x38A47`: 30/s at 10, 60/s at 20) while the
picture still changes 30 times a second — units simply take bigger steps — so at 20 the
in-game clock (`tick ÷ 30`) reads **2× real time**, and any artifact whose size depends on
how far a unit moves per sim step is twice what a player at normal speed sees. Full numbers:
`exe-reverse-engineering.md` §"The engine's rates".

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
tools/tacli scenario load     t1 500v500        # the big fight: 1000 units, 1920x1080 (UI-damage fixture)
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
- **`tagpu_posedump.on` no longer reports `err=`** (landing 3). It dumped one unit's pose fields
  and, beside them, the largest disagreement between our reconstruction and the ENGINE's posed
  vertex buffer at `PrimitiveStruct+0x22` — a per-unit `Object3do` read on the render thread, which
  is what the packet removed. The fields the dump exists for are unchanged and `tools/tacob
  pose-check` is unaffected; what is gone is that column and the `v0..v2` lines.
- **`tagpu_writeback.on` does nothing: the write-back is deleted** (landing 3). It rendered one
  unit's posed 3DO into an FBO and wrote the pixels back into the engine's composite plane from
  the render thread. The native pass has drawn units directly since Phase C, and a render-thread
  store into engine memory is what the exchange exists to remove.
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
  seconds apart, is the test (the sim tick; **30 per second at `gamespeed` 10, TA's normal —
  the counter scales with `gamespeed`, so it reads 60/s at 20**, see *The game speed is
  shared state* below); `tacli shot` failing and the
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
file the play set is on, Classic++ and the extra weapons included). Note that the play set is
**wider than the `arm` line above**: since 2026-09-14 it also carries `ghost.on`, the building
preview at the placement cursor. It is deliberately left out of the bench line — it adds posed
draws that perturb a measurement — so a `defaults.off` instance has no ghost unless you arm it,
and `--defaults` (the player's configuration) does. An instance opts out:
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
| `SELHANDBACK=<n> last=<drawn>/<owed>` | frames the pass came up short on selection rects and `markown` handed the **whole set** back to the engine, which then draws every box at the **unzoomed** projection. Harmless at 1×, a one-frame scatter at any other zoom, and it must not climb steadily: 0 over a four-minute `500v500` run with ~470 units selected at 0.42× [MEASURED 2026-09-09] |

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

**A MIPPED TWIN ALSO DUMPS ITS WHOLE CHAIN, to `tagpu_restore_<tag>.mips`** — every level end to
end, `sum(max(1, dim >> L)^2 * 4)` bytes for `L` in `0..mip`, logged as `unit: the twin's mip chain
dumped to tagpu_restore_unit.mips (3 levels of 2048, 21504 KB)`. It is the instrument for the one
question a level-0 dump cannot ask: the unit twin is sampled trilinear, so its levels are part of
the picture a second backend has to reproduce. **Since landing 7e-1 the levels are ours, not
`glGenerateMipmap`'s** — the exact integer 2×2 box average `(sum + 1) / 4` — so reducing level
`L-1` of the dump in Python and comparing with level `L` must come out **100.00 % exact, max |Δ| 0
on every channel**.

**A result within ±1 means the reduction stood down and `glGenerateMipmap` ran** — that is §2.45's
measured bound for this driver, and it is the figure to recognise rather than to debug. Grep
`mip reduction` in `tagpu.log`: since landing 7e-1's review **all six** ways to stand down say so,
once per GL context, with the reason — the restorer not up, the program not built,
`glGetTexParameteriv` missing, an odd level, no framebuffer, or a level that failed with its GL
error. Four of them were silent before that, so on an older build an empty grep means nothing.
**Only the unit twin reduces at all**: the terrain, feature, effects and UI atlases are unmipped
(`mip` 0), and `twin_mips` returns before the reduction for them.

**THE DUMPS ARE A CROSS-BUILD BYTE ORACLE, and it is the cheapest strong one this repo has.**
`tagpu_restoredump.on` reads the finished atlas with `glGetTexImage` off a **texture**, not off the
framebuffer — so unlike every capture-based A/B it needs no visible window, no Route D, no parked
pointer and no settle heuristics, and it answers a much harder question than a screenshot does:
whether two builds produce the *same pixels* for the whole restore rather than for one frame of it.
Used to prove the landing-7 split changed nothing:

```bash
# one instance, the two DLLs swapped under --keep-dll so tacli does not refresh
tools/tacli arm <i> classicpp.on terr.on feat.on fx.on native.on restoredump.on 'restoreglsl.on=log'
cp <build>/ddraw.dll <gamedir>/ddraw.dll        # md5sum it and print that line
tools/tacli launch <i> --keep-dll --res 1024x768 --maxfps 0
tools/tacli scenario load <i> feat-forest --restart --res 1024x768 --maxfps 0
# poll tagpu.log for `terr: done`, then give the lazy queues ~25 s, then cmp the files
```

- **Compare the four CODE-DETERMINED counts in the `done` line, never the timings.** `N frames
  (W wrap-padded) in B batches, D draws` are properties of the map and the model; the `in S of F
  frames`, the wall ms and the fps are properties of the machine that hour, because the slice
  budget is wall-clock driven. A run that matches on pixels and differs by 2 slices is a match.
- **Use two fixtures, because they cover different halves of the scheduler.** `static-terrain`
  exercises only the ONE-SHOT job (a fixed list added once). The open QUEUE path — `job_add` while
  a run is live, size-class mixing, the batch-boundary re-pick, the `queue drained` tally — needs
  an atlas that misses lazily, and `feat-forest` with `native.on` gets there through the **unit**
  atlas, whose `tagpu_restore_unit.{r8,rgba,idx}` are all comparable.
- **`feat` and `fx` arm their jobs on that fixture but often never drain inside the window**, so
  do not read their `lazy restore armed` line as coverage. Check for the dump FILE, not the arm.
- **The `.idx` file is worth diffing too** — it lists every entry, so it catches a twin that holds
  the right pixels in the wrong cells, which the `.rgba` alone would also catch but the `.idx`
  localises in one line.

**AND SINCE LANDING 7c IT IS A SAME-RUN TWO-LANE ORACLE, which is strictly stronger than the
cross-build one above.** With `restorevk.on` the gather half stops mirroring its restored atlas for
the Vulkan lane and publishes the frame LIST, so both restorers run in **one process** over the
same atlas, the same palette and the same rectangles in the same order, and each writes its own
dump:

```bash
# native.on=all wrecks is NOT optional for the sprite atlases -- see below
tools/tacli arm <i> 'vk.on=color=0,0,0' 'native.on=all wrecks' terr.on feat.on fx.on sfx.on \
                    classicpp.on 'restoreglsl.on=log' restorevk.on restoredump.on
tools/tacli launch <i> --res 1024x768 --maxfps 0
tools/tacli scenario load <i> fx-mix --restart --res 1024x768 --maxfps 0
# let it settle, then one cmp per atlas that came up:
for t in terr feat fx unit; do
    cmp <gamedir>/tagpu_restore_$t.rgba <gamedir>/tagpu_restore_${t}_vk.rgba
done
```

- **The pair is `tagpu_restore_<tag>.rgba` against `tagpu_restore_<tag>_vk.rgba`, for every
  consumer** — the Vulkan half moved out of `tagpu_vk_terr.c` into `tagpu_vk_restore.c` in landing
  7d, so each job dumps its own destination and a new consumer gets the oracle for nothing. The
  terrain's GL dump was renamed `tagpu_restore_terr.rgba` to match; **`tagpu_restore.rgba` and
  `tagpu_restore_vk.rgba` are the pre-7d names and no longer written.**
- **`feat.on` alone does not make a feature atlas exist.** Without `native.on=all wrecks` the
  feature pass never owns the leaf, emits nothing and atlases nothing: the log says `atlas=0` and
  `(nothing emitted: native.on needs "wrecks" before we can own the leaf)`, and the run measures a
  clean terrain pair and two missing files. One whole run was spent on this. **Check for the dump
  FILE and the `atlas=` count, not for the arm line.**
- **A sprite atlas's two lanes dump on each lane's own `idle`**, so compare after the scene has
  settled — an effects atlas that is still adding frames can be caught at two different moments,
  which is a property of the measurement and not a bug in the lane.

### The source-comparison line says nothing about what either lane READ

`unit SOURCE: IDENTICAL` compares the GL texture against the Vulkan device image **after
everything has settled**. It cannot see a source that was empty *while* a lane was restoring, and
it will happily assert "the two lanes restored the SAME bytes differently" when the premise is
false. Landing 7e-2 lost most of its length to reading that line as evidence.

**When a dependent lane's picture is wrong, measure its INPUT at the moment of use before
reasoning about its arithmetic.** The probe that settled 7e-2 in one run was a temporary edit to
`TAGPU_RESTORE_OUT_FS` making it report the index it had read —
`frag = vec4(pi/255, (key+1)/255, 0, 1)` — compiled into **both** lanes, so the dump became a
direct two-lane comparison of what each lane sampled. Only the R channel differed: GL read the
art, Vulkan read 0 everywhere. Reading the device image back **early** (snapshot the dump file
while the run is still going, rather than after it) then showed 404 798 texels missing where the
same read-back taken later is exact. Editing a shader string re-hashes the generated SPIR-V, so
run `tools/spirv-gen.py` after the edit and `git checkout` both the header and `inc/spirv/`
afterwards.

### Wait for the dump lines, never for a clock (measured 2026-09-17)

**A fixed `sleep` in an oracle script is both slower and weaker than polling the log.** The
harness that drove landing 7e-2 slept 150 s; every byte it compares is on disk about **9 s**
after the world goes live, and the whole cycle came down from **~200 s to 46 s** by replacing
the sleep with a poll — same figures, to the byte, on the same build.

It is weaker as well as slower because the sleep asserts *nothing*. Both lanes already refuse
to dump a half-finished picture — the GL side needs `tagpu_rglsl_job_idle` and a moved entry
count (`tagpu_gaf.c` `dump_if_armed`), the Vulkan side needs `qn == 0`, `!inflight` and
`painted >= 1` (`tagpu_vk_restore.c` `dump_step`) — so "every dump line present, then quiet"
is a real precondition where a clock is a guess. A sleep that lands mid-paint `cmp`s a 154-entry
GL dump against a 155-entry Vulkan one and reports a DIFFER that belongs to the instrument. The
poll caught exactly that twice on the day it was written: a broken build reported
`unit CHAIN: missing (gl ok, no vk)` instead of a plausible-looking byte count.

Poll for **all** the expected `dumped to` lines, then require a quiescence window (10 s is
plenty) in which no new one appears *and* the dump files' sizes stop moving. Keep the old
settle value as the **timeout**, not as the wait. Do not gate on the two lanes' counts being
equal to each other: GL prints `%d entries` (`a->n`) and Vulkan prints `%d frames`
(`core->tframes`), which are different quantities that happen to agree when every entry is
painted once. Require each lane's own count to be stable, and the two files to be the same size.

### One launch, not two — and the second one destroys the window you parked

`tacli scenario load <i> <fixture> --restart` **stops the instance and launches it again**
(`cmd_scenario_load`). A script that calls `tacli launch` first, parks that window with
`xdotool`, and then calls `scenario load --restart` has thrown away the window it parked and is
showing the human the *second* one, placed from the instance's recorded tile. Drop the first
launch: it costs ~20 s and it is not the run being measured.

**Then park the window that actually survives, and fix the tile at the source.** The recorded
tile in `tagpu/instances/<name>/instance.json` is what places it, and on the reference setup one
instance was recording `[1064, 0]` against a primary head starting at x=1080 — so 1008 of a
1024-wide window sat on the human's own screen for the whole of every run. Read the heads with
`xrandr --query`, pick the smallest connected **non-primary** one, and write that tile into
`instance.json`; `tile_is_onscreen` will keep it. **`--res` on the launch recomputes the tile and
undoes this**, and it buys nothing when the instance already records that resolution, so leave it
off (`--maxfps 0` alone rewrites `ddraw.ini` from the recorded tile).

This is the mechanism behind the standing rule that the desktop is the human's: the rule is not
satisfied by an `xdotool` park alone, because a later `--restart` silently un-parks it.

- **The GL lane's own log prefix is `restoreglsl:` and the Vulkan lane's is `restorevk:`**, both
  through the shared core, and `restoreglsl.on=log` turns on *both* — the options are the core's,
  read once per backend. Diffing the two lanes' `batch N (S.., CxC slots, K frames)` lines is how
  a divergence gets localised to a batch.
- **No second launch means no second machine-state**, so the only difference left is the two
  implementations. This is what found landing 7c's six unpainted cells; a cross-build run would
  have found them too, but it could not have told a scheduler difference from a draw difference.
- **A difference that is a multiple of the CELL PITCH SQUARED is a dropped frame, not a wrong
  pixel.** `34² = 1156` for terrain (32 px tile + 1 px border each side): divide the differing
  texel count by it and you have the number of cells that were never painted. Cluster them by
  `(y / 34, x / 34)` before theorising — six whole cells and every other cell identical is a
  completely different bug from 6 936 scattered texels, and the count alone does not tell them
  apart.
- **The lever suppresses the read-back, and the producer publishes one or the other and never
  both** — with `restorevk.on` there is no `atlasRgb` in the hand-over at all. Turning it off is
  what puts the shipped mirror path back. (It is an either/or written as one, not an exclusivity
  that follows from the arm: the lever is a poll that can land on any frame, and the first version
  of this claim was wrong for exactly that reason.)
- **A SPRITE ATLAS IS WHAT MAKES A SWAPPED PER-BATCH TABLE VISIBLE, and the terrain is blind to
  it** (landing 7d, [gpu-status](gpu-status.html) §2.44). The restorer's per-frame tables carry
  each frame's rect, source rect and **colour key**; the terrain's frames are one size with no key,
  so a frame restored through another frame's table costs a source rect and looks plausible. On a
  GAF atlas the same fault paints **the key's own palette colour, opaque (84, 84, 252), where the
  GL twin writes (0, 0, 0, 0)** — which is a signature worth recognising: if a `_vk` dump has
  opaque key-coloured texels the GL dump has transparent, a per-frame parameter reached the shader
  from the wrong frame.
- **AND THE LOG LINE DOES NOT SHOW THE CONDITION FOR IT**, which the landing review corrected.
  `restorevk: <tag>: batch N … issued at slice M` is printed at the batch's **OUT**, so two of
  those lines sharing a slice is the condition for the *vertex* collision (landing 7c), whose data
  was written at OUT. The tables ride the **FILL**, and a batch's FILL is normally several slices
  before its OUT, so two batches sharing a slice by that line says nothing about them. The table
  collision needs two batches whose **FILLs** share a slice, which happens when the batches are
  small — a sprite atlas, not the terrain, whose 64-frame batches put at most one FILL in a slice.
  Both conditions are **timing-dependent**: a clean run proves nothing about either.

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
  overlay tints every tall feature purple. **It is a Vulkan-ported pass since G19e**, so arm it
  when the Vulkan lane's A/B is what you are running, and not otherwise.
- `writeback.on` — the older per-type sprite composite, targeted at one unit name;
  `native.on=all` + `owndraw.on` is the path that replaced it.
- `weapons.on` — sim-changing, and inert without `.ufo` content built for it. Arm it
  for the extra-weapons work, not for a play session.
- The `.off` flags (`curs.off`, `wheel.off`, `zoomedge.off`, `ss.off`,
  `subpix.off`, `nano.off`, `r3dcache.off`, `overlay.off`) — these **disable** features.
  Arming everything means leaving all of them absent. (**`shade.off` was on this list and does
  not exist** — nothing in the tree reads the file. Found 2026-09-15 looking for it as an A/B
  lever; only `tagpu_render3do.c`'s comment still mentioned it, and that is corrected too.)
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
- **The frame packet exchange is on by default and `packet.off` is its A/B lever, not a feature
  switch** (landings 1 and 2, 2026-09-12; [frame packet exchange](../../research/notes/frame-packet-exchange.html),
  gpu-status §2.16, §2.17). The game thread publishes a copy of the per-frame engine state after every
  in-play `DrawGameScreen` (only when the renderer has taken the previous one) and the render
  thread takes it once at the top of its frame; since landing 2 the copy IS the view every pass
  draws from (the true viewport, the eye, the rect the engine can name, the palette and gamma)
  and the render thread's camera writes travel the other way as a command record the game
  thread applies at the top of every in-play draw (the zoom level, the cursor anchor's eye
  delta, `tagpu_eye.txt`, the follow release, the widened rect, `ScrollSpeed`). So **with
  `packet.off` no world pass draws at all** (the engine's own terrain comes back once terrown's
  skip expires, ~90 frames), no command is applied (the engine keeps its own camera range, rect
  and scroll rate; `tagpu_eye.txt` and the wheel do nothing), and every string through
  `tagpu_text_place` draws nothing — the group digits, the `ShowRanges` labels and the FPS
  readout. Read `packet: ARMED 5 slots x 8 MB reserved … (unit 100 B, piece 24 B, wreck 40 B,
  anchor 16 B); commands: 4 slots x 64 KB …` — **five** frame slots since landing 3, because the
  consumer holds three (READ, PREV and a SPARE) so the give-back can be tick-aware and `prev`
  always carries a different sim tick from `read`; the command record's instance still holds
  two — and
  `packet: publisher ARMED on DrawGameScreen 0x468CF0 … level-end packet by tagpu_reclaim's
  teardown post hook …` at launch (`… by our own observer on the teardown 0x491B60` under
  `reclaim.off`), then
  every 300 frames `packet: pub=… skip=… overrun=… foreign=… acq=… taken=… gap=… grow=…
  commitfail=… trunc=… viol=… pviol=… crcbad=… nopkt=… | pub/s=… taken/s=… pubus p50=… p99=… |
  seq=… tick=… tps=… speed=… paused=… in_game=… gen=… flags=… eye=… vp=… addr=… z=… pal=… gamma=…
  flips=… font=… fg=… trunc=… used=… | cmd: post=… take=… new=… overrun=… viol=… nocmd=… seq=…
  ack=… unacked=(dx,dy) z=… live=… hold=… | draws=… inplay=… draws/s=… inplay/s=… foreign=…
  deep=… fontcopies=… levelend=reclaim|own|none vpapply=… vpwh=…`, with **two world segments
  since landing 3**: in the packet segment `units=… pieces=… wrecks=… anchors=<n>(<cols>x<rows>)
  dup=… pair=… same=…`, and at the very end `| world: u=… p=… w=… a=<anchors>/<cells scanned>
  scan=<scans>/<reuses> dup=… trunc=<units>/<pieces>/<wrecks>/<anchors> relbad=… woob=… shd=…`.
  **`dup` must read 0** — it is the stable-id collision oracle over one packet, and the pose blend
  matches units across two packets by that id — and **`relbad` must read 0**: it counts draws on
  which the engine's `end` pointer did not equal `begin + (count−1)·0x118`, the relation the exe
  note records. **`woob` should read 0 too**: it counts feature cells naming a wreck record outside
  the engine's own 2048-record pool, and it is a monitor rather than the guard — the guard is the
  bound itself, which refuses the read either way. `pair` is the frames that had a two-tick pair to blend over and `same` the
  rotations that displaced READ because the tick had not moved; `scan=` says how many publishes
  actually walked the feature grid (it is cached per tick, so at 300 published frames a second
  against a 60 Hz sim expect roughly one scan in five). `trunc` past the first fill of each slot
  is a fault; the first fill of each is the growth path doing its job. **`viol`, `pviol`,
  `crcbad`, `foreign` and `commitfail` must stay 0, on both exchanges, and so must `vpwh`**
  (in-play draws on which the viewport's W/H disagreed with the screen — the old race, now
  impossible by construction); `skip` is the FRESH gate doing its job (one
  relaxed load per engine draw), `overrun` and `gap` are 0 in play and count only under `stress`
  or across a level end (the forced out-of-game packet); `grow`/`trunc` say a slot grew past its
  first fill (once per slot under `stress`, never in play so far). In the `cmd:` segment `post`
  is one per render frame, `take` one per in-play draw, `new` the records that were actually new,
  `overrun` the posts nobody took (every one at the shell, a handful in play — not a fault),
  `unacked` must read `(0,0)` whenever no wheel gesture is in flight (how far the eye the render
  thread is drawing runs ahead of the last packet's), `cum=(x,y)` is the anchor's sum since the
  level began (it resets to (0,0) with `epoch=` at every level end), and `applyus p50= p99=`
  at the very end is the cost of the whole command apply per in-play draw (2 µs on the landing). `tps` is `GameTime` per wall
  second — **3 × `speed`**, 60 at the GameSpeed 20 a scenario lands on — and `pubus` the publish
  cost in µs. Levers, read at attach: `packet.check` (CRC-32 of every record, verified per take),
  `packet.stress` (publish on every draw with a garbage pre-fill, one-page slots that must grow,
  the render thread sleeping 0..50 ms per take — the protocol gate's mode, ~35 taken frames/s),
  `packet.poison` (the slot handed back is memset, so a pointer cached past its frame reads
  0xDD), `packet.show` (a `PK<seq> T<tick> E<eye> A<ack> D<dx>,<dy>` row under the FPS readout,
  needs `fps.on`).
  A level change logs `packet: level end -> gen N …`, then `packet: loader thread T entered
  0x497C70 …` / `… leaving 0x497C70 …` and `packet: level gen N: first in-play packet …` — in
  that order, which is the load hazard's closing argument (exe note, "The in-play publish point").

  **Since landing 4 (2026-09-12) the heartbeat carries two more segments**, and every counter in
  them must read 0 in a healthy game:

  - `| fx: proj= expl= deb= part=<this packet>/<high water> scan=<gathers>/<reuses> trunc=
    layerbad= subbad= lht= want=<fx>/<sfx>` — the effects and the ten particle layers.
    **`trunc`, `layerbad` and `subbad` must all read 0**: `layerbad` counts a particle layer whose
    object count exceeded the engine's own 401 (the emitters drop the front and shift past 400, so
    401 is the steady state, not 400), and `subbad` an object whose sub-particle vector exceeded
    the containment filter. `scan` is the gather's per-(level, tick) cache: at the publish rates
    the reference setup reaches against a 60 Hz sim, expect roughly one scan in five to twenty.
    `part=`'s high-water was **800** on a 1080p `200v200`, against a 16 384-entry cap.
    `want=` is the render thread's standing request, and **the pass does not claim the engine's
    draw until the packet says the publisher was filling for it** — so a freshly armed `fx.on`
    costs a frame or two of the engine's own effects rather than a frame or two of none.
  - `| fog: <cols>x<rows> wide=<cols>x<rows>/<publishes> refused= shade=` and, on the `gui:` half,
    `| gui: mm=<w>x<h>/<copies> refused= pic=<w>x<h>/<sent>` — both fog grids and the GL UI's
    minimap. **`refused` must read 0** on both.

  **The native pass's line gained `fog=<mode>(<cols>x<rows>) bare=<n>`.** `bare` is
  `tagpu_fogwide`'s old counter, moved here with the grid: frames drawn zoomed out (or from an eye
  the game thread has not acknowledged) whose packet carried no WIDE fog grid, so the outer ring
  fell back to the engine's 1× one. **It must read 0** unless `tagpu_fogwide.off` is armed.

  **`tagpu_fogwide`'s own line lost `bare=`, `ret=`, `held=` and `strand=`** — it hands nothing
  over any more, so there is nothing to retire and nothing to strand. It is
  `fogwide: CxR cells=N cap=CxR rebuilds=N in S = R/s ticks=N build=…/… us (mean/max)`.
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
  fog is on whenever the grid uploaded, and `los` is the engine's raw `LosType`. **`fog=0` means
  the grid was REFUSED, and that is no fog at all** — not black, not grey, the whole map lit at
  every zoom. It read 0 at every resolution whose grid cell count is not a multiple of 8
  (1920×1080 among them) until 2026-09-09.
- **The fog grid at zoom < 1 is OURS, not the engine's** (G13r): the engine's spans the 1× viewport
  only, so `tagpu_fogwide.c` builds the same masks over a window the whole zoom range fits in and
  the passes sample that instead. Levers: `tacli arm <i> fogwide.off` disables it live (the A/B —
  with it the outer ring goes back to a smear of the border cell), `fogwide_check.on` arms the
  oracle, which logs `fogwide check: … compared=N of cells=M differ=N` every 120th tick and
  **must read `differ=0`**. `compared` is `cols*rows` and `cells` the engine's ALLOCATION, which
  it rounds up to a multiple of 8 — comparing the tail reads entries nothing built.
  Its heartbeat is `fogwide: <cols>x<rows> cells=… cap=<C>x<R> rebuilds=N in 5.0s = R/s ticks=…
  build=…/… us (mean/max) bare=N ret=F/R held=N strand=N/NKB`, **one line per five seconds of wall
  time** (G13s; it used to be per 300
  ticks, which was incomparable between runs because a tick is a `DrawGameScreen` call and the game
  loop turns that over 330–4900 times a second depending on the scene while the presenter holds 60).
  **`bare=` must read 0**: it counts render frames that drew zoomed and were refused a wide grid,
  i.e. frames painted over the engine's 1× grid, and it read 1 per gesture before the fix. It does
  **not** count `tagpu_fogwide.off`, so the lever does not make the heartbeat cry wolf.
  **The module builds at every zoom since G13s** — the grid has to exist before the frame that
  eases past 1.0, so the tick no longer waits for the level — while the *picture* at zoom ≥ 1 is
  still the engine's grid, bit for bit, because the consumer is what gates on the level now. That
  costs **~30 rebuilds a second whenever anything is moving** (the rate is the sim tick's, not the
  camera's: every LOS stamp clears the engine's is-current bit) at ~145 µs, i.e. ~4.4 ms of
  game-thread time a second.
  **The grid is sized from the screen since 2026-09-10, not from a constant** — `cap=` is what the
  three buffers are allocated for and the `fogwide: grid CxR, N KB for the set` line says the cost
  once per size: **212 KB at 1920x1080, 84 KB at 1024x768**, where it used to be a flat 6144 KB in
  every session. `ret=freed/retired held=N/NKB` is the grow path — a video-mode change grows the set
  and hands the old three blocks to `tagpu_reclaim`'s quiescence fence, so **`held=` must fall
  back to 0** and **`strand=` must read 0**: stranding is what happens when the fence is unarmed
  (`tagpu_reclaim.off`, or an exe where the install failed) or the ring is full, and it is safe but
  it means memory is not coming back. A session that never changes resolution shows `ret=0/0`
  throughout — **the grow path does not run on a normal launch**, so testing it needs either a real
  game → shell → game cycle at a different resolution or a temporary probe that inflates
  `fogw_capacity`.
  **`rebuilds=0` is not a fault — check `LosType` before you chase it.** The rebuild fires on the
  engine's is-current bit, which the LOS stamps clear; at **`LosType 12` (permanent LOS,
  `--los 0`) nothing stamps**, so the rate is legitimately 0 however much is moving on screen.
  At 14 (true LOS) the same scene gives ~30/s. Read the word at `*0x511DE8 + 0x14281` with
  `tacli peek` rather than guessing — this cost a round of "is my change broken?" on 2026-09-11
  when the answer was that `scenario load` had been given `--los 0`.
  **One `bare=1` per video-mode change is expected** and is not the alarm the counter is for: the
  render thread is recreated across a mode switch while `fogwide`'s staleness statics survive, so
  the first frame after it reports one refusal. A second one, or any at all without a mode change,
  is the real signal.
- **A one-frame fog artifact is not findable with `glshot`.** Record the window losslessly
  (`ffmpeg -f x11grab -window_id <id> -framerate 60 -c:v libx264rgb -qp 0`) and scan every frame;
  the criterion that separates a fog failure from the grey band is **green dominance**
  (`g > r+20 && g > b+20 && g > 60`), because the band is a grey remap and lit grass is not.
- **Put the camera where you want it by SCROLLING, not with `tacli eye`.** The engine rebuilds its
  fog grid only when its own is-current bit is cleared, which a camera *move* does; `tacli eye`
  writes the eye and the scroll target together, so nothing clears it and the stale grid is drawn
  at the new position. The symptom is a lit LOS circle sitting over an enemy base you have never
  scouted, which looks exactly like a fog bug and is not. `keys <i> mouse:0,1079` and wait.
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
  `tacli peek <i> '*0x511DE8+0x391BF:4'`.
  **THIS RECIPE NEEDS A CONDITION THIS NOTE DOES NOT STATE, measured 2026-09-17.** On a
  `scenario load` fixture (`static-terrain`, `vk.on` + `terr.on` + `classicpp.on`) it does
  nothing: every token is delivered and parsed — `tagpu.log` carries
  `input: keys x char:+ char:s …` for all of them — and `tagpu_shield.c:362` translates
  `WM_TAGPU_CHAR` into a real `WM_CHAR` for the engine, yet **`main+0x391BF` stays 0**.
  `+gamma 13` behaves the same way (`main+0x37F08` stays at its startup value through both
  `+gamma13` and `+gamma 13`), so it is not one cheat being refused — it is the chat path.
  The likeliest cause, unproven: the Enter that OPENS the chat line has to reach the engine's
  own key handling, and TA takes game keys through DirectInput rather than from the window, so
  a posted `WM_KEYDOWN` never opens the line and the `WM_CHAR`s that follow have nowhere to go.
  **So do not use a typed cheat as a probe on a scenario fixture without peeking its address
  first** — the keystrokes will look like they landed. Whether the recipe works in a real
  skirmish is untested here; it is recorded as verified above and that is not withdrawn. With it on, a selected unit draws nine def-range
  circles and three weapon ones, each labelled.
- **Text is closed too, as of G13p.** The group digit and the `ShowRanges` labels are ours —
  TA's own glyphs, rasterised through `0x4CCF60` into an atlas of ours and drawn at a constant
  SCREEN size. `mark.on=nodigits` hands the digit back for an A/B (at 1× the two are
  pixel-identical; at 0.25× in the ring the engine's is absent), and `order.on=nolabels` hands
  back the labels.

### Making the marker pass actually DRAW — six levers, one per kind

`mark.on` arms the pass; it opens **none** of the buckets. `tagpu_mark_render` returns before it
draws anything while every bucket is empty, so a measurement fixture that only arms the pass
produces no picture and no capture — four automated A/B runs died there before this was written
down. Each draw kind has its own gate:

| kind | what makes it non-empty |
|---|---|
| health bars | a unit of the **watched** player on screen, `damagebars` on. No selection needed |
| group digits | `u->squad` non-zero — `tacli keys <i> ctrl+1` on a selection, and nothing else does it |
| order lines | `order.on` **and** SHIFT held **and** an order that does not complete |
| order triangles | the marching route dots — the engine draws them only for the **hovered** unit |
| labels | `+showranges`, typed into the chat (above) |
| cursors | a **held** drag, or a build placement |
| post-fog layer | `mark.on=nocursor` — the only window in normal play that still fills it |

Four of those are worth spelling out because each cost a run:

- **SHIFT must be physically held, not tapped.** The engine's order driver at `0x469BFC` is
  shift-gated, so the markers exist only while the key is down. `tacli keys <i> down:shift` holds
  it across frames; `up:shift` releases. A bare `shift` token is a 150 ms tap and will not survive
  to the capture.
- **A `move` order completes and takes its markers with it. Use `patrol`** — it never finishes, so
  the order node stays for the whole session.
- **The route dots need `flag == 1`, which means the HOVERED unit** (or the tracked one, or the
  camera's). Park the pointer on a unit with `pmove:x,y` from a fresh `tacli roster`. A unit under
  orders walks out from under the pointer between the two commands, so **drop the game speed
  first**: nine `tacli keys <i> minus` puts it at speed 1, where the gap does not matter.
- **The band box is a held drag**: `pmove:x0,y0`, `down:lbutton`, `pmove:x1,y1`, capture, then
  `up:lbutton`. Releasing before the capture leaves `s_ncurs` empty. The drag also takes the
  pointer off the unit, so the cursor bucket and the route dots cannot be in the same frame.

A band-box drag is also the reliable way to **select** several units for the bars and the digit:
`0 sel` in the `native:` log line is our own selection-box count and is 0 whenever `native.on` is
not armed, so it is not the signal. Ask the engine instead — `tacli order <i> move pos X Y --sel`
reports `N issued`, and N is the selection.

### Comparing the two LANES on screen — and why an A/B cannot do it

`tools/vk-ab.py` compares the GL capture against the Vulkan one, so it is blind to any error the
two share. A whole-frame Y flip is exactly that, and it survived eight landings because the GL
half of the capture was mirrored by the same rule (gpu-status §2.40). **When the question is "does
the Vulkan lane draw the same picture as the game", the screen is the oracle.**

Route D's window is a second top-level window over the game's. Find the pair and capture each:

```bash
DISPLAY=:0 xwininfo -root -tree | grep 1024x768
#   the TITLED window is the game ("… | tacli:<instance>")
#   its UNTITLED sibling at the same +X+Y is Route D's
DISPLAY=:0 import -window 0x… out.png
```

Four things will waste a run if you do not know them, and the first one wasted several:

- **ARM `vk.on` AT LAUNCH, NEVER AFTERWARDS.** The mirrors the lane samples are asked for on the
  30-frame arm poll, and a lane that comes up mid-session never gets the ones established at
  launch — it draws a partial frame and says nothing about why. Measured on `one-unit` with
  `native.on` alone, nothing else moved: **382** non-black pixels in Route D with `vk.on` armed at
  launch (the commander, matching that fixture's A/B exactly) against **89** armed late, the body
  simply missing. That reads as a rendering bug and is not one. The obvious reason to arm late —
  Route D covers the game window, so you want the game captured first — is the trap: use
  **`tacli glshot`** for the GL side instead, which reads the GL lane rather than an obscured
  window, and leave the lane armed throughout.
- **Stop every other instance first.** `park.sh` puts every window at the same coordinates, so
  "the untitled sibling at that position" can belong to a *different* instance. This produced a
  pair showing two unrelated game states and read as a rendering bug.
- **An obscured window's backing store is stale.** Route D covers the game completely, so
  `import -window` on the game returns whatever it last held — once, the pre-scenario frame, which
  measured as a 534 730-px difference that was really two different moments. Capture the **game
  first, before arming `vk.on` at all**, on a static fixture (parked units, pinned camera).
- **Diff against the mirror as well.** `PIL.ImageChops.difference(a, b)` and again against
  `b.transpose(FLIP_TOP_BOTTOM)`: if the mirrored one is the smaller, the lane is still flipped.
  Reading "lots of pixels differ" without that check tells you nothing about which way.

**One drawing Vulkan pass per capture.** The seam refuses with *"N A/B levers claimed this frame and
M passes drew into it"*. Two live consequences:

- **`feat` needs `native.on` to own its leaf**, and since gate 3a that makes the Vulkan **unit**
  pass draw too — so the feature A/B cannot be taken the way gate 2 took it. Pan the camera off
  every unit (`tacli eye <i> X Y`) until the `native:` line reads `0 unit(s)`, and `feat` is alone
  again.
- **`fx-lasers` does not reliably fire.** Two runs measured `fx: proj=0 laser=0` at the capture
  frame, so the GL half never reached the disk. Use **`fx-rockets`** — model projectiles stay in
  flight for minutes — and poll `fx: proj=` until it is non-zero before claiming the frame.

**Zoom has two levers, and the file wins.**

**`tagpu_zoom.txt` in the gamedir** — a bare float 0.25–8.0, re-read every frame; write it
**atomically** (temp + rename) or the DLL reads a torn value. This is the scripted lever, so
every scenario and every `tacli` recipe still drives zoom exactly as before.

**The mouse wheel** is what the player uses, and what the level falls back to whenever the
file is *absent*: one notch is ×1.1 geometric, clamped to the same 0.25–8.0, eased over
about six frames.

**The wheel ZOOMS TO THE CURSOR since G13t (2026-09-10), so it MOVES THE CAMERA.** The world
point under the pointer is held still, which means `tacli wheel --at X Y` is no longer a
camera-neutral operation: the eye steps by `(a − c)(1/z0 − 1/z1)`, where `c` is the viewport
centre. Four consequences for driving:

- **Re-read the eye after any wheel**, and do not assume a recipe's camera survived one.
- **`--at` the viewport centre is the old behaviour exactly** — the delta is 0 there, so that
  is the control for any A/B, and it needs no flag (there isn't one).
- **`tagpu_zoom.txt` still does NOT move the camera.** Only the wheel anchors, so every
  scripted zoom and every fixture is unchanged.
- **An off-centre wheel RELEASES a camera follow (G13u), and it needs the GAME THREAD to be
  DRAWING.** Ctrl+C follows your commander (it does not merely centre on it) and the
  cycle-through-units keys do the same; since the frame packet's landing 2 the wheel's eye delta
  travels as a command the game thread applies at the top of its next in-play `DrawGameScreen`,
  and that apply zeroes the three follow slots on the draw it steps the eye. Two consequences for
  driving: a recipe that sets up a follow and then wheels has no follow afterwards — read
  `main+0x142F3` (`CameraToUnit`, 0 = nothing followed) rather than assuming, and expect one
  `zoom: cursor anchor took the camera - the unit follow is released` per follow in the log.
  **Pausing the sim (`tab`) does NOT stop this** — the apply runs on the in-play draw, not the
  sim tick (measured on the landing with the second game paused at tick 0: −4 notches at
  (900,600) moved the eye by exactly (58,−28) and zeroed the slot). What does stop it
  is the game thread ceasing to DRAW, which is the fail-safe direction and not a state you meet
  while testing. A wheel aimed at the viewport centre moves the eye by nothing and leaves the
  follow alone, which is the control. **Read the eye AFTER the game thread has applied** — one
  present later; a `peek` fired in the same instant as the wheel can read the old eye — and note
  that the picture moves on the very frame of the notch (the render thread draws from the
  packet's eye plus the unacknowledged delta) while the engine's field follows within a draw.
- **`Ctrl+C` needs the SHIELD ON.** It is a modifier combo, so under injection it only reaches
  the game through `fake_GetAsyncKeyState` — with `--no-shield` your `ctrl` is invisible and
  the follow is never established, which looks exactly like the feature not working.

Anchoring is off — and says so once a second in the log — while `tagpu_eye.txt` holds the
camera (`zoom: cursor anchor off - tagpu_eye.txt holds the camera`). The old second gate, that
`terrown` had to be skipping so the fog rebuild was ours to ask for, is gone since the frame
packet's landing 2: the game thread's own apply invalidates the fog grid on the draw it moves the
eye, whoever draws the fog. **`scenario load` pins the camera**, so `tacli eye <i> --release`
first or the wheel will zoom to the centre and the log will tell you why. The hold itself is a
command too: `tacli eye <i> X Y` is applied at the top of every in-play draw, clamped into the
camera's range (a hold past the map edge lands on `map − view`), and the packet heartbeat's
`hold=1` says it is in force. It is live **only while our zoomed world is actually on screen and the
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
- **An engine dialog over the world at zoom ≠ 1 does not take `ui click`.** The exit menu, the
  surrender confirm and any other stock screen drawn inside the world viewport is hit-tested by
  the engine in screen space, but a click there goes through the zoom transform like a world
  click (only our own render-options panel is exempt), so at 0.683 `ui click MAINMENU` lands
  ~20 px off and is "delivered (no GUI-visible change)". Use `ui press <gadget>` (the gadget's
  own quickkey) or wheel back to 1.0 first. Found 2026-09-12 driving a level cycle after a
  wheel; a pre-existing limit of the transform, not of the packet.
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
**under `tacli keys`** — TA's scroll hotkeys are its own ids `0xF4`/`0xF5`/`0xF6`/`0xF7` and our
injection posts VK arrows, which those ids are not. *[CORRECTED 2026-09-09. This said "arrow keys
do not scroll" flat, and it was read back to the owner as a statement about the GAME: they were
sitting at a handed-over instance whose arrows would not pan, and this line agreed with them that
that was normal. It is not — on a real keyboard the arrows scroll TA perfectly well, and the
actual cause was that the window they were typing into belonged to another session's instance
entirely. A measurement made under injection is a fact about the injection until it has been
checked with the shield off.]* A *held* left button on the minimap does jump the camera, and
lands exactly on `world − (W/2, H/2)` before the clamp:

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
  such limit either way. **A band-box drag is a click and goes the same way**: with
  `vpwide.off` armed, a drag across the whole viewport at 0.42× selected **0** units where
  the same drag with vpwide selects ~470 [MEASURED 2026-09-09] — so an A/B that turns
  vpwide off has to make its selection at 1× first, or it is comparing against an empty one.
- **In-game dialogs drawn inside the viewport keep 1:1 clicks at every zoom since
  the modal-input fix (2026-09-14).** `ARMOPT`, `EXITMENU`, `YESORNO` and the
  preferences screens leave the zoomed world drawing underneath them, so the
  published world view stays live. The shared s -> u transform now reads the
  engine ownership bit at `main+0x37EBE`: while bit 0 is set, button positions
  remain in screen space for both hardware and injected input. Measured through
  F2 at 0.25x and 8x: `EXIT`, `MAINMENU`, `EXITGAME`, both confirmation choices
  and `OK` all land on the engine-reported gadget; the bit clears on Resume and
  world input resumes. The pre-fix 0.25x click at `(577,336)` returned to
  `ARMMAIN2` or did nothing instead of raising `YESORNO`. This gate is specific
  to that stack: `SHARE.GUI` sets bit 6 of the same word and its zoomed clicks
  remain a known gap.

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

- **`ErrorLog.txt` is rotated to `.prev` at every launch since 2026-09-10**, so a report in it
  belongs to the current run. Before that it was appended to forever and `tacli crash` reported
  the FIRST (oldest) entry with no timestamp — a 20-minute-old fault read as five consecutive
  fresh crashes on 2026-09-09. It is also per-instance by construction now (TA writes it beside
  the exe, and `mirror_gamedir` no longer symlinks it through from the template; instances built
  on or before 2026-09-03 carry a dangling symlink that the launch rotation unlinks). **It lives
  in the MAIN CHECKOUT** at `tagpu/instances/<inst>/gamedir/`, never under a worktree — a relative
  path from a worktree deletes nothing, silently.
- **`mark.on=noselbox` is the forcing lever for anything the engine draws inside the viewport.**
  It sets `g_selbox = 0`, so `markown` never suppresses the engine's selection rects and the
  engine draws every one of them, every frame, at the **unzoomed** position. That turns a
  fraction-of-a-percent artifact into a deterministic one — it is how the cyan-square bug
  (`gui-renderer.md` §20) went from 15 hits in 3600 frames to 12 of 12. Confirm it took with
  `tacli log <i> -g 'markown: engine selection'` → `restored`; the `mark: ARMED (… selbox=…)`
  line is written only when the arm state changes and is stale otherwise. **Remove it afterwards**
  — a player seeing hundreds of green boxes scattered over the map is this lever, not a bug.
- **A 1-frame artifact is not findable with `glshot`** (~1 sample/s against 60 fps). Record the
  window losslessly instead and scan every frame:
  `ffmpeg -f x11grab -window_id <id> -framerate 60 -c:v libx264rgb -qp 0 out.mkv`. Use
  `-window_id`, not `:0+x,y` — a screen-region grab captures whatever is on top, which on a shared
  desktop is usually a browser.
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
to main's (parity md5 measured equal, §10 — but see the note below: "trigger absent" changed
meaning in landing 10c-2 and that row has not been re-measured since).
**`gui.on` is part of the default arm set** now.

**Everything `tacli` reads now comes from the GAME thread, so every verb works on every renderer
including `renderer=gdi`** (the vulkan-only plan's gate 10c, three landings). The triggers and the
key/click injection run from the engine's flip; the live-state log — `units:`, the roster dump and
`mouse:`, which with `peek:` are the whole of what `tacli` greps out of `tagpu.log` — is written
by `tagpu_packet_pub.c` beside the frame packet. What still does NOT reach gdi: `tacli eye` and
`tacli wheel`, which both need the overlay frame that only the GL and Vulkan backends run;
`tacli gui`, which writes a lever with no effect on that lane; and **both capture verbs**.

**`tacli shot` AND `tacli glshot` REACH THE OPENGL LANE ONLY — measured on both gdi and
Vulkan, and this paragraph claimed otherwise for one commit.** It is not about which surface
each one reads; it is about who polls the trigger. `tagpu_shot.trigger` is consumed at exactly
one site, in `render_ogl.c`, and `tagpu_glshot.trigger` in that file and in `tagpu_scaffold.c`,
itself a GL pass. Neither `render_gdi.c` nor `render_vk.c` polls either file, so on those lanes
the trigger is written and never read, and the verb times out with *"no surface screenshot
appeared"* — for `shot` exactly as for `glshot`. Pictures of the gdi or Vulkan lane come from
the live display (see the capture skill), not from these verbs. Rewiring them is on landing 11,
which deletes `render_ogl.c`: whatever polls the two triggers afterwards has to be code every
lane reaches.

Note too that the `packet:` heartbeat is emitted from the render thread, so the exchange's
counters are not printed on gdi at all.

**`gui.on` DOES NOT GATE `tacli` ITSELF, and for one commit in the vulkan-only plan's landing 10c
it did** — which is worth knowing because the failure was silent. The flip `0x4C63A0` is where the
whole on-demand trigger family runs since that landing (peek, `ui`, the catalogues, scenario
detection, and the key/click injection), so `tagpu_gui_hook.c` installs its observer of the flip
**whenever the engine's bytes match**, and `tagpu_gui.on` gates only the layer, the census and the
17 leaf detours. `tacli gui <i> remove`, `gui.off` and a bare `launch` (which writes
`tagpu_defaults.off`, so no default applies) therefore leave the instance fully drivable.

**The diagnostic is a phrase, not the absence of a line.** `tagpu_gui_init` has **six** exits —
five `return`s and the fall-through — and every one of them logs. Only the first two mean the
instance cannot be driven, and both say so in those words:

| boot line in `tagpu.log` | layer | drivable |
|---|---|---|
| `gui: NOT armed — engine bytes differ at the flip 0x4C63A0 … no tacli verb can answer` | no | **NO** |
| `gui: NOT armed — the flip observer refused to install … no tacli verb can answer` | no | **NO** |
| `gui: trigger host only (tagpu_gui.on is not on) …` | no | yes |
| `gui: UI layer NOT armed — engine bytes differ at a watched leaf …` | no | yes |
| `gui: UI layer NOT armed — no arena …` | no | yes |
| `gui: ARMED flip@0x4C63A0=1 leaves=17/17 …` | yes | yes |
| `gui: FAILED flip@0x4C63A0=1 leaves=n/17 …` (a partial leaf install) | no — `s_installed` is 0, so the layer never draws | yes |

So: **grep for `no tacli verb can answer`.** [An earlier draft of this section said "if neither
line is in `tagpu.log`, no `tacli` verb can answer", naming only the first and last rows — which
would have had an agent abandon a perfectly drivable instance on either of the two middle rows,
the exact case the landing exists to protect.]

**And `tacli gui <i> remove` does not always remove the layer.** It only unlinks `tagpu_gui.on`,
and `tagpu_gui.on` is in the defaults table — so on an instance launched with `--defaults` the
default re-applies and the layer stays fully armed. `gui.off` and a bare launch (which writes
`tagpu_defaults.off`) are what actually take it away. `tacli gui`'s status string used to call the
missing file "absent (module not armed)", which had the same error in it; it now says "absent (the
DLL's default applies unless the instance has `tagpu_defaults.off`)".

```bash
tools/tacli gui <i> on            # arm BEFORE launch (the detours install at DLL attach); the draw follows the file live
tools/tacli gui <i> off           # keep the detours, stop the draw — the live A/B, 500 ms poll
tools/tacli gui <i> strict        # the harness's mode: fallback off, a miss painted magenta (never for a player)
tools/tacli gui <i> remove        # un-arm the LAYER at the next launch (see below)
tools/tacli arm <i> gui.on=norestore   # G15e: the layer WITHOUT Classic++ art — the UI-only A/B
tools/tacli arm <i> 'gui.on=sharptest log'   # G17a: the sharp layer filled with a known pattern
tools/tacli arm <i> 'mark.on=noselbox'      # the engine draws its OWN selection box again
#   FORCING A DIAGONAL UI LINE (landing 8c) takes THREE things at once, and any two give zero:
#   mark.on=noselbox, a unit SELECTED, and an ORIENTATION OFF THE AXIS -- so order a diagonal move
#   first and re-select after it arrives. Read the result off `GUI kinds:` as `line` vs `diag`.
#   NOT "a heading off a multiple of 90": the engine hands 0x4B6CC0 all THREE angles (bank,
#   heading, pitch at u+0x64, see tagpu_native.c:3815), so ON A SLOPE a heading of 0 or 90 still
#   gives a rotated square. Flat ground is what makes heading alone predictive.
#   And diagonals are NOT confined to mark.on=noselbox: markown suppresses PER UNIT and only while
#   tagpu_native_selbox_complete(), so any frame where the native pass comes up short hands every
#   box back to the engine (MEASURED 2026-09-09: ~460 of them).
tools/tacli gui <i>               # report
tools/tacli log <i> -g 'gui: twins='   # heartbeat per 300 frames: twins= seeds= sprites= pixels= bars= rects= atlas= resets= overflows= k= sharp= fps=
#   bars= (8a) and rects= (8b) count PK_BAR/PK_RECT replayed as GEOMETRY; pixels= counts boxes of arena
#   bytes. Read them against each other: every bar used to be a pixels. Neither is a per-frame
#   number -- both are running totals at the moment the line was printed.
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
- **When the walk is a REGRESSION gate, run the identical walk on the previous DLL and diff the
  two `report.md` tables column by column** — it is ~25 minutes a pass at 1024x768 and it is the
  only thing that tells a finding from a fixture. Measured 2026-09-12 on the frame packet's
  landing 4: of the two non-zero columns, `MISS=1 [FPS] drift=23px` on `VISUALS.GUI` reproduced
  exactly on the older DLL (pre-existing, `uiwalk`'s hit-test inverse on a row `tagpu_menu.c`
  emits) and `strict holes = 4` did not (real). Ignore `fps`, `atlas`, `twins`, `skipped`,
  `resets` and `stalls` in that diff — they are run-dependent and differ everywhere.
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

### The cursor is ours (phase 2, G17c)

```bash
tools/tacli arm <i> gui.on=nocursor          # phase 1's cursor, the engine's own — the A/B
tools/tacli arm <i> 'gui.on=cursorscale=2'   # ours, at 2 device px per art px (default 1, clamped 0.25-8)
tools/tacli keys <i> "dmove:768,576"         # move the pointer in CLIENT pixels, no click
tools/tacli log <i> -g 'curs='               # curs=<own>,<w>x<h>,dev=<1 if the client point>,sc=,drawn=,warm=
```

- **`dmove:` is how you place the cursor without clicking**, and it is client-area pixels. A
  logical `pmove:` (what `ui hover` sends) works too, but it makes `dev=0`: an injected logical
  point has no pointer behind it, so the draw falls back to the engine's own position.
- **The measure is the cursor's device FOOTPRINT, and it needs no reference image.** Park the
  pointer far away, `glshot`, move it to a known client point, `glshot`, and take the bounding box
  of the pixels that changed. Ours is **10x20 at every k** (one device pixel per art pixel); the
  engine's is that art nearest-blown-up — 15x30 at k = 1.5, 30x60 at k = 3. **The box size is also
  how you tell one cursor from two**: if the engine's were still underneath, the changed box would
  be the union, i.e. the bigger one.
- **`warm=` counts frames spent atlasing a shape for the first time**, and one per new shape is
  correct — ownership latches on the atlas so the erase never runs ahead of the draw. A `warm=`
  that keeps climbing means the atlas is refusing the frame.
- **The cursor rect is excluded from `uiwalk`'s diff** (padded 8 px) and exempt from `strict`
  either way, so neither is a test of the cursor. The footprint above is.
- **But the `strict holes` column is NOT masked by that rect** — `holes = mag.sum()` counts every
  magenta pixel in the frame — and since the frame packet's landing 4c the exempt rect comes from
  the packet, one `DrawGameScreen` behind the blit. **The engine's cursors pulse** (the move cursor
  cycles 27x27, 29x29, 31x31, 33x33, 35x35, one pixel per side per step — which is why `uiwalk`
  pads by 8 in the first place), so where the next step is larger its outer ring reads as holes: 4
  px at two in-game stops, measured 2026-09-12. **A handful of holes at a stop where the pointer
  sits over the world is that, not a lost op.** Read the `curs=` sizes in `tagpu.log` before
  chasing one.

### Text is a string op (phase 2, G17d)

```bash
tools/tacli arm <i> gui.on=nostring     # BEFORE the launch: text stays a box of captured pixels
tools/tacli log <i> -g 'str='           # str=<ops>/<glyph quads>,miss=,reseed=,glyphs=<cached>/<drops>,fonts=
tools/tacli log <i> -g 'arena='         # the queue's MONOTONIC arena head: the delta over 300 frames
```

- **`norepaint` turns off the forced redraw** (landing 9, 2026-09-18). Shipped behaviour is to call
  the engine's own `GUI_StageUpdateDraw 0x4A81E0(gi, 0x40)` on the **top** screen at the flip's
  return whenever the layer arms or the twins are reseeded, so the shell's art arrives as sprite
  ops instead of only as a `PK_SEED` of flat bytes. `log` prints one line per repaint with its op
  count **by kind** — `gui: repaint #N … 115 op(s) [gaf 105 line 4 focus 6]` — and the heartbeat
  carries `repaints=<done>/<refused> rops=<last>` — `rops=` and not a second `ops=`, which is
  what it was until the review pointed out the heartbeat then had two different keys spelled the
  same. `<refused>` counts EPISODES, not flips: a guard failure leaves the repaint pending, so
  counting every refusal counted ~5000 a second in the shell. Two things to know before reading those numbers:
  **in game it is 1 op**, because `ARMMAIN2.GUI` is a three-label screen and the HUD is not a
  gadget tree, so a repaint is a **shell** measurement; and the number that moves is the **GUI
  atlas**, `gui: atlas mirror armed, … N painted frame(s)` — **28 shipped against 19 under
  `norepaint`**, reproduced three boots each.
- **`nostring`, and every other token the HOOK owns (`census`, `log`, `pgm`, `trace`,
  `norepaint`), is read at
  ATTACH.** `read_tokens()` runs once, from `tagpu_gui_init`. Arming any of them on a running
  instance silently does nothing — only the surf module's tokens (`strict`, `norestore`,
  `sharptest`, `nocursor`, `cursorscale=`) follow the file live. One A/B was lost to this.
- **`miss=` and `reseed=` must stay 0.** `miss` counts glyphs the cache refused that the engine
  would have drawn; `reseed` counts strings that stamped nothing and asked for a fresh seed. Two
  full 120-stop walks produced 0 of each over ~20 000 string ops.
- **`glyphs=` climbing is NOT `str=` climbing** [2026-09-16]. The glyph records are installed in
  the drain loop *above* the skip gate and unconditionally; the stamping happens in `case
  PK_STRING` and needs a twin for that surface. A skirmish left to sit has read `glyphs=16,
  fonts=1` with `str=0/0` and `miss=reseed=0` — the ops arrived and nothing drew them. Read
  `str=` for "text was drawn", `glyphs=`/`fonts=` for "the records arrived". The `--vk` walk is
  the fixture that makes `str=` climb (6 281–6 532 ops over a walk); a string figure from a
  fixture whose `str=` is flat says nothing.
- **A static in-game frame publishes its text ONCE** — `str=` freezes at ~22 ops on the parity
  fixture, because the panel's labels are drawn once and then deduped. To measure anything about
  text, turn the clock on (`+clock` in chat) or open a screen: then it is ~1 000 ops per 300
  frames.
- **The arena A/B needs a redrawing fixture and two launches**: 3 606 998 bytes per 300 frames
  with text as pixel ops against 2 035 029 with the string op.
- The 120-stop `strict` walk is the real oracle here — it diffs our frame against the engine's own
  surface, so a glyph off by one shows as `differing`/`vpdiff`.

### The minimap is ours at k > 1 (phase 2, G17e)

```bash
tools/tacli arm <i> gui.on=nominimap    # the engine's minimap back (live, the surf module polls it)
tools/tacli arm <i> gui.on=mmbase       # ours forced on at k = 1 too, where it is otherwise OFF
tools/tacli log <i> -g 'mm='            # mm=<draws>,fog=<hidden>/<texels>,noeng=<frames the surfaces would not read>
tools/tacli scenario load <i> <scn> --mapping 0    # THE fixture: an unmapped game, so there IS fog
```

- **At `k = 1` it is the engine's, deliberately** — the box is 106x126 *device* pixels there, so
  our 252-px source is thrown away and ours counts 30 distinct colours against the engine's 36.
  `mmbase` forces it on for the A/B; nothing else does.
- **`fog=0/13356` means the fixture tests NOTHING.** A mapped skirmish hides nothing, so the mask
  is inert and a clean-looking diff proves only that. `--mapping 0` gives `fog=13301/13356`.
- **The safety property is a bound you can check**: our base is used only where the engine's
  fogged and unfogged bases agree across 3x3, so the pixels that may differ from the engine's are
  at most the unfogged texels times `k²`. Measured: 2 of 13 356 on a 99.6 % fogged map, both
  inside the engine's own lit region.
- **Measure sharpness by DISTINCT COLOURS in the box, not by replication** — replication inverts
  here (59.2 % for ours against 50.4 % for the engine's, because the engine's ramp perturbs every
  pixel of a poorer source while palette-exact regions are flat). Colours: 2084 vs 532 at k = 1.5.
- The dots, arcs and points are the engine's own pixels, not a replay: `+0x142DB` differs from
  `+0x142DF` exactly where one landed.

### Driving `renderer=vulkan` (the vulkan-only plan, landing 4a onward)

**Since landing 4a the lane is also a RENDERER BACKEND, and then the lever below does not arm
it — the renderer choice does.** The two configurations are different runs and one build answers
both:

```bash
# the vulkan-only backend: the surface goes on the GAME window, no route D window
sed -i 's/^renderer=.*/renderer=vulkan/' tagpu/instances/<i>/gamedir/ddraw.ini
tools/tacli launch <i>                        # BARE: a bare launch does not rewrite ddraw.ini
tools/tacli log <i> -g '^vk:'                 # expect "route E: the surface is on the game window"

# the control, same build: GL presents and route D's window comes back
sed -i 's/^renderer=.*/renderer=openglcore/' tagpu/instances/<i>/gamedir/ddraw.ini
tools/tacli arm <i> vk.on
```

Four things that cost a session if they are not known:

- **`tacli glshot` CANNOT WORK under `renderer=vulkan`**, and it does not fail loudly. It reads a
  GL framebuffer through `tagpu_overlay_capture_*`, and there is no GL context in the process on
  that path. **The oracle is an X grab by window id** — `import -window <id>` on the id `tacli
  launch` prints — and `tacli shot` still reads the engine's own surface as always. Keep `glshot`
  for the `renderer=openglcore` control, where it is what proves the GL picture is still whole
  (a healthy shell frame reads ~148 distinct colours; one colour means GL drew nothing).
- **`tagpu_vk.on` is not needed and `tagpu_vk.off` no longer helps.** The renderer choice arms the
  lane, and disarming it would leave a black window rather than a GL fallback, so it cannot. The
  ON file is still READ for its `color=`, which is how the clear colour is changed for a grab.
- **A LAUNCH THAT REWRITES `ddraw.ini` SILENTLY PUTS YOU BACK ON GL.** `write_ddraw_ini`
  hard-codes `renderer=openglcore` (`tools/tacli:546`), and the launch path rewrites the file
  when `--maxfps`/`--res`/`--window` is passed **and also when the recorded tile is
  off-screen** — the self-heal at `tools/tacli:1116`, whose whole purpose is never to open a
  window somewhere `glshot` cannot read. So a tile `tile_is_onscreen` rejects turns a
  `renderer=vulkan` run into a GL run, and **nothing says so** except the absence of `vk:` lines.
  A bare launch is what preserves `renderer=`. After every launch meant to be Vulkan, check both:
  `tools/tacli log <i> -g '^vk:'` **and** the ini's own `renderer=` line.
- **The ini's `posX`/`posY` place the window, not `instance.json`'s `tile`** — so when a fresh
  instance records a tile on the human's primary, edit `posX`/`posY` in the ini together with
  `renderer=`, in one pass. (Measured 2026-09-17: the fork then placed the window at 14,198
  regardless, under BOTH renderers — so verify with `xdotool getwindowgeometry` rather than
  trusting either file. The check is the same one for the GL path.)
- **A window that is 100 % one colour is the right answer for landing 4a and the wrong one after
  4b.** 4a calls no gather half, so no pass has a hand-over. Read §2.48's table before calling a
  flat window a fault. **Since 4b-1 (§2.49) `tagpu_scaffold.on` and `tagpu_fps.on` DO reach the
  screen on this lane** — an armed instance showing only the clear colour is a fault now. The
  world and the UI layer are still 4b-2 and still stand down whole.

> **GONE SINCE LANDING 4d-1 (2026-09-18): the two-lane A/B no longer exists.** Route D — the
> Vulkan lane presenting into a popup of its own beside the GL backend — was deleted, along with
> `render_ogl.c`'s call to `tagpu_vk_frame`. `renderer=openglcore` + `tagpu_vk.on` now brings up no
> Vulkan lane at all, so there is no second half to capture and every recipe below is **history**.
> It is kept because the figures are the record of what was proved, and because the cross-BUILD
> capture that replaces it still uses `tagpu_vk_shot.c` and the same eight `.ab` levers — arm one,
> get a PPM of the Vulkan frame, diff it against one taken from an earlier build. What you cannot
> do any more is diff the two backends against each other.
>
> **HOW THE SURVIVING CAPTURE IS TAKEN, measured 2026-09-18 on the 4d-2 build.** Launch with
> `renderer=vulkan`; arm **one** pass and nothing else, because the seam still refuses a frame that
> more than one pass drew into (*"N A/B levers claimed this frame and M passes drew into it"* — the
> first attempt at this check armed the full play set and got no file, which looks exactly like a
> broken build and is not); `rm` the `.ab` and the `.ppm`, let the fixture settle, then `touch` the
> `.ab`. The lane logs `vk: shot: wrote tagpu_<tag>_vk.ppm, 2048x1536` and the file is
> `gw*ss × gh*ss`. Then `tools/vk-ab.py <old.ppm> <new.ppm>` — the two-FILE form. **The
> `--pass <tag> <gamedir>` form no longer works**: it looks for a `_gl.ppm` that nothing writes.

**The per-pass A/Bs on the vulkan-only lane, as of landing 4c-3.** All five world passes and the
UI layer draw there now, and the **five world** ones run at any `ss` — their ink counts below are
`ss=1` figures, so at the shipped `ss=2` expect four times as many out of 3 145 728 rather than
786 432. **The UI-layer row does not scale**: it is a 640x480 shell capture of the window, which
`ss` does not size. What each needs:

| pass | fixture | arm set, and the trap |
|---|---|---|
| terrain | `feat-forest` | `terr.on` — 630 719 ink px |
| features | `feat-forest` + `tacli eye <i> 1400 1600` | `'native.on=nosuchunit wrecks' feat.on` — the camera puts the map's one 3D wreck off screen, or the unit twin draws too and the lane refuses the frame. 132 274 ink px |
| effects | `fx-lasers` | `'native.on=nosuchunit' fx.on`, and arm the `.ab` ~4 s after the apply. Two runs can NEVER agree — it draws transient projectiles |
| markers | `selbox-facings` | `'native.on=nosuchunit' 'mark.on=log'`, then `tacli click <i> 312 362` to select a unit so the pass has content (`mark: bars=3`). 297 ink px |
| units | `selbox-facings` | `native.on=all` and **`mark.on` OFF**, or two passes draw. 2 125 ink px |
| UI layer | **the shell**, or `feat-forest` | `'tagpu_gui.on=mmbase'` and **`native.on` OFF**. Shell = 307 200 ink px, whole frame. In game only the CHROME is comparable — see below |

**The UI layer's A/B has four traps of its own, all paid for in the 4b-3 landing.**

- **`tagpu_gui.on` must exist BEFORE the launch, and `tacli arm` adds the `tagpu_` prefix
  itself.** `tagpu_gui_init` runs from the attach path and its first line is
  `if (!read_tokens()) return;` — no lever file, no producer hooks, so `tagpu_gui_installed()`
  stays false, `tagpu_gui_present` returns at its first line and there is no heartbeat at all.
  A lever armed into a running game cannot undo that. The spelling is **`tacli arm <i>
  gui.on=mmbase`** (`tools/tacli:1474` builds `tagpu_{name}`): passing `tagpu_gui.on` produces
  `tagpu_tagpu_gui.on`, which `tacli` reports as armed and nothing ever reads. `echo mmbase >
  $G/tagpu_gui.on` before the launch does the same job with no spelling to get wrong.
- **`native.on` OFF, or nothing is captured.** The unit pass and the UI pass both draw into the
  Vulkan frame and the seam refuses with *"1 A/B levers claimed this frame and 2 passes drew into
  it"*. `tacli launch` will also drop `owndraw.on` by itself when `native.on` is unset, and say so.
- **The shell is the fixture for a WHOLE-FRAME comparison.** In game the UI layer composites over
  the engine's live primary, so with `native.on` off the world inside the viewport is whatever
  moment that run reached — two runs are two moments and ~50 % of the viewport differs for
  reasons that have nothing to do with the pass. Outside the viewport (the panel, the top bar,
  the side bar) it is 0 px of 155 648. Compare the regions separately, or use the shell.
- **`mmbase`, or `sharp_minimap` never runs.** At k = 1 the engine's own minimap stands by design
  (§13.6), so the default path returns early on both lanes and the pass is not under test. With
  `mmbase` the heartbeat's `mm=` climbs and `fog=` reports the fogged texel count.

- **Read the lane's own periodic lines before believing a stand-down.** Four exist and all carry
  the driver's frame number, so they line up: `native: vulkan lane handed over frame N: …`,
  `vk: census: frame N: N drew, M claimed …`, `posedraw: nothing to hand over for frame N - …`,
  and `vk: unit: frame N: the hand-over carries nunit= …`. `vk: census` is the first thing to
  read when a capture does not appear: it says whether a pass drew at all.
- **The shell walk is flaky when the machine is busy.** `scenario apply` timed out twice waiting
  for a map that had not finished loading. Wait on the condition rather than the clock:
  `until tools/tacli roster <i> | grep -q '^u1'; do sleep 3; done`.

**THE ONE SCENE THAT REPRODUCES ACROSS BOOTS, and the two things that ruin a paired A/B**
[MEASURED 2026-09-18, landing 10b]. Cross-boot comparison is what a build-vs-build A/B needs and
most fixtures cannot give it. `scenario apply exit-sort` under `renderer=vulkan` does: same camera
to the digit, **1 535 colours**, and **0–1 differing pixels** between boots AND between builds —
but only over the frame **below y = 130**, and only if you ignore colour counts.

* **The message band lies.** `exit-sort` clears units to place its own, every death writes a line
  to the log, and TA picks the wording at random — *"vermin have been exterminated"* against
  *"forces have been obliterated"*. Two boots therefore differ by **~5 200 px of text** with a
  pixel-identical world underneath. Any fixture with `clear_existing` has this. Crop it off.
* **A colour count is not an oracle across boots.** The same build, the same fixture and the same
  camera gave **2 727** colours on one boot and **498** on another, because the map had revealed
  more by the time of the capture; a bare in-game screen gave 680 where an earlier run recorded
  709. Use it to tell a game from a lever colour, never to compare two runs.
* **And `shadow-struct` cannot carry a cross-boot A/B at all** — two boots differ in fog-of-war
  reveal and starting resources, hundreds of thousands of pixels.

**NO `tacli` VERB WORKS UNDER `renderer=gdi`** [MEASURED 2026-09-18]. `tacli ui` answers *"no UI
snapshot appeared"*, and it is not a timing problem: the whole on-demand trigger family —
`tagpu_ui_frame`, `tagpu_peek_frame`, `tagpu_cat_frame`, `tagpu_weapons_frame`, the scenario
applier's detection half — is called from `tagpu_overlay_draw` and nowhere else, and that has
exactly two callers, `render_ogl.c:1632` and `render_vk.c:232`. So on the gdi lane the shell
cannot be driven past the main menu, no scenario can be applied and nothing can be peeked. Do not
spend time on it; it is landing 10c of the vulkan-only plan.

**Getting a LIVE WORLD under `renderer=vulkan`, which `scenario load` cannot do for you.**
`scenario load --restart` goes through the launch path with a resolution, so it rewrites
`ddraw.ini` — `renderer=openglcore`, and `posX`/`posY` from a freshly computed tile. Both of those
are silent. So the world is reached by hand, and the whole recipe is four steps:

```bash
G=<main checkout>/tagpu/instances/<i>/gamedir
tools/tacli stop <i>
tools/tacli arm <i> mark.on <the pass under test>      # levers that hook at DLL attach go first
sed -i -e 's/^renderer=.*/renderer=vulkan/' -e 's/^posX=.*/posX=4936/' -e 's/^posY=.*/posY=16/' $G/ddraw.ini
tools/tacli launch <i>                                 # BARE: the only launch that preserves the ini
tools/tacli ui <i> click SINGLE; tools/tacli ui <i> click Skirmish
tools/tacli ui <i> click Start --no-wait; sleep 25     # `Start` tears the shell down: no gadget to settle on
tools/tacli scenario apply <i> feat-forest             # `apply` stacks onto a live game; `load` would relaunch
```

- **`tacli ui` works with nothing on the screen**, which is what makes this possible at all: it
  reads the engine's own gadget tree and injects a token, so the menus are driven normally while
  the window shows the lane's clear colour. The skirmish screen also *remembers the last map* in
  the engine's own settings, so `Two Continents` is still selected from the previous `load` — check
  it in the `ui` snapshot's `text:` line rather than assuming, because a scenario's coordinates are
  map-specific.
- **`scenario apply` reproduces the camera exactly.** Three runs of the same fixture logged
  `camera: [2950, 1010] (eye [2566, 616])` to the digit, which is what makes a cross-configuration
  A/B of a world pass meaningful at all.
- **The window can land on the human's monitor after any launch that rewrote the ini.** Check it —
  `tacli ls --json` carries `[id, x, y, w, h]` in `window` — and move it with `xdotool windowmove
  <id> <x> <y>` on the instance's OWN id. That touches no pointer and steals no focus, and the A/B
  does not care where the window is: the GL half reads an FBO we own and the Vulkan half copies a
  swapchain image, so neither is subject to the ownership test.

### The Vulkan lane and the GPU row (Phase G, `tagpu_vk.on`)

Since G19a a second backend can present the frame. It is **off unless armed** — GL stays the
default through Phase G. At G19a it drew a solid colour and nothing else; **since G19d it also
draws the frame-rate readout**, so an armed instance with `tagpu_fps.on` shows a magenta window
with `FPS<n>` in its top-left corner and no game. That is the lane working, not a fault.

```bash
tools/tacli arm <i> vk.on                     # the lane; LIVE, polled every 250 ms
tools/tacli arm <i> 'vk.on=color=0,255,255'   # a different clear colour
tools/tacli arm <i> vk.on=off                 # back to GL, live, same second
tools/tacli arm <i> vk.off                    # the WHOLE module off, enumeration included
tools/tacli log <i> -g '^vk:'                 # the window, the device, the swapchain, the VA cost
```

- **`tacli glshot` and `tacli shot` do NOT show the Vulkan frame, and that is correct.** Route D
  gives Vulkan its own top-level window over the game's client area; GL goes on rendering into
  the game window underneath, so `glshot` reads the GL frame and `shot` reads the engine surface
  exactly as before. **To see what is on screen, grab THE WINDOW BY ITS ID** —
  `import -window <id> out.png`, at the client size, no cropping needed:

  ```bash
  import -window "$(tools/tacli ls --json | ...window[0]...)" out.png
  ```

  **`import -window root` DOES NOT WORK under `renderer=vulkan`, and it fails SILENTLY** [MEASURED
  2026-09-18, landing 8a's re-verification]. The root capture comes back as a **solid black
  rectangle** at the right size and position — the Vulkan surface is not in the root's redirected
  pixmap. Nothing errors. The trap is that black **passes every "no lever colour on screen" test
  vacuously**: the 8a check is "0 magenta of 786 432", and an all-black grab scores 0 magenta
  while showing no game at all. The same grab through `-window <id>` on the same frame gave
  **2 350 distinct colours** and the health bars. So a capture that is meant to prove a picture is
  RIGHT must assert something POSITIVE about it — a colour count, an expected run of pixels —
  never only the absence of a sentinel. (`xwininfo -id <win>`'s *Absolute* origin is still what a
  root crop would need, and is **not** the position `tacli ls` reports, which is the frame.)
  Using `tacli ls`'s origin is how a 100 % magenta window reads as 87.8 %.
- **The lever is two-way.** Clearing it puts the GL frame back on screen the same second. That is
  the whole reason the backend is on its own window: presenting on the game's `HWND` kills GL's
  presentation for the life of the PROCESS (roadmap Phase G, *Coexistence*) — the API keeps
  saying yes and the screen never changes again.
- **`tagpu_vk.off` is what an A/B against a pre-G19 DLL arms**, because half the module is not
  gated on `tagpu_vk.on`: the GPU enumeration runs whenever the GL render thread starts, armed or
  not, since the picker outlives the lane. (A launch that falls back to the GDI renderer never
  starts it — there is no lane to pick a GPU for there.)

**Since G19e it draws the whole world**: the SCAFFOLD overlay (`tagpu_scaffold.on`), the FEATURES
(`tagpu_feat.on` — trees, rocks, splats, GAF wreckage), the TERRAIN (`tagpu_terr.on`), the EFFECTS
and particles (`tagpu_fx.on` / `sfx.on`), the Classic++ cast-shadow DEPTH MAP (no lever of its
own — it follows `classicpp.on` and `shadows=1`) and the UNITS (no lever of its own either —
`tagpu_posedraw.c` is THE unit renderer, so what arms it is `native.on`). **Seven ported passes
now**, and their A/B levers must not be armed together — see below.

**The pixel A/B between the two lanes: `tagpu_<pass>.ab`.** Route D means no GL-side capture can
see the Vulkan frame, so each lane captures its own half of the SAME frame and the two files are
diffed. This is the shape every later ported pass copies.

```bash
tools/tacli arm <i> fps.on mark.on 'vk.on=color=0,0,0'   # the readout, the FONT, a black field
tools/tacli scenario load <i> selbox-facings --restart --res 1024x768 --maxfps 0
sleep 8                                                  # the readout's first averaging window
touch <gamedir>/tagpu_fps.ab                             # one frame, both lanes, then it latches
../.venv-undither/bin/python tools/vk-ab.py <gamedir>    # 0 px apart, or it names the first
```

- **`mark.on` is not optional ON EITHER LANE, and this costs half an hour if you miss it.** Under
  `renderer=vulkan` too: the hook is installed at DLL attach, so the requirement is unchanged and
  is not a property of the new backend. And because `mark.on` makes the marker pass DRAW in a
  fixture with anything selected, two passes then draw into one Vulkan frame and the lane refuses
  the capture (`1 A/B levers claimed this frame and 2 passes drew into it`). Launch with `mark.on`
  for the font, then `tacli arm <i> mark.on=off` and wait a few seconds before arming the `.ab`:
  `tagpu_text_frame` keeps its own copy of the font, so the readout survives the disarm. The readout draws
  TA's own glyphs, and the font reaches the render thread in the frame packet, published at
  **hook 8** — which is `markown`'s. With only `fps.on` armed the packet reads `font=0/0B`,
  `tagpu_text_place` refuses every string, and the readout silently draws nothing on EITHER lane:
  no `fps: the Vulkan edition is up` line, no quads, and an A/B that compares two black frames.
  `grep -a 'packet:' tagpu.log | grep -o 'font=[^ ]*'` is the one-line check. Under `--defaults`
  (the play set) the question does not arise.
- **`color=0,0,0` matters.** The GL half clears the frame to black before it draws; the Vulkan
  half clears to whatever `color=` says. Left at the default magenta the two captures differ in
  every pixel that is not a glyph.
- **The two captures are the same frame by construction** — the flag travels with the vertices,
  not through two independent lever polls — so a difference in the digits is a real difference and
  not two clocks.
- **The readout's INK COUNT is not a regression figure — only the 0 is.** `vk-ab.py` prints
  non-black pixels a side as the proof that it did not compare two blank frames, and for this pass
  those pixels are the digits of the frame rate, so the count moves with the number on screen: 88,
  89 and 92 have all been recorded on passing runs. Do not chase it. **For the world passes the
  ink count IS worth reading**, because there it is the scene (terrain fills the viewport, the
  scaffold's is `tall=`), and a big change in it means the fixture moved.
- **TA'S OWN FRAME IS WITHHELD ON A CLAIMED FRAME, and if you ever see it in a world capture the
  build predates 2026-09-18.** Landing 4c-1 put TA's 8-bit frame under everything on the Vulkan
  lane, which silently broke every world pass's A/B: the GL half is the bare world FBO (black
  wherever the pass did not draw) and the Vulkan half is the swapchain image, so every uncovered
  pixel differed — and for a pass that BLENDS, so did every translucent fragment, because one lane
  composites the effect over the game and the other over black. The terrain A/B read *NOT
  identical, 118 751 px* for a pass that was exactly right; the effects pass differed on 1 396 of
  its 1 622 GL-ink pixels. **4c-2 fixed it in the seam** — `if (draw_surf && nclaim == 0)`, so the
  bottom layer is skipped on exactly the frames a measurement is claimed — and both are 0 px again
  (terrain 630 719 non-black a side, effects 31 532). Nothing about the recipe changed; this is
  here so the numbers in the old notes are readable.

- **`vk-ab.py` PRINTS `on GL ink` / `on GL black` — read it, do not take it for a verdict.** The
  split is what tells an OVER-DRAW from a framing difference: a pass that paints where its twin
  painted nothing differs only on GL-black pixels, which is exactly G19e's line-rasterisation
  defect (*"all 126 of the twin's pixels plus exactly one extra fragment at the end of each line
  segment"*). An earlier cut of the tool exited **0** whenever every difference fell there, under
  the words "the pass agrees with its twin" — it does not any more. **The exit status is 0 only
  when every pixel agrees.**

- **THREE WAYS TO RUN AN A/B THAT LOOKS LIKE A RESULT AND IS NOT** [all three cost the gate-2
  landing a tick or worse, 2026-09-16]:

  * **`tacli arm <i> classicpp.off` does NOT turn Classic++ off** on an instance that was armed
    with `classicpp.on`. Both files then exist and `tagpu_opt.c`'s precedence is that the **`.on`
    wins** — `tagpu_menu.c`'s `write_levers` documents this after finding it in play, which is
    why the menu row owns both files. An "indexed control" run this way is **still restored**, so
    it agrees with the restored run exactly and proves nothing. It produced one confidently wrong
    conclusion (that a 5-px difference was pre-existing) that the previous-build A/B then
    disproved. Use a FRESH instance for the off side, or delete the `.on`.
  * **The `.ab` lever must appear while the instance is settled**, not be present at boot. Armed
    before a `--restart` it is consumed before the pass is ready and nothing is written: the GL
    half's "A/B wrote" line in the log is then the PREVIOUS run's, because the log appends across
    a restart. `rm` it, let the game reach the fixture, then `touch` it.
  * **`ss.off` IS NO LONGER REQUIRED FOR A WORLD PASS, and since landing 4c-3 (2026-09-18) the
    A/B runs in the shipped configuration.** It used to be mandatory at LAUNCH — the GL capture is
    the world FBO's viewport at `gw*ss × gh*ss` and the Vulkan one was the window's client rect, so
    at the default `ss` 2 the two files differed by a factor of two. The Vulkan half now comes from
    the world target, which IS `gw*ss × gh*ss`, so both halves are that size at any `ss` and the
    four *"the A/B needs ss=1"* refusals are gone. At 1024x768 with `ss=2` a capture is
    **2048x1536, 3 145 728 px** — expect the px counts below to be four times the `ss=1` ones.
    Still true: **arming `ss.off` LIVE does not take**, because the FBO is built where the lever is
    not re-read, so whichever `ss` you want has to be settled before the launch.
  * **The three UI passes (`gui`, `scaffold`, `fps`) are not `ss`-bound and never were.** `ss`
    sizes only the world FBO (`s_fbo2`) and none of the three ever binds it — `tagpu_scaffold.c`
    and `tagpu_fps.c` contain no `glBindFramebuffer` at all, and `tagpu_gui_surf.c` binds only its
    own mirror FBO and 0 — so their GL half is the default framebuffer at any `ss`. None of them
    ever carried an `ss != 1` refusal either. What they DO need is the window's own size: their
    Vulkan half is still the swapchain image, so the two agree only with no letterbox and `k = 1`.
  * **A world capture at 1080p with `ss = 2` is 3840x2160 — 33.2 MB a side.** The GL half mallocs
    that on the render thread and the Vulkan half allocates as much again in host-visible coherent
    memory, in a 32-bit address space where `tagpu_vk_shot.c` calls 8.3 MB *"real money"*. Both
    refuse and log rather than fault (`no memory for the A/B capture`, `%u bytes of readback would
    not allocate`), but before 4c-3 four of the five passes could not get here at all. At 1024x768
    it is 9.4 MB a side and nothing to think about.
  * **On a frame with LINE vertices, expect ~100 px that are not a port fault.** The GL twin's
    `glLineWidth(ss)` is clamped to 1 by the driver (measured, `tagpu_native.c:337`) while the
    Vulkan lane has `wideLines` and draws the `ss` px it asked for. Every Vulkan column of the line
    carries two ink pixels and every GL column one; **no pixel GL drew differs**. `vk-ab.py` reports
    it as *"0 of the N pixels the GL capture DREW"*, which is the signature. **Measured on both**:
    effects with lasers ~100 px of 3 145 728, markers with order lines **34 px** (vertical: GL inks
    column 620, Vulkan 619-620; horizontal: GL row 739, Vulkan 739-740). In coverage terms GL lays
    down 0.5 of a game pixel per step where the engine's rule is 1.0, and Vulkan lays down 1.0 —
    so the GL half is the wrong one, and `ss.off` is how you get a 0-px baseline on a line pass.

- **A world pass's A/B needs the fixture to still be ALIVE.** `vk-ab.py` refuses two blank
  frames — *"agree perfectly and prove nothing"* — and on `fx-lasers` that is what you get a
  minute in: `units: alive=2 onscreen=0`, no projectiles, no explosions. Reload and trigger the
  `.ab` within a few seconds of going live. The `fx:` log line is the check: `lines=`/`flashq=`
  at 0 means there is nothing to capture.

- **`vk-ab.py` REFUSES two captures of different sizes** rather than scaling one: the GL capture is
  the GL viewport and the Vulkan one is the client rect, so a mismatch means the fork is
  letterboxing (`--window` against `--res`, or k != 1). Run at a size where they agree.
- **The Vulkan file appears a few frames after the GL one**, and that is by design: the capture
  takes no wait of its own, so it is written when the seam's own fence for that frame slot comes
  round again (swapchain image count frames later — milliseconds). Sleep a second before diffing,
  or read `vk: shot: wrote …` in `tagpu.log`.
- **A capture lost to a swapchain rebuild or a teardown is not written at all**, and the log says
  so (`the A/B capture was lost to …`). `vk-ab.py` then reports the missing half rather than
  comparing against a stale file.
- The lever re-arms when the file is taken away and put back, on both lanes, so a second capture
  needs no relaunch — **`touch` on a file that is already there does NOT re-arm**, and the symptom
  is `vk-ab.py` reporting a missing half after you deleted the PPMs. `rm` it, sleep a second, then
  `touch`. `tagpu_<pass>_gl.ppm` / `_vk.ppm` are binary PPMs; `ffmpeg -i x.ppm x.png` to look.
- **A MISSING `_vk.ppm` NOW MEANS "no capture", never "a stale one" (since 4b-1).** The pass
  unlinks the target in the same breath as it latches the claim — so on every path, including the
  ones where the lane never presents at all, the file comes back only if `vk: shot: wrote …`
  appears. The `rm -f $G/tagpu_<pass>_*.ppm` in every recipe above is still worth keeping for the
  GL half, which has no such guard; the Vulkan half no longer depends on your remembering it.
- **ONE line means the arming was refused and the file you are looking at is NOT it:**
  `vk: ab: tagpu_<pass>_vk.ppm could not be removed (error N) - this arming is REFUSED`. It fires
  when something holds the target open or it is read-only — an image viewer left on the last
  capture is the usual cause. Close it, `rm` the file, and re-arm. Every other refusal now names
  itself too (`shot: a capture is already in flight`, `shot: the image is WxH, outside 1..8192`,
  `shot: the N-byte staging buffer was refused`), so an absent file with no line beside it means
  the lever genuinely never fired — a different fault with a different fix.
- **Under `renderer=vulkan` the Vulkan half is claimed on the INTENT, and there is no `_gl.ppm`
  at all.** `vk-ab.py <gamedir> --pass <p>` therefore reports a missing half on that lane by
  design; use its **file-to-file** mode instead, against the same build's two-lane `_vk.ppm`:
  `vk-ab.py two-lane_vk.ppm vulkan-only_vk.ppm`. For a pass whose picture is a pure function of
  the camera that is byte-identical (the scaffold was); for one that draws its own measurement it
  cannot be, and the honest reading is WHERE the differing pixels are — the readout's 17 px all sit
  in the digit columns while the label agrees exactly.
- **ARM ONE PASS'S `.ab` AT A TIME, and turn the other ported passes' `.on` off** (`fps`,
  `scaffold`, `feat`). Each GL capture
  holds one pass (its twin blacks the frame around its own draw); the Vulkan capture is one frame
  and holds *every* armed pass. The lane refuses to capture when more than one pass claimed the
  frame **or** more than one drew into it, and says so:
  `vk: N A/B levers claimed this frame and M passes drew into it - nothing captured`. That is the
  guard working — it writes no pair rather than a wrong one.

**The scaffold's A/B (G19e), which is the world-pass shape:**

```bash
tools/tacli arm <i> scaffold.on 'vk.on=color=0,0,0'      # ONLY the pass under test
tools/tacli scenario load <i> feat-forest --restart --res 1024x768 --maxfps 0
sleep 8                                                   # let the level settle
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_scaffold.ab $G/tagpu_scaffold_*.ppm; sleep 2; touch $G/tagpu_scaffold.ab
../.venv-undither/bin/python tools/vk-ab.py $G --pass scaffold     # 0 px apart
```

- **`feat-forest` is the fixture** because the scaffold only stamps TALL features (def Height >=
  10): it gives `tall=151` and 190 247 ink pixels at 1024x768, where an empty scene would give a
  0-px result that means nothing. `grep -a 'scaffold: swept' tagpu.log` reports `tall=` — read it
  before believing a pass. `selbox-facings` works too (`tall=163`).
- **No `mark.on` needed for this one** — the scaffold draws no text, so the font trap above is the
  readout's alone.
- The GL twin blacks the frame immediately before its own draw and reads back immediately after,
  so **the player sees one frame with the terrain missing**. That is the lever, not a fault.
**The feature pass's A/B (G19e), which is the shape for every pass that DEPTH-TESTS:**

```bash
tools/tacli arm <i> 'native.on=all wrecks' feat.on ss.off 'vk.on=color=0,0,0'
tools/tacli scenario load <i> feat-forest --restart --res 1024x768 --maxfps 0
sleep 12
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_feat.ab $G/tagpu_feat_*.ppm; sleep 2; touch $G/tagpu_feat.ab; sleep 6
../.venv-undither/bin/python tools/vk-ab.py $G --pass feat        # 0 px apart
```

- **`ss.off` is optional since 4c-3.** Both halves are the world target's size, `gw*ss × gh*ss`,
  at any `ss` — the ink counts quoted here are `ss=1` figures, so multiply by `ss²` if you leave
  supersampling on. What is still refused is a world claim on a frame with **no** world target, and
  the lane says so by name: *"a world pass claimed the A/B and this frame has no world target"*.
  Confirm the size you are measuring at with `grep -a 'vk: world:' tagpu.log` → `2048x1536 target
  (1024x768 at ss=2)`.
- **`native.on` must carry `wrecks`, or the feature pass emits nothing at all.** It only owns the
  draw when the native wreck pass owns `DrawUnit` (see *Features* below); without it the gather is
  muted, both halves are black and the A/B reads a meaningless 0. Measured 2026-09-18: with no
  `wrecks` no `_gl.ppm` is written at all and `vk-ab.py` reports the GL half missing.
- **AND THE ARM SET ABOVE NO LONGER PRODUCES A PAIR BY ITSELF — USE `tacli eye <i> 1400 1600`**
  [measured 2026-09-18]. `wrecks` makes the native husk pass draw the map's one 3D wreck, so the
  UNIT twin draws too, and the lane refuses the frame: `1 A/B levers claimed this frame and 2
  passes drew into it - nothing captured`. That is the guard working — a Vulkan frame carries every
  armed pass while a GL capture carries one — and the recipe predates the unit pass having a Vulkan
  twin. The fix is a camera where the wreck is off-screen: on `feat-forest` at 1024x768,
  `tacli eye <i> 1400 1600` gives `3dwreck=0 body=101 shadow=76` with features still on screen,
  and the pair comes back **0 px apart at 132 274 ink pixels a side**. Read
  `grep -a 'feat: rect=' tagpu.log | tail -1` for `3dwreck=` before arming; `(2566,616)` and
  `(2100,900)` both have the wreck in shot, and `(3400,2200)` has no features at all.
  `native.on=<a type that matches nothing> wrecks` does NOT help: the wreck is not a unit.
- **No `mark.on` needed** — the readout's font trap is the readout's alone.
- **Both halves come out UPSIDE DOWN**, and that is correct: this pass draws into the world FBO,
  whose clip-space +1 is the bottom of the screen (the composite quad turns it over). They are
  upside down identically, which is all the comparison asks. `ffmpeg -i x.ppm x.png` to look.
- **Classic++ makes the pass stand down**, with one line in the log: its restored atlas has no CPU
  mirror, and drawing without it would be a different picture from the twin's. A `tacli` instance
  opts out of the play defaults so this does not arise; `--defaults` does.
- Expect `feat: atlas mirror armed, 4096 KB — N painted frame(s) re-decode …` once per session, and
  `vk: feat: the Vulkan edition is up … depth format 129` (`VK_FORMAT_D24_UNORM_S8_UINT`). A
  device that offers no 24-bit depth does **not** print `depth format 0` — the pass returns before
  that line — it prints `the seam's render pass carries no depth attachment …` and stays down on
  purpose. *(This said to look for `depth format 0`, which cannot be printed; corrected by the
  G19e re-review, 2026-09-15.)*

**The terrain pass's A/B (G19e), the simplest of the world passes to run:**

```bash
tools/tacli arm <i> terr.on ss.off 'vk.on=color=0,0,0'
tools/tacli scenario load <i> feat-forest --restart --res 1024x768 --maxfps 0
sleep 10
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_terr.ab $G/tagpu_terr_*.ppm; sleep 2; touch $G/tagpu_terr.ab; sleep 8
../.venv-undither/bin/python tools/vk-ab.py $G --pass terr     # 0 px apart
```

- **No `native.on` needed** — terrain takes the draw on `terrown.on` alone, which `tacli`
  auto-arms at launch when `terr.on` exists. (That is unlike the feature pass, which is muted
  without `native.on … wrecks`.) `ss.off` is optional since 4c-3, as for every world pass —
  at `ss=2` this pass measures **0 px of 3 145 728 with 2 522 876 ink a side**.
- **Terrain covers the WHOLE viewport, so the ink count is the viewport**: 630 719 of 630 784 at
  1024×768, 1 820 568 at 1080p. A pass that reads much less than the viewport has been scissored
  wrong or has drawn nothing; there is no "sparse fixture" failure mode here to worry about.
- **The terrain pass stands down under Classic++ `assets=1`**, and says so once: the restored tile
  atlas is a GPU-only surface with no CPU mirror. A `tacli` instance opts out of the play defaults
  so it does not arise; `--defaults` does. **The cast-shadow map is NO LONGER one of these**
  (G19e's fifth pass): `tagpu_vk_shadow.c` draws it and the terrain pass samples it. What stands
  the pass down now is a frame whose map the shadow pass could not reproduce — every frame with a
  unit caster on screen, until the unit pass lands — and that refusal is per-frame, not latched.
- Expect `terr: atlas built 2176x<h> for <n> tiles`, `terr: height grid WxH uploaded`, and
  `vk: terr: the Vulkan edition is up - 4 frame slots, uniform stride 256, depth format 129`. The
  two CPU mirrors are the buffers those two builds were handed, **kept rather than freed** while
  the Vulkan lane is armed: **6.16 MB on Two Continents, 8.15 MB on Anteer Strait**. A
  `terr: the GL tile atlas has no CPU mirror yet` line is the first frames of a session and clears
  itself; if it persists, `tagpu_vk_armed()` is answering no (check `tagpu_vk.on` is really there).
- **An in-process map change brings the WHOLE Vulkan lane down and back up**, pass included —
  measured 2026-09-15: leaving a level to `MAINMENU` and starting another stops the GL render
  thread, and the lane goes with it (`vk: render thread stopping - down`, then a fresh
  `vk: swapchain`). So a second map is a fresh lane, not a resized one, and the atlas is rebuilt
  at that map's size (Anteer Strait 2176×3774 for 7051 tiles against Two Continents' 2176×2720 for
  5062). Useful as a **second fixture**: the pass reads 0 px there too.

**The effects pass's A/B (G19e), and it has a bucket switch the others do not:**

```bash
tools/tacli arm <i> native.on fx.on ss.off 'vk.on=color=0,0,0'
tools/tacli scenario load <i> fx-lasers --restart --res 1024x768 --maxfps 0   # or fx-mix
sleep 13
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_fx.ab $G/tagpu_fx_*.ppm; sleep 2; touch $G/tagpu_fx.ab; sleep 9
<main checkout>/.venv-undither/bin/python tools/vk-ab.py $G --pass fx
```

- **`native.on` is required** — `tagpu_fx_render` is called from inside the native pass, so with
  only `fx.on` there is no gather and no capture, on either lane.
- **...BUT GIVE IT A TYPE FILTER THAT MATCHES NOTHING** — `native.on=nosuchunit` [measured
  2026-09-18]. A bare `native.on` draws every unit, so the unit twin draws beside the effects one
  and the lane refuses the frame for two passes drawing. The filter keeps the call site (which is
  what `fx` needs) without the unit draw; the lasers come from the engine's projectile list and do
  not care which units the native pass paints.
- **ARM THE `.ab` ABOUT FOUR SECONDS AFTER THE SITUATION IS APPLIED, not thirteen.** The
  recipe's `sleep 13` then `sleep 9` lands after the fixture has gone quiet — measured
  `lines=0 flashq=0`, and the pair then agrees about an empty frame. At `sleep 4` it reads
  `flashq=6` and the pair is **0 px at 3 327 ink pixels**. `grep -a '^fx:' tagpu.log | tail -1` is
  the check.
- **THIS PASS CANNOT BE COMPARED ACROSS TWO RUNS, and no camera fixes that.** It draws transient
  projectiles, so two captures agree only if the same ones happen to be alive: one run read
  `flashq=6 lines=0` (3 327 ink px) and the next `lines=2 flashq=0` (730 ink px), in the same
  region of the frame. Its valid oracle is the two-lane A/B, which compares ONE frame across both
  lanes. Same shape as the frame-rate readout, and for the same reason.
- **`fx.on` TAKES TOKENS, AND THEY ARE HOW YOU ISOLATE A BUCKET.** `nolines` leaves the sprites,
  flashes and explosions; `nosprites noexpl nodebris nomodels` leaves the lasers alone. Both lanes
  read the same tokens, so either one is a valid A/B — and isolating is the only way to tell a
  line finding from a sprite finding. Read the heartbeat to see what you actually got:
  `grep -a '^fx: proj' $G/tagpu.log` prints `lines=N sprites=N flashq=N models=N`.
- **THE COMBAT BURNS OUT.** `fx-lasers` fires for tens of seconds and then everything is dead;
  `fx: proj=0 … lines=0 sprites=0` in the heartbeat means there is nothing to capture, and the
  lever correctly waits rather than capturing an empty frame — `vk-ab.py` then reports the missing
  half. **Reload the scenario before each capture** rather than firing the lever repeatedly at a
  finished battle.
- **The lever waits for a frame that has effects**, by construction: the GL twin returns before the
  capture block when all four buckets are empty, so `tagpu_fx.ab` stays armed until something is
  actually drawn. That is why a capture can appear several seconds after the `touch`.
- **THE LASERS ARE NOT ALWAYS 0 px, AND THAT IS KNOWN AND SHIPPED.** A line whose exact path lands
  *exactly* halfway between two pixel rows is a tie, and the Y flip makes the two lanes break it
  opposite ways — measured at **3 px of 786 432** on one `fx-lasers` bolt, deterministic, 49 of its
  50 columns identical ([gpu-status](gpu-status.html) §2.31). **Do not go hunting for it as a new
  bug.** Triangle buckets are 0 px and stay 0 px; if a *triangle* run is non-zero, that is real.

**The unit pass's A/B: the lever is `tagpu_posedraw.ab` and the pass name is `posedraw`.**

```bash
tools/tacli arm <i> 'native.on=all wrecks' mark.on classicpp.on \
      'classicpp.cfg=assets=1 shadows=0 aniso=1' ss.off 'vk.on=color=0,0,0'
tools/tacli scenario load <i> selbox-facings --restart --res 1024x768 --maxfps 0
sleep 26
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_posedraw.ab $G/tagpu_posedraw_*.ppm; sleep 2; touch $G/tagpu_posedraw.ab; sleep 10
<main checkout>/.venv-undither/bin/python tools/vk-ab.py $G --pass posedraw   # 0 px
```

- **USE `tagpu_rglsl.step`, NOT `mark.on`, TO MAKE THE RESTORER PAINT.** `touch
  $G/tagpu_rglsl.step` steps the restorer while arming NO pass, which is what an A/B of restored
  art needs: the capture refuses when two passes draw, so the stepper must not be one.
  **Measured 2026-09-17** on `selbox-facings` at 1024×768, `native.on` alone plus the lever:
  **0 px of 786 432** with Classic++ art on, 2 128 non-black a side — identical to what `mark.on`
  gave — and the GL lane's own picture moves by **6 200 bytes** between `assets=1` and `assets=0`,
  which is the proof the twin painted rather than the branch being untaken.
  The bullet below is the history and the trap it replaces; read it before trusting any older
  recipe you find.
- **`mark.on` WAS the lever, and why it stopped being one.** `tagpu_rglsl_step()` — the only thing
  that ever paints a Classic++ restored twin — has **two** callers. The one that matters for a
  world A/B is `tagpu_native.c:3297`, inside `if (fxOn || sfxOn || featOn || terrOn || markOn)`;
  the other is `tagpu_gui_surf.c:2465`, which steps it when the UI atlas has a restore job of its
  own and nothing else stepped it this frame. Armed with `native.on` alone the first never runs —
  the unit atlas's job is still queued (`tagpu_native_frame` reaches `tagpu_r3d_atlas_frame` on
  `s_armed` alone) and it sits at priority 3, behind terrain, features and effects. **Measured:
  the twin was still unpainted through the whole fixture** — the `uRestored == 1` branch read
  alpha 0 on both lanes, both fell back to the palette per texel, and the A/B reported **0 px
  about a branch neither lane took**. Treat that as what one fixture did, not as a guarantee the
  code gives: the UI caller can step the queue, so **check the twin painted** (next bullet)
  rather than inferring it from the levers.
  [The second caller was found by the gate-3a re-review; the first wording said the native call
  was the only one.]
  `mark.on` is the one item on that list with no Vulkan pass of its own, so it steps the restorer
  and still leaves the unit pass as the only pass drawing into the Vulkan frame. This cost gate 3a
  a whole round of wrong conclusions: the fixture went from 0 px to 2 126 of 2 132 differing when
  that one file was added, and four real faults were hiding behind the 0.
- **CHECK THE TWIN ACTUALLY PAINTED before quoting any restored-art figure.** There is no log line
  that says it did. Two ways, both cheap: `touch $G/tagpu_restoredump.on` writes
  `tagpu_restore_<tag>.{r8,rgba,idx}` once the restore job goes idle (and logs `restored twin
  dumped`); or run the fixture twice at `assets=1` and `assets=0` and `cmp` the two **GL**
  captures — byte-identical means the restorer painted nothing.
- **`aniso=1` is the A/B's, and 4 is what ships.** Anisotropic sample placement is
  implementation-defined; GL and Vulkan do it differently on the same hardware, and that is the
  whole of what is left between the lanes once everything else is carried across — 566 of 2 132
  unit pixels at worst channel 9. `aniso=` is a `tagpu_classicpp.cfg` knob that **both lanes
  read**, so setting it to 1 for the measurement is a stated substitution, not a lane being
  configured differently from its oracle. At the 4× default the pass draws and the A/B is
  expected to be non-zero; do not report that as a regression.
- **`shadows=0` for this one**, unless you want the soft-shadow PCF's 1 px at (517, 396) in the
  figure too. That pixel is on both builds and is the PCF, not the pass.
- **Do NOT arm `terr.on` alongside it.** With shadows on, terrain needs the unit pass *drawing*
  for the caster census to close — and two drawing passes make the lane refuse the capture
  outright (`1 A/B levers claimed this frame and 2 passes drew into it`). So with shadows on,
  terrain and units are two runs, always. That message is also the best evidence you have that
  both passes really drew: the guard counts them.
- **A tacli instance ships a REPLACEMENT MESH active.** `gamedir/hires/armpw.glb` is loaded for
  every Peewee — `hires/off/` beside it is a *parking directory*, not a lever, so removing it does
  nothing and `rm` says `Is a directory`. Until gate 3b lands, any fixture with an ARMPW in it
  stands the whole Vulkan world down: the caster census refuses 1 with one Peewee on screen and 16
  on `crowd-static`. `grep -a 'hires\\' tagpu.log` names every unit that took one.
- **A scratch worktree cannot run tacli**: `tacli create` wants the wine prefix template, which is
  gitignored and lives only in the real checkouts (`tacli: template wine prefix missing`). To A/B
  a FOREIGN build, make the instance from a real worktree, `cp` the other tree's `ddraw.dll` over
  `<gamedir>/ddraw.dll`, and launch with **`--keep-dll`** so tacli does not refresh it back.
  `md5sum` the three DLLs in the script's own output; that line is what tells you the run tested
  what you think it did.

**Putting the game window somewhere other than the main monitor.** `tile_for` lays its grid over
the WHOLE X screen from (0,0), which spans every head, so no `--slot` number means "that monitor"
and instances land wherever the grid falls — in practice on the primary. To park one elsewhere,
read the layout and move the window after launch:

```bash
wid=$(tools/tacli ls --json | python3 -c "import json,sys;print([r for r in json.load(sys.stdin) if r['name']=='<i>'][0]['window'][0])")
DISPLAY=:0 xdotool windowmove "$wid" <x> <y>
```

- **Take the window id from `tacli ls --json`, never from `xdotool search --name`.** Searching by
  title matches ANY window carrying the string — including the terminal running the script, whose
  title holds the command line. That is not hypothetical: it moved a 668×546 shell and reported
  success.
- **Derive the head from `xrandr --query`** rather than hard-coding one; reading the layout is a
  read, which the desktop-is-the-user's rule allows (what it forbids is changing modes). Picking
  the *smallest* connected non-primary output that fits is the polite default — a large secondary
  is more likely to be something the human is reading.
- **The window must stay fully on a VISIBLE head.** This is not cosmetic: `glReadPixels` outside
  the visible region is undefined, and that is exactly what every A/B capture reads. `tile_for`'s
  own docstring is about the same trap.

**The shadow map's A/B (G19e), and it is the only pass whose oracle is another pass's pixels:**

```bash
tools/tacli arm <i> terr.on ss.off 'vk.on=color=0,0,0' classicpp.on \
      'classicpp.cfg=assets=0 shadows=1 terrainshadow=1 shadowsun=225,8'
tools/tacli scenario load <i> static-terrain --restart --res 1024x768 --maxfps 0
sleep 12
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_terr.ab $G/tagpu_terr_*.ppm; sleep 2; touch $G/tagpu_terr.ab; sleep 8
<main checkout>/.venv-undither/bin/python tools/vk-ab.py $G --pass terr    # 0 px apart
```

- **`tagpu_shadow.ab` does not exist, and cannot.** The pass draws a DEPTH MAP, not pixels, so its
  A/B is **the terrain pass's** with the map on: 0 px there means the geometry, the stored depth
  values, the blocker search, the bilinear PCF and the receiver-plane bias all agree.
- **`assets=0` is what makes Classic++ measurable at all.** With `assets=1` the terrain pass
  refuses every frame (the restored tile atlas has no CPU mirror), so there is nothing to compare.
- **`shadowsun=225,8` is the fixture, and the default `225,40` is NOT.** `terrainshadow=1`
  self-shadows the ground, and at 40 degrees of elevation that is nearly invisible — measured on
  `static-terrain` at nine camera positions, shadows on against shadows off: **0 px at seven of
  them, 38 and 54 at the other two**. At elevation 8 the map shadows **517 270 of 786 432**
  pixels. A 0-px A/B at the default sun proves nothing about the map.
- **CONFIRM THE PICTURE DEPENDS ON THE MAP BEFORE BELIEVING A 0.** One `glshot` with `shadows=1`
  and one with `shadows=0`, diffed, is the whole check, and it is what turned a vacuous pass into
  a measurement.
- **`static-terrain` is the fixture because its two towers are in opposite CORNERS.** The gather is
  on-screen only, so with the camera in the middle of the map there is no unit caster — and **every
  caster but the heightfield refuses the frame**, because the units, the posed bodies and the
  replacement meshes are the unit pass's to port. One tower in view is enough:
  `vk: shadow: the GL map holds 1 caster(s) this lane has no copy of`, and the terrain pass stands
  down with it. Both recover the moment it leaves view; the refusal is per-frame, not latched.
- **Without `native.on` there are no unit casters at all**, which is why the recipe above does not
  arm it. Arm it to test the refusal, not to take the measurement.
- **`vk: shadow: the GL map holds N caster(s)` also fires when the HEIGHTFIELD has no CPU mirror**,
  not only for units: `tagpu_terr_hills_draw` draws whether or not the mirror is there, and a draw
  with no copy of it is counted as a caster like any other. The symptom of the alternative — which
  is what the landing review caught before it shipped — would have been an A/B that reads 0 px on a
  frame where the Vulkan map is empty and the GL one is not.
- **The terrain pass has a THIRD way to stand down**: a device that will not filter a depth format
  linearly, which makes the 16-tap PCF impossible to reproduce (`terr: … compare sampler could not
  be made LINEAR`). It does not arise on the reference setup.
- **The 1 px that is left is the LAMBERT, not the shadow.** With Classic++ `light=1` the terrain
  pass reads **1 px of 786 432, one level**, deterministic for a given camera; `light=0` reads 0,
  and `shadows=0` reads the same 1 px at the same pixel. It is a stated bar of the terrain pass
  (gpu-status §2.32), newly measured because Classic++ frames used to be refused outright.
- Expect `vk: shadow: up (depth format 125, linear 1, N slots)` — 125 is
  `VK_FORMAT_X8_D24_UNORM_PACK32`, `GL_DEPTH_COMPONENT24` exactly — and one
  `vk: shadow: caster mesh uploaded, … serial N` per map. The GL twin's own map is dumped with
  `tacli arm <i> shadowdump.on` (`tagpu_shadow.pgm`, 16-bit, near = small) and is the way to check
  the map has contents at all: on Town & Country 54 % of its texels carry geometry.
- **The caster mesh costs 10 MB on Town & Country** (291 600 vertices / 1 743 126 indices) in the
  GL module's CPU mirror and again in device memory, and the map is **16 MB a frame slot** at the
  default `shadowres` 2048. Both are paid only while the Vulkan lane is armed.

**The UI pass's A/B (G19f landing 1), and its levers are the GL layer's own:**

```bash
# the four tokens are what empty the sharp layer and the colour twins, so the
# GL lane draws the 1x mirror ALONE and the two halves are comparable.
# `nostring` is read at ATTACH, so it must be armed BEFORE the launch.
tools/tacli arm <i> 'gui.on=nostring nocursor nominimap norestore' 'vk.on=color=0,0,0'
tools/tacli launch <i>                         # the SHELL is a valid fixture here
# ...or in game, where the resolution clause actually bites:
tools/tacli scenario load <i> selbox-slope --restart --res 1024x768 --maxfps 0
sleep 12; tools/tacli keys <i> tab tab
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_gui.ab $G/tagpu_gui_*.ppm; sleep 2; touch $G/tagpu_gui.ab; sleep 6
<main checkout>/.venv-undither/bin/python tools/vk-ab.py $G --pass gui
```

Four traps, every one of them paid for on 2026-09-16:

* **THE LEVER LATCHES UNTIL THE FILE GOES AWAY.** `s_abDone` is cleared only by `tagpu_gui.ab`
  being ABSENT on a poll, so `touch`ing it again captures nothing. Remove it, wait past the 500 ms
  poll, then touch it — the recipe above does, and a second capture that "did not fire" is almost
  always this.
* **THE TWO HALVES CAN LAND ON DIFFERENT FRAMES**, and then `vk-ab.py` says one file "is not there"
  rather than comparing stale captures. Retry the whole arm-and-wait; it pairs within a couple of
  tries. It is not a port failure and it is not worth debugging in the moment.
* **CHECK THE CAPTURE'S SIZE AGAINST THE FIXTURE YOU ASKED FOR.** `gui: A/B wrote
  tagpu_gui_gl.ppm, 640x480` after a `--res 1920x1080` means the `--restart` left the game in the
  SHELL and you are measuring the menu. A figure whose fixture cannot be confirmed is not a figure.
* **THE SHELL IS 640×480 WHATEVER `--res` SAYS** — TA's front end has its own resolution, so the
  `--res` clause only bites in game.

**The UNIT pass's A/B (G19e, the last of the gate), and it is the first that needs no `.on` of its
own:**

```bash
tools/tacli arm <i> 'native.on=all wrecks' ss.off 'vk.on=color=0,0,0' classicpp.on \
      'classicpp.cfg=assets=0 shadows=1'
tools/tacli scenario load <i> crowd-static --restart --res 1024x768 --maxfps 0
sleep 12; tools/tacli keys <i> tab tab            # PAUSE: crowd-static is NOT static
G=<main checkout>/tagpu/instances/<i>/gamedir
rm -f $G/tagpu_posedraw.ab $G/tagpu_posedraw_*.ppm; sleep 2; touch $G/tagpu_posedraw.ab; sleep 8
<main checkout>/.venv-undither/bin/python tools/vk-ab.py $G --pass posedraw
```

- **The lever is `tagpu_posedraw.ab`, not `tagpu_unit.ab`** — it is named after the GL twin like
  every other one, and `--pass posedraw` is what `vk-ab.py` wants.
- **There is no `unit.on`.** `tagpu_posedraw.c` is THE unit renderer and has had no lever since
  G16 step 8, so what arms the pass is `native.on` plus the Vulkan lane. `ss.off` is required as
  for every world pass.
- **`crowd-static` ANIMATES, whatever its name says.** Two captures a few seconds apart differ by
  ~17 700 px on either lane. That does not invalidate the A/B — both halves are the same frame by
  construction — but it makes every other comparison meaningless, so **pause with `tab tab`
  first**: frozen, each lane is byte-identical to itself across captures and the differing SET
  reproduces exactly.
- **THE SHADOW MAP REFUSES ANY FRAME WITH A CASTER THIS LANE HAS NO COPY OF, AND `crowd-static` HAS
  16 OF THEM.** Its ARMPWs take the hi-res path, so with `shadows=1` you get
  `vk: shadow: the GL map holds 256 caster(s) this lane has no copy of (240 of them the unit pass
  carries)` and both passes stand down. That is the census working. To measure the BODIES at scale,
  set `shadows=0`; to measure the DEPTH TWIN, use a fixture with no replacement mesh —
  `selbox-facings` with `shadows=1` is the one (4 units, 2125 ink, and `hires/armpw.glb` is not
  among them).
- **Expect a non-zero answer, and it is a stated bar.** 64 px of 786 432 on 208 699 ink at
  1024×768, 90 of 2 073 600 at 1080p, 51–65 across three cameras — a stable **0.03 % of unit ink**.
  It is deterministic, it is 48 single pixels of 55 clusters, and zooming one run shows a texture
  ROW boundary picked one row apart: a fragment centre landing exactly on a texel edge, which is
  §3.0's terrain finding in a second lane. **Do not chase it as a new bug**; a *structured*
  difference (a whole unit, a whole face, a shift) would be real.
- **The small fixture is the exact one**: `selbox-facings` reads **0 px** with `shadows=0` and
  **1 px** with `shadows=1` — and that 1 px survives `light=0` and vanishes with `shadows=0`, so it
  is the shadow term's own quantisation and not §2.32's lambert.
- **`scaffold.on` makes the pass stand down, and that is not a bug**: the unit fragment shader
  locates itself with `gl_FragCoord`, whose origin is the LOWER left in GL and the UPPER left in
  Vulkan, so the flipped viewport mirrors it. The log says so once and the pass recovers within a
  frame when the lever is cleared. `scaffold.on` is not in the default arm set anyway.
- **ARM ONE OF `terr.on` / `feat.on` / `fx.on` / `mark.on` OR THE SHADOW MAP IS GARBAGE — this cost
  an hour.** Until 2026-09-15 `tagpu_native.c` filled its `TAGPU_FXVIEW` only when one of those
  five was armed and handed it to `tagpu_shadow_begin` regardless, so with none of them armed the
  shadow module read uninitialised stack: `shadow: frame zoom=0.000 res=2048 k=-4 … window=(65776,
  66912028 3486028x520)` in the log, and the Vulkan shadow pass silently found nothing **with no
  line of its own**. Fixed, but **`shadow: frame zoom=0.000` in any older log means exactly this**,
  and the tell is the insane window numbers beside it.

- **To measure constraint 4** — that the GL lane did not move — arm `vk.off` on two instances, one
  running the tree's DLL and one the previous one, load the same static fixture, park the pointer
  in the same place (`keys <i> mouse:60,400`, the side panel, so its animating sprite is outside a
  viewport crop), pause with `tab tab`, and diff two `glshot`s over the world viewport only.
- **THE `PAUSED` BANNER BLINKS, AND IT IS WORTH ~2 800 px.** Measured 2026-09-15 on
  `selbox-facings` at 1024x768: two paused instances captured at different moments in that blink
  differ by the whole word — **2 747 px on one pair, 2 933 on another, at x 502..636, y 372..400**
  — while **every pairing reads 0 outside that rect**, including two separate relaunches of the
  same binary. So **exclude the banner rect from the diff, or catch both halves in the same blink
  phase**; loading the two instances together does *not* help, because the blink is free-running.
  **And crop the region and LOOK at it before explaining a floor.** The first pair here read 0,
  the second read 2 747, and the obvious inference — tick skew, two sims frozen at different
  ticks — was wrong; rendering the 180x60 crop showed the word `PAUSED` in one capture and bare
  grass in the other, which took a minute and saved a fix for a problem that did not exist. This
  is a *different* artefact from the two-state 71-px sliver at x 1017..1023 (the frame's right
  edge); both are reasons the floor is measured rather than quoted.
  **Build the previous DLL with `git archive <rev> | tar -x -C <dir>`** and symlink
  `tagpu/gamedir` and `unditherer/models` into it: `tacli` pins the DLL of the tree it is run
  from, purely by path, so that tree's `tools/tacli` launches the old binary with no other setup.
  **`wineprefix` must be a `cp -al` CLONE, not a symlink**, and `tagpu/instances` is better
  symlinked at the main checkout's than left inside the archive tree. `clone_prefix` runs
  `cp -al <tree>/wineprefix <inst>/prefix`, and `cp -al` on a SYMLINK copies the symlink rather
  than hardlinking the tree — so every instance made from such a tree gets a `prefix` symlink
  pointing straight at the shared template, and the game exits during launch with **no
  `ErrorLog.txt` and a perfectly healthy `tagpu.log`** that simply stops. (One instance created
  that way launched anyway and therefore ran directly inside the template prefix; a base tree set
  up this way can write the shared registry. Measured 2026-09-15.)
- **MEASURE THE CROSS-LAUNCH FLOOR WITH TWO SAMPLES OF ONE BINARY, every time.**
  `selbox-facings` at 1024x768 has a **two-state 71-px artefact** at x 1017..1023, y 236..277 —
  the frame's right edge — which reproduces DLL-against-itself. G19e's first base-versus-landing
  pair happened to land in the same state and read 0; the next sample of the same binary read 71.
  **`feat-forest` cannot answer a cross-launch question at all**: its walking commander gives it a
  ~4000 px floor, *larger* than the difference being looked for. It is an excellent two-lane A/B
  fixture (both captures are the same frame) and a useless cross-launch one.
- **Let the game RUN until the "obliterated" chat lines go, and only then pause.** The engine's own
  "Arm forces have been obliterated" messages from `clear_existing` sit in the viewport's top-left
  (x 138..430, y 52..106 at 1024x768) and are worth ~8 200 px until they expire — and **they expire
  on TICKS, not on wall-clock seconds**. Pausing with `tab` straight after `scenario load` freezes
  them on screen indefinitely: G19e's second landing read **8 282 px** between two binaries purely
  because one had been paused at tick 155 and the other at tick 433. About 60 s unpaused at
  `speed` 10 clears them; check with
  `grep -a 'packet:' tagpu.log | tail -1 | grep -o 'tick=[0-9]*'` on both instances before pausing.
  (The older note here said "wait ~35 s after the load", which is only true of a game left running.)

**The GPU row is in Options → Visuals, Window column, and its list is ONE LAUNCH BEHIND.** The
captions live in the generated `.GUI`, which is written at DLL attach, and a Vulkan instance
cannot be created there — so a worker enumerates after the render thread is up and writes
`gamedir/tagpu_vk.gpus` for the *next* launch. On a gamedir that has never run this DLL the row
reads `(not listed yet)` and is greyed; relaunch once and it lists the devices.

```bash
tools/tacli ui <i> show VGPU                  # stages, stage, grayed
tools/tacli ui <i> click VGPU                 # cycle; the render thread rebuilds within a frame
cat <gamedir>/tagpu_vk.gpus                   # "<0|1> <name>" per device, 1 = discrete
cat <gamedir>/tagpu_vk.cfg                    # gpu=<name> — the choice, stored BY NAME
```

- **The row is greyed unless the Vulkan lane is armed**, and unless there are at least two
  devices. It binds the *Vulkan* device only: OpenGL cannot be retargeted in-process, so under
  the GL lane the GPU is a launcher-level setting (`DRI_PRIME` / `__NV_PRIME_RENDER_OFFLOAD`
  through `Instance.env()`, or the per-application driver profile on Windows).
- **The row plates the device actually BOUND, not the one requested.** A stored name that is no
  longer present falls back to the discrete default and logs `the requested GPU "…" is not among
  the devices present`, and the row then shows the device that was used.
- **At most eight devices are listed** — our cap, not the engine's: a stage button's art index
  is clamped at `0x4A8003`, so a row past four stages draws the four-bar plate and still works
  (the `UI scale` row has six). Past the fourth the bar count saturates and the caption stays
  right. The log says when any were dropped. Names are truncated to 31 characters at a word
  boundary, because they come from the driver and land in a generated `.GUI`.

### The HUD is scaled inside the Screen Size (G18f, `tagpu_hud.on`)

Since G18f the in-game HUD is magnified **within** the player's chosen Screen Size — the panel
`128s` wide, the two bars `32s` tall — and simply **covers** the outer part of a world the
engine goes on drawing at full size. Auto is `H/480`: exactly 1.0 at 640×480, **2.25 at 1080p**
and **4.5 at 4K**.

**It is NOT a play default** — it is armed by hand. It was one for a day, and in that day it
wrote the engine's viewport rect and tore the world in two (what you clicked was
`((s−1)·128, (s−1)·32)` from what you saw); [GUI renderer](../../../research/notes/gui-renderer.md)
§22.5 has the measurement. It writes no engine memory now.

```bash
tools/tacli arm <i> hud.off                 # stock HUD; the A/B, and what a 1x measurement needs
tools/tacli arm <i> 'hud.on=scale=auto'     # Auto: the panel fills the screen height
tools/tacli arm <i> 'hud.on=scale=150'      # a percentage of stock; clamped to this screen's ceiling
tools/tacli log <i> -g '^hud:'              # one line at attach: ARMED/off, the centre-on observer, the stored percentage
tools/tacli log <i> -g 'k=[0-9.]* s='       # the gui heartbeat carries s= beside k=
```

- **The lever file is read once, at attach.** The in-game row puts a change in force
  immediately (the store writes the live word as well as the file), but `tacli arm` only writes
  the file, so **arming it on a running instance does nothing until you relaunch**.
- **640×480 is always stock**, because Auto's ceiling is 1.0 there, whatever `scale=` says.
- **`L` and `T` must NEVER move; `R`/`B`/`viewW`/`viewH` must.** That split is the whole design
  (gui-renderer 22.6), so it is the first thing to peek at:
  `tacli peek <i> '*0x511DE8+0x37E27:4' '*0x511DE8+0x37E2B:4' '*0x511DE8+0x37E37:4' '*0x511DE8+0x37E3B:4'`
  must read `128`, `32`, `W−128s`, `H−64s`. At 4K Auto: `128 32 3264 1872`. A moved `L` is the
  withdrawn design and means a torn world.
- **`tacli` coordinates are the ENGINE's, and at s > 1 that is NOT where the thing is drawn.**
  The world is drawn shifted by `(128s−128, 32s−32)`, so `roster`'s `screen` — and `pmove:`,
  `click`, `ui show` — are all in engine space and a screenshot will show the unit that vector
  away. `dmove:`/`dclick:` speak device pixels and DO go through the map, so **a device click is
  the only one that tests "what you click is what you see"**.
- **The one check that catches a torn world**: take a unit's `screen` from `tacli roster`, add
  the shift, and **click it in device space** — `keys <i> dclick:X+dx,Y+dy` then `order <i> --sel
  stop`, which reports `1 issued` when it selected. Clicking the UNSHIFTED place must select
  nothing. Use a click, not a hover: `main+0x2CBA` is refreshed by the engine's own GetCursorPos
  polls and drifts back to the screen centre within a second of an injected move, so a hover read
  a moment later is measuring the poll, not your point. A selection persists.
- **`tacli click --device` and `ui click --device` were measured wrong at `s > 1`** before the
  rebuild: they aimed where the gadget would be *unmagnified*. The engine-side conversion
  (`mouse_client_to_game`) does apply the map — a raw `keys <i> dmove:X,Y` in device pixels is
  correct, measured — so what is left is whichever of the two computes its own screen point,
  and that has **not** been re-checked since. Until it is, multiply the
  engine coordinate by `s` yourself: `ui <i> show <gadget>` gives the engine rect centre, and
  the screen point is that times `s` in the panel and the top bar, and
  `H − (H − y)·s` for y in the bottom bar. `uiwalk.py`'s per-stop hit check has the same gap,
  so **run it with `hud.off`**.
- **Every other injected click is unaffected** — `tacli click`, `ui click`, `order`, `eye` all
  speak the engine's own coordinates and never touch the map. Only the `--device` path does.
- **Reading the pointer back is the cheapest check that the map is right**:
  `tacli keys <i> dmove:X,Y` then peek `*0x511DE8+0x2C76:4` / `+0x2C7A:4` (the engine's point)
  and `+0x2CC6:1` (bit0 minimap, bit1 world). At `s = 2.25`, screen (140,140) must read
  engine (62,62) flags 5, and screen (960,600) must read (960,600) flags 6 — the world region
  is the identity at every scale. **`dmove:` is device pixels; `pmove:` is the engine's own
  coordinates** and does not go through the map at all, so `pmove` over a magnified HUD region
  aims at the 1× grid, which is a different point from the one under your finger.
- **Three ways of driving the CAMERA do not work through the harness**, all of them with the
  pass on *or* off, so none is a HUD symptom (measured 2026-09-11/12): injected band-select
  (`down:lbutton` / `mouse:x,y` / `up:lbutton`) selects nothing; arrow keys do not scroll; and
  an injected `click` on the minimap — at the engine coordinates `main+0x142BB` itself reports —
  does not move the camera. **To move the camera, use `tacli eye`**, and note it clamps its own
  x argument at 0, so it cannot test a negative eye. To test a camera BOUND, pin past the edge
  and release: `tacli eye <i> <x> <y>` then `tacli eye <i> --release`, then peek
  `*0x511DE8+0x1431F:4` / `+0x14323:4`.
- **`--res` does not always reach the game.** The in-game resolution is the Screen Size
  (`main+0x37F1B/+0x37F1F`), and `tacli` records the resolution the game actually came up at —
  so once it drops, the next launch re-applies the dropped value and it is sticky. Symptom: you
  ask for 3840x2160 and `hud:`/the rect say 1024x768. Fix by editing `res` in the instance's
  `instance.json` before launching, or drive Screen Size from Options > Visuals.
- **`--shield on` is not a flag** — it is bare `--shield` (and `--no-shield`). `--shield on`
  makes the whole launch fail, and because `tagpu.log` is only truncated by a launch that
  succeeds, the log still holds the PREVIOUS run and reads exactly like a healthy one. Check the
  `launched <name> pid=` line is actually there before you believe an arm list.
- **A cross-build pixel A/B must state which band it is diffing.** At `s = 1` the panel, top
  bar and bottom bar are byte-identical by construction (nothing is written, the shader takes
  the identity path); the world is relaunch noise, and on `selbox-facings` that floor is
  ~3800 px at 1080p — bigger than most differences worth chasing. Diff the bands separately.

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
  reaches the screen thinner and dimmer than the engine's.
- **`tagpu_selgeom.on` is the answer to that, and the second lever to arm under `devres`.** It
  draws the rect as two triangles per edge instead of `GL_LINES`, a band of `w` GAME pixels
  (`arm <i> 'selgeom.on=w=2'`, default 1; `wdev=` states it in device pixels). At `k = 1.5` the
  rect then reaches full colour — 1019 device pixels at ≥ 0.9 coverage against the line path's 5
  — and at 1:1 it is **bit-identical**, so the parity md5 does not move (measured 2026-09-11 at
  `ss = 1`, `ss = 2`, zoom 0.5 and 2.0). The native log line carries
  `selgeom=<w>gpx@1x|@ss` whenever it is armed, and `selgeom.on=main` turns the 1x detour off
  (the A/B for whether that apparatus is still owed — it is: 1320 px at `ss = 2`).
- **A/B-ing the rect needs a fixture that is frozen without pausing.** `tacli keys <i> tab` opens
  the in-game menu and writes PAUSED across the middle of the viewport, over whatever is there.
  Still tanks on `Two Continents` settle by themselves: `scenarios/selbox-facings.json` and
  `selbox-slope.json` both reach a **0-pixel noise floor** within a few seconds and reproduce the
  same md5 across relaunches. Two things still move in a "static" frame — the **cursor sprite**
  animates wherever it is parked (exclude its rect, as `uiwalk` does), and at zoom < 1 a stray
  animating feature can come into view (mask it). Band-select with
  `keys <i> mouse:X,Y down:lbutton mouse:… up:lbutton`; there is no `tacli select`.

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
