# Live rewind (S4d)

The live rewind implementation is under verification. Its two prerequisite engine experiments passed
on the reference setup, 2026-10-02, using a normal limits build based on
`f0748b8c`, the battle fixture, and an isolated Wine/Xvfb instance. Temporary
instrumentation ran in the existing game-thread frame visit before `0x4969CD`;
it added no detour and was removed after the experiments.

## P1: a spectator seated during play

At GameTime 1522, paused, the lowest unused seat was 4. Its controller, live
count, AI object and acquisition pointer were zero. The probe called
`0x464290(4, 3)`, then `0x464700(rec)` only because `rec+0x7C` was NULL. It set
the watcher flag and the controlled player's side, made seat 4 controlled, and
kept seat 0 viewed. It did not write the pacing stamp `rec+0x18`.

The sight, group and statistics buffers (`+0x7C`, `+0x78`, `+0xEC`) were all
non-NULL; both AI objects remained NULL. After restoring control/view to 0 and
deactivating through `0x464290(4, 0)`, a second activation reused all three
buffer addresses. The two paused open/status intervals completed 225 and 199
frame visits respectively, without advancing GameTime.

Unpaused with the spectator controlled, GameTime advanced from 1522 to 1840
in 10.454 seconds: 318 ticks, approximately 30 ticks/s. Deactivation followed
by the fixture's normal defeat reached the results screen and main menu. A
second skirmish loaded in the same process, contained an `armcom`, and advanced
beyond GameTime 1000. Control was seat 0 and seat 4's controller byte was zero.
Both recordings closed with zero corrections.

This observes one defeat/main-menu/new-skirmish route. It does not establish
the value of seat 4's active DWORD in the next level or close the general F1
seat-reset question. The helper initially checked uppercase `ARMCOM`, whereas
the roster returns lowercase; a separate roster and memory read established
the successful second load.

## P2: destroy local units before freeing their AI

A fresh live battle paused at GameTime 1523. Before the kills, seats 0–3 had
controller types 1/2/2/2, live counts 343/334/356/337, and non-NULL AI and
acquisition objects. The probe used the existing seek bounds and rebuild gate,
emptied projectiles, and ran the seek's three destructor passes while those
seat types and objects remained intact.

All **1370 units were killed; zero remained**. Only then did each recorded seat
become type 3 through `0x463C60`, followed by the engine teardown sequence:
`0x40B390(rec+0x146)`, `0x408F10(rec+0x74)`, `0x4B4F20(rec+0x74)`, and clearing
`rec+0x74`. All four seats then had zero live count and NULL AI/acquisition
objects. The map-bit index was checked against the seat before conversion.

The ordinary seek stages rebuilt iteration 601 from the first experiment's
recording: **1416 units made, zero refused**, 387 ms total (356 ms reading,
30 ms rebuilding, rounded separately). Playback resumed from frame GameTime
600 and reached 915 over 10.341 seconds, with no reported COB stop. The game
then exited through its menu.

The lifetime argument is the ordering, not the successful run: local unit
destructors retain their AI objects until no unit remains; the first puppet
create follows the type flip and object release. The experiment exercises that
ordering with the battle fixture's units. It does not prove every engine handler
or replace the complete replay comparison gates.

## Live rewind protocol

`tacli rewind INSTANCE open|seek T|now|fork|close [--dump NAME]` operates on
an already running recorded skirmish. Opening pauses at frame GameTime G and
asks the existing writer to flush its complete prefix. The writer observes the
request before acquiring the published ring cursor, writes only closed iterations,
retains the open iteration in its stream buffer, and acknowledges after the write.
The acknowledgement also publishes the actual filename the writer opened, including
a collision suffix when needed; rewind never assumes its first-choice name won.
The complete seek interval ends at G−1. Opening and closing without a seek never
rebuilds the world and restores the prior pause bit.

The first seek saves a standalone frame snapshot at iteration G+1, the original
controllers, view, message log and pause state. Its snapshot reader ignores dump-only
images, sight counters and count/name evidence; those records are not simulation
operations. The recorder suspends event hooks, capture and diagnostic counters,
retaining its comparison baselines and unpublished step-G events. The live-to-puppet
conversion uses the P1/P2 ordering above, then the ordinary bounded seek.

`now` rebuilds the saved frame through takeover using the saved controller types.
It restores messages and view, resumes the existing recorder and restores pause.
Its temporary sight effects use the recording's lead through G−1 plus the retained
step-G events. The next capture compares the complete pools, map and globals;
all feature-grid rows are marked for comparison. The original iteration G is
published once with the events it already held.

`fork` ends the original with `REC_END_FORKED` (13), including those pending events.
A duplicated writer-completion event transfers ownership only after the writer
closes its file and S4c lock. The frame visit polls that event, then the ordinary
file-locking takeover appends the new timeline. A failed append leaves playback
paused and permits retrying `fork`; `now` cannot resume an ended recording.

