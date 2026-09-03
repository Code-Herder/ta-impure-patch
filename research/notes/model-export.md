# Model export — 3DO + GAF → glTF 2.0, and the standard views

`tools/ta3do` turns a stock Total Annihilation unit into a modern asset and a fixed set of
pictures, without unpacking anything to disk:

```
tools/ta3do render armpw -o renders/armpw --sheet
```

reads `totala*.hpi` in place, parses `objects3d/ARMPW.3do`, resolves its GAF textures through
`PALETTE.PAL` into one atlas, writes `armpw.glb`, then drives headless Chrome over three.js to
screenshot the model from **front, side, top, back and 3/4** — one PNG per view, plus a contact
sheet. Tests: `python3 tools/test_ta3do.py` (60, offline, no game files).

With `--undither` the textures go through the [unditherer](undither.md) first, one GAF frame at
a time, and the model comes out in true colour instead of TA's 256; `ta3do compare` puts the two
side by side on a locally served page.

The format details all live in [file formats](file-formats.md); this page is about the
pipeline, the decisions it had to make, and the two places the shipped archives disagreed
with that page. The same stack is loaded as a **library** by
[tascene](tascene-design.md) — the browser render lab imports this file for HPI/VFS, the
palette, GAF decode, the 3DO parser and the PNG codec, and reuses its `Viewer` and headless
Chrome shooter, so a change to the format code here lands in both. Taking a glTF back the other way — into the running engine, posed by the
unit's own script — is [model import](model-import.md), and it is worth reading the axes
section there before editing an exported model: the frame this page writes is a **mirror** of
the file, while the frame the engine holds that same file in is a **180° yaw** of it, so the
inverse trip is not the conversion below run backwards.

## The pipeline

| Stage | What it does | Where |
|-------|--------------|-------|
| **Mount** | Every `.hpi`/`.ccx`/`.gp3`/`.ufo` in the game dir, later shadowing earlier, into one lookup. Loose files on disk win. | `Assets` |
| **Resolve** | `armpw` → `units/armpw.fbi` → `Objectname` → `objects3d/armpw.3do`. A bare model name or a file path also work; a miss suggests near matches. | `resolve_model` |
| **Parse** | The 3DO object tree: 52-byte headers, 16.16 vertices, quads/N-gons, first-child/next-sibling links. | `parse_3do` |
| **Hide** | Read `scripts/<unit>.cob` and collect the pieces `Create` hides on the first frame; a second, flat scan of the whole script collects every piece it `SHOW`s, and the intersection is the muzzle flash. | `hidden_at_create`, `shown_after_create`, `piece_visibility` |
| **Undither** (optional) | `--undither`: each GAF frame the model uses goes through the [unditherer](undither.md)'s CNN **on its own**, as an indexed PNG carrying TA's palette, and comes back true colour. | `undither_frames` |
| **Atlas** | Every GAF frame the model names — restored or as shipped — plus a 4×4 swatch per flat-colour palette index, shelf-packed into one RGBA image with a 1px extruded border. | `Atlas` |
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

## Undithering: 256 colours, or true colour

TA's textures are 8-bit palette art, dithered to fake shades the palette does not have. The
[unditherer](undither.md) undoes that. One switch turns it on:

```
tools/ta3do export armpw -o renders --undither            # learned (the 12x64 CNN)
tools/ta3do render armpw -o renders --undither tuned      # or a classical preset
tools/ta3do compare armpw --port 8731                     # both, side by side, served
```

**Each GAF frame is restored on its own, and the atlas is packed from the results.** Not the
other way round: the network was trained on single game textures, and a packed sheet puts
tiles next to each other that are neighbours nowhere on the model, so colour can creep across
a packing seam. The frames go out as **indexed PNGs with TA's own palette** rather than
flattened RGB, because that is what the unditherer wants — it measures the dither amplitude and
the palette band step from the indices, and inpaints the colour-key index before filtering.
All of a model's frames go through **one** process, since loading the CNN costs far more than
running it on a 32×32 texture (ARMPW: 13 frames, one invocation).

The undithered export is a separate file — `armpw-undithered.glb`, `armpw-undithered-atlas.png`,
`armpw-undithered-front.png` — so the two variants sit in one directory without collision.
`ta3do` itself stays stdlib-only: it drives `python -m unditherer restore` in the checkout's
`.venv-undither`, overridable with `--undither-python` or `TA3DO_UNDITHER_PYTHON`.

