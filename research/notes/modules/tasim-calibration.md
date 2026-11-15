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
| `tools/tasim/calib.py engine --scenarios GLOB [--runs N] [--j J] [--mode M] [--setup NAME]` | the engine phase: lanes, runs, manifests and recordings; M is `reload` (the default: run after run in one level) or `fresh` (a launch a run); with `--setup` every lane plays the compat setup's own folder (*On a compat setup*) | yes, under the run lock |
| `calib.py engine --probe-pause` | one duel ended by the pause's units clause, timed against its recording | yes |
| `calib.py engine --arena tools/tasim/calib-arena-packed[-naval].json --pack K --scenarios GLOB [--prior REFS] [--batches-per-launch N] [--draw]` | the packed lane: the plan written to the run's `plan.json`, played in batches of at most K runs on the generated map; `--resume DIR` plays the plan's missing batches (*The packed lane*) | yes, under the run lock |
| `calib.py refs build RUNS_DIR` | the recordings read, checked R1..R8, merged into references: at most `--j` lanes at once, each in a process of its own on the slots of the run lock that carry its memory (*The trace reader's memory*) | no |
| `calib.py check --tasim PATH [--only GLOB] [--require-fresh] [--json OUT]` | tasim graded against the references: the scoreboard, its plays on `--j` slots of the run lock (fewer when fewer are free) | no |
| `calib.py selftest [--against REF]` | each metric's false-alarm rate over 200 splits, seat effect, exact self-consistency, reload against fresh | no |
| `calib.py stale [--since REV] [--json]` | the references the code has made stale | no |
| `tools/tasim/arenascan.py scan` / `pick [MAP]` / `check` / `place` | every map the install resolves graded for the arena; the arena config and scenario written from the best; the configured arena re-derived; the tracked scenarios laid out on it | no |
| `tools/tasim/duelgen.py [--dry-run] [--setup NAME]` | the duels and movement courses from the install's roles, or a compat setup's | no |
| `tools/tasim/native.py`, `native.py test`, `native.py roles` / `defs [--gamedir DIR \| --setup NAME] [--language L]` | `tasim_run` built with the host's gcc; the classifier's test under ASan and UBSan; `tasim_run roles` or `defs` on an install or a compat setup's folder, each read through its profile (*On a compat setup*) | no |
| `tools/impai/roles_check.py LOG… [--gamedir DIR \| --setup NAME]` | a game's `impai: role` lines against `tasim_run roles` on the folder it played (*Roles*) | no |
| `tools/tasim/defcheck.py --setup NAME` / `--catalogue RESULTS --setup NAME` | the core's definitions against the engine's, byte for byte: in a test launch of the setup's folder, or offline against the compat suite's reads | `--setup` only |
| `tools/tasim/tests/test_load.py` | the install loader on a synthetic install: the class record, the weapon slots, the `[DAMAGE]` rows, `nottoair`, the folders, the build options as the engine reads them, the report | no |
| `tools/tasim/synthmap.py install GAMEDIR land\|naval` / `sha256` | the packed lane's generated flat maps and their walled sites (*The packed lane*) | no |
| `tools/tasim/pack.py plan --scenarios GLOB --arena CFG --k K --refs DIR [--viewed N]` | the packed lane's batches of a corpus on one generated map | no |
| `tools/tasim/packcheck.py --u REFS --p REFS [--predfit-u J --predfit-p J]` | the packed references against the unpacked: the equivalence proof's tests T1..T4 | no |

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
slot (the [allocator](../engine/network.html), *The allocator*); each launch asserts it from its
log. The applier passes a slot only for a unit whose wire line says `slot=`, and only one that
`sched_slot_ok` admits: inside the owner's block, holding no unit, not freed by this apply's clear
or in the current tick, and watched by no pending delayed order. So a request bypasses B4's hold
only on a slot no entry watches and no recent death freed, and no create lands in a watched slot,
and the delayed orders' watch holds. A run whose wire has such a create requires the applier's
report to count every one made (`engrun.sched_report`). Every wait bounds a dead or hung game (`ACK_S`, `STALL_S`)
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

**The pacing.** A GDI lane runs NO DRAW (`tagpu_nodraw.on`, `src/tagpu_nodraw.c`; `engrun.py`, *THE
PACING*): while the turbo runs, a frame whose batch was the turbo's skips the game screen's draw and runs
DrawUnit's repose of the units the draw would reach and what the harness reads in its place; held frames,
where every apply, settle and dump is taken, draw. `calib.py engine --draw` renders every frame, and
`--gpu` implies it. Each lane-launch's manifest head names its pacing (`pacing`: draw or nodraw; a head
without one was drawn, the only pacing before the field), and a resume refuses to mix two, as it refuses
two renderers. MEASURED 2026-11-13: a calibration run about a quarter faster under no draw. Screenshots
and `tacli ui` of a lane under no draw show the last drawn frame. That an undrawn frame reposes exactly
the units the draw would, in its order, is checked in one process: the replay test group `nodraw-shadow`
compares that selection with the draw's own calls on every drawn frame. That nothing else the draw
writes reaches the simulation rests on the disassembly ([render](../engine/render.html), *What
DrawGameScreen writes*) and is open: `nodraweq.py` found two kept fields that part drawn from undrawn
takeovers (same page, *Not established*). `rendereq.py --pacing nodraw` compares the traces of whole
drawn and undrawn engine runs.

