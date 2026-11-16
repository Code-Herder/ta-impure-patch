# The bar

What ImpAI must win before it becomes the default AI. The owner judges the stock AI too weak to lose to, so the bar is every game, not a majority.

## The rules

- **Every level plays OTA AI at Hard** (owner's decision, 2026-11-14): **Hard wins 100 of 100** games (`hota`) --
  a single loss is a defect to diagnose and fix, not noise to average away; **Normal takes at least 90 % of the
  points** (`noh`, 120 games); **Easy takes at least 50 % of them** (`eoh`, 120 games), the point estimate judged
  and its t90 interval reported. Easy has no ceiling: the owner accepts an Easy that takes more than the earlier
  band's 75 %, as long as the levels stay ordered -- Easy easier than Normal, Normal easier than Hard, which the
  ladder below measures (owner's decision, 2026-11-15, after round 3 measured Easy at 0.996 of the points); `judge` still prints
  where a point estimate stands against 0.75, for the calibration's rows. A game's points are 1 for a win, ½ for a draw, 0 for a loss. The earlier rule, each
  level against OTA AI on its own Difficulty in 100 of 100 (`eota` Easy against Easy, `nota` Normal against
  Medium), is what `judge` still asks of those pairs when they are played.
- **Adjacent levels stay distinct:** ImpAI Normal beats ImpAI Easy, and Hard beats Normal, in at least 90 of 100
  games (`ne`, `hn`). A game won by value at the limit counts as a win, a draw does not, and a pair with 11 games
  not won has failed for certain.
- **A game nobody has won at the cap** is scored by value at the limit in every pair but `eota`, `nota` and
  `hota`: a 1.3× lead in the value of all finished units alive -- army and economy, commanders counted -- wins, and
  anything less is a draw (owner's decision, 2026-11-14, the same rule as the tuning tournament's).
- **Easy stays beatable by a new player.** No script can measure that, so the owner's playtest is the judge.
- **No stalls:** in no game do ImpAI's army or economy stop for more than two game minutes.

Note that ImpAI Easy plays at full income with handicapped behaviour, while OTA AI on Easy plays at 50 % income ([ImpAI for the player](player.html)).

## The games

- 1v1, on five maps: land, sea, mixed, small and large. The levels' rows are chosen on five others of the same
  kinds, the development pool, so no row is chosen on the maps that judge it.
- Both sides played on Arm and on Core.
- Commander death ends the game.
- A game of `eota`, `nota` or `hota` that nobody has won after **45 game minutes** counts as a **failure**, not a
  draw; in every other pair a game at that limit is scored by value (above).
- Every game plays from **its own seed**, so the 100 games of a pair are 100 games, not a few repeated.
- **Maps, not games, are the unit** of a verdict on a share: each map's mean points is one observation, since
  games on one map are correlated (the protocol of Nanolathe's Modern AI, [decisions](decisions.html)).
- Played in the engine at turbo k = 1, one tick a frame (rung 2 of the [testing ladder](testing.html)), under the
  run lock.

## The harness

`tools/impai/bar.py` (`ids`, `play`, `row`, `judge`, `check`; `test_bar.py` holds its game list and seeds, the
blocks, the new pairs and their maps, seating, start schema, stall rule, the holds' end at the cap, what a result
reads from the level's end, the judge with the value at the limit, the points, the interval, the targets, the
ladder's curtailment and the ordering's looks, the row checks with each yard verdict, `row`, `--setup` and its
commanders, the cheat flag and the judge per setup, a game's end on a fake instance in each way a game ends, a level
not seeded from its game's seed, a persona row's lever, and `play`'s stop and curtailment on fake lanes, offline;
under the shared venv also the bounds' integration and a recorded game's value at the limit). A pair is a list of
game ids `bar-<pair>-<map>-<sides>-<n>`:

| pair | A | B | Difficulty | games |
|---|---|---|---|---|
| `hota` | ImpAI Hard | OTA AI | Hard | 100 |
| `eoh`, `noh` | ImpAI Easy, Normal (the level lever) | OTA AI | Hard | 12 blocks, 120 |
| `ne`, `hn` | ImpAI Normal, Hard (the level lever) | ImpAI Easy, Normal | Hard | 10 blocks, 100 |
| `p:<row>` | ImpAI Hard on a persona row (the persona lever) | OTA AI | Hard | 4 blocks, 40, on the development pool |
| `eota`, `nota` | ImpAI Easy, Normal | OTA AI | Easy, Medium | 100 |
| `dry` | OTA AI | OTA AI | Hard | 10 |

`hota`, `eota` and `nota` are the five maps × four side pairings × five games, the player under test at the map's
first start in games 1, 3 and 5; the dry run is ten games; the others are played in blocks.

- **The blocks**: a block is every map of the pair's pool played once from each start, 10 games on five maps.
  Block b is game 2b − 1 (A at the map's first start) and 2b (its second) of each map, with the side pairing
  `BLOCK_SIDES[(the map's index in MAPS + b − 1) % 4]`, so a block holds all four side pairings and a map plays
  each once every four blocks. `ids` lists a pair's blocks in order, so its first k blocks are its first 10 k games
  and a list cut there is whole blocks; `ids --blocks N` lists more than the pair's run (`hn --blocks 12` for the
  ordering's third look). [INFERRED] A block is one runner session at ten lanes: a game to the cap took 87.8 to
  194.4 s (*The rate*).
- **The persona rows**: the calibration ladder L0 (softest) to L5 (Hard's, no limits), each the nine integers the
  persona lever `tagpu_impai_persona.on` takes (`level_lever.py --persona`; L1 and L3 are the DLL's Easy and
  Normal, `IMPAI_PERSONAS` in `impai_react.c`), in `PERSONA_ROWS`. A calibration game writes its seat's row to
  that lever beside the level lever's `hard`, and could not run unless the seat's `impai: persona` line at its init
  names the same nine integers `from=lever`. Each result carries every ImpAI seat's persona as logged
  (`persona`). A row between two is a new entry under a new name; a row's games resume and are judged only on its
  own nine integers.

- **The seed**: each game's own, a function of its id alone -- the first four bytes of the SHA-256 of
  `bar.py seed <id>` (`game_seed`) -- written as the scenario's `seed`, which tacli puts on the wire. Every
  tareplay Worker writes the harness's switch `tagpu_seeds.on`, under which the scenario's clearing apply seeds
  the simulation RNG, the game thread's CRT stream, the wind and the meteors from the wire's seed and logs
  `enginefix: run seeds before the creates: the wire's seed <n>` (`tagpu_seed_run`, [engine patches](../modules/engine-patches.html),
  §2.6c). A game is the same game whenever its id is played, and no two ids of the pairs and the persona rows
  share a seed. A game
  whose log does not name its own seed so could not run, each result carries its seed, a resume replays a result
  of another seed, and `judge` refuses a result with none. Why: with the level's fixed seeds and no run seed,
  every game of one map, side pairing and start was one game -- games 1 and 3 of each id were identical in 42
  pairs of 42 (MEASURED 2026-11-14, b7 round 2, whose 242 logs carry `test seeds` and no `run seeds`), so a pair's
  five games a map and pairing were two distinct games.
- **The maps**, one of each kind by the rule the specification states (at least three starts, no uniform metal;
  land, sea and mixed from the middle third of the 90 such retail maps by area, small from the smallest third,
  large from the largest, each the median of its class): **Pincushion** (land), **Red Triangle** (sea, 75 %
  under sea), **Trout Farm** (mixed, 40 %), **Lava Mania** (small), **Plains and Passes** (large). These five
  judge (`BAR_MAPS`). **The development pool** (`DEV_MAPS`), each the runner-up of its kind in the same survey, is
  where the calibration rows play and the tuning tournament's tier B: **Acid Foursome** (land, 3 % under sea),
  **Steel Jungle** (mixed, 44 %), **Luschaven** (sea, 65 %), **Gasplant Plain** (small), **Lava Alley** (large).
  MEASURED 2026-11-14: each of the five reads ten start positions from its `.ota` through tavfs, and
  `bar-p:L0-acidfoursome-ac-1` played on Acid Foursome to its cap.
- **A game**: `scenarios/impai/bar.json` filled -- an idle human with no unit, the two players in seats 1 and 2,
  each commander placed at its start position from the map's `.ota` (the schema a three-seat skirmish reads), base
  storage 1000/1000, the end test off. A lane, a held tareplay Worker at turbo k = 1, holds the level every 3000
  ticks (`STEP`), or earlier at the first frame visit at which seat 1's or seat 2's units alive `+0x144` is 0 (`pause at T or
  units <s> <= 0`, `tagpu_replay.c` THE PAUSE AT T, which at k = 1 lands on the visit after the step in which the
  count fell), and peeks both: the game ends when one is 0, or at GameTime 81 000. Both at 0 at one look -- both
  sides gone in one tick -- is a draw, judged as a game at the cap. A launch the engine does not reach is "could
  not run", never an outcome.
- **The end of a game** is the engine's own end test, the same for every setup whatever its menus: a mod's
  in-game menu need not hold the stock Exit -- Escalation's `TABMENU.GUI` has none, TA Zero's `GOKMAIN2.GUI` no
  route to one, and both lost their games' level ends and recordings to it (MEASURED 2026-11-14). Once the outcome
  is read, on the held level, the end scenario (`end_doc`, written beside the game's as `<id>.end.scenario.json`)
  clears every unit, places one unit of the human seat -- its side's commander -- and arms the end test
  (`objective on`; `bar.json` sets it off). The level runs on: the skirmish victory `0x490230` answers 1 once no
  record the controlled player does not ally has a live unit ([sim](../engine/sim.html), *Seating a level*), and
  the level is torn down 151 to 180 ticks later, inside the bound `Log.wait_end` holds the wait to (`tareplay.py`,
  THE LEVEL'S END). The teardown `0x491B60` ends the recording and writes ImpAI's level-end lines; the harness
  waits for `impai: handicap rows`, `recorder: ended after` and the writer's `closed`, then stops the game through
  its own instance (`Worker.stop(hard=True)`: `tacli stop --hard`, the PID the instance's prefix runs, then its
  wineserver; it fails unless the game is gone). So no game is left at the post-game screen holding the
  instance's registry store when the lane launches its next -- the failure `is running: its registry store is the
  DLL's until the game exits` of the first mod runs. A game whose level does not end so could not run. The unit
  kept is the human's, so no AI seat holds a unit in the ticks the end test takes, and the pulses and task lines a
  result reads are the ones logged before the end's apply (`read_result`'s `upto`); a seat that ordered no second
  factory is stamped by the level end with its own GameTime, not the outcome's, and each result names that
  GameTime (`level_end_at`). **It gives the in-game menu's results**: MEASURED 2026-11-14 on the base game,
  `bar-nota-pincushion-ac-1` (won at GameTime 32 853) and `-ac-2` (at the cap 36 000), each played to its end by
  the menu's Exit (`tareplay.Worker.quit`) and by this end, gave results equal in every field but `level_end_at`,
  177 and 165 ticks later.
- **What a result reads from the level's end**, for each ImpAI seat: `end`, from its `impai: eco` and
  `impai: mil … level=` lines -- its units completed, its army units completed by class (`army`, ImpAI's own
  classes `lraider` … `ascout`, and `army_built`), its factories completed by their build tech, the highest
  tech their constructors build (`factories`, `IMPAI_TINFO.ctech`, `"3"` holding 3 and above), its water-only
  factories completed (`yards`), the sea plants it ordered, its attack waves, raids and scouting runs begun, its
  naval judgement and its tech-up income `tech_m`; `first`, the GameTime of the first `impai: mil seat= t= attack=
  raid= scout=` line that began an ATTACK, a RAID and a scouting run, `null`
  for a kind it never began; `income_max`, its best metal income a pass over a pulse minute (the rise of
  TotalMetalProduced between two pulses, over the passes between them); `second`, its second-factory decision
  from its `impai: second` line (*The row checks*); `persona`, its persona from its `impai: persona` line at its
  init, the nine integers in the lever's order and whether they came `from=lever` or `from=level`. `judge`
  prints, per pair and side, in how many games each first task came, its range and median, and the games
  without one.
- **The value at the limit**: a game at the cap -- not a draw at one look -- carries every seat's value at the cap,
  `value: {A, B: {live, t}}`, read from its recording at the cap's own iteration (`recording_evidence` with `at`,
  `value_at`): the value of each seat's finished units alive -- army, economy and commander alike, a unit's type's
  metal + energy / 60 (`IMPAI_TINFO.value`) by the level's own type table (`tacli units`, `Lane.type_table`) --
  over the units the recording holds alive, by owner `+0xFF`, type `+0xA6` and fraction left `+0x104` (0.0
  finished; [units](../engine/units.html)). No log line lists a seat's units: the `units:` heartbeat carries
  only `alive=`, `onscreen=`, `eye=` and `me=`, and only an ImpAI seat writes a pulse, so the recording is the one
  source for every seat, OTA AI's included, with no DLL change. The harness holds the level to the cap + 15 before
  the end's clear, so the clear falls 15 ticks after the iteration the value reads; a live unit of a type the table
  does not hold makes the game "could not run". MEASURED 2026-11-14 on the ten kept recordings of the base game's
  capped bar games (`eota`, `nota`, `hota`, in `.data/impai/runs/broad/198899ef/base` and the brain's runs):
  at two pulses a game, mid-game and the cap's minute, the ImpAI seat's value so read equals its own pulse's
  `vlive`, 20 of 20, within 0.75 -- ImpAI's float32 sum over up to 677 units against the reader's exact one; and
  at the cap each seat's units alive in the recording equal the harness's look there (`+0x144`), OTA AI's seat
  included, in 10 games of 10. The pulse is the one independent check of a seat's value; OTA AI's seat is
  checked by its count of units, its value being read by the same code from the same table.
- **What a result keeps for a diagnosis**: its last two looks (`looks`: the GameTime and each seat's units alive,
  and at the last each seat's commanders lost, the word `+0x106` the destructor raises at `0x486996`,
  [units](../engine/units.html)). A game A did not win (lost, at the cap, a draw) or in which an ImpAI seat
  stalled, and a game that could not run, keeps its instance log from the launch on, `<id>.tagpu.log` beside its
  JSON. A bar game of the first kind keeps its recording too, moved to `<id>.imprec` beside its JSON with the
  path in the result's `recording` -- once its writer's `closed` line is in the log, which is what says the file
  is whole; a recording to keep that was not made or not closed makes the game "could not run". The
  `.imprec.lock` beside a recording is no sign of an open file: the lock file stays beside every recording that
  exists, by design (`tagpu_recfile.h`, THE PATH LOCK). Every other game's recording is deleted once the cheat
  flag's evidence is read from it, and its `recording` is `null`; the tuning tournament's games (`tourney.py`, through `play_game`) keep none. A draw reads
  as both commanders dying (`+0x106` 1 for each; [INFERRED] each death a kill-all of its side, whose units die as
  self-destructs, so its blasts can reach the other commander) or as something else, from the log's `units:`
  heartbeats.
- **Why k = 1, and no `--turbo`**: the turbo's k ticks a frame are not the same game drawn less often. The step
  `0x495490` runs its tail once a call, not once a tick -- the temporary sight records' expiry `0x482130` among
  it -- so a sight record outlives its deadline by up to one batch and where the batches fall changes what units
  see and do ([sim](../engine/sim.html), *The batch is part of the result*: speed 20 against a fixed k = 2 parted
  in 1 272 and 1 383 unit fields). k = 1 is the batch a player at normal speed gets; a bar game at k > 1 would be
  judged on another game. The holds would stay exact (the pause at T bounds the batch to end at T), the end's
  units clause would not (a batch of k ticks ends where it ends).
- **The rate** (MEASURED 2026-11-14, b7 round 2: 242 games, GDI, no draw, k = 1, held every 3000 ticks, two
  lanes a piece): a game run to the cap took 87.8 to 194.4 s from its launch (36 games), the slowest of all 242
  games 194.4 s; one game to GameTime 18 000 took 25.8 s. A piece of four games on two lanes therefore fits a
  ten-minute bound.
- **The renderer**: every result names it, a game that could not run included; `judge` refuses a result that
  names none, and a directory of two renderers.
- **The pacing**: a GDI game runs NO DRAW (`tagpu_nodraw.on`, `src/tagpu_nodraw.c`): a running frame whose batch
  was the turbo's skips the game screen's draw and runs in its place DrawUnit's repose of the units the draw
  would reach, the triggers, ImpAI's digests and the roster; held frames, and every frame whose scoreboard could
  write its rank, draw. `--draw` renders every frame, and `--gpu` implies it. Every result names its pacing
  (`pacing`: draw or nodraw) and, under no draw, the level's account (`nodraw`); a resume refuses a result of the
  other pacing. MEASURED 2026-11-13: a bar game's cost a tick fell from 1.38 ms drawn to 0.60 ms under no draw.
  Screenshots and `tacli ui` of a game under no draw show the last drawn frame. That an undrawn frame reposes
  exactly what the draw would is the replay test group `nodraw-shadow`'s check (THE SHADOW).
- **The stop**: SIGINT or SIGTERM during `play` starts no further game, stops each game still playing through its
  own instance (`tacli stop`, by the PID its prefix runs), writes it as could not run with the reason "stopped",
  closes the lanes, releases the run lock and exits 2. A game not started is not written, so the same `play` into
  the same directory resumes. One thread a lane takes games from one queue, so no two games share a lane.
- **Commander death**: `bar.json` sets `setup.commander_death: true`, which `tacli scenario load` sets on the
  SKIRMISH screen and reads back from `ActiveCommanderDeath` (`main+0x37EF6`); each result carries the level's
  value as the harness peeked it. A veteran CORKROG can outlive its side's kill-all
  ([units](../engine/units.html)).
- **The stall rule as built**: from ImpAI's pulse lines, a window of 3600 ticks starting at a pulse after GameTime
  1800 and ending before the game's end in which `built` and `eroots` both did not rise, or in which the seat
  held an army unit and `mroots` did not rise.
- **Mod setups**: `play` and `row` take `--setup <name>`, a compatibility setup (`tools/compat/setups.json`),
  and play the same ids, seeds, holds and end on the setup's own folder as the compatibility suite builds it
  (`engrun.SetupWorker` over `tacompat.prepare_wine`); a setup whose goal is not Impure running is refused. Each
  result names its setup (`setup`, `null` for the base game); one process and one directory hold one setup's
  games -- a resume replays a result of another setup, and `play` refuses a directory that holds one -- and the
  default directory is `.data/impai/bar/<setup>-<time>`. A setup's commanders are its own sidedata's, `[SIDE0]`
  for `arm` and `[SIDE1]` for `core` (tacli's side numbering), read through the profile of the setup's running
  image (`tavfs.profile`: the folder its exe or Patch Loader names, the archives it mounts), as are the maps'
  start positions. TA Zero's are `GoKCommander` and `ArmCommander`, the commanders its compatibility ImpAI stage
  recorded the engine giving those sides (results of 2026-11-13); `escalation`, `prota-4.8`, `mayhem-11.3.0`,
  `twilight-2.0b98`, `loader+tadr-ota` and `392+tadr-2026.8.6` name `ARMCOM` and `CORCOM`.
- **The cheat flag**: a setup's stock AI is judged a cheat by evidence from its own games, never by a list. Each
  result carries, from its recording (`tools/replay/tarec.py`): `income_t1`, each side's metal and energy
  production (player record `+0x9C`, `+0x90`) at the recording's first iteration, GameTime 1, when each seat holds
  its commander alone; and `ai_only`, each side's completed units (a death image whose fraction left `+0x104` is
  0.0, [units](../engine/units.html); the end's clear kills every unit, so each unit completed dies once in the
  recording) of a type on no build page any commander's menus lead to -- tech 0 in `tasim_run roles` on the
  lane's own folder, the file filler `roles_check.py` holds to the engine's. `judge` flags a setup in which an
  OTA AI seat's income at t=1 is above an ImpAI seat's **on the same side** -- more than a player of that side
  gets; the stock handicap (× 0.7 at Medium) or a mod that drops it is no cheat -- or in which an OTA AI seat
  completed an AI-only type. Sides are compared with themselves alone: TA Zero's commanders make 6 and 8 energy.
  MEASURED 2026-11-14, pair `nota` (OTA AI at Medium), at t=1: OTA AI made 0.7 of ImpAI's commander income on the
  same side on the base game, `loader+tadr-ota`, `392+tadr-2026.8.6`, `twilight-2.0b98` and `tazero-alpha5`; 1.0
  of it on `prota-4.8` and `mayhem-11.3.0`; and 2.0 on `escalation` (4 metal and 100 energy against 2 and 50).
  TA Zero's OTA AI completed `_AI` types on no page (`ArmT1Mex_AI`, `GoKT1AF_AI` and 19 more in two games); 60 of
  TA Zero's 270 types are on no page, and 12 of the base game's 278 (`ARMBEAC`, `ARMGATE`, ...). Not judged: a
  bonus a mod gives only later in the game, or one that does not raise the commander's income at t=1 and builds
  no AI-only type -- neither shows in this evidence.
- **The verdict per setup**: `judge` reads each setup's games apart, the base game's among them, and prints for
  each whether it is stock or cheats (with the evidence) and each pair's wins, losses and caps; only the stock
  setups' games count toward the verdict, and with none it could not run.
- **The verdict**, pair by pair:
    - `hota`, `eota`, `nota`: the pair's 100 games, every one a win for A and none with a stall.
    - Every other pair scores a game at the cap by the value at the limit (`by_value`, the one rule wherever a
      cap is scored by value; `tourney.py`'s `value` rule calls it, a draw going to seat B, the incumbent): a seat
      at least 1.3 times the other's value, and above it, wins, and otherwise the game is a draw. A draw at one
      look -- both sides gone in one tick -- is not at the limit and stays a draw. A result with no value at the
      cap itself leaves its game unscored and its block incomplete.
    - **The points and the interval**: a block pair reads its blocks complete -- every game played and scored --
      in order; each map's mean points is one observation, the estimate their mean, with the two-sided 90 % t
      interval over the map means beside it (t at df = maps − 1).
    - `eoh`: its 12 blocks' points in 0.50 to 0.75, and no game with a stall. `noh`: at least 0.90, and no game
      with a stall. A calibration row: its 4 blocks' points read against both -- above Easy's band a harsher row,
      below it a softer one; below Normal's floor a softer row -- and no verdict, since one row can be a candidate
      for either level.
    - `ne`, `hn`: 90 wins of their first 100 games. **The curtailment**: at the 11th of those not won the pair has
      failed for certain; `judge` says so whatever is left, and `play` starts none of the pair's games after it
      (`--no-curtail` plays on).
    - **The ordering**, for `ne` and `hn`, printed beside the ladder and judging nothing (the verdict left if the
      ladder is relaxed): three looks at the pair's first 4, 8 and 12 blocks, each the t of the map means against
      a share of ½ and the O'Brien–Fleming t bound of a two-sided 10 % test over three looks on five maps (6.398,
      3.099, 2.253; on ten maps 4.010, 2.458, 1.918; `OBF_T`, which the venv's test checks by integrating the
      joint normal). "better" or "worse" at the bound, "futile" at an earlier look whose conditional power under
      the current trend is below 0.20, read to the first look that gives one.
- **The row checks**: `bar.py check b4|b5 DIR…` scores A2b's rows (the A2 specification's section 9) from the
  results: ImpAI Hard against OTA AI Hard (`hota`), every game played to the row's bound -- b4 27 000, at least
  four games on two maps; b5 36 000, a game on each of the five maps. `bar.py row b4|b5 --lanes L [--out DIR]`
  plays the row's own games (`ROWS`: b4 Pincushion ac-1 and ca-2, Trout Farm aa-1 and cc-2; b5 one a map,
  Pincushion aa-1, Red Triangle ac-1, Trout Farm ca-1, Lava Mania cc-1, Plains and Passes ac-2) to its bound into
  a directory of their own, then checks them there; `play --cap` and `check` still do it by hand. **b4**: an
  attack wave begun (`first` ATTACK), a scouting run begun and no stall. **b5**: army units of at least
  `ROLES_MIN` (2) classes completed; a factory of build tech 2 or above -- one whose constructors build tech 2 --
  completed wherever `income_max` reached `tech_m`; and the yard, **judged at the second-factory decision**, the
  one decision at which the naval rule acts (the sea plant is wanted only beside one factory, and `factory_max`
  is 2 at every level): its `impai: second` line (*the second-factory line* below). On Red Triangle, and wherever
  ImpAI judged the map naval at that decision: **yard ordered** when it chose a sea plant; **naval, no site**
  when it chose a land plant because the decision's sea search found no candidate at all -- met, and listed by
  name under the games; **yard missing** otherwise -- a land plant after the sea plant was given up on its
  refusals or with no search, or no second factory by the level's end. Elsewhere the yard is not asked. The
  level end's `naval=` is not read: it is the last decision's judgement, which can flip after the factories are
  placed, on a structure frame seen late (the b5 regression of 2026-11-14 on Trout Farm). Exit 0 when every
  game meets each, 1 when one does not, 2 when a game could not run, is of another pair or bound, carries no
  level-end lines of A's or, for b5, no `impai: second` line of A's, or the games are short of the row's. The
  holds of every game end at its cap exactly (`next_hold`), whatever the cap. **Not established**: b5's "an army
  of each wanted role" -- which classes a seat wants follows from its table, its picture and its factories' menus
  at each decision, and no line logs them, so the check asks for a composition of two classes and prints the
  classes completed.
- **The second-factory line** (`tagpu_impai.c`, THE SECOND FACTORY): once a level a seat, at the first decision
  that orders a factory while the census counts one -- a sea plant, or a land plant the choice answered for a
  further factory -- `impai: second seat= gametime= naval= chose=sea|land sea_site=found|none|refused|-`: the
  military's naval judgement the economy read (-1 never judged, 0 land, 1 naval) and the decision's sea search,
  a site found, no candidate at all (`sea_search`'s -1), given up after 16 counted refusals, or `-` when the
  decision searched none. A seat that ordered no second factory logs `chose=none sea_site=-` at the level end with
  its last judgement. Logging alone: no simulation state. MEASURED 2026-11-14 on GDI under no draw:
  `bar-hota-redtriangle-ac-1` to GameTime 18 000 logged `impai: second seat=1 gametime=5947 naval=1 chose=sea
  sea_site=found`.
- **`play_game(game, lane, out_dir)`** is the tuning tournament's game runner (`tools/impai/tourney.py`): one game
  of its spec on one lane, with each seat's table copied into the game folder and named by
  `tagpu_impai_table.on`, its result carrying each seat's table hash from its `impai: table` line and, at the cap,
  each seat's value at the limit.
- **The first mod games** (2026-11-14, GDI, no draw, pair `nota`, `--cap 36000`, one lane a setup): on
  `escalation` and `tazero-alpha5`, `bar-nota-pincushion-ac-2` and `bar-nota-lavamania-ca-2` each, both on one
  lane, ended by the end test with their level-end lines and closed recordings, the lane's game gone before its
  next launch, 28 to 51 s a game. ImpAI lost all four; `judge` named `escalation` a cheat by its income and
  `tazero-alpha5` by its AI-only types, and the base game, two games, stock.
- **The paired outcome bar** `tools/impai/paired.py BASE TEST [--pairs N]` judges a change by its effect on the
  same games: two arms played by `play` on the same ids, BASE without the change and TEST with it, each
  recording measured by M1's `measure.py` (`.data/impai/runs/a3/tools/`, `run_all.sh` over a list). Its seat-games
  are ImpAI's seats against OTA AI only -- against ImpAI the other seat changes too -- each paired with the same id
  and seat of the other arm, whose result must name the same seed. Per seat-game, from the deaths of finished units
  from GameTime 30 on: **lost**, the seat's army metal killed by another player; **killed**, the other players'
  army and constructor metal the seat killed. The bar (the owner's, 2026-11-14, [decisions](decisions.html)):
  over at least 60 paired seat-games, the mean difference TEST - BASE with a 95 % percentile bootstrap interval
  (10 000 resamples from a fixed seed), (a) lost falls -- the interval's upper bound below 0 -- and (b) killed
  falls by no more than 10 % -- its lower bound above −10 % of BASE's mean killed. The exchange and the
  differences by level are printed beside it and not judged. Exit 0 pass, 1 fail, 2 could not judge: a seat-game
  with no partner, seeds that differ, an id measured twice in one arm, a recording not read to its end, a result
  naming no seed, or fewer than N pairs. `test_paired.py` holds each count, each refusal and each verdict
  offline. It judged the mend, a retreat of the damaged army to be repaired, which failed it twice and was removed
  ([decisions](decisions.html), *The mend*).
- **What has run**: b7's round 2 (2026-11-14) played half of each ImpAI pair -- games 1 to 3 of `aa` and `ac`,
  1 and 2 of `ca` and `cc`, on every map: 50 a pair, 45 and 47 of the ladder's -- with the level's fixed seeds and
  no run seed, so its games repeat (*The seed*), and no result names a seed: `judge` refuses them. Its verdict was
  COULD NOT RUN, every pair short of its 100, and its row checks COULD NOT RUN, its `hota` games played to the
  bar's cap rather than the rows' bounds. The dry run of 2026-11-13 ended one game, Trout Farm 1, with both seats at 0 in one 300-tick look
  at GameTime 30 000 (22 and 31 units at the look before), on a level held there by the harness's own `pause at
  30000` and nothing else; its log was not kept, so whether both commanders died in that step is not
  established. The new pairs' first games (2026-11-14, GDI, no draw, `--cap 18000`, three lanes):
  `bar-eoh-pincushion-aa-1`, `bar-noh-redtriangle-ac-1` and `bar-p:L0-acidfoursome-ac-1` each ran to its cap in
  27.7 to 31.1 s; ImpAI's persona lines named Easy's and Normal's rows `from=level` and L0's `from=lever`; each
  game's value at the limit was read for both seats from its recording (A 34 673, 35 536 and 33 475 against OTA
  AI's 33 796, 27 838 and 29 581), all three draws by value. A smoke run of the pairs, not a calibration.

## The paired margin test

Once ImpAI wins every fair game against OTA AI, a count of wins no longer tells two DLLs apart: both arms win 120
of 120. The time a game takes to win does, in every game, and on one seed both arms play the same game until
their code differs. `tools/impai/margin.py` judges a candidate DLL (TEST) against the champion (BASE) by it, a
sequential test that stops as soon as the games decide; `test_margin.py` holds it offline.

- **The games**: both arms play the same ids, ImpAI against OTA AI at Hard, through `bar.py play --dll` -- the
  bar's ids, seeds, seating and results. The ids are the bar's `hota` ids in a fixed shuffled order (seed
  20261115), then its `noh` ids, 200 in all, so every run sees the same sequence; they are played in blocks of 16,
  both arms of a block at once, each arm under the run lock with its own lanes.
- **The measure**: a seat-game's time to win -- its `end_tick` when ImpAI won, the cap (81 000) when the game
  reached it (a draw at one look included). **A TEST loss fails the test outright**, as Hard's 100 of 100 rules;
  a BASE loss against a TEST win or cap is a pair TEST won. A pair scores 1 when TEST's time is shorter by more
  than 300 ticks, 0 when longer by more than 300, and otherwise -- inside the band, or both at the cap -- 0.5, a
  tie. The band: a decision at Hard is 75 to 150 ticks, and identical code ends on the same tick.
- **The pairing**, as `paired.py` pairs: by id, each result stating its seed; seeds that differ, a result with no
  partner, an id twice in one arm or a game that could not run, and the test could not judge.
- **The test**: a GSPRT on the mean pair score, the normal approximation engine-testing frameworks use: over n
  pairs of mean m and variance v, LLR = n (s1 − s0)(2m − s0 − s1) / (2v), against ±ln 19 (±2.944, α = β = 0.05).
  **IMPROVE** (a change meant to make ImpAI stronger) tests s = 0.50 against s = 0.60: accept, TEST is better;
  reject, it is not measurably better. **NOT-WORSE** (a fix batch) tests s = 0.40 against s = 0.50: accept, TEST
  is not worse -- the batch's pass; reject, it is worse. The hypotheses are 0.10 apart because the test's job is
  a clear slowdown, not a 5-point one: a batch already fails on any loss in the regression set and the 40 `hota`
  games, and the full bar runs at milestones. A tie stays in at 0.5 and lowers v, so a run of ties is
  evidence that the arms play alike; a sign test that drops ties cannot decide identical code at all. The ratio
  is read after each whole block, in order: the first block at which it reaches a bound decides, and the blocks
  after it are not read; with all 200 pairs read and no bound reached the verdict is NO DECISION. The verdict is
  a function of the results alone.
- **The floor**: m and v are taken over the pairs and one pseudo-pair, half a pair better and half worse. It moves
  no mean and gives v the spread of one non-tied pair, so a run with no pair or all ties divides by no zero. n
  ties read ±(s1 − s0)² (n + 1)² / 0.5: at the width of 0.10 the bound is first reached at 12 ties, so one block
  of ties decides (±5.78). At a width of 0.05 one block could not (±1.44 at 16, the bound first at 24).
- **The cost**, simulated (`margin.py asn`: 2000 seeded runs a row, blocks of 16, every row in its output), by the
  true mean score and the share of pairs tied. NOT-WORSE, from a clear slowdown (0.40, H0) through its
  hypotheses' midpoint 0.45 to a batch that changes nothing measurable (0.50, H1); IMPROVE mirrors it, accept and
  reject swapped (at 0.50 with 0.3 tied: reject 0.853, no decision 0.113, 104 pairs; at its midpoint 0.55, 0.370
  no decision):

  | true mean, ties | accept | reject | no decision at 200 | pairs played | pairs, no cap |
  |---|---|---|---|---|---|
  | 0.400, 0.3 | 0.031 | 0.866 | 0.103 | 101 | 110 |
  | 0.425, 0.3 | 0.109 | 0.619 | 0.272 | 129 | 164 |
  | 0.450, 0.0 | 0.238 | 0.276 | 0.486 | 157 | 275 |
  | 0.450, 0.3 | 0.291 | 0.323 | 0.386 | 143 | 218 |
  | 0.450, 0.6 | 0.420 | 0.410 | 0.170 | 111 | 132 |
  | 0.500, 0.0 | 0.733 | 0.041 | 0.227 | 128 | 154 |
  | 0.500, 0.3 | 0.850 | 0.033 | 0.118 | 105 | 115 |
  | 0.500, 0.6 | 0.950 | 0.029 | 0.021 | 71 | 72 |
  | 0.500, 0.9 | 0.999 | 0.001 | 0.000 | 29 | 29 |
  | 0.550, 0.3 | 0.995 | 0.002 | 0.004 | 63 | 63 |

  A batch that changes nothing, with a third of pairs tied, decides in about 7 blocks (105 pairs) and ends at the
  200-pair cap with NO DECISION 12 % of the time; at the midpoint, 39 %. `asn --width` and `--alpha` price other
  hypotheses (at 0.50 with 0.3 tied, no cap: width 0.05, 418 pairs; width 0.10 with α = β = 0.1, 87). At the
  hypotheses both errors stay below α in every simulated row of both modes, at most 0.041.
- **Reported, not judged**: the paired mean of ln(`end_tick`), TEST − BASE, over the pairs both arms won; and
  the paired mean of ln(value A / value B) at GameTimes 15 000 and 30 000, from the results' `values` (`bar.py
  play --values`, which `run` asks: each seat's value at each GameTime the game reached before its end, read from
  the recording as *The value at the limit* reads it). Lost and killed metal (`paired.py`'s measures) need M1's
  measurement of every recording, which `run` does not make.
- **`margin.py run --base DLL --test DLL --mode improve|notworse --out DIR [--lanes L] [--blocks N]`** copies
  both DLLs into DIR first -- a DLL rebuilt elsewhere mid-run changes no game -- and refuses a DIR whose copies
  differ from the DLLs given. It plays block after block, one `bar.py play` an arm at once with L lanes each (1 to
  7, default 6: a GDI lane weighs 2 of the run lock's 28 slots, so two arms of 7 fill it), judges after each, and
  stops at a verdict, at the end of the order or after the order's first N blocks (a run in pieces: the next
  resumes it, its games played skipped). A block with a game that could not run is played once more. It is one
  runs-viewer run with a step a block (`tools/tarun.py`), whose id its games inherit (`TARUN_RUN`). Each block is
  one admission of the run lock an arm, both arms at once, and about 5 min of play at 6 lanes an arm on the
  reference setup; a run is as many of those as it takes to decide, about 7 for a batch that changes nothing.
- **`margin.py judge BASE TEST [--mode]`** prints the pairs, the ties, the mean score, the ratio by block, its
  bounds, the reported measures and the verdict. Exit 0 accept, 1 reject or a TEST loss, 2 could not judge, 3 no
  decision.
- **Identical code**: a seed replays a game exactly, so the same DLL in both arms ties every pair, and the test
  accepts NOT-WORSE (rejects IMPROVE) at the first block. MEASURED 2026-11-15, the 57d39d71 build as BASE and
  TEST, NOT-WORSE, two blocks at 4 lanes an arm and again from nothing at 6: 32 pairs, 32 ImpAI wins an arm,
  all 32 on the same `end_tick` (27 556 to 74 829) with the same values in both arms and both runs. Judged at
  the hypotheses above, block 1 reads +5.78, TEST is not worse; the blocks at 6 lanes played in 5.2 and 4.3 min
  on the reference setup beside round 3 of the bar. A change that alters play diverges its games from the first
  different decision, so its pairs tie only when both reach the cap.

## When the default flips

At A2, once Hard passes 100 of 100, Normal and Easy their targets against OTA AI at Hard, the 90 % ladder and the
no-stall rule ([landings](landings.html)).
