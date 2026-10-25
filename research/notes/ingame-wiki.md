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
| The demos' generator; the headless check of every demo | `tools/wiki/gen-quickbuild.mjs`; `tools/wiki/simcheck.mjs` |
| Serving it | `python3 tools/hudart/serve.py`, then `http://127.0.0.1:8000/tools/wiki/mockup/` |
| This page | the decisions, the data format and the simulation's contract |

The first and only entry is **Quick Build: Resource Extractors** (Quality of Life → Building).
The Shift-drag placement family, landed on `main` at `90c67078`, is the next entry to add.

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
  construction as a fill.

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
- **Keys and buttons**: `shift`, `alt`, `ctrl`, `lmb`, `rmb`; `{ "build": "ARMMEX" }` picks a
  build-menu button (the manual placement mode); `{ "build": null }` leaves it.
- **The scene**: the map's size in cells, its `surfaceMetal`, its metal as rectangles of a value,
  its vents, its fog rectangles (`grey` or `black`), the units (type, owner, position, finished or
  not), and which are selected.

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
  the engine's test refuses one standing on it.

**The demos are generated**: `node tools/wiki/gen-quickbuild.mjs` writes
`art/wiki/entries/quickbuild.json` (its cursor paths are hundreds of small moves), and
`node tools/wiki/simcheck.mjs` plays every demo headless and checks its claim on the
simulation's state, plus the rules no demo shows.

**What it draws** (its view): the cells, metal, vents and fog; the units with their selection and
progress; the ghost and its colour; the queued sites; the order lines; the cursor and its shape;
the keys held; the caption.

## Gaps

- **The top-right row is drawn by two renderers today**: the sprocket on every renderer, the
  rewind button by the Vulkan HUD pass only. Where the wiki button is drawn on GDI is not decided.
- **Nothing of the wiki is in the DLL.** The window, the button, the pausing and the input capture
  are specified here and in the mockup only.
- **The figures are the stock game's.** A mod with other extractors or other build lists shows the
  stock demos.
- **No hotkey** opens the wiki.
- **The Shift-drag placement family** is not an entry yet.
