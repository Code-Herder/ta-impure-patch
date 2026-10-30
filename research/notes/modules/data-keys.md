# Weapon keys, veterancy and transported explosions

The TADR section C data keys.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.97 The weapon keys and the weather's silence (`tagpu_datakeys.c`, `tagpu_patches.c`, `tagpu_weapons.c`) — TADR section C, landing C2, 2026-09-25

**What it is.** Six weapon keys that TADR's content uses and the stock loader ignores, read into
one byte a weapon and consulted where the engine decides. The plan and the measurements are
[data keys, C2 as built](../tadr-port/data-keys.html#c2-as-built); the disassembly is in
[the evidence](../tadr-port/data-keys-evidence.html) Parts 1 and 2 and in the
[engine map](../exe-reverse-engineering.html), *The weapon keys' sites*.

**The store.** `s_wkey[TAGPU_LIM_WEAPONS]`, indexed by the weapon's ID as
`tagpu_limits_weapon_index` validates it from a record's address, so a pointer that is not a whole
record of this build's array reads 0. Written on the LOADER thread: emptied at the weapon load's
entry `0x42E310`, filled per section at A′3's ID site `0x42E468` after the loader accepts the ID
and name, so a skipped section writes nothing and a later one with the same ID replaces it. Read on
the GAME thread in play. No retail weapon carries a key.

**The targeting keys: five sites in the fail-closed table.** A site whose weapon has no key runs
the engine's own instructions.

| site | what it decides |
|---|---|
| `0x49ABB0`, entry, 7 bytes | The can-engage verdict, for every one of its 11 callers: acquisition, retaliation, the attack cursor (`0x43E59E`), the order handlers. `wk_check` calls stock's body (or, for a slot past 2, the extra-weapons module's C verdict) and filters the answer by the slot's weapon. `surfacefire` first: a water weapon's refusal of a target that is not flying becomes stock's own range test. Then `nottoair` refuses a flying target and `nottounderwater` a target whose top is at or below the sea. One function, so no order between the keys can loop. This module is the entry's only owner. |
| `0x43F1D4`, 6 bytes | The order action `0x43F0E0`'s unit branch, the right-click's order. Stock's decisions in stock's order, each key mirrored where stock tests its counterpart: `nottoair` beside `toairweapon`, `nottounderwater` in the submerged test (weapon 0 and slot 1), `surfacefire` at the hover's water refusal. Refuse `0x4401DC`, no action `0x43F26C`, or on `0x43F27A` with `edx` the target's def. |
| `0x49B9EB`, 7 bytes | The self-propelled flight's water test: a `surfacefire` projectile above the sea steers (`0x49BA16`) instead of falling. |
| `0x49E1FD`, 6 bytes | AutoAim's fire gate, after the slot's per-tick target read `0x49E1E1`. `notoverwater`/`notoverland` hold the slot for the tick by the ground under the firer (off the map, not gated), so a dead target is still dropped every tick. A `nottoair` slot holding a flying unit drops it through `ClearTarget 0x48A0F0`. |
| `0x42E310`, 6 bytes | The store emptied before the load assigns it. |

The extra-weapons module's slots past 2 take the same verdict (through `0x49ABB0`) and the same
gate (its C loop calls `tagpu_datakeys_fire_gate`). It no longer hooks `0x49ABB0` itself (19
hooks), and its retaliation asks the engine's entry.