### Pause ordering and atomic dumps

A before-observer steals the six-byte `test byte [eax+0x38A51],bl` at `0x496920`.
On the game thread, while rewind owns the level, it sets pause before the original
test executes. The following `jne 0x496969` skips scheduler `0x495230` and step
`0x495490`. A physical Pause key therefore cannot advance the suspended world.
If the site cannot be installed, live rewind refuses to open. Ordinary playback
commands cannot move a level owned by rewind.

`--dump` is taken at the operation's frame visit, before restoring an unpaused
state, and before its success acknowledgement. It captures the exact rebuilt
frame regardless of CLI polling latency. `seek-frame T dump NAME` in the replay
trigger provides the corresponding paused, atomic fresh-process comparison.

### Initial smoke verification

On the reference setup, the normal-limits smoke test opened at GameTime 221,
closed without moving, paused at 625, sought to iterations 301 and 331, and
returned to exactly frame 625. The Pause key advanced neither an open rewind
nor its historical frame. Atomic seek/now/fork dumps succeeded. Conversion took
323 ms, return-to-now 556 ms and fork 558 ms, including the requested dumps.
A subsequent open at 918 forked at iteration 601 and played on.

The preceding smoke recording passed `tarec check`: 927 explained steps, zero
corrections. Its fork's first iteration compared EQUAL with its parent. The
pre/post comparison under D2 matched except for the newly allocated unseated
spectator's sight buffer (72,904 counters). At this run, D2 excluded unseated player
images only. The later approved amendment is recorded below.
Seated players' sight counters matched exactly.

The full `tareplay.py rewind battle` fixture runs a live battle through open/close,
return-to-now, and fork; compares a fresh seek and original playback; checks
contiguous iterations, event counts, AI activity and window health; and reports
conversion, return and fork times. It is also the U lane of battle and stock-limits
landing fixtures, using the battle on each limits build. The initial coverage
results below failed two checks; the later accepted scope and fresh passing
verification are recorded in their own sections.

### First full normal-limits run

The live run returned from iterations 901 and 1201 to frame 1519, then forked
from live frame 3346 to iteration 2731. Both timelines pass `tarec check` with
zero corrections; the original contains exactly iterations 1..3346, including
1519 and 1520 once each, and ends `REC_END_FORKED`. The fork's first frame
matches its parent. Playback dumps at 1549 and 2119 compare EQUAL, including
the radar picture. The 60-second now judge and 31-second fork judge passed.
Conversion was 658 ms, return 575 ms, fork 715 ms.

The gates still name these gaps:

- Pre/post: only the unseated spectator's sight allocation differs, as above.
- Fresh seek: V's WinLoseTime, loaded/count flags and version bytes differed.
  The implementation now initializes those fields through the loader's documented
  scalar values. The subsequent normal-limits smoke test compares the historical
  frame at iteration 301 with a fresh seek EQUAL, with no comparison masks.
- AI activity: 96.3% of 1019 AI mobile units moved over the window, but the
  baseline made no unit in that window. Under the approved OQ-C amendment this
  is UNRUN, not a pass. Its first later AI create was at 3835, outside the window.
- Counts spanning return: all 15 root sites match. Six COB child-script keys at
  `0x4B08C0`, called by `cob_run_step`'s START/CALL branch, differ beyond the
  existing 5%/2 band: SmokeUnit 32/0, Go 13/1, MotionControl 3/0,
  RequestState 30/7, open 123/94 and activatescr 13/2 (live/playback).
  Disassembly of the tested normal DLL identifies the counted return offset
  `0x7EB87` as that branch's allocation call. Return-to-now reconstructs COB
  objects but the plan excludes preserving their thread state. Any scoped
  count exception or expansion to preserve COB state needed an owner decision;
  no exclusion or tolerance had been changed for this run.

### Filename and recovery checks

A normal-limits run pre-created the recorder's candidate filenames in its owned
instance. The writer selected the `(2)` suffix; open, both seeks, return-to-now
and fork succeeded on that actual file. Return took 553 ms and fork 542 ms.

A separate four-seat run made its recording read-only after seeking to iteration
61. Fork refused after the original writer closed, retained frame GameTime 60,
and refused return-to-now because the original was ended. Restoring write access
and retrying fork succeeded in 140 ms. Opening the new timeline at 155 and asking
`now --dump` without seeking closed without movement and dumped frame 155.
Another open/seek followed by menu exit completed through the normal shutdown.


### Normal and stock coverage results

Both full rewind fixtures finished with 20 passing checks and two failures, with
no UNRUN checks. The extra builders introduced at tick 1200 in both the baseline
and live run let the AI choose construction work within the comparison window.
Seat 2 created a unit in both runs; seats 1 and 3 had no baseline create and were
reported as skipped under OQ-C. Mobile movement was 92.512% of 1015 units on the
normal build and 80.273% of 1024 on stock limits.

