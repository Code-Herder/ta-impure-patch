# Model export — 3DO + GAF → glTF 2.0, and the standard views

`tools/ta3do` turns a stock Total Annihilation unit into a modern asset and a fixed set of
pictures, without unpacking anything to disk:

```
tools/ta3do render armpw -o renders/armpw --sheet
```

reads `totala*.hpi` in place, parses `objects3d/ARMPW.3do`, resolves its GAF textures through
`PALETTE.PAL` into one atlas, writes `armpw.glb`, then drives headless Chrome over three.js to
screenshot the model from **front, side, top, back and 3/4** — one PNG per view, plus a contact
sheet. Tests: `python3 tools/test_ta3do.py` (51, offline, no game files).

The format details all live in [file formats](file-formats.md); this page is about the
pipeline, the decisions it had to make, and the two places the shipped archives disagreed
with that page.

## The pipeline

| Stage | What it does | Where |
|-------|--------------|-------|
| **Mount** | Every `.hpi`/`.ccx`/`.gp3`/`.ufo` in the game dir, later shadowing earlier, into one lookup. Loose files on disk win. | `Assets` |
| **Resolve** | `armpw` → `units/armpw.fbi` → `Objectname` → `objects3d/armpw.3do`. A bare model name or a file path also work; a miss suggests near matches. | `resolve_model` |
| **Parse** | The 3DO object tree: 52-byte headers, 16.16 vertices, quads/N-gons, first-child/next-sibling links. | `parse_3do` |
| **Hide** | Read `scripts/<unit>.cob` and collect the pieces `Create` hides on the first frame. | `hidden_at_create` |
| **Atlas** | Every GAF frame the model names, plus a 4×4 swatch per flat-colour palette index, shelf-packed into one RGBA image with a 1px extruded border. | `Atlas` |
| **Build** | Triangulate (fan), Newell normals per face, flat shading by vertex duplication, UVs from the atlas rect. | `build_model` |
| **Export** | glTF 2.0 as a single `.glb`: one node per 3DO piece, one mesh per piece, **one material**, the atlas embedded as a PNG buffer view. | `model_to_gltf` |
| **Render** | Private Xvfb → headless Chrome → `tools/ta3do-view.html` (three.js) over a loopback HTTP server → `--screenshot` per view. | `cmd_render` |

Everything is stdlib-only: `zlib` does the archive inflate and also writes and reads the PNGs.

## The standard views

Azimuth 0 looks at the model's **front**, which after export is **+Z** — glTF's own convention
("the front of an asset faces +Z"). Azimuth rises anticlockwise seen from above; elevation is
degrees above the horizon.

| View | Azimuth | Elevation | Notes |
|------|---------|-----------|-------|
| `front` | 0 | 0 | |
| `side` | -90 | 0 | the unit's **right-hand** side (glTF calls -X right) |
| `top` | 0 | 89.9 | the nose points to the top of the frame |
| `back` | 180 | 0 | |
| `quarter` | -45 | 30 | the 3/4: front, right and above |
| `left`, `right`, `bottom` | | | available, not in the default set |

Framing is the same rule every time: fit the model's bounding box **as the camera sees it**
(per-corner, not its bounding sphere — a sphere wastes half the frame on a tall thin kbot),
plus an 8% margin. Lights ride with the camera, so a back view is no darker than a front one
and two views of the same unit are comparable. Background is transparent by default.

`tools/ta3do views` prints the table; the viewer page repeats it so it works standalone, and a
test compares the two so they cannot drift apart.

## Decisions, and what backs them

- **Handedness: negate Z, reverse winding.** The 3DO file is Y-up left-handed; glTF is
  right-handed. Negating Z crosses the handedness, which reverses every face, so the triangle
  order is reversed too. `ta3do check-winding` measures it rather than asserting it: for
  ARMPW/ARMCOM/ARMSTUMP/ARMSOLAR, reversed order puts **66–87%** of faces pointing away from
  the hull and as-written puts **13–34%** — the remainder being the genuinely concave parts of
  a kbot. [VERIFIED — run the command]
