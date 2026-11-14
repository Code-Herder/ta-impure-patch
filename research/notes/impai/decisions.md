# Decisions

Every decision of the 2026-11-11 design interview, each with the alternative it was chosen over and why. When a later question asks "did we consider Y?", the answer should be here.

## ImpAI as the player sees it

- **ImpAI sits beside the stock AI**, over replacing it: a computer player's AI button switches **ImpAI / OTA AI**, so engine games can always pit one against the other.
- **OTA AI stays the default until A2**, then ImpAI. This was chosen over flipping at A1, so that a published A1 never ships an AI that cannot fight yet.
- **No cheating in either direction**, over BARb's optional global vision and over stock TA's income handicap. `WORLDQ` filters an ImpAI seat's view to what the seat sees, by construction.
- **ImpAI seats are exempt from the engine's Difficulty scaling**, over letting Easy play at half income. This is a simulation change ([entering the game](engine-seam.html)).
- **Three levels on the global Difficulty gadget**, over per-seat levels on the AI button, because that is the screen players already know.
- **Levels change behaviour only**, over resource handicaps.
- **One brain, a level is a row of parameters**, over separate easy code paths.
- **No memory between games**, over an opening bandit that learns across games, so every game is reproducible.
- **Allied ImpAI seats are separate brains** sharing only what the game shares, over a private coordination channel.
- **Saved games rebuild ImpAI's view on load**, over extending the save format. Rewind and takeover carry ImpAI's `SIM_STATE` through the recorder.
- **Skirmish first**, network games last (A4).
- **One tuning table for every mod**, on derived roles, over a table per mod.
- **At most 1 ms per ImpAI seat per tick**, worst case, bounded by counts and never by a clock.
- **Not now:** the threat overlay is a `tacli` test lever, never a player setting. A wiki entry for ImpAI is decided at A2.

## Settled at L0

Defaults set once the engine research (L0) was in ([ImpAI for the player](player.html), [entering the game](engine-seam.html)):

- **An ImpAI seat keeps the stock weapon scan** `0x4089A0(obj, 1)`, so CommandFire weapons fire on their own as they do for OTA AI, over making ImpAI fire them by order only. **Reversed by the owner on 2026-11-12** (below).
- **ImpAI owns its units' standing states**: the stock group assignment `0x408830` does not run on an ImpAI seat, over keeping it for parity. It would force fire at will and write the unit's group number, the field a human's Ctrl groups use.
- **The `AI` chat command still starts the stock AI**, over choosing per seat which AI it starts.
- **OTA AI keeps its stock behaviour**, including a target picker that sees every enemy unit. The asymmetry with ImpAI's vision rule is a fairness choice, not a defect.

## The bar

- **100/100 at every level against OTA AI on the same Difficulty**, over the first proposal of at least 70 % against OTA Hard. The owner judges OTA AI too weak to lose to, so any loss is a defect, not noise. This is to be re-evaluated once results exist. **Replaced by the owner on 2026-11-14** with a target a level against OTA AI at Hard (*The levels are personas*, below).
- **A 90 % ladder between adjacent levels**, so that three levels which all crush OTA AI stay distinct. The owner's playtest judges Easy.
- **A stalemate at 45 minutes is a failure**, over counting it as a draw.

## Code

- **One shared C core** carved out of `tagpu_wikisim.c`, over a second simulation beside the wiki's.
- **`sim.js` is retired**: the mockup calls the C core through `serve.py`, as `cob.js` then did for the COB host alone. This was chosen over keeping two languages in parity.
- **The wiki inherits every fidelity change**, over keeping a simple mode for it.
- **No C++**: the core, the rules and the brain are C, and the harness is a C driver plus Python, over a C++ harness.
- **BARb / CircuitAI is inspiration only**, over porting it: it is GPL v2 and Impure is MIT. No code is copied or translated.

## tasim and calibration

- **tasim simulates the whole game as Impure ships it**, over ground and sea only.
- **Coverage is measured** against an inventory of every key the engine reads, over a claimed feature list.
- **At least 20 × turbo on one core**, checked at every T step.
- **Calibration bands are the engine's own spread**, over owner-picked tolerances. The owner decides the rules once, and the numbers come from the engine.
- **50 perturbed runs per scenario.**
- **Duels are generated** from the mod's data, over writing them all by hand.
- **The tasim and AI landings alternate**, over finishing tasim first, so ImpAI plays real games early.
- **Research step L0 runs beside S1**, since neither touches the other's files.

## Settled at S3 and L1 (2026-11-11)

- **ImpAI sees what a human in its seat would see.** Under Permanent or Circular LOS its view uses the seat's own mapped bit, over the local player's bit the engine reads (`0x408090`, `0x47D2E0`), which would show it the human's exploration. Submerged enemies are located only by the seat's own sonar and never identified, and an ally's units are position-only when the ally shares radar, as the engine shows a human; both stand until the naval and alliance work.
- **One rule everywhere for snapping, placement and the sweep:** the wiki's demos change to follow the game's rules, over keeping the wiki's simplified copies, and the snap scorer's distance tie-break becomes exact 64-bit integer arithmetic in the game too, over a separate x87 build of the shared module.
- **`tagpu_worldq.c` and `tagpu_impai.c` are `publisher sim` rows** in `thread-split.allow`: both read engine state on the game thread only and publish through the frame packet.
- **The seat choice at L1 is a test lever,** `tagpu_impai.on`; `impure.cfg` and the AI button come with A1.
- **ImpAI's maps are derived, not stored, at L1**; A1 rebuilds them on demand, over a stored copy in the recording.
- **Build options come from the build menu,** as Quick Build reads them, over the CANBUILD lists.

## Settled at A1 (2026-11-11)

- **A1 is three landings:** A1a the seat choice as data (`impure.cfg`'s `seatai`, the recording's `RT_PLAYER` byte 15) and the handicap exemption; A1b ImpAI's economy brain in the engine together with the seat button, so a build never offers an ImpAI that does nothing; A1c the same brain in tasim, after T1. Chosen over one landing that would wait on T1.
- **An ImpAI seat skips the stock AI's hit-commander rule** (`0x406FC3`: a hit commander drops its orders and pauses, drawing the simulation's random stream), so ImpAI owns its commander's orders; OTA AI keeps it.
- **`seatai` is honoured under `--defaults`**, a loosening of that launch contract for one key, so launches with every other setting at its default can still seat ImpAI.
- **New `thread-split.allow` row `src/tagpu_skirmish.c publisher presentation`** for the seat button, and the `tagpu_impai.c` and `tagpu_resource.c` rows' reasons widened to the loader thread, where the build menus are read once at level init.

## Settled during S3, A1a and C1 verification (2026-11-12)

- **The reclaim sweep's split computes on SSE2 in the game too** (distances and builder loads), like the snap tie-break: one rule for the game and the wiki, over a separate x87 build; it can differ from the old game only on near-exact ties.
- **Calibration's reload runs count engine schedules as advancing:** the pathfinder's budget and search counters (`pfbudget`, `pfsearches`, `pfcalls`), `mcwatermark`, the wind state and the meteor shower's schedule join the R6 advancing set, over a fresh launch per run (about ten times the cost); the reload-versus-fresh self-test guards their effect on the metrics.
- **Handicap rows 11, 12 and 17 are proven at A1b,** when ImpAI owns its seat's orders and its reclaims reach them; the stock AI replaces any scripted reclaim order on a computer seat. A1a shows in game every other row a scenario can reach without such orders, and names each row it cannot reach and why.

## Settled at T1 (2026-11-12)

- **T1 is four landings,** T1a (definitions and the world), T1b (movement and pathing), T1c (economy and work orders), T1d (sensors and whole games), each with its own exit test, over one T1 landing.
- **The wiki plays the engine's numbers.** Its double-based movement, build and economy rules retire; the shared core keeps positions in 16.16 and the economy in float32, so the wiki inherits every fidelity change. The pages play the real economy: no simplified mode; an entry that wants no stalls gives itself storage.
- **Transcendentals are the core's own SSE2 fdlibm routines,** the same in every build, over inline x87. A difference proven to be a rounding tie gets no tolerance: each case comes to the owner with its proof, and the scenario is replaced by one without the tie.
- **Where the engine hangs, faults or reads the clock, tasim differs and names it:** the pathfinder's wall-following walk is bounded; types with turn rate or brake rate 0 are refused; hover over water is graded only by the water courses' distributions (the engine reads `GetTickCount` there); headless games expire temporary sight every tick.
- **The re-recorded wiki demos are reviewed page by page in the mockup,** and a scout proposes the terrain-and-water arena from the install's stock maps, the choice coming back to the owner.

