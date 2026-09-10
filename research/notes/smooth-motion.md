# Smooth unit movement and animation

***Option A is built and all five gates are answered** (2026-09-09), and it is **not shipped**:
`tagpu_lerp.c` behind `tagpu_lerp.on`, off by default and absent from `tagpu_opt.c`'s
play-default table, so only that file arms it. Gate 0 passed on the tacob viewer (§7a) — the
owner watched stepped against smoothed side by side and the smoothed walk looks good — but that
was a **model** of the change; **nobody has looked at option A itself in the game**, and that
taste question is the one thing still open. Gate 2 parity and gate 4 sim-untouched both PASS
(§7e, §7g), gate 3 put the cost at **0.56 ms a frame for 240 posed units** (§7f) — **that is the
first cut's `double` blend; the shipped blend is 16.16 fixed point and its frame cost is not yet
re-measured (§7i)** — and §7h shows the blend is measurably live. The concrete piece is interpolating COB-driven piece poses toward
the next keyframe so models animate at render rate instead of at the sim tick — which is **60 a
second in a skirmish, not 30**, one of the three things this build got wrong first (§7d). The
wider topic — unit turn rate, body rotation, position between ticks — is §8. Companion pages: [file formats](file-formats.md) §2 (the COB format and the
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

**One part of locomotion is already interpolated: the stop.** `StopMoving` only clears a static;
`MotionControl`'s idle branch then eases the legs to rest with `speed`, one-shot, and the stride
in flight finishes first because `call-script` blocks. Measured on ARMCOM: **10 ticks, 0.33 s**,
at the 6.67°/tick its `speed <200>` implies — [file formats](file-formats.md) §2.6 carries the tick
table. **There is no start transition**; `walk` opens with `TURN_NOW` and snaps in. So the blend
this page proposes must leave the stop alone and hold the whole burden of the start, which is the
asymmetry to look for when judging whether it is worth shipping.

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
| **0 — taste** | does smoothed TA look better than the authored stepping? | **PASSED 2026-09-09** — the owner's verdict on the A\|B viewer (§7a), which is the only thing that could decide it |
| **1 — coverage** | how often would B fall back? | `tools/cob_lookahead.py` — **run, §5** |
| **2 — parity** | with the lever off, is output unchanged? | **PASSED 2026-09-09** — `tagpu_posecrc.on`, §7e |
| **3 — cost** | what does it cost at scale? | **MEASURED 2026-09-09 — 0.56 ms/frame at 240 posed units**, §7f |
| **4 — sim untouched** | did anything reach the simulation? | **PASSED 2026-09-09** — `cobtrace` on `scenarios/walk-lerp.json`, §7g |

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

**Order of work.** ~~Gate 0 first, in tacob~~ — **done, and it passed**; §7a is what was built to
answer it and §7c is what it cost. ~~Next is option A behind the lever, with gates 2–4~~ — **built
2026-09-09**, §7d, and gates 2–4 are answered in §7e–§7g. Then B, only if the latency actually
shows on screen — gate 1 says the coverage is there for it (§5), so the decision is about latency,
not feasibility. **Nobody has yet looked at option A in the game**; gate 0 was passed on the tacob
viewer's model of it, and the taste question for the real renderer is open.

### 7c. What answering gate 0 actually cost — nine bugs, none of them the interpolation

Every one was found by measuring the running thing, and all nine were in the *instrument*, not in
the idea being tested. Kept because the pattern is the lesson: **an instrument that has never been
looked at closely is not evidence, and most of these produced a picture that flatly contradicted a
correct measurement.** `[MEASURED 2026-09-09]`

| # | what was wrong | how it showed up |
|---|---|---|
| 1 | The viewer set the model's yaw from `body[1]` raw, but `scene3` maps world to scene as `(-x, y, -z)` — a half turn about Y | Units faced **180° away from travel, always**: a constant 174.3° error with 0.0° spread. On the old shuttle it cancelled on the return leg, so it read as "backwards only part of the time" |
| 2 | The A\|B gap sat *inside* the yaw rotation | Every heading change swept both copies through an arc of radius `abGap`, so the outer one jumped much further per step |
| 3 | `ground`, the motion sketch every walking class used, is a shuttle with two 1 s dead stops and a **one-tick 180° heading flip** | "It pauses and walks backwards" — it did, because reversing without turning *is* walking backwards. Its own docstring admitted the path "was never measured against a moving unit" |
| 4 | `CLASS_PLANS` carried one speed per class | ARMCOM walked at ARMPW's pace, **67% too fast**, and the stride slipped against the ground to match. The FBI has the number per unit: 1.2 against 1.8 |
| 5 | `classify` tested for `KBOT` in the category word list | ARMCOM reads `ARM commander LEVEL10 …` with no `KBOT`, so both commanders were driven as tanks |
| 6 | The director drove every weapon slot the FBI listed | ARMCOM's `Weapon3` is `ARM_DISINTEGRATOR`, so the commander **D-gunned on a loop forever**. `commandfire=1` is TA's manual-fire tag and the engine's own `AutoAim` never calls those scripts |
| 7 | `applyPieces` was dispatched on `mode === 'stepped' ? null : b` | In `ab` that is false, so **both** copies were smoothed and the side-by-side compared nothing. After: they differ by up to 35.14° on 70% of frames — the other 30% are pose *holds*, where correctly nothing blends |
| 8 | `GridHelper`'s first colour is the **centre cross**, and it is the lighter one | Snapping the grid under a walking unit moved a distinct landmark back one cell every 40 units — a reset every **1.1 s**, forever, which read as shuttling however far the unit really walked |
| 9 | The page pushed its own `path` select to the server on every load, defaulting to `loop` | Every reload silently replaced the chosen path with a 72 wu circle. Measured on the rendered position: `loop` reverses the drawn world z **1009 times in 2160 frames**; `forward` reverses it **0 times in 1200** |

**Two of my own diagnoses were wrong and are recorded as such**, because the way they were wrong
is the transferable part:

- The playback clock was "fixed" twice by reasoning — a bigger buffer, then gentler easing — before
  anything was measured, and then measured under **headless Chrome with `--virtual-time-budget`**,
  where rAF runs on virtual time and the server on the wall clock, so their rates are unrelated by
  construction. The ±20% surge that justified the rate-locked clock was an artifact of that
  harness. The real server runs **29.83 ticks/s at 1×, 99.4% of nominal**. The rate-locked clock is
  kept because it is the right control law, not because that surge was ever demonstrated.
- The pause-and-teleport was attributed to a ring wipe on slow polls. A 40-seed simulation of the
  clock under adversarial stalls produced **no backward jumps before or after** the change, and a
  wipe gives pause-then-jump-*forward* anyway. It was item 9. The hardening (monotonic playback,
  no wipe except on a real restart) is insurance, and the note says so rather than claiming a fix.

### 7d. Option A, built — `tagpu_lerp.c` (2026-09-09)

`[VERIFIED — the code is in the tree]` The lever is **`tagpu_lerp.on`**, a file beside the exe,
**off by default and absent from `tagpu_opt.c`'s play-default table**, so only that file arms it;
there is no default that could turn it on for a player who did not ask.

The insertion in `posed_pose` is what §2 said it would be — the two field reads, and one line:

```c
mv  = (const int*)(pr[i] + P_POS);
tn  = (const unsigned short*)(pr[i] + P_TURN);
if (lpos) { mv = lpos + i * 3; tn = lturn + i * 3; }
```

`lpos` is non-NULL only when the lever is armed *and* that unit has two usable samples, so with the
lever off the function reads the live fields with the code it always had. Everything else lives in
`tagpu_lerp.c`:

| | |
|---|---|
| key | `(Object3do, nparts, level generation)` — a pointer alone would let a new unit inherit a dead one's poses, because slots and allocations are both recycled |
| table | 4096 records, open-addressed on the pointer, probe 8, swept 256/frame and dropped after 180 frames unseen |
| arena | fixed blocks of **48 pieces** × 2048 blocks × 2 banks = **3.4 MB**, less than the 4.7 MB pose arena beside it. 48 is above every stock model (36, ARMSCORP/CORSCORP) and below `TAGPU_PBMAXPIECE` 256, so a bigger model gets no history and draws stepped, counted as `big=` |
| banks | a new sample is written into the bank the record is *not* pointing at and the index flips — the "shift" costs nothing |
| line | `lerp=<blended>/<snapped> p=<ms> u=<weight>` on `native:`, and **nothing at all** when the lever is off |

**The degradation is a `return 0`, never a blend at weight 1.0** — and that distinction is not
pedantry. `a + (b - a) * 1.0f` is *not* `b` in floating point, so "blend with weight 1" would have
been off by an ulp on every piece of every unit and invariant 2's parity claim would have been
false before it was ever tested. Refusing hands the caller back its own untouched read.

**Three things the design had wrong.** The first two were found by the instrument and both are about the clock; the third by re-reading the code, which is worth saying because §7c's lesson is not *only* "measure":

1. **The sim tick is not 30 Hz.** `main+0x38A47` advanced **60 a second** on the gate fixture, not
   the 30 the engine's own `+clock` arithmetic implies: a skirmish starts at `GameSpeed`
   (`main+0x38A4B`) **20**, and the tick rate follows it. A hard-coded 33.3 ms would have run every
   blend at half speed and left the legs a whole stride behind the unit. The period is **measured**
   — the learned value reads `p=16.7ms` — which closes §9 question 1's first half. See
   [exe reverse engineering](exe-reverse-engineering.md) §"The simulation clock".
2. **The pair to interpolate is not "last tick and this tick".** A render slower than the sim sees
   several ticks arrive at once, and a unit that was off screen has not been sampled for many more
   — so the window to spread a sample pair over is however many ticks apart *that unit's* last two
   samples were, not one. The first cut tested `tick == prev + 1` and would have blended nothing at
   all on this fixture, where the render is at half the sim rate. The gap is per record, capped at
   8 ticks, and the period is learned from the observed interval **divided by the gap**.
3. **`u > 0` where it had to be `u >= 0`**, and it is a visible stutter rather than a nicety. On the
   frame a sample lands `s_now` *is* the record's stamp, so `u` is exactly 0 and the honest answer
   is "show PREV". Refusing there instead falls through to the live fields, which are **CUR** — so
   the unit would have jumped forward one tick and been pulled back on the very next frame, once
   per tick, forever. It was written as a NaN guard; `>= 0` rejects NaN just as well, and `u == 0`
   blends exactly (`ap[i] + (int)(d * 0.0f)` is `ap[i]`), so there was nothing to guard against.
   **Found by reading the function back, not by looking at it**, and every gate below was re-run on
   the corrected binary rather than on the one that had already produced numbers.

**What is deliberately NOT smoothed.** `pose_accum_body` — the path `hires_pose` and
`tagpu_posedump.on` take — is untouched, so replacement meshes still step and the sim oracle still
reports the engine's own fields. The unit's world position and `O3_BTURN` are §8. That asymmetry
has a visible consequence this page has to own: **the pose is one sample behind a position that is
not delayed**, so the feet slide against the ground by speed × the delay — for ARMCOM at 1.2 wu per
tick, under 2 wu. Stock strides were never distance-synced to ground speed either (§9 question 4),
so this adds to a slip that is already there rather than creating one; whether it reads worse is a
taste question and **nobody has looked at option A in the game yet**.

### 7e. Gate 2 — parity, PASSED `[MEASURED 2026-09-09]`

The oracle is new and general: **`tagpu_posecrc.on`** logs, once per unit per sim tick,
`in=` a CRC32 of every byte `posed_pose` reads and `out=` a CRC32 of every byte it writes.
`tagpu_posedump.on` dumps the *engine's* fields and `tools/tacob pose-check` diffs tacob's own
reconstruction of them; **nothing had ever watched the matrices this pass hands the GPU.**

It is joined on the **input, not the tick**, and that is the whole point. Two runs are not
tick-for-tick comparable — the gate-C fixture's move order goes over the wire and lands where it
lands — but `posed_pose` is a pure function of its input, so every `in` that appears in both runs
must carry the same `out`, whatever tick each run saw it on.

Two builds, the lever absent on both: the tree as it stands, against **HEAD plus the oracle and
nothing else** (built in the scratchpad, diffed to confirm the only change was the oracle).

| | |
|---|---|
| samples | 1502 / 1495 over a six-leg walk |
| distinct inputs | 476 / 477 |
| **joined on inputs seen by both** | **361** (75.8% of the smaller side) |
| **inputs producing a different output** | **0** |
| inputs producing more than one output *within* a run | 0 |

**The first attempt failed, and the failure was worth more than the pass.** One input in 1498 had
two outputs *inside a single run* — at tick 3595, the only odd tick in a stream of even ones. That
is not an impurity: the COB scripts run on the **game** thread while this one poses, so the fields
moved between the input hash and the loop that read them. It is [G16](gpu-posing.md) §2's accepted
residual, one tick of one piece, and it had never been given a number. The oracle now hashes the
input on **both sides** of the pose loop and drops the sample when they disagree, which turns a
false positive into a measurement: `raced=` 0 to 5 per ~1500 samples, **0 to 0.33%**, present in
the control build too and not attributable to this change.

### 7h. Is the blend actually doing anything? `[MEASURED 2026-09-09]`

The gates all ask whether the change *breaks* something. This asks whether it *works*, and it
falls out of the same `tagpu_posecrc.on` log for free.

`in` hashes the live engine fields; `out` hashes what `posed_pose` wrote. With the lever **off**
the output is a function of the input alone, so an input the log saw twice must carry one output.
With it **on** the output also depends on the sub-tick weight, so a repeated input must carry
*several* — one per weight the render sampled it at.

| | lever OFF | lever ON |
|---|---|---|
| samples / distinct inputs | 1676 / 624 | 1683 / 670 |
| inputs seen more than once | 144 | 162 |
| **of those, carrying more than one output** | **0 (0.0%)** | **104 (64.2%)** |
| most outputs for one engine state | 1 | **10** |

So the lever-off pass is exactly the pure function it has always been, and the lever-on pass draws
one engine state at up to ten different poses. `lerp=300/0 p=16.6ms u=0.84` on the `native:` line
says the same thing from the other side: **every** one of the window's 300 draws blended and none
snapped — which is also how the `u >= 0` fix above shows up, since before it every sampling frame
refused.

### 7f. Gate 3 — cost, measured `[MEASURED 2026-09-09]`

This gate has no pass mark to hit; it produces a number, and the number is the owner's to weigh.

**The first attempt was not a measurement and is recorded as such.** `scenarios/200v200.json` is a
battle: it kills units as it runs, so the two sides were taken at different points in the fight —
202 posed units against 136 — and the 325-vs-565 fps that came out of it says nothing at all about
the change. `scenarios/crowd-static.json` exists precisely because a battle cannot be paired: 256
units, one owner, no orders, nothing fighting, identical between runs.

Free-running (`tacli create --maxfps 0`, or both sides read the cap and the answer is 60 either
way), 1024×768 `ss=2`, the oracles disarmed, a 60 s window starting a fixed 20 s after the
scenario applies. **All four runs drew the same scene — `posed=240/32288tri` on every one of
them**, which is what makes the pair valid.

| pass | lever off | lever on |
|---|---|---|
| 1 | 294.98 fps | 254.99 fps (`lerp=71040/960`, `p=16.8ms`) |
| 2 | 299.98 fps | 254.99 fps (`lerp=68640/3360`, `p=16.0ms`) |
| **mean** | **297.5** | **255.0** |

| | |
|---|---|
| **cost** | **+0.560 ms per frame** at 240 posed units |
| per unit | **2.33 µs** (a stock model is ~36 pieces, so ~65 ns a piece) |
| as a share of a **60 fps** budget | **3.4 %** |
| as a share of *this* frame rate | 14.3 % — the less useful framing: at 297 fps the whole frame is only 3.4 ms |
| granularity | one `native:` line per 300 frames over 60 s, i.e. **±5 fps**; the 40–45 fps gap is well outside it |

**Read it in milliseconds, not in percent.** Fourteen percent sounds alarming and is an artifact of
measuring where the frame is already 3.4 ms long; what a player at 60 fps would give up is a third
of a millisecond per hundred units. For scale, [G16](gpu-posing.md) step 7 bought **2.3 ms a frame**
by deleting the CPU emitters, so this spends about a quarter of that win back.

**The obvious optimisation was not taken for this gate**: the position blend goes
through a `double` per component (`(double)d * u`) so that no pair of endpoints can overflow the
subtraction. A `float` path with the wide subtraction kept, or a 16.16 fixed-point multiply, would
remove most of it. Left alone deliberately — this is the first cut, and a correctness-first blend
that costs 2.33 µs is the right thing to measure before tuning it. **It was taken afterwards;
§7i is the follow-up, and the 0.560 ms above is the FLOAT blend's number, not the shipped one.**

### 7g. Gate 4 — sim untouched, PASSED `[MEASURED 2026-09-09]`

`scenarios/walk-lerp.json` is new and exists because the gate-C fixture cannot answer this:
its orders come from the shell, so two runs are several ticks out of phase and every trace differs
for reasons that have nothing to do with the code. The new fixture carries its order, and it has to
be a **patrol** — `ORDERS_NewMainOrder2Unit 0x43AFC0` *replaces* the main order rather than
queueing it, and drops one whose target is within ±16 wu of the standing one (the tolerance test at
`0x43B006`), so a list of six move legs collapses to its last, which is the start point, and the
unit never takes a step. That was measured the slow way: six orders "issued, 0 failed" and a
commander that stood still for 55 seconds.

**The sim is deterministic, and that was established before it was relied on**: two runs of the
same build and the same lever produced **1084 COB events, byte-identical once the tick column is
dropped** — same order, same arguments, same `rand` draws — with a constant +2 tick offset from
when the scenario applied. `tagpu_cobtrace.on` records every thread start, return, kill and RNG
draw on the game thread, so it is the simulation's own fingerprint.

The result, lever ON against lever OFF on the same build, filtered to the walker: the lever-OFF
run's **273-event trace occurs VERBATIM inside** the lever-ON run's 675 (a longer capture — the
same patrol, more cycles of it). Same events, same order, same arguments, same `rand` draws.

- The **AI is the one source of noise** and it is not ours: the CORE keepalive is AI-controlled and
  in one run it decided to walk and to build a CORFMKR, which is why the comparison filters to the
  scripted unit. Worth fixing in the fixture (a structure keepalive has nothing to decide); it does
  not affect the result.

**A harness trap worth writing down, because it cost half an hour.** After a dozen rapid
`scenario load --restart` cycles the instance's **wineserver wedges**: every subsequent launch dies
with `exited during launch before showing a window (no ErrorLog.txt)`, the DLL is fine, and nothing
in the log says why. Killing the wineserver **for that prefix alone** (match `WINEPREFIX` in
`/proc/<pid>/environ`, never `pkill wineserver` — that would take every other instance and the
human's own session with it) clears it and the next launch is normal. It looks exactly like a DLL that will not
load, which is the wrong thing to go and debug.

The structural half of the gate is stronger than the measurement anyway: the module writes to its
own statics and to nothing else, and the invariant-1 read-only rule is checkable by inspection —
there is no write to `PrimitiveStruct` anywhere in the diff.

### 7i. The blend became fixed point — the x87 control word was the cost `[2026-09-09]`

§7f left the `double` multiply in place deliberately and named the two ways out. The 16.16 one is
now taken, and the reason it was worth taking is **not** the multiply:

**This target has no SSE.** C requires a float→int conversion to truncate toward zero; the x87
rounds to nearest; so GCC brackets every `(int)` of a float with an `fnstcw` / `fldcw` pair to
change the rounding mode and another to change it back. The first cut did **two** such conversions
per iteration — the position and the turn — so the inner loop carried **four `fldcw`**, each a
serialising reload of the whole x87 state, around roughly ten cycles of real arithmetic.

**Established from the compiler, with this makefile's own flags** (`-O2 -std=c99`,
`i686-w64-mingw32-gcc`), by compiling the two loop bodies side by side:

| | x87 instructions | of which `fldcw` | total instructions |
|---|---|---|---|
| `(double)d * u` and `(float)w * u` | 13 | **4** | 65 |
| `(d * w16) >> 16` and `(t * w16) >> 16` | **0** | 0 | 63 |

The **total instruction count barely moves** (65 → 63) and that is the point: the win is not
fewer instructions, it is that none of the remaining ones serialises the pipeline. Anyone
re-deriving this by counting instructions will conclude there was nothing to win.

**What the weight costs now.** One float→int conversion per *unit* instead of two per piece per
axis. `u` is already known to be in `[0,1)` and multiplying a float by 2^16 only moves the
exponent, so the product is exact and `w16` cannot exceed 65535 — but the bound is **clamped
rather than argued**, because the turn multiply has only 32767 of headroom (`t` reaches +32768,
and `32768 × 65535 = 2147450880` against an `INT_MAX` of 2147483647, so a `w16` of 65536 is
signed overflow). Two instructions a unit buy an invariant that a later change to the refusal
above, or to the weight's scale, cannot silently break. See CLAUDE.md, *Fixes must be safe by
construction*: this is a bound, not a "the weight is never that big" argument.

**Resolution and rounding.** 16.16 gives the weight 1/65536 of a tick, orders of magnitude finer
than a piece moves in one tick. The position delta stays a **wide** multiply, so no pair of
endpoints can overflow it whatever a mod puts in those fields. `>> 16` floors where `(int)`
truncated toward zero, so a negative delta can land one LSB — 1/65536 of a world unit — lower
than the float version did. That is on a path with no parity requirement: invariant 2 is about
the lever being **off**, which never reaches this function.

#### The frame cost has NOT been re-measured, and here is exactly how far it got `[GAP]`

**§7f's 0.560 ms is the float blend's number and is now stale. The fixed-point blend's number is
not yet established.** The attempt is recorded because the conditions, not the change, defeated it:

- The fixture reproduced §7f's exactly — `crowd-static`, 1024×768 `ss=2`, `--maxfps 0`,
  `posed=240/32288tri`, and the same blend volume (`lerp=71040/960`, the identical reading §7f
  quotes), at the same `GameSpeed` 20 / 64 ticks a second. So the work being timed is like-for-like.
- The lever polls live every 30 frames, so this was run as an **interleaved live A/B in one
  process** — off/on pairs back to back — which is strictly better than §7f's paired launches:
  no relaunch, no sim divergence, no different point in a fight.
- **Three other TA instances were running on the reference setup throughout**, and the baseline
  drifted from 267 fps to 183 fps *during* the run as they ramped. Eight off/on pairs gave
  per-pair costs of 0.068, 0.120, 0.127, 0.129, 0.156, 0.236, 0.260 and 0.461 ms — a 7× spread
  that tracks the load, not the lever.

**What that does and does not support.** Every pair came in **below** §7f's 0.560 ms, and the
median is around 0.14 ms; the direction is not in doubt. But a 7× spread is not a measurement,
and §7f itself is the precedent for saying so rather than quoting the mean — *"the first attempt
was not a measurement and is recorded as such"*. **Do not quote a speedup factor from this
section.** To close it: re-run the same interleaved A/B with no other instance on the machine,
and put the number in §7f's table beside the float one.

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
