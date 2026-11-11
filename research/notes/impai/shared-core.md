# Layers and modules

One copy of the simulation and of the player-facing rules, shared by the in-game wiki, tasim, ImpAI and the game. Today the same rules are written three times: in `sim.js` (3,747 lines), in its C port `tagpu_wikisim.c` (5,829 lines), and in the game's own `tagpu_resource.c`, `tagpu_place.c` and `tagpu_sweep.c`.

## The layers

| Layer | What | Used by |
|---|---|---|
| **Consumers** | the wiki front end; the tasim harness; the ImpAI brain; the game's features (Quick Build, placement, the reclaim sweep) | — |
| **Shared rules** | placement, snapping and sweep rules, engine-free | every consumer, through `WORLDQ` |
| **`WORLDQ`** | one read-and-order interface, with two implementations: the core, and the engine adapter in `ddraw.dll` | the rules, ImpAI, the game's features |
| **The core** | the simulation, `tasim_*` | the wiki and tasim, which own a world |

The wiki and the tasim harness own a world and call the core directly. ImpAI and the game's features see the world only through `WORLDQ`, so the same rule code runs in the game, in a wiki demo and in a headless game.

## The core

| Module | Holds | Comes from |
|---|---|---|
| `tasim_defs` | unit, weapon, feature and move-class definitions; loaders for FBI and TDF from the install; the wiki's casting binds to it | wikisim *definitions*; the def half of `tagpu_wikidata.c` |
| `tasim_world` | the grid (height, metal, occupancy); units and features by ID; spawn, kill, corpse; the state hash; fork | wikisim *the grid*, *units*, *features*, hash and fork |
| `tasim_orders` | the bounded order list (one append, `PLACE_LIST_CAP`); order kinds; shift, splice, unqueue; arming a build | wikisim *the one append*, *orders* |
| `tasim_move` | bodies, the step rule, the A* planner over move classes; later TA's acceleration and turn rate | wikisim *movement*, *the step rule*, *the planner* |
| `tasim_econ` | the ledger, each builder's build request, settle, storage, extractor rates | wikisim *the build's request*, the economy pass |
| `tasim_work` | nanoframes, build progress, the INBUILDSTANCE gate, assist, repair, reclaim | wikisim *a unit appears*, *the reclaim handler* |
| `tasim_sight` | sight stamps and fog; later radar and jamming | wikisim `sight_*` |
| `tasim_cob` | the COB host over `tagpu_cobvm.c`, which stays as it is | `tagpu_wikicob.c` |
| `tasim_combat` | weapons, projectiles, damage and death; each weapon's DPS against a target | new |
| `tasim_fx` | nano spray, sounds and notices, as presentation events outside the state hash | wikisim *the nano spray*, *the sound* |

The core keeps `tagpu_wikisim.c`'s discipline: no globals, all state in a struct its caller owns, doubles with each operation rounded once (SSE2 in the DLL), no clock, a state hash and a fork. It is built three ways from one source: i686 in `ddraw.dll`, native x86-64 for tasim and tests, and the mockup's host ([retiring sim.js](sim-js.html)). Module names are proposals.

## The shared rules

| Module | Holds | Comes from | Wiki | tasim | ImpAI | Game |
|---|---|---|---|---|---|---|
| `tagpu_placegeo` | shapes, the take-off rule | shared today | ● | ● | ● | ● |
| `tagpu_sweepgeo` | sweep geometry | shared today | ● | | | ● |
| `tasim_snap` | the extractor snap scorer, the quick-build choice | `tagpu_resource.c` + wikisim's copy | ● | ● | ● | ● |
| `tasim_place` | the placement gesture: lines, grids, rings, spacing, blocking plans | `tagpu_place.c` + wikisim's copy | ● | | ● | ● |
| `tasim_sweep` | which features a ring takes; the commit | `tagpu_sweep.c` + wikisim's copy | ● | | | ● |

## What stays outside

- **The wiki front end:** the UI emulation (the cursor, the sidebar's gadgets, the page router, what a press lands on, the click window); scripted input (the build and pick trips, the glide, the input timeline); selection and casting. Drawing is already in separate files.
- **The tasim harness:** a C driver, `tasim_run` (load, seat the brains, run, print JSON), built for i686 too so whole games get the Wine parity check; and Python in `tools/tasim/` for tournaments, Elo, sweeps and calibration. Nothing goes into the harness that the wiki or ImpAI could need; that goes into the core.
- **No C++.** The core, the rules and the brain are C.

## The order

S1 carves the core with no change in behaviour, while `sim.js` still exists so that `simparity.mjs` proves it. S2 retires `sim.js`. S3 moves the game's rules above `WORLDQ` ([landings](landings.html)).
