# Demo recorder S2 — events, sounds and effects on a replay host (evidence)

## Status

**Desk research, 2026-09-30**, for the [skirmish slice](demo-recorder-skirmish-slice.md)'s S2, with the owner's decisions in that note's *Decided 2026-09-30*. S2a, the recording half, implements what this page found for the recorder and verifies it (the [slice note](demo-recorder-skirmish-slice.md)'s *S2a*); S2b builds the playback on the rest. The recorder's event sites as built are the [engine map](../exe-reverse-engineering.md)'s *The demo recorder's event sites*; where they differ from a recipe below, the map and `tagpu_recorder.c` are the source.

Read at the worktree `demo_recorder`'s
`45f01501`: the retail 3.1 exe `pristine/TotalA.exe.pristine`, image base 0x400000, read with
`i686-w64-mingw32-objdump -d -M intel` from aligned function starts. Every engine fact below
is **DISASSEMBLED** unless it is tagged **[INFERRED]**. Nothing was run, built or measured for
this document; the one measurement it leans on is experiment 3 in
`research/notes/tadr-port/demo-recorder-state-evidence.md` §1.

Terms:

- **owner**: the recording machine's live skirmish; every unit is local (type 1 or 2).
- **puppet host**: the replay level. Every seat is type 3 (`player+0x73` = 3) and the network
  bit `main+0x2A44` is clear.
- **image T**: S1's capture at 0x4954ED (B2's stub), taken before the unit tick of step T.
- **E_T**: the events the owner's step T makes after image T.
- **Addresses**: `main` = `*(0x511DE8)`; unit i = `*(main+0x14357) + i·0x118`; player k =
  `main+0x1B63 + k·0x14B`. `max` = `(*(main+0x1435B) − *(main+0x14357)) / 0x118`, the last slot,
  inclusive.

The step T tick, in order: the network pump 0x453D40 (network only), then these calls.

| Call | Pass |
|---|---|
| 0x4954ED | unit tick 0x48AD30 (B2's capture sits here) |
| 0x495513 | projectile pass 0x49B720 |
| 0x495539 | 0x420F30 |
| 0x49555F | players and AI phase 0x464F80 |
| 0x495585 | features 0x424050 |
| 0x49558A | 0x415B30 |
| 0x49558F | wind 0x490C40 (B6) |
| 0x495594 | meteor shower 0x437DE0 |
| 0x495599 | 0x41CA10 |

---------------------------------------------------------------------------------------------

## 0. The answers in brief

1. **Event table (§1).** There are seven root classes a live remote gets only from a message:
   - create;
   - death;
   - broadcast script start;
   - shot, meteors included;
   - build done;
   - state byte `+0x10E`;
   - interception `0x0E`.

   There are two more that a remote never gets at all. They are the owner's decision (§6):
   - unit replies and notices;
   - the COB starts the owner makes where damage or a transport order is applied.

   Each row names the capture, the replay call, the bounds and the receiver's writes.
   - **Record every call, nested ones included, and replay in recorded order.**
   - Nested pairs are inert by construction: a compare, an occupancy test, an alive test.
   - The exception is the pair that the puppet's order nodes would trigger (§1.2, §1.3).
2. **Damage (§2).** On a puppet host a replayed shot, meteor or death explosion is a picture
   and a sound only. The owner gate at 0x49A01B..0x49A047 stops all damage from a projectile
   whose owner is an active type-3 seat, and every one of the ten callers of the impact 0x499EB0
   passes it. Four paths still write recorded state on the host. Each must be kept off puppets:
   - the destructor's cargo-loop hit, 0x48680B → 0x489BB0 → 0x489CE0;
   - the unit tick's reaper, 0x48AFD1, for a puppet unit with `+0x110` bit 14;
   - the host's own meteor shower (0x495594);
   - fire spreading from burning features, 0x49A0C0, which calls area damage with no gate
     (S3's concern).

   In addition:
   - Impure's own hooks inside the replayed roots write SIM_STATE: B4's slot stamps, and C4's
     marks.
   - The console kills (0x4161F0) reach Send_UnitDeath and kill-all.
3. **New units (§3).**
   - **Replay path.** Replay through the create 0x485F50 with its argument 8 set to the
     recorded slot, and with the recorded `finished` and `layer`. The engine refuses an
     occupied slot or one outside the owner's block (0x486026..0x486043).
   - **Bounds.** Bound the type to [1, `main+0x1438F`) and the owner to < 10; the create bounds
     neither.
   - **Why not CreateFromNetwork.** CreateFromNetwork 0x4861D0 is the live remote's path. It is
     always unfinished, checks nothing and destroys an occupant: the wrong tool.
   - **Build progress.** `+0x104` (a float, build remaining) and HP ride in the image, and the
     nanoframe look follows `+0x104`. The model's composite frame (`[+0x9E]+0x10`, the cached
     1x bake) is cleared at a finish only by the build-done event.
4. **Sounds (§4).** The replayed receivers make every sound a live remote makes, with no help:
   - fire sounds (0x49C866);
   - bursts (0x49B87A);
   - impact and water sounds (0x499FF2, 0x499FD8);
   - death blasts (through the same projectile code);
   - "taken the lead" and the elimination message with MessageArrived (0x463DF9).

   What a remote never makes are the **unit replies**: the notice queue, only for the viewed
   player's live units.

   **Open item 1:** 0x48B090 does make sounds, but only as notices (kinds 3, 4, 14 and 15)
   played by the queue's consumer through 0x4D0640 by file name. It never reaches 0x47F0C0 or
   0x47F300 and sends no `0x13`. B8's comment and two notes say otherwise; see §7.
5. **Hooks (§5).**
   - **Free sites.** All of these are free and made of whole instructions, with no jump target
     inside:
     - the entries of 0x4866D0, 0x41B8D0, 0x48B090, 0x456190, 0x456200, 0x456290, 0x47F780
       and 0x47F850;
     - the create's success exit 0x4861B3;
     - the five fire-function network tests;
     - the reaper call 0x48AFD1.
   - **Occupied sites.** In the raised build the five `0x0D` sends and the two `0x0E` sends are
     already Impure's (`wpn_tx_fired`, `wpn_tx_intercept_send`). The recorder takes those
     packets from a callout there, never through a second site: `lim_overlap` would refuse it.
   - **Broadcaster entries.** A hook at the entry of 0x456200 also catches Impure's
     `my_AutoAim`, which calls 0x456200 directly.

---------------------------------------------------------------------------------------------

## 1. The event table

### 1.0 Rules for every row

- **Ordering.** Every event carries the step T and a sequence number in engine order. As built
  (S2b), the host's step at 0x4954ED runs its own unit tick 0x48AD30 first, and then the replay:
  E_T in recorded order, then image T+1. E_T is what the recording machine's unit tick T made
  from image T, which the host holds; image T+1 is the world that unit tick and the rest of
  iteration T left. The rest of the host's iteration then reads what the recording machine's
  read, and the players phase re-stamps sight from the unit tick's positions. Image T before the
  host's unit tick, the desk order, left the stamps `+0x7A`/`+0x7C`/`+0xF8` of 15–20 of 1,413
  battle units one tick behind at every dump [MEASURED 2026-09-30]. What the rest of the
  iteration keeps a clock for, it keeps on the host: the players phase's 30-tick deadline
  ([the host note](demo-recorder-s2-host.md) §2.3). Section 1.9 covers the one refinement this
  needs: the victim's record for deaths.
- **IDs.** Units are named by recorder IDs (from 1, never the engine's index: commit
  `84b5fae7`).
  - Playback maps each ID to a slot before any call.
  - It bounds the slot to [1, `max`] and, for a create, to the owner's block.
  - Every other value read from the recording is data, bounded before it indexes anything.
- **Writes.** A receiver's writes to recorded state are the recorder's own writes, because the
  recorder called the receiver.
  - They must equal what the owner's call wrote.
  - Any other change the host makes to recorded state between two images is a second writer.
  - §1.10 turns this into a check that names such a writer instead of masking it.
- **Nested roots.** Recording every call, nested ones included, needs no depth logic. It also
  avoids depending on puppet state to reproduce what a nested call did.
  - Each nested pair below is inert on its second run by an engine test.
  - The one pair that is not (StopBuilding from the order-node teardown) is closed by a rule on
    the puppet's order nodes (§1.3).
  - As built (S2b), a root recorded at its entry is played before what it nests, which follows it
    in the recording and is played as its own event; the puppet gates keep the replayed root's
    own nested COB writes and replies inert (`tagpu_puppet.c`, *The replay's own calls*). The
    create is recorded at its exit, after what it nests (the state byte, the Create script's
    first slice with its SETs, their replies), so those find no unit when played and are counted
    as not played; the replayed create makes them again, with the gates open around it.

### 1.1 Create — root `0x485F50`

- **Signature:** `0x485F50(owner u8, type u16, pos (3 dwords by value), finished, layer, slot u16)`,
  stdcall `ret 0x20`. The engine map writes it as `0x485F50(seat, type, pos, finished, layer,
  slot)`.
- **Capture:** at the success exit **0x4861B3** (`8B C6 5F 5E 5D`, `mov eax,esi; pop edi; pop
  esi; pop ebp`).
  - `esi` is the unit made. Its slot gives its ID.
  - The arguments sit on the stack, past the five registers the function pushed.
  - The exit sees the slot actually taken. B4's `hit_take` at 0x486036 may move a first-free
    create (argument 8 = 0) to a later free slot; the entry would not see that.
  - Record: owner, type, pos, finished, `layer & 3` (the create keeps only bits 0–1 of `+0x110`,
    0x4860F1..0x486103), and the ID.
  - The failure exit is 0x4861BD. Every NULL return comes before any send, per B8's note at
    `tagpu_patches.c` ~8310.
- **Replay:** `0x485F50(owner, type, pos, finished, layer, slot)` with the mapped slot.
  - A nonzero argument 8 skips first-free and takes exactly that slot (0x486002..0x486030).
  - An occupied slot fails cleanly (0x486036 `cmp w [esi+0xA6],0`, then 0x486040/0x486043
    `jne 0x4861BD`). A replayed create into a slot whose previous occupant has not been replayed
    dead returns NULL and writes nothing, which the harness can name.
  - Under B4 the stub tests type 0 first, and `hit_take` does not redirect a nonzero argument 8
    (`tagpu_patches.c` ~6477).
- **Bounds the create lacks:**
  - The **type**: `test si,si` (0x485F6F) and `je 0x4861BD` (0x485F7C) refuse 0. Nothing bounds it above before
    `def = *(main+0x1439B) + type·0x249` (0x485F82..0x485F9A). Bound it to [1, `main+0x1438F`).
  - The **owner** byte: it scales `main+0x1B63 + owner·0x14B + 0x67/0x6B` (0x485F5C..0x485F78,
    0x485FBB, 0x485FEA) unbounded. Bound it to < 10.
  - The per-type limit `def+0x15A` (0x485FB0..0x485FE4) counts the owner's block on the host
    exactly as on the owner.
- **What the create writes on the host.** All of it is the owner's create's own writes, so image
  T+1 carries the same.
  - The init 0x485A40 writes the whole record: `+0xA6`, `+0x10E` = 0, and the rest.
  - COB and model 0x485D40 (the `Create` script, 0x485DE6).
  - Weapons 0x49E070, with the weapons module's side rows.
  - SetSpeed 0x437840.
  - The mover (`def+0x22F` = 1 → 0x43DC00, `+0x66` from `def+0x210`).
  - `+0x110` layer bits, then 0x48A870.
  - The bucket restamp 0x47CC30.
  - The send 0x456050.
  - Owner `+0x144`++ and `+0x140`++ (0x486187, 0x48619C).
  - 0x482AC0 and 0x490580 [INFERRED: sight stamp, objective bookkeeping].
  - With `finished` set:
    - a non-mover def (`def+0x22F` = 0) sends a self-`0x12`, `0x4560C0(u,u)` (0x48612A);
    - `activatewhenbuilt` (`def+0x241` bit 18) → `0x48B090(1,1)` (0x486148);
    - `isfeature` (bit 24) → `+0xF5` = 7 and `+0x110` bit 14 (0x486161..0x486171).
  - Impure: B4's `hit_take` writes `s_hit`'s stamp and local time for the slot. `s_hit` is
    `SIM_STATE` (`tagpu_patches.c:6316`), so this is a recorded table: see §2.5.
- **Nested roots recorded with it:**
  - `0x48B090(1,1)`: inert on replay by 0x48B090's own compare (0x48B0D6..0x48B0DE; an
    unchanged byte returns at once).
  - The self-`0x12` is a send, not a call of 0x41B8D0. On the owner nothing calls 0x41B8D0 for
    it, so nothing is recorded.

### 1.2 Death — root `0x4866D0(rec, mode)`

- **The record read by the destructor.** Offsets and reading sites:

  | Offset | Field | Read at |
  |---|---|---|
  | +1 | unit (u16) | 0x4866E0 |
  | +3 | killer netid (u32) | 0x48677E → `0x44FE40` → `+0xF4` |
  | +7 | killer unit (u16) | 0x486753 → `+0xF0` |
  | +9 | severity (signed byte) | 0x486855, 0x486D28 |
  | +0xA | low nibble: corpse level; high nibble: death kind | 0x4867DA, 0x486880, 0x486D42, 0x486D55 |

  - `0x44FE40` turns a netid into a player index, 10 when there is none (0x44FE40..0x44FEA0).
  - `+0xA`'s nibbles:
    - `0x30` = self-destruct: the explosion takes `SelfDestructAs` (0x486D42..0x486D4B).
    - `0x70` flags the corpse call's third argument (0x486D5C..0x486D66).
  - **Mode.** The owner's Send_UnitDeath passes mode **1** (`push 1` at 0x486676, call
    0x486679). The `0x0C` case passes mode **0** (0x455420, call 0x455423).
  - The mode is read once, at 0x48684A: only mode 0 starts `Killed` (0x486877). The owner has
    already run Killed as a query in Send_UnitDeath (0x4865C3), which is where the severity and
    the corpse nibble come from.
  - So **replay with mode 0**, as a live remote does. The death animation then plays on the
    puppet.
