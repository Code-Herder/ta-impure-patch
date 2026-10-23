# The replay HUD's layout: one source for the mockup and the game

## Status

**The chosen design is in the repository since 2026-10-07.** The replay HUD whose art
[HUD art](hud-art.md) describes is kept in `art/hud/`: its parts as lossless PNGs, a kit file
with what drawing each part needs, and a layout file holding every size, position, gap and text
style. The mockup in `tools/hudart/mockup/` draws the HUD from those files and nothing else.

- **The DLL draws it in game** on every Vulkan replay, and takes its input there
  ([The replay HUD in game](hud-in-game.md)). It embeds the same files, packed as they are by
  `tools/hudpak.py`, reads the JSON at run time and implements the rules on this page. The
  numbers have one home, so the mockup and the game cannot disagree about one.
- **The game draws it with a pass of our own, not a UI library** (the owner's choice,
  2026-10-08). It is a new alpha-blended Vulkan pass at the device's resolution, with
  [stb_truetype](https://github.com/nothings/stb) (C, public domain) rasterising the fonts.
  Each library considered fails on one of these facts:
    - **The layout.** None of them can express the band map and the wells, and the game must run
      the mockup's rules to stay 1:1 with it, so a library would add only fonts, a draw list and
      button state. Of those, only the fonts are hard.
    - **The shaders.** The build takes shaders only as GLSL in C strings through
      `tools/spirv-gen.py`, and every pass owns its pipelines; a library's Vulkan backend brings
      its own.
    - **The language.** The DLL is C99 with no C++ toolchain, which rules out Dear ImGui and
      RmlUi. Nuklear is C, but would leave only its font baker (stb_truetype itself) and its
      draw list in use.
- **The rules are implemented twice:** in `hud.js` and in `tagpu/ddraw/src/tagpu_hudlay.c`.
  This page states them; `tools/hudart/laycheck.py` compares the two element by element
  (*Checking the C against the mockup*), and a capture of the game is compared with the mockup
  fed the game's own state ([The replay HUD in game](hud-in-game.md), *Checking it against the
  mockup*).
- **The exploration prototype** (the published artifact, with looks A to D, the button sets,
  fills and header variants) stays the record of how the design was chosen. It is not
  maintained.

| What | Where |
|---|---|
| The parts, lossless | `art/hud/parts/<name>.png` |
| What drawing each part needs | `art/hud/kit.json` |
| Every size, position, gap and text style | `art/hud/layout.json` |
| The rules, for the browser | `tools/hudart/mockup/hud.js`, `hud.css` |
| The rules, for the DLL | `tagpu/ddraw/src/tagpu_hudlay.c` |
| The faces, cut down, and their licences | `art/hud/fonts/`, cut by `tools/hudart/fonts.py` |
| The mockup: a synthetic game, the controls, the input | `tools/hudart/mockup/index.html` |
| The rules checked against each other | `tools/hudart/laycheck.py`, `laycheck.c` |
| Filing cut parts into `art/hud/` | `tools/hudart/file.py` |
| The replay frame drawn behind the mockup | `research/notes/assets/hud-art/backdrop.png` |

## Viewing it

The page reads its files over HTTP, so serve the repository root with `tools/hudart/serve.py`:

```
python3 tools/hudart/serve.py            # --bind ADDR --port N; defaults 127.0.0.1 and 8000
# then open http://127.0.0.1:8000/tools/hudart/mockup/
```

It marks every response `Cache-Control: no-cache`. `python3 -m http.server` sends no caching
headers, and a browser then keeps each file for a time of its own: a reload after the kit changes
can draw new parts against the old `kit.json` or `hud.js`, with slices out of place and bands
stretched that should repeat.

The toolbar sets the screen, the game's HUD scale, the replay HUD's scale, the players and the
teams. The same switches, and more, go in the URL, for captures:

- `res=3840x2160`, `game=150` (`off`, `auto` or a percentage), `hud=150`, `np=10`, `nt=2`;
- `t=70` (seconds into the 132-second game), `paused`;
- `stat=1` or `stat=0` (the stats window), `sm=units` (its measure), `ss=team` (players or teams),
  `sx=150&sy=56` (its place). The window opens closed on every visit, as a replay does; the page
  keeps its measure, split and place;
- `scroll=3` (the column's first item), `sight=0` and `follow=0` (a row's Sight and Follow drawn
  pressed), `toast=<text>` (a toast shown);
- `state=<url>`: draw the state the game dumped (`tagpu_replayhud.json`) — its screen, its
  engine frame, its players and its stats window with the game's samples — instead of the
  synthetic game. The synthetic game's chart is 67 made-up samples over its 132 seconds;
- `prop=0` or `prop=1`, `nf=2`, `lo` (the lanes open), `tl=1`, `pick`, `mode=live`, `rw`: the proposed
  features, below.

`tools/hudart/capture.sh "<url>?paused&res=3840x2160&game=150&hud=150" out.png` captures the stage at
native 4K. It lays the page out at device scale factor 1, one CSS pixel to a screen pixel, as the
game draws (*Text* says why). `compare.sh` captures the button states — play held, the foot
plate, the stats window's chips, a row hovered with JUMP held — at device scale factor 2. On the
mockup its query carries `stat=1`, so that the chips it captures are on screen.

## Iterating

