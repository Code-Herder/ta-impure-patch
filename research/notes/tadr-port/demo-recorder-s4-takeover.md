# The demo recorder, S4b: taking over a recording, and its fork in the file

S4b of the [skirmish slice](demo-recorder-skirmish-slice.md#s4b-the-takeover-and-its-fork). A level
that plays a recording becomes a live skirmish at GameTime T: player P is human, every other
recorded seat is an AI, and the viewer's seat V is dropped. The game after T is recorded as a new
**timeline** of the file the level plays, and a playing of that timeline crosses from its parent to
it with a seek. The engine facts are in the engine map's
[*The takeover from a recording*](../exe-reverse-engineering.md#the-takeover-from-a-recording-seat-types-the-ais-objects-passability-and-the-order-nodes-disassembled-2026-10-02).
A multiplayer recording is taken over the same way, on its composed world, and its timeline is a
skirmish segment of the multiplayer file:
[*The takeover of a multiplayer recording*](demo-recorder-multiplayer-slice.md#the-takeover-of-a-multiplayer-recording).

## Driving it

- `tacli replay takeover <instance> [<recording>] --at T --player P [--difficulty D] [--timeline N]`.
  With a recording it launches the instance on it with P viewed, so the side's interface the level
  loads once is P's, and writes the trigger once the level plays. The level plays the instance's
  copy of the recording, in its `replays` folder, so the fork goes into that copy and the file given
  is left as it was; tacli prints the copy's path. Without a recording it takes over the instance's
  running playback. `--dump NAME --dump-at G` arms a dump of the live game at G before the
  takeover, which the recorder keeps across the takeover's seek. It waits for the takeover's line
  and the recorder's start of the new timeline, and exits 1 on a refusal.
- By hand, `takeover <P> at <T>[ difficulty <D>]` in `tagpu_replay.trigger` on a playing level.
  D defaults to the timeline's own difficulty when it has an AI seat, else 2.
- **A takeover starts at a refresh.** T is rounded down to the refresh at or before it, read from
  the file's index (`recr_refresh_at`): a segment's first and second iterations, then each GameTime
  1 modulo the refresh interval (the first iteration past one that a gap skips), the interval being
  RT_HOLD's in a skirmish recording -- 300, or the test lever `tagpu_rec.refresh`'s -- and 300 in
  a multiplayer recording, whose header has no RT_HOLD (`reclossy_is_refresh`). The trigger logs
  `a takeover asked for at GameTime T takes over at R` when they differ, the taken-over line names
  R, and tacli reports it (`at` in `--json`); the level holds at R − 1. A live rewind's fork rounds
  the same way. At a refresh the stream's world is the writer's exact copy, and between two the
  stream may hold weapon targets that owner code resolves with no alive test (R0 below).
- **Every GameTime after a timeline's first iteration can be taken over.** The fork is appended
  after R − 1, which at a timeline's first iteration is its parent's iteration, or none for the
  original (`recr_append_point` refuses both). The second iteration is a refresh so that a request
  inside the first interval rounds to it; only a request for the first iteration itself is
  refused, naming the second. The start and the append point are found before anything is
  committed (`tagpu_replay.c`, `takeover_point`): the trigger refuses before the seek, and a live
  rewind's `fork` refuses before it ends the original's recording, so a refused fork leaves the
  live game where it was.
- **The trigger's takeover is the HUD's two calls** (`inc/tagpu_replay.h`, *THE HUD'S RECORDER
  CALLS*). `tagpu_replay_takeover_plan(T, &plan)` finds R, the timeline the fork would evict and,
  when T rounds to a timeline's first iteration, `next` = R + 1, the first GameTime a takeover can
  start at, which the HUD snaps forward to (the trigger never does). It commits nothing, and the
  trigger's `takeover <P> at <T> dry-run` prints its answer: the refusal, the rounding line and
  `fork would delete ...`. `tacli replay takeover --dry-run` asks it when no recording is given;
  with one it predicts offline (`tarec.fork_prediction`). In a live rewind gone into the past the
  plan answers for the rewind's `fork`, whose reader it reads. `tagpu_replay_takeover_request(P,
  T, D)` is the trigger's takeover with the trigger's lines: refused at once by `takeover_point`,
  or queued for the frame visit.
- **The latest request runs.** A seek, the HUD's included, replaces a takeover not yet taken:
  every request writes every field `seek_run` reads at its entry (the mode, T, P and D, the dump,
  the poison, the boundary), so a HUD seek after a queued takeover is a seek to its own T.
- **Each takeover's outcome is the last request's** (`tagpu_replay_result`, kind TAKEOVER): the
  refusal's reason, or `taken over at GameTime R as player P`. Only a request records one: a
  live rewind's fork reports through the rewind's own outcome
  ([live rewind](demo-recorder-s4-rewind.md#live-rewind-protocol)). The trigger's `hud state`
  prints it, with the timelines and the rewind's state (`tagpu_replay.c`, the trigger's comment).
- `tacli replay play` and `replay seek --recording` take `--timeline N`, which the request file
  carries as `timeline N`.
- `tools/replay/tarec.py segments FILE` lists a file's timelines (so does `info`),
  `check --timeline N` and `compare --timeline N` read timeline N's chain, and `compare-fork FILE N` compares timeline N's
  first iteration with its parent's chain at its fork point (D2 below).
- `tools/replay/tareplay.py takeover FIXTURE` runs a fixture's live game (L) and its takeover
  processes K and F alone; every tier runs K and F for a fixture with a `takeover` entry:
  `battle`, `takeover-end` and `tenseat` on the default build, and `stock-limits` on the
  stock-limits build, which compiles the takeover too, so the landing tier's coverage needs a
  takeover on each. K takes over at T2, or at the entry's `at` (tenseat's: 901, the tick after its
  typed LOS command); `tareplay.py takeover --at T` overrides both. Those fixtures record with
  `tagpu_rec.refresh 30`, a refresh every 30 ticks, and every point the harness draws is 1 modulo
  30, so the refresh at or before it is the point itself; the harness reads it from L's file all
  the same (`tarec.refresh_at`).

## The takeover's steps

The takeover is the seek (`seek_run`) in its TAKEOVER mode. The seek's steps R0 to R17 run in their
order, and the takeover's are inserted (`tagpu_replay.c`, *THE TAKEOVER*):

| Step | Where | What, and the invariant it rests on |
|---|---|---|
| R0 bound | before any host write | `tk_bound`: T is a refresh iteration of the file (`recr_refresh_at`) unless the rewind returns to the saved present, so the takeover hands the engine the writer's exact copy: the target resolver `0x48A190` forms `begin + movsx(word0)·0x118` with no bound and no alive test and AutoAim's read `0x48A1E0` tests only `+0xA6`, and both run as owner code from TK4a; a held target is an index the engine produced for that unit, so what they would meet is a dead or reused slot, never an address outside the array ([engine map](../exe-reverse-engineering.md#the-demo-recorders-lossy-stream-what-a-held-byte-meets-on-a-puppet-and-at-a-takeover-disassembled-2026-10-11)); P is a seat of the recording, and its live count `+0x144` at T is above 0 (the destructor's own elimination count); D is 0, 1 or 2; every recorded seat holds no AI object and no acquisition record (a puppet seat's player init skipped both), has its group table and sight counters, and has map bit `+0x146` = its index, below 10 (the acquisition table has ten entries), on the host and in the image at T, which R8 writes over the host's; V holds no AI object; the level has an objective; the recording's declared tables are the ones this build records, on the fields a reader compares across a chain, so the fork can be played (S4's D3: a fork is always recorded); and the recorder can record a fork (`tagpu_rec_fork_ready`). A refusal writes nothing and puts the reader back |
| TK0 | the last act of R0 | `tk_open`: the file opened for writing with `FILE_SHARE_READ` and checked to be the file the reader holds (volume and file index), **then** the append point found. Nothing after TK0 refuses |
| TK1 | after R6, before R7 | `tk_seats`: viewed = P, the difficulty, each seat's type through the engine's setter (P 1, the others 2), each seat's AI object and acquisition record in the player init's order, then the AI profiles |
| R8 | the orders | every node restarts (D1), the builders adopt their frames, Paralyze and BuildWeapon resume, and p1 is bounded (`tk_node`, `tk_node_bounded`); the lists of a host unit the kills left alive and the image does not describe are torn down, and its three weapon slots' targets set to none, (0, `0x8000`), since its image was last written from the stream before T and the refresh does not reach it (`tk_orphans`); the seat types again after `players_put`, which writes PlayerInfo `+0x94` from the image |
| R10b | the cells | on a recording that holds its cells (`RECL_CELLS`), R10 writes each cell's unit slots and yard bit from the image at T, a slot only when 0 or naming a live unit, where a seek keeps the host's, and `tk_cells` writes every live unit's contest marks, `+0x110` bits 26 and 27, from it: exact at a refresh, and the engine's writers read both from TK4a (the clear's re-claim walk, the re-claim's bit-27 test); TK4a's passability refresh then reads the slots ([engine map](../exe-reverse-engineering.md), *The footprint contest's rule*) |
| R13 | the globals | `holds_put` also writes the `hold` rows (the end countdown and the game end's flags bits 2 and 4), so a takeover inside a recorded countdown continues it; the explored map is written with no V bit |
| R14 | sight | P's minimap fog only, from the counters R5 to R8b made |
| R14b | contacts | the viewed record's radar pass `0x467440` on T's world, then, when P is the recording's viewed record, `+0x110` bits 8–10 from the recording (`rw_contacts`: a slot below the array's count, live in the image, whose host slot holds a live unit of the image's type; only the three bits written). The acquisition records TK1 made start with `+0xED` 0 (`0x40918B`, `0x4092DC`), so the AIs' first scans `0x40AA40` run in iteration T's players phase and read these bits, as the recording's AIs read them until its next pass; nothing after R14b writes bits 8–10 before the capture point of T. As another player the pass's bits stand (*Residuals*) |
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

**The adoption.** A builder whose node holds its frame goes on building it (MEASURED 2026-09-29 in
the feasibility experiment): BuildingBuild and MobileBuild at state 3 with `+0x06 = 9` and the
deadline at T, as each handler arms itself (`0xB`) less bit 1. A frame that dies before the first
call fires event 8, which both handlers test before their states, so state 3 never runs without its
target: MobileBuild's state 3 has no NULL test, and BuildingBuild's answers one with 7, on which the
dispatcher destroys the factory's whole main list.

**The bounds.** The three build handlers index the unit types with the whole p1 dword, so p1 is
below the type count; BuildWeapon's p1 is one of the three stock slots and the slot's weapon is set;
a node that fails either ends its list there and is counted. Attack_Chase's p1 names a slot that
holds a weapon, or becomes 0, the handler's "unset": the engine reads the slot's weapon from p1's
low byte with no NULL test (`0x49ABCB`, `0x49AE04`), so an empty stock slot 1 or 2 counts as well
as a p1 past the unit's weapons.

**The units the kills left alive.** A host unit that survives R3 and that the image does not
describe keeps the nodes its puppet steps left, which `tk_node` never sees, and from T its owner's
type makes the unit tick run their handlers. Its lists are torn down instead, safe for the reason
the seek's own teardown is: no handler has run on a puppet's node, so none has armed the
destructor's handler call or its StopBuilding. The takeover's line counts these units.

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

**Both readers** (`tagpu_recreader.c` and `tarec.py`) refuse by name a file that breaks the
segments' bounds: at most 64, every later one with a unique `RT_TIMELINE`, each parent an earlier
segment, no header block inside a segment, and every segment of a chain the shape of the file's
first header. They walk differently: `tarec` checks every block's payload CRC and stops at the first
that fails, while the DLL's scan checks the payloads of the header, timeline-metadata and end blocks only, and a
stream block's when a reading reads it, which stops the playing there.

**The append point is safe by construction.** The scan tells three outcomes apart: a block is whole;
torn, what an interrupted append leaves (the file ends inside it, or it fails its magic or a CRC
and nothing but zeros follows); or neither, damaged (it fails and data follows) or unreadable (a
read, an allocation or an inflate of bytes that passed their CRC failed). Only a torn block is cut;
the others refuse the takeover in TK0, since a cut there would delete the complete blocks after it.
The last block's payload is checked by a CRC streamed over its packed bytes, never inflated. A
takeover at a chain segment's first iteration is refused too: the iteration before it is the
parent's, so the fork would name the parent and stand for a world that was not the one played
(`takeover_point` refuses it before the seek, naming the second iteration, the first that can be).

**The boundary while playing** (*THE BOUNDARY*). When the chain goes on in its next segment, the
playing holds: the step applies nothing, the recorder's armed dumps wait, and at the next frame
visit a seek to the child's `fork_at` makes the world from the child's first iteration, which
replaces the world whole, so the seek is exact. A frame may run several steps; the seek sets
GameTime to `fork_at` − 1 whatever the host reached, so every armed dump's GameTime comes again. A
seek or takeover the trigger puts in place of the boundary's and that is refused leaves the hold and
the boundary's seek in place, since the reader still stands at the boundary.

## The fork's handle

The fork is written into the file it was played from, through the handle TK0 opened.
[S4c's persistent path lock](demo-recorder-s4-forks.md#file-ordering-and-lifetimes)
travels with that handle and orders its mutation against compaction's replacement. **TK0 opens the
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
segment had none, and appends the new segment. The cut drops a torn last block, or an unfinished
segment with no complete iteration with its header, and never a complete iteration.

## Residuals

- **COB state** (O4, every seek's class), including the adopted builders' pose.
- **VTOL_MobileBuild's orphaned frame**, above.
- **A seat's control-group order inside its vectors** is the rebuild's, not the recording's.
- **The contact bits as another player.** When P is not the recording's viewed record the recorded
  bits are another record's scan, so R14b's pass stands, and the radar lists the fork's AIs build
  from P's picture as of T stand until their next scan at T+30, where a playing viewed as P holds
  its last pass's; D2 (d) masks bits 8–10 there. Bit 12 and `+0xB0` are the pass's in every view
  (its fourth loop, `0x4676A1..0x4676E8`, through the fresh acquisition records), and D2 (d) masks
  bit 12 always. A multiplayer recording's takeover keeps the pass's bits in every view
  ([its takeover](demo-recorder-multiplayer-slice.md#the-takeover-of-a-multiplayer-recording)).
- **The CRT `rand`** (`0x4E4870`) keeps its state per thread, in neither image
  ([the engine map](../exe-reverse-engineering.md)), so the fork's draws from it — the meteor
  shower's among them, which a puppet level gates — start from the host's state, not the recording
  machine's.
- **The deactivated V's path budget** keeps the value it had. The path tick's round robin gives
  each of the ten budgets above 0 its turn (`0x40ECB1..0x40ECD8`, DISASSEMBLED), so V's is spent
  after the takeover; nothing was found that refills an inactive record's [INFERRED], so it is spent
  once and the rest of the game is unaffected.
- **The recorder can still fail after the arm.** `rec_start` at the fork's first capture point can
  refuse (no memory for the ring or the tables, the writer's thread not made); the game then goes on
  live and unrecorded, with the recorder's `FAILED timeline … is not recorded` line, and the file
  keeps what it had.
- **An allocation failure inside `0x40B320`** is the engine's own crash, as at every level load;
  the takeover's own allocation of the AI object refuses the game by name instead.
- **The mixed-side interface on the take-over-from-watch path.** MAIN2 keeps the side viewed at the
  level's load, so a takeover as a seat of the other side from a running playback leaves the side
  panel in the old side's art while the bars follow P. `tacli replay takeover` with a recording
  launches with view = P and does not show it. The owner chose a mid-level re-skin (OQ-B), planned
  as its own landing after S4b.
- **No fixture reaches the objective's own victory test after a takeover.** `takeover-end` ends by
  `+iwin`, which sets the won and stop bits itself; `+kill 1` disarms the objective
  (`0x4164EF`), and `battle`'s game ends by `replay-end.json`. So the re-armed `+0x88` and the
  end tests in P's iteration rest on the disassembly above, not on a run. A fixture that does
  reach it needs every unit of the other seats killed by damage after T; with
  `ActiveCommanderDeath` 0, as the tests launch, a commander's death takes nothing else with it.
- **A recording whose tables another build declared** plays, but cannot be taken over (R0's table
  check): its fork would hold this build's tables, and no reader plays a chain whose tables differ.
- **The layout's hash changed.** The `order 0x006 ondestroy` row of `replay-layout.tsv` names all nine
  handler functions that arm the destructor's call, so `TAGPU_REPLAY_LAYOUT_SHA256` changed and every
  recording made before S4b refuses to play. The recorder is unpublished, so this costs only local
  test recordings.

## Verified by

**Verified and measured** (2026-10-02, the landing tier on the coverage builds; 18 fixtures, 78
processes). 414 checks passed, none failed, and 51 round trips were EQUAL (seeks 8 to 515 ms).

The takeovers:

| fixture | build | T | the line | units live 4 s after / made | the fork's segment | checks |
|---|---|---|---|---|---|---|
| `battle` | default | 1741 | 581 ms (passability 122, AI 1) | 1349 / 1357 | 44.2 MB | judge, line, orders, ai, fork |
| `takeover-end` | default | 331 | 24 ms (passability 13, AI 1) | 27 / 26 | 220 KB | judge, line, fork, victory |
| `stock-limits` | stock limits | 721 | 201 ms (passability 178, AI 1) | 10 / 8 | 476 KB | judge, line, fork |

- `judge` reads the engine's live count 4 s after the line, so it includes units the AIs made since;
  it is a floor on what the rebuild kept, not an exact match.
- `orders`: the ordered ARMCK moved 203 wu in the 300 ticks after its order.
- `ai`: in [1741, 3541] seat 1 made a unit in both timelines; seats 2 and 3 made none in the
  original either.
- `fork`: every fork is `compare-fork` EQUAL against its parent's chain at T, with the original's
  bytes unchanged; F, a fresh process, plays timeline 1 across its boundary EQUAL, the radar
  picture too.
- `victory`: timeline 1 of `takeover-end` ends complete at GameTime 537, player 0 won.

The fork's first iteration costs 4.0 to 9.5 ms, against the original's 8.4 ms on the battle.

**R14b's contact bits, verified and measured** (2026-10-11, `tareplay.py takeover` on the release
build, game speed 20, turbo 1; P = the recording's viewed record, so `compare-fork` compares bits
8–10 strictly):

| fixture | T | R14b | result |
|---|---|---|---|
| `tenseat` | 901 | restores bits 8–10 | every check passed; `compare-fork` EQUAL, 23 live units; F's f1201 EQUAL, the radar picture too |
| `tenseat` | 915 | restores bits 8–10 | every check passed; `compare-fork` EQUAL, 25 live units |
| `tenseat` | 915 | the pass's bits | `compare-fork` 21 differences in 22 live units, every one `+0x110` bit 8 |
| `tenseat` | 885 | either | `compare-fork` EQUAL: nothing the pass reads changed in 871..885 |
| `battle` | 2101 | restores bits 8–10 | judge, line (cpu 340 ms), orders, ai, fork: `compare-fork` EQUAL, 1295 live units |

`tareplay.py rewind battle` takes the live rewind's fork through the same R14b line: every check
passed, its fork `compare-fork` EQUAL at 4501 with 932 live units.

The reader's damaged-file paths were run in a scratch Wine build of `tagpu_recreader.c` on copies of
`takeover-end`'s forked file:
- **cut 3000 bytes short:** the torn block is not kept, and the append point is after timeline 1's
  first iteration (`end_open`, open 1/331);
- **the same, then 8192 zero bytes:** the same point;
- **one byte flipped in a block of timeline 0:** appended at the end, since the DLL does not check a
  mid-file block's CRC;
- **cut short, then 4096 bytes of data:** refused, "the block at byte 627949 is damaged and data
  follows it".

Seven functions no fixture reaches landed `UNCOVERED` by the owner's decision of 2026-10-02:
- the reader's damage paths `crc_stream`, `failed_block`, `zero_tail` and `seg_tally`, run in the
  scratch build above;
- `recr_timelines`, S5's listing, run there too;
- `ring_free`, after the arm when `rec_start` fails;
- `sk_refused`, a seek refused during a takeover.

One process of the run, `naval-push`'s round trips, stopped on O15, the COB guard's stop after seeks
in the naval recordings (`CORMSHIP.COB`, word 724), which has a landing of its own.
