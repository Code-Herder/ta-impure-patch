# cobtrace fixtures — the nine class scenarios, traced (tacob landing 2, 2026-09-07)

One folder per class, produced by `tools/cobtrace_fixtures.py` from `scenarios/cob-<class>.json`
on this tree's `ddraw.dll` with `tagpu_cobtrace.on=<type>` (only that unit type is logged) and
`tagpu_native.on=all` (the pose oracle lives in the native unit pass):

| File | What |
|---|---|
| `cobtrace.log` | every `S`/`R`/`X`/`K`/`D` line the type produced, from DLL attach to `tacli stop` — the contract is `research/notes/tacob-design.md` §"The trace contract" |
| `posedump.txt` | the one `tagpu_posedump.on` dump taken after the behaviour, camera parked on the unit; its header `posedump: tick=N idx=U` joins `cobtrace.log`'s `(tick, unit)` |
| `apply.json` | tacli's load report: engine indices, requested and actual positions, the switches, the camera |

These are landing 3's gate: `tacob run` replays the director's events and the diff of its
lines against `cobtrace.log` must be empty (with `Killed`'s second argument masked — it is the
caller's uninitialised local, a different number every run — and `D` lines fed back as the
`rand` results). A few seconds of one unit each, so the files stay small. Regenerate with
`tools/cobtrace_fixtures.py [class…]` after any engine-side change to the oracle; the seed is
fixed (7) so a run repeats, but the AI seat's decisions and the wall clock are not part of
the seed, so a regenerated file is *equivalent*, not byte-identical.

Produced 2026-09-07 (the table is `tools/cobtrace_fixtures.py`'s output summarised: line counts by kind, the tick span, the posedump's tick and its piece/vertex line count, the scripts that appear):

| # | class | type | lines | S | R | X | K | D | first..last tick | posedump | scripts seen |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | kbot | ARMPW | 38 | 20 | 18 | 0 | 0 | 0 | 93..324 | tick=585 (60 pieces/verts lines) | ARMPW, AimFromPrimary, Create, QueryPrimary, SetMaxReloadTime, StartMoving, StopMoving, walk |
| 2 | tank | ARMSTUMP | 722 | 347 | 340 | 0 | 6 | 29 | 91..1080 | tick=657 (16 pieces/verts lines) | ARMSTUMP, AimFromPrimary, AimPrimary, Create, FirePrimary, HitByWeapon, QueryPrimary, RestoreAfterDelay, RockUnit, SetMaxReloadTime, SweetSpot |
| 3 | building | ARMWIN | 23 | 12 | 11 | 0 | 0 | 0 | 91..902 | tick=583 (12 pieces/verts lines) | ARMWIN, Activate, Create, Go, InitState, RequestState, SetDirection, SetSpeed |
| 4 | death | ARMPW | 31 | 17 | 14 | 0 | 0 | 0 | 97..126 | tick=287 (20 pieces/verts lines) | ARMPW, AimFromPrimary, Create, Killed, QueryPrimary, SetMaxReloadTime, SweetSpot |
| 5 | fighter | ARMHAWK | 393 | 194 | 193 | 0 | 0 | 6 | 91..765 | tick=353 (19 pieces/verts lines) | ARMHAWK, Activate, Create, InitState, MoveRate2, QueryPrimary, QuerySecondary, RequestState, activatescr |
| 6 | gunship | ARMBRAWL | 99 | 50 | 49 | 0 | 0 | 0 | 97..404 | tick=791 (45 pieces/verts lines) | ARMBRAWL, Activate, Create, FirePrimary, InitState, QueryPrimary, RequestState, activatescr |
| 7 | bomber | ARMTHUND | 55 | 28 | 27 | 0 | 0 | 0 | 95..748 | tick=1059 (22 pieces/verts lines) | ARMTHUND, Activate, Create, InitState, QueryPrimary, RequestState, activatescr |
| 8 | ship | CORBATS | 1591 | 796 | 784 | 0 | 11 | 0 | 93..950 | tick=597 (54 pieces/verts lines) | AimFromPrimary, AimFromSecondary, AimPrimary, AimSecondary, CORBATS, Create, FirePrimary, FireSecondary, QueryPrimary, QuerySecondary, RestoreAfterDelay, SetMaxReloadTime, SweetSpot |
| 9 | sub | CORSUB | 1320 | 660 | 659 | 0 | 1 | 0 | 93..954 | tick=703 (10 pieces/verts lines) | CORSUB, Create, FirePrimary, QueryPrimary, StartMoving, StopMoving, SweetSpot |

The **fighter** run is short by design: an air-to-air engagement faults the engine in `ORDERS_CreateObject` (`0x43A164`, `mov ecx,[eax]` with `eax` = a null unit pointer + the position offset) within seconds of the first shots — measured with the oracle armed *and* in a control run without it, with a Vamp and with an unarmed Valkyrie as the target, so it is the engine's, not the trace's — and the pose dump is taken in flight before the shooting. The shipped run happened not to fault: it holds the takeoff, the flight, the dump at tick 353 and 91 shots (a `QueryPrimary`/`QuerySecondary` pair each), and ends at `tacli stop` at tick 765; a regenerated file may be cut short by that fault. The **death** run ends when the Peewee dies (its `Killed` line is the point). Every other run ends at `tacli stop`.

What each run shows, briefly — the skill's "what the first traces taught" and the design
note's contract carry the details: a run-later start (`SetMaxReloadTime`) first steps at the
next tick and a run-now start (`StartMoving`, `Create`) returns within its own tick; the
kbot's `walk` is a 19-tick `call-script` loop from `MotionControl` (`L:1`); the tank's
`RestoreAfterDelay` dies to the next aim's `signal` (`K`) and its `SmokeUnit` draws `rand`
under 66 % health (`D`); `HitByWeapon` has no trailing return and its thread ends under
`SweetSpot`'s name; `Killed` is a query with four arguments; the ship's two turrets run
`AimSecondary`/`AimFromSecondary`/`QuerySecondary` while a Roy shells it (`SweetSpot` every
tick); the sub fires eight torpedoes (`FirePrimary`, `QueryPrimary`) without an aim script;
aircraft answer `QueryPrimary`/`QuerySecondary` per shot and never run an `Aim*` script of
their own. No fixture refuses a start (`X`): stock scripts never fill the eight records.
