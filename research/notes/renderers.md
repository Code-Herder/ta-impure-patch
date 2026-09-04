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
| Textures | the engine's 8bpp GAF frames as palette indices, `GL_NEAREST`, the `PALETTE.SHD` shade LUT | **restored true colour for all three atlases** — terrain tiles, feature sprites and unit textures — through the `unditherer` full model. Units: 4-texel padded atlas, trilinear to mip level 2, 4× anisotropic. Tiles and sprites: 1:1, `NEAREST`. *In the game so far: the terrain (G14a).* |
| Terrain | the engine's 32-px tile blit, no height, no light | the same tiles in restored colour, per-pixel lambert from the heightfield normal, **normalised so level ground is exactly 1.0** (the art is already lit) |
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

### 2.5 The restorer runs at map load, inside the DLL, through ONNX Runtime — spike first

**Static atlases, built once per map.** Everything Classic++ restores is in memory when a
map loads: the tile set is built by `LoadMap` and never changes after (tagpu already builds
its terrain atlas once per map for that reason **[SOURCE `tagpu_terr.c`]**), and every unit
definition, model and texture GAF is loaded at game start. tagpu's unit and feature atlases
fill lazily on first draw today only as an implementation choice **[SOURCE
`tagpu_render3do.c atlas_get`, `tagpu_gaf.c`]**. **Decided: Classic++ builds its three
restored atlases at map load, so a new unit on screen costs nothing.** The lab restores the
same three (`terrain/atlas.rgba.bin`, `features/…`, `units/…` **[SOURCE `tools/tascene
build`]**); the base pack's 5192 restored frames are 5062 tiles + 53 feature frames + 77 unit
frames.

**The workload, per map load** **[MEASURED 2026-09-04]**: Two Continents has 5062 tiles =
5.2 M texels; the whole install's `textures/*.gaf` is 11 files, 546 entries, 753 frames
(team-colour variants included) = 1.35 M texels, median entry 32×64. Feature frames are
small beside these.

**The model is a plain residual CNN.** `unditherer/model.py`: 3×3 convolutions, ReLU,
BatchNorm folded at export, output = input − net(input). The exported graph is only `Conv`,
`Relu`, `Sub`, opset 17, 24 nodes. Full: 12 × 64, 373,443 parameters, 30.48 dB; tiny: 6 × 24,
22,251 parameters, 29.79 dB **[SOURCE `unditherer/models/models.json`]**. **Only the full
model is in scope** (decided before the engine question was settled).

**Engines measured, full model, this machine (Ryzen + RTX 4070)** **[MEASURED 2026-09-04]**:

| Engine | 32×32 | 64×64 | Notes |
|---|---|---|---|
| onnxruntime 1.29, Linux x64, 1 thread | 5.1 ms | 20 ms | the lab's own path |
| **onnxruntime 1.20.1, Windows x86 DLL, under Wine 9.0, 1 thread / all** | **10.5 / 4.1 ms** | **42 / 11 ms** | 32-bit test exe, fresh prefix, Wine's built-in `msvcp140`/`vcruntime140`, nothing installed |
| naive C loops (libonnx class), 1 thread | 286 ms | 1180 ms | `gcc -O2`, same shape; libonnx's `Conv` is plain loops, no SIMD, no threads |
| **onnxruntime 1.20.1 x86 + DirectML, under Wine 9.0 on vkd3d-proton, RTX 4070** | **0.087–0.094 ms** | **0.239–0.241 ms at 56×56** | per tile in batches of 64, as the module issues them — the measured GPU path, below |
| GLSL passes on the GPU (estimate) | ≪ 1 ms | ≪ 1 ms | ~750 k FLOPs per texel |

So at load, batched by frame size and threaded, Two Continents' tiles are roughly 7–10 s
through the x86 runtime, the install's unit textures about 2 s; with libonnx as it ships it
would be minutes; on the GPU about a second.

**Availability of Microsoft's runtime** **[MEASURED 2026-09-04, NuGet package contents]**:
the official C API is one exported function returning a table of function pointers and a
MinGW 32-bit build against the shipped header works (SAL macros stubbed). But TotalA.exe is a
32-bit process and the 32-bit Windows binary is gone:

| `Microsoft.ML.OnnxRuntime` | Windows native runtimes |
|---|---|
| 1.20.1, 1.22.1 (May 2025) | x64, arm64, **x86** |
| 1.23.2, 1.24.4, 1.26.0, 1.29.0 (current) | x64, arm64 |

The build docs say 32-bit builds are no longer supported. **1.22.1 is the last usable
runtime, frozen.** It runs the opset-17 model.

