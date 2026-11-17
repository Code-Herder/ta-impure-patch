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
- **The blind start** (`impai_brain.c`, THE BLIND START). The first plant comes once the seat has `open_mex` extractors finished and `open_energy` energy buildings standing, or at `open_t` (GameTime 1 800) -- and at once when its patch scan has read the whole map and published no patch, off a metal map: the opening's extractors have no patch to stand on, and only the plant's constructors explore for one. The scan's last row sets the fact (`IMPAI_SEAT.scanned`), so the rule never fires before the first whole scan, with a patch known, or on a metal map, whose extractors take the base ring. MEASURED 2026-11-16: the seat at Two Continents' start 3, whose nearest patch lies just past the commander's sight, gave no order to GameTime 1 806 in an unmapped game -- a network game's joining peer and a skirmish alike -- and its first at 81 in a mapped skirmish; with the rule the unmapped skirmish ordered its first plant at 81 and had four units by 1 806.
- **No patch another player built on** (`impai_build.c`, THE PATCH ANOTHER PLAYER BUILT ON). A patch whose box a building of another player's touches -- an enemy's or an ally's, seen now or remembered as a ghost -- is not taken, as one an own claim touches is not: the engine places no footprint over another building, so a builder sent there stands beside the other's extractor until its job fails, and is sent back once the failure lapses. MEASURED 2026-11-14 on retail Lava Mania under the loader and TADR: 10 of the 13 sites ImpAI's seat failed were an extractor ordered on a patch where OTA AI's stood, 3 of them on a patch the seat had failed on 5 400 ticks or more before. A patch the seat has not seen built on is still sent to and fails once; a ghost keeps its patch until the seat sees the block again. **Gap**: a BUILD job's timeout does not count the trip, so a patch the builder cannot reach in `job_timeout` fails (5 of the 9 sites Lava Mania's seat failed with the rule).
- **A constructor claims only held ground** (`impai_build.c`, THE HELD GROUND). A patch, and a feature to reclaim, is taken only on a 256-px cell the seat holds now: one its strength reaches before any known enemy strength does. The features fall under the rule because the wrecks the army's look finds lie where the armies fought, the contested band the rule exists for. The explorer keeps to it as well: the explore cell is the nearest unexplored cell the seat holds, since it is asked for once no held patch is left, when the nearest unexplored ground lies in the enemy's half. The first decision that places an extractor walks the threat maps' grid twice in 8-neighbour steps. The seat's side walks from its own and its allies' army -- units armed against the ground that move on it, held now or remembered; an aircraft holds no ground -- and from the base anchor; each of its towers marks its own cell and nothing beyond. The enemy's side walks from every identified enemy army unit, seen now or a ghost, whichever seat it belongs to, from each radar contact never identified while the military counts such contacts as an army type (`IMPAI_MIL.radar_as`), and from each enemy's base: every identified enemy structure that is not a tower, seen now or a ghost, and while an enemy the rows identify has none known -- or no enemy is identified -- every start position but the seat's own where an enemy's base can stand, the map's starts being public. Under a new skirmish's fixed starts the engine seats record r at start r (`0x4978DA..0x497972`), so the `starts` op names the record at each start (`WQ_START.seated`) and a start stands only for an enemy record seated there whose base the rows do not hold; an empty start, the seat's own and an ally's stand for nothing. Random starts, a network game and a saved game name none, and every other start stands as before. Each base is its own source, not the military's estimate: in a free-for-all the estimate, one value-weighted centre of every enemy's structures, falls by the seat's own base when two enemies stand on opposite sides. An enemy tower contests only its reach, which the spot's threat test already refuses (*The route*). A cell becomes held when its steps from the seat's side are at most three quarters of those from the enemy's, and stops being held only when the enemy's side is nearer; between the two it keeps what the last judging decision left it (`IMPAI_BRAIN.held`, one bit a cell, carried by the recording), so a front that wavers by a cell flips no patch. The held ground so follows the front: it grows behind a won push, where the army stands nearer than any enemy known, and shrinks after a lost one, and in a free-for-all every enemy counts. Before any enemy is identified the other starts that can hold it stand for it, so a cell is held up to three sevenths of the way to the nearest; with no other start and no enemy known every cell is held; with no maps every patch is; the commander keeps `comm_r` and its opening's far patch. In the owner's game on Comet Catcher the three ImpAI seats' extractors 2 048-2 559 px from their start had a median life of 4 470 ticks, against 11 010-17 250 in every other 512-px band out to 4 095; seats 1 and 3 started 4 340 px apart, so that band is the ground half way between them, contested while the armies met there (MEASURED 2026-11-14 from the recording). **Gaps**: a step is a straight cell, blind to water and cliffs, so ground across a lake counts as near; a ghost holds its ground for the enemy until the seat sees the block empty; an enemy the seat has never seen or heard on radar counts for nothing.
- **The base ring grows from the anchor.** Its sites are taken nearest the anchor first, ties to the site farther from the military's estimate of the enemy base, then the lower cell; the commander takes its own nearest where it stands, because its reach already holds every site it takes within `comm_r` of the anchor and its walk is the opening's cost. The ring's own order (`place_ring`, the east side first) grew every base east, toward the enemy at the bar's second start: in the cmdr run the structures' mean projection toward the enemy at GameTime 15 000 was +577 px at the second start and −146 px at the first (medians over 60 and 46 seat-games), and 1 961 constructors died at the second start against 551. Ordered from the anchor, the commander walked back from its far extractors to the anchor's side of the box, and the economy game's bar came at 4 218, past the 4 100 the Fleas' order leaves; ordered from where it stands it comes at 4 055, as in the ring's own order. The key is a function of distances alone, so a start and its mirror build mirrored bases. **The ring grows only for a lack of room**: a ring whose sites were left for the commander's reach, for ground threat at the site or on the route, or for the commander's safe distance (below) is not exhausted, so a raid over the base for a few decisions neither pushes the next ring out nor passes a plant type over. **The commander's safe distance**: no economy task sends the commander within 600 px (`IMPAI_CMD_SAFE_PX`) of a seen armed enemy -- the military picture's enemies, armed by their threat row or never identified --, the distance at which the military weighs a contact and flees it; otherwise the economy would send it back to the point the flight took it from once the window lapses, a cycle every 300 ticks. One predicate decides for a ring site, an extractor patch, a frame to help, a feature to reclaim or walk to and the sea plant's site; every task order to the commander refuses such a point, and each search skips it first, so another candidate is taken; a walk to a remembered feature that comes into sight there ends with no failure. More than 64 such enemies within `comm_r` + 601 px of the anchor refuse every commander point within `comm_r`; a point past it -- the opening's far patch, the finish's sea plant in a dry hunt -- is tested against every row of the view. **Gaps**: a ring longer than 256 sites loses its top side's tail before the order, as it did without one; a commander standing on the enemy's side of the base takes that side's sites first, so its buildings can lean toward the enemy, by at most `comm_r` from the anchor, ties going to the site farther from the estimate; near a contact the military would stand against (*The military*, the stand), the commander's tasks there still wait for it to leave.
- **The assist helps the products; no builder guards.** With no building frame to help, a builder helps the seat's unfinished mobile units -- the factories' products on their yards -- with the help order (resolver mode 8, HELPBUILD on any unfinished target), the commander only within `comm_r`. HelpBuild `0x403F70` adds the helper's work every tick and ends at its frame's completion (the "Building complete" notice at `0x40422F`, then done, `0x40423C..0x404248`; DISASSEMBLED), so a help never outlives its frame. **A help the engine refuses is not given again**: HelpBuild walks to within the builder's builddistance plus half of 16 × √(fx² + 2·fz) of the target's footprint -- the engine's own formula (`0x403FF9..0x404045`) -- and when the walk cannot end there it shows "I can't get there" and ends the help (state 1, `0x404069`; DISASSEMBLED). After the census, a builder listed idle whose job is a help given before this decision, and whose frame still stands unfinished, has its job ended and the frame's **cell** recorded for `fail_t` (`IMPAI_JOB.help_cx`, `help_cz`, `help_until`; `impai_brain.c`, THE HELP REFUSED), and the assist gives that builder no help on a frame at that cell until then. The cell, not the unit: a plant makes every product at its one build point, each a new slot. `help_refused=` on the `eco` line counts them. **Why**: on Trout Farm an ARMLAB made all 15 of its products in 10 000 ticks at cell (168, 414), and three ARMCVs north and west of it were given the help on each and stood Standby two ticks later, at every decision -- one changed its head 246 times in 3 600 ticks -- while the one south of the plant helped (MEASURED 2026-11-14, `bar-hota-troutfarm-ac-1`). A guard never ends by itself: its reactions pushed the attacks that took the commander past its reach, and the engine stands the guarder on the factory's yard, where it holds the factory's next product for as long as it stands (*The yard stays clear*). In the cmdr run the commander stood on Standby or with nothing in 21-22 % of its samples after GameTime 9 000 (the finish run 3-6 %) and helped or reclaimed in 3-5 % (33 %), while every seat-game had a factory within `comm_r`. **Not yet measured**: how much build power a help adds to a product's frame. **Gaps**: the refusal's cause -- the plant's body between the builder and its build point -- stays, and a builder that has since moved to where the help would work still passes that cell over until `fail_t` ends.
- **The yard and its way stay clear** (`impai_brain.c`, THE YARD). A factory creates its next product only when the engine's placement test passes its yard, and the test excuses no unit: BuildingBuild asks it at `0x402899` with the unit to ignore pushed as 0 (`0x402896`), and on a refusal sleeps 15 ticks and asks again, for ever ([map](../engine/map.html), *A lab can open its yard*). A unit standing just beyond the exit holds it too: the finished product cannot leave and stands on the yard. So a waiting factory -- finished, its head a BuildingBuild, its footprint holding no unfinished mobile unit -- has a **region**: its footprint and the rectangles of its kept way out (*A plant keeps a way out*, below). After the job and walk checks, each builder the economy lists idle or as a helper that touches the region -- grown by the builder's own half-size -- is moved off it to a point it can walk to **without crossing the plant** (`impai_build.c`, THE WAY OFF): a builder inside the footprint stands on the yard and leaves by an **exit** -- an edge holding cells a product walks off, read from the plant type's yard map -- to a point beyond the exit edge's line; a builder outside it to a point the straight line to which stays clear of the footprint. The point must leave the builder touching no part of the region, no building of the seat's, no builder the census lists where it stands (the commander included) and no point the decision holds -- every idle builder's live economy walk, recorded before the yard moves any builder (**the walks first**), so none is sent beside a walk whose turn comes later or that has left the region, and each point the decision has sent a builder to, one a builder --, and lie on its class's own reach label -- a builder whose own blocks carry no label is held to none. The engine's move to a point a unit cannot stand on never ends (Move_Ground ends only on the goal reached, [orders](../engine/orders.html)): on Lava Alley ca-4 two constructors on the CORVP's way were sent to points an idle constructor stood on, and on Steel Jungle ac-7 two to points 28 px apart; each stood with its move for the rest of the game and the plant waited, the store full (MEASURED 2026-11-16, diagnostic builds at c1dc2e70). A footprint with no exit readable (an air plant's pad, a yard map past the bound or unread) and a flying builder are searched round the builder itself, on the region and the label alone (a flier on the region alone). **The search runs rings**: round the middle of each exit for a builder inside, round the builder itself for one outside, 32 px apart, at most 17 (past a kept way's 256 px), eight directions each, the outward one first -- the exit's, or the one nearest the line from the factory's centre through the builder; the first ring holding such a point ends the search, and of its points the first that is off every factory and kept way of the seat is taken, else its first. A point pushed straight out of the exit met ground the builder's class cannot stand on past the way's end -- on Steel Jungle ac-7 the ARMVP's own product stood parked on its build point from GameTime 13 507 to the game's end, its only point (9472, 4952) on no label, the factory waiting and the seat completing nothing (MEASURED 2026-11-16, a diagnostic build) --, while the ground beside the way was the builder's; its job ended, it takes no other order that decision, and a helper the walk check turned back in the same decision is not the yard's -- its turn back is its one order. **A builder already walking off is left to its walk**: one whose one order is a point order -- naming no unit, no build, not the unit's idle behaviour -- to a point THE WAY OFF accepts, given by the economy within `WALK_OFF_T` (900 ticks), leaves its list with no order (`yard_left=`). The trust tests what holds still alone -- the decision's points and the idle builders the decision holds no point for, one whose walk has ended among them --, so a helper, a jobbed builder or the commander passing the point does not end a walk. Any other is moved again -- a point order through the plant, one to a point another builder now stands still on, or a walk the engine has not ended in 900 ticks --, **never to the point it was last sent to**, which did not take it off. **Gap**: only the last point is held off, so two points both closed by what the census does not list (an army unit, a busy builder, a feature its block does not show) can alternate, 900 ticks each. The commander inside its window is the military's and left alone, and **the army keeps off yards and ways where it is sent**: a squad member whose spread point touches a factory's footprint or a kept way out goes to the next of the 25 spread points that touches none (*The military*, step 5). **Why**: on Trout Farm a constructor guarding the seat's one factory stood on its yard for 27 450 ticks, the factory's next product created 5 ticks after it drifted off, and in another game for 61 350 (MEASURED 2026-11-14, the closing run). In the broad run's two economy freezes the factory was held from outside its yard: on Pincushion an Easy seat's army unit, a move at its head, stood on its ARMVP's exit rows from GameTime 16 000 to the end, and on Trout Farm two idle constructors of a Normal seat stood under its plant's exit from 31 621 (MEASURED 2026-11-14, the recordings replayed with a trace); the builder each held in on the yard could not leave, and the rule gave it the move again at every decision, 368 of the Easy seat's 407 orders and 539 of the Normal seat's 766, which starved its other builders and factories of the persona's actions. **Why not through the plant**: in round 3 at 57d39d71 four of the eight games ImpAI failed at Hard against the stock AI's Hard on Steel Jungle and Lava Alley were seats frozen for good -- nothing completed for 37 800 to 68 400 GameTime ticks, one Steel Jungle seat giving 31 orders in 81 180 ticks, `yard_left=` 2 724 to 11 494 decisions -- and a diagnostic build replaying four of them showed every idle constructor on a waiting plant's yard either inside the footprint north of its centre or on the kept way south of it, each sent north, through the plant whose exit and way lie south; the engine found no way round (Lava Alley's rock field and map edge, Steel Jungle's jungle) and the builder stood with its move for the rest of the game, holding the yard (MEASURED 2026-11-15). `yard=` on the `eco` line counts the moves, `yard_left=` the builders left to their walk. **Gaps**: a builder held in by what this rule does not move -- an enemy, a wreck, a unit by a plant ImpAI did not place, which keeps no way -- is moved once a 900 ticks and its factory waits; the reach label is the content map's, so a point it opens can still be closed by units the census does not list and by another player's buildings; a builder inside a footprint whose every exit opens onto another label is left where it stands; a frame past the census's 32 is not seen, so builders beside a producing factory may be moved once; with every spread point on a yard or way, a member keeps its own.
- **The assist in the hold.** In the hold (*The military*, the governor's paragraph) the assist helps only an extractor's, a geothermal's, an energy building's or a maker's frame: nobody helps a factory's product or a plant's frame, which its own builder finishes. A helper already on such work is given the ladder an idle builder gets, or stopped when the ladder has nothing for it.
- **A plant keeps a way out.** A factory's site is taken only when a bounded search proves, for each movement class of its ground products, that the class's largest product can leave it: from the places just beyond the yard cells of the footprint's edges, over places where the product's whole square fits the class's **content map** -- the class's pathing map as the level load builds it from the heights and the features (slope, water depth and blocking features, the class footprint folded in), with no unit: the engine's live map holds every player's units, seen or not, which no seat may read ([player](player.html), *No cheating*) -- and where the square overlaps neither the plant, nor a claim, nor a building the seat knows of: its own census's, and another player's its knowledge rows hold, over every footprint its last-known 32-px block allows. The search ends at a place 16 cells clear of the plant. Each class is searched on its own path: classes cross different ground (a hovercraft's map holds water a K-bot's does not), so the largest product's path proves nothing of another class's, and one path open to all classes at once is stricter than the units are. The paths, inflated by each class's square, are claimed for as long as the plant stands or is ordered, so no later building closes them. A plant whose yard, build menu or a product's footprint cannot be read whole is refused, never taken as one nothing leaves. A plant that traps what it builds was the measured cause of Core seats that built no army on Pincushion and Trout Farm: a feature below the exit, the constructors circling the yard for the rest of the game. A site the way out refuses is avoided by plants of its type for `fail_t`, in a ring of its own (32 entries), never the shared fail ring, so the refusals cannot push a patch's failure out of it. **The work is budgeted in cells, not searches**: a site's proof costs its box once and, for each product class, the content map's window and the box again, and a decision spends at most 98 304 cells, 0.59 ms at the worst 6.0 ns a cell measured with the DLL's compiler under Wine (`impai_build.c`, THE WORK BUDGET and THE COST) -- a retail plant's eight classes fit one decision. A proof that would pass what is left is deferred, not refused: the decision stops proving, and the next one, its budget whole, comes back to the site; the first proof of a decision always fits, so every decision proves or refuses one. A plant type refused 16 times in a row, or whose base ring is exhausted at its widest, is passed over by the factory choice for `fail_t` -- every unit type has its own entry, so no type is ever evicted from the record --, which takes the next type; when every type it would take is passed over, the plant goes to the first ring site the site test passes, with no way out, and the game's log names it (`impai: way waived seat= plant= cell=`): a seat with no factory loses more than one with a cramped exit, and a plateau or island start must not wait the game out. The rule, its bound and its termination are in `impai_build.c`, THE WAY OUT, THE WAY REFUSALS and THE PLANT'S FALLBACK; `tests/tasim_class_test.c` holds the content map to the core's fill, which `tools/tasim/classcheck.py` holds to the engine's, and under the check lever every level init compares it with the engine's own map before any unit stands (`impai: check pass`, judged by `impai_check.py`). **Gaps**: a building of another player the seat has never seen is not in the content map, so a product may meet it as a human's would; the content map is the level load's, which a re-mark lowers at the map's last column and row; a plant placed by the fallback has no proven way, and what it builds may circle its yard as on Pincushion; a product class whose proof alone costs more than a decision's work budget -- a mod's product of about 50 cells or more at the largest box -- is refused, and its plant falls back.
- **A site the builder could not reach is that builder's alone.** When a build ends with nothing built and the builder stands farther from the site than MobileBuild's own reach test allows (builddistance plus both footprints, [orders](../engine/orders.html), *MobileBuild's reach abandon*), the builder is the one held back, for `fail_t` -- a recorded end further off holds nothing, as a refused help's --, and no site is: the cause is where the builder stands, not the site, and failing the site for everyone emptied a whole map's patches one by one. A reclaim that ends with its feature still there or out of sight records the same, at the feature. While its record lasts the builder avoids the recorded spot and is given no site and no feature beyond its reach from where it stands, by the same compare, so it is not sent from one unreachable site to the next, each failing in turn; and it takes no factory, sea plant or tech-up, which go to a builder that can walk, so it cannot hold the seat's one plant want either. The record ends after `fail_t`; a builder still walled in then costs one more job a `fail_t`, not one a decision. Any other build that ends with nothing built fails its site for every builder.
- **A builder's label is its first cell's** (`impai_maps.c`, THE UNIT'S OWN LABEL). Every economy reach test
  compares the builder's own label with the site's: the label of the block holding its footprint's first cell, as
  the set-position `0x48A9F0` rounds it, `(x − 8 fx + 8) >> 4` px (`0x48AA00..0x48AA32`), the cell the class map's
  code describes a unit by -- its centre's block's only when that block has none. A centre within half a footprint
  of a block's edge lies in the next block, which a feature's footprint and ring can leave without a passable cell
  for the class: on ProTA's `bar-nota-pincushion-ac-2` at 57d39d71 the commander stopped at (3354, 5856) after a
  help, its centre's block (104, 183) beside a feature anchored at (204, 367) [INFERRED: no label], and every rule
  that reads the builder's label -- the assist, the extractor and reclaim spots, the reach of a site -- turned it
  down from 39 151 to the game's end, 89 % of its idle decisions (`com_idle` 55 %). **The label beside**: the
  labels are made once, at the level init, so a unit standing where a feature stood then -- reclaimed or destroyed
  since -- stands in a hole of them; where neither block carries a label, the one label the eight blocks round the
  first cell's block carry stands in, and none or two reach nothing. On Trout Farm's south start
  (`bar-hota-troutfarm-aa-1`) the commander stood from GameTime 52 300 to the cap where the level's Truck08 had
  stood (cells 222..227 x 433..436 hold no class-2 entry: its 5 x 3 footprint and the class's ring), the ring round
  it one component with the ARMSY site: each offer of the shipyard to it was refused by THE BUILDER'S REACH, no sea
  army was built against the stock AI's last base on the lake, and the game reached the cap ahead (MEASURED
  2026-11-16, a diagnostic build logging each offer). The military's reach reads keep the centre's block.
  **Gaps**: a block holding two components carries its first passable cell's label, so a unit on the other reads
  the wrong one, and the label beside can be such a block's; a hole wider than the blocks round the first cell's
  block reads 0.
- **Builders keep out of known ground threat.** Every builder's choice is refused when its site's 256-px cell is on the seat's ground threat map; for a constructor -- and for any builder's patch and feature -- also when **the route**, the straight segment from where it stands to the site, crosses such a cell (its own cell left out, so a builder standing in threat may still walk out). **The route** is one sampler, `impai_route_threat` (`impai_maps.c`, THE ROUTE): the straight segment sampled every 32 px in 64-bit integers, each 256-px cell it crosses past the start's read once on the ground threat map, a cell off the grid reading 0 and the point held to the coordinate limit, 0..32 767 px, first. The builders read it by its **CLEAR** rule: any cell above 0 refuses. The military reads its **OUT** rule. This holds on the base ring as on a walk: in that Comet Catcher game four of the 27 died at work in the base. **The walk check**, each decision: a constructor whose build has no frame yet, or whose explore walk or reclaim walk -- a RECLAIM_GO, or a RECLAIM farther from its feature than its build reach -- crosses known threat, or whose frame, assist target or feature in reach has the threat within reach (its cell or the builder's own), is turned back to the anchor, or to the nearest clear cell while the anchor's is threatened or its block is not reachable on the builder's class (by the cell's centre block), so a constructor is never sent onto sea or a cliff; an abandoned explore cell is failed for `fail_t`. The turned-back builder holds the move and no job, and the next decision lists it idle (**the hand-back**, *The military*, step 6), so a move whose point it never reaches -- the engine's ground move ends only on its arrival -- cannot hold it, nor the governor count a constructor the economy cannot task; the ladder's next site for it is threat-tested, route included. A ground defence's builder is no exception: one sent to a site at the threat's edge is turned back once the threat's reach covers its cell or the frame's. **The walk checks leave the factories their roots**: the orders before the factories' -- stops, turn backs, the ladder -- spend at most `roots_max` -- the plan's `roots`, or the economy bucket's actions when fewer -- less the factories' reserve: one for each idle factory, up to `queue_max`, in the hold up to the floor's `hold_fac` less the busy factories, the most its walk orders, and never more than half of `roots_max` (THE SPLIT), so a raid that turns back every builder still leaves each idle factory its order. **A radar dot is a threat too**: on the map the builders read, each contact the seat holds now and has never identified stamps the ground reach of the type the military counts it as -- the median identified enemy mobile type seen of an army class, else the seat's own median army type (*The military*, step 2) -- through one test the military's picture uses as well (`impai_row_unidentified_now`), the type the last military decision's (`IMPAI_MIL.radar_as`, at most `look_mil` old); a ghost is not held now and stamps nothing, and an identified row is never stamped twice. The commander keeps its own rules (`comm_r`, the military's safety and leash). **Gaps**: the route is a straight line, not the path the engine walks; a threat the seat has not seen is not on the map; the check runs once a decision and its turn back is carried out a reaction later, so a builder walks up to `look_eco` + `react` ticks into a threat that appears (15 at Hard, 105 at Easy), and under the economy's bucket a turn back waits as well for a decision with a command left: the bucket refills an action every 1800 / `apm_eco` ticks (180 at Easy, 75 at Normal), and the job checks' stops and the earlier turn backs take theirs first (`impai_brain.c`, THE WALK CHECK).
- **A claim is never dropped**: the claim table holds its bound -- one claim a census unit (an extractor's site or the build order at a builder's head), the kept ways' rectangles and one a command -- and the census walks at most the engine's units a player, so the table is never full; were it ever, the decision would answer every site claimed and place nothing, rather than place over a site or a way out it could not hold.
- **A helper answers the wants.** A builder at its assist -- helping a building's frame or a factory's product -- is given the ladder's rules of a want the seat has now: the energy of a stall, the plant (the sea plant, the next factory, the tech-up), a ground defence, an anti-air structure and a sensor, and while the seat is short of metal the reclaim; for anything else it keeps its assist, whose build power goes to work under way. A seat whose constructors all help -- the factories' products, frame after frame -- otherwise answers none of its wants until a build ends.
- **Reclaim when it pays.** A feature's reclaim pays its whole metal at the end of a countdown of about half a tick a metal, whoever reclaims it -- far faster than any extractor makes metal -- but only what the store has room for: the economy pass holds the store to its maximum and the rest is lost. So a feature is worth its metal held to the store's room, less what the seat's reclaims under way will pay, and a full store reclaims nothing. The ladder asks for a feature twice, never before the plants or the first `open_mex` extractors: **before an extractor**, once those are up, when the feature pays the builder more metal a tick than an extractor makes (the seat's steady income a pass over its extractors, over the pass's 30 ticks); and **before the buildings that spend** -- energy, makers, exploring, the assist -- while the seat is **short of metal** for what it wants next: stalling on it, or -- below a surplus, the store under 0.8 of its maximum -- making less a pass than its next factory's `factory_per_m` times the factories it has, or with all its factories the tech-up's `tech_m` while the tech-up's own count holds (`factory_max` plants of its highest build tech) -- past the tech-up no plant is wanted outside a surplus, and only a stall is short. **A surplus is never short**: the store pays the next want whatever the income, and the income's test alone sent Escalation's commander to reclaim beside a full metal store, ahead of the energy its seat was stalling on (MEASURED 2026-11-15, the economy diagnosis at d665a00a). **And it harvests** (`impai_econ.c`, THE HARVEST): a builder the ladder has nothing else for reclaims before it assists, whenever the store is below 0.8 of its maximum -- the metal it brings is what the seat spends next --, and not in a surplus, where the assist spends faster what the store already holds. A helper at its assist reclaims too while the seat is short (*A helper answers the wants*). A wreck is a feature like any other. On a metal-poor start the opening still goes to the first extractors, however far, and then to the rocks and wrecks near the base while the store has room for them.
- **The steady income.** Every income gate -- the second factory's and the sea plant's `factory_per_m`, the tech-up's `tech_m`, the stall, the projection `pm_in`, short of metal, and the reclaim's extractor rate -- reads the seat's metal a pass less the one-off metal the last pass settled: each unit's metal produced in the pass's period (its ledger's `+0xE4`) above its own make rate, metalmake plus makesmetal, for every unit but an extractor that can take a payout -- a frame, a factory, a builder (workertime above 0) or a reclaimer: the reclaim completion credits the reclaimer, a torn-down product's refund the factory, the unbuild the frame, a reclaimed unit's credit its reclaimer, so the metal credited to a finished structure that is none of them recurs and stays income (TA Twilight's ARMADVSOL, whose def makes no metal, made 1.34-4.03 a pass at every sample, 18.8 of the seat's 27.1 from six of them, MEASURED 2026-11-14). The economy pass writes a unit's produced metal only with an extractor's extraction, a maker's output and a finished unit's metalmake; between passes the reclaim completion, a factory's refund for a product torn down unfinished, the unbuild and a reclaimed unit's credit add to it ([weapons](../engine/weapons.html), *The ledger*), and the pass sums it all into the player's income `+0x9C`. A 75-metal rock or a cancelled product carried a plant want for one pass: 17 of 65 plant wants on the mods1 run rode on one, 13 of TA Twilight's 14, each below its gate without it, and the plants they ordered starved at the commander's 1 a pass (MEASURED 2026-11-14). The steady income lies in [0, the income]. Its residuals: a payout to a unit that died since the pass counts once as income, and so does an extractor's own reclaim; an extractsmetal structure with no yard map, which the WORLDQ does not class an extractor, has its extraction subtracted; and the energy twin -- tree payouts in the energy income, read by the energy stall and `pe_in` -- is not subtracted.
- **The content's price of metal** (`impai_tinfo.c`, THE CONTENT'S PRICE OF METAL): the ladder keeps an energy
  income of `e_per_m` times the projected metal income `pm_in` (with 1.1 times the energy spent, and a held plant's
  shortfall, the energy target), and `e_per_m` is the content's: the lower median of energy cost over metal cost
  among the types of an army class the classifier rates tech 1, computed once at the level init and logged on the
  `impai: tinfo` line as `e_per_m=`; the plan's 8 only for a content with no such type. **Why**: the keep is the
  energy that lets the metal income be spent, and a content's opening spends its metal on its tech-1 army at that
  army's own price. MEASURED 2026-11-15 from the five installs' roles dumps, the median over the tech-1 mobile types
  of the army roles: retail 8.63, Total Mayhem 8.65, TA Twilight 9.71, Escalation 12.07, TA Zero 2.40 -- the one 8
  starved Escalation's energy-heavy opening (its first products cost 3 000 to 3 600 energy) and kept more than three
  times the energy TA Zero spends.
- **The energy stall is the store's** (`impai_brain.c`, THE STALLS): the energy store below `stall_pm` (200
  thousandths) of its storage, whatever the last pass's flows. The stall holds the plants, the tech-up, the sea plant
  and the ground defences back, and is the ladder's first rung: every idle builder builds energy until the store
  stands above the line again, which it does only on more coming in than going out, so the stall ends by its own
  rule. **Why not the flows**: a pass's flows flip between a factory's products -- one asks nothing while its next is
  not begun -- and on Escalation's `bar-nota-pincushion-ac-1` at GameTime 7 604 the store stood at 6 % with 64
  coming in against 23.7 asked, so no stall was declared and the second plant was ordered inside it; ImpAI's store
  was empty in 56 % of the samples on Escalation against 17 % under the loader (MEASURED 2026-11-15, the economy
  diagnosis at d665a00a). The metal stall keeps the flows' test beside the hold, which reads the store alone.
  **The stall takes the reclaimers** (`impai_econ.c`, `impai_econ_assign`): in an energy stall a builder at a reclaim,
  or walking to one, is given the stall's energy -- the ladder's first rung alone, which replaces the job only when a
  site takes it, so one with no energy to place keeps its reclaim; a job given in the same decision is left as it
  is. Without it the job checks listed only an idle builder idle, and Escalation's commander reclaimed into a full
  metal store while the energy stood at 0. A builder on a build is left at it: its frame stands where it was ordered,
  and the stall keeps the plants that frame would be from being ordered in one. `stall_taken=` on the `eco` line
  counts them.
