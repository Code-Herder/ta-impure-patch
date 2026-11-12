# Layers and modules

One copy of the simulation and of the player-facing rules, shared by the in-game wiki, tasim, ImpAI and the game. The player-facing rules -- Quick Build and the snap, the placement gesture, the reclaim sweep -- are written once, in `tasim_snap.c`, `tasim_place.c` and `tasim_sweep.c`, which the game's `tagpu_resource.c`, `tagpu_place.c` and `tagpu_sweep.c` and the wiki's front end call through `WORLDQ`. The simulation is still written twice: in `sim.js` (3,747 lines), which keeps its own copy of the rules until it is retired, and in its C port, the shared core `tasim_*.c` (S1 carved it out of `tagpu_wikisim.c`, which keeps the wiki's front end).

## The layers

| Layer | What | Used by |
|---|---|---|
| **Consumers** | the wiki front end; the tasim harness; the ImpAI brain; the game's features (Quick Build, placement, the reclaim sweep) | — |
| **Shared rules** | placement, snapping and sweep rules, engine-free | every consumer, through `WORLDQ` |
| **`WORLDQ`** | one read-and-order interface (`worldq.h`), with two implementations: the core's (`tasim_worldq.c`), and the engine's in `ddraw.dll` (`tagpu_worldq.c`) | the rules, ImpAI, the game's features |
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

The core keeps `tagpu_wikisim.c`'s discipline: no globals, all state in a struct its caller owns, doubles with each operation rounded once (SSE2 in the DLL), no clock, a state hash and a fork. It is built three ways from one source: i686 in `ddraw.dll`, native x86-64 for tasim and tests, and the mockup's host ([retiring sim.js](sim-js.html)). `tasim_combat` is a proposal still.

### The core as S3 left it

The files are `tagpu/ddraw/src/tasim_*.c` over one internal header, `tasim.h` (the shared vocabulary -- `CELL`, `XZ`, `BOX`, `TARGET`, the acting seat `ST` -- the small helpers every module reads, `static inline`, and each module's functions), with `tasim_defs.h` for the definitions. The state is still the one `WIKISIM` of `tagpu_wikisim.h`, unchanged, and the public API is still `tagpu_wikisim_*`. Every function moved whole: its body is the one it had in `tagpu_wikisim.c`, with a shared function's name prefixed `tasim_` and nothing else changed, and `simparity.mjs` holds the result to `sim.js` natively and under Wine.

| File | Holds |
|---|---|
| `tasim_defs.h` | `tasim_def`, `tasim_fpx`, `tasim_fpz`, `tasim_mobile`, `tasim_kind`, `tasim_g_cell`, `tasim_can_build`, `tasim_type_pages`, `tasim_npages` |
| `tasim.h` | the grid's helpers `tasim_bound_cell`, `tasim_js_round`, `tasim_cell_of`, `tasim_overlap`, `tasim_box`, `tasim_fix16`, `tasim_foot_cell`, `tasim_hits`; the declarations of the rest |
| `tasim_world.c` | `enter`, `tasim_new_unit`, `tasim_seat_unit` (seats a unit on a cell), `tasim_add`, `tasim_by_id`, `tasim_structure_at`, the features (`tasim_feat_box`, `_by_id`, `_at`, `_remove`), the site test (`tasim_interior`, `tasim_vent_under_g`, `tasim_site_ok`), `tasim_centre`, `tasim_remove_unit`, `tasim_grow`, `corpse`, `tasim_kill` |
| `tasim_orders.c` | `tasim_push_order` (the one append), `tasim_splice_orders`, `tasim_shift_order`, `tasim_started`, `tasim_assign`, `tasim_order_build`, `tasim_order_target`, `tasim_arm`, `tasim_disarm` |
| `tasim_move.c` | MOVEMENT whole: the bodies and claims, the targets (`tasim_to_point`, `_site`, `_unit`, `_feat`), the step rule, the straight walk, the planner and its heap, `tasim_go`, `tasim_evacuate`, `tasim_reach` |
| `tasim_econ.c` | `metal_byte`, `tasim_extractor_speed`, `tasim_econ_request`, `tasim_econ_counts`, `econ_settle`, `tasim_econ_pass`, and `tagpu_wikisim_army` |
| `tasim_work.c` | `stance` (INBUILDSTANCE), `tasim_spawn` (a unit appears), `credit`, the feature's reclaim handler `feat_work`, `tasim_advance` (the tick's work), `tasim_drop_work` |
| `tasim_sight.c` | `tagpu_wikisim_sight_lit` and `tagpu_wikisim_contacts` with their stamp |
| `tasim_cob.c` | the binding to the COB host: `tasim_facts`, `tasim_cob_call` |
| `tasim_fx.c` | the nano spray and the sound: `tasim_spray`, `tasim_spray_tick`, `tasim_sound_named`, `tasim_notice`, `tasim_notice_active`, `tasim_sound_tick`, `tasim_begin`, `tasim_cant_said`, `tasim_death_sound` |

Where S1 differs from the table above:

- **`tasim_defs` is the accessors only.** The loaders are still `tagpu_wikidata.c`'s, and the casting's binder (`bind`) is still the front end's: both move when the definitions grow past the wiki's.
- **`tasim_cob` is the binding, not the host.** The host stays `tagpu_wikicob.c` under its own name, which `serve.py` and the checks build as they did.
- **The state hash and the fork stay in the front end,** with the view and the other entry points: the hash's items read the input seats and the sidebar, and the fork rewrites the script, both the front end's.
- **The spray and the sound are in the state hash** (`tagpu_wikisim.h`, items 9 and 10), as before the carve; taking them out is a change of behaviour, not S1's.
- **The front end keeps no copy of the rules.** It builds each rule's input -- the acting seat's selected units in slot order, the cursor, Shift, the armed type -- opens the core's `WORLDQ` on the acting seat and calls `tasim_snap`, `tasim_place` and `tasim_sweep`, so a demo plays the game's rule. It keeps the selection helpers (`is_builder`, `is_reclaimer`), `unit_at` and the grid's fog reads (`seen`, `flat`) for its own stock-click paths and the view.
- **The core reads two things of the front end's** through the shared `WIKISIM`: the acting seat (`ST`, by `tasim_arm` and `tasim_disarm`), and the sidebar's pages of a type (`tasim_npages`, by `enter`, since a unit's page state is set at its create); `tasim_remove_unit` clears the sidebar's shown unit with its unit.

The build lists take the core by its name: the Makefile compiles every `src/tasim_*.c` on SSE2 with `tagpu_wikisim.c`, `tools/wiki/tasimsrcs.mjs` lists them for `simparity`, `framecheck`, `scenecheck`, `stagecheck` and `sidecheck`, and `wikilaycheck.py` globs the same files.

## The shared rules

| Module | Holds | Comes from | Wiki | tasim | ImpAI | Game |
|---|---|---|---|---|---|---|
| `tagpu_placegeo` | the shapes and the take-off arithmetic, integers only | shared | ● | ● | ● | ● |
| `tagpu_sweepgeo` | the ring's test and the split, SSE2 | shared | ● | | | ● |
| `tasim_snap` | the extractor and vent snap, the patch scan, the queued-plan rules, Quick Build's choice and commit | `tagpu_resource.c` | ● | ● | ● | ● |
| `tasim_place` | the gesture's builders, each site judged by the view's site test, the take-offs, the commit's removals and appends | `tagpu_place.c` | ● | | ● | ● |
| `tasim_sweep` | which features a ring takes, the reclaimers' split inputs, the commit's removals and appends | `tagpu_sweep.c` | ● | | | ● |

Each rule is engine-free and reads a world only through the `WORLDQ` it is given: a const ops table and an instance opened for one call on one seat, copying lists into the caller's scratch. The engine's view (`tagpu_worldq.c`) bounds every read as the game's modules did, enters the order roots `0x43ADC0`, `0x43AFC0` and `0x439F80` only after re-deriving the unit from its slot, and removes a node only while its place still holds the action, position and flags the copy had; the core's (`tasim_worldq.c`) numbers units and types as the engine does (slot = index + 1, type = entry type + 1, 16.16 positions). `WQ_MAP.list_cap` is the world's own bound on a list: 0 in the engine, `PLACE_LIST_CAP` in the core. Two answers differ between the views by construction: the engine's mapped test answers for the viewed player alone (`0x408090` tests `1 << main+0x2A43`, [map](../engine/map.html)), so its view refuses a mapped query for any other seat; and the core's mapped test is the 16-px explored cell, not the engine's sheared 32-px block.

## What stays outside

- **The wiki front end:** the UI emulation (the cursor, the sidebar's gadgets, the page router, what a press lands on, the click window); scripted input (the build and pick trips, the glide, the input timeline); selection and casting. Drawing is already in separate files.
- **The tasim harness:** a C driver, `tasim_run` (load, seat the brains, run, print JSON), built for i686 too so whole games get the Wine parity check; and Python in `tools/tasim/` for tournaments, Elo, sweeps and calibration. Nothing goes into the harness that the wiki or ImpAI could need; that goes into the core.
- **No C++.** The core, the rules and the brain are C.

## The order

S1 carves the core with no change in behaviour, while `sim.js` still exists so that `simparity.mjs` proves it. S2 retires `sim.js`. S3 moved the game's rules above `WORLDQ` ([landings](landings.html)).
