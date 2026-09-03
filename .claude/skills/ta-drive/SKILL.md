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
6. **When the human is going to play it, check the window is on their monitor**
   before handing it over — see *Where the window lands*. A game that is running
   perfectly but sits off-screen still answers every `tacli` command and shows
   them nothing, which is indistinguishable from a launch that failed.

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
tools/tacli click t1 320 240                # game coords; --right for orders
tools/tacli roster t1 --json                # units + camera eye
tools/tacli stop t1
```

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

- Clicks need `Interface Type=1` (right-mouse orders) — tacli sets it. Select with
  `click`, order with `click --right`. Classic left-click-order resists posted clicks.
- **Ctrl/Shift/Alt combos land** (since phase 1.1): `tacli keys t1 ctrl+d`,
  `shift+2`, `ctrl+shift+a`. The modifier is held 150 ms because TA *polls* it.
  If a combo does nothing, read the diagnostic the shield logs when the hold expires —
  `shield: vk=17 released after 150ms, polls=2 down=2`. `down>0` means the game saw
  your modifier and the binding is the problem, not the input path.
- Hold anything across frames with `down:<tok>` / `up:<tok>` — keys, or
  `lbutton`/`rbutton`/`mbutton`. Drag-select is `down:lbutton`, `mouse:x,y`,
  `up:lbutton`.
- `tacli eye X Y` pins the camera (writes both eye and scroll-target, else the engine
  fights back); `tacli eye <name> --release` frees it. Read the settled value from a
  `roster` call before doing coordinate maths.
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
- `tacli peek <name> '*0x511DE8+0x2C74:2'` — read game memory from inside the
  process (deref with `*`, `+hex` offsets, `:1|2|4|s<N>|x<N>`). The cheap way to
  answer "did that actually change anything?" without a debugger. Grammar:
  `tagpu/ddraw/inc/tagpu_peek.h`; worked example: `cmdline-options.md` §A/B.
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

## The input firewall (on by default)

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
the log line `owndraw: ARMED … opaque@0x459830=OK` and `OWND … skipped>0`.

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
`nocapture`, `noselbox` — on its own patch, `markown.on`, which tacli auto-arms at launch
when `mark.on` exists. It covers health bars, group digits, order/waypoint/build-queue
markers, range circles, the build-cursor footprint and the drag band box, and it stops the
engine drawing its own copy of the selection rect underneath ours. Two mechanisms:
**health bars are re-drawn** from unit state (the engine's own loop walks HotUnits, culled
to the *unzoomed* viewport, so a capture would leave a zoomed-out view's outer ring bare),
**everything else is captured** — the engine draws it into a scratch buffer of ours and we
replay that buffer through the zoom transform, so parity is exact including text. Verify
`markown: ARMED (hook8/hook9/transp x2/selbox x2 redirected …)`, `MARKOWN capture=1
bars-skipped=1 selbox=1`, and `mark: bars=N prefog=… postfog=…` (`log`).

Three things to know:

- **Health bars need the `damagebars` registry option**, which is *off* when the value is
  missing — that is the engine's own gate (`main+0x37F06` bit0) and we honour it. Set it
  before launch under `HKCU\Software\Cavedog Entertainment\Total Annihilation`.
- **Order markers only draw while SHIFT is HELD** — the engine samples its own hotkey
  `0xF9`, which this build resolves to `GetAsyncKeyState(VK_SHIFT)` (jump table at
  `0x4C1C48`, verified). `tacli keys <i> down:shift` … `up:shift` around a shot. We call
  the engine's sampler rather than reading the key ourselves, so a different keymap cannot
  make us disagree with it.
- **`passive` is the A/B lever** and hands *everything* back — bars, capture and the
  selection rect — so the engine draws the lot while we still gather and count.

**Zoom is `tagpu_zoom.txt` in the gamedir** — a bare float 0.25–8.0, re-read every frame;
delete the file for 1×. Write it **atomically** (temp + rename) or the DLL reads a torn
value. Since G13e the input follows it: a click lands on the world point it is drawn
over, the engine's cursor is moved back under the pointer, and the minimap's view box and
the scroll rate scale with it. `tacli arm <i> zoom.on` (at launch, like every other
code-patching pass) additionally installs the one engine patch it needs — the minimap
view rectangle — and logs `zoom: minimap view rect ARMED`. Everything else needs no arm
at all and is inert at 1×.

Two things to know when driving zoomed:

- **`tacli click` takes the position ON SCREEN**, the same as your eyes — the transform
  is applied on the far side of `g_ddraw.cursor`, so the injected path and the human's
  mouse cannot disagree.
- **At zoom < 1 the outer ring of the view is DISPLAY-ONLY.** The engine can only name
  screen positions inside its own 1× viewport, so the world the zoom-out reveals beyond
  it has no address: a click there is **dropped** (the selection is left alone rather
  than being moved to whatever sat at the 1× position), and the captured marker layers
  stop at the same edge. At 0.5× the addressable region is the central half of the frame
  in each axis. Zoom ≥ 1 has no such limit.

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

- `pkill -f TotalA.exe` kills your own shell (the pattern matches the wrapper).
  Use `pkill -x` / `pgrep -x`, or just `tacli stop`.
- `tagpu.log` contains binary bytes: always `grep -a` (tacli's `log`/`wait` handle it).
- Two X windows share the title `Total Annihilation` (frame + client), and the user's
  browser/Discord windows match the *substring* — tacli matches exact title + pid.
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