## Settled during C1 and T1 (2026-11-12, later)

- **The calibration's distribution rule is a stratified permutation test,** over tasim's spread between half and double the engine's: the engine's and tasim's runs are pooled and reshuffled within each seat swap, and two statistics are tested at 2.5 % each -- the two-sample Cramér–von Mises statistic (the shape and location) and the mean absolute deviation from the pooled median (the spread) -- so the false-alarm rate is 5 % per metric by construction, ties included, and a doubled spread is caught; the reference stores the shuffle's seed, the threshold is computed at check time. A threshold from the engine's runs alone still read OUT 7–18 % of the time on a metric where most runs share one value (a duel's survivors), and Cramér–von Mises alone caught a doubled spread about half the time at 50 runs a side; the median rule and the win rate stay.
- **T1's terrain-and-water arena is Etorrep Glacier, `[256, 1408, 2048, 2048]`:** a ridge with passes, a bad-slope band, shallows two to eight cells wide, deep water, no blocking features and a flat plain; its water is a strip of 300–400 px, so ship courses wait for an arena with more of it.
- **The replay globals' hash covers what defines the recorded bytes, not the names and descriptions,** and the reader also accepts v0.5.1's hash, whose file and generated tables are proven to read the same bytes; so `pfbudget` (the per-player pathfinding heuristic weight, `0x5119E8`) and `pfsearches` (slots examined, `0x511A10`) can be renamed without making a published recording unplayable.
- **The handicap exemption drops the Deathmatch respawn's two grants:** the respawn only recreates the controlled player's commander (the owner byte `main+0x2A42`, pushed at `0x4653D2`), which is never an ImpAI seat, so their exempt branch cannot be reached; stock respawn stays unpatched. The mobile energy producer's row, which no retail unit reaches, is shown on a compatibility-suite mod's unit.
- **Hover movement is exact everywhere,** land and water: on the ticks the core's own copy of the hover bob marks altitude, pitch and roll clock-tainted (the engine reads the wall clock below half speed or within 60 ticks of a move), the comparison skips those three fields, and it skips a unit whose motion was ever tainted.
- **T1's ship tracks are short:** Etorrep Glacier's water is three to four rows deep, so ships run 64, 400 and 1000 px east–west and 64 px on the other bearings; long ship courses wait for a water arena.

## Settled at T2's spec (2026-11-12)

- **T2 is five landings**: T2a direct fire and targeting, T2b guided weapons and bombs, T2c death, wrecks and the game's end, T2d features and fire, T2e stockpiles, nukes, interceptors, Impure's extra weapons and keys. Stockpiles, nukes, interceptors and kamikaze move in from T3, and the extra weapons and keys from T4; bombers and anything aimed at aircraft stay in T3.
- **A loosening, limited to two causes**: once a run diverges at a shatter past the effect pools' bound, or at a corpse type the engine took from an uninitialised stack word, the rest of that run is graded statistically by C1's pooled permutation test. Every other divergence ends the run's comparison as a failure.
- **The wiki's rival seats fight**: entries that must not show a fight set their units to hold fire, a scripted kill becomes the engine's death credited to a killer unit, and the affected entries (players, watching, rewind, takeover) are re-baselined and reviewed.
- **A shatter is exact under the pools' bound (option A)**: the core carries an upper bound on the debris and explosion pools and counts a shatter's draws exactly while that bound proves neither pool can be full; past it the divergence is named and the two-cause loosening above applies. Debris itself is not simulated — it never touches combat — but its draws share the one RNG stream. A separate stream for effects (a DLL change that would desync against a DLL without it) and giving up exactness at every shatter were both rejected.
- **The rest of T2's spec is accepted as recommended**: R4, the ballistic solver `0x49A890`, is a named rounding residual with no tolerance; a burst weapon's divide by zero (D1) follows the weapontimer rule; a type with MaxDamage 0 (D2) is refused at load; a corpse type from an uninitialised stack word (D3) is 0, under the loosening; headless games run the engine's victory and defeat tests and end when at most one alliance has units; Impure's key rules and extra-weapon aiming move into the shared core behind thin DLL adapters, so T2e is a DLL simulation landing with its replay test groups.
- **Content past three engine faults is refused or named (T2-Q11)**: a death or burn weapon with the interceptor flag (D7, faults at `0x49A79D`) and a GAF feature whose death or reclaim sequence ends in a 3DO successor (D8) are refused at load; a wreck moved by a stale sequence address (D9, reachable in retail only through the Mars plants) is the named failure `TS_DIVERGE_PTR`, not graded statistically. None is in a calibration arena.
- **T2e's 1..N weapons are proven on the repository's test units (T2-Q12)**: ARMPW4, ARMLLT10, ARMPW4B and WarlordEx with their scenarios, because no supported mod has a type with more than three weapons (2 919 unit files).

## Settled at A1b's verification (2026-11-12)

- **A1b's economy exit is the model's numbers at 5400**: on `impai-eco` the seat reaches at least four extractors, two energy buildings and a finished factory with a constructor queued, which the tick-by-tick economy model in `impai_brain_test.c` (retail CORE costs and build times from a 1000/1000 start) predicts and guards. "Three constructors by 5400" was unreachable under the ladder: a CORCK's 2540 energy against the commander's 25 a second, with the stall rule putting solars first. Lengthening the game and changing the stall rule now were rejected; A2 tunes the ladder.
- **The economy bench's human seat is an idle enemy**: the engine's Start (`0x479760`) refuses a game whose enabled seats share one alliance group, so the template sets no alliance and turns the end test off. A passive third seat in another group was rejected.

## The plan changed: ImpAI is tuned in the engine (2026-11-12)

- **The exact out-of-game simulator is no longer the platform for ImpAI.** T1b, T1c, T1d, T2, T3 and T4 as exact ports of the engine are stopped; A1c to A4 are built, verified and tuned in the engine through C1's harness under the weighted run lock. The engine is the ground truth a simulator would have to be checked against anyway, and it is fast enough: a 5 580-tick game plays in 2.7 to 4 s at turbo, a bench game with its launch takes about 40 s, and a scenario reload costs 3 to 5 lane-seconds a short duel, so 28 slots give thousands of full games an hour. The port was most of the remaining work, every step of it turned up engine oddities to model, and ImpAI's playing landings were sequenced behind it.
- **What stays:** the shared core the in-game wiki plays (S1 to S3); T1a's definitions, map and class maps with defcheck and classcheck; the finished standalone modules (the RNG stream, the pathfinder, T2's combat arithmetic); C1's harness in full. The T1 and T2 specs stay in the working notes as the record of what was learned.
- **The fallback:** if A3's tournament finds the engine's throughput insufficient, the answer is a targeted, approximate combat model calibrated against the engine, not the exact port. A2's fight predictor is a statistical model fitted to engine duels.

## Settled when the AI landings were re-specified for the engine (2026-11-12)

