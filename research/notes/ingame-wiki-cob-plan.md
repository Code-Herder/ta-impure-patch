# Our own COB VM, and the in-game wiki's units animated by it, planned

**Written** 2026-10-27. **L1 and L2 are built** (the core, tacob on it, the shared rules, the
wiki's units recorded and replayed), and **L3a and L3b are built** (2026-10-28, on
`worktree-wiki-anim`): the wiki's units run their scripts in both simulations through one host,
the scripts gate the builds, the demos wait on the work, and the stage draws the poses the scripts
set, in the game and in the mockup; **L3c**, the corpus gate, is built with one refusal open
(ZZZ, below); **L4a**, the nano spray, is built (2026-10-28): a builder at work sprays from the
piece its script's `QueryNanoPiece` names, as the engine's particles, in both simulations and on
the stage (*The spray*). L4b (`emit-sfx`) and L5 (sound) are planned (see *The landings*). The wiki's specification is still
[the in-game wiki](ingame-wiki.html) and its mockup, which stays the 1:1 spec of the DLL wiki. The
engine facts below carry their address and are the engine map's
([*The COB engine — mapped by us*](exe-reverse-engineering.html)); a fact this plan needs and the map
does not have yet is listed under *Research each landing owes*, and the landing that first relies on
it disassembles it. When the work closes this file is folded into the wiki's note, `tacob-design.md`,
`gpu-status.md` and `roadmap.md`, and then deleted.

## Why

Before this work the wiki's stage drew each unit from its script's `Create()` hides alone
(`tagpu_cobscan.c`): every unit in its rest pose, a kbot's legs still, a constructor's nanolathe
folded, an extractor's arms unturned, and no nano spray. Animating them needs a COB VM the wiki can
run. The wiki runs one (L3a), draws what it poses (L3b) and sprays from the piece the script
names (L4a).

The one VM we have outside the engine is tacob's, in Python, byte-identical to the engine on nine
cobtrace fixtures ([tacob](tacob-design.html), landing 3). It has already drifted from the game:
it holds eight records and answers no `get` above id 20, while the game has run ten 128-word records
and the eight port-F getters since 2026-10-06 ([COB compatibility](tadr-port/cob.html)). A second
and third VM written for the wiki would drift the same way, and the owner's concern is exactly that:
the VMs drifting apart as COB features are added.

## The owner's decisions (2026-10-27)

1. **One COB VM outside the engine: `tagpu_cobvm.c`.** Engine-free, global-free, bounded by
   construction. The DLL's wiki links it; **tacob deletes its Python VM** and loads the same C file
   through ctypes, and so does the mockup's local server. No second implementation exists anywhere.
2. **It is complete and byte-exact, not a wiki subset**: every opcode, every `get`/`set` id, every
   Impure extension, the engine's quirks included (CREATE_LOCAL_VAR writes nothing, a refused CALL
   sleeps forever, an unknown opcode kills its thread silently).
3. **It replaces the game's interpreter — in the next program, not this one.** The owner has
   committed to the swap; this program builds the core so that the swap is a change of code and not
   of recorded state.
4. **It runs on the engine's memory layout** (below): the records, the statics block and the piece
   animation block are byte for byte what Impure's game holds, pinned by static asserts, so the
   recorder's script slice, the save format and the puppets see the same bytes after the swap.
5. **It is safe to run on several threads.** Reentrant; the unit of parallelism is one COB object;
   reads leave through host callbacks, writes outside the object through an ordered per-object
   effect log (below). The core never owns a random number generator.
6. **Drift is a test failure with a name.** Each Impure COB extension is one shared engine-free
   function that the game's adapter and the core both call; cobtrace fixtures recorded from the real
   game are replayed through the core byte for byte; **a landing that adds a COB feature to the game
   adds a fixture that exercises it.**
7. **In the wiki the scripts are part of the simulation**, not decoration: a build progresses only
   once the builder's script has set INBUILDSTANCE, a factory's once YARD_OPEN is set, an extractor
   produces only once ACTIVATION is set — each read as the committed value, as the engine reads it.
8. **The wiki calls what the engine calls for the actions the wiki performs**, with the engine's
   arguments, entry and side of the tick, and nothing else (the table below). Weapons, transports,
   aircraft and `Killed` wait until the wiki has those actions.
9. **A script the core refuses quarantines its unit type in the wiki**: the type stands in its
   `Create()` rest pose, every gate of its counts as open (today's timing), the demo plays on, and
   the wiki's log names the type, the script, the pc and the reason. **Every compatibility setup's
   COB corpus runs through the core as a gate**: a refusal of a script the real engine runs is a
   core bug to fix, never a quarantine to accept.
10. **Demo timelines wait on simulation state.** An input step may be `until` a state (a unit
    built, a builder in its build stance), bounded by a maximum after which the demo stops and the
    page says why. Timing becomes a property of the unit's script, so the per-mod tutorials need no
    per-mod timeline. The 11 Quick Build demos are converted once.
11. **The nano spray and `emit-sfx` are drawn.** The spray starts at the point `QueryNanoPiece`
    returns; both are drawn the way `tagpu_fx.c` draws the engine's particles, and their randomness
    comes from a wiki-owned deterministic generator — never the engine's, so an open wiki cannot
    consume an MP game's random numbers.
12. **The wiki gets sound, through our own mixer**: DirectSound from the DLL (the game imports it
    already), the game's own WAVs read through `tagpu_vfs`, the player's sound settings honoured
    (volume, NoDirectSound `0x51E690`, UseWindowsSound `0x51E694`), attenuated by the stage's camera;
    WebAudio with the same rules in the mockup. The engine's sound entries (`0x47F0C0`, `0x47F1A0`,
    `0x47F300`) are not called: they attenuate by the game's camera and spend its 24 per-kind
    cooldowns (`0x5086EC`).
13. **What sounds**: the unit's sounds.tdf categories on the engine's own triggers (select, ok,
    cant, build, working, unitcomplete, activate, deactivate, arrived where the engine plays it);
    the sidebar's button sounds; the engine's cooldowns reproduced as wiki state. No combat sounds;
    the game's own sounds are not ducked while the wiki is open. A script makes no sound: the
    engine's `PLAY_SOUND` (`0x10072000`) reaches the unknown-opcode tail `0x4B1B60` and kills its
    own thread, and the core does the same (the grill assumed otherwise; the engine map corrected it).
14. **tacob gains a native part**: it compiles the core with `cc` on first use into a gitignored
    cache keyed by the source's hash; `tacob-build.py` cross-compiles it with mingw at the Windows
    Python's width and proves it loads in a prefix with no compiler.
15. **Order**: L1 and L2 now, beside the sidebar's landing (they share no file with it); L3, L4, L5
    after the sidebar lands and before the per-mod tutorials, which need L3's `until` waits.

## The core

### Its memory: the engine's layout

