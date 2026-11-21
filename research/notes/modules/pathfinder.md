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
step — and its searches run on up to three helper threads, beside the game. The test lever `tagpu_pathverify.on` leaves the engine's in place and runs our
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

**The tick's searches.** At most **64 units** are chosen, and **the writes** made for each: the
course's stamp; the class's watermark and the marking of the units that have just become
obstacles, once a class a tick, as the engine's `0x440AF0` does; and for a searcher that is an
obstacle, its rectangle recomputed in the map through the engine's `0x440830`, as the engine's
cleanup leaves it. **The searches** are begun in the order chosen, none once the pops of the
tick's ended searches reach **64 000**. **The results** go through the engine's own set path
`0x44F080` — a path's corners, or none — and the search's order flags through `0x44CED0`; a unit
chosen whose search did not begin gets its stamp back and the cursor does not pass it. With no
pops cap one tick of the battle popped 1 621 833 nodes.

**The searches run beside the game** where there is a helper and the memory for it. A search then
reads no engine memory: its query holds the goal, the unit's cell and heading and the patch, and
names **the copies**, made after the tick's writes — a page of the explored map, and a page of the
map of each class the tick searches. The helpers search while the game thread leaves the scheduler
and goes on.

**The join waits for nothing.** It stands at the step's call of `0x466580` (`0x4955E5`), after the
particles — the step runs from the players phase's return to it in a straight line — on the game
thread. The claiming of the tick's searches ends there, and each is one of three:

- **ended**: its result is applied, in the order chosen;
- **not begun**: the unit is put back as the walk found it, and chosen again;
- **running**: it is **carried**. Its helper goes on, its pages of the copies stay its own, and its
  result is applied at the first later join, or scheduler's call, that finds it ended. There are
  as many carried searches as helpers at most.

So which tick a long search's path arrives in depends on the machine and the moment. What the
path is does not: it is the search of the tick that chose the unit, on that tick's maps. A machine
finds the paths of its own players' units alone, so no other machine's game depends on it.

**A set goal on a course with a search due meets it first**, at the function's entry
(`goal_touch`): a search not begun is taken back and the unit put as the walk found it; one that
has ended has its result applied there, as when searches ended inside the scheduler; one that is
running is marked stale, and its path is never applied — if it found one, the unit was on its way
and is served for its new goal at once (the stamp goes to 0), and if it found none the engine's
own 10-tick rule stands. Every free of a goal a course names — the order's destructor `0x43A1F0`,
the four goal installers — enters set goal first ([units](../engine/units.md)), so a result is
never applied to a goal it was not made for.

Where there is no helper, or no memory for the copies, the searches are made inside the scheduler
from the engine's own maps and applied at the same join. In the loader's call the join is the
scheduler's own last step.

**The copies** are one block: as many explored pages as threads, then class pages, made at the
first tick that searches beside the game and grown to the most classes a tick has searched and as
many more as there are helpers, 32 MiB at most (3.6 MB of class pages and 1.5 of explored ones in
the battle). A job pins the two pages it reads until the scheduler's thread ends it; a page is
written only while no job pins it, and the block grows only while no search is carried. A tick
that finds no page while one is — the block wants to grow — puts its units back.

**What a tick carries to the next**, beside the units' own records: per unit slot the stamp of its
last search that found a path (`s_found`), the two cursors (`s_cursor`), and **the searches owed**
(`s_owed`): for each carried search its unit's slot, the search's stamp and the stamp before it.
All three are recorded state. A carried search itself is not: a game restored from a recording has
no thread behind an owed entry, and the scheduler gives that unit its stamp back, so it is chosen
again. A game put back to an earlier tick drops its searches whole.

**The helpers.** Threads made at install work beside the scheduler's own thread, each search on a
scratch of its thread's own. Their number is the machine's processor cores less one, left to the
render thread, and at most 4 with the scheduler's thread counted: one thread on two cores, two on
three, three on four, four from five up. It decides how long the searches take and nothing of what
they find: a search reads nothing another writes. It decides which searches of a long tick begin
in it and which tick a long one's result arrives in.

