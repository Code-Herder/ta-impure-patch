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
| Textures | the engine's 8bpp GAF frames as palette indices, `GL_NEAREST`, the `PALETTE.SHD` shade LUT | **restored true colour for all three atlases** — terrain tiles, feature sprites and unit textures — through the `unditherer` full model. Units: 4-texel padded atlas, trilinear to mip level 2, 4× anisotropic. Tiles and sprites: 1:1, `NEAREST`. *In the game: the terrain (G14c), the feature and effect sprites (G14e) and the unit textures (G14g, 2026-09-05) — the sprites and the units lazily on first draw; the unit atlas padded, aligned and mipped as this row says (§5 step 2).* |
| Terrain | the engine's 32-px tile blit, no height, no light | the same tiles in restored colour, per-pixel lambert from the heightfield normal, **normalised so level ground is exactly 1.0** (the art is already lit). *In the game since G14f (2026-09-05): the engine's height grid as an R8 texture per map, the lab's 16-px grid normals evaluated per fragment; feature sprites take the ground's lambert at their anchor as the lab's do* |
| Units | per-face shade row from `SH_L` through the 32-row LUT | per-pixel lambert in map space from the posed face normal, same level normalisation. *In the game since G14f: the face normal rides the vertex stream and replaces the LUT row under the switch; pieces the engine draws unshaded stay at exactly 1.0 (the lab lights every face)* |
| Shadows | the engine's rules. **In the game**: the 5-px silhouette drop for mobiles and the cached slant for structures, each blended once per silhouette pixel (G13n). **In the lab**: both since G14j (2026-09-07) — the silhouette for a mobile unit and, for a structure, the slant from the pack's caster mesh (every face of every visible, cached piece), projected and blended the way `tagpu_native.c`'s `emit_slant` does it. The silhouette only from 2026-09-04 to then; this row claimed the lab had both before that, when it had neither | a depth map along `shadowsun`, PCSS-lite (the blocker search: the receiver's own texel and the 16 Poisson taps since G14i, 8 ring taps before; 16-tap Poisson PCF), receiver-plane bias, per-caster length `14 + 0.25·height`; hills cast and receive; an airborne caster follows `airshadow` (§2.2). *In the game since G14i (2026-09-06): `tagpu_shadow.c`'s map-anchored depth map, the read-back in the terrain and unit shaders, the hills from a static mesh, the replacement meshes casting, the two Classic sub-passes off under the switch; measured against the lab in §5 step 5* |
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

**Looked at, 2026-09-05 (G14f)** **[MEASURED, by eye]**: Two Continents (`artlight` r +0.014,
art nearly unlit) takes the constant sun well — the ridges and the pond's basin read as relief
on `feat-forest` at zoom 1 and 0.25, and trees on a slope sit in the slope's light
([classicpp-light-two-continents.png](assets/shots/classicpp-light-two-continents.png), sun
on beside sun off). Metal Heck (r +0.659, the art painted from the engine's own azimuth)
barely changes: the sun adds a little relief to what the art already carries, no fight. Coast
to Coast (peak az 195, the art's light from the opposite side) shows nothing wrong on the
*land* at its steepest viewport — what it shows is the seabed, §2.3. No map has yet read wrong
because of the azimuth; the per-map option stays on the shelf.

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

**In the game (decided 2026-09-06, §2.12): all three are ported**, as `airshadow=len|physical|drop`
in `tagpu_classicpp.cfg`, default `len`, with the lab's semantics — so the judgement this section
defers to the game can be made in the game, live. `drop` is the one thing that keeps the Classic
silhouette alive under the switch: an air unit under `drop` is left out of the depth pass and
gets the stencil silhouette from the per-unit loop instead, exactly as the lab's `drawUnits` does.

### 2.3 Water: the seabed stays shaded and shadowed
The viewer lights and shadows underwater cells by the seabed, and never reads the sea level
in its lighting or shadow receivers **[SOURCE `tools/tascene-view.html`]**. **Decided: keep
it** — it is part of the approved look. Boats go into the same viewer prototype as aircraft,
so the shadow of a hull on the seabed is seen before it is built.

**What it looks like in the game, and the owner has not seen it** **[MEASURED 2026-09-05,
by eye — OPEN]**: Two Continents' sea is flat, so the decision never showed there. On Coast
to Coast, whose seabed has relief, the lit water carries large dark patches where the seabed
slopes away from the sun — the shore's own gradient continued under the surface
([classicpp-light-coast-to-coast.png](assets/shots/classicpp-light-coast-to-coast.png), sun
on beside sun off, eye 2176,1152). The water tiles are painted flat, so the lambert is the only
thing shaping them, and it reads as stains rather than as depth. Whether that is the look or
whether the seabed should take level ground's 1.0 (the lab knows the sea level as
`scene.map.sealevel`; where the engine keeps it has not been located) is the owner's call.

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

### 2.4 Hires glb units: they cast, they do not receive
`tagpu_hires_draw.c` lights in linear space with a GGX lobe; Classic++ is a lambert in sRGB
space, and hires meshes were in no depth pass. ~~**Decided: not a Classic++ concern.** Revisit
after the port.~~ **Revised 2026-09-06 (the owner: "if it's not going to be too expensive —
units are going to be between 2k and 3k" triangles): a hires unit CASTS.** Its vertex shader
already computes the posed model-space position before the engine projection, so the depth
variant is that same program behind a uniform switch, the same pose upload, the same
per-material draws and no fragment work — 2–3 k depth-only triangles per unit is under a
millisecond for a couple of hundred casters — and the model height for the length rule is the
replaced 3DO's AABB, which the gather already has. Its stencil silhouette goes off under the
switch with the 3DO ones. **It still does not receive**: the GGX lighting is untouched, so a
hires body standing in a cast shadow is lit as though it were not. That half stays deferred.

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

**Engines measured, full model, the reference setup (Ryzen + RTX 4070)** **[MEASURED 2026-09-04]**:

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
padding); output is clipped to 8 bits. **Those 12 levels are measured in the engine's own
palette (`main+0x143A7`), not in the one the screen is shown with**: the threshold is a raw
colour distance, so a gamma-scaled palette stretches every distance by the same factor and moves
tiles across it — 177 of Two Continents' 5062 wrap-padded at factor 1.5 against 400 at 1.0, when
the test briefly read the presented palette on 2026-09-09. Tileability is a property of the ART
([GPU status](gpu-status.html) §2.3f); the restore itself resolves through the presented one. The restored RGBA atlases are new objects beside
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

**The texel rule, refined 2026-09-06 (§2.12).** The zoom is continuous and eased
(`tagpu_zoom.c`: 10 % geometric notches, 0.25..8, eased over frames), so a texel that is a
smooth function of zoom would re-anchor the grid every frame of an ease. **Decided: octaves.**
The *base* texel is the lab's density at 1× — the light-space extent of the 1× window plus the
caster margins, over `shadowres`; the texel is `base·2^k` with the smallest integer `k`
(negative allowed, zoomed in) whose map of `res` texels covers the zoomed window plus the
margin; `res` is `shadowres` at zoom ≥ 1 and twice it, capped at 4096, below. So the grid
changes only at octave boundaries (about 1× and 0.42× at the defaults), an ease between them
moves the window by whole texels, and the density is never worse than §2.9's original sketch.
The depth range is the map's full 0..255 plus a 256-unit caster allowance (an aircraft under
`physical` at CruiseAlt 200 still fits), constant per map; the window's depth *offset* moves
with the window, which is a translation both sides of every compare share.

