# Live rewind (S4d)

Live rewind supports open, seek, return-to-now, fork and close through `tacli` on
a recorded skirmish. The full landing tier passes under the approved limits
below: 570 checks across 20 fixtures, with all 62 changed functions reached on
every required build.

Its two prerequisite engine experiments passed
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

### The verbs as a queue, and their outcomes

The trigger's `rewind ...` line is parsed (`rw_trigger`: the prefix, the length, the ` dump NAME`
suffix, whose refusals are logged and record no outcome) into a verb, and the HUD calls `tagpu_replay_rewind(op, T, dump)`
with one of the five; both queue it, the latest replacing an earlier one not yet taken. A verb the
parse does not know is queued too, so it meets the state refusals first (`waiting for the writer`,
`no rewind is open`) and its own last, as every verb always has. **The ordering, on one thread:**
the frame visit takes the queue right after `trigger_read` and before `rw_poll` (`rw_visit`), so
the trigger's verbs run where they ran inside the trigger's read; a HUD click comes from the wndproc
or the shield's mouse path, both on the game thread, and waits for that visit, between two steps,
where `seek_run` must run.

Every rewind line that ends a verb -- refused, failed, opened, complete, returned to now, forked,
closed -- is logged by `rw_said` with its text unchanged and becomes the last request's outcome
(`tagpu_replay_result`, kind REWIND, the verb as its op). The close that follows a refused open
is logged and is not its outcome: the refusal stays the last. `tagpu_replay_rewind_state` gives
the state (idle, flush, open, past, ending), whether an open would be taken now (a live recorded
skirmish, not a network game, whose recorder is healthy: `tagpu_rec_rewind_ready`, the open's
test without its flush), the live recording's span once open, G and the last seek's T. The
trigger's `hud state` prints both; the rewind lane checks them before the first open (idle,
available), after it (open, G, the outcome `opened at GameTime ...`) and after a refused
`rewind seek 0`.

**The fork and the original's compaction.** When the recording a rewind forks was itself a fork
(a live game a takeover made), its writer is an append writer, which queues the file's compaction
before it signals its done event (`tagpu_recwriter.c`, the hand-over); the compaction holds the
file's lock while it runs. So `rw_poll` takes the fork only once the done event is seen and no
request on the file is queued or running (`recfile_pending`, which leaves out a prune: one named
for this file only keeps it, and opens neither it nor its lock): from the event on the compaction is
queued, running or finished, and the fork's `tk_open` takes the lock after it, never against it.

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

Those runs failed on two policy questions:

- Return-frame equality differs only in the unseated spectator's 72,904 newly
  allocated sight counters. Seated records' counters remain exact.
- Counts across return match all 15 root sites, but seven COB child-script keys
  exceed the existing band on each build. The keys are SmokeUnit, close, Go,
  MotionControl, RequestState, open and activatescr. The normal coverage DLL
  reports allocator return offset `0x1066C7`; stock reports `0x4B18C0` and
  `0x4B192D`. The raw differences remain failures; no new exception is applied.

These runs exercised the U lane, not the complete landing tier. The decisions
below settle their two policy questions; the later verification runs apply that scope.


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
remain recorded as failures under the previous policy. The fresh runs below apply the accepted scope.


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
The dedicated reviews and their correction are recorded below.


### Landing review correction

The dedicated review found that return-to-now used takeover's unconditional
objective arming. Stock `+kill 9` with an unused seat still calls `0x4904B0` and
disables objectives, so rewind must preserve that disabled state. The rewind
saves the objective's DWORD at `+0x88` before conversion and restores it before
returning to live steps; the same level owns the object throughout. A fork keeps
the takeover policy of arming objectives. The U lane now checks disabled-before,
disabled-after-return, and armed-after-fork through live reads. The first full
gate above predates this correction; the verification below includes it.

The synchronization review found no surviving issue. The other confirmed finding
was stale policy prose; the current descriptions now state the approved scope.


The first regression setup omitted `cheats=on`, required by the level-4 `Kill`
command, and its disabled-before check correctly failed. The interrupted run is
not a passing gate. The fixture now enables that switch for the command and
restores its original value before rewind. An isolated four-seat run verified
objective values 0 before rewind, 0 after return at frame 387 (137 ms), and 1
after fork at frame 60 (142 ms), through live reads. The full tier is rerun with
this verified setup; its U-lane results follow.