A tick hands tasks out twice. **The first**, inside the scheduler (`run_send`, `run_wait`): the
jobs' queries and patches and the tick's copies, which read engine memory — the scheduler's thread
stays inside `run_wait` while any helper is in that handing-out, so while a helper reads, the
thread that owns the engine's memory is in that call. **The second**, the searches, the game
thread does not wait for: its tasks read their job, the pages the job pins and their scratch, all
ours. One word a helper carries the ordering: the scheduler's thread alone gives a helper a
handing-out, by exchanging the helper's empty word for it, and the helper alone empties the word,
as the last thing it does. A handing-out's record is rewritten only where no helper's word names
it; a job and its pages are given back only after the scheduler's thread has read the helper's
last write to the job; the scratches are freed only after every word is empty.

**Helpers are woken only in play**, when the scheduler's thread is the game thread — the load's
word `main+0x38D75` is 0: the loader goes on to its call of the players phase only after reading
the game thread's bit 3 (`0x4975D6..0x4975F1`, set `0x498576`), and the game thread zeroes the word
at its entry into play (`0x49847E`) after the loader's last store. In the loader's one call of the
players phase the game thread is not stopped — it runs its loading frame, the network pump
included — and the loader's entry `0x497C70` catches a fault and ends its thread; there everything
runs on the loader's thread alone, as the engine's own searches did. When the scratches' memory is
not to be had for every thread, one is made and the searches run one after another.

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
built natively, the best of three runs]: 53 µs a search at 3 times the base weight, 44 at 6
times and 243 at the base; about 12 µs of a search does
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
| on four threads, batches of 8 | | 2.4 ms |
| on four threads, groups of 16, the scheduler waiting for each | | 1.95 ms, one run, 19 000 pops a tick |
| the same, the searches made while the tick goes on, the join waiting | | 0.87 to 0.92 ms, three runs, 23 000 to 25 000 pops a tick |
| the join waiting for nothing, a search not ended carried | | **0.95 ms**, one run, 20 000 pops a tick |

The time is the frame profiler's: ours the span `pathfind`; the engine's the players phase of a
run on its scheduler, 7.4 ms a tick, less the 2.9 to 3.0 ms that phase takes beside ours. The
battle does not play the same twice: three runs on one thread before the walk listed the still
units took 4.3, 4.3 and 6.3 ms a tick, and the ranges above are the logged runs.

**The join waiting** [three runs of about 3650 ticks]: the game thread's 0.87, 0.92 and 0.88 ms a
tick are the walk, the writes, the queries and copies; the join's wait for the searches was 68.8
and 37.5 ms in all over 3750 ticks in the two runs that timed it, a tenth of a millisecond or more
in 30 and 37 ticks, 3.9 ms at most in one run and **34.5 ms** in another, in a tick of 451 695 pops.

**The join waiting for nothing** [three runs]: 0.95 ms a tick on the game thread, the same work;
of 116 000 searches in 3653 ticks **10 were carried**, 3 at once at most, each ended by the next
scheduler's call; a set goal overtook 5 not begun; the most pops in a tick 151 616. With every
search also made inside the scheduler on the engine's own maps: 124 335 of 124 335 results equal,
the 13 carried among them. With one search in five held 30 to 480 ms after it ended, so that
searches were carried for ticks: 4425 carried for 12 928 ticks in all, a set goal overtook 75
running and 1053 not begun, 19 ticks were put back for want of a page, and all 19 361 results
applied were equal, the 4423 carried among them.

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
1 543 197 entries all equalled what the engine had just recomputed; in a second game 74 304 of
74 304, and 1 460 524 patch entries. Offline, the same search
equalled the engine's on 3152 searches captured with their maps, built natively, under the
sanitizers and with the DLL's compiler. A search of the engine's that spanned ticks (2 to 3 % of
them) is counted apart: the map changes under it, and about one in seven differed.

## Bounds

