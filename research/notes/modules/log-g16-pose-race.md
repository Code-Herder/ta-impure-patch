# Log — the pose race (G16)

The pose race and the guard that closed it, removed by G16 step 8.

*A frozen record: nothing new is written here. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.9 The pose race, and the guard that closed it — HISTORY (removed by G16 step 8, 2026-09-09)

> ⚠ **None of this is behaviour any more.** The guard, the rest-equality detector, the
> reconstruction, `posewatch` and the levers `tagpu_posefix.off` / `tagpu_posewatch.on` /
> `tagpu_poserecon.on` were deleted with the CPU emitters that were the only things that reached
> them, and the `native:` line's `posefix=` / `guard=` / `rest=` / `norecon=` / `errmax=` fields
> went with them. **Nothing reads `prim+0x22` now** — §2.11 builds the pose from the FIELDS — so
> the race described below cannot happen rather than being detected and worked around.
>
> The section is kept whole because it is the evidence, not the behaviour: it is where the
> artifact was characterised, where the engine's two-stage repose was established, and what every
> later gate was measured against. Read it as the record of a bug that no longer has a mechanism.
> What replaced each of its parts is [GPU posing](../gpu-posing.html) §4's refusal ledger; what is
> left of the residual is named at the end of §2.11.


**The bug.** A walking commander showed one-frame pops: for exactly one presented frame the unit
was drawn in its **unrotated rest orientation** — upright, front-on, no body yaw — and then
snapped back (~1400 changed pixels at 2× zoom, three frames of a 62-second walk). Zoom is an
amplifier and not the cause: the event *rate* is the same at 1× and 2×, and zoom multiplies each
event's on-screen size about fourfold in pixels.

**The cause, and why nothing else can produce it.** The engine rewrites every posed vertex buffer
`prim+0x22` **in place, on the game thread, in two stages** — first `rep movs` of the node's rest
vertices back over it for the whole piece tree (`0x45ACC1`, `0x45B030`), then the compose that
puts the piece turns and the body turn into them (`0x45B0A0` → `0x45B150`), one vertex at a time.
The native unit pass gathers on the **render thread** and reads that buffer live. The only code in
the engine that ever writes rest vertices into `prim+0x22` is that reset, so a frame that draws
the unit at rest is a frame that read between the two stages. Full derivation and every address:
[engine map](../exe-reverse-engineering.html) "The repose, and the window it leaves open".

**The interlock.** `Object3do+0x08` is set to 1 before the reset and cleared only after the
compose returns, and the rewrite is entered only when it is non-zero — so it brackets the window
exactly. `emit_geom` and `emit_slant` read it on either side of each piece's vertex copy (two
`int` loads and a compiler barrier; x86 does not reorder loads with loads, so nothing stronger is
needed). If it was set on either side the whole unit is re-emitted from the **pose fields** —
`pose_accum`'s reconstruction, with **all three** cached body words folded into the base
piece's turn the way `0x45B0DB` folds them (`pose_dump`'s `err=` is built from the same
reconstruction since 2026-09-08, where it used to apply the heading alone — though not the same
number: `recon_err` skips pieces whose visible bit is clear and compares `recon_prim`'s
16.16-rounded output, `pose_dump` reports every piece including hidden ones, in float) — written into the same 16.16 representation the engine's buffer holds, so
both emit paths consume it with the arithmetic they already had.

The flag is *also* 1 while the buffer is merely **stale** (a COB `move`/`turn` the next `DrawUnit`
has not composed yet), which is most of what trips the guard and would have been safe to draw.
Nothing in the struct tells the two apart, and the reconstruction is the pose the engine is on its
way to, so the guard does not try: it takes the fields whenever the flag says the buffer might be
moving.

**Why the fallback is not itself a change.** Forcing it for every unit on every frame
(`tagpu_poserecon.on`, a measuring lever) renders **byte-identical** to the engine-buffer path —
0 differing pixels of 1920×1080 on a parked commander, and the same change bbox as two
consecutive engine-path shots on a scene with a spinning radar dish and a solar collector, i.e.
the dish's own motion and nothing else. The oracle below reads the disagreement between the two
as `0.00` model units in every five-second window that contains no trip.

**The evidence, from inside the DLL.** `tagpu_posewatch.on` arms an oracle that, once per unit per
frame, rebuilds the pose from the fields and reports the largest disagreement with the engine's
buffer in model units, with the frame number and the dirty flag read on either side. A stale
buffer reads a unit or two out; a buffer caught mid-rewrite reads the model's own size out — the
readings run to the model's own size — 34.00, 38.63, 34.16, 33.03, 32.96, 32.82, 32.69,
32.00, 25.23, 23.18, 22.24. **Every one of them had the dirty flag set on both sides**, which is
the property the guard depends on, and the guarded pass reported the same event on the ones it
sampled. `tagpu_posefix.off` leaves the guard measuring and draws the engine's buffer anyway,
which is the baseline the fix is measured against; the guard deliberately finishes its walk rather
than bailing at the torn piece, so that baseline is the emission the pass made before it existed.

