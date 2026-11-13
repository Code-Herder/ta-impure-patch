# Landings

The order of work and each landing's exit test. Research step L0 runs beside S1; S2, S3, L1, C1 and T1a follow. ImpAI is then built, verified and tuned in the engine itself, through C1's harness (scenario reload in a live game, perturbed runs, references, the permutation test) under the weighted run lock. The exact out-of-game port that was to follow T1a -- T1b, T1c, T1d, T2, T3, T4 -- is stopped (decided 2026-11-12, `decisions.md`); its rows stay below, struck, for the record, and A1c to A4 no longer wait on them.

| Id | What | Done when |
|---|---|---|
| **L0** | Research: the AI seat's lifecycle; the order roots for move, attack, build and repair; the skirmish screen's AI button and where a seat's choice lives; what the stock planners do; the inventory of every FBI, weapon-TDF and `moveinfo` key the engine reads; the handicap's sites | the engine map holds each address with its evidence, the key inventory is a page of it, the seam has its safety argument |
| **S1** | Carve `tagpu_wikisim.c` into the core and the wiki front end, behaviour unchanged | `simparity.mjs` equal natively and under Wine; the wiki gate passes |
| **S2** | The mockup calls the C core through `serve.py`; `sim.js` deleted ([retiring sim.js](sim-js.html)) | every demo plays through the host, the hashes match the pre-S2 ones, no JavaScript simulation left |
| **S3** | The game's rules move above `WORLDQ`; the engine adapter is its first engine implementation ([layers](shared-core.html)) | `place_regression.py`, `resource_regression.py`, `sweep_regression.py` and the wiki gate pass |
| **L1** | ImpAI's world view and spatial maps, read-only, with the overlay lever; the stock AI still plays | the overlay matches a recorded game; the replay gates pass with ImpAI observing |
| **C1** | The calibration runner: scenario reload in a live game, perturbed runs, references, the offline check and its scoreboard ([calibration](calibration.html)) | one scenario of each kind produces a reference and a report; the cost per scenario is measured |
| **T1a** | tasim's definitions and world: every key loaded or named, the core on the engine's numbers (16.16 positions, float32 economy), one caller-owned state | the wiki plays as before; the definitions and the move-class grids equal the engine's after load |
| ~~**T1b**~~ | *stopped:*  movement and pathing, terrain and water, ground and sea | the movement scenarios match the engine tick by tick; the courses in band; the wiki's demos reviewed on the new movement; the speed gate |
| ~~**T1c**~~ | *stopped:*  the whole economy and every work order | C1's exact families and the work families pass; the economy opening in band |
| ~~**T1d**~~ | *stopped:*  the sensors and whole headless games | sight and radar exact; a 10 000-game soak clean; at least 20 × turbo |
| **A1** | ImpAI builds: economy, builders, factories; the ImpAI / OTA AI switch with OTA AI the default; the handicap exemption | ImpAI's income at 10 minutes beats OTA AI's on three maps, in the engine |
| ~~**T2**~~ | *stopped:*  tasim, combat: every ground and sea weapon kind, damage, wrecks, fire, experience, self-destruct, the commander's explosion, the game's end | the duel, brawl, naval and base-assault scenarios are in band |
| **A2** | ImpAI fights: the fight predictor, the military manager, the three levels; the default flips to ImpAI | the predictor's 90 % check against engine duels; [the bar](bar.html) at every level |
| ~~**T3**~~ | *stopped:*  tasim, air and specials: flight, bombers, transports, cloak, stealth, stockpiles, nukes and interceptors, kamikaze | those scenarios in band; no unmodelled key left in the base game |
| **A3** | ImpAI adapts: role composition, response tables, the tournament harness that tunes them, air play | a tuned table beats the hand-written one in the engine tournament, and the win holds at rung 2 |
| ~~**T4**~~ | *stopped:*  tasim, Impure's engine and the mods: extra weapons, raised limits, the defect patches, the TADR sim fixes; the compatibility-suite mods | no key of a supported mod unmodelled; each mod's calibration subset in band |
| **A4** | ImpAI everywhere: network games, every supported mod | the compatibility suite and a two-player recorded game with ImpAI seats come back clean |

Every landing follows the repository's rules: review at the level the diff calls for, the documentation pass, and replay test groups for any landing that changes simulation code. This plan does not repeat them.
