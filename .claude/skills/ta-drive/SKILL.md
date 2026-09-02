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

Skirmish settings come from the registry, no clicking: `--map "Two Continents"`,
`--player 2:2:1:1` (`N:controller[:side[:color]]`, controller 0=off 1=human 2=AI,
side 0=ARM 1=CORE), `--los`, `--mapping`. **The registry is the only way** — every
TotalA.exe switch was traced in phase 1.2 and none of them sets a game rule
(`cmdline-options.md`). Raw switches go through `--arg=-t --arg=120` (keep the `=`);
`-r` and `-d` are refused.

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
- **`SELPROV`'s `SELECT` button kills the game** — `Access Violation ... at 0023:00000000`
  in `ErrorLog.txt`, TA's DirectPlay path under wine, reproducible with plain `tacli keys`
  and nothing to do with `ui`. Reading the provider list and moving its selection are
  safe; pressing `SELECT` is not. That is what blocks agent-vs-agent multiplayer.