- **Left and right survive the flip.** ARMPW's piece named `lfire` sits at +X in the file, and
  glTF calls +X *left*. The naming and the convention agree, so the export needs no extra
  rotation. [VERIFIED]
- **Forward is -Z in the file.** Muzzle pieces sit at negative Z on every unit checked
  (ARMPW `lfire` -13.2, ARMSTUMP `flare` -16.3, CORRAID `flare` -15.2), so file -Z becomes
  export +Z and the model faces the glTF front. [VERIFIED]
- **Muzzle flares are hidden by the unit's own script, not by a name heuristic.** Every stock
  `Create` opens with a run of `HIDE <piece>`: ARMPW hides `rfire`/`lfire`, ARMSTUMP hides
  `flare`, ARMCOM hides `rbigflash`/`lfirept`/`nanospray`. `ta3do` walks that prologue with a
  whitelist of opcodes whose length is certain and **stops at the first word it cannot
  account for**, so it never guesses. Without this the flares draw as floating spikes and, worse,
  stretch the framing: ARMPW's bounding box is 33.3 units deep with them and 21.7 without.
  [VERIFIED] `--show-hidden` keeps them.
- **One material per unit.** Textured faces sample a GAF frame; untextured ones are a single
  palette index. Both go in the same atlas (the flat colours as small swatches), so a unit is
  one draw call and one file, and `NEAREST` magnification keeps the 1997 pixels crisp.
- **UVs are implied, not stored.** A 3DO primitive names a texture but carries no texture
  coordinates: TA stretches the frame across the face, corner to vertex. Quads and triangles
  therefore get exact corners; for N-gons past four vertices there is no corner left, so those
  get a planar projection into the same rect — a choice, not a spec.
- **The selection quad is dropped.** The root's `SelectionPrimitive` is the invisible footprint;
  `--keep-selection` puts it back.
- **WebGL headless needs coaxing.** Chrome 149 returns a null WebGL context under
  `--headless=new` unless it is given `--use-gl=angle --use-angle=swiftshader
  --enable-unsafe-swiftshader` **and** an X display exists. `ta3do` starts its own Xvfb rather
  than borrow the user's desktop, and takes it down again. `--headless=old` fails regardless.
  [VERIFIED — the probe matrix in this session]

## Corrections to `file-formats.md`

Both found by reading real archives; the note tags them `[CLAIMED]`, and the claims were wrong.

1. **HPI LZ77 tag polarity is inverted in §5.** The note says "set = copy literal byte, clear =
   back-ref". The archives say the opposite: a **clear** bit is a literal, a **set** bit is the
   two-byte back-reference. Decoded the other way, `ARMPW.3DO`'s first tag byte (`0x74`) asks
   for a back-reference before anything is in the window and the stream ends immediately at
   zero bytes; decoded this way the file opens with its `01 00 00 00` version signature.
   [VERIFIED]
2. **§0's team-colour band is off.** The note says "indices ~216–223". The ARM colour patches
   (`colorsmd`, `colorsdk`, `colordk2` in `textures/logos.gaf`) use **105–110**. `ta3do` leaves
   the palette as shipped, so renders come out in the default ARM blue. [VERIFIED — pixel
   histogram of those three frames]

## What it covers

All **608** stock models parse, texture, build and serialise — 96,963 triangles in total, no
failures and no unresolved texture names. Cross-checked against TA's own artwork: the 3/4 render
of ARMPW matches `unitpics/armpw.pcx` in silhouette, camo, grey arms and dark legs.

## Not done yet

- **No pose.** Pieces sit at their rest offsets. The COB VM is only read for `Create` hides; a
  real animation path would run the bytecode (§2.6 of the file-formats note) and write per-piece
  translation/rotation into the glTF nodes — or export animation samplers.
- **No team-colour remap.** The palette band above is left alone, so every ARM unit renders
  blue and every CORE unit red-ish. A `--team-color` that rewrites 105–110 at atlas-build time
  is a small addition.
- **No wreck/feature variants.** `armpw_dead.3do` exports fine, but nothing joins a unit to its
  wreckage or to `features/*`.
- **Three.js comes from a CDN.** The viewer page needs network on first load. Vendoring the two
  modules would make the render step fully offline.
