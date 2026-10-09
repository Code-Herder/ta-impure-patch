# Units between snapshots: the unit frame, the fields, sampling

Area: `units: deltas`, 46.68 MB packed alone (49.9 %). "MB" is recsize's MiB throughout. Every size
is MEASURED on `size-battle.imprec` as recsize attributes a category: each stream block's unit
section compressed alone; "writer" is zlib 6 with a 2 KB window (within 0.4 % of lodepng per
recsize). Scripts and `results.jsonl` are in `research/units/`. Every frame2 run was decoded back
from its bytes: 7,830 iterations, 4,669,500 unit images, 0 differences; one flipped bit is detected
(`corrupt_check.py`).

## Headline

| | writer | zstd 19 | lzma | saving vs today (writer) | share of the 99.6 MB file |
|---|---:|---:|---:|---:|---:|
| Today's records | 46.68 | 34.82 | 29.68 | — | — |
| **Best lossless**: frame, field lanes, per-unit predictor switching, engine integrations | **4.50** | 4.35 | 4.13 | −42.2 | 42 % |
| Same, generic predictors only (no engine rules) | 9.84 | 9.38 | 8.72 | −36.8 | 37 % |
| Best lossless with 300-iteration blocks | 3.77 | 3.58 | **3.32** | −42.9 | 43 % |
| Lossy, the contract (10 Hz, whole wu, 8-bit turns) | 2.87 | 2.73 | 2.63 | −43.8 | 44 % |
| **Best lossy**: error-bounded, ε = 1 wu / 2° | **2.56** | 2.48 | 2.38 | −44.1 | 44 % |
| Floor: motion dropped entirely, everything else exact | 2.04 | 1.98 | 1.92 | | |

After the lossless frame, sampling motion can buy at most 2.5 MB more on this file (to the floor).

## 1. Engine facts the design rests on

Unit image offsets (the mover is at 0x118). Field boundaries are from the engine map and the
disassembly below. Every rule's "misses" is MEASURED over the battle's live unit-iterations.

| image dword (recsize) | fields | how established |
|---|---|---|
| `+0x140`, the 18-bit step | mover `+0x28` (u16, constant here) and the low word of **mover `+0x2A`, the GameTime of the last move** | DISASSEMBLED `0x43D85A..0x43D865` (`mov ecx,[main+0x38A47]; mov [mover+0x2A],ecx`). It is written when the position plus the velocity, or the layer, differs from the current one (`0x43D7FF..0x43D854`). MEASURED: all 2,164,214 changes equal the iteration's GameTime − 1. The rule "GameTime−1 when the velocity ≠ 0 or the layer changed, else unchanged" misses **0** of 4,360,620. 531,554 stamps advance with the position unchanged: blocked units. |
| `+0x18`, the 1-bit step | weapon slot 1 `+0x14`, **the reload countdown** (u16); `+0x16`, the aim heading | DISASSEMBLED `0x49E1C8..0x49E1D2`: AutoAim decrements it while it is > 0. MEASURED: 1,089,621 of 1,116,370 changes are −1; the rest are reloads at fire. |
| `+0xF8` | `+0xF8` sight h, `+0xF9` piece, **`+0xFA` a damage countdown**, `+0xFB` the give lock | DISASSEMBLED `0x48ADF0..0x48AE0D` (each −1 a tick while nonzero). MEASURED: 812,874 of 838,218 changes are −1. |
| `+0x64` | `+0x64` pitch, `+0x66` **heading** (u16, 65536 = 360°) | DISASSEMBLED `0x43CFD1..0x43CFDB`: heading += the mover's turn rate. MEASURED: misses **963** of 4.67 M. Pitch and roll never changed in this battle (Core Prime is flat). |
| `+0x68`/`+0x6C`/`+0x70`/`+0x74` | roll; then **x, y, z as 16.16 dwords at `+0x6A`/`+0x6E`/`+0x72`**, straddling the dwords; footprint x at `+0x76` | the saver's table in the engine map; `0x43D7FF` reads `+0x6A/+0x6E/+0x72` as dwords |
| `+0x120`/`+0x124`/`+0x128` | the mover's velocity x, y, z (16.16 wu per tick) | MEASURED: **v = −(sin h, 0, cos h)·speed** using the engine's 512-entry table at `0x509F00`, rounded as `0x4B70EF`/`0x4B7123` do (`(T·s + 0x1000) >> 13`). It holds in all 4,362,632 live mover unit-iterations. vy is 0 throughout: there are no aircraft. |
| `+0x138` | the mover's speed `+0x20` | engine map (`0x43CC20`, `0x43D9B6`); at most 1.8 wu a tick |
| `+0x13C` | the mover's **turn rate** `+0x24` (i16); `+0x26` (u16, not identified: 89 k changes, no writer in `0x43C000..0x43DF00` but the constructor) | DISASSEMBLED as above |

