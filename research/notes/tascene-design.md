# tascene — a browser lab for rendering ideas, on real game assets

*Outcome of the `/grill-me` interview, 2026-09-03. Goal: put a real TA map with real units on
screen in a browser in seconds, so a rendering idea can be tried, compared against the game's
own frame, and — when it wins — moved into `tagpu` as the same GLSL it was prototyped in.
Companion references: `file-formats.md` §1/§3/§5/§6 (the formats), `terrain-depth.md` §1–§2
(what the engine builds from a TNT and how it draws it), `model-export.md` (the `ta3do`
pipeline this reuses), `scenario-format.md` (the situation files it consumes),
`undither.md` (the restorer), `roadmap.md` (where a winning prototype goes).*

Facts tagged `[VERIFIED]` were measured this session against the stock archives; the numbers
are quoted so they can be re-checked. `[CLAIMED]` = asserted but not yet run.

---

## The one-paragraph summary

`tools/tascene` compiles a **scenario JSON** — the same file `tacli scenario load` runs in the
real game — into a **scene pack**: the map's tile atlas, tile index map, heightmap, feature
anchors and unit meshes, all read straight out of the game's own archives with no game running
and nothing unpacked to disk. `tools/tascene-view.html` draws that pack in WebGL2 in one of two
lanes: the **parity lane** uses **`tagpu`'s own shaders**, extracted from the C sources at
export time so they cannot drift, and is diffable against a real game frame; the **exploration
lane** uses the page's own lab shaders for relief, restored colour and a real sun, and makes no
parity claim. Every setting is a URL query parameter, so a look is a link, the same string
drives a headless screenshot, and `?a=…&b=…&wipe=…` puts two looks in one frame at identical
pixel coordinates. `tascene ab` runs both sides of the same scenario — engine and browser — and
diffs them.

## Two lanes, and the reason there are two

The lab has one job the game cannot do (try a look) and one job that keeps it honest (prove the
export is correct). They pull in opposite directions, so they are separate modes, not a blend.

| | **Parity lane** | **Exploration lane** |
|---|---|---|
| terrain | flat quads on the engine's grid | heightfield relief from `mapattr` |
| textures | R8 indexed — the texel *is* the palette index | undithered RGB |
| shading | the engine's `PALETTE.SHD` LUT, in index space | N·L sun over real normals |
| camera | oblique, shear 0.5 | oblique, shear 0.5 (the same one — relief needs no new camera) |
| claim it can make | **pixel-diffable against the game** | "this is what it could look like" |

The parity lane is the **calibration**. A subtly wrong tile stride, height scale or sort key
produces a picture that looks entirely plausible — this codebase's characteristic failure —
and the diff against a real `glshot` is the only oracle that catches it offline. So it lands
first, and the exploration lane is built on ground already proven.

## Decisions locked

| Branch | Decision |
|---|---|
| Purpose | **Port-faithful lab, not an art sandbox.** A shader that wins in the browser is a diff to `tagpu_glsl.h`, not a reimplementation. GLSL 330 core → GLSL ES 300 is `#version` plus three `precision` lines; nothing else in the existing shaders changes. |
| Scene source | **Offline, from a scenario JSON.** No game needed, deterministic, checked in — and the *same* file runs in the real game, which is what makes the A/B free. A live snapshot writer may fill the fields offline leaves null (COB poses, the fog grid) later, into the same pack schema. |
| Terrain | **Both flat and relief**, one uniform apart. Flat reproduces `0x483FA0`'s grid blit and is diffable; relief displaces a vertex per 16-px cell from the heightmap. |
| Camera | **The engine's oblique projection is the default and the only mode carrying parity claims.** TA's `screenY = worldZ − h/2` is a *shear of 0.5*, not a rotation (a rotated ortho would need `cos θ = 1` and `sin θ = 0.5` at once) — so a heightfield under that same shear stays consistent with every engine rule: units still anchor at `worldZ − h/2`, features still sort by 16-px row. A free orbit to inspect geometry was proposed and is **not built** — landing 2 did not need one, because the heightfield renders under the same shear. |
| Textures | **Dithered and undithered are a viewer toggle**, and they are two whole shading philosophies, not two files: indexed keeps the SHD LUT and stays diffable; undithered drops the LUT and lights with a real sun. Reconciling them (carrying the index alongside the colour and ramp-sampling) was considered and deliberately **not** taken. |
| Units | **Stock 3DO bind pose + the `gamedir/hires/<name>` replacement slot**, switchable per unit type — so the lab is also the authoring loop for G11's replacement pipeline. `ta3do`'s `Create`-`HIDE` scan keeps muzzle flares out, as in `render`. **No COB VM**: a second interpreter to keep correct, when the deferred live-snapshot path would give real poses far more cheaply. |
| Shader sharing | **One-way extraction at export time.** `tascene` parses `static const char* VS/FS =` out of `tagpu_terr.c` / `tagpu_native.c` / `tagpu_feat.c`, expands the `TAGPU_GLSL_*` macros from `tagpu_glsl.h`, swaps the version header, and writes real `.glsl` into the pack. `tagpu` stays authoritative and the browser is provably never stale. Lab-only shaders live outside the pack, checked in; `pack/` is generated and gitignored. |
| Tool layout | **New `tools/tascene`, importing `tools/ta3do` via `SourceFileLoader`** — the idiom `tools/test_ta3do.py` already uses. `ta3do` keeps its name and its meaning (one model, standard views); `tascene` owns maps, scenes and the lab. No refactor of a tested 1824-line tool. |
| Options | **Every knob is a URL query parameter** (the `ta3do-view.html` house idiom, including `?shot=1` → hide UI, one frame, stamp `document.title`). A look is a link; the headless shooter takes the same string. **The wipe** — `?a=<query>&b=<query>&wipe=<0..1>`: two looks rendered into two offscreens, and the canvas split between them at `wipe`, so two parameter sets are read **at identical pixel coordinates**. Drag the divider when serving; pass a fixed `wipe=` to `shot`. Named looks live in a checked-in `tools/tascene-presets.json`, staged next to the viewer. **Both landed in landing 2.** |
| Pack encoding | **Raw `.bin` for anything whose bytes are semantic; PNG only for display RGB.** Browsers colour-manage and premultiply PNGs — in the parity lane the atlas texel *is* a palette index, so an image decode path would silently rewrite the data and the diff would measure nothing. Raw blobs go straight to `texImage2D`, byte-exact by construction. |
| Map extent | **Whole map, no windowing or streaming.** Worst stock case (Two Continents) is a 2176×2720 R8 atlas, a 336×400 `u16` tilemap, a 672×800 R8 heightmap and ~5000 feature anchors; the relief mesh is 537k verts. Trivial for WebGL2. |
| A/B | **One verb drives both sides.** `tascene ab <scenario>` loads the scenario in a real instance, holds the eye, `glshot`s, reads the live eye/viewport back through `roster`, builds the pack with those exact numbers, shoots the browser at the same resolution, diffs, and writes an `sbs.py` panel plus a mismatched-pixel count. It degrades to build+shot with no instance running. A comparison that only happens when someone remembers is a comparison that does not happen. |
| v1 | **Parity lane first**, exploration second. See "Landing plan". |