**`nomapweaponalert`: display only, skip-and-log.** A hit is harmless weather when its projectile
has no attacker and its weapon carries the key with default damage 0. The hit record names no
weapon, and a death explosion or a burning feature also makes attacker-less weapon hits, so the
answer comes from where the weapon is known. A hit computed here is judged by its projectile's own
weapon. A record received from a peer (`0x0B`, the dispatcher's case) is harmless only when it lost
nothing: no attacker (`+3` = 0), a weapon kind (`+8` 1 or 2), amount 0 (`+5`), and the level's
meteor weapon `[0x512328]` keyed with default damage 0. Every hit is applied exactly as stock
applies it, and:

| site | what it does |
|---|---|
| `0x499E37`, the call of `0x489BB0` | The damage function `0x499CD0`'s one send of a weapon hit (kinds 1 and 2), through a stub that adds the projectile (`[esp+0x30]` at the call): `weather_send` arms the projectile's weapon's answer (no attacker, the key, default damage 0) for the call and clears it on return. |
| `0x489C89`, the call of `0x489CE0` | `0x489BB0`'s local apply of the record it built on its own stack: `local_apply` takes the armed answer and pins it to that record's address for the apply's extent, saving and restoring the outer one. A record at another address is a received one. The send's frame lands first; without the apply, every record is judged as received. |
| `0x489DA2`, the call of `0x406F80` | Through an 8-byte stub that adds the record (`pop eax; push edi; push eax; jmp`), `hit_frame` holds the record's answer in `s_hitQuiet` for the whole of `0x406F80`'s extent. It saves and restores the previous answer, so a nested hit hands the outer one back. |
| `0x4071D8`, the call of `0x47F850` | The one "Under Attack" (notification 2): `tagpu_datakeys_alert` returns without calling it inside a harmless hit. The extra-weapons module's retaliation calls the same gate. The frame lands before this site, and the gate silences only once both have. |
| `0x489D8E`, the call of `0x467950` | `hit_mark` records, per unit slot, `E`: the part of `+0xFA` owed to harmless hits alone. A loud hit sets `E = 0`, a harmless one `E = 0xF0 − (fa − E)`. |
| `0x466EB9`, 6 bytes | The radar rebuild's blink test: with `0 < +0xFA ≤ E` the dot is drawn as if not hit. A real hit followed by weather blinks exactly until its own 0xF0 ticks run out. `+0xFA`'s every writer is accounted for: the hit, the unit tick, the create and the saved game's restore. |
| `0x48797B`, 8 bytes | The unit saver `0x4876C0`: the record's WORD `+0xB2` is stored with `E` in its high byte, where stock stores the zero-extended byte `+0x10E`, so `E` rides in `+0xB3`, a byte every save stock writes holds 0 and nothing reads. |
| `0x4872CC`, 6 bytes | The restore `0x487080`, where it writes `+0xFA` from the record's `+0xB1`: the unit's slot gets back the `E` saved beside it, so a loaded game blinks exactly as the saved one did. A save stock wrote carries 0, which is stock. |
| `0x49E010`, entry observer | The loader's closing call: a keyed `meteor=1` weapon with default damage 0 gets `noradar` (bit 6), so its projectiles draw no minimap dot. |

**Threads.** The store and `noradar` are written on the LOADER thread inside the level's load.
Everything else runs on the GAME thread: the decisions, the hit applier (the sim tick, and the
dispatcher's `0x0B` case in play), the radar rebuild, the unit saver. `E` is emptied with the store
at `0x42E310`, and a saved game's restore then writes it on the same LOADER thread, later in the same
load, so program order puts the restored `E` after the clear. The render thread reads none of it.

**Measured** (the full list is in the plan's *C2, as built*). Every key does what its
documentation says against its control, on the raised and the stock-limits builds. The order
action's three mirrors change the right-click's order and nothing else. The two `surfacefire`
shapes of real content (a water-beam D-gun from land and from the seabed, a vertical-launch missile
from a submerged sub) kill a target ashore that their controls refuse. Harmless hail is silent, its
hit units' dots hold still and its stones draw no dot, with every hit applied (a per-type damage
still hurts). The same hail without the key alerts, blinks and draws, and a real attack under keyed
hail still alerts and blinks. On two peers, harmless hits that arrive as `0x0B` from the other peer
are silenced when they did no damage; a received per-type hit alerts, while the same hit computed
locally stays silent. A fusion's death explosion and a collector's among keyed hail, attacker-less
like the stones, alert. A game saved with harmless and loud hits in flight loads with each
unit's `E` as saved, and only the loud unit's dot blinks.

**Not covered.** The attack cursor's in-range answer for a keyed weapon was not seen: the AI flies
its aircraft away. It follows from the cursor calling `0x49ABB0` itself.

### 2.100 Veterancy and four stock defects (`tagpu_datakeys.c`, `tagpu_patches.c`, `tagpu_weapons.c`, `tagpu_scenario.c`) — TADR section C, landing C3, and section B's B7, 2026-09-26

**What it is.** Two unit keys TADR's content uses, `VeterancyThresholds=` and
`VeterancyAccuracyBuffRate=`, read into the unit-key records and consulted at every place the engine
reads a unit's veterancy level; and four stock defects the veterancy survey found (B7). The plan and
the measurements are [data keys, C3 as built](../tadr-port/data-keys.html#c3-as-built) and
[simulation fixes, B7](../tadr-port/sim-fixes.html); the disassembly is in the
[engine map](../exe-reverse-engineering.html), *Veterancy, a hit's word, the radar's owner test and the
meteor shower*.

**The keys.** Read by the unit-key reader (`0x42BF97`, inside the FBI loader `0x42BF40`) into the
type's record, emptied at the unit-data load `0x42D2E0`, on the LOADER thread at the level's load
and at a saved game's load, and on the GAME thread at the console's one-type `Reload`.
`VeterancyThresholds=` is up to 32 whole numbers from 1 to 65 535, strictly increasing;
`VeterancyAccuracyBuffRate=` one whole number from 0 to 65 535 (0: no accuracy buff). A malformed
key is refused whole, logged with its reason, and the type plays with stock's levels. A type's
level is the count of its thresholds at or below its kills (a binary search).

**The reader installs before the fail-closed table.** `fix_veterancy` calls
`tagpu_datakeys_units_install()` first; if the reader's sites did not byte-match, the table
refuses to install and the process ends through the limits report, since the veterancy sites would
otherwise read no keys. `tagpu_datakeys_init()` asks again later, and gets the first answer.

**Veterancy: seven sites in the fail-closed table.** Each stub asks a C answer (`pushad; push
unit; call`); `-1` runs stock's own instructions (the type has no key), any other value is the
keyed type's, already bounded, and resumes at the instruction that consumes stock's value.

| site | the effect, its answer and its bound |
|---|---|
| `0x489BFA`, 7 bytes | damage taken, the victim's: L for `(25 − L)·4 %`, L ≤ 25. Stock's reduction there covers every hit but the heal and (B7) the kill-outright calls, so a unit at 25 takes no weapon damage, no paralysis and no unit reclaim's step |
| `0x499DB5`, 7 | damage dealt, the shooter's: L for `(100 + 6L) %`, L ≤ 32 (`DK_VET_MAX`); the HP word's saturation bounds the hit |
| `0x49E468`, 7 | a slot's reload: L for `(100 − 6L) %`, L ≤ 16, the largest level with a positive reload. The extra-weapons module's own reload for slots past 2 takes the same level (`tagpu_datakeys_vet_reload_level`) |
| `0x48A324`, 14 | target lead: on past the first threshold (1 → `0x48A332`, 0 → `0x48A42D`) |
| `0x49D6EA`, 5 | the fire method's spread: the divisor `kills / rate`, applied when above 1 as stock's `kills / 12` is; a type with the rate but no thresholds keeps stock's levels elsewhere |
| `0x4043D8`, 7 | the capture's cost, the target's: `10 + L` tenths of it, the level open past the last threshold (by the last gap) and capped at 13 107 |
| `0x43869D`, 7 | a unit reclaim's step (the HP taken from the target every 15 ticks), the reclaimer's: `1 + L`, the same open level and cap, then held to what the step's 32-bit product holds: the stub also passes the workertime `edx`, the target's MaxHitPoints `[esi+0x1FA]` and the ticks `[esp+0x1C]`, the product's other factors, and the answer is the largest factor that keeps it at or below 2³² − 1 (1 when none does, the engine's own product at a recruit's factor). MEASURED with `scenarios/c3-reclaim-bound.json`: 4.44 × stock's rate at 31 kills (31/7, each step floored), where the build before the bound wrapped to 1 % of it |

**The kill lines: display only, skip-and-log.** At `0x46B306` (the unit panel, the unit in `esi`)
and `0x467CCF` (the second panel, `edi`), every unit at level 1 or more says `"VetL"` where stock
says `"Veteran"` past 4 kills: its own type's level, or stock's for a type without the key. Below
level 1 the line is stock's; the singular "kill" stays. Each site writes into its own buffer, used
by the `sprintf` that follows it.

**B7: four stock defects, three in the fail-closed table and one local.**

| site | what it fixes |
|---|---|
| `0x489C71`, the store of a hit's amount into the record's WORD | The amount saturates into the range its reader reads: −32 768..32 767 for the HP subtraction, 0..65 535 for the paralyser and the heal. Stock wraps a hit past 32 767 into a gain; retail play reaches it with a veteran commander's D-gun (30 000 raised by the commander's level). A saturation is counted and logged at powers of two. Simulation. |
| `0x489BF3`, the veterancy reduction's start | A call whose amount is 30 000 or more (kill outright: self-destruct, player defeat, a dying transport's cargo; and the D-guns) skips the reduction, as the armour reduction already skips it (`0x489BD1`). Stock let a veteran above 24 000 HP survive its own self-destruct. Counted when the victim had a level. Simulation. |
| `0x49A01B`, the damage gate, with the calls at `0x49DF7D` and `0x49D307` | A meteor is computed on the peer that spawned it. Every peer runs its own shower and broadcasts each stone; stock computed every stone on every peer, so a stone's hit was applied once by the victim's owner and once more for each other peer's `0x0B`. The spawn marks its stone's `+0x62` 0, the receiver's meteor branch 1; the gate sends a projectile with no attacker, owner 10 and the mark 1 where stock sends a remote one's (`0x49A0AE`): its explosion plays and its damage is its spawner's. The mark lives in the record, so the pool's compaction carries it. Counted. Simulation. |
| `0x4673B1`, 11 bytes, local | The radar rebuild's owner test for a targetable projectile out of sight read the attacker's owner through a NULL attacker. No attacker is not the local player's, the answer every other player's projectile gets. Counted. |

**B10: a unit reclaim's step in 64 bits (two rows of the fail-closed table).** C3's survey found that
`0x438650` multiplies the reclaimer's workertime, its `(kills + 5)/5`, the target's MaxHitPoints and the
15 ticks in 32 bits, so a veteran's step wraps: an ARMCOM at 155 kills (factor 32) reclaiming a CORKROG
took 1 HP a step where the formula gives 486. `0x4386B9` (23 bytes) takes the product in 64 bits,
saturated at `INT64_MAX`; `0x4386D8` (7 bytes) clamps the quotient to 2147483647.0 before `_ftol`. Both
keep the engine's x87 operations in its order, so every product below 2³² gives stock's step bit for bit.
C3's keyed site `0x43869D` is not touched: its keyed path jumps to `0x4386B9`, so a keyed factor gets the
same product. The plan's B10 entry has the numbers ([simulation fixes, B10](../tadr-port/sim-fixes.html)).
C3's factor hold is now narrower than it needs to be; releasing it is left open for the owner.

**Threads.** The keys are written on the LOADER thread inside the level's load (and a saved game's),
before play; everything else runs on the GAME thread: the sim tick's damage, reload, aim and orders,
the radar rebuild, the panels' draw. The render thread reads none of it.

