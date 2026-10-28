# The in-game wiki: Impure's features, shown and tried in the game

## Status

**Designed 2026-10-25; the browser mockup is the first build, and the DLL has the wiki's core.**
The owner wants a wiki inside the game that explains Impure's own features with examples a player
can watch and try. The decisions below came out of a design interview with the owner on
2026-10-25; each is recorded with its reason. The DLL carries the tree and the entries, runs the
simplified simulation in C, equal to the mockup's tick for tick on every demo, lays the window out
in C, equal to the mockup's boxes, lines and baselines, and builds the stage's picture as the
replay HUD's ops, equal to the mockup's canvas call for op. On the Vulkan lane the game draws the
wiki button and opens the window over the game, takes every input while it is open and pauses
single player; the window holds the page -- the tree, the demo list, the entry's column -- and the
stage with the selected demo playing, driven by the page's controls, the wheel and the scrollbars,
or Try it yourself, the demo's scene handed to the player
([the plan](ingame-wiki-plan.html), landings 2 to 5). The stage's ground is the game's own: the
description's tiles, its deposits and vents and the engine's fog, drawn by Impure's own terrain and
feature passes; its units are the game's models, drawn by Impure's own unit pass, facing where they
walk or work, buildings rising in the nanoframe, translucent ghosts at the cursor and the gesture's
sites; a type the running game does not have stays the whitebox's box ([the 3D stage's
plan](ingame-wiki-3d-stage-plan.html), landings 6g and 6h). Under each supported mod the stage
draws the mod's own files, read in the running game's order ([below](#the-3d-stage-under-the-mods),
landing 6i). Beside the map the stage shows the running mod's left sidebar, its minimap and the
page the engine would show, and its build menu is the only one: a scripted build is the cursor's
trip to the button, and in Try it yourself the player presses it
([below](#the-left-sidebar-on-the-stage)). Every demo plays the player's own side in the running
mod: its units, their figures and their display names are that side's own, picked from the mod's
files by one rule that names no mod, and the demo plays, and its captions name, the buttons of the
player's Interface Type ([below](#the-per-mod-tutorials)). The owner's decisions for the sidebar
and the tutorials are [below](#the-sidebar-and-the-per-mod-tutorials-the-owners-decisions). The
units run their own COB scripts in both simulations, through one C host on our own COB VM: a
builder works only once its script has taken its build stance, the Quick Build demos wait for
the work to be done, the stage draws the poses the scripts set, in the game and in the mockup,
and a builder at work sprays the engine's nano particles from the piece its script names
([the COB plan](ingame-wiki-cob-plan.html), L3a, L3b and L4a built; [below](#the-units-scripts)).

| What | Where |
|---|---|
| The tree of categories and entries | `art/wiki/tree.json` |
| One entry: its text, its mods, its demos | `art/wiki/entries/<id>.json` |
| The mockup, which reads only those files | `tools/wiki/mockup/` (`index.html`, `wiki.js`, `wiki.css`, `sim.js`, `text.js`, `stage.js`; under `gfx=game` also `scene.js`, `gfx.js` and `tools/tascene-render.js`) |
| The window's art kit | `art/wiki/kit/` (`kit.json` and `parts/`), in `art/hud/kit.json`'s format, fitted by `tools/hudart/fit.py --set wiki` from the parts as cut in `art/wiki-full/kit/` |
| The stage's fallback cursor icons, the controls' glyphs and the tree's marks | `cursor_*.png`, `i_play`, `i_pause`, `i_replay`, `i_open`, `i_shut` and `i_dot` in the kit, written by `tools/wiki/cursors.sh` from `stage.js` `cursorShape` and `tools/wiki/mockup/glyphs.js` |
| The window's layout numbers and its text roles | `art/wiki/layout.json`, which `wiki.js` sets as custom properties of the frame (`--w-` and the number's path) for `wiki.css`, and which the DLL reads |
| The stage in 3D, as the game will draw it | `tools/wiki/gfx.py` resolves each stage description and each entry's units from the local install into the gitignored `tools/wiki/mockup/gfx/`; under `gfx=game`, `scene.js` turns each view into model records and the fog grid, and `gfx.js` renders them with `tools/tascene-render.js` (tagpu's own shaders, which the tascene lab shares) |
| The stage's ground as references: which tile of which map in each square, which feature on which cell | `art/wiki/stage/<entry>.json`, written and checked against the local install by `tools/wiki/stagemap.py` (`tiles`, `build`, `check`) through `tools/wiki/tavfs.py`, the engine's file precedence in Python; not packed yet |
| The demos' generators; the headless check of every demo | `tools/wiki/gen-quickbuild.mjs`, `tools/wiki/gen-buildshapes.mjs`; `tools/wiki/simcheck.mjs` |
| The data in the DLL | `art/wiki/` (`tree.json`, `layout.json`, the entries) packed by `tools/hudpak.py --set wiki` into the resource `WIKIPAK`; read by `tagpu/ddraw/src/tagpu_wikidata.c` (through `tagpu_pak.c`, the archive reader the HUD shares) |
| The window's layout in the DLL | `tagpu/ddraw/src/tagpu_wikilay.c`: pure, the HUD stack's ops, text and hit boxes for a page state (`WIKIPAGE`) |
| The layout check, C against the mockup | `python3 tools/wiki/wikilaycheck.py`, with `tools/wiki/wikilaycheck.c` |
| The simulation in the DLL | `tagpu/ddraw/src/tagpu_wikisim.c` |
| The parity check, C against `sim.js` | `node tools/wiki/simparity.mjs [--wine]`, with `tagpu/ddraw/tests/wikitest.c`; `--hash` prints each demo's final state hash from `sim.js` |
| The stage in the DLL | `tagpu/ddraw/src/tagpu_wikistage.c`: a view copied by value, built into HUD ops, each tagged with its range; `tagpu_wikiscene.c`: the ground's cells, feature quads and fog grid; `tagpu_wikiworld.c`: the 3D stage's producer for the wiki's copies of the world passes — [GPU status](gpu-status.html) §2.12d |
| The 3D ground's check, C against the mockup | `node tools/wiki/scenecheck.mjs`, with `tagpu/ddraw/tests/wikiscenetest.c`; in the game, `tools/wiki/stage3dcheck.py` over `wiki_regression.py stage3d`'s captures |
| The stage's check, C against `stage.js` | `node tools/wiki/stagecheck.mjs`, with `tagpu/ddraw/tests/wikistagetest.c` |
| The units' poses | in the DLL `tagpu_wikistage.c` `tagpu_wikistage_pose` (the pose pool, game thread) and `tagpu_wikiworld.c` `pose_compose` (render thread) over `tagpu_posemath.h`; in the mockup `tools/wiki/mockup/pose.js`; `tools/wiki/posecheck.py` holds `pose.js` to tacob's composition |
| The sidebar's fallback layouts | `art/wiki/side/fixture.json` (480 rows) and `fixture-tall.json` (768 rows), synthetic |
| The sidebar in the mockup | `tools/wiki/mockup/side.js` (the column's composer); `sim.js` (the layout's check, the router, the buttons and the build trip); each installed mod's pack under the gitignored `tools/wiki/mockup/gfx/side/<mod>/`, written by `tools/wiki/sidepacks.py` through `gfx.py --side-only` |
| The sidebar in the DLL | `tagpu_wikiasset.c` (`tagpu_wikiside_load`, the reading) over `tagpu_guifile.c` (the .GUI files by position); `tagpu_wikiside.c` (the composer); the router and the buttons in `tagpu_wikisim.c`, the column's ops in `tagpu_wikistage.c` |
| Each side's casting, by the cast rule | `tools/wiki/gfx.py` (`side_casting`, into each pack's `side.json` by `tools/wiki/sidepacks.py`, which checks the stock identity); in the DLL `tagpu_wikiasset.c` (`tagpu_wikiside_load`) |
| The fixture's castings | `tools/wiki/casting-fixture.json`: `identity` (the entries' own types and figures), `shifted` (invented figures, a 3 × 3 builder and a 4 × 4 geothermal plant with four interior G cells), `unbound` (`identity` without the wall) |
| The text filled for the side and the interface | `tools/wiki/mockup/text.js`; in the DLL `tagpu/ddraw/src/tagpu_wikitext.c`; `node tools/wiki/textparity.mjs [--wine]` holds the two equal |
| Every check that plays no game, in one run | `tools/wiki_regression.py - offline` |
| The sidebar's checks | `node tools/wiki/sidecheck.mjs` (with `tagpu/ddraw/tests/wikisidetest.c`); `python3 tools/wiki/assetcheck.py --side`; `tools/wiki/sidedump.mjs` (a pack's column for a state, as a PNG) and `tools/wiki/sideview.mjs` (`sim.js` at a game dump's state) |
| The window in the game | `tagpu_wiki.c` (input, pause, the page's state and the demo's simulation; game thread), `tagpu_wikistate.c` (`tagpu_wikilay.c` and `tagpu_wikistage.c` on the record) and `tagpu_vk_hud.c` (drawing; render thread) — [GPU status](gpu-status.html) §2.12c |
| The game test of the window | `tools/wiki_regression.py <instance> sp|page|demos|try|recorded|asset|stage3d|units --launch`, `tools/wiki_regression.py <name> mp`, `tools/wiki_regression.py all mods --shots DIR` (the 3D stage under each supported setup: [below](#the-3d-stage-under-the-mods)); `demos --shots DIR` grabs each entry's demo mid-play beside the mockup's; `stage3d --shots DIR` captures the 3D stage through the trigger `tagpu_wiki.shot` and compares it with the mockup's; `units --shots DIR` does the same with the units, then the facing, the `nounit=` lever, the render thread's time and a reclaim in the game; `try` replays two demos through Try it yourself's stage as device input and holds the game's simulation to `simparity.mjs --try` |
| The units' scripts | `tagpu/ddraw/src/tagpu_wikicob.c`, the host on `tagpu_cobvm.c`, which the DLL's simulation calls in-process; the mockup's `sim.js` reaches the same C through `tools/wiki/serve.py` (`POST /cob`, from the page through `tools/wiki/mockup/cob.js`) and, under node, `tools/wiki/cobhost.mjs` (`serve.py --stdio`) |
| The nano spray | the particles in both simulations (`tagpu_wikisim.c` *THE NANO SPRAY*, `sim.js`'s spray functions), the piece's origin from the host (`tagpu_wikicob_query`, `tagpu_wikicob_origin`); drawn by `tagpu_wikistage.c` and `stage.js` ([the COB plan](ingame-wiki-cob-plan.html), *The spray*) |
| Serving it | `python3 tools/wiki/serve.py`: the repository with `Cache-Control: no-cache`, and the units' scripts at `POST /cob`; then `http://127.0.0.1:8000/tools/wiki/mockup/` |
| This page | the decisions, the data format and the simulation's contract |

Two entries, both under Quality of Life → Building: **Quick Build: Resource Extractors** and
**Build Lines, Grids and Rings** (the Shift-drag placement family, landed on `main` at `90c67078`).

## The decisions

### The mockup is the specification

- **A permanent browser mockup, maintained like the replay HUD's** (`tools/hudart/mockup/`). It is
  not a throwaway: it is where the wiki is designed and changed first.
- **It is a 1:1 specification of the in-game wiki.** The DLL copies it: the same layout, the same
  data, the same simplified simulation, the same drawing. Nothing goes into the game that the
  mockup does not show. `tools/wiki/simparity.mjs` keeps the two simulations equal (*The
  simulation's contract*, below), as `laycheck.py` keeps the HUD's layouts equal.
- **Try it yourself is a simplified simulation in the game too.** A second hidden game instance,
  or swapping the running game out for a practice scene and back, were considered and declined
  (2026-10-25): the simplified simulation is enough, and the mockup can then be exact. The DLL's
  version calls the real shapes, `tagpu_placegeo.c`'s `place_shape` and `place_ring`, which read
  nothing but their arguments. The snap scorer, the best-fit choice and the gesture's rules it
  ports from `sim.js` instead, because `tagpu_resource.c` and `tagpu_place.c` read the engine.

### Content is data

- **The tree** is `art/wiki/tree.json`: categories that nest to any depth, their leaves entries.
- **An entry** is `art/wiki/entries/<id>.json`: title, subtitle, text, the mods that lock the
  feature off, its unit figures and its demos.
- **A demo is a scene and an input timeline, nothing more.** The inputs (cursor moves, keys and
  buttons down and up, waits) are played through the same simulation as Try it yourself, which
  draws whatever follows from them. A demo therefore cannot show something the rules would not do,
  and a demo and Try it yourself cannot disagree. The one scripted extra is a caption, written at
  a given tick.
- `art/wiki/` mirrors `art/hud/`, and the DLL embeds it the same way: `tools/hudpak.py --set wiki`
  packs `tree.json`, `entries/*.json`, `kit/kit.json` and `kit/parts/*.png` into the resource
  `WIKIPAK`.

### The window

- **Opened by a button at the top right of the game**, in the row of Impure's own buttons: from
  left to right, **wiki**, rewind, gear. The gear is Impure's options sprocket, 28 × 28 bar pixels
  at x = W−44..W−16 of the 32-pixel top bar (`tagpu_menu.c` `trigger_rect`); the rewind button is
  the replay HUD's, 28 × 28 at W−75..W−47, 3 pixels left of it (`art/hud/layout.json` `live.entry`),
  shown only in a live, recorded, single-player skirmish on Vulkan. The wiki button is 28 × 28,
  3 pixels left of the rewind button; where the rewind button is absent it takes that slot, next to
  the gear.
- **About 90 % of the game screen**, centred, and it follows the screen's resolution and aspect.
  The share is a setting of the mockup (`size=`), to be tuned.
- **Single player pauses while it is open; multiplayer keeps running.**
- **It takes all input while open.** A click on the margin of game around it does nothing to the
  game. In multiplayer the game keeps moving behind it and the top bar keeps counting, so the
  player can see time passing, but cannot order until it is closed.
- **Closed by Esc, its X, or the wiki button.** No hotkey opens it yet: most free keys collide with
  TA's or a mod's bindings.

### The page

```
┌ Wiki ────────────────────────────────────────────────────────── [X] ┐
│ Tree        │ Demos            │ Quick Build: Resource Extractors     │
│ ▾ Quality…  │ 1 Snap onto the  │ subtitle                             │
│   ▾ Building│   deposit        │ (● Turned off in this mod)           │
│     • Quick │ 2 Hold Alt …     │ ┌──────────── stage ───────────────┐ │
│       Build │ 3 Click a deposit│ │                                  │ │
│             │ …                │ └──────────────────────────────────┘ │
│             │ 11 Fog           │ caption                              │
│             │                  │ keys: [Shift] + [Right-click]        │
│             │                  │ ▶ ⟲  1× 2×   [Try it yourself]       │
│             │                  │ About: the entry's text, scrolled    │
└─────────────┴──────────────────┴──────────────────────────────────────┘
```

- **The tree** on the left; selecting an entry opens **its demo list** as a second column.
- **The stage** fills the rest, with the demo's caption under it, a strip of the keys and buttons
  held at that moment, and the controls: play/pause, replay, 1× and 2×, Try it yourself.
- **A demo plays when it is selected**, and holds its last frame when it ends; replay plays it
  again.
- **Try it yourself takes over the stage from the selected demo's scene**: the same map, units
  and fog, with the script stopped and the controls handed to the player. Back to demo returns.
- **The entry's text** scrolls below the controls.
- **Columns** take fixed shares of the window and scale with it; long names wrap.
- **A mod that turns the feature off**: a red circle with "Turned off in this mod" on the entry's
  page, and the same mark on its row in the tree. Its demos still play.
- **A demo stopped at a build the mod's sidebar cannot reach** shows the same badge on the page
  while it holds (`wiki.js` `off = locked(entry()) || v.locked`; in the DLL `WIKIPAGE.sim_locked`,
  set from the view's `locked`). The tree's mark stays the data's alone. `wikilaycheck.py` holds
  it to the mockup (`1080-tazero-buildshapes-locked`).

### The art

**The window is drawn from one kit, `art/wiki/kit/`, chosen 2026-10-25** from three looks painted
in the replay HUD's style (`research/notes/hud-art.md`): **slim**'s window, panels, bezel, buttons,
keycaps, badge, close and wiki buttons (thin worn rims round dark screen faces), with **plate**'s
two row parts — a framed steel row, and for the selected row its lit, raised twin. Each part is a
nine-slice drawn by the replay HUD's rules; their specs are `tools/hudart/specs/parts_wiki_slim.py`
and, for the rows, `parts_wiki_plate.py`. Every text role takes a style of `art/hud/layout.json`
in the replay HUD's faces (`art/hud/fonts/`); `art/wiki/layout.json` `roles` names each role's
style, and its other numbers place what the kit leaves room for (`geom`), the columns, rows,
strips, controls and panels. The parts as cut, about 10 px a unit, are filed in
`art/wiki-full/kit/`; `tools/hudart/fit.py --set wiki` fits them into `art/wiki/kit/` at what the
game draws at most: 3 device pixels a unit for the window's parts (a 2160-row screen) and 4.5 for
the button's (the top bar at 4K and a 150 % HUD), 0.59 MB of parts against 2.75 as cut.

### The stage

A whitebox of the game's own view, so a demo reads as what the player will see:

- the map as a flat grid of TA's footprint cells (16 world units), seen from above; metal as
  darker cells shaded by value, a geothermal vent as a marked circle, grey fog and black
  unexplored ground as overlays;
- units and buildings as grey boxes at their footprint size with a team-coloured edge and their
  unit name (`ARMCK`, `ARMMEX`); the selected ones in the game's selection box;
- the cursor, the green or red placement ghost, queued build squares, order lines, and
  construction as a fill;
- a placement gesture's sites as TA's build square in the site's colour (green placed, red refused,
  yellow takes a queued order off) over a translucent ghost of the building, as the game's Vulkan
  path draws a ghost at each site, with the site's place among the orders the release adds small in
  its corner (the game draws no number; the mockup does, so the queue order can be read);
- the running mod's left sidebar beside the map, its minimap and the page the engine would show
  ([below](#the-left-sidebar-on-the-stage));
- the nano spray, when the units run their scripts: each particle a dot of 2 × 2 game pixels in
  the retail palette's green ramp (`0xA1..0xA7`), above the units, only where its fog cell is
  clear ([below](#the-units-scripts)).

**How it is drawn.** `tools/wiki/mockup/stage.js` draws one view of `sim.js` into a 2D canvas; it
runs in the page, where `wiki.js` calls it, and under node. The DLL's `tagpu_wikistage.c` builds the
same picture as the replay HUD's ops, and `tools/wiki/stagecheck.mjs` pairs the two call for op on
every demo, so a change to the stage is made in `stage.js` first. Everything it draws has an op to
be:

- rectangles, a rectangle's stroke, straight strokes (dashed by the canvas's dash rule), circles and
  their strokes;
- text in one face of the replay HUD, the `toast` style's Oswald 700 with no letter-spacing, each
  line centred on its point. A name longer than 1.5 times its box shrinks until it fits, as the HUD
  fits its text;
- the cursor: the game's own, a frame of the player's `anims\cursors.GAF` (below). Where those
  frames did not resolve, one of three kit icons (`cursor_arrow`, `cursor_move`, `cursor_build`)
  instead, drawn at screen size centred on the pointer. `tools/wiki/cursors.sh` rasterises the
  icons once, in Chromium, from `stage.js`'s own `cursorShape`, so the mockup and the game draw the
  same pixels.

**The cursor is the game's.** Each of the view's three shapes shows the cursor the game shows in
that situation, measured in the game (2026-10-27, the installed index `main+0x2CBE` read back on
Show Down at 1024x768, Interface Type 1): nothing of the player's mobile selected, `cursornormal`
(index 19); a mobile unit selected, `cursormove` (14), animating; a building armed from the build
menu, `cursornormal` again, at every point of the map. The build cursor `0x4197D0` installs no
cursor: in mode `0x0E` over the map `0x499200` calls it in place of the picker, so the cursor stays
the one the click on the menu installed. Quick Build's ghost changes the packet alone
(`tagpu_resource.c`), so over a deposit the cursor is the selection's: `cursormove`, MEASURED
2026-10-28 with an ARMCK selected over a deposit and over a vent, index 14 at Interface Type 0 on
retail and at type 1 in Twilight. The sims' shape follows: the build-menu mode, else a mobile unit of the player's selected,
else none (`cursor_shape` in both); Quick Build's ghost no longer makes it a build cursor.

- **The art** is the 3D asset load's (`tagpu_wikiasset.c` `cursors_load`): `<anims>\cursors.GAF`
  by the engine's path rule (`0x429700("cursors")`, language variant first), `cursornormal` and
  `cursormove` found as `0x4B8D40` finds a name and copied in the engine's form into the arena,
  which is immutable once published and never freed; WIKI3D `cursor[3]` by shape, and a `cursors`
  element in the report (GAF, source, each sequence's frames). A mod that renames `anims` is
  followed. The render thread decodes the frames through the presented palette, as the sharp layer
  resolves the player's cursor, into a sprite sheet of 16 cells of 64x64 RGBA (`tagpu_wikistate.c`
  `sprites_bind`), again when that palette moves; each frame is bounded by the cell and the count
  by the sheet, and a shape whose frames do not fit or did not resolve keeps its kit icon. The HUD
  pass holds the sheet as one more texture (`HUD_TEX_SPRITE`, `tagpu_vk_hud.c`), sent whole when
  its generation moves, its sprites hidden in a frame that names a generation the device does not
  hold.
- **The size and place** are the player's cursor's (`tagpu_gui_surf.c` `sharp_cursor`): one device
  pixel an art pixel times `cursorscale=` of `tagpu_gui.on` (1 by default; `tagpu_gui_cursor_scale`),
  whatever the screen, the map's scale or the HUD scale, the frame's signed hotspot subtracted from
  the pointer's device pixel: `w = (int)(w_art * k + 0.5)`, `x0 = x - (int)(hx * k)`. The op is
  `HOP_PART` with `HOPF_SPRITE`; `stage.js` draws the same cell with `drawImage`'s nine arguments,
  unsmoothed.
- **The animation** is the engine's (`tagpu_wikistage_cursor_frame`, `stage.js` `cursorFrame`):
  `SetUICursor 0x4AB400` starts a sequence at frame 0 when it installs one, and the UI's update
  `0x4AB5D0` counts each frame's ticks down on a 30 Hz clock (`0x4B6340`), looping when the
  sequence's byte `+2` says so, else drawing nothing past the end. The engine installs a cursor
  only when its index differs from the installed one (`0x499254`, `0x4992A9` compare `main+0x2CBE`
  first, DISASSEMBLED), so arming a building over the arrow, both `cursornormal`, runs the arrow's
  animation on; both sims restart it only when the shape's sequence changes. The stage counts the
  simulation's ticks since the sequence began (`cursor_since` in both sims and their views): a
  scripted input's change begins on its own tick, and a change the player makes in Try it, between
  two ticks, on the tick the sim stands at, the one the next view shows. So at 2x the move cursor
  pulses twice as fast, with the demo. In the game the cursor runs on that wall clock
  (`0x4B6340` is GetTickCount × the 30 Hz rate / 1000), so it keeps pulsing while the game is paused
  and ignores the game speed; the stage's runs on simulation ticks, so it stands still while a demo
  is paused or has ended.

`stagecheck.mjs` draws every frame twice, with the kit's icons and with stand-ins carrying
`cursors.gaf`'s own sizes, hotspots and ticks and a third, negative-hotspot sequence that does not
loop, each size at its own `cursorscale` (1, 1.5, 2); every frame whose tick / 30 is odd leaves the
cursor out, as Try it does off the stage, and each frame's `WIKISTAGEOPS.cursor` must name a cursor
op exactly when `stage.js` drew one. `assetcheck.py` holds the `cursors` element
to its Python reference and to `gfx.py --resolve-json`. `gfx.py` writes the mockup's copy,
`gfx/cursors.json` and the sheet `gfx/cursors.rgba`, gitignored with the rest of `gfx/`, and the
mockup draws them in either look.

The map is fitted into the stage with a pad of 6 units (`stage.js` `fit`, `tagpu_wikistage_fit`):
the one function the game's render thread draws with and its game thread will turn a click into a
world point with.

The mockup's `gfx=game` draws the same stage in 3D, as the game does: a render of the map at zoom 1
in the game's own shaders, from the local install, composited into the map's rect between the
stage's op ranges (*In game: what each element is drawn with*, below), each edge of that rect
snapped to the device pixel as the game's composite is. The layout and the simulation do not change
for it. `?layers=terrain,features,units` draws only the named layers (`stage3dcheck.py` renders the
features alone to class the game's pixels).

### The units and their figures

**Stock TA, ARM side**: ARMCK (construction kbot), ARMACK (advanced construction kbot), ARMMEX,
ARMMOHO, ARMGEO, and ARMPW as a unit that cannot build. In the stock build lists ARMCK builds
ARMMEX and ARMGEO, and ARMACK builds ARMMOHO but neither of the others (`sidedata.tdf`
`[CANBUILD]`; MEASURED 2026-10-25 from the retail archives).

| Unit | Footprint | Extracts / makes | BuildTime | WorkerTime | BuildDistance | MaxVelocity |
|---|---|---|---|---|---|---|
| ARMCK | 2 × 2 | | 5597 | 80 | 40 | 0.8 |
| ARMACK | 2 × 2 | | 13432 | 160 | 60 | 0.6 |
| ARMMEX | 3 × 3 | ExtractsMetal 0.001 | 1800 | | | |
| ARMMOHO | 5 × 5 | ExtractsMetal 0.003 | 35750 | | | |
| ARMGEO | 4 × 4 | EnergyMake 250 | 13078 | | | |
| ARMPW | 2 × 2 | | 1452 | | | 1.8 |

"The strongest option" compares the definitions' own figures, ExtractsMetal for an extractor and
EnergyMake for a geothermal plant, strictly greater winning and a tie going to definition order
(`tagpu_resource.c` `strength`, `quick_choice`). What a finished extractor then makes is
ExtractsMetal × Σ over its footprint cells of (the cell's metal + 1) (`0x437840`, DISASSEMBLED
2026-10-25); the wiki does not show income.
A geothermal plant needs one of its footprint cells (yard `G`) on a feature with `geothermal=1`,
a 1 × 1 vent (the site test `0x47D2E0`). Their footprints, extraction, geothermal
output, build times, worker times, speeds, build distances and build lists are read once from the
stock unit files and written into `quickbuild.json` as plain numbers, with the unit they came
from. No game file is copied. These authored types and figures are what a demo plays on the
fixture, with no sidebar, or under a mod whose layout is refused. On the running mod's own layout
each unit is its cast's type in the player's side instead, with that mod's figures
([below](#the-per-mod-tutorials)).

### The sidebar and the per-mod tutorials: the owner's decisions

Decisions 1 to 12 (2026-10-27) made [the left sidebar on the stage](#the-left-sidebar-on-the-stage);
13 to 26 (2026-10-27 and 28) made [the per-mod tutorials](#the-per-mod-tutorials). Both are built
as stated except where a line says otherwise.

1. **The sidebar is inside the stage, at the map's scale**: a slice of the game's screen, the
   running mod's left sidebar then the map, one game pixel each. A demo that shows it gets a wider
   stage; the map's framing is unchanged.
2. **The wiki's own build column is retired**; the mod's sidebar is the only build menu. It is a
   per-demo flag, on when the scene gives the player a constructor, and Try it shows it whenever
   the player has one on the stage. *As built*: a **mobile** builder (MaxVelocity > 0,
   WorkerTime > 0).
3. **A click on a build button shows the game's pressed frame**, held about half a second in a
   demo, and pressed while the button is down in Try it.
4. **A demo names the unit, never a position**: its build is resolved against the mod's pages for
   the selected builder, through PREV/NEXT when needed; a builder that cannot build it shows the
   "Turned off in this mod" state.
5. **One picture rule for every mod, never a list of mod names**: the page's GAF frame, else the
   unit's download picture, else `unitpics\<UNIT>.PCX`, else a labelled button, checked against
   each compatibility setup's own sidebar in game (all six match, *The checks* below).
6. **The minimap comes with it**: the demo's map, the unit dots, the camera box; live in Try it,
   never clickable.
7. **In Try it the build buttons, PREV/NEXT, ORDERS/BUILD and STOP work**; every other order button
   is greyed until an entry needs it.
8. **HUD scale is ignored**: the stage keeps TA's native proportions.
9. **The stage maps are 40 × 30 cells**, so retail's 480-row sidebar fits beside the map; a 768-row
   one leaves a band under the map, the map top-aligned.
10. **A scripted `build` with the sidebar plays out** as the cursor's trip to the button and back;
    every demo was re-timed once by hand, and the data's ticks are exactly what plays.
11. **Build buttons the entry does not define are greyed.**
12. **A demo whose build has no equivalent in the mod stops at that tick under the entry's badge**,
    and only when the mod truly has none (decision 13).
13. **Each mod gets a tutorial that works in it**, built from the mod's own equivalents of the units
    the entry uses, wherever the feature is on.
14. **Equivalents come from a general rule over each unit's own data**: no list of mod names, no
    override files; the in-game check shows what the rule picked per mod. *As amended by 20.*
15. **The mod's own figures**: footprint, extraction, build time, speed and build distance from the
    mod's unit files.
16. **Text names roles** (`{builder}`, `{mex}`), filled with the unit's in-game display name in the
    running mod, never its internal name.
17. **The tutorial uses the player's own side in the current game**; the mockup has a side
    selector.
18. **The wiki follows the player's Interface Type** (`main+0x37EFA`) in demos and in Try it: demos
    name actions (select, order, cancel), mapped to the player's buttons, and captions name the
    button the player uses; the mockup has an interface selector.
19. **Keys follow the player's key bindings where they can be rebound**; game speed is not followed
    (the wiki keeps its own 1× and 2×). *Closed by 21.*
20. **The rule ranks by tech level, then the mod's own menu order** (amending 14's alphabetical
    tie-break, which picked wrong units in the stock game itself), and "of the demo's movement
    class" is dropped: no unit field tells movement class across mods.
21. **Decision 19 is closed: nothing is rebindable.** TA's keys are a table in the executable
    (`0x495EB7..0x495ECD`), `impure.cfg` has no binding, and the mods' key settings are TADR's,
    whose code never runs beside Impure. Key names live in one table on each side (`KEYCAP` in
    `wiki.js`, `tagpu_wikidata_key_name`), so a rebinding feature would change one place.
22. **In a replay the tutorial follows the controlled player's side**, the one the engine's own
    sidebar follows.
23. **Quick Build picks from the build menu the player sees**, not the engine's AI build list
    (CANBUILD), as its own simulation landing: it is on `main` (`b01f816f..240ed6be`) and merged
    here, so Quick Build and the casting read the same menu (a live check had shown Twilight's
    Quick Build offering the AI's extractor).
24. **A demo of a feature the mod turns off plays under the badge, and its claims are not checked
    there**: a named rule over the entry's own turned-off list, never a list of mods in the check.
25. **A scene that a mod makes false is redesigned, not narrowed**: Build Lines' ring around a
    queued build was reworked so every claim holds for every builder.
26. **One demo length for every mod**: the geothermal demo's tail is 465 ticks, set by the slowest
    plant (Twilight's CORE), and the stock game holds the finished plant longer.

**Settled from the engine, not asked**: the page shown follows the engine's router (nothing
selected: MAIN2; exactly one unit, a builder: its pages; anything else: GEN); a build arms when
its button is released inside it; a constructor's queued build carries no `+N`; MAIN2's KILLS,
LOSSES and TOTALUNITS labels have no writer; the side's art is the controlled player's side's.

## The mockup's controls

A strip above the stage, outside the game frame:

| Control | Values | URL switch |
|---|---|---|
| Resolution | 1920×1080, 2560×1440, 3840×2160, 3440×1440, 1280×1024, 1024×768, fit to the browser | `res=3840x2160` / `res=fit` |
| Window size | share of the screen, default 90 % | `size=90` |
| Mode | single player (pauses) or multiplayer (keeps running) | `mode=sp` / `mode=mp` |
| Detected mod | Stock, Total Mayhem, ProTA, Twilight, Escalation, TA Zero, unknown | `mod=escalation` |
| Side | the side the tutorial plays, among the sidebar layout's sides; with the mod's own pack as the layout the demo plays that side's casting | `side=CORE` (by name, without case; the layout's first side when it names none) |
| Interface | the Interface Type: right-click (1, the default) or left-click (0) | `iface=1` / `iface=0` |
| Open, entry, demo | the wiki open on an entry and a demo | `open&entry=quickbuild&demo=3` |
| Try it | Try it yourself on that demo's scene | `try` |
| Rewind button | shown (a live recorded skirmish) or absent | `rewind=1` / `rewind=0` |
| Skin | the kit, or the flat whitebox | `skin=kit` / `skin=whitebox` |
| Sidebar | the detected mod's pack (the fixture where it has none), the fixture, the 768-row fixture, or none | `sidebar=mod` / `fixture` / `tall` / `off` |
| Graphics | the stage's whitebox, or the stage in 3D from `gfx/`, supersampled `ss` times (default 2) | `gfx=whitebox` / `gfx=game`, `ss=2` |

Behind the window: the replay HUD mockup's backdrop (`research/notes/assets/hud-art/backdrop.png`),
scaled to the resolution, under a whitebox of Impure's top bar: metal and energy on the left, and
the wiki, record/replay and gear buttons at the right.

## In game: what each element is drawn with

The stage is a whitebox of the game's view. In the game, each of its elements is drawn by the
asset and the Impure code below. On Vulkan, the engine's own 8-bpp frame never reaches the screen
(`tagpu_native.c`, the comment above the unit ownership test; `tagpu_vk.c`, above
`world_records`), so every element is Impure's redraw. The engine draws it only on GDI. The world
passes record in this order: terrain, features, units, effects, build ghosts, markers
(`tagpu_vk.c` `world_records`). The UI comes after them, and the cursor last. Every reference was
read in the source on 2026-10-25. The engine addresses are those the source and the notes cite;
they were not re-disassembled for this table.

| Stage element | Game asset | Impure code that draws it in game | `gfx=game` in the mockup |
|---|---|---|---|
| Ground | The map's TNT tiles: the tile set `*(main+0x14283)`, 32 × 32 8-bpp, and the tile map `*(main+0x1428B)`, one u16 per 32-pixel cell | `tagpu_terr.c` `ensure_atlas` (the R8 atlas, 64 tiles a row, 34-texel pitch) and `tagpu_terr_gather`; `tagpu_vk_terr.c` `tagpu_vk_terr_record`. `tagpu_terrown.c` `terr_fill` suppresses the engine's blit `0x483FA0`. | The stage description's tiles (`art/wiki/stage/<entry>.json`), in the same atlas form, through tagpu's terrain shader (`tascene-render.js` `classicTerrain`) |
| Metal deposit | A map **feature** with `metal=` and footprint 3 × 3: on lush maps `lushmetal1..3` (`metals.gaf`, `seqname` frame 0). The metal a mex reads is the feature-grid byte `+7`, which `0x422040` writes from the definition; the sprite is only the picture. | The feature pass, which has no metal case: `tagpu_feat.c` `tagpu_feat_gather` → `draw_feature`; `tagpu_vk_feat.c` `tagpu_vk_feat_record`. `tagpu_featown.c` skips the engine leaf `0x46A610`. | The description's deposit feature, frame 0 of its sequence at its GAF hotspot, through tagpu's sprite shader (`classicSprites`) |
| Geothermal vent | A map feature with `geothermal=1`, footprint 1 × 1: on lush maps `lushvent01..03` (`lushvents.gaf`, `geovent0N`) | The feature pass, as for metal | The description's vent feature, as for metal |
| Grey fog, unexplored ground | The engine's fog grid, built by `0x4843C0` from the line-of-sight counters and the mapped bits | No separate pass. Each world pass applies `tagpu_glsl.h`: `TAGPU_GLSL_FOG_TERRAIN` paints unexplored ground palette index 0 (black), `TAGPU_GLSL_FOG_GREY_RGB` turns a fogged pixel into its R+G+B mean, and `TAGPU_GLSL_FOG_DISCARD` hides units and effects outside sight. The grid is rebuilt by `tagpu_terrown.c` `terr_fogtick` and, zoomed out, by `tagpu_fogwide.c`. | The view's fog cells in the engine's grid form (`scene.js` `fogGrid`: one entry a 32-unit block, two corner masks), on terrain and features. Units and ghosts take none ([the 3D stage's plan](ingame-wiki-3d-stage-plan.html), section 7). When the ground falls back, the whitebox's fog overlays |
| Units and buildings | Each unit's 3DO, posed by its COB script. Faces are textured with GAF frames; a team-coloured face takes frame *k* = the player's logo colour. | The pose is copied by `tagpu_packet_pub.c` `fill_pieces` and composed by `tagpu_native.c` `posed_pose`. `tagpu_posebake.c` `tagpu_posebake_unit` bakes it, `tagpu_posedraw.c` `tagpu_posedraw_unit` hands it over, and `tagpu_vk_unit.c` `tagpu_vk_unit_record` draws it. Team colour: `tagpu_packet_pub.c` `player_logo` reads PlayerInfo `+0x96`; `tagpu_render3do.c` `face_texframe` takes that frame of a team-coloured face's sequence. | The 3DO as the engine holds it (in each node with a selection primitive, that face swapped to 0 and skipped, the rest sorted by mean Y), posed by its script as the game's stage poses it (`pose.js`), or at rest with what Create() hides left out for a unit no script poses and every ghost, its faces bound by the engine's rules with the owner's logo frame, back faces culled as the game's body pipeline culls them, through tagpu's unit shader (`classicUnits`). A mobile unit faces the way the simulation says; a structure faces TA's default heading `0x8000`. The Classic shadows first: a mobile unit's silhouette, a structure's slant |
| Selection | The engine's box `0x46A530`, in GUI colour `0x0A` | `tagpu_native.c` `selbox_emit` → `tagpu_mark.c` `tagpu_mark_emit_selbox` → `tagpu_vk_mark.c` `tagpu_vk_mark_record`. `tagpu_markown.c` `mark_selbox` redirects the engine's box. | The whitebox brackets, over the image (R2) |
| Placement ghost | The armed building's 3DO, `build_unit_id` at `main+0x2CC4`, at rest, heading `0x8000`, alpha 0.40 | `tagpu_native.c` `ghost_record` → `ghost_one` (alpha set in `ghost_armed`) → `tagpu_vk_unit.c` `tagpu_vk_unit_record_ghosts`. The square under it is the next row's. | The building's model, untinted, alpha 0.40, depth writes off, after every unit; its green or red square over the image |
| The build square, and a gesture's sites | Outlines in the GUI colour table at `main+0xDCB`. `0x0A` is green, `0x04` red (refused), `0x0E` yellow (takes a queued order off). These are table indices, not palette indices: stock reads them as palette `0xE9`, `0xD5` and `0xC2` (`terrain-depth.md`, the live read on Two Continents). | `tagpu_mark.c` `gather_cursor` (the single square, coloured by `region_flags` bit 6) and `gather_place` (one square per site), both through `square_emit`. The site colours are `tagpu_place.h` `PLACE_OK`, `PLACE_BLOCKED` and `PLACE_TOGGLE`, assigned by `tagpu_place.c` `classify`. `ghost_record` adds a ghost at each site. | A ghost at each site, whatever its colour, as in the game; the squares and the order numbers over the image |
| Queued build squares | The engine's queued-build outline `0x438C00`, drawn while Shift is held | `tagpu_order.c` `draw_build`, a port with the engine's ten-tick sweep, gathered into the marker pass. `ghost_record` adds a translucent 3DO on each queued site not yet started. | A ghost on each queued site not yet started; the whitebox's dashed squares, in the owner's colour, over the image |
| Order lines | TA draws no lines. It draws marching `pathicon` route dots, a waypoint sprite, target circles and range circles. | `tagpu_order.c` `draw_dots`, `draw_sprite` (the engine's own frames, one texel a screen pixel, out of `tagpu_order_art`'s atlas), `draw_circle`, `draw_ranges` | The whitebox's dashed lines, over the image |
| Construction progress | The unit's build fraction: unit `+0x104`, the fraction remaining, staged as the engine's nano frame (`0x458D30` / `0x458DD0`) | `tagpu_render3do.c` `tagpu_r3d_nano_state` stages it; the unit shader in `tagpu_native.c` applies it, and the wire is `tagpu_posedraw.c` `pd_record`. A unit being reclaimed is not a nanoframe: the engine's reclaim lowers its hit points and leaves `+0x104` alone (`exe-reverse-engineering.md`, *A reclaimed unit is never a nanoframe*). | `tagpu_r3d_nano_state` ported line for line (`scene.js` `nanoState`) into the same shader, then the wire, every face's outline, after every body. A unit being reclaimed is drawn finished, as the game draws it |
| Nano spray | The engine's nano particles (vtable `0x4FD5B8`, layer 6), palette `0xA1..0xA7`, each a `DrawBar` of 2 × 2 pixels (`0x473A00`; `exe-reverse-engineering.md`, *The nano particle*) | In the game `tagpu_packet_pub.c` gathers them (`PK_PART` kind 5) and `tagpu_sfx.c` / `tagpu_fx.c` draw each as a 2 × 2 quad. On the stage the wiki's own particles (`tagpu_wikisim.c`) are drawn by `tagpu_wikistage.c` as 2 × 2 game-pixel rects in the retail palette's RGB, over the image | The same rects (`stage.js`), over the image, in both looks |
| Cursor | `anims/cursors.gaf`, chosen by the engine's picker `0x43E490` into `main+0x2CBE` from `cursor_ary` (`exe-reverse-engineering.md`, the cursor table): `cursornormal`, `cursormove`, `cursorselect`, … | `tagpu_gui_surf.c` `tagpu_gui_cursor_frame` takes the frame from the packet; `sharp_cursor` draws it last in the UI's sharp layer. | The same frames from the player's `cursors.gaf` (`cursornormal` for the arrow and build placement, `cursormove` animating with a mobile unit selected), at the same size and hotspot, over the image (R2); the kit's icons where they did not resolve |
| The left sidebar | The engine's own column: PANELSIDE, the minimap, the routed .GUI screen and its gadgets' GAF frames (the engine map's *The left sidebar*) | `tagpu_wikiside.c` composes the column the engine would show into `HUD_TEX_SIDE`, drawn by one `HOP_SIDE` op in the HUD pass (`tagpu_vk_hud.c`; [GPU status](gpu-status.html) §2.12c) | `side.js` `compose`, the same composer, from the mod's pack |
| Keys strip | None: the game has no such strip | Only the wiki draws one, under its own stage: `tagpu_wikilay.c` lays out the keycaps of the keys the demo's view holds, drawn by the replay HUD's stack (`tagpu_huddraw.c`, `tagpu_hudtext.c`, `tagpu_vk_hud.c`). | Unchanged |

**The three op ranges.** Everything `stage.js` draws belongs to one range ([the 3D stage's
plan](ingame-wiki-3d-stage-plan.html), section 3): **R0** under the image, the stage's
background; **R1** the whole whitebox stage; **R2** the overlays over the image: order lines,
queued squares, selection brackets, the sites' and the ghost's squares and the sites' order
numbers, the drag box, the cursor, the readouts, and the whitebox box and label of any unit, site
or ghost whose model did not resolve. A frame whose 3D image is complete draws R0, the image and
R2; any other frame draws R0 and R1, the whitebox, whole. In the mockup, complete means every
record of the frame was emitted and the GL context reported no error and was not lost; in the game,
that the wiki's world target and every pass the frame expects (the terrain, and the features when
the stage has any) prepared on that frame, decided once before the HUD's pass builds the wiki
(`tagpu_vk.c`, [GPU status](gpu-status.html) §2.12d). **The
ground falls back whole**: when any tile or feature of the description failed to resolve, the
image carries neither terrain nor features, and R1's ground and fog are drawn under it. **Units
fall back per type**: a type without a model keeps its whitebox box in R2. `stage.js` names each
call's range (`o.layer`), and `tools/wiki/stagecheck.mjs` pairs R0 and R2, over three stand-in
images (every model resolved, none, the ground fallen back), with R1's own C ops kept by the same
rule.

**The generator.** `tools/wiki/gfx.py` writes `tools/wiki/mockup/gfx/`, which is gitignored:
everything in it is derived from the game's files. Run it with the shared venv
(`.venv-undither/bin/python tools/wiki/gfx.py`); it takes `--gamedir`, else `TA3DO_GAMEDIR`,
else Steam's default library, and `--exe`, the executable whose directory names it reads. It
resolves by the engine's rules through `tools/wiki/tavfs.py` (loose files first, then the first
archive in the engine's mount order; an enumeration lists each path once, from its first source),
with `tools/ta3do`'s HPI, palette, GAF and 3DO readers and `tools/tascene`'s TNT and feature
readers:

- **The ground**: each source map's tile count and each referenced tile's FNV-1a hash compared
  with the description; each feature section found in the first `features\*.tdf` that holds it,
  with its GAF and sequence. Any miss and the entry's pack carries no ground.
- **The units**: `units\<UnitName>.FBI`; its `Objectname`, else the UnitName, names
  `objects3d\<name>.3DO`; `scripts\<UnitName>.COB` gives Create()'s hides, and is written beside
  the mesh as `units/<type>.cob` with its posed model's order and visible flags (the mesh's
  `script`; `posed_order`), for the units' scripts. Every piece's faces are written, Create()'s
  hides included, since a script can show what Create() hid: `units/<type>.pieces.bin` gives each
  vertex its piece (u16: the body's vertices, then the slant's, then the wire's) and the mesh's
  `pieces` table each piece's parent, cumulative rest offset in the file's axes, and whether
  Create() hides it or marks it dont-cache, both in the posed order (the walk's order for a type
  without a script). A face binds to the
  first GAF of the texture bank (`textures\*.GAF` without `logos.GAF`) that has its name, else to
  `logos.GAF`, as a team face when the entry has exactly 10 frames, else to flat colour `0xD1`.
  One mesh is written per owner the entry's scenes use, with the slant casters and the wire's
  edges.
- **The directory names** (`units`, `objects3d`, `scripts`, `textures`, `logos`, `anims`,
  `features`, `palettes`) are read out of the executable at their addresses, so a mod that renames
  one is followed; every open tries the `-english` variant of its folder first.
- **The output**: one tascene-format pack an entry, `gfx/<entry>/scene.json` and its binaries;
  `gfx/palette/` (PALETTE.PAL and the SHD table); `gfx/shaders/` (tagpu's GLSL, extracted from the
  repository's C sources); and `gfx/index.json` with the whole resolution report, which
  `--resolve-json` also writes to a file.
- **Colour**: palette colour throughout. No restorer runs: the stage is Classic.

**The renderer.** `tools/wiki/mockup/gfx.js` renders each frame into an offscreen WebGL2 target of
the map at zoom 1, 16 × `ss` pixels a cell, in the world passes' order: terrain, features (shadow
first), the units' Classic shadows, their bodies, the nanoframes' wire, then the ghost stage. Its
code is the tascene lab's parity lane, factored into `tools/tascene-render.js`, which
`tools/tascene-view.html` loads too; the lab's shots are md5-identical across the factoring.
`stage.js` draws the target into the map's rect, bilinear. Without `gfx/index.json`, `gfx=game`
stays on the whitebox and the controls strip says "game graphics: run tools/wiki/gfx.py".

**The units' poses.** The stage draws each unit in the pose its script sets
([below](#the-units-scripts)): a kbot's legs walk, a constructor opens and aims its nanolathe, an
extractor's arms turn. A unit that runs no script (none loaded, its type quarantined) and every
ghost draw the rest pose with what Create() hides left out. In the mockup `pose.js` composes the
host's posed model over the pack's piece table as `tagpu_wikiworld.c` `pose_compose` does, in
float32 step for step, and `gfx.js` culls the bodies' and the shadows' back faces as the game's
body pipeline does (`tagpu_vk_unit.c`), so a turned piece shows the face the game shows.

**The nano spray.** In the game it is the engine's particles (`PK_PART` kind 5, drawn by
`tagpu_fx.c`); on the stage it is the wiki's own particles, simulated in both simulations as the
engine simulates them and drawn by the stage's ops, not by the 3D world passes: one 2 × 2
game-pixel dot each, at the pixel the engine's leaf `0x473A00` puts it on, in the op range over
the image, so above the units as the engine's layer 6 is. `stage.js` draws the same in both looks
([the COB plan](ingame-wiki-cob-plan.html), *The spray*).

## Try it yourself in the game

Try it yourself is `wiki.js`'s, on the game thread (`tagpu_wiki.c`, *Try it yourself*); the
specification is the mockup's handlers, and [the plan](ingame-wiki-plan.html), landing 5, says
where the plan's first version and the mockup differed.

- **The simulation** is `sim.js` `create` on the selected demo's scene (`tagpu_wikisim_init` with no
  timeline, its units running their scripts as a demo's do): never done, the controls' play and replay acting on it as on a demo. Its button swaps the
  stage between Try it and the demo; picking an entry or a demo leaves Try it.
- **Input**, from the messages the window already takes while it is open, so the engine sees no
  more and no less than with the demo on the stage:
  - the pointer over the stage becomes `input({move})` through `tagpu_wikistage_fit` on the last
    drawn frame's canvas (its `stage` control) and unit, clamped to the map: the world point comes
    from the frame the player saw;
  - a left or right press on the stage gives a move there and then the button's down; a release
    anywhere gives its up;
  - Shift, Alt and Ctrl give a down and an up; Z and X with Alt down give a down every time, an
    auto-repeat included.
- **Nothing is left held.** What went down and not up is one ordered set; a close or a focus loss
  sends every member's up in that order; leaving Try it, a replay or a new selection empties it
  with the simulation it belonged to. A real press released off the window is let go at the first
  real move that carries its button up.
- **The build menu is the stage's sidebar.** Nothing in Try it gives `input({build})`: a press on
  the column is a press like any other, and the simulation's sidebar rules arm the building, page,
  switch tabs or stop ([below](#the-left-sidebar-on-the-stage)). The pointer is clamped to the
  stage's content rect, the column with the map, rather than to the map alone.
- **The cursor**: exactly one on screen. In Try it the stage draws the simulation's cursor only
  while the player's pointer is over its canvas (`tagpu_wikistage_build`'s `cursor`, `stage.js`'s
  `o.cursor`), and the player's is hidden only in a frame whose stage cursor was drawn: the HUD pass
  reports whether the op the build named (`WIKISTAGEOPS.cursor`) was given instances that were not
  hidden for a sprite sheet the device does not hold yet (`tagpu_wikistate_drawn`), so a sequence
  that ended, a sheet waiting for staging or an instance cap that was full leaves the player's own.
  The pointer's place, the build and the pass are one frame's (`tagpu_wikistate_cursor`). The
  mockup does the same: the stage's cursor while the pointer is over the canvas, and `#stage.try`
  (`cursor: none`) only in a frame that drew it. A demo draws its scripted cursor wherever the
  pointer is.
- **The test**: `wiki_regression.py <instance> try --launch` gives `snap` and `line`'s inputs as
  device input on the stage, compares the paused simulation's state hash with `sim.js` fed the
  inputs the game logged (`simparity.mjs --try`), and checks the releases on Back to demo and on
  close. A demo's `build` is armed as a player arms it: PREV or NEXT the nearer way round, then
  the building's button, each a release inside. It then pages, switches tabs and presses STOP on
  the sidebar, and reads every step back from `sim.js` fed the logged inputs
  (`sideview.mjs --dump`). `wikilaycheck.py` holds Try it's page to the mockup's (three Try it
  cases).

## The 3D stage under the mods

**What each supported setup shows** (MEASURED 2026-10-27 on the reference setup, Wine, each setup
of `tools/compat/setups.json` built as the compatibility suite builds it, on a private Xvfb with the
RTX presenting, at 1280 × 1024 and ss 2; `tools/wiki_regression.py all mods --shots DIR`, one setup
at a time under the run lock, about a minute each). Each run: a skirmish on Two Continents, the wiki
opened, its asset report `tagpu_wiki3d.json` read, Quick Build demo 3 at tick 180 and Build Lines
demo 2 at tick 100 captured through `tagpu_wiki.shot` with the whole display grabbed beside each,
a whole 300-frame line of each stage, and the display polled for any window that is not the game's
own, an `ErrorLog.txt` and the process at every step. The stage resolves through the engine's own
mount list in the engine's order (the plan's section 5), so the archive named is the one the
running game reads that file from.

| Setup | First archive; units folder | Ground: the two TNTs; the deposit and vent | Units: FBI and COB; 3DO; textures | Fell back | Error boxes | Pass |
|---|---|---|---|---|---|---|
| retail | `rev31.gp3`; `units` | `tactics6.hpi` (Crossing Two Rivers), `totala2.hpi` (Metal Heck); TDFs `rev31.gp3`, frames `btdata.ccx` | `rev31.gp3`; `totala1.hpi`; `rev31.gp3` | nothing | none | yes |
| Escalation (`escalation`) | `taesc.gp3`; `unitsE` | the same TNTs; TDFs `taesc.gp3`, frames `btdata.ccx` | `taesc.gp3` for all nine; `taesc.gp3` for all nine (every shape its own); `btdata.ccx`, the team faces `taesc.gp3`, one animated face of ARMGEO `txesc.ufo` | nothing | none | yes |
| Total Mayhem (`mayhem-11.3.0`) | `mayhem.gp3`; `units` | the same TNTs; TDFs `mayhem.gp3`, frames `btdata.ccx` | `mayhem.gp3`; `mayhem.gp3` but ARMLLT and ARMSOLAR, `totala1.hpi` (Mayhem ships their FBIs, not their models); `mayhem.gp3` | nothing | none | yes |
| ProTA (`prota-4.8`) | `prota.gp3`; `units` | the same TNTs; TDFs `prota.gp3`, frames `btdata.ccx` | `prota.gp3` for all nine, file and model alike; `prota.gp3` | nothing | none | yes |
| TA Zero (`tazero-alpha5`) | `taz31.gp3`; `ZUnits` | the same TNTs; TDFs `taz31.gp3`, frames `btdata.ccx` | none: `ZUnits\` holds none of the nine (its 270 FBIs are TA Zero's own sides) | all nine types are the whitebox's boxes; the ground is 3D | none | yes |
| Twilight (`twilight-2.0b98`) | its own `rev31.gp3`; `units` | the same TNTs; TDFs and frames `rev31.gp3` | `rev31.gp3`; `totala1.hpi` but ARMLLT, `rev31.gp3` (the stock shape); `rev31.gp3` | nothing | none | yes |

- **Every setup drew both stages' ground in 3D on 300 of 300 frames**, the image whole: the ground
  in 3D on the five setups whose units resolved, and under TA Zero the ground in 3D with every unit
  a box over it (R2), as the plan's section 3 says a type the running game does not have is drawn.
- **The units are the mod's own.** Every unit model a mod ships is the one drawn: Escalation's nine
  (every shape hash differs from stock's; its ARMMOHO is visibly larger, the plan's risk 9), Mayhem's
  seven, and ProTA's nine, four of them a different shape from stock's. Create()'s hides follow the
  mod's scripts: Escalation hides a `shield` piece on five types, Mayhem hides the turrets, sleeves and
  barrels of its armed extractors and geothermal (ARMMEX, ARMMOHO, ARMGEO).
- **Against retail's captures at the same tick**: Twilight's are identical, pixel for pixel (its
  models and textures are retail's). Escalation's differ in 13 203 and 435 of 983 040 pixels,
  ProTA's in 25 286 and 17 663, Mayhem's in 27 671 and 14 902, all over units, and under Mayhem
  also over the deposits' highlights: the same frame of the same `btdata.ccx`, drawn in another
  colour of the same ramp. The stage takes the engine's live palette table
  (`tagpu_pal_engine`), and `palettes\PALETTE.PAL` resolves to the same `totala1.hpi` entry in all
  six setups, so Mayhem's running game holds another table for that ramp [INFERRED: the table was
  not read].
- **The load**: 42 ms retail (31 archives, an arena of 249 124 bytes), 71 ms Escalation (38,
  295 825), 57 ms Mayhem (32, 369 453), 80 ms ProTA (31, 286 563), 107 ms TA Zero (33, 19 392),
  152 ms Twilight (27, 253 711). The render thread's stage, over a line of a stage already built:
  the producer 0.062 to 0.074 ms and the passes 0.013 to 0.037 ms a frame on average under every
  setup; the line that spans the change of entry averages up to 0.064 ms in the passes, one frame
  of it 2.9 to 4.2 ms (the new stage built).
- **Not measured**: Windows; a network game under a mod; any setup at another resolution. The
  figures are the stock game's whatever the mod (see *Gaps*), so under TA Zero the demos are
  whitebox units on the game's ground.

## The left sidebar on the stage

**The stage is a slice of the game's screen.** It shows the running mod's left sidebar, 128 game
pixels wide, then the map, at one game pixel a world unit each, whatever the HUD scale. The
column is:

- the minimap's 128 × 128 square: the demo's map, the units' dots in their owners' colours and
  the camera box, which is the picture's border because the stage shows the whole map. It is
  never clickable (the owner's decision 6).
- under it, the page the engine's router would show for the selection.

Its build buttons, PREV and NEXT, the ORDERS and BUILD tabs and STOP work as the game's do.
Every other order button is drawn greyed. The owner's decisions are 1 to 12
[above](#the-sidebar-and-the-per-mod-tutorials-the-owners-decisions). The engine facts are the engine map's [*The
left sidebar: its pages, its buttons and who takes a
press*](exe-reverse-engineering.html#the-left-sidebar-its-pages-its-buttons-and-who-takes-a-press-disassembled-measured-2026-10-27).

### Which layout, and when a demo shows it

- **The layout is the running mod's**, read from its files by the engine's rules (*What the DLL
  reads*, below; in the mockup, the detected mod's pack). A mod's layout that is absent or
  refused is replaced by the fixture, `art/wiki/side/fixture.json` (*The data format*, below).
  With no layout at all there is no sidebar.
- **A demo shows the sidebar** when its `sidebar` says so. By default it shows it when the scene
  gives the player a mobile builder (owner 0, MaxVelocity > 0, WorkerTime > 0), which is every
  one of the 21 demos.
- **Try it yourself shows it** whenever the demo's scene gives the player one, whatever the demo
  said.
- **The sidebar is on or off for a simulation's whole life**, and the simulation keeps the layout
  it started with.
- **In the DLL a demo waits for the loader's answer** (`tagpu_wiki.c` `layout_now`). It starts on
  the mod's layout once the asset loader has published it (`WIKISIDE_READY`), and on the
  fixture once the loader has refused it or could not start. Until then the stage says
  "loading…" and no demo starts, so none ever runs on a layout that arrives later. The parity
  check and the capture trigger wait for the same answer.
- **A simulation's refusal** (a layout, the context, the entry's binding to the casting, a scene
  structure the casting's footprint no longer fits, the resolved script's data rule, memory) is
  shown on the stage in its own words, up to 127 bytes (`TAGPU_WIKI_MSG`), as the mockup shows its
  `simErr`.
- **The demo plays the player's side and Interface Type.** At every packet fill, on the game
  thread in game, `tagpu_packet_pub.c` reads the controlled player's side, byte `+0x95` of the
  PlayerInfo of the record `main+0x1B63 + 0x14B·byte[main+0x2A42]` -- the record the engine's
  sidebar router reads (0x41AD1C..0x41AD66), the viewer's in a replay -- and `main+0x37EFA == 1`,
  and hands both to `tagpu_wiki_fill` by value. The wiki maps the side through the published
  block's `side_of_engine`: a mapped side plays on the mod's layout with that side's casting; an
  unmapped one, or none, plays the fixture with no casting. A fill given another pair restarts the
  open window's demo, so no demo plays past one fill on a stale side or interface.
- **The text is filled on the render thread, as it is read.** The caption is filled on the game
  thread with the running demo's words; the title, subtitle, paragraphs, demo titles and tree
  leaves are filled by `tagpu_wikilay.c` from the record's two values -- the layout side whose
  casting names the units, and the Interface Type -- over the wiki's data and the published
  casting. Both are immutable and never freed, and the fill is a pure function of them, so the
  render thread reads nothing the game thread writes, and the record carries two small values
  rather than a copy of every text in the window.

### The rules

`sim.js` and `tagpu_wikisim.c` hold them bit for bit. Every rule below is the engine's, cited in
the code.

- **Where the cursor is.** Left of x = 0 is the column. Then comes the map, closed at its far
  edges. Below a map shorter than the column is the dead band. The map's rules read the cursor
  clamped onto the map, so a box dragged onto the column selects up to the map's edge.
- **The router.**
    - Nothing of the player's selected: the side's MAIN2.
    - Exactly one selected unit whose type has pages: its numbered page while its bit 22 is set,
      else its orders page.
    - Anything else, two builders of one type included: the side's GEN.

    Each unit carries bit 22 (`bp`) and its page. A unit of a type with pages starts at page 1
    with the bit set. The simulation routes after every input and every tick.

- **Gadget states**: −1 not drawn, 0 normal, 1 pressed or the current tab, 2 greyed.
    - The tabs follow bit 22.
    - Every order button but STOP is greyed (decision 7).
    - STOP is live while some selected unit of the player's has `canstop`.
    - A build button is live only when the entry defines its unit and the shown builder's build
      list holds it (decision 11).
- **A press on the column goes to the sidebar only inside the routed screen's rect**, as the
  engine's GUI takes a press only inside its window.
    - **Which button takes it.** Where buttons overlap, the last release-acting button in file
      order takes the press, and the first tab holding the point wins over every button. A greyed
      or hidden gadget takes nothing.
    - **When it acts.** A button acts on the release inside it, from either mouse button. A tab
      acts on its press.
    - **PREV and NEXT** step the shown unit's page, wrapping within 1..n, and set bit 22.
    - **ORDERS and BUILD** clear and set bit 22.
    - **A build button** arms its building, keeping the Shift-placement bit.
    - **STOP** disarms, keeps the Shift-placement bit, and empties every selected unit's orders.
- **A press off the screen and off the map** lands on the minimap, the column's rows outside the
  screen, or the band.
    - A right press there cancels an armed building and orders nothing.
    - An armed left press there places the building once, at the cursor clamped onto the map, and
      Shift keeps it armed. In the game the point is the view's edge; the stage shows the map where
      the game shows the view.
    - The band has no engine equivalent, and the same clamp is the nearest rule.
    - The minimap stays inert.
- **Off the map, the gesture's evaluation stands.** Its preview and its release are the shape the
  map last showed, as the build cursor `0x4197D0` runs only over the viewport.
- **The build trip.** A scripted demo with the sidebar plays `{ "build": T }` at tick t as the
  cursor's trip to T's button. The inputs are generated when tick t is fed, and they take the
  exact path a player's clicks take in Try it:
    - a glide to the pager, then the pager presses the nearer way round, a tie going to NEXT;
    - a glide to the button;
    - the button's press at t + 45 and its release at t + 60, which arms T;
    - a glide back, ending at t + 90.

    The trip takes `WIKI_TRIP` = 90 ticks whatever the page or the mod, so the armed mode and
    everything else in the world are the same in every mod at every tick; only the cursor and the
    sidebar differ. Row k of `WIKI_TRIP_ROWS` is the trip through k pager presses, k ≤ 3. The glide
    is exact in both languages: step i of n from a to b is
    `a + trunc((b − a) · i²(3n − 2i) / n³)`, each axis.

- **A build the layout cannot reach** stops the demo at that tick. The view's `locked` names the
  type, and the stage holds the frame under the entry's "Turned off in this mod" badge
  (decision 12). In TA Zero, whose sidebar has no page for the stock ARMCK, a demo stops at its
  first build: tick 0 or 20 in Build Lines, tick 20 in the three Quick Build demos that arm one.
- **Try it never sends `build`.** The player presses the sidebar. A simulation made with
  `create`, simcheck's rule cases among them, keeps `build` as an instant arm.
- **Two placement rules hold with the sidebar off as well**, deliberately and in both
  simulations: arming a building (`arm`) keeps the Shift-placement bit (`shift_placed`) and keeps
  the gesture when the type armed is the same one, as the engine's arm at `0x41AB89`/`0x41AB9C`
  leaves `main+0x2CC6` and `tagpu_place.c` ends a gesture only on another type; and the stage
  shows an evaluation only while the building it was made for is still armed (`placed`, as
  `tagpu_place.c`'s `current()`). So a demo without the sidebar is not the earlier simulation plus
  the view's `locked` field: these two rules change what it does too.

### How it is drawn

- **The content rect.** With the sidebar, the stage's content is x ∈ [−128, map width) and
  z ∈ [0, the taller of the map and the column), in world units. The map stays at the top, and
  the band under a shorter map is only R0's background.
- **The fit.** `tagpu_wikistage_fit(w, h, side_h, …)` fits that rect with the pad of 6 units. It
  is the one function the render thread draws with and the game thread turns a click into a stage
  point with. Without the sidebar it is the map's fit to the bit.
- **Where the column's ops go.** They are a range of their own, `side` (`WIKISTAGE_IN_SIDE`;
  `stage.js` `o.layer('side')`). The range is drawn after R0 and under the image, and it never
  overlaps the map's rect.
- **The whitebox column** is ops:
    - a solid for the column, then the minimap's well;
    - the minimap's cells in the ground's shades and its fog;
    - a dot per unit and the camera box;
    - the routed screen's panel;
    - per drawn gadget, a solid, a ring and its label.

    A greyed button is one group at opacity 0.45 (`WIKI_MAXGRP`, one a gadget, which the HUD pass's
    group bound holds at compile time). A pressed button takes the pressed colour, with its label
    moved one game pixel right and down. The colours are `stage.js` `SIDE_WB`, mirrored in
    `tagpu_wikistage.c`.

- **The game look**, used when the view runs on the mod's own layout, is one `HOP_SIDE` op. It
  draws the column `tagpu_wikiside.c` composed into `HUD_TEX_SIDE`, sampled nearest, with a
  whitebox button over each gadget whose picture is a label. A view on the fixture draws the
  whitebox column, since the mod's pictures belong to the mod's gadgets.
- **The composer** (`side.js` `compose`, `tagpu_wikiside.c` line for line) works in palette
  indices, then through the mod's palette into RGBA. In order:
    - palette index 0 over the whole column, as the engine clears its sidebar;
    - PANELSIDE at (0, 0);
    - the minimap: its base picture under the fog, the dots (`radlogo` frame by owner, and
      `radlogohigh` over the unit under the cursor), then the box;
    - the routed screen's panel, then each drawn gadget's frame for its state. A greyed gadget is
      tinted as the engine tints it, and a held one-frame button draws no picture, as the engine
      draws none.
- **The minimap's extent is the stage map**, where the engine uses its scroll extent, which would
  cut the stage's bottom rows off. Each stage's base picture is generated as `0x466780` generates
  one, shrunk 2:1 through the mod's `PALETTE.ALP`.

### What the DLL reads

`tagpu_wikiside_load` (`tagpu_wikiasset.c`) runs on the asset loader's thread before the 3D load.
It reads every file by the engine's path rule, the language's folder first, with a 0-byte file
counting as missing:

- `<units>\*.FBI`: `UnitName`, `canstop`, the display name and the cast rule's fields (*The
  per-mod tutorials*, below), the first file listed winning for a UnitName (`0x488B10`);
- `<gamedata>\sidedata.TDF`: each side's `name`, `nameprefix`, `intgaf` and `commander`;
- `<download>\*.TDF`, read by position;
- each side's MAIN2 and GEN, and the pages and orders page of each type the side's casting
  names, read with that side's own prefix, with their download fill (every side with MAIN2 and
  GEN is a layout side; the sidebar is refused only when no side has them);
- the pictures by decision 5's rule:
    - for an order, tab or pager: `<anims>\<SCREEN>.GAF`, then commongui, then commongui's
      generic picture;
    - for a build button: the page's own picture by the engine's rule, then
      `<anims>\<UNIT>_gadget.GAF`, then `<unitpics>\<UNIT>.PCX`, else a labelled button;
- each layout side's PANELSIDE;
- `fx.GAF`'s dots;
- the palette, its `.ALP` and `.SHD`, and `guipal`.

Mayhem's and ProTA's build pictures come by that rule from their own `commongui.GAF`, under the
gadget's name (the engine's second lookup, after the screen's own GAF), not from `unitpics` PCX
files their DLLs draw; and TA Zero's pages are templates its `ZBuildMenu` `_h` files fill by the
stock download rule (the engine map's *The left sidebar*). MEASURED from the packs `gfx.py`
resolves.

A GAF is read only in the 64 KiB archive pages its lookups touch: Escalation's `fx.GAF` is 82 MB
(`TAGPU_HPI_READMAX` bounds a read's range, not the file).

- **The .GUI files are read by position.** `tagpu_guifile.c` is the engine's own grammar,
  sections in file order, because retail pages repeat section names, which `tagpu_tdf` would
  merge.
- **The result is one immutable block.** The layout passes `tagpu_wikilayout_check`, every frame
  rect lies inside the sheet, and one interlocked store publishes the block, which is never
  freed (`tagpu_wikiside_get`; [GPU status](gpu-status.html) §2.12c).
- **The log line** is `wikiside: loaded in N ms - S screens, T types, R rows, a H-row sheet`. The
  trigger `tagpu_wiki3d.dump` also writes the sidebar's report, `tagpu_wikiside.json`.

**Where it departs from the engine**, as `gfx.py` does (the two are compared with each other, not
with the engine):

- **A build button with only commongui's generic picture** skips that picture and keeps the
  .GUI's rect, where the engine would shrink a 64 × 64 slot to a 96 × 20 `BUTTONS0` frame.
- **A download entry whose `BUTTON + 4` is no gadget of its page** is skipped and reported. The
  engine has no bound there.
- **A type with more than 7 numbered pages** keeps none, and the router shows GEN for it.
- **The format strings** (`%s%d.GUI`, `%sDL`, `_gadget`, …) are the retail executable's. A mod
  that patched one would not be followed.

### The checks

The offline checks were MEASURED 2026-10-28 on this tree, the game's on 2026-10-27, before each
side had its own types.

- **`simparity.mjs`** plays every demo through both simulations with the fixture's sidebar, with
  `fixture-tall.json`'s 768-row column and with none, each in the eight contexts of *The
  simulation's contract* (below), with the units' scripts and without: 1008 plays. It replays the
  47 rule cases and the 180 refusal cases (the `sidebar` flag, the trip's data rule, every bound
  of the layout on both sides of it, the context, the binding and the scene's fit), reads 24
  castings and layouts back, then the 21 malformed-data cases, and compares the trip's constants.
  All equal, natively and with `--wine`.
- **`stagecheck.mjs`** pairs the stage's ops with `stage.js`'s calls, the `side` range and the
  greyed buttons' groups included: 6550 frames, the fixture at three sizes, the tall fixture at
  1920 × 1080, no sidebar at three sizes, and, when a gfx pack holds the units' scripts, the
  fixture at the first size with them (`sim.js` through `serve.py`'s host, the C side from the same
  files through `tests/wikicobfile.h`): 447 frames, the nano spray in 53 of them, 8325 particles.
  With a sidebar it draws the whitebox column, the game look's image with its minimap, and the
  image without it, over a stand-in image.
- **`sidecheck.mjs`** compares the two composers byte for byte (with
  `tagpu/ddraw/tests/wikisidetest.c`), every side of every pack at both Interface Types with its
  casting: 8 packs and 18 740 frames. Two packs are synthetic, generated from a fixed seed to take
  every branch of the composer. The other six are the compat setups' own packs, present locally.
- **`assetcheck.py --side`** compares the DLL's loader (`wikiassettest side`) with `gfx.py
  --side-only` per compat setup: the layout, every picture's source and frames, PANELSIDE, the
  dots, the minimap's tables and bases, the greyed tint and the sheet byte for byte, and per side
  its PANELSIDE, its types and its casting, the display names under `--language` and the 3D
  load's unit types. `fuzz-side` damages synthetic files. All six setups equal.
- **The composed column against the game.** `tools/wiki/sidedump.mjs` composes a pack's column
  for a named state as a PNG, and it equals the game's own sidebar pixel for pixel in all six
  setups, in every captured state but the cursor, the minimap's content and TA Zero's run-time
  states. The positional reader agrees with an emulation of the engine's parser on all 1581
  .GUI files of the six setups.
- **The game's demos** (`wiki_regression.py demos`, (d1)) play the 21 demos on the sidebar layout
  a demo would start on, every hash equal to `simparity.mjs --hash --layout` on the layout the
  check wrote (2026-10-27). `tagpu_wiki.check` now plays them for every side of that layout, with
  the side's casting on the mod's layout, at both Interface Types, and logs
  `<entry>/<demo>/<side>/<iface> <hash>`, which `simparity.mjs --hash --side S --iface I` prints;
  (d1) and (b2) compare every key, with `--pack` the side pack of the game's layout when it is the
  mod's, and fail on a context played on one side only, a different hash or a demo refused on one
  side only. (d4)'s mockup beside the game runs in the game look on the stock pack's sidebar.
- **Try it in the game** (`wiki_regression.py try`) arms each demo's builds through the stage's
  sidebar, as described [above](#try-it-yourself-in-the-game). In (t2c) NEXT and PREV page the
  ARMCK, ORDERS shows the side's GEN and BUILD its page again, and STOP disarms and empties the
  orders. Every step is read back from `sim.js` fed the logged inputs, once its hash equals the
  game's (`sideview.mjs --dump`).
- **Every mod in the game** (`wiki_regression.py all mods`) plays one game a compatibility setup
  on a private 2560 × 1440 display:
    - **(b1)** the setup's own sidebar, cropped from the engine's frame below the minimap, against
      `sidedump.mjs` on the setup's pack, greyed as the engine's snapshot of the screen has it:
      with nothing selected, and with an ARMCK placed and selected where the pack gives it pages;
    - **(b2)** the layout every demo starts on is the mod's, `tagpu_wiki.layout.json` equal to the
      pack's, and all 21 hashes equal to `simparity.mjs --hash --layout` of that file;
    - **(b3)** Quick Build demo 1 held at tick 10: the presented frame's column, each client pixel
      mapped back to the texel its nearest sampling shows, against the composer on the pack for
      the same view (`sideview.mjs --play --compose`);
    - **(b4)** Build Lines demo 1 played: where the mod's sidebar cannot reach its first build it
      holds at that tick under the badge, and elsewhere it plays past it.

  (b1) and (b3) make one chain: the stage equals the composer on the pack and the wiki's state,
  and the composer on the pack and the engine's state equals the engine's own sidebar. MEASURED
  2026-10-27 under Wine: all six setups passed (retail, Escalation, Total Mayhem 11.3.0, ProTA
  4.8, TA Zero alpha 5, Twilight 2.0b98), 0 pixels differing in every comparison. (b4) held TA
  Zero's demo at its lock, tick 20, and played past it in the other five.

**The game's files for the checks.**

- `tagpu_wiki.check` writes the layout it plays on to `tagpu_wiki.layout.json`, in the layout
  schema, or `null` for none. It logs `wiki check: the sidebar layout is the mod's / the fixture
  / none ...; the units' scripts are on / off` and waits, as a demo does, for the loader's answer
  and the 3D load's. After a demo's hash it logs each quarantined type's note
  (`<TYPE>'s script is quarantined - <note>`) and a demo a wait stopped (`stopped: a wait ran
  past its max`); `wiki_regression.py` passes `--scripts` to `simparity.mjs --hash` when the log
  says the scripts are on.
- The state dump `tagpu_wiki.json` gains three fields: `side_h`, the running simulation's
  column rows, 0 without one; `layout_src`, one of `"mod"`, `"fixture"`, `"none"`, or `""`
  with no simulation; and `layout`, the layout itself, which `simparity.mjs --try` and
  `sideview.mjs --dump` replay. Its `ctx` carries `cob`, whether the simulation runs its units'
  scripts, which `simparity.mjs --try` then takes from the gfx packs and `sideview.mjs --scripts
  DIR` from a folder. `tools/wiki/sideview.mjs` plays `sim.js` to a dump's state and
  prints its view (`--dump`), or plays a demo to a tick and composes its column (`--play
  --compose`).
- Layout names are written as their UTF-8 bytes, as both readers count them.

## The per-mod tutorials

**Every demo plays the player's side of the running mod, in the player's Interface Type.** An
entry names each of its units by a **cast** -- `builder`, `advbuilder`, `mex`, `moho`, `geo`,
`walker`, `tower`, `wall`, `solar` -- and a side's **casting** answers every cast with one of that
side's own types: its FBI figures, its G cells and its in-game display name. The text names units
and mouse buttons by tokens, filled with the casting's names and the player's buttons. The owner's
decisions are 13 to 26 [above](#the-sidebar-and-the-per-mod-tutorials-the-owners-decisions); the
engine facts are the engine map's *The wiki's per-mod tutorials* and *What each Interface Type does
with a press* ([engine map](exe-reverse-engineering.html)).

### The cast rule

It names no mod, lists no unit and reads no unit's `Side=`; `gfx.py` (`side_casting`) and
`tagpu_wikiasset.c` run it per layout side, the sides of sidedata that have MAIN2 and GEN, in the
engine's order.

- **The walk.** Breadth-first from the side's `commander` (sidedata's `[SIDEk]`, matched without
  case) through each type's **menu**: the units of its build buttons on its numbered pages as the
  sidebar reads them for that side (the page probe, else `<prefix>DL` with the download fill; the
  greyed pass; the 15- and 31-character name cuts and the role order of `0x41AA00`), in (page,
  gadget) order, hidden and greyed buttons left out, each unit once. The order in which units are
  first met ranks them by tech level (their depth from the commander) first and by the mod's own
  menu order second, which is total, so no tie is left to break.
- **The predicates**, over each FBI as the engine parses it (integers as `atoi`, figures as `strtod`
  of the first number, a missing key 0, `BMcode` included):
    - *mobile*: BMcode ≠ 0 and MaxVelocity > 0; *structure*: BMcode 0 and both footprint sides > 0;
    - *builder*: Builder bit 0 and WorkerTime > 0; *land*: no CanFly, and MinWaterDepth absent or
      ≤ 0;
    - *extractor*: a structure with ExtractsMetal > 0; *geothermal*: a structure, not an
      extractor, whose yard holds a G cell; *plain*: a land structure that is neither and not a
      builder.

| cast | the pick |
|---|---|
| builder | the first unit met, the commander excluded, that is a mobile land builder whose menu holds a land extractor |
| mex | the land extractor on the builder's menu with the greatest ExtractsMetal; equal, the earlier on the menu |
| geo | the land geothermal plant on the builder's menu with the greatest EnergyMake − EnergyUse; equal, the earlier |
| advbuilder | the first unit met, the builder excluded, that is a mobile land builder whose menu holds a land extractor stronger than the mex |
| moho | the strongest land extractor on the advbuilder's menu that is stronger than the mex |
| walker | the first mobile land non-builder on the menu of the first unit met whose menu holds the builder |
| tower | the first plain structure on the builder's menu with a Weapon1 |
| wall | the first plain structure on the builder's menu with no Weapon1 and TEDClass `FORT` (without case) |
| solar | the first plain structure on the builder's menu with EnergyMake − EnergyUse > 0 (a solar collector's energy is a negative EnergyUse: retail ARMSOLAR's is −20) |

- **Every cast's unit is distinct**: one unit in two casts would bind two of an entry's defs to one
  type, which the binder refuses for every demo of the entry. Tower, wall and solar are drawn from
  one list, the builder's plain structures, and each skips a unit an earlier cast took, in that
  order: a tower that makes energy is the tower, not the solar too. The other casts' tests already
  exclude one another, and a last pass in the casts' order unbinds a pick that still repeats an
  earlier cast's, with the reason, so the casts are distinct whatever the tests. No pick of the six
  setups changed (`assetcheck.py`'s synthetic install has the collision).
- A cast the rule cannot fill, or whose type's footprint is not 1 to 16 cells a side
  (`WIKI_FP_MAX`, the width of a G row's mask), is **unbound**, and the loader's report says why.
- **What pins it: the stock identity.** On the stock game it gives ARM exactly the types and
  figures the entries author, cast for cast, name and build list included, and CORE their
  counterparts (`sidepacks.py` `stock_identity`, run on every stock pack).

**The picks** (MEASURED offline 2026-10-28, `gfx.py --side-only` over the six compatibility
setups; the C loader's are the same, `assetcheck.py --side`). No cast is unbound in any of the 13
sides, and no strength is tied anywhere.

| setup / side | builder | advbuilder | mex | moho | geo | walker | tower | wall | solar |
|---|---|---|---|---|---|---|---|---|---|
| stock ARM | ARMCK | ARMACK | ARMMEX | ARMMOHO | ARMGEO | ARMPW | ARMLLT | ARMDRAG | ARMSOLAR |
| stock CORE | CORCK | CORACK | CORMEX | CORMOHO | CORGEO | CORAK | CORLLT | CORDRAG | CORSOLAR |
| Mayhem, ProTA: ARM / CORE | as stock | | | | | | | | |
| Twilight ARM | ARMCK | ARMACK | ARMAMEX | ARMMOHO_TWILIGHT | ARMGEO | ARMFLEA (1 × 1) | ARMLLT | ARMDRAG | ARMSOLAR |
| Twilight CORE | CORCK | CORACK | COREXP | CORMEXP | CORGEO | CORAK | CORLLT | CORDRAG | CORSOLAR |
| Escalation ARM | ARMCK | ARMACK | ARMMEX | ARMMOHO | ARMGEO | ARMMARK | ARMLLT | ARMDRAG | ARMSOLAR |
| Escalation CORE | CORCK | CORACK | CORMEX | CORMOHO | CORGEO | CORVOYR | CORLLT | CORDRAG | CORSOLAR |
| TA Zero GOK | GoKT1ConHover (3 × 3) | GoKT2ConHover (4 × 4) | GoKT1Mex | GoKT2Mex | GoKT1Geo (5 × 5) | GoKT1ScoutHover (3 × 3) | GoKT1PDTurret | GoKT1PD | GoKT1PGen (3 × 3) |
| TA Zero ARM | ArmT1FEKbot | ArmT2ConTank (4 × 4) | ArmT1Mex | ArmT2Mex | ArmT1Geo (5 × 5) | ArmT1ReconHover | ArmT1PDTurret | ArmT1PD | ArmT1Solar |
| TA Zero CORE | CoreT1ConTank (3 × 3) | CoreT2ConTank (4 × 4) | CoreT1Mex | CoreT2Mex | CoreT1Geo (5 × 5) | CoreT1ScoutBike | CoreT1PDTurret | CoreT1PD | CoreT1PGen (3 × 3) |

A footprint in brackets differs from stock ARM's. The towers and walls are 2 × 2 everywhere, so
Build Lines' counts keep their premise. Picks that read oddly but break no demo: TA Zero ARM's
advbuilder is a tank (its T2 tank factory comes first on the commander's menu); Twilight's mex is
its stealth extractor, the strongest on the menu; Escalation's walkers are radar kbots; TA Zero's
walkers are scouts, its GOK builder a hover and its CORE builder a tank (neither side has a T1
kbot builder); TA Zero's geothermal plants make no EnergyMake (Quick Build is off there).

### Casts, tokens and actions in the data

- **Every unit of an entry names its cast** (`"cast": "builder"`), and no two units of one entry
  name the same one; both loaders refuse otherwise. The authored types and figures stay: they are
  stock ARM's, which the rule reproduces.
- **Tokens.** In an entry's title, subtitle and paragraphs, its demos' titles and every caption:
    - `{builder}` … `{solar}` (lower case only): the bound cast's display name (its type when the
      name is empty); with no casting or an unbound cast, the name of the entry's unit with that
      cast, else its key;
    - `{order}` / `{Order}`: `right-click` / `Right-click` at Interface Type 1, `left-click` /
      `Left-click` at any other value; `{select}` / `{Select}`: `click` / `Click`;
      `{cancel}` / `{Cancel}`: `right-click` / `Right-click`; key names are written as they are.

  A token is a `{` and everything to the next `}`. Both loaders refuse an entry with a `{` and no
  `}`, `{}`, a name that is neither a cast nor a button word, a capitalised cast or a cast no unit
  of the entry has, so a typo fails the build and never the player. The fill is presentation only:
  the simulation and its hash keep the raw text, so a hash depends on neither the language nor the
  side's names. One pure function fills, `text.js` `fillText` and `tagpu_wikitext_fill`, cut at a
  UTF-8 character as the stage's copy cuts.
- **The caption's bound.** A filled caption has 511 bytes and its terminator (`WIKI_CAPTION_MAX`,
  the stage record's and the page's field), and both loaders refuse an entry one of whose captions
  could fill past it with any casting at either type: its own bytes with a button token at its
  longer word and a cast token at the longer of 93 bytes, a casting name's bound, and the entry's
  own name for that cast (`text.js` `captionWorst`, `tagpu_wikidata.c` `caption_worst`). So no
  caption is ever cut; the longest bound today is 350 bytes. `textparity.mjs` fills every caption
  with every pack's casting and a casting of 93-byte names and checks each fits, and
  `tagpu_wikilay_check` builds the page with each casting it is given and with the worst ones
  (*Gaps* says what it checks of the caption's strip).
- **Actions.** A demo's press names what it does: `select` (the left button at both types),
  `order` (the right button at type 1, the left at type 0), `cancel` (the right button at both);
  `lmb` and `rmb` stay for presses that are one button at both types (a placement, a sidebar
  press). The simulation resolves the actions once, at creation, into its own copy of the script,
  and then holds it to **the data rule**: a press of a button already held, or a release of one
  that is up, refuses the demo with the input's tick. `simcheck.mjs` plays every demo at both
  types, so an authored overlap fails there and never reaches a player.

### The two interface types in the simulations

**The click window**, the same at both types (`sim.js` `CLICK_TIME`, `CLICK_BOX`;
`tagpu_wikisim.c` the same): a left release on the map is a click when its press is under 25 ticks
old and the release point under 32 world units from the press point on each axis; any other release
is a box. That is the engine's test at
`0x4993E9..0x499441`: the press is stamped with the clock `0x4B6340` (MEASURED at 30 units a second,
so 25 units are 25 ticks) and the world point's x and z words, and the release compares the last
point before it with the press's, so a wobble that comes back is a click. The 4 world units past
which the stage draws the box only decide the drawing. Try it counts the window in the stage's
ticks too, as every rule of the simulation is counted (it reads no clock), so at 2× it is half the
engine's 833 ms, and on a paused stage a hold never runs out.

Type 1 is the rows the stage played before. At type 0 (`sim.js` `leftClick`, `rightPress0`;
`tagpu_wikisim.c` the same), with nothing armed on the map:

- **a left click** -- the release of a press that went down on the map, inside the click window --
  is Quick Build's choice first, exactly as type 1's right click gives it (Impure's gate is on the
  left click at type 0, `tagpu_resource.c`); else over a unit of the player's, an unfinished
  building with a builder selected is assisted and any other unit selected; else the selected
  mobile units move, and with none nothing happens. The order lands **at the release**, where type
  1's right press orders at the press.
- **a right press** deselects every unit; with Ctrl it does nothing, since the engine's Ctrl +
  right press hands the mouse to a view mode (`0x41CC60`) and the stage has no camera to move.

Everything armed, every sidebar press, box drags and the minimap are the same at both types; an
armed right press disarms at both. The engine's type-0 rows were MEASURED in game on 2026-10-28 (the
engine map has the numbers): the move at the release, Quick Build on a deposit and a vent, the
assist on an unfinished building with cursor 6, the right press's deselect. [INFERRED]: an
unfinished building of the player's with no builder selected is selected.

### The bound entry: the casting's figures and G cells

- **With a casting the simulation plays its own copy of the entry** (`sim.js` `binder`,
  `tagpu_wikisim.c`): each def of a bound cast takes the casting's type, display name, footprint,
  BuildTime, WorkerTime, BuildDistance, MaxVelocity, ExtractsMetal, EnergyMake, `canstop` and G
  cells, and a build list by cast (def j is on it when def j's cast is on the cast's menu). A def
  of an unbound cast keeps the authored one. Indices do not move, so the scene and the inputs name
  the same defs, and the copy lives and dies with the simulation.
- **A context is refused** -- shown on the stage in its own words -- when the Interface Type is not
  0 or 1, the side is not one of the layout's, a unit has no cast, two units share one, two defs
  bind one type, or a scene structure's bound footprint leaves the placeable interior (cells 1 to
  w − 2) or overlaps one placed before it ("`<id>`'s `<type>` does not fit the scene"). No context
  of the six setups is refused.
- **Only G cells take a vent.** A geothermal plant needs a vent under one of its yard's G cells,
  and the vent it anchors to is the one under a G cell, as the engine's site test `0x47D2E0` and
  Impure's `geo_anchor` read it. Retail's plants are G throughout, so no stock hash moved; every
  other setup's plant has G only inside its yard (MEASURED in Mayhem in game: a vent under an outer
  cell refused, under a G cell placed).
- **Display names**: the FBI's `<language>name` when the key exists, else `name`, its first 31
  bytes (the engine's `def+0x00` is 0x20 bytes), converted from Windows-1252 to UTF-8 (a byte with
  no character there becomes `?`), the UnitName when that is empty; at most 93 bytes, 94 with the
  terminator (`WIKI_TITLE_MAX`). The language is the game's own, `0x51FB50` (a command-line
  argument that is no option, else the registry's value, else English), copied on the game thread
  (`gfx.py` takes `--language`, English by default). MEASURED in game with `language=french`: the
  engine's names are the French keys with their trailing blanks gone and their accents in
  Windows-1252.

### Locks, and the claims a turned-off feature does not check

- **A demo locks** -- holds its frame under the entry's "Turned off in this mod" badge -- at the
  first of: a scene unit of an unbound cast (tick 0), a `build` of an unbound cast (its tick), a
  build the layout cannot reach (its tick). The first lock wins, the tick finishes, and the lock
  holds with the sidebar off too. A Try it whose scene holds a unit of an unbound cast is locked
  at tick 0, input ignored, and Back to demo works. In the six setups no cast is unbound, so no
  demo locks for one.
- **An entry the mod turns off** (its own `lockedIn`: Quick Build in Escalation, TA Zero and an
  unidentified mod) plays under the badge, its demos to their end. `simcheck.mjs --pack` plays
  them and does **not** check their claims there, saying so ("locked: claims not checked") and
  naming the entries (decision 24): the badge already says the demo does not apply, and the rule
  reads the entry's own list, never a list of mods. TA Zero makes two of Quick Build's claims false
  (its cancel, and geothermal plants that make no energy).
- **A scene a mod makes false is redesigned** (decision 25). Build Lines' ring around a queued
  build brings its own builder into the map's top left and stands the far solar collector in the
  bottom right, 615 world units away, so every side's builder is still walking at the ring click
  (tick 336; TA Zero's builders start the far one at 349 and 357). Quick Build's helpers demo walks
  its walker toward the far corner, so a walker of up to 2.8 world units a tick stays in the box
  and keeps walking at both types. The geothermal demo's tail is 465 ticks, set by Twilight
  CORE's plant (decision 26).

### The side, the Interface Type and the text in the game

The game thread reads the controlled player's side and `main+0x37EFA == 1` at every packet fill
and plays the demo on them; the render thread fills the page's text from the record's two values
(*Which layout, and when a demo shows it*, above, has the rule; [GPU status](gpu-status.html)
§2.12c the sites).

- **The side is the engine sidebar's**: PlayerInfo `+0x95` of the record `main+0x2A42` names. In a
  replay that is the viewer's record, 10, whose side byte was the recorded player's in both
  replays measured (decision 22). A side byte that maps to no layout side plays the fixture with no
  casting, so a wrong byte shows another side's tutorial and reads nothing outside the published
  block.
- **A change of side or Interface Type restarts the open window's demo** at the next fill, and the
  log says `wiki: the player's side is S and the interface type I`.
- **Why the text is filled on the render thread**: the title, subtitle, paragraphs, demo titles
  and tree leaves are read by the layout (`tagpu_wikilay_build`) on the render thread. Filling them
  there is a pure function of the wiki's data and the published casting, both immutable and never
  freed, and of two values the record carries by value, so the render thread reads nothing the
  game thread writes, and the record carries two small values rather than every text of the window
  filled. The caption alone is filled on the game thread, where the stage's view is copied.

### The checks

All MEASURED 2026-10-28 on this tree (native gcc with AddressSanitizer and UndefinedBehaviorSanitizer
unless said).

- **`simcheck.mjs`** plays every demo with the fixture in eight contexts (the authored types, and
  the `identity`, `shifted` and `unbound` castings, each at types 1 and 0), and each again with the
  units' scripts (`+scripts`; under `--pack` each mod's own stage scripts first): 21 of 21 pass in each,
  `identity` held to the authored play frame for frame, and `unbound` stopping under the badge
  where its wall is built (five Build Lines demos at tick 0, the ring around a queued build at 206).
  **`--pack`** plays each mod's pack in every side at both types: stock, Mayhem, ProTA and Twilight
  21 of 21 in every side; Escalation and TA Zero Build Lines' 10 of 10 in every side, and Quick
  Build's 11 played with their claims unchecked.
- **`simparity.mjs`**: 1008 demo plays (21 demos × the fixture, the tall fixture and no sidebar ×
  the eight contexts × with and without the units' scripts), 47 rule cases, 180 refusal cases, 24
  castings and layouts read back and 21 malformed-data cases, all equal.
- **`textparity.mjs`**: 18 castings at Interface Type 1, 0, 2 and −1 over both entries and 24 edge
  strings, 12 600 fills and 27 656 cut fills, equal byte for byte.
- **`assetcheck.py --side`**: the DLL's loader equal to `gfx.py --side-only` in all six setups,
  the castings (2 sides × 9 casts bound in five setups, 3 × 9 in TA Zero), the per-side types and
  PANELSIDE, the display names and the 3D load's unit types included; the sidebar loads in 244 to
  436 ms in that sanitised native build.
- **The game** (2026-10-28, retail as CORE under Wine): the log said `the player's side is 1 and
  the interface type 1`; the Quick Build demo's caption read "Construction Kbot selected. Pick the
  Metal Extractor in the build menu." over CORCK's figures; `tagpu_wiki.check` played 84 demos (2
  sides × 2 types × 21), none refused, and all 84 hashes equal `simparity.mjs --hash --pack` on the
  stock pack, compared afterwards from the log. After a typed `+iface 0` the log said `the interface type 0`
  and the click demo's caption read "Left-click builds it." where it had read "Right-click".
  With the units' scripts (`wiki_regression.py <instance> wikicob demos --launch`, a private Xvfb
  at 1920 × 1080): `wiki check: 84 demos played, 0 did not start, in 89 ms`, the scripts on, all 84
  hashes equal `simparity.mjs --hash --scripts`.
- **`tools/wiki_regression.py - offline`** runs, in one run with no lock, `simcheck` (plain,
  `--null-layout` and `--pack` for each pack present), `simparity` and `textparity` natively and
  `--wine`, `sidecheck`, `stagecheck` and `scenecheck`: 522 s, all passing. `assetcheck.py --side`
  and `wikilaycheck.py` are run on their own.

## The first entry: Quick Build: Resource Extractors

The feature is [Resource snapping and Quick Build](tadr-port/resource-snapping.md). BAR's names
for the same ideas are *Quick Build Extractor* (its widget) and *Upgrade T1 » T2*. Its demos, named here in stock ARM's units, as the fixture plays them; on a mod's own layout each
unit is its side's cast, and "the order click" is the right click at Interface Type 1 and the left
click at type 0:

| # | Demo | What the player sees |
|---|---|---|
| 1 | Snap onto the deposit | an ARMMEX placed from the build menu near a deposit lands on it (search radius 3) |
| 2 | Hold Alt to place exactly | with Alt held the ghost stays where the cursor is |
| 3 | Right-click a deposit to build (Left-click at type 0) | with a constructor selected, hovering a deposit shows the ghost; the order click builds, no build menu |
| 4 | The best extractor that fits | ARMCK and ARMACK selected: ARMMOHO where it fits; on a deposit too small for it, ARMMEX |
| 5 | Queue several with Shift | Shift + the order click, deposit after deposit; the mode stays on |
| 6 | Cancel a queued one | Shift + the order click on its site again |
| 7 | Helpers assist | in a mixed selection, the builders that cannot make it guard the ones that can |
| 8 | Upgrade your extractor | the order click on a finished ARMMEX with ARMACK selected: reclaimed, then ARMMOHO built |
| 9 | Geothermal vents | the same click on a vent builds ARMGEO |
| 10 | Metal maps | on uniform metal ground a click is a move; only a richer deposit builds |
| 11 | Fog | an unexplored deposit does not snap; one under grey fog does |

Every demo ends on its work done: after its last input it waits until no unit of the player's
holds a build or a reclaim order (at most 1800 ticks), then plays 60 more; *The best extractor
that fits* also waits between its two clicks, and *Metal maps* waits for its constructor to arrive
before its second (`gen-quickbuild.mjs` `until`, `TAIL`, `WAIT_MAX`). The ring and helpers demos
need their unit still walking at a click instead; a path round something only lengthens a walk, so
that holds.

**Mods that lock it off:** Escalation, TA Zero and an unidentified mod (the resource profiles,
`resource-snapping.md` *Scope and defaults*). Stock, Total Mayhem, ProTA and Twilight allow it.

## The second entry: Build Lines, Grids and Rings

The feature is [several buildings in one gesture](tadr-port/quality-of-life.md) (*Placing several
buildings in one gesture*, *What Impure does*), `tagpu_place.c` with its shapes in
`tagpu_placegeo.c`. Only what the game does today is shown: Build Split, which is not built, is
not.

| # | Demo | What the player sees |
|---|---|---|
| 1 | Click and Shift-click | a plain click places one ARMLLT and disarms; Shift-clicks queue two more and stay armed; letting go of Shift disarms |
| 2 | Line | Shift+drag lays seven ARMLLT at a shallow angle, each site snapped; letting go of Shift disarms. Then a second drag with Shift released before the button: the preview goes, nothing is ordered, ARMLLT stays armed |
| 3 | Straight line | Shift+Ctrl+drag: ten sites held to the press's row while the cursor wanders off it; the three over the ARMSOLAR are red and skipped |
| 4 | Filled rectangle | Shift+Alt+drag: a 4 × 3 block of ARMDRAG numbered in serpentine order, then built in that order |
| 5 | Hollow rectangle | Shift+Alt+Ctrl+drag from bottom right to top left: fourteen ARMDRAG round the edge, from beside the press back to it |
| 6 | Spacing | a Shift-drag line of seven ARMDRAG; Alt+Z twice mid-drag spreads it to five, then four, at once; released; armed again the next line starts spread, and Alt+X twice brings it back to seven |
| 7 | Ring around a building | Shift+Ctrl over the ARMSOLAR previews twelve ARMDRAG round it; a press and release orders them |
| 8 | Ring around a queued build | the demo's own ARMCK, top left, queues two ARMSOLAR by Shift-click, the far one in the bottom right and then a near one; it is still walking to the first when Shift+Ctrl over the second, still only a queued square, rings it |
| 9 | Placing it again removes it | a row of five ARMLLT; dragging back over three shows them yellow and the release takes them off, adding nothing; a Shift-click on a fourth takes it off too |
| 10 | Change the shape mid-drag | one drag: a line, Alt makes it a filled rectangle, Ctrl hollow, and the release orders the hollow one; a second drag is cancelled by a right press, which also disarms |

**Mods that lock it off:** none. `tagpu_place_init` (`dllmain.c`) arms it whatever the mod, and no
resource profile reads it.

**The units**, stock ARM, read from the retail unit files as for Quick Build (rev31.gp3's copy,
the one loaded):

| Unit | Footprint | BuildTime | Role |
|---|---|---|---|
| ARMCK | 2 × 2 | 5597 | the one selected builder (WorkerTime 80, BuildDistance 40, MaxVelocity 0.8) |
| ARMLLT | 2 × 2 | 4662 | lines and the toggle |
| ARMDRAG | 2 × 2 | 520 | rectangles, walls, spacing and rings |
| ARMSOLAR | 5 × 5 | 2495 | the finished building to ring and to be refused over, and the queued one to ring |

Every demo has exactly one ARMCK selected: the stock interface arms a building only with one unit
selected (`quality-of-life.md` *Who builds*). The entry's `pace` is 24: an ARMLLT takes ARMCK
2.4 s on the stage (58 s in the game), an ARMDRAG 0.27 s. The demos end on the shape and its
first pieces, not the whole queue: with its script ARMCK folds and unfolds its nanolathe between
two queued pieces, about 268 ticks a piece, so a whole line would run for minutes. *Click and
Shift-click* and *Filled rectangle* wait until two pieces are finished and *Ring around a
building* until one is (`until: "built"`), then play 60 ticks; the others end a fixed tail after
their last input, since their claims read only the shape and the queue.

## The data format

### `art/wiki/tree.json`

```json
{ "format": 1,
  "tree": [ { "title": "Quality of Life", "children": [
              { "title": "Building", "children": [ { "entry": "quickbuild" } ] } ] } ] }
```

A node is a category (`title`, `children`) or a leaf (`entry`, the id of `entries/<id>.json`).

### `art/wiki/entries/<id>.json`

```json
{ "format": 1, "id": "quickbuild",
  "title": "Quick Build: Resource Extractors",
  "subtitle": "…",
  "text": ["paragraph", "…"],
  "lockedIn": ["escalation", "tazero", "unknown"],
  "units": { "ARMMEX": { "cast": "mex", "footprint": [3, 3], "extracts": 0.001, "…": "…", "from": "ARMMEX.fbi" } },
  "scene": { "…": "the map every demo starts from" },
  "demos": [ { "id": "snap", "title": "Snap onto the deposit",
               "scene": { "…": "additions to the entry's scene" },
               "inputs": [ { "at": 0, "move": [120, 80] }, { "at": 30, "down": "order" },
                           { "at": 32, "up": "order" }, { "at": 30, "caption": "{order} the {mex}…" } ],
               "end": 240 } ] }
```

- **Time is in simulation ticks**, 30 a second, so a demo plays the same at any frame rate and the
  game's version can be compared tick by tick.
- **Positions are world units** (a cell is 16), the game's own.
- **Keys and buttons**: `shift`, `alt`, `ctrl`, `z`, `x`, `lmb`, `rmb`, and the actions `select`,
  `order` and `cancel`, which the simulation turns into a button by the Interface Type (*Casts,
  tokens and actions in the data*, above). A `z` or `x` down steps again while the key is held, as
  an auto-repeat does.
- **A unit's `cast`** is one of the nine casts, required, and no two units of an entry share one.
  `"vent": true` makes every footprint cell a G cell; an optional `"g": [[dx, dz], …]` lists the
  G cells when they are fewer (no entry needs it). A unit with G cells and a footprint past 16
  cells a side is refused.
- **Text** -- the title, subtitle, paragraphs, demo titles and captions -- names units by cast
  tokens and buttons by `{order}`, `{select}` and `{cancel}` (above); any other `{…}` is refused.
- **`{ "build": "ARMMEX" }`** arms a building (the manual placement mode). With the sidebar on it
  is the cursor's trip to the button (*The rules*, above), and both simulations refuse to play a
  demo that breaks the trip's data rule (`sim.js` `checkTrips`, `tagpu_wikidata_trips`): the build names a type; nothing but a caption stands in its trip,
  the ticks (t, t + 90), nor after it in tick t; the demo lasts the whole trip; and the left button
  is up when it starts. Without the sidebar it arms at once, and `{ "build": null }` leaves the
  mode.
- **A wait, `{ "at": t, "until": "idle", "max": m }`**, holds the demo's clock at `t` until no unit
  of the player's holds a build or a reclaim order, at most `m` ticks (an integer, 1 to 3600);
  past that the demo stops, stalled, and the check names it. **`"until": "arrived"`** holds it
  instead until no unit of the player's holds a move order: each reached its place or stopped
  close by it. A demo waits on the state its next input needs rather than timing that input to a
  walk, whose length the units and buildings in the way decide ([below](#how-units-move)).
  **`"until": "built"`**, the demo's k-th such wait, holds once k buildings of the player's have
  been finished in the demo -- a count, so a building finished before the wait counts too, and a
  fast mod's builder never leaves a wait with nothing left to finish. The states are
  `WIKI_UNTIL_IDLE` 1, `WIKI_UNTIL_ARRIVED` 2 and `WIKI_UNTIL_BUILT` 3, in `sim.js`'s `UNTILS`
  order. A wait
  carries no other key, and no other input shares its tick; both simulations refuse a demo that
  breaks either. Every later input, trip and the demo's `end` count the clock's ticks, which stand
  still during a wait (*The units' scripts*, below).
- **A demo's `sidebar`**, a boolean, is optional: absent, the sidebar is shown when the scene gives
  the player a mobile builder. No demo sets it today.
- **A unit's `canstop`** (a boolean, or a number whose integer part's bit 0 counts) is the FBI key; absent, it is 0, as the
  engine's FBI reader defaults it. The entries give it to ARMCK, ARMACK, ARMPW and ARMLLT.
- **A unit's `activatewhenbuilt`** (a boolean, or a number whose integer part's bit 0 counts; a
  casting's must be an integer) is the FBI key, def `+0x241` bit 18; absent, it is 0. A structure
  finished, or made whole in a scene, with it set is switched on: its script's `Activate` runs
  (`0x41B9A6`, `0x48612F`). The entries give it to ARMMEX, ARMMOHO, ARMGEO and ARMSOLAR, and the
  castings carry it for every cast (`gfx.py`, `tagpu_wikiasset.c`, `casting-fixture.json`).
- **The scene**: the map's size in cells, its `surfaceMetal`, its metal as rectangles of a value,
  its vents, its fog rectangles (`grey` or `black`), the units (type, owner, position, finished or
  not, and optionally move orders and a `facing`), and which are selected. `"facing": [x, z]` is
  any vector with a length, normalised on load; without it a unit faces `(0, 1)`, and a building
  faces `(0, 1)` whatever it gives.
- **The ground's grid**: the map's size, every fog rectangle and every broad metal rectangle (one
  that touches both opposite edges of the map on an axis) are whole tiles, even numbers of cells,
  because the stage's ground is drawn in the map's 32-unit tiles and its fog in the engine's 32-unit
  blocks. A demo adds no metal, vent or fog: the stage's ground is the entry's. `stagemap.py check`
  refuses both.

### `art/wiki/stage/<entry>.json`

The stage's ground for one entry's scene, by reference to the player's own files; written by
`tools/wiki/stagemap.py build`, never by hand.

```json
{ "format": 1, "entry": "quickbuild",
  "cells": [40, 30],
  "sources": [ { "map": "maps\\crossing two rivers.tnt", "tiles": 4792 },
               { "map": "maps\\metal heck.tnt", "tiles": 583 } ],
  "tileset": [ [0, 13, "04e02f02"], [1, 170, "8cb0058f"] ],
  "tilemap": [ 0, 0, 0, 1, "…" ],
  "features": [ { "name": "lushmetal3", "cell": [14, 16] },
                { "name": "lushvent01", "cell": [28, 9] } ] }
```

- **`cells`** is the scene's map; **`tilemap`** is `cells / 2` tiles a side, row-major, each an
  index into `tileset`.
- **A `tileset` row** is `[source, tile index in that TNT, FNV-1a 32 of the tile's 1024 bytes]`,
  and a **source**'s `tiles` is its TNT's tile count. The map path is the engine's (`\` between
  components, compared without case). The hash and the count are identity checks: a map of the same
  name with other tiles is refused, never drawn as a patchwork.
- **A feature** is a TDF section name at its anchor cell, the footprint's top-left cell, as the TNT
  and LoadFeature `0x4224B0` place it; its definition comes from the first `features\*.tdf` that
  holds the section.
- **Nothing of the game is in it**: indices, names, counts and hashes only. A file opened, a tile
  read and a section found all follow the engine's precedence: loose first, then the first archive
  in mount order, the first pattern read out of the player's executable (`tools/wiki/tavfs.py`;
  [the 3D stage's plan](ingame-wiki-3d-stage-plan.html), section 5).
- **`stagemap.py build`** derives it from the entry's scene and the recipe the script holds: the
  ground tile everywhere, the metal-ground tile over each broad metal rectangle, a deposit feature
  centred on each deposit (the bounding box of the other metal rectangles that touch), and a vent
  feature on each vent's cell. Each recipe tile is its map's most used tileable tile under flat,
  featureless ground, the first `stagemap.py tiles <map>` lists; that command also draws a contact
  sheet into the gitignored `tools/wiki/mockup/gfx/`.
- **`stagemap.py check`** exits 0 when every entry's description resolves on the local install —
  every map with its tile count, every tile with its hash, every feature section — fits its scene
  (each deposit carries a deposit feature over its metal, each vent a vent feature on its cell,
  every feature inside the map, the grid of the bullet above), and is what `build` writes now; 1
  naming each failure, 2 when it finds no install or executable.
- **A miss in the game** — no such map, another tile count, another hash, no such section — makes
  the whole ground fall back to the whitebox ([the plan](ingame-wiki-3d-stage-plan.html),
  section 3), never part of it.

### `art/wiki/side/<name>.json`: a sidebar layout

The layout the simulation and the stage run on: a fixture here, or the running mod's, which the
DLL's loader and `gfx.py --side-only` write in the same schema. `sim.js` `checkLayout` states it
and checks it, and `tagpu_wikilayout_check` (`tagpu_wikidata.c`) holds the same bounds, so a
layout breaking any of them is refused whole and no index read out of it addresses past the
`WIKILAYOUT` tables.

```json
{ "format": 1, "note": "…", "width": 128, "height": 480,
  "sides": [ { "name": "ARM", "prefix": "ARM", "main2": "ARMMAIN2", "gen": "ARMGEN",
               "types": { "ARMCK": { "pages": ["ARMCK1", "ARMCK2", "ARMCK3", "ARMCK4"], "orders": null } } } ],
  "screens": { "ARMCK1": { "y": 128, "w": 128, "h": 352, "gadgets": [
                 { "name": "ARMMEX", "role": "build", "unit": "ARMMEX", "rect": [4, 32, 60, 60] },
                 { "name": "ARMPREV", "role": "prev", "rect": [4, 228, 58, 20] } ] } } }
```

- **`width`** is 128, the engine's panel. **`height`** is the column's rows, the larger of 480
  (PANELSIDE) and the lowest screen's end.
- **A screen** has its row `y` in the column and its `w` (1..128) and `h`, ending by row 1024,
  the composed column's texture.
- **A gadget** has a non-empty `name`, a `role` and a `rect` inside its screen, in the screen's
  pixels, and optionally `grey` and `hidden`, booleans. The roles are `orders`, `buildtab`,
  `build`, `prev`, `next`, `stop` and `order`, as `0x41AA00` tells them apart by name. A build
  button's `unit` is its type, or `null` (greyed) when its name is no type.
- **`sides`** lists 1 to 8 sides, each with a name, its prefix, its MAIN2 and GEN screens and its
  `types`. The simulation plays the side its context names; nothing in the layout says which side
  is the player's.
- **A side's `types`** holds only the types with a page that its casting names, each with at most
  7 numbered page screens and its orders page or `null`, and at least one of the two; its pages
  are read with that side's prefix, so one type may have other pages in another side. At most 64
  over all the sides. A type's name is upper case, as the build buttons spell it (the engine
  compares names without case, `0x488B10`).
- **A mod's pack** (`gfx/side/<mod>/side.json`, gitignored) carries, beside the layout, `casting`,
  index-aligned with `sides`: per side its `commander` and `casts`, each cast's type, title,
  footprint, figures, `canstop`, `vent` and `g`, and `builds` naming casts, or `null` when unbound,
  and the rule's `report` (why each pick); and `art.panelside`, a list a side.
- **The bounds**: at most 48 screens and 64 gadgets a screen. A name, with its terminator, fits 32
  bytes in UTF-8. A layout that names a screen that is not in `screens` is refused. `note` and any
  key not named here are ignored.

## The simulation's contract

`sim.js` is the mockup's and `tagpu/ddraw/src/tagpu_wikisim.c` the DLL's: a port of `sim.js`
function for function, under its names in C's spelling (`siteOk` is `site_ok`). They agree tick
for tick on the same inputs, and `node tools/wiki/simparity.mjs` checks it: it plays every demo of
every entry through both and compares every field of the view, every unit's orders and the state
hash on every tick, then replays `simcheck.mjs`'s rule cases (the rules no demo shows) through the
C sim from a recording of their calls on `sim.js`, and compares every answer they read, the state
hash after every view included. A rule case must finish with one sim before it makes the next: the
recording replays one at a time. For each demo and case it names the first differing tick or call
and field. Last, it loads data the loader must survive or refuse (`wikitest.c`'s `malformed`
cases). Every demo plays with the fixture's sidebar, the 768-row fixture's and none, each in eight
contexts: the entry's own types and the `identity`, `shifted` and `unbound` castings of
`tools/wiki/casting-fixture.json`, each at Interface Type 1 and 0, and each of those with the
units' scripts and without when a gfx pack under `tools/wiki/mockup/gfx/` holds them (`wikitest`
reads the same script files). MEASURED 2026-10-28: 1008 demo plays and 55 rule cases equal, all
180 refusal cases alike, 24 castings and layouts read back alike and all 21 malformed-data cases
as stated, natively (gcc, with AddressSanitizer and UndefinedBehaviorSanitizer) and with `--wine`
(the DLL's compiler and flags, run under Wine).

- **The context.** `WikiSim.play(entry, demo, layout, ctx)` and `create`, and
  `tagpu_wikisim_init(…, const WIKISIMCTX* ctx, …)`, take `{side, casting, iface, cob}`: the
  layout's side the player plays, that side's casting or none (the entry's own types), the
  Interface Type, and where the units' scripts come from or none (`ctx.cob`, a `WIKICOBSRC` in C,
  read during init only; *The units' scripts*, below). An absent context (NULL in C) is side 0, no
  casting, type 1, the interface the demos are authored in, and no scripts. LIFETIME: the casting and the layout must outlive the simulation; in the DLL both
  lie in the published WIKISIDE block, immutable and never freed. The bound entry, the resolved
  script and the type-0 rows are [above](#the-per-mod-tutorials).
- **The hash's first step** is the tick, `done`, the side (−1 without a sidebar) and the Interface
  Type; `locked` is hashed with the sidebar off too. The captions enter it raw. Its last is the
  demo's clock, the wait, `stalled`, then 1 and the scripts' host's own hash, or 0 alone without a
  host (`tagpu_wikisim.h`, item 8), and then the nano spray: the particle count and each
  particle's x, y, z, its step, its end and its colour, in the order made, then `part_cut` and the
  waiting second bursts, each its two boxes, its due tick and its key (item 9).

- **Bit for bit, by construction.** Both visit units in creation order, use no clock and no
  randomness, and compute on doubles with + − × ÷ and sqrt only, each rounded once. The DLL
  compiles `tagpu_wikisim.c`, and `tagpu_wikidata.c`, which normalises the scenes' facings, for
  SSE2 (`-msse2 -mfpmath=sse`, the stack realigned, as the game's threads align it to 4 bytes), so
  the wiki's simulation needs an SSE2 processor: built for x87, whose intermediates keep 80 bits,
  1196 of the 9298 frames differ from `sim.js` in a last bit (MEASURED 2026-10-25), and 1804 of
  3720 facings normalised as the loader does (MEASURED 2026-10-26; none built for SSE2).
- **The heading is a vector, not an angle.** Each unit faces along a unit vector `(hx, hz)`, which
  the view carries and the state hash takes after its position. An angle would need `atan2`,
  which no standard rounds correctly, so its last bit differs between C libraries and JavaScript
  engines; the vector needs only ÷ and sqrt. Nothing draws it yet: the stage's whitebox
  (`stage.js`, `tagpu_wikistage.c`) ignores it, and `tagpu_wikistage_copy`'s by-value view does
  not carry it until the 3D stage's units do. Units turn at once, with no turn rate (the owner's
  answer of 2026-10-26, [the 3D stage's plan](ingame-wiki-3d-stage-plan.html) question 9):
  - a walk step faces along the step, `dx / d`, `dz / d`, the numbers the step itself moves by,
    and a turned step along its turn, that vector rotated by `Math.SQRT1_2` (the correctly
    rounded double the C literal names too); the tick a unit arrives, with no step, keeps the last
    one's facing;
  - a builder in range of what it builds or reclaims, its own order or a guarded unit's, faces
    that building's centre every tick it works (unchanged if it stands on the centre);
  - a building faces `(0, 1)`, toward the viewer, a scene's `facing` included: TA's default
    heading `0x8000` is the centre of the spawn's `0x8000 − BuildAngle/2 + rand(BuildAngle)`
    (`0x485A40`, cited in `tagpu_native.c` `ghost_one`), and the simulation has no randomness;
  - a mobile scene unit takes its `facing`, normalised, or `(0, 1)`.
- **Engine-free, and owned by its caller.** It reads no engine memory and names no engine address,
  so `thread-split.allow` has no line for it. Every piece of its state lives in the `WIKISIM` its
  caller owns and on the heap it owns, so a later landing runs it on whichever thread owns the
  window. The shapes and the take-off rule (`place_take_offs_one`) are `tagpu_placegeo.c`'s own,
  called as the game calls them; where `sim.js` mirrors anything else of `tagpu_resource.c` or
  `tagpu_place.c`, the C ports `sim.js`'s version and names the function it mirrors above each.
  The take-off rule compares 16.16 positions, as the game's order nodes hold them; the C converts
  its doubles exactly, and clamps an order position to ±8192 world units, which changes no answer
  because a site the rule compares is never refused and so lies inside the map (a
  `_Static_assert` holds the bound).
- **Bounded by construction.** Every append to a unit's orders goes through one function that
  refuses past 1024 (`PLACE_LIST_CAP`, `tagpu_placegeo.h`, the game's own cap on a list): a
  gesture's commit, a Shift move, a Shift Quick Build, a Shift assist, a scene's own orders. A
  click's orders go in whole or not at all, so an upgrade's reclaim never goes in without its
  build. The loader refuses a map wider or taller than 128 cells (`WIKI_MAP_MAX`), which bounds
  every per-cell array and the map a packet will carry by value. `sim.js` caps the same appends
  in the same way, and `simcheck.mjs` checks it as a rule case. The planner's scratch is sized at
  init from the map: W × H bytes for the held cells, W × H nodes of state, cost and parent, and a
  heap of 8 × W × H + 4 entries -- a node is pushed once from a seed and at most once from each of
  its eight neighbours, since a neighbour relaxes it only when popped, once -- about 2.3 MB at
  128 × 128 and 0.2 MB on the 40 × 30 stage maps.
- **Its calls** (`tagpu_wikisim.h`): `tagpu_wikisim_init` from an entry and a demo, scripted (the
  demo plays, `sim.js`'s `play`) or not (Try it yourself on the demo's scene, `create`);
  `tagpu_wikisim_input`; `tagpu_wikisim_step`, one tick; `tagpu_wikisim_view`, `sim.js`'s
  `view()` field for field; `tagpu_wikisim_orders`; `tagpu_wikisim_hash`, a 64-bit FNV-1a over
  what the view and the orders show, in the order and encoding the header states, so that the
  in-game check can compare one number per demo with `simparity.mjs --hash`. It reads the
  `WIKISIM` and changes nothing; the view's derived fields (queued sites, order lines, the cursor's
  shape, order numbers, footprints, the pace) follow from the hashed ones and are left out.
- **The data** (`tagpu_wikidata.h`): the tree in preorder and each entry as C structs, read with
  the DLL's JSON reader from `WIKIPAK`. Where `sim.js` would throw or read `undefined`, the loader
  refuses the file instead: a unit type the entry does not define, a key it does not name, a
  rectangle or a map size that is not an integer, a missing footprint, a BuildTime that is not
  positive, a `facing` that is not two numbers or whose length is not a positive finite double
  (zero, or a square that underflows or overflows: `sim.js`'s `facing()` throws on the same).
  Every other absent figure reads as 0, which every rule treats as it treats `undefined`. It also
  refuses a map larger than 128 × 128 cells, which `sim.js` would play.
- **Where the C cannot throw.** A scene that selects a unit it does not have makes
  `tagpu_wikisim_init` refuse it, where `sim.js` throws; an allocation that fails leaves out what
  it would have added and sets `oom`. Cells beyond ±2²⁰ (positions far off any map) are held
  there, and a gesture's cells to the int16 `place_shape` takes, where `sim.js` keeps the number;
  every test refuses such a cell in both.
- **Not ported:** `sim.log`, the construction events `simcheck.mjs`'s checks read. The view and
  the orders carry the state they follow from, and the parity check compares those.

**What it implements, from the feature's own rules** (`resource-snapping.md`), following
`tagpu_resource.c` function for function (`site`, `snapped_site`, `distinct`, `same_plan`,
`find_cancel`, `queued`, `quick_choice`, `plans_block`, `site_known`, `fits`). "The order click" is
the right click at Interface Type 1 and the left click at type 0 (*The two interface types in the
simulations*, above):

- **Manual snapping** (a build-menu extractor or geothermal plant): candidates within a square
  radius of 3 cells; a mex scores the footprint cells whose metal is strictly above
  `surfaceMetal`, only on explored ground; the highest score wins and a tie goes to the candidate
  nearest the raw cursor (`candidate + 0.5`); a second search of radius max(footprint) around the
  winner must pick the same place, or the placement stays where the cursor is. A geothermal plant
  snaps to the nearest legal place on a vent. Occupied deposits stay targets (the ghost is red).
  Alt bypasses snapping.
- **Quick Build** (no build-menu mode, at least one selected builder able to build a resource
  building): hovering empty ground over a distinct deposit or a vent shows the ghost of the
  strongest option the selection can build that fits; the order click orders it, replacing the
  participating builders' orders, and Shift + the order click appends. The mode stays on.
  Selected builders that cannot build it guard those that can. Units that cannot build keep their
  orders.
- **A queued site**: hovering it shows its ghost; Shift + the order click removes the selected builders'
  matching plans; a plain repeat click changes nothing.
- **Upgrade**: hovering a finished extractor the player owns, with a selection that can build a
  strictly stronger one that fits, shows that one; the click orders a reclaim, then the build.
- **Metal maps**: uniform metal ground is not a deposit (the order click there is a move); a
  distinct richer patch is.
- **Fog**: unexplored (black) cells do not count; grey-fogged ones do.
- **Selection**: left-click a unit, left-drag a box; a left release is a click only inside the
  click window (under 25 ticks and under 32 world units on each axis), else a box; at type 1 a
  left click on empty ground deselects, at type 0 a right press does.
- **Orders**: the order click with nothing to build is a move, or the assist over an unfinished
  building of the player's.

**The placement gesture**, following `tagpu_place.c` function for function (`evaluate`,
`toggles`, `take_offs`, `classify`, `ring_target`, `remove_taken` and `append_sites` as `commit`,
`place_button` as the press and the release; `take_offs` and `remove_taken`, which replaces
`remove_toggled`, as they stand on the wiki branch, which carries the take-off fix
`34b1b586` that `main` does not have yet). In `sim.js`, `tagpu_placegeo.c`'s `place_shape` and
`place_ring` are ported line for line, integer arithmetic included (checked against the compiled C
on 20,000 random cases, 2026-10-25: identical), and so is its take-off rule; the C sim calls all
three:

- **The cursor site** is the build cursor's (`0x4197D0`): a footprint of f cells centred on world x
  starts at cell floor((x − 8f + 8) / 16), half up, after manual snapping for a resource building.
- **A plain click** in the build-menu mode places one at the cursor site and disarms. **A left
  press with Shift** starts a gesture at the cursor site instead of placing.
- **The preview** (`evaluate`) reads Shift, Ctrl and Alt live. Without Shift nothing previews;
  Shift + Ctrl over a ring target is a ring, before any drag shape, mid-drag included; otherwise,
  while a gesture lives, the shape from the press site to the cursor site: a line, Ctrl held to
  its axis, Alt a filled rectangle in serpentine order, Alt + Ctrl a hollow one. At most 256 sites
  (`PLACE_MAX_SITES`), cut to 4096 orders over the selected builders. With no shape the
  engine's single square stands.
- **A ring target** is the unit under the cursor, any unit, else a selected builder's build order
  whose footprint holds the cursor's cell, first in selection order then queue order. A ring is
  `place_ring`'s: no spacing; its sites that would take an order off are left out.
- **The take-off rule** (`take_offs`, one call of `place_take_offs_one` per selected builder)
  settles what each site does, and both the preview's colours and the release read it. Per selected builder, each queued build order is taken off by at most
  one site, judged against the queue as it stood before the gesture: first every site not red
  takes the build order at its own position, then each site that took none takes the first build
  order within 16 world units of its centre on both axes (`0x43AFC0`'s predicate, inclusive; any
  building) that no site has taken — both passes in the shape's order, orders in queue order. A
  ring takes nothing off: its sites with a build order of some selected builder within 16 world
  units are left out.
- **A site's colour**: red where the site test refuses it; yellow where it takes an order off
  from some selected builder; green otherwise.
- **The release with Shift held** commits. A one-site gesture ending at the cursor site is the
  stock Shift-click itself: the first build order within 16 world units is taken off instead of
  adding one, otherwise the site is appended. Any other gesture is classified afresh by the same
  rule, then per selected builder: the orders the rule gave a site are taken off, and each of
  those sites adds nothing for that builder; the rest that are not red are appended in the
  shape's order while the queue is shorter than 1024 orders (every other append stops there
  too). Every selected builder gets them,
  its build list not consulted, as `0x419670` orders.
- **Shift released before the button** drops the gesture, orders nothing and keeps the building
  armed. A placement or a commit that changed a queue sets the Shift-placement bit (`main+0x2CC6`
  bit 5), and **Shift released afterwards disarms**, as after a stock Shift-click (`0x4992D8`).
  **A right press** disarms and ends the gesture.
- **Spacing** (`tagpu_place.c` `tagpu_place_key`, `spacing_step`, `spacing_of`): with a building
  armed and Alt down, Z adds one empty cell between neighbours and X takes one away, clamped to
  0..16, kept per building type by its unit name (the game saves it in `impure.cfg` as
  `placespacing=`; the sim keeps it for its life); every Z or X down steps, an auto-repeat
  included, and the next evaluation shows it, mid-drag too. The pitch is footprint + spacing cells;
  the ring takes none. The page's "Spacing: N" readout is the mockup's: the game shows the spacing
  only in the sites it lays.

**What it simplifies, on purpose and in both places:**

- units walk by the wiki's own movement rule ([below](#how-units-move)), not the engine's
  pathfinder: bodies are footprint boxes, a path is A* over the map's cells, nobody is pushed, and
  units turn at once, with no turn rate;
- construction fills at the builders' total worker time over the building's build time, with no
  resource cost and no economy; a reclaim takes as long as building it would;
- construction and reclaim run at the entry's `pace` (`"pace": {"build": N}`), N times faster
  than the game, which the stage says: an ARMMOHO takes one ARMACK about 223 s
  (BuildTime 35,750 / WorkerTime 160), too long to watch. The units' scripts run at game speed,
  so their animations, and a build's wait for the builder's stance, take their game time;
- no terrain height, slopes, enemies or other orders;
- the engine's own site test (`0x47D2E0`) as: inside the map's interior cells, clear of
  structures, a geothermal plant with a vent under it; a mobile unit never blocks a site, where
  the engine's test refuses one standing on it;
- the gesture: Shift released after a placement disarms at once, where the engine disarms at the
  first mouse record that finds Shift up; order nodes flagged `0x40000`, which
  the engine's toggle matches but does not remove, do not exist in the sim; the one-site Shift-click
  orders only builders whose build list has the building, where the engine orders every selected
  builder (the same under stock input, which arms a building for one unit only).

**The demos are generated**: `node tools/wiki/gen-quickbuild.mjs` and
`node tools/wiki/gen-buildshapes.mjs` write `art/wiki/entries/<id>.json` (their cursor paths are
hundreds of small moves), and `node tools/wiki/simcheck.mjs` plays every demo headless and checks
its claim on the simulation's state, plus the rules no demo shows; `simparity.mjs` then holds the
DLL's simulation to `sim.js` on the same demos and rules.

**What it draws** (its view): the cells, metal, vents and fog; the units with their selection,
progress and facing (`hx`, `hz`, not drawn yet); the ghost and its colour; the queued sites; the order lines; the cursor, its shape and the tick the shape began;
the keys held; the caption; a gesture's sites (`sites`: centre, footprint, `state` `place`,
`refused` or `remove`, and `order`, the site's place among the orders the release adds), its
`shape`, and the armed building's `spacing`; the demo's `clock`, `waiting` (the state a wait holds
it for, or none), `stalled`, and `cob`, the scripts' host's hash as two 32-bit halves; and the
nano spray's live particles, `parts` (each `[x, y, z, step x, y, z, end, colour]`, 16.16 world
units, x, altitude, z; `npart` and `part` in C) and `partsCut` (`part_cut`: a bound dropped one).

### How units move

The owner's request of 2026-10-28: units do not walk over each other or through builders. The
owner chose a rule of the wiki's own over a port of the engine's mover and pathfinder (`0x43DD20`,
`0x40E9E0`, unmapped), and that a unit whose place is taken stops close by rather than pushing.
`sim.js` *MOVEMENT* and `tagpu_wikisim.c` *MOVEMENT* are the rule, function for function.

- **Bodies.** A mobile unit's body is its footprint's box about its centre (`fx × 8` either side,
  so ARMCK is 32 × 32); a structure's is its footprint, finished or a nanoframe; and a builder
  within build distance of a site it has not started yet holds the site as a **claim**, a body of
  no unit. Bodies overlap only with area in common: touching is not overlapping.
- **The step rule**, which every position change obeys: a step may not make its unit overlap a
  body it does not overlap already, nor take its body off the map. So no step ever puts two bodies
  together, and a unit that something was put over (a scene, a structure started on it) can still
  walk out. `simcheck.mjs` holds every demo, in every context, to no overlap on any tick.
- **Straight while clear.** A unit walks straight at its target, exactly as it did before this
  rule (the same arithmetic, so a walk with nothing in its way is unchanged), while the straight
  line is clear of every **still** body: a structure, a claim, or a unit that did not step on the
  last tick. The test is the segment against each body grown by the walker's half sides, which is
  exactly where the sliding box would overlap it. A walking unit does not block the line: a step
  one refuses tries 45° left, 45° right, 90° left, 90° right, and failing all five the unit waits
  the tick.
- **Otherwise the planner**: A* over the map's cells, a node standing the unit's body on whole
  cells, free when no body covers it (walking units included, at their present place: a guard
  that steps one tick in two would otherwise flicker on and off every path past it). A side costs
  16, a corner `16√2` and needs both sides beside it free. It starts from the nodes whose cells the
  unit's body touches already (one on a node, up to four between nodes), each at its distance,
  and ends at the first node popped that meets the goal; the heap orders by f, then push order,
  a total order both heaps pop alike. The goal is the target's range (a point: within one cell;
  a site: build distance; a guarded unit: 48), or touching the target's own body.
- **Legs.** A path is walked one node at a time. The unit keeps its leg until it stands on the
  node (the last step lands on it exactly), a body covers the node, the straight step is refused
  or a turn was taken, or its order or the kind of its target changes (a guard whose leader moves
  to a new job keeps its leg to the next node); only then does it plan again. Replanning
  every tick dithered: from between two nodes the nearer can lie behind (seen on *The best
  extractor that fits*, a constructor rocking between two nodes for 1500 ticks). A leg crosses no
  structure cell: it starts on a node or within a cell of it on each axis, so the box sweeps only
  cells the unit's body or the node's covers already.
- **Stuck, and close by.** When no node is nearer the target than where the unit stands, it is
  **stuck**: a move ends there (two units sent to one place: the first takes it, the second stops
  beside it), and a build, reclaim or guard waits, planning again each tick, so a helper at a
  crowded site stands close by until a place frees.
- **Arrival at a target body.** A unit is in range when within its range of the target, or
  touching the target's own body, or when the target itself refuses its straight step. A site is
  a body to the unit heading for it, started or not, so that step stops at its edge: a builder
  whose BuildDistance is shorter than its own half width still reaches its site rather than
  walking onto it. A **claim** follows the same arrival: the builder's centre within its
  BuildDistance of the site, or its body within one step (MaxVelocity) of it, which covers
  touching and a refused step. A builder that arrived without claiming would never send what
  stands on the site away, and its build would wait for good. For every builder whose
  BuildDistance is at least its half width plus a step, the second test adds nothing.
- **A site is cleared before it starts.** A builder in range does not start a building while any
  unit's body overlaps the site. A unit on a structure or a claim (the claimant included, when it
  was ordered to build where it stands) spends its turn stepping straight away from the body's
  centre -- the first it overlaps of the units' bodies in creation order, then the claims in their
  claimants' order -- along `(0, 1)` from the centre itself.
- **A scene's mobile unit starts clear.** A casting's bigger units can overlap an authored scene
  (TA Zero's GOK hovers are 3 × 3 and 4 × 4 where stock's kbots are 2 × 2, 48 units apart). Such
  a unit stands at the nearest centre a whole number of cells from its authored one, at most 8
  either way, nearest by distance, then z, then x, whose body is on the map and clear of the
  scene's structures and of the mobile units before it in the scene; with none it stands where
  authored and walks out. It is settled before any unit enters, since entering starts the unit's
  script, which may read where it stands. A stock scene never moves.
- **What the rule does not do.** Nobody pushes: an idle unit in a path is walked round, never moved
  aside (but off a claim). A unit boxed in on a claimed site, every step refused, holds that build
  until something moves. A crowd going opposite ways through a one-unit gap can take many ticks of
  turns. Paths are eight-direction: diagonal runs look stepped, since nothing smooths them. None
  of these is seen in a demo.

### The units' scripts

Each unit runs its own COB script, on our own VM, in both simulations
([the COB plan](ingame-wiki-cob-plan.html), *The wiki host*, which has the engine's calls and the
addresses behind them). The host is one engine-free C file, `tagpu_wikicob.c`, on `tagpu_cobvm.c`
under Impure's rules: `tagpu_wikisim.c` calls it in-process and `sim.js` through the mockup's
server, so the two simulations implement only *when* they call it, and the host's state hash, which
both fold into theirs, holds them to the same calls.

- **The tick**: the inputs, then every unit's script slice (`DoScriptsNow(1)`), then the units'
  work, then the engine's calls of each unit's turn, unit by unit in creation order -- the
  engine's order of a unit's turn, slice before orders before movement.
- **The scripts gate the work.** A builder in range credits nothing on a work until its script
  has set INBUILDSTANCE since the work began; after that the stance is not read again until the
  work changes, as the engine reads it only at the work's start (`0x438700`). A structure finished, or made whole, whose FBI has `activatewhenbuilt` is switched
  on. Without a host, or for a type whose script the host refused (quarantined), every gate reads
  open and the unit plays at the figures' pace alone.
- **The calls**: `Create` at a unit's appearance, and an extractor's `SetSpeed` with the metal
  under it; `StartBuilding` and `StopBuilding` when a builder's work target changes, and
  `StopBuilding` at once when the player takes its order away; `StartMoving` with `MoveRate1` and
  `StopMoving` when a mobile unit starts and stops walking; `Activate` for a structure switched
  on, and `Activate` or `Deactivate` when a script changes its own ACTIVATION.
- **Where the scripts come from**: in the DLL, the 3D asset load's (`WIKI3DUNIT` `script`, with
  the posed model's order and visible flags); in the mockup, the gfx pack the stage draws
  (`gfx.py` writes `units/<type>.cob`). **No demo, Try it or check starts before the 3D load has
  answered** (`tagpu_wiki3d_answer`, `tagpu_wiki.c` `scripts_now`), so a demo's units have their
  scripts from tick 0 or never; a load that failed plays them all without.
- **The demo's clock.** A wait (*The data format*) stops the clock the demo's inputs are written in
  while the simulation's tick runs on, so a demo can end on its work done whatever the side's
  builder: every Quick Build demo ends on `until idle` and 60 more ticks. The scripts run at game
  speed while builds run at the entry's `pace`, so a build stance or a nanolathe folding between
  two queued pieces takes its game time: stock's ARMCK takes about 268 ticks a queued piece, and
  the Build Lines demos whose claims read finished pieces wait for them (`until: "built"`).
- **The poses.** What a unit's script poses is drawn, not simulated: the poses are outside the
  view and the state hash. In the DLL the page fill copies each unit's posed model (each
  primitive's MOVE, TURN and flags) after the view's copy into the stage record's pose pool,
  `WIKISTAGEPIECE pose[WIKISTAGE_POSE]` (4096 primitives over all the units, `pose_at` and
  `pose_n` a unit; `tagpu_wikistage_pose`); a unit with more than `WIKISTAGE_PIECES` (256) or past
  the pool draws at rest and sets `WIKISTAGE_TRUNC_POSE`. In the mockup `serve.py`'s `models` op
  answers every unit's model block (`{"models": {unit: base64}}`, `tagpu_wikicob_model`'s
  PrimitiveStructs in the posed order), `cob.js` `model(unit)` asks once after each call, and
  `sim.js` `poses()` gives them aligned with `view().units`, null for a unit no script poses;
  `gfx.js` `frame(entry, v, tick, dir, poses)` draws them ([the COB plan](ingame-wiki-cob-plan.html),
  *Drawing*).
- **The spray** is simulated, unlike the poses: a builder whose work is credited this tick asks
  its script's `QueryNanoPiece` (run-now, once a spray: it can write the script's statics) and
  sprays from that piece's origin into the structure's box, every tick for a build and every
  second tick for a reclaim, in reverse; two bursts of five particles an emission, each at about
  4 world units a tick (`len >> 18` steps), its colour cycling `0xA1..0xA7`. The particles are in the view and the
  state hash (item 9), bounded at 1024 live and 256 waiting bursts, drawn from the wiki's own
  stateless generator and never from the host's `rand`. The host answers the query and the
  piece's origin (`tagpu_wikicob_query`, `tagpu_wikicob_origin`, from the model's shape the type
  now carries: offsets, parents and the model's height); in the mockup `serve.py`'s `query` and
  `origin` ops, `cob.js`'s `query`, `origin` and `top` (the heights `new` answers), and
  `cobhost.mjs` carrying the shape from the pack, which `gfx.py` writes (`model_shape`) -- a pack
  written before it gives every piece the unit's own point. In the DLL the page fill copies the
  particles into the stage record, `WIKISTAGEPART part[WIKISTAGE_PARTS]` (x, y, z, colour; the
  sim's 1024), and sets `WIKISTAGE_TRUNC_SPRAY` (`0x400`) when the simulation dropped one. Without
  the scripts nothing sprays ([the COB plan](ingame-wiki-cob-plan.html), *The spray*, which has
  the engine's addresses and the gaps).
- **The page** asks `tools/wiki/serve.py` for its host; served without it, or with no gfx pack
  built, it plays without the scripts and says why in the console, as it says a quarantined type's
  note. Run the mockup with `serve.py`.
- **What it costs**: the host's slice over a demo's units is about 70 µs; with the scripts the page
  cost the game thread at most 0.281 ms in a frame over 1253 frames, and *Queue several with
  Shift* plays 1054 ticks where it played 570 (MEASURED 2026-10-28).

## Gaps

- **No wiki on GDI** (the owner's decision, 2026-10-25): the button and the window are drawn by the
  Vulkan HUD pass only, as the rewind button is, so on GDI the wiki cannot open.
- **The demo restarts at every open.** The plan frees the simulation at a close and makes it at an
  open, so a reopened window plays the selected demo from its start, or is in Try it yourself on a
  fresh scene, where the mockup, which keeps its simulation, carries on.
- **Try it yourself's Alt is the key events'.** Alt+Z and Alt+X step while the last Alt key event
  the window had was a press, as the browser's `altKey` reads the keyboard. An Alt let go while the
  window had no focus reads down until Alt is next pressed and released.
- **Try it yourself on the real input path is the owner's hand-over.** The test drives the stage
  through the shield's injected device path; the real messages share every rule but the lost
  release (a real move with the button up), which no test runs, and neither the focus loss's
  release nor Try it in a network game or a replay has a test.
- **The wheel's step and a track's page are the DLL's.** The mockup leaves both to the browser: the
  game scrolls 48 units a notch (three of the tree's rows) and a press on a track off its thumb
  pages by seven eighths of the panel, as Chrome does.
- **The stage's budget holds the data's map, not the largest.** Cells and fog are one op a cell,
  as the canvas calls them. By `tagpu_wikistage.h`'s count, a 40 × 30 map's cells, grid and fog
  come to about 3750 ops, the whitebox minimap's to at most 2 × 1200 + 128 more and the sidebar's
  buttons to about 200, against `WIKI_MAXOP` 16384. A square map past about 70 cells a side, fogged everywhere, drops ops and
  edges the stage in red.
- **The controls' glyphs and the tree's marks are kit icons, pending the owner.** No HUD face has
  play, pause, replay, the folds' triangles or the entry's dot, so both the mockup and the game
  draw them as icons rasterised from `glyphs.js`, in the kit icons' pale fill and black edge; the
  play and replay buttons are therefore their least width (`controls.min`, 24 units), where the
  browser's fallback glyphs made them 37.1 and 29.3 units wide at 1920 × 1080. The other fix, the
  glyphs added to a HUD face, is the owner's to choose; the icons are one commit.
- **Chrome sets a face at a size of whole sixty-fourths of a pixel**, by a rule not identified:
  9.0667 px is set at 579/64 and 9.07 at 580/64 (MEASURED 2026-10-25 against stb_truetype's
  widths, which equal Chrome's at those sizes to the sixty-fourth). Where a font size is not a whole
  number of sixty-fourths -- every style at 1024 × 768, and the chips' 6.8 units at every
  resolution -- the mockup's lines are up to 0.2 % narrower than the game's, and a line that just
  fits in the mockup can wrap one word earlier in the game. `wikilaycheck.py` gives the C layout
  the browser's set size and compares the rules on it. The game sets every face at its own size.
- **The drawn icons must be in the kit as cut.** `fit.py --set wiki` rewrites `art/wiki/kit/` from
  `art/wiki-full/kit/` and drops parts that are not there, so `tools/wiki/cursors.sh` is run on
  `art/wiki-full/kit` and the fit carries its icons over.
- **The resource rules are copies.** The C sim calls the game's shapes and take-off rule
  (`tagpu_placegeo.c`), but its resource rules and the rest of the gesture's port `sim.js`'s
  mirrors of `tagpu_resource.c` and `tagpu_place.c` rather than calling them. `simparity.mjs`
  holds the C to `sim.js`, and so `sim.js`'s take-off port to the game's function; nothing holds
  the resource rules to the game's code, so a change there is followed by hand in `sim.js` and
  `tagpu_wikisim.c` (the plan's `rulecheck.py` would notice it; it is not built).
- **No build gate runs the parity checks.** They need node and gcc (and Wine for `--wine`); `make`
  compiles and links the simulation and the stage but runs neither check. The game's own build is
  checked by `wiki_regression.py demos`, whose `tagpu_wiki.check` hashes must equal
  `simparity.mjs --hash`. `stagecheck.mjs` builds
  the stage natively (x86-64, SSE); the DLL compiles it for SSE2, but no check runs the i686 build.
- **The units' scripts are checked on the entries' units only.** `simcheck.mjs --pack` plays the
  six setups' stage units with their own scripts, none quarantined; `tools/wiki/cobcorpus.py` runs
  every compatibility setup's whole script corpus through the host and refuses one type, ZZZ (the
  COB plan, *Gaps open*), and `posecheck.py` holds the drawn poses to
  tacob's composition on the stock pack's types only. GET answers what the flat stage has: piece and unit heights 0, a piece's position the
  unit's, XZ_ATAN without the unit's heading.
- **The nano spray has not been compared with the game's.** Its arithmetic is the engine's,
  disassembled, and the two stages draw it alike (`stagecheck.mjs`), but no capture of the same
  build in a game has been set beside it. It takes the retail palette's colours under every mod,
  tests its sight on the wiki's 16-unit fog cell rather than the engine's 32-unit tile, and turns
  the nano point by the facing in a second rounding step; only the units' scripts make it, and a
  gfx pack written before the model's shape sprays every piece from the unit's own point (the COB
  plan, *Gaps open*). `emit-sfx` is not drawn (the COB plan's L4b).
- **The per-mod tutorials have been played in one setup in game.** Retail as CORE at both
  Interface Types (*The per-mod tutorials*, *The checks*); the six setups at both types, each
  demo's `<entry>/<demo>/<side>/<iface>` hash from `tagpu_wiki.check` against
  `simparity.mjs --hash --pack`, with pictures for the owner, have not been run, and neither has the
  mods check (b1)–(b4) since each side got its own types.
- **The sidebar's load time with the cast rule is not measured in game.** The budget is
  `wikiside: loaded in N ms` under 1500 ms on the reference setup in Twilight and TA Zero, the
  largest walks. Offline, in `assetcheck.py`'s sanitised native build, the six setups load in 244
  to 436 ms.
- **A restart with the window open has not been seen live.** A fill given another side or
  Interface Type restarts the open window's demo, but a player cannot change either while the wiki
  holds input: the typed command and the options screen both need the window closed. What has been
  seen is the next open after a change, playing the new type.
- **A caption of names longer than any setup's can pass its strip.** Its bytes always fit (the
  caption's bound, above), but the strip has a fixed height (27 units, `layout.json` `strips`) and
  shows a caption's first lines:
  `tagpu_wikilay_check` requires every caption to show whole with the entries' own names and with
  every pack's casting (`wikilaycheck.py --check-only`), and builds the worst castings for the
  page's ops only. The game's own check runs before the side loads, so it checks no casting.
- **Twilight's Quick Build has not been measured again since it reads the build menu.** MEASURED
  2026-10-28 in Twilight as ARM at type 1 with an ARMCK selected, while Quick Build still read the
  AI list, CANBUILD: a click on a deposit ordered AAIMEXX, the AI list's extractor, where the
  casting's mex is ARMAMEX, and a click on a vent gave a move. Quick Build now reads each
  constructor's build menu, the one the casting reads (decision 23), so the click is the
  casting's ARMAMEX [INFERRED]; the in-game run over the six setups is still open.
- **The code page is inferred.** The display names are converted as Windows-1252; the French bytes
  measured are the same letters in Latin-1. No game run has shown the wiki's own text in another
  language; `assetcheck.py --language` compares the two loaders' names offline.
- **The replay's side rests on two replays.** `main+0x2A42` is 10 in a replay and record 10's side
  byte was the recorded player's in both measured; that the replay always seats it so is
  [INFERRED].
- **Type 0's untested rows.** An unfinished building of the player's clicked with no builder
  selected is selected in both simulations [INFERRED: its cursor was not measured]; the engine's
  Ctrl + right press (`0x41CC60`, a view mode) is not modelled, and `main+0x2CDF` was never seen at
  1 in game; no scene has a unit of another player, so the engine's attack cursor over one never
  arises.
- **The mixed interface value is shown as left-click.** The typed command `IFace` stores any
  integer and the registry load's clamp is signed, so `main+0x37EFA` can hold a value outside 0
  and 1. The engine's four `== 1` readers then take the type-0 branch and its two `!= 0` readers
  (the right press 0x499162, a release 0x499352) the type-1 branch; the wiki plays the left-click
  interface for any value but 1.
- **The sidebar's paths not run in the game.** The fixture's fallback (a mod's layout refused, or
  the asset loader unable to start) and the "loading…" wait are covered by reading the code and
  by the loader's native fuzz (`assetcheck.py fuzz-side`), and held to the mockup by
  `simparity.mjs` and `stagecheck.mjs`. No compatibility setup triggers them and no test lever
  does, so the game has not been seen taking them.
- **What the mods check does not read from the engine.** The pressed state (b1) gives the ARMCK's
  page (the BUILD tab and ARMMOVEORD drawn pressed) is copied from the captures' comparison, not read
  from the engine's snapshot. (b4) finds the badge by its red disc in the frame; the dump carries
  no badge field.
- **The mods check's coverage.** At 2560 × 1440 a 768-row column is drawn at 0.85 to 0.89 client
  pixels a game pixel, so (b3) samples 71 to 79 % of its texels (retail's 480 rows, at 1.43, all
  of them). Every texel needs a display of about 4K.
- **What the composed column does not show.** It matches the game's captures except for the
  cursor over the column, which no capture compared; the minimap's content, which is the stage
  map's and not the game's; and TA Zero's run-time states.
- **The folder names are followed, the format strings and the archive pattern are not.** A mod
  that patched `%s%d.GUI`, `%sDL` or `_gadget` would not be followed; none of the six setups
  does. The archive pattern Mayhem's and ProTA's loaders rewrite (`0x5028CC`) was not read live.
- **No hotkey** opens the wiki.
- **The spacing readout is the mockup's**: the game has no on-screen spacing value.
- **Build Split is not shown**: it is not built.
- **`gfx=game` keeps the whitebox's markers over the image.** The order lines stay dashed lines
  (the game draws route dots and a waypoint sprite), the queued squares are always shown (the
  game shows them only while Shift is held), and the sites' order numbers are in dark ink over the
  ghosts.
- **The arrow cursor is drawn by no demo.** Every demo keeps a mobile unit selected or a building
  armed, so `cursornormal` is seen in game as the build cursor only; the arrow shows the same
  sequence. The stage's other cursor situations (selecting, reclaiming, attacking) are not modelled.
- **What draws the geothermal steam plume is not established**: a feature animation or the
  engine's particles. `gfx=game` shows the vent's frame 0, without a plume.
- **`gfx=game` departs from the game in known ways**: a textured face of other than four vertices
  takes the lab's corner collapse (the engine textures four-vertex faces); an animated texture shows
  frame 0; the Gamma curve is not applied; the Shading option is taken as on, the game's default.
  Its units follow the game's own rules (`gfx.js`, `tascene-render.js` `emitUnits` with
  `engineShade`): the model turned by the record's yaw after the loader's half turn, a posed unit
  composed by `pose.js`, back faces culled for the bodies and the shadows, a structure and
  every ghost on the engine's lit rows from its vertex normals (`gfx.py` `vertex_normals`), any
  other unit on the neutral row, a textured quad mapped and shaded over its own frame, and the
  nanoframe's wire on the line grid thickened to `ss`. `stage3dcheck.py` compares the whole image
  with the game's own: see the 3D stage's plan, landing 6h, for what it measured, and the COB
  plan's *L3b's gate* for the image with the poses.
- **A stage whose description names a 3D feature (a wreck) is drawn as the whitebox**, in the
  game and in `gfx=game` alike: no stage names one.

## Open issues in the placement code

Porting `tagpu_place.c` and `tagpu_placegeo.c` into `sim.js` turned up three behaviours of the
game's placement worth stating; the first is the specified rule, kept, and the second is fixed. They were found by reading
and porting the code (the port matches `tagpu_placegeo.c` on 20 000 random cases); none has been
reproduced in game yet. The sim mirrors the code as it is, so the Build Lines entry's demos route
round them; when the code changes, `sim.js` and the demos follow it.

1. **A Shift+Ctrl line drag turns into a ring over a unit or a queued build — by design.**
   `evaluate()` asks `ring_target()` first on every evaluation while Shift and Ctrl are held, so a
   line (or, with Alt, a hollow rectangle) whose cursor crosses any unit or queued build previews
   a ring round it, and a release there orders the ring. That is the specified rule: the design
   (`bar-placement-design.md` §2.2, "ring wins over line/rect when Shift + Ctrl and a target is
   under the cursor, as in Recoil"), the approved lab mockup and Recoil's `GuiHandler`, which
   takes the ring target from the live mouse ray, all do the same. Demo 3 routes its cursor round
   the solar collector for that reason. The owner chose to keep it (2026-10-25): the shape is
   decided live, as in BAR, not at the press.
2. **The yellow preview promised more than the release did — fixed in the game.** Dragging back
   over a queued row of 1 × 1 sites took off every one only when the drag started on the row:
   with 10, 11 and 12 queued, a drag from 9 to 12 previewed four yellow sites, and the release
   removed three, queued a fresh building at the last and placed nothing at 9. `tagpu_place.c`
   `take_offs` now settles every site's action once, and the preview's colours and the commit
   both read it: per builder each queued order is taken off by at most one site, first every site
   taking the order at its own position, then each site still without one the first order within
   16 world units that no site has taken, both passes in the shape's order and orders in queue
   order; a site that takes one is yellow, every other site not red is appended, and the ring is
   unchanged. The same drag now previews 9 green and 10 to 12 yellow, and the release takes the
   three off and queues one at 9, from either end. The fix is on the branch
   `worktree-agent-ac37d55e26a5e56f3` and reaches `main` with that branch's landing; `sim.js`
   already follows it, and `simcheck.mjs` checks this case as a rule.
3. **A hollow rectangle is ordered from beside the press round to the press.** Its first site is
   the one next to the pressed corner and its last is the pressed corner itself. Nothing breaks;
   it is undocumented, and stays as it is.
