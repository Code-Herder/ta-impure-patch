# The in-game wiki: Impure's features, shown and tried in the game

## Status

**Designed 2026-10-25; the browser mockup is the first build, and the DLL has the wiki's core.**
The owner wants a wiki inside the game that explains Impure's own features with examples a player
can watch and try. The decisions below came out of a design interview with the owner on
2026-10-25; each is recorded with its reason. The DLL carries the tree and the entries, runs the
simplified simulation in C, equal to the mockup's tick for tick on every demo, lays the window out
in C, equal to the mockup's boxes, lines and baselines, and builds the stage's picture as the
replay HUD's ops, equal to the mockup's canvas call for op. On the Vulkan lane the game draws the
wiki button and opens the window frame over the game, takes every input while it is open and
pauses single player ([the plan](ingame-wiki-plan.html), landing 2); the window's body is empty:
nothing in the DLL draws the page or the stage into it yet.

| What | Where |
|---|---|
| The tree of categories and entries | `art/wiki/tree.json` |
| One entry: its text, its mods, its demos | `art/wiki/entries/<id>.json` |
| The mockup, which reads only those files | `tools/wiki/mockup/` (`index.html`, `wiki.js`, `wiki.css`, `sim.js`, `stage.js`) |
| The window's art kit | `art/wiki/kit/` (`kit.json` and `parts/`), in `art/hud/kit.json`'s format, fitted by `tools/hudart/fit.py --set wiki` from the parts as cut in `art/wiki-full/kit/` |
| The stage's cursor icons | `cursor_*.png` in the kit, written by `tools/wiki/cursors.sh` from `stage.js` `cursorShape` |
| The window's layout numbers and its text roles | `art/wiki/layout.json`, which `wiki.js` sets as custom properties of the frame (`--w-` and the number's path) for `wiki.css`, and which the DLL reads |
| The stage in the game's own graphics | `tools/wiki/gfx.py` writes the gitignored `tools/wiki/mockup/gfx/` from the local install; `gfx.js` draws it under `gfx=game` |
| The stage's ground as references: which tile of which map in each square, which feature on which cell | `art/wiki/stage/<entry>.json`, written and checked against the local install by `tools/wiki/stagemap.py` (`tiles`, `build`, `check`) through `tools/wiki/tavfs.py`, the engine's file precedence in Python; not packed yet |
| The demos' generators; the headless check of every demo | `tools/wiki/gen-quickbuild.mjs`, `tools/wiki/gen-buildshapes.mjs`; `tools/wiki/simcheck.mjs` |
| The data in the DLL | `art/wiki/` (`tree.json`, `layout.json`, the entries) packed by `tools/hudpak.py --set wiki` into the resource `WIKIPAK`; read by `tagpu/ddraw/src/tagpu_wikidata.c` (through `tagpu_pak.c`, the archive reader the HUD shares) |
| The window's layout in the DLL | `tagpu/ddraw/src/tagpu_wikilay.c`: pure, the HUD stack's ops, text and hit boxes for a page state (`WIKIPAGE`) |
| The layout check, C against the mockup | `python3 tools/wiki/wikilaycheck.py`, with `tools/wiki/wikilaycheck.c` |
| The simulation in the DLL | `tagpu/ddraw/src/tagpu_wikisim.c` |
| The parity check, C against `sim.js` | `node tools/wiki/simparity.mjs [--wine]`, with `tagpu/ddraw/tests/wikitest.c`; `--hash` prints each demo's final state hash from `sim.js` |
| The stage in the DLL | `tagpu/ddraw/src/tagpu_wikistage.c`: a view copied by value, built into HUD ops |
| The stage's check, C against `stage.js` | `node tools/wiki/stagecheck.mjs`, with `tagpu/ddraw/tests/wikistagetest.c` |
| The window and the button in the DLL | `tagpu_wiki.c` (input, pause; game thread), `tagpu_wikiframe.c` (the layout), `tagpu_wikistate.c` and `tagpu_vk_hud.c` (drawing; render thread) — [GPU status](gpu-status.html) §2.12c |
| The game test of the window | `tools/wiki_regression.py <instance> sp --launch` (or `recorded`), `tools/wiki_regression.py <name> mp` |
| Serving it | `python3 tools/hudart/serve.py`, then `http://127.0.0.1:8000/tools/wiki/mockup/` |
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
  its corner (the game draws no number; the mockup does, so the queue order can be read).

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
- the cursor, one of three kit icons (`cursor_arrow`, `cursor_move`, `cursor_build`), drawn at
  screen size centred on the pointer. `tools/wiki/cursors.sh` rasterises them once, in Chromium,
  from `stage.js`'s own `cursorShape`, so the mockup and the game draw the same pixels.

