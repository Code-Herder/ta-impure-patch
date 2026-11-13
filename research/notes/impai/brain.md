# The brain

How ImpAI decides, in four parts. It follows the design of the strongest shipped RTS AIs (BARb and the Brood War competition bots): hand-written heuristics on spatial maps, plus a combat simulator. None of it is machine-learned.

BARb (CircuitAI) is read for its design only. It is GPL v2 and Impure is MIT, so no code is copied or translated.

## Spatial maps

- **A threat map** per layer (ground, air, water), on a coarse grid. Each seen enemy adds its DPS over its weapon range. An entry decays after contact is lost, so a remembered threat fades instead of vanishing.
- **An influence map:** friendly value minus enemy value, which gives the front line and safe expansion.
- **Metal clusters:** TA maps carry a metal map, not spots. Spots are extracted with the snap scorer Quick Build already uses (`tasim_snap`, today in `tagpu_resource.c`), then grouped.
- **Reachable areas** per move class (`moveinfo.tdf`: footprint, maximum slope, water depth), so no unit is ordered where it cannot go.
- **Routes** are chosen as threat-weighted waypoints, with A* over the threat grid (`tasim_move`'s planner). The engine still paths the unit itself.

### As built (L1)

What exists, engine-free in `impai_maps.c` over a seat's knowledge ([ImpAI in the engine](../modules/impai.html), *What a seat knows*):

| map | cell | per cell | size | kept |
|---|---|---|---|---|
| threat, one per layer (ground, air, water) | 256 px, at most 128 × 128 | u32: the summed damage per second × 16 of identified enemy rows whose type's range reaches the cell's centre | 3 × 64 KB | derived on demand |
| influence | 256 px | i32: own value minus identified enemies', over the cell's 3 × 3 box | 64 KB | derived on demand |
| presence | 256 px | u16: enemy rows known by radar alone or as ghosts | 32 KB | derived on demand |
| the threat table | a unit type | `IMPAI_THREAT`, 16 B: damage per second × 16 against ground, air and water, ground range, value, layer, role flags | 256 KB (16 384 types) | `SIM_CONTENT`, built at every level init |
| reach labels | 32-px block, per movement class the level has | u16: the 4-connected component of the block's first passable cell | ≤ 32 classes × (W/2)(H/2) × 2 B; 15 classes × 321 × 324 on Core Prime Industrial Area, 3.1 MB | `SIM_CONTENT`, built at every level init |
| metal patches | a closed, explored metal patch | `SNAP_PATCH`, 14 B | 256 published and 256 in the scan, a seat | `SIM_STATE` |

Not built yet: the decay of a remembered threat (a ghost row keeps its type's threat until its block is seen again), routes, and a stored copy of the maps (A1 decides whether a decision tick derives them or keeps them).

## Task allocation

- **Managers:** economy (the income ratio, storage, when to tech), builders, factories and military (squads that raid, defend, attack or retreat).
- **Every unit holds one task** and is re-tasked when it goes idle or its task fails.
- **Roles are derived from unit data**: builder, raider, assault, artillery, anti-air, scout, defence. The thresholds are relative to the mod's own spread of units, so any mod works with no hand-written unit lists.
- **Placement** reuses the game's own rules: `tasim_snap` for extractors and `tasim_place` for lines and grids ([layers](shared-core.html)). On a metal map -- the threshold 255, so no patch, every cell the background, as on Core Prime Industrial Area -- an extractor takes a base-ring site like any building, the stock AI's general search there ([ImpAI in the engine](../modules/impai.html), the brain).

## Adaptation

- **The enemy's composition by role**, from what has been seen. Radar dots count as unknown ground.
- **Response tables** turn that composition into role demand, for example more anti-air when enemy air passes a share of the seen value. The tables are data, tuned by the tasim harness (A3), one table for every mod.
- **Scouting** is a task the military manager staffs on a schedule, and again whenever the enemy picture goes stale.
- **Openings** are picked per game from the map and scouting. There is no memory between games.

## The fight predictor

- Two groups in; the winner and both sides' remaining value out.
- Stepped coarsely on `tasim_combat`'s weapon model: damage against the target's category, reload, range, projectile speed, accuracy and area of effect. It models focus fire and overkill.
- It decides engage, hold or retreat for every squad, which targets are worth attacking, and when an attack wave is big enough.
- Calibrated against the engine: it must agree with the engine's majority on at least 90 % of decisive fights ([calibration](calibration.html)).

## What it has to beat

What the stock AI does, from the engine map ([orders](../engine/orders.html), *The stock AI's helpers*; DISASSEMBLED, not measured):

- **It builds by a weighted random pick.** Each type on a builder's list gets a score from storage, income and per-type ratings scaled by the profile's weights, and one is drawn in proportion to its score. It builds nothing while stored energy is below 50 or metal below 25.
- **It builds on unseen ground.** Sites are random points on a per-seat lattice around its base, tested with no sight or explored-map check; its metal spots are every metal cell on the map.
- **It attacks with six or more armed units, every 300 ticks, at the enemy unit nearest its force**, chosen from every enemy on the map: its target picker has no sight, radar or explored-map test. Three or fewer regroup at the base.
- **Its commander stops building at five builders** (the seat's complete units with a build list, factories included) and roams the base instead; a hit commander drops its orders and pauses for 30 to 329 ticks.
- **Its weights and limits change with the Difficulty** through the profile's `plan` sections, and on Easy and Medium the engine cuts its resources to 50 % and 70 %.

## Levels

A level is one row of parameters for this same brain: reaction delay, order rate, micro, judgement noise, aggression and scouting ([ImpAI for the player](player.html)).
