# Coding: the container, framing and entropy coding

Everything here is **lossless**: every re-encoding was decoded back to the original stream
payloads byte for byte on the battle (all 7,830 iterations, at one block length at least), so the
replay gate's EQUAL checks, seek and takeover are untouched. Sizes are **MiB** (recsize.py's "MB"), on `size-battle.imprec`'s stream:
**97.97 MiB today**, 226.3 MiB raw. Writer CPU is **MEASURED** with a 32-bit MinGW build at the
DLL's own flags (`-O2`, default target pentiumpro, no SSE2), run under Wine on the reference setup
(Ryzen 9 7950X), as "ms of writer CPU per second of game": the whole battle's 261 s, and its
busiest 30 s; a slower player CPU scales them [INFERRED]. Speeds marked "64-bit" are Python
bindings. Scripts and outputs: `research/coding/`.

## 1. Framing (MEASURED)

| Category | records | header | pad | slot | mask | reserved 0 | other head | values |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| unit deltas | 3.02 M | 11.51 | – | 5.76 | 31.66 | 8.63 | – | 50.11 |
| projectiles | 0.66 M | 2.52 | – | 1.25 | 2.44 | 1.25 | – | 52.86 |
| cells | 0.57 M | 2.18 | 1.49 | – | – | – | 4.35 | 14.23 |
| orders | 0.24 M | 0.92 | – | – | – | – | 1.84 | 20.39 |
| events | 0.29 M | 1.10 | 0.18 | – | – | – | – | 8.40 |

Raw MiB. Unit-delta slots are ascending within an iteration in 100 % of cases (3,010,200 of
3,010,200 successive pairs); padding and reserved bytes are always zero; 88 % of unit masks are
among the 255 most common of 15,920 distinct masks.

