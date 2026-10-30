# The unit renderer

The geometry bake, the posed program, interpolation, the unit cap, draw order, culling, quad mapping and structure shading.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.10 The per-type geometry bake (`tagpu_posebake.c`, OFF by default, `tagpu_posebake.on`) — G16 step 4

**It draws nothing.** This is the data half of [GPU posing](../gpu-posing.html) — the step that turns
a `Model3DONode` template into the two static vertex buffers a posed shader will draw a unit from,
so that the emitters above stop reading `prim+0x22` and §2.9's whole apparatus can be deleted. The
shader is step 5; until then the module bakes, caches, invalidates and **checks itself**, and with
the trigger absent it costs one `GetFileAttributesA` every 30 frames and the `native:` line is
byte-identical to what it was.

| | |
|---|---|
| **the geometry buffer**, per type | rest position, the mean normal of the vertex, the piece index, a flags word and a quad's four corner normals as twelve SNORM16 (§2.106) — 14 floats — with body triangles, slant triangles and wire lines laid down as three ranges of one buffer. Keyed on primitive 0's node pointer plus the level and GL generations |
| **the material stream**, per (type, owner, atlas generation) | UV, flat colour, colour key and a **skip** flag — 5 floats. The two buffers always hold the **same** vertex count: a face the engine paints nothing for is baked and collapsed by its flag rather than dropped, which is what lets either be rebuilt without the other |
| **the topology**, per type | the parent array and the accumulated rest offsets, so `pose_accum_body`'s per-unit sibling scan stops being a per-frame cost. Cached now, consumed in step 5 |
| **invalidated by** | the level generation (the frame packet's `level_gen`), the GL generation, the **atlas** generation — new, `TAGPU_GAFATLAS.gen`, bumped by every `atlas_reset` and `atlas_lost` because every UV moves — and the owner, which is in the key |

**Fields we write: none.** Every engine read is the one the emitters already make.

**What it measured on the pose inventory** (67 distinct types, four camera stops, `posebake.on=log
check`): **0 check failures** on either of the two things the lever compares — the body vertex
count `emit_geom` produced against the bake's own prediction, and the accumulated rest offsets
against `pose_accum_body`'s per-unit walk — with `anom=0` and `refused=0`. The two ordinary
findings (`odd=` faces outside `3 ≤ fvc ≤ 32`, `nomat=` faces with neither texture nor colour) are
content, not faults; [GPU posing §3](../gpu-posing.html) carries the correction that says so. The GL
invalidation was watched on an exit to the shell: `posebake: dropped 53 geometry (taking 53
material with them) … GL 2`. *[The 27 this first quoted was an earlier run of the same test, before
the drop line reported the cascade separately; both are real, but only one is the shipped build.]*

### 2.11 The posed program (`tagpu_posedraw.c`) — THE unit renderer, G16 steps 5-8

**There is no lever and no alternative.** `tagpu_posedraw.on` was a measurement lever while the CPU
emitters were Gate B's oracle; step 8 deleted them, so this pass draws every unit or the unit is
not drawn. A unit is one `glDrawArrays` out of its type's geometry and material VBOs with its whole
pose in a uniform block; no vertices are built for it on the CPU at all. That covers **all three
baked ranges** — the body, the structure-shadow slant and the nanoframe wireframe — which was every
reader of `prim+0x22` except the selection lines and the effects models. Neither is on a shared
stream: the selection rect is the marker pass's (§2.84) and the effects models are rasterised from
the frame packet (§2.89), so `tagpu_native.c` builds no vertices at all.

| | |
|---|---|
| **the program** | a TWIN of the native one: its own vertex stage (the port of `emit_node` — piece transform, `sx = ax + x` / `sy = ay + (-z - y/2)`, the depth key, the world x/z the fog samples, the model height the waterline clips on, and the shade quantised off the baked rest normal), and the native pass's **own** fragment stage rather than a copy — through `tagpu_native_unit_fs()` on GL; on Vulkan (and since 2026-09-23, when that accessor went) through `tools/spirv-gen.py`'s `pose_unit` row, which pairs `tagpu_posedraw::VS` with `tagpu_native::FS` |
| **the shadow-depth twin** | the same vertex shader with an empty fragment shader and `uDepthPass = 1`, mirroring `tagpu_shadow.c`'s `VS_U`/`FS_NONE`, so a colour-keyed texel casts on both paths. (`VS_U` and its `shadow_unit` program are gone since 2026-09-23: nothing on the Vulkan lane drew them) |
| **the Classic silhouette** | routed through the posed program too — it reuses the body geometry, so a posed unit would otherwise lose its shadow whenever Classic++ is off |
| **who gates it** | `shadows=`, not the Classic++ master arm and not `assets`/`light` (G18a/G18b, merged in 2026-09-09). Both posed shadow loops in `tagpu_native.c` skip on `cpp && !hard`, so **Classic++ at `shadows=2` (HARD) draws the Classic pair through this program** and `tagpu_shadow_begin` then refuses the depth pass outright (it requires `TAGPU_SHADOWS_SOFT`, `tagpu_shadow.c:368`). At `shadows=0` nothing is drawn, an aircraft's `airshadow=drop` included. This gating was written against the CPU emitters G16 step 8 deleted and was **re-expressed**, not merged |
| **the ground does NOT cast** (2026-09-09) | **Since 2026-09-23 nothing builds a ground caster at all — `build_hills` and `tagpu_terr_hills_draw` are deleted and `terrainshadow=` is parsed and logged only.** The record: `terrainshadow` now defaults to **0** (`tagpu_classicpp.c` `shadow_defaults`), so `tagpu_shadow.c:398` returns before `tagpu_terr_hills_draw` and the heightfield contributes nothing to the depth map. Units are unaffected — this is the caster's own gate, not the shared bias. The ground casting on itself darkened open water in the caster's own 16-unit lattice and got worse as `Shadow quality` went up; seven candidate fixes were swept and costed and every one removed the artifact and the terrain-shadow feature together, because a cell's own relief *is* the terrain shadow ([renderers](../renderers.html) §2.7b). `terrainshadow=1` in the cfg still turns it on and is the fixture the eventual fix is measured in; the render-options screen has no row for the key and never writes it, so nothing a player can click re-enables it. The hills mesh is still BUILT at map load (6.4 MB + 12.9 MB on Two Continents) — it is simply never drawn, which is an **open TODO**: `build_hills` is unconditional where the draw is gated, and the fix is a lazy build on first draw rather than gating the build (the flag is live, and `terrainshadow=1` mid-session is the fixture the shadow fix is measured in). See [roadmap](../roadmap.html) |
| **the three ranges** (step 6) | `uRange` selects. **BODY** as above. **SLANT** takes `0x45A610`'s projection `(x + y/4, −z − y/4)` off the posed vertex snapped to whole units, the neutral SHD row, `waterT`/`digT` pinned at −1e9 by the pass itself (the structure branch never erases — the G14j fix), and its own per-piece rule `(P_FLAGS & 3) == 3`. **WIRE** is `GL_LINES` on the body projection, one notch nearer (+0.15), the nanoframe's animated blue from a uniform because it is per unit while the material stream is per type and owner |
| **the 16.16 snap** (step 6) | the posed vertex is rounded onto the engine's own grid — `floor(m·65536 + 0.5)/65536` — **before anything reads it**, in every range. The engine holds each posed vertex as three 16.16 integers and every CPU emitter reads them back as `v[i]/65536.0f`, so a float compose that stops short sits up to half an LSB off a value that is exactly representable; `recon_prim` rounds the same way. This is what makes the slant portable at all (its `>>16` is a FLOOR, so half an LSB is a whole screen unit) and it took the body's residual to zero as well |
| **the pose** | a std140 block, `vec4 uRow[3*256]` + two packed per-piece words, `uPieceFlag[64]` (shaded) and `uPieceVis[64]` (0 not drawn / 1 drawn / 3 drawn and casting) = **14 336 bytes**. `GL_MAX_UNIFORM_BLOCK_SIZE` is read at build time and the pass **refuses to arm** below that. Since step 8 there is no CPU emitter to leave those units to, so the refusal is *published* and `owndraw` stops skipping the engine's own unit rasterise — see "when it cannot arm" below. The wire reads the same word rather than the all-zero matrix: a zero-area triangle provably produces no fragments, a zero-length LINE is not promised away, and one bright pixel per hidden edge would land on the unit's origin |
| **the topology** | consumed from the bake entry's `parent[]` — `pose_accum_body`'s per-unit sibling scan is gone from the posed path, which is what §2.10 cached it for |
| **the VAO** | one per material stream, built at bake time, binding the geometry buffer (locations 0-3) and the material stream (4-6) together, so a draw is one bind |
| **the model top** | `s_emitTop` is gone; the top was each piece's baked BODY-range rest AABB through its pose matrix (`tagpu_posedraw_top`), an over-estimate that fed wrecks only. **Deleted 2026-09-23, with the AABB the bake kept for it (`pmn`/`pmx`/`pbody`): nothing called it once the Vulkan lane took the units** |

**Fields we write: none.** Every engine read is one the emitters used to make; the pass adds GL
objects and no engine state.

**Two uniforms this program does NOT hold what `tagpu_native.c`'s holds** [FOUND 2026-09-15 by the
Vulkan port, which had to reproduce them]. Both are facts about the shipped GL renderer, recorded
because a port that "fixed" either would draw a different picture from the game:

| | |
|---|---|
| `uLambert` | **never set at all.** `tagpu_posedraw.c` does not look the uniform up and never writes it, so it keeps a freshly linked program's **0** for the pass's life — and the Classic++ lambert therefore lights a unit from a **flat up normal** (`vec3(0,1,0)`) rather than from `vNrm`. `tagpu_native.c:4010` sets it from `tagpu_classicpp_lit()` on ITS program. Whether that is deliberate has not been established; what is established is that it is what the game draws |
| `uRestored` | (GL record; `tagpu_r3d_atlas_rgbref` is gone since 2026-09-23 — the Vulkan unit pass takes `restored` from its hand-over) set from `tagpu_r3d_atlas_rgbref() && tagpu_classicpp_ON()` here, and from `… && tagpu_classicpp_ASSETS()` on the native program. The two disagree whenever the master arm is on and `assets=0` |

A third is not an asymmetry but is the same kind of trap: **`uNanoT` and `uNanoC` are STICKY**. The
pass writes them only on a unit with `nanoOn`, so a unit without one is drawn against whatever the
last unit that had one left in the program. Nothing reads them on a `uNanoOn == 0` unit, which is
what makes that safe — and it is a statement about the fragment shader, not about the upload.

**The three degradations, and why none of them is a fallback** (step 8; the full ledger with its
invariants is [GPU posing](../gpu-posing.html) §4). There is one renderer, so every condition that
used to fall through to `emit_geom` now degrades *inside* the unit. All three ride the `native:`
line beside `posed=` and are printed **only when they have caught something**, because in a healthy
game none of them ever does:

| field | what happened | what the player sees |
|---|---|---|
| `rest=` | the frame's pose arena was full | that unit drawn **at rest** for the frame — right geometry, material, position, fog, shadow and depth, only its animation frozen. The block it takes is one shared static set of identities, so the degradation allocates nothing and cannot itself fail |
| `unpl=` | a piece's node did not read, or its parent link never resolved | that **piece** at rest inside a unit that is otherwise posed — `hires_pose`'s answer for the same condition |
| `nobake=` | the type would not bake: over `TAGPU_PBMAXPIECE` (256) pieces or past `PB_MAXVERT` (49152) vertices | **nothing drawn for that unit.** The one honest drop, and it is counted whether or not `posebake.on` is armed — without a count an undrawable model is a unit missing from the screen with nothing in the log. Stock's worst model is 36 pieces and 574 vertices |

`q=` is the fourth and is not a degradation: it prints the units the gather **queued** against the
units the pass **drew**, only when they disagree, because since step 8 a queued unit the draw
dropped is a unit missing from the screen. It came out of chasing a `posed=` that read lower than
the unit count, which turned out to be the **hires** pass taking those units — not a miss.

⚠ **The arena degradation looks exactly like the artifact §2.9 exists to describe** — a unit at its
unrotated rest orientation. That is not a coincidence and it is the reason it is counted: it is the
same picture, but bounded, deliberate and visible in the log, rather than a race. Forced with a
48-slot arena it reads `rest=6900` with the **same unit and triangle counts** as the healthy build:
every unit still drawn, nothing dropped.

**When it cannot arm at all** (step 8, decision B). `owndraw`'s detours skip the engine's own unit
rasterisers and are installed at DLL attach, so with no CPU emitter left "draw nothing" would be
the default failure — every unit in the game invisible. So `tagpu_owndraw_classify` **asks before
it skips**: it reads a readiness word this pass publishes, and hands the draw back to the engine
(8bpp, composited through the terrain key) when the pass is not live. That read crosses threads and
is safe by **direction**, not by timing: the word is one aligned `int`, written only by the render
thread, set to "live" only after the programs have linked, and cleared by `tagpu_posedraw_glreset()`
*before* a new context is used. A stale "not ready" costs a one-frame double draw; a stale "ready"
is the unsafe direction and no write order produces it. The log line fires only once the pass has
**tried and failed**, not during the ordinary first frames before the render thread has built
anything.