**Measured (G14i, 1024×768, the 896×704 viewport)**: the base texel is **0.862 world units**
(the light-space extent of the 1× window with the lab's margins is 1765 along `u`, and `u`
is what the height range does not enter — so the lab's "about 0.7 world units per texel" was
a guess and this is the number, the lab's too), `res` 2048 at zoom ≥ 1 and 4096 below it with
the texel unchanged down to 0.42× (`k = 0` logged at 0.83, 0.72, 0.64), `k = 1` and a
1.724-unit texel at the 0.25 floor with the map spanning 7060 units over a 3595×2838 window.
**The anchoring holds**: twelve `tacli eye` steps of (3, 2) px with a `glshot` each, the
shadow crop re-registered by the known scroll, differ by **0.00 levels on every consecutive
pair** (the frames differ on 630,626 px before re-registration) — the shadow moves rigidly
with the ground under it.

### 2.7b Soft shadows self-shadow the ground, and MORE as the map sharpens [MEASURED 2026-09-09]

**The defect, stated plainly: turning `Shadow quality` UP makes the picture worse.** On open
sea with no land anywhere in the sample — nothing that can cast — the water darkens by up to
50/255 in a blocky lattice. Measured at zoom 0.564 on `shadow-mix`, against the same frame with
`shadows=0`, over a patch that is 100 % water:

| `shadowres` | texel (world units) | acne (std) | worst darkening |
|---|---|---|---|
| 512 | 5.115 | **0.03** | 3.3 |
| 1024 | 2.558 | 1.03 | 33.3 |
| 2048 | 1.279 | 2.50 | 50.0 |
| 4096 | 0.639 | 2.50 | 50.0 |

At `Low` it is **effectively absent**. 2048 and 4096 tie because the zoom-out doubling (§2.7)
caps both at `res=4096, texel=1.279`, which the `shadow: frame` log line confirms.

**The lattice is the caster's, not the map's.** Autocorrelation of the shadow term on open water
peaks at 9, 18, 27, 36, 45 screen px — a fundamental of 9 px, and at 1.81 world units per pixel
that is **16.3 world units**: exactly the grid `build_hills` lays down (`o[0] = c*16`,
`o[2] = r*16 + hh*0.5`, `tagpu_terr.c`). The dumped depth map itself is clean — a smooth
gradient, 45 029 distinct depths, no blockiness — so the map is right and the **sampling** is
wrong.

**It is bias, not occlusion, and that was proved rather than argued.** Forcing the constant bias
to a floor of 24 world units takes the acne to **exactly 0.00 std / 0.0 darkening**. But that is
a diagnostic, not a fix: at that bias every unit shadow disappears (peter-panning, measured —
59 970 of 163 200 px changed in the unit region).

**Why it scales with the texel.** The blocker search is a **fixed 24 world units**
(`search = 24.0 / uShScale.x * uShScale.z`), and the receiver-plane bias `dot(o, dzduv)` is a
*linear* extrapolation across that distance. On curved seabed the extrapolation error is fixed
in world units, while the constant bias `(1 + 2(1 − nl)) × texel` shrinks as the map sharpens —
so past some resolution the bias no longer covers the error and false blockers appear. That the
required bias turned out to be ~24 world units, the search radius itself, is the confirmation.

**IT IS THE TERRAIN CASTER ALONE, and `terrainshadow=0` removes it completely**
[MEASURED 2026-09-09, and this supersedes the "candidate fix" this section first proposed].
The ground shadowing itself is the *hills* mesh (§2.8) casting onto the ground it was built
from. Same camera, same `shadowres=2048`, shadow term isolated against `shadows=0`:

| | shadow term (std) | worst darkening | 16-unit lattice (autocorr @ 9 px) |
|---|---|---|---|
| `terrainshadow=1` | 8.72 | 71.0 | 0.71 |
| `terrainshadow=0` | **0.00** | **0.0** | **0.00** |

**Exactly zero, not merely reduced** — and unit shadows are untouched by the switch, so
`terrainshadow=0` is a complete workaround today for anyone who sees it. That also relocates
the fix: it belongs to the **hills draw alone** (a depth offset on that one `glDrawElements` in
`tagpu_shadow_hills`, which casters conventionally get), not to the shared bias or the blocker
search. An earlier candidate here — capping the search at `min(24.0 / uShScale.x, 8.0)` — only
cut the acne 2.50 → 0.49 and touched *every* shadow including units, so it is the wrong shape
and is not the recommendation.

**What the fix is NOT — three candidates, all measured, none of them right.** Same camera,
`shadowres=2048`, shadow term isolated against `shadows=0` (baseline **8.72 std / 71.0 worst**):

| candidate | result | why it is wrong |
|---|---|---|
| constant bias floor of 24 world units | **0.00 / 0.0** | also erases every *unit* shadow — peter-panning, 59 970 of 163 200 px changed in the unit region |
| blocker search capped at 8 texels (`min(24.0/uShScale.x, 8.0)`) | 0.49 / 28.7 | incomplete, and it shortens the penumbra for **every** shadow, units included |
| `glPolygonOffset` on the hills draw alone | 6.48 @ ~1.1 wu, 5.27 @ ~4.3 wu, **3.22 @ ~12.8 wu** | scoped correctly but converges far too slowly; the magnitude that would finish the job is enough to visibly detach hill shadows |

The polygon-offset result is the informative one: **a uniform depth push cannot reconcile the two
surfaces**, which means the mismatch is not a constant offset but a *shape* difference between
the two reconstructions. (Note `units` is in minimum-resolvable-depth steps — here
3577 / 2²⁴ ≈ 2.1e-4 world units — so the useful range is tens of thousands, not single digits.)

**So the fix has to be structural: one surface, not two.** The receiver and the caster must be
the same geometry, which is what the lab does and what the game stopped doing at §2.8. That is
work in the terrain pass, not a knob, and it is not attempted here. Until it is done,
`terrainshadow=0` is the complete and correct answer for anyone who sees this.

**WHY THE LAB NEVER SHOWED IT: the lab has ONE terrain, the game has TWO.**
The shading maths is not the difference — `tascene-view.html`'s shadow function is
character-for-character what shipped: the same `(1.0 + 2.0*(1.0 - nl)) * uShScale.x /
uShScale.y` bias, the same `24.0 / uShScale.x` blocker search, the same 16 Poisson taps, the
same clamped `dzduv`. (Its own comment records hitting a cousin of this — *"without this the
blocker search found the hill under every hill pixel"* — and the receiver-plane bias is what
fixed it there.) The difference is **what geometry each one casts**:

- **In the lab, the caster IS the receiver.** `buildTerrainLab` fills `ltVAO` with vertices that
  each carry the world point they depict, and `shadowPass` casts *that same array*
  (`gl.bindVertexArray(ltVAO); gl.drawArrays(...)`). One buffer, one triangulation, one
  rasterisation — so caster and receiver depths can only differ by the map's own texel
  quantisation, which is precisely the quantity the texel-scaled bias is sized for. The bias is
  correct there, and it always will be.
- **In the game they are two different meshes.** §2.8's own premise is that the terrain gather
  emits screen-space quads *with no height*, so the receiver's world point is reconstructed
  analytically per fragment (`taTerrW`), while the caster is a **separate** world-space VBO
  built from the height bytes (`build_hills`: `o[1] = hh`, `o[2] = r*16 + hh*0.5`). Same bytes,
  two pipelines, one surface described twice.

Their disagreement is a **fixed world-space quantity**. It does not shrink with the texel, so a
texel-scaled bias covers it at coarse resolutions and stops covering it as the map sharpens —
which is exactly the 1/texel law measured above, and why the lattice is the caster mesh's 16-unit
cell rather than anything of the shadow map's.

**The generalisable lesson.** The lab is a faithful oracle for the *shading*, and it is not one
for anything that depends on caster and receiver being the same geometry. §2.8 created that
split deliberately and for a good reason; what was not recorded is that it also invalidated the
lab as the oracle for terrain self-shadowing. Any future pass that casts from a rebuilt copy of
something it also shades inherits the same blind spot.

**How to make the lab faithful again — mirror the split, do not remove it.** The lab is only
misleading here because it casts `ltVAO`, the very buffer it shades. Give `shadowPass` a second
terrain source: build a world-space heightfield mesh from the same height data the lab already
has, on the game's 16-unit grid and with the game's per-vertex shear (`z = r*16 + h/2`,
`build_hills`), and cast **that** instead of `ltVAO`. Put it behind a query knob
(`castsplit=0` to get the old behaviour back for comparison) and **default it on**, so what the
lab shows is what the game does.

That is worth doing for its own sake, not just to reproduce this bug: it turns a defect that
currently takes a game build, a scenario, a camera and a shadowres sweep — minutes per
iteration — into a browser reload, and it puts the game's actual topology under the lab's
existing shadow oracles. The A/B is immediate too: `castsplit=1` against `castsplit=0` on the
same frame *is* the caster/receiver mismatch, isolated, with no shadow maths in the way.

**And the rule this suggests for the lab in general:** the lab is a faithful oracle for a pass
only where its data flow has the same *shape* as the game's. Where the game builds a second
representation of something — a rebuilt mesh, a reconstructed position, a cached copy — the lab
has to build it too, or its verdict on that pass does not transfer. Worth checking the other
passes for the same asymmetry before trusting them.

**How to measure it, because the obvious metric lies.** The raw standard deviation of the water
band is ~38 either way: it is dominated by the tile art, and it moved by 0.15 when the artifact
went from full to absent. The shadow term has to be isolated against an otherwise identical
`shadows=0` frame first; only then does the signal appear (8.72 → 0.00).

**Not a regression of the G18 landing.** The landed build and `bbceeb8` (main before it) render
this scene **byte-identically with soft shadows on — 0 of 270 000 px differ**. The menu only
made the knob reachable, which is how it was found.

### 2.8 Hills cast: a static per-map heightfield mesh
The terrain gather emits screen-space quads with no height **[SOURCE `tagpu_terr.c`]**, so
the depth pass has nothing of the ground to draw. **Decided: one world-space VBO of the whole
heightfield at the engine's 16-px cells, built when the map loads** from the height byte the
feature pass already reads (§3), drawn once per frame in the depth pass. Two Continents is
672 × 800 cells. It joins the GL reset protocol like every other object.

### 2.9 Classic++ applies at every zoom
Under `vpwide` the view reaches 16× the area at the 0.25 floor. **Decided: all zooms.** The
shadow map's world-per-texel follows the zoom: ~~2048² at zoom ≥ 0.5, 4096² below~~ **2048² at
zoom ≥ 1, 4096² below, the texel by octave (§2.7, refined 2026-09-06)**; the PCF minimum
radius follows the texel. Restored textures are mipmapped to level 2, which covers
0.25 at `ss=2`. The unit vertex cap is unchanged because the depth pass reuses the frame's
one vertex buffer. The softer shadows at the floor are judged in play — the lab's Classic++
lane runs at zoom 1 by construction and cannot show them (§4).

### 2.10 Settings: an in-game screen, drawn by the engine's own GUI  [REVISED 2026-09-09]

**Decided: the render options are a real `.GUI` screen — the engine's own gadgets, its own
GAF art, its own dispatcher — not a panel the DLL paints.** Six stage buttons, no pages.
This supersedes the original decision below, which was for a DLL-drawn panel hanging off an
"Options" button in the top bar.

The record layout, the stage-button frame grammar and the panel recess grids are on
[GUI gadgets](gui-gadgets.html) §10; the lab that draws the screen is
`tools/ta-guiscreen.html`, and `tools/guiart.py` extracts the art it needs (nothing of the
game's is tracked). Interviewed with the owner 2026-09-08/09.

**The two rules the owner set.**

*Seven was the count before mouse-wheel zoom was cut (below); the row table and every
geometry number here are six.*

1. **The menu never offers the unmodified original engine.** No row has an "off, let the
   1997 code draw it" position — we own the draw, and the only question a row asks is which
   of *our* two renderers owns it.
2. **Simplify.** Six gadgets, and everything else demoted to the cfg.

**The screen — a drop-down, not a stock rect** [SHAPE DECIDED 2026-09-09]. `RENDER.GUI`,
panel `id=0` at `(w−16−304, 32) 304×212` — right-aligned by `MARGIN = 16`, hanging from the
top bar's underside, over the world. Background gadget `id=12` naming its panel frame. Six
`id=1` buttons at `x=166 w=120 h=20` on a **28 px** pitch, each with an `id=5` label at
`x=14 w=144` **on the same line**:

*The height is 212, not the 240 an earlier revision of this line said — `tools/guipanel.py`
is the source of truth for the geometry (`W,H = 304,212`, `DIV_BOT = 202`, the last row
ending at 194), the paragraph on modality below already said 212, and the built screen
measures 212.*

| y | row | stages | what drives it |
|---|---|---|---|
| 9 | *(the caption, `Render options`)* | — | an `id=5` label above the rule at y=30 |
| 34 | **Renderer Style** | Classic \| Classic++ \| Custom | sets every row below it |
| 62 | **Undithered assets** | Off \| On | `assets=` — done, G18a |
| 90 | **Dynamic lighting** | Off \| On | `light=` — done, G18a |
| 118 | **Shadows** | Off \| Hard \| Soft | `shadows=` — done, G18b |
| 146 | **Shadow quality** | Low \| Med \| High \| Ultra | `shadowres=`, live only at Soft |
| 174 | **Supersampling** | Off \| 2× | `tagpu_ss.off` |

**Every row is live, and that is a rule the menu keeps** [DECIDED 2026-09-09]. Mouse-wheel zoom
was the seventh row and was **cut**: `tagpu_zoom_init()` runs *once* from `dllmain.c:130` and
installs byte patches, so flipping `tagpu_zoom.on` mid-game lights the plate green and changes
no pixel until the next launch. It is also a play mode rather than a rendering option. The
lever stays; the row goes. The other six were checked against the code and all take effect on
the next frame — `assets`/`light` as per-frame uniforms, `shadows` as a per-frame branch,
`shadowres` because `tagpu_shadow.c:223` reallocates the depth texture when the edge changes,
`ss` because `tagpu_ss.off` is re-`stat`ed per unit render. So **no row ever needs an asterisk**,
and any future row must clear the same bar or stay in the cfg.

*This supersedes a 150×352 panel at `(128,128)` — the rect `VISUALRT.GUI` uses — with the
label 16 px **above** its control on a 44 px pitch. The label moved beside the control, and
that is the whole reason the frame has to be composed rather than reused: every stock panel
puts the label above, which seven rows have no room for.*

**It is NON-MODAL, and it does not pause** [DECIDED 2026-09-09, superseding the click-away
behaviour prototyped the same day]. The panel opens on the sprocket and closes on the sprocket;
clicks anywhere else go to the game untouched. Two things forced it, and both are measurements:
**every row is live**, so a menu you must dismiss to see the effect of is the wrong shape —
you would click, close, look, reopen; and the panel is 304×212 in a corner, covering ~8 % of a
1024×768 frame and none of the side panel. It also removes the only place our input code would
have had to arbitrate with the game's, and makes the earlier *"not measured: whether a `.GUI`
dispatcher reports a click outside its `id=0` rect"* moot — nothing needs that answer now.

**It must not pause the sim, in either mode** [OWNER'S CONSTRAINT 2026-09-09]. That is free:
pausing is a separate flagged action, not a property of the push (`ARMOPT.GUI` and a build page
share a rect and a push path, and only the first pauses), and it is **single-player only**
anyway. The exit criterion is ready-made — the `+clock` cheat draws game time from the sim tick
at `main+0x38A47`, and the notes record the seconds *stopping* with TA's own menu open, so
"the clock still ticks with our panel up" is a two-screenshot test.

**No Apply button** [DECIDED 2026-09-09]. A stage button **is** the setting — there is no
edit buffer for an Apply to commit — so `OnCommand` writes the row's key on the click and
the panel is dismissed by the trigger or by clicking away, the way a drop-down is. Not only
a visual choice: it removes an eighth gadget from the `.GUI` and means no code ever has to
gather seven gadgets' state at once. It also takes 40 px off the panel, which is why the
height is 240 and not 280.

**The ground is `frontend.gaf`'s own `back*` nine-slice** [DECIDED 2026-09-09] — the shell's
mottled panelling, 64×64, composed at 304×240 by `tools/guipanel.py --nine back`. Three were
built and looked at: `dia` (TA's dialog exactly — `diatile` is *one colour*, flat black, in a
grey bevel), `back`, and a `hybrid` putting the back texture inside the dia frame. `back`
was chosen. **Use `backtile` frame 4, not 0** — frame 0 carries a lit bottom edge that puts
seams through a tiled centre. Only the seven recesses are drawn over it; `text16*` is a
*blue* text-field well, not a neutral recess, which is why they cannot come from the kit.

**What opens it: a frameless sprocket on the top bar** [DECIDED 2026-09-09]. 28×28 in the
32 px bar, right-inset by **`MARGIN = 16`** — the *same* margin the panel is right-aligned
by, so the icon's right edge and the drop-down's right edge land on one line and the menu
visibly drops from the icon. Both are anchored to the frame's **right edge**, never to a
fixed coordinate: `trigger_at(w) = (w − 16 − 28, 2)` and `panel_at(w) = (w − 16 − 304, 32)`,
which is 980 and 704 at 1024, 1876 and 1600 at 1920. `tools/guipanel.py --trigger` generates
the icon; nothing of the game's art is in it. **The DLL draws it and hit-tests it** — see the
resolved *who hosts the trigger* gate below — so it never enters the `.ufo` and stays generated
geometry plus nine palette indices.

*The inset was 36 for a day — the position picked by eye from the prototype, which was not
derived from anything. `MARGIN` is the only non-arbitrary number available, and using it for
both rects is what turns the placement into a rule.*

- **The engine already ships the idea of a frameless icon button.** `mainmenu.gui` GADGET5
  `Credits` — the Cavedog logo — is an ordinary `id=1` button with `text=` empty and
  `attribs=1026`, where every other button on that screen is `attribs=2`. Its art is
  `anims/mainmenu.gaf`, one entry, 80×40, five frames, and it has no plate, no bevel and no
  text.
- **The "faint tan" is a palette ramp, not a colour.** TA's palette 55..63 is a dark warm
  ramp — `55 (95,99,71) · 56 (91,87,59) · 57 (83,67,51) · 58 (71,59,43) · 59 (59,51,35) ·
  60 (47,43,27) · 61 (35,31,19) · 62 (23,19,15) · 63 (11,11,7)` — and the Cavedog logo is
  drawn **entirely** inside it: its resting frame is 778 px of 62, 395 of 61, 205 of 60,
  113 of 59 and three of 58. The outline reads as faint because it is three ramp steps above
  its ground, not because it is desaturated.
- **The state change is a slide along that ramp, not a second picture.** Of the five frames,
  0/2/3 are identical, 1 drops 58 entirely and 59 falls 113 → 24 (pressed), and 4 gains
  56/57 and more than doubles 58 (over). Nothing moves. That is how a button with no plate
  still reads as a button, and it is what the trigger's four frames do.
- **The logo's own scheme could NOT be copied straight onto the bar.** Measured on real
  1920×1080 and 1024×768 skirmish frames: the top bar is exactly **32 px** (rows 0..31; row
  32 is the world) and **its own texture is index 62** — the modal bar pixel is (23,19,15),
  the same value that is 52 % of the logo's ink. Laid on the bar the logo's ink would be
  invisible. So the ramp is re-hung around a lighter ground: the **body goes below** the bar
  (63, near-black at the core) and the **outline above** it — `59` at rest, `57` over, `60`
  pressed — which keeps the logo's three-step relationship around a different centre.
- **The bar has room.** Quiet runs (no bright art in the 32 px band) measured at 1920:
  x 984..1327 and 1497..1840, 343 px each, plus 1841..1920 at the corner. At 1024 the whole
  right end from x≈940 is quiet. The corner itself was prototyped and rejected in favour of
  the inset, where a window border cannot clip it.
- **The bar's own art is LEFT-anchored, and that is why no inset can be chosen to dodge it**
  [MEASURED 2026-09-09]. On 1024 and 1920 frames of the same map every seam sits at the
  **identical x** in both — 123, 132, 169, 215, 352, 397, 418, 468, 604, 641, 798, 814,
  983… — so the bar is drawn from the left and the extra width at 1920 is simply more of it.
  Past the resource readouts it repeats on a **513 px period**: seams at 814, 983, 1327,
  1496, 1840, gaps of 169, 344, 169, 344. (`LIGHTBAR` frame 1 is 507×32, suggestively close;
  the tile is *not* confirmed and the phase origin is not pinned.) A seam's distance from the
  **right** edge therefore changes with resolution — 40 px in at 1024, 79 px at 1920 — so
  chasing it would make the icon's position resolution-dependent, for a 4 px feature peaking
  at 71 on a bar whose own texture already reaches 59.

  **What that does cost is the bore, which is transparent** — a seam crossing it reads as a
  defect rather than as texture. At `MARGIN = 16` the seam falls on the icon's left teeth at
  1024 and misses the icon entirely at 1920; the bore is clean in both. An inset of 26 would
  have put a seam straight through the bore at 1024, which is the one placement to avoid.
- **Rejected first: the icon on a plate.** Two rounds went to `commongui.buttons0` — frames
  0..3 (16×16, which leaves a 10 px canvas in a 32 px bar and made every cog near-abstract)
  and then frames 24..27, the real 96×31 in-game menu plate `ARMOPT.GUI` and `PREFS.GUI` use.
  Six 27×27 cogs were drawn for that plate before the whole plate idea was dropped. Two
  findings survive it: **the 96×31 plate's face is 26 px, not 31** — row 0 and rows 28..30
  are bezel and row 27 a bright bottom chamfer — and **the ink inverts on it**, since it is
  light brushed steel (~155 normal / 187 hover / 123 pressed) rather than the dark green of
  the 16×16.
- **Eight teeth, and it matters.** 8 is the only count that pixelises cleanly at this size,
  because 45° steps put every tooth in mirror symmetry with another across an axis or a
  diagonal; 10 and 12 gave ragged flanks and, at 12, a rough circle at 1:1.

**Custom is derived, never chosen.** Clicking Renderer alternates Classic and Classic++;
touching any row below makes it read Custom. **Shadows Off / Hard / Soft falls out of the
model rather than being invented**: Classic's shadows *are* the hard ones (the 5-px
silhouette drop and the cached slant, G13n) and Classic++'s *are* the soft ones (the
map-anchored depth map, PCSS-lite, G14i), and a three-stage button is what the engine
already draws for that (`stagebuttn3`).

**Absorbed, not dropped:** `tagpu_vpwide.on` has no row because `tagpu_opt.c` already pairs
it with `tagpu_zoom.on` through the `needs` column — one Zoom row arms both.

**Demoted to `tagpu_classicpp.cfg`:** `penumbra`, `shade`, `airshadow`, `terrainshadow`,
`amb`, `unitsun`, plus `tagpu_hires.on`, `tagpu_nano.off` and `tagpu_fpsosd.on`. Still
written and still editable — a human, a tacli verb and the screen drive the same state —
just not player-facing options.

**State is still the trigger files and the cfg** (unchanged from the original decision):
on/off is a file the screen creates and deletes, polled per frame; numbers are `key=value`
in `gamedir/tagpu_classicpp.cfg`, re-read on mtime change, keyed like the lab's URL
parameters. The shadow keys are §2.12's, and the depth map still runs only when
`shadows=1` **and** the engine's own Shadow option bit is set (`main+0x37F06` bit 2;
`tagpu_shadow.c:359` reads it) — the player's in-game Shadows toggle keeps its meaning.