- **In a surplus the metal producers wait.** Past the opening -- `open_mex` extractors finished and `open_energy` energy buildings standing, or `open_t` reached -- the extractor rung (its reclaim before the extractor included) and the maker rung are skipped while the store is at 0.8 of its maximum or more **and the seat already makes more metal than it asks for a further plant**: below `factory_max`, `factory_per_m` of steady income a pass for each factory, at least one; at `factory_max` or more, the steady income less the seat's request at least a busy plant's measured request (*The military*, the tech-up's paragraph) (`impai_econ.c`, IN A SURPLUS THE METAL PRODUCERS WAIT). So whenever the extractors wait the full store has a spender: the next plant, the energy it waits for, or the factories standing, which the governor keeps busy as the persona's actions allow. Below that income no plant comes and the extractors are the only way to it, so they never wait, and with no factory asking metal -- one waiting at its yard -- there is no price and they never wait either: a store is a level and the waste the rule is for is a flow. On Trout Farm a seat with one factory and 3.3-3.8 a pass against the plant's 8 kept its store full on reclaim payouts while its one factory stood blocked, and held one extractor for 40 000 ticks (MEASURED 2026-11-14, the closing run's four Trout Farm games). The economy pass holds a full store to its storage and adds every further unit of metal to MetalWasted, the player's double `+0xD4` (`0x401AF0..0x401B27`, DISASSEMBLED), so an extractor or a maker built then adds only waste; the ladder goes on to energy, exploring and the assist. The extractor's type is still chosen, so a constructor explores only when no patch is left for it. **The opening is exempt** because a seat's store starts full: without it the rule held back the opening's own extractors (the first at GameTime 3282 against 529 in `impai_brain_test`'s economy game). WHY: in long games against the stock AI 34-53 % of the metal produced was wasted while energy stayed short (MEASURED 2026-11-14 from the recordings' player images). The makers already built are switched (next).
- **The makers are switched, all the time** (`impai_econ.c`, THE MAKERS), with the engine's own on and off: the orders `ACTIVATE` and `DEACTIVATE` (`0x403010`, `0x403040`), which set or clear `+0x10E` bit 0 only for an onoffable def, and the economy pass charges a maker's energy and pays its metal only while that bit is set ([orders](../engine/orders.html), *The on and off*). Before the ladder, each decision switches at most 4 of the census's onoffable makers that cost energy: **off**, the least metal per energy first, while energy is short -- the store below 30 % of its storage and the energy made below the energy asked -- until those switched off cost the shortfall, and while the metal is full -- at 95 % or more, with the steady income above the metal asked -- until they made the overflow; **on**, the most metal per energy first, while the energy store is at 60 % or more, the energy made above the energy asked and the metal below 0.8 of its storage, each maker whose cost the spare still pays. **Hysteresis by construction**: no decision holds both an off and an on, a maker switched by one store's crossing comes back only after that store has crossed the band to the other side, and a maker switched either way waits 300 ticks before its next switch (`IMPAI_JOB_SWITCH`), so a maker whose own script turns it back on is switched once in 300 ticks, never once a decision. **The flows' lag**: the flows are the last economy pass's, and a seat decides up to twice a pass, so each maker switched since that pass is put into them before they are read -- an active maker whose ledger asked no energy in the last pass was off through it, its cost added to the energy asked and its metal to the metal made, and an inactive one that asked was on through it, both taken off -- since the pass charges a structure's energy use only while it is active ([weapons](../engine/weapons.html), *The settle*). Without it the decision after a switch read the same shortfall or spare and switched a second batch for it. The ladder builds no maker while one stands off, and counts the lag and the energy this decision switched on; the plants' energy term reads the lag too. `makers_off=` and `makers_on=` on the `eco` line count the switches. **Why**: the owner's game on Comet Catcher, its seat 3 with 5 makers, held its metal store at 95 % or more 76 % of the time from GameTime 9 000 while in an energy stall 30 % of it, and wasted 26 % of the metal it made (MEASURED 2026-11-14 from the recording). **Gaps**: a maker past the census's 64 is counted but never switched; one that is not onoffable, or costs no energy, stands as it is.
- **Features are known as the game draws them for the seat** (`impai_build.c`, THE FEATURE MEMORY; `tasim_sweep.c`, `tasim_feature_drawn`). The seat knows a feature exactly when `DrawGameScreen` would draw it for the seat's player: on explored ground every feature, live, greyed outside sight, except a `nodrawundergray` def -- six wall defs --, which is drawn only to the player its owner bits name or where a sheared corner of it is in sight ([map](../engine/map.html), *What the feature draw shows a player*). So the seat knows what a human in its seat is shown and no more, by construction, and a feature on unexplored ground is never taken. It remembers what it was shown (at most 128); where the draw does not show a feature now -- unexplored ground, a gated def out of sight -- the entry is a belief, which anything may have reclaimed, burnt or crushed since. Each decision looks within 768 px round one builder and **round one army unit**, each in turn in slot order: the wrecks lie where the army fought, which the builders, kept out of known threat, rarely reach -- in the owner's four-seat game on Comet Catcher the three ImpAI seats reclaimed 1 548, 1 731 and 4 149 metal in 47 minutes (MEASURED 2026-11-14). A full memory gives the place of the belief shown longest ago, and with every entry shown now, of the entry of least metal when the new feature has more -- never an entry a reclaim job claims, so a job keeps its claim, its footprint and its metal in flight. What the seat knows comes from the draw, what it risks from its own sight: each feature is scored with a confidence of `1800 / (1800 + age)`, the age the ticks since the seat's own sight last covered its anchor's block (the whole GameTime for one it never covered), a half one game minute after -- the draw shows every wreck on explored ground as now, but the threat round it is known only within sight, so a wreck on a battlefield the army left scores below one the seat sees: with every feature scored as seen now, the broad check's reclaims ordered doubled and the builders lost on them rose from 7 to 20 in 24 games (MEASURED 2026-11-15). The choice is the most metal times confidence a tick by the reclaim sweep's own model -- the walk past the builder's builddistance plus the reclaim countdown, which has no worker-time term -- among those within 2 048 px of the builder -- a walk inside the job's 2 700 ticks at a builder's speed; the score itself prices the walk --, unclaimed, not its own failure, under no ground threat, on ground the seat holds, and for the commander within `comm_r` of the anchor. The danger tests are the same whatever told the seat of the feature. A feature shown now is reclaimed; a belief is walked to, and reclaimed when the draw shows it there. Shown gone -- its place drawn without it, or playing the sequence a paid reclaim or a death begins, which no reclaim can take -- it is dropped from the memory for every builder, and its builder moves on in the same decision; a builder another hand gave an order ends its reclaim with no failure; a builder that gave up with its feature unknown or still there avoids it alone for `fail_t`, as an unreachable site. A burning tree is left to that rule: the seat's view of a feature carries no burn state. `harvest_lost=` on the `eco` line counts the builders lost on a reclaim or the walk to one, beside `reclaim_gained=`. On a metal-poor start the map's rocks and wrecks are the metal nearest at hand. **What bounds the harvest** is the store and the ground, not the search (MEASURED 2026-11-14 with diagnostic counters on the bar's games): the Hard seat of troutfarm-ac-1 passed the harvest rung for a store at 80 % or more 1 444 times, the assist spending instead, against 161 reclaim searches in the whole game, and the one of troutfarm-ca-1 had no room left, after the reclaims under way, at 1 101 of 1 269 asks; with room, 42 of 87 searches in troutfarm-ac-1 and 87 of 146 in the Easy troutfarm-cc-1 found every scored feature within 2 048 px past a land builder's reach (wrecks in the lake, [INFERRED]). No walk timed out, and the route refused no feature.

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