**The risk the spike must answer.** The overlay's header says a runtime `LoadLibrary` of a
companion DLL "destabilised TA under wine", which is why every pass is compiled into the
fork. The record of that event ([field notes](field-notes.html), G1 gotchas, 2026-08-31)
shows the crash was a **null GL function pointer**: `glGetIntegerv` resolved through
`wglGetProcAddress` is NULL under Wine, the companion called it, and TA's own crash reporter
then faulted at `0x4D94E0` while reporting it — the note's later bullets say so themselves.
The module load was never isolated as the cause. onnxruntime.dll is nevertheless a 10 MB C++
DLL with its own thread pool and a VC++ 2019 runtime dependency inside the game's process,
which the standalone test did not exercise.

**Decided: spike ONNX Runtime x86 inside TA first; the GLSL passes are the fallback.**
Rejected: libonnx (minutes per map, threading and SIMD would be ours), a 64-bit helper
process (exact and fast, but IPC, process lifecycle and a Windows x64 helper to ship).

**The spike ran, 2026-09-04 — it is the engine** (`tagpu_restore.c`, `4b2cbdf`). What it
does: the DLL loads `onnxruntime.dll` lazily from a worker thread it creates (never from
DllMain, never from the render thread mid-present), copies the tile set and the live palette
into the job, runs the full model in batches of 64 by input shape, writes
`gamedir/tagpu_cache/terr_<crc32>_<count>.rgba`, and the terrain pass uploads the result as a
second atlas in the same cell layout and samples it under `tagpu_classicpp.on`.
Measured in the running game under Wine 9 on Two Continents **[MEASURED]**:

| Step | Cost |
|---|---|
| `LoadLibrary` of the 10 MB runtime, inside TotalA.exe | 11–13 ms |
| `CreateEnv` + `CreateSession` on `full.onnx` | 23–25 ms |
| first restore of 5062 tiles, 4 intra-op threads (400 tiles wrap-padded at 56×56, 80 batches) | 22.6 s, off both game threads |
| the same map from the cache | 29–33 ms |
| Classic parity baselines after the shader change | unchanged, `6f7ad6b1` / `9c9ab215` |

**The provider is DirectML, and under Wine that means vkd3d-proton** **[MEASURED
2026-09-04]**. The CPU was never a decision, only what the spike reached for first; the
question the owner asked — why not the GPU — has three answers stacked on each other:

- The pinned `Microsoft.ML.OnnxRuntime` package is **CPU-only**, and the CUDA provider is
  **x64 only**, so it cannot load in a 32-bit process at all.
- `Microsoft.ML.OnnxRuntime.DirectML` **1.20.1 does ship `runtimes/win-x86/native`**, and
  that DLL is a superset of the CPU-only one: it serves the CPU provider at the same speed
  (3.16 ms per 32×32 tile on four threads, batch 64 — the same number the CPU-only build
  gives), so installing it costs nothing when the GPU side is unavailable. It loads under
  Wine 9's built-in `msvcp140` exactly as 1.20.1 does — same generation, no
  `std::_Throw_Cpp_error`.
- **Wine 9's built-in D3D12 cannot host DirectML.** `vkd3d` creates the device on the
  RTX 4070 (max feature level 11_1) and then refuses the provider two ways:
  `ID3D12Device5::EnumerateMetaCommands` is a stub, which is the actual failure —
  `OrtSessionOptionsAppendExecutionProvider_DML` returns `E_NOTIMPL` at
  `dml_provider_factory.cc(520)` — and behind it `CheckFeatureSupport` answers **shader
  model 0x51 to DirectML's 0x66 ask**, so its DXIL shaders would not compile either.
  **vkd3d-proton 3.0.1's 32-bit `d3d12.dll`/`d3d12core.dll` host it**, and are pinned by
  hash in `tools/fetch_onnxruntime.sh`. They must be *preferred over the built-in*:
  `WINEDLLOVERRIDES=d3d12,d3d12core=n,b`, which `tacli` now sets for every instance. Loading
  them by full path from our own thread first does **not** work as a substitute — Wine keys
  loaded modules by path, so DirectML's later `d3d12.dll` by name still finds the built-in.
  On real Windows none of this arises.

