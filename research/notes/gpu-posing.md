# GPU posing for 3DO units — the G16 plan

**Status: in build.** Gate 0 (§0) and Gate A (§0b) are measured and passed, the level generation
(step 3) landed on 2026-09-08, and the per-type bake (step 4) is being built; the shader, the
gates that follow it and the deletion of the CPU emitters are not. §7 is the running score. The
rest of the page records the design grilled out on 2026-09-08 and the measurements taken while
planning.

The pass this replaces is `tagpu_native.c`'s unit emitter. Today it reads the engine's **posed
vertex buffer** `prim+0x22` and rebuilds a 14-float stream vertex per triangle corner, per unit,
per frame. The engine rewrites those buffers **in place, on the game thread, in two stages**
([engine map](exe-reverse-engineering.html), "The repose"), which is the pose race
[§2.9](gpu-status.html) detects and works around. G16 stops reading them: the geometry becomes a
static per-type vertex buffer and the pose becomes a per-piece matrix built from the engine's
**fields**, exactly as the replacement-mesh pass (`tagpu_hires_draw.c`) has done since it was
written.

---

## 0. Gate 0 — SETTLED 2026-09-08. The oracle omitted pitch and roll.

**The answer: the residual was the omission, not the buffer. There is no lag component at all,
and G16 is unblocked.**

The question was whether the residual `tacob pose-check --all` reported on fast movers —
**4.1 / 7.0 / 48.4 / 77.2** world units on the tank, fighter, gunship and bomber against 0 on
the kbot, building and ship — was the vertex buffer lagging the fields (the standing
attribution) or the checker applying **yaw only** while the engine folds **all three** body
words at `0x45B0DB`.

**Measured twice, offline and live.**

*Offline, from evidence already in the tree.* Each tracked fixture records its base piece's rest
vertices (`node=`) beside the engine's posed ones (`vbuf=`), and the base piece composes as
`posed = R(turn + body)·v + (off + move)` — so Kabsch on those pairs recovers `R` exactly and a
ZXY extraction gives the triple. Supplying it takes **all eight classes to exactly 0.000**:

| class | unit | yaw only | with the recovered body | recovered body (pitch / heading / roll) |
|---|---|---|---|---|
| kbot | ARMPW | 0.000 | **0.000** | +0.00° / −82.86° / +0.00° |
| tank | ARMSTUMP | 4.110 | **0.000** | −12.34° / −90.00° / +0.00° |
| building | ARMWIN | 0.000 | **0.000** | +0.00° / −180.00° / +0.00° |
| fighter | ARMHAWK | 7.023 | **0.000** | +0.00° / +94.78° / −23.96° |
| gunship | ARMBRAWL | 48.373 | **0.000** | +0.00° / −19.85° / −50.38° |
| bomber | ARMTHUND | 77.188 | **0.000** | +0.00° / +88.59° / +0.02° |
| ship | CORBATS | 0.000 | **0.000** | +0.00° / −90.00° / +0.00° |
| sub | CORSUB | 0.002 | **0.000** | +0.00° / −84.38° / +0.00° |

The recovery validates itself: on **five of the eight** — kbot, tank, building, ship and sub —
the heading it recovers reproduces the fixture's recorded `yaw` to within 2 units, and the
recovery never sees `yaw`. The tank is the one that makes the point: its residual was 4.110, so
it is not one of the four that already read 0, and its heading still comes back exactly right
while its *pitch* comes back as the −12.34° the old checker was throwing away. A fit to noise
would not agree with a number it was never shown.

*Live, with the fixed instrument.* `pose_dump` now folds the cached triple and prints it. A
fresh capture of the tank and the bomber reads **`err=0.00` on every piece**, against 5.45 and
77.19 before, and the tank's recorded `body=(63290, 49152, 0)` is **exactly** the triple
recovered offline from its own fixture — the game reproducing a number solved for out of a file.

**Two things came out of it that were not the question.**

1. ***The cached triple is not always the live one.*** The tank read `body=` and `live=`
   identical. The bomber read cached `(0, 16128, 3)` against live `(0, 44767, 65508)` — a
   heading **28639 units (157°)** apart — and the drawn geometry follows the **cached** one.
   Which of the two moves, and why, is **not established**; the new `live=` field is what will
   say on the next capture. G16 is on the right side of it either way: `recon_begin` folds
   `Object3do+0x18/+0x1A/+0x1C`, which is what the compose folds.
2. **`hires_pose` had the bug the oracle had — FIXED 2026-09-09.** The replacement-mesh pass
   posed in model space and applied the heading alone as an outer rotation (`uYawEnc`), so a
   glTF unit was drawn without the terrain's tilt — right on level ground, wrong on a hillside,
   and wrong by 157° of heading on the bomber above. It now folds the whole cached triple
   through `pose_accum_body` like `recon_begin`, and the caller sends 0 for the shader's yaw
   whenever a pose was produced so the rotation is applied once. Measured against the same unit
   drawn natively on the `selbox-slope` hillside; the flat-ground and Kbot frames are
   byte-identical to the old build. `model-import.md`, "The body turn is all three words".