### Verification of the review correction

On the reference Wine setup, 2026-10-03, both coverage builds pass all 25 U-lane
checks, including objective values 0 before rewind, 0 after return, and 1 after
fork. Normal limits returns to frame 1697 and forks from 3543 to iteration 2941;
stock returns to frame 1684 and forks from 3529 to iteration 2911.

| Build | Conversion | Return to now | Fork |
|---|---:|---:|---:|
| Normal limits | 343 ms | 662 ms | 398 ms |
| Stock limits | 258 ms | 550 ms | 266 ms |

Return dumps compare EQUAL under amended D2. Historical frame 1080 compares EQUAL
with a fresh-process seek without that mask. Both timelines have zero corrections;
the original iterations are contiguous through the saved return boundary. Fork
frames and subsequent playback dumps, including radar, compare EQUAL, and both
health windows pass. AI creation is compared for seat 2 on normal limits and
seats 2 and 3 on stock; the other seats have no baseline create and are reported
as skipped. Movement is 92.2% of 1000 mobile units on normal limits and 80.924%
of 1017 on stock. All 15 root counts match on each build; only the reported
START/CALL child pairs use the approved rewind exception.

The renewed complete tier did not pass: the ordinary takeover's AI check found
seat 3 creating in the parent and only seat 1 in the fork, while `forest-fire`
ended naturally at GameTime 4320 before its required 4500. Both rewind lanes
passed all 25 checks. Replaying the same takeover recording at 1651 on both this
branch and pre-S4d `f0748b8c` reproduced the same single fork creation at tick
3347, slot 502, seat 1; the parent's creation was tick 2446, seat 3. This failure
predates live rewind and is not waived. The battle fixture now adds unordered
construction units at tick 120 so the AI check has more builders; its per-seat
creation and movement requirements are unchanged. The forest fixture keeps the
same two remote stationary units as `forest-circle`, so losing the battle's last
combatant does not end the fixture before its scripted endpoint.


The repaired forest fixture passes all 23 checks at the same seeded sample ticks:
1291, 2431, 3091 and 3391. It reaches its scripted end, playback and fresh seek
compare EQUAL, and all three round trips have zero poison residue.

The strengthened takeover fixture passes at the failing tick 1651: seats 1, 2
and 3 all create in both timelines over [1651, 3451], and 92% of 1007 mobile
units move. Takeover takes 571 ms; the controlled unit moves 205 wu. The 1377-unit
fork frame, both stream checks and playback across the fork all compare EQUAL.
These targeted runs repair the two fixture failures; the complete landing tier
must still be rerun on the committed fixture changes.


### Landing gate stopped, 2026-10-03

The third post-review full-tier attempt, on `9e1c88da`, reached the known O15
naval puppet failure: `naval-unmapped` R stopped in `CORMSHIP.COB` at word 724,
`CALL cannot obtain a child thread`. Its stock-limits W process also failed to
obtain the initial UI snapshot. This is not a passing gate. The remaining runs
were interrupted after those failures under the project's three-attempt limit;
their incomplete results are not counted as passes. The earlier U-lane and
fixture-repair results above remain evidence for those runs only.

O15 is an actual VM refusal, distinct from the approved rewind comparison of
START/CALL allocation counts. The exception does not permit the guard to stop,
and full COB execution-state preservation remains outside this landing. S4d
has not been fast-forwarded to main. The next work is to resolve the naval
puppet guard failure and establish why the Watch launch had no UI snapshot
before attempting another complete landing gate.


### O15 and worker cleanup correction

