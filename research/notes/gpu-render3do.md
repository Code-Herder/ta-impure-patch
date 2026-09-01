# Phase B — real GPU 3DO geometry into TA's compositor (render3do)

*The Phase B renderer: `tagpu/ddraw/src/tagpu_render3do.c` replaces the G6 proof gradient
with a real GPU render of the unit's engine-posed 3DO, written into the per-unit composite
(`Object3do+0x10`) that TA's own blit stamps onto the frame. **Proven live 2026-08-31**: the
ARM Commander drawn by our GL pipeline is visually a faithful reproduction of the engine's
own sprite (shots below). This note records the renderer design and the new RE findings it
took — chiefly the real, runtime-verified semantics of `Model3DOFace`'s colour/texture
fields, which differ in practice from what the loader-era view suggested.*

Evidence tags: **[LIVE]** = observed this session in a running skirmish (log
`tagpu.log`, screenshots in `assets/shots/`); **[TAMEM]** = `vendor/TADR/src/DDraw/tamem.h`.

## The renderer in one paragraph

Per frame, for the target unit (armed via `tagpu_writeback.on`, first token = unit type):
walk the inline `PrimitiveStruct[]` at `Object3do+0x22` (visible pieces only, `prim+0x28`
bit0), take the **engine-posed vertex buffer** (`prim+0x22`, i32 16.16 ×3, unit-model
space — fresh because we do *not* suppress `DrawUnit`, so TA's lazy repose keeps running),
triangulate each `Model3DOFace` as a fan from index 0, and render flat-coloured triangles
into a GL FBO with **TA's own dimetric projection**: sprite `px = x + HotspotX`,
`py = (−z − y/2) + HotspotY`, GL depth `= 2y − z` (the true view-ray depth; nearer wins).
The FBO is 640×640 (stock composite is AABB-capped 600×600); only the sprite's `W×H`
corner is used, sized/hotspotted from the header TA itself wrote into the composite.
Readback rows map 1:1 onto the top-down colour plane because the projection flips Y
(sprite row 0 → NDC −1 → `glReadPixels` row 0). The composite's **depth plane is left
untouched** — it encodes elevation (`y + const`) and is only consumed by the cargo
z-merge `0x4B90A0`; the ground blit reads the colour plane alone, and our silhouette
matches the engine's because we render the same posed verts under the same projection.

**The palettisation trick.** There is no nearest-match pass at all: faces resolve to TA
palette *indices* (below), the fragment shader writes `index/255` into the red channel of
an RGBA8 attachment (flat-interpolated, byte-exact both directions), the FBO clears to
index 1 (= composite ColorKey/transparent), and `glReadPixels(GL_RED)` returns the finished
8bpp plane ready to `memcpy` row-by-row into `PtrColour`. Exact, and it keeps the whole
composite path 8-bit clean. (True-colour output later means swapping this single encode
step for a real palettiser or — the endgame — bypassing the 8bpp composite entirely.)

## Model3DOFace colour/texture fields — runtime truth [LIVE]

`Model3DOFace` (0x20 B, offsets per [TAMEM] confirmed): `+0x00 pColorTable`,
`+0x04 VertexCount`, `+0x08 pTextureName`, `+0x0C pVertexIndices (u16*)`,
`+0x10/14/18 TexState a/b/c`, `+0x1C flags`. What the loaded records actually hold:

| Case | Field state (live armcom) | Colour source |
|---|---|---|
| Flat-coloured face | `pColorTable` = small int? (none seen on armcom) | treat `1 < v < 255` at `+0x00` as a literal palette index |
| **Plain textured** (arms, legs, head…) | `+0x08` **zeroed** after resolution; **`+0x10` → inline `GAFFrame[]`** (stride 0x18, one per anim frame; texture frames use **ColorKey 9**, `PtrDepth` garbage) | sample frame 0's 8bpp plane (we probe centre + 4 quarter texels, skip ColorKey) |
| **Team-colour textured** (torso) | `+0x10` = non-pointer scalar (`0x00060000`), `+0x14` = 1, **`+0x18` → GAF anim entry** `{u16 nFrames; u16; u32; char name[32]; ptr @+0x28 → inline GAFFrame[] table}` (armcom torso: 8 frames, entry name reads "glow") | sample the table's frame 0 (per-player frame selection TODO) |
| **Nothing resolved** (`ground` footprint quad, some interior faces) | `+0x00` = 0, `+0x10` = 0, `+0x18` = junk | **draw nothing** — the engine skips these too; painting them was the "green slab" bug |

Two hard-won gotchas:

- **`pColorTable` can hold deterministic non-pointer garbage** (`0x01E11F00` on live armcom
  faces) that passes a naive "is in heap range" check and faults on deref. The first Phase B
  run crashed exactly there. Never dereference it; and note a fault report at a
  `0x7B4xxxxx` address is not necessarily wine's kernelbase — **our fork DLL loaded at
  `0x7B4D0000`** in that session (`+loaddll` is the arbiter).
- **GAF anim frame tables are inline arrays of `GAFFrame` records** (stride 0x18), not
  arrays of pointers — `*(entry+0x28)` is already the first frame's address.

## Sequencing & integration

`writeback_paint()` runs **last** in the overlay pass (`tagpu_overlay.c`): the FBO pass
clobbers viewport + framebuffer binding (restores binding 0), which is safe there because
nothing uses GL afterwards and the fork re-establishes state at the top of the next frame.
No suppression: the engine builds/reposes the composite each frame, we overwrite the colour
plane after the blit, TA re-blits our pixels next frame (one-frame latency, invisible).
Geometry reads are guarded (`ptr_ok` + `IsBadReadPtr` per piece/face) while layouts firm up.
Cost for armcom: ~185 triangles, one 36×43 readback — negligible.

## Proven live — 2026-08-31 [LIVE]

`render3do: 185 tris (555 verts) -> 36x43 hotspot=(18,25)` steady at speed; the on-screen
commander is our GL render: blue ARM shoulder pads, grey metal torso/head, twin gold legs,
dark gun/nanolath arms — matching the engine sprite feature for feature. Shots:
`phaseb-render3do-zoom.png` (our render in-game), `phaseb-render3do-compare.png`
(engine sprite vs ours side by side), `phaseb-render3do-scene.png` (whole scene).

## Real texturing, depth plane & the diff harness (same day, later)

All landed live the same session:

- **Texture atlas.** Faces render with their real GAF textures: each resolved `GAFFrame`'s
  8bpp plane is uploaded once into a 1024x1024 `GL_R8` atlas (shelf-packed, 1px gaps) and
  NEAREST-sampled — the sampled texel *is* the palette index, so the whole path stays
  index-exact with no palettiser. Quad corners map to texture edges
  (`v0..v3 -> (0,0)(1,0)(1,1)(0,1)`); the fragment shader discards texels equal to the
  texture's own ColorKey (9 for unit textures), which is what makes the transparent
  'ground'-quad style faces disappear exactly as in the engine. Team-colour faces pick
  `frameTable[owner]` (clamped; owner 0 verified blue — other colours pending a visible
  non-P0 unit).
- **Own depth plane** via MRT: a second `R8` attachment receives `modelY + 0x32`, read
  back and written over the composite's depth plane. The engine's formula was pinned
  empirically with the harness: its depth plane over the commander spans 49..89 for
  model-y -1..39 ⇒ **depth = y + 0x32 (50)**, 0 = transparent/far (the earlier
  "+0x4B if not cloaked" reading was wrong).
- **Engine-vs-ours diff harness.** `tagpu_diff.trigger` forces one engine rebuild of the
  target's composite (zero the `TimeVisible` stamp at `Object3do+0x04` *and* set
  pose-dirty `+0x08` — pose-dirty alone reposes but does **not** re-rasterise), skips our
  paint for a frame, then dumps the engine's fresh colour+depth planes and ours as PGMs
  (`tagpu_eng.pgm`/`tagpu_engdep.pgm`/`tagpu_ours.pgm`, atlas as `tagpu_atlas.pgm`).
  Results: **silhouette coverage 96.4% identical** (55/1548 px differ, all at edges); of
  overlapping opaque pixels 55% differ in index, but the disagreement is edge/texel-phase
  scatter from TA's truncating 1997 scanline rasteriser vs GL rules — colour bands and
  structure agree, no global offset wins (best shift is (0,0)), and our output is
  bit-deterministic frame to frame. Texel parity with the software rasteriser is
  explicitly a non-goal (the endgame replaces it at native resolution).
