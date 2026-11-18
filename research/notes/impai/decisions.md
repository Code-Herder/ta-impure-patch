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

- **ImpAI sees what a human in its seat would see.** Under Permanent or Circular LOS its view uses the seat's own mapped bit, over the local player's bit the engine reads (`0x408090`, `0x47D2E0`), which would show it the human's exploration. Its radar and sonar contacts are its seat's acquisition's (`tagpu_seatcontact_scan`), never the viewed player's. Submerged enemies are located only by the seat's own sonar and never identified, and an ally's units are position-only when the seat receives the ally's contacts (allied sharing), as the engine shows a human; both stand until the naval and alliance work.
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
- **A submarine is submerged wherever it can go** -- decided 2026-11-14 ([impai](../modules/impai.html), *The
  fight predictor*). A sea type with a mover that is `upright`, or no `floater`, stands on the sea floor (the
  altitude fix-up `0x48A870`), so the content's `SUBMERGED` no longer asks whether its top is below its waterline,
  which plays no part there; a floater and a structure keep that test. **Why submerged everywhere**: on the floor
  a type is under water wherever the water is deeper than its top, and the content is one flag a type; every type
  of that class in retail, Total Mayhem 11.3 and TA Zero is a submarine of top 9.7 to 42.5, and the naval
  arena's water is 38 deep or more. **What it costs**: such a type in water shallower than its top stands surfaced
  and is predicted out of reach of every weapon but a water weapon. The retail content's digest changed (only
  CORSSUB flips there), so the coefficients were refitted at the cross-validation's choice; each moved by under
  0.004 and the check reads 114/122.
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
- **Builders keep to their layer too.** An assist's target must be reachable on the builder's class
  (`impai_build.c`, `builder_near`), which widens the rule beyond the military. **Why**: a sea plant makes sea
  constructors; without the test a sea constructor walks at a plant inland to help it. On Comet Catcher every
  block is one component for every class, so the test does not change `impai-eco`'s choices.

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
- **The harvest keeps to held ground, and its walks are checked**, 2026-11-15: a constructor takes no feature on a
  cell the seat does not hold, the extractors' rule, and the walk check covers a RECLAIM_GO and a RECLAIM still out
  of its build reach as walks and a RECLAIM in reach as work. **Why**: the army's look finds wrecks where the armies
  fought, the band where extractors lived 4 470 ticks against 11 010 or more elsewhere, and nothing turned back a
  builder already walking to one when the next push arrived. **What it costs**: a rich wreck field
  past the front waits until a push holds its ground; the commander keeps `comm_r`.
- **The held ground's enemy side starts from each enemy's base**, 2026-11-15: every identified enemy structure that
  is not a tower, and while an enemy the seat identifies has none known, or none is identified, every start but the
  seat's own -- not the military's estimate. **Why**: the estimate is one value-weighted centre of every enemy's
  structures, and in a free-for-all two enemies on opposite sides put it by the seat's own base, so the middle seat
  held its anchor's cell and its army's and claimed no patch. **What it costs**: on a map of more starts than
  players, an empty start counts as an enemy's until each enemy's structures are known, and the ground near it is
  held only within three sevenths of the way.
- **The orders before the factories' leave them their roots.** The job checks' stops, the walk checks' turn
  backs and the ladder spend at most `roots` less one for each idle factory, up to `queue_max`. **Why**: a raid
  that turned back eight builders spent every root, and no factory queued that decision (the brain review's
  L4). **What it costs**: a decision with idle factories gives the builders at most `roots` − min(`queue_max`,
  idle factories), four at the least at every level; one with none keeps all eight.