**What changed in the tree.** `pose_dump` folds all three cached words (so its `err=` is now the
same quantity `recon_err` reports) and prints `body=` and `live=`; `tacob`'s parser and
`pose_check` take `body=` when the fixture has it. **The tracked fixtures predate `body=`**, so
`pose-check` still reports their old residuals and labels them `legacy: yaw only`. Regenerating
them is `tools/cobtrace_fixtures.py`, which also regenerates `cobtrace.log` and needs
`tacob fit-world` re-run for the tank's `replay.json` — a separate gate (`tacob run --all`,
nine byte-identical replays), deliberately not done here.

---

## 0b. Gate A — PASSED 2026-09-08. Nothing reconstructs wrong, and the extremes are exact.

Gate 0 fixed the oracle on eight one-unit fixtures. **Gate A is the same oracle over a screen
inventory**: 69 units, both sides, all eight classes plus the two extremes of stock content —
`ARMSCORP` / `CORSCORP` (36 pieces, the most in the game) and `CORGANT` (574 vertices, 304 faces,
the most of either). It is the last thing before the GPU path is built on the reconstruction,
and it needed **no new code**: `recon_err` has always folded all three body words, so the whole
gate is `tagpu_posewatch.on`, a camera stop per cluster, and the `native:` line's own counters.

The fixture is `scenarios/pose-inventory.json`; the arm set, the four stops and the full numbers
are in `research/notes/evidence/posewatch/gate-a-2026-09-08.txt`.

| stop | units | guard | rest | norecon | errmax | units flagged (err ≥ 4) |
|---|---|---|---|---|---|---|
| ground, on hold | 28 | 4814 | 15084 | **0** | 61.97 | 5 |
| ground, walking | 29 | 4951 | 15114 | **0** | 61.97 | 6 |
| structures | 26 | 1850 | 25500 | **0** | 39.81 | 3 |
| **the extremes** | 12 | **0** | **0** | **0** | **0.00** | **0** |
| air | 35 | 3845 | 0 | **0** | 35.50 | 3 |

The naval classes are a second load on Anteer Strait (`scenarios/pose-inventory-sea.json`): both
submarines, four surface ships, the patrol boats and the tidal generators, on hold and then under
move orders — 4404 lines, all `dirty=1/1`, `norecon` 0, and **no ship and neither submarine
flagged at all**, moving or still.

**Three things it establishes.**

1. **No model failed to reconstruct.** `norecon` is 0 in every window across 82 unit types:
   `pose_accum_body` returned a whole piece tree every time. That is the number §3 says to take
   before the landing, and it is zero.
2. **No disagreement without the engine's own flag.** All **27142** `posewatch:` lines of the run
   read `dirty=1/1`; **not one read `dirty=0/0`**. That is precisely the signature `recon_watch`
   was written to catch — "a large err with the flag clear both times" would mean the guard's
   bracket is not the whole window — and it is absent. Every err over 4 model units is a buffer
   the engine was itself declaring stale or mid-rewrite.
3. **The extremes are exact.** The 36-piece trees and the 304-face model read **errmax 0.00**
   with zero guard trips and zero rest-equal reads. Piece count and face count are not where
   this breaks.

The residuals that *are* there are the two §2.9 already names: a walking unit reads 10–19 (a tick
of animation), an aircraft 24–35 with `rest=0` — its buffer is being actively composed and the
guard catches the rewrite.

**What it found that was not the question.** A handful of **structures** read byte-equal to their
own rest vertex arrays *permanently* — `rest=` runs at tens of piece-reads per frame while
`guard=` names only three to six units — with the pose flag set on both sides of every read. So
their err is the model's own size for the whole session and `posefix` draws them from the
reconstruction every frame rather than occasionally. Identified by peeking
`Object3do+0x0C → unit+0x92 →` the def's name: "Gaat Gun", "Sentinel", "Solar Collector" (both
sides), "Vulcan", "Scorpion", "Wind Generator", and one aircraft, "Hurricane". **Why the engine
leaves those buffers at rest is not established.** It does not touch Gate A — the rest-equality
detector catches every one and the reconstruction is what gets drawn — and G16 stops reading that
buffer at all, so it becomes moot rather than fixed.

---

## 1. What moves, and what it is replaced by

