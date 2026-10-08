# Challenge: aircraft in the unit frame (`air/air.md`)

**Verdict.** The engine reading and every number survive re-derivation. Two things do not:
the coverage claim (no aircraft in the recording ever landed) and the claim that the
floating-point rules decode the same in C and in Python. That claim is unverified, and three holes
in the specification can break it. **All-integer** bank and turn rules cost +0.24 % and remove the
question. Scripts: `an/`, `lib_int/`; runs: `runs/*/run.out`.

## 1. The disassembly

**CONFIRMED (re-DISASSEMBLED).** The order is `0x43DD20` → `[vt+8]` → `0x43D290` (bit 11,
`0x43DD36..0x43DD44`) → `0x43D6D0` → `0x43DA70` (writes only `+0x110` bits 2–3) → `0x43DB50` (writes
only `+0x10A`). Rows 0–8 hold as written:
- the drift limit is skipped when m ≤ c (`test ah,0x41`);
- the vertical clamp takes its limit from the previous tick's speed;
- speed is trunc(fsqrt) of a sum that is exact while |v| < 2^25;
- Δv is the velocity after the step minus the velocity on entry (saved at `0x43D2C9`);
- the rotation's `DE E9` is st1 − st0, so x′ = x cos − z sin;
- `+0x68` reads the same x′ (`0x43D1D5`).

The doubles and floats at `0x509EF0`, `0x509EF8` and `0x4FD2D0..0x4FD2E0` match the report bit for
bit.

**WRONG: the coverage.** The "take-off/landing" column (200 iterations) holds **200 take-offs and
0 landings**. MEASURED: no layer 2→1 transition in 601,724 pairs, and all 13,272 landed iterations
precede the first take-off.

The landing, `0x43D210(1)` (`0x43D22A..0x43D25B`), zeroes v and speed and calls `0x43D0D0` with
Δv = 0. A_s decays once and `+0x64`/`+0x68` are recomputed on a layer-1 tick, where §5 predicts
"unchanged". This costs size only. The fix: on a 2→1 transition, predict decay(A_s) and its bank.

**Not covered, each costing size only:**
- cargo: `0x43D6E5..0x43D7EB` rewrites `+0x64..+0x69` from the transporter's piece (`0x43E180`) and
  copies the transporter's v and speed;
- the blocked branch `0x43D92F..0x43DA0B`: speed is clamped to maxv/2 and v is rebuilt with vy = 0;
- bankscale ≠ 1 and pitchscale ≠ 0, which no type here uses. `+0x68` is 0 on every airborne
  iteration, so its 100 % is trivial;
- the other LosType: with `main+0x14281` bit 2 clear, `0x4825B0` takes `0x4826B7`, which uses
  truncating division and another z cell;
- remote aircraft: the air mover is made only when `[+0x96]+0x73 == 3` (`0x43DC53`);
- other maps' gravity, construction aircraft, transports.

**Minor:** the sight emit runs from `0x4827B0`, and only for units with `+0x110` bit 28 set
(`0x465046`). Def `+0x170` is a byte there (`0x4827FB`).

## 2. The float claims

**CONFIRMED.** At start-up `0x4E4320` calls `_controlfp(0x10000, 0x30000)` at `0x4EA5B0`.
`_fpreset` (`0x4E8D00`) is `fninit` plus that call. All 31 `fldcw` lie at `0x4E43B6` or above. The
`fsave`/`frstor` pairs (`0x4E54A1`, `0x4EC738`) restore what they saved. `fnstenv`/`fldenv`
(`0x4F5623`) edit only the status word. Every other `_controlfp` call changes masks. One of them,
`0x4DF1B1`, **unmasks** invalid-operation and zero-divide when the game runs with `-fpufussy`
(`0x50D260`).

**CORRECTED.** The DLL source has no `fldcw`, but the built DLL has 482: GCC's save/set/restore
around float-to-int conversions. Also, mingw-w64's TLS callback `0x66E22080` executes `fninit`
(`0x66E22580`) on every DLL_THREAD_ATTACH, so each thread created after load starts at 0x037F
(precision 64). The game thread is the main thread, which gets only PROCESS_ATTACH, so the engine
claim stands. A writer thread in the DLL, however, runs at precision 64, so the explicit `0x027F` is
load-bearing. The measurements show only precision ≥ 53.

