# Fidelity: the lossy side, measured on size-battle.imprec

Area: quantization, sampling rate, what a puppet needs exact, takeover and verification once the
stream is lossy. Scripts and outputs: `research/fidelity/` (listed at the end). **MB here = 10^6
bytes**; recsize's "MB" are MiB (its 46.7 MB of unit deltas = 48.95 MB here; the file = 104.4 MB).
Sizes are column streams (per tick: slot gaps, then each field's residual from the line through
the unit's two previous values, zigzag varints) compressed per 300-tick block with zstd 19;
the writer's deflate (2 KB window) is 5–15 % larger on them. "Tail" = GameTime 4,500–7,199
(386 units on average, still fighting) scaled to an hour. Every claim is labelled MEASURED,
DISASSEMBLED or [INFERRED].

## The answer

**Reduce precision, not rate.** Once motion is column-coded with linear prediction, a unit
standing still or moving straight costs almost nothing per tick; the bytes are the unpredictable
low bits. Truncating the 16.16 fraction (keeping the whole-unit high word exact) at every tick
beats every sampled stream on size *and* error *and* keeps every engine-derived value on the
puppet exact by construction. Rounding — and the product contract's "whole world units, 10 Hz,
interpolated" read literally — gives puppets footprint cells the recording never had.

## Engine facts this rests on

| Fact | Where | How |
|---|---|---|
| Position = three i32 16.16 at +0x6A/+0x6E/+0x72; the high word is whole world units (1 wu = 1 game px at zoom 1, zoom range 0.25–8, `tagpu_zoom.h`) | notes, 0x459233.. | DISASSEMBLED (notes) |
| Set-position's footprint cell = (pos − (fw<<19) + 0x80000) sar 20: depends on the high word only | 0x48AA0C..0x48AA32 | DISASSEMBLED; formula MEASURED equal to +0x76/+0x78 in 4,669,500 of 4,669,500 live unit-ticks |
| Sort bucket = (z sar 23)·cols + (x sar 23) | 0x47CCA9..0x47CCC6 | DISASSEMBLED |
| Sight (ray fan) reads the high words of x, alt, z (record +0x12/+0x16/+0x1A): cell x_hw>>5, height def+0x170+alt_hw clamped, z cell (z_hw − h/2)>>5; the raise compares alt with (sea+1)<<16 | 0x4825D4..0x48261C, 0x48280D..0x48281B | DISASSEMBLED; sx = x>>21 MEASURED 99.96 % |
| Water state reads the altitude's high word (`movsx` +0x70) | 0x43DB71 | DISASSEMBLED |
| Walk class reads the whole speed dword mover+0x20 against def +0x1AE/+0x1B2, a zero test of mover+0x24, mover+0x2E bit 2, the carrier | 0x43DA70..0x43DABD | DISASSEMBLED |
| mover+0x24 is the turn rate: equals the heading's step | — | MEASURED 1,304,746 of 1,305,711 |
| recsize's "+0x140 counter" is the moved-at stamp mover+0x2A = GameTime−1, stored when pos+vel ≠ pos or the layer changes | 0x43D83D..0x43D865 | DISASSEMBLED; MEASURED it changes exactly when velocity ≠ 0 or layer changes, 2,164,214 of 2,164,214 |
| Velocity mover+0x08/+0x10: direction = heading+180° within 0.52° (p99), length = speed; computed with x87 floating point in the ground step | 0x43CD20.. (calls 0x4FB440, 0x4E43A0) | MEASURED relation; exact recomputation [INFERRED unsafe] |
| 7.4 % of moving units' x/z fractions are exactly 0xFFFF (pinned just below a whole unit) | — | MEASURED |
| A type-3 owner's movement pass never runs (0x43DD20 is called only from 0x48AFAA for local owners and the 0x2C sweep) | notes | DISASSEMBLED (notes) |

## What a puppet computes from what it receives

From `unit_put` (tagpu_replay.c) and the table above:

| Recorded value | Engine work on the puppet | Must be exact? |
|---|---|---|
| x, z, alt **high words** | set-position cells + claims, bucket, sight stamp every tick (players phase), water state, the stock draw, minimap dots | **yes** — else cells, sight, fog differ from the recording |
| x, z, alt **fractions** | Impure's sub-pixel draw only (`tagpu_native_unit_pos`) | no (display) |
| heading, tilts | written raw after the placement; draw | no (display) |
| velocity | only `a_pre` (vy) feeds set-position; the movement pass that adds it does not run | no (derivable) |
| speed, turn rate, layer, mover bit 2 | walk class → StartMoving/StopMoving/MoveRate | **yes** |
| HP, build fraction, state, stamps, orders | build finish, GUI, harness | **yes** (cheap: inside "rest") |

COB on a puppet: its engine writes are gated (tagpu_puppet.c), so a getter reading an
approximate position can change only animation [INFERRED]. Projectiles: the host's own tick is
overwritten from the recording at the frame visit and the capture point, so unit-position error
never reaches recorded state through them.

## Measured: size against error (x, z, alt, heading; whole battle)

| Stream | MB zstd (deflate) | tail MB/h | pos err p50 / max, wu | heading max | footprint cell wrong | jitter p99 |
|---|---|---|---|---|---|---|
| every tick, exact (lossless, round-tripped) | 3.52 (3.81) | 24.8 | 0 | 0 | 0 | 0 |
| every tick, 4 fraction bits, heading 16-bit | 1.90 (2.13) | 13.4 | 0.050 / 0.088 | 0 | 0 | 0.066 |
| **every tick, 4 bits, heading 8-bit** (round-tripped) | **1.50 (1.71)** | **10.5** | 0.050 / 0.088 | 0.70° | **0** | 0.066 |
| every tick, 2 bits, heading 8-bit | 1.23 (1.42) | 8.7 | 0.20 / 0.35 | 0.70° | 0 | 0.26 |
| every tick, whole wu truncated, heading 8-bit (round-tripped) | 1.07 (1.24) | 7.7 | 0.82 / 1.41 | 0.70° | 0 | 1.10 |
| error-bounded 0.25 wu/1°, 1/16 truncated, two-sided samples at derived changes | 1.95 (2.01) | 14.6 | 0.06 / 0.25 | 1.0° | 0 | 0.20 |
| error-bounded 1 wu/2°, whole wu truncated, two-sided | 1.52 | 11.2 | 0.76 / 1.41 | 2.0° | 0 | 0.90 |
| 10 Hz whole wu **rounded**, linear (the contract as worded) | 0.62 (0.69) | 4.6 | 0.29 / 1.48 | 8.7° | **17.2 %** | — |
| 10 Hz whole wu truncated, linear | 0.62 (0.69) | 4.6 | 0.79 / 2.38 | 8.7° | 1.4 % | 0.62 |
| 5 Hz whole wu truncated, linear | 0.47 (0.50) | 3.5 | 0.80 / 3.76 | 19.1° | 2.7 % | — |

Errors over the moving unit-ticks (1.63–1.65 million of 4,669,500). Rate alone at full precision (linear):
15 / 10 / 5 / 3 Hz = 2.58 / 2.08 / 1.45 / 1.08 MB, max 0.90 / 1.20 / 2.73 / 4.58 wu, heading
max 6.2 / 8.2 / 18.5 / 30.8°. Catmull-Rom cuts p99 by ~30 % (10 Hz: 0.51 → 0.36 wu, 3.7 → 2.7°)
but not the max, costs nothing in size, and overshoots — which breaks the convexity argument
below. Rounding to 1/16 wu at every tick still moves 14.9 % of moving units to a wrong cell
(the 0xFFFF fractions); truncation never does. Cost by field, lossless: x+z 2.60, heading 0.79
(8-bit 0.34), altitude 0.14 MB.

**Drift** (MEASURED, `drift.py`): quantizing each tick's *change* to whole wu open-loop, reset at
every snapshot: p99 63 wu, max 418 wu (rounded); 387 / 836 wu (truncated). Quantize the value
(a mask is closed-loop by construction), never the delta.

**The mover** (velocity, speed, turn rate): each against its previous 4.03 MB; lossless with the
velocity predicted by the position step and the turn rate by the heading step 2.07 MB (13.4
MB/h); speed and turn rate exact, velocity derived from decoded positions 0.66 MB (4.9 MB/h).
Derived velocity error p50 0.06, p99 0.90 wu/tick (blocked units). **The rest**, exact: stamps
0.25 MB (2.0 MB/h, moved-at predicted as GameTime−1), derived cells/sight/bucket/water 0.26 (2.1),
the 62 other dwords 1.78 (9.9). Today these motion dwords cost 31.8 MiB of the 46.7 MiB unit
deltas (`today_attr.py`).

| Unit stream, zstd 19 | Battle MB | Tail MB/h |
|---|---|---|
| Today's unit deltas (writer) | 48.95 | — |
| Lossless columns | 7.88 | 52.2 |
| **Recommended** (4 bits, heading 8, velocity derived) | **4.45** | **29.4** |
| + exact points every second (0.38 low bits + 0.23 velocity) | 5.06 | ≈ 34 (velocity's share [INFERRED]) |
| Contract (10 Hz whole wu, velocity derived) | 3.57 | 23.5 |

## Ideas, ranked

1. **Truncate, every tick.** x/z/alt keep the high word and k fraction bits (`v & ~((1<<(16−k))−1)`),
   heading 8 bits. Saves 2.0 MB (k=4) – 2.45 MB (k=0) on this file (2 %), 14–17 MB/h at the
   tail. Writer thread: a mask; game thread: 0 µs. Fidelity: < 2^−k wu per axis (k=4: 0.09 wu =
   0.7 game px at the 8× zoom maximum), heading ≤ 0.7°; derived state exact because every reader
   read so far takes the high word (P1 lists the ones not read). Seek: a pure function of the file; exact points at snapshot ticks (0.02
   MB) keep "play through S" equal to "seek to S". MP: the source peer truncates.
2. **Derive velocity, keep speed and turn rate.** Saves 1.41 MB / 8.5 MB/h. Nothing on a puppet
   reads velocity but `a_pre`, which then equals the decoded altitude [INFERRED from the
   movement pass not running for type 3; QUESTIONS.md batch 2 asks whether it is recomputable].
3. **Exact points every 30 ticks** for takeover: +0.61 MB / ≈ 4 MB/h.
4. **Rate reduction (10 Hz, or error-bounded sampling)**: 0.45 MB more off this file than whole
   units every tick (3.1 MB/h), 0.88 MB more than idea 1 (5.9 MB/h), for 2.4 wu / 8.7° error,
   wrong derived state (or two-sided samples that cost more than they save), a decoder that
   reads ahead to the next sample, and derived fields the harness can no longer compare EQUAL.
   Not recommended. Error-bounded sampling is the right form *if* sampling is wanted: a hard bound,
   per-field adaptive; two-sided samples around each derived-field change keep the cells exact
   (linear interpolation between two points of one cell stays in it — the derivations are
   monotone per axis); MEASURED 0 wrong cells, 0 wrong sight cells.
5. **Never**: rounding; open-loop delta quantization; Catmull-Rom.

## Takeover between exact points

The seek kills and re-creates every unit (tagpu_replay.c, *THE SEEK*: fresh COB, model pose,
mover course), so a takeover is already never a bit-exact continuation; what "exact" buys is
recorded fields equal to the recording's. With idea 1 the world at any T is within < 2^−k wu and
0.7°, with derived state exact and the host's own claims consistent by construction (its own
set-position wrote them). Choices: (a) exact points every second, takeover at the exact point at
or before T (≤ 1 s back) or played forward to the next; (b) snapshots only (≤ 30 s); (c) take
over from the bounded world.

## Verification for a lossy stream

- **G1, unchanged in kind:** host dump EQUAL to tarec.py's *decode* at T. The rule (mask,
  heading, `v̂ = p̂(t) − p̂(t−1)`, exact at exact points) is part of the format, implemented once
  in `tagpu_recreader.c` and once in tarec.py with a shared test vector. Derived fields stay
  recorded and compared: a host value differing names a reader the high-word argument missed.
