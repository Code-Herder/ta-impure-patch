# ImpAI for the player

What a player sees of ImpAI: the AI button, the levels, and how ImpAI behaves in recordings, saved games, alliances, network games and mods.

## The AI button

- **The seat's button is the `SKIRMISH.GUI` gadget `Player<N>`.** Stock cycles it Open → Computer → Player → Open, Player only while no other seat is the player ([the GUI](../engine/ui.html), *The SKIRMISH screen's handler*). ImpAI splits the Computer step in two: **Open → ImpAI → OTA AI → Player → Open**. Until A2 the Computer step offers OTA AI first, **Open → OTA AI → ImpAI → Player → Open**; at A2 the order flips, so ImpAI is the first computer step. Player is skipped, as in stock, while another seat holds it.
- **OTA AI is the stock AI and keeps its stock behaviour**, including that its target picker sees every enemy unit on the map (no sight, radar or explored-map test). A seat that plays OTA AI runs the stock code untouched.
- **OTA AI is the default**: `seatai`'s default is `none`. **The default becomes ImpAI** at A2, once every level passes [the bar](bar.html).
- **Where the choice lives.** Both kinds are a Computer seat to the engine: the skirmish entry and the record's controller `+0x73` stay 2 for ImpAI and OTA AI alike, so every stock reader of the seat sees what it always did ([sim](../engine/sim.html) lists the engine's other tests of the controller against 2). The per-seat choice is the `impure.cfg` key `seatai`, the seats ImpAI plays (`seatai=1,3`, or `none`), honoured under every launch, `--defaults` included, as the engine honours its own SKIRMISH settings. The level init copies it into a `SIM_STATE` table; a replay restores that table from the recording, whose header also names each seat's AI. Until the button lands (A1b) the key is set by hand or by a scenario's `impai` seats.
- **The `AI` chat command** turns a seat into the stock AI, OTA AI, as it does in stock: it also clears the seat's ImpAI mark, so a seat taken over as ImpAI's and given back with `AI` goes to OTA AI.

## Levels

- **The level is the skirmish screen's global Difficulty**: Easy, Medium and Hard pick ImpAI's Easy, Normal and Hard. Stock TA has no per-seat level, and neither does ImpAI. ImpAI follows the remembered `SkirmishDifficulty`, so there is no separate default.
- **A level changes behaviour only:**
  - reaction delay (ticks before it responds to what it sees);
  - an order rate cap (orders per second);
  - micro (pulling back damaged units, focus fire);
  - noise in the fight predictor's and the economy's judgement;
  - attack thresholds and scouting frequency.
- **One brain:** a level is a row of these parameters, held as data. There is no separate easy code.

## No cheating, in either direction

- ImpAI gets no vision, resource or unit advantage, and no resource penalty either.
- `WORLDQ` returns only what the seat sees: its own units, what the game shares with allies, and enemies in its line of sight or on radar. The rule holds by construction, in the game and in tasim alike.
- **The handicap exemption:** stock Difficulty scales an AI player's resources at seventeen sites: the eight incomes of the economy pass, the build-cancel and unbuild refunds, the reclaim payout's energy and metal, both resource transfers, the Deathmatch respawn's two grants and the credit for a reclaimed unit — × 0.5 on Easy, × 0.7 on Medium, and nothing on Hard, which is unscaled at every site ([sim](../engine/sim.html), *Difficulty and the AI handicap*). Those sites skip ImpAI seats, so ImpAI always plays at full rate. So does the stock AI's on-hit commander purge: a hit OTA AI commander drops its orders and its planners pause, an ImpAI commander, like a human's, does not. OTA AI keeps the stock handicap and the purge. This is a simulation change, with replay test groups ([entering the game](engine-seam.html)); the transfers and the respawn are not exercised by any test.

## The stock AI's machinery on an ImpAI seat

What stock AI code runs on a seat ImpAI plays, now (A1a) and once ImpAI builds (A1b):

| | A1a | A1b |
|---|---|---|
| the group assignment `0x408830` and the ten planners | run: the wrapper always calls the stock tick | not run |
| the weapon scan `0x4089A0(obj, 1)` | runs | runs, after the brain |
| the acquisition tick `0x40B2C0(slot)` | runs | runs, as for every seat with an acquisition record, humans included |
| the AI profile's `plan`/`weight`/`limit` parse at the level load | runs | runs; only the stock planners read what it writes |
| the AI object and acquisition record's life | unchanged | unchanged |
| the seventeen Difficulty scalings | skipped | skipped |
| the on-hit commander purge `0x406FC3` | skipped | skipped |
| the engine's other tests of the controller against 2 | unchanged | unchanged |


- **The stock weapon scan.** An ImpAI seat runs `0x4089A0(obj, 1)` as the stock AI does, so its units pick their own targets in range and **fire CommandFire weapons on their own**, as OTA AI's do; a human's scan leaves those to orders.
- **Its own standing states.** ImpAI owns its units' move and fire states. The stock group assignment `0x408830`, which forces fire at will on every unit and writes the unit's group number (the field a human's Ctrl groups use), does not run on an ImpAI seat. The weapon scan retargets a unit on its own only at fire state 2, fire at will ([orders](../engine/orders.html), *Direct AI order activity observer*).

## Behaviour around the game

- **No memory between games.** ImpAI picks its opening from the map and from scouting, with a fixed seed per game, so every game is reproducible.
- **Allies:** two allied ImpAI seats, or ImpAI allied with a player, are separate brains. They share only what the game shares with allies (line of sight and radar) and take no orders from the player.
- **Recordings:** the recording's header names each seat's AI and level, and a replay takes the per-seat choice from it. On a replay puppet ImpAI does not run, because the recorder is the only writer. A takeover or rewind resumes ImpAI's `SIM_STATE` at that tick.
- **Saved games:** on load, ImpAI rebuilds its view from the world: its maps are derived, and its tasks are reassigned. It forgets only what it scouted and no longer sees. The save format does not change.
- **Network games** come last (A4). As with stock AI seats, an ImpAI seat is chosen and run on the peer that adds it in the battle room, which can be the host or a joiner; the other peers see its units through the owner's unit stream and never run it. Across peers a seat is referenced by its DirectPlay ID or its unit block, never by its row, which can differ from peer to peer.
- **Mods:** every setup of the compatibility suite where Impure runs a game. One tuning table serves every mod, through roles derived from the units' data. A mod passes when ImpAI never stalls in it and no key its units use is unmodelled ([tasim: scope](tasim.html)).
- **Cost:** at most 1 ms per ImpAI seat per simulation tick, worst case, on the reference setup, measured on a ten-seat stress scenario. Work that does not fit is spread across ticks by a fixed schedule, never by a clock.

## Not now

- The threat-map overlay is a `tacli` test lever, never a player setting.
- A wiki entry for ImpAI is decided at A2.
