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
not seeded from its game's seed, a persona row's lever, the mirror's pairs, rows and judging, and `play`'s stop
and curtailment on fake lanes, offline; under the shared venv also the bounds' integration and a recorded game's value at the limit). A pair is a list of
game ids `bar-<pair>-<map>-<sides>-<n>`:

| pair | A | B | Difficulty | games |
|---|---|---|---|---|
| `hota` | ImpAI Hard | OTA AI | Hard | 100 |
| `eoh`, `noh` | ImpAI Easy, Normal (the level lever) | OTA AI | Hard | 12 blocks, 120 |
| `ne`, `hn` | ImpAI Normal, Hard (the level lever) | ImpAI Easy, Normal | Hard | 10 blocks, 100 |
| `p:<row>` | ImpAI Hard on a persona row (the persona lever) | OTA AI | Hard | 4 blocks, 40, on the development pool |
| `m:<row>`, `n:<row>` | ImpAI Hard; ImpAI on Normal's row (the persona lever) | ImpAI on a persona row (the persona lever) | Hard | 10 blocks, 100, on the development pool |
| `h:<m>` | ImpAI Hard | OTA AI, its production × m (the income lever) | Hard | 2 blocks, 20: a look of the handicap ladder |
| `g:<a>.<b>` | ImpAI on the settings a (a persona row, or `H`: none, the DLL's Hard) | ImpAI on the settings b | Hard | 2 blocks, 20, on the development pool: a member of the champion gauntlet |
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
  Normal, `IMPAI_PERSONAS` in `impai_react.c`), in `PERSONA_ROWS`. A game on a row writes its seat's row to
  that lever beside the level lever's `hard`, and could not run unless the seat's `impai: persona` line at its init
  names the same nine integers `from=lever`. Each result carries every ImpAI seat's persona as logged
  (`persona`). A row between two is a new entry under a new name; a row's games resume and are judged only on its
  own nine integers. The fields, in the lever's order: the economy's and the military's looks (ticks between
  decisions, multiples of 15 up to 150), the reaction (ticks from a decision to its orders, at most the shorter
  look less 15), each manager's budget (actions a minute and burst), and its attention (builders busy at once,
  operations at once); a budget or attention of 0 is no limit ([the brain](brain.html), *The plan and the
  personas*). The rows beyond L0..L5:

  | row | look eco, mil | react | apm/burst eco, mil | att eco, mil | what it is |
  |---|---|---|---|---|---|
  | L01 | 75, 120 | 60 | 7/2, 10/2 | 3, 1 | between L0 and L1 |
  | L0a | 105, 150 | 90 | 3/1, 4/1 | 2, 1 | below L0 on every axis it can go below |
  | L0b | 150, 150 | 135 | 2/1, 3/1 | 1, 1 | below L0a: the lever's harshest looks, reaction and attention |
  | Hlook | 150, 150 | 0 | none | none | Hard with L0b's looks alone |
  | Hreact | 150, 150 | 135 | none | none | Hlook with L0b's reaction: against Hlook, the reaction's own cost |
  | Happm | 15, 30 | 0 | 2/1, 3/1 | none | Hard with L0b's budgets alone |
  | Hatt | 15, 30 | 0 | none | 1, 1 | Hard with L0b's attention alone |

  The military's look 150 and attention 1 are the lever's limits already at L0, so L0a goes below it on the
  others; the reaction cannot be cut alone, since it needs a look 15 longer, hence Hreact beside Hlook.