The map is fitted into the stage with a pad of 6 units (`stage.js` `fit`, `tagpu_wikistage_fit`):
the one function the game's render thread draws with and its game thread will turn a click into a
world point with.

The mockup's `gfx=game` draws the same stage in the game's own graphics, rendered from the local
install (*In game: what each element is drawn with*, below); the layout and the simulation do not
change for it.

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
from. No game file is copied. A mod's own figures are not shown: the demos are the stock game's.

## The mockup's controls

A strip above the stage, outside the game frame:

| Control | Values | URL switch |
|---|---|---|
| Resolution | 1920×1080, 2560×1440, 3840×2160, 3440×1440, 1280×1024, 1024×768, fit to the browser | `res=3840x2160` / `res=fit` |
| Window size | share of the screen, default 90 % | `size=90` |
| Mode | single player (pauses) or multiplayer (keeps running) | `mode=sp` / `mode=mp` |
| Detected mod | Stock, Total Mayhem, ProTA, Twilight, Escalation, TA Zero, unknown | `mod=escalation` |
| Open, entry, demo | the wiki open on an entry and a demo | `open&entry=quickbuild&demo=3` |
| Try it | Try it yourself on that demo's scene | `try` |
| Rewind button | shown (a live recorded skirmish) or absent | `rewind=1` / `rewind=0` |
| Skin | the kit, or the flat whitebox | `skin=kit` / `skin=whitebox` |
| Graphics | the stage's whitebox, or the game's own graphics from `gfx/` | `gfx=whitebox` / `gfx=game` |

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
| Ground | The map's TNT tiles: the tile set `*(main+0x14283)`, 32 × 32 8-bpp, and the tile map `*(main+0x1428B)`, one u16 per 32-pixel cell | `tagpu_terr.c` `ensure_atlas` (the R8 atlas, 64 tiles a row, 34-texel pitch) and `tagpu_terr_gather`; `tagpu_vk_terr.c` `tagpu_vk_terr_record`. `tagpu_terrown.c` `terr_fill` suppresses the engine's blit `0x483FA0`. | `ground.png`: the most used tileable tile under flat, featureless ground of the first lush map in archive order (Crossing Two Rivers, tile 13), repeated |
| Metal deposit | A map **feature** with `metal=` and footprint 3 × 3: on lush maps `lushmetal1..3` (`metals.gaf`, `seqname` frame 0). The metal a mex reads is the feature-grid byte `+7`, which `0x422040` writes from the definition; the sprite is only the picture. | The feature pass, which has no metal case: `tagpu_feat.c` `tagpu_feat_gather` → `draw_feature`; `tagpu_vk_feat.c` `tagpu_vk_feat_record`. `tagpu_featown.c` skips the engine leaf `0x46A610`. | `metal.png`: `lushmetal3` (metal 223) at its hotspot, stretched over each patch above SurfaceMetal of at most 6 × 6 cells. Broad metal ground and cells at SurfaceMetal get `metalground.png` (a metal-world map's tile) |
| Geothermal vent | A map feature with `geothermal=1`, footprint 1 × 1: on lush maps `lushvent01..03` (`lushvents.gaf`, `geovent0N`) | The feature pass, as for metal | `vent.png`: `lushvent01` frame 0, at its hotspot |
| Grey fog, unexplored ground | The engine's fog grid, built by `0x4843C0` from the line-of-sight counters and the mapped bits | No separate pass. Each world pass applies `tagpu_glsl.h`: `TAGPU_GLSL_FOG_TERRAIN` paints unexplored ground palette index 0 (black), `TAGPU_GLSL_FOG_GREY_RGB` turns a fogged pixel into its R+G+B mean, and `TAGPU_GLSL_FOG_DISCARD` hides units and effects outside sight. The grid is rebuilt by `tagpu_terrown.c` `terr_fogtick` and, zoomed out, by `tagpu_fogwide.c`. | Unchanged: the whitebox overlays |
| Units and buildings | Each unit's 3DO, posed by its COB script. Faces are textured with GAF frames; a team-coloured face takes frame *k* = the player's logo colour. | The pose is copied by `tagpu_packet_pub.c` `fill_pieces` and composed by `tagpu_native.c` `posed_pose`. `tagpu_posebake.c` `tagpu_posebake_unit` bakes it, `tagpu_posedraw.c` `tagpu_posedraw_unit` hands it over, and `tagpu_vk_unit.c` `tagpu_vk_unit_record` draws it. Team colour: `tagpu_packet_pub.c` `player_logo` reads PlayerInfo `+0x96`; `tagpu_render3do.c` `face_texframe` takes that frame of a team-coloured face's sequence. | `units/<name>.png`: the 3DO in its rest pose, facing 0, at TA's oblique camera, flat-shaded by the PALETTE.SHD rows, with player 0's frames (the ARM blue); labels off, drawn back to front by world z |
| Selection | The engine's box `0x46A530`, in GUI colour `0x0A` | `tagpu_native.c` `selbox_emit` → `tagpu_mark.c` `tagpu_mark_emit_selbox` → `tagpu_vk_mark.c` `tagpu_vk_mark_record`. `tagpu_markown.c` `mark_selbox` redirects the engine's box. | Unchanged: the whitebox brackets |
| Placement ghost | The armed building's 3DO, `build_unit_id` at `main+0x2CC4`, at rest, heading `0x8000`, alpha 0.40 | `tagpu_native.c` `ghost_record` → `ghost_one` (alpha set in `ghost_armed`) → `tagpu_vk_unit.c` `tagpu_vk_unit_record_ghosts`. The square under it is the next row's. | The building's sprite tinted green or red |
| The build square, and a gesture's sites | Outlines in the GUI colour table at `main+0xDCB`. `0x0A` is green, `0x04` red (refused), `0x0E` yellow (takes a queued order off). These are table indices, not palette indices: stock reads them as palette `0xE9`, `0xD5` and `0xC2` (`terrain-depth.md`, the live read on Two Continents). | `tagpu_mark.c` `gather_cursor` (the single square, coloured by `region_flags` bit 6) and `gather_place` (one square per site), both through `square_emit`. The site colours are `tagpu_place.h` `PLACE_OK`, `PLACE_BLOCKED` and `PLACE_TOGGLE`, assigned by `tagpu_place.c` `classify`. `ghost_record` adds a ghost at each site. | The sites' squares unchanged, over the building's sprite at 45 % |
| Queued build squares | The engine's queued-build outline `0x438C00`, drawn while Shift is held | `tagpu_order.c` `draw_build`, a port with the engine's ten-tick sweep, gathered into the marker pass. `ghost_record` adds a translucent 3DO on each queued site not yet started. | Unchanged: the whitebox's dashed squares, in the owner's colour |
| Order lines | TA draws no lines. It draws marching route dots in the `pathicon` ink, a waypoint sprite, target circles and range circles. | `tagpu_order.c` `draw_dots`, `draw_sprite` (a crosshair in place of the GAF sprite), `draw_circle`, `draw_ranges` | Unchanged: the whitebox's dashed lines |
| Construction progress | The unit's build fraction: unit `+0x104`, the fraction remaining, staged as the engine's nano frame (`0x458D30` / `0x458DD0`) | `tagpu_render3do.c` `tagpu_r3d_nano_state` stages it; the unit shader in `tagpu_native.c` applies it, and the wire is `tagpu_posedraw.c` `pd_record`. | The unbuilt share a faint green sprite, the built share filling from the bottom in full colour; a reclaim the reverse, in faint red |
| Cursor | `anims/cursors.gaf`, chosen by the engine's picker `0x43E490` into `main+0x2CBE` from `cursor_ary` (`exe-reverse-engineering.md`, the cursor table): `cursornormal`, `cursormove`, `cursorselect`, … | `tagpu_gui_surf.c` `tagpu_gui_cursor_frame` takes the frame from the packet; `sharp_cursor` draws it last in the UI's sharp layer. | `cursornormal` and `cursormove`, frame 0, at their hotspots, nearest-scaled by round(height / 720) |
| Keys strip | None: the game has no such strip | Nothing in the DLL draws one. A wiki window would draw it with the replay HUD's stack (`tagpu_hudlay.c`, `tagpu_huddraw.c`, `tagpu_hudtext.c`, `tagpu_vk_hud.c`), or with TA's font through `tagpu_text.c`. | Unchanged |

**The generator.** `tools/wiki/gfx.py` writes `tools/wiki/mockup/gfx/`, which is gitignored:
everything in it is derived from the game's files. Run it with the shared venv
(`.venv-undither/bin/python tools/wiki/gfx.py`); it takes `--gamedir`, else `TA3DO_GAMEDIR`,
else Steam's default library. It reads the install with `tools/ta3do`'s HPI, palette, GAF and 3DO
readers and its COB Create-hide scan, and with `tools/tascene`'s TNT, feature definitions, GAF
hotspots and SHD calibration. The archives load in `ta3do`'s order, then `rev31.gp3` again, so its
copy wins over any `.ufo`.

- **The units.** Every unit an entry's `units` names is rendered at 3 pixels a world unit, with
  ×3 supersampling. The rest pose is tascene's: pieces at their offsets, what the script hides at
  Create left out, the selection primitive skipped. Facing 0 is TA's default heading `0x8000`.
  The camera maps screen y to world z − height / 2, the shear of 0.5. Each face is flat-shaded by
  `tagpu_render3do.c`'s rule as tascene's parity lane ports it (the SHD row's RGB multiplier).
  Quads take per-pixel bilinear texture coordinates; other faces are fanned. `anchor` is the
  pixel of the unit's world position.
- **The other graphics.** The ground and metal-ground tiles, the deposit and the vent at their
  GAF hotspots, and the cursors at their own pixels. All are written to `gfx/index.json` with
  their sizes and anchors.
- **Colour.** Palette colour throughout: no restorer runs.

The page draws all of this with `tools/wiki/mockup/gfx.js`. `stage.js` hands it each element,
through the hook `wiki.js` passes, and keeps its whitebox wherever `gfx.js` declines one. Without `gfx/index.json`,
`gfx=game` falls back to the whitebox, and the controls strip says "game graphics: run
tools/wiki/gfx.py".

**What the simplified simulation cannot drive.** No COB script runs, so:

- every unit stands in its rest pose;
- a kbot does not walk and a constructor does not aim its nano-lathe;
- an extractor's arms do not turn, and a building's doors and panels stay as the 3DO has them
  at rest.

The nano spray is the engine's particles (`PK_PART` kind 5, drawn by `tagpu_fx.c`
`tagpu_fx_gather`), and it starts at the point COB `QueryNanoPiece` returns, so the mockup has
none. Units do not turn to face where they walk: the sim keeps no heading, and every sprite faces
0. The engine's nano-frame staging (bands in the palette's blues, the wire) is replaced by the
green fill above.

