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
ten-record waits and old/new saved state. Native scheduling and valid opcode bodies remain
in use; unsupported types are unavailable with an entry warning, while required-unit creation
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

The trace keeps a lock-ordered per-thread pending start, logs every transferred argument and
new stack peaks (`H`); capacity failures invalidate its evidence with `INCOMPLETE`. It writes
no engine state. The allocation/runner/return/signal/random observers in §2.4 remain its seams.

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
and stock rewind checks pass. The sole failure is native cosmetic-pool state in the unchanged
stock secondary-orders control, reproduced in a fresh worker. Seek clears those pools, whose
record layouts remain unimplemented. The owner approved only this native control's three
cosmetic-pool ranges as an explicit unlisted-image known gap on 2026-10-06; all other bytes,
all resource fixtures and ordinary state/order/effect/restore checks retain their policy.
The saved comparisons pass with the option and still fail without it. The new whole gate
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