- **Capture:** at the entry **0x4866D0** (`83 EC 68` `sub esp,0x68`, then `8B 15 E8 1D 51 00`,
  9 bytes of whole instructions).
  - Record the unit ID and the killer ID.
  - Record the **killer's player index**, not the netid: player `+4` is `drop` in
    `replay-layout.tsv` ("the recording carries it as the player's key"), so a netid means
    nothing on the host.
  - Record the severity byte and the `+0xA` byte.
  - Record **C4's carried decision** for this call. It lives in `s_txCarried[idx]` while
    `tx_wrap` runs the destructor, so the capture is taken inside that call. C4's pick at
    0x49B017 reads it to choose `TransportedExplodeAs` over `ExplodeAs`.
  - Record **the victim's recorded fields at that moment** (§1.9).
- **Replay:**
  1. Write the victim's recorded fields, as the image application does for one unit.
  2. Arm C4's flag for the slot, as `tx_rx_death` does.
  3. Call `0x4866D0(rec', 0)`, where `rec'` has the mapped IDs and the host's `+4` for the
     killer's player.
  4. Clear the flag.

  The destructor must be entered the way `tx_wrap` enters it, or the carried pick draws the
  wrong blast. Experiment 3 called 0x4866D0 bare.
- **Duplicates are harmless.** A second recorded death of one slot, or the owner's call on an
  already-dead unit, replays inert because of the alive test at 0x486706. So the capture may be
  unconditional.
- **Bounds:**
  - B3's `wire_s0c` at 0x4866E5 bounds the unit index for every destructor call: index 0 and
    anything past `max` are dropped to 0x486E59.
  - `wire_s0ckill` at 0x486753 turns a killer past the array into NULL.
  - The alive test at 0x486706 makes a second death of a slot inert.
  - The recorder still maps and bounds its IDs itself.
  - The killer's player index must be < 10 or "none". `0x44FE40` gives 10 for none, and
    0x4948B0 writes the byte table `0x51E810[+0xF4]` with only a `jl` test. That is stock's
    behaviour for every kill with no killing player.
