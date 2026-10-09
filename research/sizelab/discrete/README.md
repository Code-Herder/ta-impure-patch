# Discrete state: orders, cells, wrecks, events, globals, players, snapshots

Everything but unit motion and projectile flight. Scripts and outputs: `research/discrete/`
(each `*.py` beside its `*.txt`). MB = MiB, as recsize.py. Every number is MEASURED on
`size-battle.imprec` unless marked [INFERRED] or [ESTIMATED]. "Packed" is recsize's attribution
(a category's records of one block deflated alone, writer settings), zstd-19 beside it. Every
re-encoding was decoded back and compared with every record it replaces (`[decoded: equal]`).

| Category (stream) | Today deflate | New deflate | Today zstd19 | New zstd19 | Saving (deflate) |
|---|---:|---:|---:|---:|---:|
| Feature-grid cells, after the first grid | 3,353 KB | 678 KB | 2,202 KB | 669 KB | 2.61 MB |
| the first grid (one-time, 5.17 MB raw) | 246 KB | 246 KB | 33 KB | 33 KB | compressor only |
| Order lists | 2,862 KB | 872 KB | 1,734 KB | 827 KB | 1.94 MB |
| Events | 2,986 KB | 1,530 KB | 2,399 KB | 1,496 KB | 1.42 MB |
| Wreck records | 405 KB | 166 KB | 366 KB | 160 KB | 0.23 MB |
| Globals | 348 KB | 112 KB | 337 KB | 116 KB | 0.23 MB |
| Players | 146 KB | 102 KB | 147 KB | 108 KB | 0.04 MB |
| Feature heads (RK_FEATURES) | 47 KB | 21 KB | 49 KB | 28 KB | 0.03 MB |
| **Sum** | **10.15 MB** | **3.64 MB** | **7.10 MB** | **3.36 MB** | **6.5 MB (6.5 %)** |

The attribution flatters today: the same kinds deflated *together* per block in stream order
are **12.69 MB** (the 2 KB window loses matches across interleaved kinds) against **3.39 MB**
re-encoded together (`combined.txt`), **−9.3 MB**. For this area re-encoding beats the
compressor (10.2 → 3.6 vs → 7.1 MB); both give 3.4 MB.

## 1. Cells: the next images repeat the tick's input (−2.61 MB, lossless)

- **360,850 of the images' 371,001 changed cells (97.3 %) are the same cell with the same 13
  bytes as the previous step's tick input** (`cells.txt`). `cells_tick` (tagpu_recorder.c) emits
  the input without updating the shadow, so the next images send each again.
- Slot A (`+0x00`) alone changed in 360,384; slot B never (no aircraft); runs are 1–2 cells under
  12 bytes of framing.
- **Change:** the images' list says "the last tick input stands" (a flag, or a bitmap over it)
  plus the cells it does not cover; each cell as a gap, a 7-bit mask of changed fields, those
  fields, "slot A cleared" as a code. 3,353 → 678 KB.
- **Further, [ESTIMATED] −0.6 MB:** 356,862 of 358,237 tick-input slot-A changes (99.6 %) are
  exactly the ground units' footprint moves (`+0x76/+0x78/+0x7E/+0x80`; stamp `0x47CC30`, clear
  `0x47D0E0`) between the iteration's images and the next's, the value the unit's slot
  (`cells_predict.txt`); exceptions price at ~20 KB. It needs the next images first, so the tick
  input moves after the next iteration's units; the playback already holds the next image at a
  step's late phase (THE PHASES, tagpu_replay.c) [INFERRED that the reader can match]. The
  predictor becomes part of the format.

## 2. Order lists: send the changed node (−1.94 MB, lossless)

