# Demo recorder: making recordings smaller

## Status

**L1 is built and measured, not landed** (*L1: recording format 8, measured*, below): recording
format 8 packs every block with zstd and carries the unit deltas in a unit frame predicted from the
engine's own rules, and a new run of the battle below recorded in 29.1 MiB where the measured one
took 99.6 (not like for like: another game of the same scenario). L2 to L6 are
plans. The page gathers what has been measured about the size of a demo recording, the research
round that prototyped each way of reducing it on one battle recording, and the plan that came out
of it: **lossless, the battle's 99.6 MiB file becomes about 12 MiB**, and lossy options add about
1 MiB per category on top. It feeds the [product page](demo-recorder.md)'s open decision 2 (measured size) and the
landing that turns recording on for every game, which waits for a smaller stream (the product
page's status: "Recording every game waits for the reduced stream").

The owner chose lossless first (2026-10-07, *Decisions* below): the reduced stream keeps every value
at every tick, and the product contract's *Disk budget* says so; precision is cut only if the
measured hour misses a budget. The budgets are
**10 MB an hour for a 1v1, 25 MB for a typical four-player game and 100 MB for ten players with
1,500 units**. Today's recorder writes every value at every tick, exactly, which is what the replay
gate's EQUAL checks rest on. The research below recommends lossless first, which keeps those checks
exact; a lossy option needs tolerances, which are the owner's decision (*What the lossy options
still buy*).

## The measurement

MEASURED 2026-10-07 on the reference setup, build `337246d4` (raised limits), with
`tools/replay/recsize.py` on a recording of `scenarios/size-battle-10p.json`: ten seats on Core
Prime Industrial Area, five ARM armies against five CORE armies of 200 units each (Peewee/AK,
Hammer/Thud, Rocko/Storm in limits-tier1's 12:8:5 mix), in the open strip between the northern
enclosures and the central chasm, ordered onto each other. The scenario format has no alliances, so
same-side neighbours can fight too; that does not change where the bytes go.

- 7,830 ticks (4 min 21 s of game time), 2,010 units at the start (the ten commanders kept), 1,115
  at GameTime 952, 557 at 2,804 and 346 at 7,282: a battle that burns down in its first two minutes
  and a long tail of about 400 units.
- **99.6 MB**, `tarec.py check` clean (8 snapshots, 0 differing, every change explained). The stream
  is 98.4 % of the file, the snapshots 1.6 %.
- Disk per 30 s of game time: 40.2 MB in the first, then 17.7, 10.9, 7.3, 6.0, and 4.6–4.9 MB once
  about 400 units remain, **about 560 MB an hour** at that tail rate.
- The capture cost 337 µs an iteration on average and 1,892 µs at worst (GameTime 70, 1.1 MB into
  the ring); the ring held 1.9 MB at most.

**The stream by category**, packed sizes attributed by deflating each category's records of a block
alone with the writer's settings (their sum is 93.5 MB against the file's real 98.0 MB):

| Category | Records | Raw | Packed alone | Share of packed |
|---|---:|---:|---:|---:|
| Unit deltas | 3,017,794 | 107.7 MB | 46.7 MB | 49.9 % |
| Projectiles | 660,632 | 60.3 MB | 36.5 MB | 39.1 % |
| Feature-grid cells | 570,607 | 22.2 MB | 3.5 MB | 3.8 % |
| Events | 288,343 | 9.7 MB | 2.9 MB | 3.1 % |
| Order lists | 240,928 | 23.2 MB | 2.8 MB | 3.0 % |
| Everything else | | 3.2 MB | 1.1 MB | 1.1 % |

**Unit deltas.** 385 an iteration on average, 56 % of the live units. A delta is a record header
(4 bytes), the slot (2), the 82-bit mask of changed dwords (11) and padding to a dword: 20 bytes
before its first value, and it carries 4.4 changed dwords on average. So the framing is **57.6 MB
raw, more than the 50.1 MB of values it frames**. The dwords that change most, by count: the
mover's `+0x140` (2.2 million times, an 18-bit step every time), the turn `+0x64`, the position and
altitude dwords `+0x68` and `+0x70`, `+0x18` inside weapon slot 1 (1.2 million times by one bit),
`+0xF8`, and the mover's `+0x120`, `+0x128`, `+0x13C` and `+0x138`. The tool measures by dword;
the position (`+0x6A`) and altitude straddle dword boundaries, so their changes read as 30–32 bits
there and need a field-by-field measurement.

**Projectiles.** 640,407 deltas, each rewriting about 21 of the record's 27 dwords: 51.4 MB of
values and 7.5 MB of framing. A shot in flight changes most of its state every tick, so a delta
saves little against the whole record.

**Order lists.** 240,928 lists sent; 238,706 were the same length as before with exactly one of
their order images changed (the list is re-sent whole). 23.2 MB raw, but 8:1 under deflate.

