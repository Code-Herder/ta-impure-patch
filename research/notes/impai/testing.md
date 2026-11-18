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

ImpAI wins every fair game against OTA AI, so a broad win count no longer tells whether a brain change helped. The gate has three tiers: the session tier after every writer session, the promotion tier when a build is put up as champion, and the milestones.

**The session tier**, minutes:

```
tools/impai/session.py --test CANDIDATE.dll --base CHAMPION.dll --tag TAG [--only NAME[,NAME...]] [--neutral] [--lanes L]
```

It plays the four exploiter situations and the confirm set (`tools/impai/confirm.txt`: Red Triangle, Trout Farm, Plains & Passes and Steel Jungle against OTA AI, Lava Mania on ProTA 4.8, and two games on metal maps, Core Prime Industrial Area and Metal Heck) with each DLL and prints a line a game: the champion's result, the candidate's, and one word.

- **SAME**: the two games' decision lines are equal -- every `impai:` line that carries a GameTime and no timing field -- and so are the results. **PARTED**: the same category, a different game; the first parting's GameTime is named and nothing is judged. **BETTER** and **WORSE**: the category changed (for a bar game, best first: won, at the cap ahead in value, at the cap not ahead, lost; for a situation: pass, FAIL). **UNRUN**: a game that could not run, or one whose log has no decision line.
- Exit 1 when a game is WORSE, 2 when one could not run, else 0. **`--neutral`** is a refactor's proof: it fails on any game that is not SAME and prints both lines at each first parting.
- **`--only`** narrows it to the situations or ids named: the item a session works on is one or two games.
- The champion's games are played once a champion, kept by the DLL's hash (`.data/impai/runs/session/base-<hash>/` in the main checkout); the candidate's are under its tag, and a tag is one build. A second run of the same pair plays nothing.
- MEASURED 2026-11-17 on the reference setup, the champion ed3e5634 against 278fc3fe, both DLLs' nine games played afresh: 8.6 min, 5.5 of them in the run lock's queue behind a ladder; 8 SAME and the naval turtle PARTED at GameTime 6 960, both at the cap. The same command again: 0.25 s.

**The promotion tier**, when a build is put up as champion, is one command:

```
tools/impai/batch.py --test CANDIDATE.dll --base CHAMPION.dll --tag TAG [--mode notworse|improve] [--base-batch DIR]
```

With `--base-batch`, the champion's own batch directory, each held-out game that ends in a worse category than the champion's same game is flagged in its line and counted (`held_out_worse` in `summary.json`); it still judges nothing. On the batches of 82cab0f3 and ed3e5634 it flags one game of eight: Lava Alley ca-4, won at 48 742 by the first and at the cap not ahead by the second.

It plays every gate with the candidate's DLL and stops at the first that fails, in this order:

1. **The regression set**: the brain writer's known-hard games, `tools/impai/regress.txt` -- the fourteen verification games, the extras that capped or fell at an earlier build, and ProTA's two, 19 games -- every one a win.
2. **The win check**: the 40 `hota` games of the broad set (games 1 and 2 of each map and side pairing), every one a win (Hard's 100 of 100).
3. **The paired margin test** against the champion ([the bar](bar.html), *The paired margin test*): a GSPRT on the mean pair score of the time to win, NOT-WORSE for a fix batch unless `--mode improve`. A loss in the candidate fails it outright, and no decision is no pass.
4. **The fair mod setups**: the four `nota` games of the broad set's mod pieces on the Community Patch Loader route, the 3.9.02 exe with TADR and ProTA 4.8 (`loader+tadr-ota`, `392+tadr-2026.8.6`, `prota-4.8`), 12 games, every one a win.

Beside them it reports, and judges nothing, **the held-out check**: the eight `p:L5` games OTA AI won on Steel Jungle and Lava Alley in round 3 at 57d39d71, each game's outcome and whether ImpAI reached the cap ahead in value. They wait on the brain's fix for those maps; `--gate-held-out` makes them gate, every one a win.

A win is ImpAI's opponent gone before the cap; a game at the cap is no win, and stalls are reported. The regression set, the win check and the mods play beside each other in pieces of one wave of games each -- one `bar.py play` a setup's piece, seven lanes on the base game and four on a mod setup, a game of two gates played once -- and the held-out games beside them, the pieces queued on the run lock in that order, two seconds apart. A piece stopped 600 s after its admission could not run; a piece with a game that could not run is played once more. Once a game of a gate is lost, every gating piece still playing is stopped, the margin test is skipped and the held-out check plays on. The margin test, the dearest, runs only once the others have passed. Both DLLs are copied into `.data/impai/runs/batch/<tag>/` first, where the results, each piece's log and the one-screen `summary.txt` go. Exit 0 every gate passed, 1 a gate failed, 2 a gate could not run. `test_batch.py` holds it offline on a fake player. MEASURED 2026-11-16, the 57d39d71 build against itself on the reference setup: every gate passed -- 19 of 19, 40 of 40, the margin test accepted at block 2 (32 pairs, all ties), 12 of 12 -- in 27.7 min, the bar games in 15 min (no piece past 4 min after its admission) and the margin test's two blocks in 12.6; the held-out games ended as in round 3, one lost and seven at the cap with ImpAI at 0.42 to 0.60 of OTA AI's value.

**The exploiter suite** (`tools/impai/exploit.py run --dll DLL --tag TAG [--puzzles FILE]`, [the bar](bar.html), *The exploiter suite*) hunts one blind spot a situation: an air rush, a naval turtle, artillery creep and a commander rush against a scripted opponent, each a short seeded game of its own launch (cap 6 000 to 20 000), and puzzles taken over from the build's own recordings of the games it did not win (`exploit.py cut`). A failed situation is a finding for the brain writer, not a gate.

The full bar (500-780 games), the handicap ladder (`ladder.py run`, 120 games in six looks) and the 16 games against the setups whose stock AI cheats run at milestones only: before the default flips and before every `v*` tag. Every batch of games registers one run in the runs viewer (`tools/tarun.py`), its games nested under it: `batch.py`'s run has a step a gate, the margin test's run nested under it; `ladder.py`'s has a step a look.

## A brain writer's session

The brain's sources have one writer at a time, and a writer works in sessions of **one queue item** each: a fresh writer a session, started from the hand-off (`.data/impai/brain-handoff.md` in the main checkout), which names the item, its scenario and the measure that says the item is closed.

1. **Find it in one call.** `tools/impai/rules.py NAME...` answers for a rule's name (`"THE HOLD"`), a function or a decision field (`hold`, `pos.pressed`, `plan.retreat_margin`): where the rule is stated, every function that names it, the fields each reads and writes, who calls whom. `--list [FILE]` names every rule with its first site; `--coupling` the fields the most functions read. It reads the sources as they are on disk, so there is no index to keep.

**Two of the session tier's games are on metal maps** (`bar.py`, THE METAL MAPS: Core Prime Industrial Area, Metal Heck and Metal Gridlock, `SurfaceMetal` 255, the brain's own test). No pool holds a metal map, so no gate of the promotion tier plays one; the tier's two are the only standing check of the brain's metal path, where the extractors stand on the base ring. MEASURED 2026-11-18 at 5643b394: `bar-hota-coreprime-cc-1` is won at 48 285 with 6 991 of 19 064 metal wasted by 20 000, and `bar-hota-metalheck-aa-2` is lost at 44 718 ([landings](landings.html), *Metal maps, read*).

