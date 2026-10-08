# Challenge A: the five size reports, checked

Disassembly: `full.s`. Re-runs in this folder: `zstd_dropin.py`, `rulecheck.py` (my own delta replay),
`snapequal.py`, `snapalign.py`, `lanestats_keepwalk.py`, units' `run2.py`, weapons' `p10_measure.py`
(plain and `restrict`, `p10_wmax.py`). Sizes are MiB unless marked.

## 1. Headline numbers

| Claim | Verdict | Evidence |
|---|---|---|
| units 46.68 → 4.50 lossless | **CONFIRMED** | Re-ran `run2.py` (current `frame2.py`): 4.497 writer; 7,830 iterations, 4,669,500 images decoded from the bytes, 0 mismatches. The decoder takes from outside only created units' RK_UNIT images, liveness and GameTime. |
| units' seek (reset after each snapshot) | **CONFIRMED by equivalence** | The decoder keeps its own L1 instead of loading the snapshot. MEASURED equivalent: at all 8 snapshot ticks the snapshot's live set and images equal the stream state, and a block ends there. |
| weapons 36.50 → 2.55 lossless | **CONFIRMED** | Re-ran `p10_measure.py … event`: 36.503 → 2.547; 7,830/7,830 pools equal (count, hw, every slot below hw); `restrict` 2.541. Seek: `p18_seek.py` (not re-run). |
| coding: zstd-3 drop-in, 98.0 → 66.8 | **CONFIRMED** | Independent script: 97.965 → 66.831. Blocks hold 9.97 iterations on average (median 7). |
| coding: sectioned blocks, 36.5 (35.82 uncapped) | **CONFIRMED from the outputs, not re-run** | `container.py` compares each decoded block with the original payload; the outputs say "roundtrip OK". |
| discrete, ~10 → 3.5 | **CONFIRMED (arithmetic)** | The Sum row includes the first grid (10,393 → 3,727 KiB); without it 9.91 → 3.40. Interleaved 12.69 → 3.39. Decoders compare with the parsed originals (not re-run). |
| fidelity: 7.88 lossless, 4.45 truncated | **arithmetic CONFIRMED; basis not comparable** | The sums add up. Basis: 10⁶ B, zstd-19, 300-tick blocks, sub-streams compressed separately; "today 48.95" is the *writer* figure, so one table mixes two compressors. Only the motion columns were round-tripped. |

## 2. Engine facts (DISASSEMBLED by me unless marked)

