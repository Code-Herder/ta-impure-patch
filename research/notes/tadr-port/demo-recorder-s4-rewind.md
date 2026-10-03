# Live rewind (S4d): prerequisite experiments

Live rewind is not implemented. Its two prerequisite engine experiments passed
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

## Remaining implementation

The next work is the writer flush acknowledgement and capture suspension,
followed by the live rewind state machine. Return-to-now must preserve the
unpublished live iteration and pending effects; a fork must transfer writer
ownership under [S4c's file locks](demo-recorder-s4-forks.md). These experiments
used a completed recording as the rebuild source and do not test concurrent
writer/reader opening, return-to-now equality, or fork handoff. Stock limits,
the full rewind fixture, documentation and dedicated review remain required
before the S4d landing.
