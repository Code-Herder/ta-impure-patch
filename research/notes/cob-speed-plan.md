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
(the query) and `0x4B0D60` (`DoScriptsNow`) jump to the game host's functions, which run the core on
the unit's object. `0x4B0A70` and `0x4B0BC0` stay the engine's: they look the name up themselves, with the
first-match loop `0x4B07C0` also runs (`0x4B0A85..0x4B0AC1`, `0x4B0BD5..`), and call `0x4B0B00`
(`0x4B0AEE`) / `0x4B0C40` (`0x4B0C2F`). The runner, the stepper and the allocator `0x4B08C0` are no longer
reached. Every caller of the entries — the engine's 15 + 21 + 5 + 14 sites, the recorder's start and
query stubs, `tagpu_replay.c`, `tagpu_weapons.c`, `dtor_killed` — keeps calling the same addresses.

**What a script reaches outside its unit goes through the engine's own functions**, so every side
effect the engine has is kept by construction (DISASSEMBLED, the scout's inventory of 2026-11-21):

| the core asks | the game host calls | what that keeps |
|---|---|---|
| SET | the object's vt+0x40, `0x480B20` | `+0xBA` bit 2 for every id; the state setter `0x48B090`, which starts Activate, Deactivate, StartBuilding, StopBuilding **run-later** on the same object (`push 0, push 0` at `0x48B0FD..0x48B169`) — the engine advances the SET's pc only after the call (`0x4B1BD5`), as the core does after its commit; the puppet gate at `0x480B26`; the COB_WRITES keys |
| EMIT_SFX, EXPLODE | vt+0x30 `0x480EB0`, vt+0x34 `0x481140` | the viewer's sight test and the repose `0x45AB10`; the six sim-RNG draws and the debris |
| ATTACH, DROP | vt+0x38 `0x481340`, vt+0x3C `0x4813B0` | the transport messages; the puppet gates |
| GET, GET_UNIT_VALUE | `cob_get`, the function `0x480770` jumps to | the getter rule `tagpu_cob_getter`, then the stock handler |
| cargo, carrier | vt+0x48 `0x481430`, vt+0x4C `0x481470` | |
| RAND | `0x4B6C30` | the sim RNG's order |
| a RETURN's completion, a refused start's | the completion object's slot 0 | the weapon slot's `+8` (`0x481490`) |

The piece setters (vt+0x00..+0x10) write only the `PrimitiveStruct`, its `+0x26` and the model's
header words `+0x04/+0x08/+0x10`, which the core writes directly the same way (DISASSEMBLED
`0x480C50..0x480E2B`).

**What only the engine's interpreter produced, the host produces itself**:

- the recorder's allocator counts (COB_CHILD_ALLOC): the core reports every allocation it attempts,
  refused ones and unknown scripts included, and the host counts it with the key the sink at `0x4B08C0`
  would have used (`0x4B18C0` START, `0x4B192D` CALL, and the return address of each engine entry's
  own call of the allocator);
- the slice-marker observers on the `0x4B0B00` and `0x4B0940` entries, whose bytes the swap takes:
  the host's entries call the same observation;
- cobtrace: from the core's trace callback, which writes the oracle's lines already.

**The unit's VM state** (the program, the "every opcode checked" flag, the trace latch) is a tail of the
COB object, which Impure sizes already (`0x485D74`); the allocation at `0x485D78` is zeroed so the tail
starts empty. A fault is the guarded interpreter's refusal: `cob_fault` with the core's reason. A
saved game's records pass `tagpu_cobvm_loaded` after `cob_load`.

**The oracle is the COB plan's: the recorder's, in process.** Two runs of one build are not one game,
so two builds' dumps cannot be compared (MEASURED 2026-11-21: `slice-walkers` played twice on one DLL,
both passing every test, differ by tick 961 in unit positions and records, and their recordings end at
4 464 763 and 5 037 891 bytes; the parallel pathfinder's carried searches are one known source). So
behind a lever the engine's interpreter goes on running the game, and around each entry call — a turn,
a start, a query — the host takes the object's records, count, flag, statics, animation and model
before it, and logs what the engine read and did during it: each getter's id and answer, each draw,
each effect with its operands and the starts it nested (SET ACTIVATION's), each completion. Then the
core runs the same call on the copy over a host that answers from that log in order and compares each
effect with the next one logged, and the copy must equal the object byte for byte. A difference names
the unit, the script, the pc and the field. Then the replay gate's groups that name COB
(`"cob": "exact"`: slice-walkers, battle, tenseat, codec-decline; and cob-getters,
resource-core-upgrade, stock-resource-mex, contest-waves, teleport, transport-turn) and the
compatibility suite run on the swap.

### L3 — A turn only when something is due

A unit's turn runs the core only when a thread is due: a wake slice per unit (the earliest sleeper's,
or now for a runnable thread or a cleared wait), the sleep decrements it is owed, and an animating
flag for the animation-only step. The owed decrements count **turns given** (`tagpu_slice_give`),
never game ticks: a puppet gives turns at recorded places and at the step's end, a type-4 seat's units
get none, a paused game none. Every entry into the object (a start, a query, the save) applies them
first. That is exact because the unit was not due: every sleeper still has at least one tick left
afterwards, as the engine would have left it.

### L4 — The block

A unit's COB memory becomes one block sized by its program, made with the unit: a header line (the
fields the engine reads, the scheduling data), the ten record headers packed, ten stacks of the
program's `depth`, the statics, the animation block. The posed model stays the engine's allocation.
Blocks of one size come from a free list of their own. The save translates to and from the 128-word
format, a dead record's leftover heap bytes written as zeros; a saved record the proof does not cover
gets a block of 128-word stacks. The unit loop prefetches the header line of the units ahead.

## Gaps open

- **The oracle sees what the engine logs**: a getter whose answer the same call's own SET changes is
  answered from the log as the engine saw it, which is the comparison's point; a host read the oracle
  does not log (cargo, carrier) is read again after the engine's call, where an ATTACH or DROP in that
  call could have changed it.
- **`tagpu_cobvm.c` and `tagpu_cob_safe.c` are not `sim` files**: once the game runs them, they need
  their `thread-split.allow` lines and the program validator's `malloc` through the registry's
  wrappers (the COB plan, *Gaps open*). L2.
- **The core's other hosts** (the wiki, tacob, the mockup) run engine-layout blocks: L4 either gives
  the core a layout description or moves every host to the block.
- **Not disassembled**: whether `0x43E060` (getters 7 and 8, EXPLODE's positions) and `0x47DB70`
  (DROP's site test) write anything.
- **The bench's limits**: the engine's getters were answered by a hash, only turns were stepped (no
  engine starts or queries), and it ran native 64-bit code. Each landing measures itself in the game.
