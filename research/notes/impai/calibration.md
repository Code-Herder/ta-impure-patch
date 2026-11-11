# Calibration bands

How tasim is judged against the real game. tasim cannot be closer to the engine than the engine is to itself, so each band is the engine's own run-to-run spread, measured. The owner decides the rules once; the numbers come from the engine.

## Why the engine has a spread

The same scenario does not come out the same twice. Each run is perturbed (start positions jittered, first orders staggered, sides swapped), weapon spread draws the simulation's random stream (`0x4B6C30`, called at `0x49D727` and `0x49D733`, DISASSEMBLED), whose state carries on from run to run, and where the step's batches fall changes what units see (*The batch is part of the result*, [Sim step and clock](../engine/sim.html)). The engine's spread is the spread of a metric over those perturbed runs.

## Three kinds of metric

| Kind | Examples | Rule |
|---|---|---|
| **Exact** | build time at a given build power; cost drawn per tick; an extractor's income on a given metal; weapon range, reload, damage per hit; sight radius; storage | match exactly, one run each, compared tick by tick against the recorder's dump. No chance is involved, so any difference is a tasim bug |
| **Win rate** | which side wins a duel or a brawl | tasim's and the engine's win rates differ by no more than two standard errors of the difference (about ±0.20 at 50 runs a side on an even fight, ±0.14 at 100) |
| **Distribution** | time to kill; the winner's remaining value; travel time A→B with collisions; the income curve of an opening | tasim's median inside the engine's 10th–90th percentile, and tasim's spread (P90 − P10) between half and double the engine's |

**ImpAI's fight predictor** is held to its decisions: on decisive fights (the engine's win rate above 80 % or below 20 %), its engage-or-retreat call agrees with the engine's majority at least 90 % of the time. Close fights are left out, because no predictor can call a coin flip.

A band is loosened only per metric, with a written reason. Loosening is the owner's decision.

## The procedure

1. **Scenarios** in the existing `scenarios/` format:
   - **generated** duels and movement courses: for each role pair and each tech level, three unit pairs (the cheapest, the median and the most expensive unit of each role); for each move class, a course over terrain;
   - **hand-written** scenarios for what a generator cannot set up: the economy opening, a base assault, naval fights, air raids, the ten-seat stress game.
2. **Perturbed runs:** each scenario runs **50 times** in the engine at turbo. Each run shifts start positions within one footprint cell and staggers the first orders by 0–15 ticks. tasim runs the same perturbations, drawn from a seed.
3. **The engine reference**, recorded once per scenario: every run's metrics and the DLL build that produced them. It is measured again only when a landing changes Impure's simulation, which `tareplay.py landing` already detects. Engine runs hold the run lock.
4. **The offline check:** every tasim change replays the whole set against the references, with no game running and no lock.
5. **A scoreboard:** for each scenario and metric, in or out of band, and by how far. It shows which area tasim gets most wrong, so fidelity work goes where the error is.

## Cost

This is an estimate, not a measurement: about 40 scenarios × 50 runs is 2,000 engine runs. A duel is one to two game minutes, a few seconds at turbo, so the set takes a few hours of serialized engine time, once, and again only after simulation landings. Loading the next scenario into a running game instead of relaunching is what keeps it that short. C1 verifies that this is possible.
