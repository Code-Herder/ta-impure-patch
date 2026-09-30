# Demo recorder S2 — order lists and factory queues (evidence)

## Status

**Desk research, 2026-09-30**, for the [skirmish slice](demo-recorder-skirmish-slice.md)'s S2, with the owner's decisions in that note's *Decided 2026-09-30*. It covers the order node, whether orders run on a type-3 puppet, how to record them, factory queues and rally points, and churn. S2a, the recording half, implements what this page found for the recorder and verifies it (the [slice note](demo-recorder-skirmish-slice.md)'s *S2a*); S2b builds the playback on the rest.

**How the facts were established.**
- The engine facts come from the retail executable, `pristine/TotalA.exe.pristine` (image base
  `0x400000`). Each one is tagged **DISASSEMBLED** with the addresses that show it, or
  **[INFERRED]**.
- **MEASURED** facts come from the notes, which are cited.
- Nothing was run for this research: no game, no `tacli`, no build. S2a's runs measured the recorder built on it.
- Facts the notes already hold are cited rather than derived again. §6 lists what is new or what
  corrects them.

"The engine map" is `research/notes/exe-reverse-engineering.md`. "The evidence note" is
`research/notes/tadr-port/demo-recorder-state-evidence.md`. "The completeness note" is
`research/notes/tadr-port/demo-recorder-completeness.md`.

---

## 0. The answers

**Q1: the node.**
- A node is one `0x56`-byte block from `MEM_Alloc`. There is no arena and no capacity limit, and a
  node never moves. It is freed with `MEM_Free` right after its destructor.
- Each unit has two singly linked lists: the main list at `unit+0x5C` and the sub list at
  `unit+0x60`. `next` is at `+0x4A`, and there is no previous link.
- The node carries:
  - a unit reference at `+0x12`, which links the node into its target's `+0xA2` chain;
  - per-order parameters at `+0x22..+0x3E`;
  - flags at `+0x42`;
  - a GameTime deadline at `+0x0A`;
  - a wanted-events mask at `+6` and a fired-events mask at `+0x4E`;
  - an owned object at `+0x52`.
- §1.9 has the byte map and §1.10 the proposed rows.

**Q2: do orders run on a puppet? No.**
- Both list controllers sit behind the unit tick's owner gate, which admits types 1 and 2 only.
- The only other handler call is the destructor's event-2 call, and a puppet can hold that one
  disarmed.
- On a puppet, a node is written only by these, which §2.2 lists in full:
  - the draw path's last-seen cache;
  - a replayed death, which frees the dying unit's lists and notifies nodes that target it;
  - a replayed hit, which notifies nodes that target the victim;
  - a replayed attach, which wipes the child's main list and pushes BeCarried;
  - the viewer's interface, if it can reach puppet units.
- Recorded nodes are inert data for drawing once four conditions hold (§2.4):
  - a bounded action byte;
  - a target set through the engine's reference set;
  - a bounded build type in `+0x36`;
  - the disarmed flags.

**Q3: recording. Recommended: option (b), list images reconciled.**
- Record each unit's two lists as a value image, delta-coded against the recorder's own previous
  image.
- Playback reconciles the puppet's lists against the decoded image each tick, using the engine's
  constructor, reference set, linker, destructor and allocator.
- No engine hooks are needed and no node identity. No node pointer survives a tick.
- A puppet holds three flag bits and the owned object disarmed.

**Q4: factory queues.**
- A factory's queue is its **count nodes** on the main list: BuildingBuild, with the type in `+0x36`
  and the count in `+0x3A`.
- The unit in progress is the head's target.
- The rally is a **QMove/QPatrol node** on the same list.
- **New:** the rally is inherited by the new unit's **GetBuilt handler** (`0x402DA0`) when the unit
  completes. The notes list this as open (T15).
- The engine shows a queue in three places:
  - the build menu's `+N` (`0x4199B0` → `0x439D80`);
  - the unit panel's status line and target (`0x46A860`);
  - the order markers, where count nodes draw nothing (mask 0) and the rally draws a QMove sprite.

**Q5: churn.**
- In steady state, idle armed units dominate: each unit's Standby node wakes every 30–59 ticks and
  draws the simulation RNG.
- In a fight, engagements dominate. Each one replaces a Standby node with two reaction nodes and
  later makes three frees and one new default order.
- Deaths, commands and builds add the rest.
- None of this was measured. §5 gives an order of magnitude from the notes' measured battle.

---

## 1. The order node

### 1.1 Storage: one heap block per node, no arena

- **Allocation** [DISASSEMBLED]:
  - Every node is `push 0x56; call MEM_Alloc 0x4B4F10`. There are **54 such sites**, grouped in
    Appendix B, and all are on the game thread.
  - `MEM_Alloc` is the CRT heap (engine map, the allocator section). A 0x56-byte block is in the
    small-block heap, which can decommit a page inside a free. **So a node pointer is dead the
    moment its node is freed.** Nothing may keep one across engine code.
  - Nothing counts nodes, so there is no capacity limit. A block never moves.
- **Construction** [DISASSEMBLED]:
  - Nodes are built only by the constructor `0x43A0C0` (48 calls) and the restore's `0x43A420` (one
    call, `0x487594`).
  - The proof: the node vtable `0x4FD2C8` (base `0x4FD2CC`) is written only by those two
    (`0x43A0DA`, `0x43A131`; `0x43A432`, `0x43A445`) and by the destructor (`0x43A1F8`), and by no
    other instruction in `.text`.
- **Free** [DISASSEMBLED]:
  - The node's class has one virtual, slot 0 = `0x438870` (`+0x4E |= event`; the engine map). The
    base's slot 0 is `0x43A1E0`, a bare `ret 4`.
  - So nothing frees a node through its vtable. Every destruction is a direct `call 0x43A1F0`
    followed by `MEM_Free 0x4B4F20` (cdecl; for example `0x439FC9..0x439FCF`,
    `0x4384E4..0x4384EA`).
- **Impure's "order arena"** (`tagpu_order.c`, `MAXORD 6144` records of `ORDREC`) is Impure's
  double-buffered render snapshot. It is not the engine's store.

### 1.2 The descriptors and the action byte

The notes already establish that the array lies between `*(0x512344)` and `*(0x512348)`, with
stride `0x19` and 68 records fixed for the process. It is built once by `0x43C050` and sorted by
name, and the action byte `+4` is a node's rank in it (sim-fixes evidence; the engine map).

New or pinned down here [DISASSEMBLED]:
- **The record:**

  | offset | field | shown by |
  |---|---|---|
  | `+0x00` | the status text | `0x439DF0`, `[base + 0x19·action]` |
  | `+0x04` | the handler | the three dispatch sites |
  | `+0x0C` | the marker mask | `0x439B30` |
  | `+0x11` | the node flags | copied into `+0x42` by the constructor |
  | `+0x15` | the name | the binary search in `0x438760`, compared at `0x4387A9..0x4387AD` |

- **`0x438760`** looks an action up by name. It is thiscall with `ecx` = the slot that receives the
  rank, and one argument, the name. It derives the count from the array itself as
  `([0x512348] − [0x512344]) / 0x19` (`0x43876C..0x43878A`); this is the bound the playback uses.
- Readers that index the array by `+4` **with no bound**:
  - the controllers and the destructor (the notes);
  - the marker walker's mask reads (`0x439B30`, five sites);
  - `0x439DF0`, the panel's status line.
- Appendix A has all 68 records: rank, name, handler, marker mask, flags and status.

### 1.3 Construction

**`0x43A0C0`** [DISASSEMBLED]:
- Calling convention: thiscall on the block, arguments `(action, target, pos*, p1, p2, p3)`,
  `ret 0x18`, returns the node.
- What it writes:

  | field | value |
  |---|---|
  | vtable | `0x4FD2C8` |
  | `+0x04` | the action |
  | `+0x05` | 0 |
  | `+0x06` | 0 |
  | `+0x0A` | −1 |
  | `+0x0E` | 0 |
  | `+0x12..+0x21` | the reference, built by `0x4895C0` with the target, owner = the node (`0x43A137`) |
  | `+0x22/+0x26/+0x2A` | `*pos`, or zeros |
  | `+0x2E..+0x35` | zeroed as two dwords |
  | `+0x36/+0x3A/+0x3E` | `p1, p2, p3`, copied verbatim |
  | `+0x42` | `[desc+0x11]`, minus `0x200` with no target and minus `0x400` with no `pos` |
  | `+0x46` | GameTime (`0x43A12E`) |
  | `+0x4A` | 0 |
  | `+0x4E` | 0 |
  | `+0x52` | 0 |

