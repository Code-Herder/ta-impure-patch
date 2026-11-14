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
| threat, one per layer (ground, air, water) | 256 px, at most 128 × 128 | u32: the summed damage per second × 16 of identified enemy rows whose type's range reaches the cell's centre; on the ground layer the type's **ground reach** (below), and in the builders' copy each radar contact never identified as the military's substitute (*Task allocation*) | 3 × 64 KB | derived on demand |
| influence | 256 px | i32: own value minus identified enemies', over the cell's 3 × 3 box | 64 KB | derived on demand |
| presence | 256 px | u16: enemy rows known by radar alone or as ghosts | 32 KB | derived on demand |
| the threat table | a unit type | `IMPAI_THREAT`, 20 B: the role's damage per second × 16 against ground, air and water and its range; the ground reach's damage and range; value, layer, role flags | 320 KB (16 384 types) | `SIM_CONTENT`, built at every level init |
| reach labels | 32-px block, per movement class the level has | u16: the 4-connected component of the block's first passable cell | ≤ 32 classes × (W/2)(H/2) × 2 B; 15 classes × 321 × 324 on Core Prime Industrial Area, 3.1 MB | `SIM_CONTENT`, built at every level init |
| metal patches | a closed, explored metal patch | `SNAP_PATCH`, 14 B | 256 published and 256 in the scan, a seat | `SIM_STATE` |

**The ground reach is the engine's rule, not the role's.** A type's role figures (`tasim_roles`: what a type is *for*) leave out a weapon whose category says NOTAIR, so a missile tower or a Samson is anti-air there; the target finder `0x40B7B0` takes NOTAIR only as a preference and falls back to the best ground target in reach when no aircraft is a candidate ([orders](../engine/orders.html)), so a Samson kills constructors. The ground layer therefore stamps `hit_ground` over `hit_range`: every regular weapon slot without `toairweapon`, whatever its category, damage × burst over reload. The role figures, the classifier and the calibration read the role's; only the threat maps read the reach (the level init logs `reach_over_role=`, the types whose two differ). MEASURED 2026-11-13 on Comet Catcher: a human's line of 29 Samsons killed 26 of ImpAI's 27 constructors.

Not built yet: the decay of a remembered threat (a ghost row keeps its type's threat until its block is seen again), threat-weighted routes (a builder's walk is tested as one straight segment, *Task allocation*), and a stored copy of the maps (A1 decides whether a decision tick derives them or keeps them).

## Task allocation