**Feature-grid cells.** 22.2 MB raw, 6:1 under deflate; 8.5 MB of it is the cells inside a feature
call or a death (the tick's input to the reproduction), the rest the cells' images.

**The same stream blocks under other compressors:** the writer (lodepng's deflate, a 2 KB window)
98.0 MB; zlib level 9 with a 32 KB window 82.1 MB; zstd level 3 66.8 MB; zstd level 19 59.5 MB;
lzma preset 6 51.9 MB; zstd level 19 over the whole stream as one frame 47.2 MB.

Earlier measurements this page draws on:

- **The tolerance sampler** ([state evidence §4](demo-recorder-state-evidence.md#4-size-estimate),
  MEASURED 2026-09-29): a value kept only when linear interpolation from the last kept sample would
  miss a skipped tick by more than ε, positions as 1/16-wu deltas, no general compression. A
  1,432-unit battle: 1,714 MB an hour with every change, **392 / 198 / 162 MB** at ε = 0.25 wu and
  1°, 1 wu and 2°, 2 wu and 4°, with the unit speed left out; **643 / 517 / 500** with the speed
  kept as a sampled value. The four-AI game: 14.8 MB an hour, 6.2 / 5.4 / 5.3 (speed left out).
- **The scratch repacking** ([skirmish slice, *Measured*](demo-recorder-skirmish-slice.md#measured),
  S1's landing build): one record an iteration instead of one a unit, each value as its difference
  from the unit's last, each field's differences stored together. A battle deflated to 13.8 MB
  against 34.5 MB as written, **2.5 times smaller**; the eight-minute game 2.7 times. One record an
  iteration alone gained 1.2 times; the differences are most of the gain. It was not decoded back.
- **The changed-only estimate** (state evidence §4): 2.4–3.3 bytes per changed unit-sample with
  int8 whole-pixel deltas, an 8-bit heading, 16-bit HP and an 8-bit build fraction; 5 Hz is 62–63 %
  of 10 Hz, so halving the rate saves only 38 % of the unit state.

## The research round

On 2026-10-07 five researchers each took one area of this recording, prototyped re-encodings in
Python and decoded every lossless one back against the original bytes; two challengers then re-ran
the headline measurements, checked the engine facts in the disassembly, and assembled the combined
plan without counting any saving twice. Sizes in this section are binary megabytes (MiB): the file
is 99.6 MiB, the stream 98.0 MiB. "Writer" means the recorder's own deflate (lodepng's defaults, a
2 KB window), per block as the file is written today. The prototypes, outputs and each area's full report are
kept in `research/sizelab/` (its README says how to re-run them).

### Units: 46.7 → 4.5 MiB, lossless

Each iteration's unit records become one frame: the changed slots as gaps, a mask per unit, then
each field as a column of residuals against a per-unit prediction. Fields follow the record's own
boundaries, not dwords (the 16.16 position at `+0x6A/+0x6E/+0x72` and the heading at `+0x66`
straddle dwords). The prediction is, per unit and per field, whichever of three was exact last
time: the last value, a linear extrapolation, or the engine's own rule:

- heading ← turn rate (DISASSEMBLED `0x43CFD1`); the mover's `+0x24` is the turn rate;
- velocity ← the integer sine table at `0x509F00` × speed (DISASSEMBLED `0x43CC20`,
  `0x43CCDB..0x43CD13`: vx = −sin(h)·speed, vy = 0, vz = −cos(h)·speed); MEASURED exact in
  4,362,632 of 4,362,632 live mover unit-ticks;
- position ← velocity (DISASSEMBLED `0x43D7FF`); MEASURED exact in 1,991,061 of 1,991,061
  unblocked unit-ticks.

MEASURED, writer per block: **4.50 MiB** (4.13 with lzma); 7,830 iterations and 4,669,500 unit
images decoded back with 0 differences, and re-run by a challenger with the same result; at all 8
snapshots the decoder's images and live set equal the snapshot's. The tiers:

| Prediction | Units, MiB |
|---|---:|
| today's deltas | 46.68 |
| generic (last value, linear) | 9.84 |
| + integrators: a field advances by another field (heading by turn rate, position by velocity, the stamp, the countdowns) | 7.30 |
| + the engine's sine-table velocity rule | 4.50 |

The integrators are a general mechanism; the sine-table rule pins the format to one engine fact,
and it is worth about 17 MiB an hour at the battle's tail [INFERRED, scaled], the difference
between meeting and missing the ten-player budget. A rule that differed between writer and reader
would decode wrong values silently, so the rules and the table are constants of the format
version, and `tarec.py check`'s comparison at each snapshot catches a divergence.

The hot fields, established: `+0x140` is the low word of the mover's `+0x2A`, the GameTime of the
unit's last move, set to GameTime − 1 when the velocity is nonzero or the layer changes
(DISASSEMBLED `0x43D83D..0x43D865`; MEASURED 0 misses in 4,360,620); `+0x18` is weapon slot 1's
reload countdown, decremented while above zero (DISASSEMBLED `0x49E1C8..0x49E1D2`; 1,089,621 of
1,116,370 changes are −1); `+0xF8`'s step is the `+0xFA` damage countdown (`0x48ADF0`); the mover's
`+0x26` is a dword GameTime claim stamp (`0x440A91`). A separate "counters recomputed" mechanism is
worth only about 0.2 MiB: linear prediction already captures them.

Differences from the snapshot instead of the previous tick cost 55.4 MiB, worse than today.

### Projectiles: 36.5 → 2.5 MiB, lossless

Today's projectile deltas are large not because shots move but because the engine compacts its pool:
`0x49AE20`, at the end of every tick, slides every record behind a removed one into a new slot (and
`0x49C8F4` does the same in a dying unit's cleanup `0x49C880`). MEASURED: of 497,169 survivors
whose `+0x67` stamp names their previous slot, 439,044 (88 %) changed slot, so a slot's delta
compares two different projectiles. The same projectile one tick later changes its position in
86 % of ticks, its vertical velocity in 25 %, everything else under 6 %.

Coding the pool by projectile identity (survivors matched to their previous record, births as new
records, deaths as removals): MEASURED **2.55 MiB** writer (2.42 zstd 19), 7,830 of 7,830 pools
decoded back exactly and re-run by a challenger; decoding forward from each snapshot is exact. Two
other recordings also decoded exactly: Town & Country's battle 28.2 → 2.44 MiB, naval 4.85 → 0.46.
The decoder never searches; the encoder's bounded search averages 154k dword compares an
iteration, but the busiest second needs 288 million, so the search needs a cap.

Sampling or quantizing projectiles does not pay: 10 Hz is larger than exact 30 Hz (2.77 against
2.55), whole world units every tick 3.10 against 2.96. Births are 46 % of the stream and are
events, not samples. Recording from the launch is worth about 1.1 MiB more at most: dead reckoning
from a projectile's first image reproduces its position at every tick for 77 % of projectiles, but
whether a puppet's own flight is bit-exact is not measured (burst copies draw the simulation's
random numbers; the guidance `0x49B3E0` reads the target's position one unit tick earlier on a
puppet [INFERRED]), and it would make the puppet's engine a writer of its pool, a change to the
replay contract.

### Cells, orders, events and the rest: 10.2 → 3.6 MiB, lossless

MEASURED, each re-encoding decoded back equal to the records it replaces (writer per block):

| Category | Today | Re-encoded | What was redundant |
|---|---:|---:|---|
| Feature-grid cells | 3,353 KB | 678 KB | 360,850 of 371,001 changed cells in the images (97.3 %) repeat the tick's own input cells byte for byte: `cells_tick` does not update the recorder's shadow |
| Order lists | 2,862 KB | 872 KB | the node's `+0x0A` wake time, set to GameTime plus a delay by `0x439E80` (75 call sites); not recomputable on a puppet, sent as the changed node |
| Events | 2,986 KB | 1,530 KB | shot points within 63 wu of the shooter, script names as strings, a death's image ~5 dwords from the unit's, repeated sprays |
| Wrecks, globals, players, feature heads | 946 KB | 401 KB | `rngseed` as a Park-Miller step count (median 16 a tick); the pathfinder counters by delta of deltas |

Two more: **every snapshot carries all 8,192 wreck records**, because the level load links each free
record to its neighbours so none is zero; storing only those that differ from the first iteration
saves 0.37 MiB here and, in a small game, most of each snapshot (51 of 58 KB in the ten-seat
fixture, about 6 MiB an hour). And the cells could be derived from the units' footprint moves
(99.6 % predicted, about 0.6 MiB more [INFERRED]). Lower rates or priority sampling are not worth
it here: after coding, sprays and replies cost under 24 KB.

### The container and the compressor

As a drop-in on today's blocks, zstd level 3 makes the stream 66.8 MiB (−32 %), cuts the writer's
CPU from 38 to 3.6 ms per second of game (130 to 13 in the busiest 30 s) and decompresses about
eight times faster (129 MB/s to about 1 GB/s; a seek through the busiest interval decodes in 0.06 s
instead of 0.75) — MEASURED with a 32-bit
MinGW build at the DLL's flags under Wine on the reference setup. Vendored without the greedy, lazy
and binary-tree strategies (level 3 is dfast), the legacy formats and the error strings, it adds
345,088 bytes to the DLL (MEASURED, L1).

Once the codecs above are in, the compressor no longer shrinks the file: on the unit frame zstd 3
is 4.63 MiB against the writer's 4.50, zstd 19 about 5 % better. Snapshots are the exception
(1,629 KB writer, 1,164 zstd 19). So zstd buys CPU and seek speed, not size. Removing 54 MiB of raw
framing alone saves only 6.1 MiB under zstd 3: implicit lengths (where the format fixes a record's
size), runs and slot gaps pay; varint headers, dropping padding, trained dictionaries, sparse mask
encodings, byte shuffles and order-0/1 rANS gain nothing on this data. The container that carries
the codecs: blocks of at most 30 iterations, ending at each snapshot, holding a kind sequence and
one section per category. As built in L1 each block's zstd frame is self-contained, but the unit
frame's state runs on from block to block and restarts only at reset points (*L1*, below), since a
restart costs every unit's first iterations of history.

### The plan, combined

Challenger B's sum, nothing counted twice (each category measured by its author; the total is a
sum):

| Category | Today, MiB | Lossless plan, MiB | Tail today, MiB/h | Tail lossless, MiB/h |
|---|---:|---:|---:|---:|
| Units | 46.68 | 4.50 (7.30 integrators, 9.84 generic) | 302.9 | 27.9 |
| Projectiles | 36.50 | 2.55 | 131.6 | 15.0 (MEASURED: the tail compresses worse) |
| Cells, orders, events, the rest | 10.15 | 3.64 | 62.5 | ~21–30 [INFERRED] |
| Other kinds | 0.14 | 0.14 | 0.6 | 0.6 |
| Interleaving loss | 4.5 | ~0 (sections) | 69.3 | ~0 |
| Snapshots | 1.59 | 1.22 | 22.5 | 17.2 |
| **The file** | **99.6** | **≈ 12.1** | **589** | **81–91** |

Against the budgets: ten players at about 400 units meet 100 MB an hour, barely, and only with the
engine-rule predictors; ten players with 1,500 units fighting miss it under every design measured;
a four-player game's 25 MB is not established (it holds only with about 100 moving units on
average). Snapshots become about 21 % of the lossless tail, so they are a target after all.

### What the lossy options still buy

The quantization researcher found that cutting precision beats cutting the rate: truncating the
position's fraction to 4 bits every tick gives an error under 0.09 wu, against 2.38 wu for 10 Hz,
for nearly the same size; and **never round** — 7.4 % of moving units sit at a fraction of exactly
0xFFFF, so rounding moves them into the next world unit and the wrong footprint cell (17.2 % of
moving unit-ticks under the contract's "10 Hz whole units" rounded). The challengers then showed:

- the lossless unit frame (3.75 MiB on that researcher's basis) is already smaller than its lossy
  stream (4.45), so truncation adds at most about 0.7 MiB;
- truncation is not free of consequences: the radar pass's bucket walk `0x47E890` tests distance
  on the full 16.16 positions (`0x47E995..0x47E9E6`) and stores `+0x110` bits 8–10, so contacts
  can flip at a range edge;
- speed and turn rate must stay exact, because a puppet's walk class `0x43DA70` derives its
  start/stop and move rate from them; with them exact, error-bounded sampling (ε 1 wu / 2°) or
  10 Hz saves about 1.0 MiB, not 2.5;
- projectiles: dropping corrections between snapshots about 1 MiB; the stale records past the
  pool's live count left out of snapshots 0.37 MiB (an exclusion, which needs every append proven
  to write all 0x6B bytes).

Lossy on top of lossless saves about 1–2 MiB on this battle, about 11 MiB an hour at the tail.
Every one is a loosening and the owner's decision.

### Gaps and risks

- **Multiplayer contributions**: below.
- **One flat battle**: no aircraft or hills (pitch, roll and vertical velocity never changed). L0
  (below) re-ran every codec on three other recordings: lossless everywhere, and aircraft are the
  one class that costs far more than the battle suggested.
- **Seek in C**: decode work grows with the units that move, since a unit at rest is skipped. L1
  MEASURED a seek under one second everywhere except the first snapshot interval of the two largest
  battles, 1.15–1.23 s, where every unit marches (*L1*, below): open, for L4's snapshot interval.
- **The writer and the ring**: L1's unit frame cost at most 95 ms for one block of 30 iterations on
  the writer thread, and the 16 MiB ring held at most 2.1 MiB (*L1*, below).
- **A codec bug's reach**: every codec decodes its block before writing and falls back to the
  records on a mismatch, so a bug costs size, never data.
- **Predictor resets**: the unit frame restarts at a segment's first stream block, at the first
  after a snapshot and at the first after a block written as records, a rule of the format, so a
  seek can start at any snapshot.

## L0: the codecs on other recordings

On 2026-10-07 each codec ran, its rules unchanged, on three recordings unlike the battle: the replay
gate's **Town & Country** battle (hilly; four seats, about 1,440 kbots, bases and factories, 12
aircraft; 4,021 ticks), its **naval** fixture (Anteer Strait, 3,841 ticks) and a new **air war**
(`scenarios/air-war.json` on Two Continents: 200 aircraft over flak-defended bases, about 96 still
flying after ARM's air force died; 5,610 ticks). The reports, scripts and outputs are in
`research/sizelab/l0/`.

**Every codec decoded every iteration back exactly on all three** (MEASURED: units, projectiles,
cells, orders, events, wrecks, globals, players, feature heads, the snapshot wreck base; the unit
decoder's state equals every snapshot). MiB, writer; the totals are sums of the measured categories
plus the small kinds, as for the battle:

| | the battle | Town & Country | naval | air war |
|---|---:|---:|---:|---:|
| the file today | 99.6 | 79.2 | 10.1 | 46.2 |
| units | 46.7 → 4.50 | 52.7 → 4.52 | 2.52 → 0.22 | 24.9 → **6.22** |
| projectiles (with the guided rule) | 36.5 → 2.55 | 15.1 → 1.05 | 5.01 → 0.35 | 3.37 → 0.35 |
| cells, orders, events, the rest | 10.2 → 3.64 | 9.38 → 2.41 | 1.04 → 0.58 | 15.3 → 2.59 |
| snapshots (wreck base) | 1.59 → 1.22 | 0.63 → 0.43 | 0.30 → 0.10 | 0.47 → 0.17 |
| **the file, lossless** | **≈ 12.1 (8.2×)** | **≈ 8.5 (9.3×)** | **≈ 1.3 (7.9×)** | **≈ 9.4 (4.9×)** |
| per hour at this fight's rate | 167 | 228 | 36 | 181 |

**Ground units and ships**: every engine rule stays exact (heading, velocity from the sine table,
position from velocity while unblocked, the stamp; the countdowns 98.3–99.9 %). Ships carry the
sticky blocked bit in 87 % of their unit-iterations, where position-from-velocity falls to 64–77 %
and the per-unit switching falls back to linear extrapolation at no visible cost. On hilly Town &
Country, kbots changed pitch and roll 0 times in 4.78 M unit-iterations; tracked vehicles on slopes
are untested [INFERRED: every ground type in that battle is a kbot].

**Aircraft are the finding.** Heading, position, the stamp and the countdowns stay exact, but the
velocity rule holds on only 2.6–5 % of airborne iterations: the air step (from `0x43D2C0`, partly
read) damps the velocity, rescales it against a speed cap in x87 floating point and adds thrust
along the heading. Pitch changes in 66 % of their iterations, vertical velocity in 9–13.5 %, and
three unidentified mover fields at `+0x14`, `+0x18` and `+0x1C` in 75–92 %. MEASURED: aircraft
are about 4.0 of the air war's 6.2 MiB of units, and 586 k of 602 k aircraft iterations still
carry a change. That is roughly ten times a ground unit's cost [INFERRED: about 0.7 MiB an hour
per aircraft here, against about 0.07 per unit at the battle's tail], so a game with a large air
force cannot meet the four-player budget on the L0 rules. The aircraft research (*Aircraft*, below)
found the rules that recover it.

**Projectiles**: guided flight, rare in the battle, is 43–63 % of the projectile stream on the
other three. The engine rebuilds a guided projectile's velocity from its own heading, pitch and
speed (DISASSEMBLED `0x49BA74..0x49BABE`), exact on 99.9 % of changing transitions on naval, 96 %
on the air war and 88 % on Town & Country (the rest is burst spray); as a survivor predictor it
decodes exactly and takes Town & Country 1.27 → 1.05 MiB, naval and the air war 0.49 → 0.35, the
battle unchanged. The `+0x67` stamp holds on 99.3–100 %. A shot event predicts its birth except for
the aircraft weapons and one burst weapon's templates, whose births sit 4–16 wu from the event's
fire point (804 of 2,031 births on the air war): size only. **The search cap**: at most 4
candidates per table section, the same shooter (`+0x52`) first, costs no size on any recording
(the battle 2.539 against 2.548) and bounds each searched record at 432 compares, so the battle's
busiest second falls from 288 M compares to 2.2 M; picking the candidates without scanning the pool
needs per-section lists keyed by weapon and shooter [INFERRED]. Unrestricted, the search on the
three new recordings stays under 0.6 M compares per iteration.

**Cells**: the air war's cells are 30.7 % of its file, because the engine re-files every airborne
unit in each cell's second slot (`+0x02`) every tick (the clear `0x47D0E0` and the stamp
`0x47CC30`), about 4.6 cells per moving aircraft, each sent twice. The tick-input repeat holds on
all three (1,367,178 of 1,369,928 image cells on the air war). The footprint predictor extended to
that slot predicts 89 % of its changes and cuts the tick input from 1,766 to 365 KB on the air war
and from 1,091 to 24 KB on Town & Country (about −1.4 and −1.0 MiB [INFERRED], not in the table).

**Fields the battle never moved**, absorbed by the codecs: wrecks' `+0x04/+0x06/+0x2E` on the
hilly maps and `+0x0C` (height) on naval; ships' second weapon reload; air war deaths differing in
12–17 dwords rather than about 5. The RNG step count is found for every change (at most 3,833).

**zstd 3 as a drop-in** on whole files: Town & Country 0.80, naval 0.63, the air war 0.76 (the
battle 0.69); decoding 1.8–3 times faster than the writer's inflate. Town & Country's blocks hold
only 5.8 iterations each, which is why its ratio is weak; the codecs' sectioned blocks replace them.

## Aircraft

Research and a challenge on 2026-10-08 (`research/sizelab/air/`, `air-challenge/`): what an
airborne unit's movement writes each tick, and which of it a lossless codec can predict.

**The step** (DISASSEMBLED, re-derived by the challenger, the constants checked against the exe).
The movement pass `0x43DD20` runs the air step `0x43D290` for a unit whose definition has `+0x241`
bit 11 set. In order: damping, a drift limit, a vertical clamp, the turn, a proportional-derivative
term toward the target point the course `0x490690` supplies, the speed, then `0x43D0D0`. The
target point, its change and the desired heading live in the course, not in the unit record.

- The mover's `+0x14/+0x18/+0x1C` is a **smoothed acceleration**: A ← ((A·0xF333) >> 16) + Δv.
- The unit's `+0x64`, which L0 called pitch, is the **bank**: round(atan2(−(x′·bankscale) >> 16,
  (gravity << 16) / 0xCCD)·K), where x′ is A turned by the heading (x cos − z sin). `+0x68` reads
  the same x′ with the type's pitch scale.

**Exact rules**, each on 588,452 of 588,452 airborne iterations of the air war (MEASURED): speed =
isqrt(v·v), the smoothed acceleration, the bank, `+0x68`, altitude ← altitude + vy, the footprint
cell from the position (`0x43D86F`), the mover's `+0x26` claim stamp (GameTime when the footprint
changes, `0x440A91`) and the sight x (x >> 21; the sight z holds on 91–100 %). The velocity is the
one field the record does not determine, because the target point is hidden: the
proportional-derivative term is saturated on 96 % of iterations, and predicting it as the last
term, or that term turned by its last change, is exact on 66–70 % in straight flight and 15–28 %
when turning.

