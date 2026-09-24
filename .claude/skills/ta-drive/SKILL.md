---
name: ta-drive
description: Launch and drive Total Annihilation from the CLI with tacli — isolated parallel instances, windowed on the real desktop, silent, no intro, scripted keyboard/mouse/camera, skirmish presets, log/roster queries, and arming the renderer's passes. Use when asked to run, start, play, test, or drive the game, to run several game sessions in parallel, or to arm, observe or measure a pass on a running game.
---

# Driving TA from the CLI (tacli)

`tools/tacli` runs the game as **isolated instances**: each has its own gamedir, wine prefix,
wineserver, window, config and logs. Several agent sessions can each own one instance without
colliding, and the human keeps using the desktop meanwhile.

This file states the **present** behaviour of the tool and the DLL. How it got that way is in
the research notes and in `git log`, not here (*Maintaining this skill*, at the end).

**Read a reference file when you need it, not before:**

| file | read it when |
|---|---|
| `references/levers.md` | you arm anything beyond the bench line, drive zoom or the camera, or read a `tagpu.log` heartbeat |
| `references/measuring.md` | before any A/B, pixel diff, cross-build comparison or frame-time figure |
| `references/ui-layer.md` | the question is the HUD, the shell, the cursor, text, the minimap, HUD scale or a windowed `k` |
| `references/modules.md` | extra weapons, the COB trace, multiplayer, the render-options screen and the GPU row |

Design and mechanism: `research/notes/tacli-design.md` (the tool), `input-firewall.md` (input
isolation), `windowed-mode.md` (display bugs), `cmdline-options.md` (every launch knob),
`scenario-format.md` (scenarios). Capture and video: the **ta-capture** skill.

## Rules of engagement

1. **One instance per session.** Name it after your task (`tacli launch fogtest`). Never drive
   an instance you did not launch — another agent may own it.
2. **Never use xdotool for keys or clicks.** All input goes through tacli, which writes the
   in-process token file the DLL consumes. X injection is unreliable and has landed keystrokes
   in the user's unlock dialog (memory: `ta-input-injection-safety`). xdotool is fine for
   *reading* window geometry and for capture. **Never run `xrandr` against the live display**,
   never move the human's pointer, never activate windows to "test" something.
3. **Silence is the default** (`NoDirectSound`, six registry values, no `music/`). Pass
   `--sound` only when asked.
4. **Clean up**: `tacli stop <name>` when done, `tacli rm <name>` when the instance has no
   further use. Every running game is a GPU client.
5. **Rebuilt the DLL? Relaunch.** `tacli launch` copies `ddraw.dll` into the instance from
   **the tree whose `tools/tacli` you invoked**, not your cwd: `cd <tree> && tools/tacli …`, or
   you test the main checkout's binary, and the symptom is modules that never log `ARMED`. When
   an armed module does not log, `md5sum` the instance's `gamedir/ddraw.dll` against your build
   (two builds of one tree differ in the PE timestamp, so an md5 written down earlier means nothing).
6. **When the human is going to play, check the window is on their monitor** before handing it
   over (*Where the window lands*). A game running off-screen answers every `tacli` command and
   shows them nothing.
7. **Arm every pass unless the launch is a measurement.** "Launch the game" means the game with
   the work in it. A bare `tacli launch` is the 1997 renderer — a control, not a demo. An
   instance that exists to measure one pass arms only that pass.

## Where the window lands

**The display is pinned when the instance is created; `--display` on a later launch does not
move it.** `tacli create` records `TACLI_DISPLAY`, else the inherited `DISPLAY`, else a live
socket in `/tmp/.X11-unix`, skipping virtual servers (Xvfb/Xephyr/Xnest) because parallel
sessions leave 3840x2160 Xvfb displays around. Read `display` back with `tacli ls --json` before
telling the human it is ready. An instance whose recorded display has died does not look bricked:
the DLL loads and logs its hook lines, then TA stops at window creation with the modal **"Error:
Environment Initialization Failed! Check your DirectX setup"**, no `ErrorLog.txt`, no exit, and
`tacli` reports *"exited during launch before showing a window"*. Any DLL reproduces it. Check
the recorded display against a live server (`xdpyinfo -display <d>`), then make a **new**
instance with `tacli create <n> --display <live>` — `instance.json` is shared state another
session may own. A hand-run `wine TotalA.exe` without `WINEDLLOVERRIDES=ddraw=n,b` loads wine's
builtin ddraw and fails with the same dialog, so it proves nothing.

