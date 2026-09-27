# B. Simulation bug fixes — the plan

## Summary

Section B brings TADR's fixes for **defects in the stock 3.1 engine** into our stack, as our own
code, over six landings and a seventh. The owner decided every choice below on 2026-09-25 **[DECIDED]**, in a
grill that followed [the evidence pass](sim-fixes-evidence.md). **All six landings are landed on local
main (2026-09-25), each after its review**, and B7, four defects the veterancy survey found, landed
with section C's C3 on 2026-09-26. The rules
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
- **Ghost commander: the cause measured, then both halves.** A start-of-game create is refused by
  the dispatcher's state gate on a peer still loading, not for want of the sender's block; the
  receiver keeps it and replays it at the in-play entry, so the owner's own create arrives and
  nothing is guessed. And a dirty `0x2C` create takes the position its entry's move payload
  carries, bounded to the map, for disassembled move classes only (the owner chose both,
  2026-09-25, from B5's measurement). The Assist is not ported in any form.
- **The wire's unit indices: bounded.** `0x09`, `0x0B`, `0x0C` and the `0x2C` receiver; on a bad
  `0x2C` field the parser stops at the engine's own end of list, since past it the bitstream cannot
  be framed.
- **A `0x0D` whose shooter has diverged: dropped and counted.** Firing from the local slot would
  invent a projectile the owner never fired.
- **Wind: one shared wind, re-seeded every game.** Our own generator with its own state, reset at
  each level load from the host's DirectPlay ID and a hash of the map's name; single player takes
  the same path, seeded from the counter stock seeds its RNG with. (Built with DirectPlay's session
  instance GUID in place of the host's ID: see B6's deviations below.)
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
  hash sets with no capacity. The `0x0E` receiver reaches area damage only on the game thread — the
  dispatcher passes its mask 4 (`0x45200B`) only in net state 6 (`0x454762..0x45478B`), set at
  `0x498445` after the load — so a stack would hold too; the per-thread frames cost nothing and
  need no argument about who calls area damage.
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

**B2 — stacked aircraft. Landed 2026-09-25 on local main (`74dc093`) after its reviews.** What was
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

**B3 LANDED 2026-09-25** on local main (worktree-tadr_port_b3, from `e0ba336`, main merged at `74dc093`
and again before landing; two high reviews, their findings acted on in `49c640c` and `5193986`, and two
focused reviews of the rounds after them — the padded copy, the splitter, the pump's pointer, the
`0x2C`'s nested unit references `b9a5692` and the `0x0A` attach `cefcac8` — whose three lows were
acted on in `18667b4`). What was done, and where it deviates from the plan above:

- **No record-injection lever.** The plan's `tagpu_wirefuzz.on` is dropped. A malformed-message fix
  meets the plan's own evidence bar by disassembly (the identity everywhere else), so instead each
  bound is a **pure C predicate** over the record's field values and the engine's counts
  (`wire_index_ok`, `wire_killer_ok`, `wire_type_ok`, `wire_delta_ok`, `wire_ref_idx`,
  `wire_attach_ok`, `wire_block_ok` in `tagpu_patches.c`), and a test-only self-check, **`tagpu_wirecheck.on`**, evaluates each on a table
  of boundary values at attach and logs each verdict against the expected one — a unit test of our
  own code, not traffic. The compiler folds every case to a constant, so it checks the predicates'
  C and nothing more; the stubs and their drop paths rest on the disassembly. Measured 2026-09-25:
  22/22 predicate cases OK. The table has 33 since `cefcac8` (`wire_ref_idx`'s six, `wire_attach_ok`'s
  five); those eleven, with six more (a child and parent both 0, no array, the raised limit of
  15 000), ran first on the host against the predicates compiled from the source (8/8 and 9/9 OK),
  then through `tagpu_wirecheck.on` in the game: 33/33 OK on both peers of the final round below.
- **Every stub is a jmp at a clean 5-byte boundary**, verifies the whole stock span first
  (all-or-nothing: one non-stock span leaves the image untouched, `FIX_BYTES`), sets the registers
  stock sets, and continues at the same address; a failed bound goes to the receiver's own drop/exit.
  The `0x0C` destructor is entered at `0x4866E5` so stock's `push edi` (`0x4866E4`) stays and the
  drop's `pop edi` at `0x486E59` still balances.
- **The paired `0x2C` stubs carry the validated type in a game-thread static** (`s_wire_2c_type`,
  `s_wire_rr_type`), not a register, so the engine's downstream register state is exactly stock's; a
  misframed `0x2C` points the bit reader at a static zero dword and jumps to the engine's own
  end-of-list `0x48BA28`, so no round-robin entry is parsed from a stream that cannot be re-framed.
  **Game thread by the gate**: `0x451FD0` (called at `0x491369`) is the only filler of the receive
  mask table `0x512BC0` for these codes and gives `0x09`, `0x0B`, `0x0C`, `0x0D`, `0x0E` and `0x2C`
  the mask 4 (`0x451FE7..0x45200B`); the dispatcher passes mask 4 only in net state 6
  (`0x454762..0x45478B`), which `0x498445` sets on the game thread after the load (state 5 during
  it). The static also rests on the receiver being serialised with no re-entry between the type
  check and the after-create check: the direct-call closure of `0x4861D0` never reaches the pump
  `0x453D40`.
- **The `0x2C` is parsed from a zero-padded copy of itself** (`wire_s2c_copy`, at `0x48B92B`, before
  the header's reads), and its reader is bounded by the message's length (`wire_2c_len`). The
  receive `0x4534E0` takes one message into the buffer at `main+0x2A38` and learns its length, which
  it hands only to the statistics call `0x415EF0`; the two calls (`0x453595` after the transport
  `0x462F30`, `0x45361F` after a raw DirectPlay receive, the `-p` switch below 0) go through a note
  that keeps the buffer and length **per thread** (TLS), since the receive runs on whichever thread
  pumps. On the transport a `0x2C`'s length **is** its `[16]` size field: the splitter `0x463790`
  reads it (`0x4639E5`) and queues the message only when it fits the packet (`0x46393E`); a raw
  receive has no splitter, so the length is the smaller of the two, at most 0xFFFF. The receiver's
  entry copies that many bytes into this thread's buffer (0xFFFF + 48 bytes, allocated at the
  thread's first `0x2C`; none → the message is dropped, `nocopy`), zeroes the 48 after them, and
  hands the copy to stock's `0x48B933` as the reader's buffer. **Every read of the parse is then
  inside memory we own, whatever the message says**, and a message that ends inside an entry reads
  zeros for that one entry before the next check stops the stream. Before each read the stubs
  precede, the position plus the read's width must fit the length: the header and first delta at
  the entry, each dirty type at the delta stub, the flag bit (and, when it is set, the round-robin
  type) at `0x48BA5E`, and the round-robin entry at `0x48B40E`; the end is keyed by the reader's
  address, and the entry checks the reader holds the copy. A `0x2C` that is not the buffer the
  receive described is dropped (`stale`). The copy changes nothing for a well-formed message:
  nothing reads the reader after the handler returns (the dispatcher's case `0x4553EE` jumps to the
  pump's back edge `0x455F50`, the reader is a local of `0x48B920`'s frame), and nothing advances
  by it — each message is its own receive, and the splitter advances by the size it reads from the
  packet itself.
- **The padding's size, by disassembly of every path past the last check.** Two stretches are read
  by engine code whose width depends on the data, and neither loops on data it reads. A dirty
  entry's move-class payload `[vt+0x24]` is followed by the next delta (16) at `0x48BA19` before the
  delta stub checks it; the move classes' vtables (`0x4FD458`, `0x4FD488`, `0x4FD980`, `0x4FD9B0`,
  `0x4FD9E0`; no other table's `+0x24` reaches the reader) give `0x44EFD0` (reads nothing),
  `0x44F5C0` (1 + 2 + 3 × 32 = 99: its loop runs a 2-bit count) and `0x490A10` (2 + the larger of
  `0x44E080`'s 8 + 32 + 16 + 16 + 16 + 96 = 184 and `0x44E9C0`'s 1 + 6 × 32 + 16 = 209, + 2 = 213),
  so 213 + 16 = **229 bits**. The round robin's tail after `0x48B40E` reads 16 + 8 + 8 + 2 + 1
  (`0x48B4A9..0x48B557`), then 15 + 8 or 32 × 3 + 16 × 3 (`0x48B56B..0x48B60F`), and 32
  (`0x48B6F2`): at most 211; the calls on the way (`0x48B090`, `0x48AB70`, `0x47D0E0`, `0x47CC30`,
  `0x4827B0`) read nothing from it. The header before the entry check is 56. So at most 229 bits,
  29 bytes, past the last checked bit. The message's end can sit 3 bytes into a dword, and the
  reader `0x415DC0` also loads the next dword when a read reaches the end of one
  (`0x415E0C..0x415E3E`): 29 + 3 + 8 = 40, rounded up to **48**. A `typedef` in `tagpu_patches.c`
  fails the build if the padding falls below that sum.
- **The pump's message pointer follows the buffer.** The pump `0x453D40` takes its message pointer
  once, before its loop (`0x453D90` → `[esp+0x10]`; the back edge `0x455F59` re-enters at the call
  `0x453D94`), and the dispatcher's cases read the message there; nothing else writes that slot or
  takes its address. `0x4534E0` grows the buffer when a message outgrows it (`0x453565`, `0x4535EE`: `0x4D84A0`,
  a realloc [INFERRED: block and size in, the block out]), so after a growth that moves the block,
  every later message of the same pump call was dispatched from the freed one. The fix is the
  small one: the note re-points the pump's `[esp+0x10]` at the buffer the message was just received
  into, after checking the return address `0x453D99` (its frame — `0x4534E0`'s one caller, its one
  push, the three argument pushes — is in the spans verified at install). It is the identity when
  the buffer did not move, and `pump` counts the times it did. **Well-formed traffic does not reach
  it**: the buffer holds 0x2000 bytes from the main menu on, before any session (MEASURED
  2026-09-25, `main+0x2A34`), and stayed there, at the same address, through a four-peer tier-2 game
  on every peer (`pump` 0, below). On the transport the splitter's longest fixed-length message is
  186 bytes (code `0x20`, the table `0x512AD8` read live) and a `0x2C` is capped near 0x200 bytes by
  its sender (`0x48B7F6`), so only a malformed message — a `0x2C` whose size field runs past 8 KB
  inside a large packet, or an oversized raw receive — grows the buffer. That makes it a
  malformed-input fault, B3's class; the fix was built before the measurement answered and is
  kept for that reason.
- **The splitter ends a packet at a message too short to advance it** (`wire_split_*`). The
  splitter walks a packet by each message's length and advances by it, so a `0x2C` whose size is 0
  keeps its counting loop (`0x4638F0..0x463947`) on one message for good, and its third walk
  (`0x463AD3..0x463B8B`) queues that message until the 512-entry queue is full. Both take the
  length at one place (`0x463939`, `0x463B33`, after the `0x2C` and table paths join); a length of
  0, or a `0x2C` below its 7-byte header, ends the split there through the engine's own end for an
  unknown code (`0x463949`; `0x463B91`), counted (`split`). The length table itself holds 0 for
  codes 4 and `0x2B` (read live 2026-09-25), so those stop the split too. The counting pass decides how many
  messages the queuing pass `0x4639BC` takes, so that pass never reaches it. A length of 0 hangs
  stock's counting loop whatever the code, so no packet stock survives carries one, and every
  sender writes a `0x2C` with its 7-byte header: it is the identity for a well-formed packet.
- **The `0x2C` block check is at the receiver's entry `0x48B960`, not per dirty entry**: one check
  there covers the dirty loop, the block sweep `0x48BA28` and the round-robin slot `0x48BAAD`, all of
  which read `[player+0x67]`/`[player+0x6B]`. It is the full slot-run shape
  (`begin + (1 + k·N)·0x118`, `k < 10`, ending `N-1` slots on), not just the `!= 0` stock check.
  `k` is a **rank**, not the record's index: `0x4858A6..0x4858E0` assigns blocks in the order of an
  insertion sort (`0x485657..0x4856C0`) that, in a network game, compares the records' DirectPlay ids
  `[rec+4]` — so record 2 can own the block at slot 6001.
- **The `0x2C`'s nested unit references** (`b9a5692`). Two values inside the stream are unit indices
  scaled into the array with no bound, each an optional reference whose 0 the engine itself takes as
  no unit; an index past the array now takes that 0, counted, and a valid one leaves every register
  and byte as stock does. One predicate serves both, `wire_ref_idx` (0 stays 0, `[1, max]` stays
  itself, anything else becomes 0 — `wire_index_ok`'s range). By disassembly of every decoder the
  dirty list and the round robin reach ([the engine
  map](../exe-reverse-engineering.md#the-network-receivers-take-a-unit-index-straight-off-the-wire-0x09-0x0b-0x0c-0x2c-disassembled-2026-09-25)):
  - **A move payload's target**, `0x44E080` (the `0x4FD9E0` class's parse, `0x490A10` → `0x490A4F`):
    a `u16` at `0x44E0D0`, sent as the target's own `+0xA8` or 0 (`0x44DDC0`); stock sends 0 to
    `0x44E0DA` (`xor eax,eax`) and scales any other at `0x44E0DE..0x44E0F9`, then hands it to the
    reference set `0x489690`, which reads the unit's `+0xA6` and links into its `+0xA2`, or for NULL
    clears the reference. The stub is at `0x44E0DE` (stock `8B 15 E8 1D 51 00`, `mov edx,[0x511DE8]`;
    span `0x44E0D5..0x44E103` verified) and continues at `0x44E0FC` with the slot, or NULL (`target`).
  - **The round robin's carrier**: under the full state's flag bit, a 15-bit index at `0x48B56B`, sent
    as the carrier's own `+0xA8` (`0x48B2A4..0x48B332`), packed as the parent id of the attach record
    `0x48AB70` applies at `0x48B58B` and scales there unbounded (`0x48ABAF..0x48ABC4`); its 0 is
    `0x48AB70`'s own no parent (`0x48ABA9`), the detach. The stub replaces the record's
    `mov [esp+0x17],ax` at `0x48B574` (stock `66 89 44 24 17`; span `0x48B55B..0x48B58F` verified),
    writes the index or 0 (`carrier`) and continues at `0x48B579`. The skip-the-call alternative
    was not taken: 0 is the engine's own no-unit value at both sites.
  - **None needed**: `0x44E080`'s other `u16` (`[this+0x10]`) is a piece number `0x43DEF0` bounds
    itself; `0x44E9C0` reads flags, six 32-bit values and a heading; `0x44F5C0` a bit, a 2-bit count
    and that many coordinate pairs into three pairs of room; `0x490A10` a 2-bit selector and a 2-bit
    value; `0x44EFD0` nothing; and the rest of the round robin's tail (`0x48B4A2..0x48B6FC`) names
    no other unit: its fields go into the entry's own slot, its state mask to `UNITS_SetStateMask
    0x48B090`.
  - The same scaling in `0x48AB70` is reached from the wire by the `0x0A` receiver too: the next
    bullet.
- **The `0x0A` attach, bounded at its case** (`cefcac8`). The dispatcher's `0x0A` case `0x4553FE`
  (jump-table entry 8, `0x455FA4`) hands the wire record to `0x48AB70`, which scales both its
  child (`+1`, `0x48AB8A`) and its parent (`+3`, `0x48ABAF`) into the unit array unbounded, reads
  each one's `+0x110` and writes the parent's `+0x8A`. `0x48AB70`'s callers are four: that case,
  the local wrapper `0x48AAC0` (`0x48AB62`, 30 callers, the ids of live units), and the round robin
  (`0x48B58B`, the carrier bounded at `0x48B574`; `0x48B5C5`, the slot's own id and parent 0), so
  the check guards the case, not the function. What 0 means: a child 0 makes `0x48AB70` return
  having done nothing (`0x48ABC7`), and no sender writes one (the wrapper reads its child's `+0x110`
  before it sends); a parent 0 is the detach (`0x48ABA9`). So `wire_attach_ok` requires the child
  in `[1, max]` and the parent 0 or in `[1, max]`. The stub replaces the case's `mov eax,[esp+0x10];
  push eax` (stock `8B 44 24 10 50`; the case's 15 bytes through its `jmp 0x455F50` and the table
  entry verified); a record that fails goes to `0x455F50`, the pump's back edge that every case and
  every unhandled code takes (drop `0a`), and one that passes runs the displaced two instructions
  and continues at `0x455403` (in `0a`). A dropped child 0 has the effect stock gives it, nothing;
  only the count differs. Not examined: the attach point `+5`, stored into the child's `+0xF9`,
  is a piece of the carrier, not a unit index.
- **After-create guards `0x48BA05` (S7) and `0x48B49C` (S10)** require the slot now holds the created
  type and, before the parse dereferences it, the dirty create's mover `[esi]` and its object `[[esi]]`,
  or the round robin's model object `[edi+0x9E]` (the Object3do, set at `0x485DCC`; `+0x9A` is the
  COB script), non-NULL — TADR's 13 field faults at `0x48BA07` and the NULL at `0x48B4A6`.
- **The move class is checked in the dirty list only**, not in the round robin as the plan's
  "a move class present" read. The dirty list's sender lists only units with a mover
  (`0x48B782..0x48B786`), so a type without one there is malformed; the round robin carries every
  unit, structures included, and tests the mover itself before its one use (`0x48B6E8`), so there it
  is well-formed and only the type is bounded. The tier-2 run caught the first build refusing the
  AI's structures (types 78 and 112) at the round robin, three entries per joiner (`569031d`).
- **The `0x09` sender-block rule is a drop** (`blk`), as the plan intends once the AI-seat question
  was answered (the tier-2 run below: an AI seat's creates arrive from the AI's own record). A wire
  `0x09` is dropped when its slot is not in the block of its TRANSPORT sender — the dispatcher's
  `edi`, saved at `[esp+0]` — or when the sender is no player record (the pump names record ten, one
  past the ten, for a sender it cannot find, `0x453E14`). `argdiff` still counts a player argument
  that is not the sender, and each sender's first create is logged with its name and block.
- **Records accepted, per receiver**, on the heartbeat's `wire:` section (`in 09 0a 0b 0c 0d 2c
  dirty create rr`): the evidence each bound ran on real traffic. `0x0B` and `0x0C` count only the
  dispatcher's call (return `0x455417`/`0x455428`), not the local damage and kill paths that share
  the function; `0x0A`'s stub sits on the dispatcher's path alone; `0x0D` counts a live shooter
  whose slot weapon matched. Drops: `09 blk 0a 0b 0c kill 2c len stale nocopy 0d split target
  carrier`; `pump` counts a re-pointed pump pointer.
- **The B4/B5 oracles**: `morph` (a create onto a live slot whose type changes), `dcreate`
  (`CreateFromNetwork` called by the dirty list, returning to `0x48BA05`: a dirty entry whose type is
  not its slot's, the unit made from the entry rather than from its own `0x09` — into an empty slot,
  the ghost commander's mechanism), `rcreate` (the same from the round robin, `0x48B49C`) and `ghost`
  (the engine's ghost sweep, a round-robin type 0 over an occupied slot). For B5's cause,
  `CreateFromNetwork`'s own refusal — `0x486229` returns 0 when the create's player has no block — is
  counted per caller (`noblock 09 dirty rr`, the first 64 logged with the caller, player, slot and
  type), and `noarr` counts `wire_s09` finding no unit array to bound against.
- **`0x0D` diverged shooter** dropped in `wpn_rx_fired` via a new accessor
  `tagpu_weapons_slot_weapon` (NULL past the unit's count, no clamp/VIOLATION; past slot 2 it reads
  through `side_row`, which may build or rebuild the module's own side table and log it, as every
  side-row read does), only for a live shooter. Stock divides by the **local** slot weapon's velocity
  `+0x68` (`0x49CE62..0x49CE6A`): it faults #DE only when that is 0, and otherwise builds a mixed
  projectile (the packet weapon's branch, the local weapon's velocity). Evidence §8 classes it
  simulation, only when diverged; it rides the weapon-ID site `0x49D280`, a row of the fail-closed
  table, so it is held fail-closed with it.

Measurements (2026-09-25, Xvfb `:79`, own instances `b3wc`, `b3h1`, `b3j1`, `b3j2`, `b3j3`):

- **Self-check** (`tagpu_wirecheck.on`): 16/16 predicate cases OK; install **ARMED**, all 11 stock
  spans matched byte-for-byte, stubs 384 bytes.
- **Real two-peer traffic** (host + joiner, Two Continents) and **a three-seat AI game** (host +
  AI-on-host + joiner, Town & Country): ~5500 ticks per peer at speed 10, both `in_game=1`, no
  freeze/crash/desync. The `0x2C` receiver's every-tick paths — entry/block (`0x48B960`), delta
  (`0x48B985`), type-matched skip (`0x48B9AD`), after (`0x48BA05`), unsigned remainder (`0x48BA9F`),
  round-robin type (`0x48B40E`) and after (`0x48B49C`) — ran every tick on both peers with **every
  drop counter at 0 and no `wire robustness:` drop line**; morph and ghost read 0 (the recreate oracle
  could not count on that build — below). That is 7 `0x2C`-family sites, the most
  timing-sensitive (the round robin fires unconditionally each tick and
  the S9→S10 static type carry is exercised each time).
- **Tier 2, four peers** (host with an AI seat + three joiners, Town & Country, `limits-tier2-p0..p3`
  applied one per human peer after `tools/mp_lobby.sh`, 1499 units each, the armies ordered onto the
  centre; build `569031d`, ddraw.dll md5 `e41dd4166b8253927173f4f22f308558`; every peer paused before
  reading). **PASS**: every drop counter 0 on every peer, every receiver's accepted count above 0, no
  `wire robustness:` drop or stop line, no ErrorLog, no crash:

  | peer | ticks | in `09` | `0b` | `0c` | `0d` | `2c` | dirty | create | rr | morph | recreate (dirty + rr) | ghost | send in / out / argdiff / unk |
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
  count. It then read 3 or 4 per peer. It counted the dirty list's and the round robin's creates
  together (they are `dcreate` and `rcreate` now): the host's 3 are its 3 dirty creates (`create=3`),
  and with `morph` at 0 every one went into an empty slot — units placed from a dirty entry and not
  from their own `0x09`, the ghost commander's mechanism, three times on the host. The joiners' 1–2
  dirty creates leave 2–3 round-robin creates. `ghost` stayed 0: the engine's ghost sweep never
  fired.
- **The AI-seat answer.** On each joiner the AI seat's first create logs as
  `0x09 from sender 4 (type 3, 'AI:B3H1'), player arg 4, slot 2, sender block 1..1500: in the sender's
  block`, and across all four peers `send in` equals the accepted `0x09` count (18 011) with `out`,
  `argdiff` and `unk` at 0. The AI's units reach the joiners as `0x09` from the AI's own record, so the
  plan's sender-block rule refuses nothing a well-formed game sends, the AI's included; it is a drop
  since `49c640c`.
- **The review round, two peers** (`49c640c`, raised ddraw.dll md5 `3a9453029afeb5e4412d0de7fb29d4c9`,
  stock-limits `dc0c97414ca5fa252464d7d9f45a0c2b`; host + joiner on Town & Country,
  `limits-tier2-p0`/`p1` after `tools/mp_lobby.sh`, the armies ordered onto the centre). Install
  **ARMED**, all 14 spans stock; the live sites and stubs disassembled from the running host, each
  jump and exit as built; stubs 384 bytes, the page 3 888. The fight ran until the host had
  destroyed the joiner's 1 500 units and the game ended (ENDMSN) at tick 7 005 — the counters are
  final there. **Every drop counter 0** on both (`09 blk 0b 0c kill 2c len stale 0d`), no ErrorLog, no
  crash:

  | peer | in `09` | `0b` | `0c` | `0d` | `2c` | dirty | create | rr | morph | dcreate | rcreate | ghost | noblock 09/dirty/rr | noarr |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | host | 1500 | 3473 | 1500 | 7144 | 6981 | 201067 | 0 | 6981 | 0 | 0 | 0 | 0 | 0/0/0 | 0 |
  | joiner | 1499 | 9103 | 1418 | 16411 | 6994 | 222557 | 0 | 6994 | 0 | 0 | 1 | 0 | 0/0/0 | 0 |

  The length bound passed every one of 13 975 messages and 423 624 dirty entries and every round-robin
  entry; the sender-block drop passed all 2 999 creates. The joiner's units took slots 1..1500 and
  the host's 1501..3000: the block is the rank, not the record. `create`/`dcreate` read 0: no unit
  reached either peer through the stat stream before its `0x09`, which in the four-peer game happened
  1–3 times a peer; the length checks on that path are the delta stub's, which every dirty entry
  passed, so a four-peer rerun would add a count, not a covered check. `rcreate` counted one on the
  joiner. The in-play heartbeat line was then about 1 430 bytes of its 1 700.
- **The padded-copy round, four peers** (`60f49db`, raised ddraw.dll md5
  `07bc42e798d9aed808a51651fc2e6c04`, stock-limits `dca01cc3987504b337c7fbe5a93624c3`; host with an AI
  seat + three joiners, Town & Country, `limits-tier2-p0..p3` after `tools/mp_lobby.sh`, 1 499 units
  each, the armies ordered onto the centre; every peer paused before reading). Install **ARMED**,
  all 21 spans stock, 22/22 predicate cases OK; the new sites and stubs disassembled from the
  running host — `0x48B92B` → the copy, drop `0x48BAC8`, the displaced `xor esi,esi; push 8;
  lea ecx,[esp+0x14]` then `0x48B933`; `0x463939` → `0x463949` or `and eax,0xFFFF` then `0x46393E`;
  `0x463B33` → `0x463B91` or `mov edx,eax; and edx,0xFFFF` then `0x463B3B` — each calling the
  function its offset in an unstripped link of the same objects names. Stubs 480 bytes, the page
  3 984 of 4 096. **PASS**: every drop counter 0 on every peer (`09 blk 0b 0c kill 2c len stale
  nocopy 0d split`), `dcreate` above 0 on every peer, no ErrorLog, no crash:

  | peer | ticks | in `09` | `0b` | `0c` | `0d` | `2c` | dirty | create | rr | morph | dcreate | rcreate | ghost | pump | noblock 09/dirty/rr | noarr |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | host | 7873 | 4497 | 24878 | 2957 | 38488 | 17883 | 516832 | 3 | 17883 | 0 | 3 | 0 | 0 | 0 | 0/0/0 | 0 |
  | j1 | 2125 | 4501 | 13934 | 653 | 11378 | 8583 | 195663 | 2 | 8583 | 0 | 2 | 2 | 0 | 0 | 0/0/0 | 0 |
  | j2 | 7883 | 4506 | 14331 | 2446 | 28442 | 25742 | 519805 | 1 | 25742 | 0 | 1 | 3 | 0 | 0 | 0/0/0 | 0 |
  | j3 | 7878 | 4506 | 29475 | 2655 | 41555 | 25749 | 516065 | 3 | 25749 | 0 | 3 | 1 | 0 | 0 | 0/0/0 | 0 |

  j1's commander died at tick 2125 and `MultiCommanderDeath` ended its game (ENDMSN), as in the
  first four-peer run; its counters stop there. Every one of 77 957 `0x2C` messages was parsed from
  its copy, and the length bound passed every dirty entry and round-robin entry; the nine dirty
  creates are the path the plan's ghost commander takes, each passing the delta and type checks and
  the after-create guard. The receive buffer held 0x2000 bytes at one address from the menu to the
  pause on every peer. The in-play heartbeat line is now about 1 470 bytes of its 1 700.
- **The final round, two peers** (`3e6a684`, raised ddraw.dll md5
  `65b43b35f3fdccc47c93821b395ed83a`; host + joiner through `tools/mp_lobby.sh`, `limits-tier2-p0`/`p1`,
  the armies ordered onto each other, a factory building; both paused before reading). Install
  **ARMED** on both, stubs 544 bytes, `limits: installed 183 sites`, `tagpu_wirecheck.on` 33/33 OK.
  **PASS**: every drop counter 0 on both peers (`09 blk 0a 0b 0c kill 2c len stale nocopy 0d split
  target carrier`), the `0x0A` attach receiver on real traffic, no ErrorLog, no crash:

  | peer | ticks | in `09` | `0a` | `0b` | `0c` | `0d` | `2c` | dirty | rr | rcreate |
  |---|---|---|---|---|---|---|---|---|---|---|
  | host | 1951 | 1503 | 2 | 6226 | 777 | 24936 | 12315 | 342850 | 12315 | 0 |
  | joiner | — | 1510 | 13 | 11270 | 614 | 10575 | 12306 | 333682 | 12306 | 1 |

  The landed build adds to it only the site table's bound (`18667b4`, install code) and main's merge;
  its install lines were read again on it before landing.

**B4 — stale hits.**

- The incarnation: a `u32` per slot, in DLL tables sized from the engine's own slot count at the
  unit array's allocation (the design below), bumped at create; the
  companion message's format and its binding to its `0x09`/`0x0B` are this landing's first design
  step, after A′3's `0x0E` companion. Bandwidth measured: `0x0B` is the most frequent message.
- The hold: a detour at `0x486036` (first-free, skipping a slot freed less than two ticks ago; arg 8
  keeps stock's rule, which the saved-game restore `0x487080` uses) and one at `0x486DC1` stamping
  the free; the state reset at `0x4854A0`, the array's own lifetime, never keyed to GameTime.
- Settle first: that `0x485F50` is only called for local players, and whether a loaded game starts
  at GameTime > 0. **SETTLED [DISASSEMBLED].** The level start's creates filter the player's
  `+0x73` to 1 or 2 (`0x4976BB`); the other, `0x496EE0`, also takes a type-3 player, but only when
  `0x435100` answers 2 (`0x4978EB`), skirmish, which has no remote player (a network game answers
  3, the value the block sort `0x485842` tests). GameTime is 0 at every level entry, a loaded save
  included (`0x498180` before state 6 at `0x498445`).
- Tests: a test-only delay on outgoing `0x0B` (`tagpu_dmgdelay.on=K`) on two peers, a Kbot lab
  building under fire; the counter "a `0x0B` applied to a unit younger than K ticks" above 0 before,
  0 after; a third peer for the bystander's copy; single player's creation indices equal to the
  previous build's except where a slot freed within two ticks would have been taken.

**B4 DESIGN (2026-09-25, written before the code).** What the plan above left open, decided.

*The binding is containment, not adjacency.* Nothing is known about whether one peer's subpackets
reach another in the order sent (the open question below), so a companion sent NEXT TO a stock
`0x0B` could arrive apart from it, or without it. The companion therefore **replaces** each message:
the stock record travels inside it, and a peer applies the record only after checking the
incarnation that rides in the same bytes. Loss, duplication and reordering then act on the pair as a
unit, by construction. Both are tagged `0x05` messages, A′3's idiom: `0x41` bytes, `m[1] = 0` so
stock's chat handler ignores them (`0x463CA7`), and a tag byte outside TADR's (`0x2B..0x31`,
`0x60`) and A′3's `0x49`:

| tag | replaces | bytes |
|---|---|---|
| `0x4A` | the `0x09` of `0x456050` (its only sender, called from the create `0x486115`) | `05 00 4A`, the stock 23-byte `0x09` at `m+3`, the owner's birth `u32` at `m+26`, zeros |
| `0x4B` | the `0x0B` of `0x489BB0` (its only sender: the calls `0x489CB9`, `0x489CCD`) | `05 00 4B`, the stock 9-byte `0x0B` at `m+3`, the sender's stamp of its victim `u32` at `m+12`, zeros |

The two `call 0x451DF0` sites and the one in `0x456050` call ours instead, with the send's own
signature (`stdcall(net, msg, len)`, `ret 0xC`), so every caller is covered by three sites. The
receiver is the `0x05` dispatch slot (`0x455F90`), in BOTH builds; A′3's raised-build receiver
becomes one branch of it. **A bare `0x09` or `0x0B` is dropped and counted** (their dispatch slots
`0x455FA0`, `0x455FA8`): no peer of this build sends one, so a bare one is a peer on another build,
which is not a supported game. That is the answer to "a `0x0B` with no companion": it cannot come
from this build, by construction, and one that comes from elsewhere is refused rather than guessed.

*The receiver mirrors the dispatcher exactly.* The dispatcher gates a message on its type before the
switch (`0x45473F`: `[0x512BC0 + 4·type]`, bit 2 in state 5, bit 4 in state 6, bit 1 otherwise,
filled by `0x451FD0`: `0x05` passes every state, `0x09` and `0x0B` only state 6). The companion's
`0x05` has passed its own gate; ours then applies the gate of the type it carries, from the same
table, so a carried `0x09` or `0x0B` is refused wherever the bare one would have been. It then
enters stock's own handler with the return address stock's case would push — `0x4861D0` returning
to `0x4553E9`, `0x489CE0` returning to `0x455417`, both `jmp 0x455F50` — so B3's receiver bounds and
its sender-block rule, which key on those return addresses, run on the carried records unchanged.

*The incarnation is the owner's GameTime at the create, made strictly increasing per slot.* At the
owner's take (`0x486036`), `birth = max(GameTime, previous birth + 1)`; the previous birth is kept
from the array's allocation (`0x4854A0`) on. A counter would do for a copy made by a `0x4A`, but not
for the one path that makes a copy without it: **the `0x2C` recreate** (a dirty entry, `0x48BA05`,
or the round robin, `0x48B49C`), which B5's ghost commander proves is real. A `0x2C` carries the
sender's GameTime (`[32]` at `0x48B955`, stored at `player+0x18` at `0x48B963`), so a copy made from
one records a **lower bound**: "the unit in this slot at the owner's time g₀". So one `u32` stamp,
bit 31 set for a lower bound:

- a copy made by a `0x4A` takes its birth, exact; a copy made by a `0x2C` takes g₀, a lower bound
  (`0x48634F`, CreateFromNetwork's success exit, told apart by its return address); a local unit
  takes its birth, exact;
- a hit carries the attacker's stamp of its copy of the victim.

**The owner applies a hit iff `birth ≤ stamp`** (bit 31 ignored). For an exact stamp that is
equality. For a lower bound: the unit the `0x2C` described was alive at g₀, so it was freed at some
f ≥ g₀; the hold never reuses a slot in the tick that freed it, so the next unit's birth is at
least f + 1 > g₀; so `birth ≤ g₀` holds for the unit the `0x2C` described and for no later one. It needs
GameTime not to go back between g₀ and the create, and every `0x2C` is sent in play, where GameTime
only increments (`0x4954C0`; its other writes are at a level's entry, `0x491979`, `0x4971BB`,
`0x498180`, and the shell's clock `0x44A696`).

**A bystander refuses only a hit provably aimed at another incarnation**, and applies the rest as
stock does, counting the undecidable ones. Every value is the owner's GameTime: both exact, it
applies iff they are equal; an exact birth b against a lower bound g₀ on the other side (the unit
alive at g₀, so born at or before it), it refuses iff b > g₀; two lower bounds, or a stamp with no
information, are undecidable. Refusing only what is proven matters because a `0x2C` copy keeps its
lower bound for its life (the `0x2C` recreates a slot only when the type changes): the first
build's "exact and equal" made every bystander drop every hit on the other players' start
commanders, and with them `0x467950`, the health bar's timer. Applying cannot change shared
state [DISASSEMBLED]: `0x489CE0` sets pending death only for a victim whose player is local
(`+0x73` 1 or 2, `0x489EC6`), paralysis likewise (`0x489E24`), every branch of `0x406F80` tests the
same, and the owner's round robin rewrites a copy's hit points (`0x48B235` → `0x48B4B2`) within N
ticks; a copy's death comes from the owner's `0x0C`.

*The hold.* The loop's free test at `0x486036` jumps to a stub: an occupied slot continues the loop
at `0x486040` as stock; a free slot is taken unless the requested index (arg 8) is 0 and the slot
was freed at GameTime f with `GameTime − f < 2` (unsigned, so a stamp from before a GameTime reset
never holds). The free is stamped at `0x486DC1`, the destructor's store of type 0; the stamps are
reset at `0x4854A0`. The decision is made once per create, at the block's first free slot: when
it is held, the block's first unheld free slot is taken instead; when every free slot is held,
the one freed longest ago is taken if that was in an earlier tick (the fallback, counted); a slot
freed in THIS tick is never taken, because a birth equal to its free time would accept a lower
bound from a `0x2C` sent earlier that tick, and forcing it to f + 1 would refuse a later `0x2C`'s
bound for the unit's whole life. **Invariant — the owner's**, from the tick order of `0x495490`
(GameTime++ `0x4954C0`, the pump `0x4954C8`, units `0x4954ED`, projectiles `0x495513`, players
`0x464F80`): a slot freed in tick t, in any phase, and not needed by the fallback is empty for the
whole of tick t + 1, so every reader on the owner that runs once a tick sees it empty at least once
before a new unit takes it. **It does not hold on the other peers, and B4 does not change that:**
`CreateFromNetwork 0x4861D0` writes a copy's type at `0x4862A2` with no hold, so a `0x0C` and the
slot's next `0x4A` can arrive in one pump batch, and a per-tick reader there (a local weapon aimed
at a remote enemy, `0x48A295`; the smooth-motion pose pairing) never sees the slot empty. That is
stock behaviour, and it does not break the owner's rule: a hit such a reader then sends carries the
incarnation now in the slot, so it is not stale by the rule's definition.

*When the hold fails a create.* With every free slot of the block freed in this very tick, the
create returns NULL below the cap. `0x485F50`'s callers [DISASSEMBLED]: the four factory and build
callers (`0x4028EA`, `0x403D5B`, `0x405104`, `0x41409B`) link the result to the order
(`0x489690`, which takes NULL) and test the link: NULL shows "Unable to create any more units"
(`0x4028FF` → `0x47F780`), and the first three sleep the order 300 ticks (`0x439E80`) while
`0x41409B` returns 8 at once (`0x4140BD`); `0x41794F` ignores it;
`0x48718E` (the saved-game restore, arg 8 nonzero, never held), `0x488462` and `0x488700` test it;
`0x497002` and `0x4977BB` run in the level load, before anything is freed. **`0x4653D9`** is
the Deathmatch respawn (described in the engine map, *Unit identity on the wire*): it uses the unit
at `0x465414` with no test, so stock faults there on every NULL `0x485F50` can return — the block
with no free slot, and below the cap a type 0 (`0x485F7C`), a type without def `+0x241` bit
`0x800000` (`0x485FAA`) or the type's own limit def `+0x15A` (`0x485FE4`) — and the hold would add
one more. B4 tests it at `0x4653DE` and **writes nothing**: a NULL takes the block's own end
`0x4654FB` and leaves the countdown `main+0x39239` at −1, exactly as stock's fire leaves it.
That is the rule the guard keeps: every reader of the countdown sees stock's own sequence. It
could not be a reset to "one decrement before the fire": the countdown's readers include
`0x46554F`, which runs `0x401360` for a local player only while the countdown is below 0 (and
`main+0x3923B` bit 2 is clear) — `0x401360` zeroes the player's `+0xA4`/`+0xA8` and adds up each
live unit's production [INFERRED: the economy's per-pass sum] — and the path that decrements it runs only while the respawn's trigger holds (`0x465154`'s options
bit `0x40` clear and `0x490360` nonzero), so a 0 left behind when the trigger lapses would stop
`0x401360` for good where stock leaves −1. As it is, while the trigger holds the countdown reloads
at the controlled player's next pass (`0x46518B`, to 4) and fires six of its passes later (a pass
is the player's `+0xF0` gate, every 30 ticks, `0x465083..0x465092`): the retry, placement search
included, about 180 ticks on. On a NULL that never clears — the block full, or one of the type
refusals above — stock faults; ours retries at every fire while the trigger holds.

**The remaining exception** [rule 7]: a create that comes in a tick that freed every free slot of
its player's block fails where stock would have taken one of those slots — a factory or builder
then says "Unable to create any more units" (three of the four callers also sleep the order 300
ticks), and a Deathmatch respawn waits for its countdown's next fire. Nothing faults that stock
does not. `0x485F50`'s eleven callers pass arg 8 = 0 except the
saved-game restore `0x48718E`, which runs before play.

*Class: sim, fail closed, both builds.* Fifteen rows in the table: the three dispatch slots, the
three send calls, the free test, the free, CreateFromNetwork's exit, the array's allocation and the
players phase's create, written; and compared unchanged, the two continuations the carried records return to (`0x4553E9`,
`0x455417`) and the two lengths the senders push (`0x45605C` `push 0x17`, `0x489CA8` `push 9`),
which are the sizes the companions copy.

*Test lever and counters.* `tagpu_dmgdelay.on` holds K: each outgoing hit waits until GameTime has
advanced K ticks and goes out at the next send opportunity (any outgoing hit, any incoming
companion). The oracle, per receiver: a hit applied to a unit whose local creation is younger than K
ticks. The previous build gets the same lever and oracle as a scratch-only patch (the three send
calls and B3's `0x0B` bound), so the two builds are measured with one instrument. Counted on the
heartbeat's `hits:` section: companions out and bytes, companions in, applied/refused per rule
(the bystander's applies split into proven and undecidable), bare messages dropped, copies by
kind (exact, a real lower bound, unknown), creates the hold moved, took by the fallback or
failed, the Deathmatch respawns put off to their countdown's next fire, the oracle.

*The tables.* Sized from the engine's own slot count, `u16 10·N + 1` as `0x4854A0` computes it
(`0x4854E3..0x4854EF`), bounded by it at every index, and never freed: a level whose count exceeds
the room gets a new table published through one pointer, and the old one stays, so a stale pointer
still reads memory the DLL owns (rooms are powers of two from 16 384, so three tables at most).
The loading thread (`0x497C70` → `0x497180`) resets them in the level init (`0x497581` →
`0x4917D0` → `0x4854A0`) and makes the level's first creates (`0x496EE0`, `0x4977BB`) while the
main thread pumps the network in state 5, where the gate keeps the dispatch out of them; in play
every access is on the game thread [INFERRED from the tick's order].

*What B4 does not close.* The attacker named in a hit (`0x0B`'s `+3`) is not stamped: a hit applied
to the right victim can still name a recycled attacker for the retaliation bookkeeping
(`0x406F80`, `+0xF0`). A delayed `0x0C` or `0x2C` from the owner itself is a question about one
sender's order, the open question below; B4 fixes the case that needs no answer to it, a hit from a
peer that is not the owner. B5's queue now holds `0x4A` companions refused in state 5, not bare
`0x09`s.

**B4 LANDED 2026-09-25** on local main (`e10201f` on its own worktree from B3's `bd5582b`; reviewed
at high by two reviewers, fix rounds `c9f939b` and `3eb0237` each reviewed again, B3's final tip
merged and the merge reviewed on its own). The first build's numbers follow; the fix round's and
the merged build's are after them. As designed above; `fix_stale_hits` in `tagpu_patches.c`. The install line reads
`limits: installed 189 sites` in the raised build (176 before: fourteen rows, less A′3's `0x455F90`
row, which B4 now owns) and `the simulation fixes' 64 sites installed` in the stock-limits one;
`tagpu_wirecheck.on` runs the rules' 16 cases, all OK.

*Measured, three peers on Town & Country* (`scenarios/b4-guns.json` on the host, laser towers
and two fusion plants; `scenarios/b4-victims.json`, four ARMCK on hold, applied on a joiner every
2 s; the third peer a bystander; `tagpu_dmgdelay.on=30` on every peer; eight minutes, GameTime
about 14 670 at the pause). The previous build is `bd5582b` with the same lever and oracle as a
scratch-only patch (the three sends, `CreateFromNetwork`'s exit, B3's `0x0B` bound):

| | previous build | B4 |
|---|---|---|
| hits sent by the towers' peer | 10 195 bare, 9 B each | 12 545 carried, 65 B each |
| owner: applied / refused | 5 772 / — | 5 689 / 2 956 |
| owner: applied to a unit younger than 30 ticks | **1 562** (27 %) | **0** |
| bystander: applied / refused | 6 254 / — | 6 481 / 2 370 |
| bystander: applied to a copy younger than 30 ticks | **1 406** (22 %) | **1** (below) |
| owner's creates / moved by the hold | 829 / — | 829 / 67 (85 held slots skipped) |
| bare `0x09`/`0x0B` dropped, malformed, delay overflow | — | 0 everywhere |
| copies: exact / made by the `0x2C` | — | 846 / 2 on the bystander, 19 / 1 on the owner |

The first build counted every `0x2C` copy as a lower bound, found or not; most were not (the
review's first finding, below). The `0x2C` copies and the three companions refused by the state gate (1 on the owner, 2 on the
bystander) are the start of the game: the other peers' commanders, whose create arrives while a
peer still loads and which the `0x2C` recreates — B5's cause, seen from here.

*The one young hit on the bystander is the oracle's, not a stale hit* [INFERRED for its cause]. The
first build's bystander applied a hit only when its copy's stamp and the hit's were exact and equal, and the
owner's births rise strictly per slot, so an applied hit names the incarnation the bystander holds.
The oracle measures something else: the age of the bystander's own copy, in the bystander's own
ticks. A correct hit counts as young when the bystander made its copy late relative to the
attacker, or when the attacker's 30 ticks passed faster than the bystander's; the peers are not in
lockstep (at the pause the host read 14 675 and the joiners 14 685). A second eight-minute run of
the new build, with every copy, every hit computed and every young application logged (a
scratch-only patch), read 0 young on both receivers (owner 5 519 applied / 3 129 refused, bystander
6 398 / 2 483, of 12 176 hits), so the event was not caught in the act.

*Bandwidth.* Every carried message is `0x41` bytes: a hit costs 65 bytes where stock sent 9, a
create 65 where stock sent 23. At the towers' rate, about 25.6 hits a second, that is 1.66 KB/s of
payload against 0.23 KB/s. The engine's send layer packs subpackets into packets of up to `0x42A`
bytes, so the packet count grows less than the bytes; the length is the dispatcher's own for
`0x05` (`0x512AD8 + 5·4`), fixed.

*Single player* (Two Continents, AI laser towers against ARMCK applied every 2 s, the lever armed
for its trace): every one of 188 creates took the slot stock's first-free would have taken, except
3, each skipping exactly the slots the hold counts (`held=3`); on the owner of the network run, 67
of 829. Across two separate runs the previous build's creates and B4's agree for the first 27,
until the applies land on different ticks (106 against 118) — wall-clock applies, not the hold.
The stock-limits build: 86 creates, all first-free (no slot freed within two ticks of a create).

*The stock-limits build*, the same protocol for three minutes: 4 481 hits carried; the owner
applied 2 064 and refused 1 032, the bystander 2 364 and 844; young 0 on both; nothing bare,
malformed or overflowed; 38 slots held.

*Deviations from the plan.* The Kbot lab under fire became four ARMCK re-applied every 2 s: a
build queue is not in the scenario format, and the applies create into the freed slots as fast.
The delay lever releases a held hit at the next send opportunity (an outgoing hit, an incoming
companion on the game thread), so a hit waits at least K ticks, not exactly K.

*The fix round* (`c9f939b`, from the two high reviews; each finding verified against the
disassembly first). The `0x2C`'s record is found by the block that holds the slot; the bystander
refuses only what is provably stale; the hold decides once per create and falls back to a slot
freed in an earlier tick; the Deathmatch respawn's untested create is tested; the tables are sized from
the engine's count and never freed; the carried record reaches the stub in `eax`. Install: 190
sites raised (fifteen B4 rows), `the simulation fixes' 65 sites` stock-limits; hitcheck 23 cases,
all OK; `slots=15001` raised and `slots=2501` stock-limits (N = 250), one table each.

Measured, the same three peers and protocol, eight minutes, plus one light laser tower of the
host's beside each joiner's start commander for the last minute (a scratch fixture):

| | owner (the victims' peer) | bystander (the third peer) |
|---|---|---|
| hits applied / refused | 5 434 / 3 200 | 6 212 same + 52 undecidable / 2 582 stale |
| applied to a unit younger than 30 ticks | **0** | 4 (among the undecidable, below) |
| copies: exact / real lower bound / unknown | 20 / 2 / 0 | 848 / 2 / 0 |
| creates moved by the hold / fallback / failed | 73 / 0 / 0 | — |

Both joiners computed real lower bounds for the two start commanders the `0x2C` recreated, with
none left unknown, so the peer that is not first in DPID order found its sender's record too. The
bystander's 52 undecidable applies are the host's hits on the other joiner's commander, whose
copy there is the `0x2C`'s: the first build refused every one of them. The other commander's owner
applied the 4 hits on it, and the first joiner, a bystander there, applied them undecidable. The bystander's 4 young applies are
undecidable ones, by definition not provably stale; the owner's rule is the one that decides a
death, and it applied none young. The fallback and the retry did not fire: both need a block
with no unheld free slot, which eight minutes of four kbots at a time do not reach; they rest on the
disassembly above and the self-check's cases.

*The merged build, two peers* (`ae5e632`: B4 with B3's final tip; raised ddraw.dll md5
`f2802b871fbeb515611e3453cd04a8d1`; Town & Country through `tools/mp_lobby.sh`, `limits-tier2-p0`/`p1`,
1 499 units a side, about 3.5 minutes of battle, both peers paused at tick 7 838 / 7 844). Install:
`limits: installed 197 sites` (B3's 183, fifteen rows, less A′3's `0x455F90`), stale hits in the
fail-closed table (stubs 336 of 4 096 bytes), wire robustness ARMED, the shared stubs 4 016 bytes in
one page; the rules' 23 cases and the wire's 33 all OK; the stock-limits build `the simulation
fixes' 72 sites installed`, `hits: slots=2501`. Every `wire:` drop counter 0 on both peers, no bare
`0x09`/`0x0B`, and each peer's hits sent are the other's received:

| | host | joiner |
|---|---|---|
| `0x09` sent / received | 1 500 / 1 500 | 1 500 / 1 499 |
| `0x0B` sent / received | 9 645 / 3 049 | 3 049 / 9 645 |
| owner applied / refused, dead | 2 922 / 0, 127 | 9 008 / 0, 637 |
| copies: exact / real lower bound | — / 0 | — / 1 |

The joiner's one missing create is the host's commander, refused in the joiner's load and
recreated by the round robin as a real lower bound (`gate=1`, `bound=1`, `rcreate=1`) — the ghost
B5 closes; every `0x0B` received is accounted for (3 049 = 2 922 + 127; 9 645 = 9 008 + 637).

**B5 — ghost commander.**

- Measure: on the joiner, log each `0x4861D0` call with its return address (`0x4553E9` for `0x09`,
  `0x48BA05` for a dirty entry, `0x48B49C` for the round robin) and the host commander's first
  appearance; decode the dispatcher's state gate `0x512BC0`.
- The fix follows the cause (the item above). Tests: two peers, rosters every 2 s for 60 s; the
  window at 1500 and at 500; after the fix, the joiner matches the host from the first sample.

**B5 DESIGN (2026-09-25).** The cause was measured first, on the previous build (B4's tip
`a8e529e`), and the owner chose the fix from it: **queue the refused create, and fix the dirty
create's position** [DECIDED].

*The cause is the state gate, not the sender's block.* Every peer creates its commander from its
loader thread (`0x485F50` at `0x4977BB`) after the load barrier, while it is still in state 5, and
the dispatcher passes a create only in state 6 (`0x45473F`, `0x512BC0`; the carried `0x09` since B4,
whose receiver applies the same table). The peer whose load ends later refuses the others'
commanders: in every two-peer start measured, the joiner (`gate=1` there, 0 on the host). The
refused commander then exists on that peer only when the round robin re-creates it, as a lower-bound
copy (`bound=1`); and a commander that moves first comes back sooner through a dirty `0x2C` entry,
whose create record (`0x48B9B6..0x48B9FB`) takes the **slot's own** position — `(0,0,0)` in a fresh
array — after which the proxy walks it toward the owner's goal from the map's corner. The engine
map's *A create refused during the load* has the load state, the two threads that pump and the
move classes.

*The queue.* In B4's `0x05` receiver, before the carried `0x09`'s own gate: in state 5, where that
gate refuses it, the message is held with the case's player argument (`[esp+0x14]`, whose low byte
is the sender's record index) and the sender's DirectPlay id (`record+4`), only when `edi` is that
record (the pump pairs them, `0x453E84..0x453EA1`, a compared row). The queue is emptied at the load's start
(`0x497F5E`, the load state's first call) and drained **before the first tick**, at the in-play
entry's call of the frame function (`0x49842F`). That function's catch-up ticks — up to five —
still run in state 5, and so does the frame function's own pump after them (`0x4968CB`); **a
create refused in either is made at once**, in the pump it arrives in (a tick's `0x4954C8`, or
`0x4968CB`), which is where state 6 makes a create: the dispatcher's sender
test has just passed on it, and only the state test is skipped. The measurement below is why:
the host's create reaches the late peer in its catch-up ticks, after the load's last pump.
A held create is replayed when its sender still passes the dispatcher's own sender test
(`0x4547AD..0x4547E2`) under the same DirectPlay id, and its slot is empty or holds an older
incarnation by B4's stamps and is not a local player's unit; it goes through B4's receiver past its
state test only (`hit_rx_create_armed`, which hands on the whole carried message, so
`CreateFromNetwork`'s exit reads the owner's birth 23 bytes into its record and the copy is exact)
and into `CreateFromNetwork` from a stub that puts the sender in `edi` as the `0x09` case does. The
receivers that key on the `0x09` case's return `0x4553E9` (B3's sender-block rule, B4's stamp) treat the
stub's return alike.

*A create before the first tick is safe by construction.* `CreateFromNetwork` calls what the local
create `0x485F50` calls, and over an occupied slot first destroys its unit through `0x4864B0`
(`0x486237..0x486244`), the unit tick's own destroy, which sends a `0x0C` only for a local
player's unit (`0x48664B`) — a slot the replay never takes. Stock's loader runs `0x485F50` for
this peer's commander in state 5 before any tick (`0x4977BB`); neither body reads the net state or GameTime, and B4's stamp
at `0x48634F` keys on the return address and the armed index, not the state. Nothing ticks before
`0x49842F`'s call: the frame function only moves its profile counters and reads the clock before
`0x495490`. A create in a catch-up tick is made in the tick loop's own pump, `0x4954C8`, or in the
frame function's `0x4968CB`, the ones play reaches through the same frame function (`0x4995B8`, `0x4996A5` in the state-6 function
`0x499200`). The replay skips the dispatcher's state test and nothing else.

*The kills.* Every unit message is state 6 only (`0x451FD0` gives `0x0B`, `0x0C` and `0x2C` mask 4
like `0x09`), so a unit the owner destroys while this peer loads has its `0x0C` refused too, and a
replay alone would make a unit that no longer exists. Stock removes such a copy — the owner's round
robin sends type 0 for its empty slot and the receiver marks the copy dying (`0x48B415..0x48B42F`),
within N of the owner's ticks, 50 s at 1500 — but B5 would have made it, so B5 closes it: the
dispatcher's refusal branch in state 5 (`0x45477F`) notes a refused `0x0C`, which cancels the
latest create held for its slot from its sender (counted `killed=`). One refused during the
catch-up ticks, for a copy made before state 6 (by the replay or in a catch-up tick; up to 1024
are listed), marks that copy dying exactly as the engine's ghost sweep does, bit 14 of `+0x110`
(`swept=`), and the unit tick destroys it. The
owner alone sends a unit's `0x0C` (`0x48666D`, the one send of its 11 bytes), and a slot is taken
only once free, so one sender's stream for a slot alternates create, kill: the latest held create
is the unit the kill names.

*What stays un-queued.* A refused `0x0B` or `0x2C` is not held: the owner's round robin rewrites
every unit's state, hit points included, within N ticks (`0x48B235` → `0x48B4B2`), and the dirty
list re-sends a moving unit's motion every tick it moves.

*The position.* The dirty create's `call 0x4861D0` at `0x48BA00` calls a stub that reads the
entry's move payload ahead of its decoder, from a copy of the `0x2C`'s bit reader, in the engine's
bit order: for the ground proxy (vtable `0x4FD488`, decoder `0x44F5C0`) the path's point 0, x and z;
for the air proxy (`0x4FD9E0`, `0x490A10`) selector 2's x, y, z (the motion `0x44E9C0`, the unit's
dead-reckoned position). Selector 1 (`0x44E080`'s vector, a **goal**), selectors 0 and 3, and a
ground payload with no point leave stock's record. These are every class `0x43DC00` gives a
remote unit; the local classes' readers read nothing (`0x44EFD0`). Ground point 0 is the node the
owner's unit last **reached**, never the next one: the owner's mover step `0x44F1A0` (vtable
`0x4FD458` + 8, called through `0x43DD20` from the unit tick at `0x48AFAA`) drops the path's front
only once the unit is within 5 px of point 1 (`0x44F1D7..0x44F235`, the squared distance against
`0x19`), and a straight order's path is [the unit's position, the goal] (`0x44F3F2..0x44F417`), so
a straight move keeps its origin as point 0 until the unit is within 5 px of its goal.

The payload is read only from B3's copy of the `0x2C` and only within the message's length: the
reader must be the one B3's copy stub set up for this message and still point at its copy (whose
48 zero bytes cover the dword a last read touches), and a read that would pass the message's end
is refused before it is made (`short=`), leaving stock's record. Only the bits the stub reads are
tested — 35 for a ground payload, 99 for an air one — not the payload's remaining points or
fields: the engine's decoder reads those from B3's zero-padded copy, and B3's length check before
the next entry's type (`0x48B985`) stops the stream if the payload ran past the end. A well-formed
entry's payload is always inside its message.

**B5 requires B3 armed.** B3 stays a local fix for its own purpose, but a peer where it did not
arm would place dirty creates at the slot's stale position while the others take the payload's —
a peer silently playing by stock's rule, which *How B fixes are held* forbids. So
`fix_ghost_commander` takes `fix_wire_bounds`' result and refuses the fail-closed table unless it
is `ARMED`: the limits line fails with the reason and the startup report names it. The
heartbeat's `unbound=` stays as the in-game guard, for a reader that is not B3's.

*Invariants* (the code's header states each beside the code):

- **Bounded:** 64 records per sender, ten senders; a record past a full queue is counted and left
  to the round robin, where stock leaves every one.
- **Emptied once per level by the level's own lifetime**, never by GameTime: cleared at the load's
  start, on the game thread before `0x4982CA` creates the loader thread; drained before the first
  tick. Every record leaves exactly once: replayed, cancelled by its kill, or counted.
- **An ordering, not a window.** Two threads pump during a load (the game thread at `0x49852E`, the
  loader at `0x49727D`), so records go in under a lock. The loader's last pump precedes its last
  store (bit 1 of `main+0x38D75`, `0x497C62`), and the game thread reaches `0x49842F` only after
  reading that bit (`0x498342`), so the drain sees every create the load refused. After it only
  the game thread pumps (the catch-up ticks' `0x4954C8`, the frame function's `0x4968CB`), and
  nothing more is held: a create refused there is made in that pump.
- **Kills in one sender's arrival order.** If one peer's messages could reach another out of order
  (the open question below), a kill could arrive before its create; the create is then replayed,
  and the owner's round robin marks the copy within N ticks — stock's bound for any ghost.
- **Only active senders, only empty or older slots**, by the dispatcher's sender test and B4's
  stamps; never a local player's unit.
- **The game thread only**: the drain, the catch-up ticks' creates and the dying mark check their
  thread and do nothing on any other.
- **On the map by construction**: a position is taken only when `0 ≤ x < W·16` and `0 ≤ z < H·16`
  px (`main+0x14233`/`+0x14237`, each 1..4096); an air unit's y only up to `0x1FF` px, the
  ceiling the goal point `0x44E3C0` clamps its y to (`0x44E4F1`).
- **The payload inside its message**: read only from B3's copy, under B3's reader, within the
  message's length in bits; otherwise stock's record.

*Class: sim, fail closed, both builds.* Fifteen rows: `0x497F5E`, `0x49842F`, `0x45477F` and
`0x48BA00` written; `0x497F54`, `0x497F64`, `0x497C5F`, `0x498348`, `0x48B9F5`, `0x4861D0`,
`0x496790`, `0x454788`, `0x455F50`, and the two layouts the stubs read without writing —
`0x48B933` (the `0x2C` reader's buffer, word and bit) and `0x453E84` (the sender's record from
`[esp+0x14]`, the index the pump found by matching `main+0x4C9` [INFERRED: the sender's
DirectPlay id] against each record's `+4`, `0x453DBD..0x453E12`, stored at `0x455F78`) —
compared; and the whole
table is refused unless B3 armed (above). The hold itself is code in
B4's receiver, whose slot `0x455F90` is B4's row; B4's `hit_rx_create` is split into its gate and
`hit_rx_create_armed`, and a catch-up create hands its record back through `regs[PR_EAX]` as a live
one does. Lever `tagpu_ghostq.off` (test only: nothing is held or noted, the position
still applies); `tagpu_wirecheck.on` runs 21 rule cases at attach (the queue's age rule, the map
bound, the bit reader, the ground and air payloads, a goal not taken, a position read one bit
past its message's end and one that ends on its last bit, which create a kill cancels). Counters
on the heartbeat's `ghost:` section, its alert fields first.

**B5 LANDED 2026-09-25** on local main (from `c2e5850` on its own worktree from B4's `a8e529e`, B4's
fix rounds merged at `fbda477`, main with B4 landed at `cf1ebe2`; reviewed at high by two reviewers,
their findings acted on in `89a76b2`/`73573aa`, and a focused review of that round acted on in
`58afd89`/`2573ce1`). `fix_ghost_commander` in
`tagpu_patches.c`. Merged with main after B4 landed, the install line reads `limits: installed 212
sites` in the raised build and `the simulation fixes' 87 sites installed` in the stock-limits one,
B5's fifteen rows among them; `tagpu_wirecheck.on` runs its 21 rule cases, all OK, in both, beside
B4's 23 and B3's 33. The
table ran on the first build, which replayed at the state-6 store; the starts after it (below the
table) on the builds that replay before the first tick.

*Measured, two peers on Two Continents* (host and joiner by `tools/mp_lobby.sh`, rosters of both
commanders' slots every 2 s for 60 s from the first in-play tick; "moved": the host's commander
ordered to `(848,7344)` the moment the host is in play). The joiner's copy of the host's commander:

| build | limit | commander | joiner at t = 0 | joiner matches the host from | joiner's counters |
|---|---|---|---|---|---|
| previous | 1500 | idle | missing | t = 50 s | `gate=1`, `recreate=1`, copy `bound=1` |
| B5 | 1500 | idle | `(368,7664)`, the host's | t = 0 | `q=1 replay=1`, copy `exact=1` |
| previous | 500 | idle | missing | t = 18 s | `gate=1`, `recreate=1`, `bound=1` |
| B5 | 500 | idle | the host's | t = 0 | `q=1 replay=1`, `exact=1` |
| previous | 1500 | moved | `(1,−2)`, walking to `(45,574)` while the host's reached `(842,7344)` | t = 50 s | `create=1 recreate=1 gate=1 bound=1` |
| B5 | 1500 | moved | `(369,7660)`, host `(374,7656)` | t = 0 | `q=1 replay=1`, `exact=1` |
| previous | 500 | moved | `(2,−3)`, walking to `(39,504)` | t = 18 s | `create=1 recreate=1 gate=1 bound=1` |
| B5 | 500 | moved | `(369,7661)`, host `(375,7656)` | t = 0 | `q=1 replay=1`, `exact=1` |
| B5, queue off on the joiner | 1500 | moved | `(368,7661)`, host `(375,7655)` | t = 0 | `gate=1 create=1 bound=1`, `pos ground=1`: "at (368, 7664) from its ground payload; stock's record had (0, 0)" |

A moving copy trails the host's by the peers' tick offset (6–9 ticks, 5–10 px) and equals it once
the unit stops. On every start of the first build with the queue on, the joiner's replay line
reads `replayed slot 1 from sender 1 (birth 0, type 34) at GameTime 5`, with `inactive`, `stale`,
`bad`, `cleared`, `offthread` and `over` 0, and the host holds nothing (`q=0`); the host had both
commanders at every sample of every run (31 samples each), both builds.

*Before the first tick* (1500, moved, one start each). With a drain before the first tick and one
after the state-6 store, the first found nothing (`q=1`, made by the second at GameTime 5): the
host's create reaches the joiner in its catch-up ticks, after the load's last pump. On the final
build the drain at GameTime 0 again finds nothing, and the create is made in the pump of the
joiner's first tick — `created slot 1 from sender 1 (birth 0, type 34) at GameTime 1, in a
catch-up tick`, `now=1`, copy `exact=1`, `gate=0`; the commander at the first sample `(368,7663)`
against the host's `(369,7660)`, equal at `(842,7344)` from t = 20 s.

*On the merged tree* (`fbda477`, 1500, moved, one start). This start dealt the host the second
block, so its commander is slot 1501 (the move order went to 1501 on the host). The joiner's drain
at GameTime 0 again finds nothing, and the create is made in its first tick: `created slot 1501
from sender 1 (birth 0, type 34) at GameTime 1, in a catch-up tick`, `now=1`, copy `exact=1`,
`gate=0`; the commander at the joiner's first sample `(369,7661)` against the host's
`(375,7655)`, equal at `(841,7344)` from t = 20 s. On both peers the carried create went out at
65 bytes (the bare `0x09`'s 23), `bad`, `bare`, `over` and `holdfail` read 0, and every `wire:`
drop and `noblock` count is 0 over 1 743 and 1 676 parsed `0x2C`.

*The dirty create alone, on the final build* (`8474cf4`, 1500, moved, `tagpu_ghostq.off` on the
joiner, one start). The joiner refused the host's create (`gate=1`) and the host's commander, slot
1, came back through the dirty create, its payload read from B3's copy within the message:
`dirty create slot 1 type 34 at (368, 7664) from its ground payload; stock's record had (0, 0)`,
`pos ground=1`, `unbound=0`, `short=0`, copy `bound=1`. At the joiner's first sample it stood at
`(368,7663)` against the host's `(375,7655)` — point 0, the origin of a move begun a second
earlier — and equal at `(841,7344)` from t = 20 s. Every `wire:` drop and `noblock` count is 0 on
both peers (1 777 and 1 715 parsed `0x2C`).

*B4's lower-bound copies at game start are gone.* The refused create now makes an exact copy: the
joiner's `gate` and `bound` read 0 and `exact` 1 in every B5 start with the queue on, against `gate=1 bound=1` on the
previous build. A lower-bound copy remains possible only through the round robin or a dirty entry
for a create the queue did not hold (below). The slot and its sender bear out that a record's index is
not its block's: on the joiner record 0 is its own and holds the second block (1501 or 501), and
record 1, the host, the lower DirectPlay id, holds the first, whose slot 1 the replay filled.

*Where the build differs from the owner's decision.*

- **A create refused in a catch-up tick is made in that tick, not held.** The decision replays
  held creates before the first tick; the create that matters arrives after that point, in the
  five catch-up ticks, and holding it to the state-6 store left the commander missing for those
  ticks. It is made where play makes it, so the late peer has it from the tick it arrives in.
- **Ground point 0 is the last node the unit reached, not its position**: a straight order's
  origin, until the unit is within 5 px of its goal (*The position* above). No field of the ground
  payload is the unit's position. It lands on the true start in the runs above, where the
  commander had just set off; a unit created well along a long straight move is placed at its
  origin and trails the owner's until the round robin's full state for its slot writes the owner's
  x, y, z into it (`0x48B5CA..0x48B6A7`) — at most N owner ticks, 50 s at 1500 and 16.7 s at 500,
  the same bound that corrects stock's `(0,0,0)`, from a start on the unit's own path instead of
  the map's corner.
- **The kill note is new simulation behaviour.** The decision queues creates; B5 also cancels a
  held create when its unit's `0x0C` is refused during the load, and marks a copy made before
  state 6 dying (bit 14 of `+0x110`, the engine's own ghost sweep) when its `0x0C` is refused in the
  catch-up ticks. Without it the queue would make units their owner had already destroyed, which
  stock never makes, and each would last until the owner's round robin marked it, up to N ticks.
- **An air unit's selector 1 is a goal and is not taken**; that dirty create keeps stock's record.

*What B5 does not close.* A refused `0x0B` or `0x2C` is not held (the design says why). The kill
note rests on one sender's messages arriving in order; out of order, a replayed ghost lasts until
the owner's round robin marks it, at most N ticks. The kill paths are proven by disassembly and
the rule cases, not provoked live: a kill has to land inside the late peer's remaining load, a
fraction of a second in these starts.

*What the final build has not run.* The table's five B5 starts ran the first build, which
replayed at the state-6 store. The build that replays before the first tick has run three starts
(1500, the commander moving): two with the queue on — one before and one after B4's fix rounds
were merged — where the drain found nothing and the create was made in the first catch-up tick,
and one with the joiner's queue off, where the dirty create placed it. The 500-unit and idle
starts were not re-run on it. Its live replay and three peers were measured afterwards (below).

*Measured on local main `8d033d1`, three peers (2026-09-26).* `b5dh` hosting, `b5dj` and `b5dk`
joining, Town & Country, 1500, `tools/mp_lobby.sh`; three scripted runs (a scratch driver: launch,
battle room, the start, counters, stop), wall 321 s, 126 s and 136 s, plus one start driven step
by step before the scripted-run rule.

- **Natural three-peer starts** (two): every peer's drain before the first tick found nothing
  (`q=0 replay=0`) and made each late create in a catch-up tick (`created slot … at GameTime 1`
  or `3`): `now=1` on the host and on `b5dj`, `now=2` on `b5dk`. Every copy `exact` (`exact=2` on
  each peer), every `wire:` drop and `noblock` count 0, no ErrorLog.
- **The replay at `0x49842F`, live: one start in two of the design below.** The peer that
  finishes loading *first* passes the barrier *last* — it waits for the others' status, while a
  slow loader already holds everyone's — so the driver duty-cycles the host and `b5dj`
  (SIGSTOP/SIGCONT, 10 %) through their load, lets `b5dk` load at full speed, and once
  `b5dk` waits at the barrier (bit 2 of `main+0x38D75` set, bit 3 clear) slows it to 3 % until
  both others have played 4 s. In the run that replayed, `b5dk` logged `replayed slot 1 from
  sender 1 (birth 0, type 34) at GameTime 0, before the first tick; the slot now holds type 34`,
  the same for slot 1501 from sender 2, and `before the first tick (GameTime 0), 2 held creates:
  2 replayed, 0 killed while held, 0 not (inactive 0, stale 0, bad 0 in total)`. That run stopped
  at the start, before a heartbeat, so the replayed copies' positions and `copy` counters were not
  read. The repeat, which read every commander on every peer 12 s in, did not replay: `b5dk` made
  both in catch-up ticks at GameTime 3, and every commander stood at its owner's position on all
  three peers (`(352,4384)`, `(8400,4384)`, `(4336,304)`), `exact=2` on each, every drop 0.
  **What does not provoke it:** slowing the late peer through its whole load (15 %, 15 s) — it
  then holds everyone's status and passes the barrier first, making both creates in catch-up
  ticks; and freezing it at the barrier — the others stay at bit 2 (`[5,0,5]` on both for 90 s),
  so the barrier needs a live message from every peer after the waiting peer's own load, not the
  status it sent before.
- **Kills while held** (`killed=`, `swept=`) were not provoked: nothing dies inside a peer's
  remaining load at a start, where only commanders exist.

*The departing host (for group E).* The host leaves through its own menus (`tools/mp_leave.sh`:
Tab → OPTIONS → EXIT → MAINMENU → "Surrender this battle and return to main menu?" → yes).
**The game ends on every joiner.** In the step-by-step start, each joiner dropped from 23 units
to its own commander (the host's 21 and the other joiner's commander left its roster) within one
150-tick heartbeat, and both left the level at the same GameTime, 11345, for `ENDMSN` with the
net state 7; in the scripted repeat both read state 7 and GameTime 2495 on `ENDMSN` 15 s after
the host's click, each with one unit. No fault, no ErrorLog, and every `wire:` drop and `ghost:`
count 0 on all three at their last in-play heartbeat. **A joiner leaving leaves the others
playing:** when `b5dk` surrendered, the host and `b5dj` played on (state 6, GameTime 1992 → 2354),
each with both remaining commanders; the host leaving that two-peer game then ended it on `b5dj`
(`ENDMSN`, state 7). Whether the end is the engine's rule or DirectPlay's session ending with
its host was not traced; that is group E's question.

**B6 — loaders and the rest. LANDED 2026-09-25** on local main (built on its own branch,
`worktree-tadr_port_b6`, from B1's tip at the time, before B2–B5 landed; reviewed at high by two
reviewers at `7601399`, then in two focused reviews of its fix rounds at `0105d3c` and `1249243`,
whose findings were acted on; merged with main after B5 landed at `c01f6c6`). What was built,
and where it differs from the plan below:

- **Built as planned:** the wind and the yardmaps as rows of the fail-closed table (nine and three
  rows: on its own branch, 173 sites in the raised build and 47 in the stock-limits build; merged
  with B1–B5, 224 and 99; all MEASURED at launch),
  and the three local fixes, each checked and skipped alone.
  The engine map's *The wind*, *A yardmap parsed past its string*, *The saved-game loader's order
  fallback*, *The stockpile bar's divide* and *A range circle of radius 1* have the disassembly.
- **The path is the engine's own.** A network game is the engine's test, `GameingState +0` = 3
  (the game start's dispatch, `0x4971C7`), not "some record looks like a host". Inside a network
  game nothing local to a peer is read: when DirectPlay names no session the seed is the map's hash
  alone, counted. TADR instead takes the host's ID with a clock fallback.
- **Changed: the network seed is DirectPlay's session instance GUID, not the host's DirectPlay
  ID.** The owner's decision named "host DPID + map hash". A focused review showed the host seat can
  change during the load: until the load ends the game thread pumps the network
  (`0x4984DD..0x49852E`), and the pump's leave case removes a departing human (`0x452CC0`: type
  cleared, ID −1) and, for the host, elects the human seat with the highest ID
  (`0x452EE3..0x452FF8`). A peer that handles the departure before its seed at `0x491903` would
  read the new host, one that handles it after the old, one mid-removal a set type beside an ID of
  −1, and the winds would differ for the rest of the game, silently — a timing-dependent seed.
  The session's instance GUID is fixed when DirectPlay creates the session, every peer holds it
  from the moment it joins (a joiner names it to `Open(DPOPEN_JOIN)`, `0x4CA03A`), and no departure
  or host election writes it. It is read with `GetSessionDesc` through the engine's
  interface (`main+0x4D9`, an `IDirectPlay3A`, or an `IDirectPlay2A` when lobbied) on the game
  thread, from a stub on the loader thread's start (`0x4982CA`): before that thread exists, so the
  call adds no DirectPlay concurrency stock does not already have (stock's loader pumps the
  network at `0x49727D` while the game thread pumps at `0x49852E`, and it calls `SetSessionDesc`
  at `0x497C0B`), and the thread's creation orders the capture before the seed. The engine map's
  *The session, not the host* has the disassembly.
- **Added: the engine's one `SetSessionDesc` passes the GUID DirectPlay holds.** It is the one
  engine call that could move the GUID: it hands DirectPlay the engine's whole session copy, and an
  implementation that takes `guidInstance` from it (Wine's copies the whole descriptor) moves the
  session to the copy's GUID, which after a lobbied launch is the lobby's descriptor
  (`0x4C9B4F`). The census found one site: of the six calls through a vtable's `+0x7C` in the
  image, `0x4C9903` in `HAPINET_updategameinfo 0x4C9890` is the only one through `main+0x4D9` and
  the only one with three arguments (the other five, `0x47C0CB`, `0x4B4FB8`, `0x4B5735`,
  `0x4B6069`, `0x4B60E1`, push two). `0x4C9890` is reached from `0x451180` — the battle room's
  handlers and every load's end on the loader thread (`0x497BFF`, `0x497C0B`) — and from the
  pump (`0x454135`). A `jmp` over the call's nine bytes (`0x4C98FD`) goes to a wrapper that calls
  `GetSessionDesc` first, validated as the capture validates it, and makes the call with a copy of
  the engine's descriptor carrying that GUID, counted when the engine's differed; with no valid
  answer it makes no call and returns the refusal, which the engine takes as a failed call,
  counted. So no `SetSessionDesc` the engine makes, in the battle room or the load, lobbied or
  not, can move the session, and the engine's copy is never written. This replaces the capture's
  write of DirectPlay's GUID into the copy, which covered only the calls after the capture and
  left a lobbied host's battle-room calls open; the capture stays the seed's source. `0x4C98FD`
  is also the site of TADR's `NullLpszPasswordInUpdateGameInfo` (not ported, the evidence's §3):
  a port of it now writes `lpszPassword` in the wrapper's copy.
- **DirectPlay's answer is validated, and the engine's copy is the fallback.** The answer is used
  only when its `dwSize` is 0x50 and its GUID is not null (Wine can answer `DP_OK` with a zeroed
  descriptor). Otherwise the seed takes the engine's copy `main+0x479` when it is not null (it held
  DirectPlay's answer on every peer measured), then the map alone, each counted. `GetSessionDesc`
  fails only with no interface or no open session, and a peer with no open session exchanges no
  game traffic, so its wind changes nothing another peer sees.
- **Stock's load call draws nothing.** It finds `next` = 0 (`0x4918FD`) and GameTime 0, so the seed
  at `0x491903` precedes every draw, and the first draw is the first tick's.
- **Changed: B6 logs a line of its own** (`patch_loader_defects`), not a clause of B1's
  `enginefix:` line, plus one line per level load naming the seed.
- **Changed: a stockpile order whose slot index is above 2 draws no bar.** The extra-weapons
  module's slots past the third are not inline slots, and the engine's bar has no reader for them.
- **Evidence.** The wind is reproduced on the previous build: two peers paused at GameTime 825
  read speed 2525, heading `0xF5C6`, and 1498, `0xC827`. Every peer read the same next change,
  speed and heading in five games on the earlier seeds: a first one, sampled twice (at 795 and at
  2003 or 2006); a second in the same processes; a third after restarting the joiner, where the
  host was on its third level load and the joiner on its first; and, on the host-seat rule, one
  with two peers and one with three (components and ratio equal too). On the session-GUID seed:
  two peers on Two Continents logged the same GUID and seed and, paused at GameTime 1262, read
  next 1350, speed 1627, heading `0xB693`; three peers on Town & Country logged the same GUID and
  seed, the host was killed 3 ms after its own seed while the joiners loaded, and both joiners went
  into play reading next 1530, speed 2834, heading `0x0616` at GameTime 1131 and 1137. With the
  validation, two peers on Two Continents logged the same GUID and seed and read next 420, speed
  433, heading `0x355C`; both answers passed and none was seeded from the copy or the map alone.
  With the `SetSessionDesc` wrapper, two peers on Two Continents logged the same GUID
  (`{181E3FD8-DEA0-42FE-AAB8-33E00D9AB262}`) and seed (`0xE5210DF9515CC70E`); the host made 6
  calls before its seed and 7 by play, the joiner 1 and 2, none over another GUID and none
  withheld. On every peer of the four games the engine's copy `main+0x479` held the GUID
  `GetSessionDesc` returned. A skirmish reads game mode 2 and
  takes the counter path. The yardmaps' 2440 retail cells are
  byte-identical to the previous build's. The two scratch structures read `2f2f31313100002b2b…`
  from the stack on the previous build and all `0x2F` on the new one, on two launches each. The
  three local fixes rest on the disassembly: none of their inputs is in stock content, and the
  `ShowRanges` cheat could not be typed under injected input.
- **Not measured:** the side measurement this landing was to carry (a tracked unit's death leaving
  an order armed, and our build ghost drawing) did not fit its time budget. The engine's removal
  of a departed host during the load was not exercised: DirectPlay never reported the killed host
  (its seat still held its type and ID 115 s of game time later; the engine's sessions carry no
  `DPSESSION_KEEPALIVE` when `createnewgame` makes them; a lobbied one takes the lobby's flags),
  and a host cannot quit through the UI during the load. The departure argument rests on the
  construction: the seed reads nothing a departure writes. **A lobbied launch was not run.** The
  construction covers it: the seed reads DirectPlay's own record, validated, and every
  `SetSessionDesc` the engine makes, the battle room's included, goes through the one site the
  wrapper replaces. The wrapper's replacement and withholding were measured only at 0: nothing
  differed and nothing was withheld.
- **Merged with B1–B5** (`2fc6528`: main `c01f6c6` merged at `38e2005`, then the last review's
  lows). `ddraw.dll` md5 `08e0c4530e71235b709a1af8f050580b` and `ddraw-stocklimits.dll`
  `c22ba59a2928d94e4682822a08bc305e`, both warning-free. The raised build reads `limits: installed
  224 sites` and the stock-limits build `the simulation fixes' 99 sites installed`, B5's 212 and 87
  plus the wind's and the yardmaps' twelve rows; in both, each of the twenty-eight fixes reads
  `ARMED` or in the fail-closed table (the weapon IDs, in the raised build, with the raised limits)
  and none is skipped, `tagpu_wirecheck.on` runs the wire's 33 cases, the stale hits' 23 and the
  ghost's 21 with 0 failed, and the stubs take 4 208 bytes in two pages (raised) and 3 856 in one
  (stock-limits). A skirmish on Town & Country seeds from the counter (game mode 2). Two peers on
  Town & Country (`tools/mp_lobby.sh`; `limits-tier2-p0` applied on the host, `-p1` on the
  joiner, and 512 of each army ordered onto the other): both seeded from the session
  `{751C2AC8-58DC-4889-912B-2C36A717020F}` and the map's `0x29FE9EA5`, seed `0xDAD824C4D2EA38BC`,
  the engine's copy agreeing; at the seed the host had made 6 `SetSessionDesc` calls and the
  joiner 1, and by the end 7 and 3, none over another GUID and none withheld. The fight ended the
  game at GameTime 2715 on the host and 2705 on the joiner: the joiner's commander died and
  `MultiCommanderDeath` took its army. So the peers were compared at the level end, where the sim
  had stopped on both, not at a pause. Both read the wind's next change 2880, speed 3123, heading
  `0xE01C`. Every `wire:` drop and `noblock` count read 0 on both (2 691 and 2 704 `0x2C` parsed).
  In `hits:`, `bad`, `bare` and `over` read 0 on both; the host sent 1 500 `0x09` and 930 `0x0B`
  and the joiner received 1 500 and 930, the joiner sent 1 500 and 367 and the host received
  1 500 and 367, and every copy was exact (1 500 on each). In `ghost:` every alert field read 0;
  the joiner, which loaded last, made the host's commander in its first catch-up tick
  (`created slot 1501 from sender 1 (birth 0, type 34) at GameTime 1, in a catch-up tick`,
  `now=1`) and held both commanders from its first roster; the host, in play before the joiner's
  create arrived, made it on arrival as stock does and held both from its second roster.

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

**B7 — four defects the veterancy survey found. LANDED 2026-09-26** on local main with C3
([C3, as built](data-keys.md#c3-as-built)), built on C's branch; reviewed at high with C3. The
survey of [data keys Part 3](data-keys-evidence.md) found them; the owner assigned them to C3's
session. The disassembly is the engine map's *Veterancy, a hit's word, the radar's owner test and
the meteor shower*; the hooks are [gpu-status §2.100](../gpu-status.html).

- **A hit's word** (sim, the fail-closed table, `0x489C71`): `0x489BB0` stores a hit's int amount
  into its record's WORD; the damage path subtracts it signed and the paralyser and the heal read it
  unsigned, so a hit past 32 767 wrapped into a gain. The store saturates into the reader's range
  (−32 768..32 767, or 0..65 535 for kinds 2 and `0xA`), the identity inside it. Retail play reaches
  it with the D-gun of a commander at level 2 or more (30 000 raised by its own level).
- **Kill-outright** (sim, the table, `0x489BF3`): a call of 30 000 or more skips the veterancy
  reduction as it skips the armour one, by the same test of the caller's amount. Stock let a
  veteran above 24 000 HP survive its own self-destruct, defeat or a dying transport; the only
  retail unit that high is CORKROG (29 918), whom a D-gun now kills whatever its level. A veteran
  so killed reads the full overkill in its death's severity (`0x48655E`), as a recruit does. This
  replaces the plan's "kill-outright uses stock's level": stock's level is the defect.
- **The radar's owner test** (local, `0x4673B1`): the radar rebuild's marker branch for a targetable
  or interceptor projectile out of sight read its attacker's owner, and a meteor has no attacker.
  No attacker is not the local player's.
- **One meteor, one hit** (sim, the table, the gate `0x49A01B` and the calls at `0x49DF7D` and
  `0x49D307`): every peer runs its own shower and broadcasts each stone, and stock computed every
  stone's damage on every peer, so a stone's hit on a unit was applied once by its owner and once
  more for each other peer. A stone is now computed on the peer that spawned it, as a shot is on the
  firer's: the spawn marks its stone's `+0x62` 0, the receiver's meteor branch 1 (the field is the
  firing piece the shooter's `Query*` script answers, written only for a projectile with a shooter
  and read only behind a burst count a stone does not have), and the gate sends a received stone where stock sends a remote
  projectile. The mark lives in the record, so the pool's compaction carries it.

**Measured** on a private Xvfb, each defect first on the build before B7 (C2's tip with the
scenario `kills` column), then on the new one:

- **The word** (`scenarios/b7-word-outright.json`, `WK_HUGE` deals 65 000 whole): the storage went
  1 329 → 7 225 over eleven hits, 536 up a hit, and died only when its HP word wrapped; on the new
  build it died at the first hit, one saturation logged. **The retail D-gun**
  (`scenarios/b7-dgun.json`, a commander with 25 kills, `blast unit N`): the storage read
  −11 135 = 1 329 + 2 · 26 536 − 65 536, two wrapped hits; on the new build −31 438 = 1 329 − 32 767 at
  the first.
- **Kill-outright**: the Krogoth with 25 kills survived its self-destruct at 5 918 HP; on the new
  build it died, and so did a keyed tower at 30 kills (level 25, which no hit under 30 000 can damage),
  both counted.
- **The radar** (`scenarios/b7-radar-hail.json`, `WK Hail T`'s targetable hail, True line of sight,
  unmapped): an access violation at `0x4673B4` reading `0xFF` with `ECX = 0`; the new build runs,
  the event counted. Owner 10's player record is populated and its colour pointer valid, so the
  stones in sight draw safely.
- **The meteors** (two peers, `scenarios/b7-mp-host.json` and `b7-mp-join.json` on `WK Hail C`,
  5 a stone against a storage; a temporary trace of every stone hit a peer computed, since
  removed): before, all 19 hits were computed on both peers and each owner's storages lost exactly
  twice what the stones dealt; after, none of 24 was computed on both, each peer computed only its
  own stones, and each storage lost exactly the damage computed against it (11, 19, 15, 10 on one
  peer; 3, 10, 0, 5 on the other).
- **Stock content** (`scenarios/200v200.json`): no B7 event through the fight.

**Not covered, and left as they are:**

- Every peer still runs its own shower, so a network game of N players rains N showers.
- Kill counts differ between peers in stock (the victim's owner counts a kill of its unit on its
  copy of the killer; the killer's own copy does not). Found by C3's two-peer run; a new question
  for B.
- A unit reclaim's step wraps in stock as well: `0x438650` multiplies the workertime, the factor
  `(kills + 5)/5`, the target's MaxHitPoints and 15 in 32 bits, so ARMCOM reclaiming a CORKROG from
  155 kills takes a sliver of its step. C3 bounds the keyed factor only; stock's is a new question
  for B.

**Measured alongside a landing** (time-boxed, each with its session):

- resurrection's failure branch: counters on `0x405155`/`0x405164` over a few hundred CORNECRO
  resurrections of 1×1 and multi-cell wrecks (B1's session);
- a tracked unit's death leaving an order armed (`0x2CC3`) and our build ghost drawing (B6);
- a departing host in a three-peer game (B5's session; the result goes to group E) — measured
  2026-09-26: it ends the game on every joiner (B5, *The departing host*);
- a cargo unit killed in a transport over land leaving its wreck in the air (B2's session; a
  floating wreck is raised as a visual residual);
- the repair rate per call, ARMCOM against ARMCK on an ARMLLT (B1's session).

**Kill counts across peers — measured for B8 (2026-09-26, local main `8d033d1`).** The destructor
counts a kill (`inc word [attacker+0xB8]`, `0x4869CA`) only when the attacker is non-NULL
(`0x48699D`), the victim's owner `+0xFF` differs from the killer's player `+0xF4` (`0x4869BA`),
and the victim's `+0x104` equals `0.0f` (`0x4869A7`, the constant at `0x4FD6F8`) — read from
**this peer's own copy** of the victim. Three peers in one game (the B5 runs above): the host's
towers (`scenarios/b4-guns.json`) against `b5dj`'s ARMCK, `b5dk` a bystander, every value read
by `tacli peek` of the slot on each peer:

| victims (`b5dj`'s, by the scenario applier) | victim's `+0x104` on host / `b5dj` / `b5dk` | kills added to the towers' copies on host / `b5dj` / `b5dk` |
|---|---|---|
| four ARMCK, dead within ~1 s of their create (`b4-victims.json` beside the towers), step-by-step start | 1.0 / 0.0 / 1.0 | 0 / 4 / 0 (one each on four towers) |
| the same, scripted repeat, 8 s after the apply | 1.0 / 0.0 / 1.0 | 0 / 4 / 0 |
| four ARMCK on hold at (2600, 6983) for 90 s, then two ARMLLT created beside them, step-by-step start | 0.0 / 0.0 / 0.0 | 4 / 4 / 4 (two each) |
| the same, 40 s old, scripted repeat | 0.0 / 0.0 / 0.0 | 3 / 3 / 3 (one victim still alive at the read) |

The 1.0 → 0.0 of the remote copies is a `tacli peek` series every ~3 s from the apply: it read
1.0 on both non-owners until 26–32 s after the create (step-by-step) and 29–37 s (scripted,
`b5dk` first), then 0.0, all four victims at once — the owner's round robin writing `+0x104` from
its full state (`0x48B3F0`, `[8] ÷ 255`). The towers' own remote copies read 1.0 on both joiners
right after their create and 0.0 later. Every commander's copy read 0.0 on every peer from the
start. **So a kill counts on every peer once the non-owners' copy of the victim reads 0.0, and
only on the victim's owner before that.** The victims here come from the scenario applier, which
creates a unit complete and then writes its `+0x104` locally; how long a factory-built unit's
copy reads nonzero on the other peers, and so how often the split happens in play, was not
measured.

## Open questions

- Whether each peer pair's subpackets arrive in the order sent (`0x451DF0`'s DirectPlay flags, the
  TAF tunnel). B4's design does not depend on it; a later acknowledgement scheme would.
- The `0x2C` dirty list stops at 0x200 bytes a tick, filled in slot order. If a skipped unit's
  dirty state is not kept, high slots of a busy block wait for the round robin, up to N ticks: this
  bears on [section A's open question](raised-limits.md#open-questions) about remote lag at 1500.
- A stock-fix claim in the Delphi recorder, inactive for stock content and not yet verified: a
  ground transport's overload (`0x406789`) (evidence Part 4 §7). Its other claim, a veteran's
  damage reduction scaling the kill damage (`0x489C2F`), is B7's kill-outright.
- Kill counts differ between peers: a kill of another peer's unit is counted on the victim's
  owner's copy of the killer, not on the killer's own (MEASURED by C3, 2026-09-26; the engine map's
  *The kill increment*). Veterancy, stock's and C3's, reads the computing peer's copy.
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
