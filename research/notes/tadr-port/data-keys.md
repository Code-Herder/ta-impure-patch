# C. New data keys — the plan

## Summary

Section C brings the weapon and unit keys that TADR taught the engine to read into our stack, as
our own code, over four landings. The owner decided every choice below on 2026-09-25 **[DECIDED]**,
in a grill that followed [the evidence pass](data-keys-evidence.md). **C1, C2 and C3 have landed**;
C4 has not. The rules shared by every group are in
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
  `0x42E468` and emptied at the weapon load's entry `0x42E310`. Unit keys: one fixed-size record per type, indexed by
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
- **Every weapon flag also applies to slots 3..N.** `tagpu_weapons.c` runs those slots in C (its
  verdict `tagpu_weapons_check_slot`, `my_AutoAim`). The weapon keys own `0x49ABB0`'s entry in
  both builds, armed or not, and ask that verdict for a slot past 2.
- **`nomapweaponalert`: a silence, not a deletion.** For a hit with no attacker, a weapon kind, and
  the level's meteor weapon carrying the key with default damage 0: no "Under Attack"
  (its one site `0x4071D8`, and `my_Retaliate`), no alarm, no blink of the hit unit's minimap dot,
  and the loader sets the weapon's `noradar` bit so its projectile draws no dot. The hit is applied
  exactly as stock applies it. Display only.
- **Veterancy: TADR's levels, bounded per effect.** Levels are the count of thresholds at or below
  the kill count (u16, as stock). Damage taken: up to 25 levels, as TADR documents, applied where
  stock applies its own level, to every hit but the heal, so a unit at 25 takes no weapon damage, no
  paralysis and no unit reclaim's step (TADR's hook sits at the same site); **a call of 30 000 or more (kill outright: self-destruct,
  defeat, a dying transport; and the D-guns) takes no veterancy reduction**, as it takes no armour
  reduction, so it kills every unit, veteran or not (B7's fix of stock, which let a veteran above
  24 000 HP live). Damage dealt: every hit saturated into the HP word's range (B7). Reload: up to
  16 levels, the largest with a positive multiplier. Accuracy: `kills / rate`, 0 = off. The
  capture's cost (`0x4043D8`) and a unit reclaim's step (`0x438650`, which TADR misses), the level
  capped at stock's own maximum, 13 107, and the reclaim step's factor held to what its 32-bit
  product holds. The panel shows "VetN" on
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

