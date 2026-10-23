# The replay HUD's art: painted, cut, repeated and filed

## Status

**The replay HUD is painted in worn painted steel.** The owner picked this look, M3, from four
re-imaginations of the dock in old metal. The dock was painted in it first, and then, from a
concept the owner approved, every other plate and button was repainted to match. The parts are
filed losslessly in `art/hud/`, with their measurements in `art/hud/kit.json`. Two programs draw
them by the same rules:

- the mockup in `tools/hudart/mockup/` ([The replay HUD's layout](hud-layout.md));
- the DLL, through `tools/hudpak.py`, which packs `art/hud/` into `inc/tagpu_hudpak.bin` at build
  time ([The replay HUD in game](hud-in-game.md)).

This page is the full reference for making that art: the style guide, the pipeline, the tools,
what each tool measures, and how the mockup and the DLL draw a part. `pixel-forge.md` at the
repository root is the short how-to for the painter, and it points here. The two agree; where
this page gives a reason or a number, `pixel-forge.md` gives the step.

**How a part is made, in one line:**

1. Draw a layout from the part specs.
2. Paint it with **pixel-forge** on **Nano Banana Pro** (model `gemini-3-pro-image`).
3. Cut the painting with the repository's cutters.
4. Make the middle of every long part repeat instead of stretch (`xtile`).
5. File the parts into `art/hud/` and check them in the mockup and the game.

**What is in the look and what is not.** These 35 parts were painted in worn painted steel:

- the one-piece plates `tp_one`, `rail_one`, `rail_forks`, `row_one`, `head_one`, `foot_one`
  and `win_one`;
- every 9-slice plate, strip and button, each with its pressed version where it has one.

The 28 icons, the marker badge, the playhead and the timeline fill come from the earlier method
and were not repainted (*Gaps*). That method painted with Codex in a "distressed gunmetal" look
and tried several looks for the whole HUD. Its record, prompts included, is in this page's git
history (`git log -- research/notes/hud-art.md`).

| What | Where |
|---|---|
| The parts, lossless; their kit; the layout | `art/hud/` ([The replay HUD's layout](hud-layout.md)) |
| The mockup, drawn from `art/hud/` | `tools/hudart/mockup/` |
| The specs: each part's size and recesses in HUD units | `tools/hudart/specs/` |
| The tools: layouts, cutters, repeats, filing, checks | `tools/hudart/` (*The tools*) |
| The packer the DLL is built from | `tools/hudpak.py` (the Makefile runs it) |
| The painter's settings and short method | `pixel-forge.md` (repository root) |
| The style guide, the pipeline, the measurements | this page |
| The renders, the prompts, the job scripts | **not in the repository** (below) |

**Not in the repository.** On the reference setup these live in the main checkout's `.data/`,
which `.gitignore` ignores:

- `.data/pixel-forge/`: pixel-forge's store, with each session's references and renders
  (`m3-kit`, the dock; `m3-all`, the rest of the HUD);
- `.data/hud-art/work/m3kit/`: the dock job's layouts, prompts and scripts
  (`compose_housing.py`, `unlabel.py`, `level.py`);
- `.data/hud-art/work/m3all/`: the whole-HUD job, with `make_sheets.py`, which draws its layouts,
  `cut_all.py`, which cuts them, and the four prompts (`pf_*.txt`).

Only the cut parts filed into `art/hud/` are committed. **Total Annihilation's own interface art**
(crops of the game's screens, palette samples) may be shown to a painter as a reference, but it
is original game art: it stays under `.data/` and is never committed (CLAUDE.md, *Publishing*).

**Evidence tags.**

- **MEASURED:** measured on the kit, the renders or a capture, with the numbers.
- **[INFERRED]:** not proved.

## The style guide

### Worn painted steel

Every plate and button is the same material. This is the material block every prompt of the
whole-HUD job carries (`.data/hud-art/work/m3all/pf_*.txt`), and `pixel-forge.md` *Prompts* states
the same rules:

- **The material.** Grey military paint over steel, chipped along every edge and bevel to bright
  bare steel. Never rust.
- **The wear.**
    - Short scratches in groups.
    - Oily grime in the corners and along the recess rims.
    - Spread evenly along a long part, with no single large feature.
- **The shape.** Crisp bevels lit from the top left, and a thin near-black outline.
- **The recesses.** Dark sunken steel: near-black grey, faint grime, a crisp inner bevel.
- **The colour.** Neutral greys only: no green, cyan or teal, and no brown rust.
- **Never** marble, stone, concrete, cracks or veins.

### The decisions that hold

- **Buttons are a shade lighter than the plates they sit in,** so a button stands apart from its
  bay. MEASURED on the kit (the median brightness of the opaque pixels, 0–255):
    - `bay_button`: 149, in `tp_one`, whose steel (the pixels above 70) averages 111;
    - `act_button_wide`: 106, on `hover_strip`, whose median is 80.
- **Pressed is the same button pushed in.** The face is sunk and darker, with an inner shadow
  along the top and left. It is the only state language: a button that is held, selected or on
  is drawn pressed. There are no lamps, LEDs or glows, and no brightness change on a row.
    - Each layout draws a pressed part darker than its raised twin and labels it PRESSED.
    - `cut.py` cuts every sheet of buttons with `PRESSED_DARK=1.5`.
    - MEASURED medians, raised against pressed: `bay_button` 149 / 90, `chip_face` 104 / 57,
      `close_face` 105 / 55, `takeover_button` 105 / 74, `pick_row` 52 / 39.
- **The team header is a solid plate.** Its left part is plain plate, with no recess: the game
  writes the team's title there, in a **text zone** (*Text zones*). Only the unit sum and the two
  income lines sit in recesses (`specs/head_plate.py`). The header is the same light steel as the
  rows; what sets it apart is its plain left part, a field of paint where a row is a frame around
  dark recesses.
    - The title's style is `header` in `layout.json`: Oswald 500, `#ece6c0`, with a near-black
      outline, because it sits on paint rather than in a well.
- **No screws, rivets, bolts, lamps or glow** on any part. A corner is plain worn steel, with no
  dark block or notch.
- **Every part is painted empty.** No text, labels, numbers, icons, badges, fills or playheads.
    - The game draws all of these, and the mockup draws them by the same rules.
    - The text is set in the faces in `art/hud/fonts/` ([The replay HUD's layout](hud-layout.md),
      *Text*).
    - The resource colours are TA's own bar fills: metal `#d3dbff` and energy `#ffff00`
      (`layout.json` `resource`). The source is in the [engine map](exe-reverse-engineering.md),
      *The resource block, and the 33-byte memo that skips it*.
    - The timeline's played fill is the green tile `track_fill`. It is repeated along the groove,
      never stretched.

## The pipeline

| Step | What | With |
|---|---|---|
| 1 | Draw the layout: whiteboxes from the specs, at an aspect the model takes exactly, with a piece of finished art on the canvas | `whitebox.py`, `sheets.py`, or a job script on the same specs (`make_sheets.py`) |
| 2 | Render it: tests at 1K, the kit at 4K; look at every 4K render before cutting | `pixel-forge gen` |
| 3 | Prepare it: labels keyed out, scaled to 10 px a unit, an uneven flat recess levelled | `unlabel.py`, `cut_all.py`, `level.py` (job scripts) |
| 4 | Cut it | `gridslice.py`, `cut.py`, `iconcut.py` |
| 5 | Make long middles repeat | `seamband.py` (`xtile`); `tile.py` for a fill |
| 6 | File it into `art/hud/` | `file.py` |
| 7 | Check it: the rules agree, the mockup at native 4K, the game | `laycheck.py`, `serve.py` + `capture.sh`, a DLL build |

### 1. The layout

**Layouts are drawn from the specs, never by hand.** A one-piece part is a module in
`tools/hudart/specs/`. It holds:

- `DESIGN`: the part's size in HUD units;
- `ZONES`: each recess as `(x, y, w, h)` in units;
- `TEXT`, optional: each text zone;
- `XB` / `YB`: for each stretch band, the recesses it must run through;
- `S`, `CELL`, `LEGEND`: how `whitebox.py` draws it.

`whitebox.py <spec> <out>` writes the whitebox as `<out>.png`, with numbered recesses and a
legend, and `<out>.json`, the zones `gridslice.py` measures against. A sheet of 9-slice parts
comes from `PLAIN=1 sheets.py <spec> <out dir>`. `PLAIN=1` leaves out the corner marks, which a
painter reads as places for hardware.

A job may place several whiteboxes on one canvas from the same specs. `make_sheets.py` draws the
whole-HUD job's four layouts this way, at 6 px a unit, with the recesses dark and unnumbered:

| Layout | Aspect | Size in units | Holds |
|---|---|---|---|
| `grid.png` | 21:9 | 441 × 189 | `rail_one`, `row_one`, `head_one`, `foot_one` |
| `window.png` | 16:9 | 533.3 × 300 | `win_one` |
| `nine.png` | 16:9 | 480 × 270 | every 9-slice part, the pressed ones darker, each labelled |
| `long.png` | 21:9 | 441 × 189 | the live strip, the hover strip and the groove, each 420 units long |

**These rules hold for every layout:**

- **At an aspect the model takes exactly** (21:9, 16:9, 3:2, …), so nothing is stretched to fit
  and back. A sheet holds several parts at their true aspect, all at one scale. 21:9 is the
  widest aspect Nano Banana Pro takes.
- **The reference is on the canvas.** A piece of a finished part, cut from `art/hud/parts/`,
  sits beside the whiteboxes. The prompt names it as finished art to keep exactly as it is
  ("REF"). Passed as a separate style reference instead, its content was copied into the
  painting: an extra dock plate appeared above the parts.
- **A recess sits where `layout.json` puts what the game draws in it.** The whitebox's insets
  come from the layout, not from a guess at the look.
    - **The case that set the rule:** the Take over picker's list area. `layout.json` starts the
      picker's rows at `pad` + `title.h` = 4 + 16 = 20 units. The list area was painted 12.9
      units down, so the title fell on the list's edge, with a dark gap under it.
    - **The fix:** the plate band was lengthened by 6.6 units of its own paint, joined with
      `seamband.py`'s minimum-error seam, and the top slice followed (129 → 195 px, 19.5 units).
      `make_sheets.py` now draws the list area 19.5 units down.
- **A part longer than 21:9 is never squashed onto the sheet.** Squashed back, the painting's
  wear comes out several times wider than tall: the timeline housing, once painted stretched to
  21:9, was squashed back about 3.9 times.
    - **Up to about 420 units** a part fits whole on a 21:9 sheet (`grid.png`, `long.png`).
    - **Longer,** it is painted as **windows** of its whitebox, each cut at exactly 21:9 and
      stacked on one sheet: its left end, a stretch of its middle, its right end. The open sides
      run off the sheet's edge.
    - `compose_housing.py` then rebuilds the part:
        1. It fits each window's horizontal edges to the whitebox's.
        2. It places the two ends where the whitebox has them.
        3. It covers the gap with the middle window and then its mirror image, joined by
           `seamband.py`'s seams. A left-right mirror keeps the bevels lit from the top.
    - `rail_forks` was made this way. Its wells measure within half a unit of the whitebox.
- **Paint a part at about the length it is drawn.**
    - The live strip is drawn 787 units long at HUD 150. It is painted 420 units long, so the
      period `seamband.py` learns from its middle is long enough (197 units) that its wear does
      not read as a rhythm.
    - The hover strip is drawn about 137 units long at every scale, so it does not repeat.
      `cut_all.py` cuts it from the 420-unit painting and joins its two ends with a
      minimum-error seam. The filed PNG is 145.6 units long, because the 60-pixel overlap of the
      join is kept on top of the 140 units the script aims for.

### 2. The render

`pixel-forge.md` holds the settings: the store `.data/pixel-forge`, the model
`gemini-3-pro-image`, 4K, and the key `#FF00FF`. A render is:

```
pixel-forge gen -s <session> --ref <layout>.png:layout -f <prompt>.txt --size 1K   # a test
pixel-forge gen -s <session> --ref <layout>.png:layout -f <prompt>.txt --size 4K  # the kit
```

With a layout, the layout decides the aspect.

- **Nano Banana Pro, not Nano Banana 2.** Pro keeps a layout's recess widths. Nano Banana 2
  painted every recess the same width.
    - MEASURED on the dock's Pro renders: the transport plate's seven bays, and the housing's bay
      pocket, lanes and time recess, are each within 1.3 HUD units of the whitebox, with no
      re-proportioning.
- **Test at 1K and render the kit at 4K.** Every 4K render is above the 10 px a unit the kit is
  filed at (*The sizes Pro paints*, below).
- **Look at every 4K render before cutting it.** In one render in four, the model reframed its
  sheet: it painted the parts several times taller and dropped the reference. A reroll kept the
  layout.

**The prompts.** Each prompt of the whole-HUD job opens by saying what the layout is: a LAYOUT of one-piece plates, or
a SHEET of exactly N parts. It then names the finished piece as the reference to keep exactly as
it is, and asks for every grey whitebox to be painted as a finished part in the reference's
material. The material block (*Worn painted steel*) and the rules block follow. The rules block
reads:

```
RULES: keep the layout EXACTLY. Every part at the same position and size, every recess where it
is with the same width and height; add no part, remove none, merge none. Every part stays EMPTY:
no text, labels, numbers, icons, badges or fills. No screws, rivets, bolts, lamps or glow; plain
edges, no dark blocks or notches in corners. Spread the wear evenly along long parts, no single
large feature. Everything outside the parts is pure flat #FF00FF magenta.
```

- **A sheet** adds: "Plain grey parts are raised plates and buttons, buttons a shade lighter than
  plates. Parts labelled PRESSED are the same buttons pushed in: sunk, a darker face, an inner
  shadow along the top and left edges."
- **The stats window** adds: "a heavy outer frame with chipped bevels".
- **The windows of a long part** add that each strip is a window onto the same part, that its
  open sides continue past the image's edge with no end cap, bevel, outline or shading, and that
  only the part's real ends are closed.

**The sizes Pro paints.** MEASURED in the store:

| Aspect | Size | Pixels | On the job's layouts |
|---|---|---|---|
| 21:9 | 4K | 6336 × 2688 | 14.4 px a unit |
| 16:9 | 4K | 5504 × 3072 | 11.5 px a unit (`nine.png`); 10.3 (`window.png`) |
| 21:9 | 1K | 1584 × 672 | 3.6 px a unit |

### 3. Before cutting

- **Key out the labels.** A paint-over keeps a sheet's labels. `unlabel.py` returns everything
  outside the cells, each grown by a margin, to the flat key, so a label cannot be cut as a part.
- **Scale each painting to 10 px a HUD unit,** from its layout's px per unit. At 4K the one-piece
  parts would otherwise outgrow the 8 MB commit limit.
    - **The stats window goes in at 7.** At 10 it is a 13 MB PNG; at 7 it is 7.3 MB (MEASURED,
      `win_one.png`).
- **Level a flat recess painted with uneven tone.** `level.py` scales each column of the recess
  interior to the interior's median brightness, by a gain smoothed along x, so the grime keeps its
  contrast. The dock's housing had a track recess painted lighter over a fifth of its length.

### 4. Cut

`cut_all.py` runs the whole job:

- **One-piece parts.** Each is cropped to its cell plus 4 units, then measured by `gridslice.py`
  against its spec's whitebox (`whitebox.py` renders the JSON).
- **9-slice parts.** `cut.py` cuts the sheet of 9-slices with `PRESSED_DARK=1.5`, from a spec the
  script writes: each part's cell size and a border of 2 units (3 for the picker), half of which
  is the least inset `cut.py` allows.
- **Repeats.** It then runs `seamband.py` on the parts drawn longer than they were painted.
- **The scroll thumb's ends are pinned to 3 units.** The thumb is drawn as short as 10 units
  (`scroll.thumb.min`), so its two fixed ends must total less than that. `cut.py` measures the
  ends off the wear, which left them too long for that.

By hand, the cutters run as:

```
PY=.venv-undither/bin/python; H=tools/hudart
$PY $H/gridslice.py painted.png wb_row.json kit/ row_one                       # a one-piece part
PRESSED_DARK=1.5 $PY $H/cut.py <spec> sheet.png <sheet> sheets.json kit/        # a sheet of 9-slices
$PY $H/cut.py <spec> painted.png <sheet> one:<part> kit/                       # one part painted alone
ICON_DIL=25 $PY $H/iconcut.py painted.png kit/ i_a i_b i_c                      # icons, in reading order
```

**Look at every cut.** Compare the recesses `gridslice.py` prints with the whitebox's, and read
each band's stray (*How the cutters measure*).

### 5. Repeat

```
$PY $H/seamband.py kit/ live_one live_groove pick_row pick_row_down rail_one   # sets xtile
$PY $H/tile.py kit/ track_fill                                                 # a fill strip
```

**Run `seamband.py` on every part drawn wider than it was painted.** It turns the part's x
stretch band into one period of a seamless tile and sets `xtile`. The mockup and the DLL then
repeat that band a whole number of times instead of stretching it (*How a part is drawn*).

- **The parts with `xtile`:** `rail_one`, `rail_forks`, `live_one`, `live_groove`, `pick_row`,
  `pick_row_down`, `takeover_button`, `takeover_button_down`, `lanes_plate`, `lane` and
  `lane_down`.
- **For a 9-slice,** the middle becomes the period.
- **For a grid part,** the part is compacted to its two ends and one period (*Seamless middles*).
  This is the rule for a grid part whose fixed cells would otherwise leave its band too little
  room.

### 6. File

```
$PY $H/file.py kit/ row_one head_one        # replaces art/hud/parts/<part>.png and its kit.json entry
```

`file.py` rewrites each PNG with zlib's best compression and checks it pixel for pixel against
the cut. Each kit entry carries what drawing the part needs and nothing about how it was made:
its `kind`, its `scale` in PNG pixels per HUD unit, and the kind's own fields.

| Kind | Fields |
|---|---|
| `grid` | `design`, `xs`, `ys`, `wells`, `text` |
| `nine` | `slice` |
| `tile` | none |
| `whole` | none |
| `icon` | none |

Either `grid` or `nine` may also carry `xtile`. [The replay HUD's layout](hud-layout.md),
*kit.json*, defines the kinds.

**A repainted grid part brings its own measured wells,** and the layout places everything
through them. A well renamed or removed must be renamed in `layout.json` too.

### 7. Check

```
python3 tools/hudart/laycheck.py                            # must end "laycheck: the two agree"
python3 tools/hudart/serve.py &                             # no-cache, so a reload never mixes kit versions
tools/hudart/capture.sh 'http://127.0.0.1:8000/tools/hudart/mockup/?paused&res=3840x2160&game=150&hud=150' hud.png
```

- **The rules agree.** `laycheck.py` checks the DLL's layout rules against the mockup's over the
  new wells ([The replay HUD's layout](hud-layout.md), *Checking the C against the mockup*).
- **The mockup at native 4K.**
    - `capture.sh` lays the page out at device scale factor 1 and screenshots at device scale.
    - It waits until every grid part and fill is composed, and prints `missing art`, which must
      be 0.
    - Look at the capture at native 4K, never through a downscaled copy.
    - Repeat at HUD 75, 100 and 125, where a repeat shows more copies.
- **The game.** `tools/hudpak.py` packs `art/hud/` at build time, so new art needs a DLL build.
  The check of the game against the mockup fed the game's own dump is in
  [The replay HUD in game](hud-in-game.md), *Checking it against the mockup*.

### The tools

| Tool | What it does |
|---|---|
| `whitebox.py <spec> <out>` | A one-piece part's whitebox: `<out>.png` and `<out>.json` for `gridslice.py` |
| `sheets.py <parts spec> <out dir>` | A sheet of parts, one grey cell each, plus `sheets.json`; `PLAIN=1` leaves out the corner marks |
| `gridslice.py <painting> <whitebox.json> <kit dir> <part>` | Cuts a one-piece part and measures its recesses and stretch bands |
| `cut.py <parts spec> <painting> <sheet> <sheets.json> <kit dir>` | Cuts a sheet into 9-slice parts; `PRESSED_DARK=<w>` gives pressed slots the darker shapes; `one:<part>` cuts a part painted alone |
| `iconcut.py <painting> <kit dir> <name>...` | Cuts icons as whole shapes, in reading order; `ICON_DIL=<px>` joins strokes that far apart |
| `keying.py` | `despill`, which every cutter runs |
| `seamband.py <kit dir> <part>...` | Makes a part's x band one period of a seamless tile and sets `xtile` |
| `tile.py <kit dir> <part>...` | Makes a fill strip repeat along x without a seam |
| `file.py <kit dir> [--look <l>,...] <part>[=<name>]...` | Files cut parts into `art/hud/`, losslessly, replacing old ones |
| `laycheck.py` | The DLL's layout rules against the mockup's |
| `serve.py` | Serves the repository for the mockup, with no-cache headers |
| `capture.sh <url> <out.png>` | The mockup's stage in device pixels, once all its art is composed |
| `compare.sh`, `compose.py` | A button set's states side by side (play held, stats on, chips selected, a row's JUMP held) |
| `fonts.py` | Cuts the faces in `art/hud/fonts/` |
| `lookmerge.py`, `pack.py` | Tools of the earlier exploration prototype: they file parts as `<part>@<look>` and pack a kit into one JSON of data URIs |
| `tools/hudpak.py <art/hud> <out.bin>` | Packs `art/hud/` as it is into the archive the DLL embeds |

The tools need the shared venv's Python (numpy, scipy, Pillow) and DejaVu Sans Bold for the
labels. The captures need `playwright-cli`; `laycheck.py` needs it and gcc.

## How the cutters measure

### Keying and despill

**The key is flat `#FF00FF`.** The key mask is the product of two terms, each clipped to 0–1:

- magenta-ness: `(min(r, b) − g − 60) / 120`;
- red-equals-blue: `1 − |r − b| / 90`.

Paint is where the mask is under 0.5. **Every pixel then takes the colour of its nearest solid
pixel** (mask under 0.05), so the edges keep no key colour.

`despill` (`keying.py`) takes the key's share back out of mixed pixels. It computes
`s = clip((min(r, b) − g − 6) / 255, 0, 0.7)`, then returns `(p − s·(255, 0, 255)) / (1 − s)`.

- For neutral paint the share is exact.
- Paint with no red-and-blue excess over green is left alone.
- Without it, a dark outline anti-aliased into the key survives as an opaque purple line.

### One-piece parts (`gridslice.py`)

**One scale per part.** The scale is the painting's width over the design width, on both axes.
The design height is then **taken from the painting**, and the stretch bands absorb the
difference. The worn-steel paintings keep the whitebox's proportions closely. MEASURED, the
filed design against the spec:

| Part | Painted | Spec |
|---|---|---|
| `row_one` | 250 × 27.49 | 250 × 27 |
| `head_one` | 250 × 23.29 | 250 × 23 |
| `foot_one` | 204 × 36.48 | 204 × 36 |
| `tp_one` | 284 × 46.6 | 284 × 46 |
| `rail_forks` | 456 × 43.99 | 456 × 44 |
| `win_one` | 428 × 274.1 | 428 × 272 |

`rail_one` was painted 420 × 46.51 against 420 × 46. It is now 338.88 units long (*Seamless
middles*).

**The recesses.**

1. **The dark mask.** Otsu's split of the part's brightness separates recess from plate.
    - A dark plate can land on the dark side with its recesses. When the dark side covers more
      than the whitebox's recess share plus 0.2, the cutter splits the dark side again,
      separating near-black recess from dark paint.
2. **Seeds.** The dark mask is **eroded**, so grime or a shadow line across a plate band cannot
    join two recesses into one region.
    - The erosion is one HUD unit, or 0.3 of the smallest recess's short side when that is more.
    - On the stats window, grime bridged the 3-unit plate bands by more than a unit and joined
      the title strip to the economy tray.
    - Only the seeds are eroded. Every edge is measured on the un-eroded mask.
3. **One-to-one matching.** Each whitebox recess is matched to **one** region by the Hungarian
    method. The cost is the centre distance, plus the log ratio of the widths, plus the log ratio
    of the heights. A painting that shifts or resizes a recess still pairs correctly.
4. **Extents.** A recess's extent is scanned outward from its region's centre on the un-eroded
    mask: across the middle band of rows, then down the middle half of columns.

Any recess left without a region is named.

**Stretch bands.**

- **Where they go.** A band spans the overlap of the recesses its spec lists, trimmed by 30 % at
    each end. It therefore lies **inside the recesses, where every cross-section is plain**: the
    recess floor, its two rims and the plate between them repeat along it.
- **What they do.** Everything outside the bands keeps its painted size when the part is drawn
    at another size: corners, bevels and recess ends.
- **Their stray.** `gridslice.py` prints, for each band, how far a cross-section inside it differs
    from the band's median one, in 0–255 levels. A plain band strays by about 2.

### Text zones

The team header carries its title on the plate, not in a recess.

- **The spec** lists the area under `TEXT`.
- **`whitebox.py`** outlines it in light grey. `make_sheets.py` leaves it out of the layout, so
  the painter sees plain plate there.
- **`gridslice.py`** puts it into the part's `wells` at its whitebox place, scaled to the
  painting's height, and lists it under `text`. Nothing is measured there: the area is plain
  plate, so a few tenths of a unit off cost nothing.

### 9-slice parts (`cut.py`)

1. **The shapes** are the connected painted regions on the key, after a 2-pixel dilation.
2. **Matching.** Shapes are matched to the sheet's cells one-to-one by the Hungarian method. The
    cost is:
    - the centre distance;
    - plus 1.5 × the log aspect-ratio difference;
    - plus 0.5 × the log size difference.
3. **`PRESSED_DARK=<w>`.** It adds `w × z` to a pressed slot's cost and `−w × z` to a raised
    one's, where `z` is the shape's standardized mean brightness. A pressed slot therefore takes
    the darker shape of a pair, and a sheet with an extra or shuffled shape cannot hand a raised
    face to a pressed slot.
4. **Scale.** One scale per piece, across its short side, from the cell's size in units.
5. **9-slice insets.** Each inset is measured by comparing **whole cross-sections** with a
    typical one from the middle third, scanning outward:
    - each cross-section has its mean removed first, so a slow gradient does not count;
    - the slice line goes where the sections stop matching, so a bracket or a large chip is
      enclosed in its corner and never stretched with the edge;
    - every inset is at least half the spec's border and at most 40 % of the piece;
    - a thin bar (at most 14 units tall, more than three times as long) gets equal end caps.

### Icons (`iconcut.py`)

- **Whole shapes.** The N largest painted shapes, after a dilation that joins one icon's
  strokes, become the N icons. Use `ICON_DIL` of about 25 for an icon made of separate strokes.
- **Reading order.** Shapes whose vertical centres lie within half an icon's height share a row,
  and a row reads left to right.
- **Why not cells.** Whole shapes do not depend on the painting keeping the sheet's aspect ratio.

### Seamless middles (`seamband.py`)

**Why repeat.** A stretched middle widens every scratch and chip with it: a 9-slice painted 140
units long and drawn 280 shows its wear twice as wide as tall. A middle that repeats keeps the
painting's proportions at any length.

1. **The tile.** The period is learned from the band's own paint by `tile.py`'s quilting (*Fills*),
    so it wraps continuously by construction.
2. **The joins.** The tile is joined to the fixed ends through an overlap `ov` = max(8, band / 12)
    pixels of the band's own paint, along the minimum-error seam (`seam_join`), feathered over 3
    pixels. The ends keep their bevels.
3. **A 9-slice.** The middle gives up `ov` at each side to the joins and is replaced by one
    period. The slice grows by `ov` on each side.
4. **A grid part: compaction.** A grid part's fixed cells keep their painted length when it is
    drawn, so the room its band has to repeat in is the drawn length less everything outside the
    band. A long part with a short band leaves the copies too little room: either they stretch
    by up to half, or the period must be short enough to show as a rhythm. So `seamband.py`
    compacts the part:
    - It widens the band to the wells the band crosses, `GRID_INSET` = 5 units in from their
      ends, clear of a recess's end wall and its shadow.
    - It shortens the part to its two ends and one period. The design and the PNG shorten by
      what the band gave up, `xs` becomes the period, and every well edge right of the band
      moves left with the paint.
    - **The case:** the timeline housing `rail_one` was painted 420 units with a 58-unit band.
      Identical scratches showed every 130 px at HUD 75 and 100. Compacted, it is 338.88 units
      with a 194.75-unit period (MEASURED, `kit.json`).

MEASURED periods in the kit:

| Part | Kind | Painted length | Fixed ends | Period |
|---|---|---|---|---|
| `rail_one` | grid | 338.9 | 144.1 | 194.8 |
| `rail_forks` | grid | 456.0 | 361.9 | 94.1 |
| `live_one` | nine | 270.6 | 38.8 + 34.5 | 197.4 |
| `live_groove` | nine | 307.3 | 73.3 + 73.3 | 160.8 |
| `takeover_button` | nine | 116.6 | 49.1 + 13.7 | 53.8 |
| `lanes_plate` | nine | 110.0 | 16.4 + 14.5 | 79.1 |
| `pick_row` | nine | 101.3 | 15.3 + 13.8 | 72.2 |
| `lane` | nine | 64.4 | 11.0 + 11.0 | 42.4 |

`rail_forks` was given `xtile` before the compaction rule existed and has not been compacted
(*Gaps*).

`fill()` is the operation both cases use, and a job script may call it, or `seam_join`, to join
separately painted pieces: `compose_housing.py`, the hover strip, the picker's title band.

### Fills (`tile.py`)

The timeline fill is a strip that repeats along x as a whole (`kind` `tile`).

1. **The ends go.** Columns not fully painted are dropped, then 4 % more at each end, because a
    painter shades a strip's ends.
2. **The length.** The tile is a slice `seg[0:L]` of the strip. For each L from 55 % of the strip
    up, the strip's continuation `seg[L:L+o]` is compared with its start `seg[0:o]`, over an
    overlap o of 12 % of the strip. The L where they match best wins.
3. **The seam.** The tile's first o columns are rebuilt from those two along the cheapest
    top-to-bottom path through their difference, feathered over 3 px. Nothing is blended across
    the whole overlap, so the wear keeps its contrast.
4. **Why the wrap cannot show.** The tile's last column is `seg[L-1]` and its first is `seg[L]`,
    neighbours in the painting. The only join is the seam inside the tile.

## How a part is drawn

The mockup (`hud.js`, `hud.css`) and the DLL (`tagpu_hudlay_expand` in
`tagpu/ddraw/src/tagpu_hudlay.c`) draw a part by the same rules. The mockup composes images; the
DLL emits textured quads.

- **The box is snapped to whole device pixels,** each edge rounded half up by `tagpu_hud_snap`
  (`tagpu_hudlay.h`), and every edge below — a cell's, a copy's — by the same rule. Every rule
  below works in the snapped box. The DLL snaps a part's box in `tagpu_hudlay_expand`, and a
  solid's, a dot's and a clip's in `tagpu_huddraw.c` (`snap`): the rasteriser's pixel-centre rule
  alone rounds an edge that falls exactly on a half pixel *down*.
    - **An edge exactly on a half rounds up on both sides.** The layout computes in float and
      `hud.js` in double, so such an edge comes out a hair either side of the half. The rule
      counts an edge less than 1/64 px below a half as on it. Of the 16 728 snapped edges in
      laycheck's first twenty-three cases, 389 fall in that margin, most of them halves the float
      arithmetic left a ten-thousandth short (MEASURED 2026-10-23 from the ops laycheck prints).
      What the margin cannot close is an edge whose exact value lies within the arithmetic's
      error of 1/64 px below a half; the nearest in those cases lies about 7 × 10⁻⁴ px from it,
      and both sides round it alike.
    - **The DLL's rounding is one arithmetic wherever it is compiled.** `tagpu_hud_snap` takes a
      float, so every caller rounds its product (and a far edge's sum, `tagpu_hud_edge`) to float
      before the rule, and the rule itself is exact in double. The DLL's x87 code (`-std=c99`
      rounds an excess-precision value at a cast, an assignment and a parameter) and laycheck's
      native SSE build therefore put the same float on the same pixel, and the hit map
      (`tagpu_hudstate.c`) tests a click against those pixels too. hud.js computes in double, the
      one place the margin above is what makes the sides agree.
    - **The mockup pins every box itself** (`pin` in `hud.js`). It computes each box's edges from
      the layout's own numbers, kept on the element as `--x`, `--y`, `--w` and `--h`, and gives the
      element its offset from its parent's pinned corner in CSS pixels. Chrome's layout is never
      read back: it keeps a position in LayoutUnits, sixty-fourths of a CSS pixel summed down the
      element tree, which put an edge on a half a hair below it and painted a bay button's edge a
      pixel left of the DLL's. Its truncation of each offset now leaves a pinned box under a tenth
      of a device pixel off a whole one, which its own pixel snapping takes back out. A part, a
      fill's clip, a solid and a dot are pinned. A text, the stats window (whose drop shadow the
      DLL blurs from the exact box) and the chart's lines keep their exact place. The chart's SVG
      is given a viewport on whole pixels and a view box offset to match, its legend swatches are
      snapped, and their rings are drawn as the DLL draws them, four sides on edges snapped once
      (`ringEdges`). A resource bar's
      fill is a box of its own, as the DLL's is.
  At 1920 × 1080 and replay HUD 100, 1.5 pixels a unit, many edges fall on halves: the lanes
  plate spans 874.5 to 958.5 and the Take over button 928.5 to 958.5. MEASURED 2026-10-23 on the
  build before the snap: the game drew both a row higher than the mockup, the plate's light top
  band from row 874 and its dark bottom band from 957, the mockup from 875 and 958.
- **A grid part** is drawn through the **band map**. Drawn at a length T for a design length D
  with stretch bands of total length S:
    - every stretch band is multiplied by (T − (D − S)) / S;
    - everything outside the bands keeps its painted length;
    - drawn shorter than its fixed bands (T < D − S), the whole part scales uniformly instead;
    - the cells share edges rounded to whole pixels from the box's corner.
- **A 9-slice part** keeps its slice's edges at their size in units, unless the box is smaller
  than two of them together; then all four shrink by one factor f. An edge is drawn a whole
  number of pixels wide, floored, and a thinner one a pixel wide, as CSS snaps a border width. The
  middle stretches.
- **An `xtile` part's x band repeats.** It is drawn as n = max(1, round(drawn band length /
  painted band length)) copies, each taking an equal share of the drawn band, their edges rounded
  to whole pixels. Along y the band still stretches.
    - **For a grid part,** the painted band length is the band's design length.
    - **For a 9-slice,** it is the middle's painted width at its row's edge scale: the top and
      middle rows at the top edge's drawn width over its slice, the bottom row at the bottom
      edge's. This is how CSS `border-image-repeat: round` sizes the tile.
    - In the DLL each copy is one more quad with the band's own texture coordinates.
- **The mockup composes the grid and 9-slice parts and the tiled fills itself,** on a canvas of
  the snapped box's size (`composeGrid`, `composeNine`, `composeTile`), and samples every piece
  as the DLL's GPU does (`paint`): from the part's premultiplied texels and a mip chain made as
  `blit_chain` makes it, through a linear filter between the two levels the piece's larger scale
  falls between (`mk_sampler` in `tagpu_vk_hud.c`). Chrome's own resampling differs: its
  `border-image` drew the Take over button's middle with no mip levels, so the scratches painted
  into it showed as aliased "/" marks the game's minified middle does not have, and its canvas
  `drawImage` gives the same picture at every `imageSmoothingQuality`, sharper than the game's
  along the less-minified axis. A grid or 9-slice part keeps its pieces in `data-quads`, and
  `tools/hudart/laycheck.py` compares them, edge for edge, with the DLL's.
- **The DLL's bound.** The DLL draws a part into at most `HUD_MAXQUAD` = (2 · `HUD_MAXBAND` + 1)²
  = 289 quads, `HUD_MAXBAND` being 8 (`tagpu_hudlay.h`). That is every cell of the largest grid
  the loader accepts. The copies are fitted to what is left once every cell has its quad, so a
  band drawn longer than that holds fewer, longer copies and no cell is ever dropped.
    - **A grid row** gets 289 / rows quads. Its cells take one each, and each band shares the
      rest. `rail_one`, with 5 rows of 3 cells, allows 55 copies.
    - **A 9-slice's middle** allows 1 + 289 / 3 − 3 = 94 copies.
    - The mockup does not cap the copies.
- **A tiled fill** is scaled to its drawn height and repeated along x at a whole-pixel period, one
  piece whose texture coordinates run past 1 through a repeating sampler.
- **An icon** is drawn whole inside its box, keeping its aspect, centred. **A whole part** is
  stretched to its box. The mockup draws both as CSS backgrounds.

**The timeline housing at 3840 × 2160,** with the game's HUD at 150 (a 192-pixel panel).
It is 1216 / s − 310 units long, s being the replay HUD's scale; its fixed cells are 144.13 units
and its period 194.75. Computed from `kit.json` and `layout.json`:

| Replay HUD | Housing | Band drawn | Copies | Each copy |
|---|---|---|---|---|
| 75 | 1311.3 | 1167.2 | 6 | 1.00 × |
| 100 | 906.0 | 761.9 | 4 | 0.98 × |
| 125 | 662.8 | 518.7 | 3 | 0.89 × |
| 150 | 500.7 | 356.5 | 2 | 0.92 × |

At HUD 125 and 75 the game's copies fall on the same pixels as the mockup's (MEASURED
2026-10-15, [The replay HUD in game](hud-in-game.md), *How it is built*). At HUD 150 the timeline
differed in 1.50 % of its pixels, from the world above it and the clock's text edges.

## How sharp the art is

A HUD unit is screen height / 720 × the replay HUD's scale. At 3840 × 2160 that is 2.25, 3, 3.75
and 4.5 device pixels at HUD 75, 100, 125 and 150. At 1920 × 1080 it is half that. The art stays
sharp while it is painted at more pixels per unit than it is drawn at. The DLL draws every part
from a mip chain, so it minifies cleanly.

The table gives painted pixels per unit at each part's drawn size, and in the four right-hand
columns how many times the part is **minified** at 3840 × 2160. MEASURED from `kit.json`, the
PNGs' sizes and the boxes `layout.json` draws them in:

| Part | Painted px per unit | HUD 75 | HUD 100 | HUD 125 | HUD 150 |
|---|---|---|---|---|---|
| Plates, strips and buttons (`scale` 9.4–10.9) | 9.4 – 10.9 | 4.2 – 4.9 | 3.1 – 3.6 | 2.5 – 2.9 | 2.1 – 2.4 |
| Stats window (`win_one`) | 7.0 | 3.1 | 2.3 | 1.9 | 1.56 |
| Timeline fill, at its height of about 9.8 units | 7.9 | 3.5 | 2.6 | 2.1 | 1.75 |
| Icons, at their boxes | 7.5 – 26 | 3.3 – 11.6 | 2.5 – 8.7 | 2.0 – 6.9 | 1.7 – 5.8 |
| Marker badge, 12 units | 5.9 | 2.6 | 2.0 | 1.6 | 1.3 |
| Playhead, along its height of about 15.8 units | 5.2 | 2.3 | 1.7 | 1.4 | 1.15 |

**No part is magnified at any of the four scales.** The least margin is the playhead's, 1.15 at
HUD 150.

A stretch band or a repeat changes this along its own axis only:

- The team header is drawn 25 units tall from a 23.29-unit painting, so its two y bands are drawn
  1.30 × their painted height: 7.7 px a unit there.
- An `xtile` copy is drawn at most 1.5 × its painted length, which happens when n rounds down to
  1; for n ≥ 2 a copy is drawn between 0.75 × and 1.25 ×.

## Recipes

- **Re-cut without repainting:** to change a band, the keying, the matching or a pin. Nothing
  about the paint changes, so this is the cheapest change.
    1. Run `cut_all.py` again on the archived renders, or the single cutter on one painting.
    2. Run `seamband.py` on the parts that repeat.
    3. File with `file.py` and check (*7. Check*).
    4. Start from the cutter's output, never from an already-banded kit: `seamband.py` rewrites
       a part in place.
- **Repaint one part in the look:**
    1. Draw its layout from its spec. Take the recess insets from `layout.json`, and put a piece
       of a finished part on the canvas as the reference.
    2. Render at 1K until it holds the layout, then at 4K.
    3. Look at the 4K render. Prepare and cut it.
    4. Compare the measured wells with the whitebox's, then repeat, file and check.
- **Change a part's layout** (a recess wider, one more recess):
    1. Edit its spec, including `XB` / `YB` if the bands' recesses change, and `layout.json` if a
       well is renamed.
    2. Repaint it as above.
    3. Run `laycheck.py`: the layout places text and buttons through the new wells.
- **A new part:**
    1. Add it to a spec or a sheet.
    2. Paint it with the look's reference on the canvas, cut and file it.
    3. Give it its placement in `layout.json` and its rule in `hud.js` and `tagpu_hudlay.c`
       ([The replay HUD's layout](hud-layout.md), *Iterating*).
- **A part drawn far longer than it was painted:** repaint it at about the length it is drawn
  (*1. The layout*), or as windows if it is longer than 21:9. Then run `seamband.py`.
- **Remove a feature from finished parts** (a mark, a stain): repaint each part from its own
  render, one render per part, with the render as the canvas's reference and the one change named.
  Cut each with the cutter that made it and compare its wells and insets with the kit's.

**After any change:** `laycheck.py` ends "the two agree", a capture at native 4K shows
`missing art 0`, and the DLL is rebuilt so that `inc/tagpu_hudpak.bin` carries the new kit.

## Gaps

- **The icons, the marker badge, the playhead and the timeline fill were not repainted.** These
  31 parts were cut from Codex paintings of the earlier method, by `iconcut.py`, `cut.py` and
  `tile.py`: the icons in khaki stencil, the rest in that method's gunmetal and green. They were
  not repainted in worn painted steel.
- **`rail_forks` keeps a 94-unit period.** It was banded before `seamband.py` learned to compact a
  grid part, and its fixed cells are 361.9 of its 456 units. Re-running `seamband.py` on its cut
  would compact it. It is drawn only by the mockup's proposed section, which the DLL does not
  read.
- **The hover strip is 145.6 units, not 140.** `cut_all.py` keeps its join's 60-pixel overlap on
  top of the 140 units it aims for. The strip is drawn about 137 units long and does not repeat,
  so the 9-slice absorbs the difference.
- **The stats window is filed at 7 px a unit,** under the commit size limit. It is still minified
  1.56 × at 4K and HUD 150, but it has the least margin of the plates.
- **A painting cannot be reproduced.** The renders exist only in `.data/pixel-forge/`. A reroll
  of the same prompt and layout paints different wear, so a lost render means a repaint, not a
  re-cut.
- **The job scripts are not in the repository.** `make_sheets.py`, `cut_all.py`,
  `compose_housing.py`, `unlabel.py` and `level.py` live in `.data/hud-art/work/`. This page and
  `pixel-forge.md` state what they do, but repeating a job needs them or a rewrite.
- **The art reaches the game on Vulkan only.** It travels full colour and soft-edged, packed by
  `tools/hudpak.py` and drawn by an alpha-blended pass at the device's resolution, not through
  `tools/artbake.py`, which refuses any colour outside TA's palette and any partial alpha. The GDI
  renderer draws no HUD ([The replay HUD in game](hud-in-game.md)).
- **The HUD's font is undecided.** The mockup and the game both draw Oswald and Barlow Condensed,
  from the faces in `art/hud/fonts/`.
