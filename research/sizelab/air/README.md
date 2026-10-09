# Aircraft in the unit frame: the air step and the rules that predict it

MiB throughout; **writer** = each stream block's unit section deflated alone (zlib 6, 2 KB window),
as L0 measured it. Recording: `l0/air-war.imprec` (5,610 ticks, 601,724 aircraft unit-iteration
pairs, 588,452 of them airborne, about 105 aircraft on average). Every codec run below decoded its
bytes back: **0 mismatching iterations, 859,616 unit images compared** in each.

## Summary

| frame3 on the air war | units, writer | aircraft (lanes + their framing) |
|---|---:|---:|
| L0's frame2 (rules=1) | 6.224 | 6.05 |
| + the exact rules (speed, smoothed acceleration, bank, altitude, footprint, sight, claim stamp) | 2.166 | 2.04 |
| + the velocity estimate (the step's known part plus the last PD term) | **1.334** | **1.20** |

Every output of the step and the placement that does not depend on the course's target is
predicted **exactly** (588,452 of 588,452 each; sight z 91–100 %). The velocity's input is a target
point the record does not carry; the rules reduce it to one saturated direction a tick. Aircraft now cost about **0.22 MiB an hour each**, against 1.1 under L0 [INFERRED, scaled].

## 1. The air step (DISASSEMBLED)

The movement pass `0x43DD20` calls the mover's class step `[vt+8]`, then **`0x43D290` when def
`+0x241` bit 11 is set** (`0x43DD30..0x43DD46`; else the ground step `0x43CD20`), then the placement
`0x43D6D0`. An aircraft is that def bit.

The local air mover (vtable `0x4FD9B0`) steps through `0x490880` → **`0x490690`, the course**. The
air step reads three course outputs through `[vt+0x10]` = `0x490650`:

- a target point (course `+0xC`), taken from its manoeuvre object's `[vt+0x20]` (the vtables at
  `0x4FD328..0x4FD3F8` [INFERRED: all manoeuvres]; `0x44E3C0` aims at a target unit's piece through
  `0x43E060`);
- the target's change this tick (`+0x18`);
- a desired heading (`+0x24`): the manoeuvre's within 320 wu, else (or when it has none beyond
  16 wu) `0x48A980`, the bearing round(atan2(dx, dz) · K).

Beyond 160 wu the course sets the target altitude to (cruisealt def `+0x21C` + the bucket's height
byte, or the sea level under def `+0x241` bit 22) << 16 (`0x49071E..0x49076B`). **None of these is
in the record**: the recorder copies only the 47-byte outer mover.

| # | `0x43D290` | writes | arithmetic |
|---|---|---|---|
| 0 | layer ≠ 2 (`0x43D297..0x43D2BD`) | v, speed, turn ← 0; return | integer |
| 1 | damping (`0x43D2F1..0x43D38B`) | v ← (v · d) >> 16, d = 0x10000 − (acc << 16) / maxv (`_alldiv 0x4E4440`, `_allmul 0x4E4400`, `_allshr 0x4E43D0`) | integer |
| 2 | drift limit (`0x43D38E..0x43D47D`) | m = (float)(hypot(vx, vz) / 65536) (`_hypot 0x4FB440`); if m > brakerate: v_xz ← v_xz · trunc(brake / m · 65536) >> 16, then v_xz −= (sin, cos)(h) · trunc((m − brake) · 65536) by the table (`0x4B70EF`/`0x4B7123`), h before the turn | x87 hypot, fdiv, fsub; truncated |
| 3 | vertical (`0x43D4B8..0x43D502`; skipped in the off-map bucket) | vy ← clamp(target y − y, ±c), c = speed >> 2, at least 1 wu (0x10000 below 4 wu) | integer; hidden target |
| 4 | turn (`0x43D520..0x43D587`) | mover `+0x24` ← clamp(desired − h, ±turnrate def `+0x1BA`); heading `+0x66` += it; `+0x110` bit 16 | integer; hidden desired heading |
| 5 | PD (`0x43D505..0x43D65A`) | A = −w·d − (v − v_target), w = √(2·acc / max(dist, 8)); \|A\| limited to acc (second hypot); v_xz += trunc(A · 65536) | x87 fsqrt, fdiv, hypot, float32 stores; hidden target |
| 6 | speed (`0x43D65D..0x43D688`) | mover `+0x20` ← trunc(√(vx² + vy² + vz²)) | fsqrt of an exact sum |
| 7 | `0x43D0D0(unit, Δv)` (`0x43D0D7..0x43D159`) | mover `+0x14/+0x18/+0x1C` ← ((A_s · 0xF333) >> 16) + Δv | integer |
| 8 | `0x43D0D0` (`0x43D16A..0x43D1F6`) | `+0x64` ← round(atan2(−(x′ · bankscale) >> 16, q) · K); `+0x68` the same with pitchscale and **the same x′** (`0x43D1A0` and `0x43D1D5` read one local) | x87 fsincos (`0x4B7173`), fpatan (`0x4B715A`), fistp |

