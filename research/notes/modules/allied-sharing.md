# Allied sharing

Allies see what each other sees. Two players allied both ways share their line of sight, the
death vision of their units and their explored map, in skirmish and in network games, with AI
seats as with human ones: a unit acquires and fires at what only an ally sees, the fog and the
minimap fog open where an ally sees, and the cull draws what an ally sees. They share their radar
and sonar contacts: an ally's radar and sonar mark enemies on your radar picture, an ally's jammers
do not jam you, and an ally's units keep their dot on the minimap. Every seat, human or AI, targets
with its own contacts and its allies', never the viewed player's. An ally's cloaked or submerged
units stay in sight, and the unit panel shows an ally's unit as your own. The engine's one-byte sight counters never wrap: a cell under more than 255
stamps reads 255, not 0.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`; player record `p` = `main + 0x1B63 + p*0x14B`. Every site the module writes is a row of the [hook map](hook-map.html), *Allied sight*, and every field it writes a row of §2.5.

## The owner's decisions

1. Always between allies (TA's alliance rows `+0x108`), ignoring TA's ShareRadar and ShareLOS bits.
2. Share sight, radar and sonar contacts and allied minimap dots. The unit panel shows an ally's
   unit as your own; a remote ally's four resource numbers stay hidden, since your machine never
   computes them (a local AI ally's show).
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
10. Every seat targets with its own contacts and its allies', computed when it acquires; the
    viewed player is no exception. Sonar is per seat too. Only the seat's own targeting facility
    lets it fire on a radar contact (stock's rule, unchanged). Under Permanent or Circular LOS the
    sight test still reads the viewed player's explored map (*Gaps*).

A one-way alliance shares nothing in either direction, including the radar a stock ShareRadar giver
handed out one way. In a network game the rows between two *other* players can be stale on your
machine (the `0x23` packet goes to its target alone, `0x452B05`, `0x451C24..0x451C58`), so the
picture you see when you `+view` a third player may differ from that player's own; your own seats'
picture is exact, since both rows involving a local seat are local.

## In a network game

Alliances come from the battle room: two humans by each other's Ally buttons, an AI only by the
team icons (`TEAMICONS<n>`: the room's ALLY gadget is inactive on an AI's row, [allies'
resources](share.html), *AI allies*). Every peer stamps the counters of every seat of type 1..3
from the units it replicates, and computes S at its own players phase from its own copy of the
rows. Both rows that involve a local seat are always on the local machine: a seat's own row is
written where it is local, and the setter `0x452960` sends the other side's to the peer that holds
it (`0x23`, `0x452B05`). So each peer's S is exact for its own seats, a human's and the AIs it hosts.
An AI's units acquire on the peer that hosts it, from its counters there, which carry what its
allies' units see, a remote human's included; that human's view, on the other peer, carries what
the AI's units see. MEASURED 2026-11-15 (`tools/mp_allysight.py`, Two Continents; H hosts a human
and AI2, J a human and AI3; H and J allied by the Ally buttons, AI2 and J by one team, H and AI2
not allied): S read `003 007 006 008` on H and `007 003 005 008` on J (each peer numbers itself 0);
H's own counter at J's commander 3, J's at AI2's commander 1, AI2's on H at J's commander 3, H's at
AI2's commander 0 (no ally sees it, and S is not transitive); on J an AI3 unit inside AI2's radar
tower's range and outside its sight carried bit 8. With no alliance every one of those read 0 and
the unit carried no bit 8.

An alliance changed in the middle of a game follows at the next players phase of each peer that
holds a record of the pair. In the same game J opened the in-game ALLIES dialog (Tab, `ALLIES`,
`LIVEALLY<H's record on J>`, OK; engine/ui.md) and broke its alliance with H: H's counter at J's
commander fell to 0 while J's at AI2's commander stayed 1, and after J allied H again it read 3.

## The files

| file | thread | what |
|---|---|---|
| `tagpu_allysight.c/.h` | game, loader | S and W, the hooks, the walk, the migration, the replay's pair; `sim` in `thread-split.allow` |
| `tagpu_replay.c` `tables_put` | game | calls `tagpu_allysight_restore_begin` / `_restored` around the table write |
| `tagpu_recorder.c` `dump_sight` | game | writes each kept record's W after its counters (`RK_SIGHTW`) |
| `tagpu_seatcontact.c/.h` | game | each seat's contacts at its acquisition; the scan for a skirmish AI; `sim` |
| `tagpu_allypanel.c/.h` | game | the unit panel's owner tests; `presentation` |
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

