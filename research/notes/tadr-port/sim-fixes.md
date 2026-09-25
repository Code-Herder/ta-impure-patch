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
  source.
- **Changed: the defect is the read, wherever it is inlined — 45 copies, not four.** Every bound of
  a row against a player's grid height is a compare with `+0x84`; `.text` holds 50, and 45 are the
  read, each the same world point against one player's LOS or mapped grid (a search by the shear's
  shape alone found 37: eight reuse a `y >> 1` computed far above). Beyond the eight above: the order resolver `0x43F0E0` (six) and the view player's map build
  `0x467440` (two, which mark units seen for the acquisition) join the table; the cursor picker
  `0x43E490` (six), the build cursor's site test `0x47D2E0` (one), the feature helper `0x4658E0`
  (four), the radar rebuild `0x466DC0`'s projectile dots (four), the particle leaves `0x473590`,
  `0x473A00`, `0x474170`, `0x4745E0`, `0x475470` (two each) and positional sound `0x47F300` (two)
  are local, `fix_los_local`, all or none; `0x474B80`'s two stay
  stock, since nothing calls it. The engine map's census has the table. The lone row tests take one
  generic stub built from the site's stock bytes; the order resolver's second read, whose point
  register stock overwrites first, is a hand stub over its block. Every stub counts its own rows per
  function, an interlocked `LONG` each. What each generic stub relies on is compared: the bytes from
  its z load through its row test (the radar's whole loop for its dots). 176 sites in the raised
  build, 50 in the stock-limits build (MEASURED from the log lines).
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
  report](raised-limits.md#the-failure-report)). 176 sites in the raised build (130 + 46), 50 in the
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

**B4 BUILT AHEAD (2026-09-25, `e10201f` on its own worktree from B3's `bd5582b`; reviewed at high by
two reviewers, fix round `c9f939b`; not landed).** The first build's numbers follow; the fix
round's are after them. As designed above; `fix_stale_hits` in `tagpu_patches.c`. The install line reads
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
