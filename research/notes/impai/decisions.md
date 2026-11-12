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
