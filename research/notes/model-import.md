# Model import — glTF 2.0 → the running engine, posed by the unit's own script

The other half of [model export](model-export.md). Drop a glTF beside the game and the fork
draws it *instead of* the unit type's 3DO, in the engine's own frame, driven by the engine's
own COB script — so the replacement walks, aims and recoils rather than sliding around as a
statue:

```bash
cp armpw.glb <instance>/gamedir/hires/armpw.glb     # lowercase unit def name
```

That is the whole interface. No registry key, no FBI tag, no restart: the slot looks at the file
every 30 frames and reloads when its write time moves, so the edit loop is about two seconds. The
file going away puts the engine's 3DO back.

A load that **fails** puts the 3DO back too, says why, and then stands down until the file's
write time moves again — re-parsing megabytes on the render thread twice a second for the rest
of the session is not a retry, it is a stall, and it appends the same failure to `tagpu.log`
forever. That still retries the case retrying is for (a file caught mid-copy: the copy ends by
stamping its own write time on the destination), and the failed model hands its payload slot
back, so a broken export cannot hold one of the eight against a model that works. Save the file
again to try again.

| Piece of it | Where |
|---|---|
| Loader — glTF/GLB → one static interleaved VBO per model, triangles sorted by material, one piece index per vertex | `tagpu/ddraw/src/tagpu_hires.c` |
| Renderer — its own GL program: per-pixel light, normal maps, GGX, the per-piece pose | `tagpu/ddraw/src/tagpu_hires_draw.c` |
| Pose — reads the engine's live per-piece state and hands the renderer one matrix per piece | `tagpu/ddraw/src/tagpu_native.c`, `pose_accum` / `hires_pose` |
| The oracle — `tagpu_posedump.on`, and what makes all of this checkable | `tagpu_native.c`, `pose_dump` |

Scenarios: `scenarios/hires-one.json` (one Peewee beside one engine-drawn AK, the close-up),
`scenarios/hires-crowd.json` (12 ordinary types on screen before the one with a file — the
payload-slot regression), `scenarios/hires-peewee.json` (20 v 20), and
`scenarios/hires-wreck.json` (units, then husks on the indices they held — the gather-array
regression; its own `description` says how to drive it and what failure looks like). Driving
them: the **ta-drive** skill.

## What the file has to contain

Supported, and nothing outside this list is read:

- `.glb` or `.gltf`; buffers and images from the GLB `BIN` chunk, a `data:...;base64,` URI, or
  a file beside the `.gltf`;
- the whole scene graph — node `TRS` or `matrix`, composed down the tree and baked as the
  **rest** pose;
- primitive `mode` 4 (triangles), indexed or not; `POSITION`, `NORMAL`, `TEXCOORD_0`;
- per material: `baseColorFactor`, `baseColorTexture`, `normalTexture`, `metallicFactor`,
  `roughnessFactor`, `alphaMode` (`MASK` cuts out at its own threshold; `BLEND` also cuts out,
  at half, because this pass does not sort back to front), `doubleSided`;
- **PNG images only** — lodepng is the only decoder in the DLL.

Deliberately unsupported: skins, morph targets, glTF animation (the pose comes from the
engine, not the file), sparse accessors, texture wrap modes (UVs clamp), KHR extensions, and
every material channel past base colour + normal + metallic/roughness.

Caps, all of which log when they bite: 65536 triangles, 32 materials, 32 images, 2048 px on an
image side, 4096 nodes, 64 MB of file, `TAGPU_HMAXPIECE` = **48 posable pieces** (a GPU budget:
the vertex shader spends 3 `vec4` of uniform on each), and **8 replacement models loaded at
once** — that last one counts only types that actually have a file, so ordinary units on screen
never consume it. The node cap is a refusal rather than a truncation: the walk's cycle guard is
a mark per node index, and past the end of that array a malformed child list would recurse
2^depth deep with nothing to stop it.

An image that fails to decode says so too. PNG is the only decoder in the DLL and a JPEG is
legal glTF, so a normal Blender export can hit it; the material falls back to 1×1 white, which
otherwise reads as a broken export rather than a rejected image.

## Axes: the round trip lands one axis away from where you would guess

This is the part that cost the most and is the easiest to get subtly wrong, because a wrong
answer here **renders perfectly at some unit facings**.

Three spaces, all left-handed except glTF's, all in TA model units (game px = the 3DO's 16.16
fixed point ÷ 65536):

| Space | Relation to the `.3do` file |
|---|---|
| The `.3do` file itself | — |
| What `tools/ta3do` writes into the glTF | `(x, y, −z)` — a mirror, which is why it also reverses triangle winding |
| What the **engine** holds in memory after loading that same file | `(−x, y, −z)` — a 180° yaw, no mirror |

