# L0 / units: the lossless unit frame (`frame2.py`) on three more recordings

"MiB" throughout; **writer** = each stream block's unit section deflated alone (zlib 6, 2 KB window), as
recsize attributes `units: deltas`. All numbers MEASURED unless marked.

## How it was run

The committed scripts are unchanged. They ran through `lib/runu.py`, which puts `lib/` ahead of
`research/sizelab/units/` on `sys.path`. The one copy is `lib/sim.py`, with `SLOTS = 2501` replaced
by `$SIZELAB_SLOTS` (5001). All three recordings have **5001 unit slots**, so the committed `sim.py`
cannot hold them.

Per recording (`battle/`, `naval/`, `air/`):
- `extract.py` and `check_sim.py`;
- `run.py baseline`;
- `run2.py` with rules=0, the ladder `ruleset=stamp,counts` → `+heading` → `+velocity`, and rules=1.
  Each run decodes its bytes back and compares every live unit at every iteration (verify=1).

New analysis scripts:
- `lib/classstats.py` gives the rule exactness per class. It calls `frame2.Rules.candidate` itself.
- `lib/lanecost.py` is `lanestats.py` with a split by class.
- `lib/newlanes.py` lists the lanes that change here but never changed in size-battle. It reads
  `ref/`, a re-extraction of `size-battle.imprec`.
- `lib/airprobe.py` probes the aircraft velocity.

**Unit class** is decided per type (`+0xA6`) from behaviour [INFERRED], not from the def's flags:
- *structure*: `+0x110` bit 29 (no mover);
- *air*: a unit of the type was on move-object layer 2;
- *floater*: a mover whose type's altitude is a single value over the whole file;
- *ground*: every other mover.

On naval-push the six floater types match the scenario's ship counts exactly (30/20/50 per side).
Type `34` (one unit, constant y; probably an idle commander) also lands there; negligible.

## 1. Lossless and the snapshot check

| recording | iterations | unit-iterations decoded | mismatching iterations (all 5 frame2 runs) | stream ⇄ snapshot images |
|---|---:|---:|---:|---|
| battle (Town & Country) | 4,021 | 4,921,626 | **0** | 4/4 EQUAL |
| naval-push | 3,841 | 232,294 | **0** | 4/4 EQUAL |
| air-war | 5,610 | 859,616 | **0** | 6/6 EQUAL |

No mismatch to explain. The replayed stream also equals every snapshot's unit images and slot set.

## 2. Size (writer, MiB)

| | size-battle (ref) | battle | naval-push | air-war |
|---|---:|---:|---:|---:|
| today's `units: deltas` | 46.68 | 52.72 | 2.52 | 24.95 |
| frame2 generic (rules=0) | 9.84 (4.7×) | 12.01 (4.4×) | 0.405 (6.2×) | 8.14 (3.1×) |
| + stamp, countdowns | 9.65 | 11.96 | 0.391 | 8.12 |
| + heading ← turn rate | 8.97 | 11.02 | 0.377 | 7.95 |
| + velocity ← sine × speed | 6.32 | 7.31 | 0.283 | 7.90 |
| **+ position ← velocity (best)** | **4.50 (10.4×)** | **4.52 (11.7×)** | **0.215 (11.7×)** | **6.22 (4.0×)** |
| best, zstd 19 / lzma | 4.35 / 4.13 | 4.38 / 4.08 | 0.207 / 0.202 | 5.91 / 5.46 |
| saving, % of the file | 42 % | 60.9 % | 22.8 % | 40.5 % |

Ground and naval generalize at or above the battle's ratio. **Aircraft do not**: air-war reaches
4.0×, and the velocity rule there is worth only 0.05 MiB.

## 3. Assumptions: the rules per class

Each rule is scored as exact over all unit-iterations live at t−1 and t, and separately over the
iterations where the lane changed. Ground, floater and air count movers; the countdowns count every
unit.

| rule | ground (battle) | ships (naval) | aircraft (air-war) | aircraft (battle, 12) |
|---|---|---|---|---|
| heading ← turn rate | 100 % (1.72 M changes) | 100 % | **100 %** (181 k changes) | 100 % |
| v ← −(sin, cos)·speed | 100 % | 100 % | **5.0 % / 2.6 %** (vx / vz) | 20 % / 11 % |
| x, z ← + v, free | 100 % | 100 % | **100 %** | 100 % |
| x, z ← + v, blocked bit set | 46 % / 45 % (lin 89 %) | 64 % / 77 % (lin 99 / 91 %) | — (never blocked) | — |
| y ← + vy (not a frame2 rule) | 0 % of 105 k changes (vy = 0) | constant | **99.97 %** of 97 k changes | 99.97 % |
| stamp ← GameTime−1 if moved | 100 % | 100 % | 100 % (588 k changes) | 100 % |
| reload countdowns (slots 1–3) | 99.8 % | 99.5 % | 99.8–99.96 % | 99.7 % |
| `+0xFA` countdown | 99.9 % | 98.3 % | 99.9 % | 99.4 % |