**What `MAXNV` stopping applying to units actually looks like.** The claim used to be a design
note; `scenarios/crowd-static.json` (256 units of 16 types, one owner, no orders — the large scene
that stays identical between runs, so unlike a battle it can be diffed) makes it a picture. On it
the CPU emitters log `49152 verts VERTEX-BUDGET-HIT` and **the bottom rows of the block are health
bars with no models**: the shared stream ran out mid-gather and the units after it got empty
ranges. The posed path draws all 256 — `posed=240/32288tri slant=96/13120tri`, plus 16 hires
ARMPWs, and 96 slant is the 6 structure types x 16 — because per-type GL buffers have no shared
budget to exhaust. It wanted ~136k vertices where the stream holds 49 152.

**The frame time, at last** *[MEASURED 2026-09-09; it needed `tacli --maxfps 0`, since both paths
had been reading 58.5 fps because both hit the cap]*. 200v200 at 1920x1080, sim paused, 281 units
and 76 wrecks on screen: the CPU emitters **184.0 / 180.0 fps** against this pass's **313.0 /
306.9** — **1.70x**, and the CPU side gets that while truncating.

**The residual this pass leaves.** It reads the pose fields on the render thread with no interlock,
so a unit can be drawn with its pieces mixed across one tick boundary — strictly smaller than what
§2.9 describes (there, the engine's buffer lagged the fields by a whole tick for the entire unit),
and it cannot produce the rest pose, because nothing it reads is ever `rep movs`'d from a rest
array. Gate C looked for it over 62 s under scheduling pressure and found nothing visible
([GPU posing](../gpu-posing.html) §4 step 7). The anchor is the other half and is still unchecked: the
unit's 16.16 position is three dwords read without an interlock.

**What it measured**, 1024x768, `ss=2`, the sim paused, one build, twelve scenes —
[GPU posing](../gpu-posing.html) §4 step 6 has the full table. With `poserecon.on` on **both** sides,
which is Gate B's protocol and isolates the port: **0 differing pixels of 786 432 on eleven of
twelve scenes and 1 on the twelfth**, across four camera stops of `pose-inventory`, `200v200` at
128 posed units and 13 219 triangles, a six-nanoframe wire sweep, and all five G14j shadow
fixtures. Against the engine's own posed buffer the worst scene is 43 pixels (`shadow-struct`) —
but **that column is the reconstruction, not the port**: running the CPU emitter against itself
(`poserecon.on` vs off, the posed pass out of the picture) reproduces it row for row, and it is in
the shipping build today because the guard falls back to the reconstruction on a torn read. Every
differing pixel in the table is isolated; nothing is the "whole face one SHD row off" tell that
would mean the shade quantisation is wrong. **Gate B and Gate D both PASSED.**

*[The step-5 numbers this paragraph used to carry — 2 pixels and 1 pixel — were the shader not
snapping the posed vertex onto the engine's 16.16 grid. Step 6 does, for every range, and the same
fixtures read 0.]*

**A trap this cost a crash to find.** `opengl_utils`' global `glGetIntegerv` is **NULL**:
`wglGetProcAddress` returns NULL for GL 1.1 core entry points under wine, which is why
`opengl_utils` guards its own use of it and why `tagpu_terr.c`, `tagpu_shadow.c` and
`tagpu_restoreglsl.c` each load their own through a `getgl()` that falls back to `opengl32.dll`.
Calling the global one is a jump to address 0 — `ErrorLog.txt` reads `Access Violation … at
0023:00000000`, during map load, with nothing in `tagpu.log` because the module dies before its
first line.

### 2.13 Sub-tick pose interpolation (`tagpu_lerp.c`, OFF by default, `tagpu_lerp.on`) — 2026-09-09

The design, the invariants and the gate results are [smooth motion](../smooth-motion.html); this is
the hook-map entry. **No engine address, no patch, no engine write.**

`posed_pose` composes each piece's 4×3 from exactly `pr+P_POS` (i32[3] 16.16) and `pr+P_TURN`
(u16[3] TAang). With the lever armed, those two reads are served from a blend of the unit's last
two **sim-tick** snapshots instead of the live fields, so the model animates at render rate rather
than snapping at the tick. It is only this small because G16 turned the pose back into per-piece
*fields*; before it, the pose arrived already baked into vertices.

- **Read-only.** The history arena is ours (§2.5). Nothing is written back to `PrimitiveStruct` —
  the sim reads those fields and TA has no runtime desync detection, so a per-machine
  framerate-dependent blend written there would diverge two peers silently.
- **The degradation is a refusal, not a weight.** No history, a stale pair, a model over 48 pieces,
  a full table, or a weight that has reached 1.0 — every one returns 0 and the caller reads the
  live fields with its own untouched code. Not "blend at weight 1.0": `a + (b - a) * 1.0f` is not
  `b` in floating point, and the parity claim would have been false.
- **The tick period is measured, not assumed.** `main+0x38A47` advances `3 × GameSpeed` a second
  and a skirmish starts at `GameSpeed` **20**, i.e. 60 ticks a second — [engine
  map](../exe-reverse-engineering.html) §"The simulation clock". The module learns the period from the
  observed interval divided by the tick gap, so it is right at any speed and follows a live change
  (`p=33.2ms` at GameSpeed 10 against a predicted 33.3). **On pause it degrades by the same
  refusal**: no new tick arrives, the weight passes 1.0, and every unit draws from the live fields.
- **Not smoothed, deliberately**: `pose_accum_body` — the path `hires_pose` and `tagpu_posedump.on`
  take — is untouched, so replacement meshes still step and the sim oracle still reports the
  engine's own fields. The unit's world position and `O3_BTURN` are out of scope, which means the
  pose runs a sample behind a position that is not delayed.
- **Cost**: the first cut's `double` blend measured `+0.560 ms a frame at 240 posed units` — 2.33 µs
  a unit, 3.4 % of a 60 fps budget, on the paired `crowd-static` fixture
  ([smooth motion](../smooth-motion.html) §7f). **The shipped blend is 16.16 fixed point and that
  number no longer describes it**: the loop now carries no x87 at all, where the float form carried
  13 instructions including 4 `fldcw` (§7i, established from the compiler). Its frame cost is
  **not re-measured** — an interleaved live A/B put every off/on pair below 0.560 ms but spread
  0.068–0.461 ms under load from three other instances, which is not a number. §7i says how to
  close it.
- **The bound on the weight is clamped, not argued.** `w16` is forced into `[0, 65535]` because the
  turn multiply has only 32767 of headroom (`t` reaches +32768; `32768 × 65535` is 2 147 450 880
  against `INT_MAX` 2 147 483 647). The `[0,1)` refusal above already implies it today; the clamp is
  what keeps a later change to that refusal from becoming silent signed overflow.
- **Known residual, open**: the `(o3, nparts, gen)` key does not separate two units of the **same
  type** landing on the same reused `Object3do`, so a freshly built unit drawn within `LERP_MAXGAP`
  of a dead one's last sample can blend one frame from the dead unit's stance. Cosmetic and bounded
  — every index stays inside the same 48-piece block — but not closed; closing it wants a stable
  per-unit identity (`unit+0xA8`) rather than the allocation address. Found by the landing review.
- `lerp=<blended>/<snapped> p=<ms> u=<weight>` rode the `native:` line, and **nothing at all** was
  printed when the lever was off. **Gone since 2026-09-23**: nothing called `tagpu_lerp_stats` any
  more, and it was deleted.

### 2.86 The unit cap goes — one pose buffer a frame, and every cap sized for the design point

**What was wrong.** Past **512 posed units on screen, no unit body drew at all**. The Vulkan unit
pass bound one 14 336-byte uniform window per unit per frame slot — the 256-piece ceiling, whatever
the model — so the hand-over was capped at `TAGPU_PD_MAXHAND` 512 to keep that affordable, and a
frame over the cap was refused whole (§2.84 found it on `500v500`). Behind it sat a row of other
fixed caps that scale with the unit count, each sized for stock's 500 a player.

**The design point.** The engine's unit array has `10 × UnitLimit + 1` slots; retail clamps
`UnitLimit` to 500 (`0x491658`, [deep-tadr](../deep-tadr.html)), and the raised limits make it 1500
(§2.6b), so **15 001**. That number is `TAGPU_PK_DESIGN_SLOTS` in `tagpu_packet.h`, and every cap
below is either grown to the frame's own count or fixed at a size asserted against it at compile
time. The fixed sizes below are the 15 001-slot ones.

**The pose is one storage buffer a frame.** The hand-over already kept the poses in three arenas
(`rows`, `flags`, `vis`) with per-record offsets; the Vulkan pass now copies the three whole into
one `readonly` storage buffer per frame slot, laid out as `tagpu_posebake.h` states, and each
unit's uniform block carries three base indices into it (`uRowBase`, `uFlagBase`, `uVisBase`,
`VGL_SZ` 176 → 192). A unit costs what its model has: 2 016 bytes at stock's worst, 36 pieces.
`tools/spirv-gen.py` accepts `layout(std430) readonly buffer` blocks for it.