---

## What a `.TNT` gives us — the whole scene, offline  [VERIFIED]

The on-disk layout was only `[CLAIMED]` in `file-formats.md` §6 before this session. It is now
verified; the field table lives there (§6). What matters for the lab:

| Block | Granularity | What the lab does with it |
|---|---|---|
| `PTRmapdata` | `u16` per **32-px** cell, row-major, stride `Width/2` | one quad per cell, indexing the tile atlas — the flat lane |
| `PTRmapattr` | 4 B per **16-px** cell, row-major, stride `Width` | **byte 0 = height 0–255** (relief, and the `−h/2` anchor); **bytes 1–2 = `u16` feature index**, `0xFFFF` = none |
| `PTRtilegfx` | `tiles` × 1024 B | the 32×32 8-bpp tiles → one `GL_R8` atlas, 64 per row, exactly as `tagpu_terr.c` builds it — **including its `CELL_PITCH` guard border**, see "The seam that came back" |
| `PTRtileanim` | `tileanims` × 132 B (`i32` + name) | **the feature name table** the `mapattr` index resolves against — `Tree1`, `RockMetal2`, `DryRuin10` |
| `PTRminimap` | `i32 w`, `i32 h`, then `w·h` 8-bpp | the map picture (`TED_GENERATED_PIC`) |
| `sealevel` | byte | the waterline |

The feature chain closes entirely offline:
`mapattr` index → TNT name table (`Tree1`) → `features/green/trees.tdf` `[Tree1]` →
`filename=trees; seqname=leaf1; seqnameshad=leafshad; height=40`. That `height` is precisely
G13a's tall-vs-flat sort test (`Height < 10` = flat band), so trees, rocks, their shadows *and
their sort class* all come out of the export with no game running.

The engine's lighting tables are on disk too, so the parity lane needs no live game to shade:
`palettes/palette.shd` is **8192 B = the real 32×256 shade table** (row 15 is the near-identity
neutral row, 232/256 entries) — the same table `tagpu_render3do.c` prefers to read live from
`*(TAProgram+0xC4)`. `palette.alp` (256×256 blend) and `palette.lht` (32×256 light) are there
as well, which puts feature shadows and the LHT flash within reach at true parity.

### How that was established  [VERIFIED, this session]

- **Layout, 275/275 stock maps.** Every block offset predicted from the previous block's size
  and the header's counts, with no slack: `mapattr = 64 + pad16(mapdata)`,
  `tilegfx = mapattr + W·H·4`, `tileanim = tilegfx + tiles·1024`,
  `minimap = tileanim + tileanims·132`, `filesize = minimap + 8 + w·h`. All 275 exact; all are
  version `0x2000`.
- **Row order and stride, cross-validated between two independent blocks.** Correlating tile
  brightness (from `mapdata`) against `height > sealevel` (from `mapattr`) gives **−0.484** on
  Two Continents with the correct strides versus **−0.018** transposed — a 27× discrimination —
  and **+0.606** on Gods of War. Maps with `sealevel` 0–1 (Painted Desert, A Gentle Time) score
  0.000 because every cell is land: the test is *undefined* there, not failing. A separate check
  against the stored minimap agrees where it has power (Painted Desert **+0.885** row-major vs
  **+0.015** transposed); it is uninformative on ocean-dominated maps, whose average-colour
  thumbnails are too uniform to correlate.
- **Heights are real relief.** Two Continents 0→189 with `sealevel` 75; Painted Desert 0→140 but
  mostly 1–4 with `sealevel` 0 — which independently corroborates G13a's finding that Painted
  Desert has no tall features.
- **Undithering terrain per-tile is safe.** A tile undithered *in isolation* differs from the
  same tile undithered inside a 3×3 patch of its real map neighbours by **2.03/255 mean on the
  edge ring, 0.45/255 interior, max 7** — invisible. So no neighbour-context machinery and no
  whole-map assembly: undither the tiles.
- **And it is cheap.** 19 ms/tile through the `learned` preset ⇒ the largest stock map's 5062
  tiles in **~1.6 min**, cached per (map, preset).

---

## The pack

Generated, disposable. One directory per scene; this is what `build` writes.

