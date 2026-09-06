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
| Textures | the engine's 8bpp GAF frames as palette indices, `GL_NEAREST`, the `PALETTE.SHD` shade LUT | **restored true colour for all three atlases** — terrain tiles, feature sprites and unit textures — through the `unditherer` full model. Units: 4-texel padded atlas, trilinear to mip level 2, 4× anisotropic. Tiles and sprites: 1:1, `NEAREST`. *In the game: the terrain (G14c) and the feature and effect sprites (G14e, lazily on first draw); unit textures not yet.* |
| Terrain | the engine's 32-px tile blit, no height, no light | the same tiles in restored colour, per-pixel lambert from the heightfield normal, **normalised so level ground is exactly 1.0** (the art is already lit) |
| Units | per-face shade row from `SH_L` through the 32-row LUT | per-pixel lambert in map space from the posed face normal, same level normalisation |
| Shadows | the engine's rules. **In the game**: the 5-px silhouette drop for mobiles and the cached slant for structures, each blended once per silhouette pixel (G13n). **In the lab**: the silhouette only — a structure casts NOTHING there, because that lane does not draw the slant projection. This row claimed the lab had both until 2026-09-04; it did not have either, and now has one | a depth map along `shadowsun`, PCSS-lite (8-tap blocker search, 16-tap Poisson PCF), receiver-plane bias, per-caster length `14 + 0.25·height`; hills cast and receive; an airborne caster follows `airshadow` (§2.2) |
| Suns | one, `SH_L = (−0.35, 0.80, −0.49)` in model space | **three** knobs: `sun=324.5,53.1` (terrain), `unitsun=215.5,53.1` (= `SH_L` in map space), `shadowsun=225,40` |
| Fog of war | the engine's per-index grey LUT | one RGB rule after lighting (§2.6) |
| Supersampling | 2× box, `tagpu_ss.off` | the same |

**The Classic baselines moved on 2026-09-04, deliberately**, when the lane gained the
silhouette drop shadow it had always claimed. `tascene-parity.json`: default
`md5 f42f300a69f843a669d64fc30deb08e2`, `ss=1` `md5 59482d4d2519801fbc41b7d33510b471`
**[MEASURED]**.

**The re-baseline is checkable, and that is the point of it.** `unitshadow=0` reproduces the
previous pair — `6f7ad6b122591d6db2a2b028938be5b3` and `9c9ab215099e581288b24cc47d41f9be` —
**byte for byte**, so the shadow is provably the only thing that moved; the change itself is
225 px at a median darkening of 0.515 of the bare ground. A lane whose claim is "it must not
move by a pixel" can only be re-baselined this way: with a switch that puts the old pixels back
and a diff that says what the new ones are.

---

## 2. Decisions for the in-game Classic++  [DECIDED 2026-09-04 unless noted]

### 2.1 The suns stay constant
The terrain sun is the engine's own, `324.5,53.1`, on every map. The lab's `artlight`
survey found the tile art painted from a different azimuth on 78 % of stock maps and from the
opposite side on 23 of them, so a constant sun will fight the painting somewhere. **Decided:
ship the constant and look.** The engine itself lights units and casts shadows from fixed
directions whatever the map, and the per-map option (`artlight`'s peak azimuth) stays on the
shelf until a map actually reads wrong.

### 2.2 Aircraft: prototyped, and what the viewer showed  [MEASURED 2026-09-04]
The lab has aircraft now — units carry an altitude, the FBI's own `CruiseAlt` (ARMBRAWL 60,
ARMATLAS 90, ARMFIG 110, ARMPEEP 180, ARMTHUND 200), fixture `scenarios/tascene-air.json`.

**The engine's own answer, measured first** ([shadows & cloak](shadows-cloak.html) §4b): the
shadow is the plane's silhouette, `+5 px` in x and `(altitude − ground)/2` **straight down** —
on the ground under the plane, separating downward as it climbs.

**The prediction held.** At `shadowsun`'s 40° a Thunder at CruiseAlt 200 throws its shadow
**211 px right and 91 px up** of its body, and two of the five airframes threw theirs clean off
the frame. The cue under the plane is lost exactly as this section said it would be.

`airshadow=<how>` now picks, and all three are one query apart in the lab:

| | what it does | how it reads |
|---|---|---|
| `len` *(default, provisional)* | `shadowlen`'s own rule extended to the ALTITUDE: the throw is `a + b·(agl + model height)` rather than `(agl + h)·cot el` — 67 world units for a Thunder at 200 instead of 250 | stays with the plane, keeps the altitude cue, and every shadow in the frame still comes from one light |
| `physical` | cast from the true altitude | detached; reads as a cloud shadow that happens to be nearby |
| `drop` | do not cast at all — draw the Classic silhouette instead, which is what the engine does | exact, but puts a hard-edged 1997 silhouette into a soft-shadow scene for one object class |

**Decided 2026-09-05: `len`.** The owner looked at it in the viewer and settled on what the
lane already defaults to — the throw stays with the plane, the altitude cue survives, and every
shadow in the frame still comes from one light. `physical` and `drop` stay in the viewer as
queries, not as options to ship. Boats (§2.3) still have no prototype, and the decision above
does not pre-empt them: a hull sits ON the water, so it is the ordinary ground case.

### 2.3 Water: the seabed stays shaded and shadowed
The viewer lights and shadows underwater cells by the seabed, and never reads the sea level
in its lighting or shadow receivers **[SOURCE `tools/tascene-view.html`]**. **Decided: keep
it** — it is part of the approved look. Boats go into the same viewer prototype as aircraft,
so the shadow of a hull on the seabed is seen before it is built.