- Without `0x200` it calls `0x489690(0)`.
- It links nothing. The caller links the node.

**`0x43A420`, the restore** (one caller, `0x487594`, on the LOADER thread inside a game load; the
engine map) [DISASSEMBLED]:
- It finds the action by name, falling back to the saved index.
- The owner must match.
- It takes the target through `0x487080`, then `0x489690`.
- It restores these fields from the save:
  - `+4`, `+5`, `+6`, `+0xA`;
  - `+0x22..+0x2A`, `+0x2E`, `+0x32`;
  - `+0x36`, `+0x3A`, `+0x3E`;
  - `+0x42`, `+0x4E`.
- It stamps `+0x46` with the load's GameTime (`0x43A471`); the saver does not save it.
- It builds the owned object by its class code through the jump table `0x43A918` (§1.7).

**`0x43A970`, the saver** (callers `0x487777` and `0x4877B7`) [DISASSEMBLED]:
- It writes a `0x3A`-byte record:

  | record offset | holds |
  |---|---|
  | `+0` | the owner's slot |
  | `+2` | the live target's slot, or 0 |
  | `+4` | the owned object's class code, `+0x52->vt[2]()`, or 0 |
  | `+8` | `+4` |
  | `+9` | `+5` |
  | `+0xA` | `+6` |
  | `+0xE` | `+0xA` |
  | `+0x12` | `+0x22..+0x2D` |
  | `+0x1E` | `+0x2E` |
  | `+0x22` | `+0x32` |
  | `+0x26`, `+0x2A`, `+0x2E` | `+0x36`, `+0x3A`, `+0x3E` |
  | `+0x32` | `+0x42` |
  | `+0x36` | `+0x4E` |

- It then writes three more things:
  - `"%s_name"`, the descriptor's name;
  - for MobileBuild, VTOL_MobileBuild and BuildingBuild, `UTYPENAME%4d`, the name of the type in
    `+0x36`;
  - the owned object's own serialization through its `vt[1]` (`"%sg"`).
- **This is the engine's own model of an order list: a sequence of value records.** Option (b)
  follows it.

### 1.4 The lists and their linkers

The two heads are `unit+0x5C` (main) and `unit+0x60` (sub). A node belongs on the sub list when its
`+0x42` has `0x40000` (BuildWeapon, SelfDestruct). There is no previous link, no tail pointer and no
count. Every unlinker walks from the head.

| function | convention | what it does [DISASSEMBLED] |
|---|---|---|
| `0x43AD10` | stdcall `(unit, node)`, `ret 8` | appends at the tail of the list that `node+0x42 & 0x40000` names; sets `+0xE = unit` and `+0x4A = 0` (`0x43AD38..0x43AD3D`); 16 callers |
| `0x43AC60` | stdcall `(unit, node, before)` | inserts before `before`; 2 callers (the default order, `0x43BA1E` and `0x43BA30`) |
| `0x43ACB0` | stdcall `(unit, node)` | pushes at the head; 40 callers, the handlers among them |
| `0x4384A0` | stdcall `(child)` | the attach's wipe and insert, inline (§2.2) |
| `0x439F80` | stdcall `(unit, node)`, `ret 8` | finds the node in the list its `0x40000` names; unlinks it; sets `+0x42 \|= 0x10000` unless the node is the **main** head (it compares with `[unit+0x5C]` even for a sub-list node, `0x439F89`/`0x439FB6`); destroys and frees it. A node it does not find is left alone, not freed. 2 callers |
| `0x439EB0` | `(unit, all)` | clears the lists; 3 callers, the unit destructor's `0x4867A1` among them |
| `0x43ADC0` | stdcall `(action, queue, unit, target, pos*, p1, p2)`, `ret 0x1C` | the order command: allocates at `0x43ADC4`; with `queue` 0 it destroys the main-list nodes that lack flag 4; 27 callers (§2.2) |
| `0x43AFC0` | — | SHIFT commands, cancels within 16 world units; 2 callers, `0x4197A9` and `0x48D1F4` |
| `0x43B0B0` | `(action, unit, type, amount)` | the queue count (the evidence note); 2 callers |
| `0x43A020` | — | adds the patrol return point (allocates at `0x43A054`); 4 callers |
| `0x43B1F0` | stdcall `(unit, target, flag)` (Standby, `0x40600B..0x40600F`) | a reaction pair (§1.9, the leash); 9 callers |
| `0x43B400` | — | a reaction pair; 2 callers |
| `0x43B730` | stdcall `(unit, action)` | pushes a temporary (`0x4000`) node before the head; **no callers** (`xref`: none, E8 or absolute) |
| `0x439E30` | — | finds by action; 1 caller, the Ctrl+D cancel |
| `0x439D80` | stdcall `(unit, type)`, `ret 8` | sums `+0x3A` over the nodes with `0x100` and `+0x36 == type`, **both lists** |
| `0x439DD0` | stdcall `(unit)`, `ret 4` | the main head's target, or 0 |
| `0x439DF0` | stdcall `(unit)`, `ret 4` | the main head's status text, or record 0's text (`'Ready'`) |
| `0x439E80` | thiscall `(node, n)`, `ret 4` | `+6 \|= 1` and `+0xA = GameTime + n` (the engine map) |
| `0x43AD50`, `0x439FE0` | — | no callers |

### 1.5 Destruction and its side effects

**`0x43A1F0`** [DISASSEMBLED, `0x43A1F0..0x43A2C9`]:
- Calling convention: thiscall, `ret`.
- It has 12 callers: `0x4384E4`, `0x439EF3`, `0x439F5F`, `0x439FC9`, `0x43AE47`, `0x43AEB8`,
  `0x43B075`, `0x43B1C9`, `0x43B93B`, `0x43BA5B`, `0x43BBE6`, `0x43BC49`.
- It runs these steps in order:
  1. When `+6` has `0x2`, it calls `handler(owner, node, 2)` (`0x43A1FE..0x43A21A`). This dispatch
     is unbounded (the notes).
  2. When `+0x42` has `0x400000`, it starts COB `StopBuilding` on the owner (`0x4B07C0`,
     `0x4B0B00`), **broadcasts** it (`0x456190` at `0x43A258`) and clears the bit (`0x43A21E..0x43A25D`).
  3. The owned object, when the owner has a mover (`[owner+0]`, `0x43A264..0x43A26B`). The mover's
     class object is `C = [mover+0]` and its current object is `[C+4]`.
     - When `[C+4]` is non-NULL and equals `+0x52`, `C->vt[1](0)` uninstalls it, then
       `+0x52->vt[0](1)` deletes it.
     - Otherwise it deletes `+0x52` when non-NULL (`0x43A2A2`, the MEASURED crash site).
     - Then `+0x52 = 0`.
     - **When `+0x52` is 0 the mover is never touched:** a NULL `[C+4]` jumps to `0x43A297`, a
       different non-NULL one fails the `cmp`, and `0x43A27D` refuses a zero `+0x52`.
     - With no mover the owned object is **not** deleted: the jump goes from `0x43A26B` to
       `0x43A2AB`.
  4. **When `+0x42` lacks `0x10000`, it calls `0x489800(owner, 3)`** (`0x43A2AB..0x43A2B9`).
  5. `0x489650` unlinks the reference at `+0x12` (`0x43A2C1`).

**`0x489800(unit, slot)`** [DISASSEMBLED]:
- Calling convention: thiscall. Slot 3 means all three.
- For each present weapon slot whose bit 4 is clear:
  - it sets bit 4;
  - it resets the slot's target to `(0, 0x8000)`;
  - it starts COB **`TargetCleared`** (`0x508D58`) through `0x4B0A70`, the COB object's own start,
    not one of the broadcasters.
- It has 20 callers: 16 in handlers, plus the destructor, the spawn init `0x485C58` and its own
  recursion.
- **So destroying a head node that lacks `0x10000` writes the owner's weapon slots and starts a
  script.**

**Where `0x10000` comes from** [DISASSEMBLED]:
- Thirteen removal sites set it right before a destruction, for a node that is not the head:
  - `0x4384D7`;
  - `0x439EE6`, `0x439F52`, `0x439FBC`;
  - `0x43AE3A`, `0x43AEAB`;
  - `0x43B068`, `0x43B1C0`;
  - `0x43B92E`, `0x43BA4E`;
  - `0x43BB9B`, `0x43BBD9`, `0x43BC3C`.
