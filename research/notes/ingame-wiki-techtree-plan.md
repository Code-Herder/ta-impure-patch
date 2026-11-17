# The tech tree entry in the game: the plan

The specification is the mockup ([the in-game wiki](ingame-wiki.html), *The tech tree entry*):
`tools/wiki/mockup/techtree.js` (the model, the layout, the drawing, the hit test),
`unitview.js` (the unit's model in 3D) and `wiki.js` (the stage's input and the unit's panel).
The game today reads `"frame": "techtree"` and lays the entry out as a text entry. This note is
how the DLL gets to the mockup, in landings, each one finished, verified in the game and reviewed
before the next starts (`CLAUDE.md`, *Commit freely, land locally, merge to main once*).

## What the DLL already has

The wiki's sidebar loader (`tagpu_wikiasset.c`, the `SIDE` half, on the wiki's asset thread)
already reads, by the engine's rules and from the running process's folder names:
`sidedata.tdf`'s sides (`sides_read`), every FBI of the units folder (`units_index`, `SUNIT`),
a builder's build menu exactly as the sidebar reads it (`menu_of`, `type_pages`,
`fill_downloads`), the cast rule's walk from a commander (`side_casting`), and a build button's
picture by the sidebar's rule (`gadget_picture`: the page's bank or commongui, the download
picture, the `<unitpics>` PCX) into the side's frame sheet. Its models (`WIKI3DUNIT`: the 3DO
built as the engine holds it, the COB script, the posed order) are read for the units the
entries cast. So the tree's data is the same reads taken over the whole walk, not a new reader.

The stage's pattern is the one to follow: a **pure** C module that names no engine address and
reads only its arguments, drawing HUD ops (`tagpu_wikistage.c`), paired call for op against the
mockup's JavaScript by a node check that compiles it natively (`stagecheck.mjs`).

## The landings

**T1: the data.** The loader extends its walk to every side's whole tree: the walk order and each
builder's menu (`side_walk` over `menu_of`, `gfx.py` `tech_pack`'s twin), each unit's figures
(`SUNIT` gains health, speed, sight, radar, description, category, the weapons' names, ranges and
damage, cloak, stealth, hidedamage), and each unit's picture by the same rule as the mockup's
(`gadget_picture` over the first menu of the walk that holds it, the PCX for a unit on no
button) into an atlas of its own. Published immutable with the rest of the arena, bounded on
every count. *Exit*: `tools/wiki/techdata.py` (new) compares the loader's tree, dumped by a
native build over each compatibility setup's files, with `tech_pack`'s `tree.json`: every side's
walk, every menu and every figure equal, every picture byte-identical.

*Built (`f1b3c3b9`).* The exit is `assetcheck.py --side` rather than a `techdata.py`: the
asset check already loads each setup's files natively beside `gfx.py`'s pack, so the tree's
comparison went into it (`wikiassettest tech`).

**T2: the tree on the stage.** `tagpu_wikitech.c`, pure: `techtree.js`'s `model`, `layout`,
`fit`, `hit` and `draw` in C, the draw as HUD ops (rectangles, polylines, images, clipped
one-line text); the entry takes the theatre with its stage given to the tree; pan by drag, zoom
by wheel, Fit, the zoom buttons, All links, a click that selects. *Exit*: `techcheck.mjs` (new)
pairs `techtree.js` with the C call for op over every setup's tree at three sizes and a set of
cameras and selections; `wikilaycheck` covers the entry's frame; in the game, the tree drawn
in all six setups and a selection made.

*Built (`d50f7330`, then this landing).* The model and layout came first (`d50f7330`, paired by
`assetcheck.py --side` through `wikiassettest techlay`), then the drawing, split into
`tagpu_wikitechdraw.c` so the asset test compiles the model without the stage, and the window.
What the landing found: the measuring dry run of `techcheck.mjs` has to repeat until it asks for
nothing new, since a clip asks for its shortened strings only once it has the full string's
width; the tree's text needed plain styles of its own (`tech_title`, `tech_body`, `tech_label`)
for the mockup and the game to set the same widths; and the regression's tree nodes, which named
entries by their position, shifted when the Tech tree went first, so they are looked up by entry
and title now. In the game (`wiki_regression.py techtree`, 1920 × 1080, stock): the tree opens
fitted at z 1.3556 in a 1404 × 842 canvas, a click on the second card picks ARMLAB, a 120 × 40
pixel drag moves the camera 88.52 × 29.51 tree units, a notch zooms 1.3556 → 1.5750 about the
pointer, and About narrows the fitted canvas to 866 pixels.

**T3: the unit panel.** The panel over the bezel's right (`tagpu_wikilay.c`): picture, title,
description, flags, the figures and weapons, the walk to it, who builds it, its upgrades, what it
builds by kind, each a chip that selects; About and the panel never show together. *Exit*:
`wikilaycheck` pairs it with `wiki.js`'s panel; in the game, a unit of each kind opened in two
setups, Escalation's upgrades among them.

**T4: the unit in 3D.** A perspective view of one unit in the panel's view rect on the render
thread: the model posed by its own COB script through the wiki's COB host (`tagpu_wikicob.c`),
back faces culled and textured quads mapped per pixel as the unit pass draws them, a free orbit
camera (drag, wheel, Turn), and the actions (Walk, Aim, Fire, Build, On / off, Take off, Reset)
as the engine's own calls. An upgrade shows the builder it is built onto. *Exit*: in the game,
the view's picture against the mockup's unit view at the same camera, per setup; the cull and
the quad mapping checked on ARMCV and ARMSOLAR.

## Open questions

- The game's unit pass draws in the engine's fixed projection; T4 needs a perspective camera of
  its own. Whether it is a new pipeline over `tagpu_posebake`'s vertices or the unit pipeline
  with its matrix replaced is T4's first question.
- The tree's pictures *(answered in T2)*: a sheet of their own, 2048 × 2048 RGBA8, holding the
  shown side's pictures; Escalation's 274 use 916 rows (its pictures are not all 64 × 64: 19 are
  128 × 192 and 7 128 × 128). It is larger than a frame slot's staging, so it is sent in bands.
