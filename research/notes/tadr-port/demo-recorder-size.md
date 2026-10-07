# Demo recorder: making recordings smaller

## Status

Research, 2026-10-07. Nothing here is built yet. The page gathers what has been measured about the
size of a demo recording and the ideas for reducing it, ranked by what the measurements say each
is worth. It feeds the [product page](demo-recorder.md)'s open decision 2 (measured size) and the
landing that turns recording on for every game, which waits for a smaller stream (the product
page's status: "Recording every game waits for the reduced stream").

The precision is already decided by the product contract (*Disk budget*): moving units are
recorded ten times a second in whole world units with 8-bit headings and interpolated during
playback, and snapshots, which seek and takeover start from, keep full precision. The budgets are
**10 MB an hour for a 1v1, 25 MB for a typical four-player game and 100 MB for ten players with
1,500 units**. Today's recorder writes every value at every tick, exactly, which is what the replay
gate's EQUAL checks rest on; how those checks change for a sampled stream is part of the design
(below, *What the reduced stream changes for verification*).

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

## The ideas

Ranked by the share of the file they address. The gains overlap: a better compressor shrinks what
every other idea leaves, and the repacking changes what the compressor sees. Each is measured on
this same file before it is built (*Next*).

### 1. Units: one frame an iteration, values as differences

The recording already keeps a snapshot every 900 ticks and, between them, only what changed since
the previous tick. What makes the unit stream big is how each change is packed: one record per
changed unit, each with its own header, slot and 82-bit mask.

The change keeps snapshots and per-tick changes and repacks the tick's units into one frame:

1. **One header for all the units of an iteration**, not one a unit.
2. **The changed units as gaps between slot numbers.** Slots are sorted, so a gap is usually one
   byte.
3. **A short field mask.** Most moving units change the same fields every tick (turn, position,
   altitude, the mover's), so a "same fields as last tick" flag or a mask over field groups takes
   one or two bytes in place of eleven.
4. **The values by column, as differences.** Every unit's turn, then every unit's position, each
   stored as its change from that unit's previous value. Small, similar numbers side by side
   compress far better than interleaved records.
5. **New and destroyed units** keep their records (a whole image, and the gone record).
6. **The snapshots stay the reset points** that seek and takeover start from.

Differences from the snapshot, rather than from the previous tick, would grow as a unit moves away
from its snapshot position over the 30 seconds; the previous tick's stay small. So the base is the
previous tick.

Expected: the framing from about 58 MB raw to about 10 MB (3 million changes at about 3 bytes),
and the values compressing far better than today's 2.3:1 [INFERRED]. The scratch repacking's 2.5
times on a battle is the measured precedent for the whole of this idea.

This is lossless: every value at every tick is still recorded, so the replay gate's checks stand
unchanged.

### 2. Units: motion sampled at 10 Hz

The product contract's reduced stream: motion at ten samples a second, whole world units, 8-bit
headings, interpolated during playback. It applies to the fields that move continuously (position,
altitude, turn, and the mover's motion fields), not to discrete state (HP, build fraction, the state
bits, orders), which stays exact. On this battle's tick rate (30 a second) it removes about two
thirds of the motion samples before any packing. The tolerance sampler is the stronger form: a
sample only when interpolation would miss.

Open: which mover fields are motion and which are state the simulation reads (the mover's `+0x140`
changes by an 18-bit step every tick: a counter or a velocity, not established); and the speed,
the sampler's most expensive term, which the player needs only as the walk class `0x43DA70`
derives from it (2 bits, rarely changing) [INFERRED, state evidence §4].

### 3. Projectiles recorded from their launch

About 21 of a projectile's 27 dwords change every tick, so the projectile stream is nearly the
whole record of every shot in flight, every tick: 36.5 MB packed, 39 % of the file. A shot's flight
follows from its launch. Recording the launch (the shot event already exists, 26,782 of them here)
and the impact, with the flight recomputed during playback or sampled sparsely, removes most of the
category. The puppet host already regenerates shots from the engine's own scripts in a 1,432-unit
battle ([state evidence §4](demo-recorder-state-evidence.md#4-size-estimate)); the projectile pool
is recorded today because seek and takeover rebuild the shots in flight exactly, which a snapshot
can keep doing at full precision.

Open: whether every weapon's flight is reproducible from its launch (guided missiles, which follow
a moving target, and interceptions); the fallback is a sparse sample with the launch exact.

### 4. A stronger compressor

The writer deflates with lodepng's default settings, a 2 KB window. On the same blocks, without
changing what is recorded, zstd level 19 gives 0.61 of the size, lzma 0.53, and zstd over the whole
stream as one frame 0.48. It is the cheapest change and stacks under every other.

Open: the writer's cost per block at the chosen level (the writer thread, not the game thread,
pays it), a library small enough for the 32-bit DLL (zstd's single-file decoder and a fast
compressor level, or miniz at level 9 with a 32 KB window as the smallest step), the reader in
`tarec.py` and the DLL, and whether a block per iteration range keeps seeking cheap. A whole-stream
frame is not compatible with seeking from a snapshot; per-block compression is, which is why
the per-block numbers are the ones to compare.

### 5. Counters recomputed, not stored

Some unit dwords change on almost every tick by a fixed small step: the mover's `+0x140` (2.2
million changes, 18 bits each) and `+0x18` in weapon slot 1 (1.2 million, one bit). If they are
counters the engine advances from state the recording already carries, playback can recompute
them and the recording stores only a correction when the recomputed value differs. 12.7 MB raw
between the two here.

Open: what each field is, in the disassembly; this is an engine fact before it is a format choice.

### 6. Order lists by the changed order

238,706 of 240,928 order lists re-sent changed one order image of one; the list is sent whole.
Sending the changed order (or the changed field of it) shrinks a category that is 23.2 MB raw but
only 2.8 MB packed. Small on disk; worth doing with idea 1's frame, not on its own.

### Not targets

- **Snapshots** are 1.6 % of the file at one every 900 ticks. The interval is chosen for seek time
  (open decision 3), not for size.
- **The explored map** is 0.2 MB raw over the whole recording and compresses to almost nothing.
- **Feature-grid cells** compress 6:1 already (3.5 MB packed); idea 4 shrinks them further.

## What the reduced stream changes for verification

Ideas 1, 4 and 6 are lossless: every value at every tick is still recorded and the replay gate's
EQUAL checks stand. Ideas 2, 3 and 5 make the stream lossy between snapshots: a puppet's unit is
where the interpolation puts it, not where the recording machine had it. Snapshots stay exact, so
seek and takeover still start from the exact world; the checks between snapshots need tolerances,
and choosing them is the owner's decision (CLAUDE.md, *Simulation changes stay replayable*:
loosening). The completeness design already reports the cost and size budgets rather than failing
them until the quantized stream lands
([completeness design](demo-recorder-completeness.md), OQ5).

## Next

1. **Measure the lossless ideas offline.** A re-encoder in `recsize.py` that rewrites this
   recording's stream as idea 1's frame (and idea 6's orders), decodes it back to check it is
   lossless, and reports its size under the writer's deflate, zlib 9, lzma and zstd. That says
   what the repacking and the compressor are worth together, before any DLL change.
2. **Identify the counters** (`+0x140` in the mover, `+0x18` in weapon slot 1) and the mover's
   motion fields in the disassembly; measure the position and altitude field by field.
3. **Measure the lossy ideas offline the same way**: 10 Hz motion with interpolation, and
   projectiles from their launch, against the tolerance sampler's numbers.
4. **A real hour** of a four-player game with AI and structures, the product page's open decision
   2, measured with the chosen encoding before the format is frozen.

Then the design: the format change, the writer and both readers, the gate's checks.