- **Managers:** economy (the income ratio, storage, when to tech), builders, factories and military (squads that raid, defend, attack or retreat).
- **Every unit holds one task** and is re-tasked when it goes idle or its task fails.
- **Roles are derived from unit data**: builder, raider, assault, artillery, anti-air, scout, defence. The thresholds are relative to the mod's own spread of units, so any mod works with no hand-written unit lists.
- **Placement** reuses the game's own rules: `tasim_snap` for extractors and `tasim_place` for lines and grids ([layers](shared-core.html)).
- **The extractor for the patch.** A builder's menu gives two extractors, each the most metal per value within its kind, as energy is ranked: the best land type and the best water-only type (minwaterdepth above 0). The type is chosen at the patch, never before it: the water type where the patch's centre lies at least its minwaterdepth under the sea, the land type everywhere else, and on a metal map the anchor's ground decides. The site test refuses a footprint whose highest cell lies above sea − minwaterdepth (`0x47D7D7..0x47D7F0`, [orders](../engine/orders.html)), so a type chosen first was refused at every patch of the other kind: on Total Mayhem and TA Twilight, whose water extractors extract more than the land ones, the seat ordered the water type at every dry patch and on Pincushion failed 118-136 sites a level with one to five extractors (MEASURED 2026-11-14). Retail's land extractor no longer rests on its type number being the lower of a tie. A constructor whose extractors suit no patch it knows explores. A shore patch whose centre lies on the other side of the line from its best footprint is refused once and waits `fail_t`. On a metal map -- the threshold 255, so no patch, every cell the background, as on Core Prime Industrial Area -- an extractor takes a base-ring site like any building, the stock AI's general search there ([ImpAI in the engine](../modules/impai.html), the brain).
- **The opening's far patch.** The commander looks for patches within `comm_r` of the anchor, except in the opening -- the seat has never had a factory this level (none counted, frames included), none is ordered, and it has fewer than `open_mex` extractors: then, with none within `comm_r`, it takes the nearest patch past it, reachable and unthreatened, within twice `comm_r` of the anchor (2048 px at every level) and within the straight line its build job can walk before `job_timeout` gives it up, `job_timeout` times its maxvelocity (2700 ticks at CORCOM's 1.07 px a tick, about 2890 px). The nearest, not the richest: past `comm_r` every patch is a walk from the base, and the shortest is the soonest income. A seat that lost its factories keeps its commander home. Without it Trout Farm's ARM start, whose nearest patch lies 1353 px from the anchor, idled its commander until `open_t` and starved the factory at 1.25 metal a pass until about GameTime 13 800.
- **The base ring grows from the anchor.** Its sites are taken nearest the anchor first, ties to the site farther from the military's estimate of the enemy base, then the lower cell; the commander takes its own nearest where it stands, because its reach already holds every site it takes within `comm_r` of the anchor and its walk is the opening's cost. The ring's own order (`place_ring`, the east side first) grew every base east, toward the enemy at the bar's second start: in the cmdr run the structures' mean projection toward the enemy at GameTime 15 000 was +577 px at the second start and −146 px at the first (medians over 60 and 46 seat-games), and 1 961 constructors died at the second start against 551. Ordered from the anchor, the commander walked back from its far extractors to the anchor's side of the box, and the economy game's bar came at 4 218, past the 4 100 the Fleas' order leaves; ordered from where it stands it comes at 4 055, as in the ring's own order. The key is a function of distances alone, so a start and its mirror build mirrored bases. **The ring grows only for a lack of room**: a ring whose sites were left for the commander's reach, for ground threat at the site or on the route, or for the commander's safe distance (below) is not exhausted, so a raid over the base for a few decisions neither pushes the next ring out nor passes a plant type over. **The commander's safe distance**: the commander takes no ring site within 600 px (`IMPAI_CMD_SAFE_PX`) of a seen armed enemy -- the military picture's enemies, armed by their threat row or never identified --, the distance at which the military weighs a contact and flees it; otherwise the economy would send it back to the site the flight took it from once the window lapses, a cycle every 300 ticks. More than 64 such enemies within `comm_r` + 600 px of the anchor refuse every commander ring site. **Gaps**: a ring longer than 256 sites loses its top side's tail before the order, as it did without one; a commander standing on the enemy's side of the base takes that side's sites first, so its buildings can lean toward the enemy, by at most `comm_r` from the anchor, ties going to the site farther from the estimate; near a contact the military would stand against (*The military*, the stand), the commander's ring sites still wait for it to leave; the commander's extractor patches are held to the threat map alone, not to this distance.
- **The commander helps at home.** With no building frame within `comm_r`, the commander helps the seat's unfinished mobile units there -- the factories' products on their yards -- with the help order (resolver mode 8, HELPBUILD on any unfinished target), never a guard: a guard's assist of a factory is the same help, but the guard's reactions pushed the attacks that took the commander past its reach. In the cmdr run the commander stood on Standby or with nothing in 21-22 % of its samples after GameTime 9 000 (the finish run 3-6 %) and helped or reclaimed in 3-5 % (33 %), while every seat-game had a factory within `comm_r`. **Not yet measured**: that the help adds the commander's build power to the frame; were the resolver to refuse it, the commander would be as idle as before and no worse. Nor whether the help blocks the yard: the help keeps the commander where it helped while the factory builds frame after frame, and a parked unit was seen in a plant's way (the census's Park, [INFERRED]). The bar's per-game columns hold no factory output for the windows the commander helps; were the blocking confirmed, the help would be refused while the commander stands inside a kept way out.
- **The assist in a metal stall.** In the hold (*The military*, the governor's paragraph) the assist helps only an extractor's, a geothermal's, an energy building's or a maker's frame: no constructor guards a factory, the commander helps no factory's product, and nobody helps a plant's frame, which its own builder finishes. A helper already on such work is given the ladder an idle builder gets, or stopped when the ladder has nothing for it.
- **A plant keeps a way out.** A factory's site is taken only when a bounded search proves, for each movement class of its ground products, that the class's largest product can leave it: from the places just beyond the yard cells of the footprint's edges, over places where the product's whole square fits the class's **content map** -- the class's pathing map as the level load builds it from the heights and the features (slope, water depth and blocking features, the class footprint folded in), with no unit: the engine's live map holds every player's units, seen or not, which no seat may read ([player](player.html), *No cheating*) -- and where the square overlaps neither the plant, nor a claim, nor a building the seat knows of: its own census's, and another player's its knowledge rows hold, over every footprint its last-known 32-px block allows. The search ends at a place 16 cells clear of the plant. Each class is searched on its own path: classes cross different ground (a hovercraft's map holds water a K-bot's does not), so the largest product's path proves nothing of another class's, and one path open to all classes at once is stricter than the units are. The paths, inflated by each class's square, are claimed for as long as the plant stands or is ordered, so no later building closes them. A plant whose yard, build menu or a product's footprint cannot be read whole is refused, never taken as one nothing leaves. A plant that traps what it builds was the measured cause of Core seats that built no army on Pincushion and Trout Farm: a feature below the exit, the constructors circling the yard for the rest of the game. A site the way out refuses is avoided by plants of its type for `fail_t`, in a ring of its own (32 entries), never the shared fail ring, so the refusals cannot push a patch's failure out of it. **The work is budgeted in cells, not searches**: a site's proof costs its box once and, for each product class, the content map's window and the box again, and a decision spends at most 98 304 cells, 0.59 ms at the worst 6.0 ns a cell measured with the DLL's compiler under Wine (`impai_build.c`, THE WORK BUDGET and THE COST) -- a retail plant's eight classes fit one decision. A proof that would pass what is left is deferred, not refused: the decision stops proving, and the next one, its budget whole, comes back to the site; the first proof of a decision always fits, so every decision proves or refuses one. A plant type refused 16 times in a row, or whose base ring is exhausted at its widest, is passed over by the factory choice for `fail_t` -- every unit type has its own entry, so no type is ever evicted from the record --, which takes the next type; when every type it would take is passed over, the plant goes to the first ring site the site test passes, with no way out, and the game's log names it (`impai: way waived seat= plant= cell=`): a seat with no factory loses more than one with a cramped exit, and a plateau or island start must not wait the game out. The rule, its bound and its termination are in `impai_build.c`, THE WAY OUT, THE WAY REFUSALS and THE PLANT'S FALLBACK; `tests/tasim_class_test.c` holds the content map to the core's fill, which `tools/tasim/classcheck.py` holds to the engine's, and under the check lever every level init compares it with the engine's own map before any unit stands (`impai: check pass`, judged by `impai_check.py`). **Gaps**: a building of another player the seat has never seen is not in the content map, so a product may meet it as a human's would; the content map is the level load's, which a re-mark lowers at the map's last column and row; a plant placed by the fallback has no proven way, and what it builds may circle its yard as on Pincushion; a product class whose proof alone costs more than a decision's work budget -- a mod's product of about 50 cells or more at the largest box -- is refused, and its plant falls back.
- **A site the builder could not reach is that builder's alone.** When a build ends with nothing built and the builder stands farther from the site than MobileBuild's own reach test allows (builddistance plus both footprints, [orders](../engine/orders.html), *MobileBuild's reach abandon*), the builder is the one held back, for `fail_t`, and no site is: the cause is where the builder stands, not the site, and failing the site for everyone emptied a whole map's patches one by one. A reclaim that ends with its feature still there or out of sight records the same, at the feature. While its record lasts the builder avoids the recorded spot and is given no site and no feature beyond its reach from where it stands, by the same compare, so it is not sent from one unreachable site to the next, each failing in turn; and it takes no factory, sea plant or tech-up, which go to a builder that can walk, so it cannot hold the seat's one plant want either. The record ends after `fail_t`; a builder still walled in then costs one more job a `fail_t`, not one a decision. Any other build that ends with nothing built fails its site for every builder.
- **Builders keep out of known ground threat.** Every builder's choice is refused when its site's 256-px cell is on the seat's ground threat map; for a constructor -- and for any builder's patch and feature -- also when **the route**, the straight segment from where it stands to the site, crosses such a cell (its own cell left out, so a builder standing in threat may still walk out). **The route** is one sampler, `impai_route_threat` (`impai_maps.c`, THE ROUTE), which the military's way out shares (*The military*, step 8): the straight segment sampled every 32 px in 64-bit integers, each 256-px cell it crosses past the start's read once on the ground threat map, a cell off the grid reading 0 and the point held to the coordinate limit, 0..32 767 px, first. The builders read it by its **CLEAR** rule: any cell above 0 refuses. The military reads its **OUT** rule. This holds on the base ring as on a walk: in that Comet Catcher game four of the 27 died at work in the base. **The walk check**, each decision: a constructor whose build has no frame yet or whose explore walk crosses known threat, or whose frame or assist target has the threat within reach (its cell or the builder's own), is turned back to the anchor, or to the nearest clear cell while the anchor's is threatened or its block is not reachable on the builder's class (by the cell's centre block), so a constructor is never sent onto sea or a cliff; an abandoned explore cell is failed for `fail_t`. The turned-back builder holds the move and no job, and the next decision lists it idle (**the hand-back**, *The military*, step 6), so a move whose point it never reaches -- the engine's ground move ends only on its arrival -- cannot hold it, nor the governor count a constructor the economy cannot task; the ladder's next site for it is threat-tested, route included. A ground defence's builder is no exception: one sent to a site at the threat's edge is turned back once the threat's reach covers its cell or the frame's. **The walk checks leave the factories their roots**: the orders before the factories' -- stops, turn backs, the ladder -- spend at most `roots` less one for each idle factory, up to `queue_max` -- in the hold up to the floor's `hold_fac` less the busy factories, the most its walk orders --, so a raid that turns back every builder still leaves each idle factory its order. **A radar dot is a threat too**: on the map the builders read, each contact the seat holds now and has never identified stamps the ground reach of the type the military counts it as -- the median identified enemy mobile type seen, else the seat's own median army type (*The military*, step 2) -- through one test the military's picture uses as well (`impai_row_unidentified_now`), the type the last military decision's (`IMPAI_MIL.radar_as`, at most `look_mil` old); a ghost is not held now and stamps nothing, and an identified row is never stamped twice. The commander keeps its own rules (`comm_r`, the military's safety and leash). **Gaps**: the route is a straight line, not the path the engine walks; a threat the seat has not seen is not on the map; the check runs once a decision, so a builder walks up to `decide` ticks into a threat that appears (`impai_brain.c`, THE WALK CHECK).
- **The repair.** Before the ladder, the economy sends a constructor to each army unit the military holds at its haven (*The military*, step 8) -- a unit in `IMPAI_JOB_MEND`, within 512 px of the haven (`IMPAI_MEND_NEAR_PX`) and below full health, in slot order: of the idle builders and then the helpers, the nearest that can repair, ties to the lower slot, with no known ground threat at the mender or on the route there and able to come near it, at most `repair_max` repairing at once, none in an energy stall (a repair costs energy) and none in **the hold** (*The military*, after the governor): a repair asks no metal, but the hold keeps the build power for the economy's buildings while the store is short, so a mend waits it out and a mender it outlasts is released after `mend_wait` with its mark; the pressure lifts the hold. The order is resolver mode 8, which answers `REPAIRUNIT` for a finished target its repair test `0x4899B0` accepts ([orders](../engine/orders.html), *What each resolver mode answers*). **Who can repair** is the engine's rule: the test needs the def bit the FBI loader copies from `canreclamate`, so every reclaimer can repair and nothing else can, and RepairUnit `0x405300` adds ⌊workertime / 30⌋ steps of at most 1 HP a tick ([weapons](../engine/weapons.html), the emitters' table), so a repairer gives at most 30 HP a second and none below a workertime of 30: a constructor with `canreclamate` and a workertime of 30 or more. Never a factory, which has no mover and never repairs through RepairUnit, and **never the commander**: a repair at its head names a unit, so the hand-back never lists it idle, and a commander repairing a mender the military then calls into a fight would follow it (RepairUnit follows a walking target) with nothing of the economy's to end the chase. **Before the ladder**, because an idle constructor is tasked at every build it finishes, so a repair after the ladder would almost never find one. RepairUnit ends by itself at full health, which is why the military releases a mender there. **The walk check** turns a repairer back, as any builder, when its mender's mend has ended, the mender stands past 512 px of the haven, its slot holds another unit or none, or known ground threat reaches the mender or the repairer. `repairs=` on the `eco` line counts the orders. **Gaps**: a repairer walks a straight line by THE ROUTE's test, not the engine's path; a mender the military releases unrepaired leaves its repairer to the walk check's next decision.
- **A claim is never dropped**: the claim table holds its bound -- one claim a census unit (an extractor's site or the build order at a builder's head), the kept ways' rectangles and one a command -- and the census walks at most the engine's units a player, so the table is never full; were it ever, the decision would answer every site claimed and place nothing, rather than place over a site or a way out it could not hold.
- **Reclaim when it pays.** A feature's reclaim pays its whole metal at the end of a countdown of about half a tick a metal, whoever reclaims it -- far faster than any extractor makes metal -- but only what the store has room for: the economy pass holds the store to its maximum and the rest is lost. So a feature is worth its metal held to the store's room, less what the seat's reclaims under way will pay, and a full store reclaims nothing. The ladder asks for a feature twice, never before the plants or the first `open_mex` extractors: **before an extractor**, once those are up, when the feature pays the builder more metal a tick than an extractor makes (the seat's steady income a pass over its extractors, over the pass's 30 ticks); and **before the buildings that spend** -- energy, makers, exploring, the assist -- while the seat is **short of metal** for what it wants next: stalling on it, or making less a pass than its next factory's `factory_per_m` times the factories it has, or with all its factories the tech-up's `tech_m`. A helper at its assist reclaims too while the seat is short. A wreck is a feature like any other. On a metal-poor start the opening still goes to the first extractors, however far, and then to the rocks and wrecks near the base while the store has room for them.
- **The steady income.** Every income gate -- the second factory's and the sea plant's `factory_per_m`, the tech-up's `tech_m`, the stall, the projection `pm_in`, short of metal, and the reclaim's extractor rate -- reads the seat's metal a pass less the one-off metal the last pass settled: each unit's metal produced in the pass's period (its ledger's `+0xE4`) above its own make rate, metalmake plus makesmetal, for every unit but an extractor. The economy pass writes a unit's produced metal only with an extractor's extraction, a maker's output and a finished unit's metalmake; between passes the reclaim completion, a factory's refund for a product torn down unfinished, the unbuild and a reclaimed unit's credit add to it ([weapons](../engine/weapons.html), *The ledger*), and the pass sums it all into the player's income `+0x9C`. A 75-metal rock or a cancelled product carried a plant want for one pass: 17 of 65 plant wants on the mods1 run rode on one, 13 of TA Twilight's 14, each below its gate without it, and the plants they ordered starved at the commander's 1 a pass (MEASURED 2026-11-14). The steady income lies in [0, the income]. Its residuals: a payout to a unit that died since the pass counts once as income, and so does an extractor's own reclaim; an extractsmetal structure with no yard map, which the WORLDQ does not class an extractor, has its extraction subtracted; and the energy twin -- tree payouts in the energy income, read by the energy stall and `pe_in` -- is not subtracted.
- **Features are known by sight, and remembered as beliefs.** The seat reads a feature only where it sees now, and remembers what it saw (at most 128, with when each was last seen); anything may reclaim, burn or crush a feature out of sight. A feature in sight is a fact; a remembered one is scored with a confidence of `1800 / (1800 + age)`, a half one game minute after it was last seen. The choice is the most metal times confidence a tick by the reclaim sweep's own model -- the walk past the builder's builddistance plus the reclaim countdown, which has no worker-time term -- among those within 768 px of the builder, unclaimed, not its own failure, under no ground threat, and for the commander within `comm_r` of the anchor. A feature in sight is reclaimed; a remembered one is walked to, and reclaimed when sight shows it there. Seen gone -- or playing the sequence a paid reclaim or a death begins, which no reclaim can take -- it is dropped from the memory for every builder, and its builder moves on in the same decision; a builder another hand gave an order ends its reclaim with no failure; a builder that gave up with its feature out of sight or still there avoids it alone for `fail_t`, as an unreachable site. A burning tree is left to that rule and the decay: the seat's view of a feature carries no burn state. On a metal-poor start the map's rocks and wrecks are the metal nearest at hand.

## Adaptation

- **The enemy's composition by role**, from what has been seen. Radar dots count as unknown ground.
- **Response tables** turn that composition into role demand, for example more anti-air when enemy air passes a share of the seen value. The tables are data, tuned by the tasim harness (A3), one table for every mod.
- **Scouting** is a task the military manager staffs on a schedule, and again whenever the enemy picture goes stale.
- **Openings** are picked per game from the map and scouting. There is no memory between games.

## The fight predictor

- Two groups in, with the gap between their fronts; both sides' remaining value and the margin out.
- A stepped fight of volleys over the engine's own numbers for every type and weapon, read at the level init:
  each type closes at its own speed to its first armed slot's reach less a stand-off, as the engine's chase
  does, each weapon fires its first volley at once
  and then by its reload, a hit deals at most one unit's hp and an area hit reaches more units, and a hit's
  probability is fitted by what guides the shot (straight, ballistic, homing, dropped) and how far the target
  moves while it flies. 5400 ticks at most, the references' limit. The form and its engine facts:
  [impai](../modules/impai.html), *The fight predictor*.
- It decides engage, hold or retreat for every squad, which targets are worth attacking, and when an attack wave is big enough.
- Calibrated against the engine: it must agree with the engine's majority on at least 90 % of decisive fights,
  and on 90 % of each kind's when the kind has at least 20 ([calibration](calibration.html)).
- **Not yet fitted**: `impai_fitparams.c` holds the starting coefficients until the references are complete.

## The values

`impai_tinfo_values` (`impai_tinfo.c`, THE VALUES), once at the level init, content like the fight tables
([ImpAI](../modules/impai.html), *The values*): for each type of a land fighting class (raider, assault,
artillery; hover and amphibious types are land classes) with a value above 0, and each tech level T = 1..4,

- **the field** F_T: the mobile types of those classes with a value above 0 and classifier tech 1..T, sorted by
  value then type, and past 32 of them the value stride F_T[floor(k n / 32)], k = 0..31;
- **a pair** m(t, e): the predictor's margin, t's side's remaining share of its value less e's, in [−1, 1], each
  side n = min(65535, round(V / value)) units, V = 32 × the larger value -- equal value, at least 32 units a
  side --, both closing at full health from the gap max(t's reach on e, e's on t, 1), with no gravity;
- **the value** q_T(t): the mean of m(t, e) over F_T; 0 for an empty field, a refused run counting 0.

A field equal to the one before is copied, not re-run. Retail: 47 types, fields of 12, 32, 32 and 32, 3 572 runs
(MEASURED 2026-11-14, the C build over the retail fight content offline). The field is every side's and the seat's
table, picture and reach do not enter it, so the values are the same for every seat and every tournament table.
**Why a mean of pair fights**: one mixed fight against the whole field moved with its scale and gap (CORLAB −0.31
to +0.44 across V 3 000..12 000 and gap 300..1 000), the pair mean by at most 0.03 at a gap past both ranges, and
the pair's own reach as the gap removes the rest; the pair rates' square law agreed with the predictor's sign on
87 % of the land pairs and was wrong exactly on fast units, whose misses it prices where a closed fight has none.
**Gaps**: the enemy is assumed at the seat's own tech; artillery's siege purpose, the D-gun, transports and
anti-air are not valued, and an air plant is never scored for its army; a plant's own cost does not enter its score.
**Calibration**: the values rest on the predictor where no reference fight checks it -- CORAH, ARMAH, CORSNAP,
ARMANAC, CORMH, CORAK, CORGATOR, ARMSPID, ARMFIDO, ARMFAST and CORREAP have none, and the homing coefficients are
held until a homing-at-a-mover set exists ([calibration](calibration.html)). Offline on Total Mayhem's content
(MEASURED 2026-11-14, the C build) four jammers (ARMJAM, ARMASER, CORSPEC, CORETER) score +1.000 at tech 1 through one long-range vertical-launch
weapon (range 1 120 px, 720 damage a shot every 270 ticks): unchecked by any reference, and the reason Mayhem's
advanced plants score +1.

## What it has to beat

What the stock AI does, from the engine map ([orders](../engine/orders.html), *The stock AI's helpers*; DISASSEMBLED, not measured):

- **It builds by a weighted random pick.** Each type on a builder's list gets a score from storage, income and per-type ratings scaled by the profile's weights, and one is drawn in proportion to its score. It builds nothing while stored energy is below 50 or metal below 25.
- **It builds on unseen ground.** Sites are random points on a per-seat lattice around its base, tested with no sight or explored-map check; its metal spots are every metal cell on the map.
- **It attacks with six or more armed units, every 300 ticks, at the enemy unit nearest its force**, chosen from every enemy on the map: its target picker has no sight, radar or explored-map test. Three or fewer regroup at the base.
- **Its commander stops building at five builders** (the seat's complete units with a build list, factories included) and roams the base instead; a hit commander drops its orders and pauses for 30 to 329 ticks.
- **Its weights and limits change with the Difficulty** through the profile's `plan` sections, and on Easy and Medium the engine cuts its resources to 50 % and 70 %.

## The military

As built (`impai_mil.c`; its engine glue: [ImpAI in the engine](../modules/impai.html), *The military*). One
decision a seat every `look_mil` ticks of its persona, on a tick the economy never takes:

1. **The army**: the seat's finished mobile units with an army class (A3's thirteen, by layer and role), at most
   512; each in one squad, its job `IMPAI_JOB_SQUAD` naming it. A unit with none joins its layer's FORM squad.
   Each marks the **sweep cell** it stands in -- the map in 16 × 16 cells, `IMPAI_MIL.swept` the GameTime / 64
   an army unit of the seat last stood in each, 0 never, 16 bits for 38 game hours.
   **The squad table cannot fill**: every task change goes through one transition (`become`), which keeps at most
   one FORM, one DEFEND and one RETREAT squad a layer (a squad given a task its layer already has joins that
   squad), at most 4 ATTACK squads (a wave past them reinforces its layer's ATTACK squad whose point is nearest its
   goal; with none of its layer, the four stand on at most two layers, so one holds two, and the two whose points
   are nearest each other merge to make room -- four land waves cannot keep a sea or air wave home), at most 2 RAID
   squads, and one SCOUT:
   3 × 3 + 4 + 2 + 1 = 16 squads, so a layer with no FORM squad always finds a free one and no army unit is left
   without a squad (`left_squad=` on the `mil` line, 0 by construction). A normalisation at each decision's start
   makes the counts a fact whatever a recorded table holds: it frees a squad whose task or layer is past its enum,
   merges duplicates into the lowest-indexed and sends ATTACK, RAID and SCOUT squads past their bound to FORM.
2. **The picture**, from the seat's own column only: enemy rows seen now (own sight, an ally's, the radar pass),
   and structures also as ghosts; a radar contact never identified counts as the median identified enemy mobile
   type. It fills A3's twelve features (`impai_picture_features`) and the enemy's mobile composition the type
   choice weighs, and the enemy base's estimate: the value-weighted centre of the enemy structures known, else
   the start position nearest the seat's own that is not it (the starts are public). **The commander's memory**
   (`IMPAI_MIL.foe`, one a seat): a foe's commander in the seat's own line of sight (`IMPAI_LOS`), its slot, block
   and GameTime -- never an ally's sharing (`IMPAI_SHARED`), a position only, which may describe a unit that has
   left the slot since; nothing else writes it, and the sweep forgets it (below), never a read of the other seat.
   **Contacts**: the enemies seen now, joined across 256-px cells within 768 px, seeded nearest the anchor first,
   at most 32.
3. **Defence**: a contact threatens the base only through a member that is **mobile and armed** (any dps in the
   threat table, or a radar contact never identified) and stands **nearer the anchor than its own side's base**
   -- than every known structure of its owner (of every foe for a contact never identified), else than every
   start position but the seat's own -- within `base_r` + 512 of the anchor or 512 of an own structure that itself
   stands within `base_r` of it (the anchor box, the bound). Its own owner's and not one estimate of every enemy's:
   with enemies on opposite sides the value-weighted centre of their structures falls near the seat's own base. After `react_confirm` decisions of it each layer's FORM squad becomes DEFEND, sent to that threatening
   member nearest the anchor, of the nearest threat **the squad can hurt** -- the fight content's pair rates of its
   types on the threat's types summed above 0, over at most 8 types a side, so a ground army is not pinned by
   aircraft it cannot shoot; a DEFEND squad with no such threat left goes back to FORM. Why the half-map bound:
   `base_r` + 512 is 2 048 px, which reaches into the enemy base on a small map (the enemy commander stood
   2 674..4 767 px from ImpAI's anchor in the 16 Pincushion cap games of b7 r1, 2 629..3 380 px in the five Lava
   Mania ones), and the economy's outlying extractors stand beside the enemy's. **Gaps**: an outlying structure
   raided by armed enemies is no base threat, and neither is an armed enemy nearer one of its own side's
   structures than the anchor -- one the enemy built inside the anchor box among them; only a squad already within
   reach of the contact answers either.
4. **Tasks**, each squad round-robin. An ATTACK squad's **goal** is chosen again first, before and whatever the
   contact test finds, the first of these the squad reaches: an enemy commander the seat sees now, the nearest
   (its slot); a remembered one last seen within `IMPAI_FOE_FRESH` (5 400) ticks in a sweep cell no army unit of
   the seat has stood in since; the enemy base's most valuable known structure within 1 024 px of the estimate;
   the **sweep**, the cell whose centre members holding at least half the squad's value reach by label that an army
   unit of the seat stood in least recently, ties to the nearest the estimate (so with nothing known it starts at
   the enemy's start position and spreads out from it), then to the lowest index -- the squad's reach test holds
   there by construction, so a mixed squad goes where its army goes, not where a hovercraft listed first goes. **The commander comes first**: it is the win condition of a
   commander-death game. A RAID ends first, before the contact test, at `task_timeout`, when its target is no
   longer known, or on arrival. Then a contact within the squad's longest range + 384 is predicted (the fight
   predictor, at most `predict_max` a decision) -- below `retreat_margin` the squad goes back
   (RETREAT) to the rally point, `rally_px` from the anchor towards the estimate; otherwise it fights. With no
   contact a FORM squad worth `wave_value` + waves sent median army units attacks the goal above when the
   prediction against its defences passes `attack_margin`; a RAID of three to six of a FORM squad's fastest units
   (scouts aside) goes for the known enemy extractor or energy building of least ground threat, up to two at
   once; one SCOUT walks the start positions it reaches in turn and then the sweep's cells (ties to the nearest
   the scout), three legs, every `scout_period` or while the estimate rests on no structure. **An ATTACK has no
   timeout**: it ends by retreat, or when it has **arrived with nothing to attack** -- members holding half its
   value within 256 px of the points travel gives them (a spread point, the point, or the substitute where the
   class stops short of it), and no member attacking -- and no goal it has not arrived at: its point's sweep cell is
   stamped as its own members', the goal is chosen again, and with none, or the same place (a commander it sees
   and cannot attack, a structure it cannot hit), it goes back to FORM; and no wave forms at a point the squad has
   already arrived at. **A RETREAT ends when every member has arrived** at the rally point, each within 512 px of
   the point travel gives it, so a squad that joined a retreat still fighting does not end it by the centroid.
   **Every other task has a lifetime**: a RAID,
   a RETREAT and a SCOUT, and a DEFEND with no contact in its reach, go back to FORM once `task_timeout` -- the
   plan's time *at* the point -- plus the travel has passed since the task began: twice the rally point's
   straight line to the task's point at the squad's slowest member's maxvelocity, at most 16 200 ticks. So no
   point the squad cannot reach -- a sea retreat to a rally point on land, a scout leg across water, a threat out
   of reach -- holds it longer than `task_timeout` + 16 200, and a slow wave is not recalled before it arrives
   (CORLEVLR's 0.8 px a tick takes 3 641 ticks over Pin Cushion's 2 913 px, past Hard's 2 700). **A refused
   prediction changes no task.**
5. **Orders**, at most `mil_roots` a decision. **A squad never stands while it has nothing to fight**: a fighting
   squad with no member attacking -- given now or already at its head -- travels to its point as with no contact,
   so a contact of nothing it can attack (a submarine for a land squad, a target across water) cannot hold a wave.
   A fighting squad attacks only units the seat sees now, and only those its own observation of the same turn
   still finds as the unit its row is about -- a row is as fresh as the slice's last visit of its slot, up to 8
   ticks at the raised limit, so the slot may hold another unit by then: a row seen in sight must name the type and
   owner the observation shows, any other the incarnation the seat's radar recorded -- an enemy commander
   among them, else the one its members kill quickest (type health over the summed pair rates), else each member
   its nearest. A member attacks only a unit the order resolver will answer it for -- its tests of the unit's
   canattack, weapon, weapon 1's toairweapon and dropped, its waterweapons, canhover and canfly, and of the
   target's airborne and under-the-sea state ([orders](../engine/orders.html), *What each resolver mode
   answers*) -- read from the type's facts at the content build, the unit's flag word and the target's state
   that turn, where the seat sees it; a target known by radar alone shows no such state, so there the resolver
   decides and a refusal costs no order. While some member attacks, one with nothing it can attack, an anti-air
   unit in a ground fight, keeps with the squad: it moves to the centroid when it stands farther than 64 px from
   it, and is given nothing nearer. The attacks and the steps back are ordered before those moves, so the
   decision's orders go to the fight first. A member under `micro_hp` of its health steps 400 px back, once in `micro_period`, and is left on its
   way between steps. A travelling squad moves to a 5 × 5 grid of points 64 px apart; a move toward a unit passes
   no target (a move with an enemy target is a reclaim or a capture, [orders](../engine/orders.html)). No order
   repeats what the unit's head already says.