⚠ **`[2026-09-08]` Gate A ran that oracle over a 69-unit screen inventory, and two things came
back.** ([GPU posing §0b](../gpu-posing.html), the numbers in
`research/notes/evidence/posewatch/gate-a-2026-09-08.txt`.)

1. **The guard's bracket holds over everything, not just the commander.** 27142 `posewatch:` lines
   across all eight classes and both extremes of stock geometry, and **every one of them reads
   `dirty=1/1`** — not one large `err` with the flag clear on both sides, which is the one reading
   that would say the bracket is not the whole window. `norecon` is 0 in every window, so no
   model's piece tree failed to rebuild either.
2. **A handful of STRUCTURES sit at rest permanently, and this pass has been carrying them the
   whole time.** `rest=` — the rest-equality detector, which is supposed to read 0 in play — runs
   at tens of piece-reads per frame on a static base, from three to six units whose posed buffer
   is byte-equal to their own rest vertex arrays for the entire session with the pose flag set.
   Named by peeking `Object3do+0x0C → unit+0x92`: "Gaat Gun", "Sentinel", "Solar Collector" (both
   sides), "Vulcan", "Scorpion", "Wind Generator", and one aircraft, "Hurricane". Their `err` is
   therefore the model's own size (39–62) for as long as they are on screen, and `posefix` draws
   them from the reconstruction **every frame** rather than occasionally — which it does
   correctly, which is why nothing looked wrong. **Why the engine leaves those buffers at rest is
   not established.** It is a fact about the engine's buffer, not about our reconstruction, and
   G16 stops reading that buffer at all.

**Reproducing it costs scheduling pressure, not zoom.** The window is microseconds wide per unit
and opens ~30 times a second, so on an idle machine with cores to spare a walk of a minute usually samples it
never. Pinning the game to one core and putting spinners on that same core — its own render and
game threads then have to timeshare, which is what a loaded machine does to a player — brings it
to a handful of readings a minute. `_local` is not involved; a single core is used and nothing
else on the machine is touched.

**The regression, on the walk fixture** — Two Continents, one ARMCOM, static camera at
`eye (1818, 850)`, zoom 2×, three south legs over 62 s, the transient detector run over the world
band only (the frame's top carries the engine's message log, which scrolls on its own and
produced the largest transient of one capture without a unit ever being drawn wrong):

| capture | > 350 px | > 500 px | > 1000 px | worst |
|---|---|---|---|---|
| before the guard | 5 | **3** | **2** | 1403 px, the rest-pose draw |
| guard on, run 1 | 7 | 0 | 0 | 475 px |
| guard on, run 2 | 7 | 0 | 0 | 492 px |
| guard on, run 3 | 0 | 0 | 0 | 294 px |
| + rest-equality, run 1 | 0 | 0 | 0 | — |
| + rest-equality, run 2 | 0 | 0 | 0 | — |

⚠ **`[2026-09-09]` Those rows were driven by hand and their leg geometry was never written
down**, which is why re-running the protocol for G16's Gate C could not reproduce the `> 500`
column: the fixture built to replace them (`scenarios/walk-gatec.json` + `tools/gatec.sh`, at
1920×1080) walks a more energetic leg and reaches 520 px **on this same guarded path**. Treat the
`> 500` numbers above as a property of the original fixture, not as a threshold that transfers.
The `> 1000` band — the only one the rest-pose draw reaches — is the part that does transfer, and
it is 0 on every run of both fixtures. [GPU posing](../gpu-posing.html) §4 step 7 carries the G16
table and the two caveats.

The `> 350` band does not move and is not meant to: it is the walk itself — a leg swing or a fast
yaw passes the "differs from both neighbours while the neighbours agree" test, and every ranked
frame up to ~600 px opened as ordinary animation. What goes to zero is the band only a wrong pose
reaches.

**Cost.** Per piece per unit per frame on the common path: two `int` reads and a compiler barrier
for the flag, plus — for the rest-equality test — a second stream of the piece's `nvert × 3`
words XOR-accumulated inside the copy loop that already runs, and one `IsBadReadPtr` on the node's
vertex array. That probe is the one avoidable part: the array is per model **type** and immutable,
so its readability could be resolved once per type the way `pmap_for` caches the piece map, and is
not. The reconstruction itself runs only on a trip. 60.0 fps before and after on the walk fixture,
which is one unit — **no measurement exists at 200 units**, for this or for any of the options
below. *[MEASURED 2026-09-09, once `tacli` stopped forcing the frame cap: on 200v200 at 1920×1080
with the sim paused, 281 units and 76 wrecks on screen, this path runs at **184.0 / 180.0 fps**
against the posed program's **313.0 / 306.9** — 1.70×. And it gets that while **truncating**:
`49152 verts VERTEX-BUDGET-HIT`, so it is drawing less than the scene asks for. §2.11 and
[GPU posing](../gpu-posing.html) §4 step 7.]*

