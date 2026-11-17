# The tech tree entry in the game: the plan

The specification is the mockup ([the in-game wiki](ingame-wiki.html), *The tech tree entry*):
`tools/wiki/mockup/techtree.js` (the model, the layout, the drawing, the hit test),
`unitview.js` (the unit's model in 3D) and `wiki.js` (the stage's input and the unit's panel).
The game reads the tree from the running mod, draws it on the theatre's stage and opens a picked
unit's panel (T1-T3), and the panel shows the unit in 3D, posed by its own script under an orbit
camera (T4). This note is how the DLL gets to the mockup, in landings, each one finished, verified in the game and reviewed
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

*Built (`7c1a2393`).* The panel is the page's (`tagpu_wikilay.c` `unit_panel`), laid out from the
render thread's own tree, in About's place and the top layer; its pictures are `HOP_PIC` ops of
the tree's sheet on the page's list. What the landing found: the HUD faces have neither `…` nor
`›`, so a cut text ends in three periods -- the panel's chips and, since this landing, the cards'
clips, whose ellipsis the game drew as a missing glyph -- and the walk is joined by `»`; the
mockup cuts its chips itself (`chipCut`), since the browser's `text-overflow` can only draw `…`,
and cuts them again once the faces and the text styles have loaded (`document.fonts.ready` can
resolve before any text asks for a face). `wikilaycheck`'s six panel cases agree box for box,
line for line and picture for picture. In the game: `wiki_regression.py techtree` opens the panel
on a pick (the canvas narrows 1404 -> 866 pixels), follows a chip at 1:1 and closes it; the mods
run's (t3) opens a unit of every kind each setup's side holds -- in Escalation all ten kinds,
ARMASP_UPGRADE among them, 36 panels; ProTA nine, 21 panels.

**T4: the unit in 3D.** A perspective view of one unit in the panel's view rect on the render
thread: the model posed by its own COB script through the wiki's COB host (`tagpu_wikicob.c`),
back faces culled and textured quads mapped per pixel as the unit pass draws them, a free orbit
camera (drag, wheel, Turn), and the actions (Walk, Aim, Fire, Build, On / off, Take off, Reset)
as the engine's own calls. An upgrade shows the builder it is built onto. *Exit*: in the game,
the view's picture against the mockup's unit view at the same camera, per setup; the cull and
the quad mapping checked on ARMCV and ARMSOLAR.

*T4's four landings.* What the game has to start from (read 2026-11-17): no perspective camera
anywhere in the DLL; the unit shader (`tagpu_posedraw.c` VS, `tagpu_native.c` FS) bakes the
engine's oblique projection, its depth key, the clockwise front face, the slant shadow, the line
grid and fog into its stages, so a matrix is not a uniform away; the wiki's 3D load resolves only
the stages' unit types, into an arena published once (`tagpu_wiki3d_load`), so most of a tree's
units have no model or script in the game; the wiki's render thread has its own unit atlas, which
paints a texture frame the first time a bake asks for it, keyed by the frame's address
(`tagpu_scene.h`'s wiki instance of `render3do`); and the mockup's view lights by face with its
own rule, not the engine's shade rows. So:

- **T4a: the models.** After the tree, the loader resolves every unit of it as the 3D load
  resolves a stage's types -- FBI, model, script, hides, posed order, textures from the bank --
  into an arena of its own, published once and never freed (`tagpu_wikitech3d_load`,
  `tagpu_wikitech3d_get`). *Exit*: `assetcheck.py --side` pairs it with the pack (a model for
  every unit the pack has a mesh of, a script for every COB), and the mods run's load line.
  *Built.* Measured: stock 266 units, 6.2 MB, 152 ms; Escalation 532 of 536, 26.7 MB, 542 ms --
  the largest, under the arena's 64 MB -- its four refused by the bake's 512 faces a piece.
- **T4b: the unit's script.** The panel's unit runs its own COB script on the game thread
  (`tagpu_wikicob.c`, one host for the view), Create, then a slice a tick at 30 ticks a second
  while the panel is open; the actions are the engine's calls as `unitview.js` makes them; its
  pieces' poses travel in `TAGPU_PK_WIKI`, bounded on the render thread. An upgrade's view is the
  builder it is built onto. *Built*, with the orbit camera's input (T4d's first half): (tt8) runs
  the commander's script (15 pieces), Walk moves them, Reset makes a new host with Turn kept, a
  drag of 80 x 20 pixels turns the camera 0.8 and 0.2 radians and stops Turn, a notch scales the
  distance by e^-0.1. The packet's head grew to 5 488 bytes with the pose.
- **T4c: the picture.** A pipeline of its own for the view: the pieces' rest vertices from
  `tagpu_posebake_unit` (the wiki's instance, the stage's atlas), posed by the packet's pieces,
  under a perspective camera (`unitview.js` `mat`: 0.9 radians tall, near and far from the
  distance), back faces culled, a textured quad mapped per pixel through `taQuadST`, lit by face as
  the mockup lights it; drawn into a target of the view's size and composited into the panel as a
  picture of the page's top layer. *Built* (`tagpu_wikiview.c` lays the posed body out on the
  render thread; `tagpu_vk_wikiview.c` draws it into a target twice the view's size each way, with
  its own copy of the wiki's unit atlas, since the stage's unit pass uploads only while a stage is
  drawn; the page's top layer is drawn in two around the picture so the note lies over it). (tt8)
  checks the commander's picture is drawn -- 555 vertices, 15 pieces -- and grabs it. The
  mockup's rest pose now stands in the same held axes as its posed one, as the DLL's does.
- **T4d: the camera and the checks.** Drag turns, the wheel moves in, Turn spins (a drag stops
  it), Reset; the picture against the mockup's at the same camera, per setup; the cull and the
  quad mapping on ARMCV and ARMSOLAR. *Built*: (tt9) grabs ARMCV's and ARMSOLAR's views with Turn
  off and writes their cameras; `tools/wiki/viewcheck.py` draws the mockup's view of each at the
  same camera headless (`unitview.js` `ucam`) and compares the silhouettes and the colour --
  measured stock at 1920 x 1080: ARMCV 0.961 of the union and 5.9 apart, ARMSOLAR 0.991 and 10.3
  (bounds 0.90 and 20). It found the fit taken over the faces the bake collapses (ARMSOLAR drawn
  at a third of its size, 0.248). *Per setup is not run*: every setup's picture is the same code
  over models the loader resolves as `assetcheck.py --side` pairs them with each setup's pack, so
  a setup could differ only by a model the pairing already covers; the stock pictures stand for
  the pass.

## Open questions

- *(answered for T4)* The unit pipeline cannot take a perspective matrix without a new vertex
  stage, a new depth rule and a new front face, and its fragment stage carries the engine's line
  grid, fog and shadows: T4c is a pipeline of its own over the same baked vertices.
- The tree's pictures *(answered in T2)*: a sheet of their own, 2048 × 2048 RGBA8, holding the
  shown side's pictures; Escalation's 274 use 916 rows (its pictures are not all 64 × 64: 19 are
  128 × 192 and 7 128 × 128). It is larger than a frame slot's staging, so it is sent in bands.
