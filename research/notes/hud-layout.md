# The replay HUD's layout: one source for the mockup and the game

## Status

**The chosen design is in the repository since 2026-10-07.** The replay HUD whose art
[HUD art](hud-art.md) describes is kept in `art/hud/`: its parts as lossless PNGs, a kit file
with what drawing each part needs, and a layout file holding every size, position, gap and text
style. The mockup in `tools/hudart/mockup/` draws the HUD from those files and nothing else.

- **The DLL does not draw it yet** ([the plan](hud-in-game.md)). When it does, it is to bake the same files at build time,
  as `tools/artbake.py` bakes `art/ui/`, and implement the rules on this page. The numbers then
  have one home, so the mockup and the game cannot disagree about one.
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
- **The rules are implemented twice:** in `hud.js` now, in C later. This page is what keeps
  them in step, with a capture of each compared (*Gaps*).
- **The exploration prototype** (the published artifact, with looks A to D, the button sets,
  fills and header variants) stays the record of how the design was chosen. It is not
  maintained.

| What | Where |
|---|---|
| The parts, lossless | `art/hud/parts/<name>.png` |
| What drawing each part needs | `art/hud/kit.json` |
| Every size, position, gap and text style | `art/hud/layout.json` |
| The rules, for the browser | `tools/hudart/mockup/hud.js`, `hud.css` |
| The mockup: a synthetic game, the controls, the input | `tools/hudart/mockup/index.html` |
| Filing cut parts into `art/hud/` | `tools/hudart/file.py` |
| The replay frame drawn behind the mockup | `research/notes/assets/hud-art/backdrop.png` |

## Viewing it

The page reads its files over HTTP, so serve the repository root:

```
python3 -m http.server 8000 -d "$(git rev-parse --show-toplevel)"
# then open http://127.0.0.1:8000/tools/hudart/mockup/
```

The toolbar sets the screen, the game's HUD scale, the replay HUD's scale, the players and the
teams. The same switches, and more, go in the URL, for captures:

- `res=3840x2160`, `game=150` (`off`, `auto` or a percentage), `hud=150`, `np=10`, `nt=2`;
- `t=70` (seconds into the 132-second game), `paused`;
- `stat=1` or `stat=0` (the stats window), `sm=units` (its measure), `ss=team` (players or teams).

`tools/hudart/capture.sh "<url>?paused&res=3840x2160&game=150&hud=150" out.png` captures the stage at
native 4K. `compare.sh` captures the button states: play held, stats on, the chips, a row
hovered with JUMP held.

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

- **A new element** needs data, a rule in `hud.js`, and later the same rule in the DLL. It is
  the only kind of change that touches code.

- **After any change,** capture at 3840×2160 with both scales at 150, the owner's setting, and
  look at it. Check `game=auto` too, where the engine's frame is largest and the list scrolls.

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

- **Grid-sliced parts** are composed into one image per drawn size, cell by cell. The cells share
  rounded pixel edges, so no seam can show.
- **A tiled fill** is scaled to its drawn height inside a strip of three copies, so its edges
  resample against their neighbours. It is then copied at whole-pixel steps.

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
- **Numbers** use a thousands separator. A team header's income sums switch to thousands with a
  K from 10 000 on. The clock reads m:ss.

### Buttons

- A button draws its `part`. While held, and while on for a toggle, it draws its `pressed` part
  instead.
- The content on a pressed face sinks by `press_shift` units.
- A disabled chip (TEAMS with one team) is drawn at the `split.off` opacity and takes no input.

### The dock

- **Both pieces** are `dock.h` tall, standing on the bottom bar.
- **The transport plate** starts `transport.l` right of the side panel and is `transport.w`
  wide. Each button fills its well, padded by `transport.button.pad`. The play button shows
  `playing`'s icon while the replay plays.
- **The timeline housing** runs from `rail.gap` right of the plate to `rail.r` from the screen's
  right.
    - **The track** is its `track` well. The fill is inset by `fill.pad` and uncovered up to
      the played fraction p of its width. The playhead is `playhead.w` wide, centred at
      `ends` + (w − 2 × `ends`) × p, and extended above and below by its pad.
    - **The markers' lane** takes its x from the `track` well and its y from the `markers` well,
      `markers.h` tall. Each badge is h × h, centred at `ends` + (w − 2 × `ends`) × t / T.
      Its dot is inset by `dot` and drawn in the player's colour, or `bookmark_dot` for a
      bookmark, under the badge part, then the icon inset by `icon`.
    - **The clock** is in the `time` well.

