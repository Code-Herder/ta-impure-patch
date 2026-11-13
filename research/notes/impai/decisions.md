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

- **An ImpAI seat keeps the stock weapon scan** `0x4089A0(obj, 1)`, so CommandFire weapons fire on their own as they do for OTA AI, over making ImpAI fire them by order only.
- **ImpAI owns its units' standing states**: the stock group assignment `0x408830` does not run on an ImpAI seat, over keeping it for parity. It would force fire at will and write the unit's group number, the field a human's Ctrl groups use.
- **The `AI` chat command still starts the stock AI**, over choosing per seat which AI it starts.
- **OTA AI keeps its stock behaviour**, including a target picker that sees every enemy unit. The asymmetry with ImpAI's vision rule is a fairness choice, not a defect.

## The bar

- **100/100 at every level against OTA AI on the same Difficulty**, over the first proposal of at least 70 % against OTA Hard. The owner judges OTA AI too weak to lose to, so any loss is a defect, not noise. This is to be re-evaluated once results exist.
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
- **The predictor's 90 % check judges group, naval and defence fights as well as duels**, on fights held out of the fit; the duels-alone figure is reported beside it. Stricter than the landing row, chosen because ImpAI decides group fights.
- **A3's "the win holds at rung 2"** is a pre-registered set of 400 engine games, passing at 224 wins or more (p < 0.01), plus a re-run of the bar with the tuned table.
- **A4's coverage** is every unit type of a supported mod classified or refused by name, and the compatibility suite's ImpAI stage passing.
- **A gap accepted for A4:** in a network game ImpAI runs on the peer that added its seat, as the engine's own AI seats do, and a multiplayer recording carries ImpAI's tables only as the recording peer held them: that peer's recorder images every `SIM_STATE` table whole, as it does in a skirmish, and the other peers' contributions carry no table, so another peer's ImpAI state is in no file but that peer's own. Its units' orders replay exactly; a replay or takeover on another peer cannot rebuild ImpAI's own state.
- **The specs' smaller recommendations are accepted as written:** the stall rule, the bar's seating and settings and its five maps, the level lever, the always-on pulse line, the bench's game count, the tuning's machine-time budget, the network toggle and seat name, the network level, and the length of the suite's ImpAI stage.