The resumed investigation reproduced both symptoms from the retained recording.
The naval stop came from treating stock's exhausted-CALL child `-1` as fatal;
its exact native behavior is now preserved with a sentinel-or-slot bound. The
subsequent Watch failure came from reusing that still-stopped game process after
graceful cleanup failed. Failed jobs now terminate their owned process before
worker reuse and retire the worker if termination cannot be confirmed. See
[the COB note](cob.md#o15-exhausted-call-and-failed-worker-cleanup).

On the reference Wine setup, the same recording now passes round trips at 1681,
2071 and 2761 (63, 25 and 32 ms), each with 26 tables poisoned and zero residue.
The dumps 300 ticks after each round trip compare EQUAL, including radar, with
37, 34 and 33 live units respectively. A control using the earlier DLL still
stops at the guard; the fixed harness confirms its process has ended before
starting stock Watch on that same worker; all eight Watch checks pass, including
Leave and a second Watch. The full-pool CALL probe separately
checks both human and local-AI records and a continuing match. Full-tier and
review verification of these corrections remain required before landing.

The START full-pool control also passes all 16 getter checks: refused arguments
are consumed, SIGNAL frees the seven sleeping children, and getter CALLs resume.
The portable COB suite passes 40 tests, and the replay harness suite passes 32,
including collection-failure cleanup and retirement after failed termination.


### Return-to-now preserves the previous radar scan

The next complete tier cleared the naval COB stop and stock Watch failure, but
found a new exact-frame difference: unit 1004 at GameTime 1679 gained contact
bit 8 on return-to-now. The viewed player's radar pass runs every 30 ticks;
rerunning it during restoration can reveal a newly created unit before the
original scan would have done so. This is simulation state: acquisition reads
these contact bits. No comparison mask is widened.

A focused reproduction creates one human solar plant while paused, then opens
rewind, seeks to the past and returns. The earlier implementation changes its
state from `0x30280221` to `0x30280321` at GameTime 329. The correction restores
the four saved contact bits for bounded, identity-matched live units and skips
the extra scan only on return-to-now. Its same test at GameTime 318 compares
EQUAL across the entire saved and restored frame. Both permanent rewind lanes
now create that paused unit before their saved-present dump.

That full-tier attempt also found AI construction comparison failures in both
rewind lanes. The resumed AI remained active: in the normal run, seat 2's
nanoframe reached completion, its builders continued moving and executing
orders, and resources were ample. The missing event was a later creation in
the comparison window. This is still a failing check; the contact correction
is being tested with the same AI comparison and constructor fixture.


The completed report for that tier contains 512 passing and four failing checks:
the contact bit, the two AI construction comparisons, and `naval-unmapped`'s
flare count from explosion-add caller `0x4812D3` (33 live, 30 puppet, outside
the existing band). Replaying its retained recording reproduces the same flare
count. No count tolerance or expected-absent row has been changed. The normal
and stock rewind runs with the contact correction both compare the saved and
restored frames EQUAL; the normal AI construction check still fails, while the
stock check passes. These results do not constitute a passing landing tier.


#### Separating setup time from the AI window

The objective regression's command setup originally ran after the wait for
GameTime 1500. It advanced the game to 1670–1692 before pausing. The earlier
passing runs began their AI comparison near 1530; an initial construction at
1668 lay inside that window, but before the later window. The fixture now
performs its debug-switch and empty-seat `+kill` setup during the first rewind's
existing pause, before closing it and waiting for 1500. The comparison's
per-seat creation rule, movement fraction and 1800-tick length are unchanged.
A targeted normal-limits run holds GameTime 638 throughout the setup commands,
starts its comparison at GameTime 1520 and passes all 25 checks. Seat 2 creates
in both timelines, and 93.235% of 1020 AI mobile units move. Return-to-now takes
496 ms and the fork 704 ms.

The stock-limits run holds GameTime 623 during setup and begins the comparison
at 1519. It passes 24 of 25 checks, including exact return-to-now, fresh-process
seek, fork images, both recording streams and effect counts. Return-to-now takes
436 ms and the fork 512 ms. The remaining failure is the per-seat AI creation
check: seats 2 and 3 have baseline creations, but seat 3 has no corresponding
creation in the resumed window; 82.466% of 1038 AI mobile units move. Moving setup
under pause corrects the comparison start but does not resolve this failure.

Verification stopped at the three-attempt limit for this resumed investigation.
S4d remains unlanded. The AI construction failure needs further diagnosis; the
naval count proposal below was a separate owner decision. The full-tier
runtime results above remain evidence for that run, but its coverage report is
invalid because the coverage maps were rebuilt before offline reporting ended.
A future complete tier must retain its maps and binaries until reporting exits.

#### The naval flare count

The retained failing window has 201 COB explosion additions versus 197 in
playback at caller `0x4812D3`. That parent difference already passes the existing
5% band; its child flares, 33 versus 30, fail their separate floor of two.
Repeated playback reproduces both figures. A fresh instrumented live/playback
pair produces 198 additions and 41 flares on each side. Disassembly establishes
at most one child flare per addition, conditional on the piece point being
above water. The measurements do not identify the individual missing additions.
The temporary trace has been removed and all four clean builds pass. The owner
approved bounding this child difference by its already-accepted parent difference.
The comparator restricts the allowance to child `(0x472630, 0x420AE6, 0x4812D3)`
and parent `(0x420A30, 0x4812D3, 0)`. The parent must be counted in every dump and
pass the ordinary band; differences must have the same direction and the child
difference cannot exceed the parent difference. Counts must be nonnegative and
child counts cannot exceed parent counts on either side. Every accepted case
reports both pairs. The same retained failing dumps now pass with the explicit
line `accepted bounded COB flare 0x4812D3: parent live 201, puppet 197; flare live 33, puppet 30`.
This is an approved tolerance; it does not establish the identity of the missing
parent calls. The 39 portable tests include wrong callers, larger/opposite child
differences, unaccepted parents, invalid counts and an unobserved parent.


#### Control without rewind: independent games fail the AI comparison

A resumed investigation ran the same stock-limits battle and scripted builder
additions without any rewind operation. Against the retained baseline over
GameTime [1519,3319], the control fails the same per-seat test: compared seats
2 and 3, missing seat 3, seat 1 skipped; 77.401% of 1031 AI mobile units move.
This demonstrates that the current comparison can fail without a rewind. It
does not establish that every reconstructed AI task is correct.

The retained failing rewind run already differs from its separate baseline at
GameTime 1519, before reconstruction: seat 3's commander (slot 1501) has action
26 in the live run and a MobileBuild order (action 25, type 268) in the baseline.
The baseline creates that type at 2819. The live run's four constructors added
at 1217 retain their MobileBuild orders after return and continue moving; the
baseline's corresponding constructors are also still travelling at 3300. The
recorded positions and orders are evidence of different pre-rewind tasks, not
evidence of a task lost by reconstruction.

The initially proposed same-game construction comparison was rejected during
owner discussion: a common prefix does not establish identical future choices
when AI internal state is rebuilt. The owner approved direct AI activity checks
instead. Construction counts remain diagnostic evidence, without a matching
outcome requirement. The separate naval flare allowance was subsequently approved as described above.

### Direct AI activity after return and fork

The U lane observes a fresh 1800-tick window after each restoration. Every AI
seat must construct new orders within its planning routine and at least one
sampled order must reach normal handler dispatch. The existing requirement
that 25% of AI mobile units move more than eight map units remains. Exact frame
restoration, recording continuity, replay counts and runtime health checks remain.
The separate unrewound baseline launch is removed. The fork's observation now
also runs for 1800 ticks and at least 60 seconds.

The recorder's harness trigger is `ai-activity NAME from START until END`,
where START is later than the current GameTime and END is exclusive. It reports
orders and first dispatches per seat, then a completion line for that exact
interval. An unavailable observer, missing report or invalid context fails the
check. Samples are a bounded set of 32 pending nodes per seat, recycled when
dispatched or destroyed. This establishes execution of sampled new orders,
not completion of every order the AI issued or equivalence of strategy.

The context brackets `0x408C40`, but stops attributing at its final shared
weapon-retargeting call, `0x4089A0`, through the existing weapons wrapper. The common constructor `0x43A0C0` supplies
new nodes; the normal dispatch sites `0x43B878` and `0x43BB1D` prove a sampled
node was linked and reached its handler. Destructor `0x43A1F0` forgets its
sample before freeing can recycle the address. Player slots, unit slots and
context depth are bounded. Only the game thread accesses active observations;
presentation, player commands, scenario orders, restored nodes and the shared
automatic targeting phase cannot supply an AI-planner sample. No observation
changes engine state or a recording's format.

The first attempted observer, on the list-insertion functions, missed the AI
helpers' inline insertions and was replaced before acceptance. A second attempt
overlaid the existing acquisition detour; the limits integrity check refused
startup. The final implementation calls through the existing weapons wrapper.

MEASURED on the reference setup, coverage stock-limits build: a native `+AI 2`
control disabled seat 2 before arming [40,940). Seats 1 and 3 issued and dispatched
1 and 2 sampled orders respectively; seat 2 reported 0/0, and the checker failed
with exactly seat 2 missing. Re-enabling seat 2 and observing [948,2748) passed:
seat 1 = 2/2, seat 2 = 3/3, seat 3 = 2/2 orders/dispatches. All other seats
reported zero and both windows completed with context errors zero. This checks
that active neighbours cannot hide a disabled planner and that revival is seen.
The control used `replay-four-ai.json`, a paused native toggle before each future
observation window, and the stock-limits coverage DLL. It supplied no unit orders.
The dedicated U lanes pass **28/28 checks on each build**, including exact
return-frame and fresh-seek comparisons, both timelines, contiguous recording,
playback across return-to-now, and the existing count policy. Their AI results:

| build / operation | interval | seat 1 orders/dispatches | seat 2 | seat 3 | mobile movement |
|---|---|---|---|---|---|
| normal / now | [1533,3333) | 1878/215 | 1888/197 | 1808/203 | 94.94% of 1047 |
| normal / fork | [2825,4625) | 1514/197 | 1561/195 | 1509/204 | 89.97% of 907 |
| stock / now | [1520,3320) | 1728/197 | 2028/214 | 1907/192 | 80.74% of 1023 |
| stock / fork | [2805,4605) | 1433/194 | 1952/218 | 1773/192 | 87.01% of 947 |

All four windows completed with context errors zero. Neither fork observed
construction during its window; both nevertheless prove fresh planner orders
reached dispatch for every AI seat. Arming each window occurs while paused, so
command latency cannot consume the requested future starting tick.

All four DLL variants build, the 39 portable harness tests pass, and the driving
skill check is clean. These are targeted U results, not a completed landing tier.
The naval flare-count allowance is approved and verified against the retained
failure. The completed full-tier result is recorded below.

### Secondary order coverage

The first full tier with both approved policies passed all **524 runtime
checks**, including both 28-check U lanes. The coverage gate rejected the run:
61 of 62 changed functions were reached; `ev_nano_h2` was absent on both builds.
The existing fixtures issued no BuildWeapon or SelfDestruct order, the two
actions whose descriptor flag `0x40000` selects the secondary list at unit+0x60.

The added `secondary-orders` and `stock-secondary-orders` fixtures select an
isolated solar at the pinned camera centre with native `pclick:512,384`, then
send `ctrl+d` in a later step. Two remote unarmed units keep both seats alive.
A one-batch drag and shortcut did not destroy the unit; the separate native
click and shortcut did, and the normal coverage build reached `ev_nano_h2`.
This is coverage of the dispatcher observation, not AI liveness evidence.
The fixture runs the normal live/playback/round-trip/fresh-seek comparisons.
Both coverage builds reach `ev_nano_h2`; all **46 checks** pass across the two
fixtures, including exact playback, round trips, fresh seeks, event counts and
zero COB engine writes on puppets.

### Landing verification (2026-10-04)

The complete tier on `384d55a3` passes **570/570 checks across 20 fixtures**,
including **60 exact round trips**. Coverage reaches **62/62 changed functions**
on every required build. The `replay-roundtrip:` note records the fixture seeds.
Both U lanes pass all 28 checks, including exact return frames, fresh seeks,
contiguous original recordings, both timelines and the effect-count comparison.
The secondary-order dispatcher is reached on both builds.

| build / operation | interval | seat 1 orders/dispatches | seat 2 | seat 3 | mobile movement |
|---|---|---|---|---|---|
| normal / now | [1530,3330) | 1890/198 | 2030/196 | 1758/192 | 92.47% of 1023 |
| normal / fork | [2827,4627) | 1654/205 | 1927/195 | 1504/192 | 91.13% of 947 |
| stock / now | [1521,3321) | 1982/209 | 2018/197 | 1930/195 | 96.28% of 1047 |
| stock / fork | [2836,4636) | 1659/203 | 1944/198 | 1791/192 | 79.46% of 998 |

Return-to-now takes 552 ms on normal limits and 466 ms on stock limits; fork
conversion takes 691 ms and 648 ms respectively. All four DLL variants build,
39 portable harness tests pass, and the wiki builds 97 pages plus its index.

The accepted limits remain: unseated sight counters are outside D2; COB threads
are reconstructed, with rewind-only VM child counts reported; the specific
naval flare allowance is bounded by its accepted parent-count difference. AI
liveness establishes new planning and dispatch, not identical strategy or full
COB execution continuity. In-game rewind controls and multiplayer recording are
separate work.

### Independent review corrections

The two HIGH read-only reviews of `6191f265` verified the synchronization
protocol and found three distinct issues: missing byte checks before the AI
observers, takeover's nonempty-seat condition incorrectly applied to a saved
present, and a stale reference to the removed independent AI baseline.

All three observer prefixes are now checked before any is installed. A mismatch
disables this observation rather than overwriting an unknown engine entry.
Return-to-now permits a saved empty human seat while keeping all other takeover
bounds; ordinary takeover still requires a playable seat. The U-lane description
now names its live game and direct per-seat planning/dispatch checks.

MEASURED on the reference setup: `replay-secondary-orders`, developer cheats
enabled, then native `+kill 9` and `+kill 0`, leaves the human live count at zero.
Before the correction, open and seek to 301 succeeded but return-to-now was
refused at saved GameTime 563. After the correction, normal limits restored
GameTime 568 in 243 ms and stock limits restored 566 in 238 ms. Both saved and
restored frame pairs are EQUAL under the approved takeover comparison, and
both processes reopened and closed rewind successfully.

The byte-check control used an isolated executable copy with `0x43A0C0`'s
first byte changed from `83` to `90`, and launched only to the main menu.
Initialization reported that entry's mismatch. Live reads showed `53`, `90`
and `53` at `0x408C40`, `0x43A0C0` and `0x43A1F0`: none of the three observers
was installed. The original executable was unchanged and the instance's
original executable link was restored after the control.

The full tier on `ce7be889` passes 569 of 570 runtime checks. Both U lanes pass
all 28 checks. Its one failure is the ordinary K takeover's older construction
comparison: seat 2 creates no unit where its original timeline did, although
92% of 978 AI mobile units move. This remaining strategy-equivalence assertion
is inconsistent with the direct planning-and-dispatch rule adopted for forks.

K now uses the same observer and parser as U. It waits for the engine's pause
bit before arming a future 1800-tick window, resumes only after the armed
acknowledgement, and waits for the completion report before ending the game.
Every AI seat must supply newly planned orders that reach normal dispatch;
movement remains gated at 25%, while construction is reported only. Missing or
invalid observation data cannot pass. The frame dump remains at its original
scheduled tick. This changes the harness, not the engine or recording format.

Five takeover-check regression tests cover active seats without construction,
an inactive seat despite movement elsewhere, missing observation, insufficient
movement, and no mobile units. The complete portable suite passes 39 tests.
MEASURED on the retained failing recording, takeover at 1981: the observer's
[1990,3790) window reports seats 1, 2 and 3 at 1710/196, 1955/196 and 1670/190
new orders/dispatches, with no observer errors. 92% of 978 mobile units move.
Only seats 1 and 3 construct a unit: seat 2 is planning and dispatching despite
making none. The judge, conversion-time and human-order checks also pass.
### Final gate after the review corrections

The complete landing tier on `26f1586b` passes **570/570 checks across 20
fixtures**, with **60 exact round trips** and **62/62 changed functions reached
on every required build**. The passing `replay-roundtrip:` note records its
fixture seeds. Both U lanes pass all 28 checks; ordinary takeover's AI check
passes with the shared direct-activity rule.

Normal-limit return-to-now takes 629 ms and fork conversion 774 ms; stock
limits takes 461 ms and 614 ms respectively. AI movement after now/fork is
94.44%/90.01% on normal limits and 81.82%/74.11% on stock limits. Every AI seat
issues fresh orders that reach normal dispatch in each window.

The ordinary takeover observes [2083,3883): seats 1, 2 and 3 report
1699/204, 1966/198 and 1683/190 new orders/dispatches, with no observer errors;
92% of 972 mobile units move. Only seats 1 and 3 construct a unit. The check
therefore proves the accepted planning-and-dispatch requirement without relying
on identical building choices.

All four DLL variants build without warnings, 39 portable harness tests pass,
and the wiki builds 97 pages plus its index. Both full HIGH reviews verified the
engine and synchronization changes; the three confirmed findings are corrected
and rechecked. The targeted HIGH review of the ordinary takeover harness
correction also reports no remaining finding. The accepted limits listed above
remain unchanged.
