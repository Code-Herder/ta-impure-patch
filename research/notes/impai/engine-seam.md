# Entering the game

Where ImpAI plugs into TotalA.exe, which engine facts the plan rests on, and what is still undocumented. Every address is cited from the engine map with how it was established. The seam is built and observes; the exemption and the button are designs.

## The seam: the call at `0x465031`

From [Orders and the AI](../engine/orders.html), *The AI object, its lifecycle and its ten planners* (DISASSEMBLED):

- The players phase `0x464F80` calls the AI tick `0x408C40` from one instruction, `0x465031` (`E8 0A 3C FA FF`). The gate before it (`0x464FCE..0x46502F`) is: the record is seated (`[rec]` ≠ 0), its controller `+0x73` is 1, 2 or 3, its `+0x146` is not 10, and its AI object `+0x74` is set. Nothing else calls the tick or holds a pointer to it. At the call `ECX` is the AI object (`0x46502A`), `EDI` the record and `ESI` the record's index, the loop counter `BL` zero-extended (`0x464FAB..0x464FB5`).
- `0x408C40` is thiscall with no stack argument and returns with a plain `ret` (`0x408CA2`, `0x408CAF`). Its own test, `[rec]` ≠ 0 and `+0x73` = 2 (`0x408C45..0x408C50`), decides whether it plans: then it runs the group assignment `0x408830` every 30 calls, the ten planner slots when due, then the weapon target scan `0x4089A0(obj, 1)`; any other record only `0x4089A0(obj, 0)`. Human seats have an AI object too, for the scan.
- The AI object lives from the player init `0x464700` to the teardown `0x464A00`. A replay puppet (active type 3) has none, so nothing reached from `0x465031` runs on a puppet.
- The players phase also runs once on the **loader thread**, at `0x497C10`, at the end of a level load and before the loader's last store `0x497C62` sets bit 1 of `main+0x38D75`. The game thread runs no step until it has read that bit set (`0x498342`), and `0x498480` clears the whole flag word after the level's first in-game frame, so the bit marks an ordering, not "loaded" during play.

**ImpAI retargets the call at `0x465031`**, not the entry of `0x408C40`: that entry's five bytes are the recorder's AI activity observer, and `0x4089A0`'s entry is the extra-weapons module's acquisition detour ([the hook map](../modules/hook-map.html)). A call-site redirect composes with both.

### As built: the stock AI plays, ImpAI observes (`tagpu_impai.c`)

- **The lever.** `tagpu_impai.on`, read once at attach: `ai` (every record whose controller is 2 at the level's start) or a list of record indexes such as `1,2`; `tacli arm <inst> impai.on=1,2` writes it. Any other text logs `impai: tagpu_impai.on: <text> not understood` and installs nothing. With no lever the site stays stock.
- **The install.** `tagpu_detour_redirect` checks `E8` and the rel32 reaching `0x408C40`, then rewrites the rel32 at `0x465032`. A mismatch logs `impai: seam 0x465031 not stock (<five bytes>)` and installs nothing; success logs `impai: seam 0x465031 in, seats <text>`.
- **The wrapper** is `__fastcall(obj, edx)`: ECX arrives as its first argument and it pops nothing, so the site's stack is unchanged, and `EBX`, `ESI`, `EDI` and `EBP`, which the players phase uses after the call (`0x465036` `push esi`, `0x46506A` `cmp bl,…`), are callee-saved. It counts the turn of an ImpAI seat and then **always calls `0x408C40`**: the stock AI plays every seat, and the recorder's observer still sees each call.
- **Which turns count**, all of these:
  - **the game thread** (`GetCurrentThreadId()` against `g_ddraw.gui_thread_id`), which excludes the loader thread's one players phase by thread identity;
  - **the bound**: `rec = *(obj+0)` lies at `main+0x1B63 + i·0x14B`, `i` below 10, by range and exact division, before anything is read through it;
  - the record is seated with controller 2, the test `0x408C40` makes before it plans;
  - the seat table marks record `i`, and `i` is not a puppet seat.
- **The seat table** `s_seat[10]` is `SIM_STATE`, keyed by player record. The level init writes it, called from `tagpu_replay_level_players` right after the engine's players init `0x464990`, on the loader thread inside the `PLAYERS` call `0x4919C8`. On a live level it clears the table and marks each lever seat whose record is seated with controller 2, logging `impai: seats 1,2 (lever) of 4 records`; a record with controller 3 (a network game) marks none and logs why. **On a puppet level it writes nothing**: the recording restores the table, and the recorder is its only writer there.
- **Why a game-thread turn sees the whole table.** The level init runs before the loader's last store `0x497C62`, and the game thread runs no step until it has read that store's bit (`0x498342`), the ordering `tagpu_replay.c`'s *THE VIEWER'S BUFFERS* rests on. No per-call test of the load flags is made: the bit is clear for the rest of play.
- **The level end.** The teardown's entry (`tagpu_reclaim.c`, `reclaim_teardown_pre`, beside the recorder's level end) logs `impai: calls seat=<i> n=<turns> gametime=<GameTime>` for each marked seat. The counts are `SIM_PRIVATE`. With `tagpu_reclaim.off` the teardown wrap is not installed and no line is written.
- **The replay feature** is `SIM_FEATURE(impai_observe, SIM_PUPPET_OWNER, "impai-observe impai-observe-perm")`: the two test groups play four records on Core Prime Industrial Area (the human seat idle, stock AIs on 1–3, ImpAI observing 1 and 2) under true and permanent line of sight, the lever written by `tools/impai_fixture.py`.