- 238,612 of 240,928 sends changed exactly one node, always the head; 237,344 lists hold one node.
- **What changes every tick is `+0x0A`, a dword wake time.** DISASSEMBLED: the constructor
  `0x43A0C0` sets −1 (`0x43A145`); `0x439E80` (thiscall, `ret 4`, 75 call sites) sets
  `+0x06 |= 1`, `+0x0A = [main+0x38A47] + delay` (GameTime); inline at `0x43B94F..0x43B970`
  (+30 + `0x4B6C30(30)`) and `0x43BB31..0x43BB52` (+30 + `0x4B6C30(15)`). MEASURED: action 25,
  state 3 re-sends every tick with `+0x0A` = the iteration's GameTime; idle (41) every 30–56 ticks.
  Not recomputable: handlers do not run on puppets, and a delay can be an RNG draw.
- Also: `+0x4E` event bits (105 K changes), action/state `+0x04/+0x05` (95 K), target position
  `+0x22..+0x2D` (~34 K), `+0x42` flags, `+0x52` owned class.
- **Change:** slot gap, list, per node a mask of changed dwords and their values, by column; a new
  or resized list whole. 2,862 → 872 KB. Byte masks (896), delta-of-delta (1,211) and a GameTime
  rule for `+0x0A` (885) measured no better: the general form suffices.

## 3. Events: columns and what the reader holds (−1.42 MB, lossless)

| Kind | Records | Today | New | What was redundant (MEASURED) |
|---|---:|---:|---:|---|
| shot | 26,782 | 831 KB | 583 KB | start within 63 wu of the shooter's position; target within 15 wu of the target's (99.9 %); weapon as the slot's last |
| broadcast start | 73,150 | 679 KB | 298 KB | AutoAim's aim scripts (11/12/13/9), argc 2 in 99.8 %, args past argc always 0 |
| owner's start | 93,482 | 592 KB | 195 KB | three names (HitByWeapon, TakeDamage, TargetCleared) sent as strings |
| death | 2,087 | 243 KB | 58 KB | its 0x148-byte image differs from the unit's image at the iteration's images in 0–17 dwords, ~5 typical |
| reply | 37,476 | 101 KB | 12 KB | two kinds 99 %, four texts |
| nano spray | 18,067 | 88 KB | 12 KB | 99.6 % the same unit and box as the step before, 94.3 % the same point |
| feature call + end | 32,982 | 123 KB | 81 KB | FR_TICK every step, sites constant |
| kind, unit ID, flags | — | — | 359 KB | the shared order and identity columns |

- **Change:** a kind byte an event keeps their order; fields by column; unit IDs as differences;
  "same as this unit's last" flags; names and texts as dictionary indices; a death's image as a
  dword delta; shot points as offsets from the two positions.
- Every kind is played (`ev_*`, tagpu_replay.c); replies and sprays are presentation, but "a
  replay shows what the game showed". A lower rate for them would save under 24 KB now.

## 4. Globals and the small kinds (−0.5 MB, lossless)

- `rngseed`: `0x4B6C30` is Park-Miller, ×16807 mod 2³¹−1 (DISASSEMBLED `0x4B6C47..0x4B6C87`;
  n < 2 returns without stepping, `0x4B6C35`). Steps an iteration: median 16, p99 986, max
  12,622 — **recorded as the step count**, recomputed exactly. Kept: a takeover between snapshots
  needs the exact seed.
- `pfsearches` rise ~6,665 a tick each, `pfcalls` and `mcwatermark`s by 1: delta-of-delta makes
  them ~0. Not recomputable (no pathfinding on puppets). 348 → 112 KB, mostly the 10-byte mask.
- The feature cursor `main+0x14257` steps −1, wrapping to W×H−1 (`0x4240AA`, `0x4240CC`): a
  flag. Wrecks (GAF frame/timer dominate) and players as dword differences.

## 5. Snapshots

| Snapshots (8) | deflate | zstd19 |
|---|---:|---:|
| today | 1,629 KB | 1,164 KB |
| wrecks only where they differ from the first iteration (lossless, decoded) | 1,247 KB | 943 KB |
| + projectile records `[count, hw)` left out (**a loosening**) | — | 569 KB |
| base form, compressed with the previous snapshot as dictionary (a chain) | — | 351 KB |

