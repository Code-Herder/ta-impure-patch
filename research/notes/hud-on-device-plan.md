# The HUD pass's shapes and instances on the device: the plan

The HUD pass (`tagpu_vk_hud.c`) draws the replay HUD and the in-game wiki from op lists that
`tagpu_huddraw.c` turns into quads, one 96-byte instance each, written every frame into one
persistently mapped buffer of `HUD_MAXINST` (32 768) instances per frame slot: 9 MB at the
three slots the swapchain gives, mapped as one block in a 32-bit process whose address space a
heavy mod already takes past 2 GB. A refused mapping takes the pass down for the session.

Two things make that buffer large. The tech tree draws shapes the op list has no word for -- a
rounded card is three rectangles and four discs, an S-curve 24 lines, a dashed edge a line per
dash, the stage's dot grid a rectangle per dot -- so its op count grows with the tree, the zoom
and the window, and a cap on dashes was needed to keep it inside `WIKI_MAXOP`. And every list is
rebuilt and rewritten every frame, the still ones too.

The goal: no persistently mapped instance memory at all, every shape one instance, and the tech
tree's dashes the prototype's at any zoom.

## The landings

**D1: the tree's shapes as shapes.** Three op kinds the shader draws per pixel, each one
instance: a rounded rectangle (`HOP_SOLID` with a corner radius, the drop shadow's too), the
S-curve (`HOP_CURVE`: the prototype's cubic between two points, its handles at the midpoint's x,
dashed per pixel by the canvas rule -- a ribbon of `2 (HUD_CURVE_SEG + 1)` vertices built in the vertex
shader) and the dot grid (`HOP_GRID`: one quad over the canvas). The mockup draws the same shapes
with canvas paths (`roundRect`, `bezierCurveTo` with `setLineDash`); `techcheck.mjs` pairs shape
for shape. The dash cap and the cull of edges off the canvas go: the ops are bounded by the
tree's cards and links, whatever the zoom. *Exit*: techcheck pairs every setup; a pixel check of
the game's tree against the mockup's at the same camera; wikilaycheck; the techtree lane.

*Built.* Escalation with every link at the game's stage: 10 547 ops before, 1 080 after.
`tools/wiki/treecheck.py` (new) compares the lane's two tree shots with the mockup's canvas:
0.76 and 0.80 levels apart on average, 0.4 % of the canvas more than 48 apart, at 1920 x 1080 on
stock -- the cards' shrunken pictures and the text, which D1 does not touch. Each card is still
five ops (its shadow, the selected card's halo, the rim, the fill, the header's line); folding
them into one is left: the op count no longer depends on anything but the tree.

**D2: the tree kept on the device.** The tree's instances are built at a zoom with the camera's
translation taken out -- whole device pixels, so a pan moves every edge by whole pixels -- and
uploaded through the pass's staging (`tagpu_vk_stage`) into device-local memory only when they
change: zoom, selection, All links, the canvas, the glyph atlas's generation. Each frame draws them
with the camera's translation as a push constant. A pan sends nothing. *Exit*: the lane pans and
zooms, the log counts the uploads (none on a pan), the pixel check as D1.

*Built.* The pan is a whole-pixel shift in both drawings (techtree.js X: a point rounded at the
zoom, the camera's offset rounded on its own) and every card is drawn whatever the pan, so the
tree's ops are a pure function of everything but the pan (`WTSHIFT`); techcheck moves them by the
shift and pairs every setup, its budget case now the most zoom with every link: Escalation 1 348
ops and 4 976 bytes of text, under `TREE_MAXINST` (16 384 quads, 1.5 MB). The constants are not
push constants: the HUD's shaders are GL 3.3 text translated by `spirv-gen.py`, which carries no
push-constant block, so they are a second vertex binding of stride 0 in a device-local buffer
written by `vkCmdUpdateBuffer`. The lane's drag (tt3) sends the tree no time (the dump's
`tech.sent`), which took the fit's zoom at the packet's float precision: a drag's camera comes back
as a float, and a double fit made the first drag rebuild the tree once. treecheck as D1: 0.76 and
0.80 apart, 0.4 % loud.

**D3: the whitebox stage's ground.** The map's cells, grid and fog drawn per pixel from small
images uploaded when they change, instead of an op a cell. *Exit*: stagecheck, the whitebox
entries' lanes, the op counts before and after on the largest map.

*Built.* One image, `HUD_TEX_CELLS` (128 × 128 RGBA8: the ground's shade in rgb, the fog in
alpha), made on the render thread from the view the stage was built from and sent whole when a
byte differs, as the sidebar column is; `HOP_CELLS` draws it per pixel, the ground with or
without its grid, then after the features the fog. The rule changed in the mockup too: cell k
covers the pixels from `round(X(k))` to the next cell's, the last one pixel past the map, where
each cell used to be a rect `ceil(cp) + 1` wide overlapping its neighbour -- which laid the fog
twice on every seam, and the grid's ends were antialiased. stagecheck pairs each layer and the
image's hash with the mockup's. On the largest map the demos show (4 608 cells) the ground, grid
and fog took up to some 14 000 ops (4 608 cells, about 4 750 grid pieces, up to 4 608 fog cells);
they take 2. The whitebox minimap is still an op a cell and is what now bounds the map's size.
stagecheck pairs the op and the image, not the pixels: nothing renders the shader's cell rule
against the mockup's fillRects, so their agreement rests on the rule being one formula on both
sides and on the whitebox picture looked at (the stage3d lane's `nomap` lever shot). A pixel check
of that shot against the mockup is the gap.

**D4: no mapped instance buffer.** Every other list -- the replay HUD, the page, the stage HUD,
the whitebox's units -- is built into the frame's staging and copied into a device-local buffer of
the slot before the pass draws; the persistent mapping is removed. A frame takes from the staging
what its instances need and no more. *Exit*: the HUD's and the wiki's lanes, a replay with its
HUD, the log's peaks, the address space mapped before and after.

Each is a local landing on `worktree-wiki_tech_tree`; the feature lands on main once, after D4.