**Four of the session tier's games are openings** (`tools/impai/opening.py`): ImpAI at a network game's start, the seats below it idle commanders, to GameTime 6 000, exact and recorded, about 22 s. Three are mapped, `mapped-<map>-<start>`, at starts no bar game plays: Steel Jungle 2 and Trout Farm 2, slow openings (MEASURED 2026-11-18: the first plant at 5 324 and 4 499, 4 and 5 units at 6 000), and Luschaven 3, a start with no room for the vehicle plant in the commander's reach (ARMLAB at 4 056). One is unmapped, `unmapped-luschaven-3`: the same game and seed with `mapping` false, as a network game is. An ImpAI seat reads the map's layout in both ([the brain](brain.html), *The map's layout is known in every game*), and this game shows the unmapped seat taking its mapped twin's decisions (MEASURED 2026-11-18: equal in all 32 decision lines, the plant at 4 056 in both). What the engine itself does for an unmapped seat stays. Under True line of sight, the openings' setting and the bar's (`scenarios/impai/bar.json`), that is its pathfinder, which walks the seat's units over unexplored blocks as over free ground ([map](../engine/map.html), *What Mapped gives a player*); with True line of sight off the seat's explored bit is its sight as well -- the units and gated features it sees, the occupied sites its test refuses ([ImpAI in the engine](../modules/impai.html), *What a seat knows*) -- and no opening plays that. So at other starts an unmapped game's brain state parts from its twin's on a field made from a unit's position (Steel Jungle 2 at GameTime 74, Trout Farm 2 at 390, Lava Alley 3 at 589), every order until then equal [INFERRED: the pathfinder's doing; the units' paths were not compared]. Its figures: the first order, the first plant, the first extractor ordered, and at the cap the extractors standing, the units and the metal income. SAME when the decisions are equal; BETTER or WORSE when one build alone reached an order, a plant or an extractor, or its units at the cap differ by more than 2 and a quarter of the larger, with nothing pointing the other way; PARTED otherwise. It does not judge when an order comes, nothing past 6 000 and no fight, and it is a skirmish at a network game's starts, not a network game.

2. **Look before building.** What the seat did in a game already played is `think.py`'s to answer (*A recording answered in one call*): a diagnostic DLL is built only for what no recording holds.
3. **Play first.** The change is made, built as a scratch DLL (`diag.py build`) and played on the item's scenario against the champion (`diag.py run --dll CHAMPION --dll SCRATCH`). It must move the scenario's outcome, or the measure the item names. At most three tries; a change that moves nothing is dropped -- the sources restored, what was tried and what the games showed written into the hand-off -- and the session ends.
4. **Harden what moved it**, and only that: the brain test's fixture, the mutations, [the brain](brain.html) in place. The game that showed the fault goes to [decisions](decisions.html), not into a comment: a comment keeps the rule's invariant, its engine facts and a pointer. A rule the item changes has its older measurements moved the same way.
5. **Commit and hand off**: one item, the hand-off's queue brought up to date.

**A session is about 100 turns.** Its cost grows with the square of its turns: every turn reads the whole context again, and the context grows by the turn (MEASURED 2026-11-17 on nine writer sessions: about 62 000 tokens at the first turn, 335 000 to 466 000 at the last, 126 to 312 turns, 27 to 80 million tokens read from cache a session; three sessions of 100 turns cost about half of one of 300). So:

- **Look-ups are batched**: every read and search the next step needs goes in one turn; a function is read whole, once, not in windows.
- **The brain test runs after a group of edits**, not after each.
- **An item still open at about 100 turns is handed off**: what is proven is committed, the hand-off says where it stands, and a fresh session takes it.

## A recording answered in one call

Every game the tools play keeps its recording, and a recording carries the brain's state at every tick: the military's (`IMPAI_MIL`), the economy's (`IMPAI_BRAIN`), each slot's job and what the seat knows. `tools/impai/think.py` reads it with no build and no replay, at most 60 lines a call:

```
tools/impai/think.py state    REC [--seat S] --at T [--part mil,brain,job,known] [--fields PATH,...]
tools/impai/think.py timeline REC [--seat S] --field PATH[,PATH...] [--from T] [--to T]
tools/impai/think.py squads   REC [--seat S] [--from T] [--to T]
tools/impai/think.py units    REC [--every N]
tools/impai/think.py diff     REC_A REC_B [--seat S] [--part ...]
tools/impai/think.py why      REC [--seat S] (--at T | --want NAME | both) [--from T] [--to T] [--log FILE]
```

- **A field is named as the C names it**: `mil.naval`, `mil.sq[3].task`, `mil.sq[*].task`, `job[1512].kind`; an enum prints by its name. `timeline` prints only the ticks at which a value changes; `squads` each squad's tasks with their ticks, size, value and goals; `units` the units alive a seat; `diff` the first tick at which two recordings' brain state differs and the fields that differ (exit 1).
- **The layout is the compiler's**: `thinklayout.py` compiles a probe of `impai.h` with the DLL's compiler and flags and reads the members from its debug information, cached by the headers' hash. A recording whose rows are another size is refused by name; `--tree DIR` names the source tree that made it. A layout of the same size with members moved is not detected: every headline prints the recording's build. `session.py --state` reads the base's recordings by the `--tree` layout first, so a proof whose base has rows of this tree's size is still read rightly; and where the two layouts number an enum apart, a value is compared by its name.
- **A recording's tick T holds what the step from GameTime T-1 wrote**, so the log's `t=6960` shows at tick 6961.
- **`why` answers "why was this want not taken at tick T"**, from the reason trace ([the brain](brain.html), *The reason trace*). A decision's gates, its posture and what it held of each want are scratch, so no recording holds them: the DLL writes them to the game's log, `impai: why seat= t= gates= naval= pos= need= veto= take=`, at each economy decision whose trace differs from the seat's last line or whose GameTime is not above the seat's last decision's (the first after a rewind's fork), and `why` reads the log beside the recording (`<name>.tagpu.log` for `<name>.imprec`, or `--log FILE`). `--at T` prints the gates up at tick T, the posture and each of the six wants -- not needed; needed and taken; needed and VETOED, by which gates; needed, held by no gate and not taken, which is the term its rule reads last -- each with the ticks it stands, from..until. `--want want_factory` prints that want's history, change by change, under one line of its totals in ticks (taken, vetoed and by which gate, held by its last term, not needed). A log with no `why` line was written by a build before the trace, and one whose GameTimes go back for the seat is refused by name.
- **`why --at T` ends with the plant rung's last answer** when the log holds one at or before the tick: the seat's last `impai: plantwalk` line ([the brain](brain.html), *The plant walk's trace*), which names what a taken plant want met when it ordered nothing -- the type, the builder, the ring's growth, the sites each test took and the return the walk took. It is a decision line as `why` is.
- **The `why` line is a decision line** (`diag.decisions`): every game of a build that writes it carries it, so a proof that compares a build with the trace against one without parts on that line kind alone, and compares the rest with `diag.py logs A B --grep '^impai: (?!why )'`.
- **`session.py --neutral --state`** asks more than equal decision lines: every game's brain state equal at every tick, else PARTED with the tick and the field. MEASURED 2026-11-17: the champion's naval turtle and 278fc3fe's part in state at the step from GameTime 6 172 (a builder's site), 788 ticks before their logs part.
- **A situation's figures**: `exploit.py` prints, under each verdict, and `session.py` beside it for both DLLs, the opponent's units at four ticks, the tick of ImpAI's first attack and the attacks' count, and its army at its peak. They are reported, never judged.
- MEASURED 2026-11-17 on the reference setup: a first call on a recording of 20 190 ticks 1.6 s, of 81 180 ticks 9.1 s; later calls 0.06 to 3 s, from the extract it keeps in `.data/impai/think-cache/`.

