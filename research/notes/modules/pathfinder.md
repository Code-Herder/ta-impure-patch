# The pathfinder: our scheduler and our search

`tagpu_pathfind.c`, `tagpu_pathfind.h`; the search itself in the shared core, `tasim_path.c`
(`ts_pf_search`). The plan it belongs to, the owner's decisions and how each rule's number was
arrived at: [the pathfinder in parallel](../pathfinder-parallel.md). The engine's pathfinder:
[units](../engine/units.md), *The pathfinder*; its maps: [map](../engine/map.md), *The pathing maps*.

## What it is now

**Every path of a ground unit a player's machine owns is found by our scheduler and our search.**
The players phase's call of the engine's scheduler (`0x464F92`) goes to ours; the engine's scheduler
`0x40EB70`, its budget and its search `0x40E630` do not run. It runs on the game thread, one search
after another. The test lever `tagpu_pathverify.on` leaves the engine's in place and runs our
search beside it as an oracle (below).

## The scheduler — `sched_tick`

Once a tick, in three parts.

**The walk.** Every unit of every player the engine's scheduler serves (a record in use, of type 1,
2 or 3, not in state 10), in the players' and their blocks' order, whose local ground course wants
a search (bit 1 of its `+0x64`), is *ready* when:

| the unit | ready | served |
|---|---|---|
| has a new goal: its course's stamp `+0x60` is 0 | at once | first |
| is not blocked and has fewer than two corners left of a path its last search found | at once | first |
| any other: blocked, or its last search found nothing | 60 ticks after its last search began, the engine's own gate | after the first two |

Each kind is served from its own cursor on — the slot served last — so the caps starve no unit, and
a jam of blocked units never delays a new goal.

**The stamp is 0** for a course that has never searched, and after set goal `0x44F2A0`: the
engine's own rule zeroes it when the course's last search began more than 10 ticks ago, and our
observer at the function's entry zeroes it inside those 10 ticks when that search found a path — a
unit on its way that is sent elsewhere. A unit whose last search found nothing keeps the engine's
rule.

**The batches**, 8 searches each, while fewer than **64 searches** have begun this tick and their
pops are below **64 000**. Both bounds count work and never time, and the batch is a constant: a
tick begins the same searches on every machine. A batch is three steps, each over all its searches
before the next:

1. **the writes**: the course's stamp; the class's watermark and the marking of the units that
   have just become obstacles, once a class a tick, as the engine's `0x440AF0` does; and for a
   searcher that is an obstacle, its rectangle recomputed in the map through the engine's
   `0x440830`, as the engine's cleanup leaves it;
2. **the searches**, which read the class's map, the cells and the units' stamps and write only
   their own patch, the scratch and their result;
3. **the results**, each through the engine's own set path `0x44F080` — a path's corners, or
   none — and the search's order flags through `0x44CED0`.

No search spans ticks, so nothing of a search is state. What a tick carries to the next, beside
the units' own records: per unit slot the stamp of its last search that found a path (`s_found`),
and the two cursors (`s_cursor`); both are recorded state.

**The heuristic's weight** is one of the engine's three, by the tick's load where the engine goes
by how slowly its budget serves a player:

| units ready in the tick | weight | a search pops | its path |
|---|---|---|---|
| at most 4 | the base, 1.5 | 2900 | the best |
| at most 64: all can begin this tick | 4.5 | 510 | 3.5 % longer; 39 % more of the engine's cost |
| more | 9.0 | 410 | 0.7 % longer; 71 % more of the engine's cost |

MEASURED 2026-11-20: the 3152 searches captured from the battle below, each run at the three
weights; the length over the 1365 paths of under 20 corners, the engine's cost — length and its
penalties for cells beside an obstacle or on a slope — over the 2672 found. In that capture the
engine itself searched 54 % at the base, 23 % at 4.5 and 23 % at 9.0.

**What gets no search** gets the result of a search that found nothing, through the same set path,
and is counted: a goal of a kind other than the three ground kinds, a search past the scratch's
65 536 nodes, a unit whose class, grid or patch is outside its bound. None occurred.

**On a puppet** every unit's course is a proxy, so the walk finds nothing and the recorder alone
writes the state.

## The search — `ts_pf_search`

One unit's search, whole: the search start `0x40E630` after its watermark, the A* passes to their
end, the extraction. It takes a query — the class's map and footprint, the explored map and the
served player's bit, the searcher's cell and heading, the goal, the heuristic's weight — and a
scratch (`TS_PF`), and returns how it ended (found, no search, off the grid, the open set emptied)
with the path's corners, at most 20, and the flags the engine ORs into the goal's order. It writes
the scratch and the result and nothing else; a search leaves nothing in the scratch the next reads.

- **The scratch's pool** is 65 536 nodes where the engine's grows (`ts_pf_init_n`): the engine's
  node word is 16 bits. A search that asks for more answers `TS_PF_OVERFLOW` with no result.
