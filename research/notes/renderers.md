# Classic and Classic++ — the two GPU renderers

*tagpu has two looks and they do not blend. This page is the record of what each one is,
the decisions taken for bringing Classic++ into the game, what the code already settles so
nobody re-derives it, and what is still open. Decisions are dated; measurements say where
they were taken. The look itself, every knob and every number, is on
[tascene — browser render lab](tascene-design.html); the engine facts behind the shadows are
on [shadows & cloaking](shadows-cloak.html).*

Evidence tags: **[SOURCE]** = read from the code named; **[MEASURED]** = a live or offline
measurement with the numbers; **[DECIDED]** = a choice made by the project owner, with the
date; **[OPEN]** = not settled.

---

## 1. The two renderers

| | **Classic** | **Classic++** |
|---|---|---|
| What it is | what tagpu draws today; the lab's `lane=classic` | the lab's `lane=classicpp` at its current defaults |
| Claim | **pixel parity** with itself: it must not move by a pixel, and the lab's parity ritual is the proof | none against the engine; it is judged by eye and measured against the lab |
| Textures | the engine's 8bpp GAF frames as palette indices, `GL_NEAREST`, the `PALETTE.SHD` shade LUT | restored true colour (the `unditherer` full model), 4-texel padded atlas, trilinear to mip level 2, 4× anisotropic; indexed frames stay `NEAREST` |
| Terrain | the engine's 32-px tile blit, no height, no light | the same tiles, per-pixel lambert from the heightfield normal, **normalised so level ground is exactly 1.0** (the art is already lit) |
| Units | per-face shade row from `SH_L` through the 32-row LUT | per-pixel lambert in map space from the posed face normal, same level normalisation |
| Shadows | the engine's rules: 5-px silhouette drop for mobiles, the cached slant for structures | a depth map along `shadowsun`, PCSS-lite (8-tap blocker search, 16-tap Poisson PCF), receiver-plane bias, per-caster length `14 + 0.25·height`; hills cast and receive |
| Suns | one, `SH_L = (−0.35, 0.80, −0.49)` in model space | **three** knobs: `sun=324.5,53.1` (terrain), `unitsun=215.5,53.1` (= `SH_L` in map space), `shadowsun=225,40` |
| Fog of war | the engine's per-index grey LUT | one RGB rule after lighting (§2.6) |
| Supersampling | 2× box, `tagpu_ss.off` | the same |

The Classic baselines, re-shot 2026-09-04 from the merged tree after `main` came in at
`1696be2`: `tascene-parity.json`, default `md5 6f7ad6b122591d6db2a2b028938be5b3`, `ss=1`
`md5 9c9ab215099e581288b24cc47d41f9be` — both unchanged **[MEASURED]**.

---

## 2. Decisions for the in-game Classic++  [DECIDED 2026-09-04 unless noted]

