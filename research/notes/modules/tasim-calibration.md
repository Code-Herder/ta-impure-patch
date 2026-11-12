# The calibration runner

How tasim is graded against the engine: scenarios played many times in the engine, their metrics
kept as references, and an offline check that plays the same runs in tasim and grades each metric
by the [calibration bands](../impai/calibration.html). The code is `tools/tasim/` (Python) and
`tagpu/ddraw/src/tasim_roles.c` (the role classifier, C, in the shared core).

*A module note: it states the module as it is now. A landing that changes the module edits its
section here in place; what the landing measured and what its review found go to the commit and
the gate's plan note.*

| Command | What it does | Plays games |
|---|---|---|
| `tools/tasim/calib.py engine --scenarios GLOB [--runs N] [--j J] [--mode M]` | the engine phase: lanes, runs, manifests and recordings; M is `reload` (the default: run after run in one level) or `fresh` (a launch a run) | yes, under the run lock |
| `calib.py engine --probe-pause` | one duel ended by the pause's units clause, timed against its recording | yes |
| `calib.py refs build RUNS_DIR` | the recordings read, checked R1..R8, merged into references | no |
| `calib.py check --tasim PATH [--only GLOB] [--require-fresh] [--json OUT]` | tasim graded against the references: the scoreboard | no |
| `calib.py selftest [--against REF]` | split-half, seat effect, exact self-consistency, reload against fresh | no |
| `calib.py stale [--since REV] [--json]` | the references the code has made stale | no |
| `tools/tasim/arenascan.py scan` / `pick [MAP]` / `check` | every map the install resolves graded for the arena; the arena config and scenario written from the best; the configured arena re-derived | no |
| `tools/tasim/duelgen.py [--dry-run]` | the duels and movement courses from the install's roles | no |
| `tools/tasim/native.py`, `native.py test`, `native.py roles` | `tasim_run` built with the host's gcc; the classifier's test under ASan and UBSan; `tasim_run roles` on the install | no |

## The arena

Every run plays on one map, the one `tools/tasim/calib-arena.json` names. Nothing in it is written
by hand: `arenascan.py pick` grades every map the install resolves (`tavfs`, the engine's mount
order) and writes the config and the arena scenario's map from the best; `arenascan.py check`
re-derives the configured map before every `calib engine` and refuses one the install no longer
gives. A map qualifies when:

- **its meteor shower is off in every schema, by the engine's own rule.** The `.ota` reader switches
  the shower on (`meteoron` `0x51232C`, through `0x437D40`) only when `MeteorWeapon` is not empty and
  `MeteorRadius`, `MeteorDensity`, `MeteorDuration` and `MeteorInterval` are all non-zero
  (`0x436759..0x4367EE`). With the switch off the shower tick still schedules showers but clears
  their active flag, so no stone falls ([engine/weapons.md](../engine/weapons.md), *The meteor
  shower on two peers*). A stone is a hit with no attacker at a point drawn from the CRT's `rand`,
  which the draw path shares: no run could be independent of the frame rate. Every lane reads the
  switch itself at its launch (`engrun.py`, a peek of `0x51232C`) and refuses a level where it is set;
- schema 0 has two start positions, the arena's two seats;
- its TNT holds a dry rectangle of ONE height (the TNT's height samples, one per 16 world units, all
  equal and above the sea level), 8 samples in from every edge, at least 1 600 world units across;
  the longest such, then the largest. The duel line runs along its long side through the middle, `a`
  and `b` 256 in from its ends, and one course lane per land move class at each fifth across. Every
  apply's result gives each unit's actual altitude, which a flat placement makes equal.

On the retail install (275 maps): 4 qualify, 156 are single-player missions with one start
position, 87 have no such rectangle and 28 have the shower on -- Slate Gordon among them
(`MeteorWeapon=earthquake` in Schema 0). The arena is Cavedog Links CC, `[4544, 384, 5584, 1600]`
at height 85 (sea level 75); the next is Moon Quartet at 2 592 long. The arena's scenario
(`scenarios/calib/arena.json`) loads the map with the objective off, seat 1 an AI seat with its AI
off, and each seat's base storage at 1 000 000. Line of sight and mapping default to the
skirmish's True and Mapped [INFERRED: the skirmish screen's defaults], and a reference is keyed by
both.

## The lane and its runs

