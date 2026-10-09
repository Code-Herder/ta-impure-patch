# L0 weapons: does the identity-coded projectile stream generalize?

MEASURED unless marked. MiB = 2^20 B; "writer" = deflated alone per stream block. Outputs: `out/`.

## Which tier, which files

The final tier ("+ shot events as references", 2.55 MiB) is `p10_measure.py <file> event`:
`codec4.Codec4(stamp=True, event=True, xpos=False)` over `codec.py`/`codec2.py`/`codec3.py`/`pool.py`,
unrestricted search. The round's Town & Country (28.2 → 2.44) and `naval.imprec` (4.85 → 0.46)
runs were **this tier but not these exact files**: in the round's scratch folder their outputs are
dated 20:56–20:57, `codec2–4.py` 21:08 (byte-identical to the committed ones) and `p10_measure.py`
21:14; the two outputs lack the search-count lines. What changed is unrecorded (likely the seek
reset of `lastnew` and `RESTRICT` [INFERRED]). Below: the committed scripts, unchanged, on new files.

## 1. Lossless, and size (final tier, committed files unchanged)

| Recording | iterations decoded exactly | today (writer) | final tier | ratio | + guided rule (§3) | ratio |
|---|---:|---:|---:|---:|---:|---:|
| size-battle (reference) | 7,830 / 7,830 | 36.503 | 2.547 | 14.3× | 2.548 | 14.3× |
| battle.imprec (Town & Country) | 4,021 / 4,021 | 15.145 | 1.274 | 11.9× | 1.049 | 14.4× |
| naval-push.imprec | 3,841 / 3,841 | 5.012 | 0.487 | 10.3× | 0.348 | 14.4× |
| air-war.imprec | 5,610 / 5,610 | 3.365 | 0.493 | 6.8× | 0.348 | 9.7× |
| (round) T&C fixture / naval.imprec | 6,152 / 3,810 | 28.222 / 4.847 | 2.443 / 0.461 | 11.6× / 10.5× | | |

**No mismatch on any iteration of any recording**, in all 51 runs (every variant below). Survivors
are 0.835 / 0.388 / 0.359 MiB of the battle / naval / air-war streams, births 0.352 / 0.080 / 0.099.

## 2. The engine facts the codec uses: how often they hold

| Fact (`p1b`, `p7`, `p11`) | size-battle | battle | naval-push | air-war |
|---|---:|---:|---:|---:|
| `+0x67` stamp = old index (survivors) | 517,055 / 539,736 (95.8 %) | 263,081 / 264,803 (99.3 %) | 123,751 / 123,751 | 77,128 / 77,177 |
| birth stamp ≥ old count | 56,906 / 57,078 | 18,335 / 18,380 | 2,146 / 2,216 | 2,982 / 2,983 |
| birth tail = its event's fire point | 25,566 / 25,566 | 8,105 / 8,136 | 2,146 / 2,146 | **1,227 / 2,031** |
| survivors with any nonzero residual (fixed modes) | 43.4 % | 43.9 % | 70.8 % | 50.5 % |
| position residuals ≠ 0 (phys rule), of which burst templates | 131,238 / 108 k | 37,333 / 32,307 | 3,474 / 0 | 6,760 / 2,980 |

- **Stamp**: holds better than on the reference. Misses come in runs one off the old index (battle
  tick 1,237, slots 10–19): a record left the pool between the tick's compaction and the capture,
  consistent with challenger A's second compactor `0x49C8F4` [INFERRED]. Battle cost: 10.8 KiB.
- **Event → birth** misses are whole weapons: 110 and 109 on the battle (31 births, aircraft
  weapons [INFERRED]), 114, 115, 109 and weapon 19's burst templates on air-war. The
  tail sits 4–16 world units per axis from the fire point, never by the velocity (`tailmiss.py`):
  those weapons launch elsewhere than the event's point.
- **Position ← velocity + drift** holds on slopes (battle: 5,026 nonzero position dwords in 237 k
  flight transitions besides the templates). Air-war weapons 6 and 4 (from aircraft [INFERRED])
  miss on up to 15 % / 11 % of transitions.

## 3. What changes off the reference battle: guided flight

On the reference, heading/pitch/speed (d13, d14) never changed and horizontal velocity moved only
by burst spray. Off it, guided weapons (missiles, torpedoes) dominate the survivors:

| survivors' columns, writer KiB | size-battle | battle | naval-push | air-war |
|---|---:|---:|---:|---:|
| velocity d7+d8+d9 | 192 | 368 | 192 | 190 |
| heading, pitch, speed d13+d14 | < 30 | 183 | 121 | 99 |
| share of the final tier | ≈ 8 % | 43 % | 63 % | 58 % |