**How it is assembled** [DECIDED 2026-09-09, with the owner]. The governing rule the owner set
is **use TA's gadget/UI mechanism as much as possible**, and every choice below was taken under it.

1. **`impure-patch.ufo`, written by the DLL at `DLL_PROCESS_ATTACH` if absent.** New names go in
   a `.ufo` and overrides go loose — measured, `file-formats.md` §5 — and `RENDER.GUI` plus our
   GAF are new names, so a `.ufo` is the engine's own answer. The DLL writes it rather than CI
   shipping it, so distribution stays **one `ddraw.dll`** and the archive can never drift out of
   step with the DLL that expects it. The writer is cheap because a **literal-only SQSH method-1
   stream is a valid uncompressed encoding** (`file-formats.md` §5), so there is no compressor.
   ○ **If the asset set ever grows much beyond these two files, revisit and let CI build it
   instead** — the single-file property stops being worth a hand-rolled archive writer at some
   size, and that trade should be re-taken rather than inherited.
2. **The frame in the `.ufo` is our *drawn* panel** (`guipanel.py`'s `draw_panel()`, nothing
   sampled), because Cavedog's pixels can never ship. At screen-load time the DLL composes the
   chosen `back*` ground from the **player's own install** — read through the engine's loader,
   long after HAPI is up — and repaints the frame's pixel buffer in place (`+0x10
   PtrFrameBits`). Ship the frame **uncompressed** and that is a flat `w*h` copy. If the
   composition ever fails the drawn frame is still there, so the failure mode is a plainer
   panel, not no panel.
3. **The screen is pushed with `0x495207`'s idiom**, transcribed: save `main+0x37EA0`, write
   `"RENDER.GUI"`, `GUI_Load(gi, main+0x37EA0, flags)`, set `+0x08` to our `OnCommand` and
   `+0x0C` to `main`. Closing restores the saved name and lets `UpdateIngameGUI` pop us — **we
   never call `GUI_Pop`**. Without this the engine pops the screen at the next of 21 call sites.
4. **State is set through the engine and drawn by the engine.** `0x4A1080(gi, name, value)` is
   read and thin — name scan, `mov [rec+0x137],cl`, return 1, **no clamp, no callback, no
   redraw** — so it is the direct field write plus a lookup, which is also exactly what TA's own
   code does at `0x477416`. Repaint is separate: `GUI_StageUpdateDraw 0x4A81E0(gi, 0x40)`.
   **[CORRECTED 2026-09-09 by the landing review: the engine DOES advance it.]** Three sites
   `inc` `+0x137` — `0x4A6EC8`, `0x4A9DB6`, and `0x4AA377`, the last wrapping against the stage
   count at `+0x136` and skipped entirely when `grayedout` bit 0 is set (`0x4AA36A`). So
   `OnCommand` advances **its own model** and re-pushes every row, which overwrites the
   engine's advance; it must not advance the gadget field itself, or every click would move two
   stages. ● **`gi+0xCCA` is identified**
   (2026-09-09): the screen's **deferred-repaint flag**. About twenty state-changing calls set
   it, there are bare accessors at `0x49FA90`/`0x49FAB0`, and its one reader in the GUI pump
   (`0x4AA0AF`) clears it and calls `GUI_StageUpdateDraw(gi, top->flags | 0x40)`. So it is not
   a precondition of anything; setting it is *better* than calling the draw by hand, because
   it repaints with the screen's own flags and coalesces several changes into one repaint.

5. **The trigger's hit-test must sit on BOTH input paths.** `tagpu_shield.c` handles the
   injected `WM_TAGPU_MOUSE` → `deliver_mouse()` *before* the shield check, and then, with the
   shield on, swallows every real `WM_LBUTTONDOWN`. So a tacli instance sees **only injected**
   clicks and a player sees **only real** ones. A hit-test hung off one path passes its own
   tests and fails for players, or the reverse — it has to be one function called from both.
   (The same shape as the `field-notes` patch-2b bug, where a cursor change quietly altered
   what a left click did.)
6. **`OnCommand` does not write the file.** It sets an in-memory value; the cfg is written at
   the next present. TA is lockstep, `OnCommand` runs on the game thread, and a synchronous
   write there is an unbounded stall — a slow disk, a scanner touching a just-written file, a
   network drive — which can drop a player from a session whatever the content was. The
   settings are render-only so they cannot desync *by content*; this is about the stall.
   Deferring also coalesces four rapid clicks into one write, and the cfg poller re-reads on
   mtime either way, so nothing downstream changes. *Note the existing knobs are written
   synchronously — but by humans and by tacli, from outside the game thread, which is not the
   same thing.*
7. **If the `.ufo` is unusable: silent on screen, loud in `tagpu.log`, and rewritten every
   launch** with a version stamp. Silent because a rendering menu failing to appear must never
   cost someone a game; logged because that is this codebase's idiom (`terrown: ARMED`, and the
   ta-drive skill's advice to `md5sum` the DLL when a module does not log). Rewritten
   unconditionally because staleness after a DLL upgrade is the one failure here that would be
   genuinely confusing, and it costs a few ms at startup. *Write access to the gamedir is not a
   new requirement — the DLL already writes seven files from 17 create-for-write sites.*

**WHAT WAS BUILT, and the six things the live runs corrected** [BUILT 2026-09-09, G18 gates
1-3; `tagpu_menu.c`, `tagpu_ufo.c`]. All three gates are met on a 1024×768 skirmish. The
design above survived contact almost intact; what did not is recorded here rather than
quietly fixed, because every one of them cost a build-and-launch cycle.

1. **`GUI_Load` stamps its name argument into `ControlsAry[0].name`** (`0x4AAC98`), which is
   what `IsOnTop` compares — so the `.GUI`'s authored `name=` is irrelevant and the string we
   pass is what matters. `armmain2.gui` says `name=HEADER;` on disk and reads `ARMMAIN2.GUI`
   live for exactly this reason.
2. **`flags & 0x400` suppresses GUI_Load's STAGE 1, not just a repaint** — and stage 1 is
   what builds the panel's surface. Using it to patch the right-aligned `xpos` first and then
   asking for a bare `0x40` repaint left the panel with no surface and the engine composited
   the frame's own pixels at its rect. Reproduce the suppressed call (`flags | 1` under the
   `0x4C2470`/`0x4C2870` pair), do not replace it.
3. **The tick cannot hang off `UpdateIngameGUI`.** None of its 21 call sites is the frame
   loop. `DrawGameScreen 0x468CF0` is.
4. **A world click rebuilds the whole in-game GUI stack** — a fresh `ARMMAIN2.GUI` with a NULL
   `per_active`, our screen freed — and a panel over the world takes its own clicks through
   that path. The re-push is the right recovery; **re-reading the levers on it is not**, and
   doing so put every plate back the moment it was clicked. Reading the levers is edge-
   triggered on the player's open; a recovery re-push keeps the model.
5. **`gi->UIChange_f == -1` is not proof of a pop** (the pump resets it and calls `OnCommand`
   again on the same click). The `per_active` chain is the authority.
6. **The `id=12` gadget does not load the GAF — the panel does**, so the file is
   `anims\<screen>.GAF` and not `anims\<gadget>.GAF`. Named after the gadget it was never
   opened; named after the screen it loads. See [engine map](exe-reverse-engineering.html)
   *A screen's own GAF*.
7b. **The Renderer row applied nothing, because the menu drove only ONE of the switch's
   two files.** `tagpu_opt.c`'s precedence is *an `.on` wins, and an `.off` only defeats a
   pass that was on **by default***, so writing `tagpu_classicpp.off` alone fails in both
   directions: with a hand-armed `.on` present (what `tacli arm` writes) the `.off` is inert
   and Classic++ can never be turned off, and on any tacli instance — which carries
   `tagpu_defaults.off`, so the table's default does not apply — deleting the `.off` is not
   enough to turn it **on** either. The screen now owns both files, which is correct under
   the shipped DLL, a tacli instance and a hand-armed `.on` alike. Measured: Classic++ →
   Classic changes **1 755 893 of 2 073 600 px** outside the panel, and the round trip back
   returns to **53 px** of the original — the cursor and a restoring tile.
   *Supersampling deliberately keeps its single file: `tagpu_ss.off` is read directly by the
   native pass, there is no `tagpu_ss.on` and no table entry, so inventing one would arm
   nothing.* **The general lesson: a row that drives a `tagpu_opt` pass must write the pair,
   because the table's default is only one of three configurations it will meet.**
7c. **A row that cannot bite is now greyed rather than left looking live.** Four of the six
   describe Classic++'s behaviour and are inert under Classic; leaving them reading `On`
   there was the menu asserting something untrue. `grayedout` also makes the engine refuse
   the click, so the two are one change.

7. **A panel over the world takes its clicks through the ZOOM TRANSFORM**, and at any zoom
   ≠ 1 that bends them — so no row worked at all. Found in play within minutes, and invisible
   to every gate test, because those ran at zoom 1 with nothing else armed. `tagpu_zoom.h`
   already promised dialogs arrive unmodified and noted it cannot see them; the screen now
   answers `tagpu_menu_owns_point()` for the sprocket and its panel, and both
   `tagpu_zoom_mouse_lparam` and `tagpu_zoom_drop_mouse` pass such a point through untouched.
   Verified at 3.138×. *The same trap still bends `ARMOPT`, `EXITMENU` and `YESORNO`, which
   the engine also draws over the world — this fixes only the screen that hits it on every
   click.* **The lesson for the next screen over the world: a gate test at zoom 1 with a bare
   arm set is not a test of the thing a player uses.**

The oracles, for the record: the sim tick at `main+0x38A47` ran **1801 → 2057** over four
seconds with the panel open and **2147 → 2147** with TA's own `ARMOPT` open, which is the
non-modal claim measured rather than asserted; every row reports the engine's own `stage N →
M` confirmation and the cfg on disk follows; `Shadow quality` greys at `Shadows ≠ Soft` and
the engine then **refuses the click**; and the sprocket opens and closes the menu while a
click at (500,400) does neither.

**Still open after the spike.** The panel is torn down and re-pushed on every world click, so
a click costs one frame of the panel being rebuilt — cheap, but visible if you look for it,
and worth closing if a better re-assert exists. `stagebuttn2` (green/green) is still
unreachable: a `stages=2` button with `texturenumber=0` gets `stagebuttn1`, the red/green
plate, which is right for the Off/On rows and would be wrong for a neutral two-choice one.

**What that buys, and when it would stop being worth it.** The engine keeps hit-testing,
dispatch, the `stagebuttn` pressed/greyed art, `hattfont12` labels at any resolution, and the
`panel+0xB8`/`+0xBC` save-under — and, decisively, `tagpu_gui_surf.c`'s *"every engine surface
the publisher has seeded gets a twin"* means the panel is **undithered and sharp for free**
through G15e's colour twin and G17a's device-res layer. What we do ourselves is all *values* and
no mechanism: two bytes and a repaint request, one frame's pixels, one 28×28 trigger, one small
archive. **If the menu ever needs a control TA has no gadget for** — a colour picker, a preview,
a scrolling list — the gadget system stops paying and the superseded DLL-drawn panel wins.

**What has to be built before the screen can exist** — the gates are in the roadmap:

- **Split the switch** — ● **done 2026-09-09 (G18a)**. `assets=0|1` and `light=0|1` in
  `tagpu_classicpp.cfg`, read by `tagpu_classicpp_assets()` and `tagpu_classicpp_lit()`
  (each is the master arm AND its key, so neither can be on while `tagpu_classicpp.on`
  is absent). *Undithered assets* and *Dynamic lighting* are real, independent rows.

  **`uLit` was not the lambert, and feeding it from `light=` was wrong** — the mistake is
  recorded because this page asserted otherwise. `uLit` is the **Classic++ colour branch
  itself** in all three shaders ("Nothing below this branch runs under Classic++, nothing
  in it runs under Classic", `tagpu_terr.c`), and `uRestored` is read *inside* it. Feeding
  `uLit` from `light=` therefore dropped the whole frame back to Classic and silently
  overrode `assets=`: measured on the parity fixture, `assets=1 light=0` and
  `assets=0 light=0` came out **byte-identical**, 0 px apart. What was actually built:

  | | fed from |
  |---|---|
  | `uLit` — the colour branch, and with it the RGB fog rule of §2.6 | the master arm, `tagpu_classicpp_on()` |
  | `uRestored` (`tagpu_terr.c`, `tagpu_native.c`, `tagpu_feat.c`, `tagpu_fx.c`), the terrain restore step, `tagpu_gaf.c`'s twin, the restorer's job pump, and the UI colour twin of G15e | `tagpu_classicpp_assets()` |
  | `uLambert` — **new**, in `tagpu_glsl.h`'s `TAGPU_GLSL_LIGHT_UNIFORMS`; and the ground lambert `tagpu_feat.c` bakes into an anchor | `tagpu_classicpp_lit()` |

  **`uLambert == 0` does not skip `taLambert` — it hands it the LEVEL normal**, so the
  slope shading goes and the shadow term, which lives *inside* `taLambert`, stays. Level
  ground is exactly 1.0 there by construction (`uNorm` is `1/level`, and `level` is that
  same lambert of the up normal), which the measurement confirms: `light=0 shadows=0` is
  **byte-identical to `sun=off`**, 0 px. A feature has no normal to flatten, so there
  `light=` is applied by baking 1.0 instead.
- **Stop `sun=off` clearing shadows** — ● **done 2026-09-09 (G18b)**. `sun=off` no longer
  forces `amb=1.0` and no longer writes `s_light.shadows` at all: it simply **is** `light=0`,
  the same level normal handed to `taLambert`. The picture does not move — the new
  `sun=off shadows=0` is **0 px** from the old `sun=off` on the parity fixture — and the
  shadow term, which lives inside the lambert and was being multiplied by `(1 - amb) = 0`,
  survives: `sun=off shadows=1` differs from `sun=off shadows=0` by 1800 px, and is 0 px
  from `light=0 shadows=1`. Note `sun=off` flattens **unit** lighting as well as terrain,
  which is why the row is "Dynamic lighting" and not "Terrain dynamic lighting".

  Two consequences worth having. `tagpu_shadow.c`'s `amb >= 1.0f` refusal is now reachable
  only through an explicit `amb=1`, where it is an honest early out rather than a policy —
  at `amb = 1` the pass would cost a depth render and change no pixel. And the **level-ground
  rule now holds with a shadow on it**: level ground takes exactly 1.0 in both lanes whether
  or not it is shadowed, because a level cell's own normal *is* the normal `light=0`
  substitutes. Measured: 300 026 level-ground pixels identical between the lit and the flat
  lane, 558 of them inside the soft shadow. The old `sun=off` could not show this — it had
  no shadows to compare.
- **A third value on `shadows=`** — ● **done 2026-09-09 (G18b)**. `shadows=` is now
  `0` none, `1` **soft** (the depth map of §2.12, the default) and `2` **hard** (Classic's
  own 5-px silhouette and cached slant, emitted under the switch). The two are never both
  on: `tagpu_shadow_begin` runs only at `1`, and `tagpu_native.c`'s slant emission and
  silhouette loop only at `2` — or under Classic, which the key does not govern at all.
  An out-of-range value is reported as a bad token and the default stands.

  **A/B'd against `classicpp.off` at the same eye** on `shadow-lab` (six units and
  structures, the sim paused with Tab so the animating mex and wind blades hold still):
  Classic's shadow mask 2144 px, ours at `shadows=2` **2067 px**, intersection 2006 —
  **IoU 0.910**, 138 px Classic-only and 61 px ours-only, all of them one-pixel slivers on
  the same silhouette edges. That is inside §2.12's "shadowed-pixel counts within 5 %" bar
  (−3.6 %). `shadows=0` also stops an aircraft's `airshadow=drop` silhouette, which the
  switch used to draw whatever `shadows=` said.
- **Panel art and an entry point** — ● **answered and BUILT 2026-09-09 (G18 gate 3).** Seven `h20`
  gadgets need seven `h20` recesses and no stock runtime panel has more than five; `PREFS`
  has six recesses and uses all six, so there is nowhere to hang a "Graphics" button either.
  Both answers are above: the ground is **composed at runtime** from `frontend.gaf`'s `back*`
  nine-slice with the recesses drawn over it, and the entry point is the **frameless sprocket
  on the top bar**. `tools/guipanel.py` builds both the way the DLL will — see
  [GUI gadgets](gui-gadgets.html) §10.3.

  **Nothing of the game's art is carried either way**, but the mechanism changed on
  2026-09-09 once the loader was read. *Superseded: "composed in memory and appended to the
  gadget GAF blob at `gi+0x04`".* `gi+0x04` really does hold **one** bank from **one** call
  site (`0x49154E` → `0x4AEEE0` with the hardcoded `"commongui"`) — but the corollary drawn
  from that, *"so a second file the engine finds by itself is not an option"*, **is wrong**:
  gadgets load their own GAFs by name (115 `*_gadget.gaf` ship; `armopt.gaf`, `prefs.gaf`,
  `mainmenu.gaf` exist plain). See [engine map](exe-reverse-engineering.html) *The GAF banks a
  screen can reach*. So the art arrives as **a file the engine loads itself**, and no
  in-memory bank surgery is needed at all.
- **Whether a new `.GUI` name can be pushed at all** — ● **ANSWERED 2026-09-09: yes.**
  `GUI_Load 0x4AA8F0(gi, name, flags)` turns the name into a **file path** (`<prefix at
  gi+0x9B6><name>` + the extension `"GUI"` at `0x502828`, opened by `0x4BBC40`), so the exe's
  screen-name string table is never consulted; our DLL is the call site and no stock screen is
  sacrificed. The push itself is `0x4AAC56` and `flags & 0x200` suppresses it. Full reading,
  including the `0x495207` template to copy: [engine map](exe-reverse-engineering.html) *The
  screen lifecycle*.
- **Who hosts the trigger** — ● **ANSWERED 2026-09-09: nobody can; the DLL draws and
  hit-tests it.** Not a preference — geometry. Every in-game screen's panel is the *side*
  panel (`armmain2`/`cormain2`/`armmain`/`armgen` are all `(0,128) 128×352`; `tabmenu` alone
  is `(130,−33) 510×33`), **no GUI screen owns the top bar**, and a gadget is drawn into its
  panel's own `w×h` surface at panel-relative coordinates — so a gadget at `x=960` has nowhere
  to be drawn. The `ARMMAIN2`/`CORMAIN2` idea is dead twice over: it is also covered by a
  builder's page (LIVE: `ARMCOM1.GUI`, `under: ARMMAIN2.GUI`), which is most of a game.
  The 28×28 trigger is therefore ours to draw and hit-test; the menu it opens stays entirely
  the engine's.

*Superseded in part, kept so it is not re-derived.* The original decision (2026-09-04) was a
small "Options" button at the top right over the engine's top bar, opening a **DLL-drawn**
panel of buttons and steppers for every Classic++ knob; three shapes were prototyped against
it (a drop-down, a full panel, an edge rail; the full panel won).

**The placement came back; only the drawing moved.** The 2026-09-09 design is again a small
icon at the top right of the bar opening a drop-down — what changed is that the panel is the
engine's own gadgets on its own GAF art instead of pixels the DLL paints, and that the
drop-down beat the full panel this time round. So the old reasoning is not moot, it is the
**fallback**, and it still holds where the overlay is concerned: the composite draws our
non-empty pixels over the engine's frame outside the viewport, the top-right ~400 px of the
1024-wide frame are bare panelling, the DLL sees every window message before the game and
already swallows the wheel, and our pixels cover the engine's cursor. That last set is
exactly what a DLL overlay forwarding the trigger click would rest on, which is why the open
question above has two answers and not one.
### 2.11 Two small calls made by the implementer
- **The unit vertex stream** grows from 11 to 15 floats: the map-space normal and the
  world height join `x, y, depthEnc, u, v, flat, ck, shadeRow, wx, wzp, vy`. Face normals are
  already computed on the CPU for the shade row **[SOURCE `tagpu_render3do.c` ~794]**, so
  this is plumbing; about 2.9 MB per frame at the 49 152-vertex cap. **Built as 14 (G14f)**:
  the normal joined (`NVST` 14, `tagpu_native.c` — the native pass's own `emit_node`, not
  `tagpu_render3do.c`, which is the blit path's twin); ~~the world height has no reader until
  the shadow pass and joins with it (§5 step 5)~~ **the stream stays at 14 (decided
  2026-09-06)**: the lab bakes each caster's shadow-space position (world x, ground + scaled
  height, real z) into three floats, but the stream already carries world x, the *projected* z
  and the posed height, and real z = projected z + (altitude + height)/2 — `wz0 = fy − fz/2`
  and `o[9] = wz0 − z − y/2` in `emit_node` — so the same position is derived in the vertex
  shader from three per-unit uniforms (altitude, ground + air throw, the length rule's scale),
  set in the per-unit loops that already set `uWaterT`/`uDigT`. Nothing in `emit_node` or the
  buffer moves.
- **Nanoframes** go through the same unit shader (G13l), so they are lit and shadowed for
  free. Their band and fill colours are palette indices, so the restored-texture branch looks
  them up through the palette texture; and they cast nothing, as the native pass already
  rules **[MEASURED, build-state §7]**.

### 2.12 Shadows in the game: the decisions of 2026-09-06
Grilled with the owner before any code, one branch at a time; the ones above (§2.2, §2.4,
§2.7, §2.9, §2.10, §2.11) are amended in place. The rest:

- **Landing shape: one landing, two stages.** Units and wrecks cast first, A/B'd against the
  lab at `terrainshadow=0`; then the heightfield mesh, at `terrainshadow=1`. Both land together
  — one review, one docs pass, no intermediate `main` where hills receive and do not cast.
- **Sampler objects, so the context request goes 3.2 → 3.3 core** (§3's own advice): one depth
  texture read through a compare+bilinear sampler for the PCF taps and a raw one for the
  blocker search, on texture units 12 and 13 as the lab binds them. Every driver with 3.2 core
  has 3.3 (they shipped together), so the fork's legacy fallback is not reached. The two
  samplers stay bound and named even with shadows off — left at unit 0 they are two sampler
  *types* on one unit and GL drops the whole draw (the lab's blank afternoon).
- **Placement.** The frame's unit geometry is complete before the frame FBO is bound and the
  VBO upload sat after the terrain render; the upload moves up and the depth pass runs there,
  so the terrain read-back is this frame's, not last frame's.
- **What casts: the lab's rule.** Every gathered unit and wreck, cloaked or not; nanoframes
  excluded (§2.11); the FBI silhouette gates (`noshadow`, `canhover`, `floater`) ignored — a
  hovercraft over water does cast, and those bits are 1997 workarounds for the silhouette
  blit; effects models (missiles, shells, debris, in the same buffer) do **not** — the lab has
  none, and a shell's shadow flickering at sim rate under smooth bodies is noise. **Cloaked
  units cast exactly for whoever can see them**, the owner's condition: the gather drops a
  cloaked enemy before any vertex exists (`tagpu_native.c`, "enemies never see cloak") and
  keeps the watched player's own, so the buffer the depth pass draws *is* what the viewer sees.
- **The hill mesh**: indexed, one vertex per grid point, indices ordered by cell row, built in
  `tagpu_terr.c` beside the R8 upload from the same byte copy under the same key and retry; the
  depth pass draws the contiguous row span the light window covers plus the margin, so the
  biggest stock map costs what the smallest does. Two Continents: 539 k vertices, 3.2 M
  indices, under 20 MB. The cell's diagonal is the one `taTerrN` interpolates across.
- **Sub-passes off under the switch**: the per-unit stencil silhouette and the slant range are
  not emitted or drawn under Classic++ (air units under `airshadow=drop` excepted); the hires
  silhouette likewise (§2.4). **Amended 2026-09-09 (G18b)**: that is `shadows=0|1`. At
  `shadows=2` the switch emits and draws the pair — every unit, the replacement meshes
  included — and the depth map refuses instead; at `shadows=0` even the `airshadow=drop`
  aircraft loses its silhouette.
- **Modules**: `tagpu_shadow.c/h` owns the map, the samplers, the FBO, the light-space frame
  and texel rule, the depth program for the 3DO stream, the per-frame pass and the read-back
  uniforms, and joins the GL reset; the mesh is the terrain module's; the eight keys are parsed
  by `tagpu_classicpp.c` into the light struct; `shadowAt` is the second half of `taLambert` in
  `tagpu_glsl.h`, once, for the terrain and unit shaders. The hires pass gets its depth entry.
- **The receiver-plane derivatives are taken at the top of the fragment shader**, before any
  `discard`, and handed to `shadowAt` — G14g's mipped-sample lesson applied to `dFdx`. Since a
  face's normal is flat, `dFdx(p)` is exactly `½·M₃·dFdx(W)`, so the function's arithmetic is
  the lab's with the derivative supplied rather than taken.
- **What building it changed in the lab's shader, in both copies (2026-09-06).** The blocker
  search opened with eight ring taps 12–28 texels out and nothing nearer, so a caster the
  size of the commander's head — 30 texels across at this density — could hold the
  receiver's own texel and be missed by every tap: in the game the head's and gun's shadows
  cast nothing on every frame (520 changed px against the lab's 773 on the parity fixture)
  while the lab's view-anchored lattice happened to land a tap on the body. **The receiver's
  own texel now opens the search** (741 / 896), and **all 16 Poisson taps follow** rather than
  8, so the blocker-distance estimate — the penumbra's width — stops depending on which taps
  the lattice happens to put on the caster (961 / 918). `tascene-view.html` `LAB_LIGHT` and
  `tagpu_glsl.h` carry the same text; the Classic lane's md5s are untouched, `LAB_LIGHT` is
  the exploration lane's alone. Eight raw fetches more per lit fragment.
- **The bar for "verified by running it"**: G14f's within-a-level bar cannot hold on a
  map-anchored grid against a view-anchored one (the edges differ by up to a texel by
  construction), so: Classic `tascene ab` unchanged; Classic++'s shadow on-minus-off against the
  lab's `shadows=1` minus `shadows=0`, at `terrainshadow` 0 then 1, **shadowed-pixel counts
  within 5 %** (any change, and darker than 0.7× lit) with the two difference images looked at
  side by side; fps within 1 of G14g's on the four fixtures; one x11grab scroll under Classic++
  stepped frame by frame for crawl. The zoom-floor look is the owner's, in play.

---

## 3. What the code already settles — do not re-derive

- **No engine shadow to suppress.** Since G12c/G13k the native pass draws every unit and
  structure shadow itself; Classic++ skips its own two sub-passes. No byte patch is needed,
  so the landing review runs at `medium`. GAF-baked feature shadows stay, as in the lab.
- **The heightmap is already read.** `FeatureStruct+0x04` is the per-16-px-tile height byte,
  the grid is `*(main+0x14287)` with stride `0xD`; `tagpu_feat.c` reads four of them for every
  feature anchor **[SOURCE]**. The terrain shader already carries world x and z per vertex.
  **Done (G14f)**: `tagpu_terr.c` copies the byte of every cell into one R8 texture when it
  builds the atlas (once per map, unit 5), and the fragment shader takes the lab's normal at
  the four grid points of the 16-px cell under the fragment and interpolates them as the lab's
  two triangles per cell do — per fragment rather than per vertex because the lab's vertices
  are four sub-quads per tile, 4× the terrain stream and 29 MB a frame at the zoom floor, for
  the same field. Sixteen `texelFetch`es of a byte per fragment (twelve distinct texels); the
  parity fixture's frame rate did not move (59.7 fps during the restore against G14e's 59.4).
- **The definition tables are known.** Unit definitions: count `main+0x1438F`, table
  `main+0x1439B`, stride `0x249`; feature definitions: count `main+0x14253`, table
  `main+0x1426F`, stride `0x100` **[SOURCE `tagpu_cat.c`]**. The load-time atlas walk starts
  there; the path from a definition to its loaded model's faces is not yet written down (§4).
- ~~**The GL context is requested at 3.2 core** (`render_ogl.c` 189–192) and the field notes
  record `GL_VERSION 3.2.0 core` on the 4070. The native shaders are `#version 330 core` and
  compile, which is the NVIDIA driver being lenient, not a guarantee; sampler objects are a
  3.3 feature. **Bump the request to 3.3** before relying on either **[SOURCE, field notes]**.~~
  **Done in G14i**: the request is 3.3 core, `tagpu.log` reads `shadow: GL ready (GL_VERSION
  3.3.0 NVIDIA 595.84, max texture 32768)`, and the shadow map's two sampler objects run on it
  **[MEASURED 2026-09-06]**.
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
  §1's 4-texel pad and mips 0–2 anyway *(done in G14g: the unit atlas is a `TAGPU_GAFATLAS`
  with a 4-texel pad, 4-aligned cells, 2048 entries in 2048², recycled when full)*.

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
effects 2, units 3 (G14g) — re-picked at every batch boundary. Each `TAGPU_GAFATLAS` carries its twin and its
job; `tagpu_gaf_atlas_get` queues every new entry (tileability decided at upload, while the
pixels are still on the CPU), a recycle clears the twin and drops the queue, a context loss forgets
everything, and arming the switch mid-play queues what the atlas already holds. **The unit atlas
(G14g, 2026-09-05)** is the same object since `tagpu_render3do.c` ported onto it, with the layout
§2.5 decided: every frame's cell carries a **4-texel replicated border** and is **4-aligned** in
origin and size (`tagpu_gaf.h` `pad`/`align`), so the twin can be **mipmapped to level 2**
without a level-≤2 texel that touches a frame holding another frame's texels — the lab's
`UNIT_PAD` rule, layout for layout. The twin is `GL_LINEAR_MIPMAP_LINEAR`/`GL_LINEAR`,
`MAX_LEVEL 2`, 4× anisotropic when `GL_TEXTURE_MAX_ANISOTROPY_EXT` takes (the driver's error flag
says, and the log says which), and its two mip levels are regenerated with `glGenerateMipmap`
**one frame after every batch the restorer paints** (the OUT draw is issued after the gathers, the
atlas compares the job's painted count on the next frame's gather), once when the twin is made
(so it is never sampled incomplete — an incomplete texture reads opaque black, which the alpha
test would take for a restored texel) and after a recycle (the restorer clears only level 0).
The cell's slack past the border, where the alignment rounds up (0–3 texels on the right and
bottom), is filled with the replicated edge as well — in the R8 by the upload, in the twin by
the OUT pass (`padR`/`padB` on the restorer's frame): the review showed that at level 2 the
far-edge sample of a frame whose width or height is 3 mod 4 takes a quarter of its weight from
the level-2 texel that covers the slack, which unwritten would darken that column by a
sixteenth (no stock unit texture seen so far has such a size — 104 entries across the fixture's
and `200v200`'s atlases, all multiples of 4 — and the lab's pack leaves its slack at zero, so
the lab has the flaw where the game no longer does). Classic's R8 atlas
gets the same cell layout and the same 4-texel border, sampled `NEAREST` on the frame's own
texels as before, so its pixels do not move — `tascene ab` measured it. The unit FS samples the
twin **before the colour-key discard** (a mipmapped sample's implicit derivatives are only
defined while every fragment of the quad is still running; the R8's NEAREST sample never
cared), takes `t.rgb / t.a` where `t.a > 0.5` — the twin is `(0,0,0,0)` at a keyed texel, so a
bilinear sample beside one is premultiplied by its coverage, and the lab's `LAB_UNIT_FS`, which
takes `t.rgb` as is, draws a one-texel dark ring there that the game does not (unexercised on
the fixture: its unit textures have no keyed texel) — the palette's colour for a flat face, a
nanoframe band or a texel not yet restored, then the lambert (§2.11) and the grey rule (§2.6);
the colour-key hole stays the index compare, which is what keeps a keyed texel out of the depth
buffer. **The unit atlas's compressed frames** (`comp != 0`, none seen) still draw flat, as they
did before the port — whether the engine would texture them is not established, and Classic
must not move. Measured on the parity fixture: the twin dumped under `tagpu_restoredump.on` and
held to the pack's `units/atlas.rgba.bin` by `tascene unitdiff` — **25 of 25 entries found, far
band max 1 level on 2 of 116,736 bytes (0.0017 %), no keyed texels, the 4-texel ring an exact
copy of the edge on all 66,816 bytes** (the unit textures have no key on this fixture, so the
near band is empty). Measured, Two
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

- ~~**The restorer covers terrain, features and effects; unit textures are next.**~~ **Done
  2026-09-05 (G14g)**: the unit atlas is a `TAGPU_GAFATLAS` with the 4-texel pad, 4-aligned
  cells and a twin mipped to level 2, and the unit shader samples it (§4c, §5 step 2). Every
  atlas the game draws from is restored. What G14g did not close: the whole-frame residual
  below is not the units' (one commander on the fixture), and the atlas's entries are keyed on
  the frame header's address, its pixel pointer and its size — a later map that reuses all
  three for a different plane would draw the old texels, the same exposure the sprite atlases
  and the old unit atlas (keyed on the address alone) have. An in-process map change through
  the menu replaces the GL context (`tagpu: GL CONTEXT CHANGED`), so the atlas is rebuilt from
  nothing and the exposure does not arise there; a map change that kept the context would
  carry it.
- **The seabed under open water** — the lit look of §2.3 on a map with a sloped seabed
  (Coast to Coast) has not been judged by the owner; the shot is in §2.3.
- **The whole-frame Classic++ residual against the lab is unexplained in one number.** With
  every sun off on both sides, the game and the lab differ on 130,997 of 630,784 viewport
  pixels (mean 0.22 levels; 94,631 of them by one level, 18,564 by two), where Classic's
  residual is ~7,300. The keyed sprites' near band (§4c: 27 % of its bytes differ, by 0.41
  levels) on a viewport full of trees, the two units' restored-against-indexed texels, the
  chat and the cursor account for it **[INFERRED — not isolated]**; the lighting itself adds
  6 pixels past one level (§5 step 8).
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
- **Hires glb under Classic++** — ~~lighting model, depth pass, shadow read-back, silhouette
  shadow off; deferred by §2.4~~ **the depth pass and the silhouette are done (G14i, §2.4)**;
  the lighting model and the shadow read-back stay deferred — a replacement mesh casts and
  does not receive.