- **A threatened site does not grow the ring.** A ring whose sites were left for ground threat, at the site or
  on the route, for the commander's reach or for its safe distance is not exhausted: growth answers only a lack
  of room. **Why**: a raid over the base for a few decisions grew the ring toward eight and passed a plant type
  over for `fail_t`, so the seat built another plant type (the brain review's M1).
- **The commander keeps its safe distance in every economy task.** No task sends the commander to a point within
  600 px (`IMPAI_CMD_SAFE_PX`) of a seen armed enemy: a ring site, an extractor patch (the opening's far patch
  included), a frame to help, a feature to reclaim or walk to, the sea plant's site. One predicate decides
  (`cmd_unsafe`), refused at every task order to the commander (`build_at`, `order_mode`, `reclaim_order`) and
  skipped by each search, so another candidate is taken; a walk to a remembered feature that comes into sight
  there ends with no failure. **Why**: the military flees a contact that close, and the economy took the
  commander back to the same point once the window lapsed, a cycle of 300 ticks -- the brain review's L3 on the
  ring, and the coherence review's item 8 for the spot, the assist and the reclaim, which kept to `comm_r`
  alone. **Why one predicate at the order**: as `comm_r` is held in `build_at`, a path added later cannot send
  the commander past it; the searches skip first only so that a refusal costs no candidate. **Why a walk past
  `comm_r`**: the decision lists the enemies within `comm_r` + 601 px of the anchor, which holds every enemy that
  close to a point within `comm_r`; the opening's far patch lies up to twice `comm_r` out, where the list would
  miss one, so such a point walks the view's rows itself. **What it costs**: near a contact the military would
  stand against, the commander's tasks there wait for it to leave; its moves of no job -- the turn back, the
  yard's step -- are never refused.
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
- **On an island the plant order is a calculation, not "sea, then air"** (owner's decision, 2026-11-17; it replaces
  the fixed order below): the sea first when there is metal in the water -- the open water patches a sea plant's
  constructor reaches are at least the island's -- or sea defences are needed (an enemy ship or submarine known);
  a land plant first when the island is big enough that land constructors pay (at least six open patches its
  constructors reach and 1 024 blocks of its ground within 1 536 px of the anchor, both the brain writer's
  hand-written figures); never an air plant first, and aircraft eventually: past the first plant the sea plant
  until the seat holds two, then an air plant. The owner: "Air should be built eventually but probably not as a
  first plant on naval maps if there are metal extractors in water. Maybe the AI has to calculate if it's better to
  go air or water first for resources / defences. Air first is often a bad idea and extremely hard to pull off even
  for a human. Whether you have a land plant first really depends on the size of the island to know if it's worth
  it." **What it costs**: the default table buys no aircraft, so the air plant makes constructors until an air
  share is added for seats whose enemy land cannot reach (in scope, not done); the island's area is read near the
  base only.
- **The air plant for the blind** (owner's decision, 2026-11-17): on an island the air plant may come ahead of the
  second sea plant -- never ahead of the first, never the first plant -- while (a) an enemy area needs the air
  scout (*One air scout a seat*), (b) the seat safely holds the water and (c) the economy pays for it without
  stalling the yard. The owner: "When blind AND you safely control the water. Not much upside to seeing the island
  if you're losing the water war. We need a heuristic that is not too safe but also a bit cautious, so that an air
  plant won't hobble your naval forces in the early game." **The heuristic** (the brain writer's figures, the bar's
  to move): (b) the seat's sea army value -- built, building and queued -- is above 0 and at least 1.5 times the
  most enemy sea army value seen in the last three minutes (held, since the picture counts a ship only while it is
  seen); a lost prediction does not count against it, since an island seat's retreats come from what stands ashore
  -- on the naval turtle a retreat from the commander at its shore came 127 ticks before the second plant, no enemy
  ship ever seen (MEASURED 2026-11-17 at b54d27f3) --, which is what the air scout is for; (c) no metal or
  energy stall, not in the hold, and a steady metal income of `factory_per_m` (8 a pass) or more, the plan's own
  pace for a further plant, so the air plant never takes a slot the surplus alone would have paid. **What it does
  not reach**: a seat that never builds a sea army, or one whose income stays under 8, keeps the old order (air
  past two sea plants); and the air plant never makes the yard wait: with no free builder offering it, the sea
  plant comes as before. **The figures are kept** (owner's decision, 2026-11-17): re-fly after 2 700 ticks, the
  water held at a sea army of 1.5 times the enemy sea army seen in three minutes, the early air plant at a steady
  metal income of `factory_per_m` (8).
  **The air plant takes the energy builder** (owner's decision, 2026-11-17): a due air plant for the blind takes a
  builder off an energy build -- wind, solar, any energy type -- unless the energy store is in a stall. On the naval
  turtle only the commander offers the air plant and it was on wind generators whenever the plant was due, so
  before this the turtle got no air plant (MEASURED 2026-11-17 at 73593317). The energy frame stays where it was
  ordered; a builder on any other build (an extractor, a plant, a defence) is not taken, and in a stall the air
  plant for the blind does not hold at all. **The take is for the air plant for the blind only** (owner's answer,
  2026-11-17): no other plant takes a builder off an energy build. Before the energy builders, every free builder
  that offers the air plant is asked -- the idle ones, then the helpers --; the first that places it gets it.
  **Its own site search** (owner's answer, 2026-11-17): on the naval turtle the commander's base ring at 8 587
  held 24 sites, 20 tested, none taking CORAP (none past its reach, none threatened: the base ring is full at its
  first size), so the second shipyard came instead and the seat wanted no further plant before the cap at 20 000
  (MEASURED 2026-11-17 at 38efc351, a diagnostic build). The owner: give the air plant for the blind its own
  site search outside the base ring -- a safe, unthreatened site the builder can reach -- and do not hold the
  shipyard up for it. **As built**: the rings from the base ring's present size out to eight more round the base
  box, nearest first, each site one the builder reaches -- tested before the site test, so a builder that reaches
  none of them (an idle constructor on another landmass, asked first) spends none of the search's site tests and
  ends its own walk after 32 such sites -- and every safety test of the base ring's held (no ground
  threat at the site or on a constructor's route, the commander's reach and safe distance, the way out); the
  search spends 32 site tests a decision of its own, never the decision's, so the sea plant's search after it
  keeps all of them, and it never grows the base ring. When no builder places the air plant, the first builder
  asked for a plant is asked the sea plant's question in the same decision -- the yard is never held up and the
  want never goes unanswered. **What it does not reach**: the search walks the same sites each decision, so on a
  map where its first 32 tested sites all fail the sites past them are never tried. On the naval turtle it does
  not bring the air plant: at 8 587, with the budget lifted to 600 in a diagnostic build, the commander's rings
  0..4 offered 136 sites within its 1 024 px leash and the engine's site test refused every one, and rings 5..8 lay
  wholly past the leash (MEASURED 2026-11-17 at d9d34dcb's tree); a CORAP set down 160 px from the commander at
  the game's start stands and works, so the island's room is gone by the time the plant is due. Only the commander
  offers CORAP there, so a site past its leash, or the air plant earlier, is the owner's call.
- **A commander at the shore** (owner's decision, 2026-11-17): ships treat an enemy commander they cannot reach --
  one confined to land, whose class has no way to the ships' water -- like a defence: they stand off past its D-gun
  reach plus a pad and shell it from range; they do not retreat home for it. **As built**: its reach is the longer of
  its ground reach and its D-gun's range; the stand-off measures it from the block of the commander's component
  nearest the member, not from where the commander stands, so no walk along its shore brings a ship into reach;
  the member moves out when inside reach + 64 px and attacks the commander only when it is in the member's own
  range from where the member stands. Every other order an ATTACK gives is kept out of that reach: its travel
  point and its fight's moves are moved out past it (the review of 82cab0f3 found that holding a member where it
  stood froze every ship near any coast of the commander's landmass, so the hold was replaced by this), the
  arrival is measured at the moved point, and the fight attacks no target it would have to close on inside it.
  The stay-or-go reads such a commander as one that does not close on an ATTACK squad only when every member of
  the squad stands it off, and then a contact's other members still close by their own test. **Scope kept**
  (owner's answer, 2026-11-17): as built, for any ATTACK squad that cannot reach the commander -- ships ashore,
  and a land squad facing it across water. **What it does not reach**: a commander that can walk to the ships
  (amphibious into their water) is left to the fight as before; only ATTACK squads stand off; a commander farther
  inland than the ships' range is not shelled, and the squad moves on from the stand-off line -- a ship stands at
  least the commander's reach + 64 px from every shore block the commander can walk to, so it hits only what lies
  within its own range less that of the shore (a destroyer of 660 px against a D-gun of 240: about 356 px inland);
  a commander in a bay, or behind a strait narrower than its reach + 64 px, leaves no clear point at all: a ship
  sent there stops where it stands when that is clear and otherwise keeps its order, its arrival counts as made
  and the squad moves on, so that commander is never shelled (the brain test's shore-commander case (j)); the
  stand-off's own attack on a defence is not held out of the commander's reach; the engine's own path to a firing
  point is not checked.
- **Q3 on an island: the land army is capped, and the plants past the first are sea or air plants** (owner's
  decision, 2026-11-17). When the military judges the seat on an island (THE ISLAND: on a naval map the land army
  reaches no start that can hold an enemy and no enemy known), the land factories build only a base-defence
  budget, and every plant past the first is a sea or an air plant. Q3 stays as it is on every linked map, so a map
  of three or more players keeps its land army. **The cap** (the brain writer's choice): the land army classes
  take an order only while their value -- built, building and queued -- is below a quarter of the whole army's,
  so the base's defence grows with the army the seat can send and is nothing while it has none; the base meets
  only what crosses the water, hovercraft and amphibious units, which the sea army and the air meet first. A
  share and not a fixed value, so the bound means the same at minute 10 and minute 60. **The plants**: a further
  plant on the island, on the pacing of any further plant, follows the island's plant order (above; it replaced
  "first the sea plant, then the air plant" on 2026-11-17): the sea plant by the sea plant's own search
  while its refusals are not spent, and the plant choice's air plant only once a search finds no site at all or no
  builder of the seat offers a sea plant -- one builder that cannot place it now leaves it to the others. Sea before
  air because the default table buys no aircraft, so an air plant there makes only constructors. **Both wait for
  the economy's own reach to agree** (the brain writer's call): the plants only when no plant the choice may take
  has a land army that reaches, the cap only when no army type of a gated class on the seat's factories' menus
  reaches -- both aimed at the estimate and the foe's starts together, so an estimate first taken from structures
  on a lake does not stand for an enemy base the land army reaches (the brain writer's call on review 1001266a's
  MED-2). The military's
  verdict reads a row on a block the land army does not stand on as off the land, so it came and went on Trout
  Farm, a linked map: at GameTime 12 517 the seat's second plant was an air plant beside a lab whose army reached,
  and the win came at 54 559 against 45 445 (MEASURED 2026-11-17, `bar-hota-troutfarm-ca-1` at 31ada7aa; with
  the agreement the lab again, and 45 445).
  **Why**: on Gods of War the island seat's vehicle plant, its two constructors and its one Leveler cost 1 242
  metal by the cap, 1.4 of the destroyers it had three of (MEASURED 2026-11-17, `exploit-naval-turtle` at
  cc27da06); with the rule its second plant was a second shipyard at 8 527 and it held four destroyers at the cap
  (the scratch build of the change). **What it costs**: an island seat whose menus offer no sea plant builds no
  land plant past its first; a linked map misread as an island (THE ISLAND's gaps, [brain](brain.html)) loses
  its land army to the cap.

The first bar runs of the plant choice (13 games, 2026-11-14) found three faults. The fixes are design calls within
the approval, each with its evidence; the owner may reverse any of them:

- **Unlocks.** The candidates include the plants that only a constructor the seat does not have yet can build, when
  one of the seat's own factories makes that constructor. When such a plant wins, the governor's next constructor
  is that type. **Why**: 6 of 7 CORE seats opened on CORVP, held CORCVs only, and teched up to CORAVP at −0.396;
  CORALAB (+0.517) was never a candidate. **What it costs**: while the unlock is wanted, a factory that does not
  make it gets no constructor. The governor's rules still bound how many constructors there are, so the
  unlocks are candidates only below `cons_max`: at the cap no constructor can come, and the seat takes the best of
  its own plants rather than wait for ever. The other rules can still hold an unlock back below the cap (the
  army's share and the pressure), and the plant waits while they do: a stated gap. The cap is the governor's whole
  room, `att_eco` included (*The brain's seams*, below).
- **A tech-up whose best candidate scores below 0 still goes ahead** (owner's decision, 2026-11-14). The score
  ranks the plants' armies; the tech-up also brings the tech-2 constructors and economy, and the army orders still
  go only to the plants whose units are good here, so a plant with a poor army makes constructors.
- **Q3 applies per class.** Q3's fallback (read without the reach) now applies to each gated class no type of the
  seat reaches in. **Why**: one amphibious unit that reached on Trout Farm's advanced plant switched Q3 off for the
  whole seat. The two vehicle plants then sat poor for 726 factory-decisions.
- **A poor plant buys army in a surplus**, its cheapest type of any class. **Why**: the governor promises that no
  factory idles beside a full store, and part 4's "or nothing" was about the metal a poor army would cost, which a
  full store does not lack.
- **The reach targets every start that can hold an enemy's base until an enemy structure is seen** -- the foe's
  starts, the military's and the held ground's rule: not the seat's own and, where the level seated its starts,
  neither left empty nor an ally's -- and a class reaches when it reaches one of them. A class's component at the
  base is the label most blocks within 8 rings of the anchor carry, not the anchor's block alone.
  **Why**: ImpAI's guess, the nearest other start, sits across the water on Trout Farm, while the enemy there was
  land-linked, so the hover plant was chosen for a route the map did not need. Separately, the anchor's impassable
  block, read as no evidence, made CORREAP "reach" where CORGATOR did not. The foe's starts since 2026-11-17: an
  empty start on the seat's own island of Gods of War made every land class reach, and the seat opened on a
  vehicle plant whose army reached no enemy (`exploit-naval-turtle` at 5e4e9c24). **What it costs**: at random
  starts a seat whose real enemy sits on the one start cut off from it builds land plants until it sees an enemy
  structure, and an ally's start is a target.
- **On an island the first plant is the sea plant when the island's plant order says sea** (brain writer's call
  within Q1, 2026-11-17; the order is the owner's, above). With the foe's
  starts as the targets, on an island no plant on the commander's menu has a land army that reaches, and Q1 then
  takes the least value of all; a water plant is no candidate on a dry anchor only because the ring holds no water
  site, so the sea plant is placed by its own search instead, when the military judges the map naval. A builder
  that offers none builds the land plant as before, and the sea gets one attempt: once a refusal of the sea plant is
  counted -- a search that orders none, a job that ends unbuilt -- every later decision builds the land plant
  (coordinator's call on review d8799688's MED-1, 2026-11-17: before it, a sea plant whose jobs kept failing held
  the first plant for up to sixteen `fail_t` windows). **Why**: on Gods of War the seat's vehicle plant came at
  2 974 and its shipyard on the pacing at 9 552, and at the cap it held two ships and had attacked nothing. **What
  it costs**: the island seat's land economy has the commander alone, its plants past the first being sea or air
  plants (Q3 on an island, above); a seat whose one sea attempt fails builds the land plant one failed job late.
- **On an island the waves of every layer but land go at the finish's size** (brain writer's call, 2026-11-17):
  while the seat's land army reaches no start that can hold an enemy's base and no enemy row on a naval map, a sea
  or air wave goes at 1 + its layer's waves median units, still predicted to win, inside the attention. **Why**: the
  finish's own reason -- the land army has nothing it can reach --, and the 8 median units a sea wave asks never
  came on Gods of War. A start counts by the ground within 8 rings of it, the economy's reach of a target, and at
  random starts, once enemy structures are known, an occupant-unknown start is no evidence (review d8799688's LOW-1
  and LOW-4). **What it costs**: a row on a block the land army's classes do not stand on reads as off the land; at
  random starts with two enemies, one enemy's structures seen hide a second enemy's base on the seat's own land
  until a row of it is seen, and the sea's waves then go small.
- **The scout is the first layer's that has a start to go to**, land, then sea, then air (brain writer's call,
  2026-11-17). **Why**: on Gods of War no land unit reached a start, so no scout went and the seat saw no enemy to
  the cap. A leg is arrived at where travel sends the scout, as a squad's arrival reads it, and a leg with nowhere
  to travel to is over at once. **What it costs**: a ship's scouting trip ends at the water nearest the start, which
  may lie past its sight of it.
- **Scouts are cheap, fast, bought first and expendable** (owner's decision, 2026-11-17: "we can use cheap scouts to
  see what is on the island and then shell it from memory or use that to attack. On land we use weasels or
  equivalent [...] and on water it's ships like skeeters [...] Scouts are expected to die and go as deep as they
  can."). Each surface layer has a scout class by the classifier's role -- a mobile armed type of role SCOUT: the
  land scout (`lscout`: ARMFAV, CORFAV, ARMFLEA) and the sea scout (`sscout`, new: CORPT) --, and its plant buys one
  before any other class whenever the seat holds none of it, built, building or queued, so the first unit of a layer
  is its scout and a dead scout is replaced by the next order. **The scout is one** (brain writer's call, the same
  day): no other buy takes a scout class -- the table's share, 0.05 on each surface layer, is only the switch --, so
  a layer holds one scout at a time. **Why**: a share is a share of what the layer can build, and at sea, with the
  destroyer the only other class a seat's targets allowed, 0.05 became a sixth of the navy: five CORPT beside two
  destroyers on Gods of War (MEASURED 2026-11-17 at bf92df19). **What it costs**: the land army no longer carries
  the jeeps the share bought as fighters (16 of 64 army units on Steel Jungle `ca-1` at d8799688). A scout's trip no longer ends at its first contact: it walks its legs to the end, so on the sea a
  leg to a start ashore is a peek at the shore, and the next leg pulls it away. What it sees becomes the seat's rows,
  and the army shells a remembered defence from them (the stand-off's ground attack, bounded by the unreached ring).
  **Not the rule**: blind fire at a start never seen -- the owner calls it an option; scouting comes first.
  **The replacements' cost is accepted** (coordinator's call on review 133c30c2's LOW-1): nothing bounds how often a
  dead scout is replaced but the one built, building or queued a layer at a time, so a seat whose scout dies on every
  trip pays a scout's price every trip -- what the owner's "expected to die" buys. A cooldown waits for a game that
  shows the cost: 3 to 9 land scouts a game in the confirm set at bca430ab.
  **Repair stays removed** (*The mend: removed*, reconfirmed by the owner on 2026-11-17). **What it does not reach**:
  a seat whose side has no role-SCOUT type on a layer (ARM's skeeter, ARMPT, is a raider by the classifier's
  thresholds) buys none there, and its scout is then the cheapest of the layer's fastest quarter, as before; a sea
  scout's eyes reach no farther than a destroyer's -- under stock's `numtables` the engine holds every unit's sight
  to 8 blocks, 256 px on an axis ([engine/map](../engine/map.html), *The sight table and the stamp*) --, so on Gods of War it sees no farther
  inland than the navy (the
  naval turtle: [the bar](bar.html), *The exploiter suite*).
- **One air scout a seat** (owner's decision, 2026-11-17: yes to one unarmed air scout per seat, an explicit
  exception to *No army class without damage*). An aircraft of role SCOUT that deals nothing -- CORFINK, ARMPEEP,
  the AWACs, the seaplane scouts -- takes the air scout's class, and the seat buys one, under *The scout is one*,
  while a known or expected enemy area -- a start that can hold an enemy's base, or the base's estimate -- is out of
  sight of every land and sea army unit the seat has: none reaches it, and none's nearest point of its own
  component has it in its eyes -- the engine's sight table, at most 8 blocks (THE BLIND SPOT). The scout flies
  there first, beside any scout of another layer, and what it sees becomes rows the ships shell from memory (the
  stand-off's ground attack). The air plant stays never-first. **Why**: on Gods of War the enemy commander and its
  west tower stand ~370 and ~415 px from the nearest water, past every ship's eyes, so nothing the seat bought saw
  them to the cap (MEASURED 2026-11-17 at bca430ab). **The air scout is scout only** (owner's decision,
  2026-11-17): it is never in a wave, a defence or any army sum; between trips it waits at home in no squad. **Re-fly
  only when needed** (owner's decision, 2026-11-17): it flies only to an area no land or sea unit sees and that it
  has not had in its eyes for `IMPAI_EYED_REFRESH` -- 2 700 ticks, 90 s, the writer's figure inside the owner's
  "about 1-2 game minutes" --, each leg to the nearest such area, and home when none is left; so a start that stays
  empty (random starts, or one an ally watches) costs one look a refresh, not a trip at every decision. Only a unit
  of the air scout's class makes the trip: a seat without one sends none of its aircraft, which would otherwise
  detach a fighter to a blind area at every decision. **The enemy's air scouts stay out**: the exception is for the
  seat's own buy, so an enemy air scout that deals nothing is never what a radar dot counts as (*the substitute*)
  and counts in no class of the picture, as before the class took it. With it in the substitute's median, on
  bar-nota-lavamania-ca-2 under ProTA nine radar dots were weighed as air scouts, the stay-or-go retreated 344
  times against 164 and the game reached the cap where the brain before the class won at 57 495 (MEASURED
  2026-11-17; the brain with the rule, built beside *A commander at the shore* at c7f40dd4, wins it at 37 242). **What it costs**: an enemy air scout presses
  nothing (THE PRESSURE takes armed members only), and the fight content's digest changes with the class, so
  `predfit check` needs a refit to the same coefficients. **What it does not reach**: the turtle's seat gets no air plant before the cap (its second sea
  plant came at GameTime 8 587, metal income 9.7 at the cap, MEASURED at bca430ab), so there the scout waits on the
  economy.
- **A type is bought only if its weapons can hit a target class the seat knows or expects** (coordinator's call,
  2026-11-17, approving the brain writer's proposal). The classes are dry, floating, under the sea and air, as the
  engine's can-engage test sorts a target; known are those of the enemies the military counted within 5 400 ticks,
  expected the ground of every start that can hold an enemy's base. With no evidence nothing is narrowed, and with
  something known but no such start dry is kept beside it, so an aircraft or a ship alone never narrows the land
  away (review 1fa981c3's LOW-3). The plant choice scores a plant only by the types that pass the same filter, so
  it never takes a plant for a unit its factory would not buy (LOW-2). **Why**: on Gods of War the island seat's
  shipyard made two submarines of 1 199 metal each while every enemy it knew stood on land. **What it costs**: a
  submarine waits for a ship or a submarine the seat identifies -- a radar contact on the water counts as dry --;
  the anti-air classes wait for an aircraft known, which the default table's shares already did. Two misreadings
  (LOW-1): a land unit wading below the sea, no floater, counts as dry though a waterweapon hits it, and a landed
  aircraft counts as air though an anti-air weapon cannot hit it; both stand until the unit moves, and neither
  is worth a read of the unit's height.
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
- **The base's defence is exempt from the attention** (the owner's decision, 2026-11-14, "Defence exempt"): a
  squad holding DEFEND does not count against `att_mil`, and attacks and raids still do. **Why**: the attention is
  how much a slower player keeps going at once, and a player answers a threat to its base whatever it has out; a
  seat that let its base fall because two waves were away would be weaker by a different plan, not a slower one.
- **The commander keeps an action under the budget**: the squads never spend the military bucket's last action,
  the reserve held below the persona's burst so a full bucket always has one to spend. **Why**: the commander's blast horizon counts on the next decision moving it
  (`look_mil` + `react`), and an empty bucket at Easy could hold it 225 ticks against the horizon's 135. **What it
  costs**: one action less for the squads at the end of a decision, and a burst of 1 keeps no reserve.
- **The hold is the plan's, at Hard's values** (2026-11-14): `stall_pm` and `hold_fac` decide *what* the seat
  does, so every level plays them alike. **Why**: a decision that differs by level is a weaker plan, which the
  personas replaced.
- **The hold's orders go through the budget**: a hold's release is the economy's order, given through its funnel,
  and with a bucket the idle builders leave the decision's last root to a helper the hold must release, so a slow
  persona cannot leave a helper on a factory's work for good. **What it costs**: at a low bucket an idle builder
  waits a decision for its order while a helper is released.

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
- **THE HOLD**, one rule for every level in the plan: while the stored metal is below `stall_pm`
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

## The mend: removed (2026-11-15)

The mend sent an army unit below half its health out of its squad to a haven behind the base, where a constructor
repaired it, unless it was the base's last defender (the owner's rule of 2026-11-14, after M1 estimated that 16-21 %
of the army metal ImpAI loses to enemies would be kept by such a retreat). **It is removed, with everything that
existed only for it** -- the mend and repair rules, their job kinds, the economy's offer to the military, the plan's
`mend_hp`, `mend_wait`, `mend_roots` and `repair_max`, their counters and log fields, the `impai-mend` test group --
and no switch is left to bring it back. **Why**: it never showed the value it was built for, against the same games without it. Over
its first value runs (13 seat-games against OTA AI) the loss did not fall within the interval (lost −164 [−349,
+24], killed +14 [−160, +196]), and a larger repair cap only raised it (`repair_max` 3, 4 and one scaled with the
constructors, each against 2 over 60 Hard seat-games). Redesigned -- the haven behind the anchor, the gate on a
repairer that could serve, the cone, the yard rule --, it failed the owner's paired outcome bar at bb11df10 on both
of its terms ([the bar](bar.html), *The paired outcome bar*; 60 pairs against OTA AI): lost −42 [−131, +42], killed
−37 [−104, +18], the killed bound −25.7 % of the base arm's 405 (`.data/impai/runs/retreat/v4/_driver/paired.txt`).
A unit out of the fight is metal that does not fight, and the repairs did not return enough of it in time to pay
for that. A retreat to repair may come back as a new design.
**What stays**: the factories' yard rule for travel's spread points; THE ROUTE, the builders' straight-line threat
test, now with its one rule. The fight's step back of a member under `micro_hp` went later the same day (*The step
back: removed*, below).

## The steady income's payouts and the patches others built on (2026-11-14)

- **A payout reaches only a frame, a factory, a builder or a reclaimer; Twilight's hover plant stays a tech-up** -- decided
  2026-11-14 ([the brain](brain.html), *The steady income*; the brain handoff's item 3). (b) THE STEADY INCOME
  subtracted every non-extractor's metal above its make rate. In TA Twilight (bar-nota-pincushion-ac-1) six
  ARMADVSOL -- metalmake, makesmetal and extractsmetal 0 in the def, 15 extractor types in both the DLL's census and
  the content dump, so the DLL and `steady.py` read them alike -- made 1.34, 2.69 or 4.03 a pass at every sample:
  18.8 of the seat's 27.1, counted as payouts, so the seat's steady income read 8.29. The engine's four payout
  writers credit the reclaimer, the factory tearing down its product, the frame being unbuilt and a reclaimed
  unit's killer ([sim](../engine/sim.html), the handicap's rows 9, 10 and 12), so only a frame, a factory, a unit of
  workertime above 0 or a canreclamate one has its excess subtracted now. **Why**: the rule is the engine's, not the def's; metal a script or a mod credits a structure
  every pass is income. The seven ARMLAB choices at 8.29 were not that error's: the second lab's gate is 8 a pass.
  (a) ARMHP was the tech-up's choice from a tech-1 menu because the DLL's own build tech gave it 2 -- the tech-up
  pass takes no plant at or below the seat's highest (impai_econ.c, THE CHOICE) -- so in Twilight its hover
  constructor's options reach tech 2, unlike retail's ARMCH [INFERRED from the choice; the menus not dumped].
  That is the rule's purpose, reaching what the tech-1 builders cannot, so the build tech's extractor test stays
  as it is. **What it costs**: a payout to a finished unit that neither builds nor reclaims would now be income;
  none of the engine's writers makes one. **Played** (8b7f2059 against a07938b9, the rule before the frame term,
  which only an unbuild reaches; four nota games a setup, `--cap 36000`): on the loader and TADR the four games
  the same to the tick; on Twilight, a setup the judge counts apart (OTA AI completes AI-only extractors), 3 lost
  and 1 at the cap against 2 and 2.
- **A patch another player built on is not free** -- decided 2026-11-14 ([the brain](brain.html), *No patch
  another player built on*). A patch whose box a building of another player's touches, as the seat's rows hold it
  (seen now or a ghost), is skipped as an own claim is. **Why**: the brain handoff's Lava Mania item -- retail
  under the loader and TADR, `failed=13` on each ImpAI seat against 1-8 on the other setups: 10 of seat 1's 13
  failed sites were its CORMEX ordered where OTA AI's ARMMEX stood, the CORCV waiting beside it until the job
  failed, and 3 were the same patch again once the failure lapsed. Every building, not only extractors, because
  the engine refuses the footprint over any of them. **What it costs**: one pass over the view's rows a decision
  that picks a patch, each building against at most 256 patches; a ghost keeps its patch until the seat looks
  again, so an extractor destroyed out of the seat's sight holds its patch until then. **Played** on the
  handoff's four games (loader and TADR, Lava Mania and Pin Cushion, `--cap 36000`, 93178b57 against 8b7f2059):
  Lava Mania ca-1 failed 12 against 9 -- the 4 refusals left each a first visit to an ARMMEX the seat had not seen,
  the other 5 trips that outlasted `job_timeout` 267-660 px short of their site --, the other three games the same
  to the tick; ImpAI won three in both arms, the fourth at the cap. **Gap**: a BUILD job's timeout does not count
  the trip, so a constructor sent to a patch it cannot reach in `job_timeout` fails the site, and with the near
  patches known taken more of its trips are long.

## The army's faults: jammers bought as army, and wins not finished (2026-11-14)

The owner's rule of 2026-11-14: ImpAI always beats the stock OTA AI, and a game it fails to win against an AI that
does not cheat is a bug. Diagnosed from the recordings of the mods check and of the bar's long games: a weaponless
type was bought as army, radar dots were counted as commanders, the army was pinned at home by aircraft, three
factories spent a third to a half of the metal produced, and waves chased outlying structures while the home was
overrun ([the brain](brain.html), *The military*, steps 1-4, and the economy's *Tech-up*).

- **No army class without damage.** A type takes an army class only when the classifier's damage per second,
  ground or air, is above 0. A jammer has no regular weapon, so the classifier calls it a scout, and as the
  cheapest of its class the class share and the surplus's buy took it: 15 CORSPEC alive at the cap of a base-game
  Pin Cushion game, 4.6 % of the army's value at about four times a combat unit's build time per value
  (MEASURED). 17 retail types lose their class -- ARMASER, ARMJAM, ARMMARK, ARMSEER, ARMSPY, CORETER, CORSPEC,
  CORSPY, CORTRUCK, CORVOYR, CORVRAD, ARMAWAC, ARMPEEP, ARMSEHAK, CORAWAC, CORFINK, CORHUNT -- and over the six
  contents no type that deals damage does. **The owner's one exception** (2026-11-17, *One air scout a seat* below):
  the six air scouts take the air scout's class again. The scouting rule still prefers a land scout, now an armed one, and
  takes the fastest-quarter cheapest member when none stands. The class is part of the fight content's digest
  and enters no fight, so the predictor's coefficients refit to the same values under the new digest.
  **Not covered**: Total Mayhem gives its jammers and radar units a 720-damage vertical-launch rocket their scripts
  have no aim script for, so the engine never fires it (`0x49DB7B`'s aim gate); a slot like that is a weapon until
  the content learns to leave it out.
- **The radar substitute is an army type**, never a commander, a builder or a type that deals nothing: ARMCOM was
  the substitute at 12 of 13 retreats on Lava Mania against TADR's install, CORCA and CORCK on ProTA. The air
  classes stay in, since a dot carries no height.
- **A squad weighs only what it can hurt or outrun.** An enemy it can neither hurt nor outrun costs it the same
  whether it stays or goes, so it decides neither the retreat nor the wave. 185 of 200 retreats on a base-game
  Pin Cushion had an aircraft nearest (CORVENG 121), and the army of 70 to 169 units sat at home from GameTime
  41 000 to 72 600. The anti-air answer itself is A3's.
- **A wave leaves against what will decide whether it stays** (brain writer's call, 2026-11-17): its departure is
  predicted against the mobiles seen now that its stay-or-go weighs and every enemy structure the seat holds a row
  for within 1 280 px of the goal, seen or remembered, armed or not -- the side its stay-or-go meets at a base's
  contact -- over the goal's armed defences alone. **Why**: with the two tests weighing different sides, the waves
  on `bar-hota-plainspasses-ac-1` left at margin 0.12 and turned back at −0.45, 50 times, each retreat the whole
  way home, and the game reached the cap with 280 units against 61. **Chosen over** dropping unarmed structures
  from the stay-or-go: a wave's fire is spread over a base's plants, so a stay-or-go that saw only the defenders
  would hold the wave in a fight it is losing.
- **The enemy's plants are a wave's goal**, after the commander seen or remembered and before the estimate's
  most valuable structure: the estimate is a value-weighted centre, and outlying structures pull it away.
- **The recall**: a home that loses its prediction against a confirmed threat takes the nearest ATTACK or RAID
  squad of its layer that can hurt the threat back to defend. Two Trout Farm commanders died at home beside
  waves on the far side; the base confirms the threat, and the squad comes home as a RETREAT (*The brain's seams*,
  below).
- **In a surplus the metal producers wait, past the opening** (the coordinator's decision of 2026-11-14): no
  extractor or maker is started while the store is at 0.8 of its maximum or more, since the engine adds a full
  store's further metal to MetalWasted, but only once the opening has ended on the first factory's terms -- the
  game starts with a full store, and the rule unexempted held back the opening's own extractors. They wait only
  while the income pays for a further plant (*The brain's seams*, below).
- **A seat with an action budget queues up to five an army order in a surplus**, the build button's shift-click,
  and one otherwise ([the brain](brain.html), *The military*, the governor). **Why**: the persona's budget counts
  orders, and a human's shift-click is one action for five units; the Easy seat's plants stood waiting on its 10
  actions a minute beside a full store, 40.6 % of its steady income wasted. Five and not more: the engine's own
  count, `WQ_QUEUE_MAX`, which the WORLDQ refuses past. Not a seat with no budget: it reaches each idle plant at its
  next decision, and the Hard seat given five on Trout Farm grew an army past the military's 512 rows and did not
  finish the game by the cap.
- **The five are what the store pays above the hold's line**, 2026-11-15: the store less `stall_pm` of the storage,
  less the metal already committed past the unit each factory makes -- the busy factories' count nodes past the
  unit in production, as the census reads them, and the decision's earlier orders past their first unit --, shared
  by the orders the decision may still give, at the type's metal cost, at least one; a cost that is not finite buys
  one. **Why**: the hold refuses only new orders, so five-unit queues the store could not pay -- four orders of five
  150-metal units against 800 -- ran the store through the line while they ran, and the economy's own buildings
  were paid at the stall's fraction. **Why the queued metal**: the high review at bb11df10's M-3 -- counted within
  one decision alone, the next decision committed the same store again to the factories newly idle, an Easy seat
  with four idle plants about 2.1 times it over consecutive decisions; the census already walks the count nodes for
  the army's rows, so the queues cost nothing to read. **What it costs**: a full store of a small storage buys fewer
  than five, so the budgeted seat's plants come back to the budget sooner.
- **No army order past the military's 512 rows**, 2026-11-15: the governor's army count (finished units and count
  nodes) refuses an order that would pass `IMPAI_ARMY_MAX`, and holds a five-unit order to what the rows leave.
  **Why**: a unit past the rows is in no squad, takes no order and is weighed by no prediction; at cf3f68c9 the
  Normal seat on Trout Farm left 1 183 army units out over its decisions. **What it costs**: a seat at 512 builds no
  army until it loses some, however full its store.
- **The finish's rows**, 2026-11-15, the whole-brain coherence review at d665a00a's finding 2: on a level that can hold
  the dry hunt, an order of a type with no water weapon stops at 448 of the 512 rows, so 64 always hold units with a
  water weapon or are free. **Why**: the dry menus offer water weapons alone, so a land army that had filled the rows
  when the dry hunt began left the finish's sea plant nothing to make. **Why a reserve and not the sea layer listed
  first**: the reserve is a bound the governor keeps at every order, where listing the sea layer first in the
  military's census would change which units the squads hold each time the hunt begins or ends. **What it costs**:
  the land army stops at 448 rows on such a level, and a water-weapon army bought before the hunt -- a naval seat's
  -- may already hold the 64.
- **In a surplus the factories are paced by what they spend**, past `factory_max`: a further plant when the unasked
  income pays a busy plant's measured request and every factory is busy, and a tech-up due beside it comes first
  ([the brain](brain.html), *The military*, the tech-up's paragraph). The tech-up's own test needs `factory_max` factories of the seat's highest
  tech, which the measured end state, two of tech 1 and one of tech 2, never meets. **Why the measured price**: the
  plan's flat `factory_per_m` of 8 a pass a factory priced every plant alike, so a seat with five held its sixth
  until the income passed 40 whatever its plants spent; a factory's ledger says what it asks. **What it costs**: a
  seat whose plants make cheap units prices its next plant low and builds more of them; a factory between products
  at the census puts the plant off a decision; **an overflow below one plant's price is wasted** until the extractors,
  which do not wait on it, raise it to the price -- the Easy seat on Trout Farm (`bar-eota-troutfarm-cc-1`) spent
  35 of 44 a pass with its store full from GameTime 58 000 to its win at 72 910 (MEASURED 2026-11-14).
- **Far expansions: claim only the ground the seat holds now, and let it follow the front** ([the brain](brain.html),
  *Task allocation*). A constructor takes a patch only on a cell the seat's army, towers or base reach before any
  known enemy strength does, judged by step counts on the threat maps' grid with a hysteresis (enter at three
  quarters, leave past one). **Why claim, not defend**: a defence at each far patch costs a turret's metal and a
  builder's walk for an extractor that makes a few metal a second, and the measured deaths are not at a distance but
  at a place: in the owner's Comet Catcher game the extractors 2 048-2 559 px out lived a median 4 470 ticks against
  11 010-17 250 in every other band out to 4 095, and that band is the ground half way between seats 1 and 3, 4 340
  px apart. **Why from the present situation, not a line between the bases**: the anchor and the estimate stay put,
  so such a line refuses for the rest of the game the ground a push wins; on the bar's Trout Farm games it left the
  seats 7-9 extractors at the end against 16-24 with no limit, the makers making up the gap. The front is where the
  two sides' strengths meet, and the army is what moves it. **Why steps on the grid**: they are bounded (two walks of
  the 128 x 128 grid a decision), exact in integers, and every enemy's units are sources alike, so a free-for-all
  needs no estimate per enemy. **Why the hysteresis**: an army that sways a cell back and forth would otherwise
  claim and refuse the same patch in turn. **What it costs**: a straight step ignores water and cliffs, and a ghost
  holds ground for the enemy until the seat looks again.
- **The held ground is judged once in every decision, at its start**, 2026-11-18 ([the brain](brain.html), *Task
  allocation*). The judgement used to run in the first decision step that asked whether a patch was held, so a
  decision in which nothing asked kept the old bits, and which rung ran decided when the hysteresis stepped: a
  rung that ordered nothing still moved recorded state, and removing one changed three games' state. **Why an
  ordering and not a call kept where it was**: a call kept for its side effect is that fault again; judged before
  any reader, every reader of a decision reads that decision's judgement whoever asks. **What it changes**: the
  hysteresis steps in decisions where it did not, so a cell is gained or let go sooner. MEASURED on the session
  tier against the champion 275d56ca, the same category in every game: Red Triangle ac-1 won at 74 077 by both,
  Plains and Passes ac-1 at 36 363 by both, Core Prime cc-1 at 48 285 by both; Trout Farm ca-1 won at 42 509 |
  47 710, Lava Mania ca-2 on ProTA at 32 216 | 43 160, Metal Heck aa-2 at 37 236 | 40 771 -- three wins later
  and none sooner; Steel Jungle ca-1 at the cap not ahead by both, value 0.85. **What it costs**: the two walks
  and the maps in every decision, where a decision with no asker made neither.
- **The harvest: an idle builder reclaims outside a surplus, the army looks for wrecks, and a reclaim reaches 2 048
  px** ([the brain](brain.html), *Task allocation*). Without them the fields were learned only by one builder's
  look a decision, the army's sightings never entered the memory, the choice reached 768 px, and the ladder
  reclaimed only short of metal or before an extractor. **Why each**: a wreck field lies where the army fought, so
  the army's look is what finds it; a field 1-2 km off pays by its metal over the walk, which the score already
  prices, and 2 048 px is what a builder walks inside the job's 2 700 ticks; outside a surplus every metal a
  builder brings is spent next, and in one the assist spends the store faster. **Which fields are safe** stays the
  reclaim's own rule: no known ground threat at the feature or on the straight way there, which is now tested on
  the candidates tried rather than every candidate, so the longer routes cost at most 66 samples a try. **How many
  builders**: every builder the ladder has nothing else for, each on its own claimed feature, within the
  decision's attention and roots. **What it costs**: one more look a decision; a builder walks up to 2 km to a
  field the army saw, through threat the seat has not seen; a builder that would have assisted a plant reclaims
  instead while the store is below 0.8. **Why a search still stops at its fourth reach refusal**: a search with
  no such cap took exactly the same features in eight games (the bar's six and two Comet Catcher free-for-alls,
  every game bit-identical, MEASURED 2026-11-14), because on Trout Farm an ask whose first four candidates a land
  builder cannot come near has no reachable one after them -- 42 of 87 asks with room in troutfarm-ac-1 --, and it
  spent some 35 reach tests on each such ask. What bounds the harvest there is the store and the lake, not the
  search ([the brain](brain.html), *What bounds the harvest*).
- **A reclaim's worth is its metal held to the room, and a feature larger than the room does not wait for it**
  -- decided 2026-11-14 ([the brain](brain.html), *Reclaim when it pays*). A feature larger than the store's room,
  net of the reclaims under way, is taken at the room's worth and the overflow of its payout is lost. **Why not
  wait for room**: the objective is the metal kept and the games won, not the overflow avoided. A rule that took a
  feature only when its metal fitted the room (one past the whole storage once the room was three quarters of it)
  cut the payout lost from 3 181 metal to 417 on the bar's six games against OTA AI, but the metal kept fell from
  47 747 to 26 139 and four of the six games were won later (troutfarm-ac-1 at 51 010 against 32 601,
  redtriangle 40 192 against 18 747), every game still won (MEASURED 2026-11-14): a store that sits near full
  takes little that fits [INFERRED]. Two
  looser rules each left a game unwon at the cap -- a room counting what the store's net drain would free by the
  payout, and a quarter of any feature's metal allowed to go. **What it costs**: the overflow, 6 % of the payout
  there (troutfarm-ca-1 2 758 of 13 991).
- **The makers are switched all the time, by the engine's own on and off** -- the owner's decision of 2026-11-14:
  "if energy is too low, we selectively turn some off and then back on when needed" ([the brain](brain.html), *Task
  allocation*). Off the least metal per energy first while energy is short or metal full, as many as the shortfall
  or the overflow asks; on the most metal per energy first while energy is spare and metal has room; the store bands
  30-60 % of the energy and 80-95 % of the metal and a 300-tick dwell a maker are the hysteresis. **Why**: a maker in
  an energy stall is paid nothing and still asks its energy, and one beside a full store makes only MetalWasted;
  the owner's seat 3 held its store at 95 % or more 76 % of the time while energy-stalled 30 % of it. The player's
  order and not the stock AI's direct call (`0x408670` calls the state setter itself, every pass, with no band). **What
  it costs**: an order a switch, at most 4 a decision; a maker switched off waits a band crossing and 300 ticks
  before it converts again. **How the switches share a budgeted seat's actions**, by construction: each is one root
  of the economy's decision within `root_cap` (`impai_brain.c`, THE FACTORIES' ROOTS), so it spends the builders'
  share and never the reserve kept for the idle factories, nor the military's bucket, which is apart
  (`IMPAI_SRC_MIL`). The Easy seat on Trout Farm (`bar-eota-troutfarm-cc-1`, `apm_eco` 10) switched 42 times in
  its 72 910 ticks, about one of its ten economy actions a minute (MEASURED 2026-11-14); the same game's win at
  72 910, where the build before the spending work won at 41 946, is left to the benchmark round to measure.
- **No rule keeps a helper's standing point off a yard: the loop it would close was measured absent**, 2026-11-15,
  the high review at cf3f68c9's L-6. The claims keep sites off yards and ways, but a helper walks to its frame's
  builddistance wherever that lies, so in principle THE ASSIST could send it onto a waiting factory's way, THE YARD
  step it off and THE ASSIST send it back. **Measured** on the recall run's recordings (a237d62a), every head change
  of the ImpAI seat's builders over the whole game, looking for a help, then a move with no target, then the same
  help within 600 ticks: none in `bar-nota-troutfarm-aa-2` (Normal, 105 yard moves) nor in
  `bar-hota-redtriangle-aa-2` (Hard, 46). **So no fix**: a rule against an effect that does not occur costs a
  test and a state record for nothing; the yard's own bound (one move a builder a walk) caps the loop at two
  actions a cycle if it ever appears.
- **The finish goes to sea when the land army has nothing left to hunt**, 2026-11-15, the coordinator's direction
  "if the remaining enemy is beyond land reach, make the finish target the layer that can reach it": the military's
  dry hunt (`impai_mil.c`, `dry_hunt`) holds once no enemy has been known on ground land-linked to the anchor for
  `IMPAI_DRY_T` (5 400 ticks) after one was; the economy then wants its one sea plant past `factory_max` and the
  pacing, its search reaching `FAR_PATCH_K` x `comm_r` for a constructor on held ground, and the governor buys only
  water-weapon types. **Why**: `bar-nota-troutfarm-aa-2` reached the cap at a237d62a with 541 units against 19, the
  stock AI's last plants in Trout Farm's lake and its commander on the lake bed, which only sonar sees and only a
  water weapon hits; the seat's land army could neither see nor hurt either. **Why the land link and not the
  sweep**: the anchor's land component is where the land plants' products stand, the test the naval judgement
  already makes, and it is cheap; a remnant the land army could shell from the shore still reads as dry, which
  costs one sea plant. **Why the governor stops the land army**: the military lists 512 army units, and the seat
  stood at 481, so without the water-weapon rule the land plants would fill the rows the ships need. **What it
  costs**: one sea plant and its spending in the end game; a seat that lost its land army to an enemy it then lost
  sight of builds no land army while dry -- but an enemy that destroyed it was seen, and stamps. **The 5 400 ticks**
  are the foe memory's own freshness (`IMPAI_FOE_FRESH`), hand-written: long enough for the sweep to cross the
  seat's land, short enough to leave the finish time before an 81 000-tick cap. **The finish as played**, the same
  day, on the eleven verification games: the plant first went to land constructors, which cannot reach the deep
  water off the west start's shallow shore (225 px from the nearest cell a vehicle stands on, ARMCV's limit 163) --
  so every builder is now reach-tested and the commander, whose class walks the sea floor, may build that one plant
  within `FAR_PATCH_K` x `comm_r`, the leash allowing as much while the hunt lasts (the opening's precedent). The
  submarines then never sailed: eight median units took longer than the hunt, and Easy's two operations were held
  by two land sweeps -- so a finishing wave goes at 1 + its layer's waves median units, and outside the attention.
  **Rejected**: ending the least land operation to make room for the finish (*the finish's room*). Measured on
  `bar-eota-troutfarm-cc-2`: the stock AI's commander had walked back onto land unseen, and with a sweep ended it
  rebuilt nine extractors and two labs, the game reaching its cap (with the room and the finish's end; 73 475 with
  the room alone, 65 176 with the exemption). A dry hunt is the seat's knowledge, not the enemy's position, so the
  land sweep keeps running. **The exemption's cost**: up to two operations past `att_mil` while the hunt lasts, a
  persona's attention exceeded by the finish's waves; they go back to FORM when the hunt ends (*the finish's end*),
  since a sea ATTACK of the finish held one of Easy's two operations for 4 400 ticks after the enemy's commander was
  found on land.
- **The hunt needs evidence off the land**, 2026-11-15, the high review at bb11df10's M-1: an enemy known on a block
  not land-linked within `IMPAI_DRY_T` (`sea_t`), and while dry only an identified row held now ends it -- a
  contact never identified ends nothing, nor, since the high review of d665a00a..fd67c52d, does a ghost or a foe's
  memory. **Why**: a
  land army that has met no enemy is no evidence the enemy is off the land -- on `bar-eota-troutfarm-cc-2` the
  remnants rebuilt on land unseen --, and a blip on the shore ended the hunt and demoted its waves. **What it costs**:
  a remnant only on land the seat has not seen is hunted by the land sweep alone; an aircraft only the radar sees over
  the water reads as off the land, since a blip carries no type.
- **The governor buys for the sea only while the finish has a plant**, 2026-11-15, the same review's M-1: one stands
  or is a frame, or a builder offers one, the refusals are not spent and the search has not found the place empty.
  **Why**: with no water near the estimate the sea search finds nothing, and every land plant's menu filtered to water
  weapons left the seat building no army at all.
- **The frame resumed**, 2026-11-15, the same review's H-1: an unfinished water plant is the sea plant's want while
  the hunt lasts, and the nearest builder that reaches it helps it, the commander within the plant's radius. **Why**:
  the commander begins the plant past `comm_r`, a contact within its safe distance ends the build, no land constructor
  reaches the water off Trout Farm's west shore, and the frame counts as the plant, so no second one is wanted: the
  frame would stand for good. **Why a help**: the frame already stands, and resolver mode 8 answers HELPBUILD for any
  unfinished target ([orders](../engine/orders.html), *What each resolver mode answers*) -- the order THE ASSIST gives
  every helper.
- **The finish's end is bounded by the attention**, 2026-11-15, the same review's M-2: the least valuable ATTACK of a
  layer but land goes back to FORM only while the seat holds more operations than its `att_mil`, never at Hard (no
  `att_mil`), and gives its wave back. **Why**: the end that demoted every sea and air ATTACK, at every level, kept
  their waves on the layer's next threshold and sent a submarine wave home on any brief end of the hunt.
- **The sea plant's factory count stays below `IMPAI_IDLE_CAP`** (32), 2026-11-15, the same review's L-3: past it
  a factory is in neither of the census's factory lists and takes no order.
- **The release keeps its root from the yard and the makers too; a turned-back helper is not the yard's; a
  record is live for fail_t at most**, 2026-11-15, the high review at cf3f68c9's L-1, L-2 and L-4: under a
  persona's bucket THE YARD and THE MAKERS leave the decision's last root to THE RELEASE as the idle loop
  already did (`impai_release_keep`); THE YARD passes over a helper whose ASSIST the walk check ended in the
  same decision; a far or help-refused record whose `until` lies more than `fail_t` past now holds nothing.
  **Why**: an Easy seat in a hold with an energy short could spend its one action a decision on maker switches or
  yard moves while a helper stayed on a factory's work; the yard gave a turned-back helper a second order and read
  its head after the turn back, which THE DECISION'S READS forbids; and a restored record past `fail_t` barred one
  builder from a cell for the rest of the game. **What it costs**: a yard move or a maker switch waits one
  decision per helper the hold releases, at most `IMPAI_IDLE_CAP`; the job and walk checks are not held back.
- **The step back: removed**, 2026-11-15, the whole-brain coherence review at d665a00a's finding 1: a member under
  `micro_hp` of its health no longer steps 400 px away from the contact its squad fights; every member fights, and
  the plan's `micro_hp` and `micro_period` are gone with it. **Why**: no army type heals by itself, and the mend that
  repaired a damaged member went before it (*The mend: removed*), so a member below the share stepped back from every
  fight it met for the rest of its life -- while the squad's side in each prediction, its wave's threshold, the
  home, the pressure and the stand all counted it as a fighter. A wave of four healthy units and five damaged ones
  was predicted to win as nine and fought as four. Of the two ways to make the count true -- leave such a member out
  of every count, or let it fight -- only the second keeps the unit worth its row: left out, it stays in its squad,
  walks with every wave and holds an army row (THE ARMY'S ROWS) for the rest of the game, a unit out of the fight,
  which is the metal the mend's paired bar found does not pay. **What it costs**: a damaged member dies in fights it
  would have survived standing back, which no measure has weighed yet against the fire it adds; the paired bar has
  not judged this change.
- **The makers read the flows with the switches since the pass in them**, 2026-11-15: a maker whose activation bit
  disagrees with its ledger's last-pass energy ask was switched since the pass, and its cost and metal are moved
  into or out of the flows before the switches, the ladder's maker rule and the plants' energy term read them.
  **Why**: at Hard a seat decides twice a pass, and the second decision read the same shortfall or spare and
  switched a second batch, committing about twice the energy and cycling the store from 60 % below 30 %. **What it
  costs**: nothing beyond the census's reads: an active structure with an energy use always asks it
  (`0x4013ED..0x401421`), so the bit and the ask disagree only for a maker switched since the pass, by an order or
  by its own script.
- **Known loss, the owner's decision of 2026-11-14**: Trout Farm `cc-2`, lost to a CORAPE gunship with the stock AI
  fielding air from 12 factories, may stay lost until A3's anti-air step, whose verification must close it. Every
  other game against a stock AI must be a win.
- **Stated gaps**: decoy commanders can draw the
  commander-first goal; whether a wave should weigh the defences along its way, and not only those near its
  target, is not diagnosed (17 of 22 losses on ProTA were to its defences).

## The brain's seams (2026-11-14)

A whole-brain review of the integration found rules that were each sound on their own and disagreed where the
parallel landings met. The coordinator's decisions; the owner may reverse any of them:

- **A home defence holds in the anchor box.** A DEFEND squad whose centroid stands in the anchor box while a base
  threat stands takes no retreat and spends no prediction: it fights ([the brain](brain.html), *The military*, step
  4). **Why**: a retreat goes to the rally point, `rally_px` towards the estimate, so a defence retreating from a raid
  that has passed it walks towards the raid with no attack order, and the next decision's defence makes it DEFEND
  again: the defence cycled through RETREAT and FORM while the raid stood in the base. The recordings cannot measure
  it cleanly: over the 20 games of the closing run, 2 of 297 squads that entered RETREAT were DEFEND at the record
  before, and a FORM squad that the defence and the retreat turn within one decision shows as FORM to RETREAT.
  **What it costs**: a home defence that would lose fights on rather than fall back; its damaged members still step
  back.
- **The base confirms its threats, and an empty home loses.** A threat is confirmed once the base has had one standing
  at `react_confirm` decisions in a row, whatever squads the home holds, and a home with nothing in the anchor box --
  no home squad's member, tower or commander -- is a predicted loss, margin −1 ([the brain](brain.html), *The
  military*, step 3, the recall). **Why**: a wave
  takes the whole FORM squad, so the home had no squad to confirm the threat until a new unit was finished, and
  raiders reached an empty base with nothing recalled: the Trout Farm loss mode. **What it costs**: a raid on an empty
  home recalls the nearest wave that can hurt it, however small the raid.
- **The recalled squad comes home as a RETREAT**, not DEFEND. **Why**: as DEFEND it merged into the home's DEFEND
  squad, whose centroid its far members dragged out of the threat's reach, so the home defenders stopped fighting and
  travelled while it walked home. A RETREAT is no operation, so the attention bound holds. **What it costs**: the
  recalled squad gives no attack order on its way home; it fights once the defence makes it DEFEND there. Its members
  count in the home once they stand in the anchor box (the next section).
- **The unlocks read the governor's room.** An unlock is a candidate only while the governor could make its
  constructor: below `cons_max`, and below the persona's `att_eco`, one predicate for both ([the brain](brain.html),
  *The plant choice*). **Why**: the unlocks were admitted to `cons_max` while the governor stopped at `att_eco`, so at
  Easy and Normal a plant chosen through an unlock waited for a constructor that never came, and while it stood no
  factory made a constructor at all. **What it costs**: at the attention's cap a seat takes the best of its own plants,
  as at `cons_max`.
- **The attention counts what it holds, once** -- decided 2026-11-14 ([the brain](brain.html), *The plan and the
  personas*, the attention). `att_eco` bounds the builders busy at once, the commander's economy jobs among them,
  so the governor's cap counts the commander beside its constructors; and a helper given another task by the
  helpers' pass stays one busy builder, taking no room and needing none. **Why**: the governor counted
  constructors alone while the attention counted the commander too, so under a limiting persona one constructor
  more than the attention lets work stood idle while the commander built; and a helper given another task was
  counted again as an assignment, which shut an idle builder out of the ladder (the coherence review's item 10). **What it costs**: with the commander alive, Easy keeps three
  constructors and Normal seven, one fewer than `att_eco`; a seat that lost its commander keeps `att_eco`.
- **One store threshold for the stalls and the hold, and no shortage past the tech-up** -- decided 2026-11-14
  ([the brain](brain.html), *The military*, the hold). `stall_pm` (200, the plan's) is where the metal and energy
  stalls' store terms begin and where the hold holds; it replaces the stalls' hard-coded 0.2 and the hold's own
  `hold_pm`. A seat is short of metal against the tech-up's `tech_m` only while the tech-up's own count holds,
  `factory_max` plants of its highest build tech. **Why**: the hold was a plan knob and the stalls' fraction a
  constant, so moving the one left the hold and the metal stall it is meant to start beside apart; and past the
  tech-up -- its plant the first of the new tech -- no plant is wanted outside a surplus, while the shortage still
  read `tech_m`, so a seat under it took every feature first for the rest of the game (the coherence review's item
  12). Its third point, the military's threat map without the radar contacts, was already closed: both maps stamp
  them as the military's substitute. **What it costs**: nothing at Hard's values -- 200 / 1000 is the same float as
  0.2.
- **The unlock is kept between the decisions that choose a plant** -- decided 2026-11-14 ([the brain](brain.html),
  *The plant choice*). The constructor a chosen plant waits for stays the governor's next constructor until the
  next decision that runs the choice, which replaces it, and is dropped as soon as the seat has a finished unit of
  it or no factory of the seat's makes it. **Why**: the choice runs only while a plant is wanted, and the wants
  are gates that lapse for a decision; the unlock came and went with them, and a factory the unlock is not on the
  menu of made its own constructor in between (the coherence review's item 9). **Why not keep the plant too**: the
  plant is ordered only while it is wanted, and the next decision that wants one chooses again on what it then
  sees; only the constructor has to come before that. **What it costs**: while kept, a factory that does not make the
  unlock makes no constructor, as in a decision that chose it.
- **A helper answers every want the seat has now**: the tech-up, a ground defence and an anti-air structure as well
  as the stall's energy, the plants and the reclaim ([the brain](brain.html), *Task allocation*). **Why**: the helpers
  were asked for the stall, the next factory and the sea plant alone, so a seat whose only constructor guarded a
  factory never answered its tech-up, and a pressed seat built no tower while its constructors helped. **What it
  costs**: a want takes its helper off the work under way; the extractors and the energy the ladder paces still go
  to idle builders only.
- **In a surplus the extractors wait only while the income pays for a further plant**, `factory_per_m` a pass for
  each factory below `factory_max` and a busy plant's measured request above it ([the brain](brain.html), *Task
  allocation*). Above it they wait on that overflow whether or not a plant is wanted: an Easy seat whose factories
  wait on its actions builds no plant, and its extractors wait all the same. **Why**: the rule
  read the store, a level, where the waste it is for is a flow; on Trout Farm a seat with one factory and an income
  of 3.3-3.8 a pass against the plant's 8 kept its store full on reclaim payouts and held one extractor for 40 000
  ticks, in each of the closing run's four Trout Farm games. **What it costs**: below the plant's income a full store
  still builds extractors, whose metal is wasted until a spender comes.
- **No builder guards; every builder helps the factories' products**, with the help that ends at its frame's
  completion. **Why**: a guard never ends, and the engine stands the guarder on the factory's yard, where the
  factory's create refuses its next product: a constructor guarding Trout Farm's one factory held it for 27 450
  ticks. **What it costs**: between two products a helper is idle for a decision and its build power waits.
- **The yard stays clear**: a builder the economy lists idle or as a helper that stands on a waiting factory's
  yard is moved off it, whatever put it there ([the brain](brain.html), *Task allocation*). **Why**: the engine's
  create placement test excuses no unit (`0x402896`), so any idle builder on a yard -- a parked product, a helper
  whose frame is done -- holds the factory for as long as it stands. **What it costs**: one order a builder, and a
  factory waits up to a decision and a reaction per product on the economy's builders.
- **The yard's region is its footprint and its kept way out**, for the builders the yard rule moves and for every
  point the military sends a squad member to. **Why**: a unit standing just beyond the exit holds the factory as
  one on the yard does -- the product cannot leave -- and the way is the plant's own proven exit, kept for as long
  as it stands, so the region needs no new geometry; the army is the other hand that leaves units standing, at the
  rally point the anchor's factories sit beside. **What it costs**: a builder parked down the way walks round to
  another side of the factory; a rally amid factories spreads its members over fewer points; a plant ImpAI did
  not place keeps no way, so only its footprint is kept clear.
- **A builder walking off a yard is left to its walk**: the rule orders a builder off once a walk, not once a
  decision. **Why**: the census lists a builder with a handed-back move idle, so the rule re-gave the move at every
  decision until the builder was off -- 5 to 8 orders a walk in the Hard games, and for a builder held in, at
  every decision for the rest of the game, which spent the economy's actions on nothing: an Easy seat's 368 of 407
  orders. A repeat changes nothing a held-in builder can do. **What it costs**: a builder that cannot leave is
  lost to the economy until what holds it in moves, and nothing in the brain records that it cannot.
- **A help the engine refused is not given again for `fail_t`, at the frame's cell** ([the brain](brain.html),
  *Task allocation*, the assist). A builder whose help ended while its frame stood unfinished takes no help on a
  frame at that cell until `fail_t` has passed. **Why**: the assist re-gave the refused help at every decision -- on
  Trout Farm three constructors beside an ARMLAB, one of them 246 head changes in 3 600 ticks, each an action of the
  persona's budget at Easy and Normal, the yard's drain. The cell and not the slot, because a plant makes all its
  products at one point, each a new slot: a slot would cost a refused order per product per builder, 15 products in
  10 000 ticks there. Recorded once for `fail_t` as THE FAR RECORD is: the refusal comes from where the builder
  stands, which can change. **What it costs**: a builder that has moved to where the help would work passes that
  cell over until the record ends; the plant's body, which caused the refusal, is not removed.
- **Each building and unit counted once.** The army's value by class takes the finished units and the factories'
  count nodes, which hold the unit in progress, so a product's frame is not counted beside its node; a builder's
  build head adds its ground defence, anti-air structure or energy only while its site holds no building of its
  type, frame or finished ([the brain](brain.html), *The military*, ground defences). **Why**: a head stays at its
  builder's list while the builder works on the frame it placed, so a tower under construction counted as two and
  the defences stopped a tower short, an energy frame held the energy rule back by its gain, and a tank in progress
  put the buy's next class off its share. **What it costs**: a head counted only after the whole census, one pass over the
  claims.
- **A pressed seat builds its ground defences in a metal stall.** The want still waits out an energy stall
  ([the brain](brain.html), *The military*, ground defences). **Why**: the pressure lifts the hold, but `m_stall`
  still held the towers back, and a raid that lasts keeps the store below a fifth of its storage with income below
  demand, so the towers the pressure is for never came. **What it costs**: a tower in a metal stall takes its share
  of the paid fraction from the factories' army, at most `def_max` of them.
- **The waves count and size by layer, and a destroyed target starts its layer over** (the owner's decision of
  2026-11-14): each layer keeps its own count of waves and its own median unit, each new wave of a layer is one
  median unit of that layer bigger, and a wave that destroys its target resets its layer's count ([the
  brain](brain.html), *The military*, the wave). Destroyed means a structure target whose row the seat no longer
  holds while the wave stands at it: only a structure's row stays until it is seen gone. **Why**: one count and one median for every layer let 52 sea waves
  and a median the submarines pulled up hold a land FORM squad of 24 at home on Red Triangle. **What it costs**:
  each layer's escalation starts from `wave_value` separately, so a seat may send waves of two layers at once.
- **A full substitute cache decides no arrival**: a member whose substitute the cache cannot give is not arrived,
  and the squad's arrival undecided, read as "go on as it is". **Why**: the arrival read a full cache as "no point
  to travel to, arrived", and a sea squad flipped FORM/ATTACK 52 times without moving. **What it costs**: a squad
  whose arrival stays undecided keeps its task until a later decision's cache answers.

## The finish by construction: the lead and the goal's progress (2026-11-15)

The broad check at bb11df10 won 129 games and reached the cap in three, each with the seat ten times the enemy's
units: 415 against 38 on `bar-eota-redtriangle-ac-2`, 483 against 36 on `bar-hota-troutfarm-aa-1`, 589 against 76
on `bar-nota-plainspasses-ca-2`. Each had its own cause -- the attention held by sea sweeps, the dry hunt never
begun, three operations held by a goal never reached -- and the coordinator asked for one rule that finishes a won
game whatever the thresholds ([the brain](brain.html), *The military*).

- **The lead**: the seat's live value at 2.5 times the enemy's as its rows remember it, the commander's value in
  place of a commander never seen, until it falls below 2 times. In the lead no operation counts against the
  attention, and a wave goes at its base size and counts nothing (*The lead on the evidence*, below). **Why the
  bar's measure**: a game at the cap is scored by value at 1.3 times, so the seat finishes the games the bar would
  call its own. **Why 2.5**: the seat's ratio overstates the true one by up to 2.36 on twelve recordings, and at
  1.3 the lead held in ten of them, in five where the seat was in fact behind; at 2.5 it held only where the true
  ratio was 1.31 or more, in all three caps and in none of the nine wins (*The lead*, its why). **Why mobile
  ghosts count**: the military's picture forgets a mobile out of sight, so the lead counted on it would read an
  army seen and lost as gone. A ghost lasts only until the seat's sight covers its last block, though, not until
  the unit is known gone, so the lead keeps the enemy's mobile value in a memory of its own (*The lead on the
  evidence*). **What it costs**: the margin measured on games against the stock AI alone.
- **The goal's progress**: an ATTACK gives up a goal it has not come 256 px nearer for `task_timeout` plus its
  travel, and the known goals pass over a given-up one for 16 200 ticks. **Why**: an ATTACK had no lifetime because
  its goal was chosen again at every decision, which held only while the goal moved; a sweep cell never reached is
  never stamped, so the sweep chose it at every decision for 45 000 ticks on Plains Passes. **Why the travel in the
  window**: the straight line can stay flat for minutes on a detour round a lake or through a pass, and a window of
  `task_timeout` alone would give up goals the squad was on its way to. **Why a goal taken on moving on is measured
  afresh**: the next goal may lie within 256 px of the one given up, and measured from its progress it was given up
  at once, never tried (found by the brain test's case).
- **The dry hunt asks the land army**: an enemy row is a land enemy when half the seat's land army reaches its
  block by its own classes' labels; the content's majority test stands only with no land army. **Why**: the
  content's test read every enemy as off the land on Trout Farm's west start all game. At 0c7e71bb, where the lead
  let the hunt begin with no land stamp, `bar-hota-troutfarm-ac-1` turned its factories to the sea three times
  while its land army fought on land and won only at GameTime 78 459 against 28 480; with a land wave's goal
  stamping the land instead (75beb607), the stamp made the ordinary hunt possible, the seat went dry while its army
  regrouped at home, and lost at 44 786. **Why the army and not a better majority**: the waves go by the squad's
  members' own reach, so the hunt asking the same army cannot disagree with them about whether the land army has
  anything to go for. **What it costs**: a shore target the land army could shell from its own component reads as
  off the land.
- **The lead on the evidence**, 2026-11-15, the broad check at d665a00a and the whole-brain coherence review's
  findings 6, 7 and 9: the lead begins and holds only while the estimate rests on enemy structures and no
  prediction of the seat's was lost within `IMPAI_DRY_T`; the enemy's mobile value is a memory of its largest lately,
  decaying over `IMPAI_DRY_T`; the commander's floor is counted a foe at a time, never for a foe whose commander the
  seat has seen; the lead's waves go at the base size; and the home keeps the mobile memory. **Why the evidence**: on
  `bar-nota-lavamania-ca-1` (ProTA) the lead began at 8 235 on the enemy's commander alone and went in and out nine
  times on rows holding a third of the enemy's structures, while the true ratio stayed near 1; its one-unit waves made
  66 retreat decisions where bb11df10 made 22, and the game reached the cap at 1.13, which bb11df10 had won. A lost
  prediction is the enemy showing a strength the rows do not hold; the three true leads at bb11df10 made one retreat
  decision after the lead would have begun. **Why not the army at the enemy's start**: measured on the same
  recordings, it held in false leads as in true ones. **Why the memory**: ghosts emptied on lit ground and radar
  contacts coming and going moved the ratio across the band. **Why a floor a foe**: a commander killed in sight left
  no row, and one floor came back worth 30 423. **Why the base and the floor**: at one median unit a lead fed its army
  in a unit at a time, and with the attention lifted each wave took the whole FORM squad. **What it costs**: one lost
  prediction holds the lead off for 5 400 ticks, so a seat far ahead whose squads keep meeting defences finishes by
  the ordinary waves; a foe the rows name nothing of counts no floor once another is named.
- **The home floor leaves the commanders out**, 2026-11-15, the high review of d665a00a..fd67c52d, MED-1 and LOW-8:
  the lead's home floor keeps home the enemy's raid memory, its mobile memory less the rows of commander types
  (`IMPAI_MIL.raid_v`, `raid_t`), while the ratio keeps counting them; and each wave that leaves takes its members in
  the anchor box off the home's value (`home_v`) for the rest of the decision. **Why**: the home's value leaves the
  seat's own commander out, and a commander remembered, 24 109-30 423 at retail, made a floor larger than a home of
  22 500, so no lead wave formed while it was remembered; a second layer's wave in the same decision counted the
  first wave's members as home. **What it costs**: a remembered enemy commander raiding the base alone is answered
  by the recall, not kept home against.
- **The estimate keeps the last structures' centre**, 2026-11-15, the verification games at 95c8d47b: with no
  enemy structure known any longer, the enemy base's estimate stays at the last centroid of those once known, and
  falls back to the nearest other start only before any was known (`IMPAI_MIL.est_seen`: `EST_START`,
  `EST_STRUCT`, `EST_KEPT`); a kept estimate begins no lead but holds one (the high review of d665a00a..fd67c52d,
  MED-2: the lead's waves raze the last structures they know, and a lead ended there demoted its hunters and began
  again at the next structure seen), and the economy reads the byte as the military does (`impai_est_known`, LOW-5).
  **Why**: on `bar-nota-plainspasses-ca-2`
  a wave razed the structures it saw near (8 144, 3 952) by GameTime 47 000, the estimate fell back to a start
  3 600 px west, the sweep's ties went nearest it, and the stock AI's base -- 65 units and its commander, 13
  extractors east of where the wave stood -- was never seen again: the cap at 561 units against 74. At d665a00a the
  same game, identical to GameTime 44 000, had seen the next structure before the fall back and won. **What it
  costs**: an enemy that has moved its base is looked for first where it was.
- **The dry hunt needs a land stamp, in the lead too**, the whole-brain coherence review at d665a00a's finding 4:
  the lead no longer lets the hunt begin with none. **Why**: the clause stood for a seat whose land army had never
  reached a known enemy, under the content's test; the army's reach and a land wave's goal make that stamp now.
  Over the 14 verification games at d665a00a, sampled every 300 ticks, the lead held on a level with a sea in one
  game, `bar-hota-troutfarm-ac-1`, 24 samples, the land stamp made in each; on `bar-nota-lavamania-ca-1` it held
  135 samples with none, on a level with no sea, never dry. A seat with no land stamp late in the game has known
  every enemy across the water, where water weapons would hit nothing ashore. **What it costs**: a seat that has
  only ever known enemies off the land never begins the finish at sea; the naval judgement answers that map.
- **A goal given up stamps nothing**, the same review's finding 3: an enemy row or a foe's commander at a goal the
  land layer gave up within `IMPAI_UNREACHED_T` neither stamps the land nor is evidence of an enemy off it. **Why**:
  a structure's ghost stays until the seat sees it gone, so one ashore that no land wave could come to kept the land
  stamp fresh. The ring holds it out only for `IMPAI_UNREACHED_T`; once its entry went, the ghost stamped the land
  again and ended the hunt, the seat dry two thirds of each such cycle (the high review of d665a00a..fd67c52d,
  MED-LOW-3), so once dry only a row seen now stamps the land (*the hunt needs evidence off the land*). **Why
  neither stamp**: the land army failing to come to a goal says nothing of what lies across the water.
- **Under the sea, a water weapon**, the high review at d665a00a's observation: a row under the sea (the seat's
  sonar alone, or a submerged type) is the land's only by the land units with a water weapon, half the land army's
  value. **Why**: a hover army's class crosses the water, so its labels "reached" a commander on the lake bed and
  kept the land stamp fresh while none of its weapons could hit it. **What it costs**: a ship the seat holds by
  sonar alone reads as under the sea.

## The home and the recall (2026-11-15)

The whole-brain coherence review at cf3f68c9 found the recall's seams the worst left: it judged the home one layer
at a time, one threat at a time, without the towers or the commander, and could spend every prediction of a
decision. One change answers them ([the brain](brain.html), *The military*, step 3, the home and the recall).

- **The home is one side, judged whole**: every member of a FORM, DEFEND or RETREAT squad of any layer standing in
  the anchor box, each at its own point, every finished own structure there with a weapon the fight content holds,
  and the commander there while the military's window is shut. The recall predicts it and the pressure weighs it.
  **Why**: a wave takes its layer's whole FORM squad, so a home judged per layer was empty in every layer whose wave
  was out, and a land raid the land army beat recalled the air and sea waves; towers and the commander kill a lone
  scout the squads alone read as a loss; and a home squad anywhere on the map -- a field retreat 4 000 px out --
  counted as if it stood at the threat. **What it costs**: a home squad on its way does not defend until it is in
  the box.
- **One prediction a layer group**: for each layer with an ATTACK or RAID squad out that can hurt a threat, the home
  against every threat such a squad can hurt, each enemy once, at the group's nearest gap; a group equal to an
  earlier layer's takes its margin. At most three predictions, and each only while the decision keeps one for every
  squad whose retreat test may follow; the recall runs after the commander's safety. **Why**: one prediction a threat
  and layer took all of `predict_max` under a raid of ten contacts, so the commander fled with no D-gun and the waves
  fought untested, and two prongs the home beat one at a time but not together recalled nothing. **What it costs**:
  a group takes a contact whole, so a land wave able to hurt a contact's tanks is weighed against its bombers too.
- **The recall weighs every member of the group**, with no stay-or-go. **Why**: the recall asks whether the home
  holds the base; an enemy the home can neither hurt nor outrun is just what it cannot hold against, and the
  predictor gives a fight in which neither side hurts the other a margin of 0, so harmless aircraft recall nothing.
- **One recall of a layer on its way**: while a RETREAT member of a layer stands outside the box, the layer recalls
  nothing more. **Why**: a recalled squad counts only once home, so each decision until then would find the home
  still losing and recall the layer's next wave. **What it costs**: the next recall waits for the squad coming,
  however small it is.
- **A recalled ATTACK gives its wave back**: its layer's count falls by one, held at 0. **Why**: a recall is no lost
  fight, and with the count kept every raid that brought a wave home made the next one a median unit bigger.
- **The wave's count**, 2026-11-15, the whole-brain coherence review at d665a00a's finding 8 and the high review's
  LOW-2: an ATTACK whose wave added to the count carries a bit, carried by a merge into another ATTACK and cleared
  when a squad takes a task of its own, and only such an ATTACK gives a wave back -- at the recall, at the finish's
  end and at the lead's end alike. **Why**: a lead's wave adds nothing, so the recall took from the count waves that
  never counted, and the lead's end gave back none of those that had. **What it costs**: an ATTACK that two counted
  waves made gives back one.
- **The commander fights for nothing of its own**, the high review at bb11df10's H-2: in the recall's prediction
  the commander fires and is fired at, but its value is no part of the margin (`IMPAI_FE_NO_VALUE`, an entry flag
  of the predictor's), so the margin is what the rest of the home keeps against what survives of the group. **Why**:
  worth 24 109-30 423 at retail against a few hundred a tower, the commander made the home's kept share its own
  health, and a raid that killed every tower while it lived read as a win, so the waves stayed out -- the very
  reason the pressure's home value leaves it out. **Why a flag and not leaving it out**: out of the side, the
  commander's guns and its body would vanish from the fight, and a lone scout it kills would read as a loss.
  **Kept first**, the high review at d665a00a's LOW-1: the commander's entry is its side's first, and the
  predictor's merge keeps flagged entries before any other and never merges one into another. **Why**: a hurt
  commander ranked below sixteen entries was merged into a cheap entry as 30 000 / v of its units and lost the flag,
  its value counted in the margin again; and the recall's side added it last, past a full side of 64. The stand's
  entry carried no flag while the D-gun could fire, so it too could be merged away; every commander entry now
  carries `IMPAI_FE_KEEP`, a flag the predictor reads nowhere but the merge (the high review of
  d665a00a..fd67c52d, LOW-4). **A
  side with nothing else of value** keeps all while the flagged entries are untouched and nothing once one is hurt
  (the predictor sets a bit at the damage itself): kept at 0 whatever happened, a home of the commander alone under
  aircraft that hurt nothing read as a loss and recalled a wave, against the rule that a fight in which neither
  side hurts the other is a margin of 0. **What it costs**: a home of the commander alone, once hurt, holds only by
  destroying the group, so a raid it would merely outlast recalls a wave.
- **A RETREAT's lifetime counts the whole way home**, from where its retreat began -- its centroid then, kept as the
  squad's point, or the farther point of a squad that joins it, which starts its lifetime again -- to the rally
  point. **Why**: its point is the rally point, so the travel read 0 and a wave recalled from farther than it walks
  in `task_timeout` turned FORM on the way and could leave again; counted from the way still left at each decision
  (the high review at bb11df10's L-1), it still ended at (T + 2W/v)/3 of a way W, 3 430 px out at Hard. **What it
  costs**: a retreat held up out of reach lasts `task_timeout` plus twice its whole way at its speed, at most
  16 200 ticks more.
- **The pressure weighs the home at the base and the army outside it, and the recall's verdict presses**: the stamp
  holds while the home is worth less than `press_k` × the value pressing at the base, or the whole army less than
  `press_k` × the value pressing an outlying structure, or the recall finds the home losing. **Why**: against the
  whole army a wave of 40 out and a raid of 5 in the base stamped nothing, so the economy built no ground defence and
  kept its hold while the recall brought the wave back; and against the home alone, with the waves out, one armed
  scout beside an extractor held the constructors for `press_hold` and lifted the hold (the high review at
  bb11df10's L-2) -- the home defends the base and nothing past it. **What it costs**: an outlying structure raided
  while the army is small presses as before.

## The energy and the factories' actions (2026-11-15)

The economy diagnosis of the mod games at d665a00a
(cause 2, energy handling, and cause 4, factories starved of actions), applied on retail content too.

- **The energy stall is the store's**, 2026-11-15, the economy diagnosis at d665a00a (cause 2): the energy stall is
  the store below `stall_pm` of its storage, with no test of the pass's flows. **Why**: a pass's flows flip between a
  factory's products, and on Escalation the stall read 64 coming in against 23.7 asked with the store at 6 %, so the
  second plant was ordered inside a stall; the store sums the flows, and rises above the line only on more coming in
  than going out, so the stall ends by its own rule. **What it costs**: a seat whose income only just carries its
  spending at a low store builds energy before its next plant.
- **A surplus is never short of metal**, 2026-11-15, the same diagnosis: short of metal is a metal stall, or the
  steady income below the next plant's need **with the store under 0.8 of its maximum**. **Why**: the income alone
  sent builders to reclaim into a full store -- where the room rule pays nothing -- ahead of the energy the seat was
  stalling on. **What it costs**: nothing the store does not already hold.
- **The stall takes the reclaimers, not the builds**, 2026-11-15, the same diagnosis: in an energy stall a builder
  at a reclaim or on its way to one is given the stall's energy, replacing the job only when a site takes it. **Why
  not the builds**: a builder taken off a build leaves its frame standing, the metal it holds spent; the store-based
  stall keeps a plant from being ordered in a stall, and the helpers at an assist already answer it (*A helper
  answers the wants*).
- **Every plant past the first waits on its energy; the products do not**, 2026-11-15, the same diagnosis: the
  second plant below `factory_max` and the tech-up wait on the energy term the surplus plant already waited on --
  the energy made less the energy asked at least a busy plant's measured energy ask -- and the energy rung's target
  rises by the shortfall. **Why the plant**: a plant spends its price a pass for as long as it makes, so the energy
  must be there before it; ESC's second plant was built with the store at 0. **Why not the products**: a product's
  ask is already in the energy asked (the request, paid or not), which the energy target reads at 1.1 times, and a
  factory refused a product is an idle factory, where an energy short of the ask only slows every build to the
  energy's fraction. **What it costs**: the second plant comes later on a seat whose energy only just carries its
  first. The sea plant is not gated: it is the military's want, the enemy at sea.
- **The factories' turn**, 2026-11-15, the same diagnosis (cause 4): a decision of one economy command whose idle
  factories want one reserves it for them on every other such decision (`IMPAI_BRAIN.root_turn`, flipped by each).
  **Why a turn and not a share of the budget**: THE SPLIT already gives the factories half of a decision of two or
  more, but the bucket keeps unused actions only up to its burst, and builders that spend each action as it comes
  keep every decision at one -- the split never reached. A turn holds whatever the rate: neither side waits more than
  one contested decision. **What it costs**: on the factories' turn a stop, the yard's step and the makers wait a
  decision (a turn back out of danger never does; *the factories first*, below).

## The features the seat knows: what the game draws for it (2026-11-15)

- **The seat knows a feature exactly when the engine's feature draw shows it to the seat's player**, the owner's
  idea, confirmed in the engine (the wreck-sight sweep, 2026-11-15; [map](../engine/map.html), *What the feature
  draw shows a player*): on explored ground every feature, live, greyed outside sight; a `nodrawundergray` def only
  to its owner or where a sheared corner of it is in sight. It replaces the seat's own sight at the anchor's block,
  which was stricter than what a human in the seat is shown. **Why**: "no cheating" becomes true by construction --
  the seat knows what the game would draw for it, no more and no less --, and on the bar's setting (LosType 6, the
  map explored) every remembered feature is current at every decision, so a builder is ordered to reclaim a wreck
  out of sight at once instead of walking to see it. **Unchanged**: every danger test -- the threat at the feature
  and on the route, the held ground, the commander's `comm_r` and safe distance, the walk checks --, the looks'
  radius and the memory's 128, so the search stays bounded a decision and the grid is never walked whole. **The
  risk**: the threat maps are blind where the seat has seen nothing, and a wreck known out of sight widens the
  choice; `harvest_lost=` and `reclaim_gained=` on the `eco` line measure it. A full memory, every entry shown,
  gives the place of its poorest entry to a richer feature, never an entry a reclaim job claims.

## Finding the enemy, finishing it, and the lead's evidence (2026-11-15)

- **The starts are looked at in turn.** Before any enemy structure is known the estimate is the nearest start the
  seat's army has not stood in (and its land army reaches), not the nearest start for good; the scout takes such a
  start first. **Why**: on Trout Farm's ten starts the estimate held the empty centre and the scout's trips ended at
  their first contact before the west start, where the stock AI stood: found at GameTime 55 980, and
  bar-eota-troutfarm-cc-1 reached the cap at 9d21b778. The swept stamps the waves already keep say which starts
  were looked at, so no new state.
- **The dry hunt begins when the land has been looked over.** Besides an enemy known off the land, the hunt begins
  when, since the last land enemy, the army has stood in every sweep cell its land army reaches. **Why**: an enemy
  left in the middle of a lake is seen from no shore; bar-hota-troutfarm-aa-1 at bcd7fa05 swept the whole land every
  9 000 ticks or so from 42 360 and saw nothing off the land until 66 150. **Gap**: standing in a cell is not seeing
  all of it.
- **A sea plant site no builder takes is passed.** The cursor passed only a site the resolver refused, so a site out
  of every builder's reach, or refused the commander for the threat near it, was found again at every decision; the
  same game's ARMSY site stood from 66 700 to the cap.
- **The lead's base floor.** Each foe counts at least the seat's own base times the share of the map the seat's
  army has not stood in within 5 400 ticks. **Why**: the lead began in all eight Escalation and Mayhem games of the
  broad check at 1a9350ea on the commander's floor and one to three structures; the seat's own base is the
  content-free yardstick of what a foe has built by then. **Why the looking**: the high review of
  1a9350ea..4c8727bd (MED-2) found the unbounded floor made the lead depend on the seat's own army-to-base split,
  so a grown base ruled it out late in a game; an army that fights over the map looks over much of it. **Why the
  whole map, not the ground round the known structures**: that ground is where the raids go. Held to it, the
  Escalation game `bar-nota-lavamania-ca-2` still began the early lead at 14 857 -- its raiders had stood in 7 of
  the 16 cells round two outlying extractors -- while the seat had stood in 50 of the map's 256 cells. A foe that
  truly built less delays the lead where the seat has not looked; it never begins one.
- **The factories' turn: the factories first, the rest to the builders.** The high review of fd67c52d..1a9350ea
  (MED-2) found a reserved command lost when the governor left every idle factory unordered, and the builders' turn
  backs held with it. Now on the factories' turn the governor runs first, the builders take what it leaves, and a
  turn back out of danger is never held. **Every contested decision passes the turn**, spent or not: the high review
  of 1a9350ea..4c8727bd (MED-1) found that a turn kept until the factories spent a command held the stops, the
  yard's step and the makers for as long as an idle factory stood unordered -- the yard's deadlock again. Strict
  alternation bounds every wait to one contested decision.
- **Knowledge from the draw, risk from sight.** The reclaim's confidence decays with the ticks since the seat's own
  sight covered the feature, not since the draw showed it (the review's LOW-3): with every feature shown the broad
  check's reclaims ordered doubled and the builders lost on them rose from 7 to 20. The held entries' marks move with
  every drop (MED-1).

## The army fights together, comes home free, and the commander keeps clear (2026-11-15)

- **The side that fights.** A squad's stay-or-go weighs its members and every army unit of another ATTACK, RAID,
  DEFEND or FORM squad in its own reach of the same contact. **Why**: the slow Hard Pin Cushion games (60-78k
  ticks, `-ca-2` at the cap with 414 units against 40) fell back wave after wave: 28 of `-ca-2`'s 35 retreats had an
  ATTACK of the same layer within 1 024 px, each squad predicted alone against the enemy's commander and towers.
  **Why not merge the squads**: the squads' table and the attention rest on the squads as they are; weighing the
  same units at one contact makes the answers agree without changing who belongs where.
- **The retreat releases each member as it arrives.** **Why**: the RETREAT squad waited for its last member and
  every squad that fell back joined it and restarted its lifetime, so it never ended while the waves kept falling
  back: 60 % of `-ca-2`'s army value stood in RETREAT from GameTime 30 000, 40-44 % in three other Hard Pin Cushion
  games.
- **The commander's flight opens the gap, and the economy never walks it towards a foe's commander.** On
  `bar-eota-lavamania-ac-2` both commanders died in one blast with the seat 57 units against 14: the economy walked
  the seat's commander towards an enemy commander it had fled 300 ticks before and could no longer see, and when
  that commander came into sight 393 px off the flight went to the structure farthest from it, across its line, so
  the gap barely opened before the seat's army killed it. The flight now takes the safe point that opens the gap
  fastest, and for 600 ticks from a foe's commander's sighting -- the flight's window and up to as long again past
  its hand-back, past the 540 the case needed -- no economy task walks ours nearer to where it was seen and to within the flight's own keep; a site
  within the commander's builddistance walks it nowhere and is never refused, and moving away or across stays
  free. **Why the walk and the 600, not the site and the hunt's 5 400**: the high review of 4c8727bd..8c7b10a4
  (MED-1) found the keep, grown over 5 400 ticks, reached about 6 600 px and turned one sighting into a
  minute-long ban on every site nearer the foe than the commander, a site already in build range among them,
  each accepted task moving the commander to the back of its comm_r disk, worst in the opening, while it is still
  the main builder. **What it does not do**: a commander never seen near ours can still arrive unseen; holding ours off every place an unseen commander
  could be would hold it off its own base for most of a game, the economy's first builder lost to a risk the
  sighting already bounds. **Owner's decision wanted** if a stronger rule is preferred.

## The posture and the arbiter (the owner, 2026-11-17)

**The fault it answers.** The economy decides through flags that name one another. `economy` (impai_brain.c) writes six wants (`want_factory`, `want_sea`, `want_tech`, `want_def`, `want_aa`, `want_sensor`), each a conjunction over the same gates (`hold`, `pressed`, `surplus`, `e_stall`, `m_stall`, `m_short`) and over what the military judged of the map (`naval`, `dry`, `island`, the blind seat); `assign` (impai_econ.c) spends them down a fixed chain of tests, clearing a want as it places it; and the island's rules reach into both (`air_take`, `air_let_go`, `plant_pick`). `hold` alone is read by eight functions in three files (`tools/impai/rules.py hold`). A rule added to one want is a term added to the others', which is where the whole-brain review of 2026-11-14 found its faults: a defence refused in a metal stall that the pressure had lifted the hold for, a helpers' gate that left out three wants, an unlock and an attention cap reading two predicates for one question. On 2026-11-17 one scenario, the naval turtle, took six rulings and about twenty-five commits and still ends at the cap.

**What is taken**, in four steps, the first three proved neutral -- the same seeds play the same games, decision for decision (`session.py --neutral`, and the brain test's transcript unchanged):

1. **The gates, one table.** The decision computes its gates once, as bits (the hold, the two stalls, the pressure, the surplus, the judgement not yet made), and each want is its need and no gate its row of the table vetoes: `want[k] = need[k] && !(gates & VETO[k])`. The table is the one place that says which gate holds which spend, and a hard constraint (a veto, safe by construction) is apart from a need (a preference, a number). The rows reproduce today's conjunctions exactly, the disagreements included; a disagreement is then one cell, changed in a session of its own and judged by its games.
2. **The posture, one object.** What the seat has judged of its map and its enemy -- naval, the dry hunt, the island, blind, pressed and where -- is filled once a decision from the military's record and read by every rule that asks; no rule reads the military's record for it or derives `island && blind && sea_plants >= 1` itself. It is derived each decision, so the recorded layout does not change. One log line a decision states it.
3. **The wants as requests.** A want becomes a request: its kind, what it asks for (a type, a point), its priority. `assign` walks the requests in priority order for each builder, and takes the first one that builder can serve (its menu, its reach, a site). The order is a table; today's chain of tests is its first value.
4. **Then, not neutral, each judged by its games**: the priorities become numbers the tuning reads; a new map situation is a posture and rows of the tables, not terms in every want; and the squads' tasks get one commitment rule (a task is kept for a stated time or margin before it may flip) in place of the exceptions that each ended one oscillation (the home holds, at the rally, the retreat home).

    MEASURED 2026-11-18 on the champion's session base: no task flip of the tier's games costs anything a commitment would recover (the artillery creep's FORM and DEFEND six times in 210 ticks moves two units and loses none; Red Triangle's and Lava Mania's are a goal with nothing left at it and whole trips ended by the stay-or-go). The squads' commitment rule is therefore not built; it waits for a game that shows its cost.

**What is not taken.** No rewrite, no behaviour tree or planner library, no scoring of every action from the start: the ladder's order is right in almost every game, and a utility score that replaced it at once could not be proved neutral. The scores come in step 4, one priority at a time, where a game can say whether each helped.

**Why neutral first.** A refactor that also changes play cannot be told from a regression, and the brain's games are exact: one build plays one id the same way every time, so a step that changes nothing shows no parting in any game, and a parting names the decision where the step went wrong.

## A start with no room in sight (2026-11-18)

The seat reads the map's layout in every game ([the brain](brain.html), *The map's layout is known in every game*), so no site of a base ring lies on ground it has not seen: the ring gives every site its cheap tests leave to the site test, and the brain has no rule for an unseen site and no walk to look at one. A start hemmed in by ground no plant fits is answered by the ring's growth alone: *The ring the budget cut* and *The first plant past the commander's reach*, below.

## A unit seen gone leaves no row (2026-11-18)

**The fault.** On `bar-hota-redtriangle-ac-1` at the champion 0a4ec04e (the session tier's base) the enemy's last structure, the advanced shipyard in slot 3028 at block (73,108), is destroyed at GameTime 35 226 by three land squads on the shore. The seat's row for it goes from line of sight to a ghost at tick 35 232 and stays to the end. Five waves take the ghost as their goal (35 371, 35 941, 36 601, the sea wave at 36 721, 37 141); a squad that arrives has nothing to attack, goes back to FORM and walks home, and the next wave forms. The win comes at 37 721, 2 495 ticks after the shipyard's death.

**The cause**, from the recording and the source, no diagnostic build: the slice made a row a ghost whenever `observe` showed nothing, and emptied a ghost only when the seat saw the row's block. The engine sees a unit by four points of its box (`0x465AC0`, `unit_los`) and the row keeps the block of its centre, so the seat saw the shipyard by a corner from the shore, saw it die, and kept a ghost at a centre block on the water that no land squad lights: the block is dark at every visit of the slice for 2 500 ticks. Of the item's two candidate faults -- a death the seat saw is not believed; a ghost that is a goal is never looked at -- the game shows the first, and the second follows from it here.

**What is taken.** A row the slice finds held by line of sight whose slot holds no unit any longer is emptied, its radar classes with it ([impai](../modules/impai.html), the information rule, with its bound of one slice round). Nothing is read of a unit: a row held by sight asks `observe` as for a contact, which then answers for any live unit, and only whether it answers is used, so the tick's work stays one observe a slot.

**The games after it** (`.data/impai/runs/session/item7-t1` against `session/base-32d93b3d89fc`, the scratch build of the committed rule): Red Triangle is won at 36 456; the shipyard dies at 36 115 and its row is empty at tick 36 119, no wave takes a dead unit's row as its goal (five before), and the win is 341 ticks after the death. The game parts from the champion's long before the shipyard, though (the rows at tick 8 924, an extractor destroyed in sight whose radar class the champion keeps until the next publish; the decisions at 22 530), so the two win ticks are two games' and not one game's with and without the ghost: the one other parted build on record (`session/nm-look-openE`, an unrelated opening change) won the same game at 41 374. The tier: no game in a worse category. The artillery creep parts at 14 017 (pass, 34 449 against 34 740); Trout Farm at 19 807 (won at 42 976 against 40 743); Plains & Passes at 16 620 (won at 49 291 against 35 901; `nm-look-openE` won it at 44 885); Steel Jungle at 27 007 (at the cap not ahead, 0.48 against 0.51); Lava Mania at 13 957 (won at 39 212 against 37 207). In each the first row that differs is a unit destroyed in sight -- an extractor, a solar collector, a mobile artillery piece, on Steel Jungle an advanced shipyard the champion keeps as a ghost and weighs in the enemy's base (`mil.ebx` 110 against 98) -- that the champion holds on as a radar contact or a ghost and the rule empties. The wave counts' resets are no more frequent (Red Triangle 1 against 3, Trout Farm 4 and 4, Plains & Passes none in either).

**What it does not do.** A unit destroyed from beyond sight, or after the seat's sight left it, is a ghost as before, and a squad sent to a ghost it cannot light from where its task holds it still stands there (the naval turtle's tower, 1 580 to 2 150 ticks of shelling a ghost): the arrival that includes seeing the spot is not built. Four of the five fight games end later or lower with the rule and one sooner; one game an id cannot tell a slower finish from another game's dice, and the paired margin test is what judges the time to win.

## The ring the budget cut (2026-11-18)

**The fault.** In every game of the look at the games the champion does not win ([landings](landings.html), *What holds the champion back*, row 1) the seat's metal store stands full and it gives no build order for the rest of the game, at one or two tier-1 plants, while `think.py why` reads the factory want "needed, taken".

**The cause, MEASURED 2026-11-18 at c16b60cc with a diagnostic build** that logs, at every fourth decision, each rung a builder at the ladder is asked and what the plant's ring walk met (three games; the share is of the metal wasted in the spans where that reason stood):

| the game, its end | the plant want met | ticks | share of the waste |
|---|---|---|---|
| `bar-hota-lavaalley-cc-1`, the cap, 16 382 wasted of 46 704 made | wanted (tech-up and surplus), CORALAB: ring 0 has 54 sites, the nearest 32 fail the site test, the decision's 32 site tests are spent | 43 087 to the cap | 61 % |
| | a plant order stands: eight CORALAB orders, each standing 1 980 to 2 700 ticks (`job_timeout` is 2 700) and ending with no frame | 29 887 to 69 487 | 32 % |
| | not wanted: the energy term | scattered | 5 % |
| `bar-hota-steeljungle-ca-1`, the cap, 2 826 wasted of 18 734 | wanted (the reaching plant), CORHP: ring 2 has 44 sites, the nearest 32 fail the site test | 15 427 to the cap | 67 % |
| | not wanted: income 5.9 below the pacing's 8 | 10 447 to 26 107 | 28 % |
| `bar-hota-lavaalley-aa-1`, won at 48 701, 5 051 wasted of 22 644 | wanted (tech-up and surplus), ARMAVP: the nearest 32 ring sites fail the site test | 20 707 to the end | 75 % |
| | a plant order stands | to 45 607 | 23 % |

The walk starts at the same site at every decision and ends at the decision's 32 site tests, so a ring with more than 32 sites the cheap tests leave, whose nearest 32 the site test refuses, is never walked to its end: it is not exhausted, does not grow and is not passed over, and the same 32 tests are spent at every decision -- 585 of Lava Alley cc-1's 1 351 sampled decisions and 1 096 of Steel Jungle's -- so no builder asked after it has a site test for its energy or its maker either. That is the "no build order". The constructors that take nothing are the helpers asked the plant rung for a type their menu lacks, and on Steel Jungle the idle vehicles with no extractor site, the energy at its target, the makers off and no frame to help.

**What is taken.** A walk the site tests end that tested at least one site and had every one refused is exhausted as a ring walked to its end is: the ring grows, and at ring 8 the plant type is passed over ([the brain](brain.html), *The base ring grows from the anchor*; `impai_build.c`, THE CUT RING GROWS). No state is added and no want: the plant comes under the wants' own terms, one pending at a time. A cursor that resumes the walk where it was cut was not taken: the walk's order belongs to the type, the growth and the builder's key, so a cursor is recorded state for each, and the growth already ends the wait.

**The games after it** (the scratch build of the committed rule; `runs/diag/item8-t1-run`, `runs/diag/item8-t1-dev`, `runs/session/item8-t1`):

- Lava Alley cc-1: won at 53 002 with a tier-2 plant standing, 3 528 wasted of 28 507 (the champion 0a4ec04e: won at 50 345, two tier-1 plants, 4 927 wasted; c16b60cc without the rule: the cap). Lava Alley aa-1: won at 39 056 with a tier-2 plant, 711 wasted of 19 588 (the champion 50 211 and 6 533; c16b60cc 48 701 and 5 051).
- Steel Jungle ca-1 is the same game in its end: the cap, one plant, 2 826 wasted, value 0.48. The ring grows to 8 and CORHP has no site there either (none of the 56 sites past its cut passes the site test), the type is passed over and the reaching plant's want ends; the site tests are spent at 166 of the 1 351 sampled decisions. Its waste is then the pacing's (income 5.9 below 8, 62 %) and the sea plant's search with no candidate (27 %): row 3 of the table, not this rule's.
- The session tier against the champion: no game in a worse category (3 the same, 2 better as at c16b60cc, 7 parted). Fifteen games against c16b60cc, game by game (the tier's four won bar games and Steel Jungle; Lava Alley aa-1, ac-1, ca-1 and cc-1; Acid Foursome, Luschaven and Gasplant Plain ac-1 and ca-1): eight won sooner (by 467 to 12 928 ticks), three later (Lava Alley ac-1 by 1 921 and ca-1 by 6 175; Red Triangle ac-1 at 58 051 against 36 456), one the same game, one won that reached the cap (Lava Alley cc-1), and two at the cap in both (Steel Jungle; Luschaven ca-1, value 1.98 against 1.31). The eleven parted games both builds won end 23 770 ticks sooner in sum.
- Red Triangle's decision lines part at 28 807, after the rule first acts at 25 807: an energy ring's walk of 82 sites, cut after ten refusals behind the walks that spent the decision's other 22 tests, grows the ring, though 27 of the 61 sites past that cut pass the site test. The same kinds of plant are ordered to 38 077; it is not won at 36 456 as c16b60cc's game is, and from 38 047 the seat stands at seven plants with a full store and idle factories, no plant wanted while a factory is idle: 21 478 of its 26 711 wasted metal fall in those spans, before the attacks of 53 070 and 57 030. The same game was won at 37 721, 36 456 and 41 374 by three other builds.

**What it does not do.** The sites of a ring past the cut are not tested before the next ring's nearest: on Steel Jungle 7 of CORLAB's 64 sites past the cut at ring 8 pass the site test, and the type is passed over without them. A walk cut after few tests, the decision's others spent by the walks before it, grows the ring on those few (Red Triangle at 25 807, ten). A walk that resumes where it was cut would answer both, at the price of recorded state. Nothing past the cut is looked at either: a site there that a whole walk would have left for the commander's reach or for ground threat does not hold the ring.

**The keyed walks are left out** (`impai_build.c`, A KEYED WALK IS NOT EXHAUSTED BY A CUT). The defence's walk and the sensors' are ordered from their key, so a cut one tested the key's nearest sites, whose refusals say nothing of the base's room, and the sensors' sites past the cover -- which hold a whole walk -- are its last and never reached; with the rule as first taken such a cut grew the base ring for every builder. A keyed walk the site tests cut now leaves the ring; walked to its end it exhausts it as before. MEASURED 2026-11-18, the scratch build `runs/diag/item11-a` against the tip 6bf6d229 (`runs/diag/item11-a-run`; the session tier `runs/session/item11-a` against `session/item10-t1` and `item10-rule`), fifteen games: eleven equal in every decision line, Red Triangle ac-1 among them (won at 74 077); the naval turtle's attacks 3 for 2, at its cap either way; `bar-hota-lavaalley-cc-1` won at 45 468 where the tip reaches the cap at 81 000, and `-aa-1` at 50 773 for 68 374, a tier-2 plant standing in each; `bar-hota-steeljungle-ca-1` at the cap not ahead, value 0.85, where the tip wins at 54 973 (the two part at 19 477, the tip's ring at 8 and this one's at 0). Against the champion 5643b394 the tier has no game in a worse category (4 the same, 9 parted). **What it costs**: a keyed walk whose nearest sites are refused, more of them than the tests the decision has left, is cut at the same sites at every decision while its want stands, as every walk was without the rule, and the walks asked after it test nothing; and Steel Jungle's win, which the keyed cuts' growth had carried. A plant order that stands and ends with no frame (Lava Alley cc-1's eight) is untouched, as is a full store beside idle factories (Red Triangle from 38 047; Luschaven ca-1, 59 817 wasted of 143 652 at the cap).

## The first plant past the commander's reach (2026-11-18)

**The fault.** Arm at Luschaven's start 3 never builds a plant, in a mapped skirmish and an unmapped one alike: `think.py why` reads the factory want "needed, taken" at every decision to the cap, no `plant` line follows, the store stands full and the seat ends with its commander, its extractors and its solars ([landings](landings.html), *Two opening faults*).

**The cause, MEASURED 2026-11-18 with the `plantwalk` line** ([the brain](brain.html), *The plant walk's trace*; the opening's game to GameTime 24 000, the commander the seat's only builder, the choice ARMVP throughout):

| the game | `ring_grow` | the walk | its return |
|---|---|---|---|
| mapped | 0, 1, 2 (3 321 to 3 966) | 8, 21 and 31 sites tested, none passed | grew |
| | 4 (3 981) | 50 sites: 27 past the reach, 21 tested, 1 passed and its way out refused | left |
| | 5 (3 996 to the cap) | 58 sites: 50 past the reach, 7 tested, none passed | left, at every decision |

The vehicle plant fits nowhere near that start, and a ring that leaves one site for the commander's reach is not exhausted, so it never grows to its widest and the type is never passed over. A scratch build that asked, for each plant type on the commander's menu and each ring, the sites within the reach that the site test passes found room for other types at the base: mapped, ARMLAB 3 sites on ring 0 and 2 on ring 1, ARMAP 2 on ring 0, each with a way out, ARMVP one site on rings 3 and 4 and none elsewhere, ARMSY 15 on ring 3; unmapped at 18 006, ARMLAB 2 and 2, ARMAP 1 and 1, ARMVP none. So the seat's case is not "no plant type has a site in reach": the type the choice takes has none, and nothing passes it over.

**What is taken.** Two rules of the base ring (`impai_build.c`, THE LONE COMMANDER'S PLANT and A TYPE PASSED OVER GIVES THE RING BACK; the second is since replaced by each plant type's own growth, *The plant's ring is its own* below). A plant site past the commander's reach counts as a lack of room while the census holds no constructor and no factory, frames included: the reach leaves such a site for another builder, and that seat has none and can get none before a plant stands. And the pass-over at the widest ring puts `ring_grow` back at 0, so the next type the choice takes is walked from the base outward: `ring_grow` is one counter for every type, and without that the next type is asked at ring 8 alone, where on Luschaven every site is past the reach. No state is added and no want; the reach itself stands, every order to the commander within `comm_r`. With no plant type that has a site in reach, each type is passed over in turn within nine walks, the waived pass walks the rings once more and stays at the widest, and the seat orders no plant, as before; whether the commander may then go farther for a first plant, or the first plant be the sea plant, is not decided here.

**The games after it** (the scratch build of the rule; `.data/impai/item10/`, `runs/session/item10-t1`), the figures named before the try -- the first plant's GameTime, the pulse's `built` at 6 000 and 24 000, metal made by 18 000:

| Luschaven, arm at start 3 | first plant | built at 6 000 | metal by 18 000 | built at 24 000, army |
|---|---|---|---|---|
| mapped, the champion 5643b394 and the line's tip | none | 10 | 5 855 | 10, 0 |
| mapped, the rule | 4 056 (ARMLAB; the tech-up's ARMALAB at 6 921) | 11 | 7 873 | 62, 20 |

- Mapped, the ring reaches 8 at 4 041, three decisions after it stood at 5, ARMVP is passed over and ARMLAB is ordered on ring 0 at the next decision.
- The session tier against the champion 5643b394: no game in a worse category (4 the same, 1 better, 7 parted, each of the seven on the `plantwalk` line or on 7c73bf7b's change). Against the line's tip with the `plantwalk` line alone, every decision line of eleven games is equal and one game parts: `bar-hota-steeljungle-ca-1`, at 11 842, where CORAP is exhausted at the widest ring and the ring is given back -- won at 54 973, against the cap at value 0.48 for the champion and the tip.

**What it does not do.** A seat with a constructor or a factory keeps the reach's rule, so a commander asked for a further plant with sites past its reach still leaves the ring. The fixture (`impai_brain_test.c`, `check_plantwalk`), `tools/impai/mutations/lone-plant.py` and the tier's opening `mapped-luschaven-3` hold it.

## The plant's ring is its own (2026-11-18)

**The fault.** One counter, `ring_grow`, held the growth of every type's base ring: any exhausted walk pushed it out for all, and any ring build, or a plant type's pass-over, put it back to 0. Three measured consequences, each read with a scratch line a base-ring walk (the type, the ring, the sites and what each test took):

- **A plant type's first walk met a ring other types had climbed.** `bar-hota-metalheck-ac-1`: under the commander's far record (532 to 5 932) the wind generator's and the extractor's walks, every site left for the record, climbed the ring to 8 from 2 227; at 5 932 the vehicle plant's first walk met ring 8, each of its 76 sites past the reach, and the lone commander's rule passed the type over with no site tested. ARMLAB was ordered at 6 352.
- **A ring was never walked for the type.** `mapped-luschaven-3`: the vehicle plant's walks took rings 0, 1 and 2 (3 321, 3 516, 3 966; 8, 21 and 31 sites tested, none passed) and its next walk, at 3 981, was ring 4: the counter had been pushed past ring 3 between them ([INFERRED] by another type's exhausted walk; the plant rung's trace holds one walk a decision). Ring 3 was not walked for the type; it was passed over at 4 041 and ARMLAB ordered at 4 056. On its own growth the vehicle plant is ordered at 3 336, the decision after its first 32 site tests took rings 0 to 2.
- **A plant walk that orders nothing costs a job.** The builder goes on down the ladder and takes the next rung's work, so each ring of a plant's climb waits for that builder's next idle decision. `bar-hota-plainspasses-ac-1`: the vehicle plant's ring 0 is exhausted at 2 092 and the plant is ordered at 4 447.

**The reading that chose the rule** (`bar-hota-metalheck-ca-2` to 8 000, the base beside a scratch build where every kind of walk kept a growth of its own). On a metal map the extractors stand on the base ring. At the base the wind generator's walk, in the stall rung, exhausts ring 0 at 1 754 and at 2 474 (8 sites, 5 under the extractors' claims, 3 refused); the extractor ordered at ring 1 in the first decision puts the counter back, and in the second the vehicle plant's one walk is at ring 1 (12 sites) and orders. With a growth a kind the wind generator is ordered at ring 1 at 2 579 and again at 3 329, 3 899, 4 409, 4 799 and 5 129, and the vehicle plant's one walk, at 5 459, is at its own ring 0 (16 sites by then) and orders at once: the plant's growth climbed nowhere. The plant is later because the stall rung stands above the plant rung and takes the commander at each idle decision once its walk finds a site -- as it does on every map whose ring 0 takes a wind generator. So the first plant's tick on a metal map rests on the wind generator's walk failing, and a growth of its own for the energy walk is a change to the opening's order, not a repair of the ring: it is not taken here, and `ring_grow` stays one counter for every type that is no plant.

**What is taken** (`impai_build.c`).

- THE PLANT'S RING IS ITS OWN. A plant type's base ring has its own growth, in the type's refusal row: `IMPAI_PLANTREF.cgrow` for the commander's walks, `.grow` for every other builder's. No plant's walk reads or writes `ring_grow`. INVARIANT, an ordering: a plant growth is written by its own walks alone -- one more at an exhausted ring below the widest, 0 at the walk's order, 0 at its exhaustion at the widest (the type then passed over unless waived), and the commander's 0 at a ring it leaves for its reach -- so no growth stands at the widest ring for a later walk to meet, whether the walks that climbed it met refusals or the builder's far record. Two growths, because the commander leaves a site past its reach for another builder: one growth for both would stand at the ring the commander leaves until a constructor asks for the type, or go back to the base under the constructor whose site lies past that ring. BOUND: two bytes of a type's eight-byte row, the row's padding until now, each 0..8; the rows are recorded whole and their size is unchanged.
- THE PLANT'S WALK CLIMBS. A plant's walk that exhausts a ring below the widest walks the next ring in the same decision, to an order, a site that is left, a way out deferred, the decision's site tests or the widest ring. BOUND: nine rings a walk and the decision's site tests, which are not raised; a walk they cut goes on from its growth at the next decision.
- THE PASS-OVERS OVERLAP. Each pass-over, and each exhausted ring of a plant's walk that is not the waived pass's, gives every pass-over still standing `fail_t` from then, so the choice reaches the waived pass however short `fail_t` is against a type's climb.

**The tries.** The plant's own growth with one ring a decision was played first and dropped on the figure named for it: the first plant at 3 254 for 2 474 in ca-2 and at 5 324 for 2 324 in aa-2. The plant's ring 0 there holds 4 sites, 3 under the extractors' claims and 1 refused; the walk grew its ring and ordered nothing, and the commander took an extractor (ca-2) or, by the energy rung at the shared ring 1, the first of six wind generators (aa-2). The climb within the decision is what the base had by accident, the wind generator's failed walk pushing the shared ring out for the plant in the same decision.

**MEASURED 2026-11-18** against the base 92d5f58a, one game each (a game replays exactly from its seed).

| the game | the base | with the rules |
|---|---|---|
| `bar-hota-metalheck-ca-2`, first plant; first wind generator ordered | 2 474 at ring 1; 4 319 | 2 474 at ring 1 (its own 0, then 1); 4 319 |
| `bar-hota-metalheck-aa-2`, the same | 2 324; 4 004 | 2 324; 4 004 |
| `bar-hota-metalheck-ac-1`, first plant | ARMLAB 6 352, the vehicle plant passed over at ring 8 | ARMVP 5 932 |
| `mapped-luschaven-3`, first plant | ARMLAB 4 056 | ARMVP 3 336 |
| `mapped-steeljungle-2`, first plant | ARMVP 5 324 | ARMVP 4 814 |
| `bar-hota-plainspasses-ac-1`, first plant; first army unit done | 4 447; 7 800 | 2 092; 6 480 |

The session tier: no game in a worse category, 5 the same and 10 parted. Won sooner: Plains & Passes 43 017 -> 32 303, Core Prime cc-1 48 285 -> 32 999 (parted at 12 292, a tech-2 plant ordered one decision sooner), Metal Heck aa-2 53 343 -> 43 588. Won later: Trout Farm ca-1 47 710 -> 51 296, parted at 36 577. Steel Jungle ca-1 is at the cap not ahead either way, value 0.76 for 0.85: the vehicle plant, every site of its rings within the reach refused by the site test or by its way out, is passed over at 3 667, five decisions after its first walk, and the K-bot lab is ordered at 8 377 where the base, its counter put back before it ever left ring 0, ordered the vehicle plant at 8 917 once the base box had grown; the first army unit is done at 12 900 for 11 610. The naval turtle and the artillery creep part at 11 917 and 16 987 with equal figures; the Luschaven openings hold 12 units at 6 000 both ways, with 4 extractors for 5. Beside the tier, whole games: Metal Heck ac-1 won at 53 992 for 73 238, and ca-2 at 51 819 for 43 923 -- its opening equal to 4 319, where the first wind generator stands at the shared ring 1 the plant's order no longer puts back, 10 units at 6 000 both ways, the second plant at 11 444 for 11 654.

**Gaps.** `ring_grow` is still one counter for every type that is no plant: on a metal map an extractor's order puts the wind generator's climb back, and the first wind generator of Metal Heck is ordered when the first plant is done (4 319), not at the first stall (1 754). It keeps the ring the walks under a far record climbed until one of them builds: in ac-1 the first wind generator after the record is ordered at ring 8 (6 697) and begun at 7 380, where the base, its counter put back by the vehicle plant's pass-over, began one at 6 150. A plant search with no site spends the decision's site tests over every ring from the base at each decision that asks it. A type passed over sooner is a different first plant where the first choice has no site yet (Steel Jungle, above). A plant ring left for ground threat keeps its growth until the threat leaves. The fixture (`impai_brain_test.c`, `check_plant_ring` and `check_plantwalk`) and `tools/impai/mutations/plant-ring.py` hold the rules; the tier's openings `mapped-luschaven-3` and `mapped-steeljungle-2` and its two metal games show them.

## The finish's yards (2026-11-18)

**The fault.** `bar-hota-troutfarm-ca-2` at 2c386510 reaches the cap with 273 units against 32: from GameTime 35 000 every enemy unit stands in the lake, the dry hunt holds from 40 657, and for 65 % of the 48 000 ticks to the cap the metal store is full (7 254 wasted at 39 000, 56 743 at the cap) while one shipyard builds eight ships.

**The cause, MEASURED 2026-11-18 with a diagnostic build at 0e3ec587** that logs, at every fourth decision from 30 000, the sea want's terms, the factory lists and what the governor answered each idle factory. The tip wins the Trout Farm game on land at 34 081, before any dry hunt, so the trace ran on the one game of the session tier that holds a dry hunt beside a full store, `bar-hota-redtriangle-ac-1` (won at 58 051; dry from 40 860; 20 410 of the 38 700 metal made between 42 000 and 57 000 wasted). The share is of the 22 719 metal wasted in the 334 decisions sampled from 38 000:

| ticks | the term | what it met | share |
|---|---|---|---|
| 43 567 to 58 027 | `want_sea` | dry, the store full, the overflow standing, the energy term holding, nothing pending: off, because one sea plant is counted; the one yard busy at every sample | 77.5 % |
| 43 867 to 57 247 | `want_sea` | the same, the overflow's test failing | 15.6 % |
| 41 827 to 43 567 | `want_sea` | the same, a plant pending (the hover platform's frame) | 4.5 % |
| 45 847 to 57 067 | the governor | the yard idle at three samples, given an army order at each | 1.5 % |
| the same spans | `want_factory`, the metal term | never needed: five or six factories idle at every sample -- three K-bot labs, the vehicle plant, the advanced vehicle plant and the hover platform, each answered "no type" by the governor, the dry menu holding water weapons alone | inside the rows above |

The yard itself runs without a pause: on the Trout Farm recording a CORSUB is finished every 3 240 ticks and a CORROY every 4 440, the built fraction linear. In that dry hunt nothing but the first yard spends: a land plant is offered only its types with a water weapon, and none of those six has one the governor takes.

**What is taken.** The finish wants one more sea plant while the overflow stands, every sea plant the census counts is a finished factory at work, the energy term holds and an enemy on or under the water is known ([the brain](brain.html), *the finish's yards*). It is the surplus plant's own rule with its idle test read over the sea plants, so its bound is that rule's: income nobody asks for, one plant at a time. The target term is the yard's reach: a shipyard's army hits what floats or stands under the water, and the dry hunt also holds over an enemy on land the land army does not reach.

**The games.** On 2c386510's lineage (`impai_build.c` of that commit under the rule) the Trout Farm game is won at 57 520: six yards by 55 320, 8 602 metal wasted between 40 000 and 57 000 against 16 678, and a sea wave of six to nine ships leaves at 53 438 and holds its attack to the end, the enemy commander dead at 57 520 (without the rule the first sea wave, of five, leaves at 60 578 and retreats). At the tip the rule acts in one game of nineteen played: the session tier against the champion 0a4ec04e reads 3 SAME, 2 BETTER, 7 PARTED, every game ending at the tip's own tick but Red Triangle ac-1, and Red Triangle's seven other games against OTA AI end at the tip's ticks, none of them ever dry.

**What it does not do.** Red Triangle ac-1 is won at 74 077 with the rule, 16 026 ticks later than without. From 42 000 the enemy commander stands on the west island at blocks (32..63, 43..85) and rebuilds there on dry ground -- 6 units at 42 000, 43 at 68 000 --, where no land unit of the seat stands and the seat's nearest unit is 23 to 62 blocks from every enemy unit until 72 000 (sampled every 1 500 ticks); the seat holds one ghost row and then none, and an enemy on the water is counted until 48 690 [INFERRED: the floating metal maker, alive to about 48 000], so one is known until 54 090. The seven yards the rule orders in that time buy ships that reach nothing there; the game ends in both builds when the enemy comes back within reach (without the rule the commander is at block (80,68) at 57 000 and dead at 58 051 [INFERRED: it walked out]; with the rule a land enemy is seen at 67 350, the dry hunt ends and the land plants finish it). Without the target term the same game reaches the cap, ten yards and 335 units against 61. An enemy on land that neither the land army nor the ships reach is not answered by any rule here: the dry hunt reads it as off the land and buys water weapons, and no scout is sent to look -- the blind spot's areas are the estimate and the starts.


## A paralysing weapon deals no damage (the owner, 2026-11-18)

**The owner's decision.** After a base-game game of three Hard seats in which the Arm seats "loved" tier-2 Spiders: the seat builds no unit whose weapons only stun until it has a use for them.

**The fault, MEASURED 2026-11-18** on that game's recording and with `tasim_run roles`: ARMSPID's one weapon, ARM_PARALYZER, carries `paralyzer` and `damage=900` at a 36-tick reload. The engine applies a paralyser's hit as damage kind 2, which takes no hit points ([weapons](../engine/weapons.html), [network](../engine/network.html)): the 900 takes none [INFERRED: it is the paralysis' length]. The classifier summed it as damage -- 25 a tick against ARMBULL's 3.5 and ARMFLASH's 2.0 -- and the fight predictor returned it as damage, so the Spider's value at tech 2 was +0.502, the best of the Arm tech-2 menus by far, and it alone made the advanced vehicle plant Arm's tech-up plant (`plant ... want=tech T=2 chose=ARMAVP s=0.502 next=ARMALAB s=0.068`). One seat of the owner's game spent a third of its army's value on Spiders, the other a ninth; in the session tier's three Arm games that reach tech 2 the champion 398a3664 begins 14, 10 and 8.

**What is taken.** One meaning of a paralyser's damage everywhere the brain reads a weapon's damage: none ([the brain](brain.html), *A paralyser deals no damage*; [the calibration runner](../modules/tasim-calibration.html), *Roles*). The classifier adds a regular paralyser to no damage figure and no range, the ground reach leaves it out, and the predictor's damage on any target is 0. Nothing names a unit: the bit is the weapon record's own, already copied by both role fillers and stored in the fight content.

**What each type becomes** (`tasim_run roles` before and after; no other row and no percentile moves in either game set):

| | type | before | after |
|---|---|---|---|
| base game | ARMSPID (tech 2) | raider, 25.0 a tick over 220, land raider | raider, 0 over 0, **no class** |
| | ARMEMP (stockpile) | other, 0 | the same |
| Escalation 10.2 | ARMSPID (tech 2) | raider, 1.67 over 270, land raider | raider, 0, **no class** |
| | ARMCBT (tech 3) | assault, 0.74 over 1 050, land assault | assault, 0, **no class** |
| | MSCORP (on no menu) | raider, 1.67 over 270, land raider | raider, 0, **no class** |
| | ARMSTUN (tech 3 building) | defence, 3.75 over 1 050 | defence, 0 over 0: no ground reach, so never the defence rung's type |
| | ARMHSTUN (two paralysers and a gauss gun) | raider, 10.2 over 288, land raider | raider, 6.9 over 288, land raider |
| | ARMASPID (a paralyser beside its guns) | artillery, 302.6 over 750 | artillery, 300.6 over 750 |
| | the 26 plants with the CommandFire `LAB_DIR` | builder, 0 | the same |

No reference fight of the calibration holds a paralysing type in either game set, so no reference and no fitted figure moves; the fight content is the same file, the rule reading its flag.

**The values after it.** ARMSPID leaves the field, so every tech-2 value moves in its second decimal: ARMAVP scores -0.037, ARMALAB +0.074 (0.068 before), CORALAB +0.484 (0.463), CORAVP -0.042 (-0.028) (the games' plant lines; by an offline approximation over the whole field the best types are ARMBULL and ARMZEUS); retail scores 46 types in 3 496 runs. Arm's tech-up plant is the advanced K-bot lab wherever a constructor of the seat's can reach both.

**The games** (the session tier against the champion 398a3664, `.data/impai/runs/session/item14-a` against `session/base-f9b5b32a4c70`): 6 SAME, 9 PARTED, none in a worse category, and no ARMSPID begun in any game.

| game | 398a3664 | with the rule | what changed |
|---|---|---|---|
| Red Triangle ac-1 | won at 74 077; ARMAVP; 14 ARMSPID, 7 ARMBULL, 3 ARMMERL | won at 40 210; ARMAVP (the one tech plant in reach, s=-0.037); 10 ARMMERL, 6 ARMBULL | 33 867 ticks sooner |
| Plains & Passes ac-1 | won at 36 363; ARMAVP at 11 542; 10 ARMSPID, 4 ARMMERL | won at 43 017; ARMALAB at 11 767; 29 ARMZEUS | 6 654 later; the army at 36 000 is 102 against 58, the metal made within 3 % |
| Metal Heck aa-2 | won at 40 771; ARMAVP at 11 189; 8 ARMSPID, 11 ARMMERL | won at 53 343; ARMALAB at 14 039, after an ARMCK is built to unlock it (11 909); 38 ARMZEUS, 30 ARMSH | 12 572 later; 2 850 ticks to the tech plant's order, the army at 36 000 42 against 71 |
| Core Prime cc-1, Lava Mania ca-2 (ProTA) | won at 48 285, 43 160 | the same ticks | only the plant line's two scores differ |
| Trout Farm ca-1 | won at 47 710 | the same tick | `naval` 1 -> 0, below |
| the air rush, the artillery creep | pass | pass | the tech plant is ARMALAB |

**What it does not do.**

- **The two later wins are the advanced K-bot lab's**, not a paralyser's: with the Spider gone the values rank the lab's best type above the vehicle plant's, and the two games that could choose took the lab and won later, while the game that could only take the vehicle plant won far sooner without Spiders. Eight more Arm games of the champion's batch played with the rule (`.data/impai/item14/wide`, every one won): the six that take the lab end at +2 333 (Lava Mania ac-1), +4 771 and +168 (Plains & Passes aa-1, ac-2), +3 660 and -6 507 (Trout Farm ac-1, aa-2) and -14 821 (Pin Cushion ac-2) ticks from the champion's; the two on Red Triangle, the vehicle plant alone in reach, at +3 606 (aa-2) and -6 556 (ac-2). Over the eleven Arm games seven are won later and four sooner, the median +2 333; of the eight that take the lab six are later, their mean +1 104. One game an id cannot tell a slower plant from another game's dice: the paired margin test judges the time to win, and no reference fight checks ARMZEUS, ARMFIDO or ARMFAST ([calibration](calibration.html)).
- **An enemy's paralysers count for nothing**: an enemy Spider stamps no threat, is not armed in the picture, presses nothing, and the predictor takes it as a target that returns no damage. A stunned unit neither moves nor fires, and nothing in the brain models that. Whether an enemy's stun must count as a threat is not decided.
- **Trout Farm's naval judgement turned on the Spider.** The land link asks whether at least half of the land army types can walk from the anchor to the enemy's estimate; there 28 can, of 57 with ARMSPID and of 56 without, so the seat judged the map naval before and judges it land now. The games' orders are the same (no sea site was ever found there); the judgement stands on one type's count.
- A mixed type (Escalation's ARMHSTUN, ARMASPID) is bought on its other weapons; its stun is neither valued nor used.

## A floating building is a water type (2026-11-18)

**The owner's report**, from an Escalation game: "the AI never builds energy producers. It does a couple of metal extractors and then jumps to a kbot lab."

**The fault, MEASURED 2026-11-18** with a scratch trace of the two energy rungs' answers (the builder, the type chosen, the ring walk's counts and return) on `bar-nota-pincushion-ac-1` and `-ca-1`, `--setup escalation`, to GameTime 12 000:

- The map's mean wind ratio is 0.3499. On the commander's menu the Floating Wind Generator (ARMFWIN: 36 of wind, value 68.5, a gain of 12.60 a pass) outranks the Solar Collector (25 over 157.7) and the Wind Generator (10.50 over 68.5): 0.184 a value against 0.159 and 0.153. It does so on every map whose ratio is above 0.302. The constructor kbot's menu has no floating generator, and its choice was the solar collector.
- ARMFWIN has `minwaterdepth = 0` and a yard of `w`. The type information flagged a type as placed on water only by `minwaterdepth > 0` or a tidal generator, so the commander's energy choice was ARMFWIN on a dry anchor, and the site test refused every site: in ac-1 the stall's energy rung asked the commander at 203 decisions from GameTime 3 127 to 11 647 and the energy rung at 7 where it wanted energy, every one ARMFWIN, not one site passed (ca-1: 263 and 10).
- Those walks spent the decision's site tests (32 a decision): 20 of the stall's walks exhausted the base ring and grew it, until its sites passed the commander's reach, and the 183 after them tested what the decision had and left the ring as it was. So a builder asked after the commander in the same decision tested nothing: the constructor's solar collector was cut with no site tested at 21 of its 25 asks (ca-1: 25 of 30), and the commander ended 130 decisions with no order (ca-1: 175) [INFERRED: its other rungs' walks were cut the same way].

**What is taken.** The flag is read from what the site test reads ([the brain](brain.html), *Placed on water only*): a building of minwaterdepth 0 whose yard map has water cells and no land cell is placed on water only: the site test's last compare holds every cell of such a footprint at or below the sea. Considered and not taken: a second choice when the best type finds no site -- it would have ended the commander's fault too, at the cost of the 32 site tests a decision the refused type's walk spends before the second is tried, and it leaves the type in every other choice that reads the flag; the flag is the cause, and with it right the first choice is one the ground takes.

**The figures named before the try**, both games to GameTime 12 000, the tip 789a3231 and then the rule:

| | ac-1 (Arm) | | ca-1 (Core) | |
|---|---|---|---|---|
| | before | after | before | after |
| the commander's first energy BUILD job | none | 3 150 | none | 3 240 |
| the first energy building begun | 5 949 | 3 173 | 6 369 | 3 479 |
| energy buildings begun | 3 | 7 | 4 | 5 |
| energy income at 6 000 / 12 000 | 64 / 130 | 164 / 255 | 64 / 125 | 139 / 185 |
| energy made by 12 000 | 29 322 | 54 192 | 29 396 | 48 636 |
| samples with the store below 20 % (30, every 300 ticks from 3 300) | 23 | 7 | 29 | 8 |
| decisions with the store empty (`e_empty`, of 406) | 134 | 38 | 194 | 37 |
| the second plant ordered | 10 447 | 5 527 | none | 5 587 |
| army units ordered | 6 | 4 | 7 | 8 |

The army in ac-1 is two fewer at 12 000: with the energy the seat spends its metal -- the store, above 1 000 of 1 125 from 5 100 to 10 500 before, holds 0 at 10 500 and 8 at 12 000 -- and it orders the advanced lab at 8 197 (ca-1 at 10 927), which the tech-up's rule takes at this income; that rule is not this change's.

**The same two games played whole** (to the commander's death or GameTime 81 000, against OTA AI on Escalation; MEASURED 2026-11-18, one game each): neither build wins one, and the rule does not change that.

| | 789a3231 | the rule |
|---|---|---|
| ac-1 (Arm) | lost at 37 981: 22 army units built, metal income at most 20.9, no attack | lost at 72 698: 198 built, at most 59.3, 7 attacks from 29 835 |
| ca-1 (Core) | at the cap, behind: value 34 415 against 107 240, 297 built, at most 68.9, 3 attacks from 30 105 | **lost at 70 041**: 212 built, at most 55.0, 3 attacks from 16 830 |

In ca-1 the rule's seat is ahead of the other build's to 36 000 (metal income 41.7 against 28.2 at 24 000 and 49.8 against 41.5 at 36 000, energy 653 against 485 and 809 against 669) and behind it from 48 000 (39.7 against 53.9, then 18.3 against 66.8 at 60 000), while its opponent's energy income passes 2 400 and its own stays below 1 100 in both builds: by the session tier's categories that game is worse with the rule, at the cap behind for a loss. Why the later game turns is not read here.

**The base game.** No type of the base game or of ProTA 4.8 changes its flag, so the rule cannot fire there: `session.py --neutral --state` against 789a3231 holds it, every game SAME in its decisions and its recorded brain state.

**What the two whole games with the rule rest on** (MEASURED 2026-11-18 from their recordings: the player records' lifetime totals and the unit images begun and ended). Not on the flag's path through the military: neither opponent began one floating generator or radar tower, Pin Cushion has no sea block, and the seat ordered no sea plant and bought land types alone. They rest on an economy that stops growing at 30 000. In ca-1 the seat makes 51 metal a second at 30 000 against its opponent's 53, with 39 extractors against 17, and never more: it builds no advanced constructor and no moho extractor in 70 000 ticks -- its seven constructors, the attention's count beside the commander, are all of the first tech by 24 000 and stay --, while its opponent goes from 3 moho extractors and 7 metal makers to 6 and 14 and from 53 to 155 a second. In ac-1 two advanced constructors are among the seven and begin seven moho extractors from 25 800 on; the seat makes 41 a second at 30 000 and 54 at most, its opponent 49 and then 270 with 22 moho extractors. Energy is not what binds: the seat wastes 11 % (ac-1) and 18 % (ca-1) of the energy it makes, its store is at 90 % or more at 8 and 11 of the 13 samples from 24 000 to 60 000, and every energy building it begins is a solar collector. Its army is never above 38 alive in ca-1: from 30 000 the seat begins 22 to 34 units every 6 000 ticks and loses as many, and its opponent begins 30 to 59 beside 3 to 19 defences.

**Gaps.** No whole Escalation game is won, with the rule or without, and one of the two is lost that reached the cap. **Everything that reads the flag** takes the changed types: the energy, defence and sensor choices and the sensor's site walk, the extractor's ranking (a water extractor with no depth above 0 is passed over), the plant candidates and the island's order, and the military's target class -- an enemy's floating generator is a FLOAT target (`targets_known`), which makes the island's order the sea first, lets a dry hunt want a further sea plant, and widens the army's buy to the types that hit only what floats; no game played shows that path, the two whole games above having no enemy floating building. The floating radar towers (ARMFRAD, CORFRAD) are not flagged -- their minwaterdepth is −10 000, where no bound holds without the waterline ([the brain](brain.html), *Placed on water only*) --, so the sensor choice can take one on a dry anchor and find no site; no game showed a sensor want at a shore. TA Twilight's six floating mines change with Escalation's two generators and no Twilight game was played. On a dry anchor with a shore inside the ring the flag costs the seat the floating generator it could have stood there, on Escalation and TA Twilight both: the choice passes over a water type unless the anchor's own ground is under the sea. Escalation's fusion reactors carry `Builder = 1` and build their own upgrade, so the classifier flags them factories on the live table (MEASURED 2026-11-18: ARMFUS, CORFUS, ARMSFUS, CORSFUS category factory, gains 1 200 and 6 250 a pass) and the energy rung never chooses one: not changed here.

## The opening's order (the coordinator, on the owner's report, 2026-11-18)

**The report.** The owner: the AI opens with "a couple of metal extractors and then jumps to a kbot lab" with no energy standing, and it plays metal maps badly.

**The fault, measured.** The plan's terms for the first factory are `open_mex` extractors finished and `open_energy` energy buildings standing, or `open_t`. Nothing made the opening's builder meet them in that order, and two rungs of the ladder stood in the way.

- **A commander with more than `open_mex` patches within `comm_r` walked patch to patch.** The energy term was then met by `open_t` alone, the plant was ordered with no energy standing, and the first energy building came on the first stall the plant caused. Escalation on Pin Cushion (`bar-nota-pincushion-ac-1` and `-ca-1`, to 12 000): four extractor images and no energy image before the K-bot lab's order at 2 160 / 2 190, the commander's first energy job at 3 150 / 3 240. In the owner's base-game match of the same day each commander had two patches in reach, the extractor rung had nothing after the second, and the wind generators at 717 to 851 came from the ordinary energy rung -- so the base game opened as the plan says only where the reach held no third patch.
- **On a metal map the extractors stand on the base ring, so the commander never runs out of them.** Metal Heck (`bar-hota-metalheck-ca-2` and `-aa-2`, to 12 000): five extractor images and no energy before the vehicle plant at 2 474 / 2 324, the first wind generator begun at 4 440 / 4 110, the energy store empty in 0.62 / 0.53 of the first 10 000 ticks -- extractors whose upkeep nothing paid --, the first army unit at 9 660 / 8 550.
- **Where energy did come early, the stall's rung answered its cost with more energy before the plant was asked.** The opening's energy buildings cost energy, so they bring the first stall forward, and the stall's energy is the ladder's top rung: each idle decision of the stall took the commander for one more. With the opening's energy alone (the first rule below, played without the second): four or five energy buildings begun before the first plant on Red Triangle (first stall 2 017 -> 937), Core Prime and Metal Heck (1 424 -> 734), and on Luschaven's start 3 five and no plant by 6 000. On Metal Heck, with the energy walk given a ring growth of its own (played for *The plant's ring is its own*, not kept), the wind generator's walk finds a site in the stall and the rung orders six running before the vehicle plant, at 5 459 for 2 474.

**The decision** (the coordinator's): the opening's builder meets the plan's own terms in their order, and in the opening the stall's energy does not stand above the first plant for ever. Both rules read the terms where the ladder already reads them -- `impai_econ_open_mex` and `impai_econ_open_energy` are their only readers -- and add no number.

- **The opening's energy** (`impai_econ.c`, THE OPENING'S ENERGY; the rung `open-energy`, second in the ladder). While the seat has never had a factory and none is ordered, once `open_mex` extractors are finished and until `open_energy` energy buildings are counted or ordered, a builder's next building is energy, before a further extractor and before the plant. **Bounds**: it ends for the level with the first factory counted, so at most `open_energy` buildings. **The invariant for a seat no site takes energy for**, an ordering: the first walk of the rung that orders nothing ends the rule for the level (`IMPAI_BRAIN.open_e_refused`); such a seat pays one decision's site tests at most, once.
- **The stall yields to the first plant** (THE STALL YIELDS TO THE FIRST PLANT; the rows `stall-energy` and `stall-yielded`). While the seat has never had a factory and none is ordered, once `open_energy` energy buildings are counted or ordered the stall's top row passes a builder on to the plant's rows, and the row under them asks it the stall's energy in the same decision when they gave it no order. **The invariant, so that no seat is left stalled behind a plant it cannot place**, an ordering: the first time the plant's rung is asked of a builder passed on and gives it no order, the yield is over for the level (`IMPAI_BRAIN.open_stall_over`), whatever ended the walk. A plant's walk with no site takes the decision's 32 site tests, so in that one decision the stalled builder may be ordered nothing; from the next the stall's rung is the top rung, as past the opening. **Bounds**: one plant at most is put ahead of the stall's energy; after the first factory is ordered or counted the stall's rung is what it was. A reclaimer the stall would take keeps its reclaim while the yield stands and is asked the plant at its next idle decision.

**Why not the energy term alone, or the stall rung moved for good.** The first rule alone was played on an earlier tree and not kept: three tier games worse -- Luschaven's start 3 twice, its plant search then sharing one ring growth with every build between its walks, and Core Prime at the cap --, the plant behind four or five energy buildings. The stall's rung below every plant past the opening would let a stalled seat order plants its energy cannot run, which THE STALLS exist to refuse; the yield is the opening's alone and ends at the first factory's order.

**Measured** (2026-11-18, the two rules against the tree without them, one game each from its seed; a capped opening is exact).

| game, to 12 000 | first energy begun | first plant ordered | extractor / energy images before the plant | first army unit | army at 12 000 | energy store |
|---|---|---|---|---|---|---|
| Escalation, Pin Cushion `ac-1` | 3 173 -> 812 | 2 160 -> 1 500 | 4 / 0 -> 2 / 2 | -- | 4 -> 7 | below 20 % in 7 -> 6 of 30 samples |
| Escalation, Pin Cushion `ca-1` | 3 479 -> 843 | 2 190 -> 1 560 | 4 / 0 -> 2 / 2 | -- | 9 -> 9 | 11 -> 6 of 30 |
| Metal Heck `ca-2` | 4 440 -> 660 | 2 474 -> 1 349 | 5 / 0 -> 2 / 2 | 9 660 -> 5 370 | 2 -> 8 | empty in 0.62 -> 0.16 of the first 10 000 |
| Metal Heck `aa-2` | 4 110 -> 630 | 2 324 -> 1 229 | 5 / 0 -> 2 / 2 | 8 550 -> 5 580 | 5 -> 7 | 0.53 -> 0.20 |

The plant is sooner in each because the terms are met before `open_t`. **The cost is early metal**: two extractors stand for four or five while the energy and the plant are built -- Escalation's metal income at 3 000 is 6.5 for 8.6 / 8.1 (equal or above from 6 000: 9.3 for 9.3 / 8.8), Metal Heck's at 6 000 is 5.9 for 12.5 in `ca-2` (above from 12 000: 20.2 / 24.8 for 15.1 / 13.0). Escalation's energy income at 6 000 is lower, 115 for 164 / 139: the four solar collectors the stalls of 3 150 to 5 130 ordered are not built; at 12 000 it is 285 for 232 / 211. Luschaven's start 3 (the tier's opening, to 6 000): the vehicle plant at 1 731 for 3 336, two energy buildings before it for four, 17 units for 12, six extractors for four.

**The session tier** against the tree without the rules (fifteen games, one each): 5 the same, 2 better, 8 parted, none worse by its categories.

| game | first energy job | first plant job | images begun by 6 000 | metal income at 6 000 | the end |
|---|---|---|---|---|---|
| Red Triangle `ac-1` | 2 040 -> 720 | 3 840 -> 1 380 | 11 -> 12 | 6.3 -> 7.2 | won 40 210 -> 30 019 |
| Lava Mania `ca-2`, ProTA | not read | parts at 2 294 | 9 -> 10 | 3.6 -> 5.1 | won 43 160 -> 38 114 |
| Plains and Passes `ac-1` | 3 030 -> 1 350 | 2 100 -> 2 490 | 13 -> 13 | 6.6 -> 9.5 | won 32 303 -> 36 031, later |
| Core Prime `cc-1` | 840 -> 540 | 3 420 -> 1 800 | 16 -> 18 | 19.4 -> 12.8 | won 32 999 -> 60 020, later |
| Metal Heck `aa-2` | 4 020 -> 540 | 2 340 -> 1 230 | 11 -> 17 | 1.0 -> 5.9 | won 43 588 -> 71 733, later |
| the air rush | 3 750 -> 1 350 | 2 370 -> 2 130 | 10 -> 18 | 1.0 -> 4.6 | passes both; the opponent's units at 18 000 24 -> 19 |
| the artillery creep | 3 750 -> 1 350 | 2 370 -> 2 130 | 11 -> 17 | 6.5 -> 4.6 | passes both; value 34 483 against 27 658 -> 35 690 against 27 105 |
| the naval turtle | 3 960 -> 840 | 1 980 -> 1 500 | 10 -> 11 | 6.3 -> 8.6 | fails both; first attack 6 960 -> 5 220 |
| Luschaven start 3, mapped and unmapped | 990 -> 630 | 3 360 -> 1 740 | 12 -> 17 | 9.1 -> 11.3 | better: 17 units for 12 |

The same: the commander rush, Trout Farm `ca-1`, Steel Jungle `ca-1` and the two slow openings, whose seats have one extractor finished by 6 000 and so never meet `open_mex`.

**Gaps.**

- **Both metal-map games of the tier are won much later with the better opening**: Core Prime at 60 020 for 32 999 and Metal Heck `aa-2` at 71 733 for 43 588, one game each. Why is not read. At the end the seat holds 307 and 231 units against none, and on Core Prime 326 against 41 at 60 000, so the game is not close when it ends; the same was seen with the first rule alone (Core Prime not finished at 81 000 with 543 units against 58). The capped metal-map openings above say the opening is the better one; what the later games do with it is open.
- **Past `open_t` the opening's energy still comes before the plant.** On Plains and Passes the second extractor is finished late, the two wind generators are ordered from 1 350, and the plant's job is at 2 490 for 2 100: 390 ticks later than `open_t` alone ordered it, in a game won 3 700 ticks later.
- **The cost in early metal** is in every opening that parted: two extractors stand where three to five stood.
- No whole Escalation game was played for these rules; the two capped openings are all that is measured on that setup.
- The energy walk's ring growth is still the one every type but a plant shares, put back by each extractor's order on a metal map. Given a growth of its own on top of these rules it moved nothing in the four capped openings before 6 000 and was not carried to a tier.
