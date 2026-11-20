# The pathfinder: our scheduler and our search

`tagpu_pathfind.c`, `tagpu_pathfind.h`; the search itself in the shared core, `tasim_path.c`
(`ts_pf_search`). The plan it belongs to, the owner's decisions and how each rule's number was
arrived at: [the pathfinder in parallel](../pathfinder-parallel.md). The engine's pathfinder:
[units](../engine/units.md), *The pathfinder*; its maps: [map](../engine/map.md), *The pathing maps*.

## What it is now

**Every path of a ground unit a player's machine owns is found by our scheduler and our search.**
The players phase's call of the engine's scheduler (`0x464F92`) goes to ours; the engine's scheduler
`0x40EB70`, its budget and its search `0x40E630` do not run. It runs on the thread that runs the
players phase — the game thread, and once the loader during a load, before the game thread's first
step — and a batch's searches run on that thread and up to three helper threads at once. The test lever `tagpu_pathverify.on` leaves the engine's in place and runs our
search beside it as an oracle (below).

## The scheduler — `sched_tick`

Once a tick, in three parts.

**The walk**, one pass over the unit array. Every unit of every player the engine's scheduler serves (a record in use, of type 1,
2 or 3, not in state 10), in slot order, whose local ground course wants
a search (bit 1 of its `+0x64`), is *ready* when:

| the unit | ready | served |
|---|---|---|
| has a new goal: its course's stamp `+0x60` is 0 | at once | first |
| is not blocked and has fewer than two corners left of a path its last search found | at once | first |
| any other: blocked, or its last search found nothing | 60 ticks after its last search began, the engine's own gate | after the first two |

The same pass lists **the still units** — alive, with a mover whose claim stamp is below the tick's
watermark — which every class's sweep of the tick then marks from, where the engine's `0x440AF0`
walks the whole unit array once a class.

Each kind is served from its own cursor on — the slot served last — so within a kind the caps
starve no unit, and a jam of blocked units never delays a new goal. The third kind waits while the
first two fill the caps: in the battle below that is its first 600 ticks.

**The stamp is 0** for a course that has never searched, and after set goal `0x44F2A0`: the
engine's own rule zeroes it when the course's last search began 10 ticks ago or more, and our
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
   their own patch, their own thread's scratch and their result — on the scheduler's thread and the
   helpers (below);
3. **the results**, each through the engine's own set path `0x44F080` — a path's corners, or
   none — and the search's order flags through `0x44CED0`.

No search spans ticks, so nothing of a search is state. What a tick carries to the next, beside
the units' own records: per unit slot the stamp of its last search that found a path (`s_found`),
and the two cursors (`s_cursor`); both are recorded state.

**The helpers.** Threads made at install search a batch beside the scheduler's own thread, each on
a scratch of its own. Their number is the machine's processor cores less one, left to the render
thread, and at most 4 with the scheduler's thread counted: one thread on two cores, two on three,
three on four, four from five up. It decides how long a batch takes and nothing of what it finds: a search reads
nothing another search of its batch writes, and the results are applied in the batch's order.
The batch is handed out and waited for by a handshake (`batch_search`): the scheduler's thread
writes the batch, releases one count of a semaphore for each helper it wakes, searches too, and
does not go on until the helper that answers the last count has set an event. A helper reads the
batch, a job or engine memory only between a count it has taken and its answer, so while any helper
reads, the thread that owns the engine's memory is inside that call. **Helpers are woken only in
play**, when that thread is the game thread — the load's word `main+0x38D75` is 0: the loader goes
on to its call of the players phase only after reading the game thread's bit 3
(`0x4975D6..0x4975F1`, set `0x498576`), and the game thread zeroes the word at its entry into play
(`0x49847E`) after the loader's last store. No other thread of the process writes what a search
reads. In the loader's one call of the players phase
the game thread is not stopped — it runs its loading frame, the network pump included — and the
loader's entry `0x497C70` catches a fault and ends its thread; there the searches run on the
loader's thread alone, as the engine's own did. When the scratches' memory is not to be had for
every thread, one is made and the searches run one after another.

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
65 536 nodes, a unit whose class, grid or patch is outside its bound. None occurred. A game whose
unit array is longer than the table of stamps, or for whose grid no scratch can be made, gets no
search at all, and the log says so once a level: `pathsched: NO GROUND UNIT GETS A PATH`.