- **A size, a position, a gap or a text style:** edit `layout.json` and reload. No code
  changes.

- **A part's art:** repaint it ([HUD art](hud-art.md), *Recipes*), cut it, then file it, which
    replaces the old one:

        $PY tools/hudart/file.py <cut dir> head_one
        $PY tools/hudart/file.py <kit dir> --look h6,c3,c,gm row_one   # from a kit of looks

    A repainted grid-sliced part brings its own measured wells and bands, and the layout places
    everything through them, so its text follows its recesses. A well renamed or removed must be
    renamed in `layout.json` too: the page names any well it cannot find.

- **A new element** needs data, a rule in `hud.js`, and the same rule in `tagpu_hudlay.c`,
  checked by `laycheck.py`. It is the only kind of change that touches code. A new control — a
  new `hot()` call site in C — also needs its term in `tagpu_hudlay_max_hot`, the bound the hit
  map is sized against; the layout's load fails a self-test build that makes more controls than
  the bound ([The replay HUD in game](hud-in-game.md), *The hit map*).

- **After any change,** capture at 3840×2160 with both scales at 150, the owner's setting, and
  look at it. Check `game=auto` too, where the engine's frame is largest and the list scrolls.

## Proposed: forks, take over and live rewind

The owner chose variation B of the replay UI's missing features on 2026-10-12, from a canvas of
four whitebox variations: forks drawn as lanes on the timeline. Its art is the worn painted steel
of the rest of the HUD (`hud-art.md`). The mockup draws it from `layout.json`'s `proposed`
section behind its **Proposed** switch (on by default, `prop=0` turns it off). **The game draws
its replay side** — the Forks bay and the forks housing, the axis, the fill and the shaded groove,
Take over and the player picker, and the fork lanes — from the same rows, always (*Take over and
the picker*, *The lanes*, and [The replay HUD in game](hud-in-game.md), *Take over* and *The
lanes*), and `laycheck.py` checks every case at `prop=1`. Live rewind is the mockup's alone. A part the kit has no art for yet is
drawn as whitebox: a mid-grey plate with a dark edge, and an icon it lacks as a stand-in glyph.

- **The Forks bay.** A bay at the timeline housing's left end, spanning the marker lane and the
  track, shows and hides the lanes; they are hidden whenever a replay starts. It shows the fold
  arrow over the number of forks kept and is dimmed when there are none. With forks, the housing
  is `forks.rail` (`rail_forks`), painted with a pocket for the bay at its left end, and the bay
  sits in that measured well (`forks.bay.in`).
- **Lanes.** Each fork the replay keeps (at most `forks.max`, 3) is a lane on a plate above the
  timeline housing, as wide as the housing. The oldest branch is on top. A lane runs from where
  the fork branched to where it ends, on the track's own time axis. A lane narrower than
  `lane.min_w` widens to the right first. A line drops from the branch to the track — or, for a
  fork of a fork, rises to its parent's lane — and the playhead's line runs up through every lane. The lane's label names the player taken over and
  the branch time. Its right end carries Export and Delete; a first click on Delete arms it,
  and a second deletes the fork.
- **The axis.** A fork can run longer than the replay. While the lanes are open or a fork is
  watched, the track's axis is therefore the longest timeline kept, and the groove past the
  replay's end is shaded (`forks.after`). Closed, the timeline is the replay's own, as today.
- **Watching a fork.** Clicking a lane watches that timeline and presses the lane in. Its played
  part fills to the playhead, the main track stays filled only to where the chain leaves the
  replay, each lane between them on the chain fills to where the next one branches off it, and
  the clock shows the fork's length. Clicking the pressed lane goes back to the replay.
- **The column.** The column stands on the lane plate, because the lanes run under it, so fewer
  rows fit while forks are kept.
- **Take over.** A button as wide as the transport plate sits on top of it, level with the lanes,
  and reads "Take over replay". It opens the **player picker**, which lists every player with
  their colour. A player out of the game — no unit left, the column's dim rule (`out`) — is
  dimmed, marked "defeated" and cannot be taken. In game `out` is the seat's live-unit count at
  the picker's start, the word the takeover itself refuses a seat by, so the picker dims exactly
  the players a takeover would refuse; the mockup's synthetic game sets `out` on its own. With 3
  forks kept, the picker says which fork a take-over replaces, the one watched least recently,
  and outlines that fork's lane.
- **Live rewind.** With `mode=live` the page shows a live game. A rewind button sits in the top
  bar beside the Impure Config sprocket; the sprocket is drawn as a dark stand-in, since the
  game draws it outside the HUD. The button opens a strip under the top bar over the paused game:
  the transport, a track up to NOW, the time, **Play from here** and **Back to now**. The strip
  reuses the bay buttons, the timeline fill and the playhead.

The shell screens of variation B (fork rows in the Replays browser, Delete and Play on there,
Rewind on the end screen, and the Record games and Keep replays rows in Impure Config) are
engine GUI. They are drawn with the game's own widgets, so the mockup does not draw them.

## Units and the screen

**The HUD keeps its own scale and fits the space the engine's frame leaves** (the owner's
rule, 2026-10-08):

- **One HUD unit** is the screen height / 720 × the replay HUD's scale, so 4.5 pixels at 4K and
  150 %. The scale is the replay HUD's own, set by its scale button (75, 100, 125 and 150 %), and
  does not follow the game's HUD scale. Every length in both files is in units, except a
  9-slice's slice, which is in its PNG's pixels.
