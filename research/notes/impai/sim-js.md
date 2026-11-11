# Retiring sim.js

The in-game wiki's rule has been that the browser mockup is the specification, with `sim.js` as its simulation. While `sim.js` stays, every change to the shared core would be written twice, in C and in JavaScript. The owner decided to retire it.

## The route already exists

The mockup has no JavaScript COB VM. `cob.js` sends each call to the C COB host (`tagpu_wikicob.c`), which `tools/wiki/serve.py` runs, through one synchronous request; under node, `tools/wiki/cobhost.mjs` runs the same host. S2 does the same for the whole simulation: the mockup calls the C core through `serve.py`, and `sim.js` is deleted.

## What changes

- **The specifications split:** the mockup stays the specification of the wiki's layout and drawing. The C core and its notes are the specification of the simulation.
- **Parity becomes one source across three builds:** `simparity.mjs` compares the native build, the i686 build under Wine, and the mockup's host, instead of two languages. It keeps the state hash and the tick-by-tick comparison.
- **S2's exit test:** every demo plays through the host, and the state hashes match the recordings made before S2.
- **A WebAssembly build** could replace the `serve.py` route later, if the mockup ever has to run without the local server. It is not planned.

## No simple mode

With one core there is one movement model and one economy. The wiki inherits each fidelity improvement made for tasim, with no switch keeping the old behaviour. When movement or combat changes, the wiki's demos play differently and are reviewed again through the wiki gate (`wiki_regression.py`). That cost buys demos that behave like the game.