And under the ray fan a record is removed only while it holds a stamp, a non-zero byte (the fan
emitter's own test, `0x482634`): the removal's two engine calls that test nothing, the destructor's
(`0x482104`, through `0x482090` from `0x486845`) and the temporary records' expiry's (`0x482161`),
are gated on that byte, as the migration's walks are.

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
engine's record builder `0x482090` into the receivers gained and out of those lost, and so is each
of its temporary sight records that holds a stamp (under the ray fan a non-zero byte `+0xB`). Every
record whose own bit is gained adopts its bytes into W first, before any walk, since a walk of
another owner into a record writes `min(W, 255)` into its bytes. On the owner's machine every
receiver gained also takes the owner's explored map through the engine's copier `0x485420`.

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

These bits are the display's. No seat targets by them: *Per-seat contacts*, below. **The sensor view** (`tagpu_rangeown.c`, [range rings](range-rings.html))
draws the coverage of the viewed player and of every owner `tagpu_allysight_contacts` names — the
owners whose radar the pass walks for the viewer.

## Per-seat contacts

The acquisition `0x40AA40` builds one seat's target lists every 30 ticks: the units in sight
(`UnitInPlayerLOS`) and the radar contacts (`+0x110` bit 8). Bits 8 and 9 are written by the radar
pass, which runs for the viewed player alone, and by the create, for the viewed player too, so stock gives every seat the viewed player's radar
picture: an AI targets what the human's radar sees and its own radar towers give it nothing
([units](../engine/units.html), *The acquisition*). `seat_contacts` (`tagpu_seatcontact.c`) gives
each acquisition its own seat's contacts instead:

- **At the acquisition's start** (its two calls `0x40AD3E` and `0x40B2E9`, redirected), four
  bitmaps over the unit slots are built for the seat by the radar pass's own rules and the engine's
  own bucket walk `0x47E890`: radar and sonar from the emitters of the seat's block and of every
  owner whose contacts it receives (`tagpu_allysight_contacts`), radar and sonar jamming from every
  jammer of any other owner. The emitter visitor is `0x467840`'s, rule for rule.
- **The radar read** (`0x40AB3F`) takes a unit when it is a radar contact not radar-jammed, or when
  its origin cell is lit for the seat and it is not cloaked: the pass's fifth loop, on the seat's
  counters under true LOS and the seat's explored-map bit otherwise, with the map-edge row of
  `tagpu_patches.c`'s shear fix.
- **The sonar gate** (`0x465B30`) answers per seat (sonar and not sonar-jammed) for the seat whose
  acquisition is running, and with bit 9 for every other caller: the display keeps the bits.
- **State**: the bitmaps and the scope are `SIM_TRANSIENT`, built at the acquisition's start and
  closed at its end. Nothing is recorded: the inputs are the units, the records, S and the explored
  map, all of which the recording carries or rebuilds.

The viewed player's own seat acquires by the same rules, so nothing the simulation reads depends on
who is viewed, and a contact counts from the moment the seat acquires rather than from the viewed
player's last pass. Only the seat's own targeting facility (`acq+0x79`, the list's consumer
`0x40AD80`) lets it fire on a radar contact, as in stock.

### For a skirmish AI

An AI must know what its seat knows and nothing more: its own and its allies' sight and contacts,
never the viewed player's. `tagpu_seatcontact_scan(seat, out, n)` gives exactly what the
acquisition uses, per unit slot: `TAGPU_SEAT_CONTACT` (the radar list takes it),
`TAGPU_SEAT_SONAR` (a sonar contact) and `TAGPU_SEAT_SEES` (`UnitInPlayerLOS` for the seat, its sonar
answered per seat). It answers for a seat with an acquisition record, a local one. One call costs
one radar pass plus one `UnitInPlayerLOS` per live unit; call it once per decision cycle, on the
game thread, never from inside an acquisition. An AI that keeps its
own copy of the radar pass must walk the emitters of every owner `tagpu_allysight_contacts` names for
its seat and skip their jammers, or call this instead; reading `+0x110` bits 8–10 reads the viewed
player's picture.

## The unit panel

The panel `0x46A860` shows the hovered unit, and five of its tests show more for the viewed
player's own units ([ui](../engine/ui.html), *The hovered unit's readouts*): the order target, the
health bar of a def that hides damage, the resource numbers with the kills line and the status
text, the stockpile bar with the target block, and the target's own health bar. `tagpu_allypanel.c`
passes each also for an owner `tagpu_allysight_shares` names. For a remote ally (owner record
`+0x73` = 3) the stub enters past the four resource numbers, at the kills line, which sets its own colours:
its numbers are always 0 on your machine. A display only: the panel
writes nothing the simulation reads.