## The first entry: Quick Build: Resource Extractors

The feature is [Resource snapping and Quick Build](tadr-port/resource-snapping.md). BAR's names
for the same ideas are *Quick Build Extractor* (its widget) and *Upgrade T1 » T2*. Its demos:

| # | Demo | What the player sees |
|---|---|---|
| 1 | Snap onto the deposit | an ARMMEX placed from the build menu near a deposit lands on it (search radius 3) |
| 2 | Hold Alt to place exactly | with Alt held the ghost stays where the cursor is |
| 3 | Click a deposit to build | with a constructor selected, hovering a deposit shows the ghost; a right-click builds, no build menu |
| 4 | The best extractor that fits | ARMCK and ARMACK selected: ARMMOHO where it fits; on a deposit too small for it, ARMMEX |
| 5 | Queue several with Shift | Shift+right-click deposit after deposit; the mode stays on |
| 6 | Cancel a queued one | Shift+right-click its site again |
| 7 | Helpers assist | in a mixed selection, the builders that cannot make it guard the ones that can |
| 8 | Upgrade your extractor | right-click a finished ARMMEX with ARMACK selected: reclaimed, then ARMMOHO built |
| 9 | Geothermal vents | the same click on a vent builds ARMGEO |
| 10 | Metal maps | on uniform metal ground a click is a move; only a richer deposit builds |
| 11 | Fog | an unexplored deposit does not snap; one under grey fog does |

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
| 8 | Ring around a queued build | two ARMSOLAR queued by Shift-click; Shift+Ctrl over the second, still only a queued square, rings it |
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
2.4 s on the stage (58 s in the game), an ARMDRAG 0.27 s.

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
  "units": { "ARMMEX": { "footprint": [2, 2], "extracts": 0.001, "…": "…", "from": "ARMMEX.fbi" } },
  "scene": { "…": "the map every demo starts from" },
  "demos": [ { "id": "snap", "title": "Snap onto the deposit",
               "scene": { "…": "additions to the entry's scene" },
               "inputs": [ { "at": 0, "move": [120, 80] }, { "at": 30, "down": "rmb" },
                           { "at": 32, "up": "rmb" }, { "at": 30, "caption": "…" } ],
               "end": 240 } ] }