Re-encoded and compressed whole (today's blocks, cumulative):

| Framing | raw | deflate 2 KB | zstd 3 | zstd 19 |
|---|---:|---:|---:|---:|
| H0 today | 226.3 | 98.42 | 66.83 | 59.51 |
| H1 u8 kind + varint length, no padding | 215.4 | 97.44 | 66.73 | 59.47 |
| H2 + implicit lengths (fixed kinds, `rec_event_len`, mask popcount, orders/cells counts) | 210.9 | 94.60 | 64.92 | 58.08 |
| H3 + one header per run of one kind (`kind, count`) | 206.8 | 93.36 | 64.33 | 57.78 |
| D1 + slot as varint gap, no reserved bytes | 193.5 | 85.41 | **60.70** | **53.96** |
| D2 + "same mask as this slot's last" flag | 179.8 | 83.99 | 60.96 | 54.59 |
| D3 masks as set-bit index lists | 182.2 | 84.86 | 61.75 | 55.52 |
| D4 masks as XOR-with-last index lists | 172.1 | 83.58 | 60.88 | 55.06 |

**Framing compresses well**: 54 MiB of raw framing removed is worth 6.1 MiB under zstd-3 (9 %)
and 15 MiB under today's 2 KB deflate. Varint headers and padding are worth nothing packed;
implicit lengths, runs and slot gaps are worth it. **Sparse mask codes gain nothing under zstd**
(−0.3 to +1.0 MiB); they only help a compressor that cannot see the slot's previous mask.

## 2. Compressors as a drop-in, today's blocks (MEASURED, 32-bit)

| Codec | MiB | × writer | compress MB/s | ms/s avg / busiest | decompress MB/s | memory (est.) | DLL |
|---|---:|---:|---:|---:|---:|---:|---:|
| lodepng, 2 KB (today) | 97.97 | 1.00 | 24 | 38 / 130 | 129 | – | in tree |
| lodepng, 32 KB window | 82.45 | 0.84 | 3.6 | 251 / **900** | 159 | – | in tree |
| libdeflate 6 (zlib format) | 82.76 | 0.84 | 98 | 9 / 35 | 1,063 | – | +43 KB |
| zlib 6 / 9, 32 KB (64-bit) | 83.14 / 82.14 | 0.85 / 0.84 | 37 / 6.4 | – | 520 | – | – |
| lz4 / lz4hc 9 | 116.1 / 85.6 | 1.18 / 0.87 | 747 / 43 | 1.2 / 4; 21 / 80 | 3,100 / 4,200 | – | – |
| zstd 1 | 72.79 | 0.74 | 418 | 2.2 / 8 | 1,065 | 1.4 MB | |
| **zstd 3** | **66.83** | **0.68** | **254** | **3.6 / 13** | **1,002** | 3.7 MB | **+609 KB** |
| zstd 9 | 63.87 | 0.65 | 47 | 19 / 73 | 1,089 | 17.6 MB | |
| zstd 19 | 59.51 | 0.61 | 5.3 | 172 / 547 | 938 | 94 MB | |
| brotli q5, 4 MB window (64-bit) | 60.35 | 0.62 | 83 | – | 606 | – | +812 KB |
| lzma 6, lc0 lp2 pb2 (64-bit) | 51.57 | 0.53 | 4.3 | – | 119 | – | – |

lodepng with a 32 KB window cannot keep up: 27 s of CPU for the busiest 30 s on the reference
setup. miniz is the same 32 KB deflate class as zlib and libdeflate (~0.84×) [INFERRED, not run].
zstd sizes: `-O2 --gc-sections`, compressor + decompressor; `-Os` +396 KB; excluding the
strategies above level 5 (`ZSTD_EXCLUDE_*`) saves 109 KB of text; the decoder alone is 148 KB.
zstd is BSD-3 (dual GPLv2), C99, and builds warning-free with `i686-w64-mingw32-gcc`.

## 3. Block length, chains, dictionaries (MEASURED, zstd 3, today's records)

| iterations a block | 1 | 3 | 10 | 30 | 100 | 300 | 900 | today's (256 KB / 300) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| each block alone | 91.84 | 72.71 | 63.75 | 60.47 | 58.92 | 58.40 | 58.29 | 66.83 |
| chained: one frame a snapshot interval, flushed at each block | 59.01 | 58.59 | 58.54 | 58.53 | 58.52 | 58.52 | 58.52 | 58.53 |

Today's blocks hold 1–3 iterations at the battle's peak (10 on average), which costs 6.4 MiB
against 30-iteration blocks. A chain
(`ZSTD_e_flush` at each block, `ZSTD_e_end` before each snapshot) makes the ratio independent of
block length. **Trained dictionaries gain nothing** (16/112 KB, trained on the battle's first
900 iterations or on `tenseat`: 66.99–67.40 vs 66.83; at one iteration a block 90.8–92.7 vs
91.8); the snapshot as a raw-content prefix gains nothing (58.53); a 16 MB window and long-distance
matching gain 0.1–0.3 MiB. The previous blocks of the interval are the dictionary that works.

## 4. A sectioned container (MEASURED, round trip exact)

Each block: a **kind sequence** (`u8 kind, varint run`), then one **section per category**, each
record's body in its category's section: unit-delta heads (slot gap, mask XOR list), unit values
**by column as the difference from the slot's previous value**, projectile heads, projectile values
**raw in record order**, cells, orders, events (varint length only where the format lets it vary),
the rest. Lengths are implicit wherever `tagpu_recfmt.h` already fixes them.

Ladder, 30-iteration blocks, zstd 3, each block alone:

| Step | MiB |
|---|---:|
| today's records, today's blocks | 66.83 |
| 30-iteration blocks | 60.47 |
| + framing H3 D1 (the values after the heads) | 55.14 |
| + sections, values raw | 52.23 |
| + unit values as column differences, **inside the block only** (self-contained blocks) | **35.82** (36.53 with a 2 MiB cap) |
| ... against the previous block's world (a reader needs the world at the block's start) | 34.44 |
| ... and chained through the snapshot interval | 33.24 |
| zstd 9 chained / zstd 19 / brotli q5 / lzma 6 lp2 pb2 (on the 34.44 layout) | 29.90 / 29.66 / 30.43 / 26.34 |