**And the water does not move, so a restored atlas may be a still snapshot** **[MEASURED
2026-09-05]**. This was raised as a hazard: Classic++ samples an RGBA atlas baked once per map
against the palette as it stood when the job started, so anything the engine animated *through*
the palette would freeze — and `tagpu_terr.c`, `tagpu_native.c` and
[the engine map](exe-reverse-engineering.html) all said the engine cycles the palette for water.
**None of them had measured it, and it is false.** Stock engine, no passes armed, camera pinned
with `tacli eye` over open sea on **Anteer Strait** and over **Ring Atoll**'s lagoon (a
100 %-water viewport): **0 of 630 784 viewport pixels changed** over 10 s, and **0** again over
30 s at **+10 game speed**, while the minimap changed 32–88 px in the same frames — the liveness
control, without which 0 would only mean the capture had frozen. The palette read straight out
of the process (`tacli peek '*0x511DE8+0x143A7'`) was **identical in all 1024 bytes across 16
samples**. So there is nothing to lose: the snapshot is exact. The claim is corrected in the
six files that carried it (`tagpu_native.c` twice, `tagpu_terr.c`, terrain & depth, the lab
design page, the engine map, the roadmap's G13b row). **What this does not prove**: that the palette never changes
anywhere — a mission script or a menu transition was not tested — only that nothing cycles in
play. The per-frame re-upload stays, as insurance rather than as a mechanism.

### 2.4 Hires glb units are out of scope
`tagpu_hires_draw.c` lights in linear space with a GGX lobe; Classic++ is a lambert in sRGB
space, and hires meshes are in no depth pass. **Decided: not a Classic++ concern.** Revisit
after the port.

### 2.5 The restorer runs at map load, inside the DLL, through ONNX Runtime — spike first

*Superseded 2026-09-05 by §4c: the restorer is fragment passes in the game's own GL context, and
the ONNX Runtime path below was deleted the same day (landing 3 — `tagpu_restore.c`, the
`onnxruntime_c_api.h` header, `tools/fetch_onnxruntime.sh`, tacli's vkd3d-proton plumbing and the
runtime files in the game directory; it survives in git history at `fab2247`). The measurements
here stay as the baseline the GLSL numbers are judged against.*

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
  **vkd3d-proton's 32-bit `d3d12.dll`/`d3d12core.dll` host it.** They must be *preferred over
  the built-in* — `WINEDLLOVERRIDES=d3d12,d3d12core=n,b` — which `tacli` sets for an instance
  that actually has the pair. Loading them by full path from our own thread first does **not**
  work as a substitute: Wine keys loaded modules by path, so DirectML's later `d3d12.dll` by
  name still finds the built-in. On real Windows none of this arises.
  **Where the pair comes from** is `tacli`'s `vkd3d_proton_dir()`: **Steam's own Proton
  first** — every Proton ships a 32-bit vkd3d-proton at
  `files/lib/wine/vkd3d-proton/i386-windows`, so a machine with Proton installed needs no
  download at all — then the hash-pinned 3.0.1 copy `tools/fetch_onnxruntime.sh` puts in the
  template gamedir, with `TA_VKD3D_PROTON` overriding both. Steam's Proton auto-updates, so
  the pinned copy is the reproducible one and `TA_VKD3D_PROTON=<template gamedir>` holds a
  version still. Both work: Proton Experimental's build (wine 11.0 tree) builds the session
  in 1.0 s against pinned 3.0.1's 1.4–1.9 s, and restores at the same rate. `launch` and
  `scenario load` print which one they linked whenever it is not the pinned copy.

**What the GPU is worth, in the running game on Two Continents** **[MEASURED 2026-09-04, and
CORRECTED — read the note below before quoting an in-game figure]**: the same 5062 tiles in 80
batches (73 plain, 7 wrap-padded) take **1.83–1.84 s on DirectML against 20.8 s on four CPU
threads — 11×**, cold, one generation, measured after the restart bug below was fixed.
Standalone, where the model runs alone, the per-batch gap is far wider: **34–37×** at 32×32
(0.087–0.094 against 3.16–3.19 ms/tile) and **41–42×** at the wrap-padded 56×56 (0.239–0.241
against 9.95–10.08).

**Why the in-game gap is a third of the standalone one, and why an earlier number here said
29–33×.** Those standalone rates predict **0.55 s** for this exact batch mix on DirectML and
**19.4 s** on the CPU. The CPU restore lands within 7 % of its prediction (20.8 s); the
DirectML one takes 3.3× its own (1.83 s against 0.55 s). The difference is *when* the batches
run. Until this landing, `tagpu_terr.c` threw the restore away on the GL reset the game does at
startup and began a second one — so the figure recorded here, 589–662 ms, was that **second**
generation, running after the map was live and the engine was idle. With one generation the
work overlaps map load, where the engine saturates the CPU, and DirectML's per-batch submission
is CPU-side work. **That last sentence is [INFERRED]** — the timing is measured, the cause is
not isolated. The honest summary: the GPU is worth ~11× on a cold map load today and ~36× on
the model alone, and closing that gap has not been attempted. The GPU's cost is a **one-time 1.0–1.9 s session build**, depending on the vkd3d-proton build (against
21 ms on the CPU) on the worker thread — which made a *cached* map cost more to reach the
runtime than to read its atlas, so the job now **reads the cache before it loads any runtime**
and a restored map never builds a session at all. The pre-warm idea in §4 is worth more, not
less. **The two providers
agree**: restoring the map both ways and diffing the two cache files, **61 of 20,733,952
bytes differ and every one by exactly 1 level** — fp32 rounding, so DirectML is running the
same graph and not a half-precision one, and a cache written by either is valid for the
other (the key is the tile content, not the provider). `tagpu_restorecpu.on` forces the CPU
side for that A/B. A 15-minute 200v200 match ran with the D3D12 device resident beside our GL
context: alive, no GL or restore errors, 13,514 log lines and none of them an error.

