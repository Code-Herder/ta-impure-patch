# cobtrace fixtures — the nine class scenarios, traced (tacob landing 2, 2026-09-07)

One folder per class, produced by `tools/cobtrace_fixtures.py` from `scenarios/cob-<class>.json`
on this tree's `ddraw.dll` with `tagpu_cobtrace.on=<type>` (only that unit type is logged) and
`tagpu_native.on=all` (the pose oracle lives in the native unit pass):

| File | What |
|---|---|
| `cobtrace.log` | every `S`/`R`/`X`/`K`/`D` line the type produced, from DLL attach to `tacli stop` — the contract is `research/notes/tacob-design.md` §"The trace contract" |
| `posedump.txt` | the one `tagpu_posedump.on` dump taken after the behaviour, camera parked on the unit; its header `posedump: tick=N idx=U` joins `cobtrace.log`'s `(tick, unit)` |
| `apply.json` | tacli's load report: engine indices, requested and actual positions, the switches, the camera |

and one shared file:

| File | What |
|---|---|
| `replay.json` | the `get` timelines the replay needs and the log cannot carry, per class, written by `tools/tacob fit-world`. Only the tank has one (`HEALTH`, derived from `SmokeUnit`'s `rand` spacing — see `tacob-design.md` §"What the replay supplies"); every other class reads no engine value |

These are landing 3's gate, and it is met `[2026-09-07]`:

```
tools/tacob run --all                       # nine of nine, trace and pose
tools/tacob run kbot --trace-out /tmp/k.log && diff /tmp/k.log kbot/cobtrace.log
```

Every replay is **byte-identical** to the log the game wrote, header included — nothing is
masked. (`Killed`'s uninitialised second argument, which the design expected to mask, is one of
the `E` line's own arguments and so replays verbatim.) A few seconds of one unit each, so the
files stay small. Regenerate with
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

The **fighter** run is short by design: an air-to-air engagement faults the engine in `ORDERS_CreateObject` (`0x43A164`, `mov ecx,[eax]` with `eax` = a null unit pointer + the position offset) within seconds of the first shots — measured with the oracle armed *and* in a control run without it, with a Vamp and with an unarmed Valkyrie as the target, so it is the engine's, not the trace's — and the pose dump is taken in flight before the shooting. The shipped run happened not to fault: it holds the takeoff, the flight, the dump at tick 353 and 91 shots (a `QueryPrimary`/`QuerySecondary` pair each), and ends at `tacli stop` at tick 765; a regenerated file may be cut short by that fault. The **death** run ends when the Peewee dies (its `Killed` line is the point) — and its
`posedump.txt` is of **another unit** (`idx=2`, at tick 287, long after the Peewee was gone),
so it is the one fixture with no pose to compare; `tacob run death` says so and checks the trace
only. Every other run ends at `tacli stop`.

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
their own. No fixture refuses a start (`X`): stock scripts never fill the eight records, so that path is
covered by a synthetic COB in `tools/test_tacob.py` instead. Every posedump also shows pieces
`HIDDEN` that no script hides — the aircraft's flares and thrust anchors, the ship's wakes, the
sub's tubes: the model builder clears a piece's visible bit when its 3DO node has fewer than
three vertices (`0x45AF1B`), which the replay reproduces.

## The in-game wiki's units and the extensions (COB L2, 2026-10-27)

Seven more classes, recorded the same way on Impure's raised build
([the plan](../../ingame-wiki-cob-plan.html), L2). The six of the wiki's units play on `RS mixed`,
the generated map `tools/resource_fixture.py` installs into the recording instance; `extension`
is COBEXT, `tools/cob_ext_fixture.py`'s generated unit (ARMSOLAR's model, the script
`extension.bos` beside this README). Each folder adds a **`world.json`**, which the recorder
writes and `tacob run` reads: `"rules": "impure"` (ten records, the guarded interpreter's rules)
and the facts `tagpu_cob_getter` answers from, read out of the game at the posedump (the slot
count, the traced unit's id, owner, kills and build fraction, its seat's kind and its own
alliance byte); `extension`'s also names its BOS `source` and the stock `model`.

| # | class | type | lines | S | R | X | K | D | first..last tick | posedump | scripts seen |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 10 | builder | ARMCK | 957 | 480 | 477 | 0 | 0 | 0 | 27..1664 | tick=825 (12 pieces lines) | Create, Go, InitState, MotionControl, QueryNanoPiece, RequestState, SmokeUnit, StartBuilding, StartMoving, Stop, StopBuilding, StopMoving, close, open, stand, walk |
| 11 | advbuilder | ARMACK | 655 | 329 | 326 | 0 | 0 | 0 | 28..1680 | tick=826 (16 pieces lines) | Create, Go, InitState, MotionControl, QueryNanoPiece, RequestState, SmokeUnit, StartBuilding, StartMoving, Stop, StopBuilding, StopMoving, close, open, stand, walk |
| 12 | reclaim | ARMCK | 493 | 248 | 245 | 0 | 0 | 0 | 27..1660 | tick=825 (12 pieces lines) | Create, Go, InitState, MotionControl, QueryNanoPiece, RequestState, SmokeUnit, StartBuilding, StartMoving, Stop, StopBuilding, StopMoving, close, open, stand, walk |
| 13 | mex | ARMMEX | 13 | 7 | 6 | 0 | 0 | 0 | 28..31 | tick=677 (3 pieces lines) | Activate, Create, Go, InitState, RequestState, SetSpeed, SmokeUnit |
| 14 | moho | ARMMOHO | 13 | 7 | 6 | 0 | 0 | 0 | 29..32 | tick=678 (2 pieces lines) | Activate, Create, Go, InitState, RequestState, SetSpeed, SmokeUnit |
| 15 | geo | ARMGEO | 3 | 2 | 1 | 0 | 0 | 0 | 27..27 | tick=524 (5 pieces lines) | Create, SmokeUnit |
| 16 | extension | COBEXT | 29 | 19 | 10 | 0 | 0 | 0 | 25..34 | tick=320 (5 pieces lines) | Create, allied_past_the_array, allied_with_me, max_id, min_id, mine_is_local, my_build_left, my_id, my_kills, my_owner, nobodys_build_left, prober, sleeper |

What they show: a constructor finishing a nanoframe and one reclaiming a wreck run the same
traffic — the walk (`StartMoving`, `MotionControl`'s `walk`/`stand` loop, `StopMoving`), then
`StartBuilding` with its heading, `Go`'s `open` and turret turn, a `QueryNanoPiece` every tick of the
nanolathe, and `StopBuilding`, `Stop`, `close` — so a reclaim is a build to the scripts. Every
`QueryNanoPiece` lands **after** the unit's own script slice of its tick: its caller `0x43E400` is
called by the order handlers (`0x40252E` … `0x405855`), which run after `DoScriptsNow`, and tacob's
`ENGINE_STARTS` says so. The scenarios spawn the extractors and the geothermal plant as nanoframes,
but the applier creates a unit whole and writes its build fraction afterwards, so their `Create`,
`SetSpeed` (the metal, `2016` and `2352`) and `Activate` run in the spawn's own tick and the repair
that finishes them adds no COB event. `extension` runs every getter in **record 9**, which the
engine's eight records do not have; its R lines are the values `cob-getters` (the replay test
group) judges against the engine's state.

`tools/tacob run --all` replays all sixteen byte for byte, trace and pose.