**The sticky blocked bit is common at sea.** It is set on 87 % of ship iterations (180 k of 208 k),
against 40 % for battle ground. Per-unit switching falls back to lin there. The fallback costs
nothing visible: naval's ratio is the best of the four.

**Aircraft velocity is not the ground rule.** DISASSEMBLED, partly read: the air step `0x43D2C0..`
does four things:
- damps vx, vy and vz by `0x10000 −` a factor from def `+0x192`/`+0x19E`, through the 64-bit helpers (`0x4E4400`/`0x4E43D0`);
- compares the horizontal magnitude (`0x4FB440` on vz and vx, as x87 floats) against def `+0x19A`
  and rescales;
- *adds* a thrust along the heading through the same sine helpers (`0x4B70EF`/`0x4B7123`);
- sets vy from the altitude error, capped at speed/4 (at least 1 wu) (`0x43D4D5..0x43D502`).

`airprobe.py` finds no simple replacement: |v_xz| is within 1 of the speed in 84 %, and v(t) = v(t−1)
+ (`+0x14..+0x1C`) holds in 5 %. An exact air-step rule would need the x87 arithmetic reproduced
bit for bit [INFERRED: feasible but riskier than the integer ground rules].

## Fields that change here and never changed in size-battle

| recording, class | lane (share of that class's unit-iterations that change it) |
|---|---|
| battle, air (10.9 k) | mover `+0x18` 90 %, `+0x14` 82 %, `+0x1C` 80 %, **pitch 66 %**, **vy 8.9 %**, roll 8 changes |
| air-war, air (602 k) | mover `+0x1C` 92 %, `+0x14` 92 %, `+0x18` 75 %, **pitch 66 %**, **vy 13.5 %**, w2 reload 3.5 %, `+0xC8` 1.7 %, w2 aim h/p 1.7 %, roll 108 changes |
| air-war, ground / structure | `+0xC8` 0.5 % / 0.1 % |
| naval, ships (208 k) | **w2 reload 38.5 %**, `+0x3B` 4.2 %, w2 aim h/p 1.4 / 1.1 %, `+0x28` 0.9 % |

**Pitch and roll never changed on a ground unit**, hills included. On hilly Town & Country, 4.78 M
ground unit-iterations change pitch 0 times and roll 0 times. Altitude changes in 2.2 % of them, the
same rate as flat Core Prime (2.2 %), at 0.05 MiB. [INFERRED] Every ground type in the battle is a
kbot (ARMPW, ARMHAM, ARMROCK, ARMJETH, CORAK, CORTHUD, CORSTORM, CORCRASH, the CKs). **Tracked
vehicles on slopes are still untested.**

Mover `+0x14..+0x1C` are aircraft-only and not identified [INFERRED: air-step state].

**What aircraft cost** (`air/lanecost.out`, deflated per column):

| aircraft lanes | MiB of air-war's 6.15 |
|---|---:|
| speed | 0.62 |
| mover `+0x14` / `+0x18` / `+0x1C` | 0.54 / 0.52 / 0.54 |
| vx / vz | 0.52 / 0.50 |
| pitch | 0.26 |
| vy | 0.18 |
| altitude | 0.16 |
| mover `+0x26` | 0.11 |
| **total** | **≈ 4.0** |

There is no dead reckoning for aircraft: 586 k of 602 k aircraft iterations are listed.

**What the rules miss, and what it costs:**
- **velocity**: 1.0 MiB here; an air-step rule is the fix [INFERRED].
- **altitude**: up to 0.16 MiB. Adding *y ← +vy* as a rule recovers most of it [INFERRED from the
  99.97 % exactness].

The other ~2.6 MiB of aircraft lanes have no known rule.

## 4. Cost

- **Encoder search does not grow**: three fixed candidates per lane.
- **Mask dictionary** grows with lane variety. On air-war it is 0.87 MiB plus 0.72 MiB of ids, against
  0.36 + 0.89 MiB on the battle.
- **Memory** is per *slot*, not per live unit. It doubles at 5001 slots: [INFERRED] ≈ 3.8 MB of
  predictor state on each side, against README's 1.9 MB.
- **Prototype time** (Python, 18 jobs on a loaded machine) is about 700–850 s per run for every file,
  naval's 60 units included. That per-iteration work over all 5001 slots is a prototype artefact
  [INFERRED: a C writer walks live slots]. Peak RSS is 1.9 GB.

## Files

`L0/units/{battle,naval,air,ref}/`: `*.out`, `results.jsonl`, `classstats.{out,json}`,
`lanecost.{out,json}`, `recsize.txt`, `units.npz` (cache, 520 MB total); `lib/` holds the scripts.