**The scenario's kills.** A scenario unit takes a `kills` attribute (0..65 535), written into
`+0xB8` by the applier as it dresses the unit; the wire line gains an eleventh column.

**Measured** (the full list is in the plan's *C3, as built* and in B7's entry):

- Veterancy against its stock controls: dealt 160 against 112 a hit, taken 60 against 92, the keyed
  reload 4.95 hits/s against 2.29, a keyed capture 16.4 s against 9.9 (its cost × 20/10 against
  × 12/10), a keyed reclaim 472 HP a step against 128; lead and spread answered 1/0/stock and 10/0/stock
  as their types; the kill lines "10 kills - Vet10", "10 kills - Vet2", "1 kill - Vet1" and stock's
  "1 kill"; the eight malformed keys refused with their reasons; a saved game's load reads the keys
  again and its units keep their kills and levels. The stock-limits build gave the same amounts.
- B7 against the build before it: the 65 000-damage laser raised a storage 536 HP a hit where the
  new build kills it at the first; a 25-kill commander's D-gun wrapped (HP −11 135 after two hits)
  where the new build kills at the first (−31 438); a 25-kill Krogoth survived its self-destruct at
  5 918 HP where the new build kills it and a keyed 30-kill tower; targetable hail faulted at
  `0x4673B4` where the new build runs; on two peers every stone was computed on both and each
  storage lost twice its hits, where on the new build none was and each lost exactly its hits.