| Unit frame, writer, MiB | air war | Town & Country | naval |
|---|---:|---:|---:|
| L0's rules | 6.224 | 4.524 | 0.215 |
| + speed, smoothed acceleration, bank, altitude | 3.029 | | |
| + footprint cell, sight, claim stamp | 2.166 | | |
| + the velocity estimate and the turned term | **1.334** | **4.034** | **0.206** |

Every run decoded back with 0 mismatches (859,616 unit images on the air war), re-run by the
challenger byte-identical. The cell rules help ground units too: they are 88 % of Town & Country's
gain. Aircraft fall from 6.05 to 1.20 MiB; the air war's file from about 9.4 to about 4.5 MiB
[INFERRED: the L0 total with the new unit figure]. What is left is the hidden target (about
0.34 MiB of velocity plus framing) and bucket fields and state bits (about 0.18), roughly 0.2 MiB
per aircraft-hour against 1.1 under L0 [INFERRED, scaled].

**Integers, not floats.** The engine computes the bank in x87 floating point at 53-bit precision
(the runtime sets `_PC_53` at start-up, `0x4EA5B0`). Reproducing it in floats would make the
format depend on the control word, which the DLL does not control by construction: the MinGW TLS
callback runs `fninit` on every new thread, so a writer thread starts at 64-bit precision, the DLL
has 482 compiler-generated `fldcw`, and GCC may move x87 arithmetic across an inline one; and the
Python reader would have to agree with the C one bit for bit, which neither the decode-before-write
check (same C predictions) nor the snapshot check (blind to one-tick errors) would catch. The
challenger's all-integer version of the bank and the turned term matches the engine's bank on
588,451 of 588,452 iterations and costs +0.24 % on the air war (−0.03 % on Town & Country), so
**the plan uses integers**: a prediction only has to be near, never bit-exact, for a lossless codec.
The integer version also fixes three holes the float prototype's specification had: clip the turned
term at 2^16 (the largest |A| measured is 26,214), division truncates toward zero as in C (Python's
`//` floors), and a zero speed cap is guarded.