```
pack/
  scene.json               manifest: map, view, palette calibration, depth keys,
                           feature defs + instance count, unit meshes + instances
  terrain/atlas.r8.bin     2176 x (ceil(tiles/64)*34) palette indices, 64 tiles/row,
                           34-texel CELL_PITCH with a replicated guard border
  terrain/tilemap.u16.bin  u16 tile index per 32-px cell, stride w16/2
  terrain/height.r8.bin    one byte per 16-px cell, straight from mapattr
  palette/pal.bin          256 x RGBA      palette/shd.bin  32 x 256 shade table
  features/atlas.r8.bin    every GAF sprite the map's features name, shelf-packed
  features/instances.bin   u16 col, u16 row, u16 def -- one per anchor, the map's
                           own and then the scenario's GAF features at pos>>4
  units/atlas.r8.bin       the 3DO textures, shelf-packed with a replicated
                           4-texel border per frame, allocations aligned to 4
                           (`pad` in the manifest) so the restored copy can be
                           mipmapped to level 2 without bleeding a neighbour
  units/<type>.bin         bind-pose mesh: x,y,z, u,v, flatColour, colourKey --
                           scenario units AND its 3DO features (every wreck is
                           `object=<name>_dead` in features/*.tdf): same
                           models, same path, the instance tagged `feature`
  shaders/terrain.*        extracted from tagpu_terr.c
  shaders/sprite.*         extracted from tagpu_feat.c
  shaders/unit.*           extracted from tagpu_native.c

  --undither only, for the exploration lane, same layout as the R8 beside it:
  terrain/atlas.rgba.bin   features/atlas.rgba.bin   units/atlas.rgba.bin
```

`serve` and `shot` also stage two **checked-in** files into the pack directory so
the page can fetch them same-origin: `tascene-view.html` itself and
`tascene-presets.json`. Neither is pack content — `pack/` is generated and
disposable, and nothing there may be the only copy of anything.

Raw `.bin` for everything whose bytes are data, as decided: the atlases go
straight to `texImage2D` with no image decode path, so nothing colour-manages
or premultiplies a palette index.

## Using it

### The two-minute recipe

```bash
tools/tascene build scenarios/tascene-parity.json -o /tmp/pack   # ~7 s, no game needed
tools/tascene serve /tmp/pack --port 8899                        # then open the URL it prints
```

`serve` prints `http://127.0.0.1:<port>/tascene-view.html`. In the page: **drag** or the
**arrow keys** pan the eye (shift = 256 px steps), and the bar along the bottom reports the
map, the live eye, the grid the engine's algorithm produced, the sub-tile scroll fraction,
the visible cell count, off-map cells, features and unit triangles.

### The verbs

