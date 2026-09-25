# C. New data keys — the plan

## Summary

Section C brings the weapon and unit keys that TADR taught the engine to read into our stack, as
our own code, over four landings. The owner decided every choice below on 2026-09-25 **[DECIDED]**,
in a grill that followed [the evidence pass](data-keys-evidence.md). **C1 is built**; C2, C3 and C4
are not. The rules shared by every group are in
[the port overview](overview.md#standing-rules-decided-2026-09-23).

The keys are not a blank slate. TA: Escalation, the largest live mod, ships TADR's DLL and uses
`nottoair` on 73 weapons, `surfacefire` on 11, and both veterancy keys on 197 unit files
([evidence Part 5](data-keys-evidence.md#part-5-what-escalations-content-uses)). So each key is a
contract with mod authors that already exists, and C keeps it.

Four findings shaped the plan:

- **The lobby's sync already covers the keys' content.** A unit's sync value is a checksum of its
  whole FBI file, and a weapon section's is a checksum of its raw text, unknown keys included. Two
  players whose files differ only in a C key lose that type from the game, exactly as for any other
  edit. Only a key that names *another* file's content needs a fold: `TransportedExplodeAs=` and
  `TransportedSelfDestructAs=`.
- **Every key is decided on one peer.** Targeting and firing run on the unit's owner, a hit's
  damage on the firer's peer, a death explosion on the dying unit's owner. A peer without a key
  plays different rules for its own units without desyncing anyone: the case rule 3 exists for.
- **TADR's documentation and its code disagree for three keys**, and in each case the code does
  more than the documentation says. `nottoair` blocks acquisition only. `surfacefire` also engages
  aircraft. `nomapweaponalert` deletes the whole hit where it promises to silence an alert.
- **Several of TADR's designs fail by construction.** Its `surfacefire` is built from jumps between
  the engine's own branch targets and froze the game once. Its terrain gate skips the per-tick
  target read and reopens the stale-target class. Its veterancy cap makes a unit immortal, even to
  self-destruct. Its transported explosions hold a passenger list that expires by timing. And
  **our own build ghost shows pieces a building's `Create()` script hides, on 21 of the 126 stock
  structures.**

## Decisions

### The contract

**TADR's key names and meanings.** Same names, same defaults, same meaning, so content written for
TADR plays the same here. **Where TADR's documentation and its code disagree, the documentation is
the meaning, unless real content relies on the code**; that case follows the code, and the
divergence is recorded. A deviation is made only where TADR's meaning is itself unsafe or
ill-defined, and each one is written down as a finding.

### How C keys are held

- **Installed at attach, always, in both builds** (`ddraw.dll` and `ddraw-stocklimits.dll`). TADR
  installs a key's hooks the first time a mod uses it; rule 2 writes every patch once, at attach.
- **A key that changes the simulation fails closed; a display key skips and logs**, by section B's
  test: fail closed when a peer without it would play different rules for its units or compute
  different shared state; a difference only in a picture, a message or a crash is local. No runtime
  opt-out (rule 4).
- **Stock content runs stock's bytes, by construction.** Each store carries a "has this key" bit per
  weapon or per type. A hook whose bit is clear runs the engine's own instructions, so rule 7 holds
  by construction, not by equal arithmetic. No retail weapon or unit carries any C key.
- **Two stores, both bounded by construction.** Weapon keys: one byte a weapon, indexed by the
  weapon's validated ID (`tagpu_limits_weapon_index`, A′3's bound), written at A′3's loader site
  `0x42E468` and cleared at the weapon wipe. Unit keys: one fixed-size record per type, indexed by
  def and valid only when its stored def pointer matches (the `def_rec` rule `tagpu_weapons` uses),
  emptied at the start of every unit-data load (`0x42D2E0`), reset at the FBI loader's entry
  (`0x42BF40`) for every def it visits, and filled at `0x42BF97` — so it holds only what this
  load wrote, keyed or not ([C1, as built](#c1-as-built)). Both are written on the
  loader thread inside the level's load (the unit records also on the game thread, at a console
  `Reload`) and read on the game thread after it: the ordering the engine's own weapon and def
  arrays rest on. The render thread reads only the frame packet.

### Sync

**A mismatch in a key is caught exactly as stock catches any edit: the type silently leaves the
game on both peers.** C adds only the folds it needs, for the two keys that name another weapon,
into `CRC_weapons` at `0x42B019`, with a mix distinct from stock's terms. A file without the key
folds nothing, so stock content syncs exactly as today. A visible mismatch report is TADR's CRC
report, a group E feature.

### Bad values

**A malformed or out-of-range value is treated as absent and logged**: the type or weapon plays
stock for that key, and the log names the file, the key and the reason, once per type. A list is
accepted whole or not at all: no partial lists, no token skipping, no clamping. The sync fold uses
what the game will play.

### Scope

C plans the keys that are the whole feature. **Keys that only switch on a group D feature go with
that feature:** `reloadbar=` with the reload bars, `Rotations=` with building rotation. The
`Preview*` keys stay in C because our build ghost already exists. The evidence for the D items is
kept in the evidence pass.

### The items

- **`nottoair`: the weapon never targets or fires at a flying unit.** A verdict filter on the
  can-engage test `0x49ABB0` (the engine's own airborne test, `+0x110 & 3 == 2`), the order action
  refusing it where stock refuses a `toairweapon` (`0x43F1D4`), and at the fire gate a held target
  that has taken off is dropped as `0x48A1E0` drops a dead one. Our table, not TADR's bit 31.
- **`nottounderwater`: the same filter against a target whose top is at or below sea**, and the order
  action's submerged branch (`0x43F21B`). On a non-water weapon it is the identity: stock already
  refuses those targets.
- **`surfacefire`: a water weapon may engage surface units, never aircraft.** Inside the same
  filter, a water-path rejection becomes stock's own range test, so no ordering between flags can
  loop; the order action's hover refusal (`0x43F24F`); guidance (`0x49B9EB`). On a weapon without
  `waterweapon` it does nothing and is logged at load, as documented, and TADR's can-aim hook is not
  ported: every one of Escalation's 11 `surfacefire` weapons is a water weapon
  ([evidence Part 5](data-keys-evidence.md#part-5-what-escalations-content-uses)). One of them,
  `DGUN_DECOY_CORE`, lacks the `nottoair` its three siblings carry, and so can no longer be ordered
  onto an aircraft; recorded, not excepted.
- **`notoverwater` / `notoverland`: the gate sits at `0x49E1FD`, after the per-tick target read.**
  The weapon keeps reloading and spends nothing, and a dead target is still dropped every tick, so
  B4's two-tick hold keeps its meaning. Off the map the weapon is not gated, as TADR.
- **Every weapon flag also applies to slots 3..N.** `tagpu_weapons.c` runs those slots in C
  (`my_CheckUnitWeapon`, `my_AutoAim`), and exactly one module owns `0x49ABB0`'s entry, decided at
  attach.
- **`nomapweaponalert`: a silence, not a deletion.** For a hit with no attacker, a weapon kind, and
  the level's meteor weapon carrying the key with default damage 0: no "Under Attack"
  (its one site `0x4071D8`, and `my_Retaliate`), no alarm, no blink of the hit unit's minimap dot,
  and the loader sets the weapon's `noradar` bit so its projectile draws no dot. The hit is applied
  exactly as stock applies it. Display only.
- **Veterancy: TADR's levels, bounded per effect.** Levels are the count of thresholds at or below
  the kill count (u16, as stock). Damage taken: up to 25 levels for weapon hits, as TADR documents,
  so a unit at 25 takes no weapon damage; **a kill-outright call (30 000 or more) uses stock's level,
  so self-destruct, defeat and a dying transport kill every unit stock kills**. Damage dealt:
  saturated at 32 767 for keyed types. Reload: up to 16 levels, the largest with a positive
  multiplier. Accuracy: `kills / rate`, 0 = off. Capture: both formulas (`0x4043D8` and `0x438650`,
  which TADR misses), the level capped at stock's own maximum, 13 107. The panel shows "VetN" on
  every unit, as TADR. Our C copy of the reload formula in `tagpu_weapons.c` takes the same level.
- **Transported explosions: carried at death, decided inside the death itself.** A per-slot byte
  set and consumed within one death: carried when the transporter link `+0x86` is set before the
  detach (`0x4867BA`), or when the death's kind is 6, which only the cargo loop passes; a passenger
  already dying when its transport dies, or of a self-destructing transport, is marked in the cargo
  loop on its owner's peer, **and the owner sends "died carried" in a companion message beside the
  passenger's `0x0C`**, so every screen draws the same blast. Names resolved once at load; the fold
  above.
- **The ghost: the hides of `Create()` by default, `PreviewPieces=` as the override.** At level load,
  on the game thread, `Create()`'s prologue is read, not run, with `tools/ta3do`'s conservative rule
  (it stops at the first opcode whose length is not certain; every operand bounded by the COB's own
  counts). The result is a per-type mask in the ghost's own walk order, computed by the same walk
  `ghost_pieces` uses, carried to the render thread through the packet, and applied only when its
  piece count matches. No file change is needed for stock content.
- **`PreviewObject3D=`: parked**; no known content uses it, Escalation included.
  **`PreviewFaceOpponent=`: waits for rotation and for the recorder's COB extensions.** Escalation
  uses it on 12 long-range defences whose scripts find the enemy through the recorder's extended
  `GET` values, which stock COB and our stack do not have; and TADR's version reads fogged enemy
  positions.

## The landings

Each landing merges main before its review, lands its documentation before its review, and is
reviewed in a dedicated read-only context. Sites are DIS in [the evidence](data-keys-evidence.md);
each landing re-reads them before writing.

**The evidence bar.** A key is shown working on a generated fixture (`tools/weaponids_fixture.py`
is the pattern: a `.ufo` of clones built in a scratch folder, never committed) on the new build.
Stock content is shown unchanged: every gate counter stays 0 through a skirmish on retail content,
against the previous build where a number can be compared. A two-peer test (`a2net0`/`a2net1` on
`:71`) runs where a decision or a message crosses peers. Measurement rounds are scoped to what each
commit can change.

1. **C1 — the ghost's piece mask. Built 2026-09-25.** Display only, touches nothing of section B's.
   `Create()`'s hides by default, `PreviewPieces=` as the override; the unit-key reader it needs
   for that one key. How it is built is in [C1, as built](#c1-as-built) below.
2. **C2 — the weapon keys.** The weapon-key store; `nottoair`, `nottounderwater`, `surfacefire`,
   `notoverwater`, `notoverland`, with the extra-weapons module's C paths; `nomapweaponalert`'s
   silence. Sim sites fail closed, the silence skips and logs. The fixtures include the two
   `surfacefire` shapes real content uses: a water beam D-gun fired by a commander on land and from
   the seabed, and a `vlaunch` missile from a submerged submarine that must steer above water.
   Reviewed at `high`.
3. **C3 — the unit-key store and veterancy.** Every effect site and the panels, the fold site C4
   will use, the saturation for keyed types. It measures what the evidence left open: kill counts
   equal on two peers after a paused fight, and whether a loaded saved game runs `0x42D2E0` with
   its keys. Reviewed at `high`.
4. **C4 — transported explosions**, after B4 has landed its companion messages. The per-slot mark,
   the pick at `0x49B017`, the fold, the "died carried" companion, and the two-peer test that the
   blast's damage and its picture agree on both. Reviewed at `high`.

## C1, as built

- **The unit-key reader** (`tagpu_datakeys.c`, a `publisher` on `thread-split.allow`): an observer
  at `0x42BF97` inside the FBI loader `0x42BF40`, where `ebp` is the def and `ecx` the FBI's
  `[UNITINFO]` section, byte-matched at attach, skip-and-log. It runs on the loader thread at the
  level's load and on the game thread at the console's one-type `Reload` (`0x42D1F0`). One record
  per def slot, holding only what this load's loader wrote: an observer at the unit-data load's
  start (`0x42D2E0`) empties them all, one at the loader's entry resets the slot's record with a
  fresh serial before the open that can fail, and the read site fills only a record its own call
  reset. So a slot the load skips, or whose FBI does not open, has no row rather than an earlier
  game's at the same def address (the def array can come back at the same address), and a
  `Reload` moves the serial whether or not its FBI opens. A reader takes a row only when its stored
  def pointer is the def it asks about. C1 reads one key, `PreviewPieces=`, through the engine's
  own reader `0x4C48C0` with TADR's 1024-byte buffer.
- **The mask**, on the game thread inside the packet fill, once per type per level: `Create()`'s
  hides read and not run, with the engine's own piece match (`0x45A950`'s pre-order and swap,
  [the engine map](../exe-reverse-engineering.html)), or every piece `PreviewPieces=` does not
  name. Every read of the COB is bounded by the block's own length, which a fourth observer
  records: the checksum `0x4B6BA0(buf, size)` that `0x4B2450` calls over exactly the block it
  read, kept only for the call that returns into the loader (`0x4B247C`). A script with no
  recorded length gets no mask. The cache is keyed by the packet's level generation, the record's
  serial (a `Reload` can bring the COB back at the same address) and the root and COB pointers.
  The cursor's row is published whenever a build is on the cursor, from the packet's own
  `build_unit_id`, so the cursor ghost never draws from a packet without it; the queued sites'
  types ride the builds table's gate. It reaches the render
  thread as `TAGPU_PK_GHOSTMASK`, bits in the order of `tagpu_model_walk`, the one walk both
  threads call; the ghost applies a row only to the template root and piece count it was written
  for. Detail: [gpu-status §2.23](../gpu-status.html).
- **Bad values, as decided:** a value that fills the reader's buffer, a name longer than 63
  characters (TADR compares node names cut at 63, so it could never match) and an empty list are
  ignored at load and logged; a list that names no piece of the model is ignored when the mask is
  computed, logged, and `Create()`'s hides apply.
- **Measured** (`scenarios/ghost-mask.json`, `tools/datakeys_fixture.py`, the cursor ghost at
  zoom 2 against the same tree without the change): ARMLLT, ARMAP, ARMHLT, CORINT and CORMOHO
  change only inside their hidden pieces (34, 177, 360, 395 and 1 121 px); CORDOOM, whose four
  hidden pieces have no faces, and ARMSOLAR, ARMVP and CORFUS, which have no row, change by 0.
  The masks the game computed match an offline reproduction of the rule over all 126 stock
  structures: 24 get a row, 21 of them with a hidden piece that has faces, and the engine's
  match equals a by-name one on every stock structure. The fixture's `PreviewPieces=base` draws
  the base alone, `" Base ,<tab>TURRET"` the base and the turret, a name no piece has falls back
  to `Create()`'s flare, and a 64-character name is refused at load. Queued ghosts take the mask
  as the cursor's does, and `+reload` of a keyed type re-reads the key and recomputes the mask.
  `nobake`, `trunc` and `maskmiss` stayed 0 throughout.
- **Not covered:** the model template has no recorded length (`0x4CB560` passes none out of
  `0x4BBE50`), so the node names the mask compares are read with a 256-character cap, the type
  bound and the level lifetime, the terms every reader of the tree has, and not against the
  template's block. The 102 structures without a row were not each drawn; they take the exact
  path they took before the change (no row, no bit read), which ARMSOLAR, ARMVP and CORFUS show.
  The engine also hides a piece with fewer than three vertices (`0x45AF1B`); the ghost does not,
  and such a piece should cover no pixel, since no face on it has three distinct vertices
  [INFERRED, not measured].

## Handed to other groups

- **Group D:** building rotation with `Rotations=`, the per-facing `PreviewPiecesS/E/N/W=` and
  `PreviewFaceOpponent=` (about 25–30 sites, a wire field, two DLL tables; TADR's version fails by
  construction five ways: [evidence Part 4 §1](data-keys-evidence.md#1-rotations-and-the-building-rotation-it-switches-on));
  the reload bars with `reloadbar=` (the engine's stored reload recorded where it is made, drawn in
  our marker pass: [evidence Part 2 §2](data-keys-evidence.md#2-reloadbar)). Their questions are
  recorded there for D's plan.
- **Section B, as B7, measured first:** a hit above 32 767 wrapping the HP word; kill-outright
  sparing a veteran above 24 000 HP (a vet-5 `CORKROG` survives its own self-destruct, INF); the
  NULL read at `0x4673B1` for an attacker-less targetable projectile out of sight; whether a meteor
  hits once per peer in a network game. [B's plan](sim-fixes.md) does not list them yet; this is
  their record until it does.

## Open questions

None of C's own. Both that the grill left open were settled by the census of Escalation's content
([evidence Part 5](data-keys-evidence.md#part-5-what-escalations-content-uses)): `surfacefire` acts on
water weapons only, and `PreviewObject3D=` stays parked.

**Escalation itself does not run on our stack as shipped.** Its own exe reads renamed data folders
(`unitsE`, `weaponE` …), and its scripts use the recorder's COB extensions. C keeps the keys'
contract for any content written against them; running Escalation is a separate question for the
owner, outside C.