| bound | where | what it holds |
|---|---|---|
| every `uPose` index clamped to the bound range | the posed VS | no vertex reads past the buffer, whatever it is handed; on data that passed the two below, a no-op |
| each unit's slices inside the copied arenas | `tagpu_vk_unit_upload` | a unit that fails is not drawn and the every-unit-or-none gate refuses the frame |
| the frame's packed size ≤ `maxStorageBufferRange` | the same, every frame | the limit is a 32-bit field, so every base index stays under 2^28 vec4 and the `int` bases cannot overflow |
| one unit at the piece ceiling bindable | `tagpu_posedraw_ready` | 14 336 bytes; the spec's 128 MB floor means it cannot refuse a conformant device |
| `TAGPU_PD_MAXHAND` = units table + wrecks + 1 + builds (30 721) | `tagpu_posedraw.h`, asserted in `tagpu_posedraw.c` | every record the producer can make, so no frame the packet carries is refused for count; nothing is sized from it |
| a packet truncated in its unit, piece or wreck table | `tagpu_native.c` → `tagpu_posedraw_uncarried` | refuses the unit hand-over whole: a frame missing units is a different frame |

**Every other cap, and how it scales.**

| cap | was | now | thread | why that is safe |
|---|---|---|---|---|
| native gather `units[]`, posed list `pdu[]` | `MAXU` 2048, static | grown to `n_units + n_wrecks` | render | one owner, grown before any pointer is taken; a failed grow refuses the hand-over |
| native pose arena | `MAXU × 48` pieces | grown to the gathered pieces (exact: the bake keys on `nparts`) | render | same; a failed grow draws units at rest, logged once |
| sub-pixel table `s_spx` | 8 192 slots | `TAGPU_PK_MAX_UNITS` 16 384 | render | static, asserted ≥ the design point |
| marker bars, selection rects | 2 048 each | grown to `n_units` | render | grown at the top of `tagpu_mark_gather`, before anything emits; the cursor rects stay static and the two blocks are pushed back to back, so vertex indices are unchanged |
| order-marker buckets | 12 000 / 12 000 / 4 800 verts | 24 000 / 24 000 / 9 600 | render | static; overflow counted |
| order arena `MAXORD` / `MAXWALK` | 2 048 / 8 192 | 6 144 / 24 576 | game fills, render copies | **fixed, never reallocated** — two threads; asserted ≥ 4 records and 16 visited nodes per design-point unit |
| packet builds table | 2 048 | 6 144 | game | asserted ≥ `MAXORD` |
| bake caches `PB_MAXGEOM` / `PB_MAXMAT` | 128 / 256, then 512 / 1 024 | 30 721 each, the frame's bound (`TAGPU_PB_FRAMEMAX`: units, wrecks, build sites and the cursor ghost), recycling from 512 / 1 024, so a level holds its busiest frame's entries | render | a slot is never taken from an entry this frame has asked for, and a frame asks for at most the bound, so an entry used earlier in the frame cannot be evicted by a later one; the lookups are hash chains; a model's topology is sized by its own pieces ([A′1](../tadr-port/content-ids.md)) |
| Vulkan vertex-buffer table `VB_MAX` | 512, then 1 536 | the bake caches' sum (61 442), recycling from 1 536 and kept across levels, a hash chain on the serial; the retire grows | render | the same rule as the bake's: never an entry this frame drew, and a retire that cannot take an eviction grows the table instead of refusing |
| packet slot reserve `PK_RESERVE` | 8 MB | 20 MB | game commits, render reads | asserted ≥ the unit-scaled tables at the design point, units at 36 pieces a model, the wreck table at stock's worst wreck model, 19 pieces, and the map-feature table at its 65 536 entries (§2.90) — 20.1 MB — plus a 512 KB allowance for the fog grids and the shade table the publisher lays down ahead of them (359 KB on a 3840×2160 screen), 20 656 484 of 20 971 520 B together; the effects models' five tables are not in the design point — each is bounded at the acquire's row cap and all five at the reserve (`fx_fits`), and they are laid down last and cut together (§2.89); address space, committed as used — five slots of it in a 32-bit process whose largest free block an instance's `vk: VA` line has logged as low as 43.6 MB |
| packet wreck table `TAGPU_PK_MAX_WRECKS` | 4 096 | the wreck pool, `TAGPU_LIM_WRECKS` (8 192; 2 048 in the stock build) | game | a wreck in the table is a cell's record and a record belongs to one cell, so the table cannot truncate |
| packet `n_units` / `n_wrecks` | unchecked against the tables | validated at acquire | render | the render arrays are sized from them |
| reclaim ring | 4 096 | 32 768 | game | a design-point level's objects fit on the timed-out teardown path; a full ring leaks, never frees |

