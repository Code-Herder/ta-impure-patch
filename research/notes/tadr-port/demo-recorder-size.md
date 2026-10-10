# Demo recorder: making recordings smaller

## Status

**L1 to L4 are landed** (*L1: recording format 8, measured*, *L2: the pool frame, measured*,
*L3: the record frames, measured* and *L4: the measured hours, the snapshot interval and R1*,
below). Recording format 11 packs every block with zstd, carries the unit deltas in a unit frame
predicted from the engine's own rules, the projectiles in a pool frame coded by identity, and the
cells, orders, events and state in four record frames, of which the cells frame restarts from the
reader's grid at every reset point; it writes a snapshot every 1,800 ticks (60 s of game time), and
its snapshots store only the wreck records that differ from a base: the battle below, recorded
anew, takes 12.00 MiB where the measured one took 99.6 (not like for like: another game of the same
scenario), and on the very games L1 recorded the battle's file goes from 29.09 MiB (L1) to 16.40
(L2) to 11.88 (L3), Town & Country's from 18.97 to 11.51 to 8.04. **L4 measured two four-player
hours, and both miss the 25 MB budget even with no snapshots at all** (24.4 and 51.5 MiB an hour
from the stream alone): whether to go lossy (L5) or to look for more lossless savings is the owner's
decision. L5 and L6 are plans. The page gathers what has been measured about the size of a demo recording, the research
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

