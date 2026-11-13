# Retiring sim.js

The in-game wiki's mockup had its own JavaScript simulation, `sim.js`, a twin of the C kept equal tick for tick. While it stood, every change to the shared core was written twice, in C and in JavaScript, so the owner decided to retire it. S2 retired it: the mockup and every node check play the wiki's simulation through the same C the DLL runs.

## The route

The simulation runs as a host process, `wikitest serve` (`tagpu/ddraw/tests/wikihost.c`): one JSON request a line in, one answer line out. `tools/wiki/mockup/simhost.js` is the mockup's `WikiSim` over it, the same calls `sim.js` answered: the page's go through `tools/wiki/serve.py`'s `POST /sim` to one host child, node's through `tools/wiki/hostchan.cjs`'s channel. The units' scripts and sounds are the host's too: it reads them from the gfx packs a sim's context names, so the mockup has no COB client of its own. [The in-game wiki](../ingame-wiki.html), *The simulation's contract*, states the protocol, the builds and what the drop-in keeps between requests.

## What it is now

- **The specifications are split:** the mockup is the specification of the wiki's layout and drawing. The C core (`tagpu_wikisim.c` over `tasim_*.c`, the loader `tagpu_wikidata.c`, the COB host `tagpu_wikicob.c`) and its notes are the specification of the simulation.
- **Parity is one source across three builds:** `simparity.mjs` sends every request to the mockup's `host` build, the `asan` build (AddressSanitizer and UndefinedBehaviorSanitizer) and the DLL's i686 build under Wine at once (`hostchan.cjs` `tee`), and any answer that differs in its text is a difference. It keeps the state hash and the tick-by-tick comparison of view, orders and hash. The replay layer, the frame, the text and the stage's drawing stay JavaScript twins of their C, held by their own checks.
- **The exit test:** every demo, game, replay and case plays through the host, and its per-tick hashes equal the record taken before S2 (`simparity.mjs --against .data/impai/s2-baseline/`).
- **A WebAssembly build** could replace the `serve.py` route, if the mockup ever has to run without the local server. It is not planned.

## No simple mode

With one core there is one movement model and one economy. The wiki inherits each fidelity improvement made for tasim, with no switch keeping the old behaviour. When movement or combat changes, the wiki's demos play differently and are reviewed again through the wiki gate (`wiki_regression.py`). That cost buys demos that behave like the game.