Impure's COB object (`TAGPU_COB_OBJECT_BYTES` = `0x158C`, `tagpu_cob_safe.h`) is the engine's
0x544-byte object with its records widened: the header `+0x00..+0x1B`, then
`TAGPU_COB_THREADS` (10) records of `TAGPU_COB_RECORD_BYTES` (`0x224`: nine dwords and 128 stack
words, `TagpuCobRecord`) from `+0x1C`, the running count at `TAGPU_COB_COUNT_OFFSET` and the posed
model at `TAGPU_COB_MODEL_OFFSET`. Beside it the engine allocates two blocks the object points to:
the statics (`+0x10`, a dword each) and the piece animation array (`+0x14`, **19 dwords a piece**:
the flag, then MOVE target, MOVE speed, TURN target, TURN speed, SPIN target speed, SPIN acceleration,
three axes each). The piece positions and angles themselves are not in the object: MOVE, TURN and the
stepper read and write them through `vt+0x00/0x04/0x14/0x18` in the posed model's
`PrimitiveStruct` (`o3 + 0x22 + piece × 0x36`, `file-formats.md` §2.6), and SHOW, HIDE, CACHE and
SHADE flip that structure's flag bits. Those setters also write three words of the model's header
(`+0x04`, `+0x08`, `+0x10`: the engine map's vtable table), so the core's fourth block is the
model from its first byte, not the `PrimitiveStruct` array alone.

The core's state is therefore four blocks, each in exactly that layout:

| Block | Layout | The wiki's | The game's, after the swap |
|---|---|---|---|
| records + count | `TagpuCobRecord[TAGPU_COB_THREADS]` and the count dword, as at `cob+0x1C` | one per unit, in the WIKISIM's heap | the engine's object, at `cob+0x1C` |
| statics | `int32[nstatics]` | idem | `*(cob+0x10)` |
| animation | `int32[npieces × 19]` | idem | `*(cob+0x14)` |
| model | the posed model's header and its `PrimitiveStruct`s from `+0x22`, stride `0x36` (`TAGPU_COBVM_MODEL_BYTES`) | a block per unit beside its COB object, read by the stage's pose composition | the posed model at `TAGPU_COB_MODEL_OFFSET` (`+0x540` of the retail object, which Impure relocates) |

The object's pointer fields (`+0x00` vtable, `+0x08` the script, `+0x10`, `+0x14`, `TAGPU_COB_MODEL_OFFSET`) are
**not** read by the core: a 64-bit host (tacob, the mockup's server) cannot store a pointer in a
32-bit field. The core takes the four blocks and the program as arguments; the host keeps whatever
pointers its world needs. Static asserts pin every offset against `tagpu_cob_safe.h` and the engine
map.

The record count is a parameter, `TAGPU_COB_THREADS` (10) for Impure and 8 for the retail fixtures
the core is checked against (the stock-limits comparison build keeps eight as well); the stride is
always `0x224`. A retail thread past 32 stack words writes into the next record; no stock script
reaches 12 words, and the core faults at 128 like the guarded game does.

### What it does

The runner `0x4B0DA0`, the stepper `0x4B1C00` and the engine's entries — `0x4B0940` (no arguments),
`0x4B0B00` (four argument words, `sp = argc − 1`), `0x4B0C40` (the query: four words in and out,
run inside the call), `0x4B0D60` (`DoScriptsNow`, `dt = 1`) — exactly as tacob's VM models them
today, opcode for opcode. Before every opcode it calls **`tagpu_cob_check_step`**, the guard the game's
adapter calls at `0x4B0E5D`: one bound for both, and a fault is the core's refusal (decision 9).

### The host interface

```
reads (callbacks, answered from the host's world; TAGPU_COBVM_HOST in tagpu_cobvm.h):
    get(id, a, b, c, d)          GET_UNIT_VALUE and GET: every id under RETAIL; under IMPURE
                                 only those tagpu_cob_getter leaves to the stock handler
    world                        IMPURE, required: the units, the caller, the alliances and the
                                 local seats tagpu_cob_getter answers the port-F ids from. The
                                 core calls it itself (the function the game's cob_get calls)
                                 and fills the piece counts from its program
    rand(n, lo)                  RAND's draw; the host owns the generator (lo only for replays)
    cargo(id), carrier()         0x10044000 and 0x10045000 (vt+0x48, vt+0x4C)
    alive(id)                    IMPURE: an ATTACH or DROP naming a dead unit is skipped
    callback(cb, value)          a RETURN with a completion object, a refused 0x4B0B00 start
the object's own state (written directly):
    records, statics, animation, the model's header words and PrimitiveStructs
everything else (appended to the object's effect log, with the record and the pc):
    SET id value, EMIT_SFX piece type, EXPLODE piece flags, ATTACH_UNIT, DROP_UNIT. (vt+0x28
    and vt+0x2C, the other two slots an opcode reaches, are bare returns.) EXPLODE without flag 0x20 draws the sim RNG six times
    (0x48116F..0x4811B9) as it commits, and EMIT_SFX emits only for a unit in the viewed player's
    sight (0x465AC0): both are the host's to reproduce when it commits.
an observer (optional):
    the cobtrace oracle's events — S, R, K, D, X lines (tacob-design.md, *The trace contract*)
```

**The effect log** is a bounded array the host gives the core with each run; the core appends,
never reads it back. The serial hosts (the wiki, tacob, the game's first host) commit it **after
each opcode**, so a `set` is visible to the next `get` exactly as in the engine. A parallel host
gives each worker its objects and their logs and commits the logs in the engine's serial unit order;
its `get` answers from the tick's start. That host — and the draw order of `rand` across units run
in parallel, which is its hardest problem — belongs to the swap program, not to this one. The core's
obligation now is only that it never needs anything else.

Thread safety is by construction: no global, no `static` that is written, no TLS.
`tools/test_cobvm.py` checks it with `nm` on both objects the core is built from,
`tagpu_cobvm.o` and `tagpu_cob_safe.o`; the DLL's build does not check it yet, because neither file
is marked `sim` (*Gaps*, below).

## Drift control

- **Stock behaviour** is frozen in a 1997 binary; the cobtrace logs of the real engine are its
  oracle. The nine existing fixtures (`research/notes/evidence/cobtrace/`) replay through the core
  byte for byte, and their posedumps match piece for piece — tacob's landing-3 gate, on the C core.
- **Impure's extensions** — the record count, the stack bound, the guarded faults, the getters — are
  each one engine-free function in shared C, called by `tagpu_patches.c`'s adapter and by the core
  (the getters under the core's IMPURE rules, before its host's own handler). An extension cannot
  be added to one and not the other without a function signature changing. The replay test group
  `cob-getters` judges the game's getter answers against the engine's own state, and binds
  `tagpu_cob_safe.c` so that any change to the rules runs it.
- **A landing that adds a COB feature to the game** adds a cobtrace fixture that exercises it. Until
  the core matches, that fixture fails by name.
- **The swap program's entry gate** is the recorder as oracle: load a recorded COB object into the
  core, step it, compare the next recorded tick byte for byte. It needs the host reads recorded too,
  which is recorder work, so it is not this program's.

## The wiki host

The calls (decision 8), each with the engine's entry, `runNow` and point of the tick, as the
engine map's *The scripts a unit's work starts* establishes them (DISASSEMBLED 2026-10-28):

| The wiki's action | The engine's calls, as both simulations make them |
|---|---|
| a unit appears: a scene's spawn, a nanoframe placed | `Create` (`0x485DE6`, run-now); for an extractor (ExtractsMetal above 0) `SetSpeed` with one word, Σ (cell metal + 1) over the footprint cells on the map, kept as a sign-extended 16-bit word (`0x437840`, `0x437866..0x437902`, run-later), at the create and so at the nanoframe's placement — ExtractsMetal does not enter the value; for a unit made whole whose FBI has ActivateWhenBuilt, `Activate` through the state setter (`0x48612F`) |
| a mobile builder's build, assist or reclaim | its work on a target starts only once its INBUILDSTANCE reads set (the stance wait `0x438700`, read once at the work's start: MobileBuild's work state `0x403E0C` and the step `0x41BA60` never read `+0x10F`), and goes on whatever the stance does after; `StartBuilding` with one word when its work target changes (`0x4385C7`, run-later; the word is the target's bearing relative to the builder's heading, 0 in the wiki, which turns the builder at once); `StopBuilding` with none when its work ends or changes (`0x43A251`, run-later: the order's destructor, in the same call as the step that finished the build), and at input time when the player takes the order away; `QueryNanoPiece` run-now (`0x43E400`, which asks it at `0x43E427`) on every tick the builder's work is credited, a reclaim's every second tick (MobileBuild `0x403E56`, ReclaimUnit `0x4049AD`; *The spray*) |
| a factory's build | `StartBuilding` / `StopBuilding` through the state setter, with no word (`0x48B14E`, `0x48B169`): no entry has a factory, so neither simulation makes them |
| a move starts, ends | `StartMoving` then `MoveRate1` / `StopMoving` (`0x43DAF2`, `0x43DB27`, run-now, in the movement pass after the unit's orders); the class thresholds are FBI `moverate1`/`moverate2`, by default 2 × MaxVelocity, which the wiki reads from no FBI, so its units, walking at MaxVelocity, are always class 1 and never start `MoveRate2`/`MoveRate3` |
| a structure finishes, is switched on or off | `Activate` on completion when its FBI has ActivateWhenBuilt (`0x41B8D0`, `0x41B9A6`); `Activate` / `Deactivate` (`0x48B106`, `0x48B12B`, run-later) when a script's `SET ACTIVATION` changes the value. The player's on and off (FBI `onoffable`) is no wiki action |

The wind generator's `SetSpeed`/`SetDirection` (`0x43795E`, `0x43798A`) is not made: the wiki has
no wind.

`get` is answered from the facts the simulation hands the host (below): the values the unit's
script `set`; HEALTH 100 (the wiki has no damage); PIECE_XZ the unit's own point; UNIT_XZ another
unit's, 0 for id 0 or a unit not alive, as the engine's (`0x480868..0x4808A4`); XZ_ATAN without
the unit's heading, which the facts do not carry; ATAN by the engine's formula (`0x4B715A`) from
an atan of + − × ÷ alone, so that every compiler gives the same answer, which can differ from the
engine's x87 `fpatan` by one unit at a rounding boundary; the hypots as the square root of the
sum of squares, truncated as `_ftol` truncates; BUILD_PERCENT_LEFT from the
fraction still to go (`tagpu_cob_build_left`); PIECE_Y, UNIT_Y, UNIT_HEIGHT and GROUND_HEIGHT 0 on
the flat stage; ids 32 and 69–75 `tagpu_cob_getter`'s over the host's units; any other id 0.
`rand` draws from a xorshift32 generator the host owns and hashes, never the engine's. The host's
state is part of the simulation's state hash, so `simparity.mjs` checks the calls both
simulations make as well as everything else.

**One host, in C, for both simulations.** Everything the host decides about a script -- which
program a type runs, the four blocks of each unit, what `get` answers, what a `SET` changes, the
generator `rand` draws from, a refusal and its quarantine, the state hash of it all -- is one
engine-free file, `tagpu_wikicob.{c,h}`. The DLL's `tagpu_wikisim.c` calls it in-process; the
mockup's `sim.js` calls the same file through the mockup's server; `wikitest.c` links it for the
parity check. What the two simulations still each implement is only *when* they call it: the
actions of decision 8, made at the same points of the same tick, which `simparity.mjs` compares
through the state hash (the host's hash is part of it). So the host cannot drift between the DLL
and the mockup any more than the core can.

**The tick.** The engine's unit turn is its script slice, then its orders, then its movement
(`0x48ADEB`, `0x48AF98`, `0x48AFAA`, the engine map's unit tick); the wiki's tick keeps that order
for all its units at once, in creation order (`tagpu_wikisim_step`, `sim.js` `step`):

1. the inputs of the tick, unless a demo's wait holds its clock (*The demo's wait*, below); then
   any work whose order the player took away ends (`drop_work`: StopBuilding, as the order's
   destructor starts it when the input is handled). Try it's inputs between two ticks do the same;
2. **the slice**: `DoScriptsNow(1)` for every unit with a script, each `SET` committed into that
   unit's values as its opcode runs (a `SET ACTIVATION` that changes the value starts `Activate`
   or `Deactivate` at the end of that VM call, so it first runs at the next slice);
3. **the units' work** (`advance`), which reads the committed values: a builder in range takes its
   job, but credits work on it only once its INBUILDSTANCE has been set since it took that work,
   and is not read again until the work changes; a structure finished this tick whose
   FBI has ActivateWhenBuilt is switched on at once; a reclaimed one leaves the host;
4. **the calls of the units' turn**, unit by unit in creation order: a builder whose work target
   changed (a new job, or its job finished, reclaimed or gone) starts StopBuilding for the old one
   and StartBuilding for the new one, both run-later; then a mobile unit that stepped this tick
   after standing starts StartMoving and MoveRate1, and one that stood after stepping StopMoving,
   both run-now. Between the two, a builder whose work was credited this tick sprays (*The
   spray*). A run-now start runs every record with `dt = 0` inside the call; a run-later one
   waits for the next slice;
5. **the particles' walk**: every live particle moves a step and dies once past its end, then each
   emission's second burst due this tick is made (*The spray*).

A builder's work is identified by its target's id and by the `serial` of the order it came from
(`WIKIORDER.serial`, `sim.js`'s order object), so an order taken away and replaced by an equal one
still ends the old work.

A unit's `get`s are answered from its **facts**, which the simulation hands the host with every
call that can run script code (the slice, a run-now start): every unit's id, owner, position,
build fraction still to go (as the float the engine's `+0x104` holds, so `tagpu_cob_build_left`
encodes it) and health. Both simulations build the facts from the same doubles with the same
truncations, so the host sees the same words from either.

**The mockup's transport.** `sim.js` stays synchronous: its context takes a host object
(`ctx.cob`) whose calls return their answers. In the page the host is a synchronous request to the
mockup's server, `tools/wiki/serve.py` (the repository served as `tools/hudart/serve.py` serves it,
plus `POST /cob`), which loads the host and the core through ctypes, built by tacob's native
builder. Under node (`simcheck.mjs`, `simparity.mjs`) the host is `tools/wiki/cobhost.mjs`: a
worker thread that talks to `serve.py --stdio` while the caller waits on an `Atomics.wait`, so the
checks call it synchronously as the page does; `forPack` takes every type's script from one gfx
pack, `forPacks` each from the first of several that holds it. Without a host (`ctx.cob` absent,
`WIKISIMCTX.cob` NULL) no unit runs a script and every gate reads open, as for a quarantined type
(below), so the rule cases that test no script play no script. The page
(`tools/wiki/mockup/cob.js`, `cobPage`) asks the host for the scripts of the gfx pack its stage
draws; served any other way, or with no pack built, the request fails and the page plays the demo
without scripts, saying why in the console, as it says a quarantined type's note.

**Where the scripts come from.** The DLL takes each type's script from the wiki's asset load,
which resolves `scripts\<UnitName>.COB` by the engine's rules (`tagpu_wikiasset.c` `unit_script`):
the bytes are kept in the arena with the posed model's primitive count and each primitive's
visible flag (`WIKI3DUNIT` `script`, `nscript`, `pieces`, `visible`). The posed order is
`tagpu_cobscan_posed`: the engine's order for a new unit (`0x45AEC0`), then `0x45A950`'s swap for
every COB piece below the primitive count; a primitive is visible with three or more vertices
(`0x45AF1B`). The load's report carries it as `posed`, which `tools/wiki/assetcheck.py` holds equal
to its Python reference and to `gfx.py --resolve-json` (`gfx.py` `posed_order`). The mockup's packs
carry the same bytes: `gfx.py` writes each unit's script beside its mesh with its posed order and
visible flags (`units/<type>.cob`, the mesh's `script`; gitignored, like everything derived from
the game), which `serve.py` reads, and `wikitest.c` takes a list of the same files (`demos … COB`,
`trace FILE COB`).

**No demo starts before the scripts' answer.** The 3D load publishes a three-state answer,
`tagpu_wiki3d_answer` (`WIKISIDE_PENDING`, `_READY`, `_REFUSED`, by one interlocked store, as the
sidebar's), and `tagpu_wiki.c` `scripts_now` makes a demo, Try it and the parity check
`tagpu_wiki.check` wait for it as they wait for the layout: a demo's units have their scripts
from tick 0 or never. A refused or never-started load plays every unit without a script.

**The quarantine** (decision 9): a type whose script the program validator refuses, or one whose
unit's VM faults, is quarantined for the simulation's life -- every unit of it drops its VM, stands
in the pose `Create()`'s hides give today, reads every gate as open (INBUILDSTANCE, ACTIVATION,
YARD_OPEN set). A type with no script, or whose model gives no pieces, is quarantined the same
way at the host's making. The host keeps the type's first refusal as its note
(`tagpu_wikicob_note`): the call that met it -- the program, `Create`, a script by name, the
slice, or "no script" -- and the reason the core gave. The game's check logs it with the type's
name (`wiki check: <entry>/<demo>/<side>/<iface> <TYPE>'s script is quarantined - <note>`), the
page's console prints it, and `simcheck.mjs` fails a play with scripts on any note. A quarantine is
part of the state hash, so both simulations must quarantine alike. The pc is not in the note,
where decision 9 asks for it.

**The demo's wait** (decision 10). A demo input `{"at", "until": "idle", "max"}` holds the demo's
**clock** -- the tick its inputs, trips and `end` are written in -- at `at` until no unit of the
player's holds a build or reclaim order, at most `max` ticks (an integer 1 to 3600,
`WIKI_UNTIL_MAX`, `sim.js` `UNTIL_MAX`); one tick past it the demo stops, `stalled`. The wait
carries no other key and shares its tick with no other input; both simulations refuse a demo that
breaks either (`sim.js` `make`, `tagpu_wikidata.c` `input` and `tagpu_wikisim_init`). The
simulation's own tick runs on under a wait. The view gains `clock`, `waiting` (the state a wait
holds the clock for), `stalled` and `cob` (the host's hash halves), and the state hash appends the
clock, the wait, `stalled` and the host's hash (`tagpu_wikisim.h`, item 8).

- **Every Quick Build demo ends on its work done**: after its last input `gen-quickbuild.mjs`
  writes a wait (`WAIT_MAX` 1800 ticks) and then 60 ticks of tail (`TAIL`); *The best extractor
  that fits* also waits between its two clicks. How long a side's builder takes -- its walk, its
  script's stance, the build -- is then the demo's own, and no tail is sized for the slowest mod.
- **The Build Lines demos keep fixed tails.** With the scripts, stock's ARMCK takes about 268
  ticks a queued piece: its script finishes closing its nanolathe (about 5 s) before it opens it
  again (about 2.6 s) on every StopBuilding and StartBuilding between two pieces, so waiting for a
  whole line would run for minutes. The tails of *Click and Shift-click*, *Filled rectangle* and
  *Ring around a building* are 300, 480 and 420 ticks, long enough for the pieces their claims
  read finished, and `simcheck.mjs`'s rectangle claim needs two pieces built in order with the
  scripts and six without.
- **The scripts run at game speed while the builds run at the entry's pace** (`"pace"`), so a
  script's animations take their game time while a build fills N times faster: Quick Build's
  *Queue several with Shift* plays 1054 ticks with the scripts and 570 without (MEASURED
  2026-10-28).

**Drawing** (L3b). The stage draws units through the game's posed program with a pose array per
piece (`tagpu_wikiworld.c`, 12 floats a piece, with per-piece visibility and shade), and fills it
from the PrimitiveStructs the core writes, composed the way `tagpu_native.c` `posed_pose` composes
the engine's:

- **The game thread copies the poses into the record.** After the view's copy, the page fill
  (`tagpu_wiki.c` `page_fill`) calls `tagpu_wikistage_pose`, which takes each copied unit's posed
  model from the simulation (`tagpu_wikisim_model`, over the host's `tagpu_wikicob_model`: NULL for
  a unit no script poses) and copies three fields of each primitive -- the MOVE delta in 16.16, the
  TURN about x, y, z and the flags (`TAGPU_COBVM_VISIBLE`, `_CACHED`, `_SHADED`) -- into the stage
  view's pose pool, `WIKISTAGEPIECE pose[WIKISTAGE_POSE]` (4096 primitives over all the units),
  each unit naming its run by `pose_at` and `pose_n`. A unit whose model has more than
  `WIKISTAGE_PIECES` (256, WIKI3D's bound) primitives, or does not fit what is left of the pool,
  keeps `pose_n` 0 and draws at rest, and the fill sets `WIKISTAGE_TRUNC_POSE` (`0x200`), logged
  once a demo as `wiki: the demo <entry>/<demo>'s view did not fit the record (cut 0x200)`. The pool
  rides in the frame packet with the rest of the view, by value.
- **The 3D load maps the two piece orders.** The stage bakes and draws a type in the readers' walk
  (`tagpu_model_walk`), the core holds primitives in the posed order; `unit_script`
  (`tagpu_wikiasset.c`) records for each of the walk's `nwalk` nodes the posed primitive it is,
  `posed_of[g]`, -1 for a node the posed order does not hold.
- **The render thread composes them** (`tagpu_wikiworld.c`). `pose_of` takes a unit's run from the
  packet as data: never for a ghost; the record's slot below the view's `nunit` and
  `WIKISTAGE_UNITS`; `npose` at most the pool; `pose_at` and `pose_n` inside `npose`; `pose_n`
  equal to the type's primitive count and the type's run the `nwalk` nodes the map was made
  against. Anything else draws the rest pose. `pose_compose` then builds `posed_pose`'s tree,
  P = parent · T(rest offset from the parent + MOVE) · R(TURN), each entry of `posed_of` bounded by
  the run's count; at the root the stage's facing matrix stands for the body turn, about the root's
  rest point, with the root's MOVE outside it, so a script that poses nothing draws the rest pose
  to float rounding (the product chain and the rest path's single product can differ in the last
  bits). A node the posed order does not hold, or whose parent never resolves, keeps the rest pose
  turned onto the facing and the rest path's visibility. The words are `posed_pose`'s: hidden (flag bit 0 clear) an all-zero
  matrix and visibility 0, visible and cached 3, visible and dont-cache 1 (no slant), and a piece
  shaded by bit 2 once any visible piece carries it.
- **One arithmetic.** `rot2` (the engine's pair rotator `0x4B7173` without its rounding),
  `m43_mul` and `piece_local` are `tagpu_posemath.h`'s, pure functions that `tagpu_native.c` (the
  game's units) and `tagpu_wikiworld.c` (the stage's) both include.
- **The mockup does the same.** `gfx.py` writes every piece's faces, `Create()`'s hides included,
  each vertex's piece (`units/<type>.pieces.bin`, u16: the body's vertices, then the slant's, then
  the wire's) and the mesh's `pieces` table in the posed order (parent, cumulative rest offset in
  the file's axes, hidden at create, dont-cache); `serve.py`'s `models` op answers every unit's
  model block, `cob.js` `model()` asks once after each call, `sim.js` `poses()` gives them aligned
  with the view's units, outside the view and the state hash, and `pose.js` composes them as
  `pose_compose` does, in float32 step for step; `gfx.js` draws a posed unit from them and every
  other unit, and every ghost, at rest. Its bodies and shadows now cull back faces as the game's
  body pipeline does (`tagpu_vk_unit.c`, *BACK FACES CULLED*): a turned piece shows the face that
  now runs clockwise on the engine's screen, and the opened ARMCK's shell panel showed its grey
  side without it.

## The spray

L4a. A builder whose work is credited sprays the engine's nano particles from the piece its
script names into the structure it works on, in both simulations, as simulation state, and the
stage draws them. The engine's side, every fact with its address: the engine map's *The nano
spray: two emitters, fifteen order handlers* and *The nano particle*.

- **When.** The engine's step-gated build handlers emit only on a tick their step returned 1
  (MobileBuild `0x403E43` → `0x403E56` → `0x403ECC`, HelpBuild `0x404139` → `0x4041C2`); the
  wiki's analogue is its `credit`, which adds work only past the stance gate, so `credit` records
  the spray (how, and the structure's type and centre as they stand then: a reclaim may remove
  the structure before the unit's turn). The spray is made in the unit's turn (*The tick*, step
  4), after its build calls. A reclaim sprays in reverse, from the box to the piece, and only every
  second tick from its first spray on that work (ReclaimUnit's state 5 re-arms itself two ticks
  on, `0x404A57`), its finishing tick included; the cadence is kept per structure (`spray_at` and
  `spray_on`, `sprayAt` and `sprayOn`), so a new work sprays at once, and it is forgotten after the
  turn the work ends in, or when the player takes the order away.
- **QueryNanoPiece on the engine's schedule.** ARMACK's `QueryNanoPiece` and the ARM factories'
  flip a static to alternate two pieces (the retail scripts, decompiled), so the call writes COB
  state and the host's hash depends on how
  often it runs: both simulations ask it exactly once a spray, run-now, through
  `tagpu_wikicob_query` (`0x4B0BC0` by name, entered through `0x4B0C40`), the piece preset to 0
  as `0x43E400` presets it; a script without the query, or a unit with no VM, leaves the 0.
- **The piece's origin, from the host.** `tagpu_wikicob_origin` is `0x43DEF0`'s arithmetic on the
  unit's posed model as its script left it: the piece's offset plus its MOVE, carried up its
  parents, each parent's TURN applied through `0x4B6CC0`'s order before its own offset plus MOVE,
  the piece's own TURN never; each pair rotation `0x4B7173`'s (skipped for a zero angle, rounded to
  the nearest, ties to even, as its `fistp` rounds), with sine and cosine from + − × ÷ alone after
  an integer reduction to the octant, so every compiler gives the same words. It answers in two
  parts: `base`, the root's offset plus MOVE, which the engine does not turn by the body, and
  `turned`, the rest turned by the root's TURN, which the engine turns by the body's angles folded
  into the root's (`0x43DF86`). The host holds no facing, so the simulation turns `turned` by the
  unit's facing, `(cs, sn) = (−hz, hx)` about the up axis as the stage turns the model (a type
  that does not move at the heading (0, 1), as `tagpu_wikiscene.c` `model` draws it), adds
  `base` and the unit's position, and takes z off, as the engine negates it (`0x43E00A`). A piece
  outside the model (the script's answer is data, bounded by the primitive count) answers the
  unit's own point, as `0x43DF16..0x43E01D` does. **The model's shape** the host needs is a new
  part of `WIKICOBTYPE`: each primitive's offset from its parent in the axes the engine holds a
  model in (the loader `0x4CB590` negates X and Z), its parent, and `top`, `0x4CB5F0`'s height of
  the model, which the target box rises to. The DLL's 3D load computes it from its arena
  (`tagpu_wikiasset.c` `unit_shape`, `model_top`), `gfx.py` `model_shape` from the 3DO for the
  packs, and `wikitest`'s and `wikistagetest`'s `cob.json` carries the pack's
  (`tests/wikicobfile.h`); `serve.py` hands it to the host (`new` answers each type's `top`;
  `query` and `origin` are new ops), `cob.js` and `cobhost.mjs` carry it. The host holds the
  parents to a tree when it is made: a parent outside the model, or a chain that does not end
  within the primitive count, leaves the whole model flat, every piece at the unit's point.
- **The particles**, `0x4720D0` / `0x472200` and their class `0x4FD5B8` repeated in int32
  (`tagpu_wikisim.c` *THE NANO SPRAY*, `sim.js`'s spray functions, line for line): the source
  box the piece's point, the target box the structure's centre ± FootprintX and FootprintZ × 8
  world units by the ground to `top`, each cut to its middle 4/11 … 7/11 on every axis
  (`0x473B50`); an emission is two bursts of five (`0x473D50`), the second on the next tick after
  that tick's moves and only when the first made a particle; each particle runs from its box1
  point to its box2 point in `(int16)(len >> 18)` steps of the distance over the steps, toward
  zero, a particle of 0 steps not made; its end the burst's tick plus the steps; its colour
  `0xA1 + i % 7`, cycling `0xA1..0xA7` as it moves (`0x4739B0`); it dies once the tick is past its
  end (`0x473B30`). The walk (`spray_tick`, `sprayTick`) runs after the units' turns, as the walker
  `0x471EB0` runs after the unit tick.
- **The wiki's generator, never the engine's and never the host's.** Each of a particle's six
  draws is a stateless counter hash, 0..`0x7FFF` as `rand`'s: lowbias32 over the emission's key
  (FNV-1a of the builder's id, mixed with the emission's place among the tick's emissions), the
  tick, the particle and the draw (`spray_draw`, `sprayDraw`). It keeps no state, so nothing about
  it can drift between the twins, and it never touches the host's xorshift `rand`, which RAND
  draws from and the host's hash carries: a spray changes no script's random numbers (decision
  11).
- **Simulation state, in the hash.** The live particles are `WIKIVPART` (x, y, z, their step, the
  end, the colour) and the waiting second bursts `WIKISPRAY` (the two cut boxes, the due tick, the
  key); the view carries both (`npart`, `part`, `part_cut`, `nburst`, `burst`; `sim.js` `parts`,
  `partsCut`, `bursts`) and the state hash appends them after the host's hash, as item 9
  (`tagpu_wikisim.h`), so `simparity.mjs` holds them equal tick for tick, a waiting burst on the
  tick it diverges.
- **Bounded.** At most `WIKI_SPRAY_PARTS` = 1024 particles live and `WIKI_SPRAY_BURSTS` = 256
  second bursts wait, on the heap, allocated only with a host; a particle or a burst past its bound
  is not made and the view says so (`part_cut`), which the stage's copy turns into
  `WIKISTAGE_TRUNC_SPRAY` (`0x400`), logged once a demo (`cut 0x400`). `sim.js` holds the same
  bounds, so both drop the same particles. The busiest demo, Quick Build's *helpers*, peaks at 457
  live particles, and no demo drops one (MEASURED 2026-10-28, a one-off count over every demo
  with the stock pack's scripts).
- **Drawn as the engine draws a particle, by the stage's own ops.** The page fill copies the
  particles into the stage record (`WIKISTAGEPART`: x, y, z and the colour; `WIKISTAGE_PARTS` =
  the sim's pool, 16 KB of the record), and `tagpu_wikistage_build` draws each as one 2 × 2
  game-pixel dot -- `DrawBar 0x4BF6F0`'s inclusive rect {sx, sy, sx + 1, sy + 1}, its fill
  `0x4CCDEA` -- at sx = x >> 16, sy = (z >> 16) − ((alt >> 16) >> 1) (`0x473A00`), in the stage's
  op range over the image, so above the units, as the engine's layer 6 is drawn after its row
  sweep. A dot is drawn only where the wiki's fog cell under it is clear and never off the map; a
  colour outside `0xA1..0xA7` is not drawn. `stage.js` draws the same, and `stagecheck.mjs` pairs
  them op for op. The colours are the retail palette's ramp, as RGB constants in both.
- **Only with the scripts.** Without a host (`ctx.cob` absent, `WIKISIMCTX.cob` NULL) no unit
  sprays: the pool is not allocated and `sim.js`'s turn returns before the spray.

## The landings

| | What | Gate |
|---|---|---|
| **L1** — built 2026-10-27 | `tagpu_cobvm.{c,h}`: the core on the layout above, the host interface, the effect log, the observer; the shared extension functions factored out of `tagpu_patches.c`; tacob on the core through ctypes, **its Python VM deleted**, `tacob-build.py` cross-compiling it; the reentrancy check | the nine fixtures byte-exact through tacob on the core; `test_tacob.py`; native tests of the core under UBSan and ASan; the layout's static asserts; the DLL builds |
| **L2** — built 2026-10-27 | live cobtrace + posedump fixtures for the wiki's units doing the wiki's actions — ARMCK and ARMACK walking, starting, assisting and stopping a build, reclaiming; ARMMEX and ARMMOHO built, activated, spinning at a set metal value; ARMGEO activating; ARMPW walking — and one exercising the extensions (records 9 and 10, the getters) | each fixture byte-exact through the core |
| **L3a** — built 2026-10-28 | the host (`tagpu_wikicob`), the calls and the gating in both simulations, the facts, the quarantine, `until` waits and the 11 Quick Build demos converted, the posed order in the DLL's asset load, the three-state 3D answer the demos wait for, `activatewhenbuilt` through every reader, the mockup's server and node host, the scripts in the packs | `simparity.mjs`, `simcheck.mjs` (every demo's claim with the scripts running), `wiki_regression.py` |
| **L3b** — built 2026-10-28 | poses drawn: the core's PrimitiveStructs copied into the stage view's pose pool on the game thread and composed over the posed order on the render thread (`tagpu_wikistage_pose`, `pose_compose`, `tagpu_posemath.h`); in the mockup `gfx.py`'s piece tables, `serve.py`'s `models`, `sim.js` `poses()`, `pose.js`, and back faces culled as the game culls them (*Drawing*) | `stage3dcheck.py` (the game's stage against the mockup's, with the poses), `posecheck.py`, `simparity.mjs`, `simcheck.mjs`, `scenecheck.mjs`, `stagecheck.mjs`, `sidecheck.mjs`, `textparity.mjs`; the demos watched in game |
| **L3c** — built 2026-10-28, one refusal open | the corpus gate, `tools/wiki/cobcorpus.py`: every compatibility setup's unit types through the host (Create, 600 slices, the wiki's calls and a state-setter Deactivate/Activate), a refusal named | the gate itself, clean over every setup: clean but for ZZZ (below) |
| **L4a** — built 2026-10-28 | the nano spray: `QueryNanoPiece` on the engine's schedule and the piece's origin through the host (`tagpu_wikicob_query`, `tagpu_wikicob_origin`, the model's shape in `WIKICOBTYPE`), the engine's emitters and particle class repeated in both simulations as state in the hash, the wiki's own generator, the particles bounded and copied into the stage record, drawn as the engine's 2 × 2 dots (*The spray*) | `simparity.mjs` natively and `--wine`; `stagecheck.mjs`'s scripted lane; `tools/test_cobvm.py`'s query and origin tests; the host's origins against `pose.js` |
| **L4b** — planned | `emit-sfx`: the engine's EMIT_SFX emitters (`0x480EB0`: fire `0x472330`, the wakes `0x472430`, bubbles `0x472530`, the smokes `0x472810` and `0x4728F0`) from the effect log the host now drops | the effect drawn against a game capture of the same script |
| **L5** | sound: the mixer, the triggers, the cooldowns, WebAudio in the mockup | a demo heard in game and in the mockup; the player's settings honoured; an MP game's own sounds unchanged with the wiki open |

L1 and L2 touch `tagpu_cobvm.*`, `tagpu_cob_safe.*`, `tagpu_patches.c`, `tools/tacob*` and the
fixtures — no file the sidebar's landing touches.

**L3a's gate**, MEASURED 2026-10-28 on the reference setup:

- `simparity.mjs`: every demo played through both simulations with the scripts and without
  (1008 plays) and the 47 rule cases equal tick for tick, natively (AddressSanitizer and
  UndefinedBehaviorSanitizer) and the i686 build under Wine; `--hash --scripts` prints each
  demo's hash with the scripts, `--try` takes a dump's `ctx.cob`.
- `simcheck.mjs`: every demo and rule passes with and without the scripts (the `+scripts`
  contexts), with and without the sidebar, and on all six mod packs (stock, ProTA, Mayhem, TA
  Zero, Twilight, Escalation) with each mod's own scripts (`--pack` takes the mod's stage packs
  first); no wait ran past its max and no type was quarantined.
- In game (`wiki_regression.py <instance> wikicob demos --launch`, a private Xvfb at 1920 × 1080):
  `wiki check: 84 demos played, 0 did not start` in 89 ms with the units' scripts on, all 84
  hashes (both sides, both Interface Types, stock castings) equal to `simparity.mjs --hash
  --scripts`; Quick Build's *Queue several* and *The best extractor that fits* at 30.0 ticks a
  second at 1×, 60.2 at 2×; the page cost the game thread at most 0.358 ms in a frame over 1222
  frames. `try --launch` passed (166 s); `asset --launch` passed, the load in 245.8 ms, an arena
  of 594 049 bytes, 18 of 18 unit types resolved, the report equal to the native load's.
- The host's slice over a demo's units costs about 70 µs (2 ms for 30 slices).
- `tools/test_cobvm.py`'s `WikiHost` tests: no writable static in the host, two hosts on two
  threads, the quarantines, `SET ACTIVATION` through the setter's rule.

**Why L4b waits.** No stock unit the wiki shows reaches EMIT_SFX: of the 278 retail scripts
(decompiled with tacob), the wiki's units' only `emit-sfx` is in `SmokeUnit`, which emits only
while `get HEALTH` is below 66, and the wiki's HEALTH is always 100 (it has no damage). The
other users are ships, hovers and submarines (their wakes and bubbles), aircraft, and a few
structures' `Activate`. And the fire, wake and bubble emitters' particle counts, lifetimes and
velocities are not established (the engine map, *The demo recorder's counts and the step's
phases*, its COB `EMIT_SFX` entry).
L4b starts when the wiki shows a unit that emits.

**L4a's gate**, MEASURED 2026-10-28 on the reference setup:

- `simparity.mjs`: all 1008 demo plays and the 47 rule cases equal tick for tick, the particles
  in the state hash, natively and with `--wine`.
- `stagecheck.mjs`: its scripted lane (the demos with the fixture's sidebar at the first size and
  the units' scripts, `sim.js` through `serve.py`'s host, the C side from the same files through
  `tests/wikicobfile.h`), 447 frames, the spray drawn in 53 of them, 8325 particles, paired op for
  op; all 6550 frames equal.
- The host's origin against the stage's composition: every visible piece of every scripted unit
  of Quick Build's and Build Lines' demos, every seventh tick, 58 529 origins, turned by the facing
  as the simulation turns them, within 0.00003 world units of `pose.js`'s (a one-off script, not
  in the tree).
- `tools/test_cobvm.py`: `QueryNanoPiece` answers its piece and a missing one leaves the preset; a
  piece's origin is carried up its parents turned; a piece outside the model and a broken parent
  chain answer the unit's point.
- `simcheck.mjs` (every pack under `--pack`), `scenecheck.mjs`, `sidecheck.mjs` and
  `textparity.mjs` pass.

**L3b's gate**, MEASURED 2026-10-28 on the reference setup:

- `stage3dcheck.py` over `wiki_regression.py stage3d`'s 18 captures, the mockup drawing the poses:
  all 18 within the owner's tolerances. A Build Lines capture at tick 330 needed the mockup's back-face
  cull: without it, the opened ARMCK's shell panel was grey where the game shows its
  team-coloured face.
- `posecheck.py`: on the stock pack's 9 types (ARMCK, ARMDRAG, ARMLLT, ARMSOLAR, ARMACK, ARMGEO,
  ARMMEX, ARMMOHO, ARMPW), 6 random poses each (a MOVE of up to 8 units and any TURN on every piece,
  a random facing), every body vertex of `pose.js` within 0.000076 model units of tacob's
  `_compose` (the tolerance is 1/1024); the start pose composes to the rest pose within 1/65536
  (`pose.js`, measured), and a
  hidden piece leaves the body and the wire, an uncached one the slant.
- `gfx.py`'s rest arrays byte for byte what the packs held before every piece was carried, on the
  stock pack and the six mod packs.
- `simparity.mjs` 1008 plays equal; `simcheck.mjs`, `scenecheck.mjs`, `stagecheck.mjs`,
  `sidecheck.mjs` and `textparity.mjs` pass; the demos recorded in game on video.

### Gaps open

- **`posecheck.py` covers the stock pack only.** The mod packs' types are drawn by the same code,
  and `stage3dcheck.py` compares the game with the mockup on retail's captures; no check holds a mod's
  posed models to tacob's composition. It is not in `wiki_regression.py - offline` either: it runs
  by hand.
- **The pose pool is bounded at 4096 primitives a view** (`WIKISTAGE_POSE`), filled in unit order;
  a unit past it draws at rest, logged once a demo (`cut 0x200`); `sim.js` `sim.poses()` applies
  the same bound, so the mockup draws the same units at rest. The most pieces of any type in the
  stock and six mod packs is 49, so 83 such units fit; no demo's unit count has been checked
  against the bound.
- **The poses and the spray ride every frame.** The pose pool (4096 × 20 = 81 920 bytes) and the
  spray's particles (1024 × 16 = 16 384, of which the fill writes the live ones) ride in the
  record at every fill the stage is shown, whatever changed (the record is a fixed 162 328 bytes
  on the i686 build), and the render thread
  composes every posed unit every frame; neither cost has been measured.
- **A dont-shade piece's row is carried as a normal in the mockup.** `pose.js` gives each vertex of
  a piece without the shaded flag the normal (0, 3.1, 0), whose `emitUnits` row is
  trunc(3.1 × 5) = 15 at every facing, standing for the engine's default row 15 that
  `tagpu_posedraw.c` gives such a piece directly. A change to `emitUnits`' row rule changes it.
- **The corpus gate refuses one type, ZZZ** (L3c's run, 2026-10-28: 19 setups, 6 distinct
  corpora, 2 423 scripts, 22 s). `tools/wiki/cobcorpus.py [SETUP ...]` builds each setup's file view
  (`sidepacks.py` `setup_tree`, `gfx.py` `Engine`), resolves every unit type as `gfx.py` does, and
  runs it through its own host; setups with the same scripts run once. Types with no script (the
  engine allows one, `0x485DFE`) are set apart, not refused. The one refusal is the demo pack's
  CORE tower ZZZ, one 1 804-byte script in Mayhem, ProTA, TA Zero and Twilight: "piece index exceeds
  model allocation" -- its `Killed` EXPLODEs piece 3 (pc 289, 324, 362, 383) of a three-node model.
  Retail runs it and reads past the model's allocation when the unit dies [INFERRED from the engine
  map, not measured]; Impure's game refuses it at load by the same rule, and its own corpus driver
  records it as the one malformed-content quarantine of those four setups (`tadr-port/cob.md`). The
  wiki's core agrees with the game, so this is not a host bug; whether to bind per entry point, to
  fault only on the access, or to accept the type as malformed is open, and it is the owner's.
  `cobcorpus.py` exits 1 until it is decided.
- **GET answers what the wiki has**: PIECE_Y, UNIT_Y, UNIT_HEIGHT and GROUND_HEIGHT are 0,
  PIECE_XZ is the unit's own point and XZ_ATAN leaves out its heading (the facts carry none);
  STANDINGMOVEORDERS and STANDINGFIREORDERS are 0 (the engine reads `unit+0x110` bits,
  `0x4807A4`, `0x4807B7`, and the wiki has no standing orders); an angle can differ from the
  engine's by one unit at a rounding boundary. A `SET` keeps what `0x480B20` keeps (its byte
  table `0x480C18`): INBUILDSTANCE, BUSY and BUGGER_OFF the value's bit 0, ACTIVATION and ARMORED
  whether it is nonzero (`0x48B090`), YARD_OPEN whether it is nonzero [INFERRED: `0x47DAC0` not
  read], and every other id nothing (`0x480BF1`).
- **A `SET ACTIVATION`'s `Activate` first runs at the next slice**; the engine starts it inside the
  opcode, so a record after the current one may run it in the same pass [not established in the
  engine: the runner's walk `0x4B0DA0` was not read for new records].
- **The quarantine note carries no pc**, where decision 9 asks for one: it is `<what>: <reason>`,
  the call the refusal came in and the core's reason, because the core's run record
  (`TAGPU_COBVM_RUN`) keeps no pc for a fault. Giving it one is a change to the core's interface.

- **The spray's facing is turned in two steps.** The engine folds the body's angles into the
  root's and rounds each pair rotation once (`0x43DF86`, `0x4B7173`); the host rounds the root's
  TURN and the simulation then turns by its facing vector in doubles and truncates, so a nano
  point can differ from the engine's by about one 16.16 unit (1/65536 of a world unit)
  [INFERRED from the arithmetic, not measured against the engine].
- **The spray's colours are the retail palette's** `0xA1..0xA7` as RGB constants in both stages
  (`tagpu_wikistage.c` `SPRAY`, `stage.js`); a mod's own palette is not followed, where the game's
  `tagpu_fx.c` reads the palette in play.
- **The spray's sight test is the wiki's fog cell**, 16 world units, a grey cell hiding the dot;
  the engine asks the viewed player's sight at its 32-unit tile, the LOS counter with true LOS and
  the mapped bit without (`0x473A50..0x473B05`).
- **The engine's cap on spray objects is not modelled.** The engine's particle layer holds at most
  400 objects in stock (20 480 under the raised limits); past that, every emitter, the nano one at
  `0x47219F` among them, destroys the oldest object and appends anyway (*the ten particle layers*
  in the engine map), and each emission is one object of up to ten particles. The wiki bounds its
  own pool instead, 1024 particles and 256 waiting bursts (`WIKI_SPRAY_PARTS`, `WIKI_SPRAY_BURSTS`),
  a drop flagged and logged rather than the oldest evicted; the busiest demo peaks at 457 live
  particles (L4a's run, 2026-10-28), so neither bound is reached by the shipped demos.
- **The spray needs the scripts.** Without a host no unit sprays, so a demo played without the
  scripts, or in a check that plays none, shows no spray.
- **A pack written before the model's shape** (`gfx.py` before L4a) has no offsets, parents or
  height: every piece's origin is the unit's own point and the target box has no height. Rebuild
  the packs with `gfx.py`.
- **The spray was not compared with a game capture of the same build**, the gate the plan's L4
  row named.

- **The core's files are not `sim`.** `tagpu_cob_safe.c` holds rules the game's simulation runs
  (the step guard, the getters, the parent's pops, the attachment bound), but `thread-split.allow`
  does not mark it `sim`, so the replay gate checks no function of it for coverage, and the build's
  writable-static check does not reach it or `tagpu_cobvm.c`. The `sources` binding of
  `cob-getters` and `resource-core-upgrade` makes any change to it run those groups, and
  `test_cobvm.py` checks both objects for written statics. Marking them `sim` needs the program
  validator's `malloc`/`free` routed through the registry's wrappers, which the swap program does
  when the core becomes the game's interpreter.

## Research each landing owes

- **L1** (answered 2026-10-27): every vtable slot the opcodes reach, the flag bits and the model's
  header words — the engine map's vtable table. `0x10063000` is refused by the program validator,
  as the guarded game refuses it, **under both rules**: a RETAIL VM therefore refuses a script that
  the retail engine runs (`0x4B1984`). No stock script carries the opcode; the difference is
  accepted, not modelled. The getters are `tagpu_cob_getter`, which the core calls itself under
  IMPURE.
- **L2** (answered 2026-10-27, `research/notes/evidence/cobtrace/README.md`, classes 10–16): a
  scripted scenario per fixture, recorded in game under the run lock on Impure's raised build and
  replayed through the core on IMPURE rules, all byte for byte. What the recordings settled:
  `StartBuilding` from the build orders is `0x4385C7`, a `0x4B0B00` start with **one** word, the
  heading `& 0xFFFF` (the map's `0x48B14E` is the other, argument-free site in
  `UNITS_SetStateMask`), so tacob's director, which passes heading and pitch, is wrong and L3
  corrects it; a reclaim runs the same script traffic as a build, `StopBuilding` included;
  `QueryNanoPiece` runs after the unit's own script slice (its callers are the order handlers),
  which tacob's replay table had wrong. The extractor's `SetSpeed` value is in the traces (`2016`
  ARMMEX, `2352` ARMMOHO on the 223-metal patch), derived in L3.
- **L3** (answered 2026-10-28, the engine map's *The scripts a unit's work starts*, DISASSEMBLED):
  - **StopBuilding** of a mobile builder comes only from the order node's destructor
    (`0x43A251`), exactly when StartBuilding was started for that node: in the same controller
    call as the step that finishes a build; for an assist, in the tick the assister sees its
    target finished, whoever finished it, and **never** when the target was finished before the
    assist's StartBuilding; on a cancel, only once StartBuilding has been started. Between two
    queued builds the engine carries no stance over: StopBuilding for the first, then the second
    node's walk, then its own StartBuilding. The factories' StartBuilding/StopBuilding is the state
    setter's (`0x48B14E`, `0x48B169`), which the calls table above wrongly gave for every build.
  - **The MoveRate thresholds** are FBI `moverate1` and `moverate2`, absolute, by default 2 ×
    MaxVelocity (`0x42C1E6..0x42C225`): a unit that never passes twice its MaxVelocity starts
    StartMoving then MoveRate1 and nothing more.
  - **The extractor's `SetSpeed`** is Σ (cell metal + 1) over the footprint cells on the map, kept
    as a sign-extended 16-bit word, sent once at the create (`0x437840`); ExtractsMetal does not
    enter it. 2016 and 2352 follow.
  - **INBUILDSTANCE against the builder's tick**: the stance wait `0x438700` is read in the order
    controller, after the unit's slice, and the handler's states chain inside one controller call,
    so a script's `SET` is seen in the tick it runs and the first progress is added in that tick;
    a stance left set by the previous build starts progress at once. Once in the work state
    (`0x403E0C`) the stance is not read again: the step `0x41BA60` does not read `+0x10F`. No build
    order tests the builder's heading: the script turns and sets the stance.
  - **ActivateWhenBuilt** is def `+0x241` bit 18, the FBI key's `and 1` (`0x42C6B9..0x42C6DE`),
    and the only reason the engine switches a finished structure on (`0x41B9A6`) or one made whole
    (`0x48612F`).
- **L4a** (answered 2026-10-28, the engine map's *The nano particle*, DISASSEMBLED): the boxes
  and their cut to 4/11 … 7/11 of each axis (`0x473B50`); five particles a burst and two bursts an
  emission (`0x473D50`, `0x472F60`, the walker `0x471EB0`); the steps `(int16)(len >> 18)` and the
  0-step rule; the step, the end and the colour cycle (`0x4739B0`, `0x473B30`); the target's
  height `0x4CB5F0`; a reclaim's two-tick cadence (`0x404A57`); the dot, 2 × 2 by `DrawBar`'s
  inclusive fill (`0x4BF6F0`, `0x4CCDEA`). `effects.md` §7.3 had the box cut as points of the
  builder→site line, and is corrected.
- **L4b**: the fire `0x472330`, wake `0x472430` and bubble `0x472530` emitters' particle counts,
  lifetimes, velocities and draws; the smoke emitters' puff count past `0x474ECE`; and what the
  wiki's HEALTH would have to be for `SmokeUnit` to emit.
- **L5**: every caller of `0x47F0C0` / `0x47F1A0` / `0x47F300` for the events the wiki has, the
  cooldown rule (`0x47FAF0`, `0x47FEFD`), the volume settings' storage, the sounds.tdf reader.
