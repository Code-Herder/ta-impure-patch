# tasim: scope and coverage

tasim is the shared core run headless by a game driver. It simulates the whole game as Impure ships it, so that ImpAI can play thousands of games without the engine. The engine's defect patches, raised limits and extra-weapons module are part of what it reproduces, because they are what ImpAI plays against.

## What it simulates

| Area | What tasim does |
|---|---|
| **Data** | every unit, weapon, feature and move class of the mod, loaded from the install at run time (FBI, weapon TDFs, `moveinfo.tdf`, features, COB scripts); the map's height, metal and features. Nothing from the game enters the repository |
| **Economy** | metal and energy production and use, storage, extractors on the metal map, solar, wind, tidal, geothermal, metal makers, build power, assist, sharing between allies |
| **Work** | building, nanoframes, repair, reclaim of units and features, resurrect, capture, self-destruct, factories and their queues |
| **Movement** | ground, hover, amphibious, ships, submarines; slope and water limits; acceleration, turn rate, collisions and pushing; the engine's own pathing |
| **Air** | the flight model (cruise height, turning, banking), take-off and landing, patrol and guard, bombing runs, transports loading and unloading |
| **Weapons** | every weapon kind: ballistic, line, dropped, starburst, guided, beam and lightning, the D-gun, flamethrowers, torpedoes and depth charges; accuracy and spray, bursts, stockpiles, nukes and interceptors, kamikaze; line of fire against terrain, area of effect and edge effect, friendly fire, damage per target category |
| **Damage and death** | health, wrecks and heaps, death explosions, the commander's explosion, burning trees and spreading fire, experience, the game's end conditions |
| **Sensors** | line of sight, radar, sonar, jammers, cloak and its energy drain, stealth |
| **Impure's engine** | the extra-weapons module (1..N weapons), the raised unit and weapon limits, the engine defect patches, the TADR sim fixes |

## Coverage is measured, not claimed

The engine map's [key inventory](../engine/keys.html) lists every FBI, weapon-TDF, `moveinfo`, feature and map key the engine's parsers read, taken from the disassembly; which of those fields the simulation reads back is not mapped yet. `tasim_defs` loads against that inventory, and names every unit and key it does not model. "The whole game" therefore means every key in the inventory is modelled. A mod with keys nobody listed shows up on its first run.

## Speed

On the same scenario, tasim on one core runs at least **20 × the game's turbo**. Battles run 64–230 ticks a second at k = 1 (MEASURED, [Sim step and clock](../engine/sim.html), *The scheduler, the clock and the test pacing*). This is checked at every T step, and a shortfall becomes work in that step. tasim runs take no run lock, so they can use every core of the reference setup at once.

## The harness

- **`tasim_run`**, in C: one game per process (players, start positions, win and loss) with the results printed as JSON. It is also built for i686, so whole games get the same Wine parity check as the wiki's demos. Today it has one subcommand, `roles`, which classifies every unit of an install with the shared core's `tasim_roles` (`tools/tasim/tasim_run.c`, built natively by `tools/tasim/native.py`); `play`, which the calibration check runs per run, lands with T1.
- **`tools/tasim/`**, in Python, in the shared venv: tournaments, Elo, tuning sweeps and the [calibration runner](../modules/tasim-calibration.html), which is built: the engine phase, the references, the check and its scoreboard, and the duel generator.

## Watching a game

The wiki's 3D stage draws a tasim game, as it already draws the core's state for a wiki demo. That is how ImpAI is debugged; logs and the scoreboard add the numbers.

## The order

T1 is the ground game, T2 combat, T3 air and specials, T4 Impure's engine and the mods. Each T step is followed by the ImpAI step that needs it ([landings](landings.html)).