- **A1c is dropped.** Its whole content was tasim's half of A1; A1's exit is A1b's `eco_bench.py` in the engine.
- **A2 lands in two parts:** A2a, the fight predictor -- a model fitted to engine fights, not a simulator -- and A2b, the military manager, the three levels and the default flip.
- **The predictor's 90 % check judges group, naval and defence fights as well as duels**, on fights held out of the fit; each kind is judged beside the total (below, 2026-11-13), and each reference on its own arena (the naval fights' water arena). Stricter than the landing row, chosen because ImpAI decides group fights.
- **A3's "the win holds at rung 2"** is a pre-registered set of 400 engine games, passing at 224 wins or more (p < 0.01), plus a re-run of the bar with the tuned table.
- **A4's coverage** is every unit type of a supported mod classified or refused by name, and the compatibility suite's ImpAI stage passing.
- **A gap accepted for A4:** in a network game ImpAI runs on the peer that added its seat, as the engine's own AI seats do, and a multiplayer recording carries ImpAI's tables only as the recording peer held them: that peer's recorder images every `SIM_STATE` table whole, as it does in a skirmish, and the other peers' contributions carry no table, so another peer's ImpAI state is in no file but that peer's own. Its units' orders replay exactly; a replay or takeover on another peer cannot rebuild ImpAI's own state.
- **The specs' smaller recommendations are accepted as written:** the stall rule, the bar's seating and settings and its five maps, the level lever, the always-on pulse line, the bench's game count, the tuning's machine-time budget, the network toggle and seat name, the network level, and the length of the suite's ImpAI stage.
- **The network level is Hard on every peer** (A4-Q3, settled by the network level above): the host's own Start writes Difficulty 2 (`0x448A0B`) and nothing writes a joiner's on its way in, so ImpAI takes Hard in every network game by construction instead of a joiner's leftover value ([sim](../engine/sim.html), *Difficulty and the AI handicap*).
- **One level rule for every ImpAI seat** (`impai_seats_level`, read by the seat line and the military's init): Hard in a network game, else the skirmish Difficulty, and a Difficulty past 2 plays Normal and is logged, because an unknown value plays the middle level and says so.

## The owner reversed L0's stock weapon scan on 2026-11-12

- **An ImpAI seat runs the weapon scan as a human seat does, `0x4089A0(obj, 0)`, and the military orders the
  commander's D-gun itself**, over L0's `0x4089A0(obj, 1)`. **The evidence**: with argument 1 the scan targets
  CommandFire weapons on its own (`0x408A88..0x408A96`: only argument 0 skips a weapon whose `+0x111` bit 26
  is set; DISASSEMBLED), so an ImpAI commander fired its D-gun at whatever came in range at 400 energy a shot,
  whatever its economy held; A1b's verification saw that drain stall ImpAI's economy for about 3000 ticks
  early in a game. **The rule** (`impai_mil.c`, the D-gun): the commander D-guns only a target the seat sees
  now, only while stored energy covers the shot plus the level's reserve (Easy 0.75, Normal 0.5, Hard 0.25 of
  its energy storage), and only a target worth the shot's energy (its value at least 400 / 60 metal), never
  inside its safety window. The extra-weapons port of the scan tests the same argument, so the rule holds for
  every weapon slot.

## Settled at A2a's GDI calibration (2026-11-13)

- **R6 exempts every moveclass's pathfinder watermark**, `mcwatermark00` through `mcwatermark31`, for the
  reason 00 and 05 were exempted on 2026-11-12: each is a GameTime the search start `0x40E630` stamps
  through `0x440AF0`, which carries over from run to run and is not fight state. The group, defence and naval
  fights bring moveclasses the duels never used (4, 6, 9, 14 seen), which is why they showed only now.
- **R4 exempts player record `+0x148`, the scoreboard rank, as display state** -- decided on the condition
  that no simulation path reads it, which the pristine exe's disassembly shows: its three writers, the
  activation `0x464290` (`0x464353`), the scoreboard `0x4948E0` (one caller, DrawGameScreen `0x469F65`), which
  compacts the ranks while it is shown (`0x494E27`), and the destructor's rank update `0x4866D0` (network and
  mode-2 games, `0x486C17`), which reads it only into the ranks and the lead message; every other `+0x148`
  operand of the exe is another record's ([units](../engine/units.html), `+0x148`). GDI runs the scoreboard and the
  Vulkan lane does not, so the rank moved between GDI runs only.
  The owner accepted the proof with its stated gap: a function handed a `record+k` pointer by its caller and
  reading `+0x148−k` through it is outside what the scan sees.
- **The predictor's check judges each kind as well as the total.** Every kind (duel, group, defence, naval)
  with at least 20 decisive check-half fights must reach 90 % on its own, so a weak kind cannot hide inside
  the total; a kind with fewer is reported as too few to judge and cannot fail the check on its own. The
  overall bar, 90 % of at least 40 decisive check-half fights, is unchanged.
- **Naval references are judged on the default water arena only** (`tools/tasim/calib-arena-naval.json`):
  `predfit` refuses by name a naval reference played on any other map, `duelgen.py --naval --arena`'s
  alternatives included.

- **The fight predictor takes its second form** (A2a's model v2, specced and adopted 2026-11-13 after the dry
  fit of the first): the horizon is the references' 5400 ticks, scored where the fight stands; a weapon's kind
  is what the projectile tick does with its shot -- straight, ballistic, homing (`selfprop ∧ ((guidance ∧
  ¬twophase) ∨ (twophase ∧ tracks))`), dropped; each weapon fires volleys, the first at once; each type closes
  at its own speed to its first armed slot's reach less a 32-px stand-off [INFERRED] (below); an area widens the radius a hit can land
  within; ranges are tested centre to centre against the gap between the two sides' fronts, a ballistic one
  capped at v²/g; a hit deals at most one unit's hp. 11 coefficients (a bias and a lead a kind, focus, splash,
  tau), fitted in a box that keeps every lead at or below 0, pulled toward the start by the prior the
  cross-validation below chooses. Engine facts and addresses:
  [impai](../modules/impai.html), *The fight predictor*.
- **The worst military decision of about 2.3 ms is accepted** (16 predictions of 16 types a side over the whole
  horizon, about 146 µs each on i686): no side is merged to 8 types in game. Owner's decision, 2026-11-13.
- **The reference corpus is played again with units that re-target**, as they do in real play (owner's
  decision, 2026-11-13; the protocol and its decisions under *The re-targeting corpus* below). Under the old
  protocol each unit had one attack order and stood idle once its target died, and an idle unit searches no
  further than its first slot's reach ([orders](../engine/orders.html), *What an idle unit does*), so fights
  stalled at the limit.
- **The check is also reported on the references built after the design**, the honest holdout, since the
  design was compared on the current check half; the gate stays the check half's figure. 2026-11-13.
- **The predictor is fitted by a procedure that reads only the training half**, decided 2026-11-13 after the
  widened fit over-fitted (its training loss 283 below the prototype's, its check loss 769 above): the loss is
  a staircase in the coefficients, kills being whole units, so a wider search finds a deeper step, not a better
  model. The prior pull and the held set are chosen by 5-fold cross-validation inside the training half,
  stratified by kind and repeated 4 times, by the summed validation **data loss**; the accuracy is printed beside
  it and never chooses, since the strong-prior candidates differ by one or two stalemates. The candidates are
  fixed in `predfit.py` (priors 30 to 3000, extended threefold past an end the choice sits on; the homing hold,
  or it with splash at 1), each fold fit the best of 3 seeds, the final fit the best of 6. `fit`, `cv` and the
  selftest refuse a check-half fight; `check` reads the check half once, after the fit, and nothing is chosen
  again after it; the cross-validation runs again whenever the reference set changes. The selftest stays at
  its prior of 1 with nothing held: the cross-validation's choice would pull it off the truth it is judged
  against, and at 0 the search stops 3.7 % above the truth's loss. A change to the model's form is judged by this cross-validation, both forms on the
  same folds, before `check` runs on it.
- **The homing kind's bias and lead are held at (2, −1)**: a guided shot at a standing target hits as a direct
  shot does, and a target fleeing at a fifth of the shot's speed is still caught about 86 % of the time
  [INFERRED: a radial-flight bound]; the references cannot tell (25 and 7 of 117 decisive training fights move
  with them, none flips), and holding cost nothing in the cross-validation. Revisited past about 20 decisive
  training fights with a homing weapon firing at a moving target.
- **The predictor's chase walks to the first armed slot's reach** (S1, the engine's literal rule: `0x4897E0`,
  `0x4035A1`, `0x403623`, `0x4035D6`), not the longest reach that hurts the enemy type: in the training-half
  cross-validation with homing held at prior 300, on the one-order references (`ceed681e6fa9`), it lowered the
  validation loss 29 % (18590 to 13250) and
  raised the accuracy from 420/468 to 424/468. The variant that walks to the first slot that hurts the type
  scored within 1 % and one fight of it; the engine's rule was taken.
- **The military reads no fact of a target that the seat's observation of it does not show** (A2a's landing
  review, 2026-11-13). `observe` gives the resolver's target facts (`WQ_OBS.aim`: airborne, under the sea, a
  type that flies) only where it identifies the unit, its own or in sight; at an ally's shared radar or the
  seat's own radar contact they stay hidden, as its type and owner do. `can_attack` takes such a target on the
  unit's own tests alone and lets the order resolver refuse the rest, counted as a refusal and costing no
  order -- the military's behaviour before it read the resolver's tests. The premises the resolver test rests
  on are in [ImpAI in the engine](../modules/impai.html), *Whom a squad attacks*.