In row 8, x′ = round(A_s.x cos θ − A_s.z sin θ), with θ = h · 9.587379924285e-05 and the rotation
skipped at h = 0. q = (gravity `main+0x14263` << 16) / 0xCCD, and K = 10430.37835047.

**Def fields** (DISASSEMBLED in the FBI parser, `0x42C129..0x42C243` and `0x42C590..0x42C5AF`):

| field | def offset | note |
|---|---|---|
| maxvelocity | `+0x192` | |
| brakerate | `+0x19A` | |
| acceleration | `+0x19E` | |
| bankscale | `+0x1A2` | default 1.0 |
| pitchscale | `+0x1A6` | default 0 |
| turnrate | `+0x1BA` | |
| cruisealt | `+0x21C` | |

16.16 values are trunc(atof · 65536) (`0x4C4800`).

**Corrections to L0:** mover `+0x14/+0x18/+0x1C` is the smoothed acceleration; `+0x64`, called
pitch, is the **bank** (the lateral acceleration); the "thrust along the heading" is the excess over
brakerate redirected to the nose (row 2), and the acceleration added each tick is the PD term (row 5).

The placement then adds v to the position, y included (`0x43D80D`). It computes the footprint cell
(x − (fw << 19) + 0x80000) >> 20 (`0x43D86F..0x43D895`). A new cell or layer goes through the grid's
clear `0x47D0E0`, whose call to `0x440A70` stamps **mover `+0x26` with GameTime** (`0x440A91`).

The sight emit `0x4825B0` runs every tick (`0x465046`). It stores `+0x7A` = x >> 21 and `+0x7C` =
(z − h/2) >> 21 (`0x4825D4..0x48261C`), where h = clamp(the altitude raised to the sea level + 1,
plus def `+0x170`, to 0..255).

**Modes are one step with different targets.** Circling, attack runs and hovering differ only in
the manoeuvre's point. Landing and take-off are `0x43D210` from the order handlers (to layer 1: v and
speed zeroed, `0x43D0D0` with Δv = 0); a landed unit takes row 0 every tick. A dying aircraft carries
the pending-death bit `+0x110` bit 14 in its last iteration (98 of 104 deaths) and is gone the next
[MEASURED]: the unit record has no crash flight.

## 2. Exactness and the control word

**The control word** (DISASSEMBLED):