**Every instance prefix shares one `user.reg` inode** (the `cp -al` clone never breaks it), so
the prefixes are isolated in everything except the registry. Never "fix" a launch by writing
registry values.

**The tile.** Instances are laid out in a grid so parallel windows do not stack; slots are held
by **running** instances, `--slot 0` claims a cell explicitly. A window created off-screen is
left **unmapped** by GNOME (`xprop -id <wid> WM_STATE` reads `Withdrawn`); a relaunch is the
clean fix. The grid spans the whole X screen from (0,0), so on a multi-head desktop the window
lands wherever the grid falls; to park it elsewhere, read `xrandr --query` and
`xdotool windowmove <id> <x> <y>` **on the id from `tacli ls --json`**, never from a title search
(a terminal whose title holds the command line matches too). `scenario load --restart`
relaunches from the recorded tile and undoes a park.

**The title names the build and the instance**: `tacli launch` writes `tagpu_title.txt` and the
DLL appends it, so the bar reads `Total Annihilation - wt:<branch> | tacli:<instance>` — the
branch whose DLL is pinned, and the name every other command takes. `--title "text"` replaces
the whole label, `--no-title` drops it, `--title auto` returns to the default; all sticky.
`tacli ls --json` reports it as `window_title`; `tacli:play1` is a substring of `tacli:play10`.

Handing the game over is `tacli launch <name> --no-shield`, or `tacli shield <name> off` on a
running one.

## The loop

```bash
tools/tacli launch t1 --res 1024x768        # creates on first use, ~2s to window
tools/tacli ls                              # names, pids, windows
tools/tacli ui t1 click SINGLE              # drive menus by gadget name (`ui t1` lists them)
tools/tacli shot t1 -o /tmp/where.png       # the engine's own frame: see where you are
tools/tacli order t1 --sel move pos 1988 1716   # orders: world coords, no mouse
tools/tacli click t1 320 240                # game coords; the UI, not orders
tools/tacli roster t1 --json                # units + camera eye
tools/tacli stop t1
```

## Ordering units: `tacli order`, not clicks

**`tacli order` is the way to command units. Reach for a click only when the thing under test
IS the mouse.** The verb names the order and the target outright and goes to the engine's own
order constructor through the scenario applier, so there is no screen in it anywhere:

```bash
tools/tacli order t1 --sel move pos 1988 1716          # everything selected
tools/tacli order t1 --unit 2 --expect ARMCOM attack pos 3160 1200
tools/tacli order t1 --unit 2 --expect-target CORCOM attack unit 251
tools/tacli order t1 --unit 2 reclaim pos 1700 1500    # a wreck: name where it is
tools/tacli order t1 --unit 2 stop
```

- **World coordinates, not pixels** — immune to zoom, resolution, the camera and the addressable
  ring. No selection, no camera work, and the order type is yours rather than the engine's guess.
- Orders: `move attack defend repair patrol reclaim capture load unload blast stop mobilebuild`
  (`guard` is an alias of `defend`). There is no attack-move in TA; `attack pos <x> <y>` is the idiom.
- **Not a validator.** A nonsense order is issued, not caught, so `1 issued` is not evidence the
  unit did anything — read the roster. **Not ownership-checked**: an enemy unit takes the order.

**Naming the unit.** `--unit` takes the `engine_index` from `tacli roster`, which the engine
recycles when a unit dies, so **pass `--expect <TYPE>` whenever you pass `--unit`**: the DLL
checks the slot still holds that type before it orders. `--expect-target` guards a `unit`
target; `--sel` asks the engine what is selected. Failures are loud and exit non-zero.

