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
and nothing unpacked to disk. `tools/tascene-view.html` draws that pack in **WebGL2 using
`tagpu`'s own shaders**, extracted from the C sources at export time so they cannot drift.
Every setting is a URL query parameter, so a look is a link, and the same string drives a
headless screenshot. `tascene ab` runs both sides of the same scenario — engine and browser —
and diffs them.

## Two lanes, and the reason there are two

The lab has one job the game cannot do (try a look) and one job that keeps it honest (prove the
export is correct). They pull in opposite directions, so they are separate modes, not a blend.

| | **Parity lane** | **Exploration lane** |
|---|---|---|
| terrain | flat quads on the engine's grid | heightfield relief from `mapattr` |
| textures | R8 indexed — the texel *is* the palette index | undithered RGB |
| shading | the engine's `PALETTE.SHD` LUT, in index space | N·L sun over real normals |
| camera | oblique, shear 0.5 | oblique (orbit available for debugging) |
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
| Camera | **The engine's oblique projection is the default and the only mode carrying parity claims.** TA's `screenY = worldZ − h/2` is a *shear of 0.5*, not a rotation (a rotated ortho would need `cos θ = 1` and `sin θ = 0.5` at once) — so a heightfield under that same shear stays consistent with every engine rule: units still anchor at `worldZ − h/2`, features still sort by 16-px row. A free orbit exists to inspect geometry; it makes no parity claim. |
| Textures | **Dithered and undithered are a viewer toggle**, and they are two whole shading philosophies, not two files: indexed keeps the SHD LUT and stays diffable; undithered drops the LUT and lights with a real sun. Reconciling them (carrying the index alongside the colour and ramp-sampling) was considered and deliberately **not** taken. |
| Units | **Stock 3DO bind pose + the `gamedir/hires/<name>` replacement slot**, switchable per unit type — so the lab is also the authoring loop for G11's replacement pipeline. `ta3do`'s `Create`-`HIDE` scan keeps muzzle flares out, as in `render`. **No COB VM**: a second interpreter to keep correct, when the deferred live-snapshot path would give real poses far more cheaply. |
| Shader sharing | **One-way extraction at export time.** `tascene` parses `static const char* VS/FS =` out of `tagpu_terr.c` / `tagpu_native.c` / `tagpu_feat.c`, expands the `TAGPU_GLSL_*` macros from `tagpu_glsl.h`, swaps the version header, and writes real `.glsl` into the pack. `tagpu` stays authoritative and the browser is provably never stale. Lab-only shaders live outside the pack, checked in; `pack/` is generated and gitignored. |
| Tool layout | **New `tools/tascene`, importing `tools/ta3do` via `SourceFileLoader`** — the idiom `tools/test_ta3do.py` already uses. `ta3do` keeps its name and its meaning (one model, standard views); `tascene` owns maps, scenes and the lab. No refactor of a tested 1824-line tool. |
| Options | **Every knob is a URL query parameter** (the `ta3do-view.html` house idiom, including `?shot=1` → hide UI, one frame, stamp `document.title`). A look is a link; the headless shooter takes the same string. An in-page **wipe** renders two parameter sets at once for direct comparison, and named looks live in a checked-in `presets.json`. |
| Pack encoding | **Raw `.bin` for anything whose bytes are semantic; PNG only for display RGB.** Browsers colour-manage and premultiply PNGs — in the parity lane the atlas texel *is* a palette index, so an image decode path would silently rewrite the data and the diff would measure nothing. Raw blobs go straight to `texImage2D`, byte-exact by construction. |
| Map extent | **Whole map, no windowing or streaming.** Worst stock case (Two Continents) is a 2048×2560 R8 atlas, a 336×400 `u16` tilemap, a 672×800 R8 heightmap and ~5000 feature anchors; the relief mesh is 537k verts. Trivial for WebGL2. |
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
| `PTRtilegfx` | `tiles` × 1024 B | the 32×32 8-bpp tiles → one `GL_R8` atlas, 64 per row, exactly as `tagpu_terr.c` builds it |
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

Generated, disposable, gitignored. One directory per scene.

```
pack/
  scene.json              manifest: map, eye, viewport, zoom, palette, unit + feature instances
  terrain/atlas.r8.bin    2048x2560 palette indices        (raw: no image decode path)
  terrain/tilemap.u16.bin 336x400 tile indices
  terrain/height.r8.bin   672x800 heights, one per 16-px cell
  palette/pal.bin         256x4 RGB0      palette/shd.bin  32x256 shade LUT
  textures/atlas.r8.bin   GAF frames, indexed              (units + features)
  textures/atlas.rgb.png  the same, undithered             (display only)
  meshes/<unit>.bin       bind-pose 3DO: pos, uv, per-face palette index, per-face normal
  shaders/*.vert|frag     extracted from tagpu, macros expanded, #version 300 es
```

## CLI surface

```
tascene build  <scenario.json> -o pack/     compile a scene from the archives
tascene serve  pack/                        the lab, on localhost
tascene shot   pack/ --opts '<query>' -o png/    headless, deterministic, one frame
tascene ab     <scenario.json>              drive both sides, diff, write the panel
```

`serve` and `shot` reuse `ta3do`'s existing `Viewer` (`ThreadingHTTPServer`) and `shoot`
(`--headless=new` Chrome on a private X display, ANGLE/SwiftShader, waiting on
`document.title`) rather than growing a second copy.

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
- **The art already contains the hill.** TED map art is painted with its relief and shading
  baked in, so draping it on a displaced heightfield double-counts the elevation. The lab is
  built to *measure* how bad that is; it does not assume an answer, and "relief looks wrong" is
  a legitimate outcome.
- **Minimap letterboxing convention** (how a non-square map maps into the stored picture) was
  not determined — the corner pixel is map content, not padding, on all three maps sampled.
- **Hires `.glb` parsing** leans on three.js's `GLTFLoader` in parse-only mode (read the
  attribute arrays, draw with our own code). Arbitrary exporter output may exceed what that
  path handles; the constrained subset `ta3do` itself writes is the tested case. [CLAIMED]

## Landing plan

1. **Landing 1 — the parity lane.** TNT + TDF parsers, `tascene build/serve/shot`, the WebGL2
   viewer with flat terrain, GAF features and indexed units under the oblique camera, the
   shader extractor, and `tascene ab`. **Exit: a near-zero pixel diff against a real `glshot`
   on a fog-off scenario** — that diff is what certifies the exporter.
2. **Landing 2 — the exploration lane.** Relief displacement, undithered atlases with the
   restorer cache, N·L sun, the wipe, `presets.json`.
3. **Then, and only then**, a winning prototype becomes a roadmap gate with its GLSL carried
   over verbatim.