So undoing the exporter and then landing in the engine's frame is `−z` followed by 180°, which
composes to exactly **negate X**. That is what `tagpu_hires.c` does — negate X on positions and
normals, reverse winding — and the baked vertex then *is* the engine's own rest-posed vertex,
which is what makes the per-piece pose a clean delta and the yaw the engine's own rotation.

**How it was measured**, because none of it is guessable: `pose_dump` prints, per piece, the
node's rest offset and the engine's posed vertex buffer. Reading `ground` (a flat 32×32 quad at
the model origin) gives the yaw map directly; reading `lfoot`, whose vertices are asymmetric in
both x and z, is the one piece that distinguishes `negX`, `negZ` and `negXZ` — every other
Peewee piece is symmetric enough to match two of the three by accident. Then the whole chain
reproduces the engine's vertex buffer to 2e-5 model units.

**Getting it wrong is invisible at four facings.** The pass shipped applying `rot2(−yaw)` where
the engine applies `rot2(yaw)`; those agree exactly when `cos(yaw) = 0`, and the test scenario
was parked at facing 90. A Peewee rendered a half-turn out at every other facing for as long as
that scenario was the test. `scenarios/hires-one.json` now parks at **facing 45** on purpose —
at 45 no candidate convention agrees with any other.

## Pieces and the pose

**A piece is a glTF node that carries a mesh, and its NAME is the whole binding.** The loader
gives one piece slot per such node, tags every vertex with its slot, and keeps the node's name;
`tagpu_native.c` matches that name, case-insensitively, against the unit's own 3DO piece names
and hands back one 4×3 matrix per piece. `tools/ta3do` names each node after the 3DO piece it
came from, so a round-tripped model binds by construction — rename a node in Blender and that
piece simply stops moving.

It says so, once per model, in `tagpu.log`:

```
hires: hires\armpw.glb loaded, 1577 tris, 6 materials, 10 images, 12 pieces
hires: pose bound 12 of 12 pieces to the unit's 3DO
```

and a shortfall is followed by the names that did not bind. A node with no engine counterpart
draws at its rest place, forever.

### What the engine hands us

It hands the pose over as explicit fields, so nothing has to recover a transform from geometry.
Per piece `p` at `pr = Object3do + 0x22 + p*0x36`, with `nd = *(Model3DONode**)pr`:

| Field | Meaning |
|---|---|
| `nd + 0x10` | `i32[3]` 16.16 **rest offset from the parent** — the field `aabb_walk` sums |
| `nd + 0x1C` | `char*` piece name |
| `nd + 0x2C` / `0x30` | next sibling / first child (the tree; a node's children are `child` plus that child's sibling chain) |
| `pr + 0x04` | `i32[3]` 16.16 COB **`MOVE`** delta |
| `pr + 0x10` | `u16[3]` COB **`TURN`**, 65536 = 360° |
| `pr + 0x22` | `i32*` fully posed vertices — the **ground truth**, and what the native pass draws directly |
| `pr + 0x28` | bit 0 = visible; this is how COB `HIDE` / `SHOW` reaches us |

Accumulated down the tree that is `P = parent · T(off + move) · R(turn)`. The **rest** transform
is the same walk with `move` and `turn` zero, which collapses to a translation by the
accumulated offset — so the matrix the shader wants is

```
M = P · T(−restOffset)
```

and it is the **identity** for a piece the script has not touched. A model whose pieces never
move therefore renders exactly as it did before any of this existed.

The unit's body yaw is *not* in `P`: the engine bakes it into `pr + 0x22` but the replacement
pass applies it itself, in the vertex shader, after the piece matrix.

### The turn conventions, and the one that is still a guess

Measured against `pr + 0x22`:

- `turn[0]` rotates about **X**, `turn[1]` about **Y**, `turn[2]` about **Z**, each with a
  **positive** angle through the same `rot2` the effects path uses. This is **not** the index
  order `emit_fx_model` uses (`turn[0]` about Z there) — that reads a different struct, and the
  two should not be assumed to share a convention.
- **The order is `Z, X, Y`, and that is now read, not guessed.** [VERIFIED 2026-09-07, tacob
  landing 4.] `UNITS_PieceOffset 0x43DEF0` composes through `0x4B6CC0`, which rotates the
  `(x,y)` pair by the `+0x14` word first, then `(y,z)` by `+0x10`, then `(x,z)` by `+0x12` —
  exactly the order this pass uses, and the pairing of word to axis with it. The sample could
  not distinguish the six orders because every piece in it turned about one axis; the
  disassembly can. `exe-reverse-engineering.md` §"The piece transform".
