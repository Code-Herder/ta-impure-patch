# ImpAI for the player

What a player sees of ImpAI: the AI button, the levels, and how ImpAI behaves in recordings, saved games, alliances, network games and mods.

## The AI button

- A computer player's AI button on the skirmish screen switches between **ImpAI** and **OTA AI** (the stock AI). A seat that plays OTA AI runs the stock code untouched.
- **OTA AI is the default** from A1, when the switch lands. **The default becomes ImpAI** at A2, once every level passes [the bar](bar.html).
- Which `SKIRMISH.GUI` gadget a seat click cycles is not documented yet; L0 finds it ([entering the game](engine-seam.html)).

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
- **The handicap exemption:** stock Difficulty scales every AI player's income, reclaim, transfers and build credit (50 % on Easy, 70 % on Medium). Those sites skip ImpAI seats, so ImpAI always plays at full rate. OTA AI keeps the stock handicap. This is a simulation change, with replay test groups ([entering the game](engine-seam.html)).

## Behaviour around the game

- **No memory between games.** ImpAI picks its opening from the map and from scouting, with a fixed seed per game, so every game is reproducible.
- **Allies:** two allied ImpAI seats, or ImpAI allied with a player, are separate brains. They share only what the game shares with allies (line of sight and radar) and take no orders from the player.
- **Recordings:** the recording's header names each seat's AI and level. On a replay puppet ImpAI does not run, because the recorder is the only writer. A takeover or rewind resumes ImpAI's `SIM_STATE` at that tick.
- **Saved games:** on load, ImpAI rebuilds its view from the world: its maps are derived, and its tasks are reassigned. It forgets only what it scouted and no longer sees. The save format does not change.
- **Network games** come last (A4). An ImpAI seat runs on the peer that hosts it, as stock AI seats do.
- **Mods:** every setup of the compatibility suite where Impure runs a game. One tuning table serves every mod, through roles derived from the units' data. A mod passes when ImpAI never stalls in it and no key its units use is unmodelled ([tasim: scope](tasim.html)).
- **Cost:** at most 1 ms per ImpAI seat per simulation tick, worst case, on the reference setup, measured on a ten-seat stress scenario. Work that does not fit is spread across ticks by a fixed schedule, never by a clock.

## Not now

- The threat-map overlay is a `tacli` test lever, never a player setting.
- A wiki entry for ImpAI is decided at A2.