**Measured 2026-09-23**, 1024×768, `ss = 2`, branch against `main` built clean from one tree:

| measurement | result |
|---|---|
| `selbox-facings`, `tagpu_posedraw.ab`, runs interleaved main/branch/main/branch | **0 px of 3 145 728 in every pairing**, 8 465 ink pixels each side, one md5 across all four |
| `pose-inventory` at the structures stop, the same four runs | cross-build 14 137–27 961 px against a same-build floor of 21 536–23 207: idle COB animation, every pose class drawn on both (checked by eye) |
| `500v500` at zoom 0.25, `main` | `posedraw: more posed units on screen than the Vulkan hand-over carries`, census `unit=0` |
| `500v500` at zoom 0.25, branch | census `unit=1`, **610 posed units drawn**, `ftime: vk p50 0.355 ms p99 0.405 ms` |
| the same under `tagpu_grow.stress` | census `unit=1`, 604 posed, p50 0.361 ms, no crash |
| `ball10` at zoom 0.25, branch | census `unit=1`, 560 posed, 1 747 units alive, no crash |

`tagpu_grow.stress` (read at attach, `tagpu_packet.h`) moves the arrays this sizes every frame:
the native gather's and the marker pass's are freed and reallocated at exactly the size asked
for, the posed hand-over's arenas grow to the exact size on each append, and the unit pass
rebuilds its two slot buffers. The other growable arrays (the unit pass's draw list and staging,
the marker hand-over) keep their ordinary doubling and are not covered by it. A pointer that
outlives a move then reads freed memory at stock unit counts.

**What the design point costs in address space, if a frame reaches it.** 15 001 units posed and
on screen at 36 pieces: the unit pass's pose buffer is 30.2 MB and its uniform blocks 23.0 MB
(1 536 bytes a unit, §2.88) **per frame slot** (four on the reference setup), and the native pose arena 25.9 MB. The frame
packet reserves 20 MB × 5 slots = 100 MB up front, committed only as packets grow. A stock game
pays what it draws.

**Not covered.**
- **A full design-point frame has not run.** Tier 1 (§2.6b) ran the 15 001-slot array with 5983
  units alive, four players' worth; ten players at 1500 is the TADR port's landing 5.
- **Frame time at 10 000 units is unknown.** The bake's per-unit lookup scans its caches linearly
  and probes every piece's node with `IsBadReadPtr`; at ~600 posed units neither shows (0.36 ms),
  and at 10 000 either may. Instancing is the next step if it does.
- ~~A packet that truncates while its slot grows at load (`trunc=5` over `grow=17` on
  `500v500`)~~ **closed**: a fill that does not fit is grown and filled again before it is
  published (§2.16, the primitive; §2.3e, the level-start bare frame), so `trunc` reads 0 inside
  the reserve.
- Type-scaled caps are unchanged: the selection-box cache (256 root nodes a level) and the
  scenario harness's selection (`SCN_MAX_SEL` 512); its unit, order and clear arrays follow the
  unit limit (§2.6b).

### 2.87 Unit bodies before the effects, and cargo sorted by the merge's height

**What was wrong.** Two depth faults in the unit pass, both reported from play.