1. **The army**: the seat's finished mobile units with an army class (the fourteen, by layer and role), at most
   512. **A type that deals no damage has no class** (`impai_class_of`): the classifier's damage per second, ground
   or air, must be above 0, so a jammer or a radar unit, which takes role SCOUT for having no regular weapon, is
   neither bought nor counted as army -- 17 retail types, the scouts' jammers, spies and radar units and the radar
   planes ([decisions](decisions.html), *No army class without damage*). **Gap**: a weapon the engine never fires
   still counts, as Total Mayhem's jammers' vertical-launch rocket, whose scripts have no aim script for it. each in one squad, its job `IMPAI_JOB_SQUAD` naming it. A unit with none joins its layer's FORM squad.
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
   type **of an army class** -- never a commander, a builder or a type that deals nothing --, else the seat's own
   median army type: early in a game the identified mobiles are the commander and its builders, which made every
   dot a commander with its D-gun (ARMCOM at 12 of 13 retreats on Lava Mania against TADR's install) or a target
   that deals nothing (CORCA, ProTA; MEASURED 2026-11-14). The air classes stay in: a dot carries no height, and
   the stay-or-go (step 4) decides what a squad does about an aircraft. **The air held** (`impai_mil.c`, THE AIR
   HELD): each air class's most value seen at once is held for `IMPAI_AIR_HOLD` (5 400) ticks after the seat last
   saw that much or more of it, and counts in the class's share, the total and `air_seen`, which the anti-air
   structures' rule reads (one per `aa_struct_v`, at most `aa_struct_max`): an aircraft is seen when it comes, so
   between raids the picture held no air, the anti-air share fell to 0 and the structures' rule read none -- on TA
   Twilight's `bar-nota-pincushion-ac-1` at d665a00a the stock AI built 94 aircraft and ImpAI lost 221 of its 293
   units to them, having built 7 anti-air units and 3 anti-air structures. With no air seen the anti-air share is
   0, as the table's rows give it. It fills A3's twelve features (`impai_picture_features`) and the enemy's mobile composition the type
   choice weighs, and the enemy base's estimate: the value-weighted centre of the enemy structures known; with none
   known any longer, the last such centre, kept; before any was known, the start position nearest the seat's own
   that is not it (the starts are public). **Why the last centre**: a wave razes what it sees, and the rest of the
   base lies nearer where that stood than any start the enemy was never seen at -- on `bar-nota-plainspasses-ca-2`
   at 95c8d47b the estimate fell back to a start 3 600 px from where a wave of 185 had razed the structures it
   saw, the sweep went nearest it for the rest of the game, and the stock AI's base, its commander among 65 units,
   was never seen again (the cap, 561 units against 74; MEASURED 2026-11-15). **The commander's memory**
   (`IMPAI_MIL.foe`, one a seat): a foe's commander in the seat's own line of sight (`IMPAI_LOS`), its slot, block
   and GameTime -- never an ally's sharing (`IMPAI_SHARED`), a position only, which may describe a unit that has
   left the slot since; nothing else writes it, and the sweep forgets it (below), never a read of the other seat.
   **Contacts**: the enemies seen now, joined across 256-px cells within 768 px, seeded nearest the anchor first,
   at most 32.
3. **Defence**: a contact threatens the base only through a member that is **mobile and armed** (any dps in the
   threat table, or a radar contact never identified) and stands **nearer the anchor than its own side's base**
   -- than every known structure of its owner (of every foe for a contact never identified), else than every
   start position that can hold an enemy's base (*the foe's starts*, below) -- within `base_r` + 512 of the anchor or 512 of an own structure that itself
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
   **The home** (`home_side`), after the defence and the commander's safety: what defends the base now, the one
   side the recall predicts and the pressure weighs -- every member of a FORM, DEFEND or RETREAT squad of any layer
   standing in the anchor box, each at its own point (a home squad outside the box, a field retreat 4 000 px out
   or a FORM squad still walking home, defends nothing yet); every finished own structure there with a weapon the
   fight content holds; and the commander there while the military's window is shut (a commander in flight is
   leaving the fight). Each threat's gap is its point's distance to
   the home's member nearest it.
   **The recall** (`recall`), after the home: once the base has had a threat standing at `react_confirm` decisions
   in a row -- the base's confirmation, not a squad's, so a home with no squad confirms its threats too --, **one
   prediction a layer group**: for each layer with an ATTACK or RAID squad out that can hurt a threat, the group is
   every threat such a squad can hurt, and the whole home is predicted against all of the group's members and the
   armed enemy structures near them, each enemy once, at the group's nearest gap; the commander's D-gun counts
   while it could fire at one of the group (the stand's rule, step 7). **The commander fights for nothing of its
   own**: it fires and is fired at, but its value is no part of the margin (`IMPAI_FE_NO_VALUE`; its entry is the
   side's first, and the predictor's merge keeps a flagged entry before any other and never merges one into
   another, so a side of more than sixteen entries never counts it as units of a cheap type -- the stand's
   commander too, whose entry carries `IMPAI_FE_KEEP`, a flag nothing but the merge reads, when the D-gun may
   fire), which is what the
   rest of the home keeps against what survives of the group -- a home of the commander alone keeps all while the
   group does not hurt it, so a fight in which neither side hurts the other is still a margin of 0, and once hurt
   holds only by destroying the group. Worth 24 109-30 423 at retail, the reason the home's value for the
   pressure leaves it out, it made the home's kept share about its own health, so a raid that killed every tower
   while it lived recalled nothing. A group equal to an earlier layer's takes
   its margin without a prediction, and an empty home is a loss, margin −1, with none. Below `retreat_margin` the
   home is losing, which stamps the pressure, and unless a squad of the layer is already on its way home -- a
   RETREAT member of the layer outside the box -- the layer's ATTACK or RAID squad nearest a threat it can hurt
   becomes RETREAT: it travels to the rally point, each member going back to FORM as it arrives,
   which the defence makes DEFEND while a threat it can hurt stands. **A recalled ATTACK gives its wave back** when
   its wave counted: its layer's wave count falls by one, held at 0, so harassment that brings waves home raises no
   threshold. **The wave's count**: an ATTACK whose wave added to the count carries a bit (`IMPAI_SQUAD.counted`),
   cleared when a squad takes a task of its own and carried into the ATTACK a squad merges into, so a lead's wave,
   which never added to the count, takes nothing from it, and a counted one demoted at the lead's end gives its own
   back. At most
   one recall a layer a decision, at most three predictions, each made only while the decision keeps one for
   every squad whose retreat test may follow -- a FORM, DEFEND, ATTACK or RAID squad with a contact within its
   reach, the home it holds aside; the recall runs after the commander's safety and the D-gun, so the commander's
   two predictions come first. It is counted among the `defends`. **Why the whole home**: a wave takes its
   layer's whole FORM squad, so a home judged one layer at a time was empty in every layer whose wave was out --
   a land raid the land army beat recalled the air and sea waves --, and towers and the commander kill a lone
   scout the squads alone read as a loss. **Why a group and not a threat**: against each threat apart the home
   was predicted once a threat and layer, all of `predict_max` under a raid of ten contacts, so the commander
   fled with no D-gun and the waves went untested; and two prongs it beat one at a time but not together recalled
   nothing. **Why every member**: the recall asks whether the home holds the base, not whether a squad should
   leave, so an enemy it can neither hurt nor outrun is just what it cannot hold against -- three tanks under a
   gunship are a loss and the anti-air wave comes back --; a fight in which neither side hurts the other is a
   margin of 0, above `retreat_margin`, so aircraft that hurt nothing at home recall nothing. **Why one on its
   way**: a recalled squad counts once it stands in the box, so without the rule every decision until then would
   find the home still losing and recall the layer's next wave too. **Why the base confirms**: a wave takes
   the whole FORM squad, so the home has no squad until a new unit is finished, and a recall that waited for a home
   squad to confirm the threat never came while raiders stood in the base. **Why RETREAT**: as DEFEND the recalled
   squad merged into the home's DEFEND squad, whose centroid its far members dragged out of the threat's reach, so
   the home defenders stopped fighting and travelled; the retreat keeps it apart until it is home, and is no
   operation, so a seat at its `att_mil` still recalls. WHY: an ATTACK has no lifetime, and on Trout Farm
   the stock AI's army of 17 killed the commander at home while 24 of the seat's units chased outlying structures
   on the far side (GameTime 38 434), and in another game an ARMMAV killed it beside a wave of 67 about 2 400 px off
   (76 160; MEASURED 2026-11-14). **Gaps**: a group takes a contact whole, so a land wave that can hurt a
   contact's tanks is weighed against its bombers too; and a squad on its way home holds its layer's next recall
   until it arrives, however small it is.
4. **Tasks**, each squad round-robin. An ATTACK squad's **goal** is chosen again first, before and whatever the
   contact test finds, the first of these the squad reaches: an enemy commander the seat sees now, the nearest
   (its slot); a remembered one last seen within `IMPAI_FOE_FRESH` (5 400) ticks in a sweep cell no army unit of
   the seat has stood in since; **the enemy factory the seat knows, seen now or a ghost, nearest the squad** (its
   slot) -- a plant is where the enemy's army comes from, and the estimate is pulled to outlying structures: on
   Trout Farm the waves went for (6128, 2416) and (5360, 4240) while the stock AI's base stood on the far side
   (MEASURED 2026-11-14) --; the enemy base's most valuable known structure within 1 024 px of the estimate;
   the **sweep**, the cell whose centre members holding at least half the squad's value reach by label that an army
   unit of the seat stood in least recently, ties to the nearest the estimate (so with nothing known it starts at
   the enemy's start position and spreads out from it), then to the lowest index -- the squad's reach test holds
   there by construction, so a mixed squad goes where its army goes, not where a hovercraft listed first goes. The
   known goals pass over one within 256 px of a goal the squad's layer has given up (*the goal's progress*, below)
   and take the next nearest. **The commander comes first**: it is the win condition of a
   commander-death game. A RAID ends first, before the contact test, at `task_timeout`, when its target is no
   longer known, or on arrival. Then the nearest contact within the squad's longest range + 384 that has a member
   entering the squad's **stay-or-go** is predicted against those members and the armed structures near it that
   are not members, each structure weighed once (the fight predictor, at most `predict_max` a decision) -- below `retreat_margin` the squad goes back
   (RETREAT) to the rally point, `rally_px` from the anchor towards the estimate; otherwise it fights. **The side
   that fights** is the squad's members first, then every army unit of another ATTACK, RAID, DEFEND or FORM squad
   that stands within its own range + 384 of the contact's member nearest the squad, whatever its own squad is
   doing, so no squad is predicted alone beside the seat's units in reach of the same enemy, and a side past the
   predictor's 64 types drops a helper's type before one of the squad's own. Two squads at one contact are still
   weighed apart, each from its own nearest member and front, so they need not take one answer; a RETREAT is
   leaving and a SCOUT is one scout, so neither counts. **At the rally**: a FORM squad whose centroid stands
   within 512 px of the rally point, against a contact that threatens the base, takes no retreat and spends no
   prediction. **The retreat home**: a squad of any task that loses its prediction while every member already
   stands within 512 px of the point a retreat would take it to, a point found (each member at the index it takes
   in the RETREAT squad it would join) takes no retreat and, that decision, not the contact; the lost prediction
   still counts as the lead's evidence, a FORM squad forms its wave if it would win, a RAID and an ATTACK go back
   to FORM where they stand, and a DEFEND keeps its task and travels home that decision -- so a squad at home that
   has lost does not walk on at its goal. A DEFEND keeps its task because the defence runs before the tasks and
   makes a FORM squad with a threat in view DEFEND again: sent back to FORM, a held DEFEND went DEFEND, FORM and
   DEFEND at every decision, a defence counted at each. A member of a class with no block of its component within
   1 024 px of the rally -- a ship with the rally ashore -- is measured against its **home point** (*Keeping to
   the layer*), so a lost sea ATTACK falls back to the water nearest home; a member with no point at all, none
   within its home point's bound either, is not home: held as home, a lost sea ATTACK went on at the force it lost
   to. **Why the retreat home**: without it a squad whose retreat would end where it stands became RETREAT, was
   released to FORM in the same decision and was predicted again at the next, for as long as a contact stood within
   its reach: on ProTA's Lava Mania
   (`bar-nota-lavamania-ca-2` at c1dc2e70), the rally 900 px short of the estimate, FORM squads were sent back 221
   times between GameTime 12 000 and 40 000, every one within 394 px of the rally, and raids 108 times, by contacts
   411 to 845 px off; no attack was given from 15 262 to 38 032 and the game reached the cap at 7 009 against
   50 567. On hota Red Triangle cc-1 at the same commit FORM squads were sent back 359 times from 30 000 to 63 000,
   344 within 512 px of the rally, and raids 229 times (MEASURED 2026-11-16, diagnostic builds logging each
   retreat). **Gap**: a squad so held may
   stand within reach of the contact it lost to -- artillery out-ranging the rally -- answered only by its units'
   own fire, and is predicted again at each decision; a squad of members with no point becomes a RETREAT whose
   members are stopped and released where they stand at the next decision, so it stops without withdrawing, and
   as FORM it is sent back every other decision while the contact stays in its reach; a stopped unit's Standby, at
   the roam move state, may take a target in its reach and chase it until a decision stops it again. **Why the side**: 28 of the 35 retreats of
   `bar-hota-pincushion-ca-2` at 1a9350ea had an ATTACK of the same layer within 1 024 px -- a squad of 41 worth
   9 755 fell back from the enemy's commander and towers beside two squads worth 9 133 that stayed --, and the
   game reached the cap at 414 units against 40 (MEASURED 2026-11-15, the recording's `s_mil`). **The home
   holds**: a DEFEND squad whose centroid stands in the anchor box (step 3) while a base threat stands takes no
   retreat and spends no prediction; it fights. A retreat goes to the rally point, towards the estimate, so a
   defence retreating from a raid that has passed the rally point walks towards the raid, gives no attack order on
   the way and is made DEFEND again by the next decision's defence: a home defence that kept losing its prediction
   went DEFEND, RETREAT, FORM and DEFEND with no attack order while the raid stood in the base. **A member
   enters** when the squad outruns it (its speed below the squad's slowest member's; a structure's 0 always is) or
   some type of the squad hurts it (a pair rate above 0, over the squad's 8 dearest types); one never identified
   is weighed as the substitute, and with no fight content every member enters. An enemy the squad can neither
   hurt nor outrun costs it the same whether it stays or goes, so it cannot decide between them: retail aircraft
   fly at 9.6 px a tick against 1.27 on land, and 185 of 200 retreats of a ground army against the stock AI on
   Pin Cushion had an aircraft nearest, the army sitting at home in RETREAT or FORM from GameTime 41 000 to 72 600
   (MEASURED 2026-11-14). A contact with no member that enters is no contact for the squad. With no
   contact a FORM squad of a layer worth `wave_value` + that layer's waves median army units of that layer attacks
   the goal above when the prediction against **the goal's side** passes `attack_margin`: the enemy mobiles seen now
   that enter its stay-or-go, wherever they stand, and every enemy structure the seat holds a row for, seen or
   remembered, armed or not, within 1 280 px of the goal (`GOAL_SIDE_PX`, the contact's 768 and the defences'
   512). The stay-or-go at the goal weighs a base's contact with its unarmed structures, a structure always
   entering, so the wave leaves only against what decides whether it stays. Why: on `bar-hota-plainspasses-ac-1`
   at 1001266a the waves left predicted against the goal's armed structures and the mobiles seen at home -- 6
   entries against 23 at GameTime 42 600, margin 0.121 -- and fell back at the base's contact against its lab,
   wind and solar plants, builders and defenders -- 12 against 13 at 42 690, −0.448 --: 50 retreats, each the
   whole way home, and the cap with 280 units against 61 (MEASURED 2026-11-17 with a build logging both sides).
   **Gaps**: the contact floods 768 px at a time over what is seen, so a base spread past 1 280 px is weighed short
   at departure; the enemy's mobiles at its base count only once seen, the picture keeping no mobile it does not
   see now. The other way the goal's side can be the heavier, and a wave waits where it would have won: the
   contact holds what is seen now, the goal's side the remembered unarmed structures too, and a remembered row
   stays until its block is lit again, so a structure destroyed out of sight weighs until the seat looks. It forms none while its arrival at the goal is undecided (below). **Each layer
   counts its own waves and takes its own median** (`IMPAI_MIL.waves`, one count a layer), so each new wave of a
   layer is one median unit of that layer bigger, and **a wave that destroys its target starts its layer over**:
   an ATTACK whose structure target the seat no longer holds a row for, while it stands within its range and 256 px
   of the target's point, resets its layer's count to 0, once. A structure's row stays, a ghost, until the seat
   sees it gone, so its absence proves it destroyed; a mobile target's row (a commander seen now) goes when it
   leaves sight and radar, and a full enemy list proves no absence, so neither resets anything ([the
   decisions](decisions.html), 2026-11-14). Why: with one count and one median, on Red Triangle 52 sea waves raised the land
   threshold to 67 median units, a median the submarines pulled up, and a land FORM squad of 24 sat at home while
   the enemy rebuilt its plants (MEASURED, the closing run's ac-1). In a dry hunt a wave of any layer but land goes
   at 1 + its layer's waves median units (*The dry hunt*, below); in the lead a wave of any layer goes at one median
   unit and adds nothing to its layer's count (*The lead*, below). `waves_l= waves_s= waves_a= wave_resets=` on the
   `mil` line count them. **One arrival, at one point**: the wave, the arrived ATTACK's move on and the orders all
   test the goal's 32-px block centre, the point the squad stores, and the substitute's cache never decides an
   arrival -- a member whose substitute a full cache (32 a decision) cannot give is not arrived and the squad's
   arrival undecided, so an ATTACK keeps its goal and a FORM squad forms no wave. On Red Triangle a sea squad
   flipped FORM to ATTACK and back 52 times without moving while its members stood 1 000 px and more off their
   goal, each flip a wave (MEASURED; the cache's part [INFERRED]). **Gap**: a squad of more than 8 types tests its 8 dearest, so a minor anti-air member of a
   large mixed squad does not make it count aircraft; a RAID of three to six of a FORM squad's fastest units
   (scouts aside) goes for the known enemy extractor or energy building of least ground threat, up to two at
   once; one SCOUT walks the start positions it reaches in turn -- first those no army unit of the seat has stood
   in, so a trip cut short still leaves a start not yet looked at for the next -- and then the sweep's
   cells (ties to the nearest the scout), three legs, every `scout_period` or while the estimate rests on no
   structure. **The scout goes deep** (owner's decision, 2026-11-17): a contact does not end its trip -- only its
   third leg, no leg left or its lifetime --, so a scout is expendable, and at sea a leg to a start ashore is a peek
   at the shore the next leg pulls it away from; what it sees becomes rows, and the stand-off shells a remembered
   defence from them. **The scout's layer**: the scout is the first of land, sea and air whose FORM squads hold one with a
   start to go to -- the dearest of the layer's scout class, else the cheapest of its fastest quarter --, so a land
   army that reaches a start scouts as before, and a seat whose land army reaches none still learns where the enemy
   stands: on Gods of War no land unit reached a start, and the seat sent no scout and saw no enemy to the cap, a
   submarine and a destroyer at home (MEASURED 2026-11-17, `exploit-naval-turtle` at 5e4e9c24). **The layer's last
   healthy unit**: while the estimate rests on an enemy structure -- the layer's wave, forming or waiting, has a
   goal known --, the scout is never the layer's only healthy army unit (at least half its health,
   whatever its squad): the pick takes only a unit below that, and none when there is none. While nothing is known
   the rule holds nothing, so a navy of one healthy ship still scouts: on an island a sea wave is one median unit,
   and a rule that held a forming wave's only unit whatever was known would never scout with it. On Gods of War
   the scout took the second of two destroyers, the one healthy, the first at 750 of 3 150 hp, east across the
   map from GameTime 15 600 to the cap while the enemy's tower and commander were known (MEASURED 2026-11-17 at
   75236135's code). A ship reaches a
   start, which stands on land, when a block of its component lies within 1 024 px of it. A leg is arrived at as a
   squad's (*the arrival*): within 256 px of the point travel sends the scout to -- its spread point when it reaches
   that, else the leg's substitute --, and a leg travel gives no point at all is over at once. **The starts looked at**: before any enemy structure is known the estimate is the start nearest the
   seat's own whose sweep cell no army unit of the seat has stood in and whose centre the land army reaches, the
   nearest other start once all have been; only a start that can hold an enemy's base is a candidate -- **the
   foe's starts**: not the seat's own and, where the level seated its starts, neither left empty
   (`WQ_SEATED_NONE`) nor an ally's, the held ground's own rule --, and with none there is no estimate, as on a
   level of one start; the scout's legs and the base threat's sides read the same starts; the sweep's ties go nearest the estimate, so the waves look at each candidate start in
   turn. On Trout Farm's ten starts the estimate held the empty centre start and the scout's
   trips ended at their first contact before the west start, where the stock AI stood: found at GameTime 55 980,
   and `bar-eota-troutfarm-cc-1` reached the cap at 9d21b778 (MEASURED 2026-11-15). **An ATTACK has no
   timeout**: it ends by retreat, or when it has **arrived with nothing to attack** -- members holding half its
   value within 256 px of the points travel gives them (a spread point, the point, or the substitute where the
   class stops short of it), and no member attacking -- and no goal it has not arrived at: its point's sweep cell is
   stamped as its own members', the goal is chosen again, and with none, or the same place (a commander it sees
   and cannot attack, a structure it cannot hit), it goes back to FORM; and no wave forms at a point the squad has
   already arrived at. **The goal's progress**: an ATTACK's goal progresses when its nearest member comes 256 px
   nearer it than at the goal's last progress, and while a member attacks; a goal that has not progressed for
   `task_timeout` plus the squad's travel to it (the task lifetime's allowance, below, at most 16 200 ticks) is
   **given up**: its block goes into a ring of 16 shared by the layers, each entry its layer's, which that layer's
   known goals pass over for 16 200
   ticks, its sweep cell is stamped, and the squad moves on as on arriving. A goal moved more than 256 px, a goal
   taken on moving on and a new task are measured afresh. So no goal its members cannot reach -- a pass the engine's
   path search does not find, a cliff the reach labels join -- holds the squad, or the persona's room for an
   operation, for good. Why: on `bar-nota-plainspasses-ca-2` (bb11df10) Normal's three operations went to one
   sweep cell on the north edge, (6736, 336), and never reached it in 45 000 ticks, so never stamped it; the sweep
   chose it at every decision, no wave could start, and a FORM squad of 255 units worth 48 000 stood at home while
   the enemy's base stood unswept (MEASURED 2026-11-15, the recording's `s_mil`). `stalled=` on the `mil` line
   counts the goals given up. **Gaps**: a squad whose nearest member edges 256 px nearer once a window keeps the goal
   another window; a goal chasing a target that moves more than 256 px a decision is measured afresh each time.
   **A RETREAT releases each member as it arrives** home, within 512 px of the point travel gives it (about the
   rally point, or its home point), or stopped where it stands with no point:
   the member joins its layer's FORM squad, which walks to the same point and fights what reaches it, and a RETREAT
   left with no member is freed; so no member home waits for another, and a squad that joins the retreat from
   farther out holds none of those already home. **Why**: the whole squad waited for its last member, and every
   squad that fell back joined it and restarted its lifetime, so the retreat never ended while the waves kept
   falling back: 60 % of the army's value stood in RETREAT from GameTime 30 000 on `bar-hota-pincushion-ca-2` at
   1a9350ea, 195 units of 414 at the cap, and 40-44 % in `-aa-2`, `-ac-2` and `-cc-2` (MEASURED 2026-11-15, the
   recordings' `s_mil` every 150 ticks from 20 000).
   **Every other task has a lifetime**: a RAID,
   a RETREAT and a SCOUT, and a DEFEND with no contact in its reach, go back to FORM once `task_timeout` -- the
   plan's time *at* the point -- plus the travel has passed since the task began: twice the rally point's
   straight line to the task's point at the squad's slowest member's maxvelocity, at most 16 200 ticks; a
   RETREAT's point is the rally point itself, so its travel is **the whole way home**, from where its retreat began
   -- its centroid then, or the farther point of a squad that joins it, which starts its lifetime again. From the
   rally point to itself it would be 0, and from the way still left at each decision a retreat ends at
   (T + 2W/v)/3 of a way W, so a wave recalled from farther than it walks in `task_timeout` would turn FORM on the
   way home and could leave again as a wave. So no
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
   it, and is given nothing nearer. The attacks are ordered before those moves, so the decision's orders go to the
   fight first. **Every member fights**, whatever its health: the squad's side in each prediction, its wave's
   threshold, the home and the stand count every member, so every member counted is one that fights -- no army type
   heals by itself, and a member that left fights below a share of its health would leave every fight for the rest
   of its life while counted as a fighter ([decisions](decisions.html), *The step back: removed*). **The stand-off**,
   before an ATTACK squad's targets and travel: a member never closes on a static defence it outranges -- an armed
   enemy structure seen now or remembered, its reach R its type's ground reach (the threat row's `hit_range`), the
   member's own at least R + 128 px (`STAND_PAD` 64 + `STAND_HYST` 64) and the fight content giving it a rate on
   the defence. Against the nearest such defence within its reach + 384 px, a member inside R + 64 px moves out to
   R + 128 px on the line from it through the member, the point held to the map and then to its substitute; the
   move is given only when the point it goes to lies outside R + 64 px -- a bay whose nearest water is under the
   tower, or the map's edge at the member's back, gives none, and the member then attacks the defence if it is seen
   and is otherwise left to the fight and travel. Else it attacks the defence when it is seen now, from its own
   range; else it attacks the ground at the defence's remembered block -- a structure stands where it was seen, and
   the member need not see it: a destroyer reaches 660 px and sees 330. **The hysteresis**: the move out ends 64 px
   past the line that orders it, and the member outranges the defence by those 128 px, so a member that stops short
   of its point or closes to its own range to fire still stands outside R + 64 px at the next decision. **Progress**:
   an attack on a defence is progress only when the member has it in range or travels to a point in range (the
   defence's own point or its substitute); a defence out of reach is shelled as a ghost is. Such a defence -- a
   ghost, or a seen one the member is neither in range of nor inside the reach of -- is attacked only while the
   squad's layer has not given its block up, by the stand-off or by the fight, and shelling it is no progress
   (*the goal's progress*), so a squad whose only work is such a defence gives its goal up in `task_timeout` + its
   travel, and the defence's block with it, and travel then lights the block or meets the defence alive. Why: on
   Gods of War the first destroyer sailed to the water nearest its goal, the enemy commander, 231 px from a light
   laser tower (reach 300 px), and lost 2 400 of its 3 150 hp there in 600 ticks (MEASURED 2026-11-17 at 75236135's
   code). **Gaps**: a defence not yet seen is met at the member's sight, which can be inside its reach; mobile units
   are not stood off; at most 32 defences a squad, the nearest its centroid; a member with weapons of several
   ranges is placed by its longest, and the engine may close it further for a shorter one (not measured); a seen
   defence out of reach whose block is not given up is a fight candidate whose attack counts as progress. A travelling squad moves to a 5 × 5 grid of points 64 px apart, each held to the map (the order check refuses a point off it, and a VTOL reaches any point), a member whose point would
   have it touch a factory's footprint or a kept way out to the next point of the grid that does not (the yard
   rule, *Task allocation*), which the arrival reads too; a move toward a unit passes no target (a move with an enemy target is a reclaim or a capture, [orders](../engine/orders.html)). No order
   repeats what the unit's head already says.
6. **The commander's safety**, before the squads' orders, by the seat's own knowledge alone; three steps, and
   the first that moves the commander ends it:
   - **The blast.** A foe's commander the hunt's memory (`IMPAI_MIL.foe`, the seat's own sight) holds as seen within the last
     `look_mil`, its row still a commander type, is fled while nearer than its type's `blast_r` + 64 px + the
     two commanders' approach over the horizon `look_mil` + `react` -- the next decision and its orders' delay
     (ours at its speed, the foe's over the horizon and the time since it was seen). ARMCOM's and CORCOM's blast is 475 px (COMMANDER_BLAST, areaofeffect 950): in b7 r1 fourteen
     games ended with both commanders dying in one blast, 17 to 484 px apart. **The flight opens the gap**: of
     the anchor and the own structures within `comm_r`, those at least the keep from the threat are safe, and the
     commander walks to the safe one that opens the gap the most for each px walked, the farthest only when none
     is safe. **The blast's approach**: for 600 ticks from a foe's commander's sighting (`IMPAI_BLAST_SEEN_T`: the
     flight's 300-tick window and up to as long again past its hand-back), no economy task walks ours nearer to where it was seen and to within the keep, the
     flight's own (`blast_keep`) with the age in it held to the same 600 (`impai_blast_nearer`, the economy's
     commander refusal and every search's skip). It is the walk that is tested, not the site: the commander works
     within its builddistance of a site, so a site within that walks it nowhere and is never refused, and
     otherwise the walk ends that far short of the site; a move -- an explore, a walk to a feature believed there --
     ends at the point itself. Away or across is never refused. `blast_refused=` counts the decisions the approach
     shaped -- something passed over, not the commander denied work --; the commander's idle share, `com_idle=` of
     `com_free=`, is the measure of its harm. **Why 600**:
     held to the hunt's 5 400 ticks instead, the keep grew to about 6 600 px at CORCOM's speed and turned one
     sighting into a minute-long ban on every site nearer the foe than the commander, the opening's worst; the
     measured case needed 540. **Why**: on
     `bar-eota-lavamania-ac-2` at 1a9350ea the Easy seat's commander, handed back after fleeing the enemy's
     commander seen 1 000 px off, walked 130 px towards where it was seen while that commander, unseen for 540
     ticks, walked 550 px towards it; seen at 393 px, ours fled north to the structure farthest from it, across
     its line, and the seat's army killed the enemy's commander 302 ticks later with the gap at 342 px: both
     died, 57 units against 14 (MEASURED 2026-11-15, the recording's units and `s_mil.foe`). **Gap**: a commander
     unseen can come from where it was not seen, and is fled only once seen.
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
     a factory, `FAR_PATCH_K` (2) × `comm_r` before, the opening's far patch, and in a dry hunt, the finish's sea
     plant -- walks to the anchor.

   **A flight stays in the base**: to the anchor or the own structure within `comm_r` of it that lies farthest
   from the threat, ties to the anchor (with the farthest structure anywhere, 94 of b7 r1's 105 moves went past
   `comm_r`, at most 6 553 px). Each move makes the commander the military's for 300 ticks, a window the
   economy's census and the D-gun honour; a window that ends more than 300 ticks after now -- which no move
   sets, only a corrupt record -- holds nothing (`impai_com_safe`), and every order the military gives the commander -- a flight, the leash, a D-gun --
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

**The governor** gives each idle factory one order a decision, in the army orders' walk -- the plants that offer
a land fighting class in ascending order of their best class value, then the rest, ties in slot order (*The army
orders*, below) -- (`impai_econ.c`, THE GOVERNOR): a
constructor when three rules hold, otherwise one army unit. **The opening**: the first `cons_open` constructors need
nothing. **The army's share**: past them, a constructor needs army ≥ `army_per_cons` × (constructors − `cons_open`
+ 1). **The pressure**: no constructor while the military's stamp lasts (below). A constructor order the factory
refuses leaves it for the army in the same decision. **No rule ties the constructors to the income**: they build
the extractors that pay for them, so a cap proportional to income holds a seat to the income it already makes.
MEASURED 2026-11-14 over 57 ImpAI seat-games of the bar (GameTime 9 000..27 000): such a cap (24 workertime a metal
a pass) was the only rule refusing a constructor in 50 % of the samples past 9 000, and one of two in 8.5 %; the
seats held 2 / 3 / 4 constructors at 15 000 / 21 000 / 27 000 against 6 / 9 / 11 before the governor, and
6 / 8 / 10.5 extractors against 8 / 12 / 14; the seats stalled in 44 % of the window against 21 % ([decisions](decisions.html), G3).
**The hold** (`impai_brain.c`, economy, THE HOLD): while the stored metal is below `stall_pm` (200) thousandths of
the storage, outside a surplus and outside the pressure, the seat holds every spend that is not economy. At most
`hold_fac` (1) factories are busy: the walk gives no order to an idle factory once the busy ones and its own orders
reach the floor (`held=` on the `eco` line), in its usual order -- a factory a budgeted seat gave five in a surplus
before the hold finishes them first --, and the floor's factory takes whatever the governor gives it, a constructor included -- the hold refuses no constructor, which would recreate the loop above. No plant
past the first is started (the second factory, the sea plant and the tech-up wait); a plant already begun is
finished by its builder. The assist helps only an extractor's, a geothermal's, an energy building's or a maker's
frame and helps no product; a helper on anything else is given the ladder an idle builder gets,
or stopped when the ladder has nothing for it (`released=`), and its job ends only with the order that replaced its
head. Each is an economy order, through the funnel and the bucket; under a persona's bucket the yard moves, the maker switches and the idle builders
leave the decision's last command to a helper the hold must release, so every decision with a command to give
releases one and no persona leaves a helper on a factory's work for good; the job and walk checks' stops and turn
backs come first. The pressure lifts the hold: the army is what a pressed seat needs. The test is the store alone, not the
stall (`m_stall` also needs income below demand, which turns false the pass after the hold cuts the demand, so the
factories would be released into an empty store and the stall come straight back); `stall_pm` is the buffer the
released factories start on, and the plan's one store threshold: the metal and energy stalls' store terms read it
too, so the hold starts where a metal stall does. **It has no hysteresis, because it does not flap**: MEASURED 2026-11-14 over 62
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
army's value by class, finished and queued -- the count nodes hold the units in progress, so a frame is counted
once, by its node) and its type from `impai_class_type` with the predictor's pair
rate. **The scout first** (owner's decision, 2026-11-17): a layer's scout class -- `lscout`, `sscout` or `ascout`, a
mobile armed type of the classifier's role SCOUT (CORFAV, ARMFAV, ARMFLEA on land, CORPT at sea) -- is the class of the
order whenever the seat holds none of it, built, building or queued, the menu offers it and the table gives it a
share, so a layer's plant makes its scout first and replaces a dead one with its next order. **The scout is one**:
no other buy takes a scout class -- the table's share (0.05 on each surface layer in the default table) is only the
switch, and the surplus's cheapest buy passes them -- and its order is one unit, never a surplus's five. A share is a
share of what the layer can build, so 0.05 grew to a sixth of a navy of destroyers and skeeters alone, and a plant's
cheapest type is often its scout: Gods of War's seat built five CORPT and two destroyers by the cap (MEASURED
2026-11-17 at bf92df19). In surplus a factory whose table buy finds nothing buys its cheapest army type that is no
scout, and one with no army type at
all is counted (`fidle=` on the `eco` line). **A seat with an action budget queues up to five units an army order
in a surplus**, the engine's own shift-click ([orders](../engine/orders.html), the build button's count), and one
unit otherwise: one order -- one action of the budget -- keeps a plant busy for five products, and a seat with no
budget reaches each idle plant at its next decision. The Easy seat on Pincushion (`apm_eco` 10) stood beside a full
store from GameTime 34 000 with 1 to 3 of its 5 plants asking metal at any sample and wasted 40.6 % of its steady
income: its plants waited on actions, not on metal. The Hard seat on Trout Farm given five an order grew its army
past the military's 512 rows and reached the game's cap at 81 000, 677 units against 28 (MEASURED 2026-11-14).
**The five are bounded by the store**: the metal above the hold's line (`stall_pm` of the storage), less the metal
already committed past the unit each factory makes -- **the queued metal**, the busy factories' count nodes past the
unit in production as the census reads them, and the decision's earlier orders past their first unit --, shared by
the orders the decision may still give, buys the order's units at its type's metal cost, at least one -- so the
units the queues hold past each factory's first never commit more than the store above the line the hold defends as
the decision read it, decision after decision: the next decision sees the queues this one left. It bounds the
queues, not the store: the units in production, the builders' work and the one unit an order always buys still
spend it, and the hold answers the store when it falls through the line. Counted within one decision alone, the
next one committed the same store again to the factories newly idle. A metal cost that is not finite buys one. **No army order takes the army past the
military's 512 rows** (`IMPAI_ARMY_MAX`): the governor's count of finished army units and count nodes' units, a
superset of the units the military lists, refuses the order, and an order of five takes no more than the rows leave;
a unit past them is in no squad and never takes an order. **The finish's rows**: on a level that can hold the dry hunt
(labels, a land army type and a block some sea class reaches), an order of a type with no water weapon stops at 448
rows (`IMPAI_ARMY_MAX` less `IMPAI_FINISH_ROWS`, 64; `army_rows`), so whatever the land army has grown to, 64 rows
hold units with a water weapon or are free for the finish at sea (*The military*, the dry hunt): the dry menus offer
water weapons alone, and a land army at 512 when the hunt began left the sea plant nothing to make. A level that
cannot be dry keeps every row for any type. **The army orders** (`impai_econ.c`, THE ARMY ORDERS)
hold each factory to the classes its units are good for here, at the plant choice's tech level and reach: a land
fighting class goes to a factory whose best reaching unit of it scores at least min(0, the seat's best of that class over
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
`bar-eota-troutfarm-cc-1`). **The island's land budget** (owner's decision, 2026-11-17): while the military
judges the seat on an island (*The island*, below), the land army classes (land raider to land scout) take an
order only while their value -- built, building and queued -- is below a quarter of the whole army's, tested
before each order, so the land army passes the quarter by at most one order; the base faces only what crosses the
water. The cap waits for the economy's reach to agree (**the island's agreement**): no army type of a gated class
on a menu of the seat's factories, as the army buy offers it, reaches the targets (*The reach*, below, the estimate
and the foe's starts on an island), read from the menus and so the same with or without values. Every linked map keeps Q3 whole. **A poor plant beside a store in surplus** buys its cheapest army type of any class,
so no factory idles beside a full store. With no values there is no gate. BARb's build-power cap and its army/builder split are the ideas, read
for design only. MEASURED before it, the constructor quota (12 at most, four plus one per three metal, taking
every idle factory first): the first army unit at a median GameTime of 13 500-15 000 against a first factory at
3 300-3 600, five or six constructors and no army at 12 000 at every level (196 seat-games, 2026-11-13); in a
game against a human, 95 % of the factory-ticks went to constructors.

**The targets bought for** (`impai_econ.c`, `army_menu`): a factory's army menu holds only the types whose weapons
can hit a target class the seat knows or expects, so a class none of whose types can is not bought, and a class is
bought in the types that can. **The target classes** (`impai.h`) are where an enemy stands as the engine's
can-engage test `0x49ABB0` sorts it ([weapons](../engine/weapons.html), *The can-engage test*): **dry** -- above
the sea and not floating, which a waterweapon refuses (`0x49AC0F`, a hovercraft `0x49AC3B`) --, **floating** -- a
ship or a structure on the water, which both kinds hit --, **under** the sea, which only a waterweapon hits
(`0x49ACFB`), and **air**, which the fight content hits only through an anti-air slot. A type's weapons
(`impai_tinfo_attack`, from the role input, so the DLL and tasim agree): a regular weapon that is neither a
waterweapon nor a toairweapon hits dry and floating targets, a waterweapon floating and under, an anti-air slot the
air. **Known**: each military decision stamps the classes of the enemies it counts (`IMPAI_MIL.tg_t`, THE TARGETS
KNOWN) -- under for a row held under the sea, air for a VTOL, floating for a type placed on water only, dry for
anything else, a contact never identified included, since the radar finds nothing under the sea --, and a class
stays known for `IMPAI_TG_HOLD` (5 400) ticks after the last decision that knew one. **Expected**: the ground of
each start that can hold an enemy's base -- dry where it lies at or above the sea, floating and under below it,
where the commander walks the sea floor. With nothing known and no start every class is bought: no evidence
narrows nothing. **Why**: on Gods of War the island seat's shipyard made two submarines of 1 199 metal each while
every enemy it knew stood on land (MEASURED 2026-11-17, `exploit-naval-turtle` at d8799688, the seated variant).
With something known and no start that can hold an enemy's base -- fewer than two starts, or every other one empty
or an ally's -- dry is kept beside it: nothing says where the base stands, so an aircraft known alone does not
leave every land plant without anti-air buying nothing; where a start is expected dry would add nothing, since a
type that hits dry hits floating too. **The plant choice** scores a plant only by the types that pass this filter
(`plant_score`), so it never takes a plant for a unit its factory would not buy. **Gaps**: a radar contact on the
water reads as dry, so a submarine waits for a ship's identification; a regular weapon that deals no damage still
counts; the anti-air classes now wait for an aircraft known as well, which the default table's anti-air shares (0
before air is seen) already did; an enemy land unit wading below the sea with no `floater` reads as dry though a
waterweapon hits it (`0x49AC0F` refuses only a target above the sea), and a landed aircraft reads as air though it
is a surface target an anti-air weapon cannot hit.
**The pressure** (`impai_mil.c`, THE PRESSURE), stamped by the military for the economy: the army-class enemies
seen now -- and the radar contacts never identified -- that are mobile and armed and stand at the base by the
defence rule, or within two 256-px cells of any own structure at any distance and nearer it than their own side's
base. When the home (step 3 of the military) is worth less than `press_k` × the value of those at the base, or
the whole army less than `press_k` × the value of those pressing an outlying structure, or the recall finds the
home losing, the stamp holds for `press_hold` ticks
with that value and the pressing member nearest the anchor, and whether it is at the base (`press=` on the `mil`
line, `pressed=` on the `eco` line). Enemy commanders and builders never press: a commander walking among the
expansions would hold the constructors all game. A radar contact presses only while the substitute it counts as
has a ground reach -- the reach the builders' map stamps for it --, so with an air substitute, or none, the radar
holds no constructor and builds no ground defence; **gap**: a radar contact carries no type, so one aircraft on the
radar presses while the enemy's median seen mobile type has a ground reach. **Why the whole army for an outlying
press**: the home defends the base and nothing beyond it, so weighed against the home, with the waves out one armed
scout beside an extractor held the constructors for `press_hold` and lifted the hold. The economy reads a stamp
past now + `press_hold` as none, so a recorded value holds nothing longer than that.
**The plant choice** (`impai_econ.c`, THE CHOICE) runs once a decision, the first time a builder reaches the
factory rule or the governor runs, for the seat and not for that builder: an ARMCV's own menu shows only ARMAVP
and ARMHP. **The candidates** are the distinct factory types on the menus of the seat's finished builder types (at
most `IMPAI_SEA_BUILDERS`, 8, builder types), then on the menus of **the unlocks** -- the constructor types the
seat's factories make that it has none of finished, at most 8 -- at most `IMPAI_PLANT_CANDS`, 16, candidates, each
one whose own menu holds a
constructor that builds an extractor, a water-only plant only on a wet anchor, and a plant the way-out record passes
over only in the waived pass (*A plant keeps a way out*); past the first plant on an island (*The island*, below)
when no candidate the pass may take has a land army -- the choice's own reach agrees with the military's verdict,
which on Trout Farm came and went on a linked map --, only a water plant or one whose menu offers an air army
type, the owner's decision (2026-11-17), and the further plant follows **the island's plant order** (below): a land
plant while the seat has none on an island big enough for one; else the sea plant, by the sea plant's own search
while its refusals are not spent, until the seat holds two sea plants (`ISLE_SEA_BEFORE_AIR`) and none of air, and
then an air plant. The air plant comes in place of the sea plant only once a search finds no site at all or no
builder of the seat offers a sea plant: a builder that cannot place it now (not its turn, a site past its reach,
another sea type on its menu, its far record live, none on its menu) leaves it to the decision's other builders and
to later decisions, so a plant held for the rest of the game is never taken for one builder's answer. The default
table buys no aircraft, so such a plant makes constructors alone. **The build tech** filters them: a plant's build tech
(`IMPAI_TINFO.ctech`) is the highest tech its constructors that can build an extractor build -- the
constructors its candidacy asks for, so a builder that only lays mines, decoys or jammers does not raise it: TA
Zero's GoKT2AF was a tech-up candidate only through GoKT2AirJammer, a tech-2 builder whose menu is mines, decoys and
a jammer (MEASURED 2026-11-14; owner's decision); a builder's is at least 2 when its menu holds an extractor past
**the extractor step**, 1.5 times the best extractor a commander builds (`impai_tinfo.c`, THE EXTRACTOR STEP), since
the graph counts factories from a commander and TA Zero's ARMCOMMANDER builds ARMT2GF itself: every type of the
content read tech 1, no plant scored above it, and the seat ordered ARMT2GF (1 100 metal, 2 640 energy) as a
further tech-1 plant at 3 187 with three extractors and its energy store empty (MEASURED, the mods batch at
d665a00a). The step only raises, and it separates every install's plants: tech-1 plants' constructors build at most
1.14 times their commanders' extractor (TA Twilight's ARMAMEX), tech-2 plants' 2.0 times or more (TA Zero's
ARMT2MEX; retail, Escalation, Total Mayhem and TA Twilight 2.8 to 3.4) (MEASURED 2026-11-15 on the installs' menus)
-- and a tech-up takes only a plant above every
factory the seat has, frames included; a first or further plant only one at or below that, or at the least among
the candidates, so the income gate stays the only way up and a mod whose cheapest plant brings tech 2 still has a
first plant. Not the plant's units' tech: the classifier's tech is a graph distance, so CORHP's units are tech 2
while its CORCH builds nothing above tech 1 -- the least value with that offer took CORHP as CORE's tech-up in 167
of 170 seats and ended the tech-up there for good (MEASURED 2026-11-14 over 377 bar seat-games). **The score** of
a plant is its best unit's: the highest value (*The values*) among its types of a land fighting class -- raider,
assault, artillery -- its **land army** -- with a value above 0 that **reach** a plausible enemy base, at the
seat's build tech held to 1..4 (the enemy assumed at the seat's own tech), and for a tech-up at the level above it,
the tech the candidates are built for (owner's decision): against the tech-1 field a tech-2 unit saturated, Total
Mayhem's CORAVP and Escalation's ARMAVP scoring 1.000, where the same Mayhem seat at tech 2 scored CORAVP 0.831
against CORHP 0.288, the same order (MEASURED 2026-11-14); none when there is none. **The reach** (THE REACH): a movement
class's component at the base is the label most blocks within 8 rings (256 px) of the anchor's block carry -- a
larger class's block can be impassable right at the commander's start, or a pocket beside it, while the yards
round it stand on other ground, and the anchor's 0 read as no evidence made CORREAP "reach" on Trout Farm where
CORGATOR did not --, none there meaning the class does not reach. The targets are the military's estimate once it rests on enemy structures
seen or remembered (`IMPAI_MIL.est_seen`), and before that every start that can hold an enemy's base -- **the
foe's starts**, the military's and the held ground's own rule: not the seat's own and, where the level seated its
starts, neither left empty nor an ally's: the estimate is then only the nearest such start, which on Trout Farm
is cut off by water while the real enemy was land-linked. An empty start on the seat's own island made every land
class reach on Gods of War, so the seat opened on a vehicle plant whose army reached no enemy (MEASURED
2026-11-17, `exploit-naval-turtle` at 5e4e9c24). On an island (*The island*, below) the targets are the estimate
and the foe's starts together -- a start seated at random left out once enemy structures are known, as the military
reads it --, so the economy agrees with the verdict only when its land army reaches none of them: an estimate taken
from structures first seen on water, a lake's tidal generators or shipyard, stands where no land class reaches on a
map whose enemy base the land army does reach. A class reaches when one target has a block of its base component within 8 rings: any, not all,
because before the enemy is known the map needs a class's extra mobility only when no plausible enemy is reached
otherwise ([decisions](decisions.html)). A VTOL always reaches; with no labels for a class, no military, no anchor
or no target every class reaches. A hover's mobility therefore adds nothing where the land classes reach and is
what makes its plant score where they do not; a route that is only much shorter over water does not count.
**Gaps**: at random starts, until a structure is seen, a seat whose real enemy sits on the one start cut off from
it builds land plants, and an ally's start is a target. **The best** is the highest score, then a plant the seat's own builders build before one that waits for
an unlock, then the least value, then the lower type, so with no values the choice is the least value among the
plants the seat can build now. **The opening** (no factory counted yet) is the least value among the candidates with a land army, unscored,
and the least value of all when none has one: its first purpose is the constructors, and A1's economy bench passed
on the least value ([decisions](decisions.html)); the land army is the owner's (2026-11-14), since the least value
alone put TA Zero's GoK on GoKT1AF, an air plant. **A wet anchor** takes the least value of all: the score ranks land armies, and a ring round an anchor below the sea may hold no site
for a land plant. **The island's first plant**: on a dry anchor, when no candidate has a land army and the
military judges the map naval, the first plant is never an air plant while another can be picked, and is the sea
plant when **the island's plant order** says sea (below), placed by the sea plant's own search, while the
level has counted no refusal of the sea plant; a builder that offers none builds the least value of all as before,
and so does every decision once one refusal is counted -- a search that orders none counts one in its own decision,
a sea plant's job that ends unbuilt counts one before the next decision's assignment --, so the first plant tries
the sea once and then builds on land: a seat whose sea plant cannot be finished waits one failed job for its first
plant, not the sixteen `fail_t` windows the sea plant's own want may spend. The candidates read are those the pick
may take, a build tech no higher than the seat's top factory's or the lowest candidate's. The opening's purpose is its constructors and the army it can make next, and on an
island the sea's is the one that reaches: on Gods of War the seat opened on a vehicle plant at GameTime 2 974, its
shipyard came on the pacing at 9 552, and it held two ships at the cap, 20 000, and attacked nothing (MEASURED
2026-11-17, `exploit-naval-turtle` at 5e4e9c24). **The island's plant order** (`isle_order`, the owner's decision of
2026-11-17) is a calculation, once a decision, over the plants the seat's builders offer and the constructors on
their menus that build an extractor: **the sea first** when sea defences are needed -- an enemy on or under the water
known (*the targets known*) -- or the metal lies at sea, the open water patches a sea plant's constructor reaches
being at least the island's (a tie is the sea's); **a land plant first** when the island is big enough that its
constructors pay, at least `ISLE_PATCHES` (6) open patches of **the island** a land plant's constructor's extractor
fits, more than the sea's, and at least `ISLE_BLOCKS` (1 024) blocks of the island within 48 rings (1 536 px) of the
anchor; never an air plant first. **The island** is the base component of the class most of the content's land army
types travel by (`isle_class`, `IMPAI_REACH.land_w`), not the land constructor's own class: a hover or amphibious
constructor's component spans the sea, and measured by it the sea's blocks and patches read as the island's; with
no such class the constructor's own class measures it. A sea constructor's patches are THE METAL'S REACH counted: those on its class's component at the
base whose extractor of its own menu fits, a ship's component at a dry anchor being the first labelled block within
32 rings (1 024 px). Each plant type is read once whatever builders offer it, and a constructor offered by a land
and a sea plant is measured on each side. Both thresholds are
hand-written, the bar's to move. **Why**: "Air first is often a bad idea and extremely hard to pull off even for a
human. Whether you have a land plant first really depends on the size of the island to know if it's worth it"
(the owner); Gods of War's island holds four patches, which the commander takes, and its land plant cost the
metal of 1.4 destroyers. **Gaps**: the island's area is measured within the window only, so a narrow island
longer than 3 km reads by its part near the base; the order counts patches open now, so an island whose patches
the commander has already taken reads small; a sea plant whose constructor also walks the land (a floating hover
plant's) counts the island's patches as the sea's, which only leans to the sea. A builder whose menu holds the chosen type builds it; another passes the rule, so the plant
waits for a builder of a type that offers it. **An unlock**: when the chosen plant is one only an unlock builds, the
governor's next constructor is that type, at a factory that makes it and at no other factory, once a decision and
until one is a frame (`impai: unlock seat= gametime= cons= plant=`); the governor's rules still bound how
many, so the unlocks are candidates only while the governor has room for a constructor -- below `cons_max`, and
with the commander below the persona's `att_eco` (*The plan and the personas*, the attention), one test the governor's own
constructor order reads too, since an unlock waiting between the two caps would never come --, and at the cap the
best of
the seat's own plants is taken. A type is lacking when the seat has no finished unit of it, by the census's
whole bitset of types. **The unlock is kept** (`IMPAI_BRAIN.unlock`) from one decision that runs the plant choice to
the next, whose answer replaces it: the choice runs only while a plant is wanted, and the wants lapse for a
decision -- an energy stall, the hold, the steady income dipping under `tech_m` --, which left the governor's
constructor type to come and go with them. A decision that does not run the choice keeps the unlock only while it
is still one -- a constructor type on a menu of the seat's factories with no finished unit -- so it ends at the
first finished unit of it. **Gap**: below the cap the governor's other rules can still hold an unlock back -- the
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
the seat's build tech. **In a surplus the factories are paced by what they spend** (`impai_brain.c`, economy, THE
PLANT'S PRICE): with the store at 0.8 of its maximum or more, a plant past `factory_max` is wanted when the metal the
seat makes and does not ask for pays what one of its busy plants asks. **The price is measured**: the build step asks
on the builder's own ledger, the settle moves each pass's request to the ledger's `+0xE8` (metal) and `+0xD0`
(energy), and a request is recorded paid or not ([weapons](../engine/weapons.html), *The ledger*), so a factory's
`+0xE8` is what it spends a pass at its rate; the price is the mean over the factories at their BuildingBuild that
asked metal -- one waiting at its yard or at the unit limit asks none and is left out, and with none asking there is
no price and no surplus plant. **The metal term**: the steady income less the seat's request (`+0xA0`) at least the
price, no factory idle -- in a surplus the governor gives each idle one an order, so one idle at the census is
between products or not yet reached by the persona's budget, and a new plant beside it would spend nothing --, fewer
than 32 factories. **The energy term**: the energy made less the energy asked, plus what the active makers cost
(they are switched off when energy runs short), at least the price's energy; with the metal term alone **energy is
the limit**, and the ladder's energy rung raises its target by the energy the plant is short of, so the energy comes
first and the plant at a later decision. No stall, no hold, one plant at a time. **Every plant past the first waits
on the energy term** -- the surplus plant, the next plant below `factory_max` and the tech-up --, since a plant spends
its price a pass for as long as it makes, and the energy asked is the request, paid or not (the player's `+0x94`);
with no factory asking there is no price and the term is the income at least the ask. On Escalation, whose products
ask about 3 000 energy each (ARMCK 3 045, ARMCV 3 615), the second plant came on metal alone and was built with the store at
0 (MEASURED 2026-11-15, the economy diagnosis at d665a00a). The products are not gated: their ask is in the energy
asked, which the energy rung's target already reads at 1.1 times, and a product that empties the store makes the
energy stall, the ladder's first rung. Every long game against the stock
AI ended with 3 factories (two of tech 1, one of tech 2: one of the highest tech, so the tech-up's own test never
held) spending about 25-36 metal a second of an income of 56-95, the store full, while the stock AI built 12-22
(MEASURED 2026-11-14), and the plan's flat 8 a pass a factory held the fifth plant until the income reached 40. **The tech-up comes first** when both are wanted: the plant choice runs the tech-up's passes, each scored at the
level above the seat's, and the further plant's, scored at the seat's own, only when they find none, so a seat with nothing above its highest tech is not held at its count
by a tech-up no menu offers. **Gap**: the further plant is the plant choice's at or below the seat's highest tech,
by its units' values; with no values the least value, a plant of tech 1 while one is the cheapest.
**The metal's reach** (`impai_econ.c`, THE METAL'S REACH): a constructor type reaches the metal when an open patch
-- free of claims, failures and other players' buildings, on held ground -- has its anchor block on the label its
class carries at the base and an extractor of its own menu fits the patch's ground (a water extractor where the
patch lies deep enough). Past its first finished factory, with no plant frame standing -- a frame's constructors are
not yet its own to judge, and an interrupted frame would read as a seat cut off on any map --, the seat is **short**
while more patches lie open than it has
extractors (built, framed or ordered) and neither a finished constructor of its own -- the commander apart -- nor a
constructor its finished factories make reaches one. While it is short and a plant of its build tech or lower on
its builders' menus makes a constructor that does, that plant is wanted whatever the income (below the factory
lists' 12), the plant choice's further pass takes only such a plant, whichever rule wanted it, and the governor's
constructor is one that reaches the metal before one that does not while no finished constructor does. The want is
the plant choice's, never the sea plant's: a naval seat's yard still waits on its own metal terms, and while it is
not wanted the reaching plant comes. Fed to the sea plant's want, it held a naval Steel Jungle seat, whose only
builder of a shipyard was its busy commander, wanting the yard from GameTime 11 707 to the end, the plant choice
never run (MEASURED 2026-11-16, ca-1, a diagnostic build). The count is
the seat's measure of what it is cut off from: a patch on an island against a base holding the rest is not worth a
plant. **Why**: Steel Jungle's 43 patches are all underwater ore features 64 to 75 below the sea, which the land
classes' content maps reach none of; in round 3's eight Hard games against the stock AI's Hard there ImpAI's
amphibious commander took one or two, its lab's and vehicle plant's constructors reached none, and it held one or two
extractors for the whole game (a metal income of 2.7 to 8.4 a pass at the end) while the stock AI went to sea for
six (MEASURED 2026-11-15, the patches read over the core's load of the map); the hover plant's constructor reaches
all of them. **Gaps**: a water-only plant is a candidate only on a wet anchor, so a seat whose only reaching
constructor comes from a shipyard waits on the sea plant's own rules; the open count is the seat's published
patches, which a map of many small patches inflates; a plant frame no builder resumes -- out of every builder's
reach, under known ground threat or at a help the engine refused (*The assist*) -- holds the reach off while it
stands.
The military's table input `F_TECH` is still its highest factory's tech level over 3, the
input A3's table was tuned on. **Anti-air structures**: one per
`aa_struct_v` of seen enemy air value, at most `aa_struct_max`, on the base ring.
**Ground defences** (`impai_econ.c`, THE GROUND DEFENCES) are reactive only, at every level: while the pressure
stands at the base, one, and one more for each `def_v` of pressing value, at most `def_max`, before an anti-air
structure in the ladder, outside an energy stall -- a metal stall does not hold it back, as the pressure lifts the
hold: a raid that lasts keeps the store low, and the towers it is for never came. A ground defence is a building of category OTHER with a
ground reach -- a missile tower is one, by the engine's rule. The count is every such building of the seat's,
wherever it stands, built, a frame or at a builder's head, each once -- a head at a site that already holds a
building of its type is that building, counted after the census (`impai_brain.c`, THE HEADS' COUNTS), as are the
anti-air structures and the energy a frame will add --, so `def_max` missile towers the anti-air rule built
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

**Sensors** (`impai_econ.c`, THE SENSORS) stand by **coverage, not by count**. Past its sight a seat knows only its
radar and sonar contacts -- its own acquisition's (`tagpu_seatcontact_scan`, *What a seat knows* in [ImpAI in the
engine](../modules/impai.html)) --, which feed the picture, the threat maps (a radar dot is a threat), the lead's
evidence and the hunts. A point is covered when it lies within three quarters of the distance of a building of
the seat's with a sensor of its kind -- built, a frame or at a builder's head, each once; a plant or a tower with a
radar covers as a radar tower does. Once `factory_max` plants are finished -- the plants the pacing asks for come
first: wanted past the first plant, a sensor delayed the second by 960 and 1740 ticks on Red Triangle ac-1 and
Trout Farm ca-1 (MEASURED 2026-11-15) --, outside both stalls and the hold, and while fewer than `sensor_max` (4)
sensor structures stand, the want is the first point not covered of: the anchor; the
seat's finished extractors, nearest the anchor first; the front, the anchor moved half way to the estimate of the
enemy base, at most `base_r`; and, on a map where a sea army class has water, the anchor again for sonar. A tower
lost is wanted again by the same rule. A **sensor structure** is a building of category OTHER with a radar or sonar
distance (def `+0x204`, `+0x206`) and neither a ground reach nor an anti-air rate: a tower with a radar beside its
gun is a defence, a plant with one a plant. The **type** is the menu's of the want's kind with the most distance
squared per value, ties to the lower type, whose energy upkeep the seat's flows (income less spending) pay before
it is built -- once it runs, its upkeep is in the spending the energy rung keeps the income above --, a water-only
type only on a wet anchor; no unit list. The **site** covers the point by construction: a site whose footprint
centre lies past three quarters of the type's distance from the point is never taken -- the base ring's first,
nearest the point, then a ring around the point itself on held ground, which the commander leaves to a
constructor, keeping to the base ring. The first builder that tries the want ends it for the
decision, ordered or not, so a point no site serves does not spend every builder's site tests. A sensor is no
army: the army classes are mobile types', and a sensor stands where it is built. The level-end lines measure
it: `sensors=` on the `eco` line, and on the `mil` line `pass_mean=` (the enemy rows a decision held by a contact
alone) and `base_t=` (the GameTime the estimate of the enemy base first stood on structures the seat knew).
WHY: the brain had no rule for a sensor, and no seat ever built one (MEASURED 2026-11-15, the owner's game of
three Hard seats at 57d39d71: 20 minutes, no radar). MEASURED 2026-11-15 over the retail set that must stay all
wins (19 distinct games: round.sh's fourteen, the five extras and ProTA's two), the same build with and without
the rule: still 21 of 21 wins; sensors ordered 0 against a median of 4; `pass_mean=` 0.05 against 0.79 a decision
(median 0 both: most decisions hold every enemy row by sight); `base_t=` a median of 13 680 both, the base first
seen by a scout before any tower stood, in 13 of the 19 games at the same tick; the games' end a median of 36 240
against 36 540. **The gain is net**: a radar or sonar tower makes the energy it uses, so a gain counted gross puts
retail's eight sensor towers among the energy buildings (impai_tinfo.c, THE GAIN), and none is then a sensor.
**Gaps**: sonar only at the anchor; no mobile sensor (a radar
vehicle or aircraft) and no jammer; the front's point waits for a held site when none is held.

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

