# Factory guard: seats, a lane out, the arm kept open

Builders told to guard a factory are seated where they can help build without moving, packed round
the plant, and a lane stays free for each movement class of the plant's ground products. The plan,
with the owner's decisions and the landings still to come, is [the plan](../factory-guard-plan.html).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). The engine side is in the engine map: the guard order and HelpBuild's band in [orders](../engine/orders.html), *The ground Guard's node* and *HelpBuild's walk*; the pathing rule that makes a still unit an obstacle in [map](../engine/map.html), *The pathing maps*. The site is in the [hook map](hook-map.html).

## What it does (`tagpu_guard.c`, `tagpu_guardgeo.c`)

**Who it acts for.** A ground builder (def `+0x241` bit `0x40`, a mover, not a flier) whose guard
order targets a **finished plant** whose build menu, for its owner's side, makes at least one mobile
unit. Anything else -- a tank guarding a plant, a builder guarding a commander, an air constructor,
a nanoframe -- runs as stock. A plant that makes only aircraft gets seats and no lane; a plant that
makes both keeps the lanes of its ground classes only.

**The hook.** The guard order's state 1 reaches its assist at `0x40654B` on every wake, and every
path to its home move passes there. The stub calls `guard_visit`, which points the guard node's
offset `+0x22..+0x2D` and radius `+0x36` at the builder's seat, or at its wait spot, before the home
move `0x4066AC` reads them, and skips the assist for a builder not standing on its seat, so a waiting
builder does not walk in to help and one on its way does not take a help that walks it elsewhere. A
builder on its seat takes the assist as stock, and the HelpBuild it gets is in range from there, so it
builds without a step.

**The band is the engine's.** HelpBuild's one range rule is the annulus its state 0 builds round the
frame's centre: in range when `(rin/16)² ≤ d² ≤ ((builddistance + rin)/16)²`, d from the place the
builder would have centred on the frame, rin from the frame's footprint, and nothing after the goal
tests range again ([orders](../engine/orders.html), *Range is tested once*). A seat lies in that band
for every product the plant makes: from the largest product's rin to the builder's builddistance plus
the smallest product's rin. The frame's centre is read from the plant's BuildingBuild head while it
has a frame and kept in the plant's row (the plant's own centre until the first frame is seen); a
frame elsewhere re-plans the plant. The frame stands in the yard, so with OTA's builddistances the band
barely leaves the footprint -- ARMCK (40) on an ARMVP's products (rin 22..30) has one seat, behind the
plant -- and a mod's longer builddistances give more. Builddistance and every help keep stock's rule.

**The lane.** For each ground class the plant builds, a breadth-first search from the yard's exits --
the footprint edges holding cells a product walks off -- over the places where the class's largest
product fits the class's content map and covers no structure, to the first place clear of the plant
by the seats' reach. The exit nearest the plant's rally point (a QMove on its list) is tried first.
The path is kept as rectangles grown by one cell either side. Other exits may be covered. A class
with no way out keeps no lane: nothing it builds could leave.

**The seats.** A seat is a place in the plant's box that the builder's class can stand on, inside
its band, covering no building's footprint (its open yard included: those cells are held by no unit
in the map, so a test of the cells alone would seat a builder inside the plant; a plant with only its
yard in the box is found from its held cells outside it), no lane of any plant in the box, no other row's seat or wait spot
and no unit standing still (a mover that has not changed cell for 30 ticks, which the pathfinder
itself treats as an obstacle), and from which a way leads out of the box for the builder -- and still
leads out for every builder already seated. Seats are taken from the band's outer edge inward, so a
builder never boxes in one seated before it. A seated builder keeps its seat; it is re-planned only
when a structure in the box appears or goes, and then only the seats
that no longer hold go back to waiting.

**Waiting.** A builder with no seat stands where it is when that is free of lanes, seats and
footprints, else at the nearest free place within 8 cells, and takes a seat at a later visit when one
frees. A seated builder that has stood on one cell short of its seat for 180 ticks gives it up, waits, and
avoids that seat. A builder outside the box waits at the box's nearest place to it.

**Neighbours.** At a re-plan every other finished plant of a ground product whose footprint stands in
the box gets a row and its own lanes, so no seat or wait spot stands in another plant's way out, and
its seats are checked again like the plant's own; then every seat of any plant that a lane now covers
goes back to waiting.

## State

`s_plant` (64 rows: the plant's slot, birth and type, its first cell, the frame's centre, its box, the
signature of the structures in it, its lanes) and `s_seat` (256 rows: the builder's slot and birth,
its plant's row, its seat or wait spot, the cell it last stood on and since when, a seat it avoids) are
SIM_STATE; a row names a unit by slot and birth, so a recycled slot never matches, and a builder's row is
freed at the next visit after its builder dies or stops guarding its plant. `s_type` (the
level's plant types by side: their ground classes, each class's largest product, the band's terms)
and `s_reach` (the widest builddistance of a mobile assisting builder, at most 400 px) are
SIM_CONTENT, read at the level init from the defs and the build menus. When a table is full, a
further guard runs as stock. The visit runs only on the builder's owner's machine (the order run's
owner test) and never on a puppet level: a replay's builders move as the recording says.

## Log lines

- `guard: installed: the guard's assist 0x40654B` at attach.
- `guard: level plant types=N sides=S reach=R` at each level init.
- `guard: lanes plant= type= cell= exits= rally_exit= classes= rects= box=` at each plan of a plant.
- `guard: seat builder= plant= at= band= seated= lanes=` when a builder is seated;
  `guard: wait builder= plant= at= ...` when its wait spot changes.

## Not yet done

- **Products park as stock.** With no rally a product's Park ring (8n × 6n round it) ends just
  outside the yard's mouth, in the lane, and later products queue behind; the owner keeps that.
- **The arm still folds between units** (StopBuilding at the end of every HelpBuild); landing 3.
- **No mod switch yet**; landing 4. The feature is always on.
- **ImpAI's yard rule** keeps its own copy of the yard's exits and its own way out; landing 5 puts it
  on `tagpu_guardgeo.c`.
- A damaged plant still pulls every guard, a waiting one too, into repairing it (the repair branch
  runs before the assist). A builder that cannot reach the box at all is still sent to the box's
  nearest place and stops wherever its path search leaves it.