- **`MOVE` is a delta in the parent's frame, added to the rest offset before the rotation** —
  also read rather than inferred. `0x43DF2A..0x43DF55` adds `prim+0x04/+0x08/+0x0C` to the
  node's `+0x10/+0x14/+0x18` and only then walks up the chain, which is what this pass's `d`
  already does. The live check is `tools/tacob pose-check --all`, which rebuilds the eight
  cobtrace fixtures' posed vertices from those rules and diffs them against `P_VBUF`: exactly
  0 on the kbot, the building and the ship, and on the four fast movers a residual that equals
  this pass's own `err=` on the same dump line (the vertex buffer being a frame behind the
  pose the dump sampled).

### Hidden pieces

A piece the script has HIDden gets an **all-zero matrix**, which collapses its triangles onto
the model origin — zero area, no fragments. This matters because it is how the muzzle flares
stay invisible, and a stock `Create` opens with a run of `HIDE`. `ta3do` drops those pieces at
export by reading the COB prologue, so the usual asset never exercises the path; exporting with
`--show-hidden` does, and the frame comes back within 5 pixels of the export without them.

The zero matrix is also what makes a **muzzle flash** on a hires model possible at all: the
engine hides and shows the flare piece by name, exactly as it does for the 3DO, so the flash
works if — and only if — the `.glb` still carries that piece's geometry. `ta3do --keep-flares`
keeps precisely the pieces the script hides at Create and shows again later, which is that
geometry and nothing else; see [model export](model-export.md). Untested in game so far: no
hires model with a flare piece has been rendered firing.

## What the renderer does that the native pass cannot

A modern asset pushed through the engine's palette-index shader comes out flat, unlit and
unmipmapped, so replacement meshes get their own GL program: sRGB in / linear light / sRGB out,
per-pixel lighting against the engine's *own* light direction, normal maps through a tangent
frame derived from screen-space derivatives (glTF gives no `TANGENT`), a small GGX lobe for
metallic/roughness, and mipmaps — a 1024 px texture on a 40 px sprite crawls without them.

It shares the **frame contract** with the native pass, which is what makes a replacement unit
composite with engine units instead of floating above them: same FBO and supersample, same
premultiplied output, same viewport-px → NDC mapping, same painter's **depth key** encoding,
same scaffold occlusion test, same fog-grid discard, same waterline and digger rules, same
engine anchor and body yaw, same silhouette shadow (black at 50%, +5 px, at ground height).

Live tuning, re-read every 30 frames from `gamedir/tagpu_hires.on`: `anchor=` (0 = modern PBR,
1 = shade the albedo by the engine's own SHD ramp, anything between mixes), `sun=`, `amb=`,
`nonormal`, `log`.

## Verifying an import

Four checks, in the order they are worth running:

1. **`err=` in the pose dump.** `touch <gamedir>/tagpu_posedump.on` (it deletes itself after one
   dump) and read `tagpu.log`. Per piece it prints the rest offset, the `MOVE` delta, the `TURN`
   triple, the first three vertices raw and posed, and `err=` — the largest disagreement, in
   model units, between the engine's own posed vertices and what `pose_accum` reconstructs from
   the fields. **It must read `0.00`.** Anything else means this unit's script does something no
   sample covered, and it names the piece.
2. **`hires: pose bound N of M`** in the log, for the name binding.
3. **An A/B against the engine's own render.** `tacli glshot`, then move `hires/<name>.glb`
   aside, wait ~2 s for the reload to notice, `glshot` again, and diff. At rest the two must
   land on the same anchor with the same orientation and the same shadow. Do it at a facing
   that is not a multiple of 90.
4. **Video for the animation** — 60 fps `x11grab`, the **ta-capture** skill. A walk cycle is
   ~0.5 s, so a 3 fps burst of stills aliases it away. Take the window's *absolute* position
   from `xwininfo`, not from `tacli ls --json`: that reports the position the WM was asked for,
   and a reparenting WM does not have to honour it.

## Not done

- **Team colour comes from the file, not the player.** The exporter bakes ARM blue into
  `baseColorFactor` and marks its intent in `asset.extras.teamColour`; the owning player's
  colour is not read, so a CORE-owned replacement is still blue, and a linear factor over a
  neutral texture renders darker than the engine's palette remap. Driving the factor from the
  owner is a real, separate feature — and the project's existing finding is that TA's team
  colour is a multi-frame GAF entry, not a palette remap.
- **`.gltf`, `data:` buffers, sidecar `.bin`, node `matrix` and non-float / normalized
  `TEXCOORD_0` accessors are implemented but unexercised.** Only the GLB + TRS + float path has
  ever run.
- **No wreck or feature replacement**: the slot is keyed on the unit def name only.