### When ImpAI plays (A1)

When ImpAI plays a seat, the wrapper runs the brain and then the stock weapon scan `0x4089A0(obj, 1)` by address, so the extra-weapons port still scans, in place of `0x408C40`; every other seat still calls `0x408C40`.

**In a network game** the seat's tick runs only on the peer that added it, where its record is type 2; on every other peer the record is type 3 with no AI object, so `0x465031` never reaches the wrapper there. The choice of ImpAI or OTA AI is therefore made and run on the adding peer, which can be a joiner. Orders never cross the wire; the owner streams their results from the AI's own DirectPlay ID. A seat's row index can differ between peers, so a reference to an ImpAI seat across peers goes by its DirectPlay ID or its unit block, never by row ([network](../engine/network.html), *A computer seat in a network game*).

**Why it is safe by construction:**

- **A bound**: the record is one of the ten, by arithmetic, never by a probe.
- **An ordering**: the players phase runs after the unit tick `0x48AD30` has returned, so no order list is being walked when a root links or wipes nodes. The one call on the loader thread is refused by thread identity, and the seat table it would read is ordered before every game-thread call by the loader's last store `0x497C62`, which the game thread waits for before its first step ([sim](../engine/sim.html)).
- **A lifetime**: the wrapper is entered only from `0x465031`, guarded by `+0x74` ≠ 0, and the AI object exists from `0x464700` to `0x464A00`. ImpAI's per-seat state is keyed by the record and lives in the same window: made at the level init (after `0x464990`), dropped at the teardown.

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

- **Difficulty is one global**, `main+0x37EEE` (0 Easy, 1 Medium, 2 Hard), set by the `SKIRMISH.GUI` gadget `Difficulty` together with the settings' `+0x228` and kept in the registry as `SkirmishDifficulty`. In a network game the battle room's START sets it to 2 on the host only (`0x448A0B`); every other peer keeps its own value.
- **It scales type-2 players' resources at seventeen sites**: the economy pass's eight incomes, the build-cancel refund, the unbuild refund, the reclaim payout's energy and metal, both transfers, the Deathmatch respawn's energy and metal grants, and the credit for a reclaimed unit. Easy × 0.5, Medium × 0.7, and **Hard applies no scaling at any site**. Each site tests the record for type 2 with one instruction pair while the record is in a register.
- **A seat's controller** is the record's byte `+0x73` (1 human, 2 AI, 3 remote), written at the seat's activation by `0x464290` from the skirmish entry and in play through `0x463C60`.
- **The AI profile depends on difficulty**: the profile command `plan` gates the `weight` and `limit` lines by `easy`, `medium`, `hard` or `any`, and every shipped profile has a `plan easy`, a `plan medium` and a `plan hard` section, so OTA AI's build weights and limits change with the Difficulty even on Hard. The profile is `ai\<aiprofile>.txt` (the map's key) or `ai\default.txt`, loaded by `0x4648E0`.

**The handicap exemption (A1):** each of the seventeen type tests (`cmp byte [rec+0x73],2` and its `jne`, 5 to 10 bytes) becomes a 5-byte `jmp` to a stub of its own, byte-matched at attach and all-or-none. The stub finds the slot from `rec` by the seam's bound and, for an ImpAI seat, jumps to the unscaled branch; otherwise it runs the original compare and branch. The stub calls no C, because one or two values are live on the x87 stack at every site. The table it reads is written only at the level init, before the first tick, and from the recording on a replay. Writing `main+0x37EEE` around calls instead is rejected: the sites are reached from the economy pass, transfers (including the network's), reclaim, building and unit death, and no one bracket covers them.

## Not documented yet

- **Readers of the remaining fields** in the [key inventory](../engine/keys.html): the main UnitDef and weapon fields have their readers; `maxvelocity`, `bankscale`, `transportcapacity`, the weapon's velocities, `burst`, `sprayangle` and `areaofeffect` do not yet, nor any def flag after it is copied into the unit.
- **Network games (A4)**: which caller of the join `0x450A10` activates a battle-room AI on its own peer; the meaning of the add-AI path's three tests; what happens to a departed peer's AI seats on the peers that remain ([network](../engine/network.html), *A computer seat in a network game*).
- Whether anything outside the load state can switch the state slot away mid-load, leaving the load flags set.

## Constraints from the repository

- **Simulation state, declared.** ImpAI changes what units do, so its files are marked `sim` and its state is `SIM_STATE`. It keeps unit IDs, never addresses.
- **Deterministic by construction.** No wall clock. Its own seeded RNG lives in its state, and it never draws the simulation RNG `0x4B6C30`: the stock planners draw it, so an ImpAI seat's stream already differs from an OTA seat's, and ImpAI must not add draws of its own. Its work per tick is bounded by counts.
- **Bounded reads.** Every engine value the adapter reads is bounded as data first: a unit index against the engine's count, a def ID against `UNITINFOCount`.
- **Game thread only.** The threat overlay reaches the render thread through the frame packet.
- **Activity attribution.** The recorder's AI activity harness brackets `0x408C40`; an ImpAI seat's orders are attributed to it only when the wrapper brackets itself the same way.
