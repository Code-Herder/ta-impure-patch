# The pathfinder: our search, in parallel (plan)

The ground pathfinder runs a tick's searches one after another on the game thread, out of a budget
a tick. On a battle of 13 500 units that is about 6 ms of a 31 ms tick. The plan replaces it with our
own search, run for every unit that wants a path in the tick, on worker threads while the game thread
waits, with the results applied in unit order. The engine facts each decision rests on:
[units, the pathfinder](engine/units.md) and [the map, the pathing maps](engine/map.md).

## The owner's decisions (2026-11-20)

1. **Our own code replaces the engine's search, and a tick's searches run in parallel.** The game
   thread collects the units that want a path, waits while workers search, and applies the results in
   unit order.
2. **No budget a tick.** The engine's `[pf+0x48]` (1333 in stock, 66 650 under the raised limits)
   rations one processor. In its place: a cap on the searches started in a tick, counted in
   searches and never in milliseconds, so that every machine starts the same ones; what is over the
   cap is served first on the next tick.
3. **The 60-tick gate** (`0x44F260`: a unit that wants a path is refused until 60 ticks after its last
   search began) is split by why the unit wants one:
   - **a new order** searches at once. Stock opens the gate on a new goal only when the unit's last
     search began 10 ticks ago or more (`0x44F426..0x44F436`), so a second order within a third of a
     second leaves the unit up to two seconds on its old path;
   - **a unit out of waypoints** (a path is handed over 20 corners at a time; count below 2,
     `0x44F24B`) searches at once;
   - **a blocked unit** (mover `+0x2E` bit 2, `0x44F245`) keeps a limit, shortened once the number
     of units the gate holds each tick is measured, and is served after the new orders when the cap is
     reached: a jam never delays a fresh order.

These change the simulation's rules: every player of a network game runs the same build, and a
recording made before them does not play after them.

Confirmed on the scheduler as built, with its measurements (2026-11-20):

4. **Decision 3's first case stands as built**: a new goal searches at once for a unit whose last
   search found a path; a unit whose last search found nothing keeps the engine's 10 ticks (rule 2
   below).
5. **The caps are 64 searches and 64 000 pops a tick.**
6. **A blocked unit's limit stays the engine's 60 ticks.**
7. **The search's scratch is `SIM_TRANSIENT`** (`s_pf`, `s_mem`, `s_mem_bytes`): written by each
   search before it reads it, and not recorded.
8. **The oracle's functions land without a replay test group reaching them**: they run only under
   `tagpu_pathverify.on`, which no test group arms, and the replay gate's coverage names them at
   the landing on main.
