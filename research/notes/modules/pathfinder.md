# The pathfinder: our search and its oracle

`tagpu_pathfind.c`, `tagpu_pathfind.h`; the search itself in the shared core, `tasim_path.c`
(`ts_pf_search`). The plan it is the first landing of, and the owner's decisions:
[the pathfinder in parallel](../pathfinder-parallel.md). The engine's pathfinder:
[units](../engine/units.md), *The pathfinder*; its maps: [map](../engine/map.md), *The pathing maps*.

## What it is now

A search of our own that gives the engine's result, and an oracle that holds it to the engine's on
every search of a game. **Nothing in a player's game runs it**: the engine's scheduler and search
decide every path, and without the lever `tagpu_pathverify.on` nothing is installed.

## The search — `ts_pf_search`

One unit's search, whole: the search start `0x40E630` after its watermark, the A* passes to their
end, the extraction. It takes a query — the class's map and footprint, the explored map and the
served player's bit, the searcher's cell and heading, the goal, the heuristic's weight — and a
scratch (`TS_PF`), and returns how it ended (found, no search, off the grid, the open set emptied)
with the path's corners, at most 20, and the flags the engine ORs into the goal's order. It writes
the scratch and the result and nothing else; a search leaves nothing in the scratch the next reads.

- **The scratch's pool** is 65 536 nodes where the engine's grows (`ts_pf_init_n`): the engine's
  node word is 16 bits, and the largest search measured used 2362. A search that asks for more
  answers `TS_PF_OVERFLOW` with no result.
- **The patch.** A unit that has stood still for 30 ticks is an obstacle in its class's map. When
  the searcher is one, the engine's search start recomputes its rectangle with the searcher left
  out, and the cleanup puts it back: two writes to a map every other search reads. Here the search
  reads those entries from a private copy instead (`TS_PFPATCH`), recomputed by the engine's rule
  `0x47E1F0` over the engine's own cells with that one unit left out (`pv_refresh`, `patch_of`).
  A searcher that is no obstacle has no patch and reads the map as it stands, as the engine does.

## The oracle — `tagpu_pathverify.on`

Read at attach. Four entry observers, byte-matched and all or none, on the game thread inside the
players phase:

| site | what runs there |
|---|---|
| the grid reset `0x40D900`, returning into the search start (`0x40E690`) | the engine's watermark has run and its search has read nothing: the patch is computed and compared with the map entry for entry, and our search runs on its own scratch |
| the extraction `0x40E050` | the engine's search found a path |
| the class's cleanup `0x440BE0` | the engine's search has ended and set its path or none: the exit, the path's points and count, the goal cell, the pool's use, the bug walk's answer and the order's flags are compared with ours |
| the scheduler `0x40EB70` | once a tick, the census of the units that want a search; every 150 ticks the log's three `pathverify:` lines |

Every value taken from engine memory is bounded before it is used: the class record against the 32
records, the grid's size against the class's and the pathfinder's, a cell's unit slot against the
unit array's end, a feature definition against the count, the patch's rectangle against 4096
entries. It writes no engine memory.

## What it measured

MEASURED 2026-11-20 on a battle of 13 500 units, ten players, the reference setup, about 3500 ticks
a run.

- **Equal to the engine.** Of 77 133 searches in one game, the 75 249 that ended in the tick they began all gave
  the engine's result, field for field; 33 540 of them read a patch, whose 1 543 197 entries all
  equalled what the engine had just recomputed. None overflowed the pool, none was refused, none had
  a goal of another kind. Offline, the same search equalled the engine's on 3152 searches captured
  with their maps from an earlier game, built natively, under the sanitizers and with the DLL's
  compiler.
- **A search that spanned ticks** (2 to 3 % of them) is counted apart: the engine's goes on reading
  a map that units change under it, ours ran on the map of the tick it began in. About one in seven
  of those differed, nearly all in the pool's use alone with the same path.
- **The patch must be made only where the engine recomputes.** Recomputing every searcher's
  rectangle afresh disagreed with the map in 3 to 5 entries of 2.9 million, and one search in 70 000
  then failed where the engine's found a path: a unit standing in a plant's yard
  ([map](../engine/map.md), the map is a cache).
- **The budget is what a unit waits on.** At the scheduler's entry 1180 units a tick on average
  (7125 at most) wanted a search with the gate open, and about 23 searches began a tick: from the
  gate opening to its search's start a unit waited 48 ticks on average and 479 at most; 24 % did
  not wait, 40 % waited 30 ticks or more.
- **The gate** held 676 units a tick: 434 blocked, 8 out of waypoints, 234 on a new order.

## Gaps

- The oracle compares searches of the three ground goal kinds; a goal of another kind is counted
  and not compared. None occurred.
- A search past 65 536 open nodes is not reproduced: the engine's node word wraps there.
- Nothing here runs in a network game differently from a skirmish, and no run of one was made.