**A resume.** `calib.py engine --resume DIR` plays only the runs DIR's finished lane-launches
(`manifest.jsonl`) lack. A run is a function of its scenario and index -- its wire, perturbation and
seed are `perturb.run_wire`'s of those alone -- and starts from an empty point its own apply makes,
which also seeds every random stream from the wire's seed (*What a reload does not reset*); an
uninterrupted run already hands its chunks to whichever lane is free, so which runs share a launch is no
part of a result's distribution. The resume refuses, naming each, a finished lane-launch that
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
| a canhover unit's mover `+0x2A` | by the mover's constructor, at the create: `0x80000000` (Impure's engine fix) | read inside the create by the hover fit (`0x48A6DE`), before the applier runs; the constructor writes it and the fit's wave runs on GameTime ([units](../engine/units.md), *The hover wave*), so a canhover unit's altitude is a function of the run in both modes |
| the simulation RNG `0x51FC88`, the game thread's CRT stream, the wind, the meteors' stream; the wind's next change `main+0x37EC4`, the next shower `0x5122E8` and its active flag `0x512318` | by an apply that clears, after its clear and before its creates, under the harness's switch `tagpu_seeds.on` that every Worker writes (`tagpu_patches.c`, *A run's seeds*): every stream seeded from the wire's seed, which `perturb` makes the run's own (`run_seed(sha, index)`) and `pack.batch_seed` the batch's, the seed's wind drawn at the apply itself (its next change now + 30·(5 + r % 10)) and the next shower set to now + the strike period | so a run's draws, the creates' own (a structure's heading, the hover phase `+0xAA`) included, are a function of its scenario and its index, not of the runs its lane-launch played before it; a packed batch's are a function of its cells' jobs, shared by its cells (*The packed lane*, *The streams*). The lane holds every apply to its one run-seeds line (`engrun.seed_report`), and each run records the streams as `exogenous` (*The per-run checks*, THE EXOGENOUS POINT). R6 reads the wind's and the meteors' generator states at the apply, against the seed's, not at the empty point (*The per-run checks*, R6); the wind's and the meteors' rows still advance there (`EMPTY_POINT_ADVANCING`) |
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
| R6 | the globals and the declared tables equal run 0's but `EMPTY_POINT_ADVANCING`: the RNG; the wind rows `main+0x37EC4..+0x37EE5` and the wind generator's state `s_windState`, advanced once a GameTime; B4's slot table; and, by the owner's decision of 2026-11-12, the engine schedules and counters that advance with time: the pathfinder's per-player weights `pfweight` (`0x5119E8`), unit slots examined `pfexamined` (`0x511A10`) and tick count `pfcalls` (`0x511A38`, its tick `0x40EB70` rewrites the weights every 150 calls), every moveclass's passability watermark `mcwatermark00`..`mcwatermark31` (a GameTime written at each search start; 0 and 5 since 2026-11-12, the rest by the owner's decision of 2026-11-13), and the meteor schedule `meteornext`, `meteorend`, `meteorstrike`, `meteorp1`, `meteorp2` (the shower tick schedules whatever the switch says). Any other field is named. In a lane-launch whose head says `streams` "apply" the two generator states `s_windState` and `s_meteorState` are read at the apply instead (`engtrace`, *THE STREAMS AT THE APPLY*): in iteration t0 + 1, captured before anything draws either again, the meteors' must equal the state its wire's seed implies (`stream_states`, which mirrors `tagpu_seed_run` and is tested against its constants), and the wind's that state after the one draw the apply makes, with the wind's rows holding that draw's next change and heading (`wind_drawn`); their empty-point values, which the apply overwrites, are not compared. MEASURED 2026-11-14 on V1's second play, on a DLL that left the wind's draw to step t0 + 1: both tables at t0 + 1 of the batches at GameTime 91 and 1381 equal `stream_states` of their seeds. Without that head both are compared at the empty point as before. **Adding to the set loosens R6: the owner's decision** |
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
−8..7 world units on each axis, then each order's `after` raised by 0..15 ticks, **one draw a queue**:
an order with `queue` adds the value its chain's head drew, so a unit's queue waits together and reaches the
engine in order, and a file with no queued order makes the same wires it made before the key existed; odd
runs swap the seats. An exact scenario is never perturbed. A reference records each run's perturbation and its
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

Untracked, under `$TACALIB_DIR`, else `<main checkout>/.data/calib/`, because they carry the
install's costs and HP. **The generation in use is the re-targeting one, `<main checkout>/.data/calib-rt`**:
the predictor's fit and check read its `refs/` (reference set `defcfcdb2359`, 284 graded fights, 2026-11-13;
[ImpAI in the engine](impai.html), *The fight predictor*), `calib.py` reaching it through `TACALIB_DIR` and
`predfit` through `--refs`. `.data/calib` holds the one-order generation (`ceed681e6fa9`) as a record and is
not written. **`predfit check` refuses a root whose reference set is not the coefficients'**: the header `emit
--fit` writes names the reference set and the content digest, and a check whose loaded references hash to
another set, or whose content has another digest, could not run (exit 2), naming both hashes and the root --
run without `TACALIB_DIR` it names `ceed681e6fa9` against `defcfcdb2359` -- so no check grades coefficients
against another generation (the check that once read 108/119 was that one; on `calib-rt` it passes 113/122).
A start file names neither and is checked against whatever it is given. `predfit check --evaluation` runs
coefficients on a reference set or content they were not fitted to, as no check of the fit: it names each
mismatch, exits 0 whenever it ran, and writes every fight's margin with the coefficients' and the content's
digests, which the packed lane's equivalence proof reads (*The equivalence proof*, T4). `refs build` refuses a runs folder outside the root it writes to. The 64 duels, byte-identical in
both generations down to every run's wire and perturbation, agree across them: no duel's side-A wins or runs
at the limit differ by Fisher's exact test at 1 % (MEASURED 2026-11-13, 50 runs each; the closest,
`duel-t2-artillery-defence-cheapest`, 33 and 41 runs at the limit, p 0.11). A run keeps its wire across
generations but not its draw: in those two generations, played before a run's apply seeded the random streams
from its wire (*What a reload does not reset*), the simulation RNG and the wind followed the runs its lane-launch
played before it and differ in all 50 of that duel's runs (`exogenous`), so its two references are two samples
of one distribution, not one game played twice. The layout, in either root:
`refs/<stem>@<sha12>/<commit12>.json` (`ta-calib-ref/2`), and an exact
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
as `calibration_stale=` in its note; re-measuring is a command, never part of a landing. A pass over
the references (`stale`, and `check`'s freshness test) reads each base's changes once
(`check.py`, `code_changes`) and grades every reference measured at that base against that one
reading: with `--since` that is one reading for all of them. MEASURED 2026-11-13, 287 references,
`--since main` over 428 changed paths: 99 s when each reference read the changes itself, 0.35 s
now, the same answer for every reference.

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
when it could not. It refuses, exiting 2, a wire with a unit whose `at` is not 0 or that carries
`slot=`, with the error "scheduled and placed creates are the engine lane's": those keys are the
packed lane's (*The packed lane*), and the simulator never plays a wire it would read differently
from the engine. It lands in T1, where tasim's world first consumes a wire; until then
`tools/tasim/tests/fake_tasim.py` stands in, refusing such a wire as the interface does
(`test_check.py`, `ScheduledCreates`) and writing traces from a table, for the checker's tests;
without `TASIM_FAKE_TABLE` it exits 2.
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
  regular slots' (regular: not commandfire, stockpile or interceptor). The role is what a slot is *for*: a NOTAIR
  slot is no toairweapon, and the engine fires it on ground units when no aircraft is a candidate (`0x40B7B0`,
  [orders](../engine/orders.html)), so ImpAI's threat rows (the ground reach, [ImpAI in the engine](impai.html),
  *The threat table*) and its fight predictor count it against ground; the role, the classifier and this
  calibration do not.
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
without case as `0x499CD0` matches them, and the units in the def array's order. The flag bits both fillers
copy are `tasim_roles.h`'s masks (`TASIM_ROLE_F241`, `TASIM_ROLE_F245`, `TASIM_ROLE_WF`): of `+0x245` the
classifier tests canmove and commander, ImpAI's type facts canattack and candgun (`impai_tinfo_attack`); of
`+0x111` ImpAI's army classes also test `beamweapon` (bit 3: a dropped beam is no bomber, `impai_class_of`).
The classifier, the type facts and the classes each assert at compile time that the masks hold every bit they
test, so a bit tested on one side cannot read 0 on both, and the extra bits change no role. Build options come
from the loader by the sidebar's page rule (`tagpu_buildmenu`) over the GUI pages and download
entries, as the engine reads them: each download file's sections by position, the page count
`+0x22E` and orders bit as the def load probes them and the download load raises them, and a page
with no file of its own as every side's `DL` page, the sides ImpAI merges ([ImpAI](impai.html), *The
threat table*). On the retail install (0.1 s with the archives in the page
cache): land 20 defence, 15 assault, 22 raider, 10 artillery, 15 scout, 6 antiair, 35 builder, 63
other; ARMPW tech 1, ARMZEUS 2, CORKROG 3. The class record changes the footprint of 13 retail types
(ARMAAS 3 × 3 by its own keys, 4 × 4 by its class); no role, layer or duel changes. On ProTA 4.8
(`native.py roles --setup prota-4.8`, MEASURED 2026-11-13):
land 32 defence, 14 assault, 22 raider, 12 artillery, 15 scout, 8 antiair, 42 builder, 64 other. The engine filler's
parity with it is `tools/impai/roles_check.py`: a game's `impai: role` lines (the check lever) against
`tasim_run roles` on the same game folder, type by type -- role, layer, value, damage per second
against ground and air, ground range, the role flags and the tech level, exactly. The engine filler's build
options are ImpAI's build menus, the same page rule over the engine's pages, merged over the sides the game
loaded ([ImpAI](impai.html), *The threat table*); the two agree on all six compatibility setups
(`roles_check --setup`, MEASURED 2026-11-13).

**Duels** (`duelgen.py`): the land roles raider, assault, artillery, scout and defence in unordered
pairs (not defence against defence), per tech level and at three ranks of value, A at the arena's
`a` and B at the longer ground range + 160 (a defence always B, at its own range + 160), each
attacking the other when the engine's resolver takes the order (`duelgen.takes_attack`, mode 3 of
`0x43F0E0` read from `tasim_run defs`' flag words: no `canattack`, no weapon and no `kamikaze`, slot 0
anti-air on a ground target, no waterweapon on a target that may be submerged, a hover's waterweapon
on one that is not; a sea target is never submerged only as a `floater`, not `upright`, of waterline 0,
because the creates put an upright unit at the terrain height, on the sea floor at sea (`0x48A870`;
Total Mayhem's and TA Zero's submarines are upright of waterline 0) -- a unit it refuses gets no order, and the group, defence and naval pools hold
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
side B at the longer range + 160. **Each unit with a mover attacks every enemy the resolver lets it attack,
nearest first, as one TA queue** (`queue_orders`): the squared distance between the scenario positions,
before the jitter, orders them, ties to the lower enemy index, and the second and later carry `"queue": true`,
so the engine starts the next attack the tick the head's target dies. A unit with no such enemy, and every
unit without a mover (def `+0x22F` not 1: the defence structures), gets no order and keeps its standing order
([decisions](../impai/decisions.html), *The re-targeting corpus*). This generation lives in its own root,
`TACALIB_DIR=<main checkout>/.data/calib-rt`; the earlier one, with one attack a unit (unit k on the first
enemy from k mod n on that it could attack), stays in `.data/calib` as a record. Types whose shots a side of 12 cannot pay from half the storage (CORKROG) and
defences whose range leaves the rectangle (CORINT) are in no pool. Retail: 149 groups (`group-015` dropped,
neither side can hurt the other), 30 defence and 40 naval fights. The naval fights play on **the water arena**,
`tools/tasim/calib-arena-naval.json`: Seven Islands, a 5000 x 1600 rectangle of open water of depth at least 38
(the naval scout's survey: the deepest retail ship needs 15), the lane's end keeping its seat-1 unit on the dry
strip north (`end`); `calib.py engine`, `refs build` and `check` take `--arena` (through `$TACALIB_ARENA`), and
arenascan.py does not grade a water rectangle, so only the lanes' launch check of the meteor shower holds
there. A reference names its arena's files for staleness by its map (`check.py`, `ARENA_FILES`).

## On a compat setup

The same lanes, references and checks on a mod: a setup of the compatibility suite
(`tools/compat/setups.json`, [the suite](../compat/suite.html)) whose goal is `impure-active`, with no
`levers` and no `managed` mod.

- **A lane is a test launch of the setup's own folder** (`engrun.SetupWorker`): the folder
  `tacompat.prepare_wine` builds -- the retail install with the setup's fixtures on top, so the exe, its
  Patch Loader or TADR module and its files are a player's -- in an instance readied by
  `tacompat.as_test_instance`. Test mode, because the turbo, the launch's hold, the exact stream and the
  harness's seeds (`tagpu_seeds.on`, which `place` writes) are honoured only in a test launch
  (`tagpu_regstore_active`); so the store `prepare_wine` parks is put back. Native DirectPlay, as a player's launch of the suite has it: without it Wine's builtin
  `dplayx.dll` stands in for the folder's own, the Patch Loader of Total Mayhem and ProTA (imported as
  `DPLAYX.dll`) never runs and its renames with it, and the builtin's registry imports end the launch at
  attach (MEASURED 2026-11-13 on `mayhem-11.3.0`). On a port leased for the instance (`tools/dpport.py`),
  saved as its `dplay_port` and released when the lane removes it, so no two games share one. The
  instance's name carries the run's tag and the lane's index, unique among live runs.
- **The settings key reaches the game**: a mod's exe or loader moves TA's registry root, and the
  test-mode store serves `HKCU\Software\<the root at 0x509EB8>` as its own key
  ([settings](settings.html), *TA's key wherever the exe names it*), so the map, the seats and the speed
  tacli writes are the ones the game reads.
- **What every lane-launch reads back** (`engrun.engine_reads`, retail's lanes too), on the held level:
  the cap in force `main+0x37EE6`, which must equal the arena's `unit_limit`; the speed ceiling
  `main+0x38A4B`, which must equal the launch's speed (a 10 says the settings key did not reach the game,
  [sim](../engine/sim.html), *The scheduler, the clock and the test pacing*); the gravity `main+0x14263`;
  and, recorded, the settings root at `0x509EB8` and the ini format at `0x5098A0`. A launch whose values
  are not the key's could not run, by name. The head carries them with the setup's name. On a setup no
  `--unit-limit` is passed: tacli writes it into `totala.ini`, and a mod's exe reads the ini its own
  `0x5098A0` names ([units](../engine/units.html), *The per-player unit cap*). MEASURED 2026-11-13, each
  setup's content launch:

  | setup | root at `0x509EB8` | ini | cap | ceiling | gravity |
  |---|---|---|---|---|---|
  | `mayhem-11.3.0` | `TotalM` | `%s\mayhem.ini` | 1500 | 20 | 8155 |
  | `prota-4.8` | `ProTA` | `%s\ProTA.ini` | 1500 | 20 | 8155 |
  | `escalation` | `TA Esc` | `%s\taesc.ini` | 1000 | 20 | 8155 |
  | `tazero-alpha5` | `TA Zero` | `%s\tazero.ini` | 1500 | 20 | 8155 |
  | `twilight-2.0b98` | `Cavedog Entertainment` | `%s\ta.ini` | 1500 | 20 | 8155 |

- **The arena is retail's**: both arena maps' TNTs and the land `.ota` are retail's bytes in every mod's
  file layers, and the naval `.ota` differs only in `aiprofile=` (MEASURED 2026-11-13, `tavfs.Vfs` over each
  folder). `calibref.arena` adds the setup's name, which makes the arena's hash the setup's own, so no
  resume and no reference mixes two setups, and the setup's `calib` block: `unit_limit`, the cap its own
  ini sets (Escalation's 1000), and `end_type`, the lane end's unit where END's is missing (TA Zero has no
  `CORFUS`: `ARMT1SOLAR`). The first launch of each lane grades the arena against the setup's own files
  (`arenascan.check`); Mayhem's and ProTA's loaders put their archive first in memory, which `tavfs.Vfs`
  over the stock exe's patterns does not model, and neither archive holds an arena file.
- **Each setup in its own root** (`calibref.calib_dir`, THE ROOT INVARIANT): a setup's calibration
  (`$TACALIB_SETUP`, set from `--setup`) lives in exactly `<main checkout>/.data/calib-mods/<setup>`,
  retail's in no folder at or under `calib-mods`, and every scenario a run plays lies in its root's own
  generation. Any other combination exits 2 before a lane starts. Every reader of a root comes through it
  -- `calib.py engine`, `refs`, `check`, `selftest`, `stale`; `predfit content`, `cv`, `fit` and `check`;
  `duelgen` -- because `predfit`'s `load_fights` takes every reference of a root and resolves each type by
  name, so a mod's fight in another root would join its fit with its types resolved where the names are
  shared. `refs build` takes the setup from the lane-launches' heads (one value across them, else
  refused), and `predfit check` from the content file. A reference carries `engine.mod` (the setup, or
  `retail`) and the gravity its lanes read; `predfit` refuses a reference of another setup by name.
- **The content launch** (`predfit content --setup NAME [--files]`) plays one lane-launch and writes
  `impai-fight-content/3` with the setup, the checks it passed (`checked`: `digest`, and `files` when the
  offline loader's reading of the folder equals the engine's content) and the readbacks. ImpAI's damage
  rows per setup, against the pool's 65 536 (`IMPAI_FDMG_POOL`): retail 653, ProTA 1 010, Total Mayhem
  3 014, Escalation 3 572, Twilight 4 877, TA Zero 22 204 (MEASURED 2026-11-13, `impai: fight types=…
  dmgrows=`).
- **The offline loader reads each setup as the game does**: `native.py --setup NAME` builds the setup's
  folder as the suite builds it (`tacompat.setup_tree`) and reads it through its profile
  (`tools/wiki/tavfs.py`, `profile`): every folder, pattern, extension and pre-scan constant the folder's
  running image reads -- its `TotalA.exe` with each imported Patch Loader's list applied -- each read
  where the engine's code reads it, at instruction sites checked to hold that instruction, and handed to
  `tasim_load.c` as a `tasim-profile/1` file. No record names a mod. Measured against the engine on the
  six setups: the type list in order and multiplicity, each type's FBI checksum and the weapon table
  (`defcheck --catalogue`, five mods), every image field (`defcheck --setup`, its two differences closed
  in the loader), and every role, figure and tech level (`roles_check --setup`, all six, *Roles*).
- **The per-mod references** (MEASURED 2026-11-14, the seven player's setups of the suite but gammata's
  drop-in): each root's generation (`duelgen --setup NAME --groups --defence --naval`) played unpacked, no
  draw, at 25 runs, by the DLL of `a236f324`: `group-000`..`-099`, the 30 defence and the 40 naval fights
  (the generator refuses `escalation`'s `group-045`, where neither side can hurt the other, and five of TA
  Zero's naval fights, where no unit can be ordered to attack the other side), 170 references a root, in 63 pieces of 5 to 7 minutes each on four
  lanes; no run could not run. Every check passed in every build. The two retail-content setups,
  `392+tadr-2026.8.6` and `loader+tadr-ota`, built one reference set (`db33fde1f57e`). What the fight
  predictor scores on them: [ImpAI in the engine](impai.html), *The fight predictor*, *On the mods*.
  A mod's group lane takes its trace reader 4.2 to 4.5 GiB, so builds share the machine by the memory they
  hold (*The trace reader's memory*).

**The trace reader's memory.** `refs build` reads each lane (`calib._lane_job`) in a process forked for that
lane alone, on the slots of the run lock that carry the memory the lane needs, and holds the process to them
(`runlock.forked`; `tools/runlock.py`, *THE HOST MEMORY*): a reader that passes them fails
its allocation, and is read again on twice the slots, up to what the build was granted; past that its lane is a
lane error. It never takes the memory of another run or of the reserve the lock leaves the desktop. A reader
holds the whole recording decoded (`tarec.load`) and every run's trace and its apply's tables until the stream
ends (`engtrace.Lane.stream`), so its peak follows the lane's size: `calib.lane_mib` is 150 MiB + 20.2 a MiB of
recording + 2.2 a thousand ticks + 10.2 a run, and a quarter more (MEASURED 2026-11-14, a process a lane, on
nine lanes of 9 to 93 MiB recordings, 45 to 200 runs and 56 000 to 203 000 ticks: 895 to 4501 MiB, each under
the fit and a quarter). A reader that ends without its result -- killed -- refuses the build by name. Its
parent loads no numpy, so a reader inherits none of OpenBLAS's reservation, which its bound would count.

## The packed lane

**Parked (owner's decision, 2026-11-14): no reference is built from a packed lane.** Calibration plays
unpacked, with no draw. That measured 8 850 runs in 46 min of wall time and 5.06 lane-hours on the reference
setup (`calib-u-e2633103`, 2026-11-14).

C10, the equivalence run at K = 8, played both arms. Every P reference build was then refused: 353 of 1 198
batches failed the P checks. The cells did not stay in their boxes. Units went up to 1 830 world units past
their box ⊕ 320, while the walls stood at box ⊕ 640 or more, so P1 as written never held.

Restating P1 and P2 on each cell's hull still left 33 batches failing:
- cells too close (21);
- the map edge (6);
- a shot past hull + 1.5 × range (1);
- the shared pathfinder credit (5).

Packing safely needs cells laid on whole sites (about 4.5 runs a batch instead of 7.3), a real bound on a
shot's flight and a bound for P3. The measured gain left was too small for that. The restatement is not on
this branch: it loosens the P1 check, and that is the owner's call. It is kept on `packed-robust-fix`
(e083d419, 02ace4b0), with the analysis in `.data/impai/runs/packed/diag/c10-refix.md`.

Revisit only if calibration time becomes the bottleneck. The tools stay built and are opt-in (`--pack K`);
the rest of this section describes them as they are.

A packed lane plays K calibration runs in one apply: a **batch** of K **cells**, each one job -- a
`(fight, index)` with `perturb.run_wire`'s perturbation, swap and seed -- translated into a walled site of
its own on a generated flat map, cell k created `at` ticks after the apply at block positions P_k of each
seat's block. Each cell is to behave as it would alone but for the simulation RNG, which the owner
accepted (2026-11-13, with the batch's shared wind and the effect pools' draw counts). The streams a
batch's cells share are the batch's own (*The streams*), so no two batches share one, and the
equivalence proof prices what the cells of one batch share (*The equivalence proof*). Everything below
is built; what has not yet been played is in *Gaps*.

**The maps** (`tools/tasim/synthmap.py`). Two 1280 × 1280-cell maps (20 480 world units a side), written
from nothing by `build(kind)`, byte-identical across calls: `impure-calib-land`, height 85 everywhere
under sea level 75, and `impure-calib-naval`, height 0 under sea level 75 with a dry plateau of 85 over
the corner region (0, 0)-(1024, 1024). Each keeps its retail arena's gravity (112), wind range (Links
CC's 1500..5000, Seven Islands' 0..4000), planet and the `.ota`'s line-of-sight and mapping values; no
meteors, no features, two start positions in the corner region. Every 16-wu cell outside the **sites**
and the corner region carries the TNT feature word `0xFFFC`, the engine's void marker (*The walls*), the
open ones `0xFFFF`: 32 land sites of 6528 × 1664 world units (a 3 × 11 lattice from (256, 384), pitch
6656 × 1792, less the one over the corner) and 36 naval sites of 4864 × 1920 (4 × 9 from (128, 1152),
pitch 5120 × 2048). `synthmap.check` asserts every site's origin on the 128-wu grid, inside the scroll
extent, at least 128 from every other site and from the corner region along x or z, and under 65 536
cells; the void words exactly off the open cells; and the meteor shower off. LoadMap's border mask
`0x4833B0` then writes `0xFFFD` over the open cells of its fixed sets (the last two columns, the top rows
while row·16 is under half the cell's height, the bottom rows past the extent; [engine/map](../engine/map.html),
*A saved game's features on the map's border*) and over nothing else, so the void stays `0xFFFC`: on both
maps it reaches the corner region's rows 0..2 alone (192 open cells, height 85), and `synthmap.check`
refuses a site holding a cell it marks (`border_cells`, `engine_words`). MEASURED: the walled maps' launch
read `0xFFFD` at cell (0, 0), the mask's mark where the TNT holds `0xFFFF`. `classcheck.py` holds a running
game's grid and class maps to the core's, its launch on GDI unless `--gpu`. The `.ufo` is a generated
file, never tracked (`.gitignore` refuses `*.ufo`): `synthmap.install` writes it into a lane's game
folder under its own name by a rename, never through a link, and `place` removes the other kind's. The
arenas are `tools/tasim/calib-arena-packed.json` and `calib-arena-packed-naval.json` (scenarios
`scenarios/calib/arena-packed*.json`, the retail arenas' seats): `synthmap` names the map, `flat` is the
region every cell's reach stays inside (the map less the strips the scroll extent leaves off, 32 world
units on x and 128 on y, the border mask `0x4833B0`), `keepout` is the corner region, and both arenas'
`end` keeps the lane end's unit at (512, 512) in it -- the retail land end's point (6020, 1184) is void
here. `engrun.arena_sha256` hashes the map's archive into the arena's, so a change to `synthmap.py`
re-keys every reference made on it, and a lane-launch on such an arena installs the map before the game
starts. On a compat setup the lane checks that the map's `.tnt` and `.ota` open from that archive, so no
file of the setup shadows it (`engrun.arena_against_setup`). `calib.py`'s `arena_check` reports a
generated arena through `synthmap.check`, and `laid_out` checks its scenarios against the retail twin's
rectangle, runs the planner's admission on them and refuses a lane end on void. MEASURED 2026-11-13, one
GDI lane launching each map before the walls: gravity `main+0x14263` 8155 (`synthmap.ENGINE_GRAVITY`;
the retail arenas' read, `.data/impai/map-gravity.json`), sea level `main+0x1427F` 75, wind range
`main+0x1425B`/`+0x1425F` 1500..5000 and 0..4000, the meteor switch off, the unit cap 1500, and an empty
wire applied at the hold. The fights keep their retail coordinates in their files; the planner
translates them.

**The walls.** The pathfinder has no bound tied to a reach and no engine cap ends a search (the A* steps
`0x40ED90..0x40EEE2` compare only the pass cost with 100, at `0x40EEAF`), so no spacing bounds it; the
map does. The invariant: **a search started for a unit of cell k reads class-map entries only inside
site k and on the void cells that border it.** It rests on these engine facts (DISASSEMBLED 2026-11-13,
`synthmap.py`'s *THE WALLS*): a search opens or stands on a cell only over 8-steps through cells of code
≥ 1 -- the expansion `0x40DA70` leaves a neighbour of code 0 dead (`0x40DC68..0x40DC6F`) unless it was
walked, and the bug walk `0x40E160` steps only onto code ≥ 1 (`0x40E21D..0x40E225`, its heads
`0x40E30E..0x40E350` and `0x40E4A9..0x40E4F4`) -- and reads only those cells and their 8-neighbours; a
void cell is 0 for every movement class, since the per-cell rule `0x47DE60` answers 0 for a feature word
of `0xFFFB` or above other than `0xFFFE` (`0x47DE9E`, `0x47DEEB..0x47DEF5`) before it reads any class
field, unit or height, and the fill `0x440500` and the refresh `0x47E1F0` answer 0 for an entry whose
footprint holds a 0 cell, so with footprints of at least 1 every void cell's entry is 0 in every class
map; void cells stay void (LoadMap spawns `0xFFFC` on every TNT cell that carries it, `0x483ACA..0x483ADE`,
on every load, and `FEATURES_Destroy` refuses it, `0x4246EC`); with Mapping on every writer of the
explored map only sets bits, so the reader's "unexplored is passable" answer (`0x40D817..0x40D83B`)
never fires; and void is invisible to everything else a fight uses -- the projectile's feature hit
(`0x49B2BD..0x49B2E5`) and the area damage (`0x49A480..0x49A485`) skip it, and the heights under it are
the ground's. Any two open regions are at least 128 apart along x or z, so an 8-step out of a site lands
on void, and the cell's units start inside it (P1). A site of fewer than 65 536 cells keeps the search's
node word (`0x40DD9A`) from wrapping; a search's pops are bounded by its site's cells, which is more than
a seat's credit in a tick, so P3 keeps its credit role and checks that searches finish within the tick.
VERIFIED offline: the core's class maps (`classmap.c`) on both maps, for all 15 classes of the retail
`moveinfo.tdf`, hold 0 on every void cell, and an 8-connected flood over entries ≥ 1 from each site's
centre stays inside that site (`tests/test_synthmap.py`, *ClassMaps*; with the walls removed the flood
leaves).

**What the DLL gives it.**

- **Scheduled and placed creates** (`tagpu_scenario.c`, *scheduled creates*). A wire unit's ` at=<n>`
  creates it n ticks after the apply, from what the apply copied; its ` slot=<p>` creates it at
  position p of its owner's block through `0x485F50`'s requested slot, admitted by `sched_slot_ok`
  (*The lane and its runs*). Its orders are issued at its create, a delayed one counting its
  `after` from there. A create is made at its own tick or never: the next apply, another level, a
  puppet or a skipped visit cancel what is pending. The scheduled creates never reset the weapon
  scans or the seats' pass results, which only the apply does. One log line reports the whole
  table: `scenario: scheduled creates of the apply at GameTime G: made X of Y, refused Z, skipped
  W; orders issued A, delayed B, refused C, skipped D`, G naming the apply, since a later apply
  reports what it cancels of an earlier table inside its own visit; the lane requires X = Y and
  nothing refused or skipped (`engrun.sched_report`). A delayed order of a scheduled create joins
  the delayed orders' table, which reports each time it empties, so one batch can have several
  delayed-orders lines, which the lane sums (`engrun.delay_reports`). The keys are
  refused in a network game and on a puppet, with the other calibration keys.
- **The `or cells` pause clause** (`tagpu_replay.c`, `pause_cells_visit`): `pause at <T> or cells
  <start>,<end>,<s>:<lo>-<hi><=<n>,<s>:<lo>-<hi><=<n>;…`, up to 50 cells of two ranges of at most
  30 positions. A cell is decided at its `end`, or after its `start` when one of its seats has at
  most n units alive in its range; the batch pauses when every cell is, each logged `pause: cell
  <i> decided at <G> (limit|units)`, then `pause: paused at GameTime G (every cell decided)`. It is
  for speed only: each cell's end is read offline from the recording, and the packed lane's P5
  check fails the generation should the clause ever pause before it.
- **The temporary sight records' cap, a test lever** (`tagpu_patches.c`, `fix_sight_cap`):
  `tagpu_sightcap.on` holding N (21..127) raises the engine's cap of 20 records to N in a test
  launch, through the fail-closed table (`0x481505`, `0x483D88`, `0x48292C`;
  [engine/map](../engine/map.html), *The temporary sight records*). The records are one list for
  the game, so a batch's cells share one cap that no fight alone reaches; raised past the batch's
  viewed-seat units, no add is refused. Impure's readers of the list take the cap in force
  (`tagpu_sightcap`): a seek's and a sight change's drop of the records is bounded by it, so it
  drops every record the engine can hold. A recording made under the lever carries its N
  (`RT_SIGHTCAP`), and a playing refuses a recording whose cap is not its own.

**The stagger.** The weapon scan `0x4089A0` examines s = ⌊limit/30⌋ + 1 slots of a seat's block a step
from a cursor that steps over every slot and that the applier zeroes at an apply that clears. A unit at
block position P + p created d steps after the reset is scanned on the steps p would be at the reset
iff P ≡ s·d (mod B), so cell k sits at P_k = (s·at_k) mod B: 30·k at limit 1500, 20·k at 1000, a
cell's capacity the least distance between two P_k. `at_k` = 30·k keeps every 30-tick phase (the
economy pass, the viewed seat's radar pass, the acquisition scan) where an unpacked run has it. Autoheal
runs every 8 ticks on GameTime ≡ 0 (mod 8) (`0x48AF5E`) for a type with `healtime` (def `+0x200`), so a
batch holding one is staggered by 120 = lcm(8, 30); no rt-corpus type has one. The scans' phase needs
their reset, which the applier makes only at an apply that clears (`reset_weapon_scans`, `g_wire.clear`
with `g_fresh_reset`), so the batch's apply clears, a level that is empty already, since `sched_slot_ok`
refuses a `slot=` create into a slot that apply's own clear emptied. Not every schedule keeps a phase:
the pathfinder's call count `pfcalls` (`0x511A38`) has a period of 150 calls, and the feature tick's
cursor `main+0x14257` steps through the map one cell a step, so where it stands depends on a cell's
place, not its time.

**The layout** (`tools/tasim/pack.py`). For a cell, R = max(sense, 1.5·range + aoe, 160, aoe) over its
types -- range over the three weapon slots, aoe over every weapon death weapons included, sense the
largest of sight, radar, sonar and both jammers -- and its box covers every start position and order
point, widened by the perturbation's jitter. Every pair of cells keeps
`gap(box ⊕ 320, box ⊕ 320) > max(R) + max(X)` along x or z, X the farthest a footprint reaches past
its unit's centre with the pathing ring, 8·fw + 8 + 16 (72 for the 6 × 6 ARMBATS and CORMSHIP, the
footprint rule `0x43D877..0x43D895`), and every `box ⊕ (320 + R)` lies inside `flat` and outside
`keepout`: the walls are invisible to every channel but pathing, so the far field keeps its rule. Each
cell stands in a site of its own at its fight's **canonical place**, the box's centre on the site's
rounded down to 128 (`pack.canonical`), so every offset is a multiple of 128 world units, which keeps
every position modulo every grid the skirmish step reads (16, 32 and 128; radar, sonar and jamming are
exact distances). All sites are one size with origins on the grid, so a cell's world out to and
including its walls is a translate of one world fixed by its fight, in whatever batch and site it
lands: a packed cell's search is its K = 1 run's on the same map. `box ⊕ 640` (WALL_CLEAR, at least 320
+ the footprint extent + 128) must lie inside the site there (`pack.site_problem`). `pack.assign` takes
a batch's cells longest first, each to the first free site that holds it, keeps the far field and the
gap rule against every cell placed, and checks every pair before it answers. ARMBRTHA's R of 6 660
fits only the middle column's middle rows.

**The explored map.** The pathfinder's map reader reads the shared mapped grid `main+0x14273` and answers
an unexplored block passable whatever stands on it (`0x40D817..0x40D831`). A packed fight plays with
`mapping` on, which explores the whole map for both seats from the level's start (MEASURED 2026-11-13 on
both generated maps at the launch's hold: all 409 600 u16 of the grid, a bit a player, hold seat 0's and
seat 1's), and the planner refuses an arena or a fight without it.

**The bounds**, each a refusal of the planner: units a seat a cell within the capacity; 400 units a
batch; the cells' **projectile bounds** summed within 1 500, half the 3 000 the line shot's append
compares with; each seat's metal and energy over the batch's window (shots at the roles' cost a tick,
energyuse and an initially cloaked type's cloak cost a second) within half its storage; the viewed
seat's units within the temporary sight records' cap (20, or the test lever's N). The admission refuses,
by name, an order other than attack and move, a feature, a course, an exact scenario, mapping off, a
unit above 100 % health, a mobile type of footprint below 1, a type with a weapon past the three slots,
a weapon with no projectile bound, and a fight whose box does not fit a site.

**The projectile bound** is static, computed from the engine's def images (`tasim_run defs`), with no
measurement. `pack.shot_life` bounds the ticks from a record's append to its compaction, by kind: a line
shot (bit 0 without bit 20) leaves at its death, GameTime + ⌊(range << 16) / weaponvelocity⌋ or
weapontimer (`0x49CB1C..0x49CB65`); a selfprop shot (bit 20, vlaunch's `0x49CCCE..0x49CD17` the same)
detonates there with burnblow, else falls under the level's gravity from the most it can climb at its
top speed, a twophase one flighttime later; a ballistic shot (no death time) falls from 512 world units
[INFERRED: no fire point stands more than 257 above height 255] to the ground, launched at most at
weaponvelocity; every death gains up to randomdecay/2 and covers a burst copy's (distance + 16) / speed.
`pack.slot_records` = (1 + burst) × (⌊W / p⌋ + 1) records a slot holds at any instant, W = shot_life +
burst · burstrate + 1 and p = max(1, ⌊70 · reload / 100⌋), the least gap between two fires at five kills
and full health (`0x49E48D..0x49E4EE`). A type's bound is the sum over its armed slots, a fight's the
sum over its units, on the map's lowest ground (85 on land, 0 at sea). groundbounce (bit 15) and dropped
(bit 8) have no bound, nor a weapon of none of bits 0, 1 and 20 or a vlaunch one without selfprop: a
fight that arms one is refused. Over the rt corpus a fight's bound is, median / p90 / max: duel 9 / 39
/ 48, defence 22 / 42 / 170, group 66 / 171 / 464, naval 92.5 / 204 / 314. Two checks hold a batch to
it, failing the generation (*The P checks*): **P4a**, for every iteration t, `projcount`
(`main+0x141F3`) in its image plus the most appends step t can make after it -- each live unit's armed
slots and armed burst slots, and the image's burst parents (`+0x60` ≠ 0) -- within the cap, the u32 at
`0x49C9D2` (3000), which implies no append was refused whether the image is captured before or after the
unit tick; **P4e**, for every cell and tick, the records whose attacker `+0x52` is a unit of the cell
no more than the cell's bound, so going over means `shot_life` is wrong.

**The plan** is a function of the jobs, K, the bounds, the sites and the prior (each fight's median
length in the unpacked references): round r takes index r of every fight, longest first, first-fit into
batches of at most K distinct fights; a round's short last batch is carried into the next round, which
fills it only with fights it does not hold; within a batch cell 0 is the longest. So the same fights
meet in batch after batch (MEASURED 2026-11-14 offline, `pack.py plan` over the rt land corpus at K = 8:
a fight shares a median 96 % of its runs with its most frequent partner). Who they are does not enter the
proof's clusters, a batch's cells being one cluster whoever they are; to first order the shared inputs
inflate T2's Stouffer S by (K − 1)·ρ whichever fights a run's partners belong to, and the synthetic arms
show T1, T3 and T4 moving little more (*The equivalence proof*). A cell is kept as
`pack.cell_json` writes it, its fight named by its path under the tree. `pack.wire` writes one
wire through tacli's writer with each unit's `at=` and `slot=` and the batch's seed, and refuses a writer
that drops them;
`pack.pause_clause` writes the batch's `pause at … or cells …`. Over the rt corpus (244 land fights and
7 700 jobs, 40 naval and 1 000 jobs), with the lever at 127, the batches hold on average 4.00, 7.33 and
12.32 cells on land at K = 4, 8 and 16, and 4.00, 8.00 and 12.66 at sea; at K = 16 the land batches are
held back by the sites (the gap rule and the far field), the naval ones by the projectile bound. Without
the lever the viewed seat's units keep group fights near two or three a batch (land 3.62, 4.13 and 4.37;
naval 2.58 at every K).

**The streams.** In a test launch every level starts from fixed seeds, so the wind is a function of
GameTime in every lane-launch (MEASURED on the first packed run: all four lane-launches changed the wind
at GameTime 2, 302, 632 and 1052 to the same values), and two runs share their wind whenever their
GameTime windows overlap, in one batch or in two launches. The applier therefore seeds the simulation RNG,
the game thread's CRT stream, the wind and the meteors from the wire's seed at every apply that clears,
after the clear and before the creates (`tagpu_scenario.c`, `apply_now`; `tagpu_patches.c`,
`tagpu_seed_run`), and draws the seed's wind there and then, so it is in force from step t0 + 1's
projectile tick on (*The wind at the apply* there): from the apply on, a run's or a batch's
random inputs are a function of its seed alone, wherever its lane-launch puts it. A batch's wire carries
`pack.batch_seed`: the first four bytes of sha256 over `calib-batch-streams` and its cells' sorted
`<scenario sha256>:<index>`, a function of its jobs in a domain apart from the runs' `perturb.run_seed`, so
a batch does not take the streams of the unpacked run of one of its cells. The seed is per batch, not per
cell: one RNG and one wind vector serve every cell of the level, and the cells run overlapped (cell 0 still
draws long after cell 7 is created), so a reseed at a cell's create would reseed the running cells' draws
with it and make no cell independent of another. The lane holds every apply, a run's or a batch's, to
exactly one run-seeds line at its GameTime with its wire's seed and the words "before the creates"
(`engrun.seed_report`), else could not run, so a DLL that seeds after the creates cannot make an "apply"
head;
the lane-launch's head then says `streams: "apply"`, which `refs build` carries into each reference's
`setup.streams`, and the batch row keeps its seed.

**The equivalence proof** (`tools/tasim/packcheck.py`) pairs the unpacked arm U and the packed arm P by
scenario id and run index, refusing a pair whose perturbations differ, and runs four tests per metric
(winner, margin, ttk, survivors), each with its null from joint relabellings of **clusters** (owner's
decision, 2026-11-14): a pair's cluster is its P cell's batch (`pack.batch_wire`, the batch's wire hash,
in each cell row's `pack`), an unpacked P run a cluster of its own, and clusters whose runs share a stream
seed (a run's, or a batch's `batch_seed`) are merged, so a 32-bit seed collision cannot couple two
clusters unseen. With *The streams* no two clusters share a random input; inside one the cells share
everything their batch draws. One seeded ±1 matrix F of 10 000 rows, a column a cluster, flips (T2, T3)
or exchanges P and U in (T1, T4) every pair of each cluster whose entry is −1, for every fight and metric
at once. Under the null the relabellings carry the statistic's covariance, a batch's cross-products
included: a Rademacher wild cluster null, valid as the number of clusters grows (a generation has about
1 000), not exact at any number, so the size at the stated alphas is measured, not proven (below). With
every run its own cluster they are each fight's independent flips. The alphas are unchanged; each threshold
moves by the dependence the data carry. Refused, could not run: a P cell without `batch_wire`; two runs of one
fight in one batch; a reference with neither `setup.streams` "apply" nor an engine that does not descend from
the fixed launch seeds (`packcheck.STREAMS_FIXED`, 2f272cd9; an engine before it seeds each launch from
the clock).
**T1** `check.py`'s rules with P against U: the OUT count within the 99 % quantile of the OUT counts
over F's exchanges (`check.grade_swaps`, every exchange graded at once; an exchange keeps the pooled runs
and each stratum's counts, so the permutation limits are the unexchanged labelling's), U's self-test,
when present, giving the Binomial(n, α) limit beside it. **T2** each fight's mean paired difference
d̄ against its flipped means M = (F[:, c]·d).mean, a one-sided mid-p, z = Φ⁻¹(1 − p), Stouffer's S; each
row of F gives every fight a null z by its rank in M, so a null S_b; |S| below the 99.75 % quantile of
|S_b|, with var(S_b), the inflation, and the normal 3.02 reported beside. **T3** the fights with a
two-sided p below 0.01, within the 99.75 % quantile of the same count over the rows (Binomial(n, 0.01)'s
beside it). **T4** the decisive labels' disagreements within the 99 % quantile of the disagreements over
F's exchanges; and predfit's errors on each arm -- one set of coefficients' margins, from `predfit.py check
--evaluation --json` of each arm's references, a decisive fight whose margin is 0 or of the other sign an
error -- with |E_P − E_U| within the 99 % quantile of the same over F's exchanges. Both arms are fresh
reference sets the coefficients were not fitted to, so neither report is predfit's own check, which refuses
another set (*References*); an evaluation runs them there and says so. A margin is a function of the fight's
definition, the content and the coefficients alone, so the two reports must name one set of coefficients and
content and give each fight one margin, and the difference is then the labels' alone. Exit 0 all pass, 1 a
test failed, 2 could not run. MEASURED on synthetic arms (`tests/test_packcheck.py`, 200 fights × 25 runs
at K = 8, ρ 0.3 in the paired difference, 2 000 flips): var(S) 3.16 over 40 replications against
1 + 7ρ = 3.1, the fixed 3.02 failing 3 of the 40 and the cluster null's T2 one and T3 none (limits
4.9..5.9); +0.25 SD on every pair still fails T2 (S 16.9, limit 5.55) and twelve fights shifted by
1.2 SD fail T3. Partners: var(S_b) 2.89 with the same partners every round and 3.07 with partners drawn
afresh; over ten arms each, fixed against mixed, T3's limit 7.5 against 6.7 fights with the twelve shifted
fights found in all ten both ways, T1's (winner) 17.3 against 17.3 and T4's 48.0 against 48.6, so mixing
moves no test's limit by more than one fight. The size, MEASURED 2026-11-14 offline (`tests/test_packcheck.py`,
*Size*: 2 000 replications of those arms at 10 000 flips): T2 failed 3 times and T3 twice, each size at most
0.0039 and 0.0031 (one-sided 95 % Clopper-Pearson) against the stated 0.0025; the forty-replication tests
alone could not tell 0.0025 from 1 %. With every run its own cluster T2's limit is within 0.2 of 3.02 and T3's within
1 of the Binomial's on the continuous metrics; the winner's T3 limit is lower (3 against 6 over 150
fights), since its 0/0.5/1 differences reach few p values and a fight whose p cannot fall below 0.01
never counts. A copy shifted by 0.1 SD of the paired difference still fails T2 (S near 7 over 150
fights).

**The batch lane** (`engrun.py`, *THE PACKED LANE*). `calib.py engine --pack K` runs only on a generated
arena, and a generated arena only with it; K = 1 plays every run alone on the generated map, the same
lane code as the batches. It plans every run of the scenarios (`--runs`, or each block's) with the
lengths of `--prior` (the root's references by default) and the viewed-seat bound at the lever's 127,
writes the plan to the run's `plan.json` before any lane, and plays it on packed lanes, each lane-launch
at most ⌈200/K⌉ batches (`--batches-per-launch`). A packed lane-launch's setup key ends with its K, so
packed and unpacked runs, or two K, never share a launch. At its launch the lane writes the sight
records' lever `tagpu_sightcap.on` (127), and `pack_reads` asserts, each a could-not-run: each seat's
block (player `+0x67`/`+0x6B`, on the array `main+0x14357` of stride `0x118`, below the slot count
`main+0x14351`) exactly the unit limit long; Mapping on (`main+0x14281` bit 0 clear); the gravity
`synthmap.ENGINE_GRAVITY`; the line shot's append cap, the u32 at `0x49C9D2`, `pack.PROJ_CAP` (3000: the
raised limits' value; stock reads 300, DISASSEMBLED `cmp eax,0x12c` at `0x49C9D1`, so a stock-limits DLL
cannot play the packed lane); and the sight cap's imm8 at `0x48292E` the lever's N (20 stock, `cmp
eax,0x14` at `0x48292C`). The head keeps each seat's first slot, the block's length, the scan step s, S
(`main+0x1434F`) and both caps. A batch (`Lane.run_batch`): the settle; the batch's wire applied on the
held frame at T, which must answer GameTime T, nothing cleared, cell 0's units -- the apply's creates --
each in slot `first + P_0 + rank`, its delayed orders, every later cell's units and their orders in
the scheduled table, and its one run-seeds line with the batch's seed (*The streams*); the `or cells`
clause armed and the game resumed; the pause at Te and the cells' `decided` lines; the clear at Te; the scheduled creates' report (every create made) and the delayed
orders' reports summed. A batch is atomic: a refusal writes one `refused` row for each scenario it holds,
which refuses each reference. A resume reads the plan back -- refused for another K or arena, a changed
scenario file or other jobs -- and plays every batch no finished lane-launch holds (`done_batches`, which
refuses a lane-launch of another DLL, renderer, pacing, setup, arena or K, and a batch row that is not
the plan's). The lane paces as every lane does (*The pacing*): no draw on GDI, `--draw` to render every
frame; the equivalence proof's two arms are paced alike, so the packing is what they differ in.

**The cell traces** (`engtrace.py`, *THE PACKED LANE*). `Batch` reads a batch row and its window of the
recording, and hands each cell its own units' images and events by slot: a `CellRun` writes the trace an
unpacked run of its job writes -- the run's handles, positions translated back by the cell's offset (16.16:
x − dx·65536, depth − dz·65536), t counted from its own create at t0 = T + at, the creates at t = 0 in
handle order -- with no seat (`p`) rows, which every cell of a batch shares; this is why the admission
refuses a metric past `pack.CELL_METRICS`. A cell's end is read offline: the first iteration after its
create in which one of its seats has at most that seat's `end_units` of the cell's units alive, else its
limit; the trace is cut there, and every survivor's state at the end of that step is written at
t_end − t0, as an unpacked run's clear writes it. Its row is a run's (`mode` packed) with `pack` naming
the batch, its k, at, offset, site, the batch's apply and pause, the online decision and P3's headroom;
its `wall.total` is the batch's over its cells. The R checks run on the batch and carry into every cell:
R1 the empty point, each cell's creates in iteration t0 exactly in the wire's slots, types and owners,
their positions at t0 + 1 the wire's, a create in a slot of no cell, and one clear death a cleared slot;
R2 at the empty point and the apply; R3; R4 against the launch's first batch whose cell 0 is the same
scenario and swap, each seat's live count `+0x144` equal to cell 0's units of it (the apply makes cell 0
alone); R5 and R6 against the launch's first batch; R7 the lane's; R8 per cell, the batch's missing
orders within its report's cancelled.

**The P checks** run on every iteration of a batch's window, T + 1 to Te, and each one that fails, in any
batch, refuses every reference of the build (`calib.py refs build`): the bounds the planner packed by did
not hold, so no batch is known to be independent of its neighbours, and dropping the failed batches alone
would keep the outcomes that did not fire the check. The fix for a failure is a tighter bound in
`pack.py`. **P1** every live unit (`U_POS`, 16.16) inside its cell's box ⊕ 320, and none in a slot of no
cell. **P2** every projectile record below the count: its attacker `+0x52` (slot + 1) a unit of a cell,
none NULL, and its position (`+0x04`, `+0x0C`) inside that cell's zone, box ⊕ (320 + 1.5·range). **P3**
`pfcalls` steps by exactly 1 (mod 150) an iteration, and each seat examines at least S + 1 slots a step
(`pfexamined`'s difference, or its value on the step `pfcalls` wrapped to 0; S = `main+0x1434F`): its
cursor swept every slot, so every search it started finished in the step; the least headroom is kept
in each cell's row. **P4a** the record count plus each live unit's armed slots and armed burst slots
(`unit_facts`' `slots` and `bursts`) plus the burst parents (`+0x60`, a word, ≠ 0) within the cap read
at the launch. **P4b** each seat's stored energy and metal (`+0x8C`, `+0x98`) at least half its
storage. **P4c** no death of the viewed seat meets as many as the sight cap of its others within 61
ticks before it. **P4d** the wreck pool's free head (`RK_FEATURES`) never −1. **P4e** each cell's records
within its projectile bound. **P5** each cell's end at or before the batch's pause: a verdict of the cell's
own (`CellRun.finish`), which the batch records as passed and never overwrites, and which
`engtrace.P_CHECKS`, the one list `refs build` reads, names with the others.

**The references.** A packed cell makes a reference run as an unpacked run does, with its `pack` beside;
a reference's `setup.pack` is its K. A scenario's runs are all of one K, and `refs build` refuses to
replace a reference of another K, or an unpacked one, at the same path (`refs/<sid>/<commit>.json`): a
packed generation and the unpacked one it is compared with go to two roots (`TACALIB_DIR`), which
`packcheck.py` then pairs. `calib.py refs build`'s cost lines share each batch's seconds over its cells.
Its line for each lane-launch names two counts, which agree only at one run a batch: the **game ticks
played**, the head's `ticks`, each batch's apply-to-pause window summed (the count `calib.py engine`
prints too), and the **run ticks**, every run's or cell's own length summed, the cells of a batch
overlapping in its window. MEASURED 2026-11-14 in a K = 4 land lane-launch of one batch: 1 008 game ticks,
3 097 run ticks (904 + 966 + 948 + 279), the window equal to max_k(at_k + length_k); at K = 1 the two are
equal, the pause being the one cell's end.

**The identity control** (`calib.py identical A B`) reads two engine runs and holds every run both hold, by
scenario and index, to the same unit rows and events at every t and to its start checks, the R checks
and the P checks a cell carries; exit 0 when every one is identical, 1 otherwise. It separates a packing
channel from the shared inputs: a fight that draws neither the simulation RNG nor the wind must play the
same packed as at K = 1, so one difference names a channel the RNG and the wind cannot explain. The
control is `duel-t1-assault-assault-cheapest`, ARMROCK against ARMROCK: it moves and fires seven rockets a
side, and KBOT_ROCKET is line and selfprop, which the wind never reaches. MEASURED: its runs 0 and 1
were identical at K = 1 and packed at k = 1, a site other than the K = 1 one, and its lengths
(966, 962) equal the retail references'. Eight byte-identical copies under distinct names, which the
planner takes as eight fights sharing one perturbation, played at K = 8 put the control at every
k = 0..7, a copy at each, and the same eight at K = 1 are the other arm; the K = 8 batch and the K = 1
batches draw from different seeds (`pack.batch_seed` is a function of a batch's jobs), which the control
does not read. Not yet played at K = 8.
MEASURED 2026-11-14, `identical` over the K = 1 and K = 4 land arms of the first packed run (8 runs): the
control's two runs identical, and the six of fights that draw the RNG or ride the wind differing first at
t 164 to 218, none before the run's own first RNG draw or a shell's flight in another wind.

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
- A defence fight's structures have no orders, so they fire through their standing Guard_NoMove and the
  weapon scan; Guard_NoMove's search (`0x40AD80` with a 640 radius [INFERRED]) and `0x40B7B0`'s range and
  sight limits for it are not read, so whether sight limits a tower in these fights is not established, and
  v2 does not model sight.
- The queue protocol's re-targeting (V4) and the applier's queued chains (V3) are not yet measured in a game.
- The per-mod references hold `group-000`..`-099` of each generation's 150 group fights (*On a compat
  setup*); `calib.py check` of tasim against them is not yet run. They were made by the DLL of `a236f324`,
  so the brain's later commits make every one of them stale by the rule (`impai_*`, `tagpu_impai.c`,
  `tagpu_worldq.c`), though no ImpAI seat plays in a calibration fight.
- `calib.lane_mib` is fit on lanes of at most 93 MiB recordings; the mods' longest lanes are past it
  (Escalation's group lanes, up to 451 MiB and 467 000 ticks, a fit of 11.5 GiB), so such a lane may be read
  twice before its figure holds. The reader keeps the whole decoded recording and every run's trace until the
  stream ends: read as a stream, run by run, a lane's memory would not grow with its length.
- The batch lane has played one small run (2026-11-14: four land fights and four naval fights, two runs
  each, at K = 1 and K = 4 on two lanes): every R and P check passed in every run and cell, and the
  identity control's fight was identical in both arms. The equivalence run, the identity control at
  K = 8, and the packed lane's speed against the unpacked one over a corpus are not yet played. A packed
  cell's metrics are compared with its unpacked run's only across processes (two lane-launches), so the
  comparison rests on the launches being repeatable; P1..P5 read one recording and need no second process.
- *The streams* are built and unit-tested, not yet played: the order-independence run (the first packed
  run's K = 4 land plan played with one batch a lane-launch and with two, at `--j 1`: `calib.py
  identical --exogenous`, every cell's trace and its `exogenous` equal in both), two seeds' wind schedules
  differing, and `tareplay.py landing`. The seeding moved before the creates, so references made before it
  are stale (`check.py` names `tagpu_scenario.c`) and U is generated again before the equivalence run.
- The wind's draw at the apply is built and unit-tested, not yet played (V1 and C8 again).
- The wind's changed flag `+0x37EE2`, which the unit tick reads through `0x437910` to start a wind
  generator's COB SetDirection and SetSpeed, is set by the apply's draw and seen in step t0 + 1 by the
  batch's cell 0 alone. A later cell's first step never sees it: the next change is the apply's GameTime
  + 30·n, drawn in step T + 30·n + 1 and seen in step T + 30·n + 2, while cell k's first step is
  T + 30·k + 1. So a wind generator in a cell past the first starts its COB at the next change packed and
  at its first step at K = 1; a fight with one there is not held to its K = 1 run by construction.
- A batch's cell admits no create but its own: a fight whose units build would fail R1, which the
  admission's attack-and-move rule keeps out.
- The engine's own class maps on the walled maps are not compared with the core's (`classcheck.py` on
  each), so the void-is-0 and flood results above are the core's.
- `predfit.py` accepts a packed reference and cross-checks its lanes' gravity with
  `.data/impai/map-gravity.json`, which is to name both generated maps at 8155; without those rows the
  lanes' own read stands unchecked.