- **The soft edge and the ridge haze are lattice noise, on both sides** (G14i). The two
  lattices — the lab's view-anchored, the game's map-anchored — put texel centres in
  different places, so the PCF's bilinear compares round differently along every shadow edge
  and on every near-grazing slope: on the parity fixture the hills stage has 156 game-only
  pixels (11 of them darker than 5 %, 1 darker than 10 %) and 60 lab-only ones of the same
  kind at 0.988. Invisible, counted, not closed; a larger constant bias would trade it for
  peter-panning at the shadow's root.
- **A ground unit's caster sits on the height byte, not on the engine's own y** (G14i,
  `tagpu_native.c`): the engine interpolates the ground under a unit, the receiver is drawn
  from the byte, and a caster floating the difference above its receiver throws a shadow
  detached by that times cot el. Only an airborne unit has an altitude in the map. The body
  is still drawn at the engine's y.
- **A wreck's model height for the length rule is its posed top** (there is no unit record to
  reach the rest AABB through); a unit's is the whole-tree AABB at rest, cached per model
  (256 models, then the posed top).
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
   (Q6). ~~**Still to do: the unit atlas** — pad and align, mips 0–2, the unit shader's branch —
   after the unit-shading worktree lands, since both edit `tagpu_native.c`'s unit FS.~~ **Done
   2026-09-05 (G14g)**: `tagpu_render3do.c`'s atlas ported onto `TAGPU_GAFATLAS` with
   `pad`/`align`/`mip` (4, 4, 2), the twin regenerated per painted batch, the unit FS's restored
   branch on texture unit 8 (§4c "The queues"). No cache (§2.5b).
