# Guarded COB compatibility

The guarded COB runtime.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.105 Guarded COB compatibility (`tagpu_patches.c`, `tagpu_cob_safe.c`) — in progress

The [COB decision and evidence note](../tadr-port/cob.html) is the verification ledger; this branch
does not yet claim complete mod parity. Required getters 32 and 69–75 use full signed-ID checks
against the match's pool before addressing a live record. The rule is one engine-free function,
`tagpu_cob_getter` (`tagpu_cob_safe.c`): the detour's `cob_get` hands it the engine's units
(id, owner, kills and build fraction from the unit array; alliance and seat kind from the player
records) and the COB VM `tagpu_cobvm.c` calls the same function under its IMPURE rules, so the two
cannot answer an id differently. The replay test group `cob-getters` judges the game's answers
against the engine's state. The guarded runner's pop and attachment rules are likewise
`tagpu_cob_parent_pops` and `tagpu_cob_attach_piece_ok` there. ID 111 remains native/inactive.
The portable core bounds file allocations, instruction/entry operands, 128-word stacks,
ten-record waits and old/new saved state. In the raised build the game's scripts run in Impure's
COB VM (*The game's COB interpreter*, below); the engine's scheduling -- the unit tick's slice --
remains. Unsupported types are unavailable with an entry warning, while required-unit creation
and unrecoverable execution faults refuse with diagnostics.

| VA / inventory | Mechanism |
|---|---|
| `0x480770` | six-byte getter detour, retaining the native fallback and relocating its model operand |
| `0x4B245C`, `0x4B246F..0x4B2475`, `0x4B26D5` | validate the actual file allocation before relocation, checksum that same allocation, unlink metadata before free |
| `cob_runtime_sites` | checked object size/model/count/record-stride operands, including all ten native slot-count loops; `0x4B0921` initializes allocated records; `0x4B0DAD` guards wait-state access and `0x4B0E5D` every instruction fetch |
| `0x4B1EC0`, `0x4B2040` | replace complete save/load methods; validate native eight-slot, guarded eight-slot or guarded ten-slot input before exposing restored state; zero additional runtime records |
| `0x42D8F4`, `0x42D299`, `0x43E0B0` | bind initial/reloaded scripts to model storage; bound the native SweetSpot result consumer |
| `0x497581` | saved unit partition established before native `0x4917D0` allocates the level |
| `0x42DA58`, `0x42BEC3`, `0x485FA0`, `0x485D64` | filter rejected types from build lists and guard creation, including required/saved units |
| existing held-create wrapper at `0x488462` | check required mission types before the wrapper hides the native caller behind its own frame; no new patch site or engine-state field |
| `0x497F5E`, `0x49842F`, `0x496A4F` | reuse load reset/in-play ordering; deliver local chat through `0x46BC70` after native frame work |
| `0x4B06B0`, `0x4B06F0` | optional trace invalidates deferred starts at both COB destructor entries before free |
| `0x485D74`, `0x485D78`; `0x4B0940`, `0x4B0B00`, `0x4B0C40`, `0x4B0D60` | the object sized and zeroed for the host's tail; the four entries jump to the game's COB VM (raised build) |
| `0x4FD6C8..0x4FD6E4`, `0x4B15E0`, `0x4B19D0` | `tagpu_cobverify.on` only: the oracle's logging thunks in seven vtable slots, RAND's draw, RETURN's completion |

The trace keeps a lock-ordered per-thread pending start, logs every transferred argument and
new stack peaks (`H`); capacity failures invalidate its evidence with `INCOMPLETE`. It writes
no engine state. In the stock-limits build the allocation/runner/return/signal/random observers in
§2.4 are its seams; in the raised build the engine's interpreter no longer runs, and the COB VM's
trace events are (`tagpu_cobtrace_core`, the same lines, `H` from the VM's report of a step past
32 words); the destructor observers stay in both.

**The game's COB interpreter** (`tagpu_patches.c`, THE GAME'S COB INTERPRETER; the raised build
only). The entries `0x4B0940`, `0x4B0B00`, `0x4B0C40` and `0x4B0D60` jump to the game host, which
runs `tagpu_cobvm.c` under its IMPURE rules on the unit's own object: the records, count and flag
in place, the statics at `*(cob+0x10)`, the animation at `*(cob+0x14)`, the posed model. `0x4B0A70`
and `0x4B0BC0` stay the engine's and reach `0x4B0B00` and `0x4B0C40`; the runner `0x4B0DA0`, the
stepper `0x4B1C00` and the allocator `0x4B08C0` are no longer reached, nor the guard's stubs inside
them, whose checks the VM makes itself (`tagpu_cob_check_step`, wherever the stack proof does not
show the check would pass). Everything a script reaches outside its unit is the engine's own
function: each effect through the object's vtable slot, as the stepper calls it -- so SET's state
setter, the puppet gates, the transport messages and EXPLODE's draws are the engine's by
construction -- GET's stock half through `0x480770`'s trampoline after the shared getter rule,
RAND through `0x4B6C30`, a completion through its object's slot 0. What only the engine's
interpreter made, the host makes: the recorder's allocation counts (`tagpu_rec_cob_alloc`, keyed
as `0x4B08C0`'s sink keyed them), the slice markers' note of a start (`tagpu_rec_cob_started`) and
the trace. Its state is a tail of the object (`CobTail`: the VM, bound at the object's first entry,
and the script's name), sized at `0x485D74` and zeroed at `0x485D78`; a saved game's records pass
`tagpu_cobvm_loaded`, and a fault is `cob_fault` with the VM's reason.

Two starts the guarded engine ran, the VM does not. A start by index with `argc` outside 0..4 is
refused as a full pool refuses (counted, its completion called with 0): only a malformed
script-start message carries one, since the `0x10` receiver passes the message's byte as `argc`
(`0x455544`) and every other caller of `0x4B0B00` and `0x4B0A70` passes 0 to 2. A record that runs
2^24 instructions without blocking is a fault (`STEP_BUDGET`) where the engine finishes it, however
long the game then stands still; the corpus gate meets it in none of its runs.

**Its oracle, `tagpu_cobverify.on`.** The engine's interpreter runs the game through trampolines
of the entries; around each entry call the oracle copies the object and logs what the engine's
interpreter asked and did outside it -- getter answers, draws, effects with the posed-model words
each wrote, cargo and carrier reads, completions, the same object's entries made inside those --
then runs the VM on the copy over a host that answers from the log, and requires each question to
match and the copy to equal the object byte for byte. The test group `cob-verify` (slice-walkers'
game) plays it and `tools/replay/cobverify_check.py` judges its log. MEASURED 2026-11-21 on six
groups' games (slice-walkers, the battle, transport-turn, teleport, tenseat, cob-getters): 6.1
million entry calls on the live lanes and 7.8 million on the puppet lanes, every one equal. DROP, the cargo and carrier
reads were not among them.

The CORE advanced-kbot upgrade reproduction needs a ninth script slot while its old
Stop/close chain overlaps the new StartBuilding callback. The guarded pool on this branch is ten
records, object `0x158C`; allocation and every reader/iterator share that bound. The
stock-limits comparison build retains the native eight records. The trace's slot bounds
follow the selected build. The ten-slot runtime completed all 61 accepted upgrades in an
all-deposit Comet Catcher CORE sweep, with 20 initial refusals and no unfinished accepted
build. Native eight-slot import and ten-slot full-pool save/load preserved every active probe
record; full-pool START/CALL probes pass. Arbitrary mod-script demand is still not proved
bounded by ten. See the engine map's *CORE reclaim-to-Moho handoff* and the resource note.

The completed-Moho replay fixture compares after completion as well as around three restores.
Its diagnostic run found a first paused hover writing the ghost-mask SIM_STATE cache.
`tagpu_datakeys_ghost_mask` computes a cache miss in local storage while native pause is set,
so hover cannot mutate carried state at a frozen tick. Puppet cache misses also use local
storage, leaving the recording the only writer. No state class or comparison is loosened.
The native save/load preflight preserves all ten active records on human and AI probes;
its coverage reaches both `cob_save` and `cob_load`. The ten-slot completion replay passes
all 14 paused comparisons and its construction/effect/restore checks. After the owner's
two four-byte keyboard-index leave rows, that fixture's strict image gate also passes
in the combined run. Focused native replays pass the corrected camera/selection samples,
worker catalogue invalidation and isolated trace coverage. The whole rerun after disk cleanup,
approved UI rows and scenario seat-count repair finishes all 37 fixtures: 1479 checks pass,
including 519 exact paused comparisons and 36 strict image gates. All 17 resource image gates
and stock rewind checks pass. A seek empties the engine's effect pools, which the recording
does not carry, and the images gate classes them as effects in every recording
([recorder completeness](../tadr-port/demo-recorder-completeness.md)). The new whole gate
passes all 37 fixtures, 1480 checks, 519 exact paused comparisons and all 37 image gates,
with 11/11 changed function/region requirements reached and a passing note on `cfe3771f`.
Its native control at T301 accepts zero cosmetic bytes and also passes the default strict
comparator; the earlier T451 gap remains. Two independent HIGH reviews found no surviving
code defect; their three LOW documentation findings are corrected.
See the recorder completeness note for the exact bounds. The native
trace lever is enabled only during CORE and stock native preflights; recorded lanes keep
all ordinary sink counters enabled. The stack-peak
observer belongs only to the guarded interpreter. Wine compatibility on the ten-slot
DLL has 18 expected setup outcomes, one documented inactive gap, zero unexpected outcomes
across 19 setups and all 15 passing
network cases. Windows was not run for this change.

**GUI first-arm ordering.** `tagpu_gui_mirror_want` requests a producer reset on its rising edge.
The render-thread mirror withholds operations and publication until its drain records that
reset. Reset debt remains until `tagpu_gui_handover` delivers the complete, non-lost record.
If allocation/atlas invalidation loses it, or swapchain acquisition skips its delivery, the
next drain requests another reset and withholds suffix history. `tagpu_gui_reset.h` carries
that render-owned state machine; its compiled tests exercise lost, skipped and successful
delivery. The first delivered non-lost history starts with RESET, by ordering rather than delay.
Measured during the compatibility investigation: first arming could previously follow four
already-drained operations and one unrecorded twin. After the change, all 19 Wine startup/battle
outcomes met expectations; all 15 network cases passed, with the loader/Mayhem case rerun after
an independent nickname-clearing defect in the driver was fixed. Windows testing remains open.