A **lane** is one tareplay worker: a private instance on a private display, turbo k = 1, held at
launch, recording the exact stream. A **lane-launch** loads the arena and plays run after run in
the same level, each from an **empty point**; it relaunches when the setup key changes (map, line
of sight, mapping, unit limit, game speed, reload or fresh) or after `--runs-per-launch` runs. The
lanes are threads under one run lock (`tools/runlock.py`).

One run (`tools/tasim/engrun.py`, `Lane.run`):

1. **Settle**: hold at the least T ≥ T_prev + 60 with T ≡ 1 (mod 30), raised by 30 while the
   projectile count `main+0x141F3` is not 0, never past T_prev + 1800. Sixty ticks clear B4's slot
   hold, which keeps a freed slot two ticks; the phase puts every seat's 30-tick economy pass at
   the tick of a fresh launch's.
2. **Apply** the run's wire on the held frame (`tagpu_scenario.c`; the applier's tick site runs on
   paused frames) and check the result: GameTime T, nothing cleared, as many delayed orders as the
   wire asks.
3. **Arm** `pause at <T + limit> or units <s> <= <n> …` and resume; the pause gives the end tick
   and its reason.
4. **Clear** on the held frame, keeping `cleared_slots`: the survivors, whose death images are
   their state at the end of the last step.
5. **The delayed orders' line**: a skipped tick makes the run unusable.
6. One **manifest** row.

A lane's first empty point is dumped (`e0`), and after its last run and a settle of 1800 ticks a
second (`eN`); its end applies `scenarios/calib/end.json` (the objective armed, seat 0 without a
unit) so the recording closes complete. A lane plays no recording, so no create passes a recorded
slot and the delayed orders' watch holds (the [allocator](../engine/network.html), *The allocator*);
each launch asserts it from its log. Every wait bounds a dead or hung game (`ACK_S`, `STALL_S`)
and decides "could not run", never an outcome; a run ends on the engine's unit count or its tick
limit.

## What a reload does not reset

| State | Reset by the next run | Why no metric is biased |
|---|---|---|
| units, features | the clear and `clear_features` in every wire | — |
| projectiles | the settle waits for none | — |
| levels and storage | every wire's `storage` and levels | — |
| lifetime totals `+0xAC..+0xDB`, kills `+0xFC..+0xFF`, `+0x104..+0x107`, units made `+0x140` | no | metrics use differences from the apply |
| deadlines `+0xF0`, `+0xF4`, `+0xF8` | their phase, by T ≡ 1 (mod 30) | a fresh run's phase |
| GameTime | no | the reload-against-fresh self-test is the evidence |
| the simulation RNG `0x51FC88`, the wind | no | random from launch to launch too; each run records them as `exogenous` |
| the explored map | no | its only simulation reader is reclaim's mode 12, R5 |
| B4's slot table | no | its only skirmish reader treats every slot freed before the last tick alike |

## The per-run checks

`tools/tasim/engtrace.py` reads each lane's recording once and checks every run; a failed check
makes the run unusable and the reference builder refuses its scenario.

| | Check |
|---|---|
| R1 | no live unit at the empty point; the apply's creates are exactly the result's units (slot = ID − 1, type, owner, position); each cleared slot dies once in the clear's iteration; a scenario's slots are the same in every run of a launch |
| R2 | no projectile at the empty point |
| R3 | after the apply the grid holds only the scenario's features |
| R4 | each seat's image after the apply equals run 0's, but the lifetime totals and the deadlines' phase; the storage, flag and levels equal the wire's |
| R5 | the explored map equals run 0's; a difference fails only a scenario that reclaims or captures |
| R6 | the globals and the declared tables equal run 0's but `EMPTY_POINT_ADVANCING`: the RNG, the wind rows `main+0x37EC4..+0x37EE5`, B4's slot table. Any other field is named. **Adding to the set loosens R6: the owner's decision** |
| R7 | every seat's sight counters zero in `e0` and `eN` |
| R8 | each delayed order changes its subject's list in iteration T + after + 1 (or the report cancelled it), and no create lands in a watched slot before |

