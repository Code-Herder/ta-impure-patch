# The bar

What ImpAI must win before it becomes the default AI. The owner judges the stock AI too weak to lose to, so the bar is every game, not a majority.

## The rules

- **Each ImpAI level beats OTA AI on the same Difficulty in 100 of 100 games**: Easy against Easy, Normal against Medium, Hard against Hard. A single loss is a defect to diagnose and fix, not noise to average away. The owner will re-evaluate this once results exist.
- **Adjacent levels stay distinct:** ImpAI Normal beats ImpAI Easy, and Hard beats Normal, in at least 90 % of games.
- **Easy stays beatable by a new player.** No script can measure that, so the owner's playtest is the judge.
- **No stalls:** in no game do ImpAI's army or economy stop for more than two game minutes.

Note that ImpAI Easy plays at full income with handicapped behaviour, while OTA AI on Easy plays at 50 % income ([ImpAI for the player](player.html)).

## The games

- 1v1, on five maps: land, sea, mixed, small and large.
- Both sides played on Arm and on Core.
- Commander death ends the game.
- A game nobody has won after **45 game minutes** counts as a **failure**, not a draw.
- Played in the engine at turbo k = 1, one tick a frame (rung 2 of the [testing ladder](testing.html)), under the
  run lock.

## The harness

`tools/impai/bar.py` (`ids`, `play`, `judge`, `check`; `test_bar.py` holds its game list, seating, start schema,
stall rule, the holds' end at the cap, what a result reads from the level's end, the judge and the row checks, a
game's end on a fake instance in each way a game ends, and `play`'s stop on fake lanes, offline). A pair is a list
of game ids `bar-<pair>-<map>-<sides>-<n>`:
`eota`, `nota`, `hota` (ImpAI Easy, Normal, Hard against OTA AI at Easy, Medium, Hard), the ladder pairs `ne` and
`hn` (through the level lever, at Hard), and `dry`, OTA AI against OTA AI at Hard. Each ImpAI pair is the five
maps × four side pairings × five games, the player under test at the map's first start in games 1, 3 and 5; the dry
run is ten games.

- **The maps**, one of each kind by the rule the specification states (at least three starts, no uniform metal;
  land, sea and mixed from the middle third of the 90 such retail maps by area, small from the smallest third,
  large from the largest, each the median of its class): **Pincushion** (land), **Red Triangle** (sea, 75 %
  under sea), **Trout Farm** (mixed, 40 %), **Lava Mania** (small), **Plains and Passes** (large).
- **A game**: `scenarios/impai/bar.json` filled -- an idle human with no unit, the two players in seats 1 and 2,
  each commander placed at its start position from the map's `.ota` (the schema a three-seat skirmish reads), base
  storage 1000/1000, the end test off. A lane, a held tareplay Worker at turbo k = 1, holds the level every 3000
  ticks (`STEP`), or earlier at the first frame visit at which seat 1's or seat 2's units alive `+0x144` is 0 (`pause at T or
  units <s> <= 0`, `tagpu_replay.c` THE PAUSE AT T, which at k = 1 lands on the visit after the step in which the
  count fell), and peeks both: the game ends when one is 0, or at GameTime 81 000. Both at 0 at one look -- both
  sides gone in one tick -- is a draw, judged as a game at the cap. A launch the engine does not reach is "could
  not run", never an outcome.
- **The end of a game**: once the outcome is read, the level is ended through the engine's own exit
  (`tareplay.Worker.quit`: the in-game menu, Exit, Exit Game, Yes). The exit menu's handler `0x4605C0` calls the
  exit `0x491C60`, a three-call stub -- the blank frame `0x4257A0`; `0x451B60`, which returns at once off a
  network game (`main+0x2A44` bit 0 at `0x451B66`, DISASSEMBLED); the teardown `0x491B60` -- so the teardown
  runs within the handler's call, on the held level, before anything of the exit ([recorder](../engine/recorder.html)).
  The teardown ends the recording and writes ImpAI's level-end lines, and the exit waits for the recording's
  writer to close its file. No tick runs: the level stays held through the in-game menu, as tareplay's twin
  checks on every record test. Ending the process (`tacli stop --hard`) reaches neither. A game whose exit does
  not reach the teardown (no `impai: handicap rows` in its log) could not run.
