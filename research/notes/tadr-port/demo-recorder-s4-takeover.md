# The demo recorder, S4b: taking over a recording, and its fork in the file

S4b of the [skirmish slice](demo-recorder-skirmish-slice.md#s4b-the-takeover-and-its-fork). A level
that plays a recording becomes a live skirmish at GameTime T: player P is human, every other
recorded seat is an AI, and the viewer's seat V is dropped. The game after T is recorded as a new
**timeline** of the same file, and a playing of that timeline crosses from its parent to it with a
seek. The engine facts are in the engine map's
[*The takeover from a recording*](../exe-reverse-engineering.md#the-takeover-from-a-recording-seat-types-the-ais-objects-passability-and-the-order-nodes-disassembled-2026-10-02).

## Driving it

- `tacli replay takeover <instance> [<recording>] --at T --player P [--difficulty D] [--timeline N]`.
  With a recording it launches the instance on it with P viewed, so the side's interface the level
  loads once is P's, and writes the trigger once the level plays. Without one it takes over the
  instance's running playback. It waits for the takeover's line and the recorder's start of the
  new timeline, and exits 1 on a refusal.
- By hand, `takeover <P> at <T>[ difficulty <D>]` in `tagpu_replay.trigger` on a playing level.
  D defaults to the timeline's own difficulty when it has an AI seat, else 2.
- `tacli replay play` and `replay seek --recording` take `--timeline N`, which the request file
  carries as `timeline N`.
- `tools/replay/tarec.py segments FILE` lists a file's timelines (so does `info`),
  `check --timeline N` and `compare --timeline N` read timeline N's chain, and `compare-fork FILE N` compares timeline N's
  first iteration with its parent's chain at its fork point (D2 below).
- `tools/replay/tareplay.py takeover FIXTURE` runs a fixture's live game (L) and its takeover
  processes K and F alone; every tier runs K and F for a fixture with a `takeover` entry.

## The takeover's steps

The takeover is the seek (`seek_run`) in its TAKEOVER mode. The seek's steps R0 to R17 run in their
order, and the takeover's are inserted (`tagpu_replay.c`, *THE TAKEOVER*):

| Step | Where | What, and the invariant it rests on |
|---|---|---|
| R0 bound | before any host write | `tk_bound`: P is a seat of the recording, and its live count `+0x144` at T is above 0 (the destructor's own elimination count); D is 0, 1 or 2; every recorded seat holds no AI object and no acquisition record (a puppet seat's player init skipped both), has its group table and sight counters, and has map bit = its index, below 10 (the acquisition table has ten entries); V holds no AI object; the level has an objective; and the recorder can record a fork (`tagpu_rec_fork_ready`). A refusal writes nothing and puts the reader back |
| TK0 | the last act of R0 | `tk_open`: the file opened for writing with `FILE_SHARE_READ`, **then** the append point found. Nothing after TK0 refuses |
| TK1 | after R6, before R7 | `tk_seats`: viewed = P, the difficulty, each seat's type through the engine's setter (P 1, the others 2), each seat's AI object and acquisition record in the player init's order, then the AI profiles |
| R8 | the orders | every node restarts (D1), the builders adopt their frames, Paralyze and BuildWeapon resume, and p1 is bounded (`tk_node`, `tk_node_bounded`); the seat types again after `players_put`, which writes PlayerInfo `+0x94` from the image |
| R13 | the globals | `holds_put` also writes the `hold` rows (the end countdown and the game end's flags bits 2 and 4), so a takeover inside a recorded countdown continues it; the explored map is written with no V bit |
| R14 | sight | P's minimap fog only, from the counters R5 to R8b made |
| TK4a | after R16 | `tk_live`: the passability refresh over every context whole, controlled = viewed = P, V deactivated, every unit deselected, the objective armed |
| TK4b | last | `tk_end`: the puppet table off, the playing closed, the recorder armed with the fork's handle, the line |

**The flip comes after the last kill and before the first create.** A create picks the proxy
movement class only for a remote owner, so every create after TK1 makes a local mover; the
feasibility experiment measured the order (flip first, 896 of 1,151 units moved; rebuild first, 4 of
1,176). The AI objects exist before any unit, as at a load, and before the first radar pass, which
reads the acquisition table untested. The eight RNG draws each acquisition record makes are
overwritten by R13's RNG from the image, so iteration T's RNG is the recording's.

**The objects' lifetime** is the engine's: they are its allocations, stored where it stores them,
and the teardown `0x464A00` frees them. R0 found every slot empty, so nothing is overwritten or
leaked.

**The level can end** because P is type 1 and controlled: the end tests run only in the controlled
record's iteration of the players phase. The objective's `+0x88 = 1` is the exact inverse of the
disarm the puppet level made, and the end tests run only inside a step, so none sees the half-built
world.

**After the takeover** `tagpu_puppet_end` has turned every gate off, the playing's reader is closed,
and every replay entry point returns at its test of the reader, so the level is a live skirmish
until its teardown. The options and V's PlayerInfo stay for the level's end, which puts the
player's back as after any playing.

## D1: the order nodes restart

**Decided** (2026-10-02): every rebuilt order node takes the constructor's handler state — `+0x05`,
`+0x06`, `+0x4E`, `+0x32` zero, `+0x0A` −1, `+0x42` bits 21 and 22 clear and bit 16 the image's, `+0x52`
NULL — the state every order is issued in. **Why it is safe:** no value taken from the recording is
a pointer (the target goes through the engine's setter, bounded by the host's live slot; `+0x52` is
NULL; every index a handler reads from p1 is bounded above), and with `+0x06` bit 1 clear the
destructor's unbounded handler call cannot run a handler before that handler arms it itself.

**The adoption.** A builder whose node holds its frame goes on building it: BuildingBuild at state 3
(its state 3 tests the frame for NULL; MEASURED 2026-09-29), MobileBuild at state 3 with `+0x06 = 9`
and its deadline at T, as the handler arms itself less bit 1. A frame that dies before the first
call fires event 8, which MobileBuild tests before its states, so its state 3 never runs without
its target.

**The bounds.** The three build handlers index the unit types with the whole p1 dword, so p1 is
below the type count; BuildWeapon's p1 is one of the three stock slots and the slot's weapon is set;
a node that fails either ends its list there and is counted. Attack_Chase's p1 at or past the unit's
weapon count becomes 0, the handler's "unset".

**D1's costs, and the owner's answers (OQ-A, 2026-10-02):**
- **Paralyze and BuildWeapon resume.** Their state is values — a count, progress and a deadline
  compared with GameTime only — and neither arms bit 1. A resumed deadline is clamped to the
  handler's own longest (Paralyze 0x708, BuildWeapon 300), so a paralysis lasts as recorded and a
  stockpile keeps its progress.
- **VTOL_MobileBuild restarts**, and is accepted as a residual: its state 0 ignores the old frame
  and its state 2 makes a new one, so the old frame is orphaned [INFERRED: not run]. Adopting it
  needs its hover object, which its state 4 installs only every 150 ticks.
- **The adopted builders' COB pose** is accepted as a residual: their fresh COB never received
  StartBuilding, so the arm or yard is in its create pose while the build goes on.

## D2: what the fork's first iteration may differ in

**Decided** (2026-10-02). `tarec compare-fork` compares timeline N's first iteration with its
parent's chain at the fork point less this list, and nothing else:
- (a) every order node's handler state, which D1 restarts: `+0x05`, `+0x06`, `+0x0A`, `+0x4E`,
  `+0x52`, `+0x32` and `+0x42` bits 21 and 22;
- (b) the player images of every record that is not a seat of the fork's header, V's and the
  deactivated records alike;
- (c) the controllers, the controlled and viewed players, and PlayerInfo `+0x94` of each seat whose
  controller the two headers give differently;
- (d) the contact bits by the puppet view's rule: bit 12 always, and bits 8 to 10 too unless the two
  worlds view the same player.

## The file: segments

A file is the magic and then one or more segments; a segment is an `RB_HEADER` and its blocks, and
every segment after the first begins right after an `RB_END` and carries `RT_TIMELINE` (its id, its
parent and `fork_at`). A reader that stops at the first `RB_END` reads the original as it was
written, so `REC_FORMAT` stays 4. Along a chain root..N, a segment covers its first iteration up to
the next segment's `fork_at` − 1, and the selected one to its last. A segment that ended without an
end block (the game was killed) is closed by the next timeline's writer with
`REC_END_UNFINISHED` at its last complete block.

**Both readers** (`tagpu_recreader.c` and `tarec.py`) walk the segments with the same bounds, each
a refusal by name: at most 64, every later one with a unique `RT_TIMELINE`, each parent an earlier
segment, no header block inside a segment, and every segment of a chain the shape of the file's
first header.

**The boundary while playing** (*THE BOUNDARY*). When the chain goes on in its next segment, the
playing holds: the step applies nothing, the recorder's armed dumps wait, and at the next frame
visit a seek to the child's `fork_at` makes the world from the child's first iteration, which
replaces the world whole, so the seek is exact. A frame may run several steps; the seek sets
GameTime to `fork_at` − 1 whatever the host reached, so every armed dump's GameTime comes again. A
seek or takeover the trigger puts in place of the boundary's and that is refused leaves the hold and
the boundary's seek in place, since the reader still stands at the boundary.

## The fork's handle

The fork is written into the file it was played from, through the handle TK0 opened. **TK0 opens the
handle first and finds the append point after**: while it is held no other writer can open the file
(its share mode is `FILE_SHARE_READ`), and an earlier one would have made the open fail, so the point
and the new id stay current until the writer uses them. The readers open with `FILE_SHARE_READ |
FILE_SHARE_WRITE`, so the playing's own reader does not block TK0; a block is whole or not by its
two CRCs, so a reader of a file being appended reads its complete blocks only.

**Its owners, in order** (`tagpu_recorder.c`, *THE FORK'S HANDLE*): the takeover from TK0 to the
arm; `s_fork` from `tagpu_rec_fork_arm` to the next capture point; the ring from `rec_start`; the
writer from `writer_main`. Each move is a store on one thread and then a NULL where it came from;
the one that crosses to the writer's thread is ordered by `CreateThread`. Each owner closes it on its
own failure: the takeover when the arm refuses, the level's end for an arm no capture point took,
`ring_free` before the writer starts, and the writer's fail and finish after.

**The arm's ordering.** The takeover runs at the frame visit, between two steps; the next step
raises GameTime to T (`0x4954C0`) before its unit-tick call (`0x4954ED`), where the capture point
is, so the fork's first capture is iteration T.

The writer seeks to the append point, cuts the file there, writes the parent's `RB_END` when its
segment had none, and appends the new segment.

## Residuals

- **COB state** (O4, every seek's class), including the adopted builders' pose.
- **VTOL_MobileBuild's orphaned frame**, above.
- **A seat's control-group order inside its vectors** is the rebuild's, not the recording's.
- **The CRT `rand`** (`0x4E4870`) keeps its state per thread, in neither image
  ([the engine map](../exe-reverse-engineering.md)), so the fork's draws from it — the meteor
  shower's among them, which a puppet level gates — start from the host's state, not the recording
  machine's.
- **The deactivated V's path budget** is spent once, at the deactivation, and is bounded.
- **An allocation failure inside `0x40B320`** is the engine's own crash, as at every level load;
  the takeover's own allocation of the AI object refuses the game by name instead.
- **The mixed-side interface on the take-over-from-watch path.** MAIN2 keeps the side viewed at the
  level's load, so a takeover as a seat of the other side from a running playback leaves the side
  panel in the old side's art while the bars follow P. `tacli replay takeover` with a recording
  launches with view = P and does not show it. The owner chose a mid-level re-skin (OQ-B), planned
  as its own landing after S4b.
- **The layout's hash changed.** The `order 0x006 ondestroy` row of `replay-layout.tsv` names all nine
  handler functions that arm the destructor's call, so `TAGPU_REPLAY_LAYOUT_SHA256` changed and every
  recording made before S4b refuses to play. The recorder is unpublished, so this costs only local
  test recordings.

## Verified by

<!-- S4B RUN -->
