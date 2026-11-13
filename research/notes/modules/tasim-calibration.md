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
| `calib.py selftest [--against REF]` | each metric's false-alarm rate over 200 splits, seat effect, exact self-consistency, reload against fresh | no |
| `calib.py stale [--since REV] [--json]` | the references the code has made stale | no |
| `tools/tasim/arenascan.py scan` / `pick [MAP]` / `check` / `place` | every map the install resolves graded for the arena; the arena config and scenario written from the best; the configured arena re-derived; the tracked scenarios laid out on it | no |
| `tools/tasim/duelgen.py [--dry-run]` | the duels and movement courses from the install's roles | no |
| `tools/tasim/native.py`, `native.py test`, `native.py roles` / `defs [--archive A] [--folders F]` | `tasim_run` built with the host's gcc; the classifier's test under ASan and UBSan; `tasim_run roles` or `defs` on the install, a mod read with its renamed first archive and folders | no |
| `tools/tasim/tests/test_load.py` | the install loader on a synthetic install: the class record, the weapon slots, the `[DAMAGE]` rows, `nottoair`, the folders, the report | no |

**The interpreter is the shared venv's**, `.venv-undither/bin/python` at the main checkout
(CLAUDE.md, *Python tooling*): `calib.py`'s commands and `applier_check.py` read recordings and
zstd-compressed traces, which need numpy, numba and zstandard. Started under the system `python3`
they re-execute under the venv (`tools/replay/devpy.py`); without one, or when a module is still
missing, they exit 2 ("could not run") naming the venv's interpreter, never 1, which means a check
failed. `arenascan.py`, `duelgen.py`, `native.py` and the fake tasim's `play` need the standard
library only.

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
skirmish's True and Mapped [INFERRED: the skirmish screen's defaults], and the commander toggle to
0, a commander's death ending nothing (the block's `commander_death`, 0 or 1; the lane's launch
sets it through the SKIRMISH screen's button, so the level holds it from its first step); a
reference is keyed by all three.

