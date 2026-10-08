# Projectiles and weapons: where 39 % of the file goes, and how to remove most of it

Sizes are in MB as `recsize.py` prints them (2^20 bytes), each the projectile category deflated alone per
stream block, so they compare with its 36.5 MB. Scripts and outputs: `research/weapons/` (`codec*.py`,
`p*.py`, `p*.out`). Exe facts are from `TotalA.exe.pristine` unless marked.

## 1. The record, and what changes

The projectile tick `0x49B720` (DISASSEMBLED) walks the records counted at its start (`0x49B740`), so
appends made during the tick are not moved until the next one. Every tick ends in the compaction
`0x49AE20` (`0x49BE53`, reached even with an empty pool). Per kind (weapon `+0x111` flags):

| Field | Written each tick by |
|---|---|
| position `+0x04..+0x0F` (16.16) | `+= velocity` (`0x49BB3A`, `0x49BB82`, `0x49BC37`, `0x49BCE3`, `0x49BD4D`); `+= wind main+0x37ECC` for flag bits 1 and 8 (`0x49BD10..0x49BD2D`) |
| velocity `+0x1C..+0x27` | `y -= gravity main+0x14263` (`0x49BAD3`, `0x49BD30`); guided (bit 20): rebuilt from pitch, heading and speed (`0x49BA74..0x49BABE`) after the aim `0x49B3E0` and turn `0x49B520`; a burst template's is sprayed by the simulation RNG (`0x49B903..0x49B94B`) |
| tail `+0x10..+0x1B` | `+= velocity` for bit 3 after `weapon+0xF0` ticks (`0x49BBB5`) |
| speed `+0x3A` | `+= weapon+0x70` to `weapon+0x68` (guided, `0x49BA16..0x49BA31`) |
| `+0x34`, `+0x38` | bit 5's tumble (`0x49BD6E..0x49BD82`) |
| `+0x42`, `+0x60` | a burst template: `+0x42 += weapon+0xEC`, `+0x60 -= 1`, its position re-read from the shooter's piece `0x43E240` (`0x49B7A6..0x49B7FA`), then a whole copy appended (`0x49B83B..0x49B848`) |
| smoke `+0x4A` | `+= weapon+0xFA` when due (bit 18, `0x49BD97..0x49BDD4`) |
| flame `+0x64` | `+= 0x400` (bit 21, `0x49B9BC`) |
| `+0x5E`, `+0x5A`, `+0x5C` | the collision `0x49B090`: `+0x5E` = (cell `+5` + cell `+6`) / 2 under the new position (`0x49B1B2..0x49B1C3`); the last feature cell hit (`0x49B357`) |
| `+0x67` | the compaction stamps every record, removed ones too, with its index before the slide (`0x49AE5F`) |

Weapon, target point, IDs, spawn/death times, piece and owner never change in flight.

**Why today's deltas rewrite 21 of 27 dwords** (MEASURED): the delta compares a *slot* with its previous
tenant, and the compaction slides every record behind a removed one. Matching records across
iterations (`pool.match`: monotone, ≥ 12 of 27 dwords equal) finds 539,736 survivor transitions,
57,078 births and 57,078 deaths; of the 497,169 survivors whose `+0x67` names their old slot, **439,044
(88 %) moved slot**. For the same projectile one tick on, position changes in 86 % of transitions,
vertical velocity 25 %, `+0x5C/+0x5E` 25 %, horizontal velocity 3.5 %, the tail 2.8 %, times 4–6 %, the
rest under 0.1 %.

Births: of 26,782 shot events, 25,566 appear at the next capture (14,944 single shots, 10,622 burst
templates, invisible with `+0x60` > 0) and 1,216 never reach one; the tick adds 31,506 burst copies.
One burst weapon (ID 17) is 55 % of all survivor transitions.

## 2. Lossless: code the pool by projectile identity (MEASURED, decoded back)

Per iteration: count and extent; the dead old records (gaps); births at the end (appends only go at the
count, the compaction never reorders — DISASSEMBLED, and only a cost if wrong: 11 transitions needed a
forced pairing); the stale tail slots that changed. Then, as columns across the block's iterations:

- **Survivors**: each dword's residual against a prediction from the same projectile's previous two
  states — held, linear, or position = previous + new velocity + last tick's drift — chosen per column
  per block by the encoder (2 bits a column). `+0x67` is predicted as the old index (exact for 517,055 of
  539,736).