3. ~~**Unit shading** in map space: normal and world height per vertex, `LAB_LIGHT` into
   `tagpu_glsl.h` with the viewer's uniform names.~~ — done 2026-09-05 (G14f): the normal per
   vertex (§2.11), `TAGPU_GLSL_LIGHT_FN` with `uSun`/`uAmb`/`uNorm`, the LUT row skipped
   under the switch; the world height waits for step 5.
4. ~~**Terrain lighting** from the height texture, level-normalised.~~ — done 2026-09-05
   (G14f): the R8 height grid per map, the normal per fragment (§3), **level ground exactly
   1.0** — 0 of 162,828 flat terrain pixels moved between sun on and off, terrain alone, on
   the parity fixture; and the feature sprites take the ground's lambert at their anchor.
   The knobs are §2.10's `tagpu_classicpp.cfg` (`sun`, `unitsun`, `amb`; `sun=off` puts
   every sun out and, with the restore settled and no grey fog band in view, reproduces
   G14e's Classic++ pixels byte for byte outside the units — an unrestored texel in the grey
   band takes §2.6's RGB mean where G14e remapped its index). A map whose height grid cannot
   be read draws Classic++ *unlit*: the restored colour and the grey rule stay, the lambert
   is skipped, and the build is retried every 60 frames.
