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

ImpAI wins every fair game against OTA AI, so a broad win count no longer tells whether a brain change helped. Each brain batch runs, in this order:

1. **The regression set**: the brain writer's known-hard games (the bar's former non-wins), every one a win.
2. **The win check**: the 40 `hota` games of the broad set, every one a win (Hard's 100 of 100).
3. **The paired margin test** against the last accepted build: the same `hota` ids, then `noh`, in blocks of 16, an SPRT on the sign of the time-to-win difference; *stronger?* for a change meant to strengthen, *not worse?* for a fix batch. A loss in the candidate fails it outright.
4. **The fair mod setups**, 12 games.

The full bar (500-780 games), the handicap ladder and the 16 games against the setups whose stock AI cheats run at milestones only: before the default flips and before every `v*` tag. Every batch of games registers one run in the runs viewer (`tools/tarun.py`), its games nested under it.
