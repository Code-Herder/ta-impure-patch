# Entering the game

Where ImpAI plugs into TotalA.exe, which engine facts the plan rests on, and what is still undocumented. Every address is cited from the engine map with how it was established; the seam, the exemption and the button are designs, nothing here has run.

## The seam: the call at `0x465031`

From [Orders and the AI](../engine/orders.html), *The AI object, its lifecycle and its ten planners* (DISASSEMBLED):

- The players phase `0x464F80` calls the AI tick `0x408C40` from one instruction, `0x465031` (`E8 0A 3C FA FF`), and only when the record's AI object `+0x74` is set (`0x46502D`). Nothing else calls the tick or holds a pointer to it. At the call `ECX` is the AI object, `EDI` the record and `ESI` the record's slot, below 10.
- `0x408C40` runs for an active type-2 record the group assignment `0x408830` every 30 calls, the ten planner slots when due, then the weapon target scan `0x4089A0(obj, 1)`; any other record only `0x4089A0(obj, 0)`. Human seats have an AI object too, for the scan.
- The AI object lives from the player init `0x464700` to the teardown `0x464A00`. A replay puppet (active type 3) has none, so nothing reached from `0x465031` runs on a puppet.

**ImpAI retargets the call at `0x465031`**, not the entry of `0x408C40`: that entry's five bytes are the recorder's AI activity observer, and `0x4089A0`'s entry is the extra-weapons module's acquisition detour ([the hook map](../modules/hook-map.html)). A call-site redirect composes with both. The wrapper, thiscall-compatible with a plain `ret`:

1. `rec = *(obj+0)`. **Bound**: `rec` must be one of the ten player records, `main+0x1B63 + i·0x14B` with i below 10, checked by exact division. Otherwise it calls stock `0x408C40` unchanged.
2. When `[rec]` ≠ 0, `rec+0x73` = 2 and the `SIM_STATE` table marks slot i as ImpAI, it runs the brain and then the stock weapon scan `0x4089A0(obj, 1)`, by address, so the extra-weapons port still scans. Otherwise it calls `0x408C40`, which the recorder's observer still sees.

**Why it is safe by construction:**

- **A bound**: the record is one of the ten, by arithmetic, never by a probe.
- **An ordering**: the players phase runs after the unit tick `0x48AD30` has returned, so no order list is being walked when a root links or wipes nodes; the one call on the loader thread (`0x497C10`, at the end of a level load) precedes the loader's done bit, which the game thread waits for before its first tick ([sim](../engine/sim.html)).
- **A lifetime**: the wrapper is entered only from `0x465031`, guarded by `+0x74` ≠ 0, and the AI object exists from `0x464700` to `0x464A00`. ImpAI's per-seat state is keyed by the slot and lives in the same window: made at the level init (after `0x464990`), dropped at the teardown.

## The order roots

From [Orders and the AI](../engine/orders.html), *Calling the order roots from the AI's tick* and *What each resolver mode answers* (DISASSEMBLED). ImpAI calls only these:

- **`0x438760`** resolves the action bytes ImpAI needs by name, once: the table is fixed for the process.
- **`0x43F0E0` then `0x43ADC0`** for move (mode 2), attack a unit or the ground (3), guard (7), repair and assist (8), patrol (9), stop (10) and build at a site (14). ImpAI tests the resolver's byte and issues nothing on 0: a 0 passed on is `Ready`, which without Shift wipes the unit's queue first. Mode 8 has no alliance test, so ImpAI checks the alliance itself.
- **A feature's reclaim by name** (`RECLAIM` / `VTOL_RECLAIM` through `0x438760`, then `0x43ADC0`), never mode 12: mode 12's map test reads the explored map of the *viewed* player, not the unit's owner's. ImpAI checks `canreclamate`, the feature's `reclaimable` bit and that the feature is in its own sight.
- **`0x43B0B0(BUILDINGBUILD, unit, type, count)`** for factory queues, the type bounded below the def count.
- **Not** `0x480460` (the group order, which never tests the resolver), `0x43AFC0` (its Shift toggles a queued node) or `0x48CF30` (it walks the human's selection).

**What each root call rests on:** no root validates its unit, so ImpAI passes `array + ID·0x118` with the ID bounded by `u16 main+0x14351` and the slot inside the unit array, and the unit live. A `0x43ADC0` without Shift can run handlers through the node destructor, so no order-node pointer survives a call (ImpAI keeps none) and every unit pointer is re-derived from its ID after each call. A target, once linked, is held by the engine's own reference chain, so a target that dies later is the engine's normal case.

## Difficulty and the seat

From [the GUI](../engine/ui.html), *The SKIRMISH screen's handler*, and [sim](../engine/sim.html), *Difficulty and the AI handicap* (DISASSEMBLED):

- **Difficulty is one global**, `main+0x37EEE` (0 Easy, 1 Medium, 2 Hard), set by the `SKIRMISH.GUI` gadget `Difficulty` together with the settings' `+0x228` and kept in the registry as `SkirmishDifficulty`. A network game sets it to 2 (`0x448A0B`).
- **It scales type-2 players' resources at seventeen sites**: the economy pass's eight incomes, the build-cancel refund, the unbuild refund, the reclaim payout's energy and metal, both transfers, the Deathmatch respawn's energy and metal grants, and the credit for a reclaimed unit. Easy × 0.5, Medium × 0.7, and **Hard applies no scaling at any site**. Each site tests the record for type 2 with one instruction pair while the record is in a register.
- **A seat's controller** is the record's byte `+0x73` (1 human, 2 AI, 3 remote), written at the seat's activation by `0x464290` from the skirmish entry and in play through `0x463C60`.
- **The AI profile depends on difficulty**: the profile command `plan` gates the `weight` and `limit` lines by `easy`, `medium`, `hard` or `any`. The profile is `ai\<aiprofile>.txt` (the map's key) or `ai\default.txt`, loaded by `0x4648E0`. Whether the shipped profiles use `plan` was not checked.

**The handicap exemption (A1):** each of the seventeen type tests (`cmp byte [rec+0x73],2` and its `jne`, 5 to 10 bytes) becomes a 5-byte `jmp` to a stub of its own, byte-matched at attach and all-or-none. The stub finds the slot from `rec` by the seam's bound and, for an ImpAI seat, jumps to the unscaled branch; otherwise it runs the original compare and branch. The stub calls no C, because one or two values are live on the x87 stack at every site. The table it reads is written only at the level init, before the first tick, and from the recording on a replay. Writing `main+0x37EEE` around calls instead is rejected: the sites are reached from the economy pass, transfers (including the network's), reclaim, building and unit death, and no one bracket covers them.

## Not documented yet

- **Who reads each key** in the [key inventory](../engine/keys.html): which UnitDef and weapon-record fields the simulation consumes, and how. The inventory lists what the parsers read; the readers are open.
- **The stock AI's helpers**: its build choice, site choice, base point and the count it compares with 5 (`0x40BDB0`, `0x40BFE0`, `0x40BA80`, `0x40C230`), the force planners' rebalance `0x407560`, `0x464AD0`, and the per-seat acquisition record `0x5119C0[seat]` — which of them draw the simulation RNG, and whether any tests sight.
- **Loose ends**: where the loader's done bit `main+0x38D75` bit 1 is cleared again; what `0x426A66`'s three-way switch of the Difficulty is; that `0x448A0B` is the battle room's setup and whether anything else sets a network game's Difficulty; where a new unit's standing states (`+0x110` bits 18–21) are first written, and whether from `standingmoveorder` / `standingfireorder`; whether the shipped `ai\*.txt` use `plan`.
- **Network games (A4)**: which peer runs a computer seat's tick, what that seat's record looks like on the host and on a client, and how its orders reach the other peers.

## Constraints from the repository

- **Simulation state, declared.** ImpAI changes what units do, so its files are marked `sim` and its state is `SIM_STATE`. It keeps unit IDs, never addresses.
- **Deterministic by construction.** No wall clock. Its own seeded RNG lives in its state, and it never draws the simulation RNG `0x4B6C30`: the stock planners draw it, so an ImpAI seat's stream already differs from an OTA seat's, and ImpAI must not add draws of its own. Its work per tick is bounded by counts.
- **Bounded reads.** Every engine value the adapter reads is bounded as data first: a unit index against the engine's count, a def ID against `UNITINFOCount`.
- **Game thread only.** The threat overlay reaches the render thread through the frame packet.
- **Activity attribution.** The recorder's AI activity harness brackets `0x408C40`; an ImpAI seat's orders are attributed to it only when the wrapper brackets itself the same way.