Every value taken from engine memory is bounded before it is used: a player's block against the
unit array, the class record against the 32 records, the grid's size against the class's and the
pathfinder's, a cell's unit slot against the unit array's end, a feature definition against the
count, the patch's rectangle against 4096 entries, a unit's slot against the table of stamps. From the walk to the searches' handing-out everything runs inside one call on the simulation's
thread, and nothing it calls makes, moves or frees a unit, a mover, a course or a goal. A result
is applied only where its slot still holds the unit, mover, class and ground course the search was
made for, the course's stamp is the search's, and no set goal has come to the course since the
search began (`job_stands`): a course made since has the stamp 0 its constructor writes, nothing
but the scheduler writes another, a later tick's, and every engine path that changes or frees a
course's goal enters set goal first ([units](../engine/units.md), *the ground class object*),
where the course's search is met (above).

## The log — `tagpu_pathlog.on`

Read at attach. Every 150 ticks two `pathsched:` lines: the searches and pops a tick against their
caps, how the searches ended, the units ready by why and those left for the next tick, the ticks at
each weight, the ticks searched beside the tick, the searches carried past their tick with the most
at once and the ticks they were carried, the searches a set goal overtook running and not begun,
the results not applied, the ticks put back for want of a page, the waits' histogram, and the set-goal calls inside the engine's 10 ticks by what the
observer did. The frame profiler (`tagpu_prof.on`) times the scheduler as the span `pathfind`.

## Gaps

- **The times by thread and the waits' table are from batches of 8**, the scheduler waiting for
  each.
- **Nothing may unwind past the scheduler's first handing-out**: it names the game thread's frame,
  so a fault there that a handler caught and survived would leave the helpers reading it. No such
  handler is known above the players phase in play; the loader's, the one known, runs no helper.
- **What reads a path between the scheduler and the join is ruled out for direct calls only.** The
  closure of the engine's direct calls over that window (723 functions) reaches no read or write
  of a ground course's path, stamp or flags and no test of an order's search flags; it reaches the
  order's destructor and, through handlers, the goal installers, which call set goal first, where
  the join stands. Its 58 indirect calls into game code (the stock AI's planner, the order
  handlers' event 2, the script host, the GUI pump) are not followed
  ([sim](../engine/sim.md), *The game step*). ImpAI's turns run in that window and read no course.
- **Which tick a long search's path arrives in is not the same from run to run.** A test that
  expects a long path in an exact tick does not hold under load.
- **A carried search keeps a helper**, so a tick that begins while three are carried searches
  nothing; and a tick that wants a page the block has not while one is carried is put back whole.
  Neither was seen in the battle but under the held searches.
- **A stale search runs to its end**: nothing stops a search a set goal has overtaken.
- **The class sweeps of a tick are made for every unit chosen**, the ones whose search does not
  begin included: such a class's units that have just become obstacles are marked a tick or more
  before the class's next search would have marked them.
- **The loader's call reads beside the game thread's loading frame**, as the engine's own players
  phase does there; what keeps the units still in that window is the engine's arrangement, not ours.
- **Cores against hardware threads was not measured**: the display's start gives the process every
  processor (`dd.c`), so a run pinned to four cores is not confined to them. The rule counts cores.
- **The searches in flight can pass the pops cap**: it is tested where a search is begun, against
  the ended ones, and a search that finds no way pops every cell its unit can reach.
- **Nothing but the caps bounds one unit's searches at a path's end**: a unit whose found path ends
  short of its goal searches again when it reaches that end. A 5-tick floor between two such
  searches held no unit in 3300 ticks of the battle, and was dropped.
- **The weights' thresholds, 4 and 64 units ready, are a first choice**, the owner's to set; the
  caps and the retry's 60 ticks are the owner's decisions ([the plan](../pathfinder-parallel.md)).
- **The simulator's scheduler is the engine's**: `tasim_path.c`'s `ts_pf_tick` still models
  `0x40EB70` and its budget. The recorder still lists the engine scheduler's three tables
  (`replay-globals.tsv`: `pfweight`, `pfexamined`, `pfcalls`), which nothing writes now.
- All measurements are one skirmish battle on one map. No network game was played.