- **`all` token**: `tagpu_writeback.on` containing `all` renders every unit the engine
  has built a composite for, not just one type.

## Loose ends (next in Phase B)

1. ~~**Own the draw**~~ — DONE same session, a finer cut than suppressing DrawUnit:
   detour only the two software rasterisers so the engine keeps every bit of bookkeeping
   including the lazy repose and the blit — see [own-the-draw](own-the-draw.html).
   Native-resolution output past the 8bpp composite is the next-phase goal.
2. ~~**Team colour verify**~~ — DONE: a 4-AI permanent-LOS skirmish (registry-preset, sim
   at +10 via TA's own speed keys) put the white AI's ARM commander on screen: white team
   badge on exactly the faces that are blue on ours, red trim on CORE structures —
   `frame[owner]` is the right mapping (shot in the roadmap gallery).
3. ~~**Animated pose test**~~ — DONE in the same run: the white commander was captured
   mid-stride between build sites; walking/turning units render correctly from the
   engine-posed vertex buffers (which stay fresh — own-the-draw keeps the engine's repose).
4. **Multi-frame plain textures** — engine sprite is byte-stable over 10 idle minutes, so
   nothing free-runs; any animation is event-driven. Revisit only if a mismatch shows up.
5. **Build-state rendering** — with own-the-draw armed, units under construction render as
   complete models instead of the engine's scaffold/nanoframe look (we skip the nanoframe
   rasteriser 0x459C70 but paint full geometry). Cosmetic; proper build-state visuals are
   next-phase work.

## Per-face directional shading (G10, Phase C — 2026-08-31)

First eye-candy delivery: every face is lit by orientation, inside the 8bpp composite.

**Mechanism** (all in `tagpu_render3do.c`):

- **Normals, CPU-side:** per triangle from the engine-posed verts. 3DO winding is not
  trusted: the cross product is flipped into the hemisphere of the model-space
  toward-camera axis `V=(0,2,-1)/√5` (the nearness gradient of the view depth `2y−z`) —
  any face that survives the depth test faces the camera, so this always yields the
  outward normal.
- **Sun** `L=(-0.35,0.80,-0.49)`: high, screen upper-left, slightly toward the viewer.
  Intensity `I = n·L` maps to brightness factor `1.0 + 0.30·I`.
- **Palette-aware shade LUT, GPU-side:** brightness in a palettised world = index remap.
  A 256×32 R8 texture: row `r` maps every palette index to the palette's nearest entry
  (RGB distance) to `rgb·(0.60 + 0.025·r)`. Row 16 is factor 1.0 exactly and forced to
  identity — shade-neutral output is bit-identical to the unshaded renderer. Candidate
  indices 2..254 only, so reserved 0/1 (ColorKey)/255 are never emitted. Built once from
  the live palette (`*(0x511DE8)+0x143A7`) on first in-game frame.
- **Plumbing:** vertex attr 4 carries the quantised LUT row (flat per face); the FS remaps
  the sampled index through the LUT after the ColorKey discard, for textured and
  flat-coloured faces alike. `tagpu_shade.off` in the game dir disables the remap per
  frame — A/B comparison in a single run.

**Live proof (4-AI Painted Desert):** the A/B pairs in the roadmap gallery — on the ARM
vehicle plant the left-facing sloped walls catch highlights and right-facing inner walls
darken (`phasec-shade-vp-{on,off}.png`), the CORE solar's flat-grey wedges split into
distinct light/dark faces (`phasec-shade-solar-{on,off}.png`), and the commander's torso
top brightens over darkened flanks (`phasec-shade-armcom-{on,off}.png`). Engine features,
wreckage and particles coexist untouched; no crash across a full AI war at +10 sim speed.

**Tuning headroom:** amplitude is the single constant `0.30` (CPU quantise step); the LUT
covers factors 0.60–1.375, so up to ±0.37 needs no LUT change. Glow/emissive faces are not
yet exempted from darkening — revisit if lit windows look wrong (windows should stay lit).

## 2x supersampled edges (Phase C — 2026-08-31)

The FBO pass renders at 2x sprite resolution (vertex positions and `uWH` scaled; falls back
to 1x if the doubled sprite exceeds the 640 FBO) and the readback is reduced by a
**palette-aware majority filter**: a final pixel is opaque iff ≥2 of its 4 subsamples are
(preserving silhouette coverage), colour = the most frequent opaque index (never a blend —
output stays pure palette), depth = nearest opaque subsample. Smooths the stair-steps on
diagonal silhouettes and interior face boundaries (edge position is effectively estimated at
half-pixel precision). Toggle: `tagpu_ss.off`. Cost: a 2x readback + a trivial CPU reduce.

## Engine shade table adopted + build-state scaffold (Phase C round 2 — 2026-08-31)

**Shading now uses TA's own SHD table.** The 32×256 table the engine's optional Gouraud
rasteriser (`0x459C70`) indexes lives at `*(TAProgramStruct+0xC4)` (`prog = *(0x51FBD0)` —
a STATIC struct: don't heap-range-check it). We self-calibrate at first use: neutral = the
row with the most identity entries (row 15, 232/256), direction from end-row luminance
(dir=1, brighter upward), then quantise our per-face intensity to ±12 rows around neutral.
Colours are now exactly what TA's own shaded mode would have produced; the computed
nearest-match LUT stays as fallback.

**Build-state (nanoframe) scaffold implemented in-shader** ([build-state](build-state.html)
Option B). Empirical finding first: the engine's blit-time scaffold `0x458DD0` does NOT
fire on our written pixels (a 24%-built CORAP rendered complete under owndraw; the engine
reference showed partial reveal) — so we own the look. Per unit: `nano = *(float*)(unit+
0x104)` (fraction remaining, also logged in the roster now); CPU picks the engine's stage
A–E threshold `t` + (above, band, below) colours incl. the two triangle-wave blues over
palette ramp `0xA0..0xAF` (tick `TAdynmem+0x38A47`, salt `unit+0xA8`); the FS classifies
each fragment by height byte `d = modelY+0x32` — erase writes ColorKey but KEEPS depth
(engine semantics: the depth plane always holds the full model). A second `GL_LINES` pass
draws every drawable face's closed outline in blue2, depth-tested `GL_LEQUAL` (`0x458FA0`
hidden-line behaviour). Toggle `tagpu_nano.off` reverts to complete-model rendering.
Live, full sequence captured on an ARMWIN build (roadmap gallery): the reveal stage
matches the engine A/B (`phasec-nano-{ours,engine}.png`), mid-build shows texture + the
wireframe scaffold on every face edge (`phasec-nano-scaffold.png` — the ramp renders GREEN
on Painted Desert's palette, as does the engine's), and the handoff to the finished shaded
model is clean (`phasec-nano-complete.png`). Known tuning gap: in stages A/B (first ~20%)
the engine already shows a solid colour-ramp mass (`phasec-nano-engine-solid.png`) where we
draw wireframe only — boundary/sweep detail, cosmetic, 20s per building. Our wireframe is
denser than the engine's sparse 2px-per-scanline look — kept deliberately.

## Build-state verified end-to-end (filmstrip, 2026-08-31)

The marker-verified filmstrip (roadmap gallery: `phasec-sbs-solar-filmstrip.png` — engine and
ours interleaved seconds apart across one full ARM-solar build, shot phases proven by log
markers matched to screenshot mtimes) confirms the in-shader scaffold reproduces the engine's
staging exactly. The earlier "stage A/B tuning gap" is RETRACTED — two engine-side effects
had been misread as fill differences: (1) the scaffold fill/band colours shimmer through the
palette ramp at ~2Hz (the two triangle waves), so pairs sampled seconds apart legitimately
differ in tone; (2) the solid dark under-slab at early stages is the engine's CACHED
nanoframe shadow (`Object3do+0x14`, slant-projected, drawn at every stage — shadows-cloak.md),
which appears identically under our pixels because the blit draws it engine-side. The stage
boundary constants and per-stage colour args were also re-verified directly in the `0x458DD0`
dispatch bytes (cmp 0xEB/0xC8/0x73/0x1E; push sequences match build-state.md §3 exactly).

**Cloak note (by construction):** the cloak path is routing inside blit `0x459200`
(`unit+0x10E` bit2 → ALP 50/50 blend of the composite; enemies never draw it). Since our
pixels live in the same composite, cloak applies to our render engine-side with no work on
our part. Formal live test pending an unlocked session (needs the cloak UI toggle), same
class as the G5 replay byte-diff.