- **What a result reads from the level's end**, for each ImpAI seat: `end`, from its `impai: eco` and
  `impai: mil … level=` lines -- its units completed, its army units completed by class (`army`, ImpAI's own
  classes `lraider` … `ascout`, and `army_built`), its factories completed by their build tech, the highest
  tech their constructors build (`factories`, `IMPAI_TINFO.ctech`, `"3"` holding 3 and above), its water-only
  factories completed (`yards`), the sea plants it ordered, its attack waves, raids and scouting runs begun, its
  naval judgement and its tech-up income `tech_m`; `first`, the GameTime of the first `impai: mil seat= t= attack=
  raid= scout=` line that began an ATTACK, a RAID and a scouting run, `null`
  for a kind it never began; `income_max`, its best metal income a pass over a pulse minute (the rise of
  TotalMetalProduced between two pulses, over the passes between them). `judge` prints, per pair and side, in how
  many games each first task came, its range and median, and the games without one.
- **What a result keeps for a diagnosis**: its last two looks (`looks`: the GameTime and each seat's units alive,
  and at the last each seat's commanders lost, the word `+0x106` the destructor raises at `0x486996`,
  [units](../engine/units.html)). A game A did not win (lost, at the cap, a draw) or in which an ImpAI seat
  stalled, and a game that could not run, keeps its instance log from the launch on, `<id>.tagpu.log` beside its
  JSON. A bar game of the first kind keeps its recording too, moved to `<id>.imprec` beside its JSON with the
  path in the result's `recording` -- once its writer's `closed` line is in the log, which is what says the file
  is whole; a recording to keep that was not made or not closed makes the game "could not run". The
  `.imprec.lock` beside a recording is no sign of an open file: the lock file stays beside every recording that
  exists, by design (`tagpu_recfile.h`, THE PATH LOCK). Every other game's recording is deleted and its
  `recording` is `null`; the tuning tournament's games (`tourney.py`, through `play_game`) keep none. A draw reads
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
- **The rate** (MEASURED 2026-11-13, the dry run: ten lanes, OTA AI against OTA AI at Hard, GDI, k = 1, game
  speed 20): about 90 to 107 ticks a second a lane -- Trout Farm reached GameTime 30 000 in 337 s with its launch,
  Lava Mania 61 200 in about 570 s. A game run to the cap took about 14.5 minutes. Those games were held every
  300 ticks and their waits read the log every 0.4 s, so about two thirds of a game's wall time was spent held
  (204 holds of about 2.06 s on Lava Mania, against about 272 ticks a second while it ran); a game is now held
  every 3000 ticks and its waits read every 0.02 s, which projects a cap game at about 6 minutes with its launch
  [INFERRED: 81 000 / 272 s running, 27 holds of about 0.6 s; not measured]. How pieces are sized against that is
  open.
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
- **The row checks**: `bar.py check b4|b5 DIR…` scores A2b's rows (the A2 specification's section 9) from the
  results: ImpAI Hard against OTA AI Hard (`hota`), every game played to the row's bound with `play --cap` --
  b4 27 000, at least four games on two maps; b5 36 000, a game on each of the five maps. **b4**: an attack wave
  begun (`first` ATTACK), a scouting run begun and no stall. **b5**: army units of at least `ROLES_MIN` (2)
  classes completed; a water-only factory completed on Red Triangle and on any map ImpAI judged naval
  (`naval=1`); a factory of build tech 2 or above -- one whose constructors build tech 2 -- completed wherever `income_max`
  reached `tech_m`. Exit 0 when every
  game meets each, 1 when one does not, 2 when a game could not run, is of another pair or bound, carries no
  level-end lines of A's, or the games are short of the row's. The holds of every game end at its cap exactly
  (`next_hold`), whatever the cap. **Not established**: b5's "an army of each wanted role" -- which classes a
  seat wants follows from its table, its picture and its factories' menus at each decision, and no line logs
  them, so the check asks for a composition of two classes and prints the classes completed.
- **`play_game(game, lane, out_dir)`** is the tuning tournament's game runner (`tools/impai/tourney.py`): one game
  of its spec on one lane, with each seat's table copied into the game folder and named by
  `tagpu_impai_table.on`, its result carrying each seat's table hash from its `impai: table` line and its value
  built and alive from its last pulse.
- **Not run**: no game of the bar has been played. The dry run of 2026-11-13 ended one game, Trout Farm 1, with both seats at 0 in
  one 300-tick look at GameTime 30 000 (22 and 31 units at the look before), on a level held there by the harness's
  own `pause at 30000` and nothing else -- no scenario, no end test, no console command; the offsets are the
  records' (`main+0x1B63` + seat × `0x14B` + `0x144`, [memory](../engine/memory.html)). Its log was not kept, so
  whether both commanders died in that step is not established; the next run's draw carries what settles it
  (above). Of the run's other games one ended with its game's process gone (Pincushion 1, 117 units alive at its
  last heartbeat, its log not kept either) and the rest were cut by the run's ten-minute bound.

## When the default flips

At A2, once all three levels pass the 100/100 rule, the 90 % ladder and the no-stall rule ([landings](landings.html)).