**On a level that plays a recording** the scheduler and the observer do nothing (`tagpu_puppet_on`):
the recorder alone writes the state, and every seat's course is a proxy besides.

## The search — `ts_pf_search`

One unit's search, whole: the search start `0x40E630` after its watermark, the A* passes to their
end, the extraction. It takes a query — the class's map and footprint, the explored map and the
served player's bit, the searcher's cell and heading, the goal, the heuristic's weight — and a
scratch (`TS_PF`), and returns how it ended (found, no search, off the grid, the open set emptied)
with the path's corners, at most 20, and the flags the engine ORs into the goal's order. It writes
the scratch and the result and nothing else; a search leaves nothing in the scratch the next reads.

- **Where it differs from the engine's in how, not in what.** The engine's heap holds node
  pointers and every compare reads the node's `f` through one; the core's heap entry carries a copy
  of `f` beside the node (`TS_PFHEAP`), stored wherever the node's is, so a sift stays inside the
  heap. A cell's read (`0x40D7B0`) takes the class, the grids' sizes, the explored map and the patch
  from a view made once a walk or a pass of the A* (`RD`, `rd_code`), where the engine's reads them
  again for every cell. The grid's reset steps from one touched block to the next. The pops, their
  order and every result are the engine's.
- **The scratch's pool** is 65 536 nodes where the engine's grows (`ts_pf_init_n`): the engine's
  node word is 16 bits. A search that asks for more answers `TS_PF_OVERFLOW` with no result.
- **The patch.** A unit that has stood still for 30 ticks is an obstacle in its class's map. When
  the searcher is one, the engine's search start recomputes its rectangle with the searcher left
  out, and the cleanup puts it back: two writes to a map every other search reads. Here the search
  reads those entries from a private copy instead (`TS_PFPATCH`), recomputed by the engine's rule
  `0x47E1F0` over the engine's own cells with that one unit left out (`pv_refresh`, `patch_of`).
  A searcher that is no obstacle has no patch and reads the map as it stands, as the engine does.