**Position:** pos(t) = pos(t−1) + v(t) exactly in all 1,991,061 unit-iterations where the mover's
`+0x2E` bit 2 is clear. That bit is sticky: it records that the footprint refused the last cell
change. In 2,371,571 unit-iterations it is set (this battle is a crowd). There the candidate is
clamped to ±0x7FFFF of the footprint cell's centre and the speed is capped at def `+0x192`/2,
DISASSEMBLED at `0x43D939..0x43D9E2`. The clamp rule alone still misses 197 k.

**Steps and quantization** (MEASURED):

- Moving units step p50 1.1, p99 1.8, at most 1.8 wu a tick.
- Heading steps are p50 0.06°, p99 6.15° a tick. Altitude changes in whole wu.
- At 10 Hz in whole wu, a step is at most 6 wu. The linear-extrapolation residual is 0 for 82–86 %
  of samples and within ±1 for 98 %.
- The 8-bit heading's residual is 0 for 87 %, at most 28 steps.
- |residual| ≤ 3 covers 99.9 % of the position residuals and 94.6 % of the heading's.

So a ground unit's motion is two inputs, speed and turn rate; the engine's integrator derives the
rest.

## 2. The lossless unit frame (ideas 1 and 5, measured as a ladder)

Mechanism, writer-side only (capture and ring unchanged). Each iteration's unit records become one
frame:

- the changed units as slot gaps (varints), and a lane mask per unit, as an index into a per-block
  mask dictionary;
- then the residuals **by lane, as columns** (struct of arrays), as zigzag varints.

