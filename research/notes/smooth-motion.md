# Smooth unit movement and animation

*A design, not a landing. **Nothing on this page is built and none of it is scheduled** — it is
future work, written down while the measurements behind it were fresh (2026-09-09). The concrete
piece is interpolating COB-driven piece poses toward the next keyframe so models animate at render
rate instead of at the 30 Hz sim tick; the wider topic — unit turn rate, body rotation, position
between ticks — is §8. Companion pages: [file formats](file-formats.md) §2 (the COB format and the
animation stepper), [GPU posing](gpu-posing.md) (the pass this plugs into), [exe reverse
engineering](exe-reverse-engineering.md) §"The COB engine" (the thread records), [tacob](tacob-design.md)
(the VM and editor the prototype lives in).*

Facts tagged `[MEASURED]` were run this session and say how. `[VERIFIED]` was read from the
binary or the source named. `[PLANNED]` is a design decision with nothing built behind it.

---

## 1. Why

TA drives every piece of a unit from its COB script at the 30 Hz sim tick, and **stock locomotion
is authored as keyframes**: a `walk` cycle is a straight-line list of `MOVE_NOW`/`TURN_NOW` writes
in pose blocks separated by `SLEEP`, with no `speed` operand anywhere — see
[file formats](file-formats.md) §2.6. ARMPW's stride is twelve poses over nineteen ticks. So legs
snap between discrete poses, and at 60 fps each pose is simply displayed twice.

The proposal is to blend each piece from its current pose toward its next one, in our renderer
only.

**The engine's own interpolator is not the thing being replaced.** `0x4B1C00` already steps
`MOVE`/`TURN`/`SPIN` by `dt × (speed / 30)` per tick, and aiming and settling scripts use it
throughout. Those pieces are already smooth at the sim rate and need nothing from this page. What
is stepped is exactly what the animators chose to author as keyframes.

## 2. Where it goes

`posed_pose()` — `tagpu/ddraw/src/tagpu_native.c`, render thread, called from the G16 unit pass.
Per piece it reads exactly two things before composing the 4×3:

```c
mv = (const int*)(pr[i] + P_POS);             /* i32[3] 16.16 COB MOVE  */
tn = (const unsigned short*)(pr[i] + P_TURN); /* u16[3] TAang COB TURN  */
```

Those two reads become blended values. That is the whole insertion, and it is only this small
because [G16](gpu-posing.md) turned the pose back into per-piece **fields** → matrices; nothing
reads the engine's posed vertex buffer `prim+0x22` any more. Before G16 the pose arrived already
baked into vertices and smoothing it would have meant lerping geometry.

## 3. Invariants

These are not options.

1. **Read-only.** Nothing is written back to `PrimitiveStruct`. The sim reads those fields —
   `get PIECE_XZ`, and `QueryPrimary`/`AimFromPrimary` hand the engine weapon muzzle origins out of
   them. A framerate-dependent, per-machine blend written there would feed the simulation, and TA
   has no runtime desync detection to catch the divergence ([networking](networking-lobbies.md)
   §"What we learned about TA's network model"). Kept inside `posed_pose`'s output it is invisible
   to the sim and needs no CRC or handshake change.
2. **One renderer.** Degradation is a **blend weight of 1.0**, never a second code path. An
   unresolvable script, a missing history entry, a unit that just came on screen, an exhausted
   arena all mean "snap to the current fields", which is bit-for-bit what the pass does today. This
   is what satisfies "fall back to default behaviour" without violating [G16](gpu-posing.md)
   decision 3.
3. **Blend the fields, not the matrices.** Lerping composed 4×3s is wrong; lerping the source
   triples and then composing is right, and the matrix build is already per piece and cheap
   (≤36 pieces on any stock model).
4. **TAang wraps — take the short way round**, as the engine's own `TURN` does
   ([file formats](file-formats.md) §2.6). 350° → 10° is +20°, not −340°.
5. **Visibility never blends.** `SHOW`/`HIDE` is binary; `P_FLAGS` bit 0 snaps.
6. **The body turn is out of scope here.** `posed_pose` folds `O3_BTURN` into the base piece and
   that comes from the unit's own yaw/pitch/roll — sim-driven, not COB. It is §8.

## 4. Where the target pose comes from

