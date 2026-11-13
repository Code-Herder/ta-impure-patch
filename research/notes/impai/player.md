# ImpAI for the player

What a player sees of ImpAI: the AI button, the levels, and how ImpAI behaves in recordings, saved games, alliances, network games and mods.

## The AI button

- **The seat's button is the `SKIRMISH.GUI` gadget `Player<N>`.** Stock cycles it Open → Computer → Player → Open, Player only while no other seat is the player ([the GUI](../engine/ui.html), *The SKIRMISH screen's handler*). ImpAI splits the Computer step in two: **Open → ImpAI → OTA AI → Player → Open**. Until A2 the Computer step offers OTA AI first, **Open → OTA AI → ImpAI → Player → Open**; at A2 the order flips, so ImpAI is the first computer step. Player is skipped, as in stock, while another seat holds it.
- **OTA AI is the stock AI and keeps its stock behaviour**, including that its target picker sees every enemy unit on the map (no sight, radar or explored-map test). A seat that plays OTA AI runs the stock code untouched.
- **OTA AI is the default**: `seatai`'s default is `none`. **The default becomes ImpAI** at A2, once every level passes [the bar](bar.html).
- **Where the choice lives.** Both kinds are a Computer seat to the engine: the skirmish entry and the record's controller `+0x73` stay 2 for ImpAI and OTA AI alike, so every stock reader of the seat sees what it always did ([sim](../engine/sim.html) lists the engine's other tests of the controller against 2). The per-seat choice is the `impure.cfg` key `seatai`, the seats ImpAI plays (`seatai=1,3`, or `none`), honoured under every launch, `--defaults` included, as the engine honours its own SKIRMISH settings. The level init copies it into a `SIM_STATE` table; a replay restores that table from the recording, whose header also names each seat's AI. The SKIRMISH screen's seat button writes it (*The AI button*), and so do a scenario's `impai` seats and `tacli launch --seatai`.
- **The `AI` chat command** (cheats on) never leaves a seat to ImpAI. On a human's seat it hands the seat to the stock AI, OTA AI, as in stock, even a seat a takeover played as ImpAI's; on an ImpAI seat it makes the seat a human's, as it does to an OTA AI seat, and a second `AI` hands it to OTA AI ([ImpAI in the engine](../modules/impai.html), *The `AI` command*).

## Levels

- **The level is the skirmish screen's global Difficulty**: Easy, Medium and Hard pick ImpAI's Easy, Normal and Hard. Stock TA has no per-seat level, and neither does ImpAI. ImpAI follows the remembered `SkirmishDifficulty`, so there is no separate default.
- **A level changes behaviour only:**
  - reaction delay (ticks before it responds to what it sees);
  - an order rate cap (orders per second);
  - micro (pulling back damaged units, focus fire);
  - noise in the fight predictor's and the economy's judgement;
  - attack thresholds and scouting frequency.
- **One brain:** a level is a row of these parameters, held as data. There is no separate easy code. The rows
  and what each holds: [the brain](brain.html), *Levels*. Two test levers, never player settings, set a seat's
  level (`tagpu_impai_level.on`) and response table (`tagpu_impai_table.on`) for the bar's and the tuning's
  ImpAI-against-ImpAI games; the recording keeps both, so a replay plays what the game played.

## No cheating, in either direction