- Retail content: every veterancy answer is stock's and no B7 event fires through a 200-unit fight.

**Not covered.**

- The second kill line (`0x467CB0`, no direct caller) was not seen drawn in play.
- A unit's kills are not always the same on two peers in stock: every peer counts a kill only when its own copy of the victim reads `+0x104` = 0.0 (`0x4869A7`), and another peer's copy of a newly created unit reads 1.0 until the owner's round robin writes it, 26–37 s after the create (measured for B8), so a unit killed in that window is counted only on its owner's copy of the killer. Veterancy reads the copy of the peer that computes the
  effect, as stock's own levels do. Not fixed here; a section-B question.
- Every peer still runs its own meteor shower, so a network game rains N showers where a
  single-player game rains one. B7 makes each stone hit once; how many showers fall is unchanged.

### 2.101 Transported explosions (`tagpu_datakeys.c`, `tagpu_patches.c`) — TADR section C, landing C4, 2026-09-26

**What it is.** Two unit keys TADR's content uses, `TransportedExplodeAs=` and
`TransportedSelfDestructAs=`: a unit that dies carried explodes with the named weapon in place of
its `ExplodeAs` or `SelfDestructAs`. The plan and the measurements are
[data keys, C4 as built](../tadr-port/data-keys.html#c4-as-built); the disassembly is in the
[engine map](../exe-reverse-engineering.html), *A unit's death, its transport and its explosion*.

**The keys.** Read by the unit-key reader (`0x42BF97`) into the type's record, beside C3's, and
resolved there to a weapon ID through the loader's own resolver `0x49E5B0`, on the LOADER thread
at the level's load and a saved game's, on the GAME thread at the console's `Reload`. A name that
resolves to no loaded weapon is logged (`<TYPE>: TransportedExplodeAs=<name> names no loaded
weapon; the type keeps stock's`) and the type keeps stock's weapon; a good one logs its ID. The
reader installs before the fail-closed table, as for C3.

**The decision: once a death, where the death starts.** A keyed unit's death is **carried** when
its transporter link `+0x86` is set, when its death's kind is 6 (the kind only the destructor's
cargo loop passes), or when the cargo loop marked it: a keyed passenger found pending death just
after the loop's hit, whatever that hit's kind (a self-destructing transport's is 3, and a
passenger already dying refuses it). The mark is the passenger's B4 birth stamp + 1, so it holds
only for that incarnation. The answer lives in one byte per slot for exactly one destructor call.

