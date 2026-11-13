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

## What it has to beat

What the stock AI does, from the engine map ([orders](../engine/orders.html), *The stock AI's helpers*; DISASSEMBLED, not measured):

- **It builds by a weighted random pick.** Each type on a builder's list gets a score from storage, income and per-type ratings scaled by the profile's weights, and one is drawn in proportion to its score. It builds nothing while stored energy is below 50 or metal below 25.
- **It builds on unseen ground.** Sites are random points on a per-seat lattice around its base, tested with no sight or explored-map check; its metal spots are every metal cell on the map.
- **It attacks with six or more armed units, every 300 ticks, at the enemy unit nearest its force**, chosen from every enemy on the map: its target picker has no sight, radar or explored-map test. Three or fewer regroup at the base.
- **Its commander stops building at five builders** (the seat's complete units with a build list, factories included) and roams the base instead; a hit commander drops its orders and pauses for 30 to 329 ticks.
- **Its weights and limits change with the Difficulty** through the profile's `plan` sections, and on Easy and Medium the engine cuts its resources to 50 % and 70 %.

## The military

As built (`impai_mil.c`; its engine glue: [ImpAI in the engine](../modules/impai.html), *The military*). One
decision a seat every `mil_period` ticks, on a tick the economy never takes:

1. **The army**: the seat's finished mobile units with an army class (A3's thirteen, by layer and role), at most
   512; each in one squad, its job `IMPAI_JOB_SQUAD` naming it. A unit with none joins its layer's FORM squad.
2. **The picture**, from the seat's own column only: enemy rows seen now (own sight, an ally's, the radar pass),
   and structures also as ghosts; a radar contact never identified counts as the median identified enemy mobile
   type. It fills A3's twelve features (`impai_picture_features`) and the enemy's mobile composition the type
   choice weighs, and the enemy base's estimate: the value-weighted centre of the enemy structures known, else
   the start position nearest the seat's own that is not it (the starts are public). **Contacts**: the enemies
   seen now, joined across 256-px cells within 768 px, seeded nearest the anchor first, at most 32.
3. **Defence**: a contact within `base_r` + 512 of the anchor or 512 of an own structure threatens the base; after
   `react_confirm` decisions of it each layer's FORM squad becomes DEFEND, sent to the nearest threat.
4. **Tasks**, each squad round-robin: a contact within its longest range + 384 is predicted (the fight predictor,
   noised by the level, at most `predict_max` a decision) -- below `retreat_margin` the squad goes back (RETREAT)
   to the rally point, `rally_px` from the anchor towards the estimate; otherwise it fights. With no contact a
   FORM squad worth `wave_value` + waves sent median army units attacks the enemy base's most valuable known
   structure when the prediction against its defences passes `attack_margin`; a RAID of three to six of a FORM
   squad's fastest units (scouts aside) goes for the known enemy extractor or energy building of least ground
   threat, up to `raids` at once; one SCOUT walks three start positions in turn, every `scout_period` or while the
   estimate rests on no structure. A task ends at its timeout, when its target is no longer known, or on
   arrival. **A refused prediction changes no task.**
5. **Orders**, at most `mil_roots` a decision: a fighting squad attacks only units the seat sees now, and only
   those its own observation of the same turn still finds as the unit its row is about -- a row is as fresh as
   the slice's last visit of its slot, up to 8 ticks at the raised limit, so the slot may hold another unit by
   then: a row seen in sight must name the type and owner the observation shows, any other the incarnation the
   seat's radar recorded -- with
   `focus`, the one its members kill quickest (type health over the summed pair rates), else each member its
   nearest. A member attacks only a unit the order resolver will answer it for -- its tests of the unit's
   canattack, weapon, weapon 1's toairweapon and dropped, its waterweapons, canhover and canfly, and of the
   target's airborne and under-the-sea state ([orders](../engine/orders.html), *What each resolver mode
   answers*) -- read from the type's facts at the content build, the unit's flag word and the target's state
   that turn, where the seat sees it; a target known by radar alone shows no such state, so there the resolver
   decides and a refusal costs no order. One with nothing it can attack, an anti-air unit in a ground fight,
   keeps with the squad: it moves to the centroid when it stands farther than 64 px from it, and is given nothing
   nearer. The attacks and the steps back are ordered before those moves, so the decision's orders go to the
   fight first. A member under `micro_hp` of its health steps 400 px back, once in `micro_period`, and is left on its
   way between steps. A travelling squad moves to a 5 × 5 grid of points 64 px apart; a move toward a unit passes
   no target (a move with an enemy target is a reclaim or a capture, [orders](../engine/orders.html)). No order
   repeats what the unit's head already says.
