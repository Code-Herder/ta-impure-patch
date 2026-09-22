# The UI layer, HUD scale and windowed `k`

Read this when the question is the HUD, the shell, a dialog, the cursor, text, the minimap, the
HUD's size, or a window that is not the game's resolution.

1. [What the layer is](#what-the-layer-is)
2. [The hook hosts tacli](#the-hook-hosts-tacli)
3. [Verbs and tokens](#verbs-and-tokens)
4. [The heartbeat](#the-heartbeat)
5. [The palette and Gamma](#the-palette-and-gamma)
6. [Cursor, text, minimap](#cursor-text-minimap)
7. [`uiwalk.py`](#uiwalkpy)
8. [HUD scale](#hud-scale)
9. [Windowed `k`](#windowed-k)

## What the layer is

The in-game panel, build pages, bars, option screens, chat, popups, the cursor and the whole
shell are drawn by **our** layer: the engine's own draw calls are captured as ops and replayed
into twins of its surfaces, which the Vulkan lane composites over the world at device resolution
(`research/notes/gui-renderer.md`). **No pixel of the engine's frame reaches the screen** — its
frame is kept only as the golden source `tacli shot` reads.

**The replay carries the engine's UI and NOT its world.** Ops
recorded while the engine is inside either of its two world spans are stamped and refused, so the
engine's feature sprites no longer land on top of ours and its whole-surface clear no longer
blacks the viewport; the viewport's erase is emitted by the layer itself, once per publish window
that held an in-play flip, and a whole-surface chrome fill is cut into bands around the viewport
instead of covering it. The consequence to remember: **any configuration that hands a world draw
back to the engine shows it in `tacli shot` and not on the screen** — `references/levers.md`, the
marker passes, has the list and the `worldphase.off` escape hatch. `gui.on` is a play default; a `tacli`
instance arms it in the bench line. Without it the game has no HUD, no sidebar, no minimap, no
cursor and no shell — which is the A/B, and still a drivable instance.

## The hook hosts tacli

The observer on the engine's flip (`0x4C63A0`) installs at DLL attach **whenever the engine's
bytes match, whatever `gui.on` says**, because every on-demand `tacli` verb (`ui`, `peek`, the
catalogues, `weapons`, scenario detection, the key and click injection, `shot`) is dispatched from
inside it. `gui.on` gates only the 17 producer leaves, the op queue, the census and the draw. So
`gui.off`, `tacli gui <i> remove` and a bare launch all leave the instance fully drivable.

The boot line says which case you are in; **grep for `no tacli verb can answer`**:

| boot line in `tagpu.log` | layer | drivable |
|---|---|---|
| `gui: NOT armed — engine bytes differ at the flip 0x4C63A0 … no tacli verb can answer` | no | **NO** |
| `gui: NOT armed — the flip observer refused to install … no tacli verb can answer` | no | **NO** |
| `gui: trigger host only (tagpu_gui.on is not on) …` | no | yes |
| `gui: UI layer NOT armed — engine bytes differ at a watched leaf …` | no | yes |
| `gui: UI layer NOT armed — no arena …` | no | yes |
| `gui: ARMED flip@0x4C63A0=1 leaves=17/17 …` | yes | yes |
| `gui: FAILED flip@0x4C63A0=1 leaves=n/17 …` (a partial leaf install; the layer never draws) | no | yes |

## Verbs and tokens

```bash
tools/tacli gui <i> on            # arm BEFORE launch (the leaves install at attach); the draw follows the file live
tools/tacli gui <i> off           # writes `off`: leaves installed, layer not drawn — the live A/B
tools/tacli gui <i> strict        # the harness mode
tools/tacli gui <i> census        # = 'gui.on=census log pgm trace'
tools/tacli gui <i> remove        # unlink the file — NOT an un-arm under --defaults (the default re-applies)
tools/tacli gui <i>               # report
tools/tacli arm <i> 'gui.on=nostring norepaint'   # tokens, the same file
```

Two families of tokens live in `tagpu_gui.on`, and they are read at different times:

| read once, at attach (`tagpu_gui_hook.c`) | read live, on the poll (`tagpu_gui_surf.c`) |
|---|---|
| `census`, `log`, `pgm`, `trace`, `norepaint`, `nostring`, `key=`, `probe=` | `off`, `strict`, `norestore`, `sharptest`, `mmbase`, `nominimap`, `nocursor`, `cursorscale=` |

Arming an attach-time token on a running instance silently does nothing.

- **`norestore`** — the layer without Classic++ art: the UI-only A/B.
- **`nostring`** — text stays a box of captured pixels instead of a string op.
- **`norepaint`** — the layer normally calls the engine's own `GUI_StageUpdateDraw` on the top
  screen when it arms or reseeds, so the shell's art arrives as sprite ops; this turns that off.
  `log` prints one line per repaint with its op count by kind (`gui: repaint #N … 115 op(s) [gaf
  105 line 4 focus 6]`). In game a repaint is 1 op (`ARMMAIN2.GUI` is three labels); it is a shell
  measurement.
- **`sharptest`** — paints a 64x64 opaque green square at the viewport's top-left and a one-device-
  pixel white column at device x = 100, as geometry: the proof the device-resolution sharp layer
  is wired. Harness only, like `strict`; never hand a player an instance with it armed.
- **`mark.on=noselbox`** (a different file) makes the engine draw its own selection rects every
  frame, at the unzoomed projection — the forcing lever for anything drawn inside the viewport.
  Confirm with `tacli log <i> -g 'markown: engine selection'`; remove it afterwards.

## The heartbeat

`tacli log <i> -g 'gui: twins='`, one line per 300 frames. Fields worth reading:

- `twins= seeds= sprites= pixels= bars= rects= atlas=` — running totals, not per-frame numbers.
  `bars=` and `rects=` are bar and rectangle ops replayed as geometry.
- **`overflows=` and `lost=` must stay 0.** `resets=` is 2 per launch (the arm, the shell→game
  switch) and +1 per context switch after that; under `log` each reset is named (`stall-over`,
  `arm`, `gl-context`, `queue-full`, `arena-full`, `box-outside-surface`, `lost-sprite`,
  `atlas-full`, `untwinned-copy`). `stalls=` is 1 per context switch; `skipped=` the stale ops
  stepped over after one.
- `k=` is device pixels per twin texel, `sharp=WxH` the sharp layer's size (the viewport).
  `k` is not always 1: the window is resizable and letterboxed, so a window off the game
  resolution is fractional, and the shell (640x480) under a 1280x984 client is `k=2.000`.
- `cpp= assets= light= col=<made>/<live> colvalid= rgb=` — the Classic++ half. `colvalid` drops to
  0 while `assets=0` or while the palette is settling.
- **`str=<ops>/<glyph quads> miss= reseed= glyphs=<cached>/<drops> fonts=`** — `miss` and
  `reseed` must stay 0. `glyphs=` climbing is not `str=` climbing: the glyph records arrive
  unconditionally, the stamping needs a twin. Read `str=` for "text was drawn".
- `curs=<own>,<w>x<h>,dev=,sc=,drawn=,warm=` — `warm=` counts frames spent atlasing a shape for
  the first time; one per new shape is correct, a climbing count means the atlas is refusing.
- `mm=<draws>,fog=<hidden>/<texels>,noeng=` — the sharp minimap.
- `repaints=<done>/<refused> rops=<last>` — `<refused>` counts episodes, not flips.
- `pixdrop=` — pixel ops dropped by design (a seed crosses as geometry).
- `paldiff=n@i` and `palchg=` — see the palette rule below.

## The palette and Gamma

**The presented palette is not the engine's table at `main+0x143A7`.** The engine scales every
palette it sets by the Gamma factor on the way to DirectDraw and never scales its table. The scale
truncates, and the factor has two formulas: an option screen applies `0.5 + Gamma/24` (1.0 at the
engine's default of 12); **`+gamma N` typed in chat sets it to `N/10` outright**. Everything we
draw follows the **presented** palette, the world included; `tagpu.log`'s `pal: presented palette
changed (… gamma=…)` line carries the live factor, and `tacli peek <i> '*0x511DE8+0x37F08:4'`
gives the option, which after a `+gamma` no longer implies the factor.

**`Gamma` is one shared, mutable registry value** — the `user.reg` inode every prefix shares,
rewritten in place by wine at every launch — so it is whatever TA last stored. `paldiff=` in the
heartbeat is whatever that makes it: 235 entries at Gamma 15 (factor 1.125), 0 at Gamma 12. **Read
it, never assume it, and never "fix" the registry value** — that moves every measurement taken
against the prefix. Anything comparing a restored twin to an offline restore must use the
presented palette, never the archives' `palette.pal`.

**Type a chat line slowly and check it before sending.** `keys return`, the `char:` tokens and
`keys return` back to back drop the opening `return` often enough to matter; the run then types
the cheat as hotkeys. A second between the three, and a shot of the bottom strip to read the
field back. On a `scenario load` fixture the chat line may not open under injection at all —
peek the address the cheat writes (`+showranges` → `main+0x391BF`, `+gamma` → `main+0x37F08`)
before trusting that it landed.

## Cursor, text, minimap

**The cursor is ours.** `tacli keys <i> "dmove:768,576"` moves the pointer in **client** pixels
without clicking (`dev=1` in `curs=`); a logical `pmove:` works but has no pointer behind it
(`dev=0`), so the draw falls back to the engine's position. The measure is the cursor's device
footprint and needs no reference image: park the pointer far away, grab the window, move it to a
known client point, grab again, take the bounding box of the pixels that changed. Ours is
**10x20 at every k** (`cursorscale=` is device px per art px, default 1, clamped 0.25–8).
`nocursor` stops the layer drawing it.

**Cursor and hover state without a screenshot**: `tacli peek <i> '*0x511DE8+0x2CBE:1'` is the
installed cursor index, `+0x2CBA` the unit under the pointer (0 = none), `+0x2CBC` the feature
(`0xFFFF` = none), `+0x2CC3` the current order byte, `+0x2CC6` the region flags (bit0 minimap,
bit1 world). Park with `keys <i> mouse:X,Y`, then peek (`exe-reverse-engineering.md` §"The
cursor chain").

**Text is a string op.** A static in-game frame publishes its text once and dedupes it, so `str=`
freezes at ~22 ops on the parity fixture; to measure anything about text, turn the clock on
(`+clock` in chat) or open a screen, and it is ~1 000 ops per 300 frames.

**The minimap.** At `k = 1` the twin's own minimap pixels stand (the box is 106x126 device px, so
a sharper source would be thrown away); `mmbase` forces the sharp rebuild on there for an A/B,
`nominimap` turns it off. Its safety property is a bound: our base is used only where the
engine's fogged and unfogged bases agree across 3x3. **`fog=0/13356` means the fixture tests
nothing** — a mapped skirmish hides nothing — so the fixture is an unmapped game:
`tacli scenario load <i> <scn> --mapping 0` gives `fog=13301/13356`. Measure sharpness by
distinct colours in the box, not by replication.

**Classic++ UI.** With `classicpp.on` the layer picks per texel between an index twin and a colour
twin; `norestore` is the A/B. On entering a game the panel is **indexed**, and that is expected:
colour reaches a twin only through a sprite op and the panel is seeded at the mode switch with
HUD icons all under the 12-px restore floor. Open `ARMOPT` with Tab or select a builder first,
then a `norestore` A/B moves ~37 000 of the panel rect's 45 056 px; taken before that it differs
by 0 px and means nothing. Read `col=`/`colvalid=` in the heartbeat before assuming colour
reached the UI at all.

## `uiwalk.py`

`tools/uiwalk.py` drives the shell and a game by gadget name and writes `report.md` with one row
per stop. Run it with the venv's python (numpy + PIL):

```bash
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --res 1024x768 --out /tmp/uiwalk          # the inventory walk
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --side core --game-only --out /tmp/uiwalk-core
../.venv-undither/bin/python tools/uiwalk.py --inst <i> --res 1024x768 --window 1536x1152 --device --out /tmp/uiwalk-k
```

- **The frame-parity mode (`--layer`) cannot run today**: it takes its GL half through
  `tacli glshot`, which is retired, and the walk fails at the first stop. The inventory walk, the
  `--device` hit check and `--cycles N` do not depend on it. Moving the parity capture to a window
  grab is a tool change, not a doc change.
- `--side core` walks `CORMAIN2`/`CORCOM1`/`2` with the `COR*` pagers on
  `scenarios/tascene-parity-core.json`; the in-game menu is `ARMOPT.GUI` on both sides. After the
  screens the walk types `+clock` and `+bps`, holds SPACE over the commander, opens the menu,
  PREFS and F4, zooms them to 0.5x and 2x by writing `tagpu_zoom.txt` (so no click is bent), walks
  the commander for the minimap's dot, and edge-scrolls for the view box. `--screens-only` stops
  after the inventory.
- `--cycles N` runs N game → shell → game cycles in one process (`park`, Tab, `EXIT`, `MAINMENU`,
  `CHOICE1`, the shell inventory again, a new skirmish, `scenario apply` of the fixture). A full run
  with three cycles is ~110 stops and ~30 minutes: `setsid nohup … & disown` and poll the log.
  **Do not walk cycles at 1280x720**: leaving a game at that mode crashes in the level teardown
  (`research/notes/resolution.md`); `--res 1024x768 --window 1536x1152` is `k = 1.5` on a mode
  with clean teardowns.
- **The hit check runs at every stop** whether or not you pass `--device` (it costs a snapshot, no
  clicking): every gadget aimed where the layer draws it, put through the DLL's own inverse,
  checked back inside its own rect. Read `MISS=` and `drift=`; worst drift is 1 logical pixel, so
  it is a hit test, not a pixel test. Zero-area rects count as `degenerate`, not misses.
- **A regression gate is two walks diffed column by column**, the same walk on the previous DLL
  against this one. Ignore `fps`, `atlas`, `twins`, `skipped`, `resets` and `stalls` — they are
  run-dependent. `MAINMENU`'s ~185 differing pixels are its sparkle animation.

## HUD scale

`tagpu_hud.on` magnifies the in-game HUD **within** the player's chosen Screen Size — the panel
`128s` wide, the two bars `32s` tall — and covers the outer part of a world the engine goes on
drawing at full size. Auto is `H/480`: 1.0 at 640x480, 2.25 at 1080p, 4.5 at 4K. The magnification
is applied by the UI layer's fragment shader (`uHud` in `tagpu_gui_surf.c`'s shader source), so
it needs `gui.on`. **It is not a play default** — it is armed by hand — and it writes no engine
memory beyond the four viewport ints named below.

```bash
tools/tacli arm <i> hud.off                 # stock HUD; the A/B
tools/tacli arm <i> 'hud.on=scale=auto'     # Auto: the panel fills the screen height
tools/tacli arm <i> 'hud.on=scale=150'      # a percentage of stock; clamped to this screen's ceiling
tools/tacli log <i> -g '^hud:'              # one line at attach
tools/tacli log <i> -g 'k=[0-9.]* s='       # the gui heartbeat carries s= beside k=
```

- **The lever file is read once, at attach** — arming it on a running instance does nothing until
  a relaunch. 640x480 is always stock (Auto's ceiling is 1.0 there).
- **`L` and `T` must never move; `R`/`B`/`viewW`/`viewH` must.** Peek
  `*0x511DE8+0x37E27:4 +0x37E2B:4 +0x37E37:4 +0x37E3B:4` → `128`, `32`, `W−128s`, `H−64s`
  (at 4K Auto: `128 32 3264 1872`). A moved `L` means a torn world.
- **`tacli` coordinates are the engine's, and at `s > 1` that is not where the thing is drawn.**
  The world is shifted by `(128s−128, 32s−32)`, so `roster`'s `screen`, `pmove:`, `click` and
  `ui show` are all in engine space. `dmove:`/`dclick:` speak device pixels and go through the
  map, so **a device click is the only click that tests "what you click is what you see"**: take a
  unit's `screen`, add the shift, `keys <i> dclick:X+dx,Y+dy`, then `order <i> --sel stop` reports
  `1 issued` if it selected. Use a click, not a hover — `main+0x2CBA` drifts back to the centre
  within a second of an injected move.
- **`tacli click --device` and `ui click --device` are unverified at `s > 1`**; multiply the engine
  coordinate by `s` yourself (`H − (H − y)·s` for y in the bottom bar). `uiwalk.py`'s hit check
  has the same gap, so run it with `hud.off`.
- Reading the pointer back is the cheapest check that the map is right: `keys <i> dmove:X,Y`,
  then peek `+0x2C76:4`/`+0x2C7A:4` and `+0x2CC6:1`. At `s = 2.25`, screen (140,140) must read
  engine (62,62) flags 5; screen (960,600) must read (960,600) flags 6 — the world region is the
  identity at every scale.
- **A cross-build pixel A/B must state which band it is diffing.** At `s = 1` the panel and bars
  are byte-identical by construction; the world is relaunch noise (~3 800 px at 1080p on
  `selbox-facings`). Diff the bands separately.

## Windowed `k`

```bash
tools/tacli launch <i> --res 1280x720 --window 1920x1080   # engine 1280x720 in a 1920x1080 client => k = 1.5
tools/tacli click <i> 1056 764 --device                    # CLIENT-AREA pixels, converted by the engine's own path
tools/tacli ui <i> click SINGLE --device                   # aim where the gadget is DRAWN
```

- **`--window WxH` needs no engine patch**: cnc-ddraw takes the ini's `width`/`height` as the
  client and letterboxes the game mode, so `k = window / res`. Sticky in the instance's meta like
  `--res`. The shell is atom-locked at 640x480, so a 1536x1152 window puts the shell at `k = 2.4`
  and the game at `k = 1.5` in the same run.
- **`--device` is the only click that tests the pointer path.** Every other injected event is
  delivered in the engine's own coordinates and would pass at any `k`, right or wrong; `--device`
  posts client-area pixels and lets `mouse_client_to_game` — the same function a hardware click
  takes — work back to a logical pixel.
- **`tagpu_devres.on`** draws the world at the device's resolution: `ss` follows `ceil(k)` and the
  box-resolve to game resolution is skipped, so the composite downsamples rather than
  nearest-stretching. Opt-in; the native log line carries `devres=` beside `ss=`.
- Two things still move in a "static" frame at `k != 1`: the cursor sprite wherever it is parked,
  and at zoom < 1 a stray animating feature in view. Band-select with
  `keys <i> mouse:X,Y down:lbutton mouse:… up:lbutton`; there is no `tacli select`.