**What a search costs** [MEASURED 2026-11-20, the 3152 searches captured from the battle below,
built natively, the best of three runs]: 53 µs a search at 3 times the base weight and 44 at 6
times, against 64 and 53 before the heap's copy and the reader's view; about 12 µs of a search does
not depend on its pops (the reset, the goal's cells, the straight walk) and a pop is 80 ns. A
sampling profile of the rest: the expansion 29 %, the heap 24 %, the cells' reads 12 %, the reset
and the straight walk 13 %: the loads of the map's and the grid's cells that are not in cache.
**The searches are uneven**: at 3 times the base weight the median pops 134 nodes, the mean 510,
one in a hundred over 5500, the largest 33 758; the top 5 % are 46 % of all pops.

## What it measured

MEASURED 2026-11-20 on a battle of 13 500 units, ten players, the reference setup, 3600 ticks, in
which about 6000 units are given a goal within a few ticks of one another.

| | the engine's scheduler | ours |
|---|---|---|
| searches a tick | 24 | 30 to 34 |
| that found a path, a tick | 20 | 25 to 28 |
| a unit's wait from ready to its search, mean | 48 ticks | 20 to 23 |
| searched in the tick they became ready | 24 % | 38 to 41 % |
| waited 30 ticks or more | 40 % | 24 to 26 % |
| the longest wait | 479 ticks | 393 to 423 |
| the pathfinder's time a tick, one thread | about 4.5 ms | 3.9 ms |
| on four threads | | 2.4 ms; 2.4 with the cheaper search, in a run of a fifth more pops |

The time is the frame profiler's: ours the span `pathfind`; the engine's the players phase of a
run on its scheduler, 7.4 ms a tick, less the 2.9 to 3.0 ms that phase takes beside ours. The
battle does not play the same twice: three runs on one thread before the walk listed the still
units took 4.3, 4.3 and 6.3 ms a tick, and the ranges above are the logged runs.

**By thread**, each a run of the battle with the phases timed:

| threads searching | the searches, ms a tick | the walk | the writes | the scheduler |
|---|---|---|---|---|
| 1 | 3.09 | 0.30 | 0.41 | 3.9 |
| 3 | 1.90 | 0.30 | 0.43 | 2.7 |
| 4 | 1.65 | 0.30 | 0.42 | 2.4 |
| 6 | 1.62 | 0.30 | 0.44 | 2.4 |
| 8 | 1.74 | 0.30 | 0.43 | 2.5 |

A batch ends with its longest search, and the searches' times are uneven, so threads past four end
it no sooner: hence the most of 4. With a sweep of the unit array for each class, the writes were
0.85 ms a tick. In a run that searched every batch on eight threads and again on one, all 105 380
results were equal.

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
1 543 197 entries all equalled what the engine had just recomputed. Again with the heap's copy and
the reader's view: 74 304 of 74 304, and 1 460 524 patch entries. Offline, the same search
equalled the engine's on 3152 searches captured with their maps, built natively, under the
sanitizers and with the DLL's compiler. A search of the engine's that spanned ticks (2 to 3 % of
them) is counted apart: the map changes under it, and about one in seven differed.

## Bounds

Every value taken from engine memory is bounded before it is used: a player's block against the
unit array, the class record against the 32 records, the grid's size against the class's and the
pathfinder's, a cell's unit slot against the unit array's end, a feature definition against the
count, the patch's rectangle against 4096 entries, a unit's slot against the table of stamps. From
the walk to the last result everything runs inside one call on the simulation's thread, and nothing
it calls makes, moves or frees a unit, a mover, a course or a goal.

## The log — `tagpu_pathlog.on`

Read at attach. Every 150 ticks two `pathsched:` lines: the searches and pops a tick against their
caps, how the searches ended, the units ready by why and those left for the next tick, the ticks at
each weight, the waits' histogram, and the set-goal calls inside the engine's 10 ticks by what the
observer did. The frame profiler (`tagpu_prof.on`) times the scheduler as the span `pathfind`.

## Gaps

- **A batch waits for its longest search**, and the searches are uneven (above), so three of the
  four threads idle through most of a long one. The batch's size is what changes that, and it is a
  rule of the simulation: the battle at batches of 8, 16 and 32 took 2.39, 1.95 and 1.65 ms a tick
  ([the plan](../pathfinder-parallel.md), *Open*).
- **Nothing may unwind past a batch**: the batch in hand names the game thread's frame, so a fault
  there that a handler caught and survived would leave the helpers reading it. No such handler is
  known above the players phase in play; the loader's, the one known, runs no helper.
- **The loader's call reads beside the game thread's loading frame**, as the engine's own players
  phase does there; what keeps the units still in that window is the engine's arrangement, not ours.
- **Cores against hardware threads was not measured**: the display's start gives the process every
  processor (`dd.c`), so a run pinned to four cores is not confined to them. The rule counts cores.
- **A batch can pass the pops cap** by its own searches: the cap is tested between batches, and a
  search that finds no way pops up to the pool's 65 536. The most pops in one tick was 507 000,
  a batch of eight such searches.
- **Nothing but the caps bounds one unit's searches at a path's end**: a unit whose found path ends
  short of its goal searches again when it reaches that end. A 5-tick floor between two such
  searches held no unit in 3300 ticks of the battle, and was dropped.
- **The weights' thresholds, 4 and 64 units ready, are a first choice**, the owner's to set; the
  caps and the retry's 60 ticks are the owner's decisions ([the plan](../pathfinder-parallel.md)).
- **The simulator's scheduler is the engine's**: `tasim_path.c`'s `ts_pf_tick` still models
  `0x40EB70` and its budget. The recorder still lists the engine scheduler's three tables
  (`replay-globals.tsv`: `pfweight`, `pfexamined`, `pfcalls`), which nothing writes now.
- All measurements are one skirmish battle on one map. No network game was played.
