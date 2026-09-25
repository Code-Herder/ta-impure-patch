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

**B1 — damage. Landed 2026-09-25 on local main (`811c311`) after its reviews.** What was built, and where it differs from
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
  source.
- **Changed: the defect is the read, wherever it is inlined — 46 copies, not four.** The census is
  by the data: every compare in `.text` that reads a grid's height in memory — a player's `+0x84`,
  the sight grid's `main+0x14297`, or `+8` through a register pointing at either — holds 57, and 46
  are the read, each a world point sheared by its altitude against one player's grid. It does not
  follow a grid pointer spilled to the stack or a height recomputed from the plot's rows; a second
  pass over those forms finds only the sight grid's builder, which projects terrain into the grid in
  sheared space (not the defect), and the circle's row clips. A search by the shear's arithmetic finds
  39: the builder's and 38 of the 46, the other eight reusing a `y >> 1` computed further up. Beyond
  the eight above: the order resolver `0x43F0E0` (six) and the view player's map build
  `0x467440` (two, which mark units seen for the acquisition) join the table; the cursor picker
  `0x43E490` (six), the build cursor's site test `0x47D2E0` (one), the feature helper `0x4658E0`
  (four), the radar rebuild `0x466DC0`'s projectile dots (four), the particle leaves `0x473590`,
  `0x473A00`, `0x474170`, `0x4745E0`, `0x475470` (two each) and positional sound `0x47F300` (two)
  are local, `fix_los_local`, all or none; `0x474B80`'s two stay
  stock, since nothing calls it. The engine map's census has the table. The lone row tests take one
  generic stub built from the site's stock bytes; the order resolver's second read, whose point
  register stock overwrites first, is a hand stub over its block. Every stub counts its own rows per
  function, an interlocked `LONG` each. What each generic stub relies on is compared: the bytes from
  its z load through its row test (the radar's whole loop for its dots). 179 sites in the raised
  build, 53 in the stock-limits build (MEASURED from the log lines).
- **Added: the observer's side, the sight emitter `0x4825B0`.** Under the ray fan (True and
  Permanent line of sight) a unit's sight is stamped from the same sheared row, stored in the unit
  and bounded against the sight grid; off it, the unit stamps nothing, so an aircraft by the north
  edge revealed nothing to its owner (MEASURED on the previous build: stored row −2, rows 0–9 of the
  owner's map unlit). A census keyed on `+0x84` alone missed it, which is why the census is now by
  the data. The table's hand stub fixes the row once, before stock compares it with the stored row
  and stores it, so the stamp and its later removal — both read the stored words — use one row by
  construction; measured paired (twelve crossings of the boundary, the edge rows all 0 afterwards,
  no count wrapped).
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
  report](raised-limits.md#the-failure-report)). 179 sites in the raised build (130 + 49), 53 in the
  stock-limits build.
- **Measured** on the new build, one peer, the raised and the stock-limits builds alike where both
  ran: every CORFLAK of `b1-victim-units` lost one hit (the unit-repeat counter 48, the 16 past the
  cap found three more times each); all 96 wrecks of `b1-victim-features` took one hit (the
  feature-repeat counter 602); the edge ARMATLAS of `b1-offmap-edge` stamped and shot down within
  2 s; `north` of `b1-los-shear` shot down on station, the flak's first target, and with every
  copy patched (one launch, True line of sight) all 43 sites read back from the running process as
  jumps to stubs that decode as intended, and own rows were taken by `UnitInPlayerLOS` (2548), the
  view player's map (4), the projectile pass (24), the radar's dots (2) and the particle leaves
  `0x474170` (679 342) and `0x475470` (2 395 955) — fire and smoke at altitude by the edge. Not
  reached by that fixture: `0x408090`, the AI probe, the order resolver, the cursor, the site test,
  the feature helper, sound, and three leaves. The cursor's point is the world point under the pointer, whose sheared row
  is the pointer's own, so on the map it never leaves the grid; and a unit at a negative sheared row
  is drawn above the view [INFERRED from the projection], so the human cannot point at `north` at all — its units acquire it, their
  pointer cannot; a corrupted expectation of one
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

**B2 — stacked aircraft. Built 2026-09-25 on its own branch from B1 and reviewed at high; landing next.** What was
built, and where it differs from the plan below (the engine map's *Stacked aircraft in area damage*
has the sites, the disassembly and the numbers):

- **Built as planned:** air only; candidates airborne (`+0x110 & 0x10000003 == 0x10000002`),
  uncarried, not in the off-map bucket, `+0x9E` non-NULL, footprint meeting the blast's rect, not in
  B1's unit set, re-validated at hand-over; every index bounded; served through the engine's own
  per-victim code after every stock victim; simulation, fail closed, in both builds.
- **Changed: served after the walk, not by looping the last cell.** The stub sits at the walk's end
  `0x49A664`, which is also where an empty rect jumps (`0x49A1DF`), so the serving runs after the
  last cell's *feature* too (for a rect that clamps to empty, `air_first` returns NULL and nothing is
  served); the selector `0x49A415` at 2 means
  "serve the next". Stock's victims, their order and values are untouched either way; this way
  stock's last cell is not re-entered.