- **Every snapshot carries all 8,192 wreck records**: the level load links every free record
  (`0x421F5F..0x421F94`, the engine map), so none is zero: ~64 KB deflated each against 459–763
  records that differ from the first iteration — the base the cells already use. **In the small
  ten-seat sample (`small/`, 914 KB, 68 s) wrecks are 51 of each snapshot's 58 KB: 117 → 13 KB,
  11 % of that file, and ~7 → 0.8 MB an hour** at 120 snapshots — most of a 1v1's 10 MB/hour.
- **Projectiles:** hw stays 1,081 while the live count falls 174 → 16; the pool compacts by
  copying (`0x49AEE0`), so 900–1,065 records a snapshot are leftovers. Dropping them is a
  loosening and needs the fact that every append writes all 0x6B bytes (`0x49C740`, the ten
  append sites).
- **Chaining** (keyframe every K) costs a seek K−1 decompressions of ~1.2 MB raw [INFERRED
  milliseconds each].
- **Interval:** worst seek 605 ms at 900 ticks, 446–587 of it reading (skirmish slice note,
  MEASURED 2026-10-01); 1,800 would be ~1.2 s [INFERRED, linear] unless the smaller stream reads
  faster — to measure once it exists. **If the stream shrinks 3–5×**, today's snapshots become
  4.7–7.5 % of the file; with the wreck base under zstd-19 2.8–4.5 %; chained ~1–1.7 %. Per hour
  the battle's snapshots are ~24 MB today (a quarter of the 100 MB budget), ~14 MB with the base.

## Every change: constraints

- **Fidelity:** lossless except the projectile leftovers; the gate's EQUAL checks stand.
- **Seek:** predictions reference what a reader holds after a snapshot (images, lists, grid,
  pool) or earlier records since its entry block. Event contexts restart at the first stream
  block and the block each snapshot follows (which the reader decodes for that step's events):
  1,530 KB so, 1,764 KB restarting every block. Globals and cells were measured without restarts
  [INFERRED: one iteration's loss a snapshot].
- **Takeover:** unchanged. **Game thread:** unchanged — the writer re-encodes from the ring with
  the world copy it keeps for snapshots; per tick a few hundred compares, a ~90-cell merge, table
  lookups, the RNG step search (capped, literal fallback) [INFERRED well under 0.1 ms].
- **Readers:** both (DLL, tarec.py) gain decoders: a format bump. **Multiplayer:** contributions
  carry the same orders and events; contexts per origin, restarted where `History` rebuilds a
  prefix [INFERRED].
- **Risk:** two decoders must agree (tarec `check` and round trips catch it); the cell reorder is
  the only change to what an iteration contains.

**Not recommended:** relevance and priority accumulators (Tribes, Halo, Unreal) choose what a
client sees; a recording's viewer may look anywhere and everything here is needed exactly.

## Ranked (MB on this 99.6 MB file, writer's deflate)

1. **Cells: the next images reference the tick input; fields and gaps** — −2.61 MB, lossless.
2. **Orders: the changed node, dword mask and values** — −1.94 MB, lossless.
3. **Events: columns, "same as last", dictionaries, death and shot deltas** — −1.42 MB, lossless.
4. **Snapshot wrecks against the first iteration** — −0.37 MB here, **11 % of a small game, ~6 MB
   an hour**; lossless; the cheapest change here.
5. **Cells from unit footprints** — ~−0.6 MB [ESTIMATED]; lossless; reorders the tick input.
6. **Globals (RNG steps, delta of delta), wrecks, players, feature heads** — −0.53 MB, lossless.
7. **Snapshot chaining** — −0.58 MB more under zstd-19; a seek dependency.
8. **Projectile leftovers out of snapshots** — −0.37 MB (zstd19); **a loosening: the owner's
   decision**, behind an engine fact still to establish.

Lossless 1–6: ~7 MB (attribution) to ~9.5 MB (interleaved); this area falls from ~10–13 MB to
~3.5 MB, near a tenth of the file once units and projectiles shrink.