- ImpAI gets no vision, resource or unit advantage, and no resource penalty either.
- `WORLDQ` returns only what the seat sees: its own units, what the game shares with allies, and enemies in its line of sight or on radar. The rule holds by construction, in the game and in tasim alike.
- **The handicap exemption:** stock Difficulty scales an AI player's resources at seventeen sites: the eight incomes of the economy pass, the build-cancel and unbuild refunds, the reclaim payout's energy and metal, both resource transfers, the Deathmatch respawn's two grants and the credit for a reclaimed unit — × 0.5 on Easy, × 0.7 on Medium, and nothing on Hard, which is unscaled at every site ([sim](../engine/sim.html), *Difficulty and the AI handicap*). Fifteen of them skip ImpAI seats, so ImpAI always plays at full rate; the respawn's two grants stay stock, because the respawn only ever creates the controlled player's commander, which no ImpAI seat can be ([ImpAI in the engine](../modules/impai.html), *The handicap exemption*). So does the stock AI's on-hit commander purge: a hit OTA AI commander drops its orders and its planners pause, an ImpAI commander, like a human's, does not. OTA AI keeps the stock handicap and the purge. This is a simulation change, with replay test groups ([ImpAI in the engine](../modules/impai.html), which names the sites no test reaches).

## The stock AI's machinery on an ImpAI seat

What stock AI code runs on a seat ImpAI plays, while ImpAI only observed (A1a) and now that it builds (A1b):

