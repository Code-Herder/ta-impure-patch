# The game's COB scripts, faster (plan)

The program that replaces the game's COB interpreter with Impure's own VM, `tagpu_cobvm.c`
([the COB plan](ingame-wiki-cob-plan.html), decision 3), and then makes it cheap: script turns only
for units with something due, checks proven when a script loads, and a memory layout of the game's
own. Parallel script execution is not in it; it is designed once these landings show what is left.

## Why

MEASURED 2026-11-21 in the 13 500-unit Total Mayhem battle on the reference setup (the evidence is in
the main checkout's `.data/prof/2026-11-21-zk13k-cobvm/` and `.../2026-11-21-zk13k-cobproto/`, local):

- The unit tick is about 11 ms; COB script turns are about 45 % of it. Impure's per-opcode guard costs
  1.20 ± 0.10 ms (an in-game A/B in 64-tick blocks). The rest is mostly cache misses.
- About 84 % of script turns run no opcode at all (every thread asleep) and still visit all ten
  records, 548 bytes apart in a 5.5 KB object.
- Prefetching the COB object of the unit four to eight slots ahead: -0.49 to -0.58 ms a tick.
- An offline bench stepped a dump of the battle's 10 378 COB objects through `tagpu_cobvm.c`, every
  variant checked byte for byte against the plain run (cache evicted before every tick):

| variant | ms a tick |
|---|---|
| the core on the engine's layout, every unit every turn | 5.41 |
| + a turn only for units with something due, an animation step only for units only animating | ~3.2 |
| + checks proven at load | 2.93 |
| + the record headers packed, stacks sized by the program | 2.45 |
| + statics and animation inside the unit's block | 2.37 |
| + blocks of one program together (in a long game's order) | 2.22 |

Memory: 54.6 MB of COB objects become 8.7 MB of blocks.

## The owner's decisions (2026-11-21)

1. **The game runs `tagpu_cobvm.c`** in place of the engine's runner `0x4B0DA0`, stepper `0x4B1C00` and
   entries; the steps below are built on it.
2. **The game's COB state may have a layout of its own** (the COB plan's decision 4, reversed).
3. **A unit's script memory is a fixed block sized by its program**, made with the unit: ten slots, as
   the engine's numbering of them requires, and no allocation while scripts run. A pool of thread
   records was rejected for that reason.
4. **Scheduling data lives in the unit's block**, so the recorder needs no new class.

## The landings

### L1 — The stack proof

`tagpu_cob_program` proves the bounds of the words a running record holds on entering each
instruction, and the core runs `tagpu_cob_check_step` only where the check could refuse
([the COB plan](ingame-wiki-cob-plan.html), *What it does*). Engine-free; its gate is
`tools/test_cobvm.py` and the corpus gate, both on a build with `TAGPU_COBVM_PROVE_CHECK`. The game is
untouched: it still runs the engine's interpreter under the guard at `0x4B0E5D`.

### L2 — The swap

The entries `0x4B0940` (start by name, no words), `0x4B0B00` (start by index, four words), `0x4B0C40`
(the query) and `0x4B0D60` (`DoScriptsNow`) jump to the game host (`tagpu_patches.c`, THE GAME'S COB
INTERPRETER), which runs the core on the unit's object; the stock-limits build keeps the engine's
interpreter. `0x4B0A70` and `0x4B0BC0` stay the engine's: they find the name with a first-match loop
of their own (`0x4B0A85..0x4B0AC1`, `0x4B0BD5..`) and call `0x4B0B00` (`0x4B0AEE`) / `0x4B0C40`
(`0x4B0C2F`). The runner, the stepper and the allocator `0x4B08C0` are no longer reached. Every caller
of the entries -- the engine's, the recorder's start and query stubs, `tagpu_replay.c`,
`tagpu_weapons.c`, `dtor_killed` -- keeps calling the same addresses.

**What a script reaches outside its unit goes through the engine's own functions**, so every side
effect the engine has is kept by construction (DISASSEMBLED, the scout's inventory of 2026-11-21):

| the core asks | the game host calls | what that keeps |
|---|---|---|
| SET | the object's vt+0x40, `0x480B20` | `+0xBA` bit 2 for every id; the state setter `0x48B090`, which starts Activate, Deactivate, StartBuilding, StopBuilding **run-later** on the same object (`push 0, push 0` at `0x48B0FD..0x48B169`) -- the engine advances the SET's pc only after the call (`0x4B1BD5`), as the core does after its commit; the puppet gate at `0x480B26`; the COB_WRITES keys |
| EMIT_SFX, EXPLODE | vt+0x30 `0x480EB0`, vt+0x34 `0x481140` | the viewer's sight test and the repose `0x45AB10`; the six sim-RNG draws, the debris and the piece it hides (`0x421639`) |
| ATTACH, DROP | vt+0x38 `0x481340`, vt+0x3C `0x4813B0` | the transport messages; the puppet gates |
| GET, GET_UNIT_VALUE | the getter rule `tagpu_cob_getter` over the world `cob_get` builds, then `0x480770`'s trampoline | `cob_get`'s answer, by construction the same function |
| cargo, carrier | vt+0x48 `0x481430`, vt+0x4C `0x481470` | |
| RAND | `0x4B6C30(hi - lo + 1)`, every draw | the sim RNG's order: `0x4B6C30` returns 0 for n < 2 without stepping it, and is still called |
| a RETURN's completion, a refused start's | the completion object's slot 0 | the weapon slot's `+8` (`0x481490`) |

The piece setters (vt+0x00..+0x10) write only the `PrimitiveStruct`, its `+0x26` and the model's
header words `+0x04/+0x08/+0x10`, which the core writes directly the same way (DISASSEMBLED
`0x480C50..0x480E2B`); the stepper's other slots are the getters `+0x14`, `+0x18` and two empty
`ret`s, `+0x28` and `+0x2C`.

**What only the engine's interpreter produced, the host produces itself**:

- the recorder's allocator counts (COB_CHILD_ALLOC): the core reports every allocation it is asked
  for, refused ones and unknown scripts included (`TAGPU_COBVM_HOST.alloc`), and `tagpu_rec_cob_alloc`
  counts it with the key the sink at `0x4B08C0` would have used (`0x4B18C0` START, `0x4B192D` CALL, the
  entries' own `0x4B09A4`, `0x4B0B0D`, `0x4B0C4D`);
- the slice markers' note of a start at `0x4B0B00` and `0x4B0940`, whose observers are the stock
  build's only now: the host's entries call `tagpu_rec_cob_started`;
- cobtrace: `tagpu_cobtrace_core` writes the same lines from the core's trace events, and the core
  reports a step whose record holds more than 32 words (`H`).

**The unit's VM state** (`CobTail`: the bound VM and the script's name) is a tail of the COB object,
which `0x485D74` sizes; the allocation at `0x485D78` is zeroed, the constructor `0x4B0610` has that
one caller, so a tail starts unbound by construction and is bound at the object's first entry
(`tagpu_cobvm_bind`, the engine's blocks as they are). A fault is the guarded interpreter's refusal,
`cob_fault` with the core's reason. A saved game's records pass `tagpu_cobvm_loaded` after
`cob_load`. `tagpu_cobvm.c` is an `engine-free sim` file (no static at all); `tagpu_cob_safe.c` is
not, because its validator allocates each program at the script's load (`tools/sim-state-check.sh`
names that gap).

**The oracle is the recorder's, in process** (`tagpu_cobverify.on`, THE ORACLE). Two runs of one
build are not one game, so two builds' dumps cannot be compared (MEASURED 2026-11-21: `slice-walkers`
played twice on one DLL, both passing every test, differ by tick 961 in unit positions and records,
and their recordings end at 4 464 763 and 5 037 891 bytes; the parallel pathfinder's carried searches
are one known source). Behind the lever the engine's interpreter runs the game through trampolines of
the entries, and around each entry call the oracle copies the object, its statics, animation and
posed model and logs what the engine's interpreter asked and did outside the object, in order: the
getter world's answers and the stock getter's, each draw, each effect with the posed-model words it
wrote (EXPLODE's hide, the repose's dirty flag and cached body turn `0x45AB65`), cargo and carrier
reads, completions, and the same object's entries made inside one of those with their answers. Then
the core runs the same call on the copy over a host that answers from that log, each question
checked against the logged one, and the copy must equal the object byte for byte; a difference
names the unit, the call, the field and the kinds logged. The test group `cob-verify` plays
slice-walkers' game under it and `tools/replay/cobverify_check.py` judges the log.

MEASURED 2026-11-21 on the live lane L of each game; its puppet lanes P, R and Q compared 7.8 million
calls more, every one equal too:

| game | entry calls compared | equal | effects among them |
|---|---|---|---|
| slice-walkers (`cob-verify`) | 246 678 (L) | all | EMIT_SFX 1103, EXPLODE 539, SET 49; 1620 completions, 1103 draws |
| the battle | 5 738 749 (L) | all | EMIT_SFX 2073, EXPLODE 4202, SET 44; 8 nested entries |
| transport-turn | 65 470 (L) | all | EXPLODE 600, ATTACH 80, SET 81 |
| teleport, tenseat, cob-getters | 33 759, 45 710, 3 045 (L) | all | the getter world's units, callers, allied and local tests |

The swap itself plays slice-walkers and cob-getters to a pass on every test: record, puppet, round
trips, seeks, and the counts with the COB children exact.

**What it costs** (MEASURED 2026-11-21, the 13 500-unit battle on the reference setup, one game: a
scratch build whose odd 64-tick blocks call the engine's entries through the oracle's trampolines and
whose even blocks run the core; the main checkout's `.data/prof/2026-11-21-cobswap/ab-battle/`): the
unit tick takes 8.72 ms a tick on the core against 9.73 on the engine's guarded interpreter, the core
0.96 ± 0.09 ms faster than its neighbouring blocks, in 30 of 32. The other spans are unchanged within
their noise. That is about the guard's measured 1.20 ms, most of which the stack proof removes; the
records' visits, which L3 and L4 address, are unchanged.

### L3 — A turn only when something is due

`tagpu_cobvm_tick` visits the records only when one of them can run. After a turn that ran, the VM
notes whether every busy record sleeps or waits on a CALL (`quiet`) and the smallest sleep (`due`).
While it is quiet, a turn whose dt does not bring that sleep to 0 visits no record: its dt is added
to what the sleepers are owed (`owed`) and only the animation stepper runs. That is the engine's
turn exactly: such a turn takes dt off each sleeper and makes none runnable (`0x4B0DEF..0x4B0E01`,
runnable only at 0 or below), a CALL record waits until its child's RETURN or a signal wakes it,
and both of those happen inside a turn or an entry. Every other entry -- a start, a query, a turn
that runs -- first takes the owed dt off the sleepers (`tagpu_cobvm_settle`) and leaves the next
turn to schedule again; the save settles before it reads the records, and a loaded object starts
unscheduled.

The owed dt is counted in the core from the turns it is given, so it is the engine's count by
construction: a puppet's turns, a paused game's and a seat's whose units get none are exactly the
calls of `0x4B0D60` that are or are not made. The schedule's three words live in the unit's VM, in
the tail of its COB object; outside the core only the oracle reads them, to carry the schedule
through its replay copy, and a hash of a VM's state hashes it settled (`tagpu_cobvm_settled`). A recording needs nothing new:
the recorder keeps no COB state (a unit's scripts are rebuilt by its create), and the save, which
does keep it, writes settled sleeps.

`tools/test_cobvm.py` is the proof of the owed dt: a scheduled VM, settled by nothing but its own
entries, plays against one made to visit its records every turn, both given the same turns, starts
and queries -- starts that run at once and put a thread to sleep inside the call, queries that
signal a sleeper away and start others, made while several turns' dt is owed -- and after every turn
the second must equal the first with its owed dt taken off (worked out in the test, and by
`tagpu_cobvm_settled`), with the statics, animation and posed model. Removing the settle from the
start or the query, or dropping the owed dt from the skip test, fails it. The oracle checks each
turn's decision in the game: its replay copy starts from the engine's state, which owes nothing, and
the scheduled core must leave it as the engine's every-turn interpreter does (MEASURED 2026-11-21,
`cob-verify` 259 206 entry calls equal). It cannot see dt owed across turns, nor can the replay
groups, whose recording and puppets both run the scheduled core.

**What it saves** (MEASURED 2026-11-21, the 13 500-unit battle on the reference setup, one game: a
scratch build whose odd 64-tick blocks clear `quiet` before every turn, the main checkout's
`.data/prof/2026-11-21-cobl3/`): the unit tick takes 8.12 ms a tick scheduled against 9.15 visiting
every record, 1.08 ± 0.09 ms less than its neighbouring blocks, in 31 of 32. The other spans are
unchanged within their noise. The bench's ~2.2 ms for this step counted the turns it no longer makes
at all; in the game each unit's turn is still a call through `0x4B0D60` that reads the VM in the
object's tail, which L4's block and prefetch address.

### L4 — The block

A unit's COB memory becomes one block sized by its program, made with the unit: a header line (the
fields the engine reads, the scheduling data), the ten record headers packed, ten stacks of the
program's `depth`, the statics, the animation block. The posed model stays the engine's allocation.
Blocks of one size come from a free list of their own. The save translates to and from the 128-word
format, a dead record's leftover heap bytes written as zeros; a saved record the proof does not cover
gets a block of 128-word stacks. The unit loop prefetches the header line of the units ahead.

## Gaps open

- **What the oracle does not see**: ATTACH's and DROP's live-unit test is asked again after the
  engine's call; an entry made inside a call on another object's entry is verified on its own, not as
  part of the outer call; a refused start's completion is logged when the start returns. No game has
  yet given it a DROP, a cargo or a carrier read.
- **`tagpu_cob_safe.c` is not a `sim` file**: its validator allocates each program with `malloc` at
  the script's load, which the sim scope refuses; the programs are content of the loaded files, and
  `tools/sim-state-check.sh` names the gap.
- **The core's other hosts** (the wiki, tacob, the mockup) run engine-layout blocks: L4 either gives
  the core a layout description or moves every host to the block.
- **Not disassembled**: whether `0x43E060` (getters 7 and 8, EXPLODE's positions) and `0x47DB70`
  (DROP's site test) write anything.
- **The bench's limits**: the engine's getters were answered by a hash, only turns were stepped (no
  engine starts or queries), and it ran native 64-bit code. Each landing measures itself in the game.