- **Every unit body drew after the effects.** `tagpu_vk_unit.c` `record_stage` picks each
  stage's draws with `(q->ghost != 0) != (stage == RS_GHOST)`; it read `!q->ghost != (stage ==
  RS_GHOST)`, which is the negated test, so ordinary units were skipped in `RS_BODY` and drawn in
  `RS_GHOST` — the stage `tagpu_vk.c` `world_records` runs after `tagpu_vk_fx_record`. The depth
  keys were right, so no key could fix it: every particle, projectile and explosion under a unit
  body was painted over. It showed first as the nanolathe spray vanishing over a factory's pad and
  the nanoframe on it (layer 6, `fxKey − 2`), most visibly zoomed in. MEASURED on
  `nano-factory` at zoom 2.5 with only the ARMAP drawn natively: spray-palette pixels over the pad
  were **0 in every frame** at any layer-6 key up to 1000, and 550–1200 a frame after the fix.
- **A unit carried by a transport drew over it.** The cargo loop gives a chain member its
  parent's row and band and lets the two sort by `md`; a carried commander's origin is 40 height
  units under its ARMATLAS, which `md` did not know. The engine merges cargo by height through
  `0x4B90A0` with `dbias = hi(Δalt)` over planes based at 0x32 / 0x7D
  ([exe-reverse-engineering](../exe-reverse-engineering.html), the cargo-loop section). Now
  `cargo_md_bias` gives the cargo `2·(baseC − baseP + Δalt)/256`, and the posed vertex stage adds
  it as **`uMdBias`** (std140 offset 68, after `uEnc`) **inside** `md`'s ±1.8 clamp, so no bias
  carries a vertex out of its row's band. MEASURED on `transport-cargo`: bias −0.31, the Atlas
  covers the commander with the bias and the commander draws over it with a live A/B that zeroed
  it; ARMATLAS cargo on an ARMAP pad reads bias 0.000.

**Fixtures.** `scenarios/nano-factory.json` (a plant, a guarding commander) and
`scenarios/transport-cargo.json` (an Atlas and a commander to load).

**Not covered.**
- The cargo bias matches the merge's height term only: `md`'s z term has no counterpart in
  `0x4B90A0`, so a cargo far north or south of its parent's origin still sorts by `md`.
- Cargo of cargo takes its immediate parent's bias plus that parent's own, in gather order; the
  engine forbids nesting (`0x48AB70`), so this is not reached in stock play.

### 2.98 Unit faces seen from behind are not drawn (`tagpu_vk_unit.c` `build_body_pipeline`, `build_shadow_pipelines`)

**What it is.** The unit body pipeline `s_pipeBody` and the two Classic hard-shadow pipelines
(`s_pipeShMark`, `s_pipeShDraw`) cull back faces: `VK_CULL_MODE_BACK_BIT`, front face
`VK_FRONT_FACE_CLOCKWISE`. No hook, no engine read or write, no shader change.

**Why the engine is the rule.** Every rasteriser the unit bakes use fills a row only when the
chain it walked forwards lies right of the one it walked backwards, so a face that does not run
clockwise on the engine's y-down screen paints nothing ([engine map](../exe-reverse-engineering.html),
*The rasterisers paint only faces that run clockwise on screen*). Stock models rely on it: 209 of
608 close a panel with two faces over the same vertices, wound apart and skinned apart. With both
drawn, the depth tie chose the side, and ARMCV's closed hatches showed their inside machinery
instead of the outside plating.

**Why clockwise is the right sign, and what keeps it right.** Nothing between the engine's screen
and the framebuffer reverses a winding: the bake emits each face's fan `(i0, ik, ik+1)` in index
order (`pb_walk`), as `0x459830` copies the face (`0x459AD5`); the pose is rotations only, and the
vertex stage (`tagpu_posedraw.c`) projects as the engine does, `px = x`, `py = −z − y/2`, then
scales by the zoom about a centre and by `ss` through the viewport, both positive; and `record_stage`'s
viewport keeps the engine's y growing downward ("NO Y FLIP"). In Vulkan's area rule
(`a = −½ Σ (xᵢ·yᵢ₊₁ − xᵢ₊₁·yᵢ)` in framebuffer coordinates) a clockwise face on a y-down framebuffer
has negative area, which `VK_FRONT_FACE_CLOCKWISE` keeps. **A future change that flips the
viewport, mirrors a piece matrix or reverses the fan must flip `frontFace` with it**; the symptom
of a wrong sign is every hull drawn inside out.

**The hard shadow follows the body, on both of the engine's branches.** A mobile unit's shadow
is the blackened composite, which holds only the faces the body bake painted; a structure's slant
is `0x45A610`'s fill through `0x4C1000`, which drops the faces that do not run clockwise under its
own projection `x + y/4`, `−z − y/4`. The vertex stage draws each range under its own projection
on the same y-down framebuffer, so the body's rule reproduces both. The mark and the clear share
one rasterisation state, so they still cover exactly the same fragments.

**What stays unculled, and why.** Built from the body's create-info after `cullMode` goes back
to NONE:

- **the build ghosts' `s_pipeGhost`.** The engine draws no ghost, and the ghost's look is every
  face blended at `uAlpha` 0.40 with depth writes off, back faces included — about 0.64 over a
  closed hull. Culling would lighten every ghost to 0.40, and it shows both skins of a
  back-to-back pair blended.
- **the effects models' `s_pipeFx` and the nanoframe wire's `s_pipeWire`**: a run is a rectangle
  and a wire record is a band, and neither's winding says anything about a face.

The sun-shadow map's `s_pipeCast` keeps its own NONE (a caster's back faces write depth too).

**Measured 2026-09-26** on one scenario (Two Continents, 1024×768, `ss=2`, the play arm set, a
private Xvfb with the GPU presenting): four ARMCVs, one finishing an ARMFUS nanoframe. Against the
GDI backend (`tagpu_gdi.on`) at 1×, the three idle ARMCVs' hatches match GDI with the change —
grey plating — and did not without it (the dark inside skin on all three); no hull renders inside
out; the building ARMCV's open hatches look the same in all three runs. A second run with the
shadow pipelines culled too (Canal Crossing, four ARMSOLARs for the slant, two ARMCVs for the
silhouette) draws every shadow where GDI draws it. On a closed hull the cull cannot change a
shadow's coverage, so that run shows the pipelines still draw, not the sign; the sign is the
body's argument above.

**Not covered.**

- Only ARMCV, ARMSOLAR and one nanoframe were looked at. Other units, wrecks and Classic++ were
  not compared against GDI; by the argument above they change only where a face is seen from
  behind.
- **The nanoframe wire still outlines faces seen from behind.** The engine's wire walk `0x4C0820`
  drops them like the fills do; ours draws every face's edges and relies on the body's depth, which
  hides them behind a closed hull but not on a single-sided face seen from behind. Not new with this
  change. Closing it needs each wire record to carry its face's winding (a third vertex) so the
  wire's vertex stage can drop the edges of back faces.
- Whether the engine's face-0 skip (`0x459AA2`) meets the file's selection primitive or a real face
  is open (engine map, the same section); `tagpu_posebake.c` keeps face 0 in the body and wire
  ranges.

### 2.99 A textured quad is mapped per pixel (`tagpu_posebake.c` `quad_frame`, `tagpu_native.c` FS `taQuadST`)

**What it is.** The unit pass draws a textured quad as two triangles, and until this change each
triangle carried its own affine uv. On any quad that is not a parallelogram that bends the texture
along the diagonal: the grid on an opened ARMSOLAR panel kinked into a V. The fragment stage now
inverts the quad's bilinear map per pixel, so the texture spans the whole quad as one surface. No
hook, no engine read or write.

**What the engine does, and why we do not copy it.** `0x4C8760` carries u and v down the quad's
two edge chains and then steps them linearly across each screen row (`0x4C7A20`; [engine
map](../exe-reverse-engineering.html), *How `0x4C8760` lays a texture on a quad*). For a
parallelogram that is the bilinear map, and so are the two affine triangles, so only
non-parallelograms change. For any other quad the engine's layout is the bilinear map only when
two opposite sides lie along screen rows; otherwise it depends on how the quad sits on screen and
shifts as a unit turns. Both remedies were shown to the owner side by side, front-on and rotating
(an offline render of ARMSOLAR, 2026-09-26), and the per-pixel bilinear map was chosen.

**How.** `quad_frame` places each textured four-vertex face of the BODY range in its own frame,
`P0 = (0,0)`, `P1 = (1,0)`, `P3 = (0,1)`, `P2 = (a,b)`, with `(a,b)` the least-squares fit of
`P2 − P0` on the two sides from the rest vertices. The material stream carries the corner's point
in that frame, `(a,b)` and the frame's atlas rect (`TAGPU_PB_MATST` 5 → 13 floats: locations 7-9,
`tagpu_posebake.h` lists them), and the rasteriser interpolates the point. That interpolation is
exact because every step from the rest vertices to the framebuffer is affine: the piece transform
is rigid, the projection `px = x`, `py = −z − y/2` is linear, and the zoom and the viewport are
scales. An affine map leaves the bilinear coordinates unchanged, so the texture belongs to the
model and does not swim as it turns. The fragment stage's `taQuadST` solves the quadratic for `t`
through the root whose slope, which is the map's Jacobian there, is positive, then takes `s`. `(s,t)` is
clamped to the face, so the sample stays inside the frame's rect, whose border repeats its edge.
Classic++'s mipped twin reads `textureGrad` with the gradient `J⁻¹·dh`: the inverse Jacobian at
the fragment's own clamped `(s,t)` times the screen derivatives of the interpolated point, taken
before any discard. `h` is affine across the face, so its derivatives are exact on every lane of a
2×2 quad. Inside a convex face the Jacobian stays above the bake's 1/64 margin, so no gradient
depends on a helper lane past the face's edge, where the extrapolated point can pick the other
root.

**What stays on the triangles.** A face that is not a quad, and a quad that is not convex in its
frame (`a > 0`, `b > 0`, `a + b > 1`, each with a 1/64 margin), where the bilinear map is not
one-to-one over the face. So does anything drawn by `FXVS`, `WVS` or the lab's vertex stage,
which write the rect as −1. In the 608 stock models there are 43 845 textured quads: 16 448
parallelograms (unchanged), 27 362 convex non-parallelograms now mapped per pixel, and 35 kept on
the triangles. 589 models have at least one changed quad. A non-planar quad (1 501 stray more
than 1 % of an edge off the plane) is fitted, not exact; its two triangles still agree along the
diagonal because both carry `P2`'s point.

**What it costs.** 32 bytes more per material vertex (20 → 52), in the CPU mirror and the GPU
buffer alike: a median model's stream is 74 KB rather than 28, and CORGANT's, the largest,
310 KB rather than 119. A stream is per (type, owner), so a game holding a few hundred costs tens
of MB more on each side. The fragment work is a square root and two divides per textured pixel.

**Measured 2026-09-26** on a private Xvfb with the GPU presenting (1024×768, the play arm set),
same-time A/B against the build before the change (08888b7):

- Canal Crossing, four ARMSOLARs at facings 0/90/45/180 and two ARMCVs, Classic, zoom 1× and
  3.5×. The panel grids run straight and meet the panel edges with no kink. The pixels that
  differ lie on the units only: nearly every panel pixel, and the ARMCVs' tapered hull faces
  (`Arm4c`, `Arm4d`, `camob3`, the `nano` arm; `quad_frame`'s rule puts them among the changed
  quads). The minimap differed only where the two games' AI units had moved differently.
- The same scene with Classic++ armed, at 0.5×, 1×, 3.5× and 5×: straight grids, no dark rim at a
  face's edge, no seam along a diagonal. The analytic gradient against one taken by `dFdx` of the
  solved coordinate, same scene and zooms: on the units only the thin ridge and fold faces differ
  (991 to 5 899 px a frame, counting the feature beside the collectors, whose restore timing
  varies), with no visible change at 6× magnification.
- `crowd-static` (256 units of 16 types), zoom 1× and 2.5×: no face broken or missing.
- Against GDI (`tagpu_gdi.on`) at 1×, the panels' texture layout matches the engine's frame.
  GDI's ARMSOLARs also show a black panel with white edges that neither Vulkan build draws.
  That is a separate, open question and not this change.

**Not covered.**

- The browser lab (`tools/tascene`) feeds no quad frame, so its unit lane keeps the two
  triangles. Its extractor also stops at `tagpu_feat.c` on `TAGPU_FEAT_MIRROR`, independently of
  this change. The unit shaders it extracts compile and link as GLSL ES 3.00.
- Only these scenes were looked at; wrecks were not.
- Where the engine pins a quad's corners to texels (`w − 1`, `h − 1`), ours sit on the frame's
  edges, as before this change.

### 2.106 Structures are shaded as the engine's lit rasteriser shades them (`tagpu_posebake.c` `piece_vnormals`, `tagpu_posedraw.c` VS, `tagpu_native.c`)

**What it is.** The engine shades a structure with a per-vertex intensity interpolated across each
face, and shades nothing else ([engine map](../exe-reverse-engineering.html), *The unit shade*). The
unit pass does the same. No hook, no engine read or write beyond the option word the packet already
carries.

**How.**
- **The bake** (`piece_vnormals`, once per model): per piece, each face's normal from its first
  three vertex indices, added to every vertex the face names, divided by the count and not
  renormalised, face 0 left out under a selection primitive. The geometry stream's normal slot holds
  that vertex normal; the slant and the wire keep a level normal. A four-corner face also carries
  its four corners' normals in the face's index order, twelve SNORM16 in the vertex's last three
  attributes (locations 10–12), on every vertex of the face; any other face repeats its own.
- **The vertex stage**: for a unit whose `uShd.y` is 1 it rotates the vertex normal by the piece
  and takes `int(dot(n, (−0.8, 1.0, 0.25)) · 5.0) & 31`, a piece with its shade flag clear taking
  row 15, and a row of 0 taken as 5 (below), and does the same for the four corner normals into the flat `vShQ`. Every other unit takes
  the neutral row. The vertex's own row leaves the stage as a smooth `vShade`.
- **The fragment stage** blends the four corners' rows bilinearly over the quad's own frame — the
  `(s, t)` `taQuadST` already solves for the texture — where the face has one (`vQR.x >= 0`), else it
  takes the smooth `vShade`; it floors the result and looks up `k[row]`. The engine spreads a quad's
  rows along its two edges and across each row (engine map, *The unit shade* 5), which the blend
  reproduces exactly when one pair of the quad's opposite edges is horizontal on the screen and
  approximates on any other quad. Two triangles
  each take a plane through three corners, and on a face with rows 0, 30, 0, 1 — a solar's opened
  panel — one of them is rows 0, 0, 1: a black half-panel the engine does not draw. Every vertex
  stage that feeds `tagpu_native::FS` writes `vShQ`.
- **Who is lit** (`TAGPU_PDUNIT.lit`, `tagpu_native.c`): the structure bit **and** the option word's
  Shading bit (`pk->gfx_opt & 0x20`) — the engine's two tests. A mobile unit, a wreck and an effect
  are not lit. A build ghost is lit as the finished building would be **[INFERRED]**: the engine
  draws no model at the placement cursor to compare with.
- `shDir`, `tagpu_r3d_shade_dir` and the old `neutral + dir · round(12 · N·SH_L)` quantiser are gone,
  and with them the face flip toward `SH_V` for the shade (the flip stays for the Classic++ lighting
  term's normal, which the game does not use).

**Row 0 is taken as row 5 — the one departure from the engine.** The stock `PALETTE.SHD` maps
every colour on row 0 to black, so a face whose corners compute 0 draws solid black: the solar's
opened right wing and the right half of its body. TADR fixes the same defect with a hook on
`0x45A2EC` that replaces a computed 0 by `ShadingZeroFallbackLevel`, default 5
([sim-fixes evidence](../tadr-port/sim-fixes-evidence.html) Part 4 §5); the pass does the same, with
the level fixed at 5, and keeps every other row, the wrap to 27–31 included. The owner chose it
from a side-by-side of the stock rows, this, a monotonic ramp, flat, per-pixel and self-shadowed
variants. The GDI lane stays stock, so it still draws those faces black. **Measured**, Classic
preset, paused at tick 137, share of pixels at luminance under 2 and mean luminance: the solar's
body right half 46 % / 16.6 → 1 % / 23.4, its right wing 49 % / 22.1 → 1 % / 26.4. Rows 1–4
(multipliers 0.06–0.27) are not touched, so a face near the wrap is still very dark.

**Measured**, on Xvfb against the GDI backend on the same scene, dry ground, paused at ticks 125–136,
shadows off, the Classic preset (`assets=0 light=0`); mean absolute luminance difference from the
engine's Shading-on frame over the pixels the engine's toggle changes:

| structure | Shading not modelled | per-triangle rows | per-quad rows | engine Shading off, for reference |
|---|---|---|---|---|
| air plant | 40.1 | 22.7 | 22.3 | 47.1 |
| laser tower | 56.4 | 19.3 | 15.5 | 52.2 |
| solar | 49.9 | 40.1 | 39.4 | 36.1 |

The kbot lab and the vehicle plant are left out: they animate continuously, and a pause cannot land
two runs on one pose (a run paused at tick 136 has the lab turned against the reference), so their
figures measure the pose, not the shade. The solar's figure is the selection box the engine draws
around it in the reference frame. Its black is the better measure — the share of pixels below
luminance 14, engine Shading-on frame against the pass. Both tables were measured before row 0
was taken as row 5 (above), with the engine's rows unaltered:

| solar region | engine, Shading off | engine, Shading on | per-triangle rows | per-quad rows |
|---|---|---|---|---|
| top trapezoid | 17 % | 23 % | 29 % | 23 % |
| body, right half | 13 % | 54 % | 69 % | 64 % |
| right wing | 15 % | 46 % | 51 % | 51 % |

With the engine's rows the body's and the wing's black is the engine's: its Shading-on frame
has it, and the pass's mask overlaps it over most of both, a little blacker (body +10 points,
wing +5, mostly one-pixel strips along the edges where the reference has its selection bracket
and the cast shadow). What per-quad rows remove is the black the pass added to the trapezoid.
With row 0 taken as 5 the pass is deliberately lighter than the engine there: about 1 % pure
black where the engine's frame has 45 %. Mobile units: mean luminance of a flash 74.8
(engine 75.0, before 83.4) and a peewee 67.4 (67.5, before 68.7). The option gates it: with the bit
clear (`main+0x37F06` reads `0x03`) the pass matches the engine's Shading-off frame (air plant 16.2
against 41.1 for the on frame), set (`0x23`) it matches the on frame.

**Not covered.**
- **Faces without a frame.** A textured quad that is not convex in its own frame, a quad with no
  texture and every face of more than four corners keep two-triangle (fan) interpolation, where the
  engine walks their edges; the untextured n-gon `0x4C0C70` is not disassembled.
- **The blend is the engine's only where a pair of opposite edges is horizontal.** The engine
  steps across horizontal rows, linearly in screen `x`; the blend's `s · t` term is quadratic along
  a horizontal line unless one edge pair is horizontal, so a tilted parallelogram whose corner
  rows do not satisfy `c0 + c2 = c1 + c3` (rows 0, 30, 0, 1) shades differently.
- **The departure is on purpose and not bounded to row 0.** A face with a row-0 vertex or
  corner draws lighter than the engine's, and the blend spreads that across the face, through
  rows 1–4 and the wrap; the solar's whole-solar luminance error against the engine's frame was
  not re-measured after it.
- **Faces the engine reads garbage for.** A face with fewer than three or more than 32 vertices adds
  nothing to the sums here; a zero-length cross is `(0,1,0)` here and a NaN in the engine. The posed
  vertices are 16.16 integers in the engine and floats here, so a product within rounding of an
  integer can land a row off.
- **The winding's sign was fitted** against the GDI frames, not decoded (`PB_SHADE_SIGN`).
- No shaded structure was compared while zoomed (GDI does not zoom), while cloaked, under
  construction, or in a network game, and the lab (`tools/tascene`) keeps its own unit light.
- The lab's kbot-lab pixel counts are pose-dependent: the lab and the vehicle plant animate, and two
  runs paused a few ticks apart differ in body pixels.

**Traps the measurement found.** The `shadow-lab` fixture puts its lab under water (altitude 63, sea
level 75): do not use it for shading. `tacli arm <i> 'classicpp.cfg=shadows=0'` before a `--defaults`
launch clears the engine's shadow bits in the settings store, and removing it later does not restore
them. The store owns `Shading` under `--defaults`, so a registry value does not gate it there: arm the
passes by hand (`tacli arm` the default set) to test the option.


The S4 rewind verifier excludes unseated spectator sight counters under the
owner-approved D2 amendment. Its return-to-now count comparison reports and
accepts VM START/CALL child allocations only; `tagpu_rec_cob_child` gives the
raised VM the stock VM's semantic caller keys around the synchronous allocator
call. This diagnostic context changes no engine state. COB threads are rebuilt,
so exact execution continuity remains outside S4. See the S4 rewind note.

Return-to-now also restores the objective's saved `+0x88` switch before resuming
steps. Conversion disarms it; takeover/fork arms it. The same level owns the
objective across rewind. This preserves games whose console command disabled
victory/defeat before the rewind.