9. **A batch is 16 searches** (the fourth landing's measurements, *Open*): 8 as first built left
   the helpers idle around each long search.
10. **The batch in hand is `SIM_TRANSIENT`** (`s_run`): what the scheduler's thread hands the
    helpers, all zero between two batches, and not recorded.
11. **The searches run while the tick goes on, and are joined in their own tick** (2026-11-20): the
    game thread waits for them at the step's end and not inside the scheduler. The pops cap stays,
    tested between groups by the threads that search (the owner's go-ahead named it as dropped,
    which measured 1.6 million pops in one tick).

## The scheduler's rules as built, and what each rests on

The rules themselves: [the module](modules/pathfinder.md). Each was arrived at by playing the
13 500-unit battle (MEASURED 2026-11-20, the reference setup, about 3500 ticks a run, the frame
profiler's `pathfind` span and the scheduler's log).

1. **The weight follows the load.** With the base weight for every search — the first rule
   tried, on the ground that no player is served slowly any more — a search popped 6300 nodes on
   average against the engine's 1200, the scheduler took 9.0 ms a tick, and the opening's backlog
   made a unit wait 100 to 260 ticks. The engine's raised weights are what makes an army's searches
   cheap: the same 3152 captured searches pop 2900, 510 and 410 nodes at the base, 3 times and 6
   times. So the three weights stayed, chosen by the tick's load instead of a player's budget: the
   base while at most 4 units are ready, 3 times while every ready unit can begin this tick, 6 times
   past that, where a search pops a fifth to a quarter less than at 3 times.
2. **A new goal opens the gate at once only for a unit on a path.** Decision 3's first case was
   first built as "every set goal with a goal zeroes the stamp". The order handlers answer a search
   that found nothing with a new goal within a tick or two, for as long as the unit stays where it
   is: late in the battle that was 58 set-goal calls a tick inside 10 ticks of such a search, each a
   search again, 85 % of all searches ending with none made, and the cap of 64 a tick spent on them.
   The engine's 10-tick rule is what stops that loop. So it stands for a unit whose last search
   found nothing, and the gate opens inside the 10 ticks only for a unit whose last search found a
   path: the case decision 3 names, a unit on its way that is sent elsewhere. In a game 5 % of the
   set-goal calls are opened that way and 10 % left to the engine's rule.
3. **A unit at its path's end searches at once**, with no floor between two searches of one unit: a
   floor of 5 ticks held no unit in 3300 ticks, a found path having at least two corners.
4. **The caps are 64 searches and 64 000 pops a tick.** The scheduler's time is 150 to 190 µs
   a search begun, the walk and the classes' sweeps included, so a tick at the searches' cap is
   about 10 ms of the game thread; the pops bound a tick whose searches are long, and one that
   finds no way opens every cell it can reach. The opening runs at the pops cap for its first 150
   ticks.
5. **The blocked units' limit is the engine's 60 ticks.** It holds 500 to 1700 units a tick, and
   the searches it lets through are the ones that find nothing (one search in six); shortening it
   buys more of those.

## What a search reads, and the one thing two searches share

MEASURED and DISASSEMBLED 2026-11-20; the evidence is `.data/pathfinder-parallel/` in the main
checkout (local, not published).

- **A search is a function of**: its class's pathing map, the explored map and the served player's
  bit of it, the searcher's cell and heading, its goal, and the heuristic's weight. No random draw,
  no floating point, nothing left by the search before it.
- **The pathing map is the one thing a search changes for the next one.** A unit that has stood
  still for 30 ticks is an obstacle in its class's map. When the searcher is one, the search start's
  watermark `0x440AF0` recomputes its rectangle with the searcher left out, so that it does not
  stand in its own way, and the cleanup `0x440BE0` puts it back; the first search of a class in a tick
  also marks the units that have just become obstacles.
- **So a searcher that is an obstacle reads a patch of its own.** The shared map holds every
  obstacle and is not written while searches run; such a search reads the entries its own rectangle
  reaches from a private copy, recomputed by the engine's rule with that one unit left out
  (`tagpu_pathfind.c`, THE PATCH). A searcher that is no obstacle reads the map as it stands, as
  the engine does. The marking of the units that have just become obstacles is done once a class,
  before the workers start.
- **The map is a cache, and the engine reads it stale in places.** MEASURED 2026-11-20: recomputing
  every searcher's rectangle afresh disagreed with the map in 3 to 5 entries of 2.9 million, and one
  search in 70 000 then failed where the engine's found a path: a unit made inside a plant's yard,
  whose cell the rule gives as blocked by the plant and the map as passable. The patch is
  therefore made only where the engine itself recomputes.
- **The question "does this unit want a path?" writes**: a yes stamps the unit (`+0x60`). It is asked
  once a unit a tick, on the game thread.
- **Applying a result** writes the unit's own path and ORs flags into its own order: nothing another
  search reads.

## What it costs today

MEASURED 2026-11-20 on the 13 500-unit battle, 86 928 searches over 3636 ticks, the reference setup:

| | share | each |
|---|---|---|
| found a path | 85 % | 142 µs, about 1200 pops |
| no search: the start is in the goal, or the straight walk gets no nearer | 15 % | 8 µs |
| the open set emptied | 3 searches | |

The search start's watermark is another 40 µs a search, 0.9 ms a tick. The slowest 5 % of searches
take 38 % of the time and the longest took 5.5 ms, so a tick's searches on four workers end in about
0.95 ms against 2.8 ms on one thread, and more workers add nothing: a tick cannot end before its
longest search.

**The budget is what a unit waits on.** It is shared equally among the players, so a player with
many units to path spends its share on a handful of searches while the others' goes on asking
units that want nothing. At the scheduler's entry 1180 units a tick on average, 7125 at most,
wanted a search with the gate open, and about 23 searches began a tick: from the gate opening to
its search's start a unit waited 48 ticks on average and 479 at most; 24 % did not wait and 40 %
waited 30 ticks or more. The gate itself held 676 units a tick: 434 blocked, 8 out of waypoints,
234 on a new order. So without the budget the first ticks of a battle ask for thousands of
searches at once, which is what the cap is for, and afterwards for about as many a tick as arrive.

Our search, `tasim_path.c`, gives the engine's result on every one of 3152 searches captured from that
battle, and on one thread takes 81 to 102 % of the engine's time. The gain is the workers' and, after
them, a cheaper search.

## The landings

1. **The search and its oracle** — `ts_pf_search`, one search whole on a map it does not write, with
   the patch; and the verify lever `tagpu_pathverify.on`, which runs it beside the engine's on every
   search of a game, compares the two results and the patch with the map, and counts the units the
   gate holds, by why. Changes nothing in a game. **Built 2026-11-20**: equal on every search that ends in
   its tick ([the module](modules/pathfinder.md)).
2. **Our scheduler on the game thread** — the units that want a path collected in one pass, searched
   by our search one after another in batches, the results applied in order; the caps; the
   gate's split; the weight by the tick's load. The engine's scheduler and search no longer run.
   **Built 2026-11-20**: 30 to 34 searches a tick against the engine's 24, a unit's wait 20 to 23
   ticks on average against 48, for the engine's time a tick or up to two fifths more (4.3 and 6.3 ms in two
   runs against about 4.5; [the module](modules/pathfinder.md)).