| site | what it does |
|---|---|
| `0x48664B`, 6 bytes | in `Send_UnitDeath`, the record complete and before its send and its destructor call: decides, consumes the mark |
| `0x486679`, 5 | the destructor call, through `tx_wrap`: reads the record's index before the call and clears its byte after. B4's `0x4C` branch (`doDeath`) jumps to the same wrap, so every destructor call this build makes goes through it |
| `0x49B017`, 16 | the pick: a carried keyed unit takes its key's weapon for the same `selfd` stock tests (`def+0x224` when set, else `+0x220`); any other unit, stock's |
| `0x486810`, 6 | the cargo loop, after each passenger's hit: the mark, and inside kill-all the kill mark |
| `0x4867CB`, 5 | a carried unit's own detach: inside kill-all, for a unit whose player is not local, applied here without the wrapper's `0x0A` broadcast |
| `0x48681D`, 5 | the cargo loop's detach of each passenger, the same way, through the same stub |
| `0x486F10`, 10 | kill-all, entered as `tx_kill_all`: the outermost call on a thread draws the token its kill marks carry |
| `0x42B019`, 6 | the menu-time fold, below |

Nine compared spans hold the bytes the stubs rely on (the pick's frame `0x49B000`, its
continuation, the record's index and kind stores, the destructor's arguments, the two detaches'
pushes, kill-all's return, the fold's context load). 17 rows in all.

**Kill-all.** Its branch for a player that is not local — a departed one, through the player
removal `0x452CC0` — detonates each unit at once and then destroys it, in slot order, so a
transport earlier in the block detaches its passengers before their turn. Each outermost kill-all
draws a token; the cargo loop marks with it every keyed passenger it detaches inside, and the pick
honours a mark only inside the kill-all that wrote it, so each detonates carried. A passenger
earlier in the block than its transport is still carried at its turn (`+0x86`), and its own
destructor detaches it. Both detaches are applied locally only: the wrapper broadcasts each, and
with three peers or more the first to remove the player would detach its units on the others
before their own kill-all, in either slot order. A departed player's blasts damage nothing (the
owner gate `0x49A043`), so what this decides is the explosion drawn, and each peer decides it from
its own copy.

**The wire.** The owner's decision travels as `m[19]` of B8's `0x4C`. A receiver arms it for the
destructor call the `0x4C` makes: its own `+0x86` cannot say it, since the transport's `0x4C`, sent
first, has already detached the passenger there. A `0x4C` refused while the receiver loads reaches
no destructor then; when B5 marks the copy dying instead, the decision waits as a mark on that
copy's stamp, for the unit tick's destroy. B3's count of received `0x0C`s looks through the wrap to
the return it carries.