**What L1's unit frame carries for aircraft** (built): the recording's header holds gravity, sea
level and LosType's bit 2 (`RT_PHYSICS`; gravity stays a `leave` row of `replay-globals.tsv`, since
the header is not the replayed globals) and, per aircraft type, a 24-byte row of its type index,
sight height and five flight fields (`RT_AIR`; type indices differ between recordings: CORVAMP is
271 on the air war, 272 on Town & Country). The capture writes them on the game thread at the
recording's start, and the writer parses them from the header's bytes, as a reader does. Each slot
keeps two images and five lanes of a third, and an age; the landing tick's rule is in (on
`0x43D210(1)` the smoothed acceleration decays once and the bank is recomputed); 0xF333, 0xCCD,
the integer tables and the sine table are constants of the format. Gravity and the definition
offset `+0x170` were fitted from the data (8155 is unique: 588,252 exact banks against about
503,000 for 8154 or 8156); the capture reads the real values (the engine map's *The demo
recorder's unit frame*).

**Not covered** by the air war: landings (its 200 transition iterations are all take-offs), air
transports and their cargo, the blocked branch, a non-zero pitch scale, the other LosType path
(`0x4826B7`), and remote aircraft in multiplayer. A wrong rule there costs size, never data.
Chasing the velocity further — estimating each aircraft's target from its turns (the turn reveals
the desired heading on 86 % of iterations) or recording the course's target (26 bytes an aircraft)
— is not measured and not in the plan unless the measured hour needs it.

