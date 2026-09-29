# Demo recorder — state design evidence

## Status and evidence boundary

**Desk research, 2026-09-28: no game was run and nothing was implemented for this page.** It is the
evidence behind the state-based design on the [product page](demo-recorder.md), beside the
packet-era [exploration](demo-recorder-exploration.md), read from the
[engine map](../exe-reverse-engineering.md), the notes, Impure's and TADR's sources (`vendor/TADR`,
main checkout, gitignored) and `pristine/TotalA.exe.pristine` (`objdump -D -Mintel`); the size
estimate is computed from the exploration's captures. Four research passes (puppet mode, rebuild and
takeover, per-player state, size) were each re-checked by an independent verifier that re-read every
cited line, disassembled every address again and re-ran every script. Only confirmed claims are
kept, stated as corrected where the verifier found an overreach; the verifiers' own findings were
disassembled again before use, and the few DIS claims read for this page alone are marked *not in
the record*. None of it is an experiment: see [§5](#5-open-experiments).

**Tags:** DIS disassembled; SRC read in source; MEASURED a live measurement, cited to the note that
made it; [INFERRED] a consequence not yet shown. `main` is `*(0x511DE8)`, a player record is
`main+0x1B63 + i·0x14B`, a unit slot is `0x118` bytes, a player's *type* is its controller byte
`+0x73`, and a game's type is `0x435100`'s answer (1 campaign, 2 skirmish, 3 network). Numbers
written with `0x` are hex and all others decimal, §4's units included (1 KB = 1,000 B,
1 MB = 1,000,000 B). **The record** is `_local/demo-recorder-exploration/state-design-2026-09-28/`
(main checkout, gitignored; `README.md` there): the scripts, their outputs, and `facts-results.json`
with every fact, its verdict and the verifier's note, under the IDs cited below (P puppet,
T takeover, L player, S size; a verifier's own finding is its topic's M number, as in size M4).

## 1. Puppet mode: how the engine treats a remote player

### Remote-player gates

**Puppets must be type 3** (P1, P2). `+0x73`, set by `0x463C60`, is 1 local human, 2 local AI,
3 remote human, 4 remote AI (TADR `tamem.h:1761-1768`). The unit tick (`0x48AD62..0x48AD9C`) and the
players phase (`0x464FCE..0x465024`) take only records with a nonzero first dword, a type of 1–3 and
`+0x146` ≠ 10; a type-4 owner's units get local movers (`0x43DC53`), and a menu-state loop resets
type 4 to 0 (`0x428446..0x42845B`). A host's AI seat reaches the other peers as type 3 [INFERRED:
the dispatcher admits only type-3 senders, `0x4547B3..0x4547E2`, and all 18,011 wire creates of a
four-peer run came from the owning record, MEASURED in the engine map].

**The unit tick `0x48AD30`** walks each such player's block `[+0x67, +0x6B]` (DIS; P3, P4):

| Work | For a type-3 owner's unit | Site |
|---|---|---|
| Wind scripts `0x437910`; COB `DoScriptsNow(1)` when `unit+0x9A` is set; the countdowns and the 30-tick health percentage; the pending-death reaper `Send_UnitDeath(unit, +0xF5)` | runs | `0x48ADC4`, `0x48ADDF..0x48ADEB`, `0x48AFB9..0x48AFD1` |
| `AutoAim 0x49E1A0` | skipped: the byte read is the **owner record's** `+0x73` (`0x48ADCE`), which must be 1 or 2 | `0x48ADC9..0x48ADDA` |
| [INFERRED] water damage (a kind-0xB hit below sea level, `0x48AED3..0x48AF32`), regeneration `0x41BD10`, the order controllers `0x43B7C0`/`0x43BAD0`, movement `0x43DD20` + `0x48A870` | skipped: an owner not of type 1 or 2 jumps to the reaper | `0x48AEC4..0x48AECD` |
| The `0x2C` sender `0x48B710` | local records in a network game only | `0x48AFE4..0x48B003` |

**Shots, hits, deaths** (DIS). A remote shot is a picture with sound: `0x499EB0` plays it, then
skips direct and area damage (`0x499CD0`, `0x49A120`) for an active type-3 owner
(`0x49A01B..0x49A047`; P7). A hit (`0x489CE0`) subtracts HP on any peer (`0x489EB5`), but only a
local victim is marked for death (`0x489EC6..0x489EEE`); a remote one is clamped at 0 (P8,
corrected). Only the owner's peer sends the death `0x0C` (`0x48664B..0x48666D`). Send_UnitDeath then
runs the destructor `0x4866D0(rec, 1)` on whichever peer called it (`0x486672..0x486679`), and a
peer receiving the `0x0C` runs `0x4866D0(rec, 0)` (case `0x45541C`). Either mode draws the corpse
and explosion and counts the statistics; only mode 0 starts `Killed` (`0x48684A..0x486877`), which
Send_UnitDeath has already run as a query on its own peer (`0x4865C3`) for a unit at HP 0 or
below whose kind is not 4, 5, 7 or 9 (`0x486525..0x48655C`) (P9, corrected). Two paths destroy a
copy in mode 1 on a peer that does not own it: a round-robin entry reading type 0 marks the copy
dying (`+0x110 |= 0x4000`, `0x48B426..0x48B42F`; P15) and the unit tick's reaper, which runs for a
type-3 owner too, calls Send_UnitDeath (P3); and kill-all `0x486F10` destroys a departed or remote
player's units directly (`0x486F73..0x486FB4`, called for a player who left at `0x452E17`; not in
the record, and SRC `tagpu_patches.c:7629-7632` says the same).

**Movers are chosen once, at creation** (P6). `0x43DC00` gives an active type-3 owner's unit an air
or ground proxy (`0x490940`, `0x44F570`) and any other owner's a local mover (`0x4907E0`,
`0x44F010`; `0x43DC48..0x43DD08`). A proxy never reports dirty (`[vt+0x1C]` = `0x44EFE0`) and only
reads the owner's payloads; the ground proxy's own step is a bare `ret` (`0x44EFB0`).