**The fold.** At `0x42B019`, the store of `CRC_weapons`, each key whose name is a weapon section of
the loaded weapon TDFs folds that section's stored CRC, XORed with its own constant (`0x7F4A7C15`,
`0x94D049BB`) and rotated by its own count (11, 23), so it cannot cancel stock's `ExplodeAs` term.
Two peers whose TDF for a named weapon differs lose the keyed types from the game, as for any other
edit. A key naming no section folds nothing.

**Threads.** The keys are written on the LOADER thread inside the load; the fold runs where the
menu-time loader runs, before the seal; everything else runs on the thread that runs the death:
the GAME thread in play (every death, the cargo loop, kill-all, the pick and the `0x4C`'s
receipt), and during a network load either thread that pumps. The retail pump `0x453D40` holds no lock
across its dispatch; the recorder's owned primary wrapper serializes it when armed. Both the loader (`0x49727D`) and the game thread's load loop (`0x49852E`) reach its leave case
and the removal's kill-all. So the running kill-all is the thread's own: its token, from an
interlocked counter, lives in a TLS slot, and a kill mark matches only the kill-all that wrote it;
two threads' kill-alls never honour each other's marks, and a mark left behind matches no later
one until the 32-bit counter has turned, so nothing is emptied. Every index is a u16 or bounded by B4's table, and each per-slot array
holds 65 536 entries.

**Measured** (the full list is in the plan's *C4, as built*):

- **A transport shot down** (`scenarios/c4-transport-down.json`): the keyed passenger's two ring
  units took exactly 111 each (`TX_BLAST_E`) where the build before took stock's `COMMANDER_BLAST`
  and lost the tower; the unkeyed control took stock's.
- **A transport self-destructed** (`c4-transport-selfd.json`): 222 each (`TX_BLAST_S`); a type
  with only `TransportedExplodeAs` got stock's `SelfDestructAs`.
- **A passenger that survives its transport** (`c4-survivor.json`, 32 000 HP against the loop's
  30 000): not marked, and killed later on the ground it took stock's blast.
- **Kill-all's local branch** (`c4-kill-all.json`, single player, `+kill 1` for the AI): the
  passenger, marked in the cargo loop, exploded `TX_BLAST_S`, 222 to each ring Krogoth whenever
  the AI had not flown its ATLAS off before the kill.
- **Two peers** (`c4-mp-host.json`, `c4-mp-join.json`): the joiner's ATLAS downed by the host's
  tower; the host drew `TX_BLAST_E` from `m[19]` alone, and its received-death and received-`0x0C`
  counts agree.
- **Three peers, one frozen** (`c4-mp-removal.json`, one pair in each slot order): the host's
  TIMEOUT.GUI `REJECT` removed the frozen player there, and the session's drop removed it on the
  third peer too. Each peer ran its own kill-all, applied both detaches locally (the cargo loop's,
  and at `0x4867CB` a passenger's own, its slot below its ATLAS's), and detonated both passengers
  as `TX_BLAST_S`, one by its kill mark and one by `+0x86`; the third peer's received `0x0A` count
  stayed at 2. With both transports first in their blocks and the host's `+kill` in place of the
  `REJECT` (no session drop), the third peer's copies stayed carried until its own `REJECT`.
- **The fold**: `CRC_weapons` of a keyed type differs with the named weapon's TDF (`0xB71E6070` at
  111, `0xCF066848` at 112) and is 0 for the unkeyed control; the build before read 0 for all. On
  two peers with the joiner's TDF at 112, exactly the three keyed types left the game on both
  (322 of 325).
- 282 sites install, B8 and B9 included.

**Not covered.**

- A carried unit killed by a direct hit through its transport's deck (`ARMTSHIP`) was not run; it
  is the `+0x86` case, which the network hits did run.
- A carried `0x4C` refused while the receiver loads (the mark on the swept copy) was not run: it
  needs a death inside B5's catch-up window.
- A departure DirectPlay reports (a lobbied session's keepalive) was not run; TIMEOUT.GUI's REJECT
  is the same removal.
- A removal during a network load, where the loader thread runs the kill-all, was not run; the
  token makes that kill-all's state its thread's own by construction.