- **Births and tail slots**: residual against the cheapest reference the encoder finds among rows both
  sides hold (survivors and births decoded so far, the old pool, each old record advanced one tick,
  and each shot event laid over its weapon's last birth), sent as an index less a default guess. A tail
  slot's guess is the record the compaction leaves there.

The decoder never searches. History restarts at each snapshot; **every snapshot interval decodes
exactly from the snapshot's pool alone** (`p18_seek.py`: 6,929 iterations, 8 snapshots).

| Step (cumulative) | writer | zstd 19 | lzma 6 |
|---|---:|---:|---:|
| Today | 36.50 | 16.78 | 15.29 |
| Identity, previous value | 9.01 | | |
| + linear, position-by-velocity, the stamp | 7.68 | | |
| + reference search for births and tail | 3.36 | 3.17 | |
| + predictor chosen per column per block | 2.99 | 2.86 | 2.85 |
| **+ shot events as references** | **2.55** | **2.42** | **2.41** |
| Same, compressed per 900-tick interval | | 2.20 | 2.15 |
| Same, one record per iteration (no transposition across a block) | 3.02 | 2.78 | 2.77 |

All rows decode back to every iteration's pool exactly (7,830/7,830). **Saving 34.0 MB, 34 % of the file.**
The final stream's parts (each deflated alone): survivors 1.04, births 1.18, control 0.18, tail 0.13,
references 0.10. Two other recordings, decoded exactly: the Town & Country battle fixture 28.2 → 2.44
MB, `naval.imprec` 4.85 → 0.46 MB.

**Cost.** The capture is unchanged: the writer applies today's ring records to its own pool copy
(≈ 1 MB: pool, previous pool, history) and re-encodes. Restricting the encoder's search to rows of the
same weapon and the live pool loses nothing (2.54 MB) and costs 154 k dword compares an iteration on
average, 3.4 M at p99, 12.4 M at the worst iteration (MEASURED counts); a cap on candidates bounds
it further [INFERRED, unmeasured]. **Robust by construction**: the writer can decode each block before
writing it and fall back to today's records on any mismatch, so a codec bug costs size, never data.

## 3. Further lossless facts, ranked by what they cost now (attributed, deflated alone per block)

| Fact | Now | Evidence |
|---|---:|---|
| `+0x5E` from the terrain under the position | 0.22 MB (its dword) | DISASSEMBLED `0x49B1B2`; needs the cell lookup `0x4815A0` in both coders |
| A template's update from its own copy (decode births first) | ≈ 0.3 MB [INFERRED] | the copy is the template's bytes after `0x43E240` (`0x49B83B`); 108 k position residuals are templates (`p11`) |
| Smoke timer `+= weapon+0xFA` when due | 0.05 MB (its dword) | `0x49BDB6..0x49BDD4` |
| A weapon's first-tick drift from its last projectile | ≈ 0.05 MB [INFERRED] | 20 k first-transition misses on two ballistic weapons (`p11`) |

The shot event and the birth carry the launch twice: the event costs 0.81 MB in the events category
(`p15`). MEASURED: a birth's tail equals the event's fire point in 25,566/25,566 launches, its target in
all, its target point in 75 % (the init leaves `+0x28` stale without a target). Either can be coded
from the other; I used event → birth (0.44 MB).

## 4. Recording from launch, the engine flying the shots

**What the event carries** (DISASSEMBLED `0x49D7AD..0x49D859`): fire point (3 × i32 from `0x43E240`),
target point, weapon ID, a flag, the slot's aim heading and pitch after the RNG spray (`0x49D727`,
`0x49D733`), target and shooter IDs, slot. Burst copies have no event: the tick makes them.

**On a puppet today** the stock receiver `0x49D270` rebuilds each shot and the host's projectile tick
flies it, but the frame visit and the next capture point overwrite the pool (`tagpu_replay.c`, *THE
GLOBALS AND THE PROJECTILES*). Whether that flight is bit-exact has **not been measured**. Inputs a
puppet has differently [INFERRED from the phase order]: the simulation RNG at the tick (bursts' decay
and spray, `0x49B8D1`, `0x49B903`); the shooter's piece pose (`0x43E240`); the guidance target `0x49B3E0`,
which reads the target unit's `+0x6A` (DISASSEMBLED `0x49B48E..0x49B4A1`) after the recording machine's
unit tick moved it but before the puppet's; unit altitudes in the collision (`0x49B1F9..0x49B20D`).

**Offline proxy** (`p14b`): dead reckoning from a projectile's first image alone reproduces its
position at every tick of its life for **77.0 % of projectiles** — 91.8–99.5 % of in-flight shots, 0.2 %
of templates — and all 27 dwords for 11.7 %.