## The thought viewer

`tools/impai/thinkview.py REC [REC...] [--seat S] [--tree DIR]... [--gamedir DIR]... [--content FILE]... --out DIR [--bare]` writes a page from recordings, through `think.py`'s reader: `index.html`, one data file a recording and seat, and one picture a map. It is the owner's instrument, one view on one clock:

- **The flight recorder**, the whole game: an events row (a plant sited, a wave, a prediction lost, first contact, the commander seen, a judgement changed), a lane a judgement (naval, island, the dry hunt, the lead, the pressure, the base's threat, the estimate), a lane a squad with its tasks as spans, and four curves (units alive a seat and the army in squads, the army's value against the enemy's remembered, metal income, the enemy rows known and how).
- **The war room**, one moment on the map itself: the map's picture, the block grid as `think.py` numbers it, the metal, the features, the anchor and the base box, own units by job with extractors marked apart, the builders' sites, each squad as a ring with its goal line and trail, the estimate, and the enemy as the seat holds it -- solid in sight, a ring on radar, a cross remembered -- over the truth drawn faint. Every layer is switchable.
- **Pan and zoom**: the wheel zooms about the cursor, a drag pans, two fingers pinch, `+`, `-` and `0` zoom and fit, beside a Fit button. The markers and the text keep their size on the screen, the grid thins as the zoom falls, and the picture is drawn sharp when magnified. A pointer that moves no more than 4 px between down and up is a click, which picks a squad; the view is kept when the moment changes and fitted again when the recording does.
- Dragging on the recorder sets the war room's moment; a span or an event jumps to it; a squad picked on the map is picked in its lane; a sentence between the two says what is happening at the moment, to copy into a queue item.
- **Whose thinking it is**: the page's list is a row a game, and beside it stands a button a seat of that game -- a swatch of the player's colour, the seat's number, its side, who played it and where it started ("Seat 2 · Arm · ImpAI · north-east"). A recording several ImpAI seats played is a data file a seat (unless `--seat` names one), and its buttons pick between them at the same tick and the same view of the map; a seat a human, the game's own AI or another machine played is shown and cannot be picked. One sentence under the buttons says it whole: the seat shown, its allies, and the seats it plays against, each with its swatch. On the map a flag in each seat's colour stands where it started, with its number and name (the labels layer), every other seat's unit of the truth underneath bears a thin line in its owner's colour, and a tooltip that names a seat carries its swatch and the colour's word. The seat's own units keep the colours of their jobs.
- **Who plays is read from the recording** (`seat_facts`): the side, the controller (player record `+0x73`: 1 a human, 2 a computer, 3 another machine) and ImpAI's own byte, the colour index (PlayerInfo `+0x96`) and the alliance bytes (`+0x108`; two seats are allies when each holds the other's) from the header's seat rows, and the start from the stream: where the seat's first unit that outlives the level's first 30 ticks -- its commander in a skirmish -- stands at its first tick, as a compass word over the map's thirds. A colour index's colour is the game's own, the middle pixel of that frame of `32xlogos` in `textures/logos.gaf` through the palette, as the replay HUD takes it (`thinkmap.team_colours`), read from the recording's archives, so a mod's colours are the mod's (Escalation's differ); when they cannot be read the page uses the unmodded game's ten (`STOCK_RGB`, measured the same way) and says so. A fact that could not be read is said, never guessed.
- **Exact and sampled**: the lanes, the events, the squads' state and the seat's scalars are exact at every tick; units' places, jobs, known rows, income and features are samples at every decision tick and a regular step, and the page says which sample it shows.
- Unit types print by name there and in `think.py`, from the fight content's names when their count is the recording's; `--content FILE` names a mod's content file (`.data/calib-mods/<setup>/fight-content.json`), and the first whose count fits names the types.

