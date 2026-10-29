# The in-game wiki's reclaim entry, planned

The third entry of [the in-game wiki](ingame-wiki.html): Impure's reclaim sweep
([gpu-status §2.110](gpu-status.html), `tagpu_sweep.c`), shown and tried on the wiki's stage. Like
every entry it is data played through the wiki's simplified simulation, `sim.js` in the mockup
and `tagpu_wikisim.c` in the game, so a demo can only show what the rules do.

## The owner's decisions (2026-10-29)

1. **All seven demos** (below), one behaviour of the sweep each.
2. **E follows the mods' layout.** In the mods (Escalation, Total Mayhem, ProTA, TA Zero,
   Twilight) every build page carries RECLAIM, so E reaches it from any page; in retail only the
   ORDERS pages (`armgen`, `corgen`) do, so E does nothing while a build page is up. The entry is
   built against the mods' behaviour and retail's gap stays open; whether E should reach RECLAIM
   from any page in retail too is decided later.
3. **Rocks first, wrecks later.** METAL is shown on greenworld's metal rocks (`green\rocks.tdf`,
   metal 25..100) and ENERGY on its trees and shrubs (`green\trees.tdf`, `green\shrubs.tdf`): all
   2D features the stage already draws. Wrecks and heaps are 3D features (`armck_dead`,
   `armck_heap`, ...), which the stage does not draw in the mockup or in the game; a later landing
   adds them and swaps them into the METAL demos.

## The entry

**Reclaim Sweep**, under Quality of Life → Reclaiming.

| # | Demo | What the player sees |
|---|---|---|
| 1 | E and a click | RECLAIM is armed and a click on one rock orders it, stock's click |
| 2 | Pick the kind | E again cycles METAL → ENERGY → ALL; the popup's lit row and the tag at the cursor follow; a click on a row picks it |
| 3 | A METAL ring | two ARMCK; the drag brackets the rocks and leaves the trees in it unmarked; the release splits the rocks nearest-first |
| 4 | An ENERGY ring | over trees and rocks: only the trees are taken |
| 5 | Shift queues another ring | the second ring's targets go after the first's, and letting go of Shift puts reclaim away |
| 6 | Without Shift it replaces | a route of queued moves is dropped; the reclaims on the ring's targets stay |
| 7 | Cancel | the cancel action mid-drag ends the drag and keeps reclaim armed; an empty ring stays armed too; a cancel outside a drag disarms |

A demo arms reclaim by a `reclaim` trip, the cursor's way to the RECLAIM button as the `build`
trip finds a build button (through ORDERS when the page shown has none), and picks a kind off the
popup by a `pick` trip to that row (sim.js `pickTrip`, PICK ticks), so every demo plays in every
setup; demo 2 presses E, which reaches RECLAIM on the page reclaim was armed from in every setup.
The text names E. The scene is 40 × 30 cells of greenworld: two ARMCK, a lone rock, a field of
rocks with three trees in it, a grove of trees with two rocks in it, and plain ground for the
empty ring (`tools/wiki/gen-reclaim.mjs`).

## What the simulation gains

- **Features as targets**: an entry's `features` table (name, footprint, metal, energy, the TDF
  they were read from) and a scene's features at their anchor cells, removed when reclaimed. The
  stage draws them from the stage description as it draws deposits and decoration.
