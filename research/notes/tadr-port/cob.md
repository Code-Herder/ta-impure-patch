# F. COB compatibility for the supported mods

## Scope and state

**Decisions accepted 2026-09-28; implementation and verification in progress.**
The goal is correct gameplay for the versions of Escalation, Total Mayhem, TA Zero,
Twilight and ProTA in [the compatibility suite](../compat/setups.md), with their
own content and executables and no TADR code executing. Launching and fighting a
generic battle does not establish COB feature parity.

The [existing census](../tadr-merge-exploration.md#f-the-cob-getters) identifies the
recorder's eight getters as the missing API. TADR is a behaviour guide; its source
is not adopted. The shipped 3.9.2.416 handler, historical source and each mod's
actual scripts establish the contract. The much larger unreleased 4.0 API and the
renumbered 2026 recorder are not the target.

| ID | Getter | Intended result |
|---|---|---|
| 32 | `VETERAN_LEVEL` | caller's kills multiplied by 100, not the configured veterancy tier |
| 69 | `MIN_ID` | first unit ID, 1 |
| 70 | `MAX_ID` | last valid unit index in the actual unit array |
| 71 | `MY_ID` | caller's unit ID |
| 72 | `UNIT_TEAM` | queried unit's owner index |
| 73 | `UNIT_BUILD_PERCENT_LEFT` | queried unit's remaining construction; exactly zero at completion |
| 74 | `UNIT_ALLIED` | queried unit's owner allied to the caller's owner |
| 75 | `UNIT_IS_ON_THIS_COMP` | queried unit controlled by a human or AI on this peer |

The shipped 416 handler has no argument-zero self-query convention: all four
unit-target getters address the supplied ID. The port treats slot zero as invalid,
returning -1 for 72/73 and 0 for 74/75, as for other invalid/dead IDs. Zero cannot
be the invalid result of 72/73 because it means player zero or completed
construction. Ordinary empty slots in a unit scan are not script faults.

## Accepted decisions

1. **Only required features.** ID `111` remains inactive in this phase, even where
   Mayhem scripts request it. New callins, map scripting, unit creation APIs and
   the rest of the unreleased extensions are outside scope.
2. **Preserve visibility semantics.** Existing scripts may scan units through fog.
   Bounds and liveness checks must not introduce a visibility filter or change
   the scripts' polling cadence.
3. **Full-width validation.** Validate the original signed 32-bit COB argument
   before any narrowing or pointer arithmetic. Never let `65537` alias unit 1.
   Bound it against the actual allocated array, validate the live slot and owner,
   and establish the lifetime of every subsequently dereferenced object. A pointer
   probe, exception handler or plausible address is not a lifetime guarantee.
4. **Retain the engine's ID format.** This phase does not widen 16-bit engine or
   network unit references. The raised build currently supports 1500 units per
   player: at that setting, 15001 array slots for ten players including slot zero
   (`tagpu_limits.h`, `OFF_UNITSLOTS`). A lower match setting makes a smaller array;
   the first live Escalation probe below measured 10001 slots. The 16384 unit-type
   slots are a different namespace. Use the actual bound, not a hard-coded 15000.
5. **Safe execution capacity.** The listed mods' valid scripts must fit and run.
   Size stacks from a verified corpus analysis and guard all accesses, including
   locals, argument transfers and query outputs. Merely refusing the known
   54–85-word scripts at stock's 32-word capacity does not meet the goal. Audit
   eight-slot thread exhaustion separately; increase thread capacity only where
   the supported content needs it. Preserve scheduling semantics where valid.
6. **Reject unsupported types visibly.** A provably unsupported or malformed COB
   makes its whole unit type unavailable, consistently on both peers. On game
   entry, chat identifies the unit name/type, script and reason; the log carries
   full details. Do not leave a buildable unit with just one required script
   silently disabled. Valid supported content must produce no such exclusions.
7. **Required units block loading.** If an excluded type is a starting commander,
   a required mission unit or already present in a saved game, refuse that match
   or load with a clear diagnostic. Do not remove or substitute existing units.
   An unrecoverable execution fault discovered only during play stops the affected
   game with a diagnostic rather than continuing with broken script behaviour.
8. **Saved games are in scope.** Read existing stock and shipped-mod saves whose
   script state is valid. New Impure saves preserve the full expanded state and
   resume an upgrade in progress correctly. Older TA/TADR builds need not read
   Impure saves containing expanded state. Recorded demos are outside this work.
9. **Use the existing integration rules.** Simulation patches join the verified,
   fail-closed patch table and have no runtime opt-out. Use the port's same-build
   multiplayer contract and existing unit synchronization; establish consistent
   exclusions before play. No new general version handshake is required.

## TADR and stock defects to audit

These are evidence to investigate, not fixes already implemented here.

- Shipped 416 lacks live-slot checks and has unsafe invalid-ID paths, including
  owner dereferences and an invalid construction query returning an address
  ([the census](../tadr-merge-exploration.md#f-the-cob-getters)).
- The current source's `TAUnit.Id2Ptr(UnitId: Word)` narrows before lookup;
  `IsAllied` explicitly casts to `Word`. `GetOwnerIndex(nil)` returns player zero,
  and `IsOnThisComp` relies on exception handling around pointer dereferences.
  These findings concern the source checkout, not a claim that every shipped
  binary has identical code.
- Stock's 32-word stack has unchecked pushes. The documented Escalation census
  reports 85 words for CORDECI/ARMCRAWL and 54 for ARMVCAR. Recheck the analysis and
  reproduce the accesses live before selecting the storage design.
- A refused stock `START` leaves arguments on its parent's stack; a refused
  `CALL` can wait forever on slot -1. Thread exhaustion is distinct from stack
  exhaustion ([engine map](../exe-reverse-engineering.md#the-eight-records-cob0x1c-slot-0xa4)).
- TADR's `MaxScriptSlots` expands eight records to 64 of the same `TScriptSlot`
  size; it does not expand each stack. Its `SaveGame` plugin separately replaces
  record serialization. Neither is a proven design for this port.
- Do not copy TADR's proposed non-owner build-finish detach. The owner already
  broadcasts attachment changes; a duplicate non-owner detach can race a later
  pickup ([B evidence](sim-fixes-evidence.md), Part 5, Option A/B).

## Verification gates

| Gate | Required evidence | State |
|---|---|---|
| Corpus | resolve the installed content and archive precedence; census extended getters/setters, bytecode validity, stack demand and script starts across all listed mods | effective native-loader census run across all five mods; model binding exposes malformed bundled ZZZ in four setups; complete dynamic occupancy proof remains |
| API | explicitly execute all eight new getters in-game in at least one real mod setup, using test-only scripts where normal content cannot reach a getter; assert results, not just call counts | 84/84 Escalation assertions pass, including nonzero kills and construction; all eight also pass on both peers of each listed mod |
| Bounds | highest supported live ID, first out-of-array ID, negative IDs, zero conventions, empty/dead/reused slots, 65535/65536/65537; no aliasing or out-of-allocation access | signed/zero/first-out-of-pool/65535–65537 pass live; live skirmish-ceiling ID 6000 passes in a 15001-slot pool; global live ID 15000 and dead/reused transitions remain |
| Capacity | real high-stack scripts, locals/arguments/query returns, signals and nested calls, thread exhaustion, rejection path and bounded allocation lifetime | real 85-word scripts, local 100, all 100 CALL arguments, refused START with 128 arguments, SIGNAL recovery and full-pool CALL diagnostic pass; complete corpus occupancy remains |
| Features | upgrades, shields, adjacency, gates, transports, constructors/factories and orientation wherever each mod uses them; ProTA/retail regression controls | real Escalation fusion upgrade completes and survives an in-progress save; remaining feature-specific cases open |
| Multiplayer | two Wine peers for every listed mod, host-owned and joiner-owned units, local-AI ownership, alliance/completion changes, attach/drop and destruction; no duplicate owner actions | all five mods pass self/remote getter ownership; local-AI getters pass in single player; dynamic feature cases remain |
| Rejection | identical unavailable types across peers, unit/script/reason chat at entry, mismatch handling, and refusal for required/pre-existing units | matching and host-only/joiner-only malformed content consistently excluded; warning captured on screen; required starting/saved commander refusals pass; mission case remains |
| Save/load | valid old saves and new saves with expanded state, including an upgrade in progress; malformed state refused safely | native 32-word save, expanded records and real upgrade round trips pass; malformed records covered offline, shipped TADR save fixtures remain |
| Windows | startup and in-game smoke checks across setups; full Windows multiplayer is deferred | pending |
| Landing | parallel build, documentation and rendered wiki, dedicated high review, fixes and reruns; local landing only | pending |

Use the real mod content for feature parity and isolated generated scripts for
complete API and invalid-input coverage. Test overlays must not alter the player's
installed mod or become runtime dependencies. Keep original game/mod assets out of
tracked evidence; retain generated fixtures, reproducible drivers and measured
results. Every gap remains explicit until its gate actually runs.

## First implementation slice: audit and live red test

Measured 2026-09-28 on the reference setup. This slice adds research/test tooling,
not simulation hooks. No getters have been enabled, and no units have been excluded.

### Independent-archive audit

`tools/cob_audit.py` follows reachable instruction paths from every named entry,
tracks literal/unknown stack values, records GET/SET IDs and child calls, and reports
unproved paths. Aliased entries and shared tails are legal; entry labels are not
code bounds. Owner-only local declarations can produce different stack depths at
the same return, so the worklist keeps those depths separate. Analysis depth and
state limits terminate a growing-stack loop with an **incomplete proof**, not a
safe verdict. Literal arithmetic uses signed 32-bit wrapping where modelled;
unmodelled expressions remain unknown.

| Mod fixture | Archive entries | Distinct COB hashes | Largest relative stack | Hashes with control-flow findings |
|---|---:|---:|---:|---:|
| Escalation GOLD 10.2.0, all eight archives | 550 | 542 | 85 | 3 |
| Mayhem 11.3.0, `mayhem.gp3` and `TADemoM.ufo` | 496 | 480 | 28 | 0 |
| Zero Alpha 5, `TAZ31.gp3` build 120526 | 269 | 249 | 13 | 0 |
| Twilight Beta 98, `rev31.gp3` | 518 | 460 | 23 | 3 |
| ProTA 4.8, `ProTA.gp3` | 311 | 288 | 22 | 0 |

These are **not effective installed-script counts**: duplicate paths are retained,
deduplication is by content hash rather than path, base-game content is not included,
and archive precedence is not inferred. They therefore do not replace the older
path-based census. The relative peaks start at `sp=-1` and assume successful child
allocation. Engine-supplied arguments, query outputs, thread occupancy and stale
locals still require their own proof; 85 is not a chosen runtime capacity.

The high-stack results reproduce the earlier static finding: ARMCRAWL and CORDECI
`Detect` reach 85, ARMVCAR `Detect` reaches 54. The remaining flow findings are:

- Escalation: ARMGANT, COREVP and CORULAB `Killed` have conditional branches to the
  first word after the declared code. ARMGANT's branch follows a severity `<= 99`
  test. Whether the engine supplies a value taking that branch is not yet measured.
- Twilight: CMGEO `Killed` can reach an unknown opcode; CORCRW and CORCRW-OLD
  `Killed` have out-of-code targets in the analysis.

These findings are **not type-rejection decisions**. The three Escalation scripts
ARMASPID `AimSecondary` and CORPOUND `AimPrimary`/`AimSecondary`, and Mayhem ARMZEPH
`SmokeUnit`, also retain computed GET IDs the literal analysis cannot resolve.
Their call sites are reported explicitly. No additional literal extension was found:
the observed extension set is still 32, 69–75 and the deliberately inactive 111.

Reproduce with `TA_COB_FIXTURES` pointing at the compatibility cache's `fixtures/`:

```bash
python3 tools/cob_audit.py "$TA_COB_FIXTURES/escalation-gold-10.2.0/TAESC.gp3" \
  "$TA_COB_FIXTURES/escalation-gold-10.2.0/T2ESC.ufo" \
  "$TA_COB_FIXTURES/escalation-gold-10.2.0/T3ESC.ufo" \
  "$TA_COB_FIXTURES/escalation-gold-10.2.0/T4ESC1.ufo" \
  "$TA_COB_FIXTURES/escalation-gold-10.2.0/T4ESC2.ufo" \
  "$TA_COB_FIXTURES/escalation-gold-10.2.0/T5ESC.ufo" \
  "$TA_COB_FIXTURES/escalation-gold-10.2.0/TXESC.ufo" \
  "$TA_COB_FIXTURES/escalation-gold-10.2.0/TADEMO.ufo" --json /tmp/cob-audit-esc.json
python3 -m unittest discover -s tools -p 'test_cob*.py' -v
```

The audit exits 1 for findings and 0 for no findings within its stated model.
Neither exit status is a runtime-safety certificate.

### All eight getters called in real Escalation

`tools/cob_getter_probe.py` uses the suite's complete Escalation setup, private Wine
registry and private virtual display. A generated `COBFPROBE` unit reuses the mod's
solar model/FBI but gets a wholly generated script; no existing unit is overwritten.
Eight sequential child calls report their GET return values through the existing
COB trace. They need only two thread records and a five-word relative stack.

Two initial fresh single-player matches and a repeated baseline ran against the worktree DLL
`b7782d9a2b56802479f96ed79438f6ac4c219d3391d21b23232f1f877a030a31` (SHA-256):

- `--expect stock-zero`: **16/16 pass**. Every getter returned zero on both the
  human-owned unit (slot 2) and the local-AI-owned unit (slot 1002).
- `--expect implemented`: **11/16 fail as expected**, with exit 1. IDs 69, 70, 71,
  74 and 75 differ on both units; 72 also differs on the AI-owned unit. The five
  matching zero answers do **not** prove implementations of 32, 72 or 73.
- The actual `u16 main+0x14351` was **10001** on each run: this match requires
  `MAX_ID=10000`, not 15000. The native count was read independently of COB.
- The repeated baseline also checked native unit records independently: both slots
  were alive, had the requested owner, zero kills and zero construction remaining.
  These preconditions are asserted by the probe, not assumed from the scenario request.
- The live executable/import inspection reported no control-flow destinations in
  TADR; no recorder log appeared. Escalation's one permitted `tdraw` startup line
  was present (the bootstrap which loads Impure), with no engine patches installed
  by it. All test instances and their display servers were stopped and removed.

```bash
python3 tools/cob_getter_probe.py --out /tmp/cob-getters-baseline --expect stock-zero
python3 tools/cob_getter_probe.py --out /tmp/cob-getters-port --expect implemented
```

Each output directory must be new and outside the repository. It retains the return
checks, trace, application result, DLL hash and hook inspection. The assertions are
for fresh completed units with zero kills; non-zero veterancy and construction,
zero/invalid argument conventions, highest-ID boundaries, two-peer ownership and
actual upgrades remain untested. This is the live **red test**, not the API gate's
completion. Eighteen new offline/tooling tests and 84 existing tacob tests pass.

## Implementation sequence

1. Establish a reproducible corpus audit and correct any earlier census claims.
2. Map interpreter accesses, unit lookup/ownership, construction and attachment,
   serialization, type exclusion and local chat delivery against each target exe.
3. Implement the bounded getter layer and its live oracle; establish safe stack
   and thread behaviour before exercising the full mod workloads.
4. Integrate validation, consistent type exclusion, diagnostics and saved state.
5. Run the gates above and document the measured outcomes before review/landing.

## Runtime implementation checkpoint — 2026-09-28

The branch implements the eight getters and a portable checked-bytecode core,
with engine integration in the existing fail-closed patch table. ID 111 remains
inactive. Every unit argument is checked as signed 32-bit data before lookup;
invalid/dead slots, including zero, return -1 for 72/73 and 0 for 74/75.
Stock unit getters 9–11 are bounded too. The caller's alliance
row is used, without a visibility filter; controller 1/2 means local human/AI.

The interpreter has eight 128-word records: stride `0x224`, object size `0x1144`,
busy count at `+0x113C`, model at `+0x1140`. The size accommodates the measured
85-word relative demand and four native arguments. Validation checks the actual
file-read allocation before relocation; execution checks each instruction's stack
access and every wait's piece/axis/child index. START consumes arguments even on a
full pool; a full-pool CALL stops with a diagnostic instead of waiting on -1.
The native scheduler and non-call opcode bodies remain in use. The file destructor
owns metadata removal; the trace's two COB destructor observers replace its old
pointer probe and age heuristic with lifetime invalidation.

Six earlier corpus findings are valid native terminal paths, not excluded units:
five death scripts branch to the declared code end, where the original next word
is zero, and Twilight CMGEO reaches opcode word 1. Stock treats unknown opcodes as
thread termination. The validator admits the proven trailing zero only; the guard
takes the stock terminal path without fetching outside the declared code. The
production validator accepted all independently archived scripts in all five mod
corpora (550 ESC, 496 Mayhem, 269 Zero, 518 Twilight, 311 ProTA).

**Measured:** both the getter-only DLL and the expanded/guarded runtime DLL passed
all 16 Escalation self-query assertions (eight getters, human and local AI).
The latter DLL SHA-256 was
`bd3d99d2941aeff034a9abd17366902e29b3cc30fb41799c757e740dd70fa499`.
The actual array held 10001 slots, and neither run found TADR execution.
The test instances and displays were removed. Eight portable native tests run
with undefined-behaviour instrumentation; together with the audit/probe tests,
26 tests passed at this checkpoint.

**Not complete:** highest-live/recycled IDs, complete feature coverage,
Windows smoke checks, documentation completion and the dedicated landing
review remain gates. Later measurements below supersede the initial runtime checkpoint;
the branch is not landed and does not yet establish full mod parity.

### Extended live assertions and save verification

The extended Escalation probe passed **82/82 assertions**, with DLL SHA-256
`72777168b74c5885d4e1422eed8ba9213ee5eb52731e346a97f4374adceb3123`.
Both human and local-AI units exercised all eight getters, with independently
checked nonzero kills (7/8) and construction remaining (50%). A generated function
used local 100 and a 102-word peak stack and returned its stored value, 9876.
For getters 72–75, the probe also checked zero, negative values, 65535, 65536,
65537, INT_MAX and the actual pool's first out-of-range ID. These are live
single-player assertions, not two-peer or natural mod-feature coverage.

The initial expanded-record save/load run and
three fix-and-rerun attempts failed in the test driver: first an unacknowledged
pause, then SAVEGAME absent from TABMENU, then the same menu transition observed
too early, and finally OPTIONS requested while ARMOPT was active. The last run
did verify the paused, sleeping records containing local 100 before navigating
menus, but did not produce a completed round-trip comparison. The captured
evidence did not establish a serialization failure or success. Following explicit
approval to continue iterating, a panel-state-driven driver passed the in-game
round trip: both sleeping records retained local 100, resumed, and the complete
82-assertion suite passed. Old-format saves and an actual upgrade-in-progress save
remain unverified. All owned probe instances were cleaned up; the branch has not
been reviewed or landed.

### Shipped-handler correction and rejection tests

Direct disassembly of ESC's shipped 416 recorder confirms the target contract:
`0x722F78` is the getter detour, `0x722E6C` the extended dispatch, and
`0x722FA3..0x722FC9` resolves getter 73's supplied ID and jumps to native
`0x480A44`. Getter 75 uses `0x723508` (premature Word narrowing, including zero)
and `0x7235D8` (owner controller through a pointer and an exception handler).
Neither substitutes the caller for ID zero. The initial port probe's zero-self
expectations for 73/75 came from newer source and were wrong; its earlier 82/82
result is not evidence for those two expectations. They are corrected to the
explicit invalid-slot results above. The corrected live run passed as recorded below.

The malformed ordinary-type probe passed with the match continuing and all
82 original assertions; the log recorded its entry message. The strengthened
probe also reads the native chat ring and unavailable bit directly. A malformed
starting ARMCOM refused the match with its unit name/type, script and local-index
reason. Existing archived COB paths need a loose test-instance override; putting
ARMCOM.COB in a new UFO does not replace the shipped one.

Script absence alone is **not** a rejection: native `0x485DFE` constructs a
scriptless model, and ESC's xARMMLS uses that path. Malformed present files are
rejected before relocation; valid scriptless content remains available. The
rejection index resets before the loader starts, build-list appenders filter it,
and required/saved creation refuses instead of silently skipping the unit.
Chat uses the local reminder API, split into its 63-character payloads, with
one rejected type announced every five seconds so a large list is not overwritten
at entry. This pacing is presentation only, never a safety condition.

### Live verification after the shipped-handler correction

DLL `e9bdfaed59a4e180949cc5b33e5ff97390f0edb2e58ecb33d41594426dace5a9`
passed **84/84** Escalation assertions, the expanded save/load round trip, and ordinary-type
quarantine. The test read both native chat lines containing the unit, type, script and reason,
and the definition's cleared availability bit. A later 84/84 run with DLL
`c58b6b49fc646587005bd68dbad586d60de250c43c908324ecf3da8a71918d0f` also captured the presented
window at entry: both warning lines were visible and readable. A 100-argument CALL preserved every argument in order and
returned argument 99, in addition to the 102-word/local-100 test. The trace keeps per-thread
latches and orders deferred argument reads against COB destruction; its `H` event records
new per-record stack peaks above 32. A trace reporting `INCOMPLETE` cannot be a passing oracle.

The generated all-eight-getter test passed on two Wine peers for Escalation, Mayhem,
Twilight, ProTA and TA Zero: each peer created its own probe, tested self ownership, and scanned for
a live remote-owned unit whose getter 75 returned zero. These checks do not establish
attachment or alliance-change behavior. TA Zero uses its own solar model/FBI and the `zunits`
directory. Identical malformed types were excluded on both Escalation peers. Separate host-only
and joiner-only malformed COB tests passed with DLL `c58b6b49…`: native checksum synchronization
removed the incompatible type from **both** definition tables before COB loading. Absence is not
a set availability bit; the first mismatch test's oracle incorrectly conflated those states.
Native mismatch removal emits no COB-validator warning because the file never reaches it.
The initial full compatibility suite ran 19 setups: 17 met their
goal, gammata remained the known not-loaded gap, and Escalation with the development TADR
reported a renderer source-twin reseed. All 15 network matches reached play, but that
renderer finding keeps the compatibility gate open.

The renderer finding was traced to first arming the Vulkan mirror after the producer had
already drained four operations and created one twin without recording it. The mirror now
requests a producer reset and withholds operations/frames until that reset is recorded;
the later review below strengthens this to actual delivery. With DLL `c58b6b49…`, all 19 startup/battle outcomes
met expectations and 14 network matches passed. The remaining loader/Mayhem match stopped in
the lobby: a field-centre click left the nickname caret inside existing text and backspace
could not remove the suffix. The input driver now clears both sides with acknowledged
backspace/delete progress; its focused regression and all 329 tacli tests pass. A targeted
loader/Mayhem rerun on the same DLL passed startup, battle and multiplayer. Together the full
run and targeted rerun cover all 19 expected outcomes and all 15 network matches.

The Windows startup attempt returned no results while another agent was using the Windows
test machine concurrently. It is not valid evidence for this DLL; further Windows checks
are deferred until exclusive use is available. No in-game Windows smoke result is claimed.

The isolated `--skirmish-ceiling` probe passed **84/84 assertions** on DLL SHA-256
`acf809553741a21c9ec3b4fedf401e4d41da175466262a19b6ea7f5f579ba3d6` (same runtime source,
rebuilt). Native creation filled player three's 1500-unit block, retaining its commander,
and placed the scripted probe at **ID 6000**. Independent observations found that probe
alive, owned by player three, with 10 kills and 50% construction remaining; the human probe
was ID 2 with 7 kills. All eight getters, invalid full-width IDs, local 100, the 102-word
stack and all 100 CALL arguments passed. The pool contained 15001 slots and TADR execution
evidence was empty. This proves the four-player skirmish ceiling, **not global live ID 15000**.

The initial fixture used retail settings and observed ESC's unchanged 1000-unit limit;
correcting it to `TAESC.ini` and `Software\TA Esc` reached 1500 but did not create player
nine. Skirmish activates only four players, as independently documented by `ball10.json`.
The fixture now names that scope explicitly; global ID 15000 needs a network fixture.
The compiled safety-core/probe/audit/display tests total 35 passing tests, the tacli suite
329, and the compatibility self-test 40. The parallel DLL build and wiki build pass.
The ta-drive reference check still flags the existing generic `tagpu_<pass>_lines.txt`
placeholder as two nonexistent readers; no missing tool/scenario paths were reported.

The full-pool fixtures passed on DLL `c58b6b49…`. Seven sleeping children plus their parent
occupied all eight records; a START with 128 arguments was refused, consumed all arguments,
then SIGNAL freed the children and all 16 human/local-AI getter checks resumed. A separate
CALL into the full pool stopped with `CALL cannot obtain a child thread`, naming the script
and word. Neither path writes past the 128-word allocation or waits on a nonexistent child.

Native-save testing exposed an independent stock load-order defect: the saved unit limit
was read after allocating the player slot ranges, silently losing AI units when the current
limit differed. The wrapper now establishes the saved partition before allocation, refusing
unsupported limits rather than narrowing or remapping IDs. The native-32-word retail save
restored all three commanders and their live COB records with the raised default using DLL
`96a536e9b5e7e03866f70cd0f196252f939f815dac816a64c864a4c0cab30471`.
See the engine map's *A saved game's slot partition must precede allocation* for the control
test and addresses. A saved ARMCOM whose script was made malformed refused loading with its
unit/type, script and local-index reason, without substitution. An actual Escalation fusion
upgrade preserved its script state in a mid-construction save/load, then completed at tick
15986 and attached to its parent (unit 2). The same run recorded stack peaks of **85** in both
shipped ARMCRAWL and CORDECI scripts, with no incomplete trace. ARMVCAR's declared 54-word
Detect routine has no observed normal entry path: Create starts SmokeUnit and track_tracks,
not Detect. Forcing a test call would not establish that the mod naturally executes it.

### Dedicated review and model-backed pieces

Two read-only HIGH reviews of `main...5ef6565` found five actionable defects: piece bounds
against declarations rather than models; an unchecked SweetSpot result consumer; missing
zero-divisor rejection; two native instructions treated as terminal; and GUI reset debt
cleared on recording rather than delivery. All were independently checked against the
source/disassembly. The fixes bind reachable accesses to model storage, bound the query
consumer, correct the opcode shapes and divide guard, and retain reset debt through lost
or skipped handovers. The engine map records each address and lifetime argument.

The first model-bound experiment rejected every declaration count larger than the model.
The installed corpus disproved that policy: CORTSAR has 110 declarations over 109 nodes,
with an unused final `height`. The bound now applies to reachable accesses, not unused names;
save/load retain declared-size rows but never access a model for the excess rows.

`tools/cob_corpus_probe.py` pauses an owned game and reads the scripts the **native loader
actually selected**, without guessing archive precedence. Relocated programs are converted
to canonical COBs in memory for analysis; only hashes and metadata are saved outside the repo.
Baseline DLL `62081b63…` observed:

| Setup | Definitions | Distinct loaded programs | Relative peak words |
|---|---:|---:|---:|
| Escalation | 549 | 540 | 85 |
| Mayhem | 506 | 482 | 28 |
| Zero | 270 | 250 | 13 |
| Twilight | 505 | 450 | 23 |
| ProTA | 317 | 288 | 22 |

On DLL `08c43e68ccefc1093491d1f9d04b92b89ba3219d85366813cc882dfe145e0b2a`,
Escalation retains all its loaded programs. Each of the other four setups excludes exactly
the bundled `ZZZ` script: its four declarations include reachable piece 3 over a three-node
model. This is a real malformed-content quarantine, not a passing zero-exclusion result;
the strict corpus driver's exit status remains nonzero. The type and reason are logged and
announced at entry. The normal units with unused excess declarations (including CORTSAR,
ARMMANT, ARMTSPD, CORAMPH and CORSILO) remain admitted. Their admission alone is not proof
of every gameplay behavior. Runtime thread occupancy and the feature gates above remain open.

On that same DLL the expanded save/load probe passed 84/84 assertions. Generated live
fixtures in `tools/cob_safety_probe.py` confirmed a divide-by-zero diagnostic, whole-type
exclusion/chat for a reachable excess piece, and a SweetSpot-result diagnostic during real
targeting, each without a crash report. The native-opcode fixture executes both instructions
and returns 9876 from the following code. A separate `--unused-model-pieces --extended
--roundtrip` fixture declares 4096 pieces over the solar model and passes all 84 assertions
after saving/restoring sleeping 101-word records. Both also ran on DLL `08c43e68…`.

The first post-merge compatibility run passed all five mods' battles/network games but
failed to start retail's multiplayer display: a filesystem vacancy check had not reserved
that number against another runner. All suite launch paths now let Xvfb reserve the number
atomically through its readiness pipe. This fixes ownership, not timing; no other runner's
server or lock is adopted or removed. The corrected full suite is being rerun.

The follow-up review of `9f4050f` cleared those five fixes and found two related lifecycle
omissions: native `+reload` has a separate publication seam, and post-relocation rejection
must remove the engine's checksum-tree node through its full script destructor. Both were
verified in disassembly and corrected. DLL
`35693a8e85b908dd35b2d9f04449d1d918bb0a3866962d376beb775b5900526f`
then passed an acknowledged native reload, creation and expanded save/load with 4096 piece
declarations, all 84 getter assertions passing. A unique unreachable tail in the replacement
script (1015 words) was observed after reload, so an ignored chat command cannot pass this gate.
The first input attempt was not acknowledged; separating typed batches with actual engine
frame captures made the entered command observable and the reload succeeded.
The malformed reachable-piece fixture also passed on this DLL: it disabled the type,
reported it on entry, and completed cleanup without a crash through the native destructor.

The second full Wine run on `08c43e68…` passed all startup/battle cases and 14 network games,
but loader/TA Zero could not start its Xvfb process. This remained a test-infrastructure
failure, not a passing multiplayer result. Launcher failures now retain stderr, exit status
and readiness bytes; a separate concurrent twelve-server reservation check passed. A third
full run on `35693a8e…` uses three simultaneous network games rather than six, and must be
judged by its actual outcomes. No timing-dependent runtime mitigation was introduced.