### The column

- **Its box** is `w` wide, `r` from the screen's right, and starts `t` below the top bar. It
  ends `dock_gap` above the dock.
- **Its items:** for each team, a header when there is more than one team, then the team's
  players unless the team is folded. A header is `header.h` tall, with `header_gap` above every
  header but the first; a row is `row.h` tall; `gap` separates items.
- **It shows the items that fit,** from the first, in its height less `foot.gap` and `foot.h`,
  and always at least one. When some are left out, a scroll bar is drawn left of the column:
    - the buttons at `button.x`;
    - the track between `track.ends` from each end;
    - the thumb inside the track, inset by `thumb.inset`. Its length is the travel × shown / all,
      at least `thumb.min`; it sits at the top, since the list shows its first items.
- **The foot plate** (stats, map, capture) sits right-aligned, `foot.gap` below the list.

### The header, the row and the hover strip

- **The header** is the fold button, the whole of it:
    - the fold icon, open or closed;
    - the title: the team, and its commanders standing out of all;
    - the unit sum;
    - for each resource line, the letter in the resource's colour and the team's income and
      spending.
- **The row:**
    - the stripe, in the player's colour, with an inner shade of `stripe.shade` units;
    - the name, padded by `lost_pad` when the commander is lost, with the skull;
    - the units;
    - for each resource line: the letter and the stored / storage value, both fitted; the bar,
      the stored fraction in the resource's colour on `bar.bg` with an `edge`; and the income
      and spending.

    A defeated player's row is drawn through the `dim` filter: brightness, then saturation.

- **The hover strip** covers the union of the row's four resource wells, padded outward. Its
  buttons share its width, with `margin` around them and between them.

### The stats window

- **Its box** is `w` × `h` at (`x`, `y`), kept on the screen, with a drop shadow [dy, blur,
  opacity].
- **The title well** holds the grip and the title, and is the drag handle. The close button
  fills its well, padded.
- **The trays:** the label, then a chip per measure from `chip.start` in, each as wide as `chips`
  gives, `chip.gap` apart, `chip.h` tall and centred in the tray. The economy tray also holds
  the players / teams chips, flush right, `split.end` from its right.
- **The chart** covers the chart and legend wells, padded by 1. Its legend column is the legend
  well's right edge minus the chart well's right edge, plus 1.
- **The caption** is in the `foot` well.

### The chart

Every number of it is in `chart`:

- the plot's margins;
- the value and minute labels: size, colour, gap, baseline, how far below, how often;
- the grid lines;
- the playhead's line and label;
- the lines: width, the opacity of the part after the playhead, the halo drawn under a dark
  player colour, the cross where a commander died;
- the legend's row height, swatch and text placement.

Each player (or team) is one line, bright up to the playhead and faint after it. The legend is
ranked by the values at the playhead.

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

## Gaps

- **The DLL draws none of it.** The pass needs the decision named in
  [HUD art](hud-art.md), *Gaps*: a full-colour, alpha-blended draw at the device's resolution.
  When it lands:
    - bake `art/hud/` at build time;
    - implement these rules;
    - take the frame's `panel`, `top` and `bottom` from the engine;
    - compare its capture with the mockup's at the same screen and state.
- **The fonts,** Oswald and Barlow Condensed (both OFL), are loaded from Google Fonts by the
  mockup. The game needs them in the release, cut down to the characters the HUD draws: four
  faces, Oswald 500 and 700 and Barlow Condensed 600 and 700, come to 51 KB zipped for ASCII and
  74 KB with Latin-1 (MEASURED, 2026-10-08). The OFL then asks for each face's copyright notice
  and the licence text beside them. The HUD font itself is not decided.
- **The column does not scroll.** It shows the items that fit from the first, as the prototype
  did.
- **The data is synthetic.** A real stats window needs a per-second stats track in the recorder
  ([S5](tadr-port/demo-recorder-s5-ui.md)).
