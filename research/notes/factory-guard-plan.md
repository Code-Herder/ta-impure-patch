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

Filled from the disassembly before landing 1 is built; each with its address and how it was
established. See the engine map's [orders](engine/orders.html) page, *The ground Guard's node*.
