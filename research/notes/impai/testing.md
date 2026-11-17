# Testing ladder

Five rungs, from unit tests to the owner's playtest. Turbo stays part of the plan, but it is not enough alone: engine games queue behind the run lock one after another, and tuning takes thousands of games. Only the headless rung can supply that many.

| Rung | What runs | Speed | What it proves | Lock |
|---|---|---|---|---|
| **0, unit tests** | the threat map, clusters, the fight predictor, the allocator, on fixed inputs | milliseconds | each part does what it says | none |
| **1, headless** | tasim tournaments: ImpAI against ImpAI or a scripted baseline, many games at once | at least 20 × turbo per core ([tasim](tasim.html)) | win rates between versions; tuning; no crash over 10,000 games | none |
| **2, engine at turbo** | ImpAI against OTA AI and against its last version, on Wine, through `tacli` | 64–230 ticks/s in battles (MEASURED), so 3–9 minutes per 20-minute game | the headless result holds in the real game; [the bar](bar.html); the levels' calibration | `runlock` |
| **3, replay gates** | `tareplay.py landing` on ImpAI's test groups | about 11 minutes a gate | recording, puppets, rewind and takeover still work with ImpAI in the game | `runlock` |
| **4, the owner plays** | a live skirmish at normal speed | real time | it is fun, and Easy is beatable | — |

**Rung 3's test groups** are named beside the code they cover (`SIM_FEATURE`; the table and the list in [ImpAI in the engine](../modules/impai.html)): the observe three, `impai-eco`, `impai-handicap`, the fight and naval pairs, the takeover pair and `impai-raid`, a raid on the base and the builders that reaches the walk check's turn back, the pressure, the ground defence and the ring's order. A changed function no group reaches fails the gate by name (`UNCOVERED`); a group that reaches it is added when that is cheap.

**Rung 2's verdicts on a share** -- the levels' targets, the ladder's ordering and the calibration of the levels'
persona rows -- read maps, not games: games on one map are correlated, so each map's mean points (1 a win, ½ a
draw, 0 a loss) is one observation and a verdict is a t interval or a t test over the map means. The games are
played in blocks, every map of a pool once from each start, so any cut at a whole block is balanced; the rows
are chosen on a development pool of five maps and judged on the bar's five; a game at the cap is scored by the
value at the limit, read from its recording for every seat; and the ordering reads up to three looks with
O'Brien–Fleming bounds and stops for futility. `tools/impai/bar.py` holds all of it ([the bar](bar.html), *The
verdict*).

**No ImpAI change lands on headless results alone.** Rung 2 confirms every headless gain, because an AI tuned in a simulator learns that simulator's mistakes. [Calibration](calibration.html) keeps tasim close enough that the two agree.

The rung 2 speeds are the turbo measurements in [Sim step and clock](../engine/sim.html): k = 1, battles from 64 ticks/s (twelve games at once) to 230 (one), against about 60 at game speed 20. A 20-minute game is 36,000 ticks.

## Per brain batch and at milestones

ImpAI wins every fair game against OTA AI, so a broad win count no longer tells whether a brain change helped. Each brain batch runs one command:

```
tools/impai/batch.py --test CANDIDATE.dll --base CHAMPION.dll --tag TAG [--mode notworse|improve]
```

It plays every gate with the candidate's DLL and stops at the first that fails, in this order:

1. **The regression set**: the brain writer's known-hard games, `tools/impai/regress.txt` -- the fourteen verification games, the extras that capped or fell at an earlier build, and ProTA's two, 19 games -- every one a win.
2. **The win check**: the 40 `hota` games of the broad set (games 1 and 2 of each map and side pairing), every one a win (Hard's 100 of 100).
3. **The paired margin test** against the champion ([the bar](bar.html), *The paired margin test*): a GSPRT on the mean pair score of the time to win, NOT-WORSE for a fix batch unless `--mode improve`. A loss in the candidate fails it outright, and no decision is no pass.
4. **The fair mod setups**: the four `nota` games of the broad set's mod pieces on the Community Patch Loader route, the 3.9.02 exe with TADR and ProTA 4.8 (`loader+tadr-ota`, `392+tadr-2026.8.6`, `prota-4.8`), 12 games, every one a win.

Beside them it reports, and judges nothing, **the held-out check**: the eight `p:L5` games OTA AI won on Steel Jungle and Lava Alley in round 3 at 57d39d71, each game's outcome and whether ImpAI reached the cap ahead in value. They wait on the brain's fix for those maps; `--gate-held-out` makes them gate, every one a win.

A win is ImpAI's opponent gone before the cap; a game at the cap is no win, and stalls are reported. The regression set, the win check and the mods play beside each other in pieces of one wave of games each -- one `bar.py play` a setup's piece, seven lanes on the base game and four on a mod setup, a game of two gates played once -- and the held-out games beside them, the pieces queued on the run lock in that order, two seconds apart. A piece stopped 600 s after its admission could not run; a piece with a game that could not run is played once more. Once a game of a gate is lost, every gating piece still playing is stopped, the margin test is skipped and the held-out check plays on. The margin test, the dearest, runs only once the others have passed. Both DLLs are copied into `.data/impai/runs/batch/<tag>/` first, where the results, each piece's log and the one-screen `summary.txt` go. Exit 0 every gate passed, 1 a gate failed, 2 a gate could not run. `test_batch.py` holds it offline on a fake player. MEASURED 2026-11-16, the 57d39d71 build against itself on the reference setup: every gate passed -- 19 of 19, 40 of 40, the margin test accepted at block 2 (32 pairs, all ties), 12 of 12 -- in 27.7 min, the bar games in 15 min (no piece past 4 min after its admission) and the margin test's two blocks in 12.6; the held-out games ended as in round 3, one lost and seven at the cap with ImpAI at 0.42 to 0.60 of OTA AI's value.

**The exploiter suite** (`tools/impai/exploit.py run --dll DLL --tag TAG [--puzzles FILE]`, [the bar](bar.html), *The exploiter suite*) hunts one blind spot a situation: an air rush, a naval turtle, artillery creep and a commander rush against a scripted opponent, each a short seeded game of its own launch (cap 6 000 to 20 000), and puzzles taken over from the build's own recordings of the games it did not win (`exploit.py cut`). A failed situation is a finding for the brain writer, not a gate.

The full bar (500-780 games), the handicap ladder (`ladder.py run`, 120 games in six looks) and the 16 games against the setups whose stock AI cheats run at milestones only: before the default flips and before every `v*` tag. Every batch of games registers one run in the runs viewer (`tools/tarun.py`), its games nested under it: `batch.py`'s run has a step a gate, the margin test's run nested under it; `ladder.py`'s has a step a look.

## One-call diagnosis

`tools/impai/diag.py` gives the brain writer's diagnosis loop in one call. It plays a few games, or reads games already played, and prints only the lines asked for, at most 60 by default:

```
tools/impai/diag.py run --dll D [--dll D2] --ids ID[,ID...] | --situation NAME[,...] [--cap T]
                        [--grep REGEX]... [--lines N] [--at T[,T...]] [--near W] [--max M] [--lanes L] [--out DIR]
tools/impai/diag.py logs DIR [DIR2] [--grep REGEX]... [--lines N] [--at T[,T...]] [--near W] [--max M]
tools/impai/diag.py build [--export NAME] [--define K=V]...
```

- **`run`** plays bar ids or exploit situations as `bar.py play` does: under the run lock, on GDI with no draw, each game from its own seed. Each DLL's games go to a folder of their own (`dll1`, `dll2` under `--out`, by default `.data/impai/runs/diag/run-<time>/`). A game already in its folder is not played again, so a second call with other patterns reads the same games. The print is then `logs`'s.
- **`logs`** prints each game's result line: the winner, the end tick and the seed, or why the game could not run. Under it come the game's `impai:` lines that each `--grep` matches, the first N of each pattern (`--lines`, 10 by default), with a count of the rest.
    - `--at` keeps only the lines whose GameTime (`t=` or `gametime=`) lies within `--near` ticks (300 by default) of a GameTime it names.
    - Given two folders, it prints the first GameTime at which the two games' matched lines differ, with both lines. Every matched line is compared, not only the printed ones.
    - Whatever the `--max` cap leaves out is counted in the last line.
- **`build`** makes a scratch DLL of the tree as it is on disk, uncommitted edits included, into `.data/impai/runs/diag/<NAME>/ddraw.dll`. It leaves `tagpu/ddraw`'s own objects and DLLs alone.
    - It copies `tagpu/ddraw` (sources and generated headers, no build products) to `<NAME>/tree/tagpu/ddraw`, beside links to the tree's `tools`, `art`, `mods` and `unditherer`, and builds it there with `tools/runlock.py make`.
    - Each `--define K=V` is a `CPPFLAGS += -DK=V` line in the copy's `config.mk`. Defines that differ from the copy's last build start it from no object, since make cannot see a flag change.
    - It prints the DLL's path, or only the compiler's errors and warnings.

`test_diag.py` covers the arguments and the extraction offline, against a fixture log and a fake player.
