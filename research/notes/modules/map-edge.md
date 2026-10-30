# The mirrored map edge

Mirroring the map past its edge.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.90 The mirrored map edge (`tagpu_terr.c`, `tagpu_feat.c`, `tagpu_packet_pub.c`; `edge`, `tagpu_mirror.on/.off`) — G20b

**What it is.** Past the map the view shows the world reflected, in the lab's tone, instead of
black: C5 of [BAR camera & full-colour Classic](../bar-camera-port.html), the lab's `edge=mirror`
(`tools/tascene-view.html`) ported to the two world passes that draw ground. The centre clamp
(G20a) is what brings it into view — up to W/2 past an edge at 1×, and at a corner three
quarters of the view at every zoom. The map ends where stock's view ends, at the engine's scroll
extent (below). The ground past it is the map's own tiles, texel for texel, each flipped across
every edge it is past; on it stand the map's own features, flipped across a side edge and upright
across the top and bottom, a copy above or below that would reach across the map's border left
out whole. Units stay on the map, and the minimap is unchanged. Nothing here patches or writes the engine. It adds one hook, a stub at LoadMap's
join `0x483B53` on the loader thread, which reads the TNT's feature records once a load (below,
§2.16 and §2.5).

**The setting: `edge = mirror | black`, default `mirror`** (`TS_EDGE`, [renderers](../renderers.html)
§2.10b). One row in the render-options panel (`EDGE`, *Map edge*, Black | Mirror, the eighth,
§2.12) and in the front end's Visuals column. `tagpu_opt.c` answers `tagpu_opt_on("tagpu_mirror.on")`
lever first (`tagpu_mirror.on` mirrors, `tagpu_mirror.off` blacks), then the store, then the
compiled default; `tagpu_defaults.off` skips the store and the play defaults, so every control
launch draws black. The render thread re-reads it with the native pass's 30-frame poll and logs
`native: map edge = mirror` or `= black` on every change. It is not a render key: the preset never
rewrites it, `derive_style()` never reads it, and the row is never greyed by the Classic lane.

**Where the map ends: the engine's area** (`tagpu_fx.h` `tagpu_edge_area`). Every edge pass ends
the map at the **scroll extent** `main+0x1422B`/`+0x1422F` — the map's pixel size less 32 px wide
and 128 px tall, written by `0x4833B0` at the level load ([engine map](../exe-reverse-engineering.html),
"LoadMap's feature placement, and a saved game") and carried in the packet as `map_pxw/h` — in
32-px tile cells, not at the tile map's own size (the PLOT grid × 16). Stock's camera clamp
`0x41C3C0` holds the eye in `[0, extent − view]`, so stock never shows a pixel past the extent,
and the strip between the two is art no player saw. Not all of it is unbuildable: pass 1 of
`0x4833B0` masks the last two 16-px columns of every row, but pass 3 walks up each column only
while `row·16 − h/2` is past the extent's height (`0x48354D..0x483557`, `jle` out at the first row
that is not) and masks the cell above the one it tested, so on raised ground at the bottom edge
some of the bottom strip stays buildable (at `h ≥ 32` its top row, at `h = 64` two rows). **The strip is often unpainted.** Painted Desert's last
tile column holds the editor's blank tile 0, flat palette blue (123, 151, 255), in rows 32..63; a
survey of the 275 stock TNTs found flat blank tiles in that strip on 9 maps — Yerrot Mountains
204 cells, Plains and Passes 118, Ring Atoll 96, Painted Desert 32, exp1cc02 14, Metal Maze 12,
Ashap Plateau 3, Silent Slayers 2, Sink the Battleship 2 — and inside the playable area on none.
A fold at the tile map's edge would draw that strip as the map and reflect the blue into the
edge. `tagpu_edge_area(extent_px, cells32)` is `extent_px / 32` **bounded to
`[1, cells32]`**, `cells32` the tile map's own width or height that the caller indexes with, so a
cell folded into `[0, area)` by `tagpu_edge_reflect` indexes the tile map whatever the packet
says; an extent that is not positive or not inside the map answers the whole map. All 275 stock
TNTs have even 16-px dimensions, so every stock extent is a whole number of tiles. The camera's
centre range ends at the same words (§2.3c), so the view centre stops where the fold is.

