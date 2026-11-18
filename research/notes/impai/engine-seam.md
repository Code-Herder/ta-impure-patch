# Entering the game

Where ImpAI plugs into TotalA.exe, which engine facts the plan rests on, and what is still undocumented. Every address is cited from the engine map with how it was established. The seam, the seat choice and the handicap exemption are built (A1a; the module note [ImpAI in the engine](../modules/impai.html)); ImpAI plays its seats' economy and the seat button chooses it (A1b).

## The seam: the call at `0x465031`

From [Orders and the AI](../engine/orders.html), *The AI object, its lifecycle and its ten planners* (DISASSEMBLED):

- The players phase `0x464F80` calls the AI tick `0x408C40` from one instruction, `0x465031` (`E8 0A 3C FA FF`). The gate before it (`0x464FCE..0x46502F`) is: the record is seated (`[rec]` ≠ 0), its controller `+0x73` is 1, 2 or 3, its `+0x146` is not 10, and its AI object `+0x74` is set. Nothing else calls the tick or holds a pointer to it. At the call `ECX` is the AI object (`0x46502A`), `EDI` the record and `ESI` the record's index, the loop counter `BL` zero-extended (`0x464FAB..0x464FB5`).
- `0x408C40` is thiscall with no stack argument and returns with a plain `ret` (`0x408CA2`, `0x408CAF`). Its own test, `[rec]` ≠ 0 and `+0x73` = 2 (`0x408C45..0x408C50`), decides whether it plans: then it runs the group assignment `0x408830` every 30 calls, the ten planner slots when due, then the weapon target scan `0x4089A0(obj, 1)`; any other record only `0x4089A0(obj, 0)`. Human seats have an AI object too, for the scan.
- The AI object lives from the player init `0x464700` to the teardown `0x464A00`. A replay puppet (active type 3) has none, so nothing reached from `0x465031` runs on a puppet.
- The players phase also runs once on the **loader thread**, at `0x497C10`, at the end of a level load and before the loader's last store `0x497C62` sets bit 1 of `main+0x38D75`. The game thread runs no step until it has read that bit set (`0x498342`), and `0x498480` clears the whole flag word after the level's first in-game frame, so the bit marks an ordering, not "loaded" during play.

**ImpAI retargets the call at `0x465031`**, not the entry of `0x408C40`: that entry's five bytes are the recorder's AI activity observer, and `0x4089A0`'s entry is the extra-weapons module's acquisition detour ([the hook map](../modules/hook-map.html)). A call-site redirect composes with both.

### As built (A1a)

The seam, the seat table, the seat choice `seatai`, the handicap exemption, the `AI` command, what a seat knows, the threat table, the overlay, the cost and their tests are the module note [ImpAI in the engine](../modules/impai.html). In short: the call at `0x465031` reaches a wrapper that plays an ImpAI seat -- its observation (its line of sight, its radar pass, its patch scan) and its brain's decision, then the weapon scan `0x4089A0(obj, 0)` -- and hands every other seat to the stock tick `0x408C40`; the level init, on the loader thread inside `0x4919C8`, also builds the content ImpAI reads -- each movement class's reach labels from the pathing maps `0x440940` built at `0x4918E3`, and the per-type threat table from the defs, weapons and categories the unit-data load `0x42D2E0` left at `0x4918CA`; the overlay is filled on the game thread from ImpAI's declared state, so a puppet draws what the live game drew; fifteen type tests of the Difficulty handicap and the on-hit purge skip a seat ImpAI's table marks; the console's `AI` never leaves a seat to ImpAI; and a saved game does not carry the choice, so a reload seats ImpAI from the `seatai` in force when it loads.

### When ImpAI plays (A1)

When ImpAI plays a seat, the wrapper runs the brain and then the weapon scan `0x4089A0(obj, 0)` by address, as a human seat's tick runs it, so the extra-weapons port still scans, in place of `0x408C40`; every other seat still calls `0x408C40`. Argument 0 leaves CommandFire weapons to orders (`0x408A88..0x408A96`), and ImpAI's military orders the commander's D-gun itself ([decisions](decisions.html), the reversal of 2026-11-12).

**In a network game** the seat's tick runs only on the peer that added it, where its record is type 2; on every other peer the record is type 3 with no AI object, so `0x465031` never reaches the wrapper there. The choice of ImpAI or OTA AI is therefore made and run on the adding peer, which can be a joiner. Orders never cross the wire; the owner streams their results from the AI's own DirectPlay ID. A seat's row index can differ between peers, so a reference to an ImpAI seat across peers goes by its DirectPlay ID or its unit block, never by row ([network](../engine/network.html), *A computer seat in a network game*).

