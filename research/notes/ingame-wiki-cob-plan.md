# Our own COB VM, and the in-game wiki's units animated by it, planned

**Written** 2026-10-27. **L1 and L2 are built** (the core, tacob on it, the shared rules, the
wiki's units recorded and replayed; see *The landings*); L3, cut into L3a to L3c on 2026-10-28,
is in progress, and L4 and L5 are not started. The wiki's specification is still
[the in-game wiki](ingame-wiki.html) and its mockup, which stays the 1:1 spec of the DLL wiki. The
engine facts below carry their address and are the engine map's
([*The COB engine — mapped by us*](exe-reverse-engineering.html)); a fact this plan needs and the map
does not have yet is listed under *Research each landing owes*, and the landing that first relies on
it disassembles it. When the work closes this file is folded into the wiki's note, `tacob-design.md`,
`gpu-status.md` and `roadmap.md`, and then deleted.

## Why

Today the wiki reads each unit's script only for what `Create()` hides (`tagpu_cobscan.c`): every unit
stands in its rest pose, a kbot's legs do not walk, a constructor does not unfold its nanolathe, an
extractor's arms do not turn, and there is no nano spray ([the wiki's note](ingame-wiki.html), *What
the simplified simulation cannot drive*). Animating them needs a COB VM the wiki can run.

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

The calls (decision 8), each with the entry, `runNow` and the side of `DoScriptsNow` that tacob's
`ENGINE_STARTS` table gives:

| The wiki's action | The engine's calls |
|---|---|
| a unit appears: a scene's spawn, a nanoframe placed | `Create` (`0x485DE6`, run-now) |
| a move starts, ends | `StartMoving` / `StopMoving` (`0x43DAF2`, run-now, after the unit's tick), the MoveRate calls (`0x43DB27`) where the engine makes them, `SetSpeed` (`0x437902`), `SetDirection` (`0x43795E`, `0x43798A`) |
| build, assist, reclaim | `StartBuilding` / `StopBuilding` (`0x48B14E`, `0x48B169`), `QueryNanoPiece` (`0x43E427`) per spray |
| a structure finishes, is switched on or off | `Activate` / `Deactivate` (`0x48B106`, `0x48B12B`); the extractor's `SetSpeed` with its metal |

`get` is answered from the WIKISIM: the unit's own position, heading, build fraction, health, the
values its script `set`, the ground height. `rand` draws from a generator the WIKISIM owns and hashes.
The per-unit COB state is part of the simulation's state hash, so `simparity.mjs` checks the events
both simulations send to the core as well as everything else.

**One host, in C, for both simulations.** Everything the host decides about a script -- which
program a type runs, the four blocks of each unit, what `get` answers, what a `SET` changes, the
generator `rand` draws from, a refusal and its quarantine, the state hash of it all -- is one
engine-free file, `tagpu_wikicob.{c,h}`. The DLL's `tagpu_wikisim.c` calls it in-process; the
mockup's `sim.js` calls the same file through the mockup's server; `wikitest.c` links it for the
parity check. What the two simulations still each implement is only *when* they call it: the
actions of decision 8, made at the same points of the same tick, which `simparity.mjs` compares
through the state hash (the host's hash is part of it). So the host cannot drift between the DLL
and the mockup any more than the core can.

**The tick.** The engine's unit turn is its script slice and then its orders and movement
(`0x48ADEB` before `0x48AFAA`, the engine map's unit tick); the wiki's tick keeps that order for all
its units at once, in creation order:

1. the inputs of the tick (as today);
2. **the slice**: `DoScriptsNow(1)` for every unit with a script, each `SET` committed into that
   unit's values as its opcode runs;
3. **the units' work** (`advance`), which reads the committed values -- a builder credits work only
   once its INBUILDSTANCE is set -- and makes the engine's calls where the engine makes them, each
   at once: a run-now start runs every record with `dt = 0` inside the call, a run-later one waits
   for the next slice.

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
checks call it synchronously as the page does. Without a host (`ctx.cob` absent, `WIKISIMCTX.cob`
NULL) every type is quarantined (below): the rule cases that test no script play exactly as before.

**Where the scripts come from.** The DLL takes each type's script from the wiki's asset load,
which already resolves `scripts\<UnitName>.COB` by the engine's rules (`tagpu_wikiasset.c`). The
mockup's packs carry the same bytes: `gfx.py` writes each unit's script and its model's piece
facts beside its mesh (gitignored, like everything derived from the game), and `serve.py` and
`wikitest.c` read them from the pack. A type's model gives the host its piece count (the
`Object3do`'s primitives, `0x45AE80`) and each primitive's initial flags (`0x45AF1B`: visible only
with three or more vertices), in the posed model's order -- the COB's piece order, after
`0x45A950`'s swaps (`tagpu_cobscan.c`).

**The quarantine** (decision 9): a type whose script the program validator refuses, or one whose
unit's VM faults, is quarantined for the simulation's life -- every unit of it drops its VM, stands
in the pose `Create()`'s hides give today, reads every gate as open (INBUILDSTANCE, ACTIVATION,
YARD_OPEN set), and the host names the type, the script, the pc and the reason once. A
quarantine is part of the state hash, so both simulations must quarantine alike.

**Drawing.** The stage already draws units through the game's posed program with a pose array per
piece (`tagpu_wikiworld.c`, 12 floats a piece, with per-piece visibility and shade); the PrimitiveStructs
the core writes are composed into it the way `tagpu_native.c` `posed_pose` composes the engine's,
over the posed model's node order (above) rather than the readers' walk the rest pose uses today.
The mockup does the same in `gfx.js` with the poses the server answers.

## The landings

| | What | Gate |
|---|---|---|
| **L1** — built 2026-10-27 | `tagpu_cobvm.{c,h}`: the core on the layout above, the host interface, the effect log, the observer; the shared extension functions factored out of `tagpu_patches.c`; tacob on the core through ctypes, **its Python VM deleted**, `tacob-build.py` cross-compiling it; the reentrancy check | the nine fixtures byte-exact through tacob on the core; `test_tacob.py`; native tests of the core under UBSan and ASan; the layout's static asserts; the DLL builds |
| **L2** — built 2026-10-27 | live cobtrace + posedump fixtures for the wiki's units doing the wiki's actions — ARMCK and ARMACK walking, starting, assisting and stopping a build, reclaiming; ARMMEX and ARMMOHO built, activated, spinning at a set metal value; ARMGEO activating; ARMPW walking — and one exercising the extensions (records 9 and 10, the getters) | each fixture byte-exact through the core |
| **L3a** | the host (`tagpu_wikicob`), the calls and the gating in both simulations, the facts, the quarantine, `until` waits and the 11 demos converted, the mockup's server and node host, the scripts in the packs | `simparity.mjs`, `simcheck.mjs` (every demo's claim with the scripts running), `wiki_regression.py` |
| **L3b** | poses drawn: the posed model's order, the core's PrimitiveStructs composed in the C stage and in the mockup | a look at every demo in game and in the mockup; the stage's checks |
| **L3c** | the corpus gate: every compatibility setup's scripts through the host, a refusal named | the gate itself, clean over every setup |
| **L4** | the nano spray and `emit-sfx`: the engine's emitters ported, the wiki generator, drawn through the stage's instance of the particle draw | the spray's look against a game capture of the same build |
| **L5** | sound: the mixer, the triggers, the cooldowns, WebAudio in the mockup | a demo heard in game and in the mockup; the player's settings honoured; an MP game's own sounds unchanged with the wiki open |

L1 and L2 touch `tagpu_cobvm.*`, `tagpu_cob_safe.*`, `tagpu_patches.c`, `tools/tacob*` and the
fixtures — no file the sidebar's landing touches.

### Gaps L1 leaves open

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
  ARMMEX, `2352` ARMMOHO on the 223-metal patch) but not yet derived.
- **L3**: when the engine calls `StopBuilding` on an assist; the MoveRate thresholds; the
  extractor's `SetSpeed` value as a function of the metal under it; the order in which the
  builder's code reads INBUILDSTANCE against its own tick.
- **L4**: the nano emitter (spawn rate, velocity, lifetime, colour, how its randomness is drawn) and
  `emit-sfx`'s types, from `effects.md` and `build-state.md` and disassembly.
- **L5**: every caller of `0x47F0C0` / `0x47F1A0` / `0x47F300` for the events the wiki has, the
  cooldown rule (`0x47FAF0`, `0x47FEFD`), the volume settings' storage, the sounds.tdf reader.