## The re-targeting corpus (2026-11-13)

- **Each unit with a mover gets one TA Shift queue of attack orders**: every enemy the resolver lets it attack,
  nearest first by the squared distance between the scenario positions, ties to the lower enemy index; the
  second and later orders carry `"queue": true` (`tools/tasim/duelgen.py` `queue_orders`;
  [scenario format](../scenario-format.html), *Queued orders*). When the head's target dies the engine's own
  controller starts the next attack (Attack_Chase `0x4034A0`, result 5 on a cleared target). Duels and courses
  keep their files and so their scenario ids.
- **Nearest-first, not ImpAI's quickest kill (Q1).** Normal and Hard fire on the quickest kill, which needs the
  predictor's own pair rates, so a corpus built with it would depend on the model it is used to fit.
  Nearest-first is ImpAI's Easy rule and its fallback on every level, and uses no fitted quantity. The cost: the
  predictor's fitted `focus` describes nearest-first fire, not ImpAI's focus. Owner's decision.
- **Defence structures get no orders (Q2)**: they keep the engine's standing Guard_NoMove `0x4021F0`, as in real
  play. Queued, a structure's attack Attack_NoMove `0x402160` ends on a head out of reach: AutoAim raises
  `+0xBA |= 0x1000` when can-aim fails at reload 0 (`0x49E53A`), state 2 (`0x402191`) returns 9, and result 9
  (`0x43B8E5`) frees a node that has a successor (`0x43B8F8`), so a nearest-first queue drains one head a tick to
  the farthest target, retries that one every 30 to 59 ticks with an RNG draw each time, and never returns to
  Guard_NoMove (the ta-scout's batch R1, DISASSEMBLED; [orders](../engine/orders.html), *What an idle unit
  does*). The generator orders only a unit whose def `+0x22F` (bmcode) is 1, the creates' own mover test
  (`0x4860BB`, `0x4862CF`), the test ImpAI uses. **The gap this leaves for the fit**: a tower's own search,
  Guard_NoMove's `0x40AD80` and `0x40B7B0`'s range and sight limits, is not established, so sight may enter
  the defence fights, which v2 does not model. The defence kind's cross-validation result decides whether it
  matters. Owner's decision.
- **The protocol was diagnosed on two check-half fights**, group-141 and naval-012 (`fit-v2-spec.md` §4's
  stalemate). The check half is meant to be read once, after the fit; these two were read before the protocol
  was chosen, so a reader of the cross-validation and of the check figure on the new references should know the
  check half informed the design here. Under the re-targeting protocol neither reaches the limit in any run:
  group-141's side A wins 25/25, which the fitted predictor calls, and naval-012's side B wins 25/25, which it
  still gives to A (m +0.61), one of the check's nine misses (MEASURED 2026-11-13, `defcfcdb2359`).
- **The predictor is fitted to the re-targeting references, and its check passed** (2026-11-13, reference set
  `defcfcdb2359`): the cross-validation chose prior 300 with homing held, its estimate 428/484 (88.4 %, group
  86.1 %); `check`, run once after the fit, passed at 113/122 (92.6 %), group 64/69, the other kinds too few to
  judge. The cross-validation's figure is the one to plan with; nothing was chosen again after the check.
  Figures and coefficients: [impai](../modules/impai.html), *The fight predictor*.