**Terrain: an off-map cell is one more instance** (`tagpu_terr_gather`). Every cell of the gather
rect past the area (`areaC` × `areaR`, from the packet's `map_pxw/h`; only the row stride stays the
tile map's) takes the tile of the cell it folds to, `tagpu_edge_reflect(m, n)` — the lab's
triangle wave, `p = m mod 2n` taken into `[0, 2n)`, `p` or `2n − 1 − p`, so any distance past the
map lands on a map cell and says whether the axis turned over. The instance carries the tile
column with `TAGPU_TERR_MIRROR` (0x2000), each flipped axis's `TAGPU_TERR_FLIP` (0x4000), and on a
flipped axis `TAGPU_TERR_FOLD_LO` (0x1000) / `TAGPU_TERR_FOLD_HI` (0x0800) when the cell's low or
high edge lies on a fold (below): spare bits by bound, since a tile column is under 64 and a row
under 1024 (a u16 tile index over 64 columns), so bits 11 to 14 are clear in every on-map record
and a flagged one is still a positive short in the `SSCALED` attribute
(`terr_mirror_flags_unchanged` holds the numbers). The vertex stage takes each flag off as an exact
power of two and runs a flipped axis's corner the other way; the fragment stage draws a mirrored
cell as the base atlas (or the restored twin where the reveal has painted it) through `taEdge`, with
no fog of war and no light, and returns.

- **The tone, `TAGPU_GLSL_EDGE_FN`, is the lab's `EDGE_TONE` at the owner's knobs:** `o = max(−w,
  w − uMapPx, 0)` per axis, `t = |o| / 1536`, the colour mixed 0.75 toward its RGB mean, times 0.5,
  times `1 − t`. `w` is the point drawn on the tile grid and `uMapPx` the area's 32-px cells × 32
  (`TAGPU_TERRHAND.mapPxW/H`, the block's last vec2 at 184; the terrain block stays 192 B). One copy
  for both passes, so a tree and the ground under it cannot drift apart.
- **The nudge, reflected.** `TAGPU_EDGE_NUDGE` moves every map quad 1/32 game px the same way on
  screen, so a sample reads the texel at its world point plus `e = NUDGE/zoom`: at 0.25× every
  sample lies on a texel boundary, and the nudge breaks the tie toward the next texel. A mirrored
  sample must read what its reflection reads on the map, the texel under `r(w) + e`, and on a
  flipped axis that is an affine function of `w` running backwards: `u = r(w) + e − (the source
  cell's origin)`. The vertex stage carries that function exactly at the quad's corners, so what
  decides a sample is WHICH quad it falls in — and on a flipped axis the cell whose `u` covers it
  is the cell's own span moved the other way by `e`. So **a flipped quad is nudged +NUDGE**, not
  −NUDGE. At a **fold** — the map's own edge, or a map's width further out (`edge_folds`), where
  the neighbour is the map or a copy the right way round, nudged −NUDGE — the shared corner keeps
  −NUDGE and its `u` takes the same function's value there, `32 + 2e` or `2e`, so the two quads
  meet with no gap and no overlap, and the guard ring is read only by the samples within `2e` of
  the fold, where `r(w) + e` is past the tile's last texel. Nudging a flipped quad −NUDGE and
  adding `2e` throughout gets every sample inside a flipped cell right and every one on a seam
  between two flipped cells one texel out of phase: at 0.25× from an eye off the 32-px grid,
  (−447, −351), up to 68 levels at every world x or y ≡ 0 (mod 32).
- **The grid starts from a floor** (`div32_floor`). The engine's `sar 5` divides toward zero,
  which for an eye < 0 — past the top or left edge, where the centre clamp now goes — starts the
  grid one cell late and leaves up to 31 game px of the view's leading edge with no cell: under
  black those are off-map cells the gather skips anyway, under the mirror a band of clear colour
  (measured at the eye (−300, −200), 1×: 12 px wide on the left, 8 px on top). For an eye ≥ 0 the
  two divisions agree, and on-map cells land on the same pixels either way, since the origin
  moves one cell and the fraction 32 px together. The lab's mirror sweep had the same band and
  takes the same floor ([terrain depth](../terrain-depth.html)).
- **`skipped` is still the off-map count under black**; under mirror the heartbeat's `mirror=N`
  counts the cells drawn past the map (`terr: … off-map=0 mirror=705 …` at the north-west corner at
  0.5× on Two Continents). The same line prints the tile map and the area side by side:
  `terr: grid=… map=289x292 area=288x288 …` on Painted Desert (MEASURED 2026-10-25). Under black
  a cell past the area is skipped like any other off-map cell, so the hidden strip is black there
  too, as stock's view never reached it.

**Features: the map's own, as its TNT lays them out.** **The rule (the owner's ruling): the
mirror's map is the MAP FILE alone** — what the map's TNT places, never what a game, a saved game,
a scenario or the map's schema has since done to it. A tree burnt, reclaimed or crushed in play
stays in the mirror, and so does one a saved game had lost before it was saved; a wreck left or a
scenario's own feature never appears. The schema is not the map file: the `.ota`'s own features,
which `0x423160` places through `0x4224B0`/`0x423C50` on a new game only, are not mirrored, and a
TNT feature that one of those spawns destroys still is.

- **Built on the loader thread, from the TNT** (`tagpu_packet_pub.c` `mapfeat_at_load`, the stub at
  LoadMap's join `0x483B53`, §2.16). No moment of a saved game's load has the TNT's features in the
  grid: LoadMap skips their placement for a saved game and the save's own records replace them
  later, with the border mask run in between (an always-on fix keeps it from losing them, §2.6c;
  [engine map](../exe-reverse-engineering.html), "LoadMap's feature placement, and a saved game"). So
  the list is rebuilt from the TNT's records
  by LoadMap's own placement rules on a private W × H grid — v2's void markers, then the TNT's
  features in row order with the spawn's refusals, its destroys and the wreck pool's count — and a
  new game and its save mirror the same features by construction. It keeps every GAF anchor that
  survives, row-major, with the anchor scan's 2×2 height term from the grid's heights:
  `TAGPU_PK_MAPFEAT {col, row, def, lift}`, 8 bytes, at most 65 536 (past it the first 65 536 in row
  order, and `TAGPU_PK_TRUNC_MAPFEAT` says so). A 3D feature is the wreck pool's and is not kept,
  and the schema's own features (the `.ota`'s) are not the TNT's. Every bound is the engine's: the
  W × H LoadMap has just read, a def read only under the FeatureDef count `0x421F20` has just set
  (the engine spawns a TNT index past it unchecked; the list skips and counts it), and every
  footprint inside the map by the spawn's own refusal. Built only when the session draws with
  Vulkan. The log: `packet: LoadMap (loader thread T, level gen N): the map's own features from
  its TNT: 4893 over 672x800 (v2, a new game; 0 spawn(s) refused, 0 past the 31 defs, 0 3D, 0
  refused a pool record) in 1.86 ms`.
- **The hand-over is an ordering, not a timing.** Every LoadMap join first empties the slot
  (`mapfeat_unpark`, under the lock), so a load whose build fails — a bound refused, a failed
  allocation — leaves nothing parked, and its edge stays black rather than mirror the previous
  load's features (a load abandoned on the loading screen does not move the level number, so its
  list would otherwise pass for the next map's). Nothing parked is not an empty list: an empty
  list is a map with no features, and mirrors bare ground. The loader parks its malloc'd list in a slot
  stamped with the loading level's generation, under an interlocked lock it waits for; the game
  thread only tries the lock, at every in-play draw until it holds this level's list
  (`mapfeat_adopt`, above the publish and outside its FRESH gate, because a publish can be skipped
  and this must not be), and adopts only a list stamped with the current level. So the list is
  read only after it is written, a second load before the adoption replaces the first rather than
  racing it, and a stamp that were ever stale would leave that level's edge black, never mirror
  another level's features. The level end bumps the generation on the game thread before the next
  load's thread is created: in the log, `packet: level end -> gen 1`, then `loader thread …
  entered 0x497C70 (entry #2; … level gen 1 …)`, then `LoadMap (… level gen 1)`, then `level gen 1:
  the map's own features adopted from the load: 4893`. It is adopted whatever the setting, so a
  switch to mirror in the middle of a level finds it.
- **It rides the packet only while it is wanted, and only whole**: `fill_mapfeat` appends it while
  `tagpu_feat_mapfeat_want()` — the feature pass will draw features (armed, not `passive`, with
  the wreck half that lets it own the feature leaf) and the edge is on mirror — and
  `tagpu_feat_mapfeat_holds(level_gen)` is 0, and sets the header's `mapfeat_ok` only when every
  entry landed. **A map with no features is a list of none**, carried with `mapfeat_ok = 1` and
  held like any other, so its ground mirrors. `want` is stored every frame before the native
  pass's first early return, so it is this frame's; `holds` is two aligned words, the level
  stored before the flag first rises, so no level value is reserved. A stale read costs one more
  packet or one packet later, never a wrong picture. The table is laid down **right after the
  world tables**, inside the reserve's design point, which counts it (§2.16, §2.86), so a cut does
  not reach it there. Its `carried=` count is among the fill counts a refill takes back.
- **The consumer's copy** (`tagpu_feat.c` `mapfeat_take`), taken only from a packet whose
  `mapfeat_ok` is 1, is bucketed by row with a counting sort
  into static arrays sized by `MF_DIM` (4096, the dimension the gather already refuses a map past),
  and **every entry is bounded on the way in** (`mf_keep`) — a row or column outside the
  dimensions it claims is dropped, and so is one not strictly right of the last entry kept on its
  row — so the lookups index by a row under `mapH` and a column under `mapW`, and each row's
  bucket is ordered by column, by this side's own test and not by the publisher's order (which is
  row-major already, so nothing is dropped there).
- **A feature is the map's by where it is DRAWN, not by its anchor.** Its drawn point is
  `draw_feature`'s `wax = col·16 + hx`, `waz = row·16 + hz − lift` (`hx`, `hz` half the footprint,
  `lift` the 2×2 height term `>> 3`, at most 127 px). Under the mirror the anchor loop drops a
  feature of the map's own drawn at or past the area (`wax ≥ s_areaW·16` or `waz ≥ s_areaH·16`,
  `s_areaW/H` the area in 16-px cells, two per tile, at most the map's), and `mirror_feature`
  takes exactly the features that test keeps as the sources of the copies: a source drawn past
  the area has no copy, and one anchored in the hidden strip but drawn above the fold has its
  copies. A band above or below the map takes only sources drawn below the map's top, `waz ≥ 0`,
  since one drawn above it would put its copy's base on the map's rows.
- **The gather** (`mirror_gather`, after the map's own anchors) folds at the area. Its rows come
  in **bands**: band 0 is the map, an odd band the map turned over and an even one the map the
  right way round, `b` areas' heights away, and a copy in band `b` sits on sweep row `b·H + srow`,
  or `(b + 1)·H − 1 − srow` turned over. Since a feature is drawn fewer than `MF_LIFT_ROWS` (8)
  rows above its anchor, a source anchored up to that far past the area is still the map's, and
  its copy's row lies up to that far inside the neighbouring band: **each row is asked of its own
  band and of a neighbour within 8 rows**, and every (row, band) pair names one source row, so no
  copy is emitted twice. Every cell of the rect the engine would sweep — the unclamped rect for a
  copy the right way round, the rect mirrored about the view for a turned one (below) — takes the
  map's anchor at the cell it folds to; a source row must lie under the table's last row
  (`s_mfH − 1`), and a source column under the area and under the map's last column
  (`lastCol`), which the engine's sweep never reaches. **It walks anchors, not cells**: a sweep row splits at the folds into runs of at most one map width, each a copy of
  the source row the right way round (copy `q = floor(col / mapW)` even) or turned (odd), and each
  run is one pass over the row's bucket, at the cost of the anchors alone. MEASURED 2026-09-24 on a
  diagnostic build, Two Continents' north-west corner at 3840 × 2160 and 0.25× (a 944 × 560-cell
  sweep, 2 193 mirrored anchors): 433 µs p50, 753 µs p99 walking every cell; 239 µs p50, 436 µs
  p99 walking the anchors, which is the emission itself. A mirrored feature **ends** on the cell
  its source begins on (`wax = (col + 1)·16 − hx` on a flipped x), and across the top or bottom
  the half height is **added** (`waz = (row + 1)·16 − hz + lift`), because the mirrored ground is
  the tile art, with the height painted in. The quad is flipped about its anchor on a side edge —
  `x0 = sx − (w − hotx)`, the u's swapped — and never on a y one.
  - **Every run is emitted in its source row's order**, a turned one right to left on the screen:
    the runs follow the rows down and each takes its bucket in the order the map's own anchors
    come. With the keys (below) the order is the whole of how a copy stacks — the shadows write no
    depth, so where two overlap the later blends over the earlier, and where two bodies' keys tie
    in the depth buffer the first drawn stays — so a copy laid down in its source's order over its
    source's keys stacks as its source does.
  - **A turned run sweeps the rect mirrored about the view** (`KEYRECT.tuc0`). The engine's rect
    reaches 10 columns left of the view and 2 right (`SWEEP_COL_LEAD`, `0x469748..0x4697B3`), so
    the map's art that overhangs right of its anchor is drawn from anchors up to 10 columns off
    screen; a turned copy overhangs left, and takes its cells from the rect the engine would sweep
    over the view reflected, `tuc0 = ceil(right edge / 16) + 10 − nCols`. A copy is drawn exactly
    when its source would be, so the side strip is the map's picture reflected up to the view's
    edge.
  - **A copy whose columns miss the view is not emitted**: a copy `q` whose columns
    `[q·mapW, (q + 1)·mapW)` (16-px cells, `mapW` the area's width) miss the view, since the fragment stage would discard
    every sample of it. Rows are not culled per band; `emit_frame` drops a quad wholly off the
    view.
  - **A copy above or below the map that would reach across its border is not drawn**
    (`frame_crosses`). The copies there stand upright, so the one nearest the line would lay its
    crown over the map's last rows (below) or its foot over the first (above), and one cut at the
    line is a tree without a top. Every frame the copy would emit, its shadow (when shadows draw)
    and its body, is tested as `emit_frame` places it — each sub-frame at its own hotspot, `y0 =
    waz − hotY`, `y1 = y0 + h` — and the copy is dropped when `y1 > 0` above the map or `y0 <
    s_areaH·16` below it. The frames are resolved once (`copy_frame`) and those are the frames
    emitted; an animating feature's state is read once (`tagpu_gaf_state_seq`: its sequence and
    frame index together) and the copy is judged on every frame of that sequence, so the decision
    holds while it animates and nothing of a drawn copy crosses the line. **The owner's choice
    (2026-10-25)** among four drawings — copies clipped at the line, unclipped over the map, the
    crossing ones dropped, or turned upside down; the upside-down and the clipped upright ones
    were rejected.
- **The depth keys** (`KEYRECT`, `map_key`, `mirror_key`). **The map's own features keep main's
  keys**, taken over the engine's sweep rect clamped to the map with the mirror on or off, so an
  on-map feature sorts against units and 3D wrecks exactly as it does under black; `map_key` is
  the one function that computes them. **A mirrored feature takes its source's key, as copy 0 of
  its row takes it**: the source's own column, whatever copy the feature stands in. On the map's
  rows copy 0 is the map, so a side copy's key is `map_key` — the same float, the same stored
  depth, the same ties. Above and below the clamped rows copy 0 is the mirror's upright
  band: a tall key is `map_key`'s rule on its row (`3 + 4·rel` plus `1.5·(col − c0)/cols`, `rel`
  clamped to the band the frame's keys were sized for, `rows + 8`, below the effects), and a flat
  one takes `0.40 + 0.10·f` with `f` in `[−0.6, −0.1)` above the map and `[1.1, 1.6)` below it,
  row-major over the whole sweep, so the three flat bands never overlap and every flat key,
  shadows (0.03 under) included, stays inside the particle layers' bracket, 0.30 and 0.60.
  **Different copies of one source share a key and never a sample** — the fragment stage keeps
  each copy inside its own columns. The source's column is inside the clamped columns for every
  copy the view can show, because the centre clamp keeps the view's centre on the map, so the
  reflection of what the view shows past an edge is on the map in view and its sweep holds the
  sources; a column outside them keys as the nearest inside, which only the sweep's margins reach.
  **Why partner keys and not a painter's order across the edge**: two flats of one row are
  `0.1/span` apart in the key, and the depth buffer resolves about `1.2e-7` of the frame's depth
  scale (`depthScale`, some 360 at 1× and 890 at 0.25× at 1024 × 768), so neighbouring flats of the
  map's own tie in the depth buffer and `VK_COMPARE_OP_LESS` keeps the first drawn — in the float
  format as in D24, which tie the same flats (§2.91, 0 px on `feat-forest`). Main draws the
  map that way; a mirror keyed to continue the order would stack those same flats differently
  from the map it reflects, and a copy keyed as its source repeats the map's picture exactly.
- **The fragment stage** reads the mode as bits: `TAGPU_FEAT_MIRROR` (4) and the copy's column
  index `q` biased by 1024 above bit 4 — the gather emits no copy outside `[−1024, 1024)`, so
  every mode stays under 2¹⁵ and the float attribute carries it exactly. **Each copy draws only
  in its own columns**, as the ground does — a quad reaching across a fold would lay one copy's
  feature over the next copy's reflection of it. **A copy above or below the map is drawn whole**:
  the gather has already left out every one that would cross the border, so no row clip is
  needed. **While the mirror draws, the map's own quads keep to the map's columns** (`uClip`, the
  block's int at 40): past a side edge the picture is the map reflected, and a tree's overhang
  there would cover the reflection of the tree itself; across the top and bottom the copies stand
  upright, so the overhang is kept. **Under a black edge nothing of the map's own draws past the
  area on the right or at the bottom** (`s.x ≥ uMapPx.x || s.y ≥ uMapPx.y`): the engine's sweep
  reaches anchors in the hidden strip (rows and columns up to the map's last but one), and
  clipping the pixel rather than dropping the anchor keeps the part of such a tree that reaches
  up into the area, which is what stock's viewport, cut at the same line, showed; the left and
  top are not clipped. `uMapPx` is the area on the tile grid (`s_areaW·16`, `s_areaH·16`), the
  terrain's own. **Every one of these clips tests the sample's point
  moved to the ground's side of a fold**, `s = vWorld + e` with `e = TAGPU_EDGE_NUDGE / zoom`
  world px (`uNudgeW`), never `vWorld` itself. The terrain puts every fold — the map's four edges
  and each copy's own `x = q·W` — at the fold less `e` on screen, so a sample belongs to the side
  its point moved by `e` lies on; at the 0.25× floor, with the eye's parity, a whole column or row
  of samples lies on a fold, where `vWorld` alone would sit on the line and take whichever side
  the interpolation rounded it to. **The margin is the invariant**: a sample on a fold is `e` from
  the line, 1/8 world px at the floor (128 ulps of a coordinate under 16 384), and at every zoom
  `8·ss` times the rasteriser's snap of the ground's own edge (1/256 of a framebuffer px,
  `subPixelPrecisionBits` 8 on the reference setup's GPU and on llvmpipe). Then the hole from the
  base's alpha, the twin's colour where the reveal painted it, `taEdge`, and premultiplied
  half-alpha frames. No fog, no light.
- **Draw order**: four buckets, the map's shadows and bodies, then the mirror's shadows
  (`s_pipeShadow`, no depth writes) and bodies (`s_pipeBody`). The fragment block is 48 B
  (`uMapPx` at 32, `uClip` at 40, `uNudgeW` at 44); each block is rounded to
  `minUniformBufferOffsetAlignment`.
- **One tie rule for every sprite** (`emit_frame`). A quad's corners lie on whole game px, so at
  the 0.25× floor, where every sample centre lands on a whole world px, every sample lies on a
  texel boundary, and which texel it reads — and whether the quad covers it at all — would be the
  interpolation's rounding, which follows where the frame sits in the atlas and runs the other way
  for a turned quad. Every feature quad, the map's and the mirror's, moves by
  `TAGPU_EDGE_NUDGE_PX / zoom` on the CPU, as the terrain's do: an upright one up and left, a
  turned one right and up, because its texels run the other way. A sample on a boundary then lies
  `NUDGE/zoom` world px (1/32 game px on screen) inside the texel after it, and a turned copy's
  sample reads the texel its reflection reads on the map. The world corners are taken from the
  moved quad, so `vWorld` stays the sample's own point for the fog lookup and the edge's clips.
  **It is the ground's 1/32-px shift, at every zoom and under a black edge too**, so a sprite and
  the ground under it shift together: every sample reads the texel under its point plus `e`. On
  the dyadic zooms from 0.5× to 8× no sample lies within `e` before a texel boundary (at 1× a
  sample is 1/4 world px inside its texel), so no sample changes; at 0.25× every tie reads the
  texel after it; and at the wheel's other resting levels (its 1.14 steps, `tagpu_zoom.c`) every
  sample within `e` before a boundary reads the next texel — about a share `e` of the sample
  columns and of the rows, the level's alone and the same at every whole-px eye: 4.0 % of each
  at one notch out (0.877×), 11.6–11.7 % at ten notches (0.270×), 3.0 % one notch in (1.163×)
  and 0.43–0.45 % at thirteen in (7.10×) at 1024 × 768 — every level the wheel rests on but 1×,
  8× and the 0.25× floor, whose samples lie on the boundaries themselves. Which zooms carry
  exact ties is the viewport width's parity: an even
  width puts the zoom centre on a whole px, the case above; an odd one puts it on a half px, which
  takes the 0.25× samples off the whole world px and puts the 0.5× ones on them.

**The mirror reflects what the world passes draw** (`tagpu_feat_mapfeat_sync`). The render thread
asks the feature pass before either gather, and the sync answers from what the pass will emit this
frame. **While it draws the map's features, ground and trees mirror together or not at all**:
`fv.mirror` is 1 only once the pass holds this level's copy of this map, so at a level's start, or
a switch to mirror, both switch on the same frame, one or two frames after the setting. **While it
draws none** — unarmed, under `feat.on=passive`, or without the wreck half (`native.on` without
`wrecks`) — the map on screen has no trees either (the engine's own feature draw reaches only the
reference surface, §2.81), and the ground mirrors alone. **A gather that bails after the sync said
1** — the atlas not set up, the FeatureDefs unreadable, the dimensions or the sweep out of bounds —
draws no feature on the map or past it that frame, so no bail gives a bare mirror beside a wooded
map; and a gather that runs mirrors exactly when the sync said so, since the sync has already
checked the level, the dimensions and the copy the gather tests again. A load whose list was not
built leaves the level with none, and the sync keeps the edge black.

**MEASURED 2026-10-25, the fold at the area:**

- **The picture reads as its reflection about the fold.** The correlation of the picture with its
  own reflection about the fold line, all at the area's fold: Painted Desert's right fold 0.972
  at 1× (terrain and its rocks), and in its bottom-right corner at 0.5× 0.927 on the right fold
  and 0.93 on the bottom one; Assault on Suburbia's bottom fold 0.79 with the terrain alone.
- **The lab mirrors as the game does**: on Assault on Suburbia's bottom edge the lab's mirror
  and the game's differ by more than 8 levels at 0.04 % of the pixels.
- **The camera's far ends** are in §2.3c (Painted Desert, Assault on Suburbia).

The blocks below, 2026-09-24 and 2026-09-25, were measured with the fold and the camera's far
ends at the tile map's own edge, the PLOT grid × 16; their south-east eye `(10304, 12448)` on Two
Continents was the corner of that range.

**MEASURED 2026-09-24** — Two Continents, `feat-forest` at 1024 × 768, `ss=2`, the reference setup's
discrete GPU on a private Xvfb display:

- **`edge=black` draws what main draws, 0 px.** Against main at the BAR camera's landing, each world
  pass A/B'd alone under `tagpu_defaults.off` at the shipped `ss=2` (3 145 728 px a capture), both
  presets. Terrain: 22 of 22 captures at 0 px, eleven views — (2566, 616) at 1× and 0.5×,
  (0, 3000) at 0.25×, (0, 0) at 0.5×, (1200, 2000) at 1×, the north-west corner (−448, −352) at 1×
  and 8×, the south-east (10304, 12448) at 0.25×, and (−300, −200), off the cell grid, at 1×, 0.5×
  and 0.25×. Features: 14 of 14 at 0 px, at seven of those views and (1200, 2000) at 0.5×; the
  south-east corner at 0.25× is open sea, and with no feature in view neither build takes a
  capture. Units: 10 of 10 at 0 px — the scenario's own at (2566, 616) at 1×, 0.5× and 0.25×, and
  nine more placed at the north-west corner, seen from (0, 0) at 0.5× and 1×; at (0, 0) at 8× no
  unit is in view and both builds draw nothing. **The feature and unit passes armed alone keep the
  engine's own eye range**, `[0, extent − view]` — the centre clamp that reaches past the edge
  comes with the terrain pass — so their captures asked for at an eye past the map were taken at
  the nearest eye inside it: the north-west corner's and (−300, −200)'s at (0, 0).
- **The mirror is the map reflected through the tone, to the rounding.** With the features hidden
  (`feat.on=noflat notall noshadow nowreck`), every off-map pixel whose reflection is on screen was
  predicted offline from that on-map pixel through `taEdge`, averaged over the `ss` samples: at the
  north-west and south-east corners at 0.25×, 0.5×, 1×, 2× and 8×, and at (−300, −200) at 0.25×,
  0.5× and 1× — every strip (side, top or bottom, corner) within one level, 100 % of its pixels.
- **Against the lab**, the pack rebuilt from this tree, `edge=mirror`, features on, the cursor
  masked: at 1× at both corners and at (−300, −200), and at 0.5× at the north-west corner and at
  (−300, −200), every pixel on and off the map within two levels. At 0.25× the lab's own mirror is
  not an exact reflection — it misses the phase this pass's reflected nudge corrects — and it misses
  by up to 88 levels at a mirrored tree's edge (12 over the south-east's open sea); the game's
  0.25× passes the recompute above. At 8× rows of texel boundaries land one pixel apart in the lab
  and the game on the map itself (up to 48 levels), and through the tone past it (up to 13).
- **No seam, gap or band.** Three scans — a clear-colour pixel where the tone is lit, a one-pixel
  line of clear colour between lit pixels, a band of clear colour along the viewport's border —
  read 0 at every view above and over 300 window grabs of a scripted edge scroll with the shipped
  Classic++ play set: at 0.25×, 0.5×, 1×, 2× and 8×, into the north-west corner along the left
  and top edges and into the south-east along the right and bottom, twelve frames each, then the
  wheel out, in and back over each corner, ten frames a gesture. `tagpu.log` over the run: no
  deferral, `trunc=0`, `viol=0`.
- **The setting**: a click on the Map edge row turns the view black within the poll (`native: map
  edge = black`), `edge=black` survives a relaunch, `tagpu_mirror.on` over that store mirrors and
  greys the row, and every `tagpu_defaults.off` launch logs `native: map edge = black`. The front
  end's Visuals column carries the row eighth under *Impure rendering*.
- **The cost**, GPU p50 with `--maxfps 0`, mirror against black alternated on one instance at the
  north-west corner, Classic++: 1.11 against 0.76 ms at 1× and 0.91 against 0.72 at 8× (six reports
  each, within 0.03 ms), and at 0.25×, the widest view, 1.18 against 0.80 ms over nine reports each
  (eye (0, 0), before the merge with the BAR camera); with the mirror the p99 at 0.25× reads
  2.3–3.4 ms against about 0.95. Two other
  sessions' instances shared the GPU for the later 0.25× runs, which scattered their reports from
  0.12 to 1.29 ms; their one quiet round read 1.21–1.29 against 0.77–0.81. Where that p99 sits is
  below, with the fix round's numbers.

**MEASURED 2026-09-24, the review's fix round** — the same setup unless a line says otherwise:

- **`edge=black` draws what main draws, 0 px**, against main's DLL at the BAR camera's landing,
  each world pass alone at `ss=2`, both presets, at the views above plus the eye off the cell grid,
  (−447, −351) at 1×, 0.5× and 0.25× and (−301, −201) at 0.25×. Terrain: 30 of 30 at 0 px (fifteen
  views a preset). Features: 24 of 24 at 0 px (twelve views a preset), the eyes past the map taken
  at (0, 0), as above — so these are on the cell grid; the round after puts one off the grid
  inside the map.
  Units: 17 of 20 at 0 px; the other three, Classic++ at (2566, 616) at 1×, 0.5× and 0.25×, differ
  by 66, 6 and 7 samples of at most 2 levels, and main differs from itself between two launches by
  exactly those — this build's capture equals main's second launch. The anchor walk (above) changes
  only the mirror's gather; its build's feature pass under black reads 0 px against main at eight
  more captures, both presets.
- **The map's own features keep main's keys under the mirror.** Features, five 3D wrecks and three
  units at the north-west corner, window grabs of the presented frame, this build mirroring against
  main drawing black: 0 px on the map at six views (1× at the corner and at (−150, −100), 0.5× and
  0.25× at (−300, −200) and at (−447, −351)), both presets.
- **The reflected nudge holds on a seam** (`TAGPU_TERR_FOLD_*`, above): the terrain recomputed
  offline from the map through `taEdge`, every strip within one level, 100 % of its samples, at
  (−447, −351) at 1×, 0.5× and 0.25×, (−301, −201) and (−448, −352) at 0.25×, the south-east at
  0.25× and (10301, 12401) at 0.5× — about 600 000 samples a strip. Before, at 0.25× from
  (−447, −351), up to 68 levels at every world x or y ≡ 0 (mod 32).
- **A map with no features mirrors its ground.** Core Prime Industrial Area (642 × 648, no
  FeatureDefs), the shipped play set, (−1500, −1200) at 0.25×: `0 of 0` taken, 7 695 cells drawn
  past the map, and 558 790 of the viewport's 630 784 pixels lit, where the first build lit the map's
  157 808 and left the rest black.
- **A saved game mirrors the map, not the save.** Two Continents: trees near the edge reclaimed, the
  game saved and loaded: `LoadMap (… a saved game; …) 4893`, and the mirror shows
  those trees whole; the first build's grid read mirrored them gone. The load itself loses the
  border trees stock TA loses ([engine map](../exe-reverse-engineering.html), "LoadMap's feature
  placement, and a saved game").
- **The feature pass drawing none leaves the ground alone**: under `feat.on=passive`, and with
  `native.on` without its wreck half, the north-west corner at 1× reads `terr: … mirror=462`, the
  table is never carried (`carried=0`), the pass holds no copy (`mirror: map=-1` under `passive`), and
  the 448 × 352 corner block is lit in all 157 696 pixels — the same count as the shipped play set.
- **A cut lands on the effects, never on the table.** A diagnostic build with a 576 KB reserve and
  the table forced into every packet, Town & Country's 6 206 features, a big battle: 3 866 publishes
  cut, each on the minimap, the particles or the light (`truncated` 0x8000, 0x8400, 0x8800), the
  table whole in every sampled packet, `viol=0`. At the shipped reserve the world tables end at
  476–519 KB and the table adds 49.6 KB, before the effects (9–75 KB) and the UI (47 KB).
- **The list's cost on the loader thread**: 1.86–2.18 ms on Two Continents, 6.89 ms on Seven Islands,
  the largest stock map; 0.66–1.40 ms on seven more.
- **`mirror_gather` on the CPU**, the diagnostic build: at the north-west corner at 0.25× on a
  3840 × 2160 screen (a 944 × 560-cell sweep, 2 193 mirrored anchors), 433 µs p50 and 753 µs p99
  walking cells, 239 and 436 µs walking anchors; at 1024 × 768 (240 × 212, 367 anchors) 39–40 µs
  and 77–89 µs.
- **Where the p99 sits.** Per-pass GPU timestamps (BOTTOM_OF_PIPE around each world pass and
  between the feature pass's four buckets, the diagnostic build), the shipped play set, the
  north-west corner at 0.25× at 1024 × 768, `--maxfps 0`, mirror and black alternated. **No world
  pass carries it**: terrain 0.49–0.52 ms p50 with the mirror against 0.22–0.24 under black, the
  features 0.08–0.10 against 0.02–0.03 (the mirror's own two buckets 0.03 and 0.04), and each
  pass's p99 within 0.17 ms of its p50 on both sides. The frame's tail lies **after** the world
  passes, in the span from the last of them to the frame's end (the marker pass, the end of the
  world target, the composite and the UI, none of which the mirror changes): 1.53 ms p50 and
  2.8–2.9 ms p99 with the mirror against 0.26 and 0.26 under black in one round, while in another
  the whole frame read 1.26/2.49 ms against 0.82/0.96. Another session's instance was drawing on
  the same GPU throughout both rounds; without one (next block), the same tail is there under
  black as well.
- **The LOS cull is not the cost**: the engine LOS-tests only FeatureDefs with `+0xFF` bit 3, and
  no tree has it, so the map's own pass culls none of what the mirror draws either.

**MEASURED 2026-09-24, the tie rule and the stacking** — the same setup; the build merged with
main at the effects' models (§2.89) unless a line names the build before the merge:

- **`edge=black` draws what main draws**, against main's DLL at the effects' models, each world
  pass alone at `ss=2`, both presets. Terrain: 30 of 30 at 0 px (the fifteen views above, the
  off-grid eyes among them). Units: 8 of 8 at 0 px in Classic, at (2566, 616) and (2567, 617)
  at 1×, 0.5× and 0.25× and at (0, 0) at 1× and 0.5×; in Classic++ this build's eight captures equal
  main's own second launch before the merge, and main's equal its first, sample for sample — the
  launch spread above. Features: 12 of 12 at 0 px at 1×, 0.5× and 8×, the eye (1201, 2001) off
  the cell grid among them, and neither side takes a capture at (2566, 616) at 0.25× or (2567, 617)
  at 1×, where a second pass draws into the same frame and the A/B capture refuses it (`tagpu.log`:
  `1 A/B levers claimed this frame and 2 passes drew into it - nothing captured`); at 0.25× the tie rule (above) changes the map's own sprites, (0, 3000)
  by 18 253 samples and (0, 0) by 69 303 in Classic, 22 279 and 84 026 in Classic++, of 3 145 728 —
  at the floor every sample centre is a tie, so every change is a former tie. Those four pictures
  are identical in three launches of three builds, two before the merge and one after. Main's two
  DLLs, before and after the effects' models, draw every one of these captures alike. Merged once
  more, with main after the weapon IDs to 4096, the set reads the same against that main, capture
  for capture, on both sides.
- **The side strips are the map reflected, to one level but for one sample.** The feature pass
  over the terrain (a capture-diagnostic build), the strip against the map's own pixels through
  `taEdge`, at the north-west corner, the left side (−448, 600) and (−448, 2000), and the right
  side (10304, 6000), each on the cell grid and one px off it, at 1×, 0.5× and 0.25×: 17 of 18
  views with every sample within one level (about 1 260 000 samples a side view, 630 000 a
  corner), and the right side at 0.25× with 1 of 1 261 410 two levels off. Two launches before
  the merge and one after are identical at all 24 views of the set, the top strips' included.
  **That one sample is a translucent shadow's rounding** (*What it does not do*): with the bodies
  not drawn, 2 of 1 261 412 at 0.25× and 8 of 1 261 560 at 1× on that side are two levels off and
  none on the left; drawn opaque, the same shadows are exact at all three (0 of 1 247 112,
  1 261 270 and 1 207 004).
- **The top strip's sprites are their sources moved straight up.** A diagnostic build that draws
  no terrain and only features standing apart (greedy in row order, 6 rows and 5 columns clear):
  every whole sprite in the band equals its source on the map moved up by a whole number of
  samples, through the tone, within one level — over the map's top-row trees at (4132, −352) and
  one px off it, 2 and 2 sprites at 1×, 2 and 2 at 0.5×, 60 and 59 at 0.25×, and 2 and 2 at
  (3000, −352) at 0.25× (every tenth row and column). A tree in the map's first rows overhangs
  the edge, so its source straddles it.
- **The depth keys, offline** (the shipped text lifted from `tagpu_feat.c`, x87 float semantics as
  the DLL is built): over 20 000 random maps and views under the centre clamp, 65 419 838 copies —
  in every copy the keys rise row by row (0 violations, flats and talls apart) and no two anchors
  of one row share a key (0), no source column falls outside the clamped columns (0), and the
  map's own keys equal main's inline arithmetic bit for bit, no two of them equal.
- **The frame's tail is not the mirror's.** Per-bucket GPU timestamps (the span after the world
  passes split into the marker pass, the world target's end, the composite with the UI, and the
  frame's close), the shipped play set, the north-west corner at 1024 × 768, `--maxfps 0`, mirror
  and black alternated, no other game on the GPU. At 0.25×, nine reports a side: frame p99
  3.94–5.60 ms with the mirror and 3.08–5.21 under black, p50 0.54–1.27 and 0.65–0.82; the tail
  on both sides in the world target's end (p99 1.92–3.85 and 1.64–3.62) and the frame's close
  (up to 2.36 and 3.58); the mirror's own two buckets 0.03 and 0.07 ms at p99. At 1×, six reports
  a side: frame p99 2.52–5.36 and 2.66–4.21. The earlier rounds' tail-free black did not
  reproduce, and the tail moves between reports on both sides by more than the mirror costs
  [INFERRED: the GPU's own clock and queue state at an uncapped frame under a millisecond; the
  desktop's own clients still shared the GPU]. `mirror_gather` on the CPU at this size: 37 µs
  p50, 73 µs p99, 345 mirrored anchors.

**MEASURED 2026-09-25, the fold clips and the nudge's reach** — the same setup, Classic, one
launch a mode:

- **The fold samples fall on the ground's side.** The feature pass over the terrain (the capture
  diagnostic), mirror against black, at 0.25× from eyes whose parity puts a whole sample column
  or row on a fold: (−447, −351) (x = 0 and y = 0), (−447, 2001) (x = 0), (10303, 6001) (x = W),
  (3001, −351) (y = 0, under the map's top-row trees) and (4001, 12349) (y = H). On the map, by
  the ground's own partition (`w + e` inside it), the mirror equals black at every sample —
  632 385 to 1 349 376 a view, the fold samples on the map's side among them (1 601, 1 408 and
  1 792 at x = 0 and y = 0, 38 of them under a sprite). On the x = W fold column the map's own
  frames overhang at 50 samples (translucent shadows); the build before this one drew them over
  the mirrored ground at all 50 (`M = B + ground/2`, within a level), this one at none — its copy
  draws there, as on the ground. Past that column the map's frames overhang at 56 more samples,
  where both builds draw the copy; 2 of them share the map frame's value by chance (a grey, the
  neighbours not), in both builds alike. No sprite reaches the
  y = H row on Two Continents (the copies below stand 20 px clear of it), so that clip is shown
  by the ground's side of it alone. 1× and 0.5× at the north-west corner: 0 samples differ from
  the build before, mirror or black.
- **The nudge at a wheel level, counted against a prediction.** The feature pass alone under
  `edge=black`, this build against main's DLL at the weapon IDs, one notch out (0.877×): a sample
  whose `frac(w) ≥ 1 − e` on an axis should read what main reads with the eye one world px on
  along it, so the prediction is main's own capture at the eye moved by (1, 0), (0, 1) or (1, 1).
  At (3, 3), 2 472 956 of the viewport's 2 523 136 samples lie more than two sub-pixel steps
  (1/256 of a framebuffer px) from both texel decisions; there 23 009 changed, 23 009 were
  predicted and 0 differ from the prediction. The other 50 180, in the tie columns and rows whose
  point lies within that snap of `1 − e` (18 columns, 14 rows), change or not as the rounding
  gives: 932 differ from the prediction. The same at (1203, 2003) (13 034 decided, 0 off; 650 in
  the snap band), at ten notches out (0.270×: 11 660, 0 off; 507 in a band) and one notch in
  (1.163×: 21 361 and 10 335 decided, 0 off, and 0 in the band either). The shares of tie
  columns and rows are the arithmetic's: 3.96 % and 4.05 % at 0.877×.
- **The mirror moves a borderline sample of the map's own sprite at a wheel level.** At the
  north-west corner at 0.877×, mirror against black, 10 on-map samples differ, all in one
  column whose point lies 0.0006 world px past `1 − e` — inside the 0.0022 of the snap — where
  the texel read follows the frame's place in the atlas, and the mirror's own frames move places
  (two mirror launches of two builds differ at 19 samples of that column). See *What it does not
  do*.

**What it does not do.**

- **A translucent shadow past the edge can round two levels from its reflection.** The world
  target is linear RGBA8, so every blend into it rounds to 8 bits. On the map a shadow blends over
  the ground and the reflection is that pixel toned; past the edge each layer is toned first and
  then blends. The two orders agree within a level almost everywhere, not everywhere. MEASURED
  with the feature pass's bodies not drawn, Two Continents' right side from (10304, 6000): 2 of
  1 261 412 samples two levels off at 0.25× and 8 of 1 261 560 at 1× (none on the left side at
  0.25×), and 0 at all three with the same shadows drawn opaque — the texels, the layers and their
  order are the map's; the arithmetic is not. With the bodies drawn, 1 of 1 261 410 at 0.25× and
  none at 1×. An exact strip needs the tone applied to the map's own finished pixels, or a world
  target wider than 8 bits, which would change the rounding of every blend on the map as well.
- **A sample within the rasteriser's snap of a decision is the rounding's, at a wheel level.**
  The nudge moves every texel decision `e` off the sample lattice of 0.25×, 0.5×, 1× and 8×; at
  the wheel's other levels a column or row can still lie within one sub-pixel step of the nudged
  boundary (about 1 % of the columns at 0.877×). Such a sample reads either texel as the frame's
  place in the atlas rounds it, and the mirror's own frames move places, so turning the mirror on
  can change it on the map (10 samples of one column, measured above). The same holds for a fold
  line: a sample within the snap of it can take the ground's side in one pass and the other in
  the clip — one column or row along the fold, not observed. Deciding both from the sample's
  screen position against one shared expression (`gl_FragCoord` with `precise`, the texel by
  `texelFetch` from the frame's integer origin) would make them exact.
- **A 3D feature the map placed — a hulk — is not mirrored** (the owner's ruling). It takes a record
  of the wreck pool, which moves with play; the list skips it rather than freeze a copy.
- **The schema's own features are not mirrored — the rule, not a gap** (the owner's ruling: the
  mirror's map is the map file). A map's `.ota` can place features of its own (`0x423160` through
  `0x4224B0`/`0x423C50`, after the TNT's, on a new game only: one on Steel Jungle, eight on King of
  the Hill); they are not the map file's, and a TNT feature one of them destroys stays mirrored.
- **A map with more than 65 536 features** is mirrored with its first 65 536 in row order.
- **Under the GDI renderer nothing is built**, since nothing would draw it.
- **The mirror carries no fog of war, no light and no units** — by the owner's call, as in the lab.
- **Units and 3D wrecks are not clipped or dropped at the area.** Only the terrain and the GAF
  features end at the extent; the unit pass draws whatever stands in the hidden strip. Units can
  stand in part of it: the load mask leaves the bottom strip buildable on raised ground (above),
  so a structure there is drawn over the mirror.
- **A map whose 16-px dimensions are odd folds a tile short.** `tagpu_edge_area` rounds the
  extent down to a whole tile, so on such a map (no stock one: all 275 TNTs are even) the last
  16 px of the map that stock shows are drawn as mirror.
- **The Classic++ restorer still sees the hidden strip.** Its neighbourhood keys (`nb_build`)
  are built over the whole tile map, so the ring of the area's last column and row is read from
  the hidden column's and rows' texels, which the mirror no longer draws beside them.
- **The lab and the wiki mockup set the cut's uniforms.** Both run this pass's fragment stage,
  extracted from `tagpu_feat.c`, through `tascene-render.js`'s `classicSprites`, whose `use()`
  requires `uMapPx` and takes `uClip` as the pass publishes them: the lab the area × 32 and
  `clip` = the mirror, the mockup the stage's tile map × 32 and 0 (the stage's cells are even, so
  that is its target's own size and the cut discards no sample inside it). Left unset, `uMapPx`
  reads (0, 0) and the black-edge cut discards every sample of the map's own features.
- **The lab's black edge is not the game's.** The lab clamps the feature sweep to the area, and
  its own sprites carry the anchor's drawn point as their world point, not each pixel's, so the
  fragment-stage cut keeps or drops a whole sprite: under a black edge a tree anchored in the
  hidden strip but drawn above the line is drawn in the game and not in the lab, and an on-map
  tree's overhang past the line is cut in the game and not in the lab.
- **Under a black edge the cut holds while the engine draws the ground.** The feature pass cuts
  the map's own features at the area whenever the mirror is off, including frames whose terrain
  the engine draws (the terrain pass handed back: its watchdog, `passive`). At zoom below 1 the
  engine's pass then draws the hidden strip's ground and the trees standing on it are cut.
- **A map whose area is under 8 rows tall** (`MF_LIFT_ROWS`; no stock map) misses the mirror
  copies of a source two bands away; no index is affected.
- **Under the debug `Edge` command** the extent can name less than the tile map or more; the
  camera's far end takes the smaller of the extent and the tile map, and `tagpu_edge_area` answers
  the whole tile map for an extent past it.

**Risky spots for a review.** The LoadMap stub: its stolen bytes, the frame offsets it reads
through `regs + 8`, and that every path joins there; `mapfeat_at_load`'s emulation of the spawn
and destroy rules (the anchor offsets, the refusal that keeps its earlier destroys, the pool
count) and its bounds (the dimensions, the def count, the version, the 65 536 cap); the
hand-over's lock (taken by a full-barrier exchange, released by a release store), the level
stamp and who frees what; `mapfeat_ok`, the empty list, and
`mapfeat_take`'s bounds and bucketing; `want` before the early returns and `feat_emits` against
the gather's own mute; `tagpu_feat_mapfeat_sync` and its caller in `tagpu_native.c`; the terrain
flags' spare-bit bound, the fold flags and the vertex stage's decode and nudge; the gather's floor
origin (`div32_floor`) and what else reads `s_rectTx0`; the fold for negative and far distances;
`mf_keep`'s order test and `mirror_gather`'s runs (`floor_div`, the copy's parity, copy 0 skipped on an
on-map row, a turned copy walked in its source's order and broken off at the run's low column);
the turned window `tuc0` against the engine's 10-left, 2-right sweep, and the view cull's copy
test; `tagpu_edge_area`'s bound (`[1, cells32]`) and that the terrain, the feature pass and
`camera_range` all read the packet's `map_pxw/h`; the bands (`floor_div(row, H)`, the neighbour
within `MF_LIFT_ROWS`, the parity tests that keep a pair from naming one source row twice) and the
drawn-position test that both the anchor loop and `mirror_feature` make; `frame_crosses` against
`emit_frame`'s placement of shadow, body and sub-frames; `map_key` as the one computation of the map's keys (the anchor loop calls it, and it
must stay bit-identical to main's inline arithmetic under x87 excess precision), and
`mirror_key`'s source-column clamp and its off-row flat bands against the unit, wreck and effects
keys; the mode's copy field (`TAGPU_FEAT_COPY_*`, bounded to `[−1024, 1024)` so the float
attribute is exact) and the fragment stage's per-copy discard and the map's own clip (`uClip`: the map's columns under the
mirror, the area's right and bottom under black), each testing `s = vWorld + e`; `uClip` at 40 and `uNudgeW` at 44 in the 48-byte block, `uClip` set only while
the gather mirrors; the tie rule's nudge (`TAGPU_EDGE_NUDGE_PX / zoom`, `+x` on a turned quad) and
the world corners taken after it; `mapfeat_unpark` at every
join, under the lock, before the build's first check; the table's place in the fill and cut order
and the reserve's assert.