- Six descriptors carry it from construction (flags `0x10060`): Activate, Cloak_Off, Cloak_On,
  Deactivate, Standing_FireOrder and Standing_MoveOrder.
- Its only direct test is `0x43A2AB`.
- `0x43ADC0` destroys the leading `0x4000` nodes each as the head (`cmp esi,esi` at `0x43AEA5`), so
  `0x489800` runs for each of them.

### 1.6 The unit reference at `+0x12`

The notes hold that `+0x12..+0x21` is `{vtable 0x4FD754, +0x16 target, +0x1A next in the target's
+0xA2 chain, +0x1E owner = the node}`, pushed at the front by `0x489690` and unlinked by `0x489650`.
Each linker refuses a NULL target or one with `+0xA6 = 0`.

What each part does:
- **At the target's death**, `0x489740` (only caller `0x486D75`, the unit destructor) calls
  `owner->vt[0](8)` on each reference, which sets `+0x4E |= 8`, and then unlinks it, which sets
  `+0x16 = 0`.
- **At a hit**, `0x4897B0(victim, 0x10)` (only caller `0x406F8C`, the hit reaction, unconditional)
  sets `+0x4E |= 0x10` on every node that targets the victim.
- The chain's order changes nothing, because both walkers only OR a bit into `+0x4E` (the engine
  map).

**New** [DISASSEMBLED, `0x4897B0..0x4897D9`]: a reference whose owner is 0 is skipped
(`0x4897C7`). That covers the air payload's reference, which is built with owner 0.

### 1.7 The owned object `+0x52`

What installs it [DISASSEMBLED]:
- `0x4388D0(node, obj)` installs one. It has 91 call sites, all in handlers. It uninstalls and
  deletes the old object, masks `+0x4E &= 0xFFFFFC1F`, calls `C->vt[1](obj)` and stores `+0x52`.
- The restore hands the head's object to the mover through `0x4388B0` (`0x4875CB`; the engine map).

Its classes, from the restore's jump table `0x43A918` and the vtable stores [DISASSEMBLED]:

| code | vtable | size | load constructor | other constructor |
|---|---|---|---|---|
| 0, 1 | — | — | none | — |
| 2 | `0x4FD3B8` (base `0x4FD2F8`) | `0x36` | `0x44DE80` | `0x44E080`, the `0x2C` receiver's payload parse |
| 3 | `0x4FD3F8` | `0x2C` | `0x44E7D0` | — |
| 4 | `0x4FD328` | `0x14` | `0x44D010` | `0x44CF60`, from `0x438930` |
| 5 | `0x4FD358` | `0x1C` | `0x44D470` | `0x44D3B0`, from `0x438A00` |
| 6 | `0x4FD388` | `0x18` | `0x44D930` | `0x44D8A0`, from `0x438AD0` |

- Class 2 is the notes' "air orders' payload object". It holds a unit reference at `+0x16` with owner
  0 (`0x44DEFE`, `0x44E0FF`), so its target is a unit ID too.
- The ground classes 4–6 are made only when def `+0x241` bit 11 is clear.
- The capture can map the vtable to the code with this fixed five-entry table. That calls no engine
  code, and an unknown vtable is a named failure.

### 1.8 The flags `+0x42`

[DISASSEMBLED] unless tagged. The descriptor bits come from `[desc+0x11]` at construction.
`0x438B90` re-types a node: the new descriptor's flags replace the old ones, keeping only `0x600`.

| bit | meaning | set by / tested at |
|---|---|---|
| `0x1` | linked by the command `0x43ADC0` | `0x43ADC0` |
| `0x2`, `0x8`, `0x10`, `0x80`, `0x800`, `0x1000000` | descriptor properties, not decoded here | `0x8` is read by the AI at `0x408198` |
| `0x4` | kept by the wipes | `0x4384C3`, `0x439ED2` |
| `0x20` | pushed at the head | descriptor |
| `0x40` | not wiped by the command | `0x43AE0E` |
| `0x100` | a count node, summed by `0x439D80` | BuildingBuild, BuildWeapon, MobileBuild, VTOL_MobileBuild |
| `0x200` / `0x400` | has a unit target / has a position | constructor |
| `0x1000` | the command's queue insertion mark | `0x43ADC0` |
| `0x2000` | announce on the first run | `0x438880` → `0x47F780(owner, 5)` |
| `0x4000` | a default or temporary order | `0x43BA05..0x43BA0D`, `0x43B761`; copied from the old head by `0x4384A0` (`0x43856E`) |
| `0x8000` | the patrol return point was added | `0x43A020` |
| **`0x10000`** | **no weapon-target reset at destruction** | §1.5 |
| `0x20000` | Standby (the hit reaction reads it) | descriptor |
| `0x40000` | belongs on the sub list | the linkers |
| `0x100000` | a nanolathe order | descriptor |
| **`0x200000`** | **the last-seen cache is valid** | `0x439740` (`0x4397C5`); Impure's `resolve_sprite` |
| **`0x400000`** | **`StopBuilding` is owed** | set by `0x438590` (`0x4385E3`), cleared by `0x4385F0` and the destructor |
| `0x800000` | set on the controller's result 9 | `0x43B7C0` |

### 1.9 The byte map

Class `—` means a plain value: no row, recorded whole and replayed exactly. §1.10 has the rows.