**A detector, not a lock — the residual window.** `Object3do+0x08` is a flag, not a sequence
number, so "zero on both sides of the read" means *no rewrite started and finished across the
read*, which is not the same as *no rewrite touched it*. A whole dirty-to-clean cycle falling
strictly between the two flag loads is missed. How much smaller that is than the bug it replaces
is the point: **before, any overlap at all between the pass's read and the rewrite produced the
artifact** — which is exactly what the captures show happening — **and now the rewrite has to be
strictly contained inside one piece's vertex copy.** From the trip rate (one in ~29 000
unit-frames on an idle reference setup, at the ~30 Hz the COB writes a piece) the dirty interval is around a
microsecond; one piece's copy is tens of nanoseconds. So a miss needs the *render* thread stalled
inside those tens of nanoseconds for at least the whole interval — and most of what that lets
through is benign anyway, two composed poses one tick apart mixed together. The rest-pose read
specifically needs the stall in the gap between a piece's last vertex load and the flag load,
with the entire remaining compose finishing in it.

**The second detector, which has no timing hole — rest-equality.** The reset copies `node+0x24`
over `prim+0x22` **verbatim** (`rep movs`), so mid-reset a piece *is* its node's vertex array;
composed, it is that array through the accumulated transform. A piece that compares byte-equal is
therefore either mid-reset or standing at an exactly identity transform — and the reconstruction
is right for both, because at identity it reproduces the array itself. This tests the data rather
than the clock, so no scheduling behaviour changes its answer. The compare folds into the copy
loop the pass already runs (one more load and an OR per component, both arrays streamed once).

It is applied only where equality would be a **contradiction** — the body turn, the piece's own
`TURN` or `MOVE`, or its rest offset from its parent is non-zero, so the accumulated transform
cannot be the identity. Without that gate a model facing exactly north whose base piece sits at
the origin would compare equal every frame and take the reconstruction forever: correct output,
for no reason. The gate reads local fields only, so it is conservative by construction — a piece
whose own fields are all zero under a rotated parent is skipped and left to the flag. It catches
what the flag can miss, never the reverse, and the two run together.

**Measured, and it is specific.** Under the amplifier with the fix off and the oracle armed, a
62-second walk on the reference setup tripped the guard 16 times; the **one** window whose oracle reading was a
mid-rewrite buffer (`err=22.23` model units, `dirty=1/1`) is the **only** window where the
rest-equality counter moved (`rest=1`). The other fifteen trips were stale buffers — validly
posed, one tick old, not byte-equal to rest — and it stayed at 0 on every one. In steady play it
does not fire at all: 19 of 20 five-second windows at `rest=0` across two shipping-configuration
runs, at facing 90 and at facing 0, standing and walking. The twentieth is the scenario load,
where `0x45AEC0` initialises `prim+0x22` as a copy of the node's array and it genuinely is
unposed until the first repose — the substitution is right there too. `rest=` rides the `native:`
line beside `guard=`, and it is the useful number of the two: it says how much of what the guard
caught was the dangerous kind.

**What is still not closed by either detector**: the "rotated but not yet translated" intermediate
(`0x45B150` rotates every vertex of a piece before adding the parent origin to any of them) is
neither byte-equal to rest nor flagged if both flag reads miss it — it is displaced by the parent
origin rather than collapsed, so it is a much smaller error, but it is not detected on the data
side. Closing everything outright still needs one of the two expensive options: a **sequence
counter**, which the engine does not keep and which we would have to synthesise with detours on
all three repose sites (two are inlined mid-function, at `0x45ACB6` and `0x45ADA5`, so byte
patches rather than prologue detours); or a **full content check** — compare against the
reconstruction on every frame instead of only on a trip, which has no timing hole anywhere but
pays a `pose_accum` per unit per frame. The architecture that removes the question entirely is
**G16** in the roadmap: pose 3DOs on the GPU from a static mesh, the way `tagpu_hires_draw.c`
already poses replacement models — that path never reads `prim+0x22` and the race cannot happen
to it. *[DONE 2026-09-09, step 8. Neither expensive option was ever built, and neither is needed:
the question was removed rather than answered. What is left is a strictly smaller residual — a
pose mixed across one tick — named at the end of §2.11.]*

**Not closed by this.** The same live read is made by `tagpu_hires_draw`'s replacement-mesh path
through `hires_pose`, which reads the pose *fields* rather than the buffer and so cannot show the
rest pose — but it can show a pose mixed across two ticks, which nothing here measures. And the
guard says nothing about the *anchor*: the unit's 16.16 position is read without any interlock,
which is sound for a single aligned dword but has never been checked across the three of them.
*[2026-09-09: since step 8 EVERY unit is drawn the way this paragraph describes the hires path,
so what was a note about one pass is now the whole renderer's residual. Both halves of it are
still open and still unmeasured, and §2.11 carries them.]*