| Coverage build | Return frame | Fork source / target iteration | Conversion / now / fork |
|---|---|---|---|
| Normal limits | 1515 | 3362 / 2761 | 671 / 504 / 679 ms |
| Stock limits | 1516 | 3379 / 2761 | 512 / 409 / 493 ms |

Both original timelines are contiguous from iteration 1 through their fork source,
with each return boundary present exactly once. Both timelines on both builds
pass the stream check with zero corrections and matching snapshots. Fork frames
match their parents. Historical frame 900 (iteration 901) matches a fresh-process
seek exactly. Subsequent playback dumps, including radar, compare EQUAL. Both
60-second return and 30-second fork health checks pass.

The remaining failures are unchanged policy questions, not passing gates:

- Return-frame equality differs only in the unseated spectator's 72,904 newly
  allocated sight counters. Seated records' counters remain exact.
- Counts across return match all 15 root sites, but seven COB child-script keys
  exceed the existing band on each build. The keys are SmokeUnit, close, Go,
  MotionControl, RequestState, open and activatescr. The normal coverage DLL
  reports allocator return offset `0x1066C7`; stock reports `0x4B18C0` and
  `0x4B192D`. The raw differences remain failures; no new exception is applied.

These runs exercised the U lane, not the complete landing tier. The decisions
below settle their two policy questions; fresh verification remains required.


### Accepted scope, 2026-10-03

The owner approved extending D2 to the sight counters of unseated player records.
Seated players' counters remain exact. The spectator's allocation remains alive
until normal level teardown.

The owner also accepted reconstruction of COB threads on return-to-now. Full COB
execution state is not preserved: program positions, stacks and waits may differ.
The rewind-only `compare-counts --rewind` exception covers VM START/CALL child
allocations at `0x4B08C0` with semantic caller keys `0x4B18C0` and `0x4B192D`.
Every accepted pair is reported. Root calls, other allocator callers, other sinks
and puppet engine writes retain their existing checks. Ordinary replay comparisons
do not enable this exception. No claim of exact script continuation is made.

The raised-limits VM calls `tagpu_rec_cob_child`, which brackets its synchronous
allocator call with the corresponding diagnostic key. The stock VM already has
those return addresses. This keeps the exception independent of DLL layout and
avoids exempting unrelated calls originating in the DLL. The prior coverage runs
remain recorded as failures under the previous policy; fresh runs are required.


### Fresh verification under the accepted scope

Both coverage builds pass all 22 live-rewind checks in the landing run begun on
2026-10-03. The normal build returns to frame 1531 in 552 ms and forks from 3378
to iteration 2761 in 699 ms; conversion takes 725 ms. Stock returns to 1530 in
454 ms and forks from 3389 to iteration 2761 in 513 ms; conversion takes 523 ms.
These include the requested atomic dumps on the reference setup.

On each build, return-frame equality passes under amended D2; fresh-seek equality
passes without that mask; both timeline stream checks have zero corrections;
original iterations are contiguous with the return boundary present once; fork
frames and subsequent playback dumps compare EQUAL. Both health windows pass.
AI seat 2 creates in both the baseline and resumed run; seats 1 and 3 have no
baseline create and are reported as skipped. Movement is 90.945% of 1016 mobiles
on normal limits and 80.686% of 1020 on stock limits.

Across return-to-now, all 15 root counts match on each build. The normal comparison
covers 80 sink keys and stock covers 78; only the explicitly reported VM child
pairs use the accepted exception. This does not establish exact COB execution
continuity. The complete landing tier passes all 18 fixtures and reaches all 47 changed
functions on their required builds. Its replay-roundtrip note is on `8cb3ed5e`.
Dedicated reviews remain required before landing.


### Landing review correction

The dedicated review found that return-to-now used takeover's unconditional
objective arming. Stock `+kill 9` with an unused seat still calls `0x4904B0` and
disables objectives, so rewind must preserve that disabled state. The rewind
saves the objective's DWORD at `+0x88` before conversion and restores it before
returning to live steps; the same level owns the object throughout. A fork keeps
the takeover policy of arming objectives. The U lane now checks disabled-before,
disabled-after-return, and armed-after-fork through live reads. The first full
gate above predates this correction; a new gate is required.

The synchronization review found no surviving issue. The other confirmed finding
was stale policy prose; the current descriptions now state the approved scope.


The first regression setup omitted `cheats=on`, required by the level-4 `Kill`
command, and its disabled-before check correctly failed. The interrupted run is
not a passing gate. The fixture now enables that switch for the command and
restores its original value before rewind. An isolated four-seat run verified
objective values 0 before rewind, 0 after return at frame 387 (137 ms), and 1
after fork at frame 60 (142 ms), through live reads. The full tier is rerun with
this verified setup.
