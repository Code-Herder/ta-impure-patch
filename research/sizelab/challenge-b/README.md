# Challenge B: the five plans as one design

MiB throughout (recsize's "MB"). **M** = measured (by the named report, or by me: scripts and
outputs in `challenge-b/`), **I** = inferred. In MiB the budgets are 95.4 (ten players), 23.8 (four)
and 9.5 (1v1) an hour.

## 1. The combined saving

**The containers overlap; the codecs do not.** Coding's sectioned container saves 14.6 MiB of its
own, but that is mostly unit and projectile framing, which the units and weapons codecs remove
anyway. The container therefore carries the stack and adds nothing to it.

Each category below was round-tripped by its author. All use one compressor (the writer's 2 KB
deflate) and today's block boundaries. The stack total is a sum (I).

| Category | Today, this file | Lossless stack | Tail today, MiB/h | Tail lossless, MiB/h | Lossy option |
|---|---:|---:|---:|---:|---|
| Unit deltas | 46.68 | **4.50** with the engine rules (units M); **7.30** with the integrators only (M here, `units_rules_a.out`: 0 of 7,830 iterations differ); 9.84 generic (units M) | 302.9 | 27.9 (units M); ~45 integrators, ~61 generic (I, scaled) | ε 1 wu/2°: 2.56 M; tail 15.8–18.1 M |
| Projectiles | 36.50 | 2.55 (weapons M) | 131.6 | **15.0** (M here, `proj_window.out`; the tail compresses 0.114×, against 0.070× for the whole battle) | about −1 (weapons I) |
| Cells, orders, events, wrecks, globals, players | 10.15 | 3.64 (discrete M) | 62.5 | 20.6 (I, at the whole battle's ratios); up to ~30 if this tail compresses worse, as the projectiles' does | — |
| Other stream kinds | 0.14 | 0.14 (no report covers them) | 0.6 | 0.6 | — |
| Interleaving loss (real − the sum of categories) | 4.5 | ~0 (I: sections) | 69.3 (M here) | ~0 (I) | — |
| Snapshots, every 900 ticks | 1.59 | 1.22 (wreck base, discrete M) | 22.5 (M here) | 17.2 (I) | the leftover projectiles out: −0.37 (zstd 19; a loosening) |
| **The file** | **99.6** | **12.1** (14.9 integrators, 17.4 generic) | **589** | **81–91** | about 55–65 (I) |

The tail is GameTime 4500–7199: about 390 units, still fighting (`window_attr.py`).

**After the codecs the compressor barely matters** (M, the reports' own columns): zlib 9 gains
0.4–1 %, zstd 19 3–8 %, and zstd 3 is 3 % *worse* on units; its 32 % applies to today's records
only. Snapshots differ (M here, `snap_comp.out`): writer 1,629 KiB, zlib 9 1,481, zstd 3 1,393,
zstd 19 1,164.

**Budgets:**

- **Ten players at about 400 units:** the stack with the engine rules comes to 81–91 MiB/h, inside
  the budget but barely. With the integrators only it is 98–108, at or over. Generic: about 114.
- **Ten players with 1,500 fighting units:** over the budget under every design. At the
  2,000-unit peak, units alone come to 205 MB/h lossless, and the lossy option cuts units by about
  40 %, not threefold. The budget holds only if the hour is mostly quiet, as the product page
  already says.
- **Four players: not established.** At about 0.17 MiB/h per fighting unit plus a floor (I),
  23.8 MiB/h holds only with about 100 moving or fighting units on average; idle structures cost
  about nothing. Only the measured hour settles it, and multiplayer adds contributions (§4).

## 2. Conflicts and choices

**Fidelity against units.**

- At the same compressor and block length, fidelity's recommended lossy stream is 4.24 MiB, and
  units' lossless stream is 3.58 MiB.
- Fidelity measured against a weaker lossless baseline (7.51 MiB). Truncation and the engine rules
  remove the same low bits, so they are alternatives, not a stack.
- They also conflict: an 8-bit heading cannot index the 512-entry sine table, and truncated
  positions break `pos = pos + v`.
- On top of the rules, truncation can win at most the x/z residuals, 0.65 MiB.
- Fidelity's correctness findings still bind any lossy option: speed, turn rate and derived
  fields exact; never round, never quantize a change; the gate compares the host EQUAL to the
  *decode*, plus a truth-within-ε check.

**The contract's 10 Hz** rests on an estimate that lossy recording was needed. Measured: lossless
shrinks units 10.4×, and lossy adds only 1.6–1.9 MiB (about 11 MiB/h at the tail). Making it
"lossless; lossy only if the measured hour misses" changes the plan: **escalation 1**.

**Rules, tiered** (the integrator tier measured here):

| Tier | What it predicts | Units, MiB |
|---|---|---:|
| Generic | — | 9.84 |
| Integrators | heading += turn rate, position += velocity, the moved stamp, the countdowns — "lane plus lane", a general mechanism | 7.30 |
| + the engine's function | velocity from the sine table `0x509F00` × speed | 4.50 |

- The table rule is worth about 17 MiB/h at the tail (I): the margin between meeting and missing
  the ten-player budget.
- A wrong rule costs only bytes. The writer and a reader disagreeing is the only hazard, and §3's
  check closes it.

**Self-contained or chained.**

- Today's deltas already need the world from the snapshot on, and the best numbers of units,
  weapons and discrete all chain their predictors through the interval.
- The invariant is therefore needed regardless: every codec context resets at the first iteration
  after a snapshot and at a segment's first iteration. The latter is whole, by the reader's own
  invariant (the head of `tagpu_recreader.c`).
- **Keep lodepng; no zstd** (609 KB of DLL and a Python dependency for about 5 %; its chaining
  saved 1.1 MiB on coding's layout).
- Longer blocks come free: `STREAM_BYTES` cuts a stream about 6× smaller (I), while `STREAM_TICKS`
  still bounds a block, and a crash's loss, at 10 s. Most of units' 300-iteration gain
  (4.50 → 3.77) follows (I).

**Projectiles from launch: not now** (as weapons argues). At most 1.1 MiB, a contract change, and an
unmeasured determinism.

## 3. Risks

**Seek.**

- The C seek reads the segment's first iteration (`tagpu_recreader.c` ~2071); the reset makes it
  decodable.
- The prototypes predict every live unit every iteration (`frame2.py`). At 2,000 units that is
  ~226 k lane predictions per iteration, or 0.2–0.6 s for a 900-iteration seek (I), against the
  one-second target.
- **Required:** decode work grows with the units whose predictor winner is not "previous". Measure
  the seek in C.

**Takeover.**

- Lossless stays exact.
- Units' lossy variant drops the mover's velocity, speed and turn rate between snapshots, so a
  takeover there must snap to an exact point.

**A codec bug: make decode-before-write the container's rule for every codec**, not only for
projectiles.

- A section is written in a codec only after the shipped C decoder has rebuilt that iteration's
  world from the bytes; otherwise it is written as today's records.
- That is an ordering, so a bug costs size, never data.
- What it requires:
  - predictor state is a function of the decoded world, updated identically whatever codec a block
    used;
  - one C decoder serves both the check and the reader;
  - a Python decoder that diverges is caught by `tarec.py check` at the snapshots, which are written
    from the *input*.
- What it costs (I): a second world copy (~10 MB) and about twice the codec CPU.

**The writer.**

- Its CPU in C is unmeasured (every figure is I). Weapons' search reaches 12.4 M compares in one
  iteration: it needs a cap.
- The 16 MB ring (`tagpu_recorder.h:130`) holds about 5 s of the peak input (92 MB raw in the first
  30 s); a writer lagging that long ends the recording (`REC_END_RING`). Measure the 32-bit build
  there. The game thread is unchanged.

**Format and readers.** The recorder is in no release (absent from v0.3.1.1 and `origin/main`), so
`REC_FORMAT` may bump per landing; the records fallback stays as a codec. `tarec.py` is
standard-library only: numpy decoders add a dependency, pure Python may be slow over an hour (I).

## 4. What is missing

1. **Multiplayer contributions.**
   - Unit kind 8 is a 20-byte prefix holding today's 11-byte changed-word mask, sent for every
     changed owned unit at every capture (`mp_units_capture`).
   - It is stored as exact source packets with checked proofs (`RB_CONTRIBUTIONS`), so the file
     writer cannot re-encode it.
   - Every owner's units are stored twice, once observed and once as authority, and the second copy
     stays in today's encoding. After the stack it would dominate a multiplayer file (I).
   - The fix is on the wire (ordered and guaranteed by design, so a codec can chain), with which
     copy playback needs: stage 6. No report measured it.
2. **The measured hour.** Nothing covers aircraft (the air step `0x43D290` is outside the
   rules), hills, an economy (resources change every tick), construction, a human's perspective
   (`REC_POV_EVERY` 3), a 1v1, or the quiet floor (pathfinding counters, idle orders waking every
   30–56 ticks, 8,192 wrecks a snapshot). Offline now: units' codec on weapons' naval and Town &
   Country recordings.
3. **Writer CPU, memory and seek time, in C.**
4. **`demo-recorder-size.md` needs corrections:**
   - idea 4 holds only before the codecs;
   - snapshots, called no target, are 21 % of the lossless tail;
   - idea 2's 10 Hz.

## 5. Order

- **L0, offline:**
  - the rule tiers on the naval and Town & Country recordings;
  - the measured hour's scenario.
- **L1, the unit frame:** the container, decode-before-write and the records fallback, with the
  unit frame at the owner's tier.
  - −39 to −42 MiB on this file; −258 to −275 MiB/h at the tail.
  - Verify: a game-free transcode test (an old file through the C codec, the C and Python
    readers equal at every iteration), `tareplay.py landing`, a re-recorded size battle
    (≤ 10 min), seek timing.
- **L2, projectiles** by identity, with a capped search: −34 MiB; −117 MiB/h. Same checks.
- **L3, discrete:** orders, cells (without the reorder), events, the small kinds, the snapshots'
  wreck base: −6.9 MiB; about −47 MiB/h (I).
- **L4, the measured hour** on the L1–L3 build. It decides the snapshot interval (open decision 3),
  whether lossy is needed, and the four-player verdict.
- **L5, lossy, only if L4 misses:** error-bounded; speed, turn rate and the derived fields exact;
  a takeover snaps to an exact point.
- **L6, contributions on the wire** (stage 6).

**Owner decisions:** (a) escalation 1, lossless replacing the 10 Hz; (b) the rule tier; (c) the
field-lane table or dword lanes (12.25); (d) no zstd; (e) dropping formats 5–7; (f) numpy in
`tarec.py`; (g) weapons' event→birth and `+0x67` facts; (h) the hour's wall time against the
10-minute rule; (i) L5's ε, tolerances and takeover rule (loosenings); (j) the snapshots' leftover
projectiles (a loosening); (k) which copy of a multiplayer unit playback needs.