**No scenario carries a map's coordinates by hand.** duelgen places its duels and courses from
the arena (`calibref.arena_point`, `arena_facing`); a tracked scenario (`duel-armpw-corak`,
`exact-damage-armllt`, `probe-pause`, the lane's `end`) gives each unit `calib.place: [along,
across, face]` -- world units from `a` toward `b` and off the axis, and the facing of side `a` or
`b` -- and `arenascan.py place`, which `pick` runs too, writes its positions from it.
`calib.py engine` refuses, before any lane, a tracked scenario whose positions are not its place
on the configured arena, and any scenario (the lane's end among them) with a unit or a move target
outside the flat rectangle (`calibref.check_positions`).

## The lane and its runs

A **lane** is one tareplay worker: a private instance on a private display, turbo k = 1, held at
launch, recording the exact stream. A **lane-launch** loads the arena and plays run after run in
the same level, each from an **empty point**; it relaunches when the setup key changes (map, line
of sight, mapping, unit limit, game speed, reload or fresh, the commander toggle) or after `--runs-per-launch` runs. The
lanes are threads under one run lock (`tools/runlock.py`).

One run (`tools/tasim/engrun.py`, `Lane.run`):

1. **Settle**: the least T ≥ T_prev + 61 with T ≡ 1 (mod 30) whose hold at T − 1 reads the
   projectile count `main+0x141F3` as 0, raised by 30 while it does not, never past T_prev + 1800;
   then one step to the hold at T. The peek is on the hold before T because the recorder captures
   iteration T's images at the head of step T, before its projectile tick (the capture at `0x4954ED`,
   the projectile tick at `0x495513`), and a held frame runs no step and appends no shot, nor does
   step T with no unit in the level (`engrun.py`, *THE EMPTY POINT'S POOL*). Sixty-one ticks drain every
   temporary sight record: a death of a viewed-seat unit under true LOS leaves one in that seat's
   sight counters (`0x482910` at `0x486748`), expiring at the death's GameTime + 60, and the expiry
   `0x482130` removes it only from GameTime d + 61 on ([engine/map.md](../engine/map.md), *The
   temporary sight records*). Every death of a run is at or before its clear's GameTime T_prev,
   and the launch's apply kills the skirmish's commanders at its hold. They also clear B4's slot
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
limit. The lane's end is bounded in ticks as tareplay's are: it fails by name when the level has
not ended by `LEVEL_END_TICKS` after the end scenario (`Log.wait_end`, *The level's end is bounded
in ticks* in [demo-recorder-completeness](../tadr-port/demo-recorder-completeness.md)).

**The renderer.** A lane renders with GDI (`tagpu_gdi.on`, tareplay's `Worker.gdi`): the GDI
backend makes no Vulkan device, so the lanes take no GPU token of the run lock. `calib.py engine
--gpu` renders them on the default renderer, for a debugging run, and takes a token a lane. Each
lane-launch's manifest head names its renderer (`renderer`; a head without one is the default
renderer's, the only one before the field) and the arena it played (`arena_sha256`: the arena's
configuration, its prose aside, and its launch scenario's bytes). `refs build` refuses a scenario
whose runs two renderers played, and a reference names its renderer in `setup.renderer`. Whether
the renderer changes a run is open ([ImpAI in the engine](impai.html), *The renderer*).

**A resume.** `calib.py engine --resume DIR` plays only the runs DIR's finished lane-launches
(`manifest.jsonl`) lack. A run is a function of its scenario and index -- its wire, perturbation and
seed are `perturb.run_wire`'s of those alone -- and starts from an empty point its own apply makes;
what the apply does not reset is random from launch to launch as well (*What a reload does not
reset*), and an uninterrupted run already hands its chunks to whichever lane is free, so which runs
share a launch is no part of a result. The resume refuses, naming each, a finished lane-launch that
another DLL, arena or renderer played, a run whose scenario file or setup key differs from this
run's, and a run held twice (`engrun.done_runs`). An unfinished lane-launch
(`manifest.partial.jsonl` alone) has no closed recording for `refs build` to stream its traces from,
so its runs are played again; `refs build` reads finished lane-launches only, the ones a resume
counts. New lane-launches are numbered past every `lane<k>-<n>` folder there is.
`tools/tasim/tests/test_resume.py` builds the references of an interrupted run and its resume and of
an uninterrupted run from synthetic manifests and finds them equal, run for run.

## What a reload does not reset

| State | Reset by the next run | Why no metric is biased |
|---|---|---|
| units, features | the clear and `clear_features` in every wire | — |
| projectiles | the settle's hold at T − 1 waits for none; step T adds none | none is in flight at the empty point or the apply; R2 checks both |
| levels and storage | every wire's `storage` and levels | — |
| lifetime totals `+0xAC..+0xDB`, kills `+0xFC..+0xFF`, `+0x104..+0x107`, units made `+0x140` | no | metrics use differences from the apply |
| deadlines `+0xF0`, `+0xF4`, `+0xF8` | their phase, by T ≡ 1 (mod 30) | a fresh run's phase |
| GameTime | no | what follows it is reset by the apply (the next two rows) or aligned by T ≡ 1 (mod 30); the reload-against-fresh self-test is the evidence |
| each record's weapon scan, the cursor `+0x39` of the AI object at record `+0x74` | by an apply that clears (`reset_weapon_scans`, every calibration wire): 0, the constructor's value | an idle weapon takes its target when the scan `0x4089A0` reaches its unit, limit/30 + 1 slots a step, so without the reset the first target, and with it the time to kill, followed the steps since the level load. No R check reads it: the object is not part of a seat's image |
| what a reused slot hands its next unit: each weapon slot's aim (`+0x8`, `+0x16`, `+0x18`), the last hit `+0xF5`, the water state `+0x10A`, `+0x110` bits 6, 7, 12, 13, 15 | by the apply (`fresh_unit`), right after each create: the level fill's 0, or the create's 7 in `+0xF5` for a def with `+0x241` bit 24 | the create does not write them, so a unit in a freed slot began with its predecessor's: the aim the weapon code reads, the last hit the reaper and the AI read, the water state whose change starts `setSFXoccupy` at the first movement pass (`0x43DB50`), and bit 12, which the economy pass reads for a cloaked unit (`0x4017ED`) before the radar pass clears it |
| a canhover unit's mover `+0x2A` | no | read inside the create by the hover fit (`0x48A6DE`), before the applier runs; it is heap the allocator does not zero, as arbitrary in a fresh launch, and the fit's wave also reads the wall clock ([units](../engine/units.md), *The hover wave runs on the wall clock*), so a canhover unit's altitude is not a function of the run in either mode: a generated scenario with a hovercraft (course-hover, some T2 duels) carries that in fresh and reloaded runs alike |
| the simulation RNG `0x51FC88`, the wind | no | random from launch to launch too; each run records them as `exogenous` |
| the explored map | no | its only simulation reader is reclaim's mode 12, R5 |
| temporary sight records | drained by the settle's 61 ticks | none is left at the empty point; R7 checks the counters |
| B4's slot table | no | its only skirmish reader treats every slot freed before the last tick alike |
| the rates (income `+0x90`/`+0x9C`, expense `+0x94`/`+0xA0`) and the storage maxima `+0xA4`/`+0xA8` | by the apply, not by the settle: no economy pass runs at an empty point (*The seat's rates*) | the applier resets them at every apply to values the apply alone decides; R4 compares them |

**The resets and the replay.** `tagpu_scenario.c` is a presentation file with no
`SIM_FEATURE`, so `tareplay.py landing` selects no test group for a change to it; its argument is
its row in `thread-split.allow`. The resets reach a recording only through the unit bytes
`fresh_unit` writes, which the recorder keeps whole but for `+0x110` bits 6–7 (dropped as marks)
and the aim words (hold rows); the AI object's cursor is in no recording, and no puppet runs it.
The test groups whose recording has a mid-level apply that clears, so that both resets run on a
recorded level, are `calib-applier` (at 60) and `contact-waves` (at 60, 90 and 120): playing them
shows the recordings stay self-consistent.

## The per-run checks

`tools/tasim/engtrace.py` reads each lane's recording once and checks every run; a failed check
makes the run unusable and the reference builder refuses its scenario.

| | Check |
|---|---|
| R1 | no live unit at the empty point; the apply's creates are exactly the result's units (slot = ID − 1, type, owner, position); each cleared slot dies once in the clear's iteration; a scenario's slots are the same in every run of a launch with the same swap (each seat's units come from its own block of the unit array, the player record's `+0x67..+0x6B`, so a swap moves them) |
| R2 | no projectile at the empty point (iteration T's images) or at the apply (T + 1's) |
| R3 | after the apply the grid holds only the scenario's features |
| R4 | each seat's image after the apply equals that of the launch's first run of the same scenario and swap (the apply sets the seat's units, and a swap moves them, with their count `+0x144`, to the other seat), but the lifetime totals and the deadlines' phase, the rates and maxima included (the apply resets them), and the scoreboard rank `+0x148`, display state by its readers ([units](../engine/units.html), `+0x148`; owner's decision of 2026-11-13); the storage, flag and levels equal the wire's |
| R5 | the explored map equals run 0's; a difference fails only a scenario that reclaims or captures |
| R6 | the globals and the declared tables equal run 0's but `EMPTY_POINT_ADVANCING`: the RNG; the wind rows `main+0x37EC4..+0x37EE5` and the wind generator's state `s_windState`, advanced once a GameTime; B4's slot table; and, by the owner's decision of 2026-11-12, the engine schedules and counters that advance with time: the pathfinder's per-player weights `pfweight` (`0x5119E8`), unit slots examined `pfexamined` (`0x511A10`) and tick count `pfcalls` (`0x511A38`, its tick `0x40EB70` rewrites the weights every 150 calls), every moveclass's passability watermark `mcwatermark00`..`mcwatermark31` (a GameTime written at each search start; 0 and 5 since 2026-11-12, the rest by the owner's decision of 2026-11-13), and the meteor schedule `meteornext`, `meteorend`, `meteorstrike`, `meteorp1`, `meteorp2` (the shower tick schedules whatever the switch says). Any other field is named. **Adding to the set loosens R6: the owner's decision** |
| R7 | every seat's sight counters zero in `e0` and `eN` |
| R8 | each delayed order changes its subject's list in iteration T + after + 1 (or the report cancelled it), and no create lands in a watched slot before |

**The seat's rates.** The economy pass `0x401360` recomputes a seat's rates and storage maxima
from its units at every pass: it zeroes the maxima before summing (`0x40137D`, `0x401383`) and writes
income `+0x90`/`+0x9C` and expense `+0x94`/`+0xA0` as the pass's sums without reading what they
held (`0x4019C6`, `0x4019DD`, `0x401A05`, `0x401A2D`, [weapons](../engine/weapons.md)); their
other writers are the record's init, the level start, the network's `0x28` receiver, the
transfers and a cheat ([units](../engine/units.md), the `+0x8C..+0xE0` row). The players phase calls
the pass only for a record with a live unit or one that never had any: with the word `+0x144` at
0 and the dword `+0x140` not, it jumps past the call (`0x465525..0x465537`; DISASSEMBLED
2026-11-12). So no pass runs at an empty point, and a seat's rates and maxima there are those of
the last pass that saw its units: the launch's starting commanders' for run 0, the previous run's
for every later one. MEASURED in the C1 lanes on Cavedog Links CC: income 25.0/1.0 (seat 0) and
12.5/0.5 (seat 1, the AI's ×0.5 while it was still type 2) after run 0's apply, 0.3/0.0 after
every later one; the exact scenario's energy maximum 1,000,000 after a launch's first apply and
1,000,100 after a reload's. No settle can reset them, and comparing them after the run's first
pass compares an outcome: two fresh launches of the exact scenario, same wire, read energy expense
20 and 10 after that pass. A seat with no unit in a scenario (the course's) never runs a pass in
the run at all.

So **the scenario applier resets them at every apply**, after its creates, on the game thread
inside the apply, for each active seat the scenario names (`reset_pass_results`, `tagpu_scenario.c`): the
rates to 0, the record init's value, and the maxima to what the next pass would compute from the
seat's units after the apply (every alive, finished unit's `energystorage`/`metalstorage`, a
pending death left out, then the base storage while `+0x149` bit 0 is set), raised to a level the
wire sets. A seat with units reads the same maxima before its first pass as after it unless a unit
is finished or lost on the way; a seat with none keeps rates 0 and its base storage for the whole
run. Either way the values follow from the wire and the units it made, never from the previous
run. The readers in between, established from the disassembly: the rates go only through the
getters `0x464AB0..0x464B20`, called by the stock AI's planners (`0x408691`, `0x40874A`,
`0x40BC53..0x40BCC5`, type 2 only) and the resource bar (`0x468F8D..0x468FB4`); the maxima are read
by RepairPatrol `0x405980` and VTOL_RepairPatrol `0x4152F0`, which compare the stored level with
0.2 × the maximum and a feature's yield with the room left, by the AI's score, the share sliders,
the network's sends and the bars. Nothing clamps a level to them but the pass, against the maxima
it recomputes (`0x401AB3..0x401B27`), so the reset never clamps the storage the scenario sets. The
reset is withheld in a network game and on a puppet, where the calibration keys are refused.

A run's `t = 0` seat row, the state after the apply, carries the reset values, so the `extractor`
exact family's metal income at `t = 0` is 0 in every run, as tasim's must be: it starts from the
applied state, before any pass.

**How a refused scenario reads.** `calib.py refs build` prints, for each scenario it refuses, the
number of failed runs, then each check with the runs it failed and under it every field it named
(R4 a seat's dword, R6 a global or table) with its count and first run, so one report shows every
cause at once. A reference is all or nothing per scenario, never per lane: a scenario the engine
refused (`engrun.Refused`: the applier refused its wire, or one of its delayed orders) is named with
the run and the reason from its lane's `refused` row, and its runs are not played on, while the
lane plays the next scenario; any other scenario of the lane builds. `refs build X` reads the
finished lane-launches in X and in every stamp folder under X (`calib.py engine --out X` plays into
`X/<stamp>`), and ends with a `cost <kind>` line for each kind (the scenario's name to its first
`-`): its runs, the lane-seconds a run (each lane-launch's seconds shared over its runs, launch and
settles included: what sizes a piece) and the run-seconds a run. `--check-only` streams and checks
every run the same and writes no reference: a smoke run plays fewer runs than its scenarios ask,
and a reference of 2 runs would be graded as one of 25, so the smoke is checked and costed, never
built.

**The iteration of an event made on a paused frame.** The recorder's capture point publishes the
last step's events and then writes the new tick's `RK_TICK` and images (`tagpu_rec_capture`), and
an event goes into the ring when the engine makes it. So the apply's creates and the clear's
deaths, made on the frames held after step T, come in iteration T after step T's events, and the
images of T + 1 are the first to show them. A recording confirms it: a probe lane's launch apply,
on the hold at GameTime 1, put both starting commanders' deaths in iteration 1.

**GameTime and iteration labels.** An event labelled iteration L was made in the step that ended at
GameTime L, or on a frame held after it; the images of iteration L are the state at GameTime L − 1
with those paused-frame events applied. A frame dump taken on a frame held at GameTime G is
therefore labelled iteration G + 1 (`frame dump eN at GameTime 511 (iteration 512)`,
`tagpu_rec_dump_now`), and the probe's verdict, "paused at GameTime 307 … the last combat death in
the step that ended at GameTime 307", compares an event's label with a GameTime in the same unit.

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
| `margin` | side A's HV(t_e)/HV(0) less side B's, in [-1, 1]: what ImpAI's fight predictor is fitted to (`tools/impai/predfit.py`) |
| `income.<res>@c` | a side's produced(c) − produced(0), c a multiple of 1800 |
| `travel.first/median/last` | the tick each mover first comes within 16·(fp + 1)·⌈√n⌉ of its destination; censored at the limit |
| exact families | `damage` (each HP decrement), `reload` (each shot), `range` (each target's first shot), `build_time`, `cost_per_tick`, `extractor`, `storage` (each side's maxima at every economy pass after t = 0), `sight` (the cells a seat's counters hold in the run's dump at t = 2) |

## Perturbations

`tools/tasim/perturb.py` makes every run's wire with tacli's own compiler, from xorshift32 seeded
with the first four bytes of sha256("<scenario sha256>:<index>"): a mobile unit's position moved by
−8..7 world units on each axis, then each order's `after` raised by 0..15 ticks; odd runs swap the
seats. An exact scenario is never perturbed. A reference records each run's perturbation and its
wire's sha256, and the checker refuses one that `perturb.py` no longer makes.

**Every unit's facing is in the wire**: the scenario's, or 0 (heading `0x8000`) where it names
none, and the applier writes it after the create. The create draws a structure's heading from the
simulation RNG, `0x8000` − BuildAngle/2 + rand(BuildAngle), BuildAngle the def's word `+0x210`
(`0x485BD6..0x485C06`; a mobile unit's is then overwritten with that word, `0x4860BB..0x4860ED`),
so without it a structure faces where the launch's random stream says and its turret's first aim
turns further or less. MEASURED on the exact scenario at `5c28bdea`: the ARMLLT at headings
`0x6E49` and `0x7054` (reload runs 0 and 1) fired first at t = 16, at `0x61D0` (fresh run 2) at
t = 18, every later shot and decrement two ticks behind; the three runs' images differed after the
apply only in the two units' heading `+0x66`, the hover phase `+0xAA` (also drawn at the create,
`0x485D22`) and the tower's weapon fields `+0x14..+0x1B`. So the self-test's fresh-against-reload failure was the
random stream, not the reload. The tower takes its attack as Attack_NoMove `0x402160`, a
structure's (slot 0 only, no range test of its own), not Attack_Chase; and its laser is a beam
weapon whose every shot is a projectile record, which the projectile tick moves and the R2 check
sees ([weapons](../engine/weapons.md), the readers of `+0x111`).

## References

Untracked, under `<main checkout>/.data/calib/` (or `$TACALIB_DIR`), because they carry the
install's costs and HP: `refs/<stem>@<sha12>/<commit12>.json` (`ta-calib-ref/2`), and an exact
scenario's engine traces beside it (`.traces.jsonl.zst`); `runs/<stamp>/lane<k>-<launch>/` each
lane-launch's manifest, log, dumps and recording (deleted once its references are built, unless
`--keep-recordings`); `scenarios/gen/` duelgen's output. A reference names the build the DLL that
played embeds — its `GIT_COMMIT`, read from the lane's recording (`RT_BUILD`), never the tree's —
with its commit (a dirty build, or one this repository does not have, is refused), the DLL file's
sha256, and the tree's HEAD when the reference was written with whether the two are one commit
(`head`, `head_matches`; `calib refs build` prints a line when they are not), the install's fingerprint (the types' keys and CRCs, the weapon
and feature-def fingerprints), the setup, the values, `perturb.py`'s sha256, the distribution
rule's `shuffle` (its seed, the first four bytes of sha256("<scenario sha256>:shuffle"), and its
count, 2,000), every run with its metrics, checks and exogenous values, and the engine time it
cost. A reference without `shuffle` cannot be checked: rebuild it with `calib.py refs build`.

**Staleness** (`calib.py stale`): a reference made at commit C is stale when C..HEAD changes a sim
file's code lines (tareplay's own reading: comments and white space are not code), a header one
includes, `tagpu_scenario.c`, `tools/tacli`, `perturb.py`, `engrun.py`, the arena config or the
arena's scenario, or the reference's own scenario. `tareplay.py landing` counts the references a gated landing made stale and records it
as `calibration_stale=` in its note; re-measuring is a command, never part of a landing.

## The check

`tools/tasim/check.py`, against each scenario's newest reference:

1. **win rate**: |p_t − p_e| ≤ 2·SE, SE = √(p̄(1 − p̄)(1/n_e + 1/n_t)), equality when SE = 0;
2. **distribution**: P10_e ≤ P50_t ≤ P90_e (numpy's linear quantiles; a censored value enters as
   its limit), and two permutation tests at 2.5 % each, so at most 5 % together: the two-sample
   Cramér–von Mises statistic (shape and location) and |D_e − D_t|, D a side's mean absolute
   deviation from the pooled median (spread). Each limit is its statistic's 97.5th percentile over
   the reference's 2,000 seeded reshuffles of the engine's and tasim's runs pooled, within each
   seat swap: tasim's run i plays the engine's run i's swap, so the observed labelling is one of
   the reshuffles whenever tasim plays the engine's distribution, and the rate holds whatever the
   ties. The limits are computed at check time from the two sides' runs; the reference stores the
   seed, so they reproduce;
3. **exact**: each metric's series equal at every tick, no tolerance;
4. **predictor**: on the decisive scenarios (p_e > 0.8 or < 0.2) the call agrees with the engine's
   majority on at least 90 % (the rule is in; the call needs `tasim_run predict`, A2).

Exit 0 every metric in band, 1 one out (each named), 2 could not run (no reference, a dirty or
malformed one, a stale one under `--require-fresh`, a tasim failure, a perturbation mismatch). The
scoreboard groups by `calib.area` less its last part; `--json` writes `ta-calib-score/1`.
**Self-tests**: each metric's false-alarm rate, the reference's runs split 200 times into halves
holding half of the unswapped and half of the swapped runs, the second graded as tasim against the
first (exchangeable, so every OUT is a false alarm; each permutation test's OUTs are counted
too); the swapped runs against the unswapped (the seat effect, reshuffled as one stratum, since
the swap is what it tests); an exact scenario's runs 0, 1 and 2 (fresh) equal; and `--against` a
fresh reference. Exit 0 when every self-test ran, 2 when one reported an error (an exact
scenario's runs differing among them) or there was nothing to test -- no reference, or none
`--only` matches: a self-test of nothing tested nothing.

**Why the pool.** A limit estimated from the engine's runs alone is noisy on a metric with few
distinct values. MEASURED 2026-11-12 on the fifth verification's `duel-armpw-corak` references:
its survivors take 7-11 distinct values with 62-76 % of the runs at 0.280. Halves of 25 runs gave
0.07-0.18 false alarms with an engine-only limit, against 0.04-0.06 with the pooled one.
Reshuffling the pool across swaps compares the swap-balanced split with unbalanced ones, which a
seat effect makes differ: 0.13 on the same survivors.

**Power at 50 runs a side** (`tests/test_check.py`, seeded draws): a +10 % median at a 10 %
coefficient of variation is OUT in 99 of 100; a doubled spread in 197, 200 and 197 of 200 for a
normal, a uniform and a lognormal shape, carried by the spread test (Cramér–von Mises alone
catches 0.29-0.63 of 400 other seeded draws). A doubled tail on the 70 % mass point is OUT in
only 48 of 200: seven runs in ten do not move.

**The self-tests on the sixth verification's references** (build `c2d40db8`, which resets the
weapon scans and a created unit's aim and last hit; MEASURED 2026-11-12; false alarms out of 200
splits, the winner's 0 in every set; R1..R8 held on every run):

| set | time to kill | survivors | travel first / median / last | seat effect | reload against fresh |
|---|---|---|---|---|---|
| `duel-armpw-corak`, fresh launches | 0.025 | 0.025 | | IN | |
| the same, reload, `-j` 1, 6 and 12 | 0.025 | 0.025 | | IN | IN, all three |
| `duel-t1-raider-assault-median` | 0.045 | 0.055 | | IN | |
| `course-kbot` | | | 0.035 / 0.060 / 0.060 | travel.last OUT | |
| `exact-damage-armllt` | | | | | runs 0, 1, 2 equal |

**Without the scan reset** (the fifth verification's references, the same 50 wires) the reload's
time to kill had a median of 236 (`-j` 1) and 234.5 (`-j` 6) against the fresh launches' 229, and
reload against fresh was OUT: 48 of the 50 fresh launches, each applied at GameTime 91, took both
targets 86 steps after the apply, where the reload lane, applied at 91 + 330k, took them 78 to 107
steps after, the ARMPW in 34 of its 50 runs on the step where 51 × GameTime mod 1500 was at most 51
(the weapon scan's cursor reaching the first slot of each block at a 1500-unit limit). With it, the
first target step equals the fresh launch's in all 50 runs of every set.

**What still differs run by run is the random stream.** Fresh and reloaded runs of the same wire
give the same time to kill in all but one or two of 50 and differ by one tick there (run 25: 235
fresh, 234 at `-j` 1, 6 and 12; run 42: 227 at `-j` 1, 226 in the others), and `-j` 6 and 12
agree on all 50.
The turret fire handler adds `rand(s) − s/2` from the simulation RNG to each shot's aim, s
growing as the shooter loses health (`0x49D6E0..0x49D733`, [weapons](../engine/weapons.md),
*accuracy*); the projectile flies the direction that gives, and a shot that meets the target near
its edge does so a step sooner or later. MEASURED by reading the recordings: `-j` 6 and 12 apply
run 25 at the same GameTime 1741 with different RNG states. Their shots are identical until the
shooter first loses health (t = 110), which makes the spread non-zero [INFERRED: the weapon's
accuracy is 0, so a full-health shot draws nothing]; from t = 114 its slot's aim words
`+0x16`/`+0x18` and its shots' velocities (the projectile record's `+0x1C`/`+0x24`) differ, from
the same positions on the same steps, and the CORAK's HP falls at t = 160 against 161 and 172
against 173, while both runs end at 234. Agreement between two sets is
therefore an outcome of the draws, not of the apply's GameTime, and a run-for-run equality would
need the RNG to be the run's, which it is not by design (*What a reload does not reset*); the
distributions are what the reference compares.

**`course-kbot`'s seat effect is the `after` draws, not the seat.** Every unit starts on the step
after its order's `after` in every run, and its walk from there to the arrival radius has the same
median in both seats to a tick (a permutation test on each unit's mean walk, the swapped runs
against the unswapped: p 0.13 to 0.93). travel.last's medians, 1689 unswapped and 1686 swapped,
are exactly the medians of max(`after` + 1 + the unit's median walk) over each group's draws, 1689
and 1686: the draws alone predict the difference, and theirs between the even and the odd runs is
chance (p 0.43). MEASURED 2026-11-12 on the sixth verification's 50 recorded runs. With three
travel metrics each tested at about 5 %, one OUT among them is expected now and then; the course
needs no stratum of its own.

### The tasim_run interface

`tasim_run play --setup F --wire F --run-seed S --exogenous F --limit L --end "0:0,1:0" --trace F
--json` plays one run and writes its trace; one JSON object on stdout, exit 0 when it ran and 2
when it could not. It lands in T1; until then `tools/tasim/tests/fake_tasim.py` stands in, writing
traces from a table, for the checker's tests; without `TASIM_FAKE_TABLE` it exits 2.
`fake_tasim.py table RUNS_DIR OUT.json` builds a table from an engine run's lane manifests, every
run's engine trace under its seed (`perturb.run_seed`: the scenario's hash and the run's index), so
`TASIM_FAKE_TABLE=OUT.json calib.py check --tasim tools/tasim/tests/fake_tasim.py` against the
references built from the same directory grades every metric in: the checker's end-to-end test,
with the engine standing in for tasim.

## Roles

`tasim_roles.c` classifies every unit type from its def's fields, the one implementation for the
duel generator (through `tasim_run roles`) and for ImpAI's threat table, filled from the engine's
loaded defs at every level init ([ImpAI in the engine](impai.html), *The threat table*). Pure: no globals,
the caller's scratch space, SSE2 in the DLL (the `Makefile` rule beside the wiki's).

- **Value** = metal + energy / R, R = `TASIM_ROLES_R` = 60, the retail metal maker's 60 energy for
  1 metal (owner's decision: one named constant; `roles` reports the mod's own maker as `r_maker`).
- **Layer group**: air if it flies; sea if it neither hovers nor is amphibious and needs water; land.
- **Anti-air slot**: its bad-target category (`wpri_`/`wsec_`/`wspe_badTargetCategory`, def
  `+0x231/+0x235/+0x239`) is NOTAIR, or its weapon is a toairweapon -- unless the weapon carries
  Impure's `nottoair` (`TASIM_WF_NOTTOAIR`), which the engine honours by never aiming it at an
  aircraft, so it hurts none ([data keys](data-keys.html), the can-engage verdict `0x49ABB0`); its damage is the weapon's
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

`tasim_run roles` (`tools/tasim/tasim_run.c`) fills the input with a copy of the definitions the
install loader read (`tools/tasim/tasim_load.c`, through the shared core's key table), as the engine
filler copies the engine's: every field as the FBI and weapon parsers store it, the water depths and
footprint the move class record's when the unit names one (the record wins over the unit's own keys,
`0x42CD0C..0x42CD97`), weapon1..weapon10 the records in the slots their names resolve to as
`0x49E5B0` resolves them (a section whose ID is past the slots is no weapon), each weapon's air
damage the mean of its `[DAMAGE]` rows over the types of category VTOL in def row order, matched
without case as `0x499CD0` matches them, and the units in the def array's order. Build options come
from the loader by the sidebar's page rule (`tagpu_buildmenu`) over the GUI pages and download
entries; the engine's page count (`+0x22E`) is not read from a file, so the pages are those that
exist or that an entry names [INFERRED]. On the retail install (0.1 s with the archives in the page
cache): land 20 defence, 15 assault, 22 raider, 10 artillery, 15 scout, 6 antiair, 35 builder, 63
other; ARMPW tech 1, ARMZEUS 2, CORKROG 3. The class record changes the footprint of 13 retail types
(ARMAAS 3 × 3 by its own keys, 4 × 4 by its class); no role, layer or duel changes. On ProTA 4.8
(`native.py roles --archive ProTA.gp3 --folders gui=guiP,gamedata=gamedatP,download=downloadP`):
land 32 defence, 10 assault, 19 raider, 14 artillery, 20 scout, 8 antiair, 42 builder, 64 other. The engine filler's
parity with it is `tools/impai/roles_check.py`: a game's `impai: role` lines (the check lever) against
`tasim_run roles` on the same game folder, type by type -- role, layer, value, damage per second
against ground and air, ground range and the role flags, exactly; the tech level apart, which the
engine filler leaves to the build options it does not read.

**Duels** (`duelgen.py`): the land roles raider, assault, artillery, scout and defence in unordered
pairs (not defence against defence), per tech level and at three ranks of value, A at the arena's
`a` and B at the longer ground range + 160 (a defence always B, at its own range + 160), each
attacking the other when the engine's resolver takes the order (`duelgen.takes_attack`, mode 3 of
`0x43F0E0` read from `tasim_run defs`' flag words: no `canattack`, no weapon and no `kamikaze`, slot 0
anti-air on a ground target, no waterweapon on a target that may be submerged, a hover's waterweapon
on one that is not -- a unit it refuses gets no order, and the group, defence and naval pools hold
only types it takes on some target); dropped when neither can hurt the other, refused when B leaves the flat area or
the shots' cost over the limit passes half the storage. **Courses**: eight of each land move class's
cheapest mobile non-builder moved across the arena. Retail: 64 duels, 3 courses (kbot, tank, hover),
4 refused: ARMVULC fits the arena (3 240 world units apart) but its shots over the 5 400-tick limit
cost more than half of each seat's 1 000 000 storage.

**The predictor's fights** (`duelgen.py --groups`, `--defence`, `--naval`), each drawn by a random stream
seeded with its own name, graded for `winner`, `ttk`, `survivors` and `margin` at 25 runs: 150 group fights
(one to three types a side of the mobile land roles, 3 to 12 units, side B at a value ratio of 0.5, 0.75, 1,
1.33 or 2 to side A, the tech levels in turn), 30 defence fights (one to eight mobile units against one to
three defence structures of the same tech, B at its range + 160) and 40 naval fights (the group rule on the sea
roles). Each side stands in rows of four across the axis (`write_fight`, the one placement every kind uses),
side B at the longer range + 160. Types whose shots a side of 12 cannot pay from half the storage (CORKROG) and
defences whose range leaves the rectangle (CORINT) are in no pool. Retail: 149 groups (`group-015` dropped,
neither side can hurt the other), 30 defence and 40 naval fights. The naval fights play on **the water arena**,
`tools/tasim/calib-arena-naval.json`: Seven Islands, a 5000 x 1600 rectangle of open water of depth at least 38
(the naval scout's survey: the deepest retail ship needs 15), the lane's end keeping its seat-1 unit on the dry
strip north (`end`); `calib.py engine`, `refs build` and `check` take `--arena` (through `$TACALIB_ARENA`), and
arenascan.py does not grade a water rectangle, so only the lanes' launch check of the meteor shower holds
there. A reference names its arena's files for staleness by its map (`check.py`, `ARENA_FILES`).

## Speed

`tools/lanecpu.py --against THREADS` measures one GDI lane's CPU: `calib.py engine --j 1` in a
`systemd-run --user --scope` cgroup, its `usage_usec` read every second, the peak over ten seconds
set beside a Vulkan lane's figure; a wineserver started before it serves the lane from outside the
scope and is not counted. The scope is `<unit>.scope` (systemctl reads a bare name as a service), and
every way out of the tool stops it, which ends the calibration with it and frees its run-lock slots.
MEASURED 2026-11-13: a GDI lane peaks at 1.54 threads over ten seconds (1.82 over one, a mean of
1.39) against a Vulkan lane's 2.20, so a GDI lane costs less than the run lock's lane size.

Measured 2026-11-13 under GDI on three lanes beside other games: lane-seconds a run 3.1 to 3.9 for
the duels (1 300 runs), 5.7 for the group fights (95 runs), 6.3 for the defence fights (610 runs) and
10.4 for the naval fights (50 runs); a smoke of 2 runs a scenario took 12.7, its launch shared over
too few runs. `--j` defaults to 6 and `--runs-per-launch` to 200, and the cost a reference records
(`cost.lane_seconds`, `ticks`) is what each scenario took. A wait reads the log every 20 ms
(`tareplay.Log.NAP_S`, every harness's log wait), since a run waits on one line per verb.

## Gaps

- The `calib-applier` fixture and the units clause's probe have passed, the fixture before its seat
  check held seat 1 to the spec; its strict check is not yet run. The sixth verification ran
  R1..R8, the settle's 61 ticks against R7 included, on full lanes of the duel (fresh and at `-j`
  1, 6 and 12), the exact scenario (`scenarios/calib/exact-damage-armllt.json`), a generated duel
  and the kbot course; their costs are in each reference and not compared here.
- `tasim_run play` and `predict` are T1 and A2.
- `defcheck.py` has not been run against a game: only against a synthetic memory made from the
  core's own JSON. It takes no `--archive` or `--folders`, so it reads a mod's instance as stock.