- **What the destructor writes on the host.** Everything below the victim-level lines is also
  what a live remote's destructor writes.
  - Victim: `+0xF0` (0x486778), `+0xF4` (0x486787).
  - Its orders: `0x439EB0(unit,1)` at 0x4867A1 → the order-node teardown 0x43A1F0 for each
    node. See the rule below.
  - Queue entries 0x47F8C0; squads `0x480250(unit,−1)`; pending bursts 0x49C880.
  - The detach (0x4867CB) and the **cargo loop**, `0x489BB0(passenger, …, 30000, kind 6 or 3)`
    at 0x48680B, then a detach at 0x48681D. §2.2: must be kept off puppets.
  - 0x47CBD0 [INFERRED: leaves the sort bucket; the layout's `bucket` row names it]; 0x482090
    under LosType bit 1 [INFERRED: sight].
  - `Killed` through 0x4B0A70 at 0x486877 when mode 0 and severity > 0.
  - Statistics, all in recorded player and unit fields:

    | Write | Site |
    |---|---|
    | the victim owner's `+0xFE`++ | 0x4868C1, 0x486A33 |
    | the killer player's `+0xFC`++ | 0x486906 |
    | commander kills `+0x104`++ | 0x486981 |
    | commander losses `+0x106`++ | 0x486996, 0x486A8C |
    | the killer unit's `+0xB8`++ | 0x4869CA |
    | every player's rank `+0x148` | 0x486AA0..0x486C69, with "taken the lead" through `0x463CA0(buf,2,0,0xA)` |
    | a kind-5 credit to the killer's `+0xD4` | 0x486C9E..0x486D1E |

  - 0x4948B0 (two byte tables, set to 30 and decayed by 0x4948E0) [INFERRED: player-list
    flashes, UI].
  - The **death blast** `0x49B000(unit, selfd)` at 0x486D50, when severity > 0 and `+0x104` is
    0.0 (0x486D24..0x486D40).
  - The **corpse** `0x486360(unit, level, flag)` at 0x486D6F, when `+0xA & 0xF` > 0:
    `SpawnFeatureOnMap 0x423C50` at 0x4863F9 or 0x486434, and smoke 0x472630 at 0x48644B. It
    has no owner gate, so it runs on every machine. See §2.2 and §6 D6.
  - FreeUnitOrders 0x489740; COB `[vt+0x50](1)`; model 0x45AAA0; mover.
  - `+0xA6` = 0 (0x486DC7); `+0x110` bits 28, 4 and 5 cleared; `+0x92` = the default def;
    owner `+0x144`−− (0x486DFC).
  - At 0 units, the skirmish elimination 0x47BD70 runs. It writes nothing recorded: a phrase
    picked with the CRT `rand` % 3, then `0x463CA0(text,4,0,player+0x146)`, which plays
    MessageArrived.
  - Impure: B4's `hit_freed` at 0x486DC1 writes `s_hit.freed[idx]`, which is SIM_STATE.
- **Nested roots:**
  - **StopBuilding.** When a teardown node has `+0x42` bit 0x400000, 0x43A1F0 starts
    StopBuilding (0x4B0B00 at 0x43A251) and broadcasts it (0x456190 at 0x43A258). That
    broadcast is recorded. For it not to run twice on the host, **the puppet's copy of an order
    node must not carry `+0x42` bit 0x400000**.
  - **The order-type callback.** When `node+6` bit 0x02 is set, 0x43A1F0 also calls the order
    type's own callback `[0x512344 + type·0x14 + 4](unit, node, 2)` (0x43A1FE..0x43A21A). That is
    owner code on a puppet, so **nor may it carry `+6` bit 0x02**.

  Both are rules for the orders track: node vtable 0x4FD2C8 = {0x438870, 0x43A1E0}.

### 1.3 Broadcast script starts — roots `0x456190`, `0x456200`, `0x456290`

- **Signatures:**
  - `0x456190(unit, idx)`, entry `A1 E8 1D 51 00`, network test 0x456198.
  - `0x456200(unit, name, argc, a0..a3)`, `ret 0x1C`, entry `8B 44 24 08 83 EC 18`.
  - `0x456290(unit, idx, argc, a0..a3)`, entry `A1 E8 1D 51 00`, network test 0x456298.
  - The message is `{0x10, u16 unit, i16 script, u8 argc, 4 × i32}`, 22 bytes.
  - 0x456110 exists but has no caller.
- **Callers:**
  - 0x456190 has two: StopBuilding in 0x4385F0 (0x438632), and the order-node teardown
    0x43A1F0 (0x43A258).
  - 0x456200 has two: 0x411403 in the order handler 0x410830, and AutoAim 0x49E3A6. Impure
    replaces AutoAim with `my_AutoAim`, which calls `E_SendStart` = 0x456200 itself
    (`tagpu_weapons.c:950, 962`).
  - 0x456290 has one: StartBuilding 0x438590 (0x4385D7).
- **Capture:** at each entry, before its network test.
  - **Record the script index.** For 0x456200, resolve the name with 0x4B07C0 on the owner's
    COB, as the broadcaster itself does. The host's COB of the same type has the same table.
  - Record the unit ID, argc and a0..a3.
- **Replay:** `[unit+0x9A]->0x4B0B00(idx, 0, 0, argc, a0, a1, a2, a3)`, the stock `0x10` case's
  call (0x455528..0x455551), made by the recorder itself as experiment 3 did.
  - Do not route it through the dispatcher case. That case scales the unit u16 with no bound,
    and a 0 there is a NULL whose `+0x110` it reads (0x4554F9..0x455518).
- **Bounds:**
  - The unit must be alive and `+0x9A` non-NULL. 0x4B08C0 reads `[cob+8]` with no NULL test
    (0x4B08CA).
  - The script index is bounded by the engine itself: `idx ≥ 0` and `idx < [[cob+8]+4]`
    (0x4B08C5..0x4B08D0).
  - **argc ≤ 4.**
    - 0x4B0B00 pushes the four arguments and then sets the thread's stack pointer to argc − 1
      (0x4B0B76..0x4B0B7B) with no bound.
    - The thread record is 0xA4 bytes with its stack at +0x40, so an argc from 26 to 255 points
      the COB stack past the thread.
    - The message carries four arguments.
  - A start fails silently when all eight threads of the unit are busy (0x4B08D7..0x4B08E8).
    That is a capacity divergence, not a fault.
- **Writes:** the unit's COB thread table only, which is not a recorded record. COB's own engine
  writes are inert in puppet mode (CLAUDE.md).
- **Double start: none.** StartBuilding and StopBuilding also run from the state setter
  0x48B090 (bit 3, 0x48B14E and 0x48B169). That call is a direct `0x4B0940`, never a broadcast.
  - The callers of `0x48B090(8,…)` are all in 0x401360 (0x4029B0, 0x402B1B).
  - The callers of 0x438590 and 0x4385F0 are all in 0x403180, 0x410830 and 0x414350.
  - So a recorded `0x10` and a recorded `+0x10E` change are never two records of one start.
    The live remote does the same through the `0x11` and `0x10` receivers.

### 1.4 Shots — the `0x0D` the fire functions build

- **Roots.** These are the functions a weapon's `w+0x60` points at, so they have no direct
  callers:

  | Function | Kind |
  |---|---|
  | 0x49D580 | turret, flag bit 19 |
  | 0x49D9C0 | flag bits 0 or 20 |
  | 0x49DB70 | vertical launch, bit 4 |
  | 0x49DD60 | bit 8 |
  | 0x49DF10 | the meteor spawn, called only by the shower at 0x438044 |

  Each builds the 36-byte `0x0D` only under the network bit:

  | Network test | Send |
  |---|---|
  | 0x49D7A0 | 0x49D859 |
  | 0x49DA92 | 0x49DB4D |
  | 0x49DC64 | 0x49DD27 |
  | 0x49DE25 | 0x49DEEE |
  | 0x49DFAF | 0x49DFF6 |

  - Each test is `F6 8x 44 2A 00 00 01` (`test b [reg+0x2A44],1`) and a `je`: `0F 84 rel32`, or
    `74 43` for the meteor.
  - Between the test and the send, the five blocks write **only the stack packet**. They call
    nothing but the send and, for the meteor, 0x44FDB0, which reads the first active local
    player's netid.
  - So releasing the test while recording changes nothing but the packet's existence. This was
    checked for every non-stack write and call in the five blocks of the retail exe. Impure's
    image adds the extra weapons' slot-index splice to four of them, which writes no simulation
    state (the engine map, *The demo recorder's event sites*).
- **Capture:** release the five tests while recording. The recorder takes the packet at the send.
  - In the raised build the five `call 0x451DF0` are `wpn_tx_fired`'s sites
    (`tagpu_patches.c:2706–2745`). It writes the 12-bit weapon ID (`+0x19` and the high nibble
    of `+0x23`) and clears the meteor's stale `+0x1A..+0x23`.
  - The recorder's capture is a callout from `wpn_tx_fired`, which returns 0 to skip the send
    when the network bit is clear (`wpn_send_or_skip`).
  - The stock build (`make LIMITS=stock`) leaves the sends stock. The recorder then needs those
    five sites itself; one owner per site in both builds is simplest.
  - Record the packet with `+0x1F`/`+0x21` as IDs.
  - Record `+0x1A` masked to bit 0. Only bit 0 (weapon `+0x111` bit 30) is set; the other bits
    are uninitialised stack (0x49D836..0x49D84B).
- **Packet layout:**

  | Offset | Field |
  |---|---|
  | +0 | 0x0D |
  | +1 | start (3 dwords): the build's, except `0x49D9C0`'s, which is the shooter's centre `+0x6A`; the recorder records that sender's build start in its place ([engine map](../exe-reverse-engineering.md#what-a-shots-packet-says-of-its-start-0x49d9c0-0x49d270-disassembled-measured-2026-10-09)) |
  | +0xD | target point (3 dwords) |
  | +0x19 | weapon ID, low byte |
  | +0x1A | bit 0 = intercepting weapon |
  | +0x1B, +0x1D | the slot's angles (`slot+0x16`, `+0x18`) |
  | +0x1F | target `+0xA8` |
  | +0x21 | shooter `+0xA8` |
  | +0x23 | low nibble: slot; high nibble: ID bits 8–11 |

  The turret function's spread comes from the sim RNG (0x49D727, 0x49D733). The packet carries
  the applied angles, so a host cannot recompute a shot; it must replay the packet.
- **Replay:** `0x49D270(0, msg')`, `ret 8`.
  - **Meteor branch.** A meteor (weapon flag bit 5) goes to 0x49C740 at 0x49D307. That is B7's
    site: the stone is marked `PJ_RECEIVED` and does no damage.
  - **Unit branch.** Otherwise the shooter (u16 `+0x21`) must be alive, and the builder is
    chosen by flag:

    | Flag | Builder |
    |---|---|
    | bit 1 | 0x49CDE0 |
    | bit 4 | 0x49CC20 |
    | bits 0 or 20 | 0x49C9C0 |
    | bit 8 | 0x49C740 direct |

  - The builders start `Fire<slot>` (0x49CB94, 0x49CD4F, 0x49CF73) and `RockUnit` (0x49CBEB,
    0x49CDA6, 0x49CFCA), spawn particles (0x4729D0), and end in the projectile init 0x49C740.
- **Bounds.** Impure already has them, in both builds, at `wpn_rx_fired` (the 0x49D280 site,
  `tagpu_patches.c:2296`).
  - The weapon ID must be < `TAGPU_LIM_WEAPONS`.
  - The shooter and target must be ≤ `max`.
  - The slot is bounded by the extra-weapons splice at 0x49D364.
  - B3's diverged-shooter drop: the shooter's slot weapon must be `&Weapons[id]`.

  The recorder only maps IDs to slots.
- **Writes on the host:**
  - The shooter's slot angles: `slot+0x18` ← `+0x1D` (0x49D37C) and `slot+0x16` ← `+0x1B`
    (0x49D384). These are the owner's values at the send.
  - A bit-4 weapon's `slot+0x8` = 0 (0x49CCA2).
  - In 0x49C740:
    - the attacker's `+0xB0` = GameTime + 600 (0x49C84B). This equals the owner's only if the
      host's GameTime equals the recording's.
    - the camera follow `main+0x142F7` (UI);
    - with no attacker, the projectile's `+0x66` = 10.
  - A new projectile in the pool (§6 D6).
- **Nested:** 0x49C740 runs `Query<w>` through Impure's 0x43E1E0 hook. That is a COB query,
  and it writes nothing recorded.

### 1.5 Build done — root `0x41B8D0(builder, built)`, `ret 8`

- **Callers:**
  - 0x401360: 0x4026EE, 0x402B25. It is the AI or players phase: its one caller is 0x46555A in
    0x464F80.
  - The builder's step 0x41BA60 (0x41BCBF).
  - The `0x12` receiver (0x4555F1, 0x455610).
- **Capture:** at the entry (`56 57 8B 7C 24 0C`: `push esi; push edi; mov edi,[esp+0xC]`, 6
  bytes). Record the builder ID and the built ID.
- **Replay:** `0x41B8D0(builder, built)`.
  - It tests on its own that the builder is alive and has a build list (`def+0x156`), and that
    the built unit is alive (0x41B8D6..0x41B917).
  - Bound the IDs first. The recorder calls it directly, so `kill_s12`, the wire's bound at
    0x4555BA, does not apply.
- **Writes, on every machine:**
  - `[built+0x9E]+0x10` = 0, the model's composite frame, the cached 1x bake (0x41B923). It is
    **not** a record byte, so this event is what clears it at a finish on the host.
  - `+0x110` bit 13 and `+0x104` = 0.0 (0x41B930..0x41B93D).
  - `activatewhenbuilt` → `0x48B090(1,1)` (0x41B9BF).
  - `isfeature` → `+0xF5` = 7 and bit 14 (0x41B9F8..0x41BA08).
- **Owner-only parts (types 1 and 2), which do not run on a type-3 unit:**
  - the build GUI when `+0x110` bit 29 is set;
  - else, when carried, the detach `0x48AAC0(built,0,−1,1)` (0x41B9A1), a `0x0A` the carry track
    records as its own event;
  - the `0x12` send 0x4560C0 (0x41BA26).
  - UI only: 0x4199B0 when the builder is the GUI's unit (`main+0x37E9C`), and the flag
    `main+0x37EBE`.
- **Nested:** `0x48B090(1,1)` is inert on its second run (its compare).

### 1.6 State byte `+0x10E` — root `0x48B090(mask, value)`, `ret 8`, `ecx` = unit

- In play, `+0x10E` is written only here (0x48B0D8), by the create's init 0x485A40 (0x485B7F),
  and by the saved-game restore 0x487080 (0x4872D9). 0x42E788 is a weapon def's
  `soundtrigger` (the TDF parser 0x42E440), not a unit field.
- **Capture:** the new byte, with the unit ID.
  - At the entry (`51 8B 44 24 0C`, 5 bytes), compute it the way 0x48B090 does, or read it
    after the store 0x48B0D8.
  - Recording the byte (the `0x11` form) rather than (mask, value) makes a replay idempotent and
    independent of the order it runs in relative to the image.
- **Replay:** the `0x11` receiver's recipe: `0x48B090(byte, 1)` then `0x48B090(~byte, 0)`
  (0x4555A1, 0x4555B0), with the unit alive (0x455585).
- **Writes and effects, on every machine:**
  - `+0x10E`.
  - Bit 0: Activate (0x48B106) and notice kind 3; Deactivate (0x48B12B) and kind 4.
  - Bit 3: StartBuilding (0x48B14E) and StopBuilding (0x48B169), by name through 0x4B0940.
  - Bit 2 set: kind 14 and **event 0x10000 to every listener on `+0xA2`** (0x48B17D..0x48B19C).
    An order node's handler 0x438870 ORs it into the node's `+0x4E`: a second writer if S2
    records `+0x4E`.
  - Bit 2 cleared: kind 15.
  - Then 0x41C110, a GUI flag for the local player's selected unit.
  - The `0x11` send only for an owner of type 1 or 2 (0x48B1B5..0x48B1F3). It has no network
    test, so a skirmish calls the send too (experiment 3 counted 143).
- **Nested:** the notices (§1.8).

### 1.7 Interceptions — the two `0x0E` of area damage `0x49A120`

- **Where it happens.** For a weapon with `+0x111` bit 30, area damage walks the projectile pool
  (`main+0x141F3` count, `main+0x141F7` array) from 0x49A664.
  - It detonates each projectile in range, `0x499EB0(proj,0)` at 0x49A764.
  - It sends **two** `0x0E`, each `{0x0E, target point (3 dwords), weapon ID byte}`: one for the
    intercepted projectile (0x49A7A8) and one for the interceptor (0x49A7E9).
  - There is no network test.
- **Why it must be recorded.** 0x49A120 sits behind the impact's owner gate. On the host a
  replayed interceptor never intercepts, and **the intercepted shot would land**.
- **Capture:**
  - In the raised build, both sends are `wpn_tx_intercept_send`'s sites, and weapons ≥ 256 go
    out as the `05 00 49` companion from `wpn_tx_intercept`.
  - A callout there takes the target point and the 12-bit ID.
- **Replay:** match as 0x49AF90 does (target point, then weapon) and detonate with
  `0x499EB0(proj,0)`. For IDs ≥ 256, match as the companion's handler does.
  - It matches by value, so there is no index to bound.
- **Writes:** the pool. The impact makes a picture and sound only (§2.1).

### 1.8 Notices (unit replies) — roots `0x47F780(unit, kind, text)` and `0x47F850` (decision D1, §6)

- `0x47F780` gates on:
  - `unit+0xFF` == the **viewed** player `main+0x2A43`;
  - alive (bit 28);
  - bit 14 clear.

  It then queues through 0x47FAD0. It has 82 callers: order handlers, 0x48B090, the order-node
  helper 0x438880 (kind 5), and selection (kind 1 at 0x48C668, 0x48C8B9, 0x48C997).
- `0x47F850` adds an on-screen test first (0x48BCB0 [INFERRED: "is on screen"]). Its one caller
  is Under Attack in 0x406F80 (0x4071D8): the hit reaction, when the victim's attacker is
  another player, or the hit is kind 1.
- **The kind table** at 0x5086D8 has **24 entries**, of 24 bytes:
  `{kind, priority, interval ×30 ticks, sound name, default text, next time}`.

  | Kind | Name | Kind | Name |
  |---|---|---|---|
  | 0 | none | 12 | load |
  | 1 | select | 13 | unload |
  | 2 | underattack "Under Attack" | 14 | cloak "Cloaked" |
  | 3 | activate | 15 | uncloak "Visible" |
  | 4 | deactivate | 16 | capture |
  | 5 | ok | 17 | count5 "five" |
  | 6 | arrived | 18 | count4 "four" |
  | 7 | cant "Cannot Comply" | 19 | count3 "three" |
  | 8 | unitcomplete "Nanolathe Complete" | 20 | count2 "two" |
  | 9 | build | 21 | count1 "one" |
  | 10 | repair | 22 | count0 "zero" |
  | 11 | working | 23 | canceldestruct "Self destruct terminated" |

  - Stock passes kind 16 (0x4046CF, after a capture) and kind 23 (0x4020AF), and one kind from
    a table (0x4020ED).
  - Nothing bounds the kind: 0x47FAD0 reads `0x5086EC + 24·kind`. **A replayed kind must be
    ≤ 23.**
- **Capture:** at both entries, before the viewer gate, for every player's units. Record the
  unit ID and kind, and the **text string itself** (not its pointer; most callers pass 0 for
  the default).
- **Replay:** through the same function, so the engine's own gate picks the watched seat's
  units.
  - The engine's own notice calls on the host are then duplicates. They come from 0x48B090's
    kinds 3/4/14/15, and from Under Attack through the cargo loop's hit. **Make them inert in
    puppet mode.**
  - 0x47FAD0's rule against queueing a kind already queued is timing and does not count.
- **Writes:** the queue `[0x51E68C]` and the table's next times. Both are UI; nothing in the
  simulation reads them (`data-keys-evidence.md:734`).

### 1.8b The script slice — `RK_EV_SLICE` (format 20)

Not a root: the unit tick's `DoScriptsNow(1)` (`0x48ADEB`) placed among the step's events. Written
just before the slice of a unit whose COB had an engine start (`0x4B0B00`, `0x4B0940`) since the
capture point, `{u32 ID}`, outside any bracket; a playing gives that unit's slice at the marker
instead of in the host's unit tick, so a start made before the slice -- AutoAim's, a create's
Create in a later slot -- takes its first slice where it did on the recording machine
([skirmish slice](demo-recorder-skirmish-slice.md#the-script-slice-format-20)).

### 1.8c The owner's COB queries — `RK_EV_QUERY` (format 20; transport and pad, format 23)

Not a root: a COB query `0x4B0BC0` only an order handler makes, so only the machine of the
handler's unit's owner -- `QueryBuildInfo` at `0x4027FB` (BuildingBuild), `QueryNanoPiece` at
`0x43E427` (inside `0x43E400`, from fifteen handlers), and since format 23 `QueryTransport` at
`0x4113B1` (VTOL_Pickup) and `QueryLandingPad` at `0x411A35`, `0x411AA6`, `0x411BF5` and
`0x411CF1` (VTOL_Landing). `{u32 ID, u8 the query, u8 0 x3}`, outside any bracket, written where
the engine makes the query, in the handler's unit's turn after that unit's slice. The queried unit
is the handler's, or a pad -- SelfRepair's repairing pad (`0x402525..0x40252E`), VTOL_Landing's
landing pad (the order's `+0x16`) -- whose own slice can come later in the tick and which can be
another player's; the query's COB is noted in the slice set so that slice is marked. A playing
makes the same query there with the engine's arguments
([engine map](../exe-reverse-engineering.md#the-owners-cob-queries-disassembled-2026-10-12)).

### 1.8d A yard's change — `RK_EV_YARD` (format 20)

Not a root: YARD_OPEN `0x47DAC0`'s write of `+0x10F` at `0x47DAF1`, past its refusal `0x47D970`.
`{u32 ID, u8 the yard (1 open, 0 closed), u8 0 x3}`, in a bracket or not, written wherever the COB
setter ran it, skirmish recordings only. A lab's script slice can open its yard and its build
handler make the nanoframe in it later in the same unit tick, so a playing applies the yard of a
live unit at its place in the events, before the create that follows it. A yard of a unit not live
there is its own create's Create slice, recorded before the create's exit: the replayed create
makes it again and the playing checks it. Anything else is malformed (THE YARD RULE,
`tagpu_recfmt.h`; [engine map](../exe-reverse-engineering.md), *An open yard gives nothing
back*).

### 1.8e The contest order — `RK_EV_TURN`, `RK_EV_LINK` and `RK_EV_PLACE` (format 26)

Not roots: what a playing needs to make a step's footprint and chain calls in the recording
machine's order (`tagpu_recfmt.h`, *THE CONTEST ORDER*; [engine map](../exe-reverse-engineering.md),
*The order of a step's footprint and chain calls*). Skirmish recordings only, outside any bracket.

- **`RK_EV_TURN {u16 key, u16 0}`**: the turn of the unit tick -- the slot whose turn it was, or
  `0xFFFF` after the tick -- that the next top-level footprint event (a create, a death, a live
  unit's yard, a link, a move) was made in, or the slot after the turn's when it was made after
  that turn's unit relinked (its walk class's run-now COB pass, its water state's start, its
  death), written just before that event and only when its key differs
  from the one a reader infers: a death's or a yard's own slot, or the step's last key, whichever
  is later, and a create's or a link's the step's last key. The keys never go back within a step,
  and a key stands before a footprint event or not at all; the writer and both readers refuse
  anything else. The recorder takes the turn at the slice stub (`tagpu_patches.c`, `unit_slice`),
  which every turn of the tick passes, and the phase after the units sets it to `0xFFFF`.
  The slot after the turn's is MEASURED on two classes: a unit's death after its own relink
  (`slice-walkers`, 2 events) and a transport's attach in its walk class's StartMoving pass
  after its relink (`transport-turn`, 6 links keyed their carrier's slot + 1, 2026-10-24); both
  groups' playings are EQUAL. For the attach the order is not observable: it commutes with
  the carrier's relink (*the completeness note*'s 2026-10-23 row).
- **`RK_EV_LINK {u32 child ID, u32 parent ID or 0, u8 the piece, u8 the layer, u16 0}`**: an attach
  or a detach `0x48AB70` that acts, written at its place from the observer at its entry. A
  playing makes it through `0x48AB70` there (`tagpu_replay.c`, `ev_link`), and the carry links'
  reconciliation after the step then finds nothing to do unless the host refused one. The
  destructor's own detaches are not written: the playing's destructor makes them again.
- **`RK_EV_PLACE {u32 ID, i32 x, altitude, z before, i32 x, altitude, z after, u8 the layer
  before, u8 the layer after, u16 0}`**: one unit Teleport's handler `0x406AA0` moves, written at
  its set-position call `0x406BC0` (a stub that then makes the call). The handler makes the flare
  `0x471FD0` from the first point to the second just before it; a playing makes both at the
  record's place (`tagpu_replay.c`, `ev_place`), the flare counted as the handler's call. A puppet
  runs no order handler, so these are its only Teleport calls.

The recorder counts every footprint or attach call no key can order -- a call from a site its table
does not hold, a turn out of slot order, a Teleport move of a unit that is not alive, an attach
inside any bracket but a death -- and logs the count at the recording's end (`recorder: N footprint calls a
playing cannot put in their order`); the replay harness fails a recording whose count is not 0.

### 1.9 The unit state a death must carry

On the owner, the destructor runs inside the unit tick. The reaper (0x48AFB9..0x48AFD1) comes
after that unit's own movement step (0x48AFA3..0x48AFB0). So the victim's position, cell and
turn at death are image T's plus one step.

The host replays E_T right after image T, which makes three results come out one step early:
- the corpse's cell `+0x76`/`+0x78` (0x4863B7..0x4863C1);
- its height and turn `+0x6A`/`+0x64` (0x4863D3, 0x4863F1);
- the blast's position.

A cell crossing then puts a **feature** (the wreck) in a different cell from the owner's.

- **Fix by construction:** the death event carries the victim's value fields as of the
  destructor's entry. The replay writes them before the call, as the image application writes a
  record.
  - That is about 0x118 bytes per death, and roughly 100 a minute in a battle.
  - The destructor then sees exactly the owner's inputs.
- No other row needs this:
  - a create's arguments carry its position;
  - a shot carries its own points and angles;
  - build done and the state byte read only flags and defs;
  - an interception matches by value.

### 1.10 Naming a second writer instead of masking it

At each capture point on the host, before image T+1 is applied:

- compare the host's recorded bytes with image T plus the **write-sets** of E_T's receivers (the
  *Writes* lines above);
- snapshot the records before E_T is replayed and diff them after;
- a byte changed outside a replayed receiver's call is a second writer, named by its field;
- a byte a receiver changed must equal image T+1's byte, except where the owner changed it again
  later in the same step.

Such a later change is benign; the shot's slot angles are one candidate. A reported mismatch is
reviewed, never loosened silently: that is the owner's call under CLAUDE.md.

---------------------------------------------------------------------------------------------

## 2. The damage path on a puppet host

### 2.1 A replayed shot, from receiver to impact

1. `0x49D270` → builder → `0x49C740` makes a projectile whose `+0x66` owner is the shooter's
   player, a type-3 seat.
2. The projectile pass 0x49B720 (0x495513) moves it on every machine. Burst weapons (`+0x111`
   bit 11) spawn their follow-ups there, with the sound at 0x49B87A.
3. **Impact `0x499EB0(proj, victim)`, `ret 8`:**
   - `ebx` = impact under water.
   - Unless weapon `+0x111` bit 22: stop the camera follow and mark the projectile done
     (`+0x69 |= 2`).
   - Water with no victim and `[main+0x391E9]+0xD48` set: return with nothing more
     (0x499F3C..0x499FA8) [INFERRED: a non-water weapon absorbed].
   - Otherwise 0x41C640.
   - The **sound** `0x47F300`, positional: water `+0xF8` (0x499FD8) or hit `+0xF6` (0x499FF2).
   - The **picture**: `0x472810` for `+0x111` bit 10, else `0x420A30(…, +0x78, …)`.
   - Then the **owner gate** (0x49A01B..0x49A047): the owner's record `main+0x1B63 +
     (+0x66)·0x14B`, active and `+0x73` == 3, jumps to 0x49A0AE with no damage.
   - Otherwise:
     - area damage `0x49A120` when AoE `+0xD6` > 16 or there is no victim;
     - else direct damage `0x499CD0`.
   - B7's hook at 0x49A01B also skips received meteors.
4. All ten callers of 0x499EB0 are behind that one gate: 0x49A764, 0x49AFEB, 0x49B074, 0x49B1A9,
   0x49B213, 0x49B275, 0x49B3CD, 0x49BA6F, 0x49BACC, 0x49BC70.

   So, for a replayed projectile on the host:
   - no call of 0x489BB0 or 0x489CE0;
   - no HP, kill or resource change;
   - no feature hit (the `0x0F` sender 0x424575 is reached from area damage);
   - no interception (§1.7).

   What remains is a picture and a sound. **Allowed: presentation.**
5. The receiver's own writes (§1.4) are the only recorded-state writes of a replayed shot.

### 2.2 A replayed death

- **The blast is a picture.** `0x49B000(unit, selfd)` builds the death blast as a projectile of
  the dying unit's owner, a type-3 seat. It ends in the same 0x499EB0, so it is a picture and a
  sound. C4's note agrees: "its damage is computed only on the dying unit's owner's peer
  (0x49A043), a picture on every other". **Allowed.**
- **The wreck.** `0x486360` spawns it through 0x423C50 on every machine, like a live remote.
  - The spawn is a function of the recorded inputs (§1.9 makes them exact) and of the feature
    pool.
  - An empty pool creates nothing: 0x423DB9 and 0x423DDF return 0 after clearing the
    footprint, per the engine map.
  - Whether this is a second writer depends on whether S3 records the feature map (§6 D6).
- **Statistics and ranks** (the table in §1.2): the owner's destructor writes the same values
  from the same inputs. **Allowed: a replayed root's writes.** §1.10 checks them.
- **The cargo loop: must be kept off puppets.** `0x489BB0(passenger, …, 30000, 6 or 3)` at
  0x48680B does three things. First, it builds a `0x0B` and **applies it locally for every
  victim** (0x489C89 → 0x489CE0); type 3 is included.

  Second, 0x489CE0 (reached with the victim alive and bit 14 clear, 0x489D3F..0x489D53) writes
  recorded fields and makes presentation:
  - 0x467950;
  - the hit reaction `0x406F80` (0x489DA2), which runs the listeners (`+0x4E`), sets the AI
    timer for type 2 only, retaliates for types 1 and 2 only, and raises Under Attack;
  - `+0xF5` = kind;
  - `+0xF0`/`+0xF4` = the attacker;
  - music 0x494FF0(1) [INFERRED];
  - **HP `+0x108` −= damage**. At ≤ 0, a type 1 or 2 owner gets bit 14 (0x489EE2); a type-3
    owner is **clamped to 0** (0x489EF1).

  Third, for a type-3 victim it also sends the `0x0B` (0x489C99..0x489CCD).

  On the owner the passenger is local: its HP goes negative and the `0x0C` for it follows,
  recorded. On the host the same call writes HP = 0 and the attacker fields, **a second writer
  whose values differ** (clamped against negative).

  **Keep it off puppets:** in puppet mode, refuse 0x489CE0 for a victim whose owner is a puppet
  seat.
  - The natural place is the existing drop of B3's `wire_s0b` site at 0x489CED, whose drop exit
    is 0x489F93. The condition lives in the C function; there is no new site.
  - It covers every path into 0x489CE0: the local calls from 0x489BB0 and the wire.
  - The passenger's own recorded death does the rest.
- The detaches (0x4867CB, 0x48681D) write the recorded links `+0x86/+0x8A/+0x8E` exactly as on
  the owner. They broadcast a `0x0A` through 0x48AAC0; in a skirmish that goes nowhere
  [INFERRED from experiment 3's counts].
- `Killed` (0x486877) is a COB start: presentation.
- The **HitByWeapon and TakeDamage** starts (0x489F43, 0x489F8E) happen only where 0x489CE0
  applies a kind-1 hit: the shooter's machine, and the victim's owner through `0x0B`. A live
  third machine never shows them, and **the host never does**, because it applies no damage. A
  replay must record them to show what the game showed (§6 D2).

### 2.3 What the host does by itself

These are not driven by events.

| Path | Puppet units? | Verdict |
|---|---|---|
| The **reaper**: unit tick 0x48AFB9..0x48AFD1, `+0x110` bit 14 → `0x4864B0(unit, +0xF5)`, for every ticked unit of an active type 1–3 record | **yes**. Bit 14 reaches a puppet unit through the replayed create (`isfeature`, 0x486171), a replayed build done (`isfeature`, 0x41BA05), or the image. | **Keep off**: skip the call at **0x48AFD1** (`E8 rel32`, unhooked) in puppet mode for a type-3 owner. The recorded death is the one writer. Otherwise the host destroys the unit itself, and the recorded death then finds it dead (0x486706). |
| **Meteor shower** 0x437DE0 (0x495594): arm flag 0x51232C (set by 0x437D40 from the .ota read 0x4367F9 or the `+meteor` console command 0x41777A; cleared by 0x437D50), active flag 0x512318 | yes: a local stone has owner 10, not type 3, so the gate lets its damage through | **Keep off**: skip the call at 0x495594 in puppet mode. The console can re-arm the flag, so a one-time clear is not enough. The recorded stones replay as `0x0D` meteors, damage-free under B7. |
| **Fire spread** 0x49A0C0 (one caller, 0x423BE0, in the features pass): an owner-10 projectile, then `0x49A120` **with no gate** | yes, once features burn on the host | **Keep off in S3**: gate it in puppet mode, or leave feature burning to recorded feature events. |
| Water damage (kind 0xB every 30 ticks), regeneration 0x41BD10, 0x43B7C0, 0x43BAD0 | no: all behind the type 1/2 test 0x48AEB5..0x48AECD | inert |
| AutoAim, whose slot is `my_AutoAim` | no: needs a type 1/2 block owner (0x48ADC9..0x48ADDA) | inert |
| **Console kills** in the console handler 0x4161F0 (caller 0x417570): Send_UnitDeath 0x486ED0 (0x4164BD), 0x486E80 (0x4174B8), kill-all 0x486F10 (0x4164DF, 0x416A00, 0x416A58) | yes | **Keep off**: refuse in puppet mode. This is the puppet host track's, flagged here because it is a death path. |
| Player removal → kill-all (0x452E17) | only on a network leave | not reached on a host with no network |

### 2.4 Summary: allowed, kept off, reconciled

- **Allowed (presentation):**
  - pictures, sounds, particles and camera shake of replayed shots, blasts and impacts;
  - Fire, RockUnit and Killed starts;
  - "taken the lead" and the elimination line;
  - 0x4948B0's flashes.
- **Allowed (a replayed root's own writes, checked by §1.10):**
  - everything each receiver writes in §1.1–§1.6;
  - the destructor's statistics and ranks;
  - the detaches;
  - the corpse, if S3 treats it so (§6 D6).
- **Kept off puppets in puppet mode:**
  - the cargo-loop hit (0x489CE0 for puppet victims);
  - the reaper for puppet units (0x48AFD1);
  - the host's meteor shower (0x495594);
  - fire-spread damage (S3);
  - console kills;
  - the engine's own notice calls, if D1 records notices.
- **Reconciled by recording:**
  - interceptions (§1.7);
  - the victim's state at death (§1.9);
  - C4's carried flag (§1.2);
  - the owner-only starts, if D2 is taken.

### 2.5 Impure's hooks inside the replayed roots

A replayed root also runs every Impure site inside it. Those that write **SIM_STATE**, and so
are recorded:

| Hook | Site | Writes |
|---|---|---|
| B4 `hit_take` | 0x486036 | `s_hit` stamp and local time, per create |
| B4 `hit_freed` | 0x486DC1 | `s_hit.freed`, per death |
| C4 cargo marks | cargo loop | `s_txMark`, `s_txKillMark` |

(`s_txCarried` is `SIM_TRANSIENT`.)

These tables exist for peers that can diverge, which a puppet host cannot. The image carries
them. **Recommendation:** they skip their writes for puppet slots in puppet mode. Otherwise the
harness will name them as second writers whenever their inputs differ, for example `now`.

B3's bounds (0x4866E5, 0x486753, 0x489CED) and `wpn_rx_fired` write only diagnostics, and they
are wanted.

---------------------------------------------------------------------------------------------

## 3. New units on a puppet host

- **CreateFromNetwork `0x4861D0(player, rec)` is the live remote's path.**
  - It checks no type, no limit and no block range.
  - It destroys an occupant through `0x4864B0(occ,0)` (0x486237..0x486244).
  - It always creates the unit unfinished, with no layer argument.
  - It counts on its player argument while the owner comes from the slot's block.
  - Its success exit 0x48634F is B4's `hit_created`. An unknown caller there is stamped
    `HIT_UNKNOWN`.
  - A remote relies on a following self-`0x12` to finish a structure. That `0x12` runs
    0x41B8D0, which **refuses a unit with no build list** (0x41B8F7..0x41B8FF), so it cannot
    finish every unit.
- **Use `0x485F50` instead** (§1.1), with the recorded `finished`, `layer` and slot. The host
  then does what the owner did, and the engine refuses an occupied slot.
- **A nanoframe's build progress.**
  - `+0x104` (float, build remaining; 0.0 = done) and HP `+0x108` are record bytes, so the image
    carries them every step.
  - The builder's order that raises them is owner code and does not run on a puppet.
  - The **model's composite frame** `[+0x9E]+0x10` (the cached 1x bake) is not a record byte.
    At a finish only 0x41B8D0 clears it, so in a skirmish the **build-done event is required**:
    without it a finished structure keeps its nanoframe bake (on GDI its dont-cache pieces drawn
    twice, in both renderers the depth plane's waterline and digger clips; the engine map's
    *The composite and the build fraction*). A multiplayer segment clears it at the image's
    write instead ([the multiplayer slice](demo-recorder-multiplayer-slice.md), *The build
    finish*).
  - The nano spray is the owner's order code and is not reproduced. It is the S2 row's own
    item.
- **When a recorded unit appears, playback:**
  1. maps the new ID to the recorded slot;
  2. bounds owner and type;
  3. calls 0x485F50;
  4. if it returns NULL, reports the slot's occupant, an ordering bug.

  The first image after the create then supplies every value field. The creation made the
  objects (COB, model, mover), so the image application's object classes have something to
  bind to.
- **Order of events around a create:** the create's nested `0x48B090` and any shot, start or
  state event of the new unit in the same step come after it in recorded order. The unit
  therefore exists before they replay.

---------------------------------------------------------------------------------------------

## 4. Sounds

### 4.1 The sound entry points

All in-game plays go through the 0x47Fxxx module; every caller of 0x4CF570, 0x4D0640 and
0x4CF540 was checked.

| Entry | Play | Broadcast |
|---|---|---|
| `0x47F0C0(idx, bcast)`, `ret 8` | the table sound `[main+0x33A13+idx·4]`; needs fxvol `main+0x37F0C` ≠ 0, `main+0x37F19 & 7`, NoDirectSound 0x51E690 = 0; UseWindowsSound 0x51E694 picks 0x49F6A0 or 0x49F680 | with `bcast` set: sends `{0x13, 1, u32 idx}` (18 bytes) at 0x47F14C |
| `0x47F1A0(name, bcast)` | looks the name up in `main+0x33E13` (32-byte names, count `main+0x33A0F`) → 0x47F0C0 | as 0x47F0C0 |
| `0x47F210` | looping variant | |
| `0x47F290(name)` | by file, 0x4D0640 | no |
| `0x47F300(idx, pos, bcast)`, `ret 0xC` | plays only when `pos` is in the **viewed** player's sight grid (0x47F403..0x47F4AE; Impure's LOS fix owns two reads there) | broadcasts **before** its sight test, and writes byte [1] = 1 |
| `0x47F610(name, pos, bcast)` | by name, positional | |

- Every in-game event call passes `bcast` = 0, so stock never sends a `0x13` in play.
  Experiment 3 saw none.
- The `0x13` receiver (0x45561A) branches on byte [1]. Both senders write 1, so it always plays
  through 0x47F0C0 and never positionally.

### 4.2 Remote units and the local player

- **Made on every machine, for every unit, by paths a host runs:**
  - fire sound, weapon `+0xF4`: 0x49C740 → 0x47F300 at 0x49C866, from every projectile init,
    replayed shots and meteors included;
  - burst shots, weapon `+0x111` bit 11: the projectile pass 0x49B720 → 0x47F300 at 0x49B87A;
  - impacts, hit `+0xF6` and water `+0xF8`: 0x499EB0 at 0x499FF2 and 0x499FD8, before the
    owner gate;
  - death blasts: 0x49B000 makes a projectile, which gets the same fire and impact sounds;
  - treeburn: 0x4233A0 → 0x47F610 at 0x42350D (features, S3);
  - MessageArrived: 0x463CA0 at 0x463DF9 when its 4th argument ≠ 10 — "taken the lead" from the
    destructor, the elimination 0x47BD70, chat;
  - "Victory Condition" in 0x48E010, on the objective, which is the puppet host track's.
- **Only for the viewed player's own live units:** the unit replies, the notice queue (§1.8).
  - They include select, ok, arrived, cant, build, repair, working, load, unload, capture, the
    self-destruct countdown, activate, deactivate, cloak, uncloak, "Nanolathe Complete" and
    Under Attack.
  - The order handlers that raise most of them run only for a type 1/2 owner. A live remote
    never plays another player's.
- **The viewer's UI:** build menu, order buttons, squads and placement (0x47F1A0 by name).

### 4.3 Do the replayed receivers make the right sounds by themselves?

- **Yes, for everything in the first list.**
  - A replayed shot plays its fire sound in 0x49C740.
  - The projectile pass plays its burst and impact sounds.
  - A replayed death plays its blast, and the destructor's messages.
  - All of it is positional against the viewed seat's sight grid, which the host stamps for
    puppet units by the round robin's recipe.
- **No, for unit replies.**
  - The only replies a host makes itself are 0x48B090's kinds 3/4/14/15 for the watched seat's
    units, and Under Attack from the cargo loop's hit. Both are wrong: partial, and the second
    comes from a path §2.2 turns off.
  - Hence §6 D1.
- **How a reply plays: `0x47F680`**, the queue's consumer, called once per main-loop pass from
  0x499890 at 0x4999A6.
  - It hands entry 0 to `0x47FD70(entry, playSound, showText)`. Sound is allowed only when the
    previous reply has finished (`main+0x38A47` ≥ `[q+0x9D]` + `[q+0xA1]`); otherwise the text
    shows without sound.
  - 0x47FD70 picks a variant with the **CRT `rand`**: the category is `def+0x20E`, the record
    `[main+0x37E13] + cat·0x160`, the count `rec + kind·12 + 0x40`.
  - It plays `sounds\….WAV` by file through **0x4D0640**.
  - Its gates are: priority > 10 − `main+0x37F17`, count > 0, `playSound`, and
    `main+0x37F19` bit 0x40.
  - Text goes through `0x463CA0(buf,1,unitId,0xA)`, which has no MessageArrived.
  - The CRT `rand` is presentation (no simulation reader), so a replay may pick another
    variant than the recording did.

### 4.4 Open item 1: does 0x48B090 play a sound?

**Yes, through the notice queue, not through the sound entries.**

- **Its notices:**
  - `0x47F780(unit,3,0)` at 0x48B110 (Activate);
  - kind 4 at 0x48B135;
  - kind 14 at 0x48B178;
  - kind 15 at 0x48B1AA.
- **Gating:** each is gated to the viewed player's alive, not-dying unit, queued by 0x47FAD0,
  and played later by 0x47F680 → 0x47FD70 → 0x4D0640.
- **No `0x13`:** it never calls 0x47F0C0 or 0x47F300, and sends no `0x13`.
  - 0x47F0C0 has four callers: 0x45562D (the `0x13` case), 0x47F1FA, 0x47F274 and 0x47F31A.
- **On a host:** a `+0x10E` change applied through 0x48B090 reproduces the same reply once, if
  the watched seat owns the unit. It duplicates no recorded sound-entry call.

---------------------------------------------------------------------------------------------

## 5. Hook placement

### 5.1 What is already there, near the roots

`lim_overlap()` refuses two fail-closed sites covering one byte.

| Owner | Sites near the roots |
|---|---|
| B2 | 0x4954ED (the capture) |
| B3 | 0x4861F7, 0x4866E5 (`wire_s0c`), 0x486753 (`wire_s0ckill`), 0x489CED (`wire_s0b`), 0x4553FE; wire spans from 0x4866E0 (38 bytes, to 0x486706), 0x486753 (37), 0x489CED (74) |
| B4 | 0x486036 (`hit_take`), 0x486DC1 (`hit_freed`), 0x48634F (`hit_created`), 0x4854A0, 0x4560AE, 0x489CB9, 0x489CCD, 0x4653DE; dispatch slots 0x455F90, 0x455FA0, 0x455FA8 |
| B5 | 0x497F5E, 0x49842F, 0x45477F, 0x48BA00; `lim_same` 0x4861D0 (3 bytes) and others |
| B6 | `lim_same` 0x490C40, 0x490CE8; branches 0x490C5A, 0x491903 |
| B7 | 0x49A01B, 0x49DF7D, 0x49D307, 0x489C71, 0x489BF3 |
| B8 | 0x451DF0 entry (6 bytes, continues at 0x451DF6), 0x48666D, the create wrappers, 0x4555BA (`kill_s12`) |
| C4 | 0x49B017, 0x48664B, 0x486679, 0x486810, 0x486F10; `lim_same` 0x49B000, 0x49B027, 0x4865FC, 0x48661C, 0x486672, 0x4867C4, 0x486816, 0x486FC4 |
| weapons | `wpn_rx_fired` 0x49D280 (16 bytes, to 0x49D290, then `jmp` 0x49D2A6); `recv` 0x49D364; `cb0.d` 0x49D7D9 (inside the turret's `0x0D` block); the five `0x0D` sends 0x49D859, 0x49DB4D, 0x49DD27, 0x49DEEE, 0x49DFF6 and the `0x0E` reads and sends 0x49A78C, 0x49A7CD, 0x49A7A8, 0x49A7E9 (raised build only); `0x0F` 0x424575, 0x455FB8; and the whole hook table: 0x49E070, 0x49E1A0, 0x43E1E0, 0x43E240, 0x43E2E0, 0x406F80 (`my_Retaliate`), 0x4897E0, 0x4898B0, 0x489800, 0x48A060, 0x48A0A0, 0x48A0F0, 0x48A160, 0x48A190, 0x49ADF0, 0x49AA80, 0x49D120, 0x42B370, 0x4089A0 |
| others | explosion pools inside 0x420A30; COB sites 0x4B08F0, 0x4B09B2, 0x4B0A27, 0x4B0B25, 0x4B0C57; getter 75 0x480770 |

### 5.2 Proposed recorder sites

All are free by a source scan for every address within −16..+24 bytes. Bytes are from the
pristine exe. No jump lands inside any of them: every `j*` and `call` in the cached disassembly
was scanned.

| Root | Site | Bytes (whole instructions) | Notes |
|---|---|---|---|
| create | success exit **0x4861B3** | `8B C6 5F 5E 5D` (5) | sees `esi` = the unit and the arguments; the failure exit 0x4861BD is separate |
| death | entry **0x4866D0** | `83 EC 68 8B 15 E8 1D 51 00` (9) | 0x4866D9 is outside B3's span, which starts at 0x4866E0. C4's `tx_wrap` sits at the callers (0x486679 and doDeath), so the carried flag is already set when the entry runs. |
| build done | entry **0x41B8D0** | `56 57 8B 7C 24 0C` (6) | |
| state | entry **0x48B090** | `51 8B 44 24 0C` (5) | or after the store 0x48B0D8 (`88 87 0E 01 00 00`, then `je` at 0x48B0DE) |
| starts | **0x456190** / **0x456290** | `A1 E8 1D 51 00` (5) each | before the network tests 0x456198 and 0x456298 |
| starts | **0x456200** | `8B 44 24 08` + `83 EC 18` (7) | catches `my_AutoAim`, since it calls this address |
| shots | the five tests **0x49D7A0, 0x49DA92, 0x49DC64, 0x49DE25, 0x49DFAF** | `F6 8x 44 2A 00 00 01` (7) + `je` | taken while recording or networked; the packet from a callout in `wpn_tx_fired` (raised) or the recorder's own send sites (stock) |
| interceptions | callout in `wpn_tx_intercept_send` / `wpn_tx_intercept` | — | no new site in the raised build |
| notices (D1) | **0x47F780** entry, **0x47F850** entry | `8B 0D E8 1D 51 00` (6); `57 8B 7C 24 08` (5) | also where puppet mode makes the engine's own calls inert |
| reaper gate | **0x48AFD1** | `E8 rel32` (5) | or 0x48AFB9 `mov ecx,[esi+0x110]` (6) |
| meteor gate | **0x495594** | `E8 rel32` (5) | the shower call |
| owner-only starts (D2) | the calls 0x489F43, 0x489F8E (HitByWeapon, TakeDamage); transports 0x406834, 0x4069BF, 0x4113EF; EndTransport 0x40F433, 0x41148D, 0x411794, 0x411DA1, 0x411E2B | `E8 rel32` each | the TakeDamage call 0x489F8E ends at 0x489F93, which is B3's `wire_s0b` drop target and the function's common exit. A 5-byte site there ends exactly at that target, so it may be placed. |

Other sites considered and rejected:
- **B8's send hook.** One site sees every message, and experiment 3 used it. But it depends on
  releasing the tests and on messages existing in a skirmish, and deaths and creates arrive
  there in B8's wrapped forms. The roots do not depend on any of that.
- **Hooking 0x49C740 for shots.** It loses the slot angles, the builders' Fire, RockUnit and
  particles, and the interception flag.

---------------------------------------------------------------------------------------------

## 6. The decisions

Taken 2026-09-30 (the slice note's *Decided 2026-09-30* and *S2a*): D1 as recommended; D2 wider than recommended, every owner-only start the game showed is recorded, TargetCleared included, and BeginTransport is not among them, because 0x411403 broadcasts it (0x456200) and the broadcasters record it; D3 and D5 as recommended, implemented by S2a; D4, D6 and D7 are S2b's.

Format 23 (2026-10-13) records BeginTransport at its start `0x4113EF` after all, and leaves its broadcast out: the start runs at once on the recording machine, while the broadcast is started with run-now 0 by a remote copy and by a playing, one slice later (the engine map, *BeginTransport is recorded at its start*). `setSFXoccupy` (`0x43DBE8`) joins the owner-only starts in the same format.


- **D1 — Unit replies on a replay.** A live remote never plays another player's; the watched
  seat's own machine did.
  - **Recommend:** record the notices at the 0x47F780/0x47F850 entries for every player, and
    replay them through the same functions. The engine's own viewer gate then plays the watched
    seat's replies as that seat heard them. Make the host's own notice calls inert in puppet
    mode.
  - The alternative is a live remote's silence.
  - Variant and timing may differ (CRT `rand`, the reply-length gate): presentation.
- **D2 — Owner-only COB starts the game showed.** These are HitByWeapon and TakeDamage on every
  hit, and TransportPickup, TransportDrop, BeginTransport and EndTransport.
  - CLAUDE.md's "a replay shows what the game showed" argues for recording them as script
    starts (index, arguments) and replaying them through 0x4B0B00.
  - **Recommend yes for HitByWeapon and TakeDamage in S2.** They are the hit's visible reaction.
  - The transport ones go with the carry track.
  - TargetCleared (0x489898, 0x489948, 0x48A149, 0x48A2E0 — Impure's replaced targeting) is
    lower value; recommend leaving it out unless a picture shows the need.
- **D3 — Recording nested calls.** Recommend recording every call and relying on the three
  engine tests plus the order-node rule (§1.0, §1.2). A depth counter would record top-level
  calls only, but it would then need puppet order nodes that carry `+6` bit 0x02 and `+0x42`
  bit 0x400000, which run owner code on a puppet.
- **D4 — Impure's SIM_STATE hooks inside replayed roots** (§2.5). Recommend that they skip their
  writes for puppet slots in puppet mode, with the image as the writer. The alternative is
  accepting their writes as a replayed root's, which is a loosening.
- **D5 — The death event carries the victim's record** (§1.9). Recommend yes: it costs about
  0x118 bytes per death and makes the corpse and blast exact by construction. Without it the
  corpse is exact only when the last step crossed no cell.
- **D6 — Projectiles and features on the host.** The replayed roots make projectiles and
  corpses on the host, as a live remote's do.
  - The recommendation here was that the projectile pool is **not** recorded state but
    presentation, rebuilt from shots, bursts and interceptions. **S3b superseded it** (the owner,
    2026-10-01): the pool is recorded and a puppet host writes it at every step, since a seek must
    show the shots in flight and a burst's copies draw the simulation RNG (the
    [slice note](demo-recorder-skirmish-slice.md)'s *Decided 2026-10-01*).
  - The corpse is S3's call. If S3 records the feature map, the destructor's corpse is a
    replayed root's write to be checked by §1.10. The alternative is to make it inert and
    replay feature events.
- **D7 — Engine gates on the host.** These are required by the one-writer rule, not loosenings:
  - the reaper (0x48AFD1);
  - the meteor shower (0x495594);
  - the cargo-loop hit (the 0x489CED drop);
  - console kills;
  - S3's fire spread.

---------------------------------------------------------------------------------------------

## 7. Corrections to the notes

1. **The B8 claim is wrong.** It says "0x48B110 → 0x47F780(unit,3,0) → 0x47F0C0, sent at
   0x47F14C" (`tagpu_patches.c` ~8323, `sim-fixes.md` ~1609, and the engine map's "what one
   local create sends", ~12223).
   - 0x47F780 queues a notice.
   - The consumer 0x47F680 plays by file through 0x4D0640.
   - No `0x13` is ever sent from that path. B8's bound counting it is 18 bytes too high:
     harmless, since it is an upper bound.
   - The engine map's line 1048 ("whether it reaches 0x47F0C0 is open") is answered: **no**.
2. **The wrong consumer.** The queue's consumer is **0x47F680**, called at 0x4999A6 from
   0x499890 once per main-loop pass. It is not 0x47FCA0, which has no caller; its two calls of
   0x47FD70 (0x47FCD9, 0x47FCF8) are dead. This affects `data-keys-evidence.md:664` and the
   engine map ~5195.
   - 0x47F680 also withholds a reply's sound while the previous one plays: the
     `main+0x38A47` test against `[q+0x9D]` + `[q+0xA1]`.
3. **The next time.** 0x47FD70 writes the kind's next time (+30·interval, 0x47FEFD) whenever its
   four gates pass, **even with fxvol 0**. The engine map's line reads as though the play must
   happen.
4. **The `0x13` branch.** The receiver's positional branch is unreachable from stock. Both
   senders (0x47F0C0 at 0x47F14C, and 0x47F300 before its sight test) write byte [1] = 1, so
   0x45561A always takes 0x47F0C0.
5. **The table size.** The notice table has 24 kinds (0–23; 16 capture, 17–22 the self-destruct
   countdown, 23 canceldestruct), ending at 0x508917. Nothing bounds the index.
6. **Engine map line 1060 needs a qualifier.** "A start a script issues is never broadcast, and
   runs again on each copy" holds for script-issued starts. The **engine-issued** owner-only
   starts are never reproduced on a copy at all: HitByWeapon and TakeDamage in 0x489CE0,
   TransportPickup and TransportDrop in 0x403180, BeginTransport and EndTransport in 0x410830
   and 0x40F1E0, and TargetCleared.

---------------------------------------------------------------------------------------------

## 8. What could not be established

- **Nothing was run.** Every recommendation is from the disassembly. In particular these are
  unmeasured:
  - the §1.9 cell-crossing rate;
  - the COB eight-thread refusals on puppets;
  - whether a skirmish's 0x451DF0 does nothing past B8's hook [INFERRED from experiment 3,
    which counted sends there].
- **Names inferred, not traced:**
  - 0x48BCB0 ("on screen");
  - 0x482910 (sight reveal of the dying unit for the viewed player);
  - 0x47CBD0 and 0x482090 (bucket, sight);
  - 0x494FF0 (music intensity);
  - 0x4948B0 (player-list flash);
  - 0x467950;
  - 0x41C640;
  - 0x48A870;
  - 0x482AC0;
  - 0x490580;
  - `[main+0x391E9]+0xD48` (the water-absorption switch).
- **The attacker's `+0xB0` = GameTime + 600** written by a replayed shot equals the owner's only
  if the host's GameTime tracks the recording's. That is the puppet host track's pacing.
- **Whether S3 records the feature map and whether S2 records order nodes' `+0x4E`.** These
  decide whether the corpse and the listener events (hit 0x10, cloak 0x10000) are second
  writers.
- **Notice text pointers.** Which of the 82 notice callers pass a non-NULL text; only 0x438899
  was seen passing its own argument. A recorder that copies the string needs no answer to this.
- **What 0x401360 is.** It is in the players/AI phase (0x46555A), and it calls `0x48B090(8,…)`,
  0x41B8D0 and notices 7/8/9/10/23. Its exact role (the AI's build plan [INFERRED]) was not
  traced; it does not change any row.