5. ~~**Shadows**: the map-anchored grid, the depth pass over units and the heightfield mesh,
   the read-back in terrain and unit shaders, the two Classic shadow sub-passes off.~~ **Done
   2026-09-06 (G14i)**, decided in full first (§2.12): `tagpu_shadow.c` (the map, the
   samplers on units 12/13, the frame and its octave rule, the depth program for the 3DO
   stream, the read-back uniforms), the hill mesh in `tagpu_terr.c`, the eight cfg keys in
   `tagpu_classicpp.c`, `taShadowAt` as the second half of `taLambert` in `tagpu_glsl.h`, the
   replacement meshes' depth entry in `tagpu_hires_draw.c`, the context at 3.3 core.
   **Measured** (parity fixture, eye 2320,720, the pack built `--undither`, the pointer parked
   off the commander — the scenario's `center_on` leaves it ON the anchor, and its crosshair
   covered the shadow's root in the first captures; the engine's health bar masked on both
   sides, 139 px): shadowed-pixel counts, game against the lab, **units only** (`terrainshadow
   0`) 961 / 918 changed (**1.047**), 628 / 615 darker than 0.7× (**1.021**); **hard**
   (`penumbra 0`) 796 / 773 (**1.030**), 629 / 623 (**1.010**); **with the hills** 1442 / 1346
   (**1.071**, the 156 haze px of §4), 632 / 619 (**1.021**). The caster's numbers are the
   lab's to the digit (`shadow: caster model=34 top=40.0 … gnd=96.0 sv=0.503`, the pack's
   mesh 40.0). The dumped map (`tagpu_shadowdump.on`) holds the commander in 496 texels
   against the lab's ~490 in its `debug=shadow` view, and the frame's matrix rows are the
   lab's basis to five digits. **Classic**: `tascene ab` 747 of 630,784 (the commander's box,
   the parked pointer's arrow, and 417 px within 2 levels along sprite edges — the chat lines
   had faded; and **against the G14g DLL's own Classic frame** — a second instance launched from the main checkout on the same fixture, eye and pointer — **0 of 630,784 pixels differ**, the whole frame, commander and all). **Frame rates** during the restore: parity 59.4 fps (G14g 59.6),
   feat-forest 53.3 (54.2), fx-mix 53.7 (54.0), 200v200 54.1 (53.9).
   **Hires**: on `hires-one` the replacement Peewee casts — a core at 0.64 of lit to its right with the map on, gone with it off — beside the 3DO AK's shadow. The zoom-floor look is the owner's, in play.