- **The mirror**: against OTA AI at Hard every row from L0 to L5 took 0.53 to 0.74 of the points with no
  gradient, and Hard against Normal was futile (`hn` 33 won, 34 lost, 33 at the cap; MEASURED 2026-11-15, round 3
  at 57d39d71), so the levels are separated by ImpAI against ImpAI. `m:<row>` puts ImpAI on the row as B against
  ImpAI Hard as A; `n:<row>` puts A on Normal's row (`NORMAL_ROW`, the DLL's L3 until the calibration chooses
  one), through the lever too, so that once Normal is chosen it decides Easy. Every ImpAI seat sits at the level
  lever's `hard`, its row from the persona lever and checked `from=lever`. A mirror pair plays 10 blocks on the
  development pool, so no row is chosen on the maps that judge it, and is judged as `ne` and `hn` are: A's wins
  of its first 100, the curtailment and the ordering. MEASURED 2026-11-15, the 57d39d71 build, the first two
  blocks of four rows (20 games each; won counts a cap won by value):

  | pair | A won | A lost | at the cap: A, B, drawn | points | t over map means |
  |---|---|---|---|---|---|
  | `m:L1` | 15 | 2 | 5, 2, 1 | 0.775 | 1.98 |
  | `m:L0b` | 14 | 0 | 1, 0, 6 | 0.850 | 3.81 |
  | `m:Hlook` | 10 | 3 | 2, 1, 6 | 0.650 | 2.06 |
  | `m:Happm` | 15 | 0 | 4, 0, 5 | 0.875 | 4.74 |

  No row loses 90 of 100 to Hard at this rate: even L0b, the harshest, drew 6 of its 20 at the cap, every one on
  Steel Jungle or Lava Alley, where 24 of the 33 caps of the four rows lie. Of the axes, on 20 games a row, the
  budgets cost B the most -- alone (Happm) as much as every axis at L0b's harshness -- and the looks the least
  (Hlook). Two blocks are no look of the ordering, so these t judge nothing. Each row played as one `bar.py play`
  at 10 lanes in 6.6 to 9.4 min on the reference setup. The draws on those two maps are a weakness of the brain's,
  so the rows are chosen in the mirror on the brain that ships, once its fix has landed.

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
  where the calibration rows and the mirror play and the tuning tournament's tier B: **Acid Foursome** (land, 3 %
  under sea), **Steel Jungle** (mixed, 44 %), **Luschaven** (sea, 65 %), **Gasplant Plain** (small), **Lava Alley** (large).
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
    - `ne`, `hn` and the mirror's pairs: 90 wins of their first 100 games. **The curtailment**: at the 11th of
      those not won the pair has failed for certain; `judge` says so whatever is left, and `play` starts none of
      the pair's games after it (`--no-curtail` plays on).
    - **The ordering**, for the same pairs, printed beside the ladder and judging nothing (the verdict left if the
      ladder is relaxed): three looks at the pair's first 4, 8 and 12 blocks, each the t of the map means against
      a share of ½ and the O'Brien–Fleming t bound of a two-sided 10 % test over three looks on five maps (6.398,
      3.099, 2.253; on ten maps 4.010, 2.458, 1.918; `OBF_T`, which the venv's test checks by integrating the
      joint normal). "better" or "worse" at the bound, "futile" at an earlier look whose conditional power under
      the current trend is below 0.20, read to the first look that gives one. Before the first look it prints the
      t of the blocks complete, which judges nothing.
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
  ties read ±(s1 − s0)² (n + 1)² / 0.5: at the width of 0.10 the bound is first reached at 12 ties (±5.78 at 16).
- **The minimum sample**: the ratio is not read before block 2 (`MIN_BLOCKS`), whatever it reads at block 1: one
  block of 16 seeds is too few to call a change not worse even when every game ties.
  So identical code accepts NOT-WORSE at block 2 (±21.78), and so does any run whose first block is already past a
  bound -- 16 pairs worse read −25.05 at block 1 and reject at block 2.
- **The cost**, simulated (`margin.py asn`: 2000 seeded runs a row, blocks of 16, the minimum sample included,
  every row in its output), by the true mean score and the share of pairs tied. NOT-WORSE, from a clear slowdown
  (0.40, H0) through its hypotheses' midpoint 0.45 to a batch that changes nothing measurable (0.50, H1); IMPROVE
  mirrors it, accept and reject swapped (at 0.50 with 0.3 tied: reject 0.866, no decision 0.108, 106 pairs; at
  its midpoint 0.55, 0.372 no decision):

  | true mean, ties | accept | reject | no decision at 200 | pairs played | pairs, no cap |
  |---|---|---|---|---|---|
  | 0.400, 0.3 | 0.029 | 0.864 | 0.108 | 103 | 112 |
  | 0.425, 0.3 | 0.112 | 0.620 | 0.269 | 130 | 165 |
  | 0.450, 0.0 | 0.231 | 0.278 | 0.492 | 158 | 280 |
  | 0.450, 0.3 | 0.294 | 0.316 | 0.390 | 144 | 216 |
  | 0.450, 0.6 | 0.421 | 0.402 | 0.177 | 114 | 136 |
  | 0.500, 0.0 | 0.728 | 0.041 | 0.232 | 130 | 156 |
  | 0.500, 0.3 | 0.860 | 0.034 | 0.107 | 104 | 113 |
  | 0.500, 0.6 | 0.946 | 0.033 | 0.022 | 73 | 74 |
  | 0.500, 0.9 | 0.999 | 0.002 | 0.000 | 36 | 36 |
  | 0.550, 0.3 | 0.997 | 0.002 | 0.002 | 64 | 64 |

  A batch that changes nothing, with a third of pairs tied, decides in about 7 blocks (104 pairs) and ends at the
  200-pair cap with NO DECISION 11 % of the time; at the midpoint, 39 %. `asn --width` and `--alpha` price other
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
- **Settings instead of builds**: `--base-row` and `--test-row` put each arm's ImpAI seat on a persona row (or `H`,
  none) through `bar.py play --persona A=<row>`, which keeps the ids and so the seeds. Two rows of one DLL are
  then compared on the same games as two DLLs are. A run writes its rows to `rows.json` and refuses a directory
  whose rows differ, and `judge` with rows refuses a result whose ImpAI seat played another persona. MEASURED
  2026-11-16: `bar-hota-pincushion-aa-1` played with `--persona A=L3` to GameTime 3 600 logged
  `impai: persona seat=1 ... from=lever` on L3's values, with the id's own seed (1873460876). `--base-tune` and
  `--test-tune` do the same for the tune lever's plan constants (`<name>=<int>,...`, `bar.py play --tune`), so a plan
  `tune.py spsa` found is judged against the champion's hand-written plan on the champion's DLL: a run writes them
  to `tunes.json`, and `judge` refuses a result whose ImpAI seat played other items than its arm's (none for an arm
  that names none).
