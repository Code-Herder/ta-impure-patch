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
     search began more than 10 ticks ago (`0x44F426..0x44F436`), so a second order within a third of a
     second leaves the unit up to two seconds on its old path;
   - **a unit out of waypoints** (a path is handed over 20 corners at a time; count below 2,
     `0x44F24B`) searches at once;
   - **a blocked unit** (mover `+0x2E` bit 2, `0x44F245`) keeps a limit, shortened once the number
     of units the gate holds each tick is measured, and is served after the new orders when the cap is
     reached: a jam never delays a fresh order.

These change the simulation's rules: every player of a network game runs the same build, and a
recording made before them does not play after them.

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
   by our search one after another in batches of 8, the results applied in order; the caps; the
   gate's split; the weight by the tick's load. The engine's scheduler and search no longer run.
   **Built 2026-11-20**: 34 searches a tick against the engine's 24, a unit's wait 20 ticks on
   average against 48, for the engine's time a tick or up to two fifths more (4.3 and 6.3 ms in two
   runs against about 4.5; [the module](modules/pathfinder.md)).
3. **The workers** — the same searches on worker threads while the game thread waits.
4. **A cheaper search** — the heap and the expansion are half of a search's time.

## Open

- **The owner's to set**: the caps' two numbers, the blocked units' 60 ticks, the three weights'
  thresholds (4 and 64 units ready), and whether decision 3's first case stands as built (rule 2
  above).
- **The scratch's class.** The search's scratch and its block are declared `SIM_TRANSIENT`: written
  by each search before it reads it. That class on memory the simulation reads is the owner's
  decision.
- A goal of a kind other than the three ground kinds (point, annulus, ring) reaching a ground unit:
  none in the 300 000 searches the oracle counted and none logged by the scheduler in 500 000 more;
  not ruled out by the disassembly. It gets the result of a search that
  found nothing.
- A search past 65 536 open nodes: the engine's node word wraps there. It gets the result of a
  search that found nothing; none occurred.
- A batch's searches can pass the pops cap together; with workers a batch's time is its longest
  search's.
- The class's sweep walks every unit once for each class that searches in a tick, as the engine's.
- ImpAI's simulator still models the engine's scheduler, and the recorder still lists its three
  tables, which nothing writes now.