Four places read `prim+0x22`. **All of them move** (owner's call, 2026-09-08):

| reader | what it draws | after G16 |
|---|---|---|
| `emit_geom` → `emit_node` | unit and wreck bodies; the silhouette shadow and the Classic++ depth pass re-draw the same vertex range | GPU-posed from the type's buffer |
| `emit_slant` | a structure's cached ground-projection shadow | GPU-posed, with the engine's integer snap in the shader |
| `emit_wire` | the nanoframe wireframe | GPU-posed, `GL_LINES` from the same buffer |
| `recon_err` / `pose_dump` | the oracle — not drawn | keeps reading the buffer; it is the only thing left that does |

`emit_fx_model` is **not** in scope: it rotates raw model vertices for effects models and never
touches an `Object3do`.

## 2. The decisions

Marked **[owner]** where the human chose in the 2026-09-08 grilling, **[mine]** where the
interview was cut short and the call was left to me — those are the ones to overturn first.

| # | decision | |
|---|---|---|
| 1 | All four readers, one landing | [owner] |
| 2 | The pose is read on the **render thread from the fields**, not snapshotted on the game thread | [owner] |
| 3 | **No per-unit fallback and no per-unit refusal** — there is one renderer, and it degrades *inside* a unit rather than dropping it | [owner] |
| 4 | Cache **split**: static geometry per type, material stream per (type, owner, atlas generation) | [owner] |
| 5 | A **level generation** owned by `tagpu_reclaim`; `s_aabb`, `s_sbox` and `s_pmap` adopt it in the same landing | [owner] |
| 6 | Two isolated gates with honest tolerances, not "0 differing pixels" | [owner] |
| 7 | **No fixed piece cap.** The pose goes in a std140 **UBO** sized for a few hundred pieces, and `HPOSE` with it | [owner] |
| 8 | The flat SHD shade is computed **in the vertex shader** from a baked rest normal | [mine] |
| 9 | A **posed twin** of the native program (and of the shadow depth VS), not a mode switch in the existing one | [mine] |
| 10 | One geometry buffer per type carrying **three ranges** — body triangles, slant triangles, wire lines | [mine] |

### Why the render thread, and what residual it leaves

The pose fields are written by the game thread — `0x480C90` (a COB `move`) and `0x480D22` (a COB
`turn`) — with no interlock either way. Reading them from the render thread can therefore mix
pieces across a tick boundary: leg A at tick *N*, leg B at tick *N+1*. The words are aligned 16-
and 32-bit, so there is no torn word, and **the rest-pose class is gone by construction** —
nothing we read is ever `rep movs`'d from a rest array. The residual is bounded by one tick of one
piece, which is strictly smaller than what the pass draws *today*, where the engine's buffer lags
the fields by a whole tick for the entire unit.

A game-thread snapshot would remove even that, at the cost of a new detour and a **second**
per-unit lifetime to guard. Not worth it for an artifact nobody can see.

## 3. Lifetime — the delete pattern, and the one thing it does not cover

The per-unit half needs no new machinery, and the guarded surface gets **smaller**:

| read | allocation | guarded by |
|---|---|---|
| `o3+0x00/+0x08/+0x18/+0x1E`, every `prim` field (`P_FLAGS`, `P_TURN`, `P_POS`, `P_NODE`) | inside the `Object3do` | `tagpu_reclaim` — `FreeObjectState 0x45AAA0` deferred until the render thread has completed the pass; the gather-time `o3` re-read against `unit+0x9E`; `ptr_ok` / `IsBadReadPtr` |
| `prim+0x22`, the posed buffer — **no longer read** | a separate `MEM_Free` at `0x45AAB2`, freed *before* the object at `0x45AAF6` | (was the same, one block earlier) |
| `node+0x04/+0x10/+0x24/+0x2C/+0x30`, faces, indices | the **model template**, shared by every unit of a type | nothing today — see below |

So G16 drops a dereference rather than adding one, and the pose fields live in the allocation the
destructor frees **last**.

**What is new is the template.** Today the template is read afresh every frame (faces, indices),
so a stale pointer is a one-frame read. G16 **caches** a GL buffer built from it, which turns the
same stale pointer into a wrong model drawn for the rest of the session. Three existing caches
already have this shape and no invalidation at all: `s_aabb` and `s_sbox` (keyed by node pointer)
and `s_pmap` (keyed by mesh + node pointer).

**The fix, for all of them:** a level generation.

- `tagpu_reclaim` already owns the level teardown `0x491B60` and wraps it on both sides: a pre-hook
  that runs on the game thread **before** the cascade frees anything, holding the render thread off
  for the duration, and a post-hook after it returns. It bumps an interlocked counter in the
  **post** hook and exposes `tagpu_reclaim_level_gen()`. *(This page said "pre-hook" while the
  plan was being written; the landing review caught that a bump there is re-read by a pass already
  past the overlay's teardown gate, which would refill the caches from templates about to be
  freed — [thread-safe destruction](thread-safe-destruction.html) §6a carries the reasoning.)*
- Every cache entry is stamped with the generation it was built under. The render thread drops
  and `glDelete`s mismatched entries — **GL deletion never happens on the game thread**.
- The bake itself runs inside the overlay-driver pass, which `teardown_active()` already gates,
  so the template read is covered by the existing handshake with no new state.

**Four things invalidate a baked buffer**, not one:

| trigger | why | detected by |
|---|---|---|
| level teardown | node pointers can be recycled by the next level | `tagpu_reclaim_level_gen()` |
| GL context loss | every GL id dies | `tagpu_native_glreset()` |
| **atlas reset** | `tagpu_gaf_atlas_reset` runs when the shelf fills (`a->full`) — **every UV changes** | a generation on `TAGPU_GAFATLAS` |
| **owner** | `face_texframe` picks `tab + owner*0x18` for team-colour faces | part of the cache key |

### One renderer: degrade inside the unit, never drop it

**There is no per-unit fallback path and no per-unit refusal.** A game does not ship a second
renderer against the possibility that its first one fails, and neither do we. Every condition
that looked like a per-unit failure in the first draft of this plan is either a *data* condition
with a local answer or a *pass-level* condition that already has one:

| condition | what it actually is | the response |
|---|---|---|
| a piece's parent link does not resolve | the piece tree, not the renderer | leave **that piece** at rest. `pose_accum_body` already sets `done[i] = 0` and `hires_pose` already uploads the identity for an undone piece — the unit draws, with one piece unposed |
| a face's indices or texture do not read | already handled per face today (`continue` in `emit_node`) | skip **that face** at bake time |
| more pieces than the pose storage holds | our own array size, not the engine's | removed — decision 7 |
| GL objects cannot be made, no atlas | the **pass** cannot run at all | existing behaviour: the pass is not armed and hands the draw back to the engine, exactly as `tagpu_r3d_ready()` already gates it |

What is left to log is not a refusal but an **anomaly**: once per model, at bake time, when the
tree walk finds something that should not exist. One line per model, not per unit per frame, plus
a counter in the `native:` line.

⚠ **`[CORRECTED 2026-09-08, by building it]` This paragraph used to name three things as the
anomaly — "a piece whose parent never resolves, a face with neither a texture nor a colour, a node
whose vertex array does not read". Measured over the 67 distinct models of the pose inventory,
they are not one kind of thing:**

| what the walk finds | over 67 stock models | so it is |
|---|---|---|
| a face with **neither a texture nor a colour** | **every model has some** — 238 faces, 3402 of 124814 baked vertices (2.7 %) | ordinary content. The engine's own rasteriser skips them too, and the slant raster *fills* them (the footprint quad). A **count**, `nomat=` |
| a face outside the emitters' `3 ≤ fvc ≤ 32` | 30 faces, in 14 of the 67 | ordinary content. A count, `odd=` |
| a piece whose **node or vertex array does not read** | **0** | an anomaly, `anom=` |
| a piece whose **parent link never resolves** | **0** | an anomaly, `anom=` |

Reporting the four as one number logged 1486 anomalies on a scene that has none. The first two
are statistics and are logged only under the lever's `log` token; the last two get a line whether
or not logging was asked for, because nothing in stock content produces either. That the parent
walk never fails is the same fact Gate A read from the other end: `norecon` was 0 there too.

The instrument to measure how often
any of it happens **before** the landing already exists: `tagpu_poserecon.on` forces the
reconstruction for every unit on every frame, and `s_poseNorecon` counts the walks that failed.

## 4. The architecture

### Per type, built once (the geometry buffer)

One walk of the `Model3DONode` tree produces, per vertex: the **rest position**, the **rest
normal** of its face, the **piece index**, the face-corner slot (the fan mapping an *n*-gon's
corner onto a quad's UVs, `slot[t] * 4 / fvc`), and a degeneracy flag. Three ranges are laid
down in the same buffer:

```
[ body triangles ][ slant triangles ][ wire lines ]
```

- **body** — every face that `emit_node` would emit, with the selection-primitive skip and the
  quad rule applied as they are today;
- **slant** — every face except face 0 when `N_SELPRIM != -1`, flat, following `0x45A610`'s
  rules rather than the body rasteriser's;
- **wire** — each drawable face as a closed polygon outline.

Keyed by `(template root, level generation)`. Static: it survives an atlas reset and a team change.

### Per (type, owner, atlas generation) — the material stream

`uv`, the colour-key flag, the flat colour, and a **skip** flag. A face with no resolved texture
*and* no flat colour is skipped by the engine's rasteriser too — today that changes the vertex
*count*; here the face is baked anyway and the skip flag collapses its triangles in the vertex
shader, exactly as a HIDden piece's all-zero matrix already does. **The two buffers therefore
always have the same vertex count**, which is what makes them independently rebuildable.

### Per unit, per frame

`pose_accum_body(o3, h, bt)` — the same call `recon_begin` makes — gives one 4×3 per piece with
the body turn folded into the base piece. One `glDrawArrays` per unit from the type's buffer,
with the per-unit uniforms the pass **already** sets (fog word, cloak alpha, waterline, digger,
nano state, caster triple).

**The pose goes in a std140 UBO, and there is no piece cap.** `TAGPU_HMAXPIECE 48` and `HPOSE`'s
`nd[64]` / `acc[64][12]` are *our* array sizes — nothing in the engine bounds a model's piece
count, and a fixed-size GLSL uniform array is the only reason a bound was ever needed. GL 3.1+
guarantees `GL_MAX_UNIFORM_BLOCK_SIZE` ≥ 16 KB, which is 1024 `vec4` — **341 pieces** at three
`vec4` each. Sizing the block and `HPOSE` to a few hundred puts the bound an order of magnitude
above the largest stock model (36) and above any plausible mod, so it stops being something the
design has to reason about. A hidden piece still arrives as an all-zero matrix, which collapses
its triangles onto the model origin.

**`pose_accum_body`'s parent-link walk must be cached per type.** It rebuilds parent links by
scanning the node list for each sibling of each node — fine on today's rare trip frames, not fine
at 200 units a frame. The topology (parent array, accumulated rest offsets) is a property of the
template and belongs in the geometry cache entry; what stays per unit per frame is the matrix
build — `[MEASURED]` at most 36 pieces on any stock model.

### The shader

The vertex shader takes over what `emit_node` does on the CPU: the piece transform, the engine's
projection (`sx = ax + x`, `sy = ay + (−z − y/2)`), the depth key
`encBase + clamp((2y − z)/256, ±1.8)`, the world x/z the fog samples, the model height the
waterline clips on, and the **shade**.

The shade is the interesting one. Every face of a 3DO belongs to **one** piece, so its normal
transforms rigidly with that piece's matrix — bake the rest normal, transform it as
`tagpu_hires_draw` already transforms `aNrm`, then do the flip toward `SH_V`, the normalise, the
dot with `SH_L` and the quantise `clamp(shNeutral + shDir·floor(I·12 + 0.5), 0, 31)` in the
shader. **One thing that is NOT exact, found by the step-4 review:** `emit_node` takes its
degeneracy test `nl > 1e-6` on the **engine's** posed vertices, which the compose has rounded into
16.16 at every axis and every level of the tree, while the bake takes it on the rest vertices. A
face of any real area gives the same answer; a near-degenerate one can land on the other side and
take the neutral row where the CPU path takes a shaded one, or the reverse. That is a whole face
one SHD row off — which §5 already names as Gate B's tell for decision 8, so Gate B should now be
read as testing this specifically rather than only the quantisation. The Classic++ outward normal comes off the same vector. **[mine]** — the alternative is a
per-face CPU shade uploaded per unit per frame, which keeps a streaming buffer the design is
trying to lose.

`emit_slant`'s snap stays the engine's: `xi = v[0] >> 16`, `q = yi >> 2`, on the posed 16.16
value. 16.16 values are exactly representable in float32 while `|model unit| < 256`
(256 × 65536 = 2²⁴), so the chain reproduces — see §5 for what it still cannot promise.

**A posed twin of the program, not a mode switch [mine].** The existing native program's
attributes arrive *already in frame-pixel space* and are shared with the selection lines and the
effects models, which stay CPU-built. A uniform switch would leave one path reading attributes
the other VAO does not bind. The shadow depth pass needs the same treatment — `tagpu_hires_depth`
is the precedent for both.

### What stops being true

- **`s_emitTop`** (the posed model top, the shadow's height rule) is currently taken from the
  posed vertices. It becomes a CPU computation from each piece's **rest AABB** through its pose
  matrix — 8 corners × ≤36 pieces. Units with a unit record already prefer `model_aabb`, so this
  only feeds wrecks.
- **`MAXNV` / `s_vtrunc`** stop applying to units: the geometry no longer passes through the
  shared 49152-vertex stream, so a 200-unit frame can no longer truncate. That is a win, and it
  is a behaviour change worth stating.
- **The pose guard, the rest-equality detector and `posewatch`** are only reachable through the
  CPU emitters. Decision 3 removes those emitters, so they go with them — **in the last commit on
  the branch, after Gate B has run**, because Gate B needs the CPU path as its oracle.

### Built 2026-09-08 — step 4, and what it measured

`tagpu_posebake.c` / `.h`, driven from `tagpu_native_frame` beside the `emit_geom` it will
replace, behind **`tagpu_posebake.on`** (tokens `log`, `check`). **Nothing draws from these
buffers yet** — the posed program is step 5 — so the lever is off in play and the `native:` line
is byte-identical to main's without it.

**One walk, not three.** `emit_node`, `emit_slant_at` and `emit_wire` each walk the same tree with
slightly different rules, so the bake has a single `pb_walk` that both bakes and the predictor
drive, parameterised by range. A rule that lives in one place cannot drift between the geometry
buffer and the material stream — which is exactly what the "same vertex count" invariant needs.
`mat_bake` refuses and logs if the two walks ever disagree, rather than uploading a stream that
lies about which vertex it belongs to.

**The invariant is checked, not asserted.** With `check` in the lever the bake is held to the
emitter that just ran on the same unit in the same frame, on two independent quantities:

- the **body vertex count** `emit_geom` produced, against `tagpu_posebake_predict_body` — the
  bake's body range minus this unit's invisible pieces and minus the faces the material stream
  collapsed;
- the **accumulated rest offsets** the bake walked off the template, against `s_recon.rest[]`,
  which `pose_accum_body` rebuilds per unit per frame — equality to within 1/65536.

Over the whole pose inventory, four camera stops: **0 mismatches on either**, `anom=0`,
`refused=0`, 67 of 67 types baked. The largest bake is **6090 vertices** — `33 piece(s) -> 6090
vert (body 1830, slant 1824, wire 2436)`. *[CORRECTED by the landing review: this first quoted the
6090 total against a different model's breakdown (1218/1212/1616, which sums to 4046) and named it
`CORGANT`, which the log does not say — the bake lines carry a root pointer, not a type name.]*

**What it deviates from §3–§4, and why.**

| | |
|---|---|
| the anomaly counter is **four** counters | see the correction in §3: three of the four things §3 called anomalies are ordinary content |
| the cache is **128 types / 256 material streams** | 64 was reached and started evicting on a 69-unit screen. At ~2000 vertices per model that is ~8 MB + ~10 MB of static VBO, against the **2.75 MB re-uploaded every frame** today |
| `TAGPU_HMAXPIECE` (48) is **left alone**; the new bound is `TAGPU_PBMAXPIECE` (256) | decision 7 is about *our* array sizes. 48 is the replacement-mesh program's **uniform array** size, and raising that one to 256 would be 768 `vec4` of non-block uniforms — a different constraint, and not this step's. `HPOSE`, `pose_accum_body`'s parent array and the emitters' `nparts` guards all move to 256, and the three `HPOSE`s become statics rather than 17 kB stack locals |
| the topology is **cached but not yet consumed** | §4 requires the parent walk to stop running per unit per frame; the entry now holds `parent[]` and `restOff[]`, and the caller that uses them instead of `pose_accum_body`'s own walk is step 5. Until then the `check` token uses the duplication as an oracle |

**Three of the four invalidations are exercised; one is not.** The **atlas generation** and the
**owner** are in the key, so ordinary play exercises them — a wrong one simply misses. The **GL
generation** was watched on an exit to the shell, which re-creates the context: `posebake: dropped
53 geometry (taking 53 material with them) and 0 material in its own right — level 0, GL 2, atlas
4`. (That line reports the cascade separately because a material stream is only meaningful against
the geometry it was walked beside, so dropping a geometry entry takes its streams with it; without
the split the line read "0 material" on a reset that had just dropped every stream there was.) The
**level generation** was not measured *at the time*: the game did not survive a level teardown with
`tagpu_reclaim` armed (§6b of [thread-safe destruction](thread-safe-destruction.html)), and with
`reclaim.off` the generation never moves at all because the hook that bumps it is not installed.
**That blocker is gone since 2026-09-09** — deferring the model-template frees makes a teardown
survivable, and two full game → shell → game cycles were measured working (the route is the
`ta-drive` skill's `reclaim.off` bullet). So this check is **runnable now and simply has not been
run**: it no longer *rests* on step 3's verification of the identical mechanism in `cache_gen_check`,
it is owed a run of its own.

### Built 2026-09-09 — step 5, the posed program, and what it measured

`tagpu_posedraw.c` / `.h` behind **`tagpu_posedraw.on`**, off in play. This is the pass that finally
draws from step 4's buffers: a unit is one `glDrawArrays` out of its type's geometry and material
VBOs with its whole pose in a uniform block, and **no vertices are built for it on the CPU at all**.

**A twin, not a mode switch**, as §4 requires. Its VERTEX stage is the port of `emit_node`; its
FRAGMENT stage is the native pass's own, handed over by `tagpu_native_unit_fs()` rather than copied,
so the two programs cannot drift in the half step 5 does not touch. The shadow-depth twin is that
same vertex shader with an empty fragment shader and `uDepthPass = 1` — which is what
`tagpu_shadow.c`'s own `VS_U`/`FS_NONE` pair does for the CPU stream, so a colour-keyed texel casts
a shadow on both paths rather than one of them discarding it.

**The pose is a std140 block: `vec4 uRow[3*256]` plus a packed `vec4 uPieceFlag[64]` — 13 312
bytes**, inside the 16 KB GL 3.1 guarantees with headroom rather than sitting exactly on it. The
guarantee is checked, not assumed: `GL_MAX_UNIFORM_BLOCK_SIZE` is read at build time (**65 536** on
the reference setup) and the pass refuses to arm below 13 312, leaving every unit to the CPU emitter
instead of drawing them all wrong. The per-piece `shaded` bit needs that second array because all
twelve floats of the 4x3 are the matrix, and it cannot live in the per-type bake: it is
`emit_geom_at`'s `pieceShaded`, which is per UNIT (a COB can clear the flag).

**Step 5 consumes the cached topology.** `posed_pose` (`tagpu_native.c`) builds the matrices off the
bake entry's `parent[]` instead of re-walking the node tree per unit per frame — the duplication §4
asked to have removed. Its arithmetic is deliberately still `pose_accum_body`'s, operation for
operation, because until step 8 that duplication is what `posebake.on=check` uses as its oracle. It
**refuses rather than mis-placing** a unit — a piece count that is not the baked model's, a node that
does not read, a piece whose parent link never resolved, the frame's pose arena full — the same bar
`recon_begin` sets, and the caller then falls back to the CPU emitter. A refusal is **counted, not
silent**: the `native:` line carries `posed=<units>/<tris>` and grows ` skip=<n>` when any unit fell
back.

**`s_emitTop` changed source**, as §4 said it must. Each piece's BODY-range rest AABB is baked
(`pmn`/`pmx`/`pbody` on the geometry entry) and the model top is those 8 corners through the piece's
pose matrix. Two deviations, stated rather than hidden: an AABB carried through a rotation *bounds*
the posed points rather than hitting them, so the top is an **over-estimate**; and it covers every
body face, including the ones whose material the stream collapses, which `emit_node` skipped before
it ever looked at their y. It feeds the shadow height of **wrecks** only — a unit with a record
prefers `model_aabb`.

**The Classic silhouette shadow is routed through the posed program too.** It reuses the body
geometry, so a posed unit would otherwise silently lose its shadow whenever Classic++ is off. Its
*slant* is not posed (step 6), so a structure is still drawn from `sfirst` by the CPU loop and
skipped by the posed one.

**MEASURED 2026-09-09**, 1024x768, `ss=2`, the sim **paused** so the poses are frozen:

| scene | result |
|---|---|
| `pose-inventory`, ground stop, 28 units | **27 posed**, `skip=0`. The 28th is skipped before the branch on **both** paths — the diffs below are what prove the two draw the same set |
| the same, posed program vs CPU emitter | **2 differing pixels of 786 432**, max channel delta 7 — two isolated single pixels, on different rows |
| the same, **`poserecon.on` on both sides** — Gate B's protocol, the same pose through both paths so the diff isolates the port | **1 differing pixel of 786 432**, max channel delta 7 |
| `200v200`, 209 units gathered, **111 posed / 12 879 triangles**, `poserecon` both sides | **0 differing pixels of 786 432** |
| the CPU vertex stream on that scene | **33 279 verts -> 132** (the remainder is slant, wire and effects — none of them ported yet) |

So the port is a **single-pixel edge flip on one fixture and byte-identical on the other**: the shape
§5 predicts from the 2e-5 model-unit residual, and **not** the "whole face one SHD row off" tell that
would send decision 8 back for rework. That is Gate B's bar met on two scenes. What makes it a
*preliminary* reading rather than the gate itself is that neither scene exercises slant or wire,
which are step 6.

**The frame-time criterion is NOT met, and is not measurable in this setup.** At 209 units both paths
hold **58.5 fps** and are indistinguishable. `tools/tacli` rewrites `maxfps=60` into the instance's `ddraw.ini` at all three of its launch paths and the DLL reads it at attach, so an edit made beforehand is overwritten — which is what actually happened here, including on the two runs labelled "uncapped" at the time. **`maxfps=0` IS the unlimited setting**: `fpsl_init` maps a NEGATIVE value onto the display refresh (60 here) and only `0` falls through every branch leaving `tick_length` at 0. So the number is obtainable — it needs the value to survive the launch, not a different value. The honest statement of what was measured is the byte count
above, not a frame time. Whoever takes the real number needs the cap lifted at launch — and the
"before" half is **not perishable**, because both paths live in one build behind the lever until
step 8.

## 5. What cannot be byte-exact, and the gates that follow

The engine's arithmetic is fixed point with rounding at every step. `0x4B7173` returns the pair
**untouched on a zero angle word**, otherwise computes `p0' = p0·cos − p1·sin`,
`p1' = p1·cos + p0·sin` in x87 and stores back with a bare **`fistp` — round to nearest, into
16.16 integers**. `0x45B150` applies that per piece, in place, per level of the tree. `tacob`
reproduces it with Python doubles and `round()` and gets worst **0** on the kbot, the building and
the ship.

Our reconstruction composes float matrices and rounds **once**, which is where the 2e-5
model-unit residual comes from: it lands on a neighbouring 16.16 grid point, one or two LSB out.
**A float32 vertex shader cannot close that** — reproducing `fistp`-per-level needs the products
in double. So the roadmap's "0 differing pixels across the screen inventory" is not attainable as
written. It bites in two very different places:

- **bodies** — 2 LSB is 3e-5 px; it flips a coverage sample only when an edge lands within 3e-5
  of it. Order one pixel per frame across a 1080p frame of units.
- **`emit_slant`** — the snap is an arithmetic **floor**, so 2 LSB flips a whole screen unit
  whenever a coordinate sits within 2/65536 of an integer, and that moves a shadow edge a full
  pixel. This is exactly where G14j won byte-exact structure-shadow parity.

**The gates, split so a failure names itself:**

| gate | what it compares | units | how |
|---|---|---|---|
| **A** | the reconstruction vs the engine's buffer | model units | `posewatch` `err=` over a screen inventory — the existing oracle, once Gate 0 has fixed it. **PASSED 2026-09-08, §0b** |
| **B** | the CPU reconstruction vs the GPU port | pixels | a **paused** scene, `tagpu_poserecon.on` rendering the same pose through the old path; the diff isolates the port alone |
| **C** | the flicker regression | pixels | the 62 s walk protocol: frames >500 px → **0**, >1000 px → **0**, 1× as the control |
| **D** | structure-shadow parity | pixels | the G14j fixtures, at a **stated tolerance** rather than "byte-exact" |

Bar for B: single-digit pixels, every one a single-pixel edge flip, characterised and written
down. A shade quantisation flip would show as a whole face one SHD row off, not an edge — if B
shows that, decision 8 is the thing to revisit.

Gate B must run on a **paused** scene: on a moving unit the engine's buffer lags the fields by a
tick, so the two paths are drawing different moments and the diff means nothing.

## 6. Measurements taken while planning

`[MEASURED 2026-09-08]` over all 608 stock `objects3d/*.3do`, read through `tools/ta3do`'s
archive reader (nothing extracted, nothing written):

| | |
|---|---|
| models | 608 |
| most pieces in one model | **36** — `armscorp`, `corscorp` |
| models over 48 pieces / over 64 | **0** / 0 |
| most vertices / faces in one model | 574 / 304 (`corgant`) |

The point of the number is not that 48 would have been safe. It is that the largest thing stock
content asks for is **36**, an order of magnitude under what a UBO holds — so the bound is
arbitrary and belongs nowhere in the design (decision 7). It also sizes the buffers: a 36-piece
model at 3 `vec4` is 432 uniform components, and the biggest geometry bake in the game is
`corgant`'s 304 faces.

## 7. Order of work

1. **Gate 0** — the pitch/roll measurement. Nothing else starts until it answers.
2. The oracle fix it implies (fixtures carry all three body words; `pose_check` passes them),
   and the note corrections.
3. `tagpu_reclaim_level_gen()`, adopted by `s_aabb`, `s_sbox`, `s_pmap` — **built 2026-09-08.**
   Verified on a real teardown: `reclaim: level teardown (gen 1)` followed by `native: level 0 ->
   1, dropping the template caches: aabb=1 selbox=1 pmap=0`. **The second half could not be
   tested**, because the game does not survive a level teardown while `tagpu_reclaim` is armed —
   a pre-existing freeze found doing exactly this, [thread-safe destruction](thread-safe-destruction.html)
   §6b. So "the caches repopulate correctly on the next level" is asserted from the code, not
   measured, and it stays that way until that freeze is fixed.
3b. **Gate A** — the reconstruction over a screen inventory rather than eight fixtures.
   **Run 2026-09-08 and PASSED**, §0b: `norecon` 0 across 82 types, 27142 watch lines all
   `dirty=1/1` and none `dirty=0/0`, and the 36-piece and 304-face extremes at errmax 0.00.
4. The per-type bake and its cache; the material stream; the bake-time anomaly log.
5. **BUILT 2026-09-09** — `tagpu_posedraw.c` behind `tagpu_posedraw.on`; §4's step-5 section
   carries the numbers. The posed program and its shadow-depth twin; bodies only, behind a lever, both paths present
   — the CPU emitters live **only** as Gate B's oracle from here to step 8.
6. Gate B on a paused scene; then slant and wire; then Gate D.
7. Gate C, the walk protocol.
8. **Last commit:** delete the CPU emitters, the pose guard, the rest-equality detector and
   `posewatch`, and the levers that only they answer to.