**Drive menus by name.** `ui click SINGLE`, `ui click Skirmish`, `ui click Start`, then
`tacli wait t1 'alive=[1-9]' --timeout 150`. Each click auto-waits for the gadget and reports the
screen it landed on; blind `keys` sequences drop their first token, `ui click` does not.
`tacli scenario load` does that whole path, map and players included.

**Leaving a game for the main menu is four steps, and none of them is Esc** (Esc in game does
nothing). The exit button sits on `ARMOPT.GUI`, which **Tab** raises over the world:

```bash
tools/tacli keys t1 tab tab          # TWICE: a keys invocation drops its first token
tools/tacli ui   t1 click EXIT       # ARMOPT.GUI -> EXITMENU.GUI (also RESTART, EXITGAME, CANCEL)
tools/tacli ui   t1 click MAINMENU   # -> YESORNO.GUI, "Surrender this battle…?"
tools/tacli keys t1 y y              # CHOICE1 — the click on it is unreliable, the key is not
tools/tacli ui   t1                  # confirm: MAINMENU.GUI 640x480 (the shell is always 640x480)
```

**Skirmish settings come from the registry, no clicking**: `--map "Two Continents"`,
`--player 2:2:1:1` (`N:controller[:side[:color[:metal[:energy]]]]`, controller 0=off 1=human
2=AI, side 0=ARM 1=CORE; an empty field leaves that key alone), `--los`, `--mapping`,
`--unit-limit` (20–1500). Metal and energy are the starting resources **and set storage** — the only way
to set them. All sticky per instance. No `TotalA.exe` switch sets a game rule; raw switches go
through `--arg=-t --arg=120`, and `-r`/`-d` are refused. **`Mapping` and `LineOfSight` are
decided by the SKIRMISH gadgets** (the registry only saves the last choice), so `scenario load`
sets them itself and starts every game **Mapped** (`--mapping 0` opts out) — a scenario places
units by world coordinate, and unmapped they sit in black. `--los 0` (Permanent) turns the grey
fog off, a play setting rather than a default; `tacli switches <i> radar=on` shows enemy units
as well. By hand: `ui t1 set Mapping 1`, `ui t1 set LineOfSight 0` before `Start`.

## Input details that cost time to learn

- **Every instance runs `Interface Type=1`** (right-mouse orders); tacli sets it on every launch.
  Select with `click`, order with `tacli order`. `click --right` orders and is the way to test
  *that the mouse orders*; a plain `click` on the world **deselects** unless something selectable
  is under it. Stock TA switches its contextual cursor off at type 1; the DLL patches it back on,
  and `tacli arm <i> curs.off` before launch restores the engine's own behaviour for an A/B.
- **`tacli click X Y` is positionally correct on its own**: the DLL posts a MOVE to (x,y), the
  button, then a MOVE back to the screen centre. So after any click the pointer is at the centre,
  and `wheel:` notches or a position-less `click`/`rclick` land there unless you `pmove:X,Y`.
- **Key tokens**: names (`space return esc tab plus minus up down left right backspace delete
  home end y …`), combos `ctrl+d`/`shift+2`/`ctrl+shift+a` (the modifier is held 150 ms because
  TA polls it), `char:c`, `down:<tok>`/`up:<tok>` to hold keys or `lbutton`/`rbutton`/`mbutton`
  across frames, `mouse:x,y`, `pmove:x,y`, `click:x,y`/`rclick:x,y`/`pclick:x,y`, `wheel:n`, and
  `dmove:x,y`/`dclick:x,y` in **device** pixels. Drag-select: `down:lbutton`, `mouse:x,y`,
  `up:lbutton`.
- **Injected modifiers need the shield ON.** A polled modifier reaches TA only through the
  shield's `GetAsyncKeyState`; with `--no-shield` your `down:shift` is invisible, so anything
  gated on a held modifier (the order markers, `Ctrl+C` follow) is untestable with the shield
  off. Measure first, hand over after. When a combo does nothing, read the diagnostic
  `shield: vk=17 released after 150ms, polls=2 down=2`: `down>0` means the game saw it and the
  binding is the problem, `polls=0` means the game never asked.
