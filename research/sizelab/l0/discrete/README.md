# L0 — discrete: cells, orders, events, wrecks, globals, players, feature heads, snapshot wrecks

All numbers are MEASURED on three recordings unless marked otherwise. The recordings are
`battle` (Town & Country, 4 seats, 4,021 ticks, 79.15 MiB), `naval-push` (Anteer Strait,
3,841 ticks, 10.10 MiB) and `air-war` (Two Continents, 5,610 ticks, 46.21 MiB). The reference is
`size-battle` (Core Prime, 99.6 MiB). KB = KiB. "Today" is the writer's deflate as recsize
attributes it (one category's records deflated alone per block).

**How the scripts were run.** The committed scripts were copied unchanged into `lab/`, except
for one fix. Six files (`cells*.py`, `small_codec.py`, `predict.py`, `units_pass.py`) hard-code
Core Prime's grid as `W, H = 642, 648`. The copies take the size from the header's `features`
instead (540×540, 578×584, 768×700). Run unfixed on another map, they would address cells
wrongly. The copies are driven by `run.sh <name>`, with outputs in `<name>/*.txt`. Four scripts
are new:

- `events_snapreset.py`: the headline events figure, with resets at each snapshot.
- `cells_predict_ab.py`: the footprint predictor for both slots.
- `cells_miss.py` and `air_feet.py`: diagnostics.
- `zstd_dropin.py`: the compressor check.

## 1. Lossless

**Every codec decodes back exactly on all three recordings: 0 mismatches.** This covers:

- cells A and B;
- orders, all 4 variants, plus bytes, dod and tick;
- events with snapshot resets;
- wrecks, players, globals (DoD + RNG steps) and feature heads;
- the snapshot wreck base, checked in every snapshot.

The scripts assert, and every run exited 0.

## 2. Size: today against the codec

| Category | battle | naval-push | air-war | size-battle ratio |
|---|---|---|---|---:|
| Cells after the first grid | 6,491 → 1,070 (**0.16**) | 134 → 29 (0.21) | 14,040 → 1,774 (**0.13**) | 0.20 |
| First grid (one-time, kept as is) | 296 | 304 | 484 | — |
| Order lists (columns, values) | 1,584 → 444 (0.28) | 95 → 50 (0.52) | 567 → 141 (0.25) | 0.30 |
| Events (resets at snapshots) | 842 → 476 (0.57) | 261 → 129 (0.50) | 217 → 130 (0.60) | 0.51 |
| Wrecks (dword diffs) | 204 → 94 (0.46) | 187 → 60 (**0.32**) | 198 → 77 (0.39) | 0.41 |
| Globals (DoD + RNG steps) | 125 → 50 (0.40) | 62 → 14 (0.23) | 94 → 22 (0.24) | 0.32 |
| Players (diffs) | 31 → 25 (0.80) | 13 → 8 (0.62) | 23 → 19 (0.83) | 0.70 |
| Feature heads (cursor flag) | 29 → 11 (0.39) | 11 → 2 (0.18) | 27 → 7 (0.25) | 0.45 |
| **Sum** | **9,602 → 2,466** | **1,067 → 596** | **15,650 → 2,654** | 10.15 → 3.64 MiB |
| Together per block, first grid apart (`combined`) | 10,279 → 2,144 (0.21) | 1,255 → 309 (0.25) | 16,020 → 2,214 (**0.14**) | 0.27 |
| Snapshots, wreck base (deflate) | 647 → 440 | 309 → 101 | 478 → 170 | 1,629 → 1,247 |

**Each category's share of the file** (recsize attribution against the file on disk):

| | battle | naval-push | air-war | size-battle |
|---|---:|---:|---:|---:|
| Cells | 8.4 % | 4.2 % (the first grid is 2.9 % of it) | **30.7 %** | 3.5 % |
| Orders | 2.0 % | 0.9 % | 1.2 % | 2.8 % |
| Events | 1.0 % | 2.5 % | 0.5 % | 2.9 % |
| Wrecks and feature heads | 0.3 % | 1.9 % | 0.5 % | 0.4 % |
| Globals and players | 0.2 % | 0.7 % | 0.2 % | 0.5 % |
| **Discrete in the stream** | **11.8 %** | **10.3 %** | **33.1 %** | 10.2 % |
| Snapshots (whole) | 0.8 % | 2.9 % (wrecks 2.0) | 1.0 % (wrecks 0.7) | 1.6 % |

The discrete codecs save:

- **air-war: 12.7 MiB, 27 % of the file.** This is now the area's biggest saving.
- **battle: 7.0 MiB, 8.8 % of the file.**
- **naval-push: 0.46 MiB, 4.6 % of the file.**

## 3. Assumptions: what still holds

**The cells' tick-input repeat** (the next images re-send the tick input). It holds everywhere:

- 607,307 of 619,363 image cells on the battle;
- 22,981 of 26,069 on naval-push;
- **1,367,178 of 1,369,928 on air-war.**

**What air-war's cells are.**

- **1,355,948 of the 1,369,928 changed image cells are slot B (cell `+0x02`).** On size-battle
  slot B never changed.
- **Who writes them.** The footprint clear `0x47D0E0` and the stamp `0x47CC30` run every tick,
  at `0x43DA0F` and `0x43DA41`. The stamp files a unit with `+0x110 & 3 == 2` (airborne) in
  slot B. Source: the engine map, the feature-grid table. Not re-disassembled here.
- **Why so many** (MEASURED, `air_feet.txt`):
  - about 105 airborne units per step, up to 176;
  - 64 of them move their footprint each step;
  - each footprint is 2×2 or 3×3, so a move changes 4.6 cells;
  - that makes about 297 cells a step, against 242 observed;
  - the stream sends each one twice: once in the tick input, once again in the images.
- The battle's 12 aircraft add 13,431 slot-B cells.

**The footprint predictor** (`cells_predict.py`, slot A only, from ground units):

- **battle:** 592,985 cells exact, 87 that it misses or gets wrong, 4,466 it predicts that do not
  change. Better than size-battle's 99.6 %.
- **naval-push:** 19,939 exact against 946 misses and 714 extra predictions, **95 %**. The
  exceptions are warships (types 170, 37, 222, 227, 100, 95, layer 1, `cells_miss.txt`). [INFERRED]
  The cause is overlapping hulls, which the stamp's contest decides and the predictor does not
  model.
- **air-war:** it covers under 1 % of the changes. The rest are slot B, so its original estimate
  there is meaningless.

**Extended to both slots** (`cells_predict_ab.py`; slot B from layer-2 footprints, the stamp's
own rule):

| | air-war | battle | naval-push |
|---|---|---|---|
| Slot B exact / another value / missed / predicted but unchanged | 1,214,784 / 101,587 / 40,910 / 95,429 (**89 %**) | 12,837 / 429 / 326 / 583 | — |
| Tick input, B coding → under the prediction | 1,766 → **365 KB** | 1,091 → **24 KB** | 27 → 11 KB |

So the prediction is worth about −1.4 MiB on air-war and −1.0 MiB on the battle [ESTIMATED: the
exceptions are priced, not decoded]. [INFERRED] Most of air-war's slot-B misses are flyers
contesting one cell, so modelling the contest rule would close more of them.

**Orders.**

- The changed node is the head of the list in 100 % of re-sends (164,942 of 164,944 on the
  battle).
- The wake time `+0x0A` still dominates on the battle (117 K byte changes).
- On naval-push the ratio is worse (0.52), because changes are spread over `+0x4C/+0x4E`, the
  event bits, and `+0x04`, the action and state. The general mask form absorbs this; it is
  45 KB in absolute terms.

**Events.**

- Shot points still sit close to the shooter's and target's positions.
- On air-war a few targets miss by up to 127 wu (moving flyers): 3 shots, 7 bits.
- Air-war deaths differ in 12–17 dwords instead of about 5 (crashing aircraft). The death column
  is still 30 → 13 KB.

**Wrecks: fields that never changed on size-battle.**

- On Town & Country and Two Continents, `+0x04/+0x06` and `+0x2E` change: the
  animating-features writer `0x425050`, per the engine map [INFERRED: trees].
- On naval-push, `+0x0C/+0x0E` changes in 26 K updates. That is the 16.16 y position
  [INFERRED: wrecks sinking].
- Dword differences beat values on all three (0.32–0.46).

**Globals.** Every `rngseed` change is found as a Park-Miller step count: 3,553, 2,789 and 3,848
of them. The median is 23, 3 and 2 steps; the maximum is 1,460, 3,833 and 1,314, under
size-battle's 12,622. The pfsearches delta-of-delta rule still holds.

**Feature cursor.** The feature cursor flag still holds.

**Snapshot wreck base.**

- At the first snapshot, the battle has 0 records that differ from the first iteration and
  naval-push has 31.
- Wrecks are 52 of each naval snapshot's 80 KB. The base removes them to about 1 KB.

## 4. Cost

Nothing grows badly. The RNG step search stays under 4 K steps. The scripts' runtime and memory
scale with the record count: the battle's whole pipeline took 3 minutes at ≤1.7 GB, Python
prototype memory, not the writer's. The cells codec is the slowest piece, 30 s on air-war.

## 5. The compressor: zstd 3 as a drop-in, every block

| | writer MiB | zstd3 per block MiB | ratio | chained (frame per snapshot interval) | iterations a block | decode, writer → zstd3 (64-bit) |
|---|---:|---:|---:|---:|---:|---|
| battle | 79.15 | 63.17 | 0.798 | 57.23 (0.723) | 5.8 | 510 → 215 ms |
| naval-push | 10.10 | 6.36 | 0.629 | 5.99 (0.593) | 49.2 | 67 → 22 ms |
| air-war | 46.21 | 34.97 | 0.757 | 33.10 (0.716) | 12.6 | 322 → 180 ms |
| size-battle | 99.59 | 68.23 | 0.685 | 59.93 (0.602) | 10.0 | 634 → 241 ms |

All of these were decoded back. The drop-in gain is smaller on the busy battle (0.80). There,
blocks hold 5.8 iterations, because the raw block cap fills fast, and chaining recovers about
6 MiB. [INFERRED] The hilly unit deltas compress worse as well. zstd still decodes 1.8–3× faster
than inflate here, and the compressor choice stands.
