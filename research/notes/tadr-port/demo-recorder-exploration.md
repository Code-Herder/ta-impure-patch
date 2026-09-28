# Demo recorder — technical exploration

## Status and evidence boundary

**First source and disassembly pass, 2026-09-28.** The product contract and feature progress live
in [Demo recorder](demo-recorder.md). This page holds technical findings and the experiments
needed to settle the open architecture decisions. No recorder code has been installed, no
replay has been played by Impure's new recorder, and no seek latency or file-size benchmark
has run. Preparation and checkpoint policy remain undecided.

Sources inspected: TADR's gitignored source checkout at `dcff5dd`, Impure's worktree based on
`485d3b0`, and `pristine/TotalA.exe.pristine` in the main checkout. **SRC** means directly read
source, **DIS** means checked against that binary, and **INFERENCE** names a consequence that
still needs an executable experiment. Existing live measurements are attributed to their notes;
they are not new recorder measurements.

## 1. What the Pascal recorder actually records

**SRC — `src/Recorder/idplay.pas`.** `TDPlay.Send` and `TDPlay.Receive` wrap DirectPlay, serialize
their processing through `cs`, and call `packetHandler`. Recording is part of that larger
handler, which also changes gameplay and UI messages. It is not a standalone raw socket logger.

`createlogfile` writes the map, player roster and status messages, extra metadata, and unit-sync
data. `SmartPak` transforms the unit-update stream: the first `0x2C` in a packet supplies a
`0xFE` sequence anchor, subsequent updates become `0xFD` records without their repeated
sequence field, and an empty 11-byte update can become `0xFF`. It then applies the packet
compressor. The recording record itself has a 16-bit byte length, 16-bit millisecond delta,
sender index, and payload (`idplay.pas:1551–1588`, `3094–3135`).

Consequences for the new format:

- Reuse knowledge of message semantics and redundant fields, not unchecked assumptions about
  old capacities. The single unit-sync record also has a 16-bit length. The existing
  [content-ID note](content-ids.md#the-unit-passs-caches) records the old format's approximately
  2,340-type limit immediately before that section; Impure supports substantially more types.
- Distinguish sender, recipient/audience, timestamp, and message kind explicitly. The ordinary
  legacy record stores only a sender; some recipient information exists inside special messages.
- Preserve binary messages as binary, even when they use a chat message's envelope. Do not
  pass them through text trimming, duplicate-chat suppression, or visibility filtering.
- Investigate independent compressed blocks, an index, and per-block integrity rather than
  copying the legacy header-only CRC behavior. `createlogfile` turns `logsave.docrc` off before
  writing the unit-data record; that CRC is not verification of the complete replay stream.
- Keep compression and disk IO out of the message-processing critical path. The Pascal
  handler calls `logsave.add` while under `cs`; `log2.pas` writes through `TGpHugeFile`. This is
  a source observation, not a measured overhead claim or a model to inherit unchanged.

### Communication and local presentation data

**SRC — `idplay.pas:2493–2521`, `3094–3157`.** TADR already has two useful precedents:

- Its `0xF9` forwarded-chat envelope carries original sender and destination IDs. The forwarding
  code has text-pattern conditions around directed messages; it is not evidence that every
  private whisper is included. Impure's agreed all-in-match-chat behavior needs its own explicit
  audience record and capture coverage.
- It appends camera positions when they change. Those records carry map X/Y, not Impure's full
  camera/zoom/cursor/selection contract.

**INFERENCE.** A recorder at one peer needs contributions from the other peers for their local
presentation state and communications it did not receive. Preserve audiences independently of
the replay viewer's chosen visibility mode. Test opt-out from local file writing separately
from participation in the data needed by another player's recording; the exact setting behavior
needs specification when the capture protocol is designed.

## 2. File marks are not world checkpoints

**SRC — `src/Server/savefile.pas:37–72`, `450–483`, `925–978`.** `TMark` contains a file offset
and a record index. The loader builds marks after about `MEM_SIZE = 50000` bytes, and `GetMove`
loads the indexed range on demand. These are file-access indexes, not serialized worlds.

**SRC — `src/Server/tasv.pas:2052–2087`, `1717–1731`; `savefile.pas:184–250`.** `SetPos` moves
the record cursor, adjusts sequence tracking, and sets `recentpos` for the recorded players.
Playback then calls `unsmartpak` with `incnon2c = false` until that sender's sequence has
advanced by `maxunits`. That decoder emits compacted unit updates but suppresses its default
case, which includes ordinary non-`0x2C` messages.

This is **jump and resynchronize**, not restore a complete saved world. No full-world checkpoint
was found in this path. Its existence is useful prior art for forward jumps, but neither exact
target-state reconstruction nor subsecond seeking follows from it.

### Why Impure cannot adopt that seek path unchanged

**SRC.** Impure encodes gameplay data inside fixed-size `0x05` messages:

| Tag | Impure meaning | Why playback must preserve it |
|---|---|---|
| `0x49` | Extended interceptor detonation | Weapon identity and effect |
| `0x4A` | Create with unit incarnation, initial build fraction, and HP | Identity and correct initial construction state |
| `0x4B` | Damage with victim incarnation | Correct damage application after slot reuse |
| `0x4C` | Death with build fraction and carried-death state | Kill/reclaim accounting and transported explosion behavior |

The implementations are in `tagpu_patches.c` (`wpn_rx_chat`, `hit_tx_create`, `hit_rx_chat`,
`kill_tx_death`, `kill_rx_death`), documented by [content IDs](content-ids.md) and
[simulation fixes B4/B8](sim-fixes.md).

The Pascal decoder's resync suppression includes these messages. Its normal playback loop also
handles `0x05` as chat and suppresses repeated `Trim(tmp)` values (`tasv.pas:1798–1803`).
**INFERENCE:** repeated binary gameplay messages must not inherit that text policy. The source
proves the classification hazard; an actual Impure replay demonstrating its consequences has
not run. This does not establish that every shipped legacy replayer behaves identically.

**Identity is another obligation.** The carried death includes the killer player's DirectPlay
ID. TADR creates new replay-player IDs and assigns unit ranges in `TAServer.CreateSession`.
A new player mapping must preserve or translate Impure's embedded IDs, not only the outer
sender. Ten-player matches and AI-owned player slots need explicit coverage.

**Existing measurement, not a seek benchmark.** B8's note reports remote completed scenario
units waiting 26–37 seconds for stock round-robin correction at a 1,500-unit limit before the
carried-state fix. This is why waiting for eventual updates cannot be accepted as the proof of
correct build state at a seek destination. It does not predict the new recorder's latency.

## 3. Candidate capture points in Impure

| Layer | Evidence | Benefit and unresolved coverage |
|---|---|---|
| DirectPlay interface boundary | **SRC:** `tagpu_takeover.c:1180–1265` already forwards creation/lobby exports to system DirectPlay. **DIS:** `0x4C97B0` calls vtable slot `+0x68`; `0x4C9840` calls `+0x64`. | Close to Pascal's interception boundary. Must handle interface creation/query/lifetime correctly and account for transport framing, retransmission, and system messages. Existing forwarders do not wrap the returned interfaces. |
| Engine message send | **DIS:** `0x451DF0` branches through `0x461990` for the transport or `0x4C97B0` for raw DirectPlay; a separate branch calls targeted send `0x451BC0`. **SRC:** B8 already owns the entry for held create messages. | Logical messages before transport framing can avoid recording transport redundancy. Do not install a competing hook or capture before B8 finalizes held creation state. Coverage of targeted sends, private chat, startup data, and generated local messages must be enumerated first. |
| Engine delivered-message boundary | **SRC:** B3's `wire_rx_note` captures the delivered buffer and length in TLS at `0x453595` / `0x45361F`, before dispatch, and repairs the pump's pointer if the buffer moved. | A bounded message boundary already exists. Copy while its lifetime is established; do not retain the engine buffer. Sender, recipient, time, order, and the loader/main-thread paths still need capture design. It is only the incoming half. |

The existing renderer exchange (`tagpu_packet.c`) is **latest-wins**: its design permits skipped
publications while the consumer holds an unread frame. That is appropriate for drawing and
**not an ordered event recorder**. A recorder needs its own ownership and bounded buffering
contract. Queue exhaustion must not silently turn a recording labeled complete into one with
missing events. Disk writes cannot become a wait in the simulation/render handshake.

These are candidate boundaries, not installed hooks. Detailed verified addresses are retained
in the [engine map](../exe-reverse-engineering.md#demo-recorder-exploration-capture-and-save-boundaries-disassembled-2026-09-28).

## 4. Two apparent checkpoint shortcuts are unproven

### The existing renderer frame packet

**SRC — `tagpu_packet.h`, `tagpu_packet_pub.c`.** It provides copied unit/build/pose data,
effects, fog grids, and renderer metadata under an existing ownership protocol. However,
`TAGPU_PK_UNIT.piece_n` may be zero outside the widest zoom rectangle, and the fog grids are
view-related. This is not a complete all-map, all-player, all-time snapshot, nor a simulation
save. It is useful infrastructure to study for a watch-only playback representation, not a
ready-to-write file format.

**INFERENCE.** A separate watch-only state stream could avoid restoring a playable simulation,
but would still need sufficient all-map poses/effects, player visibility, unit inspection data,
and statistics. It could also become larger than a packet stream. Benchmark completeness and
size before selecting it; watch-only scope does not make those data requirements disappear.

### Pascal's `SaveGame` plugin

**SRC — `src/Recorder/plugins/SaveGame.pas`.** This extends the engine's save/load path with
`TADR Extensions` entries (`UnitsSharedData`, `UnitSearch`, `SpawnedMinions`) and optional script
slot changes. It is not called as a checkpoint writer by the replay path inspected here.

**DIS.** Its main save/load splice instructions at `0x432A38` and `0x43267D` match the retail
binary. That confirms the referenced instruction bytes, not portable replay restoration.
The [existing engine-map save investigation](../exe-reverse-engineering.md) establishes that
the retail menu disables saving/loading during a multiplayer game. Any use of the save system
as a replay checkpoint needs a separate completeness and lifetime proof, including Impure's
own incarnation and other side tables. Do not equate a stock save with a replay checkpoint.

## 5. Architecture experiments to run before choosing

| Candidate | What must be demonstrated | Main tradeoff to measure |
|---|---|---|
| Packet replay plus corrected resynchronization | Continuous playback and jumps reproduce all required state, not just converged unit positions; all Impure message classes and identities survive | Small capture stream versus correctness and settling latency |
| Packet/event stream with deliberate checkpoints | Restore sufficient engine and Impure state, then advance only a bounded interval to the target | Recording cost and checkpoint storage versus seek latency |
| Watch-only state playback | Independent renderer-facing state supports the whole map, effects, player fog, unit inspection, and overlays without a running simulation | Potentially direct seeks versus capture coverage, implementation scope, and disk cost |

**No candidate has been selected.** A process-memory dump is not automatically a portable
checkpoint: pointers, system resources, thread state, and module versions would need a proved
restore contract. A file index alone cannot substitute for reconstructing the target world.

The first executable experiment should establish a **lossless continuous capture/playback
baseline with Impure's own wire messages**. Then compare jumps against reaching the same target
through continuous playback. Building a seek cache before that baseline would leave no reliable
correctness oracle for the cache.

### Workloads and measurements

Use deterministic scripted inputs where available, while comparing recorded observed state
rather than assuming TA is a lockstep simulation. Cover a small match, a busy match at supported
unit/player limits, AI in a multiplayer lobby, a long match, and supported mod content. In each
relevant workload include rapid finished-unit spawning, real construction, slot reuse,
transport/death effects, visibility changes, private/team chat, and camera/selection events.

| Measurement | Report |
|---|---|
| Live recording cost | Sim tick/frame-time distributions with recording off/on; capture and compression time; IO stalls and queue high-water mark |
| Network cost | Actual additional bytes per player-second and per match, including framing/fan-out; burst sizes and behavior under loss |
| Storage | Replay MB per match-hour, broken down by gameplay, presentation, chat, statistics, index, and checkpoints; separate cache size |
| Open/preparation | Cold/warm opening times, any first-open preparation time, peak working memory, and cancellability |
| Seeking | Request-to-correct-destination latency, including deserialize/rebuild work; distributions and worst observed cases for short/long forward and backward jumps |
| Fidelity | State/visibility/event comparison against continuous playback at the target, plus rendered inspection; no intermediate catch-up frames presented |
| Recovery | Truncation at block boundaries and inside blocks; crashes/disconnects; truthful incomplete status and last playable time |

Record workload, match duration, map/mod, player/unit counts, and the reference setup beside
every result. Report **not measured** where there is no result. Synthetic compression numbers
or nominal sample rates cannot decide the user's preparation/storage tradeoff.

## Next gate

Map complete send/receive coverage and playback identity requirements, then build the smallest
isolated continuous-playback experiment. Return real time/space/correctness results before
choosing the recording format, checkpoint method, or first-open preparation policy. The product
page remains at **feasibility in progress**, with all implementation milestones unverified.