**The ground** (`tools/impai/thinkmap.py`) has two sources, and the page's legend and the lines under its map say which each layer is drawn from:

- **The recording's own feature grid** (13 bytes a 16-px cell: `+0x04` the height, `+0x07` the metal, `+0x08` the feature's def; [the engine's map](../engine/map.html), *The cell*). The **metal** layer is the engine's value of every cell as the first sample holds it: the cells above the value most cells hold on an ordinary map, the whole ground on a metal map. The **features** are every anchor from the sample it is first seen at to the sample it is gone at, so a wreck made and reclaimed between two samples is never seen. The **heights** lift every marker. All three are the game's own values whatever the mod.
- **The map file**, `maps/<name>.tnt`, read from the archives the recording's header lists, in its mount order, loose files first: `--gamedir DIR` names a game directory to look in, by default the one the recording lies in (`<gamedir>/replays/`) and then the install. It gives the **picture** -- the whole map as one WebP image, its long side at most 4096 px, under 4 MB -- the sea level, and the **names** of the feature defs of the map's own table with what a reclaim gives and what blocks building, from the first `features/**.tdf` that holds each. A def past the map's table was loaded in play (a wreck, or a feature the game added) and is drawn without a name. An **extractor** is a type whose `units/<name>.fbi` in the same archives extracts metal.
- **The game's view**: the picture shows the ground as the game draws it, a point at height `h` standing `h/2` px higher, so every marker, the grid and the held ground are lifted the same way. A unit over water stands at the sea's level unless its type neither floats nor flies.
- **Checked against the recording**: the map file's size must be the grid's, or the picture is refused; its heights are compared cell for cell and a difference is a line of the page. An archive of another size than the header lists is used and named; one not found is named, and a file behind it is refused, since the missing archive may have held another copy. Impure's own archive, `impure-patch.ufo`, is left out: the DLL writes it at every start and it holds only Impure's menu screens.
- **The picture is derived from the original game**: it is written into `--out` and nowhere else, and the tests make their own map (a TNT of twelve one-colour tiles).

**The tree at the recording's build reads it.** `think.py` compares sizes alone, and two builds of the same sizes may number an enum otherwise: the job kinds of build `77e33eca` hold `EXPLORE` before `SQUAD`, a later tree's do not, and read by the later tree every squad of that build's recordings has no member. So of the trees that fit, `thinkview.py` takes one checked out at the commit the recording's header names, then each `--tree` in the order given, this tree last, and the page says when none is at the recording's build.

**What it cannot show**, the recording not holding it: a squad's own rally point (its ring is its members' centroid), a plant's completion (only its siting), the reasons (`think.py why` reads those from the log; the page does not merge them yet), a feature's name when its def is past the map's table, an aircraft's height (it is drawn over the ground under it), and any map whose file the archives found do not hold -- the page then draws the grid with the recording's metal and features and says what was missing.

MEASURED 2026-11-18 on the reference setup, a 57 211-tick game of three ImpAI seats on Wretched Ridges: 1.9 to 2.7 MB a seat's data file (jobs and known rows are written once and named by index, held ground as steps, so the default bound of 3 MB holds), a 3.9 MB picture; the page of that game, a 3 468-tick Escalation game and a Metal Heck game (seven data files, three pictures) builds in 1 min 37 s. Its seats, as the page reads them: 0 the human (Arm, blue, north-west, allied with 1), 1 ImpAI (Core, red, south-east), 2 ImpAI (Arm, white, north-east, allied with 3), 3 ImpAI (Arm, green, south-west); each start is the seat's commander at tick 1, and each flag stands on its seat's first buildings in the picture. In its page every extractor of every seat stands within 24 px of a cell above the ground's metal at four ticks (10, 27, 27 and 29 of them), and on a Trout Farm recording all 1 092 sampled places of a ship are cells under the sea's level.

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