**Engine = format** holds on these inputs only. The 4,096-ulp margin bounds nothing beyond them.
That costs size, not correctness.

**Writer = reader: UNVERIFIED.** No C code exists. `frame3.bank_angle` uses numpy's libm in both the
encoder and the decoder of one process, so "0 mismatches" proves nothing about C against Python.
detmath equals libm and the engine on all 588,452 iterations (MEASURED).

The IEEE argument holds for + − × ÷ √ at precision 53, and detmath stays in the normal range. Three
spec holes remain:
- **(a)** The turned candidate clips at 2^20, so x1·dot reaches 2^62. FILD loads an int64 exactly,
  unrounded by precision control; numpy rounds it first, so the quotients can differ. The real
  maximum |A| is **26,214** (MEASURED). Clip at 2^16.
- **(b)** Python's `//` floors; `_alldiv` and C truncate. So `(acc<<16)/maxv`,
  `(gravity<<16)/0xCCD` and the brake quotient split on negative def data, and maxv = 0 faults the
  writer. Bound the def constants (otherwise the type is not air) and name the operator.
- **(c)** GCC may move x87 arithmetic across an inline `fldcw`. The float code needs its own
  non-inlined function.

**Backstops: overstated.** Decode-before-write runs the same C predictions, so it cannot see a C
against Python split. `tarec.py check` sees snapshots only, and misses a one-tick bank error that
no later rule reads back.

## 3. Numbers

Re-run, every iteration decoded back (MEASURED). MiB, writer; zstd 19 in brackets.

| run | units | decoded |
|---|---:|---|
| air war f1 | **1.334** (1.275), byte-identical to the report | 0 / 859,616 |
| air war, no bank rule | 1.696 (1.617) | 0 |
| air war, **all-integer** bank and turn | **1.338** (1.278), +3,419 B | 0 |
| T&C r0 / f1 | 4.524 / **4.034** (3.920) | 0 / 4,921,626 |
| T&C, cells only (no air rules) | 4.095 (3.979) | 0 |
| T&C, all-integer | 4.033 (3.918) | 0 |
| naval f1 | **0.206** (0.199) | 0 / 232,294 |

On Town & Country the cells give 88 % of the −0.490. The aircraft add −0.061 with guessed defs and
the air war's gravity.

**The bank rule** (`an/bankcheck.py`): the int64 Q31 version matches 588,451 of 588,452; the one
miss is off by 1. Under perturbation detmath misses 0 at relative error 9·10⁻¹³, 1 at 9·10⁻¹⁰ and
126 at 6·10⁻⁸.

**Fitting.** Gravity was fitted on the scored data, but sharply: 8155 gives 588,252, while 8154
and 8156 give about 503,000 (`an/amag.py`). The writer reads `main+0x14263`. Def `+0x170` was
fitted in-sample; the writer copies the byte.

## 4. What L1 must carry

The report's list stands. **Missing:**
- each type's air bit, as the authority for the group;
- the bounds and division of (a) and (b);
- LosType as an input to the sight rules;
- the landing-tick rule;
- the Q31 constants, if the integer rules are adopted.

Gravity goes in the stream header as a codec parameter, so `replay-globals.tsv` does not change.
Recording its `leave` row instead would remove a loosening (ordinary work) and is not needed. **No
loosening:** every rule is a predictor with a residual.

## 5. Alternatives

- **Integer bank and turn:** Q31 Taylor series after integer reduction, int64 floor shifts and
  division, every operand below 2^62; the turn divides by |A2|². Cost **+0.24 %** on the air war
  and −0.03 % on T&C. With no floating point in the format, no control word, compiler or numpy
  question is left. **Recommended.**
- **Dropping the bank rule:** +0.362 MiB (+27 %).
- **Dropping the turned candidate:** +0.045 (f2).
- **TA's 512-entry sine table:** too coarse (2^-13) [INFERRED].

## Owner questions

None: no loosening, no escalation. The integer format, the bounds and the gravity header are L1
spec work.
