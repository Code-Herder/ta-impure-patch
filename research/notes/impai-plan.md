# ImpAI and tasim: plan

**Written** 2026-11-11, after a design interview with the owner. Nothing here is built. It plans a
new skirmish AI, **ImpAI**, and **tasim**, a headless simulation of the whole game for testing it,
built on **one simulation core** that the in-game wiki, tasim, ImpAI and the game share. Engine
facts are cited from [the engine map](engine/orders.html) with how each was established; the rest
is design. Like the other plan notes, this one is folded into the module notes and `roadmap.md`
when the work closes, and then deleted.

## The decisions (owner, 2026-11-11)

### ImpAI, as the player sees it

- **A computer player's AI button switches between ImpAI and OTA AI** (the stock AI). The switch
  lands at A1 with OTA AI the default, and the default flips to ImpAI at A2, once every level
  passes the bar below. A seat that plays OTA AI runs the stock code untouched.
- **The level is the skirmish screen's global Difficulty**: Easy, Medium and Hard pick ImpAI's
  Easy, Normal and Hard. There is no per-seat level, as in stock TA, and no separate default:
  ImpAI follows the remembered `SkirmishDifficulty`.
- **ImpAI never cheats, in either direction.** No vision, resource or unit advantage, and no
  resource penalty either. `WORLDQ` (below) filters what an ImpAI seat can read to what that seat
  sees, so the rule holds by construction, in the game and in tasim alike.
- **ImpAI seats are exempt from the engine's difficulty handicap.** Stock TA's Difficulty scales
  only AI players (controller 2): income in the economy pass `0x401441..0x40175E`, reclaim at
  `0x4238EF`, metal transfers at `0x464D3C` and build credit, 50 % on Easy and 70 % on Medium
  (cited: `engine/recorder.md`, `engine/network.md`, `engine/map.md`; the full rate on Hard is
  [INFERRED]). Those sites skip ImpAI seats; OTA AI keeps the stock handicap. This is a
  simulation change and carries replay test groups.
- **Three levels, one brain.** A level is a row of parameters for the same code: reaction delay,
  an order rate cap, micro (retreat, focus fire), noise in the fight predictor's and the
  economy's judgement, attack thresholds and scouting frequency. Never resources.
- **The bar**, every game 1v1 on five maps (land, sea, mixed, small, large) with both sides on Arm
  and on Core, commander death ending the game, and no result by 45 game minutes counted as a
  failure:
  - each ImpAI level beats OTA AI on the same Difficulty in **100 of 100** games; any loss is a
    defect to diagnose, not noise (the owner will re-evaluate the bar once results exist);
  - ImpAI Normal beats ImpAI Easy, and Hard beats Normal, in at least 90 % of games;
  - Easy stays beatable by a new player, judged by the owner's playtest;
  - ImpAI never stalls: no game where its army or economy stops for more than two game minutes.
- **No memory between games.** ImpAI picks its opening from the map and from scouting, with a
  fixed seed per game, so every game is reproducible.
- **Allied ImpAI seats are separate brains**, sharing only what the game shares with allies.
- **Replays:** the recording's header names each seat's AI and level. On a replay puppet ImpAI does
  not run (the recorder is the only writer); a takeover or rewind resumes its `SIM_STATE`.
- **Saved games:** on load ImpAI rebuilds its view from the world; the save format does not change.
- **Skirmish first**; ImpAI seats in network games come last (A4), run by the peer that hosts them.
- **Mods:** every setup of the compatibility suite where Impure runs a game. A mod passes when
  ImpAI never stalls in it and no key its units use is unmodelled; the 100/100 bar is the base
  game's. One tuning table serves every mod, on roles derived from the units' data.
- **Cost:** at most 1 ms per ImpAI seat per simulation tick, worst case, on the reference setup,
  measured on a ten-seat stress scenario. The work is bounded by counts and spread across ticks by
  a fixed schedule, never by a clock.
- **Not now:** the threat-map overlay is a `tacli` test lever, never a player setting; a wiki entry
  for ImpAI is decided at A2.

### The shared core

- **One simulation core, in C**, carved out of `tagpu_wikisim.c`, compiled into `ddraw.dll`
  (i686, SSE2), natively for tasim, and for the mockup's host. The wiki, tasim, ImpAI and the game
  all use it; nothing is written twice.
- **`sim.js` is retired.** The mockup calls the C core through `serve.py`, as `cob.js` already
  calls the C COB host (`tagpu_wikicob.c`). The mockup stays the specification of the wiki's layout
  and drawing; the C core and its notes are the simulation's. `simparity.mjs` becomes a parity check
  of one source across three builds.
- **No simple mode.** The wiki inherits every fidelity change to the core; its demos are reviewed
  through the wiki gate whenever movement or combat changes.
- **No C++.** The core, the rules and the brain are C. The tasim harness is a C driver
  (`tasim_run`: load, seat the brains, run, print JSON) plus Python orchestration in
  `tools/tasim/`; nothing in the harness is anything the wiki or ImpAI could need.