**What the GPU is worth, in the running game on Two Continents** **[MEASURED 2026-09-04]**:
the same 5062 tiles in 80 batches take **589–662 ms on DirectML against 19.2 s on four CPU
threads — 29–33×**; standalone the per-batch gap over two runs is **34–37×** at 32×32
(0.087–0.094 against 3.16–3.19 ms/tile) and **41–42×** at the wrap-padded 56×56 (0.239–0.241
against 9.95–10.08). The GPU's cost is a **one-time ~1.9 s session build** (against
21 ms on the CPU) on the worker thread — which made a *cached* map cost more to reach the
runtime than to read its atlas, so the job now **reads the cache before it loads any runtime**
and a restored map never builds a session at all. The pre-warm idea in §4 is worth more, not
less. **The two providers
agree**: restoring the map both ways and diffing the two cache files, **61 of 20,733,952
bytes differ and every one by exactly 1 level** — fp32 rounding, so DirectML is running the
same graph and not a half-precision one, and a cache written by either is valid for the
other (the key is the tile content, not the provider). `tagpu_restorecpu.on` forces the CPU
side for that A/B. A 15-minute 200v200 match ran with the D3D12 device resident beside our GL
context: alive, no GL or restore errors.

**Not 1.22.1 — 1.20.1.** The 1.21.0 and 1.22.1 x86 builds call `std::_Throw_Cpp_error`,
which Wine 9.0's built-in `msvcp140` does not implement: standalone in the instance prefix
they abort with "unimplemented function … aborting", and inside the game the worker thread
never returns from `CreateEnv` — no fault, no log line, because Wine's stub exception passes
the fork's filter and the thread simply stops **[MEASURED 2026-09-04]**. 1.20.1 is the last
x86 build that runs on the built-in runtime, so `tools/fetch_onnxruntime.sh` pins it by
hash; a prefix with the native VC++ 2019 runtime (winetricks) could take 1.22.1. On Windows
the redistributable is the requirement either way.

**The module-load rule is settled**: a second DLL loaded at runtime from our own thread sat in
the process through a 15-minute 200v200 match, alive throughout, no GL or restore errors
**[MEASURED 2026-09-04]**. The in-game result also matches the lab's Python-restored atlas of
the same tiles to 0.006 levels per texel on average — same model, same numbers. The
fork's `LoadLibrary` hook (`hook=4`) is bypassed with `real_LoadLibraryA` so `hook_init()`
does not re-scan the runtime's module tree — a precaution, not a measured fault.

**Rules that are ours in C whichever engine runs** **[SOURCE `unditherer/restore.py`,
`infer.py`, `classical.py`]**: colour-key texels are inpainted before the network (OpenCV
Telea, radius 3 — no C twin, a few dilation passes stand in) and alpha is restored from the
key mask after; a frame whose opposite edges agree within 12 levels (`is_tileable`) is
wrap-padded by the depth, every other frame is zero-padded (the convolutions' own `zeros`
padding); output is clipped to 8 bits. The restored RGBA atlases are new objects beside
Classic's indexed ones, which do not change: units 4-texel replicated pad, 4-aligned, mip
levels 0–2.

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
  drive the same state. The model `full.onnx` and `onnxruntime.dll` (1.22.1 x86) ship in
  gamedir beside them.

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
- **The definition tables are known.** Unit definitions: count `main+0x1438F`, table
  `main+0x1439B`, stride `0x249`; feature definitions: count `main+0x14253`, table
  `main+0x1426F`, stride `0x100` **[SOURCE `tagpu_cat.c`]**. The load-time atlas walk starts
  there; the path from a definition to its loaded model's faces is not yet written down (§4).
- **The GL context is requested at 3.2 core** (`render_ogl.c` 189–192) and the field notes
  record `GL_VERSION 3.2.0 core` on the 4070. The native shaders are `#version 330 core` and
  compile, which is the NVIDIA driver being lenient, not a guarantee; sampler objects are a
  3.3 feature. **Bump the request to 3.3** before relying on either **[SOURCE, field notes]**.
- **Fog cannot leak through shadows.** The unit gather drops units whose anchor tile is
  unexplored or unseen, and cloaked enemies, before anything is emitted; its rect has 256 px
  of slack on every side, more than the lab's 192-unit caster margin **[SOURCE
  `tagpu_native.c` gather]**. The depth pass draws the gathered set and nothing else.
- **The frame order needs no restructuring.** Terrain, features and units are gathered before
  any pass renders; a depth pre-pass over the frame's unit buffer slots in before the terrain
  render with no frame lag **[SOURCE `tagpu_native_frame`]**.
- **Team colour is per-owner GAF frames** (`frame[owner]` from the anim's inline table,
  `tagpu_render3do.c face_texframe`). Every variant is a frame in `textures/*.gaf` and is
  restored at load like any other; nothing to enumerate separately.
- **The composite over the top bar** and the wndproc interception are established, §2.10.

---

## 4. Open  [OPEN]