A *lane* is a field of the record. Each lane's residual is its value less a prediction. A unit is
listed only when some residual is nonzero, so a unit moving at constant velocity costs nothing. This
is dead reckoning made lossless (DIS/IEEE 1278's send-on-error, with an error bound of zero).

| step (each adds to the row above) | writer | zstd 19 | lzma | decoded? |
|---|---:|---:|---:|---|
| Today's records | 46.68 | 34.82 | 29.68 | — |
| One record per iteration: slot gaps, today's 11-byte masks and values interleaved | 37.87 | 31.39 | 28.95 | no |
| The frame per block, values grouped by lane (columns), still absolute | 31.34 | 25.52 | 22.02 | no |
| Values as differences from the previous iteration | 17.51 | 14.17 | 12.60 | no |
| — the same in rows rather than columns | 22.80 | 16.63 | 15.07 | no |
| Mask dictionary + zigzag varints (MRU "same mask as last time" ≈ the same, 15.96) | 15.61 | 14.17 | 13.42 | no |
| **Field lanes** (u16/u32 at the record's own boundaries, not dwords) | 13.06 | 12.20 | 11.38 | no |
| **Per-unit predictor switching**, prev / linear extrapolation | 9.84 | 9.38 | 8.72 | EQUAL |
| — the same with dword lanes (fully generic) | 12.25 | 11.67 | 11.10 | EQUAL |
| + stamp and countdown rules (idea 5) | 9.65 | 9.21 | 8.57 | EQUAL |
| + heading ← turn rate | 8.97 | 8.66 | 8.06 | EQUAL |
| + velocity ← sine table × speed | 6.32 | 6.12 | 5.75 | EQUAL |
| + **position ← velocity** (all five rules) | **4.50** | 4.35 | 4.13 | EQUAL |
| Differences from the snapshot instead (for comparison) | 55.39 | 37.60 | 34.19 | no |

**Per-unit predictor switching.** Per lane and unit, both sides predict with the candidate that was
exact last iteration (else the closest; ties: rule, prev, linear). Both derive it from decoded
values, so there is no side information; the precedent is adaptive predictor selection (LOCO-I,
FLAC). On x it misses 157 k unit-iterations, against linear's 606 k, the rule's 1.0 M and an
oracle's 107 k (`lastwin.py`).

**What is left in the 4.50:**

- framing 2.0 MB: gaps 0.64, masks 1.34;
- turn rate 0.53, x and z 0.65, the aim heading and pitch 0.38, speed 0.20;
- everything else under 0.1 each (`lanestats.py`).

**Costs and effects.**

- *CPU:* the writer evaluates every live unit's lanes each iteration. [INFERRED] In C that is about
  1 ms per iteration at 2,000 units on the writer thread, not measured. The game thread is
  unchanged.
- *Memory:* two images, plus a winner byte per lane, per slot: about 1.9 MB, on the writer and on
  each reader.
- *Seek:* the predictor state (the previous-previous image and the winners) resets at the first
  iteration after each snapshot. This is included in the numbers. A reader starting at a snapshot
  decodes forward as today; a fork's first block needs the same reset (not measured).
- *Takeover and verification:* unchanged, because the stream is lossless.
- *Multiplayer:* contributions can stay in today's records; the writer transcodes the merged stream.
  Not measured on the wire.

## 3. Reduced sampling (lossy)

Motion lanes leave the exact frame: x, y, z, the three turns, and the mover's velocity, speed, turn
rate and moved-stamp. Everything else stays exact every iteration, footprint, sort bucket, sight,
the walk class (`+0x110` bits 2–3) and the countdowns included.

- *Free anchors:* a unit's first iteration (RK_UNIT), its death image, and the snapshots.
- *Samples:* residuals against linear extrapolation in quantized units. Units with a zero residual
  are omitted when the sample times are implicit (10 Hz).
- *Error:* measured against the exact value at every unit-iteration (`lossy.py`).

| variant | writer | lzma | samples | position error wu (mean / p99 / max) | heading error ° (mean / p99 / max) |
|---|---:|---:|---:|---|---|
| 10 Hz, whole wu, 8-bit turns (the contract) | 2.87 | 2.63 | 1,554,992 | 0.09 / 0.50 / **2.67** | 0.37 / 2.34 / **8.73** |
| ε 0.5 wu / 1°, 1/16-wu samples | 3.10 | 2.89 | 318,033 | 0.03 / 0.45 / 0.50 | 0.23 / 0.74 / 1.00 |
| ε 1 wu / 2°, whole wu | **2.56** | 2.38 | 158,673 | 0.13 / 0.97 / 1.00 | 0.27 / 1.46 / 2.00 |
| ε 2 wu / 4°, whole wu | 2.44 | 2.29 | 106,854 | 0.21 / 1.73 / 2.00 | 0.35 / 2.88 / 4.00 |
| ε 1 / 2, every block self-contained (a sample forced at each block's end) | 2.95 | 2.70 | 801,080 | 0.10 / 0.67 / 1.00 | 0.33 / 0.70 / 2.00 |

The error-bounded sampler checks the *decoded* interpolation of the quantized samples. The
precedents are swinging-door trending in process historians and Douglas–Peucker. Its bound is
therefore a guarantee by construction. It beats the fixed 10 Hz on size and on worst error at the
same time.

Its costs:

- The reader needs look-ahead to the next sample.
- A segment open at the end of a block either spans into the next block, so a crash loses the open
  tails, or is cut there (+0.4 MB). The second is unoptimised: zero-residual cut samples are still
  listed.

**What would become approximate (loosenings, the owner's decision):**

- position and turns between snapshots, by at most ε, or by up to 2.7 wu / 8.7° at a fixed 10 Hz;
- the mover's velocity, speed, turn rate and moved-stamp, which are not carried between snapshots;
- a takeover at a non-snapshot GameTime, which gets an approximate world or snaps to the previous
  snapshot;
- the replay gate, which needs a tolerance on those lanes between snapshots.

The weapons' aim angles (0.38 MB) could also be sampled. That is not included and would be a
further loosening.

## Over time

Writer, MB per 30 s of GameTime:

| GameTime | today | lossless | 10 Hz | ε 1/2 |
|---|---:|---:|---:|---:|
| 0–899 (2,010 → 1,128 units) | 17.15 | 1.71 | 1.06 | 0.95 |
| 6300–7199 (~360 units) | 2.37 | 0.22 | 0.14 | 0.13 |

Per hour at the 350–430-unit tail (GameTime 4500–7199): **today 303 MB, lossless 27.9, lossy
15.8–18.1**. At the 2,000-unit peak the lossless rate is 205 MB an hour for units alone: the battle
is all motion and combat.

## Risks and prerequisites

- **Rules are format, not hints, between writer and reader.** A wrong rule only costs bytes. But a
  rule that differs between the writer and a reader decodes wrong values silently, so:
  - the rules and the sine table (512 int16, copied as constants) are pinned by REC_FORMAT;
  - the writer can decode its own block before writing it;
  - `tarec.py check`'s comparison at every snapshot catches any divergence within 900 iterations.
- **Measured on one battle**: a flat map, no aircraft, a crowd. Hills move pitch and roll
  (`0x48A490`), and aircraft use the air step `0x43D290`, which these rules do not model. The codec
  stays lossless and the gain shrinks toward the generic 9.84. **Prerequisite:** the same
  measurement on a recording with aircraft and hills (the four-AI hour).
- **The field-lane table** is the engine record's field boundaries. A wrong row costs size, never
  correctness. Dword lanes are the fully generic alternative: 12.25 against 9.84 without rules, and
  the rules need field lanes for the heading and x/z.
- **Writer CPU** must be measured in C (above, [INFERRED]).
- The 300-iteration-block number assumes the other categories shrink too: the writer cuts at
  256 KB raw.

## Ranked recommendations

1. **The lossless frame with field lanes, per-unit predictor switching and the three integration
   rules** (heading ← turn rate, velocity ← table × speed, position ← velocity). The stamp and the
   countdowns add 0.15.
   - 46.68 → **4.50 MB** (writer), −42 % of the file.
   - Gate, seek and takeover unchanged.
   - Writer and both readers change; the capture does not.
2. **The generic part alone** if the owner prefers no engine rules in the format: **9.84 MB**, −37 %
   of the file.
3. **Bigger blocks** once the stream is small: 4.50 → 3.77 (writer), 3.32 (lzma). A stronger
   compressor matters little for units once they are decorrelated: 4.50 → 4.13 with lzma.
4. **Lossy only if the owner wants it, and error-bounded rather than 10 Hz:** ε = 1 wu / 2° gives
   **2.56 MB** (2.95 self-contained), against 2.87 at 10 Hz with 2.7× the worst position error and
   4.4× the worst heading error. The ceiling is the 2.04 floor.
5. **Do not:**
   - differences from the snapshot: 55.4 MB, worse than today;
   - a separate counters mechanism: linear prediction already takes all but 0.2 MB of it.

**Questions for the owner:**

- **(a)** Is the lossless 4.5 MB enough for units, so that the contract's 10 Hz / whole-wu
  loosening is not needed here?
- **(b)** If lossy, should the contract say "error-bounded at ε" instead of "10 Hz", and with which
  ε? Which tolerance on the gate? Should a takeover between snapshots be approximate, or snap to the
  snapshot?
- **(c)** Engine rules in the format (4.50 MB), or generic predictors only (9.84 MB)?