```

- **Time is in simulation ticks**, 30 a second, so a demo plays the same at any frame rate and the
  game's version can be compared tick by tick.
- **Positions are world units** (a cell is 16), the game's own.
- **Keys and buttons**: `shift`, `alt`, `ctrl`, `z`, `x`, `lmb`, `rmb`; `{ "build": "ARMMEX" }`
  picks a build-menu button (the manual placement mode); `{ "build": null }` leaves it. A `z` or
  `x` down steps again while the key is held, as an auto-repeat does.
- **The scene**: the map's size in cells, its `surfaceMetal`, its metal as rectangles of a value,
  its vents, its fog rectangles (`grey` or `black`), the units (type, owner, position, finished or
  not, and optionally move orders), and which are selected.
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
  "cells": [40, 24],
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

## The simulation's contract

`sim.js` is the mockup's and `tagpu/ddraw/src/tagpu_wikisim.c` the DLL's: a port of `sim.js`
function for function, under its names in C's spelling (`siteOk` is `site_ok`). They agree tick
for tick on the same inputs, and `node tools/wiki/simparity.mjs` checks it: it plays every demo of
every entry through both and compares every field of the view, every unit's orders and the state
hash on every tick, then replays `simcheck.mjs`'s rule cases (the rules no demo shows) through the
C sim from a recording of their calls on `sim.js`, and compares every answer they read, the state
hash after every view included. A rule case must finish with one sim before it makes the next: the
recording replays one at a time. For each demo and case it names the first differing tick or call
and field. MEASURED 2026-10-25: all 21 demos (9298 frames) and 9 rule cases equal, both natively
(gcc, with AddressSanitizer and UndefinedBehaviorSanitizer) and with `--wine` (the DLL's compiler
and flags, run under Wine).

- **Bit for bit, by construction.** Both visit units in creation order, use no clock and no
  randomness, and compute on doubles with + − × ÷ and sqrt only, each rounded once. The DLL
  compiles `tagpu_wikisim.c` alone for SSE2 (`-msse2 -mfpmath=sse`, the stack realigned, as the
  game's threads align it to 4 bytes), so the wiki's simulation needs an SSE2 processor: built for
  x87, whose intermediates keep 80 bits, 1196 of the 9298 frames differ from `sim.js` in a last
  bit (MEASURED 2026-10-25).
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
  in the same way, and `simcheck.mjs` checks it as a rule case.
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
  positive. Every other absent figure reads as 0, which every rule treats as it treats
  `undefined`. It also refuses a map larger than 128 × 128 cells, which `sim.js` would play.
- **Where the C cannot throw.** A scene that selects a unit it does not have makes
  `tagpu_wikisim_init` refuse it, where `sim.js` throws; an allocation that fails leaves out what
  it would have added and sets `oom`. Cells beyond ±2²⁰ (positions far off any map) are held
  there, and a gesture's cells to the int16 `place_shape` takes, where `sim.js` keeps the number;
  every test refuses such a cell in both.
- **Not ported:** `sim.log`, the construction events `simcheck.mjs`'s checks read. The view and
  the orders carry the state they follow from, and the parity check compares those.

**What it implements, from the feature's own rules** (`resource-snapping.md`), following
`tagpu_resource.c` function for function (`site`, `snapped_site`, `distinct`, `same_plan`,
`find_cancel`, `queued`, `quick_choice`, `plans_block`, `site_known`, `fits`):

- **Manual snapping** (a build-menu extractor or geothermal plant): candidates within a square
  radius of 3 cells; a mex scores the footprint cells whose metal is strictly above
  `surfaceMetal`, only on explored ground; the highest score wins and a tie goes to the candidate
  nearest the raw cursor (`candidate + 0.5`); a second search of radius max(footprint) around the
  winner must pick the same place, or the placement stays where the cursor is. A geothermal plant
  snaps to the nearest legal place on a vent. Occupied deposits stay targets (the ghost is red).
  Alt bypasses snapping.
- **Quick Build** (no build-menu mode, at least one selected builder able to build a resource
  building): hovering empty ground over a distinct deposit or a vent shows the ghost of the
  strongest option the selection can build that fits; a right-click orders it, replacing the
  participating builders' orders, and Shift+right-click appends. The mode stays on.
  Selected builders that cannot build it guard those that can. Units that cannot build keep their
  orders.
- **A queued site**: hovering it shows its ghost; Shift+right-click removes the selected builders'
  matching plans; a plain repeat click changes nothing.
- **Upgrade**: hovering a finished extractor the player owns, with a selection that can build a
  strictly stronger one that fits, shows that one; the click orders a reclaim, then the build.
- **Metal maps**: uniform metal ground is not a deposit (a right-click there is a move); a
  distinct richer patch is.
- **Fog**: unexplored (black) cells do not count; grey-fogged ones do.
- **Selection**: left-click a unit, left-drag a box, left-click empty ground to deselect.
- **Orders**: a right-click with nothing to build is a move.

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

- units walk in a straight line at their speed and stop within their build distance;
- construction fills at the builders' total worker time over the building's build time, with no
  resource cost and no economy; a reclaim takes as long as building it would;
- construction and reclaim run at the entry's `pace` (`"pace": {"build": N}`), N times faster
  than the game, which the stage says: an ARMMOHO takes one ARMACK about 223 s
  (BuildTime 35,750 / WorkerTime 160), too long to watch;
- no pathfinding, collisions, terrain height, enemies or other orders;
- the engine's own site test (`0x47D2E0`) as: inside the map's interior cells, clear of
  structures, a geothermal plant with a vent under it; a mobile unit never blocks a site, where
  the engine's test refuses one standing on it;
- the gesture: Shift released after a placement disarms at once, where the engine disarms at the
  first mouse record that finds Shift up; arming a building clears the Shift-placement bit
  [INFERRED: when the engine clears it is not established]; order nodes flagged `0x40000`, which
  the engine's toggle matches but does not remove, do not exist in the sim; the one-site Shift-click
  orders only builders whose build list has the building, where the engine orders every selected
  builder (the same under stock input, which arms a building for one unit only).

**The demos are generated**: `node tools/wiki/gen-quickbuild.mjs` and
`node tools/wiki/gen-buildshapes.mjs` write `art/wiki/entries/<id>.json` (their cursor paths are
hundreds of small moves), and `node tools/wiki/simcheck.mjs` plays every demo headless and checks
its claim on the simulation's state, plus the rules no demo shows; `simparity.mjs` then holds the
DLL's simulation to `sim.js` on the same demos and rules.

**What it draws** (its view): the cells, metal, vents and fog; the units with their selection and
progress; the ghost and its colour; the queued sites; the order lines; the cursor and its shape;
the keys held; the caption; a gesture's sites (`sites`: centre, footprint, `state` `place`,
`refused` or `remove`, and `order`, the site's place among the orders the release adds), its
`shape`, and the armed building's `spacing`.

## Gaps

- **No wiki on GDI** (the owner's decision, 2026-10-25): the button and the window are drawn by the
  Vulkan HUD pass only, as the rewind button is, so on GDI the wiki cannot open.
- **The window is empty.** The button, the window frame, the input capture and the pause are in
  the DLL; the page, the stage and Try it yourself are the plan's landings 3 to 5.
  `tagpu_wikilay_build`, the simulation and `tagpu_wikistage_build` link into the DLL unused.
  `tagpu_huddraw_build` takes the replay HUD's `HUDOPS`, so the stage's own, larger op list needs
  it to take an op list before the stage can be drawn. The draw must honour `HOPF_INK` (a text in
  its run's colour with no shadow or outline: dark ink on a light face), which `tagpu_huddraw.c`
  does not yet test.
- **The stage's budget holds the data's map, not the largest.** Cells and fog are one op a cell,
  as the canvas calls them: 2587 ops at most over the demos (40 × 24 cells), against
  `WIKI_MAXOP` 16384. A square map past about 70 cells a side, fogged everywhere, drops ops and
  edges the stage in red.
- **No HUD face has the mockup's control glyphs or the tree's marks**: play ▶, pause ❚❚, replay ⟲,
  the folds ▾ ▸ and the entry's dot •. The browser sets them in its fallback face, which the DLL
  does not carry, so the game would draw each as its face's `.notdef`, and the play and replay
  buttons, which are sized to their glyphs (37.1 and 29.3 units wide at 1920 × 1080 while play
  shows pause), would take the `.notdef`'s width. `wikilaycheck.py` gives the C layout the
  browser's advances for them and names them. Kit icons, as question 5 proposes for the stage's
  cursors, or the glyphs in a HUD face would close it; the mockup changes first.
- **Chrome sets a face at a size of whole sixty-fourths of a pixel**, by a rule not identified:
  9.0667 px is set at 579/64 and 9.07 at 580/64 (MEASURED 2026-10-25 against stb_truetype's
  widths, which equal Chrome's at those sizes to the sixty-fourth). Where a font size is not a whole
  number of sixty-fourths -- every style at 1024 × 768, and the chips' 6.8 units at every
  resolution -- the mockup's lines are up to 0.2 % narrower than the game's, and a line that just
  fits in the mockup can wrap one word earlier in the game. `wikilaycheck.py` gives the C layout
  the browser's set size and compares the rules on it. The game sets every face at its own size.
- **The stage's cursor icons must be in the kit as cut.** `fit.py --set wiki` rewrites
  `art/wiki/kit/` from `art/wiki-full/kit/` and drops parts that are not there, so
  `tools/wiki/cursors.sh` writes the icons into both.
- **The resource rules are copies.** The C sim calls the game's shapes and take-off rule
  (`tagpu_placegeo.c`), but its resource rules and the rest of the gesture's port `sim.js`'s
  mirrors of `tagpu_resource.c` and `tagpu_place.c` rather than calling them. `simparity.mjs`
  holds the C to `sim.js`, and so `sim.js`'s take-off port to the game's function; nothing holds
  the resource rules to the game's code, so a change there is followed by hand in `sim.js` and
  `tagpu_wikisim.c` (the plan's `rulecheck.py` would notice it; it is not built).
- **The state hash is not checked in the game yet.** `tagpu_wikisim_hash` and its `sim.js` twin
  agree on every tick natively and under Wine; the DLL's own i686 build on the game thread logs it
  only once landing 4's `tagpu_wiki.check` exists.
- **No build gate runs the parity checks.** They need node and gcc (and Wine for `--wine`); `make`
  compiles and links the simulation and the stage but runs neither check. `stagecheck.mjs` builds
  the stage natively (x86-64, SSE); the DLL compiles it for SSE2, but no check runs the i686 build.
- **The figures are the stock game's.** A mod with other extractors or other build lists shows the
  stock demos.
- **No hotkey** opens the wiki.
- **The spacing readout is the mockup's**: the game has no on-screen spacing value.
- **Build Split is not shown**: it is not built.
- **`gfx=game` keeps whitebox elements the game draws differently.** These stay as they are:
  - the order lines (the game draws route dots and a waypoint crosshair);
  - the queued squares (the game shows them only while Shift is held, with a ghost on each);
  - the fog overlays (the game paints unexplored ground black and turns grey fog into its RGB
    mean; the mockup's translucent per-cell fills also show a seam where neighbouring cells
    overlap);
  - the order numbers on a gesture's sites, in dark ink that reads poorly over the sprites.
- **The cursor during build placement is not established.** No source names the `cursors.gaf`
  index the engine shows in mode `0x0E`, and there is no `cursorbuild`. `gfx=game` shows
  `cursornormal` there [INFERRED].
- **What draws the geothermal steam plume is not established**: a feature animation or the
  engine's particles. `gfx=game` shows the vent's frame 0, without a plume.
- **The deposit and ground art are one lush map's.** A demo's map has no world, and a patch
  larger than the 3 × 3 feature stretches its sprite.

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
