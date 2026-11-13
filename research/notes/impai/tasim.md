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

The engine map's [key inventory](../engine/keys.html) lists every FBI, weapon-TDF, `moveinfo`, feature and map key the engine's parsers read, taken from the disassembly; which of those fields the simulation reads back is not mapped yet. The core's key table (`tasim_keys.c`, [the shared core](shared-core.html)) has a row for each, with the landing whose rules read the field, and the install loader (`tools/tasim/tasim_load.c`) reads an install through it and makes the named-not-modelled report, which `tasim_run defs` prints: the keys a later landing models, or nothing in the simulation reads, each with that landing; the keys in a file that no parser reads, which the engine ignores too (the retail install's 36 include every FBI's `TEDClass`, `UnitNumber` and the other languages' names); the values the engine would fault or hang on, which the core refuses (none in the retail install); and, with `--features`, the engine behaviours outside T1 from `tools/tasim/engine-features.tsv`. "The whole game" therefore means every key in the inventory is modelled. A mod with keys nobody listed shows up on its first run. `tools/tasim/defcheck.py` holds the loaded definitions to the engine's own, byte for byte, from one running game. `tools/tasim/classcheck.py` holds a map's grid and its 32 class maps to the engine's after the level loads: it reads the map with the repository's readers, runs the core's LoadMap and fill on the values (`tools/tasim/classmap.c`), loads the map in one launch with every unit cleared and the AI off, and compares every class map entry and every grid field the load defines, `tacli peek` reading the engine's side; `tools/tasim/tests/test_classcheck.py` drives it whole against a fake engine memory. The pathfinder (`tasim_path.c`) is held offline, not yet against a game: `tagpu/ddraw/tests/tasim_pf_test.py` checks its rules on hand-made 12 × 12 maps and its skip-ahead against the engine's own loop, one unit slot at a time, on 10⁴ random worlds. Nothing calls it until the movement's port (the T1 plan's b3), whose track scenarios compare the paths with the engine's. It differs from the engine only where the engine would hang: a bug walk whose two heads never meet ends after 8·W·H pairs of steps.

## Speed

On the same scenario, tasim on one core runs at least **20 × the game's turbo**. Battles run 64–230 ticks a second at k = 1 (MEASURED, [Sim step and clock](../engine/sim.html), *The scheduler, the clock and the test pacing*). This is checked at every T step, and a shortfall becomes work in that step. tasim runs take no run lock, so they can use every core of the reference setup at once.

## The harness

- **`tasim_run`**, in C: one game per process (players, start positions, win and loss) with the results printed as JSON. It is also built for i686, so whole games get the same Wine parity check as the wiki's demos. Today it has three subcommands (`tools/tasim/tasim_run.c`, built natively by `tools/tasim/native.py`), each reading the install through the one install loader, `tools/tasim/tasim_load.c` ([the shared core](shared-core.html), *What stays outside*): `roles`, which classifies every unit of an install with the shared core's `tasim_roles` over a copy of the loaded definitions; `defs`, every unit, weapon, feature and move class as loaded, with each one's engine image and the named-not-modelled report; and `values`, every value of the install's definition files, the sample the key table's test runs the number getters on. A mod whose loader renames the first mount pattern or folders in memory is read as the game reads it: `native.py roles` and `native.py defs` take `--archive` and `--folders`, whose values `tools/wiki/sidepacks.py`'s `RECORDS` hold for each mod of the compatibility suite (ProTA 4.8: `--archive ProTA.gp3 --folders gui=guiP,gamedata=gamedatP,download=downloadP`, 317 units, 21 move classes). `tools/tasim/tests/test_load.py` holds the loader to its rules on a synthetic install. `play`, which the calibration check runs per run, lands with T1.
- **`tools/tasim/`**, in Python, in the shared venv: tournaments, Elo, tuning sweeps and the [calibration runner](../modules/tasim-calibration.html), which is built: the engine phase, the references, the check and its scoreboard, and the duel generator.

## Watching a game

The wiki's 3D stage draws a tasim game, as it already draws the core's state for a wiki demo. That is how ImpAI is debugged; logs and the scoreboard add the numbers.

## The order

T1 is the ground game, T2 combat, T3 air and specials, T4 Impure's engine and the mods. Each T step is followed by the ImpAI step that needs it ([landings](landings.html)).
