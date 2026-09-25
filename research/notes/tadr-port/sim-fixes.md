# B. Simulation bug fixes — the plan

## Summary

Section B brings TADR's fixes for **defects in the stock 3.1 engine** into our stack, as our own
code, over six landings. The owner decided every choice below on 2026-09-25 **[DECIDED]**, in a
grill that followed [the evidence pass](sim-fixes-evidence.md). **Landing B1 is built (2026-09-25) and
awaits its review; nothing is landed yet.** The rules
shared by every group are in [the port overview](overview.md#standing-rules-decided-2026-09-23).

TADR's "~15 fixes" turned out to be four kinds of change mixed together, and only the first is B:

1. **Defects in stock engine code**, whatever the subsystem: simulation, loaders, the network
   receivers, the UI. Each fix is the identity on every input stock handles correctly.
2. **Fixes to TADR's own features**: rotation (the staircase yardmap, the footprint, the return
   stack), its logger (the long path, `int 29`), its surface wrapper (print-screen), its map spawns
   (initial commands), its hotkeys (ctrl-F/B), its ID recycler ("unit limit between missions").
   None of these code paths exists in our stack.
3. **Gameplay changes presented as fixes**: repair rate ×3, aircraft wrecks falling, off-map
   anti-air (1 or 32 tiles), the share guard, the anti-nuke circle, allied jamming, the grid-claim
   tie-break. Not in B. Each is a rules decision for later, with its evidence kept.
4. **Diagnostics**: breadcrumb rings, the vectored crash report, the observe-only order-dispatch
   guard. Not in B; we have our own log and `ErrorLog` path.

Four findings shaped the plan:

- **Several real defects are live in ordinary stock play, and TADR covers them only in part.** An
  explosion that reaches more than 20 units, or more than 64 features, hits every victim past the
  cap once per footprint cell; TADR fixes the units and misses the features. Flak fired nearly
  straight up divides by zero, on the firing peer and on every peer that receives the shot; TADR
  only logs it.