- **`margin.py judge BASE TEST [--mode]`** prints the pairs, the ties, the mean score, the ratio by block, its
  bounds, the reported measures and the verdict. Exit 0 accept, 1 reject or a TEST loss, 2 could not judge, 3 no
  decision.
- **Identical code**: a seed replays a game exactly, so the same DLL in both arms ties every pair, and the test
  accepts NOT-WORSE (rejects IMPROVE) at the minimum sample, block 2. MEASURED 2026-11-15, the 57d39d71 build as
  BASE and TEST, NOT-WORSE, two blocks at 4 lanes an arm and again from nothing at 6: 32 pairs, 32 ImpAI wins an
  arm, all 32 on the same `end_tick` (27 556 to 74 829) with the same values in both arms and both runs. Judged
  as above, +5.78 after block 1 (not read) and +21.78 after block 2, TEST is not worse; the blocks at 6 lanes
  played in 5.2 and 4.3 min on the reference setup beside round 3 of the bar. A change that alters play diverges
  its games from the first different decision, so its pairs tie only when both reach the cap.

## The handicap ladder

Once ImpAI wins every fair game, wins stop measuring. The ladder's number keeps moving: **m\***, the multiplier on
OTA AI's production at which ImpAI Hard takes half the points on the bar's five maps (a win 1, a draw 0.5, a game at
the cap scored by value). A stronger ImpAI holds against a richer OTA AI, so m\* rises.

- **The lever.** `h:<m>` arms OTA AI's seat with the income lever, `tagpu_impai_income.on` at m: its production
  times m at every economy pass, credited through the player's ledger one pass after the production it multiplies,
  gives excluded ([the income lever](../modules/income-lever.html)).
  - m is written with three places in 1.000..8.000, the lever's own precision, so one multiplier is one pair and
    its games one set of seeds.
  - A game could not run unless the level init's `income: seat=` lines name exactly the seats it armed, at their
    multipliers.
  - The cheat flag reads an armed seat's income as any other's: the first pass, `income_t1`'s, carries no bonus.
  - `judge` reports an `h:` pair's points and judges nothing.
- **The method** (`tools/impai/ladder.py run --dll DLL --tag TAG`) works in x = log2(m), over the bracket [0, 3]
  (×1 to ×8).
  - Each look plays the bracket's midpoint, rounded to the lever's places, as the pair's two blocks: 20 games,
    every map from both starts and every side pairing.
  - Points of at least one half move the low end up to the look; less move the high end down.
  - A look never plays a multiplier a look has played: when the next midpoint rounds to one, the bracket is inside
    the lever's precision, the looks left are skipped and the fit takes those played.
  - After six looks, a logistic p(x) = 1 / (1 + e^((x − c)/s)) is fitted over all 120 games, points as fractional
    outcomes. A weak prior on log s (median 0.4, one unit of spread) keeps the fit finite when the looks separate
    wins from losses outright.
  - The result is m\* = 2^c with its 90 % interval from the fit's curvature, said to lie beyond ×8 or below ×1
    when it does.