`ta3do compare <unit>` exports both, writes `tools/ta3do-compare.html` next to them and serves
the directory. The page draws **one camera into two scissored halves**, so the only difference
between left and right is the texture pipeline; drag orbits both together, and the two atlases
sit underneath at 1:1. `--shots` also screenshots the pair for each standard view.

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
- **The engine does not agree with this file's frame either — it yaws it 180°.** Measured
  against a live unit's posed vertex buffer: `Model3DONode` vertices and offsets are the file's
  with **x and z both negated**, which is a rotation, not a mirror, and is invisible in any
  render of the model alone. It matters only when an exported model has to be placed back in the
  engine's own frame; [model import](model-import.md) carries the derivation. [VERIFIED —
  `tagpu_posedump.on`, residual 2e-5 model units over every piece of a walking Peewee]
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
- **Hidden-at-Create splits in two, and `--keep-flares` keeps only one half.** A piece the script
  hides *and shows again later* is geometry the unit really wears in play — the muzzle flash,
  `SHOW`n by `FireWeapon` for a few frames per shot. A piece it hides and never shows is an
  anchor the effects system fires a sprite from, invisible in game as well, and stays dropped.
  Over the 90 stock units whose `Create` hides anything: 23 hide a `flare`, 26 a `flare1`, 25 a
  `flare2` and 5 a `flash` that they all show again, against a tail of never-shown anchors
  (ARMCOM's `nanospray`, ARMJETH's `lfirept`/`rfirept`, 4 units with a dead `flare`). A few kept
  pieces are not flashes at all but geometry the unit reveals — ARMZEUS's `gun`, ARMSS's and
  CORSS's `sshead2`, ARMMAV's gun barrels, CORAH's `launcher2` — which is the same thing for the
  file's purposes: the unit shows them. [VERIFIED —
  scan of every model with a script]
  The `SHOW` scan is **flat**, not a walk: the flash is shown from `FireWeapon`, past jumps and
  calls this exporter deliberately does not decode. It accepts a word only when the word is the
  `SHOW` opcode *and* the next word is a valid piece index, and a false positive can only keep a
  piece that would otherwise be dropped.
  This matters most for [import](model-import.md), not for renders: the engine gives a HIDden
  piece an all-zero matrix, so the flare geometry in an exported `.glb` stays invisible until the
  unit's own script shows it, and a hires model exported **without** it simply cannot flash.
  It does widen the framing of a render — ARMSTUMP is 32.2 model units deep without the flare and
  42.7 with it — so it is off by default. [VERIFIED — `ta3do info armstump [--keep-flares]`]
  The file says which pieces those are: `asset.extras.flarePieces` alongside `hiddenPieces`.
  **Not every unit has flash geometry to keep.** ARMFLASH's own `flare1`/`flare2` are single
  vertices with no faces, so the flag changes nothing there — the node is exported either way,
  as an empty, and the flash is the engine's sprite. [VERIFIED — `ta3do info armflash`]
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
   **Superseded, and the framing here was wrong too:** file-formats §3 has since established
   that team colour is a **frame table**, not a band — an entry carries one separately painted
   frame per player and the engine draws `frame[owner]`. "105–110" is **player 0's ramp seen
   through frame 0**; frame 1 runs 202–204, frame 2 runs 80–83, and so on. Nothing rewrites a
   band. [VERIFIED — per-frame histogram of every `logos.gaf` entry]

## What it covers

All **608** stock models parse, texture, build and serialise — 96,963 triangles in total, no
failures and no unresolved texture names. Cross-checked against TA's own artwork: the 3/4 render
of ARMPW matches `unitpics/armpw.pcx` in silhouette, camo, grey arms and dark legs.

## Not done yet

- **No pose in the file.** Pieces sit at their rest offsets. The COB VM is only read for
  `Create` hides; writing per-piece translation/rotation into the glTF nodes, or exporting
  animation samplers, would need the bytecode run (§2.6 of the file-formats note). This is a
  gap in the *file*, not in the game: an imported model is posed by the engine's live COB state
  at render time, by node name — see [model import](model-import.md).
- **Undithering does not touch geometry or the flat-colour faces.** A face with no texture is
  a single palette index and has nothing to undither; only sampled GAF frames go through the
  network. On ARMPW that is 13 of the 41 textured faces' worth of frames, against 40 flat ones.
- **No owner colour.** A static exporter has to pick a frame and `ta3do` picks frame 0, so
  every unit renders in **player 0's ramp** regardless of side. The small addition is
  `--owner N` selecting `frame[owner]` (clamped) for multi-frame entries at atlas-build time —
  **not** a palette rewrite: an earlier revision of this page proposed rewriting 105–110, which
  followed from the band model that file-formats §3 disproved. 263 of the 608 stock models name
  at least one team-colour entry, and the engine keys on the entry being multi-frame rather
  than on the file — ARMCOM's torso takes its owner colour from `glow` in `armbldg.gaf`.
- **No wreck/feature variants.** `armpw_dead.3do` exports fine, but nothing joins a unit to its
  wreckage or to `features/*`.
- **Three.js comes from a CDN.** The viewer page needs network on first load. Vendoring the two
  modules would make the render step fully offline.