- **The sidebar's RECLAIM**: the layout's gadgets carry their quickkey and a `reclaim` role (the
  first button whose name contains `RECLAIM`, the engine's rule); its live and greyed states, its
  toggle frame, E matched against the shown screen's gadgets only.
- **The sweep**, `tagpu_sweep.c`'s rules over the sim's features: the kind and its popup, the
  ring and its threshold, the filter, the targets bracketed, the commit (Shift appends; without
  it every order but the reclaims on the ring's targets goes), the cancel, and the split and its
  ordering ported line for line from `tagpu_sweepgeo.c`, as `tagpu_placegeo.c` is.
- **The reclaim order on a feature**: the walk to its reach, the work, the feature gone at the
  end, the spray and the sound as a building's reclaim has them.
- **The view**: the ring, the brackets, the readout, the popup and the cursor's tag; queued
  reclaims as the stock markers draw them, under Shift.

## The landings

**L1, L2 and L3 land together.** The DLL loads the wiki as one archive and refuses the whole of it
when one entry does not parse (`tagpu_wikidata_load`), and main's loader knows neither an entry's
`features` nor the inputs `reclaim`, `pick` and the key `e`, so the entry in `tree.json` without
L2 would leave the game with no wiki at all; and without L3 the game would play the entry with its
rocks, ring and popup undrawn.

| | What | Gate |
|---|---|---|
| **L1** — built, not landed | the mockup: the layout's quickkey and `reclaim` role (`gfx.py`, `checkLayout`), features in `sim.js` and the stage description (`stagemap.py`'s recipe), the sweep in `sim.js`, its drawing in the mockup, `gen-reclaim.mjs` and the seven demos, the claims in `simcheck.mjs` | `simcheck.mjs` on every pack and casting; the demos watched in the mockup by the owner |
| **The grown map** — built, not landed, lands with L1 and L2 | the owner's answer on the popup (below): with the sidebar a stage's map grows to the column (`tagpu_wiki_map_rows`, `sim.js` *THE MAP*), the descriptions' ground 48 rows deep, the column at most 768 rows, the game look and the DLL's world drawing the map's top rows, each minimap built on its map's rows; mockup and DLL both | `simcheck.mjs` on every pack (passes); `scenecheck.mjs` and `assetcheck.py`'s stages (pass on a tree without the reclaim entry); the sim, side and stage parity with L2's |
| **L2** — built 2026-10-29, lands with L1 and L3 | the DLL's simulation: `tagpu_wikisim.c` and the loaders (`tagpu_wikidata.c`: the entry's `features` table, a scene's `features`, `kind`, the inputs `reclaim` and `pick`, the key `e`, the layout's `key`, a cast's `canreclamate`). Everything sim.js gained that the C side must mirror: the feature table and its checks (`checkFeature`), blocking features in `siteOk`, the spawn and `bodies`; the sweep (`popup`, `keyPress`, `reclaimPress`/`Move`/`Release`, `sweepCommit`, `sweepSplit` from `tagpu_sweepgeo.c`); the feature reclaim order (`featWork`: the walk to touch, the countdown `trunc(15 + (e+m)/2)` taken 2 a call every 2 ticks, the payout, the spray while above 15, the kind-7 notice for a feature gone); the trips `reclaim` and `pick`; the sound `specialorders`; the view's `mode`, `feats`, `gone` and `reclaim` | `simparity.mjs` natively and `--wine`; `wikitest`'s malformed cases |
| **L3** — built 2026-10-29, lands with L1 and L2 | the DLL's drawing: the features on the whitebox ground and out of the 3D image once reclaimed, the sweep's brackets, ring, readout, popup and the kind's tag through the wiki's HUD ops, the game's `cursorreclamate`, E from Try it, the RECLAIM lock's badge | `stagecheck.mjs`; `wiki_regression.py` in all six compatibility setups; the demos watched in game |
| **L4** — later | 3D wrecks on the stage, mockup and game, swapped into the METAL demos | `stage3dcheck.py` |

## What L1 built

- **The entry**, `art/wiki/entries/reclaim.json` from `tools/wiki/gen-reclaim.mjs`, under Quality
  of Life → Reclaiming in `tree.json`; its stage description `art/wiki/stage/reclaim.green.json`.
  `stagemap.py` places a scene's own features and its `check` holds each feature row of the entry
  (footprint, metal, energy, height, blocking, reclaimable) to its TDF section on the install.
- **The layouts' quickkeys.** A button's `quickkey=` is a key of its own section, after its
  `[COMMON]` block (`ARMGEN.GUI`'s ARMRECLAIM: `quickkey=101`); `gfx.py` reads it there into the
  layout's `key`. The fixtures' RECLAIM carries 101, and `fixture-tall.json`'s build pages carry
  STOP and RECLAIM as the 640-row mods' pages do. Every cast carries its FBI `canreclamate`.
- **The claims**, `simcheck.mjs` RECLAIM: what each ring marked and left, who was ordered what,
  and whether reclaim stayed armed. Passes on the fixture (every casting, both Interface Types)
  and on all six mod packs (stock, Mayhem, ProTA, TA Zero, Twilight, Escalation).
- **The drawing**, `stage.js` `drawSweep` and the whitebox features; the game look drops a
  reclaimed feature's anchor from its image. The popup is the whitebox's stand-in for Impure's
  own sweep art, which L3 draws in the game.

## What L2 and L3 built

- **The loader** (`tagpu_wikidata.c`) reads an entry's `features` table (at most 64; a name under
  32 bytes, a footprint of 1..16, amounts in 0..1e7, height 0..255, the flags booleans), a scene's
  `features` (the name, `at`, an optional id under 24 bytes) and `kind`, the inputs `reclaim`
  (only `true`) and `pick` (a kind), their trips, the key `e`, a gadget's `key` (1..255) and a
  cast's `canreclamate`. The pack (`tagpu_wikiasset.c`) reads a button's `quickkey` from its own
  section and the FBI's `canreclamate`, as `gfx.py` does, and the cursor sequence
  `cursorreclamate`.
- **The simulation** (`tagpu_wikisim.c`), rule for rule with `sim.js`: a scene's features (at most
  256) as blocking bodies and targets, RECLAIM's live state and toggle, the popup, the E key, the
  ring and its targets, the commit split by `tagpu_sweepgeo.c` itself (built on SSE2 with the
  wiki's simulation, whose sums are in double), and the feature reclaim order: the walk to the ring
  round the footprints, the countdown, the payout, the spray from the feature's box, the kind-7
  notice for a feature gone; in the view and the state hash. `sim.js` gained what the port found:
  builders' and orders' points floored as the game's `builder_inputs` floors them, the countdown
  on float amounts, a guard that follows a unit reclaiming a feature, Quick Build off while reclaim
  is armed, and the data rules the C loader holds (the feature id's 24 bytes, the 64-row table).
- **The stage** (`tagpu_wikistage.c`): the features on the whitebox ground, the sweep as
  `stage.js` `drawSweep` draws it and the kind's tag at the cursor; the 3D image leaves out a
  reclaimed feature's quads (`tagpu_wikiscene_dead`, `tagpu_wikiworld.c`), as `gfx.js` drops its
  anchor. A dash ending on a corner within 1e-9 px ends there, in `poly` and in `stagecheck.mjs`'s
  recorder alike: the ring's corners are whole world units, and the last bit of a length decided
  the corner otherwise.
- **The checks.** `simparity.mjs` adds the entry's 336 plays, nine reclaim rule cases and 39
  refusal cases (features, kinds, the two inputs and their trips, quickkeys, `canreclamate`):
  1344 plays, 66 rule cases and 232 refusals equal natively and under Wine. `simcheck.mjs` gains
  the rule cases; `stagecheck.mjs` pairs all 8576 frames (`cursorreclamate` among its stand-in
  sequences); `sidecheck`, `scenecheck`, `textparity` and `assetcheck.py` pass.

## The owner's answers (2026-10-29)

- **The readout's words: kept for now.** The game's readout counts every metal feature as a WRECK,
  so the METAL ring over rocks reads "8 WRECKS" (`tagpu_mark.c` `sw_readout`), and the stage says
  what the game says.
- **The popup on a short map: the map grows to the column, always.** The popup follows RECLAIM's
  row but stays on the map, and a 30-row map was shorter than a 768-row column, so the popup sat at
  the map's bottom, well above the button. The map now grows to the column (*The grown map* above;
  `ingame-wiki.md` decision 9), so it sits beside RECLAIM's row.
- **Queued markers under Shift: as they are.** The stage draws its own order lines for the selected
  units at all times rather than the stock markers only under Shift.

## Gaps open

- Retail's E on a build page (decision 2).
- 3D features (decision 3).
- The popup is the whitebox's stand-in in the game too: Impure's own sweep art is not drawn on the
  stage.
- `scenecheck.mjs` pairs the ground's quads once a stage, not a frame, so the 3D image's dropped
  features are not paired against `gfx.js`'s gather; the rule is one line in each (drop the anchor,
  or the quads of its feature).
- A column deeper than 768 rows does not exist on the stage: the pack builders (`gfx.py`
  `check_panel`, `tagpu_wikiasset.c`) leave out a screen that ends below row 768, and `checkLayout`
  and `tagpu_wikidata.c` refuse a layout that has one. Past it the map would outgrow the ground, and
  a deeper ground would need another metal crop: around cell (420, 370), in cells 300..560 by
  200..440, no 40-wide window at the plate's height of 139 is flat and feature-free 64 rows down
  (MEASURED 2026-10-29 with `stagemap.py`'s `crop_refs`); Seven Islands has one at (556, 40).