### 2.1 The suns stay constant
The terrain sun is the engine's own, `324.5,53.1`, on every map. The lab's `artlight`
survey found the tile art painted from a different azimuth on 78 % of stock maps and from the
opposite side on 23 of them, so a constant sun will fight the painting somewhere. **Decided:
ship the constant and look.** The engine itself lights units and casts shadows from fixed
directions whatever the map, and the per-map option (`artlight`'s peak azimuth) stays on the
shelf until a map actually reads wrong.

### 2.2 Aircraft: prototype before engine work
The lab has no aircraft. A parallel light at 40° elevation drops a plane's shadow about
1.2× its altitude away, up-right, so the cue under the plane is lost. **Decided: prototype
in the viewer first** — the candidates are the Classic drop shadow for airborne units, or
`shadowlen`'s vertical scale applied to altitude too. Nothing is coded until the viewer
shows it.

### 2.3 Water: the seabed stays shaded and shadowed
The viewer lights and shadows underwater cells by the seabed, and never reads the sea level
in its lighting or shadow receivers **[SOURCE `tools/tascene-view.html`]**. **Decided: keep
it** — it is part of the approved look. Boats go into the same viewer prototype as aircraft,
so the shadow of a hull on the seabed is seen before it is built.

### 2.4 Hires glb units are out of scope
`tagpu_hires_draw.c` lights in linear space with a GGX lobe; Classic++ is a lambert in sRGB
space, and hires meshes are in no depth pass. **Decided: not a Classic++ concern.** Revisit
after the port.

### 2.5 The restorer runs in the DLL, on the GPU, per GAF frame
The restorer is a plain residual CNN — `unditherer/model.py`: 3×3 convolutions, ReLU,
BatchNorm, output = input − net(input); the **full** model is 12 layers × 64 channels,
373,443 parameters, eval PSNR 30.48 dB; **tiny** is 6 × 24, 22,251 parameters, 29.79 dB
**[SOURCE `unditherer/models/models.json`]**. The DLL is a 32-bit MinGW build under Wine and
cannot host torch or onnxruntime **[SOURCE `tagpu/ddraw/Makefile`]**, but a chain of
convolutions is a chain of fragment-shader passes.

**Decided: port the full model only, as GLSL, and run it on each GAF frame the moment the
atlas first sees it.** Not the tiny one, not offline files. Per frame the full model is about
750 k FLOPs per texel — a 64×64 frame is 3 GFLOP, well under a millisecond on the
RTX 4070 at insert time. Consequences and port rules:

- **Per frame, in isolation.** The receptive field is 25 px; running over a packed atlas would
  pull neighbouring frames into each other's edges.
- **The Python path's gates are the spec** **[SOURCE `unditherer/restore.py`, `infer.py`,
  `classical.py`]**: colour-key texels are inpainted before the network (OpenCV Telea,
  radius 3) and alpha is restored from the key mask after; a frame whose opposite edges
  agree within 12 levels (`is_tileable`) is wrap-padded by the depth, every other frame is
  zero-padded (the convolutions' own `zeros` padding); output is clipped to 8 bits. The GLSL
  port replaces only the inpaint, which has no shader twin, with a few dilation passes.
- **Activations live in a 2D texture array**, one RGBA layer per 4 channels, RGBA32F, so a
  64-channel layer reads one texture unit and is written 8 attachments per pass — two passes
  per convolution, 24 per frame. BatchNorm is folded into the weights at export; the weights
  ship as one float texture from a Python export verb.
- **The lab's Classic++ lane switches to the same GLSL restorer**, extracted into the pack the
  way the Classic shaders already are. A GLSL port is not byte-identical to ONNX, and two
  restorers would make the game and the lab disagree by a level or two forever. This also
  removes the shipping-format question entirely: no manifest, no content hash, mods and
  third-party units are restored on first sight like stock.
- **The atlas** is a new RGBA object beside Classic's indexed one, which does not change:
  4-texel replicated pad, 4-aligned allocation, mip levels 0–2 rendered at insert.

### 2.6 Fog of war: one RGB rule after lighting
The engine's grey band remaps each palette index to the palette entry nearest its own
R+G+B mean (`*(TAProgram+0xCC)`, [shadows & cloaking](shadows-cloak.html) §1) and Classic
does the same on the index **[SOURCE `tagpu_glsl.h` `TAGPU_GLSL_FOG_SHADE`]**. Classic++
multiplies colours by light and shadow, and restored texels have no index. **Decided: in
every Classic++ pass, compute the lit and shadowed colour, then in the grey band replace it
with its own R+G+B mean, without palette quantisation.** Shadows and relief survive as darker
grey; unexplored stays palette-0 black; the unit and effect discards in the band stay as
they are. One rule, one place.

### 2.7 The shadow map is anchored to the map
The viewer rebuilds its light-space bounds every frame from the eye and the visible height
range, with no texel snapping (`shadowFrame` **[SOURCE `tools/tascene-view.html`]**). A still
shot cannot show it; in the game every scroll would move the texel grid under the PCF kernel
and every edge would crawl. **Decided: a map-anchored texel grid with a fixed depth range.**
World-per-texel is a function of zoom only, the map origin sits on a texel corner, the
window over the grid moves by whole texels, and the depth range is the map's full height
range plus the caster allowance, held constant — so the per-frame height scan goes too.
The blocker search and the PCF kernel stay in world units as the lab has them.

### 2.8 Hills cast: a static per-map heightfield mesh
The terrain gather emits screen-space quads with no height **[SOURCE `tagpu_terr.c`]**, so
the depth pass has nothing of the ground to draw. **Decided: one world-space VBO of the whole
heightfield at the engine's 16-px cells, built when the map loads** from the height byte the
feature pass already reads (§3), drawn once per frame in the depth pass. Two Continents is
672 × 800 cells. It joins the GL reset protocol like every other object.

### 2.9 Classic++ applies at every zoom
Under `vpwide` the view reaches 16× the area at the 0.25 floor. **Decided: all zooms.** The
shadow map's world-per-texel follows the zoom: 2048² at zoom ≥ 0.5, 4096² below; the PCF
minimum radius follows the texel. Restored textures are mipmapped to level 2, which covers
0.25 at `ss=2`. The unit vertex cap is unchanged because the depth pass reuses the frame's
one vertex buffer. The softer shadows at the floor are judged in play — the lab's Classic++
lane runs at zoom 1 by construction and cannot show them (§4).

### 2.10 Settings: an in-game menu the DLL draws
**Decided: a small "Options" button at the top right, over the engine's top bar, opening a
DLL-drawn panel; v1 is buttons and steppers for every Classic++ knob and the renderer switch.**
Why it fits:

- The composite draws our non-empty pixels over the engine's frame outside the viewport
  (`if (empty) discard; frag = c;` **[SOURCE `tagpu_native.c` composite shader]**), so the
  bar is ours to draw on.
- The top-right is empty: on the 1024-wide engine frame the metal and energy bars end near
  x ≈ 620 and the right ~400 px are bare panelling **[MEASURED
  `assets/shots/feat-ownership.png`, 2026-09-04]**.
- The DLL sees every window message before the game and already swallows the wheel
  (`tagpu_zoom.c`), so button clicks are consumed in the wndproc and never reach the engine.
- Our pixels cover the engine's cursor where they land, so **the menu draws its own cursor
  while the pointer is over our UI.**
- Text: the engine's own GUI font, whose glyph table `tagpu_ui.c` already dereferences; a
  small embedded font is the fallback if the glyph format fights back.
- **State: the menu is a front end, not a store.** On/off switches stay the house trigger
  files — `gamedir/tagpu_classicpp.on` selects the renderer, polled per frame like
  `tagpu_hires.on`, absent = Classic — and the menu creates and deletes them. Numeric knobs
  live in `gamedir/tagpu_classicpp.cfg` as `key=value` lines re-read on mtime change, keyed
  like the lab's URL parameters (`sun`, `unitsun`, `shadowsun`, `amb`, `penumbra`,
  `shadowlen`, `shade`, `aniso`, `shadows`). A human editing files, a tacli verb and the menu
  drive the same state. The restorer weights ship as `gamedir/tagpu_classicpp.model`.

