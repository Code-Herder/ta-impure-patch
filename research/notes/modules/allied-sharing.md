# Allied sharing

Allies see what each other sees. Two players allied both ways share their line of sight, the
death vision of their units and their explored map, in skirmish and in network games, with AI
seats as with human ones: a unit acquires and fires at what only an ally sees, the fog and the
minimap fog open where an ally sees, and the cull draws what an ally sees. They share their radar
and sonar contacts: an ally's radar and sonar mark enemies on your radar picture, an ally's jammers
do not jam you, and an ally's units keep their dot on the minimap. An ally's cloaked or submerged
units stay in sight. The engine's one-byte sight counters never wrap: a cell under more than 255
stamps reads 255, not 0.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`; player record `p` = `main + 0x1B63 + p*0x14B`. Every site the module writes is a row of the [hook map](hook-map.html), *Allied sight*, and every field it writes a row of §2.5.

## The owner's decisions

1. Always between allies (TA's alliance rows `+0x108`), ignoring TA's ShareRadar and ShareLOS bits.
2. Share sight, radar and sonar contacts and allied minimap dots; the unit panel is deferred.
3. Units target what only an ally sees: a simulation change, so allied AIs target what you see.
4. Allied jammers stop jamming you.
5. An alliance counts only when it is held both ways (the sensor view's rule).
6. Share the explored map.
7. Share death vision. It reaches only the allies on the dying unit's owner's machine: the engine
   makes death vision for the viewed player only, on each machine (`0x482910` makes records for
   `main+0x2A43`), and no network message carries it.
8. Allied cloaked and submerged units stay visible.
9. The counters do not wrap: the engine's byte holds `min(W, 255)` of a wide count W kept beside it.
   Valid because every engine reader of the counters tests the byte against zero (*The readers*).
10. AI seats keep targeting by the viewed player's radar picture.

A one-way alliance shares nothing in either direction, including the radar a stock ShareRadar giver
handed out one way. In a network game the rows between two *other* players can be stale on your
machine (the `0x23` packet goes to its target alone, `0x452B05`, `0x451C24..0x451C58`), so the
picture you see when you `+view` a third player may differ from that player's own; your own seats'
picture is exact, since both rows involving a local seat are local.

## The files

| file | thread | what |
|---|---|---|
| `tagpu_allysight.c/.h` | game, loader | S and W, the hooks, the walk, the migration, the replay's pair; `sim` in `thread-split.allow` |
| `tagpu_replay.c` `tables_put` | game | calls `tagpu_allysight_restore_begin` / `_restored` around the table write |
| `tagpu_recorder.c` `dump_sight` | game | writes each kept record's W after its counters (`RK_SIGHTW`) |
| `tagpu_rangeown.c` | game | the sensor view draws the coverage of the owners `tagpu_allysight_contacts` names |
| `tools/replay/tarec.py` | — | reads `RK_SIGHTW`; `compare-dumps` compares W as it compares the counters, and checks `u8 = min(W, 255)` in each dump |

## S and W

**S** (`s_share`, `SIM_STATE`): bit `t` of `s_share[o]` says that `o`'s sight stamps also go into
record `t`'s counters; bit `o` says that `o`'s own counters are kept through W. It is computed from
the records: an *eligible* record (counters of the level's length, in use, a seat of type 1..3, its
map bit `+0x146` its own index, counters shaped as the map's plot halved) shares with each eligible
record whose row byte toward it and its own toward that record are both non-zero.

**W** (`s_wide`, `SIM_REBUILT`): record `t`'s cell `i` is `s_wide[t * s_cells + i]`. Every stamp and
removal of an owner S keeps adds or subtracts one in W for each target and writes `min(W, 255)`
into the target's byte. A cell of W holds at most (live stamp records) × (one walk's visits to a
cell), so it cannot wrap; a removal of a cell at 0 leaves 0 and is counted.

**The invariant (an ordering).** For every live stamp record and every record `t`, `t`'s counters
hold that record's walk exactly when bit `t` of its owner's S is set (masked by the records whose
counters exist). Stamps and removals read S and nothing else; S changes only at three points, each
followed under the lock by a migration of the counters to the new S before any further stamp:

1. the level's players init `0x464990` (on the loader thread): own bits only, W zero, no stamp yet;
2. the step's players phase, its call at `0x49555F`, on the owner's machine only;
3. the replay's table write, on a puppet host: S is recorded, so the recording is its writer there.

**The removal is the stamp's walk with the sign reversed.** `0x481D50` and `0x482270` walk
identically (DISASSEMBLED: their first 0x38 bytes match, the rest differs in `dec` for `inc`, the
order of two loads in the circle loop and the jump displacements), and the walk reads only the
record, the terrain sight grid `main+0x1428F` and the circle table `main+0x1485B`, never the
counters. So every walk — stamp, removal or migration — runs the stamp's own body through its
trampoline, and the three increment sites call into the module, which writes the stock byte for an
owner S does not keep and W and the targets' bytes otherwise.

**The migration** (`migrate`): for an owner whose S changed, each of its live units holding a stamp
(the block `[+0x67, +0x6B]` bounded to the unit array; `+0xA6 != 0`; under the ray fan a non-zero
height byte `+0xF8`, in the circle mode `+0xF8` below the table's count) is walked through the
engine's record builder `0x482090` into the receivers gained and out of those lost, and so is each of
its temporary sight records. On the owner's machine every receiver gained also takes the owner's
explored map through the engine's copier `0x485420`.

**The lock.** `s_lock`, a recursive leaf lock, guards S, W and the walk in progress. It exists
because the stamps run on the loader thread during a load, and a network load's leave can run its
kill-all on the game thread's load loop at the same time ([units](../engine/units.html), *The removal
runs on whichever thread pumps*). That race is the engine's own, on its counters and its unit array,
and it is not closed here: the lock keeps one walk at a time and W consistent through it.

## Contacts

The radar pass `0x467440` builds the viewed record's radar picture, `+0x110` bits 8–10 of every unit
(bit 8 a radar contact, bit 9 a sonar contact, bit 10 jammed), every 30 ticks in the players phase.
It runs for the viewed record only. Four sites make it share (`ally_radar`), each answering by S:

- **Loop 1's share test** (`0x4674BA`): the engine gives bits 8–9 to a unit whose owner's row toward
  the viewer is non-zero and whose owner's PlayerInfo has ShareRadar (`+0x97 & 0x40`); here, when S
  shares the owner with the viewer. ShareRadar is not read; the watcher test after it is the
  engine's.
- **Loop 2's emitters** (`0x46750E`, its head, and `0x4675D2`, its exit): the engine walks the radar
  and sonar units of the viewer's own block; here, the viewer's block and then the block of every
  owner S shares with it, each bounded to the unit array (`unit_block`). The visitor `0x467840` only
  sets bits and skips the viewed player's own units, so the blocks' order does not matter. The list
  is one pass's (`s_rp`), and two passes never overlap: the players phase runs it on the game thread
  in play and once on the loader before the game thread's first step, and the replay calls it on
  the game thread between steps.
- **Loop 3's jammers** (`0x4675F5`): the engine jams with every jammer whose owner is not the viewer,
  clearing bit 8 or 9 and setting bit 10 of every unit in range; here, an owner S shares with the
  viewer is skipped as the viewer's own is (decision 4). Loop 3 still runs after all of loop 2.
- **The minimap's dots** (`0x466E6F`, in `0x466DC0`): with bits 8 and 9 clear the engine dots only
  the viewed player's own units; here, an ally's too. Loop 1 gives every allied unit bits 8 and 9,
  so this site decides only for an allied unit an enemy jams on both kinds.

AI seats keep targeting by the viewed player's bit 8 (`0x40AB3F..0x40AB5C`, decision 10), which now
holds the allies' contacts. **The sensor view** (`tagpu_rangeown.c`, [range rings](range-rings.html))
draws the coverage of the viewed player and of every owner `tagpu_allysight_contacts` names — the
owners whose radar the pass walks for the viewer.

## Visibility

`UnitInPlayerLOS 0x465AC0(player, unit)` answers 1 at once for the player's own unit; for any other
it answers 0 for a cloaked unit (`+0x10E` bit 2) and for one wholly under water that is not a sonar
contact (`+0x110` bit 9), and otherwise tests four points of the unit's box against the player's
sight. Its owner test (`0x465ACE`, `ally_visible`) also passes an owner S shares with the player,
so an ally's units are visible as the player's own are (decision 8): cloaked, submerged and jammed
on sonar alike. Loop 1 of the radar pass already gives every allied unit bit 9, and a unit's own
stamp covers its cell in its owner's counters, which S shares, so the site decides chiefly for an
ally's unit that is cloaked, or submerged while an enemy jams its sonar.

**Targeting does not change.** The acquisition skips an owner its seat's row allies (`0x40AB05`)
before it calls `0x465AC0` (`0x40AB11`), and S requires that row, so it never reaches the new
branch. The other callers draw or describe: the order sprites `0x439761`, the unit panel `0x46AF3D`
and `0x46B5B4`, COB's `EMIT_SFX` test for the viewed player `0x480EE5`, HotUnits `0x48BC56`,
`0x494396`, and Impure's own (the frame packet, the order markers, the rings, the icons).

## The hooks

| site | what the module does there |
|---|---|
| the stamp `0x482270` | walks the record `+1` into the owner's S |
| the removal `0x481D50` | walks the record `−1` out of the owner's S; inside a migration, into the receivers gained and out of those lost |
| the cell sites `0x48239C`, `0x48247F`, `0x482580` | the stock byte, or W and each target's `min(W, 255)` |
| the explored map's stamp `0x481930` | the engine's stamp, then once more for each receiver, with a 16-byte copy of the record naming it: the body sets a bit only where none it tests is set, so one call per receiver |
| the rebuild `0x4816A0` | empties the temporary records (the refill wipes their stamps, which the engine would later subtract from counters that no longer hold them) and fills W of every record the refill will refill with the mode's base, then the engine's rebuild |
| the players init `0x464990` | sizes W to the level's counters and sets each eligible record's own bit; a level whose records' counters differ in length gets no W and stays stock |
| the players phase's call `0x49555F` | on the owner's machine, S from the records and the migration, before the phase |
| the radar pass's share test `0x4674BA` | bits 8–9 for a unit whose owner S shares with the viewer |
| the radar pass's emitter loop, head `0x46750E` and exit `0x4675D2` | walks the viewer's block, then each shared owner's |
| the radar pass's jammer test `0x4675F5` | skips the jammers of an owner S shares with the viewer |
| the minimap's dot test `0x466E6F` | dots an allied unit whose bits 8 and 9 are clear |
| `UnitInPlayerLOS`'s owner test `0x465ACE` | 1 for a unit whose owner S shares with the player |
| the destructor's call `0x486845` | the removal only when the unit holds a stamp: under the ray fan the destructor removes unconditionally, and a unit whose height byte is 0 holds none (the replay's `book_stamp` rule) |

## On a puppet host

`ally_sight` runs on a puppet: the host's stamps build W as the recording machine's did. S is
`SIM_STATE`: at every capture point, frame visit and seek the replay's table write puts the
recording's S, and `tagpu_allysight_restored` migrates the counters to it under the lock, which is
held from `_restore_begin` over the write. A seek stamps and removes under the host's S (R3–R8b)
before R12, and the migration then moves them to the recording's; R14a's temporary records are
made after it. `ally_share` (the players phase) refuses to act on a puppet. `ally_radar` runs: in the
recording's viewed record `contacts_put` overwrites bits 8–10 from the image after the pass, in any
other view the pass's bits stand, and no simulation reads them on a puppet host
(`tagpu_replay.c`, *THE RECORDED VIEW'S CONTACTS*). `ally_visible` runs: its answer follows S, which the
recording writes.

W is `SIM_REBUILT`: never recorded, rebuilt on every host by the stamps, carried in a dump after
each record's counters (`RK_SIGHTW`, only for the records S keeps), where `tarec compare-dumps`
compares it between the two dumps and checks `u8 = min(W, 255)` in each.

## The readers

Every engine reader of the counters (`player+0x7C`) is a zero test, DISASSEMBLED, each a
`cmp BYTE PTR [..],0x0`: `0x407FAA`, `0x418AFD`, `0x465958`, `0x465A2D`, `0x465BA3`, `0x465C3B`,
`0x465CDF`, `0x465D76`, `0x466D25`, `0x467273`, `0x467354`, `0x4677A0`, `0x473620`, `0x473AAA`,
`0x474200`, `0x47468A`, `0x474C12` (dead code), `0x475534`, `0x47D42C`, `0x47F445`, `0x4844D7`,
`0x49BF1C`. Our copies of engine code read the same way (the minimap projectile stub's
`cmp byte [ecx+ebp],0`, twice, `tagpu_patches.c`); our C readers are `tagpu_fogwide.c` (`== 0`)
and the sight dump. The writers: the stamp's increments (`0x4823A0`, `0x48247F`, `0x482587`), the
removal's decrements (`0x481E80`, `0x481F5F`, `0x48206B`), the rebuild's refill
(`0x481741..0x481767`), the players init's zero fill (`0x46488F`) and the replay's `sight_fill`
(record 10, which S never names). The player saver `0x4662F0` does not touch `+0x7C/+0x80/+0x88`.

## Gaps

- The cloaked case has not been run: the scenario format cannot cloak a unit, and no `tacli` order
  reaches another seat's cloak.
- A one-way alliance cannot be made from the skirmish keys (ally groups are symmetric), so the
  both-ways rule's one-way case has not been run.
- No scenario has an allied unit an enemy jams on both radar and sonar, so the minimap's dot site
  is installed but its effect is not measured.
- A mid-game alliance change has no scripted path (the ALLIES panel, `0x447235` / `0x447F8E`); the
  migration it runs is the one every allied game runs at its first players phase, with live units.
- The network load's leave racing the loader is the engine's race, not closed here.
- A remote human ally's machine never makes your death vision (decision 7).
- The install is all-or-nothing but not fail-closed: a machine whose exe does not match runs stock,
  and in a network game its sight then differs from its peers'.
- The wiki's scenes keep their single-seat emulation.