- **`tacli eye X Y` pins the camera** (eye and scroll target together), applied by the game thread
  at the top of every in-play draw; `tacli eye <i> --release` frees it. `scenario load` pins the
  camera, so release it before wheeling.
- **`roster` is a snapshot the game thread logs**: the unit block every 5 s, the `units:` header
  every 500 ms, `mouse:` every 250 ms. It is empty for a second or two after `scenario load` —
  read it in a retry loop, and re-read after moving the camera. Its `screen=` is the **1x
  projection**, so at any other zoom it is not where to click. A moving unit walks between the
  read and your click, and a missed selection is silent: re-read right before clicking.
- **A right-click on water is rejected for a ground unit** and draws no marker, which looks like
  a broken marker pass. Pick grass (`g > b + 30` in a shot).
- **Game speed is `keys <i> plus`/`minus`** (up to +10), and it is **shared registry state that a
  launch does not reset**: `gamespeed` lives in the one `user.reg` every prefix shares, so one
  session pressing `+` leaves every later launch at that speed. It multiplies the sim tick (30/s
  at 10, 60/s at 20) while the picture still changes 30 times a second. Read it before any
  measurement whose answer is a rate: `wine reg query` under the instance's `WINEPREFIX` for
  `gamespeed` in `HKCU\Software\Cavedog Entertainment\Total Annihilation` (`0xa` is normal).
  `Gamma` lives in the same shared file and scales the presented palette (`references/ui-layer.md`).

## Driving the UI (menus, options, build panel)

`tacli ui` is a Playwright-style layer over TA's own gadget tree: **snapshot the screen, then
act on a gadget by name.** It reads the live gadget array from inside the process, on demand.

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
  7  button                    586,94      inactive            50
```

- **Everything auto-waits** (5 s, `--timeout`, `--no-wait`): for the gadget to exist and be
  actionable, then for a consequence, which it reports (`screen SINGLE.GUI -> SKIRMISH.GUI`,
  `stage 0 -> 1`, `engine confirmed`).
- **It refuses rather than guessing.** Absent, `inactive`, `grayed` or zero-sized gadgets error
  with the reason and a list of what is on screen; an ambiguous name is an error too
  (`button:NAME` or `#7`). TA uses both `active=0` and `grayed=1` for "you cannot have this".