- **The engine's frame** is where the game's own side panel and bars end: `panel` (the left side
  panel's width), `top` (the top bar's height) and `bottom` (the bottom bar's height). The host
  supplies them, with the screen's width and height, in units. The dock and the column are
  anchored to them, and the parts that stretch (the timeline housing along the dock, the list
  down the column) take whatever space is left.
- **The frame follows the game's HUD scale**, as `tagpu_hud_geom` (`tagpu/ddraw/src/tagpu_hud.c`)
  lays it out: 128 engine pixels of panel and 32 of bar, magnified by q8 / 256 and floored to
  whole pixels. q8 is 256 when the setting is off, and otherwise the setting's percentage × 256 /
  100. It never goes below 256, nor past the ceiling: the lesser of 480 panel rows in the screen's
  height and 256 pixels of view left beside the panel. Auto is the ceiling. At 3840 × 2160 the
  panel is 192 pixels at 150 % and 576 at Auto; the bars 48 and 144.
- **The mockup computes the frame the same way** from its *Game HUD* setting. Its backdrop is a
  1280 × 720 replay frame from our renderer, recut so that its panel and bars are drawn at that
  magnification and its world fills the rest.

## kit.json

One entry per part, by name. Icons are named `i_<icon>`. `scale` is the PNG's pixels per
HUD unit. The kinds:

| Kind | Drawn | Fields |
|---|---|---|
| `grid` | Composed at its drawn size, its stretch bands taking the change | `design` [w, h] in units; `xs`, `ys`: the stretch bands; `wells`: the measured recesses [x, y, w, h]; `text`: the wells that are text zones on the plate |
| `nine` | 9-slice: the slice's edges keep their size in units, the middle stretches | `slice` [top, right, bottom, left] in pixels |
| `tile` | Scaled to its drawn height and repeated along x | |
| `whole` | Stretched to its box | |
| `icon` | Whole inside its box, keeping its aspect, centred | |

A `grid` or `nine` part may carry **`xtile`**: its x stretch band (a grid's one `xs` band, a 9-slice's
middle column) is one period of a seamless tile, painted to join its fixed neighbours, and it **repeats**
along x instead of stretching: drawn n = max(1, round(drawn length / painted length)) times, each copy
taking an equal share of the drawn band, as CSS `border-image-repeat: round` repeats a 9-slice's middle.
Along y it still stretches. A 9-slice's painted length is its middle at **its row's edge scale**, as CSS
sizes a `round` tile: the top row and the middle row at the top edge's drawn width over its slice, the
bottom row at the bottom edge's, so the rows may hold different counts. And the edges are drawn **whole
device pixels** wide, as CSS snaps a border width (floored, a thinner one drawn one pixel): the Take
over button at 4K and HUD 150 has a top edge of 8.81 px drawn 8, which makes its top and middle rows
five copies of 199 px and its bottom row (10.67 px, drawn 10) four of 248 — MEASURED 2026-10-23 against
Chrome's `border-image`, where the exact widths gave four in every row and the scratches fell up to
55 px apart. Every edge — the box's own, a cell's and a copy's — is a whole pixel, rounded half up
by one rule (`tagpu_hud_snap`, `snapPx` in `hud.js`), counted from the snapped box's corner. A part drawn much wider than it was painted keeps its wear in
proportion this way; `tools/hudart/seamband.py` makes the band and sets the flag. The game
(`tagpu_hudlay_expand`) and the mockup (`composeGrid`, `composeNine`, which draw the part on a canvas
of the snapped size) draw it alike, piece for piece, and sample it alike ([HUD art](hud-art.md),
*How a part is drawn*).

A grid part's fixed cells keep their painted length, so the room its band has to repeat in is the drawn
length less everything outside the band. `seamband.py` therefore widens a grid part's band to the wells it
crosses and shortens the part to its two ends and one period: the timeline housing, painted 420 units with a
58-unit band, became 339 units with a 195-unit period, which repeats twice at 0.92 × on a 501-unit housing
(4K, HUD 150) where the short period showed as a rhythm of identical scratches at HUD 75 and 100.

## The rules

### Frames and the placement rule

Every child is placed against its parent's **frame**:

- **A grid-sliced part's frame** is its design box and its wells, mapped to the drawn size by the
  **band map**: the stretch bands share the difference between the design length D and the
  drawn length T, each multiplied by (T − (D − S)) / S, where S is their total length. Drawn
  shorter than its fixed bands (T < D − S), the whole part scales uniformly instead.
- **Any other box's frame** is the box itself.

A **placement** names a box in its frame:

- **`in`**: a well, a list of wells (their union), or `{"x": …, "y": …}` naming each axis
  apart. Without it, the whole frame.
- **`pad`** [left, top, right, bottom]: moves each edge inward, in design units (negative:
  outward). The padded edges are then mapped through the band map.
- **`padf`**: insets the mapped box by that fraction of its width and height.
- **`w`, `h`**: a fixed drawn size, aligned inside the mapped box by `ax` (`l`, `c`, `r`) and
  `ay` (`t`, `c`, `b`), centred by default.

### Parts

- **Every box is pinned to device pixels by `hud.js`** (`pin`), from the layout's own numbers, as
  the DLL snaps it ([HUD art](hud-art.md), *How a part is drawn*).
- **Grid-sliced and 9-slice parts and tiled fills** are composed into one image per snapped drawn size,
  piece by piece, the pieces the DLL's and sampled as its GPU samples them ([HUD art](hud-art.md),
  *How a part is drawn*). The pieces share rounded pixel edges, so no seam can show. An `xtile` part's
  x band is drawn as n copies of the band's source cell, their edges rounded the same way. A hover's
  controls are hidden by `visibility`, not `display`, so they are laid out and composed before they
  show.