| | **A — history** | **B — lookahead** | **C — extrapolation** |
|---|---|---|---|
| how | buffer past poses, render delayed | read the COB's future, blend forward | project from the last two samples |
| COB parsing | none | abstract interpretation | none |
| latency | ~66 ms on pieces | none | none |
| coverage | every unit, every mod, forever | see §5 | every unit |
| new engine reads | none | COB object + thread records | none |
| verdict | build first | upgrade path | **rejected** |

**C is rejected on the data.** TA poses are keyframes with holds; extrapolating across a snap
overshoots on every step. There is no fourth option: interpolation needs either history (latency)
or lookahead (parsing).

### A — history

Snapshot each drawn unit's `P_POS`/`P_TURN` when the sim tick counter (`main+0x38A47`) changes;
render at `t − D` with D ≈ 2 ticks; interpolate between the two bracketing samples. `[PLANNED]`

- **Arena.** Raw fields are 18 B/piece. At `PD_ARENA` sizing (`MAXU * 48` = 98 304 pieces) two
  snapshots are **3.5 MB**, less than the 4.7 MB pose arena already there. Past the arena, weight
  1.0 — and counted, the way [G16](gpu-posing.md) step 8 counts `rest=`.
- **Keyed by `(o3, nparts, level generation)`, dropped on mismatch.** Unit array slots are
  recycled; without this a new unit inherits a dead one's poses.
- Sampling happens on the render thread, so a snapshot can mix pieces across a tick boundary —
  bounded by one tick of one piece, the residual [G16](gpu-posing.md) §2 already accepts by design.

### B — lookahead

A thread parked in `SLEEP` carries everything needed. From
[exe reverse engineering](exe-reverse-engineering.md) §"The eight records": `[VERIFIED]`

| what | where |
|---|---|
| unit → COB object | `unit+0x9A` |
| eight thread records | `cob+0x1C`, stride `0xA4` |
| status (`0x02400000` = sleeping) | record `+0x00` |
| pc, a word index, kept current in memory | record `+0x04` |
| sleep ticks left | record `+0x0C` |
| static variables, `count × 4` | `cob+0x10` (read by `PUSH_STATIC` `0x4B13AA`) |
| locals — the thread's own stack | record `+0x24`, 32 words |

So for a sleeping thread we know where it resumes, in how many ticks, and the values of everything
its branches test. Decode forward from the pc, collecting the pose writes it will apply on wake.

**Run it on the game thread, not the render thread.** [G16](gpu-posing.md) §3's lifetime table
covers the `Object3do` and its `PrimitiveStruct`s under `tagpu_reclaim`'s deferral; **the COB
object is a different allocation and is not in it.** Reading it from the render thread would add a
new lifetime to guard, which is the one thing G16 spent a landing removing. Computing the lookahead
in a hook on `0x4B0D60 COBEngine_DoScriptsNow` — one call site, `0x48ADEB`, once per unit per tick,
with `cob` in hand and alive by construction — and stashing the result in our own arena means the
render thread introduces no new engine lifetime at all. Five sites in that engine are already
hooked for [cobtrace](tacob-design.md). `[PLANNED]`

**B replaces A's input, not A's code.** Same blend, same wrap handling, same weight-1.0
degradation.

## 5. Coverage — measured

`tools/cob_lookahead.py` is the harness; it re-derives every number below from the game archives.
`[MEASURED 2026-09-09, all 278 stock COBs]`

It abstract-interprets each `SLEEP`'s resume point forward with a constant-only stack: `PUSH_CONSTANT`
pushes a value and `PUSH` local/static pushes UNKNOWN; `MOVE_NOW`/`TURN_NOW` record the write and
refuse an UNKNOWN; **`MOVE`/`TURN` (the speed variants) are stepped over rather than refused**,
because those pieces are already interpolated by the stepper; `JUMP` follows and `JUMP_NOT_EQUAL`
resolves when the condition is known and otherwise forks. `SLEEP` and `RETURN` end a path.
**Resolved** means every path completed and agreed on the same write map — and reaching `SLEEP`
having written nothing counts, because "no pose change is coming" is as actionable as knowing one
is.

| scanner | walk / walklegs | every script |
|---|---|---|
| strict opcode whitelist | 46.6% | 25.4% |
| + correct terminators, step over the speed variants | 58.1% | 22.6% |
| + agreement-based resolution | **74.5%** (959/1288) | **63.5%** (2225/3503) |