## Visibility

`UnitInPlayerLOS 0x465AC0(player, unit)` answers 1 at once for the player's own unit; for any other
it answers 0 for a cloaked unit (`+0x10E` bit 2) and for one wholly under water that is not a sonar
contact (`+0x110` bit 9), and otherwise tests four points of the unit's box against the player's
sight. Its owner test (`0x465ACE`, `ally_visible`) also passes an owner S shares with the player,
so an ally's units are visible as the player's own are (decision 8): cloaked, submerged and jammed
on sonar alike. Loop 1 of the radar pass already gives every allied unit bit 9, and a unit's own
stamp covers its cell in its owner's counters, which S shares, so the site decides chiefly for an
ally's unit that is cloaked, or submerged while an enemy jams its sonar.

MEASURED 2026-11-15, `allied-sight.json` against its hostile control, the viewed seat 0, the camera
over zone 6: seat 2's spy `spy6`, cloaked by the scenario's `cloaked` (its `+0x110` bit 11 and
`+0x10E` bit 2 set at every one of ten polls, GameTime 184..2054, in both), was in seat 0's draw
list HotUnits (`main+0x1435F`) at 10 of 10 polls allied and at 0 of 10 hostile.

**Targeting does not change.** The acquisition calls `0x465AC0` (`0x40AB11`) only when its seat's
row byte toward the owner's map bit `+0x146` is zero (`0x40AB05`, the seat's record being the call's
player). The owner test passes for another owner only when S shares the two records *and* both live
row bytes are non-zero — the player's toward the owner's map bit, the very byte `0x40AB05` tests,
and the owner's toward the player's — so the acquisition never reaches the new branch, by
construction. S alone would not do: S is recomputed at the players phase (`0x49555F`), the rows have
writers earlier in the step (`0x452A2D`, `0x452ADB`, `0x447225`, `0x447F7E`) and the unit tick
`0x48AD30` runs before the phase (`0x4954ED`), so for one tick after an alliance breaks S still
names the former ally. The other callers draw or describe: the order sprites `0x439761`, the unit
panel `0x46AF3D` and `0x46B5B4`, COB's `EMIT_SFX` test for the viewed player `0x480EE5`, HotUnits
`0x48BC56`, `0x494396`, and Impure's own (the frame packet, the order markers, the rings, the
icons).

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
| the temporary records' expiry's call `0x482161` | the removal only when the record holds a stamp: under the ray fan the add `0x482910` zeroes the record's byte `+0xB` and stamps only through the emitter, which leaves it 0 and stamps nothing when the reused slot's stale words name the new cell (`0x48261F..0x48262E`) or the cell is past the grid; the expiry removes every record past its time |
| the acquisition's calls `0x40AD3E`, `0x40B2E9` | builds the seat's contact bitmaps, runs the acquisition, closes the scope (`tagpu_seatcontact.c`) |
| the acquisition's radar read `0x40AB3F` | the seat's own contact, not bit 8 |
| `UnitInPlayerLOS`'s sonar gate `0x465B30` | the seat's own sonar during its acquisition, bit 9 otherwise |
| the unit panel's tests `0x46AC85`, `0x46B013`, `0x46B10E`, `0x46B457`, `0x46B611` | pass for an ally; a remote ally's resource numbers skipped (`tagpu_allypanel.c`) |

## On a puppet host

