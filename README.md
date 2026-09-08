# ta-impure-patch

A GPU renderer and toolchain for **Total Annihilation** (Cavedog, 1997), attached to the retail
executable at run time. The 1997 simulation runs untouched; the drawing is ours.

Two DLLs do the work. Our `ddraw.dll`, a fork of [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw),
gives the game DirectDraw, a window and an OpenGL present loop. The companion `tagpu.dll` hooks
the engine at absolute addresses, reads unit state through the community's verified struct map,
suppresses the software rasterisers inside the world viewport, and draws the scene in GL 3.3
core. The engine's own 8-bit frame is composited over ours through a key colour, so anything it
still draws shows through unchanged.

## Where it stands

Drawn natively today: units, wrecks, shadows, cloak and waterline; units under construction
with their scaffold and wireframe; weapon fire, explosions and debris; smoke, fire, wakes and
nanolathe; features (trees, rocks, splats, wreckage); terrain tiles, the fog overlay and fog of
war as drawn; health bars, order markers, group digits, range labels, the build cursor and the
band box. Still the engine's: the shell — menus, side panel, minimap, chat — composited through
as before; its GL twin is in progress.

Two looks, which do not blend. **Classic** reproduces the engine's 8-bit look and is measured
against it pixel for pixel. **Classic++** restores terrain, unit textures and sprites to the true
colour their dithering stood for — a learned restorer running as GLSL in the game's own context —
then lights and shadows them. `tools/tascene` is the browser lab where both rules are developed
against the same scenes before they go into the game.

## What is in the repository

| Path | What |
|---|---|
| `tagpu/ddraw/` | the cnc-ddraw fork that is our `ddraw.dll`; `src/tagpu_*.c` are the native passes, the composite, the zoom and the Classic++ restorer |
| `tagpu/src/` | `tagpu.dll`, the companion that hooks the engine |
| `tools/tacli` | launch and drive isolated game instances from the command line: own game-dir mirror, own Wine prefix, own window, several in parallel, JSON output for scripts |
| `tools/tascene` | the browser render lab: compile a scenario into a scene pack and render it in WebGL under the Classic and Classic++ rules |
| `tools/ta3do`, `tools/ta3domod` | read 3DO models and GAF textures, render standard views, export glTF; edit a 3DO and write it back |
| `tools/hpipack.py` | read and write HAPI archives (`.hpi`, `.ufo`, `.ccx`, `.gp3`) |
| `tools/tacob*` | tacob, a BOS/COB script editor that runs the script on the unit's own model (in progress) |
| `tools/ghidra-scripts/`, `tools/ta_symbols.txt`, `tools/tamem_ghidra.h` | Ghidra headless scripts and the merged symbol and struct corpus for the executable |
| `unditherer/` | the de-dither restorer: classical filters, two trained networks with their provenance, the training pipeline, the corpus credits |
| `scenarios/` | JSON scenarios that put a running game into a known situation for measurement (`tacli scenario load`) |
| `research/notes/` | the wiki source: the engine map, every module's design and measurements, the roadmap; `research/build_wiki.py` renders it |
| `units/` | replacement models we built; a `hires/<name>.glb` in the game dir stands in for a 3DO and is posed by the unit's own COB script |
| `pristine/manifest.md5` | hashes of the retail install everything is measured against; the files themselves are never in the repository |

## Requirements

- A retail copy of Total Annihilation in the v3.1 layout (the Steam build; `TotalA.exe` md5
  `8e74a1dffa1f5988624c52048f5b20cd`). Nothing from the game is in this repository — see below.
- Linux with Wine or Proton, and an X11 session for the windowed instances. This is the reference
  setup; nothing else has been tried.
- `i686-w64-mingw32-gcc` and `windres` for the two DLLs.
- Python 3.12 for the tools. The wiki and the learned restorer use a virtual environment at
  `.venv-undither/` in the checkout root (`markdown`, torch, onnxruntime); `tacli` is stdlib only.

## Build and run

```
make -C tagpu/ddraw                       # ddraw.dll: the fork plus the native passes
make -C tagpu                             # tagpu.dll: the engine hooks
tools/tacli launch t1 --res 1024x768      # an isolated instance, created on first use
tools/tacli scenario load t1 200v200      # menus -> a live game -> 400 units -> camera
tools/tacli glshot t1 -o /tmp/frame.ppm   # what the GL passes drew this frame
tools/tacli stop t1
```

Each instance lives under `tagpu/instances/` with its own game directory (a symlink mirror of
the retail files plus private config and logs) and its own Wine prefix. Runtime features are
switched by trigger files in that directory (`tools/tacli arm t1 owndraw.on`); the levers and
what they do are listed on the wiki's status page.

## The wiki

`research/notes/` is the source of truth. It holds the engine map — every address the work
touched or merely read, how each fact was established, negative results included — each
module's design with its measurements, and the roadmap with its gates. Claims are tagged
`[VERIFIED]`, `[MEASURED]` or `[INFERRED]`, never left implied. Render it with:

```
.venv-undither/bin/python research/build_wiki.py    # -> research/site/
```

## Nothing from the game is in here

This repository ships no part of Total Annihilation; bring your own retail copy. `.gitignore`
bans the game's file formats and its archives. A pre-commit hook refuses any file byte-identical
to a file of the retail install, including everything inside its archives, and any binary outside
the classes listed in `.publish-allow`; the publish scan checks every revision the same way.
Derived work — screenshots of our renderer, figures, models we trained or built — is allowed by
class. The rules are in `CLAUDE.md`, *Publishing*.

## Lineage and credits

Everything modern in TA engine modding stands on [TADR](https://github.com/tanvanman/TADR) (MIT):
its address corpus and struct map are what `tagpu.dll` reads through. Our `ddraw.dll` is a fork of
[cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) (MIT), which carries Microsoft Detours (MIT).
The restorer's ONNX spike used onnxruntime (MIT); its networks were trained on a CC0 texture
corpus credited in `unditherer/`. The full lineage — Patch Loader, petool, totala-re, the mods that
consume the patch line — is on the wiki's project-map page.

## Working in this repository

`CLAUDE.md` holds the conventions: commit freely on a worktree branch, land one finished unit at a
time through the build and documentation gates, review engine changes before they land, and
publish only through the scanned `/git_publish` flow.

## Licence

MIT, for the code that is ours (`LICENSE`). Vendored components keep their own: cnc-ddraw and
Detours under MIT (`tagpu/ddraw/LICENSE`, `tagpu/ddraw/src/detours/LICENSE.md`), onnxruntime under
MIT (`tagpu/ddraw/inc/onnxruntime_LICENSE.txt`), the training corpus under CC0-1.0
(`unditherer/LICENSES/`). Total Annihilation itself is not part of this repository and is not
covered by any of these.