| Fact | Verdict | Evidence |
|---|---|---|
| mover+0x2A = GameTime when pos+vel ≠ pos or the layer changed | **CONFIRMED** | 0x43D7FF..0x43D865. The engine compares mover+0x2E&3 with **unit+0x110&3**; units' rule uses the mover's previous layer (measured exact; a prediction only). |
| reload countdown | **CONFIRMED** | 0x49E1C8..0x49E1D2: u16 at slot+0x14, decremented while nonzero. AutoAim is reached only for owner type 1 or 2 (0x48ADCE..0x48ADDA). |
| +0xFA / +0xFB countdowns | **CONFIRMED** | 0x48ADF0..0x48AE0D. +0xFB is a dword. |
| velocity = −(sin, 0, cos)(h) × speed from the table 0x509F00 (units) | **CONFIRMED** | The setter 0x43CC20 (called at 0x43CD52, 0x43D0C0) writes vx = −0x4B70EF(h,s), vy = 0, vz = −0x4B7123(h,s) at 0x43CCDB..0x43CD13 (`imul`, `+0x1000`, `shrd 13`). MEASURED with my own replay: 4,362,632 of 4,362,632 live mover unit-iterations exact. |
| "velocity is x87 floating point, exact recomputation unsafe" (fidelity) | **WRONG** | 0x4FB440 on `fild` doubles plus `__ftol` 0x4E43A0 compute the heading toward the goal. 0x4E43D0, 0x4E44F0, 0x4E4400 and 0x4E4440 are integer 64-bit shift, multiply and divide helpers. The velocity itself is the integer table. |
| position ← velocity; heading ← turn rate (0x43CFD1..0x43CFDB) | **CONFIRMED** | MEASURED: 1,991,061 of 1,991,061 with mover+0x2E bit 2 clear. The heading rule misses 965 of 4,362,632 (fidelity's 965 agrees). |
| compaction 0x49AE20; +0x67 stamped on every record | **CONFIRMED** | 0x49AE5F stamps before the removal test; the copy keeps the order. The tick counts the pool once (0x49B728) and always ends in 0x49BE53. **Missed by weapons:** a second caller, 0x49C8F4 in the dying unit's cleanup 0x49C880, compacts outside the tick (bytes only). |
| order +0x0A wake time | **CONFIRMED** | 0x439E80 (75 call sites); the constructor stores −1 at 0x43A145; 0x43B94F..0x43B970. |
| Park–Miller RNG | **CONFIRMED** | 0x4B6C30 (×16807; n < 2 returns without stepping). |
| mover +0x26 "not identified" (units) | **WRONG, bytes only** | It is a **dword** GameTime claim stamp, written by the claim 0x440A70 at 0x440A91. Units' lanes split it into two u16. "+0x28 constant" holds only while GameTime < 65,536 (36 min). |
| Position readers that use the high word only | **CONFIRMED** | 0x48AA0C: cell = ⌊(H−8fw+8)/16⌋ (truncation safe, rounding not). Also: 0x47CCA9 (`sar 23`); 0x4825D4, 0x43DB71, the minimap 0x466E83..0x466EA7 (`movsx`); the circle branch 0x4826E1..0x482717 (`sar 21`, for x, z ≥ 0); the raise 0x482817. The claim 0x47CC30 reads the position only at 0x47CCA9. |
| **A reader the reports missed** | fidelity's "derived state exact by construction" is **WRONG** | The radar pass 0x467440 calls the bucket walk 0x47E890 (0x4675B8, 0x46762C, 0x467655), which subtracts the **full 16.16** x and z dwords (0x47E995..0x47E9A2) and tests ⌊dx²/2³²⌋+⌊dz²/2³²⌋ ≤ r²; its visitors store +0x110 bits 8–10. Truncation can flip a unit at a radar/sonar/jammer range edge, and the harness compares bits 8–10 when the host shows the recording's viewed player. Frequency not measured. |
| 0x48A870 on a puppet | **answered** | The unit tick skips 0x43DD20 and 0x48A870 for owner type ∉ {1, 2} (0x48AEC4..0x48AECD → 0x48AFB9). The other callers (0x486109, 0x486306, 0x48BA4C) were not examined. |

## 3. Contradictions

**Units 4.50 against fidelity's 7.88 / 4.45: units is right.** On fidelity's own basis (zstd-19,
300-iteration blocks), units' lossless frame is **3.577 MiB = 3.75 MB**. That is smaller than
fidelity's *lossy* recommendation (4.45 MB). The two differ in prediction:

- Fidelity predicts x, z and altitude linearly, and the velocity from the position step (1.1 % exact).
- Units integrate: heading from turn rate, velocity from the table, position from velocity.

So:

- Fidelity's idea 2 (derive velocity, −1.41 MB, a loosening) is unnecessary: the velocity is exact
  from speed and heading.
- Its idea 1 (truncation, −2.0 MB) is measured against the weak baseline. On units' frame, x, z and
  y cost 0.32 + 0.33 + 0.05 (lanestats re-run), so truncation saves **at most ≈0.7**.
- An 8-bit heading would break the heading ← turn-rate rule.

**Units' lossy numbers are WRONG on the puppet.**

- They drop speed (mover+0x20) and turn rate (+0x24).
- `unit_put` never writes the walk class raw. E_Walk 0x43DA70 derives it from the speed against
  def+0x1AE/+0x1B2, a zero test of +0x24, bit 2 and the carrier.
- A class change starts StartMoving, StopMoving or MoveRate (strings at 0x505230, 0x50523C,
  0x505224; `call 0x4B0940` at 0x43DAF2).

Keeping speed and turn rate exact, on the same attribution method:

| | attribution |
|---|---:|
| lossless frame | 4.72 |
| units' floor (all motion dropped) | 2.28 |
| floor keeping speed and turn rate (my run) | **3.65** |

- **Any motion loosening saves at most ≈1.0 MiB**, not 2.5.
- ε = 1/2 becomes ≈3.9 and 10 Hz ≈4.2 [INFERRED: attribution difference scaled by 4.50/4.72].
- The alternative is a puppet change that records the class.

**"Same as last" flags and dictionaries: no conflict.**

- Units' own ladder shows the mask-dictionary/MRU step at zstd-19 14.17 → 14.17. Coding's D2 under
  zstd-3: 60.70 → 60.96.
- Both are 2 KB-window artifacts that disappear under zstd.
- Discrete keeps most of its gain under zstd-19 (7.10 → 3.36).
- Coding's trained dictionaries are not discrete's symbol tables.

**Coding's "projectiles must not be transposed or differenced"** holds only for differences keyed by
slot. The compaction moves 88 % of survivors (confirmed above). Weapons' columns keyed by identity
reach 2.55, so coding's 13.0 for projectiles is superseded.

**The savings do not add.**

- On units' frame, zstd-3 is *worse* than today's deflate (4.63 against 4.50); on weapons, zstd-19
  gives 2.42 against 2.55.
- After the semantic codecs, the compressor is a CPU and seek choice, not a size one.
- The lossless attributions sum to ≈4.50 + 2.55 + 3.64 + 1.6 (snapshots) + 0.2 ≈ **12.5**
  [INFERRED: a sum of categories; interleaving not measured].

## 4. Hidden assumptions

- **One flat ground battle.** The rules are the ground setter's (0x43CC20). The air step 0x43D290 also
  calls 0x4B70EF/0x4B7123 (0x43D452, 0x43D468) after x87 work [INFERRED: a similar rule may hold;
  unmeasured]. Weapons alone tested three recordings.
- **Resets at snapshots.**
  - Units: equivalence measured (above).
  - Weapons: tested explicitly.
  - Coding: blocks are self-contained.
  - Discrete cells and globals, and fidelity: no reset modelled.
- **Units' writer CPU:** the "~1 ms/iteration in C" is unmeasured. The Python run takes 104 s for
  encode plus decode.
- **The weapons encoder** (MEASURED by my `restrict` re-run):
  - average 154 k dword compares an iteration;
  - but 255 iterations exceed 1 M, the busiest second is **288 M**, and the busiest 30 s average
    35.7 M/s;
  - the unrestricted 2.547 encoder: 612 k average, p99 7.1 M, max 18.8 M.
  - At an assumed 1–2 ns a compare (32-bit build, no SSE2) the busiest second costs 0.3–0.6 s of
    writer CPU [INFERRED]. A candidate cap is the bound the design needs, and its size cost is
    unmeasured.
- **The capture's own worst case** is already 1,892 µs (`tagpu.log`), over the 1 ms budget. Every
  proposal here is writer-side and leaves it unchanged.

## Corrected numbers

| Item | Reported | Corrected |
|---|---|---|
| Units' lossy headroom over the lossless frame | 2.5 | **≤ ≈1.0** with exact walk-class inputs |
| Units ε = 1/2 / 10 Hz | 2.56 / 2.87 | ≈3.9 / ≈4.2 [INFERRED], or a puppet change |
| Fidelity's truncation gain | 2.0 MB | **≤ ≈0.7** on units' frame |
| Fidelity's velocity derivation | −1.41 MB, lossy | 0 (exact for free) |
| Fidelity's lossless baseline | 7.88 MB | 3.75 MB (units, same basis) |
| Weapons' search (unrestricted) | 154 k / 3.4 M / 12.4 M | that is `restrict`; unrestricted 612 k / 7.1 M / 18.8 M; busiest second 288 M (restricted) |
