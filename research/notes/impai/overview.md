# ImpAI and tasim

ImpAI is a skirmish AI that plays beside the stock one. It is built, verified and tuned in the engine itself, through the calibration harness. tasim, the simulation core the in-game wiki plays, keeps the engine's definitions and map rules but is no longer being extended into an exact copy of the whole game.

**Status (2026-11-14):** L0, S1 to S3, L1, C1, T1a, A1a and A2a are built on the branch `worktree-Bar_game_AI`, not on `main`; A1b, A2b, the levels (P1) and A4 are built and being verified, A2b and the levels against [the bar](bar.html). On 2026-11-12 the owner stopped the exact port of the engine that was to follow T1a (T1b to T4): ImpAI is tuned in the engine ([decisions](decisions.html), [where it stands](landings.html#where-it-stands)). Engine facts are cited from [the engine map](../engine/orders.html) with how each was established; the rest is design. Like the other plans, this section is folded into the module notes and `roadmap.md` when the work closes, and then deleted.

## The plan in four lines

- **ImpAI** is a computer player you pick with the AI button (ImpAI or OTA AI). It never cheats, and its level follows the skirmish Difficulty. Each level must beat the stock AI on the same Difficulty in every game ([ImpAI for the player](player.html), [the bar](bar.html)).
- **Its brain** is the design of the strongest shipped RTS bots: spatial threat maps, one task per unit, choices driven by scouting, and a fight predictor ([the brain](brain.html)). It enters the game where the stock planners run ([entering the game](engine-seam.html)).
- **One C simulation core** is carved out of the wiki's simulation and shared by the wiki, tasim, ImpAI and the game. `sim.js` is retired ([layers and modules](shared-core.html), [retiring sim.js](sim-js.html)).
- **The engine is the test bench.** ImpAI is verified and tuned in real games: scenarios reloaded into a live game, perturbed runs, references and a permutation test against the engine's own run-to-run spread ([calibration](calibration.html)), many games at once under the weighted run lock. tasim keeps the engine's definitions and map rules for the wiki ([scope](tasim.html)); a targeted, approximate combat model calibrated against the engine is the fallback if the tournament needs more games than the engine can play ([testing ladder](testing.html)).

## Where to read next

| If you want… | Read |
|---|---|
| what the owner decided, and what each decision was chosen over | [Decisions](decisions.html) |
| the order of work and each landing's exit test | [Landings](landings.html) |
| how ImpAI looks to a player: the button, levels, recordings, saves | [ImpAI for the player](player.html) |
| what ImpAI must win to become the default | [The bar](bar.html) |
| how ImpAI thinks | [The brain](brain.html) |
| the engine sites it plugs into, and what is still undocumented | [Entering the game](engine-seam.html) |
| which code is shared, by whom, and where it comes from | [Layers and modules](shared-core.html) |
| why the mockup's JavaScript simulation goes | [Retiring sim.js](sim-js.html) |
| what tasim keeps of the engine, and what it no longer simulates | [tasim: scope and coverage](tasim.html) |
| how ImpAI and tasim are judged against the real game | [Calibration bands](calibration.html) |
| the five rungs from unit tests to the owner's playtest | [Testing ladder](testing.html) |

## Open

- The 100/100 bar is to be re-evaluated by the owner once A2's results exist.
- A few engine questions of the network game (A4) are open ([entering the game](engine-seam.html), *Not documented yet*).