- **The patch.** A unit that has stood still for 30 ticks is an obstacle in its class's map. When
  the searcher is one, the engine's search start recomputes its rectangle with the searcher left
  out, and the cleanup puts it back: two writes to a map every other search reads. Here the search
  reads those entries from a private copy instead (`TS_PFPATCH`), recomputed by the engine's rule
  `0x47E1F0` over the engine's own cells with that one unit left out (`pv_refresh`, `patch_of`).
  A searcher that is no obstacle has no patch and reads the map as it stands, as the engine does.

## What it measured

MEASURED 2026-11-20 on a battle of 13 500 units, ten players, the reference setup, 3600 ticks, in
which about 6000 units are given a goal within a few ticks of one another.

| | the engine's scheduler | ours |
|---|---|---|
| searches a tick | 24 | 34 |
| that found a path, a tick | 20 | 28 |
| a unit's wait from ready to its search, mean | 48 ticks | 20 |
| searched in the tick they became ready | 24 % | 41 % |
| waited 30 ticks or more | 40 % | 24 % |
| the longest wait | 479 ticks | 393 |
| the pathfinder's time a tick | about 4.5 ms | 4.3 and 6.3 ms, two runs |

The time is the frame profiler's: ours the span `pathfind`; the engine's the players phase of a
run on its scheduler, 7.4 ms a tick, less the 2.9 to 3.0 ms that phase takes beside ours. The
battle does not play the same twice, and the two runs of ours are that spread.

The wait is the opening's: for its first 600 ticks 1000 to 3800 units a tick are ready on a new
goal against a cap of 64, and a unit waits 30 to 76 ticks on average; over the next 300 ticks 16 to
18; from then on under 12, and under 9 in the last third of the run. The caps bound that opening: 62 000 pops a tick, the cap,
for its first 150 ticks.

The 60-tick retry held 500 to 1700 units a tick. Their searches are the ones that find nothing:
18 800 of 121 000 ended with no search, the start in the goal or the straight walk no nearer.

## The oracle — `tagpu_pathverify.on`

Read at attach. The engine's scheduler stays in place and ours is not installed. Four entry
observers, byte-matched and all or none, on the game thread inside the players phase:

| site | what runs there |
|---|---|
| the grid reset `0x40D900`, returning into the search start (`0x40E690`) | the engine's watermark has run and its search has read nothing: the patch is computed and compared with the map entry for entry, and our search runs on its own scratch with the engine's weight |
| the extraction `0x40E050` | the engine's search found a path |
| the class's cleanup `0x440BE0` | the engine's search has ended and set its path or none: the exit, the path's points and count, the goal cell, the pool's use, the bug walk's answer and the order's flags are compared with ours |
| the scheduler `0x40EB70` | once a tick, the census of the units that want a search; every 150 ticks the log's three `pathverify:` lines |

MEASURED 2026-11-20, the same battle: of 77 133 searches in one game, the 75 249 that ended in the
tick they began all gave the engine's result, field for field; 33 540 of them read a patch, whose
1 543 197 entries all equalled what the engine had just recomputed. Offline, the same search
equalled the engine's on 3152 searches captured with their maps, built natively, under the
sanitizers and with the DLL's compiler. A search of the engine's that spanned ticks (2 to 3 % of
them) is counted apart: the map changes under it, and about one in seven differed.

## Bounds

Every value taken from engine memory is bounded before it is used: a player's block against the
unit array, the class record against the 32 records, the grid's size against the class's and the
pathfinder's, a cell's unit slot against the unit array's end, a feature definition against the
count, the patch's rectangle against 4096 entries, a unit's slot against the table of stamps. From
the walk to the last result everything runs inside one call on the game thread, and nothing it
calls makes, moves or frees a unit, a mover, a course or a goal.

## The log — `tagpu_pathlog.on`

Read at attach. Every 150 ticks two `pathsched:` lines: the searches and pops a tick against their
caps, how the searches ended, the units ready by why and those left for the next tick, the ticks at
each weight, the waits' histogram, and the set-goal calls inside the engine's 10 ticks by what the
observer did. The frame profiler (`tagpu_prof.on`) times the scheduler as the span `pathfind`.

## Gaps

- **The time a tick is not below the engine's**: it buys 40 % more searches, not less work. The
  searches run one after another on the game thread; the workers are the plan's next landing.
- **A batch can pass the pops cap** by its own searches: the cap is tested between batches. The
  most pops in one tick was 240 000.
- **The retry's 60 ticks and the caps' numbers are the engine's and a first choice**, the owner's to
  set ([the plan](../pathfinder-parallel.md), *Open*).
- **The simulator's scheduler is the engine's**: `tasim_path.c`'s `ts_pf_tick` still models
  `0x40EB70` and its budget. The recorder still lists the engine scheduler's three tables
  (`replay-globals.tsv`: `pfweight`, `pfexamined`, `pfcalls`), which nothing writes now.
- All measurements are one skirmish battle on one map. No network game was played.
