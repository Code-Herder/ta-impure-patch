# Build scatter: a builder's idle units leave its build site

A building can be placed on ground the player's own idle units stand on. When the builder arrives
and the engine refuses the site because of them, they are moved off it, each to the nearest place it
can reach and stand on, and the build starts. The plan, with the owner's decisions and the landings
still to come, is [the plan](../build-scatter-plan.html).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). The engine side is in the engine map: the refused branch, its counter and who can stand in the way in [orders](../engine/orders.html), *MobileBuild's blocked site*; the yard test and its occupant compare in the same page's `0x47D2E0` row; the controller's default order under *The controller*. The sites are in the [hook map](hook-map.html), *A builder's blocked site*.

## What it does (`tagpu_scatter.c`, `tagpu_scattergeo.c`)

**The engine's rule.** MobileBuild tests its site when the builder has arrived, with no unit ignored:
any unit on a cell the structure stamps refuses it. The builder says "Waiting for target area to
clear", tests again every 30 ticks and abandons the order at the twelfth refusal. The build cursor
asks the same test, so stock refuses the placement in the first place.

**Whose builders.** A seat a local human controls (its record's controller byte `+0x73` is 1). An AI's seat, stock's or ImpAI's, builds and gives up as stock, and so does every seat on a
puppet.

**Who is moved.** A unit standing on the refused site that is the builder's owner's, a finished
ground mover that nothing carries, and **idle**: its order list is empty, or headed by the node the
order controller makes from its type's default order (the head carries `0x4000` and its action is the
def's byte `+0x230`), or holds a lone Park. Everything else stays where it is and the builder waits
and gives up on it as stock: a unit doing anything a player or the engine ordered, a structure, an
aircraft, a frame being built, a passenger, another player's unit (an ally's included), a wreck or
any other feature. The one rule is `movable`, which the tick and the cursor both apply.

**Where the engine is touched, and where the orders are given.** Two stubs sit on the refused branch,
`0x403CD0` in MobileBuild and `0x41400D` in its VTOL twin. They run inside the builder's order
handler and write only the module's own tables and that node's retry counter. Order lists are
changed in one place, `tagpu_scatter_tick`, which the players phase calls each simulation step after
the unit tick has returned (`tagpu_allysight.c`, `players_phase`): no order handler is on the stack
and no list is being walked. A move is given there through the root a player's click uses,
`0x43ADC0` with no Shift, by the engine's WORLDQ `order` op, which checks the unit's incarnation, its
owner and that the seat is a local one. What the engine then does to the unit's list is what it does
at every click.

