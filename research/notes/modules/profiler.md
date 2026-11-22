# The frame profiler

`tagpu_prof.c`, `tagpu_prof.h`; its marks in `tagpu_packet_pub.c`, `tagpu_gui_hook.c`,
`tagpu_markown.c`, `tagpu_recorder.c`, `tagpu_replay.c`, `render_vk.c` and `tagpu_vk.c`; the
readers `tools/profsum.py` and `tools/profsample.py`. A measuring instrument: off unless its lever
is present, and nothing in a player's game depends on it.

## What it answers

Where the wall time of a frame goes, on the game thread and on the render thread, in two views
that are kept apart because they cost differently.

- **The timers** cut every frame into **spans** by marks placed only where the code passes a few
  times a frame: a phase of the tick, a section of the draw, a stage of the render loop. A mark
  reads the clock, charges the time since the last mark to the span that was open and opens the
  span it names, so every instant is in exactly one span and a row's spans sum to its total
  (MEASURED 2026-11-20: within 14 µs a frame). A mark costs one clock read; the module's own row
  write was 0.005 ms a frame.
- **The sampler** answers what the timers must not: the inside of a span whose work is many short
  calls. A mark inside a per-unit loop would cost more than what it measures, so a helper thread
  instead notes where the game thread stands about 880 times a second.

## The levers and the files

| lever | read | what it does |
|---|---|---|
| `tagpu_prof.on` | at attach for the game thread's phase marks; polled every 500 ms by each thread at its frame boundary, in every game (a file probe twice a second a thread is what the profiler costs when it is off, with a test and a load at each mark) | rows to `tagpu_prof_game.csv` and `tagpu_prof_render.csv` in the log directory, and a summary line in the log every 5 s, when the rows are also written out. Removing it closes the files; putting it back **overwrites them**, so copy a window's files before starting the next |
| `tagpu_prof_sample.on` | looked for every 500 ms by the game thread until the sampler starts, then polled every 250 ms by the sampler, with the lever above | samples to `tagpu_prof_samples.bin`, written out at each poll and overwritten like the rows |

A row is `frame, start_ms, total_us, count, tag`, then microseconds per span; `count` is the ticks
the frame ran and `tag` the game thread's GameTime at the frame's start.

## The game thread's spans

The engine already stamps its own phases: its frame function, its step and its draw read
GetTickCount after each phase through one thunk, `0x4B6560`. With the lever present at attach the
thunk jumps to `prof_stamp`, which returns the import's own answer and marks the phase its return
address names ([sim](../engine/sim.md), *The engine's phase stamps*). So the tick's phases are
timed with no hook of our own inside the step: `net`, `units`, `projectiles`, `explosions`,
`players`, `features`, `particles`, `tick_tail`, and the draw's `terrain`, `bars`, `fog`, `hud`.
Our own marks add what the engine does not stamp: `tick_ours` (the recorder's capture and a
replay's step around the unit tick), `pathfind` (our pathfinder's scheduler, entered and left
inside the players phase by `tagpu_pathfind.c`), `sort`, `sweep` and `markers` inside the draw, `before_draw`,
`ui_capture`, `flip`, `publish_world` and `publish` (the frame packet), and `outside` (the rest of
the main loop).

One thread owns a lane: the frame mark names its owner and every other mark from another thread is
dropped, so a lane's accumulators have one writer.

## The render thread's spans

`loop`, `gather`, `fence`, `acquire`, `prepare`, `unit_upload`, `unit_prepare`, `record`, `submit`,
`present`: marks in `render_vk.c` and `tagpu_vk.c`.

## The sampler

A thread suspends the game thread, reads its context, the span that is open and the values on its
stack that lie in the exe's code section (the 8 innermost and the 52 outermost), and resumes it.
The invariants it rests on:

- **It takes no lock between the suspend and the resume**, and calls nothing that could: the
  suspended thread may hold any lock, the heap's included. The record is filled from a buffer of
  its own.
- **The stack it reads is bounded by the thread's own bounds**, its `StackLimit` and `StackBase`
  read from its TIB, with the stack pointer tested for alignment and for lying between them. A
  sample whose stack pointer fails the test, or lies more than 256 KB under the base, is kept
  with no stack and flagged.
- **The span it reads is one word** the lane's owner writes; a torn read is impossible, a stale
  one charges one sample to the neighbouring span.

`tools/profsample.py` keeps the stack values that are return addresses in the pristine exe and
says, for a span, which call of the function that runs it the thread was inside (its **stage**) and
which function the instruction pointer was in. Where Impure has taken one of that function's call
sites with a jump, no return into the function is on the stack, and the stage is the first engine
function under the step. With `--dll` and a DLL linked with symbols it names our own functions. A
share is a count of samples: about one point at 2 % of 10 000.

MEASURED 2026-11-20: 36 243 samples in 41.3 s, none with the stack unread; the timed spans of the
windows before, during and after sampling fell steadily with the unit count, with no step at the
sampler's start or stop.

## Measuring a change

Two runs of one scenario are two different battles, and a second run on a loaded machine can be
several times slower throughout. A change to a phase is measured **inside one game**: a test-only
switch runs the old path on every other 64 ticks, and the rows are compared by their GameTime tag,
each old block against the mean of its two neighbours. The switch is removed before the change is
committed. The particle walker was measured this way (22 of 22 blocks).

## What it found, 2026-11-20

A scripted battle of 13 500 units falling to 7 500 (Total Mayhem, the default view, a private
display): the step ran 45 ms a tick against its 33 ms slot, so the scheduler ran five ticks a frame
and the game 19 ticks a second.

| span | ms a tick | what the samples showed |
|---|---|---|
| the unit tick | 21 | scripts 40 %, movement 25 %, an idle unit's target search 12 %, aiming 10 % |
| the players phase | 17 | the weapon target scan 52 %, the pathfinder 38 % |
| particles | 5 | the walker's shift a third, the first read of each object a third |

The target search in both spans was one function, the candidates in range `0x40AD80`, and the
particle shift is the walker `0x471EB0`'s; both are replaced ([orders](../engine/orders.md),
[engine patches](engine-patches.md)). So are two math paths the samples found in the unit tick: the
engine's piece rotation `0x4B7173` with its `fsincos`, and the CRT's `_hypot` `0x4FB440` with its
control-word switches, each by a path whose every result is the engine's on the same machine. After the first, in a second run of the same battle whose
window reached further into it: the unit tick 15 ms, the players phase 9.4, the step 31, 26 ticks
a second.

**Not covered.**
- **A frame is whatever lies between two frame marks.** The game thread's row that spans a menu
  or a level load holds all of it, and `profsum.py` drops only a file's first row: cut the window
  with the lever, inside play.
- **The script entry check's cost is mostly the engine's.** The samples in `cob_run_enter` sit two
  thirds on its first read of the thread record, which the engine reads four instructions later
  (`0x4B0DB9`); what is ours there is about 2 ms a tick, and under 1 ms of it could go without
  weakening the check.
- **The pathfinder's 6 ms a tick** is a fixed budget's worth of node work, spread over the
  expansion, the heap and the map reads, with no single hot spot.
- **No run with both fixes on a quiet machine**, and the recorder's cost was not measured: it had
  stopped itself in these runs.
- **The stage names are the engine map's.** A function with no entry there is printed by address.