**Why it is safe by construction:**

- **A bound**: the record is one of the ten, by arithmetic, never by a probe.
- **An ordering**: the players phase runs after the unit tick `0x48AD30` has returned, so no order list is being walked when a root links or wipes nodes. The one call on the loader thread is refused by thread identity, and the seat table it would read is ordered before every game-thread call by the loader's last store `0x497C62`, which the game thread waits for before its first step ([sim](../engine/sim.html)).
- **A lifetime**: the wrapper is entered only from `0x465031`, guarded by `+0x74` ≠ 0, and the AI object exists from `0x464700` to `0x464A00`. ImpAI's per-seat state is keyed by the record and lives in the same window: made at the level init (after `0x464990`) and cleared at the level end, in the teardown's entry.

## The order roots

From [Orders and the AI](../engine/orders.html), *Calling the order roots from the AI's tick* and *What each resolver mode answers* (DISASSEMBLED). ImpAI calls only these:

- **`0x438760`** resolves the action bytes ImpAI needs by name, once: the table is fixed for the process.
- **`0x43F0E0` then `0x43ADC0`** for move (mode 2), attack a unit or the ground (3), guard (7), repair and assist (8), patrol (9), stop (10) and build at a site (14). ImpAI tests the resolver's byte and issues nothing on 0: a 0 passed on is `Ready`, which without Shift wipes the unit's queue first. Mode 8 has no alliance test, so ImpAI checks the alliance itself.
- **A feature's reclaim by name** (`RECLAIM` / `VTOL_RECLAIM` through `0x438760`, then `0x43ADC0` without Shift at the feature's centre), never mode 12: mode 12's map test reads the explored map of the *viewed* player, not the unit's owner's. The handler `0x404AD0` tests only that a feature is at the position and its `reclaimable` bit, so ImpAI checks the rest itself: `canreclamate` on the builder's def, the `reclaimable` bit, that the feature is not playing its sequence (a GAF feature whose anchor holds a wreck record, which the completion `0x4237D0` refuses unpaid), and that the game draws the feature for the seat's player -- every feature on explored ground, a `nodrawundergray` def only to its owner or in sight ([map](../engine/map.html), *What the feature draw shows a player*) --, never taking a feature on ground it has not explored (`impai_build.c`, THE FEATURE MEMORY; `tasim_sweep.c`, `tasim_feature_drawn`). Its payouts it reads from its own reclaimers' ledgers (`tagpu_impai.c`, THE PAYOUT).
- **`0x43B0B0(BUILDINGBUILD, unit, type, count)`** for factory queues, the type bounded below the def count.
- **Not** `0x480460` (the group order, which never tests the resolver), `0x43AFC0` (its Shift toggles a queued node) or `0x48CF30` (it walks the human's selection).

**What each root call rests on:** no root validates its unit, so ImpAI passes `array + ID·0x118` with the ID bounded by `u16 main+0x14351` and the slot inside the unit array, and the unit live. A `0x43ADC0` without Shift can run handlers through the node destructor, so no order-node pointer survives a call (ImpAI keeps none) and every unit pointer is re-derived from its ID after each call. A target, once linked, is held by the engine's own reference chain, so a target that dies later is the engine's normal case.

**A command given later than it was decided** (a persona's reaction, [the brain](brain.html), *The plan and the
personas*) names the incarnations it was decided for (`WQ_CMD.inc`, `birth`, `tbirth`): the adapter
(`tagpu_worldq.c`, `order_check`) refuses it when the slot's birth stamp (`tagpu_unit_birth`) is not the one named,
or the slot has none, so a unit that took the slot since is never ordered and never targeted. `order_test` answers
every check `order` makes, in the same order, and calls no root: the enqueue's answer is the order's.

## The map's layout

From [the map](../engine/map.html), *What Mapped gives a player*, and [Orders and the AI](../engine/orders.html), `0x47D2E0` (DISASSEMBLED):

- **Mapped is the level's, not a record's.** The level load's `0x4816A0(1)` (`0x497AE5`) sets every bit of every word of the explored map `main+0x14273` when `LosType` bit 0 is clear, and no routine gives one player the map.
- **Who reads a seat's own bit**: the engine's pathfinder for the seat's units (`0x40D7B0`: an unexplored block is passable), the stamp's own test, and the copier `0x485420`, through which allied sharing hands the bit to every receiver the seat gains and the share message hands it to a peer; a human who views the seat is shown the map by it (`main+0x2A43`). The site test, the reclaim resolver and `0x408090` read the viewed player's bit whatever record they are given.
- **So ImpAI writes no explored map.** A seat's bit set in every word would give the map to each human the seat shares with and to whoever views it. Its views answer the layout from the map itself instead (`tagpu_worldq_open_layout`: `explored`, and so `metal`, `coverage`, the patch scan and the feature rule), and its site test's thunk skips the refusal of an unmapped centre at `0x47D3FA`. Both read engine memory and write none, and in a mapped level both answer what the bit answers, since it is set everywhere.
- **What stays the seat's sight**: every unit it knows (`unit_los`, the radar pass), and the flag a unit on a footprint refuses on in the site test.
- **What the engine still does by the bit**: the pathfinder. An unmapped seat's units take unexplored blocks as free ground until they see them; no ImpAI rule reads that, and closing it would take a write at `0x40D817`'s test.

## Difficulty and the seat

From [the GUI](../engine/ui.html), *The SKIRMISH screen's handler*, and [sim](../engine/sim.html), *Difficulty and the AI handicap* (DISASSEMBLED):

- **Difficulty is one global**, `main+0x37EEE` (0 Easy, 1 Medium, 2 Hard), set by the `SKIRMISH.GUI` gadget `Difficulty` together with the settings' `+0x228` and kept in the registry as `SkirmishDifficulty`. In a network game the battle room's START sets it to 2 on the host only (`0x448A0B`); every other peer keeps its own value.
- **It scales type-2 players' resources at seventeen sites**: the economy pass's eight incomes, the build-cancel refund, the unbuild refund, the reclaim payout's energy and metal, both transfers, the Deathmatch respawn's energy and metal grants, and the credit for a reclaimed unit. Easy × 0.5, Medium × 0.7, and **Hard applies no scaling at any site**. Each site tests the record for type 2 with one instruction pair while the record is in a register.
- **A seat's controller** is the record's byte `+0x73` (1 human, 2 AI, 3 remote), written at the seat's activation by `0x464290` from the skirmish entry and in play through `0x463C60`.
- **The AI profile depends on difficulty**: the profile command `plan` gates the `weight` and `limit` lines by `easy`, `medium`, `hard` or `any`, and every shipped profile has a `plan easy`, a `plan medium` and a `plan hard` section, so OTA AI's build weights and limits change with the Difficulty even on Hard. The profile is `ai\<aiprofile>.txt` (the map's key) or `ai\default.txt`, loaded by `0x4648E0`.