6. **The commander's safety**, before the squads' orders, by the seat's own knowledge alone; three steps, and
   the first that moves the commander ends it:
   - **The blast.** A foe's commander the hunt's memory (`IMPAI_MIL.foe`, the seat's own sight) holds as seen within the last
     `look_mil`, its row still a commander type, is fled while nearer than its type's `blast_r` + 64 px + the
     two commanders' approach over the horizon `look_mil` + `react` -- the next decision and its orders' delay
     (ours at its speed, the foe's over the horizon and the time since it was seen). ARMCOM's and CORCOM's blast is 475 px (COMMANDER_BLAST, areaofeffect 950): in b7 r1 fourteen
     games ended with both commanders dying in one blast, 17 to 484 px apart.
   - **The contact.** The contact of most value with an armed or never-identified member within 600 px is
     predicted against the army within 512 px **without the commander**, whose D-gun the fight content counts as
     firing on its own; the commander stays where that army wins alone (a margin of 0 or more). The old gate,
     a contact worth more than the commander (30 423 for ARMCOM), was never met by an army contact: no safety move
     in 162 of b7 r1's 222 seat-games. **The stand**: where the army alone does not win, the commander still
     stays when the same prediction with itself counted wins by 0.5 or more (`COM_STAY_MARGIN`), and only within
     `comm_r` of the anchor; a close fight or a stronger contact it flees. Counted with itself, its D-gun counts
     only while the D-gun's own rules could fire it at the contact this decision -- outside the window, stored
     energy for the shot and `dgun_reserve`, and a member seen now, identified (never a radar contact), worth the
     shot and no commander --; otherwise the commander's entry fights without its CommandFire weapons
     (`IMPAI_FE_NO_COMMANDFIRE`). A counted D-gun still fires at its reload in the prediction, where the seat
     orders one target a decision and the guard can refuse it; the margin bounds that overrating. In
     the cmdr run 249 of 461 flights began with 3 or fewer armed enemies within 800 px, and in one game the
     commander fled 1-8 raiders round its base for 11 000 ticks while 78 constructor frames died on its
     factory's yard.
   - **The leash.** A commander farther from the anchor than its reach + 512 px -- `comm_r` once the seat has had
     a factory, `FAR_PATCH_K` (2) × `comm_r` before, the opening's far patch -- walks to the anchor.

   **A flight stays in the base**: to the anchor or the own structure within `comm_r` of it that lies farthest
   from the threat, ties to the anchor (with the farthest structure anywhere, 94 of b7 r1's 105 moves went past
   `comm_r`, at most 6 553 px). Each move makes the commander the military's for 300 ticks, a window the
   economy's census honours, and every order the military gives the commander -- a flight, the leash, a D-gun --
   ends the economy job it held with no failure, so the site it was walking to is not failed for the seat.
   **The hand-back**: once the window lapses, a commander with no job whose head is its one order, names no unit
   and is no build is the economy's, listed idle whatever that head is; every constructor is handed back by the
   same rule, a turned-back one included. A flight's point is a structure's centre, which the
   commander never reaches, and the engine's ground move ends only on its arrival event, so the move held the
   head and the economy read it as busy: in the cmdr run the commander stood 39-51 px short of such a point with
   the move at its head in 34-35 % of its samples after GameTime 9 000, up to 67 200 ticks, 91 % of them with no
   armed enemy within 800 px. A head that names a unit -- the D-gun's attack -- stays the military's until the
   engine ends it at the target's death.
7. **The D-gun**, then, when the commander is not fleeing: the seat's weapon scan leaves CommandFire weapons to
   orders, so the commander D-guns (resolver mode 4, `ATTACKSPECIAL`) the most valuable enemy the seat sees now,
   and still finds that turn, within its D-gun's range, only while stored energy covers the shot plus `dgun_reserve` of its energy storage,
   only a target worth the shot's energy in metal, never a commander (its blast outranges the D-gun) and never
   one past `comm_r` of the anchor (an ordered attack chases). **The guard** refuses a target when a friendly unit
   -- the seat's own, frames and structures included, read where they stand by a walk of its slot block, or an
   ally's, from the seat's rows, padded by the 32-px block and 8 ticks of its speed -- stands within the shot's
   radius plus its footprint and 32 px of the line from the commander toward the target, out to the weapon's
   whole range, or within the target's death blast; and when the target's blast reaches the commander itself.
   The most valuable target the guard passes is taken. **Gaps**: a unit that walks into the line after the order,
   and the order's later shots at the same target, are not tested again; whether the shot ends at its range is
   inferred, not disassembled; more than 256 friendly units near the commander mean no shot. A type's D-gun is its longest CommandFire weapon when its
   def can take the order (`candgun`), from the content build: `impai_tinfo_attack` reads it, and `canattack`,
   from the role input, whose `+0x245` mask (`tasim_roles.h`, `TASIM_ROLE_F245`) both role fillers copy and the
   function asserts at compile time. The conditions and the guard are tested when the order is given, not at each shot: an order already given holds its target until it ends, and the engine fires each
   shot only while the seat holds the shot's energy (AutoAim, `0x49E3F3..0x49E40C`, [keys](../engine/keys.html)),
   so the reserve is not held between two shots at one target.

8. **The mend** (`impai_mil.c`, THE MEND), at each decision after the base's threats (step 3) and before the
   defence, the tasks and the orders: an army unit of a land or sea class below `mend_hp` of its health -- the
   owner's rule, an army unit below half its health -- leaves its squad for its **haven**, to be repaired there by
   the economy's constructors (*Task allocation*, the repair). The haven is the rally point spread by the unit's
   slot over the travel grid (5 × 5 points 64 px apart), held to the map, substituted onto its own component and
   taken at its 32-px block's centre, which its job records: one fixed point a repairer comes to, never the unit
   to a moving builder -- RepairUnit walks to its target and repairs it only standing. The job is
   `IMPAI_JOB_MEND_GO` until the unit stands within 256 px of the haven, then `IMPAI_JOB_MEND`; no squad counts a
   mender and `left_squad` stays 0. Aircraft never mend (they repair at pads by the engine's own rule), nor
   constructors, which are not army. The idea of retreating to a repair point at a share of health is BARb's (its
   builder retreat, `"builder": [0.85, 1.0]`, `hard/behaviour.json:47`); the rule is ours, and no code of BARb's
   (GPL) is used.
   - **The last defender stays.** While a base threat stands, the threat's types are its members' (a radar
     contact never identified as the substitute), at most 8; a unit within `base_r` + 512 px of the anchor
     defends when its type's pair rates on those types sum above 0, and every unit there defends when the types
     overflow 8 or the fight content has no type -- conservative. A damaged defender is the last while no healthy
     defender in a squad stands beside it (`mend_last=`); one already mending is called back with no mark when
     the last healthy defender goes (`mend_called=`). The commander and the towers are not counted as defenders.
   - **The cap: no cascade into flight.** With V the squad's value present and M the value mending from it, a unit
     leaves a squad of two or more only when M + its value ≤ 0.5 × (V + M), a squad's lone member only when
     nothing mends from it (`mend_cap=`): a squad keeps at least half its value at every entry, and of two
     damaged members of a two-member squad one leaves.
   - **The way out**, both on the haven (`mend_way=`): no enemy seen now that has the unit within its ground reach
     plus 384 px lies on the haven's side of the unit; and THE ROUTE's **OUT** rule on the military's ground threat
     map -- from the unit's own cell the threat never rises from cell to cell and the haven's cell reads 0. Not
     the builders' CLEAR, which would refuse almost every damaged unit: it stands in the threat that damaged it.
   - **The order of the entries**: the 2 × `mend_roots` most damaged candidates (health share, ties the lower
     slot), each tested against the cap again, the haven and the way, at most `mend_roots` entering a decision,
     each with its one move or already standing at the haven. A candidate has no **mark** against it: a unit
     released unrepaired keeps its hit points then in its job through the join and is a candidate again only below
     them, so it fights with its squad until hurt further.
   - **The release**, the first that holds, the unit standing where it is and joining FORM at the next decision:
     full health (`mend_full=`; RepairUnit ends there by itself, so no repairer is left chasing); the haven off
     the map, recorded data (`mend_back=`); the last defender (`mend_called=`); then, a `MEND_GO` within 256 px
     arrives and a `MEND` whose health rose takes the time anew; the **travel bound**, a `MEND_GO` past twice its
     straight walk at its own speed, at most 16 200 ticks (`mend_back=`); **no repair**, a `MEND` whose health has
     not risen for `mend_wait` ticks (`mend_back=`). In retail content health rises only by repair (no army type
     has a `healtime`), so the stay is finite by construction; a mod's type with one autoheals, which counts as
     progress. A mender whose move is not at its head is sent on again.
   - **The budget**: entries and re-issues together at most `mend_roots` of the decision's `mil_roots`, and the
     decision refuses a plan past `mend_roots` + 2 ≤ `mil_roots`, so the commander keeps its two orders. Under a
     persona's bucket each mender's order is one military action, given only while more than two are left, so the
     commander keeps its two actions as well and a low bucket delays the mend.
   - **Gaps**: the way out is a straight line, not the engine's path; the military's map stamps no radar contact
     never identified, so one counts only through the first test, when it has the unit in reach; an enemy faster
     than the unit may follow it home; members dying after an entry can take a squad's mending share past half;
     candidates whose way stays shut can fill the 2 × `mend_roots` for many decisions and keep a less damaged
     unit waiting.

**The governor** gives each idle factory one order a decision, in slot order (`impai_econ.c`, THE GOVERNOR): a
constructor when three rules hold, otherwise one army unit. **The opening**: the first `cons_open` constructors need
nothing. **The army's share**: past them, a constructor needs army ≥ `army_per_cons` × (constructors − `cons_open`
+ 1). **The pressure**: no constructor while the military's stamp lasts (below). A constructor order the factory
refuses leaves it for the army in the same decision. **No rule ties the constructors to the income**: they build
the extractors that pay for them, so a cap proportional to income holds a seat to the income it already makes.
MEASURED 2026-11-14 over 57 ImpAI seat-games of the bar (GameTime 9 000..27 000): such a cap (24 workertime a metal
a pass) was the only rule refusing a constructor in 50 % of the samples past 9 000, and one of two in 8.5 %; the
seats held 2 / 3 / 4 constructors at 15 000 / 21 000 / 27 000 against 6 / 9 / 11 before the governor, and
6 / 8 / 10.5 extractors against 8 / 12 / 14; the seats stalled in 44 % of the window against 21 % ([decisions](decisions.html), G3).
**The hold** (`impai_brain.c`, economy, THE HOLD): while the stored metal is below `hold_pm` (200) thousandths of
the storage, outside a surplus and outside the pressure, the seat holds every spend that is not economy. At most
`hold_fac` (1) factories are busy: the walk gives no order to an idle factory once the busy ones and its own orders
reach the floor (`held=` on the `eco` line), in its usual order, and the floor's factory takes whatever the governor
gives it, a constructor included -- the hold refuses no constructor, which would recreate the loop above. No plant
past the first is started (the second factory, the sea plant and the tech-up wait); a plant already begun is
finished by its builder. The assist helps only an extractor's, a geothermal's, an energy building's or a maker's
frame, guards no factory and helps no product; a helper on anything else is given the ladder an idle builder gets,
or stopped when the ladder has nothing for it (`released=`), and its job ends only with the order that replaced its
head. Each is an economy order, through the funnel and the bucket; under a persona's bucket the idle builders
leave the decision's last command to a helper the hold must release, so every decision with a command to give
releases one and no persona leaves a helper on a factory's work for good. The pressure lifts the hold: the army is what a pressed seat needs. The test is the store alone, not the
stall (`m_stall` also needs income below demand, which turns false the pass after the hold cuts the demand, so the
factories would be released into an empty store and the stall come straight back); `hold_pm` is the buffer the
released factories start on. **It has no hysteresis, because it does not flap**: MEASURED 2026-11-14 over 62
ImpAI seat-games of the bar with the hold (GDI, 44 recordings), the store fell below 200 thousandths a median of 3
times a seat-game (16 at most) and stayed below for a median 27 passes (16 % of the runs 3 passes or fewer), and
`released=` summed 169 over all of them, 35 at most in one. A store or storage read from the record is whole metal held to 0..10⁹, a storage it
cannot read never holds, and the hold is never set beside a surplus. WHY: the economy pass pays every request of a
seat at one fraction per resource (`0x401A4D..0x401AB1`, [weapons](../engine/weapons.html), *The settle*), so only
what is asked decides what is paid; MEASURED 2026-11-14 over the same 57 seat-games, a stalled seat was paid 44 %
of what it asked, its factories asked 43 % of it, plants and help on factories' products 41 %, and the economy's
own buildings 15 %. The counts are finished units plus the
factories' count nodes, each held to 255 -- a count node holds the unit in progress, so a frame is counted once.
The army unit's class comes from A3's table (`impai_table_shares` over the picture, `impai_buy_next` against the
army's value by class, built, building and queued) and its type from `impai_class_type` with the predictor's pair
rate; in surplus a factory whose table buy finds nothing buys its cheapest army type, and one with no army type at
all is counted (`fidle=` on the `eco` line). **The army orders** (`impai_econ.c`, THE ARMY ORDERS) hold each
factory to the classes its units are good for here, at the plant choice's tech level and reach: a land fighting
class goes to a factory whose best reaching unit of it scores at least min(0, the seat's best of that class over
its factory types), so a class whose best unit holds its value against the field is built on any factory that has
one, one below 0 only by its best maker, and a class one plant alone offers stays that plant's; other classes are
not gated. A factory allowed none of the land fighting classes it offers is **poor**: it takes the governor's
constructor when one is allowed, and no army order outside a surplus (`poor=` on the `eco` line). The idle factories
that offer a land fighting class are walked in ascending order of their best class value, then those that offer
none -- a shipyard, an air plant --, ties in slot order, so a constructor goes to the land plant whose army is
worth least; a plant the values do not score takes a constructor only when no scored plant is idle, or the land
plant's army order meeting the army's share would pass the constructor to the yard behind it (the brain test's
yard took 6 of 7 so). A naval seat whose land plants are all busy still makes sea constructors. The table's shares are taken over the classes the plants that are not poor may build, and the
surplus step's cheapest type over those classes. **Before contact** (an empty picture) a land fighting class's
type is its best value among the factory's reaching types rather than its cheapest, or a plant chosen for its best
unit would build the class's cheapest; with a picture the type is `impai_class_type`'s, which reads neither the
values nor the reach (**gap**: after contact an eligible factory may build a type of the class that does not
reach). **A class no type of the
seat reaches in** is read without the reach, as on a linked map, so the land plants still build it by value for
the base's defence ([decisions](decisions.html)); per class, because one amphibious type reaching on Trout Farm's
advanced plant left the two vehicle plants poor for 726 factory-decisions (MEASURED 2026-11-14, r3
`bar-eota-troutfarm-cc-1`). **A poor plant beside a store in surplus** buys its cheapest army type of any class,
so no factory idles beside a full store. With no values there is no gate. BARb's build-power cap and its army/builder split are the ideas, read
for design only. MEASURED before it, the constructor quota (12 at most, four plus one per three metal, taking
every idle factory first): the first army unit at a median GameTime of 13 500-15 000 against a first factory at
3 300-3 600, five or six constructors and no army at 12 000 at every level (196 seat-games, 2026-11-13); in a
game against a human, 95 % of the factory-ticks went to constructors.
**The pressure** (`impai_mil.c`, THE PRESSURE), stamped by the military for the economy: the army-class enemies
seen now -- and the radar contacts never identified -- that are mobile and armed and stand at the base by the
defence rule, or within two 256-px cells of any own structure at any distance and nearer it than their own side's
base. When the seat's whole army is worth less than `press_k` × their value, the stamp holds for `press_hold` ticks
with that value and the pressing member nearest the anchor, and whether it is at the base (`press=` on the `mil`
line, `pressed=` on the `eco` line). Enemy commanders and builders never press: a commander walking among the
expansions would hold the constructors all game. A radar contact presses only while the substitute it counts as
has a ground reach -- the reach the builders' map stamps for it --, so with an air substitute, or none, the radar
holds no constructor and builds no ground defence; **gap**: a radar contact carries no type, so one aircraft on the
radar presses while the enemy's median seen mobile type has a ground reach. The economy reads a stamp past now + `press_hold` as none, so a
recorded value holds nothing longer than that.
**The plant choice** (`impai_econ.c`, THE CHOICE) runs once a decision, the first time a builder reaches the
factory rule or the governor runs, for the seat and not for that builder: an ARMCV's own menu shows only ARMAVP
and ARMHP. **The candidates** are the distinct factory types on the menus of the seat's finished builder types (at
most `IMPAI_SEA_BUILDERS`, 8, builder types), then on the menus of **the unlocks** -- the constructor types the
seat's factories make that it has none of finished, at most 8 -- at most `IMPAI_PLANT_CANDS`, 16, candidates, each
one whose own menu holds a
constructor that builds an extractor, a water-only plant only on a wet anchor, and a plant the way-out record passes
over only in the waived pass (*A plant keeps a way out*). **The build tech** filters them: a plant's build tech
(`IMPAI_TINFO.ctech`) is the highest tech its constructors build, and a tech-up takes only a plant above every
factory the seat has, frames included; a first or further plant only one at or below that, or at the least among
the candidates, so the income gate stays the only way up and a mod whose cheapest plant brings tech 2 still has a
first plant. Not the plant's units' tech: the classifier's tech is a graph distance, so CORHP's units are tech 2
while its CORCH builds nothing above tech 1 -- the least value with that offer took CORHP as CORE's tech-up in 167
of 170 seats and ended the tech-up there for good (MEASURED 2026-11-14 over 377 bar seat-games). **The score** of
a plant is its best unit's: the highest value (*The values*) among its types of a land fighting class -- raider,
assault, artillery -- with a value above 0 that **reach** a plausible enemy base, at the seat's build tech held to
1..4 (the enemy assumed at the seat's own tech); none when there is none. **The reach** (THE REACH): a movement
class's component at the base is the label most blocks within 8 rings (256 px) of the anchor's block carry -- a
larger class's block can be impassable right at the commander's start, or a pocket beside it, while the yards
round it stand on other ground, and the anchor's 0 read as no evidence made CORREAP "reach" on Trout Farm where
CORGATOR did not --, none there meaning the class does not reach. The targets are the military's estimate once it rests on enemy structures
seen or remembered (`IMPAI_MIL.est_seen`), and before that every start position but the seat's own: the estimate
is then only the nearest other start, which on Trout Farm is cut off by water while the real enemy was
land-linked. A class reaches when one target has a block of its base component within 8 rings: any, not all,
because before the enemy is known the map needs a class's extra mobility only when no plausible enemy is reached
otherwise ([decisions](decisions.html)). A VTOL always reaches; with no labels for a class, no military, no anchor
or no target every class reaches. A hover's mobility therefore adds nothing where the land classes reach and is
what makes its plant score where they do not; a route that is only much shorter over water does not count.
**Gaps**: until a structure is seen, a seat whose real enemy sits on the one start cut off from it builds land
plants; an ally's start is a target too, since the brain has no read of which start a player holds or of the
teams. **The best** is the highest score, then a plant the seat's own builders build before one that waits for
an unlock, then the least value, then the lower type, so with no values the choice is the least value among the
plants the seat can build now. **The opening** (no factory counted yet) is the least value among the candidates, unscored: its first
purpose is the constructors, and A1's economy bench passed on it ([decisions](decisions.html)). **A wet anchor**
takes the least value too: the score ranks land armies, and a ring round an anchor below the sea may hold no site
for a land plant. A builder whose menu holds the chosen type builds it; another passes the rule, so the plant
waits for a builder of a type that offers it. **An unlock**: when the chosen plant is one only an unlock builds, the
governor's next constructor is that type, at a factory that makes it and at no other factory, once a decision and
until one is a frame (`impai: unlock seat= gametime= cons= plant=`); the governor's rules still bound how
many, so the unlocks are candidates only while the constructors are below `cons_max`, and at the cap the best of
the seat's own plants is taken. A type is lacking when the seat has no finished unit of it, by the census's
whole bitset of types. **Gap**: below the cap the governor's other rules can still hold an unlock back -- the
army's share and the pressure -- and the plant waits while they do. CORE opened on
CORVP, its governor gave the constructors to the cheapest-army plant first, so the seat held CORCVs only and
CORCK's CORALAB (+0.517) was no candidate: 6 of 7 CORE seats teched up to CORAVP at −0.396 (MEASURED 2026-11-14,
the plant choice's bar runs r1 and r3). A tech-up whose best candidate scores below 0 still goes ahead (owner's
decision, [decisions](decisions.html)): the score ranks the plants' armies, while the tech-up also brings the
tech-2 constructors and economy, and the army orders keep a poor plant to constructors.
Each plant ordered is logged with the runner-up: `impai: plant seat= gametime= want=first|factory|tech T= reach=
chose= s= next= s=`. The decision that orders the second factory, sea or land, is logged once more, with
the naval judgement it read and its sea search: `impai: second seat= gametime= naval= chose=sea|land
sea_site=found|none|refused|-` ([the bar](bar.html), *The second-factory line*).
Offline over the retail content, with Q1's opening (the spec's formulas, the values the C build gives
within 1.3e-7, MEASURED 2026-11-14):