- **BARb (CircuitAI) is inspiration, not source.** It is GPL v2 and Impure is MIT: its design is
  read, no code is copied or translated.

### tasim and calibration

- **tasim simulates the whole game as Impure ships it**: every unit, weapon, feature and move class
  of the mod; the whole economy; building, repair, reclaim, resurrect and capture; ground, hover,
  amphibious, sea, submarine and air movement; transports; every weapon kind; damage, wrecks,
  fire, experience and the end of the game; sight, radar, sonar, jamming, cloak and stealth; and
  Impure's own engine: the extra-weapons module, the raised limits, the defect patches and the
  TADR sim fixes.
- **Coverage is measured.** L0 adds to the engine map an inventory of every FBI, weapon-TDF and
  `moveinfo` key the engine's parsers read. `tasim_defs` loads against it and names every unit and
  key it does not model.
- **Speed:** on the same scenario, tasim on one core runs at least 20 × the game's turbo (battles
  run 64–230 ticks a second at k = 1, `engine/sim.md`, *The scheduler, the clock and the test
  pacing*), checked at every T step.
- **Calibration bands are the engine's own spread.** The same scenario does not replay the same in
  TA: weapon spread draws the CRT `rand` that the game thread's draw path also draws every frame
  (`engine/sim.md`, *The meteor shower and the CRT rand*). So:
  - **exact** metrics (build time at a given build power, cost per tick, an extractor's income,
    range, reload, damage per hit, sight radius, storage) match exactly, one run each against the
    recorder's dump;
  - **win rates** agree within two standard errors of the difference;
  - **distributions** (time to kill, survivors' value, travel time, the income curve): tasim's
    median inside the engine's 10th–90th percentile, its P90 − P10 between half and double the
    engine's;
  - **ImpAI's fight predictor** agrees with the engine's majority on at least 90 % of decisive
    fights (engine win rate above 80 % or below 20 %);
  - **50 engine runs per scenario**, each perturbed (start positions jittered within one footprint
    cell, first orders staggered 0–15 ticks), the same perturbations seeded in tasim.
  A band is loosened only per metric, with a written reason, and loosening is the owner's decision.
- **The engine reference** (every run's metrics and the DLL that produced them) is recorded once
  per scenario and again only after a landing changes Impure's simulation. tasim replays the whole
  set offline on every change, with no game and no run lock, and reports each metric in or out of
  band and by how far.
- **The scenarios:** duels and movement courses generated from the mod's data, three unit pairs per
  role pair per tech level (the cheapest, the median and the most expensive of each role), plus
  hand-written scenarios for what a generator cannot set up: the economy opening, a base assault,
  naval fights, air raids, and the ten-seat stress game.

## The design

### Layers

| Layer | What | Used by |
|---|---|---|
| **Consumers** | the wiki front end (UI emulation, trips, input timeline); the tasim harness; the ImpAI brain; the game's features | — |
| **Shared rules** | `tagpu_placegeo` and `tagpu_sweepgeo` (shared today); `tasim_snap` (the extractor snap scorer and the quick-build choice, from `tagpu_resource.c`); `tasim_place` (the placement gesture, from `tagpu_place.c`); `tasim_sweep` (ring targets and commit, from `tagpu_sweep.c`) | every consumer, through `WORLDQ` |
| **`WORLDQ`** | one read-and-order interface: units, defs, map, economy, what a seat sees; orders out. Two implementations: the core, and the engine adapter in `ddraw.dll` | the rules, ImpAI, the game's features |
| **The core** | `tasim_defs`, `tasim_world`, `tasim_orders`, `tasim_move`, `tasim_econ`, `tasim_work`, `tasim_sight`, `tasim_cob` (over `tagpu_cobvm.c`), `tasim_combat` (new), `tasim_fx` (presentation events, outside the state hash) | the wiki and tasim, which own a world |

The core keeps `tagpu_wikisim.c`'s discipline: no globals, all state in a struct its caller owns,
doubles with each operation rounded once, no clock, a state hash and a fork. Module names are
proposals.

### The ImpAI brain

- **Spatial maps:** a threat map per layer (ground, air, water) of seen enemy DPS over range, decaying
  after contact is lost; an influence map; metal clusters from the snap scorer; reachable areas per
  move class. Routes are chosen as threat-weighted waypoints; the engine paths the unit.
- **Task allocation:** economy, builder, factory and military managers; every unit holds one task
  and is re-tasked when idle or when its task fails; roles are derived from unit data relative to
  the mod's own spread.
- **Adaptation:** the enemy's composition by role from what has been seen; response tables (data,
  tuned by the harness) turn it into role demand; scouting is staffed on a schedule and when the
  picture goes stale.
- **The fight predictor:** two groups in, the winner and both sides' remaining value out, stepped
  coarsely on `tasim_combat`'s weapon model; it decides engage, hold or retreat and when a wave
  is big enough.

### How ImpAI enters the game

Cited from `engine/orders.md`, *Direct AI order activity observer* (DISASSEMBLED):

- the players phase calls `0x408C40` at `0x465031` with a seat's AI object; it runs only for an
  active seat with controller 2;
- `0x408C40` runs the group assignment `0x408830` every 30 calls, dispatches due planner objects
  from ten slots at `AI+0x11` through their vtable, then the weapon target scan `0x4089A0`
  (which human seats run too);
- orders are built by the common constructor `0x43A0C0`, reached through the helper `0x43ADC0`.

The proposed seam: for an ImpAI seat, skip the stock planners in `0x408C40`, keep the weapon scan,
and run the brain there on the game thread. Orders go through the engine's own order roots. Which
root takes move, attack, build and repair cleanly is L0's first question.

From the same lookup (`gui-gadgets.md`, `engine/recorder.md`, `engine/units.md`): the Difficulty
gadget is global and stored at `main+0x37EEE` (written at `0x4311D1`; registry `SkirmishDifficulty`
written at `0x4315A0`); a seat's only AI setting is its controller byte at record `+0x73`; the AI
profile scripts (`ai\default.txt` and the per-type texts, loaded through `0x4648E0`) do not depend
on difficulty. **Not documented yet:** which `SKIRMISH.GUI` gadget a seat click cycles, and its
values. L0 finds it.

### Testing ladder

| Rung | What runs | What it proves | Lock |
|---|---|---|---|
| 0 | unit tests of the maps, the predictor, the allocator | each part does what it says | none |
| 1 | tasim tournaments, many games at once | win rates between versions, tuning, no crash over 10 000 games | none |
| 2 | engine games at turbo, ImpAI against OTA AI and its last version | the headless result holds in the game | `runlock` |
| 3 | `tareplay.py landing` on ImpAI's test groups | recording, puppets, rewind and takeover still hold | `runlock` |
| 4 | the owner plays | it is fun, and Easy is beatable | — |

No ImpAI change lands on headless results alone.

## Landings

| Id | What | Done when |
|---|---|---|
| **L0** | Research: the AI seat's lifecycle; the order roots; the skirmish screen's AI button and where a seat's choice lives; what the stock planners do; the engine's key inventory; the handicap's sites | the engine map holds each address with its evidence, the key inventory is a page of it, the seam has its safety argument |
| **S1** | Carve `tagpu_wikisim.c` into the core and the wiki front end, behaviour unchanged (runs beside L0) | `simparity.mjs` equal natively and under Wine; the wiki gate passes |
| **S2** | The mockup calls the C core through `serve.py`; `sim.js` deleted | every demo plays through the host, the hashes match the pre-S2 ones, no JavaScript simulation left |
| **S3** | The game's rules move above `WORLDQ`; the engine adapter is its first engine implementation | `place_regression.py`, `resource_regression.py`, `sweep_regression.py` and the wiki gate pass |
| **L1** | ImpAI's world view and spatial maps, read-only, with the overlay lever; the stock AI still plays | the overlay matches a recorded game; the replay gates pass with ImpAI observing |
| **C1** | The calibration runner: scenario reload in a live game, perturbed runs, references, the offline check and its scoreboard | one scenario of each kind produces a reference and a report; the cost per scenario is measured |
| **T1** | tasim, the ground game: every key loaded or named, terrain and water, ground and sea movement, the whole economy, all the work orders, every sensor | the economy and movement scenarios are in band; the wiki's demos reviewed on the new movement; ≥ 20 × turbo |
| **A1** | ImpAI builds: economy, builders, factories; the ImpAI / OTA AI switch, OTA AI the default; the handicap exemption | ImpAI's income at 10 minutes beats OTA AI's on three maps, in tasim and in the engine |
| **T2** | tasim, combat: every ground and sea weapon kind, damage, wrecks, fire, experience, self-destruct, the commander's explosion, the game's end | the duel, brawl, naval and base-assault scenarios are in band |
| **A2** | ImpAI fights: the fight predictor, the military manager, the three levels; the default flips to ImpAI | the predictor's 90 % check; the bar, the ladder and no-stall at every level |
| **T3** | tasim, air and specials: flight, bombers, transports, cloak, stealth, stockpiles, nukes and interceptors, kamikaze | those scenarios in band; no unmodelled key left in the base game |
| **A3** | ImpAI adapts: role composition, response tables, the tournament harness that tunes them, air play | a tuned table beats the hand-written one headless, and the win holds at rung 2 |
| **T4** | tasim, Impure's engine and the mods: extra weapons, raised limits, the defect patches, the TADR sim fixes; the compatibility-suite mods | no key of a supported mod unmodelled; each mod's calibration subset in band |
| **A4** | ImpAI everywhere: network games, every supported mod | the compatibility suite and a two-player recorded game with ImpAI seats come back clean |

## Open

- The 100/100 bar is to be re-evaluated by the owner once results exist.
- C1 verifies that a scenario can be reloaded into a running game instead of relaunching; the
  calibration set's cost (estimated at a few hours of engine time, once) depends on it.
- The seat click's gadget and values are not documented (L0).