**The handicap exemption** is built: [ImpAI in the engine](../modules/impai.html), *The handicap exemption*.

## Not documented yet

- **Readers of the remaining fields** in the [key inventory](../engine/keys.html): the main UnitDef and weapon fields have their readers; `bankscale`, the weapon's velocities, `sprayangle` and `areaofeffect` do not yet, nor any def flag after it is copied into the unit. ImpAI's threat table reads `maxvelocity` `+0x192`, `transportcapacity` `+0x22B` and `burst` `+0xEA` as the loader stores them; which engine code reads them in play is not traced.
- **Network games (A4)**: which caller of the join `0x450A10` activates a battle-room AI on its own peer; the meaning of the add-AI path's three tests; what happens to a departed peer's AI seats on the peers that remain ([network](../engine/network.html), *A computer seat in a network game*).
- Whether anything outside the load state can switch the state slot away mid-load, leaving the load flags set.

## Constraints from the repository

- **Simulation state, declared.** ImpAI changes what units do, so its files are marked `sim` and its state is `SIM_STATE`. It keeps unit IDs, never addresses.
- **Deterministic by construction.** No wall clock. Its own seeded RNG lives in its state, and it never draws the simulation RNG `0x4B6C30`: the stock planners draw it, so an ImpAI seat's stream already differs from an OTA seat's, and ImpAI must not add draws of its own. Its work per tick is bounded by counts.
- **Bounded reads.** Every engine value the adapter reads is bounded as data first: a unit index against the engine's count, a def ID against `UNITINFOCount`.
- **Game thread only.** The threat overlay reaches the render thread through the frame packet.
- **Activity attribution.** The recorder's AI activity harness brackets `0x408C40`; an ImpAI seat's orders are attributed to it only when the wrapper brackets itself the same way.