- **Why bisection with a fit.** SIMULATED (`ladder.py simulate`, 300 runs a cell, the true point uniform in log2
  0.3..2.7, ties 0.3). The figures are the RMSE of log2 m\*:

  | slope (log2 m a logit unit) | 6 looks × 20 games | the last midpoint alone | 6 looks × 10 games |
  |---|---|---|---|
  | 0.15 | 0.030 (±2 %) | 0.049 | 0.042 |
  | 0.3 | 0.057 (±4 %) | 0.091 | 0.078 |
  | 0.5 | 0.091 (±6.5 %) | 0.136 | 0.122 |

  In the design's simulation (looks of 16 games, no ties), a fixed grid of six with the same fit came out between
  the two: 0.106 at slope 0.3, against 0.117 for the midpoint and 0.074 for bisection with the fit. The bisection
  spends its games near the half point, where a game says most about c. The slope is not known before a run; the
  fit measures it.
- **How it runs.**
  - A look is two pieces, one `bar.py play` a block on ten lanes. Each is one wave of ten games, well inside
    batch.py's 600-s deadline (games to the cap took at most 346 s on the mirror's run). They are launched in order
    through batch.py's piece runner, which stops a piece past the deadline.
  - The DLL is copied into the run's directory at its first look, and a different one is refused, so one ladder is
    one build.
  - `<out>/ladder.json` keeps every finished look, so `run` again goes on from the first look not finished.
  - A look with a game that could not run, or a cap not scored, is played once more; if it still lacks one, the run
    stops, could not judge (exit 2).
  - Six looks are 120 games, about twelve admissions of the run lock.
  - Its tests, offline (`test_ladder.py`): the bisection, the multiplier's rounding, the fit on simulated games and on
    a separated set, the report's edges, and a run on a fake player (a threshold bracketed and fitted, a draw's half
    point, the resume, a missing game played once more then could not judge, another DLL refused); also the `h:`
    pair's spec, its lever lines, the cheat flag and a kept result's multiplier.
- **Measured.** The 57d39d71 brain with the lever cherry-picked (`ladder-base` 7621d396), on the bar's five maps,
  2026-11-16, `.data/impai/runs/ladder/57d39d71/`. ImpAI Hard breaks even with OTA AI Hard at **m\* = ×1.95**
  (90 % ×1.78..×2.14), slope 0.298 log2 a logit unit:

  | look | ×m | points of 20 games |
  |---|---|---|
  | 1 | 2.828 | 0.150 |
  | 2 | 1.682 | 0.650 |
  | 3 | 2.181 | 0.525 |
  | 4 | 2.484 | 0.200 |
  | 5 | 2.328 | 0.225 |
  | 6 | 2.253 | 0.300 |

  The looks waited on the run lock beside other runs; the six took 2 h 10 min from the first admission.

## The champion gauntlet

A candidate that beats the last champion can still lose to an older one: strength among settings need not be
transitive. The gauntlet plays the candidate against a pool of earlier champions directly, ImpAI against ImpAI in
one build, so a regression that only one of them exposes shows. `tools/impai/gauntlet.py`; `test_gauntlet.py`
holds it offline.