- **The restorer is terrain-only so far.** Feature sprites and unit textures go through the
  same job next (they have colour keys, so the inpaint stand-in of §2.5 lands with them), and
  the unit atlas needs its 4-texel pad, alignment and mips.
- **The first restore blocks nothing but is visible**: on the CPU 19–23 s during which
  Classic++ terrain draws indexed, then switches; **on DirectML 0.6 s, which is no longer
  worth hiding** on a map this size. A loading-screen hook or a tacli pre-warm still pays on
  the CPU fallback and on the largest maps (11561 tiles ≈ 1.4 s GPU, 44 s CPU).
- **The cache's format is undecided.** Today it is raw RGBA, 19.8 MB for Two Continents and
  4.4 GB if every stock map were played (275 maps, 1.12 M tiles, median 3218, largest 11561).
  Measured on the real cache **[MEASURED 2026-09-04]**, per Two Continents / all maps:
  RGB+gzip 11.2 MB / 2.5 GB (zlib is in the DLL); one PNG sheet 8.7 / 1.9; RGB+zstd-19
  8.5 / 1.9 (one BSD file to drop in); xz 7.8 / 1.7; lossless WebP 7.1 / 1.6; zlib per
  tile 11.9 / 2.6; BC7 4.9 / 1.1 and BC1 2.5 / 0.5 (GPU formats: lossy, upload as-is,
  4–8× less VRAM; a crude BC1 sits at ~35 dB against the restored art, above the
  restorer's own 30.5 dB). Rejected by measurement: the residual against the palette
  colour (7.9 MB — the residual *is* the dither noise), tile de-duplication (5054 of 5062
  already unique), RGB565 (banding back). Lossless tops out near 40 %; only block
  compression goes further. Also on the table: cap the cache to the newest N maps, or
  move it to the user's profile.
  **Maps share tiles** (the owner's point, measured 2026-09-04 over all 275 stock TNTs, every
  32×32 tile hashed): 1,118,476 tile slots hold **553,094 distinct tiles (49.5 %)**; inside
  one map the compiler already de-duplicates (Two Continents 5054 of 5062), the sharing is
  *between* maps. Loading the maps in name order, the median map adds 1,611 new tiles
  (mean 2,011) and 76 maps add under 10 % — several variants share a whole tile set. So
  the cache should be a **content-keyed tile bank shared by every map**, not a file per
  map: each load restores only the tiles the bank lacks (a median map ≈ 7 s at the
  measured 4.5 ms/tile, a variant ≈ 0), and the whole game's ceiling is the unique count:
  raw RGBA 2.2 GB, zstd 0.93 GB, BC7 0.54 GB, BC1 0.27 GB. The palette is one for all
  maps, so a content key is valid across them.
- **Definition → loaded model → texture frames**: the walk the load-time atlas build needs,
  to be established from the binary and written into
  [the engine map](exe-reverse-engineering.html).
- **Aircraft and boats** — the viewer prototype of §2.2/§2.3 has not been built.
- **Hires glb under Classic++** — lighting model, depth pass, shadow read-back, silhouette
  shadow off; deferred by §2.4.
- **The lab's Classic++ lane is zoom 1 only**, so the zoom-out look of §2.9 has no lab
  reference until the lane gains `uZoom`.
- **Nanoframe wireframe back edges** show through the unbuilt part — inherited from G13l,
  needs a stencil pass per nanoframe.
- **Windows users** need the VC++ 2019 redistributable for onnxruntime.dll; under Wine the
  built-in runtime sufficed.
- **The GPU path's shape on other machines is untested.** DirectML was measured on one
  adapter (RTX 4070, NVIDIA 595.84, vkd3d-proton 3.0.1) and picks device 0 unconditionally —
  a laptop whose device 0 is an integrated GPU would get that one, and no fallback compares
  the two. Nothing has run on AMD or Intel, on real Windows, or on Wine's built-in D3D12
  once it grows `EnumerateMetaCommands`. Every failure falls back to the CPU provider, so
  the risk is speed, not correctness.
- **The GLSL conv passes are now a fallback, not the plan.** DirectML reaches the GPU with no
  shader of ours, so the hand-written passes are only worth building if the vkd3d-proton
  dependency has to go.

---

## 5. Order of work

1. ~~**The restorer spike**~~ — done 2026-09-04: onnxruntime 1.20.1 x86 in the DLL, tiles
   restored at map load, cached, the terrain drawn from it under `tagpu_classicpp.on`.
2. **The other two restored atlases at load**: the definition → model → frames walk, the
   colour-key inpaint stand-in, pad and align, mips 0–2; the unit and feature shaders'
   restored-texture branch.
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
