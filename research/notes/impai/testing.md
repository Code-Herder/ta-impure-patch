# Testing ladder

Five rungs, from unit tests to the owner's playtest. Turbo stays part of the plan, but it is not enough alone: engine games queue behind the run lock one after another, and tuning takes thousands of games. Only the headless rung can supply that many.

| Rung | What runs | Speed | What it proves | Lock |
|---|---|---|---|---|
| **0, unit tests** | the threat map, clusters, the fight predictor, the allocator, on fixed inputs | milliseconds | each part does what it says | none |
| **1, headless** | tasim tournaments: ImpAI against ImpAI or a scripted baseline, many games at once | at least 20 × turbo per core ([tasim](tasim.html)) | win rates between versions; tuning; no crash over 10,000 games | none |
| **2, engine at turbo** | ImpAI against OTA AI and against its last version, on Wine, through `tacli` | 64–230 ticks/s in battles (MEASURED), so 3–9 minutes per 20-minute game | the headless result holds in the real game; [the bar](bar.html) | `runlock` |
| **3, replay gates** | `tareplay.py landing` on ImpAI's test groups | about 11 minutes a gate | recording, puppets, rewind and takeover still work with ImpAI in the game | `runlock` |
| **4, the owner plays** | a live skirmish at normal speed | real time | it is fun, and Easy is beatable | — |

**Rung 3's test groups** are named beside the code they cover (`SIM_FEATURE`; the table and the list in [ImpAI in the engine](../modules/impai.html)): the observe three, `impai-eco`, `impai-handicap`, the fight and naval pairs, the takeover pair, and `impai-raid`, a raid on the base and the builders that reaches the walk check's turn back, the pressure, the ground defence and the ring's order. A changed function no group reaches fails the gate by name (`UNCOVERED`); a group that reaches it is added when that is cheap.

**No ImpAI change lands on headless results alone.** Rung 2 confirms every headless gain, because an AI tuned in a simulator learns that simulator's mistakes. [Calibration](calibration.html) keeps tasim close enough that the two agree.

The rung 2 speeds are the turbo measurements in [Sim step and clock](../engine/sim.html): k = 1, battles from 64 ticks/s (twelve games at once) to 230 (one), against about 60 at game speed 20. A 20-minute game is 36,000 ticks.