- **`#index` is the only way to reach an unnamed gadget** (a scrollbar's arrows have no name).
  It is a position in the current screen, not a handle; re-read it after any screen change.
- **`grp`** appears only when `assoc` means something (a radio group, a slider with its list and
  arrows); same number = acting on one moves the others. **`enter=`/`esc=`** are the screen's own
  bindings; backing out is `click PrevMenu`.
- **In game, select a unit first.** With nothing selected the top GUI is `ARMMAIN2.GUI`; a
  builder pushes its build page (`ARMCOM1.GUI`: `ARMSOLAR`, `ARMLAB`, `ARMMOVE`/`ARMSTOP`/
  `ARMATTACK`, pagers `ARMPREV`/`ARMNEXT`). **The in-game menu is Tab** (`ARMOPT.GUI`); both
  save and load land on `LOADGAME.GUI`.
- **A build button reports "delivered (no GUI-visible change)" — that is success**: it changes
  the cursor mode. Then place it: `keys t1 mouse:X,Y`, `click t1 X Y` (left; right cancels). A red
  footprint means the site is blocked.
- **`ui` is the gadget layer only.** The map, minimap and resource bars are not gadgets.
- **Lists read out and can be picked** (`select <list> <text|#index>`); an off-screen row is
  walked to with the arrow keys, ~8 s for 99 maps (raise `--timeout`). **Map and campaign
  selection are ordinary text lists**, so `Skirmish → SelectMap → select "<map>" → LOAD` works on
  a running instance, which `--map` cannot. `select` refuses a separator or a disabled row;
  `items unknown` is a picture list. **Sliders** take the engine's value (`val=32/64`).
- **Click a field before you fill it.** An unfocused field turns your text into quickkeys. `fill`
  types through `WM_CHAR`, reports what the field kept, and refuses text over `maxchars`.
  `hover` and `help` rarely show anything — the engine zeroes most tooltips.

Changing `tacli ui`? `python3 tools/test_tacli.py` covers the selector, geometry, token and
rendering logic offline; everything else needs a real instance.

## What exists in this game (catalogues)

```bash
tools/tacli units    t1 --limit 0    # 279 unit types in stock TA: ARMPW, ARMCOM, CORAK…
tools/tacli features t1 --json       # 570 features — this is where wreck names live
tools/tacli maps     t1              # 99 map names, off SELMAP's own list
tools/tacli catalogue t1             # what this instance has cached
```

- **Read `units`/`features` in a game, not at the menu** (at the shell only the names are
  trustworthy, and the table is re-indexed when a game loads). **`maps` is the opposite** — it
  walks `SINGLE → Skirmish → SelectMap` in the shell.
- **Wreck names are lowercase** (`armlab_dead`), unit names uppercase (`ARMPW`). The Commander
  has no corpse in stock TA.
- After adding or removing archives, delete the instance's `catalogue.json` or `scenario load`
  refuses the new type at validation.

## Scenarios (JSON situations)

A scenario is a JSON *situation* — 200 units fighting, a wreck of a chosen type, the camera on
it. **`tacli scenario load` goes from nothing to that situation in one command**, name-keyed and
resolved against the live game.

```bash
tools/tacli scenario load     t1 200v200        # launch → menus → live → spawned → camera
tools/tacli scenario list                       # what is in scenarios/
tools/tacli scenario validate 200v200 --instance t1   # schema + this game's catalogue
tools/tacli scenario expand   200v200 --wire    # the flat list, and the file the DLL reads
tools/tacli scenario apply    t1 200v200        # spawn it into a game already running
tools/tacli switches t1 shootall=on noshake=on  # the SoftwareDebugMode bits, live
```

- **`load` is the whole trip; `apply` is the mutation.** `load` launches with the file's `setup`
  (map, resolution, players, unit limit), drives the menus, waits for a world, checks the map TA
  actually loaded, then applies; on a running instance it refuses without `--restart`. `apply`
  stacks onto whatever is on screen and needs a **running game** (at `MAINMENU` it waits 600
  frames and says so). Explicit flags (`--map`, `--res`, `--unit-limit`) win over the file.
- **`shootall` (on by default) makes idle units engage enemy *buildings* in range**; turn it off
  when units must ignore structures. `noshake` kills screen shake, which matters for capture.
- **It validates three times and creates nothing if anything fails**: schema, the cached
  catalogue, a resolve pass inside the game. `on_error: "skip"` opts into best effort.
  `apply --json` reports, per entity, the engine index and where it landed.
- **`clear_existing` defaults true** and removes the starting commanders. A player with no
  units is a defeat (`ENDMSN.GUI`) — give both sides something. A slot with `controller: "off"`
  must own nothing (validation refuses it; `"ai"` is the idle side).
- `roster`'s `idx=` is the same recycled `UnitInGameIndex` `apply` returns, handed out in
  per-player blocks of the unit limit. Ship reusable situations in `scenarios/`; never hand-write
  the wire file.

Design and engine recipe: `research/notes/scenario-format.md`.

## Observing

- **`tacli shot`** — TA's own 8bpp frame, on every renderer. It is the **golden source**: the
  engine goes on rasterising its whole frame so our passes can be checked against it, but **no
  pixel of it reaches the screen**. So "it is in the shot" never means "it is on the screen" —
  and the replay refuses the engine's **world**, so anything handed back to the engine inside the
  viewport lands in the shot, not on the window. With the play arm set the shot shows units,
  minimap, sidebar and cursor but **no terrain** (`terrown` key-fills the viewport), minus what
  `markown` skips; **`terrown.off mark.on=passive` hands it all back** (`references/levers.md`).
- **The presented frame** is the window: `import -window <id> out.png` with the id from
  `tacli ls --json`. No `tacli` verb captures it in-process.
- **The census** is the first thing to read when something is missing:
  `tacli log <i> -g 'vk: census'` → `census: frame N: 6 pass(es) drew and 0 claimed (terr=1
  feat=1 unit=1 fx=1 mark=1 scaf=0 gui=1 fps=0)` is the play set. `mark=0` is the marker pass
  standing down; fewer passes right after a load is the level not being up yet.
- **`tacli crash <name>`** — the last crash TA recorded, **symbolised** from
  `tools/ta_symbols.txt` (the nearest preceding name: a neighbourhood, not a signature).
  `launch`, `wait` and `scenario apply/load` poll `ErrorLog.txt` and fail fast with the fault
  address. The file is per instance and rotated at every launch.
- **A sim tick that stops while every thread sleeps, with no `ErrorLog.txt`, may be a wild jump,
  not a pause.** `tacli peek <i> '*0x511DE8+0x38A47:4'` twice, four seconds apart, is the test
  (30/s at gamespeed 10). ptrace is off on the reference setup, so bisect the change
  (memory: `ta-engine-hook-traps`).
- **`tacli peek <name> '*0x511DE8+0x2C76:4'`** reads game memory from inside the process (deref
  `*`, `+hex`, `:1|2|4|s<N>|x<N>`; grammar in `tagpu/ddraw/inc/tagpu_peek.h`). Camera: eye
  `+0x1431F`/`+0x14323`, scroll target `+0x14327`/`+0x1432B`, view size `+0x37E37`/`+0x37E3B`,
  screen size `+0x37E1F`/`+0x37E23`, mouse `+0x2C76`/`+0x2C7A` (y is at `+0x2C7A`).
- **`tacli log <name> -g <regex>`** (a Python regex: `(a|b)`, not `a\|b`) and
  **`tacli wait <name> <regex>`**. The log is `gamedir/log/tagpu.log` and **rotates** (every
  launch, and at 16 MB; `tagpu.1.log` … `.10.log` behind it): read a run with
  `tools/talog.py run <gamedir>`, slice one with `talog.py mark`/`since`, never by byte offset
  (`research/notes/logging.md`). It holds binary bytes: `grep -a`.
- Video and frame-by-frame analysis: the **ta-capture** skill.

## The input firewall (on by default)

**"Human controllable" / "let me play it" / "hand it to me" means SHIELD OFF.** An instance with
the shield on answers every `tacli` command and ignores the human's keyboard and mouse entirely,
which reads to them as a game that is running but broken. `--no-shield` at launch or
`tacli shield <i> off` live, check `tacli ls` says `OPEN`, and check the window is on their
monitor — the two together are what "controllable" means. While armed, the game sees only what
tacli injects; the human can click and type across your window without perturbing your test,
and your instance does not need focus. Details: `research/notes/input-firewall.md`.

## Arming the passes

```bash
tools/tacli arm t1 native.on=all owndraw.on      # one file per lever, the value is its text
tools/tacli arm t1 'native.on=all wrecks'        # several tokens: quote them
tools/tacli arm t1 native.on=off                 # off | 0 | false | - unlink the file
```

`arm` writes `gamedir/tagpu_<name>` — give it `gui.on`, not `tagpu_gui.on`, or you create a file
nothing reads. Removal survives a relaunch. **Remove a lever with `arm`, not `rm`**: a wrong path
succeeds silently and you measure with the lever still armed.

**Two configurations.** The shipped DLL arms the **play defaults** by itself when no arm file
exists (`tagpu_opt.c`; the table is in `references/levers.md`). A `tacli` instance **opts out**:
`launch` and `scenario load` write `tagpu_defaults.off` unless given `--defaults` (sticky;
`--no-defaults` back), so on a tacli instance only the arm files count and a bare launch is the
stock control. `--defaults` is the player's configuration and the one to test a release with;
under it a pass is turned off with `tacli arm <i> <pass>.off`; the first `opt:` line says which
way it went. Menu rows live in `impure.cfg`, honoured only under `--defaults` (`references/modules.md`).

### The default arm set

**Unless the launch is a measurement, arm all of it, before the launch:**

```bash
tools/tacli arm <i> 'native.on=all wrecks' terr.on feat.on fx.on sfx.on \
                    mark.on order.on zoom.on vpwide.on gui.on
tools/tacli launch <i> --no-shield --res 1920x1080
```

Units and wrecks, terrain, features, effects, particles, the markers and the shift-held order
overlay, zoom with BAR's camera, the wide viewport that makes zoomed-out clicks
land, and the UI layer. Classic++ (restored true colour, lit, shadowed) is one switch,
`classicpp.on`, and its `assets=` knob is what feeds the restorer — world, HUD and shell.
`ghost.on` is a play default and off this bench line because its posed draws perturb a
measurement.

- **The code-patching halves are auto-armed and must exist at DLL attach.** `owndraw`,
  `terrown`, `featown`, `fxown` and `markown` install engine detours once, at attach; `launch`
  and `scenario load` write each when its pass is armed and print `auto-armed … must be
  installed at launch, not after`. Arming one on a running instance does nothing; the same holds
  for every lever whose `ARMED` line prints at launch. The draw tokens re-read live.
- **The `*own` halves hole the golden source** (they stop the engine drawing what our pass draws),
  so arm them for a frame-time figure, never for a comparison; a stale `owndraw.on` with
  `native.on` cleared makes the engine draw no units at all. **`terrown` is a play default**, so
  the reference is key-filled with no terrain until `terrown.off` — which costs the zoomed-out fog.
- **`<pass>.off` does nothing while `<pass>.on` exists.** Remove the arm instead
  (`classicpp.on=off`).
- **Health bars are a registry value, not a lever**: no bars and no group digit until
  `damagebars` is 1 under `HKCU\Software\Cavedog Entertainment\Total Annihilation`.

Every lever, its tokens, its verify line and the heartbeat fields that must read zero:
`references/levers.md`.

## Zoom and the camera

Zoom has two levers, and the file wins. **`tagpu_zoom.txt`** in the gamedir is a bare float
0.25–8.0, re-read every frame, written atomically — the scripted lever, and it does not move the
camera. **The mouse wheel** is what the player uses when the file is absent: one notch in is
x1.163 and one out x0.877, eased over 250 ms. A notch **in** zooms to the cursor, so it moves the
eye; a notch **out** pulls back from the view centre and never moves it. The log line at the end
of a gesture is `zoom: wheel +120 -> 1.163, landed 251.7 ms after the last notch`. Both levers
need the mouse->world repair, which `zoom.on` or `vpwide.on` installs at launch; with neither the
level is pinned at 1.0 and the log says so.

- **Aim it.** `--at` is a `pmove:` first; notches over the side panel or a menu do nothing, and
  the log says which gate refused. `tools/tacli wheel <i> 1 --at 576 384` (the viewport centre at
  1024x768) is the camera-neutral control.
- **A round trip does not come back to 1.0** (in then out is 1.020): write `1.0` to
  `tagpu_zoom.txt`, then delete it.
- **Re-read the eye after any wheel** — the game thread applies the delta at its next in-play
  draw. An off-centre notch in also **releases a camera follow** (`Ctrl+C`); read `main+0x142F3`
  (0 = nothing followed) rather than assuming.
- **At zoom < 1 the outer ring needs `vpwide.on`** at launch, or a click or band-box drag out
  there is dropped. `tacli click` takes the position **on screen**, the same as your eyes.
- **Edge scroll fires under injected input on the exact edge pixel only** (`mouse:1023,400` at
  1024x768; `mouse:1020,400` does nothing). Arrow keys do not scroll under injection.

The camera range, the minimap jump, the fog grid's rebuild rule, which dialogs keep 1:1 clicks
under zoom and `wheel.off`: `references/levers.md` §"Camera, viewport and fog".

## Things that will bite you

- **`mark.on=noselbox` takes the selection rects off the window.** The marker pass draws them;
  the token stops it, and the engine's own boxes (at the unzoomed position) then land in
  `tacli shot` only. A selected unit with no rect on screen is this lever, not a bug.
- **A one-frame artifact is not findable with a screenshot.** Record the window losslessly
  (`ffmpeg -f x11grab -window_id <id> -framerate 60 -c:v libx264rgb -qp 0 out.mkv`) and scan
  every frame (the ta-capture skill). `-window_id`, never a screen region.
- **A launch or wait that "timed out" has usually crashed.** The commands check `tacli crash`
  for you; a hand-rolled poll will not.
- **`pkill -f TotalA.exe` kills your own shell** (the pattern matches the wrapper): `pkill -x`,
  or `tacli stop`. **`pgrep -x TotalA.exe | head -1` can name a zombie**; ask `tacli ls`.
- **Instances live in the MAIN checkout's `tagpu/instances/`**, never under a worktree. Read the
  gamedir out of `tacli ls --json`.
- **`--shield on` is not a flag** — it is bare `--shield` / `--no-shield`. A failed launch leaves
  the previous run's `log/tagpu.log` in place; check the `launched <name> pid=` line first.
- **`--res` does not always reach the game.** `tacli` records what the game actually came up at,
  so a dropped value becomes sticky; fix `res` in `instance.json`, or drive Screen Size from
  Options > Visuals.
- **A bare `tacli launch` writes no `ddraw.ini`.** `--res`, `--window`, an explicit `--maxfps`,
  or an off-screen recorded tile rewrite it (`renderer=vulkan`, the tile, the cap);
  `scenario load` rewrites it whenever the scenario carries a resolution. Check the ini after the
  run, not before.
- **`maxfps=0` is unlimited**, a sticky launch knob (`--maxfps 0`), not an edit — the next
  rewrite overwrites a hand edit. Two paths that both hold 60 fps have both hit the cap.
- The launch briefly warps the pointer (a wine quirk); tacli restores it (`pointer_restored`).
- Monitors blanking? Both known causes are in `windowed-mode.md`; read it before theorising.
- **A scratch worktree cannot run tacli** (`create` wants the gitignored wine prefix template).
  Make the instance from a real checkout; to run a foreign build, `cp` its `ddraw.dll` over
  `<gamedir>/ddraw.dll` and launch with `--keep-dll`.

## Multiplayer: two instances in one game

```bash
tools/tacli launch h1 --dplay --free-dplay-port     # the host
tools/tacli launch j1 --dplay                       # the joiner
tools/mp_lobby.sh h1 j1 'Two Continents'            # menus -> battle room -> live
```

`--dplay` puts Microsoft's DirectPlay into that instance's prefix (wine's builtin cannot create
a session); `--free-dplay-port` kills a stale `dplaysvr.exe`, which owns UDP 47624 machine-wide —
on the **hosting** launch only, and never by hand while another agent's game is hosting.
`scenario apply` on the host replicates its units to the joiner. The lobby facts and the
provider-row crash: `references/modules.md`.

## Maintaining this skill

This file is a **reference to the present**, not a journal, and it is loaded whole on every
"run the game" request. It stays useful only if every edit follows these rules:

1. **State the present, in the present tense.** No dates, no landing ids, no "since G13x", no
   "used to". What changed and when is the research note's and the commit message's job.
2. **Corrections replace text; they are never appended to it.** If a sentence is wrong, delete
   it. No `[CORRECTED]`, `[HISTORY]`, `[SUPERSEDED]`, "an earlier draft said" — that prose was the
   bulk of a 285 KB file that described three different moments at once.
3. **A lesson about a landing goes in the note, a rule about driving goes here.** "This cost gate
   3a a round" is a story; "arm one pass's `.ab` at a time" is a rule. Keep the rule.
4. **Every lever, tool, scenario and log line named here must exist in the tree.** Run
   `.claude/skills/ta-drive/scripts/check.sh` before committing: it greps every
   `tagpu_<name>.<ext>` this skill and its references mention against the sources, every
   `tools/…` path against the tree, every file for history markers, and the main file's length
   against rule 6. A lever that no longer exists is deleted, not marked gone (the one list of
   dead names lives in `references/levers.md` and is exempt).
5. **Cite functions and log lines, not line numbers.** A line number is a fact with a version.
6. **The main file stays under 500 lines.** Detail goes into `references/` behind a one-line
   pointer that says *when* to read it. A reference file over 300 lines gets a table of contents.
7. **Read what you wrote as the next session will**: one pass, no context, present tense. If a
   paragraph needs the history to make sense, it belongs in the note.