6. **Fog** rule of §2.6 — in the terrain, feature and unit passes since G14f (the RGB mean
   after the lambert, under the switch); the effects pass hides in grey and needs none.
7. **Switch and cfg**, then the **menu** of §2.10.
8. **Verify by running it**: `tascene ab` in Classic against `lane=classic` (nothing moved),
   in Classic++ against `lane=classicpp` (the port measured). Engine code triggers the Opus
   review at `medium` and the documentation pass; a human declares it ready.
   **G14f, measured 2026-09-05** (parity fixture, eye 2320,720, the lab pack built with
   `--undither`, shot with `lane=classicpp&shadows=0`): Classic `tascene ab` 7,602 of 630,784
   (chat lines that fade with time, the units, the cursor — the terrain and features
   bit-exact, and G14e's Classic++ frame is byte-identical to the new one at `sun=off`
   outside the chat and the commander). Classic++ against the lab: of the **55,469 pixels the
   sun changes in the game, 6 differ from the lab by more than one level and none by more than
   three** ([classicpp-light-ridge-game-lab.png](assets/shots/classicpp-light-ridge-game-lab.png):
   game, lab, difference ×8 over the ridge). The whole-frame residual with the suns off is the
   open item in §4. Frame rates during the restore: parity 59.7 fps, `feat-forest` 54.3,
   `fx-mix` 54.0, `200v200` 54.0 with 179 units on screen — G14e's figures. `200v200` a
   minute into the fight (255 units, 50 wrecks, the vertex cap hit) logs 6 sixty-frame lines
   in 30 s on **every** combination of the G14e and G14f DLLs with the switch on or off, so
   that is the scenario, not the lighting; the G12f "28 lines in 30 s" was 217 units at
   sim +3 and was not re-established.

The handoff that preceded this page, and the 22 lab commits it rests on, are on the
`worktree-gpu_render` branch; the lab commits land first as one finished unit.