| | A1a | A1b |
|---|---|---|
| the group assignment `0x408830` and the ten planners | run: the wrapper always calls the stock tick | not run |
| the weapon scan `0x4089A0` | runs, argument 1: CommandFire weapons fire on their own | runs after the brain, argument 0: CommandFire weapons only by the military's order |
| the acquisition tick `0x40B2C0(slot)` | runs | runs, as for every seat with an acquisition record, humans included |
| the AI profile's `plan`/`weight`/`limit` parse at the level load | runs | runs; only the stock planners read what it writes |
| the AI object and acquisition record's life | unchanged | unchanged |
| fifteen of the seventeen Difficulty scalings (all but the Deathmatch respawn's two) | skipped | skipped |
| the on-hit commander purge `0x406FC3` | skipped | skipped |
| the engine's other tests of the controller against 2 | unchanged | unchanged |


- **The weapon scan.** An ImpAI seat runs `0x4089A0(obj, 0)` as a human seat does: its units pick their own targets in range, and its CommandFire weapons -- the commander's D-gun -- fire only by order. ImpAI's military orders the D-gun at a target the seat sees now, worth the shot, while its stored energy covers the shot and a reserve ([the brain](brain.html), *The military*). OTA AI's scan fires them on its own.
- **Its own standing states.** ImpAI owns its units' move and fire states. The stock group assignment `0x408830`, which forces fire at will on every unit and writes the unit's group number (the field a human's Ctrl groups use), does not run on an ImpAI seat. The weapon scan retargets a unit on its own only at fire state 2, fire at will ([orders](../engine/orders.html), *Direct AI order activity observer*).

## Behaviour around the game

- **No memory between games.** ImpAI picks its opening from the map and from scouting, with a fixed seed per game, so every game is reproducible.
- **Allies:** two allied ImpAI seats, or ImpAI allied with a player, are separate brains. They share only what the game shares with allies (line of sight and radar) and take no orders from the player.
- **Recordings:** the recording's header names each seat's AI and level, and a replay takes the per-seat choice from it. On a replay puppet ImpAI does not run, because the recorder is the only writer. A takeover makes every other seat a fresh AI, and ImpAI plays the ones your seat choice (`seatai`) names, starting fresh as at a level's start; a rewind resumes ImpAI's `SIM_STATE` at that tick.
- **Saved games:** a saved game does not carry the seat choice: a reload seats ImpAI from the `seatai` in force when it loads, whatever played the seats when it was saved. On load, ImpAI rebuilds its view from the world: its maps are derived, and its tasks are reassigned. It forgets only what it scouted and no longer sees. The save format does not change.
- **Network games** (A4, in part). As with stock AI seats, an ImpAI seat is chosen and run on the peer that adds it in the battle room, which can be the host or a joiner; the other peers see its units through the owner's unit stream and never run it. Across peers a seat is referenced by its DirectPlay ID or its unit block, never by its row, which can differ from peer to peer. In the battle room a click on your own AI row steps it between the two AIs before the next click removes it — Unused → ImpAI → OTA AI → Unused once the first step is ImpAI (`impure.cfg`'s `netai=impai`; OTA AI comes first until then) — and the row's name, `ImpAI:<you>` or `AI:<you>`, tells every peer which AI plays it. Stock lets each peer add one computer seat, refuses one when the game is closed, and in Deathmatch refuses one and removes those already seated. **The level is Hard on every peer**: the host's Start sets Hard, and a joiner would otherwise play at whatever its last skirmish, campaign screen or saved game left, which no player of this game chose. **A peer that leaves takes its computer seats with it**: on every peer that remains the engine removes them with their human and destroys their units where they stand, so no ImpAI seat outlives its peer and none is handed over. A recording carries ImpAI's tables only as the recording peer held them ([decisions](decisions.html), the gap accepted for A4).
- **Mods:** every setup of the compatibility suite where Impure runs a game. One tuning table serves every mod, through roles derived from the units' data. A mod passes when ImpAI never stalls in it and no key its units use is unmodelled ([tasim: scope](tasim.html)).
- **Cost:** at most 1 ms per ImpAI seat per simulation tick, worst case, on the reference setup, measured on a ten-seat stress scenario. Work that does not fit is spread across ticks by a fixed schedule, never by a clock.

### Cost, as measured (observation only, L1)

What a seat's observation costs a tick -- its view's open and its knowledge tick, the `impai: cost` line ([ImpAI in the engine](../modules/impai.html), *The cost*) -- MEASURED 2026-11-12 on the reference setup, each game alone under the run lock, the wall time of `QueryPerformanceCounter` (so a turn the OS preempted counts its wait):

| game | slots | budgets | mean | p99 | turns over 1 ms | max |
|---|---|---|---|---|---|---|
| `impai-cost` (`limits-tier1`, 5 996 kbots, three ImpAI seats, 1 980 ticks, normal speed) | 15 001 | slice 2 048, radar 32 768 | 90-125 µs (160-200 in a first run) | ≤ 512 µs (≤ 1 024) | 2-3 of 1 980 a seat | 1.2-2.4 ms (2.3-3.7) |
| the same, `tagpu_impai_budget.on` | 15 001 | slice 15 001, radar 10^6 | 435-526 µs | ≤ 2 048 µs | -- | 3.3-7.7 ms |
| `impai-observe` (Core Prime Industrial Area, two ImpAI seats, 5 580 ticks, turbo) | 5 001 | slice 2 048, radar 32 768 | 62-70 µs | ≤ 256 µs | 1-2 of 5 580 a seat | 0.6-2.8 ms |

- **The budgets stay at their L1 values** (slice 2 048 slots, 32 768 radar visits, the patch scan's rows and flood): the mean is a tenth of the bound, p99 half of it, and the slice's own share is small -- raising it from 2 048 to 15 001 slots adds about 23 ns a slot.
- **The worst case was not within 1 ms** in these runs: about one turn in a thousand took 1-4 ms. Each of those turns also allocated and zeroed a 639 KB arena, above the process heap's VirtualAlloc threshold; the turn's scratch is static now. The cost line now times the whole turn, the brain's decision included, and the `impai: tail` line counts the slow turns by what they did -- a radar publish (the tick that ends a radar pass publishes every row the pass found: `slots_max` 8 044 = the slice + 5 996 rows on `limits-tier1`), a patch scan's last row, a decision, or none of them, which leaves the OS's preemption ([ImpAI in the engine](../modules/impai.html), *The cost*). The figures with the brain playing are not yet measured; the radar publish stays whole until they show it is the slow class, since spreading it across ticks changes when a seat's knowledge moves.

## Not now

- What a seat knows can be drawn over the game -- a map layer's cells, every unit it holds and how, its metal patches -- but only through the test lever `tagpu_impaiov.on` (`tacli arm <instance> impaiov.on=1:air`), never a player setting ([ImpAI in the engine](../modules/impai.html), *The overlay*).
- A wiki entry for ImpAI is decided at A2.
