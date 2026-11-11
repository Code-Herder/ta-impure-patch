# Entering the game

Where ImpAI plugs into TotalA.exe, which engine facts the plan rests on, and what L0 still has to find. Every address is cited from the engine map; nothing here is newly disassembled.

## The AI's call site

Cited from [Orders and the AI](../engine/orders.html), *Direct AI order activity observer* (DISASSEMBLED):

- The players phase calls `0x408C40` at `0x465031` with a seat's AI object. It runs only for an active seat with controller 2.
- `0x408C40` runs the group assignment `0x408830` every 30 calls, dispatches the due planner objects from ten slots at `AI+0x11` through their vtable, then runs the weapon target scan `0x4089A0`. Human seats run that scan too.
- Orders are built by the common constructor `0x43A0C0`, reached through the helper `0x43ADC0`, and then inserted into a unit's list.

**The proposed seam:** for an ImpAI seat, skip the stock planners in `0x408C40`, keep the weapon scan, and run the brain there on the game thread. Orders go through the engine's own order roots, never by writing order lists directly. Which root takes move, attack, build and repair cleanly is L0's first question.

## Difficulty and the seat

From `gui-gadgets.md`, [Recorder boundaries](../engine/recorder.html), [Units](../engine/units.html) and [Network](../engine/network.html):

- **Difficulty is global**: the `SKIRMISH.GUI` gadget `Difficulty`, held at `main+0x37EEE` (written at `0x4311D1`, DISASSEMBLED) and saved as the registry value `SkirmishDifficulty` (written at `0x4315A0`).
- **It scales AI players only**: income in the economy pass `0x401441..0x40175E`, reclaim at `0x4238EF`, metal transfers at `0x464D3C` and build credit. The rate is 50 % on difficulty 0 and 70 % on 1. The full rate on Hard is [INFERRED]. ImpAI seats are exempted at these sites ([ImpAI for the player](player.html)), and L0 maps each site.
- **A seat's only AI setting** is its controller byte at record `+0x73` (1 human, 2 AI, 3 remote), set by `0x463C60`, which copies it to PlayerInfo `+0x94`.
- **The AI profile scripts** (`ai\default.txt` and the per-type texts, loaded through `0x4648E0`) do not depend on difficulty.

## Not documented yet (L0)

- Which `SKIRMISH.GUI` gadget a seat click cycles, and through which values.
- Where an ImpAI / OTA AI choice per seat can be stored beside the controller byte.
- The order roots for move, attack, build and repair, and whether each is safe to call from the AI's site.
- What the ten stock planner slots each do.

## Constraints from the repository

- **Simulation state, declared.** ImpAI changes what units do, so its files are marked `sim` and its state is `SIM_STATE`. It keeps unit IDs, never addresses.
- **Deterministic by construction.** No wall clock. Its own seeded RNG lives in its state. Its work per tick is bounded by counts.
- **Bounded reads.** Every engine value the adapter reads is bounded as data first: a unit index against the engine's count, a def ID against `UNITINFOCount`.
- **Game thread only.** The threat overlay reaches the render thread through the frame packet.