`ally_sight` runs on a puppet: the host's stamps build W as the recording machine's did. S is
`SIM_STATE`: at every capture point, frame visit and seek the replay's table write puts the
recording's S, and `tagpu_allysight_restored` migrates the counters to it under the lock, which is
held from `_restore_begin` over the write. The recorded S is data until it is a share set of the
form the players phase makes — a record in the set has its own bit and counters of the level's
length, a record outside it appears in no entry, and every pair is named both ways — and an invalid
one is repaired by intersection, never by adding (`share_repair`): a record without its own bit or
valid counters leaves the set, and a pair stays only when both entries name each other. A seek
stamps and removes under the host's S (R3–R8b) before R12, and the migration then moves them to the
recording's; R14a's temporary records are made after it. `ally_share` (the players phase) refuses to
act on a puppet. `ally_radar` runs: in the recording's viewed record `contacts_put` overwrites bits
8–10 from the image after the pass, in any other view the pass's bits stand, and no simulation reads
them on a puppet host (`tagpu_replay.c`, *THE RECORDED VIEW'S CONTACTS*). `ally_visible` runs: its
answer follows S, which the recording writes. `seat_contacts` runs where the acquisition runs, and on a
puppet host no seat has an acquisition record (`tagpu_replay.c`, *THE RECORDED VIEW'S CONTACTS*):
it runs there only for a seat taken over, whose acquisition is then the host's own.

W is `SIM_REBUILT`, the one declaration the class may have (the link's `sim-state-check.sh` and the
registry's attach refuse any other by name): never recorded, rebuilt on every host by the stamps,
carried in a dump after each record's counters (`RK_SIGHTW`, only for the records S keeps), where
`tarec compare-dumps` compares it between the two dumps and checks `u8 = min(W, 255)` in each.

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

- A one-way alliance cannot be made from the skirmish keys (ally groups are symmetric), so the
  both-ways rule's one-way case has not been run.
- No scenario has an allied unit an enemy jams on both radar and sonar, so the minimap's dot site
  is installed but its effect is not measured.
- A mid-game alliance change is proven in a network game only (`tools/mp_allysight.py allied`): the
  in-game ALLIES dialog is a network game's (Tab, `0x496144`; its `ALLIES` button, `0x4950FA`), and
  a skirmish has no other way to change an alliance once it runs. No test group exercises the migration of temporary sight records or a mid-game backfill of the
  explored map through `0x485420`: an allied test group's one migration is the first players
  phase's, which walks the units (no temporary record exists before the first death: the add
  `0x482910` has one engine call, the destructor's `0x486748`) and backfills only what the load's
  stamps explored.
- Exact removal is established for the ray fan only. The circle emitter (LosType bit 1 without
  bit 2: none of the SKIRMISH screen's choices makes it, Circular clearing both bits, so the allied
  test groups type `+lostype` and then `+los` to play under it) removes at
  `0x48275D` whenever the record's cell or byte changed, with no stamp test (DISASSEMBLED
  `0x48272E..0x48277A`); whether a record can reach it without a stamp is not established. One that
  did would under-count its cells, never wrap them: W stops at zero.
- The network load's leave racing the loader is the engine's race, not closed here.
- A two-player gate game froze once (2026-11-11: the joiner's private session open hung in its
  second DirectPlay join, its game thread stopped at tick 10) and has not come back in 14 games
  since, six of them with the humans allied. It is not established whether it was this feature's;
  a failed two-player gate game now saves both peers' stacks (`recmp_create_begin_test.py`,
  `dump_stacks`).
- A remote human ally's machine never makes your death vision (decision 7).
- Under Permanent or Circular LOS (true LOS off), `UnitInPlayerLOS` reads the viewed player's bit of
  the explored map (`0x4080E0`, `0x465E00`), so a seat's sight list there takes what the viewed
  player has explored. The radar read's own sight test uses the seat's bit.
- The radar pass's watcher rule (PlayerInfo `+0x9B` bit 6: every unit a contact) has no per-seat
  counterpart; a watching seat has no acquisition record.
- A remote ally's unit shows no resource numbers in the unit panel (decision 2).
- `tagpu_seatcontact_scan` has no caller yet: the skirmish AI is its first. What it computes is the
  acquisition's own (the bitmaps, the radar read, the sonar gate), which every skirmish with an AI
  runs; the scan's loop itself has not run.
- The install is all-or-nothing but not fail-closed: a machine whose exe bytes do not match the
  sites runs stock sight, and its peers are not told: the fingerprint that admits a joiner names
  the game's files ([mods](mods.html), *The fingerprint*), not which of Impure's modules installed.
  Its units then see less than its allies' do.
- The wiki's scenes keep their single-seat emulation.