Median 8 pieces written per wake, max 19.

**Two traps this measurement removed, both of which would have sunk a naive implementation.**

1. **A syntactic whitelist rejects everything.** Every pose block in a stock walk cycle is gated by
   a compile-time-constant branch — the bytecode opens `PUSH_CONSTANT 1; JUMP_NOT_EQUAL <past the
   block>` and repeats that per pose. A "no branches" rule rejects **100% of stock walk cycles**.
   The rule has to be *statically resolvable*, not *syntactically simple*.
2. **The success criterion is not "reach the next `SLEEP` through approved opcodes"** but "know the
   write set the thread will apply on wake". `RETURN` is a clean terminator (`MotionControl`
   re-calls `walk` on the same tick), the speed variants are skipped rather than refused, and an
   empty write set is a result.

**74.5% understates what the DLL would get.** Every remaining blocker on a walk script is
`paths disagree` (195) or `fork limit` (134), and **both arise only from an unknown static or
local** — which the offline harness must fork over and the game does not, because both live in
readable memory (the table in §4). For locomotion specifically the expected runtime fallback rate
is at or near zero. That is an inference from the blocker classes, **not a measurement**: nothing
has read those values at runtime yet.

**The irreducible blocker is `GET_UNIT_VALUE`** — 503 windows corpus-wide, engine state that cannot
be evaluated without executing, and executing is forbidden (`rand` alone would advance the sim RNG
at `0x4B6C30` and desync the game). **Zero of them are in a walk script.** Those are aiming and
state scripts, which drive their pieces with `TURN … speed` — so the half we cannot predict is the
half the engine's stepper already interpolates. The remaining counts (`HIDE` 134, `NOT` 58, `SET`
54, `POP_STATIC_VAR` 51, `SET_SIGNAL_MASK` 46, and a long tail) are opcodes whose stack effects the
harness simply does not model; they are pose-neutral and none of them appears in a walk script.

## 6. Safety

Bounded values, not probed pointers: slot `0..7` by construction; pc bounded against the script's
code length from the COB header; the piece operand bounded against `nparts` — the `model_root`
shape the project standard names. The tier-B arena is ours, so the render thread introduces no new
engine lifetime at all.

The sub-tick blend factor is timing-derived. **That is acceptable here only because it is a visual
weight clamped to [0,1]** which never reaches a pointer, an index or engine state: if the estimate
is wrong the result is one slightly wrong frame, and no state is corrupted. It is **not** precedent
for a timing-based correctness argument anywhere else in this stack.

## 7. Gates and levers

Lever: `tagpu_lerp.on` next to the exe, off by default, with the delay `D` and the mode (A/B) as
knobs in a `.cfg` alongside the `tagpu_classicpp.cfg` idiom. A/B-able live, which the taste question
needs.

| gate | asks | how |
|---|---|---|
| **0 — taste** | does smoothed TA look better than the authored stepping? | [tacob](tacob-design.md) A/B on ARMPW at 60 fps — **instrument built, §7a; the owner decides, not a measurement** |
| **1 — coverage** | how often would B fall back? | `tools/cob_lookahead.py` — **run, §5** |
| **2 — parity** | with the lever off, is output unchanged? | bit-identical `posed_pose` output |
| **3 — cost** | what does it cost at scale? | `tools/gatec.sh` + `scenarios/walk-gatec.json`, the 200-unit fixture [G16](gpu-posing.md) step 7 built |
| **4 — sim untouched** | did anything reach the simulation? | `cobtrace` + `posedump` unchanged, lever on and off |

### 7a. Gate 0's instrument — built 2026-09-09

`tools/tacob-edit.html` gained a `posemode` select — **stepped / smooth / A|B side by side** —
plus a `body` checkbox. Run it with `tools/tacob serve armpw`, press play, pick `A|B`: two copies
of the model, left stepped and right blended, **driven from one clock and one pair of frames**.

Three things the instrument had to get right, or the comparison would not have been a comparison:

- **The ring is dense.** The page polls every 66 ms against a 30 Hz sim, so head advances about
  two ticks a poll and drawing head directly *skips keyframes* — which would have flattered
  interpolation by smoothing over poses it never sampled. Gaps are backfilled and a playback clock
  walks the ring at the sim rate.
