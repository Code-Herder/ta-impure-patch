# ImpAI and tasim

ImpAI is a planned skirmish AI that plays beside the stock one. tasim is a headless simulation of the whole game, built to test ImpAI. Both run on one simulation core that the in-game wiki, tasim, ImpAI and the game share.

**Status:** planned 2026-11-11 after a design interview with the owner. Nothing is built yet. Engine facts are cited from [the engine map](../engine/orders.html) with how each was established; the rest is design. Like the other plans, this section is folded into the module notes and `roadmap.md` when the work closes, and then deleted.

## The plan in four lines

- **ImpAI** is a computer player you pick with the AI button (ImpAI or OTA AI). It never cheats, and its level follows the skirmish Difficulty. Each level must beat the stock AI on the same Difficulty in every game ([ImpAI for the player](player.html), [the bar](bar.html)).
- **Its brain** is the design of the strongest shipped RTS bots: spatial threat maps, one task per unit, choices driven by scouting, and a fight predictor ([the brain](brain.html)). It enters the game where the stock planners run ([entering the game](engine-seam.html)).
- **One C simulation core** is carved out of the wiki's simulation and shared by the wiki, tasim, ImpAI and the game. `sim.js` is retired ([layers and modules](shared-core.html), [retiring sim.js](sim-js.html)).
- **tasim** simulates the whole game as Impure ships it ([scope](tasim.html)). Its calibration bands come from the engine's own run-to-run spread ([calibration](calibration.html)), and no ImpAI change lands on headless results alone ([testing ladder](testing.html)).

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
| what tasim must simulate, and how coverage is measured | [tasim: scope and coverage](tasim.html) |
| how tasim is judged against the real game | [Calibration bands](calibration.html) |
| the five rungs from unit tests to the owner's playtest | [Testing ladder](testing.html) |

## Open

- The 100/100 bar is to be re-evaluated by the owner once A2's results exist.
- C1 verifies that a scenario can be reloaded into a running game instead of relaunched; the calibration set's cost depends on it.
- Which `SKIRMISH.GUI` gadget a seat click cycles, and through which values, is not documented anywhere (L0).
- That Hard Difficulty applies no income scaling to OTA AI is [INFERRED]: the notes give only 50 % and 70 %.