**Remote motion comes from the arriving `0x2C`** (P5, P13–P15). The receiver `0x48B920` stores the
sender's GameTime at the sender record's `+0x18` (`0x48B963`), creates copies
(`CreateFromNetwork 0x4861D0`), feeds dirty entries to their proxies, then runs `0x43DD20` +
`0x48A870` over every alive unit with a mover in the sender's block (`0x48BA28..0x48BA5C`), once per
owner tick. `0x43DD20` holds the `StartMoving`/`StopMoving`/`MoveRate` script starts (in
`0x43DA70`), so remote walk animations come from this sweep. A ground dirty entry is a path change
(`0x44F480`), an air one a goal or a dead-reckoned pose (P14, corrected). Each `0x2C` ends with one
round-robin full-state entry for slot `GameTime % N` (`0x48B200`, `0x48B3F0`): type, HP (the only
`0x2C` writer of a copy's HP), build fraction, state byte, layer, and a carrier or position and
turns, re-stamping grid and sight on a cell change. A slot recurs every N owner ticks: 50 s at
1,500.

**Event receivers are plain functions**, reached from the table `0x455F84` at index code − 2 (DIS;
P17–P20): death `0x0C` → `0x4866D0(rec, 0)`; weapon fired `0x0D` → `0x49D270`, which rebuilds the
shot without damage; impact `0x0E` → `0x49AF90`; script start `0x10` → `0x4B0B00(unit+0x9A, …)`,
with an alive test but no bound on the unit index (`0x4554FF..0x455516`), while `0x4B0B00`'s lookup
`0x4B08C0` refuses a script index below 0 or at or above the script count `[[cob+8]+4]`
(`0x4B08C5..0x4B08D0`) and then nothing starts (`0x4B0B0D..0x4B0B22`; not in the record); state
`0x11` → `0x48B090`; build done `0x12` → `0x41B8D0`; sound `0x13` → `0x47F0C0` or `0x47F300`.
Neither `0x49D270` nor `0x49AF90` reads its sender argument. Impure's B4/B8 stub already calls
receivers so, pushing the stock case's return address (`tagpu_patches.c:6780-6816`) because
Impure's receivers classify traffic by it (P30).

**Local simulation escapes the owner gates** (P29): the fire spread's call at `0x423BE0` reaches
area damage through `0x49A0C0`; `0x4244B0` damages features locally outside a network game; meteors
(`0x437DE0`) fall on every peer; the grid claim `0x47CC30` lets a newcomer evict a type-3 incumbent.

### Hosting without a network session

- **The message layer is inert without the network bit** (P21, P22). With `main+0x2A44` bit 0 clear,
  `0x451DF0` returns 1 without sending (`0x451EAB` → `0x451FA9`) once the sender passes its test
  (`0x451E86..0x451EA5`), and the pump `0x453D40` returns at once (`0x453D4B..0x453D5C`): no `0x2C`
  sweep, no event case. With a session, a message is dispatched only from a DirectPlay sender
  matching an active type-3 record's `+4`, which is why §6a's packet playback needed real sender
  identities.
- **The frame branches on the network bit, not on the game type** (P23, corrected;
  `0x4967E2..0x4967ED`). A non-network frame skips the simulation while `main+0x37EBE` bit 0 or
  `IsGamePaused` (`main+0x38A51`) is set, else runs `0x495230` and `0x495490(0)`
  (`0x496918..0x49693E`). Inside the tick, stock applies remote state just before the unit tick
  (`0x4954B7..0x4954ED`), where a puppet driver's writes belong.
- **A non-network level sheds what §6a needed** (P23–P25). Unit blocks follow seat order, the DPID
  compare running only when `0x435100` answers 3 (`0x485666..0x485682`, `0x485940..0x485972`); the
  loader's wait on `main+0x38D75` bit 3 is type-3 only (`0x4975A5..0x4975F1`); the battle room's
  restriction object `main+0x2A30` is read with no null test by `0x46CA60`, whose only callers are
  menu code (`0x428345`, `0x4283BC`, `0x42845D`), and its absence crashed a type-3 save load at
  `0x46E167` (MEASURED, [§6a](demo-recorder-exploration.md#6a-first-solo-engine-playback)).
- **Seat puppets before the level initialises its players.** `0x464990` (at `0x4919C8`) runs the
  player init `0x464700` only for records with a nonzero `+0x73` (`0x46499E..0x4649B1`). The init
  stamps `+0xF0/+0xF4/+0xF8`, zeroes resources, share limits and kill counters, clears `+0x149`
  bit 0, allocates `+0xEC`, sizes the sight grid `+0x7C` and, for all but an active type 3, builds
  the two per-player objects below (`0x46489D..0x4648CD`).

### Movers, AI objects and takeover

- **No stock path swaps a live unit's mover** (P26). `0x43DC00` is called by the local create
  (`0x4860D5`), `CreateFromNetwork` (`0x4862E9`) and two routines nothing references; only the
  destructor frees a mover (`0x486DA9..0x486DBF`), and a slot's owner is fixed by its block at level
  init (`0x485902`). One unreferenced routine, `0x485E50`, is a complete install (`0x4B4F10(0x2F)`,
  `0x43DC00(unit)`, store at `unit+0`) that also resets the heading `+0x66` to BuildAngle
  `def+0x210` and leaves the old mover alone (puppet M3). The takeover does not swap movers in
  place: it makes the seat local and then re-creates the units, which gives each a mover of the
  new type and fresh COB (*COB and owner-only effects on a puppet*, below).
- **The `AI` command only flips the byte** (P27): `{"AI", 0x416280, 4}` at `0x501FD0` (run level 4)
  calls `0x463C60(2)` on a type-1 seat and `0x463C60(1)` on types 2 and 3 (`0x4162CF..0x416303`).
- **The `+0x74` object** (0x3D bytes, `0x408CB0`), ticked by `0x408C40` (`0x46502A..0x465031`),
  plans only for an active type 2 (`0x408C45..0x408C50`: `0x408830` every 30 calls, ten sub-objects,
  `0x4089A0(1)`); other types run `0x4089A0(0)` (puppet M1). `0x4089A0..0x408BDF` is the seat's
  **periodic weapon acquisition**, humans included (DIS, not in the record): each call walks
  N/30 + 1 slots of the seat's block from the cursor `obj+0x39`, N being `main+0x37EE6`
  (`0x4089B9..0x4089D1`, `0x408BB1..0x408BD2`), and for each finished unit with `+0x110` bit 31 set
  and `+0x110 & 0x300000` = `0x200000` (`0x408A0F..0x408A41`) [INFERRED: the fire-state group]
  checks or searches each weapon slot's target (`0x48A190`; `0x40B7B0` → `0x40AD80`); the argument
  only decides whether weapons with `weapon+0x111` bit `0x4000000` are skipped
  (`0x408A88..0x408A96`). A seat built as 1 or 2 and flipped to 3 keeps the object; a flip to 2
  revives the planner, all the `AI` command needs.
- **The acquisition record**: `0x40B320(+0x146)` allocates 0x10D bytes at the global
  `0x5119C0[index]` beside `+0x74` (`0x4648CD`; freed with it, `0x464A41..0x464A6B`; puppet
  M2). The players phase calls `0x40B2C0` for any type-1–3 record (`0x465037`), which rebuilds its
  visible and radar lists every 30 ticks (`0x40AA40`). Not in the record (DIS): `0x40B2C0` returns
  at once for a NULL slot (`0x40B2CC`) and otherwise also draws the shared simulation RNG once per 30
ticks (`0x4B6C30(0x1E)` at `0x40B301`), so building this object at a takeover shifts the shared
random sequence from that point; and the target search `0x40AD80` reads `0x5119C0[slot]` with
  no null test (`0x40AD95..0x40AD9C`). A seat that was type 3 at level init has neither object, so
  making it local at a takeover, the player's own seat included, needs both built first [INFERRED:
  otherwise no periodic acquisition, and a null read at `0x40AD9C`].
- **Re-creation can keep the recorded slots** (P28): `0x485F50`'s eighth argument names an absolute
  unit index, taken only if free and inside the owner's block (`0x486002..0x486043`), and B4's hold
  applies only when it is 0 (`tagpu_patches.c:6369`). The engine passes it only in the save restore
  (`0x48718E`), on the loader thread.

**What §1 means for the design.** Type 3 is the lever: orders, movement, targeting, damage and death
decisions, regeneration, the economy and AI planning are local-only, while COB, hit reactions, death
pictures and sight keep running. The puppet level is a non-network game (type 2, network bit clear),
with no DirectPlay, DPID sort or load barrier (nor, [INFERRED], the battle room's restriction
object), and seating each player at its recorded rank with the recorded N reproduces the blocks.
A recording of more than four players also needs records 4–9 made active, which a stock skirmish
does not do (it refused every unit of slots 4–9: [scenario format](../scenario-format.md),
MEASURED 2026-09-24).
Impure drives motion (round-robin-style writes with walk scripts as recorded script starts, or the
movement pass on fed proxies) and calls the stock receivers in-process with bounded indices; a
skirmish recorder hooks engine functions, as messages exist only in network games. A takeover
flips `+0x73` first, builds both per-player objects, and then re-creates the units at their recorded
slots, as the save restore does; a flip in place would leave every unit's COB on the branches it
took as a remote copy (below).

**Open:** records 4–9 active in a type-2 level; a level whose seats are all type 3, with
`main+0x2A42`/`+0x2A43` naming a type-3 record
(the seatless viewer, also open in the [data-keys evidence](data-keys-evidence.md)); commanders for
type-3 seats; what `0x48A870` does; the acquisition's effect on a puppeted seat's weapons; defeat,
victory and ENDMSN with type-3 seats in a type-2 level.

### COB and owner-only effects on a puppet

DIS unless marked; the engine facts are in the engine map's *The nano spray* and *COB on a puppet*
([engine map](../exe-reverse-engineering.md)), and the decisions they led to in the
[completeness design](demo-recorder-completeness.md#decisions-taken).

- **A puppet never sprays by itself.** The nano spray's two emitters are called from 17 sites in
  15 order handlers, which run only through the main-list order controller in the unit tick of a
  unit whose owner is type 1 or 2, and never from the order destructor. A remote builder's copy
  shows no spray in a live network game either. The recorder records each spray where the engine
  makes it and replays it through the same emitter (decision 1).
- **Getter 75 is the one COB answer that depends on the machine.** Impure's getter answers 1
  when the queried unit's owner is type 1 or 2, so a puppet, whose seats are all type 3, answers 0
  for every unit, as a live peer does for units it does not simulate (MEASURED on two Wine peers in
  all five supported mods, [cob.md](cob.md)).
- **How the supported mods use it** (the compatibility suite's mod archives, their COB bytecode
  scanned statically, 2026-09-29): 494 call sites in 200 scripts, Escalation 470, Total Mayhem 14,
  TA Twilight 10; TA Zero, ProTA and the retail scripts have none. Every site feeds a single
  branch that skips a block unless the unit is simulated here; none has an else arm. **375
  branches are owner-only presentation** (indicator pieces in 170 Escalation `Detect` scripts,
  factory exit arrows, transport and gate indicators) and **119 are owner-only engine actions**
  (ATTACH and DROP for module absorption, transports and gate teleports; SET ACTIVATION, BUSY,
  INBUILDSTANCE and YARD_OPEN; debris EXPLODE). ATTACH and DROP send a `0x0A` from every peer
  that runs them, which is why the mods gate them [INFERRED].
- **Unguarded COB also writes engine state on every copy.** Scripts that never test 75 set
  `+0x10E` and `+0x10F` bits on remote copies in every mod and in retail, and attach or drop units
  in every mod (for example SET ARMORED in 389 Escalation scripts and ACTIVATION in 164; the
  retail scripts set INBUILDSTANCE in 49 and YARD_OPEN in 21, and never attach) [counted
  statically]. On a puppet those fields would
  have two writers, the scripts and the recording, and every disagreement would restart
  Activate or Deactivate: hence the one-writer rule (decision 2).
- **A flip in place keeps each unit's COB.** Only a create gives fresh COB, and Create's first
  slice reads 75 inside the create. Escalation starts a one-shot `Upgrade` script from Create in
  eight factory types, and it returns at once when 75 answers 0: after an in-place flip those
  factories would never upgrade again [INFERRED from the scripts]. A fresh create also resets
  what a mod keeps in COB statics and piece visibility, such as Total Mayhem's upgraded metal
  extractor's look [INFERRED from the scripts].

## 2. Rebuild and takeover inventory

### The scenario applier today

`apply_now` (`tagpu_scenario.c:1844-1896`) applies in one visit (T1, T5): it resolves every entity
(with `on_error: abort` a failure creates nothing), writes the switches and resources, snapshots and
clears, creates units with
`UNITS_CreateUnit 0x485F50(owner, type, x<<16, alt<<16, y<<16, fullHp 1, stateMask 1, index 0)`
inside `tagpu_kill_hold`/`tagpu_kill_flush`, then spawns features, issues orders and resolves the
camera target. The visit is at `0x4969D2`, after the draw call at `0x4969CD` in the frame
`0x496790`: on the game thread, outside the tick loop. The camera itself is placed afterwards from
the frame hook (`place_camera`, `tagpu_scenario.c:2107-2150`, called at `:2305`), which hands the
eye to the command apply at the next in-play draw.

- **A paused frame reaches it** (DIS): the pause tests skip only the tick (`0x496920..0x496926`,
  network `0x4968AA..0x4968B0`), and every path reaches the draw and `0x4969D2`. Unmeasured live.
- **`dress_unit`** (`tagpu_scenario.c:1350-1398`; T2) writes the heading in whole degrees, stance,
  nanoframe and health in 1 % steps, and kills, at whole-unit positions. A recording needs 16.16
  positions, the full turn, the exact HP and fraction, the layer and the slot.
- **`clear_existing`** kills a snapshot of the alive units with `UNITS_KillUnit(u, 0)`, parking
  ActiveCommanderDeath at 0, before the creates (creating first lost 302 of 401 units to the
  per-player cap, MEASURED, `scenario-format.md:723`). Its walk (`tagpu_scenario.c:1253`) stops one
  slot short of the array's inclusive end, which the engine's sweep includes (`0x48BD1C..0x48BD38`);
  features and wrecks stay (T3).
- **The kill is synchronous and silent** (T4): on a live unit `Send_UnitDeath 0x4864B0` gives
  severity 0 and no corpse (`0x486552..0x4865CE`) and runs the destructor at once (`0x486672`):
  passengers hit with 30,000, orders cleared, COB and object state released, mover and slot freed.
- **A create's side effects** (`0x4860BB..0x4861B3`; T16, T17 corrected): whatever fullHp says, the
  mover (mobile types), the layer from argument 7, the `0x09` send, sight (`0x482AC0`) and the
  `+0x144`/`+0x140` counters; **only with fullHp** (`0x48611A`) a self-addressed `0x12` for
  `def+0x22F` = 0 types, activation for `def+0x241` bit 18 and the pending-death setup for bit 24.
  It refuses type 0, a type lacking `def+0x241` bit `0x800000` and a type at its limit `def+0x15A`
  in the owner's block (`0x485F7C..0x485FE4`).
- **B4's hold** (T19): a create with argument 8 = 0 never takes a slot freed in the same tick, and
  takes one freed in the previous tick only when its block has no free slot outside the hold
  (`tagpu_patches.c:6260-6269`, `6369-6390`). The clear frees in the same tick as the creates, so
  clearing and recreating a nearly full block in one visit gets NULL creates for the surplus
  [INFERRED]. Argument 8 is exempt.
- **MEASURED** (T6, corrected): 402 creates and 400 orders in one visit, 0.2 s of CLI round trip, no
  visible stall (`scenario-format.md:751-753`); `scenario load` from a stopped instance to that
  200v200 state in 6.4 s, launch and level load included ([scenario format](../scenario-format.md),
  phase D). Nothing above 402 units has been timed.

### Orders and factory queues

- **Orders** (DIS; T9–T11). A node (0x56 bytes, `0x43A0C0`) holds the action `+4`, target `+0x16`,
  position `+0x22/+0x26/+0x2A`, p1/p2 `+0x36/+0x3A`, flags `+0x42` and next `+0x4A`; `unit+0x5C`
  heads the main list. `0x43AFC0` with shift 0 goes straight to the allocator `0x43ADC0`
  (`0x43AFD4..0x43AFDA`), which, unless the new order's flags `+0x42` carry `0x40`
  (`0x43AE0E..0x43AE12`), destroys the standing orders lacking flag `0x4` (`0x43AE02..0x43AE59`), so
  the applier leaves each unit its last order. With shift 1 it first scans for the same action,
  target and a position within ±16 wu on x and z (`0x43AFE0..0x43B029`) and **cancels** a match
  instead of queueing (`0x43B034..0x43B087`); `0x43ADC0` called directly with shift 1 appends
  without the scan.
- **Factory queues are count nodes** (T12–T14). `0x43B0B0(action, unit, type, amount)` adds to the
  last node of the same action and type, else appends, and destroys a node counted down to zero, so
  A, B, A is three nodes. Its only caller, `0x419B00(name, builder, amount)`, resolves the type and
  the action by name and is called by the human build-menu click (±1, or ±5 with SHIFT,
  `0x41ABFA..0x41AC3C`) and two AI sites. The factory handler `0x402640` builds while the head count
  is positive and decrements it per completion (`0x402B07..0x402B39`); the unit in progress is a
  nanoframe linked through `+0x16`.
- **Destroying an order node has side effects** (DIS; engine map, *The nano spray*). The
  destructor `0x43A1F0` runs the order's handler with event 2 when `node+6` has bit `0x02`, and
  BuildingBuild's cleanup then deals the unfinished nanoframe 30,000 (`0x402701`); it starts
  `StopBuilding` when `+0x42` has bit `0x400000`. So the applier's shift-0 order to a factory
  mid-build kills its nanoframe. A puppet's order nodes are therefore written with neither bit,
  so that replacing them runs no handler and starts no script [INFERRED from the destructor].
- **Untraced** (T15): rally inheritance (`0x41B8D0` copies no order) and adopting an existing
  nanoframe into an order. **Carried units** (T18): `+0x86` carrier, `+0x8A` first passenger,
  `+0x8E` next, `+0xF9` attach point, written by `0x48AAC0`.
- **Stock AIs adopt applier-created units** and re-plan within seconds (MEASURED, T26: a keepalive
  CORCOM walked off and built a CORFMKR, [smooth motion](../smooth-motion.md), gate 4); whether an
  AI overrides a written order at once is unmeasured.

### Per-unit state

| Class | Fields (T29, T30 corrected) |
|---|---|
| Set by the applier | type, owner, position, altitude, heading, HP, build fraction, `+0xF6`/`+0xF7`, stance, kills |
| Settable by a known engine call | orders (`0x43AFC0`, `0x43ADC0`), factory queues (`0x43B0B0`, `0x419B00`), carry links (`0x48AAC0`), layer and slot (create arguments 7, 8), the state byte `+0x10E` with its activation, armoured and cloak bits (`0x48B090`, which stores the whole byte at `0x48B0D8`, not in the record; cloak is bit 2, L14), squad membership `+0xAC` (`0x480250`) |
| Rewritten by the engine | sight height `+0xF8` (every create or move), `+0xF5` (hits, the create) |
| Restored only by the save load | the full turn, three weapon slots at `unit+0x4` (stride 0x1C: target, reload, stockpile), last attacker `+0xF0`, the rest of `+0x110`, COB state (`0x4B2040`), the mover (`0x43DE30`) |
| Unknown | velocity and path (the mover's 0x2F-byte layout), fire state (not located; [INFERRED] `+0x110 & 0x300000`), per-unit AI state |

### The savegame as the engine's checklist

The stock save is the engine's own inventory of a world (DIS; T20–T24). The saver `0x4876C0`
writes, per unit, its COB state (`0x4B1EC0`), both order lists with actions and built types by NAME
(`0x43A970`), the mover (`0x43DD70`) and a `+0xBC` sub-object (`0x4010B0`), then a 0xB8-byte record
(built from `0x4877DF`, written at `0x487A9E`). The restore (`0x497B29` → `0x432610` →
`0x486FD0`) runs in a saved game's level load, on the loader thread: units are created by type name
at their saved index (argument 8, `0x48718E`), carriers first, with HP and `+0x104` written after
the create (`0x4871B5`, `0x48727C`); order nodes are built by `0x43A420` and linked in saved order,
bypassing `0x43ADC0`. A loaded game starts paused with no projectiles, and a type-3 save is no
checkpoint: it crashed at `0x46E167` (§6a).

**What §2 means for the design.** The rebuild recipe, one visit outside the tick: an exhaustive
clear (the off-by-one fixed, features and wrecks included); creates at the recorded slots through
argument 8, exempt from B4's hold, so recorded targets map one to one as in the save restore;
carriers before cargo; unfinished units with fullHp 0 [INFERRED], so no self-addressed `0x12`
completes them; a first order through `0x43AFC0` (shift 0), the rest through `0x43ADC0` (shift 1);
factory queues by name, one `0x419B00` or `0x43B0B0` call per run of one type. A fork is a
single-player game, so B8 and per-peer ownership are off the takeover path; forking a network
recording needs a slot remap from its DPID-ordered blocks. Weapon state, the mover's velocity and
path and any squad state beyond membership are lost unless each gets a writer; cloak and squad
membership have one (`0x48B090`, `0x480250`).

**Open:** live queue writing (shift 1, `0x43ADC0`, `0x43B0B0` at the apply point); rally
inheritance; nanoframe adoption; the mover's layout; airborne creates; apply time up to 1,500 units
and the applier's caps (`tagpu_scenario.c:139-145`); clearing a loaded transport [INFERRED: the
passengers, hit first, die with a nonzero severity]; a paused apply, live.

## 3. Player and perspective state

### Resources and the economy pass

`player+0x8C` holds floats for energy current, production and expense (`+0x8C..+0x94`), the same for
metal (`+0x98..+0xA0`) and storage (`+0xA4/+0xA8`), doubles for the produced, consumed and wasted
totals (`+0xAC..+0xD4`), and base storage (`+0xDC/+0xE0`) (TADR `tamem.h:82-100`; L1). **The economy
pass `0x401360`** (DIS; L2) re-sums storage from the live, finished units (`0x40137D..0x4017C5`;
the finished test, `+0x104` = 0.0 at `0x4016C5..0x4016D6`, is not in the record), adds base
storage only while `+0x149` bit 0 is set (`0x401941..0x4019AA`), writes production, expense and the
totals, and carries current forward, clamped to storage with the excess to wasted
(`0x401A1D..0x401B27`); local-AI income is scaled by the difficulty. Its only call (`0x46555A`) is
on each type-1 or type-2 player's **30-tick pass** (`0x46507D..0x465092`), skipped while
`main+0x3923B` bit 2 is set or the countdown `main+0x39239` is ≥ 0, and once a player's units are
all gone (`0x465525..0x465537`).

**`+0x149` bit 0** is set, with base storage of at least 200, by level setup: `0x496E90` on the
campaign branch (`0x465F42`), and inlined in the skirmish seat setup (`0x496F5D`) and the network
setup (`0x4977D8`), a third copy (`0x497114`) sitting in `0x497080`, which nothing calls (not in
the record). It is also set by the Deathmatch respawn in play (`0x496E90` at `0x46540F`, after the
respawn create at `0x4653D9`, not in the record) and by the save's Players loader (`0x466200`); the player init
(`0x4647EF`) and a commander's death (`0x486512..0x48651F`) clear it (L3, L4, corrected). So an
in-play write does not stick (MEASURED: 4321 metal against 50 storage read 50 again,
[scenario format](../scenario-format.md), phase C; the same run's 12 → 50 is unexplained), and a
clear that kills a commander drops its player's base storage for good unless the Deathmatch respawn
sets the bit again [INFERRED].

**Replication** (DIS; L5–L7). Only `0x28` carries resources: its sender `0x4573D0` packs current,
storage and the six totals, never production or expense (`0x4573FD..0x45749C`), from the viewed
player's pass in a network game, every fourth pass (`0x46555F..0x4655A1`). An AI hosted on a peer
sends no periodic one, and a remote commander read zero production and expense (MEASURED,
[§6c](demo-recorder-exploration.md#6c-remote-perspectives-require-authoritative-contributions)).
TADR's replay derives remote rates from consecutive `0x28` totals
(`vendor/TADR/src/DDraw/TenPlayerReplay.cpp:279-351`), a fallback bound to that 120-tick cadence.
Transfers (`0x464C60`) pass through the `+0xEC` records, a type-2 recipient getting 50 % at
difficulty 0 and 70 % at 1 (L24).

### Sight, exploration and radar

- **Sight is counters recomputed from units** (L8, L13; T25 corrected). `player+0x7C` is one u8
  counter per 32-wu cell (half the feature grid), bumped per stamping unit (`0x482270`); a create
  stamps (`0x482AC0`), a death removes (`0x486845`, only while LosType bit 1 is set,
  `0x486838..0x486842`), and every alive unit of every type-1–3 player is re-stamped each tick
  (`0x4827B0`, `0x465046..0x465063`) from its cell and height byte `+0xF8`.
- **LosType** `main+0x14281` (L10, L11): bit 0 mapping, bit 1 true LOS, bit 2 ray fan, bit 3 fog
  current; only True restricts sight ([line of sight](../line-of-sight.md)). It changes mid-match: a
  defeated controlled player in a network game becomes a watcher with bits 0–1 cleared and
  `0x4816A0(1)` (`0x46569D..0x4656CE`), and chat commands flip bits (`LOSType` at run level 1;
  `LOS`, `Mapping`, `NowISee` at level 2). `0x4816A0`'s other callers are the Deathmatch respawn,
  `MakePoster` and the level load.
- **The explored map** `*(main+0x14273)` holds one bit per player per cell (L12).
  `0x4816A0(arg ≠ 0)` refills it all unexplored or all explored by bit 0 (`0x4816AF..0x4816FB`);
  with any argument it refills every type-1–3 sight grid and, while LosType bit 1 is set, re-emits
  the live units (`0x481808..0x481814`). The save's Mapping loader `0x484FA0` writes the whole
  block in place.
- **Radar and sonar** (L15, L16 corrected) are one set of bits per unit (`+0x110` bits 8–10) for the
  viewed player only, set by `0x467440` on that player's 30-tick pass: own units, allies whose owner
  shares radar (`PlayerInfo+0x97` bit `0x40`, `0x4674BA..0x4674D9`), everything for a watcher; then
  jammers clear them. The minimap radar picture `0x466DC0` is redrawn every tick. ShareMapping has
  no stock reader beyond its chat toggles; ShareLOS has neither a reader nor a reachable toggle.
- MEASURED (§6c; L9): one remote sight grid differed from its owner's in 34 zero/nonzero cells while
  the positions matched. The cause is unknown.

### Alliances, sharing, options and statistics

- **Identity and alliances** (L17–L19): the DPID at `+4`; `+0x146` is the record's own index; each
  peer is its own player 0, so indices differ between peers, and network blocks follow DPID order.
  `AllyFlagAry` `+0x108..+0x111` is directional, indexed by the other record's `+0x146`; `AllyTeam`
  is `+0x13F`.
- **Sharing** (DIS; player M2, M3): `PlayerInfo+0x97` holds ShareMetal `0x2`, ShareEnergy `0x4`,
  ShareMapping `0x20` and ShareRadar `0x40`, toggled by level-1 chat commands from the table
  `0x501EB8..0x501F0C`, whose ShareAll `0x419090` flips all four (not in the record). ShareLOS `0x08` has a toggle,
  `0x418F10`, that nothing references, and the other nine writes to `+0x97` change bit 0 only
  (`0x4280F3`, `0x428117`, `0x45035D`, `0x451334`, `0x45156C`, `0x45193A`, `0x451943`, `0x452FF8`,
  `0x46460F`; not in the record), so no stock path is known to set it. The auto-share `0x457D30`
  gives shared metal or energy above the limits `+0xE4`/`+0xE8` to allied type-3 players every 60
  ticks of a network game.
- **Options** (L20, corrected): ActiveCommanderDeath `main+0x37EF6`, SoftwareDebugMode `+0x37F2F`,
  LosType, and the unit array's count `+0x37EE6`, copied from `+0x37EEC` at game start (a network
  game takes the host's `PlayerInfo+0xA5`, `0x4973B5`). **Difficulty** is one global dword,
  `main+0x37EEE` (T27), restored by the save load (`0x492701`) and read by local-AI income, the
  reclaim payout (`0x4238C9`), the destructor, transfers and AI code; no seat has its own.
- **Statistics** (L21–L23, corrected): every peer's destructor counts kills `+0xFC` (only when its
  copy of the victim reads finished, `0x4868C1..0x486906`) and losses `+0xFE`; creates count
  `+0x144` (live) and `+0x140` (created). ENDMSN copies names, kills, losses, produced and wasted
  totals and a score at teardown (`0x41DC20`, from `0x491B8B`). The save's Players loader
  (`0x466050`) restores resources, totals, kills, losses and alliances, not production, expense or
  `+0x140/+0x144`.

### Presentation state and clocks

- **Camera and cursor** (L25, L26): the eye `main+0x1431F/+0x14323`, the scroll target
  `+0x14327/+0x1432B`, the follow slots `+0x142F3/+0x142F7` (Impure's zoom exists only in Impure);
  the cursor on screen `+0x2C76/+0x2C7A`, the world point under it `+0x2CAA`, the unit under it
  `+0x2CBA`, and the cursor kind `+0x2CBE`.
- **Controlled and viewed** (L27): `main+0x2A42` (orders, selection) and `+0x2A43` (sight, fog,
  bars, radar). Selection is `unit+0x110` bit 4, the group `unit+0xAC`.
- **Clocks** (L28, corrected): GameTime `main+0x38A47` is per peer (paused pairs 836/838 and
  1,620/1,623, §6c; in the §6 trace the peers' `0x2C` stamps ran 720..3,640 and 726..3,645). All
  30-tick passes start in phase (`0x46470C..0x464715`), and `0x495490` can run several ticks in one
  frame.

### Other players' units lag their owner

A copy's HP and build fraction refresh only from the round robin (50 s at N = 1,500) or a `0x12`,
and nothing carries kills ([scenario format](../scenario-format.md), `kills`). MEASURED: a copy read
unfinished 26–37 s after a create at 1,500 units ([sim fixes](sim-fixes.md), B8); ten peers at 1,500
units each agreed on all 7,307 units' slots and types, with 95.8 % at identical positions and the
rest up to 40 ticks of travel behind, partly pause skew ([raised limits](raised-limits.md); P16); in
[§6a](demo-recorder-exploration.md#6a-first-solo-engine-playback), 70 host-owned and 76 joiner-owned
units' heights differed from their controlling peer (P25).

**What §3 means for the design.** Resources come from the owner's machine: production and expense
for its players, everything for the AIs it hosts. A takeover restores base storage with `+0x149`
bit 0 (while the commander lives), then units, then current; a type-3 record keeps written values,
as the pass skips it. Sight is recomputed from the written positions every tick (`0x4816A0(0)` at
once after a seek), close to what the player saw but not identical (§6c); the explored map is stored
while bit 0 is on and written back after every `0x4816A0(1)`, and LosType is recorded as it changes.
A perspective's radar comes from pointing `main+0x2A43` at it. Players are keyed by DPID in a
network recording; a skirmish record's `+4` has never been read, so a skirmish may need the seat
index as its key. Alliances are kept as directed pairs plus `AllyTeam`; share bits, limits, totals,
kills, losses, `+0x140/+0x144` and the watcher bit are restored, since ENDMSN, elimination and the
economy gates read them. The engine's one difficulty matches the product page. Presentation is each
machine's own contribution, sampled at a tick boundary with its source tick and merged through a
defined clock mapping. HP, build state, kills and height are exact only at the owner, since a
machine's copy of other players' units is a receiver's copy; whether owners send them is open
([product page](demo-recorder.md#open-decisions-and-evidence-needed), other players' units).

**Open:** the 12 → 50 reading; a seatless viewer's radar and fog; the cause of §6c's 34 cells;
the `+4` DPIDs of skirmish records.

## 4. Size estimate

**Measured coefficients** (MEASURED; scripts and outputs in the record). The §6 host trace is
97.424 s of a two-player network battle on Two Continents: 450 ARMMERL west and 450 CORVROC east on
hold stance, alive 2 → 902 at 0.76 s → 362, mean 478.5, and no unit walked or turned. The 10 Hz
renderer samples are §6d's busy kbot battle (ticks 464–765, about 390 units) and §6b's late one
(ticks 5,979–6,276, 150 units), about ten seconds each, without structures.

| Quantity | Measured | ID |
|---|---|---|
| Trace packets, raw | 4,824 B/s: damage 2,063 (42.8 %), `0x2C` 914, create 600, script start 465, death 360, weapon fired 319 | S2 |
| Trace `0x2C` | one per player per tick; 900 dirty entries, all in the create burst; 5,001 messages at the 11-byte floor, 61.8 % of `0x2C` bytes | S5, S6 |
| Trace events, 1 s zstd-3 blocks, 6 B framing | 706 B/s, 464 without damage; 0–40 s 1,570 B/s, 40–97 s 104 B/s | S3 |
| Marginal cost per event | fire 22.0 B, create 10.0, death 9.7, damage 7.6, feature 4.3, script start 3.0, resources 43 | S4 |
| Units changing per sample | busy 69.0 % (position 63.6 %, heading 26.1 %), late 34.4 %; `+0x110` changes in 7.4 % and 4.2 % of units (overlap with the above not measured), not encoded | S10; size M4 |
| Changed-only stream (int8 whole-px deltas, u8 heading, i16 HP, u8 build) | 2.4–3.3 B per changed unit-sample; busy 16.4 B/s per alive unit (22.8 MB/h), late 9.8 (5.3 MB/h); 5 Hz is 62–63 % of 10 Hz; 1/16 px and 16-bit headings ×2.0–2.15 | S11, S12 |
| Keyframe row | 9.0–9.8 B per unit; full precision 10.4–11.2; renderer unit+piece 37–40 | S13 |

**Assumptions** [INFERRED], each to be replaced by a measured hour. *f*, the fraction of alive units
changing per 10 Hz sample over a real hour, is 0.10–0.40, the largest unknown (the samples give
0.34–0.69 per 10 Hz sample, 0.37–0.73 at 5 Hz, for all-mobile battles; the trace about 0); *c* = 2.38–2.86 B per changed unit-sample,
S11's two 10 Hz measurements, comes from dense samples, and sparse changes probably cost more.
Events are 0.2–2.5 B/s per alive unit, the trace's quiet and busy windows; the ten-player top of
5 KB/s leans on big-battle rates from games of 3,000–6,000 units (1,500 × 2.5 is 3.75 KB/s). A
keyframe every 30 s costs 10.8 B per unit, the middle of the full-precision row, since snapshots
keep full precision (the whole-pixel row's 9.4 B would take 0.08–0.25 MB/h off), plus 5–10 B per
unit for orders and queues (assumed) plus 0.2–4 KB per player for the explored map. Camera and
cursor are 160–320 B/s per human: §7's 24 B sample plus 8 B header at 5 to 10 Hz, uncompressed
([exploration](demo-recorder-exploration.md#perspective-data-and-network-budget)). Resources are
24–65 B/s per player at 1 Hz: the first eight floats (32 B) or the whole 88 B record, compressed as
the `0x28` compresses, 43 B for 58 (S16, corrected). Orders are an allowance, as no order rate has
been measured. There is no sight term: a mask is 16,800 B raw on Two Continents and modelled
changed-cell deltas reached 32–36 B/s per spread moving unit, more than the unit's own state (S14,
S15). A rate of 1 B/s is 0.0036 MB/h, and a keyframe every 30 s is 120 an hour; the table carries
every term unrounded into its sum.

| Term, 10 Hz, MB/h | 4 players, ≈500 units | 10 players, 1,500 units in total |
|---|---|---|
| Unit state, N × f × 10 × c | 500 × 0.10 × 10 × 2.38 = 1,190 B/s to 500 × 0.40 × 10 × 2.86 = 5,720 B/s: **4.28–20.59** | 1,500 × the same = 3,570 to 17,160 B/s: **12.85–61.78** |
| Events | 500 × (0.2 to 2.5) B/s = 100 to 1,250 B/s: **0.36–4.50** | 300 to 5,000 B/s: **1.08–18.00** |
| Keyframes, 120 per hour | 500 × (10.8 + 5 to 10) B + 4 × (0.2 to 4) KB = 8.7 to 26.4 KB: **1.04–3.17** | 1,500 × (10.8 + 5 to 10) B + 10 × (0.2 to 4) KB = 25.7 to 71.2 KB: **3.08–8.54** |
| Camera and cursor | 4 × (160 to 320) B/s = 640 to 1,280 B/s: **2.30–4.61** | 10 × (160 to 320) B/s = 1,600 to 3,200 B/s: **5.76–11.52** |
| Resources | 4 × (24 to 65) B/s = 96 to 260 B/s: **0.35–0.94** | 10 × (24 to 65) B/s = 240 to 650 B/s: **0.86–2.34** |
| Orders (allowance) | **0.1–2.2** | **0.4–5.4** |
| **Sum** | 4.284 + 0.36 + 1.044 + 2.304 + 0.3456 + 0.1 = **8.44** to 20.592 + 4.5 + 3.168 + 4.608 + 0.936 + 2.2 = **36.00**: about **8–36** | 12.852 + 1.08 + 3.084 + 5.76 + 0.864 + 0.4 = **24.04** to 61.776 + 18 + 8.544 + 11.52 + 2.34 + 5.4 = **107.58**: about **24–108** |

Were every alive unit as busy as §6d's all hour, unit state alone would be 29.5 MB/h
(500 × 16.4 B/s) and 88.6 MB/h (1,500), doubled at 1/16-px precision. 5 Hz saves only 38 % of unit
state, so the rate is chosen for smoothness; 1,500 units per player multiplies the unit and keyframe
terms by ten. No sample has aircraft (air payloads reach 209 bits, ground 99). With HP in the state
stream damage events can go, a third of the compressed events (706 → 464 B/s).

**Against the budgets.** The product page's budgets are 10 MB/h for a 1v1, 25 MB/h for a typical
four-player game and 100 MB/h for ten players with 1,500 units. 25 lies inside 8–36 and 100 inside
24–108: **the budgets sit inside the estimated ranges, not safely below them**, and the unit-state
term, whose *f* is assumed, is most of each top end. Only a measured real hour settles them. The one
1v1 workload estimated is the §6 trace sustained. HP changes (17.6 changed samples/s × about 3 B)
and events without damage (464 B/s) make 516.8 B/s, 1.86 MB/h; full-precision keyframes of its mean
478.5 units (478.5 × 10.8 = 5,168 B, pose rows only: the scenario issues no orders) add 0.31–1.86
every 60 to 10 s; and two players' camera, cursor and resources, 2 × (160 + 24) to
2 × (320 + 65) = 368 to 770 B/s, add 1.32–2.77. That is about **3.5–6.5 MB/h**, resources counted
twice. The explored map, stored only while the mapping option is on, adds 2 × (0.2–4) KB per
keyframe, 0.02–2.88 MB/h at 60 to 360 keyframes an hour: 3.5–9.4 MB/h with it. A moving 1v1 is not estimated: 10 MB/h is
2.8 KB/s, which the busy battle's 16.4 B/s per alive unit fills at about 170 units.

**Withdrawn.** The earlier "central" figures, 17–22 MB/h for four players and 50–64 MB/h for ten,
rest on an assumed *f* of 0.25 and on the sight term this page drops. Their recipe was never
fully stated (S19 gives only *f* = 0.25 and 30 s keyframes); *f* = 0.25, *c* = 2.64, the midpoints
of S19's and S20's own terms and their sight term's low end come within 0.5 MB/h of all four (21.5 and 63.7 MB/h at 10 Hz, 17.0 and 50.1 with the unit term cut to
62 % for 5 Hz). They are withdrawn, as are the 180 and 320 MB/h tops (their terms sum to about
168–177 and 290–355) and the sight term that capped a moving unit at 20 B/s.

**Against the packet stream** (S21). The trace's packets in §6's container (16 B framing per event,
1 s blocks) are 112,070 B per 97.424 s = 1,150 B/s, about 4.1 MB/h; with the estimate's 6 B framing,
100,302 B = 1,030 B/s, about 3.7 MB/h. The same workload's state content (1.86 MB/h) is about 0.5×
like for like: the `0x2C` floor and the damage messages drop out. The packet stream carries no
perspectives, and its round robin is already a rolling keyframe. The trace is a short, declining,
static battle; neither figure is an average hour.

**Against the renderer scenes** (S22). On §6d's frames the changed-only unit stream is 22.8 MB/h at
10 Hz against 255.0 MB/h for unit+piece and 916.5 for whole renderer packets, 11× and 40× smaller
([exploration](demo-recorder-exploration.md#wider-capture-storage-cost)); on §6b's, 5.3 against 63.9
and 300.5. §6e's 845 MB/h portable tables come from a different sample (ticks 447–744 of the
tick-1,852 run). The saving holds only if puppet mode regenerates piece poses, effects and
projectiles from the engine's own scripts, which is not yet shown.

## 5. Open experiments

Most design risk retired first, mapped to the product page's three
[feasibility experiments](demo-recorder.md#milestones-and-delivery-order): (1) the puppet host,
(2) the 1,500-unit rebuild and a puppet seat turned AI without a reload, (3) a real skirmish
recorded.

| # | Experiment | Settles | Feeds |
|---|---|---|---|
| 1 | **Puppet host**: a type-2 level with the recorded seats, 4–9 included, active and set to 3 before `0x464990`, and a viewer with no seat; commanders, defeat and ENDMSN with type-3 seats | whether puppet mode exists | (1) |
| 2 | **Motion driver**: direct round-robin-style writes against fed proxies, before the unit tick; walk scripts, footprints, sight | motion fidelity, the recorded fields | (1) |
| 3 | **Events in-process, bounded**: `0x4B0B00`, `0x49D270`, `0x49AF90`, `0x489CE0`, `0x4866D0(rec, 0)`, `0x41B8D0`, `0x48B090`, `0x47F300` | playback fidelity, §4's event term | (1), (3) |
| 4 | **1,500-unit rebuild in play**: argument-8 creates, B4's hold, a paused apply, timed against the 1 s seek and 2 s takeover | seek and takeover feasibility | (2) |
| 5 | **Seat to AI without a reload**: flip the seat, build the `+0x74` and `0x5119C0` objects, then re-create its units at their recorded slots (fresh COB, a local mover); does the AI keep, override or re-plan orders? Does an Escalation factory's one-shot `Upgrade` run in the fork? | takeover without a loading screen, or its fallback back for a decision | (2) |
| 6 | **Live rewind**: live seats flipped to 3; what their kept `+0x74` objects and local movers do (getter 75 keeps answering the recorded bit, 1 for every skirmish seat, so running scripts stay on their branch) | rewind in a live skirmish | (1), (2) |
| 7 | **Queues**: direct `0x43ADC0` appends, `0x43B0B0`/`0x419B00` by name, rally, nanoframe adoption | queue fidelity at a takeover | (2) |
| 8 | **Resources** through `+0xDC/+0xE0` and `+0x149`; residual divergence from fire spread, feature damage, meteors and grid claims | resources at a takeover; puppet drift | (2), (3) |
| 9 | **A real hour**: AI, structures, air, orders, camera, and the recorder's game-thread cost | *f*, the disk budget, the 0.5 ms per tick budget | (3) |

## 6. Corrections made to other notes

The same change corrects these statements elsewhere; the correct facts, all DIS:

- **The six-leg move list** in [smooth motion](../smooth-motion.md) (gate 4), in the description of
  `scenarios/walk-lerp.json` and in the [roadmap](../roadmap.md)'s smooth-motion entry, which said
  `0x43AFC0` replaces the main order and drops one within ±16 wu: the list collapses because the
  applier passes shift 0, on which `0x43AFC0` goes straight to the allocator
  (`0x43AFD4..0x43AFDA`), and the allocator destroys the standing orders lacking flag `0x4` unless
  the new order carries `0x40` (`0x43AE02..0x43AE59`). The ±16 wu duplicate test (`0x43B006`) runs
  only with shift 1, so it plays no part.
- **"Every simulation tick"** in [scenario format](../scenario-format.md) (phase C) and in
  `apply_players`' comment in `tagpu_scenario.c`: the economy pass runs once every 30 ticks for each
  local player, recomputes storage and clamps current to it, and carries current forward rather than
  refilling it, so it explains the 4321 → 50 reading and not the 12 → 50.
- **The launch-time storage** in [scenario format](../scenario-format.md) (phase D), said to stay
  because nothing takes it away: it lasts while the commander lives. The commander's death clears
  `+0x149` bit 0 (`0x486518`), after which the economy pass stops adding the base storage
  `+0xDC`/`+0xE0` (`0x401941`).
- **"`0x467440` runs once per sim step"** in the [B-series evidence](sim-fixes-evidence.md): it runs
  on the viewed player's 30-tick pass; the per-tick work is the sight stamps and, for the viewed
  player only (`0x465065..0x465070`), `0x466DC0`.
- **The engine map's `0x401360`**: it re-sums storage, adds base storage under `+0x149` bit 0,
  writes production, expense and the totals, and clamps current, rather than summing production.
  **Its AutoAim gate** (the COB table and the per-unit tick): the byte read is the owner player
  record's `+0x73` (`0x48ADCE`), not `unit+0x73`.
- **The LOS counters' lifetime** in the [engine map](../exe-reverse-engineering.md) (*The two
  routines that bracket a level*), in [gpu-status](../gpu-status.md) and in a comment in
  `tagpu_packet_pub.c`, which had the counters outlive the teardown until the next level's load:
  the teardown `0x491B60` frees every player's counters `+0x7C` through `0x464A00`, its one call at
  `0x491BA9`, before the map-free routine `0x483DD0` releases MAPPED (`0x491BB3`). Every in-play
  draw still falls inside both lifetimes.
- **The watchdog comment** in `tagpu_scenario.c`, which counted a pause among the states that never
  reach the apply point: a pause skips only the tick (`0x496920..0x496926`; a network game's paused
  branch `0x4968AA..0x4968B0` rejoins at `0x496969`), and every path through the frame `0x496790`
  reaches the draw call at `0x4969CD` and so `0x4969D2`. No paused apply has been measured live.

**Still stale:** the watchdog's own message in `tagpu_scenario.c`, *"apply needs a running game, not
the menus, the mission-end screen or a paused one"*, which [scenario format](../scenario-format.md)
quotes (phase C, *What the live runs corrected*). By the disassembly above a pause does not keep the
frame from the apply point, so "or a paused one" is wrong; the message is a string in code, and
settling it takes that code change and a paused apply measured live. Likewise `tools/tacli`'s
warning when `apply` is asked for resources, and the comment above its player keys, still say the
engine recomputes storage "every tick"; it is the 30-tick economy pass. Both are code text, left
for a code change.

Refuted during the research and not carried: that stock shares no radar with allies (`0x4674BA`
reads ShareRadar); that `player+0x7C` is separate from current sight (it is the true-LOS counter);
that the network frame branch keys on game type 3 (it keys on `main+0x2A44` bit 0); that dirty
payloads are never a pose (air selector 2 is one); and the size figures withdrawn in §4.