- **The settings** of a seat are a persona row (`PERSONA_ROWS`, the persona lever) or `H`, none: the DLL's Hard.
  Tuned tables join them once the table lever's files are versioned; the lever is per seat already
  (`tagpu_impai_table.on`, which `bar.py play` writes from a seat's `table`).
- **The games**: against each pool member P the candidate C plays `g:<C>.<P>`, C as A: 2 blocks by default, 20
  games on the development pool, every map from both starts and every side pairing, so each row sits in both seats
  and on every start. A game's points are C's (a win 1, a draw or a cap with no lead in value 0.5, a loss 0).
- **The verdict**, a member at a time: C's mean points m over n games and the one-sided 95 % interval
  m ± 1.645 × 0.5 / √n. The 0.5 is the most a game's points can spread, so the interval never claims more than the
  games show, whatever share were draws. **beats** when the interval's low end is above one half, **loses** when
  its high end is below, **even** otherwise. At 20 games the half-width is 0.184: a member is beaten from 0.684
  of the points. **The gauntlet fails when C loses to any member**, and says which it beats and which it is even
  with.
- **How it runs**: a piece a block, one `bar.py play` on 10 lanes, through `batch.py`'s `play_stage`, as the
  ladder's looks are. One gauntlet is one build, one candidate, one pool and one block count: `gauntlet.json` in
  the run's directory holds each member's verdict as it is judged, so `run` again goes on from the first member
  not judged, and refuses a directory of another gauntlet or DLL. A member with a game that could not run is
  played once more, then the run stops, could not judge. `gauntlet.py judge DIR` reads the verdicts again from the
  games.
- MEASURED 2026-11-16: `bar-g:L3.L0b-acidfoursome-ac-1` played to GameTime 3 600 (27.8 s at one lane) logged both
  seats' persona lines `from=lever`, seat 1 on L3's values and seat 2 on L0b's.

**Code against code is designed, not built.** It would play two builds' brains in one game, and there are two
ways to do it.

- **Two processes, a network game** (each peer's DLL runs the ImpAI seat its own peer added, and only that one:
  [entering the game](engine-seam.html); `netai_check.py` proves it on Two Continents):
  - *What exists*: `twopeers.py` walks two instances into one battle room under the run lock and a leased
    DirectPlay port, each peer adds its AI seat, and `--netai impai` on both makes both ImpAI.
  - *What it needs*: a DLL a peer (`TwoPeers` takes one); the bar's maps rather than Two Continents in the lobby
    walk; something for the two peers' own commanders, which every network game seats beside the AIs (each
    allied to its own peer's AI, or killed at the start; that a dead peer's process still runs its AI seat is
    [INFERRED], not measured); the outcome read from both peers' logs.
  - *Its cost*: a network game cannot turbo and plays variable batches at speed 20, about 60 ticks a second
    (MEASURED in the multiplayer recorder's runs, [the multiplayer slice](../tadr-port/demo-recorder-multiplayer-slice.html));
    the 80 mirror games kept end at a mean GameTime of 62 641 (33 of them at the cap, MEASURED 2026-11-16 from
    their results), so a game takes about 17 min plus 39 to 46 s of launch and lobby ([INFERRED] in
    [the compatibility suite](../compat/suite.html)), on two GDI instances, 4 slots. That is about 72 slot-minutes a game, where a skirmish mirror game takes 6.6 to 9.4 (20 games in 6.6
    to 9.4 min at 10 lanes, above): **8 to 11 times the cost**, 7 games at once on the lock's 28 slots, and
    20 games in about 55 min against 8. And **no pairing**: a network game is not deterministic, so an id does
    not replay its game, and two builds cannot be compared seed by seed. A gauntlet member then needs its games
    judged unpaired, as this gauntlet already judges them.
- **One process, a two-brain build**: brain Y's sources compiled beside brain X's, every global renamed, with the
  seat choosing its brain. Skirmish speed and exact pairing stay. Its cost is the interface, which moves with the
  brain:
  - the seam (`tagpu_impai.c`) calls 57 global symbols of the brain and its engine-free helpers (`impai_*.c`,
    `tagpu_worldq.c`, `tasim_snap.c`, `tagpu_placegeo.c`), eight other modules call 14 of them
    (`tagpu_place.c`, `tagpu_resource.c`, `tagpu_skirmish.c`, `tagpu_sweep.c`, `tagpu_wikisim.c`, `tasim_place.c`,
    `tasim_worldq.c`, `dllmain.c`; MEASURED from the objects' symbol tables), and the seam itself installs 19
    engine sites ([the hook map](../modules/hook-map.html));
  - of the 201 commits that changed `impai_*.c` since 2026-11-08, 91 also changed the seam and 112 a shared header
    (`impai.h` or `worldq.h`), MEASURED from the log on 2026-11-16. Two arbitrary builds therefore rarely share
    one seam, and a two-brain build must carry both seams and dispatch each of the 19 sites per seat;
  - the replay tables double, and a two-brain game replays only on its own two-brain DLL.

  It is cheap to run and dear to keep: every brain change that touches the seam or a shared header breaks it,
  unless the interface is frozen as a versioned ABI, which is a rule on the brain's development and so the
  owner's decision.
- **The recommendation**: the network game, since it works for any two builds without constraining the brain.
  Its 8 to 11 times the cost suits a gauntlet's few members, not a tuning loop. Its first step is a
  `netai_check.py`-sized proof that two peers with two DLLs each play their own ImpAI seat to an end on a bar
  map.

## The exploiter suite

Short situations, each hunting one blind spot, faster and more pointed than whole games. `tools/impai/exploit.py
run --dll DLL --tag TAG` plays them through the bar's lanes (seats, seeds, holds, results and end) under the run
lock, at turbo with no draw on GDI, into `.data/impai/runs/exploit/<tag>/` (`games/`, `scenarios/`,
`verdicts.json`); `test_exploit.py` holds it offline on a fake player.

- **The scripted opponent** is seat B with its AI off from the load (the scenario's `ai: "off"`: a type-1 seat whose
  units keep the orders they are given and no planner runs), so it is exactly the units the situation places.
  Its pressure comes in **waves**: scenarios applied without a clear on the level held at their GameTime, each unit
  created with an `attack` order on ImpAI's ground (TA has no attack-move; an attack on the ground advances and
  engages). The opponent has no economy. OTA AI has no lever for a forced build list, and a persona limits ImpAI's
  attention rather than its plan, so placed units are the cheapest honest script: no DLL change.
- **The seats stand at their starts.** ImpAI's anchor, home and empty-start rules all read the seat's start, so a
  situation places ImpAI's commander at its seat's start and the opponent's at its own, seat k at StartPos(k + 1) as
  the bar's fixed starts seat them, and declares it `seated`; `exploit.seated_start_misses` names a commander off its
  seat's start, and `test_exploit.py` checks every seated situation against the installed maps. The commander rush
  alone is not seated: its opponent's commander is the rush, set down away from every start.
- **The situations**, each ImpAI Hard as A in seat 1 at Difficulty Hard:

  | situation | map | the opponent | verdict, at the cap |
  |---|---|---|---|
  | `air-rush` | Acid Foursome | waves of bombers from 9 000, then gunships and fighters (3, 8 and 14 aircraft at 9 000, 12 000 and 15 000), taking off beside its start and flown at ImpAI's, 5 680 wu away | ImpAI's commander lives to 18 000 |
  | `naval-turtle` | Gods of War | its commander at its start and two light laser towers beside it, on an island apart from ImpAI's | ImpAI has killed all of it by 20 000 |
  | `artillery-creep` | Acid Foursome | a heavy laser tower and three mobile artillery set down closer to ImpAI's start every 3 000 ticks from 6 000, the tower 4 400, 3 600, 3 040 and 2 560 wu from it | ImpAI's commander lives and its value is at least the opponent's at 18 000 (or the opponent is gone) |
  | `commander-rush` | Acid Foursome | its commander alone, a quarter of the way from ImpAI's start, ordered at the load to attack ImpAI's commander | ImpAI's commander lives to 6 000 |

- **The puzzles** are real positions. `exploit.py cut RESULTS_DIR... --out FILE` takes every bar game against OTA AI
  that ImpAI did not win and whose recording was kept. A game OTA AI won is taken over 18 000 ticks before its end,
  and ImpAI must still hold a unit at the end (`survives`); a game at the cap is taken over 18 000 ticks before the
  cap, and ImpAI must end it (`kills`). The takeover (`tacli replay takeover`) is at OTA AI's seat: the idle human
  holds no unit, which the takeover refuses, and the seat taken becomes a human's. The lane hands it straight back
  to OTA AI with the console's `AI` command (cheats on; `0x416280` makes type 1 type 2) and reads the record's
  controller byte back as 2, so OTA AI starts afresh there, as on every seat a takeover gives it, and ImpAI plays
  on from what it knew. The takeover starts at the recording's refresh at or before the GameTime. `run --puzzles FILE` plays them beside the situations. A recording can
  be taken over only by a DLL that declares the recorder tables of the build that made it (R0's table check), so
  puzzles are cut from the games of the build under test, or of one with its tables; another build's takeover is
  refused, and the puzzle could not run.
- **One situation is one launch.** A clear removes the units, but ImpAI's seat memory is cleared only at a level's
  init (`tagpu_impai.c`), so a second situation in the same level would start from the first one's knowledge; and
  fights are never packed into one level ([tasim calibration](../modules/tasim-calibration.html), *The packed lane*).
  The seed of each is its id's, so a situation replays exactly.
- **The verdict**: `commander` (ImpAI's seat not eliminated and no commander lost), `kills` (the opponent's seat
  holds no unit before the cap), `survives` (ImpAI's seat holds a unit at the cap), `value` (`commander`, and its
  value at the cap at least the opponent's, or the opponent gone). A situation that could not run, or a value not
  read, could not run, never a failure. Exit 0 every situation passed, 1 one failed, 2 one could not run.

MEASURED 2026-11-16 on the reference setup, the c1dc2e70 brain, four lanes, with the situations placed before their seats stood at their starts: the four situations in 47 s of play
each at most (under a minute from admission to verdict), three passed and one failed. The commander rush ended at
3 582 with the opponent's commander dead. The air rush passed on its commander, but 20 of the opponent's 26 units
were still alive at the cap, ImpAI having completed three anti-air units. The artillery creep passed on value, 33 777
against 26 828, 12 of the opponent's 17 units killed. **The naval turtle failed**: at the cap ImpAI had a shipyard,
one naval unit and no attack, and the commander and both towers stood untouched.
The nine puzzles cut from that build's own games of the batch (`batch/c1dc2e70/base`: the eight held-out `p:L5`
games and `bar-hota-redtriangle-cc-1`, all at the cap) were each taken over at 62 701, the refresh before 62 971, and
played to 80 971 in 30 to 55 s of play, four lanes, 1.6 min from admission: **one passed** (Steel Jungle `ca-2`,
the opponent gone at 75 906), **eight failed**, the opponent still holding units at the cap. Lava Alley `ca-4`
ended with ImpAI at 8 units against 146.

MEASURED 2026-11-17 on the reference setup, the d8799688 brain, the Acid Foursome situations at their seats' starts
against the same brain's games before: the air rush passed on its commander again, the opponent holding 20 units at
the cap (18 before); the artillery creep passed on value, 34 716 against 27 381 (34 893 against 26 828 before), 7 of
the opponent's units alive (5 before). Each game took 46 to 50 s of play.

MEASURED 2026-11-17 on the reference setup, the naval turtle at 9f118119 (the stand-off, the sea scout, one air scout
a seat): still failed, the commander and both towers at full health at the cap. What loses it:

- **No air plant, so no air scout.** No unit the seat has sees the commander or the west tower -- they stand about
  370 and 415 px from the nearest water a boat sails, past the 256 px the engine's sight table gives any unit on an
  axis -- and the air scout that would ([decisions](decisions.html), *One air scout a seat*) comes from an air plant the
  seat never gets: its second plant, a shipyard, came at 8 587 on a metal income of 9.7, and a build whose island
  order took the air plant past one sea plant chose no plant at all to the cap.
- **With an air plant given** (a scratch situation, a CORAP of ImpAI's set down at its start): the air scout is
  bought and flies 75 trips, the commander and both towers become rows, and no retreat is taken; yet the three
  sea waves at the west tower bring the destroyers to 421-527 px of it, inside their 660, and it keeps 750 of 750
  hp: each ATTACK is back in FORM 1 000 to 1 400 ticks after it left. Not diagnosed: whether the stand-off's ground
  attack on the remembered tower was given and the engine's ballistic gun could not reach the hill, or the attack
  ended first.
- **The retreat, read from the predictions** (a diagnostic build at a7fc308e, no air plant): the sea wave went at
  16 470 predicted to win (m 1.000 against the tower alone), and at 18 930 its two destroyers, 281 px from the
  commander, retreated against the commander and the tower (m -0.945): the commander can come within its reach
  of them over its own shore (cb 1), and the model's commander walks to them and fires its D-gun, a CommandFire
  weapon it counts, killing both in four steps. The wave's prediction credits the range already; what it does not
  have is a ship that keeps off the shore by the commander's reach -- the stand-off does not stand off mobile units.

## Automatic tuning

`tools/impai/tune.py` holds two searches, each where its dimensions are: the levels' rows on one line, and SPSA
over the brain's plan constants. `test_spsa.py` holds both offline: the arithmetic, the line, the fits, the
determinism and resume of each run, and runs on fake players.

- **The levels' line** (`tune.py levels-line run --level normal|easy`). The levels need an order and two floors,
  not the best row in nine dimensions, so the row is one number: harshness t in [0, 1] on a fixed line from L0b,
  the lever's harshest row (t = 0), to Hard's (t = 1). The line is the persona space's diagonal -- each of the
  nine integers moved in step from L0b's to Hard's and rounded, the looks in steps of 15, the budgets and the
  attention on a log scale to none -- so every axis relaxes as t grows and every point is a row
  `impai_persona_ok` accepts; the raw integers are not interpolated, since a budget or attention of 0 is no limit
  and a straight line from L0b's 2 actions a minute to Hard's 0 would pass through 1. Normal's row is the strongest
  Hard beats in at least 90 % of the mirror's games while it still takes 0.90 of the points against OTA AI at Hard;
  Easy's the strongest Normal's row beats so while it takes 0.50.
    - **The looks** are the handicap ladder's method (*The handicap ladder*): bisection on t, each look the mirror's
      two blocks (20 games: `m:` ids with the row on B; Easy's `n:` with Normal's row on A) at the bracket's midpoint,
      on blocks past those the confirmation plays; then a logistic fit over every look's game, the ladder's with its
      slope prior in t. Six looks.
    - **The order check.** Bisection and fit both rest on the line being monotone in strength, which is a measurement,
      not a given: after every look, a pair of looks whose stronger row lost more, by more than 2.576 standard errors
      of the difference, stops the run as **not monotone**, and the question goes back to the owner. A run that ends
      states the order as measured.
    - **The aim.** At the bound itself the 100-game confirmation passes only about half the time (90 of 100 at a true
      0.90), so the row chosen is where the fit puts the stronger side's chance at 0.95, a little inside it.
    - **The confirmations** at the row chosen: the mirror's 100 games (at least 90 won) and the calibration's 40
      against OTA AI (at least the floor). Below the floor, no point on the line meets both bounds -- every lower t is
      weaker on a monotone line -- and the question goes back to the owner, as does a t90 below 0 (even L0b is not
      beaten 90 %; the mirror's two blocks of `m:L0b` had Hard at 14 of 20, every miss a draw at the cap, *The
      mirror*).
    - **Its error, SIMULATED** (`tune.py levels-line simulate`, 300 runs a slope, the true t90 uniform in [0.15,
      0.85], rows as the line rounds them): six looks of 20 games put t90 within 0.020, 0.039 and 0.070 (RMSE) at
      slopes 0.05, 0.1 and 0.2 in t a logit unit, against 0.030, 0.057 and 0.103 for the bracket's midpoint alone;
      at the aim the true win share at the row chosen is 0.945, 0.940 and 0.926, and the 100-game mirror confirms it
      in 91, 89 and 81 % of runs.
    - **Its cost**: 120 games of looks and 140 of confirmation a level, 260, against the 1 600 of 40 SPSA iterations.
      [INFERRED from the mirror's measured blocks, 6.6 to 9.4 min for ten games at ten lanes] A look is two pieces of
      ten games at seven lanes, which the lock holds at once, about 10 to 16 min; a level about 2 to 3.5 hours, the
      confirmation more than half of it.
    - **Not run live**: the rows are chosen on the brain that ships, once its draws on Steel Jungle and Lava Alley
      are fixed. A stopped run resumes where it stopped (`line.json`), on the DLL it began on; it is one runs-viewer
      run with a step a look and one for the confirmation.
- **SPSA over the plan constants** (`tune.py spsa`, `spsa.py`, Spall's gains, maximising): each iteration perturbs
  every named constant at once, plays theta+ and theta- on **the same games** (the same ids, so the same seeds --
  common random numbers; each arm's results apart under `games/<k>-<arm>/`), a new block each iteration, and steps
  along the difference. The constants reach ImpAI's seat through **the tune lever** `tagpu_impai_tune.on`
  (`level_lever.py --tune`, `bar.py play --tune A=<name>=<int>,...`): 28 named constants of `IMPAI_PLAN` and the
  military's and economy's `#define`s, each a coordinate over its range, in integers (floats in thousandths). The
  DLL's side is specified for the brain writer (`.data/impai/tune-lever-spec.md`: a plan a seat in `IMPAI_MIL`, so
  SIM_STATE and recorded; `impai_plan_ok`; the lever applied where the persona's is, never on a puppet or in a
  network game; the init line `impai: tune seat= from=lever ...` the lane checks) and **not built**: until it is,
  every game of it could not run.
    - **The objectives**: `margin`, the arms paired on each game of the margin test's block by its measure, the time
      to win (+1 where theta+ was better, -1 worse, 0 a tie); `handicap`, A's points against OTA AI on its
      production times the champion's m* (`h:<m>`); `mirror`, A's points against ImpAI Hard with no lever (`g:H.H`).
    - **A run** is fixed to the DLL it began on and plays an iteration's pieces through `batch.py`'s pieces under the
      run lock, each stopped past its ten minutes; a game that could not run plays its pieces once more, and an
      iteration still missing one stops the run, unrun (exit 2). The state (`spsa.json`) is written after each
      iteration, so a stopped run resumes at the iteration it was in; the same seed is the same run.
    - **The plan it finds is accepted against the champion** by the paired margin test with tune items on its TEST
      arm (*The paired margin test*, `--test-tune`), on the champion's DLL.

## When the default flips

**The default has flipped, with the ladder unmet, by the owner's decision** (2026-11-16, [landings](landings.html),
*Next, in order*): ImpAI is the skirmish default -- `seatai`'s default every seat, the seat button's first computer
step ImpAI, `netai`'s default `impai` ([ImpAI in the engine](../modules/impai.html), *The seat choice* and *The
network choice*) -- and the levels ship as they are. The condition this section set, and which still measures the
levels for a later release: Hard passes 100 of 100, Normal and Easy their targets against OTA AI at Hard, the
90 % ladder and the no-stall rule. Every harness launch writes `seatai` and `netai` itself, so the bar's games, the
batch and the replay test groups play the seats they name, whatever the default.