**The plan of one refusal** (`plan`). The box is the site grown by 12 cells on every side, clipped
to the map. Three rasters are filled from it: *wall*, every cell a structure holds; *taken*, those,
every structure's whole footprint (a yard's open cells are held by no unit in the map), every site
the module knows and every place a row has already sent a unit to; *occ*, the mover standing on each
cell. The site's units are taken in the order its cells hold them, row-major, up to 64 a refusal. For
each, a breadth-first search (`sg_way_off`, four-connected, neighbours north, west, east, south) runs
from the unit's own place over the places its footprint fits, its movement class can stand on
(WORLDQ's `passable` map) and no wall cell is under, to the first place whose footprint covers no
taken cell and no cell another unit stands on. The unit is ordered to that footprint's centre and
the place is marked taken before the next unit's search. A unit with no such place in the box is
left, and counts as `noway` in the log.

**A row owns one order, never a unit.** Each move is kept as a row: the unit by slot and birth, the
move by its action byte and point, the site and the place. Every step a row is checked: it stays
only while that slot holds that incarnation, alive and its owner's, and the head of its list is
still that move by value. Anything else frees the row and nothing is done to the unit. So:

- a player's order to a moved unit wins at once, whatever it is: it replaces the head, and the row
  goes at the next step;
- a unit that dies while walking, or whose slot is taken by another unit, frees its row the same
  way; no address is kept, so there is nothing to dangle;
- a unit ordered back onto the site by hand is left alone while that order runs; when it has ended
  and the unit stands idle on a site the builder is still refused on, it is moved again;
- the row of a unit that has arrived goes when the engine frees the finished move.

**The hold.** The stock abandon counts refusals, not time, so units still walking off would use the
builder's patience up. While a unit the module moved is on the site and has changed cell within 180
ticks, or another of the owner's units is crossing the site under a move and has changed cell
within 180 ticks, the stub takes one off the counter before stock adds one: the refusal does not
count. The counter never goes below 1, so the first refusal's reply is said once. A site with
nothing moving on it counts as stock and is abandoned as stock.

**A site's row** is kept per builder (slot and birth): the product, the first cell, the step of the
last refusal and whether the next one is held. It is dropped 61 steps after the last refusal, or when
the builder is no longer that live incarnation.

## The cursor (`tagpu_scatter_site_test`, `tagpu_patches.c` `build_site_test`)

The build cursor's call of the yard test `0x4198C6` goes through `build_site_test`. It asks stock
first. Refused, it asks once more with bit 16 of the test's *ignore* argument set and the placing
seat in bits 17 to 20; a stub on the test's occupant compare `0x47D554` then passes a cell whose unit
`movable` would move for that seat. Every other test of the site -- the map's edge, the mapped
ground, structures, features, slope, water -- is stock's, in the same call. The queued-site rule
(*a queued site is taken ground*, [engine patches](engine-patches.html)) is applied to the answer
either way. The bits ride in the call's own argument, which the test reads as a word at the compare
and nowhere else before its tail overwrites the low byte, so nothing is kept between calls.

A site accepted this way is ordered as any other. The builder's own test ignores nobody, so it
refuses the site on arrival, and that refusal is what moves the units: the cursor promises only what
the tick will do, by the same rule.

**The builder itself.** A selected builder with nothing to do is an idle unit, so the cursor accepts
a site it stands on. No move is given for it: MobileBuild's own approach walks the builder off its
site before the test (MEASURED 2026-11-20, an ARMCK and a solar collector placed on it: the builder
ends 50 px north of the site's centre, the solar is built and no refusal is logged).

**The square.** The frame packet carries `site_moves`, the cursor's last answer, and the marker pass
(`tagpu_mark.c`) draws the build square in the GUI palette's yellow instead of its green when it is
set. The several-buildings gestures ([placement](placement.html)) judge each site with the same test,
so they accept such sites too; their previews keep their own colours, where yellow already means
"this click removes a queued build".

## State, replay and threads

Sites and rows are `SIM_STATE` tables (`SIM_ROWS`, 32 sites and 256 rows), the level's widest yard
is `SIM_CONTENT`, the stubs `SIM_CODE`; the cursor's last answer is display state of the local
machine (`SIM_PRIVATE`). A row stores a slot, a birth, an action byte, cells and a 16.16 point: no
address. The moves are orders through an engine root, so a recording carries them as it carries a
click's. **On a puppet nothing of the module runs**: the stubs, the tick and the cursor's second
question return at once under `tagpu_puppet_on()`, and the recorder is the only writer of the tables.

The feature declares itself `SIM_PUPPET_OWNER` with the test groups `build-scatter` and
`stock-build-scatter` (the same game on the stock-limits build). Their game
(`scenarios/build-scatter.json`, Two Continents): two ARMCK place a solar collector each over four
idle ARMPW through the build panel and native clicks; one moved unit is ordered away by hand and one
back onto the site; every unit is removed while the second site's four are walking and their builder
waits (`build-scatter-clear.json`); and on a third site a wreck is dropped after the cursor accepted
it (`build-scatter-wreck.json`). `tools/replay/scatter_check.py` asserts on the live game, at two
held GameTimes, that all four were moved off each site, that the unit ordered back in was moved
again, that the unit ordered away went where the player sent it, that the first solar stands, and
that the wreck's site was abandoned with its four units off it.

The geometry is engine-free (`tagpu_scattergeo.c`, listed `engine-free sim`) and has a native
self-test under ASan and UBSan: `python3 tagpu/ddraw/tests/scattertest.py`.

**Bounds.** A unit pointer a stub is handed is tested to be a record of the unit array before a byte
of it is read; its owner must be a seat a WORLDQ view opens for; the product's def must be a record
of the def array and a structure; the cell must hold the footprint on the map; every slot read from
a map cell is bounded by the slot count. **Threads.** The tables are cleared and the yard read on the
loader thread at the level's PLAYERS call; the stubs, the tick and the cursor run on the game thread.

**Log.** `scatter: installed: the blocked site 0x403CD0 0x41400D` and `scatter: installed: the
cursor's site 0x47D554` at attach (the second address of the first line and the whole second line
are absent when those bytes do not match). One line a plan that moved something, and one for a
site's first plan whatever it did: `scatter: GameTime N: site builder=<slot> type=<def>
cell=<x>,<z> <w>x<h> on=<units on it> moved= noway= busy= other= full=`.

There is no setting: the module is installed in every game.

## Gaps

- **Only idle units are moved.** A unit standing on the site under an order that keeps it there --
  guarding, patrolling through, attacking from where it stands, a builder building -- is `busy`:
  the cursor refuses the site and a queued site is abandoned as stock.
- **Builders seated by the factory guard are busy units.** The guard planner does not know a
  scatter site and the scatter planner does not know a lane or a seat, so a moved unit can be sent
  onto a plant's lane ([factory guard](factory-guard.html)).
- **Aircraft are not moved**, landed ones included.
- **A destination can lie on another builder's queued site.** Only sites already refused, built
  structures and other rows' places are kept clear; the unit is moved again when that site's
  builder arrives.
- **A unit boxed in** -- no free place within 12 cells of the site that it can reach -- stays, and
  the builder gives up as stock.
- **A moved unit that cannot get to its place keeps the move**, as after a click on a place it
  cannot reach; it stops holding the builder after 180 ticks on one cell. The search knows the
  movement class's map and the units in the box, not a wreck or a tree that appeared since the
  level's load.
- **AI seats are not served.**
- **The several-buildings previews show such a site green**, not yellow.
- **The GDI renderer draws the engine's own square**, green.
- **No per-mod switch.**