**The iteration of an event made on a paused frame.** The recorder's capture point publishes the
last step's events and then writes the new tick's `RK_TICK` and images (`tagpu_rec_capture`), and
an event goes into the ring when the engine makes it. So the apply's creates and the clear's
deaths, made on the frames held after step T, come in iteration T after step T's events, and the
images of T + 1 are the first to show them [from the source; C1's recordings are to confirm it].

## Metrics

`tools/tasim/metrics.py` is the one implementation; engine and tasim traces go through it.

**The trace**, `ta-calib-trace/1`, one JSON line a tick with something in it: a head (`t0`, the
limit, the units by handle with their side, seat and destination), then `{"t", "u", "ev", "p",
"l"}`. A line's `t` is GameTime less the apply tick; its rows are the state at the end of step
t0 + t (t = 0: as applied), its events what that step did. `u` is `[x, altitude, depth, hp, nano
fraction left]` raw (16.16, a short, a float), `p` a side's `[e, m, e income, m income, e max,
m max, e produced, m produced]` at each economy pass, `l` a unit's eight ledger floats
(`+0xBC..+0xC8`, `+0xD4..+0xE0`) in exact scenarios. Side A is the scenario's owner 0 whatever seat
plays it. A unit made during a run is `built:<n>`.

| Metric | Definition |
|---|---|
| `winner` | at t_e = min(elimination, limit), the side with the larger HV(t_e)/HV(0); equal is a draw (0.5). hv = (metal + energy / R) × hp / maxdamage × (1 − nano left) |
| `ttk` | the first t at which the loser's HV ≤ 10 % of its start (a draw: either side's); censored at the limit |
| `survivors` | the winner's HV(t_e)/HV(0); a draw takes side A's |
| `income.<res>@c` | a side's produced(c) − produced(0), c a multiple of 1800 |
| `travel.first/median/last` | the tick each mover first comes within 16·(fp + 1)·⌈√n⌉ of its destination; censored at the limit |
| exact families | `damage` (each HP decrement), `reload` (each shot), `range` (each target's first shot), `build_time`, `cost_per_tick`, `extractor`, `storage`, `sight` (the cells a seat's counters hold in the run's dump at t = 2) |

## Perturbations

`tools/tasim/perturb.py` makes every run's wire with tacli's own compiler, from xorshift32 seeded
with the first four bytes of sha256("<scenario sha256>:<index>"): a mobile unit's position moved by
−8..7 world units on each axis, then each order's `after` raised by 0..15 ticks; odd runs swap the
seats. An exact scenario is never perturbed. A reference records each run's perturbation and its
wire's sha256, and the checker refuses one that `perturb.py` no longer makes.

## References

Untracked, under `<main checkout>/.data/calib/` (or `$TACALIB_DIR`), because they carry the
install's costs and HP: `refs/<stem>@<sha12>/<commit12>.json` (`ta-calib-ref/1`), and an exact
scenario's engine traces beside it (`.traces.jsonl.zst`); `runs/<stamp>/lane<k>-<launch>/` each
lane-launch's manifest, log, dumps and recording (deleted once its references are built, unless
`--keep-recordings`); `scenarios/gen/` duelgen's output. A reference names its DLL's build and
commit (a dirty build is refused), the install's fingerprint (the types' keys and CRCs, the weapon
and feature-def fingerprints), the setup, the values, `perturb.py`'s sha256, every run with its
metrics, checks and exogenous values, and the engine time it cost.

**Staleness** (`calib.py stale`): a reference made at commit C is stale when C..HEAD changes a sim
file's code lines (tareplay's own reading: comments and white space are not code), a header one
includes, `tagpu_scenario.c`, `tools/tacli`, `perturb.py`, `engrun.py`, the arena config or the
arena's scenario, or the reference's own scenario. `tareplay.py landing` counts the references a gated landing made stale and records it
as `calibration_stale=` in its note; re-measuring is a command, never part of a landing.

## The check

`tools/tasim/check.py`, against each scenario's newest reference:

1. **win rate**: |p_t − p_e| ≤ 2·SE, SE = √(p̄(1 − p̄)(1/n_e + 1/n_t)), equality when SE = 0;
2. **distribution**: P10_e ≤ P50_t ≤ P90_e and 0.5 ≤ spread_t/spread_e ≤ 2 (numpy's linear
   quantiles; a censored value enters as its limit; a zero engine spread needs P90_t = P10_t =
   P50_e);
3. **exact**: each metric's series equal at every tick, no tolerance;
4. **predictor**: on the decisive scenarios (p_e > 0.8 or < 0.2) the call agrees with the engine's
   majority on at least 90 % (the rule is in; the call needs `tasim_run predict`, A2).

Exit 0 every metric in band, 1 one out (each named), 2 could not run (no reference, a dirty or
malformed one, a stale one under `--require-fresh`, a tasim failure, a perturbation mismatch). The
scoreboard groups by `calib.area` less its last part; `--json` writes `ta-calib-score/1`.
**Self-tests**: H2 = {i mod 4 ∈ {2, 3}} graded against H1 with the false-alarm rate per rule
(about 4.6 % expected for the win rate), the swapped runs against the unswapped (the seat effect),
an exact scenario's runs 0, 1 and 2 (fresh) equal, and `--against` a fresh reference.

### The tasim_run interface

`tasim_run play --setup F --wire F --run-seed S --exogenous F --limit L --end "0:0,1:0" --trace F
--json` plays one run and writes its trace; one JSON object on stdout, exit 0 when it ran and 2
when it could not. It lands in T1; until then `tools/tasim/tests/fake_tasim.py` stands in, writing
traces from a table, for the checker's tests.

## Roles

`tasim_roles.c` classifies every unit type from its def's fields, the one implementation for the
duel generator (through `tasim_run roles`) and for ImpAI's adapter (an A-landing). Pure: no globals,
the caller's scratch space, SSE2 in the DLL (the `Makefile` rule beside the wiki's).

- **Value** = metal + energy / R, R = `TASIM_ROLES_R` = 60, the retail metal maker's 60 energy for
  1 metal (owner's decision: one named constant; `roles` reports the mod's own maker as `r_maker`).
- **Layer group**: air if it flies; sea if it neither hovers nor is amphibious and needs water; land.
- **Anti-air slot**: its bad-target category (`wpri_`/`wsec_`/`wspe_badTargetCategory`, def
  `+0x231/+0x235/+0x239`) is NOTAIR, or its weapon is a toairweapon; its damage is the weapon's
  [DAMAGE] averaged over the aircraft types (category VTOL). Ground dps and range are the other
  regular slots' (regular: not commandfire, stockpile or interceptor).
- **Thresholds**: percentiles (numpy's linear rule) over the layer group's mobile non-builders.
- **Precedence**: builder (flags commander, factory = static) · defence (static, armed) · other
  (static unarmed, an unarmed transport, kamikaze, only stockpile or interceptor weapons) ·
  antiair (the primary regular slot anti-air, hurting aircraft) · scout (no regular weapon) ·
  artillery (ballistic or vertical-launch ground weapon, ground range ≥ P75) · scout (speed ≥ P75,
  value ≤ P10) · raider (speed ≥ P50) · assault.
- **Tech**: max(1, the least number of factories before the unit on a build chain from a
  commander); 0 when no chain reaches it.

`tasim_run roles` (`tools/tasim/tasim_run.c`) fills the input from the install's files with the
engine's conversions (reloadtime × 30, speeds × 65536) and takes build options by the sidebar's page
rule (`tagpu_buildmenu`) over the GUI pages and download entries; the engine's page count
(`+0x22E`) is not read from a file, so the pages are those that exist or that an entry names
[INFERRED]. On the retail install (2.0 s): land 20 defence, 15 assault, 22 raider, 10 artillery,
15 scout, 6 antiair, 35 builder, 63 other; ARMPW tech 1, ARMZEUS 2, CORKROG 3. The adapter's
parity test against the live def table belongs to the A-landing that adds it.

**Duels** (`duelgen.py`): the land roles raider, assault, artillery, scout and defence in unordered
pairs (not defence against defence), per tech level and at three ranks of value, A at the arena's
`a` and B at the longer ground range + 160 (a defence always B, at its own range + 160), each
attacking the other; dropped when neither can hurt the other, refused when B leaves the flat area or
the shots' cost over the limit passes half the storage. **Courses**: eight of each land move class's
cheapest mobile non-builder moved across the arena. Retail: 64 duels, 3 courses (kbot, tank, hover),
4 refused: ARMVULC fits the arena (3 240 world units apart) but its shots over the 5 400-tick limit
cost more than half of each seat's 1 000 000 storage.

## Speed

Not measured yet: the engine runs of C1's verification are the runner's. Until then `--j` defaults
to 6 and `--runs-per-launch` to 200, and the cost a reference records (`cost.lane_seconds`, `ticks`)
is what each scenario took. A wait reads the log every 20 ms (`CalibLog.NAP_S`), since a run waits
on one line per verb.

## Gaps

- The engine verifications are not run: the `calib-applier` fixture, the units clause's probe, the
  duel at `-j` 6, 12 and 1, reload against fresh, R1..R8 on recordings, the exact scenario
  (`scenarios/calib/exact-damage-armllt.json`), a generated duel and a course, and their costs.
- The iteration of the paused frame's events is read from the source, not yet from a recording.
- `tasim_run play` and `predict` are T1 and A2; `defs` does not exist yet.