The engine rebuilds a guided projectile's velocity each tick from its own heading `+0x36`, pitch
`+0x38` and speed `+0x3A` (DISASSEMBLED `0x49BA74..0x49BABE`: vy = S(pitch, speed), h = C(pitch,
speed), vx = −S(heading, h), vz = −C(heading, h); S, C = `0x4B70EF`, `0x4B7123` on the sine table
`0x509F00`). On the records (`guided.py`), of survivor transitions whose horizontal velocity changed,
the rule is exact in 40,780 / 46,559 (battle; the rest are burst spray), 34,578 / 34,616 (naval),
24,523 / 25,558 (air-war), and in every guided birth. **The codec has no such predictor; this is
the main thing the reference could not show.** Added as a survivor mode for d7–d9 (`codec5.py`:
d13–d15 decode first; per row, the rule on the new record when the old record followed it, else
linear), it decodes exactly: battle 1.274 → 1.049, naval 0.487 → 0.348, air-war 0.493 → 0.348,
reference 2.547 → 2.548. What remains is d13/d14: the aim `0x49B3E0` and turn `0x49B520` read the
target, so they stay residuals.

## 4. Cost: the encoder's reference search

Dword compares per iteration (= one tick); busiest second = max over 30 consecutive iterations
(reproduces challenger A's 288.0 M).

| | size-battle | battle | naval-push | air-war |
|---|---:|---:|---:|---:|
| final tier (unrestricted): mean / p99 / max | 611,571 / 7.09 M / 18.8 M | 98,532 / 397 k / 561 k | 10,098 / 99.8 k / 195 k | 6,771 / 92.5 k / 150 k |
| busiest second | 447 M | 10.6 M | 2.7 M | 2.6 M |
| restricted (same weapon, live pool): mean / p99 / max | 153,936 / 3.41 M / 12.4 M | 45,312 / 210 k / 279 k | 938 / 11.5 k / 62.6 k | 2,042 / 36.5 k / 76.4 k |
| busiest second | 288 M | 5.8 M | 0.3 M | 1.2 M |
| writer MiB (unrestricted / restricted) | 2.547 / 2.541 | 1.274 / 1.276 | 0.487 / 0.486 | 0.493 / 0.493 |

Quadratic in births × same-weapon rows, it explodes only on the reference (weapon 17's burst
copies). A **positional cap** (K rows nearest the default guess per table section) costs size there:
2.541 → 2.976 / 3.824 / 5.073 MiB at K = 64 / 16 / 4 (battle 1.294 / 1.500 / 1.837): a burst
copy's best reference is its template or a sibling, anywhere in the pool — **the same shooter**.
Ordering each section by (same shooter `+0x52` first, then distance) puts the chosen row at rank
< 4 in 114,686 / 115,138 reference searches (nearest-first: 57,535) and 36,771 / 36,785 battle ones:

| shooter-first cap K (per section, ≤ 4K rows) | size-battle | battle | naval-push | air-war |
|---|---:|---:|---:|---:|
| writer MiB, K = 4 | 2.539 | 1.276 | 0.486 | 0.493 |
| mean / p99 / max compares, K = 4 | 5,105 / 43.6 k / 88.0 k | 3,093 / 9.8 k / 14.2 k | 341 / 3.4 k / 12.1 k | 305 / 3.8 k / 6.0 k |
| busiest second, K = 4 | 2.2 M | 0.3 M | 0.1 M | 0.1 M |
| K = 16: writer MiB / max compares | 2.541 / 348 k | 1.276 / 48 k | 0.486 / 42 k | 0.493 / 22 k |
| K = 2: writer MiB / max compares | 2.618 / 44 k | 1.315 / 7.3 k | 0.486 / 6.0 k | 0.497 / 3.0 k |
| K = 1: writer MiB / max compares | 2.720 / 22 k | 1.348 / 3.7 k | 0.485 / 3.0 k | 0.498 / 1.6 k |

**Proposal: K = 4 per section, shooter first**: ≤ 16 rows × 27 = 432 compares per searched
record. Worst iteration on the reference 88 k (restricted 12.4 M, unrestricted 18.8 M), busiest
second 2.2 M (288 M). Size cost: none (−0.002 to 0.000 MiB on all four); K = 2 and 1 cost 3–7 %.
Selection needs per-section lists by (weapon, shooter), not a scan [INFERRED; the prototype reads
22,651 weapon dwords an iteration]. Format and decoder unchanged.

Prototype time and memory stay linear (battle 49 s / 467 MB); nothing else grew.

## Files

`p10_l0.py` = a modified copy of `p10_measure.py` (imports the committed modules; args `restrict`,
`cap=K`, `byshooter`, `guided`; saves per-iteration counts); `capsearch.py` (counted, capped search;
`cap=0` reproduces committed `RESTRICT` exactly: 2.541, 153,936 / 3,409,798 / 12,411,819);
`codec5.py` + `guidedrule.py` (guided predictor, monkeypatch); `guided.py`, `tailmiss.py`.
Committed scripts ran in place, unchanged, bytecode writing off.