The unit-difference step is idea 1 of the size note (the units report's area); the container is
what carries it. Per category, self-contained, zstd 3 (MiB): unit deltas 46.7 → 15.9, projectiles
36.5 → 13.0, cells 3.5 → 2.1, orders 2.8 → 1.7, events 2.9 → 2.1, the rest 1.1 → 0.9.

**Projectiles must not be transposed or differenced**: by column +2.0 MiB, byte-shuffled (Blosc)
+5.0, by column as differences +16.7. Why record order wins is not established (the obvious
candidate, the tail `+0x10` being last tick's position, holds for 271 of 488,054 deltas in the
first 3,000 iterations). The container therefore needs a per-kind layout, keyed by record kind
(four delta kinds), not by field.

32-bit, self-contained 30-iteration blocks: **zstd 3 2.0 ms/s average, 6.7 ms/s busiest**,
decompression 1.23 GB/s; zstd 5 34.2 MiB at 5.5 / 20 ms/s; zstd 9 33.3 at 12 / 46 ms/s. The
re-layout itself was run in Python only; in C it is one pass over records the writer already
parses [INFERRED cheap, not measured]. Largest block: 6.1 MB (the first iteration's grid, as
today), ~5 MB at the battle's peak; with the cap a block ends once it has taken 2 MiB of records
from the ring, as `STREAM_BYTES` ends one at 256 KB today.

Per 30 s of game (the 34.44 layout): 40.4 → 13.2 MiB in the first, **4.72 → 1.75 MiB in the
~400-unit tail: 567 → 211 MiB an hour** (self-contained blocks ~4 % more). The 100 MB/h budget
still needs the semantic and sampled ideas.

## 5. Entropy coding beyond LZ (MEASURED as model cost)

The delta kinds' changed values alone (102.0 MiB raw), today's blocks, models reset per block.
An adaptive model's cost (−log2 of the KT probability) is what a rANS with adaptive frequencies
emits to within a fraction of a percent.

| Coder | raw values | differences | 2nd-order residual |
|---|---:|---:|---:|
| zstd 3 / zstd 19, columns | 45.0 / 37.8 | 43.1 / 39.9 | 48.2 / 45.1 |
| zstd 19 per column alone | – | 41.1 | 46.0 |
| order-0 per byte plane | 49.9 | 45.6 | – |
| order-0 per column: bit-length bucket + raw bits | – | 65.2 | 52.6 |
| order-1 (same unit's last bucket / neighbour's) | – | 65.2 / 65.7 | 52.8 / 53.3 |

**No**: order-0/1 rANS loses to zstd by 5–50 %; the values repeat exactly, which LZ matches and an
order-0 model cannot. The ceiling a cheap context model shows is LZMA's binary-context literals:
23 % under zstd 3 on the sectioned blocks, at 7.5 MB/s (64-bit) and 120 MB/s decode — a third of
a core for the busiest 30 s on the reference setup [computed from the 64-bit speed]. A PAQ-class
model is slower still [INFERRED].

## 6. Effects

- **Seek**: decompression 8× faster (1.0–1.2 GB/s vs 129 MB/s): the busiest interval's stream
  decodes in ~0.06 s instead of 0.75 s; applying the records is unchanged and dominates
  [INFERRED]. Self-contained blocks keep today's property that any block decodes alone.
- **Takeover, verification**: untouched (lossless; snapshots unchanged; zstd 3 also shrinks them
  1.58 → 1.36 MiB).
- **Multiplayer**: the same sections at one iteration a block cost 61.9 MiB each alone and 36.7
  with a context shared across iterations; a shared context needs in-order delivery on the
  contribution channel (not established).
- **Readers**: both keep lodepng inflate for formats 5–7. `tarec.py` is standard-library-only on
  Python 3.12, which has no zstd (3.14's `compression.zstd` has).

**Risks.** zstd adds ~0.6 MB to a 2.6 MB DLL. A chain or a world-based difference makes a block
undecodable without its predecessors: the invariant would be "a chain and a difference base never
cross a snapshot or segment start; a reader starts only at one". Self-contained blocks need no
such invariant. The reader's bounds stay a bound by construction: output buffer exactly the
block's `raw` (zstd never writes past it), `RECR_BLOCK_MAX`, section lengths against `raw`,
varints at most 5 bytes, slots against the slot count.

## Ranked recommendations

1. **zstd 3 in place of lodepng's 2 KB deflate**, blocks as today: **98.0 → 66.8 MiB (−32 %)**,
   writer CPU 38 → 3.6 ms per game second (130 → 13 at the busiest), seek decompression 8× faster.
   +609 KB DLL. The smallest change: `block()`, a codec flag in `RecBlock.flags`, both readers.
2. **Self-contained 30-iteration blocks with the sectioned layout** (kind sequence, implicit
   lengths, slot gaps, per-category sections, unit values as in-block column differences,
   projectiles in record order), at most 2 MiB of ring records a block, a block ending at every
   snapshot tick:
   **66.8 → 36.5 MiB (0.37× today)**, 2.0 / 6.7 ms/s. Of the 30.3 MiB, 14.6 is the container's own
   (block length, framing, sections) and 15.7 the unit differences.
3. Optional, each MEASURED on top of 2: differences against the previous block's world −1.4 MiB;
   chaining −1.1; zstd 5 −1.6 at 3× the CPU; zstd 9 chained down to 29.9 (17.6 MB writer memory).
4. **Not recommended**: trained dictionaries (0), sparse mask codes under zstd (≈0), byte shuffle
   (+5 MiB on projectiles), rANS/context models (worse, or ~50× slower), lzma and zstd 19 (writer
   CPU), lodepng 32 KB (cannot keep up), lz4 (bigger than today).

**Questions for the owner**: may `tarec.py` depend on a non-standard module — the `zstandard`
package (already in the shared venv), a ctypes binding to a system libzstd, or Python 3.14+?
Is +0.6 MB of DLL acceptable for zstd? Self-contained blocks (simpler readers) or the 2.5 MiB
(7 %) that chains and world-based differences save?
