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

Argument-zero conventions and invalid-unit results must be verified per getter
against the shipped handler and its callers before implementation. In particular,
zero is not a universally safe error value: it can mean player zero or completed
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
| Corpus | resolve the installed content and archive precedence; census extended getters/setters, bytecode validity, stack demand and script starts across all listed mods | independent-archive audit run; effective content and runtime proof pending |
| API | explicitly execute all eight new getters in-game in at least one real mod setup, using test-only scripts where normal content cannot reach a getter; assert results, not just call counts | Escalation zero-result baseline run; port assertions correctly fail; parity pending |
| Bounds | highest supported live ID, first out-of-array ID, negative IDs, zero conventions, empty/dead/reused slots, 65535/65536/65537; no aliasing or out-of-allocation access | pending |
| Capacity | real high-stack scripts, locals/arguments/query returns, signals and nested calls, thread exhaustion, rejection path and bounded allocation lifetime | pending |
| Features | upgrades, shields, adjacency, gates, transports, constructors/factories and orientation wherever each mod uses them; ProTA/retail regression controls | pending |
| Multiplayer | two Wine peers for every listed mod, host-owned and joiner-owned units, local-AI ownership, alliance/completion changes, attach/drop and destruction; no duplicate owner actions | pending |
| Rejection | identical unavailable types across peers, unit/script/reason chat at entry, mismatch handling, and refusal for required/pre-existing units | pending |
| Save/load | valid old saves and new saves with expanded state, including an upgrade in progress; malformed state refused safely | pending |
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

No getter, stack expansion, rejection path or save extension is implemented merely
by accepting this plan.
