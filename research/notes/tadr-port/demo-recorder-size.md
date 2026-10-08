# Demo recorder: making recordings smaller

## Status

Research, 2026-10-07. Nothing here is built yet. The page gathers what has been measured about the
size of a demo recording, the research round that prototyped each way of reducing it on one
battle recording, and the plan that came out of it: **lossless, the battle's 99.6 MiB file becomes
about 12 MiB**, and lossy options add about 1 MiB per category on top. It feeds the [product page](demo-recorder.md)'s open decision 2 (measured size) and the
landing that turns recording on for every game, which waits for a smaller stream (the product
page's status: "Recording every game waits for the reduced stream").

The precision is already decided by the product contract (*Disk budget*): moving units are
recorded ten times a second in whole world units with 8-bit headings and interpolated during
playback, and snapshots, which seek and takeover start from, keep full precision. The budgets are
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
MinGW build at the DLL's flags under Wine on the reference setup. It adds about 0.6 MB to the DLL.

Once the codecs above are in, the compressor no longer shrinks the file: on the unit frame zstd 3
is 4.63 MiB against the writer's 4.50, zstd 19 about 5 % better. Snapshots are the exception
(1,629 KB writer, 1,164 zstd 19). So zstd buys CPU and seek speed, not size. Removing 54 MiB of raw
framing alone saves only 6.1 MiB under zstd 3: implicit lengths (where the format fixes a record's
size), runs and slot gaps pay; varint headers, dropping padding, trained dictionaries, sparse mask
encodings, byte shuffles and order-0/1 rANS gain nothing on this data. The container that carries
the codecs: self-contained blocks of about 30 iterations, ending at each snapshot, holding a kind
sequence and one section per category.

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
- **One flat battle**: no aircraft or hills (pitch, roll and vertical velocity never changed). The
  codecs stay lossless elsewhere, but their gains need re-measuring on other recordings.
- **Seek in C**: the prototype decoder works over every live unit; a seek could take 0.2–0.6 s
  against the one-second target [INFERRED]. Decode work should grow with the moving units.
- **The writer and the ring**: the unit encoder's cost (about 1 ms an iteration at 2,000 units) is
  unmeasured, and the 16 MB ring holds about 5 s of peak input, so a slow writer ends a recording.
- **A codec bug's reach**: every codec decodes its block before writing and falls back to today's
  records on a mismatch, so a bug costs size, never data.
- **Predictor resets**: every codec resets at each snapshot and each segment's first iteration, a
  rule of the format, so a seek can start at any snapshot.

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
| L0 | every codec run offline on the naval and Town & Country recordings and one with aircraft and hills | confidence the gains generalize |
| L1 | the sectioned container, decode-before-write with fallback, the unit frame | about −42 MiB on this battle |
| L2 | projectiles by identity, with a capped search | about −34 MiB |
| L3 | cells, orders, events, globals; the snapshot wreck base | about −7 MiB |
| L4 | the measured hour of a four-player game with AI and structures | the snapshot interval; whether lossy is needed |
| L5 | lossy, only if L4 misses a budget | about −1 MiB per category |
| L6 | multiplayer: K (with or after L1), its snapshot checkpoints, then the wire codec in stage 6 | multiplayer files and bandwidth |

## Decisions for the owner

1. **Lossless first, instead of the contract's 10 Hz** (a change to the plan): lossless reaches
   most of the gain, and lossy adds about 1 MiB per category.
2. **The prediction tier**: generic (9.84 MiB), integrators (7.30), or with the sine-table rule
   (4.50), which pins the format to one engine fact.
3. **Field lanes or dword lanes**: a table of the record's field boundaries (a wrong row costs size,
   never correctness) or plain dwords.
4. **The compressor**: keep the writer's deflate, or add zstd (+0.6 MB of DLL) for writer CPU and
   seek speed rather than size.
5. **The Python reader's dependencies**: numpy, and zstandard if zstd is chosen.
6. **Old formats**: drop formats 5–7 (no recording from them is in a release).
7. **Projectile engine facts in the codec**: shot events and the `+0x67` slot stamp as predictions
   (0.44 + 0.22 MiB).
8. **Loosenings, if L4 needs them**: the motion tolerance, approximate radar contacts at range
   edges, what a takeover between snapshots gets, stale projectile records out of snapshots.
9. **Projectiles from launch**: whether the puppet's engine may fly its own shots, after a
   determinism probe.
10. **Multiplayer**: DECIDED 2026-10-07, K. Open: accepting the loss of the replica-against-owner
    audits, which K drops with the observed remote copies.
11. **The measured hour's wall time** against the ten-minute rule for a game run.
