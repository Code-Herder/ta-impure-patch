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

`tools/impai/bar.py` (`ids`, `play`, `judge`; `test_bar.py` holds its game list, seating, start schema, stall rule,
judge, and `play`'s stop on fake lanes, offline). A pair is a list of game ids `bar-<pair>-<map>-<sides>-<n>`:
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
  storage 1000/1000, the end test off. A lane, a held tareplay Worker at turbo k = 1, holds the level every 300
  ticks, or earlier at the first frame visit at which seat 1's or seat 2's units alive `+0x144` is 0 (`pause at T or
  units <s> <= 0`, `tagpu_replay.c` THE PAUSE AT T, which at k = 1 lands on the visit after the step in which the
  count fell), and peeks both: the game ends when one is 0, or at GameTime 81 000. Both at 0 at one look -- both
  sides gone in one tick -- is a draw, judged as a game at the cap. A launch the engine does not reach is "could
  not run", never an outcome.
- **What a result keeps for a diagnosis**: its last two looks (`looks`: the GameTime and each seat's units alive,
  and at the last each seat's commanders lost, the word `+0x106` the destructor raises at `0x486996`,
  [units](../engine/units.html)); a game no side won, and a game that could not run, also its instance log from
  the launch on, `<id>.tagpu.log` beside its JSON. A draw then reads as both commanders dying (`+0x106` 1 for
  each; [INFERRED] each death a kill-all of its side, whose units die as self-destructs, so its blasts can reach
  the other commander) or as something else, from the log's `units:` heartbeats.
- **Why k = 1, and no `--turbo`**: the turbo's k ticks a frame are not the same game drawn less often. The step
  `0x495490` runs its tail once a call, not once a tick -- the temporary sight records' expiry `0x482130` among
  it -- so a sight record outlives its deadline by up to one batch and where the batches fall changes what units
  see and do ([sim](../engine/sim.html), *The batch is part of the result*: speed 20 against a fixed k = 2 parted
  in 1 272 and 1 383 unit fields). k = 1 is the batch a player at normal speed gets; a bar game at k > 1 would be
  judged on another game. The holds would stay exact (the pause at T bounds the batch to end at T), the end's
  units clause would not (a batch of k ticks ends where it ends).
- **The rate** (MEASURED 2026-11-13, the dry run: ten lanes, OTA AI against OTA AI at Hard, GDI, k = 1, game
  speed 20): about 90 to 107 ticks a second a lane -- Trout Farm reached GameTime 30 000 in 337 s with its launch,
  Lava Mania 61 200 in about 570 s. A game run to the cap takes about 14.5 minutes, so a piece of ten minutes
  cannot hold one; how pieces are sized against that is open.
- **The renderer**: every result names it, a game that could not run included; `judge` refuses a result that
  names none, and a directory of two renderers.
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
- **`play_game(game, lane, out_dir)`** is the tuning tournament's game runner (`tools/impai/tourney.py`): one game
  of its spec on one lane, with each seat's table copied into the game folder and named by
  `tagpu_impai_table.on`, its result carrying each seat's table hash from its `impai: table` line and its value
  built and alive from its last pulse.
- **Not built**: keeping the recordings of the games ImpAI lost, stalled in or ran to the cap. **Not run**: no
  game of the bar has been played. The dry run of 2026-11-13 ended one game, Trout Farm 1, with both seats at 0 in
  one 300-tick look at GameTime 30 000 (22 and 31 units at the look before), on a level held there by the harness's
  own `pause at 30000` and nothing else -- no scenario, no end test, no console command; the offsets are the
  records' (`main+0x1B63` + seat × `0x14B` + `0x144`, [memory](../engine/memory.html)). Its log was not kept, so
  whether both commanders died in that step is not established; the next run's draw carries what settles it
  (above). Of the run's other games one ended with its game's process gone (Pincushion 1, 117 units alive at its
  last heartbeat, its log not kept either) and the rest were cut by the run's ten-minute bound.

## When the default flips

At A2, once all three levels pass the 100/100 rule, the 90 % ladder and the no-stall rule ([landings](landings.html)).
