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
| 5 | Shift queues another ring | and holding Shift shows the queue with the stock order markers |
| 6 | Without Shift it replaces | a queued move is dropped; the reclaims on the ring's targets stay |
| 7 | Cancel | a right press mid-drag ends the drag and keeps reclaim armed; an empty ring stays armed too |

A demo arms reclaim by a `reclaim` trip, the cursor's way to the RECLAIM button as the `build`
trip finds a build button (through ORDERS when the page shown has none), so every demo plays in
every setup; demo 2 presses E, which reaches RECLAIM on the page reclaim was armed from in every
setup. The text names E.

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

| | What | Gate |
|---|---|---|
| **L1** — planned | the mockup: the layout's quickkey and `reclaim` role (`gfx.py`, `checkLayout`), features in `sim.js` and the stage description (`stagemap.py`'s recipe), the sweep in `sim.js`, its drawing in the mockup, `gen-reclaim.mjs` and the seven demos, the claims in `simcheck.mjs` | `simcheck.mjs` on every pack and casting; the demos watched in the mockup by the owner |
| **L2** — planned | the DLL's simulation: `tagpu_wikisim.c` and the loaders (`tagpu_wikidata.c`, the layout's quickkeys) | `simparity.mjs` natively and `--wine`; `wikitest`'s malformed cases |
| **L3** — planned | the DLL's drawing: features removed from the stage's ground as they are reclaimed, the sweep's ring, brackets, popup and tag through the wiki's HUD ops, the queued markers | `wiki_regression.py` in all six compatibility setups; the demos watched in game |
| **L4** — later | 3D wrecks on the stage, mockup and game, swapped into the METAL demos | `stage3dcheck.py` |

## Gaps open

- Retail's E on a build page (decision 2).
- 3D features (decision 3).