### 2.11 Two small calls made by the implementer
- **The unit vertex stream** grows from 11 to 15 floats: the map-space normal and the
  world height join `x, y, depthEnc, u, v, flat, ck, shadeRow, wx, wzp, vy`. Face normals are
  already computed on the CPU for the shade row **[SOURCE `tagpu_render3do.c` ~794]**, so
  this is plumbing; about 2.9 MB per frame at the 49 152-vertex cap.
- **Nanoframes** go through the same unit shader (G13l), so they are lit and shadowed for
  free. Their band and fill colours are palette indices, so the restored-texture branch looks
  them up through the palette texture; and they cast nothing, as the native pass already
  rules **[MEASURED, build-state §7]**.

---

## 3. What the code already settles — do not re-derive

- **No engine shadow to suppress.** Since G12c/G13k the native pass draws every unit and
  structure shadow itself; Classic++ skips its own two sub-passes. No byte patch is needed,
  so the landing review runs at `medium`. GAF-baked feature shadows stay, as in the lab.
- **The heightmap is already read.** `FeatureStruct+0x04` is the per-16-px-tile height byte,
  the grid is `*(main+0x14287)` with stride `0xD`; `tagpu_feat.c` reads four of them for every
  feature anchor **[SOURCE]**. The terrain shader already carries world x and z per vertex.
  The work is one R8 height texture per map plus a sampler.