- **G2:** `tarec.py check` EQUAL at snapshots (exact points).
- **G3, the bound, by construction:** a fixture flag keeps a truth stream beside; `tarec.py`
  checks 0 ≤ x − x̂ < 2^(16−k) per axis, heading ≤ 128/65536 turn, velocity uncompared between
  exact points, everything else EQUAL. Run when the encoder changes.
- **G4:** seek agreement (fresh, forward, backward) unchanged.

## Risks and prerequisites

- **P1** (scouts, `fidelity/QUESTIONS.md`): every engine read of +0x6A..+0x75 reachable on a
  puppet host — not read: the circle-mode sight branch (0x4826B7..), the claim 0x47CC30's other
  reads, the minimap 0x466DC0, the radar pass 0x467440, the fix-up 0x48A870/0x48A490 on a puppet.
- No aircraft or hills here (layer 2 never seen; altitude changed in 2.1 % of unit-ticks). An air
  game's residuals are larger; idea 1 adapts, a fixed rate's error would grow with speed.
- Sizes of the mover/stamps/derived/rest columns are measured, not round-tripped; the motion
  columns are (lossless, k=0, k=4: 0 of 4,669,500 differing). Predictor reset at snapshots not
  modelled (one first-order residual per moving unit per 900 ticks).
- HP (u16) and the build fraction (float) change in 1.0 % and 0.4 % of unit-ticks, sit inside
  the 1.78 MB "rest", and the build finish reads the fraction: keep exact.