**What the GPU path costs** **[MEASURED 2026-09-04]**: **+160 MiB of VRAM** — the game holds
160 MiB as a pure graphics process and 320 MiB with the D3D12 device and DirectML session
alive (`nvidia-smi` also reclassifies it `G` → `C+G`). The session is never released, so that
is held for the whole run, not just the restore; releasing it after the last atlas would give
it back at the price of a 1.0–1.9 s rebuild on the next map. Irrelevant on a 12 GB card,
worth a thought on a small one. **A cached map pays none of it**: since the job reads the
cache first, `onnxruntime.dll` is not loaded at all and the process stays at the 160 MiB
graphics figure.

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
Telea, radius 3 — no shader twin; the FILL pass's nearest-ring mean stands in, §4c) and alpha is restored from the
key mask after; a frame whose opposite edges agree within 12 levels (`is_tileable`) is
wrap-padded by the depth, every other frame is zero-padded (the convolutions' own `zeros`
padding); output is clipped to 8 bits. The restored RGBA atlases are new objects beside
Classic's indexed ones, which do not change: units 4-texel replicated pad, 4-aligned, mip
levels 0–2.

### 2.5b No cache for any image map — everything is restored in the running game  [DECIDED 2026-09-05]

**The disk cache goes, for all three atlases.** `gamedir/tagpu_cache/` and
`cache_read`/`cache_write` went with `tagpu_restore.c` (landing 3 of §4c, 2026-09-05; the GLSL
restorer never had a cache); nothing restored is ever written to
disk, and every session restores what it draws. §4's whole "the cache's format is undecided"
bullet — the compression survey, the 4.4 GB ceiling, the content-keyed tile bank — is **closed
by this decision, not by an answer**.

Two consequences to hold on to, both measured:

- **Every map load now builds an inference session**, because the old fast path ("read the
  cache before loading any runtime") no longer exists: `LoadLibrary` 11–13 ms, then **1.0–1.9 s
  on DirectML** (23–25 ms on the CPU provider) on the worker thread, once per process.
- **The +160 MiB of VRAM is now permanent**, not conditional. A cached map used to pay none of
  it; there are no cached maps any more.

**Terrain needs no discovery; the GAF frames do.** `tagpu_terr.c` already holds the whole tile
set (`s_setPix`, `s_setCount`) — that is what it hands to `tagpu_restore_terrain_begin` today
**[SOURCE]**. The unit and feature textures are the opposite: they are inside `tactics*.hpi`
and the `.ufo` archives, not loose files **[MEASURED — the gamedir has no `textures/` tree]**,
so the DLL can only reach frames the engine has already decompressed into memory, and something
has to name them. The routes are enumerated in §4b.

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

## 4b. How the restored textures get into the game, with no cache  [OPTIONS, 2026-09-05]

*Written to be decided from, not re-derived. Rates below are the ones measured **inside the
running game** (§2.5), not the standalone ones: **0.36 ms per 32×32 tile on DirectML** and
**4.1 ms on four CPU threads** — 0.35 and 4.0 µs per texel. Workloads: Two Continents 5062
tiles = 5.2 M texels, the biggest stock map 11561 tiles; the whole install's `textures/*.gaf`
753 frames = 1.35 M texels, median entry 32×64 (2048 texels ⇒ **0.7 ms GPU / 8 ms CPU per
frame**). Feature frames are small beside both.*

### Option 1 — Lazy: restore each frame the first time it is drawn

Both lazy atlases already exist and are the natural hook: `atlas_get` in
`tagpu_render3do.c` (units, 1024², 256 entries) and `tagpu_gaf_atlas_get` in `tagpu_gaf.c`
(shared by `tagpu_feat.c` and `tagpu_fx.c`) **[SOURCE]**. On a miss each decodes one frame and
uploads it into a shelf slot; the change is to copy that decoded frame and its shelf rect onto a
queue, let the worker restore queued frames in shape-grouped batches, and have the render thread
blit each result into an RGBA twin atlas at the same rect, flipping a per-entry `restored` flag
the shader reads — the same two-atlas shape `tagpu_terr.c` already has.

- **Discovery needed: none.** This is the option that does not touch the engine's object graph.
- **Build**: the queue, the twin atlas per lazy atlas, the shader branch, and **W×H batching** —
  `run_batch`/`fill_tile`/`unpack_tile` are square-only (`H`) today because tiles are 32×32, and
  GAF frames are not square **[SOURCE `tagpu_restore.c`]**. That last piece is needed by every
  option that restores GAF frames, so it is not a differentiator.
- **Cost**: ~0.7 ms GPU for a median frame; a unit type appearing for the first time is 1–3
  frames ≈ **2 ms of worker time**. Everything the session ever sees ≤ **0.47 s GPU / 5.4 s CPU**,
  spread across play instead of spent at load.
- **Visible behaviour**: a frame draws indexed for the frame or two before its restore lands.
  The bad case is a mass reveal — 200 units of unseen types at once queues ~100 frames ≈ 70 ms
  GPU (invisible, off-thread) or ~0.8 s CPU (a fade-in, not a stall).
- **Risks**: mixed-mode atlases are the normal state, not an edge case; a GL reset has to re-queue
  everything; the unit atlas's 256-entry cap and its 1-texel border both need revisiting for
  §1's 4-texel pad and mips 0–2 anyway.

### Option 2 — Enumerate at load, then restore in one batch

The original §5 step 2, minus the cache. Same code as Option 1 without the laziness, plus a way
to name every frame up front. Three routes, none yet probed:

| | what it is | what it costs to find | what it covers |
|---|---|---|---|
| **2a** definition → loaded model → faces → frames | the walk §4 has always named: unit defs at `main+0x1439B` stride `0x249`, feature defs at `main+0x1426F` stride `0x100` **[SOURCE `tagpu_cat.c`]**, then each definition's model root, its node tree, its face records, and the frame pointers a face reaches at `+0x10`/`+0x18` **[SOURCE `tagpu_render3do.c face_texframe`]** | the most disassembly: the def→model field and the loaded-3DO tree layout are both unwritten | units and features; **not** effects GAFs |
| **2b** detour the GAF loader and record what it loads | we already have the detour machinery, and `0x429700` opens the `cursors` GAF and parks its handle at `main+0x14903` **[SOURCE engine map]**, so a loader of this shape is reachable | one function to identify and one detour to write | **everything the game loads**, mod `.ufo` content included |
| **2c** find the engine's own table of loaded texture anims | a face reaches frames through a pointer at `+0x18` with an inline frame table at `+0x28`, which the loader resolved from a name — so a registry probably exists | cheapest if it exists, unbounded if it does not | everything, if it exists |

- **Cost**: 0.47 s GPU / 5.4 s CPU at load for **all** textures, whether or not they are ever
  drawn — against Option 1's "only what you see, when you see it".
- **Benefit**: no mixed-mode branch and no first-draw latency at all.

### Option 3 — Drop ONNX: run the model as GLSL passes in our own context

The standing fallback of §4, promoted to a real option by the no-cache decision, because with no
cache the runtime is now loaded on **every** session. 12 layers × 64 channels as fragment passes
over ping-pong FBOs, weights in a texture; 64 channels is 16 RGBA targets, so with 8 MRT that is
2 passes per layer, ~24 passes total.

- **What it deletes**: `onnxruntime.dll` (10 MB), the vkd3d-proton dependency, DirectML, the
  +160 MiB, the 1.0–1.9 s session build, the worker thread and its copies, the 20.8 s CPU
  fallback, *and* §4's "untested on other adapters" risk — one code path on every machine.
- **Cost**: ~750 kFLOP per texel (§2.5) ⇒ ~3.9 TFLOP for a whole map's tiles, sub-second on a
  4070 in theory. **Unmeasured**, and fragment-shader efficiency on 3×3×64×64 convolutions is
  exactly the unknown. It also runs on the render thread, so it must be sliced across frames or
  it stalls the game.
- **Caveat**: compute shaders are GL 4.3; the context is requested at 3.2 core (§3), so this is
  fragment passes unless the context is bumped — a separate decision with its own risk.
- **Being prototyped** — the mechanism and the plan are §4c, decided 2026-09-05.

### Option 4 — Hybrid: the terrain job stays whole-set, the GAF frames go lazy  [BUILT 2026-09-05, G14e]

Terrain is already enumerated and batches efficiently; GAF frames are the ones that would need
archaeology, and are the ones laziness suits. So: keep `tagpu_restore_terrain_begin` exactly as
it is minus the cache, and give the two lazy atlases Option 1. **This is what shipped**, on the
GLSL engine rather than ONNX: `tagpu_gaf.c` feeds a queue on every atlas miss, and the feature
and effects atlases each carry an RGBA8 twin the sprite shaders sample where its alpha is 1 —
the mechanism and the numbers are in §4c below.

- **Discovery needed: none.** No route from §2 is needed at all.
- **Map load**: session 1.0–1.9 s + restore 1.83 s ≈ **2.9–3.7 s of indexed terrain, then a
  pop**, on DirectML; ~21 s on the CPU fallback; ~4.2 s / 47 s on the biggest stock map.
- **Everything else** costs what Option 1 costs, i.e. milliseconds nobody sees.

### Option 5 — Ship the stock textures pre-restored  [conflicts with the decision; listed for completeness]

The GAF half is small: 1.35 M texels is **4 MB raw, ~2 MB compressed**, so the stock unit and
feature textures *could* ship as data and skip the runtime entirely, with anything unknown (mod
`.ufo` content) restored on the fly. The terrain half cannot — all stock maps' tiles are 2.2 GB
raw, 0.93 GB zstd (§4). Recorded because the asymmetry is worth knowing; it is not what
"no cache, we load on the fly" asks for.

### Three levers, orthogonal to the choice above

- **The tiny model.** 6 × 24 against 12 × 64 is **7 % of the FLOPs — ~14× cheaper** — for
  **29.79 dB against 30.48** **[SOURCE `unditherer/models/models.json`]**. §2.5 put only the
  full model in scope, and it decided that when a cache meant the cost was paid once ever. With
  no cache it is paid every session, which makes this the single biggest cost lever in the
  system: it would put a Two Continents terrain restore at ~0.13 s on the GPU and ~1.5 s on the
  CPU fallback. Worth an A/B by eye before it is dismissed.
- **Visibility-ordered batches.** Restore the tiles under the camera first so the pop starts
  where the player is looking and the rest fills in behind. Matters most on the CPU fallback and
  the biggest maps.
- **fp16 on DirectML.** Unmeasured; plausibly ~2×. The two providers already agree to 1 level in
  61 of 20.7 M bytes at fp32, so there is headroom to check.

---

## 4c. The GLSL restorer — how it works, and the prototype plan  [DECIDED 2026-09-05]

*The decisions below were taken one branch at a time on 2026-09-05; each is marked with what
it fixes. The mechanism comes first because the decisions refer to it.*

### The mechanism

**The model in shader terms** **[SOURCE `unditherer/model.py`, `models.json`]**: 3×3 conv
3→64 + ReLU; ten × (3×3 conv 64→64 + BatchNorm + ReLU); 3×3 conv 64→3; `out = in − net(in)`,
RGB in [0,1]. The ONNX export already folded the BatchNorms (the graph is only `Conv`/`Relu`/
`Sub`), so it is **12 convolutions and one subtraction**; 372,096 MACs per texel (the ~750 kFLOP
of §2.5); 1.5 MB of fp32 weights. The tiny model is the same shape at 6 × 24: 22,032 MACs,
**17× less**, and its docstring says it was sized "small enough to consider porting to fragment
shaders".

**One conv layer = one fragment pass**: a full-screen quad into an FBO; each fragment computes its
own output texel from 9 taps of the previous layer × all input channels × weights, plus bias,
`max(0, ·)`. Activations live in float textures and ping-pong between two of them. 64 channels
per texel against textures of 4 is the awkward part, and there are two layouts, **measured, not
decided**: *channel-tiled* — one `RGBA32F` texture per layer holding a 4×4 grid of copies of the
image, tile *k* = channels 4k..4k+3, one draw per layer, 144 `texelFetch` and 2,304 MACs per
fragment, needing nothing beyond GL 3.0 — or *MRT*, 8 targets per draw, 2 draws per layer, fewer
redundant fetches. Start channel-tiled; switch if fetch-bound. Weights go in a texture-buffer
object (GL 3.1): a layer's 147 KB exceeds any UBO guarantee, and every fragment in a channel tile
reads the same 2,304 of them, so a TBO caches perfectly.

**The padding rule is what defines the pixels.** What the DLL feeds ONNX today **[SOURCE
`tagpu_restore.c` `is_tileable`/`fill_tile`/`unpack_tile`]**: a tile whose opposite edges agree
within 12 levels is wrap-padded by 12 to 56×56, convolved with zero padding at the 56 border and
centre-cropped; every other tile runs at 32×32 with zero padding *at every layer*. In GLSL both
are one rule — **every cell has a valid rect, and a tap outside it reads 0, at every layer** —
which is a per-fragment rect test. The trap: zero-padding the *input* to 56 and running unmasked
is not the same thing, because layer 2 would read layer 1's gutter, which is `relu(bias)`, not
zero. The last pass computes `in − net`, rounds to 8 bits and renders **straight into the
restored atlas** in its 34-px-pitch cell layout, border texels included, so `upload_rgb` and its
CPU copy go.

**Cost — MEASURED 2026-09-05, in the browser lab on the RTX 4070 (headless Chrome, ANGLE over
Vulkan, `tools/tascene restore`), Two Continents' 5,062 tiles = 4,662 cells at 32² + 400 wrap-padded
at 56², GPU time from `EXT_disjoint_timer_query_webgl2`, all against the pack's strict-fp32 reference:**

| model | precision | NK | draws | GPU time | Q2 diff (interior RGB bytes) |
|---|---|---|---|---|---|
| full 12×64 | fp32 | 4 | 3,760 | **1.15 s** (S32 0.90 s, S56 0.24 s) | **max 1 level, 179 of 15,550,464 = 0.0012 %, guard ring 0 — PASS** |
| full | fp32 | 2 | 7,280 | 1.08 s | same 179 bytes |
| full | fp32 | 1 (= channel-tiled) | 14,320 | 1.85 s | same 179 bytes |
| full | fp16 | 4 | 3,760 | 1.12 s | max 1 level on 722,948 = **4.65 %** — no faster, rejected (Q4) |
| tiny 6×24 | fp32 | 8 | 640 | 0.13 s | not diffed (a different model) |
| tiny | fp32 | 1 | 2,640 | 0.10 s | — |

Three runs of the headline row agree to 1 % (1,141–1,148 ms). The estimate this replaced said
0.5–2 s; 4.5 TFLOP in 1.15 s is ~4 TFLOPS sustained, 14 % of the card's fp32 peak. The **layout
question is settled by the NK column**: one output tile per draw (the channel-tiled cost) is
1.6× slower than two or four, and four is no better than two, so the pass is not fetch-bound past
NK=2 — it is arithmetic. fp16 storage buys nothing because the activation fetches were never the
limit, and it costs 4.65 % of bytes — so fp32 is what ships, and there is no precision decision
left to take. The tiny model is 9–11× cheaper than full here, not the 17× of the MAC count.
Transient memory: two 448² × 16-layer RGBA32F arrays = 103 MB during the restore, freed after.
The win stays what it was: deleting onnxruntime, vkd3d-proton, DirectML, the always-paid 1.0–1.9 s
session build and the permanent +160 MiB.

**In the running game — MEASURED 2026-09-05** (RTX 4070, Wine 9, 1024×768, the parity scenario
loaded with `tacli scenario load`, `tagpu_restoreglsl.on=log`), sliced from `restore_step()` at
**12 ms of GPU time per frame**, visible tiles first:

| map | tiles (wrapped) | batches / draws | frames | wall | fps held | GPU time |
|---|---|---|---|---|---|---|
| Two Continents | 5,062 (400) | 80 / 3,760 | 128 | **2.14 s** | 59.7 | 1.49 s |
| Two Continents, 8 ms budget | 5,062 (400) | 80 / 3,760 | 191 | 3.19 s | 59.8 | 1.53 s |
| Lava & Two Hills (the biggest stock map) | 11,561 (467) | 182 / 8,554 | 251 | **4.21 s** | 59.7 | 3.0 s |
| Two Continents, tiny 6×24 | 5,062 (400) | 80 / 640 | 16 | **0.25 s** | 63.8 | 0.14 s |
| Lava & Two Hills, tiny | 11,561 (467) | 182 / 1,456 | 30 | 0.48 s | 62.0 | 0.31 s |

The wall time is frame-bound: a 12 ms slice of a 16.7 ms vsync frame, so the budget sets the
elapsed time and the fps column says the game paid nothing for it. **Q1 is met** — the full model,
Two Continents, under 3 s, with the game rendering. In-game GPU time is 1.3× the browser's for
the same draws (1.49 against 1.15 s); not explained, plausibly the slices' queries bracketing
state changes the browser issues once **[INFERRED]**. **Q8, the in-game proof**: the atlas dumped
under `tagpu_restoredump.on` against the pack's fp32 reference is **max 1 level on 179 of
15,550,464 interior bytes (0.0012 %)**, the guard ring a copy of the edge in every cell — the same
179 bytes the browser bench differs on, so the DLL and the lab agree byte for byte. The
biggest map's 4.2 s is past the ~2 s trigger Q6 set for the progressive reveal, which is
therefore the next thing this engine owes. Landing 3 followed the same day: the ONNX path, its
header, the fetch script, tacli's vkd3d-proton and DirectML plumbing, `tagpu_restoreonnx.on` and
`tagpu_restorecpu.on` are gone, and `tagpu_classicpp_on()` lives in `tagpu_restoreglsl.c`.

### The reveal, the colour key and the queues  [MEASURED 2026-09-05, G14e]

**The progressive reveal (Q6, done).** The restored atlas's *alpha* is the per-cell flag: the
restorer clears its destination to 0 when a job starts and the OUT pass writes alpha 1 over every
texel it paints, guard ring included, so the terrain shader samples the restored colour where
`a > 0.5` and draws indexed elsewhere — no flag texture, no upload, and a cell's samples are
all-or-nothing because one quad paints its interior and its ring **[SOURCE `tagpu_terr.c`,
`tagpu_restore_glsl.h` OUT]**. Draws in one frame are in order, so a cell whose OUT pass a slice
issued is restored in that frame's terrain draw. The tiles are ranked by Chebyshev distance from
the **centre** of the gathered rect (ranking the whole rect 0 restored the visible cells in
tile-index order, a scatter), so the reveal radiates from the middle of the screen and carries on
outward. Measured in the game: Two Continents at map load, the first 64-tile batch is issued in
slice 4 (~80 ms after the job began); Lava & Two Hills with the switch flipped mid-play (so the
0.5 s poll is in the figures), the viewport is 68 % restored 0.94 s after arming, 98 % at 1.25 s
and complete at 1.9 s, while the whole 11,561-tile set takes 4.6 s at 58.6 fps — the per-cell map
of a shot at 0.94 s is a block growing from the screen centre. `restorediff` on the finished atlas
is unchanged: the same 179 bytes.

**Keyed frames (Q9, the GAF half).** The reference inpaints keyed texels with OpenCV's TELEA
before the network and restores the key's alpha after (`unditherer/restore.py`); a shader cannot
reproduce TELEA, so the FILL pass stands in with **the mean palette colour of the opaque texels on
the nearest ring (Chebyshev) within the model's depth** of the keyed texel — depth, because a
keyed texel farther than the receptive radius from every opaque one influences no opaque output —
and the OUT pass writes `(0, 0, 0, 0)` at the key, which is what the reference's save writes
**[SOURCE `tagpu_restore_glsl.h`]**. A frame with the key on an edge is never wrap-padded (the
reference decides tileability on the inpainted image, a coin toss a shader cannot call), a key in
the interior only leaves the 12-level test as it was. **The bar for keyed frames [DECIDED
2026-09-05]**: the Q2 bar applies to opaque texels *farther than the depth from any keyed texel*
(the far band); the near band differs from TELEA by construction and is reported and judged by
eye — the owner looked at the four worst frames beside the reference (the strip below) and
accepted column 2. Lab, the pack's 51 feature frames (all keyed, all non-square, 18 to 71 px), full
model, ANGLE/Vulkan on the 4070: **188 draws in 4 batches, 20 ms of GPU; far band max 0 on 1,287
bytes; near band 93,408 bytes, 27 % differing, mean 0.41 levels, 92 % within 1, 99.3 % within 4,
about 100 bytes over 8, one at 23** — ours and the reference are indistinguishable by eye, the
difference an outline at the silhouette (the four worst frames, indexed / ours / reference /
difference ×8: [restore-features-nearband.png](assets/shots/restore-features-nearband.png)). In the game the feature twin dumped under
`tagpu_restoredump.on` and matched frame by frame to the pack (`tascene featdiff`): 24 of 24
found, far band max 0, near band mean 0.412 — the same profile.

**Mixed sizes in one batch (the packer of Q9)**: a frame's padded edge S is `max(w, h) + 2·depth`
if it wraps; a batch takes frames of one *size class* (32, 48, 64, 96, 128, 192, 256, 384, 512)
in queue order, up to a square slot grid of `min(8, 512 / class)` per side — then on the smallest
square grid that holds what was taken, since the passes cost by the fragment and a queue's
two-frame batch must not pay for sixty-four — and its slot pitch is its largest S: a 30×25 tree
shares a batch with a 63×60 rock, a 512-px frame restores alone, and the lab's four feature batches
run on 5×5, 4×4, 4×4 and 3×3 grids.
Activations are sized to the largest `cols × S` seen, never past 512² (128 MB at fp32), and freed
after 3 s without work. The lab and the DLL run the identical batcher (`tascene-restore.js`
`batchFrames`, `tagpu_restoreglsl.c` `form_batch`); the terrain's two classes come out as before.

**The queues.** `tagpu_restoreglsl.c` is a pool of jobs sharing one set of GL objects and one
per-frame budget, stepped once per frame by `tagpu_native.c` *after the gathers* (so a frame missed
this frame is queued) and *before the renders* (so what it paints is sampled this frame). The job
with a batch in flight keeps it; otherwise the lowest priority runs — terrain 0, features 1,
effects 2 — re-picked at every batch boundary. Each `TAGPU_GAFATLAS` carries its twin and its
job; `tagpu_gaf_atlas_get` queues every new entry (tileability decided at upload, while the
pixels are still on the CPU), a recycle clears the twin and drops the queue, a context loss forgets
everything, and arming the switch mid-play queues what the atlas already holds. Measured, Two
Continents at map load with both queues live: the terrain's 5,062 tiles in **141 frames = 2.37 s
at 59.4 fps, 1.56 s of GPU** (a first feature batch of a few frames on a 2×2 grid was in flight
when the terrain job began; 13 of the 141 frames were not the terrain's); the 24 feature frames in
5 batches and the 10 effect frames in 1 drained **two slices after the terrain**, 143 frames from
the first queued. The effects twin has no reference
in the pack (its frames are the units' build and weapon sprites) and is judged by eye only.
By eye, `feat-forest` and `fx-mix` at zoom 1 and 0.25: trees, rocks, dead trees, fire, smoke and
explosions restored, no hairlines at the sprite edges (the twin's border is the edge's copy, as the
R8's is), the units still indexed. `fx-mix` is also the stress case: the fight burns the forest,
and every burning tree cycles fire frames and then becomes a wreck, each a new body and shadow —
the feature atlas grew from 13 to 1,298 entries in a minute, every one restored two or three
frames after its first draw, with no atlas recycle. The terrain restore ran at 54 fps in both
scenarios against 59 on the parity fixture; whether that is the sprite volume or the restore is
not isolated.

**What the bench changed on the way** (each a fact, not a decision):

- **The pack's reference was TF32.** `unditherer`'s torch backend ran on CUDA with cuDNN's
  default TF32 convolutions; against that reference the first fp32 GLSL run differed on **0.63 %
  of bytes** (mean 0.0063 levels) — the very figure §2.5 recorded for DirectML against the same
  pack, which means the ONNX path was measured against a TF32 reference too. `infer.py` now
  pins `allow_tf32 = False`, `tascene`'s undither cache magic went `TSU1 → TSU2` to drop the old
  entries, and the strict reference is what the table above is against.
- **Activations are array layers, not a channel-tiled sheet.** One `GL_TEXTURE_2D_ARRAY` layer
  per four channels lets a conv draw write NK layers through NK colour attachments, which is
  the MRT variant of the mechanism above with no k arithmetic in the shader; NK=1 *is* the
  channel-tiled cost. NK is chosen per device from `MAX_UNIFORM_BLOCK_SIZE` (one k-block of the
  full model is 9,472 bytes; the 16 KB WebGL2 minimum allows 1, NVIDIA's 64 KB allows 4).
- **Weights are a std140 uniform block, not a texture buffer.** The plan said a TBO; WebGL2 has
  none, and a texture of weights costs 4 fetches per activation fetch. A block of `mat4` bound
  per (layer, k-group) with `glBindBufferRange` is a broadcast read, reads the same on both
  sides, and keeps the shader body byte-identical — `unditherer/weights.py` writes the blocks
  padded to 256 bytes (the largest offset alignment any driver reports) with the layout in its
  docstring, and `run_reference` runs the model *from the packed blocks* in numpy against
  onnxruntime (2.7e-7 max) before any shader sees them.
- **The rounding writes `(k + 0.25) / 255`**, so a driver that truncates the float-to-unorm
  conversion and one that rounds both store exactly `k` — the GL 3.3 spec only *prefers*
  rounding.
- **Headless Chrome's clocks stand still** under `--virtual-time-budget`, and its `gl.finish()`
  returns before ANGLE/Vulkan is done (5,062 tiles "in 20 ms"): the bench takes its wall time
  from the server (`tascene` serves `__now`) across a one-texel `readPixels`, and its GPU time
  from timer queries collected after the run, because every task boundary costs virtual time.

### The decisions

| | Decided | What it fixes |
|---|---|---|
| **Q1 — what the prototype proves** | Correctness **and** an in-game bound: the full model restores Two Continents in **≤ 3 s** wall-clock, sliced, with the game rendering; the tiny model is timed in the same run | Two halves, in order: the browser proves the shader, the DLL proves it replaces |
| **Q2 — the correctness bar** | The provider bar: **max 1 level, on < 0.01 % of bytes**, over **all 5,062 tiles** of Two Continents (both padding classes), against the pack's `terrain/atlas.rgba.bin` — the same reference the DirectML output was matched to at 0.006 levels; the two ONNX providers themselves differ in 61 of 20.7 M bytes, every one by 1 | Any 2-level texel is a bug — that is how a wrong tap or a wrong mask first shows |
| **Q3 — where the bench lives** | A separate page, `tools/tascene-restore.html`, served from the pack: loads `atlas.r8.bin` + `pal.bin` + the weights, runs the passes, diffs against `atlas.rgba.bin`, reports the Q2 numbers and a per-batch wall time; headless through `shot`'s virtual-time machinery. The shader text is written **once**, in the lab's portable style (body shared, `#version` prefix swapped). **When it passes, the restore folds into the viewer as `restore=glsl`, the diff readout survives as a debug overlay or verb, and the bench page is deleted** | The viewer's parity lane is never touched by the bench; one page at the end; the shader that passes is byte-for-byte the shader that ships |
| **Q4 — precision** | fp32 (`RGBA32F`) is what must pass; fp16 (`RGBA16F`, one enum) is reported alongside from the same bench and adopted only if it is what gets the full model under 3 s in the game — never for tiny. Fail loudly without `EXT_color_buffer_float` | The optimisation gets its own decision after its number exists |
| **Q5 — sharing the render thread** | **Slice**: `restore_step()` in `tagpu_terr.c` issues batches with a budget in **milliseconds (~8 ms/frame)**, not cells — `GL_TIMESTAMP` queries (3.3) or a conservative wall clock. **No second GL context** (an unmeasured Wine risk of the second-DLL class) and **no CPU path** — the GPU is the only engine; a slow one restores slower, without stalling | A load-time event of a few seconds at 50 fps, on one thread where GL errors are attributable |
| **Q6 — what the player sees** | ~~**One flip** when the whole set is done~~ — **the trigger fired** (4.2 s on the biggest map) and the reveal is **progressive since 2026-09-05 (G14e)**: cells show as they land, centre-out, the flag being the restored atlas's own alpha (above). The one-flip behaviour survives as the alpha being 1 everywhere at completion | No random scatter: the order radiates from the screen centre; the biggest map's viewport is complete in 1.9 s of a 4.6 s restore |
| **Q7 — the ONNX path** | **Done 2026-09-05.** If GLSL passes, **delete it** as its own landing: `tagpu_restore.c`'s runtime half, `fetch_onnxruntime.sh`, `tacli`'s `vkd3d_proton_dir()` and `WINEDLLOVERRIDES`, `tagpu_restorecpu.on`, `onnxruntime.dll`/`full.onnx`/the d3d12 pair in gamedirs. §2.5's measurements stay as the superseded baseline. The reference oracle survives in the lab (`unditherer` on onnxruntime, `tascene build --undither`) | One engine; a fallback slower than the primary is dead code with a bill |
| **Q8 — in-game proof** | A **dump trigger**, `tagpu_restoredump.on`: the finished atlas written once as raw RGBA, diffed by a `tascene` verb against the pack with the Q2 bar — same tiles, same 2176 × 34-pitch layout, same order. It is the restorer's **only disk write, and only under the trigger**, so "no cache" stays literally true. `tascene ab` remains the whole-frame ritual, not the restore's proof | Checks the bytes the game samples, in the context that matters, with a pass/fail number |
| **Q9 — scope** | **Terrain only** for the prototype, with the per-layer mask taking a per-cell **rect** (x, y, w, h) so a GAF frame is a driver change and not a shader change. **Done 2026-09-05 (G14e)**: the driver change (size classes, the slot grid) and the colour-key stand-in landed with the lazy GAF work, proven on the pack's 51 non-square keyed feature frames rather than one debug cell | The go/no-go with the largest N and the only matched reference |
| **Q10 — order of work** | GLSL **before** the lazy GAF atlases (the only step built *on* the engine); unit shading, terrain lighting and shadows proceed **alongside** in their own worktree — they sample an atlas and do not care what filled it | The engine question idles nothing but the one step that depends on it |
| **Q11 — landings** | **Three.** (1) *Lab*: bench, weights verb, shader text, this section's numbers measured — lands whether or not the DLL half passes; no review. (2) *Engine*: the sliced restorer behind `restore_step()`, the dump trigger, in-game time and diff; ONNX stays compiled and reachable only through `tagpu_restoreonnx.on` for the same-map A/B; Opus review at medium. (3) *Deletion*, per Q7; review at medium. If full misses the bound, **tiny is judged by eye in the lab before landing 2 is written** | A negative result has somewhere to land; the engine review reads shader work, not `tacli` plumbing |

**Placements settled from the code, not asked**: the weights export is an `unditherer export-weights`
subcommand (that package owns `full.pt`/`full.onnx` and already has a `models` verb), writing the
flat fp32 `.bin` in the shader's channel order; `tascene build --undither` copies it into the
pack, and the DLL reads the identical file from the gamedir. In the DLL the restorer is its own
module, one file per pass as the others are, with the shader body as a C string equal to the
lab's.

**Two contingencies, stated now**: if `RGBA32F` colour attachments misbehave in the 3.2 core
context under Wine — the DLL has never rendered to a float target; its only non-8-bit attachment
is a `GL_DEPTH_COMPONENT24` **[SOURCE `tagpu_render3do.c:452`]** — fp16 is the first thing to try
(the bench already knows its error) and the 3.3 bump of §3 the second. And if fp32 GLSL cannot
meet the Q2 bar at all, that is a shader bug until proven otherwise: the two ONNX providers show
the graph itself is that stable.

---

## 4. Open  [OPEN]

- **The restorer covers terrain, features and effects; unit textures are next.** The unit
  atlas (`tagpu_render3do.c atlas_get`) needs its 4-texel pad, alignment and mips 0–2 and the
  unit shader's restored branch, and it waits for the unit-shading work in the other worktree,
  which edits the same fragment shader (§5 step 2).
- ~~**The near-key band's bar is the owner's call.**~~ **Decided 2026-09-05**: the Q2 bar on
  opaque texels farther than the model's depth from any keyed texel, the rest reported and judged
  by eye (mean 0.41 levels, 92 % within 1 — §4c); the owner accepted it on the four worst frames.
  The effects twin still has no lab reference at all.
- ~~**The first restore blocks nothing but is visible**~~ — **the reveal is progressive since
  2026-09-05 (G14e)**: the viewport of the biggest stock map is restored within 1.9 s of a 4.6 s
  whole-map job, centre-out (§4c). What remains visible is the first two seconds' worth of
  indexed cells filling in, and a feature drawing indexed for the frame or two before its
  restore lands — plus, while the terrain job runs at map load, every feature (its queue waits
  for the terrain's; on Two Continents that was 146 frames from first queued to painted).
- ~~**The cache's format is undecided.**~~ **Moot since 2026-09-05** — §2.5b removes the cache
  entirely, so the compression survey, the 4.4 GB ceiling and the content-keyed tile bank are all
  closed by the decision rather than by an answer. The measurements are kept in the git history
  if a cache ever comes back.
- **Definition → loaded model → texture frames**: still unwritten, but **no longer necessarily
  needed** — it is route 2a of §4b, and Options 1 and 4 there require no discovery at all. If it
  is ever established, it goes in [the engine map](exe-reverse-engineering.html).
- **Aircraft and boats** — the viewer prototype of §2.2/§2.3 has not been built.
- **Hires glb under Classic++** — lighting model, depth pass, shadow read-back, silhouette
  shadow off; deferred by §2.4.
- ~~**The lab's Classic++ lane is zoom 1 only**~~ — **stale, it zooms** since `7b45ee1`: all
  three lab shaders take `uZoom`/`uZoomC` (`tascene-view.html` 467, 598, 647) and the lab draws
  set them, and `shadowFrame` derives its bounds from the zoomed viewport extent, so the
  zoom-out look of §2.9 *can* be looked at today. What the lab still cannot show is §2.7's
  map-anchored texel grid: it rebuilds light-space bounds every frame, and crawl is only
  visible in motion anyway, so that one wants a capture in the game.
- **Nanoframe wireframe back edges** show through the unbuilt part — inherited from G13l,
  needs a stencil pass per nanoframe.
- **The restorer has run on one adapter.** The GLSL passes need GL 3.0 array textures, 3.1
  uniform blocks and `RGBA32F` colour attachments; NK adapts to `MAX_UNIFORM_BLOCK_SIZE` and
  `ARB_timer_query` is optional. Nothing has run on AMD, Intel or real Windows; the risk is
  speed (a slow GPU restores slower, at the same fps), not correctness. The ONNX path's
  "Windows needs the VC++ redistributable" and "DirectML picks device 0" go with it in landing 3.

---

## 5. Order of work

1. ~~**The restorer spike**~~ — done 2026-09-04: onnxruntime 1.20.1 x86 in the DLL, tiles
   restored at map load, cached, the terrain drawn from it under `tagpu_classicpp.on`.
1b. ~~**The GLSL restorer** (§4c)~~ — done 2026-09-05 in three landings: the lab bench (the Q2
   bar met, 1.15 s GPU), the engine (2.14 s at 59.7 fps in the game, the dump byte-identical to
   the lab), and the ONNX deletion. Steps 3–5 proceed alongside in their own worktree.
2. ~~**The feature and effects atlases**, lazily on first draw~~ — done 2026-09-05 (G14e):
   the size-class driver over `tagpu_gaf_atlas_get`, the colour-key stand-in, the twin per
   atlas, the two sprite shaders' restored branch; and the progressive reveal of the terrain
   (Q6). **Still to do: the unit atlas** — pad and align, mips 0–2, the unit shader's branch —
   after the unit-shading worktree lands, since both edit `tagpu_native.c`'s unit FS. No cache
   (§2.5b).
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