### Text

A text is one line in its box, centred vertically and clipped at the box's edge. Its **style**
(`styles` in `layout.json`) gives:

- `font`, `weight`, and `size` in units;
- `colour`, where `"resource"` means the colour of the line's resource (`resource`: metal, energy);
- `upper`, and `spacing` in em;
- `shadow`: a drop of that many units in black at 80 %; or `outline` [px, py, nx, ny]: four black
  copies, at (px, py), (nx, ny), (px, ny) and (nx, py).

The **slot** that places the text adds `align: "c"` to centre it, and `fit`: the size becomes
the lesser of the style's size and the box's height / `fit`. That keeps the digits of a resource
line clear of its bar when a row is drawn shorter than it was painted.

- **Signed pairs**, the income and spending, read "+a / −b" in the `signs` colours and weight.
- **A row's rates** read one decimal below 100 in magnitude and whole numbers from 100, rounded
  half up as JavaScript's `Math.round` rounds (`rate` in `hud.js`; `rate` and `js_round` in
  `tagpu_hudlay.c`), since the game's incomes are fractional.
- **Numbers** use a thousands separator. A team header's income sums switch to thousands with a
  K from 10 000 on. The clock reads m:ss.
- **A team header's unit sum is the bare number.** The faces carry no Greek, so there is no
  sigma before it.
- **A resource bar whose storage is 0 is empty.**

**The faces** are Oswald 500 and 700 and Barlow Condensed 600, in `art/hud/fonts/`, which the
mockup loads with `@font-face` and the DLL embeds. `tools/hudart/fonts.py` cuts them from the
Google Fonts releases to ASCII, Latin-1 without the soft hyphen, and `·`, `×` and `−`, and stops,
naming the character, when a source face lacks one. Neither family has Greek. The DLL draws a
player's name as Latin-1 with its control codes as `?` and its soft hyphen as `-`, so nothing
reaches the rasteriser that the faces lack.

**The baseline is Chrome's** (`tagpu_hudlay_baseline`): the line box, the font size tall at
line-height 1, is centred in the text's box; the face's ascent and descent (OS/2's typo metrics
when the face sets `USE_TYPO_METRICS`, as these do, else hhea) are each rounded to a whole pixel
at the font size; the half-leading, here negative, is floored to a pixel; the baseline is the
line's top plus the ascent plus that. Blink rounds these metrics in CSS pixels, which is why
`capture.sh` lays the mockup out at device scale factor 1: at a factor of 2 the baselines fell on
a two-pixel grid the game does not have. In the game the baseline is then painted on a whole
pixel and the pen keeps its fraction, in quarters of a pixel in the atlas, as Skia draws
horizontal text.

**One residual, MEASURED 2026-10-08.** A sweep of Chrome's rounded metrics (canvas
`measureText`'s `fontBoundingBoxAscent` and `fontBoundingBoxDescent`) over 1 to 60 px in 1/64
steps matches round(em × size) everywhere for Barlow Condensed, and at all but 26 of 7554 Oswald
values. There Chrome rounds down an ascent or descent that lies just past a half, by an
arithmetic not identified: float32, FreeType 26.6 and 16.16 models were all tried and fit worse.
At those sizes the game's line sits a pixel off the mockup's.

**The glyphs are unhinted on both sides.** The DLL's rasteriser (stb_truetype) has no hinter, and
the mockup draws its text with `text-rendering: geometricPrecision` (`hud.css`). Hinted, Chrome on
Linux snaps a glyph's horizontal edges to whole pixels, which grows Oswald's cap height by up to
1.3 px over its 0.81 em (43 px at 52 px against 42.12; MEASURED 2026-10-23, a T from 20 to 59.75
px), so "TAKE OVER REPLAY" at 4K and HUD 150 stood 43 rows tall in the mockup and 42 in the game.
Unhinted, Chrome's T is within a fifth of a pixel of 0.81 em.

**The mockup keeps the line where the rule puts it.** With the glyphs unhinted Blink positions them
by subpixel, and then lays the line out on other metrics: a descent that rounds down takes a pixel
from the ascent, which puts the baseline a pixel higher at about half the sizes (MEASURED
2026-10-23: 190 of 384 sizes from 4 to 60 px, all three faces, one CSS pixel each; a canvas's
`textRendering` reports the two sets of metrics). That is further from the face's own metrics
than the rule: against the unrounded baseline, the rule is off by 0.51 px on average over 4..60 px
for Oswald and the borrowed metrics by 1.00. So the mockup, not the rule, moves: `lineShift` in
`hud.js` takes the baseline offset A + floor((s − A − D) / 2) on the default metrics less the same
on the unhinted ones, both read from a canvas, and pads the text box by twice that on one side,
which moves the centred line by it and leaves the box and its clip where they were. laycheck's
baselines are then the ones the hinted mockup gave, all 22 cases agreeing, and the game's text
placement is unchanged. In the game-against-mockup capture at 4K and HUD 150, fed the game's own
dump, "TAKE OVER REPLAY", the lane labels and "PLAYER" span the same rows in the mockup as in the
game (42, 23 and 35), and the share of TAKE OVER REPLAY's pixels differing by more than 48 fell
from 3.79 % (best at a one-pixel shift) to 1.25 % (at none).

