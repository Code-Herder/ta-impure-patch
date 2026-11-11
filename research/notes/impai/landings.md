# Landings

The order of work and each landing's exit test. Research step L0 runs beside S1. After S2, S3, L1 and C1, the tasim landings (T) and ImpAI landings (A) alternate, so ImpAI plays real games early.

| Id | What | Done when |
|---|---|---|
| **L0** | Research: the AI seat's lifecycle; the order roots for move, attack, build and repair; the skirmish screen's AI button and where a seat's choice lives; what the stock planners do; the inventory of every FBI, weapon-TDF and `moveinfo` key the engine reads; the handicap's sites | the engine map holds each address with its evidence, the key inventory is a page of it, the seam has its safety argument |
| **S1** | Carve `tagpu_wikisim.c` into the core and the wiki front end, behaviour unchanged | `simparity.mjs` equal natively and under Wine; the wiki gate passes |
| **S2** | The mockup calls the C core through `serve.py`; `sim.js` deleted ([retiring sim.js](sim-js.html)) | every demo plays through the host, the hashes match the pre-S2 ones, no JavaScript simulation left |
| **S3** | The game's rules move above `WORLDQ`; the engine adapter is its first engine implementation ([layers](shared-core.html)) | `place_regression.py`, `resource_regression.py`, `sweep_regression.py` and the wiki gate pass |
| **L1** | ImpAI's world view and spatial maps, read-only, with the overlay lever; the stock AI still plays | the overlay matches a recorded game; the replay gates pass with ImpAI observing |
| **C1** | The calibration runner: scenario reload in a live game, perturbed runs, references, the offline check and its scoreboard ([calibration](calibration.html)) | one scenario of each kind produces a reference and a report; the cost per scenario is measured |
| **T1** | tasim, the ground game: every key loaded or named, terrain and water, ground and sea movement, the whole economy, all the work orders, every sensor | the economy and movement scenarios are in band; the wiki's demos reviewed on the new movement; at least 20 × turbo |
| **A1** | ImpAI builds: economy, builders, factories; the ImpAI / OTA AI switch with OTA AI the default; the handicap exemption | ImpAI's income at 10 minutes beats OTA AI's on three maps, in tasim and in the engine |
| **T2** | tasim, combat: every ground and sea weapon kind, damage, wrecks, fire, experience, self-destruct, the commander's explosion, the game's end | the duel, brawl, naval and base-assault scenarios are in band |
| **A2** | ImpAI fights: the fight predictor, the military manager, the three levels; the default flips to ImpAI | the predictor's 90 % check; [the bar](bar.html) at every level |
| **T3** | tasim, air and specials: flight, bombers, transports, cloak, stealth, stockpiles, nukes and interceptors, kamikaze | those scenarios in band; no unmodelled key left in the base game |
| **A3** | ImpAI adapts: role composition, response tables, the tournament harness that tunes them, air play | a tuned table beats the hand-written one headless, and the win holds at rung 2 |
| **T4** | tasim, Impure's engine and the mods: extra weapons, raised limits, the defect patches, the TADR sim fixes; the compatibility-suite mods | no key of a supported mod unmodelled; each mod's calibration subset in band |
| **A4** | ImpAI everywhere: network games, every supported mod | the compatibility suite and a two-player recorded game with ImpAI seats come back clean |

Every landing follows the repository's rules: review at the level the diff calls for, the documentation pass, and replay test groups for any landing that changes simulation code. This plan does not repeat them.