1. **C1 — the ghost's piece mask. Landed 2026-09-25.** Display only, touches nothing of section B's.
   `Create()`'s hides by default, `PreviewPieces=` as the override; the unit-key reader it needs
   for that one key. How it is built is in [C1, as built](#c1-as-built) below.
2. **C2 — the weapon keys. Landed 2026-09-25.** The weapon-key store; `nottoair`, `nottounderwater`, `surfacefire`,
   `notoverwater`, `notoverland`, with the extra-weapons module's C paths; `nomapweaponalert`'s
   silence. Sim sites fail closed, the silence skips and logs. The fixtures include the two
   `surfacefire` shapes real content uses: a water beam D-gun fired by a commander on land and from
   the seabed, and a `vlaunch` missile from a submerged submarine that must steer above water.
   Reviewed at `high`. How it is built is in [C2, as built](#c2-as-built) below.
3. **C3 — the unit-key store and veterancy. Landed 2026-09-26**, with section B's B7. Every effect
   site and both panels; the saturation became B7's, for every hit. It measured what the evidence
   left open: a loaded saved game runs `0x42D2E0` and reads its keys again, and kill counts are
   **not** equal on two peers, in stock. Reviewed at `high`. How it is built is in
   [C3, as built](#c3-as-built) below; the fold site went to C4.
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
  template's block. The COB's length is the engine's second open of the file, which equals the
  block only while the file holds still (a loose script rewritten by another program during the
  load breaks the engine's own checksum first). A packet whose mask table is cut draws no ghost
  that frame (`maskcut=` in the heartbeat) rather than one showing its hidden pieces. The 102
  structures without a row were not each drawn; they take the exact
  path they took before the change (no row, no bit read), which ARMSOLAR, ARMVP and CORFUS show.
  The engine also hides a piece with fewer than three vertices (`0x45AF1B`); the ghost does not,
  and such a piece should cover no pixel, since no face on it has three distinct vertices
  [INFERRED, not measured].

## C2, as built

- **The store** (`tagpu_datakeys.c`, section 4): one byte a weapon, `s_wkey[TAGPU_LIM_WEAPONS]`,
  indexed by the ID `tagpu_limits_weapon_index` validates from a record's own address, so a pointer
  that is not a whole record reads 0, which is stock. Emptied at the weapon load's entry `0x42E310`
  (its one caller is `0x4918BB`, inside the level's load) and filled at A′3's ID site `0x42E468`
  once the loader has accepted the section, so a skipped section writes nothing and a later one
  with the same ID replaces it. A key reads as the engine reads every flag of `+0x111`,
  `GetInt(key, 0) & 1`. Written on the loader thread, read on the game thread in play.
- **The targeting keys: five sites in the fail-closed table** (`tagpu_patches.c`,
  `fix_weapon_keys`), both builds. Each runs the engine's own instructions for a weapon without a
  key.
  - `0x49ABB0`, the can-engage test's entry, now this module's alone: stock's body (or the
    extra-weapons module's C verdict for a slot past 2 while it is armed), then one filter by the
    slot's weapon. `surfacefire` turns a water weapon's refusal of a target that is not flying into
    stock's own range test; `nottoair` refuses a flying target (`+0x110 & 3 == 2`);
    `nottounderwater` a target whose top (`+0x70` + def `+0x170`) is at or below the sea. The
    filter answers every caller: acquisition, retaliation, the attack cursor (`0x43E59E`, which
    asks this test itself) and the order handlers.
  - `0x43F1D4`, the order action's unit branch, which decides the right-click's order: stock's
    decisions in stock's order, each key mirrored where stock tests its counterpart. The branch
    reads weapon 0, and slot 1 in the submerged test.
  - `0x49B9EB`: a `surfacefire` projectile above the sea steers instead of falling.
  - `0x49E1FD`, AutoAim's fire gate, after the per-tick target read: `notoverwater` /
    `notoverland` hold the slot by the ground under the firer (`0x485070`; off the map, not
    gated), and a `nottoair` slot holding a flying unit drops it through `ClearTarget 0x48A0F0`.
  - `0x42E310`: the store's clear.
  The extra-weapons module's C paths take the same verdict and gate for slots past 2
  ([extra weapons](../extra-weapons.html)).
- **`nomapweaponalert`** (section 5, skip-and-log, display only). A hit is harmless weather when
  its projectile has no attacker and its weapon carries the key with default damage 0. A hit
  computed here is judged by that weapon: a frame around the damage function's one send of a
  weapon hit (`0x499E37`) holds the answer, and the local apply (`0x489C89`) pins it to the record
  it applies. A hit received from a peer names no weapon, so it is harmless only when it did no
  damage: no attacker, a weapon kind, amount 0, and the level's meteor weapon `[0x512328]` keyed.
  A death explosion or a burning feature, the other attacker-less weapon hits, alerts unless it
  did no damage (received) or its own weapon carries the key with default damage 0 (local). The
  hit is applied exactly as stock applies it. Then:
  - no "Under Attack", at its one site `0x4071D8` and in the extra-weapons module's retaliation,
    through a frame around `0x406F80` (`0x489DA2`) that saves and restores its answer;
  - no blink of the hit unit's dot (`0x489D8E`, `0x466EB9`), for exactly the part of `+0xFA` that
    harmless hits alone put there, so a real hit keeps blinking for its own 0xF0 ticks. A saved
    game carries that part beside `+0xFA`, in a byte of the unit's record that stock writes as 0
    and never reads (`0x48797B`, `0x4872CC`);
  - no dot for the stones: the loader's closing call `0x49E010` gives such a meteor weapon
    `noradar`.
  Detail and threads: [gpu-status §2.97](../gpu-status.html).
- **Bad values**, logged at load: `surfacefire` without `waterweapon` (no effect, as documented),
  both `notover*` keys on one weapon (it fires only off the map), and `nomapweaponalert` on a
  weapon that is not a meteor with default damage 0 (its stones keep their dot).
- **Measured** on the raised build, the main scenario also on the stock-limits build with the same
  results, and again after main's B3 and B4 were merged in. Fixture: `tools/datakeys_fixture.py`,
  whose weapons and units are listed in its docstring. Each key is measured against its control
  without the key:
  - **`nottoair`** (`scenarios/c2-weapon-keys.json`, the AI's towers against the player's units on
    hold). The keyed tower killed the construction kbot beside it and never fired at the hovering
    transport, which kept all 150 HP; the control killed its transport. A landed transport held by
    a keyed tower was dropped the moment it took off, and its HP froze. On `WKLLT5`, the unkeyed
    slots 0 and 3 targeted a hovering transport and the keyed slot 4 never did.
  - **`notoverland` / `notoverwater`**: on land the `notoverland` tower held its target and never
    fired, and the slot dropped its target the tick the target died; the `notoverwater` tower
    fired. On the water (floating towers) the `notoverwater` one held and the `notoverland` one
    fired.
  - **`nottounderwater`**: the keyed torpedo launcher sank the ship and left the submerged sub at
    610 HP; the control killed both.
  - **`surfacefire`**, the torpedo launcher: it killed a hovercraft on the water, which the control
    never engaged. It also engages a kbot on the shore, but the torpedo stops in the shallows and
    the kbot keeps its 700 HP.
  - **`surfacefire`, Escalation's two shapes** (`scenarios/c2-surfacefire.json`): the water-beam
    D-gun (`blast`) killed a solar collector ashore from a commander on land and from one on the
    seabed, and the vertical-launch missile (`attack`) did so from a submerged sub, logging its
    steering; the three controls did not.
  - **The order action** (`scenarios/c2-order-cursor.json`, a right-click on each pair). The order
    node at unit `+0x5C` is the evidence. A `nottoair` tower's order onto a flying unit is refused
    (it keeps its standing order, type 22), where the control gets an attack order (type 8); the
    same for `nottounderwater` onto a submerged target seen by sonar. The hovercraft with a
    `surfacefire` torpedo gets an attack order onto a ground unit (type 6), which the control's
    refuses (idle, 41).
  - **`nomapweaponalert`** (`scenarios/c2-nomapalert.json`, the maps `WK Hail` and `WK Hail C`).
    Under keyed hail: no "Solar Collector: Under Attack" in 30 shots over a minute, while the log
    counted 64 alerts silenced. A hit unit's dot is present in all of a burst of 8 shots taken
    while its `+0xFA` is above 100; the minimap changes by 0 pixels, with no stones drawn. Every
    hit is applied: the kind 1 is stored at `+0xF5`, `+0xFA` is set, and the storages lose HP to
    the per-type damage.
    Under the control hail: the text repeats (up to four lines at once), the hit unit's dot is
    missing in 3 of 8 shots, and the stones are drawn as yellow points. Under keyed hail, a real
    attack (a Peewee on a solar collector) still alerts, and its dot is missing in 5 of 8 shots.
    **Two peers** (`WK Hail`, each peer's own solar grid): each peer silenced 64 alerts and showed
    no text, against "Under Attack" on both under the control. A temporary counter showed that
    each peer's harmless hits on its own units arrive both ways, local (`0x489C89`) and received
    as a `0x0B` (the dispatcher's case, `0x455412`). The counter was removed.
    **What stays loud** (the same map, a `CORFUS` of the AI's at 1 % health among the collectors,
    finished by a Peewee): its `ATOMIC_BLAST` and a collector's `SMALL_BUILDINGEX`, attacker-less
    like the stones, were judged loud (a temporary trace of every attacker-less record, since
    removed), and "Solar Collector: Under Attack" came up at once; the stones around them stayed
    silent. **Two peers, the rule for received hits**: a received stone with amount 0 was silent on
    both peers, and a received per-type hit on a storage (amounts 3 and 5) alerted, while the same
    hit computed locally stayed silent.
    **A save across the hail** (`WK Hail`, with a Peewee of the player's shooting one solar
    collector): the game was saved while three collectors held harmless hits (`+0xFA` 70, 220 and
    231, `E` 240 each) and the Peewee's target a loud one (`+0xFA` 239, `E` 0), and loaded back. A
    temporary trace, since removed, showed the saver on the game thread (the same thread as the
    blink) and the restore on the loader thread, giving each of those slots exactly the `+0xFA` and
    `E` it was saved with. A loaded game starts paused; once it ran, a burst of 8 shots over
    2.5 s found one dot missing in some frames, the loud unit's, and 0 pixels changed anywhere
    else while the harmless ones' `+0xFA` fell from 220 and 231 to 132 and 143.
  - **Stock content**: only the fixture's weapons log a key at load.
- **Not covered.**
  - The attack cursor's in-range answer for a keyed weapon was not seen: the AI flies its
    aircraft out of range. It follows from the cursor asking `0x49ABB0`. Out of range, both
    towers show "too far" (3).
  - What stock does with an attack order whose target `0x49ABB0` later refuses. For a keyed weapon
    it is moot, because the mirror refuses the order and the gate drops a flying target.
  - Escalation's own flight numbers for its vertical-launch sub missile
    (`weaponvelocity=-10`, `startvelocity=690`) never reach a target on the stock engine, steering
    or not; the fixture uses the Merl's flight.

## C3, as built

- **The keys** (`tagpu_datakeys.c`, section 6, read by the unit-key reader at `0x42BF97`).
  `VeterancyThresholds=`: whitespace-separated whole numbers, each 1..65 535, strictly
  increasing, at most 32. `VeterancyAccuracyBuffRate=`: one whole number 0..65 535, 0 meaning no
  buff. A malformed key is refused whole with its reason in the log ("a threshold is not a whole
  number from 1 to 65535", "the thresholds do not strictly increase", "it lists more than 32
  thresholds", "not a whole number from 0 to 65535") and the type plays as stock; a good one logs
  its level count and first threshold. A type's level is the count of its thresholds at or below
  its kills, a binary search.
- **The reader installs before the fail-closed table.** C3's sites read keys, so
  `fix_veterancy` calls `tagpu_datakeys_units_install()` first and the table refuses to install
  if the reader's sites did not byte-match. The extra-weapons module's observers on the same
  loader chain onto it.
- **Seven sites in the fail-closed table** (`tagpu_patches.c`, `fix_veterancy`), both builds. Each
  runs the engine's own instructions for a type without the key.
  - Damage taken (`0x489BFA`): L, at most 25 (TADR's documented −4 % a level). Stock's reduction
    there covers every hit but the heal (`0xA`) and, since B7, the kill-outright calls, so a unit at
    25 takes no weapon damage, no paralysis and no unit reclaim's step: it cannot be reclaimed. No
    known content reaches 25; every list of Escalation's has five thresholds.
  - Damage dealt (`0x499DB5`): L, at most 32 (`DK_VET_MAX`, the thresholds a list may carry); the HP
    word's saturation (B7) bounds the hit.
  - Reload (`0x49E468`): L, at most 16. `tagpu_weapons.c`'s own reload for slots past 2 takes the
    same level.
  - Target lead (`0x48A324`): on past the first threshold.
  - Spread (`0x49D6EA`): the divisor `kills / rate`, which the engine applies above 1; rate 0 is no
    buff. This is the code's formula, stock's `kills / 12` with the rate in place of 12; TADR's
    documentation writes `1 + kills / rate`, its code does not.
  - The capture's cost (`0x4043D8`), the target's level: `10 + L` tenths of the cost, set once as
    the capture starts.
  - A unit reclaim's step (`0x43869D`, in `0x438650`), the HP a reclaimer takes from the unit it
    reclaims every 15 ticks, by the reclaimer's level: `1 + L` in place of
    stock's `(kills + 5)/5`. Both this and the capture's cost take the level open past the last
    threshold, by the last gap (by the threshold, for a list of one), capped at 13 107, stock's own
    ceiling. The capture's product is at most 236 106 000 at the cap, below 2³¹ (its base is at most 1 800,
    `0x404396`). The reclaim step's does not: `0x4386B9..0x4386C3` multiply the workertime, the
    factor, the target's MaxHitPoints and the 15 ticks in 32 bits and `0x4386CC` reads the product
    unsigned, so ARMCOM's 300 on a CORKROG wraps at a factor of 32. The answer takes the other three
    factors (the stub passes `edx`, `[esi+0x1FA]` and `[esp+0x1C]`) and holds the factor to the
    largest that fits, 1 when even 1 does not (the engine's own product at a recruit's factor).
    MEASURED (`scenarios/c3-reclaim-bound.json`, VTCOM1 and VTCOM0 at 31 kills on a CORKROG each):
    on the build before the bound the keyed target lost 30 HP in 16 s against stock's 3 074, the
    wrapped product; with it, 11 304 per 13 s against 2 544, 4.44 ≈ 31/7 (each step floored by `0x4386DA`'s conversion), the event logged.
- **`0x438650` is a unit reclaim, not a capture.** Its callers are the reclaim order (`0x40483D`)
  and the build order's reclaim (`0x414C86`); the evidence pass had it as the capture's time. The
  decision stands for it unchanged: every place stock reads a level reads the keyed type's.
- **The kill lines** (skip-and-log, display only): at `0x46B306` and `0x467CCF` every unit at level 1
  or more says "VetL", its type's level or stock's; stock's own line below level 1.
- **The scenario's `kills`** (`tagpu_scenario.c`, `tools/tacli`): a unit attribute 0..65 535 written
  into `+0xB8` as the unit is dressed, on the peer that applies it.
- **Measured** on the new build, on a private Xvfb, against stock controls in the same game. The
  fixture is `tools/datakeys_fixture.py` (types `VTLLT0/1`, `VTRATE0`, `VTBAD1..8`, the unarmed
  `VTTGT0/1`, `VTCOM0/1`, `VTKROG0/1`; `VT_LAS` deals 100 against every type).
  - **The effects** (`scenarios/c3-veterancy.json`, HP read every 0.3 s, and a temporary trace of
    every answer, since removed): a keyed shooter at 10 kills took 160 a hit off a kill-less
    Krogoth, the control at stock's level 2 took 112; a keyed Krogoth at 10 kills lost 60 a hit,
    the control 92. The keyed tower landed 4.95 hits/s against the control's 2.29 (2.16; the
    formula's 40 % against 88 % of the laser's 15.6 ticks, 6 against 13, predicts 2.17). A keyed
    reclaimer's steps took 472 HP from its target, the control's 128 (3.69 against 11/3). A keyed
    capture target (cost × 20/10) was taken in 16.4 s, the control (× 12/10) in 9.9 s. Lead
    answered 1 for a keyed tower at 10 kills, 0 at none, stock's for the control; the spread's
    divisor 10, 0 and stock's; every answer for a type without the key was stock's.
  - **The kill lines** (`scenarios/c3-kill-lines.json`, the bottom bar hovered): "10 kills - Vet10"
    (keyed), "10 kills - Vet2" (no key, stock's level), "1 kill - Vet1" and stock's "1 kill". The
    engine draws the line only for a unit whose `+0x110` bit 31 is set, which armed units get once
    they have fired.
  - **Malformed keys**: all eight refused at load with their reasons, each type played as stock.
  - **A saved game**: saved mid-fight and loaded back, the load ran the unit-key reader again (every
    key re-read, the malformed ones refused again) and the towers kept their kills and levels
    ("10 kills - Vet10", "1 kill - Vet1").
  - **The stock-limits build**: the 116 simulation sites install and the amounts are the same.
  - **Stock content** (`scenarios/200v200.json`): every answer was stock's through the fight, and no
    veterancy or B7 event fired.
  - **Two peers** (`scenarios/b7-mp-host.json`, `b7-mp-join.json`): a keyed tower's hits on the other
    peer's Krogoth took 160 there (computed on the firer's peer, applied on the owner's).
- **Not covered.**
  - The second kill line (`0x467CB0`, no direct caller) was not seen drawn.
  - **Kill counts can differ between peers, in stock:** every peer counts a kill only when its own copy of the victim reads `+0x104` = 0.0 (`0x4869A7`), and another peer's copy of a newly created unit reads 1.0 until the owner's round robin writes it, 26–37 s after the create (measured for B8), so a unit killed in that window is counted only on its owner's copy of the killer. C3's two-peer run met it:
    the host's copy of its tower stayed at 10 through two kills of the joiner's scenario-created
    units while the joiner's copy counted 2 (the build before C3; 3 on C3's). Each effect reads the copy of the
    peer that computes it, as stock's own levels do, so a veteran's level in a network game depends
    on which peer applies the effect. A stock defect for section B, reported to the owner; the
    evidence pass's "kills equal on two peers" does not hold.
  - The fold site C4 will use (`0x42B019`) stays C4's.

## Handed to other groups

- **Group D:** building rotation with `Rotations=`, the per-facing `PreviewPiecesS/E/N/W=` and
  `PreviewFaceOpponent=` (about 25–30 sites, a wire field, two DLL tables; TADR's version fails by
  construction five ways: [evidence Part 4 §1](data-keys-evidence.md#1-rotations-and-the-building-rotation-it-switches-on));
  the reload bars with `reloadbar=` (the engine's stored reload recorded where it is made, drawn in
  our marker pass: [evidence Part 2 §2](data-keys-evidence.md#2-reloadbar)). Their questions are
  recorded there for D's plan.
- **Section B, as B7:** the four stock defects the veterancy survey found (a hit past the HP word,
  kill-outright sparing veterans, the radar's NULL read at `0x4673B1`, a meteor hitting once per
  peer) landed with C3; their record is B7 in [B's plan](sim-fixes.md).
  The kill counts that can differ between peers are B's question ([B8's measurement](sim-fixes.md)), not fixed.

## Open questions

None of C's own. Both that the grill left open were settled by the census of Escalation's content
([evidence Part 5](data-keys-evidence.md#part-5-what-escalations-content-uses)): `surfacefire` acts on
water weapons only, and `PreviewObject3D=` stays parked.

**Escalation itself does not run on our stack as shipped.** Its own exe reads renamed data folders
(`unitsE`, `weaponE` …), and its scripts use the recorder's COB extensions. C keeps the keys'
contract for any content written against them; running Escalation is a separate question for the
owner, outside C.