**The dry hunt.** Each military decision after the naval judgement asks whether the land army has anything left
to go for. The seat knows a land enemy when one of its enemy rows -- seen now, by radar or sonar, or a ghost -- that
is not an aircraft, or a foe's commander remembered fresh, stands on a block **the land army reaches**: army units
of the land layer holding at least half its value reach the block by their own classes' labels -- the half the
waves ask of a squad, so the hunt and the waves agree whether the land army has anything to go for -- a unit on a
block no label tests reaching everywhere; with no land army, the naval judgement's test, a majority of the
content's land classes' weights at the anchor's block and the enemy's. It then stamps `land_t` with now. **Why the
army**: that majority is the content's, not the seat's, and at two exact blocks; on Trout Farm's west start it
read every enemy as off the land for the whole game while the land waves razed the stock AI's structures
(`bar-hota-troutfarm-aa-1` and `-ac-1`, `land_t` 0 at the level end), and once a land wave's goal stamped the
land, `-ac-1` went dry 6 750 ticks later while its army regrouped at home, turned its factories to the sea and
lost its commander at GameTime 44 786 (MEASURED 2026-11-15). An enemy known on a block the land army does not
reach -- a structure's ghost on the water, a sonar contact, a commander seen on the lake bed -- stamps `sea_t`. `dry` holds
while a land stamp exists and is more than `IMPAI_DRY_T` (5 400 ticks) old, and an off-land stamp is `IMPAI_DRY_T`
old at most: what is left of the enemy, as far as the seat knows, stands where the land army cannot walk -- on the
water, under it, or on another island. **The evidence**: a land army that has met no enemy for `IMPAI_DRY_T` is no
evidence that the enemy is off the land -- on `bar-eota-troutfarm-cc-2` the stock AI rebuilt nine extractors and
two labs unseen --, so the hunt needs an enemy known off the land as well, **or the land looked over**: since the
last land enemy, an army unit of the seat has stood in every sweep cell whose centre the land army reaches. An
enemy whose last plants float in the middle of a lake is seen from no shore: on `bar-hota-troutfarm-aa-1` at
bcd7fa05 the land stamp was 42 360, the waves swept the whole land every 9 000 ticks or so, and nothing off the
land was seen until 66 150 (MEASURED 2026-11-15). **Gaps**: a cell is a sixteenth of the map's side and a unit
standing in it does not see all of it, so a remnant in a corner of a cell stood in reads as none; a cell's stamp is
also made when a wave gives up a goal in it (*the goal's progress*), so a cell the army could not reach counts as
looked over; and the reach is tested at the cell's centre, so a cell whose centre is water is never required even
where a strip of its shore is land. **The end needs an enemy seen now**:
while dry, only an identified row the seat holds now -- in sight, shared, or the radar or sonar pass -- stamps a land
enemy. A contact never identified does not, so one blip on the shore does not end the hunt, and neither does a
ghost or a foe's memory, which say where an enemy stood when the hunt began without it; before the hunt any row,
fresh foe memory or contact delays it. Never before the
first land stamp (the opening is not a dry hunt), in the lead or not; never on a level with no sea block. A land
stamp of 0 later in the game means every enemy the seat has known stood where its land army does not reach, where
the dry menus' water weapons would hit nothing ashore -- the naval judgement's map. Over the 14 verification games
at d665a00a, sampled every 300 ticks, no seat on a level with a sea held the lead with a land stamp of 0; the one
seat that held it with none, on Lava Mania (ProTA), has no sea and is never dry (MEASURED 2026-11-15). **A goal given
up stamps nothing**: an enemy row or a foe's commander within 256 px of a goal the land layer gave up less than
16 200 ticks ago (*The goal's progress*) is one the land army has shown it cannot reach, so it neither keeps the
land stamp fresh nor is evidence of an enemy off the land -- a structure's ghost stays until the seat sees it gone,
and one ashore that no wave can come to would hold the seat out of the finish. The ring holds such a ghost out
only for those 16 200 ticks, after which it stamped the land again and ended the hunt -- dry two thirds of each
such cycle --, so once dry a ghost stamps nothing at all (the end needs an
enemy seen now). **Under the sea, a water
weapon**: a row under the sea -- held by the seat's sonar alone (no line of sight shows a unit whose top is below
the sea, and no radar finds it), or of a type the fight content holds submerged -- is the land's only when the land
units with a water weapon hold half the land army's value; with no land army it is off the land. Hover units are
of the land layer and their class crosses the water, so a hover army "reached" a commander on the lake bed by its
labels and kept the land stamp fresh while nothing of it could hit the commander. A block off the grid is the
land's -- no evidence either way --, and each unit's value counts as data (`value_of`). **Gaps**: a shore target
the land army could shell from its own component reads as off the land; past 16 (class, component) pairs a land
unit counts as not reaching; a ship on the water that the seat's sonar alone holds reads as under the sea. `impai: dry seat= gametime= dry=
land_t= sea_t=` logs each turn. **The finish at sea** follows from it:

- the economy wants its sea plant whatever its pacing, and whatever its factory count below `IMPAI_IDLE_CAP` (32,
  the census's factory lists, past which a factory would be in neither) -- one plant, the stalls, the hold and the
  refusals as below --, and its search reaches `FAR_PATCH_K` x `comm_r` (2 048 px) of the anchor, a site past
  `comm_r` only on held ground;
- the commander may build that one plant as far, and the leash allows it as far (`FAR_PATCH_K` x `comm_r` + 512)
  while the hunt lasts, as before the first factory; every builder is reach-tested (*Built*, below), so the plant
  off a shallow shore is the commander's, whose class walks the sea floor; **a site no builder of the decision takes**
  -- out of every one's reach, or refused the commander for the threat near it -- is passed by the search's cursor,
  so the next decision takes the next candidate (THE SITE NO BUILDER TAKES): on `bar-hota-troutfarm-aa-1` at
  bcd7fa05 the search found the same ARMSY site at every decision from GameTime 66 700 to the cap, three refusals
  counted and no frame ordered, while the stock AI's last plants floated in the lake (MEASURED 2026-11-15);
- the governor buys only types with a water weapon -- the weapons that hit a unit under the sea --, so a land plant
  stands idle and the rows are left to the units that can finish, 64 of them kept for those units whatever the land
  army holds (*The finish's rows*), **while the finish has a plant to
  make them**: a water plant stands or is a frame, or a builder offers one, the refusals are not spent and the
  search has not found the place empty. With none to be had the factories buy as usual, so a seat whose sea plant
  cannot be placed is never left with no army;
- **the frame resumed**: the unfinished frame of a water plant is the sea plant's want while the hunt lasts -- it
  counts as the plant, so no second one is wanted -- and the ladder answers it at the want's rung, the nearest
  builder that reaches it helping it (resolver mode 8, *The assist*'s tests), the commander within the plant's
  radius as for the order that began it. A contact within the commander's safe distance ends its build, and no
  land constructor reaches the water off Trout Farm's west shore, so without it the frame would stand for good
  (`impai: frame seat= gametime= helps= commander=` logs each decision that helps it again);
- a wave of any layer but land goes at 1 + that layer's waves median units, still predicted to win, and outside
  the attention (*The attention*, below): the land army keeps its operations sweeping for an enemy it has not seen
  on land. **The finish's end**: when the hunt ends, while the seat holds more operations than its `att_mil`, the
  least valuable ATTACK of a layer but land goes back to FORM and gives its wave back when it counted, so the land
  army has its room again and no layer's next threshold keeps a wave that never ended. Hard, with no `att_mil`, has no room to
  give, and a seat within its `att_mil` none to take: their waves go on.

**Why**: on Trout Farm the stock AI's last plants floated in the lake and its commander walked on the lake bed,
seen only by sonar and hit only by water weapons, and the seat's 481 land units swept the shore to the game's cap
(`bar-nota-troutfarm-aa-2` at a237d62a: 541 units against 19 at GameTime 81 000). The west start's nearest shipyard
site lies 1 118 px from the anchor, past `comm_r`, and 225 px from the nearest cell a vehicle stands on, past
ARMCV's reach of 163 (MEASURED 2026-11-15 on the map's class maps); with the plant a constructor's, three games'
constructors walked to it four to six times each and were abandoned short of it, and no plant stood. One ARMSY then
makes an ARMSUB each 3 300 ticks or so, so eight median units took past 26 000 ticks, and Easy's two operations
were held by two land sweeps. With a sweep ended to make room instead, the stock AI rebuilt nine extractors and
two labs on land unseen and `bar-eota-troutfarm-cc-2` reached its cap, where with both sweeps kept it ended at
65 176: a dry hunt is the seat's knowledge, and the enemy can be back on land before the seat sees it. **Gaps**: an enemy beyond both the land and the sea army's
reach -- an island no water body of the seat's sea class touches -- is not finished, since no air finish exists; a
water-weapon type with no sonar of its own sees nothing under the sea; the estimate's water decides the sea plant's
water, so a remnant in another water body waits for the estimate to move; the finish's operations outside the
attention let a seat run up to two more operations than its `att_mil` while the hunt lasts; a contact never
identified carries no type, so an aircraft only the radar sees over the water reads as an enemy off the land and
can start a hunt the remnants on land do not justify -- the factories then buy for the sea until the land army
identifies one of them.

**The island** (`impai_mil.c`, THE ISLAND), each decision after the dry hunt: on a naval map, a level with labels,
a land army type and a sea block, the seat's land army reaches nothing that can hold an enemy -- no enemy row but
an aircraft stands on a block it reaches (*the land army's reach* above, the content's land classes before the
seat has a land army), and no foe's start has a block it reaches within `IMPAI_REACH_RINGS` (8) rings, the
economy's reach of a target, so a start on a cliff reads by the ground round it -- and at least one such start or
row is known. Once enemy structures are known, or were, a start the level seated at random is no longer evidence:
its occupant is unknown, and the rows say where the enemy stands; at random starts a second start on the seat's
own land otherwise kept the island off for the whole game. A land army that crosses the water ends it. While it holds, a wave of any layer but land goes at the finish's size, 1 + that layer's waves
median units, still predicted to win, but inside the attention, since no land operation competes for it there. The
economy reads the same evidence its own way for the first plant (*The island's first plant*, above), and the
military's verdict, kept in `IMPAI_MIL.island`, for the plants past it and the land army's budget (*The
candidates*, *The island's land budget*, above). **Why**: on
an island map the land army has nothing it can reach, and the 8 median units a sea wave asks never came: on Gods
of War the seat held a submarine and a destroyer at the cap and attacked nothing (MEASURED 2026-11-17,
`exploit-naval-turtle` at 5e4e9c24). **Gaps**: a row on a block the land army's classes do not stand on -- a
cliff, a feature -- reads as off the land, and with every start read so too a linked map reads as an island; the
waves still need the prediction. At random starts with more than one enemy, once one enemy's structures are known,
a second enemy's base on the seat's own land that no row shows yet reads as none.

**The lead.** Each military decision after the naval judgement and before the dry hunt asks whether the seat is far
ahead: its live value -- each finished unit of its own, its type's value -- against the enemy's as the seat
remembers it, three sums: **the structures**, every enemy row that names a type not mobile, seen now, by radar or
sonar, or a ghost at its last block; **the mobile memory**, the enemy's mobile value -- every row that names a mobile
type, seen or a ghost, and each contact held now and never identified, at the substitute's value -- at its largest
lately, decaying to nothing over 5 400 ticks (`IMPAI_DRY_T`) and replaced by a decision's own sum whenever that is
larger; and **the commander's floor, a foe at a time**, the content's largest commander value for each foe the rows
name that has no commander row and whose commander the seat has never seen, and one while the rows name no foe;
and **the base's floor, a foe at a time**: each foe the rows name, or one while they name none, counts at least
the seat's own structures' value times the share of the map's sweep cells that no army unit of the seat has stood
in within 5 400 ticks. Each foe is taken to have built the base the seat has, somewhere the seat's army has not
been lately. **Why the floor**: on Escalation and Total Mayhem, whose stock AIs build faster than the seat, the
lead began in all eight games of the broad check at 1a9350ea at GameTime 9 180-14 857 on the commander's floor and
one to three structures, and ended 135-7 290 ticks later (MEASURED 2026-11-15); the seat's own base at the same
GameTime on the same content is a yardstick no content's number has to tune. **Why the looking**: unbounded, the
floor tied the lead to the seat's own base -- against one foe it began only while the seat's structures were at
most 40 % of its value -- and a grown base ruled it out late in a game, when the army has fought over the map.
**Why the whole map**: the ground round the structures known is where
the seat's raids go, and a base not found is anywhere else. Held to that ground, on `bar-nota-lavamania-ca-2`
(Escalation) the raiders stood in 7 of the 16 cells round two outlying extractors, the floor fell to a third and
the lead began at GameTime 14 857 at 4 987 against 1 833, the seat having stood in 50 of the map's 256 cells; on
`bar-hota-redtriangle-ac-2` the army had stood in 91 of 256 at 42 000, which holds the floor to 0.64 of the seat's
base and keeps the lead that game began at 41 527 with 61 744 against 8 349 (MEASURED 2026-11-15, the recordings'
`s_mil` and `s_known`). **How near the truth**: over the fourteen games of the round at 8c7b10a4, sampled every
3 000 ticks from GameTime 9 000 once a foe was known, the enemy's true value over the seat's estimate had a median
of 1.04 and a 90th percentile of 7.2 with the floor, against 1.16 and 62 without it (79 samples); on ProTA's
`bar-nota-pincushion-ac-2` (the broad check at 8c7b10a4) the seat's ratio stayed at most 2.46 and the true one at
most 2.16, so the lead never began, as the truth says, where the builds before the floor began it at 2.8-4.1 on an
enemy they underestimated (MEASURED 2026-11-15, the recordings' units, `s_mil` and `s_known`). **Gaps**: a cell the army cannot stand in -- sea for a land army, lava -- is never looked
over, so on a map mostly out of its reach the floor keeps that share; a cell given up as a goal counts as looked
over (*the goal's progress*).
The lead begins at 2.5 times the enemy's and ends below 2 times, **on the evidence alone**: it begins only while the
estimate rests on enemy structures seen or remembered, holds while it rests on them or on their centre kept after
they went (`impai_est_known`), and either only while no prediction of the seat's was lost within 5 400 ticks -- a
squad's retreat or the home losing stamps `lost_t` --, so a lost prediction ends it at the next decision. A kept
centre holds a lead because the lead's own waves raze the last structures they know: ended there, the hunting
ATTACKs went back to FORM at the lead's end and the next structure seen began it again. While it
holds the army converges on what the enemy has left: no squad's operation counts against the attention, a wave of
any layer goes at its layer's base size (`wave_value`, or 1 for the finish's) with the prediction still deciding and
adds nothing to its layer's count, and **the home keeps a floor**: of the FORM squad's members in the anchor box, a
wave leaves what the home lacks of the enemy's **raid memory** besides them -- the same memory of the enemy's mobile
value less its commanders' rows, which the ratio counts and the floor does not, as the home's value leaves the seat's
own commander out -- the members past it go to a free squad, which takes the wave, and none go when what would go is
worth less than the base. Each wave that leaves takes its members in the box off the home's value, so a second
layer's wave in the same decision reads the home the first left. **The lead's end**: in the decision the lead ends, while the
seat holds more operations than its `att_mil`, the least valuable ATTACK of any layer goes back to FORM, as at the
finish's end, each giving its wave back when it counted -- the waves the lead started gave the count nothing and
take nothing back. `impai: lead seat= t= lead=
own= enemy=` logs each change and `lead_t=` on the `mil` line the GameTime it first held.

**Why 2.5, and not the bar's 1.3**: the bar scores a game at its cap as the leader's at 1.3 times the other's live
value ([the bar](bar.html), `by_value`), but the seat does not know the enemy's units it has never seen, so its
ratio overstates the true one -- on twelve recordings of the broad check at bb11df10, sampled every 900 ticks past
9 000, by 1.18 at the median, 1.86 at the 90th percentile and 2.36 at most (MEASURED 2026-11-15, each recording's
`s_known` rows against its units). At 2.5 in and 2 out the lead held only where the true ratio was 1.31 or more, in
each of the three games that reached the cap -- `bar-eota-redtriangle-ac-2` from GameTime 64 800,
`bar-hota-troutfarm-aa-1` from 61 200, `bar-nota-plainspasses-ca-2` from 46 800 -- and in none of the nine the seat
won before it; at 1.3 it held in ten of the twelve, where the true ratio was below 1 in five. The commander's value,
30 423 at retail, is most of what a beaten enemy has left, so the floor holds the lead off through the opening, when
the seat knows nothing of the enemy. **Why the lead**: those three games reached the cap with the seat holding 415,
483 and 589 units against 38, 36 and 76 -- on Red Triangle Easy's two operations were held from GameTime 25 000 to
the cap by two sea ATTACKs sweeping for an enemy they could not reach, while a land FORM squad of 255 units worth
49 000 stood at home and the enemy's last base, its commander in it, stood on land; on Trout Farm the dry hunt
never began (above); on Plains Passes a goal never reached held the operations (*the goal's progress*, step 4).
**Why the evidence**: the ratio is only as good as the rows. On `bar-nota-lavamania-ca-1` (ProTA, the broad check at
d665a00a) the seat began the lead at GameTime 8 235 at 2 352 against 914 -- the enemy's commander alone, ProTA's
commander being worth 914 and the only enemy the seat had seen, its row standing in for the floor --, its rows held
about a third of the enemy's structures for the rest of the game (4 886 against 14 630 at 45 000), the true ratio
stayed between 0.61 and 1.19 past 6 000, and the lead went in and out nine times; each lead fed the army to the enemy
a unit at a time, 66 retreat decisions against 22 without the lead at bb11df10, and the game reached the cap at 1.13,
where bb11df10 had won it at 50 384. In the three caps at bb11df10 whose true lead the ratio found, the seat made one
retreat decision after the lead would have begun (MEASURED 2026-11-15, the logs' `mil` lines and the recordings'
rows against their units). **Why the memory**: a mobile ghost lasts only until the seat's sight covers its last
block, so a raid that withdraws through lit ground is forgotten at once, and radar contacts come and go; counted as
they stand, they moved the ratio across the 2-2.5 band and the lead in and out of the finish's end. **Why the base
and the floor**: the attention keeps nothing home in the lead, so each wave took the whole FORM squad, at one median
unit a unit as it was made, and the recall then brought home one unit while `coming` held the layer's others out.
**Gaps**: a foe the rows name nothing of counts no floor once another foe is named; a commander seen and lost from
sight on lit ground counts as gone; a ghost of a unit destroyed out of sight still counts, so the seat errs towards
the enemy; with no free squad the lead's wave waits, and the members kept home are the box's first in the army's
order, not its strongest; the margin was measured on twelve games of one opponent, the stock AI, and the evidence
on four.

**The sea plant** is the seat's next factory after its first, or the finish's in a dry hunt (above). The second
factory waits for the judgement (`naval >= 0`), an ordering: the two managers' order cannot decide which kind of
plant takes the slot.

- *Wanted* when naval, no water factory stands, is a frame or is at a builder's head, a factory stands, nothing
  stalls, the seat's refusals are below `IMPAI_SEA_REFUSALS` (16) and some builder type of the census offers one
  (`impai_econ_sea_able`, at most 8 types' menus), on **the land plant's metal**: `factory_per_m`'s pacing inside
  `factory_max`, or the surplus that pays a further plant's price (*In a surplus the factories are paced by what
  they spend*) -- so whatever lets a land plant come lets the sea plant come first, but for *the reaching plant*
  (*The metal's reach*), which a seat cut off from the metal is given whatever the judgement and the income; it
  counts against `factory_max` as any plant, so the yard after it comes on the surplus. A plant begun on the
  commander's own income empties the store the economy runs on ([decisions](decisions.html), *The naval rule's two
  design calls*); with the pacing alone, a land plant the surplus paid for took the slot on Red Triangle and Trout
  Farm, judged naval, with no sea search (b5 at 57d39d71, the second plant at a steady 6.59 and 6.53 a pass
  against the pacing's 8.00, MEASURED 2026-11-15).
- *Chosen*: the least value among the menu's water factories whose own menu offers a sea army type.
- *Placed* by its own search, once a decision, its answer shared by the decision's builders: centre blocks in ring
  order around the anchor within `comm_r` (`FAR_PATCH_K` x `comm_r` in a dry hunt), whose centre carries one of the sea class's labels within 48 blocks of
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
  against the type, the anchor, the estimate's block and the search's radius, the only things besides the content
  it depends on.
- *Built* by a builder that can reach the site (THE BUILDER'S REACH): it stands within MobileBuild's reach of the
  footprint (builddistance plus both footprints, the engine's own abandon compare), or a block of its class's
  component at its own block lies that near wherever on it the builder stands. A block's entries put a unit's
  centre anywhere in a 32-px square offset by its footprint (the set-position 0x48A9F0 rounds the cell to within
  8 px), so the block counts only when that square's farthest corner is within the limit. One that cannot leaves
  the site to the decision's other builders and counts the decision's refusal, as the commander's safe distance
  does. A block's centre alone admits sites no place on the block reaches: on Trout Farm's south start
  (bar-hota-troutfarm-aa-1) it put an ARMSY site 152 px from a vehicle block's centre against a limit of 163, where
  the vehicle stands 170 px off at best, and twelve land constructors' jobs walked to the shore and were abandoned
  while the commander, whose class stands 11 px from the site, stood idle.
- *Termination*: a decision whose search places nothing counts one refusal, whatever stopped it -- a refusal by the
  site test or the resolver, a builder's reach, the claims, the exploration, the fail ring, the decision's budgets
  -- and so does a
  failed water-factory job; the count advances at most once a decision and once a `fail_t`, so a cause that passes
  (a unit on the footprint, water not yet seen) costs one, not sixteen. At 16 the sea plant is given up, so the want
  ends in a sea plant, a land factory in the same decision (no candidate) or a land factory at most 16 x `fail_t`
  ticks after the first count (48 minutes at Hard's 5400), the seat holding one factory meanwhile.

**Keeping to the layer.** Every military move goes through one function, which keeps the point when the member's
class reaches it and otherwise moves it to the nearest block of the member's own component within 32 blocks (a
bounded spiral, 32 a decision, cached by class, component and point), or gives no order (`unreached=`). A FORM or
RETREAT member with no such block for the rally point -- a ship whose rally is ashore -- goes to its **home point**,
the block of its own component nearest the rally point within 192 blocks (6 144 px; a bounded spiral, 4 a decision,
cached by class and component), spread as any travel; with none it is **stopped** (the engine's Stop leaves one
Standby and the unit where it stands, [ui](../engine/ui.html), *STOP and ATTACK*), unless it holds no task -- no
order, the default order the engine drops, or a factory's lone Park, which walks it off the yard --, so no member
sent home keeps an attack at the force it lost to; a member the caches cannot answer for is given nothing that
decision (`unreached=`), never stopped. The retreat home and the release measure a member against the same point.
A wave goes
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
with it; no advanced sea plant on tech-up, so an island seat whose plants are all sea plants never techs up; a seat the island misjudges keeps or caps its land army by the misjudgement, and the table's
shares are not reweighted by reach (A4); mixed-layer predictions have no reference kind; the labels and
tops are the level init's, so features destroyed or created later, and a saved game's, are not in them; the sea plant
is at the earliest the second factory and with `factory_max` 1 there is none outside a dry hunt; any water factory counts as the sea
plant, even one whose menu has no sea army type; the builder's reach proves a place within the limit, not that the
engine's walk ends there, so a build may still be abandoned (its failure counted); it is read on the block's whole
square, so a block whose entries alone are within the limit is refused; a builder whose builddistance passes 512 px
is tested on the blocks within 512 px; a
seat whose military refuses every decision keeps one factory (the mil line's `refused=`); sea constructors build no
tidal generator on a dry anchor; before any structure is seen the estimate is the nearest start that can hold an
enemy's base -- not the seat's own, not left empty, not an ally's where the level seated its starts -- so with
random starts any start;
a FORM or RETREAT member with no block of its component within 192 blocks of the rally point is stopped where it
stands (`unreached=`), and at the roam move state its Standby may chase a target in its reach until the next
decision stops it again; a
naval judgement that comes late -- the estimate moving across water once structures are seen, as on Lake Shore --
finds both `factory_max` slots land plants and gets no sea plant until a dry hunt, since none fits inside
`factory_max` without taking a slot a land plant already holds; a sea plant search that keeps failing holds the seat at one factory for
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
| `aa_struct_max`, `aa_struct_v` | 6, 1000 | `rally_px` | 384 |
| `attack_margin`, `retreat_margin` | 0.10, −0.05 | `wave_value` (its layer's median units) | 8 |
| `scout_period`, `task_timeout` (ticks) | 1800, 2700 | `dgun_reserve` (share of energy storage) | 0.25 |
| `cons_open`, `cons_max` | 1, 12 | `army_per_cons` | 2 |
| `press_k`, `press_hold` (ticks) | 1.5, 900 | `stall_pm` (thousandths of the storage), `hold_fac` | 200, 1 |
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
  of them (THE SPLIT) --, and **a decision of one command whose idle factories want it goes to the factories and
  the builders in turn** (THE FACTORIES' TURN, `IMPAI_BRAIN.root_turn`): on the factories' turn the governor runs
  before the builders' assignment and the builders are given the command it leaves; on the builders' turn the
  assignment runs first and the factories take what it leaves; and every such decision passes the turn, whether
  or not the factories spent the command. So the turns alternate whatever the bucket's rate or burst, no command
  waits on a factory the governor leaves unordered, and what the
  factories' turn holds -- a job's stop, an arrived walk's reclaim, the yard's step, the makers -- waits one such
  decision at most: a turn that stayed with the factories until they spent a command held the yard for as long as
  an idle factory stood unordered. A turn back out of danger is
  never held by the turn. Unused actions stay in the bucket up to its burst, so a seat whose
  builders spent each action as it came never reached a decision of two, and its factories stood idle beside full
  stores -- 26 % of the samples on TA Zero and 14-18 % on TA Twilight at Normal (MEASURED 2026-11-15, the economy
  diagnosis at d665a00a). Its cost: on the factories' turn a stop, the yard's step and the makers wait for the
  next decision of one command. The military's is a squad served, its members' orders included, or a commander order
  (a flight, the leash, a D-gun). The squads never spend the bucket's last action, so the next decision's
  commander always has one, whatever the squads did (the reserve held below the burst, so a burst of 1 keeps
  none); with nothing left to spend
  the military's round robin stops at the squad, which the next decision serves first.
- **The attention.** `att_mil` holds the operations at once -- squads holding RAID or ATTACK --, and a squad
  that would start another is refused (THE ATTENTION, `become`); DEFEND is exempt, so a seat answers a threat to its
  base whatever it has out (the owner's decision, 2026-11-14), and so is a squad of a layer but land in a dry hunt
  (*The dry hunt*), which neither needs room nor takes any and goes back to FORM when the hunt ends, and every squad
  while the seat is in the lead (*The lead*), the room given back when it ends; `att_eco` holds the builders busy at once -- the
  constructors and the commander's economy jobs, the census's busy ones and the idle ones a decision makes busy --,
  and the governor keeps no more builders than that, its constructors and the commander. A helper the helpers'
  pass gives another task is one busy builder still: it needs no room and takes none.
- **The commander between two managers.** Each manager's order to the commander leaves a marker, the decision's
  GameTime + `react` (`IMPAI_MIL.com_until`, `IMPAI_BRAIN.com_until`): the economy treats the commander as the
  military's while the military's lasts, and the military does not drop its order against a head the economy's
  pending order will replace. At react 0 neither is ever set, and a marker counts only within one reaction of
  now, so a recorded one past it holds nothing.
- **A pending order is data.** An entry restored from a recording reaches the engine only through the WORLDQ's
  order check, which refuses an action byte past the engine's action table, a type past the defs and a point off
  the map (`wq_cmd_formed`), and every entry is bound to its actor's incarnation and, when it names one, its
  target's -- a radar-only target's included, whose birth the entry keeps and no decision reads.

The persona lever `tagpu_impai_persona.on` gives a seat a persona in place of its level's row, for the
calibration; `tools/impai/level_lever.py --persona` writes it. Each seat's init logs `impai: persona seat= level=
look= react= apm= att= from=level|lever`, and a limited seat's pulse an `impai: react` line ([ImpAI in the
engine](../modules/impai.html), *The persona*).