- **Several of TADR's fixes are timing, or assume lockstep.** The factory-explosion fix holds a freed
  slot for 150 ticks (2.5 s at speed 20, against the TAF tunnel's 5 s of buffering), and its
  allocator changes single-player slot order. Its stacked-air index is built a step before it is
  used, and the network pump or the unit tick can free a unit in between. Its grid tie-break fixes a
  disagreement that nothing peers must agree on reads.
- **Our 1500-unit raise tripled two stock identity windows.** A remote unit's full state recurs every
  N ticks (the `0x2C` round robin, `GameTime % N`), so a ghost commander or a ghost left by a lost
  death message lasts 50 s at 1500, against 16.7 s at stock's 500.
- **Some of TADR's items would add bugs.** The cargo-detach "Option A/B" would make every non-owner
  broadcast a duplicate detach. The ghost-commander "Assist" reports every land unit as slot 0 and
  morphs the commander on every peer. The yardmap fix turns stale bytes into a NULL that six stock
  readers dereference.

## Decisions

### How B fixes are held

**Always on, in both builds.** B fixes are corrections, not raised limits, so they sit beside the
thirteen in `patch_engine_defects` (`tagpu_patches.c`) and are in `ddraw.dll` and
`ddraw-stocklimits.dll` alike. There is no runtime opt-out (rule 4).

**A sim fix fails closed; a local fix skips and logs.** A peer that silently plays stock rules is
the desync the same-build contract exists to prevent (rule 3), so a fix whose absence would let a
peer compute different shared state, on an input where stock does not fault, checks its sites
before any is written and exits through [the failure report](raised-limits.md#the-failure-report)
on a mismatch. A fix whose absence only changes a crash, a draw, a message or a malformed input's
fate is local: skipped with its reason in the `enginefix:` line, as today. **The existing enginefixes
that change the simulation move to fail-closed with B1**; B1 classifies each of the thirteen by
the same test. As built in B1: the feature swap on a full pool (`0x423651`), the reclaim's anchor
mark (`0x423892`), the saved features on the border (`0x43265A`), the restored record's owner
(`0x4250C0`, `0x425185`), whole build lists, the download menus' records, unique sync keys and the
weapon IDs are sim; the sort buffer, the terrain window and the composite scratch are draw; the
NULL-plot guard and the out-of-memory text are local. The engine map's *Engine defects we patch*
gives each one's reason, and the register in `binary-patches.md` tags each row.

**The comparison is the previous build.** A before/after measurement runs the parent commit's DLL,
built in a scratch checkout, against the new one, each through `tacli --keep-dll`. No `make` flag is
added: `LIMITS=stock` keeps B's fixes too.

### The evidence bar

**A fix lands when it is reproduced, or proven.** Either the defect is reproduced on the previous
build and gone on the new one, measured, or, where reproducing it is impractical (a rare
multiplayer race, a malformed message), the disassembly proves the defect and the fix is the
identity everywhere else; the note says which. An item that is neither reproduced nor proven is
**parked**, not landed. TADR's "potential" fixes (the join password) are parked on this rule.

**A two-peer test only where peers can disagree.** Two peers (`a2net0`/`a2net1` on `:71`) when the
fix sits in a network receiver or its decision reads state that differs between peers
(owner-local fields, local pools, indices the owner assigns). A fix whose decision reads only
replicated state is tested on one peer, and the landing says why that suffices. Measurement rounds
are scoped to what each commit can change.

**Draw-side defects that our renderer no longer runs are moot**, recorded with the evidence and not
ported. The GDI lane (`renderer=gdi`) stays the engine's own drawing: the black and over-bright
faces of `0x45A2EC` are left there.

### The items

- **Area-damage victim caps: both fixed.** Units past 20 and features past 64 are hit once per
  explosion. Below the caps nothing changes; above them, dense bases take less from commander
  blasts and nukes than stock gives, toward the designed values.
- **Stacked aircraft: fixed, air only, with our design.** Candidates built live at each explosion,
  validated alive at use, served after stock's walk, so stock's victims, their order and their
  damage are byte-identical. Stock aircraft losing their cells is measured before anything is built.
- **Flak's divide: `weapontimer`.** A zero divisor takes the engine's own non-burnblow flight time;
  a ballistic weapon with `weaponvelocity` 0 gets 1 at load.
- **The off-map off-by-one and the line-of-sight shear: fixed.** Both are defects at the map's
  edges. Aircraft *beyond* the edge stay untouchable, as stock designed.
- **Stale hits: an incarnation on the wire, and a two-tick hold.** A per-slot incarnation, bumped by
  the owner at create and carried with `0x09` and `0x0B` in a companion message; every receiver
  drops a hit whose incarnation is not its copy's. Separately, a freed slot is not reused for two
  ticks, in every game, so every per-tick reader of slot identity (a weapon's target
  `0x48A295`, the tracked unit `0x4995E4`, our interpolation's pairing) sees it empty once. The
  allocator otherwise stays stock's first-free. TADR's bump pointer and LRU are not ported.
- **Ghost commander: measure the cause, then fix it with an ordering.** If a start-of-game `0x09` is
  dropped because the sender's block does not exist yet, the receiver keeps it and applies it once
  the block is set: the owner's own position arrives and nothing is guessed. Otherwise the create
  takes the entry's own position, bounded to the map, for disassembled move classes only. The
  Assist is not ported in any form.
- **The wire's unit indices: bounded.** `0x09`, `0x0B`, `0x0C` and the `0x2C` receiver; on a bad
  `0x2C` field the parser stops at the engine's own end of list, since past it the bitstream cannot
  be framed.
- **A `0x0D` whose shooter has diverged: dropped and counted.** Firing from the local slot would
  invent a projectile the owner never fired.
- **Wind: one shared wind, re-seeded every game.** Our own generator with its own state, reset at
  each level load from the host's DirectPlay ID and a hash of the map's name; single player takes
  the same path, seeded from the counter stock seeds its RNG with.
- **Yardmaps: parsed inside the terminator.** Past it, the last valid char repeats; a string with
  no valid char fills `o`. Identical to stock for all 126 retail structures.
- **Resurrection: a time-boxed measurement, else parked.** If the failure branch fires, the unit is
  finalised as the success path does, touching no grid cell.
- **TA's repair rate is the game's rule, not a B defect.** Stock clamps each repairer's HP and
  energy per call to *at most* 1 (`0x41BD87..0x41BDA3`, `min` where the formula reads as `max`).
  It is recorded, and a live check of the per-call rate rides along with a landing.
- **To group E:** `+lostype` at normal command level (`0x501DF4`), the in-game `0x20` overwrite of a
  player record (`0x454934`), and what a departing host does to the others.
- TADR's `IsBadReadPtr` finding under Wine (a guard-page violation escapes and kills the process)
  is recorded in the notes, not in CLAUDE.md.

## The landings

Each landing meets the evidence bar above, lands its documentation before its review, and is
reviewed at `high` (every one writes engine state or adds byte patches). Sites below are DIS in
[the evidence](sim-fixes-evidence.md); the landing re-reads each before writing it.

**B1 — damage. Built 2026-09-25, awaiting its review.** What was built, and where it differs from
the plan below:

- **Built as planned:** the victim caps (both calls wrapped, both list blocks replaced, the unit
  set reusable by B2), flak's stub and the loader's floor, `jge` → `jg`, the shear, and the
  fail-closed install in both builds through the existing report.
- **Changed: the seen-sets are per thread, not a depth-indexed stack.** Each call's frame is a
  local of its wrapper, found through a TLS slot that the wrapper saves and restores, and holds two
  hash sets with no capacity. The `0x0E` receiver reaches area damage on whichever thread pumps the
  network, the loader's included during a network load, so a single stack would rest on an
  ordering nobody enforces.
- **Changed: the shear covers four points, not two, in every mode.** `0x465AC0` tests up to four
  points of the box; the third and fourth reads (`0x465CA2`, `0x465D46`) take the same rule, and so
  do Permanent and Circular line of sight, where the points go to `PositionInPlayerMapped
  0x408090` (`0x408095`) and the fourth point's inline copy (`0x465DA9`) with the same shear.
  `0x408090`'s other two callers inline the same True-mode read beside their calls, and both are
  patched with the same rule: the AI probe's (`0x407F74`, in `0x407E90`) joins the table, since
  what the routine keeps depends on it, and the projectile draw pass's (`0x49BEE8`, `0x49BE60`) is a
  local fix, `fix_projectile_view` — the pass still runs, the engine's frame being the golden
  source. Neither is reached by a fixture: both rest on the disassembly, under the rule measured
  for units. 161 sites in the raised build, 35 in the stock-limits build.
- **Added: a bound on the sort bucket** (`0x47CCA9`). Settling the plan's question showed the bucket
  is indexed from the unit's position with no bound: inside the grid for every footprint of width 1
  or more once `jg` holds, one past it for width 0, and stock already reaches one before it for
  width 0 at the west edge, where its linear index is the previous row's last bucket — inside the
  array, and read by simulation code. So stock's index is kept wherever it lands inside the array,
  and only an index outside it has its column and row clamped into the grid. The re-claim `0x47C790` has no bound of its own but reaches only a stamped unit
  and walks the stamped footprint, so it needs none.
- **Changed: the shear has no "else clamp".** When both the sheared and the point's own row are off
  the grid, stock's answer (invisible) stands. Clamping would make a unit beyond the map's edge
  visible and acquirable, which is TADR's margin — a gameplay change, not a defect.
- **Classified differently from the provisional list:** flak's divides are **local** (a peer
  without them faults on the shot, which is not a silent divergence), and the download menus'
  records are **sim** (they feed the builders' lists and the build menus, as the build lists do).
  The weapon IDs join the table in the stock-limits build too.
- **The install.** One table, `tagpu_limits_install`, in both builds: it now refuses two rows over
  one byte as well, and the report says "engine limits and fixes" ([the failure
  report](raised-limits.md#the-failure-report)). 161 sites in the raised build (130 + 31), 35 in the
  stock-limits build.
- **Measured** on the new build, one peer, the raised and the stock-limits builds alike where both
  ran: every CORFLAK of `b1-victim-units` lost one hit (the unit-repeat counter 48, the 16 past the
  cap found three more times each); all 96 wrecks of `b1-victim-features` took one hit (the
  feature-repeat counter 602); the edge ARMATLAS of `b1-offmap-edge` stamped and shot down within
  2 s; `north` of `b1-los-shear` shot down on station, the flak's first target; a corrupted expectation of one
  site (a scratch build, never committed) produced the report and its exit. On the previous build,
  the same fixtures: 16 of 36 CORFLAK lost 4× their mirror mates, 30 of the 32 wrecks past the 64th
  were destroyed, the edge ARMATLAS was parked and untouched for 40 s, and `north` hovered 7 s at
  full HP. The engine map has the numbers.
- **Not reproduced: flak's zero divide.** Stock acquisition never aimed a flak gun above 29.6°
  (`b1-flak-overhead`, and an ARMBRAWL flown overhead), so the fix rests on the disassembly; its
  fallback counter read 0 in every run.
- **The feature cap on two peers** (every non-host peer reports feature hits; host and joiner by
  `tools/mp_lobby.sh`, `b1-victim-features-wrecks.json` on both and `-blast.json` on the joiner, whose
  ARMCOM self-destructed, all peers paused): on the previous build the host's ranks 65–96 lost 30
  wrecks, 2 standing, and the joiner lost the same 30 with its own damage fields all 0; on the new
  build all 96 stand at 9 999 on the host and all stand on the joiner, whose feature-repeat counter
  read 602, every other B1 counter 0 on both.
- **Not run:** resurrection's failure branch (parked: no resurrect verb to drive a few hundred of
  them). The
  repair rate was measured flat per repairer, ARMCOM (WorkerTime 300) and ARMCK (80) alike, as the
  disassembly says.

The plan as written:

- **The victim caps.** Wrap the two calls into `0x49A120`, `0x49A0A9` and `0x49A109`. Each call
  gets its own seen-set: a unit bitset bounded by the array's count, and a feature set keyed by
  anchor-cell ordinal, bounded by `W·H`, on a depth-indexed stack saved and restored around the
  call, so nested calls cannot share or clobber one by construction. The list blocks
  `0x49A262..0x49A2A9` and `0x49A5CE..0x49A614` answer "seen, skip" or "record, continue"; stock
  still does the damage math. **Invariant:** one explosion damages a unit at most once and reports
  a feature at most once, and every index is bounded before use. The function's byte-check table is
  shared with A′3's splices at `0x49A78C`/`0x49A7CD`.
- **Flak.** `0x49CF18..0x49CF20` (`cdq; idiv ebp; mov edx,[0x511DE8]`) becomes a stub: a zero
  divisor takes `w+0xE6` and continues at `0x49CF29`. The weapon loader gives a ballistic weapon
  with `w+0x68 == 0` the value 1, logged (the `0x49CE6A` divide). Shared with extra-weapons'
  splices at `0x49CF65`/`0x49CF8D`. **Invariant:** no divide in `0x49CDE0` sees a zero divisor.
- **The off-by-one.** The `jge` at `0x47CC8B` and `0x47CCA3` become `jg`. Before landing, settle
  that the sort-bucket index at `0x47CCA9` stays inside its grid for a unit on the last cell, and
  that the re-claim `0x47C790` has no bound of its own.
- **The shear.** At `0x465B6A..0x465B93` and `0x465C04..0x465C2D`: when the sheared row is out of
  bounds and the unit's true row is in bounds, use the true row, else clamp. Exact whenever stock's
  row is in bounds; our order markers call `0x465AC0` and follow.
- **The install.** The fail-closed path for sim fixes, and the thirteen existing enginefixes
  classified and moved.
- **Tests.** A ring of 30+ structures and 70+ multi-cell wrecks round a commander blast, HP and
  wreck records compared before and after; the feature half on two peers, since every non-host peer
  reports feature hits. An AI CORFLAK under a hovering ARMATLAS, the flak's pitch peeked: the
  previous build faults at `0x49CF19`, the new one counts fallbacks. An ARMATLAS on the last column
  and one near the north edge at cruise altitude.

**B2 — stacked aircraft** (needs B1's seen-sets).

- First measure it: ten ARMATLAS or ARMBRAWL ordered to one point, slot B of every footprint cell
  peeked.
- Candidates are airborne (`mask & 3 == 2`), not cargo (`+0x86 == 0`), not in the off-map bucket,
  alive, with `+0x9E` non-NULL, taken from a pool rebuilt at the entry of `0x49B720` and
  **re-validated in the wrapper at use**; those whose footprint meets the blast rect and that hold
  no slot inside it are served. The selector bound `0x49A415..0x49A426` lets the **last** cell of
  the walk loop `2 + n` times, so new victims go through the engine's own per-victim code after
  every stock victim, with no per-cell capacity. **Invariant:** every unit handed to the engine was
  validated alive in this call, and every index was bounded first.
- Tests: the stack under CORFLAK/CORRL, HP per unit; `scenarios/air-war.json` for cost; two peers
  paused with equal HP.

**B3 — wire robustness.**

- Bounds in the `fix_weapon_ids` idiom: `0x0C` at `0x4866E0..0x486705` (index 0 or past the array →
  `0x486E59`; the killer bounded or NULL), `0x0B` at `0x489CED` (victim and attacker; drop →
  `0x489F93`), `0x09` at `0x4861F7` (the index; the rule that it lie in the sender's block only after
  measuring that AI players' creates arrive from the AI's own seat), and the `0x2C` receiver at
  `0x48B985`/`0x48B9AD` (delta, type against the count, a move class present; the round-robin
  remainder unsigned; a failure stops at `0x48BA28`). A player's first slot is checked against
  `begin + (1 + k·N)·0x118`.
- In `wpn_rx_fired` (`0x49D280`): drop a `0x0D` whose shooter slot's weapon is not `&Weapons[id]`,
  through the extra-weapons accessor past slot 2.
- A test-only lever, `tagpu_wirefuzz.on`, feeds crafted records to the handlers on the game thread,
  and three local counters on the heartbeat (morph, recreate, ghost) become the oracles for B4 and
  B5. Every drop is counted in the `enginefix:` line.
- Class: local (malformed input only). Tests: the lever on one instance; the two-peer weapon-ID
  fixture and a ten-peer tier 2 run with every counter at 0.

**B4 — stale hits.**

- The incarnation: a DLL static `u32` per slot (`TAGPU_PK_DESIGN_SLOTS`), bumped at create; the
  companion message's format and its binding to its `0x09`/`0x0B` are this landing's first design
  step, after A′3's `0x0E` companion. Bandwidth measured: `0x0B` is the most frequent message.
- The hold: a detour at `0x486036` (first-free, skipping a slot freed less than two ticks ago; arg 8
  keeps stock's rule, which the saved-game restore `0x487080` uses) and one at `0x486DC1` stamping
  the free; the state reset at `0x4854A0`, the array's own lifetime, never keyed to GameTime.
- Settle first: that `0x485F50` is only called for local players, and whether a loaded game starts
  at GameTime > 0.
- Tests: a test-only delay on outgoing `0x0B` (`tagpu_dmgdelay.on=K`) on two peers, a Kbot lab
  building under fire; the counter "a `0x0B` applied to a unit younger than K ticks" above 0 before,
  0 after; a third peer for the bystander's copy; single player's creation indices equal to the
  previous build's except where a slot freed within two ticks would have been taken.

**B5 — ghost commander.**

- Measure: on the joiner, log each `0x4861D0` call with its return address (`0x4553E9` for `0x09`,
  `0x48BA05` for a dirty entry, `0x48B49C` for the round robin) and the host commander's first
  appearance; decode the dispatcher's state gate `0x512BC0`.
- The fix follows the cause (the item above). Tests: two peers, rosters every 2 s for 60 s; the
  window at 1500 and at 500; after the fix, the joiner matches the host from the first sample.

**B6 — loaders and the rest. Built ahead 2026-09-25 on its own branch (`worktree-tadr_port_b6`,
from B1's tip at the time), awaiting its review; not landed.** What was built, and where it differs
from the plan below:

- **Built as planned:** the wind and the yardmaps as rows of the fail-closed table (four and three
  rows: 168 sites in the raised build and 42 in the stock-limits build, both MEASURED at launch),
  and the three local fixes, each checked and skipped alone.
  The engine map's *The wind*, *A yardmap parsed past its string*, *The saved-game loader's order
  fallback*, *The stockpile bar's divide* and *A range circle of radius 1* have the disassembly.
- **The path and the host are the engine's own.** A network game is the engine's test,
  `GameingState +0` = 3 (the game start's dispatch, `0x4971C7`), not "some record looks like a
  host"; the host is the engine's host seat `0x456850`, the seat the load already waits for and
  takes the map and the unit limit from, and the seed is its DirectPlay ID. Inside a network game
  nothing local to a peer is read: with no host seat the seed is the map's hash alone, counted. TADR
  instead takes the host's ID with a clock fallback. MEASURED: the host seat is 0 on the host and
  1 on each joiner, and its ID the same on every peer, with two peers and with three.
- **Stock's load call draws nothing.** It finds `next` = 0 (`0x4918FD`) and GameTime 0, so the seed
  at `0x491903` precedes every draw, and the first draw is the first tick's.
- **Changed: B6 logs a line of its own** (`patch_loader_defects`), not a clause of B1's
  `enginefix:` line, plus one line per level load naming the seed.
- **Changed: a stockpile order whose slot index is above 2 draws no bar.** The extra-weapons
  module's slots past the third are not inline slots, and the engine's bar has no reader for them.
- **Evidence.** The wind is reproduced on the previous build: two peers paused at GameTime 825
  read speed 2525, heading `0xF5C6`, and 1498, `0xC827`. On the new build every peer read the same
  next change, speed and heading in five games: a first one, sampled twice (at 795 and at 2003
  or 2006); a second in the same processes; a third after restarting the joiner, where the host
  was on its third level load and the joiner on its first; and, on the host-seat rule, one with two
  peers and one with three (components and ratio equal too). A skirmish reads game mode 2 and
  takes the counter path. The yardmaps' 2440 retail cells are
  byte-identical to the previous build's. The two scratch structures read `2f2f31313100002b2b…`
  from the stack on the previous build and all `0x2F` on the new one, on two launches each. The
  three local fixes rest on the disassembly: none of their inputs is in stock content, and the
  `ShowRanges` cheat could not be typed under injected input.
- **Not measured:** the side measurement this landing was to carry (a tracked unit's death leaving
  an order armed, and our build ghost drawing) did not fit its time budget; nor a network game with
  an AI seat (the engine map's *The wind* says what the rule does with one).

- **Wind** (sim): `0x490C40`'s schedule and value draws from our generator, reset at `0x491903`
  before the first call; `max ≤ min` gives `min` as stock. Two peers: equal wind at a paused tick,
  two games in one process, and a third after restarting one peer.
- **Yardmaps** (sim, load): a detour at `0x42CF5E` into a parse that never reads past the NUL that
  `GetString` wrote inside the 0x400 buffer, stock's table and rules byte for byte. A test `.ufo`
  (a CORSOLAR copy without `YardMap`, and one with `"oo?"`); the 126 retail yardmaps identical.
- **The save loader's fallback** (local): `0x43A58D` `jbe` → `jb` and `0x43A58F` → the "Ready"
  branch `0x43A552`.
- **The stockpile HUD divide** (local): at `0x439D41`, a slot index above 2, a NULL weapon or a zero
  `+0xE4` goes to `0x439D6B`.
- **A range circle of radius 1** (local): at `0x438EDE`, N = 0 skips the circle and its label.

**Measured alongside a landing** (time-boxed, each with its session):

- resurrection's failure branch: counters on `0x405155`/`0x405164` over a few hundred CORNECRO
  resurrections of 1×1 and multi-cell wrecks (B1's session);
- a tracked unit's death leaving an order armed (`0x2CC3`) and our build ghost drawing (B6);
- a departing host in a three-peer game (B5's session; the result goes to group E);
- a cargo unit killed in a transport over land leaving its wreck in the air (B2's session; a
  floating wreck is raised as a visual residual);
- the repair rate per call, ARMCOM against ARMCK on an ARMLLT (B1's session).

## Open questions

- Whether each peer pair's subpackets arrive in the order sent (`0x451DF0`'s DirectPlay flags, the
  TAF tunnel). B4's design does not depend on it; a later acknowledgement scheme would.
- The `0x2C` dirty list stops at 0x200 bytes a tick, filled in slot order. If a skipped unit's
  dirty state is not kept, high slots of a busy block wait for the round robin, up to N ticks: this
  bears on [section A's open question](raised-limits.md#open-questions) about remote lag at 1500.
- Two stock-fix claims in the Delphi recorder, both inactive for stock content and not yet verified:
  a veteran's damage reduction scaling the kill damage (`0x489C2F`), and a ground transport's
  overload (`0x406789`) (evidence Part 4 §7).
- Where stock acquisition stops aiming a flak gun upward. B1 measured that it never aimed above
  29.6°, far from the zero band, but did not disassemble the cut-off.

## Corrections this plan made

- **The CRT's `srand(time(0))` at `0x4971AE` does not seed the wind's schedule.** `rand` keeps its
  state per thread; the wind draws on the game thread, whose stream WinMain seeds (`0x49E8BB`).
  Corrected in the evidence (§6a), the engine map and `gpu-status.md`; each peer's wind still
  differs.
- **The wind reaches the simulation beyond the economy.** The projectile pass `0x49B720` adds it to
  a projectile's position every tick and the fire spread `0x4239C0` reads it; the evidence had
  these readers as smoke drift [INFERRED].
- **The yardmap's stale bytes repeat from launch to launch** for one build on the reference
  setup (B6): the evidence expected two launches to differ. The fix does not rest on it; the bytes
  are still the stack's.
- **The engine map named `w+0xE0` `attackrunlength`.** The loader stores the `coverage` key there
  (`0x42E540`), and the drawer's label says so. Fixed in the engine map and `ui-markers.md`.
- **The attach wrapper starts at `0x48AAC0`**, not `0x48AB40`: 30 callers, and it sends the `0x0A`
  before applying it. Fixed in the engine map and `factory-build.md`.
- **The sim RNG's seed is per peer**, from `QueryPerformanceCounter` at `0x497180`: the engine map's
  `[INFERRED]` is now disassembled.
- TADR's own analyses, corrected in the evidence: flak's zero case is two ±0.35° bands, and its
  `weaponvelocity=0` case faults at `0x49CE6A`, not `0x49CF19`; bit 23 of `+0x111` is `burnblow`;
  the anti-nuke search tests the projectile's *target*, not its position; the order table holds 68
  records for the process, and TADR's dispatch crashes were its own re-entrancy.