- **GL 3.3 is in effect.** The native shaders are `#version 330 core` and compile, so
  sampler objects are available; `render_ogl.c` still *requests* 3.2 core — bump the request
  so the contract matches **[SOURCE `render_ogl.c` 189–192, `tagpu_native.c`]**.
- **Fog cannot leak through shadows.** The unit gather drops units whose anchor tile is
  unexplored or unseen, and cloaked enemies, before anything is emitted; its rect has 256 px
  of slack on every side, more than the lab's 192-unit caster margin **[SOURCE
  `tagpu_native.c` gather]**. The depth pass draws the gathered set and nothing else.
- **The frame order needs no restructuring.** Terrain, features and units are gathered before
  any pass renders; a depth pre-pass over the frame's unit buffer slots in before the terrain
  render with no frame lag **[SOURCE `tagpu_native_frame`]**.
- **Team colour is per-owner GAF frames** (`frame[owner]` from the anim's inline table,
  `tagpu_render3do.c face_texframe`). With the restorer in-DLL each variant is restored on
  its own when first drawn; nothing to enumerate.
- **The composite over the top bar** and the wndproc interception are established, §2.10.

---

## 4. Open  [OPEN]

- **Aircraft and boats** — the viewer prototype of §2.2/§2.3 has not been built.
- **Hires glb under Classic++** — lighting model, depth pass, shadow read-back, silhouette
  shadow off; deferred by §2.4.
- **The lab's Classic++ lane is zoom 1 only**, so the zoom-out look of §2.9 has no lab
  reference until the lane gains `uZoom`.
- **Nanoframe wireframe back edges** show through the unbuilt part — inherited from G13l,
  needs a stencil pass per nanoframe.
- **The restorer port's exactness** against the Python path is unmeasured; it becomes moot
  once the lab runs the port (§2.5), but the first port should still be diffed against the
  ONNX output on a handful of frames so a transposed weight is caught.
- **The engine's own key map** was not needed once the switch became a button; if a hotkey is
  ever wanted, it has to be checked against TA's bindings first.

---

## 5. Order of work

1. **Restorer port**: weights export verb, the GLSL passes, diff against ONNX on sample
   frames, the lab lane switched to it.
2. **RGBA atlas** at insert: pad, align, mips 0–2; the shader's restored-texture branch.
3. **Unit shading** in map space: normal and world height per vertex, `LAB_LIGHT` into
   `tagpu_glsl.h` with the viewer's uniform names.
4. **Terrain lighting** from the height texture, level-normalised.
5. **Shadows**: the map-anchored grid, the depth pass over units and the heightfield mesh,
   the read-back in terrain and unit shaders, the two Classic shadow sub-passes off.
6. **Fog** rule of §2.6.
7. **Switch and cfg**, then the **menu** of §2.10.
8. **Verify by running it**: `tascene ab` in Classic against `lane=classic` (nothing moved),
   in Classic++ against `lane=classicpp` (the port measured). Engine code triggers the Opus
   review at `medium` and the documentation pass; a human declares it ready.

The handoff that preceded this page, and the 22 lab commits it rests on, are on the
`worktree-gpu_render` branch; the lab commits land first as one finished unit.
