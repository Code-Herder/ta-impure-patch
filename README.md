# Total Annihilation — Impure Patch

The aim of the Total Annihilation Impure patch is to fully modernize Total Annihilation from a 1997 game into a modern game engine through patching alone. It's been called impure because among other features, it adds a strategic zoom. I have many more planned features but those are the main ones being worked on right now.

## Core features

- **Game/UI render replacement.** Full port of the software renderer to OpenGL.
  - Zoom in and out of the map.
  - UI resizes proportionally to the screen resolution *(work in progress)*. A prototype is partially integrated, not a full port; some UI elements are still blitted to a texture.
- **COB extensions**
  - 0 to N valid weapons per unit. You could build a super unit with 40 weapons if you wanted to.
- **Agentic tooling**
  - **[ta-cli](docs/tacli.md)** — a CLI that drives Total Annihilation. It navigates menus, selects units, starts maps with custom testing setups, drives units, and can even run multiplayer games locally for testing. It is meant to be used with an agent skill that develops features automatically, or as the runner for the unit tests you have written and want to run again.
  - **Scenarios** — a JSON file that describes a game scenario: the map, the sides, the units and where they stand, the camera. One command takes a fresh game from the menus into that live scenario, so a test or a measurement starts from the same state every time instead of from a sequence of clicks. The options are pretty comprehensive: wrecks, orientation of unit placement, naming specific units for tracking, etc.
  - **Importers and exporters for every format** (3do, cob, tnt, gaf, hpi, …), for agents to drive or to build on.
  - **COB editor** *(work in progress)*
    - An editor supporting the new COB features.
    - A full COB virtual machine: run the COB scripts in the editor and test them without loading TA.
    - A viewer that shows the model running the script and its behaviours — aim, attack and so on. Supports planes, boats, etc.
  - **Lab viewer** (`tascene`) — previews the game's rendering 1:1 in the browser and lets an agent tweak it, so you iterate on the visual look without loading Total Annihilation each time.
- **TADR integration** — investigation stage. Working out how it can live alongside all the changes made over the years.

## Builds

Every push to `main` builds `ddraw.dll` on GitHub Actions. The run's artifact is the release folder: the DLL, its `ddraw.ini`, the Classic++ restorer weights and a README. A `v*` tag publishes the same zip under Releases. Until the game has an options menu, everything is on by default; a `tagpu_<pass>.off` file next to `TotalA.exe` turns one pass off, `tagpu_defaults.off` all of them.

## The wiki

This repo also comes with a wiki that is the master reference for all the work in progress. It has accumulated a lot of TA technical knowledge from various sources, and publishes a much expanded map of the engine mappings and of how the various features work. The source is `research/notes/`.

## Licence

MIT — see `LICENSE`. It covers the original work in this repository only and grants no rights over Total Annihilation, which remains the work of Cavedog Entertainment. This repository ships no part of the game.