| side | land-linked | land route missing |
|---|---|---|
| CORE | CORVP, CORLAB, CORALAB | CORVP, CORHP, CORAVP |
| ARM | ARMVP, ARMLAB, ARMAVP | ARMVP, ARMHP, ARMAVP |

**Tech-up**: with `factory_max` factories at the seat's build tech, a steady metal income of the table's `tech_m` a
pass, and no factory ordered or pending, the next factory is the plant choice's above
the seat's build tech. The military's table input `F_TECH` is still its highest factory's tech level over 3, the
input A3's table was tuned on. **Anti-air structures**: one per
`aa_struct_v` of seen enemy air value, at most `aa_struct_max`, on the base ring.
**Ground defences** (`impai_econ.c`, THE GROUND DEFENCES) are reactive only, at every level: while the pressure
stands at the base, one, and one more for each `def_v` of pressing value, at most `def_max`, before an anti-air
structure in the ladder, outside an energy or metal stall. A ground defence is a building of category OTHER with a
ground reach -- a missile tower is one, by the engine's rule. The count is every such building of the seat's,
wherever it stands, built, a frame or at a builder's head, so `def_max` missile towers the anti-air rule built
against aircraft hold the ground defences back: by design, since each fires on ground units when no aircraft is a
candidate. The type is the one worth most against what the seat
sees, by cost: Σ over the picture's enemy types of share × the predictor's pair rate × [its ground reach ≥ the
enemy's] / its value, ties to the lower type, a water-only type only on a wet anchor, no unit list. A tower the
enemy outranges takes damage and returns none; without the reach term the light laser wins in the base game.
Offline against the opponent's T1 lab and vehicle-plant army at equal shares:

| | ARM | CORE |
|---|---|---|
| retail | ARMRL 7.19 (ARMLLT 6.57) | CORRL 7.32 (CORLLT 4.91) |
| Total Mayhem | ARMRL 6.57 (ARMHLT 6.41) | CORHLT 6.99 (CORRL 6.84) |
| ProTA | ARMRL | CORPLAS 9.65 |

The site is the ring site nearest **the defence's point**, the anchor moved `rally_px` towards the pressing member
(THE RING'S ORDER, `impai_build.c`); the ring's threat rules hold, so the tower stands at the threat's edge, which a
missile tower's range covers. **Gaps**: the score weighs the seen mobile picture, not the pressing members alone;
a tower is not moved when the threat does.

### The naval rule

As built (`impai_mil.c`, `naval` and the reach tests; `impai_brain.c`, `economy`; `impai_econ.c`, the ladder;
`impai_build.c`, *THE SEA PLANT*; `impai_maps.c`, the reach primitives).

**The judgement**, each military decision right after the picture, into `IMPAI_MIL.naval`: `naval = sea_blocks > 0
&& (mostly water || the estimate is off the anchor's land component)`; 0 on a level with no reach labels, and never
-1 after the first decision with an anchor.

- *Mostly water*: more than half the level's 32-px blocks carry a label of some class a sea army type (klass
  `SRAIDER..SAA`) travels by. Labels, not heights, so lava and land-locked shallows no sea class reaches do not
  count. The half puts Red Triangle (75 % of its cells under the sea) in and Trout Farm (40 %) and Lake Shore (39 %)
  out (MEASURED 2026-11-13 from the install's TNT heights).
- *Off the land component*: two blocks are land-linked when the classes whose labels agree there hold at least half
  of the content's land army types (`impai_land_link`), so a mod whose land army is mostly hovers or amphibians is
  not off. With no enemy structure known, the estimate's block is not land-linked to the anchor's; with structures
  known, less than half of their value stands on land-linked blocks -- their value, because a value-weighted
  centroid of two islands falls in the sea. No land army type, no estimate, a block off the grid: not off.
- *The terrain is public*: the labels answer over the whole map, as the economy's reach test does, because the
  skirmish screen shows every seat the map's picture, land and water, as it shows the start positions the estimate
  already uses.

**The sea plant** is the seat's next factory after its first, inside `factory_max`. The second factory waits for the
judgement (`naval >= 0`), an ordering: the two managers' order cannot decide which kind of plant takes the slot.

- *Wanted* when naval, no water factory stands, is a frame or is at a builder's head, one factory stands, nothing
  stalls, the seat's refusals are below `IMPAI_SEA_REFUSALS` (16) and some builder type of the census offers one
  (`impai_econ_sea_able`, at most 8 types' menus), and the metal income passes `factory_per_m`'s pacing as for any
  second factory: a plant begun on the commander's own income empties the store the economy runs on ([decisions](decisions.html),
  *The naval rule's two design calls*).
- *Chosen*: the least value among the menu's water factories whose own menu offers a sea army type.
- *Placed* by its own search, once a decision, its answer shared by the decision's builders: centre blocks in ring
  order around the anchor within `comm_r`, whose centre carries one of the sea class's labels within 48 blocks of
  the estimate and whose every footprint block lies at least the plant's minwaterdepth under the sea by the
  engine's own cell bytes `+0x05` (the bytes the site test's minwaterdepth compare reads,
  [orders](../engine/orders.html), the `0x47D2E0` row). Of those, a footprint the claims cover (a build order's, or
  a standing extractor's for as long as it stands) or whose centre the seat has not explored -- its own site test
  refuses that -- is passed over at no cost to the decision's budgets, the first 64 others are held against the
  fail ring, and the site test and then the way out decide. A candidate examined and not taken is passed by a
  cursor in the seat's state, so the next decision's walk resumes after it: the refusals are avoided for `fail_t`
  without a word of the shared fail ring, which a decision's 32 site tests would otherwise flush, pushing out a
  patch's or a land plant's failure before its `fail_t`. The cursor holds for the type and the estimate's block it
  was walked for and for `fail_t` from the walk's start, then the walk starts again at the first candidate. No
  candidate at all ends the want in the same decision, and the factory is a land one; that answer is remembered
  against the type, the anchor and the estimate's block, the only things besides the content it depends on.
- *Termination*: a decision whose search places nothing counts one refusal, whatever stopped it -- a refusal by the
  site test or the resolver, the claims, the exploration, the fail ring, the decision's budgets -- and so does a
  failed water-factory job; the count advances at most once a decision and once a `fail_t`, so a cause that passes
  (a unit on the footprint, water not yet seen) costs one, not sixteen. At 16 the sea plant is given up, so the want
  ends in a sea plant, a land factory in the same decision (no candidate) or a land factory at most 16 x `fail_t`
  ticks after the first count (48 minutes at Hard's 5400), the seat holding one factory meanwhile.

**Keeping to the layer.** Every military move goes through one function, which keeps the point when the member's
class reaches it and otherwise moves it to the nearest block of the member's own component within 32 blocks (a
bounded spiral, 32 a decision, cached by class, component and point), or gives no order (`unreached=`). A wave goes
only when at least half its squad's value reaches the target or a block of its own component within its range
(`squad_reaches`), so no land wave crosses a strait and a sea wave shells a coast; a raid only to a target its first
raider reaches; a scout only to starts it reaches, none when there is none; a member attacks a target only when it
reaches it or has it within its range + 384. A travelling member whose spread point it cannot reach goes to the
squad's point itself. Amphibious and hover units are on the LAND layer, in the land squad, and their classes'
labels cover land and water. The commander's safety window opens only while its move to safety is given or at its
head. **Builders** keep to their layer too: the explore cell must hold a block of the builder's own component --
its centre, else one within 3 blocks of it, where the builder goes -- and an assist's target one within reach, so a
sea constructor neither stalls the seat's one explorer nor walks at a plant inland, and a cell whose centre is a
rock is still explored.

**The predictor's sides close** (`impai_fight_side`'s `closes`) only when they can come within range of the other on
their own component; a side that cannot keeps speed 0 in the model, which is what an unreachable side does. Where
every class is one component both always close. Sea against sea in one water body is the calibrated naval kind; a
prediction made with a side that cannot close has no reference kind and is counted (`cross=`).

**Gaps** (named, not closed): submarines are weighed against surface ships only by `can_attack`; no land unit is
ferried; hover and amphibious plants are not preferred on island maps, and a hover minority in a land squad waits
with it; no advanced sea plant on tech-up; no sea or air scouting; land plants keep buying land army on an island
map, the table's shares not reweighted by reach (A4); mixed-layer predictions have no reference kind; the labels and
tops are the level init's, so features destroyed or created later, and a saved game's, are not in them; the sea plant
is at the earliest the second factory and with `factory_max` 1 there is none; any water factory counts as the sea
plant, even one whose menu has no sea army type; the sea plant's builder is not reach-tested (its failure counts); a
seat whose military refuses every decision keeps one factory (the mil line's `refused=`); sea constructors build no
tidal generator on a dry anchor; before any structure is seen the estimate is the nearest other start, used or not;
a FORM squad with no block of its component within 32 blocks of its rally point stays where it is (`unreached=`); a
naval judgement that comes late -- the estimate moving across water once structures are seen, as on Lake Shore --
finds both `factory_max` slots land plants and gets no sea plant, since none fits inside `factory_max` without
taking a slot a land plant already holds; a sea plant search that keeps failing holds the seat at one factory for
up to 16 x `fail_t`.

## The plan and the personas

Every level plays one plan, `IMPAI_PLAN_ROW` (`impai_brain.c`), which decides the same way at Easy as at Hard; a
level changes only how its decisions are carried out, its **persona** (`IMPAI_PERSONAS`, `impai_react.c`) -- the
owner's decision of 2026-11-14, after Nanolathe's Modern AI, MIT-licensed, whose persona, host and executor the
model follows (`internal/aikit/persona.go`, `host.go`, `cmd.go`; no code taken). The plan is A1b's economy with
the governor's fields, and the military's Hard row; nothing in it is noised, and the predictor's margins and the
economy's ratios are the plan's own numbers.

| plan field | value | plan field | value |
|---|---|---|---|
| `mil_roots` | 40 | `predict_max` | 12 |
| `roots`, `queue_max` | 8, 4 | `react_confirm` (decisions) | 1 |
| `aa_struct_max`, `aa_struct_v` | 6, 1000 | `micro_hp`, `micro_period` | 0.35, 90 |
| `rally_px` | 384 | `attack_margin`, `retreat_margin` | 0.10, −0.05 |
| `wave_value` (median units) | 8 | `scout_period`, `task_timeout` (ticks) | 1800, 2700 |
| `dgun_reserve` (share of energy storage) | 0.25 | `cons_open`, `cons_max` | 1, 12 |
| `army_per_cons` | 2 | `press_k`, `press_hold` (ticks) | 1.5, 900 |
| `hold_pm` (thousandths of the storage), `hold_fac` | 200, 1 | `mend_hp` (share of health), `mend_wait` (ticks) | 0.50, 900 |
| `mend_roots`, `repair_max` | 6, 2 | | |
| `def_max`, `def_v` (pressing value) | 4, 600 | | |

Every member attacks the focus where it reaches it (*The military*, step 5), up to two raids go at once, and the
tech-up's threshold is the table's `tech_m` (A3's, tuned with the rest of the table).

**The persona** (`IMPAI_PERSONA`, 16 bytes) is the seat's, recorded in its `IMPAI_MIL` so a replay and a takeover
carry the decisions out as the game did:

| | look_eco | look_mil | react | eco apm / burst | mil apm / burst | att_eco | att_mil |
|---|---|---|---|---|---|---|---|
| Easy | 60 | 90 | 45 | 10 / 3 | 15 / 3 | 4 | 2 |
| Normal | 30 | 45 | 15 | 24 / 4 | 36 / 6 | 8 | 3 |
| Hard | 15 | 30 | 0 | none | none | none | none |

The rows are starting points; the levels' calibration (P2) moves them through the persona lever, against OTA AI
at Hard: Easy to win 50-75 %, Normal at least 90 %, Hard 100 of 100. Hard's row is no limit at all, so a Hard game
is the game the plan plays with every order given at once.

- **The looks.** The economy decides every `look_eco` ticks (`impai_brain_due`: `GameTime % look_eco == 7 seat %
  look_eco`), the military every `look_mil` (`impai_mil_due`: `GameTime % look_mil == (7 seat + 8) % 15`). Every
  look is a multiple of 15 in 15..150, so no two seats' economies and no economy and military share a tick.
- **The reaction.** Every order a decision gives reaches the world `react` ticks later: the decision orders through
  a view of its WORLDQ (`impai_exec_view`) whose `order` keeps the command in the seat's queue
  (`IMPAI_EXEC.pend`, `IMPAI_PEND_MAX` 80 entries: one economy batch and one military batch), and the turn gives
  the entries due (`impai_exec_apply`) every tick, after the observation and before the decisions. The enqueue
  answers what the order would have answered at the decision's tick (the WORLDQ's `order_test`), so a decision
  falls back on a refusal exactly as with no reaction; an entry is bound to the incarnation of its unit and, where
  the seat knows it, its target, and the WORLDQ refuses it at the apply when the slot holds another (THE
  LIFETIME). `react` is at most the shorter look less 15, so each manager's batch is given before its next
  decision. No decision reads the queue: the delay is on orders, never on what the seat sees.
- **The budget.** Each manager has a token bucket: `apm` actions a minute, held to `burst`. The economy's action
  is a command -- a decision gives at most min(`roots`, the bucket's actions), the factories' reserve at most half
  of them (THE SPLIT) --; the military's is a squad served, its members' orders included, or a commander order
  (a flight, the leash, a D-gun). With none left the military's round robin stops at the squad, which the next
  decision serves first.
- **The attention.** `att_mil` holds the operations at once -- squads holding DEFEND, RAID or ATTACK --, and a squad
  that would start another is refused (THE ATTENTION, `become`); `att_eco` holds the builders busy at once, and the
  governor keeps no more constructors than that.
- **The commander between two managers.** Each manager's order to the commander leaves a marker, the decision's
  GameTime + `react` (`IMPAI_MIL.com_until`, `IMPAI_BRAIN.com_until`): the economy treats the commander as the
  military's while the military's lasts, and the military does not drop its order against a head the economy's
  pending order will replace. At react 0 neither is ever set.

The persona lever `tagpu_impai_persona.on` gives a seat a persona in place of its level's row, for the
calibration; `tools/impai/level_lever.py --persona` writes it. Each seat's init logs `impai: persona seat= level=
look= react= apm= att= from=level|lever`, and a limited seat's pulse an `impai: react` line ([ImpAI in the
engine](../modules/impai.html), *The persona*).