- **The new generation lives in its own calibration root**, `TACALIB_DIR=<main checkout>/.data/calib-rt`
  (`scenarios/gen`, `runs`, `refs`), and the old `.data/calib` is never written: a duel's and a course's id is
  the same in both generations, and `predfit.load_fights` takes each id's newest reference. `calib.py refs
  build` refuses a runs folder outside the calibration root it writes to (`calibref.runs_in_root`).

## The owner accepted row 18's Flea race on 2026-11-13

- **`handicap_check.py` reaches row 18, the purge, with the human seat's Fleas, and their race with the stock
  AI's D-gun is an accepted limit of the check**, over a striker of the commander's own seat. A Flea usually
  hits OTA AI's commander first and can be D-gunned before it does (MEASURED 2026-11-13 at Medium,
  8e6936f1); when it is, the check fails at once and by name (`fleas_lost`), and a re-run is the answer.
  **The reasons**: the engine review proved the row's bypass byte for byte, and a stock seat runs its own
  compare first, so the check's job is only to see each exit taken; live runs have counted the stock AI's
  scaled branch several times; and a lost Flea fails the run at once and by name, so a re-run settles it. **What the
  striker could not do**: a seat's own CORPUN, 56 and 64 wu from its commander, counted no seat in any
  launch, because the pitch solver loses the sign of a downhill shot and fires it over the commander, and a
  tower laser's blast, radius 4, goes off up to a step past its point and below the ground
  ([weapons](../engine/weapons.html), *The ballistic reach*, *A line-of-sight shot*). The striker code was
  removed.

## A takeover's fresh ImpAI seat in compare-fork (the owner, 2026-11-13)

- **`tarec compare-fork`'s D2 leaves out ImpAI's state of a seat the takeover newly gave ImpAI, at the fork's
  first capture only**: that seat's rows of `s_seat`, `s_brain` and `s_mil`, and the `s_job` rows of its unit
  block (`tarec.py`, `fork_view`'s (e)). **Why**: `seat_fresh` starts such a seat as a level start does -- its
  mark set, its brain reset, its jobs cleared, a new military -- so those rows differ from the parent's by
  design, and D2 (c) already leaves out the same seat's change of controller; the mask extends that one, never
  the general rules. **How narrow**: the seats are derived from the fork's own takeover record, the seats the
  fork's header marks ImpAI's (`RT_PLAYER` byte 15) and the parent segment's header does not, never from a list
  in a test group; only those seats' rows of those four tables; only at the capture at the fork's `fork_at`.
  Every later capture is compared in full, and so is every other seat's ImpAI state and every other table at
  the first. The header rule is never wider than the takeover's own fresh set: a parent's marks are set only at
  its level init or its takeover, both before its header's capture, and after that only cleared (the `AI`
  command), so a seat its header does not mark was not marked at the fork. The test groups that play it are
  `impai-takeover-fresh` and `stock-impai-takeover-fresh` (A4's d4, [the module](../modules/impai.html)).

## The naval rule's two design calls (A2b, 2026-11-13)

Both are the coordinator's; the first was moved by the bar's diagnosis, and the owner may reverse either.

- **The sea plant waits for `factory_per_m`'s income pacing, like any second factory.** When the military
  judges the seat's war naval, its next factory after its first is a sea plant, wanted once one factory stands,
  nothing stalls and the metal income is at least `factory_per_m` times the factories standing (`impai_brain.c`,
  `economy`). The second factory also waits for the judgement (`naval >= 0`), so the order the two managers run in
  cannot decide which kind of plant takes the slot. **Why**: the spec first exempted the sea plant from the pacing,
  on the argument that the pacing is about more of the same production while the rule is about reaching the enemy
  at all; the bar disproved it. On Trout Farm, Arm against Arm at Hard, the commander found no patch within
  `comm_r`, so the seat's income was the commander's own 1 metal a pass; the shipyard it began at GameTime 2 557
  emptied the store by 4 207 beside the factory's constructor, held the commander on a frame it could not finish,
  and the seat completed no unit from 3 607 to 12 607 (MEASURED 2026-11-13, `bar-hota-troutfarm-aa-1`). Paced,
  the plant comes once the constructors have the income up. **What it costs**: an island-map seat with a poor start
  holds one land plant longer; a seat whose military refuses every decision keeps one factory (`refused=` on its
  mil line).
- **Builders keep to their layer too.** The explore cell and an assist's target must be reachable on the
  builder's class (`impai_build.c`, `impai_build_explore` and `builder_near`), which widens the rule beyond the
  military. **Why**: a sea plant makes sea constructors; without the test one of them, made the seat's one
  explorer, is sent at a land cell it never arrives at and holds the explorer until `job_timeout`, and a sea
  constructor walks at a plant inland to help it. On Comet Catcher every block is one component for every class,
  so neither test changes `impai-eco`'s choices; on the brain test's walled map the explorer now goes to a cell on
  its own side of the wall.

## Finishing a won game: two changes to A2 §4.3 (b7, 2026-11-13)

b7 r1's diagnosis found the military never finishing games it was winning: no attack order and no D-gun (the role
fillers dropped `canattack` and `candgun`), the base-threat rule pinning every forming squad in DEFEND, the squad
table filling with one-unit DEFEND squads, fighting squads standing at their centroid, and nothing hunting the
commander. The fixes are bounds and orderings inside A2 §8.5's "fix the brain"; two of them change A2 §4.3 as
written. Both are the coordinator's, and the owner may reverse either:

- **An ATTACK no longer ends at its target or its timeout.** It ends by the predictor's RETREAT, or when it has
  arrived with nothing to attack and no goal it has not arrived at; its goal is chosen again at each decision,
  before the contact test -- a commander in sight, a remembered commander, the most valuable known structure near
  the estimate, then the sweep for the squad ([the brain](brain.html), *The military*, step 4) -- and again on
  arrival, its point's sweep cell stamped. **Why**: a wave whose structure died, or that arrived, walked home to
  FORM, and the next wave went to the same point; the commander, the one target that ends a commander-death
  game, was never a goal; and a wave that arrived at a point holding nothing it could attack -- a structure it
  cannot hit, a mixed squad's sweep cell its first member alone reached -- stood there for the rest of the game,
  holding an ATTACK slot (the high review of the finish fixes). **What it costs**: a wave that cannot win holds
  until the predictor retreats it, a land wave with nothing reachable known sweeps its own side of the water, and
  a structure the wave cannot hit sends it back to FORM, from where a later wave may go for it again.
- **A base threat is only an armed mobile enemy on the seat's side of the map**: a contact member that is
  mobile and armed, nearer the anchor than every known structure of its own owner (else every start but the
  seat's own), within `base_r` + 512 of the anchor or 512 of an own structure within `base_r`; and a squad answers
  only a threat it can hurt. **Why**: `base_r` + 512 is 2 048 px, which reaches into the enemy base on Pincushion
  and Lava Mania, and the economy's outlying extractors stand beside the enemy's, so enemy structures and unarmed
  builders held every forming squad in DEFEND: b7 r1's 30 no-attack games sent no wave. Each member's own owner
  and not the estimate of every enemy's base, because with enemies on opposite sides that estimate falls near the
  seat's own base and switched base defence off (the high review). **What it costs**: an outlying structure
  raided by armed enemies is no base threat, nor an armed enemy nearer one of its own side's structures than the
  anchor; only a squad already within reach of the contact answers either.
- **The four ATTACK squads are shared by the layers, with room for a layer's first wave.** With four out and
  none of the wave's layer, the two of one layer whose points are nearest merge. **Why**: four land waves kept
  every sea and air wave home (the high review). **What it costs**: two land waves become one.

## The commander's safety and the levels' L1 fixes (b7, 2026-11-13)

b7 r1's commander and levels diagnoses found the commander dying where it should not have stood: building and
guarding past `comm_r`, fleeing to the farthest structure anywhere, and caught in the enemy commander's death
blast. The owner's decisions on the levels:

- **Raids stay Easy 0, Normal 1, Hard 2.** The bound that a raid takes only units beyond the next wave belongs to
  the finish or L2 work.
- **`retreat_margin` stays −0.30, −0.15, −0.05**, judged again once these fixes are in.
- **`wave_value` is decided after the fixes.**
- **`task_timeout` stays a level row, 5 400 / 3 600 / 2 700, and now means the time at the target**: a task's
  lifetime is that plus its travel ([the brain](brain.html), *The military*, step 4).
- **Commander bit 4, `canattack`, turns attack orders on for the whole army**: intended; the finish fixes copy it.

The design calls these fixes make, each with its evidence; the owner may reverse any of them:

- **Every commander order lies within its reach.** The base ring, the sea plant and the assist hold the commander
  to `comm_r` of the anchor (`FAR_PATCH_K` × `comm_r` in the opening), the site's footprint centre tested, and the
  commander never guards a factory. **Why**: 221 of b7 r1's 318 commander build and guard orders lay past
  `comm_r`, up to 4 488 px. **What it costs**: a commander whose ring is full within `comm_r` builds nothing
  and leaves the ring to the constructors; it helps the factories' frames within `comm_r` instead (*The
  commander works*, below).
- **No value gate on the commander's safety.** It flees any armed contact its nearby army would not beat alone,
  over fleeing only a contact worth more than the commander. **Why**: no army contact is worth a retail
  commander's 30 423, so the old rule never moved it (162 of 222 seat-games). **What it costs**: a commander
  with no army stands back from every raid it does not beat by the stand's margin (*The commander works*,
  below), and its D-gun with it.
- **The blast keep-out**, the foe commander's blast radius + 64 px + the approach of both over a decision, and
  the D-gun never at a commander. **Why**: fourteen b7 r1 games ended in a double commander death, the two 17 to
  484 px apart against a 475 px blast. **What it costs**: a commander-death game is never won by our commander's
  D-gun.
- **The leash**: a commander past its reach + 512 px walks home. **Why**: a flight or a chase must not leave it
  in the enemy's half. **What it costs**: one move a decision at most, under the safety window.
- **The D-gun's guard** (the finish review's M3): no shot that could reach a friendly unit or our commander, and
  every military order of the commander ends its economy job with no failure. **Why**: with the role fillers
  fixed the D-gun fired for the first time, and the most valuable target in range could stand among the seat's
  own buildings. **What it costs**: no shot at a target among friendly units, nor beside the commander with a
  blast that reaches it.
- **An ATTACK keeps no lifetime.** The levels diagnosis asked the time limit plus travel for ATTACK too; the
  finish fixes made an ATTACK end by retreat or on arriving with nothing to attack and no new goal, its goal
  chosen again among the points it reaches at every decision, so no unreachable goal can park it and a lifetime
  would recall waves that are winning.
- **The ring's order** is decided below (*The commander works*).

## The commander works, and the base grows from the anchor (diag-2, 2026-11-13)

The cmdr run's diagnosis (65 games after the commander's safety fixes) found the commander kept home but idle
for most of the game, the base still growing toward the enemy at the second start, and the factories replacing
dead constructors instead of building army. The owner approved the stand (Q1) and the ring's order (L2); the
constructor bound (L3, Q2) waits on a governor design.

- **The hand-back.** A builder with no job whose head is its one order, names no unit and is no build is the
  economy's, listed idle -- the commander outside the military's window, and every constructor, a turned-back
  one included. **Why**: a flight's point is a structure's centre, which the commander never reaches, and the
  ground move ends only on its arrival event (`0x4031D0`), so the move held the head and nobody ordered the
  commander again: 34-35 % of its samples after GameTime 9 000, up to 67 200 ticks, 91 % with no armed enemy
  within 800 px. A constructor turned back from known threat held its move the same way, uncounted by every
  list but the governor's, which kept counting it (the brain review's H1). **What it costs**: such a move
  outlives its purpose by one economy decision that has work for the builder; a head that names a unit -- the
  D-gun's attack, a guard -- stays its maker's, and a list of more than one order is left alone (a Park with a
  node behind it).
- **The commander helps at home.** With no building frame within `comm_r` it helps the factories' frames there,
  with the help order, never a guard. **Why**: Standby or empty in 21-22 % of its samples (the finish run 3-6
  %), help 3-5 % (33 %), while every seat-game had a factory within `comm_r`; the guard is what chased.
  **What it costs**: nothing measured yet; that the help adds the commander's build power is the bar's to show.
- **The stand** (the owner's Q1). Where its army alone does not win, the commander stays when the prediction with
  itself counted wins by 0.5 or more, within `comm_r` of the anchor only; it still flees anything close or
  stronger, and the blast and D-gun rules are unchanged. **Why**: 249 of 461 flights began with 3 or fewer armed
  enemies within 800 px, and in `ne-troutfarm-ca-2` the commander fled 1-8 raiders for 11 000 ticks while 78
  frames died on its yard. **The D-gun counts only when it could fire**: the commander's CommandFire weapons are
  in the prediction only while the D-gun's own rules hold this decision -- the window, the energy with
  `dgun_reserve`, a member seen now, identified, worth the shot and no commander --, otherwise its entry is
  flagged `IMPAI_FE_NO_COMMANDFIRE` and fights without them (the brain review's M2: an Easy seat low on energy
  stood against raiders it beat only with a shot it would not fire). **What it costs**: a counted D-gun fires
  at its reload in the prediction, where the seat orders one target a decision, so a fight it stands can still
  be one it loses; the margin, 0.5, is hand-written and the bar's to move.
- **The ring's order.** The base ring's sites are taken nearest the anchor first, ties farther from the estimate,
  then the lower cell; the commander's nearest where it stands. **Why**: the ring's own east-first order grew
  the second-start bases toward the enemy (structures' mean projection +577 px against −146 px at the first
  start, 1 961 constructor deaths against 551). Ordered from the anchor for every builder, the commander walked
  back from its far extractors and the economy game's bar came at 4 218, past `EGAME_FLEA`'s 4 700 less 600;
  the commander's own sites are held within `comm_r` by its reach already, so its order starts where it stands,
  which keeps the bar at 4 055. **What it costs**: early in a game the commander may build on the enemy's side
  of the box within `comm_r`, ties going to the site farther from the estimate; constructors grow the box on its
  side nearest the anchor wherever their work is.
- **`predfit check` checks only the generation it was fitted on.** It refuses references or content other than
  those the coefficients' header names. **Why**: the reported fall to 108/119 was the check run against
  `.data/calib` (`ceed681e6fa9`), the default root, while the coefficients were fitted to `.data/calib-rt`
  (`defcfcdb2359`), where they pass 113/122; nothing had changed.

## The production governor, builders and threat, reactive defences (2026-11-14)

A human's game on Comet Catcher (a line of 29 Samsons across the map) and the bar's round of 196 seat-games found
the factories making constructors and the constructors walking into known fire: 95 % of the factory-ticks went
to constructors, 26 of 27 died to the line, and at 12 000 ticks the seats held five or six constructors and no
army at every level. The owner's decisions:

- **The governor runs at every level, Easy included.** Easy stays the weakest through the rest of its level row.
  The constructor quota is gone; each idle factory takes a constructor only past the opening, the army's share
  and the pressure ([the brain](brain.html), *The military*, the governor); the build power this entry also named
  was the measured cause of the stalls that followed, and is deleted (G3, below). **What it costs**:
  Hard's economy is A1b's no more; `eco_bench.py` measures the difference against A1's exit test.
- **No preventive tower.** Towers are reactive, at every level, while enemies press the base. The type is chosen
  by value against the seen threat, cost-normalised, over the types the seat can build -- never a unit list, so a
  mod's towers are chosen by the same rule. In the base game it comes out as the missile tower for both sides.
- **Builders avoid known ground threat on the base ring as on a walk**, and turn back from a walk or a site the
  threat comes within reach of. The commander's rules are unchanged: `comm_r`, the military's safety and leash.
  A turn back goes only to a cell the builder's class reaches, and the turned-back builder is the ladder's again
  at the next decision (the hand-back above).
- **The shared classifier is kept.** The threat rows gain the ground reach by the engine's target rule
  (`0x40B7B0`'s fallback past NOTAIR), and only the threat maps read it. `tasim_roles`' role, what a type is
  *for*, and the calibration stay as they are, and the code keeps the two apart (`impai_maps.c`, THE GROUND
  REACH).

The design calls, each with its evidence; the owner may reverse any of them:

- **BARb is the idea, not the code.** The build-power cap (CircuitAI's `UpdateBuildPower`), its army/builder split
  and `AiMakeDefence`'s answer to a mobile threat with a tower are read for their design only: CircuitAI is
  GPL v2 and Impure is MIT, so no code is copied or translated, and every number is ImpAI's own.
- **The pressure is local, not BARb's global army ratio.** A seat sees enemy mobiles only now, so a global
  ratio has no figure here; the pressure weighs what presses the base or a structure against the whole army.
  Enemy commanders never press: an enemy armed unit or commander came within 1 500 px of the anchor from
  GameTime 6 600 at Lava Mania's second start, and a commander among the expansions would hold the constructors
  all game. A radar contact presses only while its substitute has a ground reach: a contact of no ground reach
  -- an air scout over an extractor counted as an aircraft -- holds no constructor and builds no ground defence
  (the brain review's L5). **What it costs**: a contact carries no type, so an aircraft on the radar still
  presses while the enemy's median seen mobile type has a ground reach.
- **A frame counts once.** The governor counts finished units and the factories' count nodes, held to 255; a
  count node holds the unit in progress, so its frame is not counted again. A frame whose factory is gone is not
  counted, which errs toward the army.
- **The tower's score has a reach term.** Σ share × pair rate × [its reach ≥ the enemy's] / value. **Why**:
  without it the light laser wins in the base game against an army that outranges it; with it ARMRL and CORRL
  win, the owner's expectation. **What it costs**: a tower that one seen type outranges and that outranges the
  rest is scored on the rest alone.
- **The walk check covers frames and assists, not only walks.** A builder at a frame or an assist leaves when
  the threat reaches its work or its own cell. **Why**: four of the 27 died at work in the base during the raid.
  **What it costs**: a ground defence's builder at a site on the threat's edge is turned back as well once the
  threat's reach covers its cell; only the commander is exempt.
- **The orders before the factories' leave them their roots.** The job checks' stops, the walk checks' turn
  backs and the ladder spend at most `roots` less one for each idle factory, up to `queue_max`. **Why**: a raid
  that turned back eight builders spent every root, and no factory queued that decision (the brain review's
  L4). **What it costs**: a decision with idle factories gives the builders at most `roots` − min(`queue_max`,
  idle factories), four at the least at every level; one with none keeps all eight.
- **A threatened site does not grow the ring.** A ring whose sites were left for ground threat, at the site or
  on the route, for the commander's reach or for its safe distance is not exhausted: growth answers only a lack
  of room. **Why**: a raid over the base for a few decisions grew the ring toward eight and passed a plant type
  over for `fail_t`, so the seat built another plant type (the brain review's M1).
- **The commander keeps its safe distance on the ring.** Its ring sites within 600 px (`IMPAI_CMD_SAFE_PX`) of a
  seen armed enemy are skipped. **Why**: the military flees a contact that close, and the economy took the
  commander back to the same site once the window lapsed, a cycle of 300 ticks (the brain review's L3). **What
  it costs**: near a contact the military would stand against, the commander's ring sites wait for it to leave.
- **The ground defences count the anti-air towers that reach ground**, wherever they stand, so `def_max` missile
  towers built against aircraft hold the ground defences back. **Why**: by the engine's target rule each fires on
  ground units when no aircraft is a candidate.
- **A ground defence's ring is ordered from its point**, by the same sort as the ring's order above, whoever
  builds it: the anchor moved `rally_px` toward the pressing member, so the tower stands on the ring's side
  toward the raid. **What it costs**: nothing for the rest of the ring, whose origin rule is unchanged.

## The plant choice: the army a plant gives, on this map (2026-11-14)

The bar's round found CORE seats teching up to the hover plant and never further, and 31 % of CORE's army units
hovers on maps whose land armies reach the enemy. Two causes: the tech-up compared a plant's **offer** -- the
highest classifier tech of its units, a graph distance that makes CORHP's units tech 2 though its constructor builds
nothing above tech 1 -- and took the least value among the plants offering more, which was CORHP; and army orders
went to any idle plant, whatever its army was worth. The owner approved the fix, with four parts, and no unit
lists, so it holds across mods:

- **The tech-up compares build tech, not offer.** A plant's build tech is the highest tech its constructors build
  (`IMPAI_TINFO.ctech`; only those that can build an extractor, *The economy in mods* below); a tech-up takes only a plant whose build tech is above every factory the seat has. CORHP
  is build tech 1, so it is never a tech-up.
- **The values are computed once, at the level init.** Each land fighting type's value per metal, on each tech
  level 1..4, is the mean of the fight predictor's equal-value pair margins against the field of that tech
  ([the brain](brain.html), *The values*). They are content: no seat's table, picture or reach enters them.
- **A plant scores by its best reaching unit.** Each candidate is scored by the best value among its land
  fighting types that reach the military's estimate of the enemy from the anchor; the best score wins, ties to
  the least value.
- **Army classes go only to plants whose units are best here.** A gated class goes to a factory whose unit of
  that class is at least min(0, the seat's best of that class); a plant eligible for no class makes constructors
  only, or nothing.

The owner's answers to the spec's questions:

- **Q1, the opening plant stays the cheapest plant with a builder, as before** (among the plants with a land
  army, *The economy in mods* below, Q2). Scoring applies to every later
  factory and to the tech-ups. **Why**: the opening's first purpose is the constructors, A1's economy bench passed
  on it, and the scored opening would have sent ARM down the kbot path to ARMALAB (best unit −0.19) where the
  vehicle path reaches ARMAVP (+0.44).
- **Q2, "much shorter over water" is left out.** Only a missing land route counts for a class's extra mobility.
  **What it would need**: a route length per movement class and a term turning arrival time into value per metal,
  which nothing measures; it would be its own landing, after measuring how much longer the land routes are on Red
  Triangle and Trout Farm.
- **Q3, with no land route the land factories still build land army by value, for base defence.** When no gated
  type of the seat reaches the estimate, the army orders read the values without the reach, as on a linked map.
  **Why**: the reach follows one estimate, and on a map of three or more players a land-linked enemy who is not the
  estimate would otherwise meet no land army. The plant choice keeps the reach either way.

The first bar runs of the plant choice (13 games, 2026-11-14) found three faults. The fixes are design calls within
the approval, each with its evidence; the owner may reverse any of them:

- **Unlocks.** The candidates include the plants that only a constructor the seat does not have yet can build, when
  one of the seat's own factories makes that constructor. When such a plant wins, the governor's next constructor
  is that type. **Why**: 6 of 7 CORE seats opened on CORVP, held CORCVs only, and teched up to CORAVP at −0.396;
  CORALAB (+0.517) was never a candidate. **What it costs**: while the unlock is wanted, a factory that does not
  make it gets no constructor. The governor's rules still bound how many constructors there are, so the
  unlocks are candidates only below `cons_max`: at the cap no constructor can come, and the seat takes the best of
  its own plants rather than wait for ever. The other rules can still hold an unlock back below the cap (the
  army's share and the pressure), and the plant waits while they do: a stated gap.
- **A tech-up whose best candidate scores below 0 still goes ahead** (owner's decision, 2026-11-14). The score
  ranks the plants' armies; the tech-up also brings the tech-2 constructors and economy, and the army orders still
  go only to the plants whose units are good here, so a plant with a poor army makes constructors.
- **Q3 applies per class.** Q3's fallback (read without the reach) now applies to each gated class no type of the
  seat reaches in. **Why**: one amphibious unit that reached on Trout Farm's advanced plant switched Q3 off for the
  whole seat. The two vehicle plants then sat poor for 726 factory-decisions.
- **A poor plant buys army in a surplus**, its cheapest type of any class. **Why**: the governor promises that no
  factory idles beside a full store, and part 4's "or nothing" was about the metal a poor army would cost, which a
  full store does not lack.
- **The reach targets every other start until an enemy structure is seen**, and a class reaches when it reaches
  one of them. A class's component at the base is the label most blocks within 8 rings of the anchor carry, not
  the anchor's block alone. Allies' starts are still targets: the brain has no read of the teams yet.
  **Why**: ImpAI's guess, the nearest other start, sits across the water on Trout Farm, while the enemy there was
  land-linked, so the hover plant was chosen for a route the map did not need. Separately, the anchor's impassable
  block, read as no evidence, made CORREAP "reach" where CORGATOR did not. **What it costs**: a seat whose real
  enemy sits on the one start cut off from it builds land plants until it sees an enemy structure.
- **A plant the values do not score walks after every scored plant** for the governor's constructor -- a shipyard,
  an air plant: a factory that offers no land fighting class -- and makes one only when no scored plant is idle
  that decision (coordinator's call on the plant choice's high review, 2026-11-14). **Why**: such a plant's best
  class value is none, the lowest, so the ascending walk gave it the constructors first: CORCS from CORSY and CORCA
  from CORAP before any land plant's. **What it costs**: a naval seat whose land plants are all busy still makes
  sea constructors, which its sea plant has work for.

## The economy in mods: extractors, payouts and the plant rules (2026-11-14)

The mods1 run (28 games of ImpAI against OTA AI on six setups) found Total Mayhem and TA Twilight seats with one to
five extractors and 118-136 refused sites a level on Pincushion, and TA Twilight's plants ordered on reclaim
payouts. Two fixes followed from the diagnosis, and the owner answered four questions:

- **The extractor is chosen at the patch.** A builder's menu gives its best land and best water-only extractor; the
  water type goes where the patch's centre lies at least its minwaterdepth under the sea, the land type everywhere
  else. **Why**: the type was chosen before the patch, the water type extracted more in both mods, and the site
  test refused it at every dry patch.
- **The income gates read the steady income**: the income less each non-extractor unit's last-pass metal above
  its own make rate. **Why**: 17 of 65 plant wants rode on a reclaim payout or a factory's refund. **What it
  costs**: `factory_per_m` and `tech_m` were set against an income that included payouts, so the wants fire later
  where reclaim was a large share of the income; retail and ProTA had no want carried by one.
- **Q1, extractors are ranked by metal per value**, as energy is, each kind within itself. It changes TA
  Twilight's ARMCK from ARMAMEX (0.00125 for 214) to ARMMEX (0.001 for 82).
- **Q2, the opening plant is the cheapest plant that has a land army** (a land fighting type that reaches), and the
  cheapest of all when none has one; a wet anchor's opening is still the cheapest of all. **Why**: TA Zero's GoK
  opened on GoKT1AF, an air plant. The opening otherwise stays unscored, as Q1 of the plant choice set it.
- **Q3, a plant's build tech counts only the constructors that can build an extractor**, the set its candidacy
  already uses. **Why**: GoKT2AF was a tech-up only through a jammer-builder whose menu is mines and decoys.
- **Q4, a tech-up's candidates are scored at the next tech level** (coordinator's call: no measured choice
  changes). **Why**: scored against the tech-1 field, a tech-2 unit saturated at 1.000 (Mayhem's CORAVP,
  Escalation's ARMAVP).

## The levels are personas (2026-11-14)

The owner's decisions for the levels, after the bar showed Easy and Normal winning by playing a weaker plan
([the brain](brain.html), *The plan and the personas*):

- **One plan for every level; a level is only how the plan is carried out**, over a level row of weaker decisions
  (fewer orders, no focus, no raids, noised margins, a later tech-up). A persona limits the looks (how often each
  manager decides), the reaction (ticks from a decision to its orders), each manager's actions a minute and burst,
  and the attention (builders busy and operations held at once). **Why**: a weaker plan plays a different game, so
  the levels did not order by strength and a fix to one row did not reach the others; a slower player with the
  same plan is one dimension of strength, which a ladder can calibrate. The plan's noise goes with the rows: the
  plan draws no random numbers, so ImpAI no longer reads the simulation RNG's state `0x51FC88`.
- **The model is Nanolathe's Modern AI**, MIT-licensed, credited at the head of `impai_react.c`: its persona's
  parameters, its host's one pending batch given when due, its executor's token bucket, a group order as one
  action and a command bound to the instance it was issued for. No code is taken; the C is written from the
  formulas.
- **The targets, against OTA AI at Hard**: Easy wins 50-75 %, Normal at least 90 %, Hard 100 of 100, over the bar's
  100/100 at every level on the same Difficulty. **Why**: three levels that all crush OTA AI cannot be told apart
  by it.
- **A capped ladder game is scored by value at the limit**: a lead of 1.3 times the other side's value, the
  commanders counted, wins; less is not a win.
- **If the targets fail, harsher limits come first**, before any change to the plan.
- **Hard is no limit at all**, so a Hard game is the plan's game with every order given at once, byte for byte the
  game before the personas (the landing's V1).
- **The economy's factories reserve at most half a decision's commands** (THE SPLIT), so a budget of one or two
  actions goes to the builders first. **What it costs**: at a low budget a factory can wait a decision for its
  order.
- **The governor keeps no more constructors than the seat's attention keeps busy**, beside `cons_max`. **Why**: a
  constructor the seat cannot task is metal spent on nothing. **What it costs**: at Easy (four builders) the
  economy grows slower than its income allows, which is the weakness the level is.
- **The hold and the mend are the plan's, at Hard's values** (the merge of G3 and the mend, 2026-11-14): `hold_pm`,
  `hold_fac`, `mend_hp`, `mend_wait`, `mend_roots` and `repair_max` decide *what* the seat does, so every level
  plays them alike. `repair_max` was 1 / 1 / 2 by level; it is 2 at every level, because how many builders a level
  keeps busy in all is already its attention (`att_eco`), which holds the repairers with the ladder's builders.
  `mend_wait` is at least the longest look, 150 ticks, in place of the level's own period. **Why**: a decision
  that differs by level is a weaker plan, which the personas replaced.
- **The mend's and the hold's orders go through the budget**: a mender's order is one military action and the
  mend leaves the commander two, as it leaves it two roots; a repair and a hold's release are the economy's orders,
  given through its funnel; and with a bucket the idle builders leave the decision's last root to a helper the
  hold must release, so a slow persona cannot leave a helper on a factory's work for good. **What it costs**: at a
  low bucket a mend waits a decision, and an idle builder waits one for its order while a helper is released.

## G3: why the metal stalls rose, and the hold (2026-11-14)

The owner's order: G3 lands before A3's tier-B tournament; first find why ImpAI's metal stalls rose after the
governor, then balance spending in a stall. Read from the recordings already on disk (the bar's `governor` and
`plantchoice` runs against three earlier runs): over GameTime 9 000..27 000 the new seats stalled -- stored metal
under 5 % of the storage and demand above income -- in 44 % of the passes against 18 %. Over 51 matched
seat-games income fell 19 % (4.1 metal a pass) while demand fell 8 %: its make-up moved to the army, which was
the governor's purpose, and the extractors behind the income did not come.

- **THE BUILD POWER is deleted** (owner's decision, Q1). It was the cause: it held the constructors' summed
  workertime within 24 × the last pass's metal income, and the constructors build the extractors that make that
  income, so the rule fed back on itself. Past 9 000 it was the only rule refusing a constructor in 50 % of the
  samples and refused together with the army's share in 8.5 %; the seats kept 2 / 3 / 4 constructors at 15 000 /
  21 000 / 27 000 against 6 / 9 / 11 before the governor, and 6 / 8 / 10.5 extractors against 8 / 12 / 14. The
  opening, the army's share and the pressure stay. **What it costs**: outside a hold the constructors are bounded
  by the army's share alone; at its limit (two army units a constructor) the constructors take about 60-66 % of
  the factory-ticks at T1, and in the brain test's loss loop -- a constructor lost every decision -- the governor
  settles near two constructor orders to one army order where the build power held one to one.
- **THE HOLD**, one rule for every level in the plan: while the stored metal is below `hold_pm`
  (200) thousandths of the storage, outside a surplus and outside the pressure, at most `hold_fac` (1) factories
  are busy, no plant past the first is started, the assist helps only the economy's frames, and a helper on
  anything else is given the ladder or stopped ([the brain](brain.html), *The military*, the governor). **Why**:
  the engine pays a seat's requests at one fraction, so only what is asked decides what is paid, and in the new
  stalls the factories and plants asked 80 % of the demand and the economy's own buildings 15 %.
- **Measured with both, 2026-11-14** (the same 46 bar games as the `new` group, 62 ImpAI seat-games, GDI): the
  stall share of 9 000..27 000 fell to 16.8 % against 44.4 % (M2's own tool gives the same), income rose to 18.7 a
  pass against 17.3 and demand fell to 18.9 against 22.1; at 15 000 / 21 000 / 27 000 the seats held 3 / 5 / 8
  constructors and 7 / 10.5 / 13 extractors. The factories worked on army in 55.6 % of their passes, between
  the governor's 71.9 % and the 43.5 % before it, and the economy's own buildings asked 23 % of a stall's demand
  against 15 %. In the owner's Samson-line game the army took 53.7 % (Easy) and 50.1 % (Normal) of the
  factory-ticks after the first four constructors, against the governor's 62.8 % and 69.5 %; `route_check` found
  no violation. `eco_bench` against the OTA AI: 5 of 6 pairs, against 3 of 6 with the build power (Pincushion's
  first start still below, 18.6 against 20.3).
- **Ruled out, MEASURED**: the surplus buy and the poor plants (none of 318 stall episodes followed a store at
  80 % within 900 ticks); the towers (0.12 metal a pass); the commander helping factories (0.9 a pass against 4.5
  before the governor). Fewer constructors is the mechanism above, not a separate cause.

The design calls, each with its evidence; the owner may reverse any of them:

- **The pressure lifts the hold.** A pressed seat needs its army; the hold reads the seat's own stamp, no other
  seat's state.
- **The hold refuses no constructor.** Constructors are the economy's growth, and the measured stall is one of too
  little income; the store sat below 20 % of its storage in runs of 239 passes at the median (p90 543), so a rule
  that held constructors in a hold would hold them for most of the game and recreate the loop it replaces.
- **One floor factory, not 0.** A hold can last while a plant's builder finishes it, and the army must not stop
  for that long; with 0 the store refills sooner and the army waits. The bar may move it.
- **The store alone, not the stall.** The stall test also needs income below demand, which turns false the pass
  after the hold cuts the demand, so the factories would be released into an empty store at once.
- **A plant begun before a hold is finished by its own builder.** Stopping it would leave a frame no rung resumes,
  which would block the next plant.
- **Seen air is not held**: the anti-air structures are a reaction to a seen threat, like the pressure.

## The mend: damaged army units are repaired at the rally point (2026-11-14)

M1 measured that a conservative 16-21 % of the army metal ImpAI loses to enemies would be kept were its units to
leave the fight below half their health (17 % in the newest builds); ImpAI had no per-unit retreat. The owner's
decision of 2026-11-14: **army units below 50 % of their health retreat to be repaired, unless they are the last
defenders** ([the brain](brain.html), *The military*, step 8, and *Task allocation*, the repair). Within that rule,
the coordinator's answers to the spec's questions:

- **Aircraft are left out** until A3d brings ImpAI air: it builds none yet, and aircraft repair at pads through the
  engine's own rule below about 75 % of their health, a path of its own.
- **Constructors are left out**: they are not army, ImpAI loses few (M1: 21 units, 3 765 metal in 57 seat-games),
  and the walk check already turns them back from known threat.
- **A mender nobody repairs rejoins after 30 s** (`mend_wait`, 900 ticks) and mends again only once hurt below its
  health then (the mark). Waiting until repaired would hold a unit out of every fight on a seat whose constructors
  never idle.

The design calls, each with its reason; the owner may reverse any of them:

- **The return is at full health**, not a hysteresis such as 0.9: RepairUnit `0x405300` ends by itself only at its
  target's full health or death ([weapons](../engine/weapons.html), the emitters' table), so a release there
  leaves no repairer chasing a unit the squads walk away.
- **The haven is the rally point, and a constructor comes to it**: the repairers are the economy's moving
  constructors -- in retail no static repairer but the factories, which repair nothing, and pads serve aircraft
  only -- and RepairUnit walks to its target and repairs it only standing. Not the anchor, whose surroundings are
  the base ring's sites.
- **The commander does not repair**: a repair head names a unit, so nothing of the economy's could end its chase of
  a mender the military then sends into a fight.
- **The cap is half** a squad's value present and mending, and **the last defender** is counted against healthy
  defenders only: a squad never flees as a whole, and a base threat never takes the base's last army defender to
  the haven.
- **Repairs come before the ladder**, at most `repair_max` (2) constructors at once and none in an
  energy stall: an idle constructor is tasked at every build it finishes, so after the ladder a repair would almost
  never find one.
- **No repair in the hold** (G3's, above), the conservative reading: a repair asks energy and no metal, but the hold
  keeps the seat's build power for the economy's buildings while the store is short, and a constructor at a repair
  is one the ladder does not get. A mend waits the hold out; a mender it outlasts is released after `mend_wait` with
  its mark and fights on. The pressure lifts the hold, so a pressed seat repairs.
- **BARb is the idea, not the code.** Retreating to a repair point at a share of health is BARb's builder retreat
  (`"builder": [0.85, 1.0]`, `hard/behaviour.json:47`); CircuitAI is GPL v2 and Impure is MIT, so no code is copied
  or translated, and the rule and its numbers are ImpAI's own.