| | |
|---|---|
| `build <scenario.json> -o <dir>` | compile a pack from the archives. `--map` / `--res` override `setup.map` / `setup.res`; `--eye X,Y` sets the viewport's top-left in world units (default: derived from the scenario's `camera`); `--no-features` and `--no-units` cut the pack down — `--no-features` is also how you get a terrain-only diff; **`--undither [PRESET]`** also packs restored true-colour atlases for the exploration lane (`--undither-python`, `--cache-dir`); `--json` for agents |
| `serve <pack>` | the lab on loopback. `--port` (default: an ephemeral one) |
| `shot <pack> -o <png>` | one deterministic headless frame. `--opts '<query>'` passes the viewer parameters below; `--timeout`, `--budget` (Chrome's `--virtual-time-budget`, ms); `--json` |
| `artlight` | **is the map art already painted lit?** `--map <name>` or `--all`; `--steep` (default 0.25 ≈ 14°), `--elevation` (default 53.1, the engine's own), `--json` for the whole azimuth curve. Needs no pack and no game — it reads the TNT. See "Does the art already contain the hill" below |
| `ab <scenario.json>` | drive both sides and diff. `--name` the instance (default `tascene`), `--no-launch` to use one already running, `--eye X,Y` to pin the camera, `--los`/`--mapping` for the SKIRMISH fog toggles (defaults `0`/`1` = no fog), `--settle` seconds to wait for a roster with units and a real eye, `--opts`, `--launch-timeout`, `--json` |

### Viewer query parameters

The page's whole state is the query string — that is the point: a look is a link, and `shot
--opts` takes the same string.

| Parameter | Meaning |
|---|---|
| `shot=1` | hide the UI, render exactly one frame, then set `document.title` to `tascene-ready` so a screenshotter knows it is done |
| `pass=<list>` | comma-separated passes to draw: `terrain`, `features`, `units`. Default: every pass the pack carries. `pass=terrain` is the terrain-only render the parity split uses |
| `eye=<x,y>` | override the pack's eye (viewport top-left, world units) |
| `ss=<n>` | supersample factor for the offscreen. **Default 2, the game's own**: `tagpu_native.c` renders its native unit FBO at 2× and box-downsamples (`tagpu_ss.off` turns it off), so at `ss=1` the unit textures alias where the game's do not — the kbot lab's roof shimmered in both lanes until the default followed the game. The composite is an exact n×n `texelFetch` box for any n (a single bilinear tap, which it was, is only a box at n = 2). Terrain and sprites are 1:1 texel-to-pixel and are **0 differing pixels** at any factor; on the base fixture `ss=2` changes 12 883 parity pixels, all units, and in the exploration lane 19 939 more by ±2, the per-pixel lambert averaged over its sub-samples |
| `feat=<what>` | features: `both` (default), `body`, `shadow`. A debug split, because "is the shadow drawing at all" is not eye-answerable — it was 152 133 differing pixels, i.e. yes |
| `preset=<name>` | a named look from `tascene-presets.json`; anything else you spell out wins over it. A preset may not name another preset |
| `a=<query>` `b=<query>` `wipe=<0..1>` | **the wipe** — see below |
| `lane=<lane>` | `parity` (default) or `explore`. Naming any exploration parameter selects `explore` on its own; naming one *and* `lane=parity` is an error rather than a silent winner, because the lanes do not blend |

### Exploration-lane parameters

These exist only in `lane=explore`, and they are what landing 2 added.

| Parameter | Meaning |
|---|---|
| `relief=<k>` | displacement scale, **default 0** (`dd5e739`): the art stays exactly where it is painted and is still lit by the real gradient. **1 = the engine's own `h/2`**, which misregisters the art — the tiles are already the oblique projection of the hill, so displacing them applies it twice; measured on Two Continents it moves the terrain 41–94 px off the engine's frame while the features do not move at all. Kept as the double-count experiment |
| `datum=<h>` | the height a non-zero `relief` pivots about (`8b73f50`): a cell moves by `relief·(h − datum)/2`, so a cell at the datum stays where the engine paints it. Default the map's own sealevel, which removes the constant part of the slide (66 px of it on Two Continents) and cannot remove the part that varies with the terrain — which is why `relief` itself defaults to 0 |
| `sun=<az,el>` | the sun in degrees, or `off`. The default **`324.5,53.1` is `tagpu_render3do.c:250`'s own model light** `SH_L = {-0.35f, 0.80f, -0.49f}` re-expressed as a direction (it round-trips to within **0.0013 per component**), and the lab dots it against the same raw model-space face normal the engine's LUT path uses — so switching lanes changes the shading *model* (32 `PALETTE.SHD` rows → continuous lambert) and not the light. **Level ground always takes exactly 1.0** — see "Level ground takes exactly 1.0" under landing 2 |
| `amb=<a>` | ambient floor, default `0.35`, **relative to level ground**: the shading is `(amb + (1−amb)·max(N·L, 0)) / (amb + (1−amb)·sin(el))` for terrain, unit faces and feature sprites alike, so a face turned fully away from the sun takes `0.40` at the defaults |
| `slope=<k>` | exaggerate the heightfield's gradient before normalising, default 1 |
| `filter=<f>` | how the exploration lane samples the **restored unit textures**. `linear` = trilinear (bilinear + mipmaps, levels 0–2) with `aniso` taps — what the game's own hires path does (`tagpu_hires.c:1142`, `GL_LINEAR_MIPMAP_LINEAR`); `nearest` = one texel, the stock look. **Default `linear` when the pack's unit atlas is padded** (`pad` ≥ 4 in the manifest, every `build` since 2026-09-03), `nearest` otherwise; `filter=linear` on an unpadded pack fails loudly because its mips would bleed. Indexed colour is always nearest — an index cannot be averaged. Measured on the base fixture: 13 740 pixels differ from `nearest`, all units |
| `aniso=<n>` | anisotropic taps for `filter=linear`, default 4. TA's projection `(x, −z − y/2)` foreshortens every vertical face 2:1, which trilinear alone over-blurs along that axis. 0 or 1 = off. The status bar reports what the GPU allows (`none` = unsupported); the headless shooter honours it too — `aniso=4` vs `aniso=0` is 10 099 differing pixels there, and the live RTX 4070 reports 16× |
| `undither=<b>` | `1` = the pack's restored atlases, `0` = the palette indices. **Default: restored when the pack carries them** (`build --undither`), indexed otherwise — so the lane's defaults still reduce to parity on an indexed pack, and show the colour a restored pack was built for. `undither=1` on a pack built without `--undither` says so instead of drawing something plausible |

### The wipe

`?a=<query>&b=<query>&wipe=<0..1>` renders **two whole looks into two offscreens**
and splits the canvas between them at `wipe`, so two parameter sets are read at
**identical pixel coordinates** — which is the only way to see a 3 % change in a
picture nobody has a reference for. A side's value with no `=` in it names a
preset. `shot` takes the same string; when serving, drag the divider.

```bash
# what the restorer buys, on the same pixels
tools/tascene shot pack -o w.png --opts 'a=engine&b=restored&wipe=0.5'
# the double count: art lit by the real gradient, against art ALSO displaced
tools/tascene shot pack -o d.png --opts 'a=slope&b=relief-sun&wipe=0.5'
```

`eye`, `ss` and `shot` are global to the frame — a wipe with two eyes would not
be a wipe. Everything else is per side: the top-level query is the base and each
side's own query overrides it, so `?eye=X,Y&a=engine&b=relief` shares the camera
and differs only in the look.

### The presets  (`tools/tascene-presets.json`)

| Name | What it is for |
|---|---|
| `engine` | the parity lane — the only look carrying a pixel claim |
| `slope` | art not displaced, lit by the real gradient: the double-count **control** |
| `relief` | displaced at the engine's `h/2`, unlit: the geometry alone |
| `relief-sun` | both — where the art's baked-in shading double-counts |
| `half-relief` | `relief=0.5`, the middle of that axis |
| `restored` | the undithered atlases and nothing else changed |
| `restored-relief` | everything the exploration lane has |

`serve` and `shot` reuse `ta3do`'s existing `Viewer` (`ThreadingHTTPServer`) and `shoot`
(`--headless=new` Chrome on a private X display, ANGLE/SwiftShader) rather than growing a
second copy.

### One rule for anyone editing the viewer

**The coordinate convention is tagpu's, deliberately.** The extracted vertex shader maps game
`py` 0 to NDC −1, so the offscreen's row 0 is the game frame's *top* row and `gl_FragCoord.xy`
*is* the game-frame pixel — which is what the fog and scaffold rules in `tagpu_glsl.h` assume.
The page renders into an FBO exactly as tagpu does and flips only in the final composite to the
canvas. Do not "fix" the flip in the vertex shader: it would silently invert every screen-space
rule the shaders carry.

---

## Gaps this design does not close  [state them, don't paper over them]

- **Animated water.** The engine animates water by cycling palette entries at runtime;
  `tagpu_terr.c` gets it free because it re-uploads the live palette every frame. Which indices
  cycle, and at what rate, is **not established** — and is not in the TNT. Offline water will be
  static until that is found (or until the live-snapshot path supplies the palette per frame).
- **No posed units.** Bind pose only. Anything about walking, aiming, recoil or sub-pixel motion
  cannot be judged in the lab as designed.
- **No fog.** G13c proved the fog shape is the engine's view-anchored corner-mask grid and
  is *not* derivable from the LOS/MAPPED source maps, so an offline pack cannot reproduce it.
  The parity diff must therefore be taken on a scene with fog off, or the fogged region masked.
- ~~**The art already contains the hill.**~~ **Measured in landing 2 — see "Does the art
  already contain the hill" below.** It does, the amount is per-map, and each map's artist
  chose a different sun. The gap is now a number rather than a worry.
- **Minimap letterboxing convention** (how a non-square map maps into the stored picture) was
  not determined — the corner pixel is map content, not padding, on all three maps sampled.
- **Hires `.glb` parsing** leans on three.js's `GLTFLoader` in parse-only mode (read the
  attribute arrays, draw with our own code). Arbitrary exporter output may exceed what that
  path handles; the constrained subset `ta3do` itself writes is the tested case. [CLAIMED]

## Landing plan

1. **Landing 1 — the parity lane.** ● **done, A/B'd** (2026-09-03) — numbers below.
2. **Landing 2 — the exploration lane.** ● **done** (2026-09-03) — relief
   displacement, undithered atlases with the restorer cache, N·L sun, the wipe,
   `presets.json`. Numbers below.
3. **Then, and only then**, a winning prototype becomes a roadmap gate with its
   GLSL carried over verbatim. **Not started**, and landing 2 does not nominate
   one: what it produced is a measurement (below) that says a single sun cannot
   be right for both the terrain art and the units on most maps.

### What landing 1 actually built

`tools/tascene` (build / serve / shot / ab) and `tools/tascene-view.html`:

- **TNT and TDF parsers**, the TNT one self-checking every block offset against
  the header's counts on load — the same arithmetic that verified the format, so
  a misread file is an error and not a plausible-looking picture.
- **Terrain pass** reproducing `tagpu_terr.c`'s emit loop line for line,
  including the engine's truncating `sar 5` and its `ceil` column count.
- **Feature pass** reproducing `tagpu_feat.c`'s sweep rect, its projection
  (`+128/+32` baked in, `−(h00+h01+h10+h11)>>3` for the half-height shift), its
  flat/tall depth-key split, and its shadow-with-depth-writes-off rule.
- **Unit pass**: 3DO bind pose with `Create`-hidden pieces dropped, the
  quad-corner UV rule, per-face shading through the engine's real `PALETTE.SHD`
  with `tagpu_render3do.c`'s own neutral/direction calibration (measured: neutral
  row **15**, direction **+1**).
- **Shader extraction** from the three C sources, macros expanded, `#version`
  swapped. All three passes run tagpu's GLSL, not a copy of it.
- **`tascene ab`** — drives `tacli scenario load` / `eye` / `roster` / `glshot`,
  rebuilds the pack at the live eye and resolution, shoots the browser headless
  and diffs inside the viewport rect.
- **`scenarios/tascene-parity.json`** — the fog-off fixture (`--los 0
  --mapping 1`), fixed camera, two still units, nothing that moves or burns.
- **`scenarios/tascene-base.json`** — the *lighting* fixture (2026-09-03): a
  small ARM base on the open grass at eye (5888, 11840), chosen by scanning the
  map for the land-only 64×48-cell window with the fewest feature anchors (35;
  the flat east candidate had 40 and no relief). A hill at the top-left of the
  frame for slopes, a kbot lab, two extractors, a laser tower, Peewees and
  Flashes at facings that are not multiples of 90, and wrecks on the hill's
  foot. `build` only placed the map's own features until this fixture needed
  wrecks: a scenario feature with a GAF `seqname` now joins the anchor list,
  and one whose def names a 3DO `object` rides the unit mesh path posed by its
  facing — the engine draws it with the same 3DO renderer. Not A/B'd against
  the game yet: the wrecks' depth keys are a unit's, where the engine sorts
  them as features.

Cross-checks that passed without a game running: the unit exporter produces
**189 triangles for ARMCOM, the same count `ta3do render` reports**, and the
gold shoulder panel that looked like a bug is in `ta3do`'s own quarter view too.

### The A/B, measured  [VERIFIED 2026-09-03]

`tascene ab scenarios/tascene-parity.json` against a live instance on Two
Continents at 1024×768, engine eye **(2320,720)** read back from the roster,
`--los 0 --mapping 1` (no fog). Engine GL framebuffer vs the browser, inside the
896×704 viewport rect:

| Region | Pixels | Differing | |
|---|---|---|---|
| whole viewport | 630 784 | 75 672 | 12.00 %, mean abs **0.91/255** |
| **where we drew only terrain** | 354 744 | **284** | **0.080 %** |
| where we drew a sprite or unit | 276 040 | 75 388 | 27.31 % |

**88.00 % of the whole viewport is bit-exact**, and only 964 pixels (0.15 %)
differ by more than 40 on any channel.

The terrain residue is not terrain. **232 of those 284 pixels (81.7 %) are the
mouse cursor** at (512,384), which the engine draws into the frame and we do
not; the other 52 are scattered single pixels on feature-sprite edges. So the
terrain pass reproduces `0x483FA0` at **52 / 354 744 = 0.015 %** — consistent
with G13b's own "0 differing pixels" for the same blit.

The sprite residue is **one known divergence, inherited rather than introduced**:
tree *bodies* are pixel-identical, and the difference is the shadow. The engine
remaps shadow pixels in palette space through `PALETTE.ALP`; we do tagpu's
`frag = vec4(rgb * 0.5, 0.5)` premultiplied blend, so it is smooth where the
engine dithers. G13a already recorded exactly this gap for tagpu's own feature
pass. `palettes/palette.alp` is on disk (256×256) and shipping it in the pack
would close it — not attempted here.

Two unit-level gaps the A/B exposed, neither a placement error:

- **The anchor is exact.** Computed screen anchor (512, 384) equals the engine's
  own `roster screen=(512,384)`, and the TNT height at that cell (**96**) equals
  the engine's runtime height — so the heightmap read and the
  `worldZ − h/2` projection are both right.
- **Our commander is visibly brighter than the engine's.** Team colour is a
  **GAF frame table**, not a palette-band remap: a multi-frame entry carries one
  *separately painted* frame per player and the engine draws `frame[owner]`
  (file-formats.md §3, live-verified). tascene takes frame 0 for every entry.
  The suspect is named there: **ARMCOM's torso takes its owner colour from
  `glow` — 8 frames, in `armbldg.gaf`** — and `glow` is exactly the entry whose
  yellow faces this build already had trouble with. Frame 0 *is* player 0's ramp
  and the fixture's commander is owner 0, so this should agree; it visibly does
  not. **Named mechanism, unmeasured cause** — a per-frame histogram of `glow`
  against the engine's pixels would settle it.
- **Team colour is a certain gap for other players' units**: frame 0 for
  everyone means an owner-1 unit wears owner 0's colours. `ta3do` takes frame 0
  too, so this is shared, not a tascene regression.
- **Yaw is still unverified.** The fixture places the commander at facing 90; a
  facing-45 fixture is what would actually settle `facing + 180`.

### Three bugs the build found, all worth keeping written down

- **Intra-model depth is not optional.** `tagpu_native.c` gives every vertex
  `encBase + clamp((2y − z)/256, ±1.8)` — "squeezed into the ±2 gap between row
  keys" — and the world position per *vertex*, not per unit. With one depth key
  for a whole unit, GL_LESS keeps whatever drew first and the commander's yellow
  `GLOW` torso panels drew over its own front. It looked like a texture or
  palette bug and was neither.
- **The shader extractor's `\s+` crossed newlines**, so the header guard
  `#define TAGPU_GLSL_H` (empty body) swallowed the macro after it and that
  macro silently ceased to exist. It surfaced as a hard "unknown macro" error
  only because expansion refuses to guess — which is why it refuses.
- **The extractor only knew `TAGPU_GLSL_*`**, and shader bodies also use plain
  value macros from the same header. Merging main brought in the tile-seam fix,
  whose terrain VS reads `- vec2(" TAGPU_EDGE_NUDGE ")`; the extractor dropped
  the token and emitted `vec2()`, which does not compile. **This is the
  extract-don't-copy decision paying for itself on its first merge** — a
  hand-maintained copy would simply have gone on rendering the old shader, and
  the browser would have quietly stopped matching what tagpu ships.

And a fourth, found by the A/B rather than by the build: **`ab` trusted a zero
eye.** `tacli scenario load` returns when the applier has answered, but the
overlay logs its roster block on its own cadence, so the newest `units:` line can
still be the one from boot — whose eye is `(0,0)`. The first run built the browser
frame at eye (0,0), diffed it against a real game frame and reported a
meaningless **94.63 %**. That number looked like a result. `ab` now polls until
the roster has both a non-zero eye and units, and dies saying so if it never
settles.

### Still open on landing 1

The A/B **has** been run — the numbers are above. What it left open:

- **Feature shadows** use an RGB blend where the engine uses the `PALETTE.ALP`
  palette-space remap, which is the whole of the sprite residue. This is tagpu's
  existing gap (G13a), not a new one, but tascene could close it first —
  `palettes/palette.alp` is on disk, 256×256.
- **Owner colour**: frame 0 for every entry, so any unit not owned by player 0
  wears player 0's ramp.
- **Yaw is unverified**: the fixture is at facing 90, which looks plausible from
  every angle. A facing-45 fixture settles `facing + 180`.
- **The mouse cursor is counted against us** — it is engine-drawn and we do not
  draw it, and it is 232 of the 284 terrain-classified differing pixels. The diff
  should mask it, or the fixture should park the pointer outside the viewport.
- **No tests for `tascene`** (`ta3do` and `tacli` both have suites).

### What landing 2 actually built

The exploration lane, in `tools/tascene-view.html`'s own **lab shaders** — kept
outside the pack and checked in, as the shader-sharing decision requires, and
written to stay portable back into `tagpu_glsl.h` if one wins (same uniform
names, same premultiplied output).

- **Relief.** The same 32-px tile grid, each cell split into **four 16-px
  quads** so every `mapattr` height gets a vertex and the tile's UVs are
  quartered with it. Nothing about the art's mapping changes — only where its
  corners land. `relief` scales the displacement; the normals are central
  differences over the heightfield and are real at every `relief`, which is what
  makes `relief=0` a *control* rather than an off switch.
- **Undithered atlases.** `build --undither[=PRESET]` packs restored
  true-colour atlases for terrain, features and units beside the R8 ones. Per
  tile, with no neighbour context — that is the 2.03/255 edge-ring measurement
  above cashed in.
- **The N·L sun**, one direction and one rule (`LAB_LIGHT`) for terrain, unit
  faces and feature sprites, evaluated **per pixel** in the fragment shader
  wherever there is a normal to evaluate it on — which is where a local light
  (a laser, an explosion) will join it, as a term that varies across one face.
  A 3DO's faces are flat, so its per-pixel value equals the old per-face one
  (byte-identical with the sun off); a smooth model would only put a different
  normal on each vertex. A sprite is a billboard with no normal of its own, so
  it takes **the ground's lambert at its own anchor cell** (`lambertAt`, the
  rule's JS twin): a tree on a shaded slope sits in the shade instead of on
  top of it.
- **Level ground takes exactly 1.0.** The art is already lit (`artlight`,
  below), so the lambert is divided by what a level normal receives —
  `amb + (1−amb)·sin(el)` = 0.870 at the defaults — and the sun only modulates
  by the tilt from level. Before this the lane rendered every level cell at
  0.870, a second sun on top of the artist's, and read 13 % dark against
  parity in any wipe. Measured on the hilly viewport at eye (4864, 11392): with
  `slope=0`, so that every normal is level, the lane is now **0 differing
  pixels** against parity where it was 626 776 of 630 784; at the default sun
  its mean brightness is **0.988×** parity's, from 0.859× (the residual is the
  tilt itself — a tilted normal's N·L averages a little under level's). Unit
  faces take the same factor: their shade is the old one × 1.1505 (median over
  325 unit pixels; predicted 1/0.870 = 1.1494).
- **The wipe and the presets**, described under "Using it".

### The exploration lane, calibrated against the parity lane  [VERIFIED 2026-09-03]

Two lanes in one page is exactly the arrangement where one quietly starts
changing the other, so both directions were measured rather than assumed.

- **The parity lane did not move.** The same pack shot before and after the
  rewrite is **byte-identical** (`md5 60adadd4334a9ab7e1027cd1090e0175`). That
  md5 is the `ss=1` frame; since the default became `ss=2` (2026-09-03) the
  default frame is `md5 4549242d3e8ff29dfd52f3c42be0dbb8`, and `ss=1` still
  gives the old one — the box composite at n = 1 is the old single tap.
  **Padding the unit atlas moved it by 11 pixels, deliberately** (later the
  same day): `ss=1` is now `md5 9c9ab215099e581288b24cc47d41f9be` and the
  default `md5 6f7ad6b122591d6db2a2b028938be5b3`. All 11 are on the commander
  and all 11 were `(0,0,0)` before — fragments whose collapsed n-gon UVs land
  exactly on the frame's far edge, which `NEAREST` resolved to the empty atlas
  texel past the frame and now resolves to the frame's replicated edge, as the
  engine's rasteriser clamps. The viewer alone leaves the old pack at
  `60adadd4…` exactly; the pack layout is the whole difference.
- **The exploration lane reduces to it.** At `relief=0&sun=off` — the whole lab
  path: 16-px mesh, quartered UVs, lab shaders, lab depth keys — the frame is
  **0 differing pixels out of 630 784** against the parity lane, terrain and
  feature sprites together, on the hilly viewport at eye (4864, 11392). So a
  difference seen in the exploration lane is the relief or the light, and never
  the re-tessellation. With the sun **on** and `slope=0` it is also 0 differing
  pixels, which is the level-ground normalisation being exact.

Two lane differences remain by construction, both located rather than guessed:

- **Flat-feature depth, 315 px (0.050 %) on the parity fixture.** The parity
  lane gives every flat feature a key in one global 0.40–0.50 band, so a rock is
  behind every tall feature whatever row it stands on; under a **per-row
  heightfield** that band would sit behind the ground itself, so the lab keys
  flat features per row like everything else. The 315 differing pixels are all
  rocks that the engine's band hides behind trees anchored further away.
  Isolated by shooting the buckets separately: **bodies 315, shadows 0**, and
  unchanged when the tall-feature column term is put back to the parity 1.5 —
  so it is the flat key and nothing else.
- **Units, the remaining 365 px at the fixture eye.** The lab unit shader reads
  vertex slot 3 as a **lambert** where tagpu's reads it as a `PALETTE.SHD` row /
  31 — same buffer, same stride, one float re-meant. That is the whole
  difference between the two shading philosophies, and it is the decision
  ("undithered drops the LUT") made visible.

### The restorer, cached  [VERIFIED 2026-09-03]

Two Continents, `learned` preset: **5 154 frames** (5 062 tiles + the feature
and unit textures) in **55 s cold**. The cache is keyed by the frame's own
**content** — `sha256(preset ‖ w ‖ h ‖ transparent ‖ pixels ‖ palette)` — not by
`(map, preset)`: tiles recur between maps and between builds, and a content key
cannot go stale. Warm, the same build takes **7.3 s**, which is what a build
with no `--undither` at all costs, and the atlas is **byte-identical** to the
cold one. Writes are `tmp` + `replace`, so two builds can share one cache.

### Does the art already contain the hill?  [VERIFIED 2026-09-03]

The question the exploration lane exists to answer, and it now has a number.
Method — and it is a verb, `tascene artlight --map <name>` (or `--all`), ~2 s a
map: for every 16-px cell steeper than ~14° (`--steep 0.25`), correlate the
**mean Rec.709 luminance of the tile quadrant painted there** against the
**N·L that quadrant's own gradient would receive**, sweeping the sun's azimuth
in 15° steps at the engine's elevation of 53.1°. A directional signal shows up
as the **amplitude** of the resulting sinusoid; a material confound — steep
cells simply being painted with darker rock — shifts its **offset** instead, so
the two are separable.

Four maps as worked examples:

| Map | steep cells | peak | trough | amplitude | offset | r at the engine's own sun |
|---|---|---|---|---|---|---|
| Two Continents | 82 481 | **+0.173** @ az 240 | −0.161 @ az 60 | 0.334 | +0.006 | **+0.014** |
| Gods of War | 13 907 | **+0.213** @ az 285 | −0.227 @ az 120 | 0.440 | −0.007 | +0.177 |
| Painted Desert | 70 958 | **+0.517** @ az 270 | −0.324 @ az 90 | 0.841 | +0.097 | +0.359 |
| Metal Heck | 7 700 | **+0.666** @ az 315 | −0.555 @ az 135 | 1.221 | +0.056 | **+0.659** |

### …across all 275 stock maps  [VERIFIED 2026-09-03]

`tascene artlight --all --json`, ~11 min. 275 maps read, **273** with at least
2 000 steep cells (the other two are effectively flat and were dropped).

| | median | p10 | p90 | max |
|---|---|---|---|---|
| **amplitude** — the directional signal | **0.501** | 0.255 | 1.011 | 1.385 |
| **&#124;offset&#124;** — the material confound | **0.060** | — | 0.148 | 0.300 |
| **r at the engine's own sun** (az 324.5) | **+0.191** | +0.018 | +0.457 | — |

**The art is lit — on nearly every map.** Only **8 of 273** have an amplitude
below 0.15, and the confound stays an order of magnitude smaller than the signal
throughout, so this is a direction and not steep cells simply being painted with
darker rock.

**But not from where the engine thinks.** The amplitude-weighted circular mean
of the peak azimuths is **277.5°** (concentration R = 0.79) — the screen-left,
about **47° counter-clockwise of the engine's own unit light at 324.5°** — and
only **61 of 273 maps (22 %)** peak within 22.5° of it. The mode is az 240
(47 maps) and **252 of 273 (92 %)** peak somewhere in the 225–345 arc, so TED
artists agree the sun is up and to the left, and disagree about the rest.

Three things follow, and they are the useful output of landing 2:

1. **`relief=1` plus a sun double-counts, by an amount that is a property of the
   map**, not of the renderer. At the engine's own sun: **40 maps score r ≥
   +0.4** (the art already carries most of that light — Metal Heck is one),
   **75 score ≤ +0.1** (almost none — Two Continents is one, at +0.014), and
   **23 go negative**, i.e. their art is lit from the other side and a sun there
   fights the painting rather than reinforcing it.
2. **A single sun cannot be right for both layers on most maps.** The units are
   lit from az 324.5 by the engine itself, and the terrain art disagrees on
   78 % of stock maps. Any "add lighting" gate has to pick which layer to
   respect, or relight the terrain art rather than multiply it — and if it
   picks per map, `artlight`'s peak azimuth is the number to pick with.
3. **"Relief looks wrong" was the right thing to leave open.** It is not one
   answer. The honest knob is `relief`, and `half-relief` exists because the
   answer is somewhere on that axis and not at either end.

The correlation is a **screen, not a proof**: brightness varies for reasons
other than light, and r ≈ 0.19 explains ~4 % of the variance on the median map.
What it establishes is the *direction*, its *strength ordering* and the fact
that the disagreement with the engine's light is systematic rather than
anecdotal — which is what the lane needed.

### The seam that came back  [VERIFIED 2026-09-03]

Reported from the live page: *"explore version has blue bars between tiles."* It
was the tile seam, and the diagnosis is worth keeping because it is a good
example of a lane inheriting a bug the other lane cannot see.

`tagpu_terr.c` fixed this in G13h with a **guard rail**: cells sit
`CELL_PITCH = 34` apart and the border texel is a *copy* of the cell's edge
row/column, so a fragment centre landing exactly on a quad's far edge — which
interpolates `v` to exactly `v1`, and `GL_NEAREST` resolves to
`floor(v1·H)` = the first texel of the **next** cell, an unrelated tile 64 cells
later in the atlas — reads the right colour instead. **tascene's pack was still
building the pre-guard flush 32-pitch atlas.**

The parity lane never showed it: its quads are integer-aligned, so no fragment
centre ever lands on a cell edge. **The exploration lane displaces y by
`relief·h/2`, which is a half-pixel whenever the height is odd**, and the
hairlines came straight back — visible as blue only where the wrong cell
happened to be water, which is why they read as "blue bars".

Measured by rebuilding the pack with the guard and diffing the two, which is
exact: the guard changes nothing *except* the samples that used to fall off the
cell, so the difference **is** the seam set.

| eye | `relief=0` | `relief=1` | `relief=1` + `undither=1` |
|---|---|---|---|
| 4864,11392 | **0** | 13 660 px / 704 rows | 16 070 px |
| 2560,10496 | **0** | 4 142 px / 698 rows | 6 657 px |
| 640,1408 | **0** | 7 819 px / 695 rows | 10 931 px |
| 4736,7168 | **0** | 6 371 px / 701 rows | 9 830 px |
| 2256,768 (the fixture) | **0** | 14 858 px / 605 rows | 17 773 px |

Two things that table says, and neither was obvious from the symptom:

- **It was never one line.** Thousands of pixels across ~700 of the 704 rows, at
  every `y mod 16` phase. A displaced heightfield puts *every* quad row at some
  fractional offset, so the visible blue hairlines were only the subset where
  the neighbouring cell was water.
- **`relief=0` is 0 px on every eye**, which is `tagpu_terr.c`'s own claim
  ("the quad still spans 32 texels, so sampling at 1:1 is bit-identical to the
  un-padded atlas") holding in the lab. That is also why the parity fixture shot
  is still `md5 60adadd4…` after the change (at `ss=1`; the calibration
  section carries the two later re-baselines, `ss=2` and the padded unit
  atlas), and why the exploration lane still reduces to the
  parity lane at 0 differing pixels of 630 784.

The extracted fragment shader's own comment had been asserting the guard all
along — *"a fragment landing exactly on the far edge reads the cell's replicated
guard texel rather than the next cell (CELL_PITCH)"* — while the pack it was
sampling did not provide one. **Extracting the shader does not extract the
invariants it depends on**, and this is the second time that has bitten (the
first was `TAGPU_EDGE_NUDGE` going missing after a merge). Worth a check on any
future pack-format change: the C source is authoritative for the *data layout*
too, not only for the GLSL.

### Still open on landing 2

- **Landing 1's list above is untouched** — none of it was in landing 2's scope,
  and `tascene` still has **no tests** while `ta3do` and `tacli` both have suites.
- **No smoothing.** The undithered atlases are sampled `NEAREST`, like the
  indexed ones. The `CELL_PITCH` guard is a *one*-texel replicated border, which
  is what `NEAREST` needs and not what `LINEAR` needs — bilinear would still
  blend toward the neighbouring cell over the outer half-texel. A `filter=linear`
  knob wants a wider border, or explicit UV clamping in the shader.
  Deliberately not attempted here rather than shipped looking broken.
- **The exploration lane runs at zoom 1 by construction.** The lab shaders drop
  `uZoom`/`uZoomC`; the parity lane keeps them because tagpu's shaders have them.
- **Water is still static** in both lanes, for the reason in "Gaps" above.
- **The double-count measurement is a screen, not a proof** (see its own
  caveat). It now covers all 275 stock maps, but it says nothing about *what to
  do*: relighting painted art means removing the light already in it, and
  nothing here estimates that.
- **Nothing has been carried back into `tagpu`.** Landing 3 in the plan is
  deliberately not started, and landing 2's own finding — that the terrain art
  and the units disagree about where the sun is on three of the four maps
  sampled — is an argument for choosing carefully rather than for shipping the
  sun as it stands.