6. **The commander's safety**, before the squads' orders: a contact worth more than the commander within 600 px
   of it is predicted against the commander and the army within 512 px; a margin below 0, or no prediction, moves
   it to the farther of the rally point and the own structure farthest from the contact, and makes it the
   military's for 300 ticks, a window the economy's census honours.
7. **The D-gun**, then, when the commander is not fleeing: the seat's weapon scan leaves CommandFire weapons to
   orders, so the commander D-guns (resolver mode 4, `ATTACKSPECIAL`) the most valuable enemy the seat sees now,
   and still finds that turn, within its D-gun's range, only while stored energy covers the shot plus `dgun_reserve` of its energy storage
   and only a target worth the shot's energy in metal. A type's D-gun is its longest CommandFire weapon when its
   def can take the order (`candgun`), from the content build. The three conditions are tested when the order is
   given, not at each shot: an order already given holds its target until it ends, and the engine fires each
   shot only while the seat holds the shot's energy (AutoAim, `0x49E3F3..0x49E40C`, [keys](../engine/keys.html)),
   so the reserve is not held between two shots at one target.

**The factories buy army** in the economy's decision, after the constructors: each idle factory left queues one
unit, the class from A3's table (`impai_table_shares` over the picture, `impai_buy_next` against the army's value
by class, built, building and queued) and the type by `impai_class_type` with the predictor's pair rate.
**Tech-up**: with `factory_max` factories of its highest tech and a metal income of the table's `tech_m` times
the level's `tech_scale` a pass, the next factory is of a higher tech. **Anti-air structures**: one per
`aa_struct_v` of seen enemy air value, at most `aa_struct_max`, on the base ring.

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
  fail ring, and the site test decides. No candidate at all ends the want in the same decision, and the factory is
  a land one; that answer is remembered against the type, the anchor and the estimate's block, the only things
  besides the content it depends on.
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

## Levels

A level is one row of `IMPAI_LEVELS` for this same brain ([ImpAI for the player](player.html)); Hard's economy is
A1b's:

| | Easy | Normal | Hard |
|---|---|---|---|
| economy `decide` (ticks) | 30 | 15 | 15 |
| `mil_period` (ticks) | 150 | 60 | 30 |
| `mil_roots` | 6 | 16 | 40 |
| `react_confirm` (decisions) | 2 | 1 | 1 |
| `predict_max` | 4 | 8 | 12 |
| `focus` | no | yes | yes |
| `raids` | 0 | 1 | 2 |
| `aa_struct_max` | 2 | 4 | 6 |
| `micro_hp`, `micro_period` | none | none | 0.35, 90 |
| `rally_px` | 384 | 384 | 384 |
| `noise` (margins and ratios) | 0.25 | 0.10 | 0 |
| `attack_margin`, `retreat_margin` | 0.30, −0.30 | 0.15, −0.15 | 0.10, −0.05 |
| `wave_value` (median units) | 6 | 8 | 8 |
| `scout_period`, `task_timeout` (ticks) | 5400, 5400 | 2700, 3600 | 1800, 2700 |
| `tech_scale` (× the table's `tech_m`) | 1.5 | 1.25 | 1.0 |
| `aa_struct_v` (seen air value) | 2000 | 1500 | 1000 |
| `dgun_reserve` (share of energy storage) | 0.75 | 0.50 | 0.25 |

**`tech_m` lives in two places, and the table's rules**: A3's table carries `tech_m` as Hard's, tuned with the
rest of it, and a level row carries only its factor `tech_scale`; the threshold a seat plays is the product.

**The noise** is stateless: `impai_noise(seed, GameTime, tag, i)`, a splitmix64 finaliser of the seed recorded at
the level init, in [−1, 1); tag 1 the squads' margins, tag 2 the economy's three ratios. It never draws from the
simulation's random stream, so a replay reproduces it from the recorded seed.