- **Changed: the pool is rebuilt at the unit tick's call `0x4954ED`, not at `0x49B720`.** Death
  explosions run inside the unit tick, before the projectile tick, and would see a pool one step
  stale. The stamp's airborne path `0x47CF98` adds every unit it files between rebuilds, under a
  lock, since the stamp also runs from the network pump and the loader's restore.
- **Changed: "holds no slot inside the rect" is decided by B1's seen-set, not by a slot test.** A
  unit the walk found is in the set, so the served ones are exactly those stock missed: an aircraft
  no in-rect slot names.
- **Changed, from the reviews: the candidate rule is slot B's exactly, and a dying aircraft is never
  served.** The mask includes bit 29 (the stamp's yardmap path files such a unit in slot A); a
  candidate needs a real bucket (`+0x82` neither NULL nor off-map) and no bit 14. The death
  explosion `0x49B000` has no shooter (`proj+0x52` = 0), so stock's shooter test did not keep the
  dying aircraft out — only the damage receiver's bit-14 drop did; stock never offers it because
  the destructor clears its cells and NULLs `+0x82` first. A failed allocation calls the handler
  `0x49E700` directly (`eng_alloc_or_exit`, B1's sets too), since the handler slot can be empty on
  another thread's window; the stubs open a further page when one fills.
- **Open**: a unit whose `+0x110` turned airborne since the last rebuild through a writer not
  followed by the stamp stays stock's until the next rebuild (150 instructions write `+0x110`; not
  audited one by one); the pump's `0x0E` explosions (`0x4954C8`, before the rebuild) see the
  previous step's pool plus later stamps. Either way a victim can only be missed, never wrongly
  offered.
- **Measured**, the raised build unless said: at the same two bursts of `b2-aa-flak` on
  `b2-stack-atlas`, the previous build hit 12 times, all holders, and left the five holding none at
  150; the new one took all ten, six holding none, from 150 to 10–16, then killed them; served 40
  (37 on the stock-limits build). `air-war`: about 25 µs a sim tick (the rebuild 18.5–23.4, 0.6–0.8
  µs an area-damage call). Two peers: the firer's peer serves, the owner applies the `0x0B`
  (1503 and 1509 served on the joiner, both 150 → 19 on the host); a non-owner holds no HP for a
  remote unit (`+0x108` 0), and both peers held the same five survivors. The fire spread
  `0x49A0C0` calls area damage past the local-owner gate, so every peer computes burning damage —
  stock for the slot holders each peer's tie-break keeps, B2 for every stacked aircraft, which makes
  the damaged set the same on every peer as far as the peers agree on where each aircraft is (a
  remote unit's position, and so its pool membership, lags its owner's). After B1's landing was
  merged in, with the reviews'
  candidate rule: 183 sites installed (130 + 53), 57 in the stock-limits build, the stubs 3 504
  bytes in one page, the limits' weapon sites included (3 120 in the stock-limits build); all ten hit by the first burst (150 → 11–17)
  and killed by the second, served 10. The ride-along: a cargo
  killed in its transport leaves no wreck by design (30 000 from the destructor at `0x48680B`,
  severity 100, ARMSTUMP's corpse type 3 walks dead → heap → none).

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
- Three local counters on the heartbeat (morph, recreate, ghost) become the oracles for B4 and
  B5. Every drop is counted in the `enginefix:` line.
- Class: local (malformed input only). Tests: the two-peer weapon-ID fixture and a ten-peer tier 2
  run with every counter at 0.

**B3 BUILT AHEAD 2026-09-25** (worktree-tadr_port_b3, from `e0ba336`; commits `3c2cec1`, `b3c5a83`,
`569031d`; not landed, not reviewed). What was done, and where it deviates from the plan above:

- **No record-injection lever.** The plan's `tagpu_wirefuzz.on` is dropped. A malformed-message fix
  meets the plan's own evidence bar by disassembly (the identity everywhere else), so instead each
  bound is a **pure C predicate** over the record's field values and the engine's counts
  (`wire_index_ok`, `wire_killer_ok`, `wire_type_ok`, `wire_delta_ok`, `wire_block_ok` in
  `tagpu_patches.c`), and a test-only self-check, **`tagpu_wirecheck.on`**, evaluates each on a table
  of boundary values at attach and logs each verdict against the expected one — a unit test of our
  own code, not traffic. Measured 2026-09-25: 16/16 predicate cases OK.
- **Every stub is a jmp at a clean 5-byte boundary**, verifies the whole stock span first
  (all-or-nothing: one non-stock span leaves the image untouched, `FIX_BYTES`), sets the registers
  stock sets, and continues at the same address; a failed bound goes to the receiver's own drop/exit.
  The `0x0C` destructor is entered at `0x4866E5` so stock's `push edi` (`0x4866E4`) stays and the
  drop's `pop edi` at `0x486E59` still balances.
- **The paired `0x2C` stubs carry the validated type in a game-thread static** (`s_wire_2c_type`,
  `s_wire_rr_type`), not a register, so the engine's downstream register state is exactly stock's; a
  misframed `0x2C` points the bit reader at a static zero dword and jumps to the engine's own
  end-of-list `0x48BA28`, so no round-robin entry is parsed from a stream that cannot be re-framed.
- **The `0x2C` block check is at the receiver's entry `0x48B960`, not per dirty entry**: one check
  there covers the dirty loop, the block sweep `0x48BA28` and the round-robin slot `0x48BAAD`, all of
  which read `[player+0x67]`/`[player+0x6B]`. It is the full slot-run shape
  (`begin + (1 + k·N)·0x118`, `k < 10`, ending `N-1` slots on), not just the `!= 0` stock check.
- **After-create guards `0x48BA05` (S7) and `0x48B49C` (S10)** require the slot now holds the created
  type and, before the parse dereferences it, the dirty create's mover `[[esi]]` or the round robin's
  `[edi+0x9E]` non-NULL — TADR's 13 field faults at `0x48BA07` and the NULL at `0x48B4A6`.
- **The move class is checked in the dirty list only**, not in the round robin as the plan's
  "a move class present" read. The dirty list's sender lists only units with a mover
  (`0x48B782..0x48B786`), so a type without one there is malformed; the round robin carries every
  unit, structures included, and tests the mover itself before its one use (`0x48B6E8`), so there it
  is well-formed and only the type is bounded. The tier-2 run caught the first build refusing the
  AI's structures (types 78 and 112) at the round robin, three entries per joiner (`569031d`).
- **The `0x09` sender-block rule is NOT shipped; the question it waited on is answered.** S1 bounds
  the index `[1, max]` and the type `[1, count)` only. The plan's "index lies in the sender's block"
  is an **observe-only** count on the heartbeat (`send in`/`out`: whether the created slot lies in the
  block of the TRANSPORT sender, the dispatcher's `edi`, saved at `[esp+0]`; `argdiff`: a player
  argument that is not the sender; `unk`: a sender that is no player record), and each sender's first
  create is logged with its name and block. The tier-2 run below settles the AI-seat question: an AI
  seat's creates arrive from the AI's own player record, inside its own block. Turning the observe
  into a drop is a one-line change that needs one more multi-peer run; it is left to the landing.
- **Records accepted, per receiver**, on the heartbeat's `wire:` section (`in 09 0b 0c 0d 2c dirty
  create rr`): the evidence each bound ran on real traffic. `0x0B` and `0x0C` count only the
  dispatcher's call (return `0x455417`/`0x455428`), not the local damage and kill paths that share
  the function; `0x0D` counts a live shooter whose slot weapon matched.
- **`0x0D` diverged shooter** dropped in `wpn_rx_fired` via a new read-only accessor
  `tagpu_weapons_slot_weapon` (NULL past the unit's count, no clamp/VIOLATION), only for a live
  shooter.

Measurements (2026-09-25, Xvfb `:79`, own instances `b3wc`, `b3h1`, `b3j1`, `b3j2`, `b3j3`):

- **Self-check** (`tagpu_wirecheck.on`): 16/16 predicate cases OK; install **ARMED**, all 11 stock
  spans matched byte-for-byte, stubs 384 bytes.
- **Real two-peer traffic** (host + joiner, Two Continents) and **a three-seat AI game** (host +
  AI-on-host + joiner, Town & Country): ~5500 ticks per peer at speed 10, both `in_game=1`, no
  freeze/crash/desync. The `0x2C` receiver's every-tick paths — entry/block (`0x48B960`), delta
  (`0x48B985`), type-matched skip (`0x48B9AD`), after (`0x48BA05`), unsigned remainder (`0x48BA9F`),
  round-robin type (`0x48B40E`) and after (`0x48B49C`) — ran every tick on both peers with **every
  drop counter at 0 and no `wire robustness:` drop line**; morph and ghost read 0 (the recreate oracle
  could not count on that build — below). That is 7 of the 10 `0x2C`-family sites, the most
  timing-sensitive (the round robin fires unconditionally each tick and
  the S9→S10 static type carry is exercised each time).
- **Tier 2, four peers** (host with an AI seat + three joiners, Town & Country, `limits-tier2-p0..p3`
  applied one per human peer after `tools/mp_lobby.sh`, 1499 units each, the armies ordered onto the
  centre; build `569031d`, ddraw.dll md5 `e41dd4166b8253927173f4f22f308558`; every peer paused before
  reading). **PASS**: every drop counter 0 on every peer, every receiver's accepted count above 0, no
  `wire robustness:` drop or stop line, no ErrorLog, no crash:

  | peer | ticks | in `09` | `0b` | `0c` | `0d` | `2c` | dirty | create | rr | morph | recreate | ghost | send in / out / argdiff / unk |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | host | 5355 | 4497 | 23540 | 2624 | 27106 | 13348 | 420868 | 3 | 13348 | 0 | 3 | 0 | 4497 / 0 / 0 / 0 |
  | j1 | 2605 | 4502 | 17568 | 800 | 16068 | 10499 | 245854 | 2 | 10499 | 0 | 4 | 0 | 4502 / 0 / 0 / 0 |
  | j2 | 5376 | 4506 | 11498 | 2181 | 17784 | 18679 | 413202 | 2 | 18679 | 0 | 4 | 0 | 4506 / 0 / 0 / 0 |
  | j3 | 5370 | 4506 | 26321 | 2389 | 29816 | 18684 | 402929 | 1 | 18684 | 0 | 4 | 0 | 4506 / 0 / 0 / 0 |

  j1's commander died at tick 2605 and the lobby's default `MultiCommanderDeath` ended its game
  (ENDMSN, `in_game=0`) — the rule, not a fault; its counters stop there. Every other peer played on.
  Every `0x09`, `0x0B`, `0x0C`, `0x0D`, `0x2C` dirty-create and round-robin site ran on real traffic,
  which closes the two-peer runs' gap for the receivers. The drop paths themselves rest on the
  self-check and the disassembly: a well-formed game sends no malformed record, by construction.
- **What the run caught, both fixed in `569031d` and re-run to the table above.** On the first build
  every joiner stopped three round-robin entries (types 78 and 112, the AI's structures — the
  move-class check that belongs to the dirty list only, above). And the recreate oracle read 0 with
  dirty creates on the counter: it compared `CreateFromNetwork`'s return address with the call sites
  `0x48BA00`/`0x48B497` instead of the return addresses `0x48BA05`/`0x48B49C`, so it could never
  count. It now reads 3 or 4 per peer: the creates the full-state stream made on that peer. morph and
  ghost stayed 0: no slot changed type under a create, and
  the engine's ghost sweep never fired.
- **The AI-seat answer.** On each joiner the AI seat's first create logs as
  `0x09 from sender 4 (type 3, 'AI:B3H1'), player arg 4, slot 2, sender block 1..1500: in the sender's
  block`, and across all four peers `send in` equals the accepted `0x09` count (18 011) with `out`,
  `argdiff` and `unk` at 0. The AI's units reach the joiners as `0x09` from the AI's own record, so the
  plan's sender-block rule would refuse nothing a well-formed game sends, the AI's included.

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

**B6 — loaders and the rest.**

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