- TA's C runtime sets precision 53 at start-up: `0x4E4320` → `0x4EA5B0` = `_controlfp(_PC_53,
  _MCW_PC)`; `_fpreset` (`0x4E8D00`) does the same.
- The image's 31 `fldcw` all lie at `0x4E43B6` and above, in runtime functions that set a temporary
  word and restore it: `_ftol` (`0x4E43A0`) truncates this way, and `_hypot` runs under
  `_ctrlfp(0x133F)`, precision 64 (the sites checked: `0x4EC782`, `0x4ECA36`, `0x4F4C8A`, `0x4F553F`).
- The game calls `_controlfp` only to change exception masks (`0x4CF3F3..0x4CF423`, `0x4DF1B1`), and
  Impure's DLL source has no `fldcw` of its own.

MEASURED: speed = isqrt holds on all 588,452; precision 24 would round the 40-bit squares. All
588,452 banks round to nearest. So the game thread runs at precision ≥ 53, rounding to nearest.

**Bit-exactness is not what losslessness needs.** A rule that misses the engine costs a residual.
Only a rule that differs between **writer and reader** decodes wrong values. So each rule is a
function the format defines, and how often it matches the engine is a question of size:

| rule | engine arithmetic | the format's function | engine = format |
|---|---|---|---|
| speed, A_s, altitude, footprint, sight x, claim stamp | integer (speed: fsqrt of an exact sum) | int64 | 588,452 / 588,452 each |
| known part (rows 1–2) | integer, `_hypot`, float32 | **int64 only**: √ rounded to 24 bits exactly, then exact rationals (`lib/knownint.py`) | equals the binary64/binary32 version on all 601,724 × 2 inputs; `_hypot` = √(x² + z²) on all |
| bank, `+0x68` | fsincos, fpatan, fistp | **own polynomials** in binary64 (+ − × ÷ √ only, in a fixed order; `lib/detmath.py`) | 588,452 / 588,452 |
| velocity estimate | hidden input | int64; the turned candidate in binary64 | a prediction, see §3 |

**(a) The 32-bit writer and the C reader.** The DLL is built `-march=i486`: x87 only. At precision
53, round to nearest, the x87 gives IEEE binary64 results for + − × ÷ √ away from overflow and
underflow. So the polynomial module sets the control word to `0x027F` on entry and restores it on
exit (an ordering, not a hope about the thread's state), and uses no libm: msvcrt's `sin` and
`atan2` are x87 transcendentals. **(b) The Python reader** uses numpy float64, the same constants
in the same order; numpy does not fuse multiply-adds.

MEASURED margins: `detmath` equals numpy's libm on all 588,452 banks; scaling sin, cos and atan2 by
up to 4,096 ulp changes none; the nearest values lie 1.1·10⁻⁶ (x′) and 1.0·10⁻⁵ (bank) from a
rounding boundary; float32 evaluation would miss 51.

## 3. Measured

Rule exactness per mode, fed the true previous values (`lib/modes.py`). The modes are [INFERRED
from the record]: landed = layer ≠ 2, hover = speed below 1 wu/tick, turning = turn rate ≠ 0.

| rule | landed 13,272 | take-off/landing 200 | hover 1,738 | turning 179,298 | straight 407,216 | climbing 96,946 |
|---|---:|---:|---:|---:|---:|---:|
| speed, A_s ×3, bank, altitude, footprint, sight x, claim stamp | 100 % | 100 % | 100 % | 100 % | 100 % | 100 % |
| sight z (def `+0x170` fitted per type) | 100 % | 100 % | 87.6 % | 90.3 % | 95.5 % | 98.9 % |
| vy: continue a saturated climb, else level | 100 % | 0 % | 99.8 % | 96.7 % | 95.5 % | 74.9 % |
| v: known part + last PD term | 100 % | 100 % | 12.9 % | 15.0 % | 65.6 % | 62.6 % |
| v: that, or the term turned by its last change | 100 % | 100 % | 40.2 % | 27.6 % | 70.3 % | 66.8 % |

The PD term saturates (\|A\| = acceleration) on 96 % of airborne iterations and is 0 on 3.4 %. It
never exceeds the acceleration beyond truncation on any of 588,252 iterations, which checks rows 1–2.

The ladder (`runs/full/*/run.out`; each run decoded back):

| run | rules added to frame2's | writer | zstd 19 | lzma |
|---|---|---:|---:|---:|
| r0 | none (= L0) | 6.224 | 5.911 | 5.462 |
| r1 | speed, A_s, bank, altitude | 3.029 | 2.873 | 2.693 |
| f4 | r1 + footprint, sight, claim stamp (all exact) | 2.166 | 2.094 | 1.986 |
| f6 | r1 + the velocity estimate, no cell rules | 2.226 | 2.074 | 1.974 |
| f2 | f4 + the velocity estimate (last PD term; vz completes \|A\|) | 1.379 | 1.319 | 1.277 |
| **f1** | f2 + the turned term as a fourth candidate on vx, vz | **1.334** | **1.275** | **1.234** |

Two variants did not help: replacing the last term by the turned one was worse (2.452 against 2.334,
r3/r2), and keeping sight z's old cell north of the map gave 1.336 (f1b).

**On L0's other recordings**, the same f1 rules decoded back exactly:

| recording | unit images | before (L0) | after (f1) | change |
|---|---:|---:|---:|---:|
| Town & Country | 4,921,626 | 4.524 | 4.034 | −10.8 % |
| naval | 232,294 | 0.215 | 0.206 | −4.1 % |

On Town & Country the gain is the ground units': footprint, sight and the claim stamp. Its 12
aircraft were assumed to be the air war's ARMFIG and CORVAMP defs. Their ids there are 51 and 272,
not 271 [INFERRED match].

## 4. What the rules cannot fix

After f1 the aircraft cost 1.20 MiB, at writer scale:

| part | MiB |
|---|---:|
| vx | 0.14 |
| vz | 0.10 |
| vy | 0.10 |
| turn rate | 0.07 |
| sort-bucket `+0x82`, its chain `+0x8E`, `+0x110` bits 26–27 | 0.18 |
| other lanes | 0.12 |
| framing (the gaps and masks of listed aircraft) | 0.56 |

The velocity's information is a **hidden input**, the course's target point and its change; every
other input of the step is in the record. What remains is mostly that one saturated direction a tick
and the framing of the units it lists. It is not noise: in straight flight the last term repeats on
66 % of iterations. Two ways further, both unmeasured: **a per-unit target estimate** (the turn gives
the desired heading exactly whenever it is not saturated, 86 % of iterations, and beyond 320 wu that
is the bearing to the target), or **recording the course's point, change and heading** (26 bytes an
aircraft, with their own cost). `+0x82` is (z >> 23) · 84 + (x >> 23) + 1 on 92 % [MEASURED]; the
grid bookkeeping is not modelled here. Leaving §4 as it is costs the 0.22 MiB per aircraft-hour:
about three ground units at the battle's tail (0.07 each, L0).

## 5. Recommended predictor set for L1

The air group is the types with def `+0x241` bit 11. Per-unit switching works as in frame2, with a
fourth candidate on the aircraft's vx and vz. Decode order, per row: generic lanes; heading; **for
aircraft vx, vz, vy, then speed, A_s, bank, `+0x68`** (for other units speed stays generic, before
the ground velocity rule); position, altitude (aircraft); footprint, sight, claim stamp, move stamp,
countdowns.

| lane | rule (the format's function) | arithmetic |
|---|---|---|
| speed `+0x20` | isqrt(v · v); 0 off layer 2 | int64, exact |
| A_s `+0x14/+0x18/+0x1C` | ((A_s · 0xF333) >> 16) + Δv; unchanged off layer 2 | int64, exact |
| altitude `+0x6E` | y + vy | int64, exact |
| footprint `+0x76/+0x78` (all movers) | (p − (fw << 19) + 0x80000) >> 20 | int64, exact |
| sight `+0x7A/+0x7C` | x >> 21; (z − h/2) >> 21 | int64, exact / 91–100 % |
| claim stamp mover `+0x26` (all movers) | GameTime − 1 when the footprint or layer changed | int64, exact |
| vx, vz | rows 1–2 + the last PD term (candidate 0) or that term turned by its last change (candidate 3); vz alternative completing \|A\| = acceleration; 0 off layer 2 | int64; the turn in binary64 |
| vy | continue a saturated climb at the new limit, else 0 | int64 |
| bank `+0x64`, `+0x68` | row 8 | binary64 polynomials under `0x027F` |

| the format needs | |
|---|---|
| per recording | gravity (`main+0x14263`, a `leave` row today), the sea level |
| per aircraft type (ids differ between recordings) | maxvelocity, brakerate, acceleration, bankscale, pitchscale, def `+0x170`: about 24 bytes, from the defs |
| per slot | a third image and an age, reset at a snapshot like L2 |
| format-version constants | the sine table, 0xF333, 0xCCD, the doubles K and 9.587379924285e-05 bit for bit, the polynomial coefficients as hex |

## 6. Risks

- **Writer/reader agreement is the one correctness risk.** The polynomial module must run under
  its own control word in C and in float64 in Python. The writer's decode of its own block and
  `tarec.py check` at each snapshot are the backstops, as for the ground rules.
- Int64 headroom: values read from records are clipped before they are squared, as a format rule.
- Not measured: aircraft that set pitchscale, air transports and their cargo (the carried branch of
  `0x43D6D0`). Landing and take-off cover only 200 iterations. `frame3.py` calls numpy's libm for
  the bank; `detmath` gives the same 588,452 values.
- Sight z's def `+0x170` was fitted: exact for ARM, 91–97 % for CORE, whose misses all have a
  negative cell (north of the map's edge). The writer copies the def byte instead.
- Gravity was fitted: 8,155 = 112/900 · 65,536 [INFERRED]. The writer reads it.

## Files

`lib/`: the codec `frame3.py`, `run3.py`, `runall3.sh`, `runall3b.sh`, `env.sh`, `airdefs.py`; the
extraction `airseries.py`, `airpairs.py`; the analyses `rules_air.py`, `bankfit.py`, `airstep.py`,
`knownint.py`, `detmath.py`, `modes.py`, `attrib.py`. `runs/{full,tc,naval}/*`: each run's `run.out`
and lane attribution (r3 and r5–r7 ran an earlier sight-z rule, superseded). `fbi/`: the FBIs read
(local only). `lib/frame3_v1.py` is the code the r-series ran.