| off | size | field | class | writers (entries) [DISASSEMBLED] | notes |
|---|---|---|---|---|---|
| `0x00` | 4 | vtable | — | `0x43A0C0`, `0x43A420`, `0x43A1F0` | constant `0x4FD2C8` in every live node; the capture may assert it and leave it out of the wire |
| `0x04` | 1 | action | — | `0x43A0C0`, `0x43A420`, `0x438B90` | the rank; **bounded by the playback** (< 68) |
| `0x05` | 1 | state | — | `0x43A0C0`, `0x43A420`, the controllers (results 0, 1), handlers | the handler's state machine |
| `0x06` | 4 | wanted events | — (bit `0x2`: row) | `0x43A0C0`, `0x43A420`, `0x439E80`, 51 handler stores | the controller fires on `(+0x4E \| unit+0xBA) & +6`; bit 0 is the timer |
| `0x0A` | 4 | deadline | — | `0x43A0C0` (−1), `0x43A420`, `0x439E80` | a GameTime value (the simulation clock), so not `timer` |
| `0x0E` | 4 | owner | row | the linkers | the unit whose list holds the node |
| `0x12` | 4 | reference vtable | — | `0x4895C0` | constant `0x4FD754` |
| `0x16` | 4 | target | row | `0x4895C0`, `0x489690`, `0x489650`, `0x489740` | |
| `0x1A` | 4 | next reference | row | the reference functions | |
| `0x1E` | 4 | reference owner | row | `0x43A0C0` (`0x43A137`), `0x43A420` (`0x43A44F`) | the node itself |
| `0x22` | 12 | position | — | `0x43A0C0`, `0x43A420`, handlers (MobileBuild's state 0 snaps the site) | 16.16 world coordinates |
| `0x2E` | 2 | leash x | — | `0x43A0C0` (0), `0x43B1F0` (`0x43B334`), `0x43B400` (`0x43B4FE`, `0x43B632`), VTOL_Standby `0x40F7D0` (`0x40F9F4`) | **new:** `unit+0x6C` in whole world units, the leash origin of a reaction |
| `0x30` | 2 | leash z | — | the same (`0x43B33C`, `0x43B50F`, `0x43B63A`, `0x40F9FC`) | **new:** `unit+0x74`. Read by Attack_Chase `0x4034E1`/`0x4034F5`, RepairUnit `0x40533D`/`0x405345`, VTOL_Standby `0x40F839`/`0x40F841`, the air attacks `0x412093`/`0x4120A7`/`0x41284E`/`0x412856`/`0x412F4C`/`0x412F54`/`0x413679`/`0x41368D`, VTOL_RepairUnit `0x414EAD`/`0x414EB5` |
| `0x32` | 2 | last-seen x | row | `0x439740` (`0x4397AF`), `0x43A0C0`, `0x43A420`; Impure `resolve_sprite` | read only by `0x439740` (and saved) |
| `0x34` | 2 | last-seen z | row | `0x439740` (`0x4397BC`), the same | |
| `0x36` | 4 | p1 | — | `0x43A0C0`, `0x43A420`, handlers | the type ID of build and count nodes (u16, drawn unbounded by `0x438C00`); a countdown in Reclaim (`0x404BDC`, `0x404C84`); 1 in Standby_Mine's SelfDestruct |
| `0x3A` | 4 | p2 | — | the same | the count of a count node (`0x439D80`) |
| `0x3E` | 4 | p3 | — | the same | the reaction's def `+0x214` / `+0x202`; MobileBuild clears it |
| `0x42` | 4 | flags | — (bits `0x10000`, `0x200000`, `0x400000`: rows) | §1.8 | |
| `0x46` | 4 | issue time | — | `0x43A0C0` (`0x43A12E`), `0x43A420` (`0x43A471`) | GameTime at construction; read only by `0x438CC5` (in the build-site rect `0x438C00`) and `0x439537` (in `0x4394E0`), both drawing; not saved |
| `0x4A` | 4 | next | row | the linkers | |
| `0x4E` | 4 | fired events | — | `0x438870` (the notices), `0x43A0C0`, `0x43A420`, `0x4388D0` (`&= 0xFFFFFC1F`), the controllers | |
| `0x52` | 4 | owned object | row | `0x4388D0`, `0x43A0C0`, `0x43A1F0`, `0x43A420` | §1.7 |

The build-type reads on `p1..p3` were sampled, not audited in all 68 handlers: the build types,
counts, the reaction's range, Reclaim's countdown and SelfDestruct's 1. No pointer was found. A
pointer-valued parameter would appear as a replay mismatch the harness names.

### 1.10 The rows

The order node is the generator's third record kind (`RECORDS["order"] = 0x56`), with its rows in
`tagpu/ddraw/replay-layout.tsv`, the source of truth: `+0x0E` `owner`, `+0x1A` `refnext` and `+0x1E`
`refowner` cache; `+0x16` `target` `id:unit`; `+0x32` `seenx`, `+0x34` `seenz` and `+0x42` bit 21
`seen` drop; `+0x4A` `next` object; and the four fields that arm owner code when a node is destroyed
in the classes the owner approved 2026-09-30, `puppet0` (a puppet holds the bits clear or the
pointer NULL) and `puppet1` (held set), both recorded, never applied to a puppet, and restored by a
takeover: `+0x06` bit 1 `ondestroy`, `+0x42` bit 22 `stopbuilding` and `+0x52` `owned` (as its class
code, `puppet0:owned`) in `puppet0`, `+0x42` bit 16 `keeptargets` in `puppet1`. The file's
`ondestroy` row lists the handlers whose stores were checked one by one; a first list here named
three more (`0x403F70`, `0x413D80`, `0x414380`) and put two of AirStrike's stores in AirToGround,
which the check corrected. The unit's `orders` row gained the writer `0x4384A0` (§6).

---

## 2. Orders on a type-3 puppet

### 2.1 The handlers never run

[DISASSEMBLED, `0x48AEB5..0x48AECD`] The unit tick loads the owner, `unit+0x96`, and jumps past the
whole block to `0x48AFB9` in two cases:
- the owner is inactive (`[owner+0] == 0`);
- the owner's `+0x73` is neither 1 nor 2.

The block holds:
- the main-list controller `0x43B7C0` (`0x48AF98`, its only caller);
- the sub-list controller `0x43BAD0` (`0x48AF9E`, its only caller);
- the mover's step (`0x43DD20`, `0x48A870`);
- `0x489BB0` every 30 ticks and `0x41BD10` every 8 ticks.

The only other handler dispatch is the destructor's event-2 call (§1.5), which `+6` bit `0x2`
arms. So **on a type-3 owner no handler runs** once `+6 & 2` is held clear.

### 2.2 Every writer of a puppet's nodes

| path | gated on the owner's type? | what it writes on a puppet's nodes |
|---|---|---|
| the controllers, handlers, default order, announce | yes (§2.1) | nothing |
| the hit reaction `0x406F80` | `0x4897B0(victim, 0x10)` is unconditional (`0x406F8C`); `0x439EB0` only for a type-2 victim, `0x43B1F0` only for types 1 and 2 (`0x407002..0x407019`) | `+0x4E \|= 0x10` on nodes targeting the victim |
| Impure's `my_Retaliate` (more than three weapons, `tagpu_weapons.c`) | the same gates as stock | the same |
| Paralyze in the hit `0x489CE0` | yes, types 1 and 2 (`0x489E15..0x489E2D`); `0x489FA0` has the same gate and no callers | nothing |
| the unit destructor `0x4866D0` | no | `0x439EB0(unit, 1)` (`0x4867A1`) destroys the dying unit's nodes; `0x489740` (`0x486D75`) sets `+0x4E \|= 8` and `+0x16 = 0` on nodes targeting it |
| the attach `0x48AB70` → `0x4384A0` (`0x48AD1F`) | no | destroys the child's main-list nodes without flag 4, the old head armed; pushes a **BeCarried** node (`0x5050E8`, target = the carrier) before the head, copying the head's `0x4000` |
| the interface: `0x48CF30` (16 callers) → `0x43AFC0` (`0x48D1F4`); build placement `0x419670` → `0x43AFC0` (`0x4197A9`); the menu click → `0x419B00` → `0x43B0B0`; Ctrl+D `0x496440..0x49646F` → `0x439E30` + `0x439F80` | no: none tests `+0x73`; each acts on `main+0x2A42`'s selected units | anything, whenever the controlled seat owns puppet units |
| the AI `0x407E90`, `0x408100`, `0x480460` | per AI player; a type-3 seat has no AI object | nothing [INFERRED: they order their own player's units] |
| the draw path `0x439740`; Impure `resolve_sprite` | no | `+0x32`, `+0x34`, `+0x42` bit `0x200000` (the `drop` rows) |
| the mission loader `0x487D70..0x48821E`; Impure's scenario applier | — | not active in a replay [INFERRED] |
| the level teardown | no | destroys every node |

### 2.3 Readers of a puppet's nodes

- **The marker driver** `0x48CC30` runs over `main[0x2A42]`'s block, and so does Impure's
  `tagpu_order.c` port of it.
  - The walker `0x439B30` reads the **main list only** (`+0x5C`, next at `0x439CC8`).
  - It indexes the descriptors by the action (unbounded) and draws by mask.
  - `0x438C00` indexes `UnitDefs + p1·0x249` unbounded when `(u16)p1 != 0` (the engine map). Only
    MobileBuild and VTOL_MobileBuild have mask bit 0.
- **The unit panel** `0x46A860` (from `0x469615`) reads the head's status text (`0x439DF0`,
  unbounded). For a unit whose `+0xFF` equals `main+0x2A43` it also reads the head's target through
  `0x439DD0`: its `+0xA8` and `+0x108`.
- **The build menu** `0x4199B0` reads `0x439D80`, both lists, by comparison only.
- **The destructor**, reached through the unit destructor, the attach or the teardown.
- **In a mixed game after a takeover**, local handlers or AI reading a puppet's list were not
  examined (§8).

### 2.4 When recorded nodes are inert

A puppet's recorded nodes are inert data. The draw path and the panel read them safely, and
destroying them does nothing beyond unlinking and freeing, **by construction**, when every node on a
puppet satisfies all four of these:

1. **Disarmed:** `+6 & 0x2` clear, `+0x42 & 0x400000` clear, `+0x42 & 0x10000` set, `+0x52 = 0`.
   The destructor then runs only the vtable store and the reference unlink (§1.5). The death, attach
   and teardown paths all end in that destructor.
2. **Action bounded:** `action < ([0x512348] − [0x512344]) / 0x19`. The draw path, the panel and the
   destructor all index the array with it.
3. **Target through the engine:** set only by `0x43A0C0` or `0x489690` from a bounded ID, which
   refuses a dead slot (`+0xA6 = 0`).
4. **Build type bounded:** `(u16)+0x36 < main+0x1438F` on nodes whose action is MobileBuild or
   VTOL_MobileBuild, and on count nodes.

**This corrects the notes.** The completeness note ("COB on a puppet") and the evidence note ("A
puppet's order nodes are therefore written with neither bit") name `+6 & 2` and `0x400000` only.
Without `0x10000` a replayed death, an attach or any removal of the head still runs `0x489800` on
the puppet. That writes its weapon slots and starts `TargetCleared` (§1.5).

---

## 3. Recording strategy

### 3.1 Option (a): nodes as records, with an identity

- **Identity.** A node has no spare byte: its fields reach exactly `0x56`. A freed block is reused at
  once by the small-block heap. So an identity is a serial kept in a side table keyed by address,
  maintained by **detours** on:
  - both constructors, `0x43A0C0` (48 call sites, most of them in handlers) and `0x43A420`;
  - the destructor `0x43A1F0`.

  The vtable-writer inventory (§1.1) bounds the set to those three.
- **Recording.**
  - Each node is a layout record, with `next` and the unit heads as node IDs.
  - A creation is a whole record and a free is a tombstone.
  - A field change is a delta keyed by the node ID.
- **Playback.**
  - It maps each recorded ID to the node it built on the puppet.
  - A creation builds a node through the engine and links it after its recorded predecessor.
  - A tombstone tears the node down.
  - A delta writes fields.
  - Anything the engine changed on the puppet during the tick (an attach's BeCarried, a death's
    frees) must be matched to IDs, or it breaks the map.

### 3.2 Option (b): list images, reconciled

- **Capture.** Each tick, the recorder walks every recorded unit's two lists at the capture point
  and turns each node into a value record, with the target as an ID and the owned object as a class
  code. It compares the result with the slot's previous image, which it keeps in its own memory.
- **Wire.** A changed slot carries a delta against that previous image. A keyframe carries every
  non-empty image.
- **Playback.** It decodes the image into its own memory, then reconciles the puppet's engine lists
  against it, position by position:
  - where both sides have a node, it patches the node in place;
  - where the puppet has extra nodes, it tears them down;
  - where the puppet is missing nodes, it builds them.

  It never assumes the puppet still holds the previous image. So an engine write made during the
  tick (the attach's BeCarried, a death, a stray interface write) is simply reconciled away.

### 3.3 Comparison

| | (a) records with identity | (b) images, reconciled |
|---|---|---|
| **Size** | smallest for edits inside a list (one tombstone, no shift); pays an ID per node and per `next`, and a record per creation | pays a slot ID and counts per changed list; a removal at the head shifts every position (a delta with an edit script — drop the front, append — keeps the common queue cases as small as (a)); timers and states are the same XOR deltas in both |
| **Exactness** | exact per node, including identity | exact per value and per order; drops node identity, which nothing on a puppet uses: no engine structure outside the lists holds a node pointer [INFERRED: the unit record's pointer rows name none, and the mover holds the owned object, not the node] |
| **Safety** | three detours in the simulation path, a side table the engine does not bound, and a map from ID to address to keep right through engine-made changes; one missed path corrupts identity silently | no hooks; a read-only walk on the game thread; the playback holds no node pointer across a tick; every input is bounded before it becomes a pointer or an index |
| **Capture cost** | only on change, but inside every construction and destruction | a walk of all nodes per tick (thousands: cheap) and a memcmp per slot |
| **Takeover** | the puppet's nodes can be armed in place | the same: the puppet's nodes are value-exact, and arming them in place, or rebuilding them, follows experiment 2's MEASURED recipe (§3.7) |
| **Seek and rewind** | a keyframe must also rebuild the ID map | a keyframe is the images; reconcile rebuilds the lists |
| **The engine's own model** | — | matches the saver and restore (§1.3): a list is a sequence of value records, rebuilt fresh |

### 3.4 Recommendation: (b)

It is safe by construction with no engine patch. It follows the MEASURED rule that a rebuild writes
values, never engine pointers. It survives engine-made changes on the puppet by reconciling rather
than predicting them. And it already gives a takeover everything that (a) would. The one thing it
gives up, node identity, is used by nothing that runs on a puppet.

### 3.5 Capture recipe

On the game thread at the S1 capture point (B2's stub at `0x4954ED`), after every writer of the
tick. All the list writers in §2.2 are game-thread code.

```
for each unit slot S1 records:
  for L in (main = unit+0x5C, sub = unit+0x60):
    walk from the head, at most CAP nodes           # longer: a named failure; never truncate silently
    for each node n:
      assert n+0xE == unit                          # named failure otherwise
      note (n+0x42 & 0x40000) != (L is sub)         # counted anomaly (a re-typed node, §1.8)
      target = n+0x16 ? (n+0x16 - units)/0x118 + 1 : 0
               # the pointer must lie in the array on a record boundary, else a named failure
      owned  = n+0x52 ? code_of_vtable(*(n+0x52)) : 0
               # the five-entry table of §1.7; an unknown vtable is a named failure
      record: +0x04 action, +0x05 state, +0x06 events, +0x0A deadline, target,
              +0x22 position (12), +0x2E leash (4), +0x36 p1..p3 (12),
              +0x42 flags & ~0x200000, +0x46 issue, +0x4E fired, owned
  if the image differs from the slot's previous image: emit its delta; keep the new image
```

- **Size.** About 55 bytes per node with a 4-byte ID.
- **Constant fields.** The two vtables (`0x4FD2C8`, `0x4FD754`) are asserted, not sent.
- **Lifetime.** The walk reads engine memory only while no engine code runs. That is the same
  quiescence S1 relies on.

### 3.6 Playback recipe

Per tick, on the game thread, in this order:
1. The tick's replayed events (deaths, attaches, hits).
2. The S1 unit and player records.
3. The reconcile below.
4. The draw.

```
for each unit slot u of the recording (every slot: O(nodes), no bookkeeping of which changed):
  bound u: 1 <= u <= slots; a unit S1 shows dead must have an empty image (named failure otherwise)
  for L in (main, sub):
    img = the decoded image of L        # recorder memory
    prev = &head(L); n = *prev; i = 0
    while n and i < len(img): patch(n, img[i]); prev = &n->next(+0x4A); n = *prev; i++
    while n: nx = n->next; *prev = nx; teardown(n); n = nx
    while i < len(img): build(u, L, img[i]); i++
```

- **`build(u, L, r)`**:
  1. `node = 0x4B4F10(0x56)` (MEM_Alloc, cdecl).
  2. `0x43A0C0(node; r.action, ptr(r.target), &r.pos, r.p1, r.p2, r.p3)`. The constructor links the
     reference.
  3. Write `+5 = r.state`, `+6 = r.events & ~0x2`, `+0xA = r.deadline`, `+0x2E/+0x30 = r.leash`,
     `+0x46 = r.issue`, `+0x4E = r.fired`, and
     `+0x42 = (r.flags & ~(0x400000 | 0x200000)) | 0x10000`.
  4. Link it:
     - use `0x43AD10(u, node)` when the recorded `0x40000` matches L, writing the flags with L's
       `0x40000` first;
     - otherwise link it by hand at L's tail and restore the recorded flags afterwards (the anomaly
       case of §3.5).
- **`patch(n, r)`**:
  - Write any value field that differs, keeping the disarm rule.
  - When the target differs, call `0x489690` on `n+0x12` with `ptr(r.target)`.
  - When the target or the action changes, clear `0x200000`, as a fresh node would have it.
  - Never touch `+0x32` or `+0x34`.
- **`teardown(n)`**:
  1. `n+6 &= ~0x2`; `n+0x42 = (n+0x42 & ~0x400000) | 0x10000`.
  2. `n+0x52` must be 0. Otherwise it is a named one-writer failure, and the engine's destructor
     then deletes the object through its own path.
  3. `0x43A1F0(n)` (thiscall), then `0x4B4F20(n)` (MEM_Free, cdecl). The unlink is the pointer store
     in the loop.
- **Afterwards:** the harness check compares each puppet list with its image, masking the `drop`
  and `armed` rows.

**Why it is safe by construction:**
- **Bounds:**
  - `u` against the slots.
  - `r.action` against the descriptor count (§1.2); every draw reader indexes the array with it.
  - `r.target`: 0, or an ID within the slots whose unit is alive, and it becomes a pointer only
    through the engine's `0x43A0C0` or `0x489690`, which refuse `+0xA6 = 0`.
  - `(u16)r.p1 < main+0x1438F` on count nodes and on MobileBuild and VTOL_MobileBuild, which
    `0x438C00` indexes unbounded.
  - `len(img) <= CAP`.

  A value that fails its bound makes the node's field 0, or drops the node, and the harness names
  it. No value becomes a wild pointer.
- **Lifetime:** no node pointer is kept from one tick to the next, and every walk starts from the
  heads. The engine may free nodes during the event replay: the attach's wipe, a death.
- **Ordering:** one thread runs events, then records, then the reconcile, then the draw. The render
  thread reads only `tagpu_order.c`'s snapshot, never an engine node.
- **Disarm:** every node the playback builds, patches or tears down satisfies §2.4. The only armed
  node a puppet can hold is the BeCarried an attach just built. The same tick's reconcile disarms it.
  If the same tick's death destroys it first, the puppet repeats exactly what the live machine did
  with its own twin node.

### 3.7 Takeover recipe (a later landing)

Arm the taken-over unit's nodes in place, after the reconcile:
1. Restore `+6 & 0x2` and `+0x42 & 0x10000` from the image.
2. Restore `+0x42 & 0x400000` from the image **only if** the unit's COB state is restored as
   recorded. Otherwise clear it, as experiment 2 did with a fresh COB. A `StartBuilding` that never
   ran here must not get a `StopBuilding`.
3. Keep `+0x52 = 0`. The handler builds its own owned object when it next runs (MEASURED: a copied
   pointer faulted).
4. For nodes whose target is a nanoframe (BuildingBuild or MobileBuild with `+0x16` set), set state
   `+5 = 3`. This is the MEASURED adoption.
5. Then flip the seat's controller (the other track).

Not measured: resuming any other handler in mid-state, for example an air attack whose state
expects an owned object. The conservative alternative resets every other node's `+5` to 0, which
restarts the order. This is the owner's decision (§9).

---

## 4. Factory queues and rally points

**Where they live** [DISASSEMBLED; the queue mechanics are the evidence note's T12–T14]:
- **The queue.** A factory's queue is count nodes on its main list:
  - BuildingBuild (12), with `+0x36` the type and `+0x3A` the count;
  - flag `0x100` from the descriptor.

  `0x43B0B0(action, unit, type, amount)` adds to the last node of the same type or appends a new
  one, so A, B, A is three nodes. The factory handler `0x402640` builds while the head's count is
  positive.
- **The unit in progress** is the head's `+0x16`.
- **A mobile builder's sites** are MobileBuild (25) and VTOL_MobileBuild (54) nodes. They are also
  `0x100` count nodes.
- **A stockpile's queue** is BuildWeapon (13) count nodes on the **sub** list (flags `0xC0140`),
  with `p1 = 0`.
- **The rally** is QMove (30) / QPatrol (31) nodes on the factory's main list (the evidence note).
  Their status text is `'Ready with orders'`.

**The rally's inheritance (new; closes the notes' T15)** [DISASSEMBLED, `0x402DA0..0x402F0B`]:
- It happens in the **new unit's GetBuilt handler**, not in `0x41B8D0`.
- When the unit is complete (`+0x104 == 0.0`, `0x402DAB..0x402DBC`) and has a mover (`0x402DC8`),
  the handler walks the builder's (`node+0x16`) main list (`0x402E01..0x402E6D`).
- For each QMove (`'QMove'`, `0x5010A4`) or QPatrol (`'QPatrol'`, `0x50109C`) node:
  - it translates the node through `0x43F0E0` (kind 2 or kind 9) at the node's `+0x22`;
  - it queues the result on the new unit with `0x43ADC0(action, 1, unit, 0, &pos, 0, 0)`
    (`0x402E5E`).
- It then copies the builder's `+0x110` bits `0xC0000` and `0x300000` [INFERRED: move and fire
  state].
- For a local human owner (type 1) it also copies the builder's `+0xAC` [the control group].
- With no rally it queues `'PARK'` (`0x501358`, `0x402EFA`).
- It answers 5, so the GetBuilt node is destroyed.
- For the recorder this is a handler effect on the new unit: its list arrives as an image. For a
  takeover it runs on the new unit's machine.

**How the engine draws a factory's queue** [DISASSEMBLED]:
1. **The build menu, `0x4199B0`.**
   - It is called from `0x41A490`, `0x41ACE0`, `0x41B8D0` (which BuildingBuild's cleanup calls) and
     `0x41C150`.
   - It walks the menu's buttons, stride `0x15B`.
   - A unit button gets `"+%d"` (`0x502660`) of `0x439D80(unit, type)`, with the type resolved by
     `0x488B10` from the button's name.
   - The stockpile button gets `"%d"` (`0x50266C`) of the byte `unit+0x1E` [INFERRED: the stockpile
     count], then `" +%d"` (`0x502664`) of `0x439D80(unit, 0)`.
   - It then redraws through `0x49FA90`.
2. **The unit panel, `0x46A860`.**
   - The status line is the head's text (`0x46AC19`).
   - When the unit's `+0xFF` is `main+0x2A43`, it also shows the head's target's `+0xA8` and
     `+0x108` (`0x46ACA1`): the unit in progress.
   - It redraws only when its `0x3C`-byte summary differs from `main+0x37E60`
     (`0x46ACC8..0x46ACDD`).
3. **The order markers.**
   - The driver `0x48CC30` gives a selected unit of `main[0x2A42]` mask `0x1F`, flag 0 (the engine
     map).
   - The walker draws the main list only:
     - BuildingBuild's mask is 0, so **queue nodes draw nothing**;
     - QMove and QPatrol (mask `0x02`, cursors 14 and 7) draw the **rally sprite** at `+0x22`;
       route dots come only for the camera, tracked or hovered unit (flag 1);
     - MobileBuild (`0x13`) draws the footprint (`0x438C00`), dots and ranges.
   - The sub list (the stockpile queue) never draws.

**On a puppet host** the menu and markers follow `main+0x2A42`, while the panel's target line
follows `main+0x2A43`. So whether a watched factory's queue shows depends on the viewer's seat.
That is a puppet-host decision (§7, §9).

---

## 5. Churn (qualitative; nothing measured)

**Creations and frees** [DISASSEMBLED sources, INFERRED weights]:
- **The default order.**
  - When a type-1/2 owner's main list empties and def `+0x230` (the default action) is non-zero, the
    controller builds one and inserts it at the head (`0x43B9DE..0x43BA30`).
  - Standby (41) sleeps 30 + `0x4B6C30(30)` ticks between wakes (`0x406022..0x40603A`). On each wake
    it searches through `0x43B700` (only when `+0x110 & 0x300000` is `0x200000` [INFERRED: the fire
    state]).
  - When a search finds a target, `0x43B1F0` pushes a **pair**, the leash return and the attack
    (+2), and Standby answers 5 (−1).
  - Later the attack and the return complete (−2) and the default returns (+1).
  - **An engagement is therefore about 3 creations and 3 frees per unit.**
  - Standby_Mine (42) instead pushes a SelfDestruct (`0x4060D8`).
- **Hit reactions** (`0x406F80` for types 1 and 2): +2 per reaction, where the head is empty or
  Standby.
- **Commands.**
  - The player (`0x48CF30`), through `0x43ADC0`, destroys k nodes and adds 1 per selected unit per
    click. With SHIFT it adds 1 or cancels 1.
  - The AI's `0x43ADC0` calls (`0x408064`, `0x4082B0`, `0x408421`, `0x408453`, `0x4085A1`,
    `0x4804BB`) do the same.
- **Builds.**
  - Each new frame gets a GetBuilt node (+1: `0x4029A5`, `0x403DC5`, `0x4140F3`), freed at
    completion (−1).
  - The new unit gets one order per rally node, or PARK.
  - Count nodes change `+0x3A` per click and per completion, and are freed at zero.
- **Other creations:**
  - Patrol adds a return point (`0x43A020`, +1).
  - A transport load wipes and adds a BeCarried (`0x4384A0`, −k +1).
  - A paralyzing hit adds a Paralyze node (+1, types 1 and 2).
- **Deaths:** the whole list goes (`0x439EB0`), and every node targeting the dead unit gets
  `+0x4E \|= 8` and `+0x16 = 0`.

**Field churn with no creation:**
- `+0xA` and `+6` on every wake;
- `+5` on every state step;
- `+0x4E` from notices;
- `+0x3A`/`+0x3E` progress, and Reclaim's countdown in `+0x36`;
- MobileBuild's site snap to `+0x22`.

**Order of magnitude** [INFERRED from the notes' MEASURED battle: 1,432 units, 60 s, 84–130 deaths,
about 1,700 shots]. The estimate assumes 30 ticks a second.

| source | per tick |
|---|---|
| idle wakes, about 1,000 idle units ÷ ~45 ticks | ~20 |
| deaths | ~0.1–0.2 |
| hits | ~1, each notifying a few nodes |
| engagements | bursty, tens at a battle's start |

Together that is **roughly 20–100 changed lists per tick**, at a few bytes to about 60 bytes each:
**about 0.5–5 KB per tick** before generic compression.

**To measure it**, have S1's capture count changed images and nodes per tick in the harness's
battle.

---

## 6. What corrects or extends the notes

1. **Correction, the "neither bit" rule.** The completeness note ("COB on a puppet") and the evidence
   note (§ factory queues) say a puppet's nodes need only `+6 & 2` and `0x400000` clear. They also
   need `+0x42 & 0x10000` **set**. Otherwise the head's destruction runs `0x489800(owner, 3)`:
   weapon slots and `TargetCleared` (`0x43A2AB..0x43A2B9`).
2. **Extension, who sets `+6 & 2`.** The engine map says the five build handlers "are the only
   handlers that set that bit". That holds among the emitter handlers. VTOL_Pickup (`0x411464`),
   AirStrike (`0x412526`, `0x4125BB`) and AirToGround (`0x412CE3`) also set it, each right after
   installing an owned object.
3. **Omission, the `orders` row.** `replay-layout.tsv`'s `unit 0x05C orders` omits `0x4384A0`, which
   writes `unit+0x5C` in two places: its wipe unlinks the head (`0x4384B6`/`0x4384D3`) and it inserts
   BeCarried before the head (`0x438562`). The `suborders` row lists it, yet its insert takes the
   sub list only for a `0x40000` node, and BeCarried's flags are `0x24`.
4. **Closed, rally inheritance (T15).** GetBuilt's handler `0x402DA0` does it (§4).
5. **New node facts:**
   - storage: one block each, 54 allocation sites, two constructors by the vtable inventory;
   - the leash origin at `+0x2E/+0x30`;
   - the issue time at `+0x46`, which is not saved;
   - `+0xA = −1` at construction;
   - the flags table (§1.8);
   - the owned object's five classes and vtables (§1.7);
   - `0x439F80` compares with the **main** head even for a sub-list node;
   - `0x43B730`, `0x43AD50` and `0x439FE0` have no callers;
   - the descriptor record's `+0`, `+0x15` and the lookup's bound (§1.2).
6. **New drawing facts:** the build menu's counts (`0x4199B0`, with its strings) and the panel's
   head reads (`0x46A860`).
7. **The unit tick's owner gate also skips** the mover's step, the 30-tick `0x489BB0` and the 8-tick
   `0x41BD10` for a type-3 owner (§2.1).

---

## 7. Notes for the other S2 tracks

- **Movers.**
  - The owner gate `0x48AEB5..0x48AECD` skips the mover's step (`0x43DD20`, `0x48A870`) on a type-3
    owner.
  - The mover's class holds the current owned object at `[[unit+0]+0]+4`. The restore hands it the
    head's through `0x4388B0`.
  - A puppet's nodes, with `+0x52 = 0`, never touch it (§1.5).
  - Class 2 carries a unit reference, and the `0x2C` receiver builds one through `0x44E080`.
- **Events.**
  - `TargetCleared` is started locally through `0x4B0A70` by `0x489800` and by `0x48A0F0` (the
    controller's event `0x10000`, `0x43B856`). No broadcaster sees these starts.
  - `StopBuilding` from the destructor *is* broadcast (`0x456190`).
  - Whether COB state recording covers the first is the events track's question.
  - Replayed hits and deaths notify nodes (§1.6). The reconcile's ordering makes those notices
    harmless.
- **The puppet host.**
  - The interface's order writers act on `main+0x2A42`'s selection with no owner-type test (§2.2).
  - The markers follow `+0x2A42` and the panel's target line `+0x2A43` (§4).
  - Reconcile makes a stray interface write a one-tick glitch rather than a divergence: disarmed
    nodes have no destruction side effects. The seat design should still keep the viewer's input
    off puppet units.

---

## 8. Open questions

1. **Churn numbers:** nothing was run (§5 gives the measuring recipe).
2. **The parameter audit:** whether any of the 68 handlers keeps a pointer in `p1..p3` (sampled
   only; §1.9).
3. **The undecoded descriptor bits:** the meanings of `+0x42` bits `0x2`, `0x8`, `0x10`, `0x80`,
   `0x800` and `0x1000000`. They are plain values, so recording them needs no decoding.
4. **`+6 & 2` through a register:** only immediate stores were scanned (§1.10). Stores through a
   register were not audited. The disarm rule does not depend on knowing every setter.
5. **The takeover's handler state:** resuming a handler mid-state, other than frame adoption, is not
   measured (§3.7).
6. **Cross-unit readers:** readers of *another* unit's list from local handlers or the AI, once live
   and puppet units coexist after a takeover.
7. **Node pointers outside the lists:** whether any engine structure other than the lists holds a
   node pointer. [INFERRED none]
8. **The list cap:** its value, for the capture and the playback. The engine has none; Impure's
   drawer uses 1024 per list.

## 9. The decisions

Taken 2026-09-30: items 1, 2, 4, 5, 6 and 10 as recommended, and item 3 as recommended under the names `puppet0` and `puppet1`, all implemented by S2a; item 7 is S2b's applier; item 9's first half is S2b's viewer seat, its markers S5's; item 8 waits for the takeover (S4).


1. **Recording strategy.** Recommend **(b)**, list images with a reconciling playback (§3.4).
2. **A third record kind** `order` (stride `0x56`) in `replay-layout.tsv` and the generator.
   Recommend **yes**, so that the harness names node-field differences by row.
3. **The disarm classes** `puppet0` / `puppet1` for these four fields:
   - `+6 & 0x2`;
   - `+0x42 & 0x400000`;
   - `+0x42 & 0x10000`;
   - `+0x52`.

   Recommend **new classes**, a visible loosening, over undocumented exceptions in the order
   builder.
4. **`drop` for the last-seen cache,** `+0x32`, `+0x34` and `+0x42 & 0x200000`. Recommend **yes**:
   the cache is per viewing frame.
5. **The owned object in the recording.** Recommend the **class code only in S2**. Its full state
   waits for forks, since the takeover recipe does not need it.
6. **The list cap.** Recommend **4096 nodes per list**, with a named failure beyond it and the
   recording marked broken from that tick, never truncated.
7. **The reconcile scope.** Recommend **every slot every tick**, which is O(nodes) with no
   bookkeeping, over changed slots plus the slots that events touched.
8. **The takeover's node state.** Recommend **keeping the recorded `+5`**, with frames at 3 as
   MEASURED, and measuring the air handlers before relying on it. The alternative is resetting
   every state to 0.
9. **The viewer's seat** (puppet-host track). Recommend keeping `main+0x2A42` on a seat that owns no
   units, and having `tagpu_order.c` draw the viewed player's (`+0x2A43`) markers.
10. **The `orders` row's writers.** Add `0x4384A0` (§6.3). This is a correction, not a loosening.

---

## Appendix A: the 68 descriptors

Rank = the action byte; handler `desc+4`; mask `desc+0xC`; flags `desc+0x11`; status text `desc+0`.
Dumped from the pristine executable's four static tables and sorted by name, as `0x43C050` does.

| rank | name | handler | mask | flags | status |
|---|---|---|---|---|---|
| 0 | (Ready, no name) | `0x439EA0` | `0x00` | `0x0` | Ready |
| 1 | Activate | `0x403010` | `0x00` | `0x10060` | Activate |
| 2 | AirStrike | `0x411F50` | `0x08` | `0x600` | Airstrike |
| 3 | AirToAir | `0x412D40` | `0x08` | `0x200` | Engaging target |
| 4 | AirToGround | `0x412710` | `0x08` | `0x200` | Engaging target |
| 5 | AirToGroundHover | `0x413470` | `0x08` | `0x200` | Engaging target |
| 6 | Attack_Chase | `0x4034A0` | `0x08` | `0x280` | Attacking |
| 7 | Attack_Kamikaze | `0x403260` | `0x08` | `0x600` | Attacking |
| 8 | Attack_NoMove | `0x402160` | `0x08` | `0x280` | Attacking |
| 9 | AttackSpecial | `0x403190` | `0x08` | `0x680` | Annihilating |
| 10 | AttackUType | `0x401E00` | `0x00` | `0x4` | Attacking |
| 11 | BeCarried | `0x402FC0` | `0x00` | `0x24` | Being transported |
| 12 | BuildingBuild | `0x402640` | `0x00` | `0x10010C` | Nanolathing |
| 13 | BuildWeapon | `0x402B70` | `0x00` | `0xC0140` | Nanolathing |
| 14 | Capture | `0x404270` | `0x08` | `0x200` | Capturing |
| 15 | Cloak_Off | `0x4030A0` | `0x00` | `0x10060` | Decloaking |
| 16 | Cloak_On | `0x403070` | `0x00` | `0x10060` | Cloaking |
| 17 | Deactivate | `0x403040` | `0x00` | `0x10060` | Deactivate |
| 18 | Follow_Ground | `0x406300` | `0x12` | `0x200` | Guarding |
| 19 | GetBuilt | `0x402DA0` | `0x00` | `0x224` | Under construction |
| 20 | Ground_Pickup | `0x406780` | `0x08` | `0x200` | Loading |
| 21 | Ground_Unload | `0x406900` | `0x08` | `0x400` | Unloading |
| 22 | Guard_NoMove | `0x4021F0` | `0x00` | `0x20` | Ready |
| 23 | HelpBuild | `0x403F70` | `0x18` | `0x100208` | Nanolathing |
| 24 | MakeSelectable | `0x401CC0` | `0x00` | `0x4` | Unit is available |
| 25 | MobileBuild | `0x403A20` | `0x13` | `0x100508` | Nanolathing |
| 26 | Move_Ground | `0x4031D0` | `0x12` | `0x402` | Moving |
| 27 | Paralyze | `0x402D10` | `0x00` | `0x24` | Paralyzed |
| 28 | Park | `0x4061A0` | `0x00` | `0x0` | Parking |
| 29 | Patrol | `0x4033A0` | `0x12` | `0x412` | Patrolling |
| 30 | QMove | `0x403160` | `0x02` | `0x400` | Ready with orders |
| 31 | QPatrol | `0x403160` | `0x02` | `0x400` | Ready with orders |
| 32 | Reclaim | `0x404AD0` | `0x12` | `0x100800` | Reclaiming |
| 33 | ReclaimUnit | `0x404730` | `0x12` | `0x100200` | Reclaiming |
| 34 | RepairPatrol | `0x405980` | `0x12` | `0x412` | Repair patrol |
| 35 | RepairUnit | `0x405300` | `0x12` | `0x100200` | Repairing |
| 36 | RepairUnitNoMove | `0x405740` | `0x18` | `0x200` | Repairing |
| 37 | Resurrect | `0x404DB0` | `0x12` | `0x200` | Resurrecting |
| 38 | SelfDestruct | `0x402010` | `0x00` | `0x40040` | SELF DESTRUCT ENGAGED |
| 39 | SelfDestructFG | `0x402010` | `0x00` | `0x0` | SELF DESTRUCT ENGAGED |
| 40 | SelfRepair | `0x402430` | `0x00` | `0x1000204` | Repairing |
| 41 | Standby | `0x405FE0` | `0x10` | `0x20000` | Standby |
| 42 | Standby_Mine | `0x406090` | `0x10` | `0x1020000` | Standby |
| 43 | Standing_FireOrder | `0x403100` | `0x00` | `0x10060` | Acknowledged |
| 44 | Standing_MoveOrder | `0x4030D0` | `0x00` | `0x10060` | Acknowledged |
| 45 | Stop | `0x401C20` | `0x00` | `0x0` | Stopping |
| 46 | Suppress | `0x4038A0` | `0x08` | `0x410` | Suppressing fire |
| 47 | Teleport | `0x406AA0` | `0x08` | `0x600` | Teleporting |
| 48 | VTOL_Evade | `0x413BC0` | `0x00` | `0x0` | Evading |
| 49 | VTOL_Follow | `0x40FBE0` | `0x02` | `0x200` | Guarding |
| 50 | VTOL_GetRepaired | `0x415250` | `0x00` | `0x200` | Under repair |
| 51 | VTOL_HelpBuild | `0x414380` | `0x08` | `0x100208` | Nanolathing |
| 52 | VTOL_LandIfCan | `0x40F2A0` | `0x00` | `0x400` | Seeking to land |
| 53 | VTOL_Landing | `0x4118E0` | `0x08` | `0x600` | Landing |
| 54 | VTOL_MobileBuild | `0x413D80` | `0x03` | `0x100508` | Nanolathing |
| 55 | VTOL_Move | `0x40FA20` | `0x02` | `0x402` | Moving |
| 56 | VTOL_Patrol | `0x410E70` | `0x02` | `0x412` | Patrolling |
| 57 | VTOL_Pickup | `0x4111B0` | `0x08` | `0x200` | Loading |
| 58 | VTOL_Reclaim | `0x414770` | `0x02` | `0x100800` | Reclaiming |
| 59 | VTOL_ReclaimUnit | `0x414A80` | `0x02` | `0x100200` | Reclaiming |
| 60 | VTOL_RepairPatrol | `0x4152F0` | `0x02` | `0x412` | Repair patrol |
| 61 | VTOL_RepairUnit | `0x414E70` | `0x02` | `0x100200` | Repairing |
| 62 | VTOL_SeekAttack | `0x4103E0` | `0x00` | `0x600` | Seeking to attack |
| 63 | VTOL_SeekGuard | `0x410850` | `0x00` | `0x600` | Seeking to guard |
| 64 | VTOL_Standby | `0x40F7D0` | `0x00` | `0x20000` | Standby |
| 65 | VTOL_Unload | `0x411560` | `0x08` | `0x400` | Unloading |
| 66 | Wait | `0x401CE0` | `0x00` | `0x4` | Waiting |
| 67 | WaitForAttack | `0x401FD0` | `0x00` | `0x204` | Waiting for attack |

## Appendix B: the 54 node allocation sites (`push 0x56` ... `call 0x4B4F10`)

Grouped by the function that holds them. The handlers are named by their descriptor.

- Handlers:
  - Stop `0x401C20`: `0x401C67`
  - AttackUType `0x401E00`: `0x401F34`
  - Attack_Kamikaze `0x403260`: `0x4032C7`
  - Resurrect `0x404DB0`: `0x405272`
  - RepairPatrol `0x405980`: `0x405BD3`, `0x405C58`, `0x405CA2`, `0x405CE9`
  - Standby_Mine `0x406090`: `0x4060D8`
  - Follow_Ground `0x406300`: `0x40650A`, `0x40664A`
  - VTOL_Standby `0x40F7D0`: `0x40F8F9`
  - VTOL_Follow `0x40FBE0`: `0x40FE9B`, `0x4100D6`, `0x410348`
  - VTOL_SeekAttack `0x4103E0`: `0x41059D`
  - VTOL_SeekGuard `0x410850`: `0x4109F7`, `0x410ADE`
  - VTOL_Patrol `0x410E70`: `0x410FE7`
  - VTOL_Landing `0x4118E0`: `0x411EAF`
  - AirStrike `0x411F50`: `0x411F8A`, `0x412008`, `0x41265F`
  - AirToGround `0x412710`: `0x41274E`, `0x4127B4`, `0x412BE1`
  - AirToAir `0x412D40`: `0x412D7A`, `0x412DF6`, `0x413337`
  - AirToGroundHover `0x413470`: `0x4134A5`, `0x41351F`, `0x413B27`
  - VTOL_RepairPatrol `0x4152F0`: `0x415420`, `0x4155B7`, `0x41568D`, `0x4156DA`, `0x41571B`,
    `0x41575C`
- The order module:
  - the attach's wipe `0x4384A0`: `0x4384F8`
  - the patrol return `0x43A020`: `0x43A054`
  - the command `0x43ADC0`: `0x43ADC4` (this site covers every GetBuilt, rally and command node)
  - the reaction pair `0x43B1F0`: `0x43B28A`, `0x43B2FB`, `0x43B37B`
  - the reaction pair `0x43B400`: `0x43B456`, `0x43B4C7`, `0x43B588`, `0x43B5F9`, `0x43B67C`
  - `0x43B730` (no callers): `0x43B732`
  - the default order in `0x43B7C0`: `0x43B9DE`
- Elsewhere:
  - the restore `0x487080`: `0x48757A`, built by `0x43A420`
  - Paralyze in the hit `0x489CE0`: `0x489E72`
  - Paralyze in `0x489FA0` (no callers): `0x48A01B`

Every site but the restore's is followed by a call of `0x43A0C0`: there are 48 such calls, since
some sites share a call. The restore's is followed by `0x43A420`.