3. **The workers** — a batch's searches on the scheduler's thread and up to three helpers at once,
   the scheduler's thread inside the call until the last helper has answered; the still units listed
   once a tick for every class's sweep. **Built 2026-11-20**: 2.4 ms a tick on four threads against
   3.9 on one and the engine's 4.5; every result equal to the one-thread search's
   ([the module](modules/pathfinder.md)). The thread count is the machine's cores less one, at most
   4: threads past four end a batch no sooner.
4. **A cheaper search** — the same pops in the same order for less: the heap's entries carry their
   node's `f`, a cell's read takes its constants from a view made once a pass, the grid's reset
   steps over the touched blocks. **Built 2026-11-20**: on the captured searches 11 % less time a
   search at the base weight, 14 % at the capture's own weights and 18 % at 3 and at 6 times (64.2 to
   52.7 µs and 53.3 to 43.9, built natively), every result the engine's (3152 of 3152 offline at the capture's weights, the same
   results at the three weights before and after, and 74 304 of 74 304 in a game beside the
   engine's). In the battle the scheduler's time is the batches', not the searches' sum, so it
   moved little: 2.4 ms a tick in a run of a fifth more pops.

5. **The searches beside the tick** — the tick's units chosen and their writes made at once; the
   searches on the helpers from the tick's copies of the maps, in groups of 16 under the pops cap,
   while the game thread goes on; the join at the step's call `0x4955E5` and at set goal.
   **Built 2026-11-20**: 0.87 ms a tick on the game thread against 1.95, in a run of a fifth more
   pops; 117 044 of 117 044 results equal to the same searches made inside the scheduler on the
   engine's own maps ([the module](modules/pathfinder.md)).

## Open

- **The owner's to confirm**: the tick's jobs, what is due of them and the tick's copies are
  `SIM_TRANSIENT` (`s_job`, `s_due`, `s_frz`, `s_frz_bytes`): written by the scheduler before a
  search reads them, nothing due between two ticks, the class the scratch and the batch in hand
  have (decisions 7 and 10).
- **The owner's to set**: the three weights' thresholds (4 and 64 units ready), a first choice.
- **What the batch's size measured** (it is 16, decision 9). A batch ends with its longest search,
  and the top 5 % of searches are 46 % of the pops, so a batch of 8 leaves three threads idle
  through each long one. MEASURED 2026-11-20, the battle on four threads with the cheaper search, one run each:
  batches of 8, 16 and 32 took 2.39, 1.95 and 1.65 ms a tick, on 22 700, 19 000 and 18 600 pops a
  tick — 105, 103 and 89 ns a pop, so the runs show a gain at 32 and none that the pops do not
  explain at 16. At equal work, the captured searches' costs packed in capture order onto four
  threads, 34 a tick: the searches' part falls by 17 % at 16, 27 % at 32 and 33 % at 64, which
  of a scheduler whose walk and writes are 0.7 ms a tick is about 10, 15 and 20 %. The size is a
  rule: a search reads the map as the writes of its own batch and the earlier ones left it, and
  the pops cap is tested between batches, so a larger batch can pass the cap by more — its own
  searches, each of which pops at most the cells its unit can reach.
- **What a cheaper search cannot reach without changing a result**: a goal that cannot be reached
  opens every cell the unit can reach before it fails; knowing that before the search (the map's
  connected regions) would end it at once, and changes which path a unit that cannot arrive gets.
  The owner's decision; not built.
- A goal of a kind other than the three ground kinds (point, annulus, ring) reaching a ground unit:
  none in the 300 000 searches the oracle counted and none logged by the scheduler in 500 000 more;
  not ruled out by the disassembly. It gets the result of a search that
  found nothing.
- A search past 65 536 open nodes: the engine's node word wraps there. It gets the result of a
  search that found nothing; none occurred.
- A group's searches can pass the pops cap together (218 000 pops in the worst tick measured);
  a group's time is its longest search's, and the join waits for what the tick did not cover.
- **Long searches** — a goal deep inside a base or a crowd, where the search floods a disc before
  it finds the way in: 39 % of searches and 67 % of pops on the captured set. Two ways on, neither
  built: a search past a count of pops delivered a tick later, and one search shared by the units
  sent to one place.
- Cores against hardware threads for the helpers' number: not measured (the module's gaps).
- ImpAI's simulator still models the engine's scheduler, and the recorder still lists its three
  tables, which nothing writes now.