## Owner questions (loosenings, CLAUDE.md; none decided here)

1. Position fractions truncated to k bits between exact points (k = 4 proposed; k = 0 is the
   contract's whole units).
2. Heading and tilts at 8 bits between exact points.
3. Mover velocity derived, not compared, between exact points.
4. Takeover: exact points every second (snap back ≤ 1 s or forward), snapshots only, or the
   bounded world.
5. Escalation #1 (changes the plan): replace the product contract's "10 Hz, interpolated" by
   "every tick, reduced precision", and read "whole world units" as truncation.

## Ranked recommendations

1. Truncate positions to the high word + 4 bits, heading 8 bits, every tick: −2.0 MB here,
   −14 MB/h tail, error < 0.09 wu / 0.7°, derived state exact, harness stays EQUAL vs decode.
2. Derive velocity from decoded positions, keep speed and turn rate exact: −1.4 MB, −8.5 MB/h.
3. Exact points at every snapshot (mandatory, 0.02 MB) and every second if takeover must be
   exact (+0.6 MB, ≈ +4 MB/h).
4. Do not sample at 10 Hz: 0.88 MB (5.9 MB/h) more off than recommendation 1, for 27× its
   position error, 12× its heading error and 1.4 % wrong cells (17 % if rounded).
5. If sampling is ever wanted: error-bounded, truncated, two-sided at derived changes, linear.

Scripts: `extract.py` (dense images, 6.4 GB in `data/`, deletable), `fields.py`, `survey.py`,
`motion.py`..`motion5.py`, `mover.py`, `rest.py`, `today_attr.py`, `drift.py`, `velexact.py`,
`roundtrip.py`; outputs `*.out`, `motion*.json`. numba installed into the shared venv.