### Buttons and controls

- A button draws its `part`. While held, and while on for a toggle, it draws its `pressed` part
  instead.
- The content on a pressed face sinks by `press_shift` units.
- **A control is its `act`.** Every enabled button, and every plate that takes a click, is a
  clickable op (`HOP_HOT` in the DLL, `data-act` in the mockup) keyed by its `act`; a row's, a
  team's and a hover button's key adds the row's or the team's index in the frame, `jump:2`,
  `fold:0`, `row:4`, and a lane's and its buttons' the lane's (`lane:1`, `delete:1`; the mockup's
  `data-fork`). The game hit-tests these, topmost (last painted) first
  ([The replay HUD in game](hud-in-game.md), *Input*).
- **A disabled control** is drawn at its `off` opacity as one layer, its part, icon, stripe and
  text composited first and the whole faded once, as CSS `opacity` on its element does; it has no
  clickable of its own: the TEAMS chip when the teams are not told apart (one team, or a free-for-all), at
  `split.off` (0.45).
- **An inset edge** — a resource bar's, the toast's, the replace outline, a dark legend swatch's —
  is a ring of four solids on both sides (`ring` in `tagpu_hudlay.c` and `hud.js`): the top and
  bottom across the box, the sides between them, each snapped to whole pixels on its own. laycheck
  pairs them as solids.

### The dock

- **Both pieces** are `dock.h` tall, standing on the bottom bar. Each is a clickable `dock.act`
  (`dock`) under its buttons, so a click between them is the HUD's.
- **The transport plate** starts `transport.l` right of the side panel and is `transport.w`
  wide. Each button fills its well, padded by `transport.button.pad`. The play button shows
  `playing`'s icon while the replay plays.
- **The timeline housing** runs from `rail.gap` right of the plate to `rail.r` from the screen's
  right. Its part is `proposed.forks.rail`, the housing painted with the Forks bay's pocket.
    - **The track** is its `track` well, and the clickable `track`. Its axis T is the state's:
      the original's length, or the longest live chain's while the lanes are open or a fork is
      watched. The fill is inset by `fill.pad` and uncovered up to min(t, `fillTo`) / T of its
      width, where `fillTo` is where the watched chain leaves the original. When the original
      ends before the axis does (`ends` < T), the groove from `ends` to the fill box's end is
      covered by a solid of `proposed.forks.after`'s colour and alpha (hud.js `after`), after the
      fill and under the playhead. The playhead is
      `playhead.w` wide, centred at `ends` + (w − 2 × `ends`) × p, and extended above and below
      by its pad. The playhead's run, from p = 0 to p = 1, is handed out with the ops
      (`track_x0`, `track_x1` in `HUDOPS`) for the timeline's seek.
    - **The markers' lane** takes its x from the `track` well and its y from the `markers` well,
      `markers.h` tall. Each badge is h × h, centred at `ends` + (w − 2 × `ends`) × t / T.
      Its dot is inset by `dot` and drawn in the player's colour, or `bookmark_dot` for a
      bookmark, under the badge part, then the icon inset by `icon`.
    - **The clock** is in the `time` well: the playhead's time and `len`, the watched chain's
      end.
    - **The Forks bay** (`proposed.forks.bay`) is placed in the housing's frame like a well's
      control (`in`, `pad`), after the clock: a button keyed `lanes` with the fold icon, open
      while the lanes are open and a fork is kept, `off` at `bay.off` with none, and the count of
      forks under it, centred from `count.y` of its height down, outside the button.
