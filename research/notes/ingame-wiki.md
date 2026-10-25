# The in-game wiki: Impure's features, shown and tried in the game

## Status

**Designed 2026-10-25; the browser mockup is the first build.** The owner wants a wiki inside the
game that explains Impure's own features with examples a player can watch and try. The decisions
below came out of a design interview with the owner on 2026-10-25; each is recorded with its
reason. Nothing of the wiki is in the DLL yet.

| What | Where |
|---|---|
| The tree of categories and entries | `art/wiki/tree.json` |
| One entry: its text, its mods, its demos | `art/wiki/entries/<id>.json` |
| The mockup, which reads only those files | `tools/wiki/mockup/` (`index.html`, `wiki.js`, `wiki.css`, `sim.js`) |
| The demos' generators; the headless check of every demo | `tools/wiki/gen-quickbuild.mjs`, `tools/wiki/gen-buildshapes.mjs`; `tools/wiki/simcheck.mjs` |
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
  mockup does not show. A parity check like `laycheck.py` (the game's simulation against the
  mockup's, on a demo's inputs) is the way to keep them equal once the DLL has it.
- **Try it yourself is a simplified simulation in the game too.** A second hidden game instance,
  or swapping the running game out for a practice scene and back, were considered and declined
  (2026-10-25): the simplified simulation is enough, and the mockup can then be exact. Where it
  can, the DLL's version calls the real feature's rule code (the snap scorer, the best-fit
  choice) instead of a copy.

### Content is data

- **The tree** is `art/wiki/tree.json`: categories that nest to any depth, their leaves entries.
- **An entry** is `art/wiki/entries/<id>.json`: title, subtitle, text, the mods that lock the
  feature off, its unit figures and its demos.
- **A demo is a scene and an input timeline, nothing more.** The inputs (cursor moves, keys and
  buttons down and up, waits) are played through the same simulation as Try it yourself, which
  draws whatever follows from them. A demo therefore cannot show something the rules would not do,
  and a demo and Try it yourself cannot disagree. The one scripted extra is a caption, written at
  a given tick.
- `art/wiki/` mirrors `art/hud/`, which the DLL already embeds (`tools/hudpak.py`).

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

Real assets replace the boxes later; the layout and the simulation do not change for them.

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

Behind the window: the replay HUD mockup's backdrop (`research/notes/assets/hud-art/backdrop.png`),
scaled to the resolution, under a whitebox of Impure's top bar: metal and energy on the left, and
the wiki, record/replay and gear buttons at the right.

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
`tagpu_placegeo.c`. Only what the game does today is shown: spacing is shown following the design
(below), and Build Split, which is not built, is not.

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

## The simulation's contract

`sim.js` is the mockup's; the DLL's must agree with it tick for tick on the same inputs.

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
`toggles`, `classify`, `ring_target`, `remove_toggled` and `append_sites` as `commit`,
`place_button` as the press and the release), with `tagpu_placegeo.c`'s `place_shape` and
`place_ring` ported line for line, integer arithmetic included (checked against the compiled C on
20,000 random cases, 2026-10-25: identical):

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
- **A site's colour**: red where the site test refuses it; yellow where some selected builder has
  a build order of any building within 16 world units of the site's centre on both axes
  (`0x43AFC0`'s predicate, inclusive); green otherwise.
- **The release with Shift held** commits. A one-site gesture ending at the cursor site is the
  stock Shift-click itself: the first build order within 16 world units is taken off instead of
  adding one, otherwise the site is appended. Any other gesture is classified afresh, then per
  selected builder: each site not red takes off one build order no earlier site took — the one at
  its own position first, else the first within 16 world units — and adds nothing; the rest are
  appended in the shape's order while the queue is shorter than 1024 orders. Every selected
  builder gets them, its build list not consulted, as `0x419670` orders.
- **Shift released before the button** drops the gesture, orders nothing and keeps the building
  armed. A placement or a commit that changed a queue sets the Shift-placement bit (`main+0x2CC6`
  bit 5), and **Shift released afterwards disarms**, as after a stock Shift-click (`0x4992D8`).
  **A right press** disarms and ends the gesture.
- **Spacing**, following the design (`.data/qol-shift/bar-placement-design.md` §6 and §17.1) until
  the key handler lands; recheck against `tagpu_place.c` then: with a building armed and Alt down,
  Z adds one empty cell between neighbours and X takes one away, 0..16, kept per building type for
  the sim's life (the game saves it in `impure.cfg`); every Z or X down steps, an auto-repeat
  included, and the next evaluation shows it, mid-drag too. The pitch is footprint + spacing cells.
  The page's "Spacing: N" readout is the mockup's; the game has none specified yet.

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
its claim on the simulation's state, plus the rules no demo shows.

**What it draws** (its view): the cells, metal, vents and fog; the units with their selection and
progress; the ghost and its colour; the queued sites; the order lines; the cursor and its shape;
the keys held; the caption; a gesture's sites (`sites`: centre, footprint, `state` `place`,
`refused` or `remove`, and `order`, the site's place among the orders the release adds), its
`shape`, and the armed building's `spacing`.

## Gaps

- **The top-right row is drawn by two renderers today**: the sprocket on every renderer, the
  rewind button by the Vulkan HUD pass only. Where the wiki button is drawn on GDI is not decided.
- **Nothing of the wiki is in the DLL.** The window, the button, the pausing and the input capture
  are specified here and in the mockup only.
- **The figures are the stock game's.** A mod with other extractors or other build lists shows the
  stock demos.
- **No hotkey** opens the wiki.
- **Spacing is the design's, not the game's**: the Alt+Z / Alt+X handler is not in `tagpu_place.c`
  yet (`place_shape` is called with spacing 0). The spacing demo and rule follow
  `bar-placement-design.md` §6 and §17.1 and are to be rechecked against the code when it lands.
- **Build Split is not shown**: it is not built.
