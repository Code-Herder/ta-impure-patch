# Guarding a factory: seats, a way out and the arm kept open (plan)

Builders told to guard a factory stop shuffling between units. Each one is given a **seat** near the
plant from which it can help build, the seats are packed so as many builders help as the ground
allows, a **lane** stays free for the plant's largest ground product, and a seated builder keeps its
build arm out while the plant has more to build. Inspired by TADR's "guarding construction units"
options, which do none of this: TADR only moves a guard's home point to the nearest map diagonal
(`TABugFix.cpp`, `FixedPositionGuardingConsProc`, hook `0x4066AC`), with no knowledge of the exit,
of the room around the plant or of the arm.

## The owner's decisions (2026-11-18)

1. **Every plant that builds a ground unit keeps one way out** for each movement class of its ground
   products, wide enough for the class's largest product plus a comfortable margin. Other exits may be
   blocked. Plants with several exits, or a yard open on every side, choose one.
2. **Air-only plants get the same behaviour without a lane.** A plant that builds both keeps the lane
   of its ground classes only.
3. **Builders guarding the plant are seated by a coordinator**: packed as tightly as the ground
   allows, each within its own build range of the plant's build spot, no seat in a lane, on the plant,
   in another plant's lane or on ground the builder cannot stand on or reach.
4. **A seated builder never moves** between units. A newcomer takes the best free seat; nothing is
   re-planned unless the ground changes (a building placed or destroyed in the plant's box, the plant
   gone).
5. **Every seat keeps a way out for its builder**, so a builder given another order can leave: no
   inner ring boxed in by the outer one.
6. **A builder with no seat waits nearby**, off every lane and footprint, and does not walk in to
   help; it takes a seat when one frees.
7. **The finished product does not park in the lane**: the Park goal is steered past the lane's end.
8. **The arm stays open** while the plant has more queued; once the queue is empty it folds after a
   grace, the measured gap between two units on a stock plant plus a margin. Any other order, the
   plant's death, or the builder dying or changing hands folds it at once. The fold and the next open
   are skipped as a pair, so the script sees one long build; the deferred fold goes through the
   engine's own `0x4385F0`, so the broadcast to the other players is deferred with it.
9. **Commanders** guarding a plant are builders and are seated like any other.
10. **Each machine seats only its own units**; another player's units, an ally's included, are
    obstacles where they stand.
11. **One hard switch per mod, on by default.** A mod's catalog entry can turn the whole feature off
    (seats, lane, arm), with a reason; a player cannot turn it back on, and there is no player switch.
12. **One way-out geometry for the coordinator and ImpAI.** The yard's exits, the product classes and
    the lane search live in one engine-free module; ImpAI moves onto it once its own branch has landed
    on main (its `impai_build.c` is 193 commits ahead on `worktree-Bar_game_AI` as this plan is written,
    and two writers in one brain file is what the one-writer rule forbids).

## Landings

| # | Landing | Proof |
|---|---|---|
| 1 | The seats and the lane: the engine-free geometry (`tagpu_guardgeo.c`), the coordinator (`tagpu_guard.c`), the home-move hook at `0x4066AC` and the assist skip for a waiting builder | a unit test of the geometry; a scenario: six ARMCK guarding an ARMVP stay put through a queue of ten, the products leave by the lane, nothing stands on the yard |
| 2 | The Park goal past the lane | the same scenario with no rally: no product stands in the lane |
| 3 | The arm kept open | the scenario's builders run StartBuilding once for the queue; a fold within the grace of the queue's end; a move order folds at once |
| 4 | The mod switch | a catalog entry with the switch off plays as stock |
| 5 | ImpAI on the shared geometry | after ImpAI's branch lands: its yard region and the coordinator's lane are the same rectangles |

## Engine facts the design rests on

All DISASSEMBLED 2026-11-18 in the pristine image unless marked; the entries are in the engine map.

- **The guard's assist `0x40654B`** is passed on every way to the home move, and a hook there can
  point the home move anywhere and skip the assist ([orders](engine/orders.html), *The ground
  Guard's node*). The attack reaction and the repair branch run before it, so a damaged plant
  still pulls every guard into repairing it.
- **HelpBuild's band** ([orders](engine/orders.html), *HelpBuild's walk*): an annulus round the
  frame's centre, `rin = (int)(16 √(fx² + 2fz)) / 2` of the frame's footprint, `rout =
  builddistance + rin`, in whole cells from the place the helper would have centred on the frame.
  Inside it a helper never steps; closer than rin it walks out.
- **A still unit is an obstacle** to every path search: the per-cell rule `0x47DE60` blocks a mover
  whose claim stamp is older than the class's watermark, GameTime − 30
  ([map](engine/map.html), *The pathing maps*). A unit walking into one is held and searches again
  at most once a 60 ticks; nothing was found that pushes the standing unit
  [not established beyond the readers of the mover's `+0x2E` bit 2].
- **Park's goal** is the border of a box of 8n × 6n cells round the product, n its footprint (+3 for
  a ship), the cheapest border cell taken [INFERRED from the search]: about 9 cells off for a 2 x 2
  unit, inside a lane. Landing 2's subject.
- **The factory's queue**: BuildingBuild count nodes on the main list, `+0x3A` the units still to
  make with the one in progress included; between two units the head's frame `+0x16` is NULL until
  the next create. Landing 3's input.
- **The arm**: a HelpBuild's StopBuilding starts in the node destructor `0x43A1F0` behind `+0x42`
  bit `0x400000`, which only StartBuilding `0x438590` sets ([cob](engine/cob.html), *The scripts a
  unit's work starts*). Landing 3's subject.
- **The handlers run on the owner's peer only** (`0x48AEB5..0x48AECD`), never for a puppet seat.

## Landing 1, as built

`tagpu_guardgeo.c` (engine-free: the yard's exits, the lane search, the seat search with its way out,
the wait spot; `tests/guardtest.py`) and `tagpu_guard.c` (the hook, the rows, the level's plant
types). The module as it stands is [the module note](modules/factory-guard.html).