- **Only the blend differs.** Both models read the same clock and the same frame pair; `stepped`
  is byte-for-byte what the page did before. The timeline is not a variable.
- **The shortest arc is not optional.** `[MEASURED]` ARMPW's own walk crosses the TAang seam:
  at tick 237→238 a piece goes 176.6° → −178.4°, which a naive lerp renders as a **355° spin the
  wrong way, once per stride**. `alerp` makes it the 5° step it is. Any implementation of §3.4 that
  skips this will look broken in a way that has nothing to do with the idea being tested.

**The director's own path had to be fixed first, and it was not a rendering bug.** `[MEASURED
2026-09-09]` `ground` — the motion sketch every walking class uses — is a shuttle: a leg forward,
a **1 s dead stop**, then the return leg with the heading flipped `0 → 0x8000` **in a single
tick**, and another 1 s stop. Its own docstring says the path "was never measured against a moving
unit". Against a Peewee that is a ~90 wu leg (the radius is clamped to `0.5 × reach`), so the unit
never gets going, stops twice a cycle, and reverses direction *without ever turning* — which reads
exactly like walking backwards, because it is. None of that is the interpolation, and all of it
drowns the thing being judged. `loop` is a new path added beside it — a wide ground circle sized
from the class's speed (≈12 s a lap), tangent heading, never stopping — and it is what the page
now selects by default. **`ground` is untouched**, because the nine trace fixtures replay against
it: `tacob run --all` is 9/9 byte-identical after the change.

**Three viewer bugs the first A/B session found, none of them the interpolation.** `[MEASURED
2026-09-09]`

1. **The model faced 180° away from its direction of travel — always.** `scene3` maps world to
   scene as `(-x, y, -z)`, a half turn about Y, and the page applied `body[1]` to the model's yaw
   *without* that half turn. Measured against the loop path: a constant **174.3° error with 0.0°
   spread** (the residual is the half-interval of the sampling). `+ π` on the yaw takes it to
   1.9°. This predates the A/B work and is why the walk read as backwards; on the old shuttle path
   it cancelled on the return leg, which is exactly the "only part of the animation" symptom.
2. **The A|B separation was applied inside the yaw rotation.** With both copies parented under the
   rotating anchor, every heading change swept them through an arc of radius `abGap` — so the
   outer copy jumped laterally by far more than the inner one, and on the old shuttle's one-tick
   180° flip it crossed a half circle in a single frame. The rigs now carry the gap **above** the
   yaw, so it is a pure scene-space offset.
3. **The playback clock modulated playback speed.** Correcting `playTick` toward the ring's head
   every frame saturates whenever the page's assumed tick rate and the server's differ at all, and
   the correction then surges at the poll frequency. Trimming a **rate multiplier** instead lets
   the clock lock to whatever the server is really doing, at any speed setting and on a loaded
   machine. Measured under the headless harness: camera jitter 8% → **2%**.

   ⚠ **The magnitude of that one is not trustworthy, and the cause I first wrote for it was
   wrong.** The numbers came from headless Chrome under `--virtual-time-budget`, where rAF runs on
   *virtual* time while the server runs on the wall clock, so the two rates are unrelated by
   construction — which is what saturated the old correction there and inflated the jitter. The
   real server is **29.83 ticks/s at 1×, 99.4% of 30 Hz** `[MEASURED 2026-09-09, 179 ticks over
   6.00 s]`, so in a real browser the old control law would have had to trim 0.6%, well inside its
   own cap, and would not have saturated. The rate-locked clock is kept because it is the correct
   control law and costs nothing — **not** because a ±20% surge was ever demonstrated in a real
   browser. The jerk the owner reported is attributed to items 1 and 2, which were measured
   against the server's own data and do not depend on the harness.

Worth stating because it cost a session: the clock was twice "fixed" by reasoning (a bigger
buffer, then gentler easing) before anything was measured, and then measured under a harness whose
clock is not the product's — so the third "fix" was aimed at an artifact. **A measurement is only
as good as the thing it was taken on**: items 1 and 2 were taken against the server's own data and
hold; item 3's was taken against virtual time and does not.

### 7b. Walk-forward — the walk cycle and nothing else

`forward` `[MEASURED 2026-09-09]`: straight ahead at a fixed heading, never stopping, never
turning, no weapon events and no slots — the scripts started over a whole run are
`Create, MotionControl, StartMoving, walk` and nothing else.

**Gating the weapon events was not enough, and the reason is worth keeping.** Switching path
mid-run left the unit's *script* in whatever state it had reached: once anything has aimed,
ARMPW-shape `MotionControl` keeps calling **`walklegs`** — the aim-while-moving cycle, torso locked
for the gun — because the `aiming` static is still 1, and nothing the director stops will clear a
static the script only clears on its own events. So a path change now **restarts the run** rather
than retargeting it, and `forward` additionally empties the director's slots. Measured after: a
mid-run switch produces `Create, MotionControl, StartMoving, walk` and no `walklegs`.

**The ground was the treadmill, not the walk.** `[MEASURED 2026-09-09]` `GridHelper`'s first
colour is the **centre cross**, and it is the lighter of the two — so snapping the grid under a
walking unit moved a distinctly-coloured landmark back one cell every 40 units, a reset every
**1.1 s** at 36 wu/s, forever. "It never stops" and "it shuttles between two points" were both
true at once: the first of the simulation, the second of the picture. Both colours are now equal,
which makes the snap exactly periodic and invisible, and the span is 4000 with fog eating the edge
— that fade is the horizon.

**Camera presets reuse `tools/ta3do`'s `VIEWS` verbatim** — `front`, `side` (the unit's right),
`back`, `top`, `quarter`, plus `free` — rather than inventing a second convention for the same
idea. That table defines its angles relative to the model's front, so a preset **tracks the unit's
heading** and keeps meaning what it says on a turning path. Switching keeps the current orbit
distance so it never costs a zoom, and a drag returns the select to `free` so the dropdown cannot
disagree with the camera. Verified numerically: at `front` the camera sits **0.0°** off the model's
forward. The A|B gap moves to the **camera's right vector** (off the camera's world matrix, so it
survives looking straight down) — otherwise `side` or `front` would put one copy exactly behind the
other; `abRight · model-forward` measures **0.00**, exactly across the walk, and A still lands
screen-left.

**`commandfire=1` weapons are not driven at all.** ARMCOM's `Weapon3` is `ARM_DISINTEGRATOR` — the
D-Gun — and the director aimed and fired every slot the FBI listed, so the commander D-gunned on a
loop forever, which no unit does in a game: `commandfire` is TA's manual-fire tag and the engine's
own `AutoAim` never calls those scripts. `slots_from_fbi` now skips them. Measured on the shuttle
path afterwards: `AimPrimary`/`FirePrimary` only, no `*Tertiary` at all.

**It is genuinely unbounded.** 63 s of continuous walking took ARMCOM from 1100 to 3035 world
units at a steady **35.9 wu/s** with no wrap, clamp or stop; position is a Python int in 16.16, so
nothing overflows. The viewer snaps the ground grid to its own cell under the unit (measured max
offset 22.7 of a 40-unit cell), so the unit never runs off the 1600-unit grid and the lines stay
world-aligned as motion cues. The one real limit is float32 in the scene graph, which is ~0.008 wu
of precision after an hour of walking and only matters after many hours.

**The director's pace came from a per-class constant, not the unit.** `[MEASURED 2026-09-09]`
`CLASS_PLANS` carried one speed per class — 60 wu/s for every kbot — so ARMCOM walked at ARMPW's
pace, **67% too fast**, and the walk cycle slipped against the ground accordingly. The FBI has the
real number per unit: ARMPW `MaxVelocity` 1.8, ARMCOM 1.2, ARMSTUMP 1.7. The director now reads it
(`fbi_speed`), and `TurnRate` bounds the loop so the circle is never tighter than the unit could
hold — for stock units that guard never binds (ARMCOM: 36 wu/s against 3.0 rad/s is a 12 wu circle
where the sketch asks for 72). Measured after the change: **1.199 wu/tick against the FBI's 1.2**.
The `/state` reply and the page's status line now name the pace being used, so it is readable
rather than something to re-derive.

**The ratio between units is exact; the absolute scale is inferred.** `MaxVelocity × SIM_RATE`
puts ARMPW at 54 wu/s against the 60 that was hand-chosen for kbots here, and that correspondence
is the only anchor — TA's unit for the field is not documented in these notes and has not been
measured against a moving unit in the game. Treat the relative pace as right and the absolute as a
good guess.

**And `classify` called a commander a tank.** ARMCOM's category is
`ARM commander LEVEL10 WEAPON NOTAIR NOTSUB CTRL_C` — no `KBOT` word anywhere, so a plain `KBOT`
test dropped both commanders to the tank plan. `{"KBOT", "COMMANDER"}` fixes it. The nine fixtures
are unaffected by any of this: `run_class` replays from the recorded `cobtrace.log` and never
builds a Director, which is why `tacob run --all` stays 9/9 across all three changes.

The `body` checkbox interpolates the unit's *translation and yaw* as well. That is §8, not what the
pose pass would ship — it defaults on so the leg difference is what you see, and turning it off
shows the honest result of interpolating pieces alone (smooth legs on a stepping body), which is a
real risk of §2 landing by itself.

**Order of work.** Gate 0 first, in tacob, before any DLL work: `tacob serve` already runs the
verified VM and poses the real glTF with one node per piece, so an interpolation toggle in
`tacob-edit.html` costs a page change and answers the only question that decides the rest. Then A
behind the lever with gates 2–4. Then B, only if gate 0 likes the look *and* the 66 ms actually
shows on screen.

## 8. Future work — the rest of "smooth"

Everything on this page is future work; these are the parts not even designed.

- **Unit turn rate and body rotation.** `O3_BTURN` is the unit's own yaw/pitch/roll folded into the
  base piece, updated by the sim. Smoothing it is a separate feature with a separate failure mode
  — it changes the *unit's* apparent facing, which selection, aiming and the player's read of a
  formation all key off. It is also the one most likely to be worth the most: a tank's hull snapping
  between headings is more visible than a Peewee's knee.
- **Position between ticks.** Unit world position comes from the unit struct, not
  `PrimitiveStruct`, so it is untouched by §2. Interpolating it is the classic render-side
  smoothing and would interact with §4-A's delay: delaying the pose but not the position phase-shifts
  the legs against the translation. Either both or neither.
- **Turn *rate* as a sim property** — as distinct from smoothing its display — is an engine change,
  not a render one, and would have to ship identically to every peer.
- **Higher sim rate.** The other way to get smooth motion is to raise the 30 Hz tick. That is a
  simulation change with network consequences and is not on any road here; noted so it is not
  mistaken for an alternative to this page.

## 9. Open questions

1. **The sub-tick clock.** `main+0x38A47` gives the tick number, but ticks are not wall-clock-even
   — game speed changes and pause both break a naive estimator. Needs a measured tick duration with
   clamping; the pause and speed-change behaviour is undesigned.
2. **Runtime static/local reads are unproven.** §5's near-zero fallback claim for locomotion rests
   on reading `cob+0x10` and record `+0x24` in the game. Both are documented; neither has been read
   by our code.
3. **Mods are unmeasured.** §5 is the stock corpus. A mod that put a `get` inside a walk cycle falls
   back — correctly, and invisibly.
4. **Does it look right?** Strides were authored as poses and were never distance-synced to ground
   speed, so smoothing may expose foot-sliding the stepping currently hides. Gate 0, and the reason
   nothing else starts first.

## 10. Sources

- **This repo** — `tagpu/ddraw/src/tagpu_native.c` (`posed_pose`, the two field reads and the
  arena), `tools/cob_lookahead.py` (§5's harness), `tools/tacob` (the VM, the decompiler and the
  opcode table it reuses), `tools/gatec.sh` + `scenarios/walk-gatec.json` (gate 3's fixture),
  `research/notes/evidence/cobtrace/kbot/` (the ARMPW cadence).
- **The engine** — [exe reverse engineering](exe-reverse-engineering.md) §"The COB engine" for the
  records, the allocator, `COBEngine_DoScriptsNow 0x4B0D60` and the stepper `0x4B1C00`;
  [file formats](file-formats.md) §2 for the opcode set, the piece transform and §2.6 for the
  keyframe finding this page rests on.
- **The pass** — [GPU posing](gpu-posing.md) for what `posed_pose` is, its lifetime table and the
  decisions §3 inherits.
