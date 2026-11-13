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
- Played in the engine at turbo (rung 2 of the [testing ladder](testing.html)), under the run lock.

## The harness

`tools/impai/bar.py` (`ids`, `play`, `judge`; `test_bar.py` holds its game list, seating, start schema, stall rule
and judge offline). A pair is a list of game ids `bar-<pair>-<map>-<sides>-<n>`: `eota`, `nota`, `hota` (ImpAI
Easy, Normal, Hard against OTA AI at Easy, Medium, Hard), the ladder pairs `ne` and `hn` (through the level
lever, at Hard), and `dry`, OTA AI against OTA AI at Hard. Each ImpAI pair is the five maps × four side pairings
× five games, the player under test at the map's first start in games 1, 3 and 5; the dry run is ten games.

- **The maps**, one of each kind by the rule the specification states (at least three starts, no uniform metal;
  land, sea and mixed from the middle third of the 90 such retail maps by area, small from the smallest third,
  large from the largest, each the median of its class): **Pincushion** (land), **Red Triangle** (sea, 75 %
  under sea), **Trout Farm** (mixed, 40 %), **Lava Mania** (small), **Plains and Passes** (large).
- **A game**: `scenarios/impai/bar.json` filled -- an idle human with no unit, the two players in seats 1 and 2,
  each commander placed at its start position from the map's `.ota` (the schema a three-seat skirmish reads), base
  storage 1000/1000, the end test off. A lane, a held turbo tareplay Worker, holds every 300 ticks and peeks each
  seat's units alive `+0x144`; the game ends when one is 0, or at GameTime 81 000. A launch the engine does not
  reach is "could not run", never an outcome.
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
  game of the bar has been played; the dry run's rate sizes the pieces.

## When the default flips

At A2, once all three levels pass the 100/100 rule, the 90 % ladder and the no-stall rule ([landings](landings.html)).