**The ceiling** is the survivors and tail parts, 1.17 of the 2.55 MB: about **1.4 MB** for the
category if the engine flew every shot exactly. Measured without corrections (`p13`, generic predictor,
no events): 2.96 → 2.03 MB, position error p99 79 wu. It needs a probe first (§7).

## 5. Reduced sampling and quantization: little or no gain (MEASURED)

| Variant (same codec) | writer |
|---|---:|
| Exact at 30 Hz | 2.55 |
| Exact samples at 10 Hz, births at the next sample | 2.77 |
| Exact samples at 5 Hz | 2.29 |
| 30 Hz, positions in whole world units (codec without events) | 3.10 vs 2.96 |

10 Hz gains nothing and 5 Hz 0.26 MB, with births up to five ticks late and the shortest shots lost.
Exact flight is predictable and rounded flight is not; births, 46 % of the stream, cannot be sampled
away. Linear interpolation between 10 Hz samples would be visually exact anyway: p99 0.124 wu.

## 6. The weapon slots in the unit record

Slots at `+0x04`, stride `0x1C` (the engine map). Changes of slot 1 in the unit deltas (MEASURED, `p8`;
slots 2 and 3 at most 2,000 each):

| Field | Changes | Linear prediction misses |
|---|---:|---:|
| `+0x14` reload countdown (unit `+0x18`) | 1,116,370 | 53,461 |
| `+0x1B` flags | 165,921 | 165,921 |
| `+0x08` aim complete | 136,826 | 136,826 |
| `+0x16`/`+0x18` aim heading, pitch | 93,010 / 77,308 | all |
| `+0x00` target | 51,618 | all |

1,089,621 reload changes are AutoAim's decrement (DISASSEMBLED `0x49E1C8..0x49E1D2`; set at fire
`0x49E4EE` from reload, veterancy and HP). AutoAim does not run for a type-3 owner, so the stream must
carry the countdown, but a linear predictor makes it free outside fire and zero. The others are aim
results from COB and AutoAim and cannot be recomputed on a puppet.

## 7. Effects, risks, prerequisites

- **Seek** shown exact from every snapshot. **Takeover** is unchanged: the pool stays whole in snapshots.
  **Verification**: lossless, so the EQUAL checks stand. **Multiplayer**: projectiles come only from
  the recorder's primary stream (multiplayer slice), so contributions do not change.
- **Format**: a new transition record. `tarec.py`, `recsize.py` and `tagpu_recreader.c` decode it into
  the same `RecWorld` pool, so `proj_put` is unchanged. Transposing across a block saves 0.47 MB but
  ties projectile data to block boundaries; one record per iteration keeps the record model.
- **Risk**: fitted on three recordings; other weapon kinds (guided, interceptors) cost bytes, never
  correctness.
- **Probe for §4** (≤ 10 min): at the frame visit, before `proj_put`, compare the host's own pool with
  the recording's next image and count equal records and differing dwords per weapon, on
  `size-battle.imprec`.

## Ranked recommendations

1. **Identity-coded projectile stream** (§2), with the write-time self-check: 36.5 → 2.55 MB, lossless,
   34 % of the file. One self-contained writer and reader change. Do this.
2. **The prediction ideas from §2 apply to the unit stream too**: the reload countdown (§6) is 1.09
   M of the 1.15 M changes of unit `+0x18`.
3. Optional facts (§3): the terrain height (0.22 MB) and templates from their copies (≈ 0.3 MB).
   Each needs code shared exactly by both coders.
4. **Not recommended**: sampling or quantizing projectiles (§5: at most 0.26 MB).
5. **Not now**: from launch (§4). It is worth about 1.1 MB more, rests on an unmeasured determinism, and
   changes who writes the pool on a puppet.

## Questions for the owner

1. **Lossy projectiles** (no survivor corrections between snapshots: saves about 1 MB, positions
   approximate between snapshots) would be a loosening. Do you want it, given the lossless stream is
   2.55 MB?
2. **From launch** keeps the host's projectile tick as the writer of a puppet's pool. That changes
   the replay contract ("the recorder is the only writer"). Should it be pursued, and should the
   determinism probe run first?
3. **Engine facts in the codec.** The event→birth mapping saves 0.44 MB and the `+0x67` stamp 0.22 MB
   (MEASURED: 2.55 against 2.99 and 3.21); position-by-velocity was not measured alone. They are
   predictions, never correctness, but each is a small hand-written mapping. Accept them, or keep the
   codec generic?