- **Take over** (`proposed.forks.takeover`) is a button as wide as the transport plate,
  `takeover.gap` above it and `takeover.h` tall, keyed `takeover`, its label centred; it is down
  while the picker is asked for, sought to or open (`pick_down`; the mockup's while `picker`).

### The column

- **Its box** is `w` wide, `r` from the screen's right, and starts `t` below the top bar. It
  ends `dock_gap` above the dock.
- **Its items:** for each team, a header when the teams are told apart, then the team's players
  unless the team is folded. The teams are told apart (`headed`, in `hud.js` and
  `tagpu_hudlay.c`) when there is more than one and at least one has two players: a
  free-for-all, every player alone, has no team header at all, while a 2v1v1 gives each lone
  player a header of its own. A team without a header cannot be folded, so its rows always
  show. A header is `header.h` tall, with `header_gap` above every header but the first; a row is
  `row.h` tall; `gap` separates items.
- **It scrolls by whole items.** `last` is the greatest first item from which every item to the
  end still fits in its height less `foot.gap` and `foot.h`; the state's `scroll` is held to
  [0, `last`] and is the first item shown. Both go out with the ops (`first`, `last` in
  `HUDOPS`; `data-first`, `data-last` on the mockup's column) for the scroll's input.
- **It shows the items that fit,** from that first, and always at least one. When some are left
  out, a scroll bar is drawn left of the column:
    - the buttons at `button.x`, the clickables `button.acts` (`scroll_up`, `scroll_down`);
    - the track between `track.ends` from each end, the clickable `track.act` (`scroll_track`);
    - the thumb inside the track, inset by `thumb.inset`. Its length is the travel × shown / all,
      at least `thumb.min`; it sits at `first` / `last` of the travel left.
- **The whole column is a clickable**, `column.act` (`column`), from its top to the foot plate's
  bottom, under everything in it: a click on a row's body is the HUD's, and the wheel over it
  scrolls it.
- **The foot plate** sits right-aligned, `foot.gap` below the list: five bays, `foot.w` × `foot.h`
  (204 × 35). Their buttons are `stats`, which opens and closes the stats window, then the four
  switches `cursors`, `clicks`, `selections` and `chat`; each of the five is a toggle drawn
  pressed while on. A `toggle` name the DLL does not know stops the build (`layout: no toggle
  '…'`).

### The header, the row and the hover strip

- **The header** is the fold button, the whole of it, the clickable `fold:t`:
    - the fold icon, open or closed;
    - the title: the team, and its commanders standing out of all;
    - the unit sum;
    - for each resource line, the letter in the resource's colour and the team's income and
      spending.
- **The row** is a clickable of its own, `row:i`, under its hover strip's buttons; hovering it
  is what shows them. It draws:
    - the stripe, in the player's colour, with an inner shade of `stripe.shade` units;
    - the name, padded by `lost_pad` when the commander is lost, with the skull;
    - the units;
    - for each resource line: the letter and the stored / storage value, both fitted; the bar,
      the stored fraction in the resource's colour on `bar.bg` with an `edge`; and the income
      and spending.

    A defeated player's row is drawn through the `dim` filter: brightness, then saturation.

- **The hover strip** covers the union of the row's four resource wells, padded outward. Its
  buttons, `jump:i`, `sight:i` and `follow:i`, share its width, with `margin` around them and
  between them. **Sight and Follow are drawn pressed on the row they are set to** (the state's
  `sight`, `follow`; -1 for none), and a click on the pressed one undoes it.

### Take over and the picker

- **The picker** (`proposed.picker`, hud.js `picker`) is built after the stats window and before
  the toast. Its plate, `picker.w` wide and keyed `picker`, stands `picker.gap` above the Take
  over button, from the transport plate's left: `pad`, the title `title.h`, a row of `row.h` and
  `row.gap` for each player, the warning when shown, then `pad` again.
- **The title** reads "Take over at m:ss as", with no-break spaces round the time, inset by
  `title.pad` in `title.style`. The time is the playhead's, `t`, in game as in the mockup: the
  world a takeover starts from is the one on screen.
- **A row** is the part `row.part` (`row.pressed` while held), keyed `takeas:<i>` with i the
  row's index, as a column row's is. In it, the player's colour as a stripe `stripe.w` wide inset
  by `stripe.pad` from the row's left, top and bottom; the name inset by `row.name.pad`; and for
  a player out of the game the row drawn at `row.off` with no key and "defeated" aligned to the
  right inside `row.note.pad`.
- **The warning**, while the file keeps the fork limit (`nfork >= fork_max`; the mockup's
  `forks.max`): two lines `warn.line` tall inside `warn.pad`, in `warn.style` and `warn.colour`,
  "N of N forks kept: this one" and "replaces F<id>, the one watched least recently", naming
  the fork a takeover would delete (`replace`), or the first.

### The lanes

Drawn while the Forks bay is open and a fork is kept (`lanes_open`, `nfork`), after the bay and
before Take over (hud.js `lanes`, `tagpu_hudlay.c` `lanes`). X(t) is the track's own axis: the
track's x plus `ends` plus (w − 2 × `ends`) × t / T.

- **The plate** (`forks.plate.part`, keyed `dock`, taken and doing nothing as the dock's plates
  do) spans the housing's width, its bottom `plate.gap` above the dock; it is `plate.pad`'s top,
  a lane `lane.h` tall for each fork with `lane.gap` between, and `plate.pad`'s bottom. The
  column's bottom rises by that height and the gap (hud.js `lanesH`), so the column stands on it.
- **A lane** for fork k, in the state's `forks` order, oldest branch on top: from X(`from`) to
  max(X(`end`), X(`from`) + `lane.min_w`); one past the track's right end is moved left to end
  there, but never left of the track's start. Its part is `lane.part`, `lane.pressed` when it is
  the watched one (`timeline`) or held, keyed `lane:k`.
- **The fill** (`lane.fill.part`, inset by `lane.fill.pad`, from the branch) is drawn on a lane
  of the chain that plays: the watched lane's uncovered to min(X(t), X(`end`)); a lane before it
  on the chain (the packet's `TAGPU_PK_FORK_CHAIN`, the mockup's `chain`) to min(X(t), X(`from`
  of the fork on the chain whose parent it is)). This is the track's `fillTo` rule applied to the
  lanes, so the chain's path has no gap; in the mockup `fillTo` is the branch of the chain's first
  fork, the one whose parent is the replay.
- **A fill that has begun shows**: the track's fill and a lane's, uncovered by any positive width,
  span at least one device pixel (`tagpu_hudlay_clip`; hud.js `UF`, a CSS `max()` with one device
  pixel). Without the floor a chain fill 0.44 px wide drew nothing in the game, which clips by pixel
  centre, and one pixel in the browser, which snaps the box's edges.
- **The label** `F<id> · NAME from m:ss` in `lane.label.style`, from `lane.label.pad`'s left of
  the branch to the buttons less the same pad.
- **The buttons**, `lane.buttons.items` in order at the lane's right end, `buttons.w` wide,
  `buttons.gap` apart and `buttons.pad` in from the right, top and bottom: the item's icon in
  `buttons.part`, keyed `<act>:k` (`export:k`, `delete:k`); Delete is drawn pressed while armed
  (`armed`).
- **The replace outline**: on the lane a takeover would replace (`replace`, while the picker is
  open), a ring `replace.width` wide outside the lane's box in `replace.colour` (an inset edge on a box
  grown by the width).
- **The branch lines**, after every lane, `branch.w` wide in `branch.colour`, centred at the
  branch: for a fork whose `parent` is the id of a lane drawn above it, from its lane's top up to
  that lane's bottom, its x held to that lane's span; for every other fork, from its lane's bottom
  down to the track's top.
- **The playhead's line**, last, `playhead.w` wide in `playhead.colour`, centred at X(t), from the
  plate's top to the track's top.

### The toast

A short message (`toast` in the state) in a dark box centred across the screen, `toast.top` of
its height down, above everything: a `toast.w` × `toast.h` box (150 × 18) in `toast.bg` at
`toast.alpha`, an edge of `toast.edge` in `toast.edge_colour`, the text in the `toast` style,
centred. The box is the layout's size, not the text's, since the DLL lays out without the faces'
metrics. It shows for `toast.ms` (1800).

### The stats window

- **Its box** is `w` × `h` at (`x`, `y`) until the first drag, then where it was dragged, held
  to [0, screen − `w`] and [0, screen − `h`], with a drop shadow [dy, blur, opacity] — a CSS outer
  box shadow (`HOPF_DROP` in the DLL). The whole box is the clickable `window.act` (`window`),
  under everything in it.
- **The title well** holds the grip and the title, `Game stats · <measure> <unit>`, and is the
  clickable `drag`, the drag handle. The close button fills its well, padded, the clickable
  `close`.
- **The measures** are `window.measures`, each a key `k`, a tray `g` (`eco` or `army`), a name
  `n` and a unit `u`: metal income, energy income and wasted metal in the economy tray; units,
  army value, kills and losses in the army tray. A measure the state names that the list lacks
  reads as the first.
- **The trays:** the label, then a chip per measure from `chip.start` in, each as wide as `chips`
  gives, `chip.gap` apart, `chip.h` tall and centred in the tray, the clickable `chip:<k>`, drawn
  pressed for the measure shown. The economy tray also holds the players / teams chips,
  `chip:player` and `chip:team`, flush right, `split.end` from its right.
- **The split:** a line per player, or, when the split asks for teams and the teams are told
  apart (`headed`, as for the column's headers), a line per team, `Team N`, the sum of its
  players, in the `chart.teams` colours. When they are not told apart the TEAMS chip is disabled
  and the lines are the players'.
- **The chart** covers the chart and legend wells, padded by 1. Its legend column is the legend
  well's right edge minus the chart well's right edge, plus 1.
- **The caption** is in the `foot` well.

### The chart

Every number of it is in `chart`, and its text in the style `chart.style` (`styles.chart`):

- the plot's margins;
- the value and minute labels: size, colour, gap, baseline, how far below; `axis.every`, the
  minute steps to choose from, and `axis.space`, the least spacing in units;
- the grid lines;
- the playhead's line and label;
- the lines: width, the opacity of the part after the playhead, the halo drawn under a dark
  player colour, the cross where a commander died;
- `teams`, the colours of the team lines;
- the legend's row height, swatch and text placement.

The rules:

- **Sample s is at t0 + s · dt seconds** (the state's `stats.t0` and `stats.dt`), on the x of
  that time over the recording, 0 to T.
- **Each group is one line**, bright up to the playhead and faint after it. The faint part is
  one SVG group at `line.faint`, so where lines cross the top one shows, not the two summed. A
  dark colour's halo is a polyline `line.width` + 1 wide under it; both join round. The cross is
  at the sample nearest the group's death.
- **The minute labels step by the first of `axis.every` that puts them `axis.space` apart**
  (in units of plot width), else by its last. A minute label nearer the playhead's than
  `axis.space` is left out, so the two never overlap.
- **The value labels** are 0, half and the top: the largest sample rounded up to 1, 2, 2.5 or 5
  times a power of ten (`nice`), written by `statFormat`: metal income in tenths below 100,
  thousands as `k` from 10 000 (whole from 100 000), whole numbers otherwise.
- **The legend is ranked by the values at the playhead**, the sample nearest it.

## Checked against the prototype

**Measured on 2026-10-07:** both pages captured at 3840 × 2160 and HUD 150, with 10 players in
2 teams, the stats window open and the replay paused at 1:10.

- **The geometry matches.** Of ten regions, eight line up best unshifted. The timeline housing
  lines up best 1 pixel over, from its width (below). One player row does too, by a margin
  inside the compression noise: a mean difference of 11.5 levels shifted, against 12.4 unshifted.
- **The button states match:** play held, stats on, the selected chips, a row hovered with
  JUMP held.
- **The art differs only by compression.** The prototype drew pieces over 60 KB from WebP at
  quality 92; `art/hud/` is lossless. That makes a mean difference of up to 12 levels on bright
  distressed steel, and none in geometry.

**Six differences, each deliberate:**

1. **The foot plate sits 10 units higher.** The prototype sized its list with a 3-unit gap
   while drawing it with 2; one `gap` now does both.
2. **The scroll thumb** travels inside its track, inset by 1 at each end.
3. **The timeline housing's width** is no longer rounded to half a unit.
4. **The skull** is drawn in a fixed 11 × 10 box. The prototype's box came out 11 × 9.8, through
   the row's stretch bands.
5. **A held button's icon or label sinks with its face**, as a toggle's already did.
6. **The close button** shows its pressed part while held.

## Checking the C against the mockup

`tools/hudart/laycheck.py` opens the mockup headless at its default `prop=1` with two forks
kept, the game's look, for twenty-three cases (4K at 150 with two
teams, 4K at Auto with the list too long, 1080p at 100 with one team, 1440p at 75 with a row
hovered and a button held, 4K at 150 as a six-player free-for-all, 4K at 150 as a 2v1v1, 4K at
150 with fourteen players scrolled to item 3 and a toast, 1440p at 75 with the first row hovered
and Sight and Follow on it, and five with the stats window open: `4k-150-stats-units`,
`1440-75-stats-teams-metal`, `1080-100-stats-ffa-army-held` (a free-for-all asking for teams,
close held), `4k-auto-stats-16-excess-start` (sixteen players at 0:00) and
`4k-150-stats-clamped-energy` (placed past the screen's right and top edges), and five of the
proposed features: `4k-150-prop-no-forks` (the bay off, its count 0), `1440-75-picker`,
`4k-150-picker-full` (three forks: the warning), `1080-100-watched-fork-closed` (a fork watched
with the lanes closed: the axis takes in the forks, the fill stops at the fork's branch, the
groove past the original is shaded) and `4k-150-picker-held` (Take over held), and four with the
lanes open on the seed's three forks, the third by branch point a fork of the first:
`4k-150-lanes` (its line rising to the first's lane), `1440-75-lanes-watched` (that fork watched:
the chain filled along the first lane to its branch), `1080-100-lanes-picker` (the replace
outline) and `4k-150-lanes-held` (a lane held), and `1080-100-lanes-dpr1`, the lanes at the
game's own 1.5 pixels a unit with the browser at one device pixel to a CSS pixel, where the boxes
fall on half pixels), takes the mockup's state and
every element's box in HUD units, runs `tagpu_hudlay.c` compiled natively (`laycheck.c`) on the
same state, and pairs the two element by element: every part by name and painted opacity (its own
times its ancestors', as a disabled button's icon is dimmed), every text by its string and painted
opacity, every
solid by its colour, an inset edge's four among them, every clickable by its key. From the
chart's SVG it pairs the line ops by colour (with width and opacity), the SVG texts by string
(with anchor, size and colour), the two chart ops by opacity (with their clip) and the drop
shadow (offset, blur, opacity), and compares every group's colour, halo, points and cross, a
point within a tenth of a unit, the precision both sides write. It also measures every text's baseline in the browser — a zero-size
inline-block probe on the text's own line — against `tagpu_hudlay_baseline` from the faces'
metrics. It compares every part's, solid's, dot's and clip's device pixels three ways: the box the
DLL's draw snaps (`laycheck.c` prints it), the box `hud.js` pinned, and the box Chrome painted.
And it compares every grid and 9-slice part's pieces — the destination edges in device pixels from
the snapped corner, the source rectangle — with `tagpu_hudlay_expand`'s. A box is compared in
units by the numbers `hud.js` pinned it from, a text's baseline as Chrome lays it out. It exits 1
on a box or baseline more than its tolerance off, a part's or a text's opacity more than one level
off, a dim that disagrees, a box snapped or painted on other pixels, pieces that disagree, a box
the mockup did not pin, or an element only one side has.

MEASURED 2026-10-08 on the first six cases, again on all eight once the controls were laid out
for input, on all thirteen with the stats window (2026-10-09), on all eighteen at `prop=1`
(2026-10-23), on all twenty-two with the lanes (2026-10-23), and on all twenty-three with every
box's device pixels and every grid and 9-slice part's pieces (2026-10-23): the two agree, and across
the cases the 1615 parts, 974 solids, 173 dots and 25 clips are each on the same pixels in the
DLL, pinned and painted. A halo
polyline is told from its line by its `stroke-opacity`, which only the halo carries. At 4K every
box is within 0.018 units and every baseline within 0.0145 (the twenty-two cases, 2026-10-23). The Oswald residual above is named, not failed, and only where the same rule on
Chrome's own rounded ascent and descent (`measureText`'s `fontBoundingBoxAscent`/`Descent`) gives
the browser's baseline, so a wrong line rule still fails: in the 1080p and 1440p cases the `name`
style at 7.125 CSS px moves by a pixel (6 and 8 texts), and the rule on Chrome's metrics lands on
the browser's baseline for every one of them.

## Gaps

- **The HUD font is not decided.** Oswald and Barlow Condensed (both OFL) are the mockup's, and
  ship with the OFL texts beside them.