**As built** (format 9's pool frame, *L2* below). The research's codec, run at zstd 3 on L1's four
recordings, gives 2.33 / 1.13 / 0.41 / 0.55 MiB (the battle, Town & Country, naval, the air war);
the pool frame gives 2.09 / 0.99 / 0.39 / 0.41. A survivor is predicted dword by dword from its
own last image, the rule the engine's tick follows where there is one — position by the velocity
with the last step's drift, the guided velocity from heading, pitch and speed, the `+0x67` stamp
as its old slot — and keeps, per dword, whichever of the rule, the previous value and the linear
extrapolation was closest last. The guided rule applies only to a record whose last image already
follows it (the codec carries no weapon table, so it cannot test the weapon's bit 20): applied to
every survivor it cost the battle 0.20 MiB. A birth or a changed stale slot is coded against a
reference row in one of four sections — the new pool so far, the old pool, the old pool advanced a
step, or a row made from the last iteration's shot events — and a birth whose reference row
follows the guided rule takes its velocity from the rule as well. The search is capped by
construction: the matcher tries the stamp and then an eight-wide window; a birth's candidates
come from per-weapon chains and a (weapon, shooter) table, at most 32 probes and 4 candidates a
section; a stale slot looks ±16 rows around each section's anchor.

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

Two more: **the snapshots' wreck records**. The level load links each free record to its
neighbours, so none is zero, and a snapshot that stores every record that is not zero stores all
8,192; storing only those that differ from the pool at the end of the segment's first iteration
saves 0.37 MiB here and, in a small game, most of each snapshot (51 of 58 KB in the ten-seat
fixture, about 6 MiB an hour). Format 10's snapshots store only those (*L3*, below: on the battle,
5,524 wreck records in its 8 snapshots instead of 65,536). And the cells could be derived from the
units' footprint moves (99.6 % predicted, about 0.6 MiB more [INFERRED]; not in L3). Lower rates or
priority sampling are not worth it here: after coding, sprays and replies cost under 24 KB.

**As built** (format 10's record frames, *L3* below). Each category is a frame of columns that
restarts empty at every reset point, coded against what it saw since: the cells against a copy of
the grid and the last iteration's tick input, the order lists node by node against the last whole
image of the same (slot, list), the events field by field against the last ID, the unit copy and
dictionaries, and the globals as deltas of deltas or as Park–Miller step counts, the players by
delta, the wreck records by a mask of changed u16s and the feature heads and cursor against their
rule. In a new recording of the battle the four take 991 / 825 / 1,407 / 337 KiB (cells, orders,
events, state, each packed alone with zstd 3).

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
a four-player game's 25 MB held only with about 100 moving units on average, and L4's measured
hours miss it (*L4*, below). Snapshots become about 21 % of the lossless tail, so they are a target after all.

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
- **Seek in C**: decode work grows with the units that move, since a unit at rest is skipped, and
  with the cells the record frames decode. A seek reads from the last snapshot, so its worst case
  is one snapshot interval of decode: with snapshots 1,800 ticks apart that is 261–265 ms on L4's
  light hour, 538–550 ms on its heavy one, and about 1.7 s of decode on the big battle, a cost the
  owner accepted for dense battles (*L4*, below).
- **The writer and the ring**: L1's unit frame cost at most 95 ms for one block of 30 iterations on
  the writer thread, and the 16 MiB ring held at most 2.1 MiB (*L1*, below).
- **A codec bug's reach**: every codec decodes its block before writing and falls back to the
  records on a mismatch, so a bug costs size, never data.
- **Predictor resets**: every frame restarts at a segment's first stream block, at the first
  after a snapshot and at the first after a block written as records, a rule of the format, so a
  seek can start at any snapshot. The unit, pool and cells frames restart from the world a reader
  holds there; the orders, events and state frames restart empty (*L4*, below).

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

**What L1 is** (landed 2026-10-08). Recording format 8: every packed block is one zstd frame at
level 3 (zstd 1.5.7, vendored), and a stream block — at most 30 iterations or 2 MiB, ending at each
snapshot — is written as a **sectioned container**: the record kinds as runs, one section of
verbatim bodies per category (projectiles, cells, orders, events, the rest), and the **unit
frame**, which replaces every unit delta with residuals against per-lane predictions — the engine's
rules (heading, ground and air velocity, position, the footprint and sight cells, the claim and
move stamps, the countdowns, the aircraft's speed, smoothed acceleration and bank), the previous
value or the linear extrapolation — all in integers. The writer writes a sectioned block only when
a second decoder gives its records back byte for byte, else the records. Both readers took format 8
alone (decision 6); format 9 (L2, below) keeps all of it and replaces the projectiles' section. The bytes, the reset points, the fallback and the refusals are the skirmish
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
- **The Python reader** (`tools/replay/reccodec.py`, numpy and numba) is an independent
  implementation, checked byte-identical with the C one.

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
  units that move, and in the opening every unit marches. L4 chose the snapshot interval on two
  measured hours and the owner relaxed the target for dense battles (*L4*, below).
- **Python.** Decoding a recording in Python takes about 3 s for the large battles, as long as
  the C decoder under Wine: Town & Country 3.1 s (C 3.4 s), the size battle 3.4 s (3.6 s), the air
  war 1.6 s, the naval 0.5 s (`codectest.py verify`, MEASURED 2026-10-08 with numba's cache warm;
  the first decoding after an edit of `reccodec.py` compiles its kernels for about 8 s more). The
  unit frame runs in numba kernels, one call a block and one slot at a time. With no
  `TAREC_CACHE`, `tarec.py check` on Town & Country takes 9.8 s, its world replay most of it, and
  `info` 4.7 s. The replay gate runs `tarec.py` many times a fixture: `TAREC_CACHE=DIR`
  (`tools/replay/reccache.py`) keeps a recording's decoded blocks, one piece per segment keyed by
  the SHA-256 of the file's bytes to the piece's end and of the decoder's sources, and `tareplay.py`
  sets it for each run. A segment starts a new unit frame, so a takeover's file reads its parent's
  pieces and decodes only the fork it appended.

**What L1 leaves open.**

- **The seek in a large battle's first interval** (above): decided at L4, which accepts about 2.5 s
  there (*L4*, below).
- **Aircraft in the replay gate**: the fixtures fly aircraft only in `carry-links` (transports) and
  `battle` (a dozen AI-built ones); whether they reach the codec's air functions is the landing
  gate's answer. If the gate names them UNCOVERED, the owner's choice is a short `air-war` fixture
  (`scenarios/air-war.json`).
- **Never met in a recording**, each costing size only if a rule is wrong: the circle's sight path
  (LosType bit 2 clear), a landing, air transports' cargo, a non-zero pitch scale, remote aircraft
  in a multiplayer recording.
- **The other categories ride verbatim**: projectiles (L2's, below), cells, orders and events (L3's);
  a multiplayer file's contributions stay records blocks, zstd-packed (K is L6).

## L2: the pool frame, measured

**What L2 is** (landed). Recording format 9: format 8 with the sectioned container's projectile
section replaced by the **pool frame**, the projectile pool coded by identity. An iteration says
which old records died, how many were born and which stale slots past the count changed; a
survivor is predicted from its own last image by the projectile tick's rules (*As built*, under
*Projectiles* above), a birth and a changed stale slot are coded against a reference row, and
RK_PROJ_HEAD, RK_PROJ and RK_PROJ_DELTA keep their place in the kinds but have no body: the
decoder makes each from the pool it rebuilds. The writer still writes a sectioned block only
when a second decoder gives the records back byte for byte, and both frames restart at the same
reset points, from the reader's units and pool. No header entry is added: the frame's one
parameter is RT_PROJECTILES, which every header carries. Both readers take format 9 alone
(decision 6); format 8 was never published. The bytes are the skirmish slice's
[*The file*](demo-recorder-skirmish-slice.md#the-file); the engine facts are the engine map's
[*The demo recorder's pool frame*](../exe-reverse-engineering.md#the-demo-recorders-pool-frame-the-engine-facts-it-predicts-from-disassembled-measured-2026-10-08).

**Where it departs from the plan above**, each decided in the building:

- **A new format, not a flag in format 8**: one decode path for the projectiles in each reader is
  what keeps the two readers' refusals provably equal (`codectest.py fuzzdiff`).
- **The guided rule tests the image, not the weapon**: the codec carries no weapon table, so it
  applies the rule to a record whose last image already follows it; applied to every survivor it
  cost the battle 0.20 MiB.
- **The rule reaches births too**: a birth whose reference row follows it takes its velocity from
  the rule.
- **The search is capped by construction** (*As built*, above), so the writer's work for a block
  has a bound whatever the game does.

**Offline, like for like**: L1's four recordings rewritten as format 9 — the same games, every
stream block decoded by L1's reader and encoded again by L2's writer codec (MEASURED 2026-10-08,
native and under Wine, the two writing identical bytes): every block sectioned, 0 declined, 0
mismatched, the C and Python decoders and the records identical on every block.

| | the battle | Town & Country | naval | air war |
|---|---:|---:|---:|---:|
| **the projectiles, packed alone, MiB, format 9** | **2.088** | **0.986** | **0.387** | **0.414** |
| the projectiles, packed alone, MiB, format 8 | 14.96 | 8.44 | 3.24 | 3.33 |
| **the file, MiB, format 9** | **16.40** | **11.51** | **1.92** | **11.18** |
| the file, MiB, format 8 | 29.09 | 18.97 | 4.76 | 14.15 |
| the writer's codec under Wine (four files at once), ms in all / at most for one block | 8,571 / 75 | 7,609 / 91 | 594 / 26 | 3,833 / 30 |
| the C decoder under Wine, ms in all, format 9 (format 8) | 3,765 (3,559) | 3,369 (3,278) | 251 (220) | 1,671 (1,632) |
| the longest run from a reset point, ms, format 9 (format 8) | 1,142 (1,063) | 1,025 (1,048) | 149 (128) | 316 (306) |
| Python, `codectest.py verify`, s | 3.8 | 3.2 | 0.6 | 1.7 |

The projectiles land within 0.5 % of the format's prototype (2.09 / 0.99 / 0.39 / 0.41). On the
battle the pool frame's 2.09 MiB is the survivors' columns 0.92, the births' 0.97, the stale
tail's 0.09 and the control values and reference codes 0.13. 200,000 fuzzed naval blocks under
ASan and UBSan gave no report, and C and Python agreed on 2 × 2,000 fuzzed cases.

**In the game** (MEASURED 2026-10-08 on the reference setup: the default build, recorder armed,
game speed 20, each scenario played anew under Wine, so **not like for like** with L1's runs; a
naval-push smoke on the stock-limits build wrote 39 sectioned blocks, 0 declined, 0 mismatched).

| | the battle | Town & Country | naval | air war |
|---|---:|---:|---:|---:|
| ticks | 7,411 | 4,201 | 3,840 | 5,940 |
| **the file, MiB** | **15.30** | **11.80** | **1.83** | **8.19** |
| the projectiles, packed alone, MiB | 1.78 | 0.96 | 0.35 | 0.26 |
| projectile records (L1's game) | 588,905 (666,251) | 368,426 (357,567) | 126,631 (135,483) | 68,264 (156,799) |
| stream blocks sectioned / declined / mismatched | 263 / 0 / 0 | 146 / 0 / 0 | 129 / 0 / 0 | 199 / 0 / 0 |
| the codec on the writer thread, ms in all / at most for one block (L1) | 8,169 / 75 (8,136 / 70) | 8,168 / 97 (7,419 / 95) | 642 / 23 (565 / 21) | 2,937 / 36 (3,718 / 29) |
| the ring's high water, KiB of 16 MiB (L1) | 2,022 (2,164) | 1,718 (1,748) | 981 (945) | 1,043 (1,061) |
| a seek's read, ms, to GameTime 900 / 1800 / 2700 / 3600 | 1,259 / 638 / — / — | 1,219 / 1,013 / 994 / 655 | 175 / 74 / 34 / 23 | 352 / 261 / 197 / 184 |
| `codectest.py verify`: blocks where C and Python differ | 0 | 0 | 0 | 0 |
| Python decode, `tarec.py info`, s | 5.2 | 5.1 | 1.0 | 2.3 |

The battle's and the air war's projectiles are 15 % and 38 % below the offline figure because
these games fired less (the records' row); per record the battle's frame is within 4 % of the
offline one.

**Against the plan.**

- **Size.** The landing order planned about −34 MiB, measured against the pre-L1 file; on L1's
  own files L2 takes −12.7 MiB from the battle (29.09 → 16.40), −7.5 from Town & Country, −2.8
  from naval and −3.0 from the air war, because L1's zstd had already packed the battle's
  projectiles from 36.5 to 15.0 MiB.
- **The writer.** The most for one block was 97 ms (Town & Country), as L1's 95; the ring never
  held more than 2.0 MiB. The encoder's work is bounded by construction (`pool_encode`): the
  matcher tries 8 old slots, a birth at most 32 table probes and 4 candidates a section, a stale
  slot ±16 rows.
- **The seek.** Like for like, the pool frame adds up to 8 % to a seek window's decode on the
  large games (`codectest` decode under Wine, per 900-tick window) and 21 ms (+17 %) to naval's
  first window, whose stream is mostly projectiles. In
  the new games naval's seeks read 175 / 74 ms against L1's 148 / 58, and Town & Country's seek
  to 2,700 994 ms against 725 — that game's window is busier (its blocks decode in 795 ms there
  against 650 for L1's game). Every seek reads in under one second except in the first interval
  of the two large battles, 1.22–1.26 s, and Town & Country's seek to 1,800, 1.01 s: the seek in a
  large battle is L4's (*What L1 leaves open*).
- **Memory.** The pool frame adds 613 bytes a projectile record and about 1.4 MB a codec (the
  last birth of each weapon, the shot events and their rows, the encoder's search tables): about
  3.3 MB a codec at 3,000 records; the writer holds two codecs, a reader one.

**What L2 leaves open.**

- **The survivors' dword 23** (`+0x5C..+0x5F`, the ground under the last cell hit, written by the
  collision `0x49B090`) is the largest residual column, 214 KiB of the battle's 2.09 MiB.
- **Births are still half the frame**: a birth's spray and guidance are not predicted, and a
  burst copy's spray draws the simulation's random numbers.
- **The snapshots' projectiles stay verbatim**: 439 KB packed of the battle's 1,483 KB of
  snapshots.
- **The stock-limits build's shot rows** may name the wrong weapon (the high nibble of the
  packet's `+0x23` [INFERRED]): size only.
- **A crafted file can make a seek slow, not unsafe**: the reader copies the pool's extent every
  iteration (`pool_decode`), so a header claiming `RT_PROJECTILES` 65,535 and one extent that
  grows to it costs about 7 MB of copying an iteration, about 1.8 GB for a 256-iteration block,
  on the game thread during a seek. Every bound holds; only the time does not. Our writer's
  pools stay at their engine's 3,000 records (about 324 KB an iteration). Closing it needs the
  extent's growth bounded by the section's bytes, or a copy of only the changed slots.

## L3: the record frames, measured

**What L3 is** (landed). Recording format 10: format 9 with the verbatim bodies of the cells, the
orders and the events, and a new eighth section for the state, each replaced by a **record frame**
— a section of columns, at most 32, that makes its kinds' bodies — and with the snapshots' wreck
records coded against a **wreck base**:

- **the state frame** (`RCS_STATE = 7`, 19 columns): `RK_GLOBALS`, `RK_GLOBALS_DELTA`,
  `RK_PLAYER`, `RK_PLAYER_DELTA`, `RK_WRECK` and `RK_FEATURES`, which until format 9 rode in the
  verbatim section; a global by delta of deltas, by a count of Park–Miller steps or raw;
- **the events frame** (31 columns): one column a field, a shot's points against its units, a
  death's image against the unit copy, names and texts by dictionary;
- **the cells frame** (9 columns): against a copy of the grid and the last tick input, which the
  next iteration's images usually repeat; the tick input kept up to `CELLS_HELD`, 16,384 cells;
- **the orders frame** (26 columns): a re-sent list as the dwords that changed against its last
  whole image, in an arena of at most 65,536 nodes;
- **the wreck base**: a snapshot's `RK_WRECK`s are only the records that differ from the pool at
  the end of the segment's first iteration, which the writer (`iteration_done`), the C reader
  (`tick_take`) and `tarec.py` (`stream_to`) each take at that moment from the pool that iteration
  built from zero, so the bases are equal by construction.

Every record frame restarts **empty** at every reset point, and its state is a function of the
records since then and the unit copy alone; a frame never declines in mid-block, it latches and
its section's write declines the block. The `RecSecHead` is 40 bytes. L3's readers took format 10
alone (decision 6), as L4's take format 11 alone; format 9 was never published. The bytes and the refusals are the skirmish
slice's [*The file*](demo-recorder-skirmish-slice.md#the-file); the engine facts are the engine
map's
[*The demo recorder's record frames*](../exe-reverse-engineering.md#the-demo-recorders-record-frames-the-engine-facts-they-predict-from-disassembled-measured-2026-10-10).

**Where it departs from the plan above**, each decided in the building:

- **Restart empty (R2), not from the reader's world (R1).** The only outside state a record frame
  reads is the unit copy, which every reset already restarts from the reader's units in the
  encoder, the check decoder and every reader alike; so a frame's state is a function of the
  records since the reset, and a decline or a refusal needs no rollback. It costs the cells their
  history at each reset: +124 / +107 / +488 KiB on the battle, the replay gate's Town & Country
  battle and the air war (MEASURED by L3's encode-only prototypes on format-9 files).
- **The state is a section of its own**, so `RCS_OTHER` keeps only what no frame codes: the ticks,
  `RK_UNIT` and `RK_UNIT_GONE` (which the decoder's first pass reads) and every other kind.
- **The Park–Miller step is a size rule**, not a claim on the engine: a global it does not reach
  within the search goes by delta or raw. The encoder searches up to 16,384 steps only for a
  global the step carried last time, else 64, and at most two dwords an iteration take it.
- **Not in L3**: the cells derived from the units' footprints (−0.6 / −1.0 / −1.4 MiB on the battle,
  Town & Country and the air war [INFERRED by the research]), and R1, which would win back R2's
  cost.
- **The frames' copy of a decoder is test-only**: only the codec's self-test copies one
  (`RECF_TEST_COPY`, defined by its builds alone and listed in `tafeat.py`'s `TEST_ONLY`), so the
  DLL carries no function nothing in it calls.
- **A test lever forces a decline**: `tagpu_rec.decline` and the `codec-decline` and
  `stock-codec-decline` fixtures exercise the records block and the restart after it, through the
  encoder's first decline branch only (the skirmish slice's
  [*The file*](demo-recorder-skirmish-slice.md#the-file)).

**Offline, like for like**: L2's four recordings rewritten as format 10, the same games, with the
wreck base's saving on their snapshots counted in (MEASURED 2026-10-10 on HEAD's DLL):

| | L2, MiB | L3, MiB | change |
|---|---:|---:|---:|
| the battle | 16.40 | 11.88 | −27.6 % |
| Town & Country | 11.51 | 8.04 | −30.1 % |
| naval | 1.92 | 1.32 | −31.5 % |
| the air war | 11.18 | 5.64 | −49.5 % |

The C decoder under Wine, ms in all / the longest run from a reset point, format 10 (format 9):
the battle 3,690 / 1,122 (3,765 / 1,142), the air war 1,722 / 339 (1,671 / 316). On the battle's
file the 8 snapshots carry 5,524 wreck records instead of 65,536.

**In the game** (MEASURED 2026-10-10 on the reference setup: HEAD's DLL, recorder armed, game
speed 20, each scenario played anew under Wine, so **not like for like** with L2's runs). Each
scenario was recorded twice: once with the machine at a load average of about 30 (the files the
size, the sections, the seeks and the checks below are measured on) and once at about 3, for the
writer's cost; each recording is its own game.

| | the battle | Town & Country | naval | air war |
|---|---:|---:|---:|---:|
| ticks | 7,441 | 4,260 | 3,811 | 5,041 |
| **the file, MiB** | **12.00** | **8.74** | **1.26** | **5.26** |
| stream blocks sectioned / declined / mismatched | 266 / 0 / 0 | 149 / 0 / 0 | 128 / 0 / 0 | 169 / 0 / 0 |
| a seek from a fresh process, ms, to GameTime 900 / 1800 / 2700 / 3600 (load about 3) | 1,263 / 684 / 539 / 398 | 1,105 / 1,124 / 827 / 705 | 155 / 71 / 34 / 24 | 361 / 390 / 332 / 255 |
| `codectest.py verify`: blocks where C and Python differ | 0 | 0 | 0 | 0 |
| `tarec.py check`: snapshots, differing | 8, 0 | 4, 0 | 4, 0 | 5, 0 |
| the codec on the writer thread at load 30, ms in all / at most for one block | 12,924 / 134 | 11,094 / 127 | 850 / 64 | 3,644 / 55 |
| the ring's high water at load 30, KiB of 16 MiB | 2,569 | 1,975 | 999 | 1,061 |
| **at load 3**: the file, MiB; ticks | 11.56; 7,441 | 8.07; 4,230 | 1.29; 3,811 | 6.82; 5,220 |
| at load 3: stream blocks sectioned / declined / mismatched | 264 / 0 / 0 | 148 / 0 / 0 | 128 / 0 / 0 | 175 / 0 / 0 |
| at load 3: the codec on the writer thread, ms in all / at most for one block (L2) | 8,034 / 78 (8,169 / 75) | 7,261 / 89 (8,168 / 97) | 634 / 34 (642 / 23) | 4,720 / 54 (2,937 / 36) |
| at load 3: the ring's high water, KiB of 16 MiB (L2) | 2,104 (2,022) | 1,480 (1,718) | 963 (981) | 1,079 (1,043) |

The seek is the replay's own log line's total (`sought … in X ms`), the read nearly all of it.
The battle's seek to 900 took 1,216–1,288 ms over five seeks, fresh and running; a seek from a
running playback reads within about ±30 ms of the fresh process's. In the first air war the level
ended at GameTime 5,041, shorter than L2's 5,940.

The sections, each packed alone with zstd 3 (`recsize.py`), KiB:

| section | the battle | Town & Country | naval | air war |
|---|---:|---:|---:|---:|
| unit frame | 4,809 | 4,692 | 236 | 1,498 |
| projectiles | 2,308 | 1,060 | 361 | 378 |
| events | 1,407 | 461 | 136 | 131 |
| cells | 991 | 1,617 | 193 | 2,739 |
| orders | 825 | 401 | 63 | 164 |
| state | 337 | 135 | 140 | 126 |
| kinds | 212 | 86 | 37 | 62 |
| other | 97 | 62 | 28 | 59 |
| snapshots, on disk | 1,117 | 385 | 91 | 138 |

**Verified.** Each package (the state, events, cells and orders frames and the wreck base), on
merging: five format-9 recordings (the four above and the replay gate's Town & Country battle)
rewritten as format 10 with every stream block sectioned, 0 declined and 0 mismatched;
`codectest.py verify` with 0 differences between the C and Python decoders and the records;
100,000 fuzzed cases per section under ASan and UBSan with no report; `fuzzdiff`'s C and Python
agreeing. A review at `high` on the landing's diff found nothing of MEDIUM or above.

**Against the plan.**

- **Size.** L3's spec estimated the battle at about 12.1 MiB from format 9's 16.40, from each
  section packed alone; the transcode gives 11.88. The air war gains most (−49.5 %): its cells, the
  re-filed second slot of every airborne unit, were 30.7 % of its format-7 file, and the frame
  codes each cell's changed fields against the copy.
- **The writer.** At a load average of about 3 the most for one block was 89 ms (Town & Country),
  as L2's 97; at about 30, 134 ms. The ring never held more than 2.5 MiB of its 16. The air war's
  codec cost 4,720 ms in all at load 3 against L2's game's 2,937 (5,220 ticks against 5,940); why is
  not measured.
- **The seek.** Like for like, the battle's decode in all is 2 % faster and its longest run from a
  reset point 20 ms shorter; the air war's are 3 % and 23 ms slower. The battle's first interval
  reads in 1.22–1.29 s over repeated seeks (L2: 1.22–1.26), and Town & Country's seeks to 900 and
  1,800 in 1,105 and 1,124 ms (L2's game: 1,219 and 1,013).

**What L3 leaves open.**

- **The seek over one second**: the battle's first interval, and Town & Country's first two
  (above). L4 doubled the interval instead, with the owner's relaxed target (*L4*, below).
- **The next landing's candidates**: the cells derived from the units' footprints (about −0.6 /
  −1.0 / −1.4 MiB [INFERRED, research]) stay open; R1 for the cells frame landed in L4.
- **Memory per codec**: the record frames add about 12–13 MB a codec on the battle's 642 × 648
  grid, most of it the cells' copy (13 bytes a cell since L4 dropped the known bits, 5.4 MB) and
  the orders arena (up to 5.8 MB), and about 62.5–64.5 MB at `RECC_GRID_MAX` (4,194,304 cells, 54.5 MB
  of it the cells' copy), by the files' own counts; the writer holds two codecs, a reader one. A grid past the bound turns the
  codec off (records blocks), never the recording.
- **The declines no fixture reaches**: the decline lever forces only the encoder's first branch; a
  decline in mid-block (`enc_iteration`) and the declines after the walk (`over`, a column's failed
  allocation, `units_write`, `pool_write`, `frames_write`) run in no fixture. A real game still can
  decline a block, for one by a frame's section past 16 MiB.
- **Multiplayer-only kinds**: `RK_EV_NET_CREATE`, the `FR_CORPSE` root and the `FS_SOURCE` site meet only
  the fuzz and decode-before-write; no multiplayer recording was transcoded.
- L2's open items stand: the survivors' dword 23, births, the snapshots' projectiles, the
  stock-limits shot rows, and a crafted file's slow seek.

## L4: the measured hours, the snapshot interval and R1

**What L4 is** (landed). Recording format 11: format 10 with two changes, from the owner's
decisions of 2026-10-10 (*Decisions* 12 and 13, below), taken with the measurements below in hand.

- **A snapshot every 1,800 ticks** (`SNAPSHOT_TICKS`, `tagpu_recwriter.c`), 60 s of game time at
  normal speed, where format 10 wrote one every 900. A seek reads at most one interval of stream
  from the last snapshot, so the worst seek doubles; the owner accepted that for dense battles
  (*The seek, against the target*, below).
- **The cells frame restarts from the reader's grid (R1)** at every reset point (`RBF_FRAME_RESET`):
  its copy G becomes the grid a reader holds at the end of the iteration before the reset block,
  so a cell the reader already holds codes as no change instead of 13 new bytes. Its tick input T
  still restarts empty, since no world holds one; the first iteration after a reset codes its
  images instead of flagging them. The frame's known bits are gone (`CELLS_SCHEME` 2): a cell never
  imaged is zero in every world, so a zero G gives the bytes "unknown, so zeros" gave. A reader
  takes its own scheme alone, so a file of the scheme-1 cells frame is refused, never misread.
- **The orders, events and state frames still restart empty (R2).** Together they cost 148 KB an
  hour at 1,800 on the light hour below (orders 101, events 29, state 18). R1 for orders would need
  the encoder to hold every unit's lists through every block, declines included, about as much
  memory as a reader's world and with no small bound (up to `nslots` × 2 lists × `REC_ORDERS_CHUNK`,
  512, nodes); the events'
  state is history (IDs, dictionaries, script arguments, sprays) that no world holds; the state
  frame's 18 KB did not justify a world of its own and a Python mirror.

Readers take format 11 alone; format 10 is dropped under decision 6's rule (no published build
wrote it), so the Replays browser shows a format-10 file as unreadable.

**Where the restart's grid comes from, in each walk.** `reccodec_restart` takes a
`RecFrameWorld` (`inc/tagpu_reccodec.h`): the grid's size and its `gw · gh` cells of
`REC_CELL_BYTES`, row-major, owned by the caller. The writer's encoder keeps that world itself: the
cells frame's G takes every image of every record the encoder is handed, after a latch too, and a
declined iteration's images through the frame's `plain` op (`plain_apply`), and
`reccodec_stream_block` hands it to both of the writer's restarts (the encoder's aliases its own
G, the check decoder's copies it). The writer's own copy runs ahead of the block being encoded,
which is why the encoder does not use it. The DLL reader passes its world's grid
(`sections_body`), `codectest` a `World` that applies images by the reader's rule, and Python's
`reccodec.Frame` its own cells state, onto which `apply_records` also lays a records block's
images.

**THE INVARIANT, constructed: the grid at a reset is the same bytes in every walk.** Each walk
builds it from zero at the segment's start by one rule: an `RK_CELLS` is an image when no
`RCS_EVENTS` record came before it in its iteration, and every record is bounded by the grid. A
sequential reader and the check decoder follow by the same rule and by copy. A seeking reader's
grid comes from a snapshot of the writer's copy over the segment's base, and the writer makes that
copy take exactly the readers' images (`feature_record`, `tagpu_recwriter.c`): it keeps the
readers' events flag (`rec_section_of`, cleared at `RK_TICK`), refuses cells after an event of
their iteration unless the feature tick's call is open, and refuses `RK_GRID` anywhere but before
the first iteration's events; and it takes the base where the reader does, at the end of the
segment's first iteration (`iteration_done`; the reader's `tick_take`). A refusal abandons the file
as a capture fault. Until this landing the copy took every `RK_CELLS` outside a bracket, events or
not, and its base at `RK_GRID`; under R1 a cell the copy held and the readers did not would have
misdecoded a seek silently. **BOUNDS**: a world is taken only when it is of the codec's own grid
(`reccodec_restart`); any other, or none, leaves the codec invalid, so a reader refuses the block
by name and the writer declines it. Cell bytes are reference bytes only, never an index.

**The measured hours.** Two four-player games, each recorded for an hour of game time on the
reference setup under Wine (MEASURED 2026-10-10), with a build writing a snapshot every 300 ticks,
at turbo k = 1, an idle human seat in both:

| | the light hour | the heavy hour |
|---|---|---|
| map, grid | Core Prime Industrial Area, 642 × 648 | Moon Quartet, 610 × 616, unit limit 500 |
| seats | three stock AIs, difficulty 0 | four stock AIs, `SkirmishDifficulty` 2, 10,000 metal and energy each; each given 2 fusion plants, 4 moho mines, 4 metal makers, a kbot lab and a vehicle plant beside its commander at GameTime 1 |
| line of sight | permanent (LosType 4) | true (LosType 6) |
| iterations; wall time | 108,180; 108,000 ticks in 85 s | 108,090; 108,000 ticks in 120 s |
| live units at most | 257 | 419 (about 220–400 from the twelfth minute on) |
| shots / deaths / creates | 1,935 / 374 / 371 | 11,592 / 1,222 / 1,218 |
| the file as recorded (300), MiB | 35.07 | 69.79 |
| stream blocks sectioned / declined / mismatched | 3,607 / 0 / 0 | 3,604 / 0 / 0 |
| the codec on the writer thread, ms in all / at most for one block; the ring's high water | 19,987 / 40; 2,004 KB | 42,935 / 34; 2,895 KB |
| `tarec.py check`: snapshots, differing | 360, 0 | 360, 0 |

The light hour is `scenarios/replay-four-ai.json` without its `los` line, so the launch kept the
instance's own setting, which was permanent: it is a light game in every sense, units under 260
and few shots. The heavy one disarms the level's objective with `+kill 9`, an empty seat (the
objective's disarm `0x4904B0`, [the engine map](../exe-reverse-engineering.md)), so the idle human's
defeat at GameTime 10,232 does not end the level; seat 4 was defeated at 35,734.

**The interval, from one game each.** `codectest transcode --snap-every N` keeps every Nth
snapshot of a recording and restarts the stream only after those, so one recording made at 300
gives the file a writer at N × 300 would have written. Format 10 (R2), MiB an hour (the file scaled
to 108,000 ticks), and the C decoder's longest run from a restart under Wine, the decode a seek
faces at worst:

| interval, ticks | 300 | 600 | 900 | 1,200 | **1,800** | 2,700 | 3,600 | 5,400 | 10,800 | none |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| light hour, MiB/h | 35.0 | 30.6 | 29.0 | 28.2 | **27.4** | 26.8 | 26.5 | 26.1 | 25.7 | 24.4 |
| light hour, longest decode, ms | 47 | 95 | 136 | 178 | **263** | 378 | 495 | 728 | 1,412 | 8,846 |
| heavy hour, MiB/h | 69.7 | 61.4 | 58.6 | 57.1 | **55.7** | 54.6 | 54.1 | 53.6 | 52.9 | 51.5 |
| heavy hour, longest decode, ms | 85 | 168 | 247 | 327 | **474** | 691 | 952 | 1,387 | 2,573 | 20,304 |

"None" decodes the whole file in one run, which is what a background pass over a file with no
snapshots would cost. Fresh-process seeks in the game to the worst point of an interval (the
replay's `sought … in X ms` line, two each): light 145–156 ms at 900, 261–265 at 1,800, 534–538 at
3,600, 1,529–1,534 at 10,800, the rebuild 7 ms of it; heavy 279–289, 538–550, 1,055–1,057 and
2,869–2,970 ms, the rebuild 14–18. The seek's read is within about 15 % of the decode above.

**What a restart costs, by section.** Each section packed alone (`recsize.py`), KB:

| | light, 900 against none | light, 1,800 against none | heavy, 900 against none |
|---|---:|---:|---:|
| cells | +1,628 | +1,404 | +1,408 |
| orders | +204 | +101 | +427 |
| events | +57 | +29 | +162 |
| state | +32 | +18 | +47 |
| snapshots, on disk | 2,548 | 1,268 | 4,935 |

The cells' cost is not halved by the doubled interval (1,628 against 1,404 KB on the light hour): a
reset forgets the whole grid, and each cell touched in the next interval then costs its 13 bytes
once [INFERRED: more cells are touched in a longer interval]. Packed shares at 900: the unit frame
48.2 % (light) and 51.1 % (heavy), cells 20.1 and 13.7 %, orders 15.8 and 18.0 %, events 6.5 %
(light), state 3.7 % (light). Of the heavy hour's raw records, order lists are 64.7 % and unit
deltas 22.0 %. On disk a minute grows with the game: on the light hour at 900 from 80 KB (minute 5)
to 827 KB (minute 59); on the heavy one 0.2–0.3 MB in the first three minutes, about 0.7–1.36 MB
from the thirteenth on.

**R1, measured** (the same files converted to format 11 and transcoded natively with the landing's
codec, every block sectioned, 0 declined, 0 mismatched). The stream at 1,800, bytes written:

| | R2 (format 10's frames) | R1 | change | the file on disk, MiB |
|---|---:|---:|---:|---:|
| the light hour | 27,309,648 | 25,679,306 | −1,630,342 | 27.41 → 25.85 |
| the battle (L3's) | 11,326,194 | 11,219,909 | −106,285 | 11.36 → 11.26 |
| Town & Country | 8,671,586 | 8,591,702 | −79,884 | 8.48 → 8.40 |
| naval | 1,202,158 | 1,197,058 | −5,100 | 1.21 → 1.20 |
| the air war | 5,126,759 | 4,786,752 | −340,007 | 4.96 → 4.64 |

On the light hour, restarts now cost 167,787 bytes in all against no snapshot at all (25,511,519
bytes written): the orders, events and state frames' R2 and T's first iteration after each reset.
With no snapshot the stream is within 91 bytes of format 10's. The heavy hour was not transcoded
with R1: about −1.3 MiB an hour at 1,800 from its cells' restart cost, so about 54.4 MiB/h
[INFERRED]. Decoding is 0–3 % slower (the landing's measurement): a reset now copies the grid whole,
5.4 MB on a 642 × 648 grid, in a sequential reader and the check decoder at every reset and in a
reader at every seek.

**L3's battle at 900, 1,800 and none** (the format-10 file, transcoded): 12.00 / 11.36 / 10.69 MiB,
the longest decode from a restart 1.12 / 1.73 / 3.72 s. Per tick it decodes 6 to 8 times slower
than the light hour (whole file and worst interval), so the densest battles, not the hours, set the
worst seek.

**The seek, against the target.** The product page's target is under one second over any distance.
At 1,800 the light hour's worst seek reads in 261–265 ms and the heavy hour's in 538–550 ms; the
battle's worst interval decodes in about 1.7 s, and a dense battle's opening, where every unit
marches, reads in about 2.4–2.6 s [INFERRED: linear from 1.22–1.29 s at 900, *L3*]. **The owner
chose 1,800 with the 1.7 s in hand (2026-10-10), relaxing the one-second target for dense battles:
well under a second in ordinary games, about 2.5 s at worst in the densest (2.4–2.6 s inferred).** The product page
says so in place of the target.

**Against the budget.** The light hour misses 25 MB an hour even with no snapshots at all (24.4 MiB,
25.6 MB) and comes to 25.85 MiB (27.1 MB) transcoded to format 11, snapshots at 1,800 and R1; the heavy hour misses it by more
than twice at every interval. The unit frame and the orders are most of what is left (above). The
question is now lossy (L5) or more lossless work on the unit frame and the orders, the owner's
decision (decision 8).

**Verified.** The core commit, format 11 with every frame still R2, wrote the light hour and the
four L3 recordings byte-identical to format 10 in every stream block (27,309,648 at 1,800,
25,511,428 with no snapshot). With R1: `codectest.py --native outcomes`, 259 outcomes, under Wine
and natively (`-fsanitize=address,undefined`), among them a block that restarts as after a
snapshot and re-images cells the reader holds (sym 0, no field bytes, flag 0), a check decoder
restarted from a grid one byte off (MISMATCH, then sectioned again after the restart), a reader
refusing a world of another grid or none, a block declining in mid-block and one restarting after
it, and the encoder's grid compared with the reader's after every block; `fuzz` on the light hour
at 1,800, 100,000 cases under the sanitizers; `codectest.py verify`, C and Python, 0 blocks
differing on the light hour at 1,800 and the battle at 1,800; `tarec.py check` on the five
format-11 conversions with the constructed grid rule, 0 snapshots differing (360, 8, 4, 4, 5);
`test_tareplay.py`, 103 tests, among them `tarec`'s refusal of cells after an event;
`recmp_file_test`, whose perspective file now runs to GameTime 2,200 so its seeks straddle the
snapshot at 1,800 (701 samples, 8 clicks, 11 selections, 1 snapshot), alone and three at once.
`codectest --snap-every` takes digits alone.

**The gate** (`tareplay.py landing`, MEASURED 2026-10-10 on the coverage builds at the landing's head, the reference setup, 42 fixtures, 12 at once): 1,901 checks pass, none fails, the 21 changed functions all reached; `codec-decline` and `stock-codec-decline` each decline 17 of 87 stream blocks, every other fixture none, and none mismatches. W3: the U lane's return to now at 1,501 read 1,500 iterations from the first in 4,040 ms of the game thread's CPU on `battle` and 3,900 on `stock-limits`, under the bound of 5,000; the forks from the snapshot at 3,601 in 1,450 and 1,250; `battle`'s takeover from the snapshot at 1,801 in 650. Two earlier runs at the bound of 4,000 failed W3 by 110–120 ms, which is what the owner's raise answers.

**What L4 leaves open.**

- **The budget** (above): both hours miss 25 MB an hour. Lossy (L5) or more lossless work is the
  owner's decision.
- **The next candidate for the seek and the snapshots' bytes: dense seek points in memory, sparse
  snapshots in the file** (the owner's next exploration). The file keeps a reset block every 900 to
  1,800 ticks but writes a snapshot far less often, and the playback builds a world image at each
  reset block in memory. The points must sit on reset blocks, since a block elsewhere decodes only
  from the whole codec state; every reader already restarts at any flagged block, so no reader
  change is needed for the resets. R1 is what makes dense resets cheap: about 2.5 KB a reset from
  orders, events and state, where the cells' restart under R2 cost 14 KB a reset at 900 and 24 KB at
  1,800 (the table above, divided by the resets). It would save the snapshots' bytes, about 1.1 MB an hour on the light hour
  [INFERRED].
- **A seek's decode rests on the grid's equality at the snapshot.** It is constructed now (above),
  and `tarec.py check` compares every snapshot's world with the stream's, but no reader checks a
  decoded block against a checksum: a fault in one of those rules would misdecode after a seek,
  silently, as it would for the units.
- **Fewer fixtures reach a snapshot.** At 1,800 the 14 fixtures that end at or before GameTime
  1,800 record none, and those ending at 2,400 hold one at 1,801 that a seek uses only for a
  target past 1,924 (the reader starts from the last snapshot more than 123 ticks before it). Only
  the longer fixtures exercise a seek from a snapshot, and with it R1's restart from a
  snapshot-built grid.
- **The longest seek doubles in the gate too**: the battle's takeover seeks read up to about 1,923
  iterations instead of 1,023, and the U lane's return to now at 1,501 reads 1,500 from the
  first: 3.5–4.12 s of the game thread's CPU under the gate's load (MEASURED, two landing runs),
  past W3's 4,000. The owner raised the battle's `cpu_ms` to 5,000 (2026-10-10), the loosening
  this interval costs ([completeness](demo-recorder-completeness.md)).
- **The HUD markers' walker can hold the game thread for one rebuild**: its stop waits for one
  fork crossing, which rebuilds from the last snapshot before the fork, now up to about 1,800
  iterations ([the replay HUD](../hud-in-game.md)).
- **The cells encoder's latch runs in no fixture**: it settles the images it held back when a
  record fails its bounds, which no recording can carry (the writer refuses such a record first,
  by the same bounds); the codec's self-test runs it (`outcomes_cells`' fault blocks), and the
  owner chose to keep it inside `cells_enc_rec` rather than land a function no fixture reaches
  (2026-10-10), as L3's mid-block declines are.
- **The reset's grid copy**, 5.4 MB on a 642 × 648 grid and up to 54.5 MB at `RECC_GRID_MAX`, once
  a reset in a sequential reader and in the check decoder, and once a seek.
- L3's other open items stand: the cells derived from the units' footprints, the declines no
  fixture reaches, the multiplayer-only kinds, and L2's.

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
| L1 | the sectioned container, decode-before-write with fallback, the unit frame with the aircraft and cell rules in integers: LANDED (*L1* above) | planned about −42 MiB on this battle; MEASURED 99.6 → 29.1 MiB on a new run of it, not like for like |
| L2 | projectiles by identity (format 9's pool frame), the guided-velocity rule, the search capped at 4 candidates, shooter first: LANDED (*L2* above) | planned about −34 MiB against the pre-L1 file; MEASURED −12.7 MiB on L1's own battle file (29.09 → 16.40), because L1's zstd had already packed its projectiles from 36.5 to 15.0 MiB |
| L3 | cells, orders, events and state as four record frames that restart empty at every reset point; the snapshot wreck base: LANDED (*L3* above) | planned about −7 MiB against the pre-L1 file; MEASURED −4.52 MiB on L2's own battle file (16.40 → 11.88), −3.47 on Town & Country's, −0.60 on naval's, −5.54 on the air war's |
| L4 | two measured four-player hours, a light and a heavy one; format 11: snapshots every 1,800 ticks, the cells frame restarting from the reader's grid (R1): LANDED (*L4* above) | the snapshot interval, decided by the owner with the seek target relaxed for dense battles; MEASURED −1.63 MB an hour from R1 on the light hour at 1,800; both hours miss the 25 MB budget even with no snapshots |
| L5 | lossy, only if the owner chooses it now that L4 missed the four-player budget | about −1 MiB per category |
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

Taken by the owner on 2026-10-10, with L4's measurements in hand:

12. **Snapshots every 1,800 ticks**, with the big battle's 1.7 s worst decode known: the one-second
    seek target is relaxed for dense battles, about 2.5 s at worst there (2.4–2.6 s inferred) and well under a second
    in ordinary games (the product page says so).
13. **R1**, a frame restarting from the reader's world. L4 applies it to the cells frame; the
    orders, events and state frames keep restarting empty, at 148 KB an hour on the light hour
    (*L4*).