## L1: recording format 8, measured

**What L1 is** (built, not landed). Recording format 8: every packed block is one zstd frame at
level 3 (zstd 1.5.7, vendored), and a stream block — at most 30 iterations or 2 MiB, ending at each
snapshot — is written as a **sectioned container**: the record kinds as runs, one section of
verbatim bodies per category (projectiles, cells, orders, events, the rest), and the **unit
frame**, which replaces every unit delta with residuals against per-lane predictions — the engine's
rules (heading, ground and air velocity, position, the footprint and sight cells, the claim and
move stamps, the countdowns, the aircraft's speed, smoothed acceleration and bank), the previous
value or the linear extrapolation — all in integers. The writer writes a sectioned block only when
a second decoder gives its records back byte for byte, else the records. Both readers take format 8
alone (decision 6). The bytes, the reset points, the fallback and the refusals are the skirmish
slice's [*The file*](demo-recorder-skirmish-slice.md#the-file); every engine fact the rules rest on
is in the engine map's
[*The demo recorder's unit frame*](../exe-reverse-engineering.md#the-demo-recorders-unit-frame-the-engine-rules-it-predicts-from-disassembled-measured-2026-10-08).

**Where it departs from the plan above**, each decided in the building:

- **The unit frame's state runs across blocks** between reset points (a segment's first stream
  block, the first after a snapshot, the first after a block written as records); only each
  block's zstd frame is self-contained.
- **Each recording carries its lane table** (`RT_CODEC`, from `tagpu/ddraw/replay-lanes.tsv`, 112
  lanes), so editing the table never orphans a file; the rules find their lanes by role.
- **The capture writes the constants** (`RT_PHYSICS`, `RT_AIR`) into the header on the game
  thread; the writer, like a reader, parses them from the header's bytes.
- **A third image is five lanes** (heading, layer, velocity), not a whole image.
- **A unit at rest is skipped**: one whose image equals the two before it and the third's lanes and
  which is not due a move stamp is a fixed point of every rule, so decode work scales with the
  units that changed in the last three iterations.
- **The Python reader** (`tools/replay/reccodec.py`, numpy) is an independent implementation,
  checked byte-identical with the C one.

**Offline, on the four format-7 recordings** rewritten as format 8 (MEASURED 2026-10-08, native and
under Wine): every stream block sectioned, 0 declined, 0 mismatched; the C and Python decoders and
the records identical on every block. The unit frame, zstd 3 / the old deflate, MiB: air war
1.375 / 1.333, Town & Country 4.151 / 4.027, naval 0.213 / 0.205 (the research: 1.38 / 1.338,
4.16 / 4.034, 0.206), the battle 4.342 / 4.214 against the research's 4.634 / 4.497, which had no
footprint, sight or claim rule (without them the codec gives 4.619 / 4.486). 200,000 fuzzed blocks
under ASan and UBSan gave no report, and C and Python agreed on 3 × 2,000 fuzzed cases.

**In the game** (MEASURED 2026-10-08 on the reference setup: the default build at `a45d84ac`,
recorder armed, game speed 20, each scenario played anew under Wine). The format-7 figures are
L0's recordings of the same scenarios, which ran to other GameTimes and fought differently, so
**the comparison is not like for like**.

| | the battle | Town & Country | naval | air war |
|---|---:|---:|---:|---:|
| ticks, format 8 (format 7) | 7,410 (7,830) | 4,261 (4,021) | 3,811 (3,841) | 5,911 (5,610) |
| **the file, MiB, format 8** | **29.09** | **18.97** | **4.76** | **14.15** |
| the file, MiB, format 7 | 99.6 | 79.2 | 10.1 | 46.2 |
| stream blocks sectioned / declined / mismatched | 263 / 0 / 0 | 148 / 0 / 0 | 128 / 0 / 0 | 198 / 0 / 0 |
| the unit frame on the writer thread, ms in all / at most for one block | 8,136 / 70 | 7,419 / 95 | 565 / 21 | 3,718 / 29 |
| the ring's high water, KiB of 16 MiB | 2,164 | 1,748 | 945 | 1,061 |
| a seek's read, ms, to GameTime 900 / 1800 / 2700 / 3600 | 1,227 / 671 / — / — | 1,151–1,195 / 917 / 725 / 596–606 | 148 / 58 / 28 / 23 | 343 / 352 / 318 / 284 |
| `codectest.py verify`: blocks where C and Python differ | 0 | 0 | 0 | 0 |
| Python decode, `tarec.py info`, s | 28.9 | 27.9 | 3.2 | 16.2 |

The largest sections, each packed alone with zstd 3 (`recsize.py`; together within 2 % of the
stream as written), MiB: the battle — projectiles 15.0, the unit frame 4.76, cells 2.59, events
2.21, orders 1.93; Town & Country — projectiles 8.44, the unit frame 4.07, cells 3.52, orders 1.10;
naval — projectiles 3.24, cells 0.33; the air war — cells 7.49, projectiles 3.33, the unit frame
1.61. The seek's read is the replay's own log line (`sought … in X ms (read Y, rebuild Z)`); a seek
to 900 reads from the segment's first iteration, there being no snapshot before it.

**Against the plan.**

- **Size.** L1's row planned about −42 MiB on the battle, from the units and the interleaving. It
  measured −70 MiB, on a run that is not the same game: the sections also let zstd pack the
  categories L1 carries verbatim far tighter than the old interleaved 2 KB-window deflate — the
  projectiles' section packs to 15.0 MiB where the format-7 battle's projectiles packed alone to
  36.5. The unit frame came to 4.76 MiB against the plan's 4.5–4.6, on a run with 473 unit deltas an
  iteration where the format-7 recording had 385; on that recording itself it gives 4.34.
- **The writer.** The plan estimated about 120 ms of codec work per game second at 2,000 moving
  units [INFERRED]; the most for one block of 30 iterations (one second at normal speed) was 95 ms,
  and the ring never held more than 2.1 MiB of its 16. No recording declined a block or failed its
  check.
- **The seek.** Every seek read in under one second except those in the first snapshot interval of
  the two big battles, **1.15–1.23 s**, over the one-second target: decode work scales with the
  units that move, and in the opening every unit marches. This is **open**: the snapshot interval
  (`SNAPSHOT_TICKS`, 900) is tuned at L4, which measures the hour it is chosen for (the owner's
  decision).
- **Python.** Decoding a recording in Python takes about 28 s for the large battles, within the 30 s
  the plan allowed, and the replay gate runs `tarec.py` many times a fixture: `TAREC_CACHE=DIR`
  (`tools/replay/reccache.py`) keeps a recording's decoded blocks keyed by the SHA-256 of its bytes
  and of the decoder's sources, `tareplay.py` sets it for each run, and with the cache warm `tarec.py
  info` on Town & Country takes 2.0 s.

**What L1 leaves open.**

- **The seek in a large battle's first interval** (above), for L4.
- **Aircraft in the replay gate**: the fixtures fly aircraft only in `carry-links` (transports) and
  `battle` (a dozen AI-built ones); whether they reach the codec's air functions is the landing
  gate's answer. If the gate names them UNCOVERED, the owner's choice is a short `air-war` fixture
  (`scenarios/air-war.json`).
- **Never met in a recording**, each costing size only if a rule is wrong: the circle's sight path
  (LosType bit 2 clear), a landing, air transports' cargo, a non-zero pitch scale, remote aircraft
  in a multiplayer recording.
- **The other categories ride verbatim**: projectiles, cells, orders and events are L2's and L3's;
  a multiplayer file's contributions stay records blocks, zstd-packed (K is L6).

## Multiplayer recordings

A multiplayer file holds the recording machine's world (every unit, remote players' replicated
copies included) and, in `RB_CONTRIBUTIONS` blocks, each owner's contributions as the exact
packets received, with their ledgers, seals and receipts; the playback composes the two (*Owner-
authority materializer*, [the multiplayer slice](demo-recorder-multiplayer-slice.md)). The
contributions exist because no machine holds the other players' true state: TA replicates units
with a lag and does not replicate orders, queues, resources or cameras. Keeping the packets kept
the transport proof re-checkable and let the composition's rules change after recording (on
2026-10-06 the state byte, build finish and explored map rules changed and were re-applied to
existing files).

MEASURED on three recordings (two-peer, its chat-fix variant, the five-peer ten-seat game):
contributions are 25.9 %, 27.5 % and 49.0 % of the files on disk; their 80-byte headers are 38 % of
the raw two-peer contributions; the recorder's own packets repeat its stream exactly (24,288 of
24,288 unit images and 153 of 153 player images on the two-peer file; 2,711/2,711 and 206/206 on
the ten-seat), and of the observed remote copies the playback uses only the level-start images and
the comparison's contact bits.

**Two alternatives, both lossless, both replacing the slice's file design.** MEASURED by the
researcher and re-measured by a challenger (like for like, zlib on both sides; the recordings: two-
peer, its chat-fix variant, the five-peer ten-seat game):

| | two-peer | chat-fix | ten-seat |
|---|---:|---:|---:|
| contributions today, share of the file | 25.9 % | 27.5 % | 49.0 % |
| only the recorder's own packets dropped | −10 % | | −9 % |
| **K**: keep the remote packets, compactly | **−22.0 %** | **−23.7 %** | −40.6 % |
| **the composed world** | −19.0 % | −21.4 % | **−46.2 %** |

- **The composed world.** The writer composes at record time what the playback composes today (its
  own units from its simulation, each remote unit's owner-authoritative fields from that owner's
  contributions) and records one world through the normal stream, with a per-source proof footer of
  about 0.7 KiB in place of the packets. The composition needs no engine state (`mp_compose`,
  `mp_authority` and `mp_world` read only the reader's history and the primary world), but the
  merged stream also needs a bootstrap marker and the owners' births. Its timing rule must be: every
  record labelled T or earlier is published before the game thread advances to T+1, so the writer
  composes image T when it sees `RK_TICK` T+1 — not "the contributions for T+1 precede `RK_TICK`
  T+1", which is off by one (clicks go out between steps, post-capture roots and chat at the next
  step's start, `mp_events_flush`). It loses: a reader's re-check of each source's sequence and CRC
  chain, re-composing old files under new rules (in use: the state-byte and explored-map rules were
  re-applied to existing recordings on 2026-10-06, and leavers and an owner build-done root are the
  next rule changes), the independent Python composition (`recmp_compare`), the tests that read the
  persisted packets (`recmp_lifecycle_test.py`, `recmp_create_begin_test.py`, `recmp_compare
  --fork`), and the ability of a later build to play a file this build's writer refused.
- **K.** Drop the recorder's own state packets and seals (MEASURED: they repeat its stream exactly,
  every local slot at every iteration: units, order lists, player records and the explored-map
  bit); keep each remote packet with a 5–6-byte header from which the exact 80-byte one is rebuilt;
  store each seal as its tick, final flag and attested CRC; drop the observed remote unit records
  after the first iteration, keeping the few contact bits the comparer reads. Every kept record
  rebuilt byte-exact, and every attested CRC matched the chain recomputed from the stored packets.
  K keeps the re-checkable proof, re-composition under new rules, the comparer, the native tests
  and today's materializer; its seek replays the packets linearly through a snapshot interval,
  which composition checkpoints in the snapshots would bound [INFERRED].

Both drop the observed copy of remote units, and with it the audits of a replica against its
owner. The composed world wins on the ten-seat file (80 KiB: remote unit packets and seals), a gap
that should close once the unit codec applies to the owners' sequences, which code smaller than
the composed ones [INFERRED]. **The owner chose K (2026-10-07)**, with snapshot checkpoints for the
seek; the composed world is reconsidered once the composition rules settle. K needs one measurement
before it lands: the remote packets' unit bodies coded with the unit frame in the file and rebuilt
byte-exact (MEASURED so far only as the owners' unit sequences coded: 84.1 KiB on the two-peer
file, 16.5 on the ten-seat). If the wire codec of stage 6 lands first, the packets arrive already
coded and the file stores them as they are.

Nothing on the wire changes either way. A chained unit codec on the wire shrinks unit bodies
4.05–5.16 times (MEASURED, every tick decoded back); it is safe by construction because the
receiver accepts only the last sequence plus one and the session refuses
`DPSESSION_NOPRESERVEORDER` (`tagpu_recmp_net.c:288`). After it, the per-tick seals cost more than
the units: stage 6's traffic budget.

## Landing order

| | Landing | Gives |
|---|---|---|
| L0 | every codec run offline on the naval and Town & Country recordings and one with aircraft and hills: DONE 2026-10-07, lossless everywhere (*L0* above) | aircraft as the open cost |
| L1 | the sectioned container, decode-before-write with fallback, the unit frame with the aircraft and cell rules in integers: BUILT, measured, not landed (*L1* above) | planned about −42 MiB on this battle; MEASURED 99.6 → 29.1 MiB on a new run of it, not like for like |
| L2 | projectiles by identity, the guided-velocity rule, the search capped at 4 candidates, shooter first | about −34 MiB |
| L3 | cells, orders, events, globals; the snapshot wreck base | about −7 MiB |
| L4 | the measured hour of a four-player game with AI and structures | the snapshot interval; whether lossy is needed |
| L5 | lossy, only if L4 misses a budget | about −1 MiB per category |
| L6 | multiplayer: K (with or after L1), its snapshot checkpoints, then the wire codec in stage 6 | multiplayer files and bandwidth |

## Decisions

Taken by the owner on 2026-10-07:

1. **Lossless first**, instead of the earlier contract's 10 Hz whole-unit motion; the product
   page's *Disk budget* now says so.
2. **The prediction tier: the engine rules** (4.50 MiB): integrators plus velocity from the sine
   table × speed; the rules and the table are constants of the format version.
3. **Field lanes**: a table of the unit and mover records' field widths (a wrong row costs size,
   never correctness).
4. **zstd level 3** in the DLL (BSD-3, about +0.6 MB) for writer CPU and seek speed; the
   `zstandard` package in the development venv for `tarec.py`. Players are unaffected.
5. **numpy** may be a dependency of `tarec.py` (development tooling only).
6. **Formats 5–7 are dropped** when the new format lands (no build writing them was published).
7. **Projectile engine facts are used**: shot events as references for births and the `+0x67`
   slot stamp as the survivor's previous slot.
8. **Loosenings are decided after L4**, with the measured shortfall in hand, if there is one.
9. **Projectiles from launch: not now**; revisited only if L4 misses a budget.
10. **Multiplayer: K**, accepting the loss of the replica-against-owner audits.
11. **The measured hour runs accelerated**, within the ten-minute rule for a game run, with one
    ten-minute stretch at normal speed as a cross-check for anything paced in real time (cameras,
    cursors).
